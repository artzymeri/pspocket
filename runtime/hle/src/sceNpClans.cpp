// sceNpClans: PSN clans (groups of users with members, announcements and challenges). Reference: RPCS3
// rpcs3/Emu/Cell/Modules/sceNpClans.cpp/.h and its client of the clan servers (rpcs3/Emu/NP/clans_client.cpp).
//
// Offline, as RPCS3 with its defaults (clans disabled, PSN disabled; D-059): the library initializes, but its client
// makes no request (clans_client::create_request: SCE_NP_CLANS_ERROR_SERVICE_UNAVAILABLE), so no request handle is
// ever valid. Each call makes RPCS3's checks in RPCS3's order (the library, its pointers, its lengths and paging), then
// ends where RPCS3's client does:
// - a call that needs the user's clan ticket (most of them) finds no user signed in: SERVICE_UNAVAILABLE;
// - a call without a ticket (clan info, search by name) finds no request for its handle: NOT_INITIALIZED;
// - RPCS3's TODOs (lists by NP ID, search by profile, auto-accept, challenges) answer CELL_OK and write nothing.
// Nothing is written to the game's outputs.
//
// Difference from RPCS3: the strings whose lengths the checks measure are read only within guest memory (D-042): one
// that runs out of it is logged with the call site and refused with SCE_NP_CLANS_ERROR_INVALID_ARGUMENT.

#include <format>
#include <mutex>
#include <optional>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNpClans";

enum : u32 {
    SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED = 0x80022701,
    SCE_NP_CLANS_ERROR_NOT_INITIALIZED = 0x80022702,
    SCE_NP_CLANS_ERROR_NOT_SUPPORTED = 0x80022703,
    SCE_NP_CLANS_ERROR_INVALID_ARGUMENT = 0x80022705,
    SCE_NP_CLANS_ERROR_EXCEEDS_MAX = 0x80022706,
    SCE_NP_CLANS_ERROR_SERVICE_UNAVAILABLE = 0x80022711,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 SCE_NP_CLANS_CLAN_NAME_MAX_LENGTH = 64, SCE_NP_CLANS_CLAN_TAG_MAX_LENGTH = 8;
constexpr u32 SCE_NP_CLANS_MESSAGE_SUBJECT_MAX_LENGTH = 54, SCE_NP_CLANS_MESSAGE_BODY_MAX_LENGTH = 1536,
              SCE_NP_CLANS_ANNOUNCEMENT_MESSAGE_BODY_MAX_LENGTH = 1536;
constexpr u32 SCE_NP_CLANS_PAGING_REQUEST_START_POSITION_MAX = 1000000, SCE_NP_CLANS_PAGING_REQUEST_PAGE_MAX = 100;
constexpr u32 SCE_NP_CLANS_ROLE_LEADER = 4;
// SceNpClansMessage: subject[55], body[1537] at 55, be32 options. SceNpClansPagingRequest: be32 startPos, max.
constexpr u32 kMessageBody = 55;

// RPCS3 sce_np_clans_manager's is_initialized.
struct ClansState {
    std::mutex mutex;
    bool initialized = false;
};

bool clans_initialized()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpClans called without an HLE process");
    ClansState& s = p->state<ClansState>();
    std::lock_guard lock(s.mutex);
    return s.initialized;
}

// The length of the C string at `addr` if it is at most `max`, else max + 1; nullopt when it runs out of guest memory
// first (logged).
std::optional<u32> string_length(PPUContext& ctx, std::string_view name, u32 addr, u32 max)
{
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 i = 0; i <= max; i++) {
        if (!guest_readable(t, addr + i, 1)) {
            log_call_site(ctx, t.kernel.base(), std::format("{}: the string at {:#x} runs out of guest memory", name,
                                                    addr));
            return std::nullopt;
        }
        if (!ps_read8(t.kernel.base(), addr + i)) return i;
    }
    return max + 1;
}

