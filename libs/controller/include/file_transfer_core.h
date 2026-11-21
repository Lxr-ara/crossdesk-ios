/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-16
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FILE_TRANSFER_CORE_H_
#define _FILE_TRANSFER_CORE_H_

#include <file_transfer_format.h>
#include <file_transfer_resume.h>
#include <stream_names.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace crossdesk::file_transfer {

// UI, RTC peers and platform permissions stay in the adapters. Only mobile
// adapters transfer ownership of disposable picker copies to this engine.
struct Options {
  // 接收端的跨会话续传上下文。侧车目录由宿主层（桌面 / iOS / Android 适配层）
  // 注入，引擎自己不知道暂存目录那个随机名。enabled 为 false 时 offset != 0
  // 的首块仍然一律 reject()，也就是本次改动之前的行为。
  struct ResumeContext {
    std::filesystem::path directory;
    bool enabled = false;
  };
  bool remove_source = false;
  bool receive_subdirectory = false;
  size_t max_incoming = 0;
  // UU style ByteTokenBucket pacing for outgoing chunks. 0 keeps the unpaced
  // path, so existing adapters transfer at transport speed.
  uint64_t send_rate_bytes_per_second = 0;
  // 追加在末尾：移动端适配层用聚合初始化 {true, true, 4} 构造 Options，尾部
  // 新增成员才能让那些调用点原样编译。
  ResumeContext resume;
};

enum class Status { Idle, Sending, Completed, Failed, Cancelled };
struct SendState {
  uint32_t id = 0;
  uint64_t total = 0, acknowledged = 0;
  Status status = Status::Idle;
};

enum class ReceiveStatus { Receiving, Completed, Failed, Cancelled };
struct ReceiveState {
  uint32_t id = 0;
  std::string name;
  uint64_t total = 0, received = 0;
  ReceiveStatus status = ReceiveStatus::Receiving;
};

class Session {
 public:
  using Clock = std::chrono::steady_clock;
  using Send = std::function<int(const char*, const char*, size_t)>;
  // Progress callbacks must not reenter the session. Transport callbacks run
  // outside its mutex, allowing immediate/reentrant ACKs and concurrent peers.
  using Progress = std::function<void(const std::string&, double, bool,
                                      const std::string&)>;
  // Exact accepted bytes and terminal state, keyed by this peer's wire ID.
  using ReceiveProgress = std::function<void(const ReceiveState&)>;
  Session(Send send, Progress progress, Options options = {},
          ReceiveProgress receive_progress = {})
      : send_(std::move(send)), progress_(std::move(progress)),
        receive_progress_(std::move(receive_progress)), options_(options),
        send_rate_(options_.send_rate_bytes_per_second) {}
  ~Session() { Clear(); }

