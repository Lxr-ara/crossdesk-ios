# UU远程 iOS 4.48.1 拆解报告

包：`0F676EBE-…-UU远程-流畅可靠，就是好用！_4.48.1-decrypted.ipa`（46.59 MB，177 个条目）
主二进制：`Payload/nrd-ios-client.app/nrd-ios-client`，47,701,136 字节，`cryptid=0`（已脱壳）

## 一、包结构

| 项 | 内容 |
| --- | --- |
| App bundle | `nrd-ios-client.app`（注意：**不是** `UU远程.app`，进程名与品牌名不一致） |
| 动态库 | `NGPush`（2.50 MB，推送）、`NGUFoundation`（2.26 MB，网易基础库）、`PurchaseKit`（0.86 MB，内购）、`StreamerCore`（0.03 MB，仅壳） |
| 资源 | `Assets.car` **25.1 MB**（590 个内嵌 PDF 矢量 + 位图）、`Alamofire.bundle`、`DeviceKit.bundle`、`FeedbackSDK.bundle`、两张 AppIcon |
| 本地化 | **没有** `.lproj` / `.strings`；`Base.lproj` 只有一个启动 storyboard |
| 其它 | 4 个 `.xcconfig`（Adhoc / Appstore / Appstore_TF / Debug，说明它有 TF、企业、商店多条发布通道）、`PrivacyInfo.xcprivacy`、`ngpush.data`、`default_plan.txt` |

**关键结论 1**：UI 文案不在包里。二进制里只有 332 个 `Control.*` 键名，没有对应的中英文文本——文案是服务端下发（与下文的 feature 表同一套机制）。所以“抄界面文字”只能抄 key 结构，抄不到文本。

## 二、二进制布局

| 段 | 大小 | 说明 |
| --- | --- | --- |
| `__TEXT,__text` | **33.0 MB** | 代码本体（占全文件 69%） |
| `__TEXT,__cstring` | 1.68 MB | C 字符串 |
| `__TEXT,__const` | 1.89 MB | 常量（含浮点阈值） |
| `__TEXT,__objc_methname` | 349 KB | ObjC 方法名 |
| `__TEXT,__swift5_reflstr` | 273 KB | Swift 反射字符串（9,481 条） |
| `__TEXT,__objc_classname` | 18.7 KB | **929 个 ObjC 类** |
| `__DATA,__objc_data` | 538 KB | 类元数据 |
| `__DATA,__bss` | 1.38 MB | 全局状态 |

链接系统框架（节选）：`AVFoundation / CoreMotion / VideoToolbox / Metal / AudioToolbox / ExternalAccessory / MobileCoreServices / Security / SystemConfiguration`。同时链接了 `libc++`、`libicucore`、`libz`，并在字符串里出现 `curl command could not be created` → **内置 libcurl**，对得上它的「远程终端 / CLI」功能。

## 三、功能开关表（62 项，最有价值的部分）

二进制里有一张自描述的功能表，每条带 `original_version`（首次上线版本）和 `final_version`（转正版本）。这就是产品自己的功能清单：

```
annotation_v1 4.35.0        annotation_v2 4.40~4.42
auto_unlock 1.5.2/4.39      controlled_update 2.5.0
custom_bitrate 3.4.1        delay_reason 3.1.3/4.6.100
desktop_display 1.0.0       device_gamepad 1.5.22
device_keyboard 1.5.22      device_lock 1.0.0/4.38
device_microphone 4.16.800  device_mouse 1.5.22
device_reboot 1.0.0         device_shutdown 1.0.0
device_task_manager 1.0.0   disconnect_lock 1.1.4/4.11
file_trans_v1..v4 2.5~3.13  follow_local 2.3.1  follow_remote 3.0.0
fps_90 1.0.0                fps_144 2.2.0
fps_super_screen 4.27.0     manual_super_screen 4.27.0
full_screen 1.0.0           manual_transfer 4.9.0
mouse_cursor 1.0.0          multi_screen 4.0.0/4.31
mumu_control 3.13.1         operation_guide 2.0.0
pad_touch 1.1.0             port_mapping 4.21.0/4.32
privacy_wallpaper 4.23      quality_normal 1.0.0
remote_app_list 4.18.0      remote_clipboard_v1..v5  1.0.12~4.43
remote_connection_cli 4.23  remote_connection_cli_v2 4.39
remote_mute 1.1.0           screen_chroma 2.4.0   screen_hdr 4.2.2
screen_orientation 4.16     screen_privacy 1.2.5  screen_privacy_enhanced 4.4.0
screen_super 4.2.3          screen_virtual 1.2.1/4.35
screen_zoomscale 3.4.0      simulate_mouse 3.1.37
smart_mouse 3.2.1/4.18      virtual_mouse 1.1.0   virtual_plan 1.0.0
voice_setting 1.0.0         watch_unlock 4.3.0/4.39
```

