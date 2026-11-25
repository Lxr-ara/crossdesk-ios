/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FILE_BROWSER_H_
#define _FILE_BROWSER_H_

#include <nlohmann/json.hpp>

#include "file_browser_ops.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace crossdesk {

// Re-exported so the adapter and the ops test reach the path rules through this
// header; the definitions live in file_browser_ops.h next to the op table.
using file_browser_ops::ContainedIn;
using file_browser_ops::IsStagingDirectoryName;
using file_browser_ops::PathError;
using file_browser_ops::SafeChildPath;
using file_browser_ops::SelectStagingDirectories;

// Filesystem work is serialized off the UI and RTC threads. The adapter owns
// connection authorization and the existing reliable file transfer engine.
//
// ---------------------------------------------------------------- wire ops --
// Every op below travels in the existing version 1 envelope; the reply is always
//   {"type":"file_browser","version":1,"id":<echo>,"op":"reply","ok":bool,
//    "error":"<code>"}                       <code> from file_browser_ops.h
// and unknown paths are rejected with "invalid_path" by the same validator the
// listing and prepare ops use (SafeChildPath).
//
// existing
//   list        { path, cursor?, pagination? }  -> { path, parent, entries, offset, total, next, truncated }
//   gallery     { path, recursive?, cursor? }   -> same page shape with image entries
//   thumb       { path, edge }                  -> { path, data(base64), width, height, source_* }
//   list_cancel { cursor }                      -> { ok }
//   prepare     { destination, items[] }        -> { ok } | directory_not_writable
//   cancel      { file_id }                     -> { message } | already_finished
//   copy        { paths[], destination, transfer_id? } -> { message:"queued" }
//
// file management (added; UU 远程 parity)
//   mkdir      { path, name }      -> { path }              errors: invalid_path, invalid_request,
//                                                          exists, not_writable
//   rename     { path, name }      -> { path }              errors: invalid_path, invalid_request,
//                                                          not_found, exists, not_writable,
//                                                          symbolic_link
//   delete     { paths[] }         -> { results:[{path, ok, error}], removed }
//                                  errors: invalid_request, too_many_files
//   open       { path }            -> { path, opened }       errors: invalid_path, not_found,
//                                                          file_unreadable, unsupported
//   reveal     { path }            -> { path, revealed }     errors: invalid_path, not_found,
//                                                          file_unreadable, unsupported
//   stat       { paths[] }         -> { entries:[{path, exists, directory, size,
//                                  modified, modified_ms, link, error}] }
//                                  errors: invalid_request, too_many_files
//   temp_clear { directory }       -> { directory, root, removed:[path], removed_count, failed }
//                                  errors: invalid_request, invalid_path,
//                                  directory_unavailable
//   resume     { path?, name, total_size, mtime_ms? }
//                                -> { offset, total }
//                                  control plane half of cross-session resume,
//                                  i.e. "how many bytes of this file do you
//                                  already have". ok is always true: a client
//                                  that gets offset 0 simply restarts at 0.
//                                  `path` names a directory inside the receive
//                                  root (default: the root itself); a path that
//                                  resolves anywhere else - and a message with no
//                                  receive root at all - answers offset:0 instead
//                                  of an error, so the op never reports anything
//                                  about the rest of the filesystem.
//                                  errors: invalid_request, invalid_path
//
// resume freshness (a non-zero offset needs all four):
//   1. the sidecar for `name` parses and its target still sits in the same
//      directory with the recorded size and stamp (file_transfer_resume.h);
//   2. record.total_size == the request's total_size;
//   3. the request's mtime_ms, when it carries one, equals the record's stamp;
//   4. offset < total_size.
// No sidecar, malformed, unsupported_version, missing, stale, a mismatched
// size or stamp and a prefix that already reaches the end all answer 0.
//
// transfer panel (added; the phone clients draw the same window the desktop has)
//   transfer_list    { remote? }
//     -> { rows:[{id, name, path, size, sent, rate, state, paused, retryable,
//                 receiving}],
//          summary:{total, completed, failed, active, queued, paused,
//                   batch_partial} }
//        errors: invalid_request, unsupported
//        Rows and summary are the host's own, passed through unchanged: the op
//        never touches the filesystem, so no path in the reply is a probe
//        result. `remote` is the opaque peer id the host handed out; absent or
//        empty means the host's default scope.
//   transfer_control { action, id?, remote? }
//     -> { action, applied }        errors: invalid_request, unsupported
//        `action` is closed to kTransferActions in file_browser_ops.h
//        (pause/resume/retry/start_all/pause_all/resume_all/clear_all/
//        remove_finished). `id` is mandatory and must be a positive integer for
//        pause/resume/retry, and is ignored by the five batch actions.
//        ok:true means "well formed and handed to the host"; `applied` is the
//        host's own answer, so ok:true with applied:false is "nothing to do".
//   transfer_settings { rate_kbps?, overwrite_policy?, concurrency? }
//     -> { rate_kbps, overwrite_policy, concurrency }
//        errors: invalid_request, unsupported
//        No field present is a GET and reports the effective values. Any field
//        present is a SET: it is applied and the reply carries what the host
//        reports afterwards, clamped to the ranges below. Unknown fields, a
//        non-numeric type and an out-of-range value are invalid_request - a
//        "12" string is not a number that is nearly right.
//        Ranges: rate_kbps 0..1048576, overwrite_policy 0..2, concurrency 1..4
//        (kTransfer*Min/Max in file_browser_ops.h). rate_kbps is KiB/s like
//        config_center's transfer_rate_limit_kbps, overwrite_policy is
//        transfer_overwrite_policy (0 ask/skip, 1 overwrite, 2 rename) and
//        concurrency is the per-peer outbound fan-out
//        (FileTransferManager::kMaxConcurrentPerPeer).
//
// The three ops answer "unsupported" while no TransferControl is injected, which
// is exactly what an old host that never calls SetTransferControl keeps doing.

