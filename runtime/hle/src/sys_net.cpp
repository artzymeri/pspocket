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
constexpr u32 kTagNetset = kCtxSize + 24;    // "netset.c"
constexpr u32 kStrLo0 = kCtxSize + 40;       // "lo0", "127.0.0.1", "255.0.0.0", "pppoe0": netset.c's constants
constexpr u32 kStrLoopback = kCtxSize + 48, kStrLoopMask = kCtxSize + 64, kStrPppoe = kCtxSize + 80;
constexpr u32 kTagGethnamaddr = kCtxSize + 96, kTagResQuery = kCtxSize + 112, kTagResComp = kCtxSize + 128;
constexpr u32 kHostsText = kCtxSize + 144; // libnet's /etc/hosts

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
    // netset.c's statics (D-539): the library's name servers (a resolv.conf text in the pool, set with
    // sys_net_set_lib_name_server), the routing messages' sequence number.
    u32 lib_name_servers = 0;
    u32 route_seq = 0;
    // The netsets (sys_netset_open), most recent last, the next ID; how many are up (if_up), PPPoE links, whether lo0
    // was set up; the configuration values sys_netset_get_key_value reads (the first netset's).
    u32 sets_head = 0, sets_tail = 0, next_set = 1, sets_up = 0, pppoe_up = 0, loopback_up = 0;
    std::array<u32, 13> keys{};
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
        std::memcpy(gb(ctx) + n.data + kTagNetset, "netset.c", 9);
        std::memcpy(gb(ctx) + n.data + kStrLo0, "lo0", 4);
        std::memcpy(gb(ctx) + n.data + kStrLoopback, "127.0.0.1", 10);
        std::memcpy(gb(ctx) + n.data + kStrLoopMask, "255.0.0.0", 10);
        std::memcpy(gb(ctx) + n.data + kStrPppoe, "pppoe0", 7);
        std::memcpy(gb(ctx) + n.data + kTagGethnamaddr, "gethnamaddr.c", 14);
        std::memcpy(gb(ctx) + n.data + kTagResQuery, "res_query.c", 12);
        std::memcpy(gb(ctx) + n.data + kTagResComp, "res_comp.c", 11);
        std::memcpy(gb(ctx) + n.data + kHostsText, "127.0.0.1 localhost localhost.\n", 32);
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
    const u64 now = lv2::guest_system_time_us();
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
    if (out) ps_write64(gb(ctx), out, lv2::guest_system_time_us());
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

// ------------------------------------------------------------------------------------------------- configuration

// D-539: libnet's configuration calls: ioctl and control, the dumps, the test parameters, the name servers.

// The context's report of netset.c's last failure (sys_netset_get_status reads it): +2904 what failed, +2908..+2916 its
// arguments, +2920 libnet's source line.
constexpr u32 kCtxSetError = 2904, kCtxSetArg1 = 2908, kCtxSetArg2 = 2912, kCtxSetArg3 = 2916, kCtxSetLine = 2920;

void set_report(PPUContext& ctx, u32 error, u32 line)
{
    u8* b = gb(ctx);
    w32(b, context(ctx) + kCtxSetError, error);
    w32(b, context(ctx) + kCtxSetLine, line);
}

// libnet's refusal of an argument inside a call that goes through the context (errno EINVAL), or the aborted
// context's error.
s32 refuse_einval(PPUContext& ctx)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    if (r32(b, c + kCtxAborted)) {
        const s32 r = -static_cast<s32>(r32(b, c + kCtxErrno));
        return r >= 0 ? r : failure(ctx, r);
    }
    w32(b, c + kCtxInCall, 0);
    w32(b, context(ctx) + kCtxErrno, SYS_NET_EINVAL);
    return static_cast<s32>(SYS_NET_ERROR | SYS_NET_EINVAL);
}

// The commands libnet's _sys_net_lib_ioctl passes to lv2 (any other is EINVAL).
bool ioctl_known(u32 cmd)
{
    switch (cmd) {
    case 0x200050C8: case 0x800250CF: case 0x801050CD: case 0x80146980: case 0x80146981: case 0x8020690C:
    case 0x8020690E: case 0x80206910: case 0x80206913: case 0x80206916: case 0x80206919: case 0x80206979:
    case 0x8020697A: case 0x8020697F: case 0x8020698D: case 0x802450C9: case 0x802450CB: case 0x802450CC:
    case 0x8040691A: case 0x8040696E: case 0x80506979: case 0x8210698F: case 0xC00250D0: case 0xC01050CE:
    case 0xC0146974: case 0xC014697C: case 0xC0146988: case 0xC0186983: case 0xC018697F: case 0xC0206911:
    case 0xC0206921: case 0xC0206922: case 0xC0206923: case 0xC0206925: case 0xC020697E: case 0xC020698C:
    case 0xC02450CA: case 0xC1146975: case 0xC210698E: return true;
    default: return false;
    }
}

// _sys_net_lib_ioctl(s, cmd, arg): lv2's ioctl (724) for the commands libnet knows, an argument required for those
// that carry one (EINVAL otherwise); two clear parts of their argument first (0x80506979: +0x30, +0x38, +0x40, +0x48;
// 0x8040696E: +0x20, +0x30).
s32 _sys_net_lib_ioctl(PPUContext& ctx, s32 s, u32 cmd, u32 arg)
{
    if (((cmd & 0xC0000000) && !arg) || !ioctl_known(cmd)) return refuse_einval(ctx);
    u8* b = gb(ctx);
    if (cmd == 0x80506979 || cmd == 0x8040696E) {
        if (!usable(ctx, arg, cmd == 0x80506979 ? 0x4C : 0x34, "_sys_net_lib_ioctl: arg"))
            return static_cast<s32>(SYS_NET_ERROR | SYS_NET_EFAULT);
        if (cmd == 0x80506979) {
            for (const u32 off : {0x48u, 0x30u, 0x38u, 0x40u}) w32(b, arg + off, 0);
        } else {
            w32(b, arg + 0x30, 0);
            w32(b, arg + 0x20, 0);
        }
    }
    return net_call(ctx, 724, {u64(s64{s}), cmd, arg});
}

// _sys_net_lib_bnet_control(a, b, c, d): sys_net_control (723).
s32 _sys_net_lib_bnet_control(PPUContext& ctx, u32 a, s32 bb, u32 c, s32 d)
{
    return net_call(ctx, 723, {a, u64(s64{bb}), c, u64(s64{d})});
}

// The packet dumps: lv2's 717..719.
s32 sys_net_open_dump(PPUContext& ctx, s32 len, s32 flags) { return net_call(ctx, 717, {u64(s64{len}), u64(s64{flags})}); }
s32 sys_net_read_dump(PPUContext& ctx, s32 id, u32 buf, s32 len, u32 pflags)
{
    return net_call(ctx, 718, {u64(s64{id}), buf, u64(s64{len}), pflags});
}
s32 sys_net_close_dump(PPUContext& ctx, s32 id, u32 pflags) { return net_call(ctx, 719, {u64(s64{id}), pflags}); }

// The test parameters (512 bytes, flags at +8: bit 0 set and nothing in the high half, else EINVAL without errno): lv2
// keeps them on the interface ("eth0"), through ioctl on a UDP socket made for the call with a 528-byte block (the
// name, then the parameters).
constexpr s32 kEinval = static_cast<s32>(SYS_NET_ERROR | SYS_NET_EINVAL);

s32 test_param(PPUContext& ctx, u32 p, bool set)
{
    if (!p) return kEinval;
    if (!usable(ctx, p, 0x200, "sys_net_*_test_param: param")) return kEinval;
    u8* b = gb(ctx);
    const u32 flags = r32(b, p + 8);
    if ((flags & 0xFFFF0000) || !(flags & 1)) return kEinval;
    StackVar block(ctx, b, 0x210);
    std::memset(b + block.addr(), 0, 0x210);
    std::memcpy(b + block.addr(), "eth0", 4);
    if (set) std::memcpy(b + block.addr() + 16, b + p, 0x200);
    const s32 so = socket(ctx, 2, 2, 0);
    if (so < 0) return so;
    const s32 r = _sys_net_lib_ioctl(ctx, so, set ? 0x8210698F : 0xC210698E, block.addr());
    if (!set && r >= 0) std::memcpy(b + p, b + block.addr() + 16, 0x200);
    socketclose(ctx, so);
    return r;
}

s32 sys_net_get_test_param(PPUContext& ctx, u32 p) { return test_param(ctx, p, false); }
s32 sys_net_set_test_param(PPUContext& ctx, u32 p) { return test_param(ctx, p, true); }

// The network emulation's parameters are the test parameters with flags 1 (and, set, version 0x101 at +0; +2 must be 0).
s32 sys_net_get_netemu_test_param(PPUContext& ctx, u32 p)
{
    if (!p) return kEinval;
    if (!usable(ctx, p, 12, "sys_net_get_netemu_test_param: param")) return kEinval;
    w32(gb(ctx), p + 8, 1);
    return test_param(ctx, p, false);
}

s32 sys_net_set_netemu_test_param(PPUContext& ctx, u32 p)
{
    if (!p) return kEinval;
    if (!usable(ctx, p, 12, "sys_net_set_netemu_test_param: param")) return kEinval;
    u8* b = gb(ctx);
    if (ps_read16(b, p + 2) != 0) return kEinval;
    ps_write16(b, p, 0x101);
    w32(b, p + 8, 1);
    return test_param(ctx, p, true);
}

// The UDP-P2P test parameters (16 bytes): ioctl on a UDP-P2P socket made for the call.
s32 udpp2p_param(PPUContext& ctx, u32 p, u32 cmd)
{
    if (!p) return kEinval;
    const s32 so = socket(ctx, 2, 6, 0);
    if (so < 0) return so;
    const s32 r = _sys_net_lib_ioctl(ctx, so, cmd, p);
    socketclose(ctx, so);
    return r;
}

s32 sys_net_get_udpp2p_test_param(PPUContext& ctx, u32 p) { return udpp2p_param(ctx, p, 0xC01050CE); }
s32 sys_net_set_udpp2p_test_param(PPUContext& ctx, u32 p) { return udpp2p_param(ctx, p, 0x801050CD); }

// ----------------------------------------------------------------------------------------------- name servers

// netset.c's allocation (libnet f_177bc): a failure noted in the context (+2904 43, +2920 107, +2908 the size) with
// errno ENOMEM.
u32 netset_malloc(PPUContext& ctx, u32 size, u32 line)
{
    const u32 p = net_malloc(ctx, size, data(ctx) + kTagNetset, line);
    if (p) return p;
    set_report(ctx, 43, 107);
    w32(gb(ctx), context(ctx) + kCtxSetArg1, size);
    set_errno(ctx, SYS_NET_ENOMEM);
    return 0;
}

// netset.c's command text builder (libnet f_17860): one line of `words` (separated by spaces, ended by a newline)
// appended to `text` (a string in the pool, freed; 0 for none) in a new string, which is returned (0 when the pool
// refuses). "nameserver add X..." appends "nameserver X...", and nothing (the same text back) to a text of two lines
// or more, or with fewer than three words; "nameserver" with another second word appends nothing.
u32 add_line(PPUContext& ctx, u32 text, std::vector<std::string> words)
{
    u8* b = gb(ctx);
    if (!words.empty() && words[0] == "nameserver") {
        if (words.size() < 3 || words[1] != "add") return text;
        words.erase(words.begin() + 1);
    }
    const std::string old = text ? guest_str(ctx, text, "netset text").value_or("") : std::string();
    if (!old.empty() && 2 - static_cast<s32>(std::count(old.begin(), old.end(), '\n')) < 1) return text;
    u32 size = static_cast<u32>(old.size());
    for (const std::string& w : words) size += static_cast<u32>(w.size()) + 1;
    const u32 p = netset_malloc(ctx, size + 1, 0x360);
    if (!p) {
        net_free(ctx, text);
        return 0;
    }
    std::string line = old;
    for (usize i = 0; i < words.size(); i++) {
        line += words[i];
        line.push_back(i + 1 < words.size() ? ' ' : '\n');
    }
    net_free(ctx, text);
    std::memcpy(b + p, line.c_str(), line.size() + 1);
    return p;
}

// The dotted text of a network-order address, as libnet's inet_ntoa writes it (into the context).
std::string dotted(PPUContext& ctx, u32 in)
{
    return guest_str(ctx, inet_ntoa(ctx, in), "inet_ntoa").value_or("");
}

// _sce_net_add_name_server(unused, in_addr, text): "nameserver <address>" added to the text (above).
u32 _sce_net_add_name_server(PPUContext& ctx, u32, u32 in, u32 text)
{
    return add_line(ctx, text, {"nameserver", "add", dotted(ctx, in)});
}

// _sce_net_add_name_server_with_char(unused, name, text): the same with the address as text; a null or empty name adds
// nothing.
u32 _sce_net_add_name_server_with_char(PPUContext& ctx, u32, u32 name, u32 text)
{
    if (!name) return text;
    const auto n = guest_str(ctx, name, "_sce_net_add_name_server_with_char: name");
    if (!n || n->empty()) return text;
    return add_line(ctx, text, {"nameserver", "add", *n});
}

// lv2's resolver configuration set (infoctl 52) through the context (libnet f_11eec).
s32 set_resolv_conf(PPUContext& ctx, u32 text) { return infoctl(ctx, 52, {{4, text}}, {}); }

// _sce_net_set_name_server(text): the text is lv2's resolver configuration from now on; freed. Always 0.
s32 _sce_net_set_name_server(PPUContext& ctx, u32 text)
{
    set_resolv_conf(ctx, text);
    net_free(ctx, text);
    return 0;
}

// lv2's resolver configuration (infoctl 9) into the context's buffer (+2932, 128 bytes; +3060 the previous one, for
// `changed`): its address, or 0 when lv2 refuses (libnet f_125c4).
constexpr u32 kCtxResolvConf = 2932, kCtxResolvConfOld = 3060;

u32 resolv_conf(PPUContext& ctx, bool* changed)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    const u32 buf = c + kCtxResolvConf;
    { // libnet's f_11950 directly: no context around the call
        StackVar block(ctx, b, 32);
        w32(b, block.addr() + 4, buf);
        w32(b, block.addr() + 8, 0x80);
        if (lv2_syscall(ctx, 722, {9, block.addr()}) != 0) return 0;
    }
    if (changed) {
        *changed = std::memcmp(b + c + kCtxResolvConfOld, b + buf, 0x80) != 0;
        if (*changed) std::memcpy(b + c + kCtxResolvConfOld, b + buf, 0x80);
    }
    return buf;
}

