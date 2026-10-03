import Foundation
import Observation

/// Static facts about the device and the build, for the report header. Thread-safe (no UIKit).
enum DeviceInfo {
    /// Hardware identifier, e.g. "iPhone18,2". In the simulator, the simulated model plus "(simulator)".
    static let model: String = {
        if let simulated = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] {
            return "\(simulated) (simulator)"
        }
        return sysctlString("hw.machine") ?? "unknown"
    }()

    /// e.g. "iOS 26.6.1 (23G83)".
    static let system: String = {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        let version = v.patchVersion > 0
            ? "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)" : "\(v.majorVersion).\(v.minorVersion)"
        return "iOS \(version) (\(sysctlString("kern.osversion") ?? "?"))"
    }()

    /// e.g. "PSPocket 0.1.0 (1), com.artzymeri.pspocket.testhost".
    static let app: String = {
        let info = Bundle.main.infoDictionary ?? [:]
        let name = info["CFBundleName"] as? String ?? "PSPocket"
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        return "\(name) \(version) (\(build)), \(Bundle.main.bundleIdentifier ?? "?")"
    }()

    static var memoryFootprint: UInt64 { ps_ios_memory_footprint() }
    static var availableMemory: UInt64 { ps_ios_available_memory() }

    private static func sysctlString(_ name: String) -> String? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buffer = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &buffer, &size, nil, 0) == 0 else { return nil }
        return String(cString: buffer)
    }
}

/// Live device state shown in both tabs. Updated on the main thread from ProcessInfo notifications.
@Observable
final class DeviceStatus {
    private(set) var thermalState = ProcessInfo.processInfo.thermalState
    private(set) var lowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
    @ObservationIgnored private var observers: [NSObjectProtocol] = []

    init() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil,
                                             queue: .main) { [weak self] _ in
            self?.thermalState = ProcessInfo.processInfo.thermalState
        })
        observers.append(center.addObserver(forName: .NSProcessInfoPowerStateDidChange, object: nil,
                                             queue: .main) { [weak self] _ in
            self?.lowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
        })
    }
}

extension ProcessInfo.ThermalState {
    var label: String {
        switch self {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        case .critical: "critical"
        @unknown default: "unknown"
        }
    }
}

enum Format {
    static func bytes(_ value: UInt64) -> String {
        let mb = Double(value) / 1_048_576
        return mb >= 1024 ? String(format: "%.2f GB", mb / 1024) : String(format: "%.0f MB", mb)
    }

    static func seconds(_ value: Double) -> String {
        value < 1 ? String(format: "%.0f ms", value * 1000) : String(format: "%.2f s", value)
    }

    static func ms(_ seconds: Double?) -> String {
        guard let seconds else { return "n/a" }
        return String(format: "%.2f ms", seconds * 1000)
    }
}
