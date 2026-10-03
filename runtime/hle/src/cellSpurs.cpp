// cellSpurs, native (D-037): the SPURS API on the host. Guest-visible structures keep RPCS3's layouts
// (rpcs3/Emu/Cell/Modules/cellSpurs.h); API semantics follow RPCS3 cellSpurs.cpp where it implements them.
//
// Stage 1: attributes, initialize and finalize. Stage 3a: workload registration and tasksets (PPU side; tasks do not
// run until the native taskset policy exists). Stage 4 (PPU side): event flags, lv2 event queue ports (D-040),
// barrier initialize. Initialize fills the CellSpurs fields RPCS3's
// _spurs::initialize sets that games and SPU code can observe, and registers a host instance. What a native SPURS
// does not create, because nothing of Sony's SPURS kernel runs: the SPU thread group and its threads (spuTG, spus[]),
// the PPU handler and event helper threads (ppu0, ppu1), the per-workload lv2 semaphores (wklF1[].sem, semPrv), the
// guest lwmutex/lwcond at 0xDB0/0xDC8 and the kernel's event queue and port. Those fields stay 0 (ppu0/ppu1 keep
// RPCS3's 0xFFFFFFFF "no thread"). Workloads, tasksets, tasks, job chains and SPU execution come next.

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <string>
#include <mutex>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

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
    CELL_SPURS_POLICY_MODULE_ERROR_STAT = 0x8041080F,
    CELL_SPURS_POLICY_MODULE_ERROR_ALIGN = 0x80410810,
    CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER = 0x80410811,
    CELL_SPURS_TASK_ERROR_AGAIN = 0x80410901,
    CELL_SPURS_TASK_ERROR_INVAL = 0x80410902,
    CELL_SPURS_TASK_ERROR_PERM = 0x80410909,
    CELL_SPURS_TASK_ERROR_BUSY = 0x8041090A,
    CELL_SPURS_TASK_ERROR_STAT = 0x8041090F,
    CELL_SPURS_TASK_ERROR_ALIGN = 0x80410910,
    CELL_SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
    CELL_SPURS_JOB_ERROR_AGAIN = 0x80410A01,
    CELL_SPURS_JOB_ERROR_INVAL = 0x80410A02,
    CELL_SPURS_JOB_ERROR_STAT = 0x80410A0F,
    CELL_SPURS_JOB_ERROR_ALIGN = 0x80410A10,
    CELL_SPURS_JOB_ERROR_NULL_POINTER = 0x80410A11,
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
constexpr u32 CELL_SPURS_JOB_REVISION_0 = 0, CELL_SPURS_JOB_REVISION_1 = 1, CELL_SPURS_JOB_REVISION_2 = 2,
              CELL_SPURS_JOB_REVISION_3 = 3;
constexpr u32 CELL_SPURS_MAX_WORKLOAD = 16;
constexpr u32 CELL_SPURS_TASKSET_ATTRIBUTE_REVISION = 1;
constexpr u32 kTasksetSize = 6400; // CellSpursTaskset (CellSpursTaskset2 is larger; not supported yet)
constexpr u8 SPURS_WKL_STATE_PREPARING = 1, SPURS_WKL_STATE_RUNNABLE = 2, SPURS_WKL_STATE_SHUTTING_DOWN = 3,
             SPURS_WKL_STATE_REMOVABLE = 4;
constexpr u32 CELL_SPURS_MAX_WORKLOAD2 = 32;
constexpr u32 CELL_SPURS_EVENT_FLAG_LAST = 3;       // ANY2ANY
constexpr u32 CELL_SPURS_EVENT_FLAG_CLEAR_LAST = 1; // MANUAL
constexpr u8 CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT = 0xFF;
constexpr u8 CELL_SPURS_EVENT_FLAG_SPU2PPU = 1, CELL_SPURS_EVENT_FLAG_ANY2ANY = 3;
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
constexpr u32 wklFlag = 0x60, flags1 = 0x74, nSpus = 0x76, wklFlagReceiver = 0x77, wklEnabled = 0xB0,
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
namespace ts_off {
constexpr u32 spurs = 0x60, args = 0x68, enable_clear_ls = 0x70, wkl_flag_wait_task = 0x72, wid = 0x74,
              size = 0x1890;
}

// sys_spu_image {type, entry_point, segs, nsegs} and CellSpurs::WorkloadInfo {addr (u64), arg, size, uniqueId, ...}.
constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0;

// Host side of every initialized SPURS instance, keyed by the guest CellSpurs address.
struct Workload {
    enum class Kind { Taskset, JobChain, Custom } kind = Kind::Custom;
    u32 arg = 0; // the taskset (Taskset) or the policy module's argument
};

struct Instance {
    u32 nspus = 0;
    bool second = false;
    std::map<u32, Workload> workloads; // by workload ID
    // SPU port -> lv2 event queue ID. On the PS3, lv2 connects these on every thread of SPURS' SPU thread group
    // (sys_spu_thread_group_connect_event_all_threads); a native SPURS has no group, so the instance keeps them, and
    // SPU code run by the native scheduler sends its events through them.
    std::map<u8, u32> spu_ports;
};

struct SpursHost {
    std::mutex mutex;
    std::map<u32, Instance> instances;
};

SpursHost& host()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSpurs called without an HLE process");
    return p->state<SpursHost>();
}

u8* guest_base(PPUContext& ctx) { return thread_of(ctx).kernel.base(); }

// ------------------------------------------------------------------------------------------------- attributes

// RPCS3 _cellSpursAttributeInitialize.
s32 _cellSpursAttributeInitialize(PPUContext& ctx, u32 attr, u32 revision, u32 sdk_version, u32 nspus, s32 spu_priority,
    s32 ppu_priority, u32 exit_if_no_work)
{
    PS_LOG_INFO(kChan, "_cellSpursAttributeInitialize(attr {:#x}, revision {}, sdk {:#x}, nSpus {}, spuPrio {}, ppuPrio {}, "
                       "exitIfNoWork {})",
        attr, revision, sdk_version, nspus, spu_priority, ppu_priority, exit_if_no_work & 0xFF);
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    std::memset(base + attr, 0, attr_off::size);
    ps_write32(base, attr + attr_off::revision, revision);
    ps_write32(base, attr + attr_off::sdkVersion, sdk_version);
    ps_write32(base, attr + attr_off::nSpus, nspus);
    ps_write32(base, attr + attr_off::spuPriority, static_cast<u32>(spu_priority));
    ps_write32(base, attr + attr_off::ppuPriority, static_cast<u32>(ppu_priority));
    ps_write8(base, attr + attr_off::exitIfNoWork, (exit_if_no_work & 0xFF) != 0);
    return lv2::CELL_OK;
}

// RPCS3 cellSpursAttributeSetNamePrefix.
s32 cellSpursAttributeSetNamePrefix(PPUContext& ctx, u32 attr, u32 prefix, u32 size)
{
    if (!attr || !prefix) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (size > CELL_SPURS_NAME_MAX_LENGTH) return error(CELL_SPURS_CORE_ERROR_INVAL);
    u8* base = guest_base(ctx);
    std::memcpy(base + attr + attr_off::prefix, base + prefix, size);
    ps_write32(base, attr + attr_off::prefixSize, size);
    PS_LOG_INFO(kChan, "cellSpursAttributeSetNamePrefix(attr {:#x}, \"{}\")", attr,
        std::string_view(reinterpret_cast<const char*>(base + prefix), size));
    return lv2::CELL_OK;
}