  static uint32_t NextFileId() {
    static std::atomic<uint32_t> next{1};
    uint32_t id;
    do { id = next.fetch_add(1); } while (id == 0);
    return id;
  }
  void SetDirectory(std::filesystem::path directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    directory_ = std::move(directory);
  }
  // 宿主层注入续传上下文（侧车目录 + 开关）。运行期可改：清空目录或关掉开关
  // 就等于退回「offset != 0 一律 reject()」的老行为，不需要重建 Session。
  void SetResumeContext(const Options::ResumeContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_.resume = context;
  }
  Options::ResumeContext resume_context() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_.resume;
  }
  // A browser destination belongs to one wire ID, never to the whole peer.
  // Keep reservations until consumed: other queued files may take hours.
  bool ReserveDirectory(uint32_t id, const std::filesystem::path& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !id || incoming_.count(id) || cancelled_incoming_.count(id) ||
        destinations_.size() >= 8192)
      return false;
    return destinations_.emplace(id, directory).second;
  }
  // 跨会话续传的发送入口。resume_offset 是本机（由控制面握手）认定、对端侧车
  // 已经拥有的前缀长度：输入流直接 seek 到那里，本次会话的第一块就从那个偏移
  // 开始发。线格式一个字节都不动 —— 偏移本来就在 FileChunkHeader.offset 里，
  // 文件名也仍然只跟着本次会话的第一块走。
  bool Start(const std::string& path, const std::string& remote_name, uint64_t total_size,
             Clock::time_point now = Clock::now(), uint32_t id = 0,
             uint64_t resume_offset = 0) {
    return StartFrom(NamePath(path), remote_name, total_size, now, id, resume_offset);
  }
  // 旧签名保留：本地路径已经解析、总长度取自本地文件、起点固定 0。现有的适配器
  // 与回归测试（file_transfer_rate_test / file_transfer_queue_test /
  // controller_files / file_transfer_manager / clipboard_file_transfer /
  // 移动端 FileTransferSession）继续走这条路。
  bool Start(const std::filesystem::path& path, const std::string& name,
             Clock::time_point now = Clock::now(), uint32_t id = 0) {
    return StartFrom(path, name, 0, now, id, 0);
  }

  // At most one chunk per pump; bound queued but unacknowledged bytes to 1 MiB.
  bool Pump(Clock::time_point now = Clock::now()) {
    std::vector<FileTransferAck> expired;
    std::vector<char> chunk;
    uint32_t id = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return false;
      for (auto it = cancelled_incoming_.begin(); it != cancelled_incoming_.end();) {
        if (now - it->second >= timeout_) it = cancelled_incoming_.erase(it);
        else ++it;
      }
      for (auto it = incoming_.begin(); it != incoming_.end();) {
        if (now - it->second.updated < timeout_) { ++it; continue; }
        expired.push_back({kFileAckMagic, it->first, it->second.received, it->second.total, 2});
        NotifyReceive(it->first, it->second, ReceiveStatus::Failed);
        Notify(it->second.name, -1, false, "");
        Remove(it->second); it = incoming_.erase(it);
      }
      if (outgoing_) {
        auto& file = *outgoing_;
        if (now - file.updated >= timeout_) FinishSend(false);
        else if (send_paused_) {
          // Paused: queue nothing, but keep the deadline fresh so this session
          // never fails its own transfer while the pause is held. ACK handling
          // and receive side bookkeeping stay untouched.
          file.updated = now;
        } else if (RefillTokens(now) && !file.last && file.sent - file.acked < 1024 * 1024) {
          std::array<char, kFileChunkSize> payload{};
          const auto size = static_cast<uint32_t>((std::min<uint64_t>)(payload.size(), file.total - file.sent));
          if (size) file.input.read(payload.data(), size);
          if (size && file.input.gcount() != size) FinishSend(false);
          else {
            // 首块判定从「偏移为 0」改成「本次会话的第一块」：起点为 0 时
            // 与旧实现逐字节一致；续传时 first 位也必须置上 —— 接收端只允许
            // 首块携带文件名，而且完成判定要求先见过一次首块。
            const bool first = file.sent == file.start;
            const bool last = file.sent + size == file.total;
            chunk = EncodeFileChunk(file.id, file.sent, file.total, payload.data(), size,
                                    first ? &file.name : nullptr, first, last);
            if (chunk.empty()) FinishSend(false);
            else {
              id = file.id;
              // Reserve before calling transport: an ACK can arrive before Send returns.
              file.sent += size; file.last = last;
              // Chunks are never split, so the whole chunk is charged at once.
              if (send_rate_) tokens_ -= static_cast<double>(size);
            }
          }
        }
      }
    }
    for (const auto& ack : expired) SendAck(ack);
    if (!chunk.empty() && send_(kFileStream, chunk.data(), chunk.size()) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (outgoing_ && outgoing_->id == id) FinishSend(false);
    }
    return !chunk.empty();
  }

  // UU pacing knob: 0 is unlimited (transport speed), any other value caps the
  // outgoing stream and holds at most one second of credit in the bucket.
  void SetSendRate(uint64_t bytes_per_second) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (send_rate_ == bytes_per_second) return;
    send_rate_ = bytes_per_second;
    // Drop stale credit: a new rate must not be primed by the old one. The
    // anchor stays on the caller's clock (Pump supplies it) so a fake clock
    // keeps working after a rate change.
    tokens_ = 0;
  }
  uint64_t send_rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return send_rate_;
  }
  // Pausing only stops new chunks. ACK handling, receive side timeouts and
  // cancelled tombstones keep running; Pump refreshes the sender deadline so a
  // long pause does not fail this session's own transfer.
  void SetSendPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    send_paused_ = paused;
  }
  bool send_paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return send_paused_;
  }

  void Ack(const char* data, size_t size, Clock::time_point now = Clock::now()) {
    FileTransferAck ack{};
    if (!DecodeFileTransferAck(data, size, &ack)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outgoing_ || ack.file_id != outgoing_->id) return;
    auto& file = *outgoing_;
    if (ack.total_size != file.total || ack.acked_offset > file.sent || (ack.flags & ~3u)) return;
    if (ack.flags & 2) { FinishSend(false); return; }
    if (ack.flags & 1) {
      if (file.last && ack.acked_offset == file.total) {
        state_.acknowledged = file.total;
        FinishSend(true);
      }
      return;
    }
    if (ack.acked_offset <= file.acked) return;
    file.acked = ack.acked_offset; file.updated = now;
    state_.acknowledged = file.acked;
    Report(file.name, file.acked, file.total, true, file.percent);
  }

  bool Receive(const char* data, size_t size, Clock::time_point now = Clock::now()) {
    FileChunkView chunk;
    if (!DecodeFileChunk(data, size, &chunk)) return false;
    FileTransferAck ack;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return false;
      ack = ReceiveLocked(chunk, size, now);
    }
    SendAck(ack);
    return (ack.flags & 2) == 0;
  }
  bool Sending() const { return Snapshot().status == Status::Sending; }
  SendState Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }
  // Stop only the outgoing file. Keep the peer usable for subsequent sends.
  // The adapter calls this between pumps; late/reordered chunks are discarded
  // by the receiver's cancellation tombstone.
  bool CancelSend() {
    std::vector<char> packet;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!outgoing_) return false;
      packet = EncodeFileCancel(outgoing_->id, outgoing_->total);
      FinishSend(false, true);
    }
    if (send_) send_(kFileStream, packet.data(), packet.size());
    return true;
  }
  void CancelReceive(uint32_t id) {
    uint64_t total = 0;
    {
      std::lock_guard lock(mutex_);
      const auto found = incoming_.find(id);
      if (found != incoming_.end()) total = found->second.total;
    }
    const auto packet = EncodeFileCancel(id, total);
    Receive(packet.data(), packet.size());
  }
  // A cancelled connection cannot be restarted by a late data callback.
  void Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    if (outgoing_) FinishSend(false);
    for (const auto& [id, file] : incoming_)
      NotifyReceive(id, file, ReceiveStatus::Failed);
    Clear();
  }

 private:
  struct Outgoing {
    uint32_t id = 0;
    std::string name;
    std::filesystem::path path;
    std::ifstream input;
    // 本次会话的发送起点：跨会话续传时等于侧车认可的前缀，普通传输恒为 0。
    uint64_t start = 0;
    uint64_t total = 0, sent = 0, acked = 0;
    bool last = false;
    int percent = -1;
    Clock::time_point updated;
  };
  struct Incoming {
    std::string name;
    std::filesystem::path path, partial, folder, directory;
    std::ofstream output;
    uint64_t total = 0, received = 0;
    bool first = false, last = false;
    std::map<uint64_t, uint64_t> ranges;
    int percent = -1;
    Clock::time_point updated;
  };
  // total_size 为 0 表示「按本地文件大小决定」（旧签名走这条路）。resume_offset
  // 必须落在 [0, total_size) 内，并且本地文件真的至少有这么多字节：半真半假的
  // 起点只会拼出一个前后不一致的文件，宁可在入口拒绝。
  bool StartFrom(const std::filesystem::path& path, const std::string& name,
                 uint64_t total_size, Clock::time_point now, uint32_t id,
                 uint64_t resume_offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || outgoing_ || !send_ || name.empty() || name.size() > UINT16_MAX) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    const auto local_size = std::filesystem::file_size(path, ec);
    if (ec || local_size > INT64_MAX) return false;
    if (!total_size) total_size = static_cast<uint64_t>(local_size);
    // 越界起点，以及「本地文件根本没有这么多字节」的起点都不成立。
    if (resume_offset > total_size || resume_offset > local_size) return false;
    // 起点等于总长度表示没有剩余字节，这不是一次传输：会话保持空闲。只在真的
    // 续传（offset > 0）时成立，0 字节文件的老路径不受影响。
    if (resume_offset && resume_offset == total_size) return false;
    Outgoing file;
    file.input.open(path, std::ios::binary);
    if (!file.input) return false;
    if (resume_offset) {
      // 起点之后的字节才需要发；seek 失败说明本地文件已经不可信。
      file.input.seekg(static_cast<std::streamoff>(resume_offset), std::ios::beg);
      if (!file.input) return false;
    }
    file.id = id ? id : NextFileId();
    file.name = name; file.path = path; file.total = total_size; file.updated = now;
    file.start = resume_offset; file.sent = resume_offset; file.acked = resume_offset;
    state_ = {file.id, total_size, resume_offset, Status::Sending};
    outgoing_ = std::move(file);
    // A fresh transfer starts with an empty bucket anchored on the caller's
    // clock, so pacing never inherits credit from the previous file.
    tokens_ = 0;
    tokens_at_ = now;
    Notify(name, 0, true, "");
    return true;
  }
  // 一条本机认可的续传前缀：offset 是侧车里「已经从 0 起连续收到」的字节数，
  // prefix 是那份字节在侧车目录里的位置。
  struct ResumeHit {
    uint64_t offset = 0;
    std::filesystem::path prefix;
  };
  // 续传首块的放行谓词，只有这一条路径能让 offset != 0 的首块进入接收状态机：
  //   ① Options::resume 已启用且目录非空（宿主层注入，引擎不猜路径）；
  //   ② 该名字在侧车目录里有一条可读记录（ReadResumeRecord 已经校验过版本、
  //      JSON 结构、target_path 的 basename 与 received <= total_size）；
  //   ③ 记录的 total_size 等于首块声明的总长度；
  //   ④ 记录的 received 等于首块的 offset —— 对端只能停在「我本机已有的位置」；
  //   ⑤ 首块本身还没结束（0 < offset < total_size）。
  // 其余一切情况返回空，调用方维持原来的 reject()。
  std::optional<ResumeHit> ResumedStart(const FileChunkView& chunk) const {
    const auto& h = chunk.header;
    if (!options_.resume.enabled || options_.resume.directory.empty()) return std::nullopt;
    if (h.offset == 0 || h.offset >= h.total_size) return std::nullopt;
    const auto name = SafeName(chunk.file_name);
    if (name.empty()) return std::nullopt;
    std::string error;
    // Unchecked: the receive side records the prefix before publish, so the final
    // target is not there yet. The durable prefix size is the real proof.
    const auto record = ReadResumeRecordUnchecked(options_.resume.directory, name, &error);
    if (!record || record->total_size != h.total_size || record->received != h.offset)
      return std::nullopt;
    const auto prefix_bytes = ResumePrefixBytes(options_.resume.directory, *record);
    if (!prefix_bytes || *prefix_bytes < record->received) return std::nullopt;
    // A record that carries a real stamp must still match the prefix on disk,
    // otherwise the bytes we would replay are not the ones the record promised.
    if (record->target_mtime_ms > 0 &&
        TargetMtimeMs(options_.resume.directory / NamePath(record->target_path)) !=
            record->target_mtime_ms)
      return std::nullopt;
    ResumeHit hit;
    hit.offset = record->received;
    hit.prefix = options_.resume.directory / NamePath(record->target_path);
    return hit;
  }
  // 把侧车认可的前缀搬进本次传输的暂存文件。只搬 [0, offset)：记录里
  // offset 之后的字节不在任何校验范围内，必须等数据块来覆盖，绝不能当成已收。
  bool SeedResumedPrefix(Incoming& file, const ResumeHit& hit) {
    if (!hit.offset) return true;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(hit.prefix, ec)) return false;
    std::ifstream input(hit.prefix, std::ios::binary);
    if (!input) return false;
    std::ofstream output(file.partial, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    std::array<char, kFileChunkSize> buffer{};
    for (uint64_t copied = 0; copied < hit.offset;) {
      const auto want = static_cast<std::streamsize>(
          (std::min<uint64_t>)(buffer.size(), hit.offset - copied));
      input.read(buffer.data(), want);
      if (input.gcount() != want) { output.close(); return false; }
      output.write(buffer.data(), want);
      if (!output) { output.close(); return false; }
      copied += static_cast<uint64_t>(want);
    }
    output.close();
    return !output.fail();
  }
  void NotifyReceive(uint32_t id, const Incoming& file, ReceiveStatus status) {
    if (receive_progress_)
      receive_progress_({id, file.name, file.total, file.received, status});
  }
  FileTransferAck ReceiveLocked(const FileChunkView& chunk, size_t size, Clock::time_point now) {
    const auto& h = chunk.header;
    auto it = incoming_.find(h.file_id);
    if (h.flags == kFileCancelFlag) {
      const FileTransferAck ack{kFileAckMagic, h.file_id, 0, h.total_size, 2};
      if (size != sizeof(FileChunkHeader) || h.offset || h.name_len ||
          h.chunk_size || h.total_size > INT64_MAX ||
          (it != incoming_.end() && h.total_size != it->second.total))
        return ack;
      cancelled_incoming_[h.file_id] = now;
      destinations_.erase(h.file_id);
      if (it != incoming_.end()) {
        NotifyReceive(h.file_id, it->second, ReceiveStatus::Cancelled);
        Notify(it->second.name, -1, false, "");
        Remove(it->second);
        incoming_.erase(it);
      }
      return ack;
    }
    // KCP receive callbacks can be reordered. Do not recreate a cancelled
    // staging file or a second notification when an in-flight chunk arrives.
    if (cancelled_incoming_.count(h.file_id))
      return {kFileAckMagic, h.file_id, 0, h.total_size, 2};
    auto reject = [&]() {
      destinations_.erase(h.file_id);
      if (it != incoming_.end()) {
        NotifyReceive(h.file_id, it->second, ReceiveStatus::Failed);
        Notify(it->second.name, -1, false, ""); Remove(it->second); incoming_.erase(it);
      } else {
        if (receive_progress_)
          receive_progress_({h.file_id, SafeName(chunk.file_name), h.total_size,
                             0, ReceiveStatus::Failed});
        Notify(SafeName(chunk.file_name), -1, false, "");
      }
      return FileTransferAck{kFileAckMagic, h.file_id, h.offset, h.total_size, 2};
    };
    const bool first = h.flags & 1, last = h.flags & 2;
    // 续传首块：offset != 0 的唯一放行条件是本机侧车认可这个起点（见
    // ResumedStart）。关掉 Options::resume 时这里恒为空，条件退化成原来的
    // 「一律 reject()」，offset == 0 的正常路径逐字节不变。
    std::optional<ResumeHit> resumed;
    if (it == incoming_.end() && first && h.offset != 0) resumed = ResumedStart(chunk);
    if ((h.flags & ~3u) || h.total_size > INT64_MAX ||
        (first && h.offset != 0 && !resumed) ||
        chunk.payload_size > kFileChunkSize || size != sizeof(FileChunkHeader) + h.name_len + chunk.payload_size ||
        (!first && h.name_len) || (!chunk.payload_size && !last) ||
        last != (h.offset + chunk.payload_size == h.total_size) ||
        (it == incoming_.end() && options_.max_incoming && incoming_.size() >= options_.max_incoming))
      return reject();
    if (it == incoming_.end()) {
      Incoming file;
      file.name = SafeName(chunk.file_name); file.total = h.total_size;
      file.directory = directory_.empty() ? std::filesystem::path(".") : directory_;
      if (const auto destination = destinations_.find(h.file_id); destination != destinations_.end()) {
        file.directory = destination->second;
        destinations_.erase(destination);
      }
      std::error_code ec;
      std::filesystem::create_directories(file.directory, ec);
      if (ec || (directory_.empty() && options_.receive_subdirectory)) return reject();
      // Reserve a unique staging directory even when two peers use the same ID.
      static std::atomic<uint64_t> serial{0};
      for (int attempt = 0; attempt < 100; ++attempt) {
        const auto token = std::to_string(h.file_id) + "-" +
            std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
        file.folder = file.directory / (options_.receive_subdirectory ? token : ".crossdesk-" + token);
        if (std::filesystem::create_directory(file.folder, ec)) break;
        if (ec || attempt == 99) return reject();
      }
      file.path = file.folder / NamePath(file.name);
      file.partial = file.folder / ".partial";
      if (resumed) {
        // 续传：先把本机已确认的前缀写进新的暂存文件，再把 [0, offset) 记成
        // 已覆盖。后到的块从 offset 起与它首尾相接，AddRange 会合并成
        // [0, offset + payload)，于是「连续前缀」与完成判定的语义都不用改，
        // 而 [0, offset) 之外的字节一律等数据块覆盖。
        if (!SeedResumedPrefix(file, *resumed)) { Remove(file); return reject(); }
        file.output.open(file.partial, std::ios::binary | std::ios::in | std::ios::out);
        file.ranges.emplace(0, resumed->offset);
        file.received = resumed->offset;
      } else {
        file.output.open(file.partial, std::ios::binary | std::ios::trunc);
      }
      if (!file.output) { Remove(file); return reject(); }
      it = incoming_.emplace(h.file_id, std::move(file)).first;
    }
    auto& file = it->second;
    if (h.total_size != file.total || (first && file.first) ||
        !AddRange(file, h.offset, h.offset + chunk.payload_size)) return reject();
    if (first) {
      file.first = true; file.name = SafeName(chunk.file_name);
      file.path = file.folder / NamePath(file.name);
    }
    file.last |= last;
    // KCP's RTP and timer callbacks can race: ACK only contiguous coverage.
    file.received = !file.ranges.empty() && file.ranges.begin()->first == 0 ? file.ranges.begin()->second : 0;
    const bool complete = file.first && file.last && file.received == file.total;
    file.output.seekp(static_cast<std::streamoff>(h.offset));
    file.output.write(chunk.payload, chunk.payload_size);
    if (complete) { file.output.flush(); file.output.close(); }
    if (file.output.fail() || (complete && !Publish(file))) return reject();
    file.updated = now;
    const FileTransferAck ack{kFileAckMagic, h.file_id, file.received, file.total, complete ? 1u : 0u};
    NotifyReceive(h.file_id, file, complete ? ReceiveStatus::Completed : ReceiveStatus::Receiving);
    if (complete) {
      const auto path = file.path.u8string();
      Notify(file.name, 1, false, std::string(path.begin(), path.end()));
      incoming_.erase(it);
    } else Report(file.name, file.received, file.total, false, file.percent);
    return ack;
  }
  bool Publish(Incoming& file) {
    std::error_code ec;
    if (options_.receive_subdirectory) {
      std::filesystem::rename(file.partial, file.path, ec);
      return !ec;
    }
    // Preserve the desktop's flat destination and timestamp suffix on conflict.
    // Hard-link publication is atomic and never overwrites another peer's file.
    const auto original = NamePath(file.name);
    const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
      auto name = original;
      if (attempt) {
        name = original.stem(); name += "_" + stamp;
        if (attempt > 1) name += "_" + std::to_string(attempt);
        name += original.extension();
      }
      file.path = file.directory / name;
      ec.clear();
      std::filesystem::create_hard_link(file.partial, file.path, ec);
      if (!ec) { Remove(file); return true; }
      if (ec == std::errc::file_exists) continue;
      // Some public folder providers disallow links. Exclusively create the
      // destination so a failed copy can remove only the file we just created.
#if defined(_WIN32)
      std::FILE* output = _wfopen(file.path.c_str(), L"wbx");
#else
      std::FILE* output = std::fopen(file.path.c_str(), "wbx");
#endif
      if (!output) {
        if (errno == EEXIST) continue;
        return false;
      }
      std::ifstream input(file.partial, std::ios::binary);
      bool ok = input.is_open();
      std::array<char, kFileChunkSize> buffer{};
      while (ok && input) {
        input.read(buffer.data(), buffer.size());
        const auto count = static_cast<size_t>(input.gcount());
        if (count && std::fwrite(buffer.data(), 1, count, output) != count) ok = false;
      }
      ok = ok && input.eof() && !input.bad();
      input.close();
      if (std::fclose(output) != 0) ok = false;
      if (!ok) { std::filesystem::remove(file.path, ec); return false; }
      Remove(file); return true;
    }
    return false;
  }
  static bool AddRange(Incoming& file, uint64_t start, uint64_t end) {
    if (start == end) return true;
    auto next = file.ranges.lower_bound(start);
    if (next != file.ranges.end() && next->first < end) return false;
    if (next != file.ranges.begin()) {
      auto previous = std::prev(next);
      if (previous->second > start) return false;
      if (previous->second == start) { start = previous->first; file.ranges.erase(previous); }
    }
    if (next != file.ranges.end() && next->first == end) { end = next->second; file.ranges.erase(next); }
    file.ranges.emplace(start, end);
    return file.ranges.size() <= 1024;
  }
  static std::filesystem::path NamePath(const std::string& name) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(name.begin(), name.end()));
