// lv2's debugging calls as the retail kernel has them (D-937): the information calls on each kind of lock and queue,
// with threads waiting on them, and the PPU exception handler's registration.

#include <cstring>
#include <thread>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

enum : u32 {
    kDbgMutexInfo = 933,
    kDbgCondInfo = 935,
    kDbgRwLockInfo = 937,
    kDbgLwMutexInfo = 941,
    kDbgQueueInfo = 943,
    kDbgInitExceptionHandler = 944,
    kDbgFinalizeExceptionHandler = 945,
    kDbgSemaphoreInfo = 947,
    kDbgLwCondInfo = 951,
    kDbgFlagInfo = 971,
};

constexpr s32 kDeNoMem = static_cast<s32>(0x80010408);
constexpr s32 kDeInvalidArguments = static_cast<s32>(0x80010409);
constexpr s32 kDeHandlerAlreadyRegistered = static_cast<s32>(0x80010429);
constexpr s32 kDeInvalidHandler = static_cast<s32>(0x8001042A);
constexpr s32 kDeHandleNotRegistered = static_cast<s32>(0x8001042B);

constexpr u64 kEight = 0x6162636465666768ull; // "abcdefgh": the kernel keeps "abcdefg" and a NUL
constexpr u64 kKept = 0x6162636465666700ull;

// An information structure over 0xEE, so the words the call leaves alone show.
u32 info_block(Fixture& f, u32 size)
{
    const u32 a = f.alloc(size, 8);
    std::memset(f.base() + a, 0xEE, size);
    return a;
}

bool untouched(Fixture& f, u32 a, u32 size)
{
    for (u32 i = 0; i < size; i++) {
        if (f.base()[a + i] != 0xEE) return false;
    }
    return true;
}

} // namespace

TEST_CASE("lv2 sys_dbg: a mutex's information as the retail kernel gives it", "[lv2][dbg]")
{
    Fixture f;
    Caller c(*f.kernel, "owner");
    const u32 attr = mutex_attr(f, SYS_SYNC_PRIORITY, SYS_SYNC_RECURSIVE);
    f.w64(attr + 32, kEight);
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kMutexCreate, out, attr) == CELL_OK);
    const u32 m = f.r32(out);
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK);
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK); // a re-lock
    REQUIRE(c.call(kCondCreate, f.alloc(4), m, cond_attr(f)) == CELL_OK);

    // Two waiters, the less urgent first: the list gives the queue's order, by priority.
    u32 ids[2]{};
    std::vector<std::thread> pool;
    const s32 prios[] = {2000, 100};
    for (int i = 0; i < 2; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter", prios[i]);
            ids[i] = me.id();
            THREAD_REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
            THREAD_REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<Mutex>(*f.kernel, m) == static_cast<usize>(i + 1); }));
    }

    const u32 info = info_block(f, 0x48);
    const u32 list = f.alloc(32, 8);
    f.w32(info + 0x3C, list);
    f.w32(info + 0x40, 4);
    REQUIRE(c.call(kDbgMutexInfo, 1, m, info) == CELL_OK);
    CHECK(f.r32(info) == SYS_SYNC_PRIORITY);
    CHECK(f.r32(info + 0x04) == SYS_SYNC_RECURSIVE);
    CHECK(f.r32(info + 0x08) == SYS_SYNC_NOT_PROCESS_SHARED);
    CHECK(f.r32(info + 0x0C) == SYS_SYNC_NOT_ADAPTIVE);
    CHECK(f.r64(info + 0x10) == 0);                     // no IPC key
    CHECK(f.r64(info + 0x18) == 0xEEEEEEEEEEEEEEEEull); // the attribute's flags and padding: the caller's
    CHECK(f.r64(info + 0x20) == kKept);
    CHECK(f.r64(info + 0x28) == c.id());
    CHECK(f.r32(info + 0x30) == 1); // re-locks beyond the first
    CHECK(f.r32(info + 0x34) == 1); // one condition variable
    CHECK(f.r32(info + 0x38) == 0);
    CHECK(f.r32(info + 0x3C) == list);
    CHECK(f.r32(info + 0x40) == 2);
    CHECK(f.r32(info + 0x44) == 2);
    CHECK(f.r64(list) == ids[1]);
    CHECK(f.r64(list + 8) == ids[0]);
    CHECK(f.r64(list + 16) == 0); // as allocated: nothing past the count

    // Room for one: the first; no list: a count of 0; the total either way.
    f.w32(info + 0x40, 1);
    f.w64(list, 0);
    REQUIRE(c.call(kDbgMutexInfo, 1, m, info) == CELL_OK);
    CHECK(f.r32(info + 0x40) == 1);
    CHECK(f.r32(info + 0x44) == 2);
    CHECK(f.r64(list) == ids[1]);
    f.w32(info + 0x3C, 0);
    f.w32(info + 0x40, 4);
    REQUIRE(c.call(kDbgMutexInfo, 1, m, info) == CELL_OK);
    CHECK(f.r32(info + 0x40) == 0);
    CHECK(f.r32(info + 0x44) == 2);

    // Another process, no such mutex, a structure or a list it cannot write (the structure then left alone), and a
    // list longer than the kernel's largest heap piece.
    CHECK(c.call(kDbgMutexInfo, 2, m, info) == kDeInvalidArguments);
    CHECK(c.call(kDbgMutexInfo, 1, 0x85FFFF00, info) == rc(CELL_ESRCH));
    CHECK(c.call(kDbgMutexInfo, 1, m, 0) == rc(CELL_EFAULT));
    const u32 fresh = info_block(f, 0x48);
    f.w32(fresh + 0x3C, 0x10);
    f.w32(fresh + 0x40, 2);
    CHECK(c.call(kDbgMutexInfo, 1, m, fresh) == rc(CELL_EFAULT));
    CHECK(f.r32(fresh) == 0xEEEEEEEE);
    f.w32(fresh + 0x3C, list);
    f.w32(fresh + 0x40, 0x4001);
    CHECK(c.call(kDbgMutexInfo, 1, m, fresh) == kDeNoMem);
    f.w32(fresh + 0x40, 0x4000);
    CHECK(c.call(kDbgMutexInfo, 1, m, fresh) == CELL_OK);

    REQUIRE(c.call(kMutexUnlock, m) == CELL_OK);
    REQUIRE(c.call(kMutexUnlock, m) == CELL_OK);
    for (auto& t : pool) t.join();
}

