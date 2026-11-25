#include "gui/features/file_transfer/conflict_dialog_host.h"
#include "features/file_transfer/file_transfer_manager.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <thread>

#include "filesystem_utf8.h"
#include "localization.h"
#include "rd_log.h"
#include "runtime/gui_runtime.h"

#include <photo_library.h>

namespace crossdesk {

namespace {

// 读续传应答 JSON 的四个小工具。对端的 reply 是不可信输入：字段缺失、类型
// 不对、负数都当成「没有」，一律不抛异常，最坏情况就是从 0 发送。
uint64_t JsonByteCount(const FileBrowser::Json& message, const char* key) {
  const auto found = message.find(key);
  if (found == message.end()) return 0;
  if (found->is_number_unsigned()) return found->get<uint64_t>();
  if (found->is_number_integer()) {
    const auto value = found->get<int64_t>();
    return value > 0 ? static_cast<uint64_t>(value) : 0;
  }
  return 0;
}

uint32_t JsonRequestId(const FileBrowser::Json& message) {
  const auto value = JsonByteCount(message, "id");
  return value <= 0xFFFFFFFFull ? static_cast<uint32_t>(value) : 0;
}

bool JsonFlag(const FileBrowser::Json& message, const char* key) {
  const auto found = message.find(key);
  return found != message.end() && found->is_boolean() && found->get<bool>();
}

std::string JsonTextField(const FileBrowser::Json& message, const char* key) {
  const auto found = message.find(key);
  return found != message.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string();
}

}  // namespace

FileTransferManager::FileTransferManager(GuiRuntime& owner)
    : owner_(owner), browser_(
        [this](const auto& remote, const auto& message) { return SendBrowser(remote, message); },
        [this](const auto& remote, const auto& path, uint32_t id) {
          if (!BrowserConnected(remote)) return false;
          auto props = owner_.FindRemoteSession(remote);
          return ProcessSelectedFile(path, props, owner_.file_label_, remote, id);
        },
        [this](const auto& remote, uint32_t id, const auto& directory) {
          if (!BrowserConnected(remote)) return false;
          std::shared_ptr<PeerEventHandler> events;
          { std::lock_guard lock(browser_mutex_); events = browser_peers_[remote]; }
          const auto receiver = ReceiverFor(events.get(), remote);
          return receiver && receiver->ReserveDirectory(id, directory);
        },
        [this](const auto& remote, uint32_t id) {
          if (!BrowserConnected(remote)) return false;
          for (const auto& info : BrowserTransfers(remote))
            if (!info.receiving && info.file_id == id) return CancelSend(id);
          return false;
        }) {}
FileTransferManager::~FileTransferManager() { Shutdown(); }


void FileTransferManager::Shutdown() {
  browser_.Stop();
  // 先让还阻塞在冲突询问上的接收线程拿着「跳过」走人：它们的会话随后会被
  // Cancel，谁都不必等一个已经没人看的窗口。
  conflict_dialogs_.Shutdown();
  std::vector<std::future<void>> workers;
  {
    std::lock_guard lock(workers_mutex_);
    stopping_ = true;
    workers.swap(workers_);
  }
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, sender] : senders_) sender.session->Cancel();
    for (auto& [handler, peers] : receivers_)
      for (auto& [id, receiver] : peers) receiver.session->Cancel();
    receivers_.clear();
    // Nothing is left to hold once every session is cancelled.
    paused_remotes_.clear();
  }
  {
    std::lock_guard lock(queue_mutex_);
    send_queue_.Clear();
    sources_.clear();
  }
  wake_.notify_all();
  for (auto& worker : workers) worker.wait();
}

FileTransferManager::FileTransferState& FileTransferManager::global_state() { return global_state_; }
FileTransferManager::FileTransferState& FileTransferManager::received_state() { return received_state_; }
FileTransferManager::FileTransferState& FileTransferManager::state_for(const std::shared_ptr<RemoteSession>& props) {
  return props ? props->file_transfer_ : global_state_;
}

bool FileTransferManager::ProcessSelectedFile(const std::string& path,
    const std::shared_ptr<RemoteSession>& props, const std::string& file_label,
    const std::string& remote_id, uint32_t file_id) {
  if (path.empty() || stopping_ || (props && props->closing_)) return false;
  const auto file_path = PathFromUtf8(path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file_path, ec)) return false;
  const auto size = std::filesystem::file_size(file_path, ec);
  if (ec) return false;
  auto& state = state_for(props);
  // Allocate once at enqueue time so queued and active notifications keep the
  // same identity, including repeated sends of the same path.
  const auto id = file_id ? file_id : file_transfer::Session::NextFileId();
  const auto name = file_path.filename().u8string();
  const std::string display_name(name.begin(), name.end());
  const auto peer_id = props ? props->remote_id_ : remote_id;
  {
    // The queue board owns the control flags; the row published below is its
    // rendering mirror, so both are written from here, once.
    std::lock_guard lock(queue_mutex_);
    if (!send_queue_.Track(id, display_name, peer_id, size)) return false;
    sources_[id] = Source{file_path, file_label};
  }
  PublishQueuedRow(state, id, display_name, peer_id, file_path, size);
  state.file_transfer_window_visible_ = true;
  EnqueueWaiting(state, file_path, file_label, peer_id, id);
  ProcessQueue(props);
  return true;
}

void FileTransferManager::PublishQueuedRow(FileTransferState& state, uint32_t id,
    const std::string& name, const std::string& remote_id,
    const std::filesystem::path& path, uint64_t size) {
  std::lock_guard lock(state.file_transfer_list_mutex_);
  FileTransferState::FileTransferInfo info;
  info.file_name = name;
  info.remote_id = remote_id;
  info.file_path = path;
  info.file_size = size;
  info.file_id = id;
  info.status = FileTransferState::FileTransferStatus::Queued;
  state.file_transfer_list_.push_back(std::move(info));
}

void FileTransferManager::EnqueueWaiting(FileTransferState& state,
    const std::filesystem::path& path, const std::string& label,
    const std::string& remote_id, uint32_t id) {
  std::lock_guard lock(state.file_queue_mutex_);
  state.file_send_queue_.push({path, label, remote_id, id});
}

void FileTransferManager::ProcessQueue(std::shared_ptr<RemoteSession> props) {
  std::lock_guard workers_lock(workers_mutex_);
  LaunchQueued(props);
}

