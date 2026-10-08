// cellSheap: the SPURS shared heap, a buddy allocator over main memory that PPU and SPU code share. It is part of
// Sony's libsre. RPCS3 has only UNIMPLEMENTED stubs (rpcs3/Emu/Cell/Modules/cellSheap.cpp, which also lists the error
// codes), so the reference is libsre itself (fw 4.92, read on the Mac and never copied; D-037, D-042). Native SPURS
// keeps the heap where libsre keeps it, in guest memory, with libsre's layout and algorithm, so SPU code sharing the
// heap and the game see the same state:
//
// - Header: the first 128 bytes at `ea`. 0x00 lock word (0 free, 1 held; Allocate and Free spin on it), 0x08 the
//   header's own address (u64), 0x10 the node tree (bitmap address u64, bitmap size u64 at 0x18, node count u32 at
//   0x20), 0x28 the first block's address (u64), 0x30 the tree's span in bytes (u64, a power of two), 0x38 the usable
//   size (u64), 0x40 and 0x44 the DMA tag. cellKeySheapInitialize keeps its key table at 0x48 (not used by GTA V).
// - Bitmap: right after the header. 2 bits per tree node: node 1 is the whole span, node i has children 2i and
//   2i + 1, the leaves are 128-byte blocks. 16 nodes per big-endian word, the lowest node in the top bits. States:
//   0 free, 1 allocated (one allocation), 2 split (free space below), 3 full (no free space below). The blocks follow
//   the bitmap. Nodes past the usable size are marked allocated when the heap is initialized.
//
// Where libsre asserts (it prints "Aborted." and ends the process), pspocket stops with a fatal log. Where libsre
// would read a bad pointer, pspocket checks first and logs the call site (D-042).

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <thread>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSheap";

enum : u32 {
    CELL_SHEAP_ERROR_INVAL = 0x80410302,
    CELL_SHEAP_ERROR_BUSY = 0x8041030A,
    CELL_SHEAP_ERROR_FAULT = 0x8041030D, // libsre's cellSheapFree on a split or full leaf (0x0D: FAULT, as elsewhere)
    CELL_SHEAP_ERROR_ALIGN = 0x80410310,
    CELL_SHEAP_ERROR_SHORTAGE = 0x80410312,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kBlock = 128;     // a leaf of the tree
constexpr u32 kHeaderSize = 128;
constexpr u32 kMaxDmaTag = 31;

namespace heap_off {
constexpr u32 lock = 0x00, self = 0x08, tree = 0x10, bitmap = 0x10, bitmap_size = 0x18, count = 0x20, first_block = 0x28,
              span = 0x30, usable = 0x38, dma_tag = 0x40, dma_tag2 = 0x44;
}

enum : u32 { NODE_FREE = 0, NODE_ALLOCATED = 1, NODE_SPLIT = 2, NODE_FULL = 3 };

// The node tree of a heap (libsre's cellNodeStateTree).
struct Tree {
    u8* base;
    u32 bitmap;
    u32 count; // nodes 1..count exist