从这张表能直接读出三件事：

1. **演进顺序**：`virtual_mouse`(1.1) → `smart_mouse`(3.2) → `simulate_mouse`(3.1.37) → `analogMouse*` 资源，鼠标模式至少迭代了四代。
2. **它的强项**：`fps_144`、`screen_super`（超级屏）、`screen_virtual`、`screen_hdr`、`screen_chroma`（4:4:4）、`multi_screen`、`port_mapping`、`remote_connection_cli`。
3. **它的依赖**：`mumu_control`、`virtual_plan`、`PurchaseKit` —— 模拟器与订阅，属于生态绑定。

## 四、交互层（类名即实现）

手势识别器（ObjC 类名，可直接对上行为）：

```
TouchModeGestureRecognizer          AnalogMouseFoldGestureRecognizer
AnalogMouseMoveGestureRecognizer    AnalogMouseScrollGestureRecognizer
```

日志里的实参名（能反推算法）：

```
handleTouchMouseWheelPan(…)      gestureVelocityY=
began scale:  … changed scale:  … ended scale:  …    ← 捏合按阶段打点
forwardSingleTapMouseEvent(…)    ← 单击转鼠标事件
disableMousePanGesture / enableMousePanGesture / enableSelectionPanGesture
hiftBypassesMouseReporting(for:)  ← Shift 绕过鼠标上报（终端选择文本）
```

**它有两套指针**——`cursor_*` 自绘光标 + `analogMouse_*` 模拟鼠标（鼠身 108×119、滚轮键 36、指针 24，之前已量），并有一张 `Control.AnalogMouse.Guide` 引导页（首次使用教学）。

键盘层：

```
FullKeyboardButton                 ← 底栏「电脑键盘」按钮
combineKeyboardViewController      ← 组合键面板
ShortcutEditViewController         ← 快捷键编辑
keyboardModeAlt / keyboardEnhancementFlags / handleKittyKeyboardProtocol
```

最后一行说明它**支持 Kitty 键盘协议**（终端增强键位），配合 libcurl，终端是认真做的，不是摆设。

## 五、会话控制面板（`Control.Panel.*`，11 项）

```
exitGameMode   mouseMode   networkStatus   remoteVoice   screenRotation
screenSettings selectDisplay   selectScreen   … 
```

对得上你说的底栏结构：`screenRotation`（旋转屏幕）确实是面板项，`selectDisplay/selectScreen`（切换显示器）、`remoteVoice`（声音）、`mouseMode`（鼠标模式）。

配套的还有：
- `Control.Guide.*`（引导：`doubleScroll` 双击滚动、`doubleZoom` 双击缩放、`mouseOperationTitle` 鼠标操作说明、`quickSwitch` 快捷切换）
- `Control.FullScreen.*`（9 项，含 `mouseHint`）
- `Control.PictureSetting.*`（12 项：`normal / highDefinition / bluray / customBitrate` 及各自 `…N` 变体）
- `Control.Pasteboard.*`（9 项，剪贴板同步）
- `Control.GameMode.Menu.*`（18 项：`angleSensitivity`、`padScrollReverse`、`scrollReverse`、`gamepadShock`、`scale`、`hideKey`）
- `Control.Auth.*`（47 项，含 `OpenFaceTip`、`AutoUnlockWin`、`InputCode`）
- `Control.RemoteInfo.*`（28 项：`EncryptionChannel`、`SecurityLan`、`SecurityServiceTransit`、`clientIp`、`capture`、`bandWidth`）