// Fills every free slot of this state's peers. A peer never runs more than
// ConcurrentTransfersPerPeer() files, so the queue, not the session, is where a
// third file waits; a held or cancelled item never takes a slot, which keeps the
// line moving around a paused item.
void FileTransferManager::LaunchQueued(std::shared_ptr<RemoteSession> props) {
  if (stopping_) return;
  auto& state = state_for(props);
  std::erase_if(workers_, [](auto& worker) {
    return worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
  const auto limit = ConcurrentLimit();
  for (;;) {
    FileTransferState::QueuedFile file;
    if (!TakeStartable(state, limit, &file)) break;
    workers_.push_back(std::async(std::launch::async, [this, props, file] {
      RunFile(props, file);
      // The slot is released inside RunFile; refill from the waiting queue.
      ProcessQueue(props);
    }));
  }
}

// Pops the oldest waiting item that may start now and marks it Sending, so two
// concurrent launches can never exceed the per-peer cap. Caller holds
// workers_mutex_; queue_mutex_ is taken before the state's FIFO lock.
bool FileTransferManager::TakeStartable(FileTransferState& state, size_t limit,
                                        FileTransferState::QueuedFile* out) {
  std::lock_guard queue_lock(queue_mutex_);
  std::lock_guard fifo_lock(state.file_queue_mutex_);
  auto& fifo = state.file_send_queue_;
  std::queue<FileTransferState::QueuedFile> kept;
  bool taken = false;
  while (!fifo.empty()) {
    auto entry = std::move(fifo.front());
    fifo.pop();
    const auto* item = send_queue_.Find(entry.file_id);
    // An entry the board no longer schedules as waiting (started under another
    // slot, cancelled or cleared) is stale: dropping it keeps the queue from
    // blocking on it forever.
    if (!item || item->state != file_transfer::SendQueue::State::Queued) continue;
    if (!taken && send_queue_.Active(entry.remote_id) < limit &&
        send_queue_.Start(entry.file_id)) {
      *out = std::move(entry);
      taken = true;
      continue;
    }
    kept.push(std::move(entry));
  }
  fifo.swap(kept);
  return taken;
}

// Runs one outbound file to a terminal state. Every guarantee of the single
// file path is kept verbatim: the authorization generation captured when the
// item was queued is re-checked before Start, a file cancelled while still
// queued releases the peer's browser destination, and the pump loop still
// waits on wake_.
void FileTransferManager::RunFile(std::shared_ptr<RemoteSession> props,
                                  FileTransferState::QueuedFile file) {
  auto& state = state_for(props);
  const auto id = file.file_id;
  MiniRtcPeerPtr* peer;
  std::shared_ptr<PeerEventHandler> events;
  {
    std::shared_lock lock(owner_.remote_sessions_mutex_);
    peer = props ? props->peer_ : owner_.peer_;
    events = props ? props->peer_events_ : owner_.peer_events_;
  }
  if (!props && file.remote_id.rfind("lan/", 0) == 0) {
    events = owner_.ControllerEvents(file.remote_id);
    peer = events ? events->DirectPeer() : nullptr;
  }
  auto session = std::make_shared<file_transfer::Session>(
      [this, peer, events, file, props](const char*, const char* data, size_t size) {
        if (!peer || !events || (props && props->closing_) ||
            (!props && !owner_.IsAuthorizedController(file.remote_id))) return -1;
        auto callback = events->EnterCallback();
        if (!callback || !events->DataAllowed()) return -1;
        const auto wire_id = events->WireId(file.remote_id);
        return PeerSendReliableDataFrameToPeer(
            peer, data, size, file.file_label.c_str(),
            wire_id.data(), wire_id.size());
      }, file_transfer::Session::Progress{});
  // UU pacing: the configured outbound cap owns this file's session and is
  // read here, where the session is created; 0 keeps transport speed.
  const auto rate_limit_kbps = owner_.config_center_
      ? owner_.config_center_->GetTransferRateLimitKbps() : 0;
  session->SetSendRate(static_cast<uint64_t>(rate_limit_kbps) * 1024ull);
  // The scalars below are the legacy single file aggregate the transfer window
  // heading reads. Every row stays authoritative, so only the newest start owns
  // them and only that owner may reset them.
  state.file_sent_bytes_ = 0; state.file_total_bytes_ = 0; state.file_send_rate_bps_ = 0;
  state.current_file_id_ = id;
  state.file_sending_ = true;
  const auto started_at = std::chrono::steady_clock::now();
  auto next_rate_update = started_at + std::chrono::seconds(1);
  {
    std::lock_guard lock(state.file_transfer_mutex_);
    state.file_send_start_time_ = state.file_send_last_update_time_ = started_at;
    state.file_send_last_bytes_ = 0;
  }
  bool cancelled = false;
  {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    for (auto& info : state.file_transfer_list_) {
      if (info.file_id == id) {
        cancelled = info.status == FileTransferState::FileTransferStatus::Cancelled;
        if (!cancelled) info.status = FileTransferState::FileTransferStatus::Sending;
        info.file_id = id; info.sent_bytes = 0;
        break;
      }
    }
  }
  const auto name = file.file_path.filename().u8string();
  const std::string wire_name(name.begin(), name.end());
  // 续传入口收的是 UTF-8 路径字符串（引擎内部再转 fs::path），与 wire_name 同源，
  // 这样中文/emoji 文件名不会在 Start 里被本地代码页改写。
  const auto path_utf8 = file.file_path.u8string();
  const std::string wire_path(path_utf8.begin(), path_utf8.end());
  // 断点续传决策点（docs/RESUME_DESIGN.md：方案 B 触发 + A 数据面）。在引擎
  // 打开文件之前先走控制面问对端「这个文件你已经有几个字节」，再把答案当作本次
  // 传输的起点。询问跑在本工作线程上、有 1.5s 上界，UI 线程既不发送也不等待；
  // 任何失败（未连接 / 老版本 / 超时 / 参数不合法）都只退化成本次从 0 发送，
  // 绝不把一个本来能传的文件判成失败。
  uint64_t file_size = 0;
  std::error_code size_ec;
  if (std::filesystem::is_regular_file(file.file_path, size_ec) && !size_ec) {
    const auto measured = std::filesystem::file_size(file.file_path, size_ec);
    if (!size_ec) file_size = static_cast<uint64_t>(measured);
  }
  uint64_t resume_offset = 0;
  bool peer_already_has_file = false;
  if (!cancelled && !stopping_ && (!props || !props->closing_) && file_size &&
      (!owner_.config_center_ || owner_.config_center_->GetTransferResumeEnabled())) {
    resume_offset = QueryResumeOffset(file.remote_id, wire_name, file_size);
    // offset == size 表示对端已经有完整文件：不建立传输，这行直接完成，并在行
    // 明细里写「已存在」，用户看到的是「已完成」而不是一次零字节的传输。
    peer_already_has_file = resume_offset >= file_size;
    LOG_INFO("Resume {} at {}/{} bytes", wire_name, resume_offset, file_size);
    if (peer_already_has_file) {
      {
        std::lock_guard detail_lock(queue_mutex_);
        row_detail_[id] = "已存在";
      }
      // 没有引擎在跑，聚合计数由这里补成「已全部完成」，窗口标题才不会显示
      // 0/0。current_file_id_ 上面刚写成 id，此刻这段计数归本文件所有。
      state.file_sent_bytes_ = file_size;
      state.file_total_bytes_ = file_size;
    }
  }
  bool started = false;
  {
    std::lock_guard lock(sessions_mutex_);
    const auto closed = disconnected_.find(events.get());
    const bool disconnected = closed != disconnected_.end() &&
        (closed->second.count("") || closed->second.count(file.remote_id));
    if (!cancelled && !stopping_ && peer && events && events->IsActive() && !disconnected &&
        (!props || !props->closing_) && !file.remote_id.empty()) {
      // 第 3 个参数是本文件总大小，第 6 个是本传输的起点：引擎会 seek 到对端
      // 已有的字节处继续，而不是把整个文件重发一遍。对端已有完整文件时根本不
      // Start（引擎自己也会拒绝 offset == total 的起点）。
      started = !peer_already_has_file &&
                session->Start(wire_path, wire_name, file_size,
                               file_transfer::Session::Clock::now(), id, resume_offset);
      if (started) {
        // A hold belongs to the item and to the peer: a queued file inherits
        // both, so a parked queue starts every new item paused.
        bool held = false;
        {
          std::lock_guard queue_lock(queue_mutex_);
          held = send_queue_.Held(id);
        }
        if (held || paused_remotes_.count(file.remote_id)) session->SetSendPaused(true);
        senders_.emplace(id, Sender{session, events, file.remote_id});
      }
    }
  }
  uint64_t previous = UINT64_MAX;
  auto progress = file_transfer::SendState{id, 0, 0,
      cancelled ? file_transfer::Status::Cancelled : file_transfer::Status::Failed};
  // 对端已有完整文件：没有引擎在跑，但这行必须自己走到 Completed。走的就是下面
  // 同一条终态收尾路径，sent_bytes 会被补成完整大小。
  if (peer_already_has_file) progress.status = file_transfer::Status::Completed;
  if (cancelled && peer && events && (!props || !props->closing_) &&
      (props || owner_.IsAuthorizedController(file.remote_id))) {
    auto callback = events->EnterCallback();
    if (callback && events->DataAllowed()) {
      // A queued browser file already reserved a destination on the peer.
      // Release that reservation even though no data chunk was ever sent.
      const auto packet = EncodeFileCancel(id, 0);
      const auto wire_id = events->WireId(file.remote_id);
      PeerSendReliableDataFrameToPeer(peer, packet.data(), packet.size(),
          file.file_label.c_str(), wire_id.data(), wire_id.size());
    }
  }
  if (started) {
    do {
      {
        std::lock_guard lock(state.file_transfer_list_mutex_);
        for (const auto& info : state.file_transfer_list_) if (info.file_id == id) {
          cancelled = info.status == FileTransferState::FileTransferStatus::Cancelling;
          break;
        }
      }
      if (cancelled) session->CancelSend();
      else if (stopping_ || !events->IsActive() || (props && props->closing_)) session->Cancel();
      const bool pumped = session->Pump();
      progress = session->Snapshot();
      const auto now = std::chrono::steady_clock::now();
      const bool rate_due = now >= next_rate_update;
      if (progress.acknowledged != previous || rate_due ||
          progress.status != file_transfer::Status::Sending) {
        UpdateProgress(state, progress, now);
        previous = progress.acknowledged;
        if (rate_due) next_rate_update = now + std::chrono::seconds(1);
      }
      if (progress.status != file_transfer::Status::Sending) break;
      if (!pumped) {
        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(5));
      }
    } while (true);
  }
  // Resolve the terminal status once: the row table and the queue board must
  // agree, because the UI thread reads the row and the window reads the board.
  auto row_status = progress.status == file_transfer::Status::Completed
      ? FileTransferState::FileTransferStatus::Completed
      : cancelled ? FileTransferState::FileTransferStatus::Cancelled
                  : FileTransferState::FileTransferStatus::Failed;
  {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    for (auto& info : state.file_transfer_list_) if (info.file_id == id) {
      const bool cancelled_row = progress.status == file_transfer::Status::Cancelled ||
          info.status == FileTransferState::FileTransferStatus::Cancelled ||
          info.status == FileTransferState::FileTransferStatus::Cancelling;
      info.status = progress.status == file_transfer::Status::Completed
          ? FileTransferState::FileTransferStatus::Completed
          : cancelled_row ? FileTransferState::FileTransferStatus::Cancelled
                          : FileTransferState::FileTransferStatus::Failed;
      info.rate_bps = 0;
      if (info.status == FileTransferState::FileTransferStatus::Completed) info.sent_bytes = info.file_size;
      row_status = info.status;
      break;
    }
  }
  // Release the slot first, so the queue refills while the UI thread reads the
  // finished row.
  {
    std::lock_guard lock(queue_mutex_);
    send_queue_.Finish(id,
        row_status == FileTransferState::FileTransferStatus::Completed
            ? file_transfer::SendQueue::State::Completed
            : row_status == FileTransferState::FileTransferStatus::Cancelled
                ? file_transfer::SendQueue::State::Cancelled
                : file_transfer::SendQueue::State::Failed);
  }
  bool peer_idle = false;
  {
    std::lock_guard lock(sessions_mutex_);
    senders_.erase(id);
    peer_idle = !std::any_of(senders_.begin(), senders_.end(),
        [&file](const auto& entry) { return entry.second.remote == file.remote_id; });
  }
  bool waiting = false;
  {
    std::lock_guard lock(queue_mutex_);
    waiting = send_queue_.Waiting(file.remote_id) > 0;
    state.file_sending_ = send_queue_.Active(file.remote_id) > 0;
  }
  // A finished file ends the peer's hold only when that peer has nothing left
  // waiting: a parked queue keeps starting its items paused.
  if (peer_idle && !waiting) {
    std::lock_guard lock(sessions_mutex_);
    paused_remotes_.erase(file.remote_id);
  }
  if (state.current_file_id_.load() == id) {
    if (row_status != FileTransferState::FileTransferStatus::Completed) {
      state.file_transfer_window_visible_ = false;
      state.file_sent_bytes_ = 0; state.file_total_bytes_ = 0;
      state.file_send_rate_bps_ = 0; state.current_file_id_ = 0;
    } else {
      state.file_transfer_window_visible_ = true;
    }
  }
}

void FileTransferManager::UpdateProgress(FileTransferState& state,
    const file_transfer::SendState& progress,
    std::chrono::steady_clock::time_point now) {
  bool owner = false;
  uint32_t rate = 0;
  // With N concurrent files only the file that owns the legacy aggregate
  // samples the peer speed. Every row is still updated by id below, so two
  // running files never share one speed sample.
  if (state.current_file_id_.load() == progress.id) {
    owner = true;
    state.file_sent_bytes_ = progress.acknowledged; state.file_total_bytes_ = progress.total;
    rate = state.file_send_rate_bps_.load();
    std::lock_guard lock(state.file_transfer_mutex_);
    const auto elapsed = now - state.file_send_last_update_time_;
    // Progress follows every ACK; speed uses one full sample per second for
    // this file. Sample even without new ACKs so a stalled transfer reads zero.
    if (elapsed >= std::chrono::seconds(1)) {
      const double seconds = std::chrono::duration<double>(elapsed).count();
      const auto bytes = progress.acknowledged >= state.file_send_last_bytes_
          ? progress.acknowledged - state.file_send_last_bytes_ : 0;
      const double measured = bytes * 8.0 / seconds;
      rate = static_cast<uint32_t>((std::min)(measured,
          static_cast<double>((std::numeric_limits<uint32_t>::max)())));
      state.file_send_rate_bps_ = rate;
      state.file_send_last_bytes_ = progress.acknowledged;
      state.file_send_last_update_time_ = now;
    }
  }
  std::lock_guard lock(state.file_transfer_list_mutex_);
  for (auto& info : state.file_transfer_list_) if (info.file_id == progress.id) {
    info.sent_bytes = progress.acknowledged; info.file_size = progress.total;
    if (owner) info.rate_bps = rate;
    break;
  }
}

bool FileTransferManager::CancelSend(uint32_t file_id) {
  using Status = FileTransferState::FileTransferStatus;
  bool cancelled = false, was_queued = false;
  std::string remote_id;
  const auto cancel = [&, file_id](FileTransferState& state) {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    for (auto& info : state.file_transfer_list_) {
      if (info.file_id != file_id || info.receiving) continue;
      if (info.status != Status::Queued && info.status != Status::Sending) return false;
      was_queued = info.status == Status::Queued;
      info.status = was_queued ? Status::Cancelled : Status::Cancelling;
      info.rate_bps = 0;
      remote_id = info.remote_id;
      return true;
    }
    return false;
  };
  std::shared_ptr<RemoteSession> props;
  cancelled = cancel(global_state_);
  if (!cancelled) {
    std::shared_lock lock(owner_.remote_sessions_mutex_);
    for (const auto& [_, session] : owner_.remote_sessions_) {
      if (session && cancel(session->file_transfer_)) {
        props = session;
        cancelled = true;
        break;
      }
    }
  }
  if (!cancelled) return false;
  if (was_queued) {
    // A file that never reached a sender leaves the queue in this call: it must
    // not keep a slot, and the row has to read Cancelled in the same frame. The
    // browser destination it reserved on the peer is released from a worker, so
    // the UI thread never waits on the transport.
    std::string label;
    {
      std::lock_guard lock(queue_mutex_);
      if (const auto source = sources_.find(file_id); source != sources_.end())
        label = source->second.label;
      send_queue_.Finish(file_id, file_transfer::SendQueue::State::Cancelled);
    }
    PruneWaiting(state_for(props));
    CancelQueuedHandshake(props, file_id, remote_id, label);
  }
  wake_.notify_all();
  return true;
}

// A cancelled queued item never owned a session, so no pump loop can send its
// cancel packet. Do it here, off the calling thread, exactly as the queued
// branch of the sender does: a header only cancel on the file stream.
void FileTransferManager::CancelQueuedHandshake(std::shared_ptr<RemoteSession> props,
    uint32_t id, const std::string& remote_id, const std::string& label) {
  if (!id || remote_id.empty()) return;
  MiniRtcPeerPtr* peer;
  std::shared_ptr<PeerEventHandler> events;
  {
    std::shared_lock lock(owner_.remote_sessions_mutex_);
    peer = props ? props->peer_ : owner_.peer_;
    events = props ? props->peer_events_ : owner_.peer_events_;
  }
  if (!props && remote_id.rfind("lan/", 0) == 0) {
    events = owner_.ControllerEvents(remote_id);
    peer = events ? events->DirectPeer() : nullptr;
  }
  if (!peer || !events) return;
  const auto stream = label.empty() ? owner_.file_label_ : label;
  std::lock_guard workers_lock(workers_mutex_);
  std::erase_if(workers_, [](auto& worker) {
    return worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
  workers_.push_back(std::async(std::launch::async,
      [this, props, peer, events, id, remote_id, stream] {
        if (stopping_ || (props && props->closing_) ||
            (!props && !owner_.IsAuthorizedController(remote_id))) return;
        auto callback = events->EnterCallback();
        if (!callback || !events->DataAllowed()) return;
        const auto packet = EncodeFileCancel(id, 0);
        const auto wire_id = events->WireId(remote_id);
        PeerSendReliableDataFrameToPeer(peer, packet.data(), packet.size(),
            stream.c_str(), wire_id.data(), wire_id.size());
      }));
}

bool FileTransferManager::PauseTransfer(const std::string& remote_id) {
  // The legacy per-peer hold is the queue-level hold now that one peer can run
  // several files; the window keeps its single toggle binding.
  return PauseAllTransfers(remote_id);
}

bool FileTransferManager::ResumeTransfer(const std::string& remote_id) {
  return ResumeAllTransfers(remote_id);
}

bool FileTransferManager::TransferPaused(const std::string& remote_id) const {
  if (remote_id.empty()) return false;
  std::lock_guard lock(sessions_mutex_);
  if (paused_remotes_.count(remote_id) != 0) return true;
  return std::any_of(senders_.begin(), senders_.end(), [&remote_id](const auto& entry) {
    return entry.second.remote == remote_id && entry.second.session &&
           entry.second.session->send_paused();
  });
}

size_t FileTransferManager::ConcurrentLimit() const {
  return std::clamp(concurrent_per_peer_.load(), size_t{1}, kMaxConcurrentPerPeer);
}

size_t FileTransferManager::ConcurrentTransfersPerPeer() const {
  return ConcurrentLimit();
}

void FileTransferManager::SetConcurrentTransfersPerPeer(size_t limit) {
  // The cap is a transport budget, not a UI preference: every concurrent file
  // keeps its own 1 MiB unacknowledged window on the same association.
  concurrent_per_peer_ = std::clamp(limit, size_t{1}, kMaxConcurrentPerPeer);
}

// A remote this manager can act on: either the runtime owns a session for it, or
// its rows are still tracked (LAN controllers have no session object).
bool FileTransferManager::KnownPeer(const std::string& remote_id) {
  if (remote_id.empty()) return false;
  {
    std::lock_guard lock(queue_mutex_);
    if (send_queue_.Tracked(remote_id)) return true;
  }
  return owner_.FindRemoteSession(remote_id) != nullptr;
}

// Whatever a resume or a clear released can take a slot now instead of waiting
// for the next finished file.
void FileTransferManager::PokeQueue(const std::string& remote_id) {
  ProcessQueue(owner_.FindRemoteSession(remote_id));
}

bool FileTransferManager::PauseAllTransfers(const std::string& remote_id) {
  if (!KnownPeer(remote_id)) return false;
  bool paused = false;
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, sender] : senders_) {
      if (sender.remote != remote_id || !sender.session) continue;
      // Holds new chunks only: the file, its ACKs and its queue slot survive.
      sender.session->SetSendPaused(true);
      paused = true;
    }
    // The hold also belongs to the peer, not to one file: the queue keeps
    // starting items paused until it is released, and a waiting item inherits it.
    paused_remotes_.insert(remote_id);
  }
  {
    std::lock_guard lock(queue_mutex_);
    paused = send_queue_.PauseAll(remote_id) > 0 || paused;
  }
  ClearSendingRate(remote_id);
  // The send workers are parked on this wakeup; re-pump so the hold lands now.
  wake_.notify_all();
  return paused;
}

