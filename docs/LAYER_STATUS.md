# 分层交付状态（对齐 UU远程）

> 目标：**除连接方式（自建信令 / 9 位 ID + 密码 / TURN）外，全部对齐 UU远程**。
> 流程：每层 `实现 → 审核员自测（PASS/FAIL + 证据）→ 修复 → 编译/测试/部署`。
> 本文件是唯一进度源；审核结论必须附命令与输出片段，不接受口头声明。

> **维护范围变更（2026-11-22）**：Web 客户端（crossdesk-web-client/）停止维护，8443 服务已停。
> 代码保留作参考，不再接受新功能；手机端对齐改由 **iOS 原生 App** 承担。
> 因此 L1 的手势/工具栏、L3 的手机长按菜单，后续都只在 iOS 上维护。

## 层清单

| 层 | 内容 | 负责面 | 实现 | 自测 | 修复 | 部署 |
| --- | --- | --- | --- | --- | --- | --- |
| L1 | 手势全集（单击/长按右键/双击/双指滚轮/双指缩放/三指呼出）+ UU 式悬浮工具栏 + 文件面板入口 | `crossdesk-web-client` | 完成 | 第 3 轮 PASS-WITH-NITS | 2 轮返工已修 | 已上线（静态资源，服务端已吐新文件） |
| L2 | 手机端传输面板（队列/并发/全部开始暂停继续清除/per-item 暂停重试/限速/覆盖策略/类型图标/排序/汇总） | Web `transfer_panel.js` + 宿主 3 条 RPC + iOS | Web + 宿主完成 | 待审 | — | 桌面已编译部署，Web 已上线，iOS 待做 |
| L3 | 远端文件操作菜单（mkdir/rename/delete/stat/open/reveal/temp_clear）桌面右键 + 手机长按 | `file_browser_window.cpp` / web | 未开始 | — | — | — |
| L4 | 画质档位（流畅/高清/超清 + 自动降级） | `config_center` + 各端 UI | 未开始 | — | — | — |
| L5 | 收口：冲突询问弹窗 + 孤儿暂存巡检 + 文档/交付包同步 | 	ools/file_transfer.* + conflict_dialog_host | 完成 | 第 2 轮 FAIL→已修 | 超时唤醒 / 前缀截断 / 收尾误删已修 | 桌面已编译 |

## 审核结论（按轮次留档）

### L1 · 第 1 轮：FAIL
- 阻塞 1：`glideWheel` 的惯性定时器没被跟踪，`cancelLocalInput()` 之后仍会往通道里灌滚轮（实测甩手后 4 格）；
- 阻塞 2：捏合/滚动判定用 30% 跨度比例，真实的双指捏合（300→340 px）被判成滚动，本地缩放完全失效；
- 阻塞 3：工具栏「文件」按钮判 `files?.browser`（传输会话永远存在），相册分支是死代码。
- 修复：定时器进 `state.glideTimers` 并由 `cancelLocalInput()` 清；判定改成「两个增量互相比较」；文件链按 `browser.supported` 逐级降级。

### L1 · 第 2 轮：FAIL
- 阻塞 1：`accX` 没初始化 → 横向滚轮 `NaN`，`wheel_horizontal`(flag 8) 永不发出；
- 阻塞 2：修过头，惯性链变成单发，甩手只滚一格；
- 新发现：12 px 绝对跨度阈值会让「60 px 竖滑 + 15 px 手指漂移」判成捏合（实测 0 滚轮）。
- 修复：补 `accX`/`velocity`；惯性用 `schedule()` 逐跳重挂并保留停止标志；判定改为 `TOUCH_GESTURE_DOMINANCE`（两个增量相差 1.2 倍才算定案，否则继续等）。

### L1 · 第 3 轮：PASS-WITH-NITS
- 事实：横向 60 px → 4 次 `flag 8`、0 次 `flag 7`、`scale=1`；松手 → 2 次惯性滚轮，`cancelLocalInput()` 后 0 次；reduced-motion 下 0 次；
- 判定：60 px 竖滑 + 15 px 漂移 → `scroll` / `scale=1` / 3 次 `flag 7`；40 px 跨度 + 6 px 漂移 → `zoom` / `scale=1.2` / 0 次滚轮；
- 回归：`node --check` ×3 通过，`node --test` 54/54 通过（新增 8 条假时钟用例，含「取消后不再滚」「三指只触发一次」「reduced-motion 无惯性」）；
- 遗留 nit：新起两指手势不会中断上一次的惯性尾巴（只有 `cancelLocalInput()` 会）；`close()` 有 3 行缩进不齐；`revealAfterMenu` 依赖后续 `sync()` 落地。

