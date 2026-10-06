// SPURS queues (CellSpursQueue), native: the PPU side GTA V imports. RPCS3 has no implementation (UNIMPLEMENTED
// stubs, and a six-argument _cellSpursQueueInitialize), so everything here follows Sony's libsre (fw 4.92, read as the
// reference, D-046; never run, never copied): _cellSpursQueueInitialize 0x164C0, cellSpursQueueDetachLv2EventQueue
// 0x16630, cellSpursQueueAttachLv2EventQueue 0x16778 and cellSpursQueuePopBody 0x16CFC, with cellSpursQueuePushBody
// 0x169D0 and the SPU-side queue library that SPU tasks link statically for the other half of the protocol.
//
// CellSpursQueue is one 128-byte line (128-aligned) shared by the PPU and SPU tasks:
//
//   0x00  s32  pop index: entries taken, modulo 2 * depth; ~index while a pop is under way (see below)
//   0x04  s32  push index, likewise
//   0x08  u32  entry size: a multiple of 16, at most 16 KB
//   0x0C  u32  depth: the number of entries, below 2^30
//   0x10  u64  the entries (depth * size bytes, 16-aligned); entry i of the queue is at index i mod depth
//   0x18  u32  SPU port of the attached lv2 event queue; 0xFFFFFFFF none
//   0x1C  u32  direction: 0 SPU2SPU, 1 SPU2PPU (SPU tasks push, the PPU pops), 2 PPU2SPU
//   0x20  u8   SPU tasks waiting to pop; their task IDs from 0x21, oldest first (15 at most)
//   0x30  u8   SPU tasks waiting to push; their task IDs from 0x31
//   0x40  u8   the count at 0x20 again; the pop waiters' workload IDs from 0x41
//   0x50  u8   the count at 0x30 again; the push waiters' workload IDs from 0x51
//   0x60  u64  taskset (0: a queue of the whole SPURS instance, whose waiters are found through their workload IDs)
//   0x68  u64  CellSpurs
//   0x70  u64  lv2 event queue ID (attach writes it; initialize leaves 0x70..0x7F alone)
//
// The protocol. The indices run modulo 2 * depth, so full (push - pop == depth) and empty (push == pop) differ. A push
// or a pop is two atomic updates of the line with the copy of the entry between them. The first marks the index (stores
// ~index) and so claims the entry: another PPU caller then gets BUSY, an SPU task waits. The second stores index + 1
// (wrapping at 2 * depth), unmarked. Who waits, and how they are woken (the SPU push as GTA V's tasks link it; their
// tasks never pop, and the SPU pop mirrors the push as far as libsre's PushBody and the push library show):
// - An SPU task that finds the queue full, another push under way or older waiters appends its task ID and workload ID
//   to the waiter list and waits for a signal (taskset syscall 2, wait object `queue | 4`). Whoever frees an entry
//   signals the oldest waiter (_cellSpursSendSignal), which then removes itself from the list and takes the entry
//   without checking again: a pop from a full queue (the PPU's here), or an SPU task finishing its push while others
//   wait and the queue is not full, which leaves the push index marked for the waiter it signals. Pushes wake pop
//   waiters the same way (libsre's PushBody: a push into an empty queue).
// - The PPU keeps its index marked for its whole blocking call. When the queue is empty (full) it waits on the lv2
//   event queue the attach made, a one-event queue on a dynamic SPU port of the SPURS instance. An SPU task whose push
//   turns an empty queue non-empty while the pop index is marked and a port is attached throws an event on that port
//   (sys_spu_thread_throw_event(port, 0, 0)); the PPU then looks again. The native scheduler delivers it through the
//   instance's port map (D-040, D-050).
// Everything the SPU side does (GETLLAR/PUTLLC on the line, a DMA of the entry, syscall 2, its own
// _cellSpursSendSignal, the interrupt-mailbox event) is task code that the native taskset policy runs as it is.
//
// Implemented: the four functions GTA V imports, and PushBody (the PPU pushing to PPU2SPU queues) and Size, which Sony's
// libdmuxpamf calls (D-194). Clear, Depth and the getters stay unimplemented until a game calls them (CLAUDE.md rule 4). Where libsre would read a bad pointer,
// these functions refuse the call with TASK_ERROR_FAULT and a log of the call site (D-042); where libsre asserts (it
// prints the failed condition and aborts the process), pspocket stops with a fatal log naming it (D-047).

