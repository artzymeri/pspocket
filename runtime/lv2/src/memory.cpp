// sys_memory, sys_mmapper, and the guest address-space bookkeeping behind them (user memory areas, PPU stacks).
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_memory.h/.cpp, sys_mmapper.h/.cpp, rpcs3/Emu/Memory/vm.cpp (_find_map,
// block_t::alloc/falloc), Emu/Cell/PPUModule.cpp (main area), rpcs3/util/vm_native.cpp (iOS shm as Mach memory
// entries).
//
// Address map (pspocket/core/memory_map.h): user memory and sys_mmapper address ranges take 256 MB areas in
// [0x10000000, 0xC0000000) at the first free aligned slot (vm.cpp _find_map), skipping the main executable's area
// (Config::main_area_end) and the PRX area (0x30000000); sys_mmapper areas start at 0x20000000, the lowest address
// sys_mmapper_free_address accepts. PPU stacks live in 0xD0000000..0xE0000000. Guest memory
// is committed at host-page granularity (16 KB, D-010); every user allocation is a multiple of 64 KB.
//
// Deviations:
// - Each PPU stack gets a 16 KB uncommitted guard below it (stack_granule), not RPCS3's 4 KB guards.
// - sys_mmapper_change_address_access_right, page-fault notification and the _ext allocators are not implemented.
// - sys_memory_get_page_attribute reports SYS_MEMORY_ACCESS_RIGHT_ANY for everything but stacks (as RPCS3).

#include <algorithm>

#include "internal.hpp"
#include "pspocket/core/memory_map.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_map.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace pspocket::lv2 {

namespace {

constexpr u64 k256M = 0x10000000;
constexpr u64 SYS_MEMORY_PAGE_SIZE_1M = 0x400;
constexpr u64 SYS_MEMORY_GRANULARITY_MASK = 0xF00;
constexpr u64 SYS_MMAPPER_NO_SHM_KEY = 0xFFFF000000000000ull;
constexpr u64 SYS_MEMORY_ACCESS_RIGHT_ANY = 0xF;
constexpr u64 SYS_MEMORY_ACCESS_RIGHT_PPU_THR = 0x8;
constexpr u64 SYS_MEMORY_PROT_READ_WRITE = 0x40000;

u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

u64 host_page() { return GuestMemory::host_page_size(); }

// Guest stack layout unit: stack sizes and the guard below each stack are multiples of 16 KB on every host (the
// Apple page size, D-010), so guest-visible stack addresses don't depend on the host. Hosts with larger pages
// round up to their page.
u64 stack_granule() { return std::max<u64>(host_page(), 0x4000); }

// ---- host shared memory objects

bool shm_create(u64 size, u64& handle)
{
#if defined(__APPLE__)
    // The iOS sandbox refuses shm_open; an anonymous named memory entry works on iOS and macOS alike.
    memory_object_size_t entry_size = size;
    mach_port_t entry = MACH_PORT_NULL;
    const kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
        MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS || entry_size < size) {
        PS_LOG_ERROR(kChan, "mach_make_memory_entry_64({:#x}) failed: {}", size, kr);
        return false;
    }
    handle = entry;
    return true;
#elif defined(__linux__)
    const int fd = memfd_create("pspocket-shm", 0);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) return false;
    handle = static_cast<u64>(fd);
    return true;
#else
    (void)size;
    (void)handle;
    return false;
#endif
}

void shm_release(u64 handle)
{
#if defined(__APPLE__)
    mach_port_deallocate(mach_task_self(), static_cast<mach_port_t>(handle));
#elif defined(__linux__)
    close(static_cast<int>(handle));
#else
    (void)handle;
#endif
}

bool shm_map(u64 handle, u8* host, u64 size)
{
#if defined(__APPLE__)
    vm_address_t addr = reinterpret_cast<vm_address_t>(host);
    const kern_return_t kr = vm_map(mach_task_self(), &addr, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
        static_cast<mach_port_t>(handle), 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
        VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) PS_LOG_ERROR(kChan, "vm_map of shared memory at {} failed: {}", static_cast<void*>(host), kr);
    return kr == KERN_SUCCESS;
#elif defined(__linux__)
    return mmap(host, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, static_cast<int>(handle), 0) != MAP_FAILED;
#else
    (void)handle;
    (void)host;
    (void)size;
    return false;
#endif
}

} // namespace

// ------------------------------------------------------------------------------------------------- RangeSet