    u32 word(u32 i) const { return bitmap + (i >> 4) * 4; }
    static u32 shift(u32 i) { return 30 - 2 * (i & 15); }
    u32 get(u32 i) const { return (ps_read32(base, word(i)) >> shift(i)) & 3; }
    void set(u32 i, u32 state) const
    {
        const u32 w = word(i);
        ps_write32(base, w, (ps_read32(base, w) & ~(3u << shift(i))) | (state << shift(i)));
    }
    // A node past the tree reads as allocated, as in libsre.
    u32 get_or_used(u32 i) const { return i <= count ? get(i) : NODE_ALLOCATED; }
};

Tree tree_of(u8* base, u32 heap)
{
    return Tree{base, static_cast<u32>(ps_read64(base, heap + heap_off::bitmap)), ps_read32(base, heap + heap_off::count)};
}

// Node `x` becomes allocated; its ancestors become full while both children are used (a child past the tree counts
// as used), and the ancestors above that become split up to the first one that already is.
void mark_allocated(const Tree& t, u32 x)
{
    t.set(x, NODE_ALLOCATED);
    while (x > 1) {
        const u32 buddy = (x & 1) ? x - 1 : x + 1;
        if (buddy <= t.count) {
            const u32 s = t.get(buddy);
            if (s != NODE_ALLOCATED && s != NODE_FULL) break;
        }
        x >>= 1;
        t.set(x, NODE_FULL);
    }
    for (u32 p = x >> 1; p != 0; p >>= 1) {
        const u32 s = t.get_or_used(p);
        if (s == NODE_SPLIT) break;
        if (s != NODE_FREE) {
            PS_FATAL(kChan, "heap bitmap {:#x}: node {} is in state {} above a new allocation; libsre (fw 4.92) asserts "
                "here and aborts the process", t.bitmap, p, s);
        }
        t.set(p, NODE_SPLIT);
    }
}

// libsre's tree initialization: clears the bitmap, then walks up the right edge of the tree from the last node and
// marks every missing right sibling (a subtree past the usable size) allocated, until it reaches a complete level.
void init_tree(u8* base, u32 tree, u32 bitmap, u32 bitmap_size, u32 count)
{
    ps_write64(base, tree + 0x00, bitmap);
    ps_write64(base, tree + 0x08, bitmap_size);
    ps_write32(base, tree + 0x10, count);
    std::memset(base + bitmap, 0, bitmap_size);
    const Tree t{base, bitmap, count};
    for (u32 n = count;;) {
        // n is the last node of its depth that covers usable space.
        if (n == 0) PS_FATAL(kChan, "heap bitmap {:#x}: node count {} leaves no complete level", bitmap, count);
        const u32 sh = static_cast<u32>(32 - std::countl_zero(n - 1));
        if ((sh < 32 ? 1u << sh : 0) - n == 1) return; // n = 2^k - 1: the depth is complete
        if (n & 1) {
            n >>= 1;
            continue;
        }
        const u32 x = n + 1;
        if (x <= count && t.get(x) != NODE_FREE) {
            PS_FATAL(kChan, "heap bitmap {:#x}: padding node {} is not free; libsre (fw 4.92) asserts here", bitmap, x);
        }
        mark_allocated(t, x);
        n >>= 1;
    }
}

// libsre's search for a free node at depth `order` (node 1 is depth 0): from the leftmost node of that depth, skipping
// the whole span of every used subtree. A free node can be taken when its nearest non-free ancestor is split (or it
// has none, in an empty heap). libsre's loop stops one node short, so the last node of a depth is never taken.
// Returns the node, or 0.
u32 tree_alloc(const Tree& t, u32 order)
{
    PS_CHECK(order < 32);
    const u32 first = 1u << order;
    const u32 last = std::min(2 * first - 1, t.count);
    if (first >= last) return 0;
    for (u32 c = first;;) {
        u32 top = c; // the used subtree to skip
        if (t.get_or_used(c) == NODE_FREE) {
            u32 a = c >> 1, s = NODE_FREE;
            for (; a != 0; a >>= 1) {
                s = t.get_or_used(a);
                if (s != NODE_FREE) break;
            }
            if (a == 0 || s == NODE_SPLIT) {
                mark_allocated(t, c);
                return c;
            }
            if (s == NODE_FULL) {
                PS_FATAL(kChan, "heap bitmap {:#x}: free node {} lies below full node {}; libsre (fw 4.92) asserts here",
                    t.bitmap, c, a);
            }
            top = a; // c lies inside an allocated block
        }
        // Skip the largest used subtree around `top`.
        for (u32 p = top >> 1;; p >>= 1) {
            const u32 s = t.get_or_used(p);
            if (s != NODE_ALLOCATED && s != NODE_FULL) break;
            top = p;
        }
        u32 next = top + 1;
        while (c >= next) next <<= 1;
        PS_CHECK(next != 0);
        if (next >= last) return 0;
        c = next;
    }
}

// libsre's free of leaf `i`: a leaf inside a larger allocation frees that allocation (the leaf's nearest non-free
// ancestor); full ancestors become split, and free buddies merge upwards.
s32 tree_free(const Tree& t, u32 i)
{
    if (i > t.count) return error(CELL_SHEAP_ERROR_INVAL);
    const u32 s = t.get(i);
    if (s == NODE_SPLIT || s == NODE_FULL) return error(CELL_SHEAP_ERROR_FAULT);
    if (s == NODE_FREE) {
        for (;;) {
            i >>= 1;
            if (i == 0) return error(CELL_SHEAP_ERROR_INVAL);
            if (i > t.count) break;
            const u32 a = t.get(i);
            if (a == NODE_FREE) continue;
            if (a != NODE_ALLOCATED) {
                PS_FATAL(kChan, "heap bitmap {:#x}: freeing an address in free space (node {} is in state {}); libsre "
                    "(fw 4.92) asserts here and aborts the process", t.bitmap, i, a);
            }
            break;
        }
    }
    t.set(i, NODE_FREE);
    for (u32 p = i >> 1; p != 0 && p <= t.count && t.get(p) == NODE_FULL; p >>= 1) t.set(p, NODE_SPLIT);
    while (i > 1) {
        const u32 buddy = (i & 1) ? i - 1 : i + 1;
        i >>= 1;
        if (buddy > t.count || t.get(buddy) != NODE_FREE) break;
        t.set(i, NODE_FREE);
    }
    return lv2::CELL_OK;
}

// Whether every page of [addr, addr + size) is committed guest memory (libsre reads and writes the header and the
// bitmap without checks).
bool range_committed(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    for (u64 a = addr & ~u64{0xFFF}; a < u64{addr} + size; a += 0x1000) {
        if (!guest_readable(t, static_cast<u32>(std::max<u64>(a, addr)), 1)) return false;
    }
    return true;
}

// The heap's header and bitmap, checked before Allocate or Free reads them: committed, and a bitmap large enough
// for the node count (nodes up to count + 1 are touched).
bool heap_readable(PPUContext& ctx, const char* what, u32 heap)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (range_committed(t, heap, kHeaderSize)) {
        const u64 bitmap_size = ps_read64(base, heap + heap_off::bitmap_size);
        const u32 count = ps_read32(base, heap + heap_off::count);
        if ((count == 0 || (u64{count} + 1) / 16 * 4 + 4 <= bitmap_size) &&
            range_committed(t, static_cast<u32>(ps_read64(base, heap + heap_off::bitmap)), bitmap_size)) {
            return true;
        }
    }
    log_call_site(ctx, base, std::format("{}: heap {:#x} (or its bitmap) is not readable guest memory", what, heap));
    return false;
}