#include <atomic>
#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "spurs.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

using namespace spurs;

constexpr u32 CELL_SPURS_QUEUE_SIZE = 128, CELL_SPURS_QUEUE_ALIGN = 128;
constexpr u32 CELL_SPURS_QUEUE_SPU2SPU = 0, CELL_SPURS_QUEUE_SPU2PPU = 1, CELL_SPURS_QUEUE_PPU2SPU = 2;
constexpr u32 kMaxEntrySize = 16 * 1024;
constexpr u32 kNoPort = 0xFFFFFFFF;

namespace q_off {
constexpr u32 pop = 0x00, push = 0x04, size = 0x08, depth = 0x0C, buffer = 0x10, spu_port = 0x18, direction = 0x1C,
              pop_waiters = 0x20, push_waiters = 0x30, pop_waiter_wids = 0x40, push_waiter_wids = 0x50,
              taskset = 0x60, spurs = 0x68, event_queue = 0x70;
}

// An index as the line holds it: a negative value is a mark (~index) left by the push or pop under way.
constexpr bool marked(u32 v) { return static_cast<s32>(v) < 0; }
constexpr u32 unmarked(u32 v) { return marked(v) ? ~v : v; }

// Entries in the queue (libsre's arithmetic: the indices run modulo 2 * depth).
constexpr u32 entries(u32 pop, u32 push, u32 depth) { return pop <= push ? push - pop : push - pop + 2 * depth; }

// libsre's queue functions turn a SPURS error from a helper into the task error with the same low byte (CORE BUSY
// 0x8041070A becomes TASK BUSY 0x8041090A) and return lv2 errors unchanged.
s32 queue_error(s32 rc)
{
    const u32 u = static_cast<u32>(rc);
    return ((u >> 16) & 0xFFF) == 0x041 ? error(0x80410900 | (u & 0xFF)) : rc;
}

// OK when [addr, addr + size) is guest memory, else TASK_ERROR_FAULT and a log of the call site.
s32 queue_readable(PPUContext& ctx, std::string_view what, std::string_view name, u32 addr, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return lv2::CELL_OK;
    log_call_site(ctx, t.kernel.base(), std::format("{}: the {} ({:#x}, {} bytes) is not readable guest memory; refused "
        "with CELL_SPURS_TASK_ERROR_FAULT", what, name, addr, size));
    return error(CELL_SPURS_TASK_ERROR_FAULT);
}

