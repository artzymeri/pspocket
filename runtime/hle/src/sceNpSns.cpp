// sceNpSns: the PS3's Facebook link (access tokens for the game's Facebook app). Online only, a non-goal (D-024,
// docs/phase0-gta5.md). Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNpSns.cpp/.h (sce_np_sns_manager, sns_fb_handle_t)
// and its ID manager (rpcs3/Emu/IdManager.h/.cpp).
//
// The library starts without NP; the pool it is given is not used (a TODO in RPCS3). Handles get IDs as RPCS3's idm
// hands them out for id_base 1, id_step 1, id_count 5 (SCE_NP_SNS_FB_HANDLE_SLOT_MAX + 1): a fifth handle can be
// made, but, as in RPCS3, only IDs 1 to 4 can be destroyed, aborted or used. sceNpSnsFbTerm leaves the handles.
// An access token needs a user signed in to PSN: none ever is here (sceNp.cpp), so the request is refused with
// SCE_NP_SNS_ERROR_NOT_SIGN_IN and nothing written, RPCS3's answer while PSN is offline. Aborting a handle does
// nothing: nothing ever runs on one. What RPCS3 implements is logged at info; its TODOs at warn (RPCS3's todo level).
//
// Difference from RPCS3: pointers are checked before use (D-042): one that is not guest memory is logged with the call
// site and refused with SCE_NP_SNS_ERROR_INVALID_ARGUMENT before anything changes (RPCS3 faults).

#include <format>
#include <mutex>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "idm.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNpSns";

enum : u32 {
    SCE_NP_SNS_ERROR_NOT_SIGN_IN = 0x80024502,
    SCE_NP_SNS_ERROR_INVALID_ARGUMENT = 0x80024503,
    SCE_NP_SNS_FB_ERROR_ALREADY_INITIALIZED = 0x80024511,
    SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED = 0x80024512,
    SCE_NP_SNS_FB_ERROR_EXCEEDS_MAX = 0x80024513,
    SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE = 0x80024514,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case SCE_NP_SNS_ERROR_NOT_SIGN_IN: return "SCE_NP_SNS_ERROR_NOT_SIGN_IN";
    case SCE_NP_SNS_ERROR_INVALID_ARGUMENT: return "SCE_NP_SNS_ERROR_INVALID_ARGUMENT";
    case SCE_NP_SNS_FB_ERROR_ALREADY_INITIALIZED: return "SCE_NP_SNS_FB_ERROR_ALREADY_INITIALIZED";
    case SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED: return "SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED";
    case SCE_NP_SNS_FB_ERROR_EXCEEDS_MAX: return "SCE_NP_SNS_FB_ERROR_EXCEEDS_MAX";
    case SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE: return "SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

constexpr u32 SCE_NP_SNS_FB_INVALID_HANDLE = 0, SCE_NP_SNS_FB_HANDLE_SLOT_MAX = 4;
// SceNpSnsFbInitParams (8 bytes): pool, poolSize. SceNpSnsFbAccessTokenParam: be64 fb_app_id first.
constexpr u32 kInitParamsSize = 8, kAppIdSize = 8;

struct FbHandle {}; // RPCS3 sns_fb_handle_t holds nothing

struct SnsState {
    std::mutex mutex;
    bool initialized = false; // RPCS3 sce_np_sns_manager::is_initialized
    // idm sns_fb_handle_t: id_base 1, id_step 1, id_count SCE_NP_SNS_FB_HANDLE_SLOT_MAX + 1.
    IdTable<FbHandle, SCE_NP_SNS_FB_INVALID_HANDLE + 1, SCE_NP_SNS_FB_HANDLE_SLOT_MAX + 1> handles;
};

SnsState& sns()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpSns called without an HLE process");
    return p->state<SnsState>();
}

// The handle check of RPCS3's DestroyHandle, AbortHandle and GetLongAccessToken: 1 to SCE_NP_SNS_FB_HANDLE_SLOT_MAX.
bool in_range(u32 handle) { return handle != SCE_NP_SNS_FB_INVALID_HANDLE && handle <= SCE_NP_SNS_FB_HANDLE_SLOT_MAX; }

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// RPCS3 sceNpSnsFbInit(const SceNpSnsFbInitParams* params): needs neither NP nor a network.
s32 sceNpSnsFbInit(PPUContext& ctx, u32 params)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        PS_LOG_WARN(kChan, "sceNpSnsFbInit(params {:#x}): {}{}", params, result_name(r), what);
        return r;
    };
    SnsState& s = sns();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return done(error(SCE_NP_SNS_FB_ERROR_ALREADY_INITIALIZED));
    if (!params) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    s.initialized = true;
    std::string pool = "unreadable";
    if (guest_readable(thread_of(ctx), params, kInitParamsSize)) {
        u8* base = guest_base(ctx);
        pool = std::format("{:#x}, {:#x} bytes", ps_read32(base, params), ps_read32(base, params + 4));
    }
    return done(lv2::CELL_OK, std::format("; RPCS3 todo: the pool ({}) is not used", pool));
}

