#include "pspocket/core/timebase.h"

#include <chrono>
#include <thread>

#include "pspocket/core/chaos.hpp"
#include "pspocket/core/types.hpp"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

using pspocket::u128;

namespace {

#if defined(__APPLE__)
// Host ticks to timebase ticks by one multiply (D-128): two 128-bit divisions a read were 1% of the iPhone's SPU
// time (GTA V's jobs read the decrementer millions of times a second). The factor (79.8 MHz over the host's 24 MHz
// on Apple silicon: 3.325) is kept to 32 fraction bits: it drifts about a microsecond a day and stays monotonic.
struct HostClock {
    mach_timebase_info_data_t info{};
    uint64_t start = 0;
    uint64_t tb_per_tick = 0; // timebase ticks per host tick, 32.32 fixed point
    uint64_t ns_per_tick = 0; // nanoseconds per host tick, 32.32 fixed point (41.67 on Apple silicon)
    HostClock()
    {
        mach_timebase_info(&info);
        start = mach_absolute_time();
        tb_per_tick = static_cast<uint64_t>((u128{info.numer} * PS_TIMEBASE_HZ << 32) / (u128{info.denom} * 1'000'000'000ull));
        ns_per_tick = static_cast<uint64_t>((u128{info.numer} << 32) / info.denom);
    }
    // One multiply, as the timebase (D-128): the SPURS workers and the SPU DMA log read it millions of times a second
    // (D-241).
    uint64_t ns() const { return static_cast<uint64_t>(u128{mach_absolute_time()} * ns_per_tick >> 32); }
    uint64_t ticks_for_ns(uint64_t ns) const { return static_cast<uint64_t>((u128{ns} << 32) / ns_per_tick); }
    uint64_t elapsed_ns() const
    {
        const u128 t = mach_absolute_time() - start;
        return static_cast<uint64_t>(t * info.numer / info.denom);
    }
    uint64_t elapsed_tb() const { return static_cast<uint64_t>(u128{mach_absolute_time() - start} * tb_per_tick >> 32); }
};
#else
struct HostClock {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t ns() const
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    uint64_t elapsed_ns() const
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }
};
#endif

const HostClock& host_clock()
{
    static const HostClock clock;
    return clock;
}

} // namespace

extern "C" uint64_t ps_timebase_now(void)
{
#if defined(__APPLE__)
    const uint64_t tb = host_clock().elapsed_tb();
#else
    const u128 ns = host_clock().elapsed_ns();
    const uint64_t tb = static_cast<uint64_t>(ns * PS_TIMEBASE_HZ / 1'000'000'000ull);
#endif
    // Shake mode's S7 (D-1045): a little fast or slow, a while at a time.
    if (pspocket::chaos::on(pspocket::chaos::S7_TimebaseRate)) [[unlikely]] return pspocket::chaos::shaken_timebase(tb);
    return tb;
}

extern "C" uint64_t ps_host_now_ns(void) { return host_clock().ns(); }

extern "C" void ps_host_sleep_ns(uint64_t ns)
{
#if defined(__APPLE__)
    // A Mach trap: nanosleep (std::this_thread::sleep_for) asks the clock service for the time by a Mach message first,
    // which was most of what an idle SPURS worker's short sleeps cost (D-241, run 221).
    const HostClock& c = host_clock();
    mach_wait_until(mach_absolute_time() + c.ticks_for_ns(ns));
#else
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
#endif
}
