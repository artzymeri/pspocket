// cellRudp's life (D-607): Init with the library's allocator and with the game's, End, the event handler, their
// errors as RPCS3 has them; the rest of the library is Sony's (firmware-first). Synthetic: no game data.

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 NOT_INITIALIZED = 0x80770001, ALREADY_INITIALIZED = 0x80770002, INVALID_ARGUMENT = 0x80770004;

} // namespace

TEST_CASE("cellRudp: Init, End and the event handler as RPCS3 has them", "[hle][rudp]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kAllocator = kData + 0x100, kNotGuest = 0xF0000000u;

    // Nothing before Init.
    CHECK(g.call("cellRudpEnd") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellRudpSetEventHandler", 0x10200, kData) == r3_of_code(NOT_INITIALIZED));

    // The library's own allocator; one Init only.
    CHECK(g.call("cellRudpInit", 0) == 0);
    CHECK(g.call("cellRudpInit", 0) == r3_of_code(ALREADY_INITIALIZED));
    CHECK(g.call("cellRudpSetEventHandler", 0x10200, kData) == 0);
    CHECK(g.call("cellRudpSetEventHandler", 0, 0) == 0); // no handler
    CHECK(g.call("cellRudpEnd") == 0);
    CHECK(g.call("cellRudpEnd") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellRudpSetEventHandler", 0x10200, kData) == r3_of_code(NOT_INITIALIZED));

    // The game's allocator: two descriptors.
    ps_write32(b, kAllocator, 0x10300);
    ps_write32(b, kAllocator + 4, 0x10310);
    CHECK(g.call("cellRudpInit", kAllocator) == 0);
    CHECK(g.call("cellRudpInit", kAllocator) == r3_of_code(ALREADY_INITIALIZED));
    CHECK(g.call("cellRudpInit", kNotGuest) == r3_of_code(ALREADY_INITIALIZED)); // RPCS3 checks that first
    CHECK(g.call("cellRudpEnd") == 0);

    // An allocator that is not guest memory initialises nothing.
    CHECK(g.call("cellRudpInit", kNotGuest) == r3_of_code(INVALID_ARGUMENT));
    CHECK(g.call("cellRudpEnd") == r3_of_code(NOT_INITIALIZED));

    // Firmware-first, the rest of the library Sony's.
    CHECK(registry().is_firmware_first("cellRudp"));
    for (const char* name : {"cellRudpCreateContext", "cellRudpBind", "cellRudpRead", "cellRudpWrite"}) {
        INFO(name);
        CHECK(GuestThread::lookup(name) == nullptr);
    }
}

TEST_CASE("cellRudp: each process its own library", "[hle][rudp]")
{
    {
        Fixture f;
        GuestThread g(f, 0, "game");
        CHECK(g.call("cellRudpInit", 0) == 0);
    }
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("cellRudpEnd") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellRudpInit", 0) == 0);
}