#else
    return std::filesystem::u8path(name);
#endif
  }
  static std::string SafeName(std::string name) {
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    for (auto& ch : name)
      if (static_cast<unsigned char>(ch) < 32 || std::string(":*?\"<>|").find(ch) != std::string::npos) ch = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == ".partial" || name.size() > 240) return "received_file";
    return name;
  }
  static void Remove(Incoming& file) {
    if (file.output.is_open()) file.output.close();
    std::error_code ec;
    std::filesystem::remove(file.partial, ec);
    std::filesystem::remove(file.folder, ec);
  }
  void SendAck(const FileTransferAck& ack) {
    if (!send_) return;
    const auto packet = EncodeFileTransferAck(ack);
    send_(kFileFeedbackStream, packet.data(), packet.size());
  }
  void Notify(const std::string& name, double value, bool sending, const std::string& path) {
    if (progress_) progress_(name, value, sending, path);
  }
  void Report(const std::string& name, uint64_t offset, uint64_t total, bool sending, int& previous) {
    const double progress = total ? static_cast<double>(offset) / total : 0;
    const int percent = static_cast<int>(progress * 100);
    if (percent != previous) { previous = percent; Notify(name, (std::min)(progress, .99), sending, ""); }
  }
  void RemoveSource() {
    outgoing_->input.close();
    if (options_.remove_source) {
      std::error_code ec;
      std::filesystem::remove(outgoing_->path, ec);
    }
    outgoing_.reset();
  }
  // UU ByteTokenBucket: the bucket buffers one second of credit, refilled from
  // the elapsed pump interval, so a burst can never exceed send_rate_. Chunks
  // are never split, so the capacity is clamped to at least one chunk and a
  // rate below one chunk per second still emits one chunk per full credit
  // instead of deadlocking. Caller holds mutex_.
  bool RefillTokens(Clock::time_point now) {
    if (!send_rate_) return true;
    using Seconds = std::chrono::duration<double>;
    const double elapsed = std::chrono::duration_cast<Seconds>(now - tokens_at_).count();
    if (elapsed > 0) {
      const double capacity = static_cast<double>(
          (std::max<uint64_t>)(send_rate_, static_cast<uint64_t>(kFileChunkSize)));
      tokens_ = (std::min)(capacity, tokens_ + elapsed * static_cast<double>(send_rate_));
      tokens_at_ = now;
    }
    // Half a byte of slack absorbs double rounding in the elapsed time product.
    return tokens_ >= static_cast<double>(kFileChunkSize) - 0.5;
  }
  void FinishSend(bool success, bool cancelled = false) {
    state_.status = success ? Status::Completed
        : cancelled ? Status::Cancelled : Status::Failed;
    Notify(outgoing_->name, success ? 1 : -1, true, "");
    RemoveSource();
  }
  void Clear() {
    if (outgoing_) RemoveSource();
    for (auto& entry : incoming_) Remove(entry.second);
    incoming_.clear();
    cancelled_incoming_.clear();
    destinations_.clear();
  }
  const std::chrono::seconds timeout_{30};
  mutable std::mutex mutex_;
  Send send_;
  Progress progress_;
  ReceiveProgress receive_progress_;
  Options options_;
  bool closed_ = false;
  // Outgoing pacing state. send_rate_ 0 means unlimited; tokens_ is the credit
  // left in the bucket and tokens_at_ anchors the elapsed-time refill.
  uint64_t send_rate_ = 0;
  double tokens_ = 0;
  Clock::time_point tokens_at_{};
  bool send_paused_ = false;
  SendState state_;
  std::filesystem::path directory_;
  std::optional<Outgoing> outgoing_;
  std::map<uint32_t, Incoming> incoming_;
  std::map<uint32_t, std::filesystem::path> destinations_;
  std::map<uint32_t, Clock::time_point> cancelled_incoming_;
};

