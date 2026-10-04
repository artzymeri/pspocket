// cellSpurs, native (D-037): the SPURS API on the host. Guest-visible structures keep RPCS3's layouts
// (rpcs3/Emu/Cell/Modules/cellSpurs.h); API semantics follow RPCS3 cellSpurs.cpp where it implements them. Sony's
// libsre (fw 4.92, read on the Mac, never copied) is the authority where RPCS3 stubs a function (D-042): task creation,
// the task attribute, the LS pattern helpers and taskset shutdown are checked against it ("as libsre").
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
#include <format>
#include <map>
#include <mutex>
#include <string>

#include "guest.hpp"
#include "spurs.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

using namespace spurs;



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
        h.instances[spurs] = Instance{static_cast<u32>(nspus), second, {}, {}, {}};
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
    // The native scheduler starts once the guest structure is set up: its workers read it.
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        const auto it = h.instances.find(spurs);
        if (it != h.instances.end() && HleProcess::current()->config().spurs_workers) {
            start_scheduler(h, thread_of(ctx).kernel, spurs, it->second);
        }
    }
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
    std::shared_ptr<Scheduler> scheduler;
    {
        std::lock_guard lock(h.mutex);
        const auto it = h.instances.find(spurs);
        if (it == h.instances.end()) return error(CELL_SPURS_CORE_ERROR_STAT);
        scheduler = it->second.scheduler;
    }
    // The workers stop before the instance goes: they take the host lock while they finish.
    if (scheduler) stop_scheduler_of(*scheduler);
    std::lock_guard lock(h.mutex);
    h.instances.erase(spurs);
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

// cellSpursAddWorkload(spurs, wid, pm, size, data, priority, minContention, maxContention). libsre's is a thin wrapper
// over its internal add-workload with no names and no shutdown hook, as RPCS3's (_spurs::add_workload). The workload
// is a game's own policy module, which the native scheduler runs (spurs_custom.cpp).
s32 cellSpursAddWorkload(PPUContext& ctx, u32 spurs, u32 wid, u32 pm, u32 size, u64 data, u32 priority, u32 min_contention,
    u32 max_contention)
{
    PS_LOG_INFO(kChan, "cellSpursAddWorkload(spurs {:#x}, wid {:#x}, pm {:#x}, size {:#x}, data {:#x}, priority {:#x}, "
        "contention {}..{})", spurs, wid, pm, size, data, priority, min_contention, max_contention);
    if (!spurs || !wid || !pm) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, priority, 8)) {
        log_call_site(ctx, base, std::format("cellSpursAddWorkload: priority array {:#x} is not readable guest memory",
            priority));
        return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    }
    u8 prio[8];
    for (u32 i = 0; i < 8; i++) prio[i] = ps_read8(base, priority + i);
    u32 w = 0;
    const s32 r = add_workload(base, spurs, w, pm, size, data, prio, min_contention, max_contention, 0, 0,
        Workload::Kind::Custom);
    if (r == lv2::CELL_OK) {
        ps_write32(base, wid, w);
        PS_LOG_INFO(kChan, "custom workload {} of SPURS {:#x}: policy module {:#x} ({:#x} bytes), priority {} {} {} {} {} "
            "{} {} {}", w, spurs, pm, size, prio[0], prio[1], prio[2], prio[3], prio[4], prio[5], prio[6], prio[7]);
    }
    return r;
}

// The checks libsre's and RPCS3's workload functions share: NULL, 128-byte alignment, a workload ID within the
// instance's 16 (32 for SPURS2), registered (SRCH), no exception (STAT).
s32 check_workload(u8* base, u32 spurs, u32 wid)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    const bool second = (ps_read8(base, spurs + spurs_off::flags1) & SF1_32_WORKLOADS) != 0;
    if (wid >= (second ? CELL_SPURS_MAX_WORKLOAD2 : CELL_SPURS_MAX_WORKLOAD)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) {
        PS_LOG_ERROR(kChan, "SPURS2 workload {} is not implemented", wid);
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    if (!(ps_read32(base, spurs + wkl_off::enabled) & (0x80000000u >> wid))) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    return lv2::CELL_OK;
}

