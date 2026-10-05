import SwiftUI
import UIKit

/// The library's animated background, from the focused game's cover (D-142, the design's backdrop): the cover at its
/// own aspect on the right (a radial fade, a slow 1 -> 1.1 zoom over 12 s and back), three larger blurred, saturated
/// copies drifting and turning in screen blend (8 s, 10 s, a 16 s spin), 160 rising motes and a soft white sweep every
/// 6 s. A new focus crossfades in over 700 ms, starting at 1.18x and brighter and settling over 1.1 s. Everything is a
/// function of the clock (TimelineView, at most 60 Hz); the blurs were made once (Backdrop).
struct LibraryBackground: View {
    let focusID: String?
    let backdrop: Backdrop?

    private struct Layer: Identifiable {
        let id: UUID
        let key: String
        let backdrop: Backdrop
        let start: Date
        var end: Date?
    }

    @State private var layers: [Layer] = []

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 60, paused: false)) { timeline in
            let now = timeline.date
            let t = now.timeIntervalSinceReferenceDate
            GeometryReader { geo in
                let size = geo.size
                ZStack(alignment: .topLeading) {
                    EllipticalGradient(stops: [.init(color: Color(red: 0x24 / 255, green: 0x24 / 255, blue: 0x2C / 255), location: 0),
                                               .init(color: Color(red: 0x0D / 255, green: 0x0D / 255, blue: 0x11 / 255), location: 0.58),
                                               .init(color: Color(red: 0x05 / 255, green: 0x05 / 255, blue: 0x07 / 255), location: 1)],
                                       center: UnitPoint(x: 0.72, y: 0.22), startRadiusFraction: 0, endRadiusFraction: 1.2)
                    ForEach(layers) { layer in
                        art(layer.backdrop, size: size, t: t)
                            .scaleEffect(punchScale(layer, now))
                            .brightness(punchBrightness(layer, now))
                            .opacity(layerOpacity(layer, now))
                    }
                    Canvas { context, canvasSize in
                        drawMotes(&context, canvasSize, t)
                        drawSweep(&context, canvasSize, t)
                    }
                }
                .frame(width: size.width, height: size.height)
                .clipped()
            }
        }
        .onAppear { refresh() }
        .onChange(of: focusID) { refresh() }
        .onChange(of: backdrop?.image) { refresh() }
    }

    /// A new focus (or its cover arriving) adds a layer; the old one fades out and is dropped once gone.
    private func refresh() {
        let now = Date()
        layers.removeAll { $0.end.map { now.timeIntervalSince($0) > 0.8 } ?? false }
        let key = focusID ?? ""
        if let current = layers.last(where: { $0.end == nil }), current.key == key,
           current.backdrop.image === backdrop?.image { return }
        for i in layers.indices where layers[i].end == nil { layers[i].end = now }
        if let backdrop { layers.append(Layer(id: UUID(), key: key, backdrop: backdrop, start: now)) }
    }

    // MARK: one cover's art

    private func art(_ b: Backdrop, size: CGSize, t: TimeInterval) -> some View {
        let w = size.width, h = size.height
        // The cover itself: 446x520 at 6% from the right, 40 pt above the top.
        let kb = Ease.inOut(pingPong(t, period: 12))
        let soft = CGSize(width: 446, height: 520)
        let a = drift(t, period: 8, frames: Self.driftA)
        let bf = drift(t, period: 10, frames: Self.driftB)
        let spin = (t.truncatingRemainder(dividingBy: 16)) / 16 * 360
        return ZStack(alignment: .topLeading) {
            fill(b.soft, soft, anchor: UnitPoint(x: 0.5, y: 0.5))
                .mask(RadialGradient(stops: [.init(color: .black, location: 0.62), .init(color: .clear, location: 1)],
                                     center: .center, startRadius: 0, endRadius: min(soft.width, soft.height) / 2))
                .scaleEffect(1 + 0.1 * kb)
                .offset(x: soft.width * (-0.02 + 0.04 * kb), y: soft.height * (-0.01 + 0.02 * kb))
                .opacity(0.75)
                .offset(x: w * 0.94 - soft.width, y: -40)
            glow(b.glowA, CGSize(width: w * 0.85, height: h * 1.6), UnitPoint(x: 0.5, y: 0.5), a, opacity: 0.9)
                .offset(x: -0.2 * w, y: -0.4 * h)
            glow(b.glowB, CGSize(width: w * 0.8, height: h * 1.5), UnitPoint(x: 0.3, y: 0.7), bf, opacity: 0.75)
                .offset(x: 0.4 * w, y: -0.25 * h)
            fill(b.glowC, CGSize(width: w * 0.75, height: h * 1.3), anchor: UnitPoint(x: 0.8, y: 0.2))
                .rotationEffect(.degrees(spin))
                .opacity(0.6)
                .blendMode(.screen)
                .offset(x: 0.1 * w, y: 0.05 * h)
        }
        .frame(width: w, height: h, alignment: .topLeading)
    }

    private func glow(_ image: UIImage, _ box: CGSize, _ anchor: UnitPoint, _ f: DriftFrame, opacity: Double) -> some View {
        fill(image, box, anchor: anchor)
            .scaleEffect(f.scale)
            .rotationEffect(.degrees(f.rotate))
            .offset(x: box.width * f.x, y: box.height * f.y)
            .opacity(opacity)
            .blendMode(.screen)
    }

    /// The image covering `box` (CSS background-size: cover), its `anchor` point at the box's (background-position).
    private func fill(_ image: UIImage, _ box: CGSize, anchor: UnitPoint) -> some View {
        let iw = max(image.size.width, 1), ih = max(image.size.height, 1)
        let s = max(box.width / iw, box.height / ih)
        let dw = iw * s, dh = ih * s
        return Image(uiImage: image)
            .resizable()
            .frame(width: dw, height: dh)
            .offset(x: (box.width - dw) * anchor.x, y: (box.height - dh) * anchor.y)
            .frame(width: box.width, height: box.height, alignment: .topLeading)
            .clipped()
    }

    // MARK: the focus change

    private func layerOpacity(_ layer: Layer, _ now: Date) -> Double {
        let fadeIn = Ease.inOut(min(1, max(0, now.timeIntervalSince(layer.start) / 0.7)))
        let fadeOut = layer.end.map { Ease.inOut(min(1, max(0, now.timeIntervalSince($0) / 0.7))) } ?? 0
        return fadeIn * (1 - fadeOut)
    }

    private func punch(_ layer: Layer, _ now: Date) -> Double {
        Ease.outExpo(min(1, max(0, now.timeIntervalSince(layer.start) / 1.1)))
    }

    private func punchScale(_ layer: Layer, _ now: Date) -> CGFloat { 1.18 - 0.18 * punch(layer, now) }
    private func punchBrightness(_ layer: Layer, _ now: Date) -> Double { 0.32 * (1 - punch(layer, now)) }

    // MARK: drifting

    struct DriftFrame {
        var x: CGFloat, y: CGFloat, rotate: Double, scale: CGFloat
    }

    // The design's ppk-drift-a and ppk-drift-b keyframes (translate in fractions of the element, degrees, scale).
    private static let driftA = [DriftFrame(x: -0.14, y: -0.08, rotate: 0, scale: 1.1),
                                 DriftFrame(x: 0.10, y: 0.06, rotate: 24, scale: 1.35),
                                 DriftFrame(x: -0.06, y: 0.12, rotate: -14, scale: 1.2),
                                 DriftFrame(x: 0.12, y: -0.10, rotate: 10, scale: 1.3)]
    private static let driftB = [DriftFrame(x: 0.14, y: 0.10, rotate: 0, scale: 1.3),
                                 DriftFrame(x: -0.12, y: -0.06, rotate: -28, scale: 1.1),
                                 DriftFrame(x: 0.08, y: -0.12, rotate: 18, scale: 1.4),
                                 DriftFrame(x: -0.10, y: 0.08, rotate: -8, scale: 1.2)]

    /// Keyframes at 0, 1/3, 2/3 and 1, ease-in-out between each, played forwards then backwards (alternate).
    private func drift(_ t: TimeInterval, period: Double, frames: [DriftFrame]) -> DriftFrame {
        let p = pingPong(t, period: period) * 3
        let i = min(2, Int(p))
        let f = Ease.inOut(p - Double(i))
        let a = frames[i], b = frames[i + 1]
        func lerp(_ x: CGFloat, _ y: CGFloat) -> CGFloat { x + (y - x) * f }
        return DriftFrame(x: lerp(a.x, b.x), y: lerp(a.y, b.y), rotate: a.rotate + (b.rotate - a.rotate) * f,
                          scale: lerp(a.scale, b.scale))
    }

    /// 0 -> 1 over `period`, then 1 -> 0 over the next.
    private func pingPong(_ t: TimeInterval, period: Double) -> Double {
        let p = (t / period).truncatingRemainder(dividingBy: 2)
        return p < 1 ? p : 2 - p
    }

    // MARK: motes and sweep

    private struct Mote {
        let left: Double, size: Double, duration: Double, delay: Double, dx: Double, opacity: Double
    }

    /// The design's fixed pseudo-random spread, so each mote keeps its path.
    private static let motes: [Mote] = (0..<160).map { k in
        func r(_ n: Double) -> Double {
            let v = sin(Double(k) * 127.1 + n * 311.7) * 43758.5453
            return v - v.rounded(.down)
        }
        return Mote(left: r(1), size: (2 + r(2) * 4) / 3, duration: 7 + r(3) * 9, delay: r(4) * 16,
                    dx: (r(5) - 0.5) * 160, opacity: 0.15 + r(6) * 0.3)
    }

    private func drawMotes(_ context: inout GraphicsContext, _ size: CGSize, _ t: TimeInterval) {
        for m in Self.motes {
            let p = ((t + m.delay).truncatingRemainder(dividingBy: m.duration)) / m.duration
            let alpha = p < 0.12 ? m.opacity * p / 0.12 : p < 0.85 ? m.opacity : m.opacity * (1 - (p - 0.85) / 0.15)
            guard alpha > 0.01 else { continue }
            let x = m.left * size.width + m.dx * p
            let y = size.height + 10 - m.size / 2 - 520 * p
            let glow = m.size * 3
            context.fill(Path(ellipseIn: CGRect(x: x - glow, y: y - glow, width: glow * 2, height: glow * 2)),
                         with: .radialGradient(Gradient(colors: [.white.opacity(alpha * 0.55), .white.opacity(0)]),
                                               center: CGPoint(x: x, y: y), startRadius: 0, endRadius: glow))
            context.fill(Path(ellipseIn: CGRect(x: x - m.size / 2, y: y - m.size / 2, width: m.size, height: m.size)),
                         with: .color(.white.opacity(alpha)))
        }
    }

    /// A 24%-wide band of light, skewed 18°, crossing the screen every 6 s.
    private func drawSweep(_ context: inout GraphicsContext, _ size: CGSize, _ t: TimeInterval) {
        let p = Ease.sweep((t.truncatingRemainder(dividingBy: 6)) / 6)
        let band = size.width * 0.24
        let x = band * (-1.4 + 5.6 * p)
        let top = -0.2 * size.height, height = size.height * 1.4
        var c = context
        c.translateBy(x: x + band / 2, y: size.height / 2)
        c.concatenate(CGAffineTransform(a: 1, b: 0, c: tan(-18 * .pi / 180), d: 1, tx: 0, ty: 0))
        let rect = CGRect(x: -band / 2, y: top - size.height / 2, width: band, height: height)
        c.fill(Path(rect), with: .linearGradient(Gradient(stops: [.init(color: .white.opacity(0), location: 0),
                                                                  .init(color: .white.opacity(0.13), location: 0.5),
                                                                  .init(color: .white.opacity(0), location: 1)]),
                                                 startPoint: CGPoint(x: rect.minX, y: 0),
                                                 endPoint: CGPoint(x: rect.maxX, y: 0)))
    }
}

