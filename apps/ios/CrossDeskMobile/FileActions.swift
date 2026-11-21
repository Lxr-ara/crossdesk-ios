import Foundation
import SwiftUI

// Remote file management for the iOS controller — the phone half of the
// `file_browser` management ops in `libs/controller/include/file_browser_ops.h`.
//
// Transport is the *same* `file_browser` JSON RPC the album and the transfer
// panel already use (`CrossDeskRTCBridge.sendPhotoMessage`), so the bridge keeps
// one request path and one reply path (`rtcBridge(_:didReceivePhotoMessage:)`).
// The parsers here are the shared defensive helpers from `TransferPanel.swift`
// (`TransferJSON`, `TransferFormat`, `TransferFileKind`): there is no second RPC
// path and no second JSON bridge in this file.
//
// Ops called (request field names are the ones `file_browser.cpp` reads):
//
//   list       { path }              -> { path, parent, entries[], truncated }
//   open       { path }              -> { path, opened }
//   reveal     { path }              -> { path, revealed }
//   rename     { path, name }        -> { path, parent }
//   delete     { paths[] }           -> { results:[{path, ok, error}], removed }
//   mkdir      { path, name }        -> { path, parent }
//   stat       { paths[] }           -> { entries:[{path, exists, directory,
//                                       size, modified, modified_ms, link, error}] }
//   temp_clear { directory }         -> { directory, root, removed[],
//                                       removed_count, failed }
//
// The envelope `id` is the RPC sequence number and is owned by
// `RemoteSessionModel` (its own 0x5000_0000 range), never by a row.

// MARK: - Wire vocabulary

/// Every op this view can send. `remove` carries the wire name `delete`; the
/// enum case is spelled differently only so Swift and the C++ agree explicitly.
enum RemoteFileOp: String, CaseIterable {
    case list
    case open
    case reveal
    case rename
    case remove = "delete"
    case mkdir
    case stat
    case tempClear = "temp_clear"

    /// The management block of `kFileManagementOps`. One probe covers all of
    /// them, and each op is still remembered separately when the host answers
    /// `unsupported` (see `RemoteFileBrowserStore.unsupportedOps`).
    static let management: [RemoteFileOp] = [.open, .reveal, .rename, .remove,
                                             .mkdir, .stat, .tempClear]

    var title: String {
        switch self {
        case .list: return NSLocalizedString("刷新", comment: "")
        case .open: return NSLocalizedString("打开", comment: "")
        case .reveal: return NSLocalizedString("在资源管理器中显示", comment: "")
        case .rename: return NSLocalizedString("重命名", comment: "")
        case .remove: return NSLocalizedString("删除", comment: "")
        case .mkdir: return NSLocalizedString("新建文件夹", comment: "")
        case .stat: return NSLocalizedString("属性", comment: "")
        case .tempClear: return NSLocalizedString("清理临时文件", comment: "")
        }
    }
}

/// One reply, wrapped so every caller reads `ok` / `error` the same way.
///
/// `ok` absent is a refusal too: the host always sends `ok`, so a reply without
/// it is malformed and must not be mistaken for success.
struct RemoteFileReply {
    let object: [String: Any]

    var ok: Bool { TransferJSON.bool(object["ok"]) ?? false }
    /// One code from the closed 20-code vocabulary, or "" when the reply
    /// carried none.
    var code: String { TransferJSON.string(object["error"]) }
    /// `unsupported` is terminal *per op*: the host answered and said the op does
    /// not exist, which is a stable fact for the rest of the session.
    var unsupported: Bool { !ok && code == "unsupported" }
    /// The code to report: a refusal without a code becomes `unknown`.
    var failureCode: String { code.isEmpty ? "unknown" : code }
}

// MARK: - Wire model

/// One `entries[]` row of `list` / `stat`.
///
/// `list` rows carry only a `name` (no per-entry path), so the child path is
/// built from the reply's own directory text. Every field is defaulted in
/// `decode`, so a malformed row can only be dropped — it can never throw into
/// SwiftUI.
struct RemoteFileEntry: Identifiable, Equatable {
    let path: String
    let name: String
    let directory: Bool
    let size: Int64
    let modified: String
    let link: Bool

    var id: String { path }

    var sizeText: String {
        directory ? "—" : TransferFormat.size(size)
    }

    var kindSymbol: String {
        directory ? "folder" : TransferFileKind.symbol(forName: name)
    }

    /// `list` reports a bare file name; the separator comes from the parent the
    /// host printed, so a Windows host keeps backslashes and a POSIX host keeps
    /// slashes without this build guessing the platform.
    static func childPath(parent: String, name: String) -> String {
        guard !parent.isEmpty else { return name }
        if parent.hasSuffix("/") || parent.hasSuffix("\\") { return parent + name }
        let separator = parent.contains("\\") && !parent.contains("/") ? "\\" : "/"
        return parent + separator + name
    }

    /// A drive root (`C:\`, `C:/`) or `/` already is an absolute path and must
    /// not be joined onto its parent.
    static func isAbsoluteName(_ name: String) -> Bool {
        if name == "/" { return true }
        if name.hasSuffix(":\\") || name.hasSuffix(":/") { return true }
        return false
    }

