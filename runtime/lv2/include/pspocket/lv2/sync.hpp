#pragma once

// lv2 synchronization objects. The syscalls (syscalls.hpp) are thin wrappers that read attributes from guest memory,
// look objects up by ID and call these methods; HLE modules and the SPU runtime may call them directly.
//
// Every blocking method takes the caller's Waiter (PpuThread::waiter for PPU threads) and an lv2 timeout in
// microseconds (0 = wait forever). Results are CELL_OK or a CellError.
//
// Semantics: RPCS3 rpcs3/Emu/Cell/lv2/sys_mutex.cpp, sys_cond.cpp, sys_rwlock.cpp, sys_semaphore.cpp, sys_event.cpp,
// sys_event_flag.cpp, sys_lwmutex.cpp, sys_lwcond.cpp. Deviations are listed next to each class.

#include <deque>
#include <memory>

#include "pspocket/lv2/object.hpp"

namespace pspocket::lv2 {

// sys_mutex. Ownership passes directly to the next waiter on unlock (protocol order). SYS_SYNC_PRIORITY_INHERIT is
// accepted and behaves like SYS_SYNC_PRIORITY (RPCS3 does the same; no priority inheritance).
class Mutex final : public Object {
public:
    static constexpr u32 kIdBase = 0x85000000;
    u32 id_base() const override { return kIdBase; }

    Mutex(u32 protocol, bool recursive, u64 name) : protocol(protocol), recursive(recursive), name(name) {}

    const u32 protocol;
    const bool recursive;
    const u64 name;
    u32 adaptive = SYS_SYNC_NOT_ADAPTIVE; // the attribute's word as given, set at creation (reported, not used)

    // Guarded by lock.
    u32 owner = 0;      // owning thread ID, 0 = free
    u32 lock_count = 0; // recursive re-locks beyond the first
    u32 cond_count = 0; // condition variables bound to this mutex
    WaitQueue waiters;

    Result lock_wait(Waiter& w, u64 timeout_us);
    Result try_lock(Waiter& w);
    Result unlock(Waiter& w);

    // With `lock` held: take the mutex if free, else queue `w`. Returns true if `w` owns it now.
    bool own_or_queue_locked(Waiter& w);
    // With `lock` held, owner releasing: hand the mutex to the next waiter (woken with CELL_OK) or free it.
    void hand_off_locked();
};

// sys_cond. Shares its mutex's lock (as RPCS3 does with cond.mutex->mutex), so moving a waiter from the condition
// queue to the mutex queue is atomic. `dead` and `waiters` are guarded by mutex->lock.
class Cond final : public Object {
public:
    static constexpr u32 kIdBase = 0x86000000;
    u32 id_base() const override { return kIdBase; }

    Cond(std::shared_ptr<Mutex> mutex, u32 mutex_id, u64 name) : mutex(std::move(mutex)), mutex_id(mutex_id), name(name) {}

    const std::shared_ptr<Mutex> mutex;
    const u32 mutex_id;
    const u64 name;
    WaitQueue waiters;

    Result wait(Waiter& w, u64 timeout_us);
    Result signal();
    Result signal_all();
    // CELL_EPERM if `target` is not waiting on this condition variable.
    Result signal_to(const Waiter* target);
};

// sys_rwlock. Writers are preferred: once a writer waits, new readers queue behind it (RPCS3's owner/wait-bit
// encoding, kept here as explicit fields). rlock by the write owner blocks, as in RPCS3.
class RwLock final : public Object {
public:
    static constexpr u32 kIdBase = 0x88000000;
    u32 id_base() const override { return kIdBase; }

    RwLock(u32 protocol, u64 name) : protocol(protocol), name(name) {}

    const u32 protocol;
    const u64 name;

    // Guarded by lock.
    u32 writer = 0;  // write owner thread ID
    u32 readers = 0; // read owners
    WaitQueue read_waiters;
    WaitQueue write_waiters;

    bool busy_locked() const { return writer || readers || !read_waiters.empty() || !write_waiters.empty(); }

    Result rlock(Waiter& w, u64 timeout_us);
    Result try_rlock();
    Result runlock();
    Result wlock(Waiter& w, u64 timeout_us);
    Result try_wlock(Waiter& w);
    Result wunlock(Waiter& w);
};

// sys_semaphore. `value` goes negative by the number of waiters, as in RPCS3.
class Semaphore final : public Object {
public:
    static constexpr u32 kIdBase = 0x96000000;
    u32 id_base() const override { return kIdBase; }

