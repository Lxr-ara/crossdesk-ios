/*
 * 跨会话续传数据面回归测试。
 *
 * 这里验证两端的契约：发送端从 resume_offset 起发（本次会话的首块仍然带文件名），
 * 接收端只在「本机侧车认可这个起点」时才接受 offset != 0 的首块，其余一切情况
 * 维持原来的 reject()。没有框架、没有线程、没有 peer 层：一个假传输把
 * file / file_feedback 两路包存下来，再用显式时钟喂给另一端。
 */

#include <file_transfer_core.h>
#include <file_transfer_resume.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::file_transfer::Options;
using crossdesk::file_transfer::ReceiveState;
using crossdesk::file_transfer::ReceiveStatus;
using crossdesk::file_transfer::ResumeRecord;
using crossdesk::file_transfer::Session;
using crossdesk::file_transfer::Status;
using crossdesk::file_transfer::TargetMtimeMs;
using crossdesk::file_transfer::WriteResumeRecord;

namespace {

int g_failures = 0;
fs::path g_root;

constexpr std::uint64_t kChunk = 64 * 1024;
// 192 KiB = 三块，正好让续传后还剩两块，覆盖「首块续传 + 后续连续块」两条路径。
constexpr std::uint64_t kTotal = 3 * kChunk;
constexpr std::uint64_t kResume = kChunk;
// 侧车里未被认可的区域填毒字节：实现一旦信任它们，逐字节比对就会失败。
constexpr char kPoison = static_cast<char>(0xEE);

using Clock = Session::Clock;

void Expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
}

Clock::time_point At(std::int64_t milliseconds) {
  return Clock::time_point{} + std::chrono::milliseconds(milliseconds);
}

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
#if defined(__cpp_char8_t)
  return std::string(text.begin(), text.end());
#else
  return text;
#endif
}

