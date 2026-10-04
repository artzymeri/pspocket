// sceNp offline (synthetic: no game data): RPCS3's checks and results with no user signed in to PSN, for lookups,
// the manager's user queries and tickets, basic events and presence, signaling contexts, and NP ID comparison.

#include "hle_test_util.hpp"

#include <cstring>
#include <string_view>

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kNotInit = 0xFFFFFFFF8002AA01ull, kInval = 0xFFFFFFFF8002AA03ull, kIdNotFound = 0xFFFFFFFF8002AA06ull,
              kInvalState = 0xFFFFFFFF8002AA0Aull, kOffline = 0xFFFFFFFF8002AA0Cull, kAuthInval = 0xFFFFFFFF8002A015ull;
constexpr u64 kBasicNotInit = 0xFFFFFFFF8002A662ull, kBasicInval = 0xFFFFFFFF8002A665ull,
              kBasicNoEvent = 0xFFFFFFFF8002A66Aull, kBasicExceeds = 0xFFFFFFFF8002A66Bull,
              kBasicNotRegistered = 0xFFFFFFFF8002A66Dull, kBasicInvalDataId = 0xFFFFFFFF8002A673ull;
constexpr u64 kComAlready = 0xFFFFFFFF8002A101ull, kComNotInit = 0xFFFFFFFF8002A102ull,
              kComInval = 0xFFFFFFFF8002A104ull, kComTooMany = 0xFFFFFFFF8002A107ull,
              kComInsufficient = 0xFFFFFFFF8002A1A1ull, kComInvalId = 0xFFFFFFFF8002A1A3ull,
              kComInvalOnlineId = 0xFFFFFFFF8002A1A4ull;
constexpr u64 kSigNotInit = 0xFFFFFFFF8002A801ull, kSigCtxNotFound = 0xFFFFFFFF8002A805ull,
              kSigConnNotFound = 0xFFFFFFFF8002A80Eull, kSigCtxMax = 0xFFFFFFFF8002A812ull,
              kSigInval = 0xFFFFFFFF8002A815ull, kSigOwnNpId = 0xFFFFFFFF8002A816ull;
constexpr u64 kUtilInval = 0xFFFFFFFF8002AB01ull, kUtilNotMatch = 0xFFFFFFFF8002AB0Full;

constexpr u32 kPool = kData + 0x40000; // NP's 128 KB pool

void np_up(GuestThread& g) { REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0); }

void put(u8* b, u32 addr, std::string_view s)
{
    std::memcpy(b + addr, s.data(), s.size());
    b[addr + s.size()] = 0;
}

// A SceNpId (0x24 bytes) with this online ID and every other byte 0.
void put_np_id(u8* b, u32 addr, std::string_view online_id)
{
    std::memset(b + addr, 0, 0x24);
    std::memcpy(b + addr, online_id.data(), online_id.size());
}

// A SceNpCommunicationId (12 bytes): nine characters, the terminator, num.
void put_comm_id(u8* b, u32 addr, std::string_view data, u8 num)
{
    std::memset(b + addr, 0, 12);
    std::memcpy(b + addr, data.data(), data.size());
    b[addr + 10] = num;
}

} // namespace

