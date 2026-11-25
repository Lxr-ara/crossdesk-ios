#include <cstdio>
#include "file_browser.h"

#include <file_transfer_core.h>
#include <file_transfer_resume.h>
#include <photo_library.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
// ShellExecuteW is resolved at run time so the controller archive stays free of
// an extra import library: the CLI and mobile link lines do not add Shell32.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// <windows.h> turns SendMessage into a macro; the class member of that name is
// declared in file_browser.h and has to survive the include.
#undef SendMessage
#endif

namespace crossdesk {
namespace {
using Json = FileBrowser::Json;
namespace fs = std::filesystem;
namespace ops = file_browser_ops;
constexpr size_t kPageEntries = 512;
constexpr size_t kPageBytes = 45000;
fs::path PathFromUtf8(std::string_view value) {
  return fs::path(std::u8string(value.begin(), value.end()));
}
std::string Utf8(const fs::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
fs::path Home() {
#ifdef _WIN32
  const auto* home = std::getenv("USERPROFILE");
#else
  const auto* home = std::getenv("HOME");
#endif
  return home ? PathFromUtf8(home) : fs::current_path();
}
Json Failure(const std::string& reason) { return {{"ok", false}, {"error", reason}}; }
template <typename FileTime>
auto SystemTime(FileTime time, FileTime file_now, std::chrono::system_clock::time_point system_now) {
  using Clock = typename FileTime::clock;
  if constexpr (requires { Clock::to_sys(time); })
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(Clock::to_sys(time));
  else
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - file_now + system_now);
}
std::pair<std::string, size_t> ParseCursor(const std::string& cursor) {
  const auto split = cursor.find(':');
  if (cursor.size() > 64 || split == std::string::npos || !split)
    throw std::runtime_error("invalid_listing");
  size_t offset = 0;
  const auto end = cursor.data() + cursor.size();
  const auto parsed = std::from_chars(cursor.data() + split + 1, end, offset);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::runtime_error("invalid_listing");
  return {cursor.substr(0, split), offset};
}
Json Page(const Json& listing, size_t offset, const std::string& snapshot) {
  const auto& entries = listing.at("entries");
  if (offset > entries.size()) throw std::runtime_error("invalid_listing");
  Json page{{"ok", true}, {"path", listing.at("path")}, {"parent", listing.at("parent")},
      {"entries", Json::array()}, {"offset", offset}, {"total", entries.size()}};
  size_t next = offset, bytes = 0;
  while (next < entries.size() && next - offset < kPageEntries) {
    const auto entry_bytes = entries[next].dump(-1, ' ', false, Json::error_handler_t::replace).size() + 1;
    if (entry_bytes > kPageBytes) throw std::runtime_error("invalid_listing");
    if (bytes + entry_bytes > kPageBytes) break;
    page["entries"].push_back(entries[next++]);
    bytes += entry_bytes;
  }
  page["truncated"] = next < entries.size();
  page["next"] = next < entries.size() && !snapshot.empty()
      ? snapshot + ":" + std::to_string(next) : "";
  return page;
}
std::string Text(const Json& value, const char* key) {
  const auto& field = value.at(key);
  if (!field.is_string()) throw std::runtime_error("invalid_request");
  auto text = field.get<std::string>();
  if (text.size() > 4096 || text.find('\0') != std::string::npos)
    throw std::runtime_error("invalid_path");
  return text;
}
fs::path ExistingDirectory(const std::string& path) {
  if (path.empty()) throw std::runtime_error("invalid_path");
  auto input = PathFromUtf8(path);
  if (!input.is_absolute()) throw std::runtime_error("invalid_path");
  std::error_code ec;
  auto result = fs::canonical(input, ec);
  if (ec || !fs::is_directory(result, ec)) throw std::runtime_error("directory_unavailable");
  return result;
}
// Validates the whole relative hierarchy with the shared containment rule, then
// creates it one component at a time. The validation itself lives in
// file_browser_ops.h so the listing path and the file management ops use one
// rule instead of two.
fs::path Destination(const fs::path& root, const std::string& relative) {
  fs::path result;
  ops::PathError reason = ops::PathError::None;
  if (!ops::SafeChildPath(root, relative, &result, &reason))
    throw std::runtime_error(reason == ops::PathError::SymbolicLink ? "symbolic_link" : "invalid_path");
  auto walk = root;
  for (const auto& component : PathFromUtf8(relative)) {
    walk /= component;
    std::error_code ec;
    fs::create_directory(walk, ec);
    if (ec && ec != std::errc::file_exists) throw std::runtime_error("directory_not_writable");
    if (!fs::is_directory(walk)) throw std::runtime_error("directory_not_writable");
  }
  return result;
}

// Modified stamp for one entry, mirroring what the listing reports (same text,
// same millisecond value) so `stat` can refresh a row without a full list.
struct Stamps {
  std::string text;
  Json ms = nullptr;
};
Stamps Timestamps(const fs::path& path) {
  Stamps stamps;
  std::error_code error;
  const auto time = fs::last_write_time(path, error);
  if (error) return stamps;
  const auto system_time = SystemTime(time, fs::file_time_type::clock::now(),
                                     std::chrono::system_clock::now());
  stamps.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      system_time.time_since_epoch()).count();
  const auto stamp = std::chrono::system_clock::to_time_t(system_time);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &stamp);
#else
  localtime_r(&stamp, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
  stamps.text = buffer;
  return stamps;
}

// An absolute path that must already exist. The path is walked from its own root
// one component at a time through SafeChildPath, so every component - not only
// the last - is checked for a link: a request cannot be redirected out of the
// directory it printed. `allow_link` relaxes the final component only, which the
// read-only ops use to report, open or move a link itself.
fs::path ExistingEntry(const std::string& value, bool allow_link = false) {
  const auto input = PathFromUtf8(value);
  if (!input.is_absolute() || input.filename().empty())
    throw std::runtime_error("invalid_path");
  const auto anchor = input.root_path();
  const auto relative = anchor.empty() ? fs::path{} : input.lexically_relative(anchor);
  if (relative.empty()) throw std::runtime_error("invalid_path");
  fs::path resolved;
  ops::PathError reason = ops::PathError::None;
  if (!ops::SafeChildPath(anchor, ops::Utf8Text(relative), &resolved, &reason, allow_link))
    throw std::runtime_error(reason == ops::PathError::SymbolicLink ? "symbolic_link" : "invalid_path");
  std::error_code ec;
  const auto status = fs::symlink_status(resolved, ec);
  if (ec || status.type() == fs::file_type::not_found || status.type() == fs::file_type::none)
    throw std::runtime_error("not_found");
  return resolved;
}

// One path component from a request, validated before it becomes a create or
// rename target.
std::string EntryName(const std::string& value) {
  if (!ops::ValidEntryName(value)) throw std::runtime_error("invalid_path");
  return value;
}

// True when the request already names a file or a directory.
bool Exists(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  return !ec && status.type() != fs::file_type::not_found && status.type() != fs::file_type::none;
}

// Refusal body with one code from the shared vocabulary.
Json Refuse(std::string_view code) { return Failure(std::string(code)); }

// Request field accessors for the file management ops. A missing or mistyped
// field answers "invalid_request" instead of leaking nlohmann's own exception
// text, so every reply error is a code from file_browser_ops.h.
std::string RequestText(const Json& message, const char* key) {
  const auto found = message.find(key);
  if (found == message.end() || !found->is_string())
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  return Text(message, key);
}
const Json& RequestArray(const Json& message, const char* key) {
  const auto found = message.find(key);
  if (found == message.end() || !found->is_array())
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  return *found;
}
// 非负整数字段。nlohmann 把小的正数字面量解析成 number_integer 而不是
// number_unsigned，所以两条分支都要接受；负数、浮点、字符串一律当
// invalid_request，不把 nlohmann 自己的异常文本漏到应答里。
bool RequestUint(const Json& message, const char* key, uint64_t* out) {
  const auto found = message.find(key);
  if (found == message.end() || out == nullptr) return false;
  if (found->is_number_unsigned()) {
    *out = found->get<uint64_t>();
    return true;
  }
  if (!found->is_number_integer()) return false;
  const auto value = found->get<int64_t>();
  if (value < 0) return false;
  *out = static_cast<uint64_t>(value);
  return true;
}

// 手机端传输面板的 `remote` 字段：可选，缺省是空串（宿主自己的默认范围）。
// 它只是一个不透明字符串：控制器既不给它定语义，也不拿它碰文件系统。
std::string OptionalRemote(const Json& message) {
  const auto found = message.find("remote");
  if (found == message.end() || found->is_null()) return {};
  if (!found->is_string()) throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  const auto remote = found->get<std::string>();
  if (remote.size() > 256 || remote.find('\0') != std::string::npos)
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  return remote;
}

// 生效值回读时用：宿主返回什么就夹到什么范围里，应答里永远只有合法值。
int Clamped(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

// ---------------------------------------------------- host open / reveal ----
#ifdef _WIN32
// ShellExecuteW is resolved at run time: the controller archive then needs no
// extra import library, so the CLI link line (advapi32/user32/ws2_32 only) and
// every mobile link stays valid.
using ShellExecuteWide = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
ShellExecuteWide ShellExecuteApi() {
  static const auto api = [] {
    const auto shell = ::GetModuleHandleW(L"shell32.dll");
    if (!shell) return static_cast<ShellExecuteWide>(nullptr);
    return reinterpret_cast<ShellExecuteWide>(::GetProcAddress(shell, "ShellExecuteW"));
  }();
  return api;
}
#endif

// The only place that talks to the desktop shell. Windows asks the shell to open
// the entry or its folder; every other platform answers "unsupported" and does
// nothing, which keeps the controller linkable on Linux, macOS and mobile.
Json HostOpen(const fs::path& path, bool reveal) {
#ifdef _WIN32
  const auto api = ShellExecuteApi();
  if (!api) return Refuse(ops::kErrorUnsupported);
  const auto target = path.wstring();
  const auto parent = path.parent_path().wstring();
  const auto launch = [api](const std::wstring& file, const std::wstring& parameters,
                            const std::wstring& directory) {
    const auto result = api(nullptr, L"open", file.c_str(), parameters.c_str(),
                            directory.empty() ? nullptr : directory.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
  };
  if (reveal) {
    // explorer /select,"<path>" opens the containing folder with the entry
    // selected; the documented command line verb needs no COM apartment. Fall
    // back to opening the parent folder when the shell rejects the selection.
    if (launch(L"explorer.exe", L"/select,\"" + target + L"\"", std::wstring{}))
      return {{"ok", true}, {"path", ops::Utf8Text(path)}, {"revealed", true}};
    if (!parent.empty() && launch(L"explorer.exe", L"\"" + parent + L"\"", std::wstring{}))
      return {{"ok", true}, {"path", ops::Utf8Text(path)}, {"revealed", false}};
    return Refuse(ops::kErrorFileUnreadable);
  }
  if (launch(target, std::wstring{}, parent))
    return {{"ok", true}, {"path", ops::Utf8Text(path)}, {"opened", true}};
  return Refuse(ops::kErrorFileUnreadable);
#else
  (void)path;
  (void)reveal;
  return Refuse(ops::kErrorUnsupported);
#endif
}
}

FileBrowser::FileBrowser(Send send, Enqueue enqueue, Reserve reserve, Cancel cancel)
    : send_(std::move(send)), enqueue_(std::move(enqueue)), reserve_(std::move(reserve)), cancel_(std::move(cancel)),
      worker_([this] { Run(); }) {}
FileBrowser::~FileBrowser() { Stop(); }
void FileBrowser::Stop() {
  { std::lock_guard lock(mutex_); stopping_ = true; jobs_.clear();
    for (const auto& [_, listing] : listings_) listing->cancelled = true;
    listings_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}
void FileBrowser::Post(const std::string& peer, std::function<void()> work) {
  std::lock_guard lock(mutex_);
  if (stopping_ || jobs_.size() >= 512) return;
  const auto generation = generations_[peer];
  jobs_.push_back([this, peer, generation, work = std::move(work)] {
    { std::lock_guard lock(mutex_);
      if (stopping_ || generations_[peer] != generation) return;
    }
    active_peer_ = peer; active_generation_ = generation;
    work();
  });
  wake_.notify_one();
}
void FileBrowser::Run() {
  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) return;
      if (!jobs_.empty()) { work = std::move(jobs_.front()); jobs_.pop_front(); }
    }
    try { if (work) work(); } catch (...) { /* Invalid peer data cannot kill the worker. */ }
    std::erase_if(directory_snapshots_, [](const auto& entry) {
      return Clock::now() >= entry.second.expires;
    });
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (Clock::now() < it->second.deadline) { ++it; continue; }
      auto pending = std::move(it->second); it = pending_.erase(it);
      if (Current(pending.peer, pending.generation)) {
        active_peer_ = pending.peer; active_generation_ = pending.generation;
        pending.complete(Failure("timeout"));
      }
    }
  }
}
bool FileBrowser::Current(const std::string& peer, uint64_t generation) {
  std::lock_guard lock(mutex_);
  return !stopping_ && generations_[peer] == generation;
}
void FileBrowser::Report(const std::string& peer, Json result, bool notify_peer,
                         const std::string& transfer_id) {
  if (!transfer_id.empty()) result["transfer_id"] = transfer_id;
  if (!Current(peer, active_generation_)) return;
  Publish(peer, 0, result);
  if (notify_peer) {
    result["op"] = "notice"; result["id"] = next_id_++;
    SendMessage(peer, std::move(result));
  }
}
bool FileBrowser::SendMessage(const std::string& peer, Json message) {
  { std::lock_guard lock(mutex_);
    if (stopping_ || (peer == active_peer_ && generations_[peer] != active_generation_)) return false;
  }
  message["type"] = "file_browser"; message["version"] = 1;
  return send_(peer, message);
}
void FileBrowser::Publish(const std::string& peer, uint32_t id, Json result,
                          const std::string& channel) {
  std::lock_guard lock(mutex_);
  auto& events = events_[peer];
  if (id) {
    for (auto& event : events) {
      if (event.id == id && event.channel == channel && event.result.value("loading", false)) {
        event.result = std::move(result);
        return;
      }
    }
  }
  if (events.size() < 512) events.push_back({id, std::move(result), channel});
}
std::vector<FileBrowser::Event> FileBrowser::TakeEvents(const std::string& peer) {
  return TakeEvents(peer, std::string{});
}
std::vector<FileBrowser::Event> FileBrowser::TakeEvents(const std::string& peer,
                                                        const std::string& channel) {
  std::lock_guard lock(mutex_);
  std::vector<Event> taken;
  auto& events = events_[peer];
  taken.reserve(events.size());
  for (auto it = events.begin(); it != events.end();) {
    if (it->channel != channel) { ++it; continue; }
    taken.push_back(std::move(*it));
    it = events.erase(it);
  }
  return taken;
}
uint64_t FileBrowser::Generation(const std::string& peer) {
  std::lock_guard lock(mutex_);
  const auto found = generations_.find(peer);
  return found == generations_.end() ? 0 : found->second;
}
void FileBrowser::Disconnect(const std::string& peer) {
  {
    std::lock_guard lock(mutex_);
    ++generations_[peer]; events_.erase(peer);
    std::erase_if(listings_, [&](auto& entry) {
      if (entry.second->peer != peer) return false;
      entry.second->cancelled = true;
      return true;
    });
  }
  Post(peer, [this, peer] {
    std::erase_if(pending_, [&](const auto& entry) { return entry.second.peer == peer; });
    std::erase_if(directory_snapshots_, [&](const auto& entry) { return entry.first.first == peer; });
    Publish(peer, 0, Failure("disconnected"));
  });
}
void FileBrowser::Request(const std::string& peer, Json message,
                          std::function<void(const Json&)> complete) {
  if (pending_.size() >= 256) { complete(Failure("busy")); return; }
  const auto id = next_id_++;
  message["id"] = id;
  pending_.emplace(id, Pending{peer, active_generation_, Clock::now() + std::chrono::seconds(12), std::move(complete)});
  if (!SendMessage(peer, std::move(message))) {
    auto done = std::move(pending_.at(id).complete); pending_.erase(id);
    done(Failure("disconnected"));
  }
}
uint32_t FileBrowser::List(const std::string& peer, const std::string& path, bool local) {
  const auto id = next_id_++;
  auto listing = std::make_shared<Listing>();
  listing->id = id; listing->peer = peer; listing->path = path;
  {
    std::lock_guard lock(mutex_);
    listing->generation = generations_[peer];
    listings_[id] = listing;
  }
  Post(peer, [this, listing, local] {
    if (listing->cancelled) return;
    if (local) FinishList(listing, Directory(listing->path, [listing] { return listing->cancelled.load(); }));
    else ListPage(listing);
  });
  return id;
}
uint32_t FileBrowser::Gallery(const std::string& peer, const std::string& path, bool recursive) {
  const auto id = next_id_++;
  auto listing = std::make_shared<Listing>();
  listing->id = id; listing->peer = peer; listing->path = path;
  listing->op = "gallery"; listing->recursive = recursive;
  {
    std::lock_guard lock(mutex_);
    listing->generation = generations_[peer];
    listings_[id] = listing;
  }
  Post(peer, [this, listing] {
    if (listing->cancelled) return;
    ListPage(listing);
  });
  return id;
}
void FileBrowser::Thumbnail(const std::string& peer, const std::string& path, int edge) {
  Post(peer, [this, peer, path, edge] {
    Request(peer, {{"op", "thumb"}, {"path", path}, {"edge", edge}},
        [this, peer](const Json& result) {
          Json event = result;
          event["kind"] = "thumb";
          Publish(peer, 0, std::move(event), "gallery");
        });
  });
}
void FileBrowser::CancelList(const std::string& peer, uint32_t id) {
  std::lock_guard lock(mutex_);
  const auto found = listings_.find(id);
  if (found != listings_.end() && found->second->peer == peer) {
    found->second->cancelled = true;
    listings_.erase(found);
  }
  if (const auto events = events_.find(peer); events != events_.end())
    std::erase_if(events->second, [id](const auto& event) { return event.id == id; });
}
void FileBrowser::FinishList(const std::shared_ptr<Listing>& listing, Json result) {
  std::lock_guard lock(mutex_);
  if (listing->cancelled || stopping_ || generations_[listing->peer] != listing->generation) return;
  listings_.erase(listing->id);
  const auto channel = listing->op == "gallery" ? std::string("gallery") : std::string{};
  auto& events = events_[listing->peer];
  std::erase_if(events, [&](const auto& event) {
    return event.id == listing->id && event.channel == channel;
  });
  if (events.size() < 512) events.push_back({listing->id, std::move(result), channel});
}
void FileBrowser::ReleaseList(const std::string& peer, const std::string& cursor) {
  if (!cursor.empty()) Request(peer, {{"op", "list_cancel"}, {"cursor", cursor}}, [](const Json&) {});
}
void FileBrowser::ListPage(const std::shared_ptr<Listing>& listing) {
  if (listing->cancelled) { ReleaseList(listing->peer, listing->cursor); return; }
  Json request{{"op", listing->op}, {"path", listing->path}, {"pagination", 1}};
  if (listing->op == "gallery") request["recursive"] = listing->recursive ? 1 : 0;
  if (!listing->cursor.empty()) request["cursor"] = listing->cursor;
  Request(listing->peer, std::move(request), [this, listing](const Json& page) {
    std::string next;
    try {
      next = page.value("next", std::string{});
      if (listing->cancelled) { ReleaseList(listing->peer, next.empty() ? listing->cursor : next); return; }
      if (!page.value("ok", false)) {
        ReleaseList(listing->peer, listing->cursor);
        FinishList(listing, page); return;
      }
      const auto& entries = page.at("entries");
      if (!entries.is_array() || entries.size() > 2000 || next.size() > 64)
        throw std::runtime_error("invalid_listing");
      if ((page.contains("offset") && !page["offset"].is_number_unsigned()) ||
          (page.contains("total") && !page["total"].is_number_unsigned()) ||
          (!next.empty() && !page.value("truncated", false)))
        throw std::runtime_error("invalid_listing");
      if (page.value("truncated", false) && next.empty())
        throw std::runtime_error("pagination_unsupported");
      const bool first = listing->result.is_null();
      const size_t offset = first ? 0 : listing->result.at("entries").size();
      if (page.value("offset", offset) != offset ||
          (!first && (page.at("path") != listing->result.at("path") ||
                      page.at("parent") != listing->result.at("parent") ||
                      page.at("total") != listing->result.at("total"))))
        throw std::runtime_error("invalid_listing");
      const size_t total = page.value("total", entries.size());
      if (total < offset || entries.size() > total - offset ||
          (next.empty() ? offset + entries.size() != total : entries.empty() || offset + entries.size() >= total))
        throw std::runtime_error("invalid_listing");
      if (!next.empty() && ParseCursor(next).second != offset + entries.size())
        throw std::runtime_error("invalid_listing");
      if (first) {
        listing->result = {{"ok", true}, {"path", page.at("path")}, {"parent", page.at("parent")},
            {"entries", Json::array()}, {"total", total}, {"truncated", false}};
      }
      auto& all = listing->result["entries"].get_ref<Json::array_t&>();
      all.insert(all.end(), entries.begin(), entries.end());
      if (next.empty()) { FinishList(listing, std::move(listing->result)); return; }
      listing->cursor = next;
      Publish(listing->peer, listing->id, {{"ok", true}, {"loading", true},
          {"loaded", all.size()}, {"total", total}},
          listing->op == "gallery" ? "gallery" : "");
      // Yield between pages so newer navigation and transfer work can run.
      Post(listing->peer, [this, listing] { ListPage(listing); });
    } catch (const std::exception& error) {
      ReleaseList(listing->peer, next.empty() ? listing->cursor : next);
      FinishList(listing, Failure(std::string(error.what()) == "pagination_unsupported"
          ? "pagination_unsupported" : "invalid_listing"));
    }
  });
}
void FileBrowser::Copy(const std::string& peer, const std::vector<std::string>& paths,
                       const std::string& destination, bool download) {
  Post(peer, [this, peer, paths, destination, download] {
    if (download) Request(peer, {{"op", "copy"}, {"paths", paths}, {"destination", destination}},
        [this, peer](const Json& result) { Publish(peer, 0, result); });
    else Upload(peer, paths, destination);
  });
}
void FileBrowser::CancelTransfer(const std::string& peer, uint32_t id) {
  Post(peer, [this, peer, id] {
    Request(peer, {{"op", "cancel"}, {"file_id", id}}, [this, peer](const Json& result) {
      Publish(peer, 0, result);
    });
  });
}
// One file-management request, one reply event. The event id is this request's
// own id (not the wire id Request() stamps), so a caller waiting in TakeEvents()
// matches the answer to the op it sent even when several are in flight. The
// 12-second Request() deadline turns a silent peer into a "timeout" reply
// instead of a request that never resolves.
uint32_t FileBrowser::Op(const std::string& peer, Json body) {
  const auto id = next_id_++;
  Post(peer, [this, peer, id, body = std::move(body)] {
    Request(peer, body, [this, peer, id](const Json& result) { Publish(peer, id, result); });
  });
  return id;
}
void FileBrowser::Handle(const std::string& peer, Json message) {
  Post(peer, [this, peer, message = std::move(message)] {
    try {
      if (message.at("version") != 1 || !message.at("id").is_number_unsigned()) return;
      const auto wide_id = message.at("id").get<uint64_t>();
      if (!wide_id || wide_id > UINT32_MAX) return;
      const auto id = static_cast<uint32_t>(wide_id);
      if (message.value("op", "") == "notice") {
        if (message.at("ok").is_boolean()) Publish(peer, 0, message);
        return;
      }
      if (message.value("op", "") == "reply") {
        const auto it = pending_.find(id);
        if (it == pending_.end() || it->second.peer != peer || !message.at("ok").is_boolean()) return;
        const auto generation = it->second.generation;
        auto done = std::move(it->second.complete); pending_.erase(it);
        if (!Current(peer, generation)) return;
        active_generation_ = generation;
        done(message);
      } else Serve(peer, message);
    } catch (...) { }
  });
}

