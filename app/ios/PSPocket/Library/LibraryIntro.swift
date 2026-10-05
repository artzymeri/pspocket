import SwiftUI
import UIKit

/// The intro at launch (D-142, the design's intro, about 3.3 s): on black, the app icon's mark in white scales in from
/// a blur, "PSPocket" appears as its letter spacing tightens, over a soft blue bloom and a thin line. At 1.7 s
/// (`leaving`) it fades out and grows a little while the library builds in behind it. A tap or any button skips it.
struct LibraryIntro: View {
    let start: Date
    let leaving: Bool

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 60, paused: false)) { timeline in
            let t = timeline.date.timeIntervalSince(start)
            ZStack {
                Color.black
                bloom(t)
                VStack(spacing: 0) {
                    mark(t)
                        .padding(.top, -40)
                        .padding(.bottom, -38)
                    word(t)
                    line(t)
                        .padding(.top, 12)
                }
            }
        }
        .opacity(leaving ? 0 : 1)
        .animation(.easeInOut(duration: 0.8), value: leaving)
        .scaleEffect(leaving ? 1.08 : 1)
        .animation(.timingCurve(0.16, 1, 0.3, 1, duration: 1.1), value: leaving)
        .ignoresSafeArea()
        .contentShape(Rectangle())
    }

    /// 0...1 progress of a part that starts at `delay` and lasts `duration`.
    private func progress(_ t: Double, delay: Double, duration: Double) -> Double {
        min(1, max(0, (t - delay) / duration))
    }

    private func bloom(_ t: Double) -> some View {
        // The design's ppk-bloom: opacity 0 -> 1 by half way, then down to 0.45; scale 0.4 -> 1.7; ease-out.
        let p = progress(t, delay: 0, duration: 1.9)
        func easeOut(_ x: Double) -> Double { 1 - pow(1 - x, 2) }
        let opacity = p < 0.5 ? easeOut(p / 0.5) : 1 - 0.55 * easeOut((p - 0.5) / 0.5)
        let e = easeOut(p)
        return Circle()
            .fill(RadialGradient(stops: [.init(color: Color(red: 110 / 255, green: 130 / 255, blue: 1).opacity(0.32),
                                               location: 0),
                                         .init(color: Color(red: 110 / 255, green: 130 / 255, blue: 1).opacity(0),
                                               location: 0.65)],
                                 center: .center, startRadius: 0, endRadius: 280))
            .frame(width: 560, height: 560)
            .scaleEffect(0.4 + 1.3 * e)
            .opacity(min(1, max(0, opacity)))
    }

    /// The app icon's mark in white: the icon (a navy mark on white) inverted is a light mark on black, and its
    /// luminance, contrast raised so the mark is fully opaque and the black stays clear, masks white. (The design's CSS
    /// brightness multiplies; SwiftUI's brightness adds, which turned the icon's black square grey.)
    private func mark(_ t: Double) -> some View {
        let p = progress(t, delay: 0, duration: 0.9)
        let e = Ease.outExpo(p)
        let early = min(1, p / 0.6)
        return Color.white
            .frame(width: 200, height: 200)
            .mask {
                Image("IntroMark")
                    .resizable()
                    .interpolation(.high)
                    .colorInvert()
                    .contrast(1.6)
                    .luminanceToAlpha()
            }
            .blur(radius: 14 * (1 - Ease.outExpo(early)))
            .scaleEffect(0.55 + 0.45 * e)
            .opacity(Ease.outExpo(early))
    }

    private func word(_ t: Double) -> some View {
        let e = Ease.outExpo(progress(t, delay: 0.22, duration: 1.0))
        return Text("PSPocket")
            .font(.system(size: 30, weight: .bold))
            .tracking(30 * (0.7 - 0.69 * e))
            .foregroundStyle(.white)
            .blur(radius: 10 * (1 - e))
            .opacity(e)
            .fixedSize()
    }

    private func line(_ t: Double) -> some View {
        let p = progress(t, delay: 0.45, duration: 1.3)
        let e = p < 0.5 ? 2 * p * p : 1 - pow(-2 * p + 2, 2) / 2 // ease
        let opacity = p < 0.4 ? p / 0.4 : 1 - (p - 0.4) / 0.6
        return LinearGradient(colors: [.white.opacity(0), .white, .white.opacity(0)], startPoint: .leading,
                              endPoint: .trailing)
            .frame(width: 160, height: 1)
            .scaleEffect(x: e, y: 1)
            .opacity(p == 0 ? 0 : opacity)
    }
}

/// Booting a game (D-142, the design's boot sheet): a dark blur over everything, the cover, an indeterminate bar, how
/// long the first screens take, and Cancel (○) while the boot can still be called off.
struct BootOverlay: View {
    let cover: UIImage?
    let title: String
    let cancellable: Bool
    let pad: Bool
    /// Set while this iPhone prepares the game, the first time it is played (D-154): a bar that fills, what it does
    /// and the time left.
    var preparation: Preparer.Snapshot? = nil
    var timeLeft: String = ""
    /// Set once the game's threads run, until its first frame: how long it has been starting, and what it last asked
    /// for that pspocket does not do yet (a game that never draws looked frozen: Assassin's Creed, run 096).
    var starting: Starting? = nil
    let cancel: () -> Void

