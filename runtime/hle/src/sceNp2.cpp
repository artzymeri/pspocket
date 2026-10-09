// sceNp2: NP 2's start and stop, and the matching 2 library (worlds, lobbies and rooms on PSN's matching servers).
// Offline as RPCS3 with no user signed in (D-059). NP starts and stops with sceNp2Init and sceNp2Term (through
// sceNp.cpp's np_init and np_term, np.hpp) and matching 2 initializes, but sceNpMatching2CreateContext refuses every
// context (SCE_NP_MATCHING2_ERROR_NOT_NP_SIGN_IN), so no matching 2 context ever exists. Each function that looks one
// up (RPCS3 check_match2_context, get_match2_context, destroy_match2_context) makes RPCS3's checks before that lookup,
// then finds none: no request reaches the point where RPCS3 would send it to a server, nothing is written to the
// game's output pointers, and the context, request and signaling callbacks, which RPCS3 keeps in a context, are never
// kept or called. The room event and room message callbacks need no context: they are kept as RPCS3 keeps them and
// never called (no room is ever joined).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNp2.cpp/.h, with rpcs3/Emu/NP/np_handler.cpp (get_match2_event,
// terminate_NP) and np_contexts.cpp (create_match2_context and the context lookups).

#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "np.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNp2";

enum : u32 {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x8002AA01,
    SCE_NP_ERROR_ALREADY_INITIALIZED = 0x8002AA02,
    SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED = 0x80022302,
    SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED = 0x80022303,
    SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND = 0x80022306,
    SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT = 0x8002230A,
    SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID = 0x8002230B,
    SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID = 0x8002230F,
    SCE_NP_MATCHING2_ERROR_INVALID_MEMBER_ID = 0x80022310,
    SCE_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_ID = 0x80022311,
    SCE_NP_MATCHING2_ERROR_INVALID_SORT_METHOD = 0x80022313,
    SCE_NP_MATCHING2_ERROR_NOT_NP_SIGN_IN = 0x80022334,
    SCE_NP_MATCHING2_SERVER_ERROR_SERVICE_UNAVAILABLE = 0x80022B02,
    SCE_NP_OAUTH_ERROR_ALREADY_INITIALIZED = 0x80025F02,
    SCE_NP_OAUTH_ERROR_NOT_INITIALIZED = 0x80025F03,
    SCE_NP_UTIL_ERROR_INVALID_ARGUMENT = 0x8002AB01,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// Room member attributes (SceNpMatching2AttributeId, be16): the internal binary ones, 0x59 up to the user ones (0x5F).
constexpr u16 SCE_NP_MATCHING2_ROOMMEMBER_BIN_ATTR_INTERNAL_1_ID = 0x59, SCE_NP_MATCHING2_USER_BIN_ATTR_1_ID = 0x5F;
constexpr s32 SCE_NP_MATCHING2_SORT_METHOD_JOIN_DATE = 0, SCE_NP_MATCHING2_SORT_METHOD_SLOT_NUMBER = 1;
constexpr s32 SCE_NP_SIGNALING_CONN_STATUS_INACTIVE = 0;
constexpr u32 SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2 = 2;
// SceNpMatching2CbQueueInfo (48 bytes: nine be32, the request, session event and session message queues' length,
// current and maximum length each; reserved[12]); SceNpMatching2MemoryInfo (24: be32 total, current and maximum use;
// reserved[12]); SceNpMatching2SignalingNetInfo (16: be32 size, local and mapped address, NAT status).
constexpr u32 kCbQueueInfoSize = 48, kMemoryInfoSize = 24, kSignalingNetInfoSize = 16;
constexpr u32 kCbQueueLen = 255; // RPCS3: "TODO: check the values returned on real hardware"

// RPCS3 np_handler's NP 2 state.
struct Np2State {
    std::mutex mutex;
    bool np2_initialized = false;    // sceNp2Init's (is_NP2_init)
    bool match2_initialized = false; // sceNpMatching2Init2's (is_NP2_Match2_init)
    bool oauth_initialized = false;  // sceNpAuthOAuthInit's (is_NP_Auth_init)
    // The room event and room message callbacks, each with the context and argument it was registered with
    // (np_handler room_event_cb, room_event_cb_ctx, room_event_cb_arg and room_msg_cb...). NP term drops them.
    u32 room_event_cb = 0, room_event_cb_arg = 0;
    u16 room_event_cb_ctx = 0;
    u32 room_msg_cb = 0, room_msg_cb_arg = 0;
    u16 room_msg_cb_ctx = 0;
};

Np2State& np2()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNp2 called without an HLE process");
    return p->state<Np2State>();
}

bool match2_initialized()
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    return s.match2_initialized;
}

// The nine characters of the SceNpCommunicationId at `addr`, for logs.
std::string communication_id_text(PPUContext& ctx, u32 addr)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, addr, 9)) return "(not guest memory)";
    std::string text;
    for (u32 i = 0; i < 9; i++) {
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        text += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return text;
}

// The room callbacks NP's term drops (np_handler::terminate_NP), with Np2State::mutex held.
void drop_room_callbacks(Np2State& s)
{
    s.room_event_cb = s.room_event_cb_arg = 0;
    s.room_event_cb_ctx = 0;
    s.room_msg_cb = s.room_msg_cb_arg = 0;
    s.room_msg_cb_ctx = 0;
}

