import CoreFoundation
import Foundation
import SwiftUI

// Controller-side transfer panel — the iOS half of layer L2.
//
// Transport is the *same* `file_browser` JSON RPC the photo album uses
// (`CrossDeskRTCBridge.sendPhotoMessage`), so the bridge keeps one request path
// and one reply path (`rtcBridge(_:didReceivePhotoMessage:)`). Ops:
//
//   {op:"transfer_list"}                          -> rows + summary
//   {op:"transfer_control", action:<verb>, file_id:<row id>}
//   {op:"transfer_settings"}                      -> rate_kbps/overwrite_policy/concurrency
//   {op:"transfer_settings", rate_kbps|overwrite_policy|concurrency}
//
// The per-row key is `file_id`; the envelope `id` is the RPC sequence number and
// is owned by `RemoteSessionModel`, never by a row.

// MARK: - Wire model

/// Row state vocabulary from `libs/controller/include/file_browser_ops.h`
/// (`kTransferStateNames`). Anything else decodes to `.unknown` instead of
/// dropping the row: the host may grow new states before this build ships.
enum TransferRowState: String {
    case queued
    case sending
    case paused
    case completed
    case failed
    case cancelled
    case unknown

    init(wire: String) {
        self = TransferRowState(rawValue: wire) ?? .unknown
    }

    var label: String {
        switch self {
        case .queued: return NSLocalizedString("等待传输", comment: "")
        case .sending: return NSLocalizedString("传输中", comment: "")
        case .paused: return NSLocalizedString("已暂停", comment: "")
        case .completed: return NSLocalizedString("已完成", comment: "")
        case .failed: return NSLocalizedString("失败", comment: "")
        case .cancelled: return NSLocalizedString("已取消", comment: "")
        case .unknown: return NSLocalizedString("未知状态", comment: "")
        }
    }

    var isFinished: Bool {
        switch self {
        case .completed, .failed, .cancelled: return true
        default: return false
        }
    }
}

/// One `rows[]` entry. Every field is clamped or defaulted in `decode`, so a
/// malformed reply can only drop a row — it can never throw into SwiftUI.
struct TransferRow: Identifiable, Equatable {
    let fileID: Int
    let name: String
    let path: String
    let size: Int64
    let sent: Int64
    /// Bytes per second, as the host reports it.
    let rate: Int64
    let state: TransferRowState
    let paused: Bool
    let retryable: Bool
    let receiving: Bool

    var id: Int { fileID }

    var fraction: Double {
        guard size > 0 else { return state == .completed ? 1 : 0 }
        return min(1, max(0, Double(sent) / Double(size)))
    }

    var percentText: String { "\(Int((fraction * 100).rounded()))%" }

    var sizeText: String {
        guard size > 0 else { return TransferFormat.size(sent) }
        return "\(TransferFormat.size(sent)) / \(TransferFormat.size(size))"
    }

    var rateText: String { TransferFormat.rate(rate) }

    /// `arrow.right` = 发送, `arrow.left` = 接收.
    var directionSymbol: String { receiving ? "arrow.left" : "arrow.right" }

    var directionLabel: String {
        receiving ? NSLocalizedString("接收", comment: "") : NSLocalizedString("发送", comment: "")
    }

    var kindSymbol: String {
        TransferFileKind.symbol(forName: name.isEmpty ? path : name)
    }

    var canPause: Bool {
        (state == .queued || state == .sending) && !paused && state != .paused
    }

    var canResume: Bool { paused || state == .paused }

    var canCancel: Bool { state == .queued || state == .sending || canResume }

    static func decode(_ item: [String: Any]) -> TransferRow? {
        // The host's row id is the only key every per-row action needs, so a row
        // without a usable `file_id` is useless in this panel.
        guard let fileID = TransferJSON.int(item["id"], in: 1...Int(UInt32.max)) else { return nil }
        let path = TransferJSON.string(item["path"])
        let name = TransferJSON.string(item["name"])
        guard !name.isEmpty || !path.isEmpty else { return nil }
        let size = TransferJSON.int64(item["size"], in: 0...Int64.max) ?? 0
        var sent = TransferJSON.int64(item["sent"], in: 0...Int64.max) ?? 0
        // The engine can report `sent > size` for one poll after a resume; the
        // progress bar must never leave 0...1.
        if size > 0 { sent = min(sent, size) }
        return TransferRow(
            fileID: fileID,
            name: name.isEmpty ? TransferFormat.fileName(forPath: path) : name,
            path: path,
            size: size,
            sent: sent,
            rate: TransferJSON.int64(item["rate"], in: 0...Int64.max) ?? 0,
            state: TransferRowState(wire: TransferJSON.string(item["state"])),
            paused: TransferJSON.bool(item["paused"]) ?? false,
            retryable: TransferJSON.bool(item["retryable"]) ?? false,
            receiving: TransferJSON.bool(item["receiving"]) ?? false
        )
    }
}

