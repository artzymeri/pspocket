// Shake mode (pspocket/core/chaos.hpp, D-1045): its configuration, its rates, each thread's own seeded stream, the
// injection log, the conditional store's spurious failure and the shaken timebase (continuous, monotonic).

#include <catch_amalgamated.hpp>

#include <atomic>
#include <format>
#include <cstring>
#include <thread>
#include <vector>

#include <pthread.h>

#include "pspocket/core/chaos.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/core/timebase.h"

using namespace pspocket;
using namespace pspocket::chaos;

namespace {

struct Off {
    ~Off() { disable(); }
};

} // namespace

TEST_CASE("shake mode: off costs a mask test; the configuration names points and families", "[core][shake]")
{
    Off off;
    disable();
    CHECK_FALSE(any());
    CHECK_FALSE(hit(S1_Syscall));
    std::string error;
    REQUIRE(configure("seed=42;S1=500000:0;S8", &error));
    CHECK(seed() == 42);
    CHECK(on(S1_Syscall));
    CHECK(on(S8_StoreConditional));
    CHECK_FALSE(on(S3_AfterWake));
    CHECK(description().find("S1") != std::string::npos);
    int hits = 0;
    for (int i = 0; i < 10000; i++) hits += hit(S1_Syscall);
    CHECK(hits > 4500);
    CHECK(hits < 5500);
    CHECK(counts().hits[S1_Syscall] >= static_cast<u64>(hits));

    REQUIRE(configure("seed=1;legal", &error));
    for (usize p = 1; p < kPoints; p++) {
        INFO(info(static_cast<Point>(p)).id);
        CHECK(on(static_cast<Point>(p)) == (info(static_cast<Point>(p)).family == Family::Ps3Legal));
    }
    REQUIRE(configure("seed=1;all;-S15;-S13", &error)); // all but the host load and the dialogs
    CHECK(on(S7_TimebaseRate));
    CHECK(on(S16_WakeOvertake));
    CHECK_FALSE(on(S15_HostLoad));
    CHECK_FALSE(on(S13_SysutilDialog));
    for (const char* bad : {"S99", "seed=1;banana", "S1=abc", "S1=10:x"}) {
        INFO(bad);
        CHECK_FALSE(configure(bad, &error));
        CHECK(error.find("PSPOCKET_SHAKE") != std::string::npos);
    }
    CHECK(on(S7_TimebaseRate)); // a bad text changes nothing
}

TEST_CASE("shake mode: each thread's stream follows the seed and the thread's name", "[core][shake]")
{
    Off off;
    // A thread's stream is made at its first draw from the seed then configured.
    REQUIRE(configure("seed=5;S1"));
    std::vector<u64> a, b, c;
    for (auto* out : {&a, &b}) {
        std::thread t([out] {
#if defined(__APPLE__)
            pthread_setname_np("PPU 0x1000003 worker");
#else
            pthread_setname_np(pthread_self(), "PPU worker");
#endif
            for (int i = 0; i < 8; i++) out->push_back(next());
        });
        t.join();
    }
    CHECK(a == b); // the same seed and name: the same decisions
    REQUIRE(configure("seed=6;S1"));
    std::thread t([&c] {
#if defined(__APPLE__)
        pthread_setname_np("PPU 0x1000003 worker");
#else
        pthread_setname_np(pthread_self(), "PPU worker");
#endif
        for (int i = 0; i < 8; i++) c.push_back(next());
    });
    t.join();
    CHECK(a != c); // another seed: others
}

TEST_CASE("shake mode: S8 makes conditional stores fail now and then, the log keeps every injection", "[core][shake]")
{
    Off off;
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(0x10000, 0x10000));
    u8* base = mem.base();
    REQUIRE(configure("seed=3;S8=300000"));
    const u64 before = counts().hits[S8_StoreConditional];
    int failed = 0;
    for (int i = 0; i < 2000; i++) {
        alignas(16) u8 line[PS_RES_LINE_SIZE];
        const u64 version = ps_res_acquire(base, 0x10000, line);
        if (!ps_res_store_conditional(base, 0x10000, 4, static_cast<u64>(i), version, line)) failed++;
    }
    CHECK(failed > 450);
    CHECK(failed < 750);
    CHECK(counts().hits[S8_StoreConditional] - before == static_cast<u64>(failed));
    const std::string log = log_text();
    CHECK(log.find(" S8 ") != std::string::npos);
    disable();
    for (int i = 0; i < 100; i++) {
        alignas(16) u8 line[PS_RES_LINE_SIZE];
        const u64 version = ps_res_acquire(base, 0x10000, line);
        CHECK(ps_res_store_conditional(base, 0x10000, 4, 7, version, line));
    }
}

TEST_CASE("shake mode: S7's timebase stays continuous and monotonic at a rate near the true one", "[core][shake]")
{
    Off off;
    REQUIRE(configure("seed=9;S7"));
    std::atomic<bool> bad{false};
    std::vector<std::thread> pool;
    for (int k = 0; k < 4; k++) {
        pool.emplace_back([&] {
            u64 last = 0;
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
            while (std::chrono::steady_clock::now() < end) {
                const u64 tb = ps_timebase_now();
                if (tb < last) bad = true;
                last = tb;
            }
        });
    }
    const u64 tb0 = ps_timebase_now();
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& t : pool) t.join();
    const u64 tb1 = ps_timebase_now();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(bad.load());
    const double rate = static_cast<double>(tb1 - tb0) / (secs * static_cast<double>(PS_TIMEBASE_HZ));
    CHECK(rate > 0.97);
    CHECK(rate < 1.03);
    CHECK(counts().hits[S7_TimebaseRate] >= 1); // a new rate every quarter second
}
