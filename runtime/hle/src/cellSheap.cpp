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
    const u64 area_size = (size - kHeaderSize) & ~u64{kBlock - 1};
    const u32 area = ea + kHeaderSize;

    // The bitmap's size is computed for a tree over the whole area, the tree itself over what the bitmap leaves.
    const u64 units = area_size / kBlock;
    const u64 nodes = units + pow2_at_least(units) - 1;
    if (nodes > 0xFFFFFFFF) return error(CELL_SHEAP_ERROR_INVAL);
    const u64 bitmap_size = ((u64{static_cast<u32>(2 * nodes + 2)} + 7) / 8 + kBlock - 1) & ~u64{kBlock - 1};
    const u64 usable = area_size - bitmap_size;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!range_committed(t, ea, kHeaderSize + bitmap_size)) {
        log_call_site(ctx, base, std::format("cellSheapInitialize: the heap header or bitmap at {:#x} ({:#x} bytes) is "
            "not committed guest memory; refused with CELL_SHEAP_ERROR_INVAL", ea, kHeaderSize + bitmap_size));
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

} // namespace

void register_cellSheap(Registry& r)
{
    r.add("cellSheap", {
        PS_HLE_FUNC(0xBBB47CD8, cellSheapInitialize),
        PS_HLE_FUNC(0x4B1383FB, cellSheapAllocate),
        PS_HLE_FUNC(0x5C5994BD, cellSheapFree),
    });
}

} // namespace pspocket::hle
