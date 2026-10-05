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
    SCE_NP_MATCHING2_ERROR_NOT_NP_SIGN_IN = 0x80022334,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// RPCS3 np_handler's NP 2 state.
struct Np2State {
    std::mutex mutex;
    bool np2_initialized = false;    // sceNp2Init's (is_NP2_init)
    bool match2_initialized = false; // sceNpMatching2Init2's (is_NP2_Match2_init)
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
    if (np_term() == lv2::CELL_OK) {
        s.room_event_cb = s.room_event_cb_arg = 0;
        s.room_event_cb_ctx = 0;
        s.room_msg_cb = s.room_msg_cb_arg = 0;
        s.room_msg_cb_ctx = 0;
    }
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

} // namespace

void register_sceNp2(Registry& r)
{
    r.add("sceNp2", {
        PS_HLE_FUNC(0x41251F74, sceNp2Init),
        PS_HLE_FUNC(0xAADB7C12, sceNp2Term),
        PS_HLE_FUNC(0xF4BABD3F, sceNpMatching2Init2),
        PS_HLE_FUNC(0x23009D07, sceNpMatching2Term2),
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
    });
}

} // namespace pspocket::hle