TEST_CASE("sceNpLookup: init needs NP, title contexts in idm order, transactions refused offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kBadComm = kData + 0x110, kSelf = kData + 0x120, kOnline = kData + 0x160,
                  kNpId = kData + 0x180, kOpt = kData + 0x1C0, kBuf = kData + 0x200, kLen = kData + 0x300,
                  kResult = kData + 0x304;
    put_comm_id(b, kComm, "NPWR01234", 0);
    put_np_id(b, kSelf, "");
    put(b, kOnline, "somebody");

    // Before NP: lookup init needs NP; everything else needs lookup init first.
    CHECK(g.call("sceNpLookupInit") == kNotInit);
    CHECK(g.call("sceNpLookupTerm") == kComNotInit);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == kComNotInit);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3001) == kComNotInit);
    CHECK(g.call("sceNpLookupCreateTransactionCtx", 0x3001) == kComNotInit);
    CHECK(g.call("sceNpLookupDestroyTransactionCtx", 0x4001) == kComNotInit);
    CHECK(g.call("sceNpLookupSetTimeout", 0x3001, 20'000'000) == kComNotInit);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, kNpId, 0, 0) == kComNotInit);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, kBuf, 0x100, kLen, 0, 0) == kComNotInit);
    CHECK(g.call("sceNpLookupPollAsync", 0x4001, kResult) == kComNotInit);

    np_up(g);
    REQUIRE(g.call("sceNpLookupInit") == 0);
    CHECK(g.call("sceNpLookupInit") == kComAlready);

    // Title contexts: the ID is the result, from 0x3001; a valid communication ID and an NP ID pointer are needed.
    CHECK(g.call("sceNpLookupCreateTitleCtx", 0, kSelf) == kComInsufficient);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, 0) == kComInsufficient);
    put_comm_id(b, kBadComm, "npwr01234", 0);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kBadComm, kSelf) == kComInval); // RPCS3 aborts
    put_comm_id(b, kBadComm, "NPWR01234", 100);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kBadComm, kSelf) == kComInval);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3001);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3002);
    REQUIRE(g.call("sceNpLookupDestroyTitleCtx", 0x3001) == 0);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3001) == kComInvalId);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3000) == kComInvalId);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", -1) == kComInvalId);
    // As RPCS3's idm: new IDs until each of the 32 was handed out once, then the lowest free one, then none.
    for (u32 id = 0x3003; id <= 0x3020; id++) REQUIRE(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == id);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3001);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == kComTooMany);
    REQUIRE(g.call("sceNpLookupDestroyTitleCtx", 0x3010) == 0);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3010);

    // Transactions need PSN: none is made (whatever the title context), so none can be destroyed.
    CHECK(g.call("sceNpLookupCreateTransactionCtx", 0x3002) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupCreateTransactionCtx", 0x7777) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupDestroyTransactionCtx", 0x4001) == kComInvalId);

    // The timeout is a 64-bit number of microseconds, at least 10 s; the context isn't checked.
    CHECK(g.call("sceNpLookupSetTimeout", 0x3002, 9'999'999) == kComInval);
    CHECK(g.call("sceNpLookupSetTimeout", 0x3002, 10'000'000) == 0);
    CHECK(g.call("sceNpLookupSetTimeout", 0x7777, 0x100000000ull) == 0);

    // Lookups: arguments checked, then refused offline with nothing written.
    std::memset(b + kNpId, 0xAA, 0x24);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, 0, kNpId, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, 0, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, kNpId, 0, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, kNpId, 0, 0) == kComInvalOnlineId);
    CHECK(b[kNpId] == 0xAA);
    std::memset(b + kBuf, 0xAA, 0x100);
    ps_write32(b, kLen, 0x55);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, 0, 0x100, kLen, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, kBuf, 0x100, kLen, 0, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, kBuf, 0x100, kLen, 0, 0) == kComInvalOnlineId);
    CHECK(b[kBuf] == 0xAA);
    CHECK(ps_read32(b, kLen) == 0x55);
    // Polling reports any transaction finished with result 0.
    ps_write32(b, kResult, 0xFFFFFFFF);
    REQUIRE(g.call("sceNpLookupPollAsync", 0x4001, kResult) == 0);
    CHECK(ps_read32(b, kResult) == 0);

    // NP term leaves the lookup library and its title contexts, as in RPCS3; lookup term itself needs NP.
    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(g.call("sceNpLookupTerm") == kNotInit);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3002) == 0);
    np_up(g);
    CHECK(g.call("sceNpLookupInit") == kComAlready);
    REQUIRE(g.call("sceNpLookupTerm") == 0);
    CHECK(g.call("sceNpLookupTerm") == kComNotInit);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3003) == kComNotInit);
}

