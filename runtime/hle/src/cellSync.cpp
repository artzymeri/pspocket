// cellSync: the mutex of Sony's synchronization library, the three functions Assassin's Creed (BLES00158) imports. Its
// main thread polls cellSyncMutexTryLock (run 090: 24.3 million calls in 90 s while it was unimplemented). Reference:
// RPCS3 rpcs3/Emu/Cell/Modules/cellSync.cpp/.h (cellSyncMutexInitialize, cellSyncMutexTryLock, cellSyncMutexUnlock,
// CellSyncMutex and the error codes). RPCS3 itself runs the firmware's libsre, which holds cellSync, unless libsre is
// set to HLE (rpcs3/Emu/Cell/lv2/sys_prx.cpp g_prx_list). libsre (fw 4.92, read as the reference, D-046) agrees with
// RPCS3's code on all three: the same checks in the same order, lwarx/stwcx. loops for TryLock and Unlock, no check of
// who unlocks.
//
// CellSyncMutex is one big-endian word (4 bytes, 4-aligned) that PPU threads and SPU code share. It is a ticket lock:
//
//   0x00  u16  rel: tickets released (Unlock)
//   0x02  u16  acq: tickets taken (TryLock, and Lock on the PPU or the SPU)
//
// The mutex is free when rel == acq. TryLock takes a ticket only from a free mutex (acq + 1), else it returns BUSY,
// which RPCS3 doesn't report as an error. Unlock releases one (rel + 1). Both counters wrap at 16 bits. Lock (take a
// ticket, then wait until rel reaches it) and the barrier, rwm, queue and lock-free queue functions stay unimplemented
// until a game calls them (CLAUDE.md rule 4).
//
// Every write is one atomic update of the word's 128-byte line through the shared reservation system (reservation_op,
// D-030), where RPCS3 uses host atomics on guest memory (atomic_t exchange and atomic_op). A host compare-and-swap is
// not atomic against guest code here: a conditional store (stwcx., PUTLLC) that compared the line just before the CAS
// landed keeps the CAS's bytes and merges its own around them, so a TryLock and an SPU taking a ticket could both take
// the same ticket. A TryLock that finds the mutex held writes nothing, so a game polling it neither breaks the
// reservations of other code on the line nor wakes its waiters (libsre stores the word back unchanged with a stwcx.
// whose result it ignores; writing nothing is that store failing, which the architecture allows).
//
// Memory order, where libsre has lwsyncs and RPCS3 sequentially consistent atomics: reservation_op's snapshot is an
// acquire and its store follows a release fence, so a TryLock that takes the mutex sees everything its last holder
// wrote before Unlock. Initialize ends with a release fence, as libsre ends it with an lwsync: the cleared mutex is
// visible before anything the caller stores next (the mutex's address, handed to another thread).
//
// Deviation from RPCS3 (D-042): a mutex that is not guest memory is refused with CELL_SYNC_ERROR_FAULT and the call
// site logged, where RPCS3 and a PS3 would fault.

#include <atomic>
#include <format>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSync";

enum : u32 {
    CELL_SYNC_ERROR_BUSY = 0x8041010A,
    CELL_SYNC_ERROR_FAULT = 0x8041010D,
    CELL_SYNC_ERROR_ALIGN = 0x80410110,
    CELL_SYNC_ERROR_NULL_POINTER = 0x80410111,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kMutexSize = 4, kMutexAlign = 4; // CHECK_SIZE_ALIGN(CellSyncMutex, 4, 4)

namespace mutex_off {
constexpr u32 rel = 0x0, acq = 0x2;
}

// The mutex's offset in its 128-byte line: where reservation_op's copy of the line holds it.
constexpr u32 line_offset(u32 mutex) { return mutex & (PS_RES_LINE_SIZE - 1); }

// A refused call, logged as RPCS3's error_code logs an error (BUSY is not one): the first 8 refusals, then every 1024th,
// because a game polling TryLock on a bad mutex would flood the log. For a mutex that is not guest memory, the call
// site too.
s32 refuse(PPUContext& ctx, std::string_view fn, u32 mutex, u32 code, std::string_view why)
{
    static std::atomic<u64> refusals{0};
    if (const u64 n = ++refusals; n <= 8 || n % 1024 == 0) {
        const std::string message = std::format("{}({:#x}): {} (refusal {})", fn, mutex, why, n);
        if (code == CELL_SYNC_ERROR_FAULT) log_call_site(ctx, guest_base(ctx), message);
        else PS_LOG_ERROR(kChan, "{}", message);
    }
    return error(code);
}

// RPCS3's checks, in its order: NULL_POINTER, then ALIGN. Then the mutex must be guest memory (D-042).
s32 check_mutex(PPUContext& ctx, std::string_view fn, u32 mutex)
{
    if (!mutex) return refuse(ctx, fn, mutex, CELL_SYNC_ERROR_NULL_POINTER, "CELL_SYNC_ERROR_NULL_POINTER");
    if (mutex % kMutexAlign) return refuse(ctx, fn, mutex, CELL_SYNC_ERROR_ALIGN, "CELL_SYNC_ERROR_ALIGN");
    if (!guest_readable(thread_of(ctx), mutex, kMutexSize)) {
        return refuse(ctx, fn, mutex, CELL_SYNC_ERROR_FAULT, "not guest memory; refused with CELL_SYNC_ERROR_FAULT");
    }
    return lv2::CELL_OK;
}

// cellSyncMutexInitialize(mutex), as RPCS3: after the checks the whole word is exchanged for {rel 0, acq 0}, so the
// mutex is free whatever it held.
s32 cellSyncMutexInitialize(PPUContext& ctx, u32 mutex)
{
    if (const s32 rc = check_mutex(ctx, "cellSyncMutexInitialize", mutex); rc != lv2::CELL_OK) return rc;
    PS_LOG_DEBUG(kChan, "cellSyncMutexInitialize({:#x})", mutex);
    reservation_op(guest_base(ctx), mutex, [off = line_offset(mutex)](u8* line) {
        ps_write32(line, off, 0);
        return true;
    });
    __atomic_thread_fence(__ATOMIC_RELEASE); // libsre's lwsync
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

} // namespace

void register_cellSync(Registry& r)
{
    r.add("cellSync", {
        PS_HLE_FUNC(0xA9072DEE, cellSyncMutexInitialize),
        PS_HLE_FUNC(0xD06918C4, cellSyncMutexTryLock),
        PS_HLE_FUNC(0x91F2B7B0, cellSyncMutexUnlock),
    });
}

} // namespace pspocket::hle
