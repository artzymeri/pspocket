// lv2's remaining small families as a retail console (D-527 part 2): debug kernels' calls, the system software's,
// one process, the calls Sony's game-loadable modules make, the console, and sys_rsx's device and free calls.

#include <cstring>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

Config root_config()
{
    Config c;
    c.root = true;
    return c;
}

} // namespace

TEST_CASE("lv2 retail console: debug kernels' calls are not here", "[lv2][misc]")
{
    Fixture f;
    Caller c(*f.kernel, "dbg");
    for (const u32 n : {60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u, 68u, 69u, 77u, 960u, 961u, 962u, 963u, 964u, 965u,
             966u, 967u, 699u, 880u, 881u, 882u, 883u, 884u, 885u, 890u, 891u, 892u, 893u, 894u, 895u, 896u, 897u,
             899u, 640u, 641u, 642u, 643u, 644u, 645u, 646u, 647u, 968u}) {
        INFO(n);
        CHECK(c.call(n, 0, 0, 0) == rc(CELL_ENOSYS));
    }
}

// The retail kernel's own table (D-936): the calls it points at its not-implemented slot; and the debugging calls it
// has, ported from its handlers (D-937).
TEST_CASE("lv2 retail console: what the retail kernel's table lacks, and its console type", "[lv2][misc]")
{
    {
        Fixture f;
        Caller c(*f.kernel, "game");
        for (const u32 n : {215u, 216u, 901u, 907u, 908u, 912u, 913u, 915u, 916u, 917u, 918u, 919u, 920u, 921u, 922u,
                 923u, 924u, 925u, 926u, 927u, 928u, 929u, 930u, 931u, 932u, 934u, 936u, 938u, 939u, 940u, 942u, 946u,
                 948u, 949u, 950u, 952u, 953u, 954u, 955u, 956u, 970u}) {
            INFO(n);
            CHECK(c.call(n, 0, 0, 0) == rc(CELL_ENOSYS));
        }
        // sys_dbg_get_console_type: a null pointer first; then a game's control flags are below the system level.
        CHECK(c.call(985, 0) == rc(CELL_EFAULT));
        CHECK(c.call(985, f.alloc(8)) == rc(CELL_ENOSYS));
    }
    {
        Fixture f(root_config());
        Caller c(*f.kernel, "system");
        const u32 type = f.alloc(8);
        REQUIRE(c.call(985, type) == CELL_OK);
        CHECK(f.r64(type) == 1); // a retail console
        CHECK(c.call(985, 0) == rc(CELL_EFAULT));
    }
}