TEST_CASE("sceNpManager: the user's details and tickets are not available offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kOut1 = kData + 0x100, kOut2 = kData + 0x110, kNpId = kData + 0x120, kService = kData + 0x160,
                  kBuf = kData + 0x200, kSize = kData + 0x300, kParam = kData + 0x400, kEnt = kData + 0x600,
                  kEntId = kData + 0x700;
    put(b, kService, "UP0001-NPWR01234_00");
    put(b, kEntId, "SOMEDLC");

    CHECK(g.call("sceNpManagerGetNpId", kNpId) == kNotInit);
    CHECK(g.call("sceNpManagerGetAccountRegion", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpManagerGetAccountAge", kOut1) == kNotInit);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpManagerGetChatRestrictionFlag", kOut1) == kNotInit);
    CHECK(g.call("sceNpManagerGetNetworkTime", kOut1) == kNotInit);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, kService, 0, 0, 0, 0) == kNotInit);
    CHECK(g.call("sceNpManagerGetTicket", kBuf, kSize) == kNotInit);
    CHECK(g.call("sceNpManagerGetTicketParam", 0, kParam) == kNotInit);
    CHECK(g.call("sceNpManagerGetEntitlementIdList", kEnt, 4) == kNotInit);
    CHECK(g.call("sceNpManagerGetEntitlementById", kEntId, kEnt) == kNotInit);

    np_up(g);
    // Questions about the signed-in user: every output is needed, then the answer is offline and nothing is written.
    std::memset(b + kOut1, 0xAA, 0x20);
    std::memset(b + kNpId, 0xAA, 0x24);
    CHECK(g.call("sceNpManagerGetNpId", 0) == kInval);
    CHECK(g.call("sceNpManagerGetNpId", kNpId) == kOffline);
    CHECK(g.call("sceNpManagerGetAccountRegion", 0, kOut2) == kInval);
    CHECK(g.call("sceNpManagerGetAccountRegion", kOut1, 0) == kInval);
    CHECK(g.call("sceNpManagerGetAccountRegion", kOut1, kOut2) == kOffline);
    CHECK(g.call("sceNpManagerGetAccountAge", 0) == kInval);
    CHECK(g.call("sceNpManagerGetAccountAge", kOut1) == kOffline);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", 0, kOut2) == kInval);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", kOut1, 0) == kInval);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", kOut1, kOut2) == kOffline);
    CHECK(g.call("sceNpManagerGetChatRestrictionFlag", 0) == kInval);
    CHECK(g.call("sceNpManagerGetChatRestrictionFlag", kOut1) == kOffline);
    CHECK(g.call("sceNpManagerGetNetworkTime", 0) == kInval);
    CHECK(g.call("sceNpManagerGetNetworkTime", kOut1) == kOffline);
    CHECK(ps_read64(b, kOut1) == 0xAAAAAAAAAAAAAAAAull);
    CHECK(ps_read64(b, kOut2) == 0xAAAAAAAAAAAAAAAAull);
    CHECK(ps_read32(b, kNpId) == 0xAAAAAAAA);

    // Ticket requests are refused offline (after their own argument checks), so there is never a ticket to read.
    CHECK(g.call("sceNpManagerRequestTicket2", 0, 0, kService, 0, 0, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, 0, 0, 0, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, kService, kBuf, 1025, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, kService, kBuf, 1024, 0, 0) == kOffline);
    CHECK(g.call("sceNpManagerGetTicket", kBuf, 0) == kInval);
    ps_write32(b, kSize, 0x1234);
    REQUIRE(g.call("sceNpManagerGetTicket", 0, kSize) == 0); // no buffer: the ticket's size
    CHECK(ps_read32(b, kSize) == 0);
    ps_write32(b, kSize, 0x100);
    std::memset(b + kBuf, 0xAA, 0x100);
    REQUIRE(g.call("sceNpManagerGetTicket", kBuf, kSize) == 0); // a buffer: nothing copied
    CHECK(ps_read32(b, kSize) == 0);
    CHECK(b[kBuf] == 0xAA);
    CHECK(g.call("sceNpManagerGetTicketParam", 0, 0) == kInval);
    CHECK(g.call("sceNpManagerGetTicketParam", -1, kParam) == kInval);
    CHECK(g.call("sceNpManagerGetTicketParam", 12, kParam) == kInval);
    CHECK(g.call("sceNpManagerGetTicketParam", 0, kParam) == kInvalState);
    CHECK(g.call("sceNpManagerGetTicketParam", 11, kParam) == kInvalState);

    // No entitlements: the list's count is 0 and no ID is found.
    CHECK(g.call("sceNpManagerGetEntitlementIdList", kEnt, 4) == 0);
    CHECK(g.call("sceNpManagerGetEntitlementIdList", 0, 0) == 0);
    CHECK(g.call("sceNpManagerGetEntitlementById", 0, kEnt) == kInval);
    CHECK(g.call("sceNpManagerGetEntitlementById", kEntId, kEnt) == kIdNotFound);

    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(g.call("sceNpManagerGetNpId", kNpId) == kNotInit);
    CHECK(g.call("sceNpManagerGetTicket", 0, kSize) == kNotInit);
}

