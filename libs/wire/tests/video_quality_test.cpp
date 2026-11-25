// L4 regression test: the UU picture-tier table, the video_quality_preset
// configuration value, the one-way auto-degrade ladder and the atomic change set
// a tier is pushed as. No peer, no runtime, no socket: the sink seam in
// video_quality_preset.h records what the runtime would have done.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <video_quality_preset.h>
#include <remote_action.h>

// The configuration key is read and written through the same SimpleIni copy the
// ConfigCenter uses, so this pins the on-disk form, not just the value codec.
#include "SimpleIni.h"

namespace {

int failures = 0;

bool ExpectTrue(const char* name, bool value) {
  if (value) return true;
  std::cerr << name << " expected true\n";
  ++failures;
  return false;
}

template <typename T>
bool ExpectEqual(const char* name, T actual, T expected) {
  if (actual == expected) return true;
  std::cerr << name << " mismatch\n"
            << "  expected: " << expected << "\n"
            << "  actual:   " << actual << "\n";
  ++failures;
  return false;
}

bool ExpectText(const char* name, const std::string& actual,
                const std::string& expected) {
  if (actual == expected) return true;
  std::cerr << name << " mismatch\n"
            << "  expected: [" << expected << "]\n"
            << "  actual:   [" << actual << "]\n";
  ++failures;
  return false;
}

// Logged metrics are percentages and frame rates; compare them with a tolerance
// instead of exact float equality.
bool ExpectNear(const char* name, float actual, float expected) {
  if (std::fabs(actual - expected) <= 0.01f) return true;
  std::cerr << name << " mismatch\n"
            << "  expected: " << expected << "\n"
            << "  actual:   " << actual << "\n";
  ++failures;
  return false;
}

// Records every live-reconfigure call so a tier application can be inspected as
// one change set. |fail_on| makes that step fail, like a transport that refuses
// the mid-stream resolution change.
class RecordingSink final : public crossdesk::video_quality::ChangeSink {
 public:
  explicit RecordingSink(int fail_on_step = -1) : fail_on(fail_on_step) {}

  int ApplyVideoSettings(int quality, int frame_rate, int preference) override {
    ++operations;
    if (!Refuse(0)) return -1;
    settings_calls.push_back({quality, frame_rate, preference});
    return 0;
  }

  int ApplyBitrateLimit(int max_bitrate_bps) override {
    ++operations;
    if (!Refuse(1)) return -1;
    bitrate_limits.push_back(max_bitrate_bps);
    return 0;
  }

  int ApplyResolution(int width, int height) override {
    ++operations;
    if (!Refuse(2)) return -1;
    resolutions.push_back({width, height});
    return 0;
  }

  struct Settings {
    int quality;
    int frame_rate;
    int preference;
  };
  struct Resolution {
    int width;
    int height;
  };

  int fail_on = -1;
  int operations = 0;
  std::vector<Settings> settings_calls;
  std::vector<int> bitrate_limits;
  std::vector<Resolution> resolutions;

 private:
  bool Refuse(int step) {
    if (fail_on != step) return true;
    fail_on = -1;  // one shot
    ++refusals;
    return false;
  }

