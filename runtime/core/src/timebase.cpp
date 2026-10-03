#include "pspocket/core/timebase.h"

#include <chrono>

#include "pspocket/core/types.hpp"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

using pspocket::u128;

namespace {

#if defined(__APPLE__)
struct HostClock {
    mach_timebase_info_data_t info{};
    uint64_t start = 0;
    HostClock()
    {
        mach_timebase_info(&info);
        start = mach_absolute_time();
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
    const u128 ns = host_clock().elapsed_ns();
    return static_cast<uint64_t>(ns * PS_TIMEBASE_HZ / 1'000'000'000ull);
}

extern "C" uint64_t ps_host_now_ns(void) { return host_clock().ns(); }
