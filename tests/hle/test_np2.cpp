// sceNp2 offline (synthetic: no game data): NP 2's init and term, and matching 2 with no user signed in, as RPCS3. It
// initializes, but no context can be created, so every function that names one finds none after RPCS3's checks before
// that lookup, no request gets a request ID, and only the room callbacks, which need no context, are kept. Also the
// registry entries of the sceNp2 and sceNp functions GTA V imports, in the modules of their import libraries.

#include "hle_test_util.hpp"

#include <array>
#include <cstring>
#include <string_view>

#include "np.hpp" // runtime/hle/src (internal): NP's state for the other NP modules

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kNotInit = 0xFFFFFFFF8002AA01ull, kAlready = 0xFFFFFFFF8002AA02ull, kInval = 0xFFFFFFFF8002AA03ull,
              kInsufficient = 0xFFFFFFFF8002AA0Full;
constexpr u64 kM2Already = 0xFFFFFFFF80022302ull, kM2NotInit = 0xFFFFFFFF80022303ull,
              kM2CtxNotFound = 0xFFFFFFFF80022306ull, kM2Inval = 0xFFFFFFFF8002230Aull,
              kM2InvalCtx = 0xFFFFFFFF8002230Bull, kM2NotSignIn = 0xFFFFFFFF80022334ull;

constexpr u32 kPool = kData + 0x40000; // NP's 128 KB pool

// The requests to the matching server: (context, request parameters, options, where the request ID goes).
constexpr std::array<std::string_view, 9> kRequests = {
    "sceNpMatching2GetWorldInfoList",
    "sceNpMatching2GetServerInfo",
    "sceNpMatching2SearchRoom",
    "sceNpMatching2CreateJoinRoom",
    "sceNpMatching2JoinRoom",
    "sceNpMatching2LeaveRoom",
    "sceNpMatching2SetRoomDataExternal",
    "sceNpMatching2SetRoomDataInternal",
    "sceNpMatching2GetRoomMemberDataExternalList",
};

} // namespace

TEST_CASE("sceNp2, sceNp: the functions GTA V imports, in their modules", "[hle][np]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"sceNp2", "sceNp2Init", 0x41251F74},
        {"sceNp2", "sceNp2Term", 0xAADB7C12},
        {"sceNp2", "sceNpMatching2DestroyContext", 0x01379FD7},
        {"sceNp2", "sceNpMatching2RegisterLobbyMessageCallback", 0x03A2F42A},
        {"sceNp2", "sceNpMatching2GetWorldInfoList", 0x05BF2FBD},
        {"sceNp2", "sceNpMatching2RegisterLobbyEventCallback", 0x0884FC95},
        {"sceNp2", "sceNpMatching2SearchRoom", 0x0D22867F},
        {"sceNp2", "sceNpMatching2LeaveRoom", 0x1F372697},
        {"sceNp2", "sceNpMatching2SetRoomDataExternal", 0x215B0D75},
        {"sceNp2", "sceNpMatching2Term2", 0x23009D07},
        {"sceNp2", "sceNpMatching2GetRoomMemberDataExternalList", 0x2C5B7FC9},
        {"sceNp2", "sceNpMatching2AbortRequest", 0x2ED909DC},
        {"sceNp2", "sceNpMatching2GetServerInfo", 0x3457C0DB},
        {"sceNp2", "sceNpMatching2GetEventData", 0x3C00C9D4},
        {"sceNp2", "sceNpMatching2JoinRoom", 0x5721E711},
        {"sceNp2", "sceNpMatching2ContextStartAsync", 0x6BA4C668},
        {"sceNp2", "sceNpMatching2RegisterContextCallback", 0x748029A2},
        {"sceNp2", "sceNpMatching2SetRoomDataInternal", 0x817D1090},
        {"sceNp2", "sceNpMatching2GetServerIdListLocal", 0x8E5CFE9F},
        {"sceNp2", "sceNpMatching2CreateContext", 0x9CBCE3F2},
        {"sceNp2", "sceNpMatching2RegisterSignalingCallback", 0xA9E6103E},
        {"sceNp2", "sceNpMatching2ClearEventData", 0xAA6C2C43},
        {"sceNp2", "sceNpMatching2RegisterRoomEventCallback", 0xDCB6B27D},
        {"sceNp2", "sceNpMatching2CreateJoinRoom", 0xE7A3BC7A},
        {"sceNp2", "sceNpMatching2ContextStop", 0xF43C647A},
        {"sceNp2", "sceNpMatching2Init2", 0xF4BABD3F},
        {"sceNp2", "sceNpMatching2RegisterRoomMessageCallback", 0xFEE11E72},
        {"sceNp", "sceNpBasicSendMessageGui", 0x01FBBC9B},
        {"sceNp", "sceNpBasicAddFriend", 0x27C69EBA},
        {"sceNp", "sceNpSignalingActivateConnection", 0x60897C38},
        {"sceNp", "sceNpBasicRecvMessageCustom", 0x806960AB},
        {"sceNp", "sceNpDrmIsAvailable", 0xAD218FAF},
        {"sceNp", "sceNpBasicAddPlayersHistoryAsync", 0xBCDBB2AB},
        {"sceNp", "sceNpProfileCallGui", 0xCEEEBC7A},
        {"sceNp", "sceNpBasicSendMessage", 0xEC0A1FBF},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("sceNp2Init, sceNp2Term: NP's pool checks; NP 2 once; NP up in between", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np2");
    CHECK(g.call("sceNp2Term") == kNotInit);
    CHECK(g.call("sceNp2Init", 0, kPool) == kInval);
    CHECK(g.call("sceNp2Init", 0x20000, 0) == kInval);
    CHECK(g.call("sceNp2Init", 0x1FFFF, kPool) == kInsufficient);
    CHECK_FALSE(hle::np_initialized());
    REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0);
    CHECK(hle::np_initialized());
    CHECK(g.call("sceNp2Init", 0x20000, kPool) == kAlready);
    CHECK(hle::np_initialized());
    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK_FALSE(hle::np_initialized());
    CHECK(g.call("sceNp2Term") == kNotInit);
    CHECK(g.call("sceNpManagerGetNpId", kData) == kNotInit); // NP itself is down too
    REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0);
    CHECK(hle::np_initialized());
}

