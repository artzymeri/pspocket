// cellSync: Sony's synchronization library for PPU threads and SPU code that share guest memory: the mutex, the barrier,
// the reader-writer buffer (rwm), the queue and the lock-free queue (LFQueue). Every function RPCS3 implements, as its
// rpcs3/Emu/Cell/Modules/cellSync.cpp/.h has them (cited per function). RPCS3 itself runs the firmware's libsre, which
// holds cellSync, unless libsre is set to HLE (rpcs3/Emu/Cell/lv2/sys_prx.cpp g_prx_list), so its HLE code is rarely
// exercised: libsre (fw 4.92, read as the reference, D-046) was compared with it function by function, and where the two
// differ pspocket follows libsre (each place says so). The mutex is what Assassin's Creed (BLES00158) imports (run 090:
// its main thread polls cellSyncMutexTryLock, 24.3 million calls in 90 s while it was unimplemented; D-160).
//
// The LFQueue functions RPCS3 leaves as todo stubs returning CELL_OK are pspocket's own (D-625), made from what RPCS3's
// code shows of them, unverified (D-594: libsre is to be compared on the Mac): the second forms
// (_cellSyncLFQueueGetPushPointer2, CompletePushPointer2, GetPopPointer2, CompletePopPointer2) are the pointer
// functions of a queue between any processors (CELL_SYNC_QUEUE_ANY2ANY), which RPCS3's PushBody and PopBody call for
// one with the first forms' arguments; and _cellSyncLFQueueAttachLv2EventQueue and DetachLv2EventQueue make and connect
// the lv2 event queue (m_eq_id) a blocked PPU call of the pointer functions sleeps on, which SPU code sends to.
//
// Atomicity. Every structure is shared with SPU code (the SPU side of the library updates them with GETLLAR/PUTLLC) and
// other PPU threads, and lies in one 128-byte line (mutex and barrier 4 bytes, rwm 16, queue 32, LFQueue 128, each aligned
// to its size). Every update is one atomic update of that line through the shared reservation system (reservation_op,
// D-030), where RPCS3 uses host atomics on guest memory (atomic_t exchange, atomic_op, compare_and_swap): a host
// compare-and-swap is not atomic against guest code here, because a conditional store (stwcx., PUTLLC) that compared the
// line just before the CAS landed keeps the CAS's bytes and merges its own around them (D-160: a mutex with two holders).
// An update that finds the structure busy writes nothing: a game polling a Try function neither breaks other code's
// reservations on the line nor wakes its waiters (where libsre stores a word back unchanged, writing nothing is that
// conditional store failing, which the architecture allows).
//
// Waiting. Where RPCS3 spins (`while (!ctrl.atomic_op(...)) { if (ppu.test_stopped()) return 0; }`), the thread here
// sleeps until the structure's line changes: ps_res_wait_change wakes it at once on every update made through the
// reservation system (these functions', a PPU's stwcx., an SPU's PUTLLC and PUTLLUC, DMA PUTs). Plain guest stores don't
// move the line's version, so it also looks again every kWaitPollNs, and then notices the process ending: the thread
// unwinds (lv2::ThreadExit) where RPCS3's loops return 0.
//
// Memory order: reservation_op's snapshot is an acquire and its store follows a release fence, so what a thread wrote
// before an update (an entry it pushed, a buffer it wrote under the rwm) is seen by a thread whose later update reads that
// one. Where RPCS3 has atomic_fence_acq_rel (libsre an lwsync) it is kept.
//
// Common deviations (D-042, D-047): an object, entry or buffer that is not guest memory is refused with
// CELL_SYNC_ERROR_FAULT and the call site logged, where RPCS3 and a PS3 would fault; where RPCS3 ensure()s or throws and
// libsre asserts (it prints the condition and aborts the process), pspocket stops with a fatal log naming the check.

#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "spurs.hpp" // spurs::sync_lfqueue_initialize

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSync";