TEST_CASE("lv2 sys_dbg: a condition variable's and an rwlock's information", "[lv2][dbg]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == CELL_OK);
    const u32 m = f.r32(out);
    const u32 cattr = cond_attr(f);
    f.w64(cattr + 16, kEight);
    REQUIRE(c.call(kCondCreate, out, m, cattr) == CELL_OK);
    const u32 cv = f.r32(out);

    u32 ids[2]{};
    std::vector<std::thread> pool;
    for (int i = 0; i < 2; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter", 2000 - 1000 * i);
            ids[i] = me.id();
            THREAD_REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
            THREAD_REQUIRE(me.call(kCondWait, cv, 0) == CELL_OK);
            THREAD_REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<Cond>(*f.kernel, cv) == static_cast<usize>(i + 1); }));
    }
    const u32 info = info_block(f, 0x28);
    const u32 list = f.alloc(16, 8);
    f.w32(info + 0x1C, list);
    f.w32(info + 0x20, 2);
    REQUIRE(c.call(kDbgCondInfo, 1, cv, info) == CELL_OK);
    CHECK(f.r32(info) == SYS_SYNC_NOT_PROCESS_SHARED);
    CHECK(f.r32(info + 0x04) == 0xEEEEEEEE); // the attribute's flags: the caller's
    CHECK(f.r64(info + 0x08) == 0);
    CHECK(f.r64(info + 0x10) == kKept);
    CHECK(f.r32(info + 0x18) == m);
    CHECK(f.r32(info + 0x20) == 2);
    CHECK(f.r32(info + 0x24) == 2);
    CHECK(f.r64(list) == ids[0]); // the mutex's protocol, FIFO: arrival order
    CHECK(f.r64(list + 8) == ids[1]);
    REQUIRE(c.call(kCondSignalAll, cv) == CELL_OK);
    for (auto& t : pool) t.join();
    pool.clear();

    // An rwlock written by this thread: readers wait in arrival order whatever the protocol, writers by priority.
    const u32 rattr = sync_attr(f, SYS_SYNC_PRIORITY);
    f.w64(rattr + 24, kEight);
    REQUIRE(c.call(kRwCreate, out, rattr) == CELL_OK);
    const u32 rw = f.r32(out);
    REQUIRE(c.call(kRwWLock, rw, 0) == CELL_OK);
    u32 readers[2]{}, writers[2]{};
    for (int i = 0; i < 4; i++) {
        const bool reader = i < 2;
        pool.emplace_back([&, i, reader] {
            Caller me(*f.kernel, reader ? "reader" : "writer", 2000 - 1000 * (i % 2));
            (reader ? readers : writers)[i % 2] = me.id();
            if (reader) {
                THREAD_REQUIRE(me.call(kRwRLock, rw, 0) == CELL_OK);
                THREAD_REQUIRE(me.call(kRwRUnlock, rw) == CELL_OK);
            } else {
                THREAD_REQUIRE(me.call(kRwWLock, rw, 0) == CELL_OK);
                THREAD_REQUIRE(me.call(kRwWUnlock, rw) == CELL_OK);
            }
        });
        REQUIRE(eventually([&] { return queued<RwLock>(*f.kernel, rw) == static_cast<usize>(i + 1); }));
    }
    const u32 rinfo = info_block(f, 0x40);
    const u32 rlist = f.alloc(16, 8), wlist = f.alloc(16, 8);
    f.w32(rinfo + 0x28, rlist);
    f.w32(rinfo + 0x2C, 2);
    f.w32(rinfo + 0x34, wlist);
    f.w32(rinfo + 0x38, 2);
    REQUIRE(c.call(kDbgRwLockInfo, 1, rw, rinfo) == CELL_OK);
    CHECK(f.r32(rinfo) == SYS_SYNC_PRIORITY);
    CHECK(f.r32(rinfo + 0x04) == SYS_SYNC_NOT_PROCESS_SHARED);
    CHECK(f.r64(rinfo + 0x08) == 0);
    CHECK(f.r64(rinfo + 0x10) == 0xEEEEEEEEEEEEEEEEull);
    CHECK(f.r64(rinfo + 0x18) == kKept);
    CHECK(f.r64(rinfo + 0x20) == c.id());
    CHECK(f.r32(rinfo + 0x2C) == 2);
    CHECK(f.r32(rinfo + 0x30) == 2);
    CHECK(f.r32(rinfo + 0x38) == 2);
    CHECK(f.r32(rinfo + 0x3C) == 2);
    CHECK(f.r64(rlist) == readers[0]);
    CHECK(f.r64(rlist + 8) == readers[1]);
    CHECK(f.r64(wlist) == writers[1]);
    CHECK(f.r64(wlist + 8) == writers[0]);
    REQUIRE(c.call(kRwWUnlock, rw) == CELL_OK);
    for (auto& t : pool) t.join();
}

