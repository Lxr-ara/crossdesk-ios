#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, CrossDeskSignalState) {
  CrossDeskSignalStateConnecting = 0,
  CrossDeskSignalStateConnected,
  CrossDeskSignalStateFailed,
  CrossDeskSignalStateClosed,
  CrossDeskSignalStateReconnecting,
  CrossDeskSignalStateServerClosed,
  CrossDeskSignalStateTLSCertificateError,
  CrossDeskSignalStateCredentialUnavailable,
};

typedef NS_ENUM(NSInteger, CrossDeskConnectionState) {
  CrossDeskConnectionStateConnecting = 0,
  CrossDeskConnectionStateConnected,
  CrossDeskConnectionStateGathering,
  CrossDeskConnectionStateDisconnected,
  CrossDeskConnectionStateFailed,
  CrossDeskConnectionStateClosed,
  CrossDeskConnectionStateIncorrectPassword,
  CrossDeskConnectionStateNoSuchID,
  CrossDeskConnectionStateRemoteUnavailable,
};

typedef NS_ENUM(NSInteger, CrossDeskPointerAction) {
  CrossDeskPointerActionMove = 0,
  CrossDeskPointerActionLeftDown,
  CrossDeskPointerActionLeftUp,
  CrossDeskPointerActionRightDown,
  CrossDeskPointerActionRightUp,
  CrossDeskPointerActionMiddleDown,
  CrossDeskPointerActionMiddleUp,
};

typedef NS_ENUM(NSInteger, CrossDeskVideoAdaptationPolicy) {
  CrossDeskVideoAdaptationPolicyFrameRatePriority = 0,
  CrossDeskVideoAdaptationPolicyQualityPriority,
  CrossDeskVideoAdaptationPolicyBalanced,
};

@class CrossDeskRTCBridge;

typedef NS_ENUM(NSInteger, CrossDeskTraversalMode) {
  CrossDeskTraversalModeUnknown = 0,
  CrossDeskTraversalModeDirect,
  CrossDeskTraversalModeRelay,
};

typedef struct {
  uint32_t inboundBitrate;
  uint32_t outboundBitrate;
  float lossRate;
} CrossDeskTrafficStats;

/// Value snapshot: no MiniRTC callback-owned memory crosses the main queue.
typedef struct {
  CrossDeskTrafficStats video;
  CrossDeskTrafficStats audio;
  CrossDeskTrafficStats data;
  CrossDeskTrafficStats total;
  double rttMilliseconds;
  BOOL srtpActive;
  CrossDeskTraversalMode traversalMode;
} CrossDeskNetworkStats;

@protocol CrossDeskRTCBridgeDelegate <NSObject>
@optional
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didChangeSignalState:(CrossDeskSignalState)state;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didChangeIdentityStorageError:(BOOL)hasError;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didChangeConnectionState:(CrossDeskConnectionState)state
                    remoteID:(NSString *)remoteID;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didProvisionIdentity:(NSString *)identity;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceivePixelBuffer:(CVPixelBufferRef)pixelBuffer
                    width:(NSInteger)width
                   height:(NSInteger)height
            captureUptime:(NSTimeInterval)captureUptime;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveHostName:(NSString *)hostName
            appVersion:(NSString *)appVersion
              platform:(NSString *)platform
          displayNames:(NSArray<NSString *> *)displayNames
          displaySizes:(NSArray<NSValue *> *)displaySizes
 supportsVideoSettings:(BOOL)supportsVideoSettings
     supportsTextInput:(BOOL)supportsTextInput;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveVideoSettingsQuality:(NSInteger)quality
                        frameRate:(NSInteger)frameRate
                       preference:(NSInteger)preference
                        requestID:(uint32_t)requestID
                         accepted:(BOOL)accepted;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceivePresence:(NSDictionary<NSString *, NSNumber *> *)presence
               snapshot:(BOOL)snapshot;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveDisplayResolutions:(NSData *)message;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didFailDisplayResolutionRequest:(uint32_t)requestID;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveAnnouncementMessage:(NSData *)message;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didFailAnnouncementRequest:(NSString *)requestID;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveCursorVisible:(BOOL)visible
                      shape:(NSInteger)shape
             positionUpdate:(BOOL)positionUpdate
              positionValid:(BOOL)positionValid
                          x:(float)x
                          y:(float)y
              visualOffsetX:(float)visualOffsetX
              visualOffsetY:(float)visualOffsetY
               displayIndex:(NSInteger)displayIndex
                   sequence:(uint32_t)sequence;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveAudioPCM:(NSData *)pcmData;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveClipboardText:(NSString *)text;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveServiceAvailable:(BOOL)available
              interactiveStage:(NSString *)stage;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didSendSecureAttention:(BOOL)success;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceivePrivacyStatus:(NSData *)data;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didSendPrivacyRequest:(uint32_t)requestID success:(BOOL)success;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceivePhotoMessage:(NSData *)payload;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didUpdateFileTransfer:(NSString *)fileName
                 progress:(double)progress
                  sending:(BOOL)sending;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveFileAtURL:(NSURL *)fileURL;
