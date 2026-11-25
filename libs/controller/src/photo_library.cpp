/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-20
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "photo_library.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

// Self contained decoders: STATIC keeps the symbols local so this translation
// unit never clashes with the desktop thumbnail module, which owns the normal
// stb implementation.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace crossdesk {
namespace photo {
namespace {

namespace fs = std::filesystem;

constexpr std::array<const char*, 12> kExtensions = {
    "jpg", "jpeg", "png", "gif", "bmp", "webp", "tif", "tiff", "heic", "avif", "jfif", "ico"};

fs::path PathFromUtf8(std::string_view value) {
  return fs::path(std::u8string(value.begin(), value.end()));
}

std::string Utf8(const fs::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}

std::string Fold(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

fs::path Home() {
#ifdef _WIN32
  const auto* home = std::getenv("USERPROFILE");
#else
  const auto* home = std::getenv("HOME");
#endif
  return home ? PathFromUtf8(home) : fs::current_path();
}

std::int64_t ToMillis(fs::file_time_type time, fs::file_time_type file_now,
                      std::chrono::system_clock::time_point system_now) {
  // Portable clock bridging: apply the file clock's offset to system time. MSVC
  // has no file_clock::to_sys, so this matches the browser's fallback branch.
  const auto offset = time - file_now;
  const auto bridged = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      system_now + offset);
  return std::chrono::duration_cast<std::chrono::milliseconds>(bridged.time_since_epoch()).count();
}

std::string Format(std::int64_t ms, const char* pattern) {
  const auto stamp = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &stamp);
#else
  localtime_r(&stamp, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), pattern, &tm);
  return buffer;
}

// Area average downscale. Input and output are RGBA8.
std::vector<unsigned char> Downscale(const unsigned char* source, int width, int height,
                                     int target_width, int target_height) {
  std::vector<unsigned char> out(static_cast<std::size_t>(target_width) * target_height * 4, 0);
  if (!width || !height || !target_width || !target_height) return out;
  for (int y = 0; y < target_height; ++y) {
    const int y0 = static_cast<int>(static_cast<long long>(y) * height / target_height);
    int y1 = static_cast<int>(static_cast<long long>(y + 1) * height / target_height);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < target_width; ++x) {
      const int x0 = static_cast<int>(static_cast<long long>(x) * width / target_width);
      int x1 = static_cast<int>(static_cast<long long>(x + 1) * width / target_width);
      if (x1 <= x0) x1 = x0 + 1;
      std::uint64_t sum[4] = {0, 0, 0, 0};
      std::uint64_t count = 0;
      for (int sy = y0; sy < y1 && sy < height; ++sy) {
        const unsigned char* row = source + (static_cast<std::size_t>(sy) * width) * 4;
        for (int sx = x0; sx < x1 && sx < width; ++sx) {
          const unsigned char* pixel = row + static_cast<std::size_t>(sx) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += pixel[c];
          ++count;
        }
      }
      auto* target = out.data() + (static_cast<std::size_t>(y) * target_width + x) * 4;
      if (!count) continue;
      for (int c = 0; c < 4; ++c) target[c] = static_cast<unsigned char>(sum[c] / count);
    }
  }
  return out;
}

struct JpegBuffer {
  std::vector<unsigned char>* out;
};

void WriteJpeg(void* context, void* data, int size) {
  auto* sink = static_cast<JpegBuffer*>(context);
  if (!sink || !sink->out || size <= 0) return;
  const auto* begin = static_cast<const unsigned char*>(data);
  sink->out->insert(sink->out->end(), begin, begin + size);
}

int FitEdge(int width, int height, int max_edge) {
  if (width <= 0 || height <= 0) return 0;
  const int longest = std::max(width, height);
  if (longest <= max_edge) return longest;
  return max_edge;
}

struct Decoded {
  std::vector<unsigned char> rgba;
  int width = 0;
  int height = 0;
  bool ok = false;
  std::string error;
};

Decoded Decode(const std::string& path) {
  Decoded result;
  std::error_code ec;
  if (!fs::is_regular_file(PathFromUtf8(path), ec) || ec) {
    result.error = "file_unreadable";
    return result;
  }
  int width = 0, height = 0, channels = 0;
  unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
  if (!pixels || width <= 0 || height <= 0) {
    result.error = "decode_failed";
    if (pixels) stbi_image_free(pixels);
    return result;
  }
  const auto bytes = static_cast<std::size_t>(width) * height * 4;
  result.rgba.assign(pixels, pixels + bytes);
  stbi_image_free(pixels);
  result.width = width;
  result.height = height;
  result.ok = true;
  return result;
}

}  // namespace

bool IsPhotoExtension(const std::string& extension) {
  const auto folded = Fold(extension);
  return std::find_if(kExtensions.begin(), kExtensions.end(),
                      [&](const char* item) { return folded == item; }) != kExtensions.end();
}

bool IsPhotoPath(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
  return IsPhotoExtension(path.substr(dot + 1));
}

