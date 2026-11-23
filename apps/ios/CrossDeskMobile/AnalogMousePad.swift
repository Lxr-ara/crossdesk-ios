import SwiftUI

/// UU远程's "模拟鼠标" widget, rebuilt from its own resources.
///
/// Geometry comes from the vector assets inside `com.netease.uuremote` 4.48.1
/// (`analogMouse*` family): a 108x119 pt body, a 36x36 pt scroll button, a
/// 116x116 pt scroll mode overlay and a 24x24 pt pointer. Those numbers live in
/// `libs/wire/include/remote_cursor_metrics.h` and are generated into
/// `CursorMetrics.generated.swift`, so the desktop overlay derives the same values.
///
/// Interactions:
///   * drag the body  -> pointer moves relatively (trackpad feel, speed curve)
///   * left / right   -> the corresponding button, hold-and-drag drags
///   * middle band    -> wheel, with acceleration
struct AnalogMousePad: View {
    let mouseInput: RemoteMouseInput
    let viewportSize: CGSize

    @AppStorage("analogMousePadOffsetX") private var storedX: Double = 0
    @AppStorage("analogMousePadOffsetY") private var storedY: Double = 0
    @AppStorage("analogMousePadScale") private var storedScale: Double = 1.0

    @State private var dragOrigin: CGSize = .zero
    @State private var scrollLast: CGFloat = 0
    @State private var leftDown = false
    @State private var rightDown = false
    @State private var scrollAccum: CGFloat = 0
    @State private var lastMovePoint: CGPoint?

    // Design units, straight from the asset catalogue.
    private let bodySize = CGSize(width: 108, height: 119)
    private let scrollBandHeight: CGFloat = 36
    private let buttonHeight: CGFloat = 44

    private var scale: CGFloat { CGFloat(min(max(storedScale, 0.7), 1.4)) }
    private var size: CGSize { CGSize(width: bodySize.width * scale, height: bodySize.height * scale) }

    var body: some View {
        GeometryReader { proxy in
            let centre = CGPoint(
                x: min(max(proxy.size.width / 2 + storedX, size.width / 2 + 8),
                       proxy.size.width - size.width / 2 - 8),
                y: min(max(proxy.size.height / 2 + storedY, size.height / 2 + 8),
                       proxy.size.height - size.height / 2 - 8)
            )
            mouseBody
                .frame(width: size.width, height: size.height)
                .position(x: centre.x, y: centre.y)
                .gesture(bodyDrag())
        }
        .allowsHitTesting(true)
    }

    // MARK: visual

    private var mouseBody: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                button(title: "左", active: leftDown)
                    .contentShape(Rectangle())
                    .gesture(buttonGesture(left: true))
                button(title: "右", active: rightDown)
                    .contentShape(Rectangle())
                    .gesture(buttonGesture(left: false))
            }
            .frame(height: buttonHeight * scale)

            scrollBand
                .frame(height: scrollBandHeight * scale)

            Spacer(minLength: 0)
        }
        .background(
            RoundedRectangle(cornerRadius: 26 * scale, style: .continuous)
                .fill(.ultraThinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 26 * scale, style: .continuous)
                        .strokeBorder(.white.opacity(0.35), lineWidth: 1)
                )
                .shadow(color: .black.opacity(0.25), radius: 10, y: 4)
        )
        .clipShape(RoundedRectangle(cornerRadius: 26 * scale, style: .continuous))
    }

    private func button(title: String, active: Bool) -> some View {
        Text(title)
            .font(.system(size: 13 * scale, weight: .medium))
            .foregroundStyle(active ? Color.white : Color.primary.opacity(0.75))
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(active ? Color.accentColor.opacity(0.55) : Color.primary.opacity(0.06))
    }

    private var scrollBand: some View {
        HStack(spacing: 3 * scale) {
            Image(systemName: "chevron.up")
            Image(systemName: "chevron.down")
        }
        .font(.system(size: 11 * scale, weight: .semibold))
        .foregroundStyle(Color.primary.opacity(0.55))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.primary.opacity(0.04))
        .contentShape(Rectangle())
        .gesture(scrollGesture)
    }

    // MARK: gestures

    private func bodyDrag() -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                guard let previous = lastMovePoint else {
                    lastMovePoint = value.location
                    dragOrigin = CGSize(width: storedX, height: storedY)
                    return
                }
                let dx = value.location.x - previous.x
                let dy = value.location.y - previous.y
                lastMovePoint = value.location
                if leftDown {
                    // 按住左键拖面板 = 把控件本身挪开，避免挡住画面。
                    storedX = Double(dragOrigin.width + value.translation.width)
                    storedY = Double(dragOrigin.height + value.translation.height)
                } else {
                    mouseInput.nudge(dx: dx, dy: dy)
                }
            }
            .onEnded { _ in
                lastMovePoint = nil
                dragOrigin = .zero
            }
    }

    private func buttonGesture(left: Bool) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                if left {
                    guard !leftDown else {
                        mouseInput.nudge(dx: value.translation.width, dy: value.translation.height)
                        dragOrigin = value.translation
                        return
                    }
                    leftDown = true
                    dragOrigin = .zero
                    mouseInput.setVirtualButton(left: true, down: true)
                } else {
                    guard !rightDown else { return }
                    rightDown = true
                    mouseInput.setVirtualButton(left: false, down: true)
                }
            }
            .onEnded { _ in
                if left {
                    leftDown = false
                    dragOrigin = .zero
                    mouseInput.setVirtualButton(left: true, down: false)
                } else {
                    rightDown = false
                    mouseInput.setVirtualButton(left: false, down: false)
                }
            }
    }

    private var scrollGesture: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                guard viewportSize.height > 0 else { return }
                let dy = value.translation.height - scrollLast
                scrollLast = value.translation.height
                scrollAccum += dy
                // 与两指滚轮同一套手感：12 pt 一格，快滑放大步长，最多 3 格。
                let speed = abs(dy)
                let gain = min(3.0, 1.0 + speed / 20.0)
                let step = CGFloat(12.0 / gain)
                while abs(scrollAccum) >= step {
                    mouseInput.scroll(scrollAccum > 0 ? 1 : -1)
                    scrollAccum -= scrollAccum > 0 ? step : -step
                }
            }
            .onEnded { _ in
                scrollAccum = 0
                scrollLast = 0
            }
    }

}
