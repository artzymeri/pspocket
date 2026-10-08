#pragma once

// liblv2's heap allocator (D-502): dlmalloc 2.8.3 (Doug Lea, public domain) as liblv2.sprx builds it, read from the
// firmware's module and compared with it call for call (tests/hle/test_lv2_compare.cpp). Its configuration: MSPACES,
// FOOTERS, MALLOC_ALIGNMENT 16, no mmap and no morecore (a heap never grows past the memory it was made in), errors
// ignored (liblv2's USAGE_ERROR_ACTION and CORRUPTION_ERROR_ACTION call an empty function), and three fields of
// liblv2's own in the malloc_state: the heap's lwmutex, its flags and its name. A heap lives in guest memory as
// liblv2's does: the malloc_state at the start of its memory and every chunk's header and footer between the blocks,
// in big-endian 32-bit words at the same places, so the blocks handed out and the numbers reported are liblv2's.

#include "pspocket/core/types.hpp"

namespace pspocket::hle::lv2heap {

// dlmalloc's mparams, one set per process (liblv2's init_mparams: the magic from the timebase).
struct Params {
    u32 magic = 0;
    u32 page_size = 0x1000;
    u32 granularity = 0x10000;
    u32 mmap_threshold = 0xFFFFFFFF;
    u32 trim_threshold = 0x200000;
    u32 default_mflags = 4; // USE_NONCONTIGUOUS_BIT
};

// struct mallinfo, ten size_t (liblv2's _sys_heap_get_mallinfo copies them as they are).
struct MallInfo {
    u32 arena = 0, ordblks = 0, smblks = 0, hblks = 0, hblkhd = 0, usmblks = 0, fsmblks = 0, uordblks = 0,
        fordblks = 0, keepcost = 0;
};

// The malloc_state's fields of liblv2's own: the lwmutex at +440 (24 bytes), the flags at +464, the name at +468
// (31 bytes and a NUL); the segment at +500.
inline constexpr u32 kStateLock = 440, kStateFlags = 464, kStateName = 468, kStateNameMax = 31, kStateSeg = 500;
inline constexpr u32 kStateSize = 528; // pad_request(sizeof(struct malloc_state)), liblv2's msize
inline constexpr u32 kTopFootSize = 56;

// An mspace in guest memory (the u8* is the guest base). Every function is dlmalloc's of the same name; none locks
// (liblv2 holds the heap's lwmutex around each, its callers here a host mutex).
class Space {
public:
    Space(u8* guest, const Params& params) : g_(guest), mp_(params) {}

    // The mspace (the address of its malloc_state, which liblv2 hands out as the heap's ID), or 0 when `capacity` does
    // not hold one.
    u32 create_with_base(u32 base, u32 capacity);
    // The bytes destroy_mspace frees: none, the memory being the caller's (EXTERN_BIT).
    u32 destroy(u32 m);
    u32 malloc(u32 m, u32 bytes);
    // As FOOTERS has it: the mspace comes from the chunk's footer, not from `m`.
    void free(u32 m, u32 mem);
    u32 memalign(u32 m, u32 alignment, u32 bytes);
    MallInfo mallinfo(u32 m);
    // internal_malloc_stats with liblv2's printing left out: a walk over every chunk, nothing reported.
    void malloc_stats(u32 m);
    bool ok_magic(u32 m) const;

private:
    u32 r(u32 addr) const;
    void w(u32 addr, u32 value);
    u32 head(u32 p) const { return r(p + 4); }
    u32 chunksize(u32 p) const { return head(p) & ~3u; }

    u32 sys_alloc(u32 m, u32 nb);
    int sys_trim(u32 m, u32 pad);
    u32 tmalloc_small(u32 m, u32 nb);
    u32 tmalloc_large(u32 m, u32 nb);
    void insert_small_chunk(u32 m, u32 p, u32 s);
    void unlink_small_chunk(u32 m, u32 p, u32 s);
    void unlink_first_small_chunk(u32 m, u32 b, u32 p, u32 i);
    void replace_dv(u32 m, u32 p, u32 s);
    void insert_large_chunk(u32 m, u32 x, u32 s);
    void unlink_large_chunk(u32 m, u32 x);
    void insert_chunk(u32 m, u32 p, u32 s);
    void unlink_chunk(u32 m, u32 p, u32 s);
    void init_bins(u32 m);
    void init_top(u32 m, u32 p, u32 psize);
    void set_inuse(u32 m, u32 p, u32 s);
    void set_inuse_and_pinuse(u32 m, u32 p, u32 s);
    void set_size_and_pinuse_of_inuse_chunk(u32 m, u32 p, u32 s);
    void set_size_and_pinuse_of_free_chunk(u32 p, u32 s);
    void set_free_with_pinuse(u32 p, u32 s, u32 n);
    void mark_inuse_foot(u32 m, u32 p, u32 s);
    bool ok_address(u32 m, u32 a) const;

    u8* g_;
    const Params& mp_;
};

} // namespace pspocket::hle::lv2heap