u32 RangeSet::alloc(u64 size, u64 align)
{
    if (size == 0) return 0;
    u64 cursor = align_up(lo_, align);
    for (const auto& [start, len] : used_) {
        if (cursor + size <= start) break;
        cursor = std::max(cursor, align_up(start + len, align));
    }
    if (cursor + size > hi_) return 0;
    used_[cursor] = size;
    return static_cast<u32>(cursor);
}

bool RangeSet::alloc_at(u32 addr, u64 size)
{
    if (size == 0 || addr < lo_ || u64{addr} + size > hi_) return false;
    auto it = used_.lower_bound(addr);
    if (it != used_.end() && it->first < u64{addr} + size) return false;
    if (it != used_.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second > addr) return false;
    }
    used_[addr] = size;
    return true;
}

u64 RangeSet::free(u32 addr)
{
    const auto it = used_.find(addr);
    if (it == used_.end()) return 0;
    const u64 size = it->second;
    used_.erase(it);
    return size;
}

std::pair<u32, u64> RangeSet::find(u32 addr) const
{
    auto it = used_.upper_bound(addr);
    if (it == used_.begin()) return {0, 0};
    --it;
    if (addr < it->first + it->second) return {static_cast<u32>(it->first), it->second};
    return {0, 0};
}

// ------------------------------------------------------------------------------------------------- containers

bool MemoryContainer::take(u64 amount)
{
    if (amount > size - used) return false;
    used += static_cast<u32>(amount);
    return true;
}

void MemoryContainer::give_back(u64 amount)
{
    PS_CHECK(amount <= used);
    used -= static_cast<u32>(amount);
}

SharedMemory::~SharedMemory()
{
    if (handle) shm_release(handle);
}

// ------------------------------------------------------------------------------------------------- MemoryManager

MemoryManager::MemoryManager(Kernel& k, u32 default_size, u32 main_end)
    : default_container(default_size)
    , stacks(PS_MEM_STACK_BASE, u64{PS_MEM_STACK_BASE} + PS_MEM_STACK_SIZE)
    , kernel(k)
    , main_area_end(main_end)
{
}

MemoryManager::~MemoryManager() = default;

Area* MemoryManager::area_at(u32 addr)
{
    for (auto& a : areas) {
        if (addr >= a->addr && addr - u64{a->addr} < a->size) return a.get();
    }
    return nullptr;
}

bool MemoryManager::slot_free(u64 addr, u64 size) const
{
    const u64 end = addr + size;
    if (addr < main_area_end) return false;                                          // main executable (vm::main)
    if (addr < u64{PS_MEM_PRX_BASE} + PS_MEM_PRX_SIZE && end > PS_MEM_PRX_BASE) return false; // PRX modules
    if (end > PS_MEM_RSX_LOCAL_BASE) return false;                                   // RSX, stacks, SPU
    for (const auto& a : areas) {
        if (addr < a->addr + a->size && end > a->addr) return false;
    }
    return true;
}

Area* MemoryManager::reserve_area(Area::Kind kind, u64 size, u64 align, u32 page_size, u32 fixed_addr)
{
    u64 addr = 0;
    if (fixed_addr) {
        if (!slot_free(fixed_addr, size)) return nullptr;
        addr = fixed_addr;
    } else {
        // vm.cpp _find_map: the first aligned slot from 0x10000000 below 0xC0000000 (0x20000000 for sys_mmapper).
        const u64 from = kind == Area::Kind::Mmapper ? kMmapperMin : k256M;
        for (u64 a = align_up(from, align); a + size <= PS_MEM_RSX_LOCAL_BASE; a += align) {
            if (slot_free(a, size)) {
                addr = a;
                break;
            }
        }
        if (!addr) return nullptr;
    }
    areas.push_back(std::unique_ptr<Area>(
        new Area{kind, static_cast<u32>(addr), size, page_size, RangeSet(addr, addr + size), {}}));
    PS_LOG_DEBUG(kChan, "memory area {:#010x}+{:#x} (kind {}, {} KB pages)", addr, size, static_cast<int>(kind), page_size >> 10);
    return areas.back().get();
}

u32 MemoryManager::allocate_user(u64 size, u32 page_size)
{
    const Area::Kind kind = page_size == 0x10000 ? Area::Kind::User64k : Area::Kind::User1m;
    for (auto& a : areas) {
        if (a->kind != kind) continue;
        if (const u32 addr = a->ranges.alloc(size, page_size)) return addr;
    }
    // A new area (RPCS3 also falls back to a secondary area when the first is full or fragmented).
    Area* area = reserve_area(kind, align_up(size, k256M), k256M, page_size);
    return area ? area->ranges.alloc(size, page_size) : 0;
}