// The addresses of the "nameserver " lines of the library's text and then (with `system`) lv2's: written to out (up to
// n of them, 4 bytes each) or, with no out, counted (libnet f_1a018). An address is the dots and digits after
// "nameserver ", taken by inet_aton.
s32 name_servers(PPUContext& ctx, u32 out, s32 n, bool system)
{
    u8* b = gb(ctx);
    Libnet& net = libnet();
    s32 count = 0;
    if (out && n > 0 && !usable(ctx, out, 4 * static_cast<u32>(n), "name servers: out")) return -1;
    const auto scan = [&](u32 text) {
        if (!text) return;
        const std::string t = guest_str(ctx, text, "name servers").value_or("");
        usize at = 0;
        while (at < t.size()) {
            if (t.compare(at, 11, "nameserver ") == 0) {
                at += 11;
                std::string addr;
                while (at < t.size() && (t[at] == '.' || is_digit(static_cast<u8>(t[at])))) addr.push_back(t[at++]);
                const auto a = parse_aton(addr, false);
                if (!out) {
                    if (a) count++;
                } else if (count < n && a) {
                    w32(b, out, *a);
                    out += 4;
                    count++;
                }
            }
            while (at < t.size() && t[at] != '\n') at++;
            if (at < t.size()) at++;
        }
    };
    if (!system && !net.lib_name_servers) return 0;
    scan(net.lib_name_servers);
    if (system) scan(resolv_conf(ctx, nullptr));
    return count;
}

// sys_net_get_name_server(in_addr* out, n) and _sce_net_get_name_server: the library's name servers, then lv2's.
s32 sys_net_get_name_server(PPUContext& ctx, u32 out, s32 n) { return name_servers(ctx, out, n, true); }
s32 _sce_net_get_name_server(PPUContext& ctx, u32 out, s32 n) { return name_servers(ctx, out, n, true); }

// sys_net_set_lib_name_server(primary, secondary): the library's own name servers (4-byte addresses; none: cleared).
s32 sys_net_set_lib_name_server(PPUContext& ctx, u32 primary, u32 secondary)
{
    Libnet& n = libnet();
    u8* b = gb(ctx);
    if (n.lib_name_servers) {
        net_free(ctx, n.lib_name_servers);
        n.lib_name_servers = 0;
    }
    if (!primary && !secondary) return 0;
    if ((primary && !usable(ctx, primary, 4, "sys_net_set_lib_name_server: primary")) ||
        (secondary && !usable(ctx, secondary, 4, "sys_net_set_lib_name_server: secondary")))
        return -1;
    u32 text = 0;
    if (primary) text = _sce_net_add_name_server(ctx, 0, r32(b, primary), 0);
    if (secondary) text = _sce_net_add_name_server(ctx, 0, r32(b, secondary), text);
    n.lib_name_servers = text;
    return 0;
}

// sys_net_get_lib_name_server(primary, secondary): the library's own name servers' count, the first two written where
// asked (libnet copies its stack's bytes for one it does not have: here 0).
s32 sys_net_get_lib_name_server(PPUContext& ctx, u32 primary, u32 secondary)
{
    u8* b = gb(ctx);
    StackVar found(ctx, b, 8);
    const s32 r = name_servers(ctx, found.addr(), 2, false);
    if ((primary && !usable(ctx, primary, 4, "sys_net_get_lib_name_server: primary")) ||
        (secondary && !usable(ctx, secondary, 4, "sys_net_get_lib_name_server: secondary")))
        return -1;
    if (primary) w32(b, primary, r32(b, found.addr()));
    if (secondary) w32(b, secondary, r32(b, found.addr() + 4));
    return r;
}