// RPCS3's check of a message's lengths: the body at most `body_max` characters, the subject at most 54.
s32 check_message(PPUContext& ctx, std::string_view name, u32 message, u32 body_max)
{
    const std::optional<u32> body = string_length(ctx, name, message + kMessageBody, body_max);
    const std::optional<u32> subject = body ? string_length(ctx, name, message, SCE_NP_CLANS_MESSAGE_SUBJECT_MAX_LENGTH)
                                            : std::nullopt;
    if (!body || !subject) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (*body > body_max || *subject > SCE_NP_CLANS_MESSAGE_SUBJECT_MAX_LENGTH) {
        return error(SCE_NP_CLANS_ERROR_EXCEEDS_MAX);
    }
    return lv2::CELL_OK;
}

// RPCS3's paging check: from at most position 1000000, at most 100 entries a page.
s32 check_paging(PPUContext& ctx, std::string_view name, u32 paging)
{
    if (!paging) return lv2::CELL_OK;
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, paging, 8)) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: the paging request {:#x} is not guest memory", name, paging));
        return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    }
    if (ps_read32(t.kernel.base(), paging) > SCE_NP_CLANS_PAGING_REQUEST_START_POSITION_MAX ||
        ps_read32(t.kernel.base(), paging + 4) > SCE_NP_CLANS_PAGING_REQUEST_PAGE_MAX) {
        return error(SCE_NP_CLANS_ERROR_EXCEEDS_MAX);
    }
    return lv2::CELL_OK;
}

// The ends of a call past its checks: the user's clan ticket (no user is signed in), no request for the handle, or a
// TODO of RPCS3's.
s32 no_ticket(std::string_view name, u32 handle)
{
    PS_LOG_DEBUG(kChan, "{}(request {}): no user signed in for a clan ticket", name, handle);
    return error(SCE_NP_CLANS_ERROR_SERVICE_UNAVAILABLE);
}

s32 no_request(std::string_view name, u32 handle)
{
    PS_LOG_DEBUG(kChan, "{}(request {}): no such request", name, handle);
    return error(SCE_NP_CLANS_ERROR_NOT_INITIALIZED);
}

s32 todo(std::string_view name, u32 handle)
{
    PS_LOG_WARN(kChan, "{}(request {}): a TODO in RPCS3, answered CELL_OK as there; nothing written", name, handle);
    return lv2::CELL_OK;
}

#define PS_CLANS_INIT()                                                                                                \
    if (!clans_initialized()) return error(SCE_NP_CLANS_ERROR_NOT_INITIALIZED)

// ------------------------------------------------------------------------------------------------- library

// RPCS3 sceNpClansInit: the communication ID and passphrase, no flags; the pool isn't used.
s32 sceNpClansInit(PPUContext&, u32 comm_id, u32 passphrase, u32 pool, u32 pool_size, u32 flags)
{
    ClansState& s = HleProcess::current()->state<ClansState>();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED);
    if (!comm_id || !passphrase) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (flags != 0) return error(SCE_NP_CLANS_ERROR_NOT_SUPPORTED);
    s.initialized = true;
    PS_LOG_INFO(kChan, "sceNpClansInit(pool {:#x}, size at {:#x}): up; clans are off, as in RPCS3 by default", pool,
        pool_size);
    return lv2::CELL_OK;
}

s32 sceNpClansTerm(PPUContext&)
{
    ClansState& s = HleProcess::current()->state<ClansState>();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_CLANS_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpClansCreateRequest: the handle's address, no flags; then the client, with clans off: service unavailable.
s32 sceNpClansCreateRequest(PPUContext&, u32 handle, u64 flags)
{
    PS_CLANS_INIT();
    if (!handle) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (flags != 0) return error(SCE_NP_CLANS_ERROR_NOT_SUPPORTED);
    PS_LOG_INFO(kChan, "sceNpClansCreateRequest: clans are off (RPCS3's default): service unavailable");
    return error(SCE_NP_CLANS_ERROR_SERVICE_UNAVAILABLE);
}

// RPCS3 sceNpClansDestroyRequest: no request has the handle (SCE_NP_CLANS_ERROR_INVALID_ARGUMENT); aborting ignores
// that.
s32 sceNpClansDestroyRequest(PPUContext&, u32 handle)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansDestroyRequest({}): no such request", handle);
    return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
}

