// Kernel side of sys_lwmutex / sys_lwcond. The user-space halves (sysPrxForUser) are ported here from RPCS3
// rpcs3/Emu/Cell/Modules/sys_lwmutex_.cpp and sys_lwcond_.cpp (without SYS_SYNC_RETRY) so the kernel protocol is
// exercised the way the firmware library drives it: atomics on the guest's sys_lwmutex_t, syscalls on contention.

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u32 kFree = 0xFFFFFFFFu;     // lwmutex_free
constexpr u32 kDead = 0xFFFFFFFEu;     // lwmutex_dead
constexpr u32 kReserved = 0xFFFFFFFDu; // lwmutex_reserved

// sys_lwmutex_t: owner, waiter (one big-endian u64 "all_info"), attribute, recursive_count, sleep_queue, pad.
struct LwMutexUser {
    Fixture& f;
    u32 addr;

    u32* owner_ptr() const { return reinterpret_cast<u32*>(f.base() + addr); }
    u64* all_ptr() const { return reinterpret_cast<u64*>(f.base() + addr); }
    u32 owner() const { return ps_bswap32(__atomic_load_n(owner_ptr(), __ATOMIC_SEQ_CST)); }
    void set_owner(u32 v) const { __atomic_store_n(owner_ptr(), ps_bswap32(v), __ATOMIC_SEQ_CST); }
    u32 exchange_owner(u32 v) const { return ps_bswap32(__atomic_exchange_n(owner_ptr(), ps_bswap32(v), __ATOMIC_SEQ_CST)); }
    bool cas_owner(u32 expected, u32 desired) const
    {
        u32 e = ps_bswap32(expected);
        return __atomic_compare_exchange_n(owner_ptr(), &e, ps_bswap32(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    // all_info += d (the big-endian 64-bit {owner, waiter}: adds to waiter).
    void add_all(s64 d) const
    {
        u64 cur = __atomic_load_n(all_ptr(), __ATOMIC_SEQ_CST);
        while (!__atomic_compare_exchange_n(all_ptr(), &cur, ps_bswap64(ps_bswap64(cur) + static_cast<u64>(d)), false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        }
    }
    bool cas_all(u64 expected, u64 desired) const
    {
        u64 e = ps_bswap64(expected);
        return __atomic_compare_exchange_n(all_ptr(), &e, ps_bswap64(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    u32 attribute() const { return f.r32(addr + 8); }
    u32 recursive_count() const { return f.r32(addr + 12); }
    void set_recursive_count(u32 v) const { f.w32(addr + 12, v); }
    u32 sleep_queue() const { return f.r32(addr + 16); }
};

LwMutexUser lwmutex_create(Fixture& f, Caller& c, u32 protocol, u32 recursive)
{
    const LwMutexUser m{f, f.alloc(24, 8)};
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kLwMutexCreate, out, protocol, m.addr, 0x80000001, 0x6C776D7574657800ull) == CELL_OK);
    f.w64(m.addr, u64{kFree} << 32);
    f.w32(m.addr + 8, recursive | protocol);
    f.w32(m.addr + 12, 0);
    f.w32(m.addr + 16, f.r32(out));
    return m;
}

s32 lwmutex_lock(Caller& c, const LwMutexUser& m, u64 timeout)
{
    const u32 tid = c.id();
    if (m.cas_owner(kFree, tid)) return CELL_OK;
    const u32 old = m.owner();
    if (old == tid) {
        if (!(m.attribute() & SYS_SYNC_RECURSIVE)) return rc(CELL_EDEADLK);
        m.set_recursive_count(m.recursive_count() + 1);
        return CELL_OK;
    }
    if (old == kDead) return rc(CELL_EINVAL);
    for (int i = 0; i < 10; i++) {
        if (m.owner() == kFree && m.cas_owner(kFree, tid)) return CELL_OK;
    }
    m.add_all(1);
    if (m.cas_owner(kFree, tid)) {
        m.add_all(-1);
        return CELL_OK;
    }
    const s32 res = c.call(kLwMutexLock, m.sleep_queue(), timeout);
    m.add_all(-1);
    if (res == CELL_OK) {
        const u32 prev = m.exchange_owner(tid);
        REQUIRE(prev == kReserved);
    }
    return res;
}

s32 lwmutex_trylock(Caller& c, const LwMutexUser& m)
{
    const u32 tid = c.id();
    if (m.cas_owner(kFree, tid)) return CELL_OK;
    const u32 old = m.owner();
    if (old == tid) {
        if (!(m.attribute() & SYS_SYNC_RECURSIVE)) return rc(CELL_EDEADLK);
        m.set_recursive_count(m.recursive_count() + 1);
        return CELL_OK;
    }
    if (old == kDead) return rc(CELL_EINVAL);
    if (old == kReserved) {
        const s32 res = c.call(kLwMutexTryLock, m.sleep_queue());
        if (res == CELL_OK) REQUIRE(m.exchange_owner(tid) == kReserved);
        return res;
    }
    return rc(CELL_EBUSY);
}

s32 lwmutex_unlock(Caller& c, const LwMutexUser& m)
{
    const u32 tid = c.id();
    if (m.owner() != tid) return rc(CELL_EPERM);
    if (m.recursive_count()) {
        m.set_recursive_count(m.recursive_count() - 1);
        return CELL_OK;
    }
    if (m.cas_all(u64{tid} << 32, u64{kFree} << 32)) return CELL_OK; // no waiters
    m.set_owner(kReserved);
    const s32 res = c.call(kLwMutexUnlock, m.sleep_queue());
    return res == rc(CELL_ESRCH) ? res : CELL_OK;
}

// sys_lwcond_t: lwmutex pointer, lwcond_queue.
struct LwCondUser {
    Fixture& f;
    u32 addr;
    LwMutexUser mutex;
    u32 queue() const { return f.r32(addr + 4); }
};

LwCondUser lwcond_create(Fixture& f, Caller& c, const LwMutexUser& m)
{
    const LwCondUser cv{f, f.alloc(8, 8), m};
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kLwCondCreate, out, m.sleep_queue(), cv.addr, 0x6C77636F6E640000ull) == CELL_OK);
    f.w32(cv.addr, m.addr);
    f.w32(cv.addr + 4, f.r32(out));
    return cv;
}

s32 lwcond_wait(Caller& c, const LwCondUser& cv, u64 timeout)
{
    const LwMutexUser& m = cv.mutex;
    const u32 tid = c.id();
    if (m.owner() != tid) return rc(CELL_EPERM);
    const u32 recursive = m.recursive_count();
    m.set_owner(kReserved);
    m.set_recursive_count(0);
    const s32 res = c.call(kLwCondQueueWait, cv.queue(), m.sleep_queue(), timeout);
    if (res == CELL_OK) {
        m.add_all(-1);
        const u32 prev = m.exchange_owner(tid);
        REQUIRE(prev != kFree);
        REQUIRE(prev != kDead);
        m.set_recursive_count(recursive);
        return CELL_OK;
    }
    if (res == rc(CELL_EBUSY) || res == rc(CELL_ETIMEDOUT)) {
        REQUIRE(lwmutex_lock(c, m, 0) == CELL_OK);
        m.set_recursive_count(recursive);
        return res == rc(CELL_EBUSY) ? CELL_OK : res;
    }
    FAIL("unexpected _sys_lwcond_queue_wait result " << res);
    return res;
}

s32 lwcond_signal(Caller& c, const LwCondUser& cv)
{
    const LwMutexUser& m = cv.mutex;
    if (m.owner() == c.id()) {
        m.add_all(1);
        const s32 res = c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), 0xFFFFFFFFull, 1);
        if (res != CELL_OK) {
            m.add_all(-1);
            if (res != rc(CELL_EPERM)) return res;
        }
        return CELL_OK;
    }
    if (lwmutex_trylock(c, m) != CELL_OK) return c.call(kLwCondSignal, cv.queue(), 0, 0xFFFFFFFFull, 2);
    // Locked: hand the lwmutex to the waiter (mode 3), or unlock if there is none.
    m.add_all(1);
    m.set_owner(kReserved);
    const s32 res = c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), 0xFFFFFFFFull, 3);
    if (res != CELL_OK) {
        m.add_all(-1);
        m.set_owner(c.id());
        REQUIRE(lwmutex_unlock(c, m) == CELL_OK);
        if (res != rc(CELL_ENOENT)) return res;
    }
    return CELL_OK;
}

