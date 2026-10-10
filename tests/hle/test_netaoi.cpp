// cellNetAoi as a console with no ad hoc wireless and no PSN (D-635): Init and Term, the link that never comes up, the
// peers the game names, every error, a process each. Synthetic: no game data.

#include <cstring>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 NOT_INITIALIZED = 0x80130101, NOT_TERMINATED = 0x80130102, HANDLER_MAX = 0x80130103,
              ID_NOT_FOUND = 0x80130104, INVALID_ADDR = 0x80130107, NOT_CONNECTED = 0x80130108;
constexpr u32 kNotGuest = 0xF0000000u;

// The peer `n`'s MAC address at `addr`.
void put_mac(u8* base, u32 addr, u8 n)
{
    const u8 mac[6] = {0x00, 0x1F, 0xA7, 0x10, 0x20, n};
    std::memcpy(base + addr, mac, sizeof mac);
}

} // namespace

TEST_CASE("cellNetAoi: nothing before Init; Init and Term bracket the library", "[hle][netaoi]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kPeer = kData + 0x100, kInfo = kData + 0x200;
    put_mac(b, kPeer, 1);

    CHECK(g.call("cellNetAoiTerm") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiStart") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiStop") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiAddPeer", kPeer) == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiDeletePeer", kPeer) == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiGetLocalInfo", kInfo) == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", kPeer, kInfo) == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiGetPspTitleId", kInfo) == r3_of_code(NOT_INITIALIZED));
    // Before Init even a null pointer is the state's answer.
    CHECK(g.call("cellNetAoiAddPeer", 0) == r3_of_code(NOT_INITIALIZED));

    CHECK(g.call("cellNetAoiInit") == 0);
    CHECK(g.call("cellNetAoiInit") == r3_of_code(NOT_TERMINATED));
    CHECK(g.call("cellNetAoiTerm") == 0);
    CHECK(g.call("cellNetAoiTerm") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiStart") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiInit") == 0); // again after Term
    CHECK(g.call("cellNetAoiTerm") == 0);

    CHECK(registry().is_firmware_first("cellNetAoi"));
}

TEST_CASE("cellNetAoi: the link never comes up, and nothing is written", "[hle][netaoi]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kPeer = kData + 0x100, kInfo = kData + 0x200;
    put_mac(b, kPeer, 1);
    std::memset(b + kInfo, 0xA5, 0x100);

    REQUIRE(g.call("cellNetAoiInit") == 0);
    CHECK(g.call("cellNetAoiStart") == r3_of_code(NOT_CONNECTED));
    CHECK(g.call("cellNetAoiStart") == r3_of_code(NOT_CONNECTED)); // the same each time
    CHECK(g.call("cellNetAoiStop") == r3_of_code(NOT_CONNECTED));  // nothing was started

    CHECK(g.call("cellNetAoiGetLocalInfo", kInfo) == r3_of_code(NOT_CONNECTED));
    CHECK(g.call("cellNetAoiGetLocalInfo", 0) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiGetLocalInfo", kNotGuest) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiGetPspTitleId", kInfo) == r3_of_code(NOT_CONNECTED));
    CHECK(g.call("cellNetAoiGetPspTitleId", 0) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiGetPspTitleId", kNotGuest) == r3_of_code(INVALID_ADDR));

    // A peer the game added: the link never saw it.
    REQUIRE(g.call("cellNetAoiAddPeer", kPeer) == 0);
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", kPeer, kInfo) == r3_of_code(NOT_CONNECTED));
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", kPeer, 0) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", kPeer, kNotGuest) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", 0, kInfo) == r3_of_code(INVALID_ADDR));

    for (u32 i = 0; i < 0x100; i++) {
        INFO(i);
        CHECK(ps_read8(b, kInfo + i) == 0xA5);
    }
    CHECK(g.call("cellNetAoiTerm") == 0);
}

TEST_CASE("cellNetAoi: the peers the game names, by MAC address, sixteen at most", "[hle][netaoi]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kPeers = kData + 0x1000, kOther = kData + 0x2000, kInfo = kData + 0x3000;
    for (u8 i = 0; i < 20; i++) put_mac(b, kPeers + 8 * i, i);
    const auto peer = [&](u32 i) { return kPeers + 8 * i; };

    REQUIRE(g.call("cellNetAoiInit") == 0);
    CHECK(g.call("cellNetAoiAddPeer", 0) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiAddPeer", kNotGuest) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiDeletePeer", 0) == r3_of_code(INVALID_ADDR));
    CHECK(g.call("cellNetAoiDeletePeer", kNotGuest) == r3_of_code(INVALID_ADDR));

    CHECK(g.call("cellNetAoiDeletePeer", peer(0)) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", peer(0), kInfo) == r3_of_code(ID_NOT_FOUND));
    REQUIRE(g.call("cellNetAoiAddPeer", peer(0)) == 0);
    CHECK(g.call("cellNetAoiAddPeer", peer(0)) == 0); // already there: nothing changes

    // The same address from another buffer is the same peer.
    put_mac(b, kOther, 0);
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", kOther, kInfo) == r3_of_code(NOT_CONNECTED));
    REQUIRE(g.call("cellNetAoiDeletePeer", kOther) == 0);
    CHECK(g.call("cellNetAoiDeletePeer", peer(0)) == r3_of_code(ID_NOT_FOUND)); // added twice, kept once

    // Sixteen peers; the seventeenth is refused until one goes.
    for (u32 i = 0; i < 16; i++) {
        INFO(i);
        REQUIRE(g.call("cellNetAoiAddPeer", peer(i)) == 0);
    }
    CHECK(g.call("cellNetAoiAddPeer", peer(16)) == r3_of_code(HANDLER_MAX));
    CHECK(g.call("cellNetAoiAddPeer", peer(15)) == 0); // one already kept still answers OK
    REQUIRE(g.call("cellNetAoiDeletePeer", peer(7)) == 0);
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", peer(7), kInfo) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellNetAoiAddPeer", peer(16)) == 0);
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", peer(16), kInfo) == r3_of_code(NOT_CONNECTED));
    CHECK(g.call("cellNetAoiAddPeer", peer(17)) == r3_of_code(HANDLER_MAX));

    // Term forgets them.
    REQUIRE(g.call("cellNetAoiTerm") == 0);
    REQUIRE(g.call("cellNetAoiInit") == 0);
    CHECK(g.call("cellNetAoiDeletePeer", peer(1)) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellNetAoiGetRemotePeerInfo", peer(16), kInfo) == r3_of_code(ID_NOT_FOUND));
    CHECK(g.call("cellNetAoiTerm") == 0);
}

TEST_CASE("cellNetAoi: each process its own library", "[hle][netaoi]")
{
    {
        Fixture f;
        GuestThread g(f, 0, "game");
        CHECK(g.call("cellNetAoiInit") == 0);
    }
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("cellNetAoiTerm") == r3_of_code(NOT_INITIALIZED));
    CHECK(g.call("cellNetAoiInit") == 0);
}
