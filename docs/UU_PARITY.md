# CrossDesk × UU远程（GameViewer）功能对齐报告

- 仓库：`C:\Users\Mayn\Documents\deepseek-harness\default-workspace\crossdesk`
- 代码快照：2026-11-21 08:4x（**并行施工**：队列控制、浏览器 UI、远端 ops、续传侧车四个 worker 分文件施工，四层当轮全部落地，见附录 A。快照时 `file_transfer_core.h` 被追加了 `SendQueue`，`:548` 之后的行号易漂）
- 目标产品：`GameViewer 4.42.0.2770`（`D:\UU远程\GameViewer\bin\GameViewer.exe`，74.5 MB；另有 `GameViewerServer.exe`）
- 本项目定位：**功能平替（drop-in replacement）**，不是协议互操作。任何"对齐"都指能力与交互对等，不指与 UU 的报文互通。

## 0 证据分级与读法

| 标记 | 含义 |
| --- | --- |
| **观测** | 该字符串/资源名确实存在于二进制中，已记录文件内偏移；或该项目代码里能指到确切行号 |
| **推断** | 由资源族名或日志模板推得的能力，行为细节未运行验证 |
| **未验证** | 本项目代码内找不到可引用行号，或该项无法从静态证据与代码双向确认 |
| **未复现** | 项目内既有文档记录过，但本轮证据文件未覆盖或结论冲突 |
| **在建** | 归属已定到「文件 + 函数名」，但快照时该函数尚未落地。**不得据此断言能力可用**；落地后须把该行降为「观测」并给出行为断言。本轮快照里该标记已清零：四个 worker 全部落地，所有「已对齐」行都能指到行号或函数 |

UU 侧证据来自 `docs/UU_FEATURE_EVIDENCE.md` 与工作区根 `uu_feature_strings.txt`（152 条命中，`utf-16le`）。三类证据强度递减：

1. **日志模板**（最强）：如 `file_transfer_task_create result=ready type= overwrite= resume=`（`GameViewer.exe 0x0401bbaa`）。这类串只在真实代码路径里存在，说明该分支必然被执行过。
2. **资源族名**：如 `file_transfer_pause_all_hover.svg`（`0x03a06cdd`）。说明界面元素存在，"按钮存在"≠"该版本一定可用"。
3. **纯符号**：来自更早一轮逆向（`docs/PHOTO_GALLERY.md:57-63`、`docs/REMOTE_CURSOR.md:12-21`），本轮未复扫。

**关于 UU 的中文文案**：本轮扫描 `utf-16le` 的 CJK 命中为 **0**，因此本文不为 UU 编造任何中文界面文字；下文出现的 UU 能力名都是资源族名或日志字段的直译。英文串确实存在（`0x04684552` `File changed; resume not supported.`、`0x04684ac2` `Disk operation failed; some tasks were canceled.`），说明文案至少部分是外部资源或英文兜底。
冲突说明：`docs/REMOTE_CURSOR.md:17-20` 曾记录 `使用主控端鼠标`（`0x0466ece0`）等中文串，与本轮"0 命中"矛盾。可能因本轮扫描范围限定文件传输族，也可能上一轮的偏移有误——**该组中文串标记为「未复现」**，本文引用时只引偏移，不引文字。

## 1 一页结论

### 1.0 本轮计数（快照口径）

| 口径 | 数量 | 说明 |
| --- | --- | --- |
| 已对齐（能力对等） | **21 项** | 全部在快照内可指到行号或函数名（0 项在建） |
| 行为等价但实现不同 | **8 项** | 见 §1.2 |
| 仍未对齐 | **7 组** | 见 §1.3 与 §4.1 |
| 本轮新增可验证产物 | **5 个文件** | `file_transfer_resume.h`（新）、`file_transfer_resume_test.cpp`（新）、`RESUME_DESIGN.md`（新）、`xmake.lua`（加一个目标）、`UU_PARITY.md`（本文件） |

上一版（07:2x 快照）的计数是：已对齐 13 项 / 行为等价 8 项 / 未对齐 7 组。本轮把队列级控制、per-item 控制、类型与方向图标、列排序、批量汇总、远端文件操作（mkdir/rename/delete/open/reveal/stat/temp_clear）、窗口内改路径与打开目录这 7 项从「未对齐」翻到「已对齐（本轮已有实现，见 §2 行号）」，并新增 1 项底层能力（续传侧车元数据层）。

### 1.1 已经对齐（能力对等）

| 能力 | UU 证据 | 本项目证据 |
| --- | --- | --- |
| 可靠文件数据面 + 应用层 ACK | `file_transfer` 族（`0x039fa783`）＋`ftp_metadata`（`0x0402006e`） | `libs/wire/include/file_transfer_format.h:18-45`；`libs/controller/include/file_transfer_core.h:196-228` |
| 分块传输 + 窗口流控 | `file_transfer_loading.svg`（`0x03a06783`） | `core.h:113-167`（1 chunk/pump、1 MiB 窗口） |
| 单任务终止（取消） | `file_transfer_item_terminate_normal.svg`（`0x03a07429`） | `core.h:234-247`；`file_transfer_manager.cpp:287-309` |
| 清除已完成 | `file_transfer_remove_complete_hover.svg`（`0x03a067bb`） | `apps/desktop/src/gui/ui/file_browser.slint:589,819`；`file_browser_window.cpp:196-202` |
| 同名冲突重命名 | `file_transfer_collision result=renamed`（`0x0401c862`） | `core.h:387-436`（时间戳后缀）；`apps/desktop/src/tools/file_transfer.cpp:24-52` |
| 接收路径安全校验 | `unsafe_relative_path` / `unsafe_folder`（`0x0401cf38`） | `libs/controller/src/file_browser.cpp:74-107`；`core.h:457-464` |
| 递归建目录 / 父目录创建 | `file_transfer_directory_create` / `_parent_create`（`0x0401c862`、`0x0401cf38`） | `file_browser.cpp:91-107`；`core.h:344-355` |
| 传输清单（折叠展开） | `file_transfer_manifest result=accepted file_count=`（`0x0401c862`） | `file_browser.cpp:636-708`（`op=prepare`，32 项/批、≤4096 项） |
| 限速 | `shared::file_transfer::ByteTokenBucket`（符号，`docs/PHOTO_GALLERY.md:61`） | `core.h:34-36,169-183,493-510`（本轮） |
| 单任务暂停/继续 | `file_transfer_item_pause_normal.svg`（`0x03a07d73`） | `core.h:184-194`；`file_transfer_manager.cpp:311-351`（本轮） |
| 列表排序 / 刷新 | `file_transfer_item_header_ascending.svg`（`0x03a08017`）、`file_transfer_refresh_normal.svg`（`0x03a07313`） | `file_browser.slint:586`；`file_browser.cpp:307-362` |
| 控制端自绘光标 + 系统指针隐藏 | `viewing_cursor_{normal,hand,grab,disabled,pen}.svg`（`docs/REMOTE_CURSOR.md:12-16`） | `apps/desktop/src/gui/ui/uu_cursor.slint:40-61`；`stream_window.slint:1110-1132` |
| 鼠标三模式（智能/游戏/仿真） | `docs/REMOTE_CURSOR.md:17-20`（未复现文字） | `config_center.cpp:206-207`；`gui_application.cpp:5841-5852` |
| 队列级控制：全部开始 / 全部暂停 / 全部继续 / 全部清除 | `start_all_*`（`0x03a064a5` 等 4 档）、`pause_all_*`（`0x03a066ef` 等 4 档）、`clear_all_*`（`0x03a06903` 等 4 档） | **观测**（本轮）：`file_transfer_core.h:554`（`SendQueue`：`State` `:558`、`Item` `:559-568`、`Summary` `:569-573`、`PauseAll` `:663`、`Clear` `:724`）；`file_transfer_manager.h:79-84`；`file_transfer_manager.cpp:541/566/596/618`（`PauseAllTransfers`/`ResumeAllTransfers`/`StartAllTransfers`/`ClearAllTransfers`）；`file_browser.slint:650-653`；`file_browser_window.cpp:377-386` |
| per-item 暂停 / 继续 / 重试 | `item_pause_*`（`0x03a07d73`）、`item_resume_*`（`0x03a073db`）、`item_retry_*`（`0x03a0747b`） | **观测**（本轮）：`SendQueue::Pause/Resume/Retryable`（`core.h:651/657/610`）；`file_transfer_manager.cpp:630/651/675`（`PauseTransferItem`/`ResumeTransferItem`/`RetryTransferItem`）、`:212`（`sources_`，重试重放）；`file_browser.slint:654-655`、`:1098-1104`（per-item 暂停按钮）、`:1106-1110`（重试按钮）；`file_browser_window.cpp:380`；引擎 `core.h:184-194` |
| 类型图标（16 类）+ 方向 × 暂停态图标 | `_item_pdf/word/excel/ppt/text/picture/music/video/folder/disk/link/desktop/document/download/my_computer/other`（`0x03a080ff` 等 16 个）；`item_arrow_left/right`（`0x03a0806b`/`0x03a074c9`）、`_left_pause`/`_right_pause`（`0x03a0768f`/`0x03a075a7`） | **观测**（本轮）：`file_browser.slint:19-35`（`BrowserTask` 的 `direction` `:23`、`paused` `:25`、`kind` `:27`、`icon` `:29`）；行渲染 `:1035`（类型图标）、`:1042-1048`（方向箭头，暂停时换色）、`:1053`（暂停角标）；`file_browser_window.cpp:73-77`（`TaskKind` 16 类）、`:79-95`（字形表）、`:121`（`TaskKindForName`） |
| 列排序方向箭头 | `_item_header_ascending.svg`（`0x03a08017`）、`_descending.svg`（`0x03a07abb`） | **观测**（本轮）：文件列表：`file_browser.slint:161`（`sort-descending` 驱动的箭头）、`:643`（`sort` 回调）；任务列表：`:649`（`sort-tasks`）+ `file_browser_window.cpp:376`（`SortTasks`） |
| 批量汇总（x/y 部分成功） | `send_part_success`（`0x03a0106b`）、`recv_part_failed`（`0x03a01463`）、`send_success`（`0x03a00e73`）、`recv_failed`（`0x03a0120b`/`0x03a012c7`） | **观测**（本轮）：`SendQueue::Summary`（`core.h:569-573`，`batch_partial` `:572`）；`file_transfer_manager.h:31-35,97`；`file_transfer_manager.cpp:714`（`BatchSummary`）；`file_browser.slint:559`（`task-summary`）；`file_browser_window.cpp:710`（`set_task_summary`） |
| 远端 mkdir / rename / delete / open / reveal / stat / temp_clear | `_item_create`（`0x03a079e1`）、`_new_folder`（`0x03a07649`）、`_item_rename`（`0x03a077b5`）、`_item_delete`（`0x03a0813b`）、`_item_open_*`（`0x03a07c29`）、`open_path_*`（`0x03a06e0d`）、`temp_clear`（`GameViewerServer.exe 0x016cb0a6`） | **观测**（本轮）：`libs/controller/include/file_browser_ops.h:50-78`（op 名表）；`libs/controller/src/file_browser.cpp` 的 `MkdirReply` `:746`、`RenameReply` `:759`、`DeleteReply` `:778`、`StatReply` `:804`、`TempClearReply` `:842`、`HostOpenReply` `:863`、`HostOpen` `:230`；`Serve` 分派 `:924-930`；未知 op 回 `unsupported`（`file_browser_ops.h:102`） |
| 窗口内「修改保存路径 / 打开所在文件夹」 | `modify_path_*`（`0x03a06457`）、`open_path_*`（`0x03a06e0d`） | **观测**（本轮）：`file_browser.slint:656-657`（回调）、`:732`（打开保存目录按钮）、`:747`（改保存路径按钮）；`file_browser_window.cpp:399/407`（`on_open_save_folder`/`on_change_save_path`） |
| 续传元数据载体（侧车） | `file_transfer_temp_lookup`（`metadata_missing`）、`file_transfer_temp_clear`、`file_transfer_partial_file_remove`（`docs/UU_FEATURE_EVIDENCE.md:20-21,48,54`，**观测**） | **本轮落地**：`libs/controller/include/file_transfer_resume.h`（header-only）：`ResumeRecord` `:39-46`、侧车后缀 `.crossdesk-resume` `:50`、`WriteResumeRecord` `:336-393`、`ReadResumeRecord` `:396-400`、`ClearResumeRecord` `:403-417`、`ListResumeRecords` `:420-450`、`TargetMtimeMs` `:317` |

