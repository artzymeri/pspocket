// sys_mutex and sys_cond through the dispatcher: semantics, error codes, protocols, contention.

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

u32 make_mutex(Fixture& f, Caller& c, u32 protocol, u32 recursive)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kMutexCreate, out, mutex_attr(f, protocol, recursive)) == CELL_OK);
    return f.r32(out);
}

u32 make_cond(Fixture& f, Caller& c, u32 mutex)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kCondCreate, out, mutex, cond_attr(f)) == CELL_OK);
    return f.r32(out);
}

} // namespace

TEST_CASE("lv2 mutex: attributes, recursion and error codes", "[lv2][mutex]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kMutexCreate, out, mutex_attr(f, 7, SYS_SYNC_NOT_RECURSIVE)) == rc(CELL_EINVAL));
    CHECK(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_FIFO, 0x30)) == rc(CELL_EINVAL));
    CHECK(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_PRIORITY_INHERIT, SYS_SYNC_RECURSIVE)) == CELL_OK);

    const u32 m = make_mutex(f, c, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE);
    CHECK(c.call(kMutexLock, m, 0) == CELL_OK);
    CHECK(c.call(kMutexLock, m, 0) == rc(CELL_EDEADLK));
    CHECK(c.call(kMutexTryLock, m) == rc(CELL_EDEADLK));
    CHECK(c.call(kMutexDestroy, m) == rc(CELL_EBUSY));
    CHECK(c.call(kMutexUnlock, m) == CELL_OK);
    CHECK(c.call(kMutexUnlock, m) == rc(CELL_EPERM));

    const u32 r = make_mutex(f, c, SYS_SYNC_FIFO, SYS_SYNC_RECURSIVE);
    for (int i = 0; i < 3; i++) CHECK(c.call(kMutexLock, r, 0) == CELL_OK);
    CHECK(c.call(kMutexTryLock, r) == CELL_OK);
    for (int i = 0; i < 4; i++) CHECK(c.call(kMutexUnlock, r) == CELL_OK);
    CHECK(c.call(kMutexUnlock, r) == rc(CELL_EPERM));

    // Another thread's view.
    CHECK(c.call(kMutexLock, m, 0) == CELL_OK);
    std::thread([&] {
        Caller other(*f.kernel, "other");
        CHECK(other.call(kMutexTryLock, m) == rc(CELL_EBUSY));
        CHECK(other.call(kMutexUnlock, m) == rc(CELL_EPERM));
    }).join();
    CHECK(c.call(kMutexUnlock, m) == CELL_OK);

    CHECK(c.call(kMutexDestroy, m) == CELL_OK);
    CHECK(c.call(kMutexLock, m, 0) == rc(CELL_ESRCH));
    CHECK(c.call(kMutexDestroy, m) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 mutex: process-shared creation by IPC key", "[lv2][mutex]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    const u64 key = 0x0123456789ABCDEFull;
    CHECK(c.call(kMutexCreate, out,
              mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NOT_CREATE)) ==
        rc(CELL_ESRCH));
    REQUIRE(c.call(kMutexCreate, out,
                mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NEWLY_CREATED)) ==
        CELL_OK);
    const u32 a = f.r32(out);
    CHECK(c.call(kMutexCreate, out,
              mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NEWLY_CREATED)) ==
        rc(CELL_EEXIST));
    REQUIRE(c.call(kMutexCreate, out,
                mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NOT_CARE)) ==
        CELL_OK);
    const u32 b = f.r32(out);
    CHECK(a != b);
    // Two IDs, one mutex.
    REQUIRE(c.call(kMutexLock, a, 0) == CELL_OK);
    CHECK(c.call(kMutexTryLock, b) == rc(CELL_EDEADLK));
    REQUIRE(c.call(kMutexUnlock, b) == CELL_OK);
    // Destroying one ID leaves the other working; the key goes with the last.
    REQUIRE(c.call(kMutexDestroy, a) == CELL_OK);
    CHECK(c.call(kMutexTryLock, b) == CELL_OK);
    CHECK(c.call(kMutexUnlock, b) == CELL_OK);
    REQUIRE(c.call(kMutexDestroy, b) == CELL_OK);
    CHECK(c.call(kMutexCreate, out,
              mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_PROCESS_SHARED, key, SYS_SYNC_NOT_CREATE)) ==
        rc(CELL_ESRCH));
    // Bad sharing attributes.
    CHECK(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, 0x300)) == rc(CELL_EINVAL));
    CHECK(c.call(kMutexCreate, out,
              mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_PROCESS_SHARED, 0, SYS_SYNC_NOT_CARE)) ==
        rc(CELL_EINVAL));
}

