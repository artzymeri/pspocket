// sys_net (libnet): BSD sockets and network setup. Online play is a non-goal (D-024); the library only has to come up
// without a network. Reference: RPCS3 rpcs3/Emu/Cell/Modules/sys_net_.cpp.

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_net";

// RPCS3: a TODO that returns CELL_OK (the parameter only sizes libnet's memory pool).
s32 sys_net_initialize_network_ex(PPUContext&, u32 param)
{
    PS_LOG_INFO(kChan, "sys_net_initialize_network_ex({:#x}): no network; returns CELL_OK (as RPCS3)", param);
    return lv2::CELL_OK;
}

// RPCS3: UNIMPLEMENTED, returns CELL_OK. A debug printout of the network interfaces.
s32 sys_net_show_ifconfig(PPUContext&)
{
    PS_LOG_DEBUG(kChan, "sys_net_show_ifconfig(): no interfaces");
    return lv2::CELL_OK;
}

// sys_net_free_thread_context(u64 tid, s32 flags). RPCS3: a TODO that returns CELL_OK. libnet frees the context it
// keeps for a thread that used it (its errno and resolver state); there is none here.
s32 sys_net_free_thread_context(PPUContext&, u64 tid, s32 flags)
{
    PS_LOG_DEBUG(kChan, "sys_net_free_thread_context(tid {:#x}, flags {}): no thread context; CELL_OK (as RPCS3)", tid,
        flags);
    return lv2::CELL_OK;
}

} // namespace

void register_sys_net(Registry& r)
{
    r.add("sys_net", {
        PS_HLE_FUNC(0x139A9E9B, sys_net_initialize_network_ex),
        PS_HLE_FUNC(0xB48636C4, sys_net_show_ifconfig),
        PS_HLE_FUNC(0xFDB8F926, sys_net_free_thread_context),
    });
}

} // namespace pspocket::hle