FileBrowser::Json FileBrowser::Directory(const std::string& value, const std::function<bool()>& cancelled) {
  try {
    Json entries = Json::array();
    if (value == "@roots") {
#ifdef _WIN32
      for (char drive = 'A'; drive <= 'Z'; ++drive) {
        const std::string root = std::string(1, drive) + ":\\";
        std::error_code ec;
        if (fs::is_directory(PathFromUtf8(root), ec))
          entries.push_back({{"name", root}, {"path", root}, {"directory", true}, {"size", 0}, {"modified", ""}, {"link", false}});
      }
#else
      entries.push_back({{"name", "/"}, {"path", "/"}, {"directory", true}, {"size", 0}, {"modified", ""}, {"link", false}});
#endif
      return {{"ok", true}, {"path", value}, {"parent", ""}, {"entries", entries}, {"truncated", false}};
    }
    auto path = ExistingDirectory(value.empty() ? Utf8(Home()) : value);
    std::error_code ec;
    fs::directory_iterator it(path, ec), end;
    if (ec) return Failure("directory_unavailable");
    // Reuse one clock offset for every entry so equal timestamps stay equal.
    const auto file_now = fs::file_time_type::clock::now();
    const auto system_now = std::chrono::system_clock::now();
    for (; it != end; it.increment(ec)) {
      if (cancelled && cancelled()) return Failure("cancelled");
      if (ec) return Failure("directory_unavailable");
      const auto name = Utf8(it->path().filename());
      if (name.rfind(".crossdesk-", 0) == 0) continue;
      std::error_code error;
      const auto status = it->symlink_status(error);
      if (error) continue;
      const bool link = fs::is_symlink(status);
      const bool directory = it->is_directory(error);
      if (!directory && !link && !fs::is_regular_file(status)) continue;
      uint64_t size = 0;
      if (!directory && !link) { size = it->file_size(error); if (error) size = 0; }
      std::string modified;
      Json modified_ms = nullptr;
      const auto time = it->last_write_time(error);
      if (!error) {
        const auto system_time = SystemTime(time, file_now, system_now);
        modified_ms = std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch()).count();
        const auto stamp = std::chrono::system_clock::to_time_t(system_time);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &stamp);
#else
        localtime_r(&stamp, &tm);
#endif
        char buffer[32]; std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
        modified = buffer;
      }
      Json entry{{"name", name}, {"directory", directory}, {"size", size}, {"modified", modified},
          {"modified_ms", modified_ms}, {"link", link}};
      entries.push_back(std::move(entry));
    }
    if (ec) return Failure("directory_unavailable");
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
      if (a["directory"] != b["directory"]) return a["directory"] == true;
      auto first = a["name"].template get<std::string>(), second = b["name"].template get<std::string>();
      const auto lower = [](unsigned char c) { return std::tolower(c); };
      auto folded_first = first, folded_second = second;
      std::transform(folded_first.begin(), folded_first.end(), folded_first.begin(), lower);
      std::transform(folded_second.begin(), folded_second.end(), folded_second.begin(), lower);
      return folded_first == folded_second ? first < second : folded_first < folded_second;
    });
    return {{"ok", true}, {"path", Utf8(path)}, {"parent", Utf8(path.parent_path())},
            {"entries", std::move(entries)}, {"truncated", false}};
  } catch (...) { return Failure("directory_unavailable"); }
}