TEST_CASE("lv2 mutex: mutual exclusion under contention", "[lv2][mutex][stress]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 m = make_mutex(f, c, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE);
    const u32 counter = f.alloc(8);
    constexpr int kThreads = 8, kIters = 2000;
    u64 plain = 0;
    std::atomic<int> inside{0}, violations{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < kThreads; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "worker", 1000 + i * 10);
            for (int k = 0; k < kIters; k++) {
                REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
                if (++inside != 1) violations++;
                plain++;
                f.w64(counter, f.r64(counter) + 1);
                inside--;
                REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
            }
        });
    }
    for (auto& t : pool) t.join();
    CHECK(violations.load() == 0);
    CHECK(plain == u64{kThreads} * kIters);
    CHECK(f.r64(counter) == u64{kThreads} * kIters);
}

TEST_CASE("lv2 mutex: lock timeout", "[lv2][mutex]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 m = make_mutex(f, c, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE);
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK);
    std::thread([&] {
        Caller other(*f.kernel, "other");
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(other.call(kMutexLock, m, 20000) == rc(CELL_ETIMEDOUT));
        CHECK(elapsed_ms(t0) >= 19);
    }).join();
    CHECK(queued<Mutex>(*f.kernel, m) == 0);
    REQUIRE(c.call(kMutexUnlock, m) == CELL_OK);
    std::thread([&] {
        Caller other(*f.kernel, "other");
        CHECK(other.call(kMutexLock, m, 20000) == CELL_OK);
        CHECK(other.call(kMutexUnlock, m) == CELL_OK);
    }).join();
}

TEST_CASE("lv2 mutex: ownership passes in protocol order", "[lv2][mutex]")
{
    const auto protocol = GENERATE(SYS_SYNC_FIFO, SYS_SYNC_PRIORITY);
    Fixture f;
    Caller c(*f.kernel, "owner");
    const u32 m = make_mutex(f, c, protocol, SYS_SYNC_NOT_RECURSIVE);
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK);

    const s32 prios[] = {3000, 100, 2000, 100};
    std::vector<int> order;
    std::vector<std::thread> pool;
    for (int i = 0; i < 4; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter", prios[i]);
            REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
            order.push_back(i); // under the mutex
            REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<Mutex>(*f.kernel, m) == static_cast<usize>(i + 1); }));
    }
    REQUIRE(c.call(kMutexUnlock, m) == CELL_OK);
    for (auto& t : pool) t.join();
    if (protocol == SYS_SYNC_FIFO) {
        CHECK(order == std::vector<int>{0, 1, 2, 3});
    } else {
        CHECK(order == std::vector<int>{1, 3, 2, 0}); // lowest value first, arrival order among equals
    }
}

TEST_CASE("lv2 cond: errors, signal, signal_all, signal_to", "[lv2][cond]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 out = f.alloc(4);
    CHECK(c.call(kCondCreate, out, 0x85001234, cond_attr(f)) == rc(CELL_ESRCH));
    const u32 m = make_mutex(f, c, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE);
    const u32 cv = make_cond(f, c, m);
    CHECK(c.call(kCondWait, cv, 0) == rc(CELL_EPERM)); // mutex not owned
    CHECK(c.call(kCondSignal, cv) == CELL_OK);         // nobody waiting
    CHECK(c.call(kCondSignalTo, cv, c.id()) == rc(CELL_EPERM));
    CHECK(c.call(kCondSignalTo, cv, 0x01000077) == rc(CELL_ESRCH));

    std::atomic<int> woke{0};
    std::vector<u32> ids(3);
    std::vector<std::thread> pool;
    for (int i = 0; i < 3; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "waiter");
            ids[i] = me.id();
            REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
            REQUIRE(me.call(kCondWait, cv, 0) == CELL_OK);
            woke++;
            REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<Cond>(*f.kernel, cv) == static_cast<usize>(i + 1); }));
    }
    CHECK(c.call(kCondDestroy, cv) == rc(CELL_EBUSY));
    CHECK(c.call(kMutexDestroy, m) == rc(CELL_EPERM));

    REQUIRE(c.call(kCondSignal, cv) == CELL_OK);
    REQUIRE(eventually([&] { return woke.load() == 1; }));
    // Equal priorities: the earliest waiter (ids[0]) went first. Now pick the last one explicitly.
    REQUIRE(c.call(kCondSignalTo, cv, ids[2]) == CELL_OK);
    REQUIRE(eventually([&] { return woke.load() == 2; }));
    CHECK(queued<Cond>(*f.kernel, cv) == 1); // ids[1] still waits
    REQUIRE(c.call(kCondSignalAll, cv) == CELL_OK);
    for (auto& t : pool) t.join();
    CHECK(woke.load() == 3);
    CHECK(c.call(kCondDestroy, cv) == CELL_OK);
    CHECK(c.call(kMutexDestroy, m) == CELL_OK);
}

