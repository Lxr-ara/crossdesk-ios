/*
 * Remote file management op checks.
 *
 * Drives the seven new file_browser ops through the entry point the peer and
 * GUI layers use (FileBrowser::Handle plus the send callback) and checks the
 * shared op-name table, the closed error vocabulary and the path-safety rules on
 * their own, so a regression is caught without a live connection.
 */

#include <file_browser.h>

#include <chrono>
#include <condition_variable>
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
                    ("crossdesk-file-browser-ops-" + tag + "-" + std::to_string(stamp));
  fs::create_directories(root, ec);
  return root;
}

void WriteFile(const fs::path& path, const std::string& body) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(body.data(), static_cast<std::streamsize>(body.size()));
}

// A peer with no transport. The send callback records what the dispatcher
// answered and Call() waits for the matching id, which is exactly how the GUI and
// peer layers consume replies.
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

  // A reply must keep the version 1 envelope and echo the request id.
  void ExpectEnvelope(const nlohmann::json& reply, const char* what) {
    Expect(reply.value("op", std::string{}) == "reply", what);
    Expect(reply.value("id", 0u) == last_id, "the reply echoes the request id");
    Expect(reply.value("type", std::string{}) == "file_browser", "the reply keeps the message type");
    Expect(reply.value("version", 0) == 1, "the reply keeps the wire version");
  }
};

// ------------------------------------------------------------------- table --
void TestOpTable() {
  Expect(ops::kOps.size() == 18, "the op table lists every dispatched op");
  Expect(ops::kFileManagementOps.size() == 11, "eleven file management ops were added");
  for (const auto name : ops::kFileManagementOps) {
    Expect(ops::IsOpName(name), "a file management op is in the dispatch table");
    Expect(ops::IsFileManagementOp(name), "a file management op is flagged as new");
  }
  Expect(ops::IsOpName("list") && ops::IsOpName("gallery") && ops::IsOpName("thumb") &&
             ops::IsOpName("list_cancel") && ops::IsOpName("prepare") &&
             ops::IsOpName("cancel") && ops::IsOpName("copy"),
         "every pre-existing op stays in the table");
  for (const auto name :
       {"list", "gallery", "thumb", "list_cancel", "prepare", "cancel", "copy"})
    Expect(!ops::IsFileManagementOp(name), "a pre-existing op is not flagged as new");
  std::size_t flagged = 0;
  for (const auto& info : ops::kOps) flagged += info.file_management ? 1 : 0;
  Expect(flagged == ops::kFileManagementOps.size(), "the table flags exactly the new ops");
  Expect(!ops::IsOpName("truncate") && !ops::IsOpName(""), "an unknown ops is not in the table");

  for (const auto code : {"invalid_path", "invalid_request", "exists", "not_found",
                          "not_writable", "symbolic_link", "unsupported", "too_many_files"})
    Expect(ops::IsErrorCode(code), "the vocabulary carries a required code");
  for (const auto code : {"directory_unavailable", "file_unreadable", "timeout", "busy",
                          "already_finished", "listing_expired"})
    Expect(ops::IsErrorCode(code), "the vocabulary keeps an existing code");
  Expect(!ops::IsErrorCode("oops") && !ops::IsErrorCode(""),
         "free text is never mistaken for a code");
  Expect(ops::kMaxDeletePaths == 256 && ops::kMaxStatPaths == 256,
         "the batches stay inside the 256 path budget");
}

