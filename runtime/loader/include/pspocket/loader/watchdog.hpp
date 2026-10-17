#pragma once

// The run watchdog (D-1043): a game that stops making progress is said so, with what every thread waits on, instead
// of running on to its timeout or being killed with nothing logged. Shared by pspocket-boot on the Mac and the iPhone
// app (which had the freeze check alone, D-135). Three checks, each polled every half second:
// - freeze: frames came, then none for `freeze_s` (10 s);
// - deadlock: every PPU thread (not an interrupt thread, not an attached host thread) blocked in lv2 without a
//   timeout, on the same wait for `deadlock_s` (5 s) or more, while the SPU threads and the RSX thread used next to
//   no CPU (under 5% of a core each kind) over that time: nothing is left that could wake them. It does not wait for
//   the freeze's 10 s, and it catches a game stuck before its first frame;
// - livelock: frames keep coming but show the same picture for `livelock_s` (off unless set), from the frames' hashes
//   the run gives it (note_frame: the Metal backend's captures); how often the controller changed meanwhile is said
//   too (a game that ignores the presses of a scripted walk).
// Each check reports once per stall (again after progress). The decision is StallJudge's, testable without threads.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::loader {

enum class Stall : u8 { None, Freeze, Deadlock, Livelock };
const char* stall_name(Stall s);

struct WatchdogConfig {
    double freeze_s = 10;   // 0: off
    double first_frame_s = 60; // no first frame this long after the start (L.A. Noire's "Starting", D-1093); 0: off
    double deadlock_s = 5;  // 0: off
    double livelock_s = 0;  // 0: off
    double poll_s = 0.5;
    double idle_cpu = 0.05; // a core's share under which the SPU threads (together) and the RSX count as idle
};

// One poll's view of the process.
struct WatchSample {
    double t = 0;             // seconds since the watch began
    u64 flips = 0;
    usize ppu_threads = 0;    // PPU threads that count (started, not exited, not interrupt or attached)
    usize ppu_stuck = 0;      // of them, blocked without a timeout
    double ppu_stuck_for = 0; // the shortest time one of those has been blocked, seconds
    u64 spu_cpu_ns = 0;       // the SPU threads' CPU time so far (all of them)
    u64 rsx_cpu_ns = 0;
    u64 frame_hash = 0;       // the last captured frame's hash; 0: none captured
    u64 frames_hashed = 0;    // how many were
    u64 pad_changes = 0;      // controller changes so far
};

struct StallReport {
    Stall kind = Stall::None;
    double at = 0; // seconds since the watch began
    u64 flip = 0;
    std::string title;  // one line
    std::string report; // the thread report (lv2::format_report), for freezes and deadlocks
};

// The checks over successive samples.
class StallJudge {
public:
    explicit StallJudge(WatchdogConfig config) : config_(config) {}
    // A stall that began with this sample (once per stall), and its title.
    std::optional<std::pair<Stall, std::string>> observe(const WatchSample& s);

private:
    WatchdogConfig config_;
    std::vector<WatchSample> history_; // the last samples, enough for the longest check
    u64 seen_flips_ = 0;
    double flips_since_ = 0;
    bool freeze_reported_ = false, deadlock_reported_ = false, livelock_reported_ = false, first_frame_reported_ = false;
    u64 same_hash_ = 0;
    double same_since_ = -1;
    u64 same_from_pad_ = 0, same_flips_ = 0, same_hashed_ = 0;
};

class Watchdog {
public:
    // `on_stall` runs on the watchdog's thread. `describe_address` names guest code addresses in the report.
    Watchdog(lv2::Kernel& kernel, WatchdogConfig config, std::function<void(const StallReport&)> on_stall,
        std::function<std::string(u32)> describe_address = {});
    ~Watchdog();
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    void start();
    void stop();
    // From the frame captures: the hash of a frame's thumbnail.
    void note_frame(u64 flip, u64 hash);
    void note_pad_change();
    // Runs on the watchdog's thread at every poll (the app saves its PPU landings from it).
    void set_tick(std::function<void()> tick);
    std::vector<StallReport> reports() const;
    // The sample a poll takes now (also for tests).
    WatchSample sample();

private:
    void run();

    lv2::Kernel& kernel_;
    WatchdogConfig config_;
    std::function<void(const StallReport&)> on_stall_;
    std::function<std::string(u32)> describe_;
    std::function<void()> tick_;
    StallJudge judge_;
    mutable std::mutex mutex_;
    std::vector<StallReport> reports_;
    u64 frame_hash_ = 0, frames_hashed_ = 0, pad_changes_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::thread thread_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stopping_ = false;
};

// A frame's hash for the livelock check: a 16x9 thumbnail of a BGRA image, each channel's top 4 bits (a picture that
// only flickers in its lowest bits counts as the same).
u64 frame_thumbnail_hash(const u8* bgra, u32 width, u32 height);

} // namespace pspocket::loader
