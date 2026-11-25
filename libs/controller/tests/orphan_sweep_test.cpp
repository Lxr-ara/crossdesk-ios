/*
 * Orphan sweep regression test.
 *
 * The receive adapter leaves two kinds of debris in the receive folder when a
 * transfer dies (crash, kill, peer gone):
 *   * the engine's staging directory ".crossdesk-<id>-<tick>-<serial>";
 *   * the resume sidecar "<name>.crossdesk-resume" (plus its ".tmp" write
 *     target) written by the cross-session resume path.
 * SweepOrphans() is the only thing that reclaims them, so every rule that keeps
 * a live transfer alive has to be pinned here: this file drives the real
 * desktop adapter (apps/desktop/src/tools/file_transfer.cpp) on a throwaway
 * directory tree and checks both the decisions and the report arithmetic.
 */

#include "file_transfer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::kOrphanMinAge;
using crossdesk::kOrphanSweepHoursEnv;
using crossdesk::kOrphanSweepInterval;
using crossdesk::ResolveOrphanSweepSchedule;
using crossdesk::SweepOrphans;
using crossdesk::SweepReport;

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

// 24 hours before "now" in the file clock, so every case can place an entry on
// either side of the min-age line without sleeping.
fs::file_time_type HoursAgo(double hours) {
  return fs::file_time_type::clock::now() -
         std::chrono::duration_cast<fs::file_time_type::duration>(
             std::chrono::duration<double>(hours * 3600.0));
}

void MakeFile(const fs::path& path, std::uintmax_t size, double age_hours) {
  fs::create_directories(path.parent_path());
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::string block(4096, 'O');
    for (std::uintmax_t written = 0; written < size;) {
      const auto step = (std::min)(static_cast<std::uintmax_t>(block.size()), size - written);
      output.write(block.data(), static_cast<std::streamsize>(step));
      written += step;
    }
  }
  std::error_code ec;
  fs::last_write_time(path, HoursAgo(age_hours), ec);
}

void Touch(const fs::path& path, double age_hours) {
  std::error_code ec;
  fs::last_write_time(path, HoursAgo(age_hours), ec);
}

void WriteSidecar(const fs::path& dir, const std::string& name, double age_hours) {
  // A sidecar is only ever a marker for this test: the sweep reads the file
  // name, not the JSON, because a corrupt record is debris too.
  const auto path = dir / (name + std::string(crossdesk::file_transfer::kResumeSidecarSuffix));
  MakeFile(path, 48, age_hours);
}