/// The design's easing curves (CSS cubic-bezier), as functions of progress 0...1.
enum Ease {
    static func bezier(_ x1: Double, _ y1: Double, _ x2: Double, _ y2: Double, _ x: Double) -> Double {
        if x <= 0 { return 0 }
        if x >= 1 { return 1 }
        // Solve x(s) = x by Newton's method, then y(s).
        var s = x
        for _ in 0..<8 {
            let cx = 3 * x1 * s * (1 - s) * (1 - s) + 3 * x2 * s * s * (1 - s) + s * s * s - x
            let dx = 3 * x1 * (1 - s) * (1 - 3 * s) + 3 * x2 * s * (2 - 3 * s) + 3 * s * s
            if abs(dx) < 1e-6 { break }
            s = min(1, max(0, s - cx / dx))
        }
        return 3 * y1 * s * (1 - s) * (1 - s) + 3 * y2 * s * s * (1 - s) + s * s * s
    }

    static func inOut(_ x: Double) -> Double { bezier(0.42, 0, 0.58, 1, x) }
    /// cubic-bezier(.16, 1, .3, 1): the design's settle curve.
    static func outExpo(_ x: Double) -> Double { bezier(0.16, 1, 0.3, 1, x) }
    static func sweep(_ x: Double) -> Double { bezier(0.45, 0, 0.2, 1, x) }
}
