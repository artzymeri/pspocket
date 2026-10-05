import SwiftUI
import UIKit

/// Keeps the app in landscape: the library and the game (OrientationLock).
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?)
        -> UIInterfaceOrientationMask {
        OrientationLock.mask
    }
}

@main
struct PSPocketApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var selfTest = SelfTestModel()
    @State private var metal = MetalLoadModel()
    @State private var device = DeviceStatus()
    @State private var game = GameModel()
    @State private var library = LibraryModel()

    init() {
        // Line-buffered stdout, so `devicectl device process launch --console` (and `simctl launch --console-pty`)
        // show each self-test line as it happens.
        setvbuf(stdout, nil, _IOLBF, 0)
        DiscExtraction.runIfRequested()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(selfTest)
                .environment(metal)
                .environment(device)
                .environment(game)
                .environment(library)
                .onAppear {
                    // A test host: long thermal runs and headless autoruns must not be cut short by auto-lock.
                    UIApplication.shared.isIdleTimerDisabled = true
                    library.attach(game)
                }
        }
    }
}

/// The game library in landscape (D-142); the running game full screen in its place; the boot overlay over both
/// until the game's first frame. Self-test and Metal are behind Settings (`-tab selftest|metal` and `-autorun` open
/// them at launch, without the intro).
struct RootView: View {
    @Environment(SelfTestModel.self) private var selfTest
    @Environment(GameModel.self) private var game
    @Environment(LibraryModel.self) private var library
    @Environment(\.scenePhase) private var scenePhase
    @State private var settingsPath: [SettingsView.Page] = RootView.launchPage.map { [$0] } ?? []
    @State private var showSettings = RootView.launchPage != nil

    private static var launchPage: SettingsView.Page? {
        if LaunchOptions.initialTab == "metal" { return .metal }
        if LaunchOptions.initialTab == "selftest" || LaunchOptions.autorun { return .selfTest }
        return nil
    }

    var body: some View {
        ZStack {
            if game.running {
                GameScreen()
            } else {
                LibraryView(settingsPath: $settingsPath, showSettings: $showSettings,
                            skipIntro: Self.launchPage != nil)
            }
            if let boot = library.boot {
                let entry = library.entries.first { $0.id == boot.entryID }
                BootOverlay(cover: library.cover(for: entry), title: entry?.displayTitle ?? "",
                            cancellable: boot.phase != .started, pad: library.inputMode == .pad,
                            preparation: boot.phase == .preparing ? library.preparer.snapshot : nil,
                            timeLeft: library.preparer.timeLeft, cancel: { library.cancel() })
                    .transition(.opacity)
            }
        }
        .animation(.easeInOut(duration: 0.36), value: library.boot != nil)
        .sheet(isPresented: $showSettings) { SettingsView(path: $settingsPath) }
        .onChange(of: game.flips) { if game.flips > 0 { library.gameShowedFrames() } }
        .onChange(of: scenePhase) {
            switch scenePhase {
            case .active: library.scheduleRescan()
            case .background: library.save()
            default: break
            }
        }
        .task { selfTest.startAutorunIfRequested() }
        .onAppear { OrientationLock.landscape() }
    }
}

/// Launch arguments (or environment variables). `scripts/ios.sh run --autorun --exit` passes `-autorun -autorun-exit`;
/// anything after `--` is passed through, e.g. `scripts/ios.sh run -- -tab metal -passes 32`.
enum LaunchOptions {
    /// Exit after the run has written its report (implies autorun). `-autorun-exit` or PSPOCKET_AUTORUN_EXIT=1.
    static let exitAfterAutorun = flag("-autorun-exit", env: "PSPOCKET_AUTORUN_EXIT")
    /// Run every probe at launch. `-autorun` or PSPOCKET_AUTORUN=1.
    static let autorun = exitAfterAutorun || flag("-autorun", env: "PSPOCKET_AUTORUN")
    /// A page shown at launch over the library (Settings): `-tab metal` or PSPOCKET_TAB=metal starts the Metal load
    /// right away, `-tab selftest` shows the probes.
    static let initialTab = value("-tab", env: "PSPOCKET_TAB")
    /// Metal load passes at launch (0...64): `-passes N` or PSPOCKET_PASSES=N.
    static let initialPasses = value("-passes", env: "PSPOCKET_PASSES").flatMap { Int($0) }
    /// Extract the executables from the disc image in Documents, then exit (DiscExtraction). `-extract-iso`.
    static let extractISO = flag("-extract-iso", env: "PSPOCKET_EXTRACT_ISO")
    /// Which image in Documents to use with `-extract-iso` (default: the first .iso by name). `-iso NAME`.
    static let isoName = value("-iso", env: "PSPOCKET_ISO")

    private static func flag(_ argument: String, env: String) -> Bool {
        ProcessInfo.processInfo.arguments.contains(argument) || ProcessInfo.processInfo.environment[env] == "1"
    }

    private static func value(_ argument: String, env: String) -> String? {
        let arguments = ProcessInfo.processInfo.arguments
        if let i = arguments.firstIndex(of: argument), i + 1 < arguments.count { return arguments[i + 1] }
        return ProcessInfo.processInfo.environment[env]
    }
}
