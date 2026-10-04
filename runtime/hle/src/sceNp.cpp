// sceNp: PlayStation Network. A non-goal (docs/phase0-gta5.md, D-024): every function reports "not initialized" or
// "offline" the way RPCS3 does when the user is not signed in, so single-player never waits on PSN.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNp.cpp/.h and sceNp2.cpp, with the NP state of
// rpcs3/Emu/NP/np_handler.cpp, np_contexts.cpp, np_helpers.cpp and signaling_handler.cpp.
//
// sceNpInit is not imported by GTA V; sceNp2Init (module sceNp2, registered here because it shares this state)
// initializes NP there. Until then the manager functions return SCE_NP_ERROR_NOT_INITIALIZED, as RPCS3 does before
// init; afterwards they report a user who is signed out (offline), and the callbacks and handlers registered are kept
// but never called.
//
// Offline is RPCS3's np_handler with PSN disabled and no network: get_psn_status() is SCE_NP_MANAGER_STATUS_OFFLINE,
// the user's NP ID is all zeros, no ticket has been received, no basic event is queued, no signaling connection exists
// and the local and public addresses are 0. Each function makes RPCS3's checks in RPCS3's order, then takes that path.

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNp";

enum : u32 {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x8002AA01,
    SCE_NP_ERROR_ALREADY_INITIALIZED = 0x8002AA02,
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x8002AA03,
    SCE_NP_ERROR_ID_NOT_FOUND = 0x8002AA06,
    SCE_NP_ERROR_INVALID_STATE = 0x8002AA0A,
    SCE_NP_ERROR_OFFLINE = 0x8002AA0C,
    SCE_NP_ERROR_INSUFFICIENT_BUFFER = 0x8002AA0F,
    SCE_NP_AUTH_EINVALID_ARGUMENT = 0x8002A015,
    SCE_NP_BASIC_ERROR_NOT_INITIALIZED = 0x8002A662,
    SCE_NP_BASIC_ERROR_INVALID_ARGUMENT = 0x8002A665,
    SCE_NP_BASIC_ERROR_NO_EVENT = 0x8002A66A,
    SCE_NP_BASIC_ERROR_EXCEEDS_MAX = 0x8002A66B,
    SCE_NP_BASIC_ERROR_NOT_REGISTERED = 0x8002A66D,
    SCE_NP_BASIC_ERROR_INVALID_DATA_ID = 0x8002A673,
    SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED = 0x8002A101,
    SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED = 0x8002A102,
    SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT = 0x8002A104,
    SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS = 0x8002A107,
    SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT = 0x8002A1A1,
    SCE_NP_COMMUNITY_ERROR_INVALID_ID = 0x8002A1A3,
    SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID = 0x8002A1A4,
    SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED = 0x8002A801,
    SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND = 0x8002A805,
    SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND = 0x8002A80E,
    SCE_NP_SIGNALING_ERROR_CTX_MAX = 0x8002A812,
    SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT = 0x8002A815,
    SCE_NP_SIGNALING_ERROR_OWN_NP_ID = 0x8002A816,
    SCE_NP_UTIL_ERROR_INVALID_ARGUMENT = 0x8002AB01,
    SCE_NP_UTIL_ERROR_NOT_MATCH = 0x8002AB0F,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr s32 SCE_NP_MANAGER_STATUS_OFFLINE = -1;
constexpr u32 SCE_NP_MIN_POOL_SIZE = 128 * 1024;
constexpr u32 SCE_NP_COOKIE_MAX_SIZE = 1024;
constexpr s32 SCE_NP_TICKET_PARAM_SERIAL_ID = 0, SCE_NP_TICKET_PARAM_SUBJECT_DOB = 11;
constexpr u32 SCE_NP_ENTITLEMENT_ID_SIZE = 32;
constexpr u32 SCE_NP_BASIC_MAX_PRESENCE_SIZE = 128;
constexpr u32 SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA = 1, SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS = 2,
              SCE_NP_BASIC_PRESENCE_OPTIONS_ALL_OPTIONS = 3;
constexpr u32 SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_CUSTOM_DATA_MESSAGE = 7;
constexpr u32 SCE_NP_BASIC_SELECTED_INVITATION_DATA = 1, SCE_NP_BASIC_SELECTED_MESSAGE_DATA = 2;
constexpr u32 SCE_NP_LOOKUP_MAX_CTX_NUM = 32;
constexpr u32 SCE_NP_SIGNALING_CTX_MAX = 8;
constexpr u32 SCE_NP_SIGNALING_CONN_STATUS_INACTIVE = 0;
constexpr u32 SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2 = 2;
constexpr u32 SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID = 1;
constexpr u32 SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN = 1;
constexpr u16 SCE_NP_PORT = 3658;

// Guest structures (RPCS3 sceNp.h).
// SceNpId (0x24 bytes): handle (SceNpOnlineId: data[16 + 1], dummy[3]), opt[8], reserved[8].
constexpr u32 kNpIdSize = 0x24, kOnlineIdLength = 16;
// SceNpBasicPresenceDetails2 (524 bytes): struct_size, state, title[128], status[192] at 136, comment[64], data[128],
// then the data's size at 520.
constexpr u32 kPresenceDetails2Size = 524, kPresence2Status = 136, kPresence2StatusMax = 192, kPresence2DataSize = 520;
// SceNpSignalingNetInfo (28 bytes): size, local_addr, mapped_addr, nat_status, upnp_status, npport_status, then the u16
// npport at 24; SceNpSignalingNetInfoDeprecated (24 bytes) stops before npport.
constexpr u32 kNetInfoSize = 28, kNetInfoDeprecatedSize = 24;

// The IDs of one kind of NP context, handed out as RPCS3's idm does (Emu/IdManager.cpp idm::allocate_id): Base,
// Base + 1, ... in order until each has been used once, then the lowest free one; 0, idm's invalid ID, when all are in
// use. Removing an ID or clearing the table never rewinds the order (idm keeps its highest index).
template <class T, u32 Base, u32 Count>
class IdTable {
public:
    u32 make(T value)
    {
        u32 i = next_;
        if (next_ < Count) next_++;
        else i = static_cast<u32>(std::find(slots_.begin(), slots_.end(), std::nullopt) - slots_.begin());
        if (i == Count) return 0;
        slots_[i] = std::move(value);
        return Base + i;
    }

