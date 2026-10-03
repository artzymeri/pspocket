// cellSpurs, native (D-037): the SPURS API on the host. Guest-visible structures keep RPCS3's layouts
// (rpcs3/Emu/Cell/Modules/cellSpurs.h); API semantics follow RPCS3 cellSpurs.cpp where it implements them.
//
// Stage 1 (this file so far): attributes, initialize and finalize. Initialize fills the CellSpurs fields RPCS3's
// _spurs::initialize sets that games and SPU code can observe, and registers a host instance. What a native SPURS
// does not create, because nothing of Sony's SPURS kernel runs: the SPU thread group and its threads (spuTG, spus[]),
// the PPU handler and event helper threads (ppu0, ppu1), the per-workload lv2 semaphores (wklF1[].sem, semPrv), the
// guest lwmutex/lwcond at 0xDB0/0xDC8 and the kernel's event queue and port. Those fields stay 0 (ppu0/ppu1 keep
// RPCS3's 0xFFFFFFFF "no thread"). Workloads, tasksets, tasks, job chains and SPU execution come next.

#include <cstring>
#include <map>
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
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_SPURS_MAX_SPU = 8;
constexpr u32 CELL_SPURS_NAME_MAX_LENGTH = 15;
constexpr u32 CELL_SPURS_SIZE = 4096;
constexpr u32 CELL_SPURS_SIZE2 = 8192;
constexpr u32 CELL_SPURS_ALIGN = 128;
constexpr u32 CELL_SPURS_KERNEL1_ENTRY_ADDR = 0x818;
constexpr u32 CELL_SPURS_KERNEL2_ENTRY_ADDR = 0x848;
constexpr u32 SPURS_IMG_ADDR_SYS_SRV_WORKLOAD = 0x100;

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
// sys_spu_image {type, entry_point, segs, nsegs} and CellSpurs::WorkloadInfo {addr (u64), arg, size, uniqueId, ...}.
constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0;

// Host side of every initialized SPURS instance, keyed by the guest CellSpurs address.
struct Instance {
    u32 nspus = 0;
    bool second = false;
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
        h.instances[spurs] = Instance{static_cast<u32>(nspus), second};
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

} // namespace

void register_cellSpurs(Registry& r)
{
    r.add("cellSpurs", {
        PS_HLE_FUNC(0x95180230, _cellSpursAttributeInitialize),
        PS_HLE_FUNC(0x07529113, cellSpursAttributeSetNamePrefix),
        PS_HLE_FUNC(0xA839A4D9, cellSpursAttributeSetSpuThreadGroupType),
        PS_HLE_FUNC(0xAA6269A8, cellSpursInitializeWithAttribute),
        PS_HLE_FUNC(0xCA4C4600, cellSpursFinalize),
    });
}

} // namespace pspocket::hle
