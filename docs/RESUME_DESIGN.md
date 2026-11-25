# 跨会话续传设计（不改变线格式）
> **状态（2026-11-21）：A + B 已闭环落地。** 数据面（`Start(..., resume_offset)`、`ResumedStart` 校验、前缀种子复制）、控制面（`resume` op）、侧车层（`file_transfer_resume.h`）、接收端前缀持久化与清理、管理器 1.5s 上界询问与 `offset == size` 短路全部实现；四个相关测试全绿（`crossdesk_transfer_resume_test`、`crossdesk_file_browser_resume_test`、`crossdesk_transfer_resume_handshake_test`，另有 wire/photo/rate/queue/ops 共 8 个测试套件全通过）。仍缺：冲突询问弹窗、孤儿暂存巡检定时器、移动端发送循环的偏移支持。


- 仓库：`C:\Users\Mayn\Documents\deepseek-harness\default-workspace\crossdesk`
- 快照：2026-11-21 08:2x（工作树并行施工中；本文件行号以该快照为准，标注「行号漂移，按函数名定位」者以函数名为准）
- 本轮产出（可验证）：`libs/controller/include/file_transfer_resume.h` + `libs/controller/tests/file_transfer_resume_test.cpp` + `xmake.lua` 的 `crossdesk_transfer_resume_test` 目标
- 本文的证据标记沿用 `docs/UU_PARITY.md` §0：**观测**（代码/二进制里能指到行号或偏移）、**推断**（由结构推得，未运行验证）、**未验证**（找不到可引用证据）

---

## 1 目标与约束

### 1.1 目标

1. 同一个文件在**两个会话之间**继续传：会话 A 传了 60% 断掉，会话 B 只传剩下的 40%，接收端最终字节与源文件一致。
2. **不改线格式**：`FileChunkHeader` 31 字节、`FileTransferAck` 28 字节、`kFileChunkSize` 64 KiB 一个字节都不能动。
3. 两端版本**可能不一致**（老客户端 ↔ 新被控端，或反之）。任何新行为必须能在对端不认识时**退化为从 0 重传**，且不产生错误弹窗、不产生损坏文件。
4. 本轮先把「安全的、可复用的、有测试的」那一层做出来：续传元数据（侧车）层。

### 1.2 硬约束（观测）

| 约束 | 证据 | 含义 |
| --- | --- | --- |
| `FileChunkHeader` = 31 B、`FileTransferAck` = 28 B，布局被 `static_assert` 锁死 | `libs/wire/include/file_transfer_format.h:22-45` | 加字段 = 破坏性变更；只能复用既有字段的**语义**或走控制面 |
| chunk 固定 64 KiB | `libs/wire/include/stream_names.h:26`（`kFileChunkSize`） | 续传起点必须是 64 KiB 的整数倍才不产生 patchy 对齐问题（本设计不需要跨块切分） |
| 数据面 `file`，反馈面 `file_feedback` | `stream_names.h:21-22` | 任何「预置 ack」都走 `file_feedback`，不会新增 stream |
| `Start()` 不接受起始偏移，状态写死 0 | `libs/controller/include/file_transfer_core.h:90-111`，`:103`（`state_ = {file.id, size, 0, Status::Sending}`） | 发送端目前只有「从 0 开始」这一种形态 |
| 首块判定是 `file.sent == 0` | `core.h:146` | 偏移改造点之一 |
| 接收端硬拒首块 `offset != 0` | `core.h:330-336`，条件在 `:331`（`first && h.offset != 0`） | 任何 `offset > 0` 的首块今天一定被 `reject()` |
| 暂存目录名随机 `.crossdesk-<id>-<tick>-<serial>` | `core.h:349-356` | 下一次会话**无法定位**上一次的半成品 |
| 发布后即删暂存 | `core.h:388-437`（`Publish`）→ `:467-472`（`Remove`）；析构 `:518-524`（`Clear`） | 三个 map（`incoming_`/`destinations_`/`cancelled_incoming_`）纯内存，随 `Session` 消失 |
| ACK 只承认「从 0 起的连续覆盖」 | `core.h:372`（`file.received` 取 `ranges` 中起点为 0 的段） | 断点必须是从 0 起的连续前缀，不能是「中间挖洞」 |
| 发送端只接受 `acked_offset <= file.sent` | `core.h:203`，并且 `:212`（`if (ack.acked_offset <= file.acked) return;`） | 预置 ack（方案 A）必须改这两处判定 |