// cellSpursReadyCountStore(spurs, wid, value), as libsre (and RPCS3): check_workload, a value of at most 255 (INVAL), a
// runnable workload (STAT), then the store. Through the reservation system, so idle workers see it at once (libsre
// stores the byte; an SPU waiting on the line notices any store).
s32 cellSpursReadyCountStore(PPUContext& ctx, u32 spurs, u32 wid, u32 value)
{
    u8* base = guest_base(ctx);
    if (const s32 r = check_workload(base, spurs, wid); r != lv2::CELL_OK) return r;
    if (value > 0xFF) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (ps_read8(base, spurs + wkl_off::state1 + wid) != SPURS_WKL_STATE_RUNNABLE) {
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    reservation_op(base, spurs, [&](u8* line) {
        ps_write8(line, wkl_off::readyCount1 + wid, static_cast<u8>(value));
        return true;
    });
    PS_LOG_DEBUG(kChan, "cellSpursReadyCountStore(SPURS {:#x}, workload {}, {})", spurs, wid, value);
    return lv2::CELL_OK;
}

// cellSpursGetWorkloadFlag(spurs, flag): the address of the instance's CellSpursWorkloadFlag (libsre, RPCS3).
s32 cellSpursGetWorkloadFlag(PPUContext& ctx, u32 spurs, u32 flag)
{
    if (!spurs || !flag) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    ps_write32(guest_base(ctx), flag, spurs + spurs_off::wklFlagStruct);
    return lv2::CELL_OK;
}

// _cellSpursWorkloadFlagReceiver(spurs, wid, isSet), as RPCS3: makes the workload the receiver of the workload flag
// (BUSY if another one is), or stops it being the receiver; either way the flag is reset to "not raised" (all ones).
// Code that raises the flag stores 0 into it, which makes a worker select the receiver with poll status 4.
s32 _cellSpursWorkloadFlagReceiver(PPUContext& ctx, u32 spurs, u32 wid, u32 is_set)
{
    u8* base = guest_base(ctx);
    if (const s32 r = check_workload(base, spurs, wid); r != lv2::CELL_OK) return r;
    s32 rc = lv2::CELL_OK;
    reservation_op(base, spurs, [&](u8* line) {
        rc = lv2::CELL_OK;
        const u8 receiver = ps_read8(line, spurs_off::wklFlagReceiver);
        if (is_set && receiver != 0xFF) {
            rc = error(CELL_SPURS_POLICY_MODULE_ERROR_BUSY);
            return false;
        }
        ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
        if (is_set) ps_write8(line, spurs_off::wklFlagReceiver, static_cast<u8>(wid));
        else if (receiver == wid) ps_write8(line, spurs_off::wklFlagReceiver, 0xFF);
        return true;
    });
    PS_LOG_INFO(kChan, "_cellSpursWorkloadFlagReceiver(SPURS {:#x}, workload {}, {}) -> {:#x}", spurs, wid,
        is_set ? "set" : "clear", static_cast<u32>(rc));
    return rc;
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

// cellSpursTasksetAttributeSetTasksetSize as libsre (fw 4.92) and RPCS3: a CellSpursTaskset (6400 bytes) or a
// CellSpursTaskset2 (10496), else INVAL.
s32 cellSpursTasksetAttributeSetTasksetSize(PPUContext& ctx, u32 attr, u32 size)
{
    if (!attr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (size != kTasksetSize && size != kTaskset2Size) return error(CELL_SPURS_TASK_ERROR_INVAL);
    ps_write32(guest_base(ctx), attr + tsattr_off::taskset_size, size);
    return lv2::CELL_OK;
}

// RPCS3 _spurs::create_taskset: clears the taskset, links it to the SPURS instance and adds it as a workload whose
// policy module is the taskset marker, with minimum contention 8 and the attribute's priorities and maximum.
s32 create_taskset(u8* base, u32 spurs, u32 taskset, u64 args, const u8 (&priority)[8], u32 max_contention, u32 name,
    u32 size, s32 enable_clear_ls)
{
    if (!spurs || !taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (size != kTasksetSize && size != kTaskset2Size) {
        PS_LOG_ERROR(kChan, "create_taskset: taskset size {:#x} is neither a CellSpursTaskset's nor a CellSpursTaskset2's",
            size);
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
    const u32 size = ps_read32(base, attr + tsattr_off::taskset_size);
    const s32 r = create_taskset(base, spurs, taskset, ps_read64(base, attr + tsattr_off::args), prio,
        ps_read32(base, attr + tsattr_off::max_contention), ps_read32(base, attr + tsattr_off::name), size,
        static_cast<s32>(ps_read32(base, attr + tsattr_off::enable_clear_ls)));
    // libsre (fw 4.92): a CellSpursTaskset2 also gets the default exit code container handler's descriptor at 0x1900
    // (EA | size in 128-byte units; the handler's marker here).
    if (r == lv2::CELL_OK && size > 0x28FF) {
        ps_write64(base, taskset + 0x1900,
            SPURS_IMG_ADDR_TASK_EXIT_CODE_HANDLER | ((kTaskExitCodeHandlerSize + 127) >> 7));
    }
    return r;
}

// cellSpursShutdownWorkload, checked against libsre (fw 4.92), which RPCS3 follows: NULL, ALIGN, INVAL for a
// workload ID past the instance's 16 (32 for SPURS2), SRCH for a workload that is not registered (its wklEnabled bit
// clear), STAT for an instance in exception state or a workload not yet runnable. The native scheduler does not keep
// wklStatus (which SPUs run a workload; status 0), so a shutdown makes the workload removable at once: a worker that is
// running it leaves at its next poll, since the selection only takes runnable workloads. The shutdown-completion
// notification libsre and RPCS3 send to the SPURS event port (when a hook is registered) has no handler thread to go
// to natively; it is logged instead.
s32 shutdown_workload(u8* base, u32 spurs, u32 wid)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    const bool second = (ps_read8(base, spurs + spurs_off::flags1) & SF1_32_WORKLOADS) != 0;
    if (wid >= (second ? CELL_SPURS_MAX_WORKLOAD2 : CELL_SPURS_MAX_WORKLOAD)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!(ps_read32(base, spurs + wkl_off::enabled) & (0x80000000u >> wid))) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
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
    // libsre also reads wklStatus2[wid] for workloads 0..15 (RPCS3 reads only wklStatus1).
    if (ps_read8(base, spurs + wkl_off::status1 + wid) || ps_read8(base, spurs + wkl_off::status2 + wid)) {
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

// cellSpursShutdownTaskset as libsre (fw 4.92): NULL, ALIGN, INVAL for a workload ID past 31 in the taskset, then
// cellSpursShutdownWorkload on the SPURS instance and workload the taskset names. Its POLICY_MODULE_ERROR_STAT becomes
// TASK_ERROR_STAT; other results pass unchanged, so a taskset that was never created (zeroed: no SPURS) gets
// POLICY_MODULE_ERROR_NULL_POINTER, and workload 16..31 of a SPURS1 instance POLICY_MODULE_ERROR_INVAL. libsre
// asserts (and aborts the process) when the shutdown returns ALIGN or SRCH: a misaligned SPURS pointer, or a workload
// that is not registered.
s32 cellSpursShutdownTaskset(PPUContext& ctx, u32 taskset)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, taskset, 128)) {
        log_call_site(ctx, base, std::format("cellSpursShutdownTaskset: taskset {:#x} is not readable guest memory; "
            "refused with CELL_SPURS_TASK_ERROR_FAULT", taskset));
        return error(CELL_SPURS_TASK_ERROR_FAULT);
    }
    const u32 wid = ps_read32(base, taskset + ts_off::wid);
    if (wid >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
    const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
    s32 r = shutdown_workload(base, spurs, wid);
    PS_LOG_INFO(kChan, "cellSpursShutdownTaskset({:#010x}): workload {} of SPURS {:#x} -> {:#x}", taskset, wid, spurs,
        static_cast<u32>(r));
    if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN) || r == error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH)) {
        log_call_site(ctx, base, "cellSpursShutdownTaskset");
        PS_FATAL(kChan, "cellSpursShutdownTaskset({:#x}): shutting down workload {} of SPURS {:#x} returned {:#x}; libsre "
            "(fw 4.92) asserts here and aborts the process", taskset, wid, spurs, static_cast<u32>(r));
    }
    if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT)) r = error(CELL_SPURS_TASK_ERROR_STAT);
    return r;
}