    T* find(u32 id)
    {
        const u32 i = id - Base; // an ID below Base wraps past Count, as in idm's get_index
        return i < Count && slots_[i] ? &*slots_[i] : nullptr;
    }

    bool remove(u32 id)
    {
        if (!find(id)) return false;
        slots_[id - Base].reset();
        return true;
    }

    void clear()
    {
        for (std::optional<T>& s : slots_) s.reset();
    }

private:
    std::array<std::optional<T>, Count> slots_{};
    u32 next_ = 0; // idm's highest_index
};

// RPCS3 np_contexts.h signaling_ctx: the creator's NP ID and its connection event handlers. Offline no connection
// exists, so the handlers are never called.
struct SignalingCtx {
    std::array<u8, kNpIdSize> npid{};
    u32 handler = 0, arg = 0;
    u32 ext_handler = 0, ext_arg = 0; // sceNpSignalingAddExtendedHandler
};

struct NpState {
    std::mutex mutex;                     // everything below but `initialized`, which changes only with it held
    std::atomic<bool> initialized{false}; // sceNpInit's state (RPCS3 is_NP_init)
    bool np2_initialized = false;         // sceNp2Init's (is_NP2_init)
    u32 manager_cb = 0, manager_cb_arg = 0;
    bool basic_handler_registered = false;
    u32 basic_handler = 0, basic_handler_arg = 0;
    // sceNpLookupInit's state (is_NP_Lookup_init) and the lookup title contexts (idm lookup_title_ctx, IDs 0x3001 on;
    // each keeps its communication ID). RPCS3's NP term leaves both ("TODO: Other contexts"). There is no table of
    // lookup transaction contexts: offline sceNpLookupCreateTransactionCtx never makes one.
    bool lookup_initialized = false;
    IdTable<std::string, 0x3001, SCE_NP_LOOKUP_MAX_CTX_NUM> lookup_title_ctxs;
    // The signaling contexts (idm signaling_ctx, IDs 0x6001 on), cleared by NP term as in RPCS3 sceNpTerm.
    IdTable<SignalingCtx, 0x6001, SCE_NP_SIGNALING_CTX_MAX> signaling_ctxs;
};

NpState& np()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNp called without an HLE process");
    return p->state<NpState>();
}

bool basic_registered(NpState& s)
{
    std::lock_guard lock(s.mutex);
    return s.basic_handler_registered;
}

bool lookup_initialized(NpState& s)
{
    std::lock_guard lock(s.mutex);
    return s.lookup_initialized;
}

// A NUL-terminated guest string of at most `max` characters, for logs (cut where guest memory ends).
std::string guest_string(lv2::PpuThread& t, u32 addr, u32 max)
{
    std::string out;
    for (u32 i = 0; i < max && guest_readable(t, addr + i, 1); i++) {
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        out += c;
    }
    return out;
}

