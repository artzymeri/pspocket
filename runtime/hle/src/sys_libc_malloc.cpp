// sys_libc part 3 (D-513): the C library's malloc family and its dlmalloc mspaces, on liblv2's dlmalloc 2.8.3 in
// guest memory (libc_malloc.hpp has the pool of mspaces malloc's heap is and the rules it follows), and strdup.
// Registered under sys_libc and sys_libm with the rest of the C library.

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string>

#include "libc_guest.hpp"
#include "libc_malloc.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

// dlmalloc's MAX_REQUEST for 32-bit sizes: larger requests fail before anything is looked at.
constexpr u32 kMaxRequest = 0xFFFFFFC0u;

// dlmalloc's internal_memalign: an alignment that is not a power of two is rounded up to one (from 32).
u32 power_of_two(u32 align)
{
    if (align <= 16 || std::has_single_bit(align)) return align;
    u32 a = 32;
    while (a < align && a < 0x80000000u) a <<= 1;
    return a;
}

u32 malloc_(PPUContext& ctx, u32 bytes) { return lib_malloc(ctx, bytes); }
void free_(PPUContext& ctx, u32 mem) { lib_free(ctx, mem); }

// calloc: n * size bytes, cleared; an overflowing product fails (dlmalloc forces the request to MAX_SIZE_T).
u32 calloc_(PPUContext& ctx, u32 n, u32 size)
{
    const u64 total = u64{n} * size;
    if (total >= kMaxRequest) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    const u32 p = lib_malloc(ctx, static_cast<u32>(total));
    if (p) std::memset(guest_base(ctx) + p, 0, total);
    return p;
}

// realloc(NULL, n) is malloc; realloc(p, 0) gives a smallest block (dlmalloc without REALLOC_ZERO_BYTES_FREES).
u32 realloc_(PPUContext& ctx, u32 mem, u32 bytes)
{
    if (!mem) return lib_malloc(ctx, bytes);
    if (bytes >= kMaxRequest) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    return lib_realloc(ctx, mem, bytes, 0, "realloc");
}

u32 memalign_(PPUContext& ctx, u32 align, u32 bytes) { return lib_memalign(ctx, power_of_two(align), bytes); }

// Sony's reallocalign(ptr, size, boundary): realloc to a block aligned to the boundary.
u32 reallocalign_(PPUContext& ctx, u32 mem, u32 bytes, u32 align)
{
    align = power_of_two(align);
    if (!mem) return lib_memalign(ctx, align, bytes);
    if (bytes >= kMaxRequest) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    return lib_realloc(ctx, mem, bytes, align <= 16 ? 0 : align, "reallocalign");
}

u32 valloc_(PPUContext& ctx, u32 bytes) { return lib_memalign(ctx, kPageSize, bytes); }

u32 pvalloc_(PPUContext& ctx, u32 bytes)
{
    if (bytes > kMaxRequest - kPageSize) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    return lib_memalign(ctx, kPageSize, (bytes + kPageSize - 1) & ~(kPageSize - 1));
}

u32 malloc_usable_size_(PPUContext& ctx, u32 mem)
{
    if (!mem) return 0;
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    u32 m = 0;
    if (!owner_of(ctx, s, mem, &m)) {
        refuse(ctx, std::format("malloc_usable_size({:#x}): not a block of the C library's heaps; 0", mem));
        return 0;
    }
    return usable_size(ctx, mem);
}

// The figures of a pool as dlmalloc's malloc_stats prints them: the most it has had, what it has, what is in use.
struct Footprint {
    u32 max = 0, now = 0, used = 0;
};

Footprint footprint(PPUContext& ctx, MallocState& s, const Pool& pool, u32 max_seen)
{
    const lv2heap::MallInfo mi = pool_info(ctx, s, pool);
    Footprint f;
    f.now = mi.arena + mi.hblkhd;
    f.used = mi.uordblks;
    f.max = std::max({max_seen, f.now, mi.usmblks});
    return f;
}

void print_stats(PPUContext& ctx, const Footprint& f)
{
    // dlmalloc's internal_malloc_stats, to stderr (the TTY).
    tty_out(ctx, std::format("max system bytes = {:10}\nsystem bytes     = {:10}\nin use bytes     = {:10}\n", f.max,
                             f.now, f.used));
}

