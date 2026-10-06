import Foundation
import Observation

/// Preparing a game on this iPhone (route B, D-154): its code translated and compiled into the game's library, signed
/// with the user's development identity, once, the first time it is played. Runs on the runtime's threads
/// (pspocket/platform/prepare.h); this polls it twice a second for the overlay.
@Observable
final class Preparer {
    struct Snapshot: Equatable {
        var phase: Int32
        var fraction: Double
        var filesDone: Int
        var filesTotal: Int
        var detail: String
    }

    private(set) var snapshot: Snapshot?
    /// "About 7 minutes left": the runtime's estimate, smoothed so it counts down steadily and moves to a new
    /// estimate over a few seconds instead of jumping (D-153).
    private(set) var timeLeft = "Working out how long it takes…"

    @ObservationIgnored private var timer: Timer?
    @ObservationIgnored private var smoothedEnd: Date?
    @ObservationIgnored private var finish: ((Result<URL, PreparationError>) -> Void)?

    struct PreparationError: Error {
        let message: String
    }

    /// Whether the game has a library this app can run (and if not, why: never prepared, another pspocket, another
    /// executable).
    static func check(gameDir: URL, executable: URL) -> (ready: Bool, why: String) {
        var why = [CChar](repeating: 0, count: 256)
        let ready = ps_prepare_check(gameDir.path, executable.path, &why, why.count) != 0
        return (ready, String(cString: why))
    }

    /// Whether the PS3 system software's own modules are translated for the system software installed now (D-178):
    /// done once on this iPhone, shared by every game.
    static func checkSystem() -> (ready: Bool, why: String) {
        var why = [CChar](repeating: 0, count: 256)
        let ready = ps_prepare_system_check(Firmware.devFlash.path, Firmware.systemLibrary.path, &why, why.count) != 0
        return (ready, String(cString: why))
    }

    /// Starts preparing; `finish` runs on the main thread with the library, or why it failed. Nil, or why it could
    /// not start.
    func start(executable: URL, titleID: String, gameDir: URL,
               finish: @escaping (Result<URL, PreparationError>) -> Void) -> String? {
        var error = [CChar](repeating: 0, count: 512)
        guard ps_prepare_start(executable.path, titleID, gameDir.path, &error, error.count) == 0 else {
            return String(cString: error)
        }
        begin(finish)
        return nil
    }

    /// Starts translating the PS3 system software's modules (D-178), as `start` does a game.
    func startSystem(finish: @escaping (Result<URL, PreparationError>) -> Void) -> String? {
        var error = [CChar](repeating: 0, count: 512)
        guard ps_prepare_system_start(Firmware.devFlash.path, Firmware.systemLibrary.path, &error, error.count) == 0
        else { return String(cString: error) }
        begin(finish)
        return nil
    }

    private func begin(_ finish: @escaping (Result<URL, PreparationError>) -> Void) {
        self.finish = finish
        smoothedEnd = nil
        timeLeft = "Working out how long it takes…"
        snapshot = Snapshot(phase: Int32(PS_PREPARE_TRANSLATING), fraction: 0, filesDone: 0, filesTotal: 0,
                            detail: "Starting")
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.poll() }
    }

    /// Stops at the next file; what is compiled is kept for the next start.
    func cancel() {
        ps_prepare_cancel()
        timer?.invalidate()
        timer = nil
        finish = nil
        snapshot = nil
    }

    private func poll() {
        var s = ps_prepare_status()
        ps_prepare_get_status(&s)
        let detail = withUnsafeBytes(of: s.detail) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        snapshot = Snapshot(phase: s.phase, fraction: s.fraction, filesDone: Int(s.files_done),
                            filesTotal: Int(s.files_total), detail: detail)
        if s.seconds_left >= 0 { smooth(s.seconds_left) }
        switch s.phase {
        case Int32(PS_PREPARE_DONE):
            let path = withUnsafeBytes(of: s.library) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            end(.success(URL(fileURLWithPath: path)))
        case Int32(PS_PREPARE_FAILED):
            let message = withUnsafeBytes(of: s.error) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            end(.failure(PreparationError(message: message)))
        default:
            break
        }
    }

    private func end(_ result: Result<URL, PreparationError>) {
        timer?.invalidate()
        timer = nil
        let finish = self.finish
        self.finish = nil
        finish?(result)
    }

    /// The end the runtime predicts, followed with a time constant of about six seconds, shown rounded the way a
    /// person says it.
    private func smooth(_ secondsLeft: Double) {
        let now = Date()
        let predicted = now.addingTimeInterval(secondsLeft)
        if let end = smoothedEnd {
            smoothedEnd = end.addingTimeInterval(0.08 * predicted.timeIntervalSince(end))
        } else {
            smoothedEnd = predicted
        }
        let left = max(0, smoothedEnd!.timeIntervalSince(now))
        switch left {
        case 90...:
            let minutes = Int((left / 60).rounded())
            timeLeft = "About \(minutes) minutes left"
        case 50..<90: timeLeft = "About a minute left"
        case 10..<50: timeLeft = "Less than a minute left"
        default: timeLeft = "Almost done"
        }
    }
}
