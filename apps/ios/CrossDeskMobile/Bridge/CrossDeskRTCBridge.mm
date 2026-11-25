#import "CrossDeskRTCBridge.h"

#import <CoreMedia/CoreMedia.h>
#import <Security/Security.h>
#import <UIKit/UIKit.h>
// 引擎泵用 DISPATCH_SOURCE_TYPE_TIMER；显式引 libdispatch，不依赖 Foundation 的
// 传递包含。
#import <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <display_stream_id.h>
#include <app_version.h>
#include <file_transfer_format.h>
#include <remote_action.h>
#include <stream_names.h>
// 共享传输引擎：分块、反馈 ACK、限速、暂停、跨会话续传的发送起点、接收侧
// 暂存与发布全部由它负责，iOS 适配层只提供三条回调和自己的目录。
// 注意：iOS 目标的 HEADER_SEARCH_PATHS 必须包含 libs/controller/include，
// 引擎自己 include 的 <file_transfer_resume.h> 也在这个目录里。
#include <file_transfer_core.h>
#include "minirtc.h"
#include "../../../../libs/controller/include/mobile_device_trust.h"
#include "../../../../libs/controller/include/mobile_direct_connection.h"

namespace {

using crossdesk::mobile_controller::DeviceTrustIdentity;
using crossdesk::mobile_controller::DirectConnection;

using crossdesk::ControlType;
using crossdesk::KeyFlag;
using crossdesk::MouseFlag;
using crossdesk::RemoteAction;
using crossdesk::ServiceCommandFlag;
using crossdesk::kAudioStream;
using crossdesk::kClipboardStream;
using crossdesk::kControlStream;
using crossdesk::kDataStream;
using crossdesk::kFileFeedbackStream;
using crossdesk::kFileStream;
using crossdesk::kKeyboardStream;
using crossdesk::kMaxClipboardBytes;
using crossdesk::kMouseStream;

// 引擎回调（发送 / 文件进度 / 接收进度）比任何一次调用活得久，lambda 里只持
// 弱引用：Session 由桥持有，强引用会形成 Session -> lambda -> 桥 的保留环，
// dealloc 永远不会跑。
struct BridgeWeakRef {
  __weak CrossDeskRTCBridge *owner = nil;
};

enum class PeerRole { Identity, Controller };

struct CallbackContext {
  __weak CrossDeskRTCBridge *owner = nil;
  PeerRole role = PeerRole::Identity;
  DirectConnection *direct = nullptr;
  uint64_t generation = 0;
};

struct PresenceRequest {
  uint64_t generation;
  bool subscription;
};

struct RTCState {
  DirectConnection direct;
  std::unique_ptr<DeviceTrustIdentity> trust;
  MiniRtcPeerPtr *identity_peer = nullptr;
  MiniRtcPeerPtr *controller_peer = nullptr;
  CallbackContext identity_context;
  CallbackContext controller_context;
  std::string signal_host;
  int signal_port = 0;
  MiniRtcVideoDegradationPreference video_adaptation_policy =
      MiniRtcVideoDegradationPreference::MaintainResolution;
  bool identity_ready = false;
  std::deque<PresenceRequest> presence_requests;
  bool identity_recovery_attempted = false;
  std::string identity_with_password;
  std::string identity_base;
  std::string controller_login;
  std::string pending_remote_id;
  std::vector<crossdesk::DisplayResolutionPreference> preferred_display_resolutions;
  uint64_t pending_controller_generation = 0;
  std::string pending_password;
  std::string transmission_id;
  std::string log_path;
};

char kRTCQueueSpecificKey;
// 引擎泵的节拍：5 ms 一拍，1 ms leeway 允许系统把附近的唤醒合并起来。
constexpr uint64_t kFilePumpIntervalNanoseconds = 5ull * 1000ull * 1000ull;
constexpr uint64_t kFilePumpLeewayNanoseconds = 1000ull * 1000ull;

bool IsReusableIdentity(const std::string &identity) {
  const size_t separator = identity.find('@');
  return separator != std::string::npos && separator > 0 &&
         separator + 1 < identity.size();
}

std::string ProvisionedIdentityCredential(const std::string &received,
                                          const std::string &current) {
  // A successful re-login can acknowledge the ID without echoing its password.
  // Retain the credential used to authenticate that same ID.
  if (!IsReusableIdentity(received) && IsReusableIdentity(current) &&
      current.substr(0, current.find('@')) == received) return current;
  return received;
}

NSString *IdentityCredentialAccount(NSString *host, NSInteger port) {
  // Preserve the old server-scoped key so upgrades retain the same identity.
  return [NSString stringWithFormat:@"CrossDeskIdentity.%@.%ld", host, (long)port];
}

NSMutableDictionary *IdentityCredentialQuery(NSString *account) {
  return [@{
    (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService: @"cn.crossdesk.mobile.signaling-identity",
    (__bridge id)kSecAttrAccount: account,
    (__bridge id)kSecAttrSynchronizable: @NO,
  } mutableCopy];
}

bool IsReusableIdentityValue(NSString *value) {
  return [value isKindOfClass:NSString.class] && value.UTF8String &&
         IsReusableIdentity(std::string(value.UTF8String));
}

OSStatus SaveIdentityCredential(NSString *account, NSString *value) {
  if (!IsReusableIdentityValue(value)) return errSecParam;
  NSMutableDictionary *query = IdentityCredentialQuery(account);
  NSDictionary *attributes = @{
    (__bridge id)kSecValueData: [value dataUsingEncoding:NSUTF8StringEncoding],
    (__bridge id)kSecAttrAccessible:
        (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
  };
  // Update in place: deleting first could lose a valid credential on failure.
  OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                  (__bridge CFDictionaryRef)attributes);
  if (status == errSecItemNotFound) {
    NSMutableDictionary *item = [query mutableCopy];
    [item addEntriesFromDictionary:attributes];
    status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
    if (status == errSecDuplicateItem) {
      status = SecItemUpdate((__bridge CFDictionaryRef)query,
                             (__bridge CFDictionaryRef)attributes);
    }
  }
  if (status == errSecSuccess) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:account];
  }
  return status;
}

OSStatus LoadIdentityCredential(NSString *account, NSString **value) {
  *value = nil;
  NSMutableDictionary *query = IdentityCredentialQuery(account);
  query[(__bridge id)kSecReturnData] = @YES;
  query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef result = nullptr;
  OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
  id object = CFBridgingRelease(result);
  NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
  if (status == errSecSuccess) {
    NSString *stored = [object isKindOfClass:NSData.class]
        ? [[NSString alloc] initWithData:object encoding:NSUTF8StringEncoding] : nil;
    if (!IsReusableIdentityValue(stored)) return errSecDecode;
    *value = stored;
    // A Keychain value wins over a stale copy left by an interrupted migration.
    [defaults removeObjectForKey:account];
    return errSecSuccess;
  }
  if (status != errSecItemNotFound) return status;

  NSString *legacy = [defaults objectForKey:account];
  if (!legacy) return errSecItemNotFound;
  if (!IsReusableIdentityValue(legacy)) {
    // Older passwordless IDs cannot authenticate on a subsequent app launch.
    [defaults removeObjectForKey:account];
    return errSecItemNotFound;
  }
  status = SaveIdentityCredential(account, legacy);
  if (status == errSecSuccess) *value = legacy;
  return status;
}

OSStatus MigrateLegacyIdentityCredentials() {
  OSStatus first_error = errSecSuccess;
  for (NSString *key in NSUserDefaults.standardUserDefaults.dictionaryRepresentation) {
    if (![key hasPrefix:@"CrossDeskIdentity."]) continue;
    NSString *unused = nil;
    OSStatus status = LoadIdentityCredential(key, &unused);
    if (status != errSecSuccess && status != errSecItemNotFound &&
        first_error == errSecSuccess) first_error = status;
  }
  return first_error;
}

OSStatus RemoveIdentityCredential(NSString *account) {
  OSStatus status = SecItemDelete((__bridge CFDictionaryRef)IdentityCredentialQuery(account));
  if (status == errSecSuccess || status == errSecItemNotFound) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:account];
    return errSecSuccess;
  }
  return status;
}

std::unique_ptr<DeviceTrustIdentity> LoadDeviceTrustIdentity() {
  // Pairing export and all bridges share one installation-wide Keychain item.
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  NSDictionary *query = @{
    (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService: @"cn.crossdesk.mobile.device-trust",
    (__bridge id)kSecAttrAccount: @"v1",
    (__bridge id)kSecAttrSynchronizable: @NO,
  };
  for (int attempt = 0; attempt < 2; ++attempt) {
    NSMutableDictionary *read = [query mutableCopy];
    read[(__bridge id)kSecReturnData] = @YES;
    read[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)read, &result);
    id object = CFBridgingRelease(result);
    std::string seed;
    struct Clean { std::string& value; ~Clean() { DeviceTrustIdentity::ClearSecret(value); } } clean{seed};
    if (status == errSecSuccess && [object isKindOfClass:NSData.class]) {
      NSData *data = object;
      seed.assign(static_cast<const char *>(data.bytes), data.length);
    } else if (status == errSecItemNotFound &&
        ![NSUserDefaults.standardUserDefaults boolForKey:@"CrossDeskDeviceTrustInitialized.v1"]) {
      seed = DeviceTrustIdentity::GeneratePrivateKey();
      NSMutableDictionary *item = [query mutableCopy];
      item[(__bridge id)kSecValueData] = [NSData dataWithBytes:seed.data() length:seed.size()];
      item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
      status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
      if (status == errSecDuplicateItem) continue;
      if (status != errSecSuccess) throw std::runtime_error("Device signing identity save failed");
    } else {
      throw std::runtime_error("Device signing identity unavailable");
    }
    auto identity = std::make_unique<DeviceTrustIdentity>(seed);
    [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"CrossDeskDeviceTrustInitialized.v1"];
    return identity;
  }
  throw std::runtime_error("Device signing identity unavailable");
}

void CopyCString(char *destination, size_t capacity,
                 const std::string &source) {
  if (!destination || capacity == 0) return;
  std::strncpy(destination, source.c_str(), capacity - 1);
  destination[capacity - 1] = '\0';
}

std::string BaseIdentity(const std::string &identity) {
  const size_t separator = identity.find('@');
  return separator == std::string::npos ? identity
                                        : identity.substr(0, separator);
}

std::string MouseJSON(float x, float y, int wheel, int flag) {
  RemoteAction action{};
  action.type = ControlType::mouse;
  action.m = {std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f),
              wheel, static_cast<MouseFlag>(flag)};
  return action.to_json();
}

std::string KeyJSON(NSUInteger key_code, bool is_down) {
  RemoteAction action{};
  action.type = ControlType::keyboard;
  action.k = {static_cast<std::size_t>(key_code), 0, false,
              is_down ? KeyFlag::key_down : KeyFlag::key_up};
  return action.to_json();
}

struct KeyMapping {
  NSUInteger key_code = 0;
  bool shift = false;
  bool valid = false;
};