// ------------------------------------------------------------------------------------------------- tasks
//
// Task creation and the LS pattern helpers follow Sony's libsre (fw 4.92, read on the Mac as the reference; D-037,
// D-042): RPCS3 implements only cellSpursCreateTask and stubs the rest. Signatures, checks and their order, error codes
// and what is written to guest memory are libsre's. libsre checks pointers only against NULL; where it would read a
// bad pointer, pspocket checks first and refuses the call with TASK_ERROR_FAULT and a log of the call site (D-042).
// Where libsre asserts (it prints the failed condition and aborts the process), pspocket stops with a fatal log.

constexpr u32 CELL_SPURS_MAX_TASK = 128;
constexpr u32 CELL_SPURS_TASK_TOP = 0x3000;     // task code and data start here; LS below belongs to SPURS
constexpr u32 CELL_SPURS_TASK_BOTTOM = 0x40000; // end of local store
constexpr u32 CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE = 1024;
constexpr u32 CELL_SPURS_TASK_ATTRIBUTE_REVISION = 1;
constexpr u32 kLsBlock = 0x800;            // one bit of an LS pattern
constexpr u32 kTaskMaxSavedBlocks = 0x7A;  // a context save area larger than 0x3D400 bytes adds no blocks
constexpr u32 kTaskset2MinSize = kTaskset2Size; // CellSpursTaskset2 (libsre: taskset size > 0x28FF)

// CellSpursTaskLsPattern: 128 bits, one per 2 KB block of local store, most significant bit first (the top bit of the
// first big-endian word is LS 0..0x7FF). The six blocks below CELL_SPURS_TASK_TOP are SPURS' own: task creation and
// cellSpursTaskGetContextSaveAreaSize refuse patterns that touch them (libsre checks the top 6 bits of word 0).
constexpr u128 kLsPatternReserved = u128{0xFC000000} << 96;

u128 read_ls_pattern(u8* base, u32 addr)
{
    return (u128{ps_read64(base, addr)} << 64) | ps_read64(base, addr + 8);
}

void write_ls_pattern(u8* base, u32 addr, u128 v)
{
    ps_write64(base, addr, static_cast<u64>(v >> 64));
    ps_write64(base, addr + 8, static_cast<u64>(v));
}

// The pattern bits of LS blocks [first, end).
u128 ls_blocks(u32 first, u32 end)
{
    u128 v = 0;
    for (u32 b = first; b < end && b < 128; b++) v |= u128{1} << (127 - b);
    return v;
}

std::string hex128(u128 v)
{
    return std::format("{:016x}{:016x}", static_cast<u64>(v >> 64), static_cast<u64>(v));
}

u32 ls_block_count(u128 v)
{
    return static_cast<u32>(std::popcount(static_cast<u64>(v >> 64)) + std::popcount(static_cast<u64>(v)));
}

// The process SDK version as libsre reads it (sys_process_get_sdk_version); a process without one counts as 0x492000.
u32 process_sdk_version(lv2::PpuThread& t)
{
    const u32 v = t.kernel.config().sdk_version;
    return v == 0xFFFFFFFF ? 0x492000 : v;
}

// OK when [addr, addr + size) is readable guest memory, else TASK_ERROR_FAULT and a log of the call site.
s32 task_readable(PPUContext& ctx, std::string_view what, std::string_view name, u32 addr, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return lv2::CELL_OK;
    log_call_site(ctx, t.kernel.base(), std::format("{}: the {} ({:#x}, {} bytes) is not readable guest memory; refused "
        "with CELL_SPURS_TASK_ERROR_FAULT", what, name, addr, size));
    return error(CELL_SPURS_TASK_ERROR_FAULT);
}

// A task's SPU ELF (Elf32, big-endian) as libsre reads it.
namespace elf_off {
constexpr u32 e_machine = 0x12, e_entry = 0x18, e_phoff = 0x1C, e_phentsize = 0x2A, e_phnum = 0x2C, ehdr_size = 0x34;
constexpr u32 p_type = 0x00, p_vaddr = 0x08, p_paddr = 0x0C, p_filesz = 0x10, p_memsz = 0x14, p_flags = 0x18;
}
constexpr u32 kElfMagic = 0x7F454C46;
constexpr u64 kSpuElfIdent = 0x7F454C4601020100; // magic, ELFCLASS32, ELFDATA2MSB, EV_CURRENT, ELFOSABI_NONE
constexpr u16 EM_SPU = 23;
constexpr u32 PT_LOAD = 1, PF_W = 2;