// RPCS3 cellSpursAttributeSetSpuThreadGroupType.
s32 cellSpursAttributeSetSpuThreadGroupType(PPUContext& ctx, u32 attr, s32 type)
{
    PS_LOG_INFO(kChan, "cellSpursAttributeSetSpuThreadGroupType(attr {:#x}, type {:#x})", attr, type);
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    u32 flags = ps_read32(base, attr + attr_off::flags);
    if (type == SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT) {
        if (flags & SAF_SPU_MEMORY_CONTAINER_SET) return error(CELL_SPURS_CORE_ERROR_STAT);
        flags |= SAF_SPU_TGT_EXCLUSIVE_NON_CONTEXT;
    } else if (type == SYS_SPU_THREAD_GROUP_TYPE_NORMAL) {
        flags &= ~SAF_SPU_TGT_EXCLUSIVE_NON_CONTEXT;
    } else {
        return error(CELL_SPURS_CORE_ERROR_INVAL);
    }
    ps_write32(base, attr + attr_off::flags, flags);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- initialize

// RPCS3 _spurs::initialize, native: see the file comment for what is not created.
s32 initialize(PPUContext& ctx, u32 spurs, u32 revision, u32 sdk_version, s32 nspus, s32 spu_priority, s32 ppu_priority,
    u32 flags, u32 prefix, u32 prefix_size)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (prefix_size > CELL_SPURS_NAME_MAX_LENGTH) return error(CELL_SPURS_CORE_ERROR_INVAL);
    // What sys_spu_thread_group_create would reject (RPCS3 sys_spu.cpp; its error is returned as is): 1..6 SPUs, and
    // a priority of 16..255 for groups that use the SPU scheduler (every type but exclusive non-context).
    const bool scheduled = (flags & SAF_SPU_TGT_EXCLUSIVE_NON_CONTEXT) == 0;
    if (nspus < 1 || nspus > 6 || (scheduled && (spu_priority < 16 || spu_priority > 255))) {
        return lv2::err(lv2::CELL_EINVAL);
    }
    if (flags & SAF_SYSTEM_WORKLOAD_ENABLED) {
        PS_LOG_ERROR(kChan, "cellSpursInitialize: the system workload (SAF_SYSTEM_WORKLOAD_ENABLED) is not implemented");
        return error(CELL_SPURS_CORE_ERROR_STAT);
    }

    const bool second = (flags & SAF_SECOND_VERSION) != 0;
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        if (h.instances.contains(spurs)) return error(CELL_SPURS_CORE_ERROR_STAT);
        h.instances[spurs] = Instance{static_cast<u32>(nspus), second, {}, {}};
    }

    u8* base = guest_base(ctx);
    std::memset(base + spurs, 0, second ? CELL_SPURS_SIZE2 : CELL_SPURS_SIZE);
    ps_write32(base, spurs + spurs_off::revision, revision);
    ps_write32(base, spurs + spurs_off::sdkVersion, sdk_version);
    ps_write64(base, spurs + spurs_off::ppu0, 0xFFFFFFFF);
    ps_write64(base, spurs + spurs_off::ppu1, 0xFFFFFFFF);
    ps_write32(base, spurs + spurs_off::flags, flags);
    ps_write8(base, spurs + spurs_off::prefixSize, static_cast<u8>(prefix_size));
    if (prefix_size) std::memcpy(base + spurs + spurs_off::prefix, base + prefix, prefix_size);
    if (!second) ps_write32(base, spurs + spurs_off::wklEnabled, 0xFFFF);
    ps_write32(base, spurs + spurs_off::sysSrvTrace, 0);
    for (u32 i = 0; i < CELL_SPURS_MAX_SPU; i++) ps_write8(base, spurs + spurs_off::sysSrvPreemptWklId + i, 0xFF);
    // The default system workload's WorkloadInfo: addr (u64), arg (u64), size, uniqueId.
    ps_write64(base, spurs + spurs_off::wklInfoSysSrv + 0x00, SPURS_IMG_ADDR_SYS_SRV_WORKLOAD);
    ps_write64(base, spurs + spurs_off::wklInfoSysSrv + 0x08, 0);
    ps_write32(base, spurs + spurs_off::wklInfoSysSrv + 0x10, 0x2200);
    ps_write8(base, spurs + spurs_off::wklInfoSysSrv + 0x14, 0xFF);
    ps_write32(base, spurs + spurs_off::unk11, 0xFFFFFFFF);
    ps_write32(base, spurs + spurs_off::unk12, 0xFFFFFFFF);
    ps_write64(base, spurs + spurs_off::unk13, 0);
    ps_write8(base, spurs + spurs_off::nSpus, static_cast<u8>(nspus));
    ps_write32(base, spurs + spurs_off::spuPriority, static_cast<u32>(spu_priority));
    // spuImg: the kernel image a real SPURS would load (RPCS3 records the same values).
    ps_write32(base, spurs + spurs_off::spuImg + 0x0, SYS_SPU_IMAGE_TYPE_USER);
    ps_write32(base, spurs + spurs_off::spuImg + 0x4, second ? CELL_SPURS_KERNEL2_ENTRY_ADDR : CELL_SPURS_KERNEL1_ENTRY_ADDR);
    ps_write8(base, spurs + spurs_off::flags1,
        static_cast<u8>(((flags & SAF_EXIT_IF_NO_WORK) ? SF1_EXIT_IF_NO_WORK : 0) | (second ? SF1_32_WORKLOADS : 0)));
    ps_write8(base, spurs + spurs_off::wklFlagReceiver, 0xFF);
    ps_write32(base, spurs + spurs_off::wklFlag, 0xFFFFFFFF);
    ps_write8(base, spurs + spurs_off::handlerDirty, 0);
    ps_write8(base, spurs + spurs_off::handlerWaiting, 0);
    ps_write8(base, spurs + spurs_off::handlerExiting, 0);
    ps_write32(base, spurs + spurs_off::ppuPriority, static_cast<u32>(ppu_priority));

    PS_LOG_INFO(kChan, "SPURS {:#010x} initialized natively: {} SPUs, SPU priority {}, PPU priority {}, flags {:#x}{}",
        spurs, nspus, spu_priority, ppu_priority, flags, second ? " (SPURS2)" : "");
    return lv2::CELL_OK;
}

s32 cellSpursInitializeWithAttribute(PPUContext& ctx, u32 spurs, u32 attr)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    const u32 revision = ps_read32(base, attr + attr_off::revision);
    if (revision > 2) return error(CELL_SPURS_CORE_ERROR_INVAL);
    const u32 flags = ps_read32(base, attr + attr_off::flags) |
        (ps_read8(base, attr + attr_off::exitIfNoWork) ? SAF_EXIT_IF_NO_WORK : 0);
    return initialize(ctx, spurs, revision, ps_read32(base, attr + attr_off::sdkVersion),
        static_cast<s32>(ps_read32(base, attr + attr_off::nSpus)), static_cast<s32>(ps_read32(base, attr + attr_off::spuPriority)),
        static_cast<s32>(ps_read32(base, attr + attr_off::ppuPriority)), flags, attr + attr_off::prefix,
        ps_read32(base, attr + attr_off::prefixSize));
}