s32 lwcond_signal_all(Caller& c, const LwCondUser& cv)
{
    const LwMutexUser& m = cv.mutex;
    if (m.owner() == c.id()) {
        const s32 res = c.call(kLwCondSignalAll, cv.queue(), m.sleep_queue(), 1);
        if (res <= 0) return res;
        m.add_all(res);
        return CELL_OK;
    }
    if (lwmutex_trylock(c, m) != CELL_OK) return c.call(kLwCondSignalAll, cv.queue(), m.sleep_queue(), 2);
    s32 res = c.call(kLwCondSignalAll, cv.queue(), m.sleep_queue(), 1);
    if (res > 0) {
        m.add_all(res);
        res = CELL_OK;
    }
    REQUIRE(lwmutex_unlock(c, m) == CELL_OK);
    return res;
}

} // namespace

TEST_CASE("lv2 lwmutex: kernel signal word, unlock2 and sleepers", "[lv2][lwmutex]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 out = f.alloc(4);
    CHECK(c.call(kLwMutexCreate, out, SYS_SYNC_PRIORITY_INHERIT, f.alloc(24), 0x80000001, 0) == rc(CELL_EINVAL));
    REQUIRE(c.call(kLwMutexCreate, out, SYS_SYNC_RETRY, f.alloc(24), 0, 0x41) == CELL_OK);
    const auto created = f.kernel->impl().objects.get<LwMutex>(f.r32(out));
    CHECK(created->name == 0); // has_name >= 0: no name
    REQUIRE(c.call(kLwMutexCreate, out, SYS_SYNC_FIFO, f.alloc(24), 0x80000001, 0x41) == CELL_OK);
    const u32 id = f.r32(out);
    CHECK(f.kernel->impl().objects.get<LwMutex>(id)->name == 0x41);

    // No sleepers: unlock leaves a signal that the next lock or trylock consumes.
    CHECK(c.call(kLwMutexTryLock, id) == rc(CELL_EBUSY));
    CHECK(c.call(kLwMutexUnlock, id) == CELL_OK);
    CHECK(c.call(kLwMutexTryLock, id) == CELL_OK);
    CHECK(c.call(kLwMutexTryLock, id) == rc(CELL_EBUSY));
    CHECK(c.call(kLwMutexUnlock, id) == CELL_OK);
    CHECK(c.call(kLwMutexLock, id, 0) == CELL_OK);
    // unlock2's signal makes the next lock return EBUSY (retry in user space); trylock ignores it.
    CHECK(c.call(kLwMutexUnlock2, id) == CELL_OK);
    CHECK(c.call(kLwMutexTryLock, id) == rc(CELL_EBUSY));
    CHECK(c.call(kLwMutexLock, id, 0) == rc(CELL_EBUSY));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kLwMutexLock, id, 15000) == rc(CELL_ETIMEDOUT));
    CHECK(elapsed_ms(t0) >= 14);

    // Sleepers are woken by unlock (CELL_OK) or unlock2 (CELL_EBUSY).
    for (const u32 how : {kLwMutexUnlock, kLwMutexUnlock2}) {
        std::atomic<s32> result{-1};
        std::thread sleeper([&] {
            Caller me(*f.kernel, "sleeper");
            result = me.call(kLwMutexLock, id, 0);
        });
        REQUIRE(eventually([&] { return queued<LwMutex>(*f.kernel, id) == 1; }));
        CHECK(c.call(kLwMutexDestroy, id) == rc(CELL_EBUSY));
        REQUIRE(c.call(how, id) == CELL_OK);
        sleeper.join();
        CHECK(result.load() == (how == kLwMutexUnlock ? CELL_OK : rc(CELL_EBUSY)));
    }
    CHECK(c.call(kLwMutexDestroy, id) == CELL_OK);
    CHECK(c.call(kLwMutexLock, id, 0) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 lwmutex: mutual exclusion through the user-space protocol", "[lv2][lwmutex][stress]")
{
    const auto protocol = GENERATE(SYS_SYNC_FIFO, SYS_SYNC_PRIORITY);
    Fixture f;
    Caller c(*f.kernel, "main");
    const LwMutexUser m = lwmutex_create(f, c, protocol, SYS_SYNC_NOT_RECURSIVE);
    const u32 counter = f.alloc(8);
    constexpr int kThreads = 6, kIters = 2000;
    std::atomic<int> inside{0}, violations{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < kThreads; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, "worker", 900 + i * 50);
            for (int k = 0; k < kIters; k++) {
                REQUIRE(lwmutex_lock(me, m, 0) == CELL_OK);
                if (++inside != 1) violations++;
                f.w64(counter, f.r64(counter) + 1);
                if ((k & 63) == 0) std::this_thread::yield(); // let others pile up in the kernel
                inside--;
                REQUIRE(lwmutex_unlock(me, m) == CELL_OK);
            }
        });
    }
    for (auto& t : pool) t.join();
    CHECK(violations.load() == 0);
    CHECK(f.r64(counter) == u64{kThreads} * kIters);
    CHECK(m.owner() == kFree);
}