// The heap's spin lock (word 0 of the header), taken through the reservation system so it is atomic against PPU
// lwarx/stwcx. and SPU GETLLAR/PUTLLC on the header line. libsre spins without limit; a lock held for long is
// logged once.
void lock_heap(u8* base, u32 heap)
{
    for (u64 spins = 0; !reservation_op(base, heap, [](u8* line) {
             if (ps_read32(line, heap_off::lock)) return false;
             ps_write32(line, heap_off::lock, 1);
             return true;
         });
         spins++) {
        if (spins == (u64{1} << 22)) PS_LOG_WARN(kChan, "heap {:#x}: still waiting for its lock (word 0)", heap);
        std::this_thread::yield();
    }
}

void unlock_heap(u8* base, u32 heap)
{
    reservation_op(base, heap, [](u8* line) {
        ps_write32(line, heap_off::lock, 0);
        return true;
    });
}

// The smallest power of two >= v (v >= 1), computed as libsre does (a 32-bit shift: 0 past 2^31).
u64 pow2_at_least(u64 v)
{
    const u32 sh = static_cast<u32>(64 - std::countl_zero(v - 1));
    return sh < 32 ? u64{1} << sh : 0;
}

// libsre's internal initialize (0x53B0): the 128-byte header at `ea`, the bitmap at `area` (128-aligned) and the blocks
// after the bitmap, over `area_size` bytes (a multiple of 128). The bitmap's size is computed for a tree over the whole
// area, the tree itself over what the bitmap leaves.
s32 cellSheapInitializeAt(PPUContext& ctx, u32 ea, u32 area, u64 area_size, u32 dma_tag)
{
    const u64 units = area_size / kBlock;
    const u64 nodes = units + pow2_at_least(units) - 1;
    if (nodes > 0xFFFFFFFF) return error(CELL_SHEAP_ERROR_INVAL);
    const u64 bitmap_size = ((u64{static_cast<u32>(2 * nodes + 2)} + 7) / 8 + kBlock - 1) & ~u64{kBlock - 1};
    const u64 usable = area_size - bitmap_size;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!range_committed(t, ea, kHeaderSize) || !range_committed(t, area, bitmap_size)) {
        log_call_site(ctx, base, std::format("cellSheapInitialize: the heap header at {:#x} or its bitmap at {:#x} ({:#x} "
            "bytes) is not committed guest memory; refused with CELL_SHEAP_ERROR_INVAL", ea, area, bitmap_size));
        return error(CELL_SHEAP_ERROR_INVAL);
    }

    ps_write64(base, ea + heap_off::self, ea);
    ps_write32(base, ea + heap_off::lock, 0);
    ps_write32(base, ea + heap_off::dma_tag2, dma_tag);
    ps_write32(base, ea + heap_off::dma_tag, dma_tag);
    ps_write64(base, ea + heap_off::usable, usable);
    const u64 usable_units = usable / kBlock;
    init_tree(base, ea + heap_off::tree, area, static_cast<u32>(bitmap_size),
        static_cast<u32>(pow2_at_least(usable_units) + usable_units - 1));
    const u64 span = usable ? pow2_at_least(usable) : 0;
    ps_write64(base, ea + heap_off::first_block, static_cast<u32>(area + bitmap_size));
    ps_write64(base, ea + heap_off::span, span);
    PS_LOG_INFO(kChan, "heap {:#x}: blocks from {:#x}, {:#x} bytes usable (span {:#x}), bitmap {:#x} bytes", ea,
        area + bitmap_size, usable, span, bitmap_size);
    return lv2::CELL_OK;
}