// A SceNpCommunicationId (data[9 + 1], num, dummy) written as RPCS3 does ("NPWR01234_00"), if it is a valid one
// (np::validate_communication_id: nine characters of 0-9 and A-Z, num at most 99).
std::optional<std::string> communication_id(u8* base, u32 addr)
{
    std::string id;
    for (u32 i = 0; i < 9; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return std::nullopt;
        id += c;
    }
    const u8 num = ps_read8(base, addr + 10);
    if (num > 99) return std::nullopt;
    return std::format("{}_{:02}", id, num);
}

// ------------------------------------------------------------------------------------------------- manager

s32 sceNpManagerGetStatus(PPUContext& ctx, u32 status)
{
    if (!np().initialized) {
        PS_LOG_DEBUG(kChan, "sceNpManagerGetStatus: NP not initialized");
        return error(SCE_NP_ERROR_NOT_INITIALIZED);
    }
    if (!status) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    ps_write32(guest_base(ctx), status, static_cast<u32>(SCE_NP_MANAGER_STATUS_OFFLINE));
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerRegisterCallback / sceNpManagerUnregisterCallback: the callback is kept for sign-in status
// changes; RPCS3 calls it from its NP handler when the user signs in or out, which never happens offline.
s32 sceNpManagerRegisterCallback(PPUContext&, u32 callback, u32 arg)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!callback) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    std::lock_guard lock(s.mutex);
    s.manager_cb = callback;
    s.manager_cb_arg = arg;
    PS_LOG_INFO(kChan, "sceNpManagerRegisterCallback({:#x}, arg {:#x}): kept; offline, it is never called", callback, arg);
    return lv2::CELL_OK;
}

s32 sceNpManagerUnregisterCallback(PPUContext&)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    s.manager_cb = 0; // RPCS3 leaves the argument
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerGetNpId, GetAccountRegion, GetAccountAge, GetContentRatingFlag, GetChatRestrictionFlag and
// GetNetworkTime ask about the signed-in user: NP must be initialized and every output pointer given; offline the
// answer is SCE_NP_ERROR_OFFLINE and nothing is written.
s32 manager_query_offline(std::string_view name, std::initializer_list<u32> outputs)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    for (const u32 out : outputs)
        if (!out) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "{}: offline", name);
    return error(SCE_NP_ERROR_OFFLINE);
}

s32 sceNpManagerGetNpId(PPUContext&, u32 np_id) { return manager_query_offline("sceNpManagerGetNpId", {np_id}); }

s32 sceNpManagerGetAccountRegion(PPUContext&, u32 country_code, u32 language)
{
    return manager_query_offline("sceNpManagerGetAccountRegion", {country_code, language});
}

s32 sceNpManagerGetAccountAge(PPUContext&, u32 age)
{
    return manager_query_offline("sceNpManagerGetAccountAge", {age});
}

s32 sceNpManagerGetContentRatingFlag(PPUContext&, u32 is_restricted, u32 age)
{
    return manager_query_offline("sceNpManagerGetContentRatingFlag", {is_restricted, age});
}

s32 sceNpManagerGetChatRestrictionFlag(PPUContext&, u32 is_restricted)
{
    return manager_query_offline("sceNpManagerGetChatRestrictionFlag", {is_restricted});
}

s32 sceNpManagerGetNetworkTime(PPUContext&, u32 tick)
{
    return manager_query_offline("sceNpManagerGetNetworkTime", {tick});
}

// RPCS3 sceNpManagerRequestTicket2: asks PSN's authentication server for a ticket; offline it is refused first.
s32 sceNpManagerRequestTicket2(PPUContext& ctx, u32 np_id, u32 version, u32 service_id, u32 cookie, u32 cookie_size,
    u32 entitlement_id, u32 consumed_count)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!np_id || !service_id || cookie_size > SCE_NP_COOKIE_MAX_SIZE) return error(SCE_NP_AUTH_EINVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpManagerRequestTicket2(npId {:#x}, version {:#x}, service \"{}\", cookie {:#x} ({} bytes), "
                       "entitlement {:#x}, consumed {}): offline, no ticket", np_id, version,
        guest_string(thread_of(ctx), service_id, 64), cookie, cookie_size, entitlement_id, consumed_count);
    return error(SCE_NP_ERROR_OFFLINE);
}

// RPCS3 sceNpManagerGetTicket: copies the ticket the last ticket request received. Offline there is none, so its size
// is 0: without a buffer that size is reported, with one nothing is copied and the size read is 0.
s32 sceNpManagerGetTicket(PPUContext& ctx, u32 buffer, u32 buffer_size)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!buffer_size) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpManagerGetTicket(buffer {:#x}): offline, no ticket", buffer);
    ps_write32(guest_base(ctx), buffer_size, 0);
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerGetTicketParam: one field of that ticket; offline there is none (SCE_NP_ERROR_INVALID_STATE).
s32 sceNpManagerGetTicketParam(PPUContext&, s32 param_id, u32 param)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!param || param_id < SCE_NP_TICKET_PARAM_SERIAL_ID || param_id > SCE_NP_TICKET_PARAM_SUBJECT_DOB) {
        return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    }
    return error(SCE_NP_ERROR_INVALID_STATE);
}