// libsre's ELF check at task creation (and in _cellSpursTaskAttributeInitialize): the first 8 bytes of e_ident, EM_SPU,
// an entry point at or above CELL_SPURS_TASK_TOP and every PT_LOAD segment's address at or above it; NOEXEC otherwise.
s32 check_task_elf(PPUContext& ctx, std::string_view what, u32 elf)
{
    if (const s32 rc = task_readable(ctx, what, "task ELF header", elf, elf_off::ehdr_size); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    if (ps_read64(base, elf) != kSpuElfIdent || ps_read16(base, elf + elf_off::e_machine) != EM_SPU ||
        ps_read32(base, elf + elf_off::e_entry) < CELL_SPURS_TASK_TOP) {
        return error(CELL_SPURS_TASK_ERROR_NOEXEC);
    }
    const u32 phoff = ps_read32(base, elf + elf_off::e_phoff), phentsize = ps_read16(base, elf + elf_off::e_phentsize),
              phnum = ps_read16(base, elf + elf_off::e_phnum);
    for (u32 i = 0; i < phnum; i++) {
        const u32 ph = elf + phoff + i * phentsize;
        if (const s32 rc = task_readable(ctx, what, "task ELF program header", ph, elf_off::p_vaddr + 4); rc != lv2::CELL_OK) {
            return rc;
        }
        if (ps_read32(base, ph + elf_off::p_type) == PT_LOAD && ps_read32(base, ph + elf_off::p_vaddr) < CELL_SPURS_TASK_TOP) {
            return error(CELL_SPURS_TASK_ERROR_NOEXEC);
        }
    }
    return lv2::CELL_OK;
}

// The pattern of LS range [start, start + size), as libsre's cellSpursTaskGenerateLsPattern computes it: both 2 KB
// aligned (ALIGN), the end at most 0x40000 (INVAL; libsre adds in 32 bits and does not check the start against
// CELL_SPURS_TASK_TOP). A range of more than 128 blocks (only an end that wraps past 2^32 gets that far) is a libsre
// assertion.
s32 ls_range_pattern(u32 start, u32 size, u128& out)
{
    if (start % kLsBlock || size % kLsBlock) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (start + size > CELL_SPURS_TASK_BOTTOM) return error(CELL_SPURS_TASK_ERROR_INVAL);
    const u32 first = start / kLsBlock, count = size / kLsBlock;
    if (first > 128 || first + count > 128) {
        PS_FATAL(kChan, "LS pattern of {:#x} + {:#x}: more than 128 blocks; libsre (fw 4.92) asserts here and aborts the "
            "process", start, size);
    }
    out = ls_blocks(first, first + count);
    return lv2::CELL_OK;
}

// cellSpursTaskGetLoadableSegmentPattern and ...GetReadOnlyAreaPattern(pattern, elf), as libsre. Checks: elf NULL,
// then the ELF's magic (4 bytes only), EM_SPU and an entry point in the task area (NOEXEC), then pattern NULL. The
// pattern is cleared, then each program header in turn: any whose vaddr + memsz (32 bits) passes 0x40000 ends the call
// with INVAL (the blocks found so far stay in the pattern). PT_LOAD segments with a paddr below 0x40000 add:
// - loadable: every block the segment's memory image touches (vaddr rounded down to vaddr + memsz rounded up);
// - read-only (segments without PF_W): the blocks their file image covers completely (vaddr rounded up to vaddr +
//   filesz rounded down). A read-only segment that covers no block completely but ends in a later block than it
//   starts in makes libsre assert (its 32-bit range wraps).
// Nothing checks segments against CELL_SPURS_TASK_TOP here, so a segment below it sets reserved blocks.
s32 task_pattern_from_elf(PPUContext& ctx, u32 pattern, u32 elf, bool read_only, const char* what)
{
    if (!elf) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (const s32 rc = task_readable(ctx, what, "task ELF header", elf, elf_off::ehdr_size); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    if (ps_read32(base, elf) != kElfMagic || ps_read16(base, elf + elf_off::e_machine) != EM_SPU ||
        ps_read32(base, elf + elf_off::e_entry) < CELL_SPURS_TASK_TOP) {
        return error(CELL_SPURS_TASK_ERROR_NOEXEC);
    }
    if (!pattern) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (const s32 rc = task_readable(ctx, what, "LS pattern", pattern, 16); rc != lv2::CELL_OK) return rc;
    const u32 phoff = ps_read32(base, elf + elf_off::e_phoff), phentsize = ps_read16(base, elf + elf_off::e_phentsize),
              phnum = ps_read16(base, elf + elf_off::e_phnum);
    u128 v = 0;
    write_ls_pattern(base, pattern, v);
    for (u32 i = 0; i < phnum; i++) {
        const u32 ph = elf + phoff + i * phentsize;
        if (const s32 rc = task_readable(ctx, what, "task ELF program header", ph, elf_off::p_flags + 4); rc != lv2::CELL_OK) {
            return rc;
        }
        const u32 vaddr = ps_read32(base, ph + elf_off::p_vaddr), memsz = ps_read32(base, ph + elf_off::p_memsz);
        if (vaddr + memsz > CELL_SPURS_TASK_BOTTOM) {
            PS_LOG_WARN(kChan, "{}(ELF {:#x}): program header {} ends at {:#x}, past local store: CELL_SPURS_TASK_ERROR_INVAL",
                what, elf, i, vaddr + memsz);
            return error(CELL_SPURS_TASK_ERROR_INVAL);
        }
        if (ps_read32(base, ph + elf_off::p_type) != PT_LOAD || ps_read32(base, ph + elf_off::p_paddr) >= CELL_SPURS_TASK_BOTTOM) {
            continue;
        }
        u32 start = 0, size = 0;
        if (!read_only) {
            start = vaddr & ~(kLsBlock - 1);
            size = (((vaddr + kLsBlock - 1 + memsz) & ~(kLsBlock - 1)) - start) & ~(kLsBlock - 1);
        } else {
            if (ps_read32(base, ph + elf_off::p_flags) & PF_W) continue;
            start = (vaddr + kLsBlock - 1) & ~(kLsBlock - 1);
            const u32 end = (vaddr + ps_read32(base, ph + elf_off::p_filesz)) & ~(kLsBlock - 1);
            size = (end - start) & ~(kLsBlock - 1);
        }
        u128 seg = 0;
        if (const s32 rc = ls_range_pattern(start, size, seg); rc != lv2::CELL_OK) return rc;
        v |= seg;
        write_ls_pattern(base, pattern, v);
    }
    PS_LOG_INFO(kChan, "{}(ELF {:#x}): {} LS blocks, {} (as libsre)", what, elf, ls_block_count(v), hex128(v));
    return lv2::CELL_OK;
}

s32 cellSpursTaskGetLoadableSegmentPattern(PPUContext& ctx, u32 pattern, u32 elf)
{
    return task_pattern_from_elf(ctx, pattern, elf, false, "cellSpursTaskGetLoadableSegmentPattern");
}

s32 cellSpursTaskGetReadOnlyAreaPattern(PPUContext& ctx, u32 pattern, u32 elf)
{
    return task_pattern_from_elf(ctx, pattern, elf, true, "cellSpursTaskGetReadOnlyAreaPattern");
}

// cellSpursTaskGenerateLsPattern(pattern, start, size), as libsre: NULL, then ls_range_pattern's checks.
s32 cellSpursTaskGenerateLsPattern(PPUContext& ctx, u32 pattern, u32 start, u32 size)
{
    if (!pattern) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    u128 v = 0;
    if (const s32 rc = ls_range_pattern(start, size, v); rc != lv2::CELL_OK) return rc;
    if (const s32 rc = task_readable(ctx, "cellSpursTaskGenerateLsPattern", "LS pattern", pattern, 16); rc != lv2::CELL_OK) {
        return rc;
    }
    write_ls_pattern(guest_base(ctx), pattern, v);
    PS_LOG_INFO(kChan, "cellSpursTaskGenerateLsPattern({:#x}, {:#x}): {} (as libsre)", start, size, hex128(v));
    return lv2::CELL_OK;
}

// cellSpursTaskGetContextSaveAreaSize(size, pattern), as libsre: 1 KB of execution context plus 2 KB per block of the
// pattern (the inverse of task creation's (size - 0x400) >> 11); INVAL for a pattern with reserved blocks. libsre
// checks neither pointer.
s32 cellSpursTaskGetContextSaveAreaSize(PPUContext& ctx, u32 size_out, u32 pattern)
{
    constexpr std::string_view what = "cellSpursTaskGetContextSaveAreaSize";
    if (const s32 rc = task_readable(ctx, what, "LS pattern", pattern, 16); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    const u128 v = read_ls_pattern(base, pattern);
    if (v & kLsPatternReserved) return error(CELL_SPURS_TASK_ERROR_INVAL);
    const u32 size = CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE + ls_block_count(v) * kLsBlock;
    if (const s32 rc = task_readable(ctx, what, "size", size_out, 4); rc != lv2::CELL_OK) return rc;
    ps_write32(base, size_out, size);
    PS_LOG_INFO(kChan, "cellSpursTaskGetContextSaveAreaSize({}): {:#x} (as libsre)", hex128(v), size);
    return lv2::CELL_OK;
}

// A task to create: what cellSpursCreateTask takes, or what a task attribute holds. `pattern` (CellSpursTaskLsPattern)
// and `args` (CellSpursTaskArgument, 16 bytes) are guest pointers, 0 for none.
struct TaskSpec {
    u32 elf = 0, context = 0, context_size = 0, pattern = 0, args = 0;
};

// libsre's task checks, in its order; _cellSpursTaskAttributeInitialize runs the same ones as task creation. ELF
// NULL; ELF 16-byte and context 16-byte aligned (128-byte from SDK 0x280000, the process' SDK version); a context of
// at least 1 KB; the ELF (check_task_elf); with a context and a pattern, a pattern of at most as many blocks as the
// context has room for and none reserved (INVAL). The pattern only describes what the context keeps, so without a
// context it is not read (run 013: GTA V passed a value that is no guest address there). `saved_blocks` gets the LS
// blocks the context save area has room for (0 without a context).
s32 check_task(PPUContext& ctx, std::string_view what, const TaskSpec& s, u32& saved_blocks)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!s.elf) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (s.elf % 16 || s.context % (process_sdk_version(t) > 0x27FFFF ? 128 : 16)) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (s.context && s.context_size < CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE) return error(CELL_SPURS_TASK_ERROR_INVAL);
    if (const s32 rc = check_task_elf(ctx, what, s.elf); rc != lv2::CELL_OK) return rc;
    saved_blocks = 0;
    if (!s.context) return lv2::CELL_OK;
    saved_blocks = s.context_size > 0x3D400 ? kTaskMaxSavedBlocks : (s.context_size - 0x400) >> 11;
    if (!s.pattern) return lv2::CELL_OK;
    if (const s32 rc = task_readable(ctx, what, "LS pattern", s.pattern, 16); rc != lv2::CELL_OK) return rc;
    const u128 v = read_ls_pattern(t.kernel.base(), s.pattern);
    if (ls_block_count(v) > saved_blocks || (v & kLsPatternReserved)) return error(CELL_SPURS_TASK_ERROR_INVAL);
    return lv2::CELL_OK;
}

// RPCS3 cellSpursSendWorkloadSignal (SPURS1 workloads): sets the workload's bit in wklSignal1.
s32 send_workload_signal(u8* base, u32 spurs, u32 wid)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!(ps_read32(base, spurs + wkl_off::enabled) & (0x80000000u >> wid))) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    if (ps_read8(base, spurs + wkl_off::state1 + wid) != SPURS_WKL_STATE_RUNNABLE) {
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    reservation_op(base, spurs, [wid](u8* line) {
        ps_write16(line, wkl_off::signal1, static_cast<u16>(ps_read16(line, wkl_off::signal1) | (0x8000u >> wid)));
        return true;
    });
    return lv2::CELL_OK;
}

