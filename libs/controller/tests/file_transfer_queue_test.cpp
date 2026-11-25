/*
 * File transfer queue regression test.
 *
 * The desktop manager keeps one Session per running file and the queue policy
 * in file_transfer::SendQueue (header only, no I/O, no session). This test
 * drives that policy exactly the way FileTransferManager does, and runs real
 * Sessions next to it, so bounded fan-out, queue level pause/resume, per-item
 * holds, retry and the batch summary are observed with no peer, no runtime and
 * no socket.
 *
 * Covered here:
 *   - bounded fan-out: two files of one peer run at once, a third waits, and a
 *     finished file admits it; the hard cap is never exceeded;
 *   - two concurrent senders on separate sessions, each with its own wire id,
 *     where cancelling one leaves the other running;
 *   - pause-all then resume-all: holds reach every live item, a parked queue
 *     admits nothing, and a paused transfer survives the 30 s ACK timeout;
 *   - retry of a failed row: same name, same peer, brand new wire id, and the
 *     first replayed chunk carries that new id at offset 0;
 *   - clear-all keeps the running row and its slot;
 *   - remove-finished drops terminal rows only;
 *   - the batch summary (total/completed/failed/active/queued/paused and the
 *     partial success flag) and its per-peer isolation.
 *
 * Not covered here, because it needs the RTC runtime: the authorization
 * re-check before Start, the Cancelling handshake on the wire, releasing a
 * browser destination for a cancelled queued item, and the UI row mapping.
 * Those stay on the manual checklist in docs/UU_PARITY.md.
 */

#include <file_transfer_core.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::file_transfer::SendQueue;
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
                    ("crossdesk-transfer-queue-test-" + std::to_string(stamp));
  fs::create_directories(root, ec);
  const auto path = root / ("payload-" + std::to_string(size) + ".bin");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const std::vector<char> block(64 * 1024, 'Q');
  for (std::uintmax_t written = 0; written < size;) {
    const auto step = (std::min)(static_cast<std::uintmax_t>(block.size()), size - written);
    output.write(block.data(), static_cast<std::streamsize>(step));
    written += step;
  }
  output.close();
  return path;
}

std::uint64_t SizeOf(const fs::path& path) {
  std::error_code ec;
  return static_cast<std::uint64_t>(fs::file_size(path, ec));
}

Clock::time_point At(std::int64_t milliseconds) {
  return Clock::time_point{} + std::chrono::milliseconds(milliseconds);
}

// Mirrors FileTransferManager::TakeStartable: the oldest waiting item that has a
// free slot and no hold starts, every other entry keeps its place in line.
bool Schedule(SendQueue& queue, const std::string& peer,
              std::deque<std::uint32_t>& waiting, std::size_t limit) {
  bool started = false;
  std::deque<std::uint32_t> kept;
  for (const auto id : waiting) {
    if (!started && queue.Active(peer) < limit && queue.Start(id)) {
      started = true;
      continue;
    }
    kept.push_back(id);
  }
  waiting.swap(kept);
  return started;
}

// Two files of one peer run at once; a third waits until a slot frees.
void TestBoundedFanOut() {
  SendQueue queue;
  const std::string peer = "peer-1";
  std::deque<std::uint32_t> waiting;
  for (std::uint32_t id = 1; id <= 3; ++id) {
    Expect(queue.Track(id, "file-" + std::to_string(id) + ".bin", peer, 4096),
           "a file is tracked as waiting");
    waiting.push_back(id);
  }
  Expect(queue.Waiting(peer) == 3, "three files wait for a slot");
  Expect(Schedule(queue, peer, waiting, 2), "the scheduler fills the first slot");
  Expect(Schedule(queue, peer, waiting, 2), "the scheduler fills the second slot");
  Expect(!Schedule(queue, peer, waiting, 2), "a third file waits under a fan-out of two");
  Expect(queue.Active(peer) == 2, "exactly two senders run at once");
  Expect(queue.Waiting(peer) == 1, "the third file is still waiting");
  Expect(waiting.size() == 1 && waiting.front() == 3, "the waiting file keeps its order");

  // A finished file frees its slot and the queue refills itself.
  Expect(queue.Finish(1, SendQueue::State::Completed), "the first file finishes");
  Expect(Schedule(queue, peer, waiting, 2), "a finished file admits the waiting one");
  Expect(queue.Active(peer) == 2, "the freed slot is filled");
  Expect(queue.Waiting(peer) == 0, "nothing is left waiting");

  // The hard cap the manager clamps to still schedules one file per slot.
  SendQueue wide;
  std::deque<std::uint32_t> many;
  for (std::uint32_t id = 101; id <= 105; ++id) {
    wide.Track(id, "wide.bin", peer, 4096);
    many.push_back(id);
  }
  for (int slot = 0; slot < 4; ++slot)
    Expect(Schedule(wide, peer, many, 4), "a slot of the hard cap starts");
  Expect(!Schedule(wide, peer, many, 4), "a fifth file never exceeds the hard cap of four");
  Expect(wide.Active(peer) == 4 && wide.Waiting(peer) == 1,
         "four run and one waits at the hard cap");
}