TEST_CASE("lv2 cond: signaled waiters reacquire the mutex; timeouts keep it", "[lv2][cond]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 m = make_mutex(f, c, SYS_SYNC_FIFO, SYS_SYNC_RECURSIVE);
    const u32 cv = make_cond(f, c, m);

    // A timed-out wait returns with the mutex owned and the recursion depth restored.
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK);
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kCondWait, cv, 15000) == rc(CELL_ETIMEDOUT));
    CHECK(elapsed_ms(t0) >= 14);
    CHECK(c.call(kMutexUnlock, m) == CELL_OK);
    CHECK(c.call(kMutexUnlock, m) == CELL_OK);
    CHECK(c.call(kMutexUnlock, m) == rc(CELL_EPERM));

    // A signaled waiter can't run until the signaler releases the mutex.
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        Caller me(*f.kernel, "waiter");
        REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
        REQUIRE(me.call(kCondWait, cv, 0) == CELL_OK);
        returned = true;
        REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<Cond>(*f.kernel, cv) == 1; }));
    REQUIRE(c.call(kMutexLock, m, 0) == CELL_OK);
    REQUIRE(c.call(kCondSignal, cv) == CELL_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(returned.load());
    CHECK(queued<Mutex>(*f.kernel, m) == 1); // moved to the mutex queue
    REQUIRE(c.call(kMutexUnlock, m) == CELL_OK);
    waiter.join();
    CHECK(returned.load());
}

TEST_CASE("lv2 cond: producer/consumer on a bounded buffer", "[lv2][cond][stress]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 m = make_mutex(f, c, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE);
    const u32 not_empty = make_cond(f, c, m);
    const u32 not_full = make_cond(f, c, m);
    constexpr int kProducers = 4, kConsumers = 4, kItems = 1000, kCapacity = 8;
    std::vector<u64> buffer;
    u64 consumed_sum = 0, consumed_count = 0;
    std::vector<std::thread> pool;
    for (int p = 0; p < kProducers; p++) {
        pool.emplace_back([&, p] {
            Caller me(*f.kernel, "producer");
            for (int i = 0; i < kItems; i++) {
                REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
                while (buffer.size() >= kCapacity) REQUIRE(me.call(kCondWait, not_full, 0) == CELL_OK);
                buffer.push_back(static_cast<u64>(p) * kItems + i + 1);
                REQUIRE(me.call(kCondSignal, not_empty) == CELL_OK);
                REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
            }
        });
    }
    for (int k = 0; k < kConsumers; k++) {
        pool.emplace_back([&] {
            Caller me(*f.kernel, "consumer");
            for (int i = 0; i < kItems * kProducers / kConsumers; i++) {
                REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
                while (buffer.empty()) REQUIRE(me.call(kCondWait, not_empty, 0) == CELL_OK);
                consumed_sum += buffer.back();
                consumed_count++;
                buffer.pop_back();
                REQUIRE(me.call(kCondSignal, not_full) == CELL_OK);
                REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
            }
        });
    }
    for (auto& t : pool) t.join();
    const u64 n = u64{kProducers} * kItems;
    CHECK(consumed_count == n);
    CHECK(consumed_sum == n * (n + 1) / 2);
    CHECK(buffer.empty());
}
