#pragma once

// Always-on correctness alarms of the RSX and its backend (D-1022): cheap counters of things done that can show on
// screen as a wrong picture, so a run says in numbers what a frame only shows (the black sea, the striped sky of
// 2026-10-16). Bumped with relaxed atomics where they happen; read by the backend's per-frame report, pspocket-boot's
// summary and scripts/gate_check.py (games/<title>/gate.json limits the ones that should stay at zero).

#include <array>
#include <atomic>
#include <string_view>

#include "pspocket/core/types.hpp"

namespace pspocket::rsx {

enum class Alarm : u8 {
    ReportEarly,        // a ZPASS report's ready word (its pad, 0) written before the GPU's count reached it
    TargetLost,         // a GPU-newer target removed with no copy of its bytes anywhere (another format took its memory)
    TargetLostKB,       // ... their bytes, in KB
    UninitSampled,      // a target nobody drew or cleared yet sampled as a texture
    ConditionalPending, // a conditional draw (NV4097_SET_RENDER_ENABLE mode 2) decided while its report was pending
    OcclusionOverflow,  // an occlusion query that found no free counter in its command buffer's visibility buffer
    DeadOverlay,        // a colour target laid over a compressed (DXT) texture: memory the GPU cannot have written as such
    StaleGpuRead,       // guest memory read where a target the GPU drew lately holds newer bytes, not brought over (D-1056)
    CpuReadsGpuTarget,  // the CPU or an SPU touched a page of a target the GPU drew lately (PSPOCKET_RSX_WATCH_CPU_READS)
    Count
};

inline constexpr std::array<std::string_view, static_cast<usize>(Alarm::Count)> kAlarmNames = {
    "reports_early", "targets_lost", "targets_lost_kb", "uninit_sampled", "conditional_pending", "occlusion_overflow",
    "dead_overlays", "stale_gpu_reads", "cpu_reads_gpu_targets"};

struct AlarmCounts {
    std::array<u64, static_cast<usize>(Alarm::Count)> v{};
    u64 operator[](Alarm a) const { return v[static_cast<usize>(a)]; }
    AlarmCounts operator-(const AlarmCounts& o) const
    {
        AlarmCounts d;
        for (usize i = 0; i < v.size(); i++) d.v[i] = v[i] - o.v[i];
        return d;
    }
};

class Alarms {
public:
    void add(Alarm a, u64 n = 1) { c_[static_cast<usize>(a)].fetch_add(n, std::memory_order_relaxed); }
    u64 get(Alarm a) const { return c_[static_cast<usize>(a)].load(std::memory_order_relaxed); }
    AlarmCounts snapshot() const
    {
        AlarmCounts s;
        for (usize i = 0; i < s.v.size(); i++) s.v[i] = c_[i].load(std::memory_order_relaxed);
        return s;
    }

private:
    std::array<std::atomic<u64>, static_cast<usize>(Alarm::Count)> c_{};
};

} // namespace pspocket::rsx
