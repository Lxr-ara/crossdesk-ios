/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FILE_BROWSER_WINDOW_H_
#define _FILE_BROWSER_WINDOW_H_

#include "crossdesk_ui.h"
#include "features/file_transfer/file_transfer_manager.h"
#include <map>
#include <optional>
#include <set>

namespace crossdesk {

class ConfigCenter;

class FileBrowserWindow {
 public:
  FileBrowserWindow(FileTransferManager& transfers, ConfigCenter& config,
                    std::string remote, bool controller, int language);
  void Show();
  void SetPeerName(const std::string& name);
  void UpdateActionLabels(int language);
  // False means this connection ended; the UI owner should discard the window.
  bool Tick();
  bool Visible() const { return window_->window().is_visible(); }
  bool PresentsTransfers() const { return Visible() && !window_->window().is_minimized(); }
  bool PresentsResult(uint32_t id, FileTransferManager::FileTransferState::FileTransferStatus status) const;

 private:
  enum class SortColumn { Name, Modified, Size };
  struct Entry {
    ui::BrowserEntry row;
    std::string name_key, modified;
    uint64_t bytes = 0;
    std::optional<int64_t> modified_ms;
  };
  struct Pane {
    std::string path, parent, home, filter, error, loading;
    bool loaded = false, busy = false, show_hidden = false;
    uint32_t request = 0;
    SortColumn sort_column = SortColumn::Name;
    bool sort_descending = false;
    std::vector<Entry> entries;
    std::set<std::string> selected, expanded;
    std::map<std::string, std::vector<ui::BrowserEntry>> directories;
    std::shared_ptr<slint::VectorModel<ui::BrowserEntry>> rows =
        std::make_shared<slint::VectorModel<ui::BrowserEntry>>();
    std::shared_ptr<slint::VectorModel<ui::BrowserTreeEntry>> tree =
        std::make_shared<slint::VectorModel<ui::BrowserTreeEntry>>();
  };
  struct Request { bool remote, tree; std::string path; };
  // The file management ops (mkdir / rename / delete / stat / open / reveal /
  // temp_clear) each answer with one reply, matched here by request id. `kind`
  // says how the body is read and which capability an "unsupported" reply turns
  // off.
  enum class OpKind { Probe, Stat, Mkdir, Rename, Delete, Open, Reveal, TempClear };
  struct OpRequest {
    OpKind kind = OpKind::Probe;
    std::string path, name;
    std::vector<std::string> paths;
  };
  // What the peer actually serves. Filled by the probe (one `stat` on the open
  // folder) and narrowed by any op that answers "unsupported", so a menu entry
  // is only offered when the host proved it can honour it.
  struct Capabilities {
    bool supported = false, open = false, reveal = false, rename = false,
         remove = false, mkdir = false, refresh = true, stat = false,
         // temp_clear (清理临时文件). Rides the same probe as the block above and
         // is dropped for good once the peer answers "unsupported" to it.
         temp_clear = false;
  };
  // Which management dialog is on screen. The pixels live in file_browser.slint;
  // the meaning (target path, typed name, delete set) lives here so a dialog can
  // never outlive the request it was opened for. TempClear is the confirmation
  // that carries no input field, only title/body/note.
  enum class Dialog { None, Rename, NewFolder, Delete, Properties, TempClear };
  struct DragPayload {
    std::weak_ptr<int> owner;
    uint64_t epoch = 0;
    bool remote = false, consumed = false;
    uint32_t source_request = 0, target_request = 0;
    std::string source_directory, target_directory;
    std::shared_ptr<const std::vector<std::string>> paths;
  };
  void Navigate(bool remote, const std::string& path, bool tree = false);
  void OpenDirectory(bool remote, const std::string& path);
  bool CenterOnScreen();
  void ShowWhenReady();
  void UpdatePane(bool remote);
  void SortEntries(Pane& pane);
  // Builds, sorts and pushes the transfer-task rows. Kept separate from Tick so
  // a header click can re-push the same model without touching the pace of the
  // directory/progress refresh.
  void UpdateTasks();
  void SortTasks(int column);
  // Single switch behind the toolbar pause button, the batch strip and a row
  // button; the engine holds the peer's outbound pipeline, not one file.
  void SetTransferHeld(bool held);
  void UpdateTree(bool remote);
  bool TransferReady() const;
  bool SessionConnected() const;
  void UpdateAvailability();
  void SelectSource(bool remote);
  slint::DataTransfer MakeDragData(bool remote, const std::string& path) const;
  std::shared_ptr<DragPayload> DropPayload(bool remote, const slint::DataTransfer& data) const;
  void CancelDrag();
  bool QueueTransfer(bool download, const std::vector<std::string>& paths,
                     const std::string& destination);
  void Transfer(bool download);
  void Notice(const std::string& message, bool error);
  std::string Error(const std::string& code) const;
  std::string Tr(const char* chinese, const char* english) const;
  // Catalog lookup for everything the management layer shows, so the menu, the
  // dialogs and the error table follow the ten-language catalog instead of the
  // zh/en fallback Tr() carries.
  std::string Label(const std::string& key) const;
  // ---------------------------------------------------- file management ops --
  // One `stat` on the open remote folder decides which menu entries exist. Until
  // it answers, only 刷新 is offered: nothing that could silently fail.
  void ProbeOperations();
  void ApplyCapabilities();
  void DisableCapability(OpKind kind);
  uint32_t SendOp(OpRequest request);
  void HandleOpReply(uint32_t id, const OpRequest& request, const FileBrowser::Json& result);
  // Entries the delete/rename actions address: the selection when it contains
  // the clicked row, otherwise just that row.
  std::vector<std::string> MenuTargets(const std::string& path) const;
  void RequestDelete(const std::vector<std::string>& paths);
  void RequestRename(const std::string& path, const std::string& name);
  void RequestMkdir(const std::string& parent, const std::string& name);
  void ShowRenameDialog(const std::string& path);
  void ShowNewFolderDialog();
  void ShowDeleteDialog(const std::vector<std::string>& paths);
  // 清理临时文件: a confirmation first, then one `temp_clear` on the directory
  // the peer's receive engine stages in. The reply never changes the listing, so
  // nothing is re-read afterwards.
  void ShowTempClearDialog();
  void RequestTempClear(const std::string& directory);
  // Directory handed to `temp_clear`: the folder the remote pane is showing when
  // it lies inside the receive root learned from a previous reply, and that root
  // otherwise. Empty when neither is known, which keeps the dialog closed.
  std::string SweepDirectory() const;
  // 属性 opens from the `stat` reply itself (HandleOpReply), so a peer that
  // cannot answer never leaves an empty info dialog on screen.
  void ShowDialogHint(const std::string& text);
  void CloseDialog();
  // Re-lists the open remote folder in place, which is how a finished mkdir,
  // rename or delete shows up without reopening the window.
  void RefreshRemote();
  // Client side validation shared by rename and mkdir: empty, unchanged and
  // colliding names never leave this process.
  bool ValidateEntryName(const std::string& name, std::string* hint) const;
  bool NameTaken(const std::string& name) const;
  // Delete/F2/F5 target whichever pane the pointer last worked in; only the
  // remote pane has a host that can execute them.
  int KeyboardPane() const;
  FileTransferManager& transfers_;
  std::string remote_;
  ConfigCenter& config_;
  uint64_t connection_generation_;
  bool chinese_;
  // Catalog language index for the task-list literals (Literal() needs it, Tr()
  // only covers the zh/en fallback).
  int language_ = 1;
  bool disconnected_ = false;
  bool show_pending_ = false;
  slint::ComponentHandle<ui::FileBrowserWindow> window_ = ui::FileBrowserWindow::create();
  Pane panes_[2];
  std::shared_ptr<int> drag_owner_ = std::make_shared<int>(0);
  std::shared_ptr<const std::vector<std::string>> drag_selection_[2];
  uint64_t drag_epoch_ = 0;
  std::map<uint32_t, Request> requests_;
  std::map<uint32_t, OpRequest> op_requests_;
  Capabilities ops_;
  Dialog dialog_ = Dialog::None;
  // Dialog payload: the entry the dialog was opened for, the name the user is
  // editing and the delete set (one entry for a right-click, the whole
  // selection for the Delete key).
  std::string dialog_path_, dialog_name_;
  std::vector<std::string> dialog_paths_;
  std::vector<std::string> dialog_names_;
  // Receive root the peer named in its last `temp_clear` reply. Remembered so a
  // second click sweeps the same folder even after the user browsed elsewhere:
  // the op only accepts the receive directory or something inside it.
  std::string sweep_root_;
  bool dialog_busy_ = false;
  bool op_probed_ = false;
  int active_pane_ = 1;
  std::set<uint32_t> dismissed_, completed_;
  std::map<uint32_t, FileTransferManager::FileTransferState::FileTransferInfo> history_;
  std::shared_ptr<slint::VectorModel<ui::BrowserTask>> tasks_ =
      std::make_shared<slint::VectorModel<ui::BrowserTask>>();
  // 0 = name, 1 = size, 2 = first seen. Time descending reproduces the previous
  // newest-first order, so the default view does not change.
  int task_sort_column_ = 2;
  bool task_sort_ascending_ = false;
  // Wall-clock arrival time per task, shown in the time column and used as the
  // time sort key. history_ has no timestamp of its own.
  std::map<uint32_t, int64_t> task_started_ms_;
  std::chrono::steady_clock::time_point next_sync_{}, refresh_at_{};
};

}  // namespace crossdesk

#endif