    static func decode(_ item: [String: Any], parent: String) -> RemoteFileEntry? {
        let name = TransferJSON.string(item["name"])
        guard !name.isEmpty else { return nil }
        let path = isAbsoluteName(name) ? name : childPath(parent: parent, name: name)
        return RemoteFileEntry(
            path: path,
            name: name,
            directory: TransferJSON.bool(item["directory"]) ?? false,
            size: TransferJSON.int64(item["size"], in: 0...Int64.max) ?? 0,
            modified: TransferJSON.string(item["modified"]),
            link: TransferJSON.bool(item["link"]) ?? false
        )
    }
}

struct RemoteFileListResult {
    var entries: [RemoteFileEntry] = []
    var path = ""
    var parent = ""
    var truncated = false
    /// Vocabulary code, nil on success.
    var error: String?

    static func failure(_ code: String) -> RemoteFileListResult {
        RemoteFileListResult(entries: [], path: "", parent: "", truncated: false, error: code)
    }

    var unsupported: Bool { error == "unsupported" }

    static func parse(_ object: [String: Any]) -> RemoteFileListResult {
        let reply = RemoteFileReply(object: object)
        guard reply.ok else { return .failure(reply.failureCode) }
        let path = TransferJSON.string(object["path"])
        var entries: [RemoteFileEntry] = []
        for item in (object["entries"] as? [[String: Any]]) ?? [] {
            if let entry = RemoteFileEntry.decode(item, parent: path) { entries.append(entry) }
        }
        return RemoteFileListResult(entries: entries,
                                    path: path,
                                    parent: TransferJSON.string(object["parent"]),
                                    truncated: TransferJSON.bool(object["truncated"]) ?? false,
                                    error: nil)
    }
}

/// `stat` answers `entries[]` with one record per requested path. The reply is
/// `ok:true` even for a path that vanished, so `exists` plus the per-entry
/// `error` is what decides.
struct RemoteFileStat: Equatable {
    let path: String
    let directory: Bool
    let size: Int64
    let modified: String
    let link: Bool

    var name: String { TransferFormat.fileName(forPath: path) }
    var sizeText: String { directory ? "—" : TransferFormat.size(size) }

    static func decode(_ item: [String: Any]) -> RemoteFileStat? {
        guard TransferJSON.bool(item["exists"]) ?? false else { return nil }
        guard TransferJSON.string(item["error"]).isEmpty else { return nil }
        let path = TransferJSON.string(item["path"])
        guard !path.isEmpty else { return nil }
        return RemoteFileStat(
            path: path,
            directory: TransferJSON.bool(item["directory"]) ?? false,
            size: TransferJSON.int64(item["size"], in: 0...Int64.max) ?? 0,
            modified: TransferJSON.string(item["modified"]),
            link: TransferJSON.bool(item["link"]) ?? false
        )
    }
}

struct RemoteFileStatResult {
    var stat: RemoteFileStat?
    var error: String?

    static func failure(_ code: String) -> RemoteFileStatResult {
        RemoteFileStatResult(stat: nil, error: code)
    }

    static func parse(_ object: [String: Any]) -> RemoteFileStatResult {
        let reply = RemoteFileReply(object: object)
        guard reply.ok else { return .failure(reply.failureCode) }
        guard let first = (object["entries"] as? [[String: Any]])?.first else {
            return .failure("not_found")
        }
        if let stat = RemoteFileStat.decode(first) {
            return RemoteFileStatResult(stat: stat, error: nil)
        }
        // A vanished entry is the normal refresh case: report `not_found` only
        // when the host did not name a better code.
        let entryCode = TransferJSON.string(first["error"])
        return .failure(entryCode.isEmpty ? "not_found" : entryCode)
    }
}

/// `open` / `reveal` / `rename` / `mkdir` all answer `ok` plus a body this view
/// only reads for errors.
struct RemoteFileOpResult {
    var ok = false
    var error: String?

    static func parse(_ object: [String: Any]) -> RemoteFileOpResult {
        let reply = RemoteFileReply(object: object)
        guard reply.ok else { return RemoteFileOpResult(ok: false, error: reply.failureCode) }
        return RemoteFileOpResult(ok: true, error: nil)
    }

    static func failure(_ code: String) -> RemoteFileOpResult {
        RemoteFileOpResult(ok: false, error: code)
    }
}

/// `delete` answers `ok:true` even when a single path failed, so the batch only
/// counts as clean when every `results[]` row is `ok`.
struct RemoteFileDeleteResult {
    var removed = 0
    var failed = 0
    var firstError: String?
    var error: String?

    static func failure(_ code: String) -> RemoteFileDeleteResult {
        RemoteFileDeleteResult(removed: 0, failed: 0, firstError: nil, error: code)
    }

    static func parse(_ object: [String: Any]) -> RemoteFileDeleteResult {
        let reply = RemoteFileReply(object: object)
        guard reply.ok else { return .failure(reply.failureCode) }
        var firstError: String?
        var removed = 0
        var failed = 0
        for item in (object["results"] as? [[String: Any]]) ?? [] {
            if TransferJSON.bool(item["ok"]) ?? false {
                removed += 1
                continue
            }
            failed += 1
            if firstError == nil {
                let code = TransferJSON.string(item["error"])
                firstError = code.isEmpty ? "unknown" : code
            }
        }
        // The host's own counter wins when it is larger: a row could be missing
        // from a truncated results array.
        let reported = TransferJSON.int(object["removed"], in: 0...Int(UInt32.max)) ?? 0
        return RemoteFileDeleteResult(removed: max(removed, reported),
                                      failed: failed,
                                      firstError: firstError,
                                      error: nil)
    }
}

