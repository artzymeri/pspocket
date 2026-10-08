#pragma once

// sys_libc's allocator (D-508): the C library's malloc family and its dlmalloc mspaces, on liblv2's dlmalloc 2.8.3 in
// guest memory (lv2_heap.hpp, D-502: mspaces, FOOTERS, 16-byte alignment, no morecore), with a configuration of the C
// library's own:
// - malloc's heap is a pool of mspaces, each in memory of its own from sys_memory_allocate: 1 MB first, then as much
//   again as the pool has (at most 64 MB more at a time, at least what the request needs), 1 MB pages; the variable
//   _malloc_limit, when not 0, caps the pool's bytes. malloc_trim gives back the mspaces after the first that hold no
//   block. dlmalloc grows its heap with morecore or mmap; liblv2's build has neither, so the pool adds mspaces.
// - mspace_create(base, capacity) makes an mspace in the caller's memory (dlmalloc's create_mspace_with_base, as
//   liblv2's heaps on the caller's memory), or, with a base of 0, in `capacity` bytes it takes from lv2. It does not
//   grow.
// - free, realloc and malloc_usable_size find a block's mspace from its footer (FOOTERS), as dlmalloc does, and
//   refuse a pointer whose footer names no mspace of the library's (logged; dlmalloc's checks ignore it too).
// - realloc keeps a block in place when the new size fits it and does not waste more than half of it (and 256
//   bytes); otherwise it moves it within the block's mspace (malloc, copy, free). independent_calloc and
//   independent_comalloc make each element a block of its own.
// One host mutex for the library's allocator (dlmalloc's lock; no guest code runs inside).

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "libc_guest.hpp"
#include "lv2_heap.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::hle::libc {

// One heap: its mspaces (the first is its handle) and the memory it took from lv2.
struct Pool {
    std::vector<u32> spaces;
    std::vector<u32> empty_used;            // each mspace's mallinfo uordblks while it holds no block
    std::vector<std::pair<u32, u32>> pages; // (address, size) from sys_memory_allocate, per mspace (0: the caller's)
    bool grows = false;
    u32 hint = 0; // the mspace that served the last allocation
};

struct MallocState {
    std::mutex mutex;
    lv2heap::Params params;
    bool params_set = false;
    Pool heap;
    bool heap_made = false;
    std::map<u32, Pool> mspaces;            // by handle
    std::unordered_map<u32, Pool*> owner;   // every mspace of the library -> its pool
    u32 granularity = 0x100000;             // mallopt(M_GRANULARITY)
    u32 max_footprint = 0;
};

inline constexpr u32 kChunkOverhead = 8; // dlmalloc's CHUNK_OVERHEAD with FOOTERS
inline constexpr u32 kPageSize = 0x1000; // mparams.page_size (valloc)
inline constexpr u32 NID_malloc_limit = 0x0B2E15ED;

inline MallocState& malloc_state() { return HleProcess::current()->state<MallocState>(); }

// dlmalloc's mparams for the library's mspaces: the magic from the timebase, as liblv2's init_mparams.
inline const lv2heap::Params& malloc_params(MallocState& s)
{
    if (!s.params_set) {
        const u32 t = static_cast<u32>(ps_ppu_timebase());
        s.params.magic = ((t ^ 0x5A5A5A5Au) | 8u) & ~7u;
        s.params_set = true;
    }
    return s.params;
}

inline u32 heap_bytes(const Pool& p)
{
    u32 n = 0;
    for (const auto& [a, size] : p.pages) n += size;
    return n;
}

// `size` bytes from lv2 (1 MB pages when it is a multiple of 1 MB, else 64 KB): the address, or 0.
inline u32 take_pages(PPUContext& ctx, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar out(ctx, t.kernel.base(), 8);
    const u64 flags = (size % 0x100000) == 0 ? 0x400 : 0x200;
    if (lv2::sys_memory_allocate(t, size, flags, out.addr()) != lv2::CELL_OK) return 0;
    return ps_read32(t.kernel.base(), out.addr());
}

