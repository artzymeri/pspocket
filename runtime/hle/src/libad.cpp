// libad_core and libad_async (sceAd*): Sony's in-game advertising, its contexts connected to the ad server on PSN, the
// ad spaces a game shows and their assets, and the reports of what was shown, pspocket's own as a console with no PSN
// that has never been online (D-637). Both libraries firmware-first (D-563): no Sony module was at hand to compare
// with, so where the system software is installed a game gets Sony's libad_core and libad_async whole.
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/libad_core.cpp and libad_async.cpp register the thirteen names only (each a
// todo answering CELL_OK, no arguments), and nothing public describes the libraries. libad_core signs with the
// console's own key (lv2's sys_ss_ad_sign, runtime/lv2/src/misc.cpp, from the firmware's modules) and depends on libfs,
// libnet and NP; libad_async on libad_core (RPCS3 cellSysmodule.cpp DEPENDENCIES_AD_CORE, DEPENDENCIES_AD_ASYNC). The
// model, each choice recorded in D-637 with what to check on the Mac:
// - A context is the library's own bookkeeping: OpenContext and AsyncOpenContext make one (at most 32, as NP's title
//   contexts) and answer its ID, 1 to 32, as NP's title context creators answer theirs (sceNpLookupCreateTitleCtx);
//   their arguments are not read and nothing is written. libad_async is a layer over libad_core, so the two share
//   their contexts: CloseContext and AsyncCloseContext forget one, whichever opened it.
// - Connecting needs the ad server on PSN, which this console never reaches: ConnectContext and AsyncConnectContext
//   answer SCE_NP_ERROR_OFFLINE (the asynchronous one at once: there is nothing to wait for), and so does everything
//   a connection gives: the connection's information, an ad space's and an asset's (a console that has never been
//   online has no ads kept either), opening an ad space, flushing the reports. Each after the context ID is checked,
//   and for the connection's information the pointer it would fill (the place of the others' is not known); nothing
//   is written. No space is ever open, so closing one finds none.
// - Error codes: the libraries' own are not known; these are NP's (rpcs3/Emu/Cell/Modules/sceNp.h), the closest
//   documented ones: ID_NOT_FOUND for a context or space not open, ID_NO_SPACE past 32 contexts, INVALID_ARGUMENT for
//   a null pointer or one that is not guest memory.
// Each process its own libraries.

#include <array>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kCore = "libad_core", kAsync = "libad_async";
constexpr const char* kChan = "libad";

// NP's codes (RPCS3 sceNp.h), the closest documented ones (D-637).
enum : u32 {
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x8002aa03,
    SCE_NP_ERROR_ID_NO_SPACE = 0x8002aa05,
    SCE_NP_ERROR_ID_NOT_FOUND = 0x8002aa06,
    SCE_NP_ERROR_OFFLINE = 0x8002aa0c,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kMaxContexts = 32; // as NP's title contexts (RPCS3 SCE_NP_LOOKUP_MAX_CTX_NUM)

// The contexts of both libraries (libad_async's are libad_core's).
struct State {
    std::mutex mutex;
    std::array<bool, kMaxContexts> open{}; // context ID - 1
    std::set<std::string_view> logged;     // the calls whose refusal was logged once
};

State& state() { return HleProcess::current()->state<State>(); }

bool is_open(const State& s, u32 id) { return id >= 1 && id <= kMaxContexts && s.open[id - 1]; }

// The checks a context call makes: the context, then the pointer it would fill (when it takes one). CELL_OK when they
// pass.
s32 check(PPUContext& ctx, const State& s, std::string_view call, u32 id, std::optional<u32> pointer)
{
    if (!is_open(s, id)) return error(SCE_NP_ERROR_ID_NOT_FOUND);
    if (pointer && !(*pointer && guest_readable(thread_of(ctx), *pointer, 1))) {
        log_call_site(ctx, guest_base(ctx), std::format("{}: {:#x} is null or not guest memory", call, *pointer));
        return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    }
    return lv2::CELL_OK;
}

// The ad server on PSN: never reached here.
s32 offline(PPUContext& ctx, std::string_view call, u32 id, std::optional<u32> pointer = std::nullopt)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check(ctx, s, call, id, pointer); r != lv2::CELL_OK) return r;
    if (s.logged.insert(call).second)
        PS_LOG_INFO(kChan, "{}(context {}): the ad server is on PSN, which this console never reaches -> OFFLINE", call,
            id);
    return error(SCE_NP_ERROR_OFFLINE);
}