TEST_CASE("lv2 sys_dbg: an lwmutex's and an lwcond's information, read from the process's structures too", "[lv2][dbg]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    // sys_lwmutex_t: lock_var (owner, waiter), attribute, recursive_count, sleep_queue, pad.
    const u32 control = f.alloc(24, 8);
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kLwMutexCreate, out, SYS_SYNC_PRIORITY, control, 0x80000001, kEight) == CELL_OK);
    const u32 lw = f.r32(out);
    f.w32(control, c.id());
    f.w32(control + 4, 1);
    f.w32(control + 8, SYS_SYNC_RECURSIVE | SYS_SYNC_PRIORITY);
    f.w32(control + 12, 3);
    f.w32(control + 16, lw);

    u32 sleeper = 0;
    std::thread th([&] {
        Caller me(*f.kernel, "sleeper");
        sleeper = me.id();
        THREAD_REQUIRE(me.call(kLwMutexLock, lw, 0) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<LwMutex>(*f.kernel, lw) == 1; }));
    const u32 info = info_block(f, 0x28);
    const u32 list = f.alloc(8, 8);
    f.w32(info + 0x1C, list);
    f.w32(info + 0x20, 4);
    REQUIRE(c.call(kDbgLwMutexInfo, 1, lw, info) == CELL_OK);
    CHECK(f.r32(info) == SYS_SYNC_PRIORITY);
    CHECK(f.r32(info + 0x04) == SYS_SYNC_RECURSIVE);
    CHECK(f.r64(info + 0x08) == kKept);
    CHECK(f.r64(info + 0x10) == c.id()); // the owner in the process's sys_lwmutex_t
    CHECK(f.r32(info + 0x18) == 3);
    CHECK(f.r32(info + 0x20) == 1);
    CHECK(f.r32(info + 0x24) == 1);
    CHECK(f.r64(list) == sleeper);
    REQUIRE(c.call(kLwMutexUnlock, lw) == CELL_OK);
    th.join();

    // Made without sysPrxForUser's kind: CELL_EINVAL; its sys_lwmutex_t not readable: CELL_EFAULT.
    REQUIRE(c.call(kLwMutexCreate, out, SYS_SYNC_FIFO, control, 0, 0) == CELL_OK);
    CHECK(c.call(kDbgLwMutexInfo, 1, f.r32(out), info) == rc(CELL_EINVAL));
    REQUIRE(c.call(kLwMutexCreate, out, SYS_SYNC_FIFO, 0x10, 0x80000001, 0) == CELL_OK);
    CHECK(c.call(kDbgLwMutexInfo, 1, f.r32(out), info) == rc(CELL_EFAULT));

    // An lwcond: its sys_lwcond_t gives the lwmutex's address.
    const u32 cvc = f.alloc(8, 8);
    REQUIRE(c.call(kLwCondCreate, out, lw, cvc, kEight) == CELL_OK);
    const u32 cv = f.r32(out);
    f.w32(cvc, control);
    f.w32(cvc + 4, cv);
    u32 waiter = 0;
    std::thread tw([&] {
        Caller me(*f.kernel, "waiter");
        waiter = me.id();
        THREAD_REQUIRE(me.call(kLwCondQueueWait, cv, lw, 0) == rc(CELL_EBUSY));
    });
    REQUIRE(eventually([&] { return queued<LwCond>(*f.kernel, cv) == 1; }));
    const u32 cinfo = info_block(f, 0x18);
    f.w32(cinfo + 0x0C, list);
    f.w32(cinfo + 0x10, 1);
    REQUIRE(c.call(kDbgLwCondInfo, 1, cv, cinfo) == CELL_OK);
    CHECK(f.r64(cinfo) == kKept);
    CHECK(f.r32(cinfo + 0x08) == control);
    CHECK(f.r32(cinfo + 0x10) == 1);
    CHECK(f.r32(cinfo + 0x14) == 1);
    CHECK(f.r64(list) == waiter);
    REQUIRE(c.call(kLwCondSignal, cv, 0, 0xFFFFFFFFull, 2) == CELL_OK); // mode 2: woken with CELL_EBUSY
    tw.join();
    CHECK(c.call(kDbgLwCondInfo, 2, cv, cinfo) == kDeInvalidArguments);
}

