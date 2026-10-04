#pragma once

// Native SPURS internals shared by cellSpurs.cpp (the PPU API) and the native scheduler and policies
// (spurs_scheduler.cpp, spurs_taskset.cpp, spurs_jobchain.cpp, spurs_custom.cpp): error codes, guest structure offsets
// (RPCS3 cellSpurs.h layouts, checked against libsre where noted, D-047) and the host side of each SPURS instance.

#include <map>
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
    CELL_SPURS_JOB_ERROR_AGAIN = 0x80410A01,
    CELL_SPURS_JOB_ERROR_INVAL = 0x80410A02,
    CELL_SPURS_JOB_ERROR_PERM = 0x80410A09,
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
constexpr u32 CELL_SPURS_JOB_REVISION_0 = 0, CELL_SPURS_JOB_REVISION_1 = 1, CELL_SPURS_JOB_REVISION_2 = 2,
              CELL_SPURS_JOB_REVISION_3 = 3;
constexpr u32 CELL_SPURS_MAX_WORKLOAD = 16;
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
              exitIfNoWork = 0x14, prefix = 0x15, prefixSize = 0x24, flags = 0x28, size = 0x200;
// Not read yet: container 0x2C, swlPriority 0x38, swlMaxSpu 0x40, swlIsPreem 0x44 (system workload).
}

// CellSpurs (4096 bytes for SPURS1, 8192 for SPURS2; 128-aligned): the fields stage 1 writes.
namespace spurs_off {
// wklFlag: CellSpursWorkloadFlag {u64, u32, u32 flag} at 0x60; the flag word is at 0x6C (RPCS3 cellSpurs.h, libsre).
constexpr u32 wklFlagStruct = 0x60, wklFlag = 0x6C, flags1 = 0x74, nSpus = 0x76, wklFlagReceiver = 0x77, wklEnabled = 0xB0,
              sysSrvPreemptWklId = 0xC0, sysSrvTrace = 0xCC, unk11 = 0x988, unk12 = 0x98C, unk13 = 0x990,
              wklInfoSysSrv = 0xD00, ppu0 = 0xD20, ppu1 = 0xD28, handlerDirty = 0xD64, handlerWaiting = 0xD65,
              handlerExiting = 0xD66, spuImg = 0xD70, flags = 0xD80, spuPriority = 0xD84, ppuPriority = 0xD88,
              prefix = 0xD8C, prefixSize = 0xD9B, revision = 0xDA0, sdkVersion = 0xDA4;
}
// Workload tables of CellSpurs (SPURS1: wid 0..15).
namespace wkl_off {
constexpr u32 readyCount1 = 0x00, idleSpuCountOrReadyCount2 = 0x10, currentContention = 0x20,
              pendingContention = 0x30, minContention = 0x40, maxContention = 0x50, signal1 = 0x70,
              sysSrvMessage = 0x72, state1 = 0x80, status1 = 0x90, event1 = 0xA0, enabled = 0xB0, mskB = 0xB4,
              status2 = 0xE0,
              sysSrvMsgUpdateWorkload = 0xBD, f1 = 0x100, f1_stride = 0x80, info1 = 0xB00, info_stride = 0x20,
              h1 = 0xE00, h_stride = 0x10, exception = 0xD6C;
// WorkloadInfo: addr (u64), arg (u64), size (u32), uniqueId (u8), priority[8] at 0x18.
constexpr u32 info_addr = 0x00, info_arg = 0x08, info_size = 0x10, info_unique = 0x14, info_priority = 0x18;
} // namespace wkl_off

// CellSpursTasksetAttribute (512 bytes) and CellSpursTaskset (6400 bytes, 128-aligned).
namespace tsattr_off {
constexpr u32 revision = 0x00, sdk_version = 0x04, args = 0x08, priority = 0x10, max_contention = 0x18, name = 0x1C,
              taskset_size = 0x20, enable_clear_ls = 0x24, size = 0x200;
}
// Task bitsets (128 bits each, task 0 the top bit): running 0x00, ready 0x10, pending_ready 0x20, enabled 0x30,
// signalled 0x40, waiting 0x50.
namespace ts_off {
constexpr u32 pending_ready = 0x20, enabled = 0x30;
// TaskInfo[128] at 0x80, 0x30 bytes each: args (16 bytes), elf (u64), context save storage | allocated LS blocks
// (u64), LS pattern (16 bytes).
constexpr u32 task_info = 0x80, task_info_stride = 0x30, ti_args = 0x00, ti_elf = 0x10, ti_context = 0x18,
              ti_ls_pattern = 0x20;
constexpr u32 spurs = 0x60, args = 0x68, enable_clear_ls = 0x70, wkl_flag_wait_task = 0x72, wid = 0x74,
              size = 0x1890;
}

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
              spurs = 0x78, error = 0x80, cause = 0x88, sdkVer = 0x90, jobMemoryCheck = 0x94;
// flags (0x2C): isFixedMemAlloc, (maxSizeJobDescriptor - 0x100) / 0x80, waiting on a guard, haltOnError.
constexpr u8 flagFixedMem = 0x80, flagMaxSizeMask = 0x70, flagGuardWait = 0x08, flagHaltOnError = 0x04;
} // namespace jc_off

// sys_spu_image {type, entry_point, segs, nsegs} and CellSpurs::WorkloadInfo {addr (u64), arg, size, uniqueId, ...}.
constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0;

// Host side of every initialized SPURS instance, keyed by the guest CellSpurs address.
struct Workload {
    enum class Kind { Taskset, JobChain, Custom } kind = Kind::Custom;
    u32 arg = 0; // the taskset (Taskset) or the policy module's argument
};

class Scheduler; // spurs_scheduler.cpp

struct Instance {
    u32 nspus = 0;
    bool second = false;
    std::map<u32, Workload> workloads; // by workload ID
    // SPU port -> lv2 event queue ID. On the PS3, lv2 connects these on every thread of SPURS' SPU thread group
    // (sys_spu_thread_group_connect_event_all_threads); a native SPURS has no group, so the instance keeps them, and
    // SPU code run by the native scheduler sends its events through them.
    std::map<u8, u32> spu_ports;
    // The native scheduler: one host worker per SPU, running the instance's workloads (stage 3b).
    std::shared_ptr<Scheduler> scheduler;
};

struct SpursHost {
    std::mutex mutex;
    std::map<u32, Instance> instances;
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

// _cellSpursSendSignal (PPU side, as libsre and RPCS3): sets the task's signalled bit; when the task was waiting for
// it, signals the taskset's workload so a worker resumes it. CELL_OK, or a CELL_SPURS_TASK_ERROR_*.
s32 send_signal(u8* base, u32 taskset, u32 task_id);

// Wakes the instance's idle workers (cellSpursWakeUp, and HLE writes to CellSpurs line 0 that don't go through the
// reservation system).
void wake_workers(u32 spurs);

} // namespace pspocket::hle::spurs