// mallinfo: struct mallinfo, ten size_t, returned through the caller's buffer in r3 (the 64-bit ABI's aggregates).
void mallinfo_(PPUContext& ctx, u32 out)
{
    if (!range_ok(ctx, "mallinfo", out, 40)) return;
    MallocState& s = malloc_state();
    lv2heap::MallInfo mi;
    {
        std::lock_guard lock(s.mutex);
        if (s.heap_made) {
            mi = pool_info(ctx, s, s.heap);
            mi.usmblks = std::max({mi.usmblks, s.max_footprint, mi.arena + mi.hblkhd});
        }
    }
    const u32 words[] = {mi.arena, mi.ordblks, mi.smblks, mi.hblks, mi.hblkhd, mi.usmblks, mi.fsmblks, mi.uordblks,
        mi.fordblks, mi.keepcost};
    for (u32 i = 0; i < 10; i++) ps_write32(guest_base(ctx), out + 4 * i, words[i]);
}

void malloc_stats_(PPUContext& ctx)
{
    MallocState& s = malloc_state();
    Footprint f;
    {
        std::lock_guard lock(s.mutex);
        if (s.heap_made) f = footprint(ctx, s, s.heap, s.max_footprint);
    }
    print_stats(ctx, f);
}

u32 malloc_footprint_(PPUContext&)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    return s.heap_made ? heap_bytes(s.heap) : 0;
}

// malloc_trim: gives lv2 back the memory of every mspace after the first that holds no block; 1 when it gave any.
s32 malloc_trim_(PPUContext& ctx, u32)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    if (!s.heap_made) return 0;
    bool gave = false;
    for (usize i = s.heap.spaces.size(); i-- > 1;) {
        if (space_empty(ctx, s, s.heap, i)) {
            drop_space(ctx, s, s.heap, i);
            gave = true;
        }
    }
    return gave ? 1 : 0;
}

// mallopt: M_TRIM_THRESHOLD (-1), M_GRANULARITY (-2: a power of two, at least a page), M_MMAP_THRESHOLD (-3); 1 when
// the parameter is one of them and the value fits, else 0 (dlmalloc's change_mparam). The pool grows by at least the
// granularity; the thresholds have nothing to act on (no trimming of the top, no mmap).
s32 mallopt_(PPUContext&, s32 param, s32 value)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    const u32 v = static_cast<u32>(value);
    switch (param) {
    case -1: return 1;
    case -2:
        if (v >= kPageSize && std::has_single_bit(v)) {
            s.granularity = v;
            return 1;
        }
        return 0;
    case -3: return 1;
    default: return 0;
    }
}

// independent_calloc and independent_comalloc (dlmalloc's ialloc): n blocks, each a block of its own here (free
// takes any of them), cleared for calloc; their pointers in `chunks`, or in an array malloc'ed after them when it is
// NULL. NULL when one fails (the ones made are freed).
u32 ialloc(PPUContext& ctx, const char* fn, u32 n, u32 elem, u32 sizes, u32 chunks, bool clear)
{
    if (!n) return chunks ? chunks : lib_malloc(ctx, 0);
    if (sizes && !range_ok(ctx, fn, sizes, u64{n} * 4)) return 0;
    if (chunks && !range_ok(ctx, fn, chunks, u64{n} * 4)) return 0;
    u8* g = guest_base(ctx);
    std::vector<u32> made;
    made.reserve(n);
    for (u32 i = 0; i < n; i++) {
        const u32 size = sizes ? ps_read32(g, sizes + 4 * i) : elem;
        const u32 p = size < kMaxRequest ? lib_malloc(ctx, size) : 0;
        if (!p) {
            for (const u32 q : made) lib_free(ctx, q, fn);
            set_errno(ctx, kENOMEM);
            return 0;
        }
        if (clear) std::memset(g + p, 0, size);
        made.push_back(p);
    }
    u32 array = chunks;
    if (!array) {
        array = lib_malloc(ctx, n * 4);
        if (!array) {
            for (const u32 q : made) lib_free(ctx, q, fn);
            return 0;
        }
    }
    for (u32 i = 0; i < n; i++) ps_write32(g, array + 4 * i, made[i]);
    return array;
}

u32 independent_calloc_(PPUContext& ctx, u32 n, u32 elem, u32 chunks)
{
    if (u64{n} * elem >= kMaxRequest) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    return ialloc(ctx, "independent_calloc", n, elem, 0, chunks, true);
}

u32 independent_comalloc_(PPUContext& ctx, u32 n, u32 sizes, u32 chunks)
{
    return ialloc(ctx, "independent_comalloc", n, 0, sizes, chunks, false);
}

// ------------------------------------------------------------------------------------------------- mspaces

