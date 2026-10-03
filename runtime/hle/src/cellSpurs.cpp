// cellSpurs, native (D-037): the SPURS API on the host. Guest-visible structures keep RPCS3's layouts
// (rpcs3/Emu/Cell/Modules/cellSpurs.h); API semantics follow RPCS3 cellSpurs.cpp where it implements them.
//
// Stage 1: attributes, initialize and finalize. Stage 3a: workload registration and tasksets (PPU side; tasks do not
// run until the native taskset policy exists). Initialize fills the CellSpurs fields RPCS3's
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
    CELL_SPURS_TASK_ERROR_INVAL = 0x80410902,
    CELL_SPURS_TASK_ERROR_STAT = 0x8041090F,
    CELL_SPURS_TASK_ERROR_ALIGN = 0x80410910,
    CELL_SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
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
constexpr u32 CELL_SPURS_MAX_WORKLOAD = 16;
constexpr u32 CELL_SPURS_TASKSET_ATTRIBUTE_REVISION = 1;
constexpr u32 kTasksetSize = 6400; // CellSpursTaskset (CellSpursTaskset2 is larger; not supported yet)
constexpr u8 SPURS_WKL_STATE_PREPARING = 1, SPURS_WKL_STATE_RUNNABLE = 2, SPURS_WKL_STATE_SHUTTING_DOWN = 3,
             SPURS_WKL_STATE_REMOVABLE = 4;
constexpr u32 CELL_SPURS_MAX_WORKLOAD2 = 32;
constexpr u32 CELL_SPURS_EVENT_FLAG_LAST = 3;       // ANY2ANY
constexpr u32 CELL_SPURS_EVENT_FLAG_CLEAR_LAST = 1; // MANUAL
constexpr u8 CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT = 0xFF;

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
    enum class Kind { Taskset, Custom } kind = Kind::Custom;
    u32 arg = 0; // the taskset (Taskset) or the policy module's argument
};

struct Instance {
    u32 nspus = 0;
    bool second = false;
    std::map<u32, Workload> workloads; // by workload ID
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
        h.instances[spurs] = Instance{static_cast<u32>(nspus), second, {}};
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

// ------------------------------------------------------------------------------------------------- event flags

// CellSpursEventFlag (128 bytes, 128-aligned): ctrl (8 bytes), spuTaskUsedWaitSlots, spuTaskWaitMode, spuPort, isIwl,
// direction, clearMode, ..., addr (u64) at 0x70.
namespace ef_off {
constexpr u32 spuPort = 0x0C, isIwl = 0x0D, direction = 0x0E, clearMode = 0x0F, addr = 0x70, size = 0x80;
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
    });
}

} // namespace pspocket::hle