TEST_CASE("lv2 lwcond: kernel signal modes and errors", "[lv2][lwcond]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const LwMutexUser m = lwmutex_create(f, c, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE);
    const LwCondUser cv = lwcond_create(f, c, m);
    const u32 out = f.alloc(4);
    CHECK(c.call(kLwCondCreate, out, 0x95001234, f.alloc(8), 0) == rc(CELL_ESRCH));
    // No waiters: mode 1 -> EPERM, mode 2 -> OK, mode 3 -> ENOENT; a bad mode -> EINVAL.
    CHECK(c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), 0xFFFFFFFFull, 1) == rc(CELL_EPERM));
    CHECK(c.call(kLwCondSignal, cv.queue(), 0, 0xFFFFFFFFull, 2) == CELL_OK);
    CHECK(c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), 0xFFFFFFFFull, 3) == rc(CELL_ENOENT));
    CHECK(c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), 0xFFFFFFFFull, 4) == rc(CELL_EINVAL));
    CHECK(c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), 0x01000077, 1) == rc(CELL_ESRCH));
    CHECK(c.call(kLwCondSignal, cv.queue(), m.sleep_queue(), c.id(), 1) == rc(CELL_EPERM));
    CHECK(c.call(kLwCondSignalAll, cv.queue(), m.sleep_queue(), 3) == rc(CELL_EINVAL));
    CHECK(c.call(kLwCondSignalAll, cv.queue(), m.sleep_queue(), 1) == 0);

    // A timed-out wait reacquires the lwmutex.
    REQUIRE(lwmutex_lock(c, m, 0) == CELL_OK);
    CHECK(lwcond_wait(c, cv, 15000) == rc(CELL_ETIMEDOUT));
    CHECK(m.owner() == c.id());
    // A waiter blocks destruction.
    std::thread waiter([&] {
        Caller me(*f.kernel, "waiter");
        REQUIRE(lwmutex_lock(me, m, 0) == CELL_OK);
        REQUIRE(lwcond_wait(me, cv, 0) == CELL_OK);
        REQUIRE(lwmutex_unlock(me, m) == CELL_OK);
    });
    REQUIRE(lwmutex_unlock(c, m) == CELL_OK);
    REQUIRE(eventually([&] { return queued<LwCond>(*f.kernel, cv.queue()) == 1; }));
    CHECK(c.call(kLwCondDestroy, cv.queue()) == rc(CELL_EBUSY));
    REQUIRE(lwcond_signal(c, cv) == CELL_OK); // not the owner: trylock + mode 3
    waiter.join();
    CHECK(c.call(kLwCondDestroy, cv.queue()) == CELL_OK);
    CHECK(c.call(kLwCondDestroy, cv.queue()) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 lwcond: producer/consumer and signal_all through the user-space protocol", "[lv2][lwcond][stress]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const LwMutexUser m = lwmutex_create(f, c, SYS_SYNC_PRIORITY, SYS_SYNC_RECURSIVE);
    const LwCondUser not_empty = lwcond_create(f, c, m);
    const LwCondUser not_full = lwcond_create(f, c, m);
    constexpr int kProducers = 3, kConsumers = 3, kItems = 800, kCapacity = 4;
    std::vector<u64> buffer;
    u64 sum = 0, count = 0;
    std::vector<std::thread> pool;
    for (int p = 0; p < kProducers; p++) {
        pool.emplace_back([&, p] {
            Caller me(*f.kernel, "producer");
            for (int i = 0; i < kItems; i++) {
                REQUIRE(lwmutex_lock(me, m, 0) == CELL_OK);
                REQUIRE(lwmutex_lock(me, m, 0) == CELL_OK); // recursion survives the waits
                while (buffer.size() >= kCapacity) REQUIRE(lwcond_wait(me, not_full, 0) == CELL_OK);
                buffer.push_back(static_cast<u64>(p) * kItems + i + 1);
                REQUIRE(lwcond_signal(me, not_empty) == CELL_OK);
                REQUIRE(lwmutex_unlock(me, m) == CELL_OK);
                REQUIRE(lwmutex_unlock(me, m) == CELL_OK);
            }
        });
    }
    for (int k = 0; k < kConsumers; k++) {
        pool.emplace_back([&] {
            Caller me(*f.kernel, "consumer");
            for (int i = 0; i < kItems * kProducers / kConsumers; i++) {
                REQUIRE(lwmutex_lock(me, m, 0) == CELL_OK);
                while (buffer.empty()) REQUIRE(lwcond_wait(me, not_empty, 0) == CELL_OK);
                sum += buffer.back();
                count++;
                buffer.pop_back();
                REQUIRE(lwcond_signal(me, not_full) == CELL_OK);
                REQUIRE(lwmutex_unlock(me, m) == CELL_OK);
            }
        });
    }
    for (auto& t : pool) t.join();
    const u64 n = u64{kProducers} * kItems;
    CHECK(count == n);
    CHECK(sum == n * (n + 1) / 2);

    // signal_all from the owner wakes every waiter, one at a time through the lwmutex.
    std::atomic<int> woke{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 4; i++) {
        waiters.emplace_back([&] {
            Caller me(*f.kernel, "waiter");
            REQUIRE(lwmutex_lock(me, m, 0) == CELL_OK);
            REQUIRE(lwcond_wait(me, not_empty, 0) == CELL_OK);
            woke++;
            REQUIRE(lwmutex_unlock(me, m) == CELL_OK);
        });
        REQUIRE(eventually([&] { return queued<LwCond>(*f.kernel, not_empty.queue()) == static_cast<usize>(i + 1); }));
    }
    REQUIRE(lwmutex_lock(c, m, 0) == CELL_OK);
    REQUIRE(lwcond_signal_all(c, not_empty) == CELL_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(woke.load() == 0); // they need the lwmutex we hold
    REQUIRE(lwmutex_unlock(c, m) == CELL_OK);
    for (auto& t : waiters) t.join();
    CHECK(woke.load() == 4);
    CHECK(m.owner() == kFree);
}