    struct Starting {
        let seconds: Double
        let logTail: String

        /// The last call the log names as not done ("UNIMPLEMENTED cellSync::cellSyncMutexTryLock (0x...)").
        var missing: String? {
            for line in logTail.split(separator: "\n").reversed() {
                guard let range = line.range(of: "UNIMPLEMENTED ") else { continue }
                let rest = line[range.upperBound...]
                return String(rest.prefix { $0 != " " })
            }
            return nil
        }
    }

    var body: some View {
        ZStack {
            Color(red: 5 / 255, green: 5 / 255, blue: 7 / 255).opacity(0.8)
                .background(.ultraThinMaterial)
            VStack(spacing: 18) {
                Group {
                    if let cover {
                        Image(uiImage: cover)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                    } else {
                        Text(title)
                            .font(.system(size: 28, weight: .bold))
                            .multilineTextAlignment(.center)
                    }
                }
                .frame(width: 150, height: 174)
                if let preparation {
                    Text("Preparing \(title) for this iPhone")
                        .font(.system(size: 17, weight: .semibold))
                        .tracking(-0.4)
                    ProgressBar(fraction: preparation.fraction)
                        .frame(width: 280, height: 4)
                    HStack(spacing: 10) {
                        Text(preparation.detail)
                        Text("·")
                        Text(timeLeft).monospacedDigit()
                    }
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(.white.opacity(0.85))
                    Text("Once, the first time: the game's code is translated and compiled here. Keep pspocket open; "
                         + "later launches start at once.")
                        .font(.system(size: 13))
                        .tracking(-0.08)
                        .multilineTextAlignment(.center)
                        .foregroundStyle(Color(red: 235 / 255, green: 235 / 255, blue: 245 / 255).opacity(0.72))
                        .frame(maxWidth: 420)
                } else {
                    IndeterminateBar()
                        .frame(width: 240, height: 4)
                    if let starting {
                        Text("Starting · \(Self.clock(starting.seconds))")
                            .font(.system(size: 13, weight: .medium))
                            .monospacedDigit()
                            .foregroundStyle(.white.opacity(0.85))
                    }
                    Text(note)
                        .font(.system(size: 13))
                        .tracking(-0.08)
                        .multilineTextAlignment(.center)
                        .foregroundStyle(Color(red: 235 / 255, green: 235 / 255, blue: 245 / 255).opacity(0.72))
                        .frame(maxWidth: 420)
                }
                Button(action: cancel) {
                    HStack(spacing: 8) {
                        if pad { ButtonBadge(kind: .circle) }
                        Text("Cancel").font(.system(size: 15, weight: .semibold)).tracking(-0.23)
                    }
                    .padding(.horizontal, 18)
                    .frame(height: 44)
                    .contentShape(Capsule())
                }
                .buttonStyle(PressScale(scale: 0.96))
                .glass(Capsule())
                .opacity(cancellable ? 1 : 0)
                .allowsHitTesting(cancellable)
                .animation(.easeInOut(duration: 0.3), value: cancellable)
            }
            .foregroundStyle(.white)
        }
        .ignoresSafeArea()
        .statusBarHidden(true)
    }

    private var note: String {
        if let starting, starting.seconds >= 45 {
            if let missing = starting.missing {
                return "No picture yet. The game last asked for \(missing), which pspocket doesn't do yet; it may "
                    + "not get further. Swipe pspocket away to quit."
            }
            return "No picture yet: some games take a few minutes to their first screen."
        }
        return "The game's code runs translated ahead of time: about a minute from Boot to the first screens."
    }

    private static func clock(_ seconds: Double) -> String {
        let s = Int(seconds)
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}

/// A bar that fills to `fraction`, easing to each new value.
private struct ProgressBar: View {
    let fraction: Double

    var body: some View {
        GeometryReader { geo in
            Capsule()
                .fill(.white)
                .frame(width: max(geo.size.height, geo.size.width * min(max(fraction, 0), 1)), height: geo.size.height)
                .animation(.easeOut(duration: 0.5), value: fraction)
        }
        .background(Capsule().fill(.white.opacity(0.18)))
        .clipShape(Capsule())
    }
}

/// A bar 40% wide sliding through its track every 1.4 s.
private struct IndeterminateBar: View {
    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 60, paused: false)) { timeline in
            let t = timeline.date.timeIntervalSinceReferenceDate
            let p = Ease.bezier(0.45, 0, 0.25, 1, t.truncatingRemainder(dividingBy: 1.4) / 1.4)
            GeometryReader { geo in
                let bar = geo.size.width * 0.4
                Capsule()
                    .fill(.white)
                    .frame(width: bar, height: geo.size.height)
                    .offset(x: bar * (-1.1 + 3.7 * p))
            }
            .background(Capsule().fill(.white.opacity(0.18)))
            .clipShape(Capsule())
        }
    }
}
