/*
 * @Author: DI JUNKUN
 * @Date: 2024-05-29
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONFIG_CENTER_H_
#define _CONFIG_CENTER_H_

#include <atomic>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <utility>
#include <string>

#include <video_quality_preset.h>


namespace crossdesk {

class ConfigCenter {
 public:
  enum class LANGUAGE {
    CHINESE = 0, ENGLISH = 1, RUSSIAN = 2, TRADITIONAL_CHINESE = 3,
    JAPANESE = 4, KOREAN = 5, GERMAN = 6, FRENCH = 7, SPANISH = 8,
    BRAZILIAN_PORTUGUESE = 9, SYSTEM = 10
  };
  enum class CONNECTION_MODE { DEVICE_ID = 0, DIRECT_IP = 1 };
  enum class VIDEO_ENCODE_FORMAT { H264 = 0, AV1 = 1 };
  enum class TURN_MODE {
    DISABLED = 0,
    AUTO_UDP_TCP = 1,
    FORCE_UDP = 2,
    FORCE_TCP = 3
  };

 public:
  explicit ConfigCenter(const std::string& config_path = "config.ini");
  ~ConfigCenter();

  // write config
  int SetLanguage(LANGUAGE language);
  int SetConnectionMode(CONNECTION_MODE mode);
  int SetIpDirectEnabled(bool enabled);
  int SetVideoEncodeFormat(VIDEO_ENCODE_FORMAT video_encode_format);
  int SetTurnMode(TURN_MODE turn_mode);
  int SetForceRelay(bool force_relay);
  int SetServerHost(const std::string& signal_server_host);
  int SetServerPort(int signal_server_port);
  int SetServerEndpoint(const std::string& address, int port);
  int SetSelfHosted(bool enable_self_hosted);
  int SetAutostart(bool enable_autostart);
  int SetDaemon(bool enable_daemon);
  int SetPrivacyScreen(bool enable_privacy_screen);
  int SetPrivacyLocalUnlock(bool allowed);
  int SetLockOnDisconnect(bool enabled);
  int SetTrustedDevicesOnly(bool enabled);
  int SetSaveRemotePreviews(bool enabled);
  int SetPauseVideoWhenMinimized(bool enabled);
  int SetPortableServicePromptSuppressed(bool suppressed);
  int SetSponsorshipPromptDismissedVersion(const std::string& version);
  int SetFileTransferSavePath(const std::string& path);
  int SetPhotoGroupByDate(bool enabled);
  // 0 = UU-style controller-drawn cursor, 1 = system cursor shapes.
  int SetRemoteCursorStyle(int style);
  // 0 = smart, 1 = game (relative), 2 = emulation (absolute, system cursor).
  int SetRemoteMouseMode(int mode);
  // Game mouse speed in tenths: 5..30, 10 = 1.0x.
  int SetGameMouseSpeed(int tenths);
  // Outbound file transfer cap in KiB/s, 0 = unlimited.
  int SetTransferRateLimitKbps(int kbps);
  // 0 = ask/skip, 1 = overwrite, 2 = rename with a " (1)" suffix.
  int SetTransferOverwritePolicy(int policy);
  // 断点续传：发送前先用 op=resume 问对端已有多少字节（1 = 开，0 = 始终从 0 发）。
  int SetTransferResumeEnabled(bool enabled);
  // 画质档位：0 = 流畅, 1 = 高清, 2 = 超清（越界值夹取到 0..2）。
  int SetVideoQualityPreset(int preset);

  // read config

  LANGUAGE GetLanguage() const;
  CONNECTION_MODE GetConnectionMode() const;
  bool IsIpDirectEnabled() const;
  VIDEO_ENCODE_FORMAT GetVideoEncodeFormat() const;
  TURN_MODE GetTurnMode() const;
  int GetDirectStartBitrateBps() const;
  int GetRelayStartBitrateBps() const;
  bool IsReceiverPaddingProbingDisabled() const;
  bool IsForceRelay() const;
  bool IsEnableSrtp() const;
  std::string GetSignalServerHost() const;
  int GetSignalServerPort() const;
  std::string GetDefaultServerHost() const;
  int GetDefaultSignalServerPort() const;
  bool IsSelfHosted() const;
  bool IsEnableAutostart() const;
  bool IsEnableDaemon() const;
  bool IsEnablePrivacyScreen() const;
  bool IsPrivacyLocalUnlockAllowed() const;
  bool IsLockOnDisconnect() const;
  bool IsTrustedDevicesOnly() const;
  bool IsSaveRemotePreviews() const;
  bool IsPauseVideoWhenMinimized() const;
  bool IsPortableServicePromptSuppressed() const;
  std::string GetSponsorshipPromptDismissedVersion() const;
  std::string GetFileTransferSavePath() const;
  bool IsPhotoGroupByDate() const;
  int GetRemoteCursorStyle() const;
  int GetRemoteMouseMode() const;
  int GetGameMouseSpeed() const;
  int GetTransferRateLimitKbps() const;
  int GetTransferOverwritePolicy() const;
  bool GetTransferResumeEnabled() const;
  // 0 = 流畅, 1 = 高清, 2 = 超清。
  int GetVideoQualityPreset() const;

  int Load();
  int Save();
  // Only connection/admission preferences cross into the SYSTEM host profile.
  std::string ExportSharedHostConfig() const;
  int ApplySharedHostConfig(const std::string& contents);

 private:
  int CommitIni(bool warn_only = false);
  int StoreValues(std::initializer_list<std::pair<const char*, std::string>> values);
  mutable std::mutex file_path_mutex_;
  std::string config_path_;
  struct IniStorage;
  std::unique_ptr<IniStorage> ini_;
  const char* section_ = "Settings";

  LANGUAGE language_ = LANGUAGE::SYSTEM;
  CONNECTION_MODE connection_mode_ = CONNECTION_MODE::DEVICE_ID;
  bool enable_ip_direct_ = true;
  VIDEO_ENCODE_FORMAT video_encode_format_ = VIDEO_ENCODE_FORMAT::H264;
  TURN_MODE turn_mode_ = TURN_MODE::AUTO_UDP_TCP;
  static constexpr int kDefaultDirectStartBitrateBps = 2'500'000;
  static constexpr int kDefaultRelayStartBitrateBps = 1'500'000;
  int direct_start_bitrate_bps_ = kDefaultDirectStartBitrateBps;
  int relay_start_bitrate_bps_ = kDefaultRelayStartBitrateBps;
  static constexpr bool kDefaultDisableReceiverPaddingProbing = true;
  bool disable_receiver_padding_probing_ = kDefaultDisableReceiverPaddingProbing;
  std::string signal_server_host_ = "";
  std::string signal_server_host_default_ = "api.crossdesk.cn";
  int signal_server_port_ = 0;
  int server_port_default_ = 9099;
  bool enable_self_hosted_ = false;
  bool enable_autostart_ = false;
  bool enable_daemon_ = false;
  std::atomic<bool> enable_privacy_screen_{true};
  std::atomic<bool> privacy_local_unlock_allowed_{true};
  std::atomic<bool> lock_on_disconnect_{true};
  std::atomic<bool> trusted_devices_only_{false};
  std::atomic<bool> save_remote_previews_{true};
  std::atomic<bool> pause_video_when_minimized_{true};
  // Received photos land in <save path>/Photos/<YYYY-MM> when enabled.
  std::atomic<bool> photo_group_by_date_{true};
  // 0 = UU-style controller cursor overlay (default), 1 = system cursors.
  std::atomic<int> remote_cursor_style_{0};
  // Smart / game / emulation, and the game-mouse speed in tenths.
  std::atomic<int> remote_mouse_mode_{0};
  std::atomic<int> game_mouse_speed_{10};
  // Outbound cap in KiB/s (0 = unlimited, 1 GiB/s ceiling) and the receive
  // side conflict policy: 0 = ask/skip, 1 = overwrite, 2 = rename " (1)".
  static constexpr int kTransferRateLimitMaxKbps = 1048576;
  std::atomic<int> transfer_rate_limit_kbps_{0};
  std::atomic<int> transfer_overwrite_policy_{2};
  // 断点续传开关，0/1，默认开。
  std::atomic<int> transfer_resume_enabled_{1};
  // UU 画质档位 0/1/2，默认 2（超清），与改动前的会话默认画质一致。
  std::atomic<int> video_quality_preset_{video_quality::kDefaultVideoQualityPreset};
  bool portable_service_prompt_suppressed_ = false;
  std::string sponsorship_prompt_dismissed_version_;
  std::string file_transfer_save_path_ = "";
};
}  // namespace crossdesk
#endif
