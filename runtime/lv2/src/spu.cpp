// sys_spu: SPU images, thread groups and threads. Reference: RPCS3 rpcs3/Emu/Cell/lv2/sys_spu.h/.cpp.
//
// Only what games are seen calling (CLAUDE.md rule 4). SPURS is native (D-037) and runs SPU code without lv2 thread
// groups, so thread groups and threads arrive when a game creates them directly.
//
// sys_spu_initialize: the process's split of the six SPUs between thread groups and raw SPUs (RPCS3 spu_limits_t).
// No thread groups or raw SPUs exist yet, so RPCS3's check_busy reduces to its limit checks. sys_spu_thread_group_log:
// the log switch.

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

// sys_spu_thread_group_log(command, stat), as RPCS3: the process-wide SPU thread group log switch. 0 turns it on
// (the default), 1 off, 2 reads it into *stat (CELL_EFAULT without one); anything else is CELL_EINVAL. Nothing is
// logged either way: the switch only answers what it was set to.
Result sys_spu_thread_group_log(PpuThread& t, s32 command, u32 stat)
{
    constexpr s32 kOn = 0, kOff = 1, kGetStatus = 2;
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard lock(s.spu_limits.mutex);
    switch (command) {
    case kGetStatus:
        if (!stat || !s.guest.valid(stat, 4)) return err(CELL_EFAULT);
        s.guest.w32(stat, static_cast<u32>(s.spu_limits.group_log));
        return CELL_OK;
    case kOn:
    case kOff:
        s.spu_limits.group_log = command;
        return CELL_OK;
    default: return err(CELL_EINVAL);
    }
}

void register_spu_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 169, sys_spu_initialize);
    PS_LV2_BIND(table, 254, sys_spu_thread_group_log);
}

} // namespace pspocket::lv2
