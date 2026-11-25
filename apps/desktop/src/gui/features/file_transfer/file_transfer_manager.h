#ifndef CROSSDESK_GUI_FILE_TRANSFER_MANAGER_H_
#define CROSSDESK_GUI_FILE_TRANSFER_MANAGER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "file_transfer.h"
#include <file_browser.h>
#include "features/file_transfer/conflict_dialog_host.h"
#include "runtime/remote_session.h"

namespace crossdesk {

class GuiRuntime;
class PeerEventHandler;

class FileTransferManager {
 public:
  using FileTransferState = gui_detail::FileTransferState;
  using RemoteSession = gui_detail::RemoteSession;
  // Row state the transfer window draws. Paused is derived from a per-item hold,
  // so a held row keeps its slot state and resumes exactly where it stopped.
  using TransferRowState = file_transfer::SendQueue::State;
  // Per-peer batch result: total, completed, failed, active, queued, paused and
  // the partial-success flag UU expresses with send_part_success /
  // recv_part_failed. Filled from the same queue board every row update writes,
  // so it can never disagree with the row table.
  using TransferBatchSummary = file_transfer::SendQueue::Summary;
  // One outbound row plus the queue facts FileTransferInfo cannot carry. The
  // window reads this next to BrowserTransfers() to paint the paused icon and
  // the retry button; FileTransferInfo itself lives in runtime/remote_session.h
  // and stays untouched, which keeps this change additive for the UI worker.
  struct TransferRow {
    uint32_t file_id = 0;
    std::string file_name;
    std::filesystem::path file_path;
    uint64_t file_size = 0, sent_bytes = 0;
    uint32_t rate_bps = 0;
    bool receiving = false;
    TransferRowState state = TransferRowState::Queued;
    bool paused = false;     // state == Paused, kept as its own binding flag
    bool retryable = false;  // RetryTransferItem accepts this row
    // 行附加明细（UTF-8）。目前只有断点续传判定出的「已存在」会填：对端已经把
    // 整个文件收全时，这条传输不建立，行直接完成并带上这句话。
    std::string detail;
  };
  // Outbound fan-out per peer. Two files in flight is what one SCTP
  // association carries next to video and input without starving either; four
  // is the hard ceiling because every concurrent file holds its own 1 MiB
  // unacknowledged window on that same association.
  static constexpr size_t kDefaultConcurrentPerPeer = 2;
  static constexpr size_t kMaxConcurrentPerPeer = 4;
  explicit FileTransferManager(GuiRuntime& owner);
  ~FileTransferManager();
  FileTransferState& global_state();
  FileTransferState& received_state();
  FileTransferState& state_for(const std::shared_ptr<RemoteSession>& props);
  // A reserved ID keeps a DeskHub notification stable while waiting for a slot.
  bool ProcessSelectedFile(const std::string& path, const std::shared_ptr<RemoteSession>& props,
                           const std::string& file_label, const std::string& remote_id = "",
                           uint32_t file_id = 0);
  void HandleAck(const char* data, size_t size, PeerEventHandler* handler, const std::string& remote);
  void Receive(const char* data, size_t size, PeerEventHandler* handler, const std::string& remote);
  void Connected(PeerEventHandler* handler, const std::string& remote);
  void Cancel(PeerEventHandler* handler, const std::string& remote = "");
  bool CancelSend(uint32_t file_id);
  // UU style pause: holds the peer's outbound pipeline without cancelling it,
  // and a queued file for that peer starts paused until ResumeTransfer runs.
  // Now that one peer can run several files these are the queue-level pair.
  bool PauseTransfer(const std::string& remote_id);
  bool ResumeTransfer(const std::string& remote_id);
  bool TransferPaused(const std::string& remote_id) const;
  // Queue controls, named after the UU assets that own them. Each returns false
  // for a remote this manager does not know, or when it changed nothing.
  bool StartAllTransfers(const std::string& remote_id);
  bool PauseAllTransfers(const std::string& remote_id);
  bool ResumeAllTransfers(const std::string& remote_id);
  // Drops waiting and finished rows and their queue slots; a running file keeps
  // its row, its session and its ACKs (no cancel).
  bool ClearAllTransfers(const std::string& remote_id);
  // The existing clear-completed behaviour, moved onto the row table itself.
  bool RemoveFinishedTransfers(const std::string& remote_id);
  // Per-item controls. A hold stops new chunks of a running item and keeps a
  // waiting one out of a slot; nothing is cancelled and no id changes.
  bool PauseTransferItem(const std::string& remote_id, uint32_t file_id);
  bool ResumeTransferItem(const std::string& remote_id, uint32_t file_id);
  // Replays a Failed/Cancelled row from the same source. The replay always gets
  // a fresh wire id: the receiver keeps a tombstone per cancelled id.
  bool RetryTransferItem(const std::string& remote_id, uint32_t file_id);
  // Alias so the transfer window's capability probe finds one obvious name.
  bool RetryTransfer(const std::string& remote_id, uint32_t file_id) {
    return RetryTransferItem(remote_id, file_id);
  }
  // Bounded fan-out per peer, clamped to [1, kMaxConcurrentPerPeer].
  void SetConcurrentTransfersPerPeer(size_t limit);
  size_t ConcurrentTransfersPerPeer() const;
  TransferBatchSummary BatchSummary(const std::string& remote_id);
  // Row view for the task list: BrowserTransfers() order plus queue state,
  // per-item paused and retryable.
  std::vector<TransferRow> TransferRows(const std::string& remote_id);
  bool TransferRowStateOf(const std::string& remote_id, uint32_t file_id,
                          TransferRow* out);
  void Poll();
  void Shutdown();
  FileBrowser& browser() { return browser_; }
  bool OpenBrowser(const std::string& remote);
  void SetBrowserSupport(PeerEventHandler* handler, const std::string& remote, bool supported);
  bool BrowserSupported(const std::string& remote);
  // Photo albums ride on the file browser RPC and need both capabilities.
  void SetGallerySupport(PeerEventHandler* handler, const std::string& remote, bool supported);
  bool GallerySupported(const std::string& remote);
  bool OpenGallery(const std::string& remote);
  // <receive folder>/Photos, used by the gallery for saved and pasted photos.
  std::filesystem::path PhotoSaveDirectory();
  bool BrowserConnected(const std::string& remote);
  bool HandleBrowser(const char* data, size_t size, PeerEventHandler* handler,
                     const std::string& remote);
  std::vector<FileTransferState::FileTransferInfo> BrowserTransfers(const std::string& remote);
  void CancelBrowserTransfer(const std::string& remote, uint32_t file_id);