KeyMapping MapASCII(unsigned char character) {
  if (character >= 'a' && character <= 'z') {
    return {static_cast<NSUInteger>(character - 'a' + 'A'), false, true};
  }
  if (character >= 'A' && character <= 'Z') {
    return {static_cast<NSUInteger>(character), true, true};
  }
  if (character >= '0' && character <= '9') {
    return {static_cast<NSUInteger>(character), false, true};
  }
  switch (character) {
    case ' ': return {0x20, false, true};
    case '\n': return {0x0D, false, true};
    case '\t': return {0x09, false, true};
    case '-': return {0xBD, false, true};
    case '_': return {0xBD, true, true};
    case '=': return {0xBB, false, true};
    case '+': return {0xBB, true, true};
    case '[': return {0xDB, false, true};
    case '{': return {0xDB, true, true};
    case ']': return {0xDD, false, true};
    case '}': return {0xDD, true, true};
    case '\\': return {0xDC, false, true};
    case '|': return {0xDC, true, true};
    case ';': return {0xBA, false, true};
    case ':': return {0xBA, true, true};
    case '\'': return {0xDE, false, true};
    case '"': return {0xDE, true, true};
    case ',': return {0xBC, false, true};
    case '<': return {0xBC, true, true};
    case '.': return {0xBE, false, true};
    case '>': return {0xBE, true, true};
    case '/': return {0xBF, false, true};
    case '?': return {0xBF, true, true};
    case '`': return {0xC0, false, true};
    case '~': return {0xC0, true, true};
    case '!': return {'1', true, true};
    case '@': return {'2', true, true};
    case '#': return {'3', true, true};
    case '$': return {'4', true, true};
    case '%': return {'5', true, true};
    case '^': return {'6', true, true};
    case '&': return {'7', true, true};
    case '*': return {'8', true, true};
    case '(': return {'9', true, true};
    case ')': return {'0', true, true};
    default: return {};
  }
}

void DispatchMain(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_async(dispatch_get_main_queue(), block);
  }
}

}  // namespace

@interface CrossDeskRTCBridge ()
- (void)handleSignalState:(CrossDeskSignalState)state
                     role:(PeerRole)role
               generation:(uint64_t)generation;
- (void)handleSignalMessage:(const char *)message
                       size:(size_t)size
                       role:(PeerRole)role
                 generation:(uint64_t)generation;
- (void)handleProvisionedIdentity:(const std::string &)identity
                       generation:(uint64_t)generation;
- (void)handleConnectionState:(CrossDeskConnectionState)state
                      remoteID:(const std::string &)remoteID
                    generation:(uint64_t)generation;
- (BOOL)isControllerGenerationActive:(uint64_t)generation;
- (BOOL)isControllerGenerationCurrent:(uint64_t)generation;
- (void)handleVideoFrame:(const MiniRtcVideoFrame *)frame
                sourceID:(const char *)sourceID
            sourceIDSize:(size_t)sourceIDSize;
- (void)enqueueVideoPixelBuffer:(CVPixelBufferRef)pixelBuffer
                           width:(NSInteger)width
                          height:(NSInteger)height
                   captureUptime:(NSTimeInterval)captureUptime;
- (void)resetVideoDelivery;
- (void)notifyVideoSettings:(crossdesk::VideoSettings)settings
                generation:(uint64_t)generation;
- (void)handleAudio:(const char *)data size:(size_t)size;
- (void)handleData:(const char *)data
              size:(size_t)size
          sourceID:(const char *)sourceID
      sourceIDSize:(size_t)sourceIDSize
        generation:(uint64_t)generation;
- (void)handleStats:(const MiniRtcNetTrafficStats *)stats
               mode:(MiniRtcTraversalMode)mode
         generation:(uint64_t)generation;
- (void)sendMessage:(const std::string &)message
            reliable:(BOOL)reliable
              stream:(const char *)stream;
- (void)createIdentityPeer;
- (void)createControllerPeer;
- (void)destroyIdentityPeer;
- (void)reportIdentityStorageStatus:(OSStatus)status;
- (void)destroyControllerPeer;
- (void)createTransferSession;
- (void)destroyTransferSession;
- (void)startFilePump;
- (void)wakeFilePump;
- (void)reportFileTransfer:(NSString *)fileName
                  progress:(double)progress
                   sending:(BOOL)sending
                generation:(uint64_t)generation;
- (void)reportReceivedFileAt:(NSString *)path
                  generation:(uint64_t)generation;
@end

namespace {

void OnVideoFrame(const MiniRtcVideoFrame *frame, const char *, size_t,
                  const char *source_id, size_t source_id_size,
                  void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (owner && context->role == PeerRole::Controller && frame &&
      [owner isControllerGenerationActive:context->generation]) {
    [owner handleVideoFrame:frame
                   sourceID:source_id
               sourceIDSize:source_id_size];
  }
}

void OnAudioBuffer(const char *data, size_t size, const char *, size_t,
                   const char *, size_t, void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (owner && context->role == PeerRole::Controller && data && size > 0 &&
      [owner isControllerGenerationActive:context->generation]) {
    [owner handleAudio:data size:size];
  }
}

void OnDataBuffer(const char *data, size_t size, const char *, size_t,
                  const char *source_id, size_t source_id_size,
                  void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (owner && context->role == PeerRole::Controller &&
      [owner isControllerGenerationActive:context->generation]) {
    [owner handleData:data
                 size:size
             sourceID:source_id
         sourceIDSize:source_id_size
           generation:context->generation];
  }
}

void OnSignalState(MiniRtcSignalStatus status, const char *, size_t, void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (owner &&
      (context->role == PeerRole::Identity ||
       [owner isControllerGenerationActive:context->generation])) {
    [owner handleSignalState:static_cast<CrossDeskSignalState>(status)
                        role:context->role
                  generation:context->generation];
  }
}

void OnSignalMessage(const char *message, size_t size, void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (owner && message && size > 0) {
    [owner handleSignalMessage:message size:size role:context->role
                     generation:context->generation];
  }
}

void OnConnectionState(MiniRtcConnectionStatus status, const char *remote_id,
                       size_t remote_id_size, void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (!owner || context->role != PeerRole::Controller ||
      ![owner isControllerGenerationActive:context->generation]) {
    return;
  }
  if (context->direct && context->direct->Status(status)) return;
  const std::string identifier(remote_id ? remote_id : "", remote_id_size);
  [owner handleConnectionState:static_cast<CrossDeskConnectionState>(status)
                       remoteID:identifier
                     generation:context->generation];
}

void OnNetworkStats(const char *peer_id, size_t peer_id_size,
                    MiniRtcTraversalMode mode, const MiniRtcNetTrafficStats *stats,
                    const char *, size_t, void *user_data) {
  auto *context = static_cast<CallbackContext *>(user_data);
  CrossDeskRTCBridge *owner = context ? context->owner : nil;
  if (!owner) return;
  if (context->role == PeerRole::Identity && mode == MiniRtcTraversalMode::MiniRtcTraversalUnknown &&
      peer_id && peer_id_size > 0) {
    [owner handleProvisionedIdentity:std::string(peer_id, peer_id_size)
                           generation:context->generation];
  } else if (context->role == PeerRole::Controller && stats &&
             [owner isControllerGenerationActive:context->generation]) {
    [owner handleStats:stats mode:mode generation:context->generation];
  }
}

MiniRtcParams MakeParams(const RTCState &state, const std::string &user_id,
                  CallbackContext *context) {
  MiniRtcParams params{};
  params.use_cfg_file = false;
  CopyCString(params.signal_server_ip, sizeof(params.signal_server_ip),
              state.signal_host);
  params.signal_server_port = state.signal_port;
  CopyCString(params.log_path, sizeof(params.log_path), state.log_path);
  // iOS H.264 is provided exclusively by VideoToolbox.
  params.hardware_acceleration = true;
  params.native_video_output = true;
  params.av1_encoding = false;
  params.turn_mode = MiniRtcTurnMode::MiniRtcTurnAutoUdpTcp;
  params.enable_srtp = true;
  params.video_content_type = MiniRtcVideoContentType::ScreenContent;
  params.video_quality = MiniRtcVideoQuality::MiniRtcVideoQualityHigh;
  params.video_frame_rate = 60;
  params.video_degradation_preference = state.video_adaptation_policy;
  params.on_receive_video_buffer = nullptr;
  params.on_receive_audio_buffer = OnAudioBuffer;
  params.on_receive_data_buffer = OnDataBuffer;
  params.on_receive_video_frame = OnVideoFrame;
  params.on_signal_status = OnSignalState;
  params.on_signal_message = OnSignalMessage;
  params.on_connection_status = OnConnectionState;
  params.on_net_status_report = OnNetworkStats;
  params.user_id = user_id.c_str();
  params.user_data = context;
  return params;
}

}  // namespace

@implementation CrossDeskRTCBridge {
  std::unique_ptr<RTCState> _state;
  dispatch_queue_t _rtcQueue;
  CVPixelBufferPoolRef _videoPool;
  size_t _videoPoolWidth;
  size_t _videoPoolHeight;
  std::mutex _videoPoolMutex;
  std::mutex _pendingVideoMutex;
  CVPixelBufferRef _pendingVideoBuffer;
  NSInteger _pendingVideoWidth;
  NSInteger _pendingVideoHeight;
  NSTimeInterval _pendingVideoCaptureUptime;
  bool _videoDeliveryScheduled;
  uint64_t _videoDeliveryGeneration;
  // 共享传输引擎（libs/controller/include/file_transfer_core.h）。与
  // controller_peer 同生共死：createControllerPeer 里创建，destroyControllerPeer
  // 里 Cancel + 释放。所有调用都在 _rtcQueue 上，因此桥内不再需要 _fileMutex
  // 保护的句柄表和分段表。
  std::shared_ptr<crossdesk::file_transfer::Session> _transfer;
  // 引擎泵（dispatch source，跑在 _rtcQueue 上）。限速与暂停的原子量是 UI
  // 线程唯一读取的来源，Session 只在 _rtcQueue 上被摸，重连后按这里的值恢复。
  dispatch_source_t _filePumpTimer;
  std::atomic<uint64_t> _fileSendRateBytesPerSecond;
  std::atomic<bool> _fileSendPaused;
  // picker 给的 security-scoped URL 只保留到本次发送的终态，之后立刻放手。
  NSURL *_sendScopedURL;
  std::atomic<int> _selectedDisplay;
  std::atomic<uint64_t> _receivedVideoFrames;
  std::atomic<uint64_t> _controllerGenerationCounter;
  std::atomic<uint64_t> _activeControllerGeneration;
  std::atomic<uint64_t> _identityGeneration;
  std::atomic<uint64_t> _signalGeneration;
  std::atomic<uint64_t> _presenceGeneration;
  std::atomic<uint64_t> _networkingGeneration;
}

+ (nullable NSString *)directEndpoint:(NSString *)input {
  const auto endpoint = DirectConnection::Endpoint(input.UTF8String ?: "");
  return endpoint.empty() ? nil : [NSString stringWithUTF8String:endpoint.c_str()];
}

+ (nullable NSDictionary<NSString *, NSString *> *)deviceTrustInfoForDeviceID:(NSString *)deviceID {
  try {
    auto trust = LoadDeviceTrustIdentity();
    auto pairing = trust->PairingInfo(BaseIdentity(deviceID.UTF8String ?: ""), "CrossDesk iOS");
    return @{@"pairing": [NSString stringWithUTF8String:pairing.c_str()],
             @"fingerprint": [NSString stringWithUTF8String:trust->Fingerprint().c_str()]};
  } catch (const std::exception&) { return nil; }
}

