/*
 * Cross-session resume sidecar regression test.
 *
 * The resume metadata layer is a plain filesystem contract: a sidecar next to
 * the partial file, an atomic replace, and a freshness check that turns a
 * changed target into "stale" instead of a silent corrupt resume. Every case
 * here runs on a throwaway directory tree, so no transfer session is needed.
 */

#include <file_transfer_resume.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::file_transfer::ClearResumeRecord;
using crossdesk::file_transfer::IsResumeSidecarFilename;
using crossdesk::file_transfer::kResumeSidecarSuffix;
using crossdesk::file_transfer::ListResumeRecords;
using crossdesk::file_transfer::ReadResumeRecord;
using crossdesk::file_transfer::ResumeRecord;
using crossdesk::file_transfer::ResumeSidecarName;
using crossdesk::file_transfer::TargetMtimeMs;
using crossdesk::file_transfer::WriteResumeRecord;

namespace {

int g_failures = 0;
fs::path g_root;

void Expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
}

fs::path FreshDir(const char* name) {
  const auto dir = g_root / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

void MakeTarget(const fs::path& path, std::uintmax_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const std::string block(64 * 1024, 'R');
  for (std::uintmax_t written = 0; written < size;) {
    const auto step = (std::min)(static_cast<std::uintmax_t>(block.size()), size - written);
    output.write(block.data(), static_cast<std::streamsize>(step));
    written += step;
  }
  output.close();
}

std::string ReadText(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

ResumeRecord RecordFor(const fs::path& directory, const std::string& target_path,
                       uint64_t total, uint64_t received, uint32_t wire_id) {
  ResumeRecord record;
  record.target_path = target_path;
  record.total_size = total;
  record.target_mtime_ms = TargetMtimeMs(directory / target_path);
  record.received = received;
  record.wire_id = wire_id;
  return record;
}

// Mirrors the engine's staging token: ".crossdesk-<file_id>-<tick>-<serial>"
// (file_transfer_core.h:349-356). A sidecar name must never match this shape,
// because the engine deletes staging directories wholesale.
bool LooksLikeStagingToken(std::string_view name) {
  constexpr std::string_view kPrefix = ".crossdesk-";
  if (name.size() <= kPrefix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0) return false;
  std::size_t groups = 0;
  std::size_t index = kPrefix.size();
  while (index <= name.size()) {
    const auto separator = name.find('-', index);
    const auto stop = separator == std::string_view::npos ? name.size() : separator;
    if (stop == index) return false;
    for (auto cursor = index; cursor < stop; ++cursor)
      if (name[cursor] < '0' || name[cursor] > '9') return false;
    ++groups;
    if (separator == std::string_view::npos) break;
    index = separator + 1;
  }
  return groups == 3;
}

// Round trip: what is written is what a later session reads, and the write is
// atomic (no temporary survives) with a versioned JSON body.
void TestRoundTrip() {
  const auto dir = FreshDir("round-trip");
  MakeTarget(dir / "video.mp4", 4096);
  const auto record = RecordFor(dir, "video.mp4", 4096, 1024, 42);
  Expect(record.target_mtime_ms != 0, "the target stamp is readable");

  std::string error = "dirty";
  Expect(WriteResumeRecord(dir, record, &error), "a resume record is written");
  Expect(error.empty(), "a successful write clears the error");
  Expect(fs::is_regular_file(dir / "video.mp4.crossdesk-resume"),
         "the sidecar is named <file>.crossdesk-resume next to the partial file");
  Expect(!fs::exists(dir / "video.mp4.crossdesk-resume.tmp"),
         "no temporary survives an atomic replace");

  const auto body = ReadText(dir / "video.mp4.crossdesk-resume");
  Expect(body.find("\"version\":1") != std::string::npos, "the sidecar carries version 1");
  Expect(body.find("\"received\":1024") != std::string::npos, "the sidecar carries received bytes");
  Expect(body.find("\"wire_id\":42") != std::string::npos, "the sidecar carries the wire id");

  const auto restored = ReadResumeRecord(dir, "video.mp4", &error);
  Expect(restored.has_value(), "the record reads back");
  Expect(error.empty(), "a fresh record reports no error");
  if (restored) {
    Expect(restored->target_path == "video.mp4", "round trip keeps target_path");
    Expect(restored->total_size == 4096, "round trip keeps total_size");
    Expect(restored->received == 1024, "round trip keeps received");
    Expect(restored->wire_id == 42, "round trip keeps wire_id");
    Expect(restored->target_mtime_ms == record.target_mtime_ms, "round trip keeps the stamp");
  }

  // A nested target stays inside the record directory and is still addressable
  // by the sidecar of its own file name.
  const auto nested = FreshDir("round-trip-nested");
  MakeTarget(nested / "clips" / "clip.mov", 512);
  const auto deep = RecordFor(nested, "clips/clip.mov", 512, 256, 7);
  Expect(WriteResumeRecord(nested, deep, &error), "a nested target is accepted");
  Expect(fs::is_regular_file(nested / "clip.mov.crossdesk-resume"),
         "the sidecar sits in the record directory, not in the target directory");
  const auto deep_read = ReadResumeRecord(nested, "clip.mov", &error);
  Expect(deep_read && deep_read->target_path == "clips/clip.mov",
         "a nested target survives the round trip");
}

// A record whose size or stamp no longer matches the target is stale: the
// caller restarts from 0 rather than appending to the wrong bytes.
void TestStale() {
  const auto dir = FreshDir("stale");
  MakeTarget(dir / "notes.txt", 100);
  const auto record = RecordFor(dir, "notes.txt", 100, 40, 7);
  std::string error;
  Expect(WriteResumeRecord(dir, record, &error), "the stale fixture is written");
  Expect(ReadResumeRecord(dir, "notes.txt", &error).has_value(), "the fixture is fresh first");

  MakeTarget(dir / "notes.txt", 101);
  const auto grown = ReadResumeRecord(dir, "notes.txt", &error);
  Expect(!grown.has_value(), "a resized target invalidates the record");
  Expect(error == "stale", "a resized target reports stale");

  // A stale record is still listed: a cleanup sweep has to find it.
  const auto listed = ListResumeRecords(dir, &error);
  Expect(listed.size() == 1, "a stale record is still enumerable");
  Expect(listed.empty() || listed.front().received == 40, "listing keeps the recorded offset");

  std::error_code ec;
  fs::remove(dir / "notes.txt", ec);
  const auto gone = ReadResumeRecord(dir, "notes.txt", &error);
  Expect(!gone.has_value(), "a missing target cannot be resumed");
  Expect(error == "missing", "a missing target reports missing");
  Expect(ListResumeRecords(dir, &error).size() == 1, "an orphaned record is still enumerable");
}

// Malformed sidecars are rejected by Read and skipped by List, never thrown.
void TestMalformed() {
  const auto dir = FreshDir("malformed");
  WriteText(dir / "garbage.bin.crossdesk-resume", "{not json");
  std::string error;
  const auto broken = ReadResumeRecord(dir, "garbage.bin", &error);
  Expect(!broken.has_value(), "invalid JSON is not a record");
  Expect(error == "malformed", "invalid JSON reports malformed");

  WriteText(dir / "huge.bin.crossdesk-resume", std::string(9000, 'x'));
  Expect(!ReadResumeRecord(dir, "huge.bin", &error).has_value(), "an oversized sidecar is rejected");
  Expect(error == "malformed", "an oversized sidecar reports malformed");

  WriteText(dir / "future.bin.crossdesk-resume",
            R"({"version":2,"target_path":"future.bin","total_size":10,)"
            R"("target_mtime_ms":0,"received":1,"wire_id":9})");
  Expect(!ReadResumeRecord(dir, "future.bin", &error).has_value(),
         "a newer sidecar version is not trusted");
  Expect(error == "unsupported_version", "an unknown version reports unsupported_version");

  WriteText(dir / "partial.bin.crossdesk-resume",
            R"({"version":1,"target_path":"partial.bin","total_size":10,)"
            R"("target_mtime_ms":0,"received":1})");
  Expect(!ReadResumeRecord(dir, "partial.bin", &error).has_value(),
         "a record without a wire id is rejected");
  Expect(error == "malformed", "a missing field reports malformed");

  WriteText(dir / "negative.bin.crossdesk-resume",
            R"({"version":1,"target_path":"negative.bin","total_size":10,)"
            R"("target_mtime_ms":0,"received":-1,"wire_id":9})");
  Expect(!ReadResumeRecord(dir, "negative.bin", &error).has_value(),
         "a negative offset is rejected");
  Expect(error == "malformed", "a negative offset reports malformed");

  WriteText(dir / "over.bin.crossdesk-resume",
            R"({"version":1,"target_path":"over.bin","total_size":10,)"
            R"("target_mtime_ms":0,"received":11,"wire_id":9})");
  Expect(!ReadResumeRecord(dir, "over.bin", &error).has_value(),
         "received beyond total is rejected");
  Expect(error == "malformed", "received beyond total reports malformed");

  Expect(ListResumeRecords(dir, &error).empty(), "listing skips every malformed sidecar");
  Expect(error.empty(), "a skipped file is not a listing error");
}

// Escapes are rejected both when writing and when reading, so a record can
// never address a file outside its own directory.
void TestPathEscape() {
  const auto dir = FreshDir("escape");
  std::string error;
  const auto write_escape = [&](const std::string& target_path) {
    ResumeRecord record;
    record.target_path = target_path;
    record.total_size = 16;
    record.received = 8;
    record.wire_id = 3;
    record.target_mtime_ms = 1234;
    error = "dirty";
    return WriteResumeRecord(dir, record, &error);
  };

  Expect(!write_escape("../escape.bin"), "a parent escape is refused");
  Expect(error == "invalid_target", "a parent escape reports invalid_target");
  Expect(!write_escape("/absolute/escape.bin"), "an absolute target is refused");
  Expect(error == "invalid_target", "an absolute target reports invalid_target");
  Expect(!write_escape("sub/../../up.bin"), "a nested parent escape is refused");
  Expect(!write_escape("sub\\..\\up.bin"), "a backslash escape is refused");
  Expect(!write_escape(".crossdesk-17-12345-3"), "the engine staging prefix is reserved");
  Expect(!write_escape(std::string(300, 'a')), "an over long name is refused");
  Expect(!ReadResumeRecord(dir, std::string(300, 'a'), &error).has_value(),
         "an over long name cannot be read");
  Expect(error == "invalid_name", "an over long name reports invalid_name");

  ResumeRecord over;
  over.target_path = "ok.bin";
  over.total_size = 8;
  over.received = 9;
  over.wire_id = 1;
  Expect(!WriteResumeRecord(dir, over, &error), "received beyond total is refused");
  Expect(error == "invalid_record", "received beyond total reports invalid_record");

  Expect(ListResumeRecords(dir, &error).empty(), "no refused record leaves a sidecar behind");

  Expect(!ReadResumeRecord(dir, "../escape.bin", &error).has_value(),
         "a parent escape cannot be read");
  Expect(error == "invalid_name", "a parent escape reports invalid_name");
  Expect(!ReadResumeRecord(dir, "sub/escape.bin", &error).has_value(),
         "a separator in the name cannot be read");
  Expect(!ReadResumeRecord(dir, ".crossdesk-17-12345-3", &error).has_value(),
         "a staging token cannot be read as a record");
  Expect(!ClearResumeRecord(dir, "../escape.bin"), "a parent escape cannot be cleared");

  // A record pointing outside its directory is skipped even if it parses.
  WriteText(dir / "evil.bin.crossdesk-resume",
            R"({"version":1,"target_path":"../evil.bin","total_size":10,)"
            R"("target_mtime_ms":0,"received":3,"wire_id":7})");
  Expect(!ReadResumeRecord(dir, "evil.bin", &error).has_value(),
         "an escaping target_path is rejected on read");
  Expect(error == "invalid_target", "an escaping target reports invalid_target");
  Expect(ListResumeRecords(dir, &error).empty(), "listing skips an escaping record");

  // A record whose target_path names a different file than the sidecar does is
  // ambiguous and therefore malformed.
  WriteText(dir / "mismatch.bin.crossdesk-resume",
            R"({"version":1,"target_path":"other.bin","total_size":10,)"
            R"("target_mtime_ms":0,"received":3,"wire_id":7})");
  Expect(!ReadResumeRecord(dir, "mismatch.bin", &error).has_value(),
         "a sidecar describing another file is rejected");
  Expect(error == "malformed", "a mismatched target_path reports malformed");
}

// Clearing is idempotent and never touches neighbour files.
void TestIdempotentClear() {
  const auto dir = FreshDir("clear");
  MakeTarget(dir / "keep.bin", 64);
  MakeTarget(dir / "drop.bin", 64);
  std::string error;
  Expect(WriteResumeRecord(dir, RecordFor(dir, "keep.bin", 64, 8, 1), &error),
         "the neighbour record is written");
  Expect(WriteResumeRecord(dir, RecordFor(dir, "drop.bin", 64, 8, 2), &error),
         "the cleared record is written");

  Expect(ClearResumeRecord(dir, "drop.bin"), "clearing a record succeeds");
  Expect(!fs::exists(dir / "drop.bin.crossdesk-resume"), "clearing removes the sidecar");
  Expect(ClearResumeRecord(dir, "drop.bin"), "clearing twice stays a success");
  Expect(ClearResumeRecord(dir, "never-written.bin"), "clearing an absent record succeeds");

  const auto listed = ListResumeRecords(dir, &error);
  Expect(listed.size() == 1, "clearing one record leaves the neighbour alone");
  Expect(listed.empty() || listed.front().target_path == "keep.bin",
         "the surviving record is the neighbour");
  Expect(fs::is_regular_file(dir / "keep.bin"), "clearing a sidecar keeps the target file");
  Expect(!ReadResumeRecord(dir, "drop.bin", &error).has_value(), "a cleared record is gone");
  Expect(error == "not_found", "a cleared record reports not_found");
}

// Listing survives one bad file among good ones and ignores foreign files.
void TestListing() {
  const auto dir = FreshDir("listing");
  for (const char* name : {"a.bin", "b.bin", "c.bin"}) {
    MakeTarget(dir / name, 128);
    std::string error;
    Expect(WriteResumeRecord(dir, RecordFor(dir, name, 128, 64, 11), &error),
           "a good record is written for listing");
  }
  WriteText(dir / "bad.bin.crossdesk-resume", "{not json");
  WriteText(dir / "escape.bin.crossdesk-resume",
            R"({"version":1,"target_path":"../../etc/passwd","total_size":10,)"
            R"("target_mtime_ms":0,"received":3,"wire_id":7})");
  WriteText(dir / "leftover.bin.crossdesk-resume.tmp", "{}");
  WriteText(dir / "notes.txt", "not a sidecar");

  std::string error = "dirty";
  const auto listed = ListResumeRecords(dir, &error);
  Expect(error.empty(), "listing a healthy directory reports no error");
  Expect(listed.size() == 3, "listing returns the good records only");
  std::vector<std::string> names;
  for (const auto& record : listed) names.push_back(record.target_path);
  std::sort(names.begin(), names.end());
  Expect(names == std::vector<std::string>({"a.bin", "b.bin", "c.bin"}),
         "listing keeps the surviving target paths");
  for (const auto& record : listed) {
    Expect(record.received == 64 && record.total_size == 128, "listing keeps the offsets");
    Expect(record.wire_id == 11, "listing keeps the wire ids");
  }
  Expect(ListResumeRecords(dir, &error).size() == 3, "listing is repeatable");

  const auto absent = ListResumeRecords(g_root / "does-not-exist", &error);
  Expect(absent.empty(), "listing a missing directory returns nothing");
  Expect(error == "directory_unavailable", "listing a missing directory reports the reason");
}

// The sidecar name can never collide with the engine's staging directories.
void TestNaming() {
  const auto sidecar = ResumeSidecarName("report.pdf");
  Expect(sidecar == "report.pdf.crossdesk-resume", "the sidecar suffix is appended");
  Expect(IsResumeSidecarFilename(sidecar), "a sidecar name is recognised");
  Expect(!IsResumeSidecarFilename(".crossdesk-17-12345-3"),
         "a staging token is not a sidecar");
  Expect(!IsResumeSidecarFilename("report.pdf"), "a plain file is not a sidecar");
  Expect(ResumeSidecarName("").empty(), "an empty name has no sidecar");
  Expect(ResumeSidecarName("../x").empty(), "an escaping name has no sidecar");

  Expect(LooksLikeStagingToken(".crossdesk-17-1234567-1"),
         "the engine staging shape is recognised by the fixture");
  Expect(!LooksLikeStagingToken(sidecar), "a sidecar never matches the staging shape");
  Expect(!LooksLikeStagingToken(ResumeSidecarName(".partial")),
         "the sidecar of the engine's .partial never matches the staging shape");
  Expect(!LooksLikeStagingToken(".crossdesk-resume"),
         "the bare suffix never matches the staging shape");
  Expect(sidecar.rfind(kResumeSidecarSuffix) == sidecar.size() - kResumeSidecarSuffix.size(),
         "the suffix is only ever a suffix");
}

}  // namespace

int main() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / ("crossdesk-transfer-resume-test-" + std::to_string(stamp));
  fs::create_directories(g_root, ec);
  if (ec) {
    std::printf("file transfer resume checks failed: %s\n", ec.message().c_str());
    return 1;
  }

  TestRoundTrip();
  TestStale();
  TestMalformed();
  TestPathEscape();
  TestIdempotentClear();
  TestListing();
  TestNaming();

  fs::remove_all(g_root, ec);
  if (g_failures) {
    std::printf("%d file transfer resume checks failed\n", g_failures);
    return 1;
  }
  std::printf("file transfer resume checks passed\n");
  return 0;
}