FileBrowser::Json FileBrowser::DirectoryPage(const std::string& peer, const Json& message) {
  const auto now = Clock::now();
  const auto cursor = message.value("cursor", std::string{});
  if (!cursor.empty()) {
    const auto [token, offset] = ParseCursor(cursor);
    const auto found = directory_snapshots_.find({peer, token});
    if (found == directory_snapshots_.end() || found->second.generation != active_generation_ ||
        now >= found->second.expires) return Failure("listing_expired");
    auto page = Page(found->second.listing, offset, token);
    if (page.at("next") == "") directory_snapshots_.erase(found);
    else found->second.expires = now + std::chrono::seconds(30);
    return page;
  }
  const auto generation = active_generation_;
  auto listing = Directory(Text(message, "path"), [this, &peer, generation] { return !Current(peer, generation); });
  if (!listing.value("ok", false)) return listing;
  const bool paginated = message.value("pagination", Json{}) == 1;
  const auto token = paginated ? std::to_string(next_snapshot_++) : "";
  auto page = Page(listing, 0, token);
  if (paginated && page.at("next") != "") {
    const auto snapshots = std::count_if(directory_snapshots_.begin(), directory_snapshots_.end(),
        [&](const auto& entry) { return entry.first.first == peer; });
    if (snapshots >= 16) return Failure("busy");
    directory_snapshots_.emplace(std::make_pair(peer, token),
        DirectorySnapshot{std::move(listing), active_generation_, Clock::now() + std::chrono::seconds(30)});
  }
  return page;
}