### L2 · 宿主侧（3 条新 RPC）
- `transfer_list` / `transfer_control` / `transfer_settings` 走同一个 version 1 信封，控制器不依赖 GUI，宿主通过 `FileBrowser::SetTransferControl` 注入；
- 逐项 id 只能走 `file_id`：`id` 是信封序号，web 端 `request()` 会覆盖它（第 1 版实现接受 `id` 被测试当场抓出，已改成只认 `file_id`）；
- `cancel` 作为第九个动作并入词表（与 `op:"cancel"` 同一条引擎路径），`kOps` 18 / `kFileManagementOps` 11；
- 桌面侧新增 `GuiTransferControl`（`apps/desktop/src/gui/features/file_transfer/transfer_control_adapter.h`），在 `GuiRuntime` 构造里注入，`GuiRuntime` 早于浏览器析构；
- 证据：`crossdesk_file_browser_transfer_ops_test` 通过（词表闭合、八个+取消动作各到达一次、非法请求零落地、GET 不写、端点接受、越界夹取、路径零泄露），全套 9 个测试目标全绿，`xmake b crossdesk` 成功。

## 审核员自测清单（每层必跑）
固定命令（Windows，仓库根 `crossdesk/`）：

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
xmake b crossdesk                       # 桌面 UI+客户端必须 build ok
foreach ($t in 'crossdesk_wire_test','crossdesk_photo_test','crossdesk_transfer_rate_test',
               'crossdesk_transfer_queue_test','crossdesk_file_browser_ops_test',
               'crossdesk_transfer_resume_test','crossdesk_file_browser_resume_test',
               'crossdesk_transfer_resume_handshake_test') { xmake r -y $t }
node --check <每个改动的 js>            # web 层
slint-compiler <改动的 .slint>          # UI 层
curl -k https://192.168.0.104:8443/<改动资源>   # 服务端是否吐出新内容
```

功能项（按层）：

- **L1**：单击=左键且两指手势期间不误触发；长按 450ms=右键；双击=双击；双指上下滑=滚轮（向上向上滚）；双指捏合=缩放且不与滚动抢手势；三指轻点呼出/收起工具栏；工具栏 5s 自动隐藏、隐藏时 `pointer-events:none`、不遮挡画面；safe-area 不被刘海/小白条盖住。
- **L2**：队列并发上限 2（可设 1–4）；全部开始/暂停/继续/清除/清除已完成；per-item 暂停/继续/重试；限速档位生效（日志 `Resume`/速率观察）；覆盖策略三档行为；类型图标 16 类映射；列排序三列双向；批量汇总数字与行一致。
- **L3**：七个 op 在 UI 有入口且失败有错误提示；路径逃逸被拒；`temp_clear` 只清 `.crossdesk-*`。
- **L4**：三档切换即时生效；弱网自动降级有日志证据；切档不重连。
- **L5**：同名冲突弹窗可选跳过/覆盖/改名；孤儿暂存与侧车被巡检清理；文档与交付包一致。

## 交付物

- 桌面：`dist/CrossDesk-1.7.6-photo-win-x64.zip` + `%LOCALAPPDATA%\Programs\CrossDeskPhoto\crossdesk.exe`
- Web：`https://192.168.0.104:8443/`（`serve_web_client.py`）
- iOS：`C:\Users\Mayn\Desktop\CrossDesk-交付包\CrossDesk-iOS-1.7.6-photo-unsigned.ipa`（CI 仓库 `Lxr-ara/crossdesk-ios`）

## L4 / L5 审核留档

### 第 1 轮：FAIL（4 条阻塞）
1. **空闲流误降级**：ReceivedVideoStats.Get() 在最后一帧超过 1 秒时把 fps 置 0，而降级机只看 fps，于是「暂停/隐私屏/对端没发流」和「全部丢帧」无法区分，一条静止的会话会每 ~18 秒掉一档，一路掉到流畅且永不回升。
   修法：判据改成「这一拍有没有新包进来」——用 ideo_inbound_stats.rtp_packet_count 与 uto_degrade_last_bytes_ 比较，没有新包就不喂样本。
2. **Web 画质页「不存在」**：审核在仓库里找 crossdesk-web-client/，实际它在工作区根目录（仓库的兄弟目录）。已确认三档按钮、VIDEO_TIERS、单击一条消息都在，服务端 data-preset 命中 3 次。
3. **冲突弹窗超时不唤醒下一个等待者**：10 分钟超时分支复位了 pending 却没有 
otify_all()，第二个接收线程会永远停在条件变量上。已补。
4. **续传前缀会截断用户已有文件**：PersistResumePrefix 无条件写 <接收目录>/<最终名>，一个分块（4 MiB 步长）就把同名旧文件清空，覆盖策略的「跳过/重命名」在弹窗出现前就已经失效；而新测试用 SetResumeEnabled(false) 把它绕过去了。
   修法：目标名被占用时前缀改写到 .crossdesk-prefix-<name>，DropResumeRecord 两种形态都清，ResumePrefixBytes 两种形态都认。

