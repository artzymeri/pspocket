// cellNetCtl: network connection control. Online play is a non-goal (D-024): the library initializes, reports the
// connection as disconnected, and accepts handler registrations that never fire.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellNetCtl.cpp/.h (with RPCS3's network set to "disconnected").

#include <atomic>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellNetCtl";

enum : u32 {
    CELL_NET_CTL_ERROR_NOT_INITIALIZED = 0x80130101,
    CELL_NET_CTL_ERROR_NOT_TERMINATED = 0x80130102,
    CELL_NET_CTL_ERROR_INVALID_ID = 0x80130105,
    CELL_NET_CTL_ERROR_INVALID_ADDR = 0x80130107,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }
constexpr s32 CELL_NET_CTL_STATE_Disconnected = 0;

struct NetCtlState {
    std::atomic<bool> initialized{false};
};

NetCtlState& netctl() { return HleProcess::current()->state<NetCtlState>(); }

s32 cellNetCtlInit(PPUContext&)
{
    if (netctl().initialized.exchange(true)) return error(CELL_NET_CTL_ERROR_NOT_TERMINATED);
    PS_LOG_INFO(kChan, "cellNetCtlInit(): no network, the connection state is Disconnected");
    return lv2::CELL_OK;
}

void cellNetCtlTerm(PPUContext&) { netctl().initialized = false; }

s32 cellNetCtlGetState(PPUContext& ctx, u32 state)
{
    if (!netctl().initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!state) return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    ps_write32(thread_of(ctx).kernel.base(), state, static_cast<u32>(CELL_NET_CTL_STATE_Disconnected));
    return lv2::CELL_OK;
}

// RPCS3: a TODO that validates and returns CELL_OK without writing a handler ID.
s32 cellNetCtlAddHandler(PPUContext&, u32 handler, u32 arg, u32 hid)
{
    PS_LOG_DEBUG(kChan, "cellNetCtlAddHandler(handler {:#x}, arg {:#x}, hid {:#x}): never called (no network)", handler,
        arg, hid);
    if (!netctl().initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!hid) return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    return lv2::CELL_OK;
}

s32 cellNetCtlDelHandler(PPUContext&, s32 hid)
{
    if (!netctl().initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (hid > 3) return error(CELL_NET_CTL_ERROR_INVALID_ID);
    return lv2::CELL_OK;
}

} // namespace

void register_cellNetCtl(Registry& r)
{
    r.add("cellNetCtl", {
        PS_HLE_FUNC(0xBD5A59FC, cellNetCtlInit),
        PS_HLE_FUNC(0x105EE2CB, cellNetCtlTerm),
        PS_HLE_FUNC(0x8B3EBA69, cellNetCtlGetState),
        PS_HLE_FUNC(0x0CE13C6B, cellNetCtlAddHandler),
        PS_HLE_FUNC(0x901815C3, cellNetCtlDelHandler),
    });
}

} // namespace pspocket::hle