std::string PhotoRoot() {
  const auto home = Home();
  std::error_code ec;
  const auto pictures = home / "Pictures";
  if (fs::is_directory(pictures, ec) && !ec) return Utf8(pictures);
#ifdef _WIN32
  if (const auto* profile = std::getenv("USERPROFILE")) {
    const auto localized = PathFromUtf8(profile) / std::u8string(u8"图片");
    if (fs::is_directory(localized, ec) && !ec) return Utf8(localized);
  }
#endif
  return Utf8(home);
}

fs::path ResolveRoot(const std::string& path, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const char* reason) {
    if (error) *error = reason;
    return fs::path{};
  };
  if (path.empty() || path == "@photos") return PathFromUtf8(PhotoRoot());
  if (path == "@home") return Home();
  if (path == "@desktop") {
    const auto desktop = Home() / "Desktop";
    std::error_code ec;
    if (fs::is_directory(desktop, ec) && !ec) return desktop;
    return fail("directory_unavailable");
  }
  auto input = PathFromUtf8(path);
  if (!input.is_absolute()) return fail("invalid_path");
  std::error_code ec;
  auto canonical = fs::canonical(input, ec);
  if (ec || !fs::is_directory(canonical, ec)) return fail("directory_unavailable");
  return canonical;
}

ScanResult Scan(const std::string& path, const ScanOptions& options,
                const std::function<bool()>& cancelled) {
  ScanResult result;
  const auto root = ResolveRoot(path, &result.error);
  if (root.empty()) return result;
  result.root = Utf8(root);
  const int max_entries = options.max_entries > 0 ? options.max_entries : kDefaultMaxEntries;
  const int max_depth = options.max_depth >= 0 ? options.max_depth : kDefaultMaxDepth;
  const auto file_now = fs::file_time_type::clock::now();
  const auto system_now = std::chrono::system_clock::now();
  std::set<fs::path> visited;
  std::error_code ec;
  const auto consider = [&](const fs::path& file) {
    if (cancelled && cancelled()) return false;
    if (static_cast<int>(result.entries.size()) >= max_entries) {
      result.truncated = true;
      return false;
    }
    const auto name = Utf8(file.filename());
    if (!IsPhotoPath(name) && !options.include_unsupported) return true;
    std::error_code error;
    const auto status = fs::symlink_status(file, error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return true;
    const auto size = fs::file_size(file, error);
    const auto time = fs::last_write_time(file, error);
    if (error) return true;
    Entry entry;
    entry.path = Utf8(file);
    entry.name = name;
    const auto dot = name.find_last_of('.');
    entry.extension = dot == std::string::npos ? std::string{} : Fold(name.substr(dot + 1));
    entry.size = size;
    entry.modified_ms = ToMillis(time, file_now, system_now);
    entry.date = Format(entry.modified_ms, "%Y-%m-%d");
    const auto folder = Utf8(file.parent_path().filename());
    entry.album = folder.empty() ? Format(entry.modified_ms, "%Y-%m")
                                 : folder + "/" + Format(entry.modified_ms, "%Y-%m");
    result.entries.push_back(std::move(entry));
    return true;
  };
  const auto walk = [&](const fs::path& directory, int depth, auto&& self) -> void {
    if (cancelled && cancelled()) return;
    if (static_cast<int>(result.entries.size()) >= max_entries) {
      result.truncated = true;
      return;
    }
    std::error_code error;
    if (!visited.insert(directory).second) return;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
    if (error) return;
    for (; it != end; it.increment(error)) {
      if (error) break;
      if (cancelled && cancelled()) return;
      const auto& item = it->path();
      const auto name = Utf8(item.filename());
      if (name.empty() || name[0] == '.') continue;
      if (name.rfind(".crossdesk-", 0) == 0) continue;
      std::error_code status_error;
      const auto status = fs::symlink_status(item, status_error);
      if (status_error) continue;
      if (fs::is_symlink(status)) continue;
      if (fs::is_directory(status)) {
        if (options.recursive && depth < max_depth) self(item, depth + 1, self);
        continue;
      }
      if (!fs::is_regular_file(status)) continue;
      if (!consider(item)) return;
    }
  };
  walk(root, 0, walk);
  std::sort(result.entries.begin(), result.entries.end(), [](const Entry& a, const Entry& b) {
    if (a.modified_ms != b.modified_ms) return a.modified_ms > b.modified_ms;
    return Fold(a.name) < Fold(b.name);
  });
  result.ok = !(cancelled && cancelled());
  if (!result.ok) result.error = "cancelled";
  return result;
}

Thumbnail MakeThumbnail(const std::string& path, int max_edge, std::size_t max_bytes) {
  Thumbnail thumb;
  if (max_edge <= 0) max_edge = kTileEdge;
  if (!max_bytes) max_bytes = kMaxThumbnailBytes;
  const auto decoded = Decode(path);
  if (!decoded.ok) {
    thumb.error = decoded.error;
    return thumb;
  }
  thumb.source_width = decoded.width;
  thumb.source_height = decoded.height;
  static constexpr std::array<int, 4> kScales = {4, 3, 2, 1};
  static constexpr std::array<int, 2> kQualities = {78, 55};
  for (const auto scale : kScales) {
    const int edge = std::max(32, max_edge * scale / 4);
    const int fitted = FitEdge(decoded.width, decoded.height, edge);
    if (!fitted) break;
    const double ratio = static_cast<double>(fitted) / std::max(decoded.width, decoded.height);
    int width = std::max(1, static_cast<int>(std::lround(decoded.width * ratio)));
    int height = std::max(1, static_cast<int>(std::lround(decoded.height * ratio)));
    std::vector<unsigned char> scaled;
    if (width == decoded.width && height == decoded.height) scaled = decoded.rgba;
    else scaled = Downscale(decoded.rgba.data(), decoded.width, decoded.height, width, height);
    for (const auto quality : kQualities) {
      std::vector<unsigned char> jpeg;
      JpegBuffer sink{&jpeg};
      const int comp = scaled.empty() ? 0 : 4;
      if (!comp) break;
      if (!stbi_write_jpg_to_func(&WriteJpeg, &sink, width, height, comp, scaled.data(), quality))
        continue;
      if (jpeg.size() > max_bytes) continue;
      thumb.ok = true;
      thumb.jpeg = std::move(jpeg);
      thumb.width = width;
      thumb.height = height;
      return thumb;
    }
  }
  thumb.error = "thumbnail_too_large";
  return thumb;
}

bool DecodeRgba(const unsigned char* data, std::size_t size, std::vector<unsigned char>* rgba,
                int* width, int* height) {
  if (!data || !size || !rgba || !width || !height) return false;
  int channels = 0;
  unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), width, height,
                                               &channels, 4);
  if (!pixels || *width <= 0 || *height <= 0) {
    if (pixels) stbi_image_free(pixels);
    return false;
  }
  const auto bytes = static_cast<std::size_t>(*width) * *height * 4;
  rgba->assign(pixels, pixels + bytes);
  stbi_image_free(pixels);
  return true;
}

