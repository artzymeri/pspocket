// lv2's GPIO, I/O buffers, system manager and debugger memory calls (D-527): a retail console running a game, and a
// root process.

#include <cstring>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u32 kGpioSet = 630, kGpioGet = 631, kIoCreate = 624, kIoDestroy = 625, kIoAllocate = 626, kIoFree = 627;
constexpr u32 kSmShopMode = 377, kSmExtEvent2 = 378, kSmShutdown = 379, kSmParams = 380, kSmLed = 386,
              kSmBuzzer = 392, kSmFanPolicy = 409, kDbgRead = 904, kDbgWrite = 905;

Config root_config()
{
    Config c;
    c.root = true;
    return c;
}

} // namespace

TEST_CASE("lv2 GPIO: a retail console's LEDs and DIP switches", "[lv2][devices]")
{
    Fixture f;
    Caller c(*f.kernel, "gpio");
    const u32 v = f.alloc(8);
    f.w64(v, 0x1234);
    CHECK(c.call(kGpioGet, 1, v) == CELL_OK);
    CHECK(f.r64(v) == 0);
    f.w64(v, 0x1234);
    CHECK(c.call(kGpioGet, 2, v) == CELL_OK);
    CHECK(f.r64(v) == 0);
    CHECK(c.call(kGpioGet, 0, v) == rc(CELL_ESRCH));
    CHECK(c.call(kGpioGet, 3, v) == rc(CELL_ESRCH));
    CHECK(c.call(kGpioGet, 1, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kGpioSet, 1, 0xFF, 0x1) == CELL_OK);
    CHECK(c.call(kGpioSet, 2, 0xFF, 0x1) == rc(CELL_EINVAL));
    CHECK(c.call(kGpioSet, 7, 0xFF, 0x1) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 I/O buffers: blocks of user memory per buffer", "[lv2][devices]")
{
    Fixture f;
    Caller c(*f.kernel, "io");
    const u32 out = f.alloc(8);
    CHECK(c.call(kIoCreate, 4, 0x100, 4, 0, 0) == rc(CELL_EFAULT));
    REQUIRE(c.call(kIoCreate, 4, 0x100, 4, 0, out) == CELL_OK);
    const u32 h = f.r32(out);
    CHECK(h == 0x44000000);
    REQUIRE(c.call(kIoCreate, 2, 0x100, 2, 0, out) == CELL_OK);
    CHECK(f.r32(out) == 0x44000001);

    CHECK(c.call(kIoAllocate, h, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kIoAllocate, 0x44000100, out) == rc(CELL_ESRCH));
    REQUIRE(c.call(kIoAllocate, h, out) == CELL_OK);
    const u32 block = f.r32(out);
    CHECK(block % 0x10000 == 0);
    CHECK(f.mem.is_committed(block));
    CHECK(f.mem.is_committed(block + 4 * 0x100 - 1));
    CHECK(c.call(kIoFree, h, 0x12340000) == CELL_OK); // not its block: nothing freed
    CHECK(f.mem.is_committed(block));
    CHECK(c.call(kIoFree, 0x44000100, block) == rc(CELL_ESRCH));
    CHECK(c.call(kIoFree, h, block) == CELL_OK);
    CHECK(!f.mem.is_committed(block));

    REQUIRE(c.call(kIoAllocate, h, out) == CELL_OK);
    const u32 kept = f.r32(out);
    CHECK(c.call(kIoDestroy, h) == CELL_OK); // its blocks go with it
    CHECK(!f.mem.is_committed(kept));
    CHECK(c.call(kIoAllocate, h, out) == rc(CELL_ESRCH));
    CHECK(c.call(kIoDestroy, h) == CELL_OK);

    REQUIRE(c.call(kIoCreate, 0, 0x100, 0, 0, out) == CELL_OK);
    CHECK(c.call(kIoAllocate, f.r32(out), out) == rc(CELL_ENOMEM));
}

TEST_CASE("lv2 system manager: a game is refused, a root process answered", "[lv2][devices]")
{
    {
        Fixture f;
        Caller c(*f.kernel, "game");
        const u32 p = f.alloc(32);
        for (const u32 n : {kSmShopMode, kSmExtEvent2, kSmShutdown, kSmParams, kSmLed, kSmBuzzer, kSmFanPolicy, 381u,
                 382u, 383u, 384u, 385u, 387u, 388u, 389u, 390u, 391u, 393u, 394u, 395u, 396u, 397u, 408u}) {
            INFO(n);
            CHECK(c.call(n, p, p + 8, p + 16, 0) == rc(CELL_ENOSYS));
        }
    }
    Fixture f(root_config());
    Caller c(*f.kernel, "root");
    const u32 p = f.alloc(32);
    std::memset(f.base() + p, 0xAA, 32);
    CHECK(c.call(kSmExtEvent2, p, p + 8, p + 16, 2) == rc(CELL_EINVAL));
    CHECK(c.call(kSmExtEvent2, p, p + 8, p + 16, 1) == rc(CELL_EAGAIN));
    CHECK(f.r64(p) == 0);
    CHECK(f.r64(p + 16) == 0);
    CHECK(c.call(kSmExtEvent2, p, 0, p + 16, 0) == rc(CELL_EFAULT));
    std::memset(f.base() + p, 0xAA, 32);
    REQUIRE(c.call(kSmParams, p, p + 1, p + 4, p + 8) == CELL_OK);
    CHECK(f.base()[p] == 0);
    CHECK(f.base()[p + 1] == 0);
    CHECK(f.r32(p + 4) == 0x200);
    CHECK(f.r64(p + 8) == 7);
    CHECK(c.call(kSmParams, p, p + 1, 0, p + 8) == rc(CELL_EFAULT));
    CHECK(c.call(kSmShutdown, 0x8201, 0, 0) == rc(CELL_ENOTSUP));
    CHECK(c.call(kSmShutdown, 0x300, 0, 0) == rc(CELL_EINVAL));
    for (const u32 n : {kSmShopMode, kSmLed, kSmBuzzer, kSmFanPolicy, 381u, 382u, 383u, 384u, 385u, 387u, 388u, 389u,
             390u, 391u, 393u, 394u, 395u, 396u, 397u, 408u}) {
        INFO(n);
        CHECK(c.call(n, p, p + 8, p + 16) == CELL_OK);
    }
}

TEST_CASE("lv2 debugger: a retail console has no process memory calls (D-938)", "[lv2][devices]")
{
    Fixture f;
    Caller c(*f.kernel, "dbg");
    const u32 src = f.alloc(64), dst = f.alloc(64);
    CHECK(c.call(kDbgRead, 1, src, 64, dst) == rc(CELL_ENOSYS));
    CHECK(c.call(kDbgWrite, 1, src, 32, dst) == rc(CELL_ENOSYS));
}