### 1.3 一个容易忽略的事实：`file_id` 不是会话身份

`Session::NextFileId()` 是**进程内**自增（`core.h:71-75`），重启后从 1 重新开始，两个并发 peer 也可能拿到同一个值。所以：

- `wire_id` 只能当**会话内**的句柄，不能当跨会话的主键（**观测**）。
- 跨会话的身份必须是 `(目标文件名, 总大小, mtime)` 三元组，这也是后面「三重校验」的来历（见 §3.3）。

---

## 2 三种可行方案

术语：**收端** = 接收文件的一方（`ReceiveLocked` 的执行者）；**发端** = `Pump` 的执行者。

### 2.1 方案 A：收端在首次 ack 里回已收字节，发端跳过

机制：收端在收到首块**之前**，用 `file_feedback` 主动发一个「我已经有 N 字节」的 ack；发端读到 `acked_offset = N` 后，把发送起点设成 N。

| 项 | 内容 |
| --- | --- |
| 改哪些函数 | 收端：`ReceiveLocked`（`core.h:294-387`）新增「预置 ack」路径；`SendAck`（`core.h:473-477`）复用即可。发端：`Ack`（`core.h:197-216`，需放开 `:203` 的 `acked_offset > file.sent` 与 `:212` 的单调递增）、`Start`（`core.h:90-111` 加起始偏移）、`Pump`（`core.h:146` 首块判定、`:148-149` 的 `is_first` 不能只看 `sent==0`）。 |
| 是否改线格式 | **否**。`FileTransferAck.acked_offset` 字段本来就在（`file_transfer_format.h:33-39`），只是从来没人在 `file.sent == 0` 时发过它。 |
| 兼容老版本 | 老**发端**：收到 `acked_offset > 0` 的预置 ack 时，`core.h:203` 的 `ack.acked_offset > file.sent` 成立（`file.sent` 刚 0）→ 直接 `return` 丢弃，于是从 0 重传。老**收端**：不会发预置 ack，新发端从 0 开始，老收端正常接收。两条路都降级为「从 0 传」，**不报错**。 |
| 失败 / 伪造风险 | **中-高**。预置 ack 是一份「未经请求的信用」：任何人（伪造的收端）都能宣称 `acked_offset = total_size - 一个块`，让发端只发最后一块，落盘就是一个**稀疏文件**（前面全是 0），用户看到的是「传成功但文件坏」。必须用 `total_size` 精确相等 + 会话绑定做门槛，且收端必须**自己**校验侧车（`mtime`）后才敢宣称前缀有效。 |
| 预计工作量 | 中（**推断**）：引擎 4 处改动 + 收端一个「启动时读侧车」的入口 + 桌面接线约 2-3 天量级。不含控制面握手。 |
| 遗留问题 | 收端凭什么在收首块前就知道 `file_id`/`total_size`/文件名？数据面首块才带文件名（`libs/wire/src/file_transfer.cpp:13` 附近，文件名只在首块）。**所以 A 必须有一个控制面的触发**，否则预置 ack 无法构造 —— 这正是方案 B 存在的理由。 |

### 2.2 方案 B：新增一个 `resume` 控制 op，先协商偏移

机制：在既有 JSON RPC（`control_data` 上的 `type=file_browser` v1）里新增 `op=resume`：客户端带 `(name, total_size, mtime, received)`，服务端校验自己的侧车，回 `{ok, resume_offset}` 或 `{ok:false,error:"stale"}`；随后数据面按 A 的偏移发。

