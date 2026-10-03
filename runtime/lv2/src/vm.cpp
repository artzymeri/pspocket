// sys_vm: the PS3's virtual memory areas (a large virtual range backed by a smaller amount of physical memory that lv2
// pages in and out). Reference: RPCS3 rpcs3/Emu/Cell/lv2/sys_vm.h/.cpp.
//
// Model (as RPCS3): sys_vm_memory_map takes a free 256 MB-aligned area, makes the whole virtual size readable and
// writable at once, and charges only the physical size to the memory container. Nothing is ever paged out, so lock,
// unlock, touch, flush, invalidate, store and sync only validate their range, sys_vm_test reports every page as on
// memory, and the statistics report no faults or paging. A process holds at most 256 MB of virtual memory in total.

#include "internal.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/ppu/runtime.h"

namespace pspocket::lv2 {

namespace {

constexpr u64 SYS_VM_STATE_ON_MEMORY = 2;
constexpr u64 SYS_VM_POLICY_AUTO_RECOMMENDED = 1;
constexpr u32 kAreaSize = 0x10000000;
constexpr u32 kMaxTotalVsize = 0x10000000; // per process (RPCS3: 0x1E000000 for root processes)

// The block containing addr, or null (RPCS3 sys_vm_t::find_id: one block per 256 MB slot).
std::shared_ptr<VmBlock> find_block(Kernel::Impl& s, u32 addr)
{
    for (const auto& [id, obj] : s.vms.list(VmBlock::kIdBase)) {
        auto b = std::static_pointer_cast<VmBlock>(obj);
        if ((b->addr >> 28) == (addr >> 28)) return b;
    }
    return nullptr;
}

// The checks every range operation shares: a non-empty range inside a mapped block.
Result check_range(PpuThread& t, u32 addr, u32 size, bool allow_empty = false)
{
    if (!size && !allow_empty) return err(CELL_EINVAL);
    const auto b = find_block(t.kernel.impl(), addr);
    if (!b || u64{addr} + size > u64{b->addr} + b->size) return err(CELL_EINVAL);
    return CELL_OK;
}

} // namespace

Result sys_vm_memory_map(PpuThread& t, u64 vsize, u64 psize, u32 cid, u64 flag, u64 policy, u32 addr_out)
{
    if (!vsize || !psize || vsize % 0x2000000 || vsize > kAreaSize || psize % 0x10000 ||
        policy != SYS_VM_POLICY_AUTO_RECOMMENDED) {
        return err(CELL_EINVAL);
    }
    if (psize < 0x100000) return err(CELL_EINVAL); // syscall 300 only (313 allows less; RPCS3)
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(addr_out, 4)) return err(CELL_EFAULT);

    std::shared_ptr<MemoryContainer> ct;
    if (cid != SYS_MEMORY_CONTAINER_ID_INVALID) {
        ct = s.containers.get<MemoryContainer>(cid);
        if (!ct) return err(CELL_ESRCH);
    }

    std::lock_guard lock(s.memory.mutex);
    if (kMaxTotalVsize - s.memory.vm_total < vsize) return err(CELL_EBUSY);
    if (!s.memory.container_or_default(ct).take(psize)) return err(CELL_ENOMEM);

    const u32 page = (flag & SYS_MEMORY_PAGE_SIZE_64K) ? 0x10000 : 0x100000;
    Area* area = s.memory.reserve_area(Area::Kind::Vm, kAreaSize, kAreaSize, page);
    if (!area || !t.kernel.memory().commit(area->addr, vsize)) {
        if (area) std::erase_if(s.memory.areas, [&](const auto& a) { return a.get() == area; });
        s.memory.container_or_default(ct).give_back(psize);
        return err(CELL_ENOMEM);
    }
    auto block = std::make_shared<VmBlock>(area->addr, static_cast<u32>(vsize), static_cast<u32>(psize), ct);
    const u32 id = s.vms.insert(VmBlock::kIdBase, block);
    if (!id) {
        t.kernel.memory().decommit(area->addr, vsize);
        std::erase_if(s.memory.areas, [&](const auto& a) { return a.get() == area; });
        s.memory.container_or_default(ct).give_back(psize);
        return err(CELL_EAGAIN);
    }
    block->id = id;
    s.memory.vm_total += static_cast<u32>(vsize);
    s.guest.w32(addr_out, area->addr);
    PS_LOG_INFO("lv2", "sys_vm_memory_map: {:#010x}, {} MB virtual, {:.1f} MB physical", area->addr, vsize >> 20,
        static_cast<double>(psize) / (1 << 20));
    return CELL_OK;
}