TEST_CASE("lv2 crypto engine: created and destroyed; without the system's keys nothing prepares", "[lv2][misc]")
{
    Fixture f;
    Caller c(*f.kernel, "game");
    const u32 id = f.alloc(4), buf = f.alloc(0x40);
    REQUIRE(c.call(976, id) == CELL_OK);
    const u32 e = f.r32(id);
    CHECK((e >> 24) == 0x78);
    // A game may prepare hasher index 1 and cipher table 1 / index 1 / mode 0-1 only.
    CHECK(c.call(978, e, 2) == rc(CELL_EPERM));
    CHECK(c.call(978, e, 1) == rc(CELL_ENOENT)); // the system's key tables are not here
    CHECK(c.call(978, 0x78000999, 1) == rc(CELL_ESRCH));
    CHECK(c.call(981, e, 1, 2, 1, buf, 16) == rc(CELL_EPERM));
    CHECK(c.call(981, e, 1, 1, 1, buf, 16) == rc(CELL_ENOENT));
    CHECK(c.call(981, e, 1, 1, 1, 0x7FFFFFF0, 0x100) == rc(CELL_EFAULT)); // the key is copied in first
    // Unprepared: a run's first piece and the hash are refused; nothing to run is fine; the hash wants 20 bytes.
    CHECK(c.call(979, e, buf, 16) == rc(CELL_EINVAL));
    CHECK(c.call(979, e, buf, 0) == CELL_OK);
    CHECK(c.call(980, e, buf, 19) == rc(CELL_EFAULT));
    CHECK(c.call(980, e, buf, 20) == rc(CELL_EINVAL));
    CHECK(c.call(982, e, buf, buf, 16) == rc(CELL_EINVAL));
    CHECK(c.call(983, e, buf, 20) == rc(CELL_EINVAL));
    // Random bytes: 16 at a time.
    CHECK(c.call(984, buf, 15) == rc(CELL_EINVAL));
    CHECK(c.call(984, buf, 64) == CELL_OK);
    REQUIRE(c.call(977, e) == CELL_OK);
    CHECK(c.call(977, e) == rc(CELL_ESRCH));
    CHECK(c.call(979, e, buf, 16) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 retail console: the system software's calls, a game's and root's", "[lv2][misc]")
{
    const std::initializer_list<u32> system_calls = {583u, 584u, 585u, 586u, 587u, 588u, 589u, 590u, 591u, 592u, 593u,
        594u, 595u, 596u, 597u, 635u, 470u, 471u, 860u, 861u, 862u, 869u, 4u};
    {
        Fixture f;
        Caller c(*f.kernel, "game");
        for (const u32 n : system_calls) {
            INFO(n);
            CHECK(c.call(n, 0, 0, 0) == rc(CELL_ENOSYS));
        }
        const u32 p = f.alloc(16);
        CHECK(c.call(2, 2, p, 0) == rc(CELL_ENOSYS));
        CHECK(c.call(28, 0x85, p) == rc(CELL_ENOSYS));
        CHECK(c.call(119, p) == rc(CELL_ENOSYS));
        CHECK(c.call(398, p, 4) == rc(CELL_ENOSYS));
        CHECK(c.call(19, 2) == rc(CELL_ESRCH));
        CHECK(c.call(19, 1) == rc(CELL_ESRCH));
    }
    Fixture f(root_config());
    Caller c(*f.kernel, "root");
    for (const u32 n : system_calls) {
        INFO(n);
        CHECK(c.call(n, 0, 0, 0) == CELL_OK);
    }
    const u32 p = f.alloc(16);
    CHECK(c.call(2, 2, p, 0) == rc(CELL_ESRCH));
    CHECK(c.call(5, 2) == rc(CELL_ESRCH));
    CHECK(c.call(23, 2, 0, 0, 0, 0, 0) == rc(CELL_ESRCH));
    CHECK(c.call(21) == rc(CELL_ENOTSUP));
    CHECK(c.call(27, p, 1000, 0, 0, 0, 0, 0, 0) == rc(CELL_ENOTSUP));
    // _sys_process_get_number_of_object: root's sys_process_get_number_of_object (mutexes here: none).
    REQUIRE(c.call(28, 0x85, p) == CELL_OK);
    CHECK(f.r32(p) == 0);
    REQUIRE(c.call(119, p) == CELL_OK);
    CHECK(f.r64(p) > 1700000000ull); // seconds since 1970, after 2023
    CHECK(c.call(119, 0) == rc(CELL_EFAULT));
}

TEST_CASE("lv2 retail console: the console, Bluetooth audio events, module-made calls", "[lv2][misc]")
{
    Fixture f;
    Caller c(*f.kernel, "misc");
    const u32 text = f.alloc(16);
    std::memcpy(f.base() + text, "hello\n", 7);
    CHECK(c.call(213, text, 6) == CELL_OK);
    CHECK(c.call(213, 0x7FF00000, 6) == rc(CELL_EFAULT));
    CHECK(c.call(213, text, 0) == CELL_OK);

    const u32 outs = f.alloc(16);
    CHECK(c.call(579, 0, outs, outs + 4, outs + 8, 0) == rc(CELL_EINVAL));
    CHECK(c.call(579, 2, outs, outs + 4, outs + 8, outs + 12) == static_cast<s32>(0x80111003));
    CHECK(c.call(579, 1, outs, outs + 4, outs + 8, outs + 12) == CELL_OK);

    for (const u32 n : {167u, 244u, 245u, 246u, 247u, 333u, 336u, 338u, 344u, 864u, 876u, 878u, 879u, 633u, 634u, 621u,
             570u, 571u, 572u, 573u, 574u, 575u, 839u, 843u, 844u, 31u}) {
        INFO(n);
        CHECK(c.call(n, 0, 0, 0) == CELL_OK);
    }
}

TEST_CASE("lv2 RSX: device open and close, freeing memory and the context", "[lv2][misc]")
{
    Fixture f;
    Caller c(*f.kernel, "rsx");
    CHECK(c.call(666) == CELL_OK);
    CHECK(c.call(667) == CELL_OK);
    CHECK(c.call(669, 0x5a5a5a5b) == rc(CELL_ENOMEM)); // no local memory yet
    CHECK(c.call(671, 0x55555555) == rc(CELL_EINVAL)); // no context
    CHECK(c.call(676, 8) == CELL_OK);
    CHECK(c.call(677, 0x100, 0, 0, 0, 0) == CELL_OK);

    const u32 handle = f.alloc(4), addr = f.alloc(8);
    REQUIRE(c.call(668, handle, addr, 0x1000000, 0, 0, 0, 0) == CELL_OK);
    const u32 local = static_cast<u32>(f.r64(addr));
    CHECK(f.mem.is_committed(local));
    REQUIRE(c.call(669, f.r32(handle)) == CELL_OK);
    CHECK(!f.mem.is_committed(local));
    CHECK(c.call(669, f.r32(handle)) == rc(CELL_ENOMEM));
    REQUIRE(c.call(668, handle, addr, 0x1000000, 0, 0, 0, 0) == CELL_OK); // allocatable again
}

TEST_CASE("lv2 root's update manager and the AV manager's UART", "[lv2][misc]")
{
    {
        Fixture f;
        Caller c(*f.kernel, "game");
        for (const u32 n : {863u, 367u, 368u, 369u, 370u}) CHECK(c.call(n, 0x6003, 0, 0) == rc(CELL_ENOSYS));
    }
    Fixture f(root_config());
    Caller c(*f.kernel, "root");
    const u32 out = f.alloc(16);
    REQUIRE(c.call(863, 0x6003, 0, out) == CELL_OK);
    CHECK(f.r64(out) == u64{0x492} << 40); // 4.92, Config's default
    REQUIRE(c.call(863, 0x600B, 0x48C1B, out) == CELL_OK);
    CHECK(f.base()[out] == 0x01);
    REQUIRE(c.call(863, 0x600B, 0x1234, out) == CELL_OK);
    CHECK(f.base()[out] == 0xFF);
    REQUIRE(c.call(863, 0x600C, 0x1234, 0x42) == CELL_OK);
    REQUIRE(c.call(863, 0x600B, 0x1234, out) == CELL_OK);
    CHECK(f.base()[out] == 0x42);
    REQUIRE(c.call(863, 0x600C, 0x1234, 0xFF) == CELL_OK);
    REQUIRE(c.call(863, 0x600B, 0x1234, out) == CELL_OK);
    CHECK(f.base()[out] == 0xFF);
    REQUIRE(c.call(863, 0x600E, 0x2000, out) == CELL_OK);
    const u32 buf = f.r32(out);
    CHECK(f.mem.is_committed(buf));
    CHECK(c.call(863, 0x600F, buf) == CELL_OK);
    CHECK(!f.mem.is_committed(buf));
    CHECK(c.call(863, 0x600F, buf) == rc(CELL_ENOMEM));
    REQUIRE(c.call(863, 0x6011, 0, out) == CELL_OK);
    CHECK(f.r64(out) == u64{0x30040} << 32);
    CHECK(c.call(863, 0x6099, 0, out) == rc(CELL_EINVAL));

    CHECK(c.call(370, out) == rc(CELL_ESRCH)); // not initialized
    REQUIRE(c.call(367) == CELL_OK);
    CHECK(c.call(367) == rc(CELL_EPERM));
    REQUIRE(c.call(370, out) == CELL_OK);
    CHECK(f.r64(out) == 0x800);
    CHECK(f.r64(out + 8) == 0x800);
    CHECK(c.call(369, out, 16, 0) == 16);
    CHECK(c.call(369, out, 16, 7) == rc(CELL_EINVAL));
    CHECK(c.call(368, out, 16, 0) == 0); // nothing comes back
    CHECK(c.call(368, out, 0, 0) == CELL_OK);
}