struct RemoteFileTempClearResult {
    var directory = ""
    var root = ""
    var removed = 0
    var failed = 0
    var error: String?

    static func failure(_ code: String) -> RemoteFileTempClearResult {
        RemoteFileTempClearResult(directory: "", root: "", removed: 0, failed: 0, error: code)
    }

    static func parse(_ object: [String: Any]) -> RemoteFileTempClearResult {
        let reply = RemoteFileReply(object: object)
        guard reply.ok else { return .failure(reply.failureCode) }
        let removedPaths = (object["removed"] as? [Any]) ?? []
        let reported = TransferJSON.int(object["removed_count"], in: 0...Int(UInt32.max)) ?? 0
        return RemoteFileTempClearResult(
            directory: TransferJSON.string(object["directory"]),
            root: TransferJSON.string(object["root"]),
            removed: max(removedPaths.count, reported),
            failed: TransferJSON.int(object["failed"], in: 0...Int(UInt32.max)) ?? 0,
            error: nil
        )
    }
}

// MARK: - Error text

/// One entry per code in `file_browser_ops.h` (`kErrorCodes`, 20 of them). The
/// wording is the desktop catalog's (`file_browser_window.cpp` `kErrorTexts`),
/// so the phone says exactly what the desktop window says for the same code.
enum RemoteFileErrorText {
    static func text(_ code: String) -> String {
        switch code {
        case "invalid_path":
            return NSLocalizedString("路径无效，请重新选择项目。", comment: "")
        case "invalid_request":
            return NSLocalizedString("请求无效，请更新两端版本后重试。", comment: "")
        case "symbolic_link":
            return NSLocalizedString("所选内容包含符号链接，请选择实际文件或目录。", comment: "")
        case "directory_unavailable":
            return NSLocalizedString("无法打开目录，请检查路径和访问权限后刷新。", comment: "")
        case "directory_not_writable":
            return NSLocalizedString("目标目录不可写，请选择其他目录。", comment: "")
        case "file_unreadable":
            return NSLocalizedString("文件无法读取，可能已移动或无访问权限。", comment: "")
        case "unsupported":
            return NSLocalizedString("远端版本不支持此操作，请更新后重试。", comment: "")
        case "unsupported_file":
            return NSLocalizedString("不支持此文件类型。", comment: "")
        case "too_many_files":
            return NSLocalizedString("单次最多操作 4096 个文件与目录，请分批选择。", comment: "")
        case "invalid_listing":
            return NSLocalizedString("目录列表未能完整加载，请刷新重试。", comment: "")
        case "pagination_unsupported":
            return NSLocalizedString("远端版本无法完整读取此目录，请更新远端后重试。", comment: "")
        case "listing_expired":
            return NSLocalizedString("目录列表已过期，请刷新后重试。", comment: "")
        case "busy":
            return NSLocalizedString("请求较多，请稍后重试。", comment: "")
        case "timeout":
            return NSLocalizedString("远端未响应，请确认两端已更新至支持文件浏览的版本后重试。", comment: "")
        case "disconnected":
            return NSLocalizedString("连接已断开，请重新连接后打开文件管理器。", comment: "")
        case "already_finished":
            return NSLocalizedString("任务已结束，无需取消。", comment: "")
        case "decode_failed":
            return NSLocalizedString("文件内容无法解码。", comment: "")
        case "exists":
            return NSLocalizedString("已存在同名项目，请换一个名称。", comment: "")
        case "not_found":
            return NSLocalizedString("项目不存在，可能已被移动或删除。", comment: "")
        case "not_writable":
            return NSLocalizedString("没有写入权限，操作被拒绝。", comment: "")
        default:
            return String(format: NSLocalizedString("文件操作出错：%@", comment: ""), code)
        }
    }
}

// MARK: - Name validation

/// The verdict of a local name check. `.unchanged` and `.rejected` both send no
/// request; only `.valid` reaches the wire.
enum RemoteFileNameCheck: Equatable {
    case valid(String)
    case unchanged
    case rejected(String)

    var name: String? {
        if case let .valid(name) = self { return name }
        return nil
    }

    /// The hint a dialog shows while typing; nil when the name would be sent.
    var hint: String? {
        switch self {
        case .valid: return nil
        case .unchanged: return NSLocalizedString("名称没有变化", comment: "")
        case let .rejected(message): return message
        }
    }
}

/// Local mirror of `file_browser_ops::ValidEntryName` plus the two checks only a
/// client can make (an unchanged name, a name already in the listing). Rejecting
/// locally is not a substitute for the host's answer — every accepted name still
/// travels and its refusal is shown.
enum RemoteFileNaming {
    /// `ValidEntryName` refuses anything longer than this in bytes, not
    /// characters, because the host filesystem limit is bytes.
    static let maximumBytes = 255
    private static let reservedStems: Set<String> = ["CON", "PRN", "AUX", "NUL", "CLOCK$"]
    private static let forbiddenCharacters = CharacterSet(charactersIn: ":*?\"<>|")