### 1.2 行为等价但实现不同

| 能力 | UU 的做法 | 本项目的做法 |
| --- | --- | --- |
| 控制面 | protobuf RPC（`rpc_file_transfer.proto`、`FileTransferModule::sendRpcRequest/Response`，`docs/PHOTO_GALLERY.md:57`） | `control_data` 上的 JSON RPC `type=file_browser` v1（`file_browser.cpp:590-634`；`file_transfer_manager.cpp:720-734`） |
| 数据面 | FTP 式（`file_transfer_ftp.cpp`、`ftp_helper.cpp`、`ff_file_transfer_ftp(2)`，`docs/PHOTO_GALLERY.md:58`） | 31 字节 `FileChunkHeader`('JNTM') + 28 字节 `FileTransferAck`('JNTC')，64 KiB/块（`file_transfer_format.h:18-45`；`stream_names.h:26`） |
| 限速算法 | `ByteTokenBucket`（符号） | 同名思路的自研 token bucket，1 秒信用、容量夹到 ≥1 chunk（`core.h:493-510`） |
| 冲突处理 | `file_transfer_collision` / `_folder_collision` 两套对话框 + `existing_file_remove` / `partial_file_remove` 前置清理 | 引擎发布时**从不覆盖**（硬链接/独占创建 + 时间戳后缀，`core.h:393-434`），发布后再按 `kOverwrite/kRename/kAskSkip` 二次改名（`tools/file_transfer.cpp:24-52`） |
| 半成品命名 | `.downloading`（`0x0401c862`） | `.partial` + 随机暂存目录 `.crossdesk-<id>-<tick>-<serial>`（`core.h:347-358`） |
| 任务模型 | `FileTransferTaskManager` + `FileSenderTask`/`FileReceivingTask` + `readNextBlock`/`sendNextBlock`（`docs/PHOTO_GALLERY.md:59`） | 每会话一个串行发送 worker（`file_transfer_manager.cpp:98-256`），接收侧 per-peer `Session`（`file_transfer_manager.cpp:438-515`） |
| 光标资源 | 5 个 SVG | 14 个 64×64 PNG（`apps/desktop/src/gui/ui/cursors/uu_*.png`；映射 `uu_cursor.slint:46-61`） |
| 相对位移注入 | 由被控端实现（推断） | `MouseFlag::relative_move`(9) + `MOUSEEVENTF_MOVE`（`remote_action.h:41-55`；`platform/windows/input/mouse/mouse_controller.cpp:93-113`，仅 Windows） |

### 1.3 尚未对齐

按 UU 自身词汇分组（详见 §4.1）。本轮的 7 项（队列级控制、per-item 控制、类型/方向图标、列排序箭头、批量汇总、远端文件操作、窗内路径入口）已翻到 §1.1，仍缺的 7 组是：

1. **真跨会话续传**：`resume=` 意图参数、`resume_point=`、`metadata_missing` 的**协议侧**语义。本轮只落地了本地侧车元数据层（§1.1 最后一行），没有握手就没有真续传（见 `docs/RESUME_DESIGN.md` §3、§5）。
2. **冲突询问**：两套冲突对话框（`collision` / `folder_collision`）、`skipped reason=already_exists` 的跳过语义。
3. **任务列表持久化**：`CachePath` + `.json`；重启后历史仍丢失。
4. **移动端**：多选、移动端相册上传、iOS 手写 `file` 线解析与引擎的 offset 语义同步、10 语言 i18n 补全。
5. **临时文件跨会话巡检**：`op=temp_clear` 落地后仍有「崩溃留下的孤儿暂存目录」无人回收（暂存目录名随机，见 §3.3）。
6. **非 Windows 被控端相对位移**：`game_mouse.cpp` 在 macOS/Linux 是 no-op。
7. **低优先外观项**：`combo_view` 下拉视图、被控端背景图（无法验证 UU 是否独立窗口）。

## 2 逐项对照表

> 「UU 证据（偏移）」均指 `GameViewer.exe` 文件内偏移，除注明 `GameViewerServer.exe` 者。
> 「本项目证据」一律为仓库相对路径 + 行号。
>
> **行号漂移警告**：`file_transfer_core.h`、`file_transfer_manager.{h,cpp}`、`file_browser.slint`、`file_browser_window.cpp`、`libs/controller/src/file_browser.cpp` 在本轮被并行的其他 worker 持续改动，标注的行号是 08:4x 快照值。定位时请优先按**函数名 / 回调名** grep（例：`PauseAllTransfers`、`on_clear_all_tasks`、`MkdirReply`、`start-all-tasks`），括号里的 `:N` 只作参考。

### 2.1 线格式与数据面

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 独立文件传输 RPC | `file_transfer_task_create result=ready type=… overwrite=… resume=…` `0x0401bbaa` | 观测 | 有（JSON RPC） | `libs/controller/src/file_browser.cpp` | `file_browser.cpp:590-634` | 报文语义不对等（无 `overwrite=`/`resume=` 字段） |
| FTP 数据面 + `ftp_metadata` | `ftp_metadata` `0x0402006e`；`GameViewerServer.exe 0x017ab5ee` | 观测 | 有（定长 chunk） | `libs/wire` | `file_transfer_format.h:22-45`；`libs/wire/src/file_transfer.cpp:8-90` | 无 FTP 的 `part/rest` 语义，靠 header 里的 `offset` |
| 传输清单 | `file_transfer_manifest result=accepted file_count=` `0x0401c862` | 观测 | 部分 | `file_browser.cpp` `op=prepare` | `file_browser.cpp:602-614,676-708` | 无清单文件；每批 ≤32 项、≤4096 项 |
| 半成品后缀 | `.downloading` `0x0401c862` | 观测 | 等价 | `file_transfer_core.h` | `core.h:352,357-358` | 命名不同，语义相同 |
| 续传元数据载体（侧车） | `file_transfer_temp_lookup`（`reason=metadata_missing`）、`file_transfer_temp_clear`、`file_transfer_partial_file_remove`（`docs/UU_FEATURE_EVIDENCE.md:20-21,48,54`） | 观测 | 部分（本轮落地） | `libs/controller/include/file_transfer_resume.h`（header-only） | `file_transfer_resume.h:39-46`（`ResumeRecord`）、`:50`（后缀 `.crossdesk-resume`）、`:317`（`TargetMtimeMs`）、`:336-393`（原子写）、`:396-400`（读+新鲜度）、`:403-417`（幂等清）、`:420-450`（枚举，跳过畸形） | 只有本地元数据层：没有 `op=resume` 协商，没有跨会话 offset 入口（见 §4.1 A 与 `docs/RESUME_DESIGN.md`） |
| 相对路径/文件夹越界校验 | `unsafe_relative_path` `0x0401cf38`；`unsafe_folder` `0x0401cf38`；`unsafe_folder_strategy` `0x0401c862`；`unsafe_empty_directory` `0x0401c862` | 观测 | 等价 | `file_browser.cpp` / `core.h` | `file_browser.cpp:74-81`（>4096B、含 NUL 拒绝）、`:91-107`（绝对路径/`..`/符号链接拒绝）；`core.h:457-464`（只取 basename、控制字符替换） | 无「strategy」概念：我们不区分"文件夹策略不安全"与"空目录不安全" |
| 目录创建 / 父目录创建 + `io_error` | `file_transfer_directory_create result=failed reason=io_error`、`file_transfer_parent_create result=failed code=` `0x0401c862`、`0x0401cf38` | 观测 | 等价 | `file_browser.cpp` | `file_browser.cpp:95-105`（逐级 `create_directory`） | 错误码不复用 UU 的 `io_error` |
| 目录删除 / 回滚 | `file_transfer_directory_remove result=failed reason=io_error` `0x0401c862` | 观测 | 部分 | `core.h` | `core.h:466-471`（`Remove()` 删 `.partial` 与暂存目录）、`:517-523` | 只在取消/失败/发布后清理，无"先建后回滚"事务 |
| 任务列表持久化 | `file_transfer` … `CachePath` … `.json` `0x0401628c` | 观测 | 无 | — | 全仓无 `CachePath` 任务列表（仅 `config.ini`/trusted-devices 等，`gui_application.cpp:1211`） | 重启后传输历史丢失 |

### 2.2 任务级控制

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 单任务暂停 | `file_transfer_item_pause_normal.svg` `0x03a07d73`；`_hover` `0x03a081b9`；`_clicked` `0x03a07fc7` | 观测 | 已对齐（本轮） | `core.h` / `file_transfer_core.h` / `file_transfer_manager.{h,cpp}` / `file_browser.slint` | 引擎：`core.h:184-194`（`SetSendPaused`）、`:135-138`（暂停时刷新 deadline）；队列：`SendQueue::Pause`（`core.h:651`）；管理器：`PauseTransferItem`（`file_transfer_manager.cpp:630`）；UI：`file_browser.slint:1098-1104`（行内暂停按钮 → `toggle-task-pause`） | 接收方向仍不能暂停（`ReceiveLocked` 无暂停语义） |
| 单任务继续 | `file_transfer_item_resume_normal.svg` `0x03a073db`；`_hover` `0x03a07889`；`_clicked` `0x03a07dfd` | 观测 | 已对齐（本轮） | 同上 | `SendQueue::Resume`（`core.h:657`）；`ResumeTransferItem`（`file_transfer_manager.cpp:651`）；同一按钮在 `task.paused` 时换成播放图标（`file_browser.slint:1100-1101`） | 同上 |
| 单任务重试 | `file_transfer_item_retry_normal.svg` `0x03a0747b`；`_hover` `0x03a07ee7`；`_clicked` `0x03a07b0f` | 观测 | 已对齐（本轮） | `file_transfer_core.h` / `file_transfer_manager.{h,cpp}` / `file_browser.slint` | `SendQueue::Retryable`（`core.h:610`，只有终态行可重放）；`RetryTransferItem`（`file_transfer_manager.cpp:675`）+ `sources_`（`file_transfer_manager.h:212`）重放同一 name/path；UI `file_browser.slint:1106-1110` | 「从失败点继续」仍依赖真续传（§4.1 A），本轮重试一律从 0 开始；重放会分配新 `file_id`，避免撞 `cancelled_incoming_` 墓碑（`core.h:302,314-315`） |
| 单任务终止 | `file_transfer_item_terminate_normal.svg` `0x03a07429`；`_hover` `0x03a07e95`；`_clicked` `0x03a07511` | 观测 | 有 | `core.h` / `file_transfer_manager.cpp` / `file_browser.slint` | `core.h:234-247`；`file_transfer_manager.cpp:287-309`；`file_browser.slint:645,1115` | 语义等价；无"终止但保留半成品"选项 |
| 方向图标 | `file_transfer_item_arrow_left.svg` `0x03a0806b`；`_right.svg` `0x03a074c9`；`_arrow_failed.svg` `0x03a07f7d`；`_item_send.svg` `0x03a07cbb`；`_item_arrive.svg` `0x03a07b99` | 观测 | 已对齐（本轮） | `file_browser.slint` / `file_browser_window.cpp` | `BrowserTask.direction`（`file_browser.slint:23`）、图标 cell + 箭头 cell 结构见 `:982`、箭头字形 `:1047`（`\u{f060}`/`\u{f061}`） | 用 FontAwesome 字形代替 UU 的 SVG，方向语义等价 |
| 方向 × 暂停态图标 | `file_transfer_item_arrow_left_pause.svg` `0x03a0768f`；`_right_pause.svg` `0x03a075a7` | 观测 | 已对齐（本轮） | `file_browser.slint` | `BrowserTask.paused`（`:25`）→ 箭头变色（`:1048`）+ 暂停角标（`:1053`），是「方向 × 暂停」的二维表达 | 视觉不是 UU 的双图标资源，语义等价 |
| 暂停态字段 | `paused` ×3 `0x04645b8a`、`0x04645d3a`、`0x04645eea` | 观测 | 已对齐（本轮） | `file_transfer_core.h` / `file_transfer_manager.{h,cpp}` / `file_browser.slint` | `SendQueue::Item.held`（`core.h:565`）是暂停的唯一真相源（`State::Paused` 由 hold 推导，`:556-558`）；行模型 `BrowserTask.paused`（`:25`）；peer 级 hold 仍保留（`file_transfer_manager.h:205` `paused_remotes_`）用于「整条流水线」语义 | 两套 hold（peer 级 + item 级）并存，需要 `StartAll`/`PauseAll` 组合时注意优先级 |
| 接收侧 resume_point | `file_transfer_receive_file result=started resume_point=` `0x0401d354` | 观测 | 无 | — | `core.h:372-385` 只在 `received==total` 时完成 | 依赖真续传握手（§4.1 A） |

