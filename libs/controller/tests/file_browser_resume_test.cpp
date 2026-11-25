/*
 * Cross-session resume control plane checks.
 *
 * Drives the `resume` op through the same entry point the peer and GUI layers
 * use (FileBrowser::Handle plus the send callback) and pins down the whole
 * answer ladder of "how many bytes of this file do you already have":
 * no sidecar, a fresh record, a size mismatch, a stamp mismatch, a prefix that
 * already reaches the end, a malformed/foreign sidecar, a traversal name and a
 * sidecar living outside the receive directory. One smoke assertion keeps
 * temp_clear and resume from interfering.
 */

#include <file_browser.h>
#include <file_transfer_resume.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::FileBrowser;
namespace ops = crossdesk::file_browser_ops;
namespace resume = crossdesk::file_transfer;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
}

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

fs::path ScratchRoot(const std::string& tag) {
  std::error_code ec;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path(ec) /
                    ("crossdesk-file-browser-resume-" + tag + "-" + std::to_string(stamp));
  fs::create_directories(root, ec);
  return root;
}

void WriteText(const fs::path& path, const std::string& body) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(body.data(), static_cast<std::streamsize>(body.size()));
}

// A file the receive directory already holds, plus the stamp the sidecar layer
// compares against. The target itself has to exist: freshness is a (size,
// mtime) comparison against it, not a guess.
struct Target {
  fs::path path;
  std::string name;
  uint64_t size = 0;
  int64_t mtime = 0;
};

Target MakeTarget(const fs::path& directory, const std::string& name, size_t size) {
  Target target;
  target.path = directory / name;
  target.name = name;
  WriteText(target.path, std::string(size, 'x'));
  target.size = static_cast<uint64_t>(size);
  target.mtime = resume::TargetMtimeMs(target.path);
  return target;
}

void WriteSidecar(const fs::path& directory, const std::string& name, uint64_t total,
                  int64_t mtime, uint64_t received, uint32_t wire_id = 7) {
  resume::ResumeRecord record;
  record.target_path = name;
  record.total_size = total;
  record.target_mtime_ms = mtime;
  record.received = received;
  record.wire_id = wire_id;
  Expect(resume::WriteResumeRecord(directory, record, nullptr),
         "the test can write a sidecar record");
}

// A peer with no transport: the send callback records what the dispatcher
// answered and Call() waits for the matching id.
struct Peer {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<uint32_t, nlohmann::json> replies;
  uint32_t next_id = 1;
  uint32_t last_id = 0;
  FileBrowser browser;

  Peer()
      : browser(
            [this](const std::string&, const nlohmann::json& message) {
              {
                std::lock_guard lock(mutex);
                if (message.contains("id") && message["id"].is_number_unsigned())
                  replies[message["id"].get<uint32_t>()] = message;
              }
              changed.notify_all();
              return true;
            },
            [](const std::string&, const std::string&, uint32_t) { return true; },
            [](const std::string&, uint32_t, const fs::path&) { return true; },
            [](const std::string&, uint32_t) { return true; }) {}

