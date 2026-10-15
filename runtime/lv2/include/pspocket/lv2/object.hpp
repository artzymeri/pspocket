#pragma once

// lv2 kernel objects, their IDs, and how threads block on them.
//
// - Object: base of every kernel object. `lock` protects its state and wait queues; `dead` is set (under `lock`)
//   when its last ID is destroyed, so an operation that looked the object up just before the destroy fails with
//   ESRCH instead of using a destroyed object. Lookups never happen while an object lock is held.
// - IdTable: RPCS3's IdManager (Emu/IdManager.h, IdManager.cpp idm::allocate_id): ID = type base + index * step,
//   plus an invalidation counter that changes when a slot is reused. Slots are handed out in increasing order until
//   the table is full; only then are freed slots reused (with the counter bumped), so a stale ID keeps failing for a
//   long time. Several types can share one index space (lv2_obj's 8192 slots).
// - Waiter: a thread's parking spot. Objects queue Waiter pointers in WaitQueues. A waker removes the waiter from
//   the queue and calls wake() while holding the lock of that object, so a waiter that times out can tell, under
//   the same lock, whether it is still queued (it removes itself and times out) or was already woken.
//   While it waits, a Waiter also says what it waits on (WaitKind and object ID), for thread reports (report.hpp).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::lv2 {

// sys_sync.h (RPCS3): protocol, recursion, sharing and creation attributes.
enum : u32 {
    SYS_SYNC_FIFO = 0x1,
    SYS_SYNC_PRIORITY = 0x2,
    SYS_SYNC_PRIORITY_INHERIT = 0x3,
    SYS_SYNC_RETRY = 0x4,
    SYS_SYNC_ATTR_PROTOCOL_MASK = 0xF,
    SYS_SYNC_RECURSIVE = 0x10,
    SYS_SYNC_NOT_RECURSIVE = 0x20,
    SYS_SYNC_PROCESS_SHARED = 0x100,
    SYS_SYNC_NOT_PROCESS_SHARED = 0x200,
    SYS_SYNC_ADAPTIVE = 0x1000,
    SYS_SYNC_NOT_ADAPTIVE = 0x2000,
};
enum : s32 {
    SYS_SYNC_NEWLY_CREATED = 0x1,
    SYS_SYNC_NOT_CREATE = 0x2,
    SYS_SYNC_NOT_CARE = 0x3,
};

class Object {
public:
    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    virtual ~Object() = default;

    // Type tag: the ID base of the object's type (0x85000000 for mutexes, ...).
    virtual u32 id_base() const = 0;

    std::mutex lock;
    bool dead = false; // guarded by lock
    u32 id = 0;        // first ID the object was registered under
    u64 ipc_key = 0;   // process-shared objects (lv2_obj::create)
    u32 refs = 0;      // IDs referring to this object (more than one only through an IPC key); guarded by lock
};

struct IdSpec {
    u32 step;
    u32 count;
    u32 invl_shift; // position and width of the invalidation counter (RPCS3 id_invl_range)
    u32 invl_bits;
    bool lowest;    // always hand out the lowest free ID (file descriptors)
};

class IdTable {
public:
    explicit IdTable(IdSpec spec);

    // Registers `obj` under a new ID of the type with ID base `base`. Returns 0 when the table is full.
    u32 insert(u32 base, std::shared_ptr<Object> obj);
    std::shared_ptr<Object> find(u32 base, u32 id) const;
    bool remove(u32 base, u32 id);
    u32 count(u32 base) const;
    std::vector<std::pair<u32, std::shared_ptr<Object>>> list(u32 base) const;
    std::vector<std::shared_ptr<Object>> all() const;

    template <class T>
    std::shared_ptr<T> get(u32 id) const
    {
        return std::static_pointer_cast<T>(find(T::kIdBase, id));
    }

private:
    struct Slot {
        std::shared_ptr<Object> obj;
        u32 base = 0;
        u32 key = 0; // the ID last handed out for this slot (keeps the invalidation counter)
        bool used = false;
    };
    u32 index_of(u32 base, u32 id) const;

    IdSpec spec_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    u32 highest_ = 0;
};

// ID layouts (RPCS3 id_base / id_step / id_count / id_invl_range).
inline constexpr IdSpec kLv2ObjIds{0x100, 8192, 0, 8, false};    // lv2_obj: mutex, cond, queues, timers, ...
inline constexpr IdSpec kPpuThreadIds{1, 100, 12, 12, false};      // ppu_thread, base 0x01000000
inline constexpr IdSpec kMemContainerIds{1, 16, 0, 0, false};      // lv2_memory_container, base 0x3F000000
inline constexpr IdSpec kFsIds{1, 255 - 3, 0, 0, true};            // lv2_fs_object, base 3