/// Batch counters. `summary` is advisory: when the host omits it the panel falls
/// back to counting `rows`, so the header line never reads 0 of 3.
struct TransferSummary: Equatable {
    var total = 0
    var completed = 0
    var failed = 0
    var active = 0
    var queued = 0
    var paused = 0
    var batchPartial = false

    static func decode(_ value: Any?) -> TransferSummary {
        guard let object = value as? [String: Any] else { return TransferSummary() }
        func count(_ key: String) -> Int {
            TransferJSON.int(object[key], in: 0...Int(UInt32.max)) ?? 0
        }
        return TransferSummary(
            total: count("total"),
            completed: count("completed"),
            failed: count("failed"),
            active: count("active"),
            queued: count("queued"),
            paused: count("paused"),
            batchPartial: TransferJSON.bool(object["batch_partial"]) ?? false
        )
    }
}

/// The three global switches the host exposes. Ranges mirror
/// `file_browser_ops.h` (`kTransferRateKbpsMax`, overwrite 0...2, concurrency 1...4).
struct TransferSettings: Equatable {
    static let maxRateKbps = 1_048_576
    /// 不限速 / 1 / 4 / 8 / 16 / 32 MB/s in KiB/s.
    static let rateChoices = [0, 1024, 4096, 8192, 16384, 32768]
    /// 自动重命名 / 覆盖 / 跳过.
    static let policyChoices = [2, 1, 0]

    var rateKbps = 0
    var overwritePolicy = 2
    var concurrency = 2

    static func rateTitle(_ kbps: Int) -> String {
        switch kbps {
        case 0: return NSLocalizedString("不限速", comment: "")
        case 1024: return "1 MB/s"
        case 4096: return "4 MB/s"
        case 8192: return "8 MB/s"
        case 16384: return "16 MB/s"
        case 32768: return "32 MB/s"
        default:
            let bytes = Int64(max(0, kbps)) * 1024
            return TransferFormat.size(bytes) + "/s"
        }
    }

    static func policyTitle(_ policy: Int) -> String {
        switch policy {
        case 0: return NSLocalizedString("跳过", comment: "")
        case 1: return NSLocalizedString("覆盖", comment: "")
        default: return NSLocalizedString("自动重命名", comment: "")
        }
    }

    static func decode(_ value: Any?) -> TransferSettings? {
        guard let object = value as? [String: Any] else { return nil }
        return TransferSettings(
            rateKbps: TransferJSON.int(object["rate_kbps"],
                                       in: 0...TransferSettings.maxRateKbps) ?? 0,
            overwritePolicy: TransferJSON.int(object["overwrite_policy"], in: 0...2) ?? 2,
            concurrency: TransferJSON.int(object["concurrency"], in: 1...4) ?? 2
        )
    }
}

/// A settings write. `nil` fields stay untouched on the host: the op accepts any
/// subset of `rate_kbps` / `overwrite_policy` / `concurrency`.
struct TransferSettingsPatch: Equatable {
    var rateKbps: Int?
    var overwritePolicy: Int?
    var concurrency: Int?

    var isEmpty: Bool {
        rateKbps == nil && overwritePolicy == nil && concurrency == nil
    }
}

/// Closed action vocabulary from `file_browser_ops.h` (`kTransferActions`).
enum TransferControlAction: String {
    case pause
    case resume
    case retry
    case cancel
    case startAll = "start_all"
    case pauseAll = "pause_all"
    case resumeAll = "resume_all"
    case clearAll = "clear_all"
    case removeFinished = "remove_finished"

    /// Per-item verbs need a `file_id`; batch verbs ignore it.
    var isItemAction: Bool {
        switch self {
        case .pause, .resume, .retry, .cancel: return true
        default: return false
        }
    }
}