| 项 | 内容 |
| --- | --- |
| 改哪些函数 | 服务端分派：`FileBrowser::Serve`（`libs/controller/src/file_browser.cpp` 的 `Serve`，快照 `:882-950`；`op` 分派 `:886-930`，op 名与错误词表在 `libs/controller/include/file_browser_ops.h:50-78,102-121`）；客户端发起：`FileBrowser::Copy`（`libs/controller/include/file_browser.h:52-53`）或新增 `FileBrowser::Resume`；路径安全复用 `Text`（快照 `:90`）、`ExistingDirectory`（`:98`）、`ExistingEntry`（`:160`）。桌面接线：`FileTransferManager` 的发送入口（`ProcessSelectedFile`/`ProcessQueue`/`RunFile`）。 |
| 是否改线格式 | **否**。JSON 信封加字段/加 op，老端不解析未知字段。 |
| 兼容老版本 | 老**被控端**收到 `op=resume` → 走 `:629` 的 `unsupported`；客户端据此从 0 传。老**控制端**不发 `op=resume`，新被控端只当普通传输处理。**完全向后兼容**（**观测**：`unsupported` 分支已存在）。 |
| 失败 / 伪造风险 | **中**。多一个 round-trip 就多了「双方对 offset 认知不一致」的窗口；数据面仍然要放宽 `core.h:331`，所以伪造面同 A。另有一个真实的失败面：控制面与数据面的 `file_id` 必须对上，否则收端 `AddRange` 会拒绝（`core.h:364-365`）。 |
| 预计工作量 | 中-大（**推断**）：A 的全部改动 + 一个新 op 的两端实现 + 一次性兼容测试。约 3-5 天量级。 |

### 2.3 方案 C：完全侧车方案，发端从 0 重发，收端丢弃已收前缀

机制：不动协议、不动 ack 语义。收端在会话 B 读到侧车（`received = N`）后，把已有前缀当作「已接受」，发端**仍然从 0 发**，收端把 `[0, N)` 区间的块视为重复并丢弃（而不是像今天这样 `reject()`）。

| 项 | 内容 |
| --- | --- |
| 改哪些函数 | `ReceiveLocked`（`core.h:337-362`）新建 `Incoming` 时不能 `trunc` 打开（`:359` 是 `std::ios::trunc`），要改成 `in`+`out` 并保留已有字节；`:364-365` 的 `!AddRange(...) → reject()` 要区分「重叠重复」与「非法重叠」；`AddRange`（`core.h:438-450`，重叠判定在 `:441`）要能返回「重复」而不是 `false`。**不需要**碰 `Start`/`Pump`/`Ack`，**不需要**碰 `core.h:331`。 |
| 是否改线格式 | **否**，连语义都不动：发端行为与老版本完全一致。 |
| 兼容老版本 | 双向无感。老收端没有侧车 → 从 0 开始，正常。老发端永远从 0 发 → 新收端靠「丢弃前缀」自然工作。**这是唯一对老版本零风险的方向**。 |
| 失败 / 伪造风险 | **低**。没有新的信任边界：没有任何一方能凭一个 ack 让对端跳过数据。伪造只能伪造「我自己本地已有多少字节」，损人只损自己。代价是**浪费带宽**（已传的 60% 重传一遍）。 |
| 预计工作量 | 中（**推断**）：`AddRange` 语义 + `Incoming` 打开方式 + 侧车读写的接线，约 2 天量级。 |
| 遗留问题 | 「重复块」与「恶意重叠块」必须能区分，否则等于给了一个「往任意偏移写」的入口（虽然只影响收端自己的文件）。判定标准：只允许 `[0, N)` 且 `N` 来自侧车，其余仍 `reject()`。 |

### 2.4 对比汇总

