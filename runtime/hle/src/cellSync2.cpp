// cellSync2 (D-531): Sony's second synchronization library, its mutex, condition variable, semaphore and queue shared
// by PPU threads, PPU fibers, SPURS tasks and job queue jobs, as libsync2.sprx (fw 4.92) has them, read from its code
// (the reference, D-046) and compared with it running on pspocket (tests/hle/test_sync2.cpp). RPCS3 leaves the whole
// library as todo stubs (rpcs3/Emu/Cell/Modules/cellSync2.cpp; its header gives the public structures).
//
// The objects are 128-byte, 128-aligned blocks whose layout the SPU side of the library shares (a SPURS task or job
// updates them with GETLLAR/PUTLLC), so every field and every bit is libsync2's, and every update is the one libsync2
// makes with the same lwarx/ldarx and stwcx./stdcx. pairs: a snapshot of the 128-byte line through the shared
// reservation system and a conditional store (class Reserve), as the PPU interpreter and lifted code make them, so a
// store by any processor in between fails it as on the hardware. Waiting goes through the caller's thread type: a
// table of callbacks (who am I, allocate a signal receiver, wait on it, free it) and, for waking, the notifier of the
// waiter's thread type (send a signal). The library's own tables (gCellSync2CallerThreadType*, gCellSync2Notifier*)
// are these variables, whose callbacks are internal functions here: a PPU thread waits on a bit of one of two lv2
// event flags (64 receivers each), a fiber through cellFiber, a SPURS task or job is signalled through cellSpurs or
// cellSpursJq (the firmware's or pspocket's, whichever the process has).
//
// Blocked callers wait in a waiter queue (wq::): a ring of 8-byte entries after the object (the buffer the caller sizes
// with *EstimateBufferSize), where waiters and wakers each claim the next entry of their own round, and a waker that
// comes first leaves a signal the waiter finds instead of sleeping. The mutex hands itself over to a woken waiter; the
// queue is a linked queue over a fixed set of entries (an allocation bitmap, a next-index array, a dummy head) with
// counters that reserve entries and elements before they are taken.
//
// Deviations: libsync2's assertions (its "PS3 SDK INTERNAL ASSERTION FAILURE" and abort) are fatal logs with the same
// text. Objects, configurations and buffers that are not guest memory are refused with CELL_SYNC2_ERROR_INVAL and the
// call site logged, where libsync2 would fault. libsync2 creates its two event flags and a lightweight mutex at module
// start; here the flags are made at the first use of a PPU thread's receivers and a host mutex guards their bitmaps.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/core/timebase.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSync2";
constexpr std::string_view kModule = "cellSync2";

enum : u32 {
    CELL_SYNC2_ERROR_AGAIN = 0x80410C01,
    CELL_SYNC2_ERROR_INVAL = 0x80410C02,
    CELL_SYNC2_ERROR_NOMEM = 0x80410C04,
    CELL_SYNC2_ERROR_DEADLK = 0x80410C08,
    CELL_SYNC2_ERROR_PERM = 0x80410C09,
    CELL_SYNC2_ERROR_BUSY = 0x80410C0A,
    CELL_SYNC2_ERROR_STAT = 0x80410C0F,
    CELL_SYNC2_ERROR_ALIGN = 0x80410C10,
    CELL_SYNC2_ERROR_NULL_POINTER = 0x80410C11,
    CELL_SYNC2_ERROR_NOT_SUPPORTED_THREAD = 0x80410C12,
    CELL_SYNC2_ERROR_NO_NOTIFIER = 0x80410C13,
};
s32 error(u32 code) { return static_cast<s32>(code); }

enum : u16 { kTypePpuThread = 1, kTypePpuFiber = 2, kTypeSpursTask = 4, kTypeJobQueueJob = 8 };
// What a receiver is allocated and waited for (the second argument of allocateSignalReceiver and waitSignal).
enum : u32 { kForMutex = 1, kForCond = 2, kForQueue = 3, kForSemaphore = 4 };
constexpr u16 kForbiddenTypes = 0x3E30; // thread type bits an object may not name
constexpr u64 kBit63 = 1ull << 63;

// libsync2's assertion: its message, then the process ends.
[[noreturn]] void sync2_assert(std::string_view file, u32 line, std::string_view function)
{
    PS_FATAL(kChan, "PS3 SDK INTERNAL ASSERTION FAILURE: libsync2, version 492000, file {}, line {}, function \"{}\"",
        file, line, function);
}

void lwsync() { std::atomic_thread_fence(std::memory_order_acq_rel); }

// The PPU's ldarx/lwarx and stdcx./stwcx. on a guest doubleword or word: a snapshot of its 128-byte line and the
// line's version, then a store that succeeds only if neither changed (pspocket/core/reservation.h).
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

private:
    alignas(16) u8 line_[PS_RES_LINE_SIZE];
    u64 version_ = 0;
    u32 addr_ = 0;
};

// Plain stores to an object (owners, counts) break other processors' reservations on its line as a store does on the
// hardware; waiters on the line are told at once.
void stored(u32 addr, u32 size) { ps_res_notify_write(addr, size); }

// mftb, read until its low word is not zero (libsync2's loops).
u64 timebase()
{
    for (;;)
        if (const u64 tb = ps_timebase_now(); static_cast<u32>(tb) != 0) return tb;
}

// A refusal of memory that is not the guest's (where libsync2 would fault).
s32 not_guest(PPUContext& ctx, std::string_view fn, std::string_view what, u32 addr)
{
    log_call_site(ctx, guest_base(ctx),
        std::format("{}: the {} at {:#x} is not guest memory; CELL_SYNC2_ERROR_INVAL", fn, what, addr));
    return error(CELL_SYNC2_ERROR_INVAL);
}

// Whether [addr, addr + size) is guest memory, page by page.
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// strncpy, as libsync2 copies a name (sysPrxForUser's _sys_strncpy: the rest of n filled with zeros).
void copy_name(u8* base, u32 to, u32 from, u32 n)
{
    u32 i = 0;
    for (; i < n && base[from + i]; i++) base[to + i] = base[from + i];
    for (; i < n; i++) base[to + i] = 0;
}

// ---------------------------------------------------------------------------------------------- imports, tables

// A function of another library libsync2 imports: the firmware module's (Registry::add_imports) or pspocket's own.
// `found` false when the process has neither.
u64 call_import(PPUContext& ctx, std::string_view library, std::string_view name, std::initializer_list<u64> args,
    bool* found = nullptr)
{
    const u32 nid = recomp::nid_of(name);
    if (found) *found = true;
    if (const u32 opd = HleProcess::current()->imported(library, nid)) return call_guest(ctx, opd, args);
    if (const Function* f = registry().find(library, nid)) {
        u32 i = 3;
        for (const u64 a : args) ctx.gpr[i++] = a;
        f->fn(ctx, guest_base(ctx));
        return ctx.gpr[3];
    }
    if (found) *found = false;
    return 0;
}

// cellFiberPpuSelf: the fiber running on this PPU thread, or 0 (also with no cellFiber in the process: no fiber).
u32 fiber_self(PPUContext& ctx) { return static_cast<u32>(call_import(ctx, "cellFiber", "cellFiberPpuSelf", {})); }

// A caller thread type (CellSync2CallerThreadType): +0 u16 type, +4 self, +8 waitSignal, +0xC allocateSignalReceiver,
// +0x10 freeSignalReceiver (descriptors), +0x14 spinWaitNanoSec, +0x18 u64 callbackArg.
struct Caller {
    u32 addr = 0;
    u16 type = 0;
    u32 self = 0, wait = 0, allocate = 0, free = 0, spin = 0;
    u64 arg = 0;
};

Caller read_caller(u8* base, u32 a)
{
    return {a, ps_read16(base, a), ps_read32(base, a + 4), ps_read32(base, a + 8), ps_read32(base, a + 0xC),
        ps_read32(base, a + 0x10), ps_read32(base, a + 0x14), ps_read64(base, a + 0x18)};
}

u64 caller_self(PPUContext& ctx, const Caller& c) { return call_guest(ctx, c.self, {c.arg}); }

// A signal receiver from the caller's thread type, for waiting on `object` (kFor*): 0 or its error.
s32 allocate_receiver(PPUContext& ctx, const Caller& c, u32 kind, u32 object, u64& receiver)
{
    StackVar v(ctx, guest_base(ctx), 8);
    const s32 r = static_cast<s32>(call_guest(ctx, c.allocate, {v.addr(), kind, object, c.arg}));
    receiver = ps_read64(guest_base(ctx), v.addr());
    return r;
}

s32 free_receiver(PPUContext& ctx, const Caller& c, u64 receiver)
{
    return static_cast<s32>(call_guest(ctx, c.free, {receiver, c.arg}));
}

// The library's variables, their addresses in this process (linked on first use).
u32 variable(std::string_view name) { return HleProcess::current()->link_variable(kModule, recomp::nid_of(name)); }

// Where a call with no configuration takes its caller: a fiber's when the object allows fibers and the caller is one,
// else a PPU thread's.
u32 default_caller(PPUContext& ctx, u16 types)
{
    if ((types & kTypePpuFiber) && fiber_self(ctx)) return variable("gCellSync2CallerThreadTypePpuFiber");
    return variable("gCellSync2CallerThreadTypePpuThread");
}

// A configuration (CellSync2ThreadConfig: callerThreadType, notifierTable, numNotifier) or the defaults.
struct Config {
    u32 caller = 0, table = 0, count = 0;
};

s32 read_config(PPUContext& ctx, std::string_view fn, u32 cfg, u16 types, Config& out, bool full)
{
    u8* base = guest_base(ctx);
    if (!cfg) {
        out.caller = default_caller(ctx, types);
        out.table = variable("_pspocket_cellSync2NotifierTable");
        out.count = 4;
        return lv2::CELL_OK;
    }
    if (!guest_readable(thread_of(ctx), cfg, full ? 12 : 4)) return not_guest(ctx, fn, "configuration", cfg);
    out.caller = ps_read32(base, cfg);
    if (full) {
        out.table = ps_read32(base, cfg + 4);
        out.count = ps_read32(base, cfg + 8);
    }
    return lv2::CELL_OK;
}

// libsync2's checks of a caller: NULL, a type of 0, a type the object does not allow; then, for a call that may wait,
// the three callbacks waiting needs.
s32 check_caller(PPUContext& ctx, std::string_view fn, u32 addr, u16 types, bool waits, Caller& c)
{
    if (!addr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (!guest_readable(thread_of(ctx), addr, 0x20)) return not_guest(ctx, fn, "caller thread type", addr);
    c = read_caller(guest_base(ctx), addr);
    if (c.type == 0) return error(CELL_SYNC2_ERROR_INVAL);
    if (!(c.type & types)) return error(CELL_SYNC2_ERROR_NOT_SUPPORTED_THREAD);
    if (waits && (!c.allocate || !c.free || !c.wait)) return error(CELL_SYNC2_ERROR_PERM);
    return lv2::CELL_OK;
}

// libsync2's check of a notifier table: NULL, no entries, an entry of type 0 or without sendSignal, and the object's
// PPU-side thread types (the low byte) not all covered.
s32 check_notifiers(PPUContext& ctx, std::string_view fn, u32 table, u32 count, u16 types)
{
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    if (!table) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (!count) return error(CELL_SYNC2_ERROR_INVAL);
    if (!guest_range(t, table, u64{count} * 4)) return not_guest(ctx, fn, "notifier table", table);
    u16 missing = types & 0xFF;
    for (u32 i = 0; i < count; i++) {
        const u32 n = ps_read32(base, table + 4 * i);
        if (!guest_readable(t, n, 0x10)) return not_guest(ctx, fn, "notifier", n);
        const u16 type = ps_read16(base, n);
        if (!type) return error(CELL_SYNC2_ERROR_INVAL);
        if (!ps_read32(base, n + 4)) return error(CELL_SYNC2_ERROR_NULL_POINTER);
        missing &= static_cast<u16>(~type);
    }
    return missing ? error(CELL_SYNC2_ERROR_NO_NOTIFIER) : lv2::CELL_OK;
}

// ------------------------------------------------------------------------------- the library's thread types

// A PPU thread's receivers: bits of two lv2 event flags, created at their first use (libsync2's module start makes
// them, with a lightweight mutex over the bitmaps).
struct PpuSignals {
    std::mutex mutex;
    bool ready = false;
    u32 flag[2] = {};
    u64 used[2] = {};
};
PpuSignals& ppu_signals() { return HleProcess::current()->state<PpuSignals>(); }

// libsync2's module start: two event flags (priority order, several waiters, no name).
bool ppu_signals_ready(PPUContext& ctx, PpuSignals& s)
{
    std::lock_guard lock(s.mutex);
    if (s.ready) return true;
    u8* base = guest_base(ctx);
    StackVar v(ctx, base, 0x30);
    const u32 attr = v.addr(), ids = v.addr() + 0x20;
    ps_write32(base, attr, 2);         // SYS_SYNC_PRIORITY
    ps_write32(base, attr + 4, 0x200); // SYS_SYNC_NOT_PROCESS_SHARED
    ps_write64(base, attr + 8, 0);
    ps_write32(base, attr + 16, 0);
    ps_write32(base, attr + 20, 0x20000); // SYS_SYNC_WAITER_MULTIPLE
    for (u32 k = 0; k < 2; k++) {
        if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 82, {ids + 4 * k, attr, 0})); r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "a PPU thread's signal receivers: sys_event_flag_create {:#x}", static_cast<u32>(r));
            if (k) lv2_syscall(ctx, 83, {ps_read32(base, ids)});
            return false;
        }
    }
    s.flag[0] = ps_read32(base, ids);
    s.flag[1] = ps_read32(base, ids + 4);
    s.ready = true;
    return true;
}