// cellSpursGetWorkloadData (libsre 0xA884), which the queue functions use to find the taskset of a waiter of a queue
// that belongs to the whole SPURS instance: the workload's argument (WorkloadInfo.arg).
s32 workload_argument(u8* base, u32 spurs, u32 wid, u32& arg)
{
    if (!spurs) return error(CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN) return error(CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    const u32 count = (ps_read8(base, spurs + spurs_off::flags1) & SF1_32_WORKLOADS) ? CELL_SPURS_MAX_WORKLOAD2
                                                                                      : CELL_SPURS_MAX_WORKLOAD;
    if (wid >= count) return error(CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    if (!(ps_read32(base, spurs + wkl_off::enabled) & (0x80000000u >> wid))) {
        return error(CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    }
    if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    const u32 info = wid < CELL_SPURS_MAX_WORKLOAD ? spurs + wkl_off::info1 + wid * wkl_off::info_stride
                                                   : spurs + 0x1000 + (wid & 15) * wkl_off::info_stride;
    arg = static_cast<u32>(ps_read64(base, info + wkl_off::info_arg));
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- initialize

// _cellSpursQueueInitialize(spurs, taskset, queue, buffer, size, depth, direction), as libsre (0x164C0). The public
// cellSpursQueueInitialize(taskset, ...) passes spurs 0 and cellSpursQueueInitializeIWL(spurs, ...) taskset 0 (GTA V:
// spurs 0, a taskset, 16-byte entries, depth 128, SPU2PPU). Checks in libsre's order: taskset and spurs both NULL, queue
// NULL, buffer NULL with a non-zero size (NULL_POINTER); spurs, taskset, queue (128) and buffer (16) alignment (ALIGN);
// then INVAL: the taskset's workload ID above 31, a size above 16 KB or not a multiple of 16, a depth of 0 or of 2^30 or
// more, a direction above PPU2SPU. With a taskset, the queue's CellSpurs is the taskset's (any spurs passed is only
// checked for alignment). Writes 0x00..0x6F as the layout above shows: indices 0, no port, no waiters.
s32 _cellSpursQueueInitialize(PPUContext& ctx, u32 spurs, u32 taskset, u32 queue, u32 buffer, u32 size, u32 depth,
    u32 direction)
{
    constexpr std::string_view what = "_cellSpursQueueInitialize";
    if (!taskset && !spurs) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (!queue) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (size && !buffer) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (spurs % CELL_SPURS_ALIGN || taskset % 128 || queue % CELL_SPURS_QUEUE_ALIGN || buffer % 16) {
        return error(CELL_SPURS_TASK_ERROR_ALIGN);
    }
    u8* base = guest_base(ctx);
    u64 owner = spurs;
    if (taskset) {
        if (const s32 rc = queue_readable(ctx, what, "taskset", taskset, ts_off::wid + 4); rc != lv2::CELL_OK) return rc;
        if (ps_read32(base, taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) return error(CELL_SPURS_TASK_ERROR_INVAL);
        owner = ps_read64(base, taskset + ts_off::spurs);
    }
    if (size > kMaxEntrySize || size % 16 || !depth || (depth & 0xC0000000) || direction > CELL_SPURS_QUEUE_PPU2SPU) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    if (const s32 rc = queue_readable(ctx, what, "queue", queue, CELL_SPURS_QUEUE_SIZE); rc != lv2::CELL_OK) return rc;

    ps_write32(base, queue + q_off::pop, 0);
    ps_write32(base, queue + q_off::push, 0);
    ps_write32(base, queue + q_off::size, size);
    ps_write32(base, queue + q_off::depth, depth);
    ps_write64(base, queue + q_off::buffer, buffer);
    ps_write32(base, queue + q_off::spu_port, kNoPort);
    ps_write32(base, queue + q_off::direction, direction);
    std::memset(base + queue + q_off::pop_waiters, 0, q_off::taskset - q_off::pop_waiters);
    ps_write64(base, queue + q_off::taskset, taskset);
    ps_write64(base, queue + q_off::spurs, owner);
    PS_LOG_INFO(kChan, "SPURS queue {:#010x} of {} {:#x}: {} entries of {:#x} bytes at {:#x}, {}", queue,
        taskset ? "taskset" : "SPURS", taskset ? taskset : spurs, depth, size, buffer,
        direction == CELL_SPURS_QUEUE_SPU2PPU ? "SPU2PPU" : direction == CELL_SPURS_QUEUE_PPU2SPU ? "PPU2SPU" : "SPU2SPU");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- lv2 event queue

// cellSpursQueueAttachLv2EventQueue(queue), as libsre (0x16778): queue NULL (NULL_POINTER), misaligned (ALIGN), an
// SPU2SPU queue (PERM: the PPU takes no part), a port already attached (STAT), no CellSpurs (INVAL). Then libsre's
// create_lv2_eq (0xB244): a PPU event queue "_spuQue" (SYS_SYNC_FIFO) of one event, connected to the lowest free dynamic
// SPU port of the instance (attach_lv2_eq, as a queue SPURS makes for itself: spuPortBits doesn't show it); when the
// connection fails the queue is destroyed again. The queue ID goes to 0x70, then the port to 0x18. Errors from SPURS
// helpers become task errors (CORE BUSY: no free port), lv2 errors pass unchanged.
s32 cellSpursQueueAttachLv2EventQueue(PPUContext& ctx, u32 queue)
{
    if (!queue) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (queue % CELL_SPURS_QUEUE_ALIGN) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (const s32 rc = queue_readable(ctx, "cellSpursQueueAttachLv2EventQueue", "queue", queue, CELL_SPURS_QUEUE_SIZE);
        rc != lv2::CELL_OK) {
        return rc;
    }
    if (ps_read32(base, queue + q_off::direction) == CELL_SPURS_QUEUE_SPU2SPU) return error(CELL_SPURS_TASK_ERROR_PERM);
    if (ps_read32(base, queue + q_off::spu_port) != kNoPort) return error(CELL_SPURS_TASK_ERROR_STAT);
    const u64 spurs = ps_read64(base, queue + q_off::spurs);
    if (!spurs) return error(CELL_SPURS_TASK_ERROR_INVAL);

    StackVar vars(ctx, base, 24); // queue ID, sys_event_queue_attribute_t {protocol, type, name[8]}
    const u32 queue_out = vars.addr(), attr = vars.addr() + 8;
    ps_write32(base, attr, lv2::SYS_SYNC_FIFO);
    ps_write32(base, attr + 4, lv2::SYS_PPU_QUEUE);
    std::memcpy(base + attr + 8, "_spuQue", 8);
    if (const s32 rc = lv2::sys_event_queue_create(t, queue_out, attr, 0, 1); rc != lv2::CELL_OK) return queue_error(rc);
    const u32 equeue = ps_read32(base, queue_out);
    u8 port = 0;
    if (const s32 rc = attach_lv2_eq(ctx, static_cast<u32>(spurs), equeue, port, true, true); rc != lv2::CELL_OK) {
        lv2::sys_event_queue_destroy(t, equeue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
        return queue_error(rc);
    }
    ps_write64(base, queue + q_off::event_queue, equeue);
    ps_write32(base, queue + q_off::spu_port, port);
    PS_LOG_INFO(kChan, "cellSpursQueueAttachLv2EventQueue: SPURS queue {:#x} -> lv2 queue {:#x}, SPU port {:#x}", queue,
        equeue, port);
    return lv2::CELL_OK;
}

// cellSpursQueueDetachLv2EventQueue(queue), as libsre (0x16630): queue NULL (NULL_POINTER), misaligned (ALIGN), no
// port (STAT), neither taskset nor CellSpurs (INVAL). Then, as one atomic update of the line, BUSY while the PPU's side
// of the queue is marked (a pop of SPU2PPU, a push of PPU2SPU: a PPU caller is under way or waiting), else the port
// becomes 0xFFFFFFFF. Then the SPU port is disconnected (detach_lv2_eq, as a queue SPURS made for itself) and the lv2
// queue destroyed (SYS_EVENT_QUEUE_DESTROY_FORCE); their errors are returned as the attach returns them.
s32 cellSpursQueueDetachLv2EventQueue(PPUContext& ctx, u32 queue)
{
    if (!queue) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (queue % CELL_SPURS_QUEUE_ALIGN) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (const s32 rc = queue_readable(ctx, "cellSpursQueueDetachLv2EventQueue", "queue", queue, CELL_SPURS_QUEUE_SIZE);
        rc != lv2::CELL_OK) {
        return rc;
    }
    if (ps_read32(base, queue + q_off::spu_port) == kNoPort) return error(CELL_SPURS_TASK_ERROR_STAT);
    if (!ps_read64(base, queue + q_off::taskset) && !ps_read64(base, queue + q_off::spurs)) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    const u32 direction = ps_read32(base, queue + q_off::direction);
    const u32 equeue = static_cast<u32>(ps_read64(base, queue + q_off::event_queue));
    u32 port = kNoPort;
    s32 rc = lv2::CELL_OK;
    reservation_op(base, queue, [&](u8* q) {
        rc = lv2::CELL_OK;
        port = ps_read32(q, q_off::spu_port);
        if ((direction == CELL_SPURS_QUEUE_SPU2PPU && marked(ps_read32(q, q_off::pop))) ||
            (direction == CELL_SPURS_QUEUE_PPU2SPU && marked(ps_read32(q, q_off::push)))) {
            rc = error(CELL_SPURS_TASK_ERROR_BUSY);
            return false;
        }
        ps_write32(q, q_off::spu_port, kNoPort);
        return true;
    });
    if (rc != lv2::CELL_OK) return rc;
    rc = detach_lv2_eq(base, static_cast<u32>(ps_read64(base, queue + q_off::spurs)), static_cast<u8>(port), true,
        t.kernel.config().sdk_version);
    if (rc == lv2::CELL_OK) rc = lv2::sys_event_queue_destroy(t, equeue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
    PS_LOG_INFO(kChan, "cellSpursQueueDetachLv2EventQueue: SPURS queue {:#x}, SPU port {:#x}, lv2 queue {:#x}: {:#x}",
        queue, port & 0xFF, equeue, static_cast<u32>(rc));
    return rc == lv2::CELL_OK ? rc : queue_error(rc);
}

// Signals the oldest SPU task waiting on the other side of `queue` (task `waiter` of workload `waiter_wid`), as libsre's
// PopBody (for a task waiting to push) and PushBody (to pop) do once the entry it waits for is there. Its taskset is
// the queue's, or for a queue of the whole instance its workload's: _cellSpursSendSignal's INVAL or STAT become
// TASK_ERROR_FATAL, any other error is a libsre assertion. libsre ignores a failed lookup of a waiter's workload and
// signals whatever its stack holds; pspocket logs it and returns FATAL.
s32 wake_oldest(PPUContext& ctx, std::string_view what, u32 queue, u8 waiter, u8 waiter_wid, std::string_view waits_to)
{
    u8* base = guest_base(ctx);
    u32 taskset = static_cast<u32>(ps_read64(base, queue + q_off::taskset));
    if (!taskset) {
        const u32 spurs = static_cast<u32>(ps_read64(base, queue + q_off::spurs));
        if (const s32 rc = workload_argument(base, spurs, waiter_wid, taskset); rc != lv2::CELL_OK) {
            log_call_site(ctx, base, std::format("{}({:#x}): the taskset of workload {} of SPURS {:#x}, whose task {} "
                "waits to {}, is unknown ({:#x}); refused with CELL_SPURS_TASK_ERROR_FATAL (libsre signals an "
                "undefined taskset here)", what, queue, waiter_wid, spurs, waiter, waits_to, static_cast<u32>(rc)));
            return error(CELL_SPURS_TASK_ERROR_FATAL);
        }
    }
    const s32 rc = send_signal(base, taskset, waiter);
    PS_LOG_DEBUG(kChan, "{}({:#x}): woke task {} of taskset {:#x}, which waits to {}: {:#x}", what, queue, waiter,
        taskset, waits_to, static_cast<u32>(rc));
    if (rc == error(CELL_SPURS_TASK_ERROR_INVAL) || rc == error(CELL_SPURS_TASK_ERROR_STAT)) {
        PS_LOG_ERROR(kChan, "{}({:#x}): signalling task {} of taskset {:#x}, which waits to {}, returned {:#x}: "
            "CELL_SPURS_TASK_ERROR_FATAL, as libsre", what, queue, waiter, taskset, waits_to, static_cast<u32>(rc));
        return error(CELL_SPURS_TASK_ERROR_FATAL);
    }
    if (rc != lv2::CELL_OK) {
        PS_FATAL(kChan, "{}({:#x}): signalling task {} of taskset {:#x} returned {:#x}; libsre (fw 4.92) asserts here "
            "(ret==CELL_OK; queuePop.c:126 for a pop) and aborts the process", what, queue, waiter, taskset,
            static_cast<u32>(rc));
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- pop

// cellSpursQueuePopBody(queue, buffer, isPeek, isBlocking), as libsre (0x16CFC): what cellSpursQueuePop (0, 1),
// TryPop (0, 0), Peek (1, 1) and TryPeek (1, 0) call. Only the low byte of the two flags counts.
// - Checks: queue or buffer NULL (NULL_POINTER), queue misaligned (ALIGN), not SPU2PPU (PERM: only SPU tasks push to
//   the PPU). Then on the line: the pop index marked by another PPU caller (BUSY), a blocking call with no lv2 event
//   queue attached (STAT), a non-blocking call on an empty queue (AGAIN; the line is left as it is).
// - The pop index is marked for the whole call (a blocking call marks it even when the queue is empty, then waits on
//   the lv2 event queue for the SPU push's event and looks again; libsre asserts that the receive succeeds). The entry
//   at pop mod depth is copied out, then the pop index advances (wrapping at 2 * depth; a peek stores it unchanged),
//   unmarked.
// - A pop (not a peek) from a full queue with SPU tasks waiting to push signals the oldest (task ID at 0x31; its taskset
//   is the queue's, or for a queue of the whole instance its workload's at 0x51) through _cellSpursSendSignal: INVAL or
//   STAT from it become TASK_ERROR_FATAL, any other error is a libsre assertion. libsre ignores a failed lookup of a
//   waiter's workload and signals whatever its stack holds; pspocket logs it and returns FATAL.
s32 cellSpursQueuePopBody(PPUContext& ctx, u32 queue, u32 buffer, u32 is_peek, u32 is_blocking)
{
    constexpr std::string_view what = "cellSpursQueuePopBody";
    if (!queue || !buffer) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (queue % CELL_SPURS_QUEUE_ALIGN) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (const s32 rc = queue_readable(ctx, what, "queue", queue, CELL_SPURS_QUEUE_SIZE); rc != lv2::CELL_OK) return rc;
    if (ps_read32(base, queue + q_off::direction) != CELL_SPURS_QUEUE_SPU2PPU) return error(CELL_SPURS_TASK_ERROR_PERM);
    // Size, depth and entries are fixed since initialize. The copy's two pointers are checked where libsre would first
    // use them, before the pop index is marked (so a refused call leaves the queue as it was).
    const u32 size = ps_read32(base, queue + q_off::size), depth = ps_read32(base, queue + q_off::depth);
    const u32 entries_ea = static_cast<u32>(ps_read64(base, queue + q_off::buffer));
    const bool peek = (is_peek & 0xFF) != 0, blocking = (is_blocking & 0xFF) != 0;

    // Step 1: claim the entry at the pop index (mark it), waiting for an SPU push while the queue is empty.
    bool waited = false;
    u32 index = 0, entry = 0;
    for (;;) {
        s32 rc = lv2::CELL_OK;
        bool empty = false;
        reservation_op(base, queue, [&](u8* q) {
            rc = lv2::CELL_OK;
            u32 pop = ps_read32(q, q_off::pop);
            if (!waited && marked(pop)) {
                rc = error(CELL_SPURS_TASK_ERROR_BUSY);
                return false;
            }
            pop = waited ? ~pop : pop; // after a wait the mark is this call's own
            if (blocking && ps_read32(q, q_off::spu_port) == kNoPort) {
                rc = error(CELL_SPURS_TASK_ERROR_STAT);
                return false;
            }
            empty = entries(pop, unmarked(ps_read32(q, q_off::push)), depth) == 0;
            if (!blocking && empty) {
                rc = error(CELL_SPURS_TASK_ERROR_AGAIN);
                return false;
            }
            index = pop;
            entry = entries_ea + (pop < depth ? pop : pop - depth) * size;
            if (!guest_readable(t, entry, size) || !guest_readable(t, buffer, size)) {
                rc = error(CELL_SPURS_TASK_ERROR_FAULT); // logged below, outside the (retried) update
                return false;
            }
            ps_write32(q, q_off::pop, ~pop);
            return true;
        });
        if (rc == error(CELL_SPURS_TASK_ERROR_FAULT)) {
            log_call_site(ctx, base, std::format("{}({:#x}): entry {} ({:#x}) or the output buffer ({:#x}), {} bytes, "
                "is not readable guest memory; refused with CELL_SPURS_TASK_ERROR_FAULT", what, queue, index, entry,
                buffer, size));
        }
        if (rc != lv2::CELL_OK) return rc;
        if (!empty) break;
        const u32 equeue = static_cast<u32>(ps_read64(base, queue + q_off::event_queue));
        PS_LOG_DEBUG(kChan, "{}({:#x}): empty; waiting on lv2 queue {:#x}", what, queue, equeue);
        if (const s32 r = lv2::sys_event_queue_receive(t, equeue, 0, 0); r != lv2::CELL_OK) {
            PS_FATAL(kChan, "{}({:#x}): sys_event_queue_receive on lv2 queue {:#x} returned {:#x}; libsre (fw 4.92) "
                "asserts here (queuePop.c:78: ret==CELL_OK) and aborts the process", what, queue, equeue,
                static_cast<u32>(r));
        }
        waited = true;
    }

    std::memmove(base + buffer, base + entry, size);

    // Step 2: release the claim, the pop index advanced (unless peeking).
    bool wake = false;
    u8 waiter = 0, waiter_wid = 0;
    reservation_op(base, queue, [&](u8* q) {
        const u32 pop = ~ps_read32(q, q_off::pop);
        const u32 count = entries(pop, unmarked(ps_read32(q, q_off::push)), depth);
        wake = ps_read8(q, q_off::push_waiters) != 0 && count == depth;
        waiter = ps_read8(q, q_off::push_waiters + 1);
        waiter_wid = ps_read8(q, q_off::push_waiter_wids + 1);
        ps_write32(q, q_off::pop, peek ? pop : (pop + 1 < 2 * depth ? pop + 1 : 0));
        return true;
    });
    PS_LOG_DEBUG(kChan, "{}({:#x}): entry {} of {}{}", what, queue, index, 2 * depth, peek ? " (peek)" : "");
    if (!wake || peek) return lv2::CELL_OK;

    return wake_oldest(ctx, what, queue, waiter, waiter_wid, "push");
}

// ------------------------------------------------------------------------------------------------- push

// cellSpursQueuePushBody(queue, buffer, isBlocking), as libsre (0x169D0): what cellSpursQueuePush (1) and TryPush (0)
// call; the mirror of PopBody. Only the low byte of the flag counts.
// - Checks: queue or buffer NULL (NULL_POINTER), queue misaligned (ALIGN), not PPU2SPU (PERM: only SPU tasks pop what
//   the PPU pushes). Then on the line: the push index marked by another PPU caller (BUSY), a blocking call with no lv2
//   event queue attached (STAT), a non-blocking call on a full queue (AGAIN; the line is stored back as it was).
// - The push index is marked for the whole call (a blocking call marks it even when the queue is full, then waits on
//   the lv2 event queue for the SPU pop's event and looks again; libsre asserts that the receive succeeds). The buffer
//   is copied to the entry at push mod depth, then the push index advances (wrapping at 2 * depth), unmarked.
// - A push into an empty queue with SPU tasks waiting to pop signals the oldest (task ID at 0x21; its taskset is the
//   queue's, or for a queue of the whole instance its workload's at 0x41), as PopBody does for pushers.
// Sony's libdmuxpamf sends its SPU task commands this way (run of the cellDmux comparison, D-194).
s32 cellSpursQueuePushBody(PPUContext& ctx, u32 queue, u32 buffer, u32 is_blocking)
{
    constexpr std::string_view what = "cellSpursQueuePushBody";
    if (!queue || !buffer) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (queue % CELL_SPURS_QUEUE_ALIGN) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (const s32 rc = queue_readable(ctx, what, "queue", queue, CELL_SPURS_QUEUE_SIZE); rc != lv2::CELL_OK) return rc;
    if (ps_read32(base, queue + q_off::direction) != CELL_SPURS_QUEUE_PPU2SPU) return error(CELL_SPURS_TASK_ERROR_PERM);
    const u32 size = ps_read32(base, queue + q_off::size), depth = ps_read32(base, queue + q_off::depth);
    const u32 entries_ea = static_cast<u32>(ps_read64(base, queue + q_off::buffer));
    const bool blocking = (is_blocking & 0xFF) != 0;

    // Step 1: claim the entry at the push index (mark it), waiting for an SPU pop while the queue is full.
    bool waited = false;
    u32 index = 0, entry = 0;
    for (;;) {
        s32 rc = lv2::CELL_OK;
        bool full = false;
        reservation_op(base, queue, [&](u8* q) {
            rc = lv2::CELL_OK;
            u32 push = ps_read32(q, q_off::push);
            if (!waited && marked(push)) {
                rc = error(CELL_SPURS_TASK_ERROR_BUSY);
                return false;
            }
            push = waited ? ~push : push; // after a wait the mark is this call's own
            if (blocking && ps_read32(q, q_off::spu_port) == kNoPort) {
                rc = error(CELL_SPURS_TASK_ERROR_STAT);
                return false;
            }
            full = entries(unmarked(ps_read32(q, q_off::pop)), push, depth) == depth;
            if (!blocking && full) {
                rc = error(CELL_SPURS_TASK_ERROR_AGAIN);
                return false;
            }
            index = push;
            entry = entries_ea + (push < depth ? push : push - depth) * size;
            if (!guest_readable(t, entry, size) || !guest_readable(t, buffer, size)) {
                rc = error(CELL_SPURS_TASK_ERROR_FAULT); // logged below, outside the (retried) update
                return false;
            }
            ps_write32(q, q_off::push, ~push);
            return true;
        });
        if (rc == error(CELL_SPURS_TASK_ERROR_FAULT)) {
            log_call_site(ctx, base, std::format("{}({:#x}): entry {} ({:#x}) or the input buffer ({:#x}), {} bytes, "
                "is not readable guest memory; refused with CELL_SPURS_TASK_ERROR_FAULT", what, queue, index, entry,
                buffer, size));
        }
        if (rc != lv2::CELL_OK) return rc;
        if (!full) break;
        const u32 equeue = static_cast<u32>(ps_read64(base, queue + q_off::event_queue));
        PS_LOG_DEBUG(kChan, "{}({:#x}): full; waiting on lv2 queue {:#x}", what, queue, equeue);
        if (const s32 r = lv2::sys_event_queue_receive(t, equeue, 0, 0); r != lv2::CELL_OK) {
            PS_FATAL(kChan, "{}({:#x}): sys_event_queue_receive on lv2 queue {:#x} returned {:#x}; libsre (fw 4.92) "
                "asserts here (ret==CELL_OK) and aborts the process", what, queue, equeue, static_cast<u32>(r));
        }
        waited = true;
    }

    std::memmove(base + entry, base + buffer, size);

    // Step 2: release the claim, the push index advanced.
    bool wake = false;
    u8 waiter = 0, waiter_wid = 0;
    reservation_op(base, queue, [&](u8* q) {
        const u32 push = ~ps_read32(q, q_off::push);
        const u32 count = entries(unmarked(ps_read32(q, q_off::pop)), push, depth);
        wake = ps_read8(q, q_off::pop_waiters) != 0 && count == 0;
        waiter = ps_read8(q, q_off::pop_waiters + 1);
        waiter_wid = ps_read8(q, q_off::pop_waiter_wids + 1);
        ps_write32(q, q_off::push, push + 1 < 2 * depth ? push + 1 : 0);
        return true;
    });
    PS_LOG_DEBUG(kChan, "{}({:#x}): entry {} of {}", what, queue, index, 2 * depth);
    if (!wake) return lv2::CELL_OK;
    return wake_oldest(ctx, what, queue, waiter, waiter_wid, "pop");
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- size

// cellSpursQueueSize(queue, size), as libsre (0x168FC): queue or size NULL (NULL_POINTER), queue misaligned (ALIGN);
// then the pop and push indices read together (one doubleword), marks removed, and the entries between them stored.
// Any direction. Sony's libdmuxpamf counts its SPU task's events with it (D-194).
s32 cellSpursQueueSize(PPUContext& ctx, u32 queue, u32 size_out)
{
    constexpr std::string_view what = "cellSpursQueueSize";
    if (!queue || !size_out) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (queue % CELL_SPURS_QUEUE_ALIGN) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (const s32 rc = queue_readable(ctx, what, "queue", queue, CELL_SPURS_QUEUE_SIZE); rc != lv2::CELL_OK) return rc;
    if (const s32 rc = queue_readable(ctx, what, "size", size_out, 4); rc != lv2::CELL_OK) return rc;
    u8* base = guest_base(ctx);
    const u64 indices = std::atomic_ref<u64>(*reinterpret_cast<u64*>(base + queue)).load(std::memory_order_acquire);
    const u32 pop = unmarked(ps_bswap32(static_cast<u32>(indices))), push = unmarked(ps_bswap32(static_cast<u32>(indices >> 32)));
    ps_write32(base, size_out, entries(pop, push, ps_read32(base, queue + q_off::depth)));
    return lv2::CELL_OK;
}

} // namespace

void register_cellSpursQueue(Registry& r)
{
    r.add("cellSpurs", {
        PS_HLE_FUNC(0x082BFB09, _cellSpursQueueInitialize),
        PS_HLE_FUNC(0xE5443BE7, cellSpursQueueAttachLv2EventQueue),
        PS_HLE_FUNC(0x039D70B7, cellSpursQueueDetachLv2EventQueue),
        PS_HLE_FUNC(0x91066667, cellSpursQueuePopBody),
        PS_HLE_FUNC(0x92CFF6ED, cellSpursQueuePushBody),
        PS_HLE_FUNC(0x54876603, cellSpursQueueSize),
    });
}

} // namespace pspocket::hle
