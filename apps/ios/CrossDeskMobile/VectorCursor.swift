import UIKit

/// Draws UU远程 自己的矢量光标（Assets.car 里的 `cursor_*.pdf`）。
///
/// 为什么不用位图：UU 的 iOS 端 `CursorView` 加载的就是这组 PDF，画布 128×128，
/// 箭头实际画在 47.6×70.5 的区域里（顶点约在 (63.5, 0)）。用同一份素材、同一套
/// 实测热点，指针的样子才和它一致，而不是"看起来差不多"。
///
/// 渲染走 CoreGraphics：`CGPDFDocument` 打开包内的 PDF，按目标尺寸重绘，带缓存。
final class VectorCursorCache {
    static let shared = VectorCursorCache()

    private var cache: [String: UIImage] = [:]
    private let lock = NSLock()

    func image(named asset: String, pointSize: CGSize) -> UIImage? {
        let key = "\(asset)@\(Int(pointSize.width))x\(Int(pointSize.height))"
        lock.lock()
        if let cached = cache[key] {
            lock.unlock()
            return cached
        }
        lock.unlock()

        guard let url = Bundle.main.url(forResource: asset, withExtension: "pdf",
                                        subdirectory: "cursor"),
              let document = CGPDFDocument(url as CFURL),
              let page = document.page(at: 1) else { return nil }

        let box = page.getBoxRect(.mediaBox)
        guard box.width > 0, box.height > 0 else { return nil }
        let scale = UIScreen.main.scale
        let format = UIGraphicsImageRendererFormat()
        format.scale = scale
        format.opaque = false
        let renderer = UIGraphicsImageRenderer(size: pointSize, format: format)
        let image = renderer.image { context in
            let cg = context.cgContext
            cg.interpolationQuality = .high
            cg.translateBy(x: 0, y: pointSize.height)
            cg.scaleBy(x: pointSize.width / box.width,
                       y: -pointSize.height / box.height)
            cg.drawPDFPage(page)
        }

        lock.lock()
        cache[key] = image
        lock.unlock()
        return image
    }
}