// RPCS3 sceNpManagerGetEntitlementIdList: a TODO that reports no entitlements (the count, 0) and writes nothing.
s32 sceNpManagerGetEntitlementIdList(PPUContext&, u32 list, u32 list_num)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpManagerGetEntitlementIdList(list {:#x}, {} entries): none", list, list_num);
    return 0;
}

// RPCS3 sceNpManagerGetEntitlementById: a TODO that finds no entitlement.
s32 sceNpManagerGetEntitlementById(PPUContext& ctx, u32 ent_id, u32 ent)
{
    if (!np().initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!ent_id) return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpManagerGetEntitlementById(\"{}\", ent {:#x}): not found",
        guest_string(thread_of(ctx), ent_id, SCE_NP_ENTITLEMENT_ID_SIZE), ent);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

// ------------------------------------------------------------------------------------------------- basic

// RPCS3 sceNpBasicRegisterContextSensitiveHandler / sceNpBasicUnregisterHandler: one basic event handler, kept for
// presence and messages from other users; offline none ever come, so it is never called.
s32 sceNpBasicRegisterContextSensitiveHandler(PPUContext&, u32 context, u32 handler, u32 arg)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (s.basic_handler_registered) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    if (!context || !handler) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    s.basic_handler_registered = true;
    s.basic_handler = handler;
    s.basic_handler_arg = arg;
    PS_LOG_INFO(kChan, "sceNpBasicRegisterContextSensitiveHandler(context {:#x}, handler {:#x}, arg {:#x}): kept; offline, "
                       "it is never called", context, handler, arg);
    return lv2::CELL_OK;
}

s32 sceNpBasicUnregisterHandler(PPUContext&)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (!s.basic_handler_registered) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    s.basic_handler_registered = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicGetEvent: the next queued basic event. Offline nothing is ever queued (presence and messages come
// from RPCN), so there is none.
s32 sceNpBasicGetEvent(PPUContext&, u32 event, u32 from, u32 data, u32 size)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (!event || !from || !data || !size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return error(SCE_NP_BASIC_ERROR_NO_EVENT);
}

// RPCS3 sceNpBasicSetPresence: the game's presence data, which RPCS3 keeps and sends to other users once online;
// offline it goes nowhere. RPCS3 ensure()s the data is readable ("Not checked by API"): refused here with a log.
s32 sceNpBasicSetPresence(PPUContext& ctx, u32 data, u32 size)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (size > SCE_NP_BASIC_MAX_PRESENCE_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    if (!guest_readable(thread_of(ctx), data, size)) {
        log_call_site(ctx, guest_base(ctx),
            std::format("sceNpBasicSetPresence: the data ({:#x}, {} bytes) is not guest memory", data, size));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    PS_LOG_DEBUG(kChan, "sceNpBasicSetPresence({:#x}, {} bytes): offline, not sent", data, size);
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicSetPresenceDetails2: presence status and data (SceNpBasicPresenceDetails2), kept and sent like
// sceNpBasicSetPresence's; offline they go nowhere. RPCS3 reads the struct unchecked: one that isn't guest memory is
// refused here with a log.
s32 sceNpBasicSetPresenceDetails2(PPUContext& ctx, u32 pres, u32 options)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (options < SCE_NP_BASIC_PRESENCE_OPTIONS_SET_DATA || options > SCE_NP_BASIC_PRESENCE_OPTIONS_ALL_OPTIONS) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, pres, kPresenceDetails2Size)) {
        log_call_site(ctx, base,
            std::format("sceNpBasicSetPresenceDetails2: the details {:#x} are not guest memory", pres));
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    const u32 struct_size = ps_read32(base, pres);
    if (struct_size != kPresenceDetails2Size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    const u32 size = ps_read32(base, pres + kPresence2DataSize);
    if (size > SCE_NP_BASIC_MAX_PRESENCE_SIZE) return error(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    PS_LOG_DEBUG(kChan, "sceNpBasicSetPresenceDetails2(options {}): status \"{}\", {} bytes of data; offline, not sent",
        options, (options & SCE_NP_BASIC_PRESENCE_OPTIONS_SET_STATUS) ? guest_string(t, pres + kPresence2Status,
                     kPresence2StatusMax) : "(unchanged)", size);
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicGetBlockListEntryCount / sceNpBasicGetBlockListEntry: the block list is the PSN account's; offline
// RPCS3 answers SCE_NP_ERROR_ID_NOT_FOUND ("TODO: Find the correct test") and writes nothing.
s32 sceNpBasicGetBlockListEntryCount(PPUContext&, u32 count)
{
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

s32 sceNpBasicGetBlockListEntry(PPUContext&, u32 index, u32 np_id)
{
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT); // RPCS3: "TODO: check index"
    PS_LOG_DEBUG(kChan, "sceNpBasicGetBlockListEntry({}): offline, no block list", index);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

// RPCS3 sceNpBasicGetMessageEntryCount: the type is checked before anything else ("TODO: verify this check and its
// location"); offline, SCE_NP_ERROR_ID_NOT_FOUND as for the block list.
s32 sceNpBasicGetMessageEntryCount(PPUContext&, u32 type, u32 count)
{
    if (type > SCE_NP_BASIC_MESSAGE_INFO_TYPE_BOOTABLE_CUSTOM_DATA_MESSAGE) {
        return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    }
    if (!np().initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

// RPCS3 sceNpBasicRecvMessageAttachmentLoad: the attachment of the invitation or message the user picked in the
// system's message screens. Offline nothing can be picked (np_handler::get_message_selected finds none), so the data
// ID is invalid there too.
s32 sceNpBasicRecvMessageAttachmentLoad(PPUContext&, u32 id, u32 buffer, u32 size)
{
    NpState& s = np();
    if (!s.initialized) return error(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!basic_registered(s)) return error(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    if (!buffer || !size) return error(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (id != SCE_NP_BASIC_SELECTED_INVITATION_DATA && id != SCE_NP_BASIC_SELECTED_MESSAGE_DATA) {
        return error(SCE_NP_BASIC_ERROR_INVALID_DATA_ID);
    }
    PS_LOG_DEBUG(kChan, "sceNpBasicRecvMessageAttachmentLoad({}): offline, nothing selected", id);
    return error(SCE_NP_BASIC_ERROR_INVALID_DATA_ID);
}

// ------------------------------------------------------------------------------------------------- lookup

// RPCS3 sceNpLookupInit / sceNpLookupTerm: the lookup (community) library on top of NP. Term leaves the title
// contexts, as RPCS3 does.
s32 sceNpLookupInit(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED);
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.lookup_initialized = true;
    PS_LOG_INFO(kChan, "sceNpLookupInit(): up; offline every lookup transaction is refused");
    return lv2::CELL_OK;
}

s32 sceNpLookupTerm(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!s.initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.lookup_initialized = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupCreateTitleCtx: returns the new title context's ID. selfNpId is only checked for null. Two
// deviations, both where RPCS3 has no answer for a real console: RPCS3 ensure()s the communication ID is valid
// (lookup_title_ctx's constructor), refused here with a log; and with all 32 contexts in use RPCS3 returns idm's
// invalid ID 0 as if it were a context, where its score and TUS contexts report
// SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS, as this does.
s32 sceNpLookupCreateTitleCtx(PPUContext& ctx, u32 communication_id_addr, u32 self_np_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!communication_id_addr || !self_np_id) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    u8* base = guest_base(ctx);
    const std::optional<std::string> id = communication_id(base, communication_id_addr);
    if (!id) {
        log_call_site(ctx, base, std::format("sceNpLookupCreateTitleCtx: {:#x} is not a valid communication ID (RPCS3 "
                                             "aborts)", communication_id_addr));
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    const u32 title_ctx = s.lookup_title_ctxs.make(*id);
    if (!title_ctx) {
        PS_LOG_ERROR(kChan, "sceNpLookupCreateTitleCtx({}): all {} title contexts are in use", *id,
            SCE_NP_LOOKUP_MAX_CTX_NUM);
        return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS);
    }
    PS_LOG_INFO(kChan, "sceNpLookupCreateTitleCtx({}): title context {:#x}", *id, title_ctx);
    return static_cast<s32>(title_ctx);
}

s32 sceNpLookupDestroyTitleCtx(PPUContext&, s32 title_ctx_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.lookup_initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!s.lookup_title_ctxs.remove(static_cast<u32>(title_ctx_id))) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupCreateTransactionCtx: a transaction talks to PSN, so offline none is made (the title context isn't
// looked at).
s32 sceNpLookupCreateTransactionCtx(PPUContext&, s32 title_ctx_id)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpLookupCreateTransactionCtx(title context {:#x}): offline, refused", title_ctx_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 sceNpLookupDestroyTransactionCtx: no transaction context exists offline, so no ID is valid.
s32 sceNpLookupDestroyTransactionCtx(PPUContext&, s32 trans_id)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpLookupDestroyTransactionCtx({:#x}): no such transaction", trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
}

// RPCS3 sceNpLookupSetTimeout: a TODO that checks the timeout (at least 10 s) but not the context ID.
s32 sceNpLookupSetTimeout(PPUContext&, s32 ctx_id, u64 timeout_us)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (timeout_us < 10'000'000) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpLookupSetTimeout({:#x}, {} us)", ctx_id, timeout_us);
    return lv2::CELL_OK;
}

// RPCS3 sceNpLookupNpIdAsync: the NP ID of an online ID, which PSN answers; offline refused before anything is
// written. The option must be null ("option check at least until fw 4.71").
s32 sceNpLookupNpIdAsync(PPUContext&, s32 trans_id, u32 online_id, u32 np_id, s32 prio, u32 option)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!online_id || !np_id) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpLookupNpIdAsync(transaction {:#x}, prio {}): offline", trans_id, prio);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 sceNpLookupTitleSmallStorageAsync: the title's small storage on PSN; offline refused before anything is
// written. The option must be null, as above.
s32 sceNpLookupTitleSmallStorageAsync(PPUContext&, s32 trans_id, u32 data, u32 max_size, u32 content_length, s32 prio,
    u32 option)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!data) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpLookupTitleSmallStorageAsync(transaction {:#x}, {} bytes, length {:#x}, prio {}): "
                        "offline", trans_id, max_size, content_length, prio);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 sceNpLookupPollAsync: a TODO that reports every transaction finished with result 0 (it checks neither the
// transaction ID nor the result pointer).
s32 sceNpLookupPollAsync(PPUContext& ctx, s32 trans_id, u32 result)
{
    if (!lookup_initialized(np())) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpLookupPollAsync({:#x}): finished, result 0", trans_id);
    ps_write32(guest_base(ctx), result, 0);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- signaling

// RPCS3 sceNpSignalingCreateCtx: a signaling context for peer-to-peer connections, holding the given NP ID and the
// connection event handler (kept; offline no connection ever calls it).
s32 sceNpSignalingCreateCtx(PPUContext& ctx, u32 np_id, u32 handler, u32 arg, u32 ctx_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!np_id || !ctx_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    SignalingCtx c;
    for (u32 i = 0; i < kNpIdSize; i++) c.npid[i] = ps_read8(base, np_id + i);
    c.handler = handler;
    c.arg = arg;
    const u32 id = s.signaling_ctxs.make(c);
    if (!id) return error(SCE_NP_SIGNALING_ERROR_CTX_MAX);
    ps_write32(base, ctx_id, id);
    PS_LOG_INFO(kChan, "sceNpSignalingCreateCtx(handler {:#x}, arg {:#x}): context {:#x}; offline, no connection ever "
                       "calls the handler", handler, arg, id);
    return lv2::CELL_OK;
}

s32 sceNpSignalingDestroyCtx(PPUContext&, u32 ctx_id)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!s.signaling_ctxs.remove(ctx_id)) return error(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingAddExtendedHandler: a second handler for the context's connection events (kept, never called
// offline). A null handler is accepted.
s32 sceNpSignalingAddExtendedHandler(PPUContext&, u32 ctx_id, u32 handler, u32 arg)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    SignalingCtx* c = s.signaling_ctxs.find(ctx_id);
    if (!c) return error(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    c->ext_handler = handler;
    c->ext_arg = arg;
    PS_LOG_INFO(kChan, "sceNpSignalingAddExtendedHandler(context {:#x}, handler {:#x}, arg {:#x}): kept; offline, it "
                       "is never called", ctx_id, handler, arg);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingGetLocalNetInfo: the console's local and public addresses (0 without a network) and RPCS3's
// guesses: NAT type 2, UPnP invalid (RPCS3 enables it only with a network), NP port open. The struct's size picks its
// layout: SceNpSignalingNetInfo (28 bytes, with the NP port) or the deprecated one without the port (24). The context
// ID is not checked.
s32 sceNpSignalingGetLocalNetInfo(PPUContext& ctx, u32 ctx_id, u32 info)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    u8* base = guest_base(ctx);
    if (!info) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    const u32 size = ps_read32(base, info);
    if (size != kNetInfoSize && size != kNetInfoDeprecatedSize) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    ps_write32(base, info + 4, 0); // local_addr
    ps_write32(base, info + 8, 0); // mapped_addr
    ps_write32(base, info + 12, SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2);
    ps_write32(base, info + 16, SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID);
    ps_write32(base, info + 20, SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN);
    if (size == kNetInfoSize) ps_write16(base, info + 24, SCE_NP_PORT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetLocalNetInfo(context {:#x}): no network", ctx_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpSignalingGetConnectionStatus, GetConnectionInfo, GetConnectionFromNpId, GetConnectionFromPeerAddress and
// TerminateConnection work on the signaling handler's connections. Offline there are none
// (sceNpSignalingActivateConnection, which would make one, needs RPCN), and RPCS3 checks neither ID against a context.
s32 sceNpSignalingGetConnectionStatus(PPUContext& ctx, u32 ctx_id, u32 conn_id, u32 conn_status, u32 peer_addr,
    u32 peer_port)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!conn_status) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionStatus(context {:#x}, connection {:#x}, addr {:#x}, port {:#x}): "
                        "inactive, no such connection", ctx_id, conn_id, peer_addr, peer_port);
    ps_write32(guest_base(ctx), conn_status, SCE_NP_SIGNALING_CONN_STATUS_INACTIVE);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

s32 sceNpSignalingGetConnectionInfo(PPUContext&, u32 ctx_id, u32 conn_id, s32 code, u32 info)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!info) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionInfo(context {:#x}, connection {:#x}, code {}): no such connection",
        ctx_id, conn_id, code);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

// The user's own NP ID is refused first. Offline it is all zeros, and np::is_same_npid compares the online IDs'
// first 16 characters as strings, so any NP ID with an empty online ID is the user's.
s32 sceNpSignalingGetConnectionFromNpId(PPUContext& ctx, u32 ctx_id, u32 np_id, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!np_id || !conn_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    if (ps_read8(guest_base(ctx), np_id) == 0) return error(SCE_NP_SIGNALING_ERROR_OWN_NP_ID);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionFromNpId(context {:#x}): no such connection", ctx_id);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

s32 sceNpSignalingGetConnectionFromPeerAddress(PPUContext&, u32 ctx_id, u32 peer_addr, u16 peer_port, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!conn_id) return error(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpSignalingGetConnectionFromPeerAddress(context {:#x}, {:#010x}:{}): no such connection",
        ctx_id, peer_addr, peer_port);
    return error(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

// Stopping a connection that doesn't exist does nothing.
s32 sceNpSignalingTerminateConnection(PPUContext&, u32 ctx_id, u32 conn_id)
{
    if (!np().initialized) return error(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpSignalingTerminateConnection(context {:#x}, connection {:#x}): no such connection",
        ctx_id, conn_id);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- util

// RPCS3 sceNpUtilCmpNpId: two NP IDs match when their online IDs do (np::is_same_npid: strncmp of 16 characters); the
// other fields are not compared. NP need not be initialized.
s32 sceNpUtilCmpNpId(PPUContext& ctx, u32 id1, u32 id2)
{
    if (!id1 || !id2) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    u8* base = guest_base(ctx);
    for (u32 i = 0; i < kOnlineIdLength; i++) {
        const u8 a = ps_read8(base, id1 + i);
        if (a != ps_read8(base, id2 + i)) return error(SCE_NP_UTIL_ERROR_NOT_MATCH);
        if (!a) break;
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- init and term

// RPCS3 sceNpInit, with NpState::mutex held. The pool would hold NP's working memory; nothing uses it offline.
u32 np_init_locked(NpState& s, u32 poolsize, u32 poolptr)
{
    if (s.initialized) return SCE_NP_ERROR_ALREADY_INITIALIZED;
    if (poolsize == 0 || !poolptr) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (poolsize < SCE_NP_MIN_POOL_SIZE) return SCE_NP_ERROR_INSUFFICIENT_BUFFER;
    s.initialized = true;
    return lv2::CELL_OK;
}

// RPCS3 sceNp2Init: sceNpInit (already initialized is fine), then NP2's own flag.
s32 sceNp2Init(PPUContext&, u32 poolsize, u32 poolptr)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (s.np2_initialized) return error(SCE_NP_ERROR_ALREADY_INITIALIZED);
    if (const u32 r = np_init_locked(s, poolsize, poolptr); r != lv2::CELL_OK && r != SCE_NP_ERROR_ALREADY_INITIALIZED) {
        return error(r);
    }
    s.np2_initialized = true;
    PS_LOG_INFO(kChan, "sceNp2Init(pool {:#x} bytes at {:#x}): NP up, no user signed in (offline)", poolsize, poolptr);
    return lv2::CELL_OK;
}

// RPCS3 sceNp2Term: matching 2 (never started here) and NP are terminated. NP term (RPCS3 sceNpTerm and
// np_handler::terminate_NP) drops the manager callback, the basic handler and the signaling contexts; the lookup state
// and its title contexts stay.
s32 sceNp2Term(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.np2_initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    s.np2_initialized = false;
    s.basic_handler_registered = false;
    s.manager_cb = s.manager_cb_arg = 0;
    s.signaling_ctxs.clear();
    return lv2::CELL_OK;
}

} // namespace

void register_sceNp(Registry& r)
{
    r.add("sceNp", {
        PS_HLE_FUNC(0xA7BFF757, sceNpManagerGetStatus),
        PS_HLE_FUNC(0xE7DCD3B4, sceNpManagerRegisterCallback),
        PS_HLE_FUNC(0x52A6B523, sceNpManagerUnregisterCallback),
        PS_HLE_FUNC(0xFE37A7F4, sceNpManagerGetNpId),
        PS_HLE_FUNC(0xB1E0718B, sceNpManagerGetAccountRegion),
        PS_HLE_FUNC(0x168FCECE, sceNpManagerGetAccountAge),
        PS_HLE_FUNC(0x6EE62ED2, sceNpManagerGetContentRatingFlag),
        PS_HLE_FUNC(0xEB7A3D84, sceNpManagerGetChatRestrictionFlag),
        PS_HLE_FUNC(0xBDC07FD5, sceNpManagerGetNetworkTime),
        PS_HLE_FUNC(0x8297F1EC, sceNpManagerRequestTicket2),
        PS_HLE_FUNC(0x0968AA36, sceNpManagerGetTicket),
        PS_HLE_FUNC(0x58FA4FCD, sceNpManagerGetTicketParam),
        PS_HLE_FUNC(0xB66D1C46, sceNpManagerGetEntitlementIdList),
        PS_HLE_FUNC(0xA1709ABD, sceNpManagerGetEntitlementById),
        PS_HLE_FUNC(0x4026EAC5, sceNpBasicRegisterContextSensitiveHandler),
        PS_HLE_FUNC(0xACB9EE8E, sceNpBasicUnregisterHandler),
        PS_HLE_FUNC(0xE035F7D6, sceNpBasicGetEvent),
        PS_HLE_FUNC(0x3F0808AA, sceNpBasicSetPresence),
        PS_HLE_FUNC(0x5E849303, sceNpBasicSetPresenceDetails2),
        PS_HLE_FUNC(0x73931BD0, sceNpBasicGetBlockListEntryCount),
        PS_HLE_FUNC(0xF2B3338A, sceNpBasicGetBlockListEntry),
        PS_HLE_FUNC(0xECD503DE, sceNpBasicGetMessageEntryCount),
        PS_HLE_FUNC(0x64A704CC, sceNpBasicRecvMessageAttachmentLoad),
        PS_HLE_FUNC(0x5F2D9257, sceNpLookupInit),
        PS_HLE_FUNC(0x8440537C, sceNpLookupTerm),
        PS_HLE_FUNC(0xCE81C7F0, sceNpLookupCreateTitleCtx),
        PS_HLE_FUNC(0x5DE61626, sceNpLookupDestroyTitleCtx),
        PS_HLE_FUNC(0xEA2E9FFC, sceNpLookupCreateTransactionCtx),
        PS_HLE_FUNC(0xFB87CF5E, sceNpLookupDestroyTransactionCtx),
        PS_HLE_FUNC(0x71E5AF7E, sceNpLookupSetTimeout),
        PS_HLE_FUNC(0xD12E40AE, sceNpLookupNpIdAsync),
        PS_HLE_FUNC(0x860B1756, sceNpLookupTitleSmallStorageAsync),
        PS_HLE_FUNC(0x7508112E, sceNpLookupPollAsync),
        PS_HLE_FUNC(0x6356082E, sceNpSignalingCreateCtx),
        PS_HLE_FUNC(0xA8CF8451, sceNpSignalingDestroyCtx),
        PS_HLE_FUNC(0x50B86D94, sceNpSignalingAddExtendedHandler),
        PS_HLE_FUNC(0x9AD7FBD1, sceNpSignalingGetLocalNetInfo),
        PS_HLE_FUNC(0xCA0A2D04, sceNpSignalingGetConnectionStatus),
        PS_HLE_FUNC(0x155DE760, sceNpSignalingGetConnectionInfo),
        PS_HLE_FUNC(0xE853D388, sceNpSignalingGetConnectionFromNpId),
        PS_HLE_FUNC(0x34CE82A0, sceNpSignalingGetConnectionFromPeerAddress),
        PS_HLE_FUNC(0x95C7BBA3, sceNpSignalingTerminateConnection),
        PS_HLE_FUNC(0xD208F91D, sceNpUtilCmpNpId),
    });
    r.add("sceNp2", {
        PS_HLE_FUNC(0x41251F74, sceNp2Init),
        PS_HLE_FUNC(0xAADB7C12, sceNp2Term),
    });
}

} // namespace pspocket::hle