// Adds an mspace in [base, base + size) to `pool` (pages: whether the memory came from lv2). False when it does not
// hold one.
inline bool add_space(PPUContext& ctx, MallocState& s, Pool& pool, u32 base, u32 size, bool pages)
{
    u8* g = guest_base(ctx);
    lv2heap::Space space(g, malloc_params(s));
    const u32 m = space.create_with_base(base, size);
    if (!m) return false;
    pool.spaces.push_back(m);
    pool.empty_used.push_back(space.mallinfo(m).uordblks);
    pool.pages.emplace_back(pages ? base : 0, pages ? size : 0);
    s.owner[m] = &pool;
    return true;
}

// Makes malloc's heap when it has none (its first 1 MB).
inline bool ensure_heap(PPUContext& ctx, MallocState& s)
{
    if (s.heap_made) return true;
    const u32 size = 0x100000;
    const u32 base = take_pages(ctx, size);
    if (!base) {
        PS_LOG_ERROR(kChan, "malloc's heap: no memory from lv2 for its first {:#x} bytes", size);
        return false;
    }
    s.heap = Pool{};
    s.heap.grows = true;
    if (!add_space(ctx, s, s.heap, base, size, true)) {
        lv2::sys_memory_free(thread_of(ctx), base);
        return false;
    }
    s.heap_made = true;
    return true;
}

// Grows a pool so that a request of `bytes` (aligned to `align`) fits.
inline bool grow(PPUContext& ctx, MallocState& s, Pool& pool, u32 bytes, u32 align)
{
    if (!pool.grows) return false;
    const u64 need = u64{bytes} + align + lv2heap::kStateSize + lv2heap::kTopFootSize + 0x100;
    const u64 have = heap_bytes(pool);
    u64 size = std::max<u64>({need, std::min<u64>(have, 0x4000000), s.granularity});
    size = (size + 0xFFFFF) & ~u64{0xFFFFF};
    if (size > 0xF0000000u) return false;
    if (&pool == &s.heap) {
        const u32 limit = ps_read32(guest_base(ctx), variable(NID_malloc_limit));
        if (limit && have + size > limit) {
            if (have + need > limit) return false;
            size = (u64{limit} - have) & ~u64{0xFFFF};
            if (size < need) return false;
        }
    }
    const u32 base = take_pages(ctx, static_cast<u32>(size));
    if (!base) return false;
    if (!add_space(ctx, s, pool, base, static_cast<u32>(size), true)) {
        lv2::sys_memory_free(thread_of(ctx), base);
        return false;
    }
    s.max_footprint = std::max<u32>(s.max_footprint, heap_bytes(s.heap));
    return true;
}

// An allocation from `pool` (alignment 0: malloc), growing it when it may.
inline u32 pool_alloc(PPUContext& ctx, MallocState& s, Pool& pool, u32 bytes, u32 align)
{
    lv2heap::Space space(guest_base(ctx), malloc_params(s));
    const auto one = [&](u32 m) { return align ? space.memalign(m, align, bytes) : space.malloc(m, bytes); };
    if (pool.hint)
        if (const u32 p = one(pool.hint)) return p;
    for (const u32 m : pool.spaces) {
        if (m == pool.hint) continue;
        if (const u32 p = one(m)) {
            pool.hint = m;
            return p;
        }
    }
    if (bytes >= 0xFFFFFF00u - align || !grow(ctx, s, pool, bytes, align)) return 0;
    pool.hint = pool.spaces.back();
    return one(pool.hint);
}

// The mspace of the block at `mem` (from its footer), when it is an in-use block of one of the library's mspaces.
inline Pool* owner_of(PPUContext& ctx, MallocState& s, u32 mem, u32* space)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (mem < 8 || (mem & 7) || !guest_range(t, mem - 8, 8)) return nullptr;
    u8* g = t.kernel.base();
    const u32 p = mem - 8;
    const u32 head = ps_read32(g, p + 4);
    const u32 size = head & ~3u;
    if (!(head & 2) || size < 16 || (size & 7) || u64{p} + size + 4 > GuestMemory::kGuestSize ||
        !guest_range(t, p + size, 4))
        return nullptr;
    const u32 m = ps_read32(g, p + size) ^ malloc_params(s).magic;
    const auto it = s.owner.find(m);
    if (it == s.owner.end()) return nullptr;
    *space = m;
    return it->second;
}

// The usable bytes of an in-use block (dlmalloc's malloc_usable_size).
inline u32 usable_size(PPUContext& ctx, u32 mem)
{
    return (ps_read32(guest_base(ctx), mem - 4) & ~3u) - kChunkOverhead;
}