// sys_net_show_nameserver(): the name servers on the TTY, the library's marked.
s32 sys_net_show_nameserver(PPUContext& ctx)
{
    u8* b = gb(ctx);
    StackVar addrs(ctx, b, 16);
    const s32 count = sys_net_get_name_server(ctx, addrs.addr(), 3);
    if (count < 1) {
        tty_print(ctx, std::format("nameserver: No name server ({})\n", count));
        return 0;
    }
    s32 lib = sys_net_get_lib_name_server(ctx, 0, 0);
    for (s32 i = 0; i < count; i++) {
        const u8* a = b + addrs.addr() + 4 * static_cast<u32>(i);
        const std::string text = ntop4(a);
        if (lib > 0) {
            lib--;
            tty_print(ctx, std::format("nameserver: {:<16} (lib)\n", text));
        } else {
            tty_print(ctx, std::format("nameserver: {:<16}\n", text));
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------- routes

// netset.c's helpers around libnet's own calls, each noting its failure in the context.

// libnet f_17d38: socket(), a failure noted (45, line 82, the arguments).
s32 netset_socket(PPUContext& ctx, s32 family, s32 type, s32 protocol)
{
    const s32 so = socket(ctx, family, type, protocol);
    if (so < 0) {
        u8* b = gb(ctx);
        set_report(ctx, 45, 82);
        w32(b, context(ctx) + kCtxSetArg1, static_cast<u32>(family));
        w32(b, context(ctx) + kCtxSetArg2, static_cast<u32>(type));
        w32(b, context(ctx) + kCtxSetArg3, static_cast<u32>(protocol));
    }
    return so;
}

// libnet f_17e00: _sys_net_lib_sysctl(mib, 6, old, oldlenp, 0, 0), a failure noted (46, line 129, mib[0], mib[1],
// mib[4]).
s32 netset_sysctl(PPUContext& ctx, u32 mib, u32 old, u32 oldlenp)
{
    const s32 r = _sys_net_lib_sysctl(ctx, mib, 6, old, oldlenp, 0, 0);
    if (r < 0) {
        u8* b = gb(ctx);
        set_report(ctx, 46, 129);
        w32(b, context(ctx) + kCtxSetArg1, r32(b, mib));
        w32(b, context(ctx) + kCtxSetArg2, r32(b, mib + 4));
        w32(b, context(ctx) + kCtxSetArg3, r32(b, mib + 16));
    }
    return r;
}

// libnet f_199b0: _sys_net_lib_ioctl, a failure noted (42, line 94, the command and its high half).
s32 netset_ioctl(PPUContext& ctx, s32 so, u32 cmd, u32 arg)
{
    const s32 r = _sys_net_lib_ioctl(ctx, so, cmd, arg);
    if (r < 0) {
        u8* b = gb(ctx);
        set_report(ctx, 42, 94);
        w32(b, context(ctx) + kCtxSetArg1, cmd);
        w32(b, context(ctx) + kCtxSetArg2, cmd >> 16);
    }
    return r;
}

// The routing table's dump request: {CTL_NET, PF_ROUTE, 0, 0, NET_RT_DUMP, 0}, its length word before it.
struct RouteDump {
    StackVar v;
    u32 len, mib;
    RouteDump(PPUContext& ctx, u8* b) : v(ctx, b, 0x20), len(v.addr()), mib(v.addr() + 4)
    {
        const u32 words[6] = {4, 17, 0, 0, 1, 0};
        for (u32 i = 0; i < 6; i++) w32(b, mib + 4 * i, words[i]);
    }
};

// What lv2 would have to list for these paths to go on: with no link it has no route (D-523).
[[noreturn]] void routes_listed(std::string_view fn)
{
    PS_FATAL(kChan, "{}: lv2 listed routes; with no link it has none (D-523, D-539)", fn);
}

// libnet f_17eb4: every route deleted: each message of the routing table's dump sent back on a routing socket as
// RTM_DELETE with the next sequence number. 0, or -1 (the socket, lv2's dump or the pool refused; the length sent when
// a send falls short).
s32 flush_routes(PPUContext& ctx)
{
    u8* b = gb(ctx);
    const s32 fd = netset_socket(ctx, 17, 3, 0);
    if (fd < 0) return -1;
    RouteDump d(ctx, b);
    if (netset_sysctl(ctx, d.mib, 0, d.len) < 0) {
        socketclose(ctx, fd);
        return -1;
    }
    const u32 buf = netset_malloc(ctx, r32(b, d.len), 0x181);
    if (!buf) {
        socketclose(ctx, fd);
        return -1;
    }
    s32 r = -1;
    if (netset_sysctl(ctx, d.mib, buf, d.len) >= 0) {
        r = 0;
        const u32 end = buf + r32(b, d.len);
        for (u32 m = buf; m < end;) {
            const u32 size = ps_read16(b, m);
            if (!size) routes_listed("_sce_net_flush_route");
            b[m + 3] = 2; // RTM_DELETE
            w32(b, m + 20, ++libnet().route_seq);
            const s32 sent = send(ctx, fd, m, size, 0);
            if (sent < static_cast<s32>(size)) {
                r = sent;
                break;
            }
            m += size;
        }
    }
    net_free(ctx, buf);
    socketclose(ctx, fd);
    return r;
}

s32 _sce_net_flush_route(PPUContext& ctx)
{
    flush_routes(ctx);
    return 0;
}

// sys_net_get_routing_table_af(af, sys_net_route_t* out, n): the routes of family af (0: all), up to n written (212
// bytes each) or, with no out, counted; -errno when lv2's dump or the pool refuses (libnet keeps the buffer when the
// second dump fails). With no link lv2 lists none: 0.
s32 sys_net_get_routing_table_af(PPUContext& ctx, s32, u32, s32)
{
    u8* b = gb(ctx);
    RouteDump d(ctx, b);
    const auto fail = [&]() { return -static_cast<s32>(r32(b, context(ctx) + kCtxErrno)); };
    if (netset_sysctl(ctx, d.mib, 0, d.len) < 0) return fail();
    const u32 buf = netset_malloc(ctx, r32(b, d.len), 2303);
    if (!buf) return fail();
    if (netset_sysctl(ctx, d.mib, buf, d.len) < 0) return fail();
    if (r32(b, d.len)) routes_listed("sys_net_get_routing_table_af");
    net_free(ctx, buf);
    return 0;
}

// sys_net_show_route(): the IPv4 routes (up to 10) on the TTY; with none, the count.
s32 sys_net_show_route(PPUContext& ctx)
{
    StackVar table(ctx, gb(ctx), 2128);
    const s32 count = sys_net_get_routing_table_af(ctx, 2, table.addr(), 10);
    if (count < 1) {
        tty_print(ctx, std::format("route: No routing table ({})\n", count));
        return 0;
    }
    routes_listed("sys_net_show_route");
}

// libnet f_181d8: a sockaddr_in (16 bytes) for the address text at `text` (guest): "default" 0.0.0.0,
// "255.255.255.255" and "0xffffffff" all ones, else inet_addr's; 0.0.0.1 stands for the address text at `extra` (when
// given). -1 (noted: 1, line 217; errno EINVAL) for a text that is no address.
s32 route_address(PPUContext& ctx, u32 sa, u32 text, u32 extra)
{
    u8* b = gb(ctx);
    std::memset(b + sa, 0, 16);
    b[sa] = 16;
    b[sa + 1] = 2;
    const std::string t = guest_str(ctx, text, "route address").value_or("");
    if (t == "default") return 0;
    if (t == "255.255.255.255" || t == "0xffffffff") {
        w32(b, sa + 4, 0xFFFFFFFF);
        return 0;
    }
    u32 a = inet_addr(ctx, text);
    w32(b, sa + 4, a);
    if (a == 1) {
        if (!extra) return 0;
        a = inet_addr(ctx, extra);
        w32(b, sa + 4, a);
    }
    if (r32(b, sa + 4) != 0xFFFFFFFF) return 0;
    set_report(ctx, 1, 217);
    set_errno(ctx, SYS_NET_EINVAL);
    return -1;
}

// libnet f_18364: the word at t[at] (up to 100 characters, ended by white space) as an IPv4 address into `sa`, through a
// 100-byte pool buffer: the characters taken (the word and the space after it), or -1 (a word the end of the text
// ends, an address it is not, or a word longer than 100 characters: noted 1, line 237, errno EINVAL).
s32 route_word(PPUContext& ctx, u32 sa, const std::string& t, usize at, u32 extra)
{
    u8* b = gb(ctx);
    const u32 word = netset_malloc(ctx, 100, 0xE6);
    if (!word) return -1;
    const auto ch = [&](usize i) { return i < t.size() ? static_cast<u8>(t[i]) : u8{0}; };
    u32 n = 0;
    u8 next = 0;
    bool space = false;
    for (; n < 100; n++) {
        const u8 c = ch(at + n);
        if (c == 0) break;
        if (is_space(c)) {
            space = true;
            break;
        }
        b[word + n] = c;
    }
    if (!space && n == 100) {
        next = ch(at + 100);
        space = is_space(next);
    }
    if (!space) {
        if (next != 0) {
            set_report(ctx, 1, 0xED);
            set_errno(ctx, SYS_NET_EINVAL);
        }
        net_free(ctx, word);
        return -1;
    }
    b[word + n] = 0; // (at n == 100, one byte past libnet's buffer, as libnet writes it)
    const s32 r = route_address(ctx, sa, word, extra);
    net_free(ctx, word);
    return r < 0 ? -1 : static_cast<s32>(n) + 1;
}

// netset.c's route command interpreter (libnet f_19010): each "route ..." line of `text` (other lines are skipped):
// "route add ...", "route del ...", "route flushall" (flush_routes), or "route ..." (an add). Its words, in any order:
// "-net " (not a host route), "-host ", "-inet ", "gw <address>", "netmask <address>", "-prefixlen <bits>" (0..32), and
// addresses taken as the destination, the gateway and the netmask in that order (no netmask: 0.0.0.0). Then a routing
// message (version 3, RTM_ADD or RTM_DELETE, flags UP|STATIC, REJECT with `reject`, HOST, GATEWAY with a gateway, the
// next sequence number, the destination, gateway and netmask it has, each rounded to 8 bytes) goes to a routing socket.
// A word it cannot take abandons the line. The three addresses live in an 84-byte pool area for the whole text. 0, or -1
// when the pool refuses that area.
s32 route_command(PPUContext& ctx, u32 text, s32 reject, u32 extra)
{
    u8* b = gb(ctx);
    const u32 work = netset_malloc(ctx, 84, 418);
    if (!work) return -1;
    const u32 dst = work, gw = work + 28, mask = work + 56;
    const std::string t = text ? guest_str(ctx, text, "route command").value_or("") : std::string();
    usize p = 0;
    const auto at = [&](usize i) { return i < t.size() ? static_cast<u8>(t[i]) : u8{0}; };
    const auto starts = [&](usize i, std::string_view w) { return i <= t.size() && t.compare(i, w.size(), w) == 0; };
    const auto skip_line = [&]() {
        while (at(p) != 0 && at(p) != '\n') p++;
        if (at(p) == '\n') p++;
    };
    while (at(p) != 0) {
        if (!starts(p, "route ")) {
            skip_line();
            continue;
        }
        p += 6;
        u32 flags = 0; // bit 0 delete, 1 host, 2 destination, 3 gateway, 4 netmask
        if (starts(p, "add ")) {
            p += 4;
        } else if (starts(p, "del ")) {
            p += 4;
            flags = 1;
        } else if (starts(p, "flushall")) {
            flush_routes(ctx);
            skip_line();
            continue;
        }
        bool build = false;
        for (;;) {
            if (starts(p, "-net ")) {
                while (starts(p, "-net ")) {
                    p += 5;
                    flags &= ~2u;
                }
                continue;
            }
            if (starts(p, "-host ")) {
                p += 6;
                flags |= 2;
                continue;
            }
            if (starts(p, "-inet ")) {
                p += 6;
                continue;
            }
            if (starts(p, "gw ")) {
                p += 3;
                const s32 r = route_word(ctx, gw, t, p, extra);
                if (r < 1) break;
                p += static_cast<usize>(r);
                flags |= 8;
                continue;
            }
            if (starts(p, "netmask ")) {
                p += 8;
                const s32 r = route_word(ctx, mask, t, p, extra);
                if (r < 1) break;
                p += static_cast<usize>(r);
                flags |= 0x10;
                continue;
            }
            if (starts(p, "-prefixlen ")) {
                const usize q = p + 11;
                usize k = 0;
                u32 bits = 0;
                u8 c = at(q);
                while (c != 0 && is_digit(c)) {
                    bits = bits * 10 + c - '0';
                    c = at(q + ++k);
                }
                if (!is_space(c)) {
                    set_report(ctx, 3, 265);
                    set_errno(ctx, SYS_NET_EINVAL);
                    p = q;
                    break;
                }
                std::memset(b + mask, 0, 28);
                b[mask + 1] = 2;
                b[mask] = 16;
                p = q + k + 1;
                s32 left = static_cast<s32>(bits);
                if (left > 32) {
                    set_report(ctx, 4, 284);
                    set_errno(ctx, SYS_NET_EINVAL);
                    break;
                }
                u32 m = mask + 4;
                while (left > 7) {
                    b[m++] = 0xFF;
                    left -= 8;
                }
                if (left > 0) b[m] = static_cast<u8>(0xFF << (8 - left));
                flags |= 0x10;
                continue;
            }
            // An address: the destination, the gateway, the netmask, whichever is still missing.
            if (!(flags & 4)) {
                if (const s32 r = route_word(ctx, dst, t, p, extra); r > 0) {
                    p += static_cast<usize>(r);
                    flags |= 4;
                    continue;
                }
            }
            if (!(flags & 8)) {
                if (const s32 r = route_word(ctx, gw, t, p, extra); r > 0) {
                    p += static_cast<usize>(r);
                    flags |= 8;
                    continue;
                }
            }
            if (!(flags & 0x10)) {
                if (const s32 r = route_word(ctx, mask, t, p, extra); r > 0) {
                    p += static_cast<usize>(r);
                    flags |= 0x10;
                    continue;
                }
                flags |= 0x10;
                std::memset(b + mask, 0, 16);
                b[mask] = 16;
                b[mask + 1] = 2;
            }
            build = true;
            break;
        }
        if (build) {
            u32 rtflags = reject ? 0x809 : 0x801;
            if (flags & 2) rtflags |= 4;
            u32 addrs = (flags & 4) ? 1 : 0;
            if (flags & 8) {
                rtflags |= 2;
                addrs |= 2;
            }
            if (flags & 0x10) addrs |= 4;
            const s32 so = netset_socket(ctx, 17, 3, 0);
            if (so >= 0) {
                const u32 msg = netset_malloc(ctx, 632, 333);
                if (msg) {
                    std::memset(b + msg, 0, 632);
                    b[msg + 2] = 3;
                    b[msg + 3] = (flags & 1) ? 2 : 1;
                    w32(b, msg + 8, rtflags);
                    w32(b, msg + 20, ++libnet().route_seq);
                    w32(b, msg + 12, addrs);
                    u32 q = msg + 120;
                    for (const auto& [bit, sa] : {std::pair{1u, dst}, std::pair{2u, gw}, std::pair{4u, mask}}) {
                        if (!(addrs & bit)) continue;
                        const u32 len = b[sa];
                        const u32 size = len ? ((len - 1) | 7) + 1 : 8;
                        std::memmove(b + q, b + sa, size);
                        q += size;
                    }
                    const u32 total = q - msg;
                    ps_write16(b, msg, static_cast<u16>(total));
                    if (send(ctx, so, msg, total, 0) < 0) set_report(ctx, 44, 360);
                }
                net_free(ctx, msg);
                socketclose(ctx, so);
            }
        }
        skip_line();
    }
    net_free(ctx, work);
    return 0;
}

// _sce_net_set_default_gateway(ifname, in_addr gateway, pppoe): "route add default gw <gateway>" (the interface is not
// used; libnet names pppoe0 for a PPPoE link). Always 0.
s32 _sce_net_set_default_gateway(PPUContext& ctx, u32, u32 gateway, s32 pppoe)
{
    const u32 text = add_line(ctx, 0, {"route", "add", "default", "gw", dotted(ctx, gateway)});
    if (!text) log_call_site(ctx, gb(ctx), "_sce_net_set_default_gateway: no text (the pool refused it); libnet reads "
                                           "address 0 here");
    else route_command(ctx, text, 0, static_cast<u32>(pppoe));
    net_free(ctx, text);
    return 0;
}

// libnet f_19c38: an interface's address, netmask and broadcast or destination address ("255." and on: broadcast) set
// with ioctls on a UDP socket, each from its text (guest strings; none: left alone). 0, or -1.
s32 interface_address(PPUContext& ctx, u32 ifname, u32 address, u32 netmask, u32 other)
{
    u8* b = gb(ctx);
    if (!ifname) {
        set_errno(ctx, SYS_NET_EINVAL);
        return -1;
    }
    const s32 so = netset_socket(ctx, 2, 2, 0);
    if (so < 0) return -1;
    StackVar ifr(ctx, b, 32); // the name (16 bytes), then a sockaddr_in
    const std::string name = guest_str(ctx, ifname, "interface name").value_or("");
    if (name.size() > 31) log_call_site(ctx, b, "interface name over 31 characters: libnet's strcpy runs past its stack");
    std::memcpy(b + ifr.addr(), name.c_str(), std::min<usize>(name.size(), 31));
    const u32 sa = ifr.addr() + 16;
    const auto apply = [&](u32 text, u32 cmd) { return route_address(ctx, sa, text, 0) >= 0 && netset_ioctl(ctx, so, cmd, ifr.addr()) >= 0; };
    bool ok = true;
    if (address) ok = apply(address, 0x8020690C);
    if (ok && netmask) ok = apply(netmask, 0x80206916);
    if (ok && other) {
        const std::string t = guest_str(ctx, other, "interface address").value_or("");
        ok = apply(other, t.compare(0, 4, "255.") == 0 ? 0x80206913 : 0x8020690E);
    }
    socketclose(ctx, so);
    return ok ? 0 : -1;
}

// _sce_net_set_ip_and_mask(ifname, in_addr address, in_addr netmask): the interface's address and netmask.
s32 _sce_net_set_ip_and_mask(PPUContext& ctx, u32 ifname, u32 address, u32 netmask)
{
    u8* b = gb(ctx);
    StackVar text(ctx, b, 32);
    const std::string a = dotted(ctx, address), m = dotted(ctx, netmask);
    std::memcpy(b + text.addr(), a.c_str(), a.size() + 1);
    std::memcpy(b + text.addr() + 16, m.c_str(), m.size() + 1);
    return interface_address(ctx, ifname, text.addr(), text.addr() + 16, 0);
}

// ------------------------------------------------------------------------------------------------- interfaces

// _sys_net_lib_if_nametoindex(name): the interface's index; with no link lv2 lists none: 0, errno ENXIO.
u32 _sys_net_lib_if_nametoindex(PPUContext& ctx, u32)
{
    StackVar ifap(ctx, gb(ctx), 8);
    if (getifaddrs(ctx, ifap.addr()) < 0) return 0;
    set_errno(ctx, 6); // ENXIO
    return 0;
}

// libnet's resolver cache flush (f_a9a4, sys_net_if_ctl 656): every cached answer dropped. pspocket's resolver keeps
// no cache yet (D-539): nothing to drop.
void resolver_cache_flush(PPUContext&) {}

// sys_net_if_ctl(index, cmd, buf, len) (D-539): libnet's interface and system reports. Index 0 with a 4-byte buf:
// 640/641 lv2's free and lowest free memory (infoctl 8), 642/643 libnet's pool (free, lowest free), 644 lv2's packet
// count (infoctl 10); 656 (no buf, index or len) flushes the resolver cache; 768 and 769 ask the wireless chip (lv2's
// eurus command -1, 16 bytes: a word back; 153, 62 bytes: the text from its first digit up to a '(' back, NUL-ended,
// len above 49); 16 (len above 19) looks for a route to the address in buf and, with none (ENOENT) and bit 0 of buf+4
// clear, pings it (an ICMP echo on a raw socket; bit 0 of buf+8 set when that fails); a negative command at index 0 is
// sys_net_control (buf cleared first unless the command has 0x81000000 set). Anything else names an interface by its
// index among those with an IPv4 address: with no link lv2 lists none, EINVAL. Errors are errno | 0x80010200.
s32 sys_net_if_ctl(PPUContext& ctx, s32 index, s32 cmd, u32 buf, s32 len)
{
    u8* b = gb(ctx);
    const auto coded_errno = [&]() { return static_cast<s32>(r32(b, context(ctx) + kCtxErrno) | SYS_NET_ERROR); };
    const auto einval = [&]() {
        set_errno(ctx, SYS_NET_EINVAL);
        return coded_errno();
    };
    const auto put4 = [&](u32 v) {
        if (!usable(ctx, buf, 4, "sys_net_if_ctl: buf")) return coded_errno();
        w32(b, buf, v);
        return 0;
    };
    const bool report4 = buf && len == 4 && index == 0;
    if (cmd > 643) {
        if (cmd == 656) {
            if (buf || index || len) return einval();
            resolver_cache_flush(ctx);
            return 0;
        }
        if (cmd == 768) {
            if (!report4) return einval();
            StackVar block(ctx, b, 16);
            const s32 r = net_call(ctx, 726, {u64(s64{-1}), block.addr(), 16});
            if (!usable(ctx, buf, 4, "sys_net_if_ctl: buf")) return coded_errno();
            w32(b, buf, r32(b, block.addr() + 12));
            return r;
        }
        if (cmd == 769) {
            if (!buf || static_cast<u32>(len) <= 49 || index) return einval();
            StackVar block(ctx, b, 64);
            const s32 r = net_call(ctx, 726, {153, block.addr(), 62});
            if (r < 0) return r;
            const u32 text = block.addr() + 12;
            u32 start = 0, count = 0;
            bool found = false;
            for (u32 i = 0; i < 50; i++) {
                const u8 c = b[text + i];
                if (!found) {
                    if (is_digit(c)) {
                        found = true;
                        start = i;
                        count = 1;
                    }
                } else {
                    if (c == '(') break;
                    count++;
                }
            }
            if (!usable(ctx, buf, count + 1, "sys_net_if_ctl: buf")) return coded_errno();
            if (found) std::memmove(b + buf, b + text + start, count);
            b[buf + count] = 0;
            return r;
        }
        if (cmd == 644) {
            if (!report4) return einval();
            return put4(static_cast<u32>(infoctl(ctx, 10, {}, {})));
        }
    } else if (cmd >= 642) {
        if (!report4) return einval();
        const Libnet& n = libnet();
        return put4(cmd == 642 ? n.free : n.min_free);
    } else if (cmd == 16) {
        if (!buf || static_cast<u32>(len) <= 19 || index) return einval();
        if (!usable(ctx, buf, 20, "sys_net_if_ctl: buf")) return coded_errno();
        w32(b, buf + 8, r32(b, buf + 4));
        if (sys_net_get_routing_table_af(ctx, 2, 0, 0) > 0) routes_listed("sys_net_if_ctl");
        set_errno(ctx, 2); // ENOENT: no route
        if (r32(b, buf + 4) & 1) return coded_errno();
        // A ping: an ICMP echo (type 8, id and sequence 12345) on a raw socket.
        const s32 so = socket(ctx, 2, 3, 1);
        if (so < 0) {
            w32(b, buf + 8, r32(b, buf + 8) | 1);
            return coded_errno();
        }
        const u32 echo = net_malloc(ctx, 9, data(ctx) + kTagNetset, 1796);
        bool failed = true;
        if (echo) {
            std::memset(b + echo, 0, 9);
            StackVar sa(ctx, b, 16);
            b[sa.addr() + 1] = 2;
            w32(b, sa.addr() + 4, r32(b, buf));
            b[echo] = 8;
            b[echo + 1] = static_cast<u8>(r32(b, buf + 4) & 1);
            ps_write16(b, echo + 6, 12345);
            ps_write16(b, echo + 4, 12345);
            u32 sum = 0;
            for (u32 k = 0; k < 8; k += 2) sum += ps_read16(b, echo + k);
            s32 v = static_cast<s32>((u32{b[echo + 8]} << 8) + sum);
            v = (v >> 16) + (v & 0xFFFF);
            v += v >> 16;
            ps_write16(b, echo + 2, static_cast<u16>(~v));
            failed = sendto(ctx, so, echo, 9, 0, sa.addr(), 16) < 0;
        }
        net_free(ctx, echo);
        socketclose(ctx, so);
        if (failed) w32(b, buf + 8, r32(b, buf + 8) | 1);
        return coded_errno();
    } else if (cmd >= 640) {
        if (!report4) return einval();
        return put4(static_cast<u32>(infoctl(ctx, 8, {{0, u32(cmd - 640)}}, {})));
    }
    // An interface by its index.
    if (index == 0 && cmd < 0) {
        if ((static_cast<u32>(cmd) & 0x81000000) != 0x81000000 && buf) {
            if (!usable(ctx, buf, static_cast<u32>(len), "sys_net_if_ctl: buf")) return coded_errno();
            std::memset(b + buf, 0, static_cast<u32>(len));
        }
        const s32 r = _sys_net_lib_bnet_control(ctx, 0, cmd, buf, len);
        return r < 0 ? coded_errno() : r;
    }
    StackVar ifap(ctx, b, 8);
    if (interfaces(ctx, ifap.addr()) != 0) return coded_errno();
    return einval(); // (none listed: getifaddrs stops on one)
}

// ------------------------------------------------------------------------------------------------- netsets

// A netset (netset.c, 244 bytes in the pool): +0 next, +4 previous, +8 its ID, +0xC if_up's timeout (ms), +0x10 busy
// in if_up, +0x14 state, +0x18 status, then its configuration: strings in the pool at the offsets of libnet's key table
// (kNetsetKeys), the flags dhcp (+0x50), dhcp_host_name_null_terminated (+0x51), want.dns1_nego (+0x70),
// want.dns2_nego (+0x71) and pppoe (+0x74), mtu (+0x80), and binary copies of escaped values with their lengths (bssid
// +0xA4/+0xA8, scan_channel_list +0xB0/+0xB4, supported_rate +0xBC/+0xC0, cap_info +0xC8/+0xCC, auth_wlan's key
// +0xE0/+0xE4).
namespace ns {
constexpr u32 next = 0, prev = 4, id = 8, timeout = 0xC, busy = 0x10, state = 0x14, status = 0x18, type = 0x20,
              resolv = 0x2C, ifname = 0x3C, dhcp = 0x50, pppoe = 0x74, size = 244;
}

// libnet's key table: each configuration key's place in a netset (also the order netset.c frees them in).
constexpr std::pair<u32, std::string_view> kNetsetKeys[] = {
    {0x1C, "my_name"}, {0x20, "type"}, {0x24, "address"}, {0x28, "netmask"}, {0x30, "search"},
    {0x34, "nameserver"}, {0x2C, "resolv_conf"}, {0x38, "route"}, {0x40, "vendor"}, {0x44, "product"},
    {0x48, "phy_config"}, {0x4C, "dhcp_host_name"}, {0x54, "http_proxy"}, {0x58, "device_type"}, {0x5C, "auth_name"},
    {0x60, "auth_key"}, {0x64, "allow.auth"}, {0x78, "pppoe_service_name"}, {0x7C, "pppoe_ac_name"},
    {0x84, "ipversion"}, {0x88, "ipv6.addr"}, {0x8C, "ssid"}, {0xA0, "bssid"}, {0xAC, "scan_channel_list"},
    {0xB8, "supported_rate"}, {0xC4, "cap_info"}, {0xD0, "op_mode"}, {0xD4, "channel"}, {0xD8, "auth_wlan_type"},
    {0xDC, "auth_wlan_key"}, {0xE8, "auth_8021x_type"}, {0xEC, "auth_8021x_user"}, {0xF0, "auth_8021x_pass"}};

// The values sys_netset_get_key_value answers (netset.c's statics: the first netset's, copied when one closes or comes
// up), and where each comes from in a netset.
constexpr std::array<std::string_view, 13> kKeyNames = {"http_proxy", "ssid", "bssid", "scan_channel_list",
    "supported_rate", "cap_info", "op_mode", "channel", "auth_wlan_type", "auth_wlan_key", "auth_8021x_type",
    "auth_8021x_user", "auth_8021x_pass"};
constexpr std::array<u32, 13> kKeyFields = {0x54, 0x8C, 0xA0, 0xAC, 0xB8, 0xC4, 0xD0, 0xD4, 0xD8, 0xDC, 0xE8, 0xEC, 0xF0};

// libnet's strdup (misc.c: the pool, line 362): 0 when the pool refuses.
u32 net_strdup(PPUContext& ctx, u32 text)
{
    u8* b = gb(ctx);
    const std::string t = guest_str(ctx, text, "strdup").value_or("");
    const u32 p = net_malloc(ctx, static_cast<u32>(t.size()) + 1, data(ctx) + kTagMisc, 362);
    if (p) std::memcpy(b + p, t.c_str(), t.size() + 1);
    return p;
}

// netset.c's value replacement (libnet f_1cab0): the old string freed, a copy of the new one (none: 0).
u32 replace_value(PPUContext& ctx, u32 old, u32 text)
{
    if (old) net_free(ctx, old);
    return text ? net_strdup(ctx, text) : 0;
}

// ... and of a binary value (f_1cb0c): `len` bytes copied (netset.c, line 836); with no data or length, a string.
u32 replace_bytes(PPUContext& ctx, u32 old, u32 bytes, s32 len)
{
    if (!len || !bytes) return replace_value(ctx, old, bytes);
    if (old) net_free(ctx, old);
    const u32 p = net_malloc(ctx, static_cast<u32>(len), data(ctx) + kTagNetset, 836);
    if (p) std::memmove(gb(ctx) + p, gb(ctx) + bytes, static_cast<u32>(len));
    return p;
}

// A netset freed (f_171b0): its values in the key table's order, then itself (the binary copies are not freed: libnet
// leaks them).
void free_netset(PPUContext& ctx, u32 set)
{
    u8* b = gb(ctx);
    for (const auto& [off, name] : kNetsetKeys) net_free(ctx, r32(b, set + off));
    net_free(ctx, set);
}

// The netset with this ID (f_17580), or 0 (noted: 5, line 1084; errno EINVAL).
u32 find_netset(PPUContext& ctx, u32 id)
{
    u8* b = gb(ctx);
    for (u32 set = libnet().sets_head; set; set = r32(b, set + ns::next))
        if (r32(b, set + ns::id) == id) return set;
    set_report(ctx, 5, 1084);
    set_errno(ctx, SYS_NET_EINVAL);
    return 0;
}

s32 errno_negated(PPUContext& ctx) { return -static_cast<s32>(r32(gb(ctx), context(ctx) + kCtxErrno)); }

// libnet's sscanf(text, "%d", out) (f_dc2c): the value written as the digits come (decimal; 0 with none, no sign).
void scan_decimal(PPUContext& ctx, u32 text, u32 out)
{
    u8* b = gb(ctx);
    w32(b, out, 0);
    const std::string t = guest_str(ctx, text, "sscanf").value_or("");
    for (const char ch : t) {
        if (!is_digit(static_cast<u8>(ch))) break;
        w32(b, out, r32(b, out) * 10 + static_cast<u32>(ch - '0'));
    }
}

// The hex digit at c (libnet's ctype: digits, a-f, A-F), or -1.
s32 hex_digit(u8 c)
{
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 87;
    if (c >= 'A' && c <= 'F') return c - 55;
    return -1;
}

// sys_netset_open's parser (f_1cbc8): the text's lines, each up to five tokens (white space between them; '#' or '\r'
// where a token would start ends the line; a line the text's end cuts is dropped). A token takes "quoted" spans, and
// escapes: \ooo (octal, up to three digits), \xHH, \a \b \f \n \r \t \v, \( ... \) (hex byte pairs), any other
// character as itself; bytes from escapes are counted per token (the length of a binary value). Its 264-byte state in
// the pool (+0 the cursor, +4 the tokens, +260 where the next byte goes) holds at most 256 bytes of a line's tokens.
// Each line's first token names a key ('-' first: clear it): dhcp, dhcp_host_name_null_terminated, pppoe,
// want.dns1_nego, want.dns2_nego (flags), mtu (a number), op_mode, channel (strings), auth_wlan (type, key and its
// bytes), bssid, scan_channel_list, supported_rate, cap_info (a string and its bytes), auth_8021x (type, user,
// password), or a key of the table: search and nameserver lines go into the resolver text (+0x2C), route and ipv6.addr
// lines into their own text, others take their first argument. Unknown keys are skipped. The new netset's ID, or
// -errno.
s32 netset_parse(PPUContext& ctx, u32 text)
{
    u8* b = gb(ctx);
    Libnet& n = libnet();
    const u32 st = netset_malloc(ctx, 264, 1048);
    if (!st) return errno_negated(ctx);
    w32(b, st, text);
    if (!text) {
        set_report(ctx, 26, 901);
        set_errno(ctx, SYS_NET_EINVAL);
        const s32 r = errno_negated(ctx);
        net_free(ctx, st);
        return r;
    }
    const u32 set = netset_malloc(ctx, ns::size, 905);
    if (!set) {
        const s32 r = errno_negated(ctx);
        net_free(ctx, st);
        return r;
    }
    std::memset(b + set, 0, ns::size);
    const auto t = guest_str(ctx, text, "sys_netset_open: text");
    const u32 text_end = text + static_cast<u32>(t ? t->size() : 0); // the NUL
    const auto ch = [&](u32 a) -> u8 { return a <= text_end ? b[a] : u8{0}; };
    u32 cur = text;
    const u32 buf = st + 4, buf_end = st + 260;
    u32 wp = buf;
    const auto save = [&]() {
        w32(b, st, cur);
        w32(b, st + 260, wp);
    };
    const auto fail = [&](u32 error, u32 line) {
        save();
        set_report(ctx, error, line);
        set_errno(ctx, SYS_NET_EINVAL);
        free_netset(ctx, set);
        const s32 r = errno_negated(ctx);
        net_free(ctx, st);
        return r;
    };
    std::array<u32, 5> tok{};
    std::array<s32, 5> counts{};
    for (;;) { // a line
        wp = buf;
        u32 count = 0;
        counts = {};
        bool line_done = false; // the line's tokens are complete: process them
        while (!line_done) {    // a token
            tok[count] = wp;
            counts[count] = 0;
            u8 c = ch(cur);
            if (c == 0) { // the text's end: the netset is made (this line's tokens are dropped)
                save();
                w32(b, set + ns::id, n.next_set++);
                w32(b, set + ns::prev, n.sets_tail);
                if (n.sets_tail) w32(b, n.sets_tail + ns::next, set);
                else n.sets_head = set;
                w32(b, set + ns::next, 0);
                n.sets_tail = set;
                const s32 id = static_cast<s32>(r32(b, set + ns::id));
                net_free(ctx, st);
                return id;
            }
            const auto skip_line = [&]() { // from the character at cur
                for (;;) {
                    cur++;
                    const u8 d = ch(cur);
                    if (d == 0) return;
                    if (d == '\n') {
                        cur++;
                        return;
                    }
                }
            };
            bool start = false;
            if (c == '\n') {
                cur++;
            } else if (c == '#') {
                skip_line();
            } else if (is_space(c)) {
                if (c == '\r') {
                    skip_line();
                } else {
                    for (;;) {
                        cur++;
                        const u8 d = ch(cur);
                        if (d == '#' || d == '\r') {
                            skip_line();
                            break;
                        }
                        if (d == 0 || !is_space(d)) {
                            start = true;
                            break;
                        }
                        if (d == '\n') {
                            cur++;
                            break;
                        }
                    }
                }
            } else {
                start = true;
            }
            if (!start) { // the line ended: its tokens, if any
                if (count >= 1) line_done = true;
                else {
                    wp = buf;
                    counts = {};
                }
                if (!line_done) continue;
                break;
            }
            // A token from cur.
            bool quote = false, hex = false;
            u8 last = 0;
            const auto store = [&](u8 v) -> bool {
                if (wp >= buf_end) return false;
                b[wp++] = v;
                return true;
            };
            const auto hex_pair = [&]() { // up to two hex digits at cur
                u32 v = 0;
                for (u32 k = 0; k < 2; k++) {
                    const s32 d = hex_digit(ch(cur));
                    if (d < 0) break;
                    v = v * 16 + static_cast<u32>(d);
                    cur++;
                }
                return static_cast<u8>(v);
            };
            for (;;) {
                c = ch(cur);
                cur++;
                last = c;
                if (c == 0 || c == '\n') break;
                if (!quote && is_space(c)) break;
                if (c == '"') {
                    quote = !quote;
                    continue;
                }
                if (c == '\r') continue;
                u8 v = c;
                bool escaped = false;
                if (c == '\\') {
                    const u8 e = ch(cur);
                    if (static_cast<u8>(e - '0') <= 7) {
                        u32 o = static_cast<u32>(e - '0');
                        cur++;
                        for (u32 k = 0; k < 2 && static_cast<u8>(ch(cur) - '0') <= 7; k++) o = o * 8 + static_cast<u32>(ch(cur++) - '0');
                        v = static_cast<u8>(o);
                    } else if (e == 'x' || e == 'X') {
                        cur++;
                        v = hex_pair();
                    } else {
                        cur++;
                        if (e == '(') {
                            if (hex) return fail(28, 780);
                            hex = true;
                            continue;
                        }
                        if (e == ')') {
                            if (!hex) return fail(28, 787);
                            hex = false;
                            continue;
                        }
                        switch (e) {
                        case 'a': v = 7; break;
                        case 'b': v = 8; break;
                        case 'f': v = 12; break;
                        case 'n': v = 10; break;
                        case 'r': v = 13; break;
                        case 't': v = 9; break;
                        case 'v': v = 11; break;
                        default: v = e; break;
                        }
                    }
                    escaped = true;
                } else if (hex) {
                    cur--;
                    v = hex_pair();
                    escaped = true;
                }
                if (escaped) counts[count]++;
                if (!store(v)) return fail(20, 750);
            }
            if ((last == 0 || last == '\n') && quote) return fail(21, 799);
            if (hex) return fail(28, 803);
            if (!store(0)) return fail(20, 809);
            if (last == 0 || last == '\n') cur--;
            if (count == 4) {
                count = 5;
                line_done = true;
            } else {
                count++;
            }
        }
        // The line's key.
        std::array<std::string, 5> words;
        for (u32 i = 0; i < count; i++) words[i] = guest_str(ctx, tok[i], "netset token").value_or("");
        const bool neg = !words[0].empty() && words[0][0] == '-';
        const std::string key = neg ? words[0].substr(1) : words[0];
        const auto flag = [&](u32 off) { b[set + off] = neg ? 0 : 1; };
        const auto field = [&](u32 off) { return r32(b, set + off); };
        const auto put = [&](u32 off, u32 v) { w32(b, set + off, v); };
        const u32 arg1 = (neg || count == 1) ? 0 : tok[1];
        const auto bytes_value = [&](u32 off) { // a string, its bytes (+4) and their count (+8)
            put(off, replace_value(ctx, field(off), arg1));
            put(off + 4, replace_bytes(ctx, field(off + 4), arg1, counts[1]));
            put(off + 8, static_cast<u32>(counts[1]));
        };
        if (key == "dhcp") flag(0x50);
        else if (key == "dhcp_host_name_null_terminated") flag(0x51);
        else if (key == "pppoe") flag(0x74);
        else if (key == "mtu") {
            if (neg) put(0x80, 0);
            else if (count != 1) scan_decimal(ctx, tok[1], set + 0x80);
        } else if (key == "want.dns1_nego") flag(0x70);
        else if (key == "want.dns2_nego") flag(0x71);
        else if (key == "op_mode") put(0xD0, replace_value(ctx, field(0xD0), arg1));
        else if (key == "channel") put(0xD4, replace_value(ctx, field(0xD4), arg1));
        else if (key == "auth_wlan") {
            put(0xD8, replace_value(ctx, field(0xD8), arg1));
            const u32 arg2 = (neg || count <= 2) ? 0 : tok[2];
            put(0xDC, replace_value(ctx, field(0xDC), arg2));
            put(0xE0, replace_bytes(ctx, field(0xE0), arg2, counts[2]));
            put(0xE4, static_cast<u32>(counts[2]));
        } else if (key == "bssid") bytes_value(0xA0);
        else if (key == "scan_channel_list") bytes_value(0xAC);
        else if (key == "supported_rate") bytes_value(0xB8);
        else if (key == "cap_info") bytes_value(0xC4);
        else if (key == "auth_8021x") {
            put(0xE8, replace_value(ctx, field(0xE8), arg1));
            put(0xEC, replace_value(ctx, field(0xEC), (neg || count <= 2) ? 0 : tok[2]));
            put(0xF0, replace_value(ctx, field(0xF0), (neg || count <= 3) ? 0 : tok[3]));
        } else {
            for (const auto& [off, name] : kNetsetKeys) {
                if (name != key) continue;
                std::vector<std::string> line(words.begin(), words.begin() + count);
                if (neg) put(off, replace_value(ctx, field(off), 0));
                else if (key == "search" || key == "nameserver") put(ns::resolv, add_line(ctx, field(ns::resolv), line));
                else if (key == "route" || key == "ipv6.addr") put(off, add_line(ctx, field(off), line));
                else if (count != 1) put(off, replace_value(ctx, field(off), tok[1]));
                break;
            }
        }
    }
}

// libnet's sys_netset_* wrappers: the call inside the thread's context (an aborted one answers its error), a negative
// result's errno kept and returned as SYS_NET_ERROR_*.
template <class F>
s32 netset_call(PPUContext& ctx, F&& inner)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    if (r32(b, c + kCtxAborted)) {
        const s32 r = -static_cast<s32>(r32(b, c + kCtxErrno));
        return r >= 0 ? r : failure(ctx, r);
    }
    w32(b, c + kCtxInCall, 1);
    const s32 r = inner();
    w32(b, c + kCtxInCall, 0);
    return r >= 0 ? r : failure(ctx, r);
}

s32 sys_netset_open(PPUContext& ctx, u32 text, u32, u32)
{
    return netset_call(ctx, [&] { return netset_parse(ctx, text); });
}

// sys_netset_close(id): the netset unlinked and freed; with none left lv2's resolver configuration is cleared and the
// key values with it, else the key values are the first netset's.
s32 sys_netset_close(PPUContext& ctx, s32 id)
{
    return netset_call(ctx, [&]() -> s32 {
        u8* b = gb(ctx);
        Libnet& n = libnet();
        const u32 set = find_netset(ctx, static_cast<u32>(id));
        if (!set) return errno_negated(ctx);
        const u32 next = r32(b, set + ns::next), prev = r32(b, set + ns::prev);
        if (next) w32(b, next + ns::prev, prev);
        else n.sets_tail = prev;
        if (prev) w32(b, prev + ns::next, next);
        else n.sets_head = next;
        free_netset(ctx, set);
        if (!n.sets_head) {
            set_resolv_conf(ctx, 0);
            n.keys = {};
            return 0;
        }
        for (usize k = 0; k < n.keys.size(); k++) n.keys[k] = r32(b, n.sets_head + kKeyFields[k]);
        return 0;
    });
}

// sys_netset_abort(id): if_up's wait cut short (its timeout 0), then a wait (100 ms at a time) until it has returned.
s32 sys_netset_abort(PPUContext& ctx, s32 id, s32)
{
    return netset_call(ctx, [&]() -> s32 {
        u8* b = gb(ctx);
        const u32 set = find_netset(ctx, static_cast<u32>(id));
        if (!set) return errno_negated(ctx);
        u32 busy = r32(b, set + ns::busy);
        w32(b, set + ns::timeout, 0);
        while (busy) {
            _sys_net_lib_usleep(ctx, 100000);
            busy = r32(b, set + ns::busy);
        }
        return 0;
    });
}

// sys_netset_get_status(id, out): the thread's last netset.c failure (what, line, its three arguments) in a 0x5C-byte
// report, and with an ID that netset's state (+0x34) and status (+0x38).
s32 sys_netset_get_status(PPUContext& ctx, s32 id, u32 out)
{
    return netset_call(ctx, [&]() -> s32 {
        u8* b = gb(ctx);
        if (!usable(ctx, out, 0x5C, "sys_netset_get_status: out")) return errno_negated(ctx);
        const u32 c = context(ctx);
        std::memset(b + out, 0, 0x5C);
        w32(b, out, r32(b, c + kCtxSetError));
        w32(b, out + 8, r32(b, c + kCtxSetArg1));
        w32(b, out + 12, r32(b, c + kCtxSetArg2));
        w32(b, out + 16, r32(b, c + kCtxSetArg3));
        w32(b, out + 4, r32(b, c + kCtxSetLine));
        if (!id) return 0;
        const u32 set = find_netset(ctx, static_cast<u32>(id));
        if (!set) return errno_negated(ctx);
        w32(b, out + 0x34, r32(b, set + ns::state));
        w32(b, out + 0x38, r32(b, set + ns::status));
        return 0;
    });
}

// sys_netset_get_key_value(0, key, out, size, n): a configuration value (kKeyNames) copied out (1) when there is one
// and it fits (else EINVAL), 0 without one (or n below 1); with no out, whether there is one. Only ID 0 (EINVAL, noted
// 5 line 2497, otherwise; an unknown key: 2, line 2527).
s32 sys_netset_get_key_value(PPUContext& ctx, s32 id, u32 key, u32 out, u32 size, s32 count)
{
    return netset_call(ctx, [&]() -> s32 {
        u8* b = gb(ctx);
        const auto einval = [&](u32 error, u32 line) {
            if (error) set_report(ctx, error, line);
            set_errno(ctx, SYS_NET_EINVAL);
            return errno_negated(ctx);
        };
        if (id) return einval(5, 2497);
        const std::string k = guest_str(ctx, key, "sys_netset_get_key_value: key").value_or("");
        usize i = 0;
        while (i < kKeyNames.size() && kKeyNames[i] != k) i++;
        if (i == kKeyNames.size()) return einval(2, 2527);
        const u32 v = libnet().keys[i];
        if (!out) return v != 0 ? 1 : 0;
        if (count < 1 || !v) return 0;
        const std::string value = guest_str(ctx, v, "key value").value_or("");
        if (size < value.size() + 1) return einval(0, 0);
        if (!usable(ctx, out, static_cast<u32>(value.size()) + 1, "sys_netset_get_key_value: out")) return errno_negated(ctx);
        std::memcpy(b + out, value.c_str(), value.size() + 1);
        return 1;
    });
}

// sys_netset_get_if_id(id, name, flags): the index of the interface (among those with an IPv4 address) a netset's
// interface (flags 1) and the name (flags 2) agree on; with no link lv2 lists none: EINVAL.
s32 sys_netset_get_if_id(PPUContext& ctx, s32 id, u32, u32 flags)
{
    return netset_call(ctx, [&]() -> s32 {
        if ((flags & 1) && !find_netset(ctx, static_cast<u32>(id))) return errno_negated(ctx);
        StackVar ifap(ctx, gb(ctx), 8);
        if (interfaces(ctx, ifap.addr()) == 0) set_errno(ctx, SYS_NET_EINVAL); // (getifaddrs stops on any interface)
        return errno_negated(ctx);
    });
}

// What lv2 would have to answer for these paths to go on: with no link it has no interface (D-539).
[[noreturn]] void interface_answered(std::string_view fn, std::string_view name)
{
    PS_FATAL(kChan, "{}: lv2 answered an ioctl on {}; with no link it has no interface (D-523, D-539)", fn, name);
}

// libnet f_19a60: an interface's flags read and changed (up, or with -1 down) through ioctls; -1 with no name (noted
// 25, line 556; EINVAL) or when lv2 refuses (with no link: ENXIO).
s32 interface_flags(PPUContext& ctx, u32 name, std::string_view fn)
{
    u8* b = gb(ctx);
    if (!name) {
        set_report(ctx, 25, 556);
        set_errno(ctx, SYS_NET_EINVAL);
        return -1;
    }
    StackVar ifr(ctx, b, 40);
    const std::string n = guest_str(ctx, name, "interface name").value_or("");
    std::memcpy(b + ifr.addr(), n.c_str(), std::min<usize>(n.size(), 39));
    const s32 so = netset_socket(ctx, 2, 2, 0);
    if (so < 0) return -1;
    if (netset_ioctl(ctx, so, 0xC0206911, ifr.addr()) >= 0) interface_answered(fn, n);
    socketclose(ctx, so);
    return -1;
}

// The DHCP client's stop for an interface (f_152dc): pspocket's never starts (its first step, the interface's index,
// finds none with no link): not found (noted 25, line 11238; EINVAL), -1.
s32 dhcp_stop(PPUContext& ctx)
{
    set_report(ctx, 25, 11238);
    set_errno(ctx, SYS_NET_EINVAL);
    return -1;
}

// The wireless link's teardown (f_c168): the chip told (lv2's eurus command 4151, 13 bytes); 0 or -70.
s32 wireless_down(PPUContext& ctx)
{
    StackVar block(ctx, gb(ctx), 16);
    return net_call(ctx, 726, {4151, block.addr(), 13}) >= 0 ? 0 : -70;
}

// sys_netset_if_up(id, timeout_ms): a netset's interface brought up (netset.c f_1bb8c): the first one sets lo0 up
// (127.0.0.1/8) before anything else, which lv2 with no link refuses (ENXIO): the netset's state goes back to 0,
// status 9, and that error (the wireless link told when its type is wlan).
s32 sys_netset_if_up(PPUContext& ctx, s32 id, s32 timeout, s32)
{
    return netset_call(ctx, [&]() -> s32 {
        u8* b = gb(ctx);
        Libnet& n = libnet();
        const u32 set = find_netset(ctx, static_cast<u32>(id));
        if (!set) return errno_negated(ctx);
        if (r32(b, set + ns::state)) return 0;
        w32(b, set + ns::status, 3);
        w32(b, set + ns::state, 2);
        w32(b, set + ns::timeout, static_cast<u32>(timeout));
        w32(b, set + ns::busy, 1);
        if (++n.sets_up != 1 || n.loopback_up) interface_answered("sys_netset_if_up", "lo0");
        const u32 d = data(ctx);
        if (interface_address(ctx, d + kStrLo0, d + kStrLoopback, d + kStrLoopMask, 0) >= 0)
            interface_answered("sys_netset_if_up", "lo0");
        if (const u32 type = r32(b, set + ns::type); type && guest_str(ctx, type, "netset type").value_or("") == "wlan")
            wireless_down(ctx);
        const s32 r = errno_negated(ctx);
        if (r < 0) {
            n.sets_up--;
            w32(b, set + ns::status, 9);
            w32(b, set + ns::state, 0);
            w32(b, set + ns::busy, 0);
            return r;
        }
        w32(b, set + ns::busy, 0);
        w32(b, set + ns::status, 6);
        w32(b, set + ns::state, 5);
        return r;
    });
}

// sys_netset_if_down(id): nothing for a netset that is not up; else (only while if_up still runs, on another thread)
// its DHCP client stopped, its PPPoE link and interface taken down, which lv2 refuses with no link: state 0, status 9.
s32 sys_netset_if_down(PPUContext& ctx, s32 id, s32, s32)
{
    return netset_call(ctx, [&]() -> s32 {
        u8* b = gb(ctx);
        const u32 set = find_netset(ctx, static_cast<u32>(id));
        if (!set) return errno_negated(ctx);
        if (!r32(b, set + ns::state)) return 0;
        w32(b, set + ns::status, 7);
        w32(b, set + ns::state, 6);
        if (b[set + ns::dhcp]) dhcp_stop(ctx);
        if (b[set + ns::pppoe]) interface_flags(ctx, data(ctx) + kStrPppoe, "sys_netset_if_down");
        else interface_flags(ctx, r32(b, set + ns::ifname), "sys_netset_if_down");
        w32(b, set + ns::status, 9);
        w32(b, set + ns::state, 0);
        return errno_negated(ctx);
    });
}

// ------------------------------------------------------------------------------------------------- the resolver
// libnet's resolver (D-539, part 4): FreeBSD's gethnamaddr.c and nsdispatch over BIND 8's res_query.c,
// res_mkquery.c, res_comp.c and res_send.c, as Sony built them, their statics in the thread's context. A name is looked
// up in libnet's built-in /etc/hosts and then asked of the context's name servers. Offline lv2 gives no resolver
// configuration (an empty text, D-523), so res_init never runs and the one name server has no address: its socket is
// refused, nothing is sent, and the lookup ends in TRY_AGAIN.

// The hostent libnet keeps in the thread's context (gethnamaddr.c's statics): struct hostent at +0xC0 {h_name,
// h_aliases, h_addrtype, h_length, h_addr_list}, the address pointers at +0x30, the alias pointers at +0xD4 (35), the
// text buffer at +0x160 (0x800 bytes), the address at +0x960, the hosts file (libnet's FILE) at +0x970 and whether to
// keep it open at +0x974.
constexpr u32 kHostAddrPtrs = 0x30, kHost = 0xC0, kHostAliases = 0xD4, kHostAliasesEnd = 0x15C, kHostBuf = 0x160;
constexpr u32 kHostBufSize = 0x800, kHostAddr = 0x960, kHostFile = 0x970, kHostStayOpen = 0x974;
constexpr u32 kHostName = kHost, kHostAliasList = kHost + 4, kHostType = kHost + 8, kHostLength = kHost + 12;
constexpr u32 kHostAddrList = kHost + 16;

// The resolver's state (BIND's struct __res_state) from +0x988: the first timeout (seconds), the number of tries, the
// options, the name servers (their count, then 16-byte addresses), the last query's ID, the search list (char*[7]),
// ndots (the high nibble of +0xAEC); res_send's socket at +0xB44, whether it is connected, whether it is a stream
// socket, its family.
constexpr u32 kResTries = kCtxResolverRetrans, kResOptions = kCtx2448, kResNsCount = kCtx2452, kResNsAddr = 2456;
constexpr u32 kResId = 0x9C8, kResSearch = 0x9CC, kResNdots = 0xAEC;
constexpr u32 kResSock = kCtx2884, kResConnected = 2888, kResStream = 2892, kResFamily = 2896;
constexpr u32 RES_INIT = 0x1, RES_USEVC = 0x8, RES_RECURSE = 0x40, RES_DEFNAMES = 0x80, RES_DNSRCH = 0x200;
constexpr u32 RES_USE_INET6 = 0x2000, RES_USE_EDNS0 = 0x40000000;

constexpr s32 NETDB_INTERNAL = -1, NETDB_SUCCESS = 0, HOST_NOT_FOUND = 1, TRY_AGAIN = 2, NO_RECOVERY = 3, NO_DATA = 4;
constexpr u32 SYS_NET_EINTR = 4, SYS_NET_EMSGSIZE = 40, SYS_NET_ENOBUFS = 55, SYS_NET_ETIMEDOUT = 60;
constexpr u32 SYS_NET_ECONNREFUSED = 61;
// nsdispatch's answers.
constexpr s32 NS_SUCCESS = 1, NS_UNAVAIL = 2, NS_NOTFOUND = 4;
// DNS classes and types.
constexpr u16 C_IN = 1, T_A = 1, T_PTR = 12, T_AAAA = 28, T_OPT = 41;

void set_h_errno(PPUContext& ctx, s32 e) { w32(gb(ctx), context(ctx) + kCtxHErrno, static_cast<u32>(e)); }
s32 h_errno_now(PPUContext& ctx) { return static_cast<s32>(r32(gb(ctx), context(ctx) + kCtxHErrno)); }
u32 errno_now(PPUContext& ctx) { return r32(gb(ctx), context(ctx) + kCtxErrno); }
u32 res_options(PPUContext& ctx) { return r32(gb(ctx), context(ctx) + kResOptions); }

u32 g_strlen(const u8* b, u32 s)
{
    u32 n = 0;
    while (b[s + n]) n++;
    return n;
}

// strpbrk on guest memory: the first byte of s that is one of `set`, or 0.
u32 g_strpbrk(const u8* b, u32 s, std::string_view set)
{
    for (; b[s]; s++) {
        if (set.find(static_cast<char>(b[s])) != std::string_view::npos) return s;
    }
    return 0;
}

// strcasecmp(a, b) == 0 in the C locale (libnet f_ed54 compares through its tolower table).
bool g_equal_nocase(const u8* b, u32 x, u32 y)
{
    const auto lower = [](u8 c) { return c >= 'A' && c <= 'Z' ? static_cast<u8>(c + 32) : c; };
    for (;; x++, y++) {
        if (lower(b[x]) != lower(b[y])) return false;
        if (!b[x]) return true;
    }
}

// The check each resolver step makes first while RES_INIT is unset (always: libnet never sets it), libnet f_e4ac:
// lv2's resolver configuration read again (f_125c4); when it changed, the resolver would be set up from it (res_init,
// f_7a34: its "nameserver", "domain", "search" and "options" lines). lv2 has none to give offline, so that is reached
// only after _sce_net_set_name_server gave it one, and a name server it names cannot be reached anyway.
void res_ready(PPUContext& ctx)
{
    if (res_options(ctx) & RES_INIT) return;
    bool changed = false;
    const u32 conf = resolv_conf(ctx, &changed);
    if (!conf) {
        // libnet then reads a stack word it never set
        PS_LOG_ERROR(kChan, "the resolver: lv2 refused its configuration (sys_net_infoctl 9); taken as unchanged");
        return;
    }
    if (changed) {
        PS_FATAL(kChan,
            "the resolver: lv2's configuration is now \"{}\": libnet's res_init, which reads it, is not pspocket's own "
            "(no name server can be reached offline, D-539)",
            guest_str(ctx, conf, "resolv.conf").value_or(""));
    }
}

// ---- the hosts file

// libnet's FILE (88 bytes from the pool, "netset.c" 1698) over a text of its own: +0 the read position, +12 (16
// bits) the flags (0x40 at the end), +28 the text.
constexpr u32 kFileSize = 88, kFilePos = 0, kFileFlags = 12, kFileText = 28;
constexpr u16 kFileEnd = 0x40;

// fopen("/etc/hosts", "r") (libnet f_e45c over f_1a2b4): libnet's built-in hosts text. 0 (errno ENOENT) when the pool
// refuses.
u32 hosts_open(PPUContext& ctx)
{
    const u32 text = data(ctx) + kHostsText;
    const u32 f = netset_malloc(ctx, kFileSize, 1698);
    if (!f) {
        set_errno(ctx, SYS_NET_ENOENT);
        return 0;
    }
    u8* b = gb(ctx);
    std::memset(b + f, 0, kFileSize);
    w32(b, f + kFileText, text);
    w32(b, f + kFilePos, text);
    return f;
}

void file_mark_end(u8* b, u32 f) { ps_write16(b, f + kFileFlags, static_cast<u16>(ps_read16(b, f + kFileFlags) | kFileEnd)); }

// fgets (libnet f_e378 over f_17000): a line (at most n - 1 bytes, its newline kept) into buf, NUL-ended; at the end of
// the text nothing is read, the end flag is set and errno is EBADF.
u32 file_gets(PPUContext& ctx, u32 buf, s32 n, u32 f)
{
    u8* b = gb(ctx);
    if (f) {
        u32 pos = r32(b, f + kFilePos);
        if (pos && b[pos]) {
            u32 out = buf;
            if (n > 1) {
                const u8 first = b[pos++];
                b[out++] = first;
                if (first != '\n') {
                    for (s32 left = n - 2; left > 0; left--) {
                        const u8 c = b[pos];
                        b[out] = c;
                        if (!c) break;
                        pos++;
                        out++;
                        if (c == '\n') break;
                    }
                }
            }
            b[out] = 0;
            w32(b, f + kFilePos, pos);
            return buf;
        }
        file_mark_end(b, f);
    }
    set_errno(ctx, SYS_NET_EBADF);
    return 0;
}

// fclose (libnet f_e404): the FILE back to the pool.
void file_close(PPUContext& ctx, u32 f)
{
    if (f) net_free(ctx, f);
    else set_errno(ctx, SYS_NET_EBADF);
}

// map_v4v6_address (f_239c): an IPv4 address (4 bytes at src) as ::ffff:a.b.c.d (16 at dst).
void map_v4v6_address(u8* b, u32 src, u32 dst)
{
    u8 v4[4];
    std::memcpy(v4, b + src, 4);
    std::memset(b + dst, 0, 10);
    b[dst + 10] = 0xFF;
    b[dst + 11] = 0xFF;
    std::memcpy(b + dst + 12, v4, 4);
}

// _gethtent (f_3358): the next entry of the hosts file (opened at first use) whose address has the type and length
// the hostent asks for (0: any), parsed into the context's hostent; 0 at the end (HOST_NOT_FOUND) or when the file
// cannot be opened (NETDB_INTERNAL).
u32 gethtent(PPUContext& ctx)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    if (!r32(b, c + kHostFile)) {
        const u32 f = hosts_open(ctx);
        w32(b, c + kHostFile, f);
        if (!f) {
            set_h_errno(ctx, NETDB_INTERNAL);
            return 0;
        }
    }
    for (;;) {
        const u32 p = file_gets(ctx, c + kHostBuf, kHostBufSize, r32(b, c + kHostFile));
        if (!p) {
            set_h_errno(ctx, HOST_NOT_FOUND);
            return 0;
        }
        if (b[p] == '#') continue;
        u32 cp = g_strpbrk(b, p, "#\n");
        if (!cp) continue;
        b[cp] = 0;
        cp = g_strpbrk(b, p, " \t");
        if (!cp) continue;
        b[cp] = 0;
        s32 af = SYS_NET_AF_INET6;
        u32 len = 16;
        if (inet_pton(ctx, SYS_NET_AF_INET6, p, c + kHostAddr) <= 0) {
            if (inet_pton(ctx, SYS_NET_AF_INET, p, c + kHostAddr) <= 0) continue;
            af = SYS_NET_AF_INET;
            len = 4;
            if (res_options(ctx) & RES_USE_INET6) {
                map_v4v6_address(b, c + kHostAddr, c + kHostAddr);
                af = SYS_NET_AF_INET6;
                len = 16;
            }
        }
        const s32 want_af = static_cast<s32>(r32(b, c + kHostType));
        const u32 want_len = r32(b, c + kHostLength);
        if ((want_af && want_af != af) || (want_len && want_len != len)) continue;
        w32(b, c + kHostAddrPtrs, c + kHostAddr);
        w32(b, c + kHostAddrPtrs + 4, 0);
        w32(b, c + kHostAddrList, c + kHostAddrPtrs);
        w32(b, c + kHostLength, len);
        w32(b, c + kHostType, static_cast<u32>(af));
        do cp++;
        while (b[cp] == ' ' || b[cp] == '\t');
        w32(b, c + kHostName, cp);
        u32 q = c + kHostAliases;
        w32(b, c + kHostAliasList, q);
        cp = g_strpbrk(b, cp, " \t");
        if (cp) {
            b[cp] = 0;
            for (;;) {
                bool end = false;
                do {
                    cp++;
                    if (!b[cp]) {
                        end = true;
                        break;
                    }
                } while (b[cp] == ' ' || b[cp] == '\t');
                if (end) break;
                if (q < c + kHostAliasesEnd) {
                    w32(b, q, cp);
                    q += 4;
                }
                cp = g_strpbrk(b, cp, " \t");
                if (!cp) break;
                b[cp] = 0;
            }
        }
        w32(b, q, 0);
        set_h_errno(ctx, NETDB_SUCCESS);
        return c + kHost;
    }
}

// _sethtent (f_37ec): the hosts file opened, or, already open, marked at its end (libnet's rewind does no more); whether
// to keep it open.
void sethtent(PPUContext& ctx, u32 stayopen)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    if (const u32 f = r32(b, c + kHostFile)) file_mark_end(b, f);
    else w32(b, c + kHostFile, hosts_open(ctx));
    w32(b, c + kHostStayOpen, stayopen);
}