// The flag a receiver names (its low word), or -1.
int ppu_flag_of(const PpuSignals& s, u64 receiver)
{
    const u32 id = static_cast<u32>(receiver);
    return id == s.flag[0] ? 0 : id == s.flag[1] ? 1 : -1;
}

u64 _pspocket_cellSync2PpuThreadSelf(PPUContext& ctx, u64)
{
    StackVar v(ctx, guest_base(ctx), 8);
    call_import(ctx, "sysPrxForUser", "sys_ppu_thread_get_id", {v.addr()});
    return ps_read64(guest_base(ctx), v.addr());
}

// The first free bit of the first flag, then of the second: (bit << 32) | flag ID. CELL_SYNC2_ERROR_AGAIN when all
// 128 are taken.
s32 _pspocket_cellSync2PpuThreadAllocate(PPUContext& ctx, u32 receiver, s32, u64, u64)
{
    PpuSignals& s = ppu_signals();
    if (!ppu_signals_ready(ctx, s)) return error(CELL_SYNC2_ERROR_STAT);
    std::lock_guard lock(s.mutex);
    for (u32 k = 0; k < 2; k++) {
        for (u32 bit = 0; bit < 64; bit++) {
            if (s.used[k] & (1ull << bit)) continue;
            s.used[k] |= 1ull << bit;
            ps_write64(guest_base(ctx), receiver, (u64{bit} << 32) | s.flag[k]);
            return lv2::CELL_OK;
        }
    }
    return error(CELL_SYNC2_ERROR_AGAIN);
}

s32 _pspocket_cellSync2PpuThreadFree(PPUContext& ctx, u64 receiver, u64)
{
    PpuSignals& s = ppu_signals();
    if (!ppu_signals_ready(ctx, s)) return error(CELL_SYNC2_ERROR_STAT);
    std::lock_guard lock(s.mutex);
    const int k = ppu_flag_of(s, receiver);
    const u64 bit = 1ull << ((receiver >> 32) & 63);
    if (k < 0 || !(s.used[k] & bit)) return error(CELL_SYNC2_ERROR_INVAL);
    s.used[k] &= ~bit;
    return lv2::CELL_OK;
}

// The receiver's bit, waited for and cleared (AND | CLEAR, no timeout).
s32 _pspocket_cellSync2PpuThreadWait(PPUContext& ctx, u64 receiver, s32, u64, u64)
{
    PpuSignals& s = ppu_signals();
    if (!ppu_signals_ready(ctx, s)) return error(CELL_SYNC2_ERROR_STAT);
    if (ppu_flag_of(s, receiver) < 0) return error(CELL_SYNC2_ERROR_INVAL);
    StackVar v(ctx, guest_base(ctx), 8);
    const u64 r = lv2_syscall(ctx, 85, {static_cast<u32>(receiver), 1ull << ((receiver >> 32) & 63), 0x11, v.addr(), 0});
    if (static_cast<s32>(r) != lv2::CELL_OK) sync2_assert("ppu_thread_signal.c", 229, "_cellSync2PpuThreadSignalWait");
    return lv2::CELL_OK;
}

s32 _pspocket_cellSync2PpuThreadSend(PPUContext& ctx, u64 receiver, u64)
{
    PpuSignals& s = ppu_signals();
    if (!ppu_signals_ready(ctx, s)) return error(CELL_SYNC2_ERROR_STAT);
    if (ppu_flag_of(s, receiver) < 0) return error(CELL_SYNC2_ERROR_INVAL);
    const u64 r = lv2_syscall(ctx, 87, {static_cast<u32>(receiver), 1ull << ((receiver >> 32) & 63)});
    if (static_cast<s32>(r) != lv2::CELL_OK) sync2_assert("ppu_thread_signal.c", 255, "_cellSync2PpuThreadSignalSend");
    return lv2::CELL_OK;
}

u64 _pspocket_cellSync2PpuFiberSelf(PPUContext& ctx, u64) { return fiber_self(ctx); }

s32 _pspocket_cellSync2PpuFiberAllocate(PPUContext& ctx, u32 receiver, s32, u64, u64)
{
    const u32 self = fiber_self(ctx);
    if (!self) return error(CELL_SYNC2_ERROR_INVAL);
    ps_write64(guest_base(ctx), receiver, self);
    return lv2::CELL_OK;
}

s32 _pspocket_cellSync2PpuFiberFree(PPUContext&, u64, u64) { return lv2::CELL_OK; }

s32 _pspocket_cellSync2PpuFiberWait(PPUContext& ctx, u64, s32, u64, u64)
{
    if (static_cast<s32>(call_import(ctx, "cellFiber", "cellFiberPpuWaitSignal", {})) != lv2::CELL_OK)
        sync2_assert("ppu_fiber_signal.c", 80, "_cellSync2PpuFiberSignalWait");
    return lv2::CELL_OK;
}

s32 _pspocket_cellSync2PpuFiberSend(PPUContext& ctx, u64 receiver, u64)
{
    StackVar v(ctx, guest_base(ctx), 16);
    if (static_cast<s32>(call_import(ctx, "cellFiber", "cellFiberPpuUtilWorkerControlSendSignal",
            {static_cast<u32>(receiver), v.addr()})) != lv2::CELL_OK)
        sync2_assert("ppu_fiber_signal.c", 92, "_cellSync2PpuFiberSignalSend");
    return lv2::CELL_OK;
}

// A SPURS task's receiver: its taskset (low word) and task ID (bits 32..39).
s32 _pspocket_cellSync2SpursTaskSend(PPUContext& ctx, u64 receiver, u64)
{
    if (static_cast<s32>(call_import(ctx, "cellSpurs", "_cellSpursSendSignal",
            {static_cast<u32>(receiver), (receiver >> 32) & 0xFF})) != lv2::CELL_OK)
        sync2_assert("spurs_task_signal.c", 33, "_cellSync2SpursTaskSignalSend");
    return lv2::CELL_OK;
}

s32 _pspocket_cellSync2SpursJqJobSend(PPUContext& ctx, u64 receiver, u64)
{
    if (static_cast<s32>(call_import(ctx, "cellSpursJq", "cellSpursJobQueueSendSignal", {static_cast<u32>(receiver)})) !=
        lv2::CELL_OK)
        sync2_assert("spurs_jq_job_signal.c", 30, "_cellSync2SpursJqJobSignalSend");
    return lv2::CELL_OK;
}

// The variables: callbacks as descriptors of the functions above, linked when the variable is.
u32 internal_descriptor(std::string_view name)
{
    return HleProcess::current()->link_function(kModule, recomp::nid_of(name), name);
}

void init_caller(u8* base, u32 at, u16 type, std::string_view self, std::string_view wait, std::string_view allocate,
    std::string_view free, u32 spin)
{
    ps_write16(base, at, type);
    ps_write32(base, at + 4, internal_descriptor(self));
    ps_write32(base, at + 8, internal_descriptor(wait));
    ps_write32(base, at + 0xC, internal_descriptor(allocate));
    ps_write32(base, at + 0x10, internal_descriptor(free));
    ps_write32(base, at + 0x14, spin);
    ps_write64(base, at + 0x18, 0);
}

void init_notifier(u8* base, u32 at, u16 type, std::string_view send)
{
    ps_write16(base, at, type);
    ps_write32(base, at + 4, internal_descriptor(send));
    ps_write64(base, at + 8, 0);
}

void init_caller_ppu_thread(u8* base, u32 at)
{
    init_caller(base, at, kTypePpuThread, "_pspocket_cellSync2PpuThreadSelf", "_pspocket_cellSync2PpuThreadWait",
        "_pspocket_cellSync2PpuThreadAllocate", "_pspocket_cellSync2PpuThreadFree", 1000);
}
void init_caller_ppu_fiber(u8* base, u32 at)
{
    init_caller(base, at, kTypePpuFiber, "_pspocket_cellSync2PpuFiberSelf", "_pspocket_cellSync2PpuFiberWait",
        "_pspocket_cellSync2PpuFiberAllocate", "_pspocket_cellSync2PpuFiberFree", 0);
}
void init_notifier_ppu_thread(u8* base, u32 at) { init_notifier(base, at, kTypePpuThread, "_pspocket_cellSync2PpuThreadSend"); }
void init_notifier_ppu_fiber(u8* base, u32 at) { init_notifier(base, at, kTypePpuFiber, "_pspocket_cellSync2PpuFiberSend"); }
void init_notifier_spurs_task(u8* base, u32 at) { init_notifier(base, at, kTypeSpursTask, "_pspocket_cellSync2SpursTaskSend"); }
void init_notifier_jq_job(u8* base, u32 at) { init_notifier(base, at, kTypeJobQueueJob, "_pspocket_cellSync2SpursJqJobSend"); }
// libsync2's default notifier table: the four notifiers, in that order.
void init_notifier_table(u8* base, u32 at)
{
    ps_write32(base, at, variable("gCellSync2NotifierPpuThread"));
    ps_write32(base, at + 4, variable("gCellSync2NotifierPpuFiber"));
    ps_write32(base, at + 8, variable("gCellSync2NotifierSpursTask"));
    ps_write32(base, at + 12, variable("gCellSync2NotifierSpursJobQueueJob"));
}

// ------------------------------------------------------------------------------------------------- waiter queue
//
// 16 bytes: +0 u64 H, +8 u32 buffer, +0xC u16 entries (its size / 8).
//   H: bit 63 the wakers' round | 48..62 wakers at work | 32..47 head (next entry a waker takes)
//      bit 31 the waiters' round | 16..30 waiters at work | 0..15 tail (next entry a waiter takes)
// An entry: bit 63 the waiters' round it was taken in | bit 62 the wakers' | bit 61 a signal left before its waiter came
//   | 52..60 a count of waiters through it | 48..51 the waiter's thread type (its bit number) | 0..47 its receiver.