enum : u32 {
    CELL_SYNC_ERROR_AGAIN = 0x80410101,
    CELL_SYNC_ERROR_INVAL = 0x80410102,
    CELL_SYNC_ERROR_NOSYS = 0x80410103,
    CELL_SYNC_ERROR_PERM = 0x80410109,
    CELL_SYNC_ERROR_BUSY = 0x8041010A,
    CELL_SYNC_ERROR_ABORT = 0x8041010C,
    CELL_SYNC_ERROR_FAULT = 0x8041010D,
    CELL_SYNC_ERROR_STAT = 0x8041010F,
    CELL_SYNC_ERROR_ALIGN = 0x80410110,
    CELL_SYNC_ERROR_NULL_POINTER = 0x80410111,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// RPCS3 CHECK_SIZE_ALIGN of each structure.
constexpr u32 kMutexSize = 4, kMutexAlign = 4;
constexpr u32 kBarrierSize = 4, kBarrierAlign = 4;
constexpr u32 kRwmSize = 16, kRwmAlign = 16;
constexpr u32 kQueueSize = 32, kQueueAlign = 32;
constexpr u32 kLfQueueSize = 128, kLfQueueAlign = 128;

// How long a blocked call sleeps on its line before it looks again: plain guest stores don't wake it, and the process
// ending is noticed at this rate.
constexpr u64 kWaitPollNs = 1'000'000;

// An object's offset in its 128-byte line: where reservation_op's copy of the line holds it.
constexpr u32 line_offset(u32 addr) { return addr & (PS_RES_LINE_SIZE - 1); }

// A refused call, logged as RPCS3's error_code logs an error (BUSY and AGAIN, the normal answers of a busy structure, are
// not logged): the first 8 refusals, then every 1024th, because a game polling a bad object would flood the log. For
// memory that is not guest memory, the call site too.
s32 refuse(PPUContext& ctx, std::string_view fn, u32 object, u32 code, std::string_view why)
{
    static std::atomic<u64> refusals{0};
    if (const u64 n = ++refusals; n <= 8 || n % 1024 == 0) {
        const std::string message = std::format("{}({:#x}): {} (refusal {})", fn, object, why, n);
        if (code == CELL_SYNC_ERROR_FAULT) log_call_site(ctx, guest_base(ctx), message);
        else PS_LOG_ERROR(kChan, "{}", message);
    }
    return error(code);
}

s32 null_pointer(PPUContext& ctx, std::string_view fn, u32 object)
{
    return refuse(ctx, fn, object, CELL_SYNC_ERROR_NULL_POINTER, "CELL_SYNC_ERROR_NULL_POINTER");
}
s32 misaligned(PPUContext& ctx, std::string_view fn, u32 object)
{
    return refuse(ctx, fn, object, CELL_SYNC_ERROR_ALIGN, "CELL_SYNC_ERROR_ALIGN");
}
s32 invalid(PPUContext& ctx, std::string_view fn, u32 object)
{
    return refuse(ctx, fn, object, CELL_SYNC_ERROR_INVAL, "CELL_SYNC_ERROR_INVAL");
}

// Whether [addr, addr + size) is committed guest memory, checked per host page (entries and buffers can span several).
bool memory_ok(lv2::PpuThread& t, u64 addr, u64 size)
{
    if (!size) return true;
    if (addr + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < addr + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// OK when [addr, addr + size) is guest memory, else CELL_SYNC_ERROR_FAULT and the call site logged (D-042).
s32 check_memory(PPUContext& ctx, std::string_view fn, u32 object, std::string_view what, u64 addr, u64 size)
{
    if (memory_ok(thread_of(ctx), addr, size)) return lv2::CELL_OK;
    return refuse(ctx, fn, object, CELL_SYNC_ERROR_FAULT,
        std::format("the {} ({:#x}, {:#x} bytes) is not guest memory; refused with CELL_SYNC_ERROR_FAULT", what, addr, size));
}

// Sleeps until the line holding `addr` changed since `version` (ps_res_version, read before the look that found the
// object busy) or kWaitPollNs passed. The process ending unwinds the thread, where RPCS3's loops return 0 on
// ppu.test_stopped().
void wait_line(lv2::PpuThread& t, u32 addr, u64 version)
{
    if (lv2::wait_aborted(t.waiter)) throw lv2::ThreadExit{};
    ps_res_wait_change(addr, version, kWaitPollNs);
    if (lv2::wait_aborted(t.waiter)) throw lv2::ThreadExit{};
}

// The word at `addr` from a copy of its line through the reservation system, with the copy's version (for wait_line). A
// wait that reads its word so is ordered after the update that let it go, and after what that thread did before it (a
// reader's copy out of an rwm's buffer, a holder's critical section), by the line's version (D-587). An atomic load of
// the word alone was ordered after them only by the fence before the reservation system's 8-byte CAS, which
// ThreadSanitizer does not model.
u32 word_and_version(u8* base, u32 addr, u64& version)
{
    alignas(16) u8 line[PS_RES_LINE_SIZE];
    version = ps_res_acquire(base, addr, line);
    return ps_ld32(line + (addr & (PS_RES_LINE_SIZE - 1)));
}

// RPCS3's `while (!ctrl.atomic_op(op)) { if (ppu.test_stopped()) return 0; }`: `op(u8* line)` updates the copy of the
// object's line and returns true, or returns false while the object is busy; then the thread waits for the line to
// change and tries again.
template <typename Op>
void update_when_ready(lv2::PpuThread& t, u32 addr, Op&& op)
{
    u8* base = t.kernel.base();
    for (;;) {
        const u64 version = ps_res_version(addr);
        if (reservation_op(base, addr, op)) return;
        wait_line(t, addr, version);
    }
}

// =============================================================================================================== mutex
//
// CellSyncMutex is one big-endian word (4 bytes, 4-aligned), a ticket lock:
//
//   0x00  u16  rel: tickets released (Unlock)
//   0x02  u16  acq: tickets taken (Lock, TryLock, and the SPU side)
//
// The mutex is free when rel == acq. Both counters wrap at 16 bits. libsre agrees with RPCS3 on all four functions: the
// same checks in the same order, lwarx/stwcx. loops, no check of who unlocks.

namespace mutex_off {
constexpr u32 rel = 0x0, acq = 0x2;
}

// RPCS3's checks, in its order: NULL_POINTER, then ALIGN. Then the mutex must be guest memory (D-042).
s32 check_mutex(PPUContext& ctx, std::string_view fn, u32 mutex)
{
    if (!mutex) return null_pointer(ctx, fn, mutex);
    if (mutex % kMutexAlign) return misaligned(ctx, fn, mutex);
    return check_memory(ctx, fn, mutex, "mutex", mutex, kMutexSize);
}

// cellSyncMutexInitialize(mutex), as RPCS3: after the checks the whole word is exchanged for {rel 0, acq 0}, so the
// mutex is free whatever it held. libsre ends with an lwsync: the cleared mutex is visible before anything the caller
// stores next (the mutex's address, handed to another thread).
s32 cellSyncMutexInitialize(PPUContext& ctx, u32 mutex)
{
    if (const s32 rc = check_mutex(ctx, "cellSyncMutexInitialize", mutex); rc != lv2::CELL_OK) return rc;
    PS_LOG_DEBUG(kChan, "cellSyncMutexInitialize({:#x})", mutex);
    reservation_op(guest_base(ctx), mutex, [off = line_offset(mutex)](u8* line) {
        ps_write32(line, off, 0);
        return true;
    });
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return lv2::CELL_OK;
}

// cellSyncMutexLock(mutex), as RPCS3 (CellSyncMutex::Counter::lock_begin): a ticket (acq + 1, the old acq is the ticket)
// in one update, then a wait until rel reaches the ticket, and RPCS3's atomic_fence_acq_rel. Nothing checks who holds the
// mutex. The wait sleeps on the mutex's line.
s32 cellSyncMutexLock(PPUContext& ctx, u32 mutex)
{
    if (const s32 rc = check_mutex(ctx, "cellSyncMutexLock", mutex); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u16 ticket = 0;
    reservation_op(base, mutex, [&ticket, off = line_offset(mutex)](u8* line) {
        ticket = ps_read16(line, off + mutex_off::acq);
        ps_write16(line, off + mutex_off::acq, static_cast<u16>(ticket + 1));
        return true;
    });
    for (;;) {
        u64 version = 0;
        if (static_cast<u16>(word_and_version(base, mutex, version) >> 16) == ticket) break;
        wait_line(t, mutex, version);
    }
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return lv2::CELL_OK;
}

// cellSyncMutexTryLock(mutex), as RPCS3 (CellSyncMutex::Counter::try_lock): a free mutex (rel == acq) gets acq + 1 and
// the caller holds it; a held one is left as it is and the call returns BUSY.
s32 cellSyncMutexTryLock(PPUContext& ctx, u32 mutex)
{
    if (const s32 rc = check_mutex(ctx, "cellSyncMutexTryLock", mutex); rc != lv2::CELL_OK) return rc;
    const bool taken = reservation_op(guest_base(ctx), mutex, [off = line_offset(mutex)](u8* line) {
        const u16 acq = ps_read16(line, off + mutex_off::acq);
        if (ps_read16(line, off + mutex_off::rel) != acq) return false;
        ps_write16(line, off + mutex_off::acq, static_cast<u16>(acq + 1));
        return true;
    });
    return taken ? lv2::CELL_OK : error(CELL_SYNC_ERROR_BUSY);
}

// cellSyncMutexUnlock(mutex), as RPCS3 (CellSyncMutex::Counter::unlock): rel + 1, whether or not the caller holds the
// mutex (there is no PERM check).
s32 cellSyncMutexUnlock(PPUContext& ctx, u32 mutex)
{
    if (const s32 rc = check_mutex(ctx, "cellSyncMutexUnlock", mutex); rc != lv2::CELL_OK) return rc;
    reservation_op(guest_base(ctx), mutex, [off = line_offset(mutex)](u8* line) {
        ps_write16(line, off + mutex_off::rel, static_cast<u16>(ps_read16(line, off + mutex_off::rel) + 1));
        return true;
    });
    return lv2::CELL_OK;
}

// ============================================================================================================= barrier
//
// CellSyncBarrier is one big-endian word (4 bytes, 4-aligned):
//
//   0x00  s16  value: notifications so far; bit 15 once it reached the count (then waits count it down)
//   0x02  u16  count: threads taking part (1..32767)

namespace barrier_off {
constexpr u32 value = 0x0, count = 0x2;
}

// RPCS3 CellSyncBarrier::try_notify: a barrier whose value has bit 15 set (every thread notified, not every one waited
// yet) takes no notification; else value + 1, and bit 15 once the value reaches the count (an s16 compared with the
// u16 count, as RPCS3).
bool barrier_try_notify(u8* line, u32 off)
{
    u16 value = ps_read16(line, off + barrier_off::value);
    if (value & 0x8000) return false;
    value = static_cast<u16>(value + 1);
    if (static_cast<s16>(value) == static_cast<s32>(ps_read16(line, off + barrier_off::count))) value |= 0x8000;
    ps_write16(line, off + barrier_off::value, value);
    return true;
}

// RPCS3 CellSyncBarrier::try_wait: only a barrier with bit 15 set lets a waiter through; value - 1, and once that leaves
// -0x8000 (bit 15 alone: every thread waited) the barrier is 0 again.
bool barrier_try_wait(u8* line, u32 off)
{
    u16 value = ps_read16(line, off + barrier_off::value);
    if (!(value & 0x8000)) return false;
    value = static_cast<u16>(value - 1);
    if (value == 0x8000) value = 0;
    ps_write16(line, off + barrier_off::value, value);
    return true;
}

s32 check_barrier(PPUContext& ctx, std::string_view fn, u32 barrier)
{
    if (!barrier) return null_pointer(ctx, fn, barrier);
    if (barrier % kBarrierAlign) return misaligned(ctx, fn, barrier);
    return check_memory(ctx, fn, barrier, "barrier", barrier, kBarrierSize);
}

// cellSyncBarrierInitialize(barrier, total_count), as RPCS3: NULL_POINTER, ALIGN, then INVAL for a count of 0 or above
// 32767; the barrier becomes {value 0, count} in one update (RPCS3's exchange).
s32 cellSyncBarrierInitialize(PPUContext& ctx, u32 barrier, u16 total_count)
{
    constexpr std::string_view fn = "cellSyncBarrierInitialize";
    if (!barrier) return null_pointer(ctx, fn, barrier);
    if (barrier % kBarrierAlign) return misaligned(ctx, fn, barrier);
    if (!total_count || total_count > 32767) return invalid(ctx, fn, barrier);
    if (const s32 rc = check_memory(ctx, fn, barrier, "barrier", barrier, kBarrierSize); rc != lv2::CELL_OK) return rc;
    reservation_op(guest_base(ctx), barrier, [off = line_offset(barrier), total_count](u8* line) {
        ps_write16(line, off + barrier_off::value, 0);
        ps_write16(line, off + barrier_off::count, total_count);
        return true;
    });
    return lv2::CELL_OK;
}

// cellSyncBarrierNotify(barrier), as RPCS3: try_notify until it succeeds (the barrier still letting the last round's
// threads through waits for them).
s32 cellSyncBarrierNotify(PPUContext& ctx, u32 barrier)
{
    if (const s32 rc = check_barrier(ctx, "cellSyncBarrierNotify", barrier); rc != lv2::CELL_OK) return rc;
    update_when_ready(thread_of(ctx), barrier, [off = line_offset(barrier)](u8* line) { return barrier_try_notify(line, off); });
    return lv2::CELL_OK;
}

// cellSyncBarrierTryNotify(barrier), as RPCS3: atomic_fence_acq_rel, one try_notify; BUSY when it can't.
s32 cellSyncBarrierTryNotify(PPUContext& ctx, u32 barrier)
{
    if (const s32 rc = check_barrier(ctx, "cellSyncBarrierTryNotify", barrier); rc != lv2::CELL_OK) return rc;
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    const bool done = reservation_op(guest_base(ctx), barrier,
        [off = line_offset(barrier)](u8* line) { return barrier_try_notify(line, off); });
    return done ? lv2::CELL_OK : error(CELL_SYNC_ERROR_BUSY);
}

// cellSyncBarrierWait(barrier), as RPCS3: atomic_fence_acq_rel, then try_wait until it succeeds (every thread notified).
s32 cellSyncBarrierWait(PPUContext& ctx, u32 barrier)
{
    if (const s32 rc = check_barrier(ctx, "cellSyncBarrierWait", barrier); rc != lv2::CELL_OK) return rc;
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    update_when_ready(thread_of(ctx), barrier, [off = line_offset(barrier)](u8* line) { return barrier_try_wait(line, off); });
    return lv2::CELL_OK;
}

// cellSyncBarrierTryWait(barrier), as RPCS3: atomic_fence_acq_rel, one try_wait; BUSY when it can't.
s32 cellSyncBarrierTryWait(PPUContext& ctx, u32 barrier)
{
    if (const s32 rc = check_barrier(ctx, "cellSyncBarrierTryWait", barrier); rc != lv2::CELL_OK) return rc;
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    const bool done = reservation_op(guest_base(ctx), barrier,
        [off = line_offset(barrier)](u8* line) { return barrier_try_wait(line, off); });
    return done ? lv2::CELL_OK : error(CELL_SYNC_ERROR_BUSY);
}

// ================================================================================================================= rwm
//
// CellSyncRwm (16 bytes, 16-aligned): a buffer that readers copy out and a writer copies in, one writer at a time and
// never while a reader copies.
//
//   0x00  u16  readers: copies out under way
//   0x02  u16  writers: 1 while a writer holds it
//   0x04  u32  size: the buffer's size (a multiple of 128, at most 16 KB)
//   0x08  u64  buffer (128-aligned)

namespace rwm_off {
constexpr u32 readers = 0x0, writers = 0x2, size = 0x4, buffer = 0x8;
}

// cellSyncRwmInitialize(rwm, buffer, buffer_size), as RPCS3: NULL_POINTER (rwm or buffer), ALIGN (rwm 16, buffer 128),
// INVAL (a size not a multiple of 128, or above 16 KB); then no readers or writers, the size and the buffer, in one
// update, and RPCS3's atomic_fence_acq_rel.
s32 cellSyncRwmInitialize(PPUContext& ctx, u32 rwm, u32 buffer, u32 buffer_size)
{
    constexpr std::string_view fn = "cellSyncRwmInitialize";
    if (!rwm || !buffer) return null_pointer(ctx, fn, rwm);
    if (rwm % kRwmAlign || buffer % 128) return misaligned(ctx, fn, rwm);
    if (buffer_size % 128 || buffer_size > 0x4000) return invalid(ctx, fn, rwm);
    if (const s32 rc = check_memory(ctx, fn, rwm, "rwm", rwm, kRwmSize); rc != lv2::CELL_OK) return rc;
    reservation_op(guest_base(ctx), rwm, [off = line_offset(rwm), buffer, buffer_size](u8* line) {
        ps_write16(line, off + rwm_off::readers, 0);
        ps_write16(line, off + rwm_off::writers, 0);
        ps_write32(line, off + rwm_off::size, buffer_size);
        ps_write64(line, off + rwm_off::buffer, buffer);
        return true;
    });
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return lv2::CELL_OK;
}

// The checks RPCS3's Read, TryRead, Write and TryWrite share (NULL_POINTER: rwm or buffer; ALIGN: the rwm), then the
// rwm's memory, then its buffer and the caller's, size bytes each: both are checked before the counters move, so a
// refused call leaves the rwm as it was (the size and buffer are fixed since Initialize).
s32 check_rwm(PPUContext& ctx, std::string_view fn, u32 rwm, u32 buffer, u32& size, u64& data)
{
    if (!rwm || !buffer) return null_pointer(ctx, fn, rwm);
    if (rwm % kRwmAlign) return misaligned(ctx, fn, rwm);
    if (const s32 rc = check_memory(ctx, fn, rwm, "rwm", rwm, kRwmSize); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    size = ps_read32(base, rwm + rwm_off::size);
    data = ps_read64(base, rwm + rwm_off::buffer);
    if (const s32 rc = check_memory(ctx, fn, rwm, "rwm's buffer", data, size); rc != lv2::CELL_OK) return rc;
    return check_memory(ctx, fn, rwm, "caller's buffer", buffer, size);
}

// RPCS3 CellSyncRwm::try_read_begin: no writer; readers + 1.
bool rwm_try_read_begin(u8* line, u32 off)
{
    if (ps_read16(line, off + rwm_off::writers)) return false;
    ps_write16(line, off + rwm_off::readers, static_cast<u16>(ps_read16(line, off + rwm_off::readers) + 1));
    return true;
}

// RPCS3 CellSyncRwm::try_read_end: readers - 1, unless it is already 0.
bool rwm_try_read_end(u8* line, u32 off)
{
    const u16 readers = ps_read16(line, off + rwm_off::readers);
    if (!readers) return false;
    ps_write16(line, off + rwm_off::readers, static_cast<u16>(readers - 1));
    return true;
}

// RPCS3 cellSyncRwmRead (blocking) and cellSyncRwmTryRead: a reader once no writer holds the rwm (TryRead: else BUSY),
// the buffer copied out, the reader gone again; ABORT when the reader count was 0 by then (something reset the rwm
// during the copy).
s32 rwm_read(PPUContext& ctx, std::string_view fn, u32 rwm, u32 buffer, bool blocking)
{
    u32 size = 0;
    u64 data = 0;
    if (const s32 rc = check_rwm(ctx, fn, rwm, buffer, size, data); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 off = line_offset(rwm);
    auto begin = [off](u8* line) { return rwm_try_read_begin(line, off); };
    if (blocking) update_when_ready(t, rwm, begin);
    else if (!reservation_op(base, rwm, begin)) return error(CELL_SYNC_ERROR_BUSY);
    std::memmove(base + buffer, base + data, size);
    if (!reservation_op(base, rwm, [off](u8* line) { return rwm_try_read_end(line, off); })) {
        return refuse(ctx, fn, rwm, CELL_SYNC_ERROR_ABORT, "no reader left after the copy: CELL_SYNC_ERROR_ABORT");
    }
    return lv2::CELL_OK;
}

s32 cellSyncRwmRead(PPUContext& ctx, u32 rwm, u32 buffer) { return rwm_read(ctx, "cellSyncRwmRead", rwm, buffer, true); }
s32 cellSyncRwmTryRead(PPUContext& ctx, u32 rwm, u32 buffer) { return rwm_read(ctx, "cellSyncRwmTryRead", rwm, buffer, false); }

// cellSyncRwmWrite(rwm, buffer), as RPCS3: the writer mark once no other writer holds it (try_write_begin), then a wait
// until the readers are gone, the buffer copied in, and readers and writers cleared in one update (RPCS3's exchange).
s32 cellSyncRwmWrite(PPUContext& ctx, u32 rwm, u32 buffer)
{
    u32 size = 0;
    u64 data = 0;
    if (const s32 rc = check_rwm(ctx, "cellSyncRwmWrite", rwm, buffer, size, data); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 off = line_offset(rwm);
    update_when_ready(t, rwm, [off](u8* line) {
        if (ps_read16(line, off + rwm_off::writers)) return false;
        ps_write16(line, off + rwm_off::writers, 1);
        return true;
    });
    for (;;) { // the readers gone: what they copied out of the buffer is done
        u64 version = 0;
        if ((word_and_version(base, rwm, version) >> 16) == 0) break;
        wait_line(t, rwm, version);
    }
    std::memmove(base + data, base + buffer, size);
    reservation_op(base, rwm, [off](u8* line) {
        ps_write32(line, off + rwm_off::readers, 0); // readers and writers
        return true;
    });
    return lv2::CELL_OK;
}

// cellSyncRwmTryWrite(rwm, buffer), as RPCS3: the writer mark only on an idle rwm (no readers, no writer: RPCS3's
// compare_and_swap {0, 0} -> {0, 1}), else BUSY; the buffer copied in, then readers and writers cleared.
s32 cellSyncRwmTryWrite(PPUContext& ctx, u32 rwm, u32 buffer)
{
    u32 size = 0;
    u64 data = 0;
    if (const s32 rc = check_rwm(ctx, "cellSyncRwmTryWrite", rwm, buffer, size, data); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    const u32 off = line_offset(rwm);
    const bool taken = reservation_op(base, rwm, [off](u8* line) {
        if (ps_read32(line, off + rwm_off::readers) != 0) return false;
        ps_write16(line, off + rwm_off::writers, 1);
        return true;
    });
    if (!taken) return error(CELL_SYNC_ERROR_BUSY);
    std::memmove(base + data, base + buffer, size);
    reservation_op(base, rwm, [off](u8* line) {
        ps_write32(line, off + rwm_off::readers, 0);
        return true;
    });
    return lv2::CELL_OK;
}

// =============================================================================================================== queue
//
// CellSyncQueue (32 bytes, 32-aligned): a ring of `depth` entries of `size` bytes, any number of pushers and poppers.
//
//   0x00  u32  _pop (top byte): a pop, peek or clear under way; next (low 24 bits): where the next push goes
//   0x04  u32  _push (top byte): a push or clear under way; count (low 24 bits): entries, the one being pushed included
//   0x08  u32  size: the entry size (a multiple of 16)
//   0x0C  u32  depth
//   0x10  u64  buffer (16-aligned)
//   0x18  u64  reserved

namespace queue_off {
constexpr u32 ctrl = 0x0, size = 0x8, depth = 0xC, buffer = 0x10;
}

struct QueueCtrl {
    u32 x0 = 0, x4 = 0;
    u32 next() const { return x0 & 0xFFFFFF; }
    u32 pop() const { return x0 >> 24; }
    u32 count() const { return x4 & 0xFFFFFF; }
    u32 push() const { return x4 >> 24; }
    void set_next(u32 v) { x0 = (x0 & 0xFF000000) | (v & 0xFFFFFF); }
    void set_pop(u32 v) { x0 = (x0 & 0xFFFFFF) | ((v & 0xFF) << 24); }
    void set_count(u32 v) { x4 = (x4 & 0xFF000000) | (v & 0xFFFFFF); }
    void set_push(u32 v) { x4 = (x4 & 0xFFFFFF) | ((v & 0xFF) << 24); }
};

QueueCtrl read_ctrl(u8* line, u32 off) { return {ps_read32(line, off), ps_read32(line, off + 4)}; }

void write_ctrl(u8* line, u32 off, const QueueCtrl& c)
{
    ps_write32(line, off, c.x0);
    ps_write32(line, off + 4, c.x4);
}

// RPCS3 CellSyncQueue::check_depth: the depth, once the control word's next and count are within it. RPCS3 throws
// ("Invalid queue pointers") and libsre asserts on a queue beyond its depth: a fatal log here (D-047).
u32 queue_depth(u8* base, u32 queue, std::string_view fn)
{
    const u64 ctrl = ps_bswap64(__atomic_load_n(host_u64(base, queue), __ATOMIC_ACQUIRE));
    const QueueCtrl c{static_cast<u32>(ctrl >> 32), static_cast<u32>(ctrl)};
    const u32 depth = ps_read32(base, queue + queue_off::depth);
    if (c.next() > depth || c.count() > depth) {
        PS_FATAL(kChan, "{}({:#x}): next {} or count {} is beyond the depth {}; RPCS3 throws \"Invalid queue pointers\" and "
            "libsre (fw 4.92) asserts here", fn, queue, c.next(), c.count(), depth);
    }
    return depth;
}

s32 check_queue(PPUContext& ctx, std::string_view fn, u32 queue)
{
    if (!queue) return null_pointer(ctx, fn, queue);
    if (queue % kQueueAlign) return misaligned(ctx, fn, queue);
    return check_memory(ctx, fn, queue, "queue", queue, kQueueSize);
}

// cellSyncQueueInitialize(queue, buffer, size, depth), as RPCS3: NULL_POINTER (the queue; the buffer of a queue with a
// size), ALIGN (queue 32, buffer 16), INVAL (no depth, a size not a multiple of 16); then an empty control word, the
// size, depth and buffer in one update, and RPCS3's atomic_fence_acq_rel.
s32 cellSyncQueueInitialize(PPUContext& ctx, u32 queue, u32 buffer, u32 size, u32 depth)
{
    constexpr std::string_view fn = "cellSyncQueueInitialize";
    if (!queue) return null_pointer(ctx, fn, queue);
    if (size && !buffer) return null_pointer(ctx, fn, queue);
    if (queue % kQueueAlign || buffer % 16) return misaligned(ctx, fn, queue);
    if (!depth || size % 16) return invalid(ctx, fn, queue);
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kQueueSize); rc != lv2::CELL_OK) return rc;
    reservation_op(guest_base(ctx), queue, [off = line_offset(queue), buffer, size, depth](u8* line) {
        write_ctrl(line, off + queue_off::ctrl, {});
        ps_write32(line, off + queue_off::size, size);
        ps_write32(line, off + queue_off::depth, depth);
        ps_write64(line, off + queue_off::buffer, buffer);
        return true;
    });
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return lv2::CELL_OK;
}

// RPCS3 CellSyncQueue::try_push_begin: no push under way and room (count + _pop below the depth: a pop under way still
// holds its entry). The entry is `next`, which moves on (wrapping at the depth), count + 1, _push marks the push.
bool queue_try_push_begin(u8* line, u32 off, u32 depth, u32& position)
{
    QueueCtrl c = read_ctrl(line, off);
    const u32 count = c.count();
    if (c.push() || count + c.pop() >= depth) return false;
    position = c.next();
    c.set_next(position + 1 != depth ? position + 1 : 0);
    c.set_count(count + 1);
    c.set_push(1);
    write_ctrl(line, off, c);
    return true;
}

// RPCS3 CellSyncQueue::try_pop_begin (and try_peek_begin, which keeps the count): no pop under way and an entry that is
// not being pushed (count above _push). The entry is the oldest, next + depth - count (modulo the depth); _pop marks it.
bool queue_try_pop_begin(u8* line, u32 off, u32 depth, u32& position, bool peek)
{
    QueueCtrl c = read_ctrl(line, off);
    const u32 count = c.count();
    if (c.pop() || count <= c.push()) return false;
    c.set_pop(1);
    position = c.next() + depth - count;
    if (!peek) c.set_count(count - 1);
    write_ctrl(line, off, c);
    return true;
}

enum class QueueOp { Push, Pop, Peek };

// RPCS3 cellSyncQueuePush, Pop, Peek and their Try forms: NULL_POINTER (queue or buffer), ALIGN (the queue), check_depth;
// the entry claimed in one update (blocking: waiting while the queue is full, empty or busy; Try: BUSY), the entry
// copied, the claim released (_push or _pop 0) in a second update. The entry and the caller's buffer are checked inside
// the claiming update, before it stores, so a refused call leaves the queue as it was. RPCS3 reads the entry size and
// the buffer address after the claim; here before it (Initialize alone sets them).
s32 queue_op(PPUContext& ctx, std::string_view fn, u32 queue, u32 buffer, QueueOp op, bool blocking)
{
    if (!queue || !buffer) return null_pointer(ctx, fn, queue);
    if (queue % kQueueAlign) return misaligned(ctx, fn, queue);
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kQueueSize); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 depth = queue_depth(base, queue, fn);
    const u32 off = line_offset(queue);
    const u32 size = ps_read32(base, queue + queue_off::size);
    const u64 entries = ps_read64(base, queue + queue_off::buffer);
    u64 entry = 0;
    bool bad_memory = false;
    auto claim = [&](u8* line) {
        bad_memory = false;
        u32 position = 0;
        const bool claimed = op == QueueOp::Push ? queue_try_push_begin(line, off, depth, position)
                                                 : queue_try_pop_begin(line, off, depth, position, op == QueueOp::Peek);
        if (!claimed) return false;
        entry = entries + u64{op == QueueOp::Push ? position : position % depth} * size;
        if (!memory_ok(t, entry, size) || !memory_ok(t, buffer, size)) {
            bad_memory = true;
            return false;
        }
        return true;
    };
    for (;;) {
        const u64 version = ps_res_version(queue);
        if (reservation_op(base, queue, claim)) break;
        if (bad_memory) {
            return refuse(ctx, fn, queue, CELL_SYNC_ERROR_FAULT, std::format("the entry ({:#x}) or the caller's buffer "
                "({:#x}), {:#x} bytes, is not guest memory; refused with CELL_SYNC_ERROR_FAULT", entry, buffer, size));
        }
        if (!blocking) return error(CELL_SYNC_ERROR_BUSY);
        wait_line(t, queue, version);
    }
    if (op == QueueOp::Push) std::memmove(base + entry, base + buffer, size);
    else std::memmove(base + buffer, base + entry, size);
    reservation_op(base, queue, [off, op](u8* line) {
        QueueCtrl c = read_ctrl(line, off);
        if (op == QueueOp::Push) c.set_push(0);
        else c.set_pop(0);
        write_ctrl(line, off, c);
        return true;
    });
    return lv2::CELL_OK;
}

s32 cellSyncQueuePush(PPUContext& ctx, u32 queue, u32 buffer)
{
    return queue_op(ctx, "cellSyncQueuePush", queue, buffer, QueueOp::Push, true);
}
s32 cellSyncQueueTryPush(PPUContext& ctx, u32 queue, u32 buffer)
{
    return queue_op(ctx, "cellSyncQueueTryPush", queue, buffer, QueueOp::Push, false);
}
s32 cellSyncQueuePop(PPUContext& ctx, u32 queue, u32 buffer)
{
    return queue_op(ctx, "cellSyncQueuePop", queue, buffer, QueueOp::Pop, true);
}
s32 cellSyncQueueTryPop(PPUContext& ctx, u32 queue, u32 buffer)
{
    return queue_op(ctx, "cellSyncQueueTryPop", queue, buffer, QueueOp::Pop, false);
}
s32 cellSyncQueuePeek(PPUContext& ctx, u32 queue, u32 buffer)
{
    return queue_op(ctx, "cellSyncQueuePeek", queue, buffer, QueueOp::Peek, true);
}
s32 cellSyncQueueTryPeek(PPUContext& ctx, u32 queue, u32 buffer)
{
    return queue_op(ctx, "cellSyncQueueTryPeek", queue, buffer, QueueOp::Peek, false);
}

// cellSyncQueueSize(queue), as RPCS3: after check_depth, the count, returned as the result (not an error).
s32 cellSyncQueueSize(PPUContext& ctx, u32 queue)
{
    constexpr std::string_view fn = "cellSyncQueueSize";
    if (const s32 rc = check_queue(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    queue_depth(base, queue, fn);
    return static_cast<s32>(atomic_load_be32(base, queue + 4) & 0xFFFFFF);
}

// cellSyncQueueClear(queue), as RPCS3: after check_depth, waits until no pop is under way and marks one
// (try_clear_begin_1), then until no push is under way and marks one (try_clear_begin_2), then clears the control word.
s32 cellSyncQueueClear(PPUContext& ctx, u32 queue)
{
    constexpr std::string_view fn = "cellSyncQueueClear";
    if (const s32 rc = check_queue(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    queue_depth(base, queue, fn);
    const u32 off = line_offset(queue);
    update_when_ready(t, queue, [off](u8* line) {
        QueueCtrl c = read_ctrl(line, off);
        if (c.pop()) return false;
        c.set_pop(1);
        write_ctrl(line, off, c);
        return true;
    });
    update_when_ready(t, queue, [off](u8* line) {
        QueueCtrl c = read_ctrl(line, off);
        if (c.push()) return false;
        c.set_push(1);
        write_ctrl(line, off, c);
        return true;
    });
    reservation_op(base, queue, [off](u8* line) {
        write_ctrl(line, off, {});
        return true;
    });
    return lv2::CELL_OK;
}

// ============================================================================================================= LFQueue
//
// CellSyncLFQueue (128 bytes, 128-aligned): a lock-free ring between the PPU and SPU code. The PPU pushes into a PPU2SPU
// queue and pops from an SPU2PPU one; the SPU side of the library does the other half on the same line. Pointers run
// modulo 2 * depth (full and empty differ); entry p is at buffer + size * (p mod depth).
//
//   0x00  u16 x4  pop1: m_h1 pops completed, m_h2 bitmap of pops completed ahead of m_h1, m_h3 event-queue state of the
//                 pop side, m_h4 next pop pointer to hand out (pop3: m_h1 and m_h2)
//   0x08  u16 x4  push1: m_h5..m_h8, the same for pushes (push3: m_h5 and m_h6)
//   0x10  u32     m_size (entry size)          0x14  u32  m_depth
//   0x18  u64     m_buffer (bit 0 set for ANY2ANY)
//   0x20  u8 x4   m_bs: all ones while no lv2 event queue is attached
//   0x24  u32     m_direction                   0x28  u32  m_v1
//   0x2C  s32     init: 1 while being set up, 2 once set up (libsre)
//   0x30  u16     push2: three 5-bit fields (bits 10..14 head, 5..9, 0..4 tail) over m_hs1
//   0x32  u16 x15 m_hs1: the ring of SPU waiters the push side signals
//   0x50  u16     pop2, 0x52 u16 x15 m_hs2: the same for the pop side
//   0x70  u64     m_eaSignal: what the signal function gets (a SPURS taskset, ...)
//   0x78  u32     m_v2                          0x7C  u32  m_eq_id: the lv2 event queue
//
// The whole queue is one line, so each pass of RPCS3's (and libsre's) loops is one update of the line here: what the
// pass reads comes from one snapshot, and it stores only if nothing in the line changed since, which is what libsre's
// lwarx/stwcx. on the line give (its reservation granule is the line).
//
// ANY2ANY (D-625): Initialize sets such a queue up differently (m_buffer's bit 0, m_bs[0..1] 0xFF, m_v1 all ones, push2
// and pop2 0xFFFF, m_h3 and m_h7 zeroed), and libsre's (and RPCS3's) cellSyncLFQueueClear reads its waiter rings' heads
// and tails from m_h7 and m_h3 instead of push2 and pop2: in ANY2ANY those two words of push1 and pop1 are the rings'
// packs, not the event states. The second forms are therefore the first forms' counters and completion bitmap on both
// sides of the queue, the rings' packs in m_h7 (pushes) and m_h3 (pops), and no event state: a blocked call sleeps until
// the line changes, as a first form without the event queue does.

constexpr u32 CELL_SYNC_QUEUE_SPU2SPU = 0, CELL_SYNC_QUEUE_SPU2PPU = 1, CELL_SYNC_QUEUE_PPU2SPU = 2,
              CELL_SYNC_QUEUE_ANY2ANY = 3;

namespace lfq_off {
constexpr u32 pop1 = 0x00, push1 = 0x08, size = 0x10, depth = 0x14, buffer = 0x18, bs = 0x20, direction = 0x24,
              v1 = 0x28, init = 0x2C, push2 = 0x30, hs1 = 0x32, pop2 = 0x50, hs2 = 0x52, ea_signal = 0x70, v2 = 0x78,
              eq_id = 0x7C;
}

// A side's four u16 counters (push1 m_h5..m_h8, pop1 m_h1..m_h4).
namespace lfh {
constexpr u32 done = 0, bits = 2, event = 4, next = 6;
}

// The PPU's side of a queue: pushing into PPU2SPU queues, popping from SPU2PPU ones, or either in an ANY2ANY queue (the
// second forms). `own` and `other` are the counters of this side and the opposite one, `pack` and `waiters` this side's
// waiter ring (whom its completions wake: SPU code waiting for an entry, or for room).
struct LfSide {
    bool push;
    u32 direction;
    u32 own, other, pack, waiters;
    bool any2any = false; // the second forms: no event state (own + lfh::event is the ring's pack)
};
constexpr LfSide kPushSide{true, CELL_SYNC_QUEUE_PPU2SPU, lfq_off::push1, lfq_off::pop1, lfq_off::push2, lfq_off::hs1};
constexpr LfSide kPopSide{false, CELL_SYNC_QUEUE_SPU2PPU, lfq_off::pop1, lfq_off::push1, lfq_off::pop2, lfq_off::hs2};
constexpr LfSide kPushSide2{true, CELL_SYNC_QUEUE_ANY2ANY, lfq_off::push1, lfq_off::pop1, lfq_off::push1 + 4,
    lfq_off::hs1, true};
constexpr LfSide kPopSide2{false, CELL_SYNC_QUEUE_ANY2ANY, lfq_off::pop1, lfq_off::push1, lfq_off::pop1 + 4,
    lfq_off::hs2, true};

// The queue checks of the functions RPCS3 checks the queue in: NULL_POINTER (the queue or `out`, the address the
// function writes to; 0 for none), ALIGN (128), then the queue's memory and `out`'s 4 bytes (D-042).
s32 check_lfqueue(PPUContext& ctx, std::string_view fn, u32 queue, u32 out, bool has_out)
{
    if (!queue || (has_out && !out)) return null_pointer(ctx, fn, queue);
    if (queue % kLfQueueAlign) return misaligned(ctx, fn, queue);
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kLfQueueSize); rc != lv2::CELL_OK) return rc;
    return has_out ? check_memory(ctx, fn, queue, "output", out, 4) : lv2::CELL_OK;
}

// The four pointer functions read the queue with no checks in RPCS3 (and libsre). Here a queue that is not guest memory
// is refused with FAULT (D-042), and one that is not 128-aligned with ALIGN: cellSyncLFQueueInitialize sets none up
// there, and the updates need the whole queue in one line.
s32 check_lfqueue_unchecked(PPUContext& ctx, std::string_view fn, u32 queue)
{
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kLfQueueSize); rc != lv2::CELL_OK) return rc;
    return queue % kLfQueueAlign ? misaligned(ctx, fn, queue) : lv2::CELL_OK;
}

// RPCS3 syncLFQueueInitialize (libsre agrees), on the copy of the queue's line. Counters 0 (m_h3 and m_h7 kept, but for
// ANY2ANY), size, depth, buffer (bit 0 set for ANY2ANY), direction, waiter rings 0, eaSignal; then, ANY2ANY: m_bs[0..1]
// 0xFF, m_v1 all ones, push2 and pop2 0xFFFF; otherwise: m_bs all ones, m_v1 0, push2 and pop2 0; m_v2 and m_eq_id 0.
void lf_setup(u8* q, u32 buffer, u32 size, u32 depth, u32 direction, u32 ea_signal)
{
    ps_write32(q, lfq_off::size, size);
    ps_write32(q, lfq_off::depth, depth);
    ps_write64(q, lfq_off::buffer, buffer);
    ps_write32(q, lfq_off::direction, direction);
    std::memset(q + lfq_off::hs1, 0, 15 * 2);
    std::memset(q + lfq_off::hs2, 0, 15 * 2);
    ps_write64(q, lfq_off::ea_signal, ea_signal);
    for (const u32 side : {lfq_off::pop1, lfq_off::push1}) {
        ps_write16(q, side + lfh::done, 0);
        ps_write16(q, side + lfh::bits, 0);
        ps_write16(q, side + lfh::next, 0);
        if (direction == CELL_SYNC_QUEUE_ANY2ANY) ps_write16(q, side + lfh::event, 0);
    }
    if (direction == CELL_SYNC_QUEUE_ANY2ANY) {
        ps_write64(q, lfq_off::buffer, u64{buffer} | 1);
        q[lfq_off::bs] = 0xFF;
        q[lfq_off::bs + 1] = 0xFF;
        ps_write32(q, lfq_off::v1, 0xFFFFFFFF);
        ps_write16(q, lfq_off::push2, 0xFFFF);
        ps_write16(q, lfq_off::pop2, 0xFFFF);
    } else {
        ps_write32(q, lfq_off::bs, 0xFFFFFFFF);
        ps_write32(q, lfq_off::v1, 0);
        ps_write16(q, lfq_off::push2, 0);
        ps_write16(q, lfq_off::pop2, 0);
    }
    ps_write32(q, lfq_off::v2, 0);
    ps_write32(q, lfq_off::eq_id, 0);
}

// cellSyncLFQueueInitialize(queue, buffer, size, depth, direction, eaSignal), as RPCS3; its checks in its order (libsre
// agrees): the queue NULL; with a size, the buffer NULL (NULL_POINTER) and a size above 16 KB or not a multiple of 16
// (INVAL); a depth of 0 or above 0x7FFF, a direction above ANY2ANY (INVAL); the queue (128) or the buffer (16) misaligned
// (ALIGN). Then `init`, for the process's SDK version (-1 counts as 0x460000): a queue whose init is 2 is only compared
// with the arguments (size, depth, buffer, and from SDK 0x180000 eaSignal and direction: INVAL when one differs). From
// SDK 0x180000 a queue with init 1, or with any byte not 0 while init is 0, is STAT; otherwise it is set up.
// Differences: libsre stores init = 2 when it is set up, RPCS3 0 (after which every later Initialize of the queue is
// STAT, where libsre's accepts the same arguments again: the SPU side initializes the same queue): as libsre. RPCS3 and
// libsre claim init (1), set the queue up with plain stores and release it; here the claim, the set-up and init = 2 are
// one update of the line, so nobody sees the queue half set up.
s32 cellSyncLFQueueInitialize(PPUContext& ctx, u32 queue, u32 buffer, u32 size, u32 depth, u32 direction, u32 ea_signal)
{
    constexpr std::string_view fn = "cellSyncLFQueueInitialize";
    if (!queue) return null_pointer(ctx, fn, queue);
    if (size) {
        if (!buffer) return null_pointer(ctx, fn, queue);
        if (size > 0x4000 || size % 16) return invalid(ctx, fn, queue);
    }
    if (!depth || depth > 0x7FFF || direction > CELL_SYNC_QUEUE_ANY2ANY) return invalid(ctx, fn, queue);
    if (queue % kLfQueueAlign || buffer % 16) return misaligned(ctx, fn, queue);
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kLfQueueSize); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u32 sdk = t.kernel.config().sdk_version;
    if (sdk == 0xFFFFFFFF) sdk = 0x460000;

    s32 rc = lv2::CELL_OK;
    bool compare = false;
    u32 old_size = 0, old_depth = 0, old_direction = 0;
    u64 old_buffer = 0, old_signal = 0;
    reservation_op(base, queue, [&](u8* q) {
        rc = lv2::CELL_OK;
        compare = false;
        const u32 init = ps_read32(q, lfq_off::init);
        if (init) {
            if (sdk > 0x17FFFF && init != 2) {
                rc = error(CELL_SYNC_ERROR_STAT);
                return false;
            }
            if (init == 2) {
                compare = true;
                old_size = ps_read32(q, lfq_off::size);
                old_depth = ps_read32(q, lfq_off::depth);
                old_buffer = ps_read64(q, lfq_off::buffer);
                old_signal = ps_read64(q, lfq_off::ea_signal);
                old_direction = ps_read32(q, lfq_off::direction);
                return false; // RPCS3's compare-and-swap of init 2 for 2 changes nothing
            }
        } else if (sdk > 0x17FFFF) {
            for (u32 i = 0; i < kLfQueueSize; i += 8) {
                if (ps_read64(q, i)) {
                    rc = error(CELL_SYNC_ERROR_STAT);
                    return false;
                }
            }
        }
        lf_setup(q, buffer, size, depth, direction, ea_signal);
        ps_write32(q, lfq_off::init, 2);
        return true;
    });
    if (rc != lv2::CELL_OK) return refuse(ctx, fn, queue, CELL_SYNC_ERROR_STAT, "CELL_SYNC_ERROR_STAT: the queue is in use");
    if (compare) {
        if (old_size != size || old_depth != depth || old_buffer != buffer) return invalid(ctx, fn, queue);
        if (sdk > 0x17FFFF && (old_signal != ea_signal || old_direction != direction)) return invalid(ctx, fn, queue);
        __atomic_thread_fence(__ATOMIC_ACQ_REL);
        return lv2::CELL_OK;
    }
    PS_LOG_DEBUG(kChan, "{}({:#x}): {} entries of {:#x} bytes at {:#x}, direction {}, signal {:#x}", fn, queue, depth, size,
        buffer, direction, ea_signal);
    return lv2::CELL_OK;
}

// RPCS3 _cellSyncLFQueueGetPushPointer(queue, pointer, isBlocking, useEventQueue) and _cellSyncLFQueueGetPopPointer(queue,
// pointer, isBlocking, arg4, useEventQueue) (libsre agrees; arg4 is unused in both): PERM unless the queue goes the side's
// way (push: PPU2SPU, pop: SPU2PPU). Then, in each pass: the side's event state (m_h7/m_h3) cleared after an event was
// received; STAT for a blocking call with the event queue while none is attached (m_bs all ones); with the event queue,
// BUSY while a pointer handed out is not completed (next != completed, an s16 against a u16) or the event state is set;
// else the next pointer is handed out when there is room (push: fewer than depth entries between it and the pops
// completed; pop: one pushed at least) and moves on (0 at 2 * depth). Without room: AGAIN when not blocking; blocking
// without the event queue, the next pass once the line changed (RPCS3 and libsre spin); with it, the event state is set
// to 3 (stored only when isBlocking is 3) and the caller receives from the lv2 event queue m_eq_id, then passes again
// (RPCS3 ensures, libsre asserts that the receive succeeds). A pass that stores with the event state set receives too,
// whatever it did. `pointer` (guest address, 0 for none) gets every pointer handed out; `position` the last.
// `copy`: PushBody's and PopBody's buffer (0 for none), checked with the entry the pointer gives before the pass stores,
// so a refused call leaves the queue as it was.
// The second forms (side.any2any, D-625): PERM unless the queue is ANY2ANY; the same claim with no event state (the
// word is the ring's pack there): without room, AGAIN when not blocking, else the next pass once the line changed,
// whatever useEventQueue says (it only chooses how libsre's PPU sleeps; here it sleeps on the line either way).
s32 lf_get_pointer(PPUContext& ctx, std::string_view fn, u32 queue, const LfSide& side, u32 is_blocking, u32 use_event_queue,
    u32 pointer, s32& position, u32 copy)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (ps_read32(base, queue + lfq_off::direction) != side.direction) {
        return refuse(ctx, fn, queue, CELL_SYNC_ERROR_PERM, side.any2any
            ? "CELL_SYNC_ERROR_PERM: not an ANY2ANY queue" : "CELL_SYNC_ERROR_PERM: the queue goes the other way");
    }
    if (pointer) {
        if (const s32 rc = check_memory(ctx, fn, queue, "pointer", pointer, 4); rc != lv2::CELL_OK) return rc;
    }
    if (side.any2any) use_event_queue = 0;
    const s32 depth = static_cast<s32>(ps_read32(base, queue + lfq_off::depth));
    bool received = false; // RPCS3 var1
    for (;;) {
        enum class Next { Return, Receive, Wait } next = Next::Return;
        s32 res = lv2::CELL_OK;
        bool claimed = false, bad_memory = false;
        s32 claim = 0;
        u32 entry = 0, size = 0;
        const u64 version = ps_res_version(queue);
        reservation_op(base, queue, [&](u8* q) {
            claimed = bad_memory = false;
            u16 event = side.any2any ? 0 : ps_read16(q, side.own + lfh::event);
            if (received) event = 0;
            if (is_blocking && use_event_queue && ps_read32(q, lfq_off::bs) == 0xFFFFFFFF) {
                res = error(CELL_SYNC_ERROR_STAT);
                next = Next::Return;
                return false;
            }
            const u16 own_next = ps_read16(q, side.own + lfh::next);
            const s32 var2 = static_cast<s16>(own_next);
            if (use_event_queue && (static_cast<s32>(ps_read16(q, side.own + lfh::done)) != var2 || event)) {
                res = error(CELL_SYNC_ERROR_BUSY);
            } else {
                const s32 other_done = ps_read16(q, side.other + lfh::done);
                s32 room = side.push ? var2 - other_done : other_done - var2;
                if (room < 0) room += depth * 2;
                if (side.push ? room < depth : room > 0) {
                    claim = var2;
                    if (copy) {
                        size = ps_read32(q, lfq_off::size);
                        const s32 at = claim >= depth ? claim - depth : claim;
                        entry = static_cast<u32>(ps_read64(q, lfq_off::buffer) & ~1ull) + size * static_cast<u32>(at);
                        if (!memory_ok(t, entry, size) || !memory_ok(t, copy, size)) {
                            bad_memory = true;
                            return false;
                        }
                    }
                    ps_write16(q, side.own + lfh::next, static_cast<u16>(claim + 1 >= depth * 2 ? 0 : own_next + 1));
                    claimed = true;
                    res = lv2::CELL_OK;
                } else if (!is_blocking) {
                    res = error(CELL_SYNC_ERROR_AGAIN);
                    next = Next::Return;
                    return false;
                } else if (!use_event_queue) {
                    next = Next::Wait;
                    return false;
                } else {
                    res = lv2::CELL_OK;
                    event = 3;
                    if (is_blocking != 3) {
                        next = Next::Receive;
                        return false;
                    }
                }
            }
            if (!side.any2any) ps_write16(q, side.own + lfh::event, event); // ANY2ANY: the ring's pack, left alone
            next = !event || res != lv2::CELL_OK ? Next::Return : Next::Receive;
            return true;
        });
        if (bad_memory) {
            return refuse(ctx, fn, queue, CELL_SYNC_ERROR_FAULT, std::format("the entry ({:#x}) or the caller's buffer "
                "({:#x}), {:#x} bytes, is not guest memory; refused with CELL_SYNC_ERROR_FAULT", entry, copy, size));
        }
        if (claimed) {
            position = claim;
            if (pointer) ps_write32(base, pointer, static_cast<u32>(claim));
        }
        switch (next) {
        case Next::Return:
            if (res == error(CELL_SYNC_ERROR_STAT)) {
                return refuse(ctx, fn, queue, CELL_SYNC_ERROR_STAT, "CELL_SYNC_ERROR_STAT: no lv2 event queue attached");
            }
            return res;
        case Next::Wait: wait_line(t, queue, version); break;
        case Next::Receive: {
            const u32 eq = ps_read32(base, queue + lfq_off::eq_id);
            if (const s32 r = lv2::sys_event_queue_receive(t, eq, 0, 0); r != lv2::CELL_OK) {
                PS_FATAL(kChan, "{}({:#x}): sys_event_queue_receive on lv2 queue {:#x} returned {:#x}; RPCS3 ensures and "
                    "libsre (fw 4.92) asserts it succeeds", fn, queue, eq, static_cast<u32>(r));
            }
            received = true;
            break;
        }
        }
    }
}

// RPCS3 _cellSyncLFQueueCompletePushPointer(queue, pointer, fpSendSignal) and _cellSyncLFQueueCompletePopPointer(queue,
// pointer, fpSendSignal, noQueueFull) (libsre agrees but for one check, below): PERM unless the queue goes the side's
// way; then `pointer` is completed. The side's completed pointer (m_h5/m_h1) moves past every completion in a row from
// it, the bitmap (m_h6/m_h2) keeps those beyond a gap (RPCS3's var1..var5, var9). When SPU code waits on this side (the
// pack's head and tail fields differ) and fewer than 16 are pending (var2: pointers the other side handed out and has not
// completed; var4: waiters taken and not yet gone), the oldest waiter is taken (the head moves on, modulo 30) and
// signalled: fpSendSignal(eaSignal, its ID), whose result is returned. A pop with noQueueFull signals nobody. Without a
// waiter to signal the completion is stored only while no waiter is pending (else the next pass once the line changed;
// RPCS3 and libsre spin). RPCS3 and libsre store the pack and then the completion, ensuring (asserting) nobody changed
// the completion in between; here both are one update from one snapshot, so that cannot fail. The push side checks
// var2 + var4 after storing the pack: RPCS3 ensures it is below 16, libsre asserts only above 16 and goes on: as libsre.
// A signal without a signal function (PushBody and PopBody pass none, in RPCS3 and in libsre) is RPCS3's ensure and
// libsre's assertion: a fatal log. `host_signal`, when given, is the signal function (cellSpurs' bodies, which signal
// SPURS tasks: D-628) in place of a guest one.
// The second forms (side.any2any, D-625): PERM unless the queue is ANY2ANY; the same completion, the ring's pack read
// from and written to m_h7 (pushes) or m_h3 (pops).
s32 lf_complete_pointer(PPUContext& ctx, std::string_view fn, u32 queue, const LfSide& side, s32 pointer, u32 signal_fn,
    u32 no_queue_full, const spurs::LfSignal* host_signal = nullptr)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (ps_read32(base, queue + lfq_off::direction) != side.direction) {
        return refuse(ctx, fn, queue, CELL_SYNC_ERROR_PERM, side.any2any
            ? "CELL_SYNC_ERROR_PERM: not an ANY2ANY queue" : "CELL_SYNC_ERROR_PERM: the queue goes the other way");
    }
    const s32 depth = static_cast<s32>(ps_read32(base, queue + lfq_off::depth));
    for (;;) {
        enum class Next { Done, Signal, Wait, BadSignal } next = Next::Wait;
        u32 waiter = 0, ea_signal = 0;
        const u64 version = ps_res_version(queue);
        reservation_op(base, queue, [&](u8* q) {
            const u16 pack = ps_read16(q, side.pack);
            const u16 done = ps_read16(q, side.own + lfh::done), bits = ps_read16(q, side.own + lfh::bits);
            s32 var1 = pointer - done;
            if (var1 < 0) var1 += depth * 2;
            s32 var2 = static_cast<s16>(ps_read16(q, side.other + lfh::next)) - ps_read16(q, side.other + lfh::done);
            if (var2 < 0) var2 += depth * 2;
            // (u16)(1 slw (15 - var1)): PPC's slw shifts by the low 6 bits and gives 0 from 32. RPCS3 writes 1 << (15 -
            // var1), 0 when bit 4 or 5 is set (no low 16 bits either way), which is undefined for some negative values.
            const u32 amount = static_cast<u32>(15 - var1) & 0x3F;
            const u32 bit = (amount & 0x30) ? 0 : 1u << amount;
            const s32 var9 = std::countl_zero<u32>(static_cast<u16>(~(bit | bits))) - 16;
            s32 var5 = static_cast<s32>(bits | bit);
            if (var9 & 0x30) var5 = 0;
            else var5 <<= var9;
            s32 var3 = done + var9;
            if (var3 >= depth * 2) var3 -= depth * 2;
            const u32 head = (pack >> 10) & 0x1F, tail = pack & 0x1F;
            s32 var4 = static_cast<s32>(head) - static_cast<s32>((pack >> 5) & 0x1F);
            if (var4 < 0) var4 += 0x1E;
            const bool quiet = !side.push && no_queue_full; // a pop with noQueueFull wakes nobody
            if (!quiet && var2 + var4 <= 15 && head != tail) {
                s32 var8 = static_cast<s32>(tail) - static_cast<s32>(head);
                if (var8 < 0) var8 += 0x1E;
                if (var9 > 1 && static_cast<u32>(var8) > 1 && 16 - var2 > 1) {
                    PS_FATAL(kChan, "{}({:#x}, pointer {}): var9 {}, var8 {}, var2 {}; RPCS3 ensures 16 - var2 <= 1 and libsre "
                        "(fw 4.92) asserts it here", fn, queue, pointer, var9, var8, var2);
                }
                if (!signal_fn && !host_signal) {
                    PS_FATAL(kChan, "{}({:#x}, pointer {}): SPU code waits on this side of the queue and there is no signal "
                        "function to wake it (PushBody and PopBody pass none); RPCS3 ensures and libsre (fw 4.92) asserts "
                        "fpSendSignal here", fn, queue, pointer);
                }
                if (!host_signal && !memory_ok(t, signal_fn, 8)) {
                    next = Next::BadSignal;
                    return false;
                }
                const u32 slot = head >= 15 ? head - 15 : head; // RPCS3 var11
                waiter = ps_read16(q, side.waiters + 2 * slot);
                ea_signal = static_cast<u32>(ps_read64(q, lfq_off::ea_signal));
                const u16 head_next = head == 0x1D ? 0 : static_cast<u16>((head + 1) << 10);
                ps_write16(q, side.pack, static_cast<u16>((pack & 0x83FF) | head_next));
                next = Next::Signal;
            } else {
                if (side.push && var2 + var4 > 16) {
                    PS_FATAL(kChan, "{}({:#x}, pointer {}): var2 {} + var4 {} is above 16; libsre (fw 4.92) asserts it is "
                        "not (RPCS3 ensures it is below 16)", fn, queue, pointer, var2, var4);
                }
                if (head != tail) {
                    next = Next::Wait;
                    return false;
                }
                next = Next::Done;
            }
            ps_write16(q, side.own + lfh::done, static_cast<u16>(var3));
            ps_write16(q, side.own + lfh::bits, static_cast<u16>(var5));
            return true;
        });
        switch (next) {
        case Next::Done: return lv2::CELL_OK;
        case Next::Signal:
            if (host_signal) return (*host_signal)(ctx, ea_signal, waiter);
            return static_cast<s32>(call_guest(ctx, signal_fn, {ea_signal, waiter}));
        case Next::BadSignal:
            return refuse(ctx, fn, queue, CELL_SYNC_ERROR_FAULT, std::format("the signal function's descriptor ({:#x}) is "
                "not guest memory; refused with CELL_SYNC_ERROR_FAULT", signal_fn));
        case Next::Wait: wait_line(t, queue, version); break;
        }
    }
}

s32 _cellSyncLFQueueGetPushPointer(PPUContext& ctx, u32 queue, u32 pointer, u32 is_blocking, u32 use_event_queue)
{
    constexpr std::string_view fn = "_cellSyncLFQueueGetPushPointer";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    s32 position = 0;
    return lf_get_pointer(ctx, fn, queue, kPushSide, is_blocking, use_event_queue, pointer, position, 0);
}

s32 _cellSyncLFQueueGetPopPointer(PPUContext& ctx, u32 queue, u32 pointer, u32 is_blocking, u32 /*arg4*/,
    u32 use_event_queue)
{
    constexpr std::string_view fn = "_cellSyncLFQueueGetPopPointer";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    s32 position = 0;
    return lf_get_pointer(ctx, fn, queue, kPopSide, is_blocking, use_event_queue, pointer, position, 0);
}

s32 _cellSyncLFQueueCompletePushPointer(PPUContext& ctx, u32 queue, s32 pointer, u32 signal_fn)
{
    constexpr std::string_view fn = "_cellSyncLFQueueCompletePushPointer";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    return lf_complete_pointer(ctx, fn, queue, kPushSide, pointer, signal_fn, 0);
}

s32 _cellSyncLFQueueCompletePopPointer(PPUContext& ctx, u32 queue, s32 pointer, u32 signal_fn, u32 no_queue_full)
{
    constexpr std::string_view fn = "_cellSyncLFQueueCompletePopPointer";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    return lf_complete_pointer(ctx, fn, queue, kPopSide, pointer, signal_fn, no_queue_full);
}

// The second forms (D-625, unverified: RPCS3's are todo stubs whose arguments are "copied from" the first forms; its
// PushBody and PopBody call them for ANY2ANY queues with the first forms' arguments, GetPopPointer2 without arg4): the
// pointer functions of an ANY2ANY queue, either side from the PPU (lf_get_pointer and lf_complete_pointer with
// kPushSide2 and kPopSide2). The same checks as the first forms (the queue's memory and alignment, D-042), then PERM
// unless the queue is ANY2ANY.
s32 _cellSyncLFQueueGetPushPointer2(PPUContext& ctx, u32 queue, u32 pointer, u32 is_blocking, u32 use_event_queue)
{
    constexpr std::string_view fn = "_cellSyncLFQueueGetPushPointer2";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    s32 position = 0;
    return lf_get_pointer(ctx, fn, queue, kPushSide2, is_blocking, use_event_queue, pointer, position, 0);
}

s32 _cellSyncLFQueueGetPopPointer2(PPUContext& ctx, u32 queue, u32 pointer, u32 is_blocking, u32 use_event_queue)
{
    constexpr std::string_view fn = "_cellSyncLFQueueGetPopPointer2";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    s32 position = 0;
    return lf_get_pointer(ctx, fn, queue, kPopSide2, is_blocking, use_event_queue, pointer, position, 0);
}

s32 _cellSyncLFQueueCompletePushPointer2(PPUContext& ctx, u32 queue, s32 pointer, u32 signal_fn)
{
    constexpr std::string_view fn = "_cellSyncLFQueueCompletePushPointer2";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    return lf_complete_pointer(ctx, fn, queue, kPushSide2, pointer, signal_fn, 0);
}

s32 _cellSyncLFQueueCompletePopPointer2(PPUContext& ctx, u32 queue, s32 pointer, u32 signal_fn, u32 no_queue_full)
{
    constexpr std::string_view fn = "_cellSyncLFQueueCompletePopPointer2";
    if (const s32 rc = check_lfqueue_unchecked(ctx, fn, queue); rc != lv2::CELL_OK) return rc;
    return lf_complete_pointer(ctx, fn, queue, kPopSide2, pointer, signal_fn, no_queue_full);
}

// RPCS3 _cellSyncLFQueuePushBody(queue, buffer, isBlocking) and _cellSyncLFQueuePopBody (cellSyncLFQueuePush and Pop
// pass 1, the Try forms 0; libsre agrees): NULL_POINTER (queue or buffer), ALIGN (queue 128, buffer 16); a pointer from
// GetPushPointer (GetPopPointer) without the event queue, the entry copied (in m_buffer with its ANY2ANY bit cleared),
// then CompletePushPointer (CompletePopPointer, noQueueFull 0) without a signal function. An ANY2ANY queue goes through
// the second forms, as RPCS3's bodies call them (D-625). `host_signal`: the signal function cellSpurs' bodies give the
// completion (D-628); the cellSync bodies have none.
// `check_buffer` false: the buffer's alignment is not looked at (libsre's SPURS bodies, D-914).
s32 lf_body(PPUContext& ctx, std::string_view fn, u32 queue, u32 buffer, u32 is_blocking, bool push,
    const spurs::LfSignal* host_signal, bool check_buffer = true)
{
    if (!queue || !buffer) return null_pointer(ctx, fn, queue);
    if (queue % kLfQueueAlign || (check_buffer && buffer % 16)) return misaligned(ctx, fn, queue);
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kLfQueueSize); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    const bool any2any = ps_read32(base, queue + lfq_off::direction) == CELL_SYNC_QUEUE_ANY2ANY;
    const LfSide& side = any2any ? (push ? kPushSide2 : kPopSide2) : (push ? kPushSide : kPopSide);
    s32 position = 0;
    if (const s32 rc = lf_get_pointer(ctx, fn, queue, side, is_blocking, 0, 0, position, buffer); rc != lv2::CELL_OK) {
        return rc;
    }
    const s32 depth = static_cast<s32>(ps_read32(base, queue + lfq_off::depth));
    const u32 size = ps_read32(base, queue + lfq_off::size);
    const s32 at = position >= depth ? position - depth : position;
    const u32 entry = static_cast<u32>(ps_read64(base, queue + lfq_off::buffer) & ~1ull) + size * static_cast<u32>(at);
    if (side.push) std::memmove(base + entry, base + buffer, size);
    else std::memmove(base + buffer, base + entry, size);
    return lf_complete_pointer(ctx, fn, queue, side, position, 0, 0, host_signal);
}

s32 _cellSyncLFQueuePushBody(PPUContext& ctx, u32 queue, u32 buffer, u32 is_blocking)
{
    return lf_body(ctx, "_cellSyncLFQueuePushBody", queue, buffer, is_blocking, true, nullptr);
}

s32 _cellSyncLFQueuePopBody(PPUContext& ctx, u32 queue, u32 buffer, u32 is_blocking)
{
    return lf_body(ctx, "_cellSyncLFQueuePopBody", queue, buffer, is_blocking, false, nullptr);
}

// ------------------------------------------------------------------------------------- the lv2 event queue (D-625)
//
// _cellSyncLFQueueAttachLv2EventQueue(spus, num, queue) and DetachLv2EventQueue (RPCS3: todos answering CELL_OK; their
// arguments are RPCS3's): the lv2 event queue a PPU thread blocked in the pointer functions with useEventQueue sleeps
// on (m_eq_id: RPCS3's GetPushPointer and GetPopPointer receive from it after marking their side's event state 3, and
// refuse a blocking call with STAT while m_bs is all ones), connected to the SPU threads whose SPU side of the library
// sends the wake-up. What libsre does is not known in detail; pspocket's, the most plausible reading of the layout:
// - a PPU queue of 127 entries (lv2's largest, so an SPU never finds it full: a send_event answer of EBUSY could
//   look like a failure to the SPU side) named "_sncLFQ", local (no key);
// - connected to SPU port p of every thread in `spus` (sys_spu_thread_connect_event, SYS_SPU_THREAD_EVENT_USER): the
//   lowest p of 0x10..0x3F (SPURS' dynamic ports, D-040) free on all of them;
// - m_bs = p (a big-endian word, the SPU side's port; RPCS3 tests the four bytes as one word), m_eq_id = the queue,
//   stored in one update of the line.
// Detach takes the port off every thread, destroys the queue and puts m_bs back as Initialize left it (all ones; for
// ANY2ANY its first two bytes, the rest 0) and m_eq_id to 0.
// Checks: the queue (and with `num`, `spus`) NULL; the queue 128-aligned; `num` 0 (INVAL); memory (FAULT, D-042). An
// SPU2SPU queue has no PPU side to wake: PERM. Attaching a queue that has one is STAT, as is detaching one that has
// none; detaching while a PPU thread waits on it (its side's event state set) is BUSY. lv2's errors (an SPU thread
// that does not exist: ESRCH) are returned as they are, with what was connected undone; no free port is BUSY.

constexpr u32 kLfPortFirst = 0x10, kLfPortLast = 0x3F;
constexpr u32 SYS_SPU_THREAD_EVENT_USER = 1;
constexpr s32 kLfEventQueueSize = 127;

s32 check_lfqueue_spus(PPUContext& ctx, std::string_view fn, u32 spus, u32 num, u32 queue)
{
    if (!queue || !spus) return null_pointer(ctx, fn, queue);
    if (queue % kLfQueueAlign) return misaligned(ctx, fn, queue);
    if (!num) return invalid(ctx, fn, queue);
    if (const s32 rc = check_memory(ctx, fn, queue, "queue", queue, kLfQueueSize); rc != lv2::CELL_OK) return rc;
    if (const s32 rc = check_memory(ctx, fn, queue, "SPU thread list", spus, u64{num} * 4); rc != lv2::CELL_OK) return rc;
    if (ps_read32(guest_base(ctx), queue + lfq_off::direction) == CELL_SYNC_QUEUE_SPU2SPU) {
        return refuse(ctx, fn, queue, CELL_SYNC_ERROR_PERM, "CELL_SYNC_ERROR_PERM: an SPU2SPU queue has no PPU side");
    }
    return lv2::CELL_OK;
}

// m_bs as cellSyncLFQueueInitialize leaves it (lf_setup), for Detach.
void lf_detached_bs(u8* q)
{
    if (ps_read32(q, lfq_off::direction) == CELL_SYNC_QUEUE_ANY2ANY) ps_write32(q, lfq_off::bs, 0xFFFF0000u);
    else ps_write32(q, lfq_off::bs, 0xFFFFFFFFu);
}

s32 _cellSyncLFQueueAttachLv2EventQueue(PPUContext& ctx, u32 spus, u32 num, u32 queue)
{
    constexpr std::string_view fn = "_cellSyncLFQueueAttachLv2EventQueue";
    if (const s32 rc = check_lfqueue_spus(ctx, fn, spus, num, queue); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u64 version = 0;
    if (word_and_version(base, queue + lfq_off::eq_id, version)) {
        return refuse(ctx, fn, queue, CELL_SYNC_ERROR_STAT, "CELL_SYNC_ERROR_STAT: an lv2 event queue is attached already");
    }
    std::vector<u32> threads(num);
    for (u32 i = 0; i < num; i++) threads[i] = ps_read32(base, spus + 4 * i);

    StackVar vars(ctx, base, 0x20); // the queue's ID, sys_event_queue_attribute_t {protocol, type, name[8]}
    const u32 id_out = vars.addr(), attr = vars.addr() + 0x10;
    ps_write32(base, attr, lv2::SYS_SYNC_FIFO);
    ps_write32(base, attr + 4, lv2::SYS_PPU_QUEUE);
    std::memcpy(base + attr + 8, "_sncLFQ", 8);
    if (const s32 rc = lv2::sys_event_queue_create(t, id_out, attr, 0, kLfEventQueueSize); rc != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "{}({:#x}): sys_event_queue_create: {}", fn, queue, lv2::error_name(rc));
        return rc;
    }
    const u32 eq = ps_read32(base, id_out);
    const auto undo = [&](u32 connected, u32 port) {
        for (u32 i = 0; i < connected; i++)
            lv2::sys_spu_thread_disconnect_event(t, threads[i], SYS_SPU_THREAD_EVENT_USER, port);
    };
    u32 port = kLfPortFirst;
    s32 rc = lv2::CELL_OK;
    for (; port <= kLfPortLast; port++) {
        u32 i = 0;
        for (; i < num; i++) {
            rc = lv2::sys_spu_thread_connect_event(t, threads[i], eq, SYS_SPU_THREAD_EVENT_USER, port);
            if (rc != lv2::CELL_OK) break;
        }
        if (i == num) break;
        undo(i, port);
        if (rc != lv2::err(lv2::CELL_EISCONN)) break; // not a taken port: that thread's answer
    }
    if (rc != lv2::CELL_OK) {
        lv2::sys_event_queue_destroy(t, eq, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
        if (rc == lv2::err(lv2::CELL_EISCONN)) {
            return refuse(ctx, fn, queue, CELL_SYNC_ERROR_BUSY, "CELL_SYNC_ERROR_BUSY: no SPU port of 0x10..0x3F is free "
                "on every SPU thread");
        }
        PS_LOG_ERROR(kChan, "{}({:#x}): connecting SPU thread port {:#x}: {}", fn, queue, port, lv2::error_name(rc));
        return rc;
    }
    bool attached = false;
    reservation_op(base, queue, [&](u8* q) {
        attached = ps_read32(q, lfq_off::eq_id) != 0;
        if (attached) return false;
        ps_write32(q, lfq_off::bs, port);
        ps_write32(q, lfq_off::eq_id, eq);
        return true;
    });
    if (attached) { // another thread attached one meanwhile
        undo(num, port);
        lv2::sys_event_queue_destroy(t, eq, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
        return refuse(ctx, fn, queue, CELL_SYNC_ERROR_STAT, "CELL_SYNC_ERROR_STAT: an lv2 event queue is attached already");
    }
    PS_LOG_INFO(kChan, "{}({:#x}): lv2 event queue {:#x} on SPU port {:#x} of {} SPU thread(s)", fn, queue, eq, port, num);
    return lv2::CELL_OK;
}

s32 _cellSyncLFQueueDetachLv2EventQueue(PPUContext& ctx, u32 spus, u32 num, u32 queue)
{
    constexpr std::string_view fn = "_cellSyncLFQueueDetachLv2EventQueue";
    if (const s32 rc = check_lfqueue_spus(ctx, fn, spus, num, queue); rc != lv2::CELL_OK) return rc;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u32 eq = 0, port = 0;
    s32 rc = lv2::CELL_OK;
    reservation_op(base, queue, [&](u8* q) {
        eq = ps_read32(q, lfq_off::eq_id);
        port = ps_read32(q, lfq_off::bs);
        rc = lv2::CELL_OK;
        if (!eq) {
            rc = error(CELL_SYNC_ERROR_STAT);
            return false;
        }
        // A PPU thread receiving from it: its side's event state (the pointer functions' m_h7 or m_h3; ANY2ANY has none).
        if (ps_read32(q, lfq_off::direction) != CELL_SYNC_QUEUE_ANY2ANY &&
            (ps_read16(q, lfq_off::push1 + lfh::event) || ps_read16(q, lfq_off::pop1 + lfh::event))) {
            rc = error(CELL_SYNC_ERROR_BUSY);
            return false;
        }
        lf_detached_bs(q);
        ps_write32(q, lfq_off::eq_id, 0);
        return true;
    });
    if (rc == error(CELL_SYNC_ERROR_STAT)) {
        return refuse(ctx, fn, queue, CELL_SYNC_ERROR_STAT, "CELL_SYNC_ERROR_STAT: no lv2 event queue is attached");
    }
    if (rc != lv2::CELL_OK) return rc; // BUSY: not logged, a normal answer
    for (u32 i = 0; i < num; i++) {
        const u32 thread = ps_read32(base, spus + 4 * i);
        if (const s32 r = lv2::sys_spu_thread_disconnect_event(t, thread, SYS_SPU_THREAD_EVENT_USER, port & 0x3F);
            r != lv2::CELL_OK) {
            PS_LOG_WARN(kChan, "{}({:#x}): disconnecting SPU thread {:#x} port {:#x}: {}", fn, queue, thread, port,
                lv2::error_name(r));
        }
    }
    if (const s32 r = lv2::sys_event_queue_destroy(t, eq, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE); r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "{}({:#x}): destroying lv2 event queue {:#x}: {}", fn, queue, eq, lv2::error_name(r));
    }
    PS_LOG_INFO(kChan, "{}({:#x}): lv2 event queue {:#x} and SPU port {:#x} gone", fn, queue, eq, port);
    return lv2::CELL_OK;
}

// cellSyncLFQueueClear(queue), as RPCS3 (libsre agrees): NULL_POINTER, ALIGN; BUSY (an error, as theirs) while either
// side has a pointer handed out and not completed ((s16)m_h4 != m_h1, (s16)m_h8 != m_h5: an s16 against a u16, so never
// equal from 0x8000, as theirs) or a waiter ring is not empty (head != tail in pop2's pack, or for ANY2ANY in m_h3 and
// m_h7); else the pop counters become the push counters (m_h1..m_h4 = m_h5..m_h8): empty.
s32 cellSyncLFQueueClear(PPUContext& ctx, u32 queue)
{
    constexpr std::string_view fn = "cellSyncLFQueueClear";
    if (const s32 rc = check_lfqueue(ctx, fn, queue, 0, false); rc != lv2::CELL_OK) return rc;
    bool busy = false;
    reservation_op(guest_base(ctx), queue, [&busy](u8* q) {
        u32 var1 = 0, var2 = 0;
        if (ps_read32(q, lfq_off::direction) != CELL_SYNC_QUEUE_ANY2ANY) {
            var1 = var2 = ps_read16(q, lfq_off::pop2);
        } else {
            var1 = ps_read16(q, lfq_off::push1 + lfh::event);
            var2 = ps_read16(q, lfq_off::pop1 + lfh::event);
        }
        const auto pending = [q](u32 side) {
            return static_cast<s16>(ps_read16(q, side + lfh::next)) != static_cast<s32>(ps_read16(q, side + lfh::done));
        };
        busy = pending(lfq_off::pop1) || pending(lfq_off::push1) || ((var2 >> 10) & 0x1F) != (var2 & 0x1F) ||
               ((var1 >> 10) & 0x1F) != (var1 & 0x1F);
        if (busy) return false;
        ps_write64(q, lfq_off::pop1, ps_read64(q, lfq_off::push1));
        return true;
    });
    return busy ? error(CELL_SYNC_ERROR_BUSY) : lv2::CELL_OK;
}

// cellSyncLFQueueSize(queue, size), as RPCS3 (libsre agrees): NULL_POINTER (either), ALIGN; then the entries between the
// pops and the pushes completed (m_h1 and m_h5, modulo 2 * depth). RPCS3 and libsre make the two reads consistent with
// a compare-and-swap (stwcx.) of the first word unchanged; here they come from one snapshot and nothing is stored.
s32 cellSyncLFQueueSize(PPUContext& ctx, u32 queue, u32 size)
{
    if (const s32 rc = check_lfqueue(ctx, "cellSyncLFQueueSize", queue, size, true); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    alignas(16) u8 q[PS_RES_LINE_SIZE];
    ps_res_acquire(base, queue, q);
    const u32 pops = ps_read16(q, lfq_off::pop1 + lfh::done), pushes = ps_read16(q, lfq_off::push1 + lfh::done);
    ps_write32(base, size, pops <= pushes ? pushes - pops : pushes - pops + ps_read32(q, lfq_off::depth) * 2);
    return lv2::CELL_OK;
}

// cellSyncLFQueueDepth(queue, depth), as RPCS3: NULL_POINTER (either), ALIGN, then the depth.
s32 cellSyncLFQueueDepth(PPUContext& ctx, u32 queue, u32 depth)
{
    if (const s32 rc = check_lfqueue(ctx, "cellSyncLFQueueDepth", queue, depth, true); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    ps_write32(base, depth, ps_read32(base, queue + lfq_off::depth));
    return lv2::CELL_OK;
}

// _cellSyncLFQueueGetSignalAddress(queue, ppSignal), as RPCS3: NULL_POINTER (either), ALIGN, then m_eaSignal as a 32-bit
// pointer.
s32 _cellSyncLFQueueGetSignalAddress(PPUContext& ctx, u32 queue, u32 signal)
{
    if (const s32 rc = check_lfqueue(ctx, "_cellSyncLFQueueGetSignalAddress", queue, signal, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, signal, static_cast<u32>(ps_read64(base, queue + lfq_off::ea_signal)));
    return lv2::CELL_OK;
}

// cellSyncLFQueueGetDirection(queue, direction), as RPCS3: NULL_POINTER (either), ALIGN, then the direction.
s32 cellSyncLFQueueGetDirection(PPUContext& ctx, u32 queue, u32 direction)
{
    if (const s32 rc = check_lfqueue(ctx, "cellSyncLFQueueGetDirection", queue, direction, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, direction, ps_read32(base, queue + lfq_off::direction));
    return lv2::CELL_OK;
}

// cellSyncLFQueueGetEntrySize(queue, entry_size), as RPCS3: NULL_POINTER (either), ALIGN, then the entry size.
s32 cellSyncLFQueueGetEntrySize(PPUContext& ctx, u32 queue, u32 entry_size)
{
    if (const s32 rc = check_lfqueue(ctx, "cellSyncLFQueueGetEntrySize", queue, entry_size, true); rc != lv2::CELL_OK) {
        return rc;
    }
    u8* base = guest_base(ctx);
    ps_write32(base, entry_size, ps_read32(base, queue + lfq_off::size));
    return lv2::CELL_OK;
}

} // namespace

// For cellSpurs' _cellSpursLFQueueInitialize (declared in spurs.hpp; RPCS3 calls cellSyncLFQueueInitialize there).
namespace spurs {
s32 sync_lfqueue_initialize(PPUContext& ctx, u32 queue, u32 buffer, u32 size, u32 depth, u32 direction, u32 ea_signal)
{
    return cellSyncLFQueueInitialize(ctx, queue, buffer, size, depth, direction, ea_signal);
}

// For cellDaisy's LFQueue2CompletePopPointer, which calls libsre's own (cellDaisy.cpp, D-538).
s32 sync_lfqueue_complete_pop_pointer(PPUContext& ctx, u32 queue, s32 pointer, u32 signal_fn, u32 no_queue_full)
{
    return _cellSyncLFQueueCompletePopPointer(ctx, queue, pointer, signal_fn, no_queue_full);
}

// For cellSpurs' _cellSpursLFQueuePushBody and PopBody (D-628).
s32 sync_lfqueue_body(PPUContext& ctx, std::string_view fn, u32 queue, u32 buffer, u32 is_blocking, bool push,
    const LfSignal& signal, bool check_buffer)
{
    return lf_body(ctx, fn, queue, buffer, is_blocking, push, &signal, check_buffer);
}
} // namespace spurs

void register_cellSync(Registry& r)
{
    r.add("cellSync", {
        PS_HLE_FUNC(0xA9072DEE, cellSyncMutexInitialize),
        PS_HLE_FUNC(0x1BB675C2, cellSyncMutexLock),
        PS_HLE_FUNC(0xD06918C4, cellSyncMutexTryLock),
        PS_HLE_FUNC(0x91F2B7B0, cellSyncMutexUnlock),
        PS_HLE_FUNC(0x07254FDA, cellSyncBarrierInitialize),
        PS_HLE_FUNC(0xF06A6415, cellSyncBarrierNotify),
        PS_HLE_FUNC(0x268EDD6D, cellSyncBarrierTryNotify),
        PS_HLE_FUNC(0x35F21355, cellSyncBarrierWait),
        PS_HLE_FUNC(0x6C272124, cellSyncBarrierTryWait),
        PS_HLE_FUNC(0xFC48B03F, cellSyncRwmInitialize),
        PS_HLE_FUNC(0xCECE771F, cellSyncRwmRead),
        PS_HLE_FUNC(0xA6669751, cellSyncRwmTryRead),
        PS_HLE_FUNC(0xED773F5F, cellSyncRwmWrite),
        PS_HLE_FUNC(0xBA5BEE48, cellSyncRwmTryWrite),
        PS_HLE_FUNC(0x3929948D, cellSyncQueueInitialize),
        PS_HLE_FUNC(0x5AE841E5, cellSyncQueuePush),
        PS_HLE_FUNC(0x705985CD, cellSyncQueueTryPush),
        PS_HLE_FUNC(0x4DA6D7E0, cellSyncQueuePop),
        PS_HLE_FUNC(0xA58DF87F, cellSyncQueueTryPop),
        PS_HLE_FUNC(0x48154C9B, cellSyncQueuePeek),
        PS_HLE_FUNC(0x68AF923C, cellSyncQueueTryPeek),
        PS_HLE_FUNC(0x4DA349B2, cellSyncQueueSize),
        PS_HLE_FUNC(0xA5362E73, cellSyncQueueClear),
        PS_HLE_FUNC(0xAA355278, cellSyncLFQueueInitialize),
        PS_HLE_FUNC(0x2AF0C515, cellSyncLFQueueClear),
        PS_HLE_FUNC(0x167EA63E, cellSyncLFQueueSize),
        PS_HLE_FUNC(0xE18C273C, cellSyncLFQueueDepth),
        PS_HLE_FUNC(0xD59AA307, cellSyncLFQueueGetDirection),
        PS_HLE_FUNC(0x0C7CB9F7, cellSyncLFQueueGetEntrySize),
        PS_HLE_FUNC(0xE9BF2110, _cellSyncLFQueueGetPushPointer),
        PS_HLE_FUNC(0x74C37666, _cellSyncLFQueueGetPopPointer),
        PS_HLE_FUNC(0x4E88C68D, _cellSyncLFQueueCompletePushPointer),
        PS_HLE_FUNC(0xFE74E8E7, _cellSyncLFQueueCompletePopPointer),
        PS_HLE_FUNC(0xBA5961CA, _cellSyncLFQueuePushBody),
        PS_HLE_FUNC(0xE1BC7ADD, _cellSyncLFQueuePopBody),
        PS_HLE_FUNC(0xAFF7627A, _cellSyncLFQueueGetSignalAddress),
        // D-625: made without libsre to compare with (D-594).
        PS_HLE_UNVERIFIED(0x6BB4EF9D, _cellSyncLFQueueGetPushPointer2),
        PS_HLE_UNVERIFIED(0x46356FE0, _cellSyncLFQueueGetPopPointer2),
        PS_HLE_UNVERIFIED(0x35BBDAD2, _cellSyncLFQueueCompletePushPointer2),
        PS_HLE_UNVERIFIED(0x7A51DEEE, _cellSyncLFQueueCompletePopPointer2),
        PS_HLE_UNVERIFIED(0x54FC2032, _cellSyncLFQueueAttachLv2EventQueue),
        PS_HLE_UNVERIFIED(0x811D148E, _cellSyncLFQueueDetachLv2EventQueue),
    });
}

} // namespace pspocket::hle
