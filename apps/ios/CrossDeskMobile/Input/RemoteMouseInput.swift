import CoreGraphics
import Foundation

/// Shared by the video surface and the virtual buttons. Coordinates always
/// refer to the remote cursor, never to the floating controls' screen location.
final class RemoteMouseInput {
    var send: ((Float, Float, Int, Int) -> Void)?
    var didCancel: (() -> Void)?
    var relativeMotion: ((Float, Float) -> Void)?
    var viewportSize = CGSize(width: 0, height: 0)
    private(set) var x: Float = 0.5
    private(set) var y: Float = 0.5
    private(set) var revision: UInt64 = 0
    private var gestureLeft = false
    private var virtualLeft = false
    private var virtualRight = false
    private var virtualMiddle = false
    var hasVirtualButton: Bool { virtualLeft || virtualRight || virtualMiddle }

    func position(x: Float, y: Float) {
        self.x = min(max(x, 0), 1)
        self.y = min(max(y, 0), 1)
    }

    func drag(dx: Float, dy: Float) {
        guard hasVirtualButton, viewportSize.width > 0, viewportSize.height > 0 else { return }
        if let relativeMotion {
            relativeMotion(dx, dy)
        } else {
            position(x: x + dx / Float(viewportSize.width) * 1.35,
                     y: y + dy / Float(viewportSize.height) * 1.35)
        }
        emit(0)
    }

    /// 相对移动：不需要按住任何键，用于「模拟鼠标」的拖动面板。
    /// 速度曲线与触摸板一致，慢推精细、快划跨屏。
    func nudge(dx: CGFloat, dy: CGFloat) {
        guard viewportSize.width > 0, viewportSize.height > 0 else { return }
        let travel = (dx * dx + dy * dy).squareRoot()
        let gain = 0.9 + min(1.6, travel / 12.0)
        position(x: x + Float(dx * gain / viewportSize.width),
                 y: y + Float(dy * gain / viewportSize.height))
        emit(0)
    }

    func setGestureLeft(_ down: Bool) {
        guard !(down && hasVirtualButton), gestureLeft != down else { return }
        gestureLeft = down
        emit(down ? 1 : 2)
    }

    func setVirtualButton(left: Bool, down: Bool) {
        guard (left ? virtualLeft : virtualRight) != down else { return }
        if down {
            setGestureLeft(false)
            revision &+= 1
        }
        if left { virtualLeft = down } else { virtualRight = down }
        emit(left ? (down ? 1 : 2) : (down ? 3 : 4))
    }

    /// 中键：线协议里的 middle_down(5) / middle_up(6)，桌面端注入 MIDDLEDOWN/UP。
    func setMiddleButton(down: Bool) {
        guard virtualMiddle != down else { return }
        if down { setGestureLeft(false) }
        virtualMiddle = down
        emit(down ? 5 : 6)
    }

    func scroll(_ direction: Int) {
        guard direction != 0 else { return }
        emit(7, wheel: direction > 0 ? 1 : -1)
    }

    func releaseAll() {
        setGestureLeft(false)
        setVirtualButton(left: true, down: false)
        setVirtualButton(left: false, down: false)
        if virtualMiddle { setMiddleButton(down: false) }
        revision &+= 1
        didCancel?()
    }

    private func emit(_ flag: Int, wheel: Int = 0) { send?(x, y, flag, wheel) }
}
