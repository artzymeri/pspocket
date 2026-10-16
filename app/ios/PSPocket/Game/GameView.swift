import SwiftUI
import UIKit

/// The running game, full screen: its frames scaled to fit, the frame rate in a corner, the on-screen buttons over them
/// while no controller is connected, and (double tap) a small status and log overlay.
struct GameScreen: View {
    @Environment(GameModel.self) private var game
    @State private var showInfo = false
    @State private var reports = 0

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            GameLayerView(screen: game.screen)
                .ignoresSafeArea()
            if game.controllerName == nil {
                TouchPad(game: game)
                    .padding(.horizontal, 28)
                    .padding(.vertical, 14)
            }
            Text(String(format: "%.0f fps", game.fps))
                .font(.caption2.monospacedDigit().bold())
                .foregroundStyle(.white.opacity(0.75))
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(.black.opacity(0.4), in: Capsule())
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                .padding(8)
            if showInfo {
                info
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onTapGesture(count: 2) { showInfo.toggle() }
    }

    private var info: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(String(format: "%.0f s · %llu frames · %.1f fps · %@", game.seconds, game.flips, game.fps,
                        game.controllerName ?? "on-screen buttons"))
                .font(.caption.bold())
            if let problem = game.problem {
                Text(problem).font(.caption2).foregroundStyle(.orange)
            }
            Text(game.logTail.split(separator: "\n").suffix(8).joined(separator: "\n"))
                .font(.system(size: 9, design: .monospaced))
                .lineLimit(8)
            // A glitch report of what was just on screen (D-1065): the last frames and the log, in Documents/reports.
            Button {
                ps_game_flag()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { reports = Int(ps_game_reports_written()) }
            } label: {
                Label(reports > 0 ? "Flag what I just saw (\(reports) saved)" : "Flag what I just saw",
                      systemImage: "flag")
                    .font(.caption.bold())
            }
            .buttonStyle(.borderedProminent)
            .tint(.orange)
        }
        .padding(8)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 6))
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(12)
    }
}

/// On-screen buttons laid over the game in landscape: shoulders at the top corners, the D-pad on the left with the
/// left stick under it, the face buttons on the right with the right stick under them, Select, Start, L3 and R3 in the
/// middle. Each button is down while touched; a stick follows the thumb and springs back when let go.
private struct TouchPad: View {
    let game: GameModel

    var body: some View {
        VStack {
            HStack {
                PadButton(game: game, button: .l2, label: "L2", shape: .capsule)
                PadButton(game: game, button: .l1, label: "L1", shape: .capsule)
                Spacer()
                PadButton(game: game, button: .r1, label: "R1", shape: .capsule)
                PadButton(game: game, button: .r2, label: "R2", shape: .capsule)
            }
            Spacer()
            HStack(alignment: .bottom) {
                VStack(spacing: 10) {
                    diamond(top: (.up, "▲"), left: (.left, "◀"), right: (.right, "▶"), bottom: (.down, "▼"))
                    VirtualStick(game: game, stick: .left)
                }
                Spacer()
                VStack(spacing: 12) {
                    HStack(spacing: 18) {
                        PadButton(game: game, button: .l3, label: "L3", shape: .capsule)
                        PadButton(game: game, button: .r3, label: "R3", shape: .capsule)
                    }
                    HStack(spacing: 18) {
                        PadButton(game: game, button: .select, label: "SELECT", shape: .capsule)
                        PadButton(game: game, button: .start, label: "START", shape: .capsule)
                    }
                }
                .padding(.bottom, 6)
                Spacer()
                VStack(spacing: 10) {
                    diamond(top: (.triangle, "△"), left: (.square, "□"), right: (.circle, "○"), bottom: (.cross, "✕"))
                    VirtualStick(game: game, stick: .right)
                }
            }
        }
    }

    private func diamond(top: (GameModel.Button, String), left: (GameModel.Button, String),
                         right: (GameModel.Button, String), bottom: (GameModel.Button, String)) -> some View {
        VStack(spacing: 2) {
            PadButton(game: game, button: top.0, label: top.1)
            HStack(spacing: 46) {
                PadButton(game: game, button: left.0, label: left.1)
                PadButton(game: game, button: right.0, label: right.1)
            }
            PadButton(game: game, button: bottom.0, label: bottom.1)
        }
    }
}

/// An analog stick: a base the thumb drags a knob across. Its deflection from the base's centre, clamped to the base,
/// is the stick's position (y grows downwards, as on the PS3); letting go recentres it.
private struct VirtualStick: View {
    let game: GameModel
    let stick: GameModel.Stick
    @State private var knob: CGSize = .zero
    private let radius: CGFloat = 46

    var body: some View {
        ZStack {
            Circle()
                .fill(Color.white.opacity(0.12))
                .overlay(Circle().stroke(Color.white.opacity(0.3), lineWidth: 1.5))
                .frame(width: radius * 2, height: radius * 2)
            Circle()
                .fill(Color.white.opacity(knob == .zero ? 0.3 : 0.5))
                .frame(width: 42, height: 42)
                .offset(knob)
        }
        .frame(width: radius * 2 + 24, height: radius * 2 + 24)
        .contentShape(Circle())
        .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                let center = radius + 12
                var dx = value.location.x - center, dy = value.location.y - center
                let length = (dx * dx + dy * dy).squareRoot()
                if length > radius {
                    dx *= radius / length
                    dy *= radius / length
                }
                knob = CGSize(width: dx, height: dy)
                game.setStick(stick, x: Float(dx / radius), y: Float(dy / radius))
            }
            .onEnded { _ in
                knob = .zero
                game.setStick(stick, x: 0, y: 0)
            })
    }
}

private struct PadButton: View {
    enum Shape { case circle, capsule }

    let game: GameModel
    let button: GameModel.Button
    let label: String
    var shape: Shape = .circle
    @State private var down = false

    var body: some View {
        Text(label)
            .font(shape == .circle ? .title3.bold() : .caption2.bold())
            .foregroundStyle(.white.opacity(0.85))
            .frame(width: shape == .circle ? 48 : 62, height: shape == .circle ? 48 : 28)
            .background {
                let fill = Color.white.opacity(down ? 0.45 : 0.18)
                if shape == .circle { Circle().fill(fill) } else { Capsule().fill(fill) }
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !down {
                        down = true
                        game.set(button, down: true)
                    }
                }
                .onEnded { _ in
                    down = false
                    game.set(button, down: false)
                })
    }
}

/// Shows the model's one GameLayerUIView: SwiftUI moves it from the setup screen to the full-screen game.
private struct GameLayerView: UIViewRepresentable {
    let screen: GameLayerUIView

    func makeUIView(context: Context) -> GameLayerUIView { screen }

    func updateUIView(_ uiView: GameLayerUIView, context: Context) {}
}