+ (nullable NSString *)availableUpdateForAppVersion:(NSString *)appVersion
                                       releaseJSON:(NSData *)releaseJSON {
  const auto latest = crossdesk::ParseVersionInfoJSON(std::string_view(
      static_cast<const char *>(releaseJSON.bytes), releaseJSON.length));
  if (!latest) return nil;
  const auto update = crossdesk::AvailableAppUpdate(appVersion.UTF8String ?: "",
                                                   *latest);
  return update ? [NSString stringWithUTF8String:update->c_str()] : nil;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _state = std::make_unique<RTCState>();
    _state->identity_context.owner = self;
    _state->identity_context.role = PeerRole::Identity;
    _state->controller_context.owner = self;
    _state->controller_context.role = PeerRole::Controller;
    _state->controller_context.direct = &_state->direct;
    _rtcQueue = dispatch_queue_create("cn.crossdesk.mobile.minirtc",
                                      DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_rtcQueue, &kRTCQueueSpecificKey,
                                _state.get(), nullptr);
    _videoPool = nullptr;
    _videoPoolWidth = 0;
    _videoPoolHeight = 0;
    _pendingVideoBuffer = nullptr;
    _pendingVideoWidth = 0;
    _pendingVideoHeight = 0;
    _pendingVideoCaptureUptime = 0;
    _videoDeliveryScheduled = false;
    _videoDeliveryGeneration = 0;
    _filePumpTimer = nil;
    _sendScopedURL = nil;
    _fileSendRateBytesPerSecond.store(0);
    _fileSendPaused.store(false);
    _selectedDisplay.store(0);
    _receivedVideoFrames.store(0);
    _controllerGenerationCounter.store(0);
    _activeControllerGeneration.store(0);
    _identityGeneration.store(0);
    _signalGeneration.store(0);
    _presenceGeneration.store(0);
    _networkingGeneration.store(0);
  }
  return self;
}

- (void)dealloc {
  _activeControllerGeneration.store(0);
  _state->identity_context.owner = nil;
  _state->controller_context.owner = nil;
  if (dispatch_get_specific(&kRTCQueueSpecificKey) == _state.get()) {
    [self destroyControllerPeer];
    [self destroyIdentityPeer];
  } else {
    dispatch_sync(_rtcQueue, ^{
      [self destroyControllerPeer];
      [self destroyIdentityPeer];
    });
  }
  [self resetVideoDelivery];
  std::lock_guard<std::mutex> lock(_videoPoolMutex);
  if (_videoPool) {
    CVPixelBufferPoolRelease(_videoPool);
    _videoPool = nullptr;
  }
}

- (void)configureWithSignalHost:(NSString *)host
                     signalPort:(NSInteger)signalPort {
  NSString *trimmed = [host stringByTrimmingCharactersInSet:
                                NSCharacterSet.whitespaceAndNewlineCharacterSet];
  const char *host_c_string = trimmed.UTF8String;
  const std::string host_value = host_c_string ? host_c_string : "";
  if (host_value.empty() || signalPort <= 0) return;

  NSString *cache = NSSearchPathForDirectoriesInDomains(
                        NSCachesDirectory, NSUserDomainMask, YES)
                        .firstObject;
  NSString *logDirectory = [cache stringByAppendingPathComponent:
                                     @"CrossDeskMobile/MiniRTC"];
  [[NSFileManager defaultManager] createDirectoryAtPath:logDirectory
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
  const char *log_c_string = logDirectory.UTF8String;
  const std::string log_path = log_c_string ? log_c_string : "logs";

  const uint64_t networking_generation = _networkingGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (networking_generation != self->_networkingGeneration.load()) return;
    const bool unchanged = self->_state->signal_host == host_value &&
                           self->_state->signal_port == signalPort &&
                           self->_state->identity_peer != nullptr;
    const OSStatus migration_status = MigrateLegacyIdentityCredentials();
    [self reportIdentityStorageStatus:migration_status];
    if (unchanged) {
      if (IsReusableIdentity(self->_state->identity_with_password)) {
        NSString *value = [NSString stringWithUTF8String:self->_state->identity_with_password.c_str()];
        [self reportIdentityStorageStatus:SaveIdentityCredential(
            IdentityCredentialAccount(trimmed, signalPort), value)];
      }
      return;
    }

    [self destroyControllerPeer];
    [self destroyIdentityPeer];
    self->_state->signal_host = host_value;
    self->_state->signal_port = static_cast<int>(signalPort);
    self->_state->log_path = log_path;
    self->_state->identity_ready = false;
    self->_state->identity_recovery_attempted = false;
    self->_state->identity_with_password.clear();
    self->_state->identity_base.clear();

    try {
      self->_state->trust = LoadDeviceTrustIdentity();
    } catch (const std::exception&) {
      [self reportIdentityStorageStatus:errSecDecode];
      [self handleSignalState:CrossDeskSignalStateCredentialUnavailable
                        role:PeerRole::Identity generation:self->_identityGeneration.load()];
      return;
    }

    NSString *cached = nil;
    OSStatus load_status = LoadIdentityCredential(
        IdentityCredentialAccount(trimmed, signalPort), &cached);
    if (load_status == errSecSuccess) {
      self->_state->identity_with_password = cached.UTF8String;
      self->_state->identity_base = BaseIdentity(self->_state->identity_with_password);
    } else if (load_status != errSecItemNotFound) {
      [self reportIdentityStorageStatus:load_status];
      // Locked/unavailable Keychain is not the same as a missing identity.
      [self handleSignalState:CrossDeskSignalStateCredentialUnavailable
                         role:PeerRole::Identity
                   generation:self->_identityGeneration.load()];
      return;
    }
    [self createIdentityPeer];
    if (load_status == errSecSuccess) {
      NSString *identifier = [NSString stringWithUTF8String:self->_state->identity_base.c_str()];
      const uint64_t identity_generation = self->_identityGeneration.load();
      DispatchMain(^{
        if (networking_generation != self->_networkingGeneration.load() ||
            identity_generation != self->_identityGeneration.load()) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didProvisionIdentity:)]) {
          [delegate rtcBridge:self didProvisionIdentity:identifier];
        }
      });
    }
  });
}

- (void)setVideoAdaptationPolicy:(CrossDeskVideoAdaptationPolicy)policy {
  dispatch_async(_rtcQueue, ^{
    switch (policy) {
      case CrossDeskVideoAdaptationPolicyFrameRatePriority:
        self->_state->video_adaptation_policy =
            MiniRtcVideoDegradationPreference::MaintainFrameRate;
        break;
      case CrossDeskVideoAdaptationPolicyBalanced:
        self->_state->video_adaptation_policy =
            MiniRtcVideoDegradationPreference::Balanced;
        break;
      case CrossDeskVideoAdaptationPolicyQualityPriority:
      default:
        self->_state->video_adaptation_policy =
            MiniRtcVideoDegradationPreference::MaintainResolution;
        break;
    }
  });
}

- (void)invalidatePresence {
  _presenceGeneration.fetch_add(1);
}

- (void)sendAnnouncementRequest:(NSData *)request requestID:(NSString *)requestID {
  NSData *payload = [request copy];
  NSString *identifier = [requestID copy];
  const uint64_t generation = _identityGeneration.load();
  const uint64_t signal_generation = _signalGeneration.load();
  const uint64_t networking_generation = _networkingGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (generation != self->_identityGeneration.load() ||
        signal_generation != self->_signalGeneration.load() ||
        networking_generation != self->_networkingGeneration.load()) return;
    if (self->_state->identity_peer && self->_state->identity_ready &&
        !self->_state->identity_base.empty() &&
        MiniRtcSendSignalMessage(self->_state->identity_peer,
                          static_cast<const char *>(payload.bytes), payload.length) == 0) return;
    DispatchMain(^{
      if (generation != self->_identityGeneration.load() ||
          signal_generation != self->_signalGeneration.load() ||
          networking_generation != self->_networkingGeneration.load()) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:@selector(rtcBridge:didFailAnnouncementRequest:)]) {
        [delegate rtcBridge:self didFailAnnouncementRequest:identifier];
      }
    });
  });
}

- (void)sendVideoSettingsWithQuality:(NSInteger)quality
                          frameRate:(NSInteger)frameRate
                         preference:(NSInteger)preference
                          requestID:(uint32_t)requestID {
  if (quality < 0 || quality > 2 || (frameRate != 30 && frameRate != 60) ||
      preference < 0 || preference > 2) return;
  const uint64_t generation = _activeControllerGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation] ||
        !self->_state->controller_peer) return;
    RemoteAction action{};
    action.type = ControlType::video_settings;
    action.vs = {static_cast<int>(quality), static_cast<int>(frameRate),
                 static_cast<int>(preference), requestID, false};
    const auto message = action.to_json();
    if (MiniRtcSendReliableDataFrame(self->_state->controller_peer, message.data(),
                              message.size(), kControlStream) != 0) {
      [self notifyVideoSettings:action.vs generation:generation];
    }
  });
}

- (void)sendDisplayResolution:(NSInteger)displayID
                  displayKey:(NSString *)displayKey
                       width:(NSInteger)width
                      height:(NSInteger)height
                   requestID:(uint32_t)requestID {
  if (displayID < 0 || displayID >= crossdesk::kMaxResolutionDisplays ||
      width <= 0 || width > 32768 || height <= 0 || height > 32768 || !requestID) return;
  NSString *key = [displayKey copy];
  const uint64_t generation = _activeControllerGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation] ||
        !self->_state->controller_peer) return;
    RemoteAction action{};
    action.type = ControlType::display_resolution;
    action.resolution_change = {int(displayID), key.UTF8String ?: "",
                                {int(width), int(height)}, requestID, false};
    const auto message = action.to_json();
    if (message.empty() || MiniRtcSendReliableDataFrame(self->_state->controller_peer,
            message.data(), message.size(), kControlStream) != 0) {
      DispatchMain(^{
        if (![self isControllerGenerationActive:generation]) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didFailDisplayResolutionRequest:)])
          [delegate rtcBridge:self didFailDisplayResolutionRequest:requestID];
      });
    }
  });
}

- (void)notifyVideoSettings:(crossdesk::VideoSettings)settings
                generation:(uint64_t)generation {
  DispatchMain(^{
    if (![self isControllerGenerationActive:generation]) return;
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:
            @selector(rtcBridge:didReceiveVideoSettingsQuality:frameRate:
                                preference:requestID:accepted:)]) {
      [delegate rtcBridge:self
          didReceiveVideoSettingsQuality:settings.quality
                              frameRate:settings.frame_rate
                             preference:settings.preference
                              requestID:settings.request_id
                               accepted:settings.accepted];
    }
  });
}

- (void)requestPresenceForRemoteIDs:(NSArray<NSString *> *)remoteIDs
                         subscribe:(BOOL)subscribe {
  NSArray<NSString *> *owned_ids = [remoteIDs copy];
  const uint64_t generation = _presenceGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (generation != self->_presenceGeneration.load() ||
        !self->_state->identity_peer || !self->_state->identity_ready ||
        self->_state->identity_base.empty()) {
      return;
    }

    NSMutableArray<NSString *> *devices =
        [NSMutableArray arrayWithCapacity:owned_ids.count];
    for (NSString *remote_id in owned_ids) {
      if (![remote_id isKindOfClass:NSString.class]) continue;
      NSString *trimmed = [remote_id
          stringByTrimmingCharactersInSet:
              NSCharacterSet.whitespaceAndNewlineCharacterSet];
      if (trimmed.length > 0) [devices addObject:trimmed];
    }
    NSString *user_id = [NSString
        stringWithUTF8String:self->_state->identity_base.c_str()];
    NSDictionary *request = @{
      @"type" : @"recent_connections_presence",
      @"user_id" : user_id ?: @"",
      @"devices" : devices,
      @"subscribe" : @(subscribe),
    };
    NSData *payload = [NSJSONSerialization dataWithJSONObject:request
                                                      options:0
                                                        error:nil];
    if (!payload) return;
    if (MiniRtcSendSignalMessage(self->_state->identity_peer,
                           static_cast<const char *>(payload.bytes),
                           payload.length) == 0) {
      // Both old and new servers return snapshots in request order. Retain the
      // epoch so a pre-foreground response cannot satisfy a fresh query.
      self->_state->presence_requests.push_back({generation, bool(subscribe)});
    }
  });
}