bool Exists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void ClearEnv(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

// The production cadence is the contract: an hour is at most hourly, and the
// sweep only touches what is older than 48 hours.
void TestScheduleDefaults() {
  ClearEnv(std::string(kOrphanSweepHoursEnv).c_str());
  const auto schedule = ResolveOrphanSweepSchedule();
  Expect(schedule.enabled, "sweep is enabled by default");
  Expect(schedule.interval == std::chrono::duration_cast<std::chrono::seconds>(kOrphanSweepInterval),
         "default sweep interval is kOrphanSweepInterval (1h)");
  Expect(schedule.min_age == std::chrono::duration_cast<std::chrono::seconds>(kOrphanMinAge),
         "default min age is kOrphanMinAge (48h)");
}

// CROSSDESK_ORPHAN_SWEEP_HOURS exists so a sweep can be observed without
// waiting an hour: any positive value replaces the interval, 0 disables it, and
// garbage keeps the default.
void TestScheduleOverride() {
  const auto env = std::string(kOrphanSweepHoursEnv);
  SetEnv(env.c_str(), "0");
  auto schedule = ResolveOrphanSweepSchedule();
  Expect(!schedule.enabled, "CROSSDESK_ORPHAN_SWEEP_HOURS=0 disables the sweep");
  // 关掉间隔不等于放宽最小年龄：判定线仍然是 48h。
  Expect(schedule.min_age == std::chrono::duration_cast<std::chrono::seconds>(kOrphanMinAge),
         "disabling the sweep keeps the 48h min age");

  SetEnv(env.c_str(), "0.0001");
  schedule = ResolveOrphanSweepSchedule();
  Expect(schedule.enabled, "a fractional override keeps the sweep enabled");
  Expect(schedule.interval < std::chrono::seconds(2),
         "a fractional override shortens the interval to about one second");

  SetEnv(env.c_str(), "2");
  schedule = ResolveOrphanSweepSchedule();
  Expect(schedule.interval == std::chrono::hours(2), "a whole-hour override is honoured");

  SetEnv(env.c_str(), "nonsense");
  schedule = ResolveOrphanSweepSchedule();
  Expect(schedule.enabled &&
             schedule.interval ==
                 std::chrono::duration_cast<std::chrono::seconds>(kOrphanSweepInterval),
         "an unparsable override keeps the default interval");

  ClearEnv(env.c_str());
}

// A transfer that is still running refreshes its ".partial" inside the staging
// directory, and the directory's own mtime never moves. Both the fresh
// directory and the old-directory-with-fresh-partial shapes must survive.
void TestLiveStagingKept() {
  const auto dir = FreshDir("live");
  const auto fresh = dir / ".crossdesk-7-100-1";
  MakeFile(fresh / ".partial", 8192, 0.0);
  // 目录本身也设成「刚建的」，这是一次正常的传输。
  Touch(fresh, 0.0);

  const auto long_running = dir / ".crossdesk-8-101-2";
  MakeFile(long_running / ".partial", 4096, 0.0);
  // 目录创建于两天多以前，但 .partial 刚刚还在被追加。
  Touch(long_running, 49.0);

  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(Exists(fresh), "a fresh staging directory is kept");
  Expect(Exists(long_running / ".partial"),
         "a staging directory whose partial is still being written is kept");
  Expect(report.staging_removed == 0, "live staging is not counted as removed");
  Expect(report.skipped_live >= 2, "live staging is counted as skipped_live");
  Expect(report.bytes_removed == 0, "keeping everything removes no bytes");
}

// The crash shape: a staging directory nobody will ever come back for.
void TestOldStagingRemoved() {
  const auto dir = FreshDir("stale");
  const auto staging = dir / ".crossdesk-9-102-3";
  MakeFile(staging / ".partial", 2048, 49.0);
  Touch(staging, 49.0);
  // 与暂存无关的普通文件/目录一个都不能动：用户收到的文件就在这里。
  MakeFile(dir / "kept.txt", 128, 49.0);
  fs::create_directories(dir / "Notes");
  MakeFile(dir / "Notes" / "old.txt", 64, 49.0);

  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(!Exists(staging), "a staging directory older than min age is removed");
  Expect(report.staging_removed == 1, "one staging directory is reported");
  Expect(report.bytes_removed == 2048, "the staging bytes are reported");
  Expect(Exists(dir / "kept.txt"), "an ordinary file is never touched");
  Expect(Exists(dir / "Notes" / "old.txt"), "an ordinary directory is never touched");
}

// A sidecar whose target is gone describes bytes that no longer exist: it can
// never be resumed, so it is debris no matter how new it looks.
void TestSidecarWithoutTargetRemoved() {
  const auto dir = FreshDir("orphan-sidecar");
  WriteSidecar(dir, "report.txt", 0.0);

  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(!Exists(dir / "report.txt.crossdesk-resume"),
         "a sidecar whose target is missing is removed even when it is fresh");
  Expect(report.sidecars_removed == 1, "one sidecar is reported");
  Expect(report.bytes_removed == 48, "the sidecar bytes are reported");
  Expect(report.skipped_live == 0, "nothing is reported as live");
}

// The prefix file is the resume proof: as long as it exists and was touched
// inside min age, the record still describes a transfer that can continue.
void TestSidecarWithLiveTargetKept() {
  const auto dir = FreshDir("resumable");
  MakeFile(dir / "movie.mkv", 4096, 0.0);
  WriteSidecar(dir, "movie.mkv", 0.0);

  // 侧车两天没动，但前缀字节刚刚还在被刷新：这条记录仍然能续传。
  const auto dir2 = FreshDir("resumable-old-record");
  MakeFile(dir2 / "archive.zip", 4096, 0.0);
  WriteSidecar(dir2, "archive.zip", 49.0);

  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(Exists(dir / "movie.mkv.crossdesk-resume"), "a fresh sidecar is kept");
  Expect(Exists(dir / "movie.mkv"), "the prefix file is kept");
  SweepReport second;
  SweepOrphans(dir2, kOrphanMinAge, &second);
  Expect(Exists(dir2 / "archive.zip.crossdesk-resume"),
         "an old sidecar whose target is still fresh is kept");
  Expect(report.sidecars_removed + second.sidecars_removed == 0,
         "no resumable record is counted as removed");
  Expect(report.skipped_live + second.skipped_live >= 2,
         "both resumable records are counted as skipped_live");
}

// Nothing has touched either half since well before min age: the transfer is
// dead, the sidecar goes. The prefix file itself stays - those are the user's
// bytes, not metadata.
void TestStaleSidecarRemovedTargetKept() {
  const auto dir = FreshDir("stale-sidecar");
  MakeFile(dir / "report.txt", 1024, 49.0);
  WriteSidecar(dir, "report.txt", 49.0);
  // 半写成的 .tmp 永远不会是有效记录，够旧就直接清。
  MakeFile(dir / "draft.txt.crossdesk-resume.tmp", 32, 49.0);

  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(!Exists(dir / "report.txt.crossdesk-resume"),
         "a sidecar older than min age with an equally old target is removed");
  Expect(!Exists(dir / "draft.txt.crossdesk-resume.tmp"),
         "an abandoned .tmp sidecar is removed");
  Expect(Exists(dir / "report.txt"),
         "the target file itself is never deleted by the sweep");
  Expect(report.sidecars_removed == 2, "both sidecars are reported");
  Expect(report.bytes_removed == 48 + 32, "both sidecar sizes are reported");
}

// Everything sweeps together in one pass; the counts and bytes have to add up
// to exactly what was reclaimed, and a fresh temporary must not be disturbed.
void TestReportArithmetic() {
  const auto dir = FreshDir("arithmetic");
  const auto first = dir / ".crossdesk-1-1-1";
  MakeFile(first / ".partial", 1000, 50.0);
  Touch(first, 50.0);
  const auto second = dir / ".crossdesk-2-2-2";
  MakeFile(second / ".partial", 2000, 50.0);
  Touch(second, 50.0);
  // 新增一个还没写过的空暂存目录：它的 mtime 就是创建时间，也够旧。
  fs::create_directories(dir / ".crossdesk-3-3-3");
  Touch(dir / ".crossdesk-3-3-3", 50.0);

  WriteSidecar(dir, "gone.bin", 0.0);        // 目标不存在：删
  MakeFile(dir / "old.bin", 16, 50.0);
  WriteSidecar(dir, "old.bin", 50.0);        // 两边都旧：删
  MakeFile(dir / "live.bin", 16, 0.0);
  WriteSidecar(dir, "live.bin", 0.0);        // 还在续传：留

  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(report.staging_removed == 3, "three abandoned staging entries are reported");
  Expect(report.sidecars_removed == 2, "two dead sidecars are reported");
  Expect(report.skipped_live == 1, "the resumable pair is counted as skipped_live");
  Expect(report.bytes_removed == 3000 + 48 + 48,
         "bytes_removed is the staging bytes plus the removed sidecar sizes");
  Expect(Exists(dir / "live.bin.crossdesk-resume"), "the resumable sidecar survives");
  Expect(!Exists(first), "the first staging directory is gone");
  Expect(!Exists(second), "the second staging directory is gone");
  Expect(!Exists(dir / ".crossdesk-3-3-3"), "the empty abandoned staging directory is gone");
}

// A sweep with nothing to do (or a directory that is not there) is a no-op, not
// an error: the manager runs it from the UI timer.
void TestEmptyAndMissingDirectories() {
  const auto dir = FreshDir("empty");
  SweepReport report;
  SweepOrphans(dir, kOrphanMinAge, &report);
  Expect(report.staging_removed == 0 && report.sidecars_removed == 0 &&
             report.bytes_removed == 0 && report.skipped_live == 0,
         "an empty receive folder reports nothing");

  SweepReport missing;
  SweepOrphans(g_root / "definitely-not-here", kOrphanMinAge, &missing);
  Expect(missing.staging_removed == 0 && missing.sidecars_removed == 0,
         "a missing receive folder is a no-op");
  // 报告指针可以为空，调用方不需要它。
  SweepOrphans(dir, kOrphanMinAge, nullptr);
}

}  // namespace

int main() {
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / "crossdesk_orphan_sweep_test";
  if (ec) {
    std::printf("FAIL  temp directory unavailable\n");
    return 1;
  }
  fs::remove_all(g_root, ec);
  fs::create_directories(g_root, ec);

  TestScheduleDefaults();
  TestScheduleOverride();
  TestLiveStagingKept();
  TestOldStagingRemoved();
  TestSidecarWithoutTargetRemoved();
  TestSidecarWithLiveTargetKept();
  TestStaleSidecarRemovedTargetKept();
  TestReportArithmetic();
  TestEmptyAndMissingDirectories();

  fs::remove_all(g_root, ec);
  if (g_failures == 0) {
    std::printf("orphan_sweep_test: all checks passed\n");
    return 0;
  }
  std::printf("orphan_sweep_test: %d check(s) failed\n", g_failures);
  return 1;
}
