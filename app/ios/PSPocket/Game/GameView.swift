import SwiftUI
import UniformTypeIdentifiers
import UIKit

/// The game tab: pick the game's disc image in Files and boot it. Once the game runs, RootView shows GameScreen
/// full screen in landscape instead of the tabs.
struct GameView: View {
    @Environment(GameModel.self) private var game
    @State private var picking = false

    var body: some View {
        VStack(spacing: 14) {
            VStack(spacing: 4) {
                Text(game.title ?? "No game chosen").font(.title2.bold())
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
            }
            GameLayerView(screen: game.screen)
                .aspectRatio(1152.0 / 640.0, contentMode: .fit)
                .clipShape(RoundedRectangle(cornerRadius: 8))
            if let problem = game.problem {
                Text(problem).font(.footnote).foregroundStyle(.orange).multilineTextAlignment(.center)
            }
            HStack(spacing: 12) {
                Button {
                    picking = true
                } label: {
                    Label("Choose ISO", systemImage: "folder")
                }
                .buttonStyle(.bordered)
                Button {
                    game.boot()
                } label: {
                    Label("Boot", systemImage: "play.fill")
                }
                .buttonStyle(.borderedProminent)
                .disabled(!game.canBoot)
            }
            Label(game.controllerName.map { "\($0) connected" } ?? "No controller: on-screen buttons",
                  systemImage: game.controllerName == nil ? "hand.tap" : "gamecontroller.fill")
                .font(.footnote)
                .foregroundStyle(.secondary)
            Text("The game runs on the interpreter for now: expect a slow start, about a minute to the first screens.")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Spacer(minLength: 0)
        }
        .padding()
        .fileImporter(isPresented: $picking, allowedContentTypes: Self.discTypes) { result in
            if case .success(let url) = result { game.pickedISO(url) }
        }
    }

    private static let discTypes: [UTType] = [UTType(filenameExtension: "iso"), .diskImage, .data].compactMap { $0 }

    private var subtitle: String {
        if let id = game.titleID { return "\(id) · \(game.isoName ?? "")" }
        return game.isoName ?? "Choose the game's .iso in Files"
    }
}

/// The running game, full screen: its frames scaled to fit, the on-screen buttons over them while no controller is
/// connected, and (tap with two fingers) a small status and log overlay.
struct GameScreen: View {
    @Environment(GameModel.self) private var game
    @State private var showInfo = false

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
            Text(String(format: "%.0f s · %llu frames · %@", game.seconds, game.flips,
                        game.controllerName ?? "on-screen buttons"))
                .font(.caption.bold())
            Text(game.logTail.split(separator: "\n").suffix(8).joined(separator: "\n"))
                .font(.system(size: 9, design: .monospaced))
                .lineLimit(8)
        }
        .padding(8)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 6))
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(12)
    }
}

/// On-screen buttons laid over the game in landscape: shoulders at the top corners, the D-pad bottom left, the face
/// buttons bottom right, Select and Start in the middle. Each button is down while touched.
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
                diamond(top: (.up, "▲"), left: (.left, "◀"), right: (.right, "▶"), bottom: (.down, "▼"))
                Spacer()
                HStack(spacing: 18) {
                    PadButton(game: game, button: .select, label: "SELECT", shape: .capsule)
                    PadButton(game: game, button: .start, label: "START", shape: .capsule)
                }
                .padding(.bottom, 6)
                Spacer()
                diamond(top: (.triangle, "△"), left: (.square, "□"), right: (.circle, "○"), bottom: (.cross, "✕"))
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
