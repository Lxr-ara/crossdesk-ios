#include "config_center/config_center.h"
#include "features/file_transfer/file_browser_window.h"

// Keeps the Win32 min/max macros out of the SDL and dialog headers below; the
// platform branch further down only needs the Win32 window query.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <SDL3/SDL.h>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <any>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
extern unsigned char fa_solid_900_ttf[];
extern unsigned int fa_solid_900_ttf_len;
#include "ui/model_update.h"
#include "ui/ui_localization.h"
#include "localization.h"
#include "application/window_geometry.h"
#include "application/window_presentation.h"
#if defined(__APPLE__)
#include "platform/window_drag.h"
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace crossdesk {
namespace {
slint::SharedString S(std::string_view value) { return slint::SharedString(value); }
std::string Size(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = bytes; int unit = 0;
  while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
  char text[64]; std::snprintf(text, sizeof(text), unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
  return text;
}
std::string Join(const std::string& parent, const std::string& name) {
  const bool windows = (parent.size() > 1 && parent[1] == ':') || parent.rfind("\\\\", 0) == 0;
  return parent + (parent.empty() || parent.back() == '/' || parent.back() == '\\'
      ? "" : windows ? "\\" : "/") + name;
}
std::string Name(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos || slash == path.size() - 1 ? path : path.substr(slash + 1);
}
bool Matches(const std::string& text, const std::string& filter) {
  return std::search(text.begin(), text.end(), filter.begin(), filter.end(),
      [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) != text.end();
}
std::string PathToUtf8(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
#else
  return path.u8string();
#endif
}
// UU ships 16 file-type families (file_transfer_item_pdf … _other). The index
// travels to Slint as `kind`; the glyph is resolved here so a row stays a plain
// Text instead of a per-extension if-chain in the .slint file.
enum TaskKind : int {
  kKindPdf, kKindWord, kKindExcel, kKindPpt, kKindText, kKindPicture,
  kKindMusic, kKindVideo, kKindFolder, kKindDisk, kKindLink, kKindDesktop,
  kKindDocument, kKindDownload, kKindMyComputer, kKindOther, kKindCount
};
// Font Awesome 6 Free Solid codepoints, in the same order as TaskKind.
constexpr char32_t kKindGlyphs[kKindCount] = {
    0xF1C1,  // pdf          file-pdf
    0xF1C2,  // word         file-word
    0xF1C3,  // excel        file-excel
    0xF1C4,  // ppt          file-powerpoint
    0xF15C,  // text         file-lines
    0xF1C5,  // picture      file-image
    0xF1C7,  // music        file-audio
    0xF1C8,  // video        file-video
    0xF07B,  // folder       folder
    0xF0A0,  // disk         hard-drive
    0xF0C1,  // link         link
    0xF108,  // desktop      desktop
    0xF15B,  // document     file
    0xF019,  // download     download
    0xE163,  // my_computer  display (same glyph as FontAwesomeIcons.display)
    0xF128,  // other        question
};
// Written out instead of using a \u escape so the bytes do not depend on the
// compiler's execution character set.
std::string Utf8Glyph(char32_t code) {
  std::string out;
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
  return out;
}
std::string Extension(const std::string& name) {
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return {};
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}
int TaskKindForName(const std::string& name) {
  static const std::unordered_map<std::string, int> kExtensions = {
      {"pdf", kKindPdf},
      {"doc", kKindWord}, {"docx", kKindWord}, {"docm", kKindWord},
      {"dot", kKindWord}, {"dotx", kKindWord}, {"rtf", kKindWord},
      {"odt", kKindWord}, {"pages", kKindWord}, {"wps", kKindWord},
      {"xls", kKindExcel}, {"xlsx", kKindExcel}, {"xlsm", kKindExcel},
      {"csv", kKindExcel}, {"ods", kKindExcel}, {"numbers", kKindExcel},
      {"ppt", kKindPpt}, {"pptx", kKindPpt}, {"pps", kKindPpt},
      {"ppsx", kKindPpt}, {"odp", kKindPpt}, {"key", kKindPpt},
      {"txt", kKindText}, {"md", kKindText}, {"log", kKindText},
      {"ini", kKindText}, {"cfg", kKindText}, {"conf", kKindText},
      {"json", kKindText}, {"xml", kKindText}, {"yaml", kKindText},
      {"yml", kKindText}, {"toml", kKindText},
      {"sh", kKindText}, {"bat", kKindText}, {"ps1", kKindText},
      {"c", kKindText}, {"h", kKindText}, {"cc", kKindText},
      {"cpp", kKindText}, {"hpp", kKindText}, {"py", kKindText},
      {"js", kKindText}, {"ts", kKindText}, {"html", kKindText},
      {"css", kKindText},
      {"png", kKindPicture}, {"jpg", kKindPicture}, {"jpeg", kKindPicture},
      {"gif", kKindPicture}, {"bmp", kKindPicture}, {"webp", kKindPicture},
      {"svg", kKindPicture}, {"ico", kKindPicture}, {"tif", kKindPicture},
      {"tiff", kKindPicture}, {"heic", kKindPicture}, {"psd", kKindPicture},
      {"mp3", kKindMusic}, {"wav", kKindMusic}, {"flac", kKindMusic},
      {"aac", kKindMusic}, {"ogg", kKindMusic}, {"m4a", kKindMusic},
      {"wma", kKindMusic}, {"ape", kKindMusic},
      {"mp4", kKindVideo}, {"mkv", kKindVideo}, {"avi", kKindVideo},
      {"mov", kKindVideo}, {"wmv", kKindVideo}, {"flv", kKindVideo},
      {"webm", kKindVideo}, {"m4v", kKindVideo}, {"mpg", kKindVideo},
      {"mpeg", kKindVideo}, {"rmvb", kKindVideo},
      {"3gp", kKindVideo},
      {"exe", kKindDownload}, {"msi", kKindDownload}, {"dmg", kKindDownload},
      {"pkg", kKindDownload}, {"deb", kKindDownload}, {"rpm", kKindDownload},
      {"apk", kKindDownload}, {"ipa", kKindDownload}, {"zip", kKindDownload},
      {"7z", kKindDownload}, {"rar", kKindDownload}, {"tar", kKindDownload},
      {"gz", kKindDownload},
  };
  // The folder / disk / link / desktop / my_computer families are reachable
  // from the pane rows, not from a transfer task, so they stay resolved above.
  const auto ext = Extension(name);
  if (ext.empty()) return kKindDocument;
  const auto found = kExtensions.find(ext);
  return found == kExtensions.end() ? kKindOther : found->second;
}
std::string TaskKindGlyph(int kind) {
  return Utf8Glyph(kKindGlyphs[kind >= 0 && kind < kKindCount ? kind : kKindOther]);
}
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string ClockText(int64_t epoch_ms) {
  if (epoch_ms <= 0) return {};
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm local{};
#if defined(_WIN32)
  if (localtime_s(&local, &seconds) != 0) return {};
#else
  if (localtime_r(&seconds, &local) == nullptr) return {};
#endif
  char text[8];
  std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
  return text;
}
// A task row plus the keys the task-list sort needs; only `row` reaches Slint.
struct TaskRow {
  ui::BrowserTask row;
  std::string name_key;
  uint64_t bytes = 0;
  int64_t started_ms = 0;
};
struct BatchCounts {
  int completed = 0, failed = 0, active = 0;
};
// FileTransferManager does not own a per-task retry yet. Detect the entry point
// so this window builds against either revision and the row lights up with no
// further UI change once it lands. Expected:
//   bool FileTransferManager::RetryTransfer(const std::string& remote, uint32_t file_id);
template <typename Manager, typename = void>
struct HasRetryTransfer : std::false_type {};
template <typename Manager>
struct HasRetryTransfer<Manager,
    std::void_t<decltype(std::declval<Manager&>().RetryTransfer(
        std::declval<const std::string&>(), std::declval<uint32_t>()))>>
    : std::true_type {};
constexpr bool kRetrySupported = HasRetryTransfer<FileTransferManager>::value;
template <typename Manager>
bool InvokeRetryTransfer(Manager& manager, const std::string& remote, uint32_t id) {
  if constexpr (HasRetryTransfer<Manager>::value) {
    return manager.RetryTransfer(remote, id);
  } else {
    (void)manager;
    (void)remote;
    (void)id;
    return false;
  }
}
// One row per code in file_browser_ops.h (kErrorCodes). The Chinese text doubles
// as the catalog key, so the ten-language table stays the only place the wording
// lives and this table only maps a code to its key. The static_assert below is
// what keeps the two lists in step when an op gains a code.
struct ErrorText { std::string_view code, key; };
constexpr ErrorText kErrorTexts[] = {
    {"invalid_path", "路径无效，请重新选择项目。"},
    {"invalid_request", "请求无效，请更新两端版本后重试。"},
    {"symbolic_link", "所选内容包含符号链接，请选择实际文件或目录。"},
    {"directory_unavailable", "无法打开目录，请检查路径和访问权限后刷新。"},
    {"directory_not_writable", "目标目录不可写，请选择其他目录。"},
    {"file_unreadable", "文件无法读取，可能已移动或无访问权限。"},
    {"unsupported", "远端版本不支持此操作，请更新后重试。"},
    {"unsupported_file", "不支持此文件类型。"},
    {"too_many_files", "单次最多操作 4096 个文件与目录，请分批选择。"},
    {"invalid_listing", "目录列表未能完整加载，请刷新重试。"},
    {"pagination_unsupported", "远端版本无法完整读取此目录，请更新远端后重试。"},
    {"listing_expired", "目录列表已过期，请刷新后重试。"},
    {"busy", "请求较多，请稍后重试。"},
    {"timeout", "远端未响应，请确认两端已更新至支持文件浏览的版本后重试。"},
    {"disconnected", "连接已断开，请重新连接后打开文件管理器。"},
    {"already_finished", "任务已结束，无需取消。"},
    {"decode_failed", "文件内容无法解码。"},
    {"exists", "已存在同名项目，请换一个名称。"},
    {"not_found", "项目不存在，可能已被移动或删除。"},
    {"not_writable", "没有写入权限，操作被拒绝。"},
};
static_assert(sizeof(kErrorTexts) / sizeof(kErrorTexts[0]) ==
                  file_browser_ops::kErrorCodes.size(),
              "every error code in file_browser_ops.h needs one catalog key here");
}
std::string FileBrowserWindow::Tr(const char* chinese, const char* english) const { return chinese_ ? chinese : english; }
FileBrowserWindow::FileBrowserWindow(FileTransferManager& transfers, ConfigCenter& config,
    std::string remote, bool controller, int language)
    : transfers_(transfers), remote_(std::move(remote)), config_(config),
      connection_generation_(transfers_.browser().Generation(remote_)),
      chinese_(language == 0 || language == 3) {
  transfers_.browser().TakeEvents(remote_);
  window_->window().window_handle().register_font_from_data(
      fa_solid_900_ttf, fa_solid_900_ttf_len);
  // Resolve the native frame at its final size while it is still hidden.
  // Otherwise show() applies the preferred size after we have centered it.
  window_->window().set_size(slint::LogicalSize({900, 620}));
  window_->set_chinese(chinese_);
  UpdateActionLabels(language);
  SetPeerName("");
  window_->set_local_role(S(controller ? Tr("控制端", "Controller") : Tr("被控端", "Host")));
  window_->set_remote_role(S(controller ? Tr("被控端", "Host") : Tr("控制端", "Controller")));
  window_->set_local_entries(panes_[0].rows); window_->set_local_tree(panes_[0].tree);
  window_->set_remote_entries(panes_[1].rows); window_->set_remote_tree(panes_[1].tree);
  window_->set_tasks(tasks_);
  // Time descending is the previous newest-first order, so the default view is
  // unchanged and a header click only flips what the user asked for.
  window_->set_sort_column(task_sort_column_);
  window_->set_sort_ascending(task_sort_ascending_);
  window_->on_navigate([this](bool remote, slint::SharedString path) {
    active_pane_ = remote ? 1 : 0;
    Navigate(remote, std::string(path));
  });
  window_->on_refresh([this](bool remote) { Navigate(remote, panes_[remote].path); });
  window_->on_up([this](bool remote) { Navigate(remote, panes_[remote].parent); });
  window_->on_filter([this](bool remote, slint::SharedString filter) {
    active_pane_ = remote ? 1 : 0;
    auto& pane = panes_[remote]; pane.filter = std::string(filter); pane.selected.clear(); UpdatePane(remote);
  });
  window_->on_sort([this](bool remote, int column) {
    active_pane_ = remote ? 1 : 0;
    auto& pane = panes_[remote];
    if (column < 0 || column > 2 || pane.busy || !pane.loaded ||
        !pane.error.empty() || window_->get_drag_active()) return;
    const auto sort_column = static_cast<SortColumn>(column);
    pane.sort_descending = pane.sort_column == sort_column ? !pane.sort_descending
        : sort_column != SortColumn::Name;
    pane.sort_column = sort_column;
    SortEntries(pane);
    UpdatePane(remote);
  });
  window_->on_toggle_hidden([this](bool remote) {
    auto& pane = panes_[remote]; pane.show_hidden = !pane.show_hidden;
    pane.selected.clear(); UpdatePane(remote); UpdateTree(remote);
  });
  window_->on_select_entry([this](bool remote, int index) {
    active_pane_ = remote ? 1 : 0;
    auto& pane = panes_[remote];
    if (index < 0 || index >= static_cast<int>(pane.rows->row_count())) return;
    auto row = *pane.rows->row_data(index);
    if (row.link) return;
    SelectSource(remote);
    const std::string path(row.path);
    if (!pane.selected.erase(path)) pane.selected.insert(path);
    UpdatePane(remote);
  });
  window_->on_select_all([this](bool remote) {
    SelectSource(remote);
    auto& pane = panes_[remote];
    bool all = true;
    for (size_t i = 0; i < pane.rows->row_count(); ++i) {
      const auto row = *pane.rows->row_data(i);
      if (!row.link && !pane.selected.count(std::string(row.path))) all = false;
    }
    for (size_t i = 0; i < pane.rows->row_count(); ++i) {
      const auto row = *pane.rows->row_data(i);
      if (all) pane.selected.erase(std::string(row.path));
      else if (!row.link) pane.selected.insert(std::string(row.path));
    }
    UpdatePane(remote);
  });
  window_->on_clear_selection([this] {
    if (window_->get_drag_active()) return;
    for (int side = 0; side < 2; ++side) {
      panes_[side].selected.clear();
      UpdatePane(side);
    }
  });
  window_->on_open_entry([this](bool remote, int index) {
    active_pane_ = remote ? 1 : 0;
    auto& pane = panes_[remote];
    if (index < 0 || index >= static_cast<int>(pane.rows->row_count())) return;
    const auto row = *pane.rows->row_data(index);
    if (row.directory) OpenDirectory(remote, std::string(row.path));
  });
  window_->on_open_directory([this](bool remote, slint::SharedString path) {
    OpenDirectory(remote, std::string(path));
  });
  window_->on_tree_open([this](bool remote, int index) {
    const auto& pane = panes_[remote];
    if (index >= 0 && index < static_cast<int>(pane.tree->row_count()))
      Navigate(remote, std::string(pane.tree->row_data(index)->path));
  });
  window_->on_tree_toggle([this](bool remote, int index) {
    auto& pane = panes_[remote];
    if (index < 0 || index >= static_cast<int>(pane.tree->row_count())) return;
    const std::string path(pane.tree->row_data(index)->path);
    if (!pane.expanded.erase(path)) {
      pane.expanded.insert(path);
      if (!pane.directories.count(path)) Navigate(remote, path, true);
    }
    UpdateTree(remote);
  });
  window_->on_drag_data([this](bool remote, slint::SharedString path, int) {
    return MakeDragData(remote, std::string(path));
  });
  window_->on_drag_state([this](const slint::DataTransfer& data, bool active) {
    if (!active) { CancelDrag(); return; }
    const auto value = data.user_data();
    const auto stored = std::any_cast<std::shared_ptr<DragPayload>>(&value);
    if (!stored || !*stored) return;
    const auto payload = DropPayload(!(*stored)->remote, data);
    if (!payload) return;
    window_->set_drag_active(true);
    window_->set_drag_count(static_cast<int>(payload->paths->size()));
    SelectSource(payload->remote);
    panes_[payload->remote].selected = {payload->paths->begin(), payload->paths->end()};
    UpdatePane(payload->remote);
  });
  window_->on_can_drop([this](bool remote, const slint::DataTransfer& data) {
    return static_cast<bool>(DropPayload(remote, data));
  });
  window_->on_dropped([this](bool remote, const slint::DataTransfer& data) {
    const auto payload = DropPayload(remote, data);
    if (!payload) return false;
    // The drag owns a snapshot of both directories and its selected paths.
    // Consume before queueing so repeated release events cannot send twice.
    payload->consumed = true;
    return QueueTransfer(payload->remote, *payload->paths, payload->target_directory);
  });
  window_->on_cancel_drag([this] { CancelDrag(); });
  window_->on_transfer([this](bool download) { Transfer(download); });
  window_->on_cancel_task([this](int id) { transfers_.CancelBrowserTransfer(remote_, static_cast<uint32_t>(id)); });
  window_->on_toggle_transfer_pause([this] {
    // UU parks a task instead of cancelling it: the engine keeps the file and
    // its ACKs, so resuming continues from the byte the sender stopped at.
    if (transfers_.TransferPaused(remote_)) transfers_.ResumeTransfer(remote_);
    else transfers_.PauseTransfer(remote_);
    window_->set_transfer_paused(transfers_.TransferPaused(remote_));
  });
  window_->on_rate_limit_changed([this](int index) {
    // 0 = unlimited, then 1 / 4 / 8 / 16 / 32 MiB/s in KiB/s.
    static constexpr int kKiBPerSecond[] = {0, 1024, 4096, 8192, 16384, 32768};
    const auto clamped = std::clamp(index, 0, 5);
    config_.SetTransferRateLimitKbps(kKiBPerSecond[clamped]);
  });
  window_->on_overwrite_policy_changed([this](int index) {
    // The list is ordered by what the user sees: auto rename, overwrite, skip.
    static constexpr int kPolicy[] = {2, 1, 0};
    const auto clamped = std::clamp(index, 0, 2);
    config_.SetTransferOverwritePolicy(kPolicy[clamped]);
  });
  window_->on_clear_finished([this] {
    using Status = FileTransferManager::FileTransferState::FileTransferStatus;
    for (const auto& [id, task] : history_)
      if (task.status == Status::Completed || task.status == Status::Failed || task.status == Status::Cancelled)
        dismissed_.insert(id);
    next_sync_ = {};
  });
  window_->on_sort_tasks([this](int column) { SortTasks(column); });
  window_->on_start_all_tasks([this] { SetTransferHeld(false); });
  window_->on_pause_all_tasks([this] { SetTransferHeld(true); });
  window_->on_resume_all_tasks([this] { SetTransferHeld(false); });
  window_->on_toggle_task_pause([this](int id) {
    // Per-item hold: the queue keeps this row's slot and the engine keeps its
    // ACKs, so a resumed item continues at the byte it stopped on. The toolbar
    // button remains the peer-wide switch.
    if (id <= 0) {
      SetTransferHeld(!transfers_.TransferPaused(remote_));
      return;
    }
    FileTransferManager::TransferRow row;
    const auto key = static_cast<uint32_t>(id);
    if (transfers_.TransferRowStateOf(remote_, key, &row) && row.paused)
      transfers_.ResumeTransferItem(remote_, key);
    else
      transfers_.PauseTransferItem(remote_, key);
  });
  window_->on_clear_all_tasks([this] {
    // Queue first: finished rows leave the model, active rows stay put.
    transfers_.ClearAllTransfers(remote_);
    for (const auto& entry : history_) dismissed_.insert(entry.first);
    next_sync_ = {};
  });
  window_->on_retry_task([this](int id) {
    if (id <= 0) return;
    if (!InvokeRetryTransfer(transfers_, remote_, static_cast<uint32_t>(id))) {
      Notice(Tr("重试需要重新选择文件后再发送。", "Retry needs the file to be selected again."), false);
      return;
    }
    Notice(Tr("已重新加入传输队列", "Re-queued for transfer"), false);
    next_sync_ = {};
  });
  window_->on_open_save_folder([this] {
    // Same file-URL idiom as the photo album's reveal action.
    auto directory = config_.GetFileTransferSavePath();
    if (directory.empty()) directory = PathToUtf8(FileReceiver::DefaultOutputDirectory());
    if (directory.empty()) return;
    std::replace(directory.begin(), directory.end(), '\\', '/');
    (void)SDL_OpenURL(("file:///" + directory).c_str());
  });
  window_->on_change_save_path([this] {
    const auto current = config_.GetFileTransferSavePath();
    const char* chosen = tinyfd_selectFolderDialog(
        localization::Literal("修改保存路径", language_).c_str(),
        current.empty() ? nullptr : current.c_str());
    if (!chosen) return;
    if (config_.SetFileTransferSavePath(chosen) != 0) {
      Notice(Tr("无法保存此目录，请选择其他文件夹。", "Cannot use this folder. Choose another one."), true);
      return;
    }
    // ReceiverFor re-reads the config for every session, so the folder applies
    // to the next received file without reopening the connection.
    Notice(Tr("保存路径已更新", "Save path updated"), false);
  });
  // -------------------------------------------------- file management ops --
  // One entry point per host op. Every one is gated on the capability the probe
  // proved, so a menu entry that is somehow still reachable cannot fire a
  // request the peer already answered "unsupported" to.
  window_->on_open_file([this](bool remote, slint::SharedString path) {
    if (!remote) return;
    active_pane_ = 1;
    if (!ops_.open) return;
    OpRequest request; request.kind = OpKind::Open; request.path = std::string(path);
    SendOp(std::move(request));
  });
  window_->on_reveal_entry([this](bool remote, slint::SharedString path) {
    if (!remote) return;
    active_pane_ = 1;
    if (!ops_.reveal) return;
    OpRequest request; request.kind = OpKind::Reveal; request.path = std::string(path);
    SendOp(std::move(request));
  });
  window_->on_rename_entry([this](bool remote, slint::SharedString path) {
    if (!remote) return;
    active_pane_ = 1;
    ShowRenameDialog(std::string(path));
  });
  window_->on_delete_entry([this](bool remote, slint::SharedString path) {
    if (!remote) return;
    active_pane_ = 1;
    ShowDeleteDialog(MenuTargets(std::string(path)));
  });
  window_->on_new_folder([this](bool remote) {
    if (!remote) return;
    active_pane_ = 1;
    ShowNewFolderDialog();
  });
  window_->on_show_properties([this](bool remote, slint::SharedString path) {
    if (!remote) return;
    active_pane_ = 1;
    if (!ops_.stat) return;
    OpRequest request; request.kind = OpKind::Stat;
    request.path = std::string(path); request.paths = {request.path};
    SendOp(std::move(request));
  });
  window_->on_refresh_pane([this](bool remote) {
    active_pane_ = remote ? 1 : 0;
    Navigate(remote, panes_[remote].path);
  });
  // 清理临时文件: the toolbar button and the empty-space menu entry share this
  // one entry point. The op runs on the peer, so only the remote side offers it.
  window_->on_temp_clear([this](bool remote) {
    if (!remote) return;
    active_pane_ = 1;
    ShowTempClearDialog();
  });
  window_->on_dialog_accept([this] {
    if (dialog_ == Dialog::None || dialog_busy_) return;
    if (dialog_ == Dialog::Properties) { CloseDialog(); return; }
    if (dialog_ == Dialog::TempClear) {
      dialog_busy_ = true; window_->set_dialog_busy(true);
      RequestTempClear(dialog_path_);
      return;
    }
    if (dialog_ == Dialog::Delete) {
      dialog_busy_ = true; window_->set_dialog_busy(true);
      RequestDelete(dialog_paths_);
      return;
    }
    // Rename and new folder share the validation: empty, unchanged and colliding
    // names stop here, so the host never sees a request that cannot succeed.
    const std::string name(window_->get_dialog_input());
    std::string hint;
    if (!ValidateEntryName(name, &hint)) { ShowDialogHint(hint); return; }
    if (dialog_ == Dialog::Rename) {
      if (name == dialog_name_) { CloseDialog(); return; }
      if (NameTaken(name)) { ShowDialogHint(Label("当前目录已存在同名项目")); return; }
      dialog_busy_ = true; window_->set_dialog_busy(true);
      RequestRename(dialog_path_, name);
    } else {
      if (NameTaken(name)) { ShowDialogHint(Label("当前目录已存在同名项目")); return; }
      dialog_busy_ = true; window_->set_dialog_busy(true);
      RequestMkdir(dialog_path_, name);
    }
  });
  window_->on_dialog_cancel([this] { CloseDialog(); });
  window_->on_keyboard_delete([this] {
    if (KeyboardPane() < 0) return;
    const auto& pane = panes_[1];
    if (pane.selected.empty()) return;
    ShowDeleteDialog({pane.selected.begin(), pane.selected.end()});
  });
  window_->on_keyboard_rename([this] {
    if (KeyboardPane() < 0) return;
    const auto& pane = panes_[1];
    if (pane.selected.size() != 1) {
      // F2 renames exactly one entry, like Explorer; anything else says why.
      Notice(Label("请先选择一个项目再重命名"), false);
      return;
    }
    ShowRenameDialog(*pane.selected.begin());
  });
  window_->on_keyboard_refresh([this] {
    // F5 re-lists whichever side the pointer last worked in; a local listing
    // needs no host at all.
    const int side = active_pane_ == 1 ? 1 : 0;
    Navigate(side == 1, panes_[side].path);
  });
  // Capabilities start closed: the probe opens them after the first listing, so
  // no menu entry is ever offered before the host has answered once.
  ApplyCapabilities();
  window_->set_dialog_kind(0);
  window_->set_dialog_name(S("")); window_->set_dialog_input(S(""));
  window_->set_dialog_hint(S("")); window_->set_dialog_message(S(""));
  window_->set_dialog_note(S("")); window_->set_dialog_title(S(""));
  window_->set_dialog_size(S("")); window_->set_dialog_modified(S(""));
  window_->set_dialog_input_visible(false); window_->set_dialog_busy(false);
  window_->set_dialog_danger(false); window_->set_dialog_directory(false);
  window_->window().on_close_requested([this] {
    show_pending_ = false;
    CancelDrag();
    CloseDialog();
    window_->hide(); return slint::CloseRequestResponse::KeepWindowShown;
  });
  Navigate(false, ""); Navigate(true, "");
}
void FileBrowserWindow::SetPeerName(const std::string& name) {
  const auto text = S(name.empty() ? Tr("远端设备", "Remote device") : name);
  if (window_->get_peer_name() != text) window_->set_peer_name(text);
}
void FileBrowserWindow::UpdateActionLabels(int language) {
  language_ = localization::detail::ClampLanguageIndex(language);
  window_->set_send_local_text(S(localization::Literal("发送到本机", language)));
  window_->set_send_remote_text(S(localization::Literal("发送到远端", language)));
  window_->set_clear_completed_text(S(localization::Literal("清除已完成", language)));
  window_->set_open_directory_text(S(localization::Literal("打开目录", language)));
  window_->set_pause_text(S(localization::Literal("暂停", language)));
  window_->set_resume_text(S(localization::Literal("继续", language)));
  window_->set_rate_limit_text(S(localization::Literal("限速", language)));
  window_->set_rate_limit_unlimited_text(S(localization::Literal("不限速", language)));
  window_->set_overwrite_policy_text(S(localization::Literal("覆盖策略", language)));
  window_->set_auto_rename_text(S(localization::Literal("自动重命名", language)));
  window_->set_overwrite_text(S(localization::Literal("覆盖", language)));
  window_->set_skip_text(S(localization::Literal("跳过", language)));
  // Task-list chrome lives in one place so the "共 {n} 项" prefix/suffix split
  // and the ten-language literals cannot drift between the two setters.
  ui_localization::ApplyFileBrowserStrings(window_, language_);
  const auto rate_kbps = config_.GetTransferRateLimitKbps();
  const int rate_index = rate_kbps == 0 ? 0 : rate_kbps <= 1024 ? 1
      : rate_kbps <= 4096 ? 2 : rate_kbps <= 8192 ? 3 : rate_kbps <= 16384 ? 4 : 5;
  window_->set_rate_limit_index(rate_index);
  window_->set_overwrite_policy_index(
      config_.GetTransferOverwritePolicy() == 2 ? 0
      : config_.GetTransferOverwritePolicy() == 1 ? 1 : 2);
  window_->set_transfer_paused(transfers_.TransferPaused(remote_));
}
bool FileBrowserWindow::PresentsResult(uint32_t id,
    FileTransferManager::FileTransferState::FileTransferStatus status) const {
  using Status = FileTransferManager::FileTransferState::FileTransferStatus;
  if (!PresentsTransfers() ||
      (status != Status::Completed && status != Status::Failed && status != Status::Cancelled))
    return false;
  const auto task = history_.find(id);
  return task != history_.end() && !task->second.receiving && task->second.status == status &&
      (window_->get_tasks_expanded() || dismissed_.count(id));
}
bool FileBrowserWindow::CenterOnScreen() {
  auto& window = window_->window();
  if (window.is_fullscreen() || window.is_maximized()) return true;
#if defined(__APPLE__)
  return CenterNativeWindowOnScreen(window.appkit_view());
#elif defined(_WIN32)
  const HWND hwnd = window.win32_hwnd();
  if (!hwnd) return false;
  MONITORINFO monitor{sizeof(MONITORINFO)};
  RECT frame{};
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) ||
      !GetWindowRect(hwnd, &frame)) return false;
  const auto& bounds = monitor.rcWork;
  const auto target = window_geometry::CenteredPosition(
      {static_cast<int>(bounds.left), static_cast<int>(bounds.top),
       static_cast<int>(bounds.right - bounds.left),
       static_cast<int>(bounds.bottom - bounds.top)},
      {static_cast<int>(frame.right - frame.left),
       static_cast<int>(frame.bottom - frame.top)});
  return SetWindowPos(hwnd, nullptr, target.x, target.y, 0, 0,
      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
#else
#if defined(__linux__) && !defined(__OHOS__)
  // Native Wayland placement belongs to the compositor.
  if (window.wayland_surface()) return true;
#endif
  const auto position = window.position();
  const auto size = window.size();
  if (!size.width || !size.height) return false;
  const SDL_Point center{position.x + static_cast<int>(size.width / 2),
                         position.y + static_cast<int>(size.height / 2)};
  SDL_DisplayID display = SDL_GetDisplayForPoint(&center);
  if (!display) display = SDL_GetPrimaryDisplay();
  SDL_Rect bounds{};
  if (!display || !SDL_GetDisplayUsableBounds(display, &bounds)) return false;
  const auto target = window_geometry::CenteredPosition(
      {bounds.x, bounds.y, bounds.w, bounds.h},
      {static_cast<int>(size.width), static_cast<int>(size.height)});
  window.set_position(slint::PhysicalPosition({target.x, target.y}));
  return true;
#endif
}
void FileBrowserWindow::Show() {
  if (!Tick()) return;
  if (Visible()) {
    window_->window().set_minimized(false);
  } else {
    show_pending_ = true;
    ShowWhenReady();
  }
  if (!panes_[1].busy && !panes_[1].error.empty()) Navigate(true, panes_[1].path);
}
void FileBrowserWindow::ShowWhenReady() {
  if (!show_pending_ || !NativeWindowReady(window_->window())) return;
#if defined(__APPLE__)
  if (!ConfigureWhiteWindowChrome(window_->window().appkit_view())) return;
#endif
  if (!CenterOnScreen()) return;
  show_pending_ = false;
  window_->show();
}
void FileBrowserWindow::OpenDirectory(bool remote, const std::string& path) {
  const auto& pane = panes_[remote];
  if (!SessionConnected() || !pane.loaded || pane.busy || !pane.error.empty() ||
      window_->get_drag_active()) return;
  // Resolve the menu target by path, never by a row index that can change.
  const auto entry = std::find_if(pane.entries.begin(), pane.entries.end(), [&](const auto& item) {
    return item.row.directory && std::string_view(item.row.path) == path;
  });
  if (entry != pane.entries.end()) Navigate(remote, path);
}
void FileBrowserWindow::Navigate(bool remote, const std::string& path, bool tree) {
  if (window_->get_drag_active()) CancelDrag();
  if (remote && !transfers_.BrowserConnected(remote_)) { Notice(Error("disconnected"), true); return; }
  auto& pane = panes_[remote];
  for (auto it = requests_.begin(); it != requests_.end();) {
    if (it->second.remote == remote && it->second.tree == tree &&
        (!tree || it->second.path == path)) {
      transfers_.browser().CancelList(remote_, it->first);
      it = requests_.erase(it);
    } else ++it;
  }
  const auto id = transfers_.browser().List(remote_, path, !remote);
  requests_[id] = {remote, tree, path};
  if (!tree) {
    pane.request = id; pane.busy = true; pane.error.clear(); pane.loading.clear();
    pane.selected.clear(); UpdatePane(remote);
  }
}
void FileBrowserWindow::SortEntries(Pane& pane) {
  std::sort(pane.entries.begin(), pane.entries.end(), [&](const Entry& a, const Entry& b) {
    // Folder grouping and unknown dates stay consistent in either direction.
    if (a.row.directory != b.row.directory) return a.row.directory;
    int order = 0;
    if (pane.sort_column == SortColumn::Modified) {
      const bool a_known = a.modified_ms.has_value() || !a.modified.empty();
      const bool b_known = b.modified_ms.has_value() || !b.modified.empty();
      if (a_known != b_known) return a_known;
      if (a.modified_ms && b.modified_ms)
        order = *a.modified_ms < *b.modified_ms ? -1 : *a.modified_ms > *b.modified_ms ? 1 : 0;
      else order = a.modified.compare(b.modified);
    } else if (pane.sort_column == SortColumn::Size && !a.row.directory) {
      order = a.bytes < b.bytes ? -1 : a.bytes > b.bytes ? 1 : 0;
    }
    if (!order) order = a.name_key.compare(b.name_key);
    if (!order) order = std::string_view(a.row.name).compare(std::string_view(b.row.name));
    if (!order) order = std::string_view(a.row.path).compare(std::string_view(b.row.path));
    return pane.sort_descending ? order > 0 : order < 0;
  });
}
void FileBrowserWindow::UpdatePane(bool remote) {
  auto& pane = panes_[remote];
  std::vector<ui::BrowserEntry> rows;
  size_t selectable = 0;
  rows.reserve(pane.entries.size());
  for (const auto& item : pane.entries) {
    auto entry = item.row;
    if (!pane.show_hidden && std::string(entry.name).starts_with('.')) continue;
    if (!Matches(std::string(entry.name), pane.filter)) continue;
    entry.selected = pane.selected.count(std::string(entry.path));
    if (!entry.link) ++selectable;
    rows.push_back(std::move(entry));
  }
  ui::UpdateModelRows(*pane.rows, std::move(rows));
  const auto summary = pane.busy
      ? pane.loading.empty() ? Tr("正在读取目录…", "Loading folder…") : pane.loading
      : std::to_string(pane.rows->row_count()) + Tr(" 项", " items");
  const bool up = pane.loaded && !pane.parent.empty() && pane.path != pane.parent;
  const bool all = selectable > 0 && pane.selected.size() == selectable;
  if (remote) {
    window_->set_remote_sort_column(static_cast<int>(pane.sort_column));
    window_->set_remote_sort_descending(pane.sort_descending);
    window_->set_remote_current_path(S(pane.path));
    window_->set_remote_filter(S(pane.filter));
    window_->set_remote_hidden(pane.show_hidden);
    window_->set_remote_busy(pane.busy); window_->set_remote_status(S(summary));
    window_->set_remote_error(S(pane.error)); window_->set_remote_up(up);
    window_->set_remote_selected(static_cast<int>(pane.selected.size())); window_->set_remote_all(all);
  } else {
    window_->set_local_sort_column(static_cast<int>(pane.sort_column));
    window_->set_local_sort_descending(pane.sort_descending);
    window_->set_local_current_path(S(pane.path));
    window_->set_local_filter(S(pane.filter));
    window_->set_local_hidden(pane.show_hidden);
    window_->set_local_busy(pane.busy); window_->set_local_status(S(summary));
    window_->set_local_error(S(pane.error)); window_->set_local_up(up);
    window_->set_local_selected(static_cast<int>(pane.selected.size())); window_->set_local_all(all);
  }
  UpdateAvailability();
}

void FileBrowserWindow::UpdateTasks() {
  using Status = FileTransferManager::FileTransferState::FileTransferStatus;
  std::vector<TaskRow> rows;
  rows.reserve(history_.size());
  BatchCounts counts;
  const bool held = transfers_.TransferPaused(remote_);
  for (const auto& [id, task] : history_) {
    // Received files still trigger the directory refresh in Tick, but only this
    // computer's outgoing transfers appear in the task list.
    if (task.receiving || dismissed_.count(id)) continue;
    TaskRow item{};
    auto& row = item.row;
    row.id = static_cast<int>(id);
    row.receiving = task.receiving;
    row.direction = task.receiving ? 1 : 0;
    row.name = S(task.file_name);
    row.size = S(Size(task.file_size));
    row.kind = TaskKindForName(task.file_name);
    row.icon = S(TaskKindGlyph(row.kind));
    item.bytes = task.file_size;
    item.name_key = task.file_name;
    std::transform(item.name_key.begin(), item.name_key.end(), item.name_key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto started = task_started_ms_.find(id);
    item.started_ms = started == task_started_ms_.end() ? 0 : started->second;
    row.time = S(ClockText(item.started_ms));
    row.queued = task.status == Status::Queued;
    row.failed = task.status == Status::Failed;
    row.active = row.queued || task.status == Status::Sending ||
        task.status == Status::Receiving || task.status == Status::Cancelling;
    // The engine holds the peer's outbound pipeline rather than one file, so
    // every in-flight row reports the state the toolbar switch toggles.
    row.paused = held && (task.status == Status::Queued ||
        task.status == Status::Sending || task.status == Status::Receiving);
    row.can_cancel = row.queued || task.status == Status::Sending ||
        task.status == Status::Receiving;
    // Retry needs the manager's per-task entry point; the trait keeps the
    // button hidden until it exists instead of offering a dead control.
    row.can_retry = row.failed && kRetrySupported;
    row.retryable_id = row.can_retry ? static_cast<int>(id) : 0;
    row.progress = task.status == Status::Completed ? 1.f
        : task.file_size ? std::clamp(float(task.sent_bytes) / task.file_size, 0.f, 1.f) : 0.f;
    const auto detail = Tr("已传输 ", "Transferred ") + Size(task.sent_bytes) +
        (task.rate_bps ? "  ·  " + Size(task.rate_bps / 8) + "/s" : "");
    row.detail = S(detail);
    // Queued / paused / failed labels come from the catalog in Slint; the
    // percentage states are composed here because they carry a number.
    switch (task.status) {
      case Status::Queued: row.status = {}; break;
      case Status::Sending: row.status = S(Tr("发送中 ", "Sending ") + std::to_string(int(row.progress * 100)) + "%"); break;
      case Status::Receiving: row.status = S(Tr("接收中 ", "Receiving ") + std::to_string(int(row.progress * 100)) + "%"); break;
      case Status::Completed: row.status = S(Tr("已完成", "Completed")); break;
      case Status::Failed: row.status = {}; break;
      case Status::Cancelling: row.status = S(Tr("正在取消", "Cancelling")); break;
      case Status::Cancelled: row.status = S(Tr("已取消", "Cancelled")); break;
    }
    if (task.status == Status::Completed) ++counts.completed;
    else if (task.status == Status::Failed) ++counts.failed;
    else if (row.active) ++counts.active;
    rows.push_back(std::move(item));
  }
  const int column = task_sort_column_;
  const bool ascending = task_sort_ascending_;
  std::stable_sort(rows.begin(), rows.end(), [column, ascending](const TaskRow& a, const TaskRow& b) {
    int primary = 0;
    if (column == 1) {
      primary = a.bytes < b.bytes ? -1 : a.bytes > b.bytes ? 1 : 0;
    } else if (column == 2) {
      primary = a.started_ms < b.started_ms ? -1 : a.started_ms > b.started_ms ? 1 : 0;
    } else {
      primary = a.name_key.compare(b.name_key);
    }
    if (primary) return ascending ? primary < 0 : primary > 0;
    // Ties keep the newest task on top in either direction.
    return a.row.id > b.row.id;
  });
  std::vector<ui::BrowserTask> model_rows;
  model_rows.reserve(rows.size());
  for (auto& item : rows) model_rows.push_back(std::move(item.row));
  // Counters come from the queue itself, so the banner cannot drift from the
  // rows the engine is actually moving (UU's 共 N 项 · 完成 x · 失败 y).
  const auto batch = transfers_.BatchSummary(remote_);
  window_->set_batch_total(static_cast<int>(batch.total));
  window_->set_batch_completed(static_cast<int>(batch.completed));
  window_->set_batch_failed(static_cast<int>(batch.failed));
  window_->set_batch_active(static_cast<int>(batch.active));
  const auto summary = std::to_string(model_rows.size()) + Tr(" 项", " items") +
      (counts.active ? " · " + std::to_string(counts.active) + Tr(" 项进行中", " active") : "") +
      (counts.failed ? " · " + std::to_string(counts.failed) + Tr(" 项失败", " failed") : "");
  window_->set_task_summary(S(summary));
  ui::UpdateModelRows(*tasks_, std::move(model_rows));
}

void FileBrowserWindow::SortTasks(int column) {
  if (column < 0 || column > 2) return;
  if (task_sort_column_ == column) task_sort_ascending_ = !task_sort_ascending_;
  else {
    task_sort_column_ = column;
    // Name reads best ascending; size and time read best largest/newest first.
    task_sort_ascending_ = column == 0;
  }
  window_->set_sort_column(task_sort_column_);
  window_->set_sort_ascending(task_sort_ascending_);
  UpdateTasks();
}

void FileBrowserWindow::SetTransferHeld(bool held) {
  // One switch for the toolbar strip, the batch buttons and a row button: the
  // engine pauses the peer's outbound pipeline, so all three mean "hold this
  // peer". Replace the body with the queue-level call once the manager owns one.
  const bool paused = transfers_.TransferPaused(remote_);
  if (held && !paused) transfers_.PauseTransfer(remote_);
  else if (!held && paused) transfers_.ResumeTransfer(remote_);
  window_->set_transfer_paused(transfers_.TransferPaused(remote_));
  next_sync_ = {};
}

void FileBrowserWindow::UpdateTree(bool remote) {
  auto& pane = panes_[remote];
  std::vector<ui::BrowserTreeEntry> rows;
  const auto append = [&](auto&& self, const std::string& path, const std::string& name, int depth) -> void {
    if (depth > 24) return;
    const bool expanded = pane.expanded.count(path);
    rows.push_back({S(name), S(path), depth, expanded, pane.path == path});
    if (expanded) {
      const auto it = pane.directories.find(path);
      if (it != pane.directories.end())
        for (const auto& entry : it->second)
          if (entry.directory && !entry.link && (pane.show_hidden || !std::string(entry.name).starts_with('.')))
            self(self, std::string(entry.path), std::string(entry.name), depth + 1);
    }
  };
  if (!pane.home.empty()) append(append, pane.home, Tr("主目录", "Home"), 0);
  append(append, "@roots", Tr("磁盘", "Drives"), 0);
  if (pane.loaded && pane.path != "@roots" && pane.path != pane.home &&
      std::none_of(rows.begin(), rows.end(), [&](const auto& row) { return std::string(row.path) == pane.path; }))
    append(append, pane.path, Name(pane.path), 0);
  ui::UpdateModelRows(*pane.tree, std::move(rows));
}
bool FileBrowserWindow::TransferReady() const {
  return window_->get_connected() && panes_[0].loaded && panes_[1].loaded &&
      !panes_[0].busy && !panes_[1].busy && panes_[0].error.empty() && panes_[1].error.empty() &&
      panes_[0].path != "@roots" && panes_[1].path != "@roots" &&
      std::string(window_->get_local_path()) == panes_[0].path &&
      std::string(window_->get_remote_path()) == panes_[1].path;
}
bool FileBrowserWindow::SessionConnected() const {
  // A peer can reconnect with the same ID between UI ticks. Its old window
  // must still close, even if the new connection is already usable.
  return !disconnected_ && transfers_.BrowserConnected(remote_) &&
      connection_generation_ == transfers_.browser().Generation(remote_);
}
void FileBrowserWindow::UpdateAvailability() {
  const bool ready = TransferReady();
  window_->set_transfer_ready(ready);
  window_->set_can_upload(ready && !panes_[0].selected.empty());
  window_->set_can_download(ready && !panes_[1].selected.empty());
  // All selected rows share one immutable list, so selecting a large folder
  // doesn't allocate a copy of the entire selection for every draggable row.
  for (int side = 0; side < 2; ++side)
    drag_selection_[side] = std::make_shared<const std::vector<std::string>>(
        panes_[side].selected.begin(), panes_[side].selected.end());
  // Callback bindings don't track C++ container mutations. Refresh the drag
  // payload only when its selection/directory state changes, never every tick.
  const int revision = window_->get_selection_revision();
  window_->set_selection_revision(revision == INT_MAX ? 0 : revision + 1);
}
void FileBrowserWindow::SelectSource(bool remote) {
  window_->set_source_remote(remote);
  if (!panes_[!remote].selected.empty()) {
    panes_[!remote].selected.clear(); UpdatePane(!remote);
  }
}
slint::DataTransfer FileBrowserWindow::MakeDragData(bool remote, const std::string& path) const {
  slint::DataTransfer result;
  if (!TransferReady()) return result;
  const auto& source = panes_[remote]; const auto& target = panes_[!remote];
  const auto found = std::find_if(source.entries.begin(), source.entries.end(),
      [&](const auto& entry) { return !entry.row.link && std::string(entry.row.path) == path; });
  if (found == source.entries.end()) return result;
  auto payload = std::make_shared<DragPayload>();
  payload->owner = drag_owner_; payload->epoch = drag_epoch_; payload->remote = remote;
  payload->source_request = source.request; payload->target_request = target.request;
  payload->source_directory = source.path; payload->target_directory = target.path;
  if (source.selected.count(path)) payload->paths = drag_selection_[remote];
  else payload->paths = std::make_shared<const std::vector<std::string>>(std::initializer_list<std::string>{path});
  // Opaque application data: a remote path must never masquerade as a local
  // file URL when the pointer leaves this window or enters another session.
  result.set_user_data(std::move(payload));
  return result;
}
std::shared_ptr<FileBrowserWindow::DragPayload> FileBrowserWindow::DropPayload(
    bool remote, const slint::DataTransfer& data) const {
  const auto value = data.user_data();
  const auto stored = std::any_cast<std::shared_ptr<DragPayload>>(&value);
  if (!stored || !*stored) return {};
  const auto& payload = *stored;
  if (payload->owner.lock() != drag_owner_ || payload->epoch != drag_epoch_ ||
      payload->consumed || payload->remote == remote || !payload->paths || payload->paths->empty() ||
      !TransferReady() || !SessionConnected()) return {};
  const auto& source = panes_[payload->remote]; const auto& target = panes_[remote];
  if (source.request != payload->source_request || target.request != payload->target_request ||
      source.path != payload->source_directory || target.path != payload->target_directory) return {};
  return payload;
}
void FileBrowserWindow::CancelDrag() {
  ++drag_epoch_;
  window_->set_drag_active(false); window_->set_drag_count(0);
  UpdateAvailability();
}
bool FileBrowserWindow::QueueTransfer(bool download, const std::vector<std::string>& paths,
                                      const std::string& destination) {
  if (paths.empty() || !TransferReady() || !SessionConnected()) return false;
  transfers_.browser().Copy(remote_, paths, destination, download);
  Notice(Tr("正在准备传输 → ", "Preparing transfer → ") + destination, false);
  panes_[download].selected.clear(); UpdatePane(download);
  return true;
}
void FileBrowserWindow::Transfer(bool download) {
  if (!TransferReady()) return;
  auto& source = panes_[download];
  const std::vector<std::string> paths(source.selected.begin(), source.selected.end());
  QueueTransfer(download, paths, panes_[!download].path);
}
void FileBrowserWindow::Notice(const std::string& message, bool error) {
  window_->set_notice(S(message)); window_->set_notice_error(error);
}
std::string FileBrowserWindow::Error(const std::string& code) const {
  // Codes that live above the ops channel (the manager's own authorization
  // answer) keep their own wording.
  if (code == "browser_not_open")
    return Label("请先在控制端打开本连接的文件管理器，再浏览控制端目录。");
  for (const auto& row : kErrorTexts)
    if (code == row.code) return Label(std::string(row.key));
  // Unknown code: keep it verbatim, so a dialog and a log line agree on it.
  return Label("操作失败") + " (" + code + ")";
}
std::string FileBrowserWindow::Label(const std::string& key) const {
  // A key missing from the catalog degrades to its Chinese source instead of an
  // empty label, so a half-translated build still reads.
  const auto& text = localization::Literal(key, language_);
  return text.empty() ? key : text;
}
void FileBrowserWindow::ApplyCapabilities() {
  ui::BrowserOps remote{};
  remote.supported = ops_.supported; remote.open = ops_.open;
  remote.reveal = ops_.reveal; remote.rename = ops_.rename;
  remote.remove = ops_.remove; remote.mkdir = ops_.mkdir;
  remote.refresh = ops_.refresh; remote.stat = ops_.stat;
  remote.temp_clear = ops_.temp_clear;
  window_->set_remote_ops(remote);
  // The local pane executes nothing through the host: this computer already has
  // a file manager, so only a fresh local listing stays enabled.
  ui::BrowserOps local{};
  local.refresh = true;
  window_->set_local_ops(local);
}
void FileBrowserWindow::DisableCapability(OpKind kind) {
  switch (kind) {
    case OpKind::Stat: ops_.stat = false; ops_.supported = false; break;
    case OpKind::Mkdir: ops_.mkdir = false; break;
    case OpKind::Rename: ops_.rename = false; break;
    case OpKind::Delete: ops_.remove = false; break;
    case OpKind::Open: ops_.open = false; break;
    case OpKind::Reveal: ops_.reveal = false; break;
    // An older peer kept it in the probe answer but does not know temp_clear:
    // the button and the menu entry go and stay away for this session.
    case OpKind::TempClear: ops_.temp_clear = false; break;
    case OpKind::Probe: ops_.supported = false; break;
  }
  ApplyCapabilities();
}
void FileBrowserWindow::ProbeOperations() {
  const auto path = panes_[1].path;
  if (path.empty() || !SessionConnected()) return;
  OpRequest request;
  request.kind = OpKind::Probe;
  request.path = path;
  request.paths = {path};
  SendOp(std::move(request));
}
uint32_t FileBrowserWindow::SendOp(OpRequest request) {
  if (!SessionConnected()) { Notice(Error("disconnected"), true); return 0; }
  // Field names are the ones file_browser.cpp reads: mkdir/rename take a parent
  // path plus a single name, delete/stat take a "paths" array, open/reveal one
  // path.
  FileBrowser::Json body;
  switch (request.kind) {
    case OpKind::Probe:
    case OpKind::Stat:
      if (request.paths.empty()) return 0;
      body = {{"op", "stat"}, {"paths", request.paths}};
      break;
    case OpKind::Mkdir:
      body = {{"op", "mkdir"}, {"path", request.path}, {"name", request.name}};
      break;
    case OpKind::Rename:
      body = {{"op", "rename"}, {"path", request.path}, {"name", request.name}};
      break;
    case OpKind::Delete:
      if (request.paths.empty()) return 0;
      body = {{"op", "delete"}, {"paths", request.paths}};
      break;
    case OpKind::Open:
      body = {{"op", "open"}, {"path", request.path}};
      break;
    case OpKind::Reveal:
      body = {{"op", "reveal"}, {"path", request.path}};
      break;
    case OpKind::TempClear:
      // The host reads exactly one field: the directory to sweep. It must be the
      // peer's receive directory or something inside it, which is why an empty
      // target never reaches the wire.
      if (request.path.empty()) return 0;
      body = {{"op", "temp_clear"}, {"directory", request.path}};
      break;
  }
  const auto id = transfers_.browser().Op(remote_, std::move(body));
  if (!id) { Notice(Error("busy"), true); return 0; }
  // A peer that never answers leaves an entry behind; the table stays bounded so
  // a long session cannot accumulate them.
  if (op_requests_.size() >= 64) op_requests_.erase(op_requests_.begin());
  op_requests_[id] = std::move(request);
  return id;
}
void FileBrowserWindow::RefreshRemote() {
  if (panes_[1].path.empty()) return;
  Navigate(true, panes_[1].path);
}
void FileBrowserWindow::HandleOpReply(uint32_t id, const OpRequest& request,
                                      const FileBrowser::Json& result) {
  (void)id;
  const bool ok = result.value("ok", false);
  const std::string code = ok ? std::string{} : result.value("error", std::string("unknown"));
  if (request.kind == OpKind::Probe) {
    // One `stat` on the open folder is the whole probe: the eight management ops
    // ship as one block, so a peer that answers it answers all of them, and an
    // older peer answers "unsupported" and keeps only 刷新.
    if (!ok && code != "unsupported") {
      // Only "unsupported" proves an old peer. A timeout, a stale path or a
      // closed channel says nothing about the ops, so stay closed and ask again
      // on the next listing instead of locking the menu for the whole session.
      op_probed_ = false;
      return;
    }
    ops_.supported = ok;
    ops_.open = ok; ops_.reveal = ok; ops_.rename = ok; ops_.remove = ok;
    ops_.mkdir = ok; ops_.stat = ok; ops_.refresh = true;
    // temp_clear ships with the same block: a peer that answers the probe can
    // honour it, and an "unsupported" answer to the op itself narrows it again.
    ops_.temp_clear = ok;
    ApplyCapabilities();
    return;
  }
  if (!ok) {
    if (code == "unsupported") DisableCapability(request.kind);
    // A request that already answered never keeps a modal up.
    if (dialog_ != Dialog::None) CloseDialog();
    Notice(Error(code), true);
    return;
  }
  switch (request.kind) {
    case OpKind::Stat: {
      const auto found = result.find("entries");
      if (found == result.end() || !found->is_array() || found->empty()) {
        if (dialog_ != Dialog::None) CloseDialog();
        Notice(Error("not_found"), true);
        return;
      }
      const auto& entry = found->front();
      const auto entry_error = entry.value("error", std::string{});
      if (!entry.value("exists", false) || !entry_error.empty()) {
        if (dialog_ != Dialog::None) CloseDialog();
        Notice(Error(entry_error.empty() ? "not_found" : entry_error), true);
        return;
      }
      // 属性: name / size / modified / directory, straight from the stat reply.
      const bool directory = entry.value("directory", false);
      dialog_ = Dialog::Properties;
      dialog_path_ = request.path;
      dialog_paths_.clear();
      dialog_names_.clear();
      dialog_name_ = Name(request.path);
      dialog_busy_ = false;
      window_->set_dialog_kind(4);
      window_->set_dialog_title(S(Label("属性")));
      window_->set_dialog_message(S(""));
      window_->set_dialog_note(S(""));
      window_->set_dialog_name(S(dialog_name_));
      window_->set_dialog_input(S(""));
      window_->set_dialog_hint(S(""));
      window_->set_dialog_input_visible(false);
      window_->set_dialog_busy(false);
      window_->set_dialog_danger(false);
      window_->set_dialog_directory(directory);
      window_->set_dialog_size(S(directory ? Tr("—", "—")
          : Size(entry.value("size", uint64_t{}))));
      window_->set_dialog_modified(S(entry.value("modified", std::string{})));
      break;
    }
    case OpKind::Mkdir:
      CloseDialog();
      Notice(Label("已创建文件夹"), false);
      RefreshRemote();
      break;
    case OpKind::Rename:
      CloseDialog();
      Notice(Label("已完成重命名"), false);
      RefreshRemote();
      break;
    case OpKind::Delete: {
      // The op answers ok:true even when one entry of the batch failed, so the
      // batch only counts as clean when no per-path result carries an error.
      std::string first_error;
      size_t failed = 0;
      if (const auto found = result.find("results");
          found != result.end() && found->is_array()) {
        for (const auto& item : *found) {
          if (item.value("ok", false)) continue;
          ++failed;
          if (first_error.empty()) first_error = item.value("error", std::string("unknown"));
        }
      }
      CloseDialog();
      if (failed) Notice(Error(first_error), true);
      else Notice(Label("已删除所选项目"), false);
      RefreshRemote();
      break;
    }
    case OpKind::Open:
    case OpKind::Reveal:
      // The peer opened its own file manager / selected the entry there; there is
      // nothing to redraw on this side.
      break;
    case OpKind::TempClear: {
      // {directory, root, removed[], removed_count, failed}: the counters are
      // what the toast reports, and `root` becomes the remembered sweep target.
      const auto root = result.value("root", std::string{});
      if (!root.empty()) sweep_root_ = root;
      const auto count = result.value("removed_count", size_t{});
      const auto failed = result.value("failed", size_t{});
      CloseDialog();
      if (count && failed) {
        // 部分失败既不吞掉成功数，也不假称全部成功：两个数字都报出来。
        std::string text = Label("已清理 {n} 项，{m} 项未能删除");
        if (const auto at = text.find("{n}"); at != std::string::npos)
          text.replace(at, 3, std::to_string(count));
        if (const auto at = text.find("{m}"); at != std::string::npos)
          text.replace(at, 3, std::to_string(failed));
        Notice(text, true);
        if (request.path == panes_[1].path) RefreshRemote();
        break;
      }
      if (!count) {
        // The op answers ok:true even when every removal failed, so a non-zero
        // `failed` with nothing removed goes through the error table instead of
        // claiming success.
        if (failed) Notice(Label("临时文件未能全部清理，请稍后重试"), true);
        // Nothing was left over: the op still succeeded, so say so plainly
        // instead of asking the user to wonder about a zero.
        else Notice(Label("已清理临时文件"), false);
        break;
      }
      std::string text = Label("已清理 {n} 项");
      if (const auto at = text.find("{n}"); at != std::string::npos)
        text.replace(at, 3, std::to_string(count));
      Notice(text, false);
      // A sweep only drops ".crossdesk-*" staging folders. It changes the listing
      // exactly when one of them sat in the folder this pane is showing.
      if (request.path == panes_[1].path) RefreshRemote();
      break;
    }
    case OpKind::Probe:
      break;
  }
}
std::vector<std::string> FileBrowserWindow::MenuTargets(const std::string& path) const {
  const auto& pane = panes_[1];
  // A right-click inside the selection acts on the whole selection, like
  // Explorer; a right-click elsewhere acts on that row only.
  if (pane.selected.count(path)) return {pane.selected.begin(), pane.selected.end()};
  return {path};
}
bool FileBrowserWindow::ValidateEntryName(const std::string& name, std::string* hint) const {
  const auto fail = [hint](const std::string& text) {
    if (hint) *hint = text;
    return false;
  };
  if (name.empty() || name.find_first_not_of(" \t") == std::string::npos)
    return fail(Label("名称不能为空"));
  if (name.find_first_of("/\\") != std::string::npos)
    return fail(Label("名称不能包含路径分隔符"));
  // Mirrors file_browser_ops::ValidEntryName: "." / ".." , Windows-illegal
  // characters and a trailing dot or space would either escape the folder or be
  // silently rewritten by the host filesystem.
  if (name == "." || name == ".." || name.front() == ' ' || name.back() == '.' ||
      name.back() == ' ' || name.find_first_of(":*?\"<>|") != std::string::npos)
    return fail(Label("名称包含不允许的字符"));
  for (const auto character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) return fail(Label("名称包含不允许的字符"));
  }
  return true;
}
bool FileBrowserWindow::NameTaken(const std::string& name) const {
  // Exact comparison: a Linux host legitimately holds "A" next to "a", and the
  // host's own `exists` error covers a case-insensitive filesystem.
  return std::any_of(panes_[1].entries.begin(), panes_[1].entries.end(),
      [&](const auto& entry) { return std::string(entry.row.name) == name; });
}
void FileBrowserWindow::RequestDelete(const std::vector<std::string>& paths) {
  if (paths.empty() || paths.size() > file_browser_ops::kMaxDeletePaths) {
    Notice(Error(std::string(file_browser_ops::kErrorTooManyFiles)), true);
    CloseDialog();
    return;
  }
  OpRequest request;
  request.kind = OpKind::Delete;
  request.paths = paths;
  if (!SendOp(std::move(request))) CloseDialog();
}
void FileBrowserWindow::RequestRename(const std::string& path, const std::string& name) {
  OpRequest request;
  request.kind = OpKind::Rename;
  request.path = path;
  request.name = name;
  if (!SendOp(std::move(request))) CloseDialog();
}
void FileBrowserWindow::RequestMkdir(const std::string& parent, const std::string& name) {
  OpRequest request;
  request.kind = OpKind::Mkdir;
  request.path = parent;
  request.name = name;
  if (!SendOp(std::move(request))) CloseDialog();
}
void FileBrowserWindow::ShowRenameDialog(const std::string& path) {
  if (!ops_.rename || !SessionConnected() || path.empty()) return;
  dialog_ = Dialog::Rename;
  dialog_path_ = path;
  dialog_paths_.clear();
  dialog_names_.clear();
  dialog_name_ = Name(path);
  dialog_busy_ = false;
  window_->set_dialog_kind(1);
  window_->set_dialog_title(S(Label("重命名")));
  window_->set_dialog_message(S(""));
  window_->set_dialog_note(S(""));
  window_->set_dialog_name(S(dialog_name_));
  window_->set_dialog_input(S(dialog_name_));
  window_->set_dialog_hint(S(""));
  window_->set_dialog_size(S(""));
  window_->set_dialog_modified(S(""));
  window_->set_dialog_directory(false);
  window_->set_dialog_danger(false);
  window_->set_dialog_input_visible(true);
  window_->set_dialog_busy(false);
}
void FileBrowserWindow::ShowNewFolderDialog() {
  if (!ops_.mkdir || !SessionConnected()) return;
  // "@roots" is this UI's drives pseudo-folder, not a host directory: there is
  // nothing to create a folder in.
  if (panes_[1].path.empty() || panes_[1].path == "@roots") return;
  dialog_ = Dialog::NewFolder;
  dialog_path_ = panes_[1].path;
  dialog_paths_.clear();
  dialog_names_.clear();
  // UU pre-fills 新建文件夹; the user types over it.
  dialog_name_ = Label("新建文件夹");
  dialog_busy_ = false;
  window_->set_dialog_kind(2);
  window_->set_dialog_title(S(Label("新建文件夹")));
  window_->set_dialog_message(S(""));
  window_->set_dialog_note(S(dialog_path_));
  window_->set_dialog_name(S(""));
  window_->set_dialog_input(S(dialog_name_));
  window_->set_dialog_hint(S(""));
  window_->set_dialog_size(S(""));
  window_->set_dialog_modified(S(""));
  window_->set_dialog_directory(true);
  window_->set_dialog_danger(false);
  window_->set_dialog_input_visible(true);
  window_->set_dialog_busy(false);
}
void FileBrowserWindow::ShowDeleteDialog(const std::vector<std::string>& paths) {
  if (!ops_.remove || paths.empty()) return;
  if (paths.size() > file_browser_ops::kMaxDeletePaths) {
    Notice(Error(std::string(file_browser_ops::kErrorTooManyFiles)), true);
    return;
  }
  dialog_ = Dialog::Delete;
  dialog_paths_ = paths;
  dialog_names_.clear();
  dialog_names_.reserve(paths.size());
  bool directory = false;
  for (const auto& path : paths) {
    dialog_names_.push_back(Name(path));
    const auto entry = std::find_if(panes_[1].entries.begin(), panes_[1].entries.end(),
        [&](const auto& item) { return std::string(item.row.path) == path; });
    if (entry != panes_[1].entries.end() && entry->row.directory) directory = true;
  }
  dialog_path_ = paths.size() == 1 ? paths.front() : std::string{};
  // A single target is named; a batch says how many entries it holds.
  dialog_name_ = paths.size() == 1 ? dialog_names_.front()
                                   : std::to_string(paths.size()) + " " + Label("项");
  dialog_busy_ = false;
  window_->set_dialog_kind(3);
  window_->set_dialog_title(S(Label("删除")));
  window_->set_dialog_message(S(Label("确定删除以下项目？")));
  // Deleting a folder removes everything inside it, so the note says so.
  window_->set_dialog_note(S(directory ? Label("该目录及其中的全部内容都会被删除。")
                                      : Label("此操作不可撤销。")));
  window_->set_dialog_name(S(dialog_name_));
  window_->set_dialog_input(S(""));
  window_->set_dialog_hint(S(""));
  window_->set_dialog_size(S(""));
  window_->set_dialog_modified(S(""));
  window_->set_dialog_directory(directory);
  window_->set_dialog_input_visible(false);
  window_->set_dialog_busy(false);
  window_->set_dialog_danger(true);
}
std::string FileBrowserWindow::SweepDirectory() const {
  const auto& open = panes_[1].path;
  const bool usable = !open.empty() && open != "@roots";
  if (sweep_root_.empty()) return usable ? open : std::string{};
  // The open folder wins while it is the receive root or lives inside it, so a
  // user who drilled into a subfolder sweeps just that subfolder. Anything else
  // (a folder outside the root, "@roots") falls back to the root itself, which
  // the op always accepts.
  if (!usable) return sweep_root_;
  const auto same = [](char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
  };
  const bool inside = open.size() >= sweep_root_.size() &&
      std::equal(sweep_root_.begin(), sweep_root_.end(), open.begin(), same) &&
      (open.size() == sweep_root_.size() || open[sweep_root_.size()] == '/' ||
       open[sweep_root_.size()] == '\\');
  return inside ? open : sweep_root_;
}
void FileBrowserWindow::ShowTempClearDialog() {
  if (!ops_.temp_clear || !SessionConnected()) return;
  const auto directory = SweepDirectory();
  if (directory.empty()) {
    Notice(Label("请先打开远端接收目录，再清理临时文件。"), false);
    return;
  }
  dialog_ = Dialog::TempClear;
  dialog_path_ = directory;
  dialog_paths_.clear();
  dialog_names_.clear();
  dialog_name_.clear();
  dialog_busy_ = false;
  window_->set_dialog_kind(5);
  window_->set_dialog_title(S(Label("清理临时文件")));
  // Only staging leftovers go: the receive engine's ".crossdesk-*" folders from
  // an interrupted transfer. Finished files are never touched, and the note names
  // the folder so the confirmation is about a concrete directory.
  window_->set_dialog_message(S(Label("只清理未完成的暂存残留，已完成的文件不会被删除。")));
  window_->set_dialog_note(S(directory));
  window_->set_dialog_name(S(""));
  window_->set_dialog_input(S(""));
  window_->set_dialog_hint(S(""));
  window_->set_dialog_size(S(""));
  window_->set_dialog_modified(S(""));
  window_->set_dialog_directory(true);
  window_->set_dialog_input_visible(false);
  window_->set_dialog_busy(false);
  window_->set_dialog_danger(true);
}
void FileBrowserWindow::RequestTempClear(const std::string& directory) {
  if (directory.empty()) { CloseDialog(); return; }
  OpRequest request;
  request.kind = OpKind::TempClear;
  request.path = directory;
  if (!SendOp(std::move(request))) CloseDialog();
}
void FileBrowserWindow::ShowDialogHint(const std::string& text) {
  window_->set_dialog_hint(S(text));
}
void FileBrowserWindow::CloseDialog() {
  dialog_ = Dialog::None;
  dialog_path_.clear();
  dialog_name_.clear();
  dialog_paths_.clear();
  dialog_names_.clear();
  dialog_busy_ = false;
  window_->set_dialog_kind(0);
  window_->set_dialog_input_visible(false);
  window_->set_dialog_busy(false);
  window_->set_dialog_hint(S(""));
  window_->set_dialog_danger(false);
}
int FileBrowserWindow::KeyboardPane() const {
  // Delete / F2 only exist while the remote pane is the active one and the host
  // proved it serves the management ops.
  return active_pane_ == 1 && ops_.supported ? 1 : -1;
}
bool FileBrowserWindow::Tick() {
  using Status = FileTransferManager::FileTransferState::FileTransferStatus;
  // Connection lifetime is checked every UI tick, outside the throttled
  // directory/progress refresh. All native window operations stay on this thread.
  if (!SessionConnected()) {
    disconnected_ = true;
    show_pending_ = false;
    window_->set_connected(false);
    CancelDrag();
    CloseDialog();
    requests_.clear();
    op_requests_.clear();
    window_->hide();
    return false;
  }
  ShowWhenReady();
  const auto now = std::chrono::steady_clock::now();
  if (now < next_sync_) return true;
  next_sync_ = now + std::chrono::milliseconds(150);
  for (auto& event : transfers_.browser().TakeEvents(remote_)) {
    if (!event.id) {
      const bool ok = event.result.value("ok", false);
      if (!ok || !window_->get_notice_error())
        Notice(ok ? event.result.value("message", "") == "cancelling"
                    ? Tr("正在取消传输…", "Cancelling transfer…")
                    : Tr("已加入传输队列", "Added to transfer queue")
                  : Error(event.result.value("error", "unknown")), !ok);
      if (ok) refresh_at_ = now + std::chrono::seconds(1);
      continue;
    }
    // File-management replies carry their own request table: an id here is a
    // stat / mkdir / rename / delete / open / reveal answer, never a listing.
    if (const auto op = op_requests_.find(event.id); op != op_requests_.end()) {
      const auto request = op->second;
      op_requests_.erase(op);
      HandleOpReply(event.id, request, event.result);
      continue;
    }
    const auto found = requests_.find(event.id);
    if (found == requests_.end()) continue;
    const auto request = found->second;
    auto& pane = panes_[request.remote];
    if (!request.tree && pane.request != event.id) continue;
    if (event.result.value("loading", false)) {
      if (!request.tree) {
        pane.loading = Tr("正在加载 ", "Loading ") + std::to_string(event.result.value("loaded", size_t{})) +
            " / " + std::to_string(event.result.value("total", size_t{})) + Tr(" 项…", " items…");
        if (request.remote) window_->set_remote_status(S(pane.loading));
        else window_->set_local_status(S(pane.loading));
      }
      continue;
    }
    requests_.erase(found);
    if (!request.tree) pane.busy = false;
    if (!event.result.value("ok", false)) {
      pane.error = Error(event.result.value("error", "unknown"));
      if (request.tree) pane.expanded.erase(request.path);
    } else {
      try {
        const auto path = event.result.at("path").get<std::string>();
        const auto& entries = event.result.at("entries");
        if (!entries.is_array()) throw std::runtime_error("invalid");
        std::vector<ui::BrowserEntry> rows;
        rows.reserve(entries.size());
        std::vector<Entry> sortable;
        if (!request.tree) sortable.reserve(entries.size());
        for (const auto& entry : entries) {
          ui::BrowserEntry row{};
          const auto name = entry.at("name").get<std::string>();
          if (name.empty() || name == "." || name == "..") continue;
          // Paths from a different operating system are opaque to this UI.
          row.name = S(name); row.path = S(path == "@roots" ? entry.at("path").get<std::string>() : Join(path, name));
          row.directory = entry.at("directory").get<bool>(); row.link = entry.value("link", false);
          const auto bytes = entry.at("size").get<uint64_t>();
          const auto timestamp = entry.at("modified").get<std::string>();
          row.size = S(Size(bytes));
          auto modified = timestamp.substr(0, 10);
          std::replace(modified.begin(), modified.end(), '-', '/');
          row.modified = S(modified);
          if (!request.tree) {
            Entry item{row, name, timestamp, bytes, std::nullopt};
            std::transform(item.name_key.begin(), item.name_key.end(), item.name_key.begin(),
                [](unsigned char c) { return std::tolower(c); });
            if (entry.contains("modified_ms") && entry["modified_ms"].is_number_integer())
              item.modified_ms = entry["modified_ms"].get<int64_t>();
            sortable.push_back(std::move(item));
          }
          rows.push_back(std::move(row));
        }
        pane.directories[path] = rows;
        if (!request.tree) {
          // A name filter belongs to the open folder. Preserve it on refresh,
          // but don't silently hide entries after navigating somewhere else.
          if (pane.path != path) pane.filter.clear();
          pane.path = path; pane.parent = event.result.at("parent").get<std::string>();
          if (request.path.empty()) pane.home = path;
          pane.entries = std::move(sortable); pane.error.clear(); pane.loaded = true;
          SortEntries(pane);
          pane.expanded.insert(path);
        }
      } catch (...) { pane.error = Error("unknown"); }
    }
    if (!request.tree) {
      if (request.remote) window_->set_remote_path(S(pane.path)); else window_->set_local_path(S(pane.path));
    }
    UpdatePane(request.remote); UpdateTree(request.remote);
    // The first remote listing is the moment to ask what the peer serves; until
    // it answers, the management entries stay hidden and only 刷新 is offered.
    // "@roots" is this UI's own drives pseudo-folder, not a host path.
    if (request.remote && !request.tree && !op_probed_ &&
        !panes_[1].path.empty() && panes_[1].path != "@roots") {
      op_probed_ = true;
      ProbeOperations();
    }
  }
  bool new_tasks = false;
  for (const auto& task : transfers_.BrowserTransfers(remote_)) {
    const bool first = !history_.count(task.file_id);
    new_tasks = new_tasks || (first && !task.receiving);
    // history_ carries no timestamp, so the arrival time for the time column
    // and the time sort key is stamped once, on first sight.
    if (first) task_started_ms_[task.file_id] = NowMs();
    history_[task.file_id] = task;
  }
  if (new_tasks) window_->set_tasks_expanded(true);
  for (const auto& [id, task] : history_)
    if (task.status == Status::Completed && completed_.insert(id).second)
      refresh_at_ = now + std::chrono::milliseconds(500);
  UpdateTasks();
  if (!window_->get_drag_active() && refresh_at_ != std::chrono::steady_clock::time_point{} && now >= refresh_at_) {
    refresh_at_ = {};
    for (int side = 0; side < 2; ++side)
      if (panes_[side].loaded && !panes_[side].busy && panes_[side].selected.empty() &&
          std::string(side ? window_->get_remote_path() : window_->get_local_path()) == panes_[side].path)
        Navigate(side, panes_[side].path);
  }
  const bool ready = TransferReady();
  if (window_->get_transfer_ready() != ready) UpdateAvailability();
  return true;
}

}  // namespace crossdesk