bool FileTransferManager::ResumeAllTransfers(const std::string& remote_id) {
  if (!KnownPeer(remote_id)) return false;
  const auto rate_limit_kbps = owner_.config_center_
      ? owner_.config_center_->GetTransferRateLimitKbps() : 0;
  bool resumed = false;
  {
    std::lock_guard lock(sessions_mutex_);
    resumed = paused_remotes_.erase(remote_id) != 0;
    for (auto& [id, sender] : senders_) {
      if (sender.remote != remote_id || !sender.session) continue;
      // Resuming is also when a changed cap reaches the running session, so the
      // setting applies without restarting the transfer.
      sender.session->SetSendRate(static_cast<uint64_t>(rate_limit_kbps) * 1024ull);
      sender.session->SetSendPaused(false);
      resumed = true;
    }
  }
  {
    std::lock_guard lock(queue_mutex_);
    resumed = send_queue_.ResumeAll(remote_id) > 0 || resumed;
  }
  if (!resumed) return false;
  PokeQueue(remote_id);
  wake_.notify_all();
  return true;
}

// UU "start all". At this layer the release is the same as ResumeAllTransfers,
// and it always refills the slots: a queue whose items were held while every
// slot was busy starts sending inside this call.
bool FileTransferManager::StartAllTransfers(const std::string& remote_id) {
  if (!KnownPeer(remote_id)) return false;
  const auto rate_limit_kbps = owner_.config_center_
      ? owner_.config_center_->GetTransferRateLimitKbps() : 0;
  {
    std::lock_guard lock(sessions_mutex_);
    paused_remotes_.erase(remote_id);
    for (auto& [id, sender] : senders_) {
      if (sender.remote != remote_id || !sender.session) continue;
      sender.session->SetSendRate(static_cast<uint64_t>(rate_limit_kbps) * 1024ull);
      sender.session->SetSendPaused(false);
    }
  }
  bool live = false;
  {
    std::lock_guard lock(queue_mutex_);
    send_queue_.ResumeAll(remote_id);
    // A row that is running or still waiting is what start-all acts on; a peer
    // whose rows all finished already is a no-op and answers false.
    live = send_queue_.Active(remote_id) + send_queue_.Waiting(remote_id) > 0;
  }
  PokeQueue(remote_id);
  wake_.notify_all();
  return live;
}