s32 sceNpClansAbortRequest(PPUContext&, u32 handle)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansAbortRequest({}): no such request", handle);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- clans

s32 sceNpClansCreateClan(PPUContext& ctx, u32 handle, u32 name, u32 tag, u32 clan_id)
{
    PS_CLANS_INIT();
    if (!name || !tag || !clan_id) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    const std::optional<u32> name_length =
        string_length(ctx, "sceNpClansCreateClan", name, SCE_NP_CLANS_CLAN_NAME_MAX_LENGTH);
    const std::optional<u32> tag_length =
        name_length ? string_length(ctx, "sceNpClansCreateClan", tag, SCE_NP_CLANS_CLAN_TAG_MAX_LENGTH) : std::nullopt;
    if (!name_length || !tag_length) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (*name_length > SCE_NP_CLANS_CLAN_NAME_MAX_LENGTH || *tag_length > SCE_NP_CLANS_CLAN_TAG_MAX_LENGTH) {
        return error(SCE_NP_CLANS_ERROR_EXCEEDS_MAX);
    }
    return no_ticket("sceNpClansCreateClan", handle);
}

s32 sceNpClansDisbandClan(PPUContext&, u32 handle, u32 clan_id)
{
    PS_CLANS_INIT();
    if (!clan_id) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    return no_ticket("sceNpClansDisbandClan", handle);
}

// The paged lists: the page result, a list wherever paging is asked for, then the paging itself.
s32 paged_checks(PPUContext& ctx, std::string_view name, u32 paging, u32 list, u32 page_result)
{
    if (!page_result || (paging && !list)) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    return check_paging(ctx, name, paging);
}

s32 sceNpClansGetClanList(PPUContext& ctx, u32 handle, u32 paging, u32 clan_list, u32 page_result)
{
    PS_CLANS_INIT();
    if (const s32 r = paged_checks(ctx, "sceNpClansGetClanList", paging, clan_list, page_result)) return r;
    return no_ticket("sceNpClansGetClanList", handle);
}

s32 sceNpClansGetClanListByNpId(PPUContext& ctx, u32 handle, u32 paging, u32 np_id, u32 clan_list, u32 page_result)
{
    PS_CLANS_INIT();
    if (!np_id) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (const s32 r = paged_checks(ctx, "sceNpClansGetClanListByNpId", paging, clan_list, page_result)) return r;
    return todo("sceNpClansGetClanListByNpId", handle);
}

s32 sceNpClansSearchByProfile(PPUContext& ctx, u32 handle, u32 paging, u32 search, u32 results, u32 page_result)
{
    PS_CLANS_INIT();
    if (!search) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (const s32 r = paged_checks(ctx, "sceNpClansSearchByProfile", paging, results, page_result)) return r;
    return todo("sceNpClansSearchByProfile", handle);
}

// RPCS3 clans_client::clan_search needs no ticket: the request it would send has no handle.
s32 sceNpClansSearchByName(PPUContext& ctx, u32 handle, u32 paging, u32 search, u32 results, u32 page_result)
{
    PS_CLANS_INIT();
    if (!search) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (const s32 r = paged_checks(ctx, "sceNpClansSearchByName", paging, results, page_result)) return r;
    return no_request("sceNpClansSearchByName", handle);
}

// RPCS3 clans_client::get_clan_info needs no ticket either.
s32 sceNpClansGetClanInfo(PPUContext&, u32 handle, u32 clan_id, u32 info)
{
    PS_CLANS_INIT();
    if (!info) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpClansGetClanInfo: clan {}", clan_id);
    return no_request("sceNpClansGetClanInfo", handle);
}