  nlohmann::json Call(const std::string& op,
                      nlohmann::json fields = nlohmann::json::object()) {
    const auto id = next_id++;
    last_id = id;
    fields["type"] = "file_browser";
    fields["version"] = 1;
    fields["id"] = id;
    fields["op"] = op;
    browser.Handle("peer", std::move(fields));
    std::unique_lock lock(mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    if (!changed.wait_until(lock, deadline, [this, id] { return replies.count(id) != 0; }))
      return nlohmann::json::object();
    return replies[id];
  }

  // The resume op keeps the version 1 envelope and echoes the request id.
  void ExpectEnvelope(const nlohmann::json& reply, const char* what) {
    Expect(reply.value("op", std::string{}) == "reply", what);
    Expect(reply.value("id", 0u) == last_id, "the reply echoes the request id");
    Expect(reply.value("type", std::string{}) == "file_browser",
           "the reply keeps the message type");
    Expect(reply.value("version", 0) == 1, "the reply keeps the wire version");
  }
};

uint64_t Offset(const nlohmann::json& reply) {
  return reply.value("offset", static_cast<uint64_t>(INT64_MAX));
}

// ------------------------------------------------------------- op table --
void TestOpTable() {
  Expect(ops::IsOpName("resume"), "resume is in the dispatch table");
  Expect(ops::IsFileManagementOp("resume"), "resume is flagged as a file management op");
  Expect(ops::kOps.size() == 18, "the op table grew by exactly one entry");
  Expect(ops::kFileManagementOps.size() == 11, "the file management subset grew by one entry");
  std::size_t flagged = 0;
  for (const auto& info : ops::kOps) flagged += info.file_management ? 1 : 0;
  Expect(flagged == ops::kFileManagementOps.size(),
         "the table flags exactly the file management ops");
  // resume reuses the existing error vocabulary: no new code was invented.
  Expect(ops::kErrorCodes.size() == 20, "resume needs no new error code");
  Expect(ops::IsErrorCode("invalid_path") && ops::IsErrorCode("invalid_request"),
         "resume answers with codes from the shared vocabulary");
}

// --------------------------------------------------------- answer ladder --
void TestNoSidecar(Peer& peer) {
  auto reply = peer.Call("resume", {{"name", "absent.bin"}, {"total_size", 4096}});
  peer.ExpectEnvelope(reply, "resume answers with the reply envelope");
  Expect(reply.value("ok", false), "resume answers ok without a sidecar");
  Expect(Offset(reply) == 0, "no sidecar starts at 0");
  Expect(reply.value("total", static_cast<uint64_t>(0)) == 4096,
         "the reply echoes the requested total");
  Expect(reply.value("error", std::string{}) == "",
         "a resume answer is not an error");
}

void TestFreshRecord(Peer& peer, const fs::path& receive) {
  const auto target = MakeTarget(receive, "movie.bin", 4096);
  WriteSidecar(receive, "movie.bin", target.size, target.mtime, 1024);

  auto reply = peer.Call("resume", {{"name", "movie.bin"},
                                     {"total_size", target.size},
                                     {"mtime_ms", target.mtime}});
  Expect(reply.value("ok", false), "resume answers ok for a fresh record");
  Expect(Offset(reply) == 1024, "a fresh record with matching size and stamp resumes");
  Expect(reply.value("total", static_cast<uint64_t>(0)) == target.size,
         "the reply reports the record total");

  // mtime_ms is optional: omitting it means "do not check the stamp".
  reply = peer.Call("resume", {{"name", "movie.bin"}, {"total_size", target.size}});
  Expect(Offset(reply) == 1024, "an omitted mtime_ms does not block the resume");

  // 0 is the sidecar layer's "unknown stamp", so a request that supplies a real
  // stamp must not match a record written before the file was published.
  const auto unpublished = MakeTarget(receive, "unpublished.bin", 4096);
  WriteSidecar(receive, "unpublished.bin", unpublished.size, 0, 2048);
  reply = peer.Call("resume", {{"name", "unpublished.bin"},
                               {"total_size", unpublished.size},
                               {"mtime_ms", unpublished.mtime}});
  Expect(Offset(reply) == 0, "a record with an unknown stamp never matches a real stamp");

  // Answering 0 must never rewrite or remove the record: the caller decides.
  Expect(fs::exists(receive / "movie.bin.crossdesk-resume"),
         "a resume answer leaves the sidecar alone");
}

void TestSizeMismatch(Peer& peer, const fs::path& receive) {
  const auto target = MakeTarget(receive, "size.bin", 2048);
  WriteSidecar(receive, "size.bin", target.size, target.mtime, 512);

  auto reply = peer.Call("resume", {{"name", "size.bin"},
                                     {"total_size", target.size + 1},
                                     {"mtime_ms", target.mtime}});
  Expect(reply.value("ok", false), "a size mismatch still answers ok");
  Expect(Offset(reply) == 0, "a different total_size starts at 0");

  reply = peer.Call("resume", {{"name", "size.bin"},
                               {"total_size", target.size - 1},
                               {"mtime_ms", target.mtime}});
  Expect(Offset(reply) == 0, "a smaller total_size starts at 0 too");

  reply = peer.Call("resume", {{"name", "size.bin"},
                               {"total_size", target.size},
                               {"mtime_ms", target.mtime}});
  Expect(Offset(reply) == 512, "the same record still resumes at its own size");
}

void TestStampMismatch(Peer& peer, const fs::path& receive) {
  const auto target = MakeTarget(receive, "stamp.bin", 1024);
  WriteSidecar(receive, "stamp.bin", target.size, target.mtime, 256);

  auto reply = peer.Call("resume", {{"name", "stamp.bin"},
                                     {"total_size", target.size},
                                     {"mtime_ms", target.mtime + 1}});
  Expect(reply.value("ok", false), "a stamp mismatch still answers ok");
  Expect(Offset(reply) == 0, "a different mtime_ms starts at 0");

  reply = peer.Call("resume", {{"name", "stamp.bin"},
                               {"total_size", target.size},
                               {"mtime_ms", target.mtime}});
  Expect(Offset(reply) == 256, "the matching stamp still resumes");
}

void TestPrefixAtEnd(Peer& peer, const fs::path& receive) {
  const auto target = MakeTarget(receive, "complete.bin", 512);
  WriteSidecar(receive, "complete.bin", target.size, target.mtime, target.size);

  auto reply = peer.Call("resume", {{"name", "complete.bin"},
                                     {"total_size", target.size},
                                     {"mtime_ms", target.mtime}});
  Expect(reply.value("ok", false), "a complete prefix still answers ok");
  Expect(Offset(reply) == 0, "a prefix that already reaches the end starts at 0");

  // received > total_size is not a record this layer trusts at all.
  const auto target2 = MakeTarget(receive, "over.bin", 512);
  WriteText(receive / "over.bin.crossdesk-resume",
            "{\"version\":1,\"target_path\":\"over.bin\",\"total_size\":512,"
            "\"target_mtime_ms\":0,\"received\":999,\"wire_id\":3}");
  reply = peer.Call("resume", {{"name", "over.bin"},
                               {"total_size", target2.size},
                               {"mtime_ms", target2.mtime}});
  Expect(reply.value("ok", false), "an over-long prefix still answers ok");
  Expect(Offset(reply) == 0, "received > total_size starts at 0");
}

void TestBrokenSidecars(Peer& peer, const fs::path& receive) {
  const auto target = MakeTarget(receive, "bad.bin", 128);
  WriteText(receive / "bad.bin.crossdesk-resume", "{not json at all");
  auto reply = peer.Call("resume", {{"name", "bad.bin"},
                                     {"total_size", target.size},
                                     {"mtime_ms", target.mtime}});
  Expect(reply.value("ok", false), "a malformed sidecar still answers ok");
  Expect(Offset(reply) == 0, "a malformed sidecar starts at 0");

  // A truncated record, a foreign version and an empty file must all be the
  // same answer instead of an exception on the dispatcher thread.
  WriteText(receive / "short.bin.crossdesk-resume", "{\"version\":1,\"target_pa");
  reply = peer.Call("resume", {{"name", "short.bin"}, {"total_size", 128}});
  Expect(reply.value("ok", false) && Offset(reply) == 0,
         "a truncated sidecar starts at 0");

  WriteText(receive / "future.bin.crossdesk-resume",
            "{\"version\":2,\"target_path\":\"future.bin\",\"total_size\":128,"
            "\"target_mtime_ms\":0,\"received\":64,\"wire_id\":1}");
  reply = peer.Call("resume", {{"name", "future.bin"}, {"total_size", 128}});
  Expect(reply.value("ok", false) && Offset(reply) == 0,
         "an unsupported sidecar version starts at 0");

  WriteText(receive / "empty.bin.crossdesk-resume", "");
  reply = peer.Call("resume", {{"name", "empty.bin"}, {"total_size", 128}});
  Expect(reply.value("ok", false) && Offset(reply) == 0,
         "an empty sidecar starts at 0");

  // A record whose target vanished is "missing": nothing to resume from.
  WriteSidecar(receive, "gone.bin", 4096, 1234, 1024);
  reply = peer.Call("resume", {{"name", "gone.bin"}, {"total_size", 4096}});
  Expect(reply.value("ok", false) && Offset(reply) == 0,
         "a record whose target is gone starts at 0");
}

void TestNameGuard(Peer& peer, const fs::path& receive) {
  const auto target = MakeTarget(receive, "guarded.bin", 256);
  WriteSidecar(receive, "guarded.bin", target.size, target.mtime, 128);

  // A traversal name never reaches the sidecar layer: the shared path guard
  // answers invalid_path and nothing outside the receive root is touched.
  const auto outside = ScratchRoot("name-outside");
  const auto victim = MakeTarget(outside, "guarded.bin", 256);
  WriteSidecar(outside, "guarded.bin", victim.size, victim.mtime, 128);

  auto reply = peer.Call("resume", {{"name", "../guarded.bin"}, {"total_size", 256}});
  peer.ExpectEnvelope(reply, "a traversal name still answers with the envelope");
  Expect(!reply.value("ok", true), "a traversal name is refused");
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "a traversal name answers invalid_path");

