/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-21
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_QUALITY_PRESET_H_
#define _VIDEO_QUALITY_PRESET_H_

#include <algorithm>
#include <cstdint>
#include <string>

namespace crossdesk {
namespace video_quality {

// UU-style one-tap picture tiers. The numbers are the wire value of
// VideoSettings::preset and the stored value of the "video_quality_preset"
// configuration key, so desktop, phone and tests agree on the same ladder.
enum Preset : int {
  kSmooth = 0,  // 流畅
  kHd = 1,      // 高清
  kUhd = 2,     // 超清
};

inline constexpr int kPresetCount = 3;
// No preset: the sender adjusts quality/frame rate/preference field by field,
// exactly like releases that predate the tier table.
inline constexpr int kNoPreset = -1;

inline constexpr const char* kVideoQualityPresetKey = "video_quality_preset";
// 超清 keeps the pre-existing session defaults (quality 2, 60 fps, balanced).
inline constexpr int kDefaultVideoQualityPreset = kUhd;

// One tier as the runtime applies it. width/height bound the outgoing video in
// physical pixels (MiniRtcUpdateVideoResolution preserves the aspect ratio and
// never upscales); 0/0 means "native source size". max_bitrate_bps is a
// sustained video ceiling including FEC/RTX (MiniRtcUpdateVideoBitrateLimit);
// 0 would mean "no ceiling", which no tier uses.
struct PresetSpec {
  int preset;
  int quality;          // VideoSettings::quality / MiniRtcVideoQuality
  int frame_rate;       // 30 or 60, VideoSettings::frame_rate
  int preference;       // 0 frame rate, 1 quality, 2 balanced
  int width;            // outgoing viewport bound, 0 = native
  int height;
  int max_bitrate_bps;  // sustained ceiling
};

// 流畅 trades resolution and bandwidth for cadence, 高清 is the middle tier,
// 超清 is native resolution with the highest ceiling. Every value stays inside
// what the current build can actually do: MiniRtcUpdateVideoResolution accepts
// [2, 16384] or 0/0, MiniRtcUpdateVideoBitrateLimit accepts [1, 100000000], and
// the encoder cadence is 30 or 60.
inline constexpr PresetSpec kPresetTable[kPresetCount] = {
    /* 流畅 */ {kSmooth, 0, 60, 0, 1280, 720, 1'500'000},
    /* 高清 */ {kHd, 1, 60, 2, 1920, 1080, 4'000'000},
    /* 超清 */ {kUhd, 2, 60, 1, 0, 0, 10'000'000},
};

inline int ClampPreset(int preset) {
  return std::clamp(preset, static_cast<int>(kSmooth),
                    static_cast<int>(kUhd));
}

inline const PresetSpec& Spec(int preset) {
  return kPresetTable[ClampPreset(preset)];
}

inline const char* PresetName(int preset) {
  static constexpr const char* kNames[kPresetCount] = {"smooth", "hd", "uhd"};
  return kNames[ClampPreset(preset)];
}

// The encoder quality column mirrors the tier ladder, so a peer that only
// reports quality (older host, phone field-by-field adjustment) still shows the
// right tier.
inline int PresetForQuality(int quality) { return ClampPreset(quality); }

// Value codec for the "video_quality_preset" configuration key. Both directions
// clamp, so a hand-edited ini file can never select a tier that does not exist.
inline std::string FormatVideoQualityPreset(int preset) {
  return std::to_string(ClampPreset(preset));
}

inline int ParseVideoQualityPreset(const char* text, int fallback) {
  if (!text) return ClampPreset(fallback);
  const char* cursor = text;
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  bool negative = false;
  if (*cursor == '+' || *cursor == '-') {
    negative = *cursor == '-';
    ++cursor;
  }
  if (*cursor < '0' || *cursor > '9') return ClampPreset(fallback);
  int value = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    value = value * 10 + (*cursor - '0');
    // 100 already exceeds every tier; stop before the int can overflow.
    if (value > 100) value = 100;
    ++cursor;
  }
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
         *cursor == '\n')
    ++cursor;
  if (*cursor != '\0') return ClampPreset(fallback);
  return ClampPreset(negative ? -value : value);
}

// The runtime surface a preset is pushed through. Every method is one live
// per-peer reconfigure call; none of them reconnect. The seam keeps the change
// set testable without a transport.
class ChangeSink {
 public:
  virtual ~ChangeSink() = default;
  virtual int ApplyVideoSettings(int quality, int frame_rate,
                                 int preference) = 0;
  virtual int ApplyBitrateLimit(int max_bitrate_bps) = 0;
  virtual int ApplyResolution(int width, int height) = 0;
};

struct ChangeResult {
  int preset = kNoPreset;
  bool applied = false;
  // 0 = nothing accepted, 3 = the whole tier landed. A failure stops the set at
  // the failing step so the caller can log exactly where it broke.
  int operations = 0;
  int failed_step = -1;  // 0 settings, 1 bitrate, 2 resolution
};

