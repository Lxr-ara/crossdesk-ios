/*
 * 收端覆盖冲突回归测试。
 *
 * 被测的是桌面端接收适配器的那条提问路径：引擎发布一个已经存在的名字时会给它
 * 加时间戳后缀，kAskSkip 必须把「被占用的目标 + 文件大小」问出来，并按回答把
 * 文件落到用户真正想要的位置。这里用 wire 的编码器直接喂一条单块传输，
 * 不建 peer、不起线程、不开窗口：resolver 就是一个记录调用并立刻回答的 lambda。
 */

#include "file_transfer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::ConflictChoice;
using crossdesk::EncodeFileChunk;
using crossdesk::FileReceiver;
using crossdesk::TransferOverwritePolicy;

namespace {

int g_failures = 0;
fs::path g_root;

void Expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
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

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::size_t FileCount(const fs::path& dir) {
  std::error_code ec;
  std::size_t count = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_error;
    if (it->is_regular_file(entry_error)) ++count;
  }
  return count;
}

// 一次单块接收：把整份 payload 当一个 first+last 的块喂给 receiver，这会走完
// 暂存 -> 发布 -> 进度回调 -> 冲突询问的整条路径。
void Deliver(FileReceiver& receiver, uint32_t wire_id, const std::string& name,
             const std::string& payload) {
  const auto total = static_cast<uint64_t>(payload.size());
  const auto packet = EncodeFileChunk(wire_id, 0, total, payload.data(),
                                      static_cast<uint32_t>(payload.size()), &name,
                                      true, true);
  Expect(!packet.empty(), "the wire encoder produces a chunk");
  receiver.OnData(packet.data(), packet.size());
}

void Setup(FileReceiver& receiver, const fs::path& dir) {
  receiver.SetOutputDir(dir);
  // 侧车目录与开关保持默认即可：这里只关心发布与覆盖，不关心续传记录。
  receiver.SetResumeEnabled(false);
}

// kAskSkip + 装了钩子 = 必须问，而且答案要落到磁盘上。
void TestAskOverwrite() {
  const auto dir = FreshDir("ask-overwrite");
  WriteText(dir / "report.txt", "old-content");
  FileReceiver receiver;
  Setup(receiver, dir);

  std::string asked_destination;
  uint64_t asked_size = 0;
  int calls = 0;
  std::string published_path;
  receiver.SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  receiver.SetConflictResolver([&](const std::string& destination, uint64_t size) {
    ++calls;
    asked_destination = destination;
    asked_size = size;
    return ConflictChoice::kOverwrite;
  });
  receiver.SetOnProgress([&](const std::string&, double, bool sending,
                             const std::string& path) {
    if (!sending) published_path = path;
  });

  Deliver(receiver, 1001, "report.txt", "new-content");
  Expect(calls == 1, "kAskSkip asks exactly once per conflicting file");
  Expect(asked_destination == Utf8(dir / "report.txt"),
         "the question names the destination the peer asked for");
  Expect(asked_destination.find("report.txt") != std::string::npos,
         "the asked destination ends with the requested name");
  Expect(asked_size == std::string("new-content").size(),
         "the question carries the size of the file that landed");
  Expect(ReadText(dir / "report.txt") == "new-content",
         "overwrite puts the new bytes under the requested name");
  Expect(FileCount(dir) == 1, "overwrite leaves no timestamped copy behind");
}

void TestAskRename() {
  const auto dir = FreshDir("ask-rename");
  WriteText(dir / "report.txt", "old-content");
  FileReceiver receiver;
  Setup(receiver, dir);
  int calls = 0;
  receiver.SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  receiver.SetConflictResolver([&](const std::string&, uint64_t) {
    ++calls;
    return ConflictChoice::kRename;
  });
  Deliver(receiver, 1002, "report.txt", "new-content");
  Expect(calls == 1, "rename asks once");
  Expect(ReadText(dir / "report.txt") == "old-content",
         "rename leaves the existing file alone");
  Expect(ReadText(dir / "report (1).txt") == "new-content",
         "rename lands the transfer under \"report (1).txt\"");
}

// 跳过 = 保留引擎的时间戳名（本次改动之前 kAskSkip 的行为），并如实报告路径。
void TestAskSkip() {
  const auto dir = FreshDir("ask-skip");
  WriteText(dir / "report.txt", "old-content");
  FileReceiver receiver;
  Setup(receiver, dir);
  int calls = 0;
  std::string published_path;
  receiver.SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  receiver.SetConflictResolver([&](const std::string&, uint64_t) {
    ++calls;
    return ConflictChoice::kSkip;
  });
  receiver.SetOnProgress([&](const std::string& name, double, bool sending,
                             const std::string& path) {
    if (sending || name != "report.txt" || path.empty()) return;
    published_path = path;
  });
  Deliver(receiver, 1003, "report.txt", "new-content");
  Expect(calls == 1, "skip asks once");
  Expect(ReadText(dir / "report.txt") == "old-content",
         "skip keeps the file that was already there");
  Expect(FileCount(dir) == 2, "skip keeps the engine's timestamped copy");
  Expect(published_path != Utf8(dir / "report.txt"),
         "the progress callback reports the timestamped path, not the requested one");
}