- (void)connectToRemoteID:(NSString *)remoteID password:(NSString *)password
      displayResolutions:(NSData *)displayResolutions {
  std::vector<crossdesk::DisplayResolutionPreference> preferences;
  if (displayResolutions.length > 0 && displayResolutions.length <= 65536) {
    const auto *bytes = static_cast<const char *>(displayResolutions.bytes);
    crossdesk::ParseDisplayResolutionPreferences(
        std::string(bytes, displayResolutions.length), preferences);
  }
  NSString *cleanID = [remoteID
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  NSString *cleanPassword = [password
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  const char *identifier_c_string = cleanID.UTF8String;
  const char *password_c_string = cleanPassword.UTF8String;
  const std::string identifier =
      identifier_c_string ? identifier_c_string : "";
  const std::string password_value =
      password_c_string ? password_c_string : "";
  if (identifier.empty()) return;

  // Invalidate queued callbacks immediately, before asynchronous teardown. A
  // canceled attempt for the same remote must not close a newly started one.
  const uint64_t generation = _controllerGenerationCounter.fetch_add(1) + 1;
  _activeControllerGeneration.store(0);
  dispatch_async(_rtcQueue, ^{
    if (generation != self->_controllerGenerationCounter.load()) return;
    self->_state->pending_remote_id = identifier;
    self->_state->preferred_display_resolutions = preferences;
    self->_state->pending_controller_generation = generation;
    self->_state->pending_password = password_value;
    [self destroyControllerPeer];
    if (identifier.find(':') != std::string::npos ||
        (self->_state->identity_ready && !self->_state->identity_base.empty())) {
      [self createControllerPeer];
    }
  });
}

- (void)disconnect {
  _controllerGenerationCounter.fetch_add(1);
  _activeControllerGeneration.store(0);
  dispatch_async(_rtcQueue, ^{
    self->_state->pending_remote_id.clear();
    self->_state->pending_password.clear();
    [self destroyControllerPeer];
  });
}

- (void)stopNetworking {
  _networkingGeneration.fetch_add(1);
  _controllerGenerationCounter.fetch_add(1);
  _activeControllerGeneration.store(0);
  _identityGeneration.fetch_add(1);
  _signalGeneration.fetch_add(1);
  _presenceGeneration.fetch_add(1);
  dispatch_async(_rtcQueue, ^{
    self->_state->pending_remote_id.clear();
    self->_state->pending_password.clear();
    [self destroyControllerPeer];
    [self destroyIdentityPeer];
    self->_state->signal_host.clear();
    self->_state->identity_with_password.clear();
    self->_state->identity_base.clear();
  });
}

- (void)requestKeyFrame {
  dispatch_async(_rtcQueue, ^{
    if (self->_state->controller_peer) {
      const std::string stream = crossdesk::MakeDisplayStreamId(
          static_cast<size_t>(self->_selectedDisplay.load()));
      MiniRtcRequestVideoKeyFrame(self->_state->controller_peer, stream.c_str());
    }
  });
}

- (void)sendPointerAtX:(float)x
                     y:(float)y
                action:(CrossDeskPointerAction)action {
  const std::string message = MouseJSON(x, y, 0, static_cast<int>(action));
  // Keep movement, button transitions, and scrolling ordered on the reliable
  // mouse stream.
#if DEBUG
  if (action != CrossDeskPointerActionMove) {
    NSLog(@"CrossDesk pointer action=%ld normalized=(%.5f, %.5f)",
          static_cast<long>(action), x, y);
  }
#endif
  [self sendMessage:message reliable:YES stream:kMouseStream];
}

- (void)sendScrollX:(float)x
                   y:(float)y
              deltaX:(NSInteger)deltaX
              deltaY:(NSInteger)deltaY {
  const bool vertical = std::abs(deltaY) >= std::abs(deltaX);
  const NSInteger raw_delta = vertical ? deltaY : deltaX;
  if (raw_delta == 0) return;
  const int flag = vertical ? 7 : 8;
  const int wheel = raw_delta > 0 ? 1 : -1;
  const std::string message = MouseJSON(x, y, wheel, flag);
  [self sendMessage:message reliable:YES stream:kMouseStream];
}

- (void)sendWindowsKeyCode:(NSUInteger)keyCode isDown:(BOOL)isDown {
  [self sendMessage:KeyJSON(keyCode, isDown)
           reliable:YES
             stream:kKeyboardStream];
}

- (void)sendKeyChord:(NSUInteger)keyCode modifiers:(NSArray<NSNumber *> *)modifiers {
  if (!keyCode || keyCode > 255 || modifiers.count > 4) return;
  std::vector<NSUInteger> keys;
  for (NSUInteger code : {17u, 16u, 18u, 91u}) {
    if ([modifiers containsObject:@(code)]) keys.push_back(code);
  }
  std::vector<std::string> messages;
  for (NSUInteger code : keys) messages.push_back(KeyJSON(code, true));
  messages.push_back(KeyJSON(keyCode, true));
  messages.push_back(KeyJSON(keyCode, false));
  for (auto it = keys.rbegin(); it != keys.rend(); ++it)
    messages.push_back(KeyJSON(*it, false));
  const uint64_t generation = _activeControllerGeneration.load();
  // A whole chord shares one RTC task and can never spill into a new session.
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation] ||
        !self->_state->controller_peer) return;
    for (const auto& message : messages) {
      MiniRtcSendReliableDataFrame(self->_state->controller_peer,
                                  message.data(), message.size(), kKeyboardStream);
    }
  });
}

- (void)sendText:(NSString *)text {
  NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
  if (!utf8.length) return;
  const std::string bytes(static_cast<const char *>(utf8.bytes), utf8.length);
  const uint64_t generation = _activeControllerGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation] ||
        !self->_state->controller_peer) return;
    for (unsigned char character : bytes) {
      KeyMapping mapping = MapASCII(character);
      if (!mapping.valid) continue;
      if (mapping.shift) {
        const std::string shift_down = KeyJSON(0x10, true);
        MiniRtcSendReliableDataFrame(self->_state->controller_peer,
                              shift_down.data(), shift_down.size(),
                              kKeyboardStream);
      }
      const std::string key_down = KeyJSON(mapping.key_code, true);
      const std::string key_up = KeyJSON(mapping.key_code, false);
      MiniRtcSendReliableDataFrame(self->_state->controller_peer, key_down.data(),
                            key_down.size(), kKeyboardStream);
      MiniRtcSendReliableDataFrame(self->_state->controller_peer, key_up.data(),
                            key_up.size(), kKeyboardStream);
      if (mapping.shift) {
        const std::string shift_up = KeyJSON(0x10, false);
        MiniRtcSendReliableDataFrame(self->_state->controller_peer, shift_up.data(),
                              shift_up.size(), kKeyboardStream);
      }
    }
  });
}

- (void)sendCommittedText:(NSString *)text {
  NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
  if (!utf8.length || utf8.length > crossdesk::kMaxTextInputBytes ||
      std::memchr(utf8.bytes, '\0', utf8.length)) return;
  RemoteAction action{};
  action.type = ControlType::text_input;
  action.text.assign(static_cast<const char *>(utf8.bytes), utf8.length);
  [self sendMessage:action.to_json() reliable:YES stream:kKeyboardStream];
}

- (void)switchToDisplay:(NSInteger)displayIndex {
  if (displayIndex < 0 || displayIndex >= 8) return;
  [self resetVideoDelivery];
  _selectedDisplay.store(static_cast<int>(displayIndex));
  _receivedVideoFrames.store(0);
  RemoteAction action{};
  action.type = ControlType::display_id;
  action.d = static_cast<int>(displayIndex);
  [self sendMessage:action.to_json() reliable:YES stream:kControlStream];
  [self requestKeyFrame];
}

- (void)setAudioEnabled:(BOOL)enabled {
  RemoteAction action{};
  action.type = ControlType::audio_capture;
  action.a = enabled;
  [self sendMessage:action.to_json() reliable:YES stream:kControlStream];
}

- (void)sendClipboardText:(NSString *)text {
  NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
  if (!utf8.length || utf8.length > kMaxClipboardBytes) return;
  const std::string bytes(static_cast<const char *>(utf8.bytes), utf8.length);
  [self sendMessage:bytes reliable:YES stream:kClipboardStream];
}

- (void)sendPhotoMessage:(NSData *)payload {
  if (payload.length == 0 || payload.length > 60000) return;
  dispatch_async(_rtcQueue, ^{
    if (!self->_state->controller_peer) return;
    const std::string message(static_cast<const char *>(payload.bytes), payload.length);
    MiniRtcSendReliableDataFrame(self->_state->controller_peer, message.data(),
                                 message.size(), kControlStream);
  });
}

// 共享传输引擎的构造点。与 controller_peer 同一处创建（createControllerPeer）、
// 同一处销毁（destroyControllerPeer）。限速、暂停、续传起点、暂存与发布都在
// libs/controller/include/file_transfer_core.h 里，桥内不再有文件句柄表、
// 分段表和自己拼的反馈 ACK。
- (void)createTransferSession {
  if (_transfer) return;
  NSURL *documents = [[NSFileManager defaultManager]
      URLsForDirectory:NSDocumentDirectory
             inDomains:NSUserDomainMask].firstObject;
  if (!documents) return;
  // 与旧实现同一个落点：Documents/Received。receive_subdirectory 关掉，引擎
  // 就平铺发布（同名冲突时用硬链接换一个带时间戳的名字，绝不覆盖别人的文件）。
  NSURL *directory = [documents URLByAppendingPathComponent:@"Received"
                                                isDirectory:YES];
  if (!directory) return;
  [[NSFileManager defaultManager] createDirectoryAtURL:directory
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
  const char *directory_path = directory.path.fileSystemRepresentation;
  if (!directory_path) return;

  BridgeWeakRef weak_ref;
  weak_ref.owner = self;
  // 会话与这次连接同代：所有通知都按创建时的 generation 过滤，绝不串到下一次
  // 连接上（destroyControllerPeer 先清 active generation，所以拆链之后一律丢弃）。
  const uint64_t session_generation = _activeControllerGeneration.load();

  // 引擎自己给每条帧挑流名：数据块 kFileStream，反馈 kFileFeedbackStream。
  // 返回 0 表示已交给传输层；非 0 会让引擎按发送失败收尾。
  auto send = [weak_ref](const char *stream, const char *data, size_t size) -> int {
    CrossDeskRTCBridge *owner = weak_ref.owner;
    if (!owner || !owner->_state->controller_peer) return -1;
    return MiniRtcSendReliableDataFrame(owner->_state->controller_peer, data,
                                        size, stream ? stream : kFileStream);
  };

  // 文件级进度：-1 失败，0..1 正常，发送侧 sending == true，语义与旧实现一致。
  auto progress = [weak_ref, session_generation](const std::string &name, double value,
                                                 bool sending, const std::string &path) {
    CrossDeskRTCBridge *owner = weak_ref.owner;
    if (!owner) return;
    NSString *fileName = name.empty() ? @"file"
        : [[NSString alloc] initWithBytes:name.data()
                                   length:name.size()
                                 encoding:NSUTF8StringEncoding];
    fileName = fileName ?: @"file";
    if (!sending && !path.empty()) {
      // 发布完成：引擎只在这里给出最终落盘路径（ReceiveState 里只有名字），
      // 也正是每份接收文件唯一的一次完成通知。
      NSString *published = [[NSString alloc] initWithBytes:path.data()
                                                     length:path.size()
                                                   encoding:NSUTF8StringEncoding];
      [owner reportFileTransfer:fileName progress:1 sending:NO
                     generation:session_generation];
      [owner reportReceivedFileAt:published generation:session_generation];
      return;
    }
    if (sending && (value < 0 || value >= 1) && owner->_sendScopedURL) {
      // 发送终态：picker 的 sandbox extension 到这里才放手。
      [owner->_sendScopedURL stopAccessingSecurityScopedResource];
      owner->_sendScopedURL = nil;
    }
    [owner reportFileTransfer:fileName progress:value sending:sending
                   generation:session_generation];
  };

  // 接收侧精确字节数，给 Swift 画接收行用；回收阶段（失败/取消）也走这里。
  auto receive_progress =
      [weak_ref, session_generation](const crossdesk::file_transfer::ReceiveState &state) {
    CrossDeskRTCBridge *owner = weak_ref.owner;
    if (!owner) return;
    NSString *fileName = state.name.empty() ? @"file"
        : [[NSString alloc] initWithBytes:state.name.data()
                                   length:state.name.size()
                                 encoding:NSUTF8StringEncoding];
    fileName = fileName ?: @"file";
    const uint64_t sent = state.received;
    const uint64_t total = state.total;
    DispatchMain(^{
      if (![owner isControllerGenerationActive:session_generation]) return;
      id<CrossDeskRTCBridgeDelegate> delegate = owner.delegate;
      if ([delegate respondsToSelector:
              @selector(rtcBridge:didReceiveFileProgress:sent:total:sending:)]) {
        [delegate rtcBridge:owner
            didReceiveFileProgress:fileName
                              sent:sent
                             total:total
                           sending:NO];
      }
    });
  };

  crossdesk::file_transfer::Options options;
  options.receive_subdirectory = false;  // 平铺发布到 Documents/Received
  options.send_rate_bytes_per_second = _fileSendRateBytesPerSecond.load();
  auto transfer = std::make_shared<crossdesk::file_transfer::Session>(
      send, progress, options, receive_progress);
  transfer->SetDirectory(std::filesystem::path(directory_path));
  transfer->SetSendPaused(_fileSendPaused.load());
  _transfer = std::move(transfer);
  [self startFilePump];
}

// 泵先停，再关会话（引擎在这里清掉未完成的暂存，并把会话标成 closed，迟到的
// 数据回调一律被忽略），最后释放 shared_ptr。全部发生在 _rtcQueue 上。
- (void)destroyTransferSession {
  if (_filePumpTimer) {
    dispatch_source_cancel(_filePumpTimer);
    _filePumpTimer = nil;
  }
  if (_sendScopedURL) {
    [_sendScopedURL stopAccessingSecurityScopedResource];
    _sendScopedURL = nil;
  }
  if (!_transfer) return;
  _transfer->Cancel();
  _transfer.reset();
}

// 引擎的节拍：dispatch source 跑在 _rtcQueue 上，5 ms 一拍。
- (void)startFilePump {
  if (_filePumpTimer) return;
  dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                   _rtcQueue);
  if (!timer) return;
  __weak CrossDeskRTCBridge *weakSelf = self;
  dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, kFilePumpIntervalNanoseconds,
                            kFilePumpLeewayNanoseconds);
  dispatch_source_set_event_handler(timer, ^{
    CrossDeskRTCBridge *owner = weakSelf;
    if (!owner) return;
    // 同一个串行队列上，_transfer 不会被换掉；拷一份 shared_ptr 保证这次调用
    // 期间对象不会被释放。一次 Pump 最多发一块（限速与暂停由引擎自己判）。
    auto transfer = owner->_transfer;
    if (!transfer) return;
    transfer->Pump();
  });
  _filePumpTimer = timer;
  dispatch_resume(timer);
}