bool FileTransferManager::ClearAllTransfers(const std::string& remote_id) {
  // Clear list: waiting and finished rows go, a running file keeps its row, its
  // session and its ACKs.
  return DropRows(remote_id, true, 0);
}

bool FileTransferManager::RemoveFinishedTransfers(const std::string& remote_id) {
  // The existing clear-completed, applied to the row table itself so the queue
  // stops tracking rows the window no longer shows.
  return DropRows(remote_id, false, 0);
}

bool FileTransferManager::PauseTransferItem(const std::string& remote_id, uint32_t file_id) {
  if (remote_id.empty() || !file_id) return false;
  {
    std::lock_guard lock(queue_mutex_);
    const auto* item = send_queue_.Find(file_id);
    if (!item || item->remote_id != remote_id) return false;
    // A finished row has nothing to hold; Pause refuses terminal items.
    if (!send_queue_.Pause(file_id)) return false;
  }
  {
    std::lock_guard lock(sessions_mutex_);
    if (const auto sender = senders_.find(file_id);
        sender != senders_.end() && sender->second.session)
      // Holds new chunks only: the file, its ACKs and its slot survive.
      sender->second.session->SetSendPaused(true);
  }
  ClearItemRate(remote_id, file_id);
  wake_.notify_all();
  return true;
}

bool FileTransferManager::ResumeTransferItem(const std::string& remote_id, uint32_t file_id) {
  if (remote_id.empty() || !file_id) return false;
  {
    std::lock_guard lock(queue_mutex_);
    const auto* item = send_queue_.Find(file_id);
    if (!item || item->remote_id != remote_id) return false;
    if (!send_queue_.Resume(file_id)) return false;
  }
  const auto rate_limit_kbps = owner_.config_center_
      ? owner_.config_center_->GetTransferRateLimitKbps() : 0;
  {
    std::lock_guard lock(sessions_mutex_);
    if (const auto sender = senders_.find(file_id);
        sender != senders_.end() && sender->second.session) {
      sender->second.session->SetSendRate(static_cast<uint64_t>(rate_limit_kbps) * 1024ull);
      sender->second.session->SetSendPaused(false);
    }
  }
  // A held waiting item was kept out of a slot, so refill now.
  PokeQueue(remote_id);
  wake_.notify_all();
  return true;
}

bool FileTransferManager::RetryTransferItem(const std::string& remote_id, uint32_t file_id) {
  if (remote_id.empty() || !file_id) return false;
  Source source;
  std::string name;
  uint64_t size = 0;
  {
    std::lock_guard lock(queue_mutex_);
    if (!send_queue_.Retryable(file_id)) return false;
    const auto* item = send_queue_.Find(file_id);
    if (!item || item->remote_id != remote_id) return false;
    name = item->name;
    size = item->size;
    if (const auto found = sources_.find(file_id); found != sources_.end())
      source = found->second;
  }
  // A retry replays the same source. A file the user moved is not retryable.
  std::error_code ec;
  if (source.path.empty() || !std::filesystem::is_regular_file(source.path, ec) ||
      ec || std::filesystem::file_size(source.path, ec) != size)
    return false;
  const auto new_id = file_transfer::Session::NextFileId();
  {
    std::lock_guard lock(queue_mutex_);
    // A fresh wire id is mandatory: the receiver keeps a tombstone for the id
    // the failed attempt used and answers flags=2 for it forever.
    if (!send_queue_.Retry(file_id, new_id)) return false;
    sources_[new_id] = source;
    sources_.erase(file_id);
  }
  auto props = owner_.FindRemoteSession(remote_id);
  auto& state = state_for(props);
  // The failed row is replaced, not duplicated: one row per attempt.
  DropRow(props, file_id);
  // 重放同样走续传询问：上一次失败很可能就断在文件中间，所以新 id 的 RunFile
  // 会重新问对端「你已经有几个字节」，而不是从 0 重发整个文件。
  PublishQueuedRow(state, new_id, name, remote_id, source.path, size);
  EnqueueWaiting(state, source.path, source.label, remote_id, new_id);
  ProcessQueue(props);
  return true;
}

FileTransferManager::TransferBatchSummary FileTransferManager::BatchSummary(
    const std::string& remote_id) {
  std::lock_guard lock(queue_mutex_);
  // Derived from the board the row updates write, so total/completed/failed/
  // active/queued/paused can never drift from the list the window renders. It
  // counts this peer's outbound rows only: the receive side keeps its own list.
  return send_queue_.Summarize(remote_id);
}

