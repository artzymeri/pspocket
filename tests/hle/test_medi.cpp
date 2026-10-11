// libmedi (cellMediator) as a console with no PSN (D-636): contexts made and closed, everything a context does answered
// offline, the console's signature not made, every error, a process each. Synthetic: no game data.

#include <cstring>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 INVALID_ARGUMENT = 0x8002aa03, ID_NO_SPACE = 0x8002aa05, ID_NOT_FOUND = 0x8002aa06, OFFLINE = 0x8002aa0c;
constexpr u32 kNotGuest = 0xF0000000u;

// The calls that take a context and a pointer to fill or read, and what they answer with both good.
struct PointerCall {
    const char* name;
    u32 answer;
};
constexpr PointerCall kPointerCalls[] = {
    {"cellMediatorGetStatus", OFFLINE},
    {"cellMediatorGetProviderUrl", OFFLINE},
    {"cellMediatorGetUserInfo", OFFLINE},
    {"cellMediatorPostReports", OFFLINE},
    {"cellMediatorReliablePostReports", OFFLINE},
    {"cellMediatorGetSignatureLength", lv2::CELL_ENOTSUP},
    {"cellMediatorSign", lv2::CELL_ENOTSUP},
};

} // namespace

TEST_CASE("cellMediator: contexts, 1 to 32, made and closed", "[hle][medi]")
{
    Fixture f;
    GuestThread g(f, 0, "game");

    CHECK(g.call("cellMediatorCloseContext", 1) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellMediatorCreateContext") == 1);
    CHECK(g.call("cellMediatorCreateContext") == 2);
    CHECK(g.call("cellMediatorCloseContext", 1) == 0);
    CHECK(g.call("cellMediatorCloseContext", 1) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellMediatorCreateContext") == 1); // the lowest free ID again
    for (u32 id = 3; id <= 32; id++) {
        INFO(id);
        CHECK(g.call("cellMediatorCreateContext") == id);
    }
    CHECK(g.call("cellMediatorCreateContext") == r3_of_code(ID_NO_SPACE));
    CHECK(g.call("cellMediatorCloseContext", 17) == 0);
    CHECK(g.call("cellMediatorCreateContext") == 17);
    for (u32 id : {0u, 33u, 0x10000u, kData}) {
        INFO(id);
        CHECK(g.call("cellMediatorCloseContext", id) == r3_of_code(ID_NOT_FOUND));
    }
    for (u32 id = 1; id <= 32; id++) {
        INFO(id);
        CHECK(g.call("cellMediatorCloseContext", id) == 0);
    }

    CHECK(registry().is_firmware_first("libmedi"));
}

TEST_CASE("cellMediator: everything a context does needs PSN, or the console's key; nothing is written", "[hle][medi]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kBuf = kData + 0x100;
    std::memset(b + kBuf, 0x5A, 0x200);

    const u64 id = g.call("cellMediatorCreateContext");
    REQUIRE(id == 1);
    for (const PointerCall& c : kPointerCalls) {
        INFO(c.name);
        CHECK(g.call(c.name, id, kBuf) == r3_of_code(c.answer));
        CHECK(g.call(c.name, id, kBuf) == r3_of_code(c.answer)); // the same each time
        CHECK(g.call(c.name, id, 0) == r3_of_code(INVALID_ARGUMENT));
        CHECK(g.call(c.name, id, kNotGuest) == r3_of_code(INVALID_ARGUMENT));
        // A context not open comes first.
        CHECK(g.call(c.name, 2, kBuf) == r3_of_code(ID_NOT_FOUND));
        CHECK(g.call(c.name, 0, 0) == r3_of_code(ID_NOT_FOUND));
    }
    CHECK(g.call("cellMediatorFlushCache", id) == r3_of_code(OFFLINE));
    CHECK(g.call("cellMediatorFlushCache", 2) == r3_of_code(ID_NOT_FOUND));
    for (u32 i = 0; i < 0x200; i++) {
        INFO(i);
        CHECK(ps_read8(b, kBuf + i) == 0x5A);
    }

    // Closed, the context is gone for every call.
    REQUIRE(g.call("cellMediatorCloseContext", id) == 0);
    for (const PointerCall& c : kPointerCalls) {
        INFO(c.name);
        CHECK(g.call(c.name, id, kBuf) == r3_of_code(ID_NOT_FOUND));
    }
    CHECK(g.call("cellMediatorFlushCache", id) == r3_of_code(ID_NOT_FOUND));
}

TEST_CASE("cellMediator: each process its own contexts", "[hle][medi]")
{
    {
        Fixture f;
        GuestThread g(f, 0, "game");
        CHECK(g.call("cellMediatorCreateContext") == 1);
        CHECK(g.call("cellMediatorCreateContext") == 2);
    }
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("cellMediatorCloseContext", 2) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellMediatorCreateContext") == 1);
}
