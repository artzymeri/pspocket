// cellSpurs, native (D-037): the SPURS API on the host. Guest-visible structures keep RPCS3's layouts
// (rpcs3/Emu/Cell/Modules/cellSpurs.h); API semantics follow RPCS3 cellSpurs.cpp where it implements them. Sony's
// libsre (fw 4.92, read on the Mac, never copied) is the authority where RPCS3 stubs a function or differs from it
// (D-042): task creation, the task attribute, the LS pattern helpers and taskset shutdown are checked against it, and
// so is every function D-171 added ("as libsre" where it differs from RPCS3).
//
// Initialize fills the CellSpurs fields RPCS3's _spurs::initialize sets that games and SPU code can observe, registers a
// host instance and starts the native scheduler (spurs_scheduler.cpp: SPURS1's 16 workloads, or SPURS2's 32). What a
// native SPURS does not create, because nothing of Sony's SPURS kernel runs: the SPU thread group and its threads (spuTG,
// spus[]: cellSpursGetSpuThreadGroupId and cellSpursGetSpuThreadId answer 0, an ID lv2 refuses), the PPU handler thread
// (ppu0), the per-workload lv2 semaphores (wklF1[].sem, semPrv) and the guest lwmutex/lwcond at 0xDB0/0xDC8. Those
// fields stay 0 (ppu0/ppu1 keep RPCS3's 0xFFFFFFFF "no thread"). The event helper thread (ppu1, its lv2 event queue,
// event port and SPU port) is created with the first workload that has a shutdown-completion hook, the only work it
// has natively: it calls the hooks (D-171).

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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

constexpr std::string_view kModule = "cellSpurs";
constexpr u32 NID_event_helper = 0x8A1D254E; // nid_of("_pspocket_spurs_event_helper"): pspocket's own

// The instance's SPURS1/SPURS2 workload count, as CellSpurs::max_workloads() (RPCS3): 32 when flags1 says so.
bool spurs2(u8* base, u32 spurs) { return (ps_read8(base, spurs + spurs_off::flags1) & SF1_32_WORKLOADS) != 0; }
u32 max_workloads(u8* base, u32 spurs) { return spurs2(base, spurs) ? CELL_SPURS_MAX_WORKLOAD2 : CELL_SPURS_MAX_WORKLOAD; }
bool workload_enabled(u8* base, u32 spurs, u32 wid)
{
    return wid < CELL_SPURS_MAX_WORKLOAD2 && (ps_read32(base, spurs + wkl_off::enabled) & (0x80000000u >> wid));
}

// A pointer argument that is not guest memory: the call is refused with `code` and the call site logged (D-042), where
// RPCS3 and libsre would fault.
s32 refuse_pointer(PPUContext& ctx, std::string_view fn, std::string_view what, u32 addr, u32 size, u32 code)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}: the {} ({:#x}, {} bytes) is not guest memory; refused with "
        "{:#x}", fn, what, addr, size, code));
    return error(code);
}

// OK when [addr, addr + size) is guest memory, else `code` with the call site logged.
s32 check_pointer(PPUContext& ctx, std::string_view fn, std::string_view what, u32 addr, u32 size, u32 code)
{
    if (guest_readable(thread_of(ctx), addr, size)) return lv2::CELL_OK;
    return refuse_pointer(ctx, fn, what, addr, size, code);
}

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

// cellSpursAttributeSetMemoryContainerForSpuThread(attr, container), as RPCS3 (libsre agrees): NULL, ALIGN (8), STAT for
// an exclusive non-context group type; then the container and SAF_SPU_MEMORY_CONTAINER_SET. The container is where
// the SPU thread group's memory would come from: a native SPURS has no group, so nothing is taken from it (initialize
// logs it).
s32 cellSpursAttributeSetMemoryContainerForSpuThread(PPUContext& ctx, u32 attr, u32 container)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    const u32 flags = ps_read32(base, attr + attr_off::flags);
    if (flags & SAF_SPU_TGT_EXCLUSIVE_NON_CONTEXT) return error(CELL_SPURS_CORE_ERROR_STAT);
    ps_write32(base, attr + attr_off::container, container);
    ps_write32(base, attr + attr_off::flags, flags | SAF_SPU_MEMORY_CONTAINER_SET);
    return lv2::CELL_OK;
}

// cellSpursAttributeEnableSpuPrintfIfAvailable(attr), as RPCS3: the flag. SPU printf output is not routed (the
// native workers have no spu_printf server to attach); SPU code's own printf channel writes are logged as before.
s32 cellSpursAttributeEnableSpuPrintfIfAvailable(PPUContext& ctx, u32 attr)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    ps_write32(base, attr + attr_off::flags, ps_read32(base, attr + attr_off::flags) | SAF_SPU_PRINTF_ENABLED);
    return lv2::CELL_OK;
}

// cellSpursAttributeEnableSystemWorkload(attr, priority[8], maxSpu, isPreemptible[8]), as RPCS3: the first of the
// instance's SPUs with priority 1 enables it (no such SPU: INVAL; no maxSpu: INVAL; a 1-SPU or exit-if-no-work
// instance: PERM; already enabled: BUSY), keeping the priorities, maxSpu and the preemptible SPUs as a mask.
s32 cellSpursAttributeEnableSystemWorkload(PPUContext& ctx, u32 attr, u32 priority, u32 max_spu, u32 is_preemptible)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 nspus = ps_read32(base, attr + attr_off::nSpus);
    if (!nspus) return error(CELL_SPURS_CORE_ERROR_INVAL);
    if (!guest_readable(t, priority, 8) || !guest_readable(t, is_preemptible, 8)) {
        log_call_site(ctx, base, std::format("cellSpursAttributeEnableSystemWorkload: priority {:#x} or isPreemptible "
            "{:#x} is not guest memory", priority, is_preemptible));
        return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    }
    for (u32 i = 0; i < std::min<u32>(nspus, 8); i++) {
        if (ps_read8(base, priority + i) != 1) continue;
        if (!max_spu) return error(CELL_SPURS_CORE_ERROR_INVAL);
        if (nspus == 1 || ps_read8(base, attr + attr_off::exitIfNoWork)) return error(CELL_SPURS_CORE_ERROR_PERM);
        const u32 flags = ps_read32(base, attr + attr_off::flags);
        if (flags & SAF_SYSTEM_WORKLOAD_ENABLED) return error(CELL_SPURS_CORE_ERROR_BUSY);
        ps_write32(base, attr + attr_off::flags, flags | SAF_SYSTEM_WORKLOAD_ENABLED);
        ps_write64(base, attr + attr_off::swlPriority, ps_read64(base, priority));
        u32 preemptible = 0;
        for (u32 j = 0; j < std::min<u32>(nspus, 8); j++)
            if (ps_read8(base, is_preemptible + j)) preemptible |= 1u << j;
        ps_write32(base, attr + attr_off::swlMaxSpu, max_spu);
        ps_write32(base, attr + attr_off::swlIsPreem, preemptible);
        return lv2::CELL_OK;
    }
    return error(CELL_SPURS_CORE_ERROR_INVAL);
}

// ------------------------------------------------------------------------------------------------- initialize

// RPCS3 _spurs::initialize, native: see the file comment for what is not created.
s32 initialize(PPUContext& ctx, u32 spurs, u32 revision, u32 sdk_version, s32 nspus, s32 spu_priority, s32 ppu_priority,
    u32 flags, u32 prefix, u32 prefix_size, u32 container)
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
        // The system workload lends SPUs to the system (cellSpursAttributeEnableSystemWorkload); there is no system
        // to lend them to here. RPCS3 adds nothing either (_spurs::add_default_syswkl is a TODO).
        PS_LOG_INFO(kChan, "cellSpursInitialize: the system workload is enabled; it does not run natively (as RPCS3)");
    }
    if (flags & SAF_SPU_MEMORY_CONTAINER_SET) {
        PS_LOG_INFO(kChan, "cellSpursInitialize: the SPU thread group's memory container {:#x} is not used (no group "
            "natively)", container);
    }

    const bool second = (flags & SAF_SECOND_VERSION) != 0;
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        if (h.instances.contains(spurs)) return error(CELL_SPURS_CORE_ERROR_STAT);
        Instance& inst = h.instances[spurs];
        inst.nspus = static_cast<u32>(nspus);
        inst.second = second;
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
    // cellSpursEnableExceptionEventHandler(spurs, true), which RPCS3's and libsre's initialize call: the flag (there
    // is no SPU thread group whose exception event it would connect).
    ps_write32(base, spurs + spurs_off::enableEH, 1);
    // Without the system workload, an instance that exits when it has no work is woken once (RPCS3 and libsre end
    // initialize with cellSpursWakeUp): the handler-dirty flag.
    if (!(flags & SAF_SYSTEM_WORKLOAD_ENABLED) && (flags & SAF_EXIT_IF_NO_WORK)) {
        ps_write8(base, spurs + spurs_off::handlerDirty, 1);
    }

    PS_LOG_INFO(kChan, "SPURS {:#010x} initialized natively: {} SPUs, SPU priority {}, PPU priority {}, flags {:#x}{}",
        spurs, nspus, spu_priority, ppu_priority, flags, second ? " (SPURS2, 32 workloads)" : "");
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

// cellSpursInitialize(spurs, nSpus, spuPriority, ppuPriority, exitIfNoWork), as RPCS3 (libsre agrees): revision and SDK
// version 0, no prefix, SAF_EXIT_IF_NO_WORK for exitIfNoWork (a b8: its low byte).
s32 cellSpursInitialize(PPUContext& ctx, u32 spurs, s32 nspus, s32 spu_priority, s32 ppu_priority, u32 exit_if_no_work)
{
    PS_LOG_INFO(kChan, "cellSpursInitialize(spurs {:#x}, nSpus {}, spuPriority {}, ppuPriority {}, exitIfNoWork {})", spurs,
        nspus, spu_priority, ppu_priority, exit_if_no_work & 0xFF);
    return initialize(ctx, spurs, 0, 0, nspus, spu_priority, ppu_priority,
        (exit_if_no_work & 0xFF) ? SAF_EXIT_IF_NO_WORK : 0, 0, 0, 0);
}

// ------------------------------------------------------------------------------------------------- event helper
//
// The SPURS event helper thread (RPCS3 _spurs::create_event_helper, event_helper_entry, stop_event_helper and
// wakeup_shutdown_completion_waiter; libsre's "SpursHdlr1"): an lv2 PPU thread at the instance's PPU priority with a 32
// KB stack, waiting on the instance's lv2 event queue "_spuPrv" (CellSpurs::eventQueue, 42 entries), which is attached
// to a dynamic SPU port (spuPort) and to a local event port (eventPort) the PPU side sends through. Natively its only
// work is the shutdown-completion hooks (cellSpursWorkloadAttributeSetShutdownCompletionEventHook): the system service
// that would send the other events (trace updates, exceptions, event port handlers) does not run. It is created with
// the first workload that has a hook, not at initialize, so instances without hooks get no thread; its entry is an
// internal function the loader links (a test that does not link it gets no helper, and a hook is then only logged).

// A hook still to be called: the shutdown is complete (event bit 0x01), a hook is registered (0x02) and it has not run
// (0x20).
bool hook_pending(u8 event) { return (event & 0x23) == 0x03; }

// The helper's work for workload `wid` (RPCS3 wakeup_shutdown_completion_waiter): RPCS3's checks (NULL, ALIGN, INVAL,
// SRCH, a workload that is not REMOVABLE: STAT), then the hook (spurs, wid, hookArg) and event bit 0x20. Natively a
// worker may still be in the workload's last job or task when the shutdown made it removable (status is never kept,
// D-136): the hook runs once the workers left it, as a join waits for them. RPCS3 then posts the workload's semaphore
// for a waiter (event bit 0x10); the native waiter sees bit 0x20 instead.
s32 call_shutdown_hook(PPUContext& ctx, u32 spurs, u32 wid)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    std::shared_ptr<Scheduler> scheduler;
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        if (const auto it = h.instances.find(spurs); it != h.instances.end()) scheduler = it->second.scheduler;
    }
    const u8 mask = wkl_nibble_mask(spurs2(base, spurs), wid);
    for (;;) {
        if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
        if (__atomic_load_n(base + spurs + wkl_state(wid), __ATOMIC_SEQ_CST) != SPURS_WKL_STATE_REMOVABLE) {
            return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
        }
        const bool left = (!scheduler || workers_inside(*scheduler, wid) == 0) &&
            !(__atomic_load_n(base + spurs + wkl_off::currentContention + (wid & 15), __ATOMIC_SEQ_CST) & mask);
        if (left) break;
        if (!lv2::sleep_us(t.waiter, 100)) throw lv2::ThreadExit{}; // the process ends
    }
    const u32 f = wkl_f(spurs, wid);
    const u32 hook = static_cast<u32>(ps_read64(base, f + wkl_off::f1_hook));
    u8* const event = base + spurs + wkl_event(wid);
    if (hook) {
        PS_LOG_INFO(kChan, "SPURS {:#x}: workload {} shut down; its completion hook {:#x} runs on the event helper", spurs,
            wid, hook);
        call_guest(ctx, hook, {spurs, wid, static_cast<u32>(ps_read64(base, f + wkl_off::f1_hook_arg))});
        const u8 before = __atomic_fetch_or(event, u8{0x20}, __ATOMIC_SEQ_CST);
        if (!(before & 0x01) || !(before & 0x02) || (before & 0x20)) {
            PS_LOG_ERROR(kChan, "SPURS {:#x}: workload {}'s event byte was {:#04x} when its hook returned (RPCS3 ensures "
                "bits 0x01 and 0x02 and not 0x20)", spurs, wid, before);
        }
    }
    return lv2::CELL_OK;
}

// The helper's body (RPCS3 _spurs::event_helper_entry): an event's data2 low 24 bits say what it is: 0 a shutdown
// completion (data3: the workloads, bit 0x80000000 >> wid for 0..15, 0x8000 >> (wid & 15) for 16..31), 1 the end
// (cellSpursFinalize). 2 (a trace update's semaphore) and 3 (the event port multiplexer's handlers) come from the
// system service, which does not run natively: logged. Ends the thread on the end event; the process ending unwinds it.
void _pspocket_spurs_event_helper(PPUContext& ctx, u64 arg)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 spurs = static_cast<u32>(arg);
    const u32 queue = ps_read32(base, spurs + spurs_off::eventQueue);
    PS_LOG_INFO(kChan, "SPURS {:#x}: event helper waiting on queue {:#x}", spurs, queue);
    for (;;) {
        if (const s32 rc = lv2::sys_event_queue_receive(t, queue, 0, 0); rc != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "SPURS {:#x}: the event helper's receive on queue {:#x} failed: {}; it ends", spurs, queue,
                lv2::error_name(rc));
            lv2::_sys_ppu_thread_exit(t, 0);
        }
        const u64 data2 = ctx.gpr[6], data3 = ctx.gpr[7];
        const u32 data0 = static_cast<u32>(data2 & 0xFFFFFF);
        if (data0 == 1) lv2::_sys_ppu_thread_exit(t, 0);
        if (data0 != 0) {
            PS_LOG_ERROR(kChan, "SPURS {:#x}: the event helper got event {} (data3 {:#x}), which nothing native sends; "
                "ignored", spurs, data0, data3);
            continue;
        }
        const u32 shutdown = static_cast<u32>(data3);
        for (u32 wid = 0; wid < CELL_SPURS_MAX_WORKLOAD2; wid++) {
            const u32 bit = wid < CELL_SPURS_MAX_WORKLOAD ? 0x80000000u >> wid : 0x8000u >> (wid & 15);
            if (!(shutdown & bit)) continue;
            if (const s32 rc = call_shutdown_hook(ctx, spurs, wid); rc != lv2::CELL_OK) {
                PS_LOG_WARN(kChan, "SPURS {:#x}: workload {}'s shutdown completion: {:#x} (RPCS3 ensures CELL_OK; the "
                    "workload was removed or replaced before its hook could run)", spurs, wid, static_cast<u32>(rc));
            }
        }
    }
}

// Creates the event helper for the instance if it has none (see above): the "_spuPrv" queue on a dynamic SPU port,
// the event port connected to it, the thread. Called on a PPU thread; failures leave the instance without a helper and
// are logged (its hooks are then not called).
void ensure_event_helper(PPUContext& ctx, u32 spurs)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    SpursHost& h = host();
    {
        std::lock_guard lock(h.mutex);
        const auto it = h.instances.find(spurs);
        if (it == h.instances.end() || it->second.helper || it->second.helper_failed) return;
    }
    HleProcess& p = *HleProcess::current();
    const auto give_up = [&](std::string_view why) {
        PS_LOG_WARN(kChan, "SPURS {:#x}: no event helper ({}): shutdown-completion hooks are not called", spurs, why);
        std::lock_guard lock(h.mutex);
        if (const auto it = h.instances.find(spurs); it != h.instances.end()) it->second.helper_failed = true;
    };
    const u32 entry = p.descriptor_of(kModule, NID_event_helper);
    if (!entry) return give_up("its function was not linked at load");
    StackVar v(ctx, base, 0x60); // queue ID, port ID, the queue attribute (0x10), thread ID (0x28), param (0x30), name
    const u32 queue_out = v.addr(), port_out = v.addr() + 4, qattr = v.addr() + 0x10, thread_out = v.addr() + 0x28,
              param = v.addr() + 0x30, name = v.addr() + 0x38;
    ps_write32(base, qattr, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, qattr + 4, lv2::SYS_PPU_QUEUE);
    std::memcpy(base + qattr + 8, "_spuPrv", 8);
    if (const s32 rc = lv2::sys_event_queue_create(t, queue_out, qattr, 0, 0x2A); rc != lv2::CELL_OK) {
        return give_up(std::format("the event queue: {}", lv2::error_name(rc)));
    }
    const u32 queue = ps_read32(base, queue_out);
    u8 spu_port = 0;
    if (const s32 rc = attach_lv2_eq(ctx, spurs, queue, spu_port, true, true); rc != lv2::CELL_OK) {
        lv2::sys_event_queue_destroy(t, queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
        return give_up(std::format("the SPU port: {:#x}", static_cast<u32>(rc)));
    }
    const auto undo_queue = [&] {
        detach_lv2_eq(base, spurs, spu_port, true, t.kernel.config().sdk_version);
        lv2::sys_event_queue_destroy(t, queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
    };
    if (const s32 rc = lv2::sys_event_port_create(t, port_out, lv2::SYS_EVENT_PORT_LOCAL, 0); rc != lv2::CELL_OK) {
        undo_queue();
        return give_up(std::format("the event port: {}", lv2::error_name(rc)));
    }
    const u32 port = ps_read32(base, port_out);
    if (const s32 rc = lv2::sys_event_port_connect_local(t, port, queue); rc != lv2::CELL_OK) {
        lv2::sys_event_port_destroy(t, port);
        undo_queue();
        return give_up(std::format("connecting the event port: {}", lv2::error_name(rc)));
    }
    ps_write32(base, spurs + spurs_off::eventQueue, queue);
    ps_write32(base, spurs + spurs_off::eventPort, port);
    ps_write8(base, spurs + spurs_off::spuPort, spu_port);
    const auto undo_ports = [&] {
        lv2::sys_event_port_disconnect(t, port);
        lv2::sys_event_port_destroy(t, port);
        undo_queue();
        ps_write32(base, spurs + spurs_off::eventQueue, 0);
        ps_write32(base, spurs + spurs_off::eventPort, 0);
        ps_write8(base, spurs + spurs_off::spuPort, 0);
    };
    const u32 tls = p.alloc_tls();
    if (!tls) {
        undo_ports();
        return give_up("no TLS block");
    }
    ps_write32(base, param, entry);
    ps_write32(base, param + 4, tls + kTlsR13Offset);
    const u8 prefix_size = std::min<u8>(ps_read8(base, spurs + spurs_off::prefixSize), CELL_SPURS_NAME_MAX_LENGTH);
    std::string thread_name(reinterpret_cast<const char*>(base + spurs + spurs_off::prefix), prefix_size);
    thread_name = thread_name.c_str(); // the prefix is not NUL-terminated, and may hold NULs
    thread_name += "SpursHdlr1";
    thread_name.resize(std::min<usize>(thread_name.size(), 0x1F));
    std::memset(base + name, 0, 0x20);
    std::memcpy(base + name, thread_name.data(), thread_name.size());
    const s32 prio = static_cast<s32>(ps_read32(base, spurs + spurs_off::ppuPriority));
    s32 rc = lv2::_sys_ppu_thread_create(t, thread_out, param, spurs, 0, prio, 0x8000, 1 /* joinable */, name);
    if (rc == lv2::CELL_OK) rc = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, thread_out)));
    if (rc != lv2::CELL_OK) {
        p.free_tls(tls);
        undo_ports();
        return give_up(std::format("the thread: {}", lv2::error_name(rc)));
    }
    const u32 thread = static_cast<u32>(ps_read64(base, thread_out));
    ps_write64(base, spurs + spurs_off::ppu1, thread);
    std::lock_guard lock(h.mutex);
    if (const auto it = h.instances.find(spurs); it != h.instances.end()) {
        it->second.helper = thread;
        it->second.helper_tls = tls;
    }
    PS_LOG_INFO(kChan, "SPURS {:#x}: event helper {:#x} \"{}\" (queue {:#x}, SPU port {:#x}, event port {:#x}) for the "
        "shutdown-completion hooks", spurs, thread, thread_name, queue, spu_port, port);
}