FileTransferManager::TransferRowState FileTransferManager::RowStateLocked(
    const FileTransferState::FileTransferInfo& info) const {
  using Status = FileTransferState::FileTransferStatus;
  if (!info.receiving) {
    if (const auto* item = send_queue_.Find(info.file_id)) {
      if (item->held && !file_transfer::SendQueue::Terminal(item->state))
        return TransferRowState::Paused;
      return item->state;
    }
  }
  // Rows that never went through the outbound queue (inbound files, rows of a
  // previous session) fall back to the status they already carry.
  switch (info.status) {
    case Status::Queued: return TransferRowState::Queued;
    case Status::Sending:
    case Status::Receiving:
    case Status::Cancelling: return TransferRowState::Sending;
    case Status::Completed: return TransferRowState::Completed;
    case Status::Failed: return TransferRowState::Failed;
    case Status::Cancelled: return TransferRowState::Cancelled;
  }
  return TransferRowState::Queued;
}

FileTransferManager::TransferRow FileTransferManager::ViewLocked(
    const FileTransferState::FileTransferInfo& info) const {
  TransferRow row;
  row.file_id = info.file_id;
  row.file_name = info.file_name;
  row.file_path = info.file_path;
  row.file_size = info.file_size;
  row.sent_bytes = info.sent_bytes;
  row.rate_bps = info.rate_bps;
  row.receiving = info.receiving;
  row.state = RowStateLocked(info);
  row.paused = row.state == TransferRowState::Paused;
  row.retryable = !info.receiving && send_queue_.Retryable(info.file_id);
  // 附加明细（目前只有续传判定出的「已存在」），受 queue_mutex_ 保护，调用方
  // 已持有它。
  if (const auto detail = row_detail_.find(info.file_id); detail != row_detail_.end())
    row.detail = detail->second;
  return row;
}

std::vector<FileTransferManager::TransferRow> FileTransferManager::TransferRows(
    const std::string& remote_id) {
  auto rows = BrowserTransfers(remote_id);
  std::lock_guard lock(queue_mutex_);
  std::vector<TransferRow> view;
  view.reserve(rows.size());
  for (const auto& info : rows) view.push_back(ViewLocked(info));
  return view;
}

bool FileTransferManager::TransferRowStateOf(const std::string& remote_id,
    uint32_t file_id, TransferRow* out) {
  auto rows = BrowserTransfers(remote_id);
  std::lock_guard lock(queue_mutex_);
  for (const auto& info : rows) {
    if (info.file_id != file_id) continue;
    if (out) *out = ViewLocked(info);
    return true;
  }
  return false;
}

// Drops waiting entries the board no longer schedules, keeping row order.
void FileTransferManager::PruneWaiting(FileTransferState& state) {
  std::lock_guard queue_lock(queue_mutex_);
  std::lock_guard fifo_lock(state.file_queue_mutex_);
  auto& fifo = state.file_send_queue_;
  std::queue<FileTransferState::QueuedFile> kept;
  while (!fifo.empty()) {
    auto entry = std::move(fifo.front());
    fifo.pop();
    const auto* item = send_queue_.Find(entry.file_id);
    if (!item || item->state != file_transfer::SendQueue::State::Queued) continue;
    kept.push(std::move(entry));
  }
  fifo.swap(kept);
}

bool FileTransferManager::DropRow(std::shared_ptr<RemoteSession> props, uint32_t file_id) {
  auto& state = state_for(props);
  bool dropped = false;
  {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    const auto before = state.file_transfer_list_.size();
    std::erase_if(state.file_transfer_list_, [file_id](const auto& info) {
      return info.file_id == file_id && !info.receiving;
    });
    dropped = state.file_transfer_list_.size() != before;
  }
  // 明细随行一起走；两把锁分开拿，保持 queue_mutex_ 的叶子锁次序。
  {
    std::lock_guard queue_lock(queue_mutex_);
    row_detail_.erase(file_id);
  }
  return dropped;
}

bool FileTransferManager::DropRows(const std::string& remote_id, bool include_queued,
                                   uint32_t only_id) {
  using Status = FileTransferState::FileTransferStatus;
  if (remote_id.empty()) return false;
  std::vector<uint32_t> dropped;
  const auto drop = [&](FileTransferState& state) {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    for (auto it = state.file_transfer_list_.begin(); it != state.file_transfer_list_.end();) {
      const bool finished = it->status == Status::Completed ||
                            it->status == Status::Failed ||
                            it->status == Status::Cancelled;
      const bool waiting = include_queued && it->status == Status::Queued;
      const bool selected = only_id ? it->file_id == only_id : (finished || waiting);
      if (it->remote_id == remote_id && selected && !it->receiving) {
        dropped.push_back(it->file_id);
        it = state.file_transfer_list_.erase(it);
      } else {
        ++it;
      }
    }
  };
  drop(global_state_);
  auto props = owner_.FindRemoteSession(remote_id);
  if (props) drop(props->file_transfer_);
  if (dropped.empty()) return false;
  {
    std::lock_guard lock(queue_mutex_);
    for (const auto id : dropped) {
      send_queue_.Forget(id);
      sources_.erase(id);
      row_detail_.erase(id);
    }
  }
  // A dropped waiting row must never start, so its queue entry goes with it.
  PruneWaiting(global_state_);
  if (props) PruneWaiting(props->file_transfer_);
  return true;
}

void FileTransferManager::ClearItemRate(const std::string& remote_id, uint32_t file_id) {
  const auto clear = [&remote_id, file_id](FileTransferState& state) {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    for (auto& info : state.file_transfer_list_) {
      if (info.file_id != file_id) continue;
      if (!remote_id.empty() && info.remote_id != remote_id) return;
      info.rate_bps = 0;
      return;
    }
  };
  clear(global_state_);
  std::shared_lock lock(owner_.remote_sessions_mutex_);
  for (const auto& [id, props] : owner_.remote_sessions_)
    if (props) clear(props->file_transfer_);
}

void FileTransferManager::ClearSendingRate(const std::string& remote_id) {
  const auto clear = [&remote_id](FileTransferState& state) {
    bool cleared = false;
    {
      std::lock_guard lock(state.file_transfer_list_mutex_);
      for (auto& info : state.file_transfer_list_) {
        if (info.receiving || info.remote_id != remote_id ||
            info.status != FileTransferState::FileTransferStatus::Sending) continue;
        info.rate_bps = 0;
        cleared = true;
      }
    }
    if (cleared) state.file_send_rate_bps_ = 0;
  };
  clear(global_state_);
  std::shared_lock lock(owner_.remote_sessions_mutex_);
  for (const auto& [id, props] : owner_.remote_sessions_)
    if (props) clear(props->file_transfer_);
}

void FileTransferManager::HandleAck(const char* data, size_t size,
    PeerEventHandler* handler, const std::string& remote) {
  FileTransferAck ack{};
  if (!DecodeFileTransferAck(data, size, &ack)) return;
  std::shared_ptr<file_transfer::Session> session;
  {
    std::lock_guard lock(sessions_mutex_);
    const auto it = senders_.find(ack.file_id);
    if (it == senders_.end() || it->second.handler.get() != handler || it->second.remote != remote) return;
    session = it->second.session;
  }
  session->Ack(data, size);
  wake_.notify_all();
}

void FileTransferManager::UpdateReceiveProgress(
    const file_transfer::ReceiveState& progress,
    std::unordered_map<uint32_t, size_t>& notifications,
    const std::string& remote_id) {
  using Status = FileTransferState::FileTransferStatus;
  std::lock_guard lock(received_state_.file_transfer_list_mutex_);
  auto& files = received_state_.file_transfer_list_;
  auto found = notifications.find(progress.id);
  if (found == notifications.end()) {
    FileTransferState::FileTransferInfo info;
    // Wire IDs can collide between peers or with outgoing transfers. Give each
    // reception a local notification identity from the same allocator as sends.
    info.file_id = file_transfer::Session::NextFileId();
    info.remote_id = remote_id;
    info.receiving = true;
    info.wire_file_id = progress.id;
    receive_rate_samples_.emplace(info.file_id,
        ReceiveRateSample{std::chrono::steady_clock::now(), 0});
    found = notifications.emplace(progress.id, files.size()).first;
    files.push_back(std::move(info));
  }
  auto& info = files[found->second];
  info.file_name = progress.name;
  info.file_size = progress.total;
  info.sent_bytes = progress.received;
  if (const auto classified = classified_photos_.find(progress.id); classified != classified_photos_.end())
    info.file_path = classified->second;
  switch (progress.status) {
    case file_transfer::ReceiveStatus::Receiving: info.status = Status::Receiving; break;
    case file_transfer::ReceiveStatus::Completed: info.status = Status::Completed; break;
    case file_transfer::ReceiveStatus::Failed: info.status = Status::Failed; break;
    case file_transfer::ReceiveStatus::Cancelled: info.status = Status::Cancelled; break;
  }
  if (info.status != Status::Receiving) {
    info.rate_bps = 0;
    receive_rate_samples_.erase(info.file_id);
    notifications.erase(found);
  }
}