namespace wq {
constexpr u32 kBuffer = 8, kEntries = 0xC;

s32 init(u8* base, u32 q, u32 buffer, u32 size)
{
    if (!buffer) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (buffer & 7) return error(CELL_SYNC2_ERROR_ALIGN);
    if (size > 0x40000) return error(CELL_SYNC2_ERROR_INVAL);
    const u32 n = size >> 3;
    ps_write32(base, q + kBuffer, buffer);
    ps_write16(base, q + kEntries, static_cast<u16>(n));
    if ((n & 0xFFFF) <= 1) return error(CELL_SYNC2_ERROR_INVAL);
    std::memset(base + buffer, 0, size);
    ps_write64(base, q, 0x8000000080000000ull);
    return lv2::CELL_OK;
}

// Nobody at work and nobody waiting: else CELL_SYNC2_ERROR_BUSY.
s32 idle(u8* base, u32 q)
{
    const u64 h = ps_read64(base, q);
    if ((h & 0x7FFF000000000000ull) == 0 && (h & 0x7FFF0000ull) == 0 && ((h >> 32) & 0xFFFF) == (h & 0xFFFF))
        return lv2::CELL_OK;
    return error(CELL_SYNC2_ERROR_BUSY);
}

// The distance from `from` to `slot` around a ring of `n`.
u32 distance(u32 slot, u32 from, u32 n) { return slot >= from ? slot - from : slot + n - from; }

// Waits on `receiver` in the next entry of the waiters' round: the entry takes the receiver and the caller's thread
// type, then the caller waits through `wait_fn` (waitSignal(receiver, kind, object, arg)), unless a waker left its
// signal in the entry first.
s32 wait(PPUContext& ctx, u32 q, u16 type, u64 receiver, u32 wait_fn, u32 kind, u32 object, u64 arg)
{
    u8* base = guest_base(ctx);
    Reserve r;
    u64 h;
    do {
        h = r.ldarx(base, q);
        h = (h & ~(0x7FFFull << 16)) | ((((h >> 16) & 0x7FFF) + 1) & 0x7FFF) << 16;
    } while (!r.stdcx(base, h));
    const u64 type_bit = static_cast<u64>(31 - std::countl_zero(static_cast<u32>(type))) & 0xF;
    u32 tail = static_cast<u32>(h & 0xFFFF);
    u32 skip = 0;
    for (;;) {
        const u32 seen = tail;
        const u32 slot = (skip + tail) % ps_read16(base, q + kEntries);
        const u64 e = r.ldarx(base, ps_read32(base, q + kBuffer) + slot * 8);
        lwsync();
        h = ps_read64(base, q);
        tail = static_cast<u32>(h & 0xFFFF);
        const u64 round = (h >> 31) & 1;
        const u32 dist = distance(slot, tail, ps_read16(base, q + kEntries));
        if (dist >= ((h >> 16) & 0x7FFF)) {
            skip = 0;
            continue;
        }
        bool restart = false;
        for (u32 j = 0; j < dist && !restart; j++) {
            const u32 idx = (tail + j) % ps_read16(base, q + kEntries);
            const u64 other = ps_read64(base, ps_read32(base, q + kBuffer) + idx * 8) >> 63;
            restart = idx < tail ? other == round : other != round;
        }
        if (restart) {
            skip = 0;
            continue;
        }
        const u64 expect = slot < tail ? round ^ 1 : round;
        if ((e >> 63) == expect) { // taken in this round already
            skip = seen == tail ? skip + 1 : 0;
            continue;
        }
        u64 claimed = (e & ~(kBit63 | (0xFull << 48) | 0xFFFFFFFFFFFFull)) | (expect << 63);
        const bool signalled = e & (1ull << 61);
        if (signalled) claimed &= ~(1ull << 61);
        else claimed |= (type_bit << 48) | (receiver & 0xFFFFFFFFFFFFull);
        claimed = (claimed & ~(0x1FFull << 52)) | ((((claimed >> 52) & 0x1FF) + 1) & 0x1FF) << 52;
        if (!r.stdcx(base, claimed)) continue;
        do {
            h = r.ldarx(base, q);
            h = (h & ~(0x7FFFull << 16)) | ((((h >> 16) & 0x7FFF) - 1) & 0x7FFF) << 16;
            const u32 next = static_cast<u32>((h & 0xFFFF) + 1) & 0xFFFF;
            if (next == ps_read16(base, q + kEntries)) h = (h & ~0xFFFFull & ~(1ull << 31)) | ((((h >> 31) & 1) ^ 1) << 31);
            else h = (h & ~0xFFFFull) | next;
        } while (!r.stdcx(base, h));
        if (!signalled && static_cast<s32>(call_guest(ctx, wait_fn, {receiver, kind, object, arg})) != lv2::CELL_OK)
            sync2_assert("waiter_queue.cpp", 198, "wait");
        return lv2::CELL_OK;
    }
}

// Wakes the waiter of the next entry of the wakers' round through the notifier of its thread type, or leaves a signal
// in the entry for the waiter that has not come yet. CELL_SYNC2_ERROR_NOMEM when the ring has no entry for another
// waker; CELL_SYNC2_ERROR_INVAL when the table has no notifier for the waiter's type.
s32 signal(PPUContext& ctx, u32 q, u32 table, u32 count)
{
    u8* base = guest_base(ctx);
    Reserve r;
    u64 h;
    for (;;) {
        h = r.ldarx(base, q);
        const u32 wakers = static_cast<u32>(((h >> 48) & 0x7FFF) + 1);
        h = (h & ~(0x7FFFull << 48)) | (u64{wakers & 0x7FFF} << 48);
        const u32 n = ps_read16(base, q + kEntries);
        const u32 pos = static_cast<u32>((h >> 32) & 0xFFFF) + static_cast<u32>((h >> 48) & 0x7FFF);
        const u64 round = pos >= n ? (~h >> 63) : (h >> 63);
        if (pos % n == (h & 0xFFFF) && ((h >> 31) & 1) != round) return error(CELL_SYNC2_ERROR_NOMEM);
        if (r.stdcx(base, h)) break;
    }
    u32 head = static_cast<u32>((h >> 32) & 0xFFFF);
    u32 skip = 0;
    for (;;) {
        const u32 seen = head;
        const u32 slot = (skip + head) % ps_read16(base, q + kEntries);
        const u64 e = r.ldarx(base, ps_read32(base, q + kBuffer) + slot * 8);
        lwsync();
        h = ps_read64(base, q);
        head = static_cast<u32>((h >> 32) & 0xFFFF);
        const u64 round = h >> 63;
        const u32 dist = distance(slot, head, ps_read16(base, q + kEntries));
        if (dist >= ((h >> 48) & 0x7FFF)) {
            skip = 0;
            continue;
        }
        bool restart = false;
        for (u32 j = 0; j < dist && !restart; j++) {
            const u32 idx = (head + j) % ps_read16(base, q + kEntries);
            const u64 other = (ps_read64(base, ps_read32(base, q + kBuffer) + idx * 8) >> 62) & 1;
            restart = idx < head ? other == round : other != round;
        }
        if (restart) {
            skip = 0;
            continue;
        }
        const u64 expect = slot < head ? round ^ 1 : round;
        if (((e >> 62) & 1) == expect) { // woken in this round already
            skip = seen == head ? skip + 1 : 0;
            continue;
        }
        const bool waiter = (e >> 63) == expect;
        u64 woken, receiver = 0;
        u16 type_bit = 0;
        if (waiter) {
            type_bit = static_cast<u16>(1u << ((e >> 48) & 0xF));
            receiver = e & 0xFFFFFFFFFFFFull;
            woken = (e & ~(1ull << 62) & ~(1ull << 61)) | (expect << 62);
        } else {
            woken = (e & ~((0xFull << 48) | 0xFFFFFFFFFFFFull | (1ull << 62))) | (expect << 62) | (1ull << 61);
        }
        if (!r.stdcx(base, woken)) continue;
        s32 status = lv2::CELL_OK;
        u32 send = 0;
        u64 send_arg = 0;
        if (waiter) {
            for (u32 i = 0; i < count; i++) { // the last notifier of the type
                const u32 nt = ps_read32(base, table + 4 * i);
                if (ps_read16(base, nt) != type_bit) continue;
                send_arg = ps_read64(base, nt + 8);
                send = ps_read32(base, nt + 4);
            }
            if (!send) status = error(CELL_SYNC2_ERROR_INVAL);
        }
        do {
            h = r.ldarx(base, q);
            h = (h & ~(0x7FFFull << 48)) | ((((h >> 48) & 0x7FFF) - 1) & 0x7FFF) << 48;
            if (status == lv2::CELL_OK) {
                const u32 next = static_cast<u32>((h >> 32) + 1) & 0xFFFF;
                if (next == ps_read16(base, q + kEntries)) h = (h & ~(0xFFFFull << 32) & ~kBit63) | (~h & kBit63);
                else h = (h & ~(0xFFFFull << 32)) | (u64{next} << 32);
            }
        } while (!r.stdcx(base, h));
        if (waiter && send && static_cast<s32>(call_guest(ctx, send, {receiver, send_arg})) != lv2::CELL_OK)
            sync2_assert("waiter_queue.cpp", 363, "signal");
        return status;
    }
}

} // namespace wq

// ------------------------------------------------------------------------------------------------------- mutex
//
// CellSync2Mutex: +0 waiter queue | +0x10 u16 owner's thread type | +0x14 u32 recursion count | +0x18 u64 owner
// | +0x20 u64 C: bit 63 locked, 32..47 hand-overs pending, 16..31 waiters, 0..15 conditions on it
// | +0x28 sdkVersion | +0x2C flags (bit 0 recursive) | +0x30 u16 thread types | +0x32 u16 maxWaiters | +0x34 name.

namespace mx {
constexpr u32 kOwnerType = 0x10, kCount = 0x14, kOwner = 0x18, kC = 0x20, kSdk = 0x28, kFlags = 0x2C, kTypes = 0x30,
              kMaxWaiters = 0x32, kName = 0x34;
}

// Spinning before a waiter sleeps (libsync2's priority hints): a yield here.
void spin_pause() { std::this_thread::yield(); }

// libsync2's lock (mutex.cpp "lock"): taken, taken again by its recursive owner (recursion count `count`), or, for a
// caller that waits, waited for after spinning spinWaitNanoSec: the unlocking owner hands it over.
s32 mutex_lock(PPUContext& ctx, u32 m, bool wait, const Caller& c, u32 count)
{
    u8* base = guest_base(ctx);
    if (wait && ps_read16(base, m + mx::kMaxWaiters) == 0) return error(CELL_SYNC2_ERROR_NOMEM);
    const u64 deadline = timebase() + (((u64{c.spin} * 1307) >> 14) & 0x3FFFF);
    bool have_receiver = false;
    u64 receiver = 0;
    Reserve r;
    for (;;) {
        u64 v = r.ldarx(base, m + mx::kC);
        bool joined = false;
        if (static_cast<s64>(v) >= 0) {
            v |= kBit63;
        } else {
            const u64 self = caller_self(ctx, c);
            if (ps_read16(base, m + mx::kOwnerType) == c.type && ps_read64(base, m + mx::kOwner) == self) {
                if (!(ps_read32(base, m + mx::kFlags) & 1)) return error(CELL_SYNC2_ERROR_DEADLK);
                const u32 n = ps_read32(base, m + mx::kCount);
                if (n == 0xFFFFFFFF) return error(CELL_SYNC2_ERROR_AGAIN);
                count = n + 1;
            } else {
                if (!wait) return error(CELL_SYNC2_ERROR_BUSY);
                const u32 waiters = static_cast<u32>((v >> 16) & 0xFFFF);
                if (ps_read16(base, m + mx::kMaxWaiters) == waiters) return error(CELL_SYNC2_ERROR_NOMEM);
                if (deadline > timebase()) {
                    spin_pause();
                    continue;
                }
                v = (v & ~(0xFFFFull << 16)) | (u64{(waiters + 1) & 0xFFFF} << 16);
                joined = true;
                if (!have_receiver) {
                    if (const s32 e = allocate_receiver(ctx, c, kForMutex, m, receiver); e != lv2::CELL_OK) return e;
                    have_receiver = true;
                    continue;
                }
            }
        }
        if (!r.stdcx(base, v)) continue;
        if (joined) {
            if (wq::wait(ctx, m, c.type, receiver, c.wait, kForMutex, m, c.arg) != lv2::CELL_OK)
                sync2_assert("mutex.cpp", 436, "lock");
            if (static_cast<s64>(ps_read64(base, m + mx::kC)) >= 0) sync2_assert("mutex.cpp", 437, "lock");
            do {
                v = r.ldarx(base, m + mx::kC);
                v = (v & ~(0xFFFFull << 32)) | ((((v >> 32) - 1) & 0xFFFF) << 32);
                v = (v & ~(0xFFFFull << 16)) | ((((v >> 16) - 1) & 0xFFFF) << 16);
            } while (!r.stdcx(base, v));
        }
        if (have_receiver && free_receiver(ctx, c, receiver) != lv2::CELL_OK) sync2_assert("mutex.cpp", 448, "lock");
        ps_write16(base, m + mx::kOwnerType, c.type);
        const u64 self = caller_self(ctx, c);
        ps_write32(base, m + mx::kCount, count);
        ps_write64(base, m + mx::kOwner, self);
        stored(m + mx::kOwnerType, 0x10);
        lwsync();
        return lv2::CELL_OK;
    }
}

