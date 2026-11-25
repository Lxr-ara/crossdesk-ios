/*
 * @Author: DI JUNKUN
 * @Date: 2025-12-18
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FILE_TRANSFER_H_
#define _FILE_TRANSFER_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <file_transfer_core.h>

namespace crossdesk {

// Receive-side conflict policy for a finished transfer. The engine publishes
// under a timestamped name whenever the requested name is already taken; this
// policy decides what the desktop does with that published file.
enum class TransferOverwritePolicy {
  kAskSkip = 0,  // ask the installed ConflictResolver, or keep the engine's name
  kOverwrite = 1,
  kRename = 2,  // "report.txt" -> "report (1).txt"
};

// What the user answered when kAskSkip found a destination that is taken. The
// *All variants are the same three answers plus "and do that for every remaining
// file of this batch"; the batch itself lives in the GUI (see the manager's
// ConflictDialogHost), not here, so the receiver never remembers a choice.
enum class ConflictChoice {
  kSkip = 0,
  kOverwrite = 1,
  kRename = 2,
  kSkipAll = 3,
  kOverwriteAll = 4,
  kRenameAll = 5,
};

// Returns the final path for `published` (the engine's staging-published file)
// when the peer asked for `intended`. Both live in the receive folder; an
// identical pair means the engine found no conflict and nothing is renamed.
std::filesystem::path ApplyTransferOverwritePolicy(
    const std::filesystem::path& published, const std::filesystem::path& intended,
    TransferOverwritePolicy policy);

// Housekeeping for the receive folder: staging directories the engine abandoned
// (crash, kill, lost session) and resume sidecars whose target is gone. Both
// constants are the production cadence; CROSSDESK_ORPHAN_SWEEP_HOURS overrides
// the interval (0 disables the sweep) so a sweep can be observed without waiting
// an hour.
inline constexpr std::chrono::hours kOrphanSweepInterval{1};
inline constexpr std::chrono::hours kOrphanMinAge{48};
inline constexpr std::string_view kOrphanSweepHoursEnv = "CROSSDESK_ORPHAN_SWEEP_HOURS";

struct SweepReport {
  size_t staging_removed = 0;
  size_t sidecars_removed = 0;
  // Entries newer than min_age (a live transfer) and sidecars whose prefix is
  // still fresh: counted, never touched.
  size_t skipped_live = 0;
  uint64_t bytes_removed = 0;
};

struct OrphanSweepSchedule {
  bool enabled = true;
  std::chrono::seconds interval =
      std::chrono::duration_cast<std::chrono::seconds>(kOrphanSweepInterval);
  std::chrono::seconds min_age =
      std::chrono::duration_cast<std::chrono::seconds>(kOrphanMinAge);
};

// kOrphanSweepInterval / kOrphanMinAge, unless CROSSDESK_ORPHAN_SWEEP_HOURS is
// set: a positive value (fractions included) replaces the interval, 0 disables
// the sweep, anything unparsable keeps the default. Read on every call so a test
// can set the variable and call immediately.
OrphanSweepSchedule ResolveOrphanSweepSchedule();

// Removes what the engine left behind in `receive_dir`:
//   * staging entries (".crossdesk-*") whose newest content is older than
//     min_age - a live transfer keeps refreshing the partial file, so anything
//     touched inside min_age is left alone and counted as skipped_live;
//   * resume sidecars ("*.crossdesk-resume" and their ".tmp" write targets):
//     removed when the file they describe is gone, or when both the sidecar and
//     that file are older than min_age. A sidecar whose target still exists and
//     was touched inside min_age belongs to a transfer that can still resume, so
//     it is kept. The target file itself is never deleted: those are the user's
//     received bytes, not metadata.
// `out` (optional) receives the same report the function returns.
SweepReport SweepOrphans(const std::filesystem::path& receive_dir,
                         std::chrono::seconds min_age, SweepReport* out);

// Desktop destination policy; protocol, assembly, ACKs and cleanup live in the
// same engine used by the mobile adapter.
class FileReceiver {
 public:
  using OnSendAck = std::function<int(const FileTransferAck&)>;
  using OnProgress = file_transfer::Session::Progress;
  using OnReceiveProgress = file_transfer::Session::ReceiveProgress;
  // Called from the receive path (see DrainPublished) with the destination the
  // peer asked for and the size of the file that landed there. It blocks until
  // the host answers, exactly like the resume query blocks up to 1500 ms; the
  // GUI host is expected to marshal the question to the UI thread and wait.
  using ConflictResolver =
      std::function<ConflictChoice(const std::string& destination, uint64_t size)>;
  FileReceiver();
  explicit FileReceiver(const std::filesystem::path& output_dir);
  bool OnData(const char* data, size_t size);
  void Poll() { session_.Pump(); }
  void Cancel() { session_.Cancel(); }
  bool ReserveDirectory(uint32_t id, const std::filesystem::path& directory) {
    return session_.ReserveDirectory(id, directory);
  }
  // Set once, before the receiver is published to transport callbacks.
  void SetOnSendAck(OnSendAck cb) { on_send_ack_ = std::move(cb); }
  // Final path of a published file arrives here; the receive-side conflict
  // policy runs while the engine still holds the completed transfer.
  void SetOnProgress(OnProgress cb) { on_progress_ = std::move(cb); }
  void SetOnReceiveProgress(OnReceiveProgress cb) { on_receive_progress_ = std::move(cb); }
  std::filesystem::path OutputDir() const {
    std::lock_guard lock(directory_mutex_);
    return output_dir_;
  }
  void SetOutputDir(const std::filesystem::path& dir) {
    std::lock_guard lock(directory_mutex_);
    output_dir_ = dir;
    session_.SetDirectory(dir);
  }
  // Desktop (or app storage) used when no receive folder is configured.
  static std::filesystem::path DefaultOutputDirectory() { return GetDefaultDesktopPath(); }

  // 覆盖策略与提问钩子。默认值就是枚举里的 0（kAskSkip）：没装钩子时它等价于
  // 本次改动之前的行为（留下引擎的时间戳名），装了钩子才会问人；管理器每次
  // 建好 receiver 都会把配置里的策略写进来，所以两种取值都不会打架。两者都由
  // conflict_mutex_ 保护，可以在传输进行中改。
  void SetOverwritePolicy(TransferOverwritePolicy policy);
  TransferOverwritePolicy OverwritePolicy() const;
  void SetConflictResolver(ConflictResolver resolver);

  // 跨会话续传的控制面这一半：收端把「这个文件已经有多少字节」写进
  // <resume_directory>/<name>.crossdesk-resume，崩溃或断链之后由 file_browser 的
  // op=resume 回答给发端。默认目录就是接收输出目录、默认开启；关掉之后既不
  // 写也不清，已有的侧车保持原样。
  void SetResumeDirectory(const std::filesystem::path& directory);
  void SetResumeEnabled(bool enabled);
  std::filesystem::path ResumeDirectory() const;
  bool ResumeEnabled() const;

 private:
  // 侧车维护：进度回调里按 4 MiB 节流写、收完清、失败/取消时按暂存数据是否
  // 还在决定保还是清。所有写盘都尽力而为，失败一律忽略，不影响传输本身。
  void ApplyResumeProgress(const file_transfer::ReceiveState& progress);
  bool ShouldPersistResume(const file_transfer::ReceiveState& progress);
  void PersistResumeRecord(const file_transfer::ReceiveState& progress);
  bool PersistResumePrefix(const file_transfer::ReceiveState& progress);
  std::filesystem::path FindStagingPartial(uint32_t wire_id) const;
  void DropResumeRecord(const std::string& name, uint32_t wire_id);
  // 本次传输实际写下的前缀副本路径：收尾时只删它，不按名字猜。
  std::unordered_map<uint32_t, std::filesystem::path> resume_prefix_path_;
  // 引擎的暂存目录是否还在（名字形如 .crossdesk-<wire_id>-<tick>-<serial>）。
  bool StagingPresent(uint32_t wire_id) const;
  // 引擎在会话锁里给出「这个文件已经发布到 <path>」，而冲突询问会阻塞接收线程、
  // Poll() 又要拿那把会话锁：所以发布事件先排队，Receive() 返回之后再处理。
  struct Published {
    std::string name;
    std::filesystem::path path;
  };
  void DrainPublished();
  // kAskSkip 的落点：装着 resolver 就逐个文件问，否则原样返回引擎的名字。
  std::filesystem::path ResolveConflict(const std::filesystem::path& published,
                                        const std::filesystem::path& intended);
  static std::filesystem::path GetDefaultDesktopPath();
  mutable std::mutex directory_mutex_;
  std::filesystem::path output_dir_;
  OnSendAck on_send_ack_;
  OnProgress on_progress_;
  OnReceiveProgress on_receive_progress_;
  // 侧车目录与开关；resume_written_ 是每个 wire id 上一次写侧车时的连续前缀
  // 字节数，只用于节流。三者都由 resume_mutex_ 保护，且与 directory_mutex_
  // 从不嵌套持有。
  mutable std::mutex resume_mutex_;
  std::filesystem::path resume_directory_;
  bool resume_enabled_ = true;
  std::map<uint32_t, uint64_t> resume_written_;
  // 覆盖冲突：策略、提问钩子和待处理的发布事件。conflict_mutex_ 是叶子锁，
  // 只在取/放钩子与排队时短暂持有——绝不在等用户回答的时候拿着它。
  mutable std::mutex conflict_mutex_;
  TransferOverwritePolicy overwrite_policy_ = TransferOverwritePolicy::kAskSkip;
  ConflictResolver conflict_resolver_;
  std::vector<Published> published_;
  file_transfer::Session session_;
};

}  // namespace crossdesk

#endif