| 维度 | A 预置 ack | B 控制面协商 | C 纯侧车 |
| --- | --- | --- | --- |
| 改线格式 | 否（复用 `acked_offset`） | 否（JSON 新 op） | 否（零语义变更） |
| 放宽 `core.h:331` | **必须** | **必须** | 不需要 |
| 老版本行为 | 降级从 0 传，静默 | 降级从 0 传，`unsupported` | 无感，完全一致 |
| 带宽 | 只传剩余 | 只传剩余 | 重传全部 |
| 伪造面 | 中-高（预置信用） | 中（同 A + 偏移不一致） | 低 |
| 能够「只传剩余」 | 是（前提：B 提供触发） | 是 | 否 |
| 工作量（推断） | 中 | 中-大 | 中 |

---

## 3 推荐方案

### 3.1 结论

**主路径 = B（触发）+ A（数据面），C 作为不需要放宽 `core.h:331` 的第一里程碑。**

理由（按重要性）：

1. **A 单独不成立**：收端在首块之前不知道 `file_id`/`total_size`/文件名（文件名只在首块携带，`libs/wire/src/file_transfer.cpp:13` 附近），构造不出预置 ack。必须有控制面先说清楚「是哪个文件的哪一次传输」。这一步天然是 `op=resume`（B），而 B 的兼容路径（未知 op → `unsupported`，`libs/controller/include/file_browser_ops.h:102`）已经存在，老端零改动。
2. **A + B 才能真省带宽**：C 虽然最安全，但每断一次就把已传部分重传一遍；移动网络/大文件场景里这条不可接受（**推断**，无实测数据）。
3. **C 仍有独立价值**：它不需要放宽 `core.h:331`，可以在 A 落地前先把「半成品 + 侧车」的落盘链路跑通（侧车读写、`temp_clear`、孤儿清理），把风险拆成两段。本轮已经把 C/A/B 共用的**侧车层**做完（见 §4 步骤 1）。

### 3.2 为什么 `core.h:331` 必须放宽

`core.h:331` 的 `(first && h.offset != 0)` 是今天唯一挡住「对端从任意偏移写我的文件」的校验（**观测**；任务书与旧文档写作 `core.h:330`，那是 `const bool first/last` 那一行，实际条件在第 331 行）。只要采用 A 或 B，首块就会带着 `offset = N > 0` 到达；如果不放宽，续传的每一块都会走到 `reject()`（`core.h:317-329`）→ 收端删暂存、通知 `Failed`、回 `flags=2`（`core.h:328`），发端 `Ack` 看到 `flags & 2` 直接 `FinishSend(false)`（`core.h:204`）。**不放宽 = 续传功能不存在**。

但直接删掉这个条件等于开门：对端可以宣称「我已经有 `total_size - 64 KiB` 了」，让本地文件变成一个几乎全是空洞的稀疏文件，磁盘被占满、内容是垃圾，用户看到「成功」。

### 3.3 放宽后如何防伪造：三重校验

放宽的形式必须是「`offset` 只能等于**本地**认定的续传起点」，而不是「`offset` 随便」。

```
ExpectedStart(file_id, name, total_size) =
    本地侧车里存在一条记录
    且 record.total_size == h.total_size          // ① 总大小一致
    且 record.target_path 的 basename == name      // ② 目标身份一致
    且 record.target_mtime_ms == 目标文件当前 mtime // ③ 目标未被改动
    且 record.wire_id 属于本次会话（由 op=resume 协商确定） // ④ 会话绑定
  ? record.received
  : 0;   // 其余一切情况：期望起点 0，非 0 首块照旧 reject()
```

四项各自挡住的东西：

| 校验 | 挡住的攻击/事故 | 载体 |
| --- | --- | --- |
| ① 总大小 | 用「大文件记录」去续一个小文件（或相反），造成长度错位 | `FileChunkHeader.total_size`（线格式已有，`file_transfer_format.h:27`） |
| ② 目标身份 | 把 A 文件的侧车当成 B 文件的（同名不同目录/不同文件） | `ResumeRecord.target_path` 的 basename 必须等于 `name`（`file_transfer_resume.h` 的读取路径已强制） |
| ③ mtime | 源文件在两次会话之间被改过——UU 的 `reason=source_changed` 正是这一类（`docs/UU_FEATURE_EVIDENCE.md` 的 `file_transfer_temp_lookup`/`metadata_missing` 族证据，**推断**）；不改就续 = 拼出一个前后不一致的文件 | `ResumeRecord.target_mtime_ms` + `TargetMtimeMs()`（与校验用的取值同源，不会「写时一个值、比时另一个值」） |
| ④ 会话绑定 | 跨会话重放旧 ack / 另一条连接上的 `wire_id` 撞号 | `ResumeRecord.wire_id` 只作会话内提示；真正的绑定由 `op=resume` 的请求-应答顺序确定（`file_id` 全局自增、重启归零，见 §1.3，不能单独当主键） |