std::shared_ptr<FileReceiver> FileTransferManager::ReceiverFor(
    PeerEventHandler* handler, const std::string& remote) {
  const auto props = owner_.FindRemoteSession(remote);
  MiniRtcPeerPtr* peer;
  std::shared_ptr<PeerEventHandler> events;
  {
    std::shared_lock lock(owner_.remote_sessions_mutex_);
    peer = props ? props->peer_ : owner_.peer_;
    events = props ? props->peer_events_ : owner_.peer_events_;
  }
  if (!props && remote.rfind("lan/", 0) == 0) {
    events = owner_.ControllerEvents(remote);
    peer = events ? events->DirectPeer() : nullptr;
  }
  if (!peer || !events || events.get() != handler || (props && props->closing_)) return {};
  std::shared_ptr<FileReceiver> receiver;
  std::filesystem::path default_directory;
  {
    std::lock_guard lock(sessions_mutex_);
    const auto closed = disconnected_.find(handler);
    if (stopping_ || (closed != disconnected_.end() &&
        (closed->second.count("") || closed->second.count(remote)))) return {};
    auto& entry = receivers_[handler][remote];
    if (!entry.session) {
      entry.session = std::make_shared<FileReceiver>(); entry.handler = events;
      entry.default_directory = entry.session->OutputDir();
      entry.directory = std::make_shared<std::filesystem::path>(entry.default_directory);
      const auto directory = entry.directory;
      // Wire name -> wire id of the file the engine is publishing right now.
      // The engine serializes both callbacks for this receiver, so the entry
      // written on completion is still there when the final path arrives.
      auto publishing = std::make_shared<std::unordered_map<std::string, uint32_t>>();
      // The transfer engine serializes progress callbacks for this receiver.
      entry.session->SetOnReceiveProgress(
          [this, remote, publishing, notifications = std::unordered_map<uint32_t, size_t>{}]
          (const file_transfer::ReceiveState& progress) mutable {
            if (progress.status == file_transfer::ReceiveStatus::Completed)
              (*publishing)[progress.name] = progress.id;
            else if (progress.status != file_transfer::ReceiveStatus::Receiving)
              publishing->erase(progress.name);  // nothing was published
            UpdateReceiveProgress(progress, notifications, remote);
          });
      entry.session->SetOnProgress(
          [this, remote, directory, publishing](const std::string& name, double value,
                                                bool sending, const std::string& path) {
            // Only the receive completion of this session carries a final
            // path; its send notifications share the same callback.
            if (sending || value != 1 || path.empty()) return;
            uint32_t wire_id = 0;
            if (const auto found = publishing->find(name); found != publishing->end()) {
              wire_id = found->second;
              publishing->erase(found);
            }
            PublishReceivedFile(wire_id, name, PathFromUtf8(path), *directory, remote);
          });
      entry.session->SetOnSendAck([this, peer, events, remote, props](const FileTransferAck& ack) {
        if (!props && !owner_.IsAuthorizedController(remote)) return -1;
        auto callback = events->EnterCallback();
        if (!callback || !events->DataAllowed()) return -1;
        const auto bytes = EncodeFileTransferAck(ack);
        const auto wire_id = events->WireId(remote);
        return PeerSendReliableDataFrameToPeer(
            peer, bytes.data(), bytes.size(), kFileFeedbackStream,
            wire_id.data(), wire_id.size());
      });
    }
    receiver = entry.session;
    default_directory = entry.default_directory;
  }
  const auto configured = owner_.config_center_->GetFileTransferSavePath();
  const auto directory = configured.empty() ? default_directory : PathFromUtf8(configured);
  receiver->SetOutputDir(directory);
  // Resume metadata must live where the host `resume` op looks: the receive
  // directory itself, so the durable prefix and its sidecar travel together with
  // the folder the user configured.
  receiver->SetResumeDirectory(directory);
  receiver->SetResumeEnabled(owner_.config_center_
      ? owner_.config_center_->GetTransferResumeEnabled() : true);
  // 覆盖冲突：策略来自配置，kAskSkip 时由这个钩子逐个文件问用户。钩子在接收
  // 线程上被调用并阻塞，Ask() 只把问题排进队列，真正画窗口的是 Poll()（UI
  // 线程）。按对端记账，所以「全部应用」只在这一批里有效。
  receiver->SetOverwritePolicy(ConfiguredOverwritePolicy());
  receiver->SetConflictResolver(
      [this, remote](const std::string& destination, uint64_t size) {
        return conflict_dialogs_.Ask(remote, destination, size);
      });
  {
    const auto utf8 = directory.u8string();
    browser_.SetReceiveDirectory(std::string(utf8.begin(), utf8.end()));
  }
  {
    std::lock_guard lock(sessions_mutex_);
    const auto found = receivers_.find(handler);
    if (found != receivers_.end()) {
      const auto peer = found->second.find(remote);
      if (peer != found->second.end() && peer->second.directory) *peer->second.directory = directory;
    }
  }
  return receiver;
}

void FileTransferManager::PublishReceivedFile(
    uint32_t wire_id, const std::string& name, const std::filesystem::path& published,
    const std::filesystem::path& directory, const std::string& remote) {
  // Only files that landed straight in the configured receive folder follow the
  // policy; browser downloads keep their requested folder layout.
  if (published.empty() || directory.empty() || name.empty()) return;
  std::error_code ec;
  if (!std::filesystem::equivalent(published.parent_path(), directory, ec) || ec) return;
  // 覆盖策略已经在接收路径上落地了（FileReceiver::DrainPublished →
  // ResolveConflict，kAskSkip 会在那里问用户），这里拿到的就是最终路径；再做
  // 一次策略只会把用户刚确认过的选择再执行一遍。
  auto path = published;
  bool classified = false;
  if (std::filesystem::is_regular_file(path, ec) && owner_.config_center_ &&
      owner_.config_center_->IsPhotoGroupByDate() && photo::IsPhotoPath(name)) {
    // Grouping runs after the conflict policy, so a renamed file keeps its
    // final name inside <receive folder>/Photos/<YYYY-MM>.
    std::filesystem::path moved;
    if (photo::ClassifyReceivedPhoto(path, directory, true, &moved)) {
      path = std::move(moved);
      classified = true;
    }
  }
  if (!wire_id) return;
  std::lock_guard lock(received_state_.file_transfer_list_mutex_);
  if (classified) classified_photos_[wire_id] = path;
  for (auto& info : received_state_.file_transfer_list_) {
    if (!info.receiving || info.wire_file_id != wire_id) continue;
    if (!remote.empty() && info.remote_id != remote) continue;
    // The UI row now points at the path the file really landed on.
    info.file_path = path;
    break;
  }
}

// ------------------------------------------- overwrite prompt / orphan sweep --
int FileTransferManager::LanguageIndex() const {
  // 目录里的语言下标就是配置中心那套枚举值；SYSTEM 交给 ClampLanguageIndex
  // 按系统语言解析，和传输窗口用的是同一个解析器。
  const auto language = owner_.config_center_
      ? static_cast<int>(owner_.config_center_->GetLanguage())
      : localization::kEnglishLanguage;
  return localization::detail::ClampLanguageIndex(language);
}

TransferOverwritePolicy FileTransferManager::ConfiguredOverwritePolicy() const {
  const auto policy = owner_.config_center_
      ? owner_.config_center_->GetTransferOverwritePolicy() : 2;
  return static_cast<TransferOverwritePolicy>(std::clamp(policy, 0, 2));
}

void FileTransferManager::PumpConflictDialogs() {
  conflict_dialogs_.Pump(LanguageIndex());
}

// 一次接收批次 = 这个对端还在往本机送文件。判定用「没有正在接收的行」+ 一段
// 空闲宽限：连续到达的两个文件之间也会有几百毫秒空档，那不属于新的一批，否则
// 用户刚选的「全部应用」会在下一个文件上被重新问一遍。
void FileTransferManager::ExpireConflictBatches() {
  const auto remotes = conflict_dialogs_.BatchRemotes();
  if (remotes.empty()) {
    conflict_batch_idle_.clear();
    return;
  }
  // 有一个问题正等着用户回答时什么都不清：用户正在看的那个批次还活着。
  const auto now = std::chrono::steady_clock::now();
  if (conflict_dialogs_.PendingRequest() != 0) {
    for (const auto& remote : remotes) conflict_batch_idle_[remote] = now;
    return;
  }
  std::set<std::string> active;
  {
    std::lock_guard lock(received_state_.file_transfer_list_mutex_);
    for (const auto& info : received_state_.file_transfer_list_) {
      if (info.receiving && info.status == FileTransferState::FileTransferStatus::Receiving)
        active.insert(info.remote_id);
    }
  }
  for (const auto& remote : remotes) {
    if (active.count(remote) != 0) {
      conflict_batch_idle_.erase(remote);
      continue;
    }
    const auto [entry, inserted] = conflict_batch_idle_.emplace(remote, now);
    if (inserted) continue;
    if (now - entry->second < kConflictBatchIdle) continue;
    conflict_batch_idle_.erase(entry);
    conflict_dialogs_.EndBatch(remote);
  }
  // 已经不在批次表里的对端不再需要计时。
  std::erase_if(conflict_batch_idle_, [&remotes](const auto& entry) {
    return std::find(remotes.begin(), remotes.end(), entry.first) == remotes.end();
  });
}

