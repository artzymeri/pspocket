// dlmalloc 2.8.3 (Doug Lea, public domain, CC0) as liblv2 builds it (lv2_heap.hpp, D-502): the mspace functions and
// what they use, in the order and with the names of the original, over guest memory. size_t is 32 bits, pointers are
// guest addresses; MALLOC_ALIGNMENT 16, FOOTERS, no mmap, no morecore, and RTCHECK failures and usage errors do nothing
// (liblv2's error actions are an empty function), so the function returns as dlmalloc's would after them.

#include "lv2_heap.hpp"

#include <bit>

#include "pspocket/core/be.h"

namespace pspocket::hle::lv2heap {
namespace {

constexpr u32 kSizeT = 4, kBits = 32;
constexpr u32 kAlignMask = 15;                // CHUNK_ALIGN_MASK
constexpr u32 kChunkOverhead = 8;             // CHUNK_OVERHEAD with FOOTERS
constexpr u32 kMinChunk = 16;                 // MIN_CHUNK_SIZE
constexpr u32 kMaxRequest = 0xFFFFFFC0;       // (-MIN_CHUNK_SIZE) << 2
constexpr u32 kMinRequest = kMinChunk - kChunkOverhead - 1;
constexpr u32 kPinuse = 1, kCinuse = 2, kInuseBits = 3;
constexpr u32 kFencepostHead = kInuseBits | kSizeT;
constexpr u32 kSmallShift = 3, kTreeShift = 8, kNSmall = 32, kNTree = 32;
constexpr u32 kMaxSmallRequest = 255 - kAlignMask - kChunkOverhead; // 232
constexpr u32 kExternBit = 8, kUseNoncontiguous = 4;

// malloc_state's fields.
constexpr u32 kSmallmap = 0, kTreemap = 4, kDvsize = 8, kTopsize = 12, kLeastAddr = 16, kDv = 20, kTop = 24,
              kTrimCheck = 28, kMagic = 32, kSmallbins = 36, kTreebins = 300, kFootprint = 428, kMaxFootprint = 432,
              kMflags = 436;
// malloc_segment's, at kStateSeg.
constexpr u32 kSegBase = 0, kSegSize = 4, kSegNext = 8, kSegFlags = 12;
// malloc_chunk's and malloc_tree_chunk's.
constexpr u32 kPrevFoot = 0, kHead = 4, kFd = 8, kBk = 12, kChild0 = 16, kChild1 = 20, kParent = 24, kIndex = 28;

constexpr u32 pad_request(u32 req) { return (req + kChunkOverhead + kAlignMask) & ~kAlignMask; }
constexpr u32 request2size(u32 req) { return req < kMinRequest ? kMinChunk : pad_request(req); }
constexpr u32 align_offset(u32 a) { return (a & kAlignMask) == 0 ? 0 : ((16 - (a & kAlignMask)) & kAlignMask); }
constexpr u32 chunk2mem(u32 p) { return p + 8; }
constexpr u32 mem2chunk(u32 mem) { return mem - 8; }
constexpr u32 align_as_chunk(u32 a) { return a + align_offset(chunk2mem(a)); }
constexpr bool is_small(u32 s) { return (s >> kSmallShift) < kNSmall; }
constexpr u32 small_index(u32 s) { return s >> kSmallShift; }
constexpr u32 small_index2size(u32 i) { return i << kSmallShift; }
constexpr u32 smallbin_at(u32 m, u32 i) { return m + kSmallbins + 8 * i; }
constexpr u32 treebin_at(u32 m, u32 i) { return m + kTreebins + 4 * i; }
constexpr u32 idx2bit(u32 i) { return 1u << i; }
constexpr u32 least_bit(u32 x) { return x & (0u - x); }
constexpr u32 left_bits(u32 x) { return (x << 1) | (0u - (x << 1)); }
constexpr u32 bit2idx(u32 x) { return static_cast<u32>(std::countr_zero(x)); }

constexpr u32 compute_tree_index(u32 s)
{
    const u32 x = s >> kTreeShift;
    if (x == 0) return 0;
    if (x > 0xFFFF) return kNTree - 1;
    const u32 k = 31 - static_cast<u32>(std::countl_zero(x));
    return (k << 1) + ((s >> (k + (kTreeShift - 1))) & 1);
}

constexpr u32 leftshift_for_tree_index(u32 i) { return i == kNTree - 1 ? 0 : (kBits - 1) - ((i >> 1) + kTreeShift - 2); }

} // namespace

u32 Space::r(u32 addr) const { return ps_read32(g_, addr); }
void Space::w(u32 addr, u32 value) { ps_write32(g_, addr, value); }

bool Space::ok_magic(u32 m) const { return m != 0 && r(m + kMagic) == mp_.magic; }
bool Space::ok_address(u32 m, u32 a) const { return a >= r(m + kLeastAddr); }

// ------------------------------------------------------------------------------------------------- chunk heads

void Space::mark_inuse_foot(u32 m, u32 p, u32 s) { w(p + s + kPrevFoot, m ^ mp_.magic); }

void Space::set_inuse(u32 m, u32 p, u32 s)
{
    w(p + kHead, (head(p) & kPinuse) | s | kCinuse);
    w(p + s + kHead, head(p + s) | kPinuse);
    mark_inuse_foot(m, p, s);
}

void Space::set_inuse_and_pinuse(u32 m, u32 p, u32 s)
{
    w(p + kHead, s | kPinuse | kCinuse);
    w(p + s + kHead, head(p + s) | kPinuse);
    mark_inuse_foot(m, p, s);
}

void Space::set_size_and_pinuse_of_inuse_chunk(u32 m, u32 p, u32 s)
{
    w(p + kHead, s | kPinuse | kCinuse);
    mark_inuse_foot(m, p, s);
}

void Space::set_size_and_pinuse_of_free_chunk(u32 p, u32 s)
{
    w(p + kHead, s | kPinuse);
    w(p + s + kPrevFoot, s); // set_foot
}

void Space::set_free_with_pinuse(u32 p, u32 s, u32 n)
{
    w(n + kHead, head(n) & ~kPinuse); // clear_pinuse
    set_size_and_pinuse_of_free_chunk(p, s);
}

// ------------------------------------------------------------------------------------------------- bins

void Space::init_bins(u32 m)
{
    for (u32 i = 0; i < kNSmall; ++i) {
        const u32 bin = smallbin_at(m, i);
        w(bin + kFd, bin);
        w(bin + kBk, bin);
    }
}

void Space::init_top(u32 m, u32 p, u32 psize)
{
    const u32 offset = align_offset(chunk2mem(p));
    p += offset;
    psize -= offset;
    w(m + kTop, p);
    w(m + kTopsize, psize);
    w(p + kHead, psize | kPinuse);
    w(p + psize + kHead, kTopFootSize);
    w(m + kTrimCheck, mp_.trim_threshold);
}

void Space::insert_small_chunk(u32 m, u32 p, u32 s)
{
    const u32 i = small_index(s);
    const u32 b = smallbin_at(m, i);
    u32 f = b;
    const u32 map = r(m + kSmallmap);
    if (!(map & idx2bit(i))) w(m + kSmallmap, map | idx2bit(i));
    else if (ok_address(m, r(b + kFd))) f = r(b + kFd);
    // else CORRUPTION_ERROR_ACTION, which does nothing, and the macro goes on with F = B
    w(b + kFd, p);
    w(f + kBk, p);
    w(p + kFd, f);
    w(p + kBk, b);
}

void Space::unlink_small_chunk(u32 m, u32 p, u32 s)
{
    const u32 f = r(p + kFd), b = r(p + kBk);
    const u32 i = small_index(s);
    if (f == b) {
        w(m + kSmallmap, r(m + kSmallmap) & ~idx2bit(i));
    } else if ((f == smallbin_at(m, i) || ok_address(m, f)) && (b == smallbin_at(m, i) || ok_address(m, b))) {
        w(f + kBk, b);
        w(b + kFd, f);
    }
}

void Space::unlink_first_small_chunk(u32 m, u32 b, u32 p, u32 i)
{
    const u32 f = r(p + kFd);
    if (b == f) {
        w(m + kSmallmap, r(m + kSmallmap) & ~idx2bit(i));
    } else if (ok_address(m, f)) {
        w(b + kFd, f);
        w(f + kBk, b);
    }
}

void Space::replace_dv(u32 m, u32 p, u32 s)
{
    const u32 dvs = r(m + kDvsize);
    if (dvs != 0) insert_small_chunk(m, r(m + kDv), dvs);
    w(m + kDvsize, s);
    w(m + kDv, p);
}

void Space::insert_large_chunk(u32 m, u32 x, u32 s)
{
    const u32 i = compute_tree_index(s);
    const u32 h = treebin_at(m, i);
    w(x + kIndex, i);
    w(x + kChild0, 0);
    w(x + kChild1, 0);
    const u32 map = r(m + kTreemap);
    if (!(map & idx2bit(i))) {
        w(m + kTreemap, map | idx2bit(i));
        w(h, x);
        w(x + kParent, h);
        w(x + kFd, x);
        w(x + kBk, x);
        return;
    }
    u32 t = r(h);
    u32 k = s << leftshift_for_tree_index(i);
    for (;;) {
        if (chunksize(t) != s) {
            const u32 c = t + kChild0 + 4 * ((k >> (kBits - 1)) & 1);
            k <<= 1;
            if (r(c) != 0) {
                t = r(c);
            } else if (ok_address(m, c)) {
                w(c, x);
                w(x + kParent, t);
                w(x + kFd, x);
                w(x + kBk, x);
                break;
            } else {
                break;
            }
        } else {
            const u32 f = r(t + kFd);
            if (ok_address(m, t) && ok_address(m, f)) {
                w(t + kFd, x);
                w(f + kBk, x);
                w(x + kFd, f);
                w(x + kBk, t);
                w(x + kParent, 0);
            }
            break;
        }
    }
}

void Space::unlink_large_chunk(u32 m, u32 x)
{
    const u32 xp = r(x + kParent);
    u32 rr = 0;
    if (r(x + kBk) != x) {
        const u32 f = r(x + kFd);
        rr = r(x + kBk);
        if (ok_address(m, f)) {
            w(f + kBk, rr);
            w(rr + kFd, f);
        }
    } else {
        u32 rp = x + kChild1;
        rr = r(rp);
        if (rr == 0) {
            rp = x + kChild0;
            rr = r(rp);
        }
        if (rr != 0) {
            for (;;) {
                u32 cp = rr + kChild1;
                if (r(cp) == 0) {
                    cp = rr + kChild0;
                    if (r(cp) == 0) break;
                }
                rp = cp;
                rr = r(rp);
            }
            if (ok_address(m, rp)) w(rp, 0);
        }
    }
    if (xp == 0) return;
    const u32 index = r(x + kIndex);
    const u32 h = treebin_at(m, index);
    if (x == r(h)) {
        w(h, rr);
        if (rr == 0) w(m + kTreemap, r(m + kTreemap) & ~idx2bit(index));
    } else if (ok_address(m, xp)) {
        if (r(xp + kChild0) == x) w(xp + kChild0, rr);
        else w(xp + kChild1, rr);
    }
    if (rr != 0 && ok_address(m, rr)) {
        w(rr + kParent, xp);
        if (const u32 c0 = r(x + kChild0); c0 != 0 && ok_address(m, c0)) {
            w(rr + kChild0, c0);
            w(c0 + kParent, rr);
        }
        if (const u32 c1 = r(x + kChild1); c1 != 0 && ok_address(m, c1)) {
            w(rr + kChild1, c1);
            w(c1 + kParent, rr);
        }
    }
}

void Space::insert_chunk(u32 m, u32 p, u32 s)
{
    if (is_small(s)) insert_small_chunk(m, p, s);
    else insert_large_chunk(m, p, s);
}

void Space::unlink_chunk(u32 m, u32 p, u32 s)
{
    if (is_small(s)) unlink_small_chunk(m, p, s);
    else unlink_large_chunk(m, p);
}

// ------------------------------------------------------------------------------------------------- malloc

u32 Space::tmalloc_small(u32 m, u32 nb)
{
    const u32 i = bit2idx(least_bit(r(m + kTreemap)));
    u32 v = r(treebin_at(m, i)), t = v;
    u32 rsize = chunksize(t) - nb;
    for (;;) {
        t = r(t + kChild0) != 0 ? r(t + kChild0) : r(t + kChild1); // leftmost_child
        if (t == 0) break;
        const u32 trem = chunksize(t) - nb;
        if (trem < rsize) {
            rsize = trem;
            v = t;
        }
    }
    if (ok_address(m, v)) {
        const u32 rem = v + nb;
        if (v < rem) { // ok_next
            unlink_large_chunk(m, v);
            if (rsize < kMinChunk) {
                set_inuse_and_pinuse(m, v, rsize + nb);
            } else {
                set_size_and_pinuse_of_inuse_chunk(m, v, nb);
                set_size_and_pinuse_of_free_chunk(rem, rsize);
                replace_dv(m, rem, rsize);
            }
            return chunk2mem(v);
        }
    }
    return 0;
}

u32 Space::tmalloc_large(u32 m, u32 nb)
{
    u32 v = 0;
    u32 rsize = 0u - nb;
    const u32 idx = compute_tree_index(nb);
    u32 t = r(treebin_at(m, idx));
    if (t != 0) {
        u32 sizebits = nb << leftshift_for_tree_index(idx);
        u32 rst = 0;
        for (;;) {
            const u32 trem = chunksize(t) - nb;
            if (trem < rsize) {
                v = t;
                rsize = trem;
                if (rsize == 0) break;
            }
            const u32 rt = r(t + kChild1);
            t = r(t + kChild0 + 4 * ((sizebits >> (kBits - 1)) & 1));
            if (rt != 0 && rt != t) rst = rt;
            if (t == 0) {
                t = rst;
                break;
            }
            sizebits <<= 1;
        }
    }
    if (t == 0 && v == 0) {
        const u32 leftbits = left_bits(idx2bit(idx)) & r(m + kTreemap);
        if (leftbits != 0) t = r(treebin_at(m, bit2idx(least_bit(leftbits))));
    }
    while (t != 0) {
        const u32 trem = chunksize(t) - nb;
        if (trem < rsize) {
            rsize = trem;
            v = t;
        }
        t = r(t + kChild0) != 0 ? r(t + kChild0) : r(t + kChild1);
    }
    if (v != 0 && rsize < r(m + kDvsize) - nb) {
        if (ok_address(m, v)) {
            const u32 rem = v + nb;
            if (v < rem) {
                unlink_large_chunk(m, v);
                if (rsize < kMinChunk) {
                    set_inuse_and_pinuse(m, v, rsize + nb);
                } else {
                    set_size_and_pinuse_of_inuse_chunk(m, v, nb);
                    set_size_and_pinuse_of_free_chunk(rem, rsize);
                    insert_chunk(m, rem, rsize);
                }
                return chunk2mem(v);
            }
        }
    }
    return 0;
}

// No mmap and no morecore: a heap that is full stays full.
u32 Space::sys_alloc(u32, u32) { return 0; }

u32 Space::malloc(u32 m, u32 bytes)
{
    if (!ok_magic(m)) return 0;
    u32 nb = 0;
    if (bytes <= kMaxSmallRequest) {
        nb = bytes < kMinRequest ? kMinChunk : pad_request(bytes);
        u32 idx = small_index(nb);
        const u32 smallbits = r(m + kSmallmap) >> idx;
        if ((smallbits & 0x3u) != 0) { // remainderless fit to a smallbin
            idx += ~smallbits & 1;
            const u32 b = smallbin_at(m, idx);
            const u32 p = r(b + kFd);
            unlink_first_small_chunk(m, b, p, idx);
            set_inuse_and_pinuse(m, p, small_index2size(idx));
            return chunk2mem(p);
        }
        if (nb > r(m + kDvsize)) {
            if (smallbits != 0) { // the chunk in the next nonempty smallbin
                const u32 leftbits = (smallbits << idx) & left_bits(idx2bit(idx));
                const u32 i = bit2idx(least_bit(leftbits));
                const u32 b = smallbin_at(m, i);
                const u32 p = r(b + kFd);
                unlink_first_small_chunk(m, b, p, i);
                const u32 rsize = small_index2size(i) - nb;
                set_size_and_pinuse_of_inuse_chunk(m, p, nb); // 4-byte sizes: never remainderless
                const u32 rem = p + nb;
                set_size_and_pinuse_of_free_chunk(rem, rsize);
                replace_dv(m, rem, rsize);
                return chunk2mem(p);
            }
            if (r(m + kTreemap) != 0) {
                if (const u32 mem = tmalloc_small(m, nb)) return mem;
            }
        }
    } else if (bytes >= kMaxRequest) {
        nb = 0xFFFFFFFF; // too big: sys_alloc fails it
    } else {
        nb = pad_request(bytes);
        if (r(m + kTreemap) != 0) {
            if (const u32 mem = tmalloc_large(m, nb)) return mem;
        }
    }

    if (nb <= r(m + kDvsize)) {
        const u32 rsize = r(m + kDvsize) - nb;
        const u32 p = r(m + kDv);
        if (rsize >= kMinChunk) { // split dv
            const u32 rem = p + nb;
            w(m + kDv, rem);
            w(m + kDvsize, rsize);
            set_size_and_pinuse_of_free_chunk(rem, rsize);
            set_size_and_pinuse_of_inuse_chunk(m, p, nb);
        } else { // exhaust dv
            const u32 dvs = r(m + kDvsize);
            w(m + kDvsize, 0);
            w(m + kDv, 0);
            set_inuse_and_pinuse(m, p, dvs);
        }
        return chunk2mem(p);
    }
    if (nb < r(m + kTopsize)) { // split top
        const u32 rsize = r(m + kTopsize) - nb;
        w(m + kTopsize, rsize);
        const u32 p = r(m + kTop);
        const u32 rem = p + nb;
        w(m + kTop, rem);
        w(rem + kHead, rsize | kPinuse);
        set_size_and_pinuse_of_inuse_chunk(m, p, nb);
        return chunk2mem(p);
    }
    return sys_alloc(m, nb);
}

// ------------------------------------------------------------------------------------------------- free

int Space::sys_trim(u32 m, u32 pad)
{
    // The top chunk lies in the heap's one segment, which is EXTERN: nothing is ever given back, so dlmalloc only stops
    // trying (released == 0).
    if (pad < kMaxRequest && r(m + kTop) != 0) w(m + kTrimCheck, 0xFFFFFFFF);
    return 0;
}

void Space::free(u32, u32 mem)
{
    if (mem == 0) return;
    u32 p = mem2chunk(mem);
    const u32 fm = r(p + chunksize(p) + kPrevFoot) ^ mp_.magic; // get_mstate_for
    if (!ok_magic(fm)) return;
    if (!(ok_address(fm, p) && (head(p) & kCinuse))) return;
    u32 psize = chunksize(p);
    const u32 next = p + psize;
    if (!(head(p) & kPinuse)) {
        const u32 prevsize = r(p + kPrevFoot); // no mmapped chunks here: its IS_MMAPPED_BIT is never set
        const u32 prev = p - prevsize;
        psize += prevsize;
        p = prev;
        if (!ok_address(fm, prev)) return;
        if (p != r(fm + kDv)) {
            unlink_chunk(fm, p, prevsize);
        } else if ((head(next) & kInuseBits) == kInuseBits) {
            w(fm + kDvsize, psize);
            set_free_with_pinuse(p, psize, next);
            return;
        }
    }
    if (!(p < next && (head(next) & kPinuse))) return;
    if (!(head(next) & kCinuse)) { // consolidate forward
        if (next == r(fm + kTop)) {
            const u32 tsize = r(fm + kTopsize) + psize;
            w(fm + kTopsize, tsize);
            w(fm + kTop, p);
            w(p + kHead, tsize | kPinuse);
            if (p == r(fm + kDv)) {
                w(fm + kDv, 0);
                w(fm + kDvsize, 0);
            }
            if (tsize > r(fm + kTrimCheck)) sys_trim(fm, 0); // should_trim
            return;
        }
        if (next == r(fm + kDv)) {
            const u32 dsize = r(fm + kDvsize) + psize;
            w(fm + kDvsize, dsize);
            w(fm + kDv, p);
            set_size_and_pinuse_of_free_chunk(p, dsize);
            return;
        }
        const u32 nsize = chunksize(next);
        psize += nsize;
        unlink_chunk(fm, next, nsize);
        set_size_and_pinuse_of_free_chunk(p, psize);
        if (p == r(fm + kDv)) {
            w(fm + kDvsize, psize);
            return;
        }
    } else {
        set_free_with_pinuse(p, psize, next);
    }
    insert_chunk(fm, p, psize);
}

// ------------------------------------------------------------------------------------------------- memalign

u32 Space::memalign(u32 m, u32 alignment, u32 bytes)
{
    if (!ok_magic(m)) return 0;
    if (alignment <= 16) return malloc(m, bytes);
    if (alignment < kMinChunk) alignment = kMinChunk;
    if ((alignment & (alignment - 1)) != 0) {
        u32 a = 16u << 1;
        while (a < alignment) a <<= 1;
        alignment = a;
    }
    if (bytes >= kMaxRequest - alignment) return 0;
    const u32 nb = request2size(bytes);
    const u32 req = nb + alignment + kMinChunk - kChunkOverhead;
    const u32 mem = malloc(m, req);
    if (mem == 0) return 0;
    u32 leader = 0, trailer = 0;
    u32 p = mem2chunk(mem);
    if (mem % alignment != 0) { // misaligned: an aligned spot inside, its leader given back
        const u32 br = mem2chunk((mem + alignment - 1) & (0u - alignment));
        const u32 pos = br - p >= kMinChunk ? br : br + alignment;
        const u32 newp = pos;
        const u32 leadsize = pos - p;
        const u32 newsize = chunksize(p) - leadsize;
        set_inuse(m, newp, newsize);
        set_inuse(m, p, leadsize);
        leader = chunk2mem(p);
        p = newp;
    }
    const u32 size = chunksize(p);
    if (size > nb + kMinChunk) { // spare room at the end given back
        const u32 remainder_size = size - nb;
        const u32 remainder = p + nb;
        set_inuse(m, p, nb);
        set_inuse(m, remainder, remainder_size);
        trailer = chunk2mem(remainder);
    }
    if (leader != 0) free(m, leader);
    if (trailer != 0) free(m, trailer);
    return chunk2mem(p);
}

// ------------------------------------------------------------------------------------------------- the heap

u32 Space::create_with_base(u32 base, u32 capacity)
{
    const u32 msize = kStateSize;
    if (!(capacity > msize + kTopFootSize && capacity < 0u - (msize + kTopFootSize + mp_.page_size))) return 0;
    // init_user_mstate
    const u32 msp = align_as_chunk(base);
    const u32 m = chunk2mem(msp);
    for (u32 i = 0; i < msize; i += 4) w(m + i, 0);
    w(msp + kHead, msize | kPinuse | kCinuse);
    w(m + kStateSeg + kSegBase, base);
    w(m + kLeastAddr, base);
    w(m + kStateSeg + kSegSize, capacity);
    w(m + kFootprint, capacity);
    w(m + kMaxFootprint, capacity);
    w(m + kMagic, mp_.magic);
    w(m + kMflags, mp_.default_mflags | kUseNoncontiguous); // disable_contiguous
    init_bins(m);
    const u32 mn = msp + chunksize(msp); // next_chunk(mem2chunk(m))
    init_top(m, mn, (base + capacity) - mn - kTopFootSize);
    w(m + kStateSeg + kSegFlags, kExternBit);
    return m;
}

u32 Space::destroy(u32) { return 0; }

MallInfo Space::mallinfo(u32 m)
{
    MallInfo nm;
    if (!ok_magic(m) || r(m + kTop) == 0) return nm;
    const u32 top = r(m + kTop);
    u32 nfree = 1; // top always free
    u32 mfree = r(m + kTopsize) + kTopFootSize;
    u32 sum = mfree;
    for (u32 s = m + kStateSeg; s != 0; s = r(s + kSegNext)) {
        const u32 sbase = r(s + kSegBase), ssize = r(s + kSegSize);
        u32 q = align_as_chunk(sbase);
        while (q >= sbase && q < sbase + ssize && q != top && head(q) != kFencepostHead) {
            const u32 sz = chunksize(q);
            sum += sz;
            if (!(head(q) & kCinuse)) {
                mfree += sz;
                ++nfree;
            }
            q += sz;
            if (sz == 0) break; // a zero size would loop forever (only in a corrupt heap)
        }
    }
    nm.arena = sum;
    nm.ordblks = nfree;
    nm.hblkhd = r(m + kFootprint) - sum;
    nm.usmblks = r(m + kMaxFootprint);
    nm.uordblks = r(m + kFootprint) - mfree;
    nm.fordblks = mfree;
    nm.keepcost = r(m + kTopsize);
    return nm;
}

void Space::malloc_stats(u32 m)
{
    if (!ok_magic(m) || r(m + kTop) == 0) return;
    const u32 top = r(m + kTop);
    for (u32 s = m + kStateSeg; s != 0; s = r(s + kSegNext)) {
        const u32 sbase = r(s + kSegBase), ssize = r(s + kSegSize);
        for (u32 q = align_as_chunk(sbase); q >= sbase && q < sbase + ssize && q != top && head(q) != kFencepostHead;) {
            const u32 sz = chunksize(q);
            if (sz == 0) break;
            q += sz;
        }
    }
}

} // namespace pspocket::hle::lv2heap