    static func check(_ proposed: String,
                      original: String?,
                      existing: [String]) -> RemoteFileNameCheck {
        // Empty or whitespace only.
        if proposed.isEmpty
            || proposed.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return .rejected(NSLocalizedString("名称不能为空", comment: ""))
        }
        // A separator would name a different directory than the one shown.
        if proposed.contains("/") || proposed.contains("\\") {
            return .rejected(NSLocalizedString("名称不能包含路径分隔符", comment: ""))
        }
        if proposed == "." || proposed == ".." {
            return .rejected(NSLocalizedString("名称包含不允许的字符", comment: ""))
        }
        // Control characters, Windows-illegal characters, and the trailing dot or
        // space the host filesystem would silently strip.
        for scalar in proposed.unicodeScalars {
            if scalar.value < 0x20 || scalar.value == 0x7F {
                return .rejected(NSLocalizedString("名称包含不允许的字符", comment: ""))
            }
        }
        if proposed.unicodeScalars.contains(where: { forbiddenCharacters.contains($0) }) {
            return .rejected(NSLocalizedString("名称包含不允许的字符", comment: ""))
        }
        if proposed.hasPrefix(" ") || proposed.hasSuffix(" ") || proposed.hasSuffix(".") {
            return .rejected(NSLocalizedString("名称包含不允许的字符", comment: ""))
        }
        if isReservedDeviceName(proposed) {
            return .rejected(NSLocalizedString("该名称是远端系统的保留名称", comment: ""))
        }
        // The host counts UTF-8 bytes, so a Chinese name runs out at 85
        // characters, not 255.
        if proposed.utf8.count > maximumBytes {
            return .rejected(String(format: NSLocalizedString("名称过长（最多 %1$@ 字节）", comment: ""),
                                    String(describing: maximumBytes)))
        }
        if let original, proposed == original {
            return .unchanged
        }
        // Exact comparison: a POSIX host keeps "A" next to "a", and the host's own
        // `exists` answer covers a case-insensitive filesystem.
        if existing.contains(proposed) {
            return .rejected(RemoteFileErrorText.text("exists"))
        }
        return .valid(proposed)
    }

    private static func isReservedDeviceName(_ name: String) -> Bool {
        let stem = (name.split(separator: ".", maxSplits: 1,
                              omittingEmptySubsequences: false).first.map(String.init) ?? name)
            .uppercased()
        if reservedStems.contains(stem) { return true }
        guard stem.count == 4 else { return false }
        let prefix = stem.prefix(3)
        guard prefix == "COM" || prefix == "LPT" else { return false }
        guard let digit = stem.last else { return false }
        return digit >= "1" && digit <= "9"
    }
}

// MARK: - Store

/// Request/parse/refresh for the remote file browser.
///
/// State machine: `idle -> requesting -> idle`, one request in flight at a time,
/// exactly like `TransferPanelStore`. `generation` is bumped on activate,
/// deactivate and session end, so a reply that arrives after the sheet closed is
/// dropped instead of mutating a dismissed view.
///
/// `unsupportedOps` is deliberately *not* cleared by `activate()`: the host
/// answered `unsupported` for that op once, and re-probing on every sheet would
/// keep offering a button that cannot work. It is cleared by `endSession()` when
/// the connection drops.
final class RemoteFileBrowserStore: ObservableObject {
    @Published private(set) var path = ""
    @Published private(set) var parent = ""
    @Published private(set) var entries: [RemoteFileEntry] = []
    @Published private(set) var loaded = false
    @Published private(set) var requesting = false
    @Published private(set) var truncated = false
    @Published private(set) var errorText = ""
    @Published private(set) var status = ""
    /// Wire names of ops the host refused with `unsupported` (this session).
    @Published private(set) var unsupportedOps: Set<String> = []

    /// Wired by `RemoteSessionModel`; funnels into the bridge's single
    /// `file_browser` entry point (`sendPhotoMessage`).
    var request: ((String, [String: Any], @escaping ([String: Any]) -> Void) -> Void)?

    /// Bumped on activate/deactivate/session end: a reply from a dismissed view
    /// fails this check and is dropped.
    private var generation = 0
    /// Identifies the request that owns the slot, so a timed-out request cannot
    /// clear a newer one.
    private var slotToken = 0
    /// The capability probe runs at most once per session unless it failed for a
    /// reason that says nothing about the ops (mirrors the desktop window).
    private var probedOps = false
    private static let requestTimeout: TimeInterval = 6

    // MARK: Lifecycle

    func activate() {
        generation &+= 1
        slotToken &+= 1
        requesting = false
        errorText = ""
        status = ""
        load(path)
    }

    func deactivate() {
        generation &+= 1
        slotToken &+= 1
        requesting = false
    }

    /// A dropped connection ends the session, so the remembered verdicts go with
    /// it: the next host may be a different build.
    func endSession() {
        generation &+= 1
        slotToken &+= 1
        requesting = false
        probedOps = false
        unsupportedOps = []
        path = ""
        parent = ""
        entries = []
        loaded = false
        truncated = false
        errorText = ""
        status = ""
    }

    // MARK: Reads

    func refresh() {
        load(path)
    }

