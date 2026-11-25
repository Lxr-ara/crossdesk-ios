#include "file_transfer.h"

#include <file_transfer_resume.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include "rd_log.h"
#if defined(__OHOS__)
#include "platform/path_backend.h"
#endif

namespace crossdesk {

namespace {

// "report.txt" -> "report (3).txt"
std::filesystem::path IndexedName(const std::filesystem::path& intended,
                                  unsigned index) {
  auto name = intended.stem();
  name += " (" + std::to_string(index) + ")";
  name += intended.extension();
  return intended.parent_path() / name;
}

// 侧车节流：每 64 个 64 KiB 块（4 MiB）落一次。崩溃最多丢 4 MiB 的续传
// 信息，但不会让每一块都做一次「临时文件 + rename」的原子替换。
constexpr uint64_t kResumeStrideChunks = 64;
constexpr uint64_t kResumeStrideBytes =
    kResumeStrideChunks * static_cast<uint64_t>(kFileChunkSize);

std::string Utf8Filename(const std::filesystem::path& path) {
  const auto value = path.filename().u8string();
  return {value.begin(), value.end()};
}

std::string Utf8Path(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}

// 引擎 worker 正在把 resume 配置加进 Options（directory/enabled 两个字段）。
// 在这两个字段落地之前这份代码也必须能编译，所以用 SFINAE 探测成员是否存在
// ——tools 目标仍是 C++17，这里不能用 concept/requires：
//   成员在   -> 把侧车目录和开关一起交给 Session；
//   成员不在 -> 保持引擎原有行为（本适配器自己的侧车照写不误）。
// 探测到的分支只在被选中时才实例化，因此字段缺失不产生编译错误。
template <typename Options, typename = void>
struct ResumeOptions {
  static void Apply(Options&, const std::filesystem::path&, bool) {}
};

template <typename Options>
struct ResumeOptions<
    Options, std::void_t<decltype(std::declval<Options&>().resume.directory),
                         decltype(std::declval<Options&>().resume.enabled)>> {
  static void Apply(Options& options, const std::filesystem::path& directory,
                    bool enabled) {
    options.resume.directory = directory;
    options.resume.enabled = enabled;
  }
};

file_transfer::Options SessionOptions(const std::filesystem::path& directory,
                                     bool enabled) {
  file_transfer::Options options;
  ResumeOptions<file_transfer::Options>::Apply(options, directory, enabled);
  return options;
}

std::filesystem::path PathFromUtf8(const std::string& value) {
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
#else
  return std::filesystem::u8path(value);
#endif
}

// ---------------------------------------------------------- orphan sweep --
using FileClock = std::filesystem::file_time_type::clock;

// 目录的 mtime 只在增删条目时改变，而一次长时间传输只往 .partial 里追加，所以
// 暂存目录本身会显得很旧。判断「还在传」必须看目录里最新的那个条目：两者取更
// 晚的那个。只读、不跟随链接、有界。
std::optional<FileClock::time_point> NewestStamp(const std::filesystem::path& entry,
                                                 bool directory) {
  std::error_code ec;
  auto newest = std::filesystem::last_write_time(entry, ec);
  if (ec) return std::nullopt;
  if (!directory) return newest;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::size_t visited = 0;
  for (std::filesystem::directory_iterator it(entry, options, ec), end;
       !ec && it != end && visited < 64; it.increment(ec)) {
    ++visited;
    std::error_code entry_error;
    if (it->is_symlink(entry_error) || !it->is_regular_file(entry_error)) continue;
    std::error_code stamp_error;
    const auto stamp = std::filesystem::last_write_time(it->path(), stamp_error);
    if (stamp_error) continue;
    if (stamp > newest) newest = stamp;
  }
  return newest;
}

// 一个条目（暂存目录或文件）占用的字节数，用于报告的 bytes_removed。有界，
// 读不出来就算 0：统计失败不能让清理停摆。
uint64_t EntryBytes(const std::filesystem::path& entry) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(entry, ec) && !ec) {
    const auto size = std::filesystem::file_size(entry, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
  }
  ec.clear();
  if (!std::filesystem::is_directory(entry, ec) || ec) return 0;
  uint64_t total = 0;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::size_t visited = 0;
  for (std::filesystem::recursive_directory_iterator it(entry, options, ec), end;
       !ec && it != end && visited < 4096; it.increment(ec)) {
    ++visited;
    std::error_code entry_error;
    if (!it->is_regular_file(entry_error) || entry_error) continue;
    const auto size = std::filesystem::file_size(it->path(), entry_error);
    if (!entry_error) total += static_cast<uint64_t>(size);
  }
  return total;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::filesystem::path ApplyTransferOverwritePolicy(
    const std::filesystem::path& published, const std::filesystem::path& intended,
    TransferOverwritePolicy policy) {
  std::error_code ec;
  if (published.empty() || intended.empty() || published == intended) {
    return published;
  }
  if (policy == TransferOverwritePolicy::kOverwrite) {
    // The engine never replaces an existing file, so overwriting means
    // dropping the old one and publishing under the requested name.
    std::filesystem::remove(intended, ec);
    if (ec) return published;
    ec.clear();
    std::filesystem::rename(published, intended, ec);
    return ec ? published : intended;
  }
  if (policy == TransferOverwritePolicy::kRename) {
    for (unsigned index = 1; index < 1000; ++index) {
      const auto candidate = IndexedName(intended, index);
      ec.clear();
      if (std::filesystem::exists(candidate, ec)) continue;
      // An unreadable destination folder keeps the engine's name.
      if (ec) return published;
      if (std::filesystem::rename(published, candidate, ec); !ec) return candidate;
      return published;
    }
  }
  // Ask/skip keeps the engine's non-destructive timestamped name. kAskSkip only
  // reaches the prompt through FileReceiver::ResolveConflict: this function is
  // pure filesystem work with no channel to the UI.
  return published;
}

OrphanSweepSchedule ResolveOrphanSweepSchedule() {
  OrphanSweepSchedule schedule;
  const char* raw = std::getenv(std::string(kOrphanSweepHoursEnv).c_str());
  if (!raw || !*raw) return schedule;
  char* end = nullptr;
  const double hours = std::strtod(raw, &end);
  if (end == raw || !std::isfinite(hours) || hours < 0) return schedule;
  if (hours <= 0) {
    schedule.enabled = false;
    return schedule;
  }
  // 分数小时也接受：测试里用 0.0001h 就能触发一次扫描，而带 48h 最小年龄的
  // 判定仍然照旧，不会因为缩短间隔就开始删新文件。
  const auto seconds = static_cast<long long>(std::llround(hours * 3600.0));
  schedule.interval = std::chrono::seconds(seconds > 0 ? seconds : 1);
  return schedule;
}

SweepReport SweepOrphans(const std::filesystem::path& receive_dir,
                         std::chrono::seconds min_age, SweepReport* out) {
  SweepReport report;
  const auto finish = [&]() {
    if (out) *out = report;
    return report;
  };
  std::error_code ec;
  if (receive_dir.empty() || !std::filesystem::is_directory(receive_dir, ec) || ec)
    return finish();
  const auto now = FileClock::now();
  const auto min_lifetime = std::chrono::duration_cast<FileClock::duration>(
      min_age.count() > 0 ? min_age : std::chrono::seconds(0));
  const auto stale = [&](FileClock::time_point stamp) {
    return stamp <= now - min_lifetime;
  };
  const std::string staging_prefix(file_transfer::kEngineStagingPrefix);
  const std::string sidecar_suffix(file_transfer::kResumeSidecarSuffix);
  const std::string temp_suffix = sidecar_suffix + ".tmp";
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  // 有界扫描：接收目录里堆了两万个条目也不该把 UI 线程卡住。
  std::size_t visited = 0;
  for (std::filesystem::directory_iterator it(receive_dir, options, ec), end;
       !ec && it != end && visited < 20000; it.increment(ec)) {
    ++visited;
    std::error_code entry_error;
    const auto status = it->symlink_status(entry_error);
    // 链接既不会被跟随，也不会被当成本机的暂存/侧车删掉。
    if (entry_error || std::filesystem::is_symlink(status)) continue;
    const auto path = it->path();
    const std::string name = Utf8Filename(path);
    if (name.empty()) continue;
    if (file_transfer::IsResumeSidecarFilename(name) || EndsWith(name, temp_suffix)) {
      const bool temporary = EndsWith(name, temp_suffix);
      const auto covered = temporary ? temp_suffix : sidecar_suffix;
      const auto stamp = NewestStamp(path, false);
      if (!stamp) continue;
      // 半写成的 .tmp 永远不是有效记录：只按它自己的年龄判断，目标在不在都
      // 说明不了什么（原子替换正好卡在中间）。
      if (temporary &&
          !stale(*stamp)) {
        ++report.skipped_live;
        continue;
      }
      if (!temporary) {
        const auto target_name = name.substr(0, name.size() - covered.size());
        const auto target = receive_dir / PathFromUtf8(target_name);
        std::error_code target_error;
        const bool target_present =
            !target_name.empty() &&
            std::filesystem::is_regular_file(target, target_error) && !target_error;
        if (target_present) {
          // 目标文件（= 侧车指向的那份前缀字节）还在被刷新，或者侧车本身是
          // 刚写的：这次续传仍然有效，记录和前缀一起留着。
          const auto target_stamp = NewestStamp(target, false);
          if ((target_stamp && !stale(*target_stamp)) || !stale(*stamp)) {
            ++report.skipped_live;
            continue;
          }
        }
      }
      // 走到这里只有两种可能：前缀已经不在了（记录再也没法续传），或者两边
      // 都早于 min_age（传输早就死了）。侧车可以删，前缀文件本身不删——那是
      // 用户已经收到的字节，不是元数据。
      const auto bytes = EntryBytes(path);
      std::error_code remove_error;
      std::filesystem::remove(path, remove_error);
      if (remove_error) continue;
      ++report.sidecars_removed;
      report.bytes_removed += bytes;
      continue;
    }
    if (!std::filesystem::is_directory(status)) continue;
    if (name.compare(0, staging_prefix.size(), staging_prefix) != 0) continue;
    if (name.size() <= staging_prefix.size()) continue;
    const auto stamp = NewestStamp(path, true);
    if (!stamp || !stale(*stamp)) {
      // 刚创建或刚开始写的暂存：属于一次仍然活着的传输。
      ++report.skipped_live;
      continue;
    }
    const auto bytes = EntryBytes(path);
    std::error_code remove_error;
    std::filesystem::remove_all(path, remove_error);
    if (remove_error) continue;
    ++report.staging_removed;
    report.bytes_removed += bytes;
  }
  return finish();
}

FileReceiver::FileReceiver() : FileReceiver(GetDefaultDesktopPath()) {}

FileReceiver::FileReceiver(const std::filesystem::path& output_dir)
    : output_dir_(output_dir), resume_directory_(output_dir),
      // 侧车目录/开关随会话一起交给引擎（字段还没落地时这次调用是空操作）。
      session_(
          [this](const char*, const char* data, size_t size) {
            FileTransferAck ack{};
            return on_send_ack_ && DecodeFileTransferAck(data, size, &ack)
                ? on_send_ack_(ack) : 0;
          },
          [this](const std::string& name, double value, bool sending,
                 const std::string& path) {
            // 收端完成事件带的是引擎发布的路径，名字可能已经带上时间戳。冲突
            // 询问会阻塞接收线程，而此刻会话锁还在这条调用链上（Poll() 也要
            // 那把锁），所以先排队，Receive() 返回之后由 DrainPublished 处理。
            if (!sending && value == 1 && !path.empty()) {
              std::lock_guard lock(conflict_mutex_);
              published_.push_back({name, PathFromUtf8(path)});
              return;
            }
            if (on_progress_) on_progress_(name, value, sending, path);
          },
          SessionOptions(resume_directory_, resume_enabled_),
          [this](const file_transfer::ReceiveState& progress) {
            // 先维护侧车，再把进度交给上层：上层回调做什么都不影响侧车。
            ApplyResumeProgress(progress);
            if (on_receive_progress_) on_receive_progress_(progress);
          }) {
  session_.SetDirectory(output_dir);
}

bool FileReceiver::OnData(const char* data, size_t size) {
  const bool accepted = session_.Receive(data, size);
  // 会话锁已经放下：这里才允许问用户（可能阻塞到有人点按钮），再把最终路径
  // 交给上层。没有发布事件时这是一次空调用。
  DrainPublished();
  return accepted;
}

// ------------------------------------------------- overwrite conflict hook --
void FileReceiver::SetOverwritePolicy(TransferOverwritePolicy policy) {
  std::lock_guard lock(conflict_mutex_);
  overwrite_policy_ = policy;
}

TransferOverwritePolicy FileReceiver::OverwritePolicy() const {
  std::lock_guard lock(conflict_mutex_);
  return overwrite_policy_;
}

void FileReceiver::SetConflictResolver(ConflictResolver resolver) {
  std::lock_guard lock(conflict_mutex_);
  conflict_resolver_ = std::move(resolver);
}

void FileReceiver::DrainPublished() {
  std::vector<Published> published;
  {
    std::lock_guard lock(conflict_mutex_);
    if (published_.empty()) return;
    published.swap(published_);
  }
  const auto directory = OutputDir();
  for (const auto& entry : published) {
    auto path = entry.path;
    std::error_code ec;
    // 只有落在接收目录本身（而不是浏览器指定给这次传输的子目录）的文件才走
    // 覆盖策略：和本次改动之前管理器那条 equivalent() 判定逐字一致。
    const bool flat = !directory.empty() && !path.parent_path().empty() &&
                      std::filesystem::equivalent(path.parent_path(), directory, ec) &&
                      !ec;
    if (flat && !entry.name.empty())
      path = ResolveConflict(path, path.parent_path() / PathFromUtf8(entry.name));
    if (on_progress_) on_progress_(entry.name, 1, false, Utf8Path(path));
  }
}

std::filesystem::path FileReceiver::ResolveConflict(
    const std::filesystem::path& published, const std::filesystem::path& intended) {
  if (published.empty() || intended.empty() || published == intended)
    return published;
  TransferOverwritePolicy policy = TransferOverwritePolicy::kAskSkip;
  ConflictResolver resolver;
  {
    std::lock_guard lock(conflict_mutex_);
    policy = overwrite_policy_;
    // 只复制钩子：等用户回答的这段时间绝不拿 conflict_mutex_。
    if (policy == TransferOverwritePolicy::kAskSkip) resolver = conflict_resolver_;
  }
  // 目标名原本空闲时，续传前缀就写在 <name> 上；发布前必须先撤掉这份临时副本，
  // 否则引擎会以为名字被占，按默认策略改名成 `name (1).ext`。
  {
    std::lock_guard lock(resume_mutex_);
    for (auto it = resume_prefix_path_.begin(); it != resume_prefix_path_.end(); ++it) {
      if (it->second == intended) {
        std::error_code remove_ec;
        std::filesystem::remove(it->second, remove_ec);
        resume_prefix_path_.erase(it);
        break;
      }
    }
  }
  // 覆盖 / 重命名保持原样，一个字节都不改。
  if (policy != TransferOverwritePolicy::kAskSkip)
    return ApplyTransferOverwritePolicy(published, intended, policy);
  // 没装钩子：行为和这次改动之前完全一致——留着引擎的时间戳名，不阻塞、不崩。
  if (!resolver) return published;
  std::error_code ec;
  uint64_t size = 0;
  if (std::filesystem::is_regular_file(published, ec) && !ec) {
    const auto measured = std::filesystem::file_size(published, ec);
    if (!ec) size = static_cast<uint64_t>(measured);
  }
  // 顶层看到的是完整的候选目标：文件名、大小和目标目录都由它派生。
  const auto choice = resolver(Utf8Path(intended), size);
  switch (choice) {
    case ConflictChoice::kOverwrite:
    case ConflictChoice::kOverwriteAll:
      return ApplyTransferOverwritePolicy(published, intended,
                                         TransferOverwritePolicy::kOverwrite);
    case ConflictChoice::kRename:
    case ConflictChoice::kRenameAll:
      return ApplyTransferOverwritePolicy(published, intended,
                                         TransferOverwritePolicy::kRename);
    case ConflictChoice::kSkip:
    case ConflictChoice::kSkipAll:
      break;
  }
  return published;
}

// ------------------------------------------------------- resume sidecar --
void FileReceiver::SetResumeDirectory(const std::filesystem::path& directory) {
  std::lock_guard lock(resume_mutex_);
  resume_directory_ = directory;
  // 目录换了，节流基准也失效：下一个进度事件立刻落一条记录。
  resume_written_.clear();
}

void FileReceiver::SetResumeEnabled(bool enabled) {
  std::lock_guard lock(resume_mutex_);
  if (resume_enabled_ == enabled) return;
  resume_enabled_ = enabled;
  resume_written_.clear();
}

std::filesystem::path FileReceiver::ResumeDirectory() const {
  std::filesystem::path directory;
  {
    std::lock_guard lock(resume_mutex_);
    directory = resume_directory_;
  }
  // 未配置时就是接收输出目录：侧车挨着用户最终会看到的那个文件。两把锁不
  // 嵌套持有，directory_mutex_ 永远在 resume_mutex_ 之外。
  return directory.empty() ? OutputDir() : directory;
}

bool FileReceiver::ResumeEnabled() const {
  std::lock_guard lock(resume_mutex_);
  return resume_enabled_;
}

// 进度 -> 侧车。只写本地元数据，不碰数据面：线的格式、块大小、ack 语义不变。
void FileReceiver::ApplyResumeProgress(const file_transfer::ReceiveState& progress) {
  if (!ResumeEnabled()) return;
  using Status = file_transfer::ReceiveStatus;
  switch (progress.status) {
    case Status::Completed:
      // 引擎在 NotifyReceive(Completed) 之前已经 Publish 成功（Publish 失败会
      // 走 Failed），所以此刻落盘的就是最终文件：侧车必须消失，否则下一次会
      // 拿着一个「其实已经收完」的记录去续传。
      DropResumeRecord(progress.name, progress.id);
      return;
    case Status::Failed:
    case Status::Cancelled:
      // 断链/失败正是续传存在的理由：暂存数据还在就保留记录，并顺手补写一条
      // 最新的前缀；引擎已经把暂存目录删掉了，就必须一起清掉，否则下一次是
      // 从一个空文件「续传」。
      if (StagingPresent(progress.id)) PersistResumeRecord(progress);
      else DropResumeRecord(progress.name, progress.id);
      return;
    case Status::Receiving:
      if (ShouldPersistResume(progress)) PersistResumeRecord(progress);
      return;
  }
}

// 节流：第一次进度立刻落一条，之后每 4 MiB（64 个 64 KiB 块）落一次；收到量
// 回退（同一个 wire id 重新从 0 开始的重试）时也立刻落。
bool FileReceiver::ShouldPersistResume(
    const file_transfer::ReceiveState& progress) {
  std::lock_guard lock(resume_mutex_);
  const auto found = resume_written_.find(progress.id);
  if (found == resume_written_.end()) return true;
  if (progress.received < found->second) return true;
  return progress.received - found->second >= kResumeStrideBytes;
}

// Copies the first `received` bytes of this transfer's staging file into the
// resume directory under the final name. That durable copy is what a later
// resume replays (and what the host `resume` op validates), so it must exist
// before the sidecar that points at it.
bool FileReceiver::PersistResumePrefix(const file_transfer::ReceiveState& progress) {
  if (!progress.received || progress.name.empty()) return true;  // nothing to keep yet
  const auto directory = ResumeDirectory();
  if (directory.empty()) return false;
  const auto staging = FindStagingPartial(progress.id);
  if (staging.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  // 目标名已经被占用时，绝不能把用户已有的文件截断后再写「续传前缀」：那会让
  // 冲突弹窗里的「跳过 / 重命名」变成一句空话（原文件已经没了）。这种情况把前缀
  // 写到 .crossdesk-prefix-<name>，发布时再由覆盖策略决定最终落点。
  // 前缀永远写在旁路名称上，一次也不写最终名：写在最终名上会让引擎发布时
  // 看到「名字已被占用」，把一份全新的文件改名成 name_<stamp>.ext；名字本来就
  // 被占用的场景还会顺手截断用户的旧文件。旁路副本收尾时由建表的那一方删。
  auto target =
      directory / std::filesystem::path(".crossdesk-prefix-" + progress.name);
  // 记住这次到底写了哪个路径：收尾时只删自己写的那份，绝不去碰最终文件。
  {
    std::lock_guard lock(resume_mutex_);
    resume_prefix_path_[progress.id] = target;
  }
  ec.clear();
  // Reuse the previous copy when it already covers this prefix: rewriting a large
  // prefix on every 4 MiB boundary would double the disk traffic.
  const auto existing = std::filesystem::file_size(target, ec);
  if (!ec && existing >= progress.received) return true;
  ec.clear();
  std::ifstream input(staging, std::ios::binary);
  if (!input) return false;
  std::ofstream output(target, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  std::array<char, 64 * 1024> buffer{};
  uint64_t copied = 0;
  while (copied < progress.received) {
    const auto want = static_cast<std::streamsize>(
        (std::min<uint64_t>)(buffer.size(), progress.received - copied));
    input.read(buffer.data(), want);
    if (input.gcount() != want) { output.close(); return false; }
    output.write(buffer.data(), want);
    if (!output) { output.close(); return false; }
    copied += static_cast<uint64_t>(want);
  }
  output.close();
  return !output.fail();
}

void FileReceiver::PersistResumeRecord(const file_transfer::ReceiveState& progress) {
  if (progress.name.empty() || !progress.total ||
      progress.received > progress.total)
    return;
  const auto directory = ResumeDirectory();
  if (directory.empty()) return;
  {
    std::lock_guard lock(resume_mutex_);
    if (!resume_enabled_) return;
    resume_written_[progress.id] = progress.received;
  }
  file_transfer::ResumeRecord record;
  // 目标就是最终文件名（接收目录里的相对名）：侧车按它的 basename 寻址。
  record.target_path = progress.name;
  record.total_size = progress.total;
  // 发布之前没有最终文件的时间戳：0 表示「不知道」，读取方不会拿它当条件。
  record.target_mtime_ms = 0;
  record.received = progress.received;
  record.wire_id = progress.id;
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  // Order matters: the prefix bytes are the freshness proof, so a sidecar that
  // points at a missing prefix would be ignored (worse, it would look corrupt).
  if (!PersistResumePrefix(progress)) return;
  // 尽力而为：写侧车失败绝不能反过来影响传输本身，所以错误串直接丢掉。
  file_transfer::WriteResumeRecord(directory, record, nullptr);
}

void FileReceiver::DropResumeRecord(const std::string& name, uint32_t wire_id) {
  {
    std::lock_guard lock(resume_mutex_);
    resume_written_.erase(wire_id);
  }
  const auto directory = ResumeDirectory();
  if (directory.empty() || name.empty()) return;
  file_transfer::ClearResumeRecord(directory, name);
  // 前缀副本属于这条记录，留着会让下一次「续传」到没有侧车描述的字节上；但它
  // 也可能是最终文件本身（目标名原本空闲时前缀就写在那个名字上），所以只删本次
  // 真正写过的路径，绝不按名字猜。
  std::error_code ec;
  {
    std::lock_guard lock(resume_mutex_);
    const auto found = resume_prefix_path_.find(wire_id);
    if (found != resume_prefix_path_.end()) {
      std::filesystem::remove(found->second, ec);
      resume_prefix_path_.erase(found);
    }
  }
  // 目标名被占用时前缀用的暂存形态：没有记录也能安全清掉（它永远不是用户的数据）。
  std::filesystem::remove(
      directory / std::filesystem::path(".crossdesk-prefix-" + name), ec);
}

// 引擎的暂存目录名是 ".crossdesk-<wire_id>-<tick>-<serial>"（file_transfer_core.h
// 的 ReceiveLocked 里拼出来的），wire id 在同一个 Session 内唯一，所以这个前缀
// 就够定位本次传输的暂存。这里只做有界、不跟随链接的探测，绝不改动任何东西。
std::filesystem::path FileReceiver::FindStagingPartial(uint32_t wire_id) const {
  const auto root = OutputDir();
  if (root.empty() || !wire_id) return {};
  const auto prefix = ".crossdesk-" + std::to_string(wire_id) + "-";
  std::error_code ec;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::size_t visited = 0;
  for (std::filesystem::recursive_directory_iterator it(root, options, ec), end;
       !ec && it != end && visited < 4096; it.increment(ec)) {
    ++visited;
    std::error_code status_error;
    const auto status = it->symlink_status(status_error);
    if (status_error) continue;
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) continue;
    const auto name = Utf8Filename(it->path());
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
    const auto partial = it->path() / ".partial";
    std::error_code partial_error;
    if (std::filesystem::is_regular_file(partial, partial_error)) return partial;
  }
  return {};
}

bool FileReceiver::StagingPresent(uint32_t wire_id) const {
  const auto root = OutputDir();
  if (root.empty() || !wire_id) return false;
  const auto prefix = ".crossdesk-" + std::to_string(wire_id) + "-";
  std::error_code ec;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::size_t visited = 0;
  for (std::filesystem::recursive_directory_iterator it(root, options, ec), end;
       !ec && it != end && visited < 4096; it.increment(ec)) {
    ++visited;
    std::error_code status_error;
    const auto status = it->symlink_status(status_error);
    if (status_error) continue;
    // 只认真目录：链接既不被跟随，也不会被当成暂存。
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status))
      continue;
    const auto name = Utf8Filename(it->path());
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
      return true;
  }
  return false;
}

std::filesystem::path FileReceiver::GetDefaultDesktopPath() {
#if defined(__OHOS__)
  // Harmony has no process HOME/Desktop. Use writable application storage
  // until the user selects and authorizes a public directory in settings.
  const auto desktop_path = platform::GetCachePath("CrossDesk") / "Received Files";
#else
#ifdef _WIN32
  const char* home_env = std::getenv("USERPROFILE");
#else
  const char* home_env = std::getenv("HOME");
#endif
  if (!home_env) {
    return std::filesystem::path{};
  }

  std::filesystem::path desktop_path =
      std::filesystem::path(home_env) / "Desktop";
#endif

  std::error_code ec;
  std::filesystem::create_directories(desktop_path, ec);
  if (ec) {
    LOG_ERROR("FileReceiver: failed to create desktop directory: {}",
              ec.message().c_str());
  }

  return desktop_path;
}

}  // namespace crossdesk