fs::path FreshDir(const char* name) {
  const auto dir = g_root / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// 源文件内容由偏移决定，任何错位、覆盖或漏写都会在字节比对里暴露。
const std::string& Bytes() {
  static const std::string bytes = [] {
    std::string data(kTotal, '\0');
    for (std::size_t index = 0; index < data.size(); ++index)
      data[index] = static_cast<char>((index * 17 + 3) & 0xFF);
    return data;
  }();
  return bytes;
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// 目录里的条目数：拒绝路径要求「什么都不留下」，包括暂存目录。
std::size_t Entries(const fs::path& dir) {
  std::error_code ec;
  std::size_t count = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    ++count;
  return count;
}

// 假的 file / file_feedback 两路传输：数据块与 ACK 分开存放，顺序即发送顺序。
struct Transport {
  std::vector<std::string> chunks;
  std::vector<std::string> feedback;

  int Send(const char* stream, const char* data, std::size_t size) {
    if (std::string(stream) == crossdesk::kFileStream) chunks.emplace_back(data, size);
    else feedback.emplace_back(data, size);
    return 0;
  }
};

Session::Send Sink(Transport& tx) {
  return [&tx](const char* stream, const char* data, std::size_t size) {
    return tx.Send(stream, data, size);
  };
}

const Session::Progress kIgnoredProgress =
    [](const std::string&, double, bool, const std::string&) {};

// 接收端外壳：保留反馈包，并按顺序记下每次 ReceiveProgress，
// 用来断言「续传后的第一个回调已经带上 offset」。
struct Receiver {
  Transport feedback;
  std::vector<ReceiveState> states;
  Session session;

  explicit Receiver(const Options& options)
      : session(
            [this](const char* stream, const char* data, std::size_t size) {
              return feedback.Send(stream, data, size);
            },
            kIgnoredProgress, options,
            [this](const ReceiveState& state) { states.push_back(state); }) {}
};

// 把接收端回的 ACK 按顺序交回发送端，与真实 file_feedback 的往返一致。
void Relay(Session& sender, Transport& back, Clock::time_point now) {
  for (const auto& packet : back.feedback) sender.Ack(packet.data(), packet.size(), now);
  back.feedback.clear();
}

// 侧车目录里造一份「前缀 + 未校验区域」的目标文件和对应记录。
// 记录必须描述一个真实存在的目标文件，长度等于 total_size。
bool WriteSidecar(const fs::path& dir, const std::string& name, std::uint64_t total,
                  std::uint64_t received, std::uint32_t wire_id, const std::string& body) {
  WriteBytes(dir / name, body);
  ResumeRecord record;
  record.target_path = name;
  record.total_size = total;
  record.target_mtime_ms = TargetMtimeMs(dir / name);
  record.received = received;
  record.wire_id = wire_id;
  std::string error;
  return WriteResumeRecord(dir, record, &error);
}

// (a) 发送端：resume_offset = 64 KiB 的首块从 64 KiB 开始，仍然带 first 位和文件名。
void TestSenderResumeStart(const std::string& source) {
  Transport tx;
  Session sender(Sink(tx), kIgnoredProgress);
  Expect(sender.Start(source, "resume.bin", kTotal, At(0), 0, kResume),
         "a send with a resume offset starts");
  Expect(sender.Snapshot().status == Status::Sending,
         "a resumed send is sending");
  Expect(sender.Snapshot().acknowledged == kResume,
         "a resumed send starts with the offset already acknowledged");

  Expect(sender.Pump(At(0)), "the resumed first chunk is queued");
  Expect(tx.chunks.size() == 1, "one chunk per pump");
  crossdesk::FileChunkView first;
  Expect(crossdesk::DecodeFileChunk(tx.chunks.front().data(),
                                    tx.chunks.front().size(), &first),
         "the resumed first chunk decodes on the wire format");
  Expect(first.header.offset == kResume,
         "the resumed first chunk starts at the resume offset");
  Expect((first.header.flags & 1) != 0,
         "the resumed first chunk keeps the first flag");
  Expect(first.header.name_len > 0,
         "the resumed first chunk still carries the file name");
  Expect(first.file_name == "resume.bin",
         "the resumed first chunk carries the remote name");
  Expect(first.header.total_size == kTotal,
         "the resumed first chunk declares the full length");
  Expect(first.payload_size == kChunk,
         "the resumed first chunk carries kFileChunkSize bytes");
  Expect(std::equal(first.payload, first.payload + first.payload_size,
                    Bytes().begin() + static_cast<std::ptrdiff_t>(kResume)),
         "the resumed payload is read from the resume offset");

  Expect(sender.Pump(At(10)), "the resumed send continues with a second chunk");
  Expect(tx.chunks.size() == 2, "the resumed send queues one chunk per pump");
  crossdesk::FileChunkView second;
  Expect(crossdesk::DecodeFileChunk(tx.chunks.back().data(),
                                    tx.chunks.back().size(), &second),
         "the second resumed chunk decodes");
  Expect(second.header.offset == kResume + kChunk,
         "the second chunk continues right after the resumed first chunk");
  Expect((second.header.flags & 1) == 0,
         "only the first chunk of the session carries the first flag");
  Expect(second.header.name_len == 0,
         "later chunks carry no file name");
  Expect((second.header.flags & 2) != 0,
         "the chunk that reaches the total marks the transfer last");
  Expect(std::equal(second.payload, second.payload + second.payload_size,
                    Bytes().begin() + static_cast<std::ptrdiff_t>(kResume + kChunk)),
         "the second payload continues at 128 KiB");

  // 旧签名（本地路径 + 由本地文件决定总长）必须仍然可用。
  Transport legacy_tx;
  Session legacy(Sink(legacy_tx), kIgnoredProgress);
  Expect(legacy.Start(fs::path(source), "legacy.bin", At(0)),
         "the legacy path signature still starts a plain send");
  Expect(legacy.Pump(At(0)), "the legacy signature still queues a chunk");
  crossdesk::FileChunkView plain;
  Expect(!legacy_tx.chunks.empty() &&
             crossdesk::DecodeFileChunk(legacy_tx.chunks.front().data(),
                                        legacy_tx.chunks.front().size(), &plain) &&
             plain.header.offset == 0,
         "the legacy signature still starts at offset 0");
}

// (b) 起点等于总长 = 没有剩余字节；起点越界同样拒绝，且不发任何包。
void TestSenderNothingToSend(const std::string& source) {
  Transport tx;
  Session sender(Sink(tx), kIgnoredProgress);
  Expect(!sender.Start(source, "done.bin", kTotal, At(0), 0, kTotal),
         "a resume offset at the total length starts nothing");
  Expect(sender.Snapshot().status == Status::Idle,
         "a refused resume leaves the session idle");
  Expect(!sender.Pump(At(0)) && tx.chunks.empty(),
         "a refused resume emits no packet");
  Expect(!sender.Sending(), "a refused resume is not sending");

  Expect(!sender.Start(source, "over.bin", kTotal, At(0), 0, kTotal + 1),
         "a resume offset beyond the total is refused");
  Expect(!sender.Start(source, "wide.bin", kTotal, At(0), 0, kTotal + kChunk),
         "a resume offset far beyond the total is refused");
  Expect(!sender.Pump(At(10)) && tx.chunks.empty(),
         "an out of range resume emits no packet");
  Expect(sender.Snapshot().status == Status::Idle,
         "an out of range resume leaves the session idle");
}

// (c) 接收端：有匹配侧车时接受续传首块，补齐剩余 128 KiB 后文件逐字节等于源文件。
void TestReceiverResume(const std::string& source) {
  const std::string name = "resume.bin";
  const auto resume_dir = FreshDir("receiver-resume/resume");
  const auto dest = FreshDir("receiver-resume/dest");
  std::string prefix = Bytes();
  std::fill(prefix.begin() + static_cast<std::ptrdiff_t>(kResume), prefix.end(), kPoison);
  Expect(WriteSidecar(resume_dir, name, kTotal, kResume, 9, prefix),
         "the receiver resume sidecar is written");

  Options options;
  options.resume.directory = resume_dir;
  options.resume.enabled = true;
  Receiver receiver(options);
  receiver.session.SetDirectory(dest);
  Expect(receiver.session.resume_context().enabled,
         "the resume context reaches the session");

  Transport out;
  Session sender(Sink(out), kIgnoredProgress);
  Expect(sender.Start(source, name, kTotal, At(0), 11, kResume),
         "the resumed send starts for the receiver case");
  Expect(sender.Pump(At(0)), "the resumed first chunk is queued");
  Expect(out.chunks.size() == 1, "one chunk is queued before the first ACK");

  Expect(receiver.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(0)),
         "a resumed first chunk with a matching sidecar is accepted");
  Expect(receiver.states.size() == 1, "an accepted resume reports one progress event");
  Expect(!receiver.states.empty() && receiver.states.back().received == kResume + kChunk,
         "the first progress report already carries the resumed offset");
  Expect(!receiver.states.empty() && receiver.states.back().status == ReceiveStatus::Receiving,
         "an accepted resume is still receiving");
  Expect(!receiver.states.empty() && receiver.states.back().total == kTotal,
         "an accepted resume reports the full total length");
  Relay(sender, receiver.feedback, At(0));

  Expect(sender.Pump(At(10)), "the resumed send continues after the ACK");
  Expect(out.chunks.size() == 2, "the resumed send queues the remaining chunk");
  Expect(receiver.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(10)),
         "the remaining 128 KiB complete the resumed transfer");
  Relay(sender, receiver.feedback, At(10));

  Expect(sender.Snapshot().status == Status::Completed,
         "the resumed send completes on the final ACK");
  Expect(sender.Snapshot().acknowledged == kTotal,
         "the resumed send acknowledges the full length");
  Expect(receiver.states.size() == 2, "the resumed receive reports both chunks");
  Expect(!receiver.states.empty() && receiver.states.back().status == ReceiveStatus::Completed,
         "the resumed receive completes");
  Expect(!receiver.states.empty() && receiver.states.back().received == kTotal,
         "the completed receive reports the full length");

  const auto published = dest / name;
  Expect(fs::is_regular_file(published),
         "the resumed transfer publishes the target name");
  Expect(ReadBytes(published) == Bytes(),
         "the published bytes equal the source file");
  Expect(Entries(dest) == 1,
         "the resumed chunk is not published as a duplicate");
  Expect(fs::is_regular_file(resume_dir / "resume.bin.crossdesk-resume"),
         "the sidecar stays in the resume directory");
  Expect(!fs::exists(dest / "resume.bin.crossdesk-resume"),
         "the sidecar is never copied into the staging output");
}