// ------------------------------------------------------------------------------------------------- NP 2

// RPCS3 sceNp2Init: NP (sceNpInit; already up is fine), then NP 2's own flag.
s32 sceNp2Init(PPUContext&, u32 poolsize, u32 poolptr)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (s.np2_initialized) return error(SCE_NP_ERROR_ALREADY_INITIALIZED);
    if (const u32 r = np_init(poolsize, poolptr); r != lv2::CELL_OK && r != SCE_NP_ERROR_ALREADY_INITIALIZED) {
        return error(r);
    }
    s.np2_initialized = true;
    PS_LOG_INFO(kChan, "sceNp2Init(pool {:#x} bytes at {:#x}): NP up, no user signed in (offline)", poolsize, poolptr);
    return lv2::CELL_OK;
}

// RPCS3 sceNp2Term: matching 2's term (sceNpMatching2Term, whose result RPCS3 ignores: matching 2 down, its contexts
// dropped, none offline), NP's (sceNpTerm, np_term), then NP 2's flag. NP's term also drops the room callbacks kept
// here (np_handler::terminate_NP).
s32 sceNp2Term(PPUContext&)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (!s.np2_initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.match2_initialized = false;
    if (np_term() == lv2::CELL_OK) drop_room_callbacks(s);
    s.np2_initialized = false;
    PS_LOG_INFO(kChan, "sceNp2Term(): NP down");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- matching 2

// RPCS3 sceNpMatching2Init2: needs NP 2. RPCS3 makes none of the thread, heap and queues this would set up ("TODO");
// the thread's stack size and priority and the SceNpMatching2UtilityInitParam are only logged.
s32 sceNpMatching2Init2(PPUContext&, u32 stack_size, s32 priority, u32 param)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (!s.np2_initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (s.match2_initialized) return error(SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED);
    s.match2_initialized = true;
    PS_LOG_INFO(kChan, "sceNpMatching2Init2(stack {:#x}, priority {}, param {:#x}): up; offline no context can be "
                       "created", stack_size, priority, param);
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2Term2: needs NP 2, then matching 2; drops the contexts (none offline).
s32 sceNpMatching2Term2(PPUContext&)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (!s.np2_initialized) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!s.match2_initialized) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    s.match2_initialized = false;
    PS_LOG_INFO(kChan, "sceNpMatching2Term2(): matching 2 down");
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2Term: the name before SDK 2.4.0 of sceNpMatching2Term2, which it calls (D-574).
s32 sceNpMatching2Term(PPUContext& ctx) { return sceNpMatching2Term2(ctx); }

// RPCS3 sceNpMatching2CreateContext: every pointer is needed, then a signed-in user (get_psn_status() is ONLINE):
// offline SCE_NP_MATCHING2_ERROR_NOT_NP_SIGN_IN, and no context ID is written.
s32 sceNpMatching2CreateContext(PPUContext& ctx, u32 np_id, u32 comm_id, u32 pass_phrase, u32 ctx_id, s32 option)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!np_id || !comm_id || !pass_phrase || !ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpMatching2CreateContext(communication ID {}, option {:#x}): no user signed in (offline), "
                       "no context", communication_id_text(ctx, comm_id), option);
    return error(SCE_NP_MATCHING2_ERROR_NOT_NP_SIGN_IN);
}

// RPCS3 sceNpMatching2DestroyContext: destroy_match2_context finds no context.
s32 sceNpMatching2DestroyContext(PPUContext&, u16 ctx_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpMatching2DestroyContext({}): no such context", ctx_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2ContextStartAsync: a context ID, then the context, which RPCS3 would mark started, reporting
// SCE_NP_MATCHING2_CONTEXT_EVENT_Start to its context callback through cellSysutilCheckCallback. There is no context,
// so no event either.
s32 sceNpMatching2ContextStartAsync(PPUContext&, u16 ctx_id, u32 timeout)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    PS_LOG_INFO(kChan, "sceNpMatching2ContextStartAsync({}, timeout {}): no such context", ctx_id, timeout);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2ContextStop: the context (the ID isn't checked first), which RPCS3 would stop, reporting
// SCE_NP_MATCHING2_CONTEXT_EVENT_Stop. There is none.
s32 sceNpMatching2ContextStop(PPUContext&, u16 ctx_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpMatching2ContextStop({}): no such context", ctx_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2RegisterContextCallback: a context ID, then the context, which would keep the callback.
s32 sceNpMatching2RegisterContextCallback(PPUContext&, u16 ctx_id, u32 cb, u32 cb_arg)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    PS_LOG_INFO(kChan, "sceNpMatching2RegisterContextCallback(context {}, callback {:#x}, arg {:#x}): no such context",
        ctx_id, cb, cb_arg);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2RegisterSignalingCallback: the context (the ID isn't checked first), which would keep the
// callback for its peers' connections.
s32 sceNpMatching2RegisterSignalingCallback(PPUContext&, u16 ctx_id, u32 cb, u32 cb_arg)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpMatching2RegisterSignalingCallback(context {}, callback {:#x}, arg {:#x}): no such "
                       "context", ctx_id, cb, cb_arg);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2RegisterRoomEventCallback and sceNpMatching2RegisterRoomMessageCallback: one callback each for
// the events and messages of the rooms joined, kept with the context given (not checked) and its argument, replacing
// the one before; a null callback is kept too. Offline no room is joined, so they are never called.
s32 sceNpMatching2RegisterRoomEventCallback(PPUContext&, u16 ctx_id, u32 cb, u32 cb_arg)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (!s.match2_initialized) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    s.room_event_cb = cb;
    s.room_event_cb_ctx = ctx_id;
    s.room_event_cb_arg = cb_arg;
    PS_LOG_INFO(kChan, "sceNpMatching2RegisterRoomEventCallback(context {}, callback {:#x}, arg {:#x}): kept; offline "
                       "it is never called", ctx_id, cb, cb_arg);
    return lv2::CELL_OK;
}

s32 sceNpMatching2RegisterRoomMessageCallback(PPUContext&, u16 ctx_id, u32 cb, u32 cb_arg)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (!s.match2_initialized) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    s.room_msg_cb = cb;
    s.room_msg_cb_ctx = ctx_id;
    s.room_msg_cb_arg = cb_arg;
    PS_LOG_INFO(kChan, "sceNpMatching2RegisterRoomMessageCallback(context {}, callback {:#x}, arg {:#x}): kept; "
                       "offline it is never called", ctx_id, cb, cb_arg);
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2RegisterLobbyEventCallback and sceNpMatching2RegisterLobbyMessageCallback: TODOs that check
// only that matching 2 is up, and keep nothing.
s32 sceNpMatching2RegisterLobbyEventCallback(PPUContext&, u16 ctx_id, u32 cb, u32 cb_arg)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpMatching2RegisterLobbyEventCallback(context {}, callback {:#x}, arg {:#x}): a TODO in "
                       "RPCS3, not kept", ctx_id, cb, cb_arg);
    return lv2::CELL_OK;
}

s32 sceNpMatching2RegisterLobbyMessageCallback(PPUContext&, u16 ctx_id, u32 cb, u32 cb_arg)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpMatching2RegisterLobbyMessageCallback(context {}, callback {:#x}, arg {:#x}): a TODO in "
                       "RPCS3, not kept", ctx_id, cb, cb_arg);
    return lv2::CELL_OK;
}

// RPCS3 generic_match2_error_check, the checks each request to the matching server makes first: matching 2 up, the
// request's parameters given, a context ID, the context. There is no context, so the request ends there: RPCS3's own
// checks of the parameters and the request itself are never reached, and no request ID is written.
s32 request_offline(std::string_view name, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!req_param) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    PS_LOG_INFO(kChan, "{}(context {}, request {:#x}, options {:#x}, request ID to {:#x}): no such context", name, ctx_id,
        req_param, opt_param, assigned_req_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

s32 sceNpMatching2GetWorldInfoList(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2GetWorldInfoList", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2GetServerInfo(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2GetServerInfo", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2SearchRoom(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2SearchRoom", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2CreateJoinRoom(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2CreateJoinRoom", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2JoinRoom(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2JoinRoom", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2LeaveRoom(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2LeaveRoom", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2SetRoomDataExternal(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2SetRoomDataExternal", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2SetRoomDataInternal(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)
{
    return request_offline("sceNpMatching2SetRoomDataInternal", ctx_id, req_param, opt_param, assigned_req_id);
}

s32 sceNpMatching2GetRoomMemberDataExternalList(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param,
    u32 assigned_req_id)
{
    return request_offline("sceNpMatching2GetRoomMemberDataExternalList", ctx_id, req_param, opt_param,
        assigned_req_id);
}

// RPCS3 sceNpMatching2AbortRequest: a context ID, then the context, then the request (np_handler::abort_request).
s32 sceNpMatching2AbortRequest(PPUContext&, u16 ctx_id, u32 req_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    PS_LOG_INFO(kChan, "sceNpMatching2AbortRequest(context {}, request {:#x}): no such context", ctx_id, req_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2GetServerIdListLocal: the context (the ID isn't checked first), whose servers it would copy and
// count. There is none, so nothing is written.
s32 sceNpMatching2GetServerIdListLocal(PPUContext&, u16 ctx_id, u32 server_id, u32 server_id_num)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpMatching2GetServerIdListLocal(context {}, {} IDs at {:#x}): no such context", ctx_id,
        server_id_num, server_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2GetEventData: copies, once, the data a request's result or an event left under its key
// (np_handler::get_match2_event) and returns the number of bytes copied. Offline nothing is ever left under any key:
// 0 bytes, nothing written. The context isn't checked.
s32 sceNpMatching2GetEventData(PPUContext&, u16 ctx_id, u32 event_key, u32 buf, u32 buf_len)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!buf) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpMatching2GetEventData(context {}, key {:#x}, {} bytes at {:#x}): no data under that key",
        ctx_id, event_key, buf_len, buf);
    return 0;
}

// RPCS3 sceNpMatching2ClearEventData: a TODO that checks only that matching 2 is up.
s32 sceNpMatching2ClearEventData(PPUContext&, u16 ctx_id, u32 event_key)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpMatching2ClearEventData(context {}, key {:#x}): a TODO in RPCS3", ctx_id, event_key);
    return lv2::CELL_OK;
}

// The other requests to the matching server: RPCS3's generic_match2_error_check ends them all offline (RPCS3's
// TODOs among them, which would answer CELL_OK past it).
#define PS_MATCH2_REQUEST(fn)                                                                                          \
    s32 fn(PPUContext&, u16 ctx_id, u32 req_param, u32 opt_param, u32 assigned_req_id)                                 \
    {                                                                                                                  \
        return request_offline(#fn, ctx_id, req_param, opt_param, assigned_req_id);                                    \
    }
PS_MATCH2_REQUEST(sceNpMatching2CreateServerContext)
PS_MATCH2_REQUEST(sceNpMatching2DeleteServerContext)
PS_MATCH2_REQUEST(sceNpMatching2GetLobbyInfoList)
PS_MATCH2_REQUEST(sceNpMatching2GetLobbyMemberDataInternal)
PS_MATCH2_REQUEST(sceNpMatching2GetLobbyMemberDataInternalList)
PS_MATCH2_REQUEST(sceNpMatching2GetRoomDataExternalList)
PS_MATCH2_REQUEST(sceNpMatching2GetRoomDataInternal)
PS_MATCH2_REQUEST(sceNpMatching2GetRoomMemberDataInternal)
PS_MATCH2_REQUEST(sceNpMatching2GetUserInfoList)
PS_MATCH2_REQUEST(sceNpMatching2GrantRoomOwner)
PS_MATCH2_REQUEST(sceNpMatching2JoinLobby)
PS_MATCH2_REQUEST(sceNpMatching2JoinProhibitiveRoom)
PS_MATCH2_REQUEST(sceNpMatching2KickoutRoomMember)
PS_MATCH2_REQUEST(sceNpMatching2LeaveLobby)
PS_MATCH2_REQUEST(sceNpMatching2SendLobbyChatMessage)
PS_MATCH2_REQUEST(sceNpMatching2SendLobbyInvitation)
PS_MATCH2_REQUEST(sceNpMatching2SendRoomChatMessage)
PS_MATCH2_REQUEST(sceNpMatching2SendRoomMessage)
PS_MATCH2_REQUEST(sceNpMatching2SetLobbyMemberDataInternal)
PS_MATCH2_REQUEST(sceNpMatching2SetRoomMemberDataInternal)
PS_MATCH2_REQUEST(sceNpMatching2SetSignalingOptParam)
PS_MATCH2_REQUEST(sceNpMatching2SetUserInfo)
PS_MATCH2_REQUEST(sceNpMatching2SignalingGetPingInfo)
#undef PS_MATCH2_REQUEST

// RPCS3 sceNpMatching2Init: sceNpMatching2Init2 without the utility parameter (SDKs before 2.4.0).
s32 sceNpMatching2Init(PPUContext& ctx, u32 stack_size, s32 priority)
{
    return sceNpMatching2Init2(ctx, stack_size, priority, 0);
}

// RPCS3 sceNpMatching2ContextStart: as sceNpMatching2ContextStartAsync without the ID check; no context.
s32 sceNpMatching2ContextStart(PPUContext&, u16 ctx_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpMatching2ContextStart({}): no such context", ctx_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

// RPCS3 sceNpMatching2AbortContextStart: a TODO that checks only that matching 2 is up.
s32 sceNpMatching2AbortContextStart(PPUContext&, u16 ctx_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpMatching2AbortContextStart({}): nothing is starting", ctx_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2SetDefaultRequestOptParam: options for the context's later requests; offline no context
// (SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID, as RPCS3 answers for an unknown one).
s32 sceNpMatching2SetDefaultRequestOptParam(PPUContext&, u16 ctx_id, u32 opt_param)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!opt_param) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    PS_LOG_INFO(kChan, "sceNpMatching2SetDefaultRequestOptParam(context {}): no such context", ctx_id);
    return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
}

// Whether `size` bytes at `addr` are guest memory; if not, the call site is logged (RPCS3 would fault).
bool usable(PPUContext& ctx, std::string_view name, u32 addr, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return true;
    log_call_site(ctx, t.kernel.base(), std::format("{}: {:#x} ({} bytes) is not guest memory", name, addr, size));
    return false;
}

// RPCS3 sceNpMatching2GetCbQueueInfo: the callback queues' lengths (255 each, the current and maximum lengths 0); the
// context isn't looked at.
s32 sceNpMatching2GetCbQueueInfo(PPUContext& ctx, u16 ctx_id, u32 queue_info)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!queue_info) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, "sceNpMatching2GetCbQueueInfo", queue_info, kCbQueueInfoSize)) {
        return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    std::memset(base + queue_info, 0, kCbQueueInfoSize);
    for (const u32 field : {0u, 12u, 24u}) ps_write32(base, queue_info + field, kCbQueueLen);
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetCbQueueInfo(context {})", ctx_id);
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2GetMemoryInfo: the NP pool's size, and how much of it is in use (none offline).
s32 sceNpMatching2GetMemoryInfo(PPUContext& ctx, u32 mem_info)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!mem_info) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, "sceNpMatching2GetMemoryInfo", mem_info, kMemoryInfoSize)) {
        return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    std::memset(base + mem_info, 0, kMemoryInfoSize);
    ps_write32(base, mem_info, np_pool_size());
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2GetClanLobbyId: SCE_NP_MATCHING2_SERVER_ERROR_SERVICE_UNAVAILABLE, so that games don't need
// clan lobbies to go on.
s32 sceNpMatching2GetClanLobbyId(PPUContext&, u16 ctx_id, u32 clan_id, u32 lobby_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetClanLobbyId(context {}, clan {}, {:#x}): service unavailable", ctx_id, clan_id,
        lobby_id);
    return error(SCE_NP_MATCHING2_SERVER_ERROR_SERVICE_UNAVAILABLE);
}

// RPCS3 sceNpMatching2GetLobbyMemberIdListLocal: a TODO that answers 0 members.
s32 sceNpMatching2GetLobbyMemberIdListLocal(PPUContext&, u16 ctx_id, u64 lobby_id, u32 member_id, u32 member_id_num,
    u32 me)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetLobbyMemberIdListLocal(context {}, lobby {:#x}, {:#x} ({}), {:#x}): none",
        ctx_id, lobby_id, member_id, member_id_num, me);
    return lv2::CELL_OK;
}

// The local room queries (RPCS3 GetRoomMemberIdListLocal, GetRoomPasswordLocal, GetRoomSlotInfoLocal,
// GetSignalingOptParamLocal, GetRoomMemberDataInternalLocal): a context ID and a room ID, then the context: none.
s32 room_local_offline(std::string_view name, u16 ctx_id, u64 room_id)
{
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    if (!room_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID);
    PS_LOG_DEBUG(kChan, "{}(context {}, room {:#x}): no such context", name, ctx_id, room_id);
    return error(SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND);
}

s32 sceNpMatching2GetRoomMemberIdListLocal(PPUContext&, u16 ctx_id, u64 room_id, s32 sort_method, u32 member_id,
    u32 member_id_num)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    if (!room_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID);
    if (sort_method != SCE_NP_MATCHING2_SORT_METHOD_JOIN_DATE && sort_method != SCE_NP_MATCHING2_SORT_METHOD_SLOT_NUMBER) {
        return error(SCE_NP_MATCHING2_ERROR_INVALID_SORT_METHOD);
    }
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetRoomMemberIdListLocal: {:#x} ({})", member_id, member_id_num);
    return room_local_offline("sceNpMatching2GetRoomMemberIdListLocal", ctx_id, room_id);
}

s32 sceNpMatching2GetRoomPasswordLocal(PPUContext&, u16 ctx_id, u64 room_id, u32 with_password, u32 room_password)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetRoomPasswordLocal: {:#x} {:#x}", with_password, room_password);
    return room_local_offline("sceNpMatching2GetRoomPasswordLocal", ctx_id, room_id);
}

s32 sceNpMatching2GetRoomSlotInfoLocal(PPUContext&, u16 ctx_id, u64 room_id, u32 slot_info)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!slot_info) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    return room_local_offline("sceNpMatching2GetRoomSlotInfoLocal", ctx_id, room_id);
}

s32 sceNpMatching2GetSignalingOptParamLocal(PPUContext&, u16 ctx_id, u64 room_id, u32 opt_param)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetSignalingOptParamLocal: {:#x}", opt_param);
    return room_local_offline("sceNpMatching2GetSignalingOptParamLocal", ctx_id, room_id);
}

// The member and the attribute IDs (internal room member binary attributes, 0x59 to 0x5E) are checked before the
// context. RPCS3 reads the IDs unchecked: ones that aren't guest memory are refused here with a log.
s32 sceNpMatching2GetRoomMemberDataInternalLocal(PPUContext& ctx, u16 ctx_id, u64 room_id, u16 member_id, u32 attr_id,
    u32 attr_id_num, u32 member, u32 buf, u32 buf_len)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!ctx_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);
    if (!room_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID);
    if (!member_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_MEMBER_ID);
    for (u32 i = 0; i < attr_id_num; i++) {
        if (!attr_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_ID);
        if (!usable(ctx, "sceNpMatching2GetRoomMemberDataInternalLocal", attr_id + 2 * i, 2)) {
            return error(SCE_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_ID);
        }
        const u16 id = ps_read16(guest_base(ctx), attr_id + 2 * i);
        if (id < SCE_NP_MATCHING2_ROOMMEMBER_BIN_ATTR_INTERNAL_1_ID || id >= SCE_NP_MATCHING2_USER_BIN_ATTR_1_ID) {
            return error(SCE_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_ID);
        }
    }
    PS_LOG_DEBUG(kChan, "sceNpMatching2GetRoomMemberDataInternalLocal: member {}, {:#x} {:#x} ({})", member_id, member,
        buf, buf_len);
    return room_local_offline("sceNpMatching2GetRoomMemberDataInternalLocal", ctx_id, room_id);
}

// ------------------------------------------------------------------------------------------------- matching 2 signaling

// RPCS3 sceNpMatching2SignalingGetConnectionStatus / GetConnectionInfo: the member's NP ID comes from the rooms in
// RPCS3's cache, which only server replies fill: offline the room is never there
// (SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID); the status is INACTIVE.
s32 sceNpMatching2SignalingGetConnectionStatus(PPUContext& ctx, u16 ctx_id, u64 room_id, u16 member_id,
    u32 conn_status, u32 peer_addr, u32 peer_port)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!conn_status) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, "sceNpMatching2SignalingGetConnectionStatus", conn_status, 4)) {
        return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    }
    ps_write32(guest_base(ctx), conn_status, static_cast<u32>(SCE_NP_SIGNALING_CONN_STATUS_INACTIVE));
    PS_LOG_DEBUG(kChan, "sceNpMatching2SignalingGetConnectionStatus(context {}, room {:#x}, member {}, {:#x} {:#x}): no "
                        "such room", ctx_id, room_id, member_id, peer_addr, peer_port);
    return error(SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID);
}

s32 sceNpMatching2SignalingGetConnectionInfo(PPUContext&, u16 ctx_id, u64 room_id, u16 member_id, s32 code,
    u32 conn_info)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!conn_info) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    PS_LOG_DEBUG(kChan, "sceNpMatching2SignalingGetConnectionInfo(context {}, room {:#x}, member {}, code {}): no such "
                        "room", ctx_id, room_id, member_id, code);
    return error(SCE_NP_MATCHING2_ERROR_INVALID_ROOM_ID);
}

// RPCS3 sceNpMatching2SignalingGetLocalNetInfo: the struct's size must be its own (16); the addresses are 0 without a
// network, the NAT type RPCS3's guess (2).
s32 sceNpMatching2SignalingGetLocalNetInfo(PPUContext& ctx, u32 net_info)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!net_info || !usable(ctx, "sceNpMatching2SignalingGetLocalNetInfo", net_info, kSignalingNetInfoSize)) {
        return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, net_info) != kSignalingNetInfoSize) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    ps_write32(base, net_info + 4, 0);
    ps_write32(base, net_info + 8, 0);
    ps_write32(base, net_info + 12, SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2);
    return lv2::CELL_OK;
}

