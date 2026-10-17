import OSLog
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// The game library, the app's root screen in landscape (D-142; the design's "Game selection", 956x440 pt on an
/// iPhone 17 Pro Max): the focused game's cover as an animated background, its title, play time and Play button on
/// the left, the covers in a row along the bottom, the controller, Add Game and Settings at the top right. A game
/// controller moves the focus (the button hints and a focus ring show while it was the last input); touch focuses a
/// cover with a tap, plays it with a second tap, and scrolls the row with a swipe.
struct LibraryView: View {
    @Environment(LibraryModel.self) private var library
    @Environment(GameModel.self) private var game
    @Binding var settingsPath: [SettingsView.Page]
    @Binding var showSettings: Bool
    @State private var picking = false
    @State private var intro: IntroPhase
    @State private var introStart = Date()
    @State private var flash: String? // "add" or "settings": a controller press shows on the button
    @State private var drag: RowDrag?
    @State private var menu = MenuInput()
    @State private var removing: LibraryEntry? // Remove Game asked for (D-1092): the confirmation shows
    @State private var removal = LibraryModel.Removal()

    enum IntroPhase: Int { case logo, buildIn, done }

    private struct RowDrag {
        var startX: CGFloat
        var startIndex: Int
        var moved = false
        var tile: Int?
    }

    // The design's geometry (pt): the focused cover at the left margin, the others scaled to its right.
    private let margin: CGFloat = 76, gap: CGFloat = 14
    private let tileW: CGFloat = 148, tileH: CGFloat = 172, tileScale: CGFloat = 0.75, radius: CGFloat = 10
    private let rowBottomInset: CGFloat = 44 // tiles' bottoms at y = 396 of 440
    private let infoBottomInset: CGFloat = 236
    private static let curve = (0.16, 1.0, 0.3, 1.0)

    init(settingsPath: Binding<[SettingsView.Page]>, showSettings: Binding<Bool>, skipIntro: Bool) {
        _settingsPath = settingsPath
        _showSettings = showSettings
        _intro = State(initialValue: skipIntro ? .done : .logo)
    }

    private var pad: Bool { library.inputMode == .pad }
    private var booting: Bool { library.boot != nil }

    var body: some View {
        GeometryReader { geo in
            let size = geo.size
            ZStack(alignment: .topLeading) {
                LibraryBackground(focusID: library.focused?.id, backdrop: library.backdrop(for: library.focused))
                    .scaleEffect(intro == .logo ? 1.18 : booting ? 1.05 : 1)
                    .animation(.timingCurve(0.16, 1, 0.3, 1, duration: 1.7), value: intro)
                    .animation(.timingCurve(0.16, 1, 0.3, 1, duration: 1.7), value: booting)
                    .opacity(intro == .logo ? 0 : 1)
                    .animation(.easeInOut(duration: 1.4), value: intro)
                    .allowsHitTesting(false)
                scrims(size)
                topBar
                    .frame(width: size.width - margin, alignment: .trailing)
                    .padding(.top, 16)
                    .opacity(intro == .logo ? 0 : 1)
                    .animation(.easeInOut(duration: 0.7).delay(intro == .buildIn ? 0.7 : 0), value: intro)
                info
                    .frame(width: 600, alignment: .leading)
                    .offset(x: margin)
                    .frame(maxHeight: .infinity, alignment: .bottom)
                    .padding(.bottom, infoBottomInset)
                    .animation(.easeInOut(duration: 0.7).delay(intro == .buildIn ? 0.38 : 0)) {
                        $0.opacity(intro == .logo ? 0 : 1)
                    }
                    .animation(.timingCurve(0.16, 1, 0.3, 1, duration: 1).delay(intro == .buildIn ? 0.38 : 0)) {
                        $0.offset(y: intro == .logo ? 18 : 0)
                    }
                carousel(size)
                if intro != .done {
                    LibraryIntro(start: introStart, leaving: intro != .logo)
                        .onTapGesture { skipIntro() }
                }
            }
            .frame(width: size.width, height: size.height)
            .coordinateSpace(name: "library")
            .onAppear { Self.logLayout(size, geo.safeAreaInsets) }
            .onChange(of: size) { Self.logLayout(size, geo.safeAreaInsets) }
        }
        .background(Color(red: 5 / 255, green: 5 / 255, blue: 7 / 255))
        .ignoresSafeArea()
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .foregroundStyle(.white)
        .simultaneousGesture(DragGesture(minimumDistance: 0).onChanged { _ in touched() })
        .fileImporter(isPresented: $picking, allowedContentTypes: Self.discTypes) { result in
            if case .success(let url) = result { library.add(url) }
        }
        .confirmationDialog(removing.map { "Remove \($0.displayTitle)?" } ?? "", isPresented: Binding(
            get: { removing != nil }, set: { if !$0 { removing = nil } }), titleVisibility: .visible) {
            if let entry = removing {
                Button("Remove (keep saves) · \(Self.bytes(removal.bytes))", role: .destructive) {
                    library.remove(entry, saves: false)
                    removing = nil
                }
                if !removal.saves.isEmpty {
                    Button("Remove with saves · \(Self.bytes(removal.bytes + removal.savesBytes))", role: .destructive) {
                        library.remove(entry, saves: true)
                        removing = nil
                    }
                }
                Button("Cancel", role: .cancel) { removing = nil }
            }
        } message: {
            Text(removal.forgetsAdded
                 ? "Its translation and cached data are deleted, and it leaves the library. The disc image stays where you added it from."
                 : "The disc image, its translation and its cached data are deleted. Adding it again means translating it again.")
        }
        .onAppear(perform: start)
        .onDisappear { game.menuInput = nil }
    }