u32 MemoryManager::allocate_kernel(u64 size)
{
    const u64 bytes = align_up(size, 0x10000);
    const u32 addr = allocate_user(bytes, 0x10000);
    if (!addr) return 0;
    if (!kernel.memory().commit(addr, bytes)) {
        if (Area* a = area_at(addr)) a->ranges.free(addr);
        return 0;
    }
    kernel_allocs[addr] = bytes;
    return addr;
}

void MemoryManager::free_kernel(u32 addr)
{
    const auto it = kernel_allocs.find(addr);
    if (it == kernel_allocs.end()) return;
    kernel.memory().decommit(addr, it->second);
    if (Area* a = area_at(addr)) a->ranges.free(addr);
    kernel_allocs.erase(it);
}

u32 MemoryManager::allocate_stack(u32 size, u32& committed)
{
    const u64 page = stack_granule();
    const u64 commit = align_up(size, page);
    const u32 start = stacks.alloc(commit + page, page); // guard page below the stack
    if (!start) return 0;
    const u32 addr = static_cast<u32>(start + page);
    if (!kernel.memory().commit(addr, commit)) {
        stacks.free(start);
        return 0;
    }
    committed = static_cast<u32>(commit);
    return addr;
}

void MemoryManager::free_stack(u32 addr, u32 committed)
{
    kernel.memory().decommit(addr, committed);
    stacks.free(static_cast<u32>(addr - stack_granule()));
}

bool MemoryManager::map_shared(SharedMemory& mem, u32 addr)
{
    if (!mem.handle && !shm_create(mem.size, mem.handle)) return false;
    if (!shm_map(mem.handle, kernel.base() + addr, mem.size)) return false;
    // Record the pages as committed (they are already readable and writable).
    return kernel.memory().commit(addr, mem.size);
}

void MemoryManager::unmap_shared(SharedMemory& mem, u32 addr) { kernel.memory().decommit(addr, mem.size); }

// ------------------------------------------------------------------------------------------------- sys_memory

namespace {

Result allocate(PpuThread& t, u64 size, u64 flags, u32 alloc_addr, const std::shared_ptr<MemoryContainer>& ct)
{
    Kernel::Impl& s = t.kernel.impl();
    const u32 align = flags == SYS_MEMORY_PAGE_SIZE_1M ? 0x100000 : flags == SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : flags == 0 ? 0x100000 : 0;
    if (!align) return err(CELL_EINVAL);
    if (size % align) return err(CELL_EALIGN);

    u32 addr = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        MemoryContainer& c = s.memory.container_or_default(ct);
        if (!c.take(size)) return err(CELL_ENOMEM);
        addr = size <= 0xFFFFFFFFull ? s.memory.allocate_user(size, align) : 0;
        if (!addr) {
            c.give_back(size);
            return err(CELL_ENOMEM);
        }
        if (!s.guest.valid(alloc_addr, 4)) {
            // RPCS3 allocates, then frees again on a bad output pointer.
            s.memory.area_at(addr)->ranges.free(addr);
            c.give_back(size);
            return err(CELL_EFAULT);
        }
        if (!t.kernel.memory().commit(addr, size)) {
            s.memory.area_at(addr)->ranges.free(addr);
            c.give_back(size);
            return err(CELL_ENOMEM);
        }
        s.memory.user_allocs[addr] = {size, ct, align};
    }
    s.guest.w32(alloc_addr, addr);
    PS_LOG_DEBUG(kChan, "sys_memory_allocate: {:#010x}+{:#x}", addr, size);
    return CELL_OK;
}

} // namespace

Result sys_memory_allocate(PpuThread& t, u64 size, u64 flags, u32 alloc_addr)
{
    if (!size) return err(CELL_EALIGN);
    return allocate(t, size, flags, alloc_addr, nullptr);
}

Result sys_memory_allocate_from_container(PpuThread& t, u64 size, u32 cid, u64 flags, u32 alloc_addr)
{
    if (!size) return err(CELL_EALIGN);
    const u32 align = flags == SYS_MEMORY_PAGE_SIZE_1M ? 0x100000 : flags == SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 : flags == 0 ? 0x100000 : 0;
    if (!align) return err(CELL_EINVAL);
    if (size % align) return err(CELL_EALIGN);
    const auto ct = t.kernel.impl().containers.get<MemoryContainer>(cid);
    if (!ct) return err(CELL_ESRCH);
    return allocate(t, size, flags, alloc_addr, ct);
}