### 2.3 队列级控制

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 全部开始 | `file_transfer_start_all_hover.svg` `0x03a064a5`；`_clicked` `0x03a0651d`；`_disabled` `0x03a066a1`；`_normal` `0x03a068b7` | 观测 | 已对齐（本轮） | `file_transfer_core.h` / `file_transfer_manager.{h,cpp}` / `file_browser.slint` | `SendQueue`（`core.h:554`：`Start` `:632`、`Active` `:616`）；`ProcessQueue`（`file_transfer_manager.cpp:121`，补满槽位）+ `StartAllTransfers`（`:596`）；UI `file_browser.slint:650` → `file_browser_window.cpp:377` | 并发槽位由 `concurrent_per_peer_`（`file_transfer_manager.h:214`）限制；对端移动端 `max_incoming=4`（`mobile_file_transfer_session.h:19-21`），槽位设过高会被对端 `reject()`（`core.h:334-335`） |
| 全部暂停 | `file_transfer_pause_all_disabled.svg` `0x03a066ef`；`_hover` `0x03a06cdd`；`_clicked` `0x03a06d25`；`_normal` `0x03a0702f` | 观测 | 已对齐（本轮） | 同上 | `SendQueue::PauseAll`（`core.h:663`）+ `PauseAllTransfers`（`file_transfer_manager.cpp:541`，同时 `SetSendPaused(true)` 到各 `senders_`）；UI `file_browser.slint:651` → `file_browser_window.cpp:378` | 只影响出站方向 |
| 全部继续 | `file_transfer_start_all_*`（同族） | 推断 | 已对齐（本轮） | 同上 | `SendQueue::ResumeAll`（`core.h:672`）+ `ResumeAllTransfers`（`file_transfer_manager.cpp:566`，并重设限速）；UI `file_browser.slint:652` → `file_browser_window.cpp:379` | 同上 |
| 清空列表 | `file_transfer_clear_all_disabled.svg` `0x03a06903`；`_hover` `0x03a06ee3`；`_clicked` `0x03a06fb5`；`_normal` `0x03a07201` | 观测 | 已对齐（本轮） | `file_transfer_manager.{h,cpp}` / `file_browser.slint` / `file_browser_window.cpp` | `ClearAllTransfers`（`file_transfer_manager.cpp:618`）+ `SendQueue::Clear`（`core.h:724`）；UI `file_browser.slint:653` → `file_browser_window.cpp:386` | 只清列表与队列簿记，不删已落盘文件（与 UU 亦可能如此，推断） |
| 清除已完成 | `file_transfer_remove_complete_hover.svg` `0x03a067bb`；`_clicked` `0x03a06f5f`；`_normal` `0x03a069cf` | 观测 | 有 | `file_browser_window.cpp` | `file_browser.slint:819`；`file_browser_window.cpp:196-202`（`dismissed_`） | 只隐藏，不删历史（UU 亦可能如此，推断） |
| 任务多选 / 全选 | `file_transfer_item_select_all.svg` `0x03a0776d`；`_checked_normal` `0x03a08243`；`_unchecked_normal` `0x03a0833d` | 观测 | 部分 | `file_browser.slint` | 文件列表有 `select-all`（`file_browser.slint:580`）、`selected-count`（`:565`）；任务列表无 | 传输列表无多选/批量操作 |

### 2.4 冲突处理

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 同名文件冲突 | `file_transfer_collision result=renamed` `0x0401c862`；资源族 `file_transfer_collision`（`docs/UU_FEATURE_EVIDENCE.md:19`） | 观测 | 部分（本轮） | `tools/file_transfer.cpp` / `file_transfer_manager.cpp` / `config_center` | `tools/file_transfer.cpp:24-52`；`file_transfer_manager.cpp:521-555`；`config_center.cpp:213-214,332-335,710-719` | 只在"直落接收目录根"时生效（`file_transfer_manager.cpp:524-528` 的 `equivalent()` 守卫）；浏览器下载与文件夹布局不走策略 |
| 同名文件夹冲突 | `file_transfer_folder_collision result=renamed` `0x0401c862` | 观测 | 无 | — | `file_browser.cpp:91-107` 对已存在目录直接复用（`create_directory` 的 `file_exists` 视为成功） | 无文件夹级冲突决策 |
| 两套冲突对话框 | `file_transfer_collision` / `file_transfer_folder_collision` 各 x1 | 推断 | 无 | — | `tools/file_transfer.cpp:49-51` 注释明说"Ask/skip has no prompt in the desktop pipeline yet" | 无对话框；`kAskSkip` 退化为保留时间戳名 |
| 跳过已存在 | `file_transfer_item result=skipped reason=already_exists` `0x0401c240` | 观测 | 无 | — | `tools/file_transfer.cpp:49-51` | "跳过"应删掉刚发布的副本，当前保留 |
| 覆盖前删除既有文件 | `file_transfer_existing_file_remove result=failed reason=io_error` `0x0401c862` | 观测 | 有（本轮） | `tools/file_transfer.cpp` | `tools/file_transfer.cpp:31-38`（`remove` 后 `rename`） | 删除失败即回落原名，无用户可见错误 |
| 清理残留半成品 | `file_transfer_partial_file_remove result=requested path_chars=` `0x0401d690` | 观测 | 部分 | `core.h` | `core.h:466-471` 立即删，无"请求清理"两段式 | 无孤儿半成品巡检（崩溃后留残） |
| 覆盖策略参数化 | `overwrite=` `0x0401bbaa` | 观测 | 部分（本轮） | `config_center` | `config_center.h:71-72,157-159`；`config_center.cpp:213-214` | 本地烘焙，不随任务传给对端 |

### 2.5 续传与临时文件

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 续传意图参数 | `file_transfer_task_create … resume=` `0x0401bbaa` | 观测 | 无 | — | `core.h:90-111`（`Start()` 无起始偏移，`state_` 写死 `0`：`core.h:103`） | 线格式无 offset 协商入口；方案对比见 `docs/RESUME_DESIGN.md` §2（A 预置 ack / B `op=resume` / C 纯侧车） |
| 元数据缺失 | `file_transfer_resume result=failed reason=metadata_missing` `0x0401c046` | 观测 | 部分（本轮落地） | `libs/controller/include/file_transfer_resume.h` | `ReadResumeRecord`（`:396-400`）返回 `not_found`/`malformed`，等价于 UU 的 `metadata_missing`；无 `op=resume` 入口把它变成协议事件 | 语义有了，协议侧没有触发器 |
| 元数据失效原因 | `reason=source_changed file_name_chars=… extension=` `0x0401cf38`；`reason=file_size` `0x0401d354`；`reason=partial_file_missing` `0x0401cf38` | 观测 | 部分（本轮落地） | `libs/controller/include/file_transfer_resume.h` | `ReadTargetState`（`:182-200`）+ 读取分支（`:280-289`）：大小或 mtime 不符 → `stale`（对应 `source_changed`/`file_size`），目标不存在 → `missing`（对应 `partial_file_missing`），三元组校验在 `:262-275` | 失败原因已可区分，但还没接到任务行/UI 文案 |
| 清理临时数据 | `file_transfer_temp_clear result=success request_id=… task_key=` `GameViewerServer.exe 0x016cb0a6` | 观测 | 已对齐（本轮） | `libs/controller/src/file_browser.cpp` / `libs/controller/include/file_browser_ops.h` | `FileBrowser::TempClearReply`（`file_browser.cpp:842-861`，`Serve` 分派 `:928`）：作用域先 `ExistingDirectory` 再 `ops::ContainedIn(root, requested)`（`:849`，只允许接收目录内），选择器 `ops::SelectStagingDirectories`（`file_browser_ops.h:270-290`，只选真目录、不跟随符号链接、`kMaxTempClearEntries=256`），删除是 `remove_all` 整个暂存目录 | 侧车随暂存目录一起消失（侧车就在暂存目录里），但**没有**按 `task_key` 粒度清理、也还没有跨会话的孤儿巡检（§4.1 E）；`TempClearReply` 目前不读 `ListResumeRecords` |
| 可续传半成品标记 | `file_transfer_receive_file result=preparing resumable=` `0x0401d354` | 观测 | 部分（本轮落地） | `libs/controller/include/file_transfer_resume.h` | 侧车存在即「可续传」，`ListResumeRecords` 就是可续传半成品的清单；`core.h:347-358` 的暂存目录名仍随机，所以侧车是**唯一**能定位半成品的锚点 | 暂存目录名未可预测化，跨会话仍要靠侧车自报 `target_path` |
| 「文件已变更，无法续传」提示 | `File changed; resume not supported.` `0x04684552`（英文） | 观测 | 部分（本轮落地） | `file_transfer_resume.h` | 判定已就绪（`stale`）；缺 UI 状态与 i18n 词条 | 见 §4.1 D（移动端/i18n） |