// _endhtent (f_376c): the hosts file closed unless it is to stay open.
void endhtent(PPUContext& ctx)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    const u32 f = r32(b, c + kHostFile);
    if (!f || r32(b, c + kHostStayOpen)) return;
    file_close(ctx, f);
    w32(b, c + kHostFile, 0);
}

// _gethtbyname2 (f_39ac), the hosts source of gethostbyname: every entry of the family whose name or an alias is the
// name (case ignored), up to 35, gathered in one block from the pool ("gethnamaddr.c" 0x3DB: the names, then the
// addresses 4-aligned) and copied into the text buffer. NS_NOTFOUND (HOST_NOT_FOUND) when there is none.
s32 files_byname(PPUContext& ctx, u32 name, s32 af, u32* result)
{
    u8* b = gb(ctx);
    sethtent(ctx, r32(b, context(ctx) + kHostStayOpen));
    u32 buf = 0, bp = 0, num = 0;
    u32 p = gethtent(ctx);
    for (;;) {
        if (!p) {
            endhtent(ctx);
            if (!num) {
                *result = 0;
                set_h_errno(ctx, HOST_NOT_FOUND);
                return NS_NOTFOUND;
            }
            break;
        }
        if (num > 34) {
            endhtent(ctx);
            break;
        }
        bool match = static_cast<s32>(r32(b, p + 8)) == af;
        if (match && !g_equal_nocase(b, r32(b, p), name)) {
            match = false;
            for (u32 a = r32(b, p + 4); r32(b, a); a += 4) {
                if (g_equal_nocase(b, r32(b, a), name)) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) {
            p = gethtent(ctx);
            continue;
        }
        const u32 length = r32(b, p + 12);
        if (!num) {
            u32 size = g_strlen(b, r32(b, p)) + length * 35 + 5;
            for (u32 a = r32(b, p + 4); r32(b, a); a += 4) size += g_strlen(b, r32(b, a)) + 1;
            buf = net_malloc(ctx, size, data(ctx) + kTagGethnamaddr, 0x3DB);
            if (!buf) {
                // the hosts file stays open
                *result = 0;
                set_h_errno(ctx, NETDB_INTERNAL);
                return NS_NOTFOUND;
            }
            u32 out = buf;
            const auto copy = [&](u32 s) {
                const u32 n = g_strlen(b, s) + 1;
                std::memmove(b + out, b + s, n);
                out += n;
            };
            copy(r32(b, p));
            for (u32 a = r32(b, p + 4); r32(b, a); a += 4) copy(r32(b, a));
            bp = (out + 4) & ~3u;
            b[out] = 0;
        }
        num++;
        std::memmove(b + bp, b + r32(b, r32(b, p + 16)), length);
        bp += length;
        p = gethtent(ctx);
    }
    if (bp - buf > 0x7FD) {
        net_free(ctx, buf);
        set_errno(ctx, SYS_NET_ENOSPC);
        set_h_errno(ctx, NETDB_INTERNAL);
        *result = 0;
        set_h_errno(ctx, HOST_NOT_FOUND);
        return NS_NOTFOUND;
    }
    const u32 c = context(ctx);
    const u32 dst = (c + kHostBuf + 3) & ~3u;
    std::memmove(b + dst, b + buf, bp - buf);
    net_free(ctx, buf);
    w32(b, c + kHostName, dst);
    u32 cp = dst + g_strlen(b, dst) + 1;
    u32 q = c + kHostAliases;
    while (b[cp]) {
        w32(b, q, cp);
        q += 4;
        cp += g_strlen(b, cp) + 1;
    }
    w32(b, q, 0);
    u32 ptrs = c + kHostAddrPtrs;
    for (u32 i = 0, a = (cp + 4) & ~3u; i < num; i++, ptrs += 4) {
        w32(b, ptrs, a);
        a += r32(b, c + kHostLength);
    }
    w32(b, ptrs, 0);
    *result = c + kHost;
    return NS_SUCCESS;
}

// _gethtbyaddr (f_3894), the hosts source of gethostbyaddr: the first entry with this address.
s32 files_byaddr(PPUContext& ctx, u32 addr, u32 len, s32 af, u32* result)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    w32(b, c + kHostLength, len);
    w32(b, c + kHostType, static_cast<u32>(af));
    sethtent(ctx, r32(b, c + kHostStayOpen));
    for (;;) {
        const u32 p = gethtent(ctx);
        if (!p) {
            endhtent(ctx);
            *result = 0;
            set_h_errno(ctx, HOST_NOT_FOUND);
            return NS_NOTFOUND;
        }
        if (static_cast<s32>(r32(b, p + 8)) == af && std::memcmp(b + r32(b, r32(b, p + 16)), b + addr, len) == 0) {
            endhtent(ctx);
            *result = p;
            return NS_SUCCESS;
        }
    }
}