// RPCS3 _spurs::stop_event_helper: the end event through the event port, the join, then the port, the SPU port and the
// queue go, and ppu1 is "no thread" again.
void stop_event_helper(PPUContext& ctx, u32 spurs, u32 thread, u32 tls)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 port = ps_read32(base, spurs + spurs_off::eventPort), queue = ps_read32(base, spurs + spurs_off::eventQueue);
    if (const s32 rc = lv2::sys_event_port_send(t, port, 0, 1, 0); rc != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "SPURS {:#x}: the event helper's end event: {}", spurs, lv2::error_name(rc));
    }
    {
        StackVar status(ctx, base, 8);
        if (const s32 rc = lv2::sys_ppu_thread_join(t, thread, status.addr()); rc != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "SPURS {:#x}: joining the event helper {:#x}: {}", spurs, thread, lv2::error_name(rc));
        }
    }
    HleProcess::current()->free_tls(tls);
    ps_write64(base, spurs + spurs_off::ppu1, 0xFFFFFFFF);
    lv2::sys_event_port_disconnect(t, port);
    lv2::sys_event_port_destroy(t, port);
    detach_lv2_eq(base, spurs, ps_read8(base, spurs + spurs_off::spuPort), true, t.kernel.config().sdk_version);
    lv2::sys_event_queue_destroy(t, queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
}

// A workload's shutdown is complete: the event to the helper when the workload's event byte had a hook or a waiter
// (0x12) and no completion before (0x01), as RPCS3's cellSpursShutdownWorkload and the system service send it. Without
// a helper there is no thread to run a hook on: logged. CELL_OK, or CORE_ERROR_STAT when the send fails (RPCS3).
s32 notify_shutdown_completion(lv2::PpuThread& t, u32 spurs, u32 wid, u8 event_before)
{
    if (!(event_before & 0x12) || (event_before & 0x01)) return lv2::CELL_OK;
    u8* base = t.kernel.base();
    u32 helper = 0;
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        if (const auto it = h.instances.find(spurs); it != h.instances.end()) helper = it->second.helper;
    }
    if (!helper) {
        if (event_before & 0x02) {
            PS_LOG_WARN(kChan, "workload {} of SPURS {:#010x} shut down: its completion hook is not called (no event "
                "helper)", wid, spurs);
        }
        return lv2::CELL_OK;
    }
    const u32 mask = wid < CELL_SPURS_MAX_WORKLOAD ? 0x80000000u >> wid : 0x8000u >> (wid & 15);
    if (const s32 rc = lv2::sys_event_port_send(t, ps_read32(base, spurs + spurs_off::eventPort), 0, 0, mask);
        rc != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "workload {} of SPURS {:#010x}: the shutdown completion event: {}", wid, spurs,
            lv2::error_name(rc));
        return error(CELL_SPURS_CORE_ERROR_STAT);
    }
    return lv2::CELL_OK;
}

// cellSpursSetGlobalExceptionEventHandler(spurs, eaHandler, arg), as RPCS3: NULL, ALIGN, STAT (an instance in
// exception state), BUSY (a handler already set); else the handler and its argument are kept. SPU exceptions stop a
// native worker with a log (CLAUDE.md rule 5) rather than reaching the handler.
s32 cellSpursSetGlobalExceptionEventHandler(PPUContext& ctx, u32 spurs, u32 handler, u32 arg)
{
    if (!spurs || !handler) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_CORE_ERROR_STAT);
    s32 rc = lv2::CELL_OK;
    reservation_op(base, spurs + spurs_off::globalSpuExceptionHandler, [&](u8* line) { // a 128-byte line of its own
        rc = lv2::CELL_OK;
        if (ps_read64(line, 0)) {
            rc = error(CELL_SPURS_CORE_ERROR_BUSY);
            return false;
        }
        ps_write64(line, 8, arg);
        ps_write64(line, 0, handler);
        return true;
    });
    PS_LOG_INFO(kChan, "cellSpursSetGlobalExceptionEventHandler(SPURS {:#x}, handler {:#x}, arg {:#x}) -> {:#x}", spurs,
        handler, arg, static_cast<u32>(rc));
    return rc;
}

// cellSpursUnsetGlobalExceptionEventHandler(spurs), as RPCS3 (libsre agrees): NULL, ALIGN, STAT (an instance in
// exception state); then the argument and the handler cleared, in one update of their line.
s32 cellSpursUnsetGlobalExceptionEventHandler(PPUContext& ctx, u32 spurs)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_CORE_ERROR_STAT);
    reservation_op(base, spurs + spurs_off::globalSpuExceptionHandler, [](u8* line) {
        ps_write64(line, 8, 0);
        ps_write64(line, 0, 0);
        return true;
    });
    return lv2::CELL_OK;
}

// cellSpursEnableExceptionEventHandler(spurs, flag), as RPCS3 (libsre agrees): NULL, ALIGN, then enableEH exchanged for
// flag (1 or 0). RPCS3 then connects (or disconnects) the SPU thread group's exception event to the instance's queue
// when the flag changes; a native SPURS has no group, and an SPU exception stops its worker with a log (CLAUDE.md
// rule 5), so only the flag changes. CELL_OK.
s32 cellSpursEnableExceptionEventHandler(PPUContext& ctx, u32 spurs, u32 flag)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    const u32 old = atomic_exchange_be32(guest_base(ctx), spurs + spurs_off::enableEH, (flag & 0xFF) ? 1 : 0);
    PS_LOG_INFO(kChan, "cellSpursEnableExceptionEventHandler(SPURS {:#x}, {}): was {}", spurs, flag & 0xFF, old);
    return lv2::CELL_OK;
}

// cellSpursSetPreemptionVictimHints(spurs, isPreemptible), as libsre (0xF6AC): NULL (both), ALIGN, PERM without the
// system workload (SAF_SYSTEM_WORKLOAD_ENABLED); then a bit for each of the instance's SPUs whose byte of the array is
// not 0 goes to sys_spu_thread_group_set_cooperative_victims on its SPU thread group, whose error libsre only prints.
// The native scheduler has no SPU thread group nor victims to choose: the hints change nothing. CELL_OK.
s32 cellSpursSetPreemptionVictimHints(PPUContext& ctx, u32 spurs, u32 is_preemptible)
{
    if (!spurs || !is_preemptible) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (!(ps_read32(base, spurs + spurs_off::flags) & SAF_SYSTEM_WORKLOAD_ENABLED)) return error(CELL_SPURS_CORE_ERROR_PERM);
    const u32 n = ps_read8(base, spurs + spurs_off::nSpus);
    u32 mask = 0;
    if (const s32 rc = check_pointer(ctx, "cellSpursSetPreemptionVictimHints", "array", is_preemptible, n ? n : 1,
            CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    for (u32 i = 0; i < n; i++)
        if (ps_read8(base, is_preemptible + i)) mask |= 1u << i;
    PS_LOG_DEBUG(kChan, "cellSpursSetPreemptionVictimHints(SPURS {:#x}): SPUs {:#x} preemptible (no effect natively)",
        spurs, mask);
    return lv2::CELL_OK;
}

// cellSpurs 0xE279681F, a libsre export no SDK names (0xF664): NULL (both), ALIGN, then the word at CellSpurs + 0x10
// (the first four idle SPU counts, or SPURS2's ready counts of workloads 16..19).
s32 cellSpurs_E279681F(PPUContext& ctx, u32 spurs, u32 out)
{
    if (!spurs || !out) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpurs_E279681F", "output", out, 4, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, out, ps_read32(base, spurs + 0x10));
    return lv2::CELL_OK;
}

// cellSpursInitializeForSpuSharing(spurs, nSpus, spuPriority, ppuPriority, exitIfNoWork, a, b), as libsre (0x8328;
// RPCS3 has none): libsre's initialize with revision and SDK version 0 and the flags exitIfNoWork | 0x80000000 with
// `a` | 0x2 with `b` (RPCS3's SAF_UNKNOWN_FLAG_0 and _30, which choose the system's SPU thread group types there). A
// native SPURS has no SPU thread group: the flags are kept in CellSpurs::flags, as libsre keeps them.
s32 cellSpursInitializeForSpuSharing(PPUContext& ctx, u32 spurs, s32 nspus, s32 spu_priority, s32 ppu_priority,
    u32 exit_if_no_work, u32 a, u32 b)
{
    u32 flags = exit_if_no_work;
    if (a) flags |= 0x80000000u;
    if (b) flags |= 0x2;
    return initialize(ctx, spurs, 0, 0, nspus, spu_priority, ppu_priority, flags, 0, 0, 0);
}

// cellSpurs 0x5C3A614C (libsre 0x714C), an attribute setter no SDK names: NULL, the attribute's 8-byte ALIGN, STAT
// when SPU sharing (flag 0x00800000 or 0x01000000, 0xB55782F9) is set; then flag 0x80000000, and 0x2 with `b`.
s32 cellSpurs_5C3A614C(PPUContext& ctx, u32 attr, u32 b)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpurs_5C3A614C", "attribute", attr, attr_off::size, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const u32 flags = ps_read32(base, attr + attr_off::flags);
    if (flags & (0x00800000u | 0x01000000u)) return error(CELL_SPURS_CORE_ERROR_STAT);
    ps_write32(base, attr + attr_off::flags, flags | 0x80000000u | (b ? 0x2u : 0u));
    return lv2::CELL_OK;
}

// cellSpurs 0xB55782F9 (libsre 0x728C), another: NULL, ALIGN, PERM for more than one SPU, STAT with flag 0x80000000
// (0x5C3A614C); then flag 0x00800000 with `a` (else 0x01000000), 0x00400000 with `b`, and 0x2.
s32 cellSpurs_B55782F9(PPUContext& ctx, u32 attr, u32 a, u32 b)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpurs_B55782F9", "attribute", attr, attr_off::size, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, attr + attr_off::nSpus) > 1) return error(CELL_SPURS_CORE_ERROR_PERM);
    u32 flags = ps_read32(base, attr + attr_off::flags);
    if (flags & 0x80000000u) return error(CELL_SPURS_CORE_ERROR_STAT);
    flags |= a ? 0x00800000u : 0x01000000u;
    if (b) flags |= 0x00400000u;
    ps_write32(base, attr + attr_off::flags, flags | 0x2);
    return lv2::CELL_OK;
}

// cellSpurs 0xBAC24478 (libsre 0x108A8) and 0x7025A5EC (0x10970), which no SDK names: NULL, ALIGN, PERM unless the
// instance shares its SPUs (0x00800000 or 0x01000000); then lv2 system calls 248 and 249 on its SPU thread group, two
// calls RPCS3 does not name either (root only). Their EAGAIN becomes INVAL, their EBUSY STAT, anything else is
// returned as it is: here lv2's own answer.
s32 cellSpurs_BAC24478(PPUContext& ctx, u32 spurs, u32 a, u32 b, u32 c, u32 d)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (!(ps_read32(base, spurs + spurs_off::flags) & (0x00800000u | 0x01000000u))) return error(CELL_SPURS_CORE_ERROR_PERM);
    const u32 rc = static_cast<u32>(lv2_syscall(ctx, 248, {ps_read32(base, spurs + spurs_off::spuTG), a, b, c, d}));
    if (rc == 0x80010002) return error(CELL_SPURS_CORE_ERROR_INVAL);
    if (rc == 0x8001000F) return error(CELL_SPURS_CORE_ERROR_STAT);
    return static_cast<s32>(rc);
}

s32 cellSpurs_7025A5EC(PPUContext& ctx, u32 spurs)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (!(ps_read32(base, spurs + spurs_off::flags) & (0x00800000u | 0x01000000u))) return error(CELL_SPURS_CORE_ERROR_PERM);
    const u32 rc = static_cast<u32>(lv2_syscall(ctx, 249, {ps_read32(base, spurs + spurs_off::spuTG)}));
    if (rc == 0x8001000F) return error(CELL_SPURS_CORE_ERROR_STAT);
    return static_cast<s32>(rc);
}

// cellSpursInitializeWithAttribute and ...WithAttribute2, as RPCS3 (libsre agrees): NULL, ALIGN (8), a revision above
// 2 (INVAL); then initialize with the attribute's values, its flags with exitIfNoWork's and, for the second,
// SAF_SECOND_VERSION: a SPURS2 instance of 32 workloads (8 KB).
s32 initialize_with_attribute(PPUContext& ctx, u32 spurs, u32 attr, bool second)
{
    if (!attr) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, second ? "cellSpursInitializeWithAttribute2" : "cellSpursInitializeWithAttribute",
            "attribute", attr, attr_off::size, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const u32 revision = ps_read32(base, attr + attr_off::revision);
    if (revision > 2) return error(CELL_SPURS_CORE_ERROR_INVAL);
    const u32 flags = ps_read32(base, attr + attr_off::flags) |
        (ps_read8(base, attr + attr_off::exitIfNoWork) ? SAF_EXIT_IF_NO_WORK : 0) | (second ? SAF_SECOND_VERSION : 0);
    return initialize(ctx, spurs, revision, ps_read32(base, attr + attr_off::sdkVersion),
        static_cast<s32>(ps_read32(base, attr + attr_off::nSpus)), static_cast<s32>(ps_read32(base, attr + attr_off::spuPriority)),
        static_cast<s32>(ps_read32(base, attr + attr_off::ppuPriority)), flags, attr + attr_off::prefix,
        ps_read32(base, attr + attr_off::prefixSize), ps_read32(base, attr + attr_off::container));
}

s32 cellSpursInitializeWithAttribute(PPUContext& ctx, u32 spurs, u32 attr)
{
    return initialize_with_attribute(ctx, spurs, attr, false);
}

s32 cellSpursInitializeWithAttribute2(PPUContext& ctx, u32 spurs, u32 attr)
{
    PS_LOG_INFO(kChan, "cellSpursInitializeWithAttribute2(spurs {:#x}, attr {:#x})", spurs, attr);
    return initialize_with_attribute(ctx, spurs, attr, true);
}

// RPCS3's cellSpursFinalize is a TODO after its argument checks; here the event helper (if any) and the workers stop
// and the host instance is released.
s32 cellSpursFinalize(PPUContext& ctx, u32 spurs)
{
    PS_LOG_INFO(kChan, "cellSpursFinalize({:#x})", spurs);
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (ps_read8(guest_base(ctx), spurs + spurs_off::handlerExiting)) return error(CELL_SPURS_CORE_ERROR_STAT);
    SpursHost& h = host();
    std::shared_ptr<Scheduler> scheduler;
    u32 helper = 0, helper_tls = 0;
    {
        std::lock_guard lock(h.mutex);
        const auto it = h.instances.find(spurs);
        if (it == h.instances.end()) return error(CELL_SPURS_CORE_ERROR_STAT);
        scheduler = it->second.scheduler;
        helper = it->second.helper;
        helper_tls = it->second.helper_tls;
        it->second.helper = 0;
    }
    if (helper) stop_event_helper(ctx, spurs, helper, helper_tls);
    // The workers stop before the instance goes: they take the host lock while they finish.
    if (scheduler) stop_scheduler_of(*scheduler);
    std::lock_guard lock(h.mutex);
    h.instances.erase(spurs);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- information

// The native SPURS has no SPU thread group or SPU threads: the IDs a game asks for are CellSpurs::spuTG and spus[],
// which stay 0, an ID lv2 refuses (ESRCH). Logged the first time.
void note_no_thread_group(std::string_view fn, u32 spurs)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        PS_LOG_WARN(kChan, "{}(SPURS {:#x}): a native SPURS has no SPU thread group or SPU threads; their IDs are 0 "
            "(logged once)", fn, spurs);
    }
}

// cellSpursGetInfo(spurs, info), as libsre (RPCS3 agrees but for its TODO of syscall 253): NULL (spurs, info), ALIGN;
// then CellSpursInfo's fields from CellSpurs: SPU and PPU priorities, exitIfNoWork and spurs2 from the flags, the trace
// buffer (its low two bits are the trace mode), size, the thread group and threads (0 natively), the handler threads,
// the name prefix (NUL-terminated) and its length. The deadline counters come from sys_spu_thread_group_syscall_253 for
// an instance with the system workload; without a group natively they are 0, as without it.
s32 cellSpursGetInfo(PPUContext& ctx, u32 spurs, u32 info)
{
    if (!spurs || !info) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursGetInfo", "CellSpursInfo", info, 0x74, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const u32 flags = ps_read32(base, spurs + spurs_off::flags);
    ps_write32(base, info + 0x00, ps_read8(base, spurs + spurs_off::nSpus));
    ps_write32(base, info + 0x04, ps_read32(base, spurs + spurs_off::spuPriority));
    ps_write32(base, info + 0x08, ps_read32(base, spurs + spurs_off::ppuPriority));
    ps_write8(base, info + 0x0C, (flags & SAF_EXIT_IF_NO_WORK) ? 1 : 0);
    ps_write8(base, info + 0x0D, (flags & SAF_SECOND_VERSION) ? 1 : 0);
    const u32 trace = static_cast<u32>(ps_read64(base, spurs + spurs_off::traceBuffer));
    ps_write32(base, info + 0x10, trace & ~3u);
    ps_write64(base, info + 0x18, ps_read64(base, spurs + spurs_off::traceDataSize));
    ps_write32(base, info + 0x20, trace & 3u);
    ps_write32(base, info + 0x24, ps_read32(base, spurs + spurs_off::spuTG));
    for (u32 i = 0; i < CELL_SPURS_MAX_SPU; i++) ps_write32(base, info + 0x28 + 4 * i, ps_read32(base, spurs + spurs_off::spus + 4 * i));
    ps_write64(base, info + 0x48, ps_read64(base, spurs + spurs_off::ppu0));
    ps_write64(base, info + 0x50, ps_read64(base, spurs + spurs_off::ppu1));
    const u8 prefix_size = std::min<u8>(ps_read8(base, spurs + spurs_off::prefixSize), CELL_SPURS_NAME_MAX_LENGTH);
    std::memcpy(base + info + 0x58, base + spurs + spurs_off::prefix, prefix_size);
    ps_write8(base, info + 0x58 + prefix_size, 0);
    ps_write32(base, info + 0x68, prefix_size);
    ps_write32(base, info + 0x6C, 0); // deadlineMissCounter
    ps_write32(base, info + 0x70, 0); // deadlineMeetCounter
    return lv2::CELL_OK;
}

// cellSpursGetNumSpuThread(spurs, nThreads), as RPCS3 (libsre agrees): NULL (both), ALIGN, then the instance's SPU count.
s32 cellSpursGetNumSpuThread(PPUContext& ctx, u32 spurs, u32 nthreads)
{
    if (!spurs || !nthreads) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursGetNumSpuThread", "count", nthreads, 4, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, nthreads, ps_read8(base, spurs + spurs_off::nSpus));
    return lv2::CELL_OK;
}

// cellSpursGetSpuThreadGroupId(spurs, group), as RPCS3 (libsre agrees): NULL (both), ALIGN, then CellSpurs::spuTG: 0
// natively, which no lv2 object has (see note_no_thread_group).
s32 cellSpursGetSpuThreadGroupId(PPUContext& ctx, u32 spurs, u32 group)
{
    if (!spurs || !group) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursGetSpuThreadGroupId", "group ID", group, 4,
            CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    note_no_thread_group("cellSpursGetSpuThreadGroupId", spurs);
    ps_write32(base, group, ps_read32(base, spurs + spurs_off::spuTG));
    return lv2::CELL_OK;
}

