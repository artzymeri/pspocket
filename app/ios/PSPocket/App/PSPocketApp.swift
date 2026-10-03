import SwiftUI
import UIKit

@main
struct PSPocketApp: App {
    @State private var selfTest = SelfTestModel()
    @State private var metal = MetalLoadModel()
    @State private var device = DeviceStatus()

    init() {
        // Line-buffered stdout, so `devicectl device process launch --console` (and `simctl launch --console-pty`)
        // show each self-test line as it happens.
        setvbuf(stdout, nil, _IOLBF, 0)
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(selfTest)
                .environment(metal)
                .environment(device)
                .onAppear {
                    // A test host: long thermal runs and headless autoruns must not be cut short by auto-lock.
                    UIApplication.shared.isIdleTimerDisabled = true
                }
        }
    }
}

struct RootView: View {
    enum Tab: Hashable { case selfTest, metal }

    @Environment(SelfTestModel.self) private var selfTest
    @State private var tab: Tab = LaunchOptions.initialTab == "metal" ? .metal : .selfTest

    var body: some View {
        TabView(selection: $tab) {
            SelfTestView()
                .tabItem { Label("Self-test", systemImage: "checklist") }
                .tag(Tab.selfTest)
            MetalTabView()
                .tabItem { Label("Metal", systemImage: "cube.transparent") }
                .tag(Tab.metal)
        }
        .task { selfTest.startAutorunIfRequested() }
    }
}

/// Launch arguments (or environment variables). `scripts/ios.sh run --autorun --exit` passes `-autorun -autorun-exit`;
/// anything after `--` is passed through, e.g. `scripts/ios.sh run -- -tab metal -passes 32`.
enum LaunchOptions {
    /// Exit after the run has written its report (implies autorun). `-autorun-exit` or PSPOCKET_AUTORUN_EXIT=1.
    static let exitAfterAutorun = flag("-autorun-exit", env: "PSPOCKET_AUTORUN_EXIT")
    /// Run every probe at launch. `-autorun` or PSPOCKET_AUTORUN=1.
    static let autorun = exitAfterAutorun || flag("-autorun", env: "PSPOCKET_AUTORUN")
    /// Tab shown at launch: `-tab metal` or PSPOCKET_TAB=metal starts the Metal load right away.
    static let initialTab = value("-tab", env: "PSPOCKET_TAB")
    /// Metal load passes at launch (0...64): `-passes N` or PSPOCKET_PASSES=N.
    static let initialPasses = value("-passes", env: "PSPOCKET_PASSES").flatMap { Int($0) }

    private static func flag(_ argument: String, env: String) -> Bool {
        ProcessInfo.processInfo.arguments.contains(argument) || ProcessInfo.processInfo.environment[env] == "1"
    }

    private static func value(_ argument: String, env: String) -> String? {
        let arguments = ProcessInfo.processInfo.arguments
        if let i = arguments.firstIndex(of: argument), i + 1 < arguments.count { return arguments[i + 1] }
        return ProcessInfo.processInfo.environment[env]
    }
}