s32 sceNpClansUpdateClanInfo(PPUContext&, u32 handle, u32 clan_id, u32 info)
{
    PS_CLANS_INIT();
    if (!info) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpClansUpdateClanInfo: clan {}", clan_id);
    return no_ticket("sceNpClansUpdateClanInfo", handle);
}

// ------------------------------------------------------------------------------------------------- members

s32 sceNpClansGetMemberList(PPUContext& ctx, u32 handle, u32 clan_id, u32 paging, u32 status, u32 mem_list,
    u32 page_result)
{
    PS_CLANS_INIT();
    if (const s32 r = paged_checks(ctx, "sceNpClansGetMemberList", paging, mem_list, page_result)) return r;
    PS_LOG_DEBUG(kChan, "sceNpClansGetMemberList: clan {}, status {}", clan_id, status);
    return no_ticket("sceNpClansGetMemberList", handle);
}

s32 sceNpClansGetMemberInfo(PPUContext&, u32 handle, u32 clan_id, u32 np_id, u32 mem_info)
{
    PS_CLANS_INIT();
    if (!np_id || !mem_info) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpClansGetMemberInfo: clan {}", clan_id);
    return no_ticket("sceNpClansGetMemberInfo", handle);
}

s32 sceNpClansUpdateMemberInfo(PPUContext&, u32 handle, u32 clan_id, u32 info)
{
    PS_CLANS_INIT();
    if (!info) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpClansUpdateMemberInfo: clan {}", clan_id);
    return no_ticket("sceNpClansUpdateMemberInfo", handle);
}

s32 sceNpClansChangeMemberRole(PPUContext&, u32 handle, u32 clan_id, u32 np_id, u32 role)
{
    PS_CLANS_INIT();
    if (!np_id || role > SCE_NP_CLANS_ROLE_LEADER) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpClansChangeMemberRole: clan {}, role {}", clan_id, role);
    return no_ticket("sceNpClansChangeMemberRole", handle);
}

s32 sceNpClansGetAutoAcceptStatus(PPUContext&, u32 handle, u32 clan_id, u32 enable)
{
    PS_CLANS_INIT();
    if (!enable) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpClansGetAutoAcceptStatus: clan {}", clan_id);
    return todo("sceNpClansGetAutoAcceptStatus", handle);
}

s32 sceNpClansUpdateAutoAcceptStatus(PPUContext&, u32 handle, u32 clan_id, bool enable)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansUpdateAutoAcceptStatus: clan {}, {}", clan_id, enable);
    return todo("sceNpClansUpdateAutoAcceptStatus", handle);
}

s32 sceNpClansJoinClan(PPUContext&, u32 handle, u32 clan_id)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansJoinClan: clan {}", clan_id);
    return no_ticket("sceNpClansJoinClan", handle);
}

s32 sceNpClansLeaveClan(PPUContext&, u32 handle, u32 clan_id)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansLeaveClan: clan {}", clan_id);
    return no_ticket("sceNpClansLeaveClan", handle);
}

// The calls about a member with an optional message: the member, then the message's lengths.
s32 member_message(PPUContext& ctx, std::string_view name, u32 handle, u32 np_id, u32 message, bool need_member)
{
    PS_CLANS_INIT();
    if (need_member && !np_id) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (message) {
        if (const s32 r = check_message(ctx, name, message, SCE_NP_CLANS_MESSAGE_BODY_MAX_LENGTH)) return r;
    }
    return no_ticket(name, handle);
}

s32 sceNpClansKickMember(PPUContext& ctx, u32 handle, u32 clan_id, u32 np_id, u32 message)
{
    PS_LOG_DEBUG(kChan, "sceNpClansKickMember: clan {}", clan_id);
    return member_message(ctx, "sceNpClansKickMember", handle, np_id, message, true);
}