// ---- the name servers

// res_close (f_9550): res_send's socket closed.
void res_close(PPUContext& ctx)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    if (static_cast<s32>(r32(b, c + kResSock)) < 0) return;
    socketclose(ctx, static_cast<s32>(r32(b, c + kResSock)));
    w32(b, c + kResSock, 0xFFFFFFFF);
    w32(b, c + kResConnected, 0);
    w32(b, c + kResStream, 0);
    w32(b, c + kResFamily, 0);
}

// res_send (f_997c): each try asks each name server not yet found bad, through libnet's DNS cache (its hooks, f_adcc
// and f_b168, act on AF_INET servers only) and a socket of the server's family kept in the context (a stream socket
// for a query over 512 bytes or with RES_USEVC, which is then made once). Offline the one name server has no family:
// lv2 refuses its socket, which marks it bad, and nothing is sent: -1, errno ECONNREFUSED (for a stream, the socket's
// errno). A server lv2 could reach needs res_init (above), so the cache and the exchange are not pspocket's own.
s32 res_send(PPUContext& ctx, s32 buflen, s32 anssiz)
{
    u8* b = gb(ctx);
    res_ready(ctx);
    if (anssiz < 12) {
        set_errno(ctx, SYS_NET_EINVAL);
        return -1;
    }
    const bool stream = (res_options(ctx) & RES_USEVC) || buflen > 512;
    u32 terrno = SYS_NET_ETIMEDOUT, badns = 0;
    for (s32 attempt = 0; attempt < static_cast<s32>(r32(b, context(ctx) + kResTries)); attempt++) {
        for (u32 ns = 0; static_cast<s32>(ns) < static_cast<s32>(r32(b, context(ctx) + kResNsCount)); ns++) {
            const u32 c = context(ctx);
            const u32 nsap = c + kResNsAddr + 16 * ns;
            const u32 bit = ns < 32 ? 1u << ns : 0;
            if (badns & bit) {
                res_close(ctx);
                continue;
            }
            const u8 family = b[nsap + 1];
            if (family == SYS_NET_AF_INET) {
                PS_FATAL(kChan,
                    "res_send: name server {}.{}.{}.{}: libnet's DNS cache and its query over the network are not "
                    "pspocket's own (offline, D-539)",
                    b[nsap + 4], b[nsap + 5], b[nsap + 6], b[nsap + 7]);
            }
            if (stream) attempt = static_cast<s32>(r32(b, c + kResTries));
            const s32 sock = static_cast<s32>(r32(b, c + kResSock));
            const bool reuse = stream ? r32(b, c + kResStream) != 0 : r32(b, c + kResStream) == 0;
            if (sock < 0 || !reuse || r32(b, c + kResFamily) != family) {
                if (stream ? sock >= 0 : r32(b, c + kResStream) != 0) res_close(ctx);
                w32(b, context(ctx) + kResFamily, family);
                const s32 s = socket(ctx, family, stream ? 1 : 2, 0);
                w32(b, context(ctx) + kResSock, static_cast<u32>(s));
                if (s < 0) {
                    terrno = errno_now(ctx);
                    badns |= bit;
                    res_close(ctx);
                    continue;
                }
            }
            PS_FATAL(kChan, "res_send: lv2 gave a socket of family {} for a name server: its exchange is not "
                            "pspocket's own (offline, D-539)", family);
        }
    }
    res_close(ctx);
    set_errno(ctx, stream ? terrno : SYS_NET_ECONNREFUSED);
    return -1;
}