 public:
  int refusals = 0;
};

// --- 1. the preset table -----------------------------------------------------

void TestPresetTable() {
  using namespace crossdesk::video_quality;
  ExpectEqual("tier count", kPresetCount, 3);
  ExpectEqual("default tier is 超清", kDefaultVideoQualityPreset, 2);

  ExpectText("key", kVideoQualityPresetKey, "video_quality_preset");

  const auto& smooth = Spec(kSmooth);
  ExpectEqual("smooth quality", smooth.quality, 0);
  ExpectEqual("smooth fps", smooth.frame_rate, 60);
  ExpectEqual("smooth preference", smooth.preference, 0);
  ExpectEqual("smooth width", smooth.width, 1280);
  ExpectEqual("smooth height", smooth.height, 720);
  ExpectEqual("smooth bitrate", smooth.max_bitrate_bps, 1500000);

  const auto& hd = Spec(kHd);
  ExpectEqual("hd quality", hd.quality, 1);
  ExpectEqual("hd fps", hd.frame_rate, 60);
  ExpectEqual("hd preference", hd.preference, 2);
  ExpectEqual("hd width", hd.width, 1920);
  ExpectEqual("hd height", hd.height, 1080);
  ExpectEqual("hd bitrate", hd.max_bitrate_bps, 4000000);

  const auto& uhd = Spec(kUhd);
  ExpectEqual("uhd quality", uhd.quality, 2);
  ExpectEqual("uhd fps", uhd.frame_rate, 60);
  ExpectEqual("uhd preference", uhd.preference, 1);
  // 0/0 restores the native source size: MiniRtcUpdateVideoResolution's own
  // "no bound" contract.
  ExpectEqual("uhd width", uhd.width, 0);
  ExpectEqual("uhd height", uhd.height, 0);
  ExpectEqual("uhd bitrate", uhd.max_bitrate_bps, 10000000);

  // Monotonic ladder: resolution and ceiling never invert.
  ExpectTrue("smooth below hd", smooth.width < hd.width &&
                                    smooth.max_bitrate_bps < hd.max_bitrate_bps);
  ExpectTrue("hd below uhd", hd.width <= 4096 &&
                               hd.max_bitrate_bps < uhd.max_bitrate_bps);

  // Every tier stays inside what MiniRTC accepts today.
  for (int preset = 0; preset < kPresetCount; ++preset) {
    const auto& spec = Spec(preset);
    ExpectEqual("quality in range", spec.quality, preset);
    ExpectTrue("fps valid", spec.frame_rate == 30 || spec.frame_rate == 60);
    ExpectTrue("preference valid", spec.preference >= 0 && spec.preference <= 2);
    ExpectTrue("viewport valid",
               (spec.width == 0 && spec.height == 0) ||
                   (spec.width >= 2 && spec.width <= 16384 && spec.height >= 2 &&
                    spec.height <= 16384));
    ExpectTrue("bitrate valid",
               spec.max_bitrate_bps >= 1 && spec.max_bitrate_bps <= 100000000);
  }

  ExpectText("smooth name", PresetName(kSmooth), "smooth");
  ExpectText("hd name", PresetName(kHd), "hd");
  ExpectText("uhd name", PresetName(kUhd), "uhd");

  // Clamping and the quality <-> tier mapping used by older peers.
  ExpectEqual("clamp low", ClampPreset(-7), 0);
  ExpectEqual("clamp high", ClampPreset(9), 2);
  ExpectEqual("clamp keeps", ClampPreset(1), 1);
  ExpectEqual("no-preset clamps to 流畅", ClampPreset(kNoPreset), 0);
  ExpectEqual("quality 0 -> tier", PresetForQuality(0), int(kSmooth));
  ExpectEqual("quality 1 -> tier", PresetForQuality(1), int(kHd));
  ExpectEqual("quality 2 -> tier", PresetForQuality(2), int(kUhd));
  ExpectEqual("quality out of range", PresetForQuality(5), int(kUhd));
}

// --- 2. the video_quality_preset value round trip ---------------------------

void TestConfigKeyRoundTrip() {
  using namespace crossdesk::video_quality;
  // Value codec: what ConfigCenter writes and how it reads it back.
  ExpectText("format 0", FormatVideoQualityPreset(kSmooth), "0");
  ExpectText("format 1", FormatVideoQualityPreset(kHd), "1");
  ExpectText("format 2", FormatVideoQualityPreset(kUhd), "2");
  ExpectText("format clamps", FormatVideoQualityPreset(42), "2");
  ExpectEqual("parse 0", ParseVideoQualityPreset("0", 2), 0);
  ExpectEqual("parse 1", ParseVideoQualityPreset("1", 2), 1);
  ExpectEqual("parse 2", ParseVideoQualityPreset("2", 2), 2);
  ExpectEqual("parse padded", ParseVideoQualityPreset("  1 \r\n", 2), 1);
  ExpectEqual("parse sign", ParseVideoQualityPreset("+2", 2), 2);
  ExpectEqual("parse clamps high", ParseVideoQualityPreset("7", 2), 2);
  ExpectEqual("parse clamps low", ParseVideoQualityPreset("-3", 2), 0);
  // Garbage keeps the default rather than inventing a tier.
  ExpectEqual("parse empty", ParseVideoQualityPreset("", 1), 1);
  ExpectEqual("parse null", ParseVideoQualityPreset(nullptr, 0), 0);
  ExpectEqual("parse junk", ParseVideoQualityPreset("uhd", 2), 2);
  ExpectEqual("parse trailing junk", ParseVideoQualityPreset("1x", 2), 2);
  ExpectEqual("parse huge", ParseVideoQualityPreset("999999999999", 2), 2);

  // On-disk round trip through the same INI reader ConfigCenter uses.
  const auto path = std::filesystem::temp_directory_path() /
                    ("crossdesk-video-quality-" +
                     std::to_string(static_cast<unsigned long long>(std::rand())) +
                     ".ini");
  const char* section = "Settings";
  {
    CSimpleIniA ini;
    ini.SetUnicode(true);
    ini.SetValue(section, kVideoQualityPresetKey, FormatVideoQualityPreset(kHd).c_str());
    ExpectTrue("ini save", ini.SaveFile(path.string().c_str()) >= 0);
  }
  {
    CSimpleIniA ini;
    ini.SetUnicode(true);
    ExpectTrue("ini load", ini.LoadFile(path.string().c_str()) >= 0);
    const char* stored = ini.GetValue(section, kVideoQualityPresetKey, nullptr);
    ExpectTrue("key present", stored != nullptr);
    ExpectEqual("ini round trip", ParseVideoQualityPreset(stored, 2), 1);
  }
  {
    // A hand-edited or corrupt value falls back to the default and clamps.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "[Settings]\n" << kVideoQualityPresetKey << "=9\n";
  }
  {
    CSimpleIniA ini;
    ini.SetUnicode(true);
    ExpectTrue("ini reload", ini.LoadFile(path.string().c_str()) >= 0);
    ExpectEqual("out of range clamps",
                ParseVideoQualityPreset(
                    ini.GetValue(section, kVideoQualityPresetKey, nullptr), 2),
                2);
  }
  std::error_code error;
  std::filesystem::remove(path, error);
}

// --- 3. the tier on the wire -----------------------------------------------

void TestWirePresetRoundTrip() {
  using namespace crossdesk::video_quality;
  crossdesk::RemoteAction action{};
  action.type = crossdesk::ControlType::video_settings;
  action.vs = {Spec(kHd).quality, Spec(kHd).frame_rate, Spec(kHd).preference,
               7, false, kHd};
  const std::string json = action.to_json();
  ExpectTrue("tier serialized", !json.empty());
  crossdesk::RemoteAction parsed{};
  ExpectTrue("tier parsed", parsed.from_json(json));
  ExpectEqual("wire type", int(parsed.type),
              int(crossdesk::ControlType::video_settings));
  ExpectEqual("wire preset", parsed.vs.preset, int(kHd));
  ExpectEqual("wire quality", parsed.vs.quality, Spec(kHd).quality);
  ExpectEqual("wire fps", parsed.vs.frame_rate, Spec(kHd).frame_rate);
  ExpectEqual("wire preference", parsed.vs.preference, Spec(kHd).preference);
  ExpectEqual("wire request id", parsed.vs.request_id, 7u);

  // Field-by-field senders (older peers, phone sliders) keep working.
  crossdesk::RemoteAction legacy{};
  legacy.type = crossdesk::ControlType::video_settings;
  legacy.vs = {2, 60, 1, 8, false};
  ExpectEqual("legacy preset is none", legacy.vs.preset, int(kNoPreset));
  crossdesk::RemoteAction legacy_parsed{};
  ExpectTrue("legacy parsed", legacy_parsed.from_json(legacy.to_json()));
  ExpectEqual("legacy stays none", legacy_parsed.vs.preset, int(kNoPreset));

  // An unknown tier cannot be encoded: the vocabulary stays closed.
  crossdesk::RemoteAction invalid{};
  invalid.type = crossdesk::ControlType::video_settings;
  invalid.vs = {2, 60, 1, 9, false, 5};
  ExpectTrue("invalid tier rejected", !crossdesk::ValidVideoSettings(invalid.vs));
  ExpectTrue("invalid tier not serialized", invalid.to_json().empty());
}

// --- 4. the atomic change set ----------------------------------------------

void TestAtomicChangeSet() {
  using namespace crossdesk::video_quality;

  for (int preset = 0; preset < kPresetCount; ++preset) {
    RecordingSink sink;
    const auto& spec = Spec(preset);
    const auto result = ApplyPreset(spec, sink);
    ExpectTrue("applied", result.applied);
    ExpectEqual("operations", result.operations, 3);
    ExpectEqual("no failed step", result.failed_step, -1);
    ExpectEqual("result preset", result.preset, preset);
    // Exactly one change set: one call per surface, never a repeated push.
    ExpectEqual("settings calls", int(sink.settings_calls.size()), 1);
    ExpectEqual("bitrate calls", int(sink.bitrate_limits.size()), 1);
    ExpectEqual("resolution calls", int(sink.resolutions.size()), 1);
    ExpectEqual("total live calls", sink.operations, 3);
    ExpectEqual("settings quality", sink.settings_calls[0].quality, spec.quality);
    ExpectEqual("settings fps", sink.settings_calls[0].frame_rate, spec.frame_rate);
    ExpectEqual("settings preference", sink.settings_calls[0].preference,
                spec.preference);
    ExpectEqual("bitrate", sink.bitrate_limits[0], spec.max_bitrate_bps);
    ExpectEqual("resolution width", sink.resolutions[0].width, spec.width);
    ExpectEqual("resolution height", sink.resolutions[0].height, spec.height);
  }

  // A refused step stops the set: the peer never sees the remaining surfaces of
  // a tier that did not land.
  {
    RecordingSink sink(1);
    const auto result = ApplyPreset(Spec(kHd), sink);
    ExpectTrue("not applied", !result.applied);
    ExpectEqual("failure step", result.failed_step, 1);
    ExpectEqual("accepted before failure", result.operations, 1);
    ExpectEqual("settings before failure", int(sink.settings_calls.size()), 1);
    ExpectEqual("no bitrate", int(sink.bitrate_limits.size()), 0);
    ExpectEqual("no resolution", int(sink.resolutions.size()), 0);
  }
  {
    RecordingSink sink(0);
    const auto result = ApplyPreset(Spec(kSmooth), sink);
    ExpectTrue("settings refused", !result.applied);
    ExpectEqual("nothing accepted", result.operations, 0);
    ExpectEqual("failure step 0", result.failed_step, 0);
  }
}

// --- 5. the auto-degrade ladder --------------------------------------------

void TestAutoDegrade() {
  using namespace crossdesk::video_quality;
  const DegradeThresholds thresholds;
  ExpectEqual("default loss threshold", thresholds.loss_fraction, 0.08f);
  ExpectEqual("default delivery ratio", thresholds.delivery_ratio_percent, 50);
  ExpectEqual("default sustain", thresholds.sustain_ms, uint64_t{3000});
  ExpectEqual("default cooldown", thresholds.cooldown_ms, uint64_t{15000});

  const auto bad_loss = [](uint64_t now) {
    DegradeSample sample;
    sample.now_ms = now;
    sample.loss_fraction = 0.15f;
    sample.received_fps = 60;
    return sample;
  };
  const auto healthy = [](uint64_t now) {
    DegradeSample sample;
    sample.now_ms = now;
    sample.loss_fraction = 0.005f;
    sample.received_fps = 59;
    return sample;
  };

  {
    AutoDegrade machine(thresholds);
    // 超清 -> 高清 after a sustained window, with the metric and threshold named.
    auto step = machine.Observe(kUhd, bad_loss(0));
    ExpectTrue("no step at window start", !step.step);
    step = machine.Observe(kUhd, bad_loss(2999));
    ExpectTrue("no step before sustain", !step.step);
    step = machine.Observe(kUhd, bad_loss(3000));
    ExpectTrue("step at sustain", step.step);
    ExpectEqual("step from", step.from, int(kUhd));
    ExpectEqual("step to", step.to, int(kHd));
    ExpectText("step metric", step.metric, "loss");
    ExpectNear("step value", step.value, 15.0f);
    ExpectNear("step threshold", step.threshold, 8.0f);

    // Cool-down: the next step needs a fresh window and 15 s since the last one.
    machine.NotifyApplied(step.to, 3000);
    step = machine.Observe(kHd, bad_loss(3001));
    ExpectTrue("no immediate second step", !step.step);
    step = machine.Observe(kHd, bad_loss(6001));
    ExpectTrue("sustain satisfied but cooling down", !step.step);
    step = machine.Observe(kHd, bad_loss(17999));
    ExpectTrue("still cooling down", !step.step);
    step = machine.Observe(kHd, bad_loss(18000));
    ExpectTrue("step after cool-down", step.step);
    ExpectEqual("second step", step.to, int(kSmooth));

    // 流畅 is the floor and the machine never steps back up.
    step = machine.Observe(kSmooth, bad_loss(40000));
    ExpectTrue("floor holds", !step.step);
    step = machine.Observe(kSmooth, bad_loss(90000));
    ExpectTrue("still at the floor", !step.step);
    ExpectEqual("never upgrades from the floor",
                machine.Observe(kSmooth, healthy(120000)).to, int(kNoPreset));
  }

  {
    // No oscillation: a single bad sample cannot move the tier, and recovery
    // gives no path back up.
    AutoDegrade machine(thresholds);
    ExpectTrue("spike ignored", !machine.Observe(kUhd, bad_loss(0)).step);
    ExpectTrue("spike cleared", !machine.Observe(kUhd, healthy(1000)).step);
    ExpectTrue("window restarted", !machine.Observe(kUhd, bad_loss(2000)).step);
    ExpectTrue("restart needs a full window",
               !machine.Observe(kUhd, bad_loss(4999)).step);
    const auto step = machine.Observe(kUhd, bad_loss(5000));
    ExpectTrue("full window steps", step.step);
    ExpectEqual("steps once", step.to, int(kHd));
    ExpectTrue("no auto-upgrade after a step",
               !machine.Observe(kHd, healthy(60000)).step);
  }

  {
    // Falling delivery rate: 25 fps against a 60 fps tier is below 50%.
    AutoDegrade machine(thresholds);
    DegradeSample sample;
    sample.now_ms = 0;
    sample.loss_fraction = 0.0f;
    sample.received_fps = 25;
    ExpectTrue("fps window start", !machine.Observe(kUhd, sample).step);
    sample.now_ms = 3000;
    const auto step = machine.Observe(kUhd, sample);
    ExpectTrue("fps step", step.step);
    ExpectText("fps metric", step.metric, "fps");
    ExpectNear("fps value", step.value, 25.0f);
    ExpectNear("fps threshold", step.threshold, 30.0f);
    ExpectEqual("fps step target", step.to, int(kHd));

    // A missing sample (-1) is not evidence of a problem.
    AutoDegrade unknown(thresholds);
    DegradeSample no_sample;
    no_sample.now_ms = 0;
    no_sample.received_fps = -1;
    ExpectTrue("unknown start", !unknown.Observe(kUhd, no_sample).step);
    no_sample.now_ms = 10000;
    ExpectTrue("unknown never steps", !unknown.Observe(kUhd, no_sample).step);
  }

  {
    // A manual pick restarts the cool-down so the ladder cannot override it, and
    // Reset clears everything.
    AutoDegrade machine(thresholds);
    machine.Observe(kUhd, bad_loss(0));
    machine.NotifyApplied(kHd, 1000);
    ExpectTrue("no step inside the manual cool-down",
               !machine.Observe(kHd, bad_loss(2999)).step);
    machine.Reset();
    ExpectTrue("reset restarts the window",
               !machine.Observe(kHd, bad_loss(4000)).step);
    ExpectTrue("reset needs a full window",
               !machine.Observe(kHd, bad_loss(6999)).step);
    ExpectTrue("reset steps after the window",
               machine.Observe(kHd, bad_loss(7000)).step);
    ExpectTrue("machine reports stepping", machine.stepping());
  }
}

}  // namespace

