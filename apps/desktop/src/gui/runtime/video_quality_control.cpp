#include "video_quality_control.h"

#include "rd_log.h"

namespace crossdesk {
namespace {

// Adapts the tier change set to MiniRTC's live per-peer reconfigure surface.
class MiniRtcQualitySink final : public video_quality::ChangeSink {
 public:
  MiniRtcQualitySink(MiniRtcPeerPtr* peer, const std::string& wire_id)
      : peer_(peer), wire_id_(wire_id) {}

  int ApplyVideoSettings(int quality, int frame_rate, int preference) override {
    return MiniRtcUpdateVideoSettings(
        peer_, wire_id_.data(), wire_id_.size(),
        static_cast<MiniRtcVideoQuality>(quality), frame_rate,
        static_cast<MiniRtcVideoDegradationPreference>(preference));
  }

  int ApplyBitrateLimit(int max_bitrate_bps) override {
    return MiniRtcUpdateVideoBitrateLimit(peer_, wire_id_.data(),
                                          wire_id_.size(), max_bitrate_bps);
  }

  int ApplyResolution(int width, int height) override {
    return MiniRtcUpdateVideoResolution(peer_, wire_id_.data(),
                                        wire_id_.size(), width, height);
  }

 private:
  MiniRtcPeerPtr* peer_;
  std::string wire_id_;
};

}  // namespace

video_quality::ChangeResult ApplyPeerVideoQualityPreset(
    MiniRtcPeerPtr* peer, const std::string& wire_id, int preset,
    const char* origin) {
  video_quality::ChangeResult result;
  if (!peer || wire_id.empty()) {
    result.failed_step = 0;
    return result;
  }
  const auto clamped = video_quality::ClampPreset(preset);
  const auto& spec = video_quality::Spec(clamped);
  MiniRtcQualitySink sink(peer, wire_id);
  result = video_quality::ApplyPreset(spec, sink);
  if (result.applied) {
    LOG_INFO(
        "Video quality preset {} applied ({}): quality={} fps={} preference={} "
        "bitrate={} viewport={}x{}",
        video_quality::PresetName(clamped), origin ? origin : "manual",
        spec.quality, spec.frame_rate, spec.preference, spec.max_bitrate_bps,
        spec.width, spec.height);
  } else {
    LOG_WARN(
        "Video quality preset {} rejected at step {} ({}): {} of 3 live "
        "changes accepted",
        video_quality::PresetName(clamped), result.failed_step,
        origin ? origin : "manual", result.operations);
  }
  return result;
}

}  // namespace crossdesk
