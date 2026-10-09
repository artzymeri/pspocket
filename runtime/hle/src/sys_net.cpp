// sys_net (libnet): BSD sockets, address text conversions, the resolver and libnet's own helpers, as Sony's
// libnet.sprx does them (D-523), over pspocket's lv2 network calls (runtime/lv2/src/net.cpp: a console whose network
// cable is unplugged), compared with libnet running on that lv2 in tests/hle/test_net_compare.cpp. RPCS3 runs Sony's
// libnet (its sys_net_.cpp is mostly todo stubs); the behaviour here is the module's, read from its code.
//
// - sys_net_initialize_network_ex(param {memory, size, flags}) hands libnet a memory pool (at least 8 KB): libnet's
//   allocator runs over it (__sys_net_lib_malloc and the rest: first fit, 20-byte block headers {next, prev, tag,
//   line, size}, a free and a used list in address order, neighbours merged on free) and keeps each thread's context
//   in it (3192 bytes: sys_net_errno at +2928, h_errno at +2900, the thread's sync object, resolver state), made at the
//   thread's first call and freed at its exit (an atexit function) or by sys_net_free_thread_context. A second
//   initialisation is CELL_OK, or SYS_NET_ERROR_EBUSY with flag 1.
// - Every call that goes to lv2 runs through the thread's context: before sys_net_initialize_network_ex (and when a
//   context can't be made) the context is libnet's static one, marked aborted with EBUSY (ENOMEM), so the call answers
//   SYS_NET_ERROR_EBUSY without reaching lv2. A failure stores the errno in the context and answers
//   SYS_NET_ERROR_* (0x80010200 | errno), not -1.
// - libnet's random numbers are a 521-word lagged XOR generator in the pool, seeded from the clock.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_net";

// ------------------------------------------------------------------------------------------------- constants

enum : u32 {
    SYS_NET_ENOENT = 2,
    SYS_NET_EBADF = 9,
    SYS_NET_ENOMEM = 12,
    SYS_NET_EFAULT = 14,
    SYS_NET_EBUSY = 16,
    SYS_NET_EINVAL = 22,
    SYS_NET_ENOSPC = 28,
    SYS_NET_EAFNOSUPPORT = 47,
};
constexpr u32 SYS_NET_ERROR = 0x80010200; // | errno

constexpr s32 SYS_NET_AF_INET = 2, SYS_NET_AF_INET6 = 24;
constexpr u32 kInaddrNone = 0xFFFFFFFF;

// The thread context (libnet's per-thread state, 3192 bytes in the pool).
constexpr u32 kCtxSize = 3192;
constexpr u32 kCtxNext = 0, kCtxPrev = 4, kCtxTid = 8, kCtxSync = 16;
constexpr u32 kCtxResolverRetry = 2440, kCtxResolverRetrans = 2444, kCtx2448 = 2448, kCtx2452 = 2452;
constexpr u32 kCtx2884 = 2884;
constexpr u32 kCtxHErrno = 2900, kCtxAborted = 2924, kCtxErrno = 2928, kCtxInCall = 3188;

// A block header of libnet's allocator.
constexpr u32 kHdrNext = 0, kHdrPrev = 4, kHdrTag = 8, kHdrLine = 12, kHdrSize = 16, kHdr = 20;

// The random number generator's state (in the pool).
constexpr u32 kRandWords = 521, kRandSize = 4 * kRandWords;

// libnet's own guest data here: the static context, then the allocation tags (its source file names).
constexpr u32 kDataSize = 0x1000;
constexpr u32 kTagMisc = kCtxSize;           // "misc.c"
constexpr u32 kTagGetifaddrs = kCtxSize + 8; // "getifaddrs.c"

// ------------------------------------------------------------------------------------------------- libnet's state

struct Libnet {
    std::mutex pool_m, ctx_m, rand_m; // libnet's sync objects for its pool, its context list and its generator
    bool initialized = false;
    // The pool (libnet keeps its lists across a finalisation: a later initialisation starts a new free list).
    u32 min_free = 0, free = 0;
    u32 free_head = 0, free_tail = 0, start = 0, end = 0, used_head = 0, used_tail = 0;
    // The context list, most recently used first.
    u32 ctx_head = 0, ctx_tail = 0;
    u32 rand_state = 0, rand_index = 0;
    u32 data = 0; // libnet's own guest data (kDataSize)
};

Libnet& libnet() { return HleProcess::current()->state<Libnet>(); }

u8* gb(PPUContext& ctx) { return guest_base(ctx); }
u32 r32(u8* b, u32 a) { return ps_read32(b, a); }
void w32(u8* b, u32 a, u32 v) { ps_write32(b, a, v); }

void set_errno(PPUContext& ctx, u32 e);

// A pointer argument checked before use (D-042): libnet would fault on one that is not guest memory; here the call
// site is logged and the call refused with errno EFAULT.
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, gb(ctx), std::format("{}: {:#x} ({} bytes) is not guest memory", what, addr, size));
    set_errno(ctx, 14); // SYS_NET_EFAULT
    return false;
}

// libnet's guest data, made at first use.
u32 data(PPUContext& ctx)
{
    Libnet& n = libnet();
    if (!n.data) {
        n.data = HleProcess::current()->main_memory().alloc(kDataSize, 0x80);
        if (!n.data) PS_FATAL(kChan, "no guest memory for libnet's data");
        std::memcpy(gb(ctx) + n.data + kTagMisc, "misc.c", 7);
        std::memcpy(gb(ctx) + n.data + kTagGetifaddrs, "getifaddrs.c", 13);
    }
    return n.data;
}

// ------------------------------------------------------------------------------------------------- the pool

// libnet's allocator over the pool (its pool lock held): first fit in the free list (address order); a block that
// leaves more than a header's room is split, the front given. The given block goes into the used list before the
// block that follows it in memory.
u32 pool_malloc(u8* b, Libnet& n, u32 size, u32 tag, u32 line)
{
    const s32 need = static_cast<s32>((size + 3) & ~3u);
    u32 blk = n.free_head;
    while (blk && need > static_cast<s32>(r32(b, blk + kHdrSize))) blk = r32(b, blk + kHdrNext);
    if (!blk) return 0;
    const s32 rest = static_cast<s32>(r32(b, blk + kHdrSize)) - need - static_cast<s32>(kHdr);
    u32 after = 0; // the block that follows the given one's region in memory (the rest, or the next)
    if (rest > 0) {
        const u32 split = blk + kHdr + static_cast<u32>(need);
        w32(b, blk + kHdrSize, static_cast<u32>(need));
        const u32 next = r32(b, blk + kHdrNext), prev = r32(b, blk + kHdrPrev);
        w32(b, split + kHdrSize, static_cast<u32>(rest));
        w32(b, split + kHdrNext, next);
        if (next) w32(b, next + kHdrPrev, split);
        else n.free_tail = split;
        w32(b, split + kHdrPrev, prev);
        if (prev) w32(b, prev + kHdrNext, split);
        else n.free_head = split;
        after = split;
    } else {
        const u32 next = r32(b, blk + kHdrNext), prev = r32(b, blk + kHdrPrev);
        if (next) w32(b, next + kHdrPrev, prev);
        else n.free_tail = prev;
        if (prev) w32(b, prev + kHdrNext, next);
        else n.free_head = next;
        after = blk;
    }
    if (!n.used_head) {
        w32(b, blk + kHdrNext, 0);
        w32(b, blk + kHdrPrev, 0);
        n.used_head = blk;
        n.used_tail = blk;
    } else {
        const u32 follow = after + kHdr + r32(b, after + kHdrSize);
        if (follow < n.end) { // a used block: the given one goes before it
            const u32 prev = r32(b, follow + kHdrPrev);
            w32(b, blk + kHdrPrev, prev);
            if (prev) w32(b, prev + kHdrNext, blk);
            else n.used_head = blk;
            w32(b, follow + kHdrPrev, blk);
            w32(b, blk + kHdrNext, follow);
        } else {
            const u32 tail = n.used_tail;
            w32(b, blk + kHdrNext, 0);
            w32(b, blk + kHdrPrev, tail);
            w32(b, tail + kHdrNext, blk);
            n.used_tail = blk;
        }
    }
    const s32 left = static_cast<s32>(n.free) - static_cast<s32>(r32(b, blk + kHdrSize)) - static_cast<s32>(kHdr);
    w32(b, blk + kHdrTag, tag);
    n.free = static_cast<u32>(left);
    w32(b, blk + kHdrLine, line);
    if (left < static_cast<s32>(n.min_free)) n.min_free = static_cast<u32>(left);
    return blk + kHdr;
}