// libsync2's unlock: by its owner; a recursive mutex counts down. With waiters not yet handed it, it stays locked and
// goes to the next one. `saved` (a condition's wait) takes the recursion count, and the mutex is released whole.
s32 mutex_unlock(PPUContext& ctx, u32 m, const Caller& c, u32 table, u32 count, u32* saved)
{
    u8* base = guest_base(ctx);
    if (static_cast<s64>(ps_read64(base, m + mx::kC)) >= 0) return error(CELL_SYNC2_ERROR_STAT);
    const u64 self = caller_self(ctx, c);
    if (ps_read16(base, m + mx::kOwnerType) != c.type || ps_read64(base, m + mx::kOwner) != self)
        return error(CELL_SYNC2_ERROR_PERM);
    if (!saved) {
        if (ps_read32(base, m + mx::kFlags) & 1) {
            const u32 n = ps_read32(base, m + mx::kCount) - 1;
            ps_write32(base, m + mx::kCount, n);
            stored(m + mx::kCount, 4);
            if (n) return lv2::CELL_OK;
        }
    } else {
        *saved = ps_read32(base, m + mx::kCount);
    }
    ps_write32(base, m + mx::kCount, 0);
    ps_write16(base, m + mx::kOwnerType, 0);
    ps_write64(base, m + mx::kOwner, 0);
    stored(m + mx::kOwnerType, 0x10);
    lwsync();
    Reserve r;
    bool hand_over;
    u64 v;
    do {
        v = r.ldarx(base, m + mx::kC);
        const s32 left = static_cast<s32>(((v >> 16) & 0xFFFF) - ((v >> 32) & 0xFFFF));
        hand_over = left > 0;
        if (hand_over) v = (v & ~(0xFFFFull << 32)) | ((((v >> 32) + 1) & 0xFFFF) << 32);
        else v &= ~kBit63;
    } while (!r.stdcx(base, v));
    if (hand_over) {
        if (const s32 e = wq::signal(ctx, m, table, count); e != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "ret = {:#x}", static_cast<u32>(e));
            sync2_assert("mutex.cpp", 524, "unlock");
        }
    }
    return lv2::CELL_OK;
}

// A condition's reference on its mutex (C bits 0..15): CELL_SYNC2_ERROR_AGAIN past 65535, STAT below 0.
s32 mutex_ref(u8* base, u32 m, bool add)
{
    Reserve r;
    u64 v;
    do {
        v = r.ldarx(base, m + mx::kC);
        if (add && ((v + 1) & 0xFFFF) == 0) return error(CELL_SYNC2_ERROR_AGAIN);
        if (!add && (v & 0xFFFF) == 0) return error(CELL_SYNC2_ERROR_STAT);
        v = (v & ~0xFFFFull) | ((add ? v + 1 : v - 1) & 0xFFFF);
    } while (!r.stdcx(base, v));
    return lv2::CELL_OK;
}

// An attribute's sdkVersion, as libsync2 takes it: 0x270000..0x330000.
bool sdk_ok(u32 sdk) { return sdk - 0x270000u <= 0xC0000; }

s32 mutex_attr_check(u8* base, u32 attr)
{
    if (!attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (!sdk_ok(ps_read32(base, attr)) || ps_read16(base, attr + 6) > 0x8000) return error(CELL_SYNC2_ERROR_INVAL);
    return lv2::CELL_OK;
}

// The object (128 bytes) as guest memory.
bool object_ok(PPUContext& ctx, u32 obj) { return guest_readable(thread_of(ctx), obj, 0x80); }

// An attribute's default: zeros, the given fields, the name.
s32 attribute(PPUContext& ctx, std::string_view fn, u32 attr, u32 sdk, std::initializer_list<std::pair<u32, u32>> u16s,
    std::initializer_list<std::pair<u32, u32>> u32s, u32 name_at, std::string_view name)
{
    if (!attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (!guest_readable(thread_of(ctx), attr, 0x80)) return not_guest(ctx, fn, "attribute", attr);
    u8* base = guest_base(ctx);
    std::memset(base + attr, 0, 0x80);
    for (const auto& [at, v] : u16s) ps_write16(base, attr + at, static_cast<u16>(v));
    for (const auto& [at, v] : u32s) ps_write32(base, attr + at, v);
    ps_write32(base, attr, sdk);
    std::memcpy(base + attr + name_at, name.data(), name.size()); // strncpy into zeros
    return lv2::CELL_OK;
}

s32 _cellSync2MutexAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    return attribute(ctx, "_cellSync2MutexAttributeInitialize", attr, sdk, {{6, 15}, {4, 0x10F}}, {}, 10, "CellSync2Mutex");
}

// *EstimateBufferSize's common part: the attribute's check, then the size written.
template <typename Check, typename Size>
s32 estimate(PPUContext& ctx, std::string_view fn, u32 attr, u32 out, Check&& check, Size&& size)
{
    u8* base = guest_base(ctx);
    if (attr && !guest_readable(thread_of(ctx), attr, 0x80)) return not_guest(ctx, fn, "attribute", attr);
    if (const s32 e = check(base, attr); e != lv2::CELL_OK) return e;
    if (!out) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (!guest_readable(thread_of(ctx), out, 4)) return not_guest(ctx, fn, "size", out);
    ps_write32(base, out, size(base, attr));
    return lv2::CELL_OK;
}

s32 cellSync2MutexEstimateBufferSize(PPUContext& ctx, u32 attr, u32 size)
{
    return estimate(ctx, "cellSync2MutexEstimateBufferSize", attr, size, mutex_attr_check, [](u8* base, u32 a) {
        const u16 n = ps_read16(base, a + 6);
        return n ? n * 8u + 8 : 0u;
    });
}

s32 cellSync2MutexInitialize(PPUContext& ctx, u32 m, u32 buffer, u32 attr)
{
    constexpr std::string_view fn = "cellSync2MutexInitialize";
    u8* base = guest_base(ctx);
    if (!m || !attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (m & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, m)) return not_guest(ctx, fn, "mutex", m);
    if (!guest_readable(thread_of(ctx), attr, 0x80)) return not_guest(ctx, fn, "attribute", attr);
    if (const s32 e = mutex_attr_check(base, attr); e != lv2::CELL_OK) return e;
    const u16 max_waiters = ps_read16(base, attr + 6);
    if (max_waiters) {
        if (!buffer) return error(CELL_SYNC2_ERROR_NULL_POINTER);
        if (buffer & 7) return error(CELL_SYNC2_ERROR_ALIGN);
    }
    const u16 types = ps_read16(base, attr + 4);
    if (!types || (types & kForbiddenTypes)) return error(CELL_SYNC2_ERROR_INVAL);
    if (max_waiters && !guest_range(thread_of(ctx), buffer, max_waiters * 8u + 8)) return not_guest(ctx, fn, "buffer", buffer);
    std::memset(base + m, 0, 0x80);
    if (max_waiters) {
        if (const s32 e = wq::init(base, m, buffer, max_waiters * 8u + 8); e != lv2::CELL_OK) return e;
        if (max_waiters != static_cast<u16>(ps_read16(base, m + wq::kEntries) - 1)) sync2_assert("mutex.cpp", 267, "initialize");
    }
    ps_write32(base, m + mx::kSdk, ps_read32(base, attr));
    if (base[attr + 8]) ps_write32(base, m + mx::kFlags, ps_read32(base, m + mx::kFlags) | 1);
    ps_write16(base, m + mx::kTypes, types);
    ps_write16(base, m + mx::kMaxWaiters, max_waiters);
    copy_name(base, m + mx::kName, attr + 10, 31);
    stored(m, 0x80);
    lwsync();
    return lv2::CELL_OK;
}

s32 cellSync2MutexFinalize(PPUContext& ctx, u32 m)
{
    if (!m) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (m & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, m)) return not_guest(ctx, "cellSync2MutexFinalize", "mutex", m);
    u8* base = guest_base(ctx);
    if (static_cast<s64>(ps_read64(base, m + mx::kC)) < 0 || ps_read16(base, m + mx::kC + 6) != 0)
        return error(CELL_SYNC2_ERROR_BUSY);
    return ps_read16(base, m + mx::kMaxWaiters) ? wq::idle(base, m) : lv2::CELL_OK;
}

// Lock and TryLock: the configuration's caller (only its first field is read), then lock.
s32 mutex_take(PPUContext& ctx, std::string_view fn, u32 m, u32 cfg, bool wait)
{
    if (!m) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (m & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, m)) return not_guest(ctx, fn, "mutex", m);
    const u16 types = ps_read16(guest_base(ctx), m + mx::kTypes);
    Config config;
    if (const s32 e = read_config(ctx, fn, cfg, types, config, false); e != lv2::CELL_OK) return e;
    Caller c;
    if (const s32 e = check_caller(ctx, fn, config.caller, types, wait, c); e != lv2::CELL_OK) return e;
    return mutex_lock(ctx, m, wait, c, 1);
}

s32 cellSync2MutexLock(PPUContext& ctx, u32 m, u32 cfg) { return mutex_take(ctx, "cellSync2MutexLock", m, cfg, true); }
s32 cellSync2MutexTryLock(PPUContext& ctx, u32 m, u32 cfg) { return mutex_take(ctx, "cellSync2MutexTryLock", m, cfg, false); }

s32 cellSync2MutexUnlock(PPUContext& ctx, u32 m, u32 cfg)
{
    constexpr std::string_view fn = "cellSync2MutexUnlock";
    if (!m) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (m & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, m)) return not_guest(ctx, fn, "mutex", m);
    const u16 types = ps_read16(guest_base(ctx), m + mx::kTypes);
    Config config;
    if (const s32 e = read_config(ctx, fn, cfg, types, config, true); e != lv2::CELL_OK) return e;
    Caller c;
    if (const s32 e = check_caller(ctx, fn, config.caller, types, false, c); e != lv2::CELL_OK) return e;
    if (const s32 e = check_notifiers(ctx, fn, config.table, config.count, types); e != lv2::CELL_OK) return e;
    return mutex_unlock(ctx, m, c, config.table, config.count, nullptr);
}

// --------------------------------------------------------------------------------------------------- condition
//
// CellSync2Cond: +0 waiter queue | +0x10 u64: 16..31 signals given, 0..15 waiters | +0x18 u16 maxWaiters
// | +0x1A u16 its mutex's thread types | +0x1C sdkVersion | +0x24 u32 its mutex | +0x28 name (31) | +0x47 0.

namespace cv {
constexpr u32 kC = 0x10, kMaxWaiters = 0x18, kTypes = 0x1A, kSdk = 0x1C, kMutex = 0x24, kName = 0x28;
}

// A condition's or semaphore's attribute: maxWaiters (at `waiters_at`) 1..0x8000.
s32 waiters_attr_check(u8* base, u32 attr, u32 waiters_at)
{
    if (!attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    const u16 n = ps_read16(base, attr + waiters_at);
    if (sdk_ok(ps_read32(base, attr)) && n <= 0x8000 && n != 0) return lv2::CELL_OK;
    return error(CELL_SYNC2_ERROR_INVAL);
}
s32 cond_attr_check(u8* base, u32 attr) { return waiters_attr_check(base, attr, 4); }

s32 _cellSync2CondAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    return attribute(ctx, "_cellSync2CondAttributeInitialize", attr, sdk, {{4, 15}}, {}, 6, "CellSync2Cond");
}

s32 cellSync2CondEstimateBufferSize(PPUContext& ctx, u32 attr, u32 size)
{
    return estimate(ctx, "cellSync2CondEstimateBufferSize", attr, size, cond_attr_check,
        [](u8* base, u32 a) { return ps_read16(base, a + 4) * 8u + 8; });
}

