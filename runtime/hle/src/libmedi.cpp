// libmedi (cellMediator): Sony's mediator service, through which a game's reports go to a PSN provider, signed by the
// console, pspocket's own as a console with no PSN (D-636). Firmware-first (D-563): no Sony module was at hand to
// compare with, so where the system software is installed a game gets Sony's libmedi whole.
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/libmedi.cpp registers the ten names only (each a todo answering CELL_OK, no
// arguments), and nothing public describes the library. libmedi signs with the console's own key: it makes lv2's
// sys_ss_ad_sign (runtime/lv2/src/misc.cpp, from the firmware's modules), and depends on libnet and NP (RPCS3
// cellSysmodule.cpp DEPENDENCIES_MEDI). The model, each choice recorded in D-636 with what to check on the Mac:
// - Contexts are the library's own bookkeeping: CreateContext makes one (at most 32, as NP's title contexts) and
//   answers its ID, 1 to 32, as NP's title context creators answer theirs (sceNpLookupCreateTitleCtx); its arguments
//   (what the mediator would be told) are not read, and nothing is written. CloseContext forgets one.
// - Everything a context does needs the mediator service on PSN, which this console never reaches: its status, the
//   provider's URL, the user's information, posting reports (reliably or not) and flushing the cache of reports kept
//   for later answer SCE_NP_ERROR_OFFLINE, after the context ID and the pointer the call would fill or read are
//   checked; nothing is written.
// - The signature is the console's (sys_ss_ad_sign, made by the PS3's security service with keys only a PS3 holds):
//   Sign and GetSignatureLength answer CELL_ENOTSUP, the system's "not supported", after the same checks. pspocket's
//   lv2 answers sys_ss_ad_sign itself with CELL_OK and no signature (for Sony's modules, D-527); this library does not
//   call it, so it never hands out a signature it did not make.
// - Error codes: the library's own are not known; these are NP's (rpcs3/Emu/Cell/Modules/sceNp.h), the closest
//   documented ones: ID_NOT_FOUND for a context ID not open, ID_NO_SPACE past 32, INVALID_ARGUMENT for a null pointer
//   or one that is not guest memory.
// Each process its own library.

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

constexpr std::string_view kModule = "libmedi";
constexpr const char* kChan = "libmedi";

// NP's codes (RPCS3 sceNp.h), the closest documented ones (D-636).
enum : u32 {
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x8002aa03,
    SCE_NP_ERROR_ID_NO_SPACE = 0x8002aa05,
    SCE_NP_ERROR_ID_NOT_FOUND = 0x8002aa06,
    SCE_NP_ERROR_OFFLINE = 0x8002aa0c,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kMaxContexts = 32; // as NP's title contexts (RPCS3 SCE_NP_LOOKUP_MAX_CTX_NUM)

struct State {
    std::mutex mutex;
    std::array<bool, kMaxContexts> open{}; // context ID - 1
    std::set<std::string_view> logged;     // the calls whose refusal was logged once
};

State& state() { return HleProcess::current()->state<State>(); }

bool is_open(const State& s, u32 id) { return id >= 1 && id <= kMaxContexts && s.open[id - 1]; }

// The checks a context call makes: the context, then the pointer it would fill or read (when it takes one). CELL_OK
// when they pass.
s32 check(PPUContext& ctx, State& s, std::string_view call, u32 id, std::optional<u32> pointer)
{
    if (!is_open(s, id)) return error(SCE_NP_ERROR_ID_NOT_FOUND);
    if (pointer && !(*pointer && guest_readable(thread_of(ctx), *pointer, 1))) {
        log_call_site(ctx, guest_base(ctx), std::format("{}: {:#x} is null or not guest memory", call, *pointer));
        return error(SCE_NP_ERROR_INVALID_ARGUMENT);
    }
    return lv2::CELL_OK;
}

s32 refuse(State& s, std::string_view call, u32 code, std::string_view why)
{
    if (s.logged.insert(call).second) PS_LOG_INFO(kChan, "{}: {}", call, why);
    return error(code);
}

// The mediator on PSN: never reached here.
s32 offline(PPUContext& ctx, std::string_view call, u32 id, std::optional<u32> pointer)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check(ctx, s, call, id, pointer); r != lv2::CELL_OK) return r;
    return refuse(s, call, SCE_NP_ERROR_OFFLINE, "the mediator is on PSN, which this console never reaches -> OFFLINE");
}