// RPCS3 sceNpMatching2SignalingGetPeerNetInfo, ...GetPeerNetInfoResult (a net info of the right size),
// ...CancelPeerNetInfo, ...GetCtxOpt and ...SetCtxOpt: TODOs that answer CELL_OK after their checks, writing nothing.
s32 sceNpMatching2SignalingGetPeerNetInfo(PPUContext&, u16 ctx_id, u64 room_id, u16 member_id, u32 req_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!req_id) return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    PS_LOG_WARN(kChan, "sceNpMatching2SignalingGetPeerNetInfo(context {}, room {:#x}, member {}): a TODO in RPCS3; no "
                       "request is made", ctx_id, room_id, member_id);
    return lv2::CELL_OK;
}

s32 sceNpMatching2SignalingGetPeerNetInfoResult(PPUContext& ctx, u16 ctx_id, u32 req_id, u32 net_info)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    if (!net_info || !usable(ctx, "sceNpMatching2SignalingGetPeerNetInfoResult", net_info, kSignalingNetInfoSize) ||
        ps_read32(guest_base(ctx), net_info) != kSignalingNetInfoSize) {
        return error(SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    }
    PS_LOG_WARN(kChan, "sceNpMatching2SignalingGetPeerNetInfoResult(context {}, request {:#x}): a TODO in RPCS3; nothing "
                       "written", ctx_id, req_id);
    return lv2::CELL_OK;
}