// libnet's free (its pool lock held): out of the used list, into the free list before the next free block in memory,
// merged with its neighbours there.
void pool_free(u8* b, Libnet& n, u32 ptr)
{
    u32 blk = ptr - kHdr;
    {
        const u32 next = r32(b, blk + kHdrNext), prev = r32(b, blk + kHdrPrev);
        if (next) w32(b, next + kHdrPrev, prev);
        else n.used_tail = prev;
        if (prev) w32(b, prev + kHdrNext, next);
        else n.used_head = next;
        n.free += r32(b, blk + kHdrSize) + kHdr;
    }
    // The first free block after it: past the run of used blocks that follow it in memory and in the used list.
    u32 at = blk, follow = 0;
    for (;;) {
        follow = at + kHdr + r32(b, at + kHdrSize);
        if (r32(b, at + kHdrNext) != follow) break;
        at = follow;
    }
    if (follow >= n.end) {
        const u32 tail = n.free_tail;
        w32(b, blk + kHdrNext, 0);
        w32(b, blk + kHdrPrev, tail);
        if (tail) w32(b, tail + kHdrNext, blk);
        else n.free_head = blk;
        n.free_tail = blk;
    } else {
        const u32 prev = r32(b, follow + kHdrPrev);
        w32(b, blk + kHdrPrev, prev);
        if (prev) w32(b, prev + kHdrNext, blk);
        else n.free_head = blk;
        w32(b, blk + kHdrNext, follow);
        w32(b, follow + kHdrPrev, blk);
    }
    // Merged into the previous free block when they touch, then with the next.
    if (const u32 prev = r32(b, blk + kHdrPrev); prev && prev + kHdr + r32(b, prev + kHdrSize) == blk) {
        const u32 next = r32(b, blk + kHdrNext);
        w32(b, prev + kHdrSize, r32(b, prev + kHdrSize) + r32(b, blk + kHdrSize) + kHdr);
        w32(b, prev + kHdrNext, next);
        if (next) w32(b, next + kHdrPrev, prev);
        else n.free_tail = prev;
        blk = prev;
    }
    if (const u32 next = r32(b, blk + kHdrNext); next && blk + kHdr + r32(b, blk + kHdrSize) == next) {
        const u32 after = r32(b, next + kHdrNext);
        w32(b, blk + kHdrNext, after);
        w32(b, blk + kHdrSize, r32(b, blk + kHdrSize) + r32(b, next + kHdrSize) + kHdr);
        if (after) w32(b, after + kHdrPrev, blk);
        else n.free_tail = blk;
    }
}

// The pool's lists only reach guest memory (a game that wrote over a block header would make libnet fault; here the
// allocation is refused and logged).
bool pool_intact(PPUContext& ctx, const Libnet& n, u32 extra)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* b = gb(ctx);
    for (const u32 head : {n.free_head, n.used_head, extra}) {
        for (u32 blk = head, guard = 0; blk; blk = r32(b, blk + kHdrNext), guard++) {
            if (guard > 0x100000 || !guest_readable(t, blk, kHdr)) {
                log_call_site(ctx, b, std::format("libnet's pool: a block at {:#x} is not guest memory", blk));
                return false;
            }
            if (head == extra) break;
        }
    }
    return true;
}

u32 net_malloc(PPUContext& ctx, u32 size, u32 tag, u32 line)
{
    Libnet& n = libnet();
    std::lock_guard lock(n.pool_m);
    if (!pool_intact(ctx, n, 0)) return 0;
    return pool_malloc(gb(ctx), n, size, tag, line);
}

void net_free(PPUContext& ctx, u32 ptr)
{
    if (!ptr) return;
    Libnet& n = libnet();
    std::lock_guard lock(n.pool_m);
    if (!pool_intact(ctx, n, ptr - kHdr)) return;
    pool_free(gb(ctx), n, ptr);
}

// ------------------------------------------------------------------------------------------------- sync objects

// libnet's sync object (32 bytes in guest memory): {lv2 mutex, lv2 condition, timeout, pad, u64 events, u64 waiters}.
// The mutex and the condition are named "_netsyc".
s32 sync_create(PPUContext& ctx, u32 sync, u64 events)
{
    u8* b = gb(ctx);
    StackVar attr(ctx, b, 0x40); // sys_mutex_attribute_t, then sys_cond_attribute_t at +0x28
    const u32 m = attr.addr(), c = attr.addr() + 0x28;
    w32(b, m, 2);           // SYS_SYNC_PRIORITY
    w32(b, m + 4, 0x20);    // SYS_SYNC_NOT_RECURSIVE
    w32(b, m + 8, 0x200);   // SYS_SYNC_NOT_PROCESS_SHARED
    w32(b, m + 12, 0x2000); // SYS_SYNC_NOT_ADAPTIVE
    std::memcpy(b + m + 0x20, "_netsyc", 8);
    s32 r = static_cast<s32>(lv2_syscall(ctx, 100, {sync, m}));
    if (r < 0) return r;
    w32(b, c, 0x200);
    std::memcpy(b + c + 0x10, "_netsyc", 8);
    r = static_cast<s32>(lv2_syscall(ctx, 105, {sync + 4, r32(b, sync), c}));
    if (r < 0) {
        lv2_syscall(ctx, 101, {r32(b, sync)});
        return r;
    }
    ps_write64(b, sync + 16, events);
    w32(b, sync + 8, 0);
    ps_write64(b, sync + 24, 0);
    return 0;
}

s32 sync_destroy(PPUContext& ctx, u32 sync)
{
    u8* b = gb(ctx);
    lv2_syscall(ctx, 106, {r32(b, sync + 4)});
    lv2_syscall(ctx, 101, {r32(b, sync)});
    return 0;
}

// The events or'ed in; the waiters (if any) woken.
s32 sync_signal(PPUContext& ctx, u32 sync, u64 events)
{
    u8* b = gb(ctx);
    u64 old = ps_read64(b, sync + 16);
    while (!atomic_cas_be64(b, sync + 16, old, old | events)) old = ps_read64(b, sync + 16);
    if (!ps_read64(b, sync + 24)) return 0;
    s32 r = static_cast<s32>(lv2_syscall(ctx, 102, {r32(b, sync), 0}));
    if (r < 0) return r;
    if (ps_read64(b, sync + 16)) {
        r = static_cast<s32>(lv2_syscall(ctx, 108, {r32(b, sync + 4)}));
        if (r != 0) {
            lv2_syscall(ctx, 104, {r32(b, sync)});
            return r;
        }
    }
    return static_cast<s32>(lv2_syscall(ctx, 104, {r32(b, sync)}));
}

// Waits until events are set, takes them all (into *out when given).
s32 sync_wait(PPUContext& ctx, u32 sync, u32 out)
{
    u8* b = gb(ctx);
    atomic_add_be64(b, sync + 24, 1);
    for (;;) {
        u64 got = ps_read64(b, sync + 16);
        while (!atomic_cas_be64(b, sync + 16, got, 0)) got = ps_read64(b, sync + 16);
        if (got) {
            if (out) ps_write64(b, out, got);
            atomic_add_be64(b, sync + 24, ~u64{0});
            return 0;
        }
        s32 r = static_cast<s32>(lv2_syscall(ctx, 102, {r32(b, sync), 0}));
        if (r < 0) {
            atomic_add_be64(b, sync + 24, ~u64{0});
            return r;
        }
        if (!ps_read64(b, sync + 16)) {
            r = static_cast<s32>(lv2_syscall(ctx, 107,
                {r32(b, sync + 4), static_cast<u64>(static_cast<s64>(static_cast<s32>(r32(b, sync + 8))))}));
            if (r < 0) {
                lv2_syscall(ctx, 104, {r32(b, sync)});
                atomic_add_be64(b, sync + 24, ~u64{0});
                return r;
            }
        }
        r = static_cast<s32>(lv2_syscall(ctx, 104, {r32(b, sync)}));
        if (r < 0) {
            atomic_add_be64(b, sync + 24, ~u64{0});
            return r;
        }
    }
}

// ------------------------------------------------------------------------------------------------- thread contexts

// libnet's static context, marked aborted with `error` (EBUSY before initialisation): calls answer it.
u32 static_context(PPUContext& ctx, u32 error)
{
    const u32 c = data(ctx);
    w32(gb(ctx), c + kCtxAborted, 1);
    w32(gb(ctx), c + kCtxErrno, error);
    return c;
}

// The calling thread's context (libnet f_d870): found by thread ID in the list (and moved to its front), else made
// in the pool and put at the front.
u32 context(PPUContext& ctx)
{
    Libnet& n = libnet();
    if (!n.initialized) return static_context(ctx, SYS_NET_EBUSY);
    u8* b = gb(ctx);
    const u64 tid = thread_of(ctx).id;
    const auto to_front = [&](u32 c) {
        w32(b, c + kCtxNext, n.ctx_head);
        if (n.ctx_head) w32(b, n.ctx_head + kCtxPrev, c);
        else n.ctx_tail = c;
        w32(b, c + kCtxPrev, 0);
        n.ctx_head = c;
    };
    {
        std::lock_guard lock(n.ctx_m);
        for (u32 c = n.ctx_head; c; c = r32(b, c + kCtxNext)) {
            if (ps_read64(b, c + kCtxTid) != tid) continue;
            if (c != n.ctx_head) {
                const u32 next = r32(b, c + kCtxNext), prev = r32(b, c + kCtxPrev);
                if (next) w32(b, next + kCtxPrev, prev);
                else n.ctx_tail = prev;
                if (prev) w32(b, prev + kCtxNext, next);
                else n.ctx_head = next;
                to_front(c);
            }
            return c;
        }
    }
    const u32 c = net_malloc(ctx, kCtxSize, data(ctx) + kTagMisc, 475);
    if (!c) return static_context(ctx, SYS_NET_ENOMEM);
    std::memset(b + c, 0, kCtxSize);
    ps_write64(b, c + kCtxTid, tid);
    w32(b, c + kCtxResolverRetrans, 5);
    w32(b, c + kCtx2448, 704);
    w32(b, c + kCtx2884, 0xFFFFFFFF);
    w32(b, c + kCtxResolverRetry, 1);
    w32(b, c + kCtx2452, 1);
    if (sync_create(ctx, c + kCtxSync, 1) < 0) {
        net_free(ctx, c);
        return static_context(ctx, 0);
    }
    { // lv2 is told the thread's sync object (sys_net_infoctl 53)
        StackVar block(ctx, b, 32);
        w32(b, block.addr(), r32(b, c + kCtxSync));
        w32(b, block.addr() + 4, r32(b, c + kCtxSync + 4));
        lv2_syscall(ctx, 722, {53, block.addr()});
    }
    std::lock_guard lock(n.ctx_m);
    to_front(c);
    return c;
}