s32 cellSync2CondInitialize(PPUContext& ctx, u32 cond, u32 m, u32 buffer, u32 attr)
{
    constexpr std::string_view fn = "cellSync2CondInitialize";
    u8* base = guest_base(ctx);
    if (!cond || !m || !buffer || !attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if ((cond & 0x7F) || (m & 0x7F) || (buffer & 7)) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, cond)) return not_guest(ctx, fn, "condition", cond);
    if (!object_ok(ctx, m)) return not_guest(ctx, fn, "mutex", m);
    if (!guest_readable(thread_of(ctx), attr, 0x80)) return not_guest(ctx, fn, "attribute", attr);
    if (const s32 e = cond_attr_check(base, attr); e != lv2::CELL_OK) return e;
    const u16 max_waiters = ps_read16(base, attr + 4);
    if (!guest_range(thread_of(ctx), buffer, max_waiters * 8u + 8)) return not_guest(ctx, fn, "buffer", buffer);
    std::memset(base + cond, 0, 0x80);
    ps_write32(base, cond + cv::kMutex, m);
    if (max_waiters > ((ps_read16(base, m + wq::kEntries) - 1u) & 0xFFFF)) return error(CELL_SYNC2_ERROR_INVAL);
    if (const s32 e = wq::init(base, cond, buffer, max_waiters * 8u + 8); e != lv2::CELL_OK) return e;
    ps_write16(base, cond + cv::kMaxWaiters, static_cast<u16>(ps_read16(base, cond + wq::kEntries) - 1));
    if (static_cast<u16>(ps_read16(base, cond + wq::kEntries) - 1) != max_waiters) sync2_assert("cond.cpp", 241, "initialize");
    ps_write16(base, cond + cv::kTypes, ps_read16(base, m + mx::kTypes));
    if (const s32 e = mutex_ref(base, m, true); e != lv2::CELL_OK) {
        if (wq::idle(base, cond) != lv2::CELL_OK) sync2_assert("cond.cpp", 248, "initialize");
        return e;
    }
    ps_write32(base, cond + cv::kSdk, ps_read32(base, attr));
    copy_name(base, cond + cv::kName, attr + 6, 31);
    base[cond + 0x47] = 0;
    stored(cond, 0x80);
    lwsync();
    return lv2::CELL_OK;
}

s32 cellSync2CondFinalize(PPUContext& ctx, u32 cond)
{
    if (!cond) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (cond & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, cond)) return not_guest(ctx, "cellSync2CondFinalize", "condition", cond);
    u8* base = guest_base(ctx);
    const s32 e = wq::idle(base, cond);
    if (e == lv2::CELL_OK && mutex_ref(base, ps_read32(base, cond + cv::kMutex), false) != lv2::CELL_OK)
        sync2_assert("cond.cpp", 272, "finalize");
    return e;
}

// One more signal than were given, while waiters are left: CELL_SYNC2_ERROR_AGAIN with none.
s32 cond_signal_one(PPUContext& ctx, u32 cond, u32 table, u32 count)
{
    u8* base = guest_base(ctx);
    Reserve r;
    u64 v;
    do {
        v = r.ldarx(base, cond + cv::kC);
        if (((v >> 16) & 0xFFFF) == (v & 0xFFFF)) return error(CELL_SYNC2_ERROR_AGAIN);
        v = (v & ~(0xFFFFull << 16)) | ((((v >> 16) + 1) & 0xFFFF) << 16);
    } while (!r.stdcx(base, v));
    return wq::signal(ctx, cond, table, count);
}

// Signal, SignalAll and Wait's common part: the configuration, the caller and the notifiers.
s32 cond_caller(PPUContext& ctx, std::string_view fn, u32 cond, u32 cfg, bool waits, Caller& c, Config& config)
{
    if (!cond) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (cond & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, cond)) return not_guest(ctx, fn, "condition", cond);
    const u16 types = ps_read16(guest_base(ctx), cond + cv::kTypes);
    if (const s32 e = read_config(ctx, fn, cfg, types, config, true); e != lv2::CELL_OK) return e;
    if (const s32 e = check_caller(ctx, fn, config.caller, types, waits, c); e != lv2::CELL_OK) return e;
    return check_notifiers(ctx, fn, config.table, config.count, types);
}

s32 cellSync2CondSignal(PPUContext& ctx, u32 cond, u32 cfg)
{
    Caller c;
    Config config;
    if (const s32 e = cond_caller(ctx, "cellSync2CondSignal", cond, cfg, false, c, config); e != lv2::CELL_OK) return e;
    const s32 e = cond_signal_one(ctx, cond, config.table, config.count);
    return e == error(CELL_SYNC2_ERROR_AGAIN) ? lv2::CELL_OK : e;
}

s32 cellSync2CondSignalAll(PPUContext& ctx, u32 cond, u32 cfg)
{
    Caller c;
    Config config;
    if (const s32 e = cond_caller(ctx, "cellSync2CondSignalAll", cond, cfg, false, c, config); e != lv2::CELL_OK) return e;
    s32 e;
    do e = cond_signal_one(ctx, cond, config.table, config.count);
    while (e == lv2::CELL_OK);
    return e == error(CELL_SYNC2_ERROR_AGAIN) ? lv2::CELL_OK : e;
}

// Wait: by the mutex's owner; the mutex is released whole while it waits and taken back, recursion count and all.
s32 cellSync2CondWait(PPUContext& ctx, u32 cond, u32 cfg)
{
    constexpr std::string_view fn = "cellSync2CondWait";
    Caller c;
    Config config;
    if (const s32 e = cond_caller(ctx, fn, cond, cfg, true, c, config); e != lv2::CELL_OK) return e;
    u8* base = guest_base(ctx);
    const u32 m = ps_read32(base, cond + cv::kMutex);
    if (!object_ok(ctx, m)) return not_guest(ctx, fn, "mutex", m);
    if (static_cast<s64>(ps_read64(base, m + mx::kC)) >= 0) return error(CELL_SYNC2_ERROR_STAT);
    const u64 self = caller_self(ctx, c);
    if (ps_read16(base, m + mx::kOwnerType) != c.type || ps_read64(base, m + mx::kOwner) != self)
        return error(CELL_SYNC2_ERROR_PERM);
    u64 receiver = 0;
    if (const s32 e = allocate_receiver(ctx, c, kForCond, cond, receiver); e != lv2::CELL_OK) return e;
    Reserve r;
    u64 v;
    for (;;) {
        v = r.ldarx(base, cond + cv::kC);
        if (ps_read16(base, cond + cv::kMaxWaiters) == (v & 0xFFFF)) {
            if (free_receiver(ctx, c, receiver) != lv2::CELL_OK) sync2_assert("cond.cpp", 323, "wait");
            return error(CELL_SYNC2_ERROR_NOMEM);
        }
        v = (v & ~0xFFFFull) | ((v + 1) & 0xFFFF);
        if (r.stdcx(base, v)) break;
    }
    u32 count = 0;
    if (mutex_unlock(ctx, m, c, config.table, config.count, &count) != lv2::CELL_OK) sync2_assert("cond.cpp", 335, "wait");
    if (wq::wait(ctx, cond, c.type, receiver, c.wait, kForCond, cond, c.arg) != lv2::CELL_OK)
        sync2_assert("cond.cpp", 340, "wait");
    do {
        v = r.ldarx(base, cond + cv::kC);
        v = (v & ~0xFFFFFFFFull) | ((v - 1) & 0xFFFF) | ((((v >> 16) - 1) & 0xFFFF) << 16);
    } while (!r.stdcx(base, v));
    if (free_receiver(ctx, c, receiver) != lv2::CELL_OK) sync2_assert("cond.cpp", 349, "wait");
    return mutex_lock(ctx, m, true, c, count);
}

// --------------------------------------------------------------------------------------------------- semaphore
//
// CellSync2Semaphore: +0 waiter queue (its entries at +0x48 with one waiter) | +0x10 u64 S: high word the count;
// low word, with one waiter: bit 31 the waiter was given what it waits for, bit 30 a waiter, 0..29 what it still
// needs; with more: 16..31 hand-overs, 0..15 waiters | +0x18 sdkVersion | +0x20 u16 thread types | +0x22 u16 maxWaiters
// | +0x28 name.

namespace sm {
constexpr u32 kS = 0x10, kSdk = 0x18, kTypes = 0x20, kMaxWaiters = 0x22, kName = 0x28, kInline = 0x48;
}

s32 semaphore_attr_check(u8* base, u32 attr) { return waiters_attr_check(base, attr, 6); }

s32 _cellSync2SemaphoreAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    return attribute(ctx, "_cellSync2SemaphoreAttributeInitialize", attr, sdk, {{6, 1}, {4, 0x10F}}, {}, 8,
        "CellSync2Semaphore");
}

s32 cellSync2SemaphoreEstimateBufferSize(PPUContext& ctx, u32 attr, u32 size)
{
    return estimate(ctx, "cellSync2SemaphoreEstimateBufferSize", attr, size, semaphore_attr_check, [](u8* base, u32 a) {
        const u16 n = ps_read16(base, a + 6);
        return n < 2 ? 0u : n * 8u + 8;
    });
}

s32 cellSync2SemaphoreInitialize(PPUContext& ctx, u32 s, u32 buffer, s32 value, u32 attr)
{
    constexpr std::string_view fn = "cellSync2SemaphoreInitialize";
    u8* base = guest_base(ctx);
    if (!s || !attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if ((s & 0x7F) || (buffer && (buffer & 7))) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, s)) return not_guest(ctx, fn, "semaphore", s);
    if (!guest_readable(thread_of(ctx), attr, 0x80)) return not_guest(ctx, fn, "attribute", attr);
    if (const s32 e = semaphore_attr_check(base, attr); e != lv2::CELL_OK) return e;
    const u16 max_waiters = ps_read16(base, attr + 6);
    if (max_waiters == 1) {
        if (buffer) return error(CELL_SYNC2_ERROR_INVAL);
    } else if (max_waiters > 1 && !buffer) {
        return error(CELL_SYNC2_ERROR_NULL_POINTER);
    }
    const u16 types = ps_read16(base, attr + 4);
    if (!types || (types & kForbiddenTypes)) return error(CELL_SYNC2_ERROR_INVAL);
    if (buffer && !guest_range(thread_of(ctx), buffer, max_waiters * 8u + 8)) return not_guest(ctx, fn, "buffer", buffer);
    std::memset(base + s, 0, 0x80);
    if (static_cast<u32>(value) + 0x800000u >= 0x1000000u) return error(CELL_SYNC2_ERROR_INVAL);
    const u32 entries_at = buffer ? buffer : s + sm::kInline;
    if (const s32 e = wq::init(base, s, entries_at, max_waiters * 8u + 8); e != lv2::CELL_OK) return e;
    if (static_cast<u16>(ps_read16(base, s + wq::kEntries) - 1) != max_waiters) sync2_assert("semaphore.cpp", 286, "initialize");
    ps_write32(base, s + sm::kS, static_cast<u32>(value));
    ps_write16(base, s + sm::kTypes, types);
    ps_write16(base, s + sm::kMaxWaiters, static_cast<u16>(ps_read16(base, s + wq::kEntries) - 1));
    ps_write32(base, s + sm::kSdk, ps_read32(base, attr));
    copy_name(base, s + sm::kName, attr + 8, 31);
    stored(s, 0x80);
    lwsync();
    return lv2::CELL_OK;
}

s32 cellSync2SemaphoreFinalize(PPUContext& ctx, u32 s)
{
    if (!s) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (s & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, s)) return not_guest(ctx, "cellSync2SemaphoreFinalize", "semaphore", s);
    return wq::idle(guest_base(ctx), s);
}

s32 cellSync2SemaphoreGetCount(PPUContext& ctx, u32 s, u32 count)
{
    if (!s || !count) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (s & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, s)) return not_guest(ctx, "cellSync2SemaphoreGetCount", "semaphore", s);
    if (!guest_readable(thread_of(ctx), count, 4)) return not_guest(ctx, "cellSync2SemaphoreGetCount", "count", count);
    ps_write32(guest_base(ctx), count, ps_read32(guest_base(ctx), s + sm::kS));
    return lv2::CELL_OK;
}

