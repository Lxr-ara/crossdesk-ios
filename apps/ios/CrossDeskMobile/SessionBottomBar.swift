import SwiftUI

/// UU远程 式底部控制条 + 会话内面板。
///
/// 几何来自 UU 自己的资源（`icon_tool_*` 24 pt 画布、`icon_tool_hover_*` 40 pt 触摸目标），
/// 由 `BarMetrics.generated.swift` 提供，桌面端读同一组常量。
///
/// 面板按 UU 的两点做法复刻：
/// 1. 工具页是**数据驱动**列表（它的日志里就有「不构建工具列表 / 刷新工具列表」）；
/// 2. 观看模式是**总开关**，开启后点击被忽略并提示，而不是把按钮藏起来。
struct SessionBottomBar: View {
    let session: RemoteSessionModel

    @Binding var keyboardVisible: Bool
    @Binding var shortcutsVisible: Bool

    var showsFiles: () -> Void
    var showsPhotoAlbum: () -> Void
    var showsReceivedFiles: () -> Void
    var showsTransferPanel: () -> Void
    var showsRemoteFiles: () -> Void
    var showsDisplays: () -> Void
    var onReleaseButtons: () -> Void
    /// true = 电脑键盘布局，false = 系统键盘。
    var onChooseKeyboard: (Bool) -> Void

    @AppStorage("watchOnlyMode") private var watchOnly = false
    @AppStorage("videoRotationDegrees") private var rotationDegrees = 0
    @State private var keyboardChooserOpen = false
    @State private var operationsOpen = false
    @State private var toolsOpen = false
    @State private var combineKeysOpen = false
    @State private var toast: String?

    private static let qualityOptions = ["流畅", "高清", "超清"]
    private static let qualitySymbols = ["speedometer", "sparkles", "4k.tv"]

    private var target: CGFloat { BarMetrics.barTarget }
    private var icon: CGFloat { BarMetrics.barIcon }

    var body: some View {
        HStack(spacing: BarMetrics.barGap) {
            item("rectangle.on.rectangle", "所有窗口", active: false) {
                act("所有窗口") { session.sendKeyChord(0x09, modifiers: [0x5B]) }
            }
            item("rectangle.inset.filled", "显示桌面", active: false) {
                act("显示桌面") { session.sendKeyChord(0x44, modifiers: [0x5B]) }
            }
            item("keyboard", "键盘", active: keyboardVisible) {
                keyboardChooserOpen = true
            }
            item("slider.horizontal.3", "操作", active: operationsOpen) {
                toolsOpen = false
                operationsOpen = true
            }
            item("wrench.and.screwdriver", "工具", active: toolsOpen) {
                operationsOpen = false
                toolsOpen = true
            }
        }
        .padding(.horizontal, BarMetrics.barInset)
        .frame(height: BarMetrics.barHeight)
        .background(.ultraThinMaterial,
                    in: RoundedRectangle(cornerRadius: BarMetrics.barCorner, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: BarMetrics.barCorner, style: .continuous)
                .strokeBorder(.white.opacity(0.22), lineWidth: 1)
        )
        .shadow(color: .black.opacity(0.22), radius: 12, y: 6)
        .overlay(alignment: .bottom) { toastView }
        .sheet(isPresented: $keyboardChooserOpen) { keyboardChooser }
        .sheet(isPresented: $operationsOpen) { operationsSheet }
        .sheet(isPresented: $toolsOpen) { toolsSheet }
        .sheet(isPresented: $combineKeysOpen) {
            CombineKeysView(
                session: session,
                onSend: { key, modifiers in session.sendKeyChord(key, modifiers: modifiers) },
                onDismiss: { combineKeysOpen = false }
            )
        }
    }

    // MARK: 观看模式：拦截 + 提示

    private func act(_ name: String, _ body: () -> Void) {
        if watchOnly {
            show("观看模式下已忽略「\(name)」")
            return
        }
        body()
    }

    private func show(_ text: String) {
        toast = text
        Task {
            try? await Task.sleep(nanoseconds: 1_600_000_000)
            await MainActor.run { if toast == text { toast = nil } }
        }
    }

    @ViewBuilder
    private var toastView: some View {
        if let toast {
            Text(toast)
                .font(.footnote)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(.black.opacity(0.72), in: Capsule())
                .foregroundStyle(.white)
                .padding(.bottom, BarMetrics.barHeight + 8)
        }
    }

