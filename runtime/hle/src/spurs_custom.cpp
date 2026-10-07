// Custom SPURS workloads (D-037, D-051): a policy module the game itself supplies with cellSpursAddWorkload. It is game
// code, so unlike Sony's policy modules it runs: the worker loads it into local store and enters it the way the SPURS
// kernel does, and the kernel's own entry points it may call are traps the worker handles. References: libsre's SPURS
// kernel (fw 4.92, read as the reference, D-046: dispatch, the select-workload and exit entry points), RPCS3
// rpcs3/Emu/Cell/Modules/cellSpursSpu.cpp spursKernelDispatchWorkload and cellSpursModulePollStatus.
//
// Entry (kernel 1 dispatch): the module image (WorkloadInfo.size bytes from WorkloadInfo.addr) at LS 0xA00, loaded only
// when another image was there, as the kernel does, so a module's local store survives between runs; then
// $0 = 0x804 (the return into the kernel), $1 = 0x3FFB0, $3 = 0x100 (the kernel context), $4 = WorkloadInfo.arg
// (preferred doubleword), $5 = the poll status, pc = 0xA00. A module leaves by returning to 0x804 or by calling the
// kernel's exit at [0x1E0] (0x808); it asks whether to give the SPU up by calling [0x1E4] (0x290) with $3 = 1, which
// answers with the selected workload ID in word 0 and its poll status in word 1. SPURS2's kernel 2 calls the module from
// 0x834: $0 and [0x1E0] are 0x838 (libsre's second SPU kernel; D-171).

#include <cstring>
#include <format>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/registry.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_programs.hpp"
#include "spurs_native.hpp"

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kCustomChan = "spurs";

ps_spu_v128 word0(u32 v)
{
    ps_spu_v128 r{};
    r.u32[3] = v;
    return r;
}

} // namespace

void run_custom(Worker& w, u32 wid, u32 poll_status)
{
    u8* base = w.sched.base;
    const u32 info = wkl_info(w.sched.spurs, wid);
    const u32 pm = static_cast<u32>(ps_read64(base, info + wkl_off::info_addr));
    const u64 arg = ps_read64(base, info + wkl_off::info_arg);
    const u32 size = ps_read32(base, info + wkl_off::info_size);
    if (!pm || pm % 16 || !size || ls::pm_entry + u64{size} > ls::size || !w.sched.kernel.memory().is_committed(pm)) {
        PS_FATAL(kCustomChan, "SPURS {:#x} SPU {}: workload {}'s policy module ({:#x} bytes at {:#x}) cannot be loaded",
            w.sched.spurs, w.spu_num, wid, size, pm);
    }
    if (ps_read64(w.ls, ls::k_wkl_addr) != pm) {
        std::memcpy(w.ls + ls::pm_entry, base + pm, size);
        ps_write64(w.ls, ls::k_wkl_addr, pm);
        w.custom_additions = spu::registry_additions();
        w.custom_size = size & ~3u;
        w.custom_program = identify_program(base, {ImageKind::PolicyModule, pm, ls::pm_entry, w.custom_size, ls::pm_entry},
            &w.custom_hash);
        w.custom_dropped = false;
        if (!w.custom_load_logged) {
            PS_LOG_INFO(kCustomChan, "SPURS {:#x} SPU {}: workload {}'s own policy module ({:#x} bytes at {:#x}, argument "
                "{:#x}) loaded and started (reloads are logged at debug level)", w.sched.spurs, w.spu_num, wid, size, pm, arg);
            w.custom_load_logged = true;
        } else {
            PS_LOG_DEBUG(kCustomChan, "SPURS {:#x} SPU {}: workload {}'s own policy module reloaded", w.sched.spurs, w.spu_num,
                wid);
        }
    }
    ps_write32(w.ls, ls::k_wkl_unique, ps_read8(base, info + wkl_off::info_unique));
    ps_write16(w.ls, ls::k_module_id, 0);

    SPUContext& c = w.ctx;
    for (auto& r : c.gpr) r = ps_spu_v128{};
    c.gpr[0] = word0(w.sched.second ? kKernel2Exit : kKernelReturn); // after kernel 1's call at 0x800, kernel 2's at 0x834
    c.gpr[1] = word0(0x3FFB0);
    c.gpr[3] = word0(ls::kernel);
    c.gpr[4].u64[1] = arg;
    c.gpr[5] = word0(poll_status);
    c.pc = ls::pm_entry;
    // Identified when loaded (D-076); with none, looked up again when programs were registered since, and while it runs
    // (D-234).
    const bool again = !w.custom_program && !w.custom_dropped;
    if (again && spu::registry_additions() != w.custom_additions) {
        w.custom_additions = spu::registry_additions();
        w.custom_program = spu::find_program_checked(w.custom_hash, ls::pm_entry, w.custom_size, false, w.ls);
        if (w.custom_program) count_reidentified();
    }
    ps_spu_attach_program(&c, w.custom_program);
    if (again && !w.custom_program)
        spu::set_pending_home(&c, w.custom_hash, ls::pm_entry, w.custom_size, false, w.custom_additions);

    for (;;) {
        const u32 reason = ps_spu_run(&c);
        if (w.custom_program && !c.home) { // a DMA over the module's code dropped it: interpreted from now on
            w.custom_program = nullptr;
            w.custom_dropped = true;
        } else if (!w.custom_program && !w.custom_dropped && c.home) {
            w.custom_program = c.home; // registered while it ran (D-233)
        }
        if (reason == PS_SPU_STOP_REQUESTED) return;
        if (reason == PS_SPU_STOP_SIGNAL && c.stop_code == kKernelExitStop) return; // 0x804 or [0x1E0]
        if (reason == PS_SPU_STOP_SIGNAL && c.stop_code == kKernelSelectStop) {
            // cellSpursModulePollStatus: [0x1E4] with $3 = 1. Only the kernel itself selects with $3 = 0.
            if (c.gpr[3].u32[3] != 1) {
                PS_LOG_WARN(kCustomChan, "SPURS {:#x}: workload {}'s policy module calls the kernel's select with $3 = {:#x}; "
                    "answered as a poll", w.sched.spurs, wid, c.gpr[3].u32[3]);
            }
            const Worker::Selection sel = w.select(true);
            c.gpr[3] = ps_spu_v128{};
            c.gpr[3].u32[3] = sel.wid;
            c.gpr[3].u32[2] = sel.poll_status;
            c.pc = c.gpr[0].u32[3] & PS_SPU_PC_MASK;
            continue;
        }
        PS_FATAL(kCustomChan, "SPURS {:#x} SPU {}: workload {}'s policy module stopped (reason {}, code {:#x}{}, exception {}) "
            "at 0x{:05x}", w.sched.spurs, w.spu_num, wid, reason, c.stop_code,
            c.stop_code == 0x3FFF ? ", stopd: an assertion" : "", c.exception, c.pc);
    }
}

} // namespace pspocket::hle::spurs