// 空闲不能算进持续窗：一次暂停之后恢复，必须重新等满 sustain_ms 才降档。
static void TestIdleClearsTheSustainWindow() {
  crossdesk::video_quality::AutoDegrade machine;
  crossdesk::video_quality::DegradeSample bad;
  bad.loss_fraction = 0.5f;
  bad.received_fps = 60;
  bad.now_ms = 1000;
  machine.Observe(crossdesk::video_quality::kUhd, bad);
  // 到 3000 ms 时本来该降档；中间插入一段空闲，窗口清零，于是还不降。
  machine.ObserveIdle();
  bad.now_ms = 3200;
  const auto not_yet = machine.Observe(crossdesk::video_quality::kUhd, bad);
  ExpectTrue("an idle gap restarts the sustain window", !not_yet.step);
  bad.now_ms = 3400;
  machine.Observe(crossdesk::video_quality::kUhd, bad);
  bad.now_ms = 6400;
  const auto later = machine.Observe(crossdesk::video_quality::kUhd, bad);
  ExpectTrue("a fresh full window still steps down once", later.step);
  ExpectEqual("the step lands one tier below", later.to,
              static_cast<int>(crossdesk::video_quality::kHd));
}

int main() {
  TestPresetTable();
  TestConfigKeyRoundTrip();
  TestWirePresetRoundTrip();
  TestAtomicChangeSet();
  TestAutoDegrade();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "video quality preset: all assertions passed\n";
  return 0;
  TestIdleClearsTheSustainWindow();
}
