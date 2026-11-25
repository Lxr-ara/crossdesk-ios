/*
 * File transfer pacing regression test.
 *
 * Drives the header-only transfer state machine with a fake transport and an
 * explicit clock, so the ByteTokenBucket rate limit and the pause switch are
 * observed without sleeping: Pump(now) takes the caller's time point.
 */

#include <file_transfer_core.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::file_transfer::Options;
using crossdesk::file_transfer::Session;
using crossdesk::file_transfer::Status;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
}

using Clock = Session::Clock;

// Records outgoing stream traffic; data chunks are the ones on kFileStream.
struct Transport {
  std::vector<std::string> chunks;
  int feedback = 0;

  int Send(const char* stream, const char* data, std::size_t size) {
    if (std::string(stream) == crossdesk::kFileStream) chunks.emplace_back(data, size);
    else ++feedback;
    return 0;
  }
  std::size_t count() const { return chunks.size(); }
};

const Session::Progress kIgnoredProgress =
    [](const std::string&, double, bool, const std::string&) {};

Session::Send Sink(Transport& tx) {
  return [&tx](const char* stream, const char* data, std::size_t size) {
    return tx.Send(stream, data, size);
  };
}

fs::path TempFile(std::uintmax_t size) {
  std::error_code ec;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path(ec) /
                    ("crossdesk-transfer-rate-test-" + std::to_string(stamp));
  fs::create_directories(root, ec);
  const auto path = root / ("payload-" + std::to_string(size) + ".bin");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const std::vector<char> block(64 * 1024, 'Z');
  for (std::uintmax_t written = 0; written < size;) {
    const auto step = (std::min)(static_cast<std::uintmax_t>(block.size()), size - written);
    output.write(block.data(), static_cast<std::streamsize>(step));
    written += step;
  }
  output.close();
  return path;
}

Clock::time_point At(std::int64_t milliseconds) {
  return Clock::time_point{} + std::chrono::milliseconds(milliseconds);
}

// No rate limit: every pump may queue one chunk, exactly as before the patch.
void TestUnlimited(const fs::path& path) {
  Transport tx;
  Session session(Sink(tx), kIgnoredProgress);
  Expect(session.send_rate() == 0, "default send rate is unlimited");
  Expect(!session.send_paused(), "sending is not paused by default");
  Expect(session.Start(path, "unlimited.bin", At(0)), "unlimited transfer starts");
  for (int i = 0; i < 3; ++i)
    Expect(session.Pump(At(i * 10)), "unlimited pump queues a chunk");
  Expect(tx.count() == 3, "an unlimited session emits one chunk per pump");
  Expect(session.Snapshot().status == Status::Sending, "unlimited transfer is still sending");

  crossdesk::FileChunkView first;
  const auto& packet = tx.chunks.front();
  Expect(crossdesk::DecodeFileChunk(packet.data(), packet.size(), &first),
         "a queued chunk decodes on the wire format");
  Expect(first.header.magic == crossdesk::kFileChunkMagic &&
             (first.header.flags & 1) != 0 && first.header.offset == 0,
         "the first chunk keeps its wire header");
  Expect(first.payload_size == crossdesk::kFileChunkSize,
         "a chunk carries kFileChunkSize bytes");
}

// 128 KiB/s with 64 KiB chunks: one chunk every half second, two per second.
void TestLimited(const fs::path& path) {
  Transport tx;
  Session session(Sink(tx), kIgnoredProgress);
  session.SetSendRate(128 * 1024);
  Expect(session.send_rate() == 128 * 1024, "the send rate is stored");
  Expect(session.Start(path, "limited.bin", At(0)), "limited transfer starts");

  Expect(!session.Pump(At(0)), "no credit at t=0");
  Expect(tx.count() == 0, "nothing is queued before the bucket has credit");
  Expect(!session.Pump(At(300)), "half a chunk of credit is not enough");
  Expect(tx.count() == 0, "no chunk at t=0.3 s");
  Expect(session.Pump(At(600)), "one chunk fits at t=0.6 s");
  Expect(tx.count() == 1, "exactly one chunk at t=0.6 s");
  Expect(!session.Pump(At(600)), "a drained bucket refuses a second chunk at the same instant");
  Expect(tx.count() == 1, "no double chunk within one pump instant");
  Expect(session.Pump(At(1000)), "the second chunk arrives at t=1.0 s");
  Expect(tx.count() == 2, "128 KiB/s yields exactly two 64 KiB chunks in the first second");

  // The bucket holds one second of credit and refills from elapsed time only,
  // so the third chunk needs another half second of credit, not a free burst.
  Expect(!session.Pump(At(1200)), "t=1.2 s has only 0.2 s of credit after the second chunk");
  Expect(tx.count() == 2, "no early burst after a chunk");
  Expect(session.Pump(At(1500)), "the third chunk arrives at t=1.5 s");
  Expect(tx.count() == 3, "credit keeps accruing at the configured rate");

  bool contiguous = true;
  std::uint64_t expected = 0;
  for (const auto& packet : tx.chunks) {
    crossdesk::FileChunkView view;
    if (!crossdesk::DecodeFileChunk(packet.data(), packet.size(), &view)) {
      contiguous = false;
      continue;
    }
    if (view.header.offset != expected) contiguous = false;
    expected += view.payload_size;
  }
  Expect(contiguous, "paced chunks keep consecutive offsets");
  Expect(session.Snapshot().status == Status::Sending, "a paced transfer is still sending");
}

