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

} // namespace

void register_sys_net(Registry& r)
{
    r.add("sys_net", {
        PS_HLE_FUNC(0x139A9E9B, sys_net_initialize_network_ex),
    });
}

} // namespace pspocket::hle
