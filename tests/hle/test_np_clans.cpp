// sceNpClans offline (synthetic: no game data), as RPCS3 with its defaults (clans off, PSN off): the library starts,
// no request handle can be made; each call makes RPCS3's checks, then ends where RPCS3's clan client does (no ticket:
// SERVICE_UNAVAILABLE; no request: NOT_INITIALIZED), or answers CELL_OK where RPCS3 has a TODO.

#include "hle_test_util.hpp"

#include <cstring>
#include <string>
#include <string_view>

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kAlready = 0xFFFFFFFF80022701ull, kNotInit = 0xFFFFFFFF80022702ull, kNotSupported = 0xFFFFFFFF80022703ull,
              kInval = 0xFFFFFFFF80022705ull, kExceeds = 0xFFFFFFFF80022706ull, kUnavailable = 0xFFFFFFFF80022711ull;
constexpr u32 kUnmapped = 0x20000000;

void put(u8* b, u32 addr, std::string_view s)
{
    std::memcpy(b + addr, s.data(), s.size());
    b[addr + s.size()] = 0;
}

} // namespace

TEST_CASE("sceNpClans offline: RPCS3's checks, then no ticket or no request; its TODOs answer CELL_OK", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "clans");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kPass = kData + 0x200, kHandle = kData + 0x300, kNpId = kData + 0x400,
                  kPaging = kData + 0x500, kList = kData + 0x600, kPage = kData + 0x700, kName = kData + 0x800,
                  kTag = kData + 0x900, kMessage = kData + 0x1000, kOut = kData + 0x2000;

    // Init: the IDs, no flags.
    CHECK(g.call("sceNpClansCreateRequest", kHandle, 0) == kNotInit);
    CHECK(g.call("sceNpClansTerm") == kNotInit);
    CHECK(g.call("sceNpClansInit", 0, kPass, 0, 0, 0) == kInval);
    CHECK(g.call("sceNpClansInit", kComm, kPass, 0, 0, 1) == kNotSupported);
    REQUIRE(g.call("sceNpClansInit", kComm, kPass, 0, 0, 0) == 0);
    CHECK(g.call("sceNpClansInit", kComm, kPass, 0, 0, 0) == kAlready);

    // Requests: clans are off.
    CHECK(g.call("sceNpClansCreateRequest", 0, 0) == kInval);
    CHECK(g.call("sceNpClansCreateRequest", kHandle, 1) == kNotSupported);
    ps_write32(b, kHandle, 0x5555);
    CHECK(g.call("sceNpClansCreateRequest", kHandle, 0) == kUnavailable);
    CHECK(ps_read32(b, kHandle) == 0x5555);
    CHECK(g.call("sceNpClansDestroyRequest", 1) == kInval);
    CHECK(g.call("sceNpClansAbortRequest", 1) == 0);

    // Clans: a name of at most 64 characters and a tag of at most 8.
    put(b, kName, std::string(65, 'n'));
    put(b, kTag, "TAG");
    CHECK(g.call("sceNpClansCreateClan", 1, kName, kTag, 0) == kInval);
    CHECK(g.call("sceNpClansCreateClan", 1, kName, kTag, kOut) == kExceeds);
    put(b, kName, std::string(64, 'n'));
    put(b, kTag, std::string(9, 't'));
    CHECK(g.call("sceNpClansCreateClan", 1, kName, kTag, kOut) == kExceeds);
    put(b, kTag, std::string(8, 't'));
    CHECK(g.call("sceNpClansCreateClan", 1, kName, kTag, kOut) == kUnavailable);
    CHECK(g.call("sceNpClansCreateClan", 1, kUnmapped, kTag, kOut) == kInval); // RPCS3 faults
    CHECK(g.call("sceNpClansDisbandClan", 1, 0) == kInval);
    CHECK(g.call("sceNpClansDisbandClan", 1, 7) == kUnavailable);

    // Paged lists: a page result, a list when paging; at most 100 entries from position 1000000.
    ps_write32(b, kPaging, 0);
    ps_write32(b, kPaging + 4, 101);
    CHECK(g.call("sceNpClansGetClanList", 1, 0, 0, 0) == kInval);
    CHECK(g.call("sceNpClansGetClanList", 1, kPaging, 0, kPage) == kInval);
    CHECK(g.call("sceNpClansGetClanList", 1, kPaging, kList, kPage) == kExceeds);
    ps_write32(b, kPaging, 1000001);
    ps_write32(b, kPaging + 4, 100);
    CHECK(g.call("sceNpClansGetClanList", 1, kPaging, kList, kPage) == kExceeds);
    ps_write32(b, kPaging, 1000000);
    CHECK(g.call("sceNpClansGetClanList", 1, kPaging, kList, kPage) == kUnavailable);
    CHECK(g.call("sceNpClansGetClanList", 1, 0, 0, kPage) == kUnavailable);
    CHECK(g.call("sceNpClansGetClanListByNpId", 1, 0, 0, 0, kPage) == kInval);
    CHECK(g.call("sceNpClansGetClanListByNpId", 1, kPaging, kNpId, kList, kPage) == 0); // RPCS3's TODO
    CHECK(g.call("sceNpClansSearchByProfile", 1, 0, kOut, 0, kPage) == 0);
    CHECK(g.call("sceNpClansSearchByName", 1, 0, 0, 0, kPage) == kInval);
    CHECK(g.call("sceNpClansSearchByName", 1, 0, kOut, 0, kPage) == kNotInit); // no request: no ticket needed
    CHECK(g.call("sceNpClansGetClanInfo", 1, 7, 0) == kInval);
    CHECK(g.call("sceNpClansGetClanInfo", 1, 7, kOut) == kNotInit);
    CHECK(g.call("sceNpClansUpdateClanInfo", 1, 7, kOut) == kUnavailable);
    CHECK(g.call("sceNpClansGetMemberList", 1, 7, kPaging, 1, kList, kPage) == kUnavailable);
    CHECK(g.call("sceNpClansGetBlacklist", 1, 7, kPaging, 0, kPage) == kInval);
    CHECK(g.call("sceNpClansGetBlacklist", 1, 7, kPaging, kList, kPage) == kUnavailable);
    CHECK(g.call("sceNpClansRetrieveAnnouncements", 1, 0xFFFFFFFFu, 0, 0, kPage) == kInval);
    CHECK(g.call("sceNpClansRetrieveAnnouncements", 1, 7, 0, 0, kPage) == kUnavailable);
    CHECK(g.call("sceNpClansRetrievePostedChallenges", 1, 7, 8, kPaging, kList, kPage) == 0);
    CHECK(g.call("sceNpClansRetrieveChallenges", 1, 7, kPaging, kList, kPage) == 0);

    // Members.
    CHECK(g.call("sceNpClansGetMemberInfo", 1, 7, 0, kOut) == kInval);
    CHECK(g.call("sceNpClansGetMemberInfo", 1, 7, kNpId, kOut) == kUnavailable);
    CHECK(g.call("sceNpClansUpdateMemberInfo", 1, 7, kOut) == kUnavailable);
    CHECK(g.call("sceNpClansChangeMemberRole", 1, 7, kNpId, 5) == kInval);
    CHECK(g.call("sceNpClansChangeMemberRole", 1, 7, kNpId, 4) == kUnavailable);
    CHECK(g.call("sceNpClansGetAutoAcceptStatus", 1, 7, 0) == kInval);
    CHECK(g.call("sceNpClansGetAutoAcceptStatus", 1, 7, kOut) == 0);
    CHECK(g.call("sceNpClansUpdateAutoAcceptStatus", 1, 7, 1) == 0);
    CHECK(g.call("sceNpClansJoinClan", 1, 7) == kUnavailable);
    CHECK(g.call("sceNpClansLeaveClan", 1, 7) == kUnavailable);

    // Messages: a subject of at most 54 characters, a body of at most 1536.
    std::memset(b + kMessage, 0, 1596);
    put(b, kMessage, std::string(55, 's'));
    CHECK(g.call("sceNpClansKickMember", 1, 7, 0, kMessage) == kInval);
    CHECK(g.call("sceNpClansKickMember", 1, 7, kNpId, kMessage) == kExceeds);
    put(b, kMessage, std::string(54, 's'));
    put(b, kMessage + 55, std::string(1537, 'm'));
    CHECK(g.call("sceNpClansSendInvitation", 1, 7, kNpId, kMessage) == kExceeds);
    put(b, kMessage + 55, std::string(1536, 'm'));
    for (const char* name : {"sceNpClansKickMember", "sceNpClansSendInvitation"}) {
        CHECK(g.call(name, 1, 7, kNpId, kMessage) == kUnavailable);
        CHECK(g.call(name, 1, 7, kNpId, 0) == kUnavailable);
    }
    CHECK(g.call("sceNpClansCancelInvitation", 1, 7, 0) == kInval);
    CHECK(g.call("sceNpClansCancelInvitation", 1, 7, kNpId) == kUnavailable);
    CHECK(g.call("sceNpClansSendInvitationResponse", 1, 7, kMessage, 1) == kUnavailable);
    CHECK(g.call("sceNpClansSendMembershipRequest", 1, 7, kMessage) == kUnavailable);
    CHECK(g.call("sceNpClansCancelMembershipRequest", 1, 7) == kUnavailable);
    CHECK(g.call("sceNpClansSendMembershipResponse", 1, 7, 0, kMessage, 1) == kInval);
    CHECK(g.call("sceNpClansSendMembershipResponse", 1, 7, kNpId, kMessage, 1) == kUnavailable);
    CHECK(g.call("sceNpClansAddBlacklistEntry", 1, 7, 0) == kInval);
    CHECK(g.call("sceNpClansAddBlacklistEntry", 1, 7, kNpId) == kUnavailable);
    CHECK(g.call("sceNpClansRemoveBlacklistEntry", 1, 7, kNpId) == kUnavailable);

    // Announcements and challenges: a message, its ID's address, a duration; a challenge needs its data.
    CHECK(g.call("sceNpClansPostAnnouncement", 1, 7, kMessage, 0, 0, kOut) == kInval);
    CHECK(g.call("sceNpClansPostAnnouncement", 1, 7, kMessage, 0, 60, kOut) == kUnavailable);
    CHECK(g.call("sceNpClansRemoveAnnouncement", 1, 7, 3) == kUnavailable);
    CHECK(g.call("sceNpClansPostChallenge", 1, 7, 8, kMessage, 0, 60, kOut) == kNotSupported);
    CHECK(g.call("sceNpClansPostChallenge", 1, 7, 8, kMessage, kList, 60, kOut) == 0);
    CHECK(g.call("sceNpClansRemovePostedChallenge", 1, 7, 8, 3) == 0);
    CHECK(g.call("sceNpClansRemoveChallenge", 1, 7, 3) == 0);

    REQUIRE(g.call("sceNpClansTerm") == 0);
    CHECK(g.call("sceNpClansJoinClan", 1, 7) == kNotInit);
}