// Acquire and TryAcquire (semaphore.cpp "acquire"): `count` taken at once, or (waiting) what there is now and the rest
// handed over by releases. More than one at a time only with one waiter, below 0x800000.
s32 semaphore_acquire(PPUContext& ctx, std::string_view fn, u32 s, u32 count, u32 cfg, bool wait)
{
    if (!s) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (s & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, s)) return not_guest(ctx, fn, "semaphore", s);
    u8* base = guest_base(ctx);
    const u16 types = ps_read16(base, s + sm::kTypes);
    Config config;
    if (const s32 e = read_config(ctx, fn, cfg, types, config, false); e != lv2::CELL_OK) return e;
    Caller c;
    if (const s32 e = check_caller(ctx, fn, config.caller, types, wait, c); e != lv2::CELL_OK) return e;
    const u16 max_waiters = ps_read16(base, s + sm::kMaxWaiters);
    if (!((max_waiters == 1 || count < 2 || !wait) && count < 0x800000)) return error(CELL_SYNC2_ERROR_INVAL);
    if (!count) return lv2::CELL_OK;
    bool have_receiver = false, waiting = false;
    u64 receiver = 0;
    Reserve r;
    for (;;) {
        u64 v = r.ldarx(base, s + sm::kS);
        if (static_cast<s32>(count) <= static_cast<s32>(v >> 32)) {
            v = (((v >> 32) - count) << 32) | (v & 0xFFFFFFFF);
            waiting = false;
        } else {
            if (!wait) return error(CELL_SYNC2_ERROR_AGAIN);
            if (!have_receiver) {
                if (const s32 e = allocate_receiver(ctx, c, kForSemaphore, s, receiver); e != lv2::CELL_OK) return e;
                have_receiver = true;
                continue;
            }
            if (max_waiters == 1) {
                if (v & 0x40000000) return error(CELL_SYNC2_ERROR_BUSY);
                v = ((count - (v >> 32)) & 0x3FFFFFFF) | (v & 0xC0000000) | 0x40000000;
            } else {
                if (max_waiters == (v & 0xFFFF)) return error(CELL_SYNC2_ERROR_NOMEM);
                v = (v & ~0xFFFFull) | ((v + 1) & 0xFFFF);
            }
            waiting = true;
        }
        if (r.stdcx(base, v)) break;
    }
    if (waiting) {
        if (!have_receiver) sync2_assert("semaphore.cpp", 392, "acquire");
        if (wq::wait(ctx, s, c.type, receiver, c.wait, kForSemaphore, s, c.arg) != lv2::CELL_OK)
            sync2_assert("semaphore.cpp", 402, "acquire");
        u64 v;
        do {
            v = r.ldarx(base, s + sm::kS);
            if (max_waiters == 1) v &= 0xFFFFFFFF3FFFFFFFull;
            else v = (v & ~0xFFFFFFFFull) | ((v - 1) & 0xFFFF) | ((((v >> 16) - 1) & 0xFFFF) << 16);
        } while (!r.stdcx(base, v));
    }
    if (have_receiver && free_receiver(ctx, c, receiver) != lv2::CELL_OK) sync2_assert("semaphore.cpp", 419, "acquire");
    lwsync();
    return lv2::CELL_OK;
}

s32 cellSync2SemaphoreAcquire(PPUContext& ctx, u32 s, u32 count, u32 cfg)
{
    return semaphore_acquire(ctx, "cellSync2SemaphoreAcquire", s, count, cfg, true);
}
s32 cellSync2SemaphoreTryAcquire(PPUContext& ctx, u32 s, u32 count, u32 cfg)
{
    return semaphore_acquire(ctx, "cellSync2SemaphoreTryAcquire", s, count, cfg, false);
}

// Release: `count` more, handed to waiters first (as many as they wait for), each woken through its notifier.
s32 cellSync2SemaphoreRelease(PPUContext& ctx, u32 s, u32 count, u32 cfg)
{
    constexpr std::string_view fn = "cellSync2SemaphoreRelease";
    if (!s) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (s & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, s)) return not_guest(ctx, fn, "semaphore", s);
    u8* base = guest_base(ctx);
    const u16 types = ps_read16(base, s + sm::kTypes);
    Config config;
    if (const s32 e = read_config(ctx, fn, cfg, types, config, true); e != lv2::CELL_OK) return e;
    Caller c;
    if (const s32 e = check_caller(ctx, fn, config.caller, types, false, c); e != lv2::CELL_OK) return e;
    if (const s32 e = check_notifiers(ctx, fn, config.table, config.count, types); e != lv2::CELL_OK) return e;
    if (count > 0x7FFFFF) return error(CELL_SYNC2_ERROR_INVAL);
    if (!count) return lv2::CELL_OK;
    lwsync();
    const u16 max_waiters = ps_read16(base, s + sm::kMaxWaiters);
    Reserve r;
    s32 woken;
    for (;;) {
        const u64 v = r.ldarx(base, s + sm::kS);
        const s32 now = static_cast<s32>(static_cast<u32>(v >> 32) + count);
        if (now > 0x7FFFFF) return error(CELL_SYNC2_ERROR_STAT);
        u64 next = (static_cast<u64>(static_cast<u32>(now)) << 32) | (v & 0xFFFFFFFF);
        woken = 0;
        if (now >= 1) {
            if (max_waiters == 1) {
                if (static_cast<s32>(((v >> 30) & 1) - ((v >> 31) & 1)) >= 1) {
                    const u32 need = static_cast<u32>(v & 0x3FFFFFFF);
                    if (!need) sync2_assert("semaphore.cpp", 488, "release");
                    if (now < static_cast<s32>(need)) {
                        next = ((need - static_cast<u32>(now)) & 0x3FFFFFFF) | (v & 0xC0000000);
                    } else {
                        woken = 1;
                        next = (static_cast<u64>(static_cast<u32>(now) - need) << 32) | (v & 0xC0000000) | 0x80000000;
                    }
                }
            } else {
                const s32 waiting = static_cast<s32>((v & 0xFFFF) - ((v >> 16) & 0xFFFF));
                if (waiting >= 1) {
                    woken = std::min(waiting, now);
                    next = (static_cast<u64>(static_cast<u32>(now - woken)) << 32) |
                           ((((v >> 16) + static_cast<u32>(woken)) & 0xFFFF) << 16) | (v & 0xFFFF);
                    if (static_cast<s64>(next) < 0) sync2_assert("semaphore.cpp", 481, "release");
                }
            }
        }
        if (r.stdcx(base, next)) break;
    }
    lwsync();
    for (s32 i = 0; i < woken; i++)
        if (wq::signal(ctx, s, config.table, config.count) != lv2::CELL_OK) sync2_assert("semaphore.cpp", 509, "release");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------------- queue
//
// CellSync2Queue: +0 the pushers' waiter queue | +0x10 the poppers' | +0x20 u64 P: high word the elements, 16..31
// poppers waiting, 0..15 hand-overs to them | +0x28 u32 head (a dummy entry) | +0x2C u32 tail | +0x30 u64 F: high
// word the free entries, 16..31 pushers waiting, 0..15 hand-overs to them | +0x38 u32 where the next allocation looks
// | +0x3C bitmap of entries taken | +0x40 next-entry array (u32 each, -1 none) | +0x44 the entries | +0x48 elementSize
// | +0x4C depth + 1 entries | +0x50 sdkVersion | +0x54 u16 thread types | +0x56 u16 maxPushWaiters | +0x58 u16
// maxPopWaiters | +0x5C name | +0x7B 0. The buffer holds the entries, then the bitmap (a bit per entry, whole
// doublewords), the next array (rounded to 8 bytes), then the pushers' and the poppers' waiter entries.

namespace qu {
constexpr u32 kPushWq = 0, kPopWq = 0x10, kP = 0x20, kHead = 0x28, kTail = 0x2C, kF = 0x30, kCursor = 0x38,
              kBitmap = 0x3C, kNext = 0x40, kEntries = 0x44, kElementSize = 0x48, kSlots = 0x4C, kSdk = 0x50,
              kTypes = 0x54, kMaxPush = 0x56, kMaxPop = 0x58, kName = 0x5C;
}

s32 queue_attr_check(u8* base, u32 attr)
{
    if (!attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    const u16 types = ps_read16(base, attr + 6);
    const u32 size = ps_read32(base, attr + 8), depth = ps_read32(base, attr + 0xC);
    if (!sdk_ok(ps_read32(base, attr)) || !types || (types & kForbiddenTypes) || (size & 0xF) || size > 0x4000 ||
        !size || !depth || depth > 0xFFFFFFFC || ps_read16(base, attr + 0x12) > 0x8000 ||
        ps_read16(base, attr + 0x10) > 0x8000)
        return error(CELL_SYNC2_ERROR_INVAL);
    return lv2::CELL_OK;
}

u32 bitmap_bytes(u32 depth) { return (((depth + 8) >> 3) + 7) & ~7u; }
u32 next_bytes(u32 depth) { return ((depth & 0x3FFFFFFF) * 4 + 0xB) & ~7u; }

s32 _cellSync2QueueAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    return attribute(ctx, "_cellSync2QueueAttributeInitialize", attr, sdk, {{0x10, 15}, {0x12, 15}},
        {{4, 0x10F}, {0xC, 0x400}, {8, 0x10}}, 0x14, "CellSync2Queue");
}

s32 cellSync2QueueEstimateBufferSize(PPUContext& ctx, u32 attr, u32 size)
{
    return estimate(ctx, "cellSync2QueueEstimateBufferSize", attr, size, queue_attr_check, [](u8* base, u32 a) {
        const u32 depth = ps_read32(base, a + 0xC), element = ps_read32(base, a + 8);
        u32 n = (depth + 1) * element + bitmap_bytes(depth) + next_bytes(depth);
        if (const u16 push = ps_read16(base, a + 0x10)) n += push * 8u + 8;
        if (const u16 pop = ps_read16(base, a + 0x12)) n += pop * 8u + 8;
        return n;
    });
}

// Takes one of F's free entries, or (a pusher that waits) waits for one to be handed over; then an entry from the
// bitmap, looking from the cursor on (queue.cpp "allocateEntry").
s32 allocate_entry(PPUContext& ctx, u32 q, u32& index, const Caller* c, bool wait)
{
    u8* base = guest_base(ctx);
    Reserve r;
    bool have_receiver = false, joined = false;
    u64 receiver = 0;
    for (;;) {
        u64 v = r.ldarx(base, q + qu::kF);
        const u32 waiters = static_cast<u32>((v >> 16) & 0xFFFF), free = static_cast<u32>(v >> 32);
        joined = false;
        if (waiters < free) {
            v = (u64{free - 1} << 32) | (v & 0xFFFFFFFF);
        } else {
            v = (v & ~(0xFFFFull << 16)) | (u64{(waiters + 1) & 0xFFFF} << 16);
            joined = true;
            if (!wait) return error(CELL_SYNC2_ERROR_AGAIN);
            if (ps_read16(base, q + qu::kMaxPush) == waiters) return error(CELL_SYNC2_ERROR_NOMEM);
            if (!have_receiver) {
                if (const s32 e = allocate_receiver(ctx, *c, kForQueue, q, receiver); e != lv2::CELL_OK) return e;
                have_receiver = true;
                continue;
            }
        }
        if (r.stdcx(base, v)) break;
    }
    if (joined) {
        if (wq::wait(ctx, q + qu::kPushWq, c->type, receiver, c->wait, kForQueue, q, c->arg) != lv2::CELL_OK)
            sync2_assert("queue.cpp", 536, "allocateEntry");
        u64 v;
        do {
            v = r.ldarx(base, q + qu::kF);
            if (!(v >> 32)) sync2_assert("queue.cpp", 541, "allocateEntry");
            if (!((v >> 16) & 0xFFFF)) sync2_assert("queue.cpp", 542, "allocateEntry");
            if (!(v & 0xFFFF)) sync2_assert("queue.cpp", 543, "allocateEntry");
            v = (((v >> 32) - 1) << 32) | ((((v >> 16) - 1) & 0xFFFF) << 16) | ((v - 1) & 0xFFFF);
        } while (!r.stdcx(base, v));
    }
    if (have_receiver && free_receiver(ctx, *c, receiver) != lv2::CELL_OK) sync2_assert("queue.cpp", 553, "allocateEntry");
    u32 start;
    do {
        start = r.lwarx(base, q + qu::kCursor);
    } while (!r.stwcx(base, start + 1 == ps_read32(base, q + qu::kSlots) ? 0 : start + 1));
    // The first entry not taken from `start` on: a doubleword of the bitmap reserved once and kept while the search
    // stays in it.
    u32 reserved = 0;
    u64 word = 0;
    for (u32 k = 0;;) {
        const u32 i = (start + k) % ps_read32(base, q + qu::kSlots);
        const u32 at = ps_read32(base, q + qu::kBitmap) + ((i >> 3) & ~7u);
        if (at != reserved) {
            word = r.ldarx(base, at);
            reserved = at;
        }
        const u64 bit = kBit63 >> (i & 63);
        if (word & bit) {
            k++;
            continue;
        }
        if (r.stdcx(base, word | bit)) {
            index = i;
            return lv2::CELL_OK;
        }
        reserved = 0;
    }
}

