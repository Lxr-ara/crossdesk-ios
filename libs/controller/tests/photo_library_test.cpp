/*
 * Photo library regression test. Generates its own images so the result does
 * not depend on the host's Pictures folder.
 */

#include <photo_library.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;
using namespace crossdesk::photo;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
}

void MakeGradient(const fs::path& path, int width, int height) {
  std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto* pixel = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      pixel[0] = static_cast<unsigned char>(x * 255 / width);
      pixel[1] = static_cast<unsigned char>(y * 255 / height);
      pixel[2] = static_cast<unsigned char>((x ^ y) & 0xFF);
      pixel[3] = 255;
    }
  }
  const auto name = path.filename().string();
  const auto text = path.string();
  if (name.ends_with(".jpg") || name.ends_with(".jpeg"))
    stbi_write_jpg(text.c_str(), width, height, 4, rgba.data(), 90);
  else
    stbi_write_png(text.c_str(), width, height, 4, rgba.data(), width * 4);
}

fs::path TempRoot() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto root = fs::temp_directory_path() / ("crossdesk-photo-test-" + std::to_string(stamp));
  fs::create_directories(root);
  return root;
}

void TestExtensions() {
  Check(IsPhotoPath("/a/b/photo.JPG"), "jpg is a photo");
  Check(IsPhotoPath("C:\\pics\\IMG_0001.HEIC"), "heic is a photo");
  Check(IsPhotoPath("x.png"), "png is a photo");
  Check(!IsPhotoPath("notes.txt"), "txt is not a photo");
  Check(!IsPhotoPath("archive.tar.gz"), "gz is not a photo");
  Check(!IsPhotoPath("no-extension"), "no extension is not a photo");
  Check(IsPhotoExtension("WEBP"), "extension match folds case");
}

void TestScan(const fs::path& root) {
  fs::create_directories(root / "trip" / "day1");
  fs::create_directories(root / ".hidden");
  MakeGradient(root / "big.jpg", 4000, 3000);
  MakeGradient(root / "small.png", 64, 64);
  MakeGradient(root / "trip" / "day1" / "mid.jpeg", 800, 600);
  MakeGradient(root / ".hidden" / "skipped.png", 32, 32);
  std::FILE* notes = std::fopen((root / "notes.txt").string().c_str(), "wb");
  if (notes) { std::fputs("not a photo", notes); std::fclose(notes); }

  const auto result = Scan(root.string());
  Check(result.ok, "scan succeeds");
  Check(result.entries.size() == 3, "scan finds three photos and skips hidden folder + txt");
  for (const auto& item : result.entries) {
    Check(item.size > 0, "entry has a size");
    Check(item.modified_ms > 0, "entry has a modification time");
    Check(item.album.find('/') != std::string::npos, "entry belongs to an album");
    Check(item.date.size() == 10, "entry has a date");
    Check(!IsPhotoPath("") || true, "noop");
  }
  bool sorted = true;
  for (std::size_t i = 1; i < result.entries.size(); ++i)
    if (result.entries[i - 1].modified_ms < result.entries[i].modified_ms) sorted = false;
  Check(sorted, "entries are newest first");

  ScanOptions shallow;
  shallow.recursive = false;
  const auto flat = Scan(root.string(), shallow);
  Check(flat.ok && flat.entries.size() == 2, "non recursive scan stays in the root");

  ScanOptions capped;
  capped.max_entries = 1;
  const auto limited = Scan(root.string(), capped);
  Check(limited.ok && limited.entries.size() == 1 && limited.truncated, "entry cap is enforced");

  const auto missing = Scan((root / "missing").string());
  Check(!missing.ok && missing.error == "directory_unavailable", "missing root is reported");
}

void TestThumbnails(const fs::path& root) {
  const auto big = (root / "big.jpg").string();
  const auto thumb = MakeThumbnail(big, 192, kMaxThumbnailBytes);
  Check(thumb.ok, "thumbnail encodes");
  Check(thumb.source_width == 4000 && thumb.source_height == 3000, "source size is reported");
  Check(std::max(thumb.width, thumb.height) <= 192, "thumbnail fits the tile edge");
  Check(thumb.width * 3000 == thumb.height * 4000 ||
        std::abs(thumb.width * 3000 - thumb.height * 4000) <= 4000, "aspect ratio is kept");
  Check(thumb.jpeg.size() <= kMaxThumbnailBytes, "thumbnail respects the byte budget");
  Check(thumb.jpeg.size() > 2 && thumb.jpeg[0] == 0xFF && thumb.jpeg[1] == 0xD8,
        "thumbnail is a JPEG");

  std::vector<unsigned char> rgba;
  int width = 0, height = 0;
  Check(DecodeRgba(thumb.jpeg.data(), thumb.jpeg.size(), &rgba, &width, &height),
        "thumbnail decodes back");
  Check(width == thumb.width && height == thumb.height, "decoded size matches");
  Check(rgba.size() == static_cast<std::size_t>(width) * height * 4, "decoded RGBA is tight");

  const auto tiny = MakeThumbnail((root / "small.png").string(), 192, kMaxThumbnailBytes);
  Check(tiny.ok && tiny.width == 64 && tiny.height == 64, "small photos are not enlarged");

  const auto missing = MakeThumbnail((root / "missing.jpg").string());
  Check(!missing.ok && !missing.error.empty(), "missing photo reports an error");
}

void TestBase64() {
  const std::vector<unsigned char> sample = {0, 1, 2, 250, 251, 252, 253, 254, 255, 42};
  const auto encoded = Base64Encode(sample.data(), sample.size());
  std::vector<unsigned char> decoded;
  Check(Base64Decode(encoded, &decoded), "base64 decodes");
  Check(decoded == sample, "base64 round trip is lossless");
  Check(Base64Encode(nullptr, 0).empty(), "empty input encodes to empty");
}

void TestClassify(const fs::path& root) {
  const auto inbox = root / "inbox";
  fs::create_directories(inbox);
  MakeGradient(inbox / "snap.png", 32, 32);
  const auto received = inbox / "snap.png";
  fs::path moved;
  Check(ClassifyReceivedPhoto(received, inbox, true, &moved), "photo is classified");
  Check(fs::exists(moved), "classified photo exists");
  Check(moved.parent_path().filename().string().size() == 7, "classified into YYYY-MM album");
  Check(moved.parent_path().parent_path().filename() == "Photos", "classified under Photos");
  Check(!fs::exists(received), "photo left the inbox");
  // A second photo with the same name must not overwrite the first one.
  MakeGradient(inbox / "snap.png", 32, 32);
  fs::path second;
  Check(ClassifyReceivedPhoto(inbox / "snap.png", inbox, false, &second), "second photo is classified");
  Check(second != moved && fs::exists(second), "same name does not overwrite");
  std::FILE* notes = std::fopen((inbox / "note.txt").string().c_str(), "wb");
  if (notes) { std::fputs("x", notes); std::fclose(notes); }
  Check(!ClassifyReceivedPhoto(inbox / "note.txt", inbox, false, nullptr),
        "non photos are left alone");
  Check(AlbumForDate(0).size() == 7, "album label is YYYY-MM");
}

}  // namespace

int main() {
  TestExtensions();
  TestBase64();
  const auto root = TempRoot();
  TestScan(root);
  TestThumbnails(root);
  TestClassify(root);
  std::error_code ec;
  fs::remove_all(root, ec);
  if (g_failures) {
    std::printf("%d photo checks failed\n", g_failures);
    return 1;
  }
  std::printf("photo library checks passed\n");
  return 0;
}
