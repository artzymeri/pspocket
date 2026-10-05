#include "pspocket/core/timebase.h"

#include <chrono>

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
    HostClock()
    {
        mach_timebase_info(&info);
        start = mach_absolute_time();
        tb_per_tick = static_cast<uint64_t>((u128{info.numer} * PS_TIMEBASE_HZ << 32) / (u128{info.denom} * 1'000'000'000ull));
    }
    uint64_t ns() const
    {
        const u128 t = mach_absolute_time();
        return static_cast<uint64_t>(t * info.numer / info.denom);
    }
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
    return host_clock().elapsed_tb();
#else
    const u128 ns = host_clock().elapsed_ns();
    return static_cast<uint64_t>(ns * PS_TIMEBASE_HZ / 1'000'000'000ull);
#endif
}

extern "C" uint64_t ps_host_now_ns(void) { return host_clock().ns(); }