// (d) 没有侧车（或续传关着）时，同一个续传首块照旧被拒，且不留半成品。
void TestReceiverWithoutSidecar(const std::string& source) {
  const std::string name = "resume.bin";
  const auto empty_dir = FreshDir("receiver-no-sidecar/empty");
  const auto dest = FreshDir("receiver-no-sidecar/dest");
  Options options;
  options.resume.directory = empty_dir;
  options.resume.enabled = true;
  Receiver receiver(options);
  receiver.session.SetDirectory(dest);

  Transport out;
  Session sender(Sink(out), kIgnoredProgress);
  Expect(sender.Start(source, name, kTotal, At(0), 21, kResume),
         "the forged resume send starts");
  Expect(sender.Pump(At(0)), "the forged resumed first chunk is queued");
  Expect(!receiver.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(0)),
         "a resumed first chunk without a sidecar is rejected");
  Expect(receiver.states.size() == 1, "a rejected resume reports one progress event");
  Expect(!receiver.states.empty() && receiver.states.back().status == ReceiveStatus::Failed,
         "a rejected resume reports Failed");
  Expect(!receiver.states.empty() && receiver.states.back().received == 0,
         "a rejected resume reports no received bytes");
  Expect(Entries(dest) == 0, "a rejected resume leaves no partial file behind");
  Relay(sender, receiver.feedback, At(0));
  Expect(sender.Snapshot().status == Status::Failed,
         "the sender fails on the reject ACK");

  // 续传上下文关着（默认 Options）时，同一个首块也必须被拒。
  const auto plain_dest = FreshDir("receiver-no-sidecar/plain");
  Receiver plain(Options{});
  plain.session.SetDirectory(plain_dest);
  Expect(!plain.session.resume_context().enabled,
         "the resume context is disabled by default");
  Expect(!plain.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(0)),
         "a resumed first chunk is rejected while resume is disabled");
  Expect(plain.states.size() == 1 && plain.states.back().status == ReceiveStatus::Failed,
         "a disabled resume reports Failed");
  Expect(Entries(plain_dest) == 0,
         "a disabled resume leaves no partial file behind");
}