// --------------------------------------------------------- transfer panel data --
// One transfer row as the phone panel draws it. The authoritative state lives in
// the desktop GUI (FileTransferManager::TransferRows / BatchSummary), and the
// controller library must not depend on the GUI, so only the shape is declared
// here and the host feeds it through TransferControl below.
struct TransferRowView {
  uint32_t id = 0;
  std::string name;
  // UTF-8 absolute path exactly as the host's queue holds it. The ops neither
  // parse nor validate it.
  std::string path;
  uint64_t size = 0;
  uint64_t sent = 0;
  // Bytes per second.
  uint32_t rate = 0;
  // One of kTransferStateNames in file_browser_ops.h.
  std::string state = "queued";
  bool paused = false;
  bool retryable = false;
  bool receiving = false;
};

struct TransferSummaryView {
  uint64_t total = 0, completed = 0, failed = 0, active = 0, queued = 0, paused = 0;
  bool batch_partial = false;
};

// Injection point for the three ops above. The desktop already owns every
// behaviour (start/pause/resume/clear all, per item pause/resume/retry, the batch
// summary and the two config keys); wrapping it in this interface is the only
// thing the GUI has to add, and the controller stays free of GUI headers.
//
// Threading: every call arrives on the browser's worker thread (the same thread
// the other ops run on, never the UI or RTC thread), so an implementation guards
// its own state. The object must outlive the browser, or the injection point has
// to be cleared (SetTransferControl(nullptr)) before it dies.
class TransferControl {
 public:
  virtual ~TransferControl() = default;
  // Fills one peer's rows and batch counters. `remote` empty means the host's
  // own default scope.
  virtual void ListTransfers(const std::string& remote, std::vector<TransferRowView>* rows,
                             TransferSummaryView* summary) = 0;
  // Per item: true when the call changed something.
  virtual bool Pause(const std::string& remote, uint32_t id) = 0;
  virtual bool Resume(const std::string& remote, uint32_t id) = 0;
  virtual bool Retry(const std::string& remote, uint32_t id) = 0;
  // 手机端逐行「取消」：与 op:"cancel" 走同一条引擎路径。
  virtual bool Cancel(const std::string& remote, uint32_t id) = 0;
  // Queue level: start / hold / release the whole batch, drop waiting and
  // finished rows, drop finished rows only.
  virtual bool StartAll(const std::string& remote) = 0;
  virtual bool PauseAll(const std::string& remote) = 0;
  virtual bool ResumeAll(const std::string& remote) = 0;
  virtual bool ClearAll(const std::string& remote) = 0;
  virtual bool RemoveFinished(const std::string& remote) = 0;
  // Settings, global (no peer): the outbound cap in KiB/s, the receive conflict
  // policy and the per-peer fan-out.
  virtual void SetRateKbps(int kbps) = 0;
  virtual int RateKbps() = 0;
  virtual void SetOverwritePolicy(int policy) = 0;
  virtual int OverwritePolicy() = 0;
  virtual void SetConcurrency(int limit) = 0;
  virtual int Concurrency() = 0;
};