FileBrowser::Json FileBrowser::GalleryListing(const std::string& path, bool recursive,
                                              const std::function<bool()>& cancelled) const {
  photo::ScanOptions options;
  options.recursive = recursive;
  auto scan = photo::Scan(path, options, cancelled);
  if (!scan.ok) return Failure(scan.error.empty() ? "directory_unavailable" : scan.error);
  Json entries = Json::array();
  for (const auto& item : scan.entries) {
    entries.push_back({{"name", item.name}, {"path", item.path}, {"directory", false},
        {"size", item.size}, {"modified", item.date}, {"modified_ms", item.modified_ms},
        {"album", item.album}, {"extension", item.extension}, {"link", false},
        {"width", item.width}, {"height", item.height}});
  }
  return {{"ok", true}, {"path", scan.root}, {"parent", Utf8(PathFromUtf8(scan.root).parent_path())},
          {"entries", std::move(entries)}, {"truncated", scan.truncated}};
}

FileBrowser::Json FileBrowser::GalleryPage(const std::string& peer, const Json& message) {
  const auto now = Clock::now();
  const auto cursor = message.value("cursor", std::string{});
  if (!cursor.empty()) {
    const auto [token, offset] = ParseCursor(cursor);
    const auto found = directory_snapshots_.find({peer, token});
    if (found == directory_snapshots_.end() || found->second.generation != active_generation_ ||
        now >= found->second.expires) return Failure("listing_expired");
    auto page = Page(found->second.listing, offset, token);
    if (page.at("next") == "") directory_snapshots_.erase(found);
    else found->second.expires = now + std::chrono::seconds(30);
    return page;
  }
  const auto generation = active_generation_;
  const bool recursive = message.value("recursive", Json{1}) != 0;
  auto listing = GalleryListing(Text(message, "path"), recursive,
      [this, &peer, generation] { return !Current(peer, generation); });
  if (!listing.value("ok", false)) return listing;
  const bool paginated = message.value("pagination", Json{}) == 1;
  const auto token = paginated ? std::to_string(next_snapshot_++) : "";
  auto page = Page(listing, 0, token);
  if (paginated && page.at("next") != "") {
    const auto snapshots = std::count_if(directory_snapshots_.begin(), directory_snapshots_.end(),
        [&](const auto& entry) { return entry.first.first == peer; });
    if (snapshots >= 16) return Failure("busy");
    directory_snapshots_.emplace(std::make_pair(peer, token),
        DirectorySnapshot{std::move(listing), active_generation_, Clock::now() + std::chrono::seconds(30)});
  }
  return page;
}