// cellSpursWakeUp (RPCS3; libsre's checks are the same): marks the SPURS handler dirty. On the PS3 that wakes SPURS'
// PPU handler thread, which restarts SPUs that went idle; natively the scheduler will be woken here (stage 3b).
s32 cellSpursWakeUp(PPUContext& ctx, u32 spurs)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    __atomic_store_n(base + spurs + spurs_off::handlerDirty, u8{1}, __ATOMIC_SEQ_CST);
    wake_workers(spurs); // natively: the scheduler's idle workers
    return lv2::CELL_OK;
}

// libsre's task creation and task start, shared by cellSpursCreateTask and cellSpursCreateTaskWithAttribute once they
// have checked their own arguments (task ID pointer, taskset): check_task, the lowest free task ID in the taskset's
// `enabled` bits (AGAIN when all 128 are taken), its TaskInfo (argument, ELF, context | saved blocks, LS pattern; the
// argument is zero without one, the pattern without a context or a pattern), then the start: the task's pending-ready
// bit, cellSpursSendWorkloadSignal (its result ignored, as libsre) and cellSpursWakeUp, whose STAT becomes
// TASK_ERROR_STAT (libsre asserts on any other error). The ID is written only when all of that succeeded.
s32 create_task(PPUContext& ctx, std::string_view what, u32 taskset, u32 task_id_out, const TaskSpec& s)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u32 saved_blocks = 0;
    if (const s32 rc = check_task(ctx, what, s, saved_blocks); rc != lv2::CELL_OK) return rc;
    // libsre reads the argument after claiming the ID; checking it first keeps a refused call from using one up.
    if (s.args) {
        if (const s32 rc = task_readable(ctx, what, "task argument", s.args, 16); rc != lv2::CELL_OK) return rc;
    }
    if (const s32 rc = task_readable(ctx, what, "taskset", taskset, ts_off::task_info + CELL_SPURS_MAX_TASK *
            ts_off::task_info_stride);
        rc != lv2::CELL_OK) {
        return rc;
    }

    u32 id = CELL_SPURS_MAX_TASK;
    reservation_op(base, taskset, [&id](u8* line) {
        id = CELL_SPURS_MAX_TASK;
        for (u32 w = 0; w < 4; w++) {
            const u32 enabled = ps_read32(line, ts_off::enabled + 4 * w);
            if (enabled == 0xFFFFFFFF) continue;
            const u32 bit = static_cast<u32>(std::countl_one(enabled));
            id = 32 * w + bit;
            ps_write32(line, ts_off::enabled + 4 * w, enabled | (0x80000000u >> bit));
            return true;
        }
        return false;
    });
    if (id >= CELL_SPURS_MAX_TASK) return error(CELL_SPURS_TASK_ERROR_AGAIN);

    const u32 info = taskset + ts_off::task_info + id * ts_off::task_info_stride;
    ps_write64(base, info + ts_off::ti_elf, s.elf);
    ps_write64(base, info + ts_off::ti_context, u64{s.context | saved_blocks});
    write_ls_pattern(base, info + ts_off::ti_args, s.args ? read_ls_pattern(base, s.args) : 0);
    write_ls_pattern(base, info + ts_off::ti_ls_pattern, s.context && s.pattern ? read_ls_pattern(base, s.pattern) : 0);

    // Task start.
    reservation_op(base, taskset, [id](u8* line) {
        const u32 w = ts_off::pending_ready + 4 * (id / 32);
        ps_write32(line, w, ps_read32(line, w) | (0x80000000u >> (id % 32)));
        return true;
    });
    const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
    const u32 wid = ps_read32(base, taskset + ts_off::wid);
    send_workload_signal(base, spurs, wid);
    const s32 woken = cellSpursWakeUp(ctx, spurs);
    if (woken == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT)) return error(CELL_SPURS_TASK_ERROR_STAT);
    if (woken != lv2::CELL_OK) {
        PS_FATAL(kChan, "{}: cellSpursWakeUp({:#x}) for task {} of taskset {:#x} returned {:#x}; libsre (fw 4.92) asserts "
            "here and aborts the process", what, spurs, id, taskset, static_cast<u32>(woken));
    }
    ps_write32(base, task_id_out, id);
    PS_LOG_INFO(kChan, "task {} of taskset {:#010x} (workload {}): ELF {:#x}, context {:#x} ({:#x} bytes, {} LS blocks); "
        "ready", id, taskset, wid, s.elf, s.context, s.context_size, saved_blocks);
    return lv2::CELL_OK;
}