TEST_CASE("sceNpBasic: no events, presence goes nowhere, no block list or messages offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kHandler = kData + 0x120, kEvent = kData + 0x140, kFrom = kData + 0x200,
                  kBuf = kData + 0x400, kSize = kData + 0x500, kPres = kData + 0x600, kCount = kData + 0x900,
                  kNpId = kData + 0x920;
    put_comm_id(b, kComm, "NPWR01234", 0);

    // Before NP; the message type is checked even before that.
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNotInit);
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 16) == kBasicNotInit);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 3) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", kCount) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetBlockListEntry", 0, kNpId) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 8, kCount) == kBasicInval);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 0, kCount) == kBasicNotInit);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, kSize) == kBasicNotInit);

    // Events, presence and attachments need the basic handler; the block list and messages don't.
    np_up(g);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 16) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 3) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, kSize) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", kCount) == kIdNotFound);
    REQUIRE(g.call("sceNpBasicRegisterContextSensitiveHandler", kComm, kHandler, 0) == 0);

    // No event ever comes offline.
    CHECK(g.call("sceNpBasicGetEvent", 0, kFrom, kBuf, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, 0, kBuf, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, 0, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, 0) == kBasicInval);
    ps_write32(b, kEvent, 0x55);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNoEvent);
    CHECK(ps_read32(b, kEvent) == 0x55);

    // Presence data: at most 128 bytes, readable.
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 129) == kBasicExceeds);
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 128) == 0);
    CHECK(g.call("sceNpBasicSetPresence", 0, 0) == 0);
    CHECK(g.call("sceNpBasicSetPresence", 0, 16) == kBasicInval); // RPCS3 ensure()s it

    // Presence details 2: options 1 to 3, struct_size 524, data size at most 128.
    std::memset(b + kPres, 0, 524);
    ps_write32(b, kPres, 524);
    put(b, kPres + 136, "Story mode");
    ps_write32(b, kPres + 520, 128);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 4) == kBasicInval);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", 0, 3) == kBasicInval); // RPCS3 reads it unchecked
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 3) == 0);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 1) == 0);
    ps_write32(b, kPres + 520, 129);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 2) == kBasicExceeds);
    ps_write32(b, kPres + 520, 0);
    ps_write32(b, kPres, 520);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 2) == kBasicInval);

    // The block list and messages are the PSN account's: not found, nothing written.
    ps_write32(b, kCount, 0x55);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", kCount) == kIdNotFound);
    CHECK(g.call("sceNpBasicGetBlockListEntry", 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetBlockListEntry", 0, kNpId) == kIdNotFound);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 7, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 7, kCount) == kIdNotFound);
    CHECK(ps_read32(b, kCount) == 0x55);

    // No invitation or message can be selected offline.
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, 0, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 0, kBuf, kSize) == kBasicInvalDataId);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, kSize) == kBasicInvalDataId);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 2, kBuf, kSize) == kBasicInvalDataId);

    // NP term drops the handler.
    REQUIRE(g.call("sceNp2Term") == 0);
    np_up(g);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNotRegistered);
}

