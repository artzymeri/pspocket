#pragma once

// Native SPURS internals shared by cellSpurs.cpp (the PPU API) and the native scheduler and policies
// (spurs_scheduler.cpp, spurs_taskset.cpp, spurs_jobchain.cpp, spurs_custom.cpp): error codes, guest structure offsets
// (RPCS3 cellSpurs.h layouts, checked against libsre where noted, D-047) and the host side of each SPURS instance.

#include <functional>
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <string_view>

#include "pspocket/core/log.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::lv2 {
class Kernel;
}

namespace pspocket::hle::spurs {

constexpr std::string_view kChan = "cellSpurs";

enum : u32 {
    CELL_SPURS_CORE_ERROR_AGAIN = 0x80410701,
    CELL_SPURS_CORE_ERROR_INVAL = 0x80410702,
    CELL_SPURS_CORE_ERROR_NOMEM = 0x80410704,
    CELL_SPURS_CORE_ERROR_SRCH = 0x80410705,
    CELL_SPURS_CORE_ERROR_PERM = 0x80410709,
    CELL_SPURS_CORE_ERROR_BUSY = 0x8041070A,
    CELL_SPURS_CORE_ERROR_STAT = 0x8041070F,
    CELL_SPURS_CORE_ERROR_ALIGN = 0x80410710,
    CELL_SPURS_CORE_ERROR_NULL_POINTER = 0x80410711,
};
enum : u32 {
    CELL_SPURS_POLICY_MODULE_ERROR_AGAIN = 0x80410801,
    CELL_SPURS_POLICY_MODULE_ERROR_INVAL = 0x80410802,
    CELL_SPURS_POLICY_MODULE_ERROR_SRCH = 0x80410805,
    CELL_SPURS_POLICY_MODULE_ERROR_PERM = 0x80410809,
    CELL_SPURS_POLICY_MODULE_ERROR_BUSY = 0x8041080A,
    CELL_SPURS_POLICY_MODULE_ERROR_STAT = 0x8041080F,
    CELL_SPURS_POLICY_MODULE_ERROR_ALIGN = 0x80410810,
    CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER = 0x80410811,
    CELL_SPURS_TASK_ERROR_AGAIN = 0x80410901,
    CELL_SPURS_TASK_ERROR_INVAL = 0x80410902,
    CELL_SPURS_TASK_ERROR_SRCH = 0x80410905,
    CELL_SPURS_TASK_ERROR_PERM = 0x80410909,
    CELL_SPURS_TASK_ERROR_BUSY = 0x8041090A,
    CELL_SPURS_TASK_ERROR_NOEXEC = 0x80410907,
    CELL_SPURS_TASK_ERROR_FAULT = 0x8041090D,
    CELL_SPURS_TASK_ERROR_STAT = 0x8041090F,
    CELL_SPURS_TASK_ERROR_ALIGN = 0x80410910,
    CELL_SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
    CELL_SPURS_TASK_ERROR_FATAL = 0x80410914,
    CELL_SPURS_TASK_ERROR_SHUTDOWN = 0x80410920,
    CELL_SPURS_JOB_ERROR_AGAIN = 0x80410A01,
    CELL_SPURS_JOB_ERROR_INVAL = 0x80410A02,
    CELL_SPURS_JOB_ERROR_PERM = 0x80410A09,
    CELL_SPURS_JOB_ERROR_BUSY = 0x80410A0A,
    CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR = 0x80410A0B,
    CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR_SIZE = 0x80410A0C,
    CELL_SPURS_JOB_ERROR_STAT = 0x80410A0F,
    CELL_SPURS_JOB_ERROR_ALIGN = 0x80410A10,
    CELL_SPURS_JOB_ERROR_NULL_POINTER = 0x80410A11,
    CELL_SPURS_JOB_ERROR_MEMORY_SIZE = 0x80410A17,
    CELL_SPURS_JOB_ERROR_UNKNOWN_COMMAND = 0x80410A18,
    CELL_SPURS_JOB_ERROR_JOBLIST_ALIGNMENT = 0x80410A19,
    CELL_SPURS_JOB_ERROR_JOB_ALIGNMENT = 0x80410A1A,
    CELL_SPURS_JOB_ERROR_CALL_OVERFLOW = 0x80410A1B,
    CELL_SPURS_JOB_ERROR_ABORT = 0x80410A1C,
    CELL_SPURS_JOB_ERROR_DMALIST_ELEMENT = 0x80410A1D,
    CELL_SPURS_JOB_ERROR_NUM_CACHE = 0x80410A1E,
    CELL_SPURS_JOB_ERROR_INVALID_BINARY = 0x80410A1F,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_SPURS_MAX_SPU = 8;
constexpr u32 CELL_SPURS_NAME_MAX_LENGTH = 15;
constexpr u32 CELL_SPURS_SIZE = 4096;
constexpr u32 CELL_SPURS_SIZE2 = 8192;
constexpr u32 CELL_SPURS_ALIGN = 128;
constexpr u32 CELL_SPURS_KERNEL1_ENTRY_ADDR = 0x818;
constexpr u32 CELL_SPURS_KERNEL2_ENTRY_ADDR = 0x848;
constexpr u32 SPURS_IMG_ADDR_SYS_SRV_WORKLOAD = 0x100;
constexpr u32 SPURS_IMG_ADDR_TASKSET_PM = 0x200; // marker for the taskset policy module (RPCS3), handled natively
constexpr u32 kTasksetPmSize = 0x1E40;
// Job chains: RPCS3 registers them with no policy module (vm::null, a TODO), so they never run there. The native
// scheduler needs to recognize them, so they get a marker of their own next to RPCS3's 0x100 and 0x200.
constexpr u32 SPURS_IMG_ADDR_JOB_CHAIN_PM = 0x300;
// libsre's exit handler for tasks with an exit code container, whose descriptor (EA | size in 128-byte units) a
// CellSpursTaskset2 keeps at 0x1900: a marker here too (no task of a supported game has such a container).
constexpr u32 SPURS_IMG_ADDR_TASK_EXIT_CODE_HANDLER = 0x400, kTaskExitCodeHandlerSize = 0x710;
// libsre's handler for CellSpursTaskset2's own tasks (cellSpursCreateTask2), whose descriptor cellSpursCreateTaskset2
// keeps at 0x78: a marker as well (the taskset policy does its work natively when such a task exits, D-626).
constexpr u32 SPURS_IMG_ADDR_TASK2_HANDLER = 0x500, kTask2HandlerSize = 0x670;
constexpr u32 CELL_SPURS_JOB_REVISION_0 = 0, CELL_SPURS_JOB_REVISION_1 = 1, CELL_SPURS_JOB_REVISION_2 = 2,
              CELL_SPURS_JOB_REVISION_3 = 3;
constexpr u32 CELL_SPURS_MAX_WORKLOAD = 16;
constexpr u32 CELL_SPURS_MAX_TASK = 128; // tasks of a taskset
constexpr u32 CELL_SPURS_TASKSET_ATTRIBUTE_REVISION = 1;
constexpr u32 kTasksetSize = 6400;   // CellSpursTaskset
constexpr u32 kTaskset2Size = 10496; // CellSpursTaskset2
constexpr u8 SPURS_WKL_STATE_PREPARING = 1, SPURS_WKL_STATE_RUNNABLE = 2, SPURS_WKL_STATE_SHUTTING_DOWN = 3,
             SPURS_WKL_STATE_REMOVABLE = 4;
constexpr u32 CELL_SPURS_MAX_WORKLOAD2 = 32;
constexpr u32 CELL_SPURS_EVENT_FLAG_LAST = 3;       // ANY2ANY
constexpr u32 CELL_SPURS_EVENT_FLAG_CLEAR_LAST = 1; // MANUAL
constexpr u8 CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT = 0xFF;
constexpr u8 CELL_SPURS_EVENT_FLAG_SPU2PPU = 1, CELL_SPURS_EVENT_FLAG_PPU2SPU = 2, CELL_SPURS_EVENT_FLAG_ANY2ANY = 3;
constexpr u32 CELL_SPURS_EVENT_FLAG_OR = 0, CELL_SPURS_EVENT_FLAG_AND = 1;
constexpr u8 CELL_SPURS_EVENT_FLAG_CLEAR_AUTO = 0;
constexpr u32 CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS = 16;

// SpursAttrFlags
constexpr u32 SAF_EXIT_IF_NO_WORK = 0x00000001;
constexpr u32 SAF_SECOND_VERSION = 0x00000004;
constexpr u32 SAF_SYSTEM_WORKLOAD_ENABLED = 0x02000000;
constexpr u32 SAF_SPU_PRINTF_ENABLED = 0x10000000;
constexpr u32 SAF_SPU_TGT_EXCLUSIVE_NON_CONTEXT = 0x20000000;
constexpr u32 SAF_SPU_MEMORY_CONTAINER_SET = 0x40000000;
// SpursFlags1
constexpr u8 SF1_32_WORKLOADS = 0x40;
constexpr u8 SF1_EXIT_IF_NO_WORK = 0x80;

constexpr s32 SYS_SPU_THREAD_GROUP_TYPE_NORMAL = 0x00;
constexpr s32 SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT = 0x18;

// CellSpursAttribute (512 bytes, 8-aligned).
namespace attr_off {
constexpr u32 revision = 0x0, sdkVersion = 0x4, nSpus = 0x8, spuPriority = 0xC, ppuPriority = 0x10,
              exitIfNoWork = 0x14, prefix = 0x15, prefixSize = 0x24, flags = 0x28, container = 0x2C, size = 0x200;
// The system workload's priorities, SPUs and preemptible-SPU mask (cellSpursAttributeEnableSystemWorkload).
constexpr u32 swlPriority = 0x38, swlMaxSpu = 0x40, swlIsPreem = 0x44;
}

// CellSpurs (4096 bytes for SPURS1, 8192 for SPURS2; 128-aligned): the fields stage 1 writes.
namespace spurs_off {
// wklFlag: CellSpursWorkloadFlag {u64, u32, u32 flag} at 0x60; the flag word is at 0x6C (RPCS3 cellSpurs.h, libsre).
constexpr u32 wklFlagStruct = 0x60, wklFlag = 0x6C, flags1 = 0x74, sysSrvTraceControl = 0x75, nSpus = 0x76,
              wklFlagReceiver = 0x77, wklEnabled = 0xB0, sysSrvPreemptWklId = 0xC0, spuPort = 0xC9, sysSrvTrace = 0xCC,
              traceBuffer = 0x900, traceStartIndex = 0x908, traceDataSize = 0x948, traceMode = 0x950,
              unk11 = 0x988, unk12 = 0x98C, unk13 = 0x990,
              wklInfoSysSrv = 0xD00, ppu0 = 0xD20, ppu1 = 0xD28, spuTG = 0xD30, spus = 0xD34, eventQueue = 0xD5C,
              eventPort = 0xD60, handlerDirty = 0xD64, handlerWaiting = 0xD65,
              handlerExiting = 0xD66, enableEH = 0xD68, spuImg = 0xD70, flags = 0xD80, spuPriority = 0xD84,
              ppuPriority = 0xD88, prefix = 0xD8C, prefixSize = 0xD9B, revision = 0xDA0, sdkVersion = 0xDA4,
              globalSpuExceptionHandler = 0xF80, globalSpuExceptionHandlerArgs = 0xF88;
}
// CellSpursWorkloadAttribute (512 bytes, 8-aligned; RPCS3 cellSpurs.h).
namespace wattr_off {
constexpr u32 revision = 0x00, sdkVersion = 0x04, pm = 0x08, size = 0x0C, data = 0x10, priority = 0x18,
              minContention = 0x20, maxContention = 0x24, nameClass = 0x28, nameInstance = 0x2C, hook = 0x30,
              hookArg = 0x34, struct_size = 0x200;
}
// Workload tables of CellSpurs. SPURS1 has workloads 0..15; SPURS2 (SF1_32_WORKLOADS) also 16..31, whose state, status
// and event bytes, WorkloadInfo, wklF and names are second tables (state2 ..., info2, f2, h2), whose ready counts are
// wklIdleSpuCountOrReadyCount2 and whose signals are wklSignal2. In SPURS2 the contention and maximum contention bytes
// hold two workloads each: wid & 15 in the low nibble, wid | 16 in the high one (RPCS3 cellSpurs.h).
namespace wkl_off {
constexpr u32 readyCount1 = 0x00, idleSpuCountOrReadyCount2 = 0x10, currentContention = 0x20,
              pendingContention = 0x30, minContention = 0x40, maxContention = 0x50, signal1 = 0x70,
              sysSrvMessage = 0x72, signal2 = 0x78, state1 = 0x80, status1 = 0x90, event1 = 0xA0, enabled = 0xB0,
              mskB = 0xB4, state2 = 0xD0, status2 = 0xE0, event2 = 0xF0,
              sysSrvMsgUpdateWorkload = 0xBD, f1 = 0x100, f1_stride = 0x80, info1 = 0xB00, info_stride = 0x20,
              h1 = 0xE00, h_stride = 0x10, exception = 0xD6C, info2 = 0x1000, f2 = 0x1200, h2 = 0x1A00;
// In a workload's wklF entry: its lv2 semaphore (u64; never created natively), the waiter count x28, the
// shutdown-completion hook and its argument (u64 each).
constexpr u32 f1_sem = 0x20, f1_x28 = 0x28, f1_hook = 0x30, f1_hook_arg = 0x38;
// WorkloadInfo: addr (u64), arg (u64), size (u32), uniqueId (u8), priority[8] at 0x18.
constexpr u32 info_addr = 0x00, info_arg = 0x08, info_size = 0x10, info_unique = 0x14, info_priority = 0x18;
} // namespace wkl_off

// Where workload `wid`'s entries are (RPCS3 CellSpurs::wklState(), wklInfo(), wklSyncInfo(), wklName(), readyCount()):
// offsets in CellSpurs, or addresses for the ones taking `spurs`.
constexpr u32 wkl_state(u32 wid) { return (wid < 16 ? wkl_off::state1 : wkl_off::state2) + (wid & 15); }
constexpr u32 wkl_status(u32 wid) { return (wid < 16 ? wkl_off::status1 : wkl_off::status2) + (wid & 15); }
constexpr u32 wkl_event(u32 wid) { return (wid < 16 ? wkl_off::event1 : wkl_off::event2) + (wid & 15); }
constexpr u32 wkl_ready(u32 wid) { return (wid < 16 ? wkl_off::readyCount1 : wkl_off::idleSpuCountOrReadyCount2) + (wid & 15); }
constexpr u32 wkl_signal_of(u32 wid) { return wid < 16 ? wkl_off::signal1 : wkl_off::signal2; } // u16: wkl_signal_bit
constexpr u16 wkl_signal_bit(u32 wid) { return static_cast<u16>(0x8000u >> (wid & 15)); }
constexpr u32 wkl_info(u32 spurs, u32 wid)
{
    return spurs + (wid < 16 ? wkl_off::info1 : wkl_off::info2) + (wid & 15) * wkl_off::info_stride;
}
constexpr u32 wkl_f(u32 spurs, u32 wid) { return spurs + (wid < 16 ? wkl_off::f1 : wkl_off::f2) + (wid & 15) * wkl_off::f1_stride; }
constexpr u32 wkl_h(u32 spurs, u32 wid) { return spurs + (wid < 16 ? wkl_off::h1 : wkl_off::h2) + (wid & 15) * wkl_off::h_stride; }
// A workload's share of a contention byte (current, pending or maximum): the whole byte in SPURS1, its nibble in SPURS2.
constexpr u32 wkl_nibble_shift(bool second, u32 wid) { return second && wid >= 16 ? 4 : 0; }
constexpr u8 wkl_nibble_mask(bool second, u32 wid) { return second ? static_cast<u8>(0x0F << wkl_nibble_shift(second, wid)) : 0xFF; }

// CellSpursTasksetAttribute (512 bytes) and CellSpursTaskset (6400 bytes, 128-aligned).
namespace tsattr_off {
constexpr u32 revision = 0x00, sdk_version = 0x04, args = 0x08, priority = 0x10, max_contention = 0x18, name = 0x1C,
              taskset_size = 0x20, enable_clear_ls = 0x24, size = 0x200;
}
// CellSpursTasksetAttribute2 (512 bytes, 8-aligned): revision, name, args (u64), priority[8], max contention, clear LS
// (s32), the task name buffer (4 KB: 128 names of 32 bytes).
namespace tsattr2_off {
constexpr u32 revision = 0x00, name = 0x04, args = 0x08, priority = 0x10, max_contention = 0x18, enable_clear_ls = 0x1C,
              task_name_buffer = 0x20, size = 0x200;
}
// Task bitsets (128 bits each, task 0 the top bit): running 0x00, ready 0x10, pending_ready 0x20, enabled 0x30,
// signalled 0x40, waiting 0x50.
namespace ts_off {
constexpr u32 pending_ready = 0x20, enabled = 0x30;
// TaskInfo[128] at 0x80, 0x30 bytes each: args (16 bytes), elf (u64), context save storage | allocated LS blocks
// (u64), LS pattern (16 bytes).
constexpr u32 task_info = 0x80, task_info_stride = 0x30, ti_args = 0x00, ti_elf = 0x10, ti_context = 0x18,
              ti_ls_pattern = 0x20;
constexpr u32 spurs = 0x60, args = 0x68, enable_clear_ls = 0x70, wkl_flag_wait_task = 0x72, wid = 0x74, x78 = 0x78,
              exception_handler = 0x1880, exception_handler_arg = 0x1888, size = 0x1890;
// CellSpursTaskset2 only: its two lv2 event flags (task joins: tasks 0..63 the first, 64..127 the second, bit id % 64),
// the task name buffer (u64), the exit code handler, and each task's exit slot (16 bytes at exit_slots + 16 * id: an
// exit callback's descriptor and argument, or a CellSpursTaskset2 task's exit code in its first word, D-626).
constexpr u32 event_flag_id1 = 0x1898, event_flag_id2 = 0x189C, task_name_buffer = 0x18A8, exit_code_handler = 0x1900,
              exit_slots = 0x1980;
}
// A CellSpursTaskset2 task's lv2 event flag (its ID's offset in the taskset) and bit (D-626).
constexpr u32 task2_flag_offset(u32 id) { return id < 64 ? ts_off::event_flag_id1 : ts_off::event_flag_id2; }
constexpr u64 task2_flag_bit(u32 id) { return u64{1} << (id & 63); }
constexpr u32 CELL_SPURS_MAX_TASK_NAME_LENGTH = 32; // a name in the task name buffer, its NUL included

// CellSpursTaskExitCode (128 bytes, 128-aligned) as libsre's exit code handler uses it (D-195, D-627): the status word
// (bit 0 initialized: a task created with the container will store its code; bit 2 the code is stored; bit 16 a PPU
// thread waits, libsre's own PPU side with an event flag at +0x40; bit 17 an SPU task waits), the exit code, and a
// waiting SPU task's taskset (u64) and ID (a byte).
namespace exitcode_off {
constexpr u32 status = 0x20, code = 0x28, waiter_taskset = 0x30, waiter_id = 0x38, size = 0x80;
}
constexpr u32 kExitCodeInitialized = 1u, kExitCodeStored = 4u, kExitCodePpuWaits = 1u << 16, kExitCodeSpuWaits = 1u << 17;

// CellSpursJobChainAttribute (8-aligned) and CellSpursJobChain (128-aligned; create clears 0x110 bytes), as libsre
// lays them out (fw 4.92, D-054). RPCS3's cellSpurs.h swaps the attribute's name and haltOnError, puts maxGrabbedJob
// where sizeJobDescriptor is and has no autoReadyCount.
namespace jcattr_off {
constexpr u32 jmVer = 0x00, sdkVer = 0x04, jobChainEntry = 0x08, sizeJobDescriptor = 0x0C, maxGrabbedJob = 0x0E,
              priorities = 0x10, maxContention = 0x18, autoSpuCount = 0x1C, tag1 = 0x20, tag2 = 0x24,
              isFixedMemAlloc = 0x28, maxSizeJobDescriptor = 0x2C, initSpuCount = 0x30, name = 0x34,
              haltOnError = 0x38, jobMemoryCheck = 0x3C;
}
namespace jc_off {
// 0x08: the call stack (3 u64, [2] the top); 0x20: an urgent call or job list is active; 0x21: FLUSH requests, 0x22:
// SPUs that stop taking jobs until their next poll, 0x25: SPUs with jobs in flight, 0x26/0x27: label request and
// pending masks (a bit per SPU); 0x28: the requested SPU count without autoReadyCount; 0x29: the lock of line 0;
// 0x2E: the ready count last stored; 0x30: four urgent command slots; 0x50/0x60: the active job lists (main stream,
// urgent).
constexpr u32 pc = 0x00, linkRegister = 0x08, urgentList = 0x20, flush = 0x21, lowered = 0x22, isHalted = 0x23,
              autoReadyCount = 0x24, busy = 0x25, labelRequest = 0x26, labelPending = 0x27, initSpuCount = 0x28,
              lock = 0x29, tag1 = 0x2A, tag2 = 0x2B, flags = 0x2C, jmVer = 0x2D, readyCount = 0x2E, val2F = 0x2F,
              urgentCmds = 0x30, jobLists = 0x50, sizeJobDescriptor = 0x70, maxGrabbedJob = 0x72, workloadId = 0x74,
              spurs = 0x78, error = 0x80, cause = 0x88, sdkVer = 0x90, jobMemoryCheck = 0x94, exceptionHandler = 0x98,
              exceptionHandlerArg = 0xA0;
// flags (0x2C): isFixedMemAlloc, (maxSizeJobDescriptor - 0x100) / 0x80, waiting on a guard, haltOnError.
constexpr u8 flagFixedMem = 0x80, flagMaxSizeMask = 0x70, flagGuardWait = 0x08, flagHaltOnError = 0x04;
} // namespace jc_off

// sys_spu_image {type, entry_point, segs, nsegs} and CellSpurs::WorkloadInfo {addr (u64), arg, size, uniqueId, ...}.
constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0;

// Host side of every initialized SPURS instance, keyed by the guest CellSpurs address.
struct Workload {
    enum class Kind { Taskset, JobChain, Custom } kind = Kind::Custom;
    u32 arg = 0; // the taskset (Taskset) or the policy module's argument
    std::string name; // the game's name for it, if it gave one ("AudioEngine", "Physics")
};

class Scheduler; // spurs_scheduler.cpp

struct Instance {
    u32 nspus = 0;
    bool second = false; // SPURS2: 32 workloads
    std::map<u32, Workload> workloads; // by workload ID
    // SPU port -> lv2 event queue ID. On the PS3, lv2 connects these on every thread of SPURS' SPU thread group
    // (sys_spu_thread_group_connect_event_all_threads); a native SPURS has no group, so the instance keeps them, and
    // SPU code run by the native scheduler sends its events through them.
    std::map<u8, u32> spu_ports;
    // The native scheduler: one host worker per SPU, running the instance's workloads (stage 3b).
    std::shared_ptr<Scheduler> scheduler;
    // The event helper PPU thread that calls shutdown-completion hooks (cellSpurs.cpp, D-171): its lv2 thread ID and
    // TLS block, 0 while there is none; `helper_failed` once creating it failed (not tried again).
    u32 helper = 0, helper_tls = 0;
    bool helper_failed = false;
};

// A CellSpursTaskset2's own tasks (cellSpursCreateTask2, D-626), by the taskset's address: the IDs of those created and
// not yet joined (a task that exited keeps its ID until cellSpursJoinTask2 takes its exit code, so a task created in
// between cannot take it) and those a JoinTask2 waits for. Bit 127 - id, as the taskset's bit sets.
struct Task2Ids {
    u128 unjoined = 0, joining = 0;
};

struct SpursHost {
    std::mutex mutex;
    std::map<u32, Instance> instances;
    std::map<u32, Task2Ids> task2;
};

inline SpursHost& host()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSpurs called without an HLE process");
    return p->state<SpursHost>();
}

// Event flags (CellSpursEventFlag, 128 bytes; layout confirmed field by field by the SPU-side library, D-047).
namespace ef_off {
constexpr u32 events = 0x00, spuTaskPendingRecv = 0x02, ppuWaitMask = 0x04, ppuWaitSlotAndMode = 0x06,
              ppuPendingRecv = 0x07, spuTaskUsedWaitSlots = 0x08, spuTaskWaitMode = 0x0A, spuPort = 0x0C,
              isIwl = 0x0D, direction = 0x0E, clearMode = 0x0F, spuTaskWaitMask = 0x10,
              pendingRecvTaskEvents = 0x30, waitingTaskId = 0x50, waitingTaskWklId = 0x60, addr = 0x70, eventPortId = 0x78, eventQueueId = 0x7C, size = 0x80;
}

// ------------------------------------------------------------------------------------------------- SPU ports
// (cellSpurs.cpp; shared with cellSpursQueue.cpp)

// Connects lv2 event queue `queue` to an SPU port of the SPURS instance (RPCS3 _spurs::attach_lv2_eq; libsre's
// helper at 0xAF2C): `port` itself, or with `dynamic` the lowest free one of 0x10..0x3F, written to `port`.
// `spurs_created`: a queue SPURS makes for itself (event flags, SPURS queues), which CellSpurs::spuPortBits does not
// show. CELL_OK, a CELL_SPURS_CORE_ERROR_* or an lv2 error.
s32 attach_lv2_eq(PPUContext& ctx, u32 spurs, u32 queue, u8& port, bool dynamic, bool spurs_created);
// Disconnects SPU port `port` (RPCS3 _spurs::detach_lv2_eq; libsre's helper at 0xB0E0).
s32 detach_lv2_eq(u8* base, u32 spurs, u8 port, bool spurs_created, u32 sdk_version);

// ------------------------------------------------------------------------------------------------- native scheduler

// Starts the scheduler of an initialized instance: `inst.nspus` host workers that run its workloads (the SPURS
// kernel's selection rules, then the workload's policy: the native taskset and job chain policies, or the game's own
// policy module). Workers sleep while nothing is ready and wake on any change to CellSpurs line 0, as the SPURS
// kernel's idle loop does. Called with the host mutex held.
void start_scheduler(SpursHost& host, lv2::Kernel& kernel, u32 spurs, Instance& inst);
// Stops and joins the workers (cellSpursFinalize; the process end does it through the Scheduler's destructor). Not
// called with the host mutex held: the workers take it.
void stop_scheduler_of(Scheduler& scheduler);
// The native workers running the workload now (D-136).
u32 workers_inside(const Scheduler& scheduler, u32 wid);

// _cellSpursSendSignal (PPU side, as libsre and RPCS3): sets the task's signalled bit; when the task was waiting for
// it, signals the taskset's workload so a worker resumes it. CELL_OK, or a CELL_SPURS_TASK_ERROR_*. For HLE callers
// (event flags, SPURS queues); the exported _cellSpursSendSignal (cellSpurs.cpp) also wakes SPURS, as RPCS3's does.
s32 send_signal(u8* base, u32 taskset, u32 task_id);
// The taskset update of _cellSpursSendSignal (RPCS3 spurs_taskset_signal_op), one reservation operation on the
// taskset's first line: sets the task's signalled bit. -1 when the task is not enabled or its bits are inconsistent
// (running and waiting, ready and pending ready, a bit of a task that is not enabled), and nothing changed; 1 when it
// was waiting for the signal and had none, so its workload needs one; 0 otherwise.
int signal_task(u8* base, u32 taskset, u32 task_id);

// Wakes the instance's idle workers (cellSpursWakeUp, and HLE writes to CellSpurs line 0 that don't go through the
// reservation system).
void wake_workers(u32 spurs);

// cellSyncLFQueueInitialize(queue, buffer, size, depth, direction, eaSignal) (cellSync.cpp), for
// _cellSpursLFQueueInitialize: CELL_OK or a CELL_SYNC_ERROR_*.
s32 sync_lfqueue_initialize(PPUContext& ctx, u32 queue, u32 buffer, u32 size, u32 depth, u32 direction, u32 ea_signal);

// _cellSyncLFQueueCompletePopPointer(queue, pointer, fpSendSignal, noQueueFull) (cellSync.cpp), for cellDaisy's
// LFQueue2CompletePopPointer: CELL_OK, a CELL_SYNC_ERROR_* or the signal function's result.
s32 sync_lfqueue_complete_pop_pointer(PPUContext& ctx, u32 queue, s32 pointer, u32 signal_fn, u32 no_queue_full);

// A signal function on the host for an LF queue's completion: (eaSignal, the waiter's ID) -> what the completion
// answers (the guest's fpSendSignal is called with the same two arguments).
using LfSignal = std::function<s32(PPUContext& ctx, u32 ea_signal, u32 waiter)>;
// _cellSyncLFQueuePushBody or PopBody (cellSync.cpp: `push`, its checks and both directions' forms, ANY2ANY's second
// ones) with `signal` waking the waiter a completion takes, for cellSpurs' _cellSpursLFQueuePushBody and PopBody
// (D-628): CELL_OK, a CELL_SYNC_ERROR_* or the signal's result.
s32 sync_lfqueue_body(PPUContext& ctx, std::string_view fn, u32 queue, u32 buffer, u32 is_blocking, bool push,
    const LfSignal& signal);

} // namespace pspocket::hle::spurs