std::filesystem::path FileTransferManager::ReceiveDirectoryForSweep() {
  const auto configured = owner_.config_center_
      ? owner_.config_center_->GetFileTransferSavePath() : std::string();
  return configured.empty() ? FileReceiver::DefaultOutputDirectory()
                            : PathFromUtf8(configured);
}

void FileTransferManager::MaybeSweepOrphans(std::chrono::steady_clock::time_point now) {
  if (!sweep_initialized_) {
    sweep_initialized_ = true;
    sweep_schedule_ = ResolveOrphanSweepSchedule();
    if (!sweep_schedule_.enabled) {
      LOG_INFO("[transfer] sweep disabled by {}",
               std::string(kOrphanSweepHoursEnv));
      return;
    }
    // 启动后第一次立刻扫一遍：接收目录此刻已经确定（用户配置的那个，或者默认
    // 桌面），上一轮崩溃留下的暂存/侧车正好在这里收掉。
    next_orphan_sweep_ = now;
  }
  if (!sweep_schedule_.enabled || now < next_orphan_sweep_) return;
  // 本次结束后重新计时。间隔至少 1 秒：env 里的极端值不该把 UI 线程变成一直
  // 在扫盘。
  next_orphan_sweep_ = now + (std::max)(sweep_schedule_.interval, std::chrono::seconds(1));
  const auto directory = ReceiveDirectoryForSweep();
  if (directory.empty()) return;
  SweepReport report;
  SweepOrphans(directory, sweep_schedule_.min_age, &report);
  const auto utf8 = directory.u8string();
  LOG_INFO("[transfer] sweep dir={} staging={} sidecars={} skipped_live={} bytes={}",
           std::string(utf8.begin(), utf8.end()), report.staging_removed,
           report.sidecars_removed, report.skipped_live,
           static_cast<unsigned long long>(report.bytes_removed));
}

void FileTransferManager::Receive(const char* data, size_t size,
    PeerEventHandler* handler, const std::string& remote) {
  if (auto receiver = ReceiverFor(handler, remote)) receiver->OnData(data, size);
}

void FileTransferManager::Connected(PeerEventHandler* handler, const std::string& remote) {
  std::lock_guard lock(sessions_mutex_);
  if (remote.empty()) disconnected_.erase(handler);
  else if (const auto it = disconnected_.find(handler); it != disconnected_.end()) {
    it->second.erase(remote);
    if (it->second.empty()) disconnected_.erase(it);
  }
}

void FileTransferManager::Cancel(PeerEventHandler* handler, const std::string& remote) {
  // 连接断了就等于这一批结束：这个对端记住的「全部应用」不能再影响下一次连接。
  if (!remote.empty()) conflict_dialogs_.EndBatch(remote);
  {
    std::lock_guard lock(browser_mutex_);
    for (auto it = browser_peers_.begin(); it != browser_peers_.end();) {
      if (it->second.get() == handler && (remote.empty() || it->first == remote)) {
        browser_.Disconnect(it->first);
        browser_opened_.erase(it->first);
        browser_supported_.erase(it->first);
        it = browser_peers_.erase(it);
      } else ++it;
    }
  }
  std::lock_guard lock(sessions_mutex_);
  disconnected_[handler].insert(remote);
  for (auto& [id, sender] : senders_)
    if (sender.handler.get() == handler && (remote.empty() || sender.remote == remote)) sender.session->Cancel();
  const auto it = receivers_.find(handler);
  if (it != receivers_.end()) {
    for (auto entry = it->second.begin(); entry != it->second.end();) {
      if (remote.empty() || entry->first == remote) {
        entry->second.session->Cancel(); entry = it->second.erase(entry);
      } else ++entry;
    }
    if (it->second.empty()) receivers_.erase(it);
  }
  wake_.notify_all();
}

void FileTransferManager::Poll() {
  std::vector<std::shared_ptr<FileReceiver>> receivers;
  {
    std::lock_guard lock(sessions_mutex_);
    for (const auto& [handler, peers] : receivers_)
      for (const auto& [remote, receiver] : peers) receivers.push_back(receiver.session);
  }
  for (const auto& receiver : receivers) receiver->Poll();
  // 覆盖冲突的提示窗口和孤儿清理都挂在这条周期性驱动上：一个 UI 线程的
  // 定时器，不额外开线程。
  PumpConflictDialogs();
  ExpireConflictBatches();
  MaybeSweepOrphans(std::chrono::steady_clock::now());
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(received_state_.file_transfer_list_mutex_);
  for (auto& info : received_state_.file_transfer_list_) {
    const auto sample = receive_rate_samples_.find(info.file_id);
    if (sample == receive_rate_samples_.end()) continue;
    const auto elapsed = now - sample->second.updated;
    if (elapsed < std::chrono::seconds(1)) continue;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const auto bytes = info.sent_bytes >= sample->second.bytes
        ? info.sent_bytes - sample->second.bytes : 0;
    info.rate_bps = static_cast<uint32_t>((std::min)(bytes * 8.0 / seconds,
        static_cast<double>((std::numeric_limits<uint32_t>::max)())));
    sample->second = {now, info.sent_bytes};
  }
}

void FileTransferManager::SetBrowserSupport(PeerEventHandler* handler,
    const std::string& remote, bool supported) {
  const auto props = owner_.FindRemoteSession(remote);
  const auto events = props ? props->peer_events_ : owner_.ControllerEvents(remote);
  if (!events || events.get() != handler || !events->DataAllowed()) return;
  std::lock_guard lock(browser_mutex_);
  if (const auto previous = browser_peers_.find(remote);
      previous != browser_peers_.end() && previous->second != events) {
    browser_.Disconnect(remote);
    browser_opened_.erase(remote);
  }
  browser_peers_[remote] = events;
  if (supported) browser_supported_.insert(remote);
  else browser_supported_.erase(remote);
}

bool FileTransferManager::BrowserSupported(const std::string& remote) {
  std::lock_guard lock(browser_mutex_);
  return browser_supported_.count(remote) != 0;
}

void FileTransferManager::SetGallerySupport(PeerEventHandler* handler,
    const std::string& remote, bool supported) {
  const auto props = owner_.FindRemoteSession(remote);
  const auto events = props ? props->peer_events_ : owner_.ControllerEvents(remote);
  if (!events || events.get() != handler || !events->DataAllowed()) return;
  std::lock_guard lock(browser_mutex_);
  if (supported) gallery_supported_.insert(remote);
  else gallery_supported_.erase(remote);
}

bool FileTransferManager::GallerySupported(const std::string& remote) {
  std::lock_guard lock(browser_mutex_);
  return gallery_supported_.count(remote) != 0;
}

bool FileTransferManager::OpenGallery(const std::string& remote) {
  // Same admission and authorization path as the dual pane browser: the peer
  // only answers album requests while this controller has the gallery open.
  if (!OpenBrowser(remote)) return false;
  if (!BrowserSupported(remote) || !GallerySupported(remote)) {
    std::lock_guard lock(browser_mutex_);
    browser_opened_.erase(remote);
    return false;
  }
  return true;
}

std::filesystem::path FileTransferManager::PhotoSaveDirectory() {
  const auto configured = owner_.config_center_->GetFileTransferSavePath();
  const auto base = configured.empty() ? FileReceiver::DefaultOutputDirectory()
                                       : PathFromUtf8(configured);
  std::error_code ec;
  auto directory = base / "Photos";
  std::filesystem::create_directories(directory, ec);
  return directory;
}

bool FileTransferManager::OpenBrowser(const std::string& remote) {
  auto props = owner_.FindRemoteSession(remote);
  auto events = props ? props->peer_events_ : owner_.ControllerEvents(remote);
  if (!events || !events->DataAllowed() || owner_.unattended_host_ ||
      (props ? props->closing_ || !props->connection_established_
             : !owner_.IsAuthorizedController(remote))) return false;
  std::lock_guard lock(browser_mutex_);
  if (const auto previous = browser_peers_.find(remote);
      previous != browser_peers_.end() && previous->second != events) {
    browser_.Disconnect(remote);
    browser_opened_.erase(remote);
    browser_supported_.erase(remote);
  }
  browser_peers_[remote] = events;
  // Opening the legacy file picker authorizes a send, not disk browsing.
  if (browser_supported_.count(remote)) browser_opened_.insert(remote);
  return true;
}