### 2.6 列表体验

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 按类型图标（16 类） | 观测各 x1：`_item_pdf` `0x03a080ff`、`_word` `0x03a0735b`、`_excel` `0x03a07567`、`_ppt` `0x03a07b5d`、`_text` `0x03a07d37`、`_picture` `0x03a08203`、`_music` `0x03a07dbf`、`_video` `0x03a07cf7`、`_folder` `0x03a07911`、`_disk` `0x03a078d3`、`_link` `0x03a07a7f`、`_desktop` `0x03a07847`、`_document` `0x03a07395`、`_download` `0x03a0799b`、`_my_computer` `0x03a07c71`、`_other` `0x03a0817b` | 观测 | 已对齐（本轮） | `file_browser.slint` / `file_browser_window.cpp` | `BrowserTask.kind/icon`（`file_browser.slint:27,29`）；行内图标 cell `:1035`；`TaskKind`（`file_browser_window.cpp:73-77`）、16 字形表 `:79-95`、扩展名映射 `TaskKindForName` `:121` | 图标是 FontAwesome 字形而非 UU 的 16 个 SVG，视觉不等价（可接受） |
| 列排序方向 | `_item_header_ascending.svg` `0x03a08017`；`_descending.svg` `0x03a07abb` | 观测 | 已对齐（本轮） | `file_browser.slint` / `file_browser_window.cpp` | 文件列表箭头随 `sort-descending` 切换（`file_browser.slint:161`）+ `sort` 回调 `:643`；任务列表 `sort-tasks`（`:649`）→ `file_browser_window.cpp:376`（`SortTasks`） | 等价（箭头 + 三列排序 + 任务列表独立排序） |
| 刷新 | `file_transfer_refresh_normal.svg` `0x03a07313`；`_hover` `0x03a06e9f`；`_clicked` `0x03a06e55` | 观测 | 有 | `file_browser.cpp` | `file_browser.slint:575`（`callback refresh(bool)`）；`file_browser.cpp:307-362` | 等价 |
| 下拉排序/筛选视图 | `:/file_transfer/file_transfer_combo_view.png` `0x04621c72`；`_combo_view@2x.png` `0x04621d02` | 观测 | 部分 | `file_browser.slint` | 过滤框 + 排序头（`file_browser.slint:585-586`） | 形式不同，无 combo 视图 |
| 加载态 | `file_transfer_loading.svg` `0x03a06783` | 观测 | 有 | `file_browser.cpp` | `file_browser.cpp:351-353`（`loading/loaded/total` 事件） | 等价 |
| 被控端背景 | `:Gfile_transfer_controlled_bg.png` `0x03a06bb1`；`@2x` `0x03a072cb` | 观测 | 未验证 | — | 相册有"本机模式"（`gui_application.cpp:4956-4993`），文件传输窗口无 | 未验证是否为独立窗口形态 |
| 远端新建文件夹 | `file_transfer_item_create_normal.svg` `0x03a079e1`；`_new_folder.svg` `0x03a07649` | 观测 | 已对齐（本轮） | `libs/controller/src/file_browser.cpp` / `file_browser_ops.h` | `FileBrowser::MkdirReply`（`file_browser.cpp:746`），`Serve` 分派 `:924`；op 名 `ops::kOpMkdir`（`file_browser_ops.h:73-78` 表内） | 旧端收到未知 op 回 `unsupported`（`file_browser_ops.h:102`），UI 需把它当「不支持」而不是错误 |
| 远端重命名 | `file_transfer_item_rename.svg` `0x03a077b5` | 观测 | 已对齐（本轮） | 同上 | `FileBrowser::RenameReply`（`file_browser.cpp:759`，`:767-773` 处理「仅大小写变化」与真冲突），分派 `:925` | 破坏性操作：符号链接与越界路径由 `ExistingEntry`/`ContainedIn` 拒绝 |
| 远端删除 | `file_transfer_item_delete.svg` `0x03a0813b`；`_item_remove_normal.svg` `0x03a080b1` | 观测 | 已对齐（本轮） | 同上 | `FileBrowser::DeleteReply`（`file_browser.cpp:778`，`kMaxDeletePaths` 上限），分派 `:926` | 破坏性最高；确认环节由 UI 负责，服务端只保证路径在接收目录内 |
| 远端打开 | `file_transfer_item_open_normal.svg` `0x03a07c29`；`_hover` `0x03a07e4d`；`_clicked` `0x03a0771f` | 观测 | 已对齐（本轮） | 同上 | `FileBrowser::HostOpenReply`（`file_browser.cpp:863`）→ `HostOpen`（`:230`，Windows `ShellExecuteW`；非 Windows 回 `unsupported`），分派 `:929`（open）/`:930`（reveal）；`StatReply`（`:804`）供行刷新 | reveal 在非 Windows 上不可用（`HostOpen` 的 `#else` 分支 `:257-258`） |
| 上一级/返回 | `file_transfer_up_normal.svg` `0x03a06665`；`file_transfer_back_normal.svg` `0x03a06627` | 观测 | 有 | `file_browser.slint` | `file_browser.slint:574-576`（`navigate/refresh/up`） | 等价 |

### 2.7 路径与入口

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 修改保存路径（窗内） | `file_transfer_modify_path_normal.svg` `0x03a06457`；`_hover` `0x03a06dbf`；`_clicked` `0x03a070b9` | 观测 | 已对齐（本轮） | `file_browser.slint` / `file_browser_window.cpp` | 按钮 `file_browser.slint:747`、回调 `:657`；`file_browser_window.cpp:407`（`on_change_save_path`）→ 目录选择 → `ConfigCenter::SetFileTransferSavePath`（设置项在 `config_center.cpp:220-226,807-814`） | 改完路径后已存在的 `FileReceiver` 会自动重设（`file_transfer_manager.cpp` 的 `ReceiverFor` 每次重取），无需重连 |
| 打开所在文件夹 | `file_transfer_open_path_normal.svg` `0x03a06e0d`；`_hover` `0x03a06b39`；`_clicked` `0x03a06c51` | 观测 | 已对齐（本轮） | `file_browser.slint` / `file_browser_window.cpp` | 按钮 `file_browser.slint:732`、回调 `:656`；`file_browser_window.cpp:399`（`on_open_save_folder`）→ 平台打开（相册已有同名能力 `gui_application.cpp:4918-4920,4966-4968`） | 打开动作走平台 shell，非 Windows 平台需各自实现 |
| 主菜单/更多工具入口 | `new_menu_file_transfer.svg` `0x039fbe73`；`file_transfer_normal.svg` `0x039fe005`；`device/devicelist/bottom_tools/file_transfer` `0x04620bc2`；`more_tools/file_transfer` `0x0462d802`；`file_transfer_menu_hover.svg` `0x03a06a27` | 观测 | 有 | `stream_window.slint` | `stream_window.slint:376,394`（`file-transfer-visible`、`file-dropped`）、`:64`（相册按钮文案） | 等价（入口名不同） |

### 2.8 批量结果状态

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 发送整体成功 | `file_transfer_send_success.svg` `0x03a00e73` | 观测 | 已对齐（本轮） | `file_transfer_core.h` / `file_transfer_manager.{h,cpp}` / `file_browser.slint` | `SendQueue::Summary`（`core.h:569-573`）+ `Summarize`（`:727`）；`BatchSummary`（`file_transfer_manager.cpp:714`，声明 `file_transfer_manager.h:97`）；UI `task-summary`（`file_browser.slint:559`，由 `file_browser_window.cpp:710` 填充） | 汇总文案需 10 语言补齐（§4.1 D） |
| 发送部分成功 | `file_transfer_send_part_success.svg` `0x03a0106b` | 观测 | 已对齐（本轮） | 同上 | `Summary::batch_partial`（`core.h:572`，注释直指 UU 的 partial success） | 同上 |
| 接收整体成功 | `Gfile_transfer_recv_success.svg` `0x03a01391` | 观测 | 已对齐（本轮） | `file_transfer_manager.{h,cpp}` | 同一批次簿记覆盖收发两向（`BatchSummary`，`file_transfer_manager.cpp:714`）；接收完成仍由 `core.h:372-385` 判定 | 同上 |
| 接收失败 | `Gfile_transfer_recv_failed.svg` `0x03a0120b`；`_warning.svg` `0x03a012c7` | 观测 | 部分 | `core.h` | `core.h:317-329`（`reject()`）；逐条失败有，汇总告警随批次层一起可见（`Summary::failed`） | 无独立告警弹窗，只有汇总计数 |
| 接收部分失败 | `Gfile_transfer_recv_part_failed.svg` `0x03a01463` | 观测 | 已对齐（本轮） | `file_transfer_core.h` / `file_transfer_manager.{h,cpp}` | `Summary::failed` + `batch_partial`（`core.h:570-572`）驱动的 x/y 汇总 | 同上 |
| 条目结果枚举 | `file_transfer_item result=started` / `result=skipped reason=already_exists` / `result=failed reason=source_missing` `0x0401c240` | 观测 | 部分 | `file_transfer_manager.cpp` | `file_transfer_manager.cpp:232-245` | 无 `skipped`/`source_missing` 区分 |

### 2.9 光标与鼠标模式

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 5 个自绘光标 | `:/control/viewing_cursor_normal.svg` `0x046259b8`、`_hand` `0x04625a38`、`_grab` `0x04625ab8`、`_disabled` `0x04625b38`、`_pen` `0x04625bb8`（来源 `docs/REMOTE_CURSOR.md:12-16`，本轮未复扫） | 未复现 | 有（14 张） | `apps/desktop/src/gui/ui/cursors/` | `uu_cursor.slint:46-61`（shape→PNG 映射）；热区 `:19-37` | 形状覆盖更全，但无法确认与 UU 视觉一致 |
| 系统指针隐藏 | 同上（推断：自绘光标必须隐藏系统指针） | 推断 | 有 | `stream_window.slint` | `stream_window.slint:1110-1112,1160-1162`（`MouseCursor.none`） | 等价 |
| 使用主控端/被控端鼠标 | `docs/REMOTE_CURSOR.md:17-18`（`0x0466ece0`/`0x0466ec58`） | 未复现 | 有 | `config_center` / `gui_application.cpp` | `config_center.cpp:204-207`；`gui_application.cpp:5841-5852` | 等价（`remote_cursor_style` 0/1 + `remote_mouse_mode` 0/1/2） |
| 智能鼠标自动回退 | `docs/REMOTE_CURSOR.md:19`（`0x0466fa38`） | 未复现 | 有 | `gui/runtime/cursor_presentation.h` | `cursor_presentation.h:63-82`（`system_hidden_ && locator_allowed_` → 保持最后形状） | 等价 |
| 游戏鼠标（相对位移） | `docs/REMOTE_CURSOR.md:20` | 未复现 | 有（仅 Windows 被控端注入） | `remote_action.h` / `game_mouse.cpp` / `mouse_controller.cpp` | `remote_action.h:41-55`；`gui_application.cpp:5854-5885`；`game_mouse.cpp:36-70`；`mouse_controller.cpp:93-113` | macOS/Linux 被控端无相对注入（`game_mouse.cpp:76-87` 非 Windows 全为 no-op） |
| 解锁快捷键提示 | `docs/REMOTE_CURSOR.md:21`（`0x0466f9a8`） | 未复现 | 有 | `stream_window.slint` / `gui_application.cpp` | `stream_window.slint:336-337,1135-1144`；`gui_application.cpp:6016-6021`（Esc 本地解锁、不转发） | 等价 |

### 2.10 照片相册

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 相册/缩略图 | 本轮扫描未覆盖（证据范围限定 `file_transfer_*`） | 未验证 | 有 | `libs/controller` + `gui/features/photo` | `file_browser.cpp:502-548`（`gallery`）、`:550-588`（`thumb`）；`photo_library.cpp:220-339` | UU 侧无相册资源名，无法判断 UU 是否用同一形态 |
| 按类型/日期归类落盘 | `file_transfer_item_picture.svg` `0x03a08203`（仅类型图标证据） | 推断 | 有 | `photo_library.cpp` / `file_transfer_manager.cpp` | `photo_library.cpp:428-465`（`Photos/<YYYY-MM>`、`" (n)"` 重名）；`file_transfer_manager.cpp:521-555` | 归类发生在冲突策略之后，语义比 UU 更明确；UU 侧未验证 |
| 缩略图预算 | — | — | 有 | `photo_library.h` / `file_browser.cpp` | `photo_library.h:33-38`（`kTileEdge=192`、`kMaxThumbnailBytes=34000`）；`file_browser.cpp:578-584`（256 条 LRU） | 不适用 |

### 2.11 移动端

