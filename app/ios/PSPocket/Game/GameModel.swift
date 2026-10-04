import Foundation
import GameController
import Observation
import QuartzCore
import UIKit

/// The game tab's state: the disc image the user picked in Files (remembered across launches), the files the game
/// needs in the app's Documents, the running game (pspocket/platform/game.h) and its controller: a connected game
/// controller (DualShock 4, DualSense, ...) through GameController, else the on-screen buttons.
///
/// Documents layout (Files > On My iPhone > PSPocket):
///   game/<TITLE_ID>/EBOOT.elf  the game's executable, decrypted on the Mac (`scripts/ios.sh push-game`)
///   dev_flash/                 system files the game loads itself, from the user's firmware (D-067)
///   hdd0/                      the game's /dev_hdd0: game data, saves, trophies
///   game.log                   the last run's log
@Observable
final class GameModel {
    private(set) var isoName: String?
    private(set) var titleID: String?
    private(set) var title: String?
    private(set) var problem: String?
    private(set) var running = false
    private(set) var flips: UInt64 = 0
    private(set) var seconds = 0.0
    private(set) var logTail = ""
    /// The connected game controller's name, or nil (the on-screen buttons show then).
    private(set) var controllerName: String?

    /// The view whose CAMetalLayer shows the game's frames. One instance for the whole run: SwiftUI moves it between
    /// the setup screen and the full-screen game, and the game keeps presenting into its layer.
    @ObservationIgnored let screen = GameLayerUIView()

    @ObservationIgnored private var isoURL: URL?
    @ObservationIgnored private var timer: Timer?
    @ObservationIgnored private var touch1: UInt16 = 0
    @ObservationIgnored private var touch2: UInt16 = 0
    @ObservationIgnored private var controllerPad = ps_game_pad.atRest
    @ObservationIgnored private var observers: [NSObjectProtocol] = []

