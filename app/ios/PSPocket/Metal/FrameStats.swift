import Foundation
import QuartzCore

/// Frame timing for the Metal tab. Written from the display link (main thread), Metal completion handlers and
/// drawable presented handlers; read on the main thread for the overlay.
final class FrameStats: @unchecked Sendable {
    struct Snapshot {
        /// Seconds. Averages over the last second, maxima over the last 5 s.
        var cpuAvg: Double?
        var cpuMax: Double?
        var gpuAvg: Double?
        var gpuMax: Double?
        var intervalAvg: Double?
        var intervalWorst: Double?
        /// "presented" (drawable presentedTime) or "display link" (callback timestamps, when presentation times are
        /// unavailable, e.g. in the simulator).
        var intervalSource = "presented"
        var dropped = 0
        var frames = 0
        var gpuBusySkips = 0
        var elapsed = 0.0
        var footprint: UInt64 = 0
    }

    static let window = 5.0
    let period: Double

    private let lock = NSLock()
    private var cpu = Series()
    private var gpu = Series()
    private var presentTimes = IntervalTracker()
    private var linkTimes = IntervalTracker()
    private var frames = 0
    private var gpuBusySkips = 0
    private var start = CACurrentMediaTime()

    init(targetHz: Double) {
        period = 1.0 / targetHz
    }

    func cpuFrame(_ seconds: Double) {
        let now = CACurrentMediaTime()
        lock.withLock {
            cpu.add(seconds, at: now)
            frames += 1
        }
    }

    func gpuFrame(start: CFTimeInterval, end: CFTimeInterval) {
        guard start > 0, end > start else { return } // not reported on every platform
        let now = CACurrentMediaTime()
        lock.withLock { gpu.add(end - start, at: now) }
    }

    func presented(at time: CFTimeInterval) {
        guard time > 0 else { return } // 0: the drawable was never shown
        lock.withLock { presentTimes.add(time, period: period) }
    }

    func linkTick(at time: CFTimeInterval) {
        lock.withLock { linkTimes.add(time, period: period) }
    }

    func gpuBusySkip() {
        lock.withLock { gpuBusySkips += 1 }
    }

    /// Call after rendering was paused, so the gap does not count as dropped frames.
    func resumeTiming() {
        lock.withLock {
            presentTimes.last = 0
            linkTimes.last = 0
        }
    }

    func reset() {
        lock.withLock {
            cpu = Series()
            gpu = Series()
            presentTimes = IntervalTracker()
            linkTimes = IntervalTracker()
            frames = 0
            gpuBusySkips = 0
            start = CACurrentMediaTime()
        }
    }

    func snapshot() -> Snapshot {
        let now = CACurrentMediaTime()
        return lock.withLock {
            // Presentation times are the real on-glass cadence; fall back to the display link when there are none.
            let usePresented = presentTimes.series.newest.map { now - $0 < 1.0 } ?? false
            let intervals = usePresented ? presentTimes : linkTimes
            return Snapshot(
                cpuAvg: cpu.average(since: now - 1), cpuMax: cpu.maximum(since: now - Self.window),
                gpuAvg: gpu.average(since: now - 1), gpuMax: gpu.maximum(since: now - Self.window),
                intervalAvg: intervals.series.average(since: now - 1),
                intervalWorst: intervals.series.maximum(since: now - Self.window),
                intervalSource: usePresented ? "presented" : "display link",
                dropped: intervals.dropped, frames: frames, gpuBusySkips: gpuBusySkips, elapsed: now - start,
                footprint: ps_ios_memory_footprint())
        }
    }

    /// Timestamped samples, pruned to the last `window` seconds.
    private struct Series {
        private var samples: [(time: Double, value: Double)] = []

        var newest: Double? { samples.last?.time }

        mutating func add(_ value: Double, at time: Double) {
            samples.append((time, value))
            if let first = samples.firstIndex(where: { $0.time >= time - FrameStats.window }), first > 0 {
                samples.removeFirst(first)
            }
        }

        func average(since: Double) -> Double? {
            let recent = samples.lazy.filter { $0.time >= since }.map(\.value)
            let count = recent.count
            return count == 0 ? nil : recent.reduce(0, +) / Double(count)
        }

        func maximum(since: Double) -> Double? {
            samples.lazy.filter { $0.time >= since }.map(\.value).max()
        }
    }

    /// Intervals between consecutive timestamps; an interval of k periods counts k - 1 dropped frames.
    private struct IntervalTracker {
        var last = 0.0
        var series = Series()
        var dropped = 0

        mutating func add(_ time: Double, period: Double) {
            defer { last = time }
            guard last > 0 else { return }
            let interval = time - last
            guard interval > 0, interval < 1.0 else { return } // a pause, not a frame
            series.add(interval, at: time)
            if interval > period * 1.5 { dropped += Int((interval / period).rounded()) - 1 }
        }
    }
}