s32 open_context(std::string_view call)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (u32 i = 0; i < kMaxContexts; i++) {
        if (s.open[i]) continue;
        s.open[i] = true;
        PS_LOG_INFO(kChan, "{}: context {}; it never connects offline", call, i + 1);
        return static_cast<s32>(i + 1);
    }
    PS_LOG_WARN(kChan, "{}: all {} contexts are open -> ID_NO_SPACE", call, kMaxContexts);
    return error(SCE_NP_ERROR_ID_NO_SPACE);
}

s32 close_context(u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!is_open(s, id)) return error(SCE_NP_ERROR_ID_NOT_FOUND);
    s.open[id - 1] = false;
    return lv2::CELL_OK;
}

// ---- libad_core

// int sceAdOpenContext(...): the new context's ID. Its arguments are not read (D-637).
s32 sceAdOpenContext(PPUContext&) { return open_context("sceAdOpenContext"); }

// int sceAdCloseContext(id)
s32 sceAdCloseContext(PPUContext&, u32 id) { return close_context(id); }

// int sceAdConnectContext(id, ...)
s32 sceAdConnectContext(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdConnectContext", id); }

// int sceAdGetConnectionInfo(id, info *)
s32 sceAdGetConnectionInfo(PPUContext& ctx, u32 id, u32 info)
{
    return offline(ctx, "sceAdGetConnectionInfo", id, info);
}

// int sceAdGetSpaceInfo(id, ...): an ad space's, which only a connection gives.
s32 sceAdGetSpaceInfo(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdGetSpaceInfo", id); }

// int sceAdGetAssetInfo(id, ...): an asset's (an ad to show), which only a connection gives.
s32 sceAdGetAssetInfo(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdGetAssetInfo", id); }

// int sceAdFlushReports(id): the reports of what was shown, sent to the ad server.
s32 sceAdFlushReports(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdFlushReports", id); }

// ---- libad_async: the same, each answering at once.

s32 sceAdAsyncOpenContext(PPUContext&) { return open_context("sceAdAsyncOpenContext"); }

s32 sceAdAsyncCloseContext(PPUContext&, u32 id) { return close_context(id); }

s32 sceAdAsyncConnectContext(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdAsyncConnectContext", id); }

// int sceAdAsyncSpaceOpen(id, ...): an ad space, opened on a connection.
s32 sceAdAsyncSpaceOpen(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdAsyncSpaceOpen", id); }

// int sceAdAsyncSpaceClose(id, space): none is ever open.
s32 sceAdAsyncSpaceClose(PPUContext&, u32 id, u32 space)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!is_open(s, id)) return error(SCE_NP_ERROR_ID_NOT_FOUND);
    PS_LOG_DEBUG(kChan, "sceAdAsyncSpaceClose(context {}, space {:#x}): no space is open -> ID_NOT_FOUND", id, space);
    return error(SCE_NP_ERROR_ID_NOT_FOUND);
}

s32 sceAdAsyncFlushReports(PPUContext& ctx, u32 id) { return offline(ctx, "sceAdAsyncFlushReports", id); }

} // namespace

void register_libad(Registry& r)
{
    r.add(kCore, {
        PS_HLE_FUNC(0x0D884B17, sceAdOpenContext),
        PS_HLE_FUNC(0xD1ADA7AE, sceAdCloseContext),
        PS_HLE_FUNC(0xF930CC21, sceAdConnectContext),
        PS_HLE_FUNC(0xE73C90BA, sceAdGetConnectionInfo),
        PS_HLE_FUNC(0xDFA9D08D, sceAdGetSpaceInfo),
        PS_HLE_FUNC(0xAC495EB8, sceAdGetAssetInfo),
        PS_HLE_FUNC(0x1B6F5F4D, sceAdFlushReports),
    });
    r.add(kAsync, {
        PS_HLE_FUNC(0x44946174, sceAdAsyncOpenContext),
        PS_HLE_FUNC(0xCF1CBCF5, sceAdAsyncCloseContext),
        PS_HLE_FUNC(0x7A7A1ED3, sceAdAsyncConnectContext),
        PS_HLE_FUNC(0x833F4516, sceAdAsyncSpaceOpen),
        PS_HLE_FUNC(0xC2DB09C5, sceAdAsyncSpaceClose),
        PS_HLE_FUNC(0x9FD61D8E, sceAdAsyncFlushReports),
    });
    r.firmware_first(kCore);
    r.firmware_first(kAsync);
}

} // namespace pspocket::hle
