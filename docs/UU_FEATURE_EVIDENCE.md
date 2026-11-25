# UU 远程特性证据（静态提取）

> 目标：`D:\UU远程\GameViewer`（GameViewer 4.42.0.2770 安装目录）
> 方法：只读字节扫描（UTF-8 与 UTF-16LE 字符串，记录文件内偏移），不执行目标程序、不读进程内存。
> 工具：`extract_uu_strings.py`（工作区根目录），原始命中 `uu_feature_strings.txt`（152 条）。
> 结论分级：**观测**＝字符串/资源名存在于二进制；**推断**＝由资源命名推得的交互能力，标注为推断。

## 1. 文件传输界面的能力清单（观测 → 推断）

UU 的文件传输 UI 资源是成套命名的，族名本身就构成功能清单（下列偏移为 `GameViewer.exe` 文件内偏移）。

| 资源族 | 观测命中 | 推断的能力 | 我们当前状态 |
| --- | --- | --- | --- |
| `file_transfer_item_pause` / `_resume` / `_retry` / `_terminate` | 各 x3 | 单任务：暂停、继续、重试、终止 | 暂停/继续本轮在加（`Session::SetSendPaused`）；重试/终止无 |
| `file_transfer_item_arrow_left` / `_left_pause` / `_right` / `_right_pause` | 各 x1 | 任务方向（发送/接收）× 是否暂停的状态图标 | 无（只有 `direction` 文本） |
| `file_transfer_start_all` / `_pause_all` / `_resume` | x4/x4/x4 | **全部开始 / 全部暂停 / 全部继续**（队列级） | 无（单文件串行） |
| `file_transfer_remove_complete` / `file_transfer_clear_all` | x4/x4 | 清除已完成、清空列表 | 无 |
| `file_transfer_item_select_all` / `_checked` / `_unchecked` | x1/x3/x3 | 多选、全选，批量操作 | 部分（浏览器多选，传输列表无） |
| `file_transfer_collision` / `file_transfer_folder_collision` | 各 x1 | **同名文件冲突 / 同名文件夹冲突** 两套对话框 | 本轮加覆盖策略（0 跳过/1 覆盖/2 重命名），无对话框 |
| `file_transfer_existing_file_remove` / `file_transfer_partial_file_remove` / `file_transfer_directory_create` / `_remove` / `file_transfer_parent_create` | 各 x1 | 覆盖前删除既有文件、清理残留半成品、递归建目录、失败回滚 | 无 |
| `file_transfer_temp_clear` | x2 | **残留临时文件清理**（续传元数据） | 无 |
| `file_transfer_manifest` | x1 | 传输清单（文件夹展开后的条目列表） | 部分（浏览器 `list` 分页） |
| `file_transfer_modify_path` / `file_transfer_open_path` | 各 x3 | 修改保存路径、打开所在文件夹 | 无按钮（只有设置里的默认接收目录） |
| `file_transfer_refresh` | x3 | 列表刷新 | 无 |
| `file_transfer_item_*`（pdf/word/excel/ppt/text/picture/music/video/folder/disk/link/desktop/document/download/my_computer/other） | 各 x1 | 按扩展名的类型图标 + 常用位置快捷入口（桌面/文档/下载/此电脑/磁盘） | 无（纯文本列表） |
| `file_transfer_item_header_ascending` / `_descending` | 各 x1 | 列表列排序 | 无 |
| `file_transfer_item_create` / `_new_folder` / `_rename` / `_delete` / `_open` | x4/x1/x1/x1/x3 | 远端新建文件夹、重命名、删除、打开 | 无（
ile_browser.cpp 的 Serve() 只有 list/gallery/thumb/list_cancel/prepare/cancel/copy，没有 mkdir/rename/delete/open） |
| `file_transfer_send_success` / `_send_part_success` / `_recv_success` / `_recv_failed` / `_recv_part_failed` / `_warning` | 各 x1 | 批量结果的**部分成功**状态（x/y 成功） | 无（逐文件状态） |
| `file_transfer_loading` / `file_transfer_controlled_bg` / `file_transfer_combo_view` | x1/x2/x4 | 加载态、被控端背景、下拉视图（排序/筛选） | 无 |
| `file_transfer` / `new_menu_file_transfer` / `file_transfer_menu` | x5/x4/x3 | 主菜单入口与二级菜单 | 有入口（主窗口文件传输） |