    private static let discTypes: [UTType] = [UTType(filenameExtension: "iso"), .diskImage, .data].compactMap { $0 }

    // The reader's size against the window's, at launch and on every change, in the device log (public): the library
    // was once laid out too high for its first second on the phone (run 087), not seen since.
    private static let layoutLog = Logger(subsystem: "com.artzymeri.pspocket", category: "library")
    private static func logLayout(_ size: CGSize, _ insets: EdgeInsets) {
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        let window = scene?.windows.first
        let text = String(format: "reader %.0fx%.0f safe %.0f/%.0f/%.0f/%.0f; window %.0fx%.0f safe %.0f/%.0f/%.0f/%.0f; "
                          + "orientation %d, state %d",
                          size.width, size.height, insets.top, insets.leading, insets.bottom, insets.trailing,
                          window?.bounds.width ?? 0, window?.bounds.height ?? 0, window?.safeAreaInsets.top ?? 0,
                          window?.safeAreaInsets.left ?? 0, window?.safeAreaInsets.bottom ?? 0,
                          window?.safeAreaInsets.right ?? 0, scene?.effectiveGeometry.interfaceOrientation.rawValue ?? -1,
                          UIApplication.shared.applicationState.rawValue)
        layoutLog.notice("pspocket library layout: \(text, privacy: .public)")
    }

    // MARK: the parts

    private func scrims(_ size: CGSize) -> some View {
        ZStack(alignment: .top) {
            LinearGradient(stops: [.init(color: .black.opacity(0.72), location: 0),
                                   .init(color: .black.opacity(0.4), location: 0.38),
                                   .init(color: .clear, location: 0.7)], startPoint: .leading, endPoint: .trailing)
            LinearGradient(stops: [.init(color: .black.opacity(0.8), location: 0),
                                   .init(color: .black.opacity(0.3), location: 0.4),
                                   .init(color: .clear, location: 0.62)], startPoint: .bottom, endPoint: .top)
            LinearGradient(colors: [.black.opacity(0.42), .clear], startPoint: .top, endPoint: .bottom)
                .frame(height: 120)
        }
        .frame(width: size.width, height: size.height)
        .allowsHitTesting(false)
    }

    private var topBar: some View {
        HStack(spacing: 10) {
            HStack(spacing: 7) {
                Image(systemName: game.controllerName == nil ? "hand.tap" : "gamecontroller.fill")
                    .font(.system(size: 16, weight: .semibold))
                Text(game.controllerName ?? "On-screen buttons")
                    .font(.system(size: 13, weight: .semibold))
            }
            .foregroundStyle(.white.opacity(0.92))
            .padding(.leading, 11)
            .padding(.trailing, 14)
            .frame(height: 36)
            .glass(Capsule())
            if pad {
                hintButton("Add Game", badge: .square, flashed: flash == "add", action: addGame)
                hintButton("Settings", badge: .menu, flashed: flash == "settings", action: openSettings)
            } else {
                roundButton("plus", size: 22, flashed: flash == "add", action: addGame)
                roundButton("gearshape.fill", size: 20, flashed: flash == "settings", action: openSettings)
            }
        }
        .fixedSize()
    }