TEST_CASE("lv2 sys_dbg: an event queue's, a semaphore's and an event flag's information", "[lv2][dbg]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 out = f.alloc(4);
    const u32 qattr = queue_attr(f, SYS_SYNC_PRIORITY);
    f.w64(qattr + 8, kEight);
    REQUIRE(c.call(kQueueCreate, out, qattr, 0x1234, 8) == CELL_OK);
    const u32 q = f.r32(out);
    REQUIRE(c.call(kPortCreate, out, SYS_EVENT_PORT_LOCAL, 0x5151) == CELL_OK);
    const u32 port = f.r32(out);
    REQUIRE(c.call(kPortConnectLocal, port, q) == CELL_OK);
    REQUIRE(c.call(kPortSend, port, 1, 2, 3) == CELL_OK);
    REQUIRE(c.call(kPortSend, port, 4, 5, 6) == CELL_OK);

    const u32 info = info_block(f, 0x38);
    const u32 events = f.alloc(64, 8);
    f.w32(info + 0x1C, f.alloc(8, 8));
    f.w32(info + 0x20, 1);
    f.w32(info + 0x28, events);
    f.w32(info + 0x2C, 1); // room for one of the two
    REQUIRE(c.call(kDbgQueueInfo, 1, q, info) == CELL_OK);
    CHECK(f.r32(info) == SYS_SYNC_PRIORITY);
    CHECK(f.r32(info + 0x04) == SYS_PPU_QUEUE);
    CHECK(f.r64(info + 0x08) == kKept);
    CHECK(f.r64(info + 0x10) == 0x1234);
    CHECK(f.r32(info + 0x18) == 8);
    CHECK(f.r32(info + 0x20) == 0);
    CHECK(f.r32(info + 0x24) == 0);
    CHECK(f.r32(info + 0x2C) == 1);
    CHECK(f.r32(info + 0x30) == 2);
    CHECK(f.r32(info + 0x34) == 0xEEEEEEEE);
    CHECK(f.r64(events) == 0x5151);
    CHECK(f.r64(events + 8) == 1);
    CHECK(f.r64(events + 16) == 2);
    CHECK(f.r64(events + 24) == 3);
    CHECK(f.r64(events + 32) == 0);
    REQUIRE(c.call(kQueueDrain, q) == CELL_OK);
    u32 receiver = 0;
    std::thread th([&] {
        Caller me(*f.kernel, "receiver");
        receiver = me.id();
        THREAD_REQUIRE(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
    f.w32(info + 0x20, 1); // the last call left its counts in the capacities
    f.w32(info + 0x2C, 1);
    REQUIRE(c.call(kDbgQueueInfo, 1, q, info) == CELL_OK);
    CHECK(f.r32(info + 0x20) == 1);
    CHECK(f.r32(info + 0x24) == 1);
    CHECK(f.r64(f.r32(info + 0x1C)) == receiver);
    CHECK(f.r32(info + 0x2C) == 0);
    CHECK(f.r32(info + 0x30) == 0);
    REQUIRE(c.call(kPortSend, port, 7, 8, 9) == CELL_OK);
    th.join();

    // A semaphore at 0 with two waiters, by priority.
    const u32 sattr = sync_attr(f, SYS_SYNC_PRIORITY);
    f.w64(sattr + 24, kEight);
    REQUIRE(c.call(kSemCreate, out, sattr, 0, 5) == CELL_OK);
    const u32 sem = f.r32(out);
    u32 ids[2]{};
    std::vector<std::thread> pool;
    for (int i = 0; i < 2; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter", 2000 - 1000 * i);
            ids[i] = me.id();
            THREAD_REQUIRE(me.call(kSemWait, sem, 0) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<Semaphore>(*f.kernel, sem) == static_cast<usize>(i + 1); }));
    }
    const u32 sinfo = info_block(f, 0x38);
    const u32 slist = f.alloc(16, 8);
    f.w32(sinfo + 0x28, slist);
    f.w32(sinfo + 0x2C, 2);
    REQUIRE(c.call(kDbgSemaphoreInfo, 1, sem, sinfo) == CELL_OK);
    CHECK(f.r32(sinfo) == SYS_SYNC_PRIORITY);
    CHECK(f.r32(sinfo + 0x04) == SYS_SYNC_NOT_PROCESS_SHARED);
    CHECK(f.r64(sinfo + 0x10) == 0xEEEEEEEEEEEEEEEEull);
    CHECK(f.r64(sinfo + 0x18) == kKept);
    CHECK(f.r32(sinfo + 0x20) == 5); // the maximum, then the value
    CHECK(f.r32(sinfo + 0x24) == 0);
    CHECK(f.r32(sinfo + 0x2C) == 2);
    CHECK(f.r32(sinfo + 0x30) == 2);
    CHECK(f.r64(slist) == ids[1]);
    CHECK(f.r64(slist + 8) == ids[0]);
    REQUIRE(c.call(kSemPost, sem, 2) == CELL_OK);
    for (auto& t : pool) t.join();
    pool.clear();

    // An event flag: the waiters, and what each waits for.
    const u32 fattr = sync_attr(f, SYS_SYNC_FIFO, SYS_SYNC_WAITER_MULTIPLE);
    f.w64(fattr + 24, kEight);
    REQUIRE(c.call(kFlagCreate, out, fattr, 0x0F) == CELL_OK);
    const u32 flag = f.r32(out);
    const u64 wants[2][2] = {{0x100, SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR}, {0x200, SYS_EVENT_FLAG_WAIT_OR}};
    for (int i = 0; i < 2; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter");
            ids[i] = me.id();
            THREAD_REQUIRE(me.call(kFlagWait, flag, wants[i][0], wants[i][1], 0, 0) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<EventFlag>(*f.kernel, flag) == static_cast<usize>(i + 1); }));
    }
    const u32 finfo = info_block(f, 0x38);
    const u32 fids = f.alloc(16, 8), fwants = info_block(f, 32);
    f.w32(finfo + 0x28, fids);
    f.w32(finfo + 0x2C, fwants);
    f.w32(finfo + 0x30, 2);
    REQUIRE(c.call(kDbgFlagInfo, 1, flag, finfo) == CELL_OK);
    CHECK(f.r32(finfo) == SYS_SYNC_FIFO);
    CHECK(f.r32(finfo + 0x04) == SYS_SYNC_NOT_PROCESS_SHARED);
    CHECK(f.r64(finfo + 0x08) == 0);
    CHECK(f.r32(finfo + 0x10) == 0xEEEEEEEE); // the attribute's flags: the caller's
    CHECK(f.r32(finfo + 0x14) == SYS_SYNC_WAITER_MULTIPLE);
    CHECK(f.r64(finfo + 0x18) == kKept);
    CHECK(f.r64(finfo + 0x20) == 0x0F);
    CHECK(f.r32(finfo + 0x30) == 2);
    CHECK(f.r32(finfo + 0x34) == 2);
    for (int i = 0; i < 2; i++) {
        CHECK(f.r64(fids + 8 * i) == ids[i]);
        CHECK(f.r64(fwants + 16 * i) == wants[i][0]);
        CHECK(f.r32(fwants + 16 * i + 8) == wants[i][1]);
    }
    // Without the second list's address neither list is written.
    const u32 none = info_block(f, 0x38);
    f.w32(none + 0x28, fids);
    f.w32(none + 0x2C, 0);
    f.w32(none + 0x30, 2);
    REQUIRE(c.call(kDbgFlagInfo, 1, flag, none) == CELL_OK);
    CHECK(f.r32(none + 0x30) == 0);
    CHECK(f.r32(none + 0x34) == 2);
    REQUIRE(c.call(kFlagSet, flag, 0x300) == CELL_OK);
    for (auto& t : pool) t.join();
    CHECK(untouched(f, none + 0x10, 4));
}