// The tail moved one entry on when one is linked after it (another thread's store in between: looked at again).
void advance_tail(u8* base, u32 q)
{
    Reserve r;
    for (;;) {
        const u32 tail = r.lwarx(base, q + qu::kTail);
        const u32 next = ps_read32(base, ps_read32(base, q + qu::kNext) + tail * 4);
        if (next == 0xFFFFFFFF || r.stwcx(base, next)) return;
    }
}

// Links the chain from `first` after the tail's entry.
void link_at_tail(u8* base, u32 q, u32 first)
{
    Reserve r;
    for (;;) {
        const u32 tail = ps_read32(base, q + qu::kTail);
        const u32 next = r.lwarx(base, ps_read32(base, q + qu::kNext) + tail * 4);
        lwsync();
        if (ps_read32(base, q + qu::kTail) != tail) continue;
        if (next != 0xFFFFFFFF) {
            advance_tail(base, q);
            continue;
        }
        if (r.stwcx(base, first)) break;
    }
    advance_tail(base, q);
}

// An entry back in the bitmap and in F, handed to a waiting pusher when one is left (queue.cpp "freeEntry").
void free_entry(PPUContext& ctx, u32 q, u32 index, u32 table, u32 count)
{
    u8* base = guest_base(ctx);
    if (index == 0xFFFFFFFF) sync2_assert("queue.cpp", 611, "freeEntry");
    if (index >= ps_read32(base, q + qu::kSlots)) sync2_assert("queue.cpp", 612, "freeEntry");
    const u32 at = ps_read32(base, q + qu::kBitmap) + ((index >> 3) & ~7u);
    const u64 bit = kBit63 >> (index & 63);
    Reserve r;
    u64 v;
    do {
        v = r.ldarx(base, at);
        if (!(v & bit)) sync2_assert("queue.cpp", 620, "freeEntry");
    } while (!r.stdcx(base, v & ~bit));
    bool hand_over;
    do {
        v = r.ldarx(base, q + qu::kF);
        v = (((v >> 32) + 1) << 32) | (v & 0xFFFFFFFF);
        hand_over = static_cast<s32>(((v >> 16) & 0xFFFF) - (v & 0xFFFF)) > 0;
        if (hand_over) v = (v & ~0xFFFFull) | ((v + 1) & 0xFFFF);
    } while (!r.stdcx(base, v));
    if (hand_over && wq::signal(ctx, q + qu::kPushWq, table, count) != lv2::CELL_OK) sync2_assert("queue.cpp", 642, "freeEntry");
}

// `n` more elements in P after a push, and up to that many waiting poppers handed one each.
void count_pushed(PPUContext& ctx, u32 q, u32 n, u32 table, u32 count)
{
    u8* base = guest_base(ctx);
    lwsync();
    Reserve r;
    u64 v;
    u32 woken;
    do {
        v = r.ldarx(base, q + qu::kP);
        v = (((v >> 32) + n) << 32) | (v & 0xFFFFFFFF);
        woken = 0;
        const s32 waiting = static_cast<s32>(((v >> 16) & 0xFFFF) - (v & 0xFFFF));
        if (waiting > 0) {
            woken = std::min<u32>(static_cast<u32>(((v >> 16) - v) & 0xFFFF), n);
            v = (v & ~0xFFFFull) | ((v + woken) & 0xFFFF);
        }
    } while (!r.stdcx(base, v));
    for (u32 i = 0; i < (woken & 0xFFFF); i++)
        if (wq::signal(ctx, q + qu::kPopWq, table, count) != lv2::CELL_OK)
            sync2_assert("queue.cpp", n == 1 ? 755 : 1061, n == 1 ? "push" : "multipleTryPush");
}

// The configuration, caller and notifiers of a push or pop.
s32 queue_caller(PPUContext& ctx, std::string_view fn, u32 q, u32 cfg, bool waits, Caller& c, Config& config)
{
    const u16 types = ps_read16(guest_base(ctx), q + qu::kTypes);
    if (const s32 e = read_config(ctx, fn, cfg, types, config, true); e != lv2::CELL_OK) return e;
    if (const s32 e = check_caller(ctx, fn, config.caller, types, waits, c); e != lv2::CELL_OK) return e;
    return check_notifiers(ctx, fn, config.table, config.count, types);
}

s32 cellSync2QueueInitialize(PPUContext& ctx, u32 q, u32 buffer, u32 attr)
{
    constexpr std::string_view fn = "cellSync2QueueInitialize";
    u8* base = guest_base(ctx);
    if (!q || !buffer || !attr) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if ((q & 0x7F) || (buffer & 0x7F)) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, q)) return not_guest(ctx, fn, "queue", q);
    if (!guest_readable(thread_of(ctx), attr, 0x80)) return not_guest(ctx, fn, "attribute", attr);
    if (const s32 e = queue_attr_check(base, attr); e != lv2::CELL_OK) return e;
    const u32 depth = ps_read32(base, attr + 0xC), element = ps_read32(base, attr + 8);
    const u16 max_push = ps_read16(base, attr + 0x10), max_pop = ps_read16(base, attr + 0x12);
    const u64 total = u64{depth + 1} * element + bitmap_bytes(depth) + next_bytes(depth) +
                      (max_push ? max_push * 8u + 8 : 0) + (max_pop ? max_pop * 8u + 8 : 0);
    if (!guest_range(thread_of(ctx), buffer, total)) return not_guest(ctx, fn, "buffer", buffer);
    std::memset(base + q, 0, 0x80);
    ps_write32(base, q + qu::kSdk, ps_read32(base, attr));
    ps_write32(base, q + qu::kElementSize, element);
    ps_write16(base, q + qu::kTypes, static_cast<u16>(ps_read32(base, attr + 4)));
    ps_write32(base, q + qu::kSlots, depth + 1);
    copy_name(base, q + qu::kName, attr + 0x14, 31);
    ps_write32(base, q + qu::kEntries, buffer);
    base[q + 0x7B] = 0;
    const u32 bitmap = buffer + element * (depth + 1);
    ps_write32(base, q + qu::kBitmap, bitmap);
    std::memset(base + bitmap, 0, bitmap_bytes(depth));
    const u32 next = bitmap + bitmap_bytes(depth);
    ps_write32(base, q + qu::kNext, next);
    std::memset(base + next, 0xFF, next_bytes(depth));
    u32 waiters = next + next_bytes(depth);
    if (max_push) {
        if (const s32 e = wq::init(base, q + qu::kPushWq, waiters, max_push * 8u + 8); e != lv2::CELL_OK) return e;
        waiters += max_push * 8u + 8;
    }
    if (max_pop)
        if (const s32 e = wq::init(base, q + qu::kPopWq, waiters, max_pop * 8u + 8); e != lv2::CELL_OK) return e;
    ps_write16(base, q + qu::kMaxPush, max_push);
    ps_write16(base, q + qu::kMaxPop, max_pop);
    ps_write32(base, q + qu::kF, depth + 1);
    u32 dummy = 0;
    if (allocate_entry(ctx, q, dummy, nullptr, false) != lv2::CELL_OK) sync2_assert("queue.cpp", 430, "initialize");
    ps_write32(base, ps_read32(base, q + qu::kNext) + dummy * 4, 0xFFFFFFFF);
    ps_write32(base, q + qu::kHead, dummy);
    ps_write32(base, q + qu::kTail, dummy);
    stored(q, 0x80);
    lwsync();
    return lv2::CELL_OK;
}

s32 cellSync2QueueFinalize(PPUContext& ctx, u32 q)
{
    if (!q) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (q & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, q)) return not_guest(ctx, "cellSync2QueueFinalize", "queue", q);
    u8* base = guest_base(ctx);
    if (ps_read16(base, q + qu::kMaxPush))
        if (const s32 e = wq::idle(base, q + qu::kPushWq); e != lv2::CELL_OK) return e;
    return ps_read16(base, q + qu::kMaxPop) ? wq::idle(base, q + qu::kPopWq) : lv2::CELL_OK;
}

s32 cellSync2QueueGetDepth(PPUContext& ctx, u32 q, u32 depth)
{
    if (!q || !depth) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (q & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, q) || !guest_readable(thread_of(ctx), depth, 4)) return not_guest(ctx, "cellSync2QueueGetDepth", "queue", q);
    ps_write32(guest_base(ctx), depth, ps_read32(guest_base(ctx), q + qu::kSlots) - 1);
    return lv2::CELL_OK;
}

s32 cellSync2QueueGetSize(PPUContext& ctx, u32 q, u32 size)
{
    if (!q || !size) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (q & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, q) || !guest_readable(thread_of(ctx), size, 4)) return not_guest(ctx, "cellSync2QueueGetSize", "queue", q);
    ps_write32(guest_base(ctx), size, ps_read32(guest_base(ctx), q + qu::kP));
    return lv2::CELL_OK;
}

// An element's bytes into an entry, or out of it: elementSize / 4 words.
void copy_words(u8* base, u32 to, u32 from, u32 size)
{
    for (u32 i = 0; i + 4 <= size; i += 4) ps_write32(base, to + i, ps_read32(base, from + i));
}

s32 cellSync2QueuePush(PPUContext& ctx, u32 q, u32 data, u32 cfg)
{
    constexpr std::string_view fn = "cellSync2QueuePush";
    if (!q || !data) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (q & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, q)) return not_guest(ctx, fn, "queue", q);
    u8* base = guest_base(ctx);
    Caller c;
    Config config;
    if (const s32 e = queue_caller(ctx, fn, q, cfg, true, c, config); e != lv2::CELL_OK) return e;
    if (!ps_read16(base, q + qu::kMaxPush)) return error(CELL_SYNC2_ERROR_NOMEM);
    const u32 element = ps_read32(base, q + qu::kElementSize);
    if (!guest_range(thread_of(ctx), data, element)) return not_guest(ctx, fn, "element", data);
    u32 index = 0;
    if (const s32 e = allocate_entry(ctx, q, index, &c, true); e != lv2::CELL_OK) return e;
    ps_write32(base, ps_read32(base, q + qu::kNext) + index * 4, 0xFFFFFFFF);
    copy_words(base, ps_read32(base, q + qu::kEntries) + element * index, data, element);
    link_at_tail(base, q, index);
    count_pushed(ctx, q, 1, config.table, config.count);
    return lv2::CELL_OK;
}

// TryPush: `n` elements (an array of pointers) at once, or none (queue.cpp "multipleTryPush").
s32 cellSync2QueueTryPush(PPUContext& ctx, u32 q, u32 data, u32 n, u32 cfg)
{
    constexpr std::string_view fn = "cellSync2QueueTryPush";
    if (!q || !data) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (q & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!n) return error(CELL_SYNC2_ERROR_INVAL);
    if (!object_ok(ctx, q)) return not_guest(ctx, fn, "queue", q);
    u8* base = guest_base(ctx);
    Caller c;
    Config config;
    if (const s32 e = queue_caller(ctx, fn, q, cfg, false, c, config); e != lv2::CELL_OK) return e;
    const u32 element = ps_read32(base, q + qu::kElementSize);
    if (!guest_range(thread_of(ctx), data, u64{n} * 4)) return not_guest(ctx, fn, "element pointers", data);
    for (u32 i = 0; i < n; i++)
        if (!guest_range(thread_of(ctx), ps_read32(base, data + 4 * i), element)) return not_guest(ctx, fn, "element", data + 4 * i);
    Reserve r;
    u64 v;
    do {
        v = r.ldarx(base, q + qu::kF);
        if ((v >> 32) < ((v >> 16) & 0xFFFF) + u64{n}) return error(CELL_SYNC2_ERROR_AGAIN);
        v = (((v >> 32) - n) << 32) | (v & 0xFFFFFFFF);
    } while (!r.stdcx(base, v));
    u32 at;
    do {
        at = r.lwarx(base, q + qu::kCursor);
    } while (!r.stwcx(base, (n + at) % ps_read32(base, q + qu::kSlots)));
    // Entries from the cursor on, a doubleword of the bitmap at a time: the bits of the run left that lie in it, those
    // not taken set at once and recorded in bit order.
    std::vector<u32> taken;
    taken.reserve(n);
    while (taken.size() < n) {
        const u32 slots = ps_read32(base, q + qu::kSlots), bitmap = ps_read32(base, q + qu::kBitmap);
        const u32 first = at % slots;
        const u32 word_at = bitmap + ((first >> 3) & ~7u);
        const u64 word = r.ldarx(base, word_at);
        u64 mask = 0;
        u32 covered = 0;
        for (u32 left = n - static_cast<u32>(taken.size()); left; left--) {
            const u32 i = (first + covered) % slots;
            if (bitmap + ((i >> 3) & ~7u) != word_at) break;
            mask |= kBit63 >> (i & 63);
            covered++;
        }
        const u64 take = mask & ~word;
        if (!take) {
            at += covered;
            continue;
        }
        if (!r.stdcx(base, word | take)) continue;
        for (u32 b = 0; b < 64; b++)
            if (take & (kBit63 >> b)) taken.push_back((first & ~63u) + b);
    }
    const u32 next = ps_read32(base, q + qu::kNext), entries = ps_read32(base, q + qu::kEntries);
    for (u32 i = 0; i < n; i++) {
        ps_write32(base, next + taken[i] * 4, i + 1 < n ? taken[i + 1] : 0xFFFFFFFF);
        copy_words(base, entries + element * taken[i], ps_read32(base, data + 4 * i), element);
    }
    link_at_tail(base, q, taken[0]);
    count_pushed(ctx, q, n, config.table, config.count);
    return lv2::CELL_OK;
}

