/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-20
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 *
 * Photo album window. Two modes share one UI:
 *   - remote: lists the peer's photos through the file browser RPC
 *     ("gallery" / "thumb") and downloads through the reliable file stream.
 *   - local:  lists this computer's photos by scanning the disk in-process and
 *     pushes them to the currently selected controller through the ordinary
 *     file stream, so the host side has an album entry too.
 */

#ifndef _PHOTO_GALLERY_WINDOW_H_
#define _PHOTO_GALLERY_WINDOW_H_

#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <photo_library.h>

#include "crossdesk_ui.h"
#include "features/file_transfer/file_transfer_manager.h"

namespace crossdesk {

class PhotoGalleryWindow {
 public:
  struct Options {
    // Local mode browses this computer instead of the peer.
    bool local = false;
    // Target controller for local mode pushes. Empty disables the send action.
    std::string send_remote;
    // Hands picked files to the ordinary file stream (no destination RPC).
    std::function<void(const std::vector<std::string>&)> send_files;
    // Native multi-select picker for the "send photos" action.
    std::function<void(std::function<void(std::vector<std::string>)>)> pick_images;
    // Reveals a folder in the desktop file manager.
    std::function<void(const std::string&)> reveal;
  };

  PhotoGalleryWindow(FileTransferManager& transfers, std::string remote, bool controller,
                     int language, Options options = {});
  void Show();
  void SetPeerName(const std::string& name);
  // False means the window should be discarded.
  bool Tick();
  bool Visible() const { return window_->window().is_visible(); }

 private:
  struct Tile {
    std::string path, name, album, modified;
    std::uint64_t bytes = 0;
    bool has_thumb = false;
    bool requested = false;
    int attempts = 0;
  };
  void Load(const std::string& path);
  void HarvestLocalScan();
  void ApplyFilter();
  void RebuildRows();
  void UpdateHeader();
  void RequestThumbnails();
  void GenerateLocalThumbnail();
  void HandleEvent(const FileBrowser::Event& event);
  void HandleGalleryPage(const FileBrowser::Json& result);
  void HandleThumbnail(const FileBrowser::Json& result);
  void ApplyThumbnail(std::size_t index, const std::vector<unsigned char>& rgba,
                      int width, int height);
  void Toggle(int index);
  void SelectAll();
  void ClearSelection();
  void DownloadSelected();
  void SendPickedPhotos();
  void PasteImage();
  void OpenFolder();
  void OpenPhoto(int index);
  void GoUp();
  void ResetToPhotoRoot();
  bool SessionConnected() const;
  bool CanSend() const;
  std::string Tr(const char* chinese, const char* english) const;
  slint::SharedString S(std::string_view value) const;
  void Notice(std::string_view message);

  FileTransferManager& transfers_;
  std::string remote_;
  uint64_t connection_generation_ = 0;
  Options options_;
  bool chinese_ = true;
  bool local_ = false;
  bool connected_ = true;
  bool busy_ = false;
  bool loading_thumbs_ = false;
  std::string root_path_;
  std::string photo_root_;
  std::string album_filter_;
  std::string error_;
  std::vector<Tile> entries_;
  std::vector<std::string> visible_;
  std::set<std::string> selected_;
  std::deque<std::string> thumb_queue_;
  std::size_t local_thumb_cursor_ = 0;
  std::future<photo::ScanResult> scan_;
  slint::ComponentHandle<ui::PhotoGalleryWindow> window_ =
      ui::PhotoGalleryWindow::create();
  std::shared_ptr<slint::VectorModel<ui::PhotoTile>> tiles_ =
      std::make_shared<slint::VectorModel<ui::PhotoTile>>();
  std::shared_ptr<slint::VectorModel<slint::SharedString>> albums_ =
      std::make_shared<slint::VectorModel<slint::SharedString>>();
  std::chrono::steady_clock::time_point next_sync_{};
  std::chrono::steady_clock::time_point refresh_at_{};
};

}  // namespace crossdesk

#endif