TEST_CASE("lv2 sys_dbg: the PPU exception handler's queue, registered once and finalized with its last event",
    "[lv2][dbg]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 8) == CELL_OK);
    const u32 q = f.r32(out);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 8) == CELL_OK);
    const u32 other = f.r32(out);

    CHECK(c.call(kDbgFinalizeExceptionHandler, q) == kDeHandleNotRegistered);
    CHECK(c.call(kDbgInitExceptionHandler, 0x8DFFFF00) == rc(CELL_ESRCH));
    REQUIRE(c.call(kDbgInitExceptionHandler, q) == CELL_OK);
    CHECK(c.call(kDbgInitExceptionHandler, other) == kDeHandlerAlreadyRegistered);
    CHECK(c.call(kDbgFinalizeExceptionHandler, other) == kDeInvalidHandler);
    CHECK(c.call(kDbgFinalizeExceptionHandler, 0x8DFFFF00) == rc(CELL_ESRCH));
    REQUIRE(c.call(kDbgFinalizeExceptionHandler, q) == CELL_OK);
    // The handler's thread hears the end: from the kernel's first port, data1 all ones.
    const u32 ev = f.alloc(32, 8);
    REQUIRE(c.call(kQueueTryReceive, q, ev, 1, out) == CELL_OK);
    REQUIRE(f.r32(out) == 1);
    CHECK(f.r64(ev) == 0x8000111100000001ull);
    CHECK(f.r64(ev + 8) == ~0ull);
    CHECK(f.r64(ev + 16) == 0);
    CHECK(f.r64(ev + 24) == 0);
    CHECK(c.call(kDbgFinalizeExceptionHandler, q) == kDeHandleNotRegistered);
    REQUIRE(c.call(kDbgInitExceptionHandler, other) == CELL_OK); // registered anew
}