// Start / Ack / Receive 之后立刻跑一拍，并把 5 ms 的间隔重新锚到当前时刻，
// 传输不需要等下一个 tick。
- (void)wakeFilePump {
  auto transfer = _transfer;
  if (!transfer) return;
  transfer->Pump();
  if (_filePumpTimer) {
    dispatch_source_set_timer(_filePumpTimer, DISPATCH_TIME_NOW,
                              kFilePumpIntervalNanoseconds,
                              kFilePumpLeewayNanoseconds);
  }
}

// 所有 didUpdateFileTransfer 通知都从这里出去：-1 失败，0..1 正常。
- (void)reportFileTransfer:(NSString *)fileName
                  progress:(double)progress
                   sending:(BOOL)sending
                generation:(uint64_t)generation {
  NSString *name = fileName.length > 0 ? fileName : @"file";
  DispatchMain(^{
    if (![self isControllerGenerationActive:generation]) return;
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:
            @selector(rtcBridge:didUpdateFileTransfer:progress:sending:)]) {
      [delegate rtcBridge:self
          didUpdateFileTransfer:name
                       progress:progress
                        sending:sending];
    }
  });
}

// 每份接收完成文件唯一的一次 didReceiveFileAtURL 通知。
- (void)reportReceivedFileAt:(NSString *)path generation:(uint64_t)generation {
  if (path.length == 0) return;
  NSURL *fileURL = [NSURL fileURLWithPath:path];
  if (!fileURL) return;
  DispatchMain(^{
    if (![self isControllerGenerationActive:generation]) return;
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(rtcBridge:didReceiveFileAtURL:)]) {
      [delegate rtcBridge:self didReceiveFileAtURL:fileURL];
    }
  });
}

- (void)setFileTransferRateKbps:(NSInteger)kbps {
  // <= 0 就是不限速（引擎里 0 表示走传输层速度）。设定值先落原子量，重连后
  // 由 createTransferSession 恢复。
  const uint64_t bytes_per_second =
      kbps <= 0 ? 0 : static_cast<uint64_t>(kbps) * 1024;
  _fileSendRateBytesPerSecond.store(bytes_per_second);
  dispatch_async(_rtcQueue, ^{
    auto transfer = self->_transfer;
    if (transfer) transfer->SetSendRate(bytes_per_second);
  });
}

- (void)setFileTransferPaused:(BOOL)paused {
  _fileSendPaused.store(paused != NO);
  dispatch_async(_rtcQueue, ^{
    auto transfer = self->_transfer;
    if (transfer) transfer->SetSendPaused(paused);
  });
}

- (BOOL)fileTransferPaused {
  return _fileSendPaused.load();
}

- (void)sendFileAtURL:(NSURL *)fileURL {
  [self sendFileAtURL:fileURL resumeOffset:0];
}

// 返回值只说明请求是否被立刻接受（URL/文件名当场可判定的失败返回 NO）；打开
// 文件的失败由引擎报告，走同一条进度回调（-1）。
- (BOOL)sendFileAtURL:(NSURL *)fileURL resumeOffset:(uint64_t)resumeOffset {
  if (!fileURL.isFileURL) return NO;
  NSURL *url = [fileURL copy];
  NSString *fileName = url.lastPathComponent.length > 0 ? url.lastPathComponent : @"file";
  NSData *name_data = [fileName dataUsingEncoding:NSUTF8StringEncoding];
  if (name_data.length == 0 ||
      name_data.length > std::numeric_limits<uint16_t>::max()) {
    return NO;
  }
  const char *path_bytes = url.path.fileSystemRepresentation;
  if (!path_bytes) return NO;
  const std::string path(path_bytes);
  const std::string name(static_cast<const char *>(name_data.bytes), name_data.length);
  const uint64_t generation = _activeControllerGeneration.load();
  // Acquire the document picker's sandbox extension before its completion
  // callback returns; 读盘、限速和 ACK 都在串行的 RTC 队列上由引擎完成。
  const BOOL scoped = [url startAccessingSecurityScopedResource];
  dispatch_async(_rtcQueue, ^{
    BOOL started = NO;
    if ([self isControllerGenerationActive:generation] && self->_transfer &&
        self->_state->controller_peer) {
      // total_size 传 0 表示「按本地文件大小决定」，所以 resumeOffset 0 就是普通
      // 发送；resumeOffset > 0 时引擎会先校验本地文件真有那么多字节再 seek。
      started = self->_transfer->Start(
          path, name, 0, crossdesk::file_transfer::Session::Clock::now(), 0,
          resumeOffset);
    }
    if (started) {
      // 引擎从现在起只从已打开的文件流读，extension 保留到这次发送的终态。
      if (scoped) {
        if (self->_sendScopedURL) {
          [self->_sendScopedURL stopAccessingSecurityScopedResource];
        }
        self->_sendScopedURL = url;
      }
      [self wakeFilePump];
      return;
    }
    if (scoped) [url stopAccessingSecurityScopedResource];
    [self reportFileTransfer:[NSString stringWithUTF8String:name.c_str()]
                    progress:-1
                     sending:YES
                  generation:generation];
  });
  return YES;
}

- (void)sendPrivacyCommand:(BOOL)enable
                      lock:(BOOL)lock
           blockLocalInput:(BOOL)blockLocalInput
                 requestID:(uint32_t)requestID {
  const uint64_t generation = _activeControllerGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation]) return;
    RemoteAction action{};
    action.type = ControlType::privacy_command;
    action.pc = {lock ? crossdesk::PrivacyCommandFlag::lock
                        : enable ? crossdesk::PrivacyCommandFlag::enable
                        : crossdesk::PrivacyCommandFlag::disable,
                 static_cast<bool>(blockLocalInput)};
    const auto message = action.to_json();
    const BOOL success = self->_state->controller_peer &&
        MiniRtcSendReliableDataFrame(self->_state->controller_peer, message.data(),
                              message.size(), kControlStream) == 0;
    DispatchMain(^{
      if (![self isControllerGenerationActive:generation]) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:@selector(rtcBridge:didSendPrivacyRequest:success:)])
        [delegate rtcBridge:self didSendPrivacyRequest:requestID success:success];
    });
  });
}

- (void)sendSecureAttentionSequence {
  const uint64_t generation = _activeControllerGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation]) return;
    RemoteAction action{};
    action.type = ControlType::service_command;
    action.c.flag = ServiceCommandFlag::send_sas;
    const auto message = action.to_json();
    const BOOL success = self->_state->controller_peer &&
        MiniRtcSendReliableDataFrame(self->_state->controller_peer, message.data(), message.size(), kControlStream) == 0;
    DispatchMain(^{
      if (![self isControllerGenerationActive:generation]) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:@selector(rtcBridge:didSendSecureAttention:)]) {
        [delegate rtcBridge:self didSendSecureAttention:success];
      }
    });
  });
}

- (void)sendMessage:(const std::string &)message
            reliable:(BOOL)reliable
              stream:(const char *)stream {
  // This method always hops to the RTC queue. Own both values before the
  // caller's stack frame disappears; several callers pass local/temporary
  // std::strings.
  const std::string owned_message(message);
  const std::string owned_stream(stream ? stream : "");
  const uint64_t generation = _activeControllerGeneration.load();
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation] ||
        !self->_state->controller_peer) return;
    if (reliable) {
      MiniRtcSendReliableDataFrame(self->_state->controller_peer,
                            owned_message.data(), owned_message.size(),
                            owned_stream.c_str());
    } else {
      MiniRtcSendDataFrame(self->_state->controller_peer, owned_message.data(),
                    owned_message.size(), owned_stream.c_str());
    }
  });
}

- (void)reportIdentityStorageStatus:(OSStatus)status {
  const BOOL has_error = status != errSecSuccess;
  if (has_error) {
    // Log only the OSStatus, never an account, credential or Keychain payload.
    NSLog(@"CrossDesk identity Keychain operation failed (%d)", (int)status);
  }
  DispatchMain(^{
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:@selector(rtcBridge:didChangeIdentityStorageError:)]) {
      [delegate rtcBridge:self didChangeIdentityStorageError:has_error];
    }
  });
}

- (void)createIdentityPeer {
  if (_state->identity_peer || _state->signal_host.empty()) return;
  _state->identity_context.generation = _identityGeneration.fetch_add(1) + 1;
  MiniRtcParams params = MakeParams(*_state, _state->identity_with_password,
                             &_state->identity_context);
  _state->identity_peer = MiniRtcCreatePeer(&params);
  if (!_state->identity_peer || MiniRtcInit(_state->identity_peer) != 0) {
    [self destroyIdentityPeer];
    [self handleSignalState:CrossDeskSignalStateFailed
                       role:PeerRole::Identity
                 generation:_identityGeneration.load()];
  }
}