s32 sceNpMatching2SignalingCancelPeerNetInfo(PPUContext&, u16 ctx_id, u32 req_id)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "sceNpMatching2SignalingCancelPeerNetInfo(context {}, request {:#x})", ctx_id, req_id);
    return lv2::CELL_OK;
}

s32 sceNpMatching2SignalingGetCtxOpt(PPUContext&, u16 ctx_id, s32 optname, u32 optval)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpMatching2SignalingGetCtxOpt(context {}, option {}, {:#x}): a TODO in RPCS3; nothing "
                       "written", ctx_id, optname, optval);
    return lv2::CELL_OK;
}

s32 sceNpMatching2SignalingSetCtxOpt(PPUContext&, u16 ctx_id, s32 optname, s32 optval)
{
    if (!match2_initialized()) return error(SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    PS_LOG_WARN(kChan, "sceNpMatching2SignalingSetCtxOpt(context {}, option {}, {}): a TODO in RPCS3", ctx_id, optname,
        optval);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- OAuth

// RPCS3 sceNpAuthOAuthInit / Term and the request functions: the library's flag (term clears it unchecked); the
// requests are TODOs that answer CELL_OK, writing nothing (no request ID, no authorization code).
s32 sceNpAuthOAuthInit(PPUContext&)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    if (s.oauth_initialized) return error(SCE_NP_OAUTH_ERROR_ALREADY_INITIALIZED);
    s.oauth_initialized = true;
    return lv2::CELL_OK;
}

s32 sceNpAuthOAuthTerm(PPUContext&)
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    s.oauth_initialized = false;
    return lv2::CELL_OK;
}