Result sys_memory_free(PpuThread& t, u32 start_addr)
{
    Kernel::Impl& s = t.kernel.impl();
    if (start_addr % 0x10000) return err(CELL_EINVAL);
    std::lock_guard lock(s.memory.mutex);
    const auto it = s.memory.user_allocs.find(start_addr);
    if (it == s.memory.user_allocs.end()) return err(CELL_EINVAL);
    t.kernel.memory().decommit(start_addr, it->second.size);
    if (Area* a = s.memory.area_at(start_addr)) a->ranges.free(start_addr);
    s.memory.container_or_default(it->second.container).give_back(it->second.size);
    s.memory.user_allocs.erase(it);
    return CELL_OK;
}

Result sys_memory_get_page_attribute(PpuThread& t, u32 addr, u32 attr)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(addr, 1)) return err(CELL_EINVAL);
    if (!s.guest.valid(attr, 24)) return err(CELL_EFAULT);
    u32 page_size = 0x10000;
    u64 access = SYS_MEMORY_ACCESS_RIGHT_ANY;
    if (addr >= PS_MEM_STACK_BASE && addr < u64{PS_MEM_STACK_BASE} + PS_MEM_STACK_SIZE) {
        page_size = 0x1000; // stacks: 4 KB pages, PPU-thread access (RPCS3 fast path)
        access = SYS_MEMORY_ACCESS_RIGHT_PPU_THR;
    } else {
        std::lock_guard lock(s.memory.mutex);
        if (const Area* a = s.memory.area_at(addr)) page_size = a->page_size;
    }
    // sys_page_attr_t: attribute, access_right, page_size, pad
    s.guest.w64(attr, SYS_MEMORY_PROT_READ_WRITE);
    s.guest.w64(attr + 8, access);
    s.guest.w32(attr + 16, page_size);
    s.guest.w32(attr + 20, 0);
    return CELL_OK;
}

Result sys_memory_get_user_memory_size(PpuThread& t, u32 mem_info)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(mem_info, 8)) return err(CELL_EFAULT);
    u32 total = 0, available = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        total = s.memory.default_container.size;
        available = s.memory.default_container.size - s.memory.default_container.used;
        for (const auto& [id, obj] : s.containers.list(MemoryContainer::kIdBase)) total -= static_cast<MemoryContainer&>(*obj).size;
    }
    // sys_memory_info_t: total_user_memory, available_user_memory
    s.guest.w32(mem_info, total);
    s.guest.w32(mem_info + 4, available);
    return CELL_OK;
}

Result sys_memory_container_create(PpuThread& t, u32 cid, u64 size)
{
    Kernel::Impl& s = t.kernel.impl();
    size &= ~u64{0xFFFFF}; // 1 MB granularity
    if (!size || size > 0xFFFFFFFFull) return err(CELL_ENOMEM);
    if (!s.guest.valid(cid, 4)) return err(CELL_EFAULT);
    std::lock_guard lock(s.memory.mutex);
    if (!s.memory.default_container.take(size)) return err(CELL_ENOMEM);
    auto ct = std::make_shared<MemoryContainer>(static_cast<u32>(size));
    const u32 id = s.containers.insert(MemoryContainer::kIdBase, ct);
    if (!id) {
        s.memory.default_container.give_back(size);
        return err(CELL_EAGAIN);
    }
    ct->id = id;
    s.guest.w32(cid, id);
    return CELL_OK;
}

Result sys_memory_container_destroy(PpuThread& t, u32 cid)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto ct = s.containers.get<MemoryContainer>(cid);
    if (!ct) return err(CELL_ESRCH);
    std::lock_guard lock(s.memory.mutex);
    if (ct->used) return err(CELL_EBUSY);
    if (!s.containers.remove(MemoryContainer::kIdBase, cid)) return err(CELL_ESRCH);
    s.memory.default_container.give_back(ct->size);
    return CELL_OK;
}

// sys_memory_container_destroy_parent_with_childs(cid, must_0, mc_child) (345): child containers belong to other
// processes, and a game has none, so this destroys the parent alone (RPCS3 Emu/Cell/lv2/sys_memory.cpp does the same).
Result sys_memory_container_destroy_parent_with_childs(PpuThread& t, u32 cid, u32 must_0, u32)
{
    if (must_0) return err(CELL_EINVAL);
    return sys_memory_container_destroy(t, cid);
}

