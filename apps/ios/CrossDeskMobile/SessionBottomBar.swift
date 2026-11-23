import SwiftUI

/// UU远程 式底部控制条。
///
/// `BarMetrics` 由 `libs/wire/include/remote_cursor_metrics.h` 生成，数值来自 UU 自己的资源：
/// `icon_tool_*` 是 24 pt 画布，`icon_tool_hover_*` 是 40 pt 触摸目标——所以图标 24、
/// 目标 40、条高 56、间距 8、圆角 28。桌面端读同一组常量，两端不会各画各的。
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

    @State private var operationsOpen = false
    @State private var toolsOpen = false

    private var target: CGFloat { BarMetrics.barTarget }
    private var icon: CGFloat { BarMetrics.barIcon }

    var body: some View {
        HStack(spacing: BarMetrics.barGap) {
            item("rectangle.on.rectangle", "所有窗口", active: false) {
                // 桌面端的「任务视图」：Win+Tab（macOS 上是 Control+↑，靠主机端映射）
                session.sendKeyChord(0x09, modifiers: [0x5B])
            }
            item("rectangle.inset.filled", "显示桌面", active: false) {
                session.sendKeyChord(0x44, modifiers: [0x5B])
            }
            item("keyboard", "键盘", active: keyboardVisible) {
                keyboardVisible.toggle()
                if keyboardVisible { shortcutsVisible = false }
            }
            item("slider.horizontal.3", "操作", active: operationsOpen) {
                toolsOpen = false
                operationsOpen.toggle()
            }
            item("wrench.and.screwdriver", "工具", active: toolsOpen) {
                operationsOpen = false
                toolsOpen.toggle()
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
        .sheet(isPresented: $operationsOpen) { operationsSheet }
        .sheet(isPresented: $toolsOpen) { toolsSheet }
    }

    private func item(_ symbol: String, _ title: String, active: Bool,
                      action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 2) {
                Image(systemName: symbol)
                    .font(.system(size: icon * 0.8, weight: .regular))
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

    // MARK: 操作

    private var operationsSheet: some View {
        NavigationStack {
            List {
                Section("画面") {
                    Button {
                        // 旋转屏幕：主机端还没有这条 op，先如实标出来。
                        operationsOpen = false
                    } label: {
                        Label("旋转屏幕（主机暂未支持）", systemImage: "rotate.right")
                            .foregroundStyle(.secondary)
                    }
                    .disabled(true)

                    // 主机只认 0/1/2 三档（流畅 / 高清 / 超清），名字与桌面端一致。
                    ForEach(0..<Self.qualityOptions.count, id: \.self) { index in
                        Button {
                            session.updateVideoSettings(quality: index)
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
                        showsDisplays()
                    } label: {
                        Label("切换显示器", systemImage: "display.2")
                    }
                }
                Section("声音与隐私") {
                    Button {
                        session.toggleAudio()
                    } label: {
                        Label(session.audioEnabled ? "关闭远端声音" : "开启远端声音",
                              systemImage: session.audioEnabled ? "speaker.slash" : "speaker.wave.2")
                    }
                    Button {
                        session.togglePrivacyScreen()
                    } label: {
                        Label("隐私屏", systemImage: "eye.slash")
                    }
                }
                Section("快捷键") {
                    Button {
                        operationsOpen = false
                        shortcutsVisible = true
                    } label: {
                        Label("快捷键面板", systemImage: "command")
                    }
                    Button {
                        operationsOpen = false
                        keyboardVisible = true
                    } label: {
                        Label("电脑键盘", systemImage: "keyboard")
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

    // MARK: 工具

    private var toolsSheet: some View {
        NavigationStack {
            List {
                Section("文件") {
                    Button { toolsOpen = false; showsTransferPanel() } label: {
                        Label("传输面板", systemImage: "arrow.up.arrow.down.circle")
                    }
                    Button { toolsOpen = false; showsFiles() } label: {
                        Label("发送文件", systemImage: "doc.badge.plus")
                    }
                    Button { toolsOpen = false; showsReceivedFiles() } label: {
                        Label("接收的文件", systemImage: "tray.and.arrow.down")
                    }
                    Button { toolsOpen = false; showsRemoteFiles() } label: {
                        Label("远端文件", systemImage: "externaldrive.connected.to.line.below")
                    }
                    Button { toolsOpen = false; showsPhotoAlbum() } label: {
                        Label("相册", systemImage: "photo.on.rectangle.angled")
                    }
                }
                Section("输入") {
                    Button { toolsOpen = false; onReleaseButtons() } label: {
                        Label("释放所有按键", systemImage: "hand.raised")
                    }
                    Button { session.sendClipboard() } label: {
                        Label("发送剪贴板", systemImage: "doc.on.clipboard")
                    }
                    Button { session.sendSecureAttention() } label: {
                        Label("发送 Ctrl+Alt+Del", systemImage: "lock.shield")
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