// libnet's answer for an lv2 failure `r` (-errno): the errno kept in the context, SYS_NET_ERROR_* returned (a CELL
// error from lv2 itself is returned as it is).
s32 failure(PPUContext& ctx, s32 r)
{
    const u64 e = static_cast<u64>(-static_cast<s64>(r));
    w32(gb(ctx), context(ctx) + kCtxErrno, static_cast<u32>(e));
    if ((e >> 16) & 0xFFF) return r;
    return static_cast<s32>(static_cast<u32>(e) | SYS_NET_ERROR);
}

// An lv2 network call through the calling thread's context (libnet's wrappers): an aborted context answers its errno
// without the call.
s32 net_call(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    s32 r = 0;
    if (r32(b, c + kCtxAborted)) {
        r = -static_cast<s32>(r32(b, c + kCtxErrno));
    } else {
        w32(b, c + kCtxInCall, 1);
        r = static_cast<s32>(lv2_syscall(ctx, number, args));
        w32(b, c + kCtxInCall, 0);
    }
    return r >= 0 ? r : failure(ctx, r);
}

// Frees the contexts of threads not inside a call: every thread's (all) or the caller's. libnet f_cdf0: each freed
// thread's lv2 state is aborted first (sys_net_abort 3), and the list starts over after each. The number freed (the
// caller's own not counted when freeing all).
s32 free_contexts(PPUContext& ctx, bool all)
{
    Libnet& n = libnet();
    u8* b = gb(ctx);
    const u64 tid = thread_of(ctx).id;
    s32 freed = 0;
    std::lock_guard lock(n.ctx_m);
    for (u32 c = n.ctx_head; c;) {
        const u64 owner = ps_read64(b, c + kCtxTid);
        if ((!all && owner != tid) || r32(b, c + kCtxInCall)) {
            c = r32(b, c + kCtxNext);
            continue;
        }
        lv2_syscall(ctx, 721, {3, owner, 0});
        sync_destroy(ctx, c + kCtxSync);
        const u32 next = r32(b, c + kCtxNext), prev = r32(b, c + kCtxPrev);
        if (next) w32(b, next + kCtxPrev, prev);
        else n.ctx_tail = prev;
        if (prev) w32(b, prev + kCtxNext, next);
        else n.ctx_head = next;
        net_free(ctx, c);
        if (!all) return freed + 1;
        if (owner != tid) freed++;
        c = n.ctx_head;
    }
    return freed;
}

// ------------------------------------------------------------------------------------------------- random numbers

// libnet's generator: words 0..31 xor words 489..520, then words 32..520 xor words 0..488 (a lagged XOR, 521 and 32).
void rand_regenerate(u8* b, u32 st)
{
    for (u32 i = 0; i < 32; i++) w32(b, st + 4 * i, r32(b, st + 4 * i) ^ r32(b, st + 4 * (i + 489)));
    for (u32 i = 0; i < 489; i++) w32(b, st + 4 * (i + 32), r32(b, st + 4 * (i + 32)) ^ r32(b, st + 4 * i));
}

// Seeded from the clock (seconds xor microseconds through an LCG, a bit at a time), as libnet's f_d154.
void rand_seed(PPUContext& ctx, u32 st)
{
    u8* b = gb(ctx);
    const u64 now = lv2::sys_time_get_system_time(thread_of(ctx));
    const u32 sec = static_cast<u32>(now / 1000000), usec = static_cast<u32>(now % 1000000);
    u32 x = sec ^ usec, word = 0;
    for (u32 i = 0; i < 17; i++) {
        for (u32 k = 0; k < 32; k++) {
            x = x * 0x5D588B65 + 1;
            word = (word >> 1) | (x & 0x80000000);
        }
        w32(b, st + 4 * i, word);
    }
    w32(b, st + 64, (r32(b, st + 64) << 23) ^ (r32(b, st) >> 9) ^ r32(b, st + 60));
    for (u32 i = 0; i < 504; i++) {
        w32(b, st + 4 * (i + 17), (r32(b, st + 4 * i) << 23) ^ r32(b, st + 4 * (i + 16)) ^ (r32(b, st + 4 * (i + 1)) >> 9));
    }
    rand_regenerate(b, st);
    rand_regenerate(b, st);
    rand_regenerate(b, st);
}

// ------------------------------------------------------------------------------------------------- setup

// An internal function of this module, by NID (its descriptor; 0, logged, when it was not linked: a test that does not
// link sys_net's internal functions).
u32 internal(std::string_view name)
{
    const u32 d = HleProcess::current()->descriptor_of("sys_net", recomp::nid_of(name));
    if (!d) PS_LOG_WARN(kChan, "{} was not linked at load: libnet's atexit function is left out", name);
    return d;
}

// sysPrxForUser's sys_ppu_thread_(un)register_atexit, as libnet imports them.
s32 atexit_call(PPUContext& ctx, std::string_view fn, u32 func)
{
    const Function* f = registry().find("sysPrxForUser", recomp::nid_of(fn));
    if (!f) PS_FATAL(kChan, "sysPrxForUser has no {}", fn);
    const u64 saved = ctx.gpr[3];
    ctx.gpr[3] = func;
    f->fn(ctx, gb(ctx));
    const s32 r = static_cast<s32>(ctx.gpr[3]);
    ctx.gpr[3] = saved;
    return r;
}

// sys_net_initialize_network_ex(sys_net_initialize_parameter_t* param {memory, memory_size, flags}): the pool (at
// least 8 KB, flags 0 or 1), the generator, the atexit function that frees a thread's context.
s32 sys_net_initialize_network_ex(PPUContext& ctx, u32 param)
{
    Libnet& n = libnet();
    u8* b = gb(ctx);
    if (n.initialized) {
        const u32 flags = param && guest_readable(thread_of(ctx), param + 8, 4) ? r32(b, param + 8) : 0;
        return flags & 1 ? static_cast<s32>(SYS_NET_ERROR | SYS_NET_EBUSY) : 0;
    }
    constexpr s32 einval = static_cast<s32>(SYS_NET_ERROR | SYS_NET_EINVAL);
    if (!param || !guest_readable(thread_of(ctx), param, 12)) return einval;
    u32 memory = r32(b, param), size = r32(b, param + 4);
    const u32 flags = r32(b, param + 8);
    if (!memory || static_cast<s32>(size) <= 8191 || (flags & ~1u)) return einval;
    PS_LOG_INFO(kChan, "sys_net_initialize_network_ex: pool {:#x}+{:#x}, flags {}", memory, size, flags);
    {
        std::lock_guard lock(n.pool_m);
        const u32 skip = ((memory + 3) & ~3u) - memory;
        memory += skip;
        size -= skip;
        w32(b, memory + kHdrSize, size - kHdr);
        n.free_head = n.free_tail = memory;
        n.min_free = n.free = size;
        w32(b, memory + kHdrNext, 0);
        w32(b, memory + kHdrPrev, 0);
        n.start = memory;
        n.end = memory + size;
    }
    n.rand_state = net_malloc(ctx, kRandSize, data(ctx) + kTagMisc, 606);
    if (n.rand_state) {
        rand_seed(ctx, n.rand_state);
        n.rand_index = 520;
    }
    if (const u32 fn = internal("_pspocket_net_thread_atexit")) atexit_call(ctx, "sys_ppu_thread_register_atexit", fn);
    n.initialized = true;
    return 0;
}

// sys_net_finalize_network(): every socket closed (sys_net_abort 4), every context freed, the generator's state too.
s32 sys_net_finalize_network(PPUContext& ctx)
{
    Libnet& n = libnet();
    if (!n.initialized) return 0;
    if (const u32 fn = internal("_pspocket_net_thread_atexit")) atexit_call(ctx, "sys_ppu_thread_unregister_atexit", fn);
    lv2_syscall(ctx, 721, {4, 0, 0});
    free_contexts(ctx, true);
    net_free(ctx, n.rand_state);
    n.rand_state = 0;
    n.initialized = false;
    PS_LOG_INFO(kChan, "sys_net_finalize_network: done");
    return 0;
}

// libnet's atexit function: the exiting thread's context freed (sys_net_free_thread_context(0, 1)).
void _pspocket_net_thread_atexit(PPUContext& ctx)
{
    if (libnet().initialized) free_contexts(ctx, false);
}

// ------------------------------------------------------------------------------------------------- the allocator

// __sys_net_lib_malloc(size, tag, line): libnet's allocator over the pool (tag and line are the caller's source file
// and line, kept in the block's header). NULL when nothing fits.
u32 __sys_net_lib_malloc(PPUContext& ctx, u32 size, u32 tag, u32 line) { return net_malloc(ctx, size, tag, line); }