s32 sceNpClansSendInvitation(PPUContext& ctx, u32 handle, u32 clan_id, u32 np_id, u32 message)
{
    PS_LOG_DEBUG(kChan, "sceNpClansSendInvitation: clan {}", clan_id);
    return member_message(ctx, "sceNpClansSendInvitation", handle, np_id, message, true);
}

s32 sceNpClansCancelInvitation(PPUContext& ctx, u32 handle, u32 clan_id, u32 np_id)
{
    PS_LOG_DEBUG(kChan, "sceNpClansCancelInvitation: clan {}", clan_id);
    return member_message(ctx, "sceNpClansCancelInvitation", handle, np_id, 0, true);
}

s32 sceNpClansSendInvitationResponse(PPUContext& ctx, u32 handle, u32 clan_id, u32 message, bool accept)
{
    PS_LOG_DEBUG(kChan, "sceNpClansSendInvitationResponse: clan {}, accept {}", clan_id, accept);
    return member_message(ctx, "sceNpClansSendInvitationResponse", handle, 0, message, false);
}

s32 sceNpClansSendMembershipRequest(PPUContext& ctx, u32 handle, u32 clan_id, u32 message)
{
    PS_LOG_DEBUG(kChan, "sceNpClansSendMembershipRequest: clan {}", clan_id);
    return member_message(ctx, "sceNpClansSendMembershipRequest", handle, 0, message, false);
}

s32 sceNpClansCancelMembershipRequest(PPUContext& ctx, u32 handle, u32 clan_id)
{
    PS_LOG_DEBUG(kChan, "sceNpClansCancelMembershipRequest: clan {}", clan_id);
    return member_message(ctx, "sceNpClansCancelMembershipRequest", handle, 0, 0, false);
}

s32 sceNpClansSendMembershipResponse(PPUContext& ctx, u32 handle, u32 clan_id, u32 np_id, u32 message, bool allow)
{
    PS_LOG_DEBUG(kChan, "sceNpClansSendMembershipResponse: clan {}, allow {}", clan_id, allow);
    return member_message(ctx, "sceNpClansSendMembershipResponse", handle, np_id, message, true);
}

// ------------------------------------------------------------------------------------------------- blacklist

s32 sceNpClansGetBlacklist(PPUContext& ctx, u32 handle, u32 clan_id, u32 paging, u32 blacklist, u32 page_result)
{
    PS_CLANS_INIT();
    if (const s32 r = paged_checks(ctx, "sceNpClansGetBlacklist", paging, blacklist, page_result)) return r;
    PS_LOG_DEBUG(kChan, "sceNpClansGetBlacklist: clan {}", clan_id);
    return no_ticket("sceNpClansGetBlacklist", handle);
}

s32 sceNpClansAddBlacklistEntry(PPUContext& ctx, u32 handle, u32 clan_id, u32 member)
{
    PS_LOG_DEBUG(kChan, "sceNpClansAddBlacklistEntry: clan {}", clan_id);
    return member_message(ctx, "sceNpClansAddBlacklistEntry", handle, member, 0, true);
}

s32 sceNpClansRemoveBlacklistEntry(PPUContext& ctx, u32 handle, u32 clan_id, u32 member)
{
    PS_LOG_DEBUG(kChan, "sceNpClansRemoveBlacklistEntry: clan {}", clan_id);
    return member_message(ctx, "sceNpClansRemoveBlacklistEntry", handle, member, 0, true);
}

// --------------------------------------------------------------------------------------- announcements, challenges

s32 sceNpClansRetrieveAnnouncements(PPUContext& ctx, u32 handle, u32 clan_id, u32 paging, u32 list, u32 page_result)
{
    PS_CLANS_INIT();
    if (clan_id == 0xFFFFFFFF) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (const s32 r = paged_checks(ctx, "sceNpClansRetrieveAnnouncements", paging, list, page_result)) return r;
    return no_ticket("sceNpClansRetrieveAnnouncements", handle);
}

