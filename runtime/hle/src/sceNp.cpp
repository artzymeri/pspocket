// sceNp: PlayStation Network. A non-goal (docs/phase0-gta5.md, D-024): every function reports "not initialized" or
// "offline" the way RPCS3 does when the user is not signed in, so single-player never waits on PSN.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNp.cpp/.h.
//
// sceNpInit is not imported by GTA V; sceNp2Init (sceNp2, not implemented yet) initializes NP there. Until a module
// sets NpState::initialized, the manager functions return SCE_NP_ERROR_NOT_INITIALIZED, as RPCS3 does before init.

#include <atomic>

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
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x8002AA03,
};
constexpr s32 SCE_NP_MANAGER_STATUS_OFFLINE = -1;

struct NpState {
    std::atomic<bool> initialized{false};
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

} // namespace

void register_sceNp(Registry& r)
{
    r.add("sceNp", {
        PS_HLE_FUNC(0xA7BFF757, sceNpManagerGetStatus),
    });
}

} // namespace pspocket::hle