Result sys_memory_container_get_size(PpuThread& t, u32 mem_info, u32 cid)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto ct = s.containers.get<MemoryContainer>(cid);
    if (!ct) return err(CELL_ESRCH);
    if (!s.guest.valid(mem_info, 8)) return err(CELL_EFAULT);
    u32 used = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        used = ct->used;
    }
    s.guest.w32(mem_info, ct->size);
    s.guest.w32(mem_info + 4, ct->size - used);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- sys_mmapper

Result sys_mmapper_allocate_address(PpuThread& t, u64 size, u64 flags, u64 alignment, u32 alloc_addr)
{
    Kernel::Impl& s = t.kernel.impl();
    if (size % k256M) return err(CELL_EALIGN);
    if (size > 0xFFFFFFFFull) return err(CELL_ENOMEM);
    if (!alignment) alignment = k256M; // PSL1GHT passes 0 (RPCS3 workaround; real lv2 accepts it)
    switch (alignment) {
    case 0x10000000:
    case 0x20000000:
    case 0x40000000:
    case 0x80000000: break;
    default: return err(CELL_EALIGN);
    }
    if (!s.guest.valid(alloc_addr, 4)) return err(CELL_EFAULT);
    const u32 page_size = (flags & SYS_MEMORY_PAGE_SIZE_64K) ? 0x10000 : 0x100000;
    u32 addr = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        Area* area = s.memory.reserve_area(Area::Kind::Mmapper, size, alignment, page_size);
        if (!area) return err(CELL_ENOMEM);
        addr = area->addr;
    }
    s.guest.w32(alloc_addr, addr);
    return CELL_OK;
}

// sys_mmapper_enable_page_fault_notification(start_addr, event_queue_id): the queue hears of page faults in the
// mmapper area starting at start_addr, through an event port lv2 makes for it (name SYS_MEMORY_PAGE_FAULT_EVENT_KEY).
// RPCS3's checks: the start of an mmapper area (EINVAL), a live queue (ESRCH), once per area (EBUSY). pspocket
// registers the watch but has no page faults to report: guest memory that isn't mapped is not caught as a fault.
Result sys_mmapper_enable_page_fault_notification(PpuThread& t, u32 start_addr, u32 event_queue_id)
{
    constexpr u64 SYS_MEMORY_PAGE_FAULT_EVENT_KEY = 0xFFFE000000000000ull;
    Kernel::Impl& s = t.kernel.impl();
    {
        std::lock_guard lock(s.memory.mutex);
        const Area* area = s.memory.area_at(start_addr);
        if (!area || area->kind != Area::Kind::Mmapper || area->addr != start_addr) return err(CELL_EINVAL);
    }
    auto queue = s.objects.get<EventQueue>(event_queue_id);
    if (!queue) return err(CELL_ESRCH);
    auto port = std::make_shared<EventPort>(SYS_EVENT_PORT_LOCAL, SYS_MEMORY_PAGE_FAULT_EVENT_KEY);
    port->refs = 1;
    const u32 port_id = s.objects.insert(EventPort::kIdBase, port);
    if (!port_id) return err(CELL_EAGAIN);
    port->id = port_id;
    {
        std::lock_guard lk(port->lock);
        port->queue = queue;
    }
    std::lock_guard lock(s.memory.mutex);
    for (const auto& w : s.memory.page_fault_watches) {
        if (w.start_addr != start_addr) continue;
        {
            std::lock_guard lk(port->lock);
            port->queue.reset();
        }
        s.objects.remove(EventPort::kIdBase, port_id);
        return err(CELL_EBUSY);
    }
    s.memory.page_fault_watches.push_back({start_addr, event_queue_id, port_id});
    PS_LOG_WARN(kChan, "sys_mmapper_enable_page_fault_notification({:#010x}, queue {:#x}): registered (port {:#x}); "
                       "pspocket reports no page faults",
        start_addr, event_queue_id, port_id);
    return CELL_OK;
}

Result sys_mmapper_allocate_fixed_address(PpuThread& t)
{
    // RPCS3 maps 0xB0000000 as a plain 1 MB-page block: it is not a sys_mmapper mapping area (no mapping_comp),
    // so map/search/free on it fail as they do there.
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard lock(s.memory.mutex);
    if (s.memory.fixed_area_mapped || !s.memory.slot_free(PS_MEM_MMAPPER_BASE, PS_MEM_MMAPPER_SIZE)) return err(CELL_EEXIST);
    s.memory.fixed_area_mapped = true;
    return CELL_OK;
}