s32 _sys_net_lib_free(PPUContext& ctx, u32 ptr)
{
    net_free(ctx, ptr);
    return 0; // libnet's r3 after unlocking (or the null pointer)
}

// __sys_net_lib_calloc(n, size, tag, line): n * size bytes (32-bit), zeroed.
u32 __sys_net_lib_calloc(PPUContext& ctx, u32 count, u32 size, u32 tag, u32 line)
{
    const u32 bytes = count * size;
    const u32 p = net_malloc(ctx, bytes, tag, line);
    if (p) std::memset(gb(ctx) + p, 0, bytes);
    return p;
}

// __sys_net_lib_realloc(ptr, size, tag, line): ptr itself when its block holds size bytes already; else a new block
// with the old bytes and the rest zeroed, the old one freed (NULL, the old one kept, when nothing fits).
u32 __sys_net_lib_realloc(PPUContext& ctx, u32 ptr, u32 size, u32 tag, u32 line)
{
    u8* b = gb(ctx);
    if (ptr && !usable(ctx, ptr - kHdr, kHdr, "__sys_net_lib_realloc: ptr")) return 0;
    const u32 old = ptr ? r32(b, ptr - kHdr + kHdrSize) : 0;
    if (ptr && old >= size) return ptr;
    const u32 p = net_malloc(ctx, size, tag, line);
    if (!p) return 0;
    if (ptr) std::memcpy(b + p, b + ptr, old);
    std::memset(b + p + old, 0, size - old);
    net_free(ctx, ptr);
    return p;
}

// ------------------------------------------------------------------------------------------------- errno

u32 _sys_net_errno_loc(PPUContext& ctx) { return context(ctx) + kCtxErrno; }
u32 _sys_net_h_errno_loc(PPUContext& ctx) { return context(ctx) + kCtxHErrno; }

// The errno-coded answer of a libnet function that goes to lv2 outside a context (no errno kept): SYS_NET_ERROR_*.
s32 coded(s32 r)
{
    if (r >= 0) return r;
    const u64 e = static_cast<u64>(-static_cast<s64>(r));
    if ((e >> 16) & 0xFFF) return r;
    return static_cast<s32>(static_cast<u32>(e) | SYS_NET_ERROR);
}

// sys_net_free_thread_context(u64 tid, flags): 1 frees the caller's context (tid 0), 2 every thread's (tid 0), 4
// aborts the thread's lv2 calls (with 1: the caller's). The number freed.
s32 sys_net_free_thread_context(PPUContext& ctx, u64 tid, s32 flags)
{
    const auto run = [&]() -> s32 {
        if (flags & ~7) return -static_cast<s32>(SYS_NET_EINVAL);
        const bool abort = flags & 4;
        if (flags & 1) {
            if (tid || (flags & 2)) return -static_cast<s32>(SYS_NET_EINVAL);
            if (abort) {
                net_call(ctx, 721, {2, thread_of(ctx).id, 0});
                return 0;
            }
            return free_contexts(ctx, false);
        }
        if (flags & 2) {
            if (tid || abort) return -static_cast<s32>(SYS_NET_EINVAL);
            return free_contexts(ctx, true);
        }
        if (abort) net_call(ctx, 721, {2, tid, 0});
        return 0;
    };
    return coded(run());
}

// ------------------------------------------------------------------------------------------------- sockets

s32 accept(PPUContext& ctx, s32 s, u32 addr, u32 paddrlen) { return net_call(ctx, 700, {u64(s64{s}), addr, paddrlen}); }
s32 bind(PPUContext& ctx, s32 s, u32 addr, u32 addrlen) { return net_call(ctx, 701, {u64(s64{s}), addr, addrlen}); }
s32 connect(PPUContext& ctx, s32 s, u32 addr, u32 addrlen) { return net_call(ctx, 702, {u64(s64{s}), addr, addrlen}); }
s32 getpeername(PPUContext& ctx, s32 s, u32 addr, u32 paddrlen)
{
    return net_call(ctx, 703, {u64(s64{s}), addr, paddrlen});
}
s32 getsockname(PPUContext& ctx, s32 s, u32 addr, u32 paddrlen)
{
    return net_call(ctx, 704, {u64(s64{s}), addr, paddrlen});
}
s32 getsockopt(PPUContext& ctx, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    return net_call(ctx, 705, {u64(s64{s}), u64(s64{level}), u64(s64{optname}), optval, optlen});
}
s32 listen(PPUContext& ctx, s32 s, s32 backlog) { return net_call(ctx, 706, {u64(s64{s}), u64(s64{backlog})}); }
s32 recvfrom(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 paddrlen)
{
    return net_call(ctx, 707, {u64(s64{s}), buf, len, u64(s64{flags}), addr, paddrlen});
}
s32 recv(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags)
{
    return net_call(ctx, 707, {u64(s64{s}), buf, len, u64(s64{flags}), 0, 0});
}
s32 sendto(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 addrlen)
{
    return net_call(ctx, 710, {u64(s64{s}), buf, len, u64(s64{flags}), addr, addrlen});
}
s32 send(PPUContext& ctx, s32 s, u32 buf, u32 len, s32 flags)
{
    return net_call(ctx, 710, {u64(s64{s}), buf, len, u64(s64{flags}), 0, 0});
}
s32 setsockopt(PPUContext& ctx, s32 s, s32 level, s32 optname, u32 optval, u32 optlen)
{
    return net_call(ctx, 711, {u64(s64{s}), u64(s64{level}), u64(s64{optname}), optval, optlen});
}
s32 shutdown(PPUContext& ctx, s32 s, s32 how) { return net_call(ctx, 712, {u64(s64{s}), u64(s64{how})}); }
s32 socket(PPUContext& ctx, s32 family, s32 type, s32 protocol)
{
    return net_call(ctx, 713, {u64(s64{family}), u64(s64{type}), u64(s64{protocol})});
}
s32 socketclose(PPUContext& ctx, s32 s) { return net_call(ctx, 714, {u64(s64{s})}); }
s32 socketpoll(PPUContext& ctx, u32 fds, u32 nfds, s32 ms) { return net_call(ctx, 715, {fds, nfds, u64(s64{ms})}); }
s32 socketselect(PPUContext& ctx, s32 nfds, u32 readfds, u32 writefds, u32 exceptfds, u32 timeout)
{
    return net_call(ctx, 716, {u64(s64{nfds}), readfds, writefds, exceptfds, timeout});
}

// sys_net_msghdr (its 64-bit pointers' high words cleared by libnet first: msg_name, msg_iov, msg_control and each
// iovec's base).
void clear_msg_high_words(PPUContext& ctx, u32 msg)
{
    u8* b = gb(ctx);
    if (!msg || !usable(ctx, msg, 48, "msghdr")) return;
    const u32 iov = r32(b, msg + 20);
    w32(b, msg + 32, 0);
    w32(b, msg + 0, 0);
    w32(b, msg + 16, 0);
    if (!iov) return;
    const s32 count = static_cast<s32>(r32(b, msg + 24));
    for (s32 i = 0; i < count; i++) {
        if (!usable(ctx, iov + 16 * static_cast<u32>(i), 4, "msghdr: msg_iov")) return;
        w32(b, iov + 16 * static_cast<u32>(i), 0);
    }
}

s32 sendmsg(PPUContext& ctx, s32 s, u32 msg, s32 flags)
{
    clear_msg_high_words(ctx, msg);
    return net_call(ctx, 709, {u64(s64{s}), msg, u64(s64{flags})});
}

s32 recvmsg(PPUContext& ctx, s32 s, u32 msg, s32 flags)
{
    clear_msg_high_words(ctx, msg);
    return net_call(ctx, 708, {u64(s64{s}), msg, u64(s64{flags})});
}

// sys_net_abort_socket(s, flags): lv2's sys_net_abort 0 (the socket's blocked calls end with EINTR).
s32 sys_net_abort_socket(PPUContext& ctx, s32 s, s32 flags) { return net_call(ctx, 721, {0, u64(s64{s}), u64(s64{flags})}); }

// sys_net_abort_resolver(u64 tid, flags): sys_net_abort 1.
s32 sys_net_abort_resolver(PPUContext& ctx, u64 tid, s32 flags) { return net_call(ctx, 721, {1, tid, u64(s64{flags})}); }

// sys_net_infoctl through the thread's context with libnet's argument block (f_11950, 32 bytes laid out per command).
s32 infoctl(PPUContext& ctx, s32 cmd, std::initializer_list<std::pair<u32, u64>> words, std::initializer_list<u32> wide)
{
    u8* b = gb(ctx);
    StackVar block(ctx, b, 32);
    for (const auto& [off, v] : words) w32(b, block.addr() + off, static_cast<u32>(v));
    for (const u32 off : wide) (void)off;
    return net_call(ctx, 722, {u64(s64{cmd}), block.addr()});
}

// The sockets' states libnet reports in a list: lv2's TCP state through its table (anything else OPENED).
constexpr std::array<u32, 11> kTcpToState = {1, 4, 5, 6, 7, 10, 8, 11, 12, 9, 13};

void map_states(u8* b, u32 p, s32 count, u32 size)
{
    if (!p || count <= 0) return;
    for (s32 i = 0; i < count; i++) {
        const u32 at = p + 32 + static_cast<u32>(i) * size;
        const u32 st = r32(b, at);
        w32(b, at, st <= 10 ? kTcpToState[st] : 3);
    }
}