// What a waiter is blocked on (Waiter::waiting_on), for thread reports.
enum class WaitKind : u8 {
    None,
    Mutex,
    Cond,        // also once woken (signalled, timed out), while the waiter re-takes the condition variable's mutex
    RwLockRead,
    RwLockWrite,
    Semaphore,
    EventQueue,
    EventFlag,
    LwMutex,     // the kernel side of sys_lwmutex (its sleep queue)
    LwCond,      // the kernel side of sys_lwcond, also once signalled onto its lwmutex's sleep queue
    Join,        // sys_ppu_thread_join; the ID is the joined thread's
    Sleep,       // sys_timer_usleep / sys_timer_sleep; no ID
    Usbd,        // sys_usbd_receive_event; the ID is the usbd handle
    Net,         // a network call (sys_net_bnet_recvfrom, accept, select, poll...); no ID
};
const char* wait_kind_name(WaitKind kind);

struct WaitOn {
    WaitKind kind = WaitKind::None;
    u32 id = 0; // the object's lv2 ID
};

// The PPU thread whose call sent an event (D-999), so the thread that receives it can let that thread go on first, as
// lv2's scheduler does on the PS3 (defer_to_sender, event.cpp): which thread, its priority, its lv2_calls when it
// sent, its host thread (for that thread's CPU time) and when.
struct WakerMark {
    u32 thread = 0; // lv2 thread ID; 0: none
    s32 prio = 0;
    u64 calls = 0;
    u64 host_thread = 0;
    std::chrono::steady_clock::time_point at{};
};

struct Waiter {
    u32 id = 0;                // lv2 thread ID (the owner value locks record)
    std::atomic<s32> prio{0};  // lv2 priority: lower is more urgent
    u64 order = 0;             // queue order, set by WaitQueue::push

    std::mutex park_mutex;
    std::condition_variable park_cv;
    bool signaled = false; // guarded by park_mutex
    bool aborted = false;  // guarded by park_mutex: the kernel is tearing the thread down

    // Written by the waker before wake(), read by the waiter after it returns.
    Result result = CELL_OK;
    u64 data[4]{};
    WakerMark waker; // an event queue's: the PPU thread that sent the event (D-999)
    // What the waiter waits for (event flag: bit pattern and mode), written before it queues.
    u64 want[2]{};

    // What the waiter is blocked on, for reports. park() and sleep_us() set it on the waiting thread for the length
    // of the wait (before the object lock is released, with release order, so a reader that sees it also sees the
    // thread's registers as they were at the call) and clear it on return. Any thread may read it.
    void begin_wait(WaitOn on, std::chrono::steady_clock::time_point since, std::chrono::steady_clock::time_point until)
    {
        wait_since_.store(since.time_since_epoch().count(), std::memory_order_relaxed);
        wait_until_.store(until.time_since_epoch().count(), std::memory_order_relaxed);
        wait_on_.store(u64{static_cast<u8>(on.kind)} << 32 | on.id, std::memory_order_release);
    }
    void end_wait() { wait_on_.store(0, std::memory_order_relaxed); }
    void clear_wait_deadline() { wait_until_.store(0, std::memory_order_relaxed); }
    WaitOn waiting_on() const
    {
        const u64 v = wait_on_.load(std::memory_order_acquire);
        return {static_cast<WaitKind>(v >> 32), static_cast<u32>(v)};
    }
    // When the current wait began and when it times out (time_point{} for no timeout).
    std::chrono::steady_clock::time_point wait_since() const
    {
        using Clock = std::chrono::steady_clock;
        return Clock::time_point(Clock::duration(wait_since_.load(std::memory_order_relaxed)));
    }
    std::chrono::steady_clock::time_point wait_until() const
    {
        using Clock = std::chrono::steady_clock;
        return Clock::time_point(Clock::duration(wait_until_.load(std::memory_order_relaxed)));
    }

private:
    std::atomic<u64> wait_on_{0}; // WaitKind << 32 | ID; 0 while not waiting
    std::atomic<std::chrono::steady_clock::rep> wait_since_{0};
    std::atomic<std::chrono::steady_clock::rep> wait_until_{0};
};