// An announcement or a challenge: the message, its ID's address, a duration; then (a challenge) its data; then the
// lengths, a body of at most 1536 characters.
s32 post_checks(PPUContext& ctx, std::string_view name, u32 message, u32 data, u32 duration, u32 message_id,
    bool need_data)
{
    if (!message || !message_id || duration == 0) return error(SCE_NP_CLANS_ERROR_INVALID_ARGUMENT);
    if (need_data && !data) return error(SCE_NP_CLANS_ERROR_NOT_SUPPORTED);
    return check_message(ctx, name, message, SCE_NP_CLANS_ANNOUNCEMENT_MESSAGE_BODY_MAX_LENGTH);
}

s32 sceNpClansPostAnnouncement(PPUContext& ctx, u32 handle, u32 clan_id, u32 message, u32 data, u32 duration,
    u32 message_id)
{
    PS_CLANS_INIT();
    if (const s32 r = post_checks(ctx, "sceNpClansPostAnnouncement", message, data, duration, message_id, false)) {
        return r;
    }
    PS_LOG_DEBUG(kChan, "sceNpClansPostAnnouncement: clan {}", clan_id);
    return no_ticket("sceNpClansPostAnnouncement", handle);
}

s32 sceNpClansRemoveAnnouncement(PPUContext&, u32 handle, u32 clan_id, u32 message_id)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansRemoveAnnouncement: clan {}, message {}", clan_id, message_id);
    return no_ticket("sceNpClansRemoveAnnouncement", handle);
}

s32 sceNpClansPostChallenge(PPUContext& ctx, u32 handle, u32 clan_id, u32 target_clan, u32 message, u32 data,
    u32 duration, u32 message_id)
{
    PS_CLANS_INIT();
    if (const s32 r = post_checks(ctx, "sceNpClansPostChallenge", message, data, duration, message_id, true)) return r;
    PS_LOG_DEBUG(kChan, "sceNpClansPostChallenge: clan {} to {}", clan_id, target_clan);
    return todo("sceNpClansPostChallenge", handle);
}

s32 sceNpClansRetrievePostedChallenges(PPUContext& ctx, u32 handle, u32 clan_id, u32 target_clan, u32 paging,
    u32 list, u32 page_result)
{
    PS_CLANS_INIT();
    if (const s32 r = paged_checks(ctx, "sceNpClansRetrievePostedChallenges", paging, list, page_result)) return r;
    PS_LOG_DEBUG(kChan, "sceNpClansRetrievePostedChallenges: clan {} to {}", clan_id, target_clan);
    return todo("sceNpClansRetrievePostedChallenges", handle);
}

s32 sceNpClansRemovePostedChallenge(PPUContext&, u32 handle, u32 clan_id, u32 target_clan, u32 message_id)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansRemovePostedChallenge: clan {} to {}, message {}", clan_id, target_clan, message_id);
    return todo("sceNpClansRemovePostedChallenge", handle);
}

s32 sceNpClansRetrieveChallenges(PPUContext& ctx, u32 handle, u32 clan_id, u32 paging, u32 list, u32 page_result)
{
    PS_CLANS_INIT();
    if (const s32 r = paged_checks(ctx, "sceNpClansRetrieveChallenges", paging, list, page_result)) return r;
    PS_LOG_DEBUG(kChan, "sceNpClansRetrieveChallenges: clan {}", clan_id);
    return todo("sceNpClansRetrieveChallenges", handle);
}

s32 sceNpClansRemoveChallenge(PPUContext&, u32 handle, u32 clan_id, u32 message_id)
{
    PS_CLANS_INIT();
    PS_LOG_DEBUG(kChan, "sceNpClansRemoveChallenge: clan {}, message {}", clan_id, message_id);
    return todo("sceNpClansRemoveChallenge", handle);
}