TEST_CASE("sceNpSignaling: contexts in idm order, cleared by NP term; no network, no connections", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kOwn = kData + 0x140, kCtx = kData + 0x180, kInfo = kData + 0x200,
                  kStatus = kData + 0x300, kAddr = kData + 0x304, kPort = kData + 0x308, kConn = kData + 0x30C,
                  kConnInfo = kData + 0x400;
    constexpr u32 kHandler = 0x00012340, kArg = 0x5678;
    put_np_id(b, kNpId, "somebody");
    put_np_id(b, kOwn, "");

    CHECK(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == kSigNotInit);
    CHECK(g.call("sceNpSignalingDestroyCtx", 0x6001) == kSigNotInit);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6001, kHandler, kArg) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, kInfo) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, kStatus, kAddr, kPort) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionInfo", 0x6001, 1, 1, kConnInfo) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, kConn) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionFromPeerAddress", 0x6001, 0x7F000001, 3658, kConn) == kSigNotInit);
    CHECK(g.call("sceNpSignalingTerminateConnection", 0x6001, 1) == kSigNotInit);

    // Contexts: IDs from 0x6001; extended handlers are kept per context.
    np_up(g);
    CHECK(g.call("sceNpSignalingCreateCtx", 0, kHandler, kArg, kCtx) == kSigInval);
    CHECK(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, 0) == kSigInval);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x6001);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, 0, 0, kCtx) == 0); // a null handler is accepted
    CHECK(ps_read32(b, kCtx) == 0x6002);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6001, kHandler, kArg) == 0);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6003, kHandler, kArg) == kSigCtxNotFound);
    REQUIRE(g.call("sceNpSignalingDestroyCtx", 0x6001) == 0);
    CHECK(g.call("sceNpSignalingDestroyCtx", 0x6001) == kSigCtxNotFound);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6001, kHandler, kArg) == kSigCtxNotFound);

    // NP term clears them but, as RPCS3's idm, doesn't rewind the IDs: 0x6003 is next. Once all 8 were handed out,
    // the lowest free one, then none.
    REQUIRE(g.call("sceNp2Term") == 0);
    np_up(g);
    CHECK(g.call("sceNpSignalingDestroyCtx", 0x6002) == kSigCtxNotFound);
    for (u32 id = 0x6003; id <= 0x6008; id++) {
        REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
        CHECK(ps_read32(b, kCtx) == id);
    }
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x6001);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x6002);
    ps_write32(b, kCtx, 0);
    CHECK(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == kSigCtxMax);
    CHECK(ps_read32(b, kCtx) == 0);

    // Local network info: no addresses; the struct's size picks the layout (28 bytes with the NP port, 24 without).
    // The context isn't checked.
    std::memset(b + kInfo, 0xAA, 32);
    ps_write32(b, kInfo, 20);
    CHECK(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, kInfo) == kSigInval);
    ps_write32(b, kInfo, 28);
    REQUIRE(g.call("sceNpSignalingGetLocalNetInfo", 0x7777, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 4) == 0);  // local_addr
    CHECK(ps_read32(b, kInfo + 8) == 0);  // mapped_addr
    CHECK(ps_read32(b, kInfo + 12) == 2); // nat_status: type 2
    CHECK(ps_read32(b, kInfo + 16) == 1); // upnp_status: invalid
    CHECK(ps_read32(b, kInfo + 20) == 1); // npport_status: open
    CHECK(ps_read16(b, kInfo + 24) == 3658);
    CHECK(ps_read16(b, kInfo + 26) == 0xAAAA);
    std::memset(b + kInfo, 0xAA, 32);
    ps_write32(b, kInfo, 24);
    REQUIRE(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 4) == 0);
    CHECK(ps_read32(b, kInfo + 20) == 1);
    CHECK(ps_read16(b, kInfo + 24) == 0xAAAA);

    // No connection exists offline.
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, 0, kAddr, kPort) == kSigInval);
    ps_write32(b, kStatus, 2);
    ps_write32(b, kAddr, 0x55);
    ps_write16(b, kPort, 0x55);
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, kStatus, kAddr, kPort) == kSigConnNotFound);
    CHECK(ps_read32(b, kStatus) == 0); // inactive
    CHECK(ps_read32(b, kAddr) == 0x55);
    CHECK(ps_read16(b, kPort) == 0x55);
    CHECK(g.call("sceNpSignalingGetConnectionInfo", 0x6001, 1, 1, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionInfo", 0x6001, 1, 1, kConnInfo) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, 0, kConn) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kOwn, kConn) == kSigOwnNpId); // the user's: empty
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, kConn) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingGetConnectionFromPeerAddress", 0x6001, 0x7F000001, 3658, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionFromPeerAddress", 0x6001, 0x7F000001, 3658, kConn) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingTerminateConnection", 0x6001, 1) == 0);
}

TEST_CASE("sceNpUtilCmpNpId: the online IDs' first 16 characters, without NP", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kA = kData + 0x100, kB = kData + 0x140;
    put_np_id(b, kA, "player-one");
    put_np_id(b, kB, "player-one");
    CHECK(g.call("sceNpUtilCmpNpId", 0, kB) == kUtilInval);
    CHECK(g.call("sceNpUtilCmpNpId", kA, 0) == kUtilInval);
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == 0);
    b[kB + 12] = 'x'; // after the terminator
    b[kB + 20] = 1;   // opt[0]
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == 0);
    put_np_id(b, kB, "player-two");
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == kUtilNotMatch);
    put_np_id(b, kB, "player");
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == kUtilNotMatch);
    put_np_id(b, kA, "sixteen-chars-id");
    put_np_id(b, kB, "sixteen-chars-id");
    b[kB + 16] = 'x'; // the 17th character isn't compared
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == 0);
}