s32 oauth_todo(std::string_view name, s32 req_id)
{
    {
        Np2State& s = np2();
        std::lock_guard lock(s.mutex);
        if (!s.oauth_initialized) return error(SCE_NP_OAUTH_ERROR_NOT_INITIALIZED);
    }
    PS_LOG_WARN(kChan, "{}(request {}): a TODO in RPCS3, answered CELL_OK as there; offline nothing is requested", name,
        req_id);
    return lv2::CELL_OK;
}

s32 sceNpAuthCreateOAuthRequest(PPUContext&) { return oauth_todo("sceNpAuthCreateOAuthRequest", 0); }
s32 sceNpAuthDeleteOAuthRequest(PPUContext&, s32 req_id) { return oauth_todo("sceNpAuthDeleteOAuthRequest", req_id); }
s32 sceNpAuthAbortOAuthRequest(PPUContext&, s32 req_id) { return oauth_todo("sceNpAuthAbortOAuthRequest", req_id); }

s32 sceNpAuthGetAuthorizationCode(PPUContext&, s32 req_id, u32 param, u32 auth_code, u32 issuer_id)
{
    PS_LOG_DEBUG(kChan, "sceNpAuthGetAuthorizationCode: {:#x} {:#x} {:#x}", param, auth_code, issuer_id);
    return oauth_todo("sceNpAuthGetAuthorizationCode", req_id);
}

