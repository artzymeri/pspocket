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

    // Matching 2 down and up again (sceNpMatching2Term is Term2's older name), then NP 2's term takes matching 2 down
    // with it.
    REQUIRE(g.call("sceNpMatching2Term2") == 0);
    CHECK(g.call("sceNpMatching2Term2") == kM2NotInit);
    CHECK(g.call("sceNpMatching2Term") == kM2NotInit);
    REQUIRE(g.call("sceNpMatching2Init2", 0x4000, 1000, 0) == 0);
    REQUIRE(g.call("sceNpMatching2Term") == 0);
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

TEST_CASE("sceNpMatching2's other requests, local queries and signaling; OAuth; the CDN URL: RPCS3's checks offline",
    "[hle][np]")
{
    constexpr u64 kM2InvalRoom = 0xFFFFFFFF8002230Full, kM2InvalMember = 0xFFFFFFFF80022310ull,
                  kM2InvalAttr = 0xFFFFFFFF80022311ull, kM2InvalSort = 0xFFFFFFFF80022313ull,
                  kM2ServiceUnavailable = 0xFFFFFFFF80022B02ull, kOAuthAlready = 0xFFFFFFFF80025F02ull,
                  kOAuthNotInit = 0xFFFFFFFF80025F03ull, kUtilInval = 0xFFFFFFFF8002AB01ull;
    Fixture f;
    GuestThread g(f, 0, "np2");
    u8* b = f.base();
    constexpr u32 kReq = kData + 0x100, kOut = kData + 0x200, kAttrs = kData + 0x400;
    constexpr u64 kRoom = 0x1122334455667788ull;
    const std::string_view requests[] = {"sceNpMatching2CreateServerContext", "sceNpMatching2DeleteServerContext",
        "sceNpMatching2GetLobbyInfoList", "sceNpMatching2GetLobbyMemberDataInternal",
        "sceNpMatching2GetLobbyMemberDataInternalList", "sceNpMatching2GetRoomDataExternalList",
        "sceNpMatching2GetRoomDataInternal", "sceNpMatching2GetRoomMemberDataInternal", "sceNpMatching2GetUserInfoList",
        "sceNpMatching2GrantRoomOwner", "sceNpMatching2JoinLobby", "sceNpMatching2JoinProhibitiveRoom",
        "sceNpMatching2KickoutRoomMember", "sceNpMatching2LeaveLobby", "sceNpMatching2SendLobbyChatMessage",
        "sceNpMatching2SendLobbyInvitation", "sceNpMatching2SendRoomChatMessage", "sceNpMatching2SendRoomMessage",
        "sceNpMatching2SetLobbyMemberDataInternal", "sceNpMatching2SetRoomMemberDataInternal",
        "sceNpMatching2SetSignalingOptParam", "sceNpMatching2SetUserInfo", "sceNpMatching2SignalingGetPingInfo"};
    for (const std::string_view name : requests) CHECK(g.call(name, 1, kReq, 0, kOut) == kM2NotInit);
    CHECK(g.call("sceNpMatching2GetMemoryInfo", kOut) == kM2NotInit);
    CHECK(g.call("sceNpMatching2Init", 0x4000, 1000) == kNotInit); // needs NP 2
    REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0);
    REQUIRE(g.call("sceNpMatching2Init", 0x4000, 1000) == 0); // sceNpMatching2Init2 without its parameter
    CHECK(g.call("sceNpMatching2Init2", 0, 0, 0) == kM2Already);

    // Requests: the parameters, a context ID, then the context: none offline. No request ID is written.
    std::memset(b + kOut, 0xAA, 0x40);
    for (const std::string_view name : requests) {
        INFO(name);
        CHECK(g.call(name, 1, 0, 0, kOut) == kM2Inval);
        CHECK(g.call(name, 0, kReq, 0, kOut) == kM2InvalCtx);
        CHECK(g.call(name, 1, kReq, 0, kOut) == kM2CtxNotFound);
    }
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpMatching2ContextStart", 1) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2AbortContextStart", 1) == 0);
    CHECK(g.call("sceNpMatching2SetDefaultRequestOptParam", 1, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2SetDefaultRequestOptParam", 1, kReq) == kM2InvalCtx);

    // Queues and memory: answered without a context.
    CHECK(g.call("sceNpMatching2GetCbQueueInfo", 1, 0) == kM2Inval);
    std::memset(b + kOut, 0xAA, 0x40);
    REQUIRE(g.call("sceNpMatching2GetCbQueueInfo", 1, kOut) == 0);
    for (u32 i = 0; i < 9; i++) CHECK(ps_read32(b, kOut + 4 * i) == (i % 3 == 0 ? 255u : 0u));
    CHECK(ps_read32(b, kOut + 36) == 0);
    CHECK(ps_read32(b, kOut + 48) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpMatching2GetMemoryInfo", 0) == kM2Inval);
    std::memset(b + kOut, 0xAA, 0x40);
    REQUIRE(g.call("sceNpMatching2GetMemoryInfo", kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x20000); // NP's pool, none of it in use
    CHECK(ps_read32(b, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut + 8) == 0);
    CHECK(ps_read32(b, kOut + 24) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpMatching2GetClanLobbyId", 1, 7, kOut) == kM2ServiceUnavailable);
    CHECK(g.call("sceNpMatching2GetLobbyMemberIdListLocal", 1, 0x99ull, kOut, 4, kOut) == 0);

    // Local room queries: their IDs, then the context.
    CHECK(g.call("sceNpMatching2GetRoomMemberIdListLocal", 0, kRoom, 0, kOut, 4) == kM2InvalCtx);
    CHECK(g.call("sceNpMatching2GetRoomMemberIdListLocal", 1, 0, 0, kOut, 4) == kM2InvalRoom);
    CHECK(g.call("sceNpMatching2GetRoomMemberIdListLocal", 1, kRoom, 2, kOut, 4) == kM2InvalSort);
    CHECK(g.call("sceNpMatching2GetRoomMemberIdListLocal", 1, kRoom, 1, kOut, 4) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2GetRoomPasswordLocal", 1, 0, kOut, kOut) == kM2InvalRoom);
    CHECK(g.call("sceNpMatching2GetRoomPasswordLocal", 1, kRoom, kOut, kOut) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2GetRoomSlotInfoLocal", 1, kRoom, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2GetRoomSlotInfoLocal", 0, kRoom, kOut) == kM2InvalCtx);
    CHECK(g.call("sceNpMatching2GetRoomSlotInfoLocal", 1, kRoom, kOut) == kM2CtxNotFound);
    CHECK(g.call("sceNpMatching2GetSignalingOptParamLocal", 1, kRoom, kOut) == kM2CtxNotFound);
    const auto member_data = [&](u16 member, u32 attrs, u32 count) {
        return g.call("sceNpMatching2GetRoomMemberDataInternalLocal", 1, kRoom, member, attrs, count, kOut, kOut, 64);
    };
    CHECK(member_data(0, 0, 0) == kM2InvalMember);
    CHECK(member_data(2, 0, 1) == kM2InvalAttr);
    ps_write16(b, kAttrs, 0x59);
    ps_write16(b, kAttrs + 2, 0x5F); // a user attribute
    CHECK(member_data(2, kAttrs, 2) == kM2InvalAttr);
    ps_write16(b, kAttrs + 2, 0x5E);
    CHECK(member_data(2, kAttrs, 2) == kM2CtxNotFound);

    // Matching 2 signaling: no room is known offline.
    std::memset(b + kOut, 0xAA, 0x40);
    CHECK(g.call("sceNpMatching2SignalingGetConnectionStatus", 1, kRoom, 2, 0, 0, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2SignalingGetConnectionStatus", 1, kRoom, 2, kOut, 0, 0) == kM2InvalRoom);
    CHECK(ps_read32(b, kOut) == 0); // inactive
    CHECK(g.call("sceNpMatching2SignalingGetConnectionInfo", 1, kRoom, 2, 1, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2SignalingGetConnectionInfo", 1, kRoom, 2, 1, kOut) == kM2InvalRoom);
    std::memset(b + kOut, 0xAA, 0x40);
    ps_write32(b, kOut, 15);
    CHECK(g.call("sceNpMatching2SignalingGetLocalNetInfo", kOut) == kM2Inval); // the size must be 16
    ps_write32(b, kOut, 16);
    REQUIRE(g.call("sceNpMatching2SignalingGetLocalNetInfo", kOut) == 0);
    CHECK(ps_read32(b, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut + 8) == 0);
    CHECK(ps_read32(b, kOut + 12) == 2);
    CHECK(g.call("sceNpMatching2SignalingGetPeerNetInfo", 1, kRoom, 2, 0) == kM2Inval);
    CHECK(g.call("sceNpMatching2SignalingGetPeerNetInfo", 1, kRoom, 2, kOut + 32) == 0);
    CHECK(ps_read32(b, kOut + 32) == 0xAAAAAAAAu);
    ps_write32(b, kOut + 16, 12);
    CHECK(g.call("sceNpMatching2SignalingGetPeerNetInfoResult", 1, 5, kOut + 16) == kM2Inval);
    ps_write32(b, kOut + 16, 16);
    CHECK(g.call("sceNpMatching2SignalingGetPeerNetInfoResult", 1, 5, kOut + 16) == 0);
    CHECK(g.call("sceNpMatching2SignalingCancelPeerNetInfo", 1, 5) == 0);
    CHECK(g.call("sceNpMatching2SignalingGetCtxOpt", 1, 1, kOut) == 0);
    CHECK(g.call("sceNpMatching2SignalingSetCtxOpt", 1, 1, 1) == 0);

    // OAuth: the library's flag; its requests are RPCS3's TODOs.
    CHECK(g.call("sceNpAuthCreateOAuthRequest") == kOAuthNotInit);
    CHECK(g.call("sceNpAuthGetAuthorizationCode", 1, kReq, kOut, kOut) == kOAuthNotInit);
    REQUIRE(g.call("sceNpAuthOAuthInit") == 0);
    CHECK(g.call("sceNpAuthOAuthInit") == kOAuthAlready);
    CHECK(g.call("sceNpAuthCreateOAuthRequest") == 0);
    CHECK(g.call("sceNpAuthGetAuthorizationCode", 1, kReq, kOut, kOut) == 0);
    CHECK(g.call("sceNpAuthAbortOAuthRequest", 1) == 0);
    CHECK(g.call("sceNpAuthDeleteOAuthRequest", 1) == 0);
    REQUIRE(g.call("sceNpAuthOAuthTerm") == 0);
    CHECK(g.call("sceNpAuthOAuthTerm") == 0); // unchecked
    CHECK(g.call("sceNpAuthDeleteOAuthRequest", 1) == kOAuthNotInit);

    // A CDN URL: a URL, no option.
    CHECK(g.call("sceNpUtilBuildCdnUrl", 0, kOut, 64, kOut, 0) == kUtilInval);
    CHECK(g.call("sceNpUtilBuildCdnUrl", kReq, kOut, 64, kOut, kAttrs) == kUtilInval);
    CHECK(g.call("sceNpUtilBuildCdnUrl", kReq, kOut, 64, kOut, 0) == 0);

    // NP's term empties the pool.
    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(hle::np_pool_size() == 0);
}