    private static let bookmarkKey = "gameISOBookmark"

    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }

    init() {
        screen.backgroundColor = .black
        restoreISO()
        watchControllers()
    }

    // MARK: the disc image

    /// A file the user picked in Files: keep access to it (it stays where it is: a disc image is 20 GB) and
    /// remember it for the next launch.
    func pickedISO(_ url: URL) {
        guard url.startAccessingSecurityScopedResource() || FileManager.default.isReadableFile(atPath: url.path) else {
            problem = "Can't open \(url.lastPathComponent)"
            return
        }
        if let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            UserDefaults.standard.set(data, forKey: Self.bookmarkKey)
        }
        useISO(url)
    }

    private func restoreISO() {
        guard let data = UserDefaults.standard.data(forKey: Self.bookmarkKey) else { return }
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
        else { return }
        _ = url.startAccessingSecurityScopedResource()
        if stale, let fresh = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            UserDefaults.standard.set(fresh, forKey: Self.bookmarkKey)
        }
        useISO(url)
    }

    private func useISO(_ url: URL) {
        isoURL = url
        isoName = url.lastPathComponent
        titleID = nil
        title = nil
        problem = nil
        let path = url.path
        DispatchQueue.global(qos: .userInitiated).async {
            var id = [CChar](repeating: 0, count: 32)
            var name = [CChar](repeating: 0, count: 256)
            var error = [CChar](repeating: 0, count: 512)
            let status = ps_game_probe_disc(path, &id, id.count, &name, name.count, &error, error.count)
            let result = (status, String(cString: id), String(cString: name), String(cString: error))
            DispatchQueue.main.async {
                guard self.isoURL == url else { return }
                if result.0 == 0 {
                    self.titleID = result.1
                    self.title = result.2
                    self.problem = self.executableURL == nil
                        ? "The decrypted EBOOT for \(result.1) isn't on this iPhone yet (scripts/ios.sh push-game)" : nil
                } else {
                    self.problem = "Not a PS3 game disc: \(result.3)"
                }
            }
        }
    }

    /// The decrypted executable for the picked disc: Documents/game/<TITLE_ID>/EBOOT.elf, else Documents/game/EBOOT.elf.
    var executableURL: URL? {
        let game = Self.documents.appendingPathComponent("game")
        var candidates: [URL] = []
        if let titleID { candidates.append(game.appendingPathComponent(titleID).appendingPathComponent("EBOOT.elf")) }
        candidates.append(game.appendingPathComponent("EBOOT.elf"))
        return candidates.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    var canBoot: Bool { !running && titleID != nil && executableURL != nil }

    // MARK: running

    func boot() {
        guard canBoot, let isoURL, let elf = executableURL else { return }
        let layer = screen.metalLayer
        let docs = Self.documents
        let flash = docs.appendingPathComponent("dev_flash")
        let hasFlash = FileManager.default.fileExists(atPath: flash.path)
        var error = [CChar](repeating: 0, count: 1024)
        let status = isoURL.path.withCString { iso in
            elf.path.withCString { elfPath in
                docs.appendingPathComponent("hdd0").path.withCString { hdd0 in
                    flash.path.withCString { flashPath in
                        docs.appendingPathComponent("game.log").path.withCString { log in
                            var options = ps_game_options(elf_path: elfPath, iso_path: iso, hdd0_dir: hdd0,
                                flash_dir: hasFlash ? flashPath : nil, log_path: log,
                                metal_layer: Unmanaged.passUnretained(layer).toOpaque())
                            return ps_game_boot(&options, &error, error.count)
                        }
                    }
                }
            }
        }
        if status != 0 {
            problem = String(cString: error)
            return
        }
        problem = hasFlash ? nil : "No dev_flash on this iPhone: the game's audio will fail (scripts/ios.sh push-game)"
        running = true
        OrientationLock.landscape()
        sendPad()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.refresh() }
    }

    private func refresh() {
        var stats = ps_game_stats()
        ps_game_get_stats(&stats)
        running = stats.running != 0
        flips = stats.flips
        seconds = stats.seconds
        var tail = [CChar](repeating: 0, count: 6000)
        ps_game_log_tail(&tail, tail.count)
        logTail = String(cString: tail)
    }

    // MARK: the controller

    /// The on-screen buttons, used when no game controller is connected.
    enum Button: CaseIterable {
        case up, down, left, right, cross, circle, square, triangle, start, select, l1, r1, l2, r2

        /// (button[2] bit, button[3] bit) of CellPadData.
        var bits: (UInt16, UInt16) {
            switch self {
            case .left: (0x80, 0)
            case .down: (0x40, 0)
            case .right: (0x20, 0)
            case .up: (0x10, 0)
            case .start: (0x08, 0)
            case .select: (0x01, 0)
            case .square: (0, 0x80)
            case .cross: (0, 0x40)
            case .circle: (0, 0x20)
            case .triangle: (0, 0x10)
            case .r1: (0, 0x08)
            case .l1: (0, 0x04)
            case .r2: (0, 0x02)
            case .l2: (0, 0x01)
            }
        }
    }

    func set(_ button: Button, down: Bool) {
        let (b1, b2) = button.bits
        if down {
            touch1 |= b1
            touch2 |= b2
        } else {
            touch1 &= ~b1
            touch2 &= ~b2
        }
        sendPad()
    }

    /// Controllers paired in Settings > Bluetooth (DualShock 4, DualSense, Xbox, MFi) come and go here.
    private func watchControllers() {
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.controllersChanged()
            })
        }
        controllersChanged()
    }

    private func controllersChanged() {
        let controller = GCController.controllers().first { $0.extendedGamepad != nil }
        controllerName = controller.map { $0.vendorName ?? "Controller" }
        controllerPad = .atRest
        if let gamepad = controller?.extendedGamepad {
            gamepad.valueChangedHandler = { [weak self] gamepad, _ in self?.controllerMoved(gamepad) }
            controllerMoved(gamepad)
        }
        sendPad()
    }

    /// A PlayStation layout through GameController: A cross, B circle, X square, Y triangle, Menu (Options) start,
    /// Options (Share/Create) and the touchpad select.
    private func controllerMoved(_ g: GCExtendedGamepad) {
        var pad = ps_game_pad.atRest
        func bit(_ pressed: Bool, _ value: UInt16) -> UInt16 { pressed ? value : 0 }
        let touchpad = (g as? GCDualShockGamepad)?.touchpadButton.isPressed == true
            || (g as? GCDualSenseGamepad)?.touchpadButton.isPressed == true
        pad.digital1 = bit(g.dpad.left.isPressed, 0x80) | bit(g.dpad.down.isPressed, 0x40)
            | bit(g.dpad.right.isPressed, 0x20) | bit(g.dpad.up.isPressed, 0x10) | bit(g.buttonMenu.isPressed, 0x08)
            | bit(g.rightThumbstickButton?.isPressed == true, 0x04) | bit(g.leftThumbstickButton?.isPressed == true, 0x02)
            | bit(g.buttonOptions?.isPressed == true || touchpad, 0x01)
        pad.digital2 = bit(g.buttonX.isPressed, 0x80) | bit(g.buttonA.isPressed, 0x40) | bit(g.buttonB.isPressed, 0x20)
            | bit(g.buttonY.isPressed, 0x10) | bit(g.rightShoulder.isPressed, 0x08) | bit(g.leftShoulder.isPressed, 0x04)
            | bit(g.rightTrigger.isPressed, 0x02) | bit(g.leftTrigger.isPressed, 0x01)
        func axis(_ v: Float) -> UInt8 { UInt8(max(0, min(255, (v + 1) * 127.5))) }
        pad.left_x = axis(g.leftThumbstick.xAxis.value)
        pad.left_y = axis(-g.leftThumbstick.yAxis.value) // GameController's y grows upwards, the PS3's downwards
        pad.right_x = axis(g.rightThumbstick.xAxis.value)
        pad.right_y = axis(-g.rightThumbstick.yAxis.value)
        func level(_ b: GCControllerButtonInput) -> UInt8 { UInt8(max(0, min(255, b.value * 255))) }
        let levels = [level(g.dpad.right), level(g.dpad.left), level(g.dpad.up), level(g.dpad.down), level(g.buttonY),
                      level(g.buttonB), level(g.buttonA), level(g.buttonX), level(g.leftShoulder),
                      level(g.rightShoulder), level(g.leftTrigger), level(g.rightTrigger)]
        withUnsafeMutableBytes(of: &pad.pressure) { bytes in
            for (i, v) in levels.enumerated() { bytes[i] = v }
        }
        controllerPad = pad
        sendPad()
    }

    /// The controller's state and the on-screen buttons together.
    private func sendPad() {
        guard running else { return }
        var pad = controllerPad
        pad.digital1 |= touch1
        pad.digital2 |= touch2
        // Full pressure for on-screen presses of the press-sensitive buttons (same order as ps_game_pad.pressure).
        let touched: [(Bool, Int)] = [(touch1 & 0x20 != 0, 0), (touch1 & 0x80 != 0, 1), (touch1 & 0x10 != 0, 2),
            (touch1 & 0x40 != 0, 3), (touch2 & 0x10 != 0, 4), (touch2 & 0x20 != 0, 5), (touch2 & 0x40 != 0, 6),
            (touch2 & 0x80 != 0, 7), (touch2 & 0x04 != 0, 8), (touch2 & 0x08 != 0, 9), (touch2 & 0x01 != 0, 10),
            (touch2 & 0x02 != 0, 11)]
        withUnsafeMutableBytes(of: &pad.pressure) { bytes in
            for (down, i) in touched where down { bytes[i] = 255 }
        }
        ps_game_set_pad(&pad)
    }
}

extension ps_game_pad {
    static var atRest: ps_game_pad {
        var pad = ps_game_pad()
        pad.left_x = 128
        pad.left_y = 128
        pad.right_x = 128
        pad.right_y = 128
        return pad
    }
}

/// The view backed by the CAMetalLayer the game's frames are copied into (1152x640, scaled to fit).
final class GameLayerUIView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        metalLayer.contentsGravity = .resizeAspect
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }
}

/// Which orientations the app allows: everything until a game runs, then landscape only (AppDelegate).
enum OrientationLock {
    static var mask: UIInterfaceOrientationMask = .all

    static func landscape() {
        mask = .landscape
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene else { return }
        scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: .landscape)) { error in
            print("game: landscape: \(error.localizedDescription)")
        }
    }
}