// cellSpursCreateTask(taskset, id, elf, context, size, lsPattern, argument), as libsre: id and taskset NULL, taskset
// 128-byte aligned, then create_task.
s32 cellSpursCreateTask(PPUContext& ctx, u32 taskset, u32 task_id, u32 elf, u32 context, u32 size, u32 ls_pattern,
    u32 argument)
{
    if (!task_id || !taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    return create_task(ctx, "cellSpursCreateTask", taskset, task_id, TaskSpec{elf, context, size, ls_pattern, argument});
}

// CellSpursTaskAttribute (256 bytes, opaque in the SDK; libsre wants it 8-byte aligned) in libsre's layout:
// revision, SDK version, ELF (u64), context (u64), context size, the LS pattern (16 bytes) at 0x20, the task argument
// (16 bytes) at 0x30 and the exit code container (u64; cellSpursTaskAttributeSetExitCodeContainer, which wants
// revision 1 and a 128-byte aligned container) at 0x40. The rest is zero.
namespace taskattr_off {
constexpr u32 revision = 0x00, sdk_version = 0x04, elf = 0x08, context = 0x10, context_size = 0x18, ls_pattern = 0x20,
              args = 0x30, exit_code_container = 0x40, size = 0x100;
}
// CellSpursTaskSaveConfig {eaContext, sizeContext, lsPattern (pointer)}: how _cellSpursTaskAttributeInitialize gets a
// context.
namespace savecfg_off {
constexpr u32 context = 0x0, context_size = 0x4, ls_pattern = 0x8, size = 0xC;
}

// _cellSpursTaskAttributeInitialize(attr, revision, sdkVersion, elf, saveConfig, argument), as libsre (six arguments;
// run 013's log shows GTA V's Bink passing a stack CellSpursTaskSaveConfig in r7 and no argument in r8). Checks: attr
// NULL, a saveConfig whose context is NULL (NULL_POINTER), attr 8-byte aligned, then check_task. The revision and SDK
// version are stored, not checked. The attribute is cleared; the context fields are set only with a saveConfig, the
// pattern only when it has one, the argument only when given.
s32 _cellSpursTaskAttributeInitialize(PPUContext& ctx, u32 attr, u32 revision, u32 sdk_version, u32 elf, u32 save_config,
    u32 argument)
{
    constexpr std::string_view what = "_cellSpursTaskAttributeInitialize";
    PS_LOG_INFO(kChan, "_cellSpursTaskAttributeInitialize(attr {:#x}, revision {}, sdk {:#x}, ELF {:#x}, saveConfig {:#x}, "
        "argument {:#x})", attr, revision, sdk_version, elf, save_config, argument);
    if (!attr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    u8* base = guest_base(ctx);
    TaskSpec s{elf, 0, 0, 0, argument};
    if (save_config) {
        if (const s32 rc = task_readable(ctx, what, "save config", save_config, savecfg_off::size); rc != lv2::CELL_OK) {
            return rc;
        }
        s.context = ps_read32(base, save_config + savecfg_off::context);
        s.context_size = ps_read32(base, save_config + savecfg_off::context_size);
        s.pattern = ps_read32(base, save_config + savecfg_off::ls_pattern);
        PS_LOG_INFO(kChan, "  saveConfig: context {:#x}, size {:#x}, LS pattern {:#x}", s.context, s.context_size, s.pattern);
        if (!s.context) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    }
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    u32 saved_blocks = 0;
    if (const s32 rc = check_task(ctx, what, s, saved_blocks); rc != lv2::CELL_OK) return rc;
    if (argument) {
        if (const s32 rc = task_readable(ctx, what, "task argument", argument, 16); rc != lv2::CELL_OK) return rc;
    }
    if (const s32 rc = task_readable(ctx, what, "attribute", attr, taskattr_off::size); rc != lv2::CELL_OK) return rc;

    std::memset(base + attr, 0, taskattr_off::size);
    ps_write32(base, attr + taskattr_off::revision, revision);
    ps_write32(base, attr + taskattr_off::sdk_version, sdk_version);
    ps_write64(base, attr + taskattr_off::elf, elf);
    if (save_config) {
        ps_write64(base, attr + taskattr_off::context, s.context);
        ps_write32(base, attr + taskattr_off::context_size, s.context_size);
        if (s.pattern) write_ls_pattern(base, attr + taskattr_off::ls_pattern, read_ls_pattern(base, s.pattern));
    }
    if (argument) write_ls_pattern(base, attr + taskattr_off::args, read_ls_pattern(base, argument));
    return lv2::CELL_OK;
}

// cellSpursCreateTaskWithAttribute(taskset, id, attr), as libsre: id and taskset NULL, taskset 128-byte aligned, attr
// NULL, attr 8-byte aligned, revision 1 (INVAL). An exit code container needs a CellSpursTaskset2 (PERM for a
// CellSpursTaskset; a misaligned one is a libsre assertion). Then create_task with the attribute's ELF, context and
// size (the low words of the u64 fields, as libsre) and its pattern and argument, which always exist (zero when unset).
s32 cellSpursCreateTaskWithAttribute(PPUContext& ctx, u32 taskset, u32 task_id, u32 attr)
{
    constexpr std::string_view what = "cellSpursCreateTaskWithAttribute";
    if (!task_id || !taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (!attr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = task_readable(ctx, what, "attribute", attr, taskattr_off::exit_code_container + 8); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, attr + taskattr_off::revision) != CELL_SPURS_TASK_ATTRIBUTE_REVISION) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    if (const u64 container = ps_read64(base, attr + taskattr_off::exit_code_container)) {
        if (container % 128) {
            PS_FATAL(kChan, "{}: exit code container {:#x} is not 128-byte aligned; libsre (fw 4.92) asserts here and "
                "aborts the process", what, container);
        }
        if (const s32 rc = task_readable(ctx, what, "taskset", taskset, ts_off::size + 4); rc != lv2::CELL_OK) return rc;
        if (ps_read32(base, taskset + ts_off::size) < kTaskset2MinSize) return error(CELL_SPURS_TASK_ERROR_PERM);
        PS_LOG_ERROR(kChan, "{}: exit code container {:#x} on a CellSpursTaskset2 is not implemented", what, container);
        return error(CELL_SPURS_TASK_ERROR_STAT);
    }
    const TaskSpec s{static_cast<u32>(ps_read64(base, attr + taskattr_off::elf)),
        static_cast<u32>(ps_read64(base, attr + taskattr_off::context)), ps_read32(base, attr + taskattr_off::context_size),
        attr + taskattr_off::ls_pattern, attr + taskattr_off::args};
    return create_task(ctx, what, taskset, task_id, s);
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

} // namespace

// attach_lv2_eq and detach_lv2_eq are shared with cellSpursQueue.cpp (declared in spurs.hpp).
namespace spurs {

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

} // namespace spurs

namespace {

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

// A flag the PPU sets: PPU2SPU or ANY2ANY (libsre's cellSpursEventFlagSet, fw 4.92; RPCS3 checks SPU2PPU here, the
// direction a PPU wait needs, which refuses GTA V's PPU-to-SPU flags with PERM).
bool event_flag_from_ppu(u8 direction)
{
    return direction == CELL_SPURS_EVENT_FLAG_PPU2SPU || direction == CELL_SPURS_EVENT_FLAG_ANY2ANY;
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
    if (!event_flag_from_ppu(ps_read8(base, event_flag + ef_off::direction))) return error(CELL_SPURS_TASK_ERROR_PERM);

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
                // The events the task receives (RPCS3 writes them after the update; libsre's SPU side inside it).
                ps_write16(ef, ef_off::pendingRecvTaskEvents + 2 * slot, relevant);
            }
        }
        const bool auto_clear = ps_read8(ef, ef_off::clearMode) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO;
        ps_write16(ef, ef_off::events, auto_clear ? events & ~to_clear : events);
        ps_write16(ef, ef_off::spuTaskPendingRecv, task_pending | pending_recv);
        return true;
    });
    if (send) {
        ps_write16(base, event_flag + ef_off::pendingRecvTaskEvents + 2 * ppu_slot, ppu_events);
        if (const s32 rc = lv2::sys_event_port_send(t, ps_read32(base, event_flag + ef_off::eventPortId), 0, 0, 0);
            rc != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "cellSpursEventFlagSet({:#x}): waking the PPU waiter failed: {:#x}", event_flag,
                static_cast<u32>(rc));
            return task_error(rc);
        }
    }
    // Then each SPU task whose condition is met gets a signal (RPCS3 cellSpursEventFlagSet): its taskset is the flag's
    // address, or for an instance-wide flag (isIwl) the taskset of the workload the waiter recorded.
    for (u32 slot = 0; slot < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; slot++) {
        if (!(pending_recv & (0x8000 >> slot))) continue;
        u32 taskset = static_cast<u32>(ps_read64(base, event_flag + ef_off::addr));
        if (ps_read8(base, event_flag + ef_off::isIwl)) {
            const u32 wid = ps_read8(base, event_flag + ef_off::waitingTaskWklId + slot);
            taskset = static_cast<u32>(ps_read64(base, taskset + wkl_off::info1 + wid * wkl_off::info_stride + wkl_off::info_arg));
        }
        const u32 task = ps_read8(base, event_flag + ef_off::waitingTaskId + slot);
        const s32 rc = send_signal(base, taskset, task);
        if (rc == error(CELL_SPURS_TASK_ERROR_INVAL) || rc == error(CELL_SPURS_TASK_ERROR_STAT)) {
            return error(CELL_SPURS_TASK_ERROR_FATAL);
        }
        if (rc != lv2::CELL_OK) {
            PS_FATAL(kChan, "cellSpursEventFlagSet({:#x}): signalling task {} of taskset {:#x} returned {:#x} (RPCS3 "
                "ensures CELL_OK)", event_flag, task, taskset, static_cast<u32>(rc));
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

// cellSpursCreateJobChainWithAttribute + create_job_chain as libsre (fw 4.92, D-054; RPCS3 has the same steps but
// swaps sizeJobDescriptor and maxGrabbedJob and leaves out autoReadyCount and haltOnError), with the native job chain
// marker as the policy module. Deviations: no "JobChain" class name, and libsre's one-title firmware quirk (val2F) is
// not reproduced (no title checks in the runtime, rule 6).
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
    const bool halt_on_error = ps_read32(base, attr + jcattr_off::haltOnError) != 0;
    ps_write8(base, job_chain + jc_off::flags,
        static_cast<u8>((fixed_mem ? jc_off::flagFixedMem : 0) | ((((max_size_job - 0x100) / 128) & 7) << 4) |
                        (halt_on_error ? jc_off::flagHaltOnError : 0)));

    // _spurs::create_job_chain
    const u32 sdk = HleProcess::current()->kernel().config().sdk_version;
    ps_write64(base, job_chain + jc_off::spurs, spurs);
    ps_write8(base, job_chain + jc_off::jmVer, sdk > 0x14FFFF ? CELL_SPURS_JOB_REVISION_1 : CELL_SPURS_JOB_REVISION_0);
    ps_write8(base, job_chain + jc_off::val2F, 0);
    ps_write8(base, job_chain + jc_off::tag1, static_cast<u8>(tag1));
    ps_write8(base, job_chain + jc_off::tag2, static_cast<u8>(tag2));
    ps_write8(base, job_chain + jc_off::isHalted, 0);
    ps_write16(base, job_chain + jc_off::sizeJobDescriptor, size_job);
    ps_write16(base, job_chain + jc_off::maxGrabbedJob, max_grabbed);
    ps_write8(base, job_chain + jc_off::autoReadyCount, auto_spu_count ? 1 : 0);
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
    PS_LOG_INFO(kChan, "job chain {:#010x} \"{}\" is workload {} of SPURS {:#010x} (entry {:#x}, descriptors {:#x} bytes, "
                       "max grab {}, max contention {}, {} ready count{})",
        job_chain, label, wid, spurs, entry, size_job, max_grabbed, max_contention, auto_spu_count ? "auto" : "fixed",
        halt_on_error ? ", halts on error" : "");
    return lv2::CELL_OK;
}

// cellSpursRunJobChain(jobChain), as libsre (fw 4.92): NULL, 128-byte ALIGN, a workload ID past 31 (INVAL), a job
// chain of revision 0 or 1 (PERM); then cellSpursSendWorkloadSignal on its workload (result ignored) and
// cellSpursWakeUp, whose STAT becomes JOB_ERROR_STAT. Any other wake-up result gives CELL_OK (RPCS3 passes it through).
// A worker then picks the chain up and runs it with the native job chain policy (spurs_jobchain.cpp).
s32 cellSpursRunJobChain(PPUContext& ctx, u32 job_chain)
{
    if (!job_chain) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (job_chain % 128) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, job_chain, 128)) {
        log_call_site(ctx, base, std::format("cellSpursRunJobChain: job chain {:#x} is not readable guest memory; refused "
            "with CELL_SPURS_JOB_ERROR_INVAL", job_chain));
        return error(CELL_SPURS_JOB_ERROR_INVAL);
    }
    const u32 wid = ps_read32(base, job_chain + jc_off::workloadId);
    const u32 spurs = static_cast<u32>(ps_read64(base, job_chain + jc_off::spurs));
    if (wid >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read8(base, job_chain + jc_off::jmVer) <= CELL_SPURS_JOB_REVISION_1) return error(CELL_SPURS_JOB_ERROR_PERM);
    send_workload_signal(base, spurs, wid);
    const s32 woken = cellSpursWakeUp(ctx, spurs);
    PS_LOG_DEBUG(kChan, "cellSpursRunJobChain({:#010x}): workload {} of SPURS {:#x} signalled", job_chain, wid, spurs);
    return woken == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT) ? error(CELL_SPURS_JOB_ERROR_STAT) : lv2::CELL_OK;
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
        PS_HLE_FUNC(0x8F122EF8, cellSpursTasksetAttributeSetTasksetSize),
        PS_HLE_FUNC(0xA789E631, cellSpursShutdownTaskset),
        PS_HLE_FUNC(0x69726AA2, cellSpursAddWorkload),
        PS_HLE_FUNC(0xF843818D, cellSpursReadyCountStore),
        PS_HLE_FUNC(0xC765B995, cellSpursGetWorkloadFlag),
        PS_HLE_FUNC(0xA73BF47E, _cellSpursWorkloadFlagReceiver),
        PS_HLE_FUNC(0x5EF96465, _cellSpursEventFlagInitialize),
        PS_HLE_FUNC(0x1D344406, cellSpursTaskGetLoadableSegmentPattern),
        PS_HLE_FUNC(0x7CB33C2E, cellSpursTaskGetReadOnlyAreaPattern),
        PS_HLE_FUNC(0x9197915F, cellSpursTaskGenerateLsPattern),
        PS_HLE_FUNC(0x9034E538, cellSpursTaskGetContextSaveAreaSize),
        PS_HLE_FUNC(0xB8474EFF, _cellSpursTaskAttributeInitialize),
        PS_HLE_FUNC(0xBEB600AC, cellSpursCreateTask),
        PS_HLE_FUNC(0x1D46FEDF, cellSpursCreateTaskWithAttribute),
        PS_HLE_FUNC(0x7E4EA023, cellSpursWakeUp),
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
        PS_HLE_FUNC(0xF31731BB, cellSpursRunJobChain),
    });
}

} // namespace pspocket::hle