    private func item(_ symbol: String, _ title: String, active: Bool,
                      action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 2) {
                Image(systemName: symbol)
                    .font(.system(size: icon * 0.8))
                    .frame(height: icon)
                Text(title)
                    .font(.system(size: 10, weight: .medium))
            }
            .foregroundStyle(active ? Color.accentColor : Color.primary.opacity(0.78))
            .frame(width: target, height: target)
            .background(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(active ? Color.accentColor.opacity(0.16) : .clear)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(title))
    }

    // MARK: 键盘选择：系统键盘 / 电脑键盘 / 快捷键 / 组合键

    private var keyboardChooser: some View {
        NavigationStack {
            List {
                Section {
                    Button {
                        keyboardChooserOpen = false
                        onChooseKeyboard(false)
                    } label: {
                        Label("系统键盘（直接打字）", systemImage: "keyboard")
                    }
                    Button {
                        keyboardChooserOpen = false
                        onChooseKeyboard(true)
                    } label: {
                        Label("电脑键盘（全尺寸布局）", systemImage: "keyboard.badge.ellipsis")
                    }
                    Button {
                        keyboardChooserOpen = false
                        shortcutsVisible = true
                    } label: {
                        Label("快捷键面板", systemImage: "command")
                    }
                    Button {
                        keyboardChooserOpen = false
                        combineKeysOpen = true
                    } label: {
                        Label("组合键（多键同时按下）", systemImage: "option")
                    }
                } footer: {
                    Text("系统键盘与电脑键盘共用同一输入通道；组合键用于 Ctrl+Alt+Del 这类桌面操作。")
                }
            }
            .navigationTitle("键盘")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("完成") { keyboardChooserOpen = false }
                }
            }
        }
        .presentationDetents([.height(320), .medium])
    }

    // MARK: 操作

    private var operationsSheet: some View {
        NavigationStack {
            List {
                Section("观看模式") {
                    Toggle(isOn: $watchOnly) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("只看不动")
                            Text("开启后底栏点击会被忽略并提示")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                Section("画面") {
                    // 旋转屏幕：UU 的 Control.Panel.screenRotation。客户端旋转显示，
                    // 触摸层同角度反解，所以点击仍然落在看到的位置。
                    ForEach([0, 90, 180, 270], id: \.self) { degrees in
                        Button {
                            act("旋转屏幕") { rotationDegrees = degrees }
                        } label: {
                            HStack {
                                Label("\(degrees)°", systemImage: degrees == 0
                                      ? "rectangle" : "rotate.right")
                                Spacer()
                                if rotationDegrees == degrees {
                                    Image(systemName: "checkmark")
                                }
                            }
                        }
                    }

                    ForEach(0..<Self.qualityOptions.count, id: \.self) { index in
                        Button {
                            act("画质") { session.updateVideoSettings(quality: index) }
                        } label: {
                            HStack {
                                Label(Self.qualityOptions[index],
                                      systemImage: Self.qualitySymbols[index])
                                Spacer()
                                if session.videoSettings.selection.quality == index {
                                    Image(systemName: "checkmark")
                                }
                            }
                        }
                    }
                    Button {
                        operationsOpen = false
                        act("切换显示器") { showsDisplays() }
                    } label: {
                        Label("切换显示器", systemImage: "display.2")
                    }
                }
                Section("声音与隐私") {
                    Button {
                        act("声音") { session.toggleAudio() }
                    } label: {
                        Label(session.audioEnabled ? "关闭远端声音" : "开启远端声音",
                              systemImage: session.audioEnabled ? "speaker.slash" : "speaker.wave.2")
                    }
                    Button {
                        act("隐私屏") { session.togglePrivacyScreen() }
                    } label: {
                        Label("隐私屏", systemImage: "eye.slash")
                    }
                }
            }
            .navigationTitle("操作")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("完成") { operationsOpen = false }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    // MARK: 工具（数据驱动）

    private struct ToolRow: Identifiable {
        let id = UUID()
        let title: String
        let symbol: String
        let note: String?
        let action: () -> Void
    }

    private var toolRows: [ToolRow] {
        [
            ToolRow(title: "传输面板", symbol: "arrow.up.arrow.down.circle", note: nil) {
                showsTransferPanel()
            },
            ToolRow(title: "发送文件", symbol: "doc.badge.plus", note: nil) {
                showsFiles()
            },
            ToolRow(title: "接收的文件", symbol: "tray.and.arrow.down", note: nil) {
                showsReceivedFiles()
            },
            ToolRow(title: "远端文件", symbol: "externaldrive.connected.to.line.below", note: nil) {
                showsRemoteFiles()
            },
            ToolRow(title: "相册", symbol: "photo.on.rectangle.angled", note: nil) {
                showsPhotoAlbum()
            },
            ToolRow(title: "发送剪贴板", symbol: "doc.on.clipboard",
                    note: session.clipboardStatus) { session.sendClipboard() },
            ToolRow(title: "组合键", symbol: "option", note: nil) { combineKeysOpen = true },
            ToolRow(title: "释放所有按键", symbol: "hand.raised", note: nil) { onReleaseButtons() },
            ToolRow(title: "Ctrl+Alt+Del", symbol: "lock.shield", note: nil) {
                session.sendSecureAttention()
            },
        ]
    }

    private var toolsSheet: some View {
        NavigationStack {
            List {
                Section("工具") {
                    ForEach(toolRows) { row in
                        Button {
                            toolsOpen = false
                            act(row.title) { row.action() }
                        } label: {
                            HStack {
                                Label(row.title, systemImage: row.symbol)
                                Spacer()
                                if let note = row.note, !note.isEmpty {
                                    Text(note)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle("工具")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("完成") { toolsOpen = false }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}