// (e) 侧车的总大小、已收字节、目标新鲜度任一不匹配都必须拒绝。
void TestReceiverMismatch(const std::string& source) {
  const std::string name = "resume.bin";

  // ① 记录里的 total_size 与首块声明不一致。
  {
    const auto resume_dir = FreshDir("receiver-mismatch/total");
    const auto dest = FreshDir("receiver-mismatch/total-dest");
    Expect(WriteSidecar(resume_dir, name, kTotal + kChunk, kResume, 4,
                        std::string(kTotal + kChunk, kPoison)),
           "a sidecar with another total size is written");
    Options options;
    options.resume.directory = resume_dir;
    options.resume.enabled = true;
    Receiver receiver(options);
    receiver.session.SetDirectory(dest);
    Transport out;
    Session sender(Sink(out), kIgnoredProgress);
    Expect(sender.Start(source, name, kTotal, At(0), 41, kResume),
           "the total mismatch send starts");
    Expect(sender.Pump(At(0)), "the total mismatch chunk is queued");
    Expect(!receiver.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(0)),
           "a sidecar with another total size is rejected");
    Expect(receiver.states.size() == 1 && receiver.states.back().status == ReceiveStatus::Failed,
           "a total size mismatch reports Failed");
    Expect(Entries(dest) == 0, "a total size mismatch leaves no partial file behind");
  }

  // ② 记录里的 received 不是首块的 offset（对端不能自己挑一个位置）。
  {
    const auto resume_dir = FreshDir("receiver-mismatch/received");
    const auto dest = FreshDir("receiver-mismatch/received-dest");
    std::string prefix = Bytes();
    std::fill(prefix.begin() + static_cast<std::ptrdiff_t>(2 * kChunk), prefix.end(), kPoison);
    Expect(WriteSidecar(resume_dir, name, kTotal, 2 * kChunk, 5, prefix),
           "a sidecar with another received offset is written");
    Options options;
    options.resume.directory = resume_dir;
    options.resume.enabled = true;
    Receiver receiver(options);
    receiver.session.SetDirectory(dest);
    Transport out;
    Session sender(Sink(out), kIgnoredProgress);
    Expect(sender.Start(source, name, kTotal, At(0), 42, kResume),
           "the received mismatch send starts");
    Expect(sender.Pump(At(0)), "the received mismatch chunk is queued");
    Expect(!receiver.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(0)),
           "a sidecar with another received offset is rejected");
    Expect(receiver.states.size() == 1 && receiver.states.back().status == ReceiveStatus::Failed,
           "a received offset mismatch reports Failed");
    Expect(Entries(dest) == 0, "a received offset mismatch leaves no partial file behind");
  }

  // ③ 记录还在，但目标文件已经不是写记录时的那一份（ReadResumeRecord 判 stale）。
  {
    const auto resume_dir = FreshDir("receiver-mismatch/stale");
    const auto dest = FreshDir("receiver-mismatch/stale-dest");
    std::string prefix = Bytes();
    std::fill(prefix.begin() + static_cast<std::ptrdiff_t>(kResume), prefix.end(), kPoison);
    Expect(WriteSidecar(resume_dir, name, kTotal, kResume, 6, prefix),
           "the stale fixture sidecar is written");
    // 记录写完之后目标文件被改动：长度变了，前缀就不再可信。
    WriteBytes(resume_dir / name, std::string(kTotal + kChunk, kPoison));
    Options options;
    options.resume.directory = resume_dir;
    options.resume.enabled = true;
    Receiver receiver(options);
    receiver.session.SetDirectory(dest);
    Transport out;
    Session sender(Sink(out), kIgnoredProgress);
    Expect(sender.Start(source, name, kTotal, At(0), 43, kResume),
           "the stale sidecar send starts");
    Expect(sender.Pump(At(0)), "the stale sidecar chunk is queued");
    Expect(!receiver.session.Receive(out.chunks.back().data(), out.chunks.back().size(), At(0)),
           "a stale sidecar is rejected instead of trusted");
    Expect(receiver.states.size() == 1 && receiver.states.back().status == ReceiveStatus::Failed,
           "a stale sidecar reports Failed");
    Expect(Entries(dest) == 0, "a stale sidecar leaves no partial file behind");
  }
}