FileBrowser::Json FileBrowser::ThumbnailReply(const Json& message) {
  const auto path = Text(message, "path");
  const auto edge = std::clamp(message.value("edge", photo::kTileEdge), 32, 384);
  std::error_code error;
  const auto file = PathFromUtf8(path);
  if (!file.is_absolute() || !fs::is_regular_file(file, error) || error)
    return Failure("file_unreadable");
  const auto stamp = fs::last_write_time(file, error);
  const auto version = error ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(
      stamp.time_since_epoch()).count();
  const auto key = path + "|" + std::to_string(version) + "|" + std::to_string(edge);
  if (const auto cached = thumbnails_.find(key); cached != thumbnails_.end()) {
    cached->second.used = ++thumb_clock_;
    return {{"ok", true}, {"kind", "thumb"}, {"path", path}, {"data", cached->second.data},
            {"width", cached->second.width}, {"height", cached->second.height},
            {"source_width", cached->second.source_width},
            {"source_height", cached->second.source_height}};
  }
  const auto thumb = photo::MakeThumbnail(path, edge, photo::kMaxThumbnailBytes);
  if (!thumb.ok) return Failure(thumb.error.empty() ? "decode_failed" : thumb.error);
  ThumbEntry entry;
  entry.key = key;
  entry.data = photo::Base64Encode(thumb.jpeg.data(), thumb.jpeg.size());
  entry.width = thumb.width;
  entry.height = thumb.height;
  entry.source_width = thumb.source_width;
  entry.source_height = thumb.source_height;
  entry.used = ++thumb_clock_;
  if (thumbnails_.size() >= 256) {
    auto oldest = thumbnails_.begin();
    for (auto it = thumbnails_.begin(); it != thumbnails_.end(); ++it)
      if (it->second.used < oldest->second.used) oldest = it;
    thumbnails_.erase(oldest);
  }
  thumbnails_.emplace(key, entry);
  return {{"ok", true}, {"kind", "thumb"}, {"path", path}, {"data", entry.data},
          {"width", entry.width}, {"height", entry.height},
          {"source_width", entry.source_width}, {"source_height", entry.source_height}};
}

// ------------------------------------------------ file management replies ----
// One `Serve()` branch per op. The envelope (op:"reply", id echo, ok, error) is
// added by Serve; these functions only build the body.
FileBrowser::Json FileBrowser::MkdirReply(const Json& message) {
  const auto parent = ExistingDirectory(RequestText(message, "path"));
  const auto name = EntryName(RequestText(message, "name"));
  fs::path target;
  ops::PathError reason = ops::PathError::None;
  if (!ops::SafeChildPath(parent, name, &target, &reason)) throw std::runtime_error("invalid_path");
  if (Exists(target)) return Refuse(ops::kErrorExists);
  std::error_code ec;
  fs::create_directory(target, ec);
  if (ec || !fs::is_directory(target, ec)) return Refuse(ops::kErrorNotWritable);
  return {{"ok", true}, {"path", ops::Utf8Text(target)}, {"parent", ops::Utf8Text(parent)}};
}

FileBrowser::Json FileBrowser::RenameReply(const Json& message) {
  const auto source = ExistingEntry(RequestText(message, "path"), true);
  const auto name = EntryName(RequestText(message, "name"));
  fs::path target;
  ops::PathError reason = ops::PathError::None;
  if (!ops::SafeChildPath(source.parent_path(), name, &target, &reason))
    throw std::runtime_error("invalid_path");
  if (Exists(target)) {
    // A case-only rename names the same file; anything else is a real collision.
    std::error_code equivalent_error;
    if (!fs::equivalent(source, target, equivalent_error) || equivalent_error)
      return Refuse(ops::kErrorExists);
  }
  std::error_code ec;
  fs::rename(source, target, ec);
  if (ec) return Refuse(ops::kErrorNotWritable);
  return {{"ok", true}, {"path", ops::Utf8Text(target)}, {"parent", ops::Utf8Text(target.parent_path())}};
}

FileBrowser::Json FileBrowser::DeleteReply(const Json& message) {
  const auto& items = RequestArray(message, "paths");
  if (items.empty()) throw std::runtime_error("invalid_request");
  if (items.size() > ops::kMaxDeletePaths) throw std::runtime_error(std::string(ops::kErrorTooManyFiles));
  Json results = Json::array();
  size_t removed = 0;
  for (const auto& item : items) {
    const auto path = Text(Json{{"path", item}}, "path");
    Json entry{{"path", path}, {"ok", false}, {"error", ""}};
    try {
      // Links are refused, and remove_all removes links instead of following
      // them, so nothing outside the validated parent can be touched.
      const auto target = ExistingEntry(path);
      if (target == target.root_path() || target.parent_path() == target)
        throw std::runtime_error(std::string(ops::kErrorInvalidPath));
      std::error_code ec;
      fs::remove_all(target, ec);
      if (ec) throw std::runtime_error(std::string(ops::kErrorNotWritable));
      entry["ok"] = true;
      ++removed;
    } catch (const std::exception& error) { entry["error"] = error.what(); }
    results.push_back(std::move(entry));
  }
  return {{"ok", true}, {"results", std::move(results)}, {"removed", removed}};
}