inline u32 lib_malloc(PPUContext& ctx, u32 bytes)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    if (!ensure_heap(ctx, s)) return 0;
    const u32 p = pool_alloc(ctx, s, s.heap, bytes, 0);
    if (!p) set_errno(ctx, kENOMEM);
    return p;
}

inline u32 lib_memalign(PPUContext& ctx, u32 align, u32 bytes)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    if (!ensure_heap(ctx, s)) return 0;
    const u32 p = pool_alloc(ctx, s, s.heap, bytes, align);
    if (!p) set_errno(ctx, kENOMEM);
    return p;
}

// free (any mspace's block: FOOTERS); `what` names the caller in a refusal.
inline void lib_free(PPUContext& ctx, u32 mem, std::string_view what = "free")
{
    if (!mem) return;
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    u32 m = 0;
    if (!owner_of(ctx, s, mem, &m)) {
        refuse(ctx, std::format("{}({:#x}): not a block of the C library's heaps; nothing freed", what, mem));
        return;
    }
    lv2heap::Space(guest_base(ctx), malloc_params(s)).free(m, mem);
}

// realloc and reallocalign within the block's own mspace.
inline u32 lib_realloc(PPUContext& ctx, u32 mem, u32 bytes, u32 align, std::string_view what)
{
    MallocState& s = malloc_state();
    std::unique_lock lock(s.mutex);
    u32 m = 0;
    Pool* pool = owner_of(ctx, s, mem, &m);
    if (!pool) {
        lock.unlock();
        refuse(ctx, std::format("{}({:#x}, {:#x}): not a block of the C library's heaps; NULL", what, mem, bytes));
        return 0;
    }
    const u32 have = usable_size(ctx, mem);
    const bool aligned = !align || (mem & (align - 1)) == 0;
    if (aligned && bytes <= have && (have - bytes <= 256 || bytes >= have / 2)) return mem;
    const u32 p = pool_alloc(ctx, s, *pool, bytes, align);
    if (!p) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    u8* g = guest_base(ctx);
    std::memmove(g + p, g + mem, std::min(have, bytes));
    lv2heap::Space(g, malloc_params(s)).free(m, mem);
    return p;
}

// dlmalloc's mallinfo of a pool: the sums of its mspaces' (usmblks the most the pool has had).
inline lv2heap::MallInfo pool_info(PPUContext& ctx, MallocState& s, const Pool& pool)
{
    lv2heap::Space space(guest_base(ctx), malloc_params(s));
    lv2heap::MallInfo sum;
    for (const u32 m : pool.spaces) {
        const lv2heap::MallInfo mi = space.mallinfo(m);
        sum.arena += mi.arena;
        sum.ordblks += mi.ordblks;
        sum.smblks += mi.smblks;
        sum.hblks += mi.hblks;
        sum.hblkhd += mi.hblkhd;
        sum.usmblks += mi.usmblks;
        sum.fsmblks += mi.fsmblks;
        sum.uordblks += mi.uordblks;
        sum.fordblks += mi.fordblks;
        sum.keepcost += mi.keepcost;
    }
    return sum;
}

// Whether the mspace at index i of the pool holds no block.
inline bool space_empty(PPUContext& ctx, MallocState& s, const Pool& pool, usize i)
{
    return lv2heap::Space(guest_base(ctx), malloc_params(s)).mallinfo(pool.spaces[i]).uordblks <= pool.empty_used[i];
}

// Gives back the memory of a pool's mspace i (when it took it from lv2) and forgets the mspace.
inline void drop_space(PPUContext& ctx, MallocState& s, Pool& pool, usize i)
{
    s.owner.erase(pool.spaces[i]);
    if (pool.pages[i].first) lv2::sys_memory_free(thread_of(ctx), pool.pages[i].first);
    if (pool.hint == pool.spaces[i]) pool.hint = 0;
    pool.spaces.erase(pool.spaces.begin() + static_cast<std::ptrdiff_t>(i));
    pool.empty_used.erase(pool.empty_used.begin() + static_cast<std::ptrdiff_t>(i));
    pool.pages.erase(pool.pages.begin() + static_cast<std::ptrdiff_t>(i));
}

} // namespace pspocket::hle::libc
