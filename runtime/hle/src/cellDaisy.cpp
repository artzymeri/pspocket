// cellDaisy (D-538): the out-of-line part of the SDK's Daisy library (cell/daisy: pipes between PPU threads and SPU
// programs, whose templates the game compiles in), as libsre.sprx (fw 4.92) has it, read from its code (the reference,
// D-046) and compared with it running on pspocket (tests/hle/test_daisy.cpp): the Lock a pipe's local queue runs on,
// the calls a PPU end of a pipe makes on an LFQueue2 (a cellSync LFQueue grown by Daisy's fields), and the
// ScatterGatherInterlock that holds a PPU thread until SPU threads (or other PPU threads) are done with a buffer.
// libsre exports every function under two names (_QN... and _ZN...); both are registered. RPCS3 leaves the library as
// todo stubs (rpcs3/Emu/Cell/Modules/cellDaisy.cpp; its cellDaisy.h gives the error codes).
//
// Layouts (byte offsets, big-endian):
//   Lock (0x20): +0 u16 head (consumed), +2 u16 consumes completed past head (bit 15: head + 1), +4 u32 next pointer to
//     consume, +8 u16 tail (produced), +0xA u16 produces completed past tail, +0xC u32 next pointer to produce, +0x10 u32
//     depth, +0x14 u32 state (0 new, 1 set up, 2 open for pushing, 3 closed), +0x18 u32 open producers, +0x1C u32 open
//     consumers. Pointers run modulo 2 * depth. Plain loads and stores, as libsre's: the pipe holds a lock around them.
//   LFQueue2 (0x100): a CellSyncLFQueue (cellSync.cpp has its layout) whose m_v1 (+0x28) counts open pushers (high
//     half) and poppers (low half), then +0x80 u8 open pushers, +0x81 u8 state (3 open, 4 closed), +0x82 u16 waiters
//     to signal at close (bit 15: +0x90's first), +0x84 u16 pop waiters not yet signalled (bit 15: the first entries of
//     +0xB0 and +0xD0), +0x86 u16 the signalled ones, +0x90 u16 x16 the close waiters' IDs, +0xB0 u16 x16 the pop
//     waiters' IDs, +0xD0 u16 x16 the pointers they wait for.
//   AtomicInterlock (its own line): +0 u32 count, +4 u32 a PPU thread waits in probe (bit 31) or the release callback's
//     argument + 1, +8 u64 the lv2 event queue probe sleeps on, +0x10 u32 the SPU threads' event port (64: none).
//   ScatterGatherInterlock (0x3C): +0 the AtomicInterlock, +4 u32 the count to reach, +8 u8 1: SPU threads release it,
//     +0xC u32 the release callback's argument, +0x10 s32 the event port (-1: not made yet), +0x14 u32 SPU threads,
//     +0x18 u32 x8 their IDs, +0x38 the release callback.
//
// Every ldarx/stdcx. or lwarx/stwcx. pair of libsre's is a snapshot of the 128-byte line through the shared reservation
// system and a conditional store (Reserve, as cellSync2's), so a store by any processor in between fails it as on the
// hardware; plain loads and stores are plain, and the stores tell the reservation system (stored()). Blocking waits are
// libsre's lv2 calls (sys_event_queue_receive and the port calls) with libsre's arguments, its stack addresses included.
//
// Deviations: libsre's assertions (its "PS3 SDK INTERNAL ASSERTION FAILURE" and abort) are fatal logs with the same
// text. Objects that are not guest memory are refused with CELL_DAISY_ERROR_INVAL (functions with no result: nothing
// done) and the call site logged, where libsre would fault. release's "sreEventPortCreate time out" (an event port
// refused 1,000 times with EAGAIN) is a log line, not text on the TTY.

#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "spurs.hpp" // spurs::sync_lfqueue_complete_pop_pointer