- (void)createControllerPeer {
  const bool direct = _state->pending_remote_id.find(':') != std::string::npos;
  if (_state->controller_peer || _state->pending_remote_id.empty() ||
      (!direct && _state->identity_base.empty())) {
    return;
  }
  const uint64_t generation = _state->pending_controller_generation;
  if (generation == 0 || generation != _controllerGenerationCounter.load()) return;
  _state->controller_context.generation = generation;
  _activeControllerGeneration.store(generation);
  _state->controller_login = "C-" + _state->identity_base;
  MiniRtcParams params = MakeParams(*_state, _state->controller_login,
                             &_state->controller_context);
  _state->controller_peer = MiniRtcCreatePeer(&params);
  if (!_state->controller_peer) {
    _activeControllerGeneration.store(0);
    [self handleConnectionState:CrossDeskConnectionStateFailed
                        remoteID:_state->pending_remote_id
                      generation:generation];
    return;
  }

  // This native iOS peer is controller-only and does not publish a local
  // display. Remote display receivers are created from the host's SDP.
  MiniRtcAddAudioStream(_state->controller_peer, kAudioStream);
  MiniRtcAddDataStream(_state->controller_peer, kDataStream, false);
  MiniRtcAddDataStream(_state->controller_peer, kMouseStream, true);
  MiniRtcAddDataStream(_state->controller_peer, kKeyboardStream, true);
  MiniRtcAddDataStream(_state->controller_peer, kControlStream, true);
  MiniRtcAddDataStream(_state->controller_peer, kFileStream, true);
  MiniRtcAddDataStream(_state->controller_peer, kFileFeedbackStream, true);
  MiniRtcAddDataStream(_state->controller_peer, kClipboardStream, true);

  // 引擎必须在流注册之后创建：它的 send 回调立刻就能把块和反馈发到这两条
  // 已注册的流上。MiniRtcInit 失败时 destroyControllerPeer 会把它关掉。
  [self createTransferSession];

  if (direct) {
    __weak CrossDeskRTCBridge *owner = self;
    const auto endpoint = _state->pending_remote_id;
    dispatch_queue_t queue = _rtcQueue;
    auto notify = [owner, queue, endpoint, generation](MiniRtcConnectionStatus status) {
      const std::string remote = endpoint;
      const uint64_t attempt = generation;
      __weak CrossDeskRTCBridge *bridge = owner;
      dispatch_async(queue, ^{
        [bridge handleConnectionState:static_cast<CrossDeskConnectionState>(status)
                            remoteID:remote generation:attempt];
      });
    };
    if (!_state->trust || !_state->direct.Start(_state->controller_peer, endpoint,
                                                _state->pending_password, notify))
      notify(MiniRtcConnectionFailed);
    _state->pending_password.clear();
    return;
  }
  if (MiniRtcInit(_state->controller_peer) != 0) {
    [self destroyControllerPeer];
    [self handleConnectionState:CrossDeskConnectionStateFailed
                        remoteID:_state->pending_remote_id
                      generation:generation];
    return;
  }
  const std::string join_id = _state->pending_remote_id + "@" +
                              _state->pending_password;
  MiniRtcJoinConnection(_state->controller_peer, join_id.c_str());

  // Joining authenticates with "remote-id@password", while leaving is routed
  // by the remote peer ID alone (the same contract used by the desktop app).
  _state->transmission_id = _state->pending_remote_id;
}

- (void)destroyIdentityPeer {
  _identityGeneration.fetch_add(1);
  _presenceGeneration.fetch_add(1);
  _state->presence_requests.clear();
  if (_state->identity_peer) {
    MiniRtcDestroyPeer(&_state->identity_peer);
  }
  _state->identity_ready = false;
}

- (void)destroyControllerPeer {
  _activeControllerGeneration.store(0);
  // 顺序要紧：先停泵，再让引擎关会话（清暂存、标 closed，迟到的回调一律丢弃），
  // 最后才拆 peer —— 引擎的 send 回调仍然引用 controller_peer。
  [self destroyTransferSession];
  if (_state->controller_peer) {
    _state->direct.Stop();
    if (!_state->transmission_id.empty()) {
      MiniRtcLeaveConnection(_state->controller_peer,
                      _state->transmission_id.c_str());
    }
    MiniRtcDestroyPeer(&_state->controller_peer);
  }
  _state->controller_login.clear();
  _state->transmission_id.clear();
  _selectedDisplay.store(0);
  _receivedVideoFrames.store(0);
  [self resetVideoDelivery];
}

- (void)handleSignalState:(CrossDeskSignalState)state
                     role:(PeerRole)role
               generation:(uint64_t)generation {
  // Only the long-lived identity peer drives presence and the home indicator.
  if (role != PeerRole::Identity || generation != _identityGeneration.load()) {
    return;
  }
  const uint64_t signal_generation = _signalGeneration.fetch_add(1) + 1;
  _presenceGeneration.fetch_add(1);
  dispatch_async(_rtcQueue, ^{
    if (generation != self->_identityGeneration.load() ||
        signal_generation != self->_signalGeneration.load()) return;
    self->_state->presence_requests.clear();
    if (role == PeerRole::Identity) {
      self->_state->identity_ready = state == CrossDeskSignalStateConnected;
      if (self->_state->identity_ready && self->_state->identity_peer) {
        // The server records client_info on the persistent identity peer,
        // before any remote session starts and again after reconnecting.
        constexpr char client_info[] =
            "{\"type\":\"client_info\",\"version\":\"ios-native\","
            "\"platform\":\"ios\"}";
        MiniRtcSendSignalMessage(self->_state->identity_peer, client_info,
                          sizeof(client_info) - 1);
      }
      if (state == CrossDeskSignalStateFailed &&
          !self->_state->identity_with_password.empty() &&
          !self->_state->identity_recovery_attempted) {
        // The saved credential can become invalid after a server-side reset or
        // an interrupted password change. Retry once with an empty identity so
        // the signaling server provisions a new credential.
        self->_state->identity_recovery_attempted = true;
        NSString *host = [NSString stringWithUTF8String:
                                  self->_state->signal_host.c_str()];
        OSStatus status = RemoveIdentityCredential(
            IdentityCredentialAccount(host, self->_state->signal_port));
        [self reportIdentityStorageStatus:status];
        if (status != errSecSuccess) {
          [self destroyIdentityPeer];
          [self handleSignalState:CrossDeskSignalStateCredentialUnavailable
                             role:PeerRole::Identity
                       generation:self->_identityGeneration.load()];
          return;
        }
        [self destroyIdentityPeer];
        self->_state->identity_with_password.clear();
        self->_state->identity_base.clear();
        [self createIdentityPeer];
      }
      if (self->_state->identity_ready &&
          !self->_state->identity_base.empty() &&
          !self->_state->pending_remote_id.empty() &&
          !self->_state->controller_peer) {
        [self createControllerPeer];
      }
    }
    // The home-page signal indicator represents the long-lived identity peer.
    // A controller peer is created and destroyed for each session; forwarding
    // its shutdown would incorrectly make the server look disconnected.
    if (role == PeerRole::Identity) {
      DispatchMain(^{
        if (generation != self->_identityGeneration.load() ||
            signal_generation != self->_signalGeneration.load()) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:
                @selector(rtcBridge:didChangeSignalState:)]) {
          [delegate rtcBridge:self didChangeSignalState:state];
        }
      });
    }
  });
}

- (void)handleSignalMessage:(const char *)message
                       size:(size_t)size
                       role:(PeerRole)role
                 generation:(uint64_t)generation {
  // Twenty 8 KiB bodies may expand substantially when JSON-escaped.
  constexpr size_t kMaxSignalMessageSize = 1024 * 1024;
  if (role != PeerRole::Identity || generation != _identityGeneration.load() ||
      !message || size == 0 ||
      size > kMaxSignalMessageSize) {
    return;
  }

  const uint64_t signal_generation = _signalGeneration.load();
  const uint64_t presence_generation = _presenceGeneration.load();

  NSData *payload = [NSData dataWithBytes:message length:size];
  id decoded = [NSJSONSerialization JSONObjectWithData:payload
                                                options:0
                                                  error:nil];
  if (![decoded isKindOfClass:NSDictionary.class]) return;
  NSDictionary *object = static_cast<NSDictionary *>(decoded);
  NSString *type = object[@"type"];
  if (![type isKindOfClass:NSString.class]) return;

  if ([type isEqualToString:@"announcements"] ||
      [type isEqualToString:@"announcements_changed"]) {
    DispatchMain(^{
      if (generation != self->_identityGeneration.load() ||
          signal_generation != self->_signalGeneration.load()) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:@selector(rtcBridge:didReceiveAnnouncementMessage:)]) {
        [delegate rtcBridge:self didReceiveAnnouncementMessage:payload];
      }
    });
    return;
  }

  NSMutableDictionary<NSString *, NSNumber *> *presence =
      [NSMutableDictionary dictionary];
  if ([type isEqualToString:@"presence"]) {
    NSArray *devices = object[@"devices"];
    if (![devices isKindOfClass:NSArray.class]) return;
    for (id value in devices) {
      if (![value isKindOfClass:NSDictionary.class]) continue;
      NSDictionary *device = static_cast<NSDictionary *>(value);
      NSString *remote_id = device[@"id"];
      NSNumber *online = device[@"online"];
      if ([remote_id isKindOfClass:NSString.class] && remote_id.length > 0 &&
          [online isKindOfClass:NSNumber.class]) {
        presence[remote_id] = @([online boolValue]);
      }
    }
  } else if ([type isEqualToString:@"presence_update"]) {
    NSString *remote_id = object[@"id"];
    NSNumber *online = object[@"online"];
    if ([remote_id isKindOfClass:NSString.class] && remote_id.length > 0 &&
        [online isKindOfClass:NSNumber.class]) {
      presence[remote_id] = @([online boolValue]);
    }
  } else {
    return;
  }
  NSDictionary<NSString *, NSNumber *> *result = [presence copy];
  const bool response = [type isEqualToString:@"presence"];
  dispatch_async(_rtcQueue, ^{
    if (generation != self->_identityGeneration.load() ||
        signal_generation != self->_signalGeneration.load() ||
        !self->_state->identity_ready) return;
    uint64_t delivery_generation = presence_generation;
    BOOL snapshot = NO;
    if (response) {
      if (self->_state->presence_requests.empty()) return;
      const auto request = self->_state->presence_requests.front();
      self->_state->presence_requests.pop_front();
      delivery_generation = request.generation;
      snapshot = request.subscription;
    }
    if (delivery_generation != self->_presenceGeneration.load()) return;
    DispatchMain(^{
      if (generation != self->_identityGeneration.load() ||
          signal_generation != self->_signalGeneration.load() ||
          delivery_generation != self->_presenceGeneration.load()) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:
              @selector(rtcBridge:didReceivePresence:snapshot:)]) {
        [delegate rtcBridge:self didReceivePresence:result snapshot:snapshot];
      }
    });
  });
}