Result sys_vm_memory_map_different(PpuThread& t, u64 vsize, u64 psize, u32 cid, u64 flag, u64 policy, u32 addr_out)
{
    // RPCS3: "if needed implement different way to map memory, unconfirmed"; same as sys_vm_memory_map without
    // the 1 MB minimum physical size.
    if (psize && psize < 0x100000 && psize % 0x10000 == 0) {
        PS_LOG_WARN("lv2", "sys_vm_memory_map_different: physical size {:#x} below 1 MB, mapping with 1 MB", psize);
        psize = 0x100000;
    }
    return sys_vm_memory_map(t, vsize, psize, cid, flag, policy, addr_out);
}

Result sys_vm_unmap(PpuThread& t, u32 addr)
{
    if (addr % kAreaSize) return err(CELL_EINVAL);
    Kernel::Impl& s = t.kernel.impl();
    const auto b = find_block(s, addr);
    if (!b || b->addr != addr || !s.vms.remove(VmBlock::kIdBase, b->id)) return err(CELL_EINVAL);
    std::lock_guard lock(s.memory.mutex);
    t.kernel.memory().decommit(b->addr, b->size);
    std::erase_if(s.memory.areas, [&](const auto& a) { return a->addr == b->addr && a->kind == Area::Kind::Vm; });
    s.memory.container_or_default(b->container).give_back(b->psize);
    s.memory.vm_total -= b->size;
    return CELL_OK;
}

Result sys_vm_append_memory(PpuThread& t, u32 addr, u64 size)
{
    if (!size || size % 0x100000) return err(CELL_EINVAL);
    Kernel::Impl& s = t.kernel.impl();
    const auto b = find_block(s, addr);
    if (!b || b->addr != addr) return err(CELL_EINVAL);
    std::lock_guard lock(s.memory.mutex);
    if (!s.memory.container_or_default(b->container).take(size)) return err(CELL_ENOMEM);
    b->psize += static_cast<u32>(size);
    return CELL_OK;
}

Result sys_vm_return_memory(PpuThread& t, u32 addr, u64 size)
{
    if (!size || size % 0x100000) return err(CELL_EINVAL);
    Kernel::Impl& s = t.kernel.impl();
    const auto b = find_block(s, addr);
    if (!b || b->addr != addr) return err(CELL_EINVAL);
    std::lock_guard lock(s.memory.mutex);
    if (b->psize <= size || b->psize - size < 0x100000) return err(CELL_EBUSY); // keep at least 1 MB
    b->psize -= static_cast<u32>(size);
    s.memory.container_or_default(b->container).give_back(size);
    return CELL_OK;
}

Result sys_vm_lock(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }
Result sys_vm_unlock(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }
Result sys_vm_touch(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }
Result sys_vm_flush(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }
Result sys_vm_invalidate(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }
Result sys_vm_store(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }
Result sys_vm_sync(PpuThread& t, u32 addr, u32 size) { return check_range(t, addr, size); }

Result sys_vm_test(PpuThread& t, u32 addr, u32 size, u32 result)
{
    if (const Result r = check_range(t, addr, size, true); r != CELL_OK) return r;
    if (!t.kernel.impl().guest.valid(result, 8)) return err(CELL_EFAULT);
    t.kernel.impl().guest.w64(result, SYS_VM_STATE_ON_MEMORY);
    return CELL_OK;
}

Result sys_vm_get_statistics(PpuThread& t, u32 addr, u32 stat)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto b = find_block(s, addr);
    if (!b || b->addr != addr) return err(CELL_EINVAL);
    if (!s.guest.valid(stat, 0x30)) return err(CELL_EFAULT);
    // sys_vm_statistics_t: page_fault_ppu, page_fault_spu, page_in, page_out (u64), pmem_total, pmem_used (u32),
    // timestamp (u64).
    for (u32 i = 0; i < 4; i++) s.guest.w64(stat + 8 * i, 0);
    s.guest.w32(stat + 0x20, b->psize);
    s.guest.w32(stat + 0x24, 0);
    s.guest.w64(stat + 0x28, ps_ppu_timebase());
    return CELL_OK;
}

void register_vm_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 300, sys_vm_memory_map);
    PS_LV2_BIND(table, 301, sys_vm_unmap);
    PS_LV2_BIND(table, 302, sys_vm_append_memory);
    PS_LV2_BIND(table, 303, sys_vm_return_memory);
    PS_LV2_BIND(table, 304, sys_vm_lock);
    PS_LV2_BIND(table, 305, sys_vm_unlock);
    PS_LV2_BIND(table, 306, sys_vm_touch);
    PS_LV2_BIND(table, 307, sys_vm_flush);
    PS_LV2_BIND(table, 308, sys_vm_invalidate);
    PS_LV2_BIND(table, 309, sys_vm_store);
    PS_LV2_BIND(table, 310, sys_vm_sync);
    PS_LV2_BIND(table, 311, sys_vm_test);
    PS_LV2_BIND(table, 312, sys_vm_get_statistics);
    PS_LV2_BIND(table, 313, sys_vm_memory_map_different);
}

} // namespace pspocket::lv2