struct TransferListResult {
    let rows: [TransferRow]
    let summary: TransferSummary
    let error: String?

    static func failure(_ code: String) -> TransferListResult {
        TransferListResult(rows: [], summary: TransferSummary(), error: code)
    }

    var unsupported: Bool { error == "unsupported" }
}

struct TransferControlResult {
    let action: TransferControlAction?
    let applied: Bool
    let error: String?

    static func failure(_ code: String) -> TransferControlResult {
        TransferControlResult(action: nil, applied: false, error: code)
    }

    var unsupported: Bool { error == "unsupported" }
}

struct TransferSettingsResult {
    let settings: TransferSettings?
    let error: String?

    static func failure(_ code: String) -> TransferSettingsResult {
        TransferSettingsResult(settings: nil, error: code)
    }

    var unsupported: Bool { error == "unsupported" }
}

// MARK: - Defensive JSON helpers

enum TransferJSON {
    static func string(_ value: Any?) -> String {
        value as? String ?? ""
    }

    static func bool(_ value: Any?) -> Bool? {
        guard let box = value as? NSNumber else { return nil }
        if CFGetTypeID(box) == CFBooleanGetTypeID() { return box.boolValue }
        return box.doubleValue != 0
    }

    static func int(_ value: Any?, in range: ClosedRange<Int>) -> Int? {
        guard let box = numeric(value) else { return nil }
        let raw = box.doubleValue
        guard raw.isFinite else { return nil }
        return min(max(Int(exactlyRepresentable(raw)), range.lowerBound), range.upperBound)
    }

    static func int64(_ value: Any?, in range: ClosedRange<Int64>) -> Int64? {
        guard let box = numeric(value) else { return nil }
        let raw = box.doubleValue
        guard raw.isFinite else { return nil }
        return min(max(Int64(exactlyRepresentable(raw)), range.lowerBound), range.upperBound)
    }

    /// 2^53 is the largest magnitude a `Double` still holds exactly. Clamping
    /// there keeps the integer initializers from trapping on a hostile or
    /// rounded-up reply (e.g. `Double(Int64.max)` is 2^63).
    private static func exactlyRepresentable(_ raw: Double) -> Double {
        let limit = 9_007_199_254_740_992.0
        return min(max(raw.rounded(), -limit), limit)
    }

    /// JSON booleans bridge to `NSNumber`; a boolean is never a number here.
    private static func numeric(_ value: Any?) -> NSNumber? {
        guard let box = value as? NSNumber, CFGetTypeID(box) != CFBooleanGetTypeID() else {
            return nil
        }
        return box
    }
}

// MARK: - Formatting

enum TransferFormat {
    static func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: max(0, bytes), countStyle: .file)
    }

    static func rate(_ bytesPerSecond: Int64) -> String {
        guard bytesPerSecond > 0 else { return "—" }
        return ByteCountFormatter.string(fromByteCount: bytesPerSecond, countStyle: .file) + "/s"
    }

    static func fileName(forPath path: String) -> String {
        let name = (path as NSString).lastPathComponent
        return name.isEmpty ? path : name
    }
}