- (void)handleProvisionedIdentity:(const std::string &)identity
                       generation:(uint64_t)generation {
  if (identity.empty()) return;
  const std::string identity_copy = identity;
  dispatch_async(_rtcQueue, ^{
    if (generation != self->_identityGeneration.load()) return;
    const std::string credential = ProvisionedIdentityCredential(
        identity_copy, self->_state->identity_with_password);
    self->_state->identity_with_password = credential;
    self->_state->identity_base = BaseIdentity(credential);
    NSString *host = [NSString stringWithUTF8String:
                                self->_state->signal_host.c_str()];
    NSString *account = IdentityCredentialAccount(host, self->_state->signal_port);
    NSString *value = [NSString stringWithUTF8String:credential.c_str()];
    OSStatus status = IsReusableIdentity(credential)
        ? SaveIdentityCredential(account, value)
        : RemoveIdentityCredential(account);
    [self reportIdentityStorageStatus:status];
    // Only the public device ID crosses into UI state; keep its password here.
    NSString *identifier = [NSString stringWithUTF8String:self->_state->identity_base.c_str()];
    DispatchMain(^{
      if (generation != self->_identityGeneration.load()) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:@selector(rtcBridge:didProvisionIdentity:)]) {
        [delegate rtcBridge:self didProvisionIdentity:identifier];
      }
    });
  });
}

- (void)handleConnectionState:(CrossDeskConnectionState)state
                      remoteID:(const std::string &)remoteID
                    generation:(uint64_t)generation {
  if (![self isControllerGenerationCurrent:generation]) return;
  if (state != CrossDeskConnectionStateFailed &&
      ![self isControllerGenerationActive:generation]) return;
  const std::string remote_copy = remoteID;
  if (state == CrossDeskConnectionStateConnected) {
    dispatch_async(_rtcQueue, ^{
      if (![self isControllerGenerationActive:generation] ||
          !self->_state->controller_peer) {
        return;
      }
      const std::string controller_name =
          self->_state->identity_base.empty()
              ? "iPhone"
              : "iPhone-" + self->_state->identity_base;
      RemoteAction action{};
      action.type = ControlType::host_infomation;
      action.preferred_display_resolutions = self->_state->preferred_display_resolutions;
      action.i.platform = crossdesk::HostPlatform::IOS;
      action.i.supports_privacy_screen = true;
      action.i.supports_video_codec_status = true;
      CopyCString(action.i.host_name, sizeof(action.i.host_name),
                  controller_name);
      action.i.host_name_size = std::strlen(action.i.host_name);
      NSString *appVersion = [[NSBundle mainBundle]
          objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
      CopyCString(action.i.app_version, sizeof(action.i.app_version),
                  appVersion.UTF8String ?: "");
      action.i.display_list = nullptr;
      action.i.display_num = 0;
      action.i.left = nullptr;
      action.i.top = nullptr;
      action.i.right = nullptr;
      action.i.bottom = nullptr;
      const std::string host_info = DeviceTrustIdentity::Advertise(action.to_json());
      MiniRtcSendReliableDataFrame(self->_state->controller_peer, host_info.data(),
                            host_info.size(), kControlStream);
      const std::string stream = crossdesk::MakeDisplayStreamId(
          static_cast<size_t>(self->_selectedDisplay.load()));
      MiniRtcRequestVideoKeyFrame(self->_state->controller_peer, stream.c_str());
    });
  }
  DispatchMain(^{
    if (![self isControllerGenerationCurrent:generation]) return;
    if (state != CrossDeskConnectionStateFailed &&
        ![self isControllerGenerationActive:generation]) {
      return;
    }
    NSString *identifier = [NSString stringWithUTF8String:remote_copy.c_str()];
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:
            @selector(rtcBridge:didChangeConnectionState:remoteID:)]) {
      [delegate rtcBridge:self didChangeConnectionState:state remoteID:identifier];
    }
  });
}

- (BOOL)isControllerGenerationActive:(uint64_t)generation {
  return generation != 0 &&
         _controllerGenerationCounter.load() == generation &&
         _activeControllerGeneration.load() == generation;
}

- (BOOL)isControllerGenerationCurrent:(uint64_t)generation {
  if (generation == 0 ||
      _controllerGenerationCounter.load() != generation) {
    return NO;
  }
  const uint64_t active = _activeControllerGeneration.load();
  return active == 0 || active == generation;
}

- (void)handleVideoFrame:(const MiniRtcVideoFrame *)frame
                sourceID:(const char *)sourceID
            sourceIDSize:(size_t)sourceIDSize {
  if (!frame || frame->width == 0 || frame->height == 0) return;
  if (sourceID && sourceIDSize > 0) {
    const std::string source(sourceID, sourceIDSize);
    const std::string expected = crossdesk::MakeDisplayStreamId(
        static_cast<size_t>(_selectedDisplay.load()));
    if (source != expected) return;
  }
  // Translate the transport's calibrated capture time once, while the peer is
  // alive. Carry it with the exact pixels through coalescing and UI submission.
  const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
  const int64_t peer_now = MiniRtcGetSystemTimeMicros(_state->controller_peer);
  NSTimeInterval capture_uptime = 0;
  if (frame->captured_timestamp > 0 && peer_now > 0 &&
      frame->captured_timestamp <= static_cast<uint64_t>(peer_now) &&
      static_cast<uint64_t>(peer_now) - frame->captured_timestamp <= 5'000'000) {
    capture_uptime = now -
        (static_cast<uint64_t>(peer_now) - frame->captured_timestamp) / 1'000'000.0;
  }
  const MiniRtcNativeVideoFrame *native_frame = frame->native_frame;
  const bool has_native_pixel_buffer =
      native_frame &&
      native_frame->struct_size >=
          static_cast<uint32_t>(sizeof(MiniRtcNativeVideoFrame)) &&
      native_frame->type == MiniRtcNativeVideoFrameCVPixelBuffer &&
      native_frame->payload.cv_pixel_buffer;
  if (has_native_pixel_buffer) {
    CVPixelBufferRef pixel_buffer =
        static_cast<CVPixelBufferRef>(
            native_frame->payload.cv_pixel_buffer);
    [self enqueueVideoPixelBuffer:pixel_buffer
                            width:frame->width
                           height:frame->height
                    captureUptime:capture_uptime];
    return;
  }

  if (!frame->data) return;
  const size_t y_size = static_cast<size_t>(frame->width) * frame->height;
  const size_t required_size = y_size + y_size / 2;
  if (frame->size < required_size) return;

  CVPixelBufferRef pixel_buffer = nullptr;
  CVReturn result = kCVReturnError;
  {
    std::lock_guard<std::mutex> lock(_videoPoolMutex);
    if (!_videoPool || _videoPoolWidth != frame->width ||
        _videoPoolHeight != frame->height) {
      if (_videoPool) CVPixelBufferPoolRelease(_videoPool);
      _videoPool = nullptr;
      _videoPoolWidth = frame->width;
      _videoPoolHeight = frame->height;
      NSDictionary *poolAttributes = @{
        (NSString *)kCVPixelBufferPoolMinimumBufferCountKey : @3,
      };
      NSDictionary *pixelAttributes = @{
        (NSString *)kCVPixelBufferWidthKey : @(frame->width),
        (NSString *)kCVPixelBufferHeightKey : @(frame->height),
        (NSString *)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (NSString *)kCVPixelBufferMetalCompatibilityKey : @YES,
      };
      CVPixelBufferPoolCreate(kCFAllocatorDefault,
                              (__bridge CFDictionaryRef)poolAttributes,
                              (__bridge CFDictionaryRef)pixelAttributes,
                              &_videoPool);
    }
    if (_videoPool) {
      result = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                                   _videoPool, &pixel_buffer);
    }
  }
  if (result != kCVReturnSuccess || !pixel_buffer) return;

  CVPixelBufferLockBaseAddress(pixel_buffer, 0);
  const uint8_t *source_y = reinterpret_cast<const uint8_t *>(frame->data);
  const uint8_t *source_uv = source_y + y_size;
  uint8_t *destination_y = static_cast<uint8_t *>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  uint8_t *destination_uv = static_cast<uint8_t *>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
  for (size_t row = 0; row < frame->height; ++row) {
    std::memcpy(destination_y + row * y_stride,
                source_y + row * frame->width, frame->width);
  }
  for (size_t row = 0; row < frame->height / 2; ++row) {
    std::memcpy(destination_uv + row * uv_stride,
                source_uv + row * frame->width, frame->width);
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
  CVBufferSetAttachment(pixel_buffer, kCVImageBufferYCbCrMatrixKey,
                        kCVImageBufferYCbCrMatrix_ITU_R_601_4,
                        kCVAttachmentMode_ShouldPropagate);

  const NSInteger frame_width = frame->width;
  const NSInteger frame_height = frame->height;
  if (_receivedVideoFrames.load() == 0) {
    uint8_t minimum_luma = std::numeric_limits<uint8_t>::max();
    uint8_t maximum_luma = std::numeric_limits<uint8_t>::min();
    uint64_t luma_total = 0;
    size_t luma_samples = 0;
    const size_t row_step = std::max<size_t>(1, frame->height / 96);
    const size_t column_step = std::max<size_t>(1, frame->width / 96);
    for (size_t row = 0; row < frame->height; row += row_step) {
      for (size_t column = 0; column < frame->width; column += column_step) {
        const uint8_t luma = source_y[row * frame->width + column];
        minimum_luma = std::min(minimum_luma, luma);
        maximum_luma = std::max(maximum_luma, luma);
        luma_total += luma;
        ++luma_samples;
      }
    }
    const double average_luma = luma_samples
                                    ? static_cast<double>(luma_total) /
                                          static_cast<double>(luma_samples)
                                    : 0.0;
    NSLog(@"CrossDesk first-frame luma min=%u max=%u average=%.1f",
          minimum_luma, maximum_luma, average_luma);
  }

  [self enqueueVideoPixelBuffer:pixel_buffer
                          width:frame_width
                         height:frame_height
                  captureUptime:capture_uptime];
  CVPixelBufferRelease(pixel_buffer);
}

- (void)enqueueVideoPixelBuffer:(CVPixelBufferRef)pixelBuffer
                           width:(NSInteger)width
                          height:(NSInteger)height
                   captureUptime:(NSTimeInterval)captureUptime {
  if (!pixelBuffer) return;

  CVPixelBufferRetain(pixelBuffer);
  bool should_schedule = false;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(_pendingVideoMutex);
    if (_pendingVideoBuffer) {
      CVPixelBufferRelease(_pendingVideoBuffer);
    }
    _pendingVideoBuffer = pixelBuffer;
    _pendingVideoWidth = width;
    _pendingVideoHeight = height;
    _pendingVideoCaptureUptime = captureUptime;
    generation = _videoDeliveryGeneration;
    if (!_videoDeliveryScheduled) {
      _videoDeliveryScheduled = true;
      should_schedule = true;
    }
  }

  if (!should_schedule) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    CVPixelBufferRef latest_buffer = nullptr;
    NSInteger latest_width = 0;
    NSInteger latest_height = 0;
    NSTimeInterval latest_capture_uptime = 0;
    {
      std::lock_guard<std::mutex> lock(self->_pendingVideoMutex);
      if (generation != self->_videoDeliveryGeneration) return;
      latest_buffer = self->_pendingVideoBuffer;
      latest_width = self->_pendingVideoWidth;
      latest_height = self->_pendingVideoHeight;
      latest_capture_uptime = self->_pendingVideoCaptureUptime;
      self->_pendingVideoBuffer = nullptr;
      self->_pendingVideoWidth = 0;
      self->_pendingVideoHeight = 0;
      self->_pendingVideoCaptureUptime = 0;
      self->_videoDeliveryScheduled = false;
    }

    if (!latest_buffer) return;
    const uint64_t frame_count = ++self->_receivedVideoFrames;
    if (frame_count == 1 || frame_count % 300 == 0) {
      NSLog(@"CrossDesk delivered latest frame %llu (%ld x %ld)",
            static_cast<unsigned long long>(frame_count), (long)latest_width,
            (long)latest_height);
    }
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:
            @selector(rtcBridge:didReceivePixelBuffer:width:height:captureUptime:)]) {
      [delegate rtcBridge:self
          didReceivePixelBuffer:latest_buffer
                         width:latest_width
                        height:latest_height
                  captureUptime:latest_capture_uptime];
    }
    CVPixelBufferRelease(latest_buffer);
  });
}