// 「全部」只是同一个动作的批次级变体：接收端对单个文件的表现必须完全一致。
void TestAllVariantsBehaveLikeTheirBase() {
  const auto dir = FreshDir("all-variants");
  WriteText(dir / "cover.png", "old");
  FileReceiver receiver;
  Setup(receiver, dir);
  receiver.SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  receiver.SetConflictResolver([](const std::string&, uint64_t) {
    return ConflictChoice::kOverwriteAll;
  });
  Deliver(receiver, 1004, "cover.png", "new");
  Expect(ReadText(dir / "cover.png") == "new", "kOverwriteAll overwrites");

  WriteText(dir / "track.flac", "old");
  receiver.SetConflictResolver([](const std::string&, uint64_t) {
    return ConflictChoice::kRenameAll;
  });
  Deliver(receiver, 1005, "track.flac", "new");
  Expect(ReadText(dir / "track.flac") == "old", "kRenameAll keeps the original");
  Expect(ReadText(dir / "track (1).flac") == "new", "kRenameAll renames the new file");

  WriteText(dir / "notes.md", "old");
  receiver.SetConflictResolver([](const std::string&, uint64_t) {
    return ConflictChoice::kSkipAll;
  });
  Deliver(receiver, 1006, "notes.md", "new");
  Expect(ReadText(dir / "notes.md") == "old", "kSkipAll keeps the original");
}

// 没装钩子：行为和这次改动之前逐字一致——不阻塞、不崩，留下时间戳名。
void TestNoResolverKeepsTodaysBehaviour() {
  const auto dir = FreshDir("no-resolver");
  WriteText(dir / "report.txt", "old-content");
  FileReceiver receiver;
  Setup(receiver, dir);
  receiver.SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  Deliver(receiver, 1007, "report.txt", "new-content");
  Expect(ReadText(dir / "report.txt") == "old-content",
         "without a resolver the existing file is untouched");
  Expect(FileCount(dir) == 2, "without a resolver the timestamped copy stays");
}

// 另外两个策略保持原样：钩子装了也不会被调用。
void TestOtherPoliciesNeverAsk() {
  const auto rename_dir = FreshDir("policy-rename");
  WriteText(rename_dir / "report.txt", "old");
  FileReceiver renaming;
  Setup(renaming, rename_dir);
  int calls = 0;
  renaming.SetOverwritePolicy(TransferOverwritePolicy::kRename);
  renaming.SetConflictResolver([&](const std::string&, uint64_t) {
    ++calls;
    return ConflictChoice::kSkip;
  });
  Deliver(renaming, 1008, "report.txt", "new");
  Expect(calls == 0, "kRename never asks");
  Expect(ReadText(rename_dir / "report (1).txt") == "new",
         "kRename keeps its own naming rule");

  const auto overwrite_dir = FreshDir("policy-overwrite");
  WriteText(overwrite_dir / "report.txt", "old");
  FileReceiver overwriting;
  Setup(overwriting, overwrite_dir);
  overwriting.SetOverwritePolicy(TransferOverwritePolicy::kOverwrite);
  overwriting.SetConflictResolver([&](const std::string&, uint64_t) {
    ++calls;
    return ConflictChoice::kSkip;
  });
  Deliver(overwriting, 1009, "report.txt", "new");
  Expect(calls == 0, "kOverwrite never asks");
  Expect(ReadText(overwrite_dir / "report.txt") == "new", "kOverwrite still overwrites");
}

// 没有冲突就不该问：目标名字空着的时候一次钩子都不许调。
void TestNoConflictDoesNotAsk() {
  const auto dir = FreshDir("no-conflict");
  FileReceiver receiver;
  Setup(receiver, dir);
  int calls = 0;
  receiver.SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  receiver.SetConflictResolver([&](const std::string&, uint64_t) {
    ++calls;
    return ConflictChoice::kOverwrite;
  });
  Deliver(receiver, 1010, "fresh.txt", "content");
  Expect(calls == 0, "a free destination asks nothing");
  Expect(ReadText(dir / "fresh.txt") == "content", "the file lands under its own name");
}

}  // namespace

int main() {
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / "crossdesk_overwrite_conflict_test";
  if (ec) {
    std::printf("FAIL  temp directory unavailable\n");
    return 1;
  }
  fs::remove_all(g_root, ec);
  fs::create_directories(g_root, ec);

  TestAskOverwrite();
  TestAskRename();
  TestAskSkip();
  TestAllVariantsBehaveLikeTheirBase();
  TestNoResolverKeepsTodaysBehaviour();
  TestOtherPoliciesNeverAsk();
  TestNoConflictDoesNotAsk();

  fs::remove_all(g_root, ec);
  if (g_failures == 0) {
    std::printf("overwrite_conflict_test: all checks passed\n");
    return 0;
  }
  std::printf("overwrite_conflict_test: %d check(s) failed\n", g_failures);
  return 1;
}