TEST_CASE("lv2 sys_dbg: the thread list, each thread's information, the process's and its memory containers'",
    "[lv2][dbg]")
{
    enum : u32 {
        kThreadList = 909,
        kThreadInfo = 910,
        kProcessInfo = 914,
        kContainerInfo = 957,
        kGroupCreate = 170,
        kSpuInitialize = 172,
    };
    Fixture f;
    Caller c(*f.kernel, "a main thread with a long name", 1500);

    // A thread asleep on a semaphore.
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_FIFO), 0, 1) == CELL_OK);
    const u32 sem = f.r32(out);
    u32 sleeper = 0;
    std::thread th([&] {
        Caller me(*f.kernel, "sleeper", 2500);
        sleeper = me.id();
        THREAD_REQUIRE(me.call(kSemWait, sem, 0) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<Semaphore>(*f.kernel, sem) == 1; }));

    // An SPU thread group of two, one thread initialized: its image has an INFO segment.
    const u32 gattr = f.alloc(16), gname = f.cstr("dbg group");
    f.w32(gattr, 10);
    f.w32(gattr + 4, gname);
    REQUIRE(c.call(kGroupCreate, out, 2, 200, gattr) == CELL_OK);
    const u32 group = f.r32(out);
    const u32 code = f.alloc(16), info_bytes = f.alloc(16), segs = f.alloc(0x30, 8), img = f.alloc(16);
    std::memcpy(f.base() + info_bytes, "spu program info", 16);
    f.w32(segs, 1);
    f.w32(segs + 8, 16);
    f.w32(segs + 0x10, code);
    f.w32(segs + 0x18, 4); // INFO
    f.w32(segs + 0x20, 16);
    f.w32(segs + 0x28, info_bytes);
    f.w32(img, 0); // a user image
    f.w32(img + 8, segs);
    f.w32(img + 12, 2);
    const u32 tattr = f.alloc(12), tname = f.cstr("spu one");
    f.w32(tattr, tname);
    f.w32(tattr + 4, 8);
    REQUIRE(c.call(kSpuInitialize, out, group, 0, img, tattr, f.alloc(32)) == CELL_OK);
    const u32 spu = f.r32(out);

    // The list: the PPU threads (64-bit IDs), then the group (its ID in the first word).
    const u32 list = f.alloc(16 * 8, 8), count = f.alloc(8, 8), all = f.alloc(8, 8);
    f.w64(count, 8);
    REQUIRE(c.call(kThreadList, 1, list, count, all) == CELL_OK);
    CHECK(f.r64(all) == 3);
    CHECK(f.r64(count) == 3);
    CHECK(f.r32(list) == 0);
    CHECK(f.r64(list + 8) == c.id());
    CHECK(f.r32(list + 16) == 0);
    CHECK(f.r64(list + 24) == sleeper);
    CHECK(f.r32(list + 32) == 2);
    CHECK(f.r32(list + 40) == group);
    // Room for two: two written, *count left as it was; no list: only *all.
    f.w64(count, 2);
    f.w64(list + 32, 0);
    REQUIRE(c.call(kThreadList, 1, list, count, all) == CELL_OK);
    CHECK(f.r64(count) == 2);
    CHECK(f.r64(all) == 3);
    CHECK(f.r64(list + 32) == 0);
    f.w64(all, 0);
    REQUIRE(c.call(kThreadList, 1, 0, count, all) == CELL_OK);
    CHECK(f.r64(all) == 3);
    CHECK(c.call(kThreadList, 1, list, count, 0) == kDeInvalidArguments);
    f.w64(count, 0x80000000ull);
    CHECK(c.call(kThreadList, 1, list, count, all) == kDeInvalidArguments);
    f.w64(count, 2);
    CHECK(c.call(kThreadList, 7, list, count, all) == kDeInvalidArguments);

    // Each one's information.
    const u32 entry = f.alloc(16, 8), info = f.alloc(0x40, 8), name = f.alloc(0x80), name2 = f.alloc(0x200);
    const auto ask = [&](u32 type, u64 id) {
        f.w32(entry, type);
        f.w64(entry + 8, id);
        return c.call(kThreadInfo, entry, info);
    };
    f.w32(info + 0x14, name);
    REQUIRE(ask(0, c.id()) == CELL_OK);
    CHECK(f.r32(info) == 1500);
    CHECK(f.r32(info + 0x04) == 2); // on a processor: the caller
    CHECK(f.r32(info + 0x10) == 1);
    CHECK(f.r32(info + 0x18) == 1500);
    CHECK(std::string(reinterpret_cast<const char*>(f.base() + name)) == "a main thread with a long n"); // 27
    REQUIRE(ask(0, sleeper) == CELL_OK);
    CHECK(f.r32(info) == 2500);
    CHECK(f.r32(info + 0x04) == 3); // sleeping
    CHECK(std::string(reinterpret_cast<const char*>(f.base() + name)) == "sleeper");
    CHECK(ask(0, 0x01FFFFFF) == static_cast<s32>(0x80010402));
    f.w32(info + 0x08, name2);
    f.w32(info + 0x0C, name);
    REQUIRE(ask(1, u64{spu} << 32) == CELL_OK);
    CHECK(f.r32(info) == group);
    CHECK(f.r32(info + 0x04) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(f.base() + name)) == "spu one");
    CHECK(std::memcmp(f.base() + name2, "spu program info", 16) == 0);
    CHECK(f.r32(name2 + 16) == 0);
    const u32 members = f.alloc(16);
    f.w32(info + 0x10, 4);
    f.w32(info + 0x18, members);
    REQUIRE(ask(2, u64{group} << 32) == CELL_OK);
    CHECK(f.r32(info) == 200);
    CHECK(f.r32(info + 0x04) == 0); // not every thread initialized yet
    CHECK(f.r32(info + 0x08) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(f.base() + name)) == "dbg group");
    CHECK(f.r32(info + 0x10) == 1);
    CHECK(f.r32(info + 0x14) == 2);
    CHECK(f.r32(members) == spu);
    CHECK(ask(3, 0) == static_cast<s32>(0x80010415));
    CHECK(c.call(kThreadInfo, 0, info) == kDeInvalidArguments);

    // The process.
    const u32 pinfo = f.alloc(0x38, 8);
    f.w32(pinfo + 0x18, 0);
    REQUIRE(c.call(kProcessInfo, 1, pinfo) == CELL_OK);
    CHECK(f.r32(pinfo) == 1);
    CHECK(f.r32(pinfo + 0x04) == 2);
    CHECK(f.r32(pinfo + 0x08) == 2);
    CHECK(f.r32(pinfo + 0x0C) == 1);
    CHECK(f.r32(pinfo + 0x10) == 0);
    CHECK(c.call(kProcessInfo, 1, 0) == kDeInvalidArguments);
    CHECK(c.call(kProcessInfo, 2, pinfo) == kDeInvalidArguments);

    // Its memory containers: its own, then one made from it.
    REQUIRE(c.call(kContainerCreate, out, 0x100000) == CELL_OK);
    const u32 ct = f.r32(out);
    const u32 cts = f.alloc(64, 8), n = f.alloc(4);
    f.w32(n, 1);
    CHECK(c.call(kContainerInfo, 1, cts, n) == rc(CELL_EINVAL));
    f.w32(n, 4);
    REQUIRE(c.call(kContainerInfo, 1, cts, n) == CELL_OK);
    CHECK(f.r32(n) == 2);
    CHECK(f.r32(cts + 8) == 0xFFFFFFFF);
    CHECK(f.r32(cts + 12) == 0xFFFFFFFF);
    CHECK(f.r32(cts + 16) == 0x100000);
    CHECK(f.r32(cts + 20) == 0x100000);
    CHECK(f.r32(cts + 24) == ct);
    CHECK(f.r32(cts + 28) == 0xFFFFFFFF);
    // Its own: what the new one took is not free (nor the SPU thread group's two local stores).
    CHECK(f.r32(cts + 4) == f.r32(cts) - 0x100000 - 2 * 0x40000);
    CHECK(c.call(kContainerInfo, 2, cts, n) == rc(CELL_ENOSYS));

    REQUIRE(c.call(kSemPost, sem, 1) == CELL_OK);
    th.join();
}