#undef PS_CLANS_INIT

} // namespace

void register_sceNpClans(Registry& r)
{
    r.add("sceNpClans", {
        PS_HLE_FUNC(0x9B820047, sceNpClansInit),
        PS_HLE_FUNC(0x42332CB7, sceNpClansTerm),
        PS_HLE_FUNC(0x9A72232D, sceNpClansCreateRequest),
        PS_HLE_FUNC(0xD6551CD1, sceNpClansDestroyRequest),
        PS_HLE_FUNC(0xE82969E2, sceNpClansAbortRequest),
        PS_HLE_FUNC(0xA6A31A38, sceNpClansCreateClan),
        PS_HLE_FUNC(0x4826F6D5, sceNpClansDisbandClan),
        PS_HLE_FUNC(0xCA4181B4, sceNpClansGetClanList),
        PS_HLE_FUNC(0x672399A8, sceNpClansGetClanListByNpId),
        PS_HLE_FUNC(0x1221A1BF, sceNpClansSearchByProfile),
        PS_HLE_FUNC(0xACE0CFBA, sceNpClansSearchByName),
        PS_HLE_FUNC(0x487DE998, sceNpClansGetClanInfo),
        PS_HLE_FUNC(0x09F9E1A9, sceNpClansUpdateClanInfo),
        PS_HLE_FUNC(0x856FF5C0, sceNpClansGetMemberList),
        PS_HLE_FUNC(0x20472DA0, sceNpClansGetMemberInfo),
        PS_HLE_FUNC(0xF4A2D52B, sceNpClansUpdateMemberInfo),
        PS_HLE_FUNC(0x9CAC2085, sceNpClansChangeMemberRole),
        PS_HLE_FUNC(0x38DADF1F, sceNpClansGetAutoAcceptStatus),
        PS_HLE_FUNC(0x5DA94854, sceNpClansUpdateAutoAcceptStatus),
        PS_HLE_FUNC(0xDBF300CA, sceNpClansJoinClan),
        PS_HLE_FUNC(0x560F717B, sceNpClansLeaveClan),
        PS_HLE_FUNC(0xAA7912B5, sceNpClansKickMember),
        PS_HLE_FUNC(0xBC05EF31, sceNpClansSendInvitation),
        PS_HLE_FUNC(0x726DFFD5, sceNpClansCancelInvitation),
        PS_HLE_FUNC(0x095E12C6, sceNpClansSendInvitationResponse),
        PS_HLE_FUNC(0x59743B2B, sceNpClansSendMembershipRequest),
        PS_HLE_FUNC(0x299CCC9B, sceNpClansCancelMembershipRequest),
        PS_HLE_FUNC(0x942DBDC4, sceNpClansSendMembershipResponse),
        PS_HLE_FUNC(0x56BC5A7C, sceNpClansGetBlacklist),
        PS_HLE_FUNC(0x4D06AEF7, sceNpClansAddBlacklistEntry),
        PS_HLE_FUNC(0x5BFF9DA1, sceNpClansRemoveBlacklistEntry),
        PS_HLE_FUNC(0x727AA7F8, sceNpClansRetrieveAnnouncements),
        PS_HLE_FUNC(0xADA45B84, sceNpClansPostAnnouncement),
        PS_HLE_FUNC(0xE2590F60, sceNpClansRemoveAnnouncement),
        PS_HLE_FUNC(0x83D65529, sceNpClansPostChallenge),
        PS_HLE_FUNC(0x8E785B97, sceNpClansRetrievePostedChallenges),
        PS_HLE_FUNC(0xD3346DC4, sceNpClansRemovePostedChallenge),
        PS_HLE_FUNC(0x0DF25834, sceNpClansRetrieveChallenges),
        PS_HLE_FUNC(0xCE6DC0F0, sceNpClansRemoveChallenge),
    });
}

} // namespace pspocket::hle