// RPCS3's cellSpursFinalize is a TODO after its argument checks; here the host instance is released.
s32 cellSpursFinalize(PPUContext& ctx, u32 spurs)
{
    PS_LOG_INFO(kChan, "cellSpursFinalize({:#x})", spurs);
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (ps_read8(guest_base(ctx), spurs + spurs_off::handlerExiting)) return error(CELL_SPURS_CORE_ERROR_STAT);
    SpursHost& h = host();
    std::lock_guard lock(h.mutex);
    if (h.instances.erase(spurs) == 0) return error(CELL_SPURS_CORE_ERROR_STAT);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- workloads

// RPCS3 _spurs::add_workload (SPURS1 workloads 0..15): claims the first free workload ID, fills its tables in the guest
// CellSpurs, gives it the unique ID of an existing workload with the same policy module (or the lowest free one) and
// makes it runnable. The native scheduler finds the workload through the host instance.
s32 add_workload(u8* base, u32 spurs, u32& wid_out, u32 pm, u32 size, u64 data, const u8 (&priority)[8], u32 min_contention,
    u32 max_contention, u32 name_class, u32 name_instance, Workload::Kind kind)
{
    if (!spurs || !pm) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || pm % 16) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    u64 prio_bits = 0;
    for (u32 i = 0; i < 8; i++) prio_bits = (prio_bits << 8) | priority[i];
    if (min_contention == 0 || (prio_bits & 0xF0F0F0F0F0F0F0F0ull)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);

    SpursHost& h = host();
    std::lock_guard lock(h.mutex);
    const auto it = h.instances.find(spurs);
    if (it == h.instances.end()) {
        PS_LOG_ERROR(kChan, "add_workload: SPURS {:#010x} is not initialized", spurs);
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    Instance& inst = it->second;
    if (inst.second) {
        PS_LOG_ERROR(kChan, "add_workload: SPURS2 (32 workloads) is not implemented");
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }

    const u32 enabled = ps_read32(base, spurs + wkl_off::enabled);
    const u32 wid = static_cast<u32>(std::countl_one(enabled));
    wid_out = wid;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return error(CELL_SPURS_POLICY_MODULE_ERROR_AGAIN);
    ps_write32(base, spurs + wkl_off::enabled, enabled | (0x80000000u >> wid));
    // RPCS3 ensure()s a free workload slot has no SPUs on it or switching to it.
    if ((ps_read8(base, spurs + wkl_off::currentContention + wid) & 0xF) ||
        (ps_read8(base, spurs + wkl_off::pendingContention + wid) & 0xF)) {
        PS_FATAL(kChan, "add_workload: free workload {} of SPURS {:#010x} still has contention", wid, spurs);
    }

    ps_write8(base, spurs + wkl_off::state1 + wid, SPURS_WKL_STATE_PREPARING);
    ps_write8(base, spurs + wkl_off::status1 + wid, 0);
    ps_write8(base, spurs + wkl_off::event1 + wid, 0);
    const u32 info = spurs + wkl_off::info1 + wid * wkl_off::info_stride;
    ps_write64(base, info + wkl_off::info_addr, pm);
    ps_write64(base, info + wkl_off::info_arg, data);
    ps_write32(base, info + wkl_off::info_size, size);
    for (u32 i = 0; i < 8; i++) ps_write8(base, info + wkl_off::info_priority + i, priority[i]);
    const u32 names = spurs + wkl_off::h1 + wid * wkl_off::h_stride;
    ps_write64(base, names, name_class);
    ps_write64(base, names + 8, name_instance);
    const u32 f = spurs + wkl_off::f1 + wid * wkl_off::f1_stride;
    std::memset(base + f, 0, 0x20);                 // keeps the semaphore at 0x20
    std::memset(base + f + 0x28, 0, 0x58);
    ps_write8(base, spurs + wkl_off::idleSpuCountOrReadyCount2 + wid, 0);
    ps_write8(base, spurs + wkl_off::minContention + wid, static_cast<u8>(std::min<u32>(min_contention, 8)));
    ps_write8(base, spurs + wkl_off::readyCount1 + wid, 0);
    const u8 maxc = ps_read8(base, spurs + wkl_off::maxContention + wid);
    ps_write8(base, spurs + wkl_off::maxContention + wid, static_cast<u8>((maxc & 0xF0) | std::min<u32>(max_contention, 8)));
    ps_write16(base, spurs + wkl_off::signal1, static_cast<u16>(ps_read16(base, spurs + wkl_off::signal1) & ~(0x8000u >> wid)));
    if (ps_read8(base, spurs + spurs_off::wklFlagReceiver) == wid) ps_write8(base, spurs + spurs_off::wklFlagReceiver, 0xFF);

    // Unique ID: shared with an enabled workload using the same policy module, else the lowest unused one.
    const u32 mask = ps_read32(base, spurs + wkl_off::mskB) & ~(0x80000000u >> wid);
    u32 unique = 0, used = 0;
    for (u32 i = 0, m = 0x80000000u; i < 32; i++, m >>= 1) {
        if (!(mask & m) || i >= CELL_SPURS_MAX_WORKLOAD) continue;
        const u32 other = spurs + wkl_off::info1 + i * wkl_off::info_stride;
        if (ps_read64(base, other + wkl_off::info_addr) == pm) {
            unique = ps_read8(base, other + wkl_off::info_unique);
            break;
        }
        used |= 0x80000000u >> ps_read8(base, other + wkl_off::info_unique);
        unique = static_cast<u32>(std::countl_one(used));
    }
    ps_write8(base, info + wkl_off::info_unique, static_cast<u8>(unique));
    ps_write32(base, spurs + wkl_off::mskB, mask | (0x80000000u >> wid));
    ps_write8(base, spurs + wkl_off::state1 + wid, SPURS_WKL_STATE_RUNNABLE);
    ps_write8(base, spurs + wkl_off::sysSrvMsgUpdateWorkload, 0xFF);
    ps_write8(base, spurs + wkl_off::sysSrvMessage, 0xFF);

    inst.workloads[wid] = Workload{kind, static_cast<u32>(data)};
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- tasksets

// RPCS3 _cellSpursTasksetAttributeInitialize.
s32 _cellSpursTasksetAttributeInitialize(PPUContext& ctx, u32 attr, u32 revision, u32 sdk_version, u64 args, u32 priority,
    u32 max_contention)
{
    if (!attr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    for (u32 i = 0; i < 8; i++)
        if (ps_read8(base, priority + i) > 0xF) return error(CELL_SPURS_TASK_ERROR_INVAL);
    u8 prio[8];
    std::memcpy(prio, base + priority, 8);
    std::memset(base + attr, 0, tsattr_off::size);
    ps_write32(base, attr + tsattr_off::revision, revision);
    ps_write32(base, attr + tsattr_off::sdk_version, sdk_version);
    ps_write64(base, attr + tsattr_off::args, args);
    std::memcpy(base + attr + tsattr_off::priority, prio, 8);
    ps_write32(base, attr + tsattr_off::taskset_size, kTasksetSize);
    ps_write32(base, attr + tsattr_off::max_contention, max_contention);
    return lv2::CELL_OK;
}

// RPCS3 cellSpursTasksetAttributeSetName: stores the guest pointer.
s32 cellSpursTasksetAttributeSetName(PPUContext& ctx, u32 attr, u32 name)
{
    if (!attr || !name) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    ps_write32(guest_base(ctx), attr + tsattr_off::name, name);
    return lv2::CELL_OK;
}

// RPCS3 _spurs::create_taskset: clears the taskset, links it to the SPURS instance and adds it as a workload whose
// policy module is the taskset marker, with minimum contention 8 and the attribute's priorities and maximum.
s32 create_taskset(u8* base, u32 spurs, u32 taskset, u64 args, const u8 (&priority)[8], u32 max_contention, u32 name,
    u32 size, s32 enable_clear_ls)
{
    if (!spurs || !taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (size != kTasksetSize) {
        PS_LOG_ERROR(kChan, "create_taskset: taskset size {:#x} (CellSpursTaskset2) is not implemented", size);
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    std::memset(base + taskset, 0, size);
    ps_write64(base, taskset + ts_off::spurs, spurs);
    ps_write64(base, taskset + ts_off::args, args);
    ps_write8(base, taskset + ts_off::enable_clear_ls, enable_clear_ls > 0 ? 1 : 0);
    ps_write32(base, taskset + ts_off::size, size);

    u32 wid = 0;
    if (const s32 r = add_workload(base, spurs, wid, SPURS_IMG_ADDR_TASKSET_PM, kTasksetPmSize, taskset, priority, 8,
            max_contention, 0, name, Workload::Kind::Taskset);
        r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "create_taskset({:#x}): adding the workload failed ({:#x})", taskset, static_cast<u32>(r));
        return r;
    }
    ps_write8(base, taskset + ts_off::wkl_flag_wait_task, 0x80);
    ps_write32(base, taskset + ts_off::wid, wid);
    std::string label;
    if (name) label = reinterpret_cast<const char*>(base + name);
    PS_LOG_INFO(kChan, "taskset {:#010x} \"{}\" is workload {} of SPURS {:#010x} (max contention {})", taskset, label, wid,
        spurs, max_contention);
    return lv2::CELL_OK;
}

s32 cellSpursCreateTasksetWithAttribute(PPUContext& ctx, u32 spurs, u32 taskset, u32 attr)
{
    if (!attr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (ps_read32(base, attr + tsattr_off::revision) != CELL_SPURS_TASKSET_ATTRIBUTE_REVISION) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    u8 prio[8];
    std::memcpy(prio, base + attr + tsattr_off::priority, 8);
    return create_taskset(base, spurs, taskset, ps_read64(base, attr + tsattr_off::args), prio,
        ps_read32(base, attr + tsattr_off::max_contention), ps_read32(base, attr + tsattr_off::name),
        ps_read32(base, attr + tsattr_off::taskset_size), static_cast<s32>(ps_read32(base, attr + tsattr_off::enable_clear_ls)));
}

// RPCS3 cellSpursShutdownWorkload. Natively no SPU runs a workload yet (status 0), so a shutdown makes it removable
// at once. The shutdown-completion notification RPCS3 sends to the SPURS event port (when a hook is registered) has
// no handler thread to go to natively; it is logged instead.
s32 shutdown_workload(u8* base, u32 spurs, u32 wid)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    const bool second = (ps_read8(base, spurs + spurs_off::flags1) & SF1_32_WORKLOADS) != 0;
    if (wid >= (second ? CELL_SPURS_MAX_WORKLOAD2 : CELL_SPURS_MAX_WORKLOAD)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) {
        PS_LOG_ERROR(kChan, "shutdown_workload: SPURS2 workload {} is not implemented", wid);
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    SpursHost& h = host();
    std::lock_guard lock(h.mutex);
    const u8 state = ps_read8(base, spurs + wkl_off::state1 + wid);
    if (state <= SPURS_WKL_STATE_PREPARING) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    if (state == SPURS_WKL_STATE_SHUTTING_DOWN || state == SPURS_WKL_STATE_REMOVABLE) return lv2::CELL_OK;
    const u8 status = ps_read8(base, spurs + wkl_off::status1 + wid);
    if (status) {
        ps_write8(base, spurs + wkl_off::state1 + wid, SPURS_WKL_STATE_SHUTTING_DOWN);
        ps_write8(base, spurs + wkl_off::sysSrvMsgUpdateWorkload, 0xFF);
        ps_write8(base, spurs + wkl_off::sysSrvMessage, 0xFF);
        return lv2::CELL_OK;
    }
    ps_write8(base, spurs + wkl_off::state1 + wid, SPURS_WKL_STATE_REMOVABLE);
    const u8 event = ps_read8(base, spurs + wkl_off::event1 + wid);
    ps_write8(base, spurs + wkl_off::event1 + wid, static_cast<u8>(event | 1));
    if ((event & 0x12) && !(event & 1)) {
        PS_LOG_WARN(kChan, "workload {} of SPURS {:#010x} shut down: its completion hook is not called (not implemented)",
            wid, spurs);
    }
    return lv2::CELL_OK;
}

// RPCS3 cellSpursShutdownTaskset.
s32 cellSpursShutdownTaskset(PPUContext& ctx, u32 taskset)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    const u32 wid = ps_read32(base, taskset + ts_off::wid);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return error(CELL_SPURS_TASK_ERROR_INVAL);
    const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
    s32 r = shutdown_workload(base, spurs, wid);
    if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT)) r = error(CELL_SPURS_TASK_ERROR_STAT);
    PS_LOG_INFO(kChan, "cellSpursShutdownTaskset({:#010x}): workload {} -> {:#x}", taskset, wid, static_cast<u32>(r));
    return r;
}

// ------------------------------------------------------------------------------------------------- lv2 event queues

constexpr u32 kSpursSpuPortBits = 0xDA8; // CellSpurs::spuPortBits (u64): ports taken by cellSpursAttachLv2EventQueue

// Sets (or clears) bit `port` of CellSpurs::spuPortBits; returns whether it was set before.
bool update_spu_port_bit(u8* base, u32 spurs, u8 port, bool set)
{
    const u64 bit = u64{1} << port;
    for (;;) {
        const u64 old = ps_read64(base, spurs + kSpursSpuPortBits);
        if (atomic_cas_be64(base, spurs + kSpursSpuPortBits, old, set ? old | bit : old & ~bit)) return old & bit;
    }
}

// RPCS3 _spurs::attach_lv2_eq. lv2 would connect `queue` to one SPU port of every thread in SPURS' SPU thread group
// (sys_spu_thread_group_connect_event_all_threads): a given port (0..0x3F; 0..0xF from SDK 1.8) or, dynamic, the
// lowest free one of 0x10..0x3F. Natively the SPURS instance records the connection. A port whose queue has been
// destroyed is free again (RPCS3 lv2_obj::check). `spurs_created`: a queue SPURS makes for itself (event flags,
// queues), which spuPortBits does not show.
s32 attach_lv2_eq(PPUContext& ctx, u32 spurs, u32 queue, u8& port, bool dynamic, bool spurs_created)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_CORE_ERROR_STAT);
    u32 first = 0x10, last = 0x3F;
    if (!dynamic) {
        if (port > 0x3F) return error(CELL_SPURS_CORE_ERROR_INVAL);
        if (t.kernel.config().sdk_version >= 0x180000 && port > 0xF) return error(CELL_SPURS_CORE_ERROR_PERM);
        first = last = port;
    }
    if (!t.kernel.event_queue(queue)) return lv2::err(lv2::CELL_ESRCH);

    SpursHost& h = host();
    std::lock_guard lock(h.mutex);
    const auto it = h.instances.find(spurs);
    if (it == h.instances.end()) return lv2::err(lv2::CELL_ESRCH); // no SPU thread group: not initialized
    auto& ports = it->second.spu_ports;
    u32 p = first;
    for (; p <= last; p++) {
        const auto c = ports.find(static_cast<u8>(p));
        if (c == ports.end() || !t.kernel.event_queue(c->second)) break;
    }
    if (p > last) return error(CELL_SPURS_CORE_ERROR_BUSY); // lv2 CELL_EISCONN
    port = static_cast<u8>(p);
    ports[port] = queue;
    if (!spurs_created) update_spu_port_bit(base, spurs, port, true);
    return lv2::CELL_OK;
}