补充两条实现级护栏（成本极低，收益明确）：

- **侧车只能落在收端自己的目录里**：`ValidRelativeTarget()` 拒 `..`、绝对路径、盘符、以及引擎暂存前缀 `.crossdesk-`，所以一个伪造侧车也无法指向别的传输的暂存目录。
- **mtime 与大小不匹配只报 `stale`，不删文件**：调用方收到 `stale` 就清侧车、从 0 重传；已收字节不丢（用户还能手工救回）。

---

## 4 落地步骤（可执行 checklist）

> 每步都给出「改哪里」和「怎么验证」。验证方式分三类：**单测**（xmake 目标）、**手工**（两端联调）、**日志断言**（按 key=value 打日志后用 grep 卡）。

### 步骤 1（本轮已完成）侧车层

- 文件：`libs/controller/include/file_transfer_resume.h`（新增，header-only）
- 接口：`WriteResumeRecord` / `ReadResumeRecord` / `ClearResumeRecord` / `ListResumeRecords`；辅助 `ResumeSidecarName` / `IsResumeSidecarFilename` / `TargetMtimeMs`
- 侧车名：`<file>.crossdesk-resume`（**后缀**，与引擎暂存目录的 `.crossdesk-<id>-<tick>-<serial>` 前缀形式永久不冲突）
- 验证：**单测** `xmake r crossdesk_transfer_resume_test`（dump 场景：round trip / stale / malformed / escape / idempotent clear / list-skip / naming）

### 步骤 2 收端写侧车

- 改：`file_transfer_core.h` 的 `ReceiveLocked`（`core.h:337-362` 新建 `Incoming` 之后）在每次 `AddRange` 成功后调用 `WriteResumeRecord(file.folder, {...})`，`received` 取 `file.received`（`:372` 的连续前缀值，不是写入总量）；`Remove()`（`:467-472`）与 `Publish()` 成功路径（`:408`/`:433`）改成「删 `.partial` 前先 `ClearResumeRecord`」。
- 频度：按「每 N 块或每 M 秒」限流，避免每 64 KiB 一次 `fsync`（**推断**：逐块写会明显拖慢小文件）。
- 验证：**单测**（假传输 + 显式时钟，断言侧车存在且 `received` 单调）；**日志断言** `resume_write name=… received=… total=…`。

### 步骤 3 收端恢复入口

- 改：`file_transfer_core.h` 新增 `std::optional<uint64_t> AdoptResume(uint32_t file_id, const std::string& name, uint64_t total_size)`：内部 `ReadResumeRecord`，命中则把 `Incoming` 的 `.partial` 以 `std::ios::in | std::ios::out` 打开（替换 `core.h:359` 的 `trunc`），先 `AddRange(0, received)` 再允许收块；`stale`/`missing` 则清侧车、从 0 开始。
- 验证：**单测**（手工造一个 40 字节的 `.partial` + 侧车，喂一个 `offset=40` 的块，断言最终文件字节正确）；**手工**（传到一半 kill 进程，重连重传，比对 `sha256sum`）；**日志断言** `resume_hit` / `resume_stale` / `resume_missing`。

### 步骤 4 放宽首块校验（`core.h:331`）