    /// `path` empty is not a bug: `Directory()` resolves it to the host's home
    /// directory, which is the only sensible place to start.
    func load(_ target: String) {
        guard let request else { return }
        guard !requesting else { return }
        begin()
        let generation = self.generation
        request(RemoteFileOp.list.rawValue, ["path": target]) { [weak self] object in
            guard let self, self.generation == generation else { return }
            self.finish()
            let result = RemoteFileListResult.parse(object)
            if let code = result.error {
                if result.unsupported {
                    // `list` predates this patch, so `unsupported` here means the
                    // host has no file browser at all: remember the block and
                    // stop offering the actions.
                    self.rememberBlockUnsupported()
                }
                self.errorText = RemoteFileErrorText.text(code)
                return
            }
            self.loaded = true
            self.errorText = ""
            self.status = ""
            self.path = result.path
            self.parent = result.parent
            self.entries = result.entries
            self.truncated = result.truncated
            if result.truncated {
                self.status = String(format: NSLocalizedString("目录较大，仅显示前 %1$@ 项", comment: ""),
                                     String(describing: result.entries.count))
            }
            self.probeIfNeeded()
        }
    }

    func enter(_ entry: RemoteFileEntry) {
        guard entry.directory else { return }
        load(entry.path)
    }

    var canGoUp: Bool { loaded && !requesting && !path.isEmpty }

    func goUp() {
        guard canGoUp else { return }
        // A drive root reports itself as its own parent, so "up" from there goes
        // back to the host home instead of looping.
        let target = parent.isEmpty || parent == path ? "" : parent
        load(target)
    }

    /// The names in the current listing, for the duplicate check.
    var currentNames: [String] { entries.map { $0.name } }

    func isUnsupported(_ op: RemoteFileOp) -> Bool {
        unsupportedOps.contains(op.rawValue)
    }

    var canRefresh: Bool { !requesting }

    var statusLine: String? {
        if !errorText.isEmpty { return errorText }
        if !status.isEmpty { return status }
        return nil
    }

    /// A local check that refused, or any message this store has to show without
    /// a host answer.
    func reportProblem(_ text: String) {
        status = ""
        errorText = text
    }

    // MARK: Writes

    func open(_ entry: RemoteFileEntry) {
        run(.open, fields: ["path": entry.path],
            success: NSLocalizedString("已请求远端打开", comment: ""))
    }

    func reveal(_ entry: RemoteFileEntry) {
        run(.reveal, fields: ["path": entry.path],
            success: NSLocalizedString("已请求远端显示所在位置", comment: ""))
    }

    func rename(_ entry: RemoteFileEntry, to name: String) {
        run(.rename, fields: ["path": entry.path, "name": name],
            success: NSLocalizedString("已完成重命名", comment: ""), reload: true)
    }

    func makeDirectory(named name: String) {
        run(.mkdir, fields: ["path": path, "name": name],
            success: NSLocalizedString("已创建文件夹", comment: ""), reload: true)
    }

    func deleteEntry(_ entry: RemoteFileEntry) {
        guard let request else { return }
        guard !requesting else { return }
        guard !isUnsupported(.remove) else {
            reportProblem(RemoteFileErrorText.text("unsupported"))
            return
        }
        begin()
        let generation = self.generation
        request(RemoteFileOp.remove.rawValue, ["paths": [entry.path]]) { [weak self] object in
            guard let self, self.generation == generation else { return }
            self.finish()
            let result = RemoteFileDeleteResult.parse(object)
            if let code = result.error {
                self.note(code, on: .remove)
                return
            }
            if let code = result.firstError {
                self.note(code, on: .remove)
            } else if result.failed > 0 {
                self.reportProblem(RemoteFileErrorText.text("not_writable"))
            } else {
                self.errorText = ""
                self.status = NSLocalizedString("已删除所选项目", comment: "")
            }
            self.reloadAfterWrite()
        }
    }

    /// 属性: one `stat` for the row, straight into the dialog.
    func loadProperties(_ entry: RemoteFileEntry,
                        completion: @escaping (RemoteFileStatResult) -> Void) {
        guard let request else {
            completion(.failure("disconnected"))
            return
        }
        guard !requesting else {
            completion(.failure("busy"))
            return
        }
        guard !isUnsupported(.stat) else {
            reportProblem(RemoteFileErrorText.text("unsupported"))
            completion(.failure("unsupported"))
            return
        }
        begin()
        let generation = self.generation
        request(RemoteFileOp.stat.rawValue, ["paths": [entry.path]]) { [weak self] object in
            guard let self else {
                completion(.failure("disconnected"))
                return
            }
            guard self.generation == generation else {
                completion(.failure("timeout"))
                return
            }
            self.finish()
            let result = RemoteFileStatResult.parse(object)
            if let code = result.error {
                self.note(code, on: .stat)
            }
            completion(result)
        }
    }

    /// 清理临时文件: the host only sweeps inside its receive directory, so a
    /// directory from anywhere else comes back `invalid_path` and the mapped text
    /// says so.
    func clearTemporary() {
        guard let request else { return }
        guard !requesting else { return }
        guard !isUnsupported(.tempClear) else {
            reportProblem(RemoteFileErrorText.text("unsupported"))
            return
        }
        guard !path.isEmpty else {
            reportProblem(RemoteFileErrorText.text("invalid_path"))
            return
        }
        begin()
        let generation = self.generation
        request(RemoteFileOp.tempClear.rawValue, ["directory": path]) { [weak self] object in
            guard let self, self.generation == generation else { return }
            self.finish()
            let result = RemoteFileTempClearResult.parse(object)
            if let code = result.error {
                self.note(code, on: .tempClear)
                return
            }
            self.errorText = ""
            self.status = RemoteFileStoreText.tempClearSummary(removed: result.removed,
                                                               failed: result.failed)
            if result.removed > 0 { self.reloadAfterWrite() }
        }
    }

