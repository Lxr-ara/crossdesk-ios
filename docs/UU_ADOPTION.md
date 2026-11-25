# 从 UU远程 取用与舍弃（汇总）

依据：`com.netease.uuremote` iOS 4.48.1 包（`Payload/nrd-ios-client.app`，主二进制
`cryptid=1` 被 FairPlay 加密，资源可读）+ Windows 端 `GameViewer.exe`（106 个内嵌 PNG、
字符串表）。每条结论后面写清"凭什么"。

## 一、取用（已落地）

| 能力 | UU 那边的证据 | 我们的实现 |
| --- | --- | --- |
| 自绘光标 | 14 张 64×64 PNG，箭头占 tile 高 88%（逐像素 alpha） | 桌面 + 手机同一张几何表（`remote_cursor_metrics.h` → 两端生成） |
| 光标家族 | `cursor_arrow/ibeam/hand/help/cross/no/person/pin/busy/size_*`，画布 128 pt | 14 种形状，指尖/中心热区按实测 |
| 模拟鼠标 | `analogMouse*`：鼠身 108×119、滚轮 36、指针 24、scrollMode 116 | `AnalogMousePad.swift` 同比例复刻 |
| 中键 | `analogMouse_middle_*`、`cursorMode_icon_middle_click` | 线协议 5/6 + `MOUSEEVENTF_MIDDLEDOWN/UP` + 滚轮键按下即中键 |
| 底部控制条 | `icon_tool_*` 24 pt 画布、`icon_tool_hover_*` 40 pt 目标 | 底栏 5 组：所有窗口 / 显示桌面 / 键盘 / 操作 / 工具；图标 24、目标 40、条高 56、间距 8、圆角 28 |
| 所有窗口 / 显示桌面 | `icon_tool_show_all_windows`、`icon_tool_back_desktop` | 底栏按钮发 Win+Tab / Win+D 组合键 |
| 键盘分组 | `icon_tool_keyboard`、`icon_keyboard_*`、`icon_tool_win_command`/`mac_command` | 键盘面板 + 快捷键面板两入口 |
| 观看模式 | `icon_tool_watch_mode` | 主机侧输入闸门 `remote_input_gate.h`（顺带修掉一个 4 处写入、0 处读取的死开关） |
| 画质档位 | `zoom_scale=`、`custom_zoom=`、`view_mode_zoom` | 流畅/高清/超清 + 弱网自动降级 |
| 文件传输 | `file_transfer_*` 图标族、`file_transfer_manifest result=accepted` | 队列、逐项暂停/继续/重试、限速、覆盖策略、续传 |
| 远端文件操作 | `file_transfer_directory_create` 等 | 8 个 op：打开/显示/重命名/删除/新建/属性/刷新/清理临时文件 |
| 隐私屏 | `icon_tool_private_screen`、`icon_tool_hide` | 已有，Windows 端黑屏 + 锁屏解锁 |
| 相册 | `gallery`/`photo` 资源族 | 三端相册 + 上传/接收列表 |
| 分辨率/显示器 | `icon_tool_display`、`icon_tool_screen` | 底栏"操作 → 切换显示器" |

## 二、暂缓（值得做，但要动协议或宿主）

| 能力 | 为什么暂缓 |
| --- | --- |
| 远程终端 | 需要在会话里新开一条 shell 通道（PTY 转发 + 编码），不是 UI 活 |
| 端口映射 | 需要中继/服务端配合，我们目前是 P2P 直连 |
| 远程开机 WOL | 需要一台常在线设备发魔术包 |
| 无线副屏 / 超级屏 | 需要虚拟显示器驱动 |
| 宏按键 / 按键映射 / 手柄 | 需要映射引擎 + 配置库下发 |
| 麦克风上行 | 需要音频采集通道（当前只有下行播放） |
| 小窗模式 PiP | 桌面窗口管理改动（置顶 + 缩略显示） |
| 免密连接 / 自定义验证码 | 认证交互改造，牵涉安全模型 |
| 设备收藏 | 设备库要加字段与迁移 |
| 客户端安全锁 | 本地密码锁 UI + 存储 |
| CLI | 独立入口，工作量中等 |
| 标注/白板 | `icon_tool_annotation*` 已定位，需要一套绘制层与同步 |
| 旋转屏幕 | 主机端没有这条 op；底栏"操作"里已如实标为「主机暂未支持」并禁用 |

## 三、舍弃（明确不做）

| 项 | 理由 |
| --- | --- |
| 云电脑 / 云游戏（`cloud_pc_*`、`game_queue`、`mumu_*`） | 绑定网易自家云与模拟器生态，与"远程控制"本体无关 |
| 云盘 / 云存档（`icon_tool_cloud_disk`） | 依赖网易云服务 |
| 网易账号体系 / 一键登录 | 绑定自家账号，我们是自建设备信任 |
| 会员 / 订阅 / 支付 | 商业化链路，与功能无关 |
| 游戏手柄云配置库 | 需要云端配置下发，且定位是串流平台 |
| 推荐位 / 广告素材（`discover_*`） | 运营素材，无功能价值 |
| 壁纸/皮肤（`wallpaper_*`） | 纯装饰 |

## 四、这条原则怎么执行

1. **能指到字节的才叫"复刻"**：数值取自 UU 自己的资源（尺寸、α 包围盒、画布），
   不取自观感。
2. **读不到的就不假装**：主二进制被 FairPlay 加密，代码级常量（手势阈值、光标 pt）
   读不出来，这类取值在 `docs/IOS_ASSUMPTIONS.md` 里标为「自研」。
3. **两端同源**：几何进头文件，桌面 C++ 直接 include，iOS 由脚本生成，改一处两端跟随。