### 第 2 轮证据（同一套 12 个测试目标）
`
crossdesk_wire_test                       ok
crossdesk_photo_test                      ok
crossdesk_transfer_rate_test              ok
crossdesk_transfer_queue_test             ok
crossdesk_file_browser_ops_test           ok
crossdesk_transfer_resume_test            ok
crossdesk_file_browser_resume_test        ok
crossdesk_transfer_resume_handshake_test  ok
crossdesk_file_browser_transfer_ops_test  ok
crossdesk_video_quality_test              ok
crossdesk_orphan_sweep_test               ok
crossdesk_overwrite_conflict_test         ok
xmake b crossdesk -> build ok
localization/generate.py --check -> 1056 strings x 10 verified
`

### L5 第 2 轮追加（同一轮复验又发现两条）
1. **超时唤醒没落地**：上一轮的补丁是空操作（文件里别处已有 
otify_all，守卫把它跳过了）。实测：第一个等待者 600 s 超时返回，第二个等待者到 791 s 仍未返回。现在超时分支里显式 
otify_all()。
2. **收尾误删最终文件**：DropResumeRecord 无条件删除 <接收目录>/<name>，而 resume 目录就是接收目录，于是「单块接收」会把刚收到的文件删掉、「跳过」会把用户的旧文件删掉、多块新文件还会被改名成 
ame (1).ext。修法：前缀一律写旁路名 .crossdesk-prefix-<name>（一次也不写最终名），并记录本次写过的路径，收尾只删自己那份；发布前撤掉占位的旁路副本。
3. 新增端到端测试 crossdesk_resume_receive_e2e_test（resume 保持默认开启）：单块/多块/跳过/覆盖/重命名五种落盘结果 + 中断后持久态（侧车可解析、前缀恰好是第一块、最终名仍空闲）。对修复前的适配器跑同一份测试会红 13 条，说明它不是空转。

### 13 个测试目标（最终）
`
crossdesk_wire_test / crossdesk_photo_test / crossdesk_transfer_rate_test /
crossdesk_transfer_queue_test / crossdesk_file_browser_ops_test /
crossdesk_transfer_resume_test / crossdesk_file_browser_resume_test /
crossdesk_transfer_resume_handshake_test / crossdesk_file_browser_transfer_ops_test /
crossdesk_video_quality_test / crossdesk_orphan_sweep_test /
crossdesk_overwrite_conflict_test / crossdesk_resume_receive_e2e_test
-> 13/13 通过，xmake b crossdesk -> build ok
`

### 已知缺口（不阻塞交付，下一轮补）
- **跨进程续传的接收半程**：持久态（侧车 + 旁路前缀）已经可靠落盘，但「新进程拿着这条记录把剩下的块接上」还没有端到端用例，且中断点由接收方销毁触发时引擎会清掉暂存（真实断链不会），需要一条模拟链路中断而不是销毁接收方的用例。
- **询问策略默认关闭**：	ransfer_overwrite_policy 默认 2（自动重命名），弹窗只在用户改成「询问」时出现；选项文案目前叫「跳过」，看不出它是开启弹窗的开关，下一步要把三个选项改成「自动重命名 / 覆盖 / 每次询问」。
- **自动降级的空闲窗口记账**：空闲期间 AutoDegrade 的弱信号计时不清零，一次停顿可能被算进 3 秒持续窗。

### L3 收口（第 3 轮：temp_clear + 手机长按面板）
- 桌面：工具栏「清理临时文件」+ 空白右键菜单项 + 确认框（只清未完成暂存，已完成文件不删）+ 三种结果提示（全部/部分/零项）；部分失败现在同时报「已清理 N 项，M 项未能删除」，不再假称成功；清扫目标只认记忆里的接收根，用户手输的路径不再被当成目标。
- 覆盖策略三项改为 自动重命名 / 覆盖 / 每次询问，{2,1,0} 映射未动，弹窗链路验证过（配置每次取用 → kAskSkip 选 resolver → Ask/Pump）。
- Web：长按 500 ms / 10 px 容差动作面板，7 项 op + 	emp_clear，payload 与宿主逐字对齐（rename{path,name}、delete{paths[]}、mkdir{path,name}、stat{paths[]}、open/reveal{path}、temp_clear{directory}）；名称校验含空/空白/分隔符/../控制字符/255 字节（按 UTF-8 计数）/同名/未改变（静默关闭，不再红字）；unsupported 逐 op 记忆，老宿主只剩「刷新」。
- 审核结论：桌面 PASS-WITH-NITS、Web PASS-WITH-NITS，无阻塞；已修 4 条 nit。
- 证据：xmake b crossdesk build ok；13/13 测试目标通过；generate.py --check 1064 串 × 10 语言；
ode --check file_manager.js 通过；服务端 ile_manager.js 与磁盘 sha256 一致。