void FileTransferManager::CancelBrowserTransfer(const std::string& remote, uint32_t file_id) {
  for (const auto& info : BrowserTransfers(remote)) {
    if (info.file_id != file_id) continue;
    if (info.receiving) browser_.CancelTransfer(remote, info.wire_file_id);
    else CancelSend(file_id);
    return;
  }
}

bool FileTransferManager::BrowserConnected(const std::string& remote) {
  auto props = owner_.FindRemoteSession(remote);
  auto events = props ? props->peer_events_ : owner_.ControllerEvents(remote);
  if (!events || !events->DataAllowed() || owner_.unattended_host_ ||
      (props ? props->closing_ || !props->connection_established_
             : !owner_.IsAuthorizedController(remote))) return false;
  std::lock_guard lock(browser_mutex_);
  const auto it = browser_peers_.find(remote);
  return it != browser_peers_.end() && it->second == events;
}

// 续传询问：控制面先说清楚「是哪个文件的哪一次传输」，数据面才有起点可言
// （docs/RESUME_DESIGN.md §3.1：方案 A 单独不成立，B 提供触发）。这里复用同一个
// file_browser v1 信封、同一条控制通道和同一个 SendBrowser 助手，不新增传输。
// 老构建：浏览器能力没通报过就不必问；通报了却回 ok:false/unsupported 也只
// warn 一次并退回 offset 0，永远不会让这次传输失败。
uint64_t FileTransferManager::QueryResumeOffset(const std::string& remote_id,
                                               const std::string& name,
                                               uint64_t total) {
  if (remote_id.empty() || name.empty() || !total || stopping_) return 0;
  if (!BrowserSupported(remote_id)) return 0;
  auto waiter = std::make_shared<ResumeWaiter>();
  waiter->remote = remote_id;
  auto answer = waiter->answer.get_future();
  const auto id = resume_next_id_.fetch_add(1);
  {
    std::lock_guard lock(resume_mutex_);
    // 连接异常时不要养一张无限长的表。
    if (resume_waiters_.size() >= 64) return 0;
    resume_waiters_[id] = waiter;
  }
  const FileBrowser::Json message{{"type", "file_browser"}, {"version", 1},
                                  {"op", "resume"},        {"id", id},
                                  {"name", name},          {"total_size", total}};
  const bool sent = SendBrowser(remote_id, message);
  ResumeAnswer reply;
  // 有界等待：本函数在发送工作线程上跑，应答由控制通道线程兑现 promise，UI
  // 线程既不发送也不等待。等待切成 100ms 一小段，这样 Shutdown()/断开最多被
  // 拖住一小段而不是整个 1.5s；超时就撤掉等待者，按 offset 0 继续。
  const auto deadline = std::chrono::steady_clock::now() + kResumeTimeout;
  while (sent) {
    if (answer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
      reply = answer.get();
      break;
    }
    if (stopping_ || std::chrono::steady_clock::now() >= deadline) break;
  }
  {
    std::lock_guard lock(resume_mutex_);
    resume_waiters_.erase(id);
  }
  if (!sent) return 0;               // 未连接 / 控制通道不可用：从 0 传
  if (!reply.answered) return 0;     // 超时：对端不回答，从 0 传
  if (!reply.ok) {
    if (reply.unsupported) {
      std::lock_guard lock(resume_mutex_);
      if (resume_unsupported_warned_.insert(remote_id).second)
        LOG_WARN("Resume unsupported by peer {}: sending from offset 0", remote_id);
    }
    return 0;
  }
  // 只接受 [0, total]：等于 total 表示对端已经有完整文件，由调用方收尾。
  return (std::min)(reply.offset, total);
}

// 续传应答从控制通道回来（和 prepare/cancel 的 reply 走同一条路）。按 id 找到
// 等待者并兑现 promise；不认识的 id（超时之后才到的迟到应答）留给调用方忽略。
bool FileTransferManager::ResolveResumeReply(const std::string& remote_id,
                                            const FileBrowser::Json& message) {
  if (!message.is_object()) return false;
  const auto op = message.find("op");
  if (op == message.end() || !op->is_string() || op->get<std::string>() != "reply")
    return false;
  const auto id = JsonRequestId(message);
  std::shared_ptr<ResumeWaiter> waiter;
  {
    std::lock_guard lock(resume_mutex_);
    const auto found = resume_waiters_.find(id);
    if (found == resume_waiters_.end()) return false;
    waiter = found->second;
  }
  if (!waiter || waiter->remote != remote_id) return false;
  ResumeAnswer answer;
  answer.answered = true;
  answer.ok = JsonFlag(message, "ok");
  if (answer.ok) answer.offset = JsonByteCount(message, "offset");
  else answer.unsupported = JsonTextField(message, "error") == "unsupported";
  // 只兑现一次：迟到的第二个 reply 不能再 set_value（会抛 future_error）。
  if (waiter->done.exchange(true)) return true;
  waiter->answer.set_value(answer);
  return true;
}

bool FileTransferManager::SendBrowser(const std::string& remote, const FileBrowser::Json& message) {
  if (!BrowserConnected(remote)) return false;
  auto props = owner_.FindRemoteSession(remote);
  std::shared_ptr<PeerEventHandler> events;
  { std::lock_guard lock(browser_mutex_); events = browser_peers_[remote]; }
  if (!events) return false;
  auto callback = events->EnterCallback();
  if (!callback || !events->DataAllowed()) return false;
  MiniRtcPeerPtr* peer = events->IsDirect() ? events->DirectPeer() : props ? props->peer_ : owner_.peer_;
  const auto data = message.dump(-1, ' ', false, FileBrowser::Json::error_handler_t::replace);
  if (!peer || data.size() > 60000) return false;
  const auto wire_id = events->WireId(remote);
  return PeerSendReliableDataFrameToPeer(peer, data.data(), data.size(),
      owner_.control_data_label_.c_str(), wire_id.data(), wire_id.size()) == 0;
}

bool FileTransferManager::HandleBrowser(const char* data, size_t size,
    PeerEventHandler* handler, const std::string& remote) {
  if (!data || size > 60000 || std::string_view(data, size).find("file_browser") == std::string_view::npos)
    return false;
  auto message = FileBrowser::Json::parse(data, data + size, nullptr, false);
  if (!message.is_object() || message.value("type", FileBrowser::Json{}) != "file_browser") return false;
  auto props = owner_.FindRemoteSession(remote);
  auto events = props ? props->peer_events_ : owner_.ControllerEvents(remote);
  if (owner_.unattended_host_ || !events || events.get() != handler || !events->DataAllowed() ||
      (props ? props->closing_ || !props->connection_established_
             : !owner_.IsAuthorizedController(remote))) return true;
  // 续传应答由发送方发起，browser_ 的 pending 表里没有对应 id，交给它只会被
  // 丢掉；先在这里认领，交回给等待中的发送线程。
  if (ResolveResumeReply(remote, message)) return true;
  bool opened;
  {
    std::lock_guard lock(browser_mutex_);
    browser_peers_[remote] = events;
    if (message.value("op", FileBrowser::Json{}) == "presentation") {
      // Older browser builds synchronized window visibility. Ignore that
      // optional event: progress now belongs exclusively to the local sender.
      return true;
    }
    opened = browser_opened_.count(remote);
  }
  // A controlled computer cannot browse the controller's disk until that
  // controller explicitly opens this peer's file manager in this connection.
  const auto op = message.value("op", FileBrowser::Json{});
  if (props && !opened && op != "reply") {
    if (message.contains("id")) SendBrowser(remote,
        {{"op", "reply"}, {"id", message["id"]}, {"type", "file_browser"},
         {"version", 1}, {"ok", false}, {"error", "browser_not_open"}});
    return true;
  }
  browser_.Handle(remote, std::move(message));
  return true;
}

std::vector<FileTransferManager::FileTransferState::FileTransferInfo>
FileTransferManager::BrowserTransfers(const std::string& remote) {
  std::vector<FileTransferState::FileTransferInfo> result;
  const auto collect = [&](auto& state) {
    std::lock_guard lock(state.file_transfer_list_mutex_);
    for (const auto& info : state.file_transfer_list_)
      if (info.remote_id == remote) result.push_back(info);
  };
  collect(global_state_); collect(received_state_);
  if (auto props = owner_.FindRemoteSession(remote)) collect(props->file_transfer_);
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.file_id > b.file_id; });
  return result;
}

}  // namespace crossdesk
