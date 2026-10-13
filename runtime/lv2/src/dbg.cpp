// lv2's debugging calls a retail console has (D-937), ported from the retail kernel's own handlers (lv2_kernel.self of
// the 4.93 system software, read in inventory/firmware/kernel493 and never committed; the handler addresses below are
// the kernel's). RPCS3 answers these with null_func (CELL_OK, nothing written).
//
// What a retail kernel checks first (see docs/decisions.md D-937):
// - A process's privileges are its SELF's control flags (supplemental header 1: 256 bits), which some calls compare as
//   a number with a level: the system software's modules have 0x4000... and the kernel 0x8000...; a game has 0 (its
//   EBOOT's flags are zero). pspocket's root process (tests, the system software's place) stands for the system
//   software's level; a game's process for a game's.

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

// The calling process's control flags reach the system software's level (0x4000... and above).
bool system_level(PpuThread& t) { return t.kernel.config().root; }

} // namespace

// sys_dbg_get_console_type(u64* type), the kernel's 0x8000000000089AE0: a null pointer is CELL_EFAULT; a process below
// the system software's level gets CELL_ENOSYS; else 1 on a retail console (2 a development one, 3 a debug one).
Result sys_dbg_get_console_type(PpuThread& t, u32 type)
{
    if (!type) return err(CELL_EFAULT);
    if (!system_level(t)) return err(CELL_ENOSYS);
    if (!t.kernel.impl().guest.put64(type, 1)) return err(CELL_EFAULT);
    return CELL_OK;
}

void register_dbg_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 985, sys_dbg_get_console_type);
}

} // namespace pspocket::lv2