// RPCS3 _spurs::detach_lv2_eq. Deviation: the port's connection is removed as well (RPCS3 leaves the SPU threads'
// port connected, so a detached port can't be attached again until its queue is destroyed).
s32 detach_lv2_eq(u8* base, u32 spurs, u8 port, bool spurs_created, u32 sdk_version)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (!spurs_created && ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_CORE_ERROR_STAT);
    if (port > 0x3F) return error(CELL_SPURS_CORE_ERROR_INVAL);
    if (!spurs_created && !update_spu_port_bit(base, spurs, port, false) && sdk_version >= 0x180000) {
        return error(CELL_SPURS_CORE_ERROR_SRCH);
    }
    SpursHost& h = host();
    std::lock_guard lock(h.mutex);
    if (const auto it = h.instances.find(spurs); it != h.instances.end()) it->second.spu_ports.erase(port);
    return lv2::CELL_OK;
}

s32 cellSpursAttachLv2EventQueue(PPUContext& ctx, u32 spurs, u32 queue, u32 port_addr, s32 dynamic)
{
    if (!spurs || !port_addr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    u8* base = guest_base(ctx);
    u8 port = dynamic ? 0 : ps_read8(base, port_addr);
    const s32 rc = attach_lv2_eq(ctx, spurs, queue, port, dynamic != 0, false);
    if (rc == lv2::CELL_OK) {
        ps_write8(base, port_addr, port);
        PS_LOG_INFO(kChan, "cellSpursAttachLv2EventQueue: queue {:#x} on SPU port {:#x} of SPURS {:#x}", queue, port,
            spurs);
    }
    return rc;
}

s32 cellSpursDetachLv2EventQueue(PPUContext& ctx, u32 spurs, u8 port)
{
    return detach_lv2_eq(guest_base(ctx), spurs, port, false, thread_of(ctx).kernel.config().sdk_version);
}

// ------------------------------------------------------------------------------------------------- event flags

// CellSpursEventFlag (128 bytes, 128-aligned): ctrl (8 bytes), spuTaskUsedWaitSlots, spuTaskWaitMode, spuPort, isIwl,
// direction, clearMode, ..., addr (u64) at 0x70.
// ctrl: events (u16), spuTaskPendingRecv (u16), ppuWaitMask (u16), ppuWaitSlotAndMode (u8: slot << 4 | mode),
// ppuPendingRecv (u8). Wait slot i is bit 15 - i of spuTaskUsedWaitSlots, spuTaskPendingRecv and spuTaskWaitMode.
namespace ef_off {
constexpr u32 events = 0x00, spuTaskPendingRecv = 0x02, ppuWaitMask = 0x04, ppuWaitSlotAndMode = 0x06,
              ppuPendingRecv = 0x07, spuTaskUsedWaitSlots = 0x08, spuTaskWaitMode = 0x0A, spuPort = 0x0C,
              isIwl = 0x0D, direction = 0x0E, clearMode = 0x0F, spuTaskWaitMask = 0x10,
              pendingRecvTaskEvents = 0x30, addr = 0x70, eventPortId = 0x78, eventQueueId = 0x7C, size = 0x80;
}

// RPCS3 _cellSpursEventFlagInitialize.
s32 _cellSpursEventFlagInitialize(PPUContext& ctx, u32 spurs, u32 taskset, u32 event_flag, u32 clear_mode, u32 direction)
{
    if ((!taskset && !spurs) || !event_flag) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || taskset % 128 || event_flag % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (taskset && ps_read32(base, taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
    if (direction > CELL_SPURS_EVENT_FLAG_LAST || clear_mode > CELL_SPURS_EVENT_FLAG_CLEAR_LAST) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    std::memset(base + event_flag, 0, ef_off::size);
    ps_write8(base, event_flag + ef_off::direction, static_cast<u8>(direction));
    ps_write8(base, event_flag + ef_off::clearMode, static_cast<u8>(clear_mode));
    ps_write8(base, event_flag + ef_off::spuPort, CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT);
    if (taskset) {
        ps_write64(base, event_flag + ef_off::addr, taskset);
    } else {
        ps_write8(base, event_flag + ef_off::isIwl, 1);
        ps_write64(base, event_flag + ef_off::addr, spurs);
    }
    return lv2::CELL_OK;
}

// The CellSpurs an event flag belongs to: its `addr` when made for SPURS itself (isIwl), else the taskset's.
u32 event_flag_spurs(u8* base, u32 event_flag)
{
    const u32 addr = static_cast<u32>(ps_read64(base, event_flag + ef_off::addr));
    if (ps_read8(base, event_flag + ef_off::isIwl) == 1) return addr;
    return static_cast<u32>(ps_read64(base, addr + ts_off::spurs));
}

bool event_flag_to_ppu(u8 direction)
{
    return direction == CELL_SPURS_EVENT_FLAG_SPU2PPU || direction == CELL_SPURS_EVENT_FLAG_ANY2ANY;
}

// An lv2 error from a step of an event flag operation as a SPURS task error (RPCS3: SPURS codes pass unchanged).
s32 task_error(s32 rc)
{
    const u32 u = static_cast<u32>(rc);
    return (u & 0x0FFF0000) == 0x00410000 ? rc : error(0x80410900 | (u & 0xFF));
}

// RPCS3 cellSpursEventFlagAttachLv2EventQueue and _spurs::create_lv2_eq: a one-event PPU queue ("_spuEvF") the PPU
// waiter blocks on, connected to a dynamic SPU port that SPU tasks setting the flag send to; for an ANY2ANY flag also
// a local event port to it, through which cellSpursEventFlagSet on the PPU wakes the waiter. A misaligned flag gives
// TASK_ERROR_AGAIN, as in RPCS3. Deviation: a failed port attach is returned (RPCS3's create_lv2_eq destroys the
// queue and returns CELL_OK).
s32 cellSpursEventFlagAttachLv2EventQueue(PPUContext& ctx, u32 event_flag)
{
    if (!event_flag) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (event_flag % 128) return error(CELL_SPURS_TASK_ERROR_AGAIN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u8 direction = ps_read8(base, event_flag + ef_off::direction);
    if (!event_flag_to_ppu(direction)) return error(CELL_SPURS_TASK_ERROR_PERM);
    if (ps_read8(base, event_flag + ef_off::spuPort) != CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT) {
        return error(CELL_SPURS_TASK_ERROR_STAT);
    }
    const u32 spurs = event_flag_spurs(base, event_flag);

    StackVar vars(ctx, base, 24); // queue ID, port ID, sys_event_queue_attribute_t {protocol, type, name[8]}
    const u32 queue_out = vars.addr(), port_out = vars.addr() + 4, attr = vars.addr() + 8;
    ps_write32(base, attr, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr + 4, lv2::SYS_PPU_QUEUE);
    std::memcpy(base + attr + 8, "_spuEvF", 8);
    if (const s32 rc = lv2::sys_event_queue_create(t, queue_out, attr, 0, 1); rc != lv2::CELL_OK) {
        return task_error(rc);
    }
    const u32 queue = ps_read32(base, queue_out);
    u8 port = 0;
    if (const s32 rc = attach_lv2_eq(ctx, spurs, queue, port, true, true); rc != lv2::CELL_OK) {
        lv2::sys_event_queue_destroy(t, queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
        return task_error(rc);
    }
    if (direction == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
        s32 rc = lv2::sys_event_port_create(t, port_out, lv2::SYS_EVENT_PORT_LOCAL, 0);
        if (rc == lv2::CELL_OK) {
            const u32 eport = ps_read32(base, port_out);
            rc = lv2::sys_event_port_connect_local(t, eport, queue);
            if (rc == lv2::CELL_OK) {
                ps_write32(base, event_flag + ef_off::eventPortId, eport);
            } else {
                lv2::sys_event_port_destroy(t, eport);
            }
        }
        if (rc != lv2::CELL_OK) {
            if (detach_lv2_eq(base, spurs, port, true, t.kernel.config().sdk_version) == lv2::CELL_OK) {
                lv2::sys_event_queue_destroy(t, queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
            }
            return task_error(rc);
        }
    }
    ps_write32(base, event_flag + ef_off::eventQueueId, queue);
    ps_write8(base, event_flag + ef_off::spuPort, port);
    PS_LOG_INFO(kChan, "cellSpursEventFlagAttachLv2EventQueue: event flag {:#x} -> queue {:#x}, SPU port {:#x}",
        event_flag, queue, port);
    return lv2::CELL_OK;
}

// RPCS3 cellSpursEventFlagDetachLv2EventQueue.
s32 cellSpursEventFlagDetachLv2EventQueue(PPUContext& ctx, u32 event_flag)
{
    if (!event_flag) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (event_flag % 128) return error(CELL_SPURS_TASK_ERROR_AGAIN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u8 direction = ps_read8(base, event_flag + ef_off::direction);
    if (!event_flag_to_ppu(direction)) return error(CELL_SPURS_TASK_ERROR_PERM);
    const u8 port = ps_read8(base, event_flag + ef_off::spuPort);
    if (port == CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT) return error(CELL_SPURS_TASK_ERROR_STAT);
    if (ps_read16(base, event_flag + ef_off::ppuWaitMask) || ps_read8(base, event_flag + ef_off::ppuPendingRecv)) {
        return error(CELL_SPURS_TASK_ERROR_BUSY);
    }
    ps_write8(base, event_flag + ef_off::spuPort, CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT);
    const u32 spurs = event_flag_spurs(base, event_flag);
    if (direction == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
        const u32 eport = ps_read32(base, event_flag + ef_off::eventPortId);
        lv2::sys_event_port_disconnect(t, eport);
        lv2::sys_event_port_destroy(t, eport);
    }
    if (detach_lv2_eq(base, spurs, port, true, t.kernel.config().sdk_version) == lv2::CELL_OK) {
        lv2::sys_event_queue_destroy(t, ps_read32(base, event_flag + ef_off::eventQueueId),
            lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
    }
    return lv2::CELL_OK;
}

// RPCS3 cellSpursEventFlagSet, as one reservation operation on the flag's 128-byte line (SPU tasks update it with
// GETLLAR/PUTLLC). Wakes the waiting PPU thread of an ANY2ANY flag through its event port. Waking SPU tasks waiting on
// the flag needs the native taskset policy (D-037 stage 3b); until then that is fatal, never silently skipped.
s32 cellSpursEventFlagSet(PPUContext& ctx, u32 event_flag, u16 bits)
{
    if (!event_flag) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (event_flag % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!event_flag_to_ppu(ps_read8(base, event_flag + ef_off::direction))) return error(CELL_SPURS_TASK_ERROR_PERM);

    bool send = false;
    u8 ppu_slot = 0;
    u16 ppu_events = 0, pending_recv = 0;
    reservation_op(base, event_flag, [&](u8* ef) {
        send = false;
        ppu_slot = 0;
        ppu_events = 0;
        pending_recv = 0;
        const u16 events = ps_read16(ef, ef_off::events) | bits;
        u16 to_clear = 0;
        const u16 ppu_mask = ps_read16(ef, ef_off::ppuWaitMask);
        const u8 slot_mode = ps_read8(ef, ef_off::ppuWaitSlotAndMode);
        if (ps_read8(ef, ef_off::direction) == CELL_SPURS_EVENT_FLAG_ANY2ANY && ppu_mask) {
            const u16 relevant = events & ppu_mask;
            if ((ppu_mask & ~relevant) == 0 || ((slot_mode & 0x0F) == CELL_SPURS_EVENT_FLAG_OR && relevant)) {
                ps_write8(ef, ef_off::ppuPendingRecv, 1);
                ps_write16(ef, ef_off::ppuWaitMask, 0);
                ppu_events = relevant;
                to_clear = relevant;
                ppu_slot = slot_mode >> 4;
                send = true;
            }
        }
        const u16 task_pending = ps_read16(ef, ef_off::spuTaskPendingRecv);
        const u16 wait_mode = ps_read16(ef, ef_off::spuTaskWaitMode);
        const u16 waiting = ps_read16(ef, ef_off::spuTaskUsedWaitSlots) & ~task_pending;
        for (u32 slot = 0; slot < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; slot++) {
            const u16 bit = static_cast<u16>(0x8000 >> slot);
            if (!(waiting & bit)) continue;
            const u16 mask = ps_read16(ef, ef_off::spuTaskWaitMask + 2 * slot);
            const u16 relevant = events & mask;
            if ((mask & ~relevant) == 0 || (!(wait_mode & bit) && relevant)) { // wait mode bit clear: OR
                to_clear |= relevant;
                pending_recv |= bit;
            }
        }
        const bool auto_clear = ps_read8(ef, ef_off::clearMode) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO;
        ps_write16(ef, ef_off::events, auto_clear ? events & ~to_clear : events);
        ps_write16(ef, ef_off::spuTaskPendingRecv, task_pending | pending_recv);
        return true;
    });
    if (pending_recv) {
        PS_FATAL(kChan, "cellSpursEventFlagSet({:#x}, {:#06x}) releases SPU tasks (wait slots {:#06x}): waking SPU "
            "tasks needs the native taskset policy", event_flag, bits, pending_recv);
    }
    if (send) {
        ps_write16(base, event_flag + ef_off::pendingRecvTaskEvents + 2 * ppu_slot, ppu_events);
        if (const s32 rc = lv2::sys_event_port_send(t, ps_read32(base, event_flag + ef_off::eventPortId), 0, 0, 0);
            rc != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "cellSpursEventFlagSet({:#x}): waking the PPU waiter failed: {:#x}", event_flag,
                static_cast<u32>(rc));
            return task_error(rc);
        }
    }
    return lv2::CELL_OK;
}

// RPCS3 _spurs::event_flag_wait (cellSpursEventFlagWait and cellSpursEventFlagTryWait). The PPU thread records its
// mask and mode in the flag and blocks on the flag's lv2 queue until a setter (PPU, or SPU through the SPU port)
// meets its condition. Fix: a thread that blocked returns the bits that woke it (RPCS3 then overwrites the mask
// with an uninitialized value).
s32 event_flag_wait(PPUContext& ctx, u32 event_flag, u32 mask_addr, u32 mode, bool block)
{
    if (!event_flag || !mask_addr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (event_flag % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (mode > CELL_SPURS_EVENT_FLAG_AND) return error(CELL_SPURS_TASK_ERROR_INVAL);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u8 direction = ps_read8(base, event_flag + ef_off::direction);
    if (!event_flag_to_ppu(direction)) return error(CELL_SPURS_TASK_ERROR_PERM);
    if (block && ps_read8(base, event_flag + ef_off::spuPort) == CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT) {
        return error(CELL_SPURS_TASK_ERROR_STAT);
    }
    if (ps_read16(base, event_flag + ef_off::ppuWaitMask) || ps_read8(base, event_flag + ef_off::ppuPendingRecv)) {
        return error(CELL_SPURS_TASK_ERROR_BUSY);
    }
    const u16 mask = ps_read16(base, mask_addr);
    s32 rc = lv2::CELL_OK;
    bool recv = false;
    u16 received = 0;
    reservation_op(base, event_flag, [&](u8* ef) {
        rc = lv2::CELL_OK;
        recv = false;
        received = 0;
        const u16 events = ps_read16(ef, ef_off::events);
        const u16 relevant = events & mask;
        const u16 used = ps_read16(ef, ef_off::spuTaskUsedWaitSlots);
        if (direction == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
            // The mask must not conflict with an SPU task still waiting: OR never conflicts with OR, OR and AND
            // conflict when the masks overlap, AND and AND when they differ.
            u16 waiting = used & ~ps_read16(ef, ef_off::spuTaskPendingRecv);
            if (mode == CELL_SPURS_EVENT_FLAG_OR) waiting &= ps_read16(ef, ef_off::spuTaskWaitMode);
            for (u32 slot = 0; slot < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; slot++) {
                if (!(waiting & (0x8000 >> slot))) continue;
                const u16 other = ps_read16(ef, ef_off::spuTaskWaitMask + 2 * slot);
                if ((other & mask) && other != mask) {
                    rc = error(CELL_SPURS_TASK_ERROR_AGAIN);
                    return false;
                }
            }
        }
        if ((mask & ~relevant) == 0 || (mode == CELL_SPURS_EVENT_FLAG_OR && relevant)) {
            if (ps_read8(ef, ef_off::clearMode) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) {
                ps_write16(ef, ef_off::events, events & ~relevant);
            }
            received = relevant;
            return true;
        }
        if (!block) {
            rc = error(CELL_SPURS_TASK_ERROR_BUSY);
            return false;
        }
        u8 slot_mode = 0;
        if (direction == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
            const u32 free_slot = static_cast<u32>(std::countr_one(used)); // lowest clear bit
            if (free_slot >= CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS) {
                rc = error(CELL_SPURS_TASK_ERROR_BUSY);
                return false;
            }
            slot_mode = static_cast<u8>((CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS - 1 - free_slot) << 4);
        }
        ps_write8(ef, ef_off::ppuWaitSlotAndMode, static_cast<u8>(slot_mode | mode));
        ps_write16(ef, ef_off::ppuWaitMask, mask);
        recv = true;
        return true;
    });
    if (rc != lv2::CELL_OK) return rc;
    if (recv) {
        const u32 queue = ps_read32(base, event_flag + ef_off::eventQueueId);
        if (const s32 r = lv2::sys_event_queue_receive(t, queue, 0, 0); r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "cellSpursEventFlagWait({:#x}): receive on queue {:#x} failed: {:#x}", event_flag, queue,
                static_cast<u32>(r));
            return task_error(r);
        }
        const u32 slot = direction == CELL_SPURS_EVENT_FLAG_ANY2ANY
            ? ps_read8(base, event_flag + ef_off::ppuWaitSlotAndMode) >> 4 : 0;
        received = ps_read16(base, event_flag + ef_off::pendingRecvTaskEvents + 2 * slot);
        reservation_op(base, event_flag, [](u8* ef) {
            ps_write8(ef, ef_off::ppuPendingRecv, 0);
            return true;
        });
    }
    ps_write16(base, mask_addr, received);
    return lv2::CELL_OK;
}

s32 cellSpursEventFlagWait(PPUContext& ctx, u32 event_flag, u32 mask, u32 mode)
{
    return event_flag_wait(ctx, event_flag, mask, mode, true);
}

// ------------------------------------------------------------------------------------------------- barriers

// RPCS3 cellSpursBarrierInitialize. CellSpursBarrier (128 bytes, 128-aligned): zero, remained (u32), taskset at 0x34.
s32 cellSpursBarrierInitialize(PPUContext& ctx, u32 taskset, u32 barrier, u32 total)
{
    if (!taskset || !barrier) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128 || barrier % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (!total || total > 128) return error(CELL_SPURS_TASK_ERROR_INVAL);
    u8* base = guest_base(ctx);
    if (ps_read32(base, taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
    std::memset(base + barrier, 0, 128);
    ps_write32(base, barrier + 0x04, total);
    ps_write32(base, barrier + 0x34, taskset);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- job chains

// CellSpursJobChainAttribute (8-aligned) and CellSpursJobChain (0x100 bytes, 128-aligned).
namespace jcattr_off {
constexpr u32 jmVer = 0x00, sdkVer = 0x04, jobChainEntry = 0x08, sizeJobDescriptor = 0x0C, maxGrabbedJob = 0x0E,
              priorities = 0x10, maxContention = 0x18, autoSpuCount = 0x1C, tag1 = 0x20, tag2 = 0x24,
              isFixedMemAlloc = 0x28, maxSizeJobDescriptor = 0x2C, initSpuCount = 0x30, haltOnError = 0x34,
              name = 0x38, jobMemoryCheck = 0x3C;
}
namespace jc_off {
constexpr u32 pc = 0x00, isHalted = 0x23, initSpuCount = 0x28, tag1 = 0x2A, tag2 = 0x2B, val2C = 0x2C, jmVer = 0x2D,
              val2F = 0x2F, maxGrabbedJob = 0x70, workloadId = 0x74, spurs = 0x78, error = 0x80, cause = 0x88,
              sdkVer = 0x90, jobMemoryCheck = 0x94;
}

// RPCS3 _spurs::check_job_chain_attribute.
s32 check_job_chain_attribute(u32 sdk_ver, u32 entry, u16 size_job, u16 max_grabbed, u64 priorities, bool auto_spu_count,
    u32 tag1, u32 tag2, u32 max_size_job, u32 init_spu_count)
{
    if (!entry) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (entry % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!max_grabbed || max_grabbed > 0x10) return error(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 max_tag = sdk_ver < 0x19000 ? 31 : 30; // tag 31 is refused from SDK 1.90 on
    if (tag1 > max_tag || tag2 > max_tag) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (priorities & 0xF0F0F0F0F0F0F0F0ull) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (size_job % 0x80 && size_job != 64) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (auto_spu_count && init_spu_count > 0xFF) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (max_size_job <= 0xFF || max_size_job > 0x400 || max_size_job % 0x80) return error(CELL_SPURS_JOB_ERROR_INVAL);
    return lv2::CELL_OK;
}

u64 read_priorities(u8* base, u32 addr)
{
    u64 v = 0;
    for (u32 i = 0; i < 8; i++) v = (v << 8) | ps_read8(base, addr + i);
    return v;
}

// RPCS3 _cellSpursJobChainAttributeInitialize (14 arguments; the last six come from the caller's stack).
s32 _cellSpursJobChainAttributeInitialize(PPUContext& ctx, u32 jm_revision, u32 sdk_revision, u32 attr, u32 entry,
    u16 size_job, u16 max_grabbed, u32 priority_table, u32 max_contention, bool auto_spu_count, u32 tag1, u32 tag2,
    bool fixed_mem_alloc, u32 max_size_job, u32 init_spu_count)
{
    PS_LOG_INFO(kChan, "_cellSpursJobChainAttributeInitialize(jm {}, sdk {:#x}, attr {:#x}, entry {:#x}, sizeJob {:#x}, "
                       "maxGrabbed {}, maxContention {}, autoSpuCount {}, tags {}/{}, fixedMem {}, maxSizeJob {:#x}, "
                       "initSpuCount {})",
        jm_revision, sdk_revision, attr, entry, size_job, max_grabbed, max_contention, auto_spu_count, tag1, tag2,
        fixed_mem_alloc, max_size_job, init_spu_count);
    if (!attr) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    const u64 prio = read_priorities(base, priority_table);
    if (const s32 r = check_job_chain_attribute(sdk_revision, entry, size_job, max_grabbed, prio, auto_spu_count, tag1,
            tag2, max_size_job, init_spu_count);
        r != lv2::CELL_OK) {
        return r;
    }
    ps_write32(base, attr + jcattr_off::jmVer, jm_revision);
    ps_write32(base, attr + jcattr_off::sdkVer, sdk_revision);
    ps_write32(base, attr + jcattr_off::jobChainEntry, entry);
    ps_write16(base, attr + jcattr_off::sizeJobDescriptor, size_job);
    ps_write16(base, attr + jcattr_off::maxGrabbedJob, max_grabbed);
    ps_write64(base, attr + jcattr_off::priorities, prio);
    ps_write32(base, attr + jcattr_off::maxContention, max_contention);
    ps_write8(base, attr + jcattr_off::autoSpuCount, auto_spu_count);
    ps_write32(base, attr + jcattr_off::tag1, tag1);
    ps_write32(base, attr + jcattr_off::tag2, tag2);
    ps_write8(base, attr + jcattr_off::isFixedMemAlloc, fixed_mem_alloc);
    ps_write32(base, attr + jcattr_off::maxSizeJobDescriptor, max_size_job);
    ps_write32(base, attr + jcattr_off::initSpuCount, init_spu_count);
    ps_write32(base, attr + jcattr_off::haltOnError, 0);
    ps_write32(base, attr + jcattr_off::name, 0);
    ps_write8(base, attr + jcattr_off::jobMemoryCheck, 0);
    return lv2::CELL_OK;
}

s32 cellSpursJobChainAttributeSetName(PPUContext& ctx, u32 attr, u32 name)
{
    if (!attr || !name) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    ps_write32(guest_base(ctx), attr + jcattr_off::name, name);
    return lv2::CELL_OK;
}

s32 cellSpursJobChainAttributeSetHaltOnError(PPUContext& ctx, u32 attr)
{
    if (!attr) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    ps_write32(guest_base(ctx), attr + jcattr_off::haltOnError, 1);
    return lv2::CELL_OK;
}

// RPCS3 cellSpursCreateJobChainWithAttribute + _spurs::create_job_chain, with the native job chain marker as the
// policy module. Deviations: no "JobChain" class name (RPCS3 points it at a temporary guest string), and RPCS3's
// one-title firmware quirk (val2F) is not reproduced (no title checks in the runtime, rule 6).
s32 cellSpursCreateJobChainWithAttribute(PPUContext& ctx, u32 spurs, u32 job_chain, u32 attr)
{
    if (!attr) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    const u32 entry = ps_read32(base, attr + jcattr_off::jobChainEntry);
    const u16 size_job = ps_read16(base, attr + jcattr_off::sizeJobDescriptor);
    const u16 max_grabbed = ps_read16(base, attr + jcattr_off::maxGrabbedJob);
    const u64 prio = ps_read64(base, attr + jcattr_off::priorities);
    const u32 max_contention = ps_read32(base, attr + jcattr_off::maxContention);
    const bool auto_spu_count = ps_read8(base, attr + jcattr_off::autoSpuCount) != 0;
    const u32 tag1 = ps_read32(base, attr + jcattr_off::tag1), tag2 = ps_read32(base, attr + jcattr_off::tag2);
    const bool fixed_mem = ps_read8(base, attr + jcattr_off::isFixedMemAlloc) != 0;
    const u32 max_size_job = ps_read32(base, attr + jcattr_off::maxSizeJobDescriptor);
    const u32 init_spu_count = ps_read32(base, attr + jcattr_off::initSpuCount);
    if (const s32 r = check_job_chain_attribute(ps_read32(base, attr + jcattr_off::sdkVer), entry, size_job, max_grabbed,
            prio, auto_spu_count, tag1, tag2, max_size_job, init_spu_count);
        r != lv2::CELL_OK) {
        return r;
    }
    if (!job_chain || !spurs) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (job_chain % 128 || spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    std::memset(base + job_chain, 0, 0x110);
    const u32 jm_ver = ps_read32(base, attr + jcattr_off::jmVer);
    if (jm_ver != CELL_SPURS_JOB_REVISION_2 && jm_ver != CELL_SPURS_JOB_REVISION_3) return error(CELL_SPURS_JOB_ERROR_INVAL);
    ps_write8(base, job_chain + jc_off::val2C,
        static_cast<u8>((fixed_mem ? 0x80 : 0) | ((((max_size_job - 0x100) / 128) & 7) << 4)));

    // _spurs::create_job_chain
    const u32 sdk = HleProcess::current()->kernel().config().sdk_version;
    ps_write64(base, job_chain + jc_off::spurs, spurs);
    ps_write8(base, job_chain + jc_off::jmVer, sdk > 0x14FFFF ? CELL_SPURS_JOB_REVISION_1 : CELL_SPURS_JOB_REVISION_0);
    ps_write8(base, job_chain + jc_off::val2F, 0);
    ps_write8(base, job_chain + jc_off::tag1, static_cast<u8>(tag1));
    ps_write8(base, job_chain + jc_off::tag2, static_cast<u8>(tag2));
    ps_write8(base, job_chain + jc_off::isHalted, 0);
    ps_write16(base, job_chain + jc_off::maxGrabbedJob, max_grabbed);
    ps_write64(base, job_chain + jc_off::pc, entry);
    u8 priority[8];
    for (u32 i = 0; i < 8; i++) priority[i] = static_cast<u8>(prio >> (56 - 8 * i));
    u32 wid = 0;
    if (s32 r = add_workload(base, spurs, wid, SPURS_IMG_ADDR_JOB_CHAIN_PM, 0, job_chain, priority, 1, max_contention, 0,
            ps_read32(base, attr + jcattr_off::name), Workload::Kind::JobChain);
        r != lv2::CELL_OK) {
        if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_AGAIN)) r = error(CELL_SPURS_JOB_ERROR_AGAIN);
        else if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL)) r = error(CELL_SPURS_JOB_ERROR_INVAL);
        else if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT)) r = error(CELL_SPURS_JOB_ERROR_STAT);
        return r;
    }
    ps_write64(base, job_chain + jc_off::cause, 0);
    ps_write32(base, job_chain + jc_off::error, 0);
    ps_write32(base, job_chain + jc_off::workloadId, wid);
    // back in cellSpursCreateJobChainWithAttribute
    ps_write8(base, job_chain + jc_off::initSpuCount, static_cast<u8>(init_spu_count));
    ps_write8(base, job_chain + jc_off::jmVer, static_cast<u8>(jm_ver));
    ps_write32(base, job_chain + jc_off::sdkVer, ps_read32(base, attr + jcattr_off::sdkVer));
    ps_write8(base, job_chain + jc_off::jobMemoryCheck, static_cast<u8>(ps_read8(base, attr + jcattr_off::jobMemoryCheck) << 1));
    std::string label;
    if (const u32 name = ps_read32(base, attr + jcattr_off::name)) label = reinterpret_cast<const char*>(base + name);
    PS_LOG_INFO(kChan, "job chain {:#010x} \"{}\" is workload {} of SPURS {:#010x} (entry {:#x}, max contention {}); "
                       "jobs do not run until the native job chain policy exists (needs libsre as the reference)",
        job_chain, label, wid, spurs, entry, max_contention);
    return lv2::CELL_OK;
}

} // namespace

void register_cellSpurs(Registry& r)
{
    r.add("cellSpurs", {
        PS_HLE_FUNC(0x95180230, _cellSpursAttributeInitialize),
        PS_HLE_FUNC(0x07529113, cellSpursAttributeSetNamePrefix),
        PS_HLE_FUNC(0xA839A4D9, cellSpursAttributeSetSpuThreadGroupType),
        PS_HLE_FUNC(0xAA6269A8, cellSpursInitializeWithAttribute),
        PS_HLE_FUNC(0xCA4C4600, cellSpursFinalize),
        PS_HLE_FUNC(0x16394A4E, _cellSpursTasksetAttributeInitialize),
        PS_HLE_FUNC(0x652B70E2, cellSpursTasksetAttributeSetName),
        PS_HLE_FUNC(0xC10931CB, cellSpursCreateTasksetWithAttribute),
        PS_HLE_FUNC(0xA789E631, cellSpursShutdownTaskset),
        PS_HLE_FUNC(0x5EF96465, _cellSpursEventFlagInitialize),
        PS_HLE_FUNC(0x87630976, cellSpursEventFlagAttachLv2EventQueue),
        PS_HLE_FUNC(0x22AAB31D, cellSpursEventFlagDetachLv2EventQueue),
        PS_HLE_FUNC(0xF5507729, cellSpursEventFlagSet),
        PS_HLE_FUNC(0x373523D4, cellSpursEventFlagWait),
        PS_HLE_FUNC(0xB9BC6207, cellSpursAttachLv2EventQueue),
        PS_HLE_FUNC(0x4E66D483, cellSpursDetachLv2EventQueue),
        PS_HLE_FUNC(0x7FDF4FEF, cellSpursBarrierInitialize),
        PS_HLE_FUNC(0x3548F483, _cellSpursJobChainAttributeInitialize),
        PS_HLE_FUNC(0x9FEF70C2, cellSpursJobChainAttributeSetName),
        PS_HLE_FUNC(0xBB68D76E, cellSpursJobChainAttributeSetHaltOnError),
        PS_HLE_FUNC(0x303C19CD, cellSpursCreateJobChainWithAttribute),
    });
}

} // namespace pspocket::hle
