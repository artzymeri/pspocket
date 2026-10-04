// SPURS semaphores (CellSpursSemaphore), native: the PPU side GTA V imports. RPCS3 has no implementation (commented
// stubs), so this follows Sony's libsre (fw 4.92, read as the reference, D-046; never run, never copied):
// _cellSpursSemaphoreInitialize 0x17F04 and cellSpursSemaphoreGetTasksetAddress 0x18024, with the SPU-side semaphore
// library that SPU tasks link statically (P and V, read in GTA V's Physics tasks) for the rest of the protocol.
//
// CellSpursSemaphore is one 128-byte line (128-aligned). The PPU sets it up; only SPU tasks take and give units:
//
//   0x00  s32  count: the free units (initialize stores the total as it is: libsre checks nothing)
//   0x10  u8   SPU tasks waiting in P (15 at most); their task IDs from 0x11, oldest first
//   0x20  u8   the count at 0x10 again; the waiters' workload IDs from 0x21
//   0x30  u64  CellSpurs: a semaphore of the whole SPURS instance (cellSpursSemaphoreInitializeIWL), else 0
//   0x38  u64  the taskset; 0 for a semaphore of the whole instance
//   the rest is 0.
//
// The protocol, all on the SPU (libsre has no PPU P or V):
// - P: NULL_POINTER, ALIGN (128), then, whether it would have to wait or not, PERM outside a task (the kernel context's
//   module ID at 0x1E8 is not 'TK') and STAT for a task that couldn't wait (no context save area, or its stack not all
//   in its LS pattern). Then one atomic update of the line (GETLLAR/PUTLLC): with count > 0 it takes a unit (count - 1)
//   and returns. Otherwise it appends its task ID and workload ID to the lists (AGAIN when 15 tasks wait already),
//   leaves the count as it is, and waits for a signal (taskset syscall 2, wait object `semaphore | 1`). It returns when
//   signalled, without looking at the count again: the V that signalled it has handed it the unit.
// - V: NULL_POINTER, ALIGN, then one atomic update: with count >= 0 and tasks waiting, the oldest leaves the lists (the
//   others move up) and the count stays; otherwise count + 1 (a negative count, which only a negative total gives,
//   comes back up to 0 before a V wakes anyone). The waiter it took off gets a signal through the SPU-side
//   _cellSpursSendSignal: its signalled bit in the taskset's line and, when it waits already, its workload's bit in
//   CellSpurs' wklSignal1/2 (line 0 goes through the kernel context's copy at LS 0x100). Its taskset is the one at 0x38
//   or, for a semaphore of the whole instance, its workload's (WorkloadInfo.arg, read by DMA from the CellSpurs at LS
//   0x1C0; INVAL for a workload ID above 31).
// So a unit given back while tasks wait goes to the oldest waiter, never to the count, and the signal may reach the
// waiter before it has called syscall 2 (the taskset policy then returns at once). Everything here is task code the
// native taskset policy runs as it is (D-050): GETLLAR/PUTLLC on the semaphore, the taskset and CellSpurs line 0,
// syscall 2, DMA, and the kernel and taskset context fields it reads (module ID, CellSpurs, workload ID 0x1DC, DMA
// tag 0x1CC; task ID 0x27D4, context 0x2798, LS pattern 0x27A0, the syscall entry at 0x27C4).
//
// Implemented: _cellSpursSemaphoreInitialize, the one function GTA V imports. cellSpursSemaphoreGetTasksetAddress
// (semaphore NULL, misaligned, output NULL; then the taskset at 0x38) stays unimplemented until a game calls it
// (CLAUDE.md rule 4). Where libsre would touch a bad pointer, initialize refuses the call with TASK_ERROR_FAULT and a
// log of the call site (D-042).

#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "spurs.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

using namespace spurs;

constexpr u32 CELL_SPURS_SEMAPHORE_SIZE = 128, CELL_SPURS_SEMAPHORE_ALIGN = 128;

namespace sem_off {
constexpr u32 count = 0x00, spurs = 0x30, taskset = 0x38; // the waiter lists at 0x10 and 0x20 are the SPU side's
}