namespace pspocket::hle {

// liblv2's _sys_snprintf on a call's variadic arguments after `fixed` named ones (sysPrxForUser_libc.cpp).
s32 liblv2_snprintf_call(PPUContext& ctx, std::string_view what, u32 dst, u32 count, u32 fmt, u32 fixed);

namespace {

constexpr std::string_view kChan = "cellDaisy";

// RPCS3 cellDaisy.h.
enum : u32 {
    CELL_DAISY_ERROR_AGAIN = 0x80410511,
    CELL_DAISY_ERROR_INVAL = 0x80410512,
    CELL_DAISY_ERROR_PERM = 0x80410509,
    CELL_DAISY_ERROR_STAT = 0x8041050F,
};
constexpr s32 kPtrUnavailable = 0x40000001, kPtrTerminated = 0x40000002;

constexpr u32 kLockSize = 0x20, kQueueSize = 0x100, kInterlockSize = 0x3C, kAtomicSize = 0x18;
constexpr u32 SYS_SPU_THREAD_EVENT_USER = 1, SYS_EVENT_PORT_LOCAL = 1, SYS_EVENT_QUEUE_DESTROY_FORCE = 1;
constexpr u32 SYS_SYNC_PRIORITY = 2, SYS_PPU_QUEUE = 1;

s32 error(u32 code) { return static_cast<s32>(code); }

// libsre's lwarx/ldarx and stwcx./stdcx. on one line (cellSync2.cpp's Reserve), with the line's snapshot for the plain
// loads libsre makes between them from the same line (a store that changes them fails the conditional store anyway).
class Reserve {
public:
    u64 ldarx(u8* base, u32 addr)
    {
        addr_ = addr;
        version_ = ps_res_acquire(base, addr, line_);
        return ps_ld64(line_ + (addr & (PS_RES_LINE_SIZE - 1)));
    }
    u32 lwarx(u8* base, u32 addr)
    {
        addr_ = addr;
        version_ = ps_res_acquire(base, addr, line_);
        return ps_ld32(line_ + (addr & (PS_RES_LINE_SIZE - 1)));
    }
    bool stdcx(u8* base, u64 value) const { return ps_res_store_conditional(base, addr_, 8, value, version_, line_); }
    bool stwcx(u8* base, u32 value) const { return ps_res_store_conditional(base, addr_, 4, value, version_, line_); }
    // A u16 of the reserved line, by its guest address.
    u16 lhz(u32 addr) const { return ps_ld16(line_ + (addr & (PS_RES_LINE_SIZE - 1))); }

private:
    alignas(16) u8 line_[PS_RES_LINE_SIZE];
    u64 version_ = 0;
    u32 addr_ = 0;
};

// A plain store's notice to the reservation system (it breaks other processors' reservations on the line, as on the
// hardware).
void stored(u32 addr, u32 size) { ps_res_notify_write(addr, size); }

bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// Whether the object is guest memory; if not, the call site is logged (libsre would fault).
bool usable(PPUContext& ctx, std::string_view fn, std::string_view what, u32 addr, u64 size)
{
    if (guest_range(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{}: the {} at {:#x} ({} bytes) is not guest memory; refused", fn,
        what, addr, size));
    return false;
}

// libsre's assertion: "PS3 SDK INTERNAL ASSERTION FAILURE: libdaisy, version 492000, file F, line L, function ..."
// printed, then "Aborted." and the process ends (sysPrxForUser_9FB6228E).
[[noreturn]] void assertion(std::string_view file, u32 line, std::string_view function, u32 rc)
{
    PS_FATAL(kChan, "PS3 SDK INTERNAL ASSERTION FAILURE: libdaisy, version 492000, file {}, line {}, function \"{}\" "
        "(the lv2 call answered {:#x}). Aborted.", file, line, function, rc);
}

u32 r32(u8* b, u32 a) { return ps_read32(b, a); }
u16 r16(u8* b, u32 a) { return ps_read16(b, a); }
void w32(u8* b, u32 a, u32 v) { ps_write32(b, a, v); }
void w16(u8* b, u32 a, u16 v) { ps_write16(b, a, v); }

// ============================================================================================================== Lock

s32 lock_initialize(PPUContext& ctx, u32 lock, u32 depth)
{
    if (depth & 0xFFFF8000) return error(CELL_DAISY_ERROR_INVAL);
    if (!usable(ctx, "Lock::initialize", "lock", lock, kLockSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    if (r32(b, lock + 0x14) == 0) {
        w32(b, lock + 0x10, depth);
        w32(b, lock + 0x14, 1);
        w32(b, lock + 0x1C, 0);
        w16(b, lock + 0, 0);
        w16(b, lock + 2, 0);
        w32(b, lock + 4, 0);
        w16(b, lock + 8, 0);
        w16(b, lock + 0xA, 0);
        w32(b, lock + 0xC, 0);
        w32(b, lock + 0x18, 0);
        stored(lock, kLockSize);
        return lv2::CELL_OK;
    }
    return r32(b, lock + 0x10) == depth ? lv2::CELL_OK : error(CELL_DAISY_ERROR_INVAL);
}

void lock_push_open(PPUContext& ctx, u32 lock)
{
    if (!usable(ctx, "Lock::pushOpen", "lock", lock, kLockSize)) return;
    u8* b = guest_base(ctx);
    w32(b, lock + 0x14, 2);
    w32(b, lock + 0x18, r32(b, lock + 0x18) + 1);
    stored(lock + 0x14, 8);
}

void lock_push_close(PPUContext& ctx, u32 lock)
{
    if (!usable(ctx, "Lock::pushClose", "lock", lock, kLockSize)) return;
    u8* b = guest_base(ctx);
    w32(b, lock + 0x18, r32(b, lock + 0x18) - 1);
    stored(lock + 0x18, 4);
}

void lock_pop_open(PPUContext& ctx, u32 lock)
{
    if (!usable(ctx, "Lock::popOpen", "lock", lock, kLockSize)) return;
    u8* b = guest_base(ctx);
    w32(b, lock + 0x1C, r32(b, lock + 0x1C) + 1);
    stored(lock + 0x1C, 4);
}

// The last consumer to close closes the lock (state 3).
void lock_pop_close(PPUContext& ctx, u32 lock)
{
    if (!usable(ctx, "Lock::popClose", "lock", lock, kLockSize)) return;
    u8* b = guest_base(ctx);
    const u32 left = r32(b, lock + 0x1C) - 1;
    w32(b, lock + 0x1C, left);
    stored(lock + 0x1C, 4);
    if (left) return;
    w32(b, lock + 0x14, 3);
    stored(lock + 0x14, 4);
}

// The next pointer to consume, moved on (0 past 2 * depth - 1), and that pointer back; DAISY_PTR_UNAVAILABLE while
// nothing is produced and producers may still push (state 2 with producers open, or any other state), or 16 consumes
// are under way; DAISY_PTR_TERMINATED when nothing is left and the producers are gone.
s32 lock_get_next_head_pointer(PPUContext& ctx, u32 lock)
{
    if (!usable(ctx, "Lock::getNextHeadPointer", "lock", lock, kLockSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    const u32 next = r32(b, lock + 4), tail = r16(b, lock + 8), head = r16(b, lock + 0), depth = r32(b, lock + 0x10);
    const u32 available = next > tail ? (tail + depth) - (next - depth) : tail - next;
    const u32 under_way = next < head ? (next + depth) - (head - depth) : next - head;
    const bool drained = r32(b, lock + 0x14) == 2 && r32(b, lock + 0x18) == 0;
    const bool wait = available == 0 && !drained;
    if (wait || under_way == 0x10) return kPtrUnavailable;
    if (drained && available == 0) return kPtrTerminated;
    u32 moved = next + 1;
    if (!(2 * depth > moved)) moved = 0;
    w32(b, lock + 4, moved);
    stored(lock + 4, 4);
    return static_cast<s32>(moved);
}

// The next pointer to produce, moved on, and that pointer back; DAISY_PTR_UNAVAILABLE when the queue is full or 16
// produces are under way.
s32 lock_get_next_tail_pointer(PPUContext& ctx, u32 lock)
{
    if (!usable(ctx, "Lock::getNextTailPointer", "lock", lock, kLockSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    const u32 next = r32(b, lock + 0xC), depth = r32(b, lock + 0x10), head = r16(b, lock + 0), tail = r16(b, lock + 8);
    const u32 used = next < head ? (next + depth) - (head - depth) : next - head;
    const u32 under_way = next < tail ? (next + depth) - (tail - depth) : next - tail;
    if (used == depth || under_way == 0x10) return kPtrUnavailable;
    u32 moved = next + 1;
    if (2 * depth <= moved) moved = 0;
    w32(b, lock + 0xC, moved);
    stored(lock + 0xC, 4);
    return static_cast<s32>(moved);
}

// A completion: with more than one of the side open, `pointer`'s bit is set in the side's bitmap and the end moves past
// every completion in a row (the bitmap shifted along); with one, the end moves on by one.
void lock_complete(u8* b, u32 lock, u32 pointer, u32 end, u32 bits, u32 open)
{
    const u32 at = r16(b, lock + end), depth = r32(b, lock + 0x10);
    u32 step = 1;
    if (r32(b, lock + open) > 1) {
        const u32 d = at > pointer ? (depth + pointer) - (at - depth) : pointer - at;
        const u32 amount = (15 - d) & 0x3F; // slw: the low six bits, 0 from 32
        const u32 bit = (amount & 0x20) ? 0 : 1u << amount;
        const u32 set = r16(b, lock + bits) | bit;
        step = static_cast<u32>(std::countl_zero(~(set | 0xFFFF0000u))) - 16;
        w16(b, lock + bits, static_cast<u16>(set << step));
    }
    u32 moved = step + at;
    if (moved >= 2 * depth) moved -= 2 * depth;
    w16(b, lock + end, static_cast<u16>(moved));
    stored(lock, kLockSize);
}

void lock_complete_consume(PPUContext& ctx, u32 lock, u32 pointer)
{
    if (!usable(ctx, "Lock::completeConsume", "lock", lock, kLockSize)) return;
    lock_complete(guest_base(ctx), lock, pointer, 0, 2, 0x1C);
}

// libsre then stores the next tail pointer back when it equals the new tail (the same value: nothing to do here).
void lock_complete_produce(PPUContext& ctx, u32 lock, u32 pointer)
{
    if (!usable(ctx, "Lock::completeProduce", "lock", lock, kLockSize)) return;
    lock_complete(guest_base(ctx), lock, pointer, 8, 0xA, 0x18);
}

// cell::Daisy::_snprintf(buffer, count, format, ...): liblv2's _sys_vsnprintf on the arguments.
s32 daisy_snprintf(PPUContext& ctx, u32 buffer, u32 count, u32 fmt)
{
    return liblv2_snprintf_call(ctx, "cell::Daisy::_snprintf", buffer, count, fmt, 3);
}

// ========================================================================================================== LFQueue2

namespace q2 {
constexpr u32 pop1 = 0, push1 = 8, depth = 0x14, bs = 0x20, direction = 0x24, open = 0x28, push2 = 0x30, hs1 = 0x32,
              ea_signal = 0x74, eq_id = 0x7C, ctl = 0x80, state = 0x81, close_ids = 0x90, waiter_ids = 0xB0,
              waiter_ptrs = 0xD0;
}
constexpr u32 CELL_SYNC_QUEUE_SPU2PPU = 1;

// The m_h3/m_h7 field (an lv2 event queue wait under way) of pop1/push1.
u64 with_event(u64 v, u64 e) { return (v & ~(u64{0xFFFF} << 16)) | (e << 16); }
u32 event_of(u64 v) { return static_cast<u32>(v >> 16) & 0xFFFF; }

// y - x on the ring of 2 * depth pointers, libsre's way: when x is past y (a signed compare), (depth + y) - (x - depth);
// in 32-bit arithmetic.
s32 ring_distance(s32 x, s32 y, u32 depth)
{
    const u32 ux = static_cast<u32>(x), uy = static_cast<u32>(y);
    return static_cast<s32>(x > y ? (depth + uy) - (ux - depth) : uy - ux);
}

// libsre's sys_event_queue_receive on the queue's lv2 event queue; `frame` is where libsre's own frame puts the event
// (below the caller's stack pointer), so the call's arguments are libsre's.
u32 wait_event(PPUContext& ctx, u32 queue, u32 frame)
{
    const u32 sp = static_cast<u32>(ctx.gpr[1]);
    return static_cast<u32>(lv2_syscall(ctx, 130, {r32(guest_base(ctx), queue + q2::eq_id), sp - frame, 0}));
}

// LFQueue2PushOpen: one more pusher (m_v1's high half, then +0x80), the queue open (+0x81 = 3).
void lfqueue2_push_open(PPUContext& ctx, u32 queue)
{
    if (!usable(ctx, "LFQueue2PushOpen", "queue", queue, kQueueSize)) return;
    u8* b = guest_base(ctx);
    Reserve r;
    for (;;) {
        const u32 v = r.lwarx(b, queue + q2::open);
        if (r.stwcx(b, (v & 0xFFFF) | ((v & 0xFFFF0000) + 0x10000))) break;
    }
    for (;;) {
        const u64 v = r.ldarx(b, queue + q2::ctl);
        const u64 pushers = ((v >> 56) + 1) & 0xFF;
        if (r.stdcx(b, (pushers << 56) | (u64{3} << 48) | (v & 0x0000FFFFFFFFFFFF))) break;
    }
}

// LFQueue2PushClose(queue, fpSendSignal): one pusher fewer (STAT when none is open); the last one signals every SPU
// waiter in the push side's ring (taken from its head, as cellSync's completions take them) through
// fpSendSignal(eaSignal, its ID).
s32 lfqueue2_push_close(PPUContext& ctx, u32 queue, u32 signal)
{
    if (!usable(ctx, "LFQueue2PushClose", "queue", queue, kQueueSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    Reserve r;
    for (;;) {
        const u32 v = r.lwarx(b, queue + q2::open);
        const u32 pushers = v >> 16;
        if (pushers == 0) return error(CELL_DAISY_ERROR_STAT);
        if (r.stwcx(b, (v & 0xFFFF) | ((pushers - 1) << 16))) break;
    }
    u8 left = 0;
    for (;;) {
        const u64 v = r.ldarx(b, queue + q2::ctl);
        left = static_cast<u8>((v >> 56) - 1);
        if (r.stdcx(b, (u64{left} << 56) | (v & 0x00FFFFFFFFFFFFFF))) break;
    }
    if (left) return lv2::CELL_OK;
    for (;;) {
        bool empty = false;
        u16 waiter = 0;
        for (;;) {
            const u32 v = r.lwarx(b, queue + q2::push2);
            const u32 pack = v >> 16;
            const u32 head = (pack >> 10) & 0x1F, tail = pack & 0x1F;
            empty = head == tail;
            u32 next = pack;
            if (!empty) {
                const u32 slot = head > 14 ? head - 15 : head;
                waiter = r.lhz(queue + q2::hs1 + 2 * slot);
                next = ((pack & 0x83FF) | (head != 29 ? (head + 1) << 10 : 0)) & 0xFFFF;
            }
            if (r.stwcx(b, (next << 16) | (v & 0xFFFF))) break;
        }
        if (empty) return lv2::CELL_OK;
        if (!usable(ctx, "LFQueue2PushClose", "signal function", signal, 8)) return error(CELL_DAISY_ERROR_INVAL);
        call_guest(ctx, signal, {r32(b, queue + q2::ea_signal), waiter});
    }
}

void lfqueue2_pop_open(PPUContext& ctx, u32 queue)
{
    if (!usable(ctx, "LFQueue2PopOpen", "queue", queue, kQueueSize)) return;
    u8* b = guest_base(ctx);
    Reserve r;
    for (;;) {
        const u32 v = r.lwarx(b, queue + q2::open);
        if (r.stwcx(b, (v & 0xFFFF0000) | ((v + 1) & 0xFFFF))) break;
    }
}

// libsre's close: the queue closed (+0x81 = 4) and the close waiters' mask taken (+0x82 = 0), then each waiter in it
// signalled, +0x90's first entry first, until a signal fails (its result back).
s32 close_signal(PPUContext& ctx, u32 queue, u32 signal)
{
    u8* b = guest_base(ctx);
    Reserve r;
    u32 mask = 0;
    for (;;) {
        u64 v = r.ldarx(b, queue + q2::ctl);
        v = (v & ~(u64{0xFF} << 48)) | (u64{4} << 48);
        mask = static_cast<u32>(v >> 32) & 0xFFFF;
        v &= ~(u64{0xFFFF} << 32);
        if (r.stdcx(b, v)) break;
    }
    for (s32 i = 15; i >= 0; i--) {
        if (!((mask >> i) & 1)) continue;
        if (!usable(ctx, "LFQueue2PopClose", "signal function", signal, 8)) return error(CELL_DAISY_ERROR_INVAL);
        const s32 rc = static_cast<s32>(call_guest(ctx, signal, {r32(b, queue + q2::ea_signal),
            r16(b, queue + q2::close_ids + 2 * static_cast<u32>(15 - i))}));
        if (rc) return rc;
    }
    return lv2::CELL_OK;
}

// LFQueue2PopClose(queue, fpSendSignal): one popper fewer (STAT when none is open); when none is left the queue is
// closed and its close waiters signalled.
s32 lfqueue2_pop_close(PPUContext& ctx, u32 queue, u32 signal)
{
    if (!usable(ctx, "LFQueue2PopClose", "queue", queue, kQueueSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    Reserve r;
    for (;;) {
        const u32 v = r.lwarx(b, queue + q2::open);
        if ((v & 0xFFFF) == 0) return error(CELL_DAISY_ERROR_STAT);
        if (r.stwcx(b, (v & 0xFFFF0000) | ((v - 1) & 0xFFFF))) break;
    }
    if (r16(b, queue + q2::open + 2) != 0) return lv2::CELL_OK;
    b[queue + q2::state] = 4;
    stored(queue + q2::state, 1);
    return close_signal(ctx, queue, signal);
}

// LFQueue2GetPopPointer(queue, &pointer, isBlocking): the next pointer to pop (pop1's m_h4) when the pushes completed
// (push1's m_h5) are ahead of it: written to *pointer and m_h4 moved on. Otherwise 3 when the queue is closed and no
// pusher is open; AGAIN without blocking; blocking, a sleep on the queue's lv2 event queue (isBlocking 3 marks the wait
// in m_h3 first), then the next try. A wait is marked (m_h3 not 0) whatever the outcome: the call sleeps on the event
// queue, then clears the mark and tries again. Only for SPU2PPU queues (PERM), and blocking needs an attached event
// queue (STAT while m_bs is all ones).
s32 lfqueue2_get_pop_pointer(PPUContext& ctx, u32 queue, u32 pointer, u32 blocking)
{
    constexpr std::string_view fn = "LFQueue2GetPopPointer";
    if (!usable(ctx, fn, "queue", queue, kQueueSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    if (r32(b, queue + q2::direction) != CELL_SYNC_QUEUE_SPU2PPU) return error(CELL_DAISY_ERROR_PERM);
    if (blocking && r32(b, queue + q2::bs) == 0xFFFFFFFF) return error(CELL_DAISY_ERROR_STAT);
    const u32 depth = r32(b, queue + q2::depth);
    const s32 end = static_cast<s32>(depth << 1);
    Reserve r;
    for (;;) {
        u64 v = r.ldarx(b, queue + q2::pop1);
        const s32 next = static_cast<s16>(v & 0xFFFF);
        const s32 available = ring_distance(next, r.lhz(queue + q2::push1), depth);
        s32 rc = lv2::CELL_OK;
        bool store = true, wait = false;
        if (available > 0) {
            if (!usable(ctx, fn, "pointer", pointer, 4)) return error(CELL_DAISY_ERROR_INVAL);
            w32(b, pointer, static_cast<u32>(next));
            stored(pointer, 4);
            const u64 moved = next + 1 >= end ? 0 : ((v + 1) & 0xFFFF);
            v = (v & ~u64{0xFFFF}) | moved;
        } else {
            const u64 ctl = ps_read64(b, queue + q2::ctl);
            if (((ctl >> 48) & 0xFF) > 2 && (ctl >> 56) == 0) {
                rc = 3;
            } else if (!blocking) {
                rc = error(CELL_DAISY_ERROR_AGAIN);
                store = false;
            } else if (blocking == 3) {
                v = with_event(v, 3);
            } else {
                store = false;
                wait = true;
            }
        }
        if (store && !r.stdcx(b, v)) continue;
        if (!wait && event_of(v) == 0) return rc;
        if (const u32 e = wait_event(ctx, queue, 192 - 112); e != lv2::CELL_OK)
            assertion("lfqueue2GetPopPtr.cpp", 81, "LFQueue2GetPopPointer", e);
        for (;;) {
            const u64 w = r.ldarx(b, queue + q2::pop1);
            if (r.stdcx(b, with_event(w, 0))) break;
        }
    }
}

// LFQueue2CompletePopPointer(queue, pointer, fpSendSignal, noQueueFull): cellSync's _cellSyncLFQueueCompletePopPointer
// (its AGAIN, INVAL and BUSY turned into Daisy's: the low byte kept, 0x80410510 set), then the first pop waiter (bit 15
// of +0x84 first) whose pointer is within the queue's depth past the pops completed and within 15 past the pushes
// completed is taken (its bit moved to +0x86) and signalled: fpSendSignal(eaSignal, its ID), whose result is returned.
s32 lfqueue2_complete_pop_pointer(PPUContext& ctx, u32 queue, s32 pointer, u32 signal, u32 no_queue_full)
{
    if (!usable(ctx, "LFQueue2CompletePopPointer", "queue", queue, kQueueSize)) return error(CELL_DAISY_ERROR_INVAL);
    s32 rc = spurs::sync_lfqueue_complete_pop_pointer(ctx, queue, pointer, signal, no_queue_full);
    if (rc != lv2::CELL_OK) {
        const u32 code = static_cast<u32>(rc);
        if (code == 0x80410101 || code == 0x8041010A || code == 0x80410102) rc = static_cast<s32>((code & 0xFF) | 0x80410510);
        return rc;
    }
    u8* b = guest_base(ctx);
    const s32 head = r16(b, queue + q2::pop1), pushed = r16(b, queue + q2::push1);
    const u32 depth = r32(b, queue + q2::depth);
    Reserve r;
    u32 found = 16;
    for (;;) {
        u64 v = r.ldarx(b, queue + q2::ctl);
        const u32 pending = static_cast<u32>(v >> 16) & 0xFFFF;
        found = 16;
        u32 bit = 0;
        for (u32 i = 0; i < 16; i++) {
            bit = 1u << (15 - i);
            if (!(pending & bit)) continue;
            const s32 w = r.lhz(queue + q2::waiter_ptrs + 2 * i);
            if (ring_distance(head, w, depth) < static_cast<s32>(depth) && ring_distance(pushed, w, depth) <= 15) {
                found = i;
                break;
            }
        }
        if (found < 16) {
            v = (v & 0xFFFFFFFF00000000) | (u64{(pending & ~bit) & 0xFFFF} << 16) | ((v | bit) & 0xFFFF);
        }
        if (r.stdcx(b, v)) break;
    }
    if (found == 16) return lv2::CELL_OK;
    if (!usable(ctx, "LFQueue2CompletePopPointer", "signal function", signal, 8)) return error(CELL_DAISY_ERROR_INVAL);
    return static_cast<s32>(call_guest(ctx, signal, {r32(b, queue + q2::ea_signal), r16(b, queue + q2::waiter_ids + 2 * found)}));
}

// LFQueue2HasUnfinishedConsumer(queue, flags): while the queue is closed (+0x81 above 2), whether the pops completed
// (pop1's m_h1) have reached the pushes completed (push1's m_h5): 0 when they have, or (flags other than bit 1) when one
// is left; else AGAIN, with a wait marked in m_h7 when an event queue is attached. Bit 1 of flags stores the mark; a
// marked wait (stored or found) sleeps on the lv2 event queue, clears the mark and looks again. 0 once not closed.
s32 lfqueue2_has_unfinished_consumer(PPUContext& ctx, u32 queue, u32 flags)
{
    if (!usable(ctx, "LFQueue2HasUnfinishedConsumer", "queue", queue, kQueueSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    const bool store = flags & 2, only = (flags & ~2u) == 0;
    Reserve r;
    while (b[queue + q2::state] > 2) {
        const u32 depth = r32(b, queue + q2::depth);
        u64 v = 0;
        s32 rc = lv2::CELL_OK;
        for (;;) {
            v = r.ldarx(b, queue + q2::push1);
            const s32 d = ring_distance(r.lhz(queue + q2::pop1), static_cast<s32>(v >> 48), depth);
            rc = lv2::CELL_OK;
            if (!((!only && d == 1) || d == 0)) {
                rc = error(CELL_DAISY_ERROR_AGAIN);
                if (r32(b, queue + q2::bs) != 0xFFFFFFFF) v = with_event(v, 3);
            }
            if (store && !r.stdcx(b, v)) continue;
            break;
        }
        if (event_of(v) == 0) return rc;
        wait_event(ctx, queue, 64);
        for (;;) {
            const u64 w = r.ldarx(b, queue + q2::push1);
            if (r.stdcx(b, with_event(w, 0))) break;
        }
    }
    return lv2::CELL_OK;
}

// ============================================================================================ ScatterGatherInterlock

namespace sgi {
constexpr u32 atomic = 0, size = 4, spu = 8, arg = 0xC, port = 0x10, spus = 0x14, ids = 0x18, callback = 0x38;
}
namespace ai {
constexpr u32 count = 0, queue = 8, port = 0x10; // +4: the waiting flag or the callback argument + 1
}

// The release callback's constructor: (atomic, size, argument, callback); the event port made at the first release a
// probe waits for.
void sgi_construct_callback(PPUContext& ctx, u32 self, u32 atomic, u32 size, u32 arg, u32 callback)
{
    if (!usable(ctx, "ScatterGatherInterlock::ScatterGatherInterlock", "interlock", self, kInterlockSize)) return;
    u8* b = guest_base(ctx);
    w32(b, self + sgi::callback, callback);
    b[self + sgi::spu] = 0;
    w32(b, self + sgi::atomic, atomic);
    w32(b, self + sgi::size, size);
    w32(b, self + sgi::arg, arg);
    w32(b, self + sgi::port, 0xFFFFFFFF);
    stored(self, kInterlockSize);
}

// The SPU threads' constructor: (atomic, size, SPU thread IDs, their number, SPU port): an lv2 event queue for probe
// ("_daiSGI", priority, PPU queue, size 1) whose ID goes to the atomic's +8; each SPU thread connected to it as a user
// event on the first port from `port` the first thread accepts (a port it refuses with other than ENOMEM or ESRCH is
// taken by something else: the next one), its number at the atomic's +0x10 (64 with no SPU threads).
void sgi_construct_spus(PPUContext& ctx, u32 self, u32 atomic, u32 size, u32 ids, u32 count, u32 port)
{
    constexpr std::string_view fn = "ScatterGatherInterlock::ScatterGatherInterlock";
    if (!usable(ctx, fn, "interlock", self, kInterlockSize) || !usable(ctx, fn, "atomic interlock", atomic, kAtomicSize))
        return;
    if (count && !usable(ctx, fn, "SPU thread IDs", ids, 4ull * count)) return;
    u8* b = guest_base(ctx);
    w32(b, self + sgi::port, 0xFFFFFFFF);
    b[self + sgi::spu] = 1;
    w32(b, self + sgi::size, size);
    w32(b, self + sgi::arg, 0);
    w32(b, self + sgi::callback, 0);
    w32(b, self + sgi::atomic, atomic);
    stored(self, kInterlockSize);
    // libsre's frame (0xE0 bytes): the queue's ID at +0x70, its attribute at +0x74.
    const u32 frame = static_cast<u32>(ctx.gpr[1]) - 0xE0;
    const u32 id = frame + 0x70, attr = frame + 0x74;
    w32(b, attr, SYS_SYNC_PRIORITY);
    w32(b, attr + 4, SYS_PPU_QUEUE);
    std::memcpy(b + attr + 8, "_daiSGI", 8);
    if (const u32 rc = static_cast<u32>(lv2_syscall(ctx, 128, {id, attr, 0, 1})); rc != lv2::CELL_OK)
        assertion("sync_pipe_interlock.cpp", 82, "ScatterGatherInterlock", rc);
    ps_write64(b, atomic + ai::queue, r32(b, id));
    stored(atomic + ai::queue, 8);
    if (count == 0) {
        w32(b, atomic + ai::port, 64);
    } else {
        u32 spup = port;
        u32 p = spup & 0xFF;
        if (p <= 63) {
            for (;;) {
                const u32 rc = static_cast<u32>(lv2_syscall(ctx, 191, {r32(b, ids),
                    static_cast<u32>(ps_read64(b, atomic + ai::queue)), SYS_SPU_THREAD_EVENT_USER, spup & 0xFF}));
                if (rc == lv2::CELL_OK) break;
                spup++;
                p = spup & 0xFF;
                if (rc - 0x80010004u <= 1) assertion("sync_pipe_interlock.cpp", 95, "ScatterGatherInterlock", rc);
                if (p == 64) break;
            }
        }
        if (p == 64) assertion("sync_pipe_interlock.cpp", 99, "ScatterGatherInterlock", 0);
        w32(b, self + sgi::ids, r32(b, ids));
        for (u32 i = 1; i < count; i++) {
            const u32 rc = static_cast<u32>(lv2_syscall(ctx, 191, {r32(b, ids + 4 * i),
                static_cast<u32>(ps_read64(b, atomic + ai::queue)), SYS_SPU_THREAD_EVENT_USER, spup & 0xFF}));
            if (rc != lv2::CELL_OK) assertion("sync_pipe_interlock.cpp", 109, "ScatterGatherInterlock", rc);
            w32(b, self + sgi::ids + 4 * i, r32(b, ids + 4 * i));
        }
        w32(b, atomic + ai::port, p);
    }
    stored(atomic + ai::port, 4);
    w32(b, self + sgi::spus, count);
    stored(self + sgi::spus, 4);
}

// The destructor: SPU threads' interlock: each thread disconnected (the last first; ESRCH, a thread gone, is fine) and
// the event queue destroyed (forced); the callback's: the event port, if one was made, disconnected (again while lv2
// says EBUSY; ENOTCONN is fine) and destroyed.
void sgi_destruct(PPUContext& ctx, u32 self)
{
    constexpr std::string_view fn = "~ScatterGatherInterlock";
    if (!usable(ctx, fn, "interlock", self, kInterlockSize)) return;
    u8* b = guest_base(ctx);
    const u32 atomic = r32(b, self + sgi::atomic);
    if (b[self + sgi::spu]) {
        if (!usable(ctx, fn, "atomic interlock", atomic, kAtomicSize)) return;
        for (u32 i = 0; r32(b, self + sgi::spus) > i; i++) {
            const u32 n = r32(b, self + sgi::spus);
            const u32 id = r32(b, self + sgi::spus + 4 * (n - i));
            const u32 rc = static_cast<u32>(lv2_syscall(ctx, 192, {id, SYS_SPU_THREAD_EVENT_USER,
                r32(b, atomic + ai::port) & 0xFF}));
            if (rc != lv2::CELL_OK && rc != lv2::CELL_ESRCH) assertion("sync_pipe_interlock.cpp", 141, fn, rc);
        }
        const u32 rc = static_cast<u32>(lv2_syscall(ctx, 129, {static_cast<u32>(ps_read64(b, atomic + ai::queue)),
            SYS_EVENT_QUEUE_DESTROY_FORCE}));
        if (rc != lv2::CELL_OK) assertion("sync_pipe_interlock.cpp", 144, fn, rc);
        return;
    }
    if (r32(b, self + sgi::port) == 0xFFFFFFFF) return;
    u32 rc;
    do rc = static_cast<u32>(lv2_syscall(ctx, 137, {r32(b, self + sgi::port)}));
    while (rc == lv2::CELL_EBUSY);
    if (rc != lv2::CELL_OK && rc != lv2::CELL_ENOTCONN) assertion("sync_pipe_interlock.cpp", 132, fn, rc);
    rc = static_cast<u32>(lv2_syscall(ctx, 135, {r32(b, self + sgi::port)}));
    if (rc != lv2::CELL_OK) assertion("sync_pipe_interlock.cpp", 135, fn, rc);
}

// proceedSequenceNumber: the atomic's count one up.
void sgi_proceed_sequence_number(PPUContext& ctx, u32 self)
{
    if (!usable(ctx, "ScatterGatherInterlock::proceedSequenceNumber", "interlock", self, kInterlockSize)) return;
    u8* b = guest_base(ctx);
    const u32 atomic = r32(b, self + sgi::atomic);
    if (!usable(ctx, "ScatterGatherInterlock::proceedSequenceNumber", "atomic interlock", atomic, 8)) return;
    Reserve r;
    for (;;) {
        const u64 v = r.ldarx(b, atomic);
        if (r.stdcx(b, (((v >> 32) + 1) << 32) | (v & 0xFFFFFFFF))) break;
    }
}

// probe(wait): the atomic's count while it is below the interlock's size; at the size, without waiting, the count;
// waiting, a wait marked (bit 31 of the atomic's +4) and a sleep on the event queue until a release sends an event, then
// the count again.
s32 sgi_probe(PPUContext& ctx, u32 self, u32 wait)
{
    constexpr std::string_view fn = "ScatterGatherInterlock::probe";
    if (!usable(ctx, fn, "interlock", self, kInterlockSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    const u32 atomic = r32(b, self + sgi::atomic);
    if (!usable(ctx, fn, "atomic interlock", atomic, kAtomicSize)) return error(CELL_DAISY_ERROR_INVAL);
    const u32 size = r32(b, self + sgi::size);
    if (r32(b, atomic + ai::count) < size) return static_cast<s32>(r32(b, atomic + ai::count));
    if (const u32 count = r32(b, atomic + ai::count); size > count) return static_cast<s32>(count);
    Reserve r;
    for (;;) {
        u64 v = 0;
        for (;;) {
            v = r.ldarx(b, atomic);
            if (wait && r32(b, self + sgi::size) <= static_cast<u32>(v >> 32)) v = (v & 0xFFFFFFFF00000000) | 0x80000000;
            if (r.stdcx(b, v)) break;
        }
        const u32 count = static_cast<u32>(v >> 32);
        if (!wait || r32(b, self + sgi::size) > count) return static_cast<s32>(count);
        const u32 sp = static_cast<u32>(ctx.gpr[1]);
        const u32 rc = static_cast<u32>(lv2_syscall(ctx, 130, {static_cast<u32>(ps_read64(b, atomic + ai::queue)),
            sp - 432 + 368, 0}));
        if (rc != lv2::CELL_OK) assertion("sync_pipe_interlock.cpp", 160, "probe", rc);
    }
}

// release: the atomic's count one down and its +4 taken (0 left). A probe waiting (bit 31): an event sent on the
// interlock's event port (made and connected to the atomic's event queue at the first such release); a callback
// argument (+4 = argument + 1): callback(+0xC's argument, argument), its result back; else 0.
s32 sgi_release(PPUContext& ctx, u32 self)
{
    constexpr std::string_view fn = "ScatterGatherInterlock::release";
    if (!usable(ctx, fn, "interlock", self, kInterlockSize)) return error(CELL_DAISY_ERROR_INVAL);
    u8* b = guest_base(ctx);
    const u32 atomic = r32(b, self + sgi::atomic);
    if (!usable(ctx, fn, "atomic interlock", atomic, kAtomicSize)) return error(CELL_DAISY_ERROR_INVAL);
    Reserve r;
    u32 taken = 0;
    for (;;) {
        const u64 v = r.ldarx(b, atomic);
        taken = static_cast<u32>(v);
        if (r.stdcx(b, (((v >> 32) - 1) & 0xFFFFFFFF) << 32)) break;
    }
    if (static_cast<s32>(taken) >= 0) {
        if (taken == 0) return lv2::CELL_OK;
        const u32 callback = r32(b, self + sgi::callback);
        if (!usable(ctx, fn, "callback", callback, 8)) return error(CELL_DAISY_ERROR_INVAL);
        return static_cast<s32>(call_guest(ctx, callback, {r32(b, self + sgi::arg), taken - 1}));
    }
    if (r32(b, self + sgi::port) == 0xFFFFFFFF) {
        u32 rc = 0;
        for (u32 tries = 0;; tries++) {
            rc = static_cast<u32>(lv2_syscall(ctx, 134, {self + sgi::port, SYS_EVENT_PORT_LOCAL, 0}));
            if (tries > 1000) {
                PS_LOG_ERROR(kChan, "{}: sreEventPortCreate time out ({:#x})", fn, rc);
                if (rc != lv2::CELL_OK) assertion("sync_pipe_interlock.cpp", 207, "release", rc);
            }
            if (rc != lv2::CELL_EAGAIN) break;
        }
        if (rc != lv2::CELL_OK) return static_cast<s32>(rc);
        rc = static_cast<u32>(lv2_syscall(ctx, 136, {r32(b, self + sgi::port),
            static_cast<u32>(ps_read64(b, atomic + ai::queue))}));
        if (rc != lv2::CELL_OK) assertion("sync_pipe_interlock.cpp", 215, "release", rc);
    }
    return static_cast<s32>(lv2_syscall(ctx, 138, {r32(b, self + sgi::port), 0, 0, 0}));
}

} // namespace

// For cellDaisy's names: libsre's (mangled) names on pspocket's functions.
#define PS_DAISY(nid, name, fn) ::pspocket::hle::Function{{}, name, nid, &::pspocket::hle::detail::thunk<&fn>, false}

void register_cellDaisy(Registry& r)
{
    r.add("cellDaisy", {
        PS_DAISY(0xA4CD494E, "_QN4cell5Daisy22ScatterGatherInterlockC2EPVNS0_16_AtomicInterlockEjPvPFiS5_jE", sgi_construct_callback),
        PS_DAISY(0xB190D6AD, "_ZN4cell5Daisy22ScatterGatherInterlockC2EPVNS0_16_AtomicInterlockEjPvPFiS5_jE", sgi_construct_callback),
        PS_DAISY(0x8D42FAE9, "_QN4cell5Daisy22ScatterGatherInterlockC1EPVNS0_16_AtomicInterlockEjPvPFiS5_jE", sgi_construct_callback),
        PS_DAISY(0x3190DFE4, "_ZN4cell5Daisy22ScatterGatherInterlockC1EPVNS0_16_AtomicInterlockEjPvPFiS5_jE", sgi_construct_callback),
        PS_DAISY(0xA98E3C4B, "_QN4cell5Daisy22ScatterGatherInterlock21proceedSequenceNumberEv", sgi_proceed_sequence_number),
        PS_DAISY(0xC0FAF91C, "_ZN4cell5Daisy22ScatterGatherInterlock21proceedSequenceNumberEv", sgi_proceed_sequence_number),
        PS_DAISY(0xDEB0D2E6, "_QN4cell5Daisy22ScatterGatherInterlock5probeEj", sgi_probe),
        PS_DAISY(0xD2BE66E6, "_ZN4cell5Daisy22ScatterGatherInterlock5probeEj", sgi_probe),
        PS_DAISY(0xFA46FD99, "_QN4cell5Daisy22ScatterGatherInterlock7releaseEv", sgi_release),
        PS_DAISY(0x1CE8C914, "_ZN4cell5Daisy22ScatterGatherInterlock7releaseEv", sgi_release),
        PS_DAISY(0xB8E85009, "_QN4cell5Daisy22ScatterGatherInterlockC2EPVNS0_16_AtomicInterlockEjPjjh", sgi_construct_spus),
        PS_DAISY(0x34234BF0, "_ZN4cell5Daisy22ScatterGatherInterlockC2EPVNS0_16_AtomicInterlockEjPjjh", sgi_construct_spus),
        PS_DAISY(0x20586BC0, "_QN4cell5Daisy22ScatterGatherInterlockC1EPVNS0_16_AtomicInterlockEjPjjh", sgi_construct_spus),
        PS_DAISY(0x1E5B1E23, "_ZN4cell5Daisy22ScatterGatherInterlockC1EPVNS0_16_AtomicInterlockEjPjjh", sgi_construct_spus),
        PS_DAISY(0x7E639D78, "_QN4cell5Daisy22ScatterGatherInterlockD2Ev", sgi_destruct),
        PS_DAISY(0x5A93E4E8, "_ZN4cell5Daisy22ScatterGatherInterlockD2Ev", sgi_destruct),
        PS_DAISY(0xED37E00A, "_QN4cell5Daisy22ScatterGatherInterlockD1Ev", sgi_destruct),
        PS_DAISY(0x8DACB8D0, "_ZN4cell5Daisy22ScatterGatherInterlockD1Ev", sgi_destruct),
        PS_DAISY(0x8FD3B122, "_QN4cell5Daisy4Lock10initializeEj", lock_initialize),
        PS_DAISY(0x2E8654F8, "_ZN4cell5Daisy4Lock10initializeEj", lock_initialize),
        PS_DAISY(0xD888F42A, "_QN4cell5Daisy4Lock8pushOpenEv", lock_push_open),
        PS_DAISY(0x706FEED9, "_ZN4cell5Daisy4Lock8pushOpenEv", lock_push_open),
        PS_DAISY(0xC6C5F3C6, "_QN4cell5Daisy4Lock9pushCloseEv", lock_push_close),
        PS_DAISY(0x7B79D6AA, "_ZN4cell5Daisy4Lock9pushCloseEv", lock_push_close),
        PS_DAISY(0x1CF82D42, "_QN4cell5Daisy4Lock7popOpenEv", lock_pop_open),
        PS_DAISY(0x17C35CC9, "_ZN4cell5Daisy4Lock7popOpenEv", lock_pop_open),
        PS_DAISY(0x4C34EC38, "_QN4cell5Daisy4Lock8popCloseEv", lock_pop_close),
        PS_DAISY(0x566C9460, "_ZN4cell5Daisy4Lock8popCloseEv", lock_pop_close),
        PS_DAISY(0xC3E29E69, "_QN4cell5Daisy4Lock18getNextHeadPointerEv", lock_get_next_head_pointer),
        PS_DAISY(0x2C967AA1, "_ZN4cell5Daisy4Lock18getNextHeadPointerEv", lock_get_next_head_pointer),
        PS_DAISY(0xF88E07DB, "_QN4cell5Daisy4Lock18getNextTailPointerEv", lock_get_next_tail_pointer),
        PS_DAISY(0x6FC83371, "_ZN4cell5Daisy4Lock18getNextTailPointerEv", lock_get_next_tail_pointer),
        PS_DAISY(0x163DAFEB, "_QN4cell5Daisy4Lock15completeConsumeEj", lock_complete_consume),
        PS_DAISY(0xBD091E26, "_ZN4cell5Daisy4Lock15completeConsumeEj", lock_complete_consume),
        PS_DAISY(0x933C2273, "_QN4cell5Daisy4Lock15completeProduceEj", lock_complete_produce),
        PS_DAISY(0x41DF5D21, "_ZN4cell5Daisy4Lock15completeProduceEj", lock_complete_produce),
        PS_DAISY(0x17348BA6, "_QN4cell5Daisy9_snprintfEPcjPKcz", daisy_snprintf),
        PS_DAISY(0x28ACB77F, "_ZN4cell5Daisy9_snprintfEPcjPKcz", daisy_snprintf),
        PS_DAISY(0x6F564A3D, "_QN4cell5Daisy21LFQueue2GetPopPointerEPNS0_8LFQueue2EPij", lfqueue2_get_pop_pointer),
        PS_DAISY(0x0D219671, "_ZN4cell5Daisy21LFQueue2GetPopPointerEPNS0_8LFQueue2EPij", lfqueue2_get_pop_pointer),
        PS_DAISY(0xCE618901, "_QN4cell5Daisy26LFQueue2CompletePopPointerEPNS0_8LFQueue2EiPFiPvjEj", lfqueue2_complete_pop_pointer),
        PS_DAISY(0x1BBCB5B9, "_ZN4cell5Daisy26LFQueue2CompletePopPointerEPNS0_8LFQueue2EiPFiPvjEj", lfqueue2_complete_pop_pointer),
        PS_DAISY(0x437151F0, "_QN4cell5Daisy16LFQueue2PushOpenEPNS0_8LFQueue2E", lfqueue2_push_open),
        PS_DAISY(0xF2034429, "_ZN4cell5Daisy16LFQueue2PushOpenEPNS0_8LFQueue2E", lfqueue2_push_open),
        PS_DAISY(0xA2E6C4B0, "_QN4cell5Daisy17LFQueue2PushCloseEPNS0_8LFQueue2EPFiPvjE", lfqueue2_push_close),
        PS_DAISY(0x0563627C, "_ZN4cell5Daisy17LFQueue2PushCloseEPNS0_8LFQueue2EPFiPvjE", lfqueue2_push_close),
        PS_DAISY(0x8921232C, "_QN4cell5Daisy15LFQueue2PopOpenEPNS0_8LFQueue2E", lfqueue2_pop_open),
        PS_DAISY(0xA286B24A, "_ZN4cell5Daisy15LFQueue2PopOpenEPNS0_8LFQueue2E", lfqueue2_pop_open),
        PS_DAISY(0x43838C2A, "_QN4cell5Daisy16LFQueue2PopCloseEPNS0_8LFQueue2EPFiPvjE", lfqueue2_pop_close),
        PS_DAISY(0x9581B8BC, "_ZN4cell5Daisy16LFQueue2PopCloseEPNS0_8LFQueue2EPFiPvjE", lfqueue2_pop_close),
        PS_DAISY(0x72DEE049, "_QN4cell5Daisy29LFQueue2HasUnfinishedConsumerEPNS0_8LFQueue2Ej", lfqueue2_has_unfinished_consumer),
        PS_DAISY(0x2BE4DDE1, "_ZN4cell5Daisy29LFQueue2HasUnfinishedConsumerEPNS0_8LFQueue2Ej", lfqueue2_has_unfinished_consumer),
    });
}

#undef PS_DAISY

} // namespace pspocket::hle
