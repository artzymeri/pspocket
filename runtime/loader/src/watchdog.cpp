// The run watchdog (watchdog.hpp, D-1043).

#include "pspocket/loader/watchdog.hpp"

#include <algorithm>
#include <format>

#include "pspocket/core/thread_report.hpp"
#include "pspocket/lv2/report.hpp"
#include "pspocket/rsx/rsx.hpp"

namespace pspocket::loader {

const char* stall_name(Stall s)
{
    switch (s) {
    case Stall::None: return "none";
    case Stall::Freeze: return "freeze";
    case Stall::Deadlock: return "deadlock";
    case Stall::Livelock: return "livelock";
    }
    return "?";
}

std::optional<std::pair<Stall, std::string>> StallJudge::observe(const WatchSample& s)
{
    history_.push_back(s);
    const double keep = std::max({config_.freeze_s, config_.deadlock_s, config_.livelock_s}) + 2;
    while (history_.size() > 2 && history_[1].t <= s.t - keep) history_.erase(history_.begin());
    std::optional<std::pair<Stall, std::string>> out;

    // Freeze: frames came, then none.
    if (s.flips != seen_flips_) {
        seen_flips_ = s.flips;
        flips_since_ = s.t;
        freeze_reported_ = false;
    } else if (config_.freeze_s > 0 && s.flips && !freeze_reported_ && s.t - flips_since_ >= config_.freeze_s) {
        freeze_reported_ = true;
        out = {Stall::Freeze, std::format("freeze: no frame for {:.0f} s after frame {}", s.t - flips_since_, s.flips)};
    }

    // Deadlock: every PPU thread blocked for good, nothing left running that could wake one.
    const bool stuck = s.ppu_threads && s.ppu_stuck == s.ppu_threads && s.ppu_stuck_for >= config_.deadlock_s;
    if (!stuck) {
        deadlock_reported_ = false;
    } else if (config_.deadlock_s > 0 && !deadlock_reported_ && !out) {
        // The sample from deadlock_s ago (or the oldest kept, when the watch is younger).
        const WatchSample* then = &history_.front();
        for (const WatchSample& h : history_) {
            if (h.t <= s.t - config_.deadlock_s) then = &h;
        }
        const double dt = s.t - then->t;
        if (dt >= config_.deadlock_s * 0.9) {
            const double spu = static_cast<double>(s.spu_cpu_ns - std::min(s.spu_cpu_ns, then->spu_cpu_ns)) / 1e9 / dt;
            const double rsx = static_cast<double>(s.rsx_cpu_ns - std::min(s.rsx_cpu_ns, then->rsx_cpu_ns)) / 1e9 / dt;
            if (spu < config_.idle_cpu && rsx < config_.idle_cpu && s.flips == then->flips) {
                deadlock_reported_ = true;
                out = {Stall::Deadlock,
                    std::format("deadlock: all {} PPU threads blocked without a timeout for {:.0f} s or more, the SPU "
                                "threads ({:.1f}% of a core) and the RSX ({:.1f}%) idle, at frame {}",
                        s.ppu_threads, s.ppu_stuck_for, spu * 100, rsx * 100, s.flips)};
            }
        }
    }

    // Livelock: frames flip, the picture stays.
    if (config_.livelock_s > 0 && s.frames_hashed) {
        if (s.frame_hash != same_hash_ || same_since_ < 0) {
            same_hash_ = s.frame_hash;
            same_since_ = s.t;
            same_from_pad_ = s.pad_changes;
            same_flips_ = s.flips;
            same_hashed_ = s.frames_hashed;
            livelock_reported_ = false;
        } else if (!livelock_reported_ && !out && s.t - same_since_ >= config_.livelock_s && s.flips > same_flips_ &&
                   s.frames_hashed >= same_hashed_ + 2) {
            livelock_reported_ = true;
            out = {Stall::Livelock,
                std::format("livelock: frames {} to {} show the same picture for {:.0f} s ({} compared); the controller "
                            "changed {} times meanwhile",
                    same_flips_, s.flips, s.t - same_since_, s.frames_hashed - same_hashed_ + 1,
                    s.pad_changes - same_from_pad_)};
        }
    }
    return out;
}

Watchdog::Watchdog(lv2::Kernel& kernel, WatchdogConfig config, std::function<void(const StallReport&)> on_stall,
    std::function<std::string(u32)> describe_address)
    : kernel_(kernel), config_(config), on_stall_(std::move(on_stall)), describe_(std::move(describe_address)),
      judge_(config)
{
}

Watchdog::~Watchdog() { stop(); }

void Watchdog::start()
{
    if (thread_.joinable()) return;
    start_ = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(stop_mutex_);
        stopping_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void Watchdog::stop()
{
    {
        std::lock_guard lock(stop_mutex_);
        stopping_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Watchdog::note_frame(u64, u64 hash)
{
    std::lock_guard lock(mutex_);
    frame_hash_ = hash;
    frames_hashed_++;
}

void Watchdog::note_pad_change()
{
    std::lock_guard lock(mutex_);
    pad_changes_++;
}

void Watchdog::set_tick(std::function<void()> tick)
{
    std::lock_guard lock(mutex_);
    tick_ = std::move(tick);
}

std::vector<StallReport> Watchdog::reports() const
{
    std::lock_guard lock(mutex_);
    return reports_;
}

WatchSample Watchdog::sample()
{
    WatchSample s;
    s.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    if (rsx::Rsx* r = kernel_.rsx()) s.flips = r->flip_count();
    double stuck_for = -1;
    for (const lv2::ThreadSnapshot& t : lv2::snapshot_threads(kernel_, 0)) {
        using State = lv2::ThreadSnapshot::State;
        if (t.attached || t.interrupt || t.state == State::NotStarted || t.state == State::Exited) continue;
        s.ppu_threads++;
        if (t.state == State::Blocked && t.remaining.count() < 0 && t.wait.kind != lv2::WaitKind::Sleep) {
            s.ppu_stuck++;
            const double w = std::chrono::duration<double>(t.waited).count();
            stuck_for = stuck_for < 0 ? w : std::min(stuck_for, w);
        }
    }
    s.ppu_stuck_for = std::max(stuck_for, 0.0);
    for (const ThreadCpu& c : thread_cpu_times()) {
        if (c.name.find("SPU") != std::string::npos) s.spu_cpu_ns += c.ns;
        else if (c.name == "pspocket.rsx") s.rsx_cpu_ns += c.ns;
    }
    std::lock_guard lock(mutex_);
    s.frame_hash = frame_hash_;
    s.frames_hashed = frames_hashed_;
    s.pad_changes = pad_changes_;
    return s;
}

void Watchdog::run()
{
    const auto poll = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(config_.poll_s));
    for (;;) {
        {
            std::unique_lock lock(stop_mutex_);
            if (stop_cv_.wait_for(lock, poll, [this] { return stopping_; })) return;
        }
        std::function<void()> tick;
        {
            std::lock_guard lock(mutex_);
            tick = tick_;
        }
        if (tick) tick();
        const WatchSample s = sample();
        const auto stall = judge_.observe(s);
        if (!stall) continue;
        StallReport r;
        r.kind = stall->first;
        r.at = s.t;
        r.flip = s.flips;
        r.title = stall->second;
        lv2::ReportOptions options;
        options.title = r.title;
        options.describe_address = describe_;
        r.report = lv2::format_report(kernel_, options);
        {
            std::lock_guard lock(mutex_);
            reports_.push_back(r);
        }
        if (on_stall_) on_stall_(r);
    }
}

u64 frame_thumbnail_hash(const u8* bgra, u32 width, u32 height)
{
    constexpr u32 kW = 16, kH = 9;
    u64 h = 1469598103934665603ull; // FNV-1a over the thumbnail's top bits
    if (!bgra || !width || !height) return 0;
    for (u32 ty = 0; ty < kH; ty++) {
        const u32 y0 = ty * height / kH, y1 = std::max(y0 + 1, (ty + 1) * height / kH);
        for (u32 tx = 0; tx < kW; tx++) {
            const u32 x0 = tx * width / kW, x1 = std::max(x0 + 1, (tx + 1) * width / kW);
            u64 sum[3] = {};
            // Every fourth row and column: enough for a picture that changes, cheap on the GPU's completion thread.
            u64 n = 0;
            for (u32 y = y0; y < y1; y += 4) {
                const u8* row = bgra + (static_cast<usize>(y) * width + x0) * 4;
                for (u32 x = x0; x < x1; x += 4, row += 16) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    n++;
                }
            }
            for (u32 c = 0; c < 3; c++) {
                h ^= static_cast<u8>((sum[c] / std::max<u64>(n, 1)) >> 4);
                h *= 1099511628211ull;
            }
        }
    }
    return h ? h : 1;
}

} // namespace pspocket::loader