/// The 16 file-type families the desktop transfer window paints
/// (`file_browser_window.cpp` `TaskKind`), mapped onto SF Symbols.
enum TransferFileKind {
    static func symbol(forName name: String) -> String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        // Folder / disk / link rows only reach a transfer queue when a host
        // enqueues a directory or a URL; resolve those before extensions.
        if trimmed.hasPrefix("http://") || trimmed.hasPrefix("https://") { return "link" }
        if trimmed.hasSuffix(":\\") || trimmed.hasSuffix(":/") { return "externaldrive" }
        if trimmed.hasSuffix("/") || trimmed.hasSuffix("\\") { return "folder" }
        switch extensionOf(trimmed) {
        case "pdf":
            return "doc.richtext"
        case "doc", "docx", "docm", "dot", "dotx", "rtf", "odt", "pages", "wps":
            return "doc.text"
        case "xls", "xlsx", "xlsm", "csv", "ods", "numbers":
            return "tablecells"
        case "ppt", "pptx", "pps", "ppsx", "odp", "key":
            return "chart.bar.doc.horizontal"
        // The text family shares `doc.text` with the word family: the icon set
        // has one plain-text glyph.
        case "txt", "md", "log", "ini", "cfg", "conf", "json", "xml", "yaml",
             "yml", "toml", "sh", "bat", "ps1", "c", "h", "cc", "cpp", "hpp",
             "py", "js", "ts", "html", "css":
            return "doc.text"
        case "png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico", "tif",
             "tiff", "heic", "psd":
            return "photo"
        case "mp3", "wav", "flac", "aac", "ogg", "m4a", "wma", "ape":
            return "music.note"
        case "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg",
             "mpeg", "rmvb", "3gp":
            return "film"
        case "exe", "msi", "dmg", "pkg", "deb", "rpm", "apk", "ipa", "zip",
             "7z", "rar", "tar", "gz":
            return "arrow.down.circle"
        case "":
            // No extension: the desktop resolves this to the document family.
            return "doc"
        default:
            return "doc.questionmark"
        }
    }

    /// Lowercased suffix after the last dot, empty when the name has no usable
    /// extension (no dot, leading dot, trailing dot, or a separator after it).
    private static func extensionOf(_ name: String) -> String {
        guard let dot = name.lastIndex(of: "."), dot != name.startIndex else { return "" }
        let suffix = name[name.index(after: dot)...]
        guard !suffix.isEmpty, !suffix.contains("/"), !suffix.contains("\\") else { return "" }
        return suffix.lowercased()
    }
}

enum TransferErrorText {
    static func text(_ code: String) -> String {
        switch code {
        case "unsupported":
            return NSLocalizedString("远端不支持传输面板", comment: "")
        case "disconnected":
            return NSLocalizedString("连接已断开，无法操作远端传输", comment: "")
        case "timeout":
            return NSLocalizedString("远端没有响应，请稍后刷新", comment: "")
        case "invalid_request":
            return NSLocalizedString("这次操作没有被远端接受，请刷新后重试", comment: "")
        case "already_finished":
            return NSLocalizedString("这项传输已经结束", comment: "")
        case "busy":
            return NSLocalizedString("远端正忙，请稍后重试", comment: "")
        default:
            return String(format: NSLocalizedString("传输面板出错：%@", comment: ""), code)
        }
    }
}

// MARK: - Store

/// Request/parse/refresh/poll for the transfer panel.
///
/// State machine: `idle -> requesting(list) -> idle` for the 1 s poll, with a
/// single in-flight request at a time; control and settings writes take the same
/// slot, so a pause tap can never race the next poll into two open requests.
/// `unsupported` is terminal for the sheet: polling stops, every control is
/// disabled, and reopening the sheet re-probes once.
final class TransferPanelStore: ObservableObject {
    @Published private(set) var rows: [TransferRow] = []
    @Published private(set) var summary = TransferSummary()
    @Published private(set) var settings = TransferSettings()
    @Published private(set) var unsupported = false
    @Published private(set) var requesting = false
    @Published private(set) var loaded = false
    @Published private(set) var status = ""
    @Published private(set) var errorText = ""
    @Published private(set) var settingsAvailable = false

    /// Wired by `RemoteSessionModel`; all three funnel into the bridge's single
    /// `file_browser` entry point.
    var requestList: ((@escaping (TransferListResult) -> Void) -> Void)?
    var requestControl: ((TransferControlAction, Int, @escaping (TransferControlResult) -> Void) -> Void)?
    var requestSettings: ((TransferSettingsPatch?, @escaping (TransferSettingsResult) -> Void) -> Void)?

    private var timer: Timer?
    /// Bumped on activate/deactivate; a reply from a dismissed panel is dropped.
    private var generation = 0
    private var requestStartedAt: Date?
    private var settingsRequested = false
    private static let pollInterval: TimeInterval = 1
    private static let requestTimeout: TimeInterval = 8

    // MARK: Lifecycle

    func activate() {
        generation &+= 1
        status = ""
        errorText = ""
        // A stale verdict from a previous session must not outlive the sheet: a
        // host that still says unsupported degrades again on the first reply.
        unsupported = false
        settingsRequested = false
        // Drop the slot a previous presentation may have left open; its reply now
        // fails the generation check, and the model times its own entry out.
        requesting = false
        requestStartedAt = nil
        startPolling()
        refresh()
    }