| UU 能力 | UU 证据（偏移） | 级别 | 本项目状态 | 实现位置 | 证据（文件:行） | 差距 |
| --- | --- | --- | --- | --- | --- | --- |
| 移动端相册选择 | 未扫描 | 未验证 | 部分 | `apps/ios` | `apps/ios/CrossDeskMobile/PhotoGallery.swift`；`CrossDeskRTCBridge.mm:1069,1866-1874` | iOS 只浏览/下载（`docs/IOS_PHOTO_ALBUM.md:46-50`）；Android 仍单文件 SAF |
| 移动端传输引擎 | `ftp_metadata`（服务端串，推断同栈） | 推断 | 部分 | `mobile_file_transfer_session.h` | `mobile_file_transfer_session.h:19-21`（`Options{true,true,4}`）；iOS 手写 loop（`bridge.mm:1153`） | iOS 不走 `Session`，无 30s 超时与区间表 |

## 3 「行为等价但实现不同」的四层说明

### 3.1 控制面：protobuf RPC ↔ JSON RPC

UU 的控制面是 protobuf 服务（`rpc_file_transfer.proto` 里 `FileTransferFtpRequest/Response`，由 `FileTransferModule::sendRpcRequest/sendRpcResponse` 收发；`docs/PHOTO_GALLERY.md:57`）。本项目在既有的可靠 DataChannel `control_data`（`stream_names.h:18`）上跑一个扁平 JSON 信封：`{"type":"file_browser","version":1,"op":…,"id":…}`，服务端分派在 `file_browser.cpp:590-634`，客户端发送在 `file_transfer_manager.cpp:720-734`。

等价性边界：
- 传输能力对等（列举、准备、取消、下发请求都齐）。
- **不等价的部分**：消息体上限 60000 字节硬编码在两处（`file_transfer_manager.cpp:730`、`:738`），因此分页参数被反向约束——`kPageEntries=512`、`kPageBytes=45000`（`file_browser.cpp:19-20`）。UU 的 protobuf 没有这个天花板，它可以一次回一个更大的清单（`file_transfer_manifest result=accepted file_count=`）。
- 错误码是字符串枚举而非 protobuf enum（`file_browser.cpp:36`），UI 侧做本地化映射。

### 3.2 数据面：FTP 式块传输 ↔ 定长二进制 chunk

UU 的 `file_transfer_ftp.cpp` / `ftp_helper.cpp` + 开关 `ff_file_transfer_ftp(2)` 表明它借用 FTP 语义（`docs/PHOTO_GALLERY.md:58`），加上 `ftp_metadata`（`0x0402006e`）作为伴随元数据。

本项目是自描述定长头：

```
FileChunkHeader 31B : magic('JNTM') file_id offset total_size chunk_size name_len flags
FileTransferAck 28B : magic('JNTC') file_id acked_offset total_size flags
```

- 布局被 `static_assert` 锁死（`file_transfer_format.h:42-45`）：`sizeof(FileChunkHeader)==31`、`sizeof(FileTransferAck)==28`。**任何字段增删都是破坏性变更。**
- chunk 固定 64 KiB（`stream_names.h:26`），1 MiB 未确认窗口（`core.h:139`，`file.sent - file.acked < 1024*1024`），ACK 只确认"从 0 起的连续覆盖"（`core.h:371`）。
- 文件名只在首块携带（`file_transfer.cpp:13`），裸 UTF-8，落盘时只取 basename 并过滤控制字符（`core.h:457-464`）。
- 结果是：没有 FTP 的 `REST`/`APPE` 命令，续传若要实现必须自己加 offset 协商——这正是它没实现的原因（见 3.3）。

### 3.3 续传：temp metadata ↔ 无跨会话续传（重要）

UU 侧证据链完整且互相印证：

- 日志模板 `file_transfer_task_create … resume=`（`0x0401bbaa`）——建任务时带续传意图。
- `file_transfer_resume result=failed reason=metadata_missing path_chars=`（`0x0401c046`）——元数据缺失是**一等失败原因**。
- `reason=source_changed file_name_chars=… extension=`（`0x0401cf38`）、`reason=file_size`（`0x0401d354`）、`reason=partial_file_missing`（`0x0401cf38`）——校验维度是 名字+扩展名+大小+半成品存在性。
- `file_transfer_receive_file result=preparing resumable=`（`0x0401d354`）、`result=started resume_point=`（`0x0401d354`）——接收侧有"可续传"标记与起点。
- `GameViewerServer.exe 0x016cb0a6` 的 `file_transfer_temp_clear result=success request_id=… task_key=`——清理是按 `task_key` 的 RPC。
- UI 侧英文兜底 `File changed; resume not supported.`（`0x04684552`）。
- 半成品用 `.downloading` 后缀（`0x0401c862`）。

本项目当前**没有跨会话续传**，四处证据（本轮复核行号）：

1. `Start()` 不接受起始偏移，状态里写死 0：`core.h:90-111`，`state_ = {file.id, size, 0, Status::Sending}`（`core.h:103`），`file.sent` 从 0 开始。
2. `Pump()` 用 `file.sent == 0` 判首块：`core.h:146`。
3. 暂存目录名随机：`core.h:349-356`（`token = file_id + tick + serial`，`attempt` 最多 100 次），落盘文件是 `.partial`（`core.h:357-358`）。随机名意味着**下一次会话无法定位上一次的半成品**。
4. 成功发布后立即删除暂存目录：`Publish()` 的 `Remove(file)`（`core.h:409`、`:434`），`Remove()` 自身删 `.partial` 与目录（`core.h:467-472`）；`Clear()` 在析构时清空所有状态（`core.h:518-524`）。三个 map（`incoming_`/`destinations_`/`cancelled_incoming_`）都是纯内存（`core.h:541-543`），随 `Session` 消失。

因此"断点续传"在本项目里是**从零重传**，不是续传。

**本轮进展（2026-11-21 08:2x）**：补上了「元数据载体」这一层 —— `libs/controller/include/file_transfer_resume.h`（header-only，无 GUI/peer 依赖）：侧车 `<file>.crossdesk-resume` 原子写、读时做 `total_size`/`mtime` 新鲜度校验、`stale`/`missing`/`malformed` 与 UU 的 `reason=source_changed|file_size|partial_file_missing|metadata_missing` 一一对应、幂等清理、枚举时跳过畸形文件。**但这仍然不是续传功能**：没有 `op=resume` 握手，`core.h:331` 的「首块 `offset != 0` 硬拒」没有放宽（**观测**：条件仍在 `:331`），`Start()` 仍不接受偏移。三种方案（A 预置 ack / B `op=resume` / C 纯侧车）的取舍、必须放宽 `:331` 的理由与三重校验（会话 id + 总大小 + mtime）写在 `docs/RESUME_DESIGN.md` §2-§3。

### 3.4 冲突：两套对话框 + 前置删除 ↔ 引擎不可覆盖 + 发布后策略

UU 的冲突处理是"两套对话框 + 一组文件系统原语"：`file_transfer_collision` / `file_transfer_folder_collision` 两个族名，配套 `existing_file_remove`、`partial_file_remove`、`directory_create`、`directory_remove`、`parent_create`，且日志模板里 `collision result=renamed` 与 `item result=skipped reason=already_exists` 并存——说明它既会改名，也会跳过。

本项目分两层，语义比 UU 更保守：

1. **引擎层永不覆盖**：`Publish()` 先试硬链接（`core.h:407`），失败后独占创建（`wbx`，`core.h:413-415`），重名时追加毫秒时间戳（`core.h:396-404`，最多 1000 次）。所以引擎保证"不丢别人文件"。
2. **桌面层事后改判**：`ApplyTransferOverwritePolicy(published, intended, policy)`（`tools/file_transfer.cpp:24-52`）——`kOverwrite` 删旧再改名（`:31-38`），`kRename` 生成 `" (n)"`（`:40-48`），`kAskSkip` 保持引擎的时间戳名（`:49-51`，注释明确"没有提示"）。

触发点在接收侧完成回调：`SetOnProgress` 拿到最终路径后调 `PublishReceivedFile`（`file_transfer_manager.cpp` 的 `SetOnProgress`/`PublishReceivedFile`，快照 `:1028`）。它有个显式前提：**只有落在接收目录根下的文件才走策略**（同一函数内用 `std::filesystem::equivalent()` 比较父目录，比字符串比较更稳），浏览器下载与多级目录布局不受影响。

差距本质：UU 有"问用户"的层级，本项目只有"配置里预设"的层级。

### 3.5 限速：同名的 ByteTokenBucket

UU 用 `shared::file_transfer::ByteTokenBucket`（符号级证据，`docs/PHOTO_GALLERY.md:61`）。本项目本轮在引擎里实现了同构算法，名字相同但独立编码：

- 配置面：`Options::send_rate_bytes_per_second`（`core.h:34-36`），构造时装载（`core.h:64-68`）。
- 桶：容量 = `max(rate, 1 chunk)`，按 `Pump()` 间隔补信用，最多存 1 秒（`core.h:499-511`）；`Start()` 与 `SetSendRate()` 都清零信用（`core.h:105-108`、`:172-180`），避免换速率后被旧速率"预热"。
- 扣费：块不切分，整块扣（`core.h:154-157`）。
- 接入：`RunFile` 按 `GetTransferRateLimitKbps()*1024` 在创建 session 时设置（`file_transfer_manager.cpp` 的 `RunFile`，快照 `:213-214`）；`ResumeTransfer`/`ResumeTransferItem` 恢复时重设（快照 `:570-579`、`:600-606`、`:665-670`），因此改设置不必重启传输。
- 配置键：`transfer_rate_limit_kbps`（`config_center.cpp:210-212,332-333`），默认 0=不限速，上限 `kTransferRateLimitMaxKbps=1048576`（`config_center.h:157`）。

### 3.6 暂停：三层（peer / item / 队列）对齐 UU 的两层

UU 同时有 `item_pause` 与 `start_all/pause_all`，即**两层**。本项目本轮把三层都补齐了，粒度比 UU 更细：

- 引擎：`SetSendPaused()` 只停止产生新块，并刷新 `file.updated` 让对端不因 30 秒无进展判失败（`core.h:135-138`）；ACK、接收超时、墓碑照常运行（`core.h:184-195` 注释）。
- 队列簿记：`SendQueue::Item.held`（`core.h:565`）是 item 级暂停的唯一真相源，`State::Paused` 由 hold 推导而不是存储（`:556-558`），因此 resume 回到原位；`Pause`/`Resume`（`:651`/`:657`）、`PauseAll`/`ResumeAll`（`:663`/`:672`）。
- 管理器三层：peer 级 hold `paused_remotes_`（`file_transfer_manager.h:205`、`PauseTransfer`/`ResumeTransfer` 在 `.cpp:491`/`:497`、`TransferPaused` `:501`）用于「整条出站流水线」，item 级 `PauseTransferItem`/`ResumeTransferItem`（`.cpp:630`/`:651`），队列级 `PauseAllTransfers`/`ResumeAllTransfers`/`StartAllTransfers`/`ClearAllTransfers`（`.cpp:541`/`:566`/`:596`/`:618`）。
- UI：行内 per-item 暂停/继续按钮（`file_browser.slint:1098-1104`）、队列按钮组（`:650-653`）、peer 级 `toggle-transfer-pause`（`:661`）。

**接收方向仍然没有暂停能力**——这个方向要改 `ReceiveLocked` 的语义，风险更高，本轮未做。

### 3.7 光标与鼠标模式

UU 用 5 个 SVG（`viewing_cursor_*.svg`，`docs/REMOTE_CURSOR.md:12-16`）；本项目用 14 个 64×64 PNG，按 `RemoteCursorShape` 全枚举映射（`uu_cursor.slint:46-61`），热区按源图 64 坐标换算（`:19-37`），系统指针置 `MouseCursor.none`（`stream_window.slint:1110-1112,1160-1162`）。"智能鼠标"的自动回退不是靠一个 flag，而是一台状态机：`CursorPresentation` 记住最后可见形状，在 `system_hidden_ && locator_allowed_` 时继续画（`cursor_presentation.h:63-82`）。这一层比 UU 的单条提示文案（`被控端无法提供鼠标光标，已切换到主控鼠标`，未复现）更细，但实现不同。

