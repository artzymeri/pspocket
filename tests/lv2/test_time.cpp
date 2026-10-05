// sys_time and sys_timer through the dispatcher.

#include <ctime>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

TEST_CASE("lv2 time: current time, timebase frequency, system time", "[lv2][time]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 sec = f.alloc(8), nsec = f.alloc(8);
    REQUIRE(c.call(kTimeGetCurrent, sec, nsec) == CELL_OK);
    const s64 host = static_cast<s64>(std::time(nullptr));
    CHECK(std::abs(static_cast<s64>(f.r64(sec)) - host) <= 2);
    CHECK(f.r64(nsec) < 1'000'000'000ull);
    CHECK(c.call(kTimeGetCurrent, 0, nsec) == rc(CELL_EFAULT));
    CHECK(c.call(kTimeGetCurrent, sec, 0) == rc(CELL_EFAULT));

    CHECK(c.call64(kTimebaseFrequency) == 79800000ull);

    const u64 t0 = c.call64(kSystemTime);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const u64 t1 = c.call64(kSystemTime);
    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 45000);
    CHECK(t1 - t0 < 2000000);
}

TEST_CASE("lv2 time: usleep", "[lv2][time]")
{
    Fixture f;
    Caller c(*f.kernel);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kUsleep, 20000) == CELL_OK);
    CHECK(elapsed_ms(t0) >= 19);
    t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kUsleep, 0) == CELL_OK); // a yield
    CHECK(elapsed_ms(t0) < 1000);
}

TEST_CASE("lv2 time: usleep(0) back to back sleeps a little after a run of yields", "[lv2][time]")
{
    // D-104: GTA V's render thread waits with usleep(0) in a loop. The first 16 are yields; after that each sleeps
    // 20 us, doubling up to 200 us: 40 in a row take at least 20 + 40 + 80 + 160 + 20 * 200 us.
    Fixture f;
    Caller c(*f.kernel);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; i++) REQUIRE(c.call(kUsleep, 0) == CELL_OK);
    CHECK(elapsed_ms(t0) >= 4);
    // A pause longer than that between two of them: a plain yield again.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; i++) REQUIRE(c.call(kUsleep, 0) == CELL_OK);
    CHECK(elapsed_ms(t0) < 2);
}

TEST_CASE("lv2 timer: periodic and one-shot timers send to their event queue", "[lv2][timer]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 64) == CELL_OK);
    const u32 q = f.r32(out);
    REQUIRE(c.call(kTimerCreate, out) == CELL_OK);
    const u32 timer = f.r32(out);
    CHECK((timer >> 24) == 0x11);

    CHECK(c.call(kTimerStart, timer, 0, 10000) == rc(CELL_ENOTCONN));
    REQUIRE(c.call(kTimerConnect, timer, q, 0x1234, 7, 8) == CELL_OK);
    CHECK(c.call(kTimerConnect, timer, q, 0, 0, 0) == rc(CELL_EISCONN));
    CHECK(c.call(kTimerConnect, timer, 0x8d00ff00, 0, 0, 0) == rc(CELL_ESRCH));
    CHECK(c.call(kTimerStart, timer, 0, 50) == rc(CELL_EINVAL)); // periods under 100 us are invalid

    const auto start = std::chrono::steady_clock::now();
    REQUIRE(c.call(kTimerStart, timer, 0, 10000) == CELL_OK);
    CHECK(c.call(kTimerStart, timer, 0, 10000) == rc(CELL_EBUSY));
    const u32 info = f.alloc(24);
    REQUIRE(c.call(kTimerGetInfo, timer, info) == CELL_OK);
    CHECK(f.r32(info + 16) == 1); // running
    CHECK(f.r64(info + 8) == 10000);
    u64 prev = 0;
    for (int i = 0; i < 5; i++) {
        REQUIRE(c.call(kQueueReceive, q, 0, 2000000) == CELL_OK);
        CHECK(c.ctx.gpr[4] == 0x1234);
        CHECK(c.ctx.gpr[5] == 7);
        CHECK(c.ctx.gpr[6] == 8);
        if (i) CHECK(c.ctx.gpr[7] == prev + 10000); // data3: the expiry time, one period apart
        prev = c.ctx.gpr[7];
    }
    CHECK(elapsed_ms(start) >= 45);
    REQUIRE(c.call(kTimerStop, timer) == CELL_OK);
    REQUIRE(c.call(kTimerGetInfo, timer, info) == CELL_OK);
    CHECK(f.r32(info + 16) == 0);
    CHECK(f.r64(info) == 0);
    REQUIRE(c.call(kQueueDrain, q) == CELL_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const u32 number = f.alloc(4);
    REQUIRE(c.call(kQueueTryReceive, q, f.alloc(32), 1, number) == CELL_OK);
    CHECK(f.r32(number) == 0); // stopped

    // One-shot: absolute guest system time.
    const u64 now = c.call64(kSystemTime);
    CHECK(c.call(kTimerStart, timer, now - 1, 0) == rc(CELL_ETIMEDOUT)); // in the past
    REQUIRE(c.call(kTimerStart, timer, now + 20000, 0) == CELL_OK);
    REQUIRE(c.call(kQueueReceive, q, 0, 2000000) == CELL_OK);
    CHECK(c.ctx.gpr[7] == now + 20000);
    CHECK(c.call64(kSystemTime) >= now + 20000);
    REQUIRE(c.call(kTimerGetInfo, timer, info) == CELL_OK);
    CHECK(f.r32(info + 16) == 0); // one-shot timers stop after firing

    CHECK(c.call(kTimerDestroy, timer) == rc(CELL_EISCONN));
    REQUIRE(c.call(kTimerDisconnect, timer) == CELL_OK);
    CHECK(c.call(kTimerDisconnect, timer) == rc(CELL_ENOTCONN));
    REQUIRE(c.call(kTimerDestroy, timer) == CELL_OK);
    CHECK(c.call(kTimerStop, timer) == rc(CELL_ESRCH));

    // An unnamed connection uses (pid << 32) | timer ID as the source.
    REQUIRE(c.call(kTimerCreate, out) == CELL_OK);
    const u32 t2 = f.r32(out);
    REQUIRE(c.call(kTimerConnect, t2, q, 0, 0, 0) == CELL_OK);
    REQUIRE(c.call(kTimerStart, t2, 0, 1000) == CELL_OK);
    REQUIRE(c.call(kQueueReceive, q, 0, 2000000) == CELL_OK);
    CHECK(c.ctx.gpr[4] == ((1ull << 32) | t2));
    REQUIRE(c.call(kTimerDisconnect, t2) == CELL_OK); // also stops it
    REQUIRE(c.call(kTimerDestroy, t2) == CELL_OK);
}