// cellSpursGetSpuThreadId(spurs, thread, nThreads), as RPCS3 (libsre agrees): NULL (all three), ALIGN; then the first
// min(*nThreads, nSpus) of CellSpurs::spus[] (0 natively, see note_no_thread_group) and that count in *nThreads.
s32 cellSpursGetSpuThreadId(PPUContext& ctx, u32 spurs, u32 thread, u32 nthreads)
{
    constexpr std::string_view fn = "cellSpursGetSpuThreadId";
    if (!spurs || !thread || !nthreads) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "count", nthreads, 4, CELL_SPURS_CORE_ERROR_NULL_POINTER); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const u32 count = std::min<u32>(ps_read32(base, nthreads), ps_read8(base, spurs + spurs_off::nSpus));
    if (const s32 rc = check_pointer(ctx, fn, "thread IDs", thread, 4 * count, CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    note_no_thread_group(fn, spurs);
    for (u32 i = 0; i < count; i++) ps_write32(base, thread + 4 * i, ps_read32(base, spurs + spurs_off::spus + 4 * i));
    ps_write32(base, nthreads, count);
    return lv2::CELL_OK;
}

// cellSpursGetSpuGuid(guid, dst), as libsre (RPCS3 checks the pointers and writes nothing): NULL (guid, dst), dst
// 8-byte ALIGN; the 16 bytes at guid must be the .SpuGUID section's four SPU instructions `ila $2, (data << 2) | i`
// for i = 0..3 (each byte masked as libsre does: 0xFE 0x00 0x01 0xFF; INVAL otherwise); the GUID is their four 16-bit
// data fields, in order, as a big-endian u64.
s32 cellSpursGetSpuGuid(PPUContext& ctx, u32 guid, u32 dst)
{
    constexpr std::string_view fn = "cellSpursGetSpuGuid";
    if (!guid || !dst) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (dst % 8) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "GUID section", guid, 16, CELL_SPURS_CORE_ERROR_NULL_POINTER); rc != lv2::CELL_OK) {
        return rc;
    }
    if (const s32 rc = check_pointer(ctx, fn, "destination", dst, 8, CELL_SPURS_CORE_ERROR_NULL_POINTER); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    static constexpr u8 kMask[4] = {0xFE, 0x00, 0x01, 0xFF};
    static constexpr u8 kPattern[16] = {0x42, 0x00, 0x00, 0x02, 0x42, 0x00, 0x00, 0x82,
                                        0x42, 0x00, 0x01, 0x02, 0x42, 0x00, 0x01, 0x82};
    for (u32 i = 0; i < 16; i++) {
        if ((ps_read8(base, guid + i) & kMask[i & 3]) != kPattern[i]) return error(CELL_SPURS_CORE_ERROR_INVAL);
    }
    u64 value = 0;
    for (u32 i = 0; i < 4; i++) value = (value << 16) | ((ps_read32(base, guid + 4 * i) >> 9) & 0xFFFF);
    ps_write64(base, dst, value);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- trace
//
// SPURS traces (RPCS3 _spurs::trace_initialize, trace_start, trace_stop and their exported functions; libsre agrees):
// the PPU side keeps the buffer, its mode and the per-SPU start indices in CellSpurs, and the kernel's system service
// writes the packets. No system service runs natively, so a trace buffer stays empty but for the header written here.

constexpr u32 CELL_SPURS_TRACE_MODE_FLAG_WRAP_BUFFER = 0x1, CELL_SPURS_TRACE_MODE_FLAG_SYNCHRONOUS_START_STOP = 0x2,
              CELL_SPURS_TRACE_MODE_FLAG_MASK = 0x3;
constexpr u32 kTraceInfoSize = 0x80, kTracePacketSize = 0x10; // CellSpursTraceInfo, CellSpursTracePacket

// RPCS3 _spurs::trace_status_update: once the system service has set the trace up on the SPUs
// (sysSrvTraceInitialised), it is told to update (notify-complete and one message bit per SPU) and the caller waits on
// the instance's semaphore for it. The native system service never sets it up, so this changes nothing; were the flag
// set (a game writing it), there would be nobody to answer the wait: logged instead.
void trace_status_update(u8* base, u32 spurs)
{
    bool initialised = false;
    reservation_op(base, spurs + spurs_off::sysSrvTrace, [&](u8* line) {
        const u32 off = spurs_off::sysSrvTrace % PS_RES_LINE_SIZE;
        initialised = ps_read8(line, off) != 0;
        if (!initialised) return false;
        ps_write8(line, off + 1, 1);
        ps_write8(line, off + 2, static_cast<u8>((1u << ps_read8(base, spurs + spurs_off::nSpus)) - 1));
        return true;
    });
    if (initialised) {
        PS_LOG_WARN(kChan, "SPURS {:#x}: a trace update for a system service that does not run natively; not waited for",
            spurs);
    }
}

// RPCS3 _spurs::trace_initialize: NULL (spurs, buffer), ALIGN (spurs 128, buffer 16), INVAL (a size below the
// CellSpursTraceInfo header, a mode outside the two flags), STAT (a buffer already); then the data size, the header
// (the SPU threads and group: 0 natively; the counts 0; the SPU count), the buffer with the wrap flag in its low bit, the
// mode, each SPU's start index (8 + i * the packets per SPU, for six SPUs) and the trace control 0.
s32 trace_initialize(PPUContext& ctx, u32 spurs, u32 buffer, u32 size, u32 mode, bool update)
{
    if (!spurs || !buffer) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || buffer % 16) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    if (size < kTraceInfoSize || (mode & ~CELL_SPURS_TRACE_MODE_FLAG_MASK)) return error(CELL_SPURS_CORE_ERROR_INVAL);
    u8* base = guest_base(ctx);
    if (ps_read64(base, spurs + spurs_off::traceBuffer)) return error(CELL_SPURS_CORE_ERROR_STAT);
    if (const s32 rc = check_pointer(ctx, "cellSpursTraceInitialize", "trace buffer", buffer, kTraceInfoSize,
            CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    const u32 data_size = size - kTraceInfoSize;
    ps_write64(base, spurs + spurs_off::traceDataSize, data_size);
    for (u32 i = 0; i < 8; i++) {
        ps_write32(base, buffer + 4 * i, ps_read32(base, spurs + spurs_off::spus + 4 * i));
        ps_write32(base, buffer + 0x20 + 4 * i, 0);
    }
    ps_write32(base, buffer + 0x40, ps_read32(base, spurs + spurs_off::spuTG));
    const u32 nspus = ps_read8(base, spurs + spurs_off::nSpus);
    ps_write32(base, buffer + 0x44, nspus);
    ps_write64(base, spurs + spurs_off::traceBuffer, buffer | ((mode & CELL_SPURS_TRACE_MODE_FLAG_WRAP_BUFFER) ? 1 : 0));
    ps_write32(base, spurs + spurs_off::traceMode, mode);
    const u32 per_spu = nspus ? data_size / kTracePacketSize / nspus : 0;
    for (u32 i = 0, j = 8; i < 6; i++, j += per_spu) ps_write32(base, spurs + spurs_off::traceStartIndex + 4 * i, j);
    ps_write8(base, spurs + spurs_off::sysSrvTraceControl, 0);
    if (update) trace_status_update(base, spurs);
    PS_LOG_INFO(kChan, "cellSpursTraceInitialize(SPURS {:#x}): buffer {:#x}, {:#x} bytes, mode {}; the native kernel "
        "writes no packets", spurs, buffer, size, mode);
    return lv2::CELL_OK;
}

// cellSpursTraceInitialize(spurs, buffer, size, mode), as RPCS3: STAT while libprof is loaded (never natively), then
// trace_initialize with the status update.
s32 cellSpursTraceInitialize(PPUContext& ctx, u32 spurs, u32 buffer, u32 size, u32 mode)
{
    return trace_initialize(ctx, spurs, buffer, size, mode, true);
}

// cellSpursTraceFinalize(spurs), as RPCS3: NULL, ALIGN, STAT without a buffer; then the control, the mode and the buffer
// cleared, and the status update.
s32 cellSpursTraceFinalize(PPUContext& ctx, u32 spurs)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (!ps_read64(base, spurs + spurs_off::traceBuffer)) return error(CELL_SPURS_CORE_ERROR_STAT);
    ps_write8(base, spurs + spurs_off::sysSrvTraceControl, 0);
    ps_write32(base, spurs + spurs_off::traceMode, 0);
    ps_write64(base, spurs + spurs_off::traceBuffer, 0);
    trace_status_update(base, spurs);
    return lv2::CELL_OK;
}

// RPCS3 _spurs::trace_start (control 1) and trace_stop (control 2), and cellSpursTraceStart and cellSpursTraceStop:
// NULL, ALIGN, STAT without a buffer; the status update only in the synchronous start/stop mode.
s32 trace_control(PPUContext& ctx, u32 spurs, u8 control)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (!ps_read64(base, spurs + spurs_off::traceBuffer)) return error(CELL_SPURS_CORE_ERROR_STAT);
    ps_write8(base, spurs + spurs_off::sysSrvTraceControl, control);
    if (ps_read32(base, spurs + spurs_off::traceMode) & CELL_SPURS_TRACE_MODE_FLAG_SYNCHRONOUS_START_STOP) {
        trace_status_update(base, spurs);
    }
    return lv2::CELL_OK;
}

s32 cellSpursTraceStart(PPUContext& ctx, u32 spurs) { return trace_control(ctx, spurs, 1); }
s32 cellSpursTraceStop(PPUContext& ctx, u32 spurs) { return trace_control(ctx, spurs, 2); }

// ------------------------------------------------------------------------------------------------- workloads

// A workload's priorities on SPUs 0..7 for logs ("1 1 1 1 1 0 0 0"; 1 is the highest, 0 not on that SPU).
std::string priority_text(const u8 (&priority)[8])
{
    std::string out;
    for (u32 i = 0; i < 8; i++) out += std::format("{}{}", i ? " " : "", priority[i]);
    return out;
}

// RPCS3 _spurs::add_workload: claims the first free workload ID (0..15, or 0..31 for SPURS2), fills its tables in the
// guest CellSpurs (SPURS2's workloads 16..31 in the second tables and the high nibbles), gives it the unique ID of an
// existing workload with the same policy module (or the lowest free one) and makes it runnable. The native scheduler
// finds the workload through the host instance. The updates of CellSpurs' first two lines go through the reservation
// system, so workers selecting meanwhile see them whole; the state turns RUNNABLE last.
s32 add_workload(u8* base, u32 spurs, u32& wid_out, u32 pm, u32 size, u64 data, const u8 (&priority)[8], u32 min_contention,
    u32 max_contention, u32 name_class, u32 name_instance, Workload::Kind kind, u32 hook = 0, u32 hook_arg = 0)
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
    const bool second = spurs2(base, spurs);
    const u32 wmax = second ? CELL_SPURS_MAX_WORKLOAD2 : CELL_SPURS_MAX_WORKLOAD;

    constexpr u32 kLine1 = 0x80; // states, statuses, events, wklEnabled, wklMskB, sysSrvMsgUpdateWorkload (both tables)
    u32 wid = CELL_SPURS_MAX_WORKLOAD2;
    reservation_op(base, spurs + kLine1, [&](u8* line) {
        const u32 enabled = ps_read32(line, wkl_off::enabled - kLine1);
        wid = static_cast<u32>(std::countl_one(enabled));
        if (wid >= wmax) return false;
        ps_write32(line, wkl_off::enabled - kLine1, enabled | (0x80000000u >> wid));
        return true;
    });
    wid_out = wid;
    if (wid >= wmax) return error(CELL_SPURS_POLICY_MODULE_ERROR_AGAIN);
    const u32 j = wid & 15;
    // RPCS3 ensure()s a free workload slot has no SPUs on it or switching to it (its nibble in SPURS2). A native worker
    // that selected the slot's previous workload just before its join removed it counts itself on the slot until its
    // next selection, which comes at once, since the workload is gone (spurs_scheduler.cpp; D-136): that is waited
    // for, up to a second.
    const u8 nibble = wid < CELL_SPURS_MAX_WORKLOAD ? 0x0F : 0xF0;
    const auto has_contention = [&] {
        return (__atomic_load_n(base + spurs + wkl_off::currentContention + j, __ATOMIC_SEQ_CST) & nibble) ||
               (ps_read8(base, spurs + wkl_off::pendingContention + j) & nibble);
    };
    for (const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
         has_contention() && std::chrono::steady_clock::now() < end;) {
        std::this_thread::yield();
    }
    if (has_contention()) PS_FATAL(kChan, "add_workload: free workload {} of SPURS {:#010x} still has contention", wid, spurs);

    reservation_op(base, spurs + kLine1, [&](u8* line) {
        ps_write8(line, wkl_state(wid) - kLine1, SPURS_WKL_STATE_PREPARING);
        ps_write8(line, wkl_status(wid) - kLine1, 0);
        ps_write8(line, wkl_event(wid) - kLine1, hook ? 2 : 0); // RPCS3: event bit 2 says there is a hook
        return true;
    });
    const u32 info = wkl_info(spurs, wid);
    ps_write64(base, info + wkl_off::info_addr, pm);
    ps_write64(base, info + wkl_off::info_arg, data);
    ps_write32(base, info + wkl_off::info_size, size);
    for (u32 i = 0; i < 8; i++) ps_write8(base, info + wkl_off::info_priority + i, priority[i]);
    const u32 names = wkl_h(spurs, wid);
    ps_write64(base, names, name_class);
    ps_write64(base, names + 8, name_instance);
    const u32 f = wkl_f(spurs, wid);
    std::memset(base + f, 0, 0x20);                 // keeps the semaphore at 0x20
    std::memset(base + f + 0x28, 0, 0x58);
    if (hook) { // RPCS3: the shutdown-completion hook
        ps_write64(base, f + wkl_off::f1_hook, hook);
        ps_write64(base, f + wkl_off::f1_hook_arg, hook_arg);
    }
    reservation_op(base, spurs, [&](u8* line) {
        if (!second) { // in SPURS2 these bytes are workload wid + 16's ready count, and unused
            ps_write8(line, wkl_off::idleSpuCountOrReadyCount2 + wid, 0);
            ps_write8(line, wkl_off::minContention + wid, static_cast<u8>(std::min<u32>(min_contention, 8)));
        }
        ps_write8(line, wkl_ready(wid), 0);
        const u8 maxc = ps_read8(line, wkl_off::maxContention + j);
        ps_write8(line, wkl_off::maxContention + j, static_cast<u8>((maxc & ~nibble) |
            (std::min<u32>(max_contention, 8) << (wid < CELL_SPURS_MAX_WORKLOAD ? 0 : 4))));
        ps_write16(line, wkl_signal_of(wid), static_cast<u16>(ps_read16(line, wkl_signal_of(wid)) & ~wkl_signal_bit(wid)));
        if (ps_read8(line, spurs_off::wklFlagReceiver) == wid) ps_write8(line, spurs_off::wklFlagReceiver, 0xFF);
        return true;
    });

    // Unique ID: shared with an enabled workload using the same policy module, else the lowest unused one (RPCS3's
    // walk over wklMskB, every workload of the instance).
    const u32 mask = ps_read32(base, spurs + wkl_off::mskB) & ~(0x80000000u >> wid);
    u32 unique = 0, used = 0;
    for (u32 i = 0, m = 0x80000000u; i < CELL_SPURS_MAX_WORKLOAD2; i++, m >>= 1) {
        if (!(mask & m)) continue;
        const u32 other = wkl_info(spurs, i);
        if (ps_read64(base, other + wkl_off::info_addr) == pm) {
            unique = ps_read8(base, other + wkl_off::info_unique);
            break;
        }
        used |= 0x80000000u >> (ps_read8(base, other + wkl_off::info_unique) & 31);
        unique = static_cast<u32>(std::countl_one(used));
    }
    ps_write8(base, info + wkl_off::info_unique, static_cast<u8>(unique));
    reservation_op(base, spurs + kLine1, [&](u8* line) {
        ps_write32(line, wkl_off::mskB - kLine1, (ps_read32(line, wkl_off::mskB - kLine1) & ~(0x80000000u >> wid)) |
            (0x80000000u >> wid));
        ps_write8(line, wkl_state(wid) - kLine1, SPURS_WKL_STATE_RUNNABLE);
        ps_write8(line, wkl_off::sysSrvMsgUpdateWorkload - kLine1, 0xFF);
        return true;
    });
    reservation_op(base, spurs, [](u8* line) { // wakes the idle workers
        ps_write8(line, wkl_off::sysSrvMessage, 0xFF);
        return true;
    });

    std::string label; // the class name's instance part, as the logs print it
    if (name_instance) label.assign(reinterpret_cast<const char*>(base + name_instance), strnlen(reinterpret_cast<const char*>(base + name_instance), 31));
    inst.workloads[wid] = Workload{kind, static_cast<u32>(data), std::move(label)};
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

// _cellSpursWorkloadAttributeInitialize(attr, revision, sdkVersion, pm, size, data, priority, minCnt, maxCnt), as
// RPCS3: NULL (attribute or policy module), ALIGN (an 8-aligned attribute, a 16-aligned module), INVAL (no minimum
// contention, a priority above 15), then the attribute zeroed and filled.
s32 _cellSpursWorkloadAttributeInitialize(PPUContext& ctx, u32 attr, u32 revision, u32 sdk_version, u32 pm, u32 size,
    u64 data, u32 priority, u32 min_contention, u32 max_contention)
{
    if (!attr) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (!pm) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (pm % 16) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, priority, 8)) {
        log_call_site(ctx, base, std::format("_cellSpursWorkloadAttributeInitialize: priority array {:#x} is not readable "
            "guest memory", priority));
        return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    }
    const u64 prio = ps_read64(base, priority);
    if (min_contention == 0 || (prio & 0xF0F0F0F0F0F0F0F0ull)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    std::memset(base + attr, 0, wattr_off::struct_size);
    ps_write32(base, attr + wattr_off::revision, revision);
    ps_write32(base, attr + wattr_off::sdkVersion, sdk_version);
    ps_write32(base, attr + wattr_off::pm, pm);
    ps_write32(base, attr + wattr_off::size, size);
    ps_write64(base, attr + wattr_off::data, data);
    ps_write64(base, attr + wattr_off::priority, prio);
    ps_write32(base, attr + wattr_off::minContention, min_contention);
    ps_write32(base, attr + wattr_off::maxContention, max_contention);
    return lv2::CELL_OK;
}

// cellSpursWorkloadAttributeSetName(attr, nameClass, nameInstance), as RPCS3: the two pointers, kept as they are.
s32 cellSpursWorkloadAttributeSetName(PPUContext& ctx, u32 attr, u32 name_class, u32 name_instance)
{
    if (!attr) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    ps_write32(base, attr + wattr_off::nameClass, name_class);
    ps_write32(base, attr + wattr_off::nameInstance, name_instance);
    return lv2::CELL_OK;
}

// cellSpursWorkloadAttributeSetShutdownCompletionEventHook(attr, hook, arg), as RPCS3 (libsre agrees): NULL (attribute
// or hook), ALIGN (8); the hook and its argument, which cellSpursAddWorkloadWithAttribute gives the workload. The event
// helper calls it once the workload's shutdown is complete (see above).
s32 cellSpursWorkloadAttributeSetShutdownCompletionEventHook(PPUContext& ctx, u32 attr, u32 hook, u32 arg)
{
    if (!attr || !hook) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    ps_write32(base, attr + wattr_off::hook, hook);
    ps_write32(base, attr + wattr_off::hookArg, arg);
    return lv2::CELL_OK;
}

// cellSpursAddWorkloadWithAttribute(spurs, wid, attr), as RPCS3: NULL, ALIGN (8), revision 1 only (INVAL), then the
// add with the attribute's names and shutdown hook. Like cellSpursAddWorkload, a game's own policy module, which the
// native scheduler runs (spurs_custom.cpp). A workload with a hook makes sure the instance has an event helper.
s32 cellSpursAddWorkloadWithAttribute(PPUContext& ctx, u32 spurs, u32 wid, u32 attr)
{
    if (!attr) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, attr, wattr_off::struct_size)) {
        log_call_site(ctx, base, std::format("cellSpursAddWorkloadWithAttribute: attribute {:#x} is not guest memory", attr));
        return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    }
    if (ps_read32(base, attr + wattr_off::revision) != 1) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!spurs || !wid) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    const u32 pm = ps_read32(base, attr + wattr_off::pm);
    const u32 size = ps_read32(base, attr + wattr_off::size);
    const u64 data = ps_read64(base, attr + wattr_off::data);
    u8 prio[8];
    for (u32 i = 0; i < 8; i++) prio[i] = ps_read8(base, attr + wattr_off::priority + i);
    const u32 name_class = ps_read32(base, attr + wattr_off::nameClass);
    const u32 name_instance = ps_read32(base, attr + wattr_off::nameInstance);
    u32 w = 0;
    const u32 hook = ps_read32(base, attr + wattr_off::hook);
    const s32 r = add_workload(base, spurs, w, pm, size, data, prio, ps_read32(base, attr + wattr_off::minContention),
        ps_read32(base, attr + wattr_off::maxContention), name_class, name_instance, Workload::Kind::Custom, hook,
        ps_read32(base, attr + wattr_off::hookArg));
    if (r == lv2::CELL_OK) {
        ps_write32(base, wid, w);
        if (hook) ensure_event_helper(ctx, spurs);
        const auto name = [&](u32 addr) {
            return addr && guest_readable(t, addr, 1)
                ? std::string(reinterpret_cast<const char*>(base + addr), strnlen(reinterpret_cast<const char*>(base + addr), 31))
                : std::string("-");
        };
        PS_LOG_INFO(kChan, "custom workload {} of SPURS {:#x} (\"{}\" \"{}\"): policy module {:#x} ({:#x} bytes), data {:#x}, "
            "priority {}", w, spurs, name(name_class), name(name_instance), pm, size, data, priority_text(prio));
    } else {
        PS_LOG_WARN(kChan, "cellSpursAddWorkloadWithAttribute(SPURS {:#x}, policy module {:#x}) -> {:#x}", spurs, pm,
            static_cast<u32>(r));
    }
    return r;
}

// The checks libsre's and RPCS3's workload functions share: NULL, 128-byte alignment, a workload ID within the
// instance's 16 (32 for SPURS2), registered (SRCH), no exception (STAT). `codes`: the error codes' base (the policy
// module's 0x80410800 or the core's 0x80410700, whose functions make the same checks).
s32 check_workload(u8* base, u32 spurs, u32 wid, u32 codes = 0x80410800)
{
    if (!spurs) return error(codes | 0x11);
    if (spurs % CELL_SPURS_ALIGN) return error(codes | 0x10);
    if (wid >= max_workloads(base, spurs)) return error(codes | 0x02);
    if (!workload_enabled(base, spurs, wid)) return error(codes | 0x05);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(codes | 0x0F);
    return lv2::CELL_OK;
}