- 改：把 `(first && h.offset != 0)` 换成 `(first && h.offset != ExpectedStart(h.file_id, chunk.file_name, h.total_size))`，`ExpectedStart` **只读本会话已登记的状态**（步骤 3 的 `Incoming` 记录），不读任何来自对端的声明。
- 同时改 `core.h:364-365`：`AddRange` 失败仍是 `reject()`，但对「与已登记前缀完全重叠」的块返回「重复」并丢弃（C 方案路径）。
- 验证：**单测** 三例 —— (a) 首块 `offset=0` 仍接受（回归）；(b) 首块 `offset=received` 接受；(c) 首块 `offset=total_size-64KiB` 且无侧车 → `reject()`（伪造用例）。**手工**：把发端故意改成「首块 offset = total-64KiB」，确认对端 `Failed` 而不是写出稀疏文件。

### 步骤 5 发端起点

- 改：`Start()`（`core.h:90-111`）加 `uint64_t start_offset = 0`，`state_` 与 `Outgoing.sent/acked` 初始化为 `start_offset`；`Pump()`（`core.h:146`）首块判定 `file.sent == file.start`；`Ack()`（`core.h:203`/`:212`）允许 `acked_offset >= file.start` 的第一次 ack，并保留「`acked_offset` 单调不减」。
- 验证：**单测** 扩展 `crossdesk_transfer_rate_test` 风格（假传输断言首块 `offset == start_offset`、限速桶从 `start_offset` 起算不预热）；**手工** 断点后续传的总耗时/流量明显小于全量（`netstat`/日志 `sent_bytes`）。

### 步骤 6 控制面 `op=resume`

- 改：服务端 `FileBrowser::Serve`（`file_browser.cpp` 的 `Serve`，快照 `:882`；行号漂移，按函数名定位）在 op 分派里加 `resume`，op 名与新错误码进 `file_browser_ops.h` 的表；路径与参数一律走 `Text`（快照 `:90`）+ `ExistingDirectory`（`:98`）+ `ExistingEntry`（`:160`）；客户端加 `FileBrowser::Resume`，并把结果（`resume_offset`）交给步骤 5 的 `Start`。
- 验证：**单测**（`crossdesk_file_browser_ops_test`，由 ops worker 拥有）；**手工** 用一个「不认识 resume」的老构建当被控端，确认回 `unsupported` 且客户端从 0 传。

### 步骤 7 `temp_clear` 与侧车的组合

- 现状（**观测**，本轮已落地）：`op=temp_clear` 的服务端实现是 `FileBrowser::TempClearReply`（`libs/controller/src/file_browser.cpp` 的 `TempClearReply`，快照 `:842-861`；`Serve` 分派 `:928`）。它做三件事：
  1. `ExistingDirectory(RequestText(message,"directory"))` + `ops::ContainedIn(root, requested)`（快照 `:843-849`）把作用域限制在**接收目录及其子目录**内；
  2. `ops::SelectStagingDirectories(requested)`（`libs/controller/include/file_browser_ops.h:270-290`）只选真目录、不跟随符号链接、名字必须以 `.crossdesk-` 开头、上限 `kMaxTempClearEntries=256`；
  3. 对每个选中的目录 `remove_all`（快照 `:852-857`）。
- 与本模块的组合关系：侧车文件 `<name>.crossdesk-resume` 就**躺在暂存目录里**（引擎把 `.partial` 放在 `.crossdesk-<id>-<tick>-<serial>/` 下），所以 `remove_all` 暂存目录 = 数据与侧车同时消失，**顺序问题不存在**。侧车名以 `.crossdesk-resume` 结尾、是文件不是目录，也不会被 `SelectStagingDirectories` 误选（它只收目录）。
- 仍然需要 `ListResumeRecords`/`ClearResumeRecord` 的场景（**未实现**）：
  1. 按**记录**粒度清理（UU 的 `temp_clear task_key=` 语义）：只知道目标名、不知道随机暂存目录名时，`ListResumeRecords(receive_dir)` 是唯一的枚举手段，再用 `ClearResumeRecord` 精确删；
  2. 判断「哪些半成品还值得保留」：`ReadResumeRecord` 给出 `stale`/`missing`，可让清理器只删失效的，保留可续传的；
  3. 若将来把侧车移出暂存目录（例如放到接收根目录以便跨会话定位），则必须改成「先 `ClearResumeRecord`、再删数据」，否则崩溃在中间会留下「侧车说有 40 字节、文件已不在」的记录（下次读会报 `missing`，有兜底但不干净）。