// RPCS3 sceNpUtilBuildCdnUrl: a URL and no option, then a TODO (CELL_OK, nothing written).
s32 sceNpUtilBuildCdnUrl(PPUContext&, u32 url, u32 buf, u32 buf_size, u32 required, u32 option)
{
    if (!url || option) return error(SCE_NP_UTIL_ERROR_INVALID_ARGUMENT);
    PS_LOG_WARN(kChan, "sceNpUtilBuildCdnUrl({:#x}, buf {:#x} ({} bytes), {:#x}): a TODO in RPCS3; nothing written", url,
        buf, buf_size, required);
    return lv2::CELL_OK;
}

} // namespace

void np2_on_np_term()
{
    Np2State& s = np2();
    std::lock_guard lock(s.mutex);
    drop_room_callbacks(s);
}

bool np_match2_initialized() { return match2_initialized(); }

void register_sceNp2(Registry& r)
{
    r.add("sceNp2", {
        PS_HLE_FUNC(0x41251F74, sceNp2Init),
        PS_HLE_FUNC(0xAADB7C12, sceNp2Term),
        PS_HLE_FUNC(0xF4BABD3F, sceNpMatching2Init2),
        PS_HLE_FUNC(0x23009D07, sceNpMatching2Term2),
        PS_HLE_FUNC(0x2FE0DA7D, sceNpMatching2Term),
        PS_HLE_FUNC(0x9CBCE3F2, sceNpMatching2CreateContext),
        PS_HLE_FUNC(0x01379FD7, sceNpMatching2DestroyContext),
        PS_HLE_FUNC(0x6BA4C668, sceNpMatching2ContextStartAsync),
        PS_HLE_FUNC(0xF43C647A, sceNpMatching2ContextStop),
        PS_HLE_FUNC(0x748029A2, sceNpMatching2RegisterContextCallback),
        PS_HLE_FUNC(0xA9E6103E, sceNpMatching2RegisterSignalingCallback),
        PS_HLE_FUNC(0xDCB6B27D, sceNpMatching2RegisterRoomEventCallback),
        PS_HLE_FUNC(0xFEE11E72, sceNpMatching2RegisterRoomMessageCallback),
        PS_HLE_FUNC(0x0884FC95, sceNpMatching2RegisterLobbyEventCallback),
        PS_HLE_FUNC(0x03A2F42A, sceNpMatching2RegisterLobbyMessageCallback),
        PS_HLE_FUNC(0x05BF2FBD, sceNpMatching2GetWorldInfoList),
        PS_HLE_FUNC(0x3457C0DB, sceNpMatching2GetServerInfo),
        PS_HLE_FUNC(0x8E5CFE9F, sceNpMatching2GetServerIdListLocal),
        PS_HLE_FUNC(0x0D22867F, sceNpMatching2SearchRoom),
        PS_HLE_FUNC(0xE7A3BC7A, sceNpMatching2CreateJoinRoom),
        PS_HLE_FUNC(0x5721E711, sceNpMatching2JoinRoom),
        PS_HLE_FUNC(0x1F372697, sceNpMatching2LeaveRoom),
        PS_HLE_FUNC(0x215B0D75, sceNpMatching2SetRoomDataExternal),
        PS_HLE_FUNC(0x817D1090, sceNpMatching2SetRoomDataInternal),
        PS_HLE_FUNC(0x2C5B7FC9, sceNpMatching2GetRoomMemberDataExternalList),
        PS_HLE_FUNC(0x2ED909DC, sceNpMatching2AbortRequest),
        PS_HLE_FUNC(0x3C00C9D4, sceNpMatching2GetEventData),
        PS_HLE_FUNC(0xAA6C2C43, sceNpMatching2ClearEventData),
        PS_HLE_FUNC(0x16B3E5A4, sceNpMatching2CreateServerContext),
        PS_HLE_FUNC(0xC6B63B7E, sceNpMatching2DeleteServerContext),
        PS_HLE_FUNC(0xEFBD9357, sceNpMatching2GetLobbyInfoList),
        PS_HLE_FUNC(0x129663D0, sceNpMatching2GetLobbyMemberDataInternal),
        PS_HLE_FUNC(0x0B2218A3, sceNpMatching2GetLobbyMemberDataInternalList),
        PS_HLE_FUNC(0xE4AC32CA, sceNpMatching2GetRoomDataExternalList),
        PS_HLE_FUNC(0x8B209CA2, sceNpMatching2GetRoomDataInternal),
        PS_HLE_FUNC(0xB0C51412, sceNpMatching2GetRoomMemberDataInternal),
        PS_HLE_FUNC(0xAC98B03A, sceNpMatching2GetUserInfoList),
        PS_HLE_FUNC(0x9344D41F, sceNpMatching2GrantRoomOwner),
        PS_HLE_FUNC(0x29E78389, sceNpMatching2JoinLobby),
        PS_HLE_FUNC(0xBA578D19, sceNpMatching2JoinProhibitiveRoom),
        PS_HLE_FUNC(0x66F19527, sceNpMatching2KickoutRoomMember),
        PS_HLE_FUNC(0x02065E3D, sceNpMatching2LeaveLobby),
        PS_HLE_FUNC(0x3F3BD413, sceNpMatching2SendLobbyChatMessage),
        PS_HLE_FUNC(0xEFFF8779, sceNpMatching2SendLobbyInvitation),
        PS_HLE_FUNC(0x7BF6E152, sceNpMatching2SendRoomChatMessage),
        PS_HLE_FUNC(0x28CAD8B3, sceNpMatching2SendRoomMessage),
        PS_HLE_FUNC(0xFA0583C6, sceNpMatching2SetLobbyMemberDataInternal),
        PS_HLE_FUNC(0xB851AACF, sceNpMatching2SetRoomMemberDataInternal),
        PS_HLE_FUNC(0x6FCD84C1, sceNpMatching2SetSignalingOptParam),
        PS_HLE_FUNC(0x1022E6D6, sceNpMatching2SetUserInfo),
        PS_HLE_FUNC(0x8D3F8D49, sceNpMatching2SignalingGetPingInfo),
        PS_HLE_FUNC(0x3F62C759, sceNpMatching2Init),
        PS_HLE_FUNC(0x12D0B0F9, sceNpMatching2ContextStart),
        PS_HLE_FUNC(0x430CE063, sceNpMatching2AbortContextStart),
        PS_HLE_FUNC(0xDB87B088, sceNpMatching2SetDefaultRequestOptParam),
        PS_HLE_FUNC(0x62D446AC, sceNpMatching2GetCbQueueInfo),
        PS_HLE_FUNC(0x196A06F9, sceNpMatching2GetMemoryInfo),
        PS_HLE_FUNC(0x12034412, sceNpMatching2GetClanLobbyId),
        PS_HLE_FUNC(0xEFFE5A16, sceNpMatching2GetLobbyMemberIdListLocal),
        PS_HLE_FUNC(0x490124FD, sceNpMatching2GetRoomMemberIdListLocal),
        PS_HLE_FUNC(0xDE6D308B, sceNpMatching2GetRoomPasswordLocal),
        PS_HLE_FUNC(0x3CC27344, sceNpMatching2GetRoomSlotInfoLocal),
        PS_HLE_FUNC(0xA2B01449, sceNpMatching2GetSignalingOptParamLocal),
        PS_HLE_FUNC(0x58A04247, sceNpMatching2GetRoomMemberDataInternalLocal),
        PS_HLE_FUNC(0x0EBE4C6B, sceNpMatching2SignalingGetConnectionStatus),
        PS_HLE_FUNC(0x26AE9FF8, sceNpMatching2SignalingGetConnectionInfo),
        PS_HLE_FUNC(0x98F74C9A, sceNpMatching2SignalingGetLocalNetInfo),
        PS_HLE_FUNC(0xDBE84071, sceNpMatching2SignalingGetPeerNetInfo),
        PS_HLE_FUNC(0x2E42F94A, sceNpMatching2SignalingGetPeerNetInfoResult),
        PS_HLE_FUNC(0x944B28E8, sceNpMatching2SignalingCancelPeerNetInfo),
        PS_HLE_FUNC(0xE9946648, sceNpMatching2SignalingGetCtxOpt),
        PS_HLE_FUNC(0xBF014813, sceNpMatching2SignalingSetCtxOpt),
        PS_HLE_FUNC(0xF30AC754, sceNpAuthOAuthInit),
        PS_HLE_FUNC(0xC0AB5CBB, sceNpAuthOAuthTerm),
        PS_HLE_FUNC(0xD7AA552E, sceNpAuthCreateOAuthRequest),
        PS_HLE_FUNC(0x2880D8D5, sceNpAuthDeleteOAuthRequest),
        PS_HLE_FUNC(0x9DB622E8, sceNpAuthAbortOAuthRequest),
        PS_HLE_FUNC(0xE8BB13FF, sceNpAuthGetAuthorizationCode),
        PS_HLE_FUNC(0x8E930999, sceNpUtilBuildCdnUrl),
    });
}

} // namespace pspocket::hle