// cellSpursSetExceptionEventHandler(spurs, wid, handler, arg), as libsre (0xDC5C; RPCS3 has none): NULL (spurs,
// handler), ALIGN, the workload's checks (INVAL, SRCH, STAT with an exception pending); then the workload's F entry
// takes the handler at 0x00 (u64) and its argument at 0x08, unless it has one (BUSY): libsre claims the entry with 1,
// stores the argument and then the handler. The native scheduler stops a worker whose SPU code faults (CLAUDE.md rule 5)
// and calls no handler; libsre's would run on the PPU from its exception event queue.
s32 cellSpursSetExceptionEventHandler(PPUContext& ctx, u32 spurs, u32 wid, u32 handler, u32 arg)
{
    if (!spurs || !handler) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    u8* base = guest_base(ctx);
    if (const s32 rc = check_workload(base, spurs, wid); rc != lv2::CELL_OK) return rc;
    const u32 f = wkl_f(spurs, wid);
    if (!atomic_cas_be64(base, f, 0, 1)) return error(CELL_SPURS_POLICY_MODULE_ERROR_BUSY);
    ps_write64(base, f + 8, arg);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ps_write64(base, f, handler);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ps_res_notify_write(f, 16);
    return lv2::CELL_OK;
}

// cellSpursUnsetExceptionEventHandler(spurs, wid), as libsre (0xD884): NULL, ALIGN, the workload's checks, then the
// handler (the F entry's u64 at 0x00) is cleared; the argument stays.
s32 cellSpursUnsetExceptionEventHandler(PPUContext& ctx, u32 spurs, u32 wid)
{
    u8* base = guest_base(ctx);
    if (const s32 rc = check_workload(base, spurs, wid); rc != lv2::CELL_OK) return rc;
    ps_write64(base, wkl_f(spurs, wid), 0);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ps_res_notify_write(wkl_f(spurs, wid), 8);
    return lv2::CELL_OK;
}

// cellSpursReadyCountStore(spurs, wid, value), as libsre (and RPCS3): NULL, ALIGN, INVAL (a workload past the
// instance's, a value above 255), SRCH, STAT (an exception, a workload not runnable), then the store (SPURS2's workloads
// 16..31: wklIdleSpuCountOrReadyCount2). Through the reservation system, so idle workers see it at once (libsre stores
// the byte; an SPU waiting on the line notices any store).
s32 cellSpursReadyCountStore(PPUContext& ctx, u32 spurs, u32 wid, u32 value)
{
    u8* base = guest_base(ctx);
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs) || value > 0xFF) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (const s32 r = check_workload(base, spurs, wid); r != lv2::CELL_OK) return r;
    if (ps_read8(base, spurs + wkl_state(wid)) != SPURS_WKL_STATE_RUNNABLE) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    reservation_op(base, spurs, [&](u8* line) {
        ps_write8(line, wkl_ready(wid), static_cast<u8>(value));
        return true;
    });
    PS_LOG_DEBUG(kChan, "cellSpursReadyCountStore(SPURS {:#x}, workload {}, {})", spurs, wid, value);
    return lv2::CELL_OK;
}