    Semaphore(u32 protocol, s32 max, s32 initial, u64 name) : protocol(protocol), max(max), name(name), value(initial) {}

    const u32 protocol;
    const s32 max;
    const u64 name;

    // Guarded by lock.
    s32 value;
    WaitQueue waiters;

    Result wait(Waiter& w, u64 timeout_us);
    Result try_wait();
    // CELL_EINVAL for count <= 0, CELL_EBUSY (not an error for the guest) when it would exceed max.
    Result post(s32 count);
    s32 get_value();
};

struct Event {
    u64 source = 0;
    u64 data1 = 0;
    u64 data2 = 0;
    u64 data3 = 0;
};

enum : u32 {
    SYS_PPU_QUEUE = 1,
    SYS_SPU_QUEUE = 2,
};
enum : s32 {
    SYS_EVENT_QUEUE_DESTROY_FORCE = 1,
    SYS_EVENT_PORT_LOCAL = 1,
    SYS_EVENT_PORT_IPC = 3,
};

// sys_event_queue. A waiting receiver gets the event in Waiter::data[0..3] (the PPU syscall copies it to r4..r7,
// as lv2 does). SPU-type queues are received by the SPU runtime through receive() with an SPU thread's Waiter.
class EventQueue final : public Object {
public:
    static constexpr u32 kIdBase = 0x8d000000;
    u32 id_base() const override { return kIdBase; }

    EventQueue(u32 protocol, u32 type, u32 size, u64 name, u64 key)
        : protocol(protocol), type(type), size(size), name(name), key(key)
    {
    }

    const u32 protocol;
    const u32 type;
    const u32 size; // 1..127
    const u64 name;
    const u64 key;

    // Guarded by lock.
    std::deque<Event> events;
    std::deque<WakerMark> senders; // beside `events`: the PPU thread that sent each (D-999)
    WaitQueue waiters;

    // CELL_OK, CELL_EBUSY when full (not an error for the guest), CELL_ENOTCONN when destroyed. `sender`: the PPU
    // thread sending (sys_event_port_send), given to the receiver.
    Result send(const Event& event, const WakerMark& sender = {});
    // Blocks until an event arrives. CELL_OK (event in `out`, its sender in `sender`), CELL_ETIMEDOUT, CELL_ECANCELED
    // (destroyed with SYS_EVENT_QUEUE_DESTROY_FORCE), CELL_ESRCH (destroyed before the call).
    Result receive(Waiter& w, u64 timeout_us, Event& out, WakerMark* sender = nullptr);
    // Takes up to `max` queued events without blocking.
    usize try_receive(Event* out, usize max);
    void drain();
    // Marks the queue dead. Without force, CELL_EBUSY while someone waits; with force, waiters get CELL_ECANCELED.
    Result destroy(bool force);
};

// How often a PPU thread a PPU thread's event woke waited for that sender to go on first (D-999, event.cpp), and why
// the wait ended: the sender waited in lv2 (or was gone), began its next call, or 10 ms passed; and how often a thread
// let that sender finish before it destroyed an event queue or created a thread (sender_barrier).
struct SenderDeferrals {
    u64 deferred = 0, by_wait = 0, by_calls = 0, by_limit = 0;
    u64 ns = 0; // time waited in all
    u64 barriers = 0, barrier_limit = 0, barrier_ns = 0;
};
SenderDeferrals sender_deferrals();

class PpuThread;
// Before `t` destroys an event queue or creates a thread: the PPU thread whose event last woke it finishes what it
// does next (D-999, event.cpp).
void sender_barrier(PpuThread& t);

// sys_event_port. Sending through a port to a destroyed queue gives CELL_ENOTCONN.
class EventPort final : public Object {
public:
    static constexpr u32 kIdBase = 0x0e000000;
    u32 id_base() const override { return kIdBase; }

    EventPort(s32 type, u64 name) : type(type), name(name) {}

    const s32 type;
    const u64 name;
    std::shared_ptr<EventQueue> queue; // guarded by lock
};

enum : u32 {
    SYS_SYNC_WAITER_SINGLE = 0x10000,
    SYS_SYNC_WAITER_MULTIPLE = 0x20000,
    SYS_EVENT_FLAG_WAIT_AND = 0x01,
    SYS_EVENT_FLAG_WAIT_OR = 0x02,
    SYS_EVENT_FLAG_WAIT_CLEAR = 0x10,
    SYS_EVENT_FLAG_WAIT_CLEAR_ALL = 0x20,
};

// sys_event_flag. set() satisfies waiters in protocol order against the running pattern, applying each one's clear
// mode before testing the next (RPCS3 sys_event_flag_set).
class EventFlag final : public Object {
public:
    static constexpr u32 kIdBase = 0x98000000;
    u32 id_base() const override { return kIdBase; }