// dn_comp (res_comp.c, f_6d58) for a query's name: ns_name_pton into a block from the pool ("res_comp.c" 0x345), then
// ns_name_pack into dst (dstsiz bytes). libnet's queries pass a list with no names in it yet, so nothing is
// compressed, but each label is looked up first (errno ENOENT). The length written, or -1 (errno EMSGSIZE).
s32 dn_comp(PPUContext& ctx, u32 src, u32 dst, s32 dstsiz)
{
    u8* b = gb(ctx);
    const u32 tmp = net_malloc(ctx, 0xFF, data(ctx) + kTagResComp, 0x345);
    if (!tmp) return -1;
    const auto fail = [&]() {
        set_errno(ctx, SYS_NET_EMSGSIZE);
        net_free(ctx, tmp);
        return -1;
    };
    // ns_name_pton (without its check for an empty label inside the name: "a..b" fails, "a.." is "a.")
    const u32 eom = tmp + 0xFF;
    u32 label = tmp, bp = tmp + 1, s = src;
    bool escaped = false, absolute = false;
    for (u8 ch; (ch = b[s]) != 0;) {
        s++;
        u32 v = ch;
        if (escaped) {
            if (is_digit(ch)) {
                const u8 d2 = b[s], d3 = d2 ? b[s + 1] : 0;
                if (!d2 || !is_digit(d2) || !d3 || !is_digit(d3)) return fail();
                v = (ch - '0') * 100u + (d2 - '0') * 10u + (d3 - '0');
                if (v > 255) return fail();
                s += 2;
            }
        } else if (ch == '\\') {
            escaped = true;
            continue;
        } else if (ch == '.') {
            const u32 len = bp - label - 1;
            if ((len & 0xC0) || label >= eom) return fail();
            b[label] = static_cast<u8>(len);
            if (!b[s]) {
                if (len) {
                    if (bp >= eom) return fail();
                    b[bp++] = 0;
                }
                if (bp - tmp > 0xFF) return fail();
                absolute = true;
                break;
            }
            if (!len) return fail();
            label = bp++;
            continue;
        }
        if (bp >= eom) return fail();
        b[bp++] = static_cast<u8>(v);
        escaped = false;
    }
    if (!absolute) {
        const u32 len = bp - label - 1;
        if ((len & 0xC0) || label >= eom) return fail();
        b[label] = static_cast<u8>(len);
        if (len) {
            if (bp >= eom) return fail();
            b[bp++] = 0;
        }
        if (bp - tmp > 0xFF) return fail();
    }
    // ns_name_pack
    for (u32 p = tmp, total = 0;;) {
        const u8 n = b[p];
        total += n + 1u;
        p += n + 1u;
        if ((n & 0xC0) || total > 0xFF) return fail();
        if (!n) break;
    }
    for (u32 sp = tmp, dp = dst;;) {
        const u8 n = b[sp];
        if (n) set_errno(ctx, SYS_NET_ENOENT); // not among the earlier names (there are none)
        if (dp + n + 1 >= dst + static_cast<u32>(dstsiz)) return fail();
        std::memmove(b + dp, b + sp, n + 1u);
        if (!n) {
            net_free(ctx, tmp);
            return static_cast<s32>(dp + 1 - dst);
        }
        sp += n + 1u;
        dp += n + 1u;
    }
}