// cellSpursReadyCountSwap, ...CompareAndSwap and ...Add (RPCS3; libsre agrees): NULL (spurs, old), ALIGN, INVAL (a
// workload past the instance's, an operand above 255), SRCH, STAT (an exception, a workload not runnable); then one
// update of the ready count in CellSpurs line 0 (through the reservation system, as cellSpursReadyCountStore), and the
// value before it in *old (u32).
template <typename Op>
s32 ready_count_op(PPUContext& ctx, std::string_view fn, u32 spurs, u32 wid, u32 old, bool operands_ok, Op&& op)
{
    if (!spurs || !old) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (wid >= max_workloads(base, spurs) || !operands_ok) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception) || ps_read8(base, spurs + wkl_state(wid)) != SPURS_WKL_STATE_RUNNABLE) {
        return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    if (const s32 rc = check_pointer(ctx, fn, "old value", old, 4, CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8 before = 0;
    reservation_op(base, spurs, [&](u8* line) {
        before = ps_read8(line, wkl_ready(wid));
        const u8 after = op(before);
        if (after == before) return false; // nothing to store (a failed compare, a swap to the same count)
        ps_write8(line, wkl_ready(wid), after);
        return true;
    });
    ps_write32(base, old, before);
    return lv2::CELL_OK;
}

s32 cellSpursReadyCountSwap(PPUContext& ctx, u32 spurs, u32 wid, u32 old, u32 swap)
{
    return ready_count_op(ctx, "cellSpursReadyCountSwap", spurs, wid, old, swap <= 0xFF,
        [swap](u8) { return static_cast<u8>(swap); });
}

s32 cellSpursReadyCountCompareAndSwap(PPUContext& ctx, u32 spurs, u32 wid, u32 old, u32 compare, u32 swap)
{
    return ready_count_op(ctx, "cellSpursReadyCountCompareAndSwap", spurs, wid, old, (swap | compare) <= 0xFF,
        [compare, swap](u8 v) { return v == compare ? static_cast<u8>(swap) : v; });
}

// The sum clamps to 0..255 (RPCS3 std::clamp).
s32 cellSpursReadyCountAdd(PPUContext& ctx, u32 spurs, u32 wid, u32 old, s32 value)
{
    return ready_count_op(ctx, "cellSpursReadyCountAdd", spurs, wid, old, true,
        [value](u8 v) { return static_cast<u8>(std::clamp<s64>(s64{v} + value, 0, 255)); });
}

// cellSpursGetWorkloadData(spurs, data, wid), as RPCS3 (libsre agrees): NULL (spurs, data), ALIGN, INVAL (past the
// instance's workloads), SRCH, STAT; then the workload's argument (WorkloadInfo.arg, u64).
s32 get_workload_data(u8* base, u32 spurs, u32 wid, u64& data)
{
    if (const s32 rc = check_workload(base, spurs, wid); rc != lv2::CELL_OK) return rc;
    data = ps_read64(base, wkl_info(spurs, wid) + wkl_off::info_arg);
    return lv2::CELL_OK;
}

s32 cellSpursGetWorkloadData(PPUContext& ctx, u32 spurs, u32 data, u32 wid)
{
    if (!spurs || !data) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    u64 value = 0;
    if (const s32 rc = get_workload_data(guest_base(ctx), spurs, wid, value); rc != lv2::CELL_OK) return rc;
    if (const s32 rc = check_pointer(ctx, "cellSpursGetWorkloadData", "data", data, 8, CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    ps_write64(guest_base(ctx), data, value);
    return lv2::CELL_OK;
}

// cellSpursGetWorkloadInfo(spurs, wid, info), as libsre (0xE814; RPCS3 has a TODO): NULL (spurs, info), the 128-byte
// ALIGN, INVAL past the instance's workloads (16 or 32), SRCH for a workload not enabled; then CellSpursWorkloadInfo:
// the argument (u64) at 0x00, the priorities at 0x08, the policy module's address and size at 0x10 and 0x14, the name
// class and instance at 0x18 and 0x1C, the contention bytes and ready count at 0x20, the signal at 0x25, the
// shutdown-completion hook and its argument at 0x28 and 0x2C (bytes 0x26, 0x27 and 0x30 on left alone). With 32
// workloads libsre reports the contention nibbles, no minimum contention or idle count, and the signal as bit
// (wid & 15) of its word counted from the right, where the workload's own bit is counted from the left (0x8000 >> wid,
// which it uses with 16 workloads): kept, as libsre.
s32 cellSpursGetWorkloadInfo(PPUContext& ctx, u32 spurs, u32 wid, u32 info)
{
    constexpr std::string_view fn = "cellSpursGetWorkloadInfo";
    if (!spurs || !info) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "SPURS", spurs, CELL_SPURS_SIZE, CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const bool second = ps_read8(base, spurs + spurs_off::flags1) & SF1_32_WORKLOADS;
    if (wid >= (second ? CELL_SPURS_MAX_WORKLOAD2 : CELL_SPURS_MAX_WORKLOAD)) {
        return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    }
    if (!((ps_read32(base, spurs + wkl_off::enabled) >> (31 - wid)) & 1)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) {
        if (const s32 rc = check_pointer(ctx, fn, "SPURS2", spurs, CELL_SPURS_SIZE2, CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
            rc != lv2::CELL_OK) {
            return rc;
        }
    }
    if (const s32 rc = check_pointer(ctx, fn, "info", info, 0x30, CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    const u32 w = wid & 15, winfo = wkl_info(spurs, wid), h = wkl_h(spurs, wid), f = wkl_f(spurs, wid);
    ps_write64(base, info + 0x00, ps_read64(base, winfo + wkl_off::info_arg));
    std::memcpy(base + info + 0x08, base + winfo + wkl_off::info_priority, 8);
    ps_write32(base, info + 0x10, static_cast<u32>(ps_read64(base, winfo + wkl_off::info_addr)));
    ps_write32(base, info + 0x14, ps_read32(base, winfo + wkl_off::info_size));
    ps_write32(base, info + 0x18, static_cast<u32>(ps_read64(base, h + 0x00))); // name class
    ps_write32(base, info + 0x1C, static_cast<u32>(ps_read64(base, h + 0x08))); // name instance
    ps_write32(base, info + 0x28, static_cast<u32>(ps_read64(base, f + wkl_off::f1_hook)));
    ps_write32(base, info + 0x2C, static_cast<u32>(ps_read64(base, f + wkl_off::f1_hook_arg)));
    if (second) {
        const bool high = wid >= CELL_SPURS_MAX_WORKLOAD;
        const auto nibble = [&](u32 off) {
            const u8 v = ps_read8(base, spurs + off + w);
            return static_cast<u8>(high ? v >> 4 : v & 0x0F);
        };
        ps_write8(base, info + 0x20, nibble(wkl_off::currentContention));
        ps_write8(base, info + 0x21, 0);
        ps_write8(base, info + 0x22, nibble(wkl_off::maxContention));
        ps_write8(base, info + 0x23, ps_read8(base, spurs + wkl_ready(wid)));
        ps_write8(base, info + 0x24, 0);
        ps_write8(base, info + 0x25, static_cast<u8>((ps_read16(base, spurs + wkl_signal_of(wid)) >> w) & 1));
    } else {
        ps_write8(base, info + 0x20, ps_read8(base, spurs + wkl_off::currentContention + wid));
        ps_write8(base, info + 0x22, ps_read8(base, spurs + wkl_off::maxContention + wid));
        ps_write8(base, info + 0x21, ps_read8(base, spurs + wkl_off::minContention + wid));
        ps_write8(base, info + 0x23, ps_read8(base, spurs + wkl_off::readyCount1 + wid));
        ps_write8(base, info + 0x24, ps_read8(base, spurs + wkl_off::idleSpuCountOrReadyCount2 + wid));
        ps_write8(base, info + 0x25, (ps_read16(base, spurs + wkl_off::signal1) & wkl_signal_bit(wid)) ? 1 : 0);
    }
    return lv2::CELL_OK;
}

// cellSpursSetMaxContention(spurs, wid, maxContention), as RPCS3 (libsre agrees): the core's checks (NULL, ALIGN, INVAL,
// SRCH, STAT), the maximum clamped to 8 and stored in the workload's maximum-contention byte (SPURS2: its nibble), in
// one update of CellSpurs line 0 (which the workers see at once).
s32 cellSpursSetMaxContention(PPUContext& ctx, u32 spurs, u32 wid, u32 max_contention)
{
    u8* base = guest_base(ctx);
    if (const s32 rc = check_workload(base, spurs, wid, 0x80410700); rc != lv2::CELL_OK) return rc;
    const u32 value = std::min<u32>(max_contention, CELL_SPURS_MAX_SPU);
    const u32 shift = wid < CELL_SPURS_MAX_WORKLOAD ? 0 : 4;
    reservation_op(base, spurs, [&](u8* line) {
        const u8 old = ps_read8(line, wkl_off::maxContention + (wid & 15));
        ps_write8(line, wkl_off::maxContention + (wid & 15),
            static_cast<u8>((old & (wid < CELL_SPURS_MAX_WORKLOAD ? 0xF0 : 0x0F)) | (value << shift)));
        return true;
    });
    return lv2::CELL_OK;
}

// cellSpursSetPriorities(spurs, wid, priorities), as RPCS3 (libsre agrees): the core's checks, a priority above 15
// (INVAL); the eight priorities in WorkloadInfo, then every SPU's system service message (sysSrvMsgUpdateWorkload,
// sysSrvMessage: 0xFF). Natively the workers read the priorities at every selection, and the message's store wakes the
// idle ones. libsre first refuses the instance's system workload (it has no workload ID natively).
s32 cellSpursSetPriorities(PPUContext& ctx, u32 spurs, u32 wid, u32 priorities)
{
    u8* base = guest_base(ctx);
    if (const s32 rc = check_workload(base, spurs, wid, 0x80410700); rc != lv2::CELL_OK) return rc;
    if (const s32 rc = check_pointer(ctx, "cellSpursSetPriorities", "priority table", priorities, 8,
            CELL_SPURS_CORE_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    const u64 prio = ps_read64(base, priorities);
    if (prio & 0xF0F0F0F0F0F0F0F0ull) return error(CELL_SPURS_CORE_ERROR_INVAL);
    __atomic_store_n(host_u64(base, wkl_info(spurs, wid) + wkl_off::info_priority), ps_bswap64(prio), __ATOMIC_RELEASE);
    reservation_op(base, spurs + wkl_off::sysSrvMsgUpdateWorkload, [](u8* line) {
        ps_write8(line, wkl_off::sysSrvMsgUpdateWorkload % PS_RES_LINE_SIZE, 0xFF);
        return true;
    });
    reservation_op(base, spurs, [](u8* line) {
        ps_write8(line, wkl_off::sysSrvMessage, 0xFF);
        return true;
    });
    return lv2::CELL_OK;
}

// cellSpursSetPriority(spurs, wid, spuId, priority), as RPCS3 (libsre agrees): NULL, ALIGN, INVAL (a workload past the
// instance's; an SPU past its count or a priority above 15), SRCH, STAT; the priority on that SPU, then that SPU's bit
// in sysSrvMsgUpdateWorkload and sysSrvMessage.
s32 cellSpursSetPriority(PPUContext& ctx, u32 spurs, u32 wid, u32 spu, u32 priority)
{
    if (!spurs) return error(CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_CORE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_CORE_ERROR_INVAL);
    if (priority > 15 || spu >= ps_read8(base, spurs + spurs_off::nSpus)) return error(CELL_SPURS_CORE_ERROR_INVAL);
    if (const s32 rc = check_workload(base, spurs, wid, 0x80410700); rc != lv2::CELL_OK) return rc;
    __atomic_store_n(base + wkl_info(spurs, wid) + wkl_off::info_priority + spu, static_cast<u8>(priority), __ATOMIC_RELEASE);
    reservation_op(base, spurs + wkl_off::sysSrvMsgUpdateWorkload, [spu](u8* line) {
        const u32 off = wkl_off::sysSrvMsgUpdateWorkload % PS_RES_LINE_SIZE;
        ps_write8(line, off, static_cast<u8>(ps_read8(line, off) | (1u << spu)));
        return true;
    });
    reservation_op(base, spurs, [spu](u8* line) {
        ps_write8(line, wkl_off::sysSrvMessage, static_cast<u8>(ps_read8(line, wkl_off::sysSrvMessage) | (1u << spu)));
        return true;
    });
    return lv2::CELL_OK;
}

// cellSpursRequestIdleSpu(spurs, wid, count), as libsre: NULL, ALIGN, STAT for SPURS2 (the old API's byte is a ready
// count there), INVAL (a workload past 15, a count above 8), SRCH, STAT; then the workload's idle SPU request
// (wklIdleSpuCountOrReadyCount2), which the native selection adds to its ready count, as kernel 1's. RPCS3 has the same
// steps with the core's error codes and refuses a count of 8; libsre's are the policy module's and 8 is accepted.
s32 cellSpursRequestIdleSpu(PPUContext& ctx, u32 spurs, u32 wid, u32 count)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    u8* base = guest_base(ctx);
    if (spurs2(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    if (wid >= CELL_SPURS_MAX_WORKLOAD || count > CELL_SPURS_MAX_SPU) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    reservation_op(base, spurs, [&](u8* line) {
        ps_write8(line, wkl_off::idleSpuCountOrReadyCount2 + wid, static_cast<u8>(count));
        return true;
    });
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

// _cellSpursWorkloadFlagReceiver(spurs, wid, isSet) and _cellSpursWorkloadFlagReceiver2(spurs, wid, isSet, debug), as
// RPCS3 (libsre agrees): check_workload; then in one update of CellSpurs line 0 the workload becomes the receiver of the
// workload flag (BUSY if one is already) or stops being it (PERM if it is not the receiver). The first also resets the
// flag to "not raised" (all ones); the second leaves it (and libsre prints a debug line when asked to: logged here).
// Code that raises the flag stores 0 into it, which makes a worker select the receiver with poll status 4.
s32 flag_receiver(PPUContext& ctx, std::string_view fn, u32 spurs, u32 wid, u32 is_set, bool reset_flag)
{
    u8* base = guest_base(ctx);
    if (const s32 r = check_workload(base, spurs, wid); r != lv2::CELL_OK) return r;
    s32 rc = lv2::CELL_OK;
    reservation_op(base, spurs, [&](u8* line) {
        rc = lv2::CELL_OK;
        const u8 receiver = ps_read8(line, spurs_off::wklFlagReceiver);
        if (is_set ? receiver != 0xFF : receiver != wid) {
            rc = error(is_set ? CELL_SPURS_POLICY_MODULE_ERROR_BUSY : CELL_SPURS_POLICY_MODULE_ERROR_PERM);
            return false;
        }
        if (reset_flag) ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
        ps_write8(line, spurs_off::wklFlagReceiver, is_set ? static_cast<u8>(wid) : 0xFF);
        return true;
    });
    PS_LOG_INFO(kChan, "{}(SPURS {:#x}, workload {}, {}) -> {:#x}", fn, spurs, wid, is_set ? "set" : "clear",
        static_cast<u32>(rc));
    return rc;
}

s32 _cellSpursWorkloadFlagReceiver(PPUContext& ctx, u32 spurs, u32 wid, u32 is_set)
{
    return flag_receiver(ctx, "_cellSpursWorkloadFlagReceiver", spurs, wid, is_set, true);
}

s32 _cellSpursWorkloadFlagReceiver2(PPUContext& ctx, u32 spurs, u32 wid, u32 is_set, u32 print_debug_output)
{
    if (print_debug_output) {
        PS_LOG_DEBUG(kChan, "_cellSpursWorkloadFlagReceiver2(SPURS {:#x}, workload {}): the debug output libsre prints",
            spurs, wid);
    }
    return flag_receiver(ctx, "_cellSpursWorkloadFlagReceiver2", spurs, wid, is_set, false);
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
    PS_LOG_INFO(kChan, "taskset {:#010x} \"{}\" is workload {} of SPURS {:#010x} (max contention {}, priorities {})", taskset,
        label, wid, spurs, max_contention, priority_text(priority));
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

s32 task_readable(PPUContext& ctx, std::string_view what, std::string_view name, u32 addr, u32 size);

// cellSpursCreateTaskset(spurs, taskset, args, priority, maxContention): RPCS3 cellSpursCreateTaskset, and libsre's
// (fw 4.92) passes the same: create_taskset for a CellSpursTaskset (6400 bytes) with no name and enable_clear_ls 0.
// The priority table is read after create_taskset's pointer checks (NULL, ALIGN), which therefore come first here; an
// unreadable table is refused with TASK_ERROR_FAULT and a log of the call site (D-042; RPCS3 and libsre read it
// unchecked).
s32 cellSpursCreateTaskset(PPUContext& ctx, u32 spurs, u32 taskset, u64 args, u32 priority, u32 max_contention)
{
    PS_LOG_INFO(kChan, "cellSpursCreateTaskset(spurs {:#x}, taskset {:#x}, args {:#x}, priority {:#x}, maxContention {})",
        spurs, taskset, args, priority, max_contention);
    if (!spurs || !taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = task_readable(ctx, "cellSpursCreateTaskset", "priority table", priority, 8); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    u8 prio[8];
    std::memcpy(prio, base + priority, 8);
    return create_taskset(base, spurs, taskset, args, prio, max_contention, 0, kTasksetSize, 0);
}

// _cellSpursTasksetAttribute2Initialize(attr, revision), as RPCS3 (libsre agrees): the 512 bytes cleared, then the
// revision, no name, arguments 0, priority 1 on every SPU, maximum contention 8, no LS clearing, no task name buffer.
// Neither checks the pointer; one that is not guest memory is refused with TASK_ERROR_FAULT here (D-042).
void write_taskset_attribute2_defaults(u8* base, u32 attr, u32 revision)
{
    std::memset(base + attr, 0, tsattr2_off::size);
    ps_write32(base, attr + tsattr2_off::revision, revision);
    ps_write32(base, attr + tsattr2_off::name, 0);
    ps_write64(base, attr + tsattr2_off::args, 0);
    std::memset(base + attr + tsattr2_off::priority, 1, 8);
    ps_write32(base, attr + tsattr2_off::max_contention, 8);
    ps_write32(base, attr + tsattr2_off::enable_clear_ls, 0);
    ps_write32(base, attr + tsattr2_off::task_name_buffer, 0);
}

s32 _cellSpursTasksetAttribute2Initialize(PPUContext& ctx, u32 attr, u32 revision)
{
    if (const s32 rc = check_pointer(ctx, "_cellSpursTasksetAttribute2Initialize", "attribute", attr, tsattr2_off::size,
            CELL_SPURS_TASK_ERROR_FAULT);
        rc != lv2::CELL_OK) {
        return rc;
    }
    write_taskset_attribute2_defaults(guest_base(ctx), attr, revision);
    return lv2::CELL_OK;
}

// cellSpursCreateTaskset2(spurs, taskset, attr), as libsre (RPCS3 has its first two steps and a TODO): without an
// attribute, _cellSpursTasksetAttribute2Initialize's defaults (revision 0); create_taskset for a CellSpursTaskset2
// (10496 bytes) with the attribute's arguments, priorities, maximum contention, name and LS clearing; a task name buffer
// not 16-byte aligned is ALIGN (after the taskset is created, as both). Then what libsre adds: the exit code handler's
// descriptor at 0x1900 and the Task2 handler's at 0x78 (markers here: those handlers are libsre's SPU code), two lv2
// event flags "taskset2" (priority protocol, waiters multiple) at 0x1898 and 0x189C, which cellSpursJoinTask2 waits on,
// and the task name buffer, cleared (4 KB) and kept at 0x18A8. An event flag that cannot be created gives lv2's error
// (the first one is destroyed when the second fails, as libsre; the workload stays added, as libsre). Tasks created with
// cellSpursCreateTask run in it as in any taskset; CellSpursTaskset2's own tasks (cellSpursCreateTask2, JoinTask2) are
// not implemented (D-171).
s32 cellSpursCreateTaskset2(PPUContext& ctx, u32 spurs, u32 taskset, u32 attr)
{
    constexpr std::string_view fn = "cellSpursCreateTaskset2";
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar vars(ctx, base, tsattr2_off::size + 0x20); // the default attribute; the event flags' attribute
    if (!attr) {
        attr = vars.addr();
        write_taskset_attribute2_defaults(base, attr, 0);
    } else if (const s32 rc = check_pointer(ctx, fn, "attribute", attr, tsattr2_off::task_name_buffer + 4,
                   CELL_SPURS_TASK_ERROR_FAULT);
               rc != lv2::CELL_OK) {
        return rc;
    }
    u8 prio[8];
    std::memcpy(prio, base + attr + tsattr2_off::priority, 8);
    const u32 name = ps_read32(base, attr + tsattr2_off::name);
    if (const s32 rc = create_taskset(base, spurs, taskset, ps_read64(base, attr + tsattr2_off::args), prio,
            ps_read32(base, attr + tsattr2_off::max_contention), name, kTaskset2Size,
            static_cast<s32>(ps_read32(base, attr + tsattr2_off::enable_clear_ls)));
        rc != lv2::CELL_OK) {
        return rc;
    }
    const u32 names = ps_read32(base, attr + tsattr2_off::task_name_buffer);
    if (names % 16) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    ps_write64(base, taskset + ts_off::exit_code_handler,
        SPURS_IMG_ADDR_TASK_EXIT_CODE_HANDLER | ((kTaskExitCodeHandlerSize + 127) >> 7));
    ps_write64(base, taskset + ts_off::x78, SPURS_IMG_ADDR_TASK2_HANDLER | ((kTask2HandlerSize + 127) >> 7));
    const u32 fattr = vars.addr() + tsattr2_off::size; // sys_event_flag_attribute_t
    std::memset(base + fattr, 0, 0x20);
    ps_write32(base, fattr + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, fattr + 0x04, 0x200);   // SYS_SYNC_NOT_PROCESS_SHARED
    ps_write32(base, fattr + 0x14, 0x20000); // SYS_SYNC_WAITER_MULTIPLE
    std::memcpy(base + fattr + 0x18, "taskset2", 8);
    if (const s32 rc = lv2::sys_event_flag_create(t, taskset + ts_off::event_flag_id1, fattr, 0); rc != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "{}({:#x}): its first event flag: {}", fn, taskset, lv2::error_name(rc));
        return rc;
    }
    if (const s32 rc = lv2::sys_event_flag_create(t, taskset + ts_off::event_flag_id2, fattr, 0); rc != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "{}({:#x}): its second event flag: {}", fn, taskset, lv2::error_name(rc));
        lv2::sys_event_flag_destroy(t, ps_read32(base, taskset + ts_off::event_flag_id1));
        return rc;
    }
    if (names) {
        if (const s32 rc = check_pointer(ctx, fn, "task name buffer", names, 0x1000, CELL_SPURS_TASK_ERROR_FAULT);
            rc != lv2::CELL_OK) {
            return rc;
        }
        std::memset(base + names, 0, 0x1000);
        ps_write64(base, taskset + ts_off::task_name_buffer, names);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    PS_LOG_INFO(kChan, "{}: CellSpursTaskset2 {:#x} (event flags {:#x} {:#x}, task names {:#x})", fn, taskset,
        ps_read32(base, taskset + ts_off::event_flag_id1), ps_read32(base, taskset + ts_off::event_flag_id2), names);
    return lv2::CELL_OK;
}

// _cellSpursTaskAttribute2Initialize(attr, revision), as libsre: the 256 bytes cleared, then the revision, no context
// (size 0), an empty LS pattern and no name. RPCS3 writes the same fields without clearing the reserved bytes. A pointer
// that is not guest memory is refused with TASK_ERROR_FAULT (D-042; neither checks it).
s32 _cellSpursTaskAttribute2Initialize(PPUContext& ctx, u32 attr, u32 revision)
{
    if (const s32 rc = check_pointer(ctx, "_cellSpursTaskAttribute2Initialize", "attribute", attr, 0x100,
            CELL_SPURS_TASK_ERROR_FAULT);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    std::memset(base + attr, 0, 0x100);
    ps_write32(base, attr + 0x00, revision); // revision, sizeContext 0x04, eaContext 0x08 (u64), lsPattern 0x10, name 0x20
    return lv2::CELL_OK;
}

// cellSpursTasksetAttributeEnableClearLS(attr, enable), as RPCS3: NULL, ALIGN (8), then 1 or 0.
s32 cellSpursTasksetAttributeEnableClearLS(PPUContext& ctx, u32 attr, s32 enable)
{
    if (!attr) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    ps_write32(guest_base(ctx), attr + tsattr_off::enable_clear_ls, enable ? 1 : 0);
    return lv2::CELL_OK;
}

// The checks of the taskset accessors below, as libsre: `outs` pointers NULL after the taskset (NULL_POINTER), the
// taskset 128-byte aligned (ALIGN), a workload ID past 31 (INVAL; RPCS3 refuses one past 15, which SPURS2 tasksets have).
// A taskset that is not guest memory is refused with TASK_ERROR_FAULT (D-042).
s32 check_taskset(PPUContext& ctx, std::string_view fn, u32 taskset, std::initializer_list<u32> outs, bool check_wid)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    for (const u32 out : outs)
        if (!out) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "taskset", taskset, ts_off::size + 4, CELL_SPURS_TASK_ERROR_FAULT);
        rc != lv2::CELL_OK) {
        return rc;
    }
    if (check_wid && ps_read32(guest_base(ctx), taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    for (const u32 out : outs) {
        if (const s32 rc = check_pointer(ctx, fn, "output", out, 4, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) return rc;
    }
    return lv2::CELL_OK;
}

// cellSpursTasksetSetExceptionEventHandler(taskset, handler, arg), as libsre: the checks (the handler NULL), BUSY when one
// is set already; then the argument and the handler (u64 each at 0x1888 and 0x1880). An SPU task's exception stops its
// worker with a log natively (CLAUDE.md rule 5) rather than reaching the handler.
s32 cellSpursTasksetSetExceptionEventHandler(PPUContext& ctx, u32 taskset, u32 handler, u32 arg)
{
    if (!taskset || !handler) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (const s32 rc = check_taskset(ctx, "cellSpursTasksetSetExceptionEventHandler", taskset, {}, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read64(base, taskset + ts_off::exception_handler)) return error(CELL_SPURS_TASK_ERROR_BUSY);
    ps_write64(base, taskset + ts_off::exception_handler_arg, arg);
    ps_write64(base, taskset + ts_off::exception_handler, handler);
    return lv2::CELL_OK;
}

// cellSpursTasksetUnsetExceptionEventHandler(taskset), as libsre: the checks, then both cleared.
s32 cellSpursTasksetUnsetExceptionEventHandler(PPUContext& ctx, u32 taskset)
{
    if (const s32 rc = check_taskset(ctx, "cellSpursTasksetUnsetExceptionEventHandler", taskset, {}, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write64(base, taskset + ts_off::exception_handler_arg, 0);
    ps_write64(base, taskset + ts_off::exception_handler, 0);
    return lv2::CELL_OK;
}

// cellSpursTasksetGetSpursAddress(taskset, spurs), as libsre: the checks, then the taskset's CellSpurs (u32).
s32 cellSpursTasksetGetSpursAddress(PPUContext& ctx, u32 taskset, u32 spurs)
{
    if (const s32 rc = check_taskset(ctx, "cellSpursTasksetGetSpursAddress", taskset, {spurs}, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, spurs, static_cast<u32>(ps_read64(base, taskset + ts_off::spurs)));
    return lv2::CELL_OK;
}

// libsre's GUID of an SPU ELF (0xEE4C), for cellSpursGetTasksetInfo: none (false) unless the ELF is 16-aligned memory
// that is there (sys_memory_get_page_attribute), an ELF ("\x7FELF") for the SPU (machine 23) with program headers;
// then the first loadable segment (type 1) that starts with the four `ila $2` of a .SpuGUID section
// (cellSpursGetSpuGuid's pattern) gives the eight bytes, bits 9-24 of each instruction. Memory libsre would fault on
// reading: none here.
bool elf_spu_guid(lv2::PpuThread& t, u32 elf, std::array<u8, 8>& out)
{
    u8* base = t.kernel.base();
    if (!elf || elf % 16 || !guest_readable(t, elf, 0x34)) return false;
    if (std::memcmp(base + elf, "\x7F" "ELF", 4) != 0 || ps_read16(base, elf + 18) != 23) return false;
    const u32 phnum = ps_read16(base, elf + 44), phoff = ps_read32(base, elf + 28), phentsize = ps_read16(base, elf + 42);
    for (u32 i = 0; i < phnum; i++) {
        const u32 ph = elf + phoff + i * phentsize;
        if (!guest_readable(t, ph, 8)) return false;
        if (ps_read32(base, ph) != 1) continue;
        const u32 seg = elf + ps_read32(base, ph + 4);
        if (!guest_readable(t, seg, 16)) return false;
        bool guid = true;
        for (u32 k = 0; k < 4 && guid; k++) {
            const u32 w = ps_read32(base, seg + 4 * k);
            guid = std::rotl(std::rotl(w, 7) & 0xFFFFu, 25) == 0x42000002u + 0x80u * k; // opcode, $2, the word's index
        }
        if (!guid) continue;
        for (u32 k = 0; k < 4; k++) {
            const u32 w = ps_read32(base, seg + 4 * k);
            out[2 * k] = static_cast<u8>(w >> 17);
            out[2 * k + 1] = static_cast<u8>(w >> 9);
        }
        return true;
    }
    return false;
}

// cellSpursGetTasksetInfo(taskset, info), as libsre (0x14568; RPCS3 has none): NULL (taskset, info), the 128-byte
// ALIGN, the taskset's workload ID past 31 (INVAL); cellSpursGetWorkloadInfo of it, whose SRCH becomes INVAL (its
// other errors as they are). Then CellSpursTasksetInfo: for each of the 128 task slots (0x48 bytes each, whether the
// task exists or not) its LS pattern, argument, ELF (8-aligned), context (128-aligned) and context size (0x400 + 0x800
// a saved LS block; 0 without a context), its state (0 none, 1 running, 2 waiting, 3 ready or pending ready), whether it
// is signalled, its exit code container (a CellSpursTaskset2's, else 0) and its ELF's GUID (zeros without one); then the
// argument, workload ID, last scheduled task, name, exception handler and its argument, and the taskset's size.
s32 cellSpursGetTasksetInfo(PPUContext& ctx, u32 taskset, u32 info)
{
    constexpr std::string_view fn = "cellSpursGetTasksetInfo";
    if (!taskset || !info) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "taskset", taskset, kTasksetSize, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) {
        return rc;
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 wid = ps_read32(base, taskset + ts_off::wid);
    if (wid >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
    u32 name = 0;
    {
        StackVar w(ctx, base, 0x30);
        if (const s32 rc = cellSpursGetWorkloadInfo(ctx, static_cast<u32>(ps_read64(base, taskset + ts_off::spurs)), wid,
                w.addr());
            rc != lv2::CELL_OK) {
            return static_cast<u32>(rc) == CELL_SPURS_POLICY_MODULE_ERROR_SRCH ? error(CELL_SPURS_TASK_ERROR_INVAL) : rc;
        }
        name = ps_read32(base, w.addr() + 0x1C);
    }
    const u32 size = ps_read32(base, taskset + ts_off::size);
    const bool second = size > kTaskset2Size - 1;
    if (second) {
        if (const s32 rc = check_pointer(ctx, fn, "taskset", taskset, kTaskset2Size, CELL_SPURS_TASK_ERROR_FAULT);
            rc != lv2::CELL_OK) {
            return rc;
        }
    }
    if (const s32 rc = check_pointer(ctx, fn, "info", info, 0x2420, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) {
        return rc;
    }
    ps_write32(base, info + 0x2408, wid);
    ps_write64(base, info + 0x2400, ps_read64(base, taskset + ts_off::args));
    ps_write32(base, info + 0x240C, ps_read8(base, taskset + 0x73)); // the task scheduled last
    ps_write32(base, info + 0x241C, size);
    ps_write32(base, info + 0x2410, name);
    ps_write32(base, info + 0x2414, static_cast<u32>(ps_read64(base, taskset + ts_off::exception_handler)));
    ps_write32(base, info + 0x2418, static_cast<u32>(ps_read64(base, taskset + ts_off::exception_handler_arg)));
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        const u32 ti = taskset + ts_off::task_info + i * ts_off::task_info_stride, out = info + i * 0x48;
        ps_write64(base, out + 0x00, ps_read64(base, ti + ts_off::ti_ls_pattern));
        ps_write64(base, out + 0x08, ps_read64(base, ti + ts_off::ti_ls_pattern + 8));
        ps_write64(base, out + 0x10, ps_read64(base, ti + ts_off::ti_args));
        ps_write64(base, out + 0x18, ps_read64(base, ti + ts_off::ti_args + 8));
        const u64 context = ps_read64(base, ti + ts_off::ti_context);
        const u32 elf = static_cast<u32>(ps_read64(base, ti + ts_off::ti_elf)) & ~7u;
        ps_write32(base, out + 0x20, elf);
        ps_write32(base, out + 0x24, static_cast<u32>(context) & ~0x7Fu);
        ps_write32(base, out + 0x28, context ? ((static_cast<u32>(context) & 0x7F) << 11) + 0x400 : 0);
        ps_write32(base, out + 0x30, second ? static_cast<u32>(ps_read64(base, taskset + 0x1988 + 16 * i)) : 0);
        const u32 word = 4 * (i / 32), bit = 0x80000000u >> (i % 32);
        const auto has = [&](u32 set) { return (ps_read32(base, taskset + set + word) & bit) != 0; };
        u8 state = 0;
        if (has(0x30)) state = has(0x00) ? 1 : has(0x10) || has(0x20) ? 3 : 2; // enabled: running, ready, waiting
        ps_write8(base, out + 0x2C, state);
        ps_write8(base, out + 0x2D, has(0x40) ? 1 : 0);
        std::array<u8, 8> guid{};
        if (!elf_spu_guid(t, elf, guid)) guid = {};
        std::memcpy(base + out + 0x34, guid.data(), 8);
    }
    return lv2::CELL_OK;
}

// cellSpursGetTasksetId(taskset, wid), as RPCS3 (libsre agrees): NULL, ALIGN, then the taskset's workload ID, whatever
// it is.
s32 cellSpursGetTasksetId(PPUContext& ctx, u32 taskset, u32 wid)
{
    if (const s32 rc = check_taskset(ctx, "cellSpursGetTasksetId", taskset, {wid}, false); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    ps_write32(base, wid, ps_read32(base, taskset + ts_off::wid));
    return lv2::CELL_OK;
}

// cellSpursLookUpTasksetAddress(spurs, taskset, id), as RPCS3 (libsre agrees): NULL for the output, then
// cellSpursGetWorkloadData on workload `id`, a policy module error becoming the task error (code ^ 0x100); the taskset
// is the workload's argument.
s32 cellSpursLookUpTasksetAddress(PPUContext& ctx, u32 spurs, u32 taskset, u32 id)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    u64 data = 0;
    if (const s32 rc = get_workload_data(guest_base(ctx), spurs, id, data); rc != lv2::CELL_OK) {
        return static_cast<s32>(static_cast<u32>(rc) ^ 0x100);
    }
    if (const s32 rc = check_pointer(ctx, "cellSpursLookUpTasksetAddress", "output", taskset, 4, CELL_SPURS_TASK_ERROR_FAULT);
        rc != lv2::CELL_OK) {
        return rc;
    }
    ps_write32(guest_base(ctx), taskset, static_cast<u32>(data));
    return lv2::CELL_OK;
}

// cellSpursShutdownWorkload, checked against libsre (fw 4.92), which RPCS3 follows: NULL, ALIGN, INVAL for a
// workload ID past the instance's 16 (32 for SPURS2), SRCH for a workload that is not registered (its wklEnabled bit
// clear), STAT for an instance in exception state or a workload not yet runnable. The native scheduler does not keep
// wklStatus (which SPUs run a workload; status 0), so a shutdown makes the workload removable at once: a worker that is
// running it leaves at its next poll, since the selection only takes runnable workloads. When the workload has a
// shutdown-completion hook (event bit 0x02) or a waiter (0x10), its completion goes to the event helper, which calls the
// hook once the workers left (RPCS3 and libsre send it through the SPURS event port; a failed send is CORE STAT).
s32 shutdown_workload(PPUContext& ctx, u32 spurs, u32 wid)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    u8 event = 0;
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        u8* const state = base + spurs + wkl_state(wid);
        const u8 now = __atomic_load_n(state, __ATOMIC_SEQ_CST);
        if (now <= SPURS_WKL_STATE_PREPARING) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
        if (now == SPURS_WKL_STATE_SHUTTING_DOWN || now == SPURS_WKL_STATE_REMOVABLE) return lv2::CELL_OK;
        // libsre also reads wklStatus2[wid] for workloads 0..15 (RPCS3 reads only wklStatus1).
        if (ps_read8(base, spurs + wkl_status(wid)) || (wid < CELL_SPURS_MAX_WORKLOAD && ps_read8(base, spurs + wkl_off::status2 + wid))) {
            __atomic_store_n(state, SPURS_WKL_STATE_SHUTTING_DOWN, __ATOMIC_SEQ_CST);
            ps_write8(base, spurs + wkl_off::sysSrvMsgUpdateWorkload, 0xFF);
            reservation_op(base, spurs, [](u8* line) {
                ps_write8(line, wkl_off::sysSrvMessage, 0xFF);
                return true;
            });
            return lv2::CELL_OK;
        }
        // Sequentially consistent: a worker that selected the workload before this reads the state again after counting
        // itself inside (spurs_scheduler.cpp), and a join reads the state and then that count (D-136).
        __atomic_store_n(state, SPURS_WKL_STATE_REMOVABLE, __ATOMIC_SEQ_CST);
        event = __atomic_fetch_or(base + spurs + wkl_event(wid), u8{1}, __ATOMIC_SEQ_CST);
    }
    return notify_shutdown_completion(t, spurs, wid, event);
}

// cellSpursShutdownWorkload(spurs, wid): shutdown_workload.
s32 cellSpursShutdownWorkload(PPUContext& ctx, u32 spurs, u32 wid)
{
    const s32 rc = shutdown_workload(ctx, spurs, wid);
    PS_LOG_INFO(kChan, "cellSpursShutdownWorkload(SPURS {:#x}, workload {}) -> {:#x}", spurs, wid, static_cast<u32>(rc));
    return rc;
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
    s32 r = shutdown_workload(ctx, spurs, wid);
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

// cellSpursWaitForWorkloadShutdown's checks and order, as RPCS3 (cellSpurs.cpp) and libsre: NULL, ALIGN, INVAL, SRCH,
// STAT. RPCS3 then waits on the workload's semaphore, which the shutdown's completion posts; the native scheduler marks
// a shut-down workload removable at once (shutdown_workload), and a worker still inside it leaves after its current
// job or task: the wait is for both (D-136), so the game may free what the workload used once a join returns. A
// workload an SPU shut down (a job chain's error) becomes removable here once its last worker left. The PPU
// thread sleeps 100 us at a time, as the thread's other waits, so a stop request still ends it. Not a second waiter's
// STAT (RPCS3's wklSyncInfo x28): the native wait keeps no slot to be taken. A workload with a shutdown-completion hook
// is waited for until its hook ran (event bit 0x20, which the event helper sets; RPCS3's waiter wakes on the semaphore
// the helper posts after the hook), when it has a helper. The waiter marks itself in the event byte (0x10) as RPCS3's
// does. A workload another thread removed meanwhile ends the wait with SRCH.
s32 wait_for_workload_shutdown(PPUContext& ctx, u32 spurs, u32 wid)
{
    u8* base = guest_base(ctx);
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    std::shared_ptr<Scheduler> scheduler;
    bool helper = false;
    {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        if (const auto it = h.instances.find(spurs); it != h.instances.end()) {
            scheduler = it->second.scheduler;
            helper = it->second.helper != 0;
        }
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* const state = base + spurs + wkl_state(wid);
    u8* const event = base + spurs + wkl_event(wid);
    // RPCS3: a waiter that will wait (no completion yet, or a hook not called yet) sets bit 0x10.
    for (u8 e = __atomic_load_n(event, __ATOMIC_SEQ_CST); !(e & 0x01) || (e & 0x22) == 0x02;) {
        if (__atomic_compare_exchange_n(event, &e, static_cast<u8>(e | 0x10), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    }
    const u8 nibble = wid < CELL_SPURS_MAX_WORKLOAD ? 0x0F : 0xF0;
    for (;;) {
        if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
        // Removable, no worker inside, and none still counted on it: a worker gives its contention back at its next
        // selection, a moment after it leaves (so a workload added in the slot next finds none).
        const u8 now = __atomic_load_n(state, __ATOMIC_SEQ_CST);
        const bool left = (!scheduler || workers_inside(*scheduler, wid) == 0) &&
            !(__atomic_load_n(base + spurs + wkl_off::currentContention + (wid & 15), __ATOMIC_SEQ_CST) & nibble);
        if (now == SPURS_WKL_STATE_REMOVABLE && left && !(helper && hook_pending(__atomic_load_n(event, __ATOMIC_SEQ_CST)))) {
            break;
        }
        if (now == SPURS_WKL_STATE_SHUTTING_DOWN && left) {
            // The system service's step when the last SPU leaves a workload that shuts down (RPCS3
            // spursSysServiceUpdateWorkload and spursSysServiceUpdateShutdownCompletionEvents): removable, the
            // completion bit in its event byte, and the completion to the event helper. Natively the waiter takes it: a
            // job chain that halts on an error shuts its workload down from the SPU (spurs_jobchain.cpp), and no system
            // service runs. Checked again after this store, so a worker that counted itself inside meanwhile is seen.
            u8 expected = SPURS_WKL_STATE_SHUTTING_DOWN;
            if (__atomic_compare_exchange_n(state, &expected, SPURS_WKL_STATE_REMOVABLE, false, __ATOMIC_SEQ_CST,
                    __ATOMIC_SEQ_CST)) {
                notify_shutdown_completion(t, spurs, wid, __atomic_fetch_or(event, u8{1}, __ATOMIC_SEQ_CST));
            }
            continue;
        }
        if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
        if (!lv2::sleep_us(t.waiter, 100)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT); // the thread is torn down
    }
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    return lv2::CELL_OK;
}

// cellSpursWaitForWorkloadShutdown(spurs, wid): wait_for_workload_shutdown.
s32 cellSpursWaitForWorkloadShutdown(PPUContext& ctx, u32 spurs, u32 wid)
{
    const s32 rc = wait_for_workload_shutdown(ctx, spurs, wid);
    PS_LOG_INFO(kChan, "cellSpursWaitForWorkloadShutdown(SPURS {:#x}, workload {}) -> {:#x}", spurs, wid,
        static_cast<u32>(rc));
    return rc;
}

// cellSpursRemoveWorkload, as RPCS3 (cellSpurs.cpp): NULL, ALIGN, INVAL, SRCH, STAT; BUSY while the workload shuts
// down, STAT unless it is removable; the workload flag released if it receives it; then in one update of the state line
// the state NON_EXISTENT and its enabled and mskB bits cleared. The native instance forgets the workload too.
s32 remove_workload(PPUContext& ctx, u32 spurs, u32 wid)
{
    u8* base = guest_base(ctx);
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    switch (ps_read8(base, spurs + wkl_state(wid))) {
    case SPURS_WKL_STATE_SHUTTING_DOWN: return error(CELL_SPURS_POLICY_MODULE_ERROR_BUSY);
    case SPURS_WKL_STATE_REMOVABLE: break;
    default: return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    }
    if (ps_read8(base, spurs + spurs_off::wklFlagReceiver) == wid) {
        if (const s32 r = _cellSpursWorkloadFlagReceiver(ctx, spurs, wid, 0); r != lv2::CELL_OK) {
            PS_FATAL(kChan, "remove_workload: releasing workload {}'s flag returned {:#x} (RPCS3 ensures it succeeds)", wid,
                static_cast<u32>(r));
        }
    }
    constexpr u32 kLine = wkl_off::state1; // both state tables, enabled and mskB share the line at 0x80
    static_assert(wkl_off::enabled - kLine < 128 && wkl_off::mskB - kLine < 128 && wkl_off::state2 + 15 - kLine < 128);
    s32 rc = lv2::CELL_OK;
    reservation_op(base, spurs + kLine, [&](u8* line) {
        u8& state = line[wkl_state(wid) - kLine];
        if (state == SPURS_WKL_STATE_SHUTTING_DOWN) {
            rc = error(CELL_SPURS_POLICY_MODULE_ERROR_BUSY); // re-verified, as RPCS3 (libsre does not)
            return false;
        }
        if (state != SPURS_WKL_STATE_REMOVABLE) {
            rc = error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
            return false;
        }
        state = 0; // SPURS_WKL_STATE_NON_EXISTENT
        const u32 bit = 0x80000000u >> wid;
        ps_write32(line, wkl_off::enabled - kLine, ps_read32(line, wkl_off::enabled - kLine) & ~bit);
        ps_write32(line, wkl_off::mskB - kLine, ps_read32(line, wkl_off::mskB - kLine) & ~bit);
        rc = lv2::CELL_OK;
        return true;
    });
    if (rc == lv2::CELL_OK) {
        SpursHost& h = host();
        std::lock_guard lock(h.mutex);
        if (const auto it = h.instances.find(spurs); it != h.instances.end()) it->second.workloads.erase(wid);
    }
    return rc;
}

// cellSpursRemoveWorkload(spurs, wid): remove_workload.
s32 cellSpursRemoveWorkload(PPUContext& ctx, u32 spurs, u32 wid)
{
    const s32 rc = remove_workload(ctx, spurs, wid);
    PS_LOG_INFO(kChan, "cellSpursRemoveWorkload(SPURS {:#x}, workload {}) -> {:#x}", spurs, wid, static_cast<u32>(rc));
    return rc;
}

// cellSpursJoinTaskset(taskset). RPCS3 leaves it unimplemented (returns CELL_OK); the SDK makes it wait for the
// taskset's shutdown and release its workload, after which the game may free the taskset (D-136). Its checks are
// cellSpursShutdownTaskset's: NULL, ALIGN, a workload ID past 31 (INVAL); the wait's POLICY_MODULE_ERROR_STAT becomes
// TASK_ERROR_STAT, as cellSpursShutdownTaskset converts it.
s32 cellSpursJoinTaskset(PPUContext& ctx, u32 taskset)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, taskset, 128)) {
        log_call_site(ctx, base, std::format("cellSpursJoinTaskset: taskset {:#x} is not readable guest memory; refused with "
            "CELL_SPURS_TASK_ERROR_FAULT", taskset));
        return error(CELL_SPURS_TASK_ERROR_FAULT);
    }
    const u32 wid = ps_read32(base, taskset + ts_off::wid);
    if (wid >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
    const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
    s32 r = wait_for_workload_shutdown(ctx, spurs, wid);
    if (r == lv2::CELL_OK) r = remove_workload(ctx, spurs, wid);
    PS_LOG_INFO(kChan, "cellSpursJoinTaskset({:#010x}): workload {} of SPURS {:#x} -> {:#x}", taskset, wid, spurs,
        static_cast<u32>(r));
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

// RPCS3 cellSpursSendWorkloadSignal (libsre agrees): NULL, ALIGN, INVAL (past the instance's workloads), SRCH, STAT (an
// exception, a workload not runnable); then the workload's bit in wklSignal1 (SPURS2's 16..31: wklSignal2), which the
// native selection reads, in one update of CellSpurs line 0 (which wakes idle workers).
s32 send_workload_signal(u8* base, u32 spurs, u32 wid)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    if (ps_read8(base, spurs + wkl_state(wid)) != SPURS_WKL_STATE_RUNNABLE) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    reservation_op(base, spurs, [wid](u8* line) {
        ps_write16(line, wkl_signal_of(wid), static_cast<u16>(ps_read16(line, wkl_signal_of(wid)) | wkl_signal_bit(wid)));
        return true;
    });
    return lv2::CELL_OK;
}

s32 cellSpursSendWorkloadSignal(PPUContext& ctx, u32 spurs, u32 wid)
{
    const s32 rc = send_workload_signal(guest_base(ctx), spurs, wid);
    PS_LOG_DEBUG(kChan, "cellSpursSendWorkloadSignal(SPURS {:#x}, workload {}) -> {:#x}", spurs, wid, static_cast<u32>(rc));
    return rc;
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

// _cellSpursSendSignal(taskset, taskId), as RPCS3 (cellSpurs.cpp) and libsre (fw 4.92): NULL, 128-byte ALIGN, a task
// ID past 127 or a workload ID past 31 (INVAL; an unreadable taskset is refused with TASK_ERROR_FAULT and a log, D-042),
// then the task's signalled bit in one reservation operation on the taskset's first line, SRCH with nothing changed
// when the task is not enabled or its bits are inconsistent (signal_task). A task that was waiting for the signal gets
// its workload signalled (cellSpursSendWorkloadSignal, its result ignored) and SPURS woken (cellSpursWakeUp: STAT
// becomes TASK_ERROR_STAT, another error is returned as RPCS3 returns it; libsre asserts it is CELL_OK).
s32 _cellSpursSendSignal(PPUContext& ctx, u32 taskset, u32 task_id)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (task_id >= CELL_SPURS_MAX_TASK) return error(CELL_SPURS_TASK_ERROR_INVAL);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, taskset, 128)) {
        log_call_site(ctx, base, std::format("_cellSpursSendSignal: taskset {:#x} is not readable guest memory; refused "
            "with CELL_SPURS_TASK_ERROR_FAULT", taskset));
        return error(CELL_SPURS_TASK_ERROR_FAULT);
    }
    const u32 wid = ps_read32(base, taskset + ts_off::wid);
    if (wid >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
    const int signal = signal_task(base, taskset, task_id);
    if (signal < 0) return error(CELL_SPURS_TASK_ERROR_SRCH);
    if (signal == 0) return lv2::CELL_OK;
    const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
    send_workload_signal(base, spurs, wid);
    const s32 rc = cellSpursWakeUp(ctx, spurs);
    return rc == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT) ? error(CELL_SPURS_TASK_ERROR_STAT) : rc;
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
    static std::atomic<u64> created{0}; // the first 8 and every 1024th, as the task starts (spurs_taskset.cpp)
    if (const u64 n = ++created; n <= 8 || n % 1024 == 0) {
        PS_LOG_INFO(kChan, "task {} of taskset {:#010x} (workload {}): ELF {:#x}, context {:#x} ({:#x} bytes, {} LS "
                           "blocks); ready (task {} created)",
            id, taskset, wid, s.elf, s.context, s.context_size, saved_blocks, n);
    }
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

// cellSpursTaskAttributeSetExitCodeContainer(attr, container), as libsre (0x10BA4): NULL (attr, container), the
// attribute's 8-byte and the container's 128-byte ALIGN, revision 1 (INVAL); then the container at 0x40.
s32 cellSpursTaskAttributeSetExitCodeContainer(PPUContext& ctx, u32 attr, u32 container)
{
    if (!attr || !container) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (attr % 8 || container % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = task_readable(ctx, "cellSpursTaskAttributeSetExitCodeContainer", "attribute", attr,
            taskattr_off::exit_code_container + 8);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, attr + taskattr_off::revision) != CELL_SPURS_TASK_ATTRIBUTE_REVISION) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    ps_write64(base, attr + taskattr_off::exit_code_container, container);
    return lv2::CELL_OK;
}

// cellSpursTaskExitCodeInitialize(container), as libsre (0x13638): NULL, the 128-byte ALIGN, then the 128 bytes zeroed.
s32 cellSpursTaskExitCodeInitialize(PPUContext& ctx, u32 container)
{
    if (!container) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (container % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = task_readable(ctx, "cellSpursTaskExitCodeInitialize", "container", container, 128);
        rc != lv2::CELL_OK) {
        return rc;
    }
    std::memset(guest_base(ctx) + container, 0, 128);
    ps_res_notify_write(container, 128);
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
            const u32 wid = ps_read8(base, event_flag + ef_off::waitingTaskWklId + slot) & 31;
            taskset = static_cast<u32>(ps_read64(base, wkl_info(taskset, wid) + wkl_off::info_arg));
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

// cellSpursEventFlagTryWait(eventFlag, mask, mode), as RPCS3: event_flag_wait without blocking (BUSY when the
// condition is not met; no lv2 queue needed).
s32 cellSpursEventFlagTryWait(PPUContext& ctx, u32 event_flag, u32 mask, u32 mode)
{
    return event_flag_wait(ctx, event_flag, mask, mode, false);
}

// cellSpursEventFlagClear(eventFlag, bits), as RPCS3 (libsre agrees): NULL, ALIGN (128), then the bits cleared from the
// events in one update of the flag's line (SPU tasks update it with GETLLAR/PUTLLC; libsre with ldarx/stdcx.).
s32 cellSpursEventFlagClear(PPUContext& ctx, u32 event_flag, u16 bits)
{
    if (!event_flag) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (event_flag % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursEventFlagClear", "event flag", event_flag, ef_off::size,
            CELL_SPURS_TASK_ERROR_FAULT);
        rc != lv2::CELL_OK) {
        return rc;
    }
    reservation_op(guest_base(ctx), event_flag, [bits](u8* ef) {
        const u16 events = ps_read16(ef, ef_off::events);
        if (!(events & bits)) return false; // nothing to clear: nothing stored
        ps_write16(ef, ef_off::events, static_cast<u16>(events & ~bits));
        return true;
    });
    return lv2::CELL_OK;
}

// The event flag getters, as libsre: the flag NULL, its 128-byte ALIGN, then the output NULL (RPCS3 checks both NULLs
// before the alignment). A flag or output that is not guest memory is refused with TASK_ERROR_FAULT (D-042).
s32 check_event_flag_getter(PPUContext& ctx, std::string_view fn, u32 event_flag, u32 out)
{
    if (!event_flag) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (event_flag % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (!out) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (const s32 rc = check_pointer(ctx, fn, "event flag", event_flag, ef_off::size, CELL_SPURS_TASK_ERROR_FAULT);
        rc != lv2::CELL_OK) {
        return rc;
    }
    return check_pointer(ctx, fn, "output", out, 4, CELL_SPURS_TASK_ERROR_FAULT);
}

// cellSpursEventFlagGetDirection(eventFlag, direction): the flag's direction (u32; libsre sign-extends the byte).
s32 cellSpursEventFlagGetDirection(PPUContext& ctx, u32 event_flag, u32 direction)
{
    if (const s32 rc = check_event_flag_getter(ctx, "cellSpursEventFlagGetDirection", event_flag, direction); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, direction, static_cast<u32>(static_cast<s8>(ps_read8(base, event_flag + ef_off::direction))));
    return lv2::CELL_OK;
}

// cellSpursEventFlagGetClearMode(eventFlag, clearMode): the flag's clear mode (u32).
s32 cellSpursEventFlagGetClearMode(PPUContext& ctx, u32 event_flag, u32 clear_mode)
{
    if (const s32 rc = check_event_flag_getter(ctx, "cellSpursEventFlagGetClearMode", event_flag, clear_mode); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, clear_mode, static_cast<u32>(static_cast<s8>(ps_read8(base, event_flag + ef_off::clearMode))));
    return lv2::CELL_OK;
}

// cellSpursEventFlagGetTasksetAddress(eventFlag, taskset): the flag's taskset (u32), 0 for one made for the whole
// SPURS instance (isIwl).
s32 cellSpursEventFlagGetTasksetAddress(PPUContext& ctx, u32 event_flag, u32 taskset)
{
    if (const s32 rc = check_event_flag_getter(ctx, "cellSpursEventFlagGetTasksetAddress", event_flag, taskset);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, taskset, ps_read8(base, event_flag + ef_off::isIwl) ? 0 : static_cast<u32>(ps_read64(base, event_flag + ef_off::addr)));
    return lv2::CELL_OK;
}

// _cellSpursLFQueueInitialize(tasksetOrSpurs, queue, buffer, size, depth, direction), as libsre: a 16-byte aligned
// tasksetOrSpurs is a taskset, whose workload ID must be at most 31 (TASK_ERROR_INVAL; RPCS3 checks nothing); then
// cellSyncLFQueueInitialize(queue, buffer, size, depth, direction, tasksetOrSpurs) (cellSync.cpp), its error becoming the
// task error with the same low byte (0x80410900 | code & 0xFF, RPCS3's SyncErrorToSpursError). A taskset that is not
// guest memory is refused with TASK_ERROR_FAULT (D-042).
s32 _cellSpursLFQueueInitialize(PPUContext& ctx, u32 taskset_or_spurs, u32 queue, u32 buffer, u32 size, u32 depth,
    u32 direction)
{
    if (taskset_or_spurs % 16 == 0) {
        if (const s32 rc = check_pointer(ctx, "_cellSpursLFQueueInitialize", "taskset", taskset_or_spurs + ts_off::wid, 4,
                CELL_SPURS_TASK_ERROR_FAULT);
            rc != lv2::CELL_OK) {
            return rc;
        }
        if (ps_read32(guest_base(ctx), taskset_or_spurs + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) {
            return error(CELL_SPURS_TASK_ERROR_INVAL);
        }
    }
    const s32 rc = sync_lfqueue_initialize(ctx, queue, buffer, size, depth, direction, taskset_or_spurs);
    return rc < 0 ? static_cast<s32>(0x80410900u | (static_cast<u32>(rc) & 0xFF)) : rc;
}

// cellSpursLFQueueGetTasksetAddress(queue, taskset), as libsre (0x17D40): the output NULL (NULL_POINTER); the queue's
// signal address (_cellSyncLFQueueGetSignalAddress's checks: NULL, 128-byte alignment, as task errors with the same
// low byte), which a SPURS LF queue holds its taskset in (_cellSpursLFQueueInitialize), 0 when its bit 0 says it is a
// queue of the whole SPURS instance. Memory that is not guest memory: TASK_ERROR_FAULT (D-042).
s32 cellSpursLFQueueGetTasksetAddress(PPUContext& ctx, u32 queue, u32 taskset)
{
    constexpr std::string_view fn = "cellSpursLFQueueGetTasksetAddress";
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (!queue) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (queue % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "queue", queue, 128, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) {
        return rc;
    }
    if (const s32 rc = check_pointer(ctx, fn, "output", taskset, 4, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const u32 signal = static_cast<u32>(ps_read64(base, queue + 0x70)); // CellSyncLFQueue::m_eaSignal
    ps_write32(base, taskset, signal & 1 ? 0 : signal);
    return lv2::CELL_OK;
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

// cellSpursBarrierGetTasksetAddress(barrier, taskset), as libsre: the barrier NULL, its 128-byte ALIGN, the output NULL
// (RPCS3 checks both NULLs first); then the barrier's taskset (the low word of its u64 at 0x30). Memory that is not
// guest memory is refused with TASK_ERROR_FAULT (D-042).
s32 cellSpursBarrierGetTasksetAddress(PPUContext& ctx, u32 barrier, u32 taskset)
{
    constexpr std::string_view fn = "cellSpursBarrierGetTasksetAddress";
    if (!barrier) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (barrier % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (const s32 rc = check_pointer(ctx, fn, "barrier", barrier, 128, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) {
        return rc;
    }
    if (const s32 rc = check_pointer(ctx, fn, "output", taskset, 4, CELL_SPURS_TASK_ERROR_FAULT); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, taskset, ps_read32(base, barrier + 0x34));
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

// libsre's create_job_chain, which cellSpursCreateJobChain and cellSpursCreateJobChainWithAttribute share (fw 4.92,
// D-054; RPCS3 _spurs::create_job_chain has the same steps but swaps sizeJobDescriptor and maxGrabbedJob and leaves
// out autoReadyCount and haltOnError): the tags, the haltOnError bit of the flags byte, the entry, sizes, autoReadyCount,
// the SPURS address, the revision by the process's SDK (1 from 1.5, else 0), the workload (the native job chain marker as
// its policy module, minimum contention 1; a policy module error AGAIN, INVAL or STAT becomes the job error), then the
// error, its cause and the workload ID. Deviations: no "JobChain" class name, and libsre's one-title firmware quirk
// (val2F) is not reproduced (no title checks in the runtime, rule 6).
s32 create_job_chain(u8* base, u32 spurs, u32 job_chain, u32 entry, u16 size_job, u16 max_grabbed, const u8 (&priority)[8],
    u32 max_contention, bool auto_ready_count, u32 tag1, u32 tag2, bool halt_on_error, u32 name, u32& wid)
{
    const u32 sdk = HleProcess::current()->kernel().config().sdk_version;
    ps_write8(base, job_chain + jc_off::tag1, static_cast<u8>(tag1));
    ps_write8(base, job_chain + jc_off::tag2, static_cast<u8>(tag2));
    const u8 flags = ps_read8(base, job_chain + jc_off::flags);
    ps_write8(base, job_chain + jc_off::flags,
        static_cast<u8>((flags & ~jc_off::flagHaltOnError) | (halt_on_error ? jc_off::flagHaltOnError : 0)));
    ps_write64(base, job_chain + jc_off::pc, entry);
    ps_write16(base, job_chain + jc_off::sizeJobDescriptor, size_job);
    ps_write16(base, job_chain + jc_off::maxGrabbedJob, max_grabbed);
    ps_write8(base, job_chain + jc_off::autoReadyCount, auto_ready_count ? 1 : 0);
    ps_write64(base, job_chain + jc_off::spurs, spurs);
    ps_write8(base, job_chain + jc_off::isHalted, 0);
    ps_write8(base, job_chain + jc_off::jmVer, sdk > 0x14FFFF ? CELL_SPURS_JOB_REVISION_1 : CELL_SPURS_JOB_REVISION_0);
    ps_write8(base, job_chain + jc_off::val2F, 0);
    if (s32 r = add_workload(base, spurs, wid, SPURS_IMG_ADDR_JOB_CHAIN_PM, 0, job_chain, priority, 1, max_contention, 0,
            name, Workload::Kind::JobChain);
        r != lv2::CELL_OK) {
        if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_AGAIN)) r = error(CELL_SPURS_JOB_ERROR_AGAIN);
        else if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL)) r = error(CELL_SPURS_JOB_ERROR_INVAL);
        else if (r == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT)) r = error(CELL_SPURS_JOB_ERROR_STAT);
        return r;
    }
    ps_write64(base, job_chain + jc_off::cause, 0);
    ps_write32(base, job_chain + jc_off::error, 0);
    ps_write32(base, job_chain + jc_off::workloadId, wid);
    return lv2::CELL_OK;
}

// cellSpursCreateJobChain(spurs, jobChain, jobChainEntry, sizeJobDescriptor, maxGrabbedJob, priorities, maxContention,
// autoReadyCount, tag1, tag2) (the old API; tag1 and tag2 come from the caller's stack), as libsre: its argument check
// (the entry NULL or not 8-aligned; INVAL for a grab limit outside 1..16, a tag above 31, a priority above 15), INVAL
// for a descriptor size other than 64, 128 or 256, then NULL and ALIGN (128) for the SPURS instance and the chain, the
// chain's 0x110 bytes cleared and create_job_chain with no name and no haltOnError: a chain of revision 0 or 1, which
// cellSpursKickJobChain starts. RPCS3's always fails: it checks the attribute fields the old API has not (a maximum
// descriptor size of 0 is INVAL). An unreadable priority table is refused with INVAL and a log (D-042).
s32 cellSpursCreateJobChain(PPUContext& ctx, u32 spurs, u32 job_chain, u32 entry, u16 size_job, u16 max_grabbed,
    u32 priorities, u32 max_contention, u32 auto_ready_count, u32 tag1, u32 tag2)
{
    PS_LOG_INFO(kChan, "cellSpursCreateJobChain(spurs {:#x}, jobChain {:#x}, entry {:#x}, sizeJob {:#x}, maxGrab {}, "
        "maxContention {}, autoReadyCount {}, tags {}/{})", spurs, job_chain, entry, size_job, max_grabbed, max_contention,
        auto_ready_count & 0xFF, tag1, tag2);
    if (!entry) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (entry % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!max_grabbed || max_grabbed > 16 || tag1 > 31 || tag2 > 31) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (const s32 rc = check_pointer(ctx, "cellSpursCreateJobChain", "priority table", priorities, 8,
            CELL_SPURS_JOB_ERROR_INVAL);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    u8 priority[8];
    for (u32 i = 0; i < 8; i++) {
        priority[i] = ps_read8(base, priorities + i);
        if (priority[i] > 15) return error(CELL_SPURS_JOB_ERROR_INVAL);
    }
    if (size_job != 64 && size_job != 128 && size_job != 256) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (!spurs || !job_chain) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || job_chain % 128) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursCreateJobChain", "job chain", job_chain, 0x110,
            CELL_SPURS_JOB_ERROR_INVAL);
        rc != lv2::CELL_OK) {
        return rc;
    }
    std::memset(base + job_chain, 0, 0x110);
    u32 wid = 0;
    if (const s32 rc = create_job_chain(base, spurs, job_chain, entry, size_job, max_grabbed, priority, max_contention,
            (auto_ready_count & 0xFF) != 0, tag1, tag2, false, 0, wid);
        rc != lv2::CELL_OK) {
        return rc;
    }
    PS_LOG_INFO(kChan, "job chain {:#010x} (old API, revision {}) is workload {} of SPURS {:#010x}", job_chain,
        ps_read8(base, job_chain + jc_off::jmVer), wid, spurs);
    return lv2::CELL_OK;
}

// cellSpursCreateJobChainWithAttribute as libsre (fw 4.92, D-054): the attribute's checks, NULL and ALIGN for the chain
// and the SPURS instance, the chain cleared, revision 2 or 3 only (INVAL), the flags byte (fixed memory, the maximum
// descriptor size, haltOnError), create_job_chain, then the requested SPU count, the attribute's revision, SDK version
// and memory check.
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
        static_cast<u8>((fixed_mem ? jc_off::flagFixedMem : 0) | ((((max_size_job - 0x100) / 128) & 7) << 4)));
    u8 priority[8];
    for (u32 i = 0; i < 8; i++) priority[i] = static_cast<u8>(prio >> (56 - 8 * i));
    u32 wid = 0;
    if (const s32 r = create_job_chain(base, spurs, job_chain, entry, size_job, max_grabbed, priority, max_contention,
            auto_spu_count, tag1, tag2, halt_on_error, ps_read32(base, attr + jcattr_off::name), wid);
        r != lv2::CELL_OK) {
        return r;
    }
    // back in cellSpursCreateJobChainWithAttribute
    ps_write8(base, job_chain + jc_off::initSpuCount, static_cast<u8>(init_spu_count));
    ps_write8(base, job_chain + jc_off::jmVer, static_cast<u8>(jm_ver));
    ps_write32(base, job_chain + jc_off::sdkVer, ps_read32(base, attr + jcattr_off::sdkVer));
    ps_write8(base, job_chain + jc_off::jobMemoryCheck, static_cast<u8>(ps_read8(base, attr + jcattr_off::jobMemoryCheck) << 1));
    std::string label;
    if (const u32 name = ps_read32(base, attr + jcattr_off::name)) label = reinterpret_cast<const char*>(base + name);
    PS_LOG_INFO(kChan, "job chain {:#010x} \"{}\" is workload {} of SPURS {:#010x} (entry {:#x}, descriptors {:#x} bytes, "
                       "max grab {}, max contention {}, priorities {}, {} ready count{})",
        job_chain, label, wid, spurs, entry, size_job, max_grabbed, max_contention, priority_text(priority),
        auto_spu_count ? "auto" : "fixed", halt_on_error ? ", halts on error" : "");
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

// The job chain's workload ID and SPURS after RPCS3's checks: NULL, 128-byte ALIGN, a workload ID past 31 (INVAL); an
// unreadable chain is refused with INVAL and a log, as cellSpursRunJobChain does.
bool job_chain_workload(PPUContext& ctx, std::string_view what, u32 job_chain, u32& wid, u32& spurs, s32& rc)
{
    rc = lv2::CELL_OK;
    if (!job_chain) rc = error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    else if (job_chain % 128) rc = error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (rc != lv2::CELL_OK) return false;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, job_chain, 128)) {
        log_call_site(ctx, base, std::format("{}: job chain {:#x} is not readable guest memory; refused with "
            "CELL_SPURS_JOB_ERROR_INVAL", what, job_chain));
        rc = error(CELL_SPURS_JOB_ERROR_INVAL);
        return false;
    }
    wid = ps_read32(base, job_chain + jc_off::workloadId);
    spurs = static_cast<u32>(ps_read64(base, job_chain + jc_off::spurs));
    if (wid >= CELL_SPURS_MAX_WORKLOAD2) {
        rc = error(CELL_SPURS_JOB_ERROR_INVAL);
        return false;
    }
    return true;
}

// cellSpursShutdownJobChain(jobChain), as RPCS3 (cellSpurs.cpp): the checks, then cellSpursShutdownWorkload on the
// chain's workload; its POLICY_MODULE_ERROR_STAT becomes JOB_ERROR_STAT, other results pass as they are.
s32 cellSpursShutdownJobChain(PPUContext& ctx, u32 job_chain)
{
    u32 wid = 0, spurs = 0;
    s32 rc;
    if (!job_chain_workload(ctx, "cellSpursShutdownJobChain", job_chain, wid, spurs, rc)) return rc;
    rc = shutdown_workload(ctx, spurs, wid);
    PS_LOG_INFO(kChan, "cellSpursShutdownJobChain({:#010x}): workload {} of SPURS {:#x} -> {:#x}", job_chain, wid, spurs,
        static_cast<u32>(rc));
    return rc == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT) ? error(CELL_SPURS_JOB_ERROR_STAT) : rc;
}

// cellSpursJoinJobChain(jobChain), as RPCS3 (cellSpurs.cpp): the checks, cellSpursWaitForWorkloadShutdown (STAT becomes
// JOB_ERROR_STAT) and cellSpursRemoveWorkload (its result as it is); then the chain's workload ID becomes 32 and the
// chain's error word is the result. The native wait also waits for the workers to leave the chain (D-136).
s32 cellSpursJoinJobChain(PPUContext& ctx, u32 job_chain)
{
    u32 wid = 0, spurs = 0;
    s32 rc;
    if (!job_chain_workload(ctx, "cellSpursJoinJobChain", job_chain, wid, spurs, rc)) return rc;
    rc = wait_for_workload_shutdown(ctx, spurs, wid);
    if (rc != lv2::CELL_OK) return rc == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT) ? error(CELL_SPURS_JOB_ERROR_STAT) : rc;
    rc = remove_workload(ctx, spurs, wid);
    if (rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    ps_write32(base, job_chain + jc_off::workloadId, CELL_SPURS_MAX_WORKLOAD2);
    const s32 chain_error = static_cast<s32>(ps_read32(base, job_chain + jc_off::error));
    PS_LOG_INFO(kChan, "cellSpursJoinJobChain({:#010x}): workload {} of SPURS {:#x} removed; the chain's error {:#x}",
        job_chain, wid, spurs, static_cast<u32>(chain_error));
    return chain_error;
}

// cellSpursAddUrgentCommand(jobChain, command), as RPCS3 (cellSpurs.cpp) and libsre (fw 4.92): the checks of
// job_chain_workload (NULL, 128-byte ALIGN, a workload ID past 31 is INVAL; an unreadable chain too, with a log), then
// in one reservation operation on the chain's first line the command goes into the first of its four urgent command
// slots that is 0; BUSY when all four are taken. libsre's ldarx/stdcx. walk over the slots ends in the same slot.
// Nothing is signalled: the chain's next visit runs it (spurs_jobchain.cpp takes slot 0 and moves the others up).
s32 cellSpursAddUrgentCommand(PPUContext& ctx, u32 job_chain, u64 command)
{
    u32 wid = 0, spurs = 0;
    s32 rc;
    if (!job_chain_workload(ctx, "cellSpursAddUrgentCommand", job_chain, wid, spurs, rc)) return rc;
    reservation_op(guest_base(ctx), job_chain, [&](u8* line) {
        for (u32 k = 0; k < 4; k++) {
            if (ps_read64(line, jc_off::urgentCmds + 8 * k) == 0) {
                ps_write64(line, jc_off::urgentCmds + 8 * k, command);
                rc = lv2::CELL_OK;
                return true;
            }
        }
        rc = error(CELL_SPURS_JOB_ERROR_BUSY);
        return false;
    });
    PS_LOG_DEBUG(kChan, "cellSpursAddUrgentCommand({:#010x}, {:#018x}): workload {} of SPURS {:#x} -> {:#x}", job_chain,
        command, wid, spurs, static_cast<u32>(rc));
    return rc;
}

// cellSpursAddUrgentCall(jobChain, commandList), as RPCS3 (libsre agrees): the list NULL, not 8-aligned (ALIGN), then
// cellSpursAddUrgentCommand with CALL to the list (its address | 4).
s32 cellSpursAddUrgentCall(PPUContext& ctx, u32 job_chain, u32 command_list)
{
    if (!command_list) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (command_list % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    return cellSpursAddUrgentCommand(ctx, job_chain, u64{command_list} | 4);
}

// The checks of the job chain getters and setters below, as libsre: the chain NULL, `outs` NULL (NULL_POINTER), the
// chain 128-byte aligned (ALIGN), and with `check_wid` a workload ID past 31 (INVAL). A chain or output that is not guest
// memory is refused with INVAL and a log, as cellSpursRunJobChain does.
s32 check_job_chain(PPUContext& ctx, std::string_view fn, u32 job_chain, std::initializer_list<u32> outs, bool check_wid)
{
    if (!job_chain) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    for (const u32 out : outs)
        if (!out) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (job_chain % 128) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "job chain", job_chain, 0xA8, CELL_SPURS_JOB_ERROR_INVAL); rc != lv2::CELL_OK) {
        return rc;
    }
    if (check_wid && ps_read32(guest_base(ctx), job_chain + jc_off::workloadId) >= CELL_SPURS_MAX_WORKLOAD2) {
        return error(CELL_SPURS_JOB_ERROR_INVAL);
    }
    for (const u32 out : outs) {
        if (const s32 rc = check_pointer(ctx, fn, "output", out, 4, CELL_SPURS_JOB_ERROR_INVAL); rc != lv2::CELL_OK) return rc;
    }
    return lv2::CELL_OK;
}

// cellSpursKickJobChain(jobChain, numReadyCount), as libsre: NULL, ALIGN, INVAL (a workload ID past 31), PERM for a chain
// of revision 2 or 3 (those run with cellSpursRunJobChain); a chain with autoReadyCount gets ready count 1 if it had
// none (cellSpursReadyCountCompareAndSwap 0 -> 1: its policy keeps the count itself from then on), one without it the
// count given (cellSpursReadyCountStore), either result ignored; then cellSpursWakeUp, whose STAT becomes JOB_ERROR_STAT
// and any other result CELL_OK. RPCS3 has the two ready count operations the other way round and passes the wake-up's
// result through. A worker then runs the chain with the native job chain policy.
s32 cellSpursKickJobChain(PPUContext& ctx, u32 job_chain, u32 num_ready_count)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursKickJobChain", job_chain, {}, true); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    if (ps_read8(base, job_chain + jc_off::jmVer) > CELL_SPURS_JOB_REVISION_1) return error(CELL_SPURS_JOB_ERROR_PERM);
    const u32 wid = ps_read32(base, job_chain + jc_off::workloadId);
    const u32 spurs = static_cast<u32>(ps_read64(base, job_chain + jc_off::spurs));
    StackVar old(ctx, base, 8);
    if (ps_read8(base, job_chain + jc_off::autoReadyCount)) {
        cellSpursReadyCountCompareAndSwap(ctx, spurs, wid, old.addr(), 0, 1);
    } else {
        cellSpursReadyCountStore(ctx, spurs, wid, num_ready_count & 0xFF);
    }
    const s32 woken = cellSpursWakeUp(ctx, spurs);
    PS_LOG_DEBUG(kChan, "cellSpursKickJobChain({:#010x}, {}): workload {} of SPURS {:#x}", job_chain, num_ready_count & 0xFF,
        wid, spurs);
    return woken == error(CELL_SPURS_POLICY_MODULE_ERROR_STAT) ? error(CELL_SPURS_JOB_ERROR_STAT) : lv2::CELL_OK;
}

// cellSpursGetJobChainId(jobChain, id), as RPCS3 (libsre agrees): NULL (both), ALIGN, then the workload ID.
s32 cellSpursGetJobChainId(PPUContext& ctx, u32 job_chain, u32 id)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursGetJobChainId", job_chain, {id}, false); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    ps_write32(base, id, ps_read32(base, job_chain + jc_off::workloadId));
    return lv2::CELL_OK;
}

// cellSpursJobChainGetError(jobChain, cause), as libsre: NULL (both; RPCS3 does not check cause), ALIGN; the chain's
// error word is the result, and *cause its cause (u32), or 0 when there is no error.
s32 cellSpursJobChainGetError(PPUContext& ctx, u32 job_chain, u32 cause)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursJobChainGetError", job_chain, {cause}, false); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const s32 chain_error = static_cast<s32>(ps_read32(base, job_chain + jc_off::error));
    ps_write32(base, cause, chain_error ? static_cast<u32>(ps_read64(base, job_chain + jc_off::cause)) : 0);
    return chain_error;
}

// cellSpursJobChainGetSpursAddress(jobChain, spurs), as libsre: NULL (both), ALIGN, INVAL for a workload ID past 31 (RPCS3
// does not check it); then the chain's CellSpurs (u32).
s32 cellSpursJobChainGetSpursAddress(PPUContext& ctx, u32 job_chain, u32 spurs)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursJobChainGetSpursAddress", job_chain, {spurs}, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, spurs, static_cast<u32>(ps_read64(base, job_chain + jc_off::spurs)));
    return lv2::CELL_OK;
}

// cellSpursJobChainAttributeSetJobTypeMemoryCheck(attr), as RPCS3 (libsre agrees): NULL, ALIGN (8), then the attribute's
// jobMemoryCheck, which cellSpursCreateJobChainWithAttribute gives the chain. The memory check itself is not done
// natively (spurs_jobchain.cpp: the jobs run without it, logged once).
s32 cellSpursJobChainAttributeSetJobTypeMemoryCheck(PPUContext& ctx, u32 attr)
{
    if (!attr) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (attr % 8) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    ps_write8(guest_base(ctx), attr + jcattr_off::jobMemoryCheck, 1);
    return lv2::CELL_OK;
}

// cellSpursJobChainSetExceptionEventHandler(jobChain, handler, arg), as RPCS3 (libsre agrees): NULL (chain, handler),
// ALIGN, INVAL (a workload ID past 31), BUSY when one is set; then the argument (u64 at 0xA0) and the handler (0x98). A
// job that faults stops its worker with a log natively (CLAUDE.md rule 5) rather than reaching the handler.
s32 cellSpursJobChainSetExceptionEventHandler(PPUContext& ctx, u32 job_chain, u32 handler, u32 arg)
{
    if (!job_chain || !handler) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (const s32 rc = check_job_chain(ctx, "cellSpursJobChainSetExceptionEventHandler", job_chain, {}, true);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read64(base, job_chain + jc_off::exceptionHandler)) return error(CELL_SPURS_JOB_ERROR_BUSY);
    ps_write64(base, job_chain + jc_off::exceptionHandlerArg, arg);
    ps_write64(base, job_chain + jc_off::exceptionHandler, handler);
    return lv2::CELL_OK;
}

// cellSpursJobChainUnsetExceptionEventHandler(jobChain), as RPCS3 (libsre agrees): NULL, ALIGN, INVAL; both cleared.
s32 cellSpursJobChainUnsetExceptionEventHandler(PPUContext& ctx, u32 job_chain)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursJobChainUnsetExceptionEventHandler", job_chain, {}, true);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write64(base, job_chain + jc_off::exceptionHandlerArg, 0);
    ps_write64(base, job_chain + jc_off::exceptionHandler, 0);
    return lv2::CELL_OK;
}

// CellSpursWorkloadInfo's fields as libsre's cellSpursGetWorkloadInfo fills them (RPCS3's is a TODO), for
// cellSpursGetJobChainInfo, which only reads the instance name: NULL, ALIGN, INVAL, SRCH (no exception check), then the
// name from wklH (wklH2 for SPURS2's 16..31).
s32 workload_name_instance(u8* base, u32 spurs, u32 wid, u32& name)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    if (wid >= max_workloads(base, spurs)) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!workload_enabled(base, spurs, wid)) return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    name = static_cast<u32>(ps_read64(base, wkl_h(spurs, wid) + 8));
    return lv2::CELL_OK;
}

// cellSpursJobHeaderSetJobbin2Param(header, binary), as libsre (0x1AE64; RPCS3 has none): a job's code in the "jobbin2"
// format, an SPU ELF whose program headers are three (code R+X, data R+W, a note) or five (three R+X, data R+W, a note)
// from offset 52, and whose first segment's file data, 16-aligned, has "BIN2" or "bin2" at +32. NULL (header, binary),
// the binary's 16-byte ALIGN, any other shape (INVAL; a second writable segment too). Then the job header gets the
// first segment's address (the low word of eaBinary), the size of what is loaded less the writable segment's bss
// (sizeBinary, 16-byte units) and the bss in 16-byte units (in eaBinary's upper half, at +2), and flag 4 at +44. A
// binary libsre would fault on reading is refused with JOB_ERROR_NULL_POINTER and logged (D-042).
s32 cellSpursJobHeaderSetJobbin2Param(PPUContext& ctx, u32 header, u32 bin)
{
    constexpr std::string_view fn = "cellSpursJobHeaderSetJobbin2Param";
    if (!header || !bin) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (bin % 16) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "binary", bin, 52, CELL_SPURS_JOB_ERROR_NULL_POINTER); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, bin + 28) != 52) return error(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 phnum = ps_read16(base, bin + 44), ph = bin + 52;
    if (phnum != 3 && phnum != 5) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (const s32 rc = check_pointer(ctx, fn, "program headers", ph, 32 * phnum, CELL_SPURS_JOB_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    const auto load = [&](u32 i, u32 flags) {
        return ps_read32(base, ph + 32 * i) == 1 && (ps_read32(base, ph + 32 * i + 24) & 7) == flags;
    };
    const bool shape = phnum == 5 ? load(0, 5) && load(1, 5) && load(2, 5) && load(3, 6) && ps_read32(base, ph + 128) == 4
                                  : load(0, 5) && load(1, 6) && ps_read32(base, ph + 64) == 4;
    if (!shape) return error(CELL_SPURS_JOB_ERROR_INVAL);
    u32 lo = 0xFFFFFFFFu, hi = 0, bss = 0xFFFFFFFFu; // bss: none yet (libsre's -1)
    for (u32 i = 0; i < phnum; i++) {
        const u32 e = ph + 32 * i;
        if (ps_read32(base, e) != 1) continue;
        const u32 vaddr = ps_read32(base, e + 8), memsz = ps_read32(base, e + 20);
        lo = std::min(lo, vaddr);
        hi = std::max(hi, vaddr + memsz);
        if (ps_read32(base, e + 24) & 2) {
            if (bss != 0xFFFFFFFFu) return error(CELL_SPURS_JOB_ERROR_INVAL);
            bss = memsz - ps_read32(base, e + 16);
        }
    }
    const u32 code = bin + ps_read32(base, ph + 4);
    if (code % 16) return error(CELL_SPURS_JOB_ERROR_INVAL);
    if (const s32 rc = check_pointer(ctx, fn, "binary's tag", code + 32, 4, CELL_SPURS_JOB_ERROR_NULL_POINTER);
        rc != lv2::CELL_OK) {
        return rc;
    }
    const u32 tag = ps_read32(base, code + 32);
    if (tag != 0x42494E32 && tag != 0x62696E32) return error(CELL_SPURS_JOB_ERROR_INVAL); // "BIN2", "bin2"
    if (const s32 rc = check_pointer(ctx, fn, "header", header, 48, CELL_SPURS_JOB_ERROR_NULL_POINTER); rc != lv2::CELL_OK) {
        return rc;
    }
    ps_write16(base, header + 2, static_cast<u16>(static_cast<s32>(bss) / 16));
    ps_write32(base, header + 4, code);
    ps_write16(base, header + 8, static_cast<u16>(((hi - lo) - bss) >> 4));
    ps_write8(base, header + 44, static_cast<u8>(ps_read8(base, header + 44) | 4));
    return lv2::CELL_OK;
}

// cellSpursGetJobChainInfo(jobChain, info), as libsre: NULL (both), ALIGN, INVAL (a workload ID past 31), the
// workload's information (cellSpursGetWorkloadInfo: its SRCH becomes INVAL, other errors pass); then CellSpursJobChainInfo
// (0x54 bytes written): the four urgent command slots, pc, the three link registers (u32 each), the cause, the error,
// the maximum descriptor size, the workload ID, autoReadyCount, isHalted and isFixedMemAlloc (as 0 or 1), the grab
// limit (u16 at 0x44), the workload's instance name and the exception handler and argument. RPCS3 reads the same fields
// at once from the chain's first line, leaves out the grab limit and takes the name from its TODO workload information.
// The first line here is one snapshot as well.
s32 cellSpursGetJobChainInfo(PPUContext& ctx, u32 job_chain, u32 info)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursGetJobChainInfo", job_chain, {info}, true); rc != lv2::CELL_OK) return rc;
    if (const s32 rc = check_pointer(ctx, "cellSpursGetJobChainInfo", "CellSpursJobChainInfo", info, 0x54,
            CELL_SPURS_JOB_ERROR_INVAL);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    const u32 wid = ps_read32(base, job_chain + jc_off::workloadId);
    u32 name = 0;
    if (const s32 rc = workload_name_instance(base, static_cast<u32>(ps_read64(base, job_chain + jc_off::spurs)), wid, name);
        rc != lv2::CELL_OK) {
        return rc == error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH) ? error(CELL_SPURS_JOB_ERROR_INVAL) : rc;
    }
    alignas(16) u8 line[PS_RES_LINE_SIZE];
    ps_res_acquire(base, job_chain, line);
    for (u32 k = 0; k < 4; k++) ps_write64(base, info + 8 * k, ps_read64(line, jc_off::urgentCmds + 8 * k));
    ps_write32(base, info + 0x20, static_cast<u32>(ps_read64(line, jc_off::pc)));
    for (u32 k = 0; k < 3; k++) ps_write32(base, info + 0x24 + 4 * k, static_cast<u32>(ps_read64(line, jc_off::linkRegister + 8 * k)));
    ps_write32(base, info + 0x30, static_cast<u32>(ps_read64(base, job_chain + jc_off::cause)));
    ps_write32(base, info + 0x34, ps_read32(base, job_chain + jc_off::error));
    const u8 flags = ps_read8(line, jc_off::flags);
    ps_write32(base, info + 0x38, ((flags & jc_off::flagMaxSizeMask) << 3) + 0x100u);
    ps_write32(base, info + 0x3C, wid);
    ps_write8(base, info + 0x40, ps_read8(line, jc_off::autoReadyCount) ? 1 : 0);
    ps_write8(base, info + 0x41, ps_read8(line, jc_off::isHalted) ? 1 : 0);
    ps_write8(base, info + 0x42, (flags & jc_off::flagFixedMem) ? 1 : 0);
    ps_write16(base, info + 0x44, ps_read16(line, jc_off::maxGrabbedJob));
    ps_write32(base, info + 0x48, name);
    ps_write32(base, info + 0x4C, static_cast<u32>(ps_read64(base, job_chain + jc_off::exceptionHandler)));
    ps_write32(base, info + 0x50, static_cast<u32>(ps_read64(base, job_chain + jc_off::exceptionHandlerArg)));
    return lv2::CELL_OK;
}

// cellSpursJobSetMaxGrab(jobChain, maxGrabbedJob), as RPCS3 (libsre agrees): NULL, ALIGN, INVAL for a limit outside 1..16;
// STAT for a chain without a valid SPURS instance (NULL or misaligned), a workload ID past 31 or a workload no longer
// registered; then the limit (u16 at 0x72, where libsre keeps it; RPCS3's layout has it at 0x70) in one update of the
// chain's first line, which the native policy merges back after its visits (spurs_jobchain.cpp).
s32 cellSpursJobSetMaxGrab(PPUContext& ctx, u32 job_chain, u32 max_grabbed)
{
    if (const s32 rc = check_job_chain(ctx, "cellSpursJobSetMaxGrab", job_chain, {}, false); rc != lv2::CELL_OK) return rc;
    if (!max_grabbed || max_grabbed > 16) return error(CELL_SPURS_JOB_ERROR_INVAL);
    u8* base = guest_base(ctx);
    const u64 spurs = ps_read64(base, job_chain + jc_off::spurs);
    if (!spurs || spurs % CELL_SPURS_ALIGN || (spurs >> 32)) return error(CELL_SPURS_JOB_ERROR_STAT);
    const u32 wid = ps_read32(base, job_chain + jc_off::workloadId);
    if (wid >= CELL_SPURS_MAX_WORKLOAD2 || !workload_enabled(base, static_cast<u32>(spurs), wid)) {
        return error(CELL_SPURS_JOB_ERROR_STAT);
    }
    reservation_op(base, job_chain, [max_grabbed](u8* line) {
        ps_write16(line, jc_off::maxGrabbedJob, static_cast<u16>(max_grabbed));
        return true;
    });
    return lv2::CELL_OK;
}

// CellSpursJobGuard (128 bytes, 128-aligned) as libsre lays it out: ncount0 (u32, counting down), ncount1 (the reset
// value), the job chain (u64 at 0x08; RPCS3 has a u32 there), requestSpuCount (u32 at 0x10), autoReset (u32 at 0x20) and
// at 0x30 the word the chain's GUARD command sets while it waits on the guard (spurs_jobchain.cpp).
namespace guard_off {
constexpr u32 ncount0 = 0x00, ncount1 = 0x04, job_chain = 0x08, request_spu_count = 0x10, auto_reset = 0x20, waiting = 0x30;
}

// cellSpursJobGuardInitialize(jobChain, jobGuard, notifyCount, requestSpuCount, autoReset), as libsre: NULL (chain,
// guard), the guard 128-byte ALIGN (RPCS3 also checks the chain's alignment), INVAL for a chain whose workload ID is past
// 31; the guard cleared, both counts notifyCount, the chain, the requested SPU count and autoReset (their low bytes).
s32 cellSpursJobGuardInitialize(PPUContext& ctx, u32 job_chain, u32 guard, u32 notify_count, u32 request_spu_count,
    u32 auto_reset)
{
    constexpr std::string_view fn = "cellSpursJobGuardInitialize";
    if (!job_chain || !guard) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (guard % 128) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, fn, "job chain", job_chain, 0x80, CELL_SPURS_JOB_ERROR_INVAL); rc != lv2::CELL_OK) {
        return rc;
    }
    if (const s32 rc = check_pointer(ctx, fn, "job guard", guard, 0x80, CELL_SPURS_JOB_ERROR_INVAL); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, job_chain + jc_off::workloadId) >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_JOB_ERROR_INVAL);
    reservation_op(base, guard, [&](u8* line) {
        std::memset(line, 0, PS_RES_LINE_SIZE);
        ps_write32(line, guard_off::ncount0, notify_count);
        ps_write32(line, guard_off::ncount1, notify_count);
        ps_write64(line, guard_off::job_chain, job_chain);
        ps_write32(line, guard_off::request_spu_count, request_spu_count & 0xFF);
        ps_write32(line, guard_off::auto_reset, auto_reset & 0xFF);
        return true;
    });
    return lv2::CELL_OK;
}

// cellSpursJobGuardNotify(jobGuard), as libsre (RPCS3 agrees but reads the chain as a u32 at 0x08, the u64's high word):
// NULL, ALIGN; in one update of the guard, JOB_ERROR_STAT when the count is 0 already, else it counts down, noting
// whether the chain waits on the guard. At 0 the chain goes on: a revision 0 or 1 chain is kicked with the requested SPU
// count (cellSpursKickJobChain), a later one, if it waits, is run (cellSpursRunJobChain). CELL_OK.
s32 cellSpursJobGuardNotify(PPUContext& ctx, u32 guard)
{
    if (!guard) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (guard % 128) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursJobGuardNotify", "job guard", guard, 0x80, CELL_SPURS_JOB_ERROR_INVAL);
        rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    u32 left = 0;
    bool waiting = false, counted = false;
    reservation_op(base, guard, [&](u8* line) {
        const u32 count = ps_read32(line, guard_off::ncount0);
        waiting = ps_read32(line, guard_off::waiting) != 0;
        counted = count != 0;
        if (!counted) return false;
        left = count - 1;
        ps_write32(line, guard_off::ncount0, left);
        return true;
    });
    if (!counted) return error(CELL_SPURS_JOB_ERROR_STAT);
    if (left) return lv2::CELL_OK;
    const u32 job_chain = static_cast<u32>(ps_read64(base, guard + guard_off::job_chain));
    if (!guest_readable(thread_of(ctx), job_chain, 0x80)) {
        log_call_site(ctx, base, std::format("cellSpursJobGuardNotify({:#x}): the guard's job chain {:#x} is not guest "
            "memory", guard, job_chain));
        return lv2::CELL_OK;
    }
    if (ps_read8(base, job_chain + jc_off::jmVer) <= CELL_SPURS_JOB_REVISION_1) {
        cellSpursKickJobChain(ctx, job_chain, ps_read32(base, guard + guard_off::request_spu_count) & 0xFF);
    } else if (waiting) {
        cellSpursRunJobChain(ctx, job_chain);
    }
    return lv2::CELL_OK;
}

// cellSpursJobGuardReset(jobGuard), as RPCS3 (libsre agrees): NULL, ALIGN; the count back to its reset value.
s32 cellSpursJobGuardReset(PPUContext& ctx, u32 guard)
{
    if (!guard) return error(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (guard % 128) return error(CELL_SPURS_JOB_ERROR_ALIGN);
    if (const s32 rc = check_pointer(ctx, "cellSpursJobGuardReset", "job guard", guard, 0x80, CELL_SPURS_JOB_ERROR_INVAL);
        rc != lv2::CELL_OK) {
        return rc;
    }
    reservation_op(guest_base(ctx), guard, [](u8* line) {
        ps_write32(line, guard_off::ncount0, ps_read32(line, guard_off::ncount1));
        return true;
    });
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
        PS_HLE_FUNC(0x52CC6C82, cellSpursCreateTaskset),
        PS_HLE_FUNC(0x8F122EF8, cellSpursTasksetAttributeSetTasksetSize),
        PS_HLE_FUNC(0xA789E631, cellSpursShutdownTaskset),
        PS_HLE_FUNC(0x9F72ADD3, cellSpursJoinTaskset),
        PS_HLE_FUNC(0x738E40E6, cellSpursShutdownJobChain),
        PS_HLE_FUNC(0xA7C066DE, cellSpursJoinJobChain),
        PS_HLE_FUNC(0x69726AA2, cellSpursAddWorkload),
        PS_HLE_FUNC(0xEFEB2679, _cellSpursWorkloadAttributeInitialize),
        PS_HLE_FUNC(0x4A5EAB63, cellSpursWorkloadAttributeSetName),
        PS_HLE_FUNC(0xC0158D8B, cellSpursAddWorkloadWithAttribute),
        PS_HLE_FUNC(0x1051D134, cellSpursAttributeEnableSpuPrintfIfAvailable),
        PS_HLE_FUNC(0x9DCBCB5D, cellSpursAttributeEnableSystemWorkload),
        PS_HLE_FUNC(0x7517724A, cellSpursSetGlobalExceptionEventHandler),
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
        PS_HLE_FUNC(0xE0A6DBE4, _cellSpursSendSignal),
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
        PS_HLE_FUNC(0x17001000, cellSpursAddUrgentCommand),
        // D-171: the rest of RPCS3's cellSpurs API, on the native scheduler.
        PS_HLE_FUNC(0xACFC8DBC, cellSpursInitialize),
        PS_HLE_FUNC(0x30AA96C4, cellSpursInitializeWithAttribute2),
        PS_HLE_FUNC(0x82275C1C, cellSpursAttributeSetMemoryContainerForSpuThread),
        PS_HLE_FUNC(0x1F402F8F, cellSpursGetInfo),
        PS_HLE_FUNC(0xC56DEFB5, cellSpursGetNumSpuThread),
        PS_HLE_FUNC(0x39C173FB, cellSpursGetSpuThreadGroupId),
        PS_HLE_FUNC(0x6C960F6D, cellSpursGetSpuThreadId),
        PS_HLE_FUNC(0x6FCDF6E3, cellSpursGetSpuGuid),
        PS_HLE_FUNC(0x84D2F6D5, cellSpursSetMaxContention),
        PS_HLE_FUNC(0x80A29E27, cellSpursSetPriorities),
        PS_HLE_FUNC(0xB52E1BDA, cellSpursSetPriority),
        PS_HLE_FUNC(0x182D9890, cellSpursRequestIdleSpu),
        PS_HLE_FUNC(0x98D5B343, cellSpursShutdownWorkload),
        PS_HLE_FUNC(0x5FD43FE4, cellSpursWaitForWorkloadShutdown),
        PS_HLE_FUNC(0x57E4DEC3, cellSpursRemoveWorkload),
        PS_HLE_FUNC(0x1D2BCA4B, cellSpursSendWorkloadSignal),
        PS_HLE_FUNC(0xA7F9E716, cellSpursGetWorkloadData),
        PS_HLE_FUNC(0x0EB4BC38, cellSpursWorkloadAttributeSetShutdownCompletionEventHook),
        PS_HLE_FUNC(0x75211196, cellSpursReadyCountAdd),
        PS_HLE_FUNC(0xF1D3552D, cellSpursReadyCountCompareAndSwap),
        PS_HLE_FUNC(0x49A3426D, cellSpursReadyCountSwap),
        PS_HLE_FUNC(0x2DDBCC0A, _cellSpursWorkloadFlagReceiver2),
        PS_HLE_FUNC(0x8FDF50B2, cellSpursAddUrgentCall),
        PS_HLE_FUNC(0x32B94ADD, cellSpursEnableExceptionEventHandler),
        PS_HLE_FUNC(0x861237F8, cellSpursUnsetGlobalExceptionEventHandler),
        PS_HLE_FUNC(0x569674E3, cellSpursTraceInitialize),
        PS_HLE_FUNC(0xCE853FBF, cellSpursTraceStart),
        PS_HLE_FUNC(0xDDC81B5A, cellSpursTraceStop),
        PS_HLE_FUNC(0x7B9CBB74, cellSpursTraceFinalize),
        PS_HLE_FUNC(0x4A6465E3, cellSpursCreateTaskset2),
        PS_HLE_FUNC(0xC2ACDF43, _cellSpursTasksetAttribute2Initialize),
        PS_HLE_FUNC(0x8ADADF65, _cellSpursTaskAttribute2Initialize),
        PS_HLE_FUNC(0xDCA13593, cellSpursTasksetAttributeEnableClearLS),
        PS_HLE_FUNC(0x2EDCFF92, cellSpursTasksetSetExceptionEventHandler),
        PS_HLE_FUNC(0x94034C95, cellSpursTasksetUnsetExceptionEventHandler),
        PS_HLE_FUNC(0x58D58FCF, cellSpursTasksetGetSpursAddress),
        PS_HLE_FUNC(0x4CCE88A9, cellSpursLookUpTasksetAddress),
        PS_HLE_FUNC(0xE7DD87E1, cellSpursGetTasksetId),
        PS_HLE_FUNC(0x4AC7BAE4, cellSpursEventFlagClear),
        PS_HLE_FUNC(0x6D2D9339, cellSpursEventFlagTryWait),
        PS_HLE_FUNC(0x890F9E5A, cellSpursEventFlagGetDirection),
        PS_HLE_FUNC(0x4D1E9373, cellSpursEventFlagGetClearMode),
        PS_HLE_FUNC(0x947EFB0B, cellSpursEventFlagGetTasksetAddress),
        PS_HLE_FUNC(0x9AEB5432, cellSpursBarrierGetTasksetAddress),
        PS_HLE_FUNC(0x011EE38B, _cellSpursLFQueueInitialize),
        PS_HLE_FUNC(0xB792CA1A, cellSpursLFQueueGetTasksetAddress),
        PS_HLE_FUNC(0x4E153E3E, cellSpursGetWorkloadInfo),
        PS_HLE_FUNC(0x9FCB567B, cellSpursGetTasksetInfo),
        PS_HLE_FUNC(0xD2E23FA9, cellSpursSetExceptionEventHandler),
        PS_HLE_FUNC(0x4C75DEB8, cellSpursUnsetExceptionEventHandler),
        PS_HLE_FUNC(0x4DE203E2, cellSpursSetPreemptionVictimHints),
        PS_HLE_FUNC(0xE279681F, cellSpurs_E279681F),
        PS_HLE_FUNC(0xA121A224, cellSpursTaskAttributeSetExitCodeContainer),
        PS_HLE_FUNC(0x34552FA6, cellSpursTaskExitCodeInitialize),
        PS_HLE_FUNC(0x97A2F6C8, cellSpursJobHeaderSetJobbin2Param),
        PS_HLE_FUNC(0x90C82BFC, cellSpursInitializeForSpuSharing),
        PS_HLE_FUNC(0x5C3A614C, cellSpurs_5C3A614C),
        PS_HLE_FUNC(0xB55782F9, cellSpurs_B55782F9),
        PS_HLE_FUNC(0xBAC24478, cellSpurs_BAC24478),
        PS_HLE_FUNC(0x7025A5EC, cellSpurs_7025A5EC),
        PS_HLE_FUNC(0x60EB2DEC, cellSpursCreateJobChain),
        PS_HLE_FUNC(0xBFEA60FA, cellSpursKickJobChain),
        PS_HLE_FUNC(0x86C864A2, cellSpursGetJobChainId),
        PS_HLE_FUNC(0x4CEB9694, cellSpursGetJobChainInfo),
        PS_HLE_FUNC(0x161DA6A7, cellSpursJobChainGetError),
        PS_HLE_FUNC(0x494613C7, cellSpursJobChainGetSpursAddress),
        PS_HLE_FUNC(0x2CFCCB99, cellSpursJobChainAttributeSetJobTypeMemoryCheck),
        PS_HLE_FUNC(0x5202E53B, cellSpursJobChainSetExceptionEventHandler),
        PS_HLE_FUNC(0x6AA76999, cellSpursJobChainUnsetExceptionEventHandler),
        PS_HLE_FUNC(0x68AAEBA9, cellSpursJobGuardInitialize),
        PS_HLE_FUNC(0xD5D0B256, cellSpursJobGuardNotify),
        PS_HLE_FUNC(0x00AF2519, cellSpursJobGuardReset),
        PS_HLE_FUNC(0xE7B0E69A, cellSpursJobSetMaxGrab),
        PS_HLE_INTERNAL(NID_event_helper, _pspocket_spurs_event_helper),
    });
}

} // namespace pspocket::hle