// Pop and TryPop: an element of P taken (or, waiting, handed over), then the head's next entry copied out and the head
// moved to it; the old head's entry is freed.
s32 queue_pop(PPUContext& ctx, std::string_view fn, u32 q, u32 out, u32 cfg, bool wait)
{
    if (!q || !out) return error(CELL_SYNC2_ERROR_NULL_POINTER);
    if (q & 0x7F) return error(CELL_SYNC2_ERROR_ALIGN);
    if (!object_ok(ctx, q)) return not_guest(ctx, fn, "queue", q);
    u8* base = guest_base(ctx);
    Caller c;
    Config config;
    if (const s32 e = queue_caller(ctx, fn, q, cfg, wait, c, config); e != lv2::CELL_OK) return e;
    if (wait && !ps_read16(base, q + qu::kMaxPop)) return error(CELL_SYNC2_ERROR_NOMEM);
    const u32 element = ps_read32(base, q + qu::kElementSize);
    if (!guest_range(thread_of(ctx), out, element)) return not_guest(ctx, fn, "element", out);
    Reserve r;
    bool have_receiver = false, joined = false;
    u64 receiver = 0;
    for (;;) {
        u64 v = r.ldarx(base, q + qu::kP);
        const u32 waiters = static_cast<u32>((v >> 16) & 0xFFFF), count = static_cast<u32>(v >> 32);
        joined = false;
        if (waiters < count) {
            v = (u64{count - 1} << 32) | (v & 0xFFFFFFFF);
        } else {
            v = (v & ~(0xFFFFull << 16)) | (u64{(waiters + 1) & 0xFFFF} << 16);
            joined = true;
            if (!wait) return error(CELL_SYNC2_ERROR_AGAIN);
            if (ps_read16(base, q + qu::kMaxPop) == waiters) return error(CELL_SYNC2_ERROR_NOMEM);
            if (!have_receiver) {
                if (const s32 e = allocate_receiver(ctx, c, kForQueue, q, receiver); e != lv2::CELL_OK) return e;
                have_receiver = true;
                continue;
            }
        }
        if (r.stdcx(base, v)) break;
    }
    if (joined) {
        if (wq::wait(ctx, q + qu::kPopWq, c.type, receiver, c.wait, kForQueue, q, c.arg) != lv2::CELL_OK)
            sync2_assert("queue.cpp", 836, "pop");
        u64 v;
        do {
            v = r.ldarx(base, q + qu::kP);
            if (!(v >> 32)) sync2_assert("queue.cpp", 846, "pop");
            if (!((v >> 16) & 0xFFFF)) sync2_assert("queue.cpp", 847, "pop");
            if (!(v & 0xFFFF)) sync2_assert("queue.cpp", 848, "pop");
            v = (((v >> 32) - 1) << 32) | ((((v >> 16) - 1) & 0xFFFF) << 16) | ((v - 1) & 0xFFFF);
        } while (!r.stdcx(base, v));
    }
    if (have_receiver && free_receiver(ctx, c, receiver) != lv2::CELL_OK) sync2_assert("queue.cpp", 858, "pop");
    u32 head;
    for (u32 spins = 0;; spins++) {
        head = r.lwarx(base, q + qu::kHead);
        const u32 next = ps_read32(base, ps_read32(base, q + qu::kNext) + head * 4);
        if (next == 0xFFFFFFFF) { // counted, but its pusher has not linked it yet
            if (spins > 64) std::this_thread::yield();
            continue;
        }
        lwsync();
        if (head == ps_read32(base, q + qu::kTail)) {
            advance_tail(base, q);
            continue;
        }
        copy_words(base, out, ps_read32(base, q + qu::kEntries) + element * next, element);
        if (r.stwcx(base, next)) break;
    }
    free_entry(ctx, q, head, config.table, config.count);
    return lv2::CELL_OK;
}

s32 cellSync2QueuePop(PPUContext& ctx, u32 q, u32 out, u32 cfg) { return queue_pop(ctx, "cellSync2QueuePop", q, out, cfg, true); }
s32 cellSync2QueueTryPop(PPUContext& ctx, u32 q, u32 out, u32 cfg)
{
    return queue_pop(ctx, "cellSync2QueueTryPop", q, out, cfg, false);
}

// --------------------------------------------------------------------------------------------- registration

// libsync2's imports from other libraries, linked to the firmware's modules when pspocket has no implementation.
constexpr HostImport kImports[] = {
    {"cellSync2", "cellFiber", 0x5D9A7034, false},   // cellFiberPpuSelf
    {"cellSync2", "cellFiber", 0x6C164B3B, false},   // cellFiberPpuWaitSignal
    {"cellSync2", "cellFiber", 0x5D3992DD, false},   // cellFiberPpuUtilWorkerControlSendSignal
    {"cellSync2", "cellSpurs", 0xE0A6DBE4, false},   // _cellSpursSendSignal
    {"cellSync2", "cellSpursJq", 0x915455B3, false}, // cellSpursJobQueueSendSignal
};

} // namespace

void register_cellSync2(Registry& r)
{
    r.add_imports(kModule, kImports);
    r.add_variables(kModule, {
        PS_HLE_VAR(0x970C7DF3, gCellSync2CallerThreadTypePpuThread, 0x20, 8, &init_caller_ppu_thread),
        PS_HLE_VAR(0x6DAD635B, gCellSync2CallerThreadTypePpuFiber, 0x20, 8, &init_caller_ppu_fiber),
        PS_HLE_VAR(0xC0845B36, gCellSync2NotifierPpuThread, 0x10, 8, &init_notifier_ppu_thread),
        PS_HLE_VAR(0x23CF1D76, gCellSync2NotifierPpuFiber, 0x10, 8, &init_notifier_ppu_fiber),
        PS_HLE_VAR(0x1DBF2498, gCellSync2NotifierSpursTask, 0x10, 8, &init_notifier_spurs_task),
        PS_HLE_VAR(0xCAC08C35, gCellSync2NotifierSpursJobQueueJob, 0x10, 8, &init_notifier_jq_job),
        ::pspocket::hle::Variable{{}, "_pspocket_cellSync2NotifierTable",
            recomp::nid_of("_pspocket_cellSync2NotifierTable"), 16, 4, &init_notifier_table},
    });
    const auto internal = [](std::string_view name, HleFn fn) {
        return ::pspocket::hle::Function{{}, name, recomp::nid_of(name), fn, true};
    };
    r.add(kModule, {
        PS_HLE_FUNC(0x0080FE88, cellSync2MutexUnlock),
        PS_HLE_FUNC(0x0C2983AC, cellSync2SemaphoreRelease),
        PS_HLE_FUNC(0x0C9A0EA9, cellSync2QueueTryPop),
        PS_HLE_FUNC(0x12F0A27D, cellSync2QueueGetSize),
        PS_HLE_FUNC(0x164843A7, cellSync2SemaphoreFinalize),
        PS_HLE_FUNC(0x27F2D61C, cellSync2MutexFinalize),
        PS_HLE_FUNC(0x2D77FE17, _cellSync2SemaphoreAttributeInitialize),
        PS_HLE_FUNC(0x4E2EE031, cellSync2SemaphoreGetCount),
        PS_HLE_FUNC(0x55836E73, _cellSync2MutexAttributeInitialize),
        PS_HLE_FUNC(0x58BE9A0F, cellSync2CondInitialize),
        PS_HLE_FUNC(0x5B1E4D7A, cellSync2CondEstimateBufferSize),
        PS_HLE_FUNC(0x5E00D433, _cellSync2QueueAttributeInitialize),
        PS_HLE_FUNC(0x5E4B0F87, cellSync2SemaphoreTryAcquire),
        PS_HLE_FUNC(0x63062249, cellSync2CondFinalize),
        PS_HLE_FUNC(0x6AF85CDF, cellSync2QueueFinalize),
        PS_HLE_FUNC(0x74C2780F, cellSync2SemaphoreEstimateBufferSize),
        PS_HLE_FUNC(0x7D967D91, cellSync2QueuePush),
        PS_HLE_FUNC(0x7FD479FE, cellSync2QueueTryPush),
        PS_HLE_FUNC(0x871AF804, cellSync2CondSignal),
        PS_HLE_FUNC(0x8AAE07C2, cellSync2CondSignalAll),
        PS_HLE_FUNC(0xA400D82E, cellSync2MutexLock),
        PS_HLE_FUNC(0xA69C749C, cellSync2MutexTryLock),
        PS_HLE_FUNC(0xBC96D751, cellSync2CondWait),
        PS_HLE_FUNC(0xC08CC0F9, cellSync2QueueEstimateBufferSize),
        PS_HLE_FUNC(0xC5DEE254, cellSync2SemaphoreInitialize),
        PS_HLE_FUNC(0xD1B0D146, cellSync2SemaphoreAcquire),
        PS_HLE_FUNC(0xD51BFAE7, cellSync2MutexEstimateBufferSize),
        PS_HLE_FUNC(0xD83AB0C9, cellSync2QueuePop),
        PS_HLE_FUNC(0xDF3C532A, _cellSync2CondAttributeInitialize),
        PS_HLE_FUNC(0xEB81A467, cellSync2MutexInitialize),
        PS_HLE_FUNC(0xF0E1471C, cellSync2QueueGetDepth),
        PS_HLE_FUNC(0xF125E044, cellSync2QueueInitialize),
        internal("_pspocket_cellSync2PpuThreadSelf", &detail::thunk<&_pspocket_cellSync2PpuThreadSelf>),
        internal("_pspocket_cellSync2PpuThreadWait", &detail::thunk<&_pspocket_cellSync2PpuThreadWait>),
        internal("_pspocket_cellSync2PpuThreadAllocate", &detail::thunk<&_pspocket_cellSync2PpuThreadAllocate>),
        internal("_pspocket_cellSync2PpuThreadFree", &detail::thunk<&_pspocket_cellSync2PpuThreadFree>),
        internal("_pspocket_cellSync2PpuThreadSend", &detail::thunk<&_pspocket_cellSync2PpuThreadSend>),
        internal("_pspocket_cellSync2PpuFiberSelf", &detail::thunk<&_pspocket_cellSync2PpuFiberSelf>),
        internal("_pspocket_cellSync2PpuFiberWait", &detail::thunk<&_pspocket_cellSync2PpuFiberWait>),
        internal("_pspocket_cellSync2PpuFiberAllocate", &detail::thunk<&_pspocket_cellSync2PpuFiberAllocate>),
        internal("_pspocket_cellSync2PpuFiberFree", &detail::thunk<&_pspocket_cellSync2PpuFiberFree>),
        internal("_pspocket_cellSync2PpuFiberSend", &detail::thunk<&_pspocket_cellSync2PpuFiberSend>),
        internal("_pspocket_cellSync2SpursTaskSend", &detail::thunk<&_pspocket_cellSync2SpursTaskSend>),
        internal("_pspocket_cellSync2SpursJqJobSend", &detail::thunk<&_pspocket_cellSync2SpursJqJobSend>),
    });
}

} // namespace pspocket::hle
