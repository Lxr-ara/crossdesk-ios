/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-20
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "features/photo/photo_gallery_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "rd_log.h"
#include "ui/model_update.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

namespace crossdesk {
namespace {

// Thumbnails requested per UI tick. The control channel is reliable but
// ordered, so a few in flight keep the album filling without stalling video.
constexpr int kThumbsPerTick = 3;
constexpr int kMaxThumbAttempts = 3;

std::string Utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}

std::string Size(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
  char text[64];
  std::snprintf(text, sizeof(text), unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
  return text;
}

std::string Name(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

#if defined(_WIN32)
// Reads CF_DIB from the clipboard and writes it as a PNG photo so the regular
// reliable file stream can carry it. Returns an empty string when the
// clipboard holds no image.
std::string ClipboardImageToPng(const std::filesystem::path& directory) {
  if (!OpenClipboard(nullptr)) return {};
  struct Closer { ~Closer() { CloseClipboard(); } } closer;
  const auto handle = GetClipboardData(CF_DIB);
  if (!handle) return {};
  const auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(handle));
  if (!header) return {};
  struct Unlocker {
    HGLOBAL handle;
    ~Unlocker() { GlobalUnlock(handle); }
  } unlocker{handle};
  const auto bit_count = header->biBitCount;
  if (bit_count != 24 && bit_count != 32) return {};
  const int width = header->biWidth;
  const int height = header->biHeight < 0 ? -header->biHeight : header->biHeight;
  if (width <= 0 || height <= 0 || width > 20000 || height > 20000) return {};
  const bool top_down = header->biHeight < 0;
  const std::size_t stride = (static_cast<std::size_t>(width) * bit_count + 31) / 32 * 4;
  const auto* pixels = reinterpret_cast<const unsigned char*>(header) +
      header->biSize + (header->biClrUsed ? header->biClrUsed * 4 : 0);
  std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4, 255);
  for (int y = 0; y < height; ++y) {
    const int source_y = top_down ? y : height - 1 - y;
    const auto* row = pixels + stride * static_cast<std::size_t>(source_y);
    auto* target = rgba.data() + static_cast<std::size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      const auto* pixel = row + static_cast<std::size_t>(x) * (bit_count / 8);
      target[x * 4 + 0] = pixel[2];
      target[x * 4 + 1] = pixel[1];
      target[x * 4 + 2] = pixel[0];
      target[x * 4 + 3] = bit_count == 32 ? pixel[3] : 255;
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto target = directory / ("clipboard-" + std::to_string(stamp) + ".png");
  const auto text = target.string();
  if (!stbi_write_png(text.c_str(), width, height, 4, rgba.data(), width * 4)) return {};
  return text;
}
#endif

}  // namespace

std::string PhotoGalleryWindow::Tr(const char* chinese, const char* english) const {
  return chinese_ ? chinese : english;
}

slint::SharedString PhotoGalleryWindow::S(std::string_view value) const {
  return slint::SharedString(value);
}

PhotoGalleryWindow::PhotoGalleryWindow(FileTransferManager& transfers, std::string remote,
                                       bool controller, int language, Options options)
    : transfers_(transfers), remote_(std::move(remote)),
      connection_generation_(transfers_.browser().Generation(remote_)),
      options_(std::move(options)), chinese_(language == 0 || language == 3),
      local_(options_.local) {
  // Drop stale results for this channel from a previous window.
  if (!local_) transfers_.browser().TakeEvents(remote_, "gallery");
  window_->window().set_size(slint::LogicalSize({940, 660}));
  window_->set_tiles(tiles_);
  window_->set_albums(albums_);
  window_->set_window_title(S(Tr("照片", "Photos")));
  window_->set_role_text(S(local_ ? Tr("本机相册", "This computer")
                                  : controller ? Tr("被控端照片", "Host photos")
                                               : Tr("控制端照片", "Controller photos")));
  window_->set_text_albums(S(Tr("相册", "Albums")));
  window_->set_text_refresh(S(Tr("刷新", "Refresh")));
  window_->set_text_select_all(S(Tr("全选", "Select all")));
  window_->set_text_clear(S(Tr("清空选择", "Clear")));
  window_->set_text_download(S(Tr("保存到本机", "Save to computer")));
  window_->set_text_send(S(Tr("发送照片", "Send photos")));
  window_->set_text_paste(S(Tr("发送剪贴板图片", "Send clipboard image")));
  window_->set_text_selected(S(Tr("张已选", "selected")));
  window_->set_text_empty(S(Tr("这个位置还没有照片。", "No photos here yet.")));
  window_->set_text_loading(S(Tr("正在加载缩略图…", "Loading thumbnails…")));
  window_->set_text_open_folder(S(Tr("打开相册目录", "Open album folder")));
  window_->set_text_up(S(Tr("上级目录", "Up")));
  window_->set_text_root(S(Tr("照片根目录", "Photo root")));
  window_->on_refresh([this] { Load(root_path_.empty() ? "@photos" : root_path_); });
  window_->on_album_selected([this](slint::SharedString album) {
    album_filter_ = std::string(album);
    ApplyFilter();
  });
  window_->on_toggle_tile([this](int index) { Toggle(index); });
  window_->on_select_all([this] { SelectAll(); });
  window_->on_clear_selection([this] { ClearSelection(); });
  window_->on_download([this] { DownloadSelected(); });
  window_->on_send([this] { SendPickedPhotos(); });
  window_->on_paste([this] { PasteImage(); });
  window_->on_open_folder([this] { OpenFolder(); });
  window_->on_open_photo([this](int index) { OpenPhoto(index); });
  window_->on_up([this] { GoUp(); });
  window_->on_root([this] { ResetToPhotoRoot(); });
  Load("@photos");
}

void PhotoGalleryWindow::Show() { window_->show(); }

void PhotoGalleryWindow::SetPeerName(const std::string& name) {
  window_->set_peer_name(S(name));
}

void PhotoGalleryWindow::Notice(std::string_view message) {
  window_->set_status(S(message));
}

bool PhotoGalleryWindow::SessionConnected() const {
  if (local_) return connected_;
  return connected_ && transfers_.BrowserConnected(remote_) &&
      connection_generation_ == transfers_.browser().Generation(remote_);
}

bool PhotoGalleryWindow::CanSend() const {
  if (local_) {
    return static_cast<bool>(options_.send_files) && !options_.send_remote.empty();
  }
  return SessionConnected();
}

void PhotoGalleryWindow::Load(const std::string& path) {
  if (!SessionConnected()) return;
  busy_ = true;
  window_->set_busy(true);
  window_->set_error(S(""));
  error_.clear();
  thumb_queue_.clear();
  local_thumb_cursor_ = 0;
  selected_.clear();
  entries_.clear();
  visible_.clear();
  RebuildRows();
  if (local_) {
    if (scan_.valid()) scan_.wait();
    photo::ScanOptions scan_options;
    scan_ = std::async(std::launch::async, [path, scan_options] {
      return photo::Scan(path, scan_options);
    });
    Notice(Tr("正在读取本机照片…", "Reading local photos…"));
    return;
  }
  transfers_.browser().Gallery(remote_, path, true);
  Notice(Tr("正在读取照片…", "Reading photos…"));
}

void PhotoGalleryWindow::HarvestLocalScan() {
  if (!scan_.valid()) return;
  if (scan_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
  auto result = scan_.get();
  busy_ = false;
  window_->set_busy(false);
  if (!result.ok) {
    error_ = Tr("无法读取该目录，请检查路径与访问权限。",
                "Cannot read this folder. Check the path and permissions.");
    window_->set_error(S(error_));
    return;
  }
  root_path_ = result.root;
  if (photo_root_.empty()) photo_root_ = result.root;
  window_->set_current_path(S(root_path_));
  std::set<std::string> albums;
  std::vector<Tile> tiles;
  for (const auto& item : result.entries) {
    Tile tile;
    tile.path = item.path;
    tile.name = item.name;
    tile.album = item.album;
    tile.modified = item.date;
    tile.bytes = item.size;
    albums.insert(tile.album.empty() ? Tr("未分组", "Unsorted") : tile.album);
    tiles.push_back(std::move(tile));
  }
  entries_ = std::move(tiles);
  std::vector<slint::SharedString> rows;
  rows.push_back(S(Tr("全部照片", "All photos")));
  for (const auto& album : albums) rows.push_back(S(album));
  ui::UpdateModelRows(*albums_, std::move(rows));
  window_->set_album(S(album_filter_));
  ApplyFilter();
  if (result.truncated) {
    Notice(std::to_string(entries_.size()) + Tr(" 张照片（列表已截断）", " photos (truncated)"));
  } else {
    Notice(std::to_string(entries_.size()) + Tr(" 张照片", " photos"));
  }
}

void PhotoGalleryWindow::HandleEvent(const FileBrowser::Event& event) {
  if (event.result.value("kind", std::string{}) == "thumb") {
    HandleThumbnail(event.result);
    return;
  }
  if (!event.id) return;
  HandleGalleryPage(event.result);
}

void PhotoGalleryWindow::HandleGalleryPage(const FileBrowser::Json& result) {
  if (result.value("loading", false)) {
    Notice(Tr("已读取 ", "Loaded ") + std::to_string(result.value("loaded", std::size_t{})) +
           " / " + std::to_string(result.value("total", std::size_t{})) + Tr(" 张", " photos"));
    return;
  }
  busy_ = false;
  window_->set_busy(false);
  if (!result.value("ok", false)) {
    error_ = Tr("无法读取照片列表，请检查远端相册目录与权限后刷新。",
                "Cannot read the photo list. Check the remote folder and permissions.");
    window_->set_error(S(error_));
    return;
  }
  try {
    root_path_ = result.at("path").get<std::string>();
    if (photo_root_.empty()) photo_root_ = root_path_;
    window_->set_current_path(S(root_path_));
    std::vector<Tile> tiles;
    std::set<std::string> albums;
    for (const auto& item : result.at("entries")) {
      Tile tile;
      tile.path = item.at("path").get<std::string>();
      tile.name = item.at("name").get<std::string>();
      tile.album = item.value("album", std::string{});
      tile.modified = item.value("modified", std::string{});
      tile.bytes = item.value("size", uint64_t{});
      albums.insert(tile.album.empty() ? Tr("未分组", "Unsorted") : tile.album);
      tiles.push_back(std::move(tile));
    }
    entries_ = std::move(tiles);
    std::vector<slint::SharedString> rows;
    rows.push_back(S(Tr("全部照片", "All photos")));
    for (const auto& album : albums) rows.push_back(S(album));
    ui::UpdateModelRows(*albums_, std::move(rows));
    if (!album_filter_.empty() && album_filter_ != Tr("全部照片", "All photos") &&
        !albums.count(album_filter_)) {
      album_filter_.clear();
    }
    window_->set_album(S(album_filter_));
    ApplyFilter();
    Notice(std::to_string(entries_.size()) + Tr(" 张照片", " photos"));
  } catch (...) {
    error_ = Tr("照片列表格式异常，请更新远端后重试。",
                "Unexpected photo list from the peer. Update it and retry.");
    window_->set_error(S(error_));
  }
}

void PhotoGalleryWindow::HandleThumbnail(const FileBrowser::Json& result) {
  const auto path = result.value("path", std::string{});
  if (!result.value("ok", false)) {
    // Let the tile retry a couple of times; a permanently broken file stops.
    for (auto& tile : entries_) {
      if (tile.path != path) continue;
      if (tile.attempts < kMaxThumbAttempts) tile.requested = false;
      break;
    }
    return;
  }
  const auto data = result.value("data", std::string{});
  if (path.empty() || data.empty()) return;
  std::vector<unsigned char> jpeg;
  if (!photo::Base64Decode(data, &jpeg)) return;
  std::vector<unsigned char> rgba;
  int width = 0, height = 0;
  if (!photo::DecodeRgba(jpeg.data(), jpeg.size(), &rgba, &width, &height)) return;
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    if (entries_[index].path != path) continue;
    entries_[index].has_thumb = true;
    entries_[index].requested = false;
    ApplyThumbnail(index, rgba, width, height);
    break;
  }
}

void PhotoGalleryWindow::ApplyThumbnail(std::size_t index, const std::vector<unsigned char>& rgba,
                                        int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (index >= entries_.size()) return;
  const auto& path = entries_[index].path;
  auto buffer = slint::SharedPixelBuffer<slint::Rgba8Pixel>(
      static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  std::memcpy(buffer.begin(), rgba.data(), rgba.size());
  const slint::Image image(std::move(buffer));
  for (std::size_t row = 0; row < tiles_->row_count(); ++row) {
    auto data = tiles_->row_data(static_cast<int>(row));
    if (!data) break;
    if (std::string(data->path) != path) continue;
    auto updated = *data;
    updated.has_thumb = true;
    updated.thumb = image;
    tiles_->set_row_data(static_cast<int>(row), updated);
    break;
  }
}

void PhotoGalleryWindow::ApplyFilter() {
  visible_.clear();
  const auto all = Tr("全部照片", "All photos");
  for (const auto& tile : entries_) {
    const auto album = tile.album.empty() ? Tr("未分组", "Unsorted") : tile.album;
    if (!album_filter_.empty() && album_filter_ != all && album != album_filter_) continue;
    visible_.push_back(tile.path);
  }
  RebuildRows();
  RequestThumbnails();
}

void PhotoGalleryWindow::RebuildRows() {
  std::vector<ui::PhotoTile> rows;
  rows.reserve(visible_.size());
  for (const auto& path : visible_) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Tile& tile) { return tile.path == path; });
    if (found == entries_.end()) continue;
    ui::PhotoTile row{};
    row.name = S(found->name);
    row.path = S(found->path);
    row.album = S(found->album);
    row.size_text = S(Size(found->bytes));
    row.modified = S(found->modified);
    row.selected = selected_.count(found->path) != 0;
    row.has_thumb = found->has_thumb;
    rows.push_back(std::move(row));
  }
  ui::UpdateModelRows(*tiles_, std::move(rows));
  UpdateHeader();
}