// OK when [addr, addr + size) is guest memory, else TASK_ERROR_FAULT and a log of the call site.
s32 semaphore_readable(PPUContext& ctx, std::string_view name, u32 addr, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return lv2::CELL_OK;
    log_call_site(ctx, t.kernel.base(), std::format("_cellSpursSemaphoreInitialize: the {} ({:#x}, {} bytes) is not "
        "readable guest memory; refused with CELL_SPURS_TASK_ERROR_FAULT", name, addr, size));
    return error(CELL_SPURS_TASK_ERROR_FAULT);
}

// _cellSpursSemaphoreInitialize(spurs, taskset, semaphore, total), as libsre (0x17F04). The public
// cellSpursSemaphoreInitialize(taskset, semaphore, total) passes spurs 0, cellSpursSemaphoreInitializeIWL(spurs,
// semaphore, total) taskset 0 (GTA V: spurs 0, its "Physics" taskset, 4 units). libsre's checks depend on spurs:
// - spurs given: semaphore NULL (NULL_POINTER), spurs then semaphore not 128-byte aligned (ALIGN). The taskset is not
//   looked at, only stored.
// - no spurs: taskset or semaphore NULL (NULL_POINTER), taskset then semaphore not 128-byte aligned (ALIGN), the
//   taskset's workload ID above 31 (INVAL).
// Then the semaphore is cleared and gets the total at 0x00, spurs (as passed, so 0 for a taskset's semaphore) at 0x30
// and the taskset at 0x38, followed by a lwsync. The total is not checked. Natively the line's reservations are broken
// too, as the PPU's stores break them on the PS3.
s32 _cellSpursSemaphoreInitialize(PPUContext& ctx, u32 spurs, u32 taskset, u32 semaphore, s32 total)
{
    if (spurs) {
        if (!semaphore) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
        if (spurs % CELL_SPURS_ALIGN || semaphore % CELL_SPURS_SEMAPHORE_ALIGN) {
            return error(CELL_SPURS_TASK_ERROR_ALIGN);
        }
    } else {
        if (!taskset || !semaphore) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
        if (taskset % 128 || semaphore % CELL_SPURS_SEMAPHORE_ALIGN) return error(CELL_SPURS_TASK_ERROR_ALIGN);
        if (const s32 rc = semaphore_readable(ctx, "taskset", taskset, ts_off::wid + 4); rc != lv2::CELL_OK) return rc;
        if (ps_read32(guest_base(ctx), taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) {
            return error(CELL_SPURS_TASK_ERROR_INVAL);
        }
    }
    if (const s32 rc = semaphore_readable(ctx, "semaphore", semaphore, CELL_SPURS_SEMAPHORE_SIZE); rc != lv2::CELL_OK) {
        return rc;
    }

    u8* base = guest_base(ctx);
    std::memset(base + semaphore, 0, CELL_SPURS_SEMAPHORE_SIZE);
    ps_write32(base, semaphore + sem_off::count, static_cast<u32>(total));
    ps_write64(base, semaphore + sem_off::taskset, taskset);
    ps_write64(base, semaphore + sem_off::spurs, spurs);
    __atomic_thread_fence(__ATOMIC_RELEASE); // libsre's lwsync
    ps_res_notify_write(semaphore, CELL_SPURS_SEMAPHORE_SIZE);
    if (spurs) {
        PS_LOG_INFO(kChan, "SPURS semaphore {:#010x} of SPURS {:#x} (taskset {:#x}): {} units", semaphore, spurs,
            taskset, total);
    } else {
        PS_LOG_INFO(kChan, "SPURS semaphore {:#010x} of taskset {:#x} (workload {}): {} units", semaphore, taskset,
            ps_read32(base, taskset + ts_off::wid), total);
    }
    return lv2::CELL_OK;
}

} // namespace

void register_cellSpursSemaphore(Registry& r)
{
    r.add("cellSpurs", {
        PS_HLE_FUNC(0x77CDAC0C, _cellSpursSemaphoreInitialize),
    });
}

} // namespace pspocket::hle