    // MARK: Plumbing

    private func run(_ op: RemoteFileOp,
                     fields: [String: Any],
                     success: String,
                     reload: Bool = false) {
        guard let request else { return }
        guard !requesting else { return }
        guard !isUnsupported(op) else {
            reportProblem(RemoteFileErrorText.text("unsupported"))
            return
        }
        begin()
        let generation = self.generation
        request(op.rawValue, fields) { [weak self] object in
            guard let self, self.generation == generation else { return }
            self.finish()
            let result = RemoteFileOpResult.parse(object)
            if let code = result.error {
                self.note(code, on: op)
                return
            }
            self.errorText = ""
            self.status = success
            if reload { self.reloadAfterWrite() }
        }
    }

    /// Remembers the op verdict and shows the mapped text. `unsupported` is the
    /// only code that narrows the menu; the others are only shown.
    private func note(_ code: String, on op: RemoteFileOp) {
        if code == "unsupported" { unsupportedOps.insert(op.rawValue) }
        reportProblem(RemoteFileErrorText.text(code))
    }

    private func rememberBlockUnsupported() {
        for op in RemoteFileOp.management { unsupportedOps.insert(op.rawValue) }
        probedOps = true
    }

    /// One `stat` on the directory just listed is the whole probe: the management
    /// ops ship as one block, so a peer that answers it can honour all of them.
    /// Each op is still recorded separately, so a single `unsupported` later
    /// hides only that one.
    private func probeIfNeeded() {
        guard !probedOps, !requesting, !path.isEmpty, let request else { return }
        guard unsupportedOps.count < RemoteFileOp.management.count else { return }
        probedOps = true
        begin()
        let generation = self.generation
        request(RemoteFileOp.stat.rawValue, ["paths": [path]]) { [weak self] object in
            guard let self, self.generation == generation else { return }
            self.finish()
            let reply = RemoteFileReply(object: object)
            if reply.ok { return }
            if reply.unsupported {
                self.rememberBlockUnsupported()
                return
            }
            // A timeout, a stale path or a closed channel says nothing about the
            // ops, so the next listing asks again instead of locking the menu for
            // the whole session. Nothing is reported: a probe is not a user action.
            self.probedOps = false
        }
    }

    private func reloadAfterWrite() {
        guard !requesting else { return }
        load(path)
    }

    private func begin() {
        requesting = true
        slotToken &+= 1
        let token = slotToken
        let generation = self.generation
        // A completion always arrives (the model times every request out), but
        // the slot must free itself even if it does not.
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.requestTimeout) { [weak self] in
            guard let self, self.generation == generation,
                  self.slotToken == token, self.requesting else { return }
            self.requesting = false
            self.errorText = RemoteFileErrorText.text("timeout")
        }
    }

    private func finish() {
        requesting = false
        slotToken &+= 1
    }
}

/// The three summary lines the desktop window prints for a sweep, reused so the
/// phone reports partial failure with both numbers instead of claiming success.
enum RemoteFileStoreText {
    static func tempClearSummary(removed: Int, failed: Int) -> String {
        if removed > 0 && failed > 0 {
            return String(format: NSLocalizedString("已清理 %1$@ 项，%2$@ 项未能删除", comment: ""),
                          String(describing: removed), String(describing: failed))
        }
        if removed > 0 {
            return String(format: NSLocalizedString("已清理 %1$@ 项", comment: ""),
                          String(describing: removed))
        }
        if failed > 0 {
            return NSLocalizedString("临时文件未能全部清理，请稍后重试", comment: "")
        }
        return NSLocalizedString("已清理临时文件", comment: "")
    }
}

// MARK: - View

/// Remote file browser. Presented as a sheet from the floating menu; a long press
/// on a row opens the eight management actions, and rename / 新建文件夹 use an
/// alert with a text field whose live hint is the same validation the host does.
struct RemoteFilesView: View {
    @ObservedObject var store: RemoteFileBrowserStore
    let peerName: String

    @Environment(\.dismiss) private var dismiss