class FileBrowser {
 public:
  using Json = nlohmann::json;
  using Send = std::function<bool(const std::string&, const Json&)>;
  using Enqueue = std::function<bool(const std::string&, const std::string&, uint32_t)>;
  using Reserve = std::function<bool(const std::string&, uint32_t, const std::filesystem::path&)>;
  using Cancel = std::function<bool(const std::string&, uint32_t)>;
  struct Event {
    uint32_t id;
    Json result;
    // Empty for the dual pane browser, "gallery" for photo album traffic.
    std::string channel;
  };
  FileBrowser(Send send, Enqueue enqueue, Reserve reserve, Cancel cancel = {});
  ~FileBrowser();
  uint32_t List(const std::string& peer, const std::string& path, bool local);
  // Photo album listing. Same pagination and cancellation contract as List,
  // but the peer answers with image entries plus the album each photo belongs
  // to. Pass "@photos" for the peer's Pictures folder.
  uint32_t Gallery(const std::string& peer, const std::string& path, bool recursive);
  // Requests one thumbnail over the control channel. The encoded JPEG arrives
  // through TakeEvents as a result with "kind" == "thumb".
  void Thumbnail(const std::string& peer, const std::string& path, int edge);
  void CancelList(const std::string& peer, uint32_t id);
  void Copy(const std::string& peer, const std::vector<std::string>& paths,
            const std::string& destination, bool download);
  void CancelTransfer(const std::string& peer, uint32_t id);
  // Client side of the file management ops (mkdir / rename / delete / stat /
  // open / reveal): one request, one reply through TakeEvents(). `body` carries
  // "op" plus the fields documented at the top of this header, and the id is
  // stamped here and echoed on the event, so a caller matches reply to request
  // without knowing the wire id. Returns the request id, or 0 when the request
  // could not be queued (the browser is stopping, or the job queue is full).
  uint32_t Op(const std::string& peer, Json body);
  // Directory the receive engine stages transfers in; temp_clear may only touch
  // ".crossdesk-*" staging directories inside it. Empty keeps the engine's own
  // default, the process current directory, which is where an unconfigured
  // Session stages files.
  void SetReceiveDirectory(const std::string& path);
  std::string ReceiveDirectory();
  // 手机端传输面板的注入点。宿主（桌面 GUI）把自己的传输管理器包一层
  // TransferControl 传进来；nullptr 回到「没有这个能力」，三个 op 一律回答
  // unsupported，老宿主不受影响。指针由调用方持有，必须在浏览器析构前置空，
  // 或者保证比浏览器活得久；回调发生在 worker 线程上。
  void SetTransferControl(TransferControl* control);
  void Handle(const std::string& peer, Json message);
  void Disconnect(const std::string& peer);
  uint64_t Generation(const std::string& peer);
  // Without a channel only the dual pane browser's events are returned; photo
  // album results stay queued for the gallery window.
  std::vector<Event> TakeEvents(const std::string& peer);
  std::vector<Event> TakeEvents(const std::string& peer, const std::string& channel);
  void Stop();
  static Json Directory(const std::string& path, const std::function<bool()>& cancelled = {});

