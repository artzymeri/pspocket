// libad_core and libad_async (sceAd*) as a console with no PSN that has never been online (D-637): contexts opened and
// closed by either library, connecting and everything a connection gives answered offline, no space ever open, every
// error, a process each. Synthetic: no game data.

#include <cstring>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 INVALID_ARGUMENT = 0x8002aa03, ID_NO_SPACE = 0x8002aa05, ID_NOT_FOUND = 0x8002aa06, OFFLINE = 0x8002aa0c;
constexpr u32 kNotGuest = 0xF0000000u;

// The calls that need a connection and take only the context (the rest of their arguments are not read).
constexpr const char* kContextCalls[] = {"sceAdConnectContext", "sceAdGetSpaceInfo", "sceAdGetAssetInfo",
    "sceAdFlushReports", "sceAdAsyncConnectContext", "sceAdAsyncSpaceOpen", "sceAdAsyncFlushReports"};

} // namespace

TEST_CASE("sceAd: contexts, 1 to 32, shared by libad_core and libad_async", "[hle][ad]")
{
    Fixture f;
    GuestThread g(f, 0, "game");

    CHECK(g.call("sceAdCloseContext", 1) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("sceAdAsyncCloseContext", 1) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("sceAdOpenContext") == 1);
    CHECK(g.call("sceAdAsyncOpenContext") == 2);
    // Either library closes either's context.
    CHECK(g.call("sceAdAsyncCloseContext", 1) == 0);
    CHECK(g.call("sceAdCloseContext", 2) == 0);
    CHECK(g.call("sceAdCloseContext", 2) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("sceAdAsyncCloseContext", 1) == r3_of_code(ID_NOT_FOUND));

    for (u32 id = 1; id <= 32; id++) {
        INFO(id);
        CHECK(g.call(id % 2 ? "sceAdOpenContext" : "sceAdAsyncOpenContext") == id);
    }
    CHECK(g.call("sceAdOpenContext") == r3_of_code(ID_NO_SPACE));
    CHECK(g.call("sceAdAsyncOpenContext") == r3_of_code(ID_NO_SPACE));
    CHECK(g.call("sceAdCloseContext", 9) == 0);
    CHECK(g.call("sceAdAsyncOpenContext") == 9); // the lowest free ID
    for (u32 id : {0u, 33u, kData}) {
        INFO(id);
        CHECK(g.call("sceAdCloseContext", id) == r3_of_code(ID_NOT_FOUND));
        CHECK(g.call("sceAdAsyncCloseContext", id) == r3_of_code(ID_NOT_FOUND));
    }
    for (u32 id = 1; id <= 32; id++) {
        INFO(id);
        CHECK(g.call("sceAdCloseContext", id) == 0);
    }

    CHECK(registry().is_firmware_first("libad_core"));
    CHECK(registry().is_firmware_first("libad_async"));
}

TEST_CASE("sceAd: a context never connects; nothing a connection gives is there, nothing is written", "[hle][ad]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kInfo = kData + 0x100;
    std::memset(b + kInfo, 0xC3, 0x100);

    const u64 core = g.call("sceAdOpenContext"), async = g.call("sceAdAsyncOpenContext");
    REQUIRE(core == 1);
    REQUIRE(async == 2);
    for (const char* name : kContextCalls) {
        INFO(name);
        CHECK(g.call(name, core) == r3_of_code(OFFLINE));
        CHECK(g.call(name, async) == r3_of_code(OFFLINE));
        CHECK(g.call(name, 3) == r3_of_code(ID_NOT_FOUND));
        CHECK(g.call(name, 0) == r3_of_code(ID_NOT_FOUND));
    }
    CHECK(g.call("sceAdGetConnectionInfo", core, kInfo) == r3_of_code(OFFLINE));
    CHECK(g.call("sceAdGetConnectionInfo", async, kInfo) == r3_of_code(OFFLINE));
    CHECK(g.call("sceAdGetConnectionInfo", core, 0) == r3_of_code(INVALID_ARGUMENT));
    CHECK(g.call("sceAdGetConnectionInfo", core, kNotGuest) == r3_of_code(INVALID_ARGUMENT));
    CHECK(g.call("sceAdGetConnectionInfo", 3, kInfo) == r3_of_code(ID_NOT_FOUND));
    // No space is ever open: closing one finds none, once the context is found.
    CHECK(g.call("sceAdAsyncSpaceClose", core, 1) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("sceAdAsyncSpaceClose", 3, 1) == r3_of_code(ID_NOT_FOUND));
    for (u32 i = 0; i < 0x100; i++) {
        INFO(i);
        CHECK(ps_read8(b, kInfo + i) == 0xC3);
    }

    // Closed, the context is gone for every call.
    REQUIRE(g.call("sceAdCloseContext", core) == 0);
    for (const char* name : kContextCalls) {
        INFO(name);
        CHECK(g.call(name, core) == r3_of_code(ID_NOT_FOUND));
    }
    CHECK(g.call("sceAdGetConnectionInfo", core, kInfo) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("sceAdAsyncCloseContext", async) == 0);
}

TEST_CASE("sceAd: each process its own contexts", "[hle][ad]")
{
    {
        Fixture f;
        GuestThread g(f, 0, "game");
        CHECK(g.call("sceAdOpenContext") == 1);
    }
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("sceAdCloseContext", 1) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("sceAdAsyncOpenContext") == 1);
}