## 3.9 本轮续传闭环（覆盖 §3.3 里「仍不是续传」的结论）

- 数据面：`file_transfer_core.h` 新增 `Start(..., resume_offset)`（seek 到起点并把 `file.start/sent/acked` 置为 offset），
  首块判定改为 `file.sent == file.start`，续传首块仍带 `flags & 1` 与文件名（否则接收端 `(!first && name_len)` 会拒）。
- 接收校验：`ResumedStart()` 只认三件事 —— 本机侧车可读、`record.total_size == 首块 total` 且 `record.received == 首块 offset`、
  前缀文件长度 `>= received`（记录带真实戳时还要求戳相等）。其余全部维持原 `reject()`；`SeedResumedPrefix` 只复制 `[0, offset)`。
- 控制面：`file_browser` v1 新增 `resume` op（`path?/name/total_size/mtime_ms?` → `offset/total`），作用域沿用 `temp_clear`
  的 `ContainedIn(ReceiveDirectory())` 守卫，越界一律回 0（回错误本身就会泄露「那边有东西」）。
- 侧车：接收端每 4 MiB 落一条 `.crossdesk-resume` 并把前缀字节复制到接收目录；完成后清记录 + 删前缀，
  失败/取消时只有暂存仍在才保留，否则一并清掉（下一次诚实从 0 开始）。
- 发送端：`RunFile` 在 `Start` 之前问一次（worker 线程、100ms 切片、1.5s 上界），`offset == size` 直接标「已存在」并跳过传输；
  老版本 / 超时 / 畸形答复一律退化从 0 发送；`RetryTransferItem` 重放时会重新询问。
- 目录对齐：管理器把 `FileReceiver::SetResumeDirectory` 与 `FileBrowser::SetReceiveDirectory` 指向同一个接收目录，
  否则控制面永远回 0（这是本轮合并阶段修掉的真实缺陷）。
- 已知缺口：冲突询问弹窗、孤儿暂存巡检定时器、移动端（iOS/Harmony）发送循环未接入偏移。

## 4 未对齐清单

每条给出：缺什么、要改哪些文件、是否改线格式、风险。本轮之后仍有 **8 条**（合并成 §1.3 的 7 组：冲突询问与文件夹冲突算同一组）。上一版列在这里的 7 项（队列级控制、per-item 控制、类型/方向图标、列排序、批量汇总、远端 ops、窗内路径入口）已在本轮落地，行号证据进入 §2；它们的**残余风险**集中在 §4.2，供后续验收时对照。

### 4.1 仍然缺失（真缺口）

**(A) 跨会话续传的协议侧握手（最大缺口）**
- 缺什么：
  1. **握手**：收端在首块到达前无法知道「是哪个文件、双方用哪个 `file_id`、起点是多少」——文件名只在首块携带，`file_id` 是进程内自增、重启归零（`core.h:71-75`），因此必须有一个控制面请求-应答把三件事对齐；
  2. 发送侧「从 offset N 开始」——`Start()` 仍不接受偏移（`core.h:90-111`，`state_` 写死 0 在 `:103`）；
  3. 接收侧「已有 N 字节可复用」的判定——`ReceiveLocked` 每次新建 `Incoming` 都用 `trunc` 打开 `.partial`（`core.h:359`），会话结束即删（`Remove()`，`:467-472`）；
  4. 首块 `offset != 0` 的硬拒（`core.h:331`）——不放宽则任何续传块必被 `reject()`（`:317-329`），放宽则必须配套防伪造。
- 已有（本轮落地，**观测**）：元数据层 `libs/controller/include/file_transfer_resume.h`（侧车 `<file>.crossdesk-resume`，`stale`/`missing` 语义已对齐 UU 的 `reason=source_changed|file_size|partial_file_missing`）。
- 要改：`file_transfer_core.h`（`Start`/`Pump`/`Ack`/`ReceiveLocked`/`AddRange`，见 `docs/RESUME_DESIGN.md` §4 步骤 2-5）、`libs/controller/src/file_browser.cpp`（`Serve` 加 `op=resume`，步骤 6）、`file_transfer_manager.{h,cpp}`（接线，步骤 8）、`apps/ios/CrossDeskMobile/Bridge/CrossDeskRTCBridge.mm`（手写 loop 同步，步骤 9）。
- 改线格式：**否**（结构体不动）。`FileTransferAck.acked_offset` 与 `FileChunkHeader.offset` 本来就存在（`file_transfer_format.h:22-45`），改的是**语义**与接收侧校验。
- 风险：**最高**。三个硬约束：
  1. 放宽 `core.h:331` 等于允许对端从任意偏移写文件（可构造稀疏文件打满磁盘）。必须按 `docs/RESUME_DESIGN.md` §3.3 做三重校验（会话 id + 总大小 + mtime），且期望起点只能来自**本地**侧车，不能来自对端声明。
  2. iOS 手写解析不共享 `Session`，offset 语义一变必须两边同步，否则同一 `file` 线上两端行为不一致。
  3. 元数据是跨会话持久状态，必须处理「用户手动删了半成品 / 源文件被改 / 目标目录换盘」三类事故——侧车层已能给 `missing`/`stale`，但 UI 还没有对应状态（见 (D)）。

**(B) 冲突询问 + 跳过语义**
- 缺什么：`kAskSkip` 当前不提示（`tools/file_transfer.cpp:49-51`）；`skipped reason=already_exists`（`0x0401c240`）没有对应行为——我们不跳过，保留时间戳副本。UU 有两套对话框（`collision` / `folder_collision`）。
- 要改：`tools/file_transfer.cpp`（`kAskSkip` 改为删 `published` 并报 `skipped`）；`file_transfer_manager.cpp`（把 skip 结果写进任务行）；`file_browser.slint` + `gui_application.cpp`（Slint 侧对话框）。
- 改线格式：**否**（纯本地决策，对端早已传完）。
- 风险：中。「跳过」要在 `PublishReceivedFile` 里删文件（快照 `:1028`），删错目录就丢用户数据；现有 `equivalent()` 守卫必须保留并收紧。

**(C) 文件夹冲突**
- 缺什么：远端目录复用即成功（`file_browser.cpp:99-105` 把 `file_exists` 视为 OK），没有「文件夹已存在，是否合并/改名」。
- 要改：`file_browser.cpp`（`Destination()` 增加「已存在」上报）、`file_browser.slint`（第二套对话框）。
- 改线格式：**是（新增字段或 op）**。建议在 `op=prepare` 加 `folder_policy`（零破坏：旧端忽略未知字段）。
- 风险：中-高。`prepare` 语义变化影响所有已发布端；批处理一次最多 64 项（`file_browser.cpp:605`），加字段要同步改 `UploadNext` 的 44000 字节预算（`:684-691`）。

**(D) 移动端：多选、相册上传、iOS 手写 loop、i18n 补全**
- 缺什么：Android 仍单文件 SAF；iOS 相册只浏览/下载（`docs/IOS_PHOTO_ALBUM.md:46-50`）；iOS 不走 `Session`（`mobile_file_transfer_session.h:19-21` 只是配置壳），无 30 s 超时与区间表；本轮新增的按钮/汇总/错误文案（`stale` 提示等）需要 10 语言词条。
- 要改：`MainActivity.java`、`NativeSession.java`、`apps/ios/CrossDeskMobile/PhotoGallery.swift` + `CrossDeskRTCBridge.mm`、`apps/desktop/src/gui/ui/ui_localization.h` + `apps/desktop/src/gui/assets/localization/localization_data.h`（+ `localization/catalog.json`）。
- 改线格式：否（循环调用单文件接口）。
- 风险：中。iOS 侧一旦进入 `file` 线的手写解析路径，任何引擎侧改动都要同步（见 (A) 风险 2）。

**(E) 孤儿暂存目录巡检**
- 缺什么：`op=temp_clear` 落地后仍有一条缺口——进程被 kill 时留下的 `.crossdesk-<id>-<tick>-<serial>` 目录无人回收（暂存目录名随机，§3.3 证据 3）；`Remove()` 只在取消/失败/发布路径执行（`core.h:467-472`）。
- 要改：`file_transfer_manager.cpp`（启动时扫接收目录，用 `ListResumeRecords` 判定「有侧车但无会话」并按 mtime 老化），必要时 `core.h`（暂存目录名可预测化）。
- 改线格式：否。
- 风险：中。「可预测命名」与「多 peer 同 `file_id` 不冲突」（`core.h:349` 注释）冲突，需要把 peer 维度并入目录名，否则两端暂存会互相覆盖。

**(F) 任务列表持久化**
- 缺什么：`CachePath` 下的 `.json`（`0x0401628c`）——重启后传输历史丢失。
- 要改：新增 `file_transfer_history.{h,cpp}`、`file_transfer_manager.cpp`（完成/失败时写、启动时读）、`file_browser.slint`（历史分组）。
- 改线格式：否。风险：低-中（多实例写同一文件需要锁）。

**(G) 非 Windows 被控端的相对位移**
- 缺什么：`game_mouse.cpp:76-87` 在非 Windows 上是 no-op；`mouse_controller.cpp` 是 Windows 专用实现。
- 要改：新增 macOS（`CGEventCreateMouseEvent` + `CGWarpMouseCursorPosition`）与 Linux（XTest）实现。
- 改线格式：否（`relative_move` 已在协议里，`remote_action.h:41-55`）。
- 风险：中（Wayland 无全局指针控制，已有 SKIP 记录，`docs/REMOTE_CURSOR.md:100-103`）。

**(H) 低优先外观项**
- 缺什么：`combo_view` 下拉排序/筛选视图（`0x04621c72`）；被控端背景图（`0x03a06bb1`，无法验证 UU 是否独立窗口）。
- 要改：`file_browser.slint`。改线格式：否。风险：低（纯外观）。

### 4.2 本轮已落地项的残余风险（验收对照）

| 项 | 落地证据 | 是否改线格式 | 残余风险 |
| --- | --- | --- | --- |
| 队列级控制：全部开始/暂停/继续/清除 | `core.h:554`（`SendQueue`）；`file_transfer_manager.cpp:541/566/596/618`；`file_browser.slint:650-653`；`file_browser_window.cpp:377-386` | 否 | `N` 并发把 N MiB 推进同一条 SCTP association；对端移动端 `max_incoming=4` 会直接 `reject()`（`core.h:334-335`）；`concurrent_per_peer_`（`file_transfer_manager.h:214`）默认值需按对端能力下探 |
| per-item 暂停/继续/重试 | `SendQueue::Pause/Resume/Retryable`（`core.h:651/657/610`）；`file_transfer_manager.cpp:630/651/675`；`file_browser.slint:1098-1110` | 否 | 重试必须换新 `file_id`（撞 `cancelled_incoming_` 墓碑 `core.h:302,314-315`）；peer 级 hold（`paused_remotes_`）与 item 级 hold 并存，需明确优先级 |
| 类型图标 + 方向 × 暂停态 | `file_browser.slint:23,25,27,29,1035-1053`；`file_browser_window.cpp:73-121` | 否 | 图标为 FontAwesome 字形，与 UU 的 16 个 SVG 视觉不等价 |
| 列排序箭头 + 任务列表排序 | `file_browser.slint:161,643,649`；`file_browser_window.cpp:376` | 否 | 低 |
| 批量汇总（x/y） | `SendQueue::Summary`（`core.h:569-573,727`）；`file_transfer_manager.cpp:714`；`file_browser.slint:559` | 否 | 汇总文案需 i18n 补全（见 §4.1 D） |
| 远端 mkdir/rename/delete/open/reveal/stat/temp_clear | `file_browser.cpp:746/759/778/804/842/863`，`Serve` 分派 `:924-930`；`file_browser_ops.h:50-78,270-290` | 是（新 op；旧端回 `unsupported`） | 删除是破坏性操作：路径必须留在接收目录内（`ContainedIn`，`file_browser.cpp:849`）、确认要在 UI 侧、`reserve_` 与 `destinations_` 竞态仍需处理；reveal 非 Windows 不可用 |
| 窗内「修改保存路径 / 打开所在文件夹」 | `file_browser.slint:656-657,732,747`；`file_browser_window.cpp:399,407` | 否 | 低（改路径后 `ReceiverFor` 自动重取） |