// Each running file owns a session and a wire id, so cancelling one leaves the
// neighbour streaming.
void TestConcurrentSenders(const fs::path& path) {
  Transport first, second;
  Session a(Sink(first), kIgnoredProgress);
  Session b(Sink(second), kIgnoredProgress);
  Expect(a.Start(path, "first.bin", At(0), 11), "the first concurrent sender starts");
  Expect(b.Start(path, "second.bin", At(0), 12), "the second concurrent sender starts");
  for (int step = 1; step <= 2; ++step) {
    Expect(a.Pump(At(step * 10)), "the first sender keeps pumping");
    Expect(b.Pump(At(step * 10)), "the second sender keeps pumping");
  }
  Expect(first.count() == 2 && second.count() == 2,
         "both senders advance in the same tick");

  crossdesk::FileChunkView first_view, second_view;
  Expect(crossdesk::DecodeFileChunk(first.chunks.front().data(),
                                    first.chunks.front().size(), &first_view),
         "the first sender emits a decodable chunk");
  Expect(crossdesk::DecodeFileChunk(second.chunks.front().data(),
                                    second.chunks.front().size(), &second_view),
         "the second sender emits a decodable chunk");
  Expect(first_view.header.file_id == 11 && second_view.header.file_id == 12,
         "each concurrent sender keeps its own wire id");

  Expect(a.CancelSend(), "one sender cancels alone");
  Expect(a.Snapshot().status == Status::Cancelled, "the cancelled sender is terminal");
  Expect(b.Snapshot().status == Status::Sending, "the neighbouring sender keeps running");
  const auto before = second.count();
  Expect(b.Pump(At(100)), "the surviving sender still pumps");
  Expect(second.count() == before + 1, "the surviving sender emits exactly one chunk");
}

// Pause all holds every live item and parks the queue; resume all releases both.
void TestPauseAll(const fs::path& path) {
  SendQueue queue;
  const std::string peer = "peer-2";
  Expect(queue.Track(21, "a.bin", peer, 4096) && queue.Track(22, "b.bin", peer, 4096) &&
             queue.Track(23, "c.bin", peer, 4096),
         "three files are tracked");
  std::deque<std::uint32_t> waiting{21, 22, 23};
  Expect(Schedule(queue, peer, waiting, 2) && Schedule(queue, peer, waiting, 2),
         "two of the three files take both slots");

  Transport first, second;
  Session a(Sink(first), kIgnoredProgress);
  Session b(Sink(second), kIgnoredProgress);
  Expect(a.Start(path, "a.bin", At(0), 21) && b.Start(path, "b.bin", At(0), 22),
         "both admitted files run on their own session");
  Expect(a.Pump(At(0)) && b.Pump(At(0)), "both senders emit one chunk before the pause");
  const auto before_a = first.count(), before_b = second.count();

  Expect(queue.PauseAll(peer) == 3, "pause all holds every live item");
  a.SetSendPaused(true);
  b.SetSendPaused(true);
  // 4 x 10 s exceeds the 30 s ACK timeout: a held transfer must survive it.
  for (int step = 1; step <= 4; ++step) {
    Expect(!a.Pump(At(step * 10000)), "a paused sender queues no chunk");
    Expect(!b.Pump(At(step * 10000)), "a paused sender queues no chunk");
  }
  Expect(first.count() == before_a && second.count() == before_b,
         "nothing leaves a paused sender");
  Expect(a.Snapshot().status == Status::Sending && b.Snapshot().status == Status::Sending,
         "a paused transfer survives the ACK timeout");
  Expect(!Schedule(queue, peer, waiting, 2), "a parked queue admits nothing");
  const auto parked = queue.Summarize(peer);
  Expect(parked.paused == 3 && parked.active == 2 && parked.queued == 0,
         "the summary reports three paused items and no free waiting one");

  Expect(queue.ResumeAll(peer) == 3, "resume all releases every hold");
  a.SetSendPaused(false);
  b.SetSendPaused(false);
  Expect(a.Pump(At(50000)), "the first sender resumes after the pause");
  Expect(b.Pump(At(50000)), "the second sender resumes after the pause");
  Expect(first.count() == before_a + 1 && second.count() == before_b + 1,
         "each resumed sender emits exactly one chunk");

  // A released waiting item takes the slot as soon as one is free again.
  Expect(queue.Finish(21, SendQueue::State::Completed), "the first file finishes");
  Expect(Schedule(queue, peer, waiting, 2), "the resumed queue admits its waiting file");
  Expect(queue.Active(peer) == 2 && queue.Waiting(peer) == 0, "the third file now runs");
}