FileBrowser::Json FileBrowser::StatReply(const Json& message) {
  const auto& items = RequestArray(message, "paths");
  if (items.empty()) throw std::runtime_error("invalid_request");
  if (items.size() > ops::kMaxStatPaths) throw std::runtime_error(std::string(ops::kErrorTooManyFiles));
  Json entries = Json::array();
  for (const auto& item : items) {
    const auto path = Text(Json{{"path", item}}, "path");
    Json entry{{"path", path}, {"exists", false}, {"directory", false}, {"size", 0},
               {"modified", ""}, {"modified_ms", nullptr}, {"link", false}, {"error", ""}};
    try {
      const auto target = ExistingEntry(path, true);
      std::error_code ec;
      const auto status = fs::symlink_status(target, ec);
      if (ec) throw std::runtime_error(std::string(ops::kErrorFileUnreadable));
      // A link (or a Windows junction) is reported as a link plus whatever it
      // points at, so a junction to a folder still reads as a folder.
      const bool link = ops::IsLinkLike(status);
      const bool directory = fs::is_directory(target, ec);
      entry["exists"] = true;
      entry["link"] = link;
      entry["directory"] = directory;
      if (!directory && !link) {
        entry["size"] = static_cast<uint64_t>(fs::file_size(target, ec));
        if (ec) entry["size"] = 0;
      }
      const auto stamps = Timestamps(target);
      entry["modified"] = stamps.text;
      entry["modified_ms"] = stamps.ms;
    } catch (const std::exception& error) {
      // A vanished entry is the normal refresh case, not an error the UI shows.
      const std::string code = error.what();
      if (code != ops::kErrorNotFound) entry["error"] = code;
    }
    entries.push_back(std::move(entry));
  }
  return {{"ok", true}, {"entries", std::move(entries)}};
}

FileBrowser::Json FileBrowser::TempClearReply(const Json& message) {
  const auto requested = ExistingDirectory(RequestText(message, "directory"));
  std::error_code ec;
  const auto root = fs::canonical(PathFromUtf8(ReceiveDirectory()), ec);
  if (ec || root.empty() || !fs::is_directory(root, ec))
    throw std::runtime_error(std::string(ops::kErrorDirectoryUnavailable));
  // Only the receive directory itself or a folder inside it may be swept.
  if (!ops::ContainedIn(root, requested)) throw std::runtime_error("invalid_path");
  Json removed = Json::array();
  size_t failed = 0;
  for (const auto& directory : ops::SelectStagingDirectories(requested)) {
    std::error_code error;
    fs::remove_all(directory, error);
    if (error) { ++failed; continue; }
    removed.push_back(ops::Utf8Text(directory));
  }
  const auto count = removed.size();
  return {{"ok", true}, {"directory", ops::Utf8Text(requested)}, {"root", ops::Utf8Text(root)},
          {"removed", std::move(removed)}, {"removed_count", count}, {"failed", failed}};
}

// -------------------------------------- resume (control plane) ----
// "这个文件你已经有多少字节"：跨会话续传的控制面那一半。数据面（起始偏移、
// 放宽首块校验）不在这个文件里；这里只回答本地已经可信的前缀长度。
//
// 作用域与 temp_clear 完全一致：只有接收目录（及其子目录）能回答非零偏移。
// 越界的询问一律回 ok:true + offset:0，而不是报错 —— 报错本身就会泄露
// 「那边有没有东西」，而调用方拿到 0 的行为与「确实没有」完全相同。
FileBrowser::Json FileBrowser::ResumeReply(const Json& message) {
  // name 只允许单个路径分量：`..`、绝对路径、盘符、分隔符都由既有守卫
  // 拒成 invalid_path，因此这个 op 不可能读接收目录之外的任何东西。
  const auto name = EntryName(RequestText(message, "name"));
  uint64_t total_size = 0;
  if (!RequestUint(message, "total_size", &total_size))
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  // mtime_ms 可选：不带就是不校验时间戳（接收端在发布之前写记录时，手里
  // 还没有最终文件的时间戳，记录里的 0 表示「不知道」）。
  uint64_t requested_mtime = 0;
  const bool check_mtime = message.contains("mtime_ms");
  if (check_mtime && !RequestUint(message, "mtime_ms", &requested_mtime))
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));

  // 默认答案：从 0 开始。所有不可信的分支都走这里。
  Json reply{{"ok", true}, {"offset", 0}, {"total", total_size}};
  std::error_code ec;
  const auto root = fs::canonical(PathFromUtf8(ReceiveDirectory()), ec);
  if (ec || root.empty() || !fs::is_directory(root, ec)) return reply;
  fs::path directory = root;
  if (const auto requested = message.find("path"); requested != message.end()) {
    if (!requested->is_string())
      throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
    std::error_code path_error;
    const auto resolved =
        fs::canonical(PathFromUtf8(requested->get<std::string>()), path_error);
    // 解不开的路径（不存在、不是目录）就是「没有可续传的东西」，不是错误。
    if (path_error || resolved.empty() || !fs::is_directory(resolved, path_error))
      return reply;
    directory = resolved;
  }
  // 与 temp_clear 同一条守卫：接收目录之外一律 0。
  if (!ops::ContainedIn(root, directory)) return reply;
  std::string error;
  // The prefix bytes are the freshness proof: a record written before publish has
  // no final target to compare against.
  const auto record = file_transfer::ReadResumeRecordUnchecked(directory, name, &error);
  // 没有侧车 / malformed / unsupported_version / missing / stale 都在这里
  // 落成 0：侧车层的错误词表只用于日志，控制面只区分「能续」和「不能续」。
  if (!record) return reply;
  if (record->total_size != total_size) return reply;
  if (check_mtime) {
    if (record->target_mtime_ms < 0) return reply;
    if (static_cast<uint64_t>(record->target_mtime_ms) != requested_mtime) return reply;
  }
  // 已经收满（或超过）就没有可续的部分：从头开始才是唯一安全的答案。
  if (record->received >= total_size) return reply;
  // 前缀文件必须真实存在且长度与记录一致，对端才允许从 received 继续。
  const auto prefix_bytes = file_transfer::ResumePrefixBytes(directory, *record);
  if (!prefix_bytes || *prefix_bytes < record->received) return reply;
  // A stamped record must still describe the prefix that is on disk now.
  if (record->target_mtime_ms > 0 &&
      file_transfer::TargetMtimeMs(directory / record->target_path) !=
          record->target_mtime_ms)
    return reply;
  reply["offset"] = record->received;
  reply["total"] = record->total_size;
  return reply;
}

FileBrowser::Json FileBrowser::HostOpenReply(const Json& message, bool reveal) {
  const auto path = ExistingEntry(RequestText(message, "path"), true);
  return HostOpen(path, reveal);
}

// ------------------------------- transfer panel (phone clients) ----
// 手机端要的画板和桌面同一个传输管理器：控制器库不依赖 GUI，所以行/汇总/开关
// 都由宿主注入的 TransferControl 提供，这三个 op 只做校验、转发和取整。
// 全程不碰文件系统：transfer_list 里的路径是宿主给的，没有一条来自这里的解析。
// 传输队列的作用域永远是「请求从哪条连接来」，不是请求体里的字符串。客户端自带
// remote 时只做一致性校验：与连接不符就是 invalid_request，避免一条被授权的主控端
// 去列举或暂停另一个会话的队列。
std::string FileBrowser::TransferScope(const std::string& peer, const Json& message) {
  // 队列属于「这条连接」，不属于请求体：请求体里的 remote 只是一个提示，
  // 与连接不符也不会被采纳，因此一条被授权的主控端无法列举或暂停别的会话。
  (void)message;
  return peer;
}