void PhotoGalleryWindow::UpdateHeader() {
  window_->set_selected_count(static_cast<int>(selected_.size()));
}

void PhotoGalleryWindow::RequestThumbnails() {
  if (local_) {
    GenerateLocalThumbnail();
    return;
  }
  int in_flight = 0;
  std::vector<std::string> wanted;
  for (const auto& path : visible_) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Tile& tile) { return tile.path == path; });
    if (found == entries_.end() || found->has_thumb) continue;
    if (found->requested) { ++in_flight; continue; }
    if (found->attempts >= kMaxThumbAttempts) continue;
    wanted.push_back(path);
  }
  loading_thumbs_ = !wanted.empty();
  for (const auto& path : wanted) {
    if (in_flight >= kThumbsPerTick) break;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Tile& tile) { return tile.path == path; });
    if (found == entries_.end()) continue;
    found->requested = true;
    ++found->attempts;
    ++in_flight;
    transfers_.browser().Thumbnail(remote_, path, photo::kTileEdge);
  }
}

void PhotoGalleryWindow::GenerateLocalThumbnail() {
  // Local decoding runs on the UI thread but one image per tick keeps the
  // window responsive even for 24 MP photos.
  if (local_thumb_cursor_ >= visible_.size()) {
    loading_thumbs_ = false;
    return;
  }
  const auto path = visible_[local_thumb_cursor_++];
  const auto found = std::find_if(entries_.begin(), entries_.end(),
      [&](const Tile& tile) { return tile.path == path; });
  if (found == entries_.end() || found->has_thumb) return;
  const auto thumb = photo::MakeThumbnail(path, photo::kTileEdge, photo::kMaxThumbnailBytes);
  if (!thumb.ok) {
    if (++found->attempts < kMaxThumbAttempts) --local_thumb_cursor_;
    return;
  }
  std::vector<unsigned char> rgba;
  int width = 0, height = 0;
  if (photo::DecodeRgba(thumb.jpeg.data(), thumb.jpeg.size(), &rgba, &width, &height)) {
    found->has_thumb = true;
    const auto index = static_cast<std::size_t>(found - entries_.begin());
    ApplyThumbnail(index, rgba, width, height);
  }
  loading_thumbs_ = local_thumb_cursor_ < visible_.size();
}