## 5 验证方式

### 5.1 自动化

```bash
# 线格式 / 能力位 / 鼠标枚举回归（31/28 字节、relative_move=9、supports_photo_gallery 往返）
xmake b crossdesk_wire_test
xmake r crossdesk_wire_test

# 相册 / 缩略图 / 接收归类回归
xmake b crossdesk_photo_test
xmake r crossdesk_photo_test

# 限速（ByteTokenBucket）与暂停开关回归，假传输 + 显式时钟，不 sleep
xmake b crossdesk_transfer_rate_test
xmake r crossdesk_transfer_rate_test

# 本轮新增：续传侧车（原子写/新鲜度/路径越界/幂等清理/枚举跳过）
xmake b crossdesk_transfer_resume_test
xmake r crossdesk_transfer_resume_test
```

依据：
- `crossdesk_wire_test` 断言 `sizeof(FileChunkHeader)==31 && sizeof(FileTransferAck)==28`、`MouseFlag::relative_move == 9`、`supports_photo_gallery` JSON 往返（`libs/wire/tests/wire_test.cpp:44-60,184-200`），目标定义在 `xmake.lua:98-103`。
- `crossdesk_photo_test` 目标在 `xmake.lua:117-125`，入口 `libs/controller/tests/photo_library_test.cpp:168-179`（`TestScan`/`TestThumbnails`/`TestClassify`）。
- `crossdesk_transfer_rate_test` 目标在 `xmake.lua:131-138`，用例见 `libs/controller/tests/file_transfer_rate_test.cpp:106,145,163,175,187,204`。
- `crossdesk_transfer_resume_test` 目标在 `xmake.lua:141-151`（镜像上一块的写法，同样 `add_deps("crossdesk_wire")` 取到 `nlohmann_json`），用例 7 组、95 处 `Expect`：`libs/controller/tests/file_transfer_resume_test.cpp:110`（round trip）、`:155`（stale/missing）、`:182`（malformed/oversized/version）、`:228`（路径越界/暂存前缀/超长名）、`:293`（幂等清理）、`:318`（列表跳过畸形）、`:354`（侧车命名 vs `.crossdesk-*`）。

### 5.2 手工验证：文件传输基础链路

前置：两端都用同一份构建（`FileChunkHeader` 31B 锁死，`file_transfer_format.h:42-45`），一端做被控端。

1. 控制端 → 被控端：会话工具栏打开文件管理（`stream_window.slint:376`），双栏各自导航到一个目录。
2. 单文件上行：选一个 >100 MB 的文件 → `transfer`。预期：任务行出现，进度条推进，速度列每秒刷新（`UpdateProgress`，快照 `:370`）。
3. 中途取消：点"取消"（`file_browser.slint` 的 `cancel-task` 回调 → `CancelBrowserTransfer`，快照 `:1207`）。预期：
   - 发送端任务行 `Cancelled`（`CancelSend`，快照 `:406`）；
   - 被控端收到 `flags=0x04` 取消包（`file_transfer.cpp:42-51`），落盘目录被整个删除（`core.h:297-311` → `Remove()` `:467-472`）；
   - 被控端任务列表出现一次 `Cancelled`（`UpdateReceiveProgress`，快照 `:905`）。
4. 校验落盘：文件字节数 == 源文件字节数；`sha256sum` 两端一致。
5. 冲突重现：同一文件再传一次。预期：**不覆盖**，出现带毫秒时间戳的第二个文件（`core.h:396-406`），或按 `transfer_overwrite_policy` 二次改名（`tools/file_transfer.cpp:24-52`）。
6. 文件夹：拖一个含子目录的文件夹 → 预期逐级建目录（`file_browser.cpp` 的 `Destination`，快照 `:91-107`）+ 逐文件发送（`op=prepare`，32 项/批）。
7. 断链：传输中断开网络 40 秒 → 预期双方 30 秒超时（`core.h:525`；接收侧补发 `flags=2`，`core.h:127`），任务行 `Failed`。
8. 重连后重传：**当前不会自动续传**，会从 0 开始（`core.h:103,146`）——这正是 §4.1 (A) 与 `docs/RESUME_DESIGN.md` 的主题。
9. 队列与本轮新增控件：连续选 5 个文件 → 「全部暂停」→ 行图标变暂停态 → 「全部继续」→ 单行「暂停/继续」→ 失败行点「重试」→ 「清空列表」。预期：并发数不超过 `SetConcurrentTransfersPerPeer`（快照 `:519`）设定值；`task-summary` 显示 x/y；清空后列表空、已落盘文件仍在。
10. 远端 ops：远端面板新建文件夹 → 重命名 → 打开 → 删除（确认框确认的是服务端解析后的路径）；再点「清理临时数据」，预期 `.crossdesk-*` 暂存目录被清空（`TempClearReply`，快照 `:842`）。

### 5.3 手工验证：本轮新增项

| 项 | 步骤 | 预期 | 依据 |
| --- | --- | --- | --- |
| 限速 | 设置 `transfer_rate_limit_kbps = 1024` 后传一个大文件 | 出站约 1 MiB/s（±10%），速度列收敛 | `core.h:499-511`；`file_transfer_manager.cpp` 的 `RunFile`（快照 `:213-214`） |
| 限速生效时机 | 传输中改配置值再点"继续" | 新速率立刻生效（不需要重连） | `ResumeTransfer`/`ResumeTransferItem`（快照 `:570-579`/`:665-670`） |
| 暂停 | 传输中点"暂停" | 进度停住、速度归零；对端不报失败（>30 秒仍活） | `core.h:135-138`；`SendQueue::Pause`（`core.h:651`）；`PauseTransferItem`（快照 `:635`） |
| 继续 | 点"继续" | 从停住处继续，不重传已确认字节 | `SendQueue::Resume`（`core.h:657`）；`ResumeTransferItem`（快照 `:656`） |
| 队列级 | 5 个文件在队列里点「全部开始/全部暂停/全部继续/清空列表」 | 并发受 `concurrent_per_peer_` 限制；暂停不丢行；清空不移除已落盘文件 | `file_transfer_manager.cpp` 的 `StartAllTransfers`/`PauseAllTransfers`/`ResumeAllTransfers`/`ClearAllTransfers`（快照 `:597/:542/:567/:623`） |
| 重试 | 让一个文件失败后点行内「重试」 | 新 `file_id` 重新入队，行不重复 | `RetryTransferItem`（快照 `:680`）、`SendQueue::Retryable`（`core.h:610`） |
| 批量汇总 | 5 个文件里 3 成 2 败 | `task-summary` 显示 x/y 部分成功 | `SendQueue::Summary`（`core.h:569-573`）、`BatchSummary`（快照 `:719`） |
| 远端 ops | 远端面板新建/重命名/删除/打开/属性；点「清理临时数据」 | 每个 op 有独立结果；删除需确认；`temp_clear` 只清接收目录内的 `.crossdesk-*` | `MkdirReply` `:746`、`RenameReply` `:759`、`DeleteReply` `:778`、`StatReply` `:804`、`TempClearReply` `:842`、`HostOpenReply` `:863`；`file_browser_ops.h:270-290` |
| 窗内路径 | 传输窗口点「修改保存路径」/「打开所在文件夹」 | 选中的目录立刻成为接收目录；打开按钮定位到该目录 | `file_browser_window.cpp` 的 `on_change_save_path`/`on_open_save_folder`（快照 `:407`/`:399`） |
| 覆盖策略 | 把 `transfer_overwrite_policy` 依次设 0/1/2，各传一次同名文件 | 0=跳过（当前退化为时间戳名）；1=替换；2=`" (1)"` 后缀 | `tools/file_transfer.cpp:24-52`；`config_center.cpp:213-214` |
| 策略边界 | 经文件浏览器把文件下载到子目录 | 策略**不**生效，保持请求的目录布局 | `PublishReceivedFile` 的 `equivalent()` 守卫（快照 `:1028`） |
| 照片归类 | 传一张 jpg 到接收目录根 | 落在 `Photos/<YYYY-MM>/`；关掉 `photo_group_by_date` 后落在根 | `photo_library.cpp:428-465`；`config_center.cpp:202-203` |
| 相册缩略图 | 打开相册窗口，滚动 | 每 tick ≤3 张在途（`kThumbsPerTick`），单张失败最多重试 3 次 | `photo_gallery_window.cpp:33-34,392-418` |
| 光标 | 远控会话内移动鼠标 | 自绘指针跟手；移出视口消失；`remote_cursor_style=1` 回退系统光标 | `uu_cursor.slint:40-61`；`stream_window.slint:1126-1132` |
| 游戏鼠标 | 模式切到 1，按 `Esc` | 指针被 `ClipCursor` 限制在视口，靠近边缘回中；`Esc` 本地解锁且不转发 | `game_mouse.cpp:36-70`；`gui_application.cpp:6016-6021` |

### 5.4 本轮验收

| 项目 | 验证命令 | 结果 |
| --- | --- | --- |
| `crossdesk_wire_test`（**本轮之前已有**） | `xmake r crossdesk_wire_test` | 未执行（本轮不构建；行格式/能力位断言未变，`file_transfer_format.h:22-45` 的 `static_assert` 仍在） |
| `crossdesk_photo_test`（**本轮之前已有**） | `xmake r crossdesk_photo_test` | 未执行 |
| `crossdesk_transfer_rate_test`（**本轮之前已有**） | `xmake r crossdesk_transfer_rate_test` | 未执行（引擎的限速/暂停未被本轮改动，`core.h:184-194,493-510` 逻辑未变） |
| `crossdesk_transfer_resume_test`（**本轮新增**，本 worker 产出） | `xmake r crossdesk_transfer_resume_test` | 目标已加入 `xmake.lua:140-151`；xmake 未执行。**替代验证**：用本机 MSVC 19.44（`/std:c++17` 与 `/std:c++20`，`/W4 /WX`）以本地 `nlohmann::json` 替身编译 7 组用例并运行，输出 `file transfer resume checks passed`，退出码 0 |
| `crossdesk_file_browser_ops_test`（**本轮新增**，ops worker 产出） | `xmake r crossdesk_file_browser_ops_test` | 目标在 `xmake.lua:158`；未执行 |
| `crossdesk_transfer_queue_test`（**本轮新增**，队列 worker 产出） | `xmake r crossdesk_transfer_queue_test` | 用例文件 `libs/controller/tests/file_transfer_queue_test.cpp` 已存在；快照时 `xmake.lua` 尚无该目标（队列 worker 未提交块），**未执行** |
| `xmake b` 全量桌面构建 | `xmake b crossdesk` | 未执行（本轮四个 worker 都不构建，见硬规则） |

> 说明：「未执行」是本轮的事实（不跑 xmake/cmake/git）；`crossdesk_transfer_resume_test` 的替代验证只覆盖侧车层逻辑与编译告警，不覆盖真实 `nlohmann_json` 的解析路径与 xmake 的依赖解析，二者仍需在下一轮 CI 里跑一遍。

