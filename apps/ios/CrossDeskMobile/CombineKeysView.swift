import SwiftUI

/// 组合键面板：UU 的 `combineKeyboardViewController` 语义是「多键同时按下」，
/// 不是快捷键列表。这里按同一语义做——点一下就是把修饰键和主键一起发出去。
///
/// 键码用 Windows 虚拟键值（主机端再做平台映射），与 `RemoteSessionModel.sendKeyChord`
/// 的约定一致：keyCode + modifiers 数组。
struct CombineKeysView: View {
    let session: RemoteSessionModel
    var onSend: (UInt, [UInt]) -> Void
    var onDismiss: () -> Void

    private static let modifiers: [(String, UInt)] = [
        ("Ctrl", 0x11), ("Shift", 0x10), ("Alt", 0x12), ("Win", 0x5B),
    ]

    private static let keys: [(String, UInt)] = [
        ("Esc", 0x1B), ("Tab", 0x09), ("Enter", 0x0D), ("空格", 0x20),
        ("←", 0x25), ("↑", 0x26), ("→", 0x27), ("↓", 0x28),
        ("F1", 0x70), ("F2", 0x71), ("F3", 0x72), ("F4", 0x73),
        ("F5", 0x74), ("F11", 0x7A), ("Home", 0x24), ("End", 0x23),
        ("PgUp", 0x21), ("PgDn", 0x22), ("Del", 0x2E), ("Backspace", 0x08),
    ]

    @State private var held: Set<UInt> = []

    var body: some View {
        NavigationStack {
            List {
                Section("按住修饰键，再点主键（可多选）") {
                    LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 4), spacing: 8) {
                        ForEach(Self.modifiers, id: \.1) { name, code in
                            chip(name, active: held.contains(code)) {
                                toggle(code)
                            }
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section("主键") {
                    LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 4), spacing: 8) {
                        ForEach(Self.keys, id: \.1) { name, code in
                            chip(name, active: false) {
                                var sent = Array(held)
                                sent.append(code)
                                onSend(code, Array(held))
                                held.removeAll()
                                _ = sent
                            }
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section("常用组合") {
                    ForEach(Self.presets, id: \.0) { title, key, mods in
                        Button {
                            session.sendKeyChord(key, modifiers: mods)
                            onDismiss()
                        } label: {
                            HStack {
                                Text(title)
                                Spacer()
                                Image(systemName: "paperplane")
                            }
                        }
                    }
                }
            }
            .navigationTitle("组合键")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("完成") { onDismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private static let presets: [(String, UInt, [UInt])] = [
        ("Ctrl+Alt+Del", 0x2E, [0x11, 0x12]),
        ("Alt+Tab", 0x09, [0x12]),
        ("Win+D 显示桌面", 0x44, [0x5B]),
        ("Win+Tab 所有窗口", 0x09, [0x5B]),
        ("Ctrl+Shift+Esc 任务管理器", 0x1B, [0x11, 0x10]),
        ("Alt+F4 关闭窗口", 0x73, [0x12]),
    ]

    private func chip(_ title: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 13, weight: .medium))
                .frame(maxWidth: .infinity, minHeight: 34)
                .background(
                    RoundedRectangle(cornerRadius: 8)
                        .fill(active ? Color.accentColor.opacity(0.22) : Color.primary.opacity(0.07))
                )
                .foregroundStyle(active ? Color.accentColor : Color.primary)
        }
        .buttonStyle(.plain)
    }

    private func toggle(_ code: UInt) {
        if held.contains(code) { held.remove(code) } else { held.insert(code) }
    }
}
