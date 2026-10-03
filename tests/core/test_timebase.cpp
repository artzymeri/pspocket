#include <catch_amalgamated.hpp>

#include <chrono>
#include <thread>

#include "pspocket/core/timebase.h"

TEST_CASE("timebase runs at 79.8 MHz")
{
    const uint64_t t0 = ps_timebase_now();
    const uint64_t h0 = ps_host_now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t t1 = ps_timebase_now();
    const uint64_t h1 = ps_host_now_ns();

    REQUIRE(t1 > t0);
    const double seconds = static_cast<double>(h1 - h0) / 1e9;
    const double hz = static_cast<double>(t1 - t0) / seconds;
    CHECK(hz == Catch::Approx(79.8e6).epsilon(0.02));
}
