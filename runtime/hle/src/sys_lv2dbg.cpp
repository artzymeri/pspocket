// sys_lv2dbg (liblv2dbg): lv2's debugger interface. A retail system has liblv2dbg_for_cex, which denies every call:
// pspocket's is that module's behaviour (D-521), compared with it in tests/hle/test_lv2dbg.cpp. Each function answers
// CELL_LV2DBG_ERROR_DEOPERATIONDENIED (0x8001042C) and touches nothing, but sys_dbg_vm_get_page_information(addr, num,
// pageinfo), which first clears the `num` 8-byte entries at pageinfo. RPCS3 implements none of them (its
// sys_lv2dbg.cpp logs a todo and returns CELL_OK, as a debug kernel's library would succeed).

#include <array>
#include <string_view>

#include "guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr s32 CELL_LV2DBG_ERROR_DEOPERATIONDENIED = static_cast<s32>(0x8001042C);

s32 denied(PPUContext&) { return CELL_LV2DBG_ERROR_DEOPERATIONDENIED; }

// sys_dbg_vm_get_page_information(addr, num, sys_vm_page_information_t* pageinfo): the entries cleared, then denied.
// A pageinfo that is not guest memory is left alone (the module would fault, D-042).
s32 sys_dbg_vm_get_page_information(PPUContext& ctx, u32, u32 num, u32 pageinfo)
{
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 i = 0; i < num; i++) {
        if (!guest_readable(t, pageinfo + 8 * i, 8)) {
            log_call_site(ctx, t.kernel.base(), "sys_dbg_vm_get_page_information: the page information array is not "
                                                "guest memory");
            break;
        }
        ps_write64(t.kernel.base(), pageinfo + 8 * i, 0);
    }
    return CELL_LV2DBG_ERROR_DEOPERATIONDENIED;
}

constexpr std::array<std::string_view, 34> kDenied = {"sys_dbg_read_ppu_thread_context",
    "sys_dbg_read_spu_thread_context", "sys_dbg_read_spu_thread_context2", "sys_dbg_initialize_ppu_exception_handler",
    "sys_dbg_finalize_ppu_exception_handler", "sys_dbg_register_ppu_exception_handler",
    "sys_dbg_unregister_ppu_exception_handler", "sys_dbg_set_stacksize_ppu_exception_handler",
    "sys_dbg_signal_to_ppu_exception_handler", "sys_dbg_enable_floating_point_enabled_exception",
    "sys_dbg_disable_floating_point_enabled_exception", "sys_dbg_signal_to_coredump_handler",
    "sys_dbg_set_mask_to_ppu_exception_handler", "sys_dbg_get_coredump_params", "sys_dbg_get_mutex_information",
    "sys_dbg_get_cond_information", "sys_dbg_get_rwlock_information", "sys_dbg_get_event_queue_information",
    "sys_dbg_get_semaphore_information", "sys_dbg_get_lwmutex_information", "sys_dbg_get_lwcond_information",
    "sys_dbg_get_event_flag_information", "sys_dbg_get_ppu_thread_ids", "sys_dbg_get_spu_thread_group_ids",
    "sys_dbg_get_spu_thread_ids", "sys_dbg_get_ppu_thread_name", "sys_dbg_get_spu_thread_name",
    "sys_dbg_get_spu_thread_group_name", "sys_dbg_get_ppu_thread_status", "sys_dbg_get_spu_thread_group_status",
    "sys_dbg_set_address_to_dabr", "sys_dbg_get_address_from_dabr", "sys_dbg_mat_set_condition",
    "sys_dbg_mat_get_condition"};

} // namespace

void register_sys_lv2dbg(Registry& r)
{
    using detail::thunk;
    const auto f = [](std::string_view name, HleFn fn) { return Function{{}, name, recomp::nid_of(name), fn, false}; };
    r.add("sys_lv2dbg", {f("sys_dbg_vm_get_page_information", &thunk<&sys_dbg_vm_get_page_information>)});
    for (const std::string_view name : kDenied) r.add("sys_lv2dbg", {f(name, &thunk<&denied>)});
}

} // namespace pspocket::hle