    EventFlag(u32 protocol, u32 type, u64 pattern, u64 name) : protocol(protocol), type(type), name(name), pattern(pattern) {}

    const u32 protocol;
    const u32 type; // SYS_SYNC_WAITER_SINGLE or _MULTIPLE
    const u64 name;

    // Guarded by lock.
    u64 pattern;
    WaitQueue waiters;

    static bool valid_mode(u32 mode);
    // Tests `bitptn` against `pattern` in `mode`; on success applies the clear mode. `result` gets the pattern
    // before clearing.
    static bool check_pattern(u64& pattern, u64 bitptn, u32 mode, u64* result);

    // CELL_OK (pattern before clearing in `result`), CELL_ETIMEDOUT, CELL_ECANCELED (sys_event_flag_cancel),
    // CELL_EPERM (single-waiter flag already has a waiter).
    Result wait(Waiter& w, u64 bitptn, u32 mode, u64 timeout_us, u64& result);
    Result try_wait(u64 bitptn, u32 mode, u64& result);
    void set(u64 bitptn);
    void clear(u64 bitptn); // pattern &= bitptn
    u32 cancel();           // wakes every waiter with CELL_ECANCELED; returns how many
    u64 get();
};

// Kernel side of sys_lwmutex: a sleep queue plus a "signaled" word the user-space part (sysPrxForUser) drives
// (RPCS3 sys_lwmutex.h lv2_lwmutex). Bit 0 = unlocked by _sys_lwmutex_unlock, bit 31 = by _sys_lwmutex_unlock2.
class LwMutex final : public Object {
public:
    static constexpr u32 kIdBase = 0x95000000;
    u32 id_base() const override { return kIdBase; }

    LwMutex(u32 protocol, u32 control, u64 name) : protocol(protocol), control(control), name(name) {}

    const u32 protocol;
    const u32 control; // guest address of the sys_lwmutex_t
    const u64 name;
    // _sys_lwmutex_create's fourth argument without its top four bits, set at creation: sysPrxForUser passes
    // 0x80000001 (bit 31: a name follows). The retail kernel's debugging call describes only kind 1 (D-937).
    u32 kind = 1;

    // Guarded by lock.
    s32 signaled = 0;
    WaitQueue sleepers;
    s32 lwcond_waiters = 0; // threads inside _sys_lwcond_queue_wait for this lwmutex
    bool destroying = false;
    std::condition_variable destroy_cv;

    Result lock_wait(Waiter& w, u64 timeout_us);
    Result try_lock();
    Result unlock(bool unlock2);

    // With `lock` held: consume a pending signal (returns it) or queue `w` (returns 0).
    s32 own_or_queue_locked(Waiter& w);
    // With `lock` held: wake the next sleeper (with CELL_OK, or CELL_EBUSY for unlock2) or record the signal.
    void release_locked(bool unlock2);
};

// Kernel side of sys_lwcond (RPCS3 sys_lwcond.h lv2_lwcond). Lock order: lwcond, then its lwmutex.
class LwCond final : public Object {
public:
    static constexpr u32 kIdBase = 0x97000000;
    u32 id_base() const override { return kIdBase; }

    LwCond(u64 name, u32 lwmutex_id, u32 protocol, u32 control)
        : name(name), lwmutex_id(lwmutex_id), protocol(protocol), control(control)
    {
    }

    const u64 name;
    const u32 lwmutex_id;
    const u32 protocol;
    const u32 control;

    // Guarded by lock.
    WaitQueue waiters;
    s32 lwmutex_waiters = 0;
    bool destroying = false;
    std::condition_variable destroy_cv;

    // _sys_lwcond_queue_wait: CELL_OK (lwmutex handed over), CELL_EBUSY (re-lock it in user space), CELL_ETIMEDOUT.
    Result queue_wait(Waiter& w, LwMutex& mutex, u64 timeout_us);
    // _sys_lwcond_signal: target = nullptr for "any". mode 1: waiter moves to the lwmutex queue; mode 2: woken
    // with CELL_EBUSY; mode 3: lwmutex forcibly handed over. Returns 1 if a waiter was signaled, else 0.
    int signal(LwMutex* mutex, const Waiter* target, u32 mode);
    // _sys_lwcond_signal_all: returns the number of waiters signaled.
    u32 signal_all(LwMutex* mutex, u32 mode);
};

} // namespace pspocket::lv2