FileBrowser::Json FileBrowser::TransferListReply(const std::string& peer, const Json& message) {
  const auto remote = TransferScope(peer, message);
  auto* control = Control();
  if (!control) return Refuse(ops::kErrorUnsupported);
  std::vector<TransferRowView> rows;
  TransferSummaryView summary;
  try {
    control->ListTransfers(remote, &rows, &summary);
  } catch (const std::exception&) {
    // 词表里没有 internal 码：宿主自己出问题时按「这条 op 提供不了」回答，
    // 而不是把宿主或 nlohmann 的异常文本漏给客户端。
    return Refuse(ops::kErrorUnsupported);
  }
  Json list = Json::array();
  for (const auto& row : rows)
    list.push_back(Json{{"id", row.id}, {"name", row.name}, {"path", row.path},
                        {"size", row.size}, {"sent", row.sent}, {"rate", row.rate},
                        {"state", row.state}, {"paused", row.paused},
                        {"retryable", row.retryable}, {"receiving", row.receiving}});
  return {{"ok", true},
          {"rows", std::move(list)},
          {"summary", Json{{"total", summary.total},
                           {"completed", summary.completed},
                           {"failed", summary.failed},
                           {"active", summary.active},
                           {"queued", summary.queued},
                           {"paused", summary.paused},
                           {"batch_partial", summary.batch_partial}}}};
}

FileBrowser::Json FileBrowser::TransferControlReply(const std::string& peer, const Json& message) {
  // 动作名来自闭合词表：不是八个已知字符串就是 invalid_request，而且这个判定
  // 先于控件探测，所以坏请求在任何宿主上都是同一个答案。
  const auto found = message.find("action");
  if (found == message.end() || !found->is_string())
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  const auto action_text = found->get<std::string>();
  const std::string_view action = action_text;
  if (!ops::IsTransferAction(action)) throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  // 队列属于「这条连接」，不属于请求体：客户端自带 remote 会变成越权入口，
  // 所以只用 Serve() 传下来的 peer，请求体里的 remote 只做一致性校验。
  const auto remote = TransferScope(peer, message);
  // 逐项动作必须带一个正整数 file_id；批量动作忽略它。
  uint32_t id = 0;
  if (ops::IsTransferItemAction(action)) {
    // The web client's request() owns the envelope `id` (it is the RPC sequence
    // number), so a per-file id travels as `file_id` — the same key prepare and
    // cancel already use. Accept `id` too for hand-written callers.
    // id is the RPC envelope sequence number, so the per-file id cannot ride on
    // it: the web client's request() overwrites it. Accept only ile_id, the key
    // prepare/cancel already use, and reject anything else as invalid_request.
    const auto field = message.find("file_id");
    if (field == message.end()) throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
    if (!field->is_number_unsigned() && !field->is_number_integer())
      throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
    const auto candidate = field->get<int64_t>();
    if (candidate <= 0 || static_cast<uint64_t>(candidate) > UINT32_MAX)
      throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
    id = static_cast<uint32_t>(candidate);
  }
  auto* control = Control();
  if (!control) return Refuse(ops::kErrorUnsupported);
  bool applied = false;
  try {
    if (action == ops::kTransferCancel) applied = control->Cancel(remote, id);
    else if (action == ops::kTransferPause) applied = control->Pause(remote, id);
    else if (action == ops::kTransferResume) applied = control->Resume(remote, id);
    else if (action == ops::kTransferRetry) applied = control->Retry(remote, id);
    else if (action == ops::kTransferStartAll) applied = control->StartAll(remote);
    else if (action == ops::kTransferPauseAll) applied = control->PauseAll(remote);
    else if (action == ops::kTransferResumeAll) applied = control->ResumeAll(remote);
    else if (action == ops::kTransferClearAll) applied = control->ClearAll(remote);
    else applied = control->RemoveFinished(remote);
  } catch (const std::exception&) {
    return Refuse(ops::kErrorUnsupported);
  }
  // ok 说的是「请求合法且送到了宿主」，applied 是宿主自己的答案：
  // ok:true + applied:false 就是「这次没有可做的东西」，不是错误。
  return {{"ok", true}, {"action", action_text}, {"applied", applied}};
}

FileBrowser::Json FileBrowser::TransferSettingsReply(const std::string& peer, const Json& message) {
  // 字段集合是闭合的：信封字段之外只认这三个键。拼错字段名不会被当成 GET
  // 悄悄放过，也不会被当成一个没人认领的 SET。
  const auto known = [](const std::string& key) {
    return key == "type" || key == "version" || key == "id" || key == "op" ||
           key == "transfer_id" || key == "rate_kbps" || key == "overwrite_policy" ||
           key == "concurrency";
  };
  for (const auto& field : message.items())
    if (!known(field.key())) throw std::runtime_error(std::string(ops::kErrorInvalidRequest));

  const bool has_rate = message.contains("rate_kbps");
  const bool has_policy = message.contains("overwrite_policy");
  const bool has_concurrency = message.contains("concurrency");
  // 类型和范围都在这里挡：字符串形式的数字、负数、浮点、越界一律
  // invalid_request，绝不静默夹一下就当成功。
  uint64_t rate = 0, policy = 0, concurrency = 0;
  if (has_rate && (!RequestUint(message, "rate_kbps", &rate) ||
                   rate > static_cast<uint64_t>(ops::kTransferRateKbpsMax)))
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  if (has_policy && (!RequestUint(message, "overwrite_policy", &policy) ||
                     policy > static_cast<uint64_t>(ops::kTransferOverwritePolicyMax)))
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));
  if (has_concurrency &&
      (!RequestUint(message, "concurrency", &concurrency) ||
       concurrency < static_cast<uint64_t>(ops::kTransferConcurrencyMin) ||
       concurrency > static_cast<uint64_t>(ops::kTransferConcurrencyMax)))
    throw std::runtime_error(std::string(ops::kErrorInvalidRequest));

  auto* control = Control();
  if (!control) return Refuse(ops::kErrorUnsupported);
  try {
    if (has_rate) control->SetRateKbps(static_cast<int>(rate));
    if (has_policy) control->SetOverwritePolicy(static_cast<int>(policy));
    if (has_concurrency) control->SetConcurrency(static_cast<int>(concurrency));
    // 应答里永远是宿主生效后的值，而且再夹一次：就算宿主回了一个越界配置，
    // 手机端读到的仍然是合法区间内的数。
    const int effective_rate =
        Clamped(control->RateKbps(), ops::kTransferRateKbpsMin, ops::kTransferRateKbpsMax);
    const int effective_policy = Clamped(control->OverwritePolicy(),
                                         ops::kTransferOverwritePolicyMin,
                                         ops::kTransferOverwritePolicyMax);
    const int effective_concurrency = Clamped(control->Concurrency(),
                                              ops::kTransferConcurrencyMin,
                                              ops::kTransferConcurrencyMax);
    return {{"ok", true},
            {"rate_kbps", effective_rate},
            {"overwrite_policy", effective_policy},
            {"concurrency", effective_concurrency}};
  } catch (const std::exception&) {
    return Refuse(ops::kErrorUnsupported);
  }
}

void FileBrowser::SetReceiveDirectory(const std::string& path) {
  std::lock_guard lock(mutex_);
  receive_directory_ = path;
}
std::string FileBrowser::ReceiveDirectory() {
  std::lock_guard lock(mutex_);
  if (!receive_directory_.empty()) return receive_directory_;
  // Unconfigured: the transfer engine stages into its own "." directory, which
  // resolves against the process working directory.
  std::error_code ec;
  const auto cwd = fs::current_path(ec);
  return ec ? std::string{} : Utf8(cwd);
}

void FileBrowser::SetTransferControl(TransferControl* control) {
  std::lock_guard lock(mutex_);
  transfer_control_ = control;
}