// The pool of an mspace handle mspace_create gave, or nullptr (refused, logged).
Pool* mspace_of(PPUContext& ctx, MallocState& s, const char* fn, u32 msp)
{
    const auto it = s.mspaces.find(msp);
    if (it != s.mspaces.end()) return &it->second;
    refuse(ctx, std::format("{}({:#x}): not an mspace of mspace_create", fn, msp));
    return nullptr;
}

// mspace_create(base, capacity): an mspace in the caller's memory, or with a base of 0 in `capacity` bytes it takes
// from lv2 (dlmalloc's create_mspace_with_base and create_mspace without morecore). It does not grow. NULL when the
// memory cannot hold one.
u32 mspace_create_(PPUContext& ctx, u32 base, u32 capacity)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    bool pages = false;
    if (!base) {
        if (!capacity || capacity > 0xF0000000u) return 0;
        const u32 size = (capacity + 0xFFFF) & ~0xFFFFu;
        base = take_pages(ctx, size);
        if (!base) return 0;
        capacity = size;
        pages = true;
    } else if (!range_ok(ctx, "mspace_create", base, capacity)) {
        return 0;
    }
    Pool pool;
    if (!add_space(ctx, s, pool, base, capacity, pages)) {
        if (pages) lv2::sys_memory_free(thread_of(ctx), base);
        return 0;
    }
    const u32 m = pool.spaces.front();
    s.owner.erase(m);
    Pool& kept = s.mspaces[m] = std::move(pool);
    s.owner[m] = &kept;
    return m;
}

// mspace_destroy: the bytes given back to lv2 (0 for the caller's memory, as destroy_mspace's EXTERN_BIT).
u32 mspace_destroy_(PPUContext& ctx, u32 msp)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    Pool* pool = mspace_of(ctx, s, "mspace_destroy", msp);
    if (!pool) return 0;
    u32 freed = 0;
    for (const auto& [addr, size] : pool->pages) {
        if (addr) {
            lv2::sys_memory_free(thread_of(ctx), addr);
            freed += size;
        }
    }
    for (const u32 m : pool->spaces) s.owner.erase(m);
    s.mspaces.erase(msp);
    return freed;
}

u32 mspace_alloc(PPUContext& ctx, const char* fn, u32 msp, u32 bytes, u32 align)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    Pool* pool = mspace_of(ctx, s, fn, msp);
    if (!pool) return 0;
    const u32 p = bytes < kMaxRequest ? pool_alloc(ctx, s, *pool, bytes, align) : 0;
    if (!p) set_errno(ctx, kENOMEM);
    return p;
}

u32 mspace_malloc_(PPUContext& ctx, u32 msp, u32 bytes) { return mspace_alloc(ctx, "mspace_malloc", msp, bytes, 0); }

u32 mspace_memalign_(PPUContext& ctx, u32 msp, u32 align, u32 bytes)
{
    return mspace_alloc(ctx, "mspace_memalign", msp, bytes, power_of_two(align));
}

u32 mspace_calloc_(PPUContext& ctx, u32 msp, u32 n, u32 size)
{
    const u64 total = u64{n} * size;
    const u32 p = mspace_alloc(ctx, "mspace_calloc", msp, total < kMaxRequest ? static_cast<u32>(total) : kMaxRequest, 0);
    if (p) std::memset(guest_base(ctx) + p, 0, total);
    return p;
}

// mspace_free takes the block's mspace from its footer, as dlmalloc's does with FOOTERS (msp is not used).
void mspace_free_(PPUContext& ctx, u32, u32 mem) { lib_free(ctx, mem, "mspace_free"); }

u32 mspace_realloc_(PPUContext& ctx, u32 msp, u32 mem, u32 bytes)
{
    if (!mem) return mspace_malloc_(ctx, msp, bytes);
    if (bytes >= kMaxRequest) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    return lib_realloc(ctx, mem, bytes, 0, "mspace_realloc");
}

u32 mspace_reallocalign_(PPUContext& ctx, u32 msp, u32 mem, u32 bytes, u32 align)
{
    align = power_of_two(align);
    if (!mem) return mspace_memalign_(ctx, msp, align, bytes);
    if (bytes >= kMaxRequest) {
        set_errno(ctx, kENOMEM);
        return 0;
    }
    return lib_realloc(ctx, mem, bytes, align <= 16 ? 0 : align, "mspace_reallocalign");
}

u32 mspace_malloc_usable_size_(PPUContext& ctx, u32 mem) { return malloc_usable_size_(ctx, mem); }