// cellSheapInitialize(ea, size, dmaTag), as libsre: ea non-null and 128-byte aligned, 2 KB <= size <= 512 GB (size
// is a 64-bit argument and is rounded down to 128 bytes), a DMA tag 0..31. The 128-byte header goes at ea, the
// bitmap after it and the blocks after the bitmap; nothing else in the range is touched. Run 008: GTA V initializes
// one 512 KB heap with tag 0.
s32 cellSheapInitialize(PPUContext& ctx, u32 ea, u64 size, u32 dma_tag)
{
    PS_LOG_INFO(kChan, "cellSheapInitialize(ea {:#x}, size {:#x}, dmaTag {})", ea, size, dma_tag);
    if (!ea) return error(CELL_SHEAP_ERROR_INVAL);
    if (ea % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (size < 0x800 || size > (u64{0x80} << 32) || dma_tag > kMaxDmaTag) return error(CELL_SHEAP_ERROR_INVAL);
    return cellSheapInitializeAt(ctx, ea, ea + kHeaderSize, (size - kHeaderSize) & ~u64{kBlock - 1}, dma_tag);
}

// cellSheapAllocate(heap, size) -> address, as libsre: 0 when the heap is misaligned, the size is larger than the
// tree's span or no block is free (no error code). The block is the smallest power of two >= size (128 bytes at
// least) and is aligned to its size relative to the first block. A size of 0 counts as 1.
u32 cellSheapAllocate(PPUContext& ctx, u32 heap, u64 size)
{
    if (!size) size = 1;
    if (heap % kBlock) return 0;
    if (!heap_readable(ctx, "cellSheapAllocate", heap)) return 0;
    u8* base = guest_base(ctx);
    lock_heap(base, heap);
    const u64 span = ps_read64(base, heap + heap_off::span);
    u32 addr = 0;
    if (size <= span) {
        const s32 order = std::countl_zero(std::max<u64>(size, kBlock) - 1) - std::countl_zero(span - 1);
        if (order >= 0) {
            if (const u32 node = tree_alloc(tree_of(base, heap), static_cast<u32>(order))) {
                const u32 level = 31 - static_cast<u32>(std::countl_zero(node));
                const u32 pos = node - (1u << level);
                const u64 first_block = ps_read64(base, heap + heap_off::first_block);
                addr = static_cast<u32>(pos * static_cast<u32>(span >> level) + first_block);
            }
        }
    }
    unlock_heap(base, heap);
    if (addr) PS_LOG_DEBUG(kChan, "cellSheapAllocate(heap {:#x}, size {:#x}) -> {:#x}", heap, size, addr);
    else PS_LOG_WARN(kChan, "cellSheapAllocate(heap {:#x}, size {:#x}): no free block", heap, size);
    return addr;
}

// cellSheapFree(heap, ptr), as libsre: ALIGN for a misaligned heap; INVAL for an address below the first block, not
// 128-byte aligned or past the tree (or in an entirely free heap); FAULT for a leaf that is split or full (leaves are
// only ever free or allocated, so a consistent bitmap never gives it). Any 128-byte step inside an allocation frees
// that allocation; an address in free space (a double free) is a libsre assertion.
s32 cellSheapFree(PPUContext& ctx, u32 heap, u32 ptr)
{
    if (heap % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (!heap_readable(ctx, "cellSheapFree", heap)) return error(CELL_SHEAP_ERROR_INVAL);
    u8* base = guest_base(ctx);
    lock_heap(base, heap);
    s32 rc = error(CELL_SHEAP_ERROR_INVAL);
    const s32 offset = static_cast<s32>(ptr - static_cast<u32>(ps_read64(base, heap + heap_off::first_block)));
    if (offset >= 0 && offset % kBlock == 0) {
        const u64 leaf = static_cast<u64>(offset) / kBlock + ps_read64(base, heap + heap_off::span) / kBlock;
        if (static_cast<u32>(leaf)) rc = tree_free(tree_of(base, heap), static_cast<u32>(leaf));
    }
    unlock_heap(base, heap);
    PS_LOG_DEBUG(kChan, "cellSheapFree(heap {:#x}, {:#x}) -> {:#x}", heap, ptr, static_cast<u32>(rc));
    return rc;
}

// The free space under the tree, as libsre adds it up (0x4940): a preorder walk that descends into split nodes and adds
// each free node's span; a node past the tree ends the walk.
u64 tree_free_bytes(const Tree& t, u64 span)
{
    u64 free = 0, size = span;
    u32 i = 1;
    while (i <= t.count) {
        const u32 st = t.get(i);
        if (st == NODE_SPLIT) {
            i *= 2;
            size >>= 1;
            continue;
        }
        if (st == NODE_FREE) free += size;
        while (i & 1) { // up while a right child
            i >>= 1;
            size <<= 1;
        }
        if (i <= 1) break;
        i++; // the right sibling
    }
    return free;
}

// The largest free block, as libsre finds it (0x48C4): the first free node in level order, its span (0 for none or an
// empty tree).
u64 tree_largest_free(const Tree& t, u64 span)
{
    for (u32 i = 1; i <= t.count; i++)
        if (t.get(i) == NODE_FREE) return span >> (31 - std::countl_zero(i));
    return 0;
}

// cellSheapQueryFree(heap) and cellSheapQueryMax(heap), as libsre (0x5334, 0x5910): ALIGN for a misaligned heap, then
// under the heap's lock the free bytes (or the largest free block), returned as the result itself (its low word).
s32 cellSheapQueryFree(PPUContext& ctx, u32 heap)
{
    if (heap % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (!heap_readable(ctx, "cellSheapQueryFree", heap)) return error(CELL_SHEAP_ERROR_INVAL);
    u8* base = guest_base(ctx);
    lock_heap(base, heap);
    const u64 free = tree_free_bytes(tree_of(base, heap), ps_read64(base, heap + heap_off::span));
    unlock_heap(base, heap);
    return static_cast<s32>(static_cast<u32>(free));
}

s32 cellSheapQueryMax(PPUContext& ctx, u32 heap)
{
    if (heap % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (!heap_readable(ctx, "cellSheapQueryMax", heap)) return error(CELL_SHEAP_ERROR_INVAL);
    u8* base = guest_base(ctx);
    lock_heap(base, heap);
    const u64 max = tree_largest_free(tree_of(base, heap), ps_read64(base, heap + heap_off::span));
    unlock_heap(base, heap);
    return static_cast<s32>(static_cast<u32>(max));
}

// ------------------------------------------------------------------------------------------------- key sheap
//
// cellKeySheap (libsre 4.92): a sheap whose first 2 KB after the header are a table of 256 keys, objects that the
// threads sharing the heap find by key. A key's entry is 8 bytes: a reference count, then the index + 1 of the object's
// first block (0 while it has none). The first thread to take a key (its count 0 -> 1, no block) makes the object and
// publishes its block; the others spin until it is there, then share it. The last to let go frees the block. The
// objects are cellSync's (a mutex, a semaphore word, a barrier, a reader-writer lock, a queue) or a plain buffer.
// cellKeySheapInitialize keeps the table's address at 0x48 (u64) and its size at 0x50.

constexpr u32 kKeys = 256, kKeyTable = kKeys * 8;

u32 key_entry(u8* base, u32 heap, u32 key) { return static_cast<u32>(ps_read64(base, heap + 0x48)) + key * 8; }

// One of the heap's blocks by its index + 1.
u32 key_block(u8* base, u32 heap, u32 index1)
{
    return static_cast<u32>(((index1 - 1u) << 7) + ps_read64(base, heap + heap_off::first_block));
}

// libsre's key acquire (0x5D44): ALIGN, INVAL past 255; then the count goes up once the entry is consistent (a block
// with no count is waited for); `ea` is the object's address, 0 for the first.
s32 key_acquire(u8* base, u32 heap, u32 key, u32& ea)
{
    if (heap % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (key >= kKeys) return error(CELL_SHEAP_ERROR_INVAL);
    const u32 e = key_entry(base, heap, key), line = e & ~(PS_RES_LINE_SIZE - 1), off = e - line;
    u32 index1 = 0;
    for (u64 spins = 0;; spins++) {
        // Through the reservation system: SPU code sharing the heap updates the entry with GETLLAR/PUTLLC.
        const bool taken = reservation_op(base, line, [&](u8* l) {
            const u32 count = ps_read32(l, off);
            index1 = ps_read32(l, off + 4);
            if (count ? index1 == 0 : index1 != 0) return false; // being made, or being given up: wait
            ps_write32(l, off, count + 1);
            return true;
        });
        if (taken) break;
        if (spins == (u64{1} << 22)) PS_LOG_WARN(kChan, "key sheap {:#x}: key {} still being made", heap, key);
        std::this_thread::yield();
    }
    ea = index1 ? key_block(base, heap, index1) : 0;
    return lv2::CELL_OK;
}

// libsre's key publish (0x5AD4): ALIGN, INVAL past 255; BUSY unless the caller holds the key (a count) and it has no
// block yet. `ea` 0 gives the key up (its count to 0, after a failed make); else the block, 128-aligned and inside the
// heap's blocks (INVAL), is published.
s32 key_set(u8* base, u32 heap, u32 key, u32 ea)
{
    if (heap % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (key >= kKeys) return error(CELL_SHEAP_ERROR_INVAL);
    const u32 e = key_entry(base, heap, key);
    if (!ps_read32(base, e) || ps_read32(base, e + 4)) return error(CELL_SHEAP_ERROR_BUSY);
    if (!ea) {
        ps_write32(base, e, 0);
        return lv2::CELL_OK;
    }
    const u64 first = ps_read64(base, heap + heap_off::first_block);
    if (ea % kBlock || ea < first || u64{ea} >= first + ps_read64(base, heap + heap_off::usable)) {
        return error(CELL_SHEAP_ERROR_INVAL);
    }
    ps_write32(base, e + 4, static_cast<u32>(((ea - first) >> 7) + 1));
    return lv2::CELL_OK;
}

// libsre's key release (0x5C88): INVAL (a misaligned heap too) or BUSY (no reference); the count goes down once the
// block is there; `ea` is the block when that was the last reference (the caller frees it), else 0.
s32 key_release(u8* base, u32 heap, u32 key, u32& ea)
{
    ea = 0;
    if (heap % kBlock || key >= kKeys) return error(CELL_SHEAP_ERROR_INVAL);
    const u32 e = key_entry(base, heap, key), line = e & ~(PS_RES_LINE_SIZE - 1), off = e - line;
    for (;;) {
        u32 count = 0, index1 = 0;
        bool busy = false;
        const bool done = reservation_op(base, line, [&](u8* l) {
            count = ps_read32(l, off);
            index1 = ps_read32(l, off + 4);
            busy = count == 0;
            if (busy || !index1) return false; // no reference; or being made: wait
            ps_write32(l, off, count - 1);
            return true;
        });
        if (busy) return error(CELL_SHEAP_ERROR_BUSY);
        if (done) {
            if (count == 1) ea = key_block(base, heap, index1);
            return lv2::CELL_OK;
        }
        std::this_thread::yield();
    }
}

// libsre's key clear (0x598C): INVAL, BUSY while referenced or without a block; then the block index is cleared.
s32 key_clear(u8* base, u32 heap, u32 key)
{
    if (heap % kBlock || key >= kKeys) return error(CELL_SHEAP_ERROR_INVAL);
    const u32 e = key_entry(base, heap, key);
    if (ps_read32(base, e) || !ps_read32(base, e + 4)) return error(CELL_SHEAP_ERROR_BUSY);
    ps_write32(base, e + 4, 0);
    return lv2::CELL_OK;
}

// cellKeySheapInitialize(ea, size, dmaTag), as libsre (0x59F4): ea NULL (INVAL), its 128-byte ALIGN, 2 KB <= size <=
// 512 GB and the tag at most 31 (INVAL); then the key table after the header (zeroed) and a sheap after the table, of
// the rest less the table, a multiple of 128 bytes.
s32 cellKeySheapInitialize(PPUContext& ctx, u32 ea, u64 size, u32 dma_tag)
{
    if (!ea) return error(CELL_SHEAP_ERROR_INVAL);
    if (ea % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    if (size < 0x800 || size > (u64{0x80} << 32) || dma_tag > kMaxDmaTag) return error(CELL_SHEAP_ERROR_INVAL);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!range_committed(t, ea, kHeaderSize + kKeyTable)) {
        log_call_site(ctx, base, std::format("cellKeySheapInitialize: {:#x} is not guest memory; refused with INVAL", ea));
        return error(CELL_SHEAP_ERROR_INVAL);
    }
    const u32 table = ea + kHeaderSize;
    ps_write64(base, ea + 0x50, kKeyTable);
    ps_write64(base, ea + 0x48, table);
    std::memset(base + table, 0, kKeyTable);
    // libsre's internal initialize (0x53B0) of the heap that follows: its header stays at ea.
    return cellSheapInitializeAt(ctx, ea, table + kKeyTable, (size - 0x880) & ~u64{kBlock - 1}, dma_tag);
}

// The handle every cellKeySheap*New fills: the heap (u64) at 0x00, the key at 0x08, the object (u64) at 0x10; a buffer's
// size (u64) at 0x18.
void fill_handle(u8* base, u32 handle, u32 heap, u32 key, u32 ea)
{
    ps_write64(base, handle + 0x00, heap);
    ps_write64(base, handle + 0x10, ea);
    ps_write32(base, handle + 0x08, key);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

// A cellSync function of the host's (registered in cellSync.cpp), as libsre calls its own.
s32 call_sync(PPUContext& ctx, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = registry().find("cellSync", nid);
    PS_CHECK(f != nullptr);
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    return static_cast<s32>(static_cast<u32>(ctx.gpr[3]));
}

// The common part of the New functions (libsre 0x5DFC..0x6C24): handle and heap non-null, the key at most 255 (INVAL);
// acquire the key; the first holder allocates `bytes`, makes the object (`make`, which may fail: the key is given up
// and the block freed) and publishes it; no block: the key is given up, SHORTAGE.
template <class Make>
s32 key_new(PPUContext& ctx, std::string_view what, u32 handle, u32 heap, u32 key, u64 bytes, Make&& make)
{
    if (!handle || !heap || key >= kKeys) return error(CELL_SHEAP_ERROR_INVAL);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!heap_readable(ctx, std::string(what).c_str(), heap) || !range_committed(t, handle, 0x20)) {
        return error(CELL_SHEAP_ERROR_INVAL);
    }
    u32 ea = 0;
    if (const s32 rc = key_acquire(base, heap, key, ea); rc != lv2::CELL_OK) return rc;
    if (!ea) {
        ea = cellSheapAllocate(ctx, heap, bytes);
        if (!ea) {
            key_set(base, heap, key, 0);
            return error(CELL_SHEAP_ERROR_SHORTAGE);
        }
        if (const s32 rc = make(ea); rc != lv2::CELL_OK) {
            key_set(base, heap, key, 0);
            cellSheapFree(ctx, heap, ea);
            return rc;
        }
        if (const s32 rc = key_set(base, heap, key, ea); rc != lv2::CELL_OK) return rc;
    }
    fill_handle(base, handle, heap, key, ea);
    return lv2::CELL_OK;
}

// The common part of the Delete functions: the handle's key released; the last holder frees the block and clears
// the key. Each step failing is a libsre assertion (it prints and ends the process).
s32 key_delete(PPUContext& ctx, std::string_view what, u32 handle)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!range_committed(t, handle, 0x10)) {
        PS_FATAL(kChan, "{}: handle {:#x} is not guest memory; libsre (fw 4.92) faults here", what, handle);
    }
    const u32 heap = static_cast<u32>(ps_read64(base, handle)), key = ps_read32(base, handle + 8);
    u32 ea = 0;
    if (const s32 rc = key_release(base, heap, key, ea); rc != lv2::CELL_OK) {
        PS_FATAL(kChan, "{}: releasing key {} of heap {:#x} failed ({:#x}); libsre (fw 4.92) asserts here and aborts the "
            "process", what, key, heap, static_cast<u32>(rc));
    }
    if (!ea) return lv2::CELL_OK;
    if (const s32 rc = cellSheapFree(ctx, heap, ea); rc != lv2::CELL_OK) {
        PS_FATAL(kChan, "{}: freeing {:#x} in heap {:#x} failed ({:#x}); libsre (fw 4.92) asserts here", what, ea, heap,
            static_cast<u32>(rc));
    }
    const s32 rc = key_clear(base, heap, key);
    if (rc != lv2::CELL_OK) {
        PS_FATAL(kChan, "{}: clearing key {} of heap {:#x} failed ({:#x}); libsre (fw 4.92) asserts here", what, key, heap,
            static_cast<u32>(rc));
    }
    return rc;
}

// cellKeySheapBufferNew(buffer, heap, key, size): libsre (0x5DFC) checks the heap's ALIGN first; `size` bytes, no
// object; the handle also gets the size (0x18).
s32 cellKeySheapBufferNew(PPUContext& ctx, u32 handle, u32 heap, u32 key, u64 size)
{
    if (heap % kBlock) return error(CELL_SHEAP_ERROR_ALIGN);
    const s32 rc = key_new(ctx, "cellKeySheapBufferNew", handle, heap, key, size, [](u32) { return lv2::CELL_OK; });
    if (rc == lv2::CELL_OK) ps_write64(guest_base(ctx), handle + 0x18, size);
    return rc;
}

s32 cellKeySheapBufferDelete(PPUContext& ctx, u32 handle) { return key_delete(ctx, "cellKeySheapBufferDelete", handle); }

// cellKeySheapMutexNew(mutex, heap, key): a CellSyncMutex (4 bytes; cellSyncMutexInitialize, whose failure libsre asserts).
s32 cellKeySheapMutexNew(PPUContext& ctx, u32 handle, u32 heap, u32 key)
{
    return key_new(ctx, "cellKeySheapMutexNew", handle, heap, key, 4, [&](u32 ea) {
        if (const s32 rc = call_sync(ctx, 0xA9072DEE, {ea}); rc != lv2::CELL_OK) {
            PS_FATAL(kChan, "cellKeySheapMutexNew: cellSyncMutexInitialize({:#x}) failed ({:#x}); libsre asserts here",
                ea, static_cast<u32>(rc));
        }
        return lv2::CELL_OK;
    });
}

s32 cellKeySheapMutexDelete(PPUContext& ctx, u32 handle) { return key_delete(ctx, "cellKeySheapMutexDelete", handle); }

// cellKeySheapSemaphoreNew(semaphore, heap, key, count): a word holding the count, stored atomically.
s32 cellKeySheapSemaphoreNew(PPUContext& ctx, u32 handle, u32 heap, u32 key, u32 count)
{
    return key_new(ctx, "cellKeySheapSemaphoreNew", handle, heap, key, 4, [&](u32 ea) {
        reservation_op(guest_base(ctx), ea & ~(PS_RES_LINE_SIZE - 1), [&](u8* l) { // libsre: lwarx/stwcx.
            ps_write32(l, ea & (PS_RES_LINE_SIZE - 1), count);
            return true;
        });
        return lv2::CELL_OK;
    });
}

s32 cellKeySheapSemaphoreDelete(PPUContext& ctx, u32 handle) { return key_delete(ctx, "cellKeySheapSemaphoreDelete", handle); }

// cellKeySheapBarrierNew(barrier, heap, key, total): a CellSyncBarrier (cellSyncBarrierInitialize with the total's low 16
// bits; its error is returned).
s32 cellKeySheapBarrierNew(PPUContext& ctx, u32 handle, u32 heap, u32 key, u32 total)
{
    return key_new(ctx, "cellKeySheapBarrierNew", handle, heap, key, 4,
        [&](u32 ea) { return call_sync(ctx, 0x07254FDA, {ea, total & 0xFFFF}); });
}

s32 cellKeySheapBarrierDelete(PPUContext& ctx, u32 handle) { return key_delete(ctx, "cellKeySheapBarrierDelete", handle); }

// cellKeySheapRwmNew(rwm, heap, key, size): a CellSyncRwm with its buffer of `size` bytes after it (128 + size).
s32 cellKeySheapRwmNew(PPUContext& ctx, u32 handle, u32 heap, u32 key, u32 size)
{
    return key_new(ctx, "cellKeySheapRwmNew", handle, heap, key, u64{size} + 128,
        [&](u32 ea) { return call_sync(ctx, 0xFC48B03F, {ea, ea + 128, size}); });
}

s32 cellKeySheapRwmDelete(PPUContext& ctx, u32 handle) { return key_delete(ctx, "cellKeySheapRwmDelete", handle); }

// cellKeySheapQueueNew(queue, heap, key, size, depth): a CellSyncQueue with its depth * size bytes after it.
s32 cellKeySheapQueueNew(PPUContext& ctx, u32 handle, u32 heap, u32 key, u32 size, u32 depth)
{
    return key_new(ctx, "cellKeySheapQueueNew", handle, heap, key, u64{size * depth} + 128,
        [&](u32 ea) { return call_sync(ctx, 0x3929948D, {ea, ea + 128, size, depth}); });
}

s32 cellKeySheapQueueDelete(PPUContext& ctx, u32 handle) { return key_delete(ctx, "cellKeySheapQueueDelete", handle); }

} // namespace

void register_cellSheap(Registry& r)
{
    r.add("cellSheap", {
        PS_HLE_FUNC(0xBBB47CD8, cellSheapInitialize),
        PS_HLE_FUNC(0x4B1383FB, cellSheapAllocate),
        PS_HLE_FUNC(0x5C5994BD, cellSheapFree),
        PS_HLE_FUNC(0x7FA23275, cellSheapQueryFree),
        PS_HLE_FUNC(0x37968718, cellSheapQueryMax),
        PS_HLE_FUNC(0xA1B25841, cellKeySheapInitialize),
        PS_HLE_FUNC(0x4A5B9659, cellKeySheapBufferNew),
        PS_HLE_FUNC(0xE6B37362, cellKeySheapBufferDelete),
        PS_HLE_FUNC(0x3478E1E6, cellKeySheapMutexNew),
        PS_HLE_FUNC(0x2452679F, cellKeySheapMutexDelete),
        PS_HLE_FUNC(0x69A5861D, cellKeySheapSemaphoreNew),
        PS_HLE_FUNC(0x73A45CF8, cellKeySheapSemaphoreDelete),
        PS_HLE_FUNC(0xE897C835, cellKeySheapBarrierNew),
        PS_HLE_FUNC(0xF6F5FBCA, cellKeySheapBarrierDelete),
        PS_HLE_FUNC(0xF01AC471, cellKeySheapRwmNew),
        PS_HLE_FUNC(0xED136702, cellKeySheapRwmDelete),
        PS_HLE_FUNC(0x987E260E, cellKeySheapQueueNew),
        PS_HLE_FUNC(0x79A6ABD0, cellKeySheapQueueDelete),
    });
}

} // namespace pspocket::hle