    static func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }

    private func roundButton(_ symbol: String, size: CGFloat, flashed: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: size, weight: .medium))
                .frame(width: 44, height: 44)
                .contentShape(Circle())
        }
        .buttonStyle(PressScale(scale: 0.92))
        .glass(Circle())
        .scaleEffect(flashed ? 0.92 : 1)
        .animation(.easeOut(duration: 0.15), value: flashed)
    }

    private func hintButton(_ title: String, badge: ButtonBadge.Kind, flashed: Bool,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 8) {
                ButtonBadge(kind: badge)
                Text(title).font(.system(size: 15, weight: .semibold)).tracking(-0.23)
            }
            .padding(.leading, 11)
            .padding(.trailing, 16)
            .frame(height: 44)
            .contentShape(Capsule())
        }
        .buttonStyle(PressScale(scale: 0.92))
        .glass(Capsule())
        .scaleEffect(flashed ? 0.92 : 1)
        .animation(.easeOut(duration: 0.15), value: flashed)
    }

    private var info: some View {
        let entry = library.focused
        return VStack(alignment: .leading, spacing: 6) {
            Text(entry?.displayTitle ?? "No games yet")
                .font(.system(size: 34, weight: .bold))
                .tracking(0.37)
                .lineLimit(2)
                .shadow(color: .black.opacity(0.35), radius: 7, y: 2)
            Text(entry.map { library.meta(for: $0) }
                 ?? "Copy .iso files (or a PSN game's .pkg and .rap) into Files › On My iPhone › PSPocket › Games, or add one with +.")
                .font(.system(size: 15))
                .tracking(-0.23)
                .foregroundStyle(Color(red: 235 / 255, green: 235 / 255, blue: 245 / 255).opacity(0.75))
                .lineLimit(2)
            if entry != nil {
                HStack(spacing: 12) {
                    Button(action: play) {
                        HStack(spacing: 8) {
                            if pad {
                                ButtonBadge(kind: .cross, inverted: true, diameter: 24)
                            } else {
                                Image(systemName: "play.fill").font(.system(size: 20, weight: .semibold))
                            }
                            Text("Play").font(.system(size: 17, weight: .semibold)).tracking(-0.43)
                        }
                        .foregroundStyle(.black)
                        .padding(.leading, 14)
                        .padding(.trailing, 24)
                        .frame(height: 44)
                        .background(Capsule().fill(.white))
                        .shadow(color: .black.opacity(0.35), radius: 9, y: 6)
                    }
                    .buttonStyle(PressScale(scale: 0.96))
                    // The game's other actions (D-1092): Remove Game, with a confirmation that says what goes.
                    Menu {
                        Button(role: .destructive) {
                            guard let entry else { return }
                            removal = library.removal(for: entry)
                            removing = entry
                        } label: {
                            Label("Remove Game…", systemImage: "trash")
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .font(.system(size: 18, weight: .semibold))
                            .foregroundStyle(.white)
                            .frame(width: 44, height: 44)
                            .contentShape(Circle())
                    }
                    .glass(Circle())
                    .disabled(library.boot != nil)
                }
                .padding(.top, 18)
            }
            if let error = library.bootError {
                Text(error)
                    .font(.system(size: 13))
                    .foregroundStyle(.orange)
                    .lineLimit(3)
                    .padding(.top, 6)
            }
            if let notice = library.notice {
                Text(notice)
                    .font(.system(size: 13))
                    .foregroundStyle(Color(red: 235 / 255, green: 235 / 255, blue: 245 / 255).opacity(0.6))
                    .lineLimit(3)
                    .padding(.top, 6)
            }
        }
    }

    // MARK: the row of covers

    private func tileX(_ i: Int, _ idx: Int) -> CGFloat {
        let small = tileW * tileScale
        if i == idx { return margin }
        if i > idx { return margin + tileW + gap + CGFloat(i - idx - 1) * (small + gap) }
        return margin - CGFloat(idx - i) * (small + gap)
    }

    private func carousel(_ size: CGSize) -> some View {
        let idx = library.focusIndex
        let bottom = size.height - rowBottomInset
        let entries = library.entries
        return ZStack(alignment: .topLeading) {
            ForEach(Array(entries.enumerated()), id: \.element.id) { i, entry in
                let focused = i == idx
                let x = tileX(i, idx)
                if x < size.width + tileW && i >= idx - 2 {
                    CoverTile(title: entry.displayTitle, cover: library.cover(for: entry), radius: radius,
                              focused: focused, ring: focused && pad)
                        .frame(width: tileW, height: tileH)
                        .scaleEffect(focused ? 1 : tileScale, anchor: .bottomLeading)
                        .offset(x: x + (intro == .logo ? 560 : 0), y: bottom - tileH)
                        .opacity(intro == .logo ? 0 : focused ? 1 : i < idx ? (idx - i == 1 ? 0.4 : 0) : 0.8)
                        .zIndex(focused ? 3 : 1)
                        .animation(.timingCurve(0.16, 1, 0.3, 1, duration: 0.32), value: idx)
                        .animation(.timingCurve(0.16, 1, 0.3, 1, duration: intro == .buildIn ? 1 : 0.32)
                                    .delay(intro == .buildIn ? 0.26 + Double(max(0, i - idx)) * 0.07 : 0),
                                   value: intro)
                        .allowsHitTesting(false)
                }
            }
            // The row's touch area: taps and swipes are worked out from the covers' places (the design's pointer
            // handlers on the row).
            Color.clear
                .frame(width: size.width, height: size.height - (bottom - tileH - 10))
                .contentShape(Rectangle())
                .offset(y: bottom - tileH - 10)
                .gesture(rowGesture(bottom: bottom))
        }
    }

    private func rowGesture(bottom: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .named("library"))
            .onChanged { value in
                if intro != .done {
                    skipIntro()
                    return
                }
                guard !booting else { return }
                if drag == nil {
                    drag = RowDrag(startX: value.startLocation.x, startIndex: library.focusIndex,
                                   tile: tile(at: value.startLocation, bottom: bottom))
                }
                guard var d = drag else { return }
                let dx = value.location.x - d.startX
                if !d.moved && abs(dx) > 8 { d.moved = true }
                if d.moved { library.focus(at: d.startIndex - Int((dx / 110).rounded())) }
                drag = d
            }
            .onEnded { _ in
                defer { drag = nil }
                guard let d = drag, !d.moved, let tile = d.tile else { return }
                if tile == library.focusIndex { play() } else { library.focus(at: tile) }
            }
    }

    /// The cover under a point of the row: the focused one first (it is drawn over the others).
    private func tile(at point: CGPoint, bottom: CGFloat) -> Int? {
        let idx = library.focusIndex
        let order = [idx] + library.entries.indices.filter { $0 != idx }
        for i in order {
            let scale = i == idx ? 1 : tileScale
            let rect = CGRect(x: tileX(i, idx), y: bottom - tileH * scale, width: tileW * scale, height: tileH * scale)
            if rect.contains(point) { return i }
        }
        return nil
    }

    // MARK: actions

    private func start() {
        library.inputMode = game.controllerName == nil ? .touch : .pad
        menu.actions = MenuInput.Actions(
            move: { d in
                guard !showSettings, !picking else { return }
                if intro != .done { skipIntro() } else if !booting { library.move(d) }
            },
            play: {
                guard !showSettings, !picking else { return }
                play()
            },
            cancel: {
                if showSettings { showSettings = false } else { library.cancel() }
            },
            add: {
                guard !showSettings, !booting else { return }
                pulse("add")
                addGame()
            },
            settings: {
                if showSettings {
                    showSettings = false
                } else if !booting {
                    pulse("settings")
                    openSettings()
                }
            },
            used: {
                library.inputMode = .pad
                if intro == .logo { skipIntro() }
            })
        game.menuInput = { [menu] gamepad in menu.update(gamepad) }
        guard intro == .logo else { return }
        introStart = Date()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.7) { if intro == .logo { intro = .buildIn } }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.3) { intro = .done }
    }

    /// A tap or any button during the intro: the library builds in at once.
    private func skipIntro() {
        guard intro == .logo else { return }
        intro = .buildIn
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { intro = .done }
    }

    private func touched() {
        if library.inputMode != .touch { library.inputMode = .touch }
    }

    private func play() {
        if intro != .done {
            skipIntro()
            return
        }
        library.play()
    }

    private func addGame() { picking = true }

    private func openSettings() {
        settingsPath = []
        showSettings = true
    }

    private func pulse(_ name: String) {
        flash = name
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.18) { if flash == name { flash = nil } }
    }
}

