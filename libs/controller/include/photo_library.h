/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-20
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 *
 * Photo library: album scanning, photo classification and thumbnail
 * generation shared by the desktop photo gallery and the file browser RPC.
 *
 * Nothing here touches the file transfer wire format. Photos travel over the
 * existing reliable "file" stream; only the control_data JSON RPC gained the
 * "gallery" / "thumb" operations that this module backs.
 */

#ifndef _PHOTO_LIBRARY_H_
#define _PHOTO_LIBRARY_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace crossdesk {
namespace photo {

// Lower case, without the dot: "jpg", "jpeg", "png", "gif", "bmp", "webp",
// "tif", "tiff", "heic", "avif", "jfif", "ico".
bool IsPhotoExtension(const std::string& extension);
// True when the file name ends in a supported photo extension (case folded).
bool IsPhotoPath(const std::string& path);

// Long edge of a gallery tile thumbnail, in pixels.
inline constexpr int kTileEdge = 192;
// Hard ceiling for one encoded thumbnail. The control channel rejects messages
// above 60000 bytes, and the base64 payload grows by 4/3.
inline constexpr std::size_t kMaxThumbnailBytes = 34000;
inline constexpr int kDefaultMaxEntries = 4096;
inline constexpr int kDefaultMaxDepth = 6;

struct Entry {
  std::string path;       // absolute UTF-8 path
  std::string name;       // file name
  std::string extension;  // lower case, no dot
  std::string album;      // "<folder>/<YYYY-MM>", used for grouping
  std::string date;       // "YYYY-MM-DD"
  std::uint64_t size = 0;
  std::int64_t modified_ms = 0;
  int width = 0;   // 0 until the thumbnail decodes the header
  int height = 0;
};

struct ScanOptions {
  int max_entries = kDefaultMaxEntries;
  int max_depth = kDefaultMaxDepth;
  bool recursive = true;
  bool include_unsupported = false;
};

struct ScanResult {
  bool ok = false;
  std::string error;  // empty on success
  bool truncated = false;
  std::string root;   // resolved absolute root that was scanned
  std::vector<Entry> entries;  // newest first
};

// Resolves the well known photo roots: "@photos" (Pictures), "@home",
// "@desktop". Any other value must be an existing absolute directory.
std::filesystem::path ResolveRoot(const std::string& path, std::string* error);
std::string PhotoRoot();  // Pictures, or the home directory when absent

// Recursive, symlink rejecting scan. Sorted newest first, then by name.
ScanResult Scan(const std::string& path, const ScanOptions& options = {},
                const std::function<bool()>& cancelled = {});

struct Thumbnail {
  bool ok = false;
  std::string error;
  std::vector<unsigned char> jpeg;  // encoded JPEG bytes
  int width = 0;                    // encoded size
  int height = 0;
  int source_width = 0;
  int source_height = 0;
};

// Decodes the photo, box-downsamples it to fit max_edge and re-encodes JPEG,
// retrying with a smaller edge / lower quality until it fits max_bytes.
Thumbnail MakeThumbnail(const std::string& path, int max_edge = kTileEdge,
                        std::size_t max_bytes = kMaxThumbnailBytes);

// Decodes any supported image into tightly packed RGBA8. Used by the desktop
// gallery to turn a wire thumbnail back into a slint::Image.
bool DecodeRgba(const unsigned char* data, std::size_t size,
                std::vector<unsigned char>* rgba, int* width, int* height);

std::string Base64Encode(const unsigned char* data, std::size_t size);
bool Base64Decode(const std::string& text, std::vector<unsigned char>* out);

// "Photos/2026-11" grouping for received files. Keeps the original name and
// appends " (2)", " (3)" ... when the destination already exists.
std::string AlbumForDate(std::int64_t modified_ms);
bool ClassifyReceivedPhoto(const std::filesystem::path& file,
                           const std::filesystem::path& base, bool group_by_date,
                           std::filesystem::path* moved_to);

}  // namespace photo
}  // namespace crossdesk

#endif