TEST_CASE("sceNpMatching2: up and down with NP 2; no context offline, so nothing reaches a server", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np2");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kComm = kData + 0x140, kPass = kData + 0x160, kCtxOut = kData + 0x300,
                  kReq = kData + 0x400, kOpt = kData + 0x480, kReqId = kData + 0x500, kBuf = kData + 0x600,
                  kParam = kData + 0x700;
    constexpr u32 kCb = 0x00012340, kArg = 0x5678;
    std::memcpy(b + kComm, "NPWR01234", 9);

    // Before NP 2: init and term name NP; everything else names matching 2.
    CHECK(g.call("sceNpMatching2Init2", 0x4000, 1000, kParam) == kNotInit);
    CHECK(g.call("sceNpMatching2Term2") == kNotInit);
    CHECK(g.call("sceNpMatching2CreateContext", kNpId, kComm, kPass, kCtxOut, 0) == kM2NotInit);
    CHECK(g.call("sceNpMatching2DestroyContext", 1) == kM2NotInit);
    CHECK(g.call("sceNpMatching2ContextStartAsync", 1, 10) == kM2NotInit);
    CHECK(g.call("sceNpMatching2ContextStop", 1) == kM2NotInit);
    CHECK(g.call("sceNpMatching2RegisterContextCallback", 1, kCb, kArg) == kM2NotInit);
    CHECK(g.call("sceNpMatching2RegisterSignalingCallback", 1, kCb, kArg) == kM2NotInit);
    CHECK(g.call("sceNpMatching2RegisterRoomEventCallback", 1, kCb, kArg) == kM2NotInit);
    CHECK(g.call("sceNpMatching2RegisterRoomMessageCallback", 1, kCb, kArg) == kM2NotInit);
    CHECK(g.call("sceNpMatching2RegisterLobbyEventCallback", 1, kCb, kArg) == kM2NotInit);
    CHECK(g.call("sceNpMatching2RegisterLobbyMessageCallback", 1, kCb, kArg) == kM2NotInit);
    for (std::string_view name : kRequests) {
        INFO(name);
        CHECK(g.call(name, 1, kReq, kOpt, kReqId) == kM2NotInit);
    }
    CHECK(g.call("sceNpMatching2AbortRequest", 1, 0x33330001) == kM2NotInit);
    CHECK(g.call("sceNpMatching2GetServerIdListLocal", 1, kBuf, 4) == kM2NotInit);
    CHECK(g.call("sceNpMatching2GetEventData", 1, 1, kBuf, 16) == kM2NotInit);
    CHECK(g.call("sceNpMatching2ClearEventData", 1, 1) == kM2NotInit);

    // NP 2 up: matching 2 isn't yet; then once.
    REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0);
    CHECK(g.call("sceNpMatching2Term2") == kM2NotInit);
    CHECK(g.call("sceNpMatching2CreateContext", kNpId, kComm, kPass, kCtxOut, 0) == kM2NotInit);
    REQUIRE(g.call("sceNpMatching2Init2", 0x4000, 1000, 0) == 0);
    CHECK(g.call("sceNpMatching2Init2", 0x4000, 1000, kParam) == kM2Already);

    // No user is signed in: every pointer is checked, then no context is made and none is written.
    ps_write32(b, kCtxOut, 0xAAAA);
    CHECK(g.call("sceNpMatching2CreateContext", 0, kComm, kPass, kCtxOut, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2CreateContext", kNpId, 0, kPass, kCtxOut, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2CreateContext", kNpId, kComm, 0, kCtxOut, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2CreateContext", kNpId, kComm, kPass, 0, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2CreateContext", kNpId, kComm, kPass, kCtxOut, 3) == kM2NotSignIn);
    CHECK(ps_read32(b, kCtxOut) == 0xAAAA);

    // So the context lifecycle finds no context. Start and the context callback check the ID first; context IDs are
    // 16 bits (0x10000 reads as 0).
    CHECK(g.call("sceNpMatching2ContextStartAsync", 0, 10) == kM2InvalCtx);
    CHECK(g.call("sceNpMatching2ContextStartAsync", 0x10000, 10) == kM2InvalCtx);
    CHECK(g.call("sceNpMatching2ContextStartAsync", 1, 10) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2ContextStop", 0) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2ContextStop", 1) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2DestroyContext", 0) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2DestroyContext", 1) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2DestroyContext", 255) == kM2CtxNotFound);

    // Callbacks: the context and signaling callbacks belong to a context; the room callbacks are kept without one;
    // the lobby callbacks are RPCS3 TODOs that keep nothing.
    CHECK(g.call("sceNpMatching2RegisterContextCallback", 0, kCb, kArg) == kM2InvalCtx);
    CHECK(g.call("sceNpMatching2RegisterContextCallback", 1, kCb, kArg) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2RegisterSignalingCallback", 0, kCb, kArg) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2RegisterSignalingCallback", 1, kCb, kArg) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2RegisterRoomEventCallback", 1, kCb, kArg) == 0);
    CHECK(g.call("sceNpMatching2RegisterRoomEventCallback", 0, 0, 0) == 0);
    CHECK(g.call("sceNpMatching2RegisterRoomMessageCallback", 7, kCb, kArg) == 0);
    CHECK(g.call("sceNpMatching2RegisterLobbyEventCallback", 0, kCb, kArg) == 0);
    CHECK(g.call("sceNpMatching2RegisterLobbyMessageCallback", 0, kCb, kArg) == 0);

    // Requests: the parameters, a context ID, then no context; no request ID is written.
    for (std::string_view name : kRequests) {
        INFO(name);
        ps_write32(b, kReqId, 0xAAAAAAAA);
        CHECK(g.call(name, 1, 0, kOpt, kReqId) == kM2Inval);
        CHECK(g.call(name, 0, 0, kOpt, kReqId) == kM2Inval); // the parameters before the context ID
        CHECK(g.call(name, 0, kReq, kOpt, kReqId) == kM2InvalCtx);
        CHECK(g.call(name, 1, kReq, 0, kReqId) == kM2CtxNotFound);
        CHECK(g.call(name, 2, kReq, kOpt, 0) == kM2CtxNotFound);
        CHECK(ps_read32(b, kReqId) == 0xAAAAAAAA);
    }
    CHECK(g.call("sceNpMatching2AbortRequest", 0, 0x33330001) == kM2InvalCtx);
    CHECK(g.call("sceNpMatching2AbortRequest", 1, 0x33330001) == kM2CtxNotFound);
    std::memset(b + kBuf, 0xAA, 16);
    CHECK(g.call("sceNpMatching2GetServerIdListLocal", 0, kBuf, 4) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2GetServerIdListLocal", 1, kBuf, 4) == kM2CtxNotFound);
    CHECK(b[kBuf] == 0xAA);

    // Event data: a buffer is needed; no data is ever left under a key, so 0 bytes are copied, whatever the context.
    CHECK(g.call("sceNpMatching2GetEventData", 1, 1, 0, 16) == kM2Inval);
    CHECK(g.call("sceNpMatching2GetEventData", 1, 1, kBuf, 16) == 0);
    CHECK(g.call("sceNpMatching2GetEventData", 0, 0x1234, kBuf, 16) == 0);
    CHECK(b[kBuf] == 0xAA);
    CHECK(g.call("sceNpMatching2ClearEventData", 1, 1) == 0);

    // Matching 2 down and up again, then NP 2's term takes matching 2 down with it.
    REQUIRE(g.call("sceNpMatching2Term2") == 0);
    CHECK(g.call("sceNpMatching2Term2") == kM2NotInit);
    CHECK(g.call("sceNpMatching2GetEventData", 1, 1, kBuf, 16) == kM2NotInit);
    REQUIRE(g.call("sceNpMatching2Init2", 0x4000, 1000, 0) == 0);
    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(g.call("sceNpMatching2Init2", 0x4000, 1000, 0) == kNotInit);
    CHECK(g.call("sceNpMatching2Term2") == kNotInit);
    CHECK(g.call("sceNpMatching2RegisterRoomEventCallback", 1, kCb, kArg) == kM2NotInit);
    REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0);
    CHECK(g.call("sceNpMatching2Term2") == kM2NotInit);
    REQUIRE(g.call("sceNpMatching2Init2", 0x4000, 1000, 0) == 0);
}
