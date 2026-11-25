/*
 * Resume-enabled receive regression test, end to end over the real receiver.
 *
 * The product default is resume ENABLED: FileReceiver constructs its engine
 * with {resume directory = receive directory, enabled = true}. That means the
 * resume layer keeps a durable prefix copy of the bytes it has already accepted
 * inside the receive directory, and the engine publishes a finished transfer
 * with a hard link that never replaces an existing name. A harness driving
 * FileReceiver with the default settings showed the consequence: the prefix copy
 * occupied the name the peer asked for, so the engine published a timestamped
 * duplicate, and the resume cleanup then removed the file that had been
 * published. The fix makes the cleanup remove only the prefix copy it actually
 * wrote, so the requested name survives.
 *
 * Every case asserts on the filesystem only: entry names, sizes and bytes. The
 * wire frames come from the shared encoders, the same way
 * overwrite_conflict_test does it, and no peer, thread or window is created.
 *
 * What this test pins down on the current tree: the published file is no longer
 * deleted by the completion cleanup (case 1), and the prefix copy no longer
 * overwrites a file that already owns the name (cases 3 and 5). What it still
 * reports as PRODUCT BUG is that a free destination name with a resume prefix
 * keeps being published under the engine's timestamped name instead of the name
 * the peer asked for (cases 2 and 6); each of those assertions carries the exact
 * product lines and the observed values.
 */

#include "file_transfer.h"

#include <file_transfer_resume.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::ConflictChoice;
using crossdesk::EncodeFileChunk;
using crossdesk::FileReceiver;
using crossdesk::TransferOverwritePolicy;
using crossdesk::kFileChunkSize;

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

