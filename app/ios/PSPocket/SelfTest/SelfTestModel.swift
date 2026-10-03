import Foundation
import Observation

/// One probe as the UI shows it.
struct ProbeResult: Identifiable {
    enum Status { case idle, queued, running, passed, failed, skipped }

    let index: Int32
    let name: String
    let description: String
    var status = Status.idle
    var summary = ""
    var seconds: Double?
    var metrics: [String] = []
    /// Info lines and runtime log lines, in the order they arrived.
    var details: [String] = []

    var id: Int32 { index }
}

struct RunSummary {
    var passed = 0
    var failed = 0
    var skipped = 0
    var seconds = 0.0

    var text: String { String(format: "%d passed, %d failed, %d skipped (%.2f s)", passed, failed, skipped, seconds) }
}

/// An event from the probe thread, delivered to the model on the main thread.
enum ProbeEvent {
    case begin(probe: String)
    case info(probe: String, text: String)
    case metric(probe: String, text: String)
    case log(probe: String, text: String)
    case end(probe: String, status: ProbeResult.Status, summary: String, seconds: Double)

    var probe: String {
        switch self {
        case .begin(let probe), .info(let probe, _), .metric(let probe, _), .log(let probe, _),
             .end(let probe, _, _, _): probe
        }
    }
}

/// Drives the C self-test API (pspocket/selftest.h). Main-thread state for SwiftUI; the probes run on their own thread.
@Observable
final class SelfTestModel {
    private(set) var probes: [ProbeResult]
    private(set) var isRunning = false
    private(set) var lastRun: RunSummary?
    /// Plain-text report of the last run (also in Documents/selftest-report.txt).
    private(set) var report = ""
    private(set) var reportStatus = ""
    let buildInfo = String(cString: ps_selftest_build_info())

    @ObservationIgnored private let runner = SelfTestRunner()
    @ObservationIgnored private var autorunStarted = false

    static let reportURL = URL.documentsDirectory.appending(path: "selftest-report.txt")

    init() {
        probes = (0..<ps_selftest_probe_count()).map { index in
            ProbeResult(index: index, name: String(cString: ps_selftest_probe_name(index)),
                        description: String(cString: ps_selftest_probe_description(index)))
        }
        runner.setDelivery { [weak self] event in self?.apply(event) }
        // Runtime log lines (pspocket::log) go to os_log and, through the runner, to stdout and the report.
        ps_ios_install_log_sink(runtimeLogCallback, Unmanaged.passUnretained(runner).toOpaque())
    }

    func runAll() { start(only: nil, exitWhenDone: false) }

    func run(_ probe: ProbeResult) { start(only: probe.index, exitWhenDone: false) }

    /// `-autorun` / `-autorun-exit` (LaunchOptions). Runs once per launch.
    func startAutorunIfRequested() {
        guard LaunchOptions.autorun, !autorunStarted else { return }
        autorunStarted = true
        runner.output("pspocket: autorun\(LaunchOptions.exitAfterAutorun ? ", exit when done" : "")")
        start(only: nil, exitWhenDone: LaunchOptions.exitAfterAutorun)
    }

    private func start(only index: Int32?, exitWhenDone: Bool) {
        guard !isRunning else { return }
        isRunning = true
        for i in probes.indices where index == nil || probes[i].index == index {
            probes[i].status = .queued
            probes[i].summary = ""
            probes[i].seconds = nil
            probes[i].metrics = []
            probes[i].details = []
        }
        let runner = runner
        let thread = Thread { [weak self] in
            let (summary, report) = runner.run(only: index)
            DispatchQueue.main.async { self?.finish(summary: summary, report: report, exitWhenDone: exitWhenDone) }
        }
        thread.name = "pspocket.selftest"
        thread.qualityOfService = .userInitiated
        // Same as the main thread the macOS CLI runs the probes on; lifted code nests calls as deep as the guest does.
        thread.stackSize = 8 << 20
        thread.start()
    }

    private func finish(summary: RunSummary, report: String, exitWhenDone: Bool) {
        isRunning = false
        lastRun = summary
        self.report = report
        do {
            try report.write(to: Self.reportURL, atomically: true, encoding: .utf8)
            reportStatus = "Saved as selftest-report.txt in the Files app, under PSPocket"
            runner.output("report: \(Self.reportURL.path)")
        } catch {
            reportStatus = "Could not save the report: \(error.localizedDescription)"
            runner.output("report: write failed: \(error)")
        }
        if exitWhenDone {
            runner.output("pspocket: exiting (\(summary.failed == 0 ? "all passed" : "\(summary.failed) failed"))")
            fflush(stdout)
            exit(summary.failed == 0 ? 0 : 1)
        }
    }

    private func apply(_ event: ProbeEvent) {
        guard let i = probes.firstIndex(where: { $0.name == event.probe }) else { return }
        switch event {
        case .begin:
            probes[i].status = .running
        case .info(_, let text), .log(_, let text):
            probes[i].details.append(text)
        case .metric(_, let text):
            probes[i].metrics.append(text)
        case .end(_, let status, let summary, let seconds):
            probes[i].status = status
            probes[i].summary = summary
            probes[i].seconds = seconds
        }
    }
}

/// Runs the probes and turns their events into report lines. Called from the probe thread and from whichever thread
/// the runtime logs on, so everything mutable sits behind `lock`.
final class SelfTestRunner: @unchecked Sendable {
    private let lock = NSLock()
    private var deliver: ((ProbeEvent) -> Void)?
    private var lines: [String] = []
    private var recording = false
    private var currentProbe: String?
    private var probeStart: UInt64 = 0