- 验证：**手工** 传一半 kill 掉被控端 → 点「清理临时数据」→ 断言暂存目录与侧车都没了，下次传输从 0 开始（而不是「幻影续传」）；**单测**（`crossdesk_file_browser_ops_test`，ops worker 拥有）覆盖 `SelectStagingDirectories` 的符号链接与越界拒绝。

### 步骤 8 桌面接线

- 改：`apps/desktop/src/gui/features/file_transfer/file_transfer_manager.cpp` 的 `ProcessSelectedFile`/`ProcessQueue`（发送侧把 `resume_offset` 传给 `Start`）与 `ReceiverFor`（接收侧在会话建立时调 `AdoptResume`）。
- 验证：`xmake r crossdesk_transfer_queue_test`（队列 worker 拥有）+ 手工端到端。

### 步骤 9 iOS 同步

- 改：`apps/ios/CrossDeskMobile/Bridge/CrossDeskRTCBridge.mm` 的手写解析/发送 loop（`docs/UU_PARITY.md` §4.4 引用为 `:1100-1163`，**未验证**是否已漂移）。
- 理由：iOS 不走 `Session`（`libs/controller/include/mobile_file_transfer_session.h:19-21` 只是配置壳），任何 `offset` 语义变化必须两边同步，否则同一 `file` 线上两端行为不一致（`docs/UU_PARITY.md` §4.4 风险 2）。
- 验证：手工（iOS 收发断链续传）。

### 步骤 10 文档与回归

- 更新 `docs/UU_PARITY.md` §4.4 的 (I)、(J) 两条：从「无」变成「侧车层已落地 + 协商未落地」。
- 回归套件：`crossdesk_wire_test`、`crossdesk_photo_test`、`crossdesk_transfer_rate_test`、`crossdesk_transfer_resume_test`（本轮新增）、`crossdesk_file_browser_ops_test`、`crossdesk_transfer_queue_test`。

---

## 5 本次未实现的部分（明确边界）

| 项 | 状态 | 原因 |
| --- | --- | --- |
| **真跨会话续传** | 未实现 | 没有握手就没有真正的续传：收端无法在首块前获知「是哪个文件、双方用哪个 `file_id`」，发端也无法被要求从偏移 N 开始（§3.1 理由 1）。本轮只交付了它的元数据层。 |
| 收端自动写/读侧车 | 未实现 | 需要改 `file_transfer_core.h`（步骤 2/3），本轮该文件不在我的编辑范围（另一个 worker 持有）。 |
| `core.h:331` 放宽 | 未实现 | 步骤 4；与步骤 2-3 必须同一批落地，单独放宽就是纯风险。 |
| 发送端起始偏移 | 未实现 | 步骤 5；`Start()` 签名变更会影响所有适配器（桌面/iOS/Android）。 |
| `op=resume` 控制面 | 未实现 | 步骤 6；需要改 `FileBrowser::Serve` 的分派与 `file_browser.h` 的客户端接口。 |
| `op=temp_clear` 按记录粒度清理 | 部分（ops worker 已落地 `TempClearReply`，但它按暂存目录 `remove_all`，**没有**调用本模块的 `ListResumeRecords`/`ClearResumeRecord`） | 见步骤 7：目录级清理已经够用；按 `task_key` 的精确清理仍缺。 |
| iOS 手写 loop 对齐 | 未实现 | 步骤 9；需要真机验收，本轮无法验证。 |
| `resume_point` 类 UI 状态（UU 的 `result=started resume_point=`） | 未实现 | 需要先把 `resume_offset` 变成任务行字段，属于 UI worker 的 `BrowserTask` 决策。 |
| 「文件已变更，无法续传」提示（UU 英文兜底 `File changed; resume not supported.`） | 未实现 | 侧车层已经能把这种情况识别成 `stale`，缺的是 i18n 词条与任务行展示。 |