// 取指针本身要锁（宿主可能随时换控件），回调控件本身必须在锁外，否则 GUI 那
// 边回调进来会和 Post() 抢同一把锁。指针的生命周期由宿主负责，取到之后它必须
// 比这次调用活得久 —— 这也是注入点注释里那条约定的来源。
TransferControl* FileBrowser::Control() {
  std::lock_guard lock(mutex_);
  return transfer_control_;
}

void FileBrowser::Serve(const std::string& peer, const Json& message) {
  Json result;
  try {
    const auto op = Text(message, "op");
    if (op == "list") result = DirectoryPage(peer, message);
    else if (op == "gallery") result = GalleryPage(peer, message);
    else if (op == "thumb") result = ThumbnailReply(message);
    else if (op == "list_cancel") {
      const auto [token, _] = ParseCursor(Text(message, "cursor"));
      directory_snapshots_.erase({peer, token});
      result = {{"ok", true}};
    }
    else if (op == "prepare") {
      auto root = ExistingDirectory(Text(message, "destination"));
      const auto& items = message.at("items");
      if (!items.is_array() || items.empty() || items.size() > 64) throw std::runtime_error("invalid_request");
      for (const auto& item : items) {
        if (!Current(peer, active_generation_)) return;
        if (!item.at("file_id").is_number_unsigned()) throw std::runtime_error("invalid_request");
        const auto id = item.at("file_id").get<uint64_t>();
        if (id > UINT32_MAX) throw std::runtime_error("invalid_request");
        auto dir = Destination(root, Text(item, "directory"));
        if (id && !reserve_(peer, static_cast<uint32_t>(id), dir)) throw std::runtime_error("directory_not_writable");
      }
      result = {{"ok", true}};
    } else if (op == "cancel") {
      const auto& id = message.at("file_id");
      if (!id.is_number_unsigned() || id.get<uint64_t>() > UINT32_MAX || !id.get<uint32_t>())
        throw std::runtime_error("invalid_request");
      if (!cancel_ || !cancel_(peer, id.get<uint32_t>())) throw std::runtime_error("already_finished");
      result = {{"ok", true}, {"message", "cancelling"}};
    } else if (op == "copy") {
      const auto& paths = message.at("paths");
      if (!paths.is_array() || paths.empty() || paths.size() > 256) throw std::runtime_error("invalid_request");
      std::vector<std::string> sources;
      for (const auto& path : paths) sources.push_back(Text(Json{{"path", path}}, "path"));
      Upload(peer, sources, Text(message, "destination"), true,
             message.contains("transfer_id") ? Text(message, "transfer_id") : "");
      result = {{"ok", true}, {"message", "queued"}};
    }
    // File management ops: same envelope, one more JSON op value, no new wire
    // message. See file_browser.h for the request and reply fields.
    else if (op == ops::kOpMkdir) result = MkdirReply(message);
    else if (op == ops::kOpRename) result = RenameReply(message);
    else if (op == ops::kOpDelete) result = DeleteReply(message);
    else if (op == ops::kOpStat) result = StatReply(message);
    else if (op == ops::kOpTempClear) result = TempClearReply(message);
    // 续传控制面：同一个信封，没有新的线消息。
    else if (op == ops::kOpResume) result = ResumeReply(message);
    else if (op == ops::kOpOpen) result = HostOpenReply(message, false);
    else if (op == ops::kOpReveal) result = HostOpenReply(message, true);
    // 手机端传输面板：同一个信封、同一条 error 词表，没有新的线消息，也没有
    // 新的线布局。三条 op 都只是宿主 TransferControl 的转发（见 file_browser.h）。
    else if (op == ops::kOpTransferList) result = TransferListReply(peer, message);
    else if (op == ops::kOpTransferControl) result = TransferControlReply(peer, message);
    else if (op == ops::kOpTransferSettings) result = TransferSettingsReply(peer, message);
    else throw std::runtime_error("unsupported");
  } catch (const std::exception& error) { result = Failure(error.what()); }
  if (message.contains("transfer_id")) result["transfer_id"] = Text(message, "transfer_id");
  result["op"] = "reply"; result["id"] = message.at("id");
  SendMessage(peer, std::move(result));
}

void FileBrowser::Upload(const std::string& peer, const std::vector<std::string>& paths,
                         const std::string& destination, bool notify_peer, const std::string& transfer_id) {
  std::vector<UploadItem> files;
  try {
    if (paths.empty() || paths.size() > 256 || destination.empty()) throw std::runtime_error("invalid_request");
    std::set<fs::path> visited;
    const auto add = [&](const fs::path& path, const fs::path& relative) {
      if (!visited.insert(path).second) return;
      if (files.size() >= 4096) throw std::runtime_error("too_many_files");
      const auto status = fs::symlink_status(path);
      if (fs::is_symlink(status)) throw std::runtime_error("symbolic_link");
      if (fs::is_directory(status)) files.push_back({0, "", Utf8(relative)});
      else if (fs::is_regular_file(status)) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("file_unreadable");
        files.push_back({file_transfer::Session::NextFileId(), Utf8(path), Utf8(relative.parent_path())});
      } else throw std::runtime_error("unsupported_file");
    };
    // Validate the entire selection before creating remote folders or sending.
    for (const auto& source : paths) {
      auto path = PathFromUtf8(source);
      if (!path.is_absolute() || path.filename().empty()) throw std::runtime_error("invalid_path");
      add(path, path.filename());
      if (fs::is_directory(fs::symlink_status(path))) {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
          { std::lock_guard lock(mutex_); if (stopping_ || generations_[peer] != active_generation_) return; }
          add(entry.path(), entry.path().lexically_relative(path.parent_path()));
        }
      }
    }
    auto job = std::make_shared<UploadJob>();
    job->files = std::move(files); job->destination = destination; job->notify_peer = notify_peer;
    job->transfer_id = transfer_id;
    UploadNext(peer, job);
  } catch (const std::exception& error) {
    Publish(peer, 0, Failure(error.what()));
    throw;
  }
}

void FileBrowser::UploadNext(const std::string& peer, const std::shared_ptr<UploadJob>& job) {
  if (!Current(peer, active_generation_)) return;
  if (job->next == job->files.size()) {
    Report(peer, {{"ok", true}, {"message", "queued"}}, job->notify_peer, job->transfer_id);
    return;
  }
  const auto start = job->next;
  Json items = Json::array();
  size_t bytes = job->destination.size();
  while (job->next < job->files.size() && items.size() < 32) {
    const auto& file = job->files[job->next];
    Json item{{"file_id", file.id}, {"directory", file.directory}};
    const auto size = item.dump().size();
    if (!items.empty() && bytes + size > 44000) break;
    bytes += size; items.push_back(std::move(item)); ++job->next;
  }
  const auto end = job->next;
  // Only one preparation batch per operation is in flight. Large folder
  // copies do not exhaust pending requests or reserve thousands of paths.
  Request(peer, {{"op", "prepare"}, {"destination", job->destination}, {"items", items},
                 {"transfer_id", job->transfer_id}},
      [this, peer, job, start, end](const Json& reply) {
        if (!reply.value("ok", false)) { Report(peer, reply, job->notify_peer, job->transfer_id); return; }
        for (auto index = start; index < end; ++index) {
          if (!Current(peer, active_generation_)) return;
          const auto& file = job->files[index];
          if (file.id && !enqueue_(peer, file.path, file.id)) {
            Report(peer, Failure("file_unreadable"), job->notify_peer, job->transfer_id); return;
          }
        }
        UploadNext(peer, job);
      });
}

}  // namespace crossdesk