class WaitQueue {
public:
    void push(Waiter* w);
    // Next waiter by protocol: SYS_SYNC_PRIORITY takes the lowest priority value, earliest first among equals;
    // anything else is FIFO. Returns nullptr when empty.
    Waiter* pop(u32 protocol);
    Waiter* peek(u32 protocol) const;
    bool remove(const Waiter* w);
    bool contains(const Waiter* w) const;
    bool empty() const { return items_.empty(); }
    usize size() const { return items_.size(); }
    // Removes and returns every waiter, in the order pop(protocol) would have returned them.
    std::vector<Waiter*> take_all(u32 protocol);
    // Every waiter in the order pop(protocol) would return them, the queue left as it is (the debugging calls' lists).
    std::vector<Waiter*> ordered(u32 protocol) const;
    const std::vector<Waiter*>& items() const { return items_; }

private:
    usize select(u32 protocol) const;
    std::vector<Waiter*> items_;
};

// Clears the waiter's previous wake-up; call before queueing it.
void prepare_wait(Waiter& w);

// Hands a result to a dequeued waiter and wakes it. Call while holding the lock of the object it was queued on.
void wake(Waiter& w, Result result);

// Marks the waiter aborted (kernel shutdown / process exit) and wakes it if parked.
void abort_wait(Waiter& w);
bool wait_aborted(Waiter& w);

enum class WaitStatus { Woken, TimedOut, Aborted };
enum class CancelResult {
    Removed,  // the waiter was still queued and is now removed: the wait ends (timed out or aborted)
    Woken,    // a waker already took it: the wake is complete
    Requeued, // moved to another queue (a condition variable waiter now waits for its mutex): wait on, no timeout
};

// Deadline for an lv2 timeout in microseconds (0 = none; huge values are clamped to ~30 years).
std::chrono::steady_clock::time_point deadline_after_us(u64 timeout_us);
std::chrono::steady_clock::time_point deadline_after_us(std::chrono::steady_clock::time_point now, u64 timeout_us);

// Blocks a queued waiter. `lk` holds the object lock on entry and is released while waiting; it is not held on
// return. On timeout or abort, `cancel(aborting)` runs with `lk` re-taken and says where the waiter is. `on` names
// the object for reports (Waiter::waiting_on) while the waiter is parked.
template <class Cancel>
WaitStatus park(Waiter& w, std::unique_lock<std::mutex>& lk, u64 timeout_us, WaitOn on, Cancel&& cancel)
{
    bool timed = timeout_us != 0;
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = deadline_after_us(now, timeout_us);
    w.begin_wait(on, now, timed ? deadline : std::chrono::steady_clock::time_point{});
    const struct EndWait {
        Waiter& waiter;
        ~EndWait() { waiter.end_wait(); }
    } end_wait{w};
    lk.unlock();
    for (;;) {
        bool aborting = false;
        {
            std::unique_lock pl(w.park_mutex);
            const auto ready = [&] { return w.signaled || w.aborted; };
            if (timed) {
                w.park_cv.wait_until(pl, deadline, ready);
            } else {
                w.park_cv.wait(pl, ready);
            }
            if (w.signaled) return WaitStatus::Woken;
            aborting = w.aborted;
        }
        lk.lock();
        const CancelResult where = cancel(aborting);
        lk.unlock();
        switch (where) {
        case CancelResult::Removed: return aborting ? WaitStatus::Aborted : WaitStatus::TimedOut;
        case CancelResult::Woken: {
            std::unique_lock pl(w.park_mutex);
            w.park_cv.wait(pl, [&] { return w.signaled; });
            return WaitStatus::Woken;
        }
        case CancelResult::Requeued:
            timed = false;
            w.clear_wait_deadline();
            break;
        }
    }
}

// Sleeps for `us` microseconds unless the wait is aborted. Returns false when aborted.
bool sleep_us(Waiter& w, u64 us);

class PpuThread;
// A yield (usleep(0), sys_ppu_thread_yield). The PS3 yields to threads of the same priority, or returns at once and
// the loop around it spins; here a spinning thread takes a core the others need (GTA V's render thread waits with
// usleep(0): a sixth of a core, run 075). After 16 yields less than 50 us apart it sleeps 20 us, doubling up to
// 200 us, until it goes longer than that between two yields. False when the thread is to exit.
bool yield_wait(PpuThread& t);
// yield_wait's rule on its own: the microseconds the yield at `now_ns` (steady clock) sleeps, 0 for a plain yield,
// with the thread's streak and backoff brought up to date.
u32 yield_backoff(PpuThread& t, u64 now_ns);

} // namespace pspocket::lv2