// ------------------------------------------------------------ path safety --
void TestSafeChildPath(const fs::path& root) {
  fs::path out;
  Expect(crossdesk::SafeChildPath(root, "child", &out), "a plain child resolves");
  Expect(out == root / "child", "a plain child keeps the root prefix");
  Expect(crossdesk::SafeChildPath(root, "a/b", &out) && out == root / "a" / "b",
         "a nested relative path resolves");
  Expect(crossdesk::SafeChildPath(root, "", &out) && out == root,
         "an empty candidate is the root itself");

  auto reason = crossdesk::PathError::None;
  Expect(!crossdesk::SafeChildPath(root, "../escape", &out, &reason), "a .. escape is refused");
  Expect(reason == crossdesk::PathError::InvalidPath, "the .. escape reports invalid_path");
  Expect(!crossdesk::SafeChildPath(root, "a/../../escape", &out), "a nested .. escape is refused");
  Expect(!crossdesk::SafeChildPath(root, "..", &out), "a bare .. is refused");
  Expect(!crossdesk::SafeChildPath(root, ".", &out), "a bare . is refused");
  Expect(!crossdesk::SafeChildPath(root, "/etc/passwd", &out), "an absolute path is refused");
#ifdef _WIN32
  Expect(!crossdesk::SafeChildPath(root, "C:\\Windows", &out), "a drive-qualified path is refused");
  Expect(!crossdesk::SafeChildPath(root, "C:relative", &out), "a drive-relative path is refused");
#endif
  Expect(!crossdesk::SafeChildPath(fs::path{}, "child", &out), "an empty root is refused");

  // A link out of the root must not be walkable. Creating one needs a privilege
  // on Windows, so a failure skips the link checks with a printed note.
  const auto outside = ScratchRoot("outside");
  const auto link = root / "outlink";
  std::error_code ec;
  fs::create_directory_symlink(outside, link, ec);
  if (ec) {
    std::printf("note  symbolic links unavailable here, link checks skipped\n");
  } else {
    std::error_code link_status_error;
    Expect(ops::IsLinkLike(fs::symlink_status(link, link_status_error)),
           "a link component is reported link-like");
    Expect(!ops::IsLinkLike(fs::symlink_status(root, link_status_error)),
           "a plain directory is not link-like");
    Expect(!crossdesk::SafeChildPath(root, "outlink", &out, &reason),
           "a final link component is refused");
    Expect(reason == crossdesk::PathError::SymbolicLink, "the link reports symbolic_link");
    Expect(!crossdesk::SafeChildPath(root, "outlink/thing", &out, &reason),
           "a link in the middle of the path is refused");
    Expect(reason == crossdesk::PathError::SymbolicLink, "the middle link reports symbolic_link");
    Expect(crossdesk::SafeChildPath(root, "outlink", &out, nullptr, true),
           "the final component may be a link when the caller allows it");
    std::error_code removed;
    fs::remove(link, removed);
  }
  std::error_code cleanup;
  fs::remove_all(outside, cleanup);
}

void TestContainment(const fs::path& root) {
  Expect(crossdesk::ContainedIn(root, root), "a directory contains itself");
  Expect(crossdesk::ContainedIn(root, root / "sub" / "deep"), "a directory contains its children");
  Expect(!crossdesk::ContainedIn(root, root.parent_path()),
         "a directory does not contain its parent");
  Expect(!crossdesk::ContainedIn(root, root / ".." / "sibling"),
         "a lexical .. escape is outside");
  Expect(!crossdesk::ContainedIn(root, fs::path{}), "an empty path is outside");
}

void TestEntryNames() {
  for (const auto name : {"report.txt", "照片 2026", ".hidden", "a"})
    Expect(ops::ValidEntryName(name), "a plain name is valid");
  for (const auto name : {"", ".", "..", "a/b", "a\\b", "a:b", "CON", "nul.txt",
                          "COM3", "LPT1.log", "trailing.", "trailing ", "a*b"})
    Expect(!ops::ValidEntryName(name), "a name that is not a single component is refused");
}

// ------------------------------------------------------- temp_clear select --
void TestStagingSelection(const fs::path& root) {
  const auto receive = root / "receive";
  const auto first = receive / ".crossdesk-1-2-3";
  const auto nested = receive / "sub" / ".crossdesk-4-5-6";
  fs::create_directories(first);
  fs::create_directories(nested);
  fs::create_directories(receive / "keep");
  WriteFile(first / ".partial", "half");
  WriteFile(receive / ".crossdesk-file", "looks like staging but is a file");

  const auto selected = crossdesk::SelectStagingDirectories(receive);
  Expect(selected.size() == 2, "only staging directories are selected");
  Expect(selected.size() == 2 && selected[0] == first && selected[1] == nested,
         "both staging directories are selected, including a nested one");
  Expect(crossdesk::SelectStagingDirectories(receive, 1).size() == 1,
         "the selection limit bounds the walk");
  Expect(crossdesk::SelectStagingDirectories(receive, 0).empty(),
         "a zero limit selects nothing");
  Expect(crossdesk::SelectStagingDirectories({}).empty(), "an empty root selects nothing");
  Expect(!ops::IsStagingDirectoryName(".crossdesk") &&
             !ops::IsStagingDirectoryName(".crossdesk-") &&
             ops::IsStagingDirectoryName(".crossdesk-1-2-3") &&
             !ops::IsStagingDirectoryName("crossdesk-1"),
         "only the .crossdesk- staging prefix matches");
}

