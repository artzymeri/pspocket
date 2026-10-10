// cellLibprof's user trace with no profiler attached (D-638): Init and Terminate, the registrations kept and forgotten,
// every error, a process each. Synthetic: no game data.

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

TEST_CASE("cellUserTrace: Init and Terminate bracket the library", "[hle][libprof]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    const u64 estat = r3_of(lv2::CELL_ESTAT);

    CHECK(g.call("cellUserTraceRegister", kData, 1, 2) == estat);
    CHECK(g.call("cellUserTraceUnregister", kData) == estat);
    CHECK(g.call("cellUserTraceTerminate") == estat);
    CHECK(g.call("cellUserTraceRegister", 0, 0, 0) == estat); // the state first

    CHECK(g.call("cellUserTraceInit") == 0);
    CHECK(g.call("cellUserTraceInit") == estat);
    CHECK(g.call("cellUserTraceTerminate") == 0);
    CHECK(g.call("cellUserTraceTerminate") == estat);
    CHECK(g.call("cellUserTraceRegister", kData, 1, 2) == estat);
    CHECK(g.call("cellUserTraceInit") == 0); // again after Terminate
    CHECK(g.call("cellUserTraceTerminate") == 0);

    CHECK(registry().is_firmware_first("cellLibprof"));
}

TEST_CASE("cellUserTrace: registrations kept under what names them, 64 at most", "[hle][libprof]")
{
    Fixture f;
    GuestThread g(f, 0, "game");

    REQUIRE(g.call("cellUserTraceInit") == 0);
    CHECK(g.call("cellUserTraceRegister", 0, 1, 2) == r3_of(lv2::CELL_EINVAL));
    CHECK(g.call("cellUserTraceUnregister", kData) == r3_of(lv2::CELL_ESRCH));
    REQUIRE(g.call("cellUserTraceRegister", kData, 1, 2) == 0);
    CHECK(g.call("cellUserTraceRegister", kData, 3, 4) == r3_of(lv2::CELL_EEXIST));
    CHECK(g.call("cellUserTraceUnregister", kData) == 0);
    CHECK(g.call("cellUserTraceUnregister", kData) == r3_of(lv2::CELL_ESRCH));
    CHECK(g.call("cellUserTraceRegister", kData, 3, 4) == 0); // once forgotten, again

    // 64 in all; the 65th waits for one to go.
    for (u32 i = 1; i < 64; i++) {
        INFO(i);
        REQUIRE(g.call("cellUserTraceRegister", kData + 0x10 * i, i, 0) == 0);
    }
    CHECK(g.call("cellUserTraceRegister", kData + 0x10 * 64, 0, 0) == r3_of(lv2::CELL_EAGAIN));
    CHECK(g.call("cellUserTraceRegister", kData + 0x10 * 5, 0, 0) == r3_of(lv2::CELL_EEXIST)); // kept comes first
    REQUIRE(g.call("cellUserTraceUnregister", kData + 0x10 * 5) == 0);
    CHECK(g.call("cellUserTraceRegister", kData + 0x10 * 64, 0, 0) == 0);
    CHECK(g.call("cellUserTraceUnregister", kData + 0x10 * 5) == r3_of(lv2::CELL_ESRCH));

    // Terminate forgets them all.
    REQUIRE(g.call("cellUserTraceTerminate") == 0);
    REQUIRE(g.call("cellUserTraceInit") == 0);
    CHECK(g.call("cellUserTraceUnregister", kData) == r3_of(lv2::CELL_ESRCH));
    CHECK(g.call("cellUserTraceUnregister", kData + 0x10 * 64) == r3_of(lv2::CELL_ESRCH));
    CHECK(g.call("cellUserTraceRegister", kData, 1, 2) == 0);
    CHECK(g.call("cellUserTraceTerminate") == 0);
}

TEST_CASE("cellUserTrace: each process its own library", "[hle][libprof]")
{
    {
        Fixture f;
        GuestThread g(f, 0, "game");
        CHECK(g.call("cellUserTraceInit") == 0);
        CHECK(g.call("cellUserTraceRegister", kData, 0, 0) == 0);
    }
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("cellUserTraceTerminate") == r3_of(lv2::CELL_ESTAT));
    CHECK(g.call("cellUserTraceInit") == 0);
    CHECK(g.call("cellUserTraceUnregister", kData) == r3_of(lv2::CELL_ESRCH));
}