 private:
  using Clock = std::chrono::steady_clock;
  struct Pending {
    std::string peer;
    uint64_t generation;
    Clock::time_point deadline;
    std::function<void(const Json&)> complete;
  };
  struct Listing {
    uint32_t id;
    uint64_t generation;
    std::string peer, path, cursor;
    // "list" for the dual pane browser, "gallery" for the photo albums.
    std::string op = "list";
    bool recursive = false;
    Json result;
    std::atomic<bool> cancelled{false};
  };
  struct DirectorySnapshot {
    Json listing;
    uint64_t generation;
    Clock::time_point expires;
  };
  struct UploadItem { uint32_t id; std::string path, directory; };
  struct UploadJob {
    std::vector<UploadItem> files;
    size_t next = 0;
    std::string destination;
    bool notify_peer = false;
    std::string transfer_id;
  };
  void Post(const std::string& peer, std::function<void()> work);
  void Run();
  void Request(const std::string& peer, Json message,
               std::function<void(const Json&)> complete);
  void Publish(const std::string& peer, uint32_t id, Json result,
               const std::string& channel = {});
  void Serve(const std::string& peer, const Json& message);
  void ListPage(const std::shared_ptr<Listing>& listing);
  void FinishList(const std::shared_ptr<Listing>& listing, Json result);
  void ReleaseList(const std::string& peer, const std::string& cursor);
  Json DirectoryPage(const std::string& peer, const Json& message);
  Json GalleryPage(const std::string& peer, const Json& message);
  Json GalleryListing(const std::string& path, bool recursive,
                      const std::function<bool()>& cancelled) const;
  Json ThumbnailReply(const Json& message);
  // File management replies. Each returns the reply body for Serve(), which
  // adds the op:"reply" envelope and the id echo.
  Json MkdirReply(const Json& message);
  Json RenameReply(const Json& message);
  Json DeleteReply(const Json& message);
  Json StatReply(const Json& message);
  Json TempClearReply(const Json& message);
  // 跨会话续传的控制面；请求/应答与新鲜度判定见文件头的 op 注释块。
  Json ResumeReply(const Json& message);
  // 手机端传输面板。三条都只做校验和转发，数据全部来自注入的 TransferControl；
  // 字段与错误码见文件头的 transfer panel 注释块。
  // 返回本次请求的传输作用域：永远是连接 id，请求体只能与之一致。
  std::string TransferScope(const std::string& peer, const Json& message);
  Json TransferListReply(const std::string& peer, const Json& message);
  Json TransferControlReply(const std::string& peer, const Json& message);
  Json TransferSettingsReply(const std::string& peer, const Json& message);
  // open/reveal share one reply; `reveal` selects the file inside its folder.
  Json HostOpenReply(const Json& message, bool reveal);
  void ListPageNext(const std::shared_ptr<Listing>& listing);
  void Upload(const std::string& peer, const std::vector<std::string>& paths,
              const std::string& destination, bool notify_peer = false,
              const std::string& transfer_id = {});
  void UploadNext(const std::string& peer, const std::shared_ptr<UploadJob>& job);
  bool Current(const std::string& peer, uint64_t generation);
  void Report(const std::string& peer, Json result, bool notify_peer,
              const std::string& transfer_id = {});
  bool SendMessage(const std::string& peer, Json message);
  // 读一次注入点：返回的指针只在本次调用内有效。
  TransferControl* Control();
  Send send_;
  Enqueue enqueue_;
  Reserve reserve_;
  Cancel cancel_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::deque<std::function<void()>> jobs_;
  std::map<std::string, uint64_t> generations_;
  std::map<std::string, std::vector<Event>> events_;
  std::map<uint32_t, std::shared_ptr<Listing>> listings_;
  // Guarded by mutex_; empty means the receive engine's current directory.
  std::string receive_directory_;
  // 宿主注入的传输面板控件。同样受 mutex_ 保护：取指针要加锁，调用控件本身
  // 必须在锁外（GUI 那边回调进来会和 Post() 抢同一把锁）。nullptr 表示这个
  // 宿主没有传输面板能力。
  TransferControl* transfer_control_ = nullptr;
  // Worker-thread state only.
  std::map<uint32_t, Pending> pending_;
  std::map<std::pair<std::string, std::string>, DirectorySnapshot> directory_snapshots_;
  // Bounded thumbnail cache: key is "<path>|<mtime_ms>|<edge>".
  struct ThumbEntry {
    std::string key, data;
    int width = 0, height = 0, source_width = 0, source_height = 0;
    uint64_t used = 0;
  };
  std::map<std::string, ThumbEntry> thumbnails_;
  uint64_t thumb_clock_ = 0;
  uint64_t next_snapshot_ = 1;
  std::string active_peer_;
  uint64_t active_generation_ = 0;
  std::atomic<uint32_t> next_id_{1};
  std::thread worker_;
};

}  // namespace crossdesk

#endif