 private:
  struct Sender {
    std::shared_ptr<file_transfer::Session> session;
    std::shared_ptr<PeerEventHandler> handler;
    std::string remote;
  };
  struct Receiver {
    std::shared_ptr<FileReceiver> session;
    std::shared_ptr<PeerEventHandler> handler;
    std::filesystem::path default_directory;
    // Published to the progress callback so a finished photo can be classified.
    std::shared_ptr<std::filesystem::path> directory;
  };
  // What a retry needs to replay an item: its source and its stream label.
  struct Source {
    std::filesystem::path path;
    std::string label;
  };
  // Fills every free slot of this state; the queue, not the session, is where a
  // file beyond ConcurrentTransfersPerPeer() waits.
  void ProcessQueue(std::shared_ptr<RemoteSession> props);
  // Same, for callers that already hold workers_mutex_.
  void LaunchQueued(std::shared_ptr<RemoteSession> props);
  // Pops the oldest item that may start now and marks it Sending, so two
  // concurrent launches can never exceed the per-peer cap.
  bool TakeStartable(FileTransferState& state, size_t limit,
                     FileTransferState::QueuedFile* out);
  // Runs one outbound file to a terminal state and releases its slot.
  void RunFile(std::shared_ptr<RemoteSession> props,
               FileTransferState::QueuedFile file);
  // Releases the peer's browser destination for an item that was cancelled
  // before it ever reached a sender. Runs off the caller's thread.
  void CancelQueuedHandshake(std::shared_ptr<RemoteSession> props, uint32_t id,
                             const std::string& remote_id,
                             const std::string& label);
  bool KnownPeer(const std::string& remote_id);
  void PokeQueue(const std::string& remote_id);
  void PublishQueuedRow(FileTransferState& state, uint32_t id,
                        const std::string& name, const std::string& remote_id,
                        const std::filesystem::path& path, uint64_t size);
  void EnqueueWaiting(FileTransferState& state, const std::filesystem::path& path,
                      const std::string& label, const std::string& remote_id,
                      uint32_t id);
  // Drops waiting entries the board no longer schedules, keeping row order.
  void PruneWaiting(FileTransferState& state);
  bool DropRow(std::shared_ptr<RemoteSession> props, uint32_t file_id);
  // include_queued separates clear-all from remove-finished; only_id pins the
  // drop to one row.
  bool DropRows(const std::string& remote_id, bool include_queued,
                uint32_t only_id);
  // Zeroes one row's speed so the UI stops advancing while the item is held.
  void ClearItemRate(const std::string& remote_id, uint32_t file_id);
  // Callers hold queue_mutex_.
  TransferRowState RowStateLocked(
      const FileTransferState::FileTransferInfo& info) const;
  TransferRow ViewLocked(const FileTransferState::FileTransferInfo& info) const;
  size_t ConcurrentLimit() const;
  std::shared_ptr<FileReceiver> ReceiverFor(PeerEventHandler* handler, const std::string& remote);
  bool SendBrowser(const std::string& remote, const FileBrowser::Json& message);
  void UpdateProgress(FileTransferState& state,
                      const file_transfer::SendState& progress,
                      std::chrono::steady_clock::time_point now);
  // 断点续传协商（docs/RESUME_DESIGN.md：方案 B 触发 + A 数据面）。
  // 对端对一次 resume 询问的回答。控制通道线程写、发送工作线程读，所以整块用
  // promise/future 交接：等待有上界，UI 线程既不发送也不等待。
  struct ResumeAnswer {
    uint64_t offset = 0;
    bool answered = false;     // 收到了 reply（不论 ok）
    bool ok = false;           // reply 里 ok=true
    bool unsupported = false;  // reply 里 error=unsupported：对端是老构建
  };
  // 一次询问的等待者。reply 到达时兑现 promise，done 保证只兑现一次。
  struct ResumeWaiter {
    std::string remote;
    std::promise<ResumeAnswer> answer;
    std::atomic<bool> done{false};
  };
  // 发一次 op=resume 并等待应答，返回对端已有的字节数（已 clamp 到 [0, total]）。
  // 未连接 / 老版本 / 超时 / 错误一律返回 0，也就是本次从 0 发送；返回 total
  // 表示对端已经有完整文件，由调用方直接收尾。跑在发送工作线程上，不碰 UI。
  uint64_t QueryResumeOffset(const std::string& remote_id, const std::string& name,
                             uint64_t total);
  // 控制通道线程：把 reply 交给等待中的发送者。不是续传应答时返回 false。
  bool ResolveResumeReply(const std::string& remote_id,
                          const FileBrowser::Json& message);
  void UpdateReceiveProgress(const file_transfer::ReceiveState& progress,
                             std::unordered_map<uint32_t, size_t>& notifications,
                             const std::string& remote_id);
  // Applies the configured overwrite policy to a file the engine just published
  // and points the receive row at the path it really landed on.
  void PublishReceivedFile(uint32_t wire_id, const std::string& name,
                           const std::filesystem::path& published,
                           const std::filesystem::path& directory,
                           const std::string& remote);
  // 覆盖冲突：接收线程在 FileReceiver 里撞到同名文件时阻塞提问，UI 定时器在
  // Poll() 里把问题画出来。批次（「全部应用」）随接收批次结束而清。
  void PumpConflictDialogs();
  void ExpireConflictBatches();
  // 一个接收批次「算是结束了」的空闲宽限：连续到达的文件之间有空档，宽限之内
  // 仍然算同一批，「全部应用」不会被重复询问。
  static constexpr auto kConflictBatchIdle = std::chrono::seconds(30);
  // 对端 -> 它开始空闲的时刻（已经被 EndBatch 的对端会从这里消失）。
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> conflict_batch_idle_;
  // 一次启动扫描 + 至多每 kOrphanSweepInterval 一次的孤儿清理；跑在 UI 线程
  // 的 Poll() 上，自带线程，不做递归遍历。
  void MaybeSweepOrphans(std::chrono::steady_clock::time_point now);
  std::filesystem::path ReceiveDirectoryForSweep();
  TransferOverwritePolicy ConfiguredOverwritePolicy() const;
  int LanguageIndex() const;
  // Zeroes the peer's in-flight speed rows so the UI stops advancing while held.
  void ClearSendingRate(const std::string& remote_id);
  struct ReceiveRateSample {
    std::chrono::steady_clock::time_point updated;
    uint64_t bytes = 0;
  };
  mutable std::mutex workers_mutex_, sessions_mutex_, wake_mutex_;
  std::condition_variable wake_;
  std::vector<std::future<void>> workers_;
  std::atomic<bool> stopping_{false};
  std::unordered_map<uint32_t, Sender> senders_;
  // Peers whose outbound pipeline is held by PauseTransfer. Guarded by
  // sessions_mutex_ so a newly started sender inherits the hold atomically.
  std::set<std::string> paused_remotes_;
  // Outbound queue policy: bounded fan-out, per-item holds, retryable rows and
  // the batch counters. queue_mutex_ is a leaf lock: it is taken after
  // workers_mutex_/sessions_mutex_ and never held while taking them back.
  mutable std::mutex queue_mutex_;
  file_transfer::SendQueue send_queue_;
  // Wire id -> source of a queued/finished item, so a retry replays the same
  // name, path and stream label. Guarded by queue_mutex_.
  std::unordered_map<uint32_t, Source> sources_;
  // 续传协商表：请求 id -> 等待者，答案由控制通道线程兑现。resume_mutex_ 是
  // 叶子锁：只在收发两端短暂持有，从不跨等待、也从不跨 UI 线程。
  std::mutex resume_mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<ResumeWaiter>> resume_waiters_;
  // 已经回过 unsupported 的对端：每个对端只 warn 一次，不按文件刷屏。
  std::set<std::string> resume_unsupported_warned_;
  // 询问 id 从高位起：FileBrowser 自己的请求 id 从 1 递增，两者永不相撞。
  static constexpr uint32_t kResumeIdBase = 0x40000000u;
  std::atomic<uint32_t> resume_next_id_{kResumeIdBase};
  // 一次询问的最长等待。再长就等于让用户等一个不存在的答复。
  static constexpr auto kResumeTimeout = std::chrono::milliseconds(1500);
  // 行明细表（wire id -> 文本），受 queue_mutex_ 保护：和行视图同一把锁。
  std::unordered_map<uint32_t, std::string> row_detail_;
  std::atomic<size_t> concurrent_per_peer_{kDefaultConcurrentPerPeer};
  std::unordered_map<PeerEventHandler*, std::unordered_map<std::string, Receiver>> receivers_;
  std::unordered_map<PeerEventHandler*, std::set<std::string>> disconnected_;
  GuiRuntime& owner_;
  FileTransferState global_state_;
  // Keep received files separate from outgoing queues.
  FileTransferState received_state_;
  // Guarded by received_state_.file_transfer_list_mutex_.
  std::unordered_map<uint32_t, ReceiveRateSample> receive_rate_samples_;
  // Wire id -> album path after a received photo was classified.
  std::unordered_map<uint32_t, std::filesystem::path> classified_photos_;
  std::mutex browser_mutex_;
  std::map<std::string, std::shared_ptr<PeerEventHandler>> browser_peers_;
  std::set<std::string> browser_opened_;
  std::set<std::string> browser_supported_;
  std::set<std::string> gallery_supported_;
  FileBrowser browser_;
  // 覆盖冲突的提问端。接收线程调 Ask() 会阻塞，所以它只由 Poll()（UI 线程）
  // 泵；批次级的「全部应用」按对端记账。
  ConflictDialogHost conflict_dialogs_;
  // 孤儿清理：schedule_ 只解析一次（启动后第一个 Poll），之后按它跑心跳。
  bool sweep_initialized_ = false;
  OrphanSweepSchedule sweep_schedule_;
  std::chrono::steady_clock::time_point next_orphan_sweep_{};
};

}  // namespace crossdesk

#endif