/// One cover in the row: the cover art filling it, else the title as text, with a hairline edge, its shadow and, for
/// the focused cover while the controller was the last input, the focus ring (a 2 pt dark gap, then white to 5 pt).
struct CoverTile: View {
    let title: String
    let cover: UIImage?
    let radius: CGFloat
    let focused: Bool
    let ring: Bool

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        ZStack {
            shape.fill(Color(red: 26 / 255, green: 26 / 255, blue: 32 / 255).opacity(0.6))
                .background(.ultraThinMaterial, in: shape)
            if let cover {
                Image(uiImage: cover)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                Text(title)
                    .font(.system(size: 19, weight: .bold))
                    .tracking(-0.3)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white.opacity(0.92))
                    .padding(.horizontal, 20)
                    .padding(.vertical, 14)
            }
            shape.strokeBorder(.white.opacity(0.18), lineWidth: 0.5)
        }
        .clipShape(shape)
        .background {
            if ring {
                ZStack {
                    RoundedRectangle(cornerRadius: radius + 5, style: .continuous).fill(.white).padding(-5)
                    RoundedRectangle(cornerRadius: radius + 2, style: .continuous).fill(.black.opacity(0.6)).padding(-2)
                }
            }
        }
        .shadow(color: .black.opacity(focused ? 0.55 : 0.3), radius: focused ? 20 : 10, y: focused ? 18 : 8)
    }
}