- (void)resetVideoDelivery {
  CVPixelBufferRef pending_buffer = nullptr;
  {
    std::lock_guard<std::mutex> lock(_pendingVideoMutex);
    ++_videoDeliveryGeneration;
    pending_buffer = _pendingVideoBuffer;
    _pendingVideoBuffer = nullptr;
    _pendingVideoWidth = 0;
    _pendingVideoHeight = 0;
    _pendingVideoCaptureUptime = 0;
    _videoDeliveryScheduled = false;
  }
  if (pending_buffer) CVPixelBufferRelease(pending_buffer);
}

- (void)handleAudio:(const char *)data size:(size_t)size {
  if (!data || size < sizeof(int16_t) || size % sizeof(int16_t) != 0) return;
  NSData *pcm = [NSData dataWithBytes:data length:size];
  DispatchMain(^{
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:
            @selector(rtcBridge:didReceiveAudioPCM:)]) {
      [delegate rtcBridge:self didReceiveAudioPCM:pcm];
    }
  });
}

- (void)handleData:(const char *)data
              size:(size_t)size
          sourceID:(const char *)sourceID
      sourceIDSize:(size_t)sourceIDSize
        generation:(uint64_t)generation {
  if (!data || size == 0 || !sourceID) return;
  const std::string source(sourceID, sourceIDSize);
  if (source == kControlStream) {
    if (DeviceTrustIdentity::IsChallenge(std::string_view(data, size))) {
      const std::string challenge(data, size);
      dispatch_async(_rtcQueue, ^{
        if (![self isControllerGenerationActive:generation] || !self->_state->controller_peer ||
            !self->_state->trust) return;
        auto proof = self->_state->trust->ChallengeResponse(challenge,
            self->_state->identity_base, "CrossDesk iOS");
        if (proof && !proof->empty())
          MiniRtcSendReliableDataFrame(self->_state->controller_peer, proof->data(), proof->size(), kControlStream);
      });
      return;
    }
    // Photo album replies are file_browser JSON (string "type"), which the
    // numeric RemoteAction parser rejects; forward them verbatim to Swift.
    if (std::string_view(data, size).find("\"file_browser\"") != std::string_view::npos) {
      NSData *payload = [NSData dataWithBytes:data length:size];
      DispatchMain(^{
        if (![self isControllerGenerationActive:generation]) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didReceivePhotoMessage:)])
          [delegate rtcBridge:self didReceivePhotoMessage:payload];
      });
      return;
    }
    RemoteAction action{};
    if (!action.from_json(std::string(data, size))) return;
    if (action.type == ControlType::video_codec_status) {
      NSString *encoder = [NSString stringWithUTF8String:action.vcs.encoder];
      DispatchMain(^{
        if (![self isControllerGenerationActive:generation]) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didUpdateRemoteVideoEncoder:)])
          [delegate rtcBridge:self didUpdateRemoteVideoEncoder:encoder ?: @""];
      });
      return;
    }
    if (action.type == ControlType::privacy_status) {
      const auto message = action.to_json();
      NSData *status = [NSData dataWithBytes:message.data() length:message.size()];
      DispatchMain(^{
        if (![self isControllerGenerationActive:generation]) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didReceivePrivacyStatus:)])
          [delegate rtcBridge:self didReceivePrivacyStatus:status];
      });
      return;
    }
    if (action.type == ControlType::display_resolution_status ||
        action.type == ControlType::host_infomation) {
      const auto message = action.to_json();
      NSData *payload = [NSData dataWithBytes:message.data() length:message.size()];
      DispatchMain(^{
        if (![self isControllerGenerationActive:generation]) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didReceiveDisplayResolutions:)])
          [delegate rtcBridge:self didReceiveDisplayResolutions:payload];
      });
      if (action.type == ControlType::display_resolution_status) return;
    }
    if (action.type == ControlType::video_settings_status) {
      [self notifyVideoSettings:action.vs generation:generation];
      return;
    }
    if (action.type == ControlType::service_status) {
      const BOOL available = action.ss.available;
      NSString *stage = [NSString stringWithUTF8String:action.ss.interactive_stage];
      DispatchMain(^{
        if (![self isControllerGenerationActive:generation]) return;
        id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
        if ([delegate respondsToSelector:@selector(rtcBridge:didReceiveServiceAvailable:interactiveStage:)]) {
          [delegate rtcBridge:self didReceiveServiceAvailable:available interactiveStage:stage ?: @""];
        }
      });
      return;
    }
    if (action.type != ControlType::host_infomation) return;
    const BOOL supports_video_settings = action.i.supports_video_settings;
    const BOOL supports_text_input = action.i.supports_text_input;
    NSString *platform = [NSString stringWithUTF8String:
        crossdesk::HostPlatformName(action.i.platform)];
    NSString *host_name = [[NSString alloc]
        initWithBytes:action.i.host_name
               length:action.i.host_name_size
             encoding:NSUTF8StringEncoding];
    NSString *app_version = crossdesk::IsValidAppVersion(action.i.app_version)
        ? [NSString stringWithUTF8String:action.i.app_version] : nil;
    NSMutableArray<NSString *> *display_names =
        [NSMutableArray arrayWithCapacity:action.i.display_num];
    NSMutableArray<NSValue *> *display_sizes =
        [NSMutableArray arrayWithCapacity:action.i.display_num];
    for (std::size_t index = 0; index < action.i.display_num; ++index) {
      NSString *name = action.i.display_list && action.i.display_list[index]
          ? [NSString stringWithUTF8String:action.i.display_list[index]]
          : nil;
      [display_names addObject:name ?: @""];
      const int width = action.i.left && action.i.right
          ? std::max(action.i.right[index] - action.i.left[index], 0)
          : 0;
      const int height = action.i.top && action.i.bottom
          ? std::max(action.i.bottom[index] - action.i.top[index], 0)
          : 0;
      [display_sizes addObject:[NSValue valueWithCGSize:CGSizeMake(width,
                                                                  height)]];
    }
    crossdesk::FreeRemoteAction(action);
    DispatchMain(^{
      if (![self isControllerGenerationActive:generation]) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:
              @selector(rtcBridge:didReceiveHostName:appVersion:platform:displayNames:
                                  displaySizes:supportsVideoSettings:supportsTextInput:)]) {
        [delegate rtcBridge:self
            didReceiveHostName:host_name ?: @""
                    appVersion:app_version ?: @""
                      platform:platform
                  displayNames:display_names
                  displaySizes:display_sizes
         supportsVideoSettings:supports_video_settings
             supportsTextInput:supports_text_input];
      }
    });
    return;
  }

  if (source == kMouseStream) {
    RemoteAction action{};
    if (!action.from_json(std::string(data, size)) ||
        action.type != ControlType::cursor_state) {
      return;
    }
    const BOOL visible = action.cs.visible;
    const NSInteger shape = static_cast<NSInteger>(action.cs.shape);
    const BOOL position_update = action.cs.position_update;
    const BOOL position_valid = action.cs.position_valid;
    const float position_x = action.cs.x;
    const float position_y = action.cs.y;
    const float visual_offset_x = action.cs.visual_offset_x;
    const float visual_offset_y = action.cs.visual_offset_y;
    const NSInteger display_index = action.cs.display_id;
    const uint32_t sequence = action.cs.seq;
    DispatchMain(^{
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:
              @selector(rtcBridge:didReceiveCursorVisible:shape:
                                    positionUpdate:positionValid:x:y:
                                    visualOffsetX:visualOffsetY:displayIndex:
                                    sequence:)]) {
        [delegate rtcBridge:self
            didReceiveCursorVisible:visible
                              shape:shape
                     positionUpdate:position_update
                      positionValid:position_valid
                                  x:position_x
                                  y:position_y
                      visualOffsetX:visual_offset_x
                      visualOffsetY:visual_offset_y
                       displayIndex:display_index
                           sequence:sequence];
      }
    });
    return;
  }

  if (source == kClipboardStream) {
    if (size > kMaxClipboardBytes || std::memchr(data, '\0', size)) return;
    NSString *text = [[NSString alloc] initWithBytes:data
                                              length:size
                                            encoding:NSUTF8StringEncoding];
    if (!text) return;
    DispatchMain(^{
      if (![self isControllerGenerationActive:generation]) return;
      id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
      if ([delegate respondsToSelector:
              @selector(rtcBridge:didReceiveClipboardText:)]) {
        [delegate rtcBridge:self didReceiveClipboardText:text];
      }
    });
    return;
  }

  if (source == kFileFeedbackStream) {
    // 反馈帧的唯一消费者是引擎：偏移校验、终态判定、进度和收尾都归它，桥内
    // 不再维护 outgoing_files。发送已经在这条串行队列上，投递即可。
    const std::string acknowledgement(data, size);
    dispatch_async(_rtcQueue, ^{
      if (![self isControllerGenerationActive:generation] || !self->_transfer) return;
      self->_transfer->Ack(acknowledgement.data(), acknowledgement.size());
      [self wakeFilePump];
    });
    return;
  }

  if (source != kFileStream) return;
  // 数据块同样整块交给引擎：暂存目录、范围合并、反馈 ACK 和最终发布都在
  // _rtcQueue 上完成，随后的 Pump 把下一块或本段的 ACK 推进下去。
  const std::string chunk_bytes(data, size);
  dispatch_async(_rtcQueue, ^{
    if (![self isControllerGenerationActive:generation] || !self->_transfer) return;
    self->_transfer->Receive(chunk_bytes.data(), chunk_bytes.size());
    [self wakeFilePump];
  });
}

- (void)handleStats:(const MiniRtcNetTrafficStats *)stats
               mode:(MiniRtcTraversalMode)mode
         generation:(uint64_t)generation {
  if (!stats) return;
  const auto traffic = [](const MiniRtcInboundStats &inbound,
                          const MiniRtcOutboundStats &outbound) {
    return CrossDeskTrafficStats{inbound.bitrate, outbound.bitrate,
                                 inbound.loss_rate};
  };
  const CrossDeskNetworkStats snapshot{
      traffic(stats->video_inbound_stats, stats->video_outbound_stats),
      traffic(stats->audio_inbound_stats, stats->audio_outbound_stats),
      traffic(stats->data_inbound_stats, stats->data_outbound_stats),
      traffic(stats->total_inbound_stats, stats->total_outbound_stats),
      stats->rtt_ms, static_cast<BOOL>(stats->srtp_active),
      mode == MiniRtcTraversalMode::MiniRtcTraversalP2P ? CrossDeskTraversalModeDirect
      : mode == MiniRtcTraversalMode::MiniRtcTraversalRelay ? CrossDeskTraversalModeRelay
                                    : CrossDeskTraversalModeUnknown};
  NSString *decoder = [[NSString alloc]
      initWithBytes:stats->video_decoder
             length:strnlen(stats->video_decoder, sizeof(stats->video_decoder))
           encoding:NSUTF8StringEncoding];
  NSString *localEndpoint = [NSString stringWithUTF8String:stats->local_endpoint];
  NSString *remoteEndpoint = [NSString stringWithUTF8String:stats->remote_endpoint];
  DispatchMain(^{
    // Disconnect/reconnect can happen while this report waits on the UI queue.
    if (![self isControllerGenerationActive:generation]) return;
    id<CrossDeskRTCBridgeDelegate> delegate = self.delegate;
    if ([delegate respondsToSelector:
            @selector(rtcBridge:didUpdateNetworkStats:localDecoder:localEndpoint:remoteEndpoint:)]) {
      [delegate rtcBridge:self didUpdateNetworkStats:snapshot localDecoder:decoder ?: @""
            localEndpoint:localEndpoint remoteEndpoint:remoteEndpoint];
    }
  });
}

@end