// res_mkquery (f_87dc) for a standard query (the only kind libnet's resolver makes): the header (a random ID, also
// kept in the context; RD from RES_RECURSE), the name, its type and class. Its length, or -1.
s32 res_mkquery(PPUContext& ctx, u32 dname, u16 cls, u16 type, u32 buf, s32 buflen)
{
    u8* b = gb(ctx);
    res_ready(ctx);
    if (!buf || buflen < 12) return -1;
    std::memset(b + buf, 0, 12);
    const u32 c = context(ctx);
    const u16 id = static_cast<u16>(_sys_net_lib_rand(ctx));
    ps_write16(b, buf, id);
    ps_write16(b, c + kResId, id);
    w32(b, buf, r32(b, buf) & 0xFFFF87FF); // opcode QUERY
    w32(b, buf, (r32(b, buf) & 0xFFFFFEF0) | ((res_options(ctx) & RES_RECURSE) ? 0x100 : 0));
    const s32 room = buflen - 12;
    if (room <= 3) return -1;
    const s32 n = dn_comp(ctx, dname, buf + 12, room - 4);
    if (n < 0) return -1;
    const u32 cp = buf + 12 + static_cast<u32>(n);
    ps_write16(b, cp, type);
    ps_write16(b, cp + 2, cls);
    ps_write16(b, buf + 4, 1); // qdcount
    return static_cast<s32>(cp + 4 - buf);
}

// res_opt (f_86bc): the EDNS0 OPT record appended (an UDP payload of anslen, at most 0xFFFF). The new length, or -1.
s32 res_opt(PPUContext& ctx, s32 n, u32 buf, s32 buflen, s32 anslen)
{
    u8* b = gb(ctx);
    if (buflen - n < 11) return -1;
    const u32 cp = buf + static_cast<u32>(n);
    b[cp] = 0;
    ps_write16(b, cp + 1, T_OPT);
    ps_write16(b, cp + 3, static_cast<u16>(std::min(anslen, 0xFFFF)));
    b[cp + 5] = 0;
    b[cp + 6] = 0;
    ps_write16(b, cp + 7, 0);
    ps_write16(b, cp + 9, 0);
    ps_write16(b, buf + 10, static_cast<u16>(ps_read16(b, buf + 10) + 1));
    return n + 11;
}

// res_query (f_8b30): the query made (in a block from the pool, "res_query.c" 0x91) and sent; on failure h_errno
// TRY_AGAIN (NETDB_INTERNAL when interrupted), NO_RECOVERY when no query could be made.
s32 res_query(PPUContext& ctx, u32 name, u16 cls, u16 type, u32 answer, s32 anslen)
{
    u8* b = gb(ctx);
    w32(b, answer, r32(b, answer) & ~0xFu); // the answer's rcode NOERROR
    res_ready(ctx);
    const u32 buf = net_malloc(ctx, 0x400, data(ctx) + kTagResQuery, 0x91);
    if (!buf) {
        set_h_errno(ctx, NETDB_INTERNAL);
        return -1;
    }
    s32 n = res_mkquery(ctx, name, cls, type, buf, 0x400);
    if (n > 0 && (res_options(ctx) & RES_USE_EDNS0)) n = res_opt(ctx, n, buf, 0x400, anslen);
    if (n <= 0) {
        set_h_errno(ctx, NO_RECOVERY);
        net_free(ctx, buf);
        return n;
    }
    n = res_send(ctx, n, anslen); // (nothing to send it to)
    PS_CHECK(n < 0); // res_send never has an answer offline
    set_h_errno(ctx, errno_now(ctx) == SYS_NET_EINTR ? NETDB_INTERNAL : TRY_AGAIN);
    net_free(ctx, buf);
    return n;
}

// res_querydomain (f_8e24): name.domain asked (made in a block from the pool, "res_query.c" 0x1A4), or with no domain
// the name without its trailing dot.
s32 res_querydomain(PPUContext& ctx, u32 name, u32 domain, u16 cls, u16 type, u32 answer, s32 anslen)
{
    u8* b = gb(ctx);
    res_ready(ctx);
    const u32 nbuf = net_malloc(ctx, 0x401, data(ctx) + kTagResQuery, 0x1A4);
    if (!nbuf) {
        set_h_errno(ctx, NETDB_INTERNAL);
        return -1;
    }
    u32 longname = name;
    const u32 n = g_strlen(b, name);
    if (!domain) {
        if (n + 1 > 0x401) {
            set_h_errno(ctx, NO_RECOVERY);
            net_free(ctx, nbuf);
            return -1;
        }
        if (n && b[name + n - 1] == '.') {
            std::memmove(b + nbuf, b + name, n - 1);
            b[nbuf + n - 1] = 0;
            longname = nbuf;
        }
    } else {
        const u32 d = g_strlen(b, domain);
        if (n + d + 2 > 0x401) {
            set_h_errno(ctx, NO_RECOVERY);
            net_free(ctx, nbuf);
            return -1;
        }
        const std::string text = std::string(reinterpret_cast<const char*>(b + name), n) + "." +
                                 std::string(reinterpret_cast<const char*>(b + domain), d);
        std::memcpy(b + nbuf, text.c_str(), text.size() + 1);
        longname = nbuf;
    }
    const s32 r = res_query(ctx, longname, cls, type, answer, anslen);
    net_free(ctx, nbuf);
    return r;
}

// res_search (f_9040): the name as it is when it has ndots dots or more, then with each domain of the search list
// (with no dots: RES_DEFNAMES; with dots and no trailing one: RES_DNSRCH), then as it is if not tried yet; h_errno
// from the most telling failure. Offline ndots is 0 and the search list empty: res_init never ran.
s32 res_search(PPUContext& ctx, u32 name, u16 cls, u16 type, u32 answer, s32 anslen)
{
    u8* b = gb(ctx);
    res_ready(ctx);
    set_errno(ctx, 0);
    set_h_errno(ctx, HOST_NOT_FOUND);
    u32 dots = 0, n = 0;
    for (; b[name + n]; n++) dots += b[name + n] == '.';
    const bool trailing = n && b[name + n - 1] == '.';
    // (a name with no dots could be a user's alias: libnet's hostalias has none)
    s32 saved = -1;
    bool tried = false;
    u32 got_nodata = 0, got_servfail = 0;
    if (dots >= static_cast<u32>(b[context(ctx) + kResNdots] >> 4)) {
        const s32 r = res_querydomain(ctx, name, 0, cls, type, answer, anslen);
        if (r > 0) return r;
        saved = h_errno_now(ctx);
        tried = true;
    }
    const u32 opts = res_options(ctx);
    if ((!dots && (opts & RES_DEFNAMES)) || (dots && !trailing && (opts & RES_DNSRCH))) {
        for (u32 d = context(ctx) + kResSearch; r32(b, d); d += 4) {
            const u32 domain = r32(b, d);
            u32 done = 0;
            if (b[domain]) {
                const s32 r = res_querydomain(ctx, name, domain, cls, type, answer, anslen);
                if (r > 0) return r;
                if (errno_now(ctx) == SYS_NET_ECONNREFUSED) {
                    set_h_errno(ctx, TRY_AGAIN);
                    return -1;
                }
                if (errno_now(ctx) == SYS_NET_EINTR) {
                    set_h_errno(ctx, NETDB_INTERNAL);
                    return -1;
                }
                switch (h_errno_now(ctx)) {
                case NO_DATA: got_nodata++; break;
                case HOST_NOT_FOUND: break;
                case TRY_AGAIN:
                    if ((r32(b, answer) & 0xF) == 2) { // SERVFAIL
                        got_servfail++;
                        break;
                    }
                    done = 1;
                    break;
                default: done = 1;
                }
                done += (res_options(ctx) & RES_DNSRCH) ? 0 : 1;
            }
            if (done) break;
        }
    }
    if (!tried) {
        const s32 r = res_querydomain(ctx, name, 0, cls, type, answer, anslen);
        if (r > 0) return r;
    }
    if (saved != -1) set_h_errno(ctx, saved);
    else if (got_nodata) set_h_errno(ctx, NO_DATA);
    else if (got_servfail) set_h_errno(ctx, TRY_AGAIN);
    return -1;
}