/// The controller button glyphs of the hints, on a white disc (black on white; inverted: white on black, as in Play).
struct ButtonBadge: View {
    enum Kind { case cross, circle, square, menu }

    let kind: Kind
    var inverted = false
    var diameter: CGFloat = 22

    var body: some View {
        let ink: Color = inverted ? .white : .black
        Circle()
            .fill(inverted ? Color.black : Color.white)
            .frame(width: diameter, height: diameter)
            .overlay {
                Canvas { context, size in
                    let unit = size.width / 10
                    var path = Path()
                    switch kind {
                    case .cross:
                        path.move(to: CGPoint(x: 1.5 * unit, y: 1.5 * unit))
                        path.addLine(to: CGPoint(x: 8.5 * unit, y: 8.5 * unit))
                        path.move(to: CGPoint(x: 8.5 * unit, y: 1.5 * unit))
                        path.addLine(to: CGPoint(x: 1.5 * unit, y: 8.5 * unit))
                    case .circle:
                        path.addEllipse(in: CGRect(x: 1.2 * unit, y: 1.2 * unit, width: 7.6 * unit, height: 7.6 * unit))
                    case .square:
                        path.addRoundedRect(in: CGRect(x: 1 * unit, y: 1 * unit, width: 8 * unit, height: 8 * unit),
                                            cornerSize: CGSize(width: 0.8 * unit, height: 0.8 * unit))
                    case .menu:
                        for y in [2.0, 5.0, 8.0] {
                            path.move(to: CGPoint(x: 1 * unit, y: y * unit))
                            path.addLine(to: CGPoint(x: 9 * unit, y: y * unit))
                        }
                    }
                    context.stroke(path, with: .color(ink),
                                   style: StrokeStyle(lineWidth: (kind == .menu ? 1.5 : 1.65) * unit, lineCap: .round))
                }
                .frame(width: diameter * 10 / 22, height: diameter * 10 / 22)
            }
    }
}

/// Shrinks a button while it is held.
struct PressScale: ButtonStyle {
    var scale: CGFloat

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? scale : 1)
            .animation(.easeOut(duration: 0.15), value: configuration.isPressed)
    }
}

extension View {
    /// Liquid Glass on iOS 26; before it, the design's frosted capsule (blur, a grey tint, a hairline edge).
    @ViewBuilder
    func glass<S: Shape>(_ shape: S) -> some View {
        if #available(iOS 26.0, *) {
            self.glassEffect(.regular.interactive(), in: shape)
        } else {
            self.background(shape.fill(Color(red: 120 / 255, green: 120 / 255, blue: 128 / 255).opacity(0.28)))
                .background(.ultraThinMaterial, in: shape)
                .overlay(shape.stroke(.white.opacity(0.15), lineWidth: 0.5))
                .shadow(color: .black.opacity(0.35), radius: 3, y: 2)
        }
    }
}