// sys_net_get_sockinfo(s, sys_net_sockinfo_t* p, n): infoctl 6; a list (s -1) has its states mapped.
s32 sys_net_get_sockinfo(PPUContext& ctx, s32 s, u32 p, s32 n)
{
    const s32 r = infoctl(ctx, 6, {{0, u32(s)}, {12, p}, {16, u32(n)}}, {});
    map_states(gb(ctx), p, r, 36);
    return r;
}

// sys_net_get_sockinfo_ex(s, sys_net_sockinfo_ex_t* p, n, flags): infoctl 11.
s32 sys_net_get_sockinfo_ex(PPUContext& ctx, s32 s, u32 p, s32 n, s32 flags)
{
    const s32 r = infoctl(ctx, 11, {{0, u32(s)}, {12, p}, {16, u32(n)}, {20, u32(flags)}}, {});
    map_states(gb(ctx), p, r, 80);
    return r;
}

// _sys_net_lib_set_libnetctl_queue(a, u64 b, u64 c) and _reset_(a): infoctl 55 and 56 (libnetctl's event queue).
s32 _sys_net_lib_set_libnetctl_queue(PPUContext& ctx, u32 a, u64 b, u64 c)
{
    return infoctl(ctx, 55, {{16, a}, {4, b}, {12, c}}, {});
}

s32 _sys_net_lib_reset_libnetctl_queue(PPUContext& ctx, u32 a) { return infoctl(ctx, 56, {{16, a}}, {}); }

// sys_net_set_resolver_configurations(retry, retrans, flags): the thread's resolver tries and timeout (0: 1 and 5);
// flags other than 0 refused (errno EBADF, then libnet's wrapper keeps 1: SYS_NET_ERROR 1).
s32 sys_net_set_resolver_configurations(PPUContext& ctx, s32 retry, s32 retrans, s32 flags)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    if (r32(b, c + kCtxAborted)) {
        const s32 r = -static_cast<s32>(r32(b, c + kCtxErrno));
        return r >= 0 ? r : failure(ctx, r);
    }
    w32(b, c + kCtxInCall, 1);
    s32 r = 0;
    if (flags) {
        w32(b, context(ctx) + kCtxErrno, SYS_NET_EBADF);
        r = -1;
    } else {
        w32(b, c + kCtxResolverRetrans, static_cast<u32>(retrans ? retrans : 5));
        w32(b, c + kCtxResolverRetry, static_cast<u32>(retry ? retry : 1));
    }
    w32(b, c + kCtxInCall, 0);
    return r >= 0 ? r : failure(ctx, r);
}

// ------------------------------------------------------------------------------------------------- libnet's helpers

s32 _sys_net_lib_sync_create(PPUContext& ctx, u32 sync, u64 events) 
{
    if (!usable(ctx, sync, 32, "_sys_net_lib_sync_create: sync")) return -static_cast<s32>(SYS_NET_EFAULT);
    return sync_create(ctx, sync, events);
}
s32 _sys_net_lib_sync_destroy(PPUContext& ctx, u32 sync) 
{
    if (!usable(ctx, sync, 32, "_sys_net_lib_sync_destroy: sync")) return -static_cast<s32>(SYS_NET_EFAULT);
    return sync_destroy(ctx, sync);
}
s32 _sys_net_lib_sync_signal(PPUContext& ctx, u32 sync, u64 events) 
{
    if (!usable(ctx, sync, 32, "_sys_net_lib_sync_signal: sync")) return -static_cast<s32>(SYS_NET_EFAULT);
    return sync_signal(ctx, sync, events);
}
s32 _sys_net_lib_sync_wait(PPUContext& ctx, u32 sync, u32 out) 
{
    if (!usable(ctx, sync, 32, "_sys_net_lib_sync_wait: sync")) return -static_cast<s32>(SYS_NET_EFAULT);
    return sync_wait(ctx, sync, out);
}

// _sys_net_lib_sync_clear(sync, bits): the bits cleared; those of them that were set.
u64 _sys_net_lib_sync_clear(PPUContext& ctx, u32 sync, u64 bits)
{
    u8* b = gb(ctx);
    if (!usable(ctx, sync, 32, "_sys_net_lib_sync_clear: sync")) return 0;
    u64 old = ps_read64(b, sync + 16);
    while (!atomic_cas_be64(b, sync + 16, old, old & ~bits)) old = ps_read64(b, sync + 16);
    return bits & old;
}

// _sys_net_lib_thread_create(u64* id, entry, arg, prio, name): a joinable thread with a 16 KB stack (sysPrxForUser's
// sys_ppu_thread_create); lv2's error, else 0.
s32 _sys_net_lib_thread_create(PPUContext& ctx, u32 id, u32 entry, u64 arg, s32 prio, u32 name)
{
    const Function* f = registry().find("sysPrxForUser", recomp::nid_of("sys_ppu_thread_create"));
    if (!f) PS_FATAL(kChan, "sysPrxForUser has no sys_ppu_thread_create");
    ctx.gpr[3] = id;
    ctx.gpr[4] = entry;
    ctx.gpr[5] = arg;
    ctx.gpr[6] = static_cast<u64>(s64{prio});
    ctx.gpr[7] = 0x4000;
    ctx.gpr[8] = 1;
    ctx.gpr[9] = name;
    f->fn(ctx, gb(ctx));
    const s32 r = static_cast<s32>(ctx.gpr[3]);
    return r <= 0 ? r : 0;
}

// _sys_net_lib_thread_exit(status): the thread's context freed, then sysPrxForUser's sys_ppu_thread_exit.
void _sys_net_lib_thread_exit(PPUContext& ctx, u64 status)
{
    free_contexts(ctx, false);
    const Function* f = registry().find("sysPrxForUser", recomp::nid_of("sys_ppu_thread_exit"));
    if (!f) PS_FATAL(kChan, "sysPrxForUser has no sys_ppu_thread_exit");
    ctx.gpr[3] = status;
    f->fn(ctx, gb(ctx));
    PS_FATAL(kChan, "sys_ppu_thread_exit returned");
}

s32 _sys_net_lib_thread_join(PPUContext& ctx, u64 id, u32 status)
{
    return static_cast<s32>(lv2_syscall(ctx, 44, {id, status}));
}

s32 _sys_net_lib_usleep(PPUContext& ctx, u64 us) { return static_cast<s32>(lv2_syscall(ctx, 141, {us})); }

// _sys_net_lib_get_system_time(u64* out).
s32 _sys_net_lib_get_system_time(PPUContext& ctx, u32 out)
{
    if (out && !usable(ctx, out, 8, "_sys_net_lib_get_system_time: out")) return 0;
    if (out) ps_write64(gb(ctx), out, lv2::sys_time_get_system_time(thread_of(ctx)));
    return 0;
}

// _sys_net_lib_rand(): the generator's next word (a fresh batch after 521).
u32 _sys_net_lib_rand(PPUContext& ctx)
{
    Libnet& n = libnet();
    u8* b = gb(ctx);
    std::lock_guard lock(n.rand_m);
    if (++n.rand_index > 520) {
        rand_regenerate(b, n.rand_state);
        n.rand_index = 0;
    }
    return r32(b, n.rand_state + 4 * n.rand_index);
}

// _sys_net_lib_abort: libnet's is an empty function (r3 comes back as it was).
void _sys_net_lib_abort(PPUContext&) {}

// ------------------------------------------------------------------------------------------------- address text
// libnet's conversions (BIND's, as Sony built them) in the C locale.