  reply = peer.Call("resume", {{"name", "../name-outside/guarded.bin"},
                               {"total_size", 256}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "a nested traversal name answers invalid_path");

  reply = peer.Call("resume", {{"name", Utf8(outside / "guarded.bin")},
                               {"total_size", 256}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "an absolute name answers invalid_path");

  reply = peer.Call("resume", {{"name", "."}, {"total_size", 256}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "a bare dot is not a file name");
  reply = peer.Call("resume", {{"name", ".."}, {"total_size", 256}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "a bare dot dot is not a file name");

  // The refused names changed nothing: the outside record and its target are
  // exactly as they were, and the inside record still answers.
  Expect(fs::exists(outside / "guarded.bin.crossdesk-resume"),
         "a refused name leaves the outside sidecar alone");
  Expect(fs::file_size(victim.path) == 256, "a refused name leaves the outside file alone");
  Expect(resume::TargetMtimeMs(victim.path) == victim.mtime,
         "a refused name does not restamp the outside file");
  reply = peer.Call("resume", {{"name", "guarded.bin"},
                               {"total_size", target.size},
                               {"mtime_ms", target.mtime}});
  Expect(Offset(reply) == 128, "the inside record is unaffected");

  std::error_code cleanup;
  fs::remove_all(outside, cleanup);
}

void TestOutsidePath(Peer& peer, const fs::path& receive) {
  // A perfect record that lives outside the receive directory answers 0: the op
  // never reports anything about paths it was not scoped to.
  const auto outside = ScratchRoot("outside-path");
  const auto target = MakeTarget(outside, "elsewhere.bin", 256);
  WriteSidecar(outside, "elsewhere.bin", target.size, target.mtime, 128);

  auto reply = peer.Call("resume", {{"name", "elsewhere.bin"},
                                     {"total_size", target.size},
                                     {"mtime_ms", target.mtime},
                                     {"path", Utf8(outside)}});
  peer.ExpectEnvelope(reply, "an outside path answers with the reply envelope");
  Expect(reply.value("ok", false), "an outside path still answers ok");
  Expect(Offset(reply) == 0, "a sidecar outside the receive directory starts at 0");

  // Without a path the search stays in the receive root, so the outside record
  // is invisible even with an otherwise perfect match.
  reply = peer.Call("resume", {{"name", "elsewhere.bin"},
                               {"total_size", target.size},
                               {"mtime_ms", target.mtime}});
  Expect(Offset(reply) == 0, "an outside sidecar is invisible from the receive root");

  // A path that does not resolve is "nothing to resume", not an error.
  reply = peer.Call("resume", {{"name", "elsewhere.bin"},
                               {"total_size", target.size},
                               {"path", Utf8(outside / "missing")}});
  Expect(reply.value("ok", false) && Offset(reply) == 0,
         "a path that does not resolve answers 0");

  // A subdirectory of the receive root is inside the allowed scope.
  const auto nested = receive / "nested";
  fs::create_directories(nested);
  const auto nested_target = MakeTarget(nested, "inner.bin", 512);
  WriteSidecar(nested, "inner.bin", nested_target.size, nested_target.mtime, 256);
  reply = peer.Call("resume", {{"name", "inner.bin"},
                               {"total_size", nested_target.size},
                               {"mtime_ms", nested_target.mtime},
                               {"path", Utf8(nested)}});
  Expect(Offset(reply) == 256, "a directory inside the receive root may answer");

  std::error_code cleanup;
  fs::remove_all(outside, cleanup);
}

void TestInvalidRequests(Peer& peer) {
  auto reply = peer.Call("resume", nlohmann::json::object());
  peer.ExpectEnvelope(reply, "a request without fields answers with the envelope");
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "resume needs a name and a total_size");

  reply = peer.Call("resume", {{"name", "x.bin"}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "resume needs a total_size");

  reply = peer.Call("resume", {{"name", "x.bin"}, {"total_size", "4096"}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a string total_size is not a size");

  reply = peer.Call("resume", {{"name", "x.bin"}, {"total_size", -1}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a negative total_size is not a size");

  reply = peer.Call("resume", {{"name", 5}, {"total_size", 4096}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a non-string name is not a name");

  reply = peer.Call("resume", {{"name", "x.bin"}, {"total_size", 4096},
                               {"mtime_ms", "yesterday"}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a non-numeric mtime_ms is not a stamp");
}

// temp_clear and resume share the receive root: one must not disturb the other.
void TestTempClearAlongside(Peer& peer, const fs::path& receive) {
  fs::create_directories(receive / ".crossdesk-1-2-3");
  WriteText(receive / ".crossdesk-1-2-3" / ".partial", "half");

  auto reply = peer.Call("temp_clear", {{"directory", Utf8(receive)}});
  Expect(reply.value("ok", false), "temp_clear still sweeps the receive directory");
  Expect(reply.value("removed_count", 0u) == 1u, "temp_clear removed the staging directory");
  Expect(!fs::exists(receive / ".crossdesk-1-2-3"), "the staging directory is gone");

  const auto size = static_cast<uint64_t>(fs::file_size(receive / "movie.bin"));
  reply = peer.Call("resume", {{"name", "movie.bin"},
                               {"total_size", size},
                               {"mtime_ms", resume::TargetMtimeMs(receive / "movie.bin")}});
  Expect(reply.value("ok", false), "resume still answers after temp_clear");
  Expect(Offset(reply) == 1024, "temp_clear does not disturb the resume answer");

  // An unknown op is still unsupported: resume did not widen the dispatcher.
  reply = peer.Call("resume_all", {{"name", "movie.bin"}, {"total_size", size}});
  Expect(reply.value("error", std::string{}) == "unsupported",
         "an unknown op next to resume answers unsupported");
}

}  // namespace

int main() {
  const auto root = ScratchRoot("root");
  const auto receive = root / "receive";
  std::error_code ec;
  fs::create_directories(receive, ec);
  Expect(fs::is_directory(receive), "the scratch receive directory exists");

  TestOpTable();
  {
    Peer peer;
    peer.browser.SetReceiveDirectory(Utf8(receive));
    Expect(peer.browser.ReceiveDirectory() == Utf8(receive),
           "the configured receive directory is reported");

    TestNoSidecar(peer);
    TestFreshRecord(peer, receive);
    TestSizeMismatch(peer, receive);
    TestStampMismatch(peer, receive);
    TestPrefixAtEnd(peer, receive);
    TestBrokenSidecars(peer, receive);
    TestNameGuard(peer, receive);
    TestOutsidePath(peer, receive);
    TestInvalidRequests(peer);
    TestTempClearAlongside(peer, receive);

    // Unconfigured (no receive root) means "nothing to resume", never an error.
    peer.browser.SetReceiveDirectory("");
    const auto reply = peer.Call("resume", {{"name", "movie.bin"}, {"total_size", 4096}});
    Expect(reply.value("ok", false), "resume answers ok without a receive root");
    Expect(Offset(reply) == 0, "without a receive root the answer is 0");
    peer.browser.SetReceiveDirectory(Utf8(receive));
  }

  fs::remove_all(root, ec);
  if (g_failures) {
    std::printf("%d file browser resume checks failed\n", g_failures);
    return 1;
  }
  std::printf("file browser resume checks passed\n");
  return 0;
}