void mspace_malloc_stats_(PPUContext& ctx, u32 msp)
{
    MallocState& s = malloc_state();
    Footprint f;
    {
        std::lock_guard lock(s.mutex);
        Pool* pool = mspace_of(ctx, s, "mspace_malloc_stats", msp);
        if (!pool) return;
        f = footprint(ctx, s, *pool, 0);
    }
    print_stats(ctx, f);
}

// Sony's mspace_is_heap_empty: 1 when the mspace holds no block.
s32 mspace_is_heap_empty_(PPUContext& ctx, u32 msp)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    Pool* pool = mspace_of(ctx, s, "mspace_is_heap_empty", msp);
    if (!pool) return 0;
    for (usize i = 0; i < pool->spaces.size(); i++)
        if (!space_empty(ctx, s, *pool, i)) return 0;
    return 1;
}

// ------------------------------------------------------------------------------------------------- set-up

// The library's start-up and shut-down of malloc (its crt calls them): the heap is made at the first allocation, so
// _malloc_init only makes it early; _malloc_finalize gives back every mspace of malloc's heap. The _lv2 forms are the
// same here (the heap's memory is lv2's either way); the lock is a host mutex, so the lock set-ups have nothing to do.
s32 _malloc_init_(PPUContext& ctx)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    return ensure_heap(ctx, s) ? 0 : -1;
}

void _malloc_finalize_(PPUContext& ctx)
{
    MallocState& s = malloc_state();
    std::lock_guard lock(s.mutex);
    if (!s.heap_made) return;
    for (usize i = s.heap.spaces.size(); i-- > 0;) drop_space(ctx, s, s.heap, i);
    s.heap = Pool{};
    s.heap_made = false;
}

s32 _malloc_init_lv2_(PPUContext& ctx) { return _malloc_init_(ctx); }
void _malloc_finalize_lv2_(PPUContext& ctx) { _malloc_finalize_(ctx); }
s32 _init_malloc_lock_(PPUContext&) { return 0; }
s32 _init_malloc_lock0_(PPUContext&) { return 0; }

// _allocate_mapped_pages(size): the library's mmap for dlmalloc, pages from lv2 (1 MB pages for a multiple of 1 MB,
// else 64 KB); MFAIL ((void*)-1) when lv2 has none.
u32 _allocate_mapped_pages_(PPUContext& ctx, u32 size)
{
    if (!size || size > 0xF0000000u) return ~0u;
    const u32 rounded = (size % 0x100000) == 0 ? size : (size + 0xFFFF) & ~0xFFFFu;
    const u32 p = take_pages(ctx, rounded);
    return p ? p : ~0u;
}

// strdup: a copy of the string from malloc.
u32 strdup_(PPUContext& ctx, u32 s)
{
    const auto t = read_string(thread_of(ctx), s);
    if (!t) {
        refuse(ctx, std::format("strdup: the string at {:#x} runs out of guest memory", s));
        return 0;
    }
    const u32 p = lib_malloc(ctx, static_cast<u32>(t->size() + 1));
    if (p) write_string(thread_of(ctx), p, *t);
    return p;
}

// _malloc_limit: 0 (no cap on malloc's pool) until the program sets it.
void malloc_limit_init(u8*, u32) {}

} // namespace

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_malloc(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(malloc), LIBC(free), LIBC(calloc), LIBC(realloc), LIBC(memalign), LIBC(reallocalign), LIBC(valloc),
            LIBC(pvalloc), LIBC(malloc_usable_size), LIBC(mallinfo), LIBC(malloc_stats), LIBC(malloc_footprint),
            LIBC(malloc_trim), LIBC(mallopt), LIBC(independent_calloc), LIBC(independent_comalloc),
            LIBC(mspace_create), LIBC(mspace_destroy), LIBC(mspace_malloc), LIBC(mspace_memalign),
            LIBC(mspace_calloc), LIBC(mspace_free), LIBC(mspace_realloc), LIBC(mspace_reallocalign),
            LIBC(mspace_malloc_usable_size), LIBC(mspace_malloc_stats), LIBC(mspace_is_heap_empty),
            LIBC(_malloc_init), LIBC(_malloc_finalize), LIBC(_malloc_init_lv2), LIBC(_malloc_finalize_lv2),
            LIBC(_init_malloc_lock), LIBC(_init_malloc_lock0), LIBC(_allocate_mapped_pages), LIBC(strdup),
        });
    }
    r.add_variables("sys_libc", {Variable{{}, "_malloc_limit", NID_malloc_limit, 4, 4, &malloc_limit_init}});
}
#undef LIBC

} // namespace pspocket::hle
