// sceNp: PlayStation Network. A non-goal (docs/phase0-gta5.md, D-024): every function reports "not initialized" or
// "offline" the way RPCS3 does when the user is not signed in, so single-player never waits on PSN.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNp.cpp/.h.
//
// sceNpInit is not imported by GTA V; sceNp2Init (module sceNp2, registered here because it shares this state)
// initializes NP there. Until then the manager functions return SCE_NP_ERROR_NOT_INITIALIZED, as RPCS3 does before
// init; afterwards they report a user who is signed out (offline), and the callbacks and handlers registered are kept
// but never called.

#include <atomic>
#include <mutex>

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
    SCE_NP_ERROR_INSUFFICIENT_BUFFER = 0x8002AA0F,
    SCE_NP_BASIC_ERROR_NOT_INITIALIZED = 0x8002A662,
    SCE_NP_BASIC_ERROR_INVALID_ARGUMENT = 0x8002A665,
    SCE_NP_BASIC_ERROR_EXCEEDS_MAX = 0x8002A66B,
    SCE_NP_BASIC_ERROR_NOT_REGISTERED = 0x8002A66D,
};
constexpr s32 SCE_NP_MANAGER_STATUS_OFFLINE = -1;
constexpr u32 SCE_NP_MIN_POOL_SIZE = 128 * 1024;

struct NpState {
    std::mutex mutex;                     // init, term and the manager callback
    std::atomic<bool> initialized{false}; // sceNpInit's state
    bool np2_initialized = false;         // sceNp2Init's, guarded by mutex
    u32 manager_cb = 0, manager_cb_arg = 0; // guarded by mutex
    bool basic_handler_registered = false;   // guarded by mutex
    u32 basic_handler = 0, basic_handler_arg = 0;
};

NpState& np()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNp called without an HLE process");
    return p->state<NpState>();
}

s32 sceNpManagerGetStatus(PPUContext& ctx, u32 status)
{
    if (!np().initialized) {
        PS_LOG_DEBUG(kChan, "sceNpManagerGetStatus: NP not initialized");
        return static_cast<s32>(SCE_NP_ERROR_NOT_INITIALIZED);
    }
    if (!status) return static_cast<s32>(SCE_NP_ERROR_INVALID_ARGUMENT);
    ps_write32(thread_of(ctx).kernel.base(), status, static_cast<u32>(SCE_NP_MANAGER_STATUS_OFFLINE));
    return lv2::CELL_OK;
}

// RPCS3 sceNpManagerRegisterCallback / sceNpManagerUnregisterCallback: the callback is kept for sign-in status
// changes; RPCS3 calls it from its NP handler when the user signs in or out, which never happens offline.
s32 sceNpManagerRegisterCallback(PPUContext&, u32 callback, u32 arg)
{
    NpState& s = np();
    if (!s.initialized) return static_cast<s32>(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!callback) return static_cast<s32>(SCE_NP_ERROR_INVALID_ARGUMENT);
    std::lock_guard lock(s.mutex);
    s.manager_cb = callback;
    s.manager_cb_arg = arg;
    PS_LOG_INFO(kChan, "sceNpManagerRegisterCallback({:#x}, arg {:#x}): kept; offline, it is never called", callback, arg);
    return lv2::CELL_OK;
}

s32 sceNpManagerUnregisterCallback(PPUContext&)
{
    NpState& s = np();
    if (!s.initialized) return static_cast<s32>(SCE_NP_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    s.manager_cb = 0; // RPCS3 leaves the argument
    return lv2::CELL_OK;
}

// RPCS3 sceNpBasicRegisterContextSensitiveHandler / sceNpBasicUnregisterHandler: one basic event handler, kept for
// presence and messages from other users; offline none ever come, so it is never called.
s32 sceNpBasicRegisterContextSensitiveHandler(PPUContext&, u32 context, u32 handler, u32 arg)
{
    NpState& s = np();
    if (!s.initialized) return static_cast<s32>(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (s.basic_handler_registered) return static_cast<s32>(SCE_NP_BASIC_ERROR_EXCEEDS_MAX);
    if (!context || !handler) return static_cast<s32>(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
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
    if (!s.initialized) return static_cast<s32>(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    std::lock_guard lock(s.mutex);
    if (!s.basic_handler_registered) return static_cast<s32>(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
    s.basic_handler_registered = false;
    return lv2::CELL_OK;
}

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
    if (s.np2_initialized) return static_cast<s32>(SCE_NP_ERROR_ALREADY_INITIALIZED);
    if (const u32 r = np_init_locked(s, poolsize, poolptr); r != lv2::CELL_OK && r != SCE_NP_ERROR_ALREADY_INITIALIZED) {
        return static_cast<s32>(r);
    }
    s.np2_initialized = true;
    PS_LOG_INFO(kChan, "sceNp2Init(pool {:#x} bytes at {:#x}): NP up, no user signed in (offline)", poolsize, poolptr);
    return lv2::CELL_OK;
}

// RPCS3 sceNp2Term: matching 2 (never started here) and NP are terminated.
s32 sceNp2Term(PPUContext&)
{
    NpState& s = np();
    std::lock_guard lock(s.mutex);
    if (!s.np2_initialized) return static_cast<s32>(SCE_NP_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    s.np2_initialized = false;
    s.basic_handler_registered = false; // RPCS3 terminate_NP drops the handlers
    s.manager_cb = s.manager_cb_arg = 0;
    return lv2::CELL_OK;
}

} // namespace

void register_sceNp(Registry& r)
{
    r.add("sceNp", {
        PS_HLE_FUNC(0xA7BFF757, sceNpManagerGetStatus),
        PS_HLE_FUNC(0xE7DCD3B4, sceNpManagerRegisterCallback),
        PS_HLE_FUNC(0x52A6B523, sceNpManagerUnregisterCallback),
        PS_HLE_FUNC(0x4026EAC5, sceNpBasicRegisterContextSensitiveHandler),
        PS_HLE_FUNC(0xACB9EE8E, sceNpBasicUnregisterHandler),
    });
    r.add("sceNp2", {
        PS_HLE_FUNC(0x41251F74, sceNp2Init),
        PS_HLE_FUNC(0xAADB7C12, sceNp2Term),
    });
}

} // namespace pspocket::hle