    func setDelivery(_ deliver: @escaping (ProbeEvent) -> Void) {
        lock.withLock { self.deliver = deliver }
    }

    /// Runs one probe (`only`) or all of them on the calling thread. Returns the summary and the full report.
    func run(only index: Int32?) -> (RunSummary, String) {
        lock.withLock {
            lines.removeAll()
            recording = true
        }
        for line in Self.header(only: index) { output(line) }

        let user = Unmanaged.passUnretained(self).toOpaque()
        let result = withExtendedLifetime(self) {
            index.map { ps_selftest_run_one($0, selfTestEventCallback, user) }
                ?? ps_selftest_run_all(selfTestEventCallback, user)
        }
        let summary = RunSummary(passed: Int(result.passed), failed: Int(result.failed), skipped: Int(result.skipped),
                                 seconds: result.seconds)
        output("")
        output(summary.text)
        output("end:     thermal \(ProcessInfo.processInfo.thermalState.label), "
               + "low power mode \(ProcessInfo.processInfo.isLowPowerModeEnabled ? "on" : "off"), "
               + "footprint \(Format.bytes(DeviceInfo.memoryFootprint))")
        let report = lock.withLock {
            recording = false
            return lines.joined(separator: "\n") + "\n"
        }
        return (summary, report)
    }

    /// Prints a line to stdout (devicectl --console shows it) and, during a run, appends it to the report.
    func output(_ line: String) {
        lock.withLock {
            print(line)
            if recording { lines.append(line) }
        }
    }

    fileprivate func selfTestEvent(_ event: ps_selftest_event, probe: String, message: String) {
        switch event {
        case PS_SELFTEST_BEGIN:
            lock.withLock {
                currentProbe = probe
                probeStart = DispatchTime.now().uptimeNanoseconds
            }
            output("")
            output("== \(probe): \(message)")
            post(.begin(probe: probe))
        case PS_SELFTEST_INFO:
            output("   \(message)")
            post(.info(probe: probe, text: message))
        case PS_SELFTEST_METRIC:
            output("   * \(message)")
            post(.metric(probe: probe, text: message))
        case PS_SELFTEST_PASS, PS_SELFTEST_FAIL, PS_SELFTEST_SKIP:
            let seconds = lock.withLock {
                currentProbe = nil
                return Double(DispatchTime.now().uptimeNanoseconds - probeStart) / 1e9
            }
            let (tag, status): (String, ProbeResult.Status) = switch event {
            case PS_SELFTEST_PASS: ("PASS", .passed)
            case PS_SELFTEST_FAIL: ("FAIL", .failed)
            default: ("SKIP", .skipped)
            }
            output("   \(tag)  \(message)  [\(Format.seconds(seconds))]")
            post(.end(probe: probe, status: status, summary: message, seconds: seconds))
        default:
            output("   (event \(event.rawValue)) \(message)")
        }
    }

    fileprivate func runtimeLog(_ level: ps_ios_log_level, channel: String, message: String) {
        let text = "[\(Self.levelName(level))] \(channel): \(message)"
        let probe = lock.withLock { currentProbe }
        output(probe == nil ? text : "   \(text)")
        if let probe { post(.log(probe: probe, text: text)) }
    }

    private func post(_ event: ProbeEvent) {
        guard let deliver = lock.withLock({ deliver }) else { return }
        DispatchQueue.main.async { deliver(event) }
    }

    private static func header(only index: Int32?) -> [String] {
        let probes = index.map { String(cString: ps_selftest_probe_name($0)) } ?? "all (\(ps_selftest_probe_count()))"
        return [
            "PSPocket self-test report",
            "build:   \(String(cString: ps_selftest_build_info()))",
            "app:     \(DeviceInfo.app)",
            "device:  \(DeviceInfo.model), \(DeviceInfo.system)",
            "date:    \(Date().ISO8601Format())",
            "start:   thermal \(ProcessInfo.processInfo.thermalState.label), "
                + "low power mode \(ProcessInfo.processInfo.isLowPowerModeEnabled ? "on" : "off"), "
                + "footprint \(Format.bytes(DeviceInfo.memoryFootprint)), "
                + "available \(DeviceInfo.availableMemory == 0 ? "n/a" : Format.bytes(DeviceInfo.availableMemory))",
            "probes:  \(probes)",
        ]
    }

    private static func levelName(_ level: ps_ios_log_level) -> String {
        switch level {
        case PS_IOS_LOG_TRACE: "trace"
        case PS_IOS_LOG_DEBUG: "debug"
        case PS_IOS_LOG_INFO: "info"
        case PS_IOS_LOG_WARN: "warn"
        case PS_IOS_LOG_ERROR: "error"
        case PS_IOS_LOG_FATAL: "fatal"
        default: "level \(level.rawValue)"
        }
    }
}

// C callbacks: `user` is the SelfTestRunner, passed unretained (the model owns it for the app's lifetime).

private let selfTestEventCallback: ps_selftest_callback = { user, event, probe, message in
    guard let user else { return }
    Unmanaged<SelfTestRunner>.fromOpaque(user).takeUnretainedValue()
        .selfTestEvent(event, probe: probe.map { String(cString: $0) } ?? "?",
                       message: message.map { String(cString: $0) } ?? "")
}

private let runtimeLogCallback: ps_ios_log_callback = { user, level, channel, message in
    guard let user else { return }
    Unmanaged<SelfTestRunner>.fromOpaque(user).takeUnretainedValue()
        .runtimeLog(level, channel: channel.map { String(cString: $0) } ?? "?",
                    message: message.map { String(cString: $0) } ?? "")
}