/// 接收侧精确字节数（传输引擎的 receive 回调）。`sent`/`total` 是引擎已经
/// 连续落盘的字节数与文件总长度，接收完成时两者相等。发送侧仍然只走
/// `didUpdateFileTransfer:progress:sending:`。
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didReceiveFileProgress:(NSString *)fileName
                      sent:(uint64_t)sent
                     total:(uint64_t)total
                   sending:(BOOL)sending;
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didUpdateNetworkStats:(CrossDeskNetworkStats)stats
             localDecoder:(NSString *)decoder
            localEndpoint:(NSString *)localEndpoint
           remoteEndpoint:(NSString *)remoteEndpoint
    NS_SWIFT_NAME(rtcBridge(_:didUpdateNetworkStats:localDecoder:localEndpoint:remoteEndpoint:));
- (void)rtcBridge:(CrossDeskRTCBridge *)bridge
    didUpdateRemoteVideoEncoder:(NSString *)encoder;
@end

/// Objective-C++ boundary around the native MiniRTC C API.
///
/// All MiniRTC ownership and calls are serialized internally. Delegate methods
/// are delivered on the main queue so SwiftUI never touches callback threads.
@interface CrossDeskRTCBridge : NSObject
+ (nullable NSString *)directEndpoint:(NSString *)input NS_SWIFT_NAME(directEndpoint(_:));
+ (nullable NSDictionary<NSString *, NSString *> *)deviceTrustInfoForDeviceID:(NSString *)deviceID;

@property(nonatomic, weak, nullable) id<CrossDeskRTCBridgeDelegate> delegate;

// Uses the same release/patch ordering as the desktop controller.
+ (nullable NSString *)availableUpdateForAppVersion:(NSString *)appVersion
                                       releaseJSON:(NSData *)releaseJSON
    NS_SWIFT_NAME(availableUpdate(appVersion:releaseJSON:));

- (void)configureWithSignalHost:(NSString *)host
                     signalPort:(NSInteger)signalPort;
- (void)setVideoAdaptationPolicy:(CrossDeskVideoAdaptationPolicy)policy;
- (void)sendVideoSettingsWithQuality:(NSInteger)quality
                          frameRate:(NSInteger)frameRate
                         preference:(NSInteger)preference
                          requestID:(uint32_t)requestID
    NS_SWIFT_NAME(sendVideoSettings(quality:frameRate:preference:requestID:));
- (void)sendDisplayResolution:(NSInteger)displayID
                  displayKey:(NSString *)displayKey
                       width:(NSInteger)width
                      height:(NSInteger)height
                   requestID:(uint32_t)requestID
    NS_SWIFT_NAME(sendDisplayResolution(displayID:displayKey:width:height:requestID:));
- (void)requestPresenceForRemoteIDs:(NSArray<NSString *> *)remoteIDs
                         subscribe:(BOOL)subscribe
    NS_SWIFT_NAME(requestPresence(remoteIDs:subscribe:));
- (void)invalidatePresence;
- (void)sendAnnouncementRequest:(NSData *)request requestID:(NSString *)requestID
    NS_SWIFT_NAME(sendAnnouncementRequest(_:requestID:));

- (void)connectToRemoteID:(NSString *)remoteID password:(NSString *)password
      displayResolutions:(NSData *)displayResolutions;
- (void)disconnect;
// Close both session and signaling peers, including automatic reconnection.
- (void)stopNetworking;
- (void)requestKeyFrame;

- (void)sendPointerAtX:(float)x
                     y:(float)y
                action:(CrossDeskPointerAction)action
    NS_SWIFT_NAME(sendPointer(x:y:action:));
- (void)sendScrollX:(float)x y:(float)y deltaX:(NSInteger)deltaX
              deltaY:(NSInteger)deltaY;
- (void)sendWindowsKeyCode:(NSUInteger)keyCode isDown:(BOOL)isDown;
- (void)sendKeyChord:(NSUInteger)keyCode modifiers:(NSArray<NSNumber *> *)modifiers;
- (void)sendText:(NSString *)text;
- (void)sendCommittedText:(NSString *)text;
- (void)switchToDisplay:(NSInteger)displayIndex;
- (void)setAudioEnabled:(BOOL)enabled;
- (void)sendClipboardText:(NSString *)text;
// 发送入口。引擎接管分块、ACK、限速与暂停；签名保持不变，Swift 无需改动。
- (void)sendFileAtURL:(NSURL *)fileURL;
// 跨会话续传的发送入口：`resumeOffset` 是本机认定对端已经拥有的前缀长度，
// 本次会话的第一块就从该偏移开始发（线格式不变，偏移本来就在块头里）。
// 返回值只表示请求是否被立刻接受；真正的打开文件失败仍走进度回调（-1）。
- (BOOL)sendFileAtURL:(NSURL *)fileURL
         resumeOffset:(uint64_t)resumeOffset
    NS_SWIFT_NAME(sendFile(at:resumeOffset:));
// 发送限速（KiB/s，<= 0 表示不限速）与暂停。两者都是会话级设置，重连后按最后
// 一次设定值恢复。
- (void)setFileTransferRateKbps:(NSInteger)kbps;
- (void)setFileTransferPaused:(BOOL)paused;
- (BOOL)fileTransferPaused;
// Photo album RPC (`file_browser` version 1) over the control stream.
- (void)sendPhotoMessage:(NSData *)payload
    NS_SWIFT_NAME(sendPhotoMessage(_:));
- (void)sendSecureAttentionSequence;
- (void)sendPrivacyCommand:(BOOL)enable
                      lock:(BOOL)lock
           blockLocalInput:(BOOL)blockLocalInput
                 requestID:(uint32_t)requestID;

@end

NS_ASSUME_NONNULL_END