// (f) offset == 0 的老路径：正常收发一整趟，行为与改动前一致。
void TestOffsetZeroRoundTrip(const std::string& source) {
  const std::string name = "plain.bin";
  const auto dest = FreshDir("round-trip/dest");
  Receiver receiver(Options{});
  receiver.session.SetDirectory(dest);

  Transport out;
  Session sender(Sink(out), kIgnoredProgress);
  Expect(sender.Start(source, name, kTotal, At(0), 31),
         "a plain send starts at offset 0");
  Expect(sender.Snapshot().acknowledged == 0,
         "a plain send acknowledges nothing up front");

  bool accepted = true;
  for (int step = 0; step < 3; ++step) {
    if (!sender.Pump(At(step * 10))) accepted = false;
    if (out.chunks.size() != static_cast<std::size_t>(step + 1)) accepted = false;
    const auto& packet = out.chunks.back();
    if (!receiver.session.Receive(packet.data(), packet.size(), At(step * 10))) accepted = false;
    Relay(sender, receiver.feedback, At(step * 10));
  }
  Expect(accepted, "the plain round trip is accepted chunk by chunk");

  crossdesk::FileChunkView first;
  Expect(!out.chunks.empty() &&
             crossdesk::DecodeFileChunk(out.chunks.front().data(),
                                        out.chunks.front().size(), &first),
         "the plain first chunk decodes");
  Expect(!out.chunks.empty() && first.header.offset == 0 &&
             (first.header.flags & 1) != 0 && first.header.name_len > 0,
         "the plain first chunk keeps offset 0, the first flag and the name");

  Expect(receiver.states.size() == 3, "the plain receive reports every chunk");
  Expect(!receiver.states.empty() && receiver.states.front().received == kChunk,
         "the plain receive reports the first chunk only");
  Expect(!receiver.states.empty() && receiver.states.back().status == ReceiveStatus::Completed,
         "the plain receive completes");
  Expect(sender.Snapshot().status == Status::Completed,
         "the plain send completes");
  Expect(sender.Snapshot().acknowledged == kTotal,
         "the plain send acknowledges the full length");

  const auto published = dest / name;
  Expect(fs::is_regular_file(published), "the plain transfer publishes the target name");
  Expect(ReadBytes(published) == Bytes(), "the plain published bytes equal the source file");
  Expect(Entries(dest) == 1, "the plain transfer publishes exactly one file");
}

}  // namespace

int main() {
  std::error_code ec;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  g_root = fs::temp_directory_path(ec) /
           ("crossdesk-transfer-resume-handshake-test-" + std::to_string(stamp));
  fs::create_directories(g_root, ec);
  if (ec) {
    std::printf("file transfer resume handshake checks failed: %s\n", ec.message().c_str());
    return 1;
  }

  const auto source_path = g_root / "source.bin";
  WriteBytes(source_path, Bytes());
  const auto source = Utf8(source_path);

  TestSenderResumeStart(source);
  TestSenderNothingToSend(source);
  TestReceiverResume(source);
  TestReceiverWithoutSidecar(source);
  TestReceiverMismatch(source);
  TestOffsetZeroRoundTrip(source);

  fs::remove_all(g_root, ec);
  if (g_failures) {
    std::printf("%d file transfer resume handshake checks failed\n", g_failures);
    return 1;
  }
  std::printf("file transfer resume handshake checks passed\n");
  return 0;
}