// A retry keeps the identity the user sees and takes a new wire id.
void TestRetry(const fs::path& path) {
  constexpr std::uint32_t kFailedId = 31;
  SendQueue queue;
  const std::string peer = "peer-3";
  Expect(queue.Track(kFailedId, "retry.bin", peer, SizeOf(path)),
         "the failed item is tracked");
  std::deque<std::uint32_t> waiting{kFailedId};
  Expect(Schedule(queue, peer, waiting, 2), "the item takes a slot");
  Expect(queue.Finish(kFailedId, SendQueue::State::Failed), "the item fails");
  Expect(queue.Retryable(kFailedId), "a failed row is retryable");
  Expect(!queue.Retryable(999), "an unknown id is not retryable");
  Expect(queue.Active(peer) == 0, "the failed attempt released its slot");

  const auto new_id = Session::NextFileId();
  Expect(new_id != kFailedId, "a retry allocates a different id");
  Expect(new_id != 0, "a retry id is never zero");
  Expect(queue.Retry(kFailedId, new_id), "the failed row is re-queued under the new id");
  Expect(queue.Find(kFailedId) == nullptr, "the failed id left the queue");
  const auto* replay = queue.Find(new_id);
  Expect(replay != nullptr, "the replayed row exists");
  if (!replay) return;
  Expect(replay->name == "retry.bin", "the replay keeps the original name");
  Expect(replay->remote_id == peer, "the replay keeps its peer");
  Expect(replay->size == SizeOf(path), "the replay keeps the original size");
  Expect(replay->state == SendQueue::State::Queued, "the replay waits for a slot");
  Expect(!queue.Retryable(new_id), "a waiting replay is not retryable");
  Expect(!queue.Retry(kFailedId, 4242), "an id that is gone cannot be retried again");

  // The replay restarts from offset zero under the new id: the receiver keeps a
  // cancellation tombstone per id, so reusing the old one would be rejected.
  Transport tx;
  Session session(Sink(tx), kIgnoredProgress);
  Expect(session.Start(path, replay->name, At(0), new_id), "the replay starts");
  Expect(session.Pump(At(0)), "the replay emits its first chunk");
  crossdesk::FileChunkView view;
  Expect(crossdesk::DecodeFileChunk(tx.chunks.front().data(), tx.chunks.front().size(), &view),
         "the replayed chunk decodes");
  Expect(view.header.file_id == new_id, "the wire carries the new id");
  Expect(view.header.offset == 0 && (view.header.flags & 1) != 0,
         "the replay restarts at offset 0 and marks its first chunk");
}

// Clear all drops waiting and finished rows and keeps the running one.
void TestClearAll() {
  const std::string peer = "peer-4";
  SendQueue queue;
  queue.Track(41, "active.bin", peer, 1024);
  queue.Track(42, "waiting.bin", peer, 1024);
  queue.Track(43, "failed.bin", peer, 1024);
  queue.Track(44, "done.bin", peer, 1024);
  queue.Track(45, "other-peer.bin", "peer-5", 1024);
  std::deque<std::uint32_t> waiting{41, 42};
  Expect(Schedule(queue, peer, waiting, 1), "one file runs while the second waits");
  queue.Finish(43, SendQueue::State::Failed);
  queue.Finish(44, SendQueue::State::Completed);

  Expect(queue.ClearAll(peer) == 3, "clear all drops the waiting and finished rows");
  Expect(queue.Find(41) != nullptr, "the running row survives clear all");
  Expect(queue.Find(42) == nullptr && queue.Find(43) == nullptr && queue.Find(44) == nullptr,
         "the waiting and finished rows are gone");
  Expect(queue.Active(peer) == 1, "clear all keeps the running slot");
  Expect(queue.Find(45) != nullptr, "clear all never touches another peer");
}