void WriteBytes(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

// Offset dependent bytes: a misplaced, truncated or duplicated chunk can never
// compare equal to the expected payload.
std::string Payload(uint64_t size) {
  std::string data(static_cast<std::size_t>(size), '\0');
  for (std::size_t index = 0; index < data.size(); ++index)
    data[index] = static_cast<char>((index * 37 + 11) & 0xFF);
  return data;
}

std::vector<std::string> EntryNames(const fs::path& dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    names.push_back(Utf8(it->path().filename()));
  std::sort(names.begin(), names.end());
  return names;
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

uint64_t FileSize(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

void PrintDirectory(const fs::path& dir) {
  for (const auto& name : EntryNames(dir)) {
    const auto path = dir / name;
    std::error_code ec;
    if (fs::is_regular_file(path, ec))
      std::printf("      [file] %s (%llu bytes)\n", name.c_str(),
                  static_cast<unsigned long long>(FileSize(path)));
    else
      std::printf("      [dir ] %s\n", name.c_str());
  }
}

// Case 7, shared by every case: a finished transfer leaves no engine staging
// entry (".crossdesk-*", which also covers ".crossdesk-prefix-<name>") and no
// resume sidecar or half written sidecar for the name it received.
void ExpectNoLeftovers(const fs::path& dir, const std::string& name, const char* what) {
  bool leftovers = false;
  const std::string suffix(crossdesk::file_transfer::kResumeSidecarSuffix);
  const std::string temp_suffix = suffix + ".tmp";
  for (const auto& entry : EntryNames(dir)) {
    const bool staging = entry.compare(0, 11, ".crossdesk-") == 0;
    const bool sidecar = entry == name + suffix;
    const bool temp = entry == name + temp_suffix;
    if (staging || sidecar || temp) leftovers = true;
  }
  if (leftovers) {
    std::printf("      leftovers under %s:\n", Utf8(dir).c_str());
    PrintDirectory(dir);
  }
  Expect(!leftovers, what);
  Expect(!fs::exists(dir / (name + suffix)), "no resume sidecar survives the transfer");
  Expect(!fs::exists(dir / (name + temp_suffix)), "no half written sidecar survives");
}

// One chunk of this transfer. `first` is the session's first chunk flag, which
// the sender derives from "sent == start" and therefore also sets on the first
// chunk of a resumed session; only that chunk carries the name. The last flag is
// derived exactly like the sender derives it.
void DeliverChunk(FileReceiver& receiver, uint32_t wire_id, const std::string& name,
                  const std::string& data, uint64_t offset, uint64_t size,
                  bool first) {
  const auto total = static_cast<uint64_t>(data.size());
  const bool last = offset + size == total;
  const auto packet = EncodeFileChunk(
      wire_id, offset, total, data.data() + offset, static_cast<uint32_t>(size),
      first ? &name : nullptr, first, last);
  Expect(!packet.empty(), "the wire encoder produces a chunk");
  if (packet.empty()) return;
  receiver.OnData(packet.data(), packet.size());
}

// Whole transfer in kFileChunkSize pieces, starting at `start`. A resume sends
// the remaining chunks of the same file, with the first of them flagged first.
void DeliverAll(FileReceiver& receiver, uint32_t wire_id, const std::string& name,
                const std::string& data, uint64_t chunk = kFileChunkSize,
                uint64_t start = 0) {
  bool first = true;
  for (uint64_t offset = start; offset < static_cast<uint64_t>(data.size());) {
    const auto size = (std::min)(chunk, static_cast<uint64_t>(data.size()) - offset);
    DeliverChunk(receiver, wire_id, name, data, offset, size, first);
    first = false;
    offset += size;
  }
}

// Default construction: the receive directory is also the resume directory and
// resume is on, which is what the product ships.
std::unique_ptr<FileReceiver> MakeReceiver(const fs::path& dir) {
  auto receiver = std::make_unique<FileReceiver>(dir);
  receiver->SetResumeEnabled(true);
  return receiver;
}

// Case 1: one chunk, free destination name, resume on. The resume layer never
// had to write a prefix (the transfer completes on its first chunk), and the
// published file must survive the completion cleanup instead of being deleted.
void TestSingleChunkFreeName() {
  const int before = g_failures;
  const auto dir = FreshDir("single-chunk-free-name");
  const std::string name = "report.txt";
  const std::string bytes = "resume-enabled-single-chunk";
  auto receiver = MakeReceiver(dir);

  DeliverAll(*receiver, 2001, name, bytes);
  receiver.reset();

  Expect(FileSize(dir / name) == bytes.size(),
         "single chunk: the requested name holds the payload length");
  Expect(ReadBytes(dir / name) == bytes,
         "single chunk: the requested name holds the exact payload");
  Expect(FileCount(dir) == 1, "single chunk: exactly one file is published");
  Expect(EntryNames(dir) == std::vector<std::string>({name}),
         "single chunk: the only entry is the requested name");
  ExpectNoLeftovers(dir, name, "single chunk: nothing from the resume layer is left behind");

  if (g_failures != before) {
    std::printf("    case 1 directory %s:\n", Utf8(dir).c_str());
    PrintDirectory(dir);
  }
}

// Case 2: three full chunks, free destination name, resume on. The resume layer
// writes a prefix copy after chunk one, so the engine finds the requested name
// "taken"; the transfer must still land under the REQUESTED name, not
// "report_<stamp>.bin".
void TestMultiChunkFreeName() {
  const int before = g_failures;
  const auto dir = FreshDir("multi-chunk-free-name");
  const std::string name = "report.bin";
  const std::string bytes = Payload(3 * kFileChunkSize);
  auto receiver = MakeReceiver(dir);

  DeliverAll(*receiver, 2002, name, bytes);
  receiver.reset();

  // PRODUCT BUG: the next three checks fail on the current tree, and their
  // cause is one chain in the product, not in this test.
  //   * apps/desktop/src/tools/file_transfer.cpp:500-508 - with a free
  //     destination name, PersistResumePrefix writes the durable prefix copy at
  //     the requested name itself ("<receive dir>/report.bin", 65536 bytes).
  //   * libs/controller/include/file_transfer_core.h:526-548 - Publish() hard
  //     links to that name, gets file_exists and falls back to
  //     "<stem>_<stamp><ext>" (line 540), so the engine publishes
  //     "report_1795257113720.bin" instead of "report.bin".
  //   * apps/desktop/src/tools/file_transfer.cpp:376-388 - the prefix removal
  //     that was just added runs in ResolveConflict(), i.e. after the engine has
  //     already published, and DropResumeRecord() (file_transfer.cpp:562-585,
  //     reached from the engine's Completed notification at
  //     file_transfer_core.h:518, which happens before OnData returns at
  //     file_transfer.cpp:322) has already erased that same map entry and
  //     deleted the prefix. The loop therefore matches nothing, and with the
  //     default kAskSkip policy and no resolver ResolveConflict returns the
  //     timestamped path unchanged.
  // Observed: exactly one file, "report_<stamp>.bin" (196608 bytes); the
  // requested "report.bin" does not exist at all.
  // Expected: "report.bin" with 196608 bytes and nothing else.
  Expect(FileSize(dir / name) == bytes.size(),
         "multi chunk: the requested name holds the full length");
  Expect(ReadBytes(dir / name) == bytes,
         "multi chunk: the requested name holds the full payload");
  Expect(FileCount(dir) == 1,
         "multi chunk: no timestamped duplicate is published next to it");
  Expect(EntryNames(dir) == std::vector<std::string>({name}),
         "multi chunk: the only entry is the requested name");
  ExpectNoLeftovers(dir, name, "multi chunk: the resume prefix does not survive");

  if (g_failures != before) {
    std::printf("    case 2 directory %s:\n", Utf8(dir).c_str());
    PrintDirectory(dir);
  }
}

// Cases 3 to 5 share one setup: a file already owns the requested name while
// resume is on, the policy is kAskSkip and a resolver answers. The pre-existing
// file is the user's data and must never be truncated by the resume prefix copy.
struct ConflictOutcome {
  fs::path dir;
  std::string name;
  std::string original;
  std::string bytes;
  int asks = 0;
  std::vector<std::string> others;
};

ConflictOutcome RunConflictCase(const char* fixture, ConflictChoice choice,
                                uint32_t wire_id) {
  ConflictOutcome out;
  out.dir = FreshDir(fixture);
  out.name = "report.bin";
  out.original = "original-bytes-that-must-survive";
  out.bytes = Payload(2 * kFileChunkSize);
  WriteBytes(out.dir / out.name, out.original);

  auto receiver = MakeReceiver(out.dir);
  receiver->SetOverwritePolicy(TransferOverwritePolicy::kAskSkip);
  receiver->SetConflictResolver([&out, choice](const std::string&, uint64_t) {
    ++out.asks;
    return choice;
  });
  DeliverAll(*receiver, wire_id, out.name, out.bytes);
  receiver.reset();

  for (const auto& entry : EntryNames(out.dir)) {
    if (entry == out.name) continue;
    out.others.push_back(entry);
  }
  return out;
}

// what the conflict dialog was asked for and how the filesystem answered.
void ExpectOneOtherFile(const ConflictOutcome& out, const char* what) {
  if (out.others.size() != 1) {
    std::printf("      expected exactly one other entry, got %zu under %s:\n",
                out.others.size(), Utf8(out.dir).c_str());
    PrintDirectory(out.dir);
  }
  Expect(out.others.size() == 1, what);
  if (out.others.size() != 1) return;
  Expect(ReadBytes(out.dir / out.others.front()) == out.bytes,
         "the other entry holds the new payload");
}

// Case 3: kSkip keeps the user's file and lands the new bytes elsewhere.
void TestAskSkipKeepsOriginal() {
  const ConflictOutcome out =
      RunConflictCase("ask-skip-keeps-original", ConflictChoice::kSkip, 2003);
  Expect(out.asks == 1, "skip: the conflict is asked exactly once");
  Expect(ReadBytes(out.dir / out.name) == out.original,
         "skip: the pre-existing file keeps its original bytes");
  Expect(out.asks == 1, "skip: the conflict is asked exactly once");
  ExpectOneOtherFile(out, "skip: the new bytes land under one different name");
  Expect(FileCount(out.dir) == 2, "skip: the receive directory holds both files");
  ExpectNoLeftovers(out.dir, out.name, "skip: nothing from the resume layer is left behind");
}

// Case 4: kOverwrite replaces the user's file and leaves no extra copy.
void TestAskOverwriteReplaces() {
  ConflictOutcome out = RunConflictCase("ask-overwrite-replaces", ConflictChoice::kOverwrite, 2004);
  Expect(out.asks == 1, "overwrite: the conflict is asked exactly once");
  Expect(ReadBytes(out.dir / out.name) == out.bytes,
         "overwrite: the destination holds the new bytes");
  Expect(FileCount(out.dir) == 1, "overwrite: no stray copy remains");
  Expect(out.others.empty(), "overwrite: nothing else is published");
  ExpectNoLeftovers(out.dir, out.name, "overwrite: nothing from the resume layer is left behind");
}

// Case 5: kRename keeps the user's file and lands the new bytes next to it.
void TestAskRenameKeepsBoth() {
  ConflictOutcome out = RunConflictCase("ask-rename-keeps-both", ConflictChoice::kRename, 2005);
  Expect(out.asks == 1, "rename: the conflict is asked exactly once");
  Expect(ReadBytes(out.dir / out.name) == out.original,
         "rename: the original file is untouched");
  ExpectOneOtherFile(out, "rename: the new bytes land under one different name");
  Expect(FileCount(out.dir) == 2, "rename: the receive directory holds both files");
  ExpectNoLeftovers(out.dir, out.name, "rename: nothing from the resume layer is left behind");
}

// Case 6: only the first chunk arrives, so the transfer is interrupted at the
// engine level. What must survive is the durable state a later process needs: a
// sidecar that parses and a prefix that holds exactly the accepted bytes, both
// under names that never collide with the user's files. Completing the resumed
// session across a receiver restart is a separate gap (documented in
// docs/LAYER_STATUS.md); this case locks the durable state itself.
void TestInterruptedKeepsDurableState() {
  const int before = g_failures;
  const auto dir = FreshDir("interrupted-then-resumed");
  const std::string name = "video.bin";
  const std::string bytes = Payload(3 * kFileChunkSize);
  const std::string sidecar = name + std::string(crossdesk::file_transfer::kResumeSidecarSuffix);

  uint64_t recorded = 0;
  {
    auto receiver = MakeReceiver(dir);
    DeliverChunk(*receiver, 2006, name, bytes, 0, kFileChunkSize, true);
  }

  // The durable prefix lives under a side name, never on the requested one: a
  // prefix written at the final name would make the engine publish the finished
  // file as "name_<stamp>.ext" and would truncate a file the user already had.
  const auto prefix = dir / (std::string(".crossdesk-prefix-") + name);
  Expect(fs::is_regular_file(dir / sidecar),
         "interrupted: a resume sidecar records the accepted prefix");
  Expect(fs::is_regular_file(prefix),
         "interrupted: the accepted prefix exists under its side name");
  Expect(FileSize(prefix) == kFileChunkSize,
         "interrupted: the prefix holds exactly the first chunk");
  Expect(!fs::exists(dir / name),
         "interrupted: the requested name is still free while the prefix is durable");

  std::string error;
  const auto record =
      crossdesk::file_transfer::ReadResumeRecordUnchecked(dir, name, &error);
  Expect(record.has_value(), "interrupted: the sidecar parses");
  if (record) {
    recorded = record->received;
    Expect(record->total_size == bytes.size(), "interrupted: the record keeps the total length");
    Expect(record->received == kFileChunkSize, "interrupted: the record keeps the prefix length");
  }
  Expect(recorded == kFileChunkSize, "interrupted: the offset to resume from is the prefix length");

  if (g_failures != before) {
    std::printf("    case 6 directory %s:\n", Utf8(dir).c_str());
    PrintDirectory(dir);
  }
}

}  // namespace

int main() {
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / "crossdesk_resume_receive_e2e_test";
  if (ec) {
    std::printf("FAIL  temp directory unavailable\n");
    return 1;
  }
  fs::remove_all(g_root, ec);
  fs::create_directories(g_root, ec);

  TestSingleChunkFreeName();
  TestMultiChunkFreeName();
  TestAskSkipKeepsOriginal();
  TestAskOverwriteReplaces();
  TestAskRenameKeepsBoth();
  TestInterruptedKeepsDurableState();

  fs::remove_all(g_root, ec);
  if (g_failures == 0) {
    std::printf("resume_receive_e2e_test: all checks passed\n");
    return 0;
  }
  std::printf("resume_receive_e2e_test: %d check(s) failed\n", g_failures);
  return 1;
}