Result sys_mmapper_free_address(PpuThread& t, u32 addr)
{
    Kernel::Impl& s = t.kernel.impl();
    if (addr < 0x20000000 || addr >= PS_MEM_RSX_LOCAL_BASE) return err(CELL_EINVAL);
    std::lock_guard lock(s.memory.mutex);
    for (auto it = s.memory.areas.begin(); it != s.memory.areas.end(); ++it) {
        Area& a = **it;
        if (a.addr != addr || a.kind != Area::Kind::Mmapper) continue;
        if (!a.mapped.empty()) return err(CELL_EBUSY);
        s.memory.areas.erase(it);
        return CELL_OK;
    }
    return err(CELL_EINVAL);
}

namespace {

Result create_shared_memory(PpuThread& t, bool pshared, u64 ipc_key, u64 size, u32 align, u64 flags, s32 mode,
    const std::shared_ptr<MemoryContainer>& ct, u32 mem_id)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(mem_id, 4)) return err(CELL_EFAULT);
    if (size > 0xFFFFFFFFull) return err(CELL_ENOMEM);

    std::unique_lock ipc_lock(s.ipc_mutex, std::defer_lock);
    if (pshared) {
        // RPCS3 create_lv2_shm / lv2_memory::on_id_create: an existing object with this key always has a handle,
        // so another create of it fails (EPERM if the geometry differs, else EEXIST).
        ipc_lock.lock();
        const auto it = s.ipc.find({SharedMemory::kIdBase, ipc_key});
        if (it != s.ipc.end()) {
            if (const auto existing = std::static_pointer_cast<SharedMemory>(it->second.lock())) {
                if (existing->size != size || existing->align != align) return err(CELL_EPERM);
                return err(CELL_EEXIST);
            }
        }
        if (mode == SYS_SYNC_NOT_CREATE) return err(CELL_ESRCH);
    }

    auto mem = std::make_shared<SharedMemory>(static_cast<u32>(size), align, flags, ct);
    {
        std::lock_guard lock(s.memory.mutex);
        if (!s.memory.container_or_default(ct).take(size)) return err(CELL_ENOMEM);
    }
    mem->refs = 1;
    mem->ipc_key = pshared ? ipc_key : 0;
    const u32 id = s.objects.insert(SharedMemory::kIdBase, mem);
    if (!id) {
        std::lock_guard lock(s.memory.mutex);
        s.memory.container_or_default(ct).give_back(size);
        return err(CELL_EAGAIN);
    }
    mem->id = id;
    if (pshared) s.ipc[{SharedMemory::kIdBase, ipc_key}] = mem;
    s.guest.w32(mem_id, id);
    return CELL_OK;
}

Result check_granularity(u64 size, u64 flags)
{
    if (size == 0) return err(CELL_EALIGN);
    switch (flags & SYS_MEMORY_GRANULARITY_MASK) {
    case 0:
    case SYS_MEMORY_PAGE_SIZE_1M: return size % 0x100000 ? err(CELL_EALIGN) : CELL_OK;
    case SYS_MEMORY_PAGE_SIZE_64K: return size % 0x10000 ? err(CELL_EALIGN) : CELL_OK;
    default: return err(CELL_EINVAL);
    }
}

} // namespace

Result sys_mmapper_allocate_shared_memory(PpuThread& t, u64 ipc_key, u64 size, u64 flags, u32 mem_id)
{
    if (const Result r = check_granularity(size, flags); r != CELL_OK) return r;
    const s32 mode = (flags & 0xC000) == 0xC000 ? SYS_SYNC_NEWLY_CREATED : (flags & 0x8000) ? SYS_SYNC_NOT_CARE : SYS_SYNC_NOT_CREATE;
    const u32 align = (flags & SYS_MEMORY_PAGE_SIZE_64K) ? 0x10000 : 0x100000;
    return create_shared_memory(t, ipc_key != SYS_MMAPPER_NO_SHM_KEY, ipc_key, size, align, flags, mode, nullptr, mem_id);
}

Result sys_mmapper_allocate_shared_memory_from_container(PpuThread& t, u64 ipc_key, u64 size, u32 cid, u64 flags,
    u32 mem_id)
{
    if (const Result r = check_granularity(size, flags); r != CELL_OK) return r;
    if (flags & 0xC000) return err(CELL_EINVAL);
    const auto ct = t.kernel.impl().containers.get<MemoryContainer>(cid);
    if (!ct) return err(CELL_ESRCH);
    const u32 align = (flags & SYS_MEMORY_PAGE_SIZE_64K) ? 0x10000 : 0x100000;
    return create_shared_memory(t, ipc_key != SYS_MMAPPER_NO_SHM_KEY, ipc_key, size, align, flags,
        SYS_SYNC_NEWLY_CREATED, ct, mem_id);
}