// RPCS3 sceNpSnsFbTerm: the handles stay (idm keeps them).
s32 sceNpSnsFbTerm(PPUContext&)
{
    SnsState& s = sns();
    std::lock_guard lock(s.mutex);
    const s32 r = s.initialized ? lv2::CELL_OK : error(SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    PS_LOG_INFO(kChan, "sceNpSnsFbTerm(): {}", result_name(r));
    return r;
}

// RPCS3 sceNpSnsFbCreateHandle(u32* handle): the new ID is written before it is checked, so with all five in use 0 is
// written and SCE_NP_SNS_FB_ERROR_EXCEEDS_MAX returned.
s32 sceNpSnsFbCreateHandle(PPUContext& ctx, u32 handle)
{
    const auto done = [&](s32 r, u32 made = SCE_NP_SNS_FB_INVALID_HANDLE) {
        PS_LOG_INFO(kChan, "sceNpSnsFbCreateHandle({:#x}): {}{}", handle, result_name(r),
            made != SCE_NP_SNS_FB_INVALID_HANDLE ? std::format(", handle {}", made) : std::string());
        return r;
    };
    SnsState& s = sns();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED));
    if (!handle) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    if (!usable(ctx, handle, 4, "sceNpSnsFbCreateHandle: handle")) {
        return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    }
    const u32 id = s.handles.make({});
    ps_write32(guest_base(ctx), handle, id);
    if (id == SCE_NP_SNS_FB_INVALID_HANDLE) return done(error(SCE_NP_SNS_FB_ERROR_EXCEEDS_MAX));
    return done(lv2::CELL_OK, id);
}

s32 sceNpSnsFbDestroyHandle(PPUContext&, u32 handle)
{
    const auto done = [&](s32 r) {
        PS_LOG_INFO(kChan, "sceNpSnsFbDestroyHandle({}): {}", handle, result_name(r));
        return r;
    };
    SnsState& s = sns();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED));
    if (!in_range(handle)) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    if (!s.handles.remove(handle)) return done(error(SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpSnsFbAbortHandle: a TODO after the checks. Nothing ever runs on a handle here (token requests are refused
// at once), so there is nothing to abort.
s32 sceNpSnsFbAbortHandle(PPUContext&, u32 handle)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        PS_LOG_WARN(kChan, "sceNpSnsFbAbortHandle({}): {}{}", handle, result_name(r), what);
        return r;
    };
    SnsState& s = sns();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED));
    if (!in_range(handle)) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    if (!s.handles.find(handle)) return done(error(SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE));
    return done(lv2::CELL_OK, "; RPCS3 todo: nothing runs on the handle to abort");
}

// RPCS3 sceNpSnsFbGetLongAccessToken(handle, const SceNpSnsFbAccessTokenParam* param,
// SceNpSnsFbLongAccessTokenResult* result): the arguments first (before the library, as RPCS3), then the handle, then
// PSN: np_handler::get_psn_status() is SCE_NP_MANAGER_STATUS_OFFLINE, so SCE_NP_SNS_ERROR_NOT_SIGN_IN and nothing is
// written. (RPCS3 returns it as not_an_error: it isn't logged as an error, but the game gets the same code.)
s32 sceNpSnsFbGetLongAccessToken(PPUContext& ctx, u32 handle, u32 param, u32 result)
{
    std::string app; // the param's fb_app_id once read, for the log
    const auto done = [&](s32 r) {
        PS_LOG_WARN(kChan, "sceNpSnsFbGetLongAccessToken(handle {}, param {:#x}{}, result {:#x}): {}", handle, param,
            app, result, result_name(r));
        return r;
    };
    if (!param || !result) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    if (!usable(ctx, param, kAppIdSize, "sceNpSnsFbGetLongAccessToken: param")) {
        return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    }
    const u64 app_id = ps_read64(guest_base(ctx), param);
    app = std::format(" (fb_app_id {})", app_id);
    if (!app_id) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    SnsState& s = sns();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED));
    if (!in_range(handle)) return done(error(SCE_NP_SNS_ERROR_INVALID_ARGUMENT));
    if (!s.handles.find(handle)) return done(error(SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE));
    return done(error(SCE_NP_SNS_ERROR_NOT_SIGN_IN));
}

} // namespace

void register_sceNpSns(Registry& r)
{
    r.add("sceNpSns", {
        PS_HLE_FUNC(0x2C0F3548, sceNpSnsFbInit),
        PS_HLE_FUNC(0xC05545FD, sceNpSnsFbTerm),
        PS_HLE_FUNC(0x8FD1D549, sceNpSnsFbCreateHandle),
        PS_HLE_FUNC(0x6E42C0DD, sceNpSnsFbDestroyHandle),
        PS_HLE_FUNC(0x3711833F, sceNpSnsFbAbortHandle),
        PS_HLE_FUNC(0xBA8C569E, sceNpSnsFbGetLongAccessToken),
    });
}

} // namespace pspocket::hle
