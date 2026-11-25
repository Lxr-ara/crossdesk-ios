/*
 * Cross-session resume sidecar.
 *
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FILE_TRANSFER_RESUME_H_
#define _FILE_TRANSFER_RESUME_H_

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Header-only metadata layer for resumable file transfer. It knows nothing
// about the GUI, the peer layer or the wire format: it only persists "this
// partial file already holds N of total M bytes" next to that partial file, so
// a later session can decide whether the prefix is still trustworthy.
//
// The engine stages each incoming file in a random .crossdesk-<id>-<tick>-
// <serial> directory as ".partial" (file_transfer_core.h:349-359). A sidecar
// therefore lives inside that directory, named "<file>.crossdesk-resume" for
// the file it describes. Nothing here changes FileChunkHeader or
// FileTransferAck, and nothing here negotiates an offset with the peer: a
// sidecar only tells a local receiver what it already has.
//
// A record is addressed by the *basename* of target_path (target_path may still
// carry subdirectories, which must stay inside the same directory). Two targets
// sharing a basename inside one directory therefore share one sidecar: the last
// writer wins and Read validates the basename, so a mismatch is never silently
// trusted.
namespace crossdesk::file_transfer {

// One resumable prefix. target_path is relative to the directory that holds
// the sidecar; total_size/target_mtime_ms must still describe that target or
// the record is stale.
struct ResumeRecord {
  std::string target_path;
  uint64_t total_size = 0;
  int64_t target_mtime_ms = 0;
  uint64_t received = 0;
  uint32_t wire_id = 0;
};

// ".crossdesk-resume" is a suffix, never a prefix: the engine's staging
// directories are ".crossdesk-<digits>-<digits>-<digits>" (a prefix plus a
// numeric tail), so a sidecar name can never be read as a staging directory.
inline constexpr std::string_view kResumeSidecarSuffix = ".crossdesk-resume";
inline constexpr std::string_view kEngineStagingPrefix = ".crossdesk-";
inline constexpr int64_t kResumeSidecarVersion = 1;
// A record is a fixed five-field object; anything larger is not ours.
inline constexpr std::size_t kResumeSidecarMaxBytes = 8192;
// Leaves room for the suffix and the ".tmp" write target on a 255 byte limit.
inline constexpr std::size_t kResumeNameMaxBytes = 200;

namespace resume_detail {

using Json = nlohmann::json;

// Same guard as file_transfer_core.h's NamePath: char8_t exists in C++20 and
// the standard library may still be built for C++17 without it.
inline std::filesystem::path PathFromUtf8(std::string_view value) {
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
#else
  return std::filesystem::u8path(value.begin(), value.end());
#endif
}

inline std::string Utf8Of(const std::filesystem::path& path) {
  const auto value = path.u8string();
#if defined(__cpp_char8_t)
  return std::string(value.begin(), value.end());
#else
  return std::string(value);
#endif
}

inline void Fail(std::string* error, std::string_view reason) {
  if (error) *error = std::string(reason);
}

inline void Succeed(std::string* error) {
  if (error) error->clear();
}

// nlohmann parses a small non-negative integer as number_integer, not as
// number_unsigned, so a field written from uint64_t comes back in the signed
// branch. Accept both and reject only negative or unrepresentable values.
inline bool ReadUnsigned(const Json& json, const char* key, uint64_t* out) {
  const auto field = json.find(key);
  if (field == json.end() || out == nullptr) return false;
  if (field->is_number_unsigned()) {
    *out = field->get<uint64_t>();
    return true;
  }
  if (!field->is_number_integer()) return false;
  const auto value = field->get<int64_t>();
  if (value < 0) return false;
  *out = static_cast<uint64_t>(value);
  return true;
}

inline bool ReadSigned(const Json& json, const char* key, int64_t* out) {
  const auto field = json.find(key);
  if (field == json.end() || out == nullptr) return false;
  if (field->is_number_unsigned()) {
    const auto value = field->get<uint64_t>();
    if (value > static_cast<uint64_t>(INT64_MAX)) return false;
    *out = static_cast<int64_t>(value);
    return true;
  }
  if (!field->is_number_integer()) return false;
  *out = field->get<int64_t>();
  return true;
}

// A base name is the address of a record, so it has to survive a round trip
// through the filesystem without gaining a path component.
inline bool ValidName(const std::string& name) {
  if (name.empty() || name.size() > kResumeNameMaxBytes) return false;
  if (name == "." || name == "..") return false;
  if (name.find('/') != std::string::npos) return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  // Reserved for the engine's staging directories.
  if (name.compare(0, kEngineStagingPrefix.size(), kEngineStagingPrefix) == 0) return false;
  // No double suffix: "<x>.crossdesk-resume.crossdesk-resume" is ambiguous.
  if (name.size() > kResumeSidecarSuffix.size() &&
      name.compare(name.size() - kResumeSidecarSuffix.size(), kResumeSidecarSuffix.size(),
                   kResumeSidecarSuffix) == 0)
    return false;
  for (const char ch : name) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 32) return false;
    if (std::string(":*?\"<>|").find(ch) != std::string::npos) return false;
  }
  if (name.back() == '.' || name.back() == ' ') return false;
  return true;
}

// target_path must stay inside the directory that holds the sidecar. This is a
// lexical check only: no component may escape, and no engine staging prefix is
// allowed inside, so a record can never point at another transfer's staging.
inline bool ValidRelativeTarget(const std::string& target_path) {
  if (target_path.empty() || target_path.size() > 1024) return false;
  if (target_path.find('\0') != std::string::npos) return false;
  std::error_code ec;
  const auto path = PathFromUtf8(target_path);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
  bool components = false;
  for (const auto& component : path) {
    const auto text = Utf8Of(component);
    if (text.empty() || text == "." || text == "..") return false;
    if (text.compare(0, kEngineStagingPrefix.size(), kEngineStagingPrefix) == 0) return false;
    if (!ValidName(text)) return false;
    components = true;
  }
  return components;
}

inline std::string SidecarName(const std::string& name) {
  return name.empty() ? std::string() : name + std::string(kResumeSidecarSuffix);
}

// The temporary used for the atomic replace. It ends with ".tmp", so listing
// never mistakes a half-written sidecar for a record.
inline std::string TempName(const std::string& name) {
  return name.empty() ? std::string() : name + std::string(kResumeSidecarSuffix) + ".tmp";
}

struct TargetState {
  bool present = false;
  uint64_t size = 0;
  int64_t mtime_ms = 0;
};

// A symlink is reported as absent: freshness must describe the bytes this
// directory owns, not whatever the link points at.
inline TargetState ReadTargetState(const std::filesystem::path& dir,
                                   const std::string& target_path) {
  TargetState state;
  std::error_code ec;
  const auto path = dir / PathFromUtf8(target_path);
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    return state;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return state;
  const auto stamp = std::filesystem::last_write_time(path, ec);
  if (ec) return state;
  state.present = true;
  state.size = static_cast<uint64_t>(size);
  state.mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      stamp.time_since_epoch()).count();
  return state;
}

// Shared reader. `check_freshness` false is used by ListResumeRecords, which
// enumerates sidecars (including stale and orphaned ones) so a cleanup caller
// can reclaim them; the public Read additionally verifies the target.
inline std::optional<ResumeRecord> ReadRecord(const std::filesystem::path& dir,
                                              const std::string& name, bool check_freshness,
                                              std::string* error) {
  if (!ValidName(name)) {
    Fail(error, "invalid_name");
    return std::nullopt;
  }
  const auto sidecar = dir / PathFromUtf8(SidecarName(name));
  std::error_code ec;
  if (!std::filesystem::is_regular_file(sidecar, ec)) {
    Fail(error, "not_found");
    return std::nullopt;
  }
  std::ifstream input(sidecar, std::ios::binary);
  if (!input) {
    Fail(error, "io_error");
    return std::nullopt;
  }
  std::string text(kResumeSidecarMaxBytes + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  const auto read = static_cast<std::size_t>(input.gcount());
  if (input.bad()) {
    Fail(error, "io_error");
    return std::nullopt;
  }
  text.resize(read);
  if (read > kResumeSidecarMaxBytes) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  const auto json = Json::parse(text, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  const auto version = json.find("version");
  if (version == json.end() || !version->is_number_integer()) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  int64_t version_value = 0;
  if (!ReadSigned(json, "version", &version_value) || version_value != kResumeSidecarVersion) {
    Fail(error, "unsupported_version");
    return std::nullopt;
  }
  const auto target = json.find("target_path");
  if (target == json.end() || !target->is_string()) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  ResumeRecord record;
  record.target_path = target->get<std::string>();
  uint64_t wire_id = 0;
  if (!ReadUnsigned(json, "total_size", &record.total_size) ||
      !ReadUnsigned(json, "received", &record.received) ||
      !ReadUnsigned(json, "wire_id", &wire_id) ||
      !ReadSigned(json, "target_mtime_ms", &record.target_mtime_ms)) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  if (wire_id > UINT32_MAX) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  record.wire_id = static_cast<uint32_t>(wire_id);
  if (!ValidRelativeTarget(record.target_path) ||
      PathFromUtf8(record.target_path).filename().u8string().empty()) {
    Fail(error, "invalid_target");
    return std::nullopt;
  }
  // The record must describe the file its own sidecar is named after.
  if (Utf8Of(PathFromUtf8(record.target_path).filename()) != name) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  if (record.received > record.total_size) {
    Fail(error, "malformed");
    return std::nullopt;
  }
  if (!check_freshness) {
    Succeed(error);
    return record;
  }
  const auto target_state = ReadTargetState(dir, record.target_path);
  if (!target_state.present) {
    Fail(error, "missing");
    return std::nullopt;
  }
  if (target_state.size != record.total_size || target_state.mtime_ms != record.target_mtime_ms) {
    // The caller restarts from 0; the record itself is not corrupt.
    Fail(error, "stale");
    return std::nullopt;
  }
  Succeed(error);
  return record;
}

inline bool WriteAll(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.flush();
  const bool ok = !output.fail();
  output.close();
  return ok && !output.fail();
}

}  // namespace resume_detail

// The exact stamp a record has to carry for this target. Callers fill
// target_mtime_ms from here so writing and validating never disagree.
// It is the platform's native file_clock count in milliseconds: opaque, but
// stable on the host that wrote the record, and only ever compared for equality.
inline int64_t TargetMtimeMs(const std::filesystem::path& path) {
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(path, ec);
  if (ec) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(stamp.time_since_epoch()).count();
}

inline std::string ResumeSidecarName(const std::string& name) {
  return resume_detail::ValidName(name) ? resume_detail::SidecarName(name) : std::string();
}

inline bool IsResumeSidecarFilename(std::string_view filename) {
  return filename.size() > kResumeSidecarSuffix.size() &&
         filename.compare(filename.size() - kResumeSidecarSuffix.size(),
                          kResumeSidecarSuffix.size(), kResumeSidecarSuffix) == 0;
}

// Persist a record atomically: temp file plus rename, so a crash mid-write can
// never leave a half-record that a later session would trust.
inline bool WriteResumeRecord(const std::filesystem::path& dir, const ResumeRecord& record,
                              std::string* error) {
  const auto target = resume_detail::PathFromUtf8(record.target_path);
  if (!resume_detail::ValidRelativeTarget(record.target_path)) {
    resume_detail::Fail(error, "invalid_target");
    return false;
  }
  const auto name = resume_detail::Utf8Of(target.filename());
  if (!resume_detail::ValidName(name)) {
    resume_detail::Fail(error, "invalid_name");
    return false;
  }
  if (record.received > record.total_size) {
    resume_detail::Fail(error, "invalid_record");
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::is_directory(dir, ec)) {
    resume_detail::Fail(error, "directory_unavailable");
    return false;
  }
  resume_detail::Json json;
  json["version"] = kResumeSidecarVersion;
  json["target_path"] = record.target_path;
  json["total_size"] = record.total_size;
  json["target_mtime_ms"] = record.target_mtime_ms;
  json["received"] = record.received;
  json["wire_id"] = record.wire_id;
  const auto body = json.dump() + "\n";
  const auto sidecar = dir / resume_detail::PathFromUtf8(resume_detail::SidecarName(name));
  const auto temp = dir / resume_detail::PathFromUtf8(resume_detail::TempName(name));
  if (!resume_detail::WriteAll(temp, body)) {
    std::filesystem::remove(temp, ec);
    resume_detail::Fail(error, "io_error");
    return false;
  }
  std::filesystem::rename(temp, sidecar, ec);
  if (ec) {
    // Some providers refuse an atomic replace; retry once after unlinking and
    // never leave the temporary behind on the failure path.
    std::error_code ignored;
    std::filesystem::remove(sidecar, ignored);
    ec.clear();
    std::filesystem::rename(temp, sidecar, ec);
  }
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    resume_detail::Fail(error, "io_error");
    return false;
  }
  resume_detail::Succeed(error);
  return true;
}

// Returns nullopt with one of these reasons: invalid_name, not_found,
// io_error, malformed, unsupported_version, invalid_target, missing, stale.
// "stale" and "missing" mean the record parsed but the target no longer
// matches it: the caller restarts from 0 instead of trusting the prefix.
inline std::optional<ResumeRecord> ReadResumeRecord(const std::filesystem::path& dir,
                                                    const std::string& name,
                                                    std::string* error) {
  return resume_detail::ReadRecord(dir, name, true, error);
}

// Parses the record without the target-freshness gate. The receive path writes
// records while the payload still lives in a staging prefix, so the final target
// does not exist yet; callers that use this overload MUST verify the prefix
// themselves (see ResumePrefixBytes) before trusting `received`.
inline std::optional<ResumeRecord> ReadResumeRecordUnchecked(const std::filesystem::path& dir,
                                                            const std::string& name,
                                                            std::string* error) {
  return resume_detail::ReadRecord(dir, name, false, error);
}

// Size of the durable prefix the record points at, or nullopt when the prefix is
// missing/unreadable. This is the freshness proof for the unchecked path: the
// sender may only restart at a byte count the receiver can actually replay.
inline std::optional<uint64_t> ResumePrefixBytes(const std::filesystem::path& dir,
                                                 const ResumeRecord& record) {
  std::error_code ec;
  auto prefix = dir / resume_detail::PathFromUtf8(record.target_path);
  // 接收方在目标名被占用时把前缀写成 .crossdesk-prefix-<name>；读取端两种形态都认，
  // 谁存在用谁（不复制、不改名，只读大小）。
  {
    std::error_code probe;
    const auto staged = dir / resume_detail::PathFromUtf8(
        std::string(".crossdesk-prefix-") + record.target_path);
    // 新记录一律用旁路副本；旧版本可能把前缀写在最终名上，两种都认。
    if (std::filesystem::exists(staged, probe)) prefix = staged;
  }
  if (!std::filesystem::is_regular_file(prefix, ec)) return std::nullopt;
  const auto size = std::filesystem::file_size(prefix, ec);
  if (ec) return std::nullopt;
  return size;
}

// Idempotent: clearing a record that was never written is a success.
inline bool ClearResumeRecord(const std::filesystem::path& dir, const std::string& name) {
  if (!resume_detail::ValidName(name)) return false;
  std::error_code ec;
  const auto sidecar = dir / resume_detail::PathFromUtf8(resume_detail::SidecarName(name));
  const auto temp = dir / resume_detail::PathFromUtf8(resume_detail::TempName(name));
  std::filesystem::remove(temp, ec);
  if (ec && !std::filesystem::is_regular_file(sidecar, ec)) return false;
  ec.clear();
  std::filesystem::remove(sidecar, ec);
  return !ec;
}

// Every readable record in the directory, in directory order. Malformed or
// escaping sidecars are skipped instead of throwing, so one corrupt file can
// never abort a cleanup sweep. Freshness is not filtered here: stale and
// orphaned records are still listed, because that is exactly what a caller
// reclaiming staging space has to find.
inline std::vector<ResumeRecord> ListResumeRecords(const std::filesystem::path& dir,
                                                   std::string* error) {
  std::vector<ResumeRecord> records;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    resume_detail::Fail(error, "directory_unavailable");
    return records;
  }
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) {
      resume_detail::Fail(error, "io_error");
      return records;
    }
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec)) continue;
    const auto filename = resume_detail::Utf8Of(it->path().filename());
    if (!IsResumeSidecarFilename(filename)) continue;
    auto name = filename.substr(0, filename.size() - kResumeSidecarSuffix.size());
    std::string ignored;
    auto record = resume_detail::ReadRecord(dir, name, false, &ignored);
    if (!record) continue;
    records.push_back(std::move(*record));
  }
  if (ec) {
    resume_detail::Fail(error, "io_error");
    return records;
  }
  resume_detail::Succeed(error);
  return records;
}

}  // namespace crossdesk::file_transfer

#endif