std::string Base64Encode(const unsigned char* data, std::size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  std::size_t index = 0;
  for (; index + 2 < size; index += 3) {
    const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
                                (static_cast<std::uint32_t>(data[index + 1]) << 8) |
                                data[index + 2];
    out.push_back(kAlphabet[(value >> 18) & 0x3F]);
    out.push_back(kAlphabet[(value >> 12) & 0x3F]);
    out.push_back(kAlphabet[(value >> 6) & 0x3F]);
    out.push_back(kAlphabet[value & 0x3F]);
  }
  const auto rest = size - index;
  if (rest == 1) {
    const std::uint32_t value = static_cast<std::uint32_t>(data[index]) << 16;
    out.push_back(kAlphabet[(value >> 18) & 0x3F]);
    out.push_back(kAlphabet[(value >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
                                (static_cast<std::uint32_t>(data[index + 1]) << 8);
    out.push_back(kAlphabet[(value >> 18) & 0x3F]);
    out.push_back(kAlphabet[(value >> 12) & 0x3F]);
    out.push_back(kAlphabet[(value >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>* out) {
  if (!out) return false;
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out->clear();
  out->reserve(text.size() / 4 * 3 + 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const auto c : text) {
    if (c == '=') break;
    const int digit = value(c);
    if (digit < 0) {
      if (c == '\n' || c == '\r' || c == ' ') continue;
      return false;
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
    }
  }
  return true;
}

std::string AlbumForDate(std::int64_t modified_ms) {
  return Format(modified_ms > 0 ? modified_ms
                                : std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count(),
                "%Y-%m");
}

bool ClassifyReceivedPhoto(const fs::path& file, const fs::path& base, bool group_by_date,
                           fs::path* moved_to) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec) || ec) return false;
  const auto name = file.filename();
  if (!IsPhotoPath(Utf8(name))) return false;
  fs::path target = base / "Photos";
  if (group_by_date) {
    const auto time = fs::last_write_time(file, ec);
    std::int64_t stamp = 0;
    if (!ec) {
      stamp = ToMillis(time, fs::file_time_type::clock::now(), std::chrono::system_clock::now());
    }
    target /= AlbumForDate(stamp);
  }
  fs::create_directories(target, ec);
  if (ec) return false;
  auto destination = target / name;
  if (fs::exists(destination, ec) && !ec) {
    const auto stem = Utf8(name.stem());
    const auto extension = Utf8(name.extension());
    for (int attempt = 2; attempt < 1000; ++attempt) {
      destination = target / PathFromUtf8(stem + " (" + std::to_string(attempt) + ")" + extension);
      if (!fs::exists(destination, ec) || ec) break;
    }
  }
  fs::rename(file, destination, ec);
  if (ec) {
    // Cross device: copy then remove.
    ec.clear();
    fs::copy_file(file, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    std::error_code remove_error;
    fs::remove(file, remove_error);
  }
  if (moved_to) *moved_to = destination;
  return true;
}

}  // namespace photo
}  // namespace crossdesk