    func deactivate() {
        stopPolling()
        generation &+= 1
        requesting = false
        requestStartedAt = nil
    }

    func suspend() {
        stopPolling()
    }

    func resume() {
        guard !unsupported else { return }
        startPolling()
        refresh()
    }

    private func startPolling() {
        guard timer == nil else { return }
        let timer = Timer(timeInterval: Self.pollInterval, repeats: true) { [weak self] _ in
            self?.tick()
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    private func stopPolling() {
        timer?.invalidate()
        timer = nil
    }

    private func tick() {
        guard !unsupported else {
            stopPolling()
            return
        }
        if requesting {
            // The model times each request out as well; this only guarantees the
            // panel's own slot is released even if a completion never fires.
            if let started = requestStartedAt,
               Date().timeIntervalSince(started) > Self.requestTimeout {
                requesting = false
                requestStartedAt = nil
                errorText = TransferErrorText.text("timeout")
            }
            return
        }
        refresh()
    }

    // MARK: Reads

    func refresh() {
        guard !unsupported, !requesting, let requestList else { return }
        beginRequest()
        let generation = self.generation
        requestList { [weak self] result in
            guard let self, self.generation == generation else { return }
            self.endRequest()
            self.apply(result)
        }
    }

    private func apply(_ result: TransferListResult) {
        if let error = result.error {
            if result.unsupported {
                degrade()
            } else {
                errorText = TransferErrorText.text(error)
            }
            return
        }
        loaded = true
        errorText = ""
        rows = result.rows
        summary = result.summary
        if !settingsRequested {
            settingsRequested = true
            loadSettings()
        }
    }

    private func degrade() {
        unsupported = true
        stopPolling()
        rows = []
        summary = TransferSummary()
        settingsAvailable = false
        errorText = ""
        status = ""
    }

    private func loadSettings() {
        guard !unsupported, !requesting, let requestSettings else { return }
        beginRequest()
        let generation = self.generation
        requestSettings(nil) { [weak self] result in
            guard let self, self.generation == generation else { return }
            self.endRequest()
            if let error = result.error {
                if !result.unsupported { self.errorText = TransferErrorText.text(error) }
                return
            }
            if let settings = result.settings { self.settings = settings }
            self.settingsAvailable = true
        }
    }

    // MARK: Writes

    func startAll() { send(.startAll, fileID: 0) }
    func pauseAll() { send(.pauseAll, fileID: 0) }
    func resumeAll() { send(.resumeAll, fileID: 0) }
    func removeFinished() { send(.removeFinished, fileID: 0) }
    func clearAll() { send(.clearAll, fileID: 0) }

    func perform(_ action: TransferControlAction, fileID: Int) {
        send(action, fileID: fileID)
    }

    func updateRate(_ kbps: Int) {
        sendSettings(TransferSettingsPatch(rateKbps: kbps))
    }

    func updateOverwritePolicy(_ policy: Int) {
        sendSettings(TransferSettingsPatch(overwritePolicy: policy))
    }

    private func send(_ action: TransferControlAction, fileID: Int) {
        guard !unsupported, !requesting, let requestControl else { return }
        if action.isItemAction && fileID <= 0 { return }
        beginRequest()
        let generation = self.generation
        requestControl(action, action.isItemAction ? fileID : 0) { [weak self] result in
            guard let self, self.generation == generation else { return }
            self.endRequest()
            if let error = result.error {
                if result.unsupported {
                    self.degrade()
                } else {
                    self.errorText = TransferErrorText.text(error)
                }
                return
            }
            self.errorText = ""
            self.status = self.feedback(action, applied: result.applied)
            self.refresh()
        }
    }

    private func sendSettings(_ patch: TransferSettingsPatch) {
        guard !unsupported, !requesting, !patch.isEmpty, let requestSettings else { return }
        beginRequest()
        let generation = self.generation
        requestSettings(patch) { [weak self] result in
            guard let self, self.generation == generation else { return }
            self.endRequest()
            if let error = result.error {
                if !result.unsupported { self.errorText = TransferErrorText.text(error) }
                return
            }
            if let settings = result.settings { self.settings = settings }
            self.settingsAvailable = true
            self.errorText = ""
        }
    }

    private func feedback(_ action: TransferControlAction, applied: Bool) -> String {
        guard applied else { return NSLocalizedString("远端没有可执行的操作", comment: "") }
        switch action {
        case .pause: return NSLocalizedString("已暂停", comment: "")
        case .resume: return NSLocalizedString("已继续", comment: "")
        case .retry: return NSLocalizedString("已重试", comment: "")
        case .cancel: return NSLocalizedString("已取消", comment: "")
        case .startAll: return NSLocalizedString("已开始全部传输", comment: "")
        case .pauseAll: return NSLocalizedString("已暂停全部传输", comment: "")
        case .resumeAll: return NSLocalizedString("已继续全部传输", comment: "")
        case .removeFinished: return NSLocalizedString("已清除已完成的任务", comment: "")
        case .clearAll: return NSLocalizedString("已清空传输列表", comment: "")
        }
    }

    // MARK: Derived view state

    private var interactive: Bool { !unsupported && !requesting }

    var canRefresh: Bool { interactive }
    var canControlRows: Bool { interactive }
    var canStartAll: Bool { interactive && rows.contains { $0.state == .queued } }
    var canPauseAll: Bool { interactive && rows.contains { $0.canPause } }
    var canResumeAll: Bool { interactive && rows.contains { $0.canResume } }
    var canRemoveFinished: Bool { interactive && rows.contains { $0.state.isFinished } }
    var canClearAll: Bool { interactive && !rows.isEmpty }

    var activeCount: Int {
        max(summary.active, rows.filter { $0.state == .sending }.count)
    }

    /// 「共 N 项 · 完成 x · 失败 y · 进行中 z」
    var summaryLine: String {
        let total = summary.total > 0 ? summary.total : rows.count
        return "共 \(total) 项 · 完成 \(summary.completed) · 失败 \(summary.failed) · 进行中 \(activeCount)"
    }

    var statusLine: String? {
        if !errorText.isEmpty { return errorText }
        if !status.isEmpty { return status }
        return nil
    }

    var settingsEnabled: Bool { interactive && settingsAvailable }

    private func beginRequest() {
        requesting = true
        requestStartedAt = Date()
    }

    private func endRequest() {
        requesting = false
        requestStartedAt = nil
    }
}

// MARK: - View

struct TransferPanelView: View {
    @ObservedObject var store: TransferPanelStore
    let peerName: String

    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase

    private let accent = Color(red: 0.21, green: 0.39, blue: 0.89)

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                header
                Divider()
                actionBar
                Divider()
                rowList
                Divider()
                settingsFooter
            }
            .navigationTitle(NSLocalizedString("传输", comment: ""))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(NSLocalizedString("关闭", comment: "")) { dismiss() }
                }
            }
            .onAppear { store.activate() }
            .onDisappear { store.deactivate() }
            .onValueChange(of: scenePhase) { phase in
                if phase == .active {
                    store.resume()
                } else {
                    store.suspend()
                }
            }
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Image(systemName: "desktopcomputer")
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
            Text(store.summaryLine)
                .font(.footnote.monospacedDigit())
                .foregroundStyle(.secondary)
            if let line = store.statusLine {
                Text(line)
                    .font(.footnote)
                    .foregroundStyle(store.errorText.isEmpty ? Color.secondary : Color.red)
                    .lineLimit(2)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var actionBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                chip(NSLocalizedString("全部开始", comment: ""),
                     enabled: store.canStartAll) { store.startAll() }
                chip(NSLocalizedString("全部暂停", comment: ""),
                     enabled: store.canPauseAll) { store.pauseAll() }
                chip(NSLocalizedString("全部继续", comment: ""),
                     enabled: store.canResumeAll) { store.resumeAll() }
                chip(NSLocalizedString("清除已完成", comment: ""),
                     enabled: store.canRemoveFinished) { store.removeFinished() }
                chip(NSLocalizedString("全部清除", comment: ""),
                     enabled: store.canClearAll) { store.clearAll() }
                chip(NSLocalizedString("刷新", comment: ""),
                     enabled: store.canRefresh) { store.refresh() }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
        }
    }

    private func chip(_ title: String, enabled: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.caption)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(enabled ? accent.opacity(0.16) : Color.secondary.opacity(0.10))
                .foregroundStyle(enabled ? accent : Color.secondary)
                .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
    }

    private var rowList: some View {
        ScrollView {
            LazyVStack(spacing: 0) {
                ForEach(store.rows) { row in
                    TransferRowView(row: row,
                                    enabled: store.canControlRows,
                                    perform: { action in
                                        store.perform(action, fileID: row.fileID)
                                    })
                    Divider()
                }
            }
        }
        .overlay {
            if store.unsupported {
                Text(NSLocalizedString("远端不支持传输面板", comment: ""))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding()
            } else if store.rows.isEmpty {
                if store.loaded {
                    Text(NSLocalizedString("暂无传输项", comment: ""))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .padding()
                } else {
                    ProgressView()
                        .padding()
                }
            }
        }
    }

    private var settingsFooter: some View {
        VStack(spacing: 6) {
            settingPicker(NSLocalizedString("限速", comment: ""),
                          selection: Binding(get: { store.settings.rateKbps },
                                             set: { store.updateRate($0) })) {
                ForEach(TransferSettings.rateChoices, id: \.self) { value in
                    Text(TransferSettings.rateTitle(value)).tag(value)
                }
            }
            settingPicker(NSLocalizedString("覆盖策略", comment: ""),
                          selection: Binding(get: { store.settings.overwritePolicy },
                                             set: { store.updateOverwritePolicy($0) })) {
                ForEach(TransferSettings.policyChoices, id: \.self) { value in
                    Text(TransferSettings.policyTitle(value)).tag(value)
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }

    private func settingPicker<Content: View>(_ title: String,
                                              selection: Binding<Int>,
                                              @ViewBuilder content: () -> Content) -> some View {
        HStack(spacing: 8) {
            Text(title)
                .font(.footnote)
                .foregroundStyle(.secondary)
            Spacer(minLength: 8)
            Picker(title, selection: selection, content: content)
                .pickerStyle(.menu)
                .labelsHidden()
                .disabled(!store.settingsEnabled)
        }
    }
}

private struct TransferRowView: View {
    let row: TransferRow
    let enabled: Bool
    let perform: (TransferControlAction) -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: row.kindSymbol)
                .font(.system(size: 18))
                .foregroundStyle(.secondary)
                .frame(width: 22)
                .accessibilityHidden(true)

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Image(systemName: row.directionSymbol)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(isPaused ? Color.orange : Color.secondary)
                        .accessibilityLabel(row.directionLabel)
                    if isPaused {
                        Image(systemName: "pause.circle.fill")
                            .font(.system(size: 11))
                            .foregroundStyle(.orange)
                    }
                    Text(row.name)
                        .font(.subheadline)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer(minLength: 4)
                    Text(row.rateText)
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                }

                ProgressView(value: row.fraction)
                    .tint(tint)

                HStack(spacing: 8) {
                    Text(row.sizeText)
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                    Text(row.percentText)
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                    Text(stateLabel)
                        .font(.caption2)
                        .foregroundStyle(row.state == .failed ? Color.red : Color.secondary)
                    Spacer(minLength: 4)
                    actions
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }

    /// `paused` is the host's row flag, `state == .paused` its derived state; the
    /// badge has to cover both so a hold shows up whichever field moved.
    private var isPaused: Bool { row.paused || row.state == .paused }

    private var stateLabel: String {
        isPaused ? NSLocalizedString("已暂停", comment: "") : row.state.label
    }

    private var tint: Color {
        switch row.state {
        case .failed: return .red
        case .completed: return .green
        case .cancelled: return .secondary
        default: return isPaused ? .orange : .accentColor
        }
    }

    @ViewBuilder private var actions: some View {
        HStack(spacing: 6) {
            if row.canResume {
                smallButton(NSLocalizedString("继续", comment: ""), .resume)
            } else if row.canPause {
                smallButton(NSLocalizedString("暂停", comment: ""), .pause)
            }
            if row.retryable {
                smallButton(NSLocalizedString("重试", comment: ""), .retry)
            }
            if row.canCancel {
                smallButton(NSLocalizedString("取消", comment: ""), .cancel)
            }
        }
        .disabled(!enabled)
    }

    private func smallButton(_ title: String, _ action: TransferControlAction) -> some View {
        Button {
            perform(action)
        } label: {
            Text(title)
                .font(.caption2)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Color.secondary.opacity(0.14), in: Capsule())
                .foregroundStyle(.primary)
        }
        .buttonStyle(.plain)
    }
}