// A rate below one chunk per second must not deadlock: capacity is clamped to
// one chunk, so credit accrues to a full chunk and one chunk is admitted.
void TestLowRate(const fs::path& path) {
  Transport tx;
  Session session(Sink(tx), kIgnoredProgress);
  session.SetSendRate(8 * 1024);
  Expect(session.Start(path, "slow.bin", At(0)), "slow transfer starts");
  Expect(!session.Pump(At(7000)), "8 KiB/s does not reach one chunk in 7 s");
  Expect(tx.count() == 0, "nothing is queued below one chunk of credit");
  Expect(session.Pump(At(8000)), "a full chunk of credit admits one chunk");
  Expect(tx.count() == 1, "a rate below one chunk per second still sends");
  Expect(!session.Pump(At(8000)), "the clamped bucket is drained after that chunk");
}

// Pausing stops new chunks, keeps the transfer alive past the ACK timeout and
// resumes from where it stopped.
void TestPause(const fs::path& path) {
  Transport tx;
  Session session(Sink(tx), kIgnoredProgress);
  Expect(session.Start(path, "paused.bin", At(0)), "pause transfer starts");
  Expect(session.Pump(At(0)), "one chunk before the pause");
  const auto before = tx.count();

  session.SetSendPaused(true);
  Expect(session.send_paused(), "the pause state is reported");
  // 5 x 10 s exceeds the 30 s ACK timeout: the paused pump refreshes the
  // deadline, so the transfer must survive.
  for (int step = 1; step <= 5; ++step)
    Expect(!session.Pump(At(step * 10000)), "a paused session queues nothing");
  Expect(tx.count() == before, "no chunk is queued while paused");
  Expect(session.Snapshot().status == Status::Sending,
         "a paused transfer does not fail on the ACK timeout");
  Expect(session.Sending(), "a paused transfer is still owned by the session");

  session.SetSendPaused(false);
  Expect(!session.send_paused(), "the pause state clears");
  Expect(session.Pump(At(60000)), "sending resumes after the pause");
  Expect(tx.count() == before + 1, "exactly one chunk after resuming");
}

// Cancelling before the first chunk keeps working, also while pacing is armed.
void TestCancel(const fs::path& path) {
  Transport tx;
  Session session(Sink(tx), kIgnoredProgress);
  Expect(!session.CancelSend(), "cancelling without a transfer is rejected");
  Expect(session.Start(path, "cancel.bin", At(0)), "cancelling transfer starts");
  session.SetSendRate(64 * 1024);
  Expect(!session.Pump(At(0)), "a paced session without credit queues no chunk");
  Expect(session.CancelSend(), "an unstarted transfer cancels");
  Expect(session.Snapshot().status == Status::Cancelled, "cancel marks the transfer cancelled");
  Expect(tx.count() == 1, "cancel replaces the data chunk with one cancel packet");
  crossdesk::FileChunkView cancel;
  Expect(crossdesk::DecodeFileChunk(tx.chunks.front().data(), tx.chunks.front().size(), &cancel) &&
             cancel.header.flags == crossdesk::kFileCancelFlag && !cancel.payload_size,
         "cancel is a header-only packet on the file stream");
  Expect(tx.feedback == 0, "cancel needs no feedback packet");
  Expect(!session.Pump(At(1000)), "a cancelled transfer never pumps");
  Expect(!session.Sending(), "a cancelled session is not sending");
}

// The pacing option seeds the session rate for adapters that configure it once.
void TestOptionsRate() {
  Transport tx;
  Options options;
  options.send_rate_bytes_per_second = 256 * 1024;
  Session session(Sink(tx), kIgnoredProgress, options);
  Expect(session.send_rate() == 256 * 1024, "the options rate seeds the session");
}

}  // namespace

int main() {
  const auto path = TempFile(1024 * 1024);
  TestUnlimited(path);
  TestLimited(path);
  TestLowRate(path);
  TestPause(path);
  TestCancel(path);
  TestOptionsRate();
  std::error_code ec;
  fs::remove_all(path.parent_path(), ec);
  if (g_failures) {
    std::printf("%d file transfer rate checks failed\n", g_failures);
    return 1;
  }
  std::printf("file transfer rate checks passed\n");
  return 0;
}