// _dns_gethtbyname (f_31f4), the DNS source of gethostbyname: an A or AAAA query for the name (the answer in a block
// from the pool, "gethnamaddr.c" 0x4BF). Offline it never has an answer: NS_NOTFOUND.
s32 dns_byname(PPUContext& ctx, u32 name, s32 af)
{
    u16 type = 0;
    if (af == SYS_NET_AF_INET) type = T_A;
    else if (af == SYS_NET_AF_INET6) type = T_AAAA;
    else return NS_UNAVAIL;
    const u32 buf = net_malloc(ctx, 0x800, data(ctx) + kTagGethnamaddr, 0x4BF);
    if (!buf) {
        set_h_errno(ctx, NETDB_INTERNAL);
        return NS_NOTFOUND;
    }
    const s32 n = res_search(ctx, name, C_IN, type, buf, 0x800);
    PS_CHECK(n < 0); // no name server answers offline
    net_free(ctx, buf);
    return NS_NOTFOUND;
}

// _dns_gethtbyaddr (f_2e34), the DNS source of gethostbyaddr: a PTR query for the address's in-addr.arpa or ip6.arpa
// name (made in a block from the pool, "gethnamaddr.c" 0x4F3; the answer in another, 0x529). Offline: NS_NOTFOUND.
s32 dns_byaddr(PPUContext& ctx, u32 addr, s32 af)
{
    u8* b = gb(ctx);
    const u32 qbuf = net_malloc(ctx, 0x402, data(ctx) + kTagGethnamaddr, 0x4F3);
    if (!qbuf) {
        set_h_errno(ctx, NETDB_INTERNAL);
        return NS_NOTFOUND;
    }
    std::string q;
    if (af == SYS_NET_AF_INET) {
        q = std::format("{}.{}.{}.{}.in-addr.arpa", b[addr + 3], b[addr + 2], b[addr + 1], b[addr]);
    } else {
        for (s32 i = 15; i >= 0; i--) q += std::format("{:x}.{:x}.", b[addr + i] & 0xF, b[addr + i] >> 4);
        q += "ip6.arpa";
    }
    std::memcpy(b + qbuf, q.c_str(), q.size() + 1);
    const u32 buf = net_malloc(ctx, 0x800, data(ctx) + kTagGethnamaddr, 0x529);
    if (!buf) {
        set_h_errno(ctx, NETDB_INTERNAL);
        net_free(ctx, qbuf);
        return NS_NOTFOUND;
    }
    const s32 n = res_query(ctx, qbuf, C_IN, T_PTR, buf, 0x800);
    PS_CHECK(n < 0); // no name server answers offline
    net_free(ctx, buf);
    net_free(ctx, qbuf);
    return NS_NOTFOUND;
}

// nsdispatch (f_65d4) with no nsswitch configuration (f_659c finds none): the default sources, the hosts file and then
// the name servers, until one answers NS_SUCCESS; the last answer.
template <typename Files, typename Dns>
s32 nsdispatch(Files&& files, Dns&& dns)
{
    if (const s32 r = files(); r & NS_SUCCESS) return r;
    const s32 r = dns();
    return r ? r : NS_NOTFOUND;
}

// The hostent for a name that is an address (gethnamaddr.c's "fake up a hostent"): the name copied into the text
// buffer (strncpy, 0x401 bytes), no aliases, the address inet_pton left at +0x960.
u32 address_hostent(PPUContext& ctx, u32 name)
{
    u8* b = gb(ctx);
    const u32 c = context(ctx);
    const u32 n = std::min<u32>(g_strlen(b, name), 0x401);
    const std::string text(reinterpret_cast<const char*>(b + name), n);
    std::memcpy(b + c + kHostBuf, text.data(), n);
    std::memset(b + c + kHostBuf + n, 0, 0x402 - n);
    w32(b, c + kHostName, c + kHostBuf);
    w32(b, c + kHostAliasList, c + kHostAliases);
    w32(b, c + kHostAliases, 0);
    w32(b, c + kHostAddrPtrs, c + kHostAddr);
    w32(b, c + kHostAddrPtrs + 4, 0);
    w32(b, c + kHostAddrList, c + kHostAddrPtrs);
    return c + kHost;
}

// gethostbyname2 (f_3fdc): a name that is an address of the family (digits and dots; or hex digits, colons and dots
// with a colon) gives a hostent of its own (or HOST_NOT_FOUND when it is not one), unless it ends with a dot; any other
// name is looked up in the hosts file and then asked of the name servers.
u32 gethostbyname2(PPUContext& ctx, u32 name, s32 af)
{
    u8* b = gb(ctx);
    u32 size = 0;
    if (af == SYS_NET_AF_INET) size = 4;
    else if (af == SYS_NET_AF_INET6) size = 16;
    else {
        set_h_errno(ctx, NETDB_INTERNAL);
        set_errno(ctx, SYS_NET_EAFNOSUPPORT);
        return 0;
    }
    w32(b, context(ctx) + kHostType, static_cast<u32>(af));
    w32(b, context(ctx) + kHostLength, size);
    // (a name with no dot could be a user's alias: libnet's hostalias has none)
    const u8 first = b[name];
    const auto address = [&](bool v4) -> u32 {
        if (inet_pton(ctx, af, name, context(ctx) + kHostAddr) <= 0) {
            set_h_errno(ctx, HOST_NOT_FOUND);
            return 0;
        }
        const u32 hp = address_hostent(ctx, name);
        if (v4 && (res_options(ctx) & RES_USE_INET6)) {
            PS_FATAL(kChan, "gethostbyname2: RES_USE_INET6 (set only by res_init) is not pspocket's own");
        }
        set_h_errno(ctx, NETDB_SUCCESS);
        return hp;
    };
    bool numeric = is_digit(first);
    if (numeric) {
        u32 i = 1;
        for (; b[name + i]; i++) {
            if (!is_digit(b[name + i]) && b[name + i] != '.') {
                numeric = false;
                break;
            }
        }
        if (numeric && b[name + i - 1] != '.') return address(true);
    }
    const bool colon = (is_xdigit(first) && g_strpbrk(b, name, ":")) || first == ':';
    if (colon) {
        u32 i = 0;
        bool ok = true;
        for (; b[name + i]; i++) {
            const u8 ch = b[name + i];
            if (!is_xdigit(ch) && ch != ':' && ch != '.') {
                ok = false;
                break;
            }
        }
        if (ok && b[name + i - 1] != '.') return address(false);
    }
    set_h_errno(ctx, NETDB_INTERNAL);
    u32 hp = 0;
    const s32 r = nsdispatch([&] { return files_byname(ctx, name, af, &hp); }, [&] { return dns_byname(ctx, name, af); });
    if (r != NS_SUCCESS) return 0;
    set_h_errno(ctx, NETDB_SUCCESS);
    return hp;
}

// gethostbyname(name): the context's hostent for the name (IPv6 first with RES_USE_INET6), or NULL with h_errno; a
// null name EINVAL, an aborted context ENOBUFS (both NETDB_INTERNAL).
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
    if (!guest_str(ctx, name, "gethostbyname: name")) {
        set_h_errno(ctx, NETDB_INTERNAL);
        return 0;
    }
    res_ready(ctx);
    if (res_options(ctx) & RES_USE_INET6) {
        if (const u32 hp = gethostbyname2(ctx, name, SYS_NET_AF_INET6)) return hp;
    }
    return gethostbyname2(ctx, name, SYS_NET_AF_INET);
}

// gethostbyaddr(addr, len, type): the context's hostent for an address (an IPv4-mapped or -compatible IPv6 address as
// its IPv4 one; a link- or site-local one is never found), or NULL with h_errno; a null address or a length not the
// type's EINVAL, another type EAFNOSUPPORT, an aborted context ENOBUFS (NETDB_INTERNAL).
u32 gethostbyaddr(PPUContext& ctx, u32 addr, u32 len, s32 type)
{
    u8* b = gb(ctx);
    const auto fail = [&](u32 e) {
        set_errno(ctx, e);
        set_h_errno(ctx, NETDB_INTERNAL);
        return 0u;
    };
    if (!addr) return fail(SYS_NET_EINVAL);
    if (r32(b, context(ctx) + kCtxAborted)) return fail(SYS_NET_ENOBUFS);
    if (type == SYS_NET_AF_INET6 && len == 16) {
        if (!usable(ctx, addr, 16, "gethostbyaddr: addr")) {
            set_h_errno(ctx, NETDB_INTERNAL);
            return 0;
        }
        const u8 scope = b[addr + 1] & 0xC0;
        if (b[addr] == 0xFE && (scope == 0x80 || scope == 0xC0)) {
            set_h_errno(ctx, HOST_NOT_FOUND);
            return 0;
        }
        const u32 w2 = r32(b, addr + 8), w3 = r32(b, addr + 12);
        if (!r32(b, addr) && !r32(b, addr + 4) && (w2 == 0xFFFF || (!w2 && w3 != 0 && w3 != 1))) {
            addr += 12;
            len = 4;
            type = SYS_NET_AF_INET;
        }
    }
    if (type == SYS_NET_AF_INET6) {
        if (len != 16) return fail(SYS_NET_EINVAL);
    } else if (type == SYS_NET_AF_INET) {
        if (len != 4) return fail(SYS_NET_EINVAL);
        if (!usable(ctx, addr, 4, "gethostbyaddr: addr")) {
            set_h_errno(ctx, NETDB_INTERNAL);
            return 0;
        }
    } else {
        return fail(SYS_NET_EAFNOSUPPORT);
    }
    set_h_errno(ctx, NETDB_INTERNAL);
    u32 hp = 0;
    const s32 r = nsdispatch([&] { return files_byaddr(ctx, addr, len, type, &hp); }, [&] { return dns_byaddr(ctx, addr, type); });
    if (r != NS_SUCCESS) return 0;
    set_h_errno(ctx, NETDB_SUCCESS);
    return hp;
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
        f("gethostbyaddr", &thunk<&gethostbyaddr>),
        f("_sys_net_lib_sysctl", &thunk<&_sys_net_lib_sysctl>),
        f("sys_net_get_if_list", &thunk<&sys_net_get_if_list>),
        f("sys_net_show_ifconfig", &thunk<&sys_net_show_ifconfig>),
        f("_sys_net_lib_ioctl", &thunk<&_sys_net_lib_ioctl>),
        f("_sys_net_lib_bnet_control", &thunk<&_sys_net_lib_bnet_control>),
        f("sys_net_open_dump", &thunk<&sys_net_open_dump>),
        f("sys_net_read_dump", &thunk<&sys_net_read_dump>),
        f("sys_net_close_dump", &thunk<&sys_net_close_dump>),
        f("sys_net_get_test_param", &thunk<&sys_net_get_test_param>),
        f("sys_net_set_test_param", &thunk<&sys_net_set_test_param>),
        f("sys_net_get_netemu_test_param", &thunk<&sys_net_get_netemu_test_param>),
        f("sys_net_set_netemu_test_param", &thunk<&sys_net_set_netemu_test_param>),
        f("sys_net_get_udpp2p_test_param", &thunk<&sys_net_get_udpp2p_test_param>),
        f("sys_net_set_udpp2p_test_param", &thunk<&sys_net_set_udpp2p_test_param>),
        f("_sce_net_add_name_server", &thunk<&_sce_net_add_name_server>),
        f("_sce_net_add_name_server_with_char", &thunk<&_sce_net_add_name_server_with_char>),
        f("_sce_net_set_name_server", &thunk<&_sce_net_set_name_server>),
        f("_sce_net_get_name_server", &thunk<&_sce_net_get_name_server>),
        f("sys_net_get_name_server", &thunk<&sys_net_get_name_server>),
        f("sys_net_set_lib_name_server", &thunk<&sys_net_set_lib_name_server>),
        f("sys_net_get_lib_name_server", &thunk<&sys_net_get_lib_name_server>),
        f("sys_net_show_nameserver", &thunk<&sys_net_show_nameserver>),
        f("_sce_net_flush_route", &thunk<&_sce_net_flush_route>),
        f("sys_net_get_routing_table_af", &thunk<&sys_net_get_routing_table_af>),
        f("sys_net_show_route", &thunk<&sys_net_show_route>),
        f("_sce_net_set_default_gateway", &thunk<&_sce_net_set_default_gateway>),
        f("_sce_net_set_ip_and_mask", &thunk<&_sce_net_set_ip_and_mask>),
        f("_sys_net_lib_if_nametoindex", &thunk<&_sys_net_lib_if_nametoindex>),
        f("sys_net_if_ctl", &thunk<&sys_net_if_ctl>),
        f("sys_netset_open", &thunk<&sys_netset_open>),
        f("sys_netset_close", &thunk<&sys_netset_close>),
        f("sys_netset_abort", &thunk<&sys_netset_abort>),
        f("sys_netset_get_status", &thunk<&sys_netset_get_status>),
        f("sys_netset_get_key_value", &thunk<&sys_netset_get_key_value>),
        f("sys_netset_get_if_id", &thunk<&sys_netset_get_if_id>),
        f("sys_netset_if_up", &thunk<&sys_netset_if_up>),
        f("sys_netset_if_down", &thunk<&sys_netset_if_down>),
        PS_HLE_INTERNAL(0x66AFE280, _pspocket_net_thread_atexit),
    });
}

} // namespace pspocket::hle