// The console's signature: not made here.
s32 unsigned_console(PPUContext& ctx, std::string_view call, u32 id, u32 pointer)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check(ctx, s, call, id, pointer); r != lv2::CELL_OK) return r;
    return refuse(s, call, lv2::CELL_ENOTSUP,
        "the signature is the console's own (sys_ss_ad_sign, a PS3's keys), not made here -> CELL_ENOTSUP");
}

// int cellMediatorCreateContext(...): the new context's ID. Its arguments are not read (D-636).
s32 cellMediatorCreateContext(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (u32 i = 0; i < kMaxContexts; i++) {
        if (s.open[i]) continue;
        s.open[i] = true;
        PS_LOG_INFO(kChan, "cellMediatorCreateContext: context {}; the mediator is never reached offline", i + 1);
        return static_cast<s32>(i + 1);
    }
    PS_LOG_WARN(kChan, "cellMediatorCreateContext: all {} contexts are open -> ID_NO_SPACE", kMaxContexts);
    return error(SCE_NP_ERROR_ID_NO_SPACE);
}

// int cellMediatorCloseContext(id)
s32 cellMediatorCloseContext(PPUContext&, u32 id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!is_open(s, id)) return error(SCE_NP_ERROR_ID_NOT_FOUND);
    s.open[id - 1] = false;
    return lv2::CELL_OK;
}

// int cellMediatorGetStatus(id, status *)
s32 cellMediatorGetStatus(PPUContext& ctx, u32 id, u32 status)
{
    return offline(ctx, "cellMediatorGetStatus", id, status);
}

// int cellMediatorGetProviderUrl(id, char *url, ...)
s32 cellMediatorGetProviderUrl(PPUContext& ctx, u32 id, u32 url)
{
    return offline(ctx, "cellMediatorGetProviderUrl", id, url);
}

// int cellMediatorGetUserInfo(id, info *)
s32 cellMediatorGetUserInfo(PPUContext& ctx, u32 id, u32 info)
{
    return offline(ctx, "cellMediatorGetUserInfo", id, info);
}

// int cellMediatorGetSignatureLength(id, length *)
s32 cellMediatorGetSignatureLength(PPUContext& ctx, u32 id, u32 length)
{
    return unsigned_console(ctx, "cellMediatorGetSignatureLength", id, length);
}

// int cellMediatorSign(id, const void *data, ...)
s32 cellMediatorSign(PPUContext& ctx, u32 id, u32 data)
{
    return unsigned_console(ctx, "cellMediatorSign", id, data);
}

// int cellMediatorPostReports(id, const reports *, ...)
s32 cellMediatorPostReports(PPUContext& ctx, u32 id, u32 reports)
{
    return offline(ctx, "cellMediatorPostReports", id, reports);
}

// int cellMediatorReliablePostReports(id, const reports *, ...): kept for later by a console that can post at all.
s32 cellMediatorReliablePostReports(PPUContext& ctx, u32 id, u32 reports)
{
    return offline(ctx, "cellMediatorReliablePostReports", id, reports);
}

// int cellMediatorFlushCache(id): the reports kept for later, sent.
s32 cellMediatorFlushCache(PPUContext& ctx, u32 id)
{
    return offline(ctx, "cellMediatorFlushCache", id, std::nullopt);
}

} // namespace

void register_libmedi(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x40AD67EB, cellMediatorCreateContext),
        PS_HLE_FUNC(0xC324F60C, cellMediatorCloseContext),
        PS_HLE_FUNC(0xF7E4A50A, cellMediatorGetStatus),
        PS_HLE_FUNC(0x9D11991D, cellMediatorGetProviderUrl),
        PS_HLE_FUNC(0x4D249136, cellMediatorGetUserInfo),
        PS_HLE_FUNC(0x4AECEA24, cellMediatorGetSignatureLength),
        PS_HLE_FUNC(0x6B8F5CB9, cellMediatorSign),
        PS_HLE_FUNC(0x85DC6981, cellMediatorPostReports),
        PS_HLE_FUNC(0xA14C6000, cellMediatorReliablePostReports),
        PS_HLE_FUNC(0x5D4431F0, cellMediatorFlushCache),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