constexpr bool is_digit(u8 c) { return c >= '0' && c <= '9'; }
constexpr bool is_lower(u8 c) { return c >= 'a' && c <= 'z'; }
constexpr bool is_xdigit(u8 c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
constexpr bool is_space(u8 c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
// A letter's value as a hex digit (only meaningful for a-f, A-F; libnet computes it for any non-digit).
constexpr u32 hex_value(u8 c) { return static_cast<u32>(c) - (is_lower(c) ? 87u : 55u); }

// The guest string at `addr` (up to a NUL; libnet reads without bounds), none (logged) when it runs into memory that
// is not guest memory first.
std::optional<std::string> guest_str(PPUContext& ctx, u32 addr, std::string_view what)
{
    const GuestString s = read_cstr(thread_of(ctx), addr, 0x10000);
    if (s.faulted) {
        log_call_site(ctx, gb(ctx), std::format("{}: {:#x} is not a string in guest memory", what, addr));
        set_errno(ctx, SYS_NET_EFAULT);
        return std::nullopt;
    }
    return s.text;
}

// libnet f_5e08: inet_aton's parser (pton: decimal only, exactly four parts, for inet_pton). Parts are 32-bit (they
// wrap). The address in host order, or none.
std::optional<u32> parse_aton(std::string_view s, bool pton)
{
    usize i = 0;
    const auto at = [&](usize k) -> u8 { return k < s.size() ? static_cast<u8>(s[k]) : 0; };
    u8 c = at(0);
    if (!is_digit(c)) return std::nullopt;
    std::array<u32, 3> parts{};
    usize count = 0;
    u32 val = 0;
    for (;;) {
        val = 0;
        u32 base = 10;
        if (c == '0') {
            c = at(++i);
            if (c == 'x' || c == 'X') {
                base = 16;
                c = at(++i);
            } else if (is_digit(c) && c != '9') {
                base = 8;
            }
        }
        if (pton && base != 10) return std::nullopt;
        for (;;) {
            if (is_digit(c)) {
                if (base <= static_cast<u32>(c - '0')) break;
                val = val * base + static_cast<u32>(c - '0');
                c = at(++i);
            } else if (base == 16 && is_xdigit(c) && hex_value(c) <= 15) {
                val = (val << 4) | hex_value(c);
                c = at(++i);
            } else {
                break;
            }
        }
        if (c != '.') break;
        if (count >= parts.size()) return std::nullopt;
        parts[count++] = val;
        c = at(++i);
        if (!is_digit(c)) return std::nullopt;
    }
    if (c != 0 && !is_space(c)) return std::nullopt;
    const usize n = count + 1;
    if (pton) {
        if (n != 4 || (parts[0] | parts[1] | parts[2] | val) > 0xFF) return std::nullopt;
        return parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | val;
    }
    switch (n) {
    case 2:
        if (parts[0] > 0xFF || val > 0xFFFFFF) return std::nullopt;
        return val | parts[0] << 24;
    case 3:
        if ((parts[0] | parts[1]) > 0xFF || val > 0xFFFF) return std::nullopt;
        return val | parts[0] << 24 | parts[1] << 16;
    case 4:
        if ((parts[0] | parts[1] | parts[2] | val) > 0xFF) return std::nullopt;
        return parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | val;
    default: return val;
    }
}

void set_errno(PPUContext& ctx, u32 e) { w32(gb(ctx), context(ctx) + kCtxErrno, e); }

// inet_aton(cp, struct in_addr* addr): 1 and the address, 0 when cp is not one; a null argument EINVAL.
s32 inet_aton(PPUContext& ctx, u32 cp, u32 addr)
{
    if (!cp || !addr) {
        set_errno(ctx, SYS_NET_EINVAL);
        return 0;
    }
    const auto text = guest_str(ctx, cp, "inet_aton: cp");
    if (!text || !usable(ctx, addr, 4, "inet_aton: addr")) return 0;
    const auto a = parse_aton(*text, false);
    if (!a) return 0;
    w32(gb(ctx), addr, *a);
    return 1;
}

// inet_addr(cp): the address, INADDR_NONE when cp is not one (a null cp: EINVAL too).
u32 inet_addr(PPUContext& ctx, u32 cp)
{
    if (!cp) {
        set_errno(ctx, SYS_NET_EINVAL);
        return kInaddrNone;
    }
    const auto text = guest_str(ctx, cp, "inet_addr: cp");
    const auto a = text ? parse_aton(*text, false) : std::nullopt;
    return a ? *a : kInaddrNone;
}

// inet_network(cp): BIND's: up to four parts of 0..255 in C notation folded into the network number.
u32 inet_network(PPUContext& ctx, u32 cp)
{
    const auto text = guest_str(ctx, cp, "inet_network: cp");
    if (!text) return kInaddrNone;
    const std::string& s = *text;
    usize i = 0;
    const auto at = [&](usize k) -> u8 { return k < s.size() ? static_cast<u8>(s[k]) : 0; };
    std::array<u32, 4> parts{};
    usize count = 0;
    u32 val = 0;
    for (;;) {
        val = 0;
        u32 base = 10;
        bool digit = false;
        if (at(i) == '0') {
            digit = true;
            base = 8;
            i++;
        }
        if (at(i) == 'x' || at(i) == 'X') {
            base = 16;
            i++;
        }
        u8 c = 0;
        while ((c = at(i)) != 0) {
            if (is_digit(c)) {
                if (base == 8 && (c == '8' || c == '9')) return kInaddrNone;
                val = val * base + static_cast<u32>(c - '0');
                i++;
                digit = true;
            } else if (base == 16 && is_xdigit(c)) {
                val = val * 16 + hex_value(c);
                i++;
                digit = true;
            } else {
                break;
            }
        }
        if (!digit) return kInaddrNone;
        if (c != '.') break;
        if (count >= parts.size() || val > 0xFF) return kInaddrNone;
        parts[count++] = val;
        i++;
    }
    const u8 c = at(i);
    if (c && !is_space(c)) return kInaddrNone;
    if (count >= parts.size() || val > 0xFF) return kInaddrNone;
    parts[count++] = val;
    u32 out = 0;
    for (usize k = 0; k < count; k++) out = out << 8 | (parts[k] & 0xFF);
    return out;
}

// inet_lnaof(in), inet_netof(in): the host and network parts by the address's class.
u32 inet_lnaof(PPUContext&, u32 in)
{
    if (!(in & 0x80000000)) return in & 0x00FFFFFF;
    if ((in & 0xC0000000) == 0x80000000) return in & 0xFFFF;
    return in & 0xFF;
}

u32 inet_netof(PPUContext&, u32 in)
{
    if (!(in & 0x80000000)) return in >> 24;
    if ((in & 0xC0000000) == 0x80000000) return in >> 16;
    return in >> 8;
}

// inet_makeaddr(net, host): struct in_addr is returned through the caller's buffer (r3), then net and host.
u32 inet_makeaddr(PPUContext& ctx, u32 out, u32 net, u32 host)
{
    if (!usable(ctx, out, 4, "inet_makeaddr: result")) return 0;
    u32 a = 0;
    if (net < 128) a = net << 24 | (host & 0x00FFFFFF);
    else if (net < 65536) a = net << 16 | (host & 0xFFFF);
    else if (net < 16777216) a = net << 8 | (host & 0xFF);
    else a = net | host;
    w32(gb(ctx), out, a);
    return out;
}

// inet_pton6: BIND 8's (a group's value up to 0xffff, so leading zeros are allowed; one "::"; a dotted quad last).
std::optional<std::array<u8, 16>> pton6(std::string_view s)
{
    std::array<u8, 16> tmp{};
    usize tp = 0;
    std::optional<usize> colonp;
    usize i = 0;
    const auto at = [&](usize k) -> u8 { return k < s.size() ? static_cast<u8>(s[k]) : 0; };
    if (at(0) == ':' && at(++i) != ':') return std::nullopt;
    usize curtok = i;
    bool saw_xdigit = false;
    u32 val = 0;
    for (u8 ch = 0; (ch = at(i)) != 0;) {
        i++;
        if (is_xdigit(ch)) {
            val = val << 4 | (is_digit(ch) ? static_cast<u32>(ch - '0') : hex_value(ch));
            if (val > 0xFFFF) return std::nullopt;
            saw_xdigit = true;
            continue;
        }
        if (ch == ':') {
            curtok = i;
            if (!saw_xdigit) {
                if (colonp) return std::nullopt;
                colonp = tp;
                continue;
            }
            if (!at(i)) return std::nullopt;
            if (tp + 2 > 16) return std::nullopt;
            tmp[tp++] = static_cast<u8>(val >> 8);
            tmp[tp++] = static_cast<u8>(val);
            saw_xdigit = false;
            val = 0;
            continue;
        }
        if (ch == '.' && tp + 4 <= 16) {
            const auto v4 = parse_aton(s.substr(curtok), true);
            if (!v4) return std::nullopt;
            for (u32 k = 0; k < 4; k++) tmp[tp++] = static_cast<u8>(*v4 >> (24 - 8 * k));
            saw_xdigit = false;
            goto done;
        }
        return std::nullopt;
    }
    if (saw_xdigit) {
        if (tp + 2 > 16) return std::nullopt;
        tmp[tp++] = static_cast<u8>(val >> 8);
        tmp[tp++] = static_cast<u8>(val);
    }
done:
    if (colonp) {
        if (tp == 16) return std::nullopt;
        const usize n = tp - *colonp;
        for (usize k = 1; k <= n; k++) {
            tmp[16 - k] = tmp[*colonp + n - k];
            tmp[*colonp + n - k] = 0;
        }
        tp = 16;
    }
    if (tp != 16) return std::nullopt;
    return tmp;
}

// inet_pton(af, src, dst): 1, 0 when src is not an address, -1 with errno for a null pointer (EINVAL) or another
// family (EAFNOSUPPORT).
s32 inet_pton(PPUContext& ctx, s32 af, u32 src, u32 dst)
{
    u8* b = gb(ctx);
    if (!src || !dst) {
        set_errno(ctx, SYS_NET_EINVAL);
        return -1;
    }
    if (af == SYS_NET_AF_INET || af == SYS_NET_AF_INET6) {
        const auto text = guest_str(ctx, src, "inet_pton: src");
        const u32 size = af == SYS_NET_AF_INET ? 4 : 16;
        if (!text || !usable(ctx, dst, size, "inet_pton: dst")) return -1;
        if (af == SYS_NET_AF_INET) {
            const auto a = parse_aton(*text, true);
            if (!a) return 0;
            w32(b, dst, *a);
        } else {
            const auto a = pton6(*text);
            if (!a) return 0;
            std::memcpy(b + dst, a->data(), 16);
        }
        return 1;
    }
    set_errno(ctx, SYS_NET_EAFNOSUPPORT);
    return -1;
}

std::string ntop4(const u8* a) { return std::format("{}.{}.{}.{}", a[0], a[1], a[2], a[3]); }

// inet_ntop6: BIND 8's (the longest run of two or more zero groups is "::"; ::a.b.c.d and ::ffff:a.b.c.d end in a
// dotted quad).
std::string ntop6(const u8* a)
{
    std::array<u32, 8> words{};
    for (u32 i = 0; i < 16; i++) words[i / 2] |= u32{a[i]} << ((1 - (i % 2)) << 3);
    struct Run {
        s32 base = -1, len = 0;
    } best, cur;
    for (s32 i = 0; i < 8; i++) {
        if (words[static_cast<usize>(i)] == 0) {
            if (cur.base == -1) cur = {i, 1};
            else cur.len++;
        } else if (cur.base != -1) {
            if (best.base == -1 || cur.len > best.len) best = cur;
            cur.base = -1;
        }
    }
    if (cur.base != -1 && (best.base == -1 || cur.len > best.len)) best = cur;
    if (best.base != -1 && best.len < 2) best.base = -1;
    std::string out;
    for (s32 i = 0; i < 8; i++) {
        if (best.base != -1 && i >= best.base && i < best.base + best.len) {
            if (i == best.base) out += ':';
            continue;
        }
        if (i != 0) out += ':';
        if (i == 6 && best.base == 0 && (best.len == 6 || (best.len == 5 && words[5] == 0xffff))) {
            out += ntop4(a + 12);
            break;
        }
        out += std::format("{:x}", words[static_cast<usize>(i)]);
    }
    if (best.base != -1 && best.base + best.len == 8) out += ':';
    return out;
}

// inet_ntop(af, src, dst, size): dst, or NULL with errno ENOSPC (the text and its NUL don't fit) or EAFNOSUPPORT.
u32 inet_ntop(PPUContext& ctx, s32 af, u32 src, u32 dst, u32 size)
{
    u8* b = gb(ctx);
    std::string text;
    if ((af == SYS_NET_AF_INET || af == SYS_NET_AF_INET6) &&
        !usable(ctx, src, af == SYS_NET_AF_INET ? 4 : 16, "inet_ntop: src")) {
        return 0;
    }
    if (af == SYS_NET_AF_INET) text = ntop4(b + src);
    else if (af == SYS_NET_AF_INET6) text = ntop6(b + src);
    else {
        set_errno(ctx, SYS_NET_EAFNOSUPPORT);
        return 0;
    }
    if (text.size() + 1 > size) {
        set_errno(ctx, SYS_NET_ENOSPC);
        return 0;
    }
    if (!usable(ctx, dst, static_cast<u32>(text.size()) + 1, "inet_ntop: dst")) return 0;
    std::memcpy(b + dst, text.c_str(), text.size() + 1);
    return dst;
}

// inet_ntoa(in): the text in the thread's context (16 bytes at +2424), as libnet keeps it.
u32 inet_ntoa(PPUContext& ctx, u32 in)
{
    u8* b = gb(ctx);
    const u32 buf = context(ctx) + 2424;
    const u8 bytes[4] = {static_cast<u8>(in >> 24), static_cast<u8>(in >> 16), static_cast<u8>(in >> 8),
        static_cast<u8>(in)};
    const std::string text = ntop4(bytes);
    std::memcpy(b + buf, text.c_str(), text.size() + 1);
    return buf;
}


// ------------------------------------------------------------------------------------------------- the resolver

// The hostent libnet keeps in the thread's context: struct hostent at +192 {h_name, h_aliases, h_addrtype, h_length,
// h_addr_list}, the aliases at +212, the address pointers at +48, the name buffer at +352 (1025 bytes), the address
// at +2400.
constexpr u32 kHost = 192, kHostAliases = 212, kHostAddrPtrs = 48, kHostBuf = 352, kHostAddr = 2400;
constexpr s32 NETDB_INTERNAL = -1, NETDB_SUCCESS = 0, HOST_NOT_FOUND = 1, TRY_AGAIN = 2;
constexpr u32 SYS_NET_ENOBUFS = 55;

void set_h_errno(PPUContext& ctx, s32 e) { w32(gb(ctx), context(ctx) + kCtxHErrno, static_cast<u32>(e)); }

// A hostent for a name that is an address itself (gethnamaddr.c's "fake up a hostent").
u32 literal_hostent(PPUContext& ctx, std::string_view name, s32 af, const u8* addr, u32 size)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    const usize n = std::min<usize>(name.size(), 1025);
    std::memset(b + c + kHostBuf, 0, 1026);
    std::memcpy(b + c + kHostBuf, name.data(), n);
    std::memcpy(b + c + kHostAddr, addr, size);
    w32(b, c + kHost, c + kHostBuf);
    w32(b, c + kHost + 4, c + kHostAliases);
    w32(b, c + kHostAliases, 0);
    w32(b, c + kHost + 8, static_cast<u32>(af));
    w32(b, c + kHost + 12, size);
    w32(b, c + kHostAddrPtrs, c + kHostAddr);
    w32(b, c + kHostAddrPtrs + 4, 0);
    w32(b, c + kHost + 16, c + kHostAddrPtrs);
    set_h_errno(ctx, NETDB_SUCCESS);
    return c + kHost;
}

// gethostbyname(name), interim (D-523): libnet's answers for a name that is an address (a hostent for it, or
// HOST_NOT_FOUND) and for the names of its /etc/hosts ("127.0.0.1 localhost localhost."); any other name needs a name
// server, and with none (offline) libnet's lookup fails: NULL, TRY_AGAIN. libnet's resolver itself (its hosts file
// reads, res_init, the DNS query and its pool allocations) is the next step.
u32 gethostbyname(PPUContext& ctx, u32 name)
{
    u8* b = gb(ctx);
    if (!name) {
        set_errno(ctx, SYS_NET_EINVAL);
        set_h_errno(ctx, NETDB_INTERNAL);
        return 0;
    }
    if (r32(b, context(ctx) + kCtxAborted)) {
        set_errno(ctx, SYS_NET_ENOBUFS);
        set_h_errno(ctx, NETDB_INTERNAL);
        return 0;
    }
    const auto text = guest_str(ctx, name, "gethostbyname: name");
    if (!text) {
        set_h_errno(ctx, NETDB_INTERNAL);
        return 0;
    }
    const std::string& n = *text;
    const auto literal = [&](bool v6) -> std::optional<u32> {
        if (n.empty() || n.back() == '.') return std::nullopt;
        for (const char ch : n) {
            const u8 c = static_cast<u8>(ch);
            if (v6 ? !(is_xdigit(c) || c == ':' || c == '.') : !(is_digit(c) || c == '.')) return std::nullopt;
        }
        const auto a = parse_aton(n, true);
        if (!a) {
            set_h_errno(ctx, HOST_NOT_FOUND);
            return 0u;
        }
        const u8 bytes[4] = {static_cast<u8>(*a >> 24), static_cast<u8>(*a >> 16), static_cast<u8>(*a >> 8),
            static_cast<u8>(*a)};
        return literal_hostent(ctx, n, SYS_NET_AF_INET, bytes, 4);
    };
    if (!n.empty() && is_digit(static_cast<u8>(n[0]))) {
        if (const auto r = literal(false)) return *r;
    }
    if ((!n.empty() && is_xdigit(static_cast<u8>(n[0])) && n.find(':') != std::string::npos) || (!n.empty() && n[0] == ':')) {
        if (const auto r = literal(true)) return *r;
    }
    if (n == "localhost" || n == "localhost.") {
        const u32 c = context(ctx);
        const u8 loop[4] = {127, 0, 0, 1};
        const u32 h = literal_hostent(ctx, "localhost", SYS_NET_AF_INET, loop, 4);
        std::memcpy(b + c + kHostBuf + 10, "localhost.", 11);
        w32(b, c + kHostAliases, c + kHostBuf + 10);
        w32(b, c + kHostAliases + 4, 0);
        return h;
    }
    PS_LOG_INFO(kChan, "gethostbyname(\"{}\"): no name server (no network); NULL, h_errno TRY_AGAIN", n);
    set_h_errno(ctx, TRY_AGAIN);
    return 0;
}


// ------------------------------------------------------------------------------------------------- interfaces

// liblv2's printf output (sys_tty_write on channel 0), as libnet's _sys_printf calls make it.
void tty_print(PPUContext& ctx, std::string_view text)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* b = gb(ctx);
    StackVar buf(ctx, b, static_cast<u32>(text.size()) + 8);
    std::memcpy(b + buf.addr(), text.data(), text.size());
    lv2::sys_tty_write(t, 0, buf.addr(), static_cast<u32>(text.size()), buf.addr() + static_cast<u32>(text.size()));
}