// Remove finished drops terminal rows only.
void TestRemoveFinished() {
  const std::string peer = "peer-6";
  SendQueue queue;
  queue.Track(61, "active.bin", peer, 1024);
  queue.Track(62, "waiting.bin", peer, 1024);
  queue.Track(63, "failed.bin", peer, 1024);
  queue.Track(64, "cancelled.bin", peer, 1024);
  queue.Track(65, "done.bin", peer, 1024);
  std::deque<std::uint32_t> waiting{61, 62};
  Expect(Schedule(queue, peer, waiting, 1), "one file runs while the second waits");
  queue.Finish(63, SendQueue::State::Failed);
  queue.Finish(64, SendQueue::State::Cancelled);
  queue.Finish(65, SendQueue::State::Completed);

  Expect(queue.RemoveFinished(peer) == 3, "remove finished drops the three terminal rows");
  Expect(queue.Find(63) == nullptr && queue.Find(64) == nullptr && queue.Find(65) == nullptr,
         "no terminal row survives remove finished");
  Expect(queue.Find(61) != nullptr, "the running row survives remove finished");
  Expect(queue.Find(62) != nullptr, "the waiting row survives remove finished");
  Expect(queue.Active(peer) == 1 && queue.Waiting(peer) == 1,
         "the running and the waiting row keep their place");
  Expect(queue.RemoveFinished(peer) == 0, "a second remove finished changes nothing");
}

// The batch summary is what the window shows as partial success.
void TestBatchSummary() {
  const std::string peer = "peer-7";
  SendQueue queue;
  for (std::uint32_t id = 71; id <= 74; ++id) queue.Track(id, "batch.bin", peer, 1024);
  queue.Track(75, "other.bin", "peer-8", 1024);

  auto summary = queue.Summarize(peer);
  Expect(summary.total == 4 && summary.queued == 4, "a fresh batch is four queued files");
  Expect(summary.completed == 0 && summary.failed == 0 && summary.active == 0 &&
             summary.paused == 0,
         "a fresh batch has no result yet");
  Expect(!summary.batch_partial, "a fresh batch is not a partial success");

  std::deque<std::uint32_t> waiting{71, 72, 73, 74};
  Expect(Schedule(queue, peer, waiting, 2) && Schedule(queue, peer, waiting, 2),
         "two files of the batch run");
  summary = queue.Summarize(peer);
  Expect(summary.active == 2 && summary.queued == 2 && summary.total == 4,
         "the summary follows the admitted slots");

  queue.Finish(71, SendQueue::State::Completed);
  queue.Finish(72, SendQueue::State::Failed);
  summary = queue.Summarize(peer);
  Expect(summary.completed == 1 && summary.failed == 1 && summary.active == 0,
         "one landed and one failed");
  Expect(summary.batch_partial, "a mixed batch is a partial success");
  Expect(summary.queued == 2 && summary.total == 4, "the waiting files still count");

  queue.Pause(73);
  summary = queue.Summarize(peer);
  Expect(summary.paused == 1 && summary.queued == 1,
         "a held file counts as paused, not as queued");

  const auto other = queue.Summarize("peer-8");
  Expect(other.total == 1 && other.queued == 1 && other.paused == 0,
         "the summary is per peer");

  Expect(queue.ClearAll(peer) == 4, "clear all drops what is left of the batch");
  summary = queue.Summarize(peer);
  Expect(summary.total == 0 && summary.batch_partial == false,
         "clearing the rows also clears the counters");
}

}  // namespace

int main() {
  const auto path = TempFile(256 * 1024);
  TestBoundedFanOut();
  TestConcurrentSenders(path);
  TestPauseAll(path);
  TestRetry(path);
  TestClearAll();
  TestRemoveFinished();
  TestBatchSummary();
  std::error_code ec;
  fs::remove_all(path.parent_path(), ec);
  if (g_failures) {
    std::printf("%d file transfer queue checks failed\n", g_failures);
    return 1;
  }
  std::printf("file transfer queue checks passed\n");
  return 0;
}