namespace {

// The _ext forms (328, 339) take up to 16 entries of 24 bytes (RPCS3 mmapper_unk_entry_struct0: four words of unknown
// use, then a 64-bit type). Types 0, 1 and 3 are accepted; 5 only for a debug or root process with 64 KB pages, which
// a game is not; any other type is EPERM. RPCS3: Emu/Cell/lv2/sys_mmapper.cpp.
Result check_ext_entries(PpuThread& t, u32 entries, s32 entry_count)
{
    if (entry_count <= 0 || entry_count > 0x10) return err(CELL_EINVAL);
    const Guest& guest = t.kernel.impl().guest;
    if (!guest.valid(entries, static_cast<u32>(entry_count) * 0x18)) return err(CELL_EFAULT);
    for (s32 i = 0; i < entry_count; i++) {
        switch (guest.r64(entries + static_cast<u32>(i) * 0x18 + 0x10)) {
        case 0:
        case 1:
        case 3: break;
        default: return err(CELL_EPERM); // 5 included: a game process is neither debug nor root
        }
    }
    return CELL_OK;
}

} // namespace

// sys_mmapper_allocate_shared_memory_ext(ipc_key, size, flags, entries, entry_count, mem_id) and
// sys_mmapper_allocate_shared_memory_from_container_ext(ipc_key, size, flags, cid, entries, entry_count, mem_id)
// (339, 328): as the plain forms, with entries (check_ext_entries), flags of the page size only, and always a new
// process-shared object (RPCS3 create_lv2_shm<true>: exclusive, keyed even by SYS_MMAPPER_NO_SHM_KEY). The firmware's
// ATRAC encoder (libatxenc) issues both.
Result sys_mmapper_allocate_shared_memory_ext(PpuThread& t, u64 ipc_key, u64 size, u32 flags, u32 entries,
    s32 entry_count, u32 mem_id)
{
    if (const Result r = check_granularity(size, flags); r != CELL_OK) return r;
    if (flags & ~SYS_MEMORY_GRANULARITY_MASK) return err(CELL_EINVAL);
    if (const Result r = check_ext_entries(t, entries, entry_count); r != CELL_OK) return r;
    const u32 align = (flags & SYS_MEMORY_PAGE_SIZE_64K) ? 0x10000 : 0x100000;
    return create_shared_memory(t, true, ipc_key, size, align, flags, SYS_SYNC_NEWLY_CREATED, nullptr, mem_id);
}

Result sys_mmapper_allocate_shared_memory_from_container_ext(PpuThread& t, u64 ipc_key, u64 size, u64 flags, u32 cid,
    u32 entries, s32 entry_count, u32 mem_id)
{
    if (const Result r = check_granularity(size, flags); r != CELL_OK) return r;
    if (flags & ~SYS_MEMORY_GRANULARITY_MASK) return err(CELL_EINVAL);
    if (const Result r = check_ext_entries(t, entries, entry_count); r != CELL_OK) return r;
    const auto ct = t.kernel.impl().containers.get<MemoryContainer>(cid);
    if (!ct) return err(CELL_ESRCH);
    const u32 align = (flags & SYS_MEMORY_PAGE_SIZE_64K) ? 0x10000 : 0x100000;
    return create_shared_memory(t, true, ipc_key, size, align, flags, SYS_SYNC_NEWLY_CREATED, ct, mem_id);
}

Result sys_mmapper_free_shared_memory(PpuThread& t, u32 mem_id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto mem = s.objects.get<SharedMemory>(mem_id);
    if (!mem) return err(CELL_ESRCH);
    {
        std::lock_guard lock(s.memory.mutex);
        if (mem->mappings) return err(CELL_EBUSY);
        if (!s.objects.remove(SharedMemory::kIdBase, mem_id)) return err(CELL_ESRCH);
        s.memory.container_or_default(mem->container).give_back(mem->size);
    }
    if (mem->ipc_key) {
        std::lock_guard ipc_lock(s.ipc_mutex);
        s.ipc.erase({SharedMemory::kIdBase, mem->ipc_key});
    }
    return CELL_OK;
}