// _sys_net_lib_sysctl(name, namelen, oldp, u32* oldlenp, newp, newlen): lv2's sysctl with the length widened to 64
// bits (made before the thread's context is looked at); errno-coded.
s32 _sys_net_lib_sysctl(PPUContext& ctx, u32 name, u32 namelen, u32 oldp, u32 oldlenp, u32 newp, u32 newlen)
{
    u8* b = gb(ctx);
    if (oldlenp && !usable(ctx, oldlenp, 4, "_sys_net_lib_sysctl: oldlenp")) return -1;
    StackVar len(ctx, b, 8);
    ps_write64(b, len.addr(), oldlenp ? r32(b, oldlenp) : 0);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 725, {name, namelen, oldp, len.addr(), newp, newlen}));
    if (oldlenp) w32(b, oldlenp, static_cast<u32>(ps_read64(b, len.addr())));
    const u32 c = context(ctx);
    if (r32(b, c + kCtxAborted)) {
        const s32 e = -static_cast<s32>(r32(b, c + kCtxErrno));
        return e >= 0 ? e : failure(ctx, e);
    }
    w32(b, c + kCtxInCall, 0);
    return r >= 0 ? r : failure(ctx, r);
}

// libnet's getifaddrs: lv2's interface list (sysctl {CTL_NET, PF_ROUTE, 0, 0, 4, 0}) into a buffer in the pool. With
// no link lv2 lists none: *ifap NULL, 0. -1 when lv2 or the pool refuses.
s32 getifaddrs(PPUContext& ctx, u32 ifap)
{
    u8* b = gb(ctx);
    StackVar v(ctx, b, 0x20); // the length, then the MIB
    const u32 len = v.addr(), mib = v.addr() + 4;
    const u32 words[6] = {4, 17, 0, 0, 4, 0};
    for (u32 i = 0; i < 6; i++) w32(b, mib + 4 * i, words[i]);
    if (_sys_net_lib_sysctl(ctx, mib, 6, 0, len, 0, 0) < 0) return -1;
    const u32 buf = net_malloc(ctx, r32(b, len), data(ctx) + kTagGetifaddrs, 95);
    if (!buf) return -1;
    if (_sys_net_lib_sysctl(ctx, mib, 6, buf, len, 0, 0) < 0) {
        net_free(ctx, buf);
        return -1;
    }
    if (r32(b, len)) PS_FATAL(kChan, "getifaddrs: lv2 listed interfaces; with no link it has none (D-523)");
    w32(b, ifap, 0);
    net_free(ctx, buf);
    return 0;
}