行为日志（取自二进制，描述点击语义）：

```
用户点击展开所有窗口 / 观看模式下点击展开所有窗口，忽略操作
用户点击显示桌面   / 观看模式下点击显示桌面，忽略操作
点击了观看模式：…  关闭观看模式，收敛退出批注模式
屏幕旋转点击 / 旋转快照 / 跳过旋转等待
控制页-麦克风-麦克风开启-已启动音频 / 麦克风关闭-已停止音频
```

**关键结论 2**：它把「观看模式」做成**总开关**，开启后底栏那几个按钮仍然响应但被忽略（不是隐藏）。我们这轮做的观看模式闸门方向一致，但我们的按钮不会打日志也不会忽略点击——如果要一模一样，应该改成"点击后提示已忽略"。

## 六、和我们的对照

| 能力 | UU | 我们 | 差在哪 |
| --- | --- | --- | --- |
| 自绘光标 | `cursor_*` 128 pt 矢量 + `mouse_cursor` 开关 | 14 种形状，64 px 素材，两端同源表 | 素材是位图，形状少 4 种（person/pin/no/arrow_up） |
| 模拟鼠标 | 四代演进，鼠身+左右中键+滚轮+引导页 | 已复刻鼠身/中键/滚轮 | 缺引导页（`AnalogMouse.Guide`）、缺 fold/guide 状态 |
| 底栏 | `Control.Panel.*` 11 项 + 工具页动态构建 | 5 组固定 | 工具页应**动态构建**（日志里 `不构建工具列表`、`刷新工具列表`） |
| 键盘 | 系统键盘 + 电脑键盘 + 组合键 + 快捷键编辑 + Kitty 协议 | 系统键盘 + 快捷键 + 电脑键盘 | 缺组合键编辑、缺终端增强键位 |
| 观看模式 | 总开关，点击被忽略并打点 | 主机侧闸门，输入直接丢弃 | 行为对齐，缺"忽略提示" |
| 画质 | `PictureSetting` 四档 + 自定义码率 + HDR + 4:4:4 | 三档 + 自动降级 | 缺自定义码率、HDR、4:4:4 |
| 屏幕 | 超级屏/虚拟屏/多屏/旋转/分辨率 | 显示器切换、分辨率 | 缺旋转、超级屏 |
| 终端 | libcurl + PTY + Kitty 协议 | 无 | 大工程 |
| 文件 | 四代传输 + 相册 + 剪贴板 v1~v5 | 队列/续传/相册 | 缺剪贴板版本协商（我们只有单向发送） |
| 隐私 | 隐私屏 + 增强版 + 屏保壁纸 | 隐私屏 | 缺 `privacy_wallpaper` |
| 安全 | 人脸解锁、自动解锁、设备锁、断开锁 | 密码连接 | 缺生物识别/自动解锁 |

## 七、下一步能立刻做的（都有证据支撑）

1. **底栏工具页动态化**：按 `工具列表` 动态构建，而不是写死 5 组（日志已证明它是动态的）。
2. **观看模式的"忽略提示"**：点击后 toast「观看模式下已忽略操作」，与它一致。
3. **`analogMouse` 引导页**：`Control.AnalogMouse.Guide.title/confirm` 两个键，配我们的鼠身图即可。
4. **组合键面板**：`combineKeyboardViewController` 的存在说明是"多键同时按下"的面板，不是快捷键列表；我们可以照这个语义重做键盘页。
5. **剪贴板双向**：它有 v1~v5 五代协商，我们目前只有发送；至少补上"接收远端剪贴板"。
6. **自定义码率**：`custom_bitrate`，主机端我们已有 `VideoSettings`，加一个 slider 就能用。

---

## 附：产物

- `uu_dec/feature_flags.json` — 62 项开关与版本
- `uu_dec/control_keys.txt` — 332 个 `Control.*` 键
- `uu_dec/objc_classes.txt` — 928 个 ObjC 类
- `uu_dec/strings_ascii.txt` / `strings_utf8.txt` — 207,834 / 6,306 条字符串
- `uu_dec/behaviour_logs.txt` — 控制面板行为日志
- `uu_dec/labels_zh.txt` — 5,800 条中文串（多为日志）