Result sys_mmapper_map_shared_memory(PpuThread& t, u32 addr, u32 mem_id, u64)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto mem = s.objects.get<SharedMemory>(mem_id);
    std::lock_guard lock(s.memory.mutex);
    Area* area = s.memory.area_at(addr);
    if (!area || area->kind != Area::Kind::Mmapper) return err(CELL_EINVAL);
    if (!mem) return err(CELL_ESRCH);
    if (mem->align < area->page_size) return err(CELL_EINVAL);
    if (addr % area->page_size) return err(CELL_EALIGN);
    if (!area->ranges.alloc_at(addr, mem->size)) return err(CELL_EBUSY);
    if (!s.memory.map_shared(*mem, addr)) {
        area->ranges.free(addr);
        return err(CELL_ENOMEM);
    }
    area->mapped[addr] = mem;
    mem->mappings++;
    return CELL_OK;
}

Result sys_mmapper_search_and_map(PpuThread& t, u32 start_addr, u32 mem_id, u64, u32 alloc_addr)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto mem = s.objects.get<SharedMemory>(mem_id);
    u32 addr = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        Area* area = s.memory.area_at(start_addr);
        if (!area || area->addr != start_addr || area->kind != Area::Kind::Mmapper) return err(CELL_EINVAL);
        if (!mem) return err(CELL_ESRCH);
        if (mem->align < area->page_size) return err(CELL_EALIGN);
        if (!s.guest.valid(alloc_addr, 4)) return err(CELL_EFAULT);
        addr = area->ranges.alloc(mem->size, mem->align);
        if (!addr) return err(CELL_ENOMEM);
        if (!s.memory.map_shared(*mem, addr)) {
            area->ranges.free(addr);
            return err(CELL_ENOMEM);
        }
        area->mapped[addr] = mem;
        mem->mappings++;
    }
    s.guest.w32(alloc_addr, addr);
    return CELL_OK;
}

Result sys_mmapper_unmap_shared_memory(PpuThread& t, u32 addr, u32 mem_id)
{
    Kernel::Impl& s = t.kernel.impl();
    u32 id = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        Area* area = s.memory.area_at(addr);
        if (!area || area->kind != Area::Kind::Mmapper) return err(CELL_EINVAL);
        const auto it = area->mapped.find(addr);
        if (it == area->mapped.end()) return err(CELL_EINVAL);
        if (!s.guest.valid(mem_id, 4)) return err(CELL_EFAULT);
        const std::shared_ptr<SharedMemory> mem = it->second;
        s.memory.unmap_shared(*mem, addr);
        area->ranges.free(addr);
        area->mapped.erase(it);
        mem->mappings--;
        id = mem->id;
    }
    s.guest.w32(mem_id, id);
    return CELL_OK;
}

void register_memory_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 324, sys_memory_container_create);
    PS_LV2_BIND(table, 325, sys_memory_container_destroy);
    PS_LV2_BIND(table, 326, sys_mmapper_allocate_fixed_address);
    PS_LV2_BIND(table, 328, sys_mmapper_allocate_shared_memory_from_container_ext);
    PS_LV2_BIND(table, 329, sys_mmapper_free_shared_memory);
    PS_LV2_BIND(table, 330, sys_mmapper_allocate_address);
    PS_LV2_BIND(table, 331, sys_mmapper_free_address);
    PS_LV2_BIND(table, 332, sys_mmapper_allocate_shared_memory);
    PS_LV2_BIND(table, 334, sys_mmapper_map_shared_memory);
    PS_LV2_BIND(table, 335, sys_mmapper_unmap_shared_memory);
    PS_LV2_BIND(table, 337, sys_mmapper_search_and_map);
    PS_LV2_BIND(table, 339, sys_mmapper_allocate_shared_memory_ext);
    PS_LV2_BIND(table, 341, sys_memory_container_create);
    PS_LV2_BIND(table, 342, sys_memory_container_destroy);
    PS_LV2_BIND(table, 343, sys_memory_container_get_size);
    PS_LV2_BIND(table, 345, sys_memory_container_destroy_parent_with_childs);
    PS_LV2_BIND(table, 348, sys_memory_allocate);
    PS_LV2_BIND(table, 349, sys_memory_free);
    PS_LV2_BIND(table, 350, sys_memory_allocate_from_container);
    PS_LV2_BIND(table, 351, sys_memory_get_page_attribute);
    PS_LV2_BIND(table, 352, sys_memory_get_user_memory_size);
    PS_LV2_BIND(table, 362, sys_mmapper_allocate_shared_memory_from_container);
    PS_LV2_BIND(table, 327, sys_mmapper_enable_page_fault_notification);
}

} // namespace pspocket::lv2