// libnet f_1a3dc: getifaddrs, its failure noted in the context (+2904 41, +2920 118) with errno ENOMEM.
s32 interfaces(PPUContext& ctx, u32 ifap)
{
    const s32 r = getifaddrs(ctx, ifap);
    if (r < 0) {
        u8* b = gb(ctx);
        w32(b, context(ctx) + 2904, 41);
        w32(b, context(ctx) + 2920, 118);
        set_errno(ctx, SYS_NET_ENOMEM);
    }
    return r;
}

// sys_net_get_if_list(s32* list, n): the indices of the interfaces with an address, up to n (with a null list, how
// many). None with no link.
s32 sys_net_get_if_list(PPUContext& ctx, u32, s32)
{
    StackVar ifap(ctx, gb(ctx), 8);
    if (interfaces(ctx, ifap.addr()) != 0) return -static_cast<s32>(r32(gb(ctx), context(ctx) + kCtxErrno));
    return 0; // the list is empty: freeifaddrs of nothing
}

// sys_net_if_ctl's global reports (interface 0, a 4-byte buffer): 640/641 lv2's free and lowest free memory (infoctl
// 8), 642/643 libnet's pool, 644 lv2's packet count (infoctl 10).
s32 if_stat(PPUContext& ctx, s32 cmd)
{
    Libnet& n = libnet();
    switch (cmd) {
    case 640:
    case 641: return infoctl(ctx, 8, {{0, u32(cmd - 640)}}, {});
    case 642: return static_cast<s32>(n.free);
    case 643: return static_cast<s32>(n.min_free);
    default: return infoctl(ctx, 10, {}, {});
    }
}

// sys_net_show_ifconfig(): libnet's report of the interfaces (none with no link), lv2's and libnet's memory, the
// packets and the threads with a context, on the TTY.
s32 sys_net_show_ifconfig(PPUContext& ctx)
{
    Libnet& n = libnet();
    u8* b = gb(ctx);
    if (sys_net_get_if_list(ctx, 0, 5) <= 0) tty_print(ctx, "No interface found.\n");
    s32 a = if_stat(ctx, 640), m = if_stat(ctx, 641);
    tty_print(ctx, std::format("kernel: free_size={}/free_min={}\n", a, m));
    a = if_stat(ctx, 642);
    m = if_stat(ctx, 643);
    tty_print(ctx, std::format("libnet: free_size={}/free_min={}\n", a, m));
    tty_print(ctx, std::format("packets: {}\n", if_stat(ctx, 644)));
    tty_print(ctx, std::format("thread context (size={}):", kCtxSize));
    std::vector<u64> tids;
    {
        std::lock_guard lock(n.ctx_m);
        for (u32 c = n.ctx_head; c; c = r32(b, c + kCtxNext)) tids.push_back(ps_read64(b, c + kCtxTid));
    }
    for (usize i = 0; i < tids.size(); i++) {
        if (i % 5 == 0) tty_print(ctx, "\n");
        tty_print(ctx, std::format(" tid={:#x}", static_cast<u32>(tids[i])));
    }
    tty_print(ctx, "\n");
    return 0;
}

} // namespace

void register_sys_net(Registry& r)
{
    using detail::thunk;
    const auto f = [](std::string_view name, HleFn fn) { return Function{{}, name, recomp::nid_of(name), fn, false}; };
    r.add("sys_net", {
        f("sys_net_initialize_network_ex", &thunk<&sys_net_initialize_network_ex>),
        f("sys_net_finalize_network", &thunk<&sys_net_finalize_network>),
        f("sys_net_free_thread_context", &thunk<&sys_net_free_thread_context>),
        f("_sys_net_errno_loc", &thunk<&_sys_net_errno_loc>),
        f("_sys_net_h_errno_loc", &thunk<&_sys_net_h_errno_loc>),
        f("__sys_net_lib_malloc", &thunk<&__sys_net_lib_malloc>),
        f("_sys_net_lib_free", &thunk<&_sys_net_lib_free>),
        f("__sys_net_lib_calloc", &thunk<&__sys_net_lib_calloc>),
        f("__sys_net_lib_realloc", &thunk<&__sys_net_lib_realloc>),
        f("accept", &thunk<&accept>),
        f("bind", &thunk<&bind>),
        f("connect", &thunk<&connect>),
        f("getpeername", &thunk<&getpeername>),
        f("getsockname", &thunk<&getsockname>),
        f("getsockopt", &thunk<&getsockopt>),
        f("listen", &thunk<&listen>),
        f("recv", &thunk<&recv>),
        f("recvfrom", &thunk<&recvfrom>),
        f("recvmsg", &thunk<&recvmsg>),
        f("send", &thunk<&send>),
        f("sendmsg", &thunk<&sendmsg>),
        f("sendto", &thunk<&sendto>),
        f("setsockopt", &thunk<&setsockopt>),
        f("shutdown", &thunk<&shutdown>),
        f("socket", &thunk<&socket>),
        f("socketclose", &thunk<&socketclose>),
        f("socketpoll", &thunk<&socketpoll>),
        f("socketselect", &thunk<&socketselect>),
        f("sys_net_abort_socket", &thunk<&sys_net_abort_socket>),
        f("sys_net_abort_resolver", &thunk<&sys_net_abort_resolver>),
        f("sys_net_get_sockinfo", &thunk<&sys_net_get_sockinfo>),
        f("sys_net_get_sockinfo_ex", &thunk<&sys_net_get_sockinfo_ex>),
        f("_sys_net_lib_set_libnetctl_queue", &thunk<&_sys_net_lib_set_libnetctl_queue>),
        f("_sys_net_lib_reset_libnetctl_queue", &thunk<&_sys_net_lib_reset_libnetctl_queue>),
        f("sys_net_set_resolver_configurations", &thunk<&sys_net_set_resolver_configurations>),
        f("_sys_net_lib_sync_create", &thunk<&_sys_net_lib_sync_create>),
        f("_sys_net_lib_sync_destroy", &thunk<&_sys_net_lib_sync_destroy>),
        f("_sys_net_lib_sync_signal", &thunk<&_sys_net_lib_sync_signal>),
        f("_sys_net_lib_sync_wait", &thunk<&_sys_net_lib_sync_wait>),
        f("_sys_net_lib_sync_clear", &thunk<&_sys_net_lib_sync_clear>),
        f("_sys_net_lib_thread_create", &thunk<&_sys_net_lib_thread_create>),
        f("_sys_net_lib_thread_exit", &thunk<&_sys_net_lib_thread_exit>),
        f("_sys_net_lib_thread_join", &thunk<&_sys_net_lib_thread_join>),
        f("_sys_net_lib_usleep", &thunk<&_sys_net_lib_usleep>),
        f("_sys_net_lib_get_system_time", &thunk<&_sys_net_lib_get_system_time>),
        f("_sys_net_lib_rand", &thunk<&_sys_net_lib_rand>),
        f("_sys_net_lib_abort", &thunk<&_sys_net_lib_abort>),
        f("inet_aton", &thunk<&inet_aton>),
        f("inet_addr", &thunk<&inet_addr>),
        f("inet_network", &thunk<&inet_network>),
        f("inet_lnaof", &thunk<&inet_lnaof>),
        f("inet_netof", &thunk<&inet_netof>),
        f("inet_makeaddr", &thunk<&inet_makeaddr>),
        f("inet_pton", &thunk<&inet_pton>),
        f("inet_ntop", &thunk<&inet_ntop>),
        f("inet_ntoa", &thunk<&inet_ntoa>),
        f("gethostbyname", &thunk<&gethostbyname>),
        f("_sys_net_lib_sysctl", &thunk<&_sys_net_lib_sysctl>),
        f("sys_net_get_if_list", &thunk<&sys_net_get_if_list>),
        f("sys_net_show_ifconfig", &thunk<&sys_net_show_ifconfig>),
        PS_HLE_INTERNAL(0x66AFE280, _pspocket_net_thread_atexit),
    });
}

} // namespace pspocket::hle
