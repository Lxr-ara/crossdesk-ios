/*
 * Remote file management ops for the dual pane file browser (UU parity).
 *
 * The ops reuse the existing version 1 "file_browser" envelope, so the wire
 * format does not change: an op is one more JSON "op" value inside the same
 * message the listing and transfer ops already use.
 *
 *   request  {"type":"file_browser","version":1,"id":N,"op":"<name>", ...}
 *   reply    {"type":"file_browser","version":1,"id":N,"op":"reply","ok":bool,
 *             "error":"<code>", ...}                    (id is echoed)
 *
 * Everything here is header only and free standing so the ops test can exercise
 * the op-name table, the error vocabulary and the path-safety rules without a
 * live peer connection.
 */

#ifndef _FILE_BROWSER_OPS_H_
#define _FILE_BROWSER_OPS_H_

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace crossdesk::file_browser_ops {

// ---------------------------------------------------------------- op names --
// Ops that existed before this header was added. The dispatcher keeps them
// byte for byte identical: same names, same request fields, same replies.
inline constexpr std::string_view kOpList = "list";
inline constexpr std::string_view kOpGallery = "gallery";
inline constexpr std::string_view kOpThumb = "thumb";
inline constexpr std::string_view kOpListCancel = "list_cancel";
inline constexpr std::string_view kOpPrepare = "prepare";
inline constexpr std::string_view kOpCancel = "cancel";
inline constexpr std::string_view kOpCopy = "copy";

// Remote file management ops added for UU 远程 parity.
inline constexpr std::string_view kOpMkdir = "mkdir";
inline constexpr std::string_view kOpRename = "rename";
inline constexpr std::string_view kOpDelete = "delete";
inline constexpr std::string_view kOpOpen = "open";
inline constexpr std::string_view kOpReveal = "reveal";
inline constexpr std::string_view kOpStat = "stat";
inline constexpr std::string_view kOpTempClear = "temp_clear";
// 跨会话续传的控制面：问接收端「这个文件你已经有多少字节」。不改变任何
// 线的格式，只是同一条 file_browser 信封里多一个 op。
inline constexpr std::string_view kOpResume = "resume";
// 手机端传输面板（和桌面传输窗口是同一份数据）。控制器库不依赖 GUI：这三个
// op 只做校验和透传，真实状态由宿主注入的 TransferControl 提供（见
// file_browser.h），没有注入时一律回答 unsupported，老宿主照旧工作。
inline constexpr std::string_view kOpTransferList = "transfer_list";
inline constexpr std::string_view kOpTransferControl = "transfer_control";
inline constexpr std::string_view kOpTransferSettings = "transfer_settings";

struct OpInfo {
  std::string_view name;
  // True for the ops introduced by the file management patch, so the header
  // documentation, the dispatcher and the tests share one table.
  bool file_management;
};

inline constexpr std::array<OpInfo, 18> kOps{{
    {kOpList, false},
    {kOpGallery, false},
    {kOpThumb, false},
    {kOpListCancel, false},
    {kOpPrepare, false},
    {kOpCancel, false},
    {kOpCopy, false},
    {kOpMkdir, true},
    {kOpRename, true},
    {kOpDelete, true},
    {kOpOpen, true},
    {kOpReveal, true},
    {kOpStat, true},
    {kOpTempClear, true},
    {kOpResume, true},
    {kOpTransferList, true},
    {kOpTransferControl, true},
    {kOpTransferSettings, true},
}};

// The subset above that this patch added, in dispatch order.
inline constexpr std::array<std::string_view, 11> kFileManagementOps{{
    kOpMkdir, kOpRename, kOpDelete, kOpOpen, kOpReveal, kOpStat, kOpTempClear,
    kOpResume, kOpTransferList, kOpTransferControl, kOpTransferSettings,
}};

inline bool IsOpName(std::string_view op) {
  return std::any_of(kOps.begin(), kOps.end(),
                     [op](const OpInfo& info) { return info.name == op; });
}

inline bool IsFileManagementOp(std::string_view op) {
  return std::any_of(kFileManagementOps.begin(), kFileManagementOps.end(),
                     [op](std::string_view name) { return name == op; });
}

// ------------------------------------------- transfer panel vocabulary ----
// transfer_control 的动作词表：闭合。客户端只发这八个字符串，别的一律
// invalid_request，所以两端不需要就「动作名」做任何协商。
inline constexpr std::string_view kTransferPause = "pause";
inline constexpr std::string_view kTransferResume = "resume";
inline constexpr std::string_view kTransferRetry = "retry";inline constexpr std::string_view kTransferCancel = "cancel";

inline constexpr std::string_view kTransferStartAll = "start_all";
inline constexpr std::string_view kTransferPauseAll = "pause_all";
inline constexpr std::string_view kTransferResumeAll = "resume_all";
inline constexpr std::string_view kTransferClearAll = "clear_all";
inline constexpr std::string_view kTransferRemoveFinished = "remove_finished";

inline constexpr std::array<std::string_view, 9> kTransferActions{{
    kTransferPause,    kTransferResume,    kTransferRetry,     kTransferStartAll,
    kTransferPauseAll, kTransferResumeAll, kTransferClearAll,  kTransferRemoveFinished,
    kTransferCancel,
}};

// 逐项动作：必须带一个正整数 id。批量动作忽略 id。
inline constexpr std::array<std::string_view, 4> kTransferItemActions{{
    kTransferPause, kTransferResume, kTransferRetry, kTransferCancel,
}};

inline bool IsTransferAction(std::string_view action) {
  return std::any_of(kTransferActions.begin(), kTransferActions.end(),
                     [action](std::string_view known) { return known == action; });
}

inline bool IsTransferItemAction(std::string_view action) {
  return std::any_of(kTransferItemActions.begin(), kTransferItemActions.end(),
                     [action](std::string_view known) { return known == action; });
}

// transfer_list 里 state 的取值，顺序与桌面 SendQueue::State 的枚举顺序一致
// （Queued, Sending, Paused, Completed, Failed, Cancelled），所以 GUI 适配器
// 可以直接 kTransferStateNames[static_cast<int>(row.state)] 取名字。
inline constexpr std::array<std::string_view, 6> kTransferStateNames{{
    "queued", "sending", "paused", "completed", "failed", "cancelled",
}};

inline bool IsTransferState(std::string_view state) {
  return std::any_of(kTransferStateNames.begin(), kTransferStateNames.end(),
                     [state](std::string_view known) { return known == state; });
}

// transfer_settings 的三个范围。越界是 invalid_request；应答里的生效值也会再
// 夹一次，所以手机端读到的永远是区间内的数。上限与
// config_center 的 kTransferRateLimitMaxKbps、FileTransferManager 的
// kMaxConcurrentPerPeer 一致。
inline constexpr int kTransferRateKbpsMin = 0;
inline constexpr int kTransferRateKbpsMax = 1048576;
inline constexpr int kTransferOverwritePolicyMin = 0;
inline constexpr int kTransferOverwritePolicyMax = 2;
inline constexpr int kTransferConcurrencyMin = 1;
inline constexpr int kTransferConcurrencyMax = 4;

// --------------------------------------------------------- error vocabulary --
// Every reply carries one of these codes in "error". Clients switch on the
// code, never on prose, so the set is closed and shared with the tests.
//
// Codes that already existed before this patch.
inline constexpr std::string_view kErrorInvalidPath = "invalid_path";
inline constexpr std::string_view kErrorInvalidRequest = "invalid_request";
inline constexpr std::string_view kErrorSymbolicLink = "symbolic_link";
inline constexpr std::string_view kErrorDirectoryUnavailable = "directory_unavailable";
inline constexpr std::string_view kErrorDirectoryNotWritable = "directory_not_writable";
inline constexpr std::string_view kErrorFileUnreadable = "file_unreadable";
inline constexpr std::string_view kErrorUnsupported = "unsupported";
inline constexpr std::string_view kErrorUnsupportedFile = "unsupported_file";
inline constexpr std::string_view kErrorTooManyFiles = "too_many_files";
inline constexpr std::string_view kErrorInvalidListing = "invalid_listing";
inline constexpr std::string_view kErrorPaginationUnsupported = "pagination_unsupported";
inline constexpr std::string_view kErrorListingExpired = "listing_expired";
inline constexpr std::string_view kErrorBusy = "busy";
inline constexpr std::string_view kErrorTimeout = "timeout";
inline constexpr std::string_view kErrorDisconnected = "disconnected";
inline constexpr std::string_view kErrorAlreadyFinished = "already_finished";
inline constexpr std::string_view kErrorDecodeFailed = "decode_failed";
// Codes added by the file management ops.
inline constexpr std::string_view kErrorExists = "exists";
inline constexpr std::string_view kErrorNotFound = "not_found";
inline constexpr std::string_view kErrorNotWritable = "not_writable";

inline constexpr std::array<std::string_view, 20> kErrorCodes{{
    kErrorInvalidPath,      kErrorInvalidRequest,    kErrorSymbolicLink,
    kErrorDirectoryUnavailable, kErrorDirectoryNotWritable, kErrorFileUnreadable,
    kErrorUnsupported,      kErrorUnsupportedFile,   kErrorTooManyFiles,
    kErrorInvalidListing,   kErrorPaginationUnsupported, kErrorListingExpired,
    kErrorBusy,             kErrorTimeout,           kErrorDisconnected,
    kErrorAlreadyFinished,  kErrorDecodeFailed,      kErrorExists,
    kErrorNotFound,         kErrorNotWritable,
}};

inline bool IsErrorCode(std::string_view code) {
  return std::any_of(kErrorCodes.begin(), kErrorCodes.end(),
                     [code](std::string_view known) { return known == code; });
}

// ------------------------------------------------------------------ limits --
// A single delete or stat request may name at most this many paths; the batch
// keeps one reply small enough for the existing 45 KB page budget.
inline constexpr std::size_t kMaxDeletePaths = 256;
inline constexpr std::size_t kMaxStatPaths = 256;
// Bounded work for temp_clear so a huge receive tree cannot stall the worker.
inline constexpr std::size_t kMaxTempClearEntries = 256;

// ------------------------------------------------------------ path safety --
inline std::filesystem::path Utf8Path(std::string_view value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

inline std::string Utf8Text(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

// Why SafeChildPath refused a candidate.
enum class PathError {
  None,
  // Empty root, an absolute candidate, a drive/root component, "." or "..".
  InvalidPath,
  // A component of the candidate is a symbolic link, a Windows junction or
  // another special file, so it does not stay inside the directory it names.
  SymbolicLink,
};

// A component that is not a plain file or directory: a symbolic link, a Windows
// junction, a device, a fifo or a socket. std::filesystem reports a junction as
// its own file type, which is_symlink() does not cover, so both checks are
// needed to keep a request inside the directory it named.
inline bool IsLinkLike(const std::filesystem::file_status& status) {
  return std::filesystem::is_symlink(status) || std::filesystem::is_other(status);
}

// The single containment validator used by both the transfer staging path
// (file_browser.cpp: Destination) and the file management ops, so there is
// exactly one "is this path legal" rule in the controller.
//
// `candidate` must be a relative path of plain names: no absolute root, no
// drive prefix, no "." and no "..". The result is `root` joined with every
// component, with each component rejected when it is a link (or a Windows
// junction), which is what stops a request from walking out of the root through
// a reparse point.
//
// `allow_link` only relaxes the *final* component so the read-only ops can
// report or move a link itself; intermediates are always checked.
inline bool SafeChildPath(const std::filesystem::path& root, std::string_view candidate,
                          std::filesystem::path* out, PathError* error = nullptr,
                          bool allow_link = false) {
  if (error) *error = PathError::None;
  const auto fail = [error](PathError reason) {
    if (error) *error = reason;
    return false;
  };
  if (!out || root.empty()) return fail(PathError::InvalidPath);
  const auto relative = Utf8Path(candidate);
  if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
    return fail(PathError::InvalidPath);
  std::vector<std::filesystem::path> components;
  for (const auto& component : relative) components.push_back(component);
  auto result = root;
  for (std::size_t index = 0; index < components.size(); ++index) {
    const auto& component = components[index];
    if (component.empty() || component == "." || component == ".." ||
        component.has_root_name() || component.has_root_directory())
      return fail(PathError::InvalidPath);
    result /= component;
    if (allow_link && index + 1 == components.size()) continue;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(result, ec);
    if (!ec && IsLinkLike(status)) return fail(PathError::SymbolicLink);
  }
  *out = result;
  return true;
}

// True when `child` is `root` or lives below it. Both paths must already be
// canonical (the caller resolves them), so this is a pure lexical containment
// test with no filesystem access.
inline bool ContainedIn(const std::filesystem::path& root,
                       const std::filesystem::path& child) {
  if (root.empty() || child.empty()) return false;
  if (root == child) return true;
  const auto relative = child.lexically_relative(root);
  if (relative.empty() || relative == ".") return root == child;
  const auto first = *relative.begin();
  return first != ".." && first != "." && !first.has_root_name();
}

// A single path component that may be created or used as a rename target.
// Rejected: empty, "." / "..", any separator, Windows illegal characters,
// device names, and trailing dots or spaces (Windows silently strips them, so
// two different requests would collide on one file).
inline bool ValidEntryName(std::string_view name) {
  if (name.empty() || name.size() > 255) return false;
  if (name == "." || name == "..") return false;
  for (const auto character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) return false;
    switch (character) {
      case '/': case '\\': case ':': case '*': case '?':
      case '"': case '<': case '>': case '|':
        return false;
      default:
        break;
    }
  }
  if (name.back() == '.' || name.back() == ' ') return false;
  if (name.front() == ' ') return false;
  const auto dot = name.find('.');
  auto stem = std::string(name.substr(0, dot == std::string_view::npos ? name.size() : dot));
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$")
    return false;
  if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
      stem[3] >= '1' && stem[3] <= '9')
    return false;
  return true;
}

// Transfer staging directories are created as ".crossdesk-<id>-<tick>-<serial>"
// by the receive engine (file_transfer_core.h). Only these are temp_clear bait.
inline constexpr std::string_view kStagingPrefix = ".crossdesk-";

inline bool IsStagingDirectoryName(std::string_view name) {
  return name.size() > kStagingPrefix.size() &&
         name.substr(0, kStagingPrefix.size()) == kStagingPrefix;
}

// Pure selection half of `temp_clear`: the staging directories below `root`.
// Symlinks are neither followed nor selected, real directories only, and the
// walk stops at `limit` (or when a staging directory is selected, since the
// whole subtree goes away with it). The caller refuses directories outside the
// configured receive root before calling this.
inline std::vector<std::filesystem::path> SelectStagingDirectories(
    const std::filesystem::path& root, std::size_t limit = kMaxTempClearEntries) {
  std::vector<std::filesystem::path> found;
  if (root.empty() || !limit) return found;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(root, options, ec), end;
  for (; !ec && it != end && found.size() < limit; it.increment(ec)) {
    std::error_code status_error;
    const auto status = it->symlink_status(status_error);
    if (status_error) continue;
    // Only a real directory: a link or a junction named like staging is neither
    // followed nor removed.
    if (IsLinkLike(status) || !std::filesystem::is_directory(status)) continue;
    if (!IsStagingDirectoryName(Utf8Text(it->path().filename()))) continue;
    found.push_back(it->path());
    it.disable_recursion_pending();
  }
  std::sort(found.begin(), found.end());
  return found;
}

}  // namespace crossdesk::file_browser_ops

#endif