void PhotoGalleryWindow::Toggle(int index) {
  if (index < 0 || index >= static_cast<int>(visible_.size())) return;
  const auto& path = visible_[index];
  if (!selected_.erase(path)) selected_.insert(path);
  for (std::size_t row = 0; row < tiles_->row_count(); ++row) {
    auto data = tiles_->row_data(static_cast<int>(row));
    if (!data) break;
    auto updated = *data;
    updated.selected = selected_.count(std::string(updated.path)) != 0;
    tiles_->set_row_data(static_cast<int>(row), updated);
  }
  UpdateHeader();
}

void PhotoGalleryWindow::SelectAll() {
  for (const auto& path : visible_) selected_.insert(path);
  RebuildRows();
}

void PhotoGalleryWindow::ClearSelection() {
  selected_.clear();
  RebuildRows();
}

void PhotoGalleryWindow::DownloadSelected() {
  if (selected_.empty() || !SessionConnected()) return;
  if (local_) {
    // Local photos are already here: reveal their folder instead of copying.
    OpenFolder();
    return;
  }
  const auto directory = transfers_.PhotoSaveDirectory();
  std::vector<std::string> paths(selected_.begin(), selected_.end());
  transfers_.browser().Copy(remote_, paths, Utf8(directory), true);
  Notice(Tr("已加入下载队列，完成后保存到 ", "Queued for download into ") + Utf8(directory));
  refresh_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

void PhotoGalleryWindow::SendPickedPhotos() {
  if (!CanSend() || !options_.pick_images) return;
  options_.pick_images([this](std::vector<std::string> paths) {
    if (paths.empty() || !CanSend()) return;
    if (local_) {
      options_.send_files(paths);
      Notice(std::to_string(paths.size()) + Tr(" 张照片已发送给当前会话",
                                              " photos sent to the session"));
      return;
    }
    if (!SessionConnected()) return;
    const auto destination = root_path_.empty() ? std::string("@photos") : root_path_;
    transfers_.browser().Copy(remote_, paths, destination, false);
    Notice(std::to_string(paths.size()) + Tr(" 张照片已加入发送队列",
                                            " photos queued to send"));
    refresh_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  });
}

void PhotoGalleryWindow::PasteImage() {
  if (!SessionConnected()) return;
#if defined(_WIN32)
  const auto png = ClipboardImageToPng(transfers_.PhotoSaveDirectory());
  if (png.empty()) {
    Notice(Tr("剪贴板中没有可用图片。", "No image on the clipboard."));
    return;
  }
  if (local_) {
    if (options_.send_files) options_.send_files({png});
  } else {
    const auto destination = root_path_.empty() ? std::string("@photos") : root_path_;
    transfers_.browser().Copy(remote_, {png}, destination, false);
  }
  Notice(Tr("剪贴板图片已发送：", "Clipboard image sent: ") + Name(png));
#else
  Notice(Tr("当前平台暂不支持发送剪贴板图片。",
            "Clipboard images are not supported on this platform yet."));
#endif
}

void PhotoGalleryWindow::OpenFolder() {
  if (!options_.reveal) return;
  if (!selected_.empty()) {
    // Reveal the folder of the first selected photo when it is local.
    options_.reveal(Utf8(std::filesystem::path(*selected_.begin()).parent_path()));
    return;
  }
  options_.reveal(Utf8(transfers_.PhotoSaveDirectory()));
}

void PhotoGalleryWindow::OpenPhoto(int index) {
  if (index < 0 || index >= static_cast<int>(visible_.size())) return;
  selected_.clear();
  selected_.insert(visible_[index]);
  RebuildRows();
  if (local_) {
    // Nothing to download: reveal the file's folder.
    OpenFolder();
    return;
  }
  DownloadSelected();
}

void PhotoGalleryWindow::GoUp() {
  if (root_path_.empty()) return;
  const std::filesystem::path current = std::filesystem::path(root_path_);
  const auto parent = current.parent_path();
  if (parent.empty() || parent == current) {
    ResetToPhotoRoot();
    return;
  }
  Load(Utf8(parent));
}

void PhotoGalleryWindow::ResetToPhotoRoot() {
  album_filter_.clear();
  window_->set_album(S(""));
  Load(photo_root_.empty() ? "@photos" : photo_root_);
}

bool PhotoGalleryWindow::Tick() {
  if (!SessionConnected()) {
    connected_ = false;
    window_->set_connected(false);
    window_->hide();
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (local_) HarvestLocalScan();
  if (now < next_sync_) return true;
  next_sync_ = now + std::chrono::milliseconds(150);
  if (!local_) {
    for (auto& event : transfers_.browser().TakeEvents(remote_, "gallery")) HandleEvent(event);
  }
  RequestThumbnails();
  if (refresh_at_ != std::chrono::steady_clock::time_point{} && now >= refresh_at_) {
    refresh_at_ = {};
    Load(root_path_.empty() ? "@photos" : root_path_);
  }
  return true;
}

}  // namespace crossdesk