一句话边界：**本轮交付的是「安全的元数据层 + 明确的落地方案」，不是续传功能本身。**任何把本轮描述成「已支持断点续传」的说法都是夸大。

---

## 附录 A 侧车格式

文件：`<dir>/<name>.crossdesk-resume`

```json
{
  "version": 1,
  "target_path": "clips/clip.mov",
  "total_size": 1073741824,
  "target_mtime_ms": 1763000000123,
  "received": 671088640,
  "wire_id": 17
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `version` | int | 当前固定 `1`；不认识就报 `unsupported_version`（不猜） |
| `target_path` | string | **相对** 侧车所在目录；禁 `..`/绝对路径/盘符/引擎暂存前缀；basename 必须等于侧车名去掉后缀 |
| `total_size` | uint64 | 与 `FileChunkHeader.total_size` 必须相等才允许续传（校验 ①） |
| `target_mtime_ms` | int64 | `last_write_time` 的原生计数（毫秒）。**不与人读时间互转**：只在本机做相等比较，取值统一走 `TargetMtimeMs()`（校验 ③） |
| `received` | uint64 | 从 0 起的连续前缀字节数（与 `core.h:372` 的 `file.received` 同义）；必须 `<= total_size` |
| `wire_id` | uint32 | 写记录时的 `file_id`，仅作会话内提示；**不是**跨会话主键（§1.3） |

写入是原子的（`.tmp` + `rename`，rename 失败时退化为「先删后 rename」），读取有 8 KiB 上限。

记录按 `target_path` 的 **basename** 寻址（`target_path` 仍可带子目录，但必须留在同一目录内）：同一目录下两个同名文件共享一个侧车，**后写覆盖先写**；读取时 basename 与侧车名不一致即判 `malformed`，所以覆盖不会被静默信任。引擎的暂存目录每次传输都是新的，实际很难撞上；若将来把侧车搬出暂存目录（放到接收根目录），必须改成以完整相对路径为键。

错误词表（`ReadResumeRecord` 的 `error`）：
`invalid_name`、`not_found`、`io_error`、`malformed`、`unsupported_version`、`invalid_target`、`missing`、`stale`。
其中 `stale` / `missing` 表示「记录本身没坏，但目标对不上了」—— 调用方一律清侧车、从 0 开始。

## 附录 B 本轮可验证产物

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `libs/controller/include/file_transfer_resume.h` | 新增 | header-only，不依赖 GUI/peer 层 |
| `libs/controller/tests/file_transfer_resume_test.cpp` | 新增 | 7 组用例、95 处 `Expect` |
| `xmake.lua` | 修改 | 新增 `crossdesk_transfer_resume_test`（镜像 `crossdesk_transfer_rate_test` 块） |
| `docs/RESUME_DESIGN.md` | 新增 | 本文件 |

验证命令：`xmake b crossdesk_transfer_resume_test && xmake r crossdesk_transfer_resume_test`，期望输出 `file transfer resume checks passed`。

## 状态（2026-11-21 收口）

- 侧车格式、握手校验、断点偏移、限速与暂停：**已闭环**，由 crossdesk_transfer_resume_test、crossdesk_file_browser_resume_test、crossdesk_transfer_resume_handshake_test、crossdesk_resume_receive_e2e_test 覆盖。
- **前缀落盘的位置已改**：durable prefix 一律写 .crossdesk-prefix-<name>，绝不再写最终名——写最终名会在发布时被判「名字被占用」而改名为 
ame_<stamp>.ext，并且会截断用户已有文件。读取端（ResumePrefixBytes）优先旁路名、兼容旧的最终名形态。
- **收尾只删自己写的那份**：DropResumeRecord 按 
esume_prefix_path_ 里记的实际路径删除，不再按名字猜测，因此「刚收到的文件」和「用户的旧文件」都不会被误删。
- 待补：跨进程续传的接收半程（新进程拿着记录接上剩余块）还缺端到端用例。