## 附录 A 本轮施工快照（08:2x）

四个 worker 并行施工于 2026-11-21 08:0x–08:4x，全部落地。快照时状态：

| 层 | 已完成 | 证据 |
| --- | --- | --- |
| 引擎（队列） | `SendQueue`：`Track`/`Start`/`Finish`/`Pause`/`Resume`/`PauseAll`/`ResumeAll`/`Retryable`/`Summarize`/`Clear`，`Summary.batch_partial` | `core.h:554-755`（`Summary` `:569-573`、`Retryable` `:610`、`PauseAll` `:663`、`ResumeAll` `:672`、`Clear` `:724`、`Summarize` `:727`） |
| 引擎（续传侧车） | `ResumeRecord` + 原子写 + 新鲜度校验 + 幂等清 + 枚举跳过 | `file_transfer_resume.h:39-46,50,182-200,204-292,317,336-393,396-400,403-417,420-450` |
| 管理器（队列/暂停/批次） | 补满槽位调度、`StartAll`/`PauseAll`/`ResumeAll`/`ClearAll`、per-item 暂停/继续/重试、批次汇总 | `file_transfer_manager.h:31-35,79-97,205-214`；`file_transfer_manager.cpp:121,541,566,596,618,630,651,675,714` |
| 控制器（远端 ops） | `mkdir`/`rename`/`delete`/`stat`/`temp_clear`/`open`/`reveal` + op 名表 + 错误词表 + 暂存目录选择器 | `file_browser.cpp:230,746,759,778,804,842,863,924-930`；`file_browser_ops.h:50-78,102-121,260-290` |
| UI | 队列按钮组、per-item 暂停/重试、类型图标 + 方向箭头 + 暂停角标、任务列表排序、批量汇总、窗内改路径/开目录 | `file_browser.slint:19-35,559,577,597,643,649-657,732,747,1035-1053,1098-1110`；`file_browser_window.cpp:73-121,376-407,710` |
| 测试 | 3 个新目标 + 1 个新用例文件（其余 3 个为既有） | `xmake.lua:140-151`（`crossdesk_transfer_resume_test`）、`:158`（`crossdesk_file_browser_ops_test`）、`libs/controller/tests/file_transfer_queue_test.cpp`（目标待补） |
| 文档 | 续传设计 + 本报告 | `docs/RESUME_DESIGN.md`（新）；`docs/UU_PARITY.md`（本文件） |
| 仍未做 | 真跨会话续传（握手）、冲突询问、任务列表持久化、移动端 i18n/多选、孤儿暂存巡检、非 Windows 相对位移 | §4.1 |

行号会随后续提交漂移；本文所有引用以该快照为准。任何在快照之后改动的文件，应以 grep 重新定位（`file_transfer_core.h` 在本轮被追加了 `SendQueue`，`:548` 之后的行号尤其易漂）。

## 附录 B 引用索引

**UU 侧**

- `docs/UU_FEATURE_EVIDENCE.md`（资源族 → 能力推断表、关键偏移）
- `uu_feature_strings.txt`（152 条原始命中）
- `docs/PHOTO_GALLERY.md:57-63`（上一轮逆向：protobuf RPC / FTP 数据面 / TaskManager / temp_lookup / ByteTokenBucket）
- `docs/REMOTE_CURSOR.md:12-21`（上一轮逆向：光标资源与模式文案）

关键偏移速查：

```
控制面日志   0x0401bbaa task_create(type,overwrite,resume)   0x0401c046 resume/metadata_missing
            0x0401c240 item(skipped/started/source_missing)  0x0401c862 collision/manifest/.downloading
            0x0401cf38 unsafe_*/source_changed               0x0401d354 resumable/resume_point
            0x0401d690 partial_file_remove                   0x0402006e ftp_metadata
            0x0401628c CachePath+.json                       0x03f7b90c file_transfer_test
服务端       GameViewerServer.exe 0x016cb0a6 temp_clear     GameViewerServer.exe 0x017ab5ee ftp_metadata
队列级按钮   0x03a064a5 start_all  0x03a066ef pause_all  0x03a067bb remove_complete  0x03a06903 clear_all
任务级按钮   0x03a07d73 pause  0x03a073db resume  0x03a0747b retry  0x03a07429 terminate
方向/状态    0x03a0806b left  0x03a074c9 right  0x03a0768f left_pause  0x03a075a7 right_pause  0x03a07f7d failed
路径/入口    0x03a06457 modify_path  0x03a06e0d open_path  0x03a07313 refresh  0x04621c72 combo_view
批量结果     0x03a00e73 send_success  0x03a0106b send_part_success  0x03a0120b recv_failed  0x03a01463 recv_part_failed
状态字面量   0x04645b8a/0x04645d3a/0x04645eea paused   0x04684552 File changed; resume not supported.
```

**本项目侧**

- `libs/wire/include/file_transfer_format.h:18-45`（31/28 字节线格式 + `static_assert`）
- `libs/wire/src/file_transfer.cpp:8-90`（编解码与校验）
- `libs/wire/include/stream_names.h:21-26`（`file`/`file_feedback`、64 KiB）
- `libs/controller/include/file_transfer_core.h`（引擎：`Start` 90-111、`Pump` 113-168、`SetSendRate` 169-183、`SetSendPaused` 184-194、`ReceiveLocked` 294-387、`Publish` 388-437、`AddRange` 438-450、`Remove` 467-472、`RefillTokens` 499-511、`Clear` 518-524、超时 525、`SendQueue` 554-755）
- `libs/controller/include/file_transfer_resume.h`（**本轮新增**：`ResumeRecord` 39-46、侧车后缀 50、`ValidName` 122、`ValidRelativeTarget` 147、`ReadTargetState` 182-200、`ReadRecord` 204-292、`TargetMtimeMs` 317、写 336-393、读 396-400、清 403-417、枚举 420-450）
- `libs/controller/include/file_browser_ops.h`（**本轮新增**：op 名表 50-78、错误词表 102-121、`IsStagingDirectoryName` 260、`SelectStagingDirectories` 270-290）
- `apps/desktop/src/tools/file_transfer.h:17-51`、`.cpp:24-52`（覆盖策略）
- `apps/desktop/src/gui/features/file_transfer/file_transfer_manager.h:31-35,79-97,205-214`（批次汇总、队列级/per-item API、`send_queue_`/`paused_remotes_`/`sources_`）
- `apps/desktop/src/gui/features/file_transfer/file_transfer_manager.cpp`（`ProcessQueue` 121、`PauseAllTransfers` 541、`ResumeAllTransfers` 566、`StartAllTransfers` 596、`ClearAllTransfers` 618、`PauseTransferItem` 630、`ResumeTransferItem` 651、`RetryTransferItem` 675、`BatchSummary` 714、接收/发布、`Poll`、相册、RPC）
- `apps/desktop/src/gui/features/file_transfer/file_browser_window.cpp`（`TaskKind` 73-77、字形表 79-95、`TaskKindForName` 121、队列/暂停/重试绑定 376-390、窗内路径 399/407、汇总 710、拖放二次校验）
- `apps/desktop/src/gui/ui/file_browser.slint`（`BrowserTask` 19-35、`task-summary` 559、排序箭头 161、任务回调 643-657、路径按钮 732/747、行内图标与方向 1035-1053、per-item 暂停/重试 1098-1110）
- `libs/controller/include/photo_library.h:33-38,52-65,101-104`、`libs/controller/src/photo_library.cpp:220-339,428-465`
- `libs/controller/src/file_browser.cpp`（服务器分派 `Serve` 882-950 附近：op 表 886-893、`prepare` 894-906、`MkdirReply` 746、`RenameReply` 759、`DeleteReply` 778、`StatReply` 804、`TempClearReply` 842、`HostOpenReply` 863、`HostOpen` 230）
- `libs/wire/include/remote_action.h:41-55,106-126,159-160`（`relative_move`、`CursorState`、`supports_photo_gallery`）
- `apps/desktop/src/gui/runtime/cursor_presentation.h:59-82`（智能回退）
- `apps/desktop/src/platform/common/gui/game_mouse.cpp:36-87`、`apps/desktop/src/platform/windows/input/mouse/mouse_controller.cpp:93-113`
- `apps/desktop/src/config_center/config_center.{h,cpp}`（`remote_cursor_style`/`remote_mouse_mode`/`game_mouse_speed`/`photo_group_by_date`/`file_transfer_save_path`/`transfer_rate_limit_kbps`/`transfer_overwrite_policy`）
- `apps/desktop/src/gui/application/gui_application.cpp:120-133,5841-5885,6016-6021,4908-4993`（速度档、鼠标模式、游戏位移、相册入口）
- `xmake.lua:80-94`（`crossdesk_photo`/`crossdesk_file_browser`）、`:98-112`（wire/keyboard 测试）、`:117-125`（photo 测试）、`:131-138`（限速测试）、`:140-151`（**本轮新增** 续传侧车测试）、`:158`（ops 测试）
- `docs/RESUME_DESIGN.md`（**本轮新增**：三方案对比、`core.h:331` 放宽与三重校验、落地 checklist、未实现边界）

> **来源声明（2026-11-22）**：本表里「桌面」一列的 ✅ 都有 UU Windows 客户端的字节/字符串证据；
> 「iOS」一列只有**光标素材**是观测（UU 自己的 64×64 PNG），其余交互数值（长按时长、滚轮步进、
> 捏合阈值、菜单布局）都是**自研取值**——手上没有 UU 手机端的包或设备，不能声称对齐。
> 逐项来源见 docs/IOS_ASSUMPTIONS.md。

## 13.5 对齐完成度（本轮收口，2026-11-21）

除连接方式（自建 9 位 ID + 密码 + 自建信令/TURN）外，下列能力已与 UU远程 对齐，均有代码与自动验证：

| 能力 | 桌面 | Web/手机 | iOS | 证据 |
| --- | --- | --- | --- | --- |
| 自绘光标 + 鼠标三模式 | ✅ | ✅ | ✅ | uu_cursor.slint、cursors/uu_*.png、game_mouse.cpp |
| 手势全集（单击/长按/双击/双指滚轮/捏合） | — | ✅ | 自研（无 UU 手机端证据） | control.js + 54 条假时钟用例 |
| UU 式悬浮工具栏（5 s 自动隐藏） | — | ✅ | 浮动菜单 | session_controls.js TouchToolbar |
| 传输面板（队列/并发/全部动作/逐项/限速/覆盖/图标/排序/汇总） | ✅ | ✅ | 已实现（布局自研） | 3 条 RPC + transfer_panel.js + TransferPanel.swift |
| 远端文件操作（open/reveal/rename/delete/mkdir/stat/refresh/temp_clear） | ✅ 右键+快捷键 | ✅ 长按+右键 | ✅ 长按菜单（布局自研） | file_browser_window.cpp、file_manager.js |
| 画质档位（流畅/高清/超清）+ 自动降级 | ✅ | ✅ | 复用桌面设置 | video_quality_preset.h + crossdesk_video_quality_test |
| 相册（三端）+ 上传/接收列表 | ✅ | ✅ | ✅ | photo_library.*、photo_gallery.*、PhotoGallery.swift |
| 续传（侧车 + 旁路前缀 + 断点校验） | ✅ | 走宿主 | 桥接已接 | ile_transfer_resume.h、
esume_receive_e2e_test |
| 同名冲突询问 | ✅ | 走宿主 | 走宿主 | conflict_dialog_host.*、overwrite_conflict_test |
| 孤儿暂存/过期侧车巡检 | ✅ | — | — | SweepOrphans、orphan_sweep_test |

未对齐（明确不做/下一轮）：	emp_clear 的 iOS 入口、跨进程续传的接收半程端到端用例、多人会话的队列分组视图。
