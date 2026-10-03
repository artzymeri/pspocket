// sys_spu: SPU images, thread groups and threads. Reference: RPCS3 rpcs3/Emu/Cell/lv2/sys_spu.h/.cpp.
//
// Only what games are seen calling (CLAUDE.md rule 4). SPURS is native (D-037) and runs SPU code without lv2 thread
// groups, so thread groups and threads arrive when a game creates them directly.
//
// sys_spu_initialize: the process's split of the six SPUs between thread groups and raw SPUs (RPCS3 spu_limits_t).
// No thread groups or raw SPUs exist yet, so RPCS3's check_busy reduces to its limit checks.

#include "internal.hpp"

namespace pspocket::lv2 {

Result sys_spu_initialize(PpuThread& t, u32 max_usable_spu, u32 max_raw_spu)
{
    if (max_raw_spu > 5) return err(CELL_EINVAL);
    max_usable_spu = 6; // RPCS3: "This value can be changed by VSH in theory"
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard lock(s.spu_limits.mutex);
    s.spu_limits.max_raw = max_raw_spu;
    s.spu_limits.max_spu = max_usable_spu - max_raw_spu;
    PS_LOG_INFO("lv2", "sys_spu_initialize: {} SPUs for thread groups, {} raw SPUs", s.spu_limits.max_spu, max_raw_spu);
    return CELL_OK;
}

void register_spu_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 169, sys_spu_initialize);
}

} // namespace pspocket::lv2