// Pushes one tier as a single change set: settings, bitrate ceiling, viewport.
// These are the same three calls the preview-subscription path already uses for
// live per-peer reconfiguration, so applying a tier never drops the connection.
inline ChangeResult ApplyPreset(const PresetSpec& spec, ChangeSink& sink) {
  ChangeResult result;
  result.preset = spec.preset;
  if (sink.ApplyVideoSettings(spec.quality, spec.frame_rate, spec.preference) !=
      0) {
    result.failed_step = 0;
    return result;
  }
  result.operations = 1;
  if (sink.ApplyBitrateLimit(spec.max_bitrate_bps) != 0) {
    result.failed_step = 1;
    return result;
  }
  result.operations = 2;
  if (sink.ApplyResolution(spec.width, spec.height) != 0) {
    result.failed_step = 2;
    return result;
  }
  result.operations = 3;
  result.applied = true;
  return result;
}

// Thresholds for the one-way auto-degrade ladder. The defaults are tuned for a
// 60 fps screen stream: 8% sustained inbound loss, or delivered frames below
// half of the tier cadence, for three seconds.
struct DegradeThresholds {
  float loss_fraction = 0.08f;
  int delivery_ratio_percent = 50;
  uint64_t sustain_ms = 3000;
  uint64_t cooldown_ms = 15000;
};

struct DegradeSample {
  uint64_t now_ms = 0;
  // Video inbound loss fraction [0, 1] from MiniRtcNetTrafficStats; the same
  // sample the network panel and the stats rows already show.
  float loss_fraction = 0.0f;
  // Frames actually delivered per second (ReceivedVideoStats); -1 = no sample.
  int received_fps = -1;
};

struct DegradeStep {
  bool step = false;
  int from = kNoPreset;
  int to = kNoPreset;
  // Which signal fired and against which threshold, for the log line.
  const char* metric = "";
  float value = 0.0f;
  float threshold = 0.0f;
};

// 超清 -> 高清 -> 流畅, one step at a time, never back up on its own. A signal
// must stay bad for sustain_ms before the first step, and consecutive steps are
// separated by cooldown_ms, so a spike cannot move the tier and a bad minute
// cannot cascade through the ladder.
class AutoDegrade {
 public:
  AutoDegrade() = default;
  explicit AutoDegrade(const DegradeThresholds& thresholds)
      : thresholds_(thresholds) {}

  const DegradeThresholds& thresholds() const { return thresholds_; }

  DegradeStep Observe(int preset, const DegradeSample& sample) {
    DegradeStep result;
    const int current = ClampPreset(preset);
    // 流畅 is the floor of the ladder.
    if (current <= kSmooth) {
      weak_ = false;
      return result;
    }
    const PresetSpec& spec = Spec(current);
    const char* metric = nullptr;
    float value = 0.0f;
    float threshold = 0.0f;
    if (sample.loss_fraction >= thresholds_.loss_fraction) {
      metric = "loss";
      value = sample.loss_fraction * 100.0f;
      threshold = thresholds_.loss_fraction * 100.0f;
    } else if (sample.received_fps >= 0 && spec.frame_rate > 0 &&
               sample.received_fps * 100 <
                   spec.frame_rate * thresholds_.delivery_ratio_percent) {
      metric = "fps";
      value = static_cast<float>(sample.received_fps);
      threshold = static_cast<float>(spec.frame_rate *
                                     thresholds_.delivery_ratio_percent) /
                  100.0f;
    }
    if (metric == nullptr) {
      weak_ = false;
      return result;
    }
    if (!weak_) {
      weak_ = true;
      weak_since_ms_ = sample.now_ms;
      return result;
    }
    if (sample.now_ms - weak_since_ms_ < thresholds_.sustain_ms) return result;
    if (stepped_ && sample.now_ms - last_step_ms_ < thresholds_.cooldown_ms)
      return result;
    result.step = true;
    result.from = current;
    result.to = current - 1;
    result.metric = metric;
    result.value = value;
    result.threshold = threshold;
    // The next step needs its own sustained window and the cool-down.
    weak_ = false;
    stepped_ = true;
    last_step_ms_ = sample.now_ms;
    return result;
  }

  // 画面根本没有在发（预览关闭、隐私屏、对端暂停）时调用：弱信号窗必须清零，
  // 否则一次停顿会被算进 sustain_ms，恢复后立刻掉一档。
  void ObserveIdle() {
    weak_ = false;
    weak_since_ms_ = 0;
  }

  // Called whenever a tier is actually applied, including a manual pick: the
  // cool-down then also keeps the machine from overriding the user right away.
  void NotifyApplied(int preset, uint64_t now_ms) {
    if (preset == kNoPreset) return;
    stepped_ = true;
    last_step_ms_ = now_ms;
    weak_ = false;
  }

  void Reset() {
    weak_ = false;
    weak_since_ms_ = 0;
    stepped_ = false;
    last_step_ms_ = 0;
  }

  bool stepping() const { return stepped_; }

 private:
  DegradeThresholds thresholds_;
  bool weak_ = false;
  uint64_t weak_since_ms_ = 0;
  bool stepped_ = false;
  uint64_t last_step_ms_ = 0;
};

}  // namespace video_quality
}  // namespace crossdesk

#endif