    /// The row whose action sheet is open. `nil` closes it.
    @State private var actionTarget: RemoteFileEntry?
    @State private var renameTarget: RemoteFileEntry?
    @State private var renameText = ""
    @State private var showingRename = false
    @State private var showingNewFolder = false
    @State private var newFolderName = ""
    @State private var deleting: RemoteFileEntry?
    @State private var showingDelete = false
    @State private var properties: RemoteFileStat?
    @State private var showingProperties = false
    @State private var showingTempClear = false
    /// Set when a long press fires: the tap that ends the same gesture must not
    /// also navigate.
    @State private var suppressTapUntil = Date.distantPast

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                header
                Divider()
                fileList
                alertCarriers
            }
            .navigationTitle(NSLocalizedString("文件", comment: ""))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(NSLocalizedString("关闭", comment: "")) { dismiss() }
                }
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        store.goUp()
                    } label: {
                        Image(systemName: "arrow.up")
                    }
                    .disabled(!store.canGoUp)
                    .accessibilityLabel(NSLocalizedString("返回上一级", comment: ""))
                }
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        store.refresh()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .disabled(!store.canRefresh)
                    .accessibilityLabel(NSLocalizedString("刷新", comment: ""))
                }
            }
            .onAppear { store.activate() }
            .onDisappear { store.deactivate() }
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Image(systemName: "folder")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                Text(peerName.isEmpty ? NSLocalizedString("远端设备", comment: "") : peerName)
                    .font(.headline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer(minLength: 4)
                if store.requesting {
                    ProgressView()
                        .controlSize(.mini)
                }
            }
            Text(store.path.isEmpty ? NSLocalizedString("正在读取远端目录…", comment: "") : store.path)
                .font(.footnote.monospacedDigit())
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.middle)
            if let line = store.statusLine {
                Text(line)
                    .font(.footnote)
                    .foregroundStyle(store.errorText.isEmpty ? Color.secondary : Color.red)
                    .lineLimit(3)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var fileList: some View {
        List {
            ForEach(store.entries) { entry in
                row(entry)
            }
        }
        .listStyle(.plain)
        .refreshable { store.refresh() }
        .overlay {
            if store.entries.isEmpty {
                if store.loaded {
                    Text(NSLocalizedString("空目录", comment: ""))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } else if store.requesting {
                    ProgressView()
                }
            }
        }
        .confirmationDialog(actionTitle,
                            isPresented: actionVisible,
                            titleVisibility: .visible,
                            presenting: actionTarget) { entry in
            actionButtons(entry)
        } message: { entry in
            Text(entry.path)
        }
    }

    private func row(_ entry: RemoteFileEntry) -> some View {
        HStack(spacing: 10) {
            Image(systemName: entry.kindSymbol)
                .font(.system(size: 17))
                .foregroundStyle(.secondary)
                .frame(width: 22)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.name)
                    .font(.subheadline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(subtitle(entry))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 4)
            if entry.directory {
                Image(systemName: "chevron.right")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { tapped(entry) }
        .onLongPressGesture(minimumDuration: 0.5) { pressed(entry) }
        .accessibilityHint(NSLocalizedString("长按可打开文件操作", comment: ""))
    }

    @ViewBuilder private func actionButtons(_ entry: RemoteFileEntry) -> some View {
        if !store.isUnsupported(.open) {
            Button(RemoteFileOp.open.title) { deferred { store.open(entry) } }
        }
        if !store.isUnsupported(.reveal) {
            Button(RemoteFileOp.reveal.title) { deferred { store.reveal(entry) } }
        }
        if !store.isUnsupported(.rename) {
            Button(RemoteFileOp.rename.title) { deferred { beginRename(entry) } }
        }
        if !store.isUnsupported(.remove) {
            Button(RemoteFileOp.remove.title, role: .destructive) {
                deferred { beginDelete(entry) }
            }
        }
        if !store.isUnsupported(.mkdir) {
            Button(RemoteFileOp.mkdir.title) { deferred { beginNewFolder() } }
        }
        Button(RemoteFileOp.list.title) { deferred { store.refresh() } }
        if !store.isUnsupported(.stat) {
            Button(RemoteFileOp.stat.title) { deferred { beginProperties(entry) } }
        }
        if !store.isUnsupported(.tempClear) {
            Button(RemoteFileOp.tempClear.title) { deferred { showingTempClear = true } }
        }
        Button(NSLocalizedString("取消", comment: ""), role: .cancel) {}
    }

    /// Every dialog button routes through here: the confirmation dialog is still
    /// dismissing when its action runs, and a sheet or alert presented in that
    /// same turn is dropped by UIKit.
    private func deferred(_ action: @escaping () -> Void) {
        DispatchQueue.main.async(execute: action)
    }

    // MARK: Presentation state

    private var actionVisible: Binding<Bool> {
        Binding(get: { actionTarget != nil },
                set: { presented in if !presented { actionTarget = nil } })
    }

    private var actionTitle: String {
        actionTarget?.name ?? NSLocalizedString("文件操作", comment: "")
    }

    /// Live validation for the alert's message line, recomputed on every keystroke.
    private var renameCheck: RemoteFileNameCheck {
        RemoteFileNaming.check(renameText,
                               original: renameTarget?.name,
                               existing: store.currentNames)
    }

    private var newFolderCheck: RemoteFileNameCheck {
        RemoteFileNaming.check(newFolderName, original: nil, existing: store.currentNames)
    }

    private var alertCarriers: some View {
        HStack(spacing: 0) {
            Color.clear
                .frame(width: 0, height: 0)
                .alert(NSLocalizedString("重命名", comment: ""), isPresented: $showingRename) {
                    TextField(NSLocalizedString("名称", comment: ""), text: $renameText)
                    Button(NSLocalizedString("取消", comment: ""), role: .cancel) {
                        renameTarget = nil
                    }
                    Button(NSLocalizedString("确定", comment: "")) { commitRename() }
                } message: {
                    Text(renameCheck.hint ?? " ")
                }
            Color.clear
                .frame(width: 0, height: 0)
                .alert(NSLocalizedString("新建文件夹", comment: ""), isPresented: $showingNewFolder) {
                    TextField(NSLocalizedString("名称", comment: ""), text: $newFolderName)
                    Button(NSLocalizedString("取消", comment: ""), role: .cancel) {}
                    Button(NSLocalizedString("确定", comment: "")) { commitNewFolder() }
                } message: {
                    Text(newFolderCheck.hint ?? store.path)
                }
            Color.clear
                .frame(width: 0, height: 0)
                .alert(deleteTitle(), isPresented: $showingDelete, presenting: deleting) { _ in
                    Button(RemoteFileOp.remove.title, role: .destructive) { commitDelete() }
                    Button(NSLocalizedString("取消", comment: ""), role: .cancel) { deleting = nil }
                } message: { entry in
                    Text(deleteMessage(entry))
                }
            Color.clear
                .frame(width: 0, height: 0)
                .alert(RemoteFileOp.stat.title,
                       isPresented: $showingProperties,
                       presenting: properties) { _ in
                    Button(NSLocalizedString("确定", comment: "")) { properties = nil }
                } message: { stat in
                    Text(propertiesText(stat))
                }
            Color.clear
                .frame(width: 0, height: 0)
                .alert(NSLocalizedString("清理临时文件？", comment: ""), isPresented: $showingTempClear) {
                    Button(NSLocalizedString("清理", comment: ""), role: .destructive) {
                        commitTempClear()
                    }
                    Button(NSLocalizedString("取消", comment: ""), role: .cancel) {}
                } message: {
                    Text(NSLocalizedString("只清理当前目录及其子目录里远端传输留下的临时文件夹，其他内容不受影响。", comment: ""))
                }
        }
        .frame(width: 0, height: 0)
    }

    // MARK: Actions

    private func tapped(_ entry: RemoteFileEntry) {
        guard Date() >= suppressTapUntil else { return }
        if entry.directory {
            store.enter(entry)
        } else {
            store.open(entry)
        }
    }

    /// Long press: 0.5 s through `onLongPressGesture`. The stamp makes the tap and
    /// the press mutually exclusive even if the tap recognizer still fires on
    /// release.
    private func pressed(_ entry: RemoteFileEntry) {
        suppressTapUntil = Date().addingTimeInterval(0.6)
        actionTarget = entry
    }

    private func beginRename(_ entry: RemoteFileEntry) {
        renameTarget = entry
        renameText = entry.name
        showingRename = true
    }

    private func commitRename() {
        guard let target = renameTarget else { return }
        let check = RemoteFileNaming.check(renameText,
                                           original: target.name,
                                           existing: store.currentNames)
        renameTarget = nil
        if let name = check.name {
            store.rename(target, to: name)
            return
        }
        // A refusal the user can fix: the reason is on screen while they type,
        // and nothing is sent for an unchanged or invalid name.
        if let hint = check.hint { store.reportProblem(hint) }
    }

    private func beginNewFolder() {
        newFolderName = NSLocalizedString("新建文件夹", comment: "")
        showingNewFolder = true
    }

    private func commitNewFolder() {
        let check = RemoteFileNaming.check(newFolderName,
                                           original: nil,
                                           existing: store.currentNames)
        if let name = check.name {
            store.makeDirectory(named: name)
            return
        }
        if let hint = check.hint { store.reportProblem(hint) }
    }

    private func beginDelete(_ entry: RemoteFileEntry) {
        deleting = entry
        showingDelete = true
    }

    private func commitDelete() {
        guard let target = deleting else { return }
        deleting = nil
        store.deleteEntry(target)
    }

    private func beginProperties(_ entry: RemoteFileEntry) {
        store.loadProperties(entry) { result in
            if let stat = result.stat {
                properties = stat
                showingProperties = true
                return
            }
            if let code = result.error {
                store.reportProblem(RemoteFileErrorText.text(code))
            }
        }
    }

    private func commitTempClear() {
        store.clearTemporary()
    }

    private func deleteTitle() -> String {
        guard let target = deleting else { return RemoteFileOp.remove.title }
        return String(format: NSLocalizedString("删除「%@」？", comment: ""), target.name)
    }

    private func deleteMessage(_ entry: RemoteFileEntry) -> String {
        if entry.directory {
            return NSLocalizedString("文件夹及其中的全部内容都会被删除，此操作无法撤销。", comment: "")
        }
        return NSLocalizedString("该文件将被永久删除，此操作无法撤销。", comment: "")
    }

    private func propertiesText(_ stat: RemoteFileStat) -> String {
        var lines = [
            String(format: NSLocalizedString("名称：%@", comment: ""), stat.name),
            String(format: NSLocalizedString("大小：%@", comment: ""), stat.sizeText),
            String(format: NSLocalizedString("修改时间：%@", comment: ""),
                   stat.modified.isEmpty ? "—" : stat.modified),
            String(format: NSLocalizedString("是否目录：%@", comment: ""),
                   stat.directory ? NSLocalizedString("是", comment: "") : NSLocalizedString("否", comment: ""))
        ]
        if stat.link {
            lines.append(NSLocalizedString("符号链接：是", comment: ""))
        }
        return lines.joined(separator: "\n")
    }

    private func subtitle(_ entry: RemoteFileEntry) -> String {
        let kind = entry.directory ? NSLocalizedString("文件夹", comment: "") : entry.sizeText
        if entry.modified.isEmpty { return kind }
        return kind + " · " + entry.modified
    }
}