// Outbound queue policy shared by every adapter: how many files of one peer may
// run at once, which items the user held, which rows can be retried, and the
// batch counters UU renders as send_part_success / recv_part_failed.
//
// The adapter owns the transport, the sessions and the display rows; this type
// owns only the bookkeeping that has to behave identically on every platform.
// It performs no I/O and is not thread safe: the caller holds one lock around
// it, which is also what makes the queue policy unit-testable on its own.
class SendQueue {
 public:
  // Paused is derived from a hold and never stored: a held item keeps its slot
  // state, so resuming puts it back exactly where it was.
  enum class State { Queued, Sending, Paused, Completed, Failed, Cancelled };
  struct Item {
    uint32_t id = 0;
    std::string name;
    std::string remote_id;
    uint64_t size = 0;
    State state = State::Queued;
    bool held = false;
    // Enqueue order, so a replay never reorders the visible list.
    uint64_t sequence = 0;
  };
  struct Summary {
    size_t total = 0, completed = 0, failed = 0, active = 0, queued = 0, paused = 0;
    // Some files of this batch finished and some failed: UU's partial success.
    bool batch_partial = false;
  };

  static bool Terminal(State state) {
    return state == State::Completed || state == State::Failed ||
           state == State::Cancelled;
  }

  // Adds a waiting item. A reused id replaces that item's own finished row, so
  // a replay of the same transfer never leaves a ghost behind.
  bool Track(uint32_t id, std::string name, std::string remote_id, uint64_t size) {
    if (!id) return false;
    if (const auto found = items_.find(id); found != items_.end() &&
        !Terminal(found->second.state) && found->second.state != State::Queued)
      return false;
    Item item;
    item.id = id;
    item.name = std::move(name);
    item.remote_id = std::move(remote_id);
    item.size = size;
    item.sequence = ++sequence_;
    items_[id] = std::move(item);
    return true;
  }
  const Item* Find(uint32_t id) const {
    const auto found = items_.find(id);
    return found == items_.end() ? nullptr : &found->second;
  }
  bool Held(uint32_t id) const {
    const Item* item = Find(id);
    return item && item->held;
  }
  bool Tracked(const std::string& remote_id) const {
    return std::any_of(items_.begin(), items_.end(), [&remote_id](const auto& entry) {
      return entry.second.remote_id == remote_id;
    });
  }
  // Only a terminal row can be replayed: an active one is resumed, not retried.
  bool Retryable(uint32_t id) const {
    const Item* item = Find(id);
    return item && Terminal(item->state);
  }
  // Files of this peer that own a slot right now, held or not: a hold keeps the
  // file, its ACKs and its slot, exactly like the engine does.
  size_t Active(const std::string& remote_id) const {
    return static_cast<size_t>(std::count_if(
        items_.begin(), items_.end(), [&remote_id](const auto& entry) {
          return entry.second.state == State::Sending &&
                 entry.second.remote_id == remote_id;
        }));
  }
  size_t Waiting(const std::string& remote_id) const {
    return static_cast<size_t>(std::count_if(
        items_.begin(), items_.end(), [&remote_id](const auto& entry) {
          return entry.second.state == State::Queued &&
                 entry.second.remote_id == remote_id;
        }));
  }
  // Queued -> Sending. A held, terminal or unknown item is refused, which is
  // what keeps a parked or cancelled item out of a slot.
  bool Start(uint32_t id) {
    const auto found = items_.find(id);
    if (found == items_.end() || found->second.state != State::Queued ||
        found->second.held)
      return false;
    found->second.state = State::Sending;
    return true;
  }
  // Sending/Queued -> terminal. A cancelled queued item leaves the wait list
  // through this call, so the scheduler never parks on it.
  bool Finish(uint32_t id, State state) {
    const auto found = items_.find(id);
    if (found == items_.end() || !Terminal(state)) return false;
    found->second.state = state;
    found->second.held = false;
    return true;
  }
  // A hold stops new chunks of an active item and keeps a waiting one out of a
  // slot; it never cancels and never drops the row.
  bool Pause(uint32_t id) {
    const auto found = items_.find(id);
    if (found == items_.end() || Terminal(found->second.state)) return false;
    found->second.held = true;
    return true;
  }
  bool Resume(uint32_t id) {
    const auto found = items_.find(id);
    if (found == items_.end() || !found->second.held) return false;
    found->second.held = false;
    return true;
  }
  size_t PauseAll(const std::string& remote_id) {
    size_t paused = 0;
    for (auto& entry : items_)
      if (entry.second.remote_id == remote_id && !Terminal(entry.second.state)) {
        entry.second.held = true;
        ++paused;
      }
    return paused;
  }
  size_t ResumeAll(const std::string& remote_id) {
    size_t resumed = 0;
    for (auto& entry : items_)
      if (entry.second.remote_id == remote_id && entry.second.held) {
        entry.second.held = false;
        ++resumed;
      }
    return resumed;
  }
  // The existing "clear completed": only terminal rows go away.
  size_t RemoveFinished(const std::string& remote_id) {
    size_t removed = 0;
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->second.remote_id == remote_id && Terminal(it->second.state)) {
        it = items_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }
  // Waiting and finished rows go away; an active item keeps its row and slot.
  size_t ClearAll(const std::string& remote_id) {
    size_t removed = 0;
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->second.remote_id == remote_id &&
          (Terminal(it->second.state) || it->second.state == State::Queued)) {
        it = items_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }
  // Re-queues a terminal item under a fresh id. The receiver keeps a tombstone
  // per cancelled wire id, so a replay must never reuse the old one.
  bool Retry(uint32_t id, uint32_t new_id) {
    const auto found = items_.find(id);
    if (found == items_.end() || !Terminal(found->second.state)) return false;
    if (!new_id || new_id == id || items_.count(new_id)) return false;
    Item item = found->second;
    item.id = new_id;
    item.state = State::Queued;
    item.held = false;
    item.sequence = ++sequence_;
    items_.erase(found);
    items_.emplace(new_id, std::move(item));
    return true;
  }
  void Forget(uint32_t id) { items_.erase(id); }
  void Clear() { items_.clear(); }
  size_t count() const { return items_.size(); }

  Summary Summarize(const std::string& remote_id) const {
    Summary summary;
    for (const auto& entry : items_) {
      const auto& item = entry.second;
      if (item.remote_id != remote_id) continue;
      ++summary.total;
      switch (item.state) {
        case State::Completed: ++summary.completed; break;
        case State::Failed: ++summary.failed; break;
        case State::Sending: ++summary.active; break;
        case State::Queued: if (!item.held) ++summary.queued; break;
        case State::Cancelled: break;
        case State::Paused: break;
      }
      if (item.held && !Terminal(item.state)) ++summary.paused;
    }
    // UU's partial success: at least one file landed and at least one did not.
    summary.batch_partial = summary.completed > 0 && summary.failed > 0;
    return summary;
  }

 private:
  std::map<uint32_t, Item> items_;
  uint64_t sequence_ = 0;
};

}  // namespace crossdesk::file_transfer

#endif