关键观测偏移（`GameViewer.exe`）：

```
0x03a066ef  file_transfer_pause_all_disabled.svg      → 队列级"全部暂停"
0x03a064a5  file_transfer_start_all_hover.svg         → 队列级"全部开始"
0x03a0106b  file_transfer_send_part_success.svg       → 批量"部分成功"
0x03a0120b  file_transfer_recv_failed.svg             → 接收失败态
0x039fdd65  file_transfer_disabled.svg                → 传输按钮禁用态
0x03a067bb  file_transfer_remove_complete_hover.svg   → 清除已完成
0x03a06903  file_transfer_clear_all_disabled.svg      → 清空列表
0x03a06a27  file_transfer_menu_hover.svg              → 传输入口菜单
0x03a06457  file_transfer_modify_path_normal.svg      → 修改保存路径
0x03a06b39  file_transfer_open_path_hover.svg         → 打开所在文件夹
```

`file_transfer_collision`、`file_transfer_folder_collision`、`file_transfer_temp_clear`、`file_transfer_manifest` 的命中说明：UU 的传输层确实区分「文件冲突 / 文件夹冲突 / 临时文件清理 / 清单」四件事，这与之前逆向得到的 `file_transfer_temp_lookup`（`metadata_missing`）、`file_transfer_temp_clear` RPC 名称一致，构成**续传 + 冲突处理**的双重证据。

## 2. 与之前逆向结论的交叉验证

| 之前逆向得到的符号 | 本次静态证据 | 一致性 |
| --- | --- | --- |
| `file_transfer_temp_lookup` / `metadata_missing` | 资源族 `file_transfer_temp_clear`、`file_transfer_partial_file_remove` | 一致（有临时文件与清理语义） |
| `shared::file_transfer::ByteTokenBucket` | 未见同名资源（限速是纯逻辑，无 UI 资源） | 不冲突；限速仍以符号证据为准 |
| `FileSenderTask` / `FileReceivingTask` + `readNextBlock` | `file_transfer_item_arrow_left/right`（方向）、`_pause`（任务级暂停） | 一致（任务是有方向、可暂停的单位） |
| `ff_file_transfer_ftp(2)` | `file_transfer_manifest`（清单） | 推断一致（FTP 数据面 + 清单项） |
| 光标 `viewing_cursor_{normal,hand,grab,disabled,pen}.svg` | 本次未扫描（范围限定文件传输） | — |

## 3. 由此确定的落地方向（按 UU 逐项对齐）

1. **任务级**：暂停/继续/重试/终止、方向图标状态（本轮已落：`SetSendPaused` + 限速）。
2. **队列级**：全部开始/全部暂停/清除已完成/清空（需要把「每 peer 一个串行文件」改成任务队列，见 `docs/UU_PARITY.md` 差距表）。
3. **冲突处理**：同名文件与同名文件夹分开处理，覆盖前删既有/删半成品（本轮落 `transfer_overwrite_policy`，对话框未做）。
4. **续传**：临时清单 + `temp_clear`（需要协议握手扩展，当前线格式无 offset 协商）。
5. **列表体验**：类型图标、列排序、常用位置入口、内部路径修改/打开、刷新。

## 3.1 更正记录

- 2026-11-21：`file_transfer_item_create` / `_new_folder` / `_rename` / `_delete` / `_open` 一行原写「部分（浏览器已有建目录/删除）」，经 `libs/controller/src/file_browser.cpp` 的 `Serve()` 复核，实际没有 mkdir/rename/delete/open 这些 op，已改为「无」。更正来源：`docs/UU_PARITY.md` 逐项对照表。

## 4. 证据边界

- 全部为**静态字符串**观测，未执行 GameViewer，未做运行时抓包；因此「按钮存在」不等于「该功能在某版本可用」，行为细节仍需运行验证。
- 中文界面文案未在二进制里出现（`utf-16le` 扫描 CJK 命中 0），说明文案走外部语言资源或加密资源，本次未定位；上表能力名是资源族名的直译，不是 UU 的原文文案。
- 偏移仅对本次安装的 `GameViewer.exe`（74.5 MB）有效，版本变化会失效。