// --------------------------------------------------------------------- ops --
void TestMkdir(Peer& peer, const fs::path& root) {
  auto reply = peer.Call("mkdir", {{"path", Utf8(root)}, {"name", "docs"}});
  peer.ExpectEnvelope(reply, "mkdir answers with the reply envelope");
  Expect(reply.value("ok", false), "mkdir creates one directory");
  Expect(fs::is_directory(root / "docs"), "mkdir created the directory on disk");
  Expect(reply.value("path", std::string{}) == Utf8(root / "docs"),
         "mkdir reports the created path");

  reply = peer.Call("mkdir", {{"path", Utf8(root)}, {"name", "docs"}});
  Expect(!reply.value("ok", true), "mkdir refuses an existing name");
  Expect(reply.value("error", std::string{}) == "exists", "mkdir answers exists");

  WriteFile(root / "file.txt", "x");
  reply = peer.Call("mkdir", {{"path", Utf8(root)}, {"name", "file.txt"}});
  Expect(reply.value("error", std::string{}) == "exists",
         "mkdir answers exists for a file with that name");

  reply = peer.Call("mkdir", {{"path", Utf8(root)}, {"name", "../escape"}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "mkdir refuses a path separator in the name");
  Expect(!fs::exists(root.parent_path() / "escape"), "the refused name created nothing");

  reply = peer.Call("mkdir", {{"path", "docs"}, {"name", "sub"}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "mkdir refuses a relative parent path");

  reply = peer.Call("mkdir", {{"path", Utf8(root / "docs")}, {"name", "CON"}});
  Expect(reply.value("error", std::string{}) == "invalid_path", "mkdir refuses a device name");

  reply = peer.Call("mkdir", {{"path", Utf8(root)}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "mkdir answers invalid_request without a name");
  Expect(ops::IsErrorCode(reply.value("error", std::string{})),
         "the mkdir error is a vocabulary code");
}

void TestRename(Peer& peer, const fs::path& root) {
  WriteFile(root / "a.txt", "one");
  WriteFile(root / "b.txt", "two");
  auto reply = peer.Call("rename", {{"path", Utf8(root / "a.txt")}, {"name", "c.txt"}});
  peer.ExpectEnvelope(reply, "rename answers with the reply envelope");
  Expect(reply.value("ok", false), "rename moves one entry");
  Expect(!fs::exists(root / "a.txt") && fs::exists(root / "c.txt"),
         "rename changed the name on disk");
  Expect(reply.value("path", std::string{}) == Utf8(root / "c.txt"),
         "rename reports the new path");

  reply = peer.Call("rename", {{"path", Utf8(root / "c.txt")}, {"name", "b.txt"}});
  Expect(!reply.value("ok", true), "rename refuses a taken name");
  Expect(reply.value("error", std::string{}) == "exists", "rename answers exists");
  Expect(fs::exists(root / "c.txt"), "a refused rename leaves the source alone");

  reply = peer.Call("rename", {{"path", Utf8(root / "c.txt")}, {"name", "sub/c.txt"}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "rename refuses to move across folders");

  reply = peer.Call("rename", {{"path", Utf8(root / "missing.txt")}, {"name", "x.txt"}});
  Expect(reply.value("error", std::string{}) == "not_found", "rename answers not_found");

  reply = peer.Call("rename", {{"path", Utf8(root / "c.txt")}, {"name", ".."}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "rename refuses a traversal target");
}

void TestStat(Peer& peer, const fs::path& root) {
  WriteFile(root / "stat.bin", "0123456789");
  auto reply = peer.Call("stat", {{"paths", {Utf8(root / "stat.bin"), Utf8(root / "gone.bin"),
                                             Utf8(root)}}});
  peer.ExpectEnvelope(reply, "stat answers with the reply envelope");
  Expect(reply.value("ok", false), "stat answers a whole batch");
  const auto& entries = reply.at("entries");
  Expect(entries.is_array() && entries.size() == 3, "stat answers one entry per path");
  if (entries.is_array() && entries.size() == 3) {
    Expect(entries[0].value("exists", false), "stat finds the file");
    Expect(entries[0].value("size", 0ull) == 10ull, "stat reports the size");
    Expect(!entries[0].value("directory", true) && !entries[0].value("link", true),
           "stat reports a plain file");
    Expect(!entries[0].value("modified", std::string{}).empty(), "stat reports a modified stamp");
    Expect(entries[0].at("modified_ms").is_number(), "stat reports modified_ms");
    Expect(!entries[1].value("exists", true), "stat reports a missing path without failing");
    Expect(entries[1].value("error", std::string{}) == "",
           "a vanished path is not an error the UI shows");
    Expect(entries[2].value("directory", false), "stat reports a directory");
  }
  reply = peer.Call("stat", {{"paths", {}}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "stat answers invalid_request for an empty batch");
}

void TestDelete(Peer& peer, const fs::path& root) {
  fs::create_directories(root / "tree" / "sub");
  WriteFile(root / "tree" / "sub" / "leaf.txt", "leaf");
  WriteFile(root / "loose.txt", "loose");
  auto reply = peer.Call("delete", {{"paths", {Utf8(root / "tree"), Utf8(root / "loose.txt"),
                                              Utf8(root / "gone.bin")}}});
  peer.ExpectEnvelope(reply, "delete answers with the reply envelope");
  Expect(reply.value("ok", false), "delete answers the whole batch");
  const auto& results = reply.at("results");
  Expect(results.is_array() && results.size() == 3, "delete answers one result per path");
  if (results.is_array() && results.size() == 3) {
    Expect(results[0].value("ok", false), "delete removes a folder tree");
    Expect(!fs::exists(root / "tree"), "the folder tree is gone, descendants included");
    Expect(results[1].value("ok", false) && !fs::exists(root / "loose.txt"),
           "delete removes a file");
    Expect(!results[2].value("ok", true), "delete reports a missing path");
    Expect(results[2].value("error", std::string{}) == "not_found",
           "delete answers not_found per path");
    Expect(results[2].value("path", std::string{}) == Utf8(root / "gone.bin"),
           "the per-path result echoes the request path");
  }
  Expect(reply.value("removed", 0u) == 2u, "delete counts what it removed");

  // A link is refused rather than followed: nothing outside the named parent can
  // be removed through it.
  const auto outside = ScratchRoot("delete-outside");
  WriteFile(outside / "keep.txt", "keep");
  std::error_code ec;
  fs::create_directory_symlink(outside, root / "dirlink", ec);
  if (ec) {
    std::printf("note  symbolic links unavailable here, delete link check skipped\n");
  } else {
    reply = peer.Call("delete", {{"paths", {Utf8(root / "dirlink")}}});
    Expect(reply.at("results").at(0).value("error", std::string{}) == "symbolic_link",
           "delete refuses a linked directory");
    std::error_code status_error;
    Expect(fs::is_symlink(fs::symlink_status(root / "dirlink", status_error)),
           "the refused link survives");
    Expect(fs::exists(outside / "keep.txt"), "the link target is untouched");
    std::error_code removed;
    fs::remove(root / "dirlink", removed);
  }
  std::error_code cleanup;
  fs::remove_all(outside, cleanup);
}

void TestDeleteCap(Peer& peer, const fs::path& root) {
  WriteFile(root / "cap.txt", "cap");
  std::vector<std::string> paths(ops::kMaxDeletePaths + 1, Utf8(root / "cap.txt"));
  auto reply = peer.Call("delete", {{"paths", paths}});
  Expect(!reply.value("ok", true), "delete refuses an oversized batch");
  Expect(reply.value("error", std::string{}) == "too_many_files",
         "the batch cap answers too_many_files");
  Expect(fs::exists(root / "cap.txt"), "an oversized batch deletes nothing");

  paths.resize(ops::kMaxDeletePaths);
  reply = peer.Call("delete", {{"paths", paths}});
  Expect(reply.value("ok", false), "a batch at the cap is accepted");
  Expect(!fs::exists(root / "cap.txt"), "a batch at the cap deletes what it names");
}

void TestTempClear(Peer& peer, const fs::path& root) {
  const auto receive = root / "receive";
  const auto staging = receive / ".crossdesk-1-2-3";
  const auto nested = receive / "sub" / ".crossdesk-4-5-6";
  fs::create_directories(staging);
  fs::create_directories(nested);
  fs::create_directories(receive / "keep");
  WriteFile(staging / ".partial", "half");
  WriteFile(receive / ".crossdesk-file", "looks like staging but is a file");
  peer.browser.SetReceiveDirectory(Utf8(receive));
  Expect(peer.browser.ReceiveDirectory() == Utf8(receive),
         "the configured receive directory is reported");

  auto reply = peer.Call("temp_clear", {{"directory", Utf8(receive)}});
  peer.ExpectEnvelope(reply, "temp_clear answers with the reply envelope");
  Expect(reply.value("ok", false), "temp_clear sweeps the receive directory");
  Expect(!fs::exists(staging) && !fs::exists(nested),
         "both staging directories are gone");
  Expect(fs::exists(receive / "keep"), "temp_clear keeps ordinary folders");
  Expect(fs::exists(receive / ".crossdesk-file"),
         "temp_clear keeps a file that only looks like staging");
  Expect(reply.value("removed_count", 0u) == 2u, "temp_clear counts what it removed");

  const auto elsewhere = root / "elsewhere";
  const auto outside_staging = elsewhere / ".crossdesk-9-9-9";
  fs::create_directories(outside_staging);
  reply = peer.Call("temp_clear", {{"directory", Utf8(elsewhere)}});
  Expect(!reply.value("ok", true), "temp_clear refuses a directory outside the receive root");
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "an outside directory answers invalid_path");
  Expect(fs::exists(outside_staging), "a refused temp_clear deletes nothing");

  fs::create_directories(receive / ".crossdesk-7-8-9");
  reply = peer.Call("temp_clear", {{"directory", Utf8(receive)}});
  Expect(reply.value("ok", false), "temp_clear accepts the receive root itself");
  Expect(!fs::exists(receive / ".crossdesk-7-8-9"), "the fresh staging directory is swept");

  reply = peer.Call("temp_clear", {{"directory", "receive"}});
  Expect(reply.value("error", std::string{}) == "invalid_path",
         "temp_clear refuses a relative directory");

  reply = peer.Call("temp_clear", {{"directory", Utf8(root / "missing")}});
  Expect(reply.value("error", std::string{}) == "directory_unavailable",
         "temp_clear answers directory_unavailable for a missing directory");
}

void TestOpenReveal(Peer& peer, const fs::path& root) {
  // Only the refusal paths are driven here: a valid path asks the desktop shell
  // for a window, which a test run must not do.
  auto reply = peer.Call("open", {{"path", "docs/readme.txt"}});
  peer.ExpectEnvelope(reply, "open answers with the reply envelope");
  Expect(!reply.value("ok", true), "open refuses a relative path");
  Expect(reply.value("error", std::string{}) == "invalid_path", "open answers invalid_path");

  reply = peer.Call("reveal", {{"path", Utf8(root / "missing.txt")}});
  Expect(reply.value("error", std::string{}) == "not_found", "reveal answers not_found");

  reply = peer.Call("reveal", nlohmann::json::object());
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "reveal answers invalid_request without a path");

  WriteFile(root / "open-me.txt", "note");
#ifndef _WIN32
  reply = peer.Call("open", {{"path", Utf8(root / "open-me.txt")}});
  Expect(reply.value("error", std::string{}) == "unsupported",
         "open is a no-op with unsupported off Windows");
  reply = peer.Call("reveal", {{"path", Utf8(root / "open-me.txt")}});
  Expect(reply.value("error", std::string{}) == "unsupported",
         "reveal is a no-op with unsupported off Windows");
#endif
}

void TestUnknownOp(Peer& peer) {
  auto reply = peer.Call("truncate", {{"path", "x"}});
  peer.ExpectEnvelope(reply, "an unknown op still answers with the reply envelope");
  Expect(!reply.value("ok", true), "an unknown op is refused");
  Expect(reply.value("error", std::string{}) == "unsupported",
         "an unknown op answers unsupported");
}

}  // namespace

int main() {
  const auto root = ScratchRoot("root");
  Expect(!root.empty() && fs::is_directory(root), "the scratch root exists");
  TestOpTable();
  TestSafeChildPath(root);
  TestContainment(root);
  TestEntryNames();
  TestStagingSelection(root);
  {
    Peer peer;
    TestMkdir(peer, root);
    TestRename(peer, root);
    TestStat(peer, root);
    TestDelete(peer, root);
    TestDeleteCap(peer, root);
    TestTempClear(peer, root);
    TestOpenReveal(peer, root);
    TestUnknownOp(peer);
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  if (g_failures) {
    std::printf("%d file browser op checks failed\n", g_failures);
    return 1;
  }
  std::printf("file browser op checks passed\n");
  return 0;
}
