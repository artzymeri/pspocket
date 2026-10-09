// sys_rwlock and sys_semaphore through the dispatcher.

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

u32 make_rwlock(Fixture& f, Caller& c, u32 protocol = SYS_SYNC_PRIORITY)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kRwCreate, out, sync_attr(f, protocol)) == CELL_OK);
    return f.r32(out);
}

u32 make_sema(Fixture& f, Caller& c, s32 initial, s32 max, u32 protocol = SYS_SYNC_FIFO)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kSemCreate, out, sync_attr(f, protocol), initial, max) == CELL_OK);
    return f.r32(out);
}

} // namespace

TEST_CASE("lv2 rwlock: single-thread semantics and errors", "[lv2][rwlock]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kRwCreate, out, sync_attr(f, SYS_SYNC_PRIORITY_INHERIT)) == rc(CELL_EINVAL));
    const u32 rw = make_rwlock(f, c);

    CHECK(c.call(kRwRUnlock, rw) == rc(CELL_EPERM));
    CHECK(c.call(kRwRLock, rw, 0) == CELL_OK);
    CHECK(c.call(kRwTryRLock, rw) == CELL_OK); // readers are counted, not identified
    CHECK(c.call(kRwTryWLock, rw) == rc(CELL_EBUSY));
    CHECK(c.call(kRwWUnlock, rw) == rc(CELL_EPERM));
    CHECK(c.call(kRwDestroy, rw) == rc(CELL_EBUSY));
    CHECK(c.call(kRwRUnlock, rw) == CELL_OK);
    CHECK(c.call(kRwRUnlock, rw) == CELL_OK);
    CHECK(c.call(kRwRUnlock, rw) == rc(CELL_EPERM));

    CHECK(c.call(kRwWLock, rw, 0) == CELL_OK);
    CHECK(c.call(kRwWLock, rw, 0) == rc(CELL_EDEADLK));
    CHECK(c.call(kRwTryWLock, rw) == rc(CELL_EDEADLK));
    CHECK(c.call(kRwTryRLock, rw) == rc(CELL_EBUSY));
    std::thread([&] {
        Caller other(*f.kernel, "other");
        THREAD_CHECK(other.call(kRwWUnlock, rw) == rc(CELL_EPERM));
        THREAD_CHECK(other.call(kRwRLock, rw, 10000) == rc(CELL_ETIMEDOUT));
        THREAD_CHECK(other.call(kRwWLock, rw, 10000) == rc(CELL_ETIMEDOUT));
    }).join();
    CHECK(c.call(kRwWUnlock, rw) == CELL_OK);
    CHECK(c.call(kRwDestroy, rw) == CELL_OK);
    CHECK(c.call(kRwRLock, rw, 0) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 rwlock: readers share, writers are preferred", "[lv2][rwlock]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 rw = make_rwlock(f, c);

    // Four readers hold the lock at once.
    std::atomic<int> inside{0};
    std::atomic<bool> leave{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&] {
            Caller me(*f.kernel, "reader");
            THREAD_REQUIRE(me.call(kRwRLock, rw, 0) == CELL_OK);
            inside++;
            while (!leave.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            THREAD_REQUIRE(me.call(kRwRUnlock, rw) == CELL_OK);
        });
    }
    CHECK(eventually([&] { return inside.load() == 4; }));

    // A writer waits for them; a reader arriving after the writer queues behind it.
    std::vector<std::string> order;
    std::mutex order_mutex;
    std::thread writer([&] {
        Caller me(*f.kernel, "writer");
        THREAD_REQUIRE(me.call(kRwWLock, rw, 0) == CELL_OK);
        {
            std::lock_guard lock(order_mutex);
            order.push_back("writer");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        THREAD_REQUIRE(me.call(kRwWUnlock, rw) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<RwLock>(*f.kernel, rw) == 1; }));
    std::thread late([&] {
        Caller me(*f.kernel, "late reader");
        THREAD_REQUIRE(me.call(kRwRLock, rw, 0) == CELL_OK);
        {
            std::lock_guard lock(order_mutex);
            order.push_back("reader");
        }
        THREAD_REQUIRE(me.call(kRwRUnlock, rw) == CELL_OK);
    });
    REQUIRE(eventually([&] { return queued<RwLock>(*f.kernel, rw) == 2; }));
    CHECK(c.call(kRwTryRLock, rw) == rc(CELL_EBUSY)); // a writer is waiting
    leave = true;
    for (auto& t : readers) t.join();
    writer.join();
    late.join();
    CHECK(order == std::vector<std::string>{"writer", "reader"});
}

TEST_CASE("lv2 rwlock: a timed-out writer lets queued readers in", "[lv2][rwlock]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 rw = make_rwlock(f, c, SYS_SYNC_FIFO);
    REQUIRE(c.call(kRwRLock, rw, 0) == CELL_OK);
    std::atomic<bool> reader_in{false};
    std::thread writer([&] {
        Caller me(*f.kernel, "writer");
        THREAD_CHECK(me.call(kRwWLock, rw, 50000) == rc(CELL_ETIMEDOUT));
    });
    REQUIRE(eventually([&] { return queued<RwLock>(*f.kernel, rw) == 1; }));
    std::thread reader([&] {
        Caller me(*f.kernel, "reader");
        THREAD_REQUIRE(me.call(kRwRLock, rw, 0) == CELL_OK); // behind the writer until it gives up
        reader_in = true;
        THREAD_REQUIRE(me.call(kRwRUnlock, rw) == CELL_OK);
    });
    writer.join();
    reader.join();
    CHECK(reader_in.load());
    CHECK(c.call(kRwRUnlock, rw) == CELL_OK);
    CHECK(c.call(kRwDestroy, rw) == CELL_OK);
}

TEST_CASE("lv2 rwlock: readers never see a half-done write", "[lv2][rwlock][stress]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 rw = make_rwlock(f, c);
    const u32 data = f.alloc(16);
    std::atomic<int> violations{0};
    std::atomic<u64> writes{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < 6; i++) {
        pool.emplace_back([&, i] {
            Caller me(*f.kernel, i < 2 ? "writer" : "reader");
            for (int k = 0; k < 1500; k++) {
                if (i < 2) {
                    THREAD_REQUIRE(me.call(kRwWLock, rw, 0) == CELL_OK);
                    f.w64(data, f.r64(data) + 1);
                    f.w64(data + 8, f.r64(data + 8) + 1);
                    writes++;
                    THREAD_REQUIRE(me.call(kRwWUnlock, rw) == CELL_OK);
                } else {
                    THREAD_REQUIRE(me.call(kRwRLock, rw, 0) == CELL_OK);
                    if (f.r64(data) != f.r64(data + 8)) violations++;
                    THREAD_REQUIRE(me.call(kRwRUnlock, rw) == CELL_OK);
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    CHECK(violations.load() == 0);
    CHECK(f.r64(data) == writes.load());
}

TEST_CASE("lv2 semaphore: creation limits, counting and errors", "[lv2][semaphore]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_FIFO), 0, 0) == rc(CELL_EINVAL));
    CHECK(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_FIFO), 3, 2) == rc(CELL_EINVAL));
    CHECK(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_FIFO), -1, 2) == rc(CELL_EINVAL));
    CHECK(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_RETRY), 0, 2) == rc(CELL_EINVAL));

    const u32 s = make_sema(f, c, 2, 3);
    const u32 v = f.alloc(4);
    CHECK(c.call(kSemTryWait, s) == CELL_OK);
    CHECK(c.call(kSemWait, s, 0) == CELL_OK);
    CHECK(c.call(kSemTryWait, s) == rc(CELL_EBUSY));
    REQUIRE(c.call(kSemGetValue, s, v) == CELL_OK);
    CHECK(f.r32(v) == 0);
    CHECK(c.call(kSemPost, s, 3) == CELL_OK);
    CHECK(c.call(kSemPost, s, 1) == rc(CELL_EBUSY)); // would exceed max
    CHECK(c.call(kSemPost, s, 0) == rc(CELL_EINVAL));
    CHECK(c.call(kSemPost, s, -2) == rc(CELL_EINVAL));
    REQUIRE(c.call(kSemGetValue, s, v) == CELL_OK);
    CHECK(f.r32(v) == 3);
    CHECK(c.call(kSemGetValue, s, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kSemDestroy, s) == CELL_OK);
    CHECK(c.call(kSemPost, s, 1) == rc(CELL_ESRCH));
    CHECK(c.call(kSemGetValue, s, v) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 semaphore: blocking waits, multi-wake posts and timeouts", "[lv2][semaphore]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 s = make_sema(f, c, 0, 10);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.call(kSemWait, s, 15000) == rc(CELL_ETIMEDOUT));
    CHECK(elapsed_ms(t0) >= 14);

    std::atomic<int> passed{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < 3; i++) {
        pool.emplace_back([&] {
            Caller me(*f.kernel, "waiter");
            THREAD_REQUIRE(me.call(kSemWait, s, 0) == CELL_OK);
            passed++;
        });
    }
    REQUIRE(eventually([&] { return queued<Semaphore>(*f.kernel, s) == 3; }));
    const u32 v = f.alloc(4);
    REQUIRE(c.call(kSemGetValue, s, v) == CELL_OK);
    CHECK(f.r32(v) == 0); // waiters make the internal value negative; get_value reports 0
    CHECK(c.call(kSemDestroy, s) == rc(CELL_EBUSY));
    REQUIRE(c.call(kSemPost, s, 2) == CELL_OK);
    REQUIRE(eventually([&] { return passed.load() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(passed.load() == 2);
    REQUIRE(c.call(kSemPost, s, 5) == CELL_OK);
    for (auto& t : pool) t.join();
    REQUIRE(c.call(kSemGetValue, s, v) == CELL_OK);
    CHECK(f.r32(v) == 4);
}

TEST_CASE("lv2 semaphore: posts and waits balance under contention", "[lv2][semaphore][stress]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 s = make_sema(f, c, 0, 1000000, SYS_SYNC_PRIORITY);
    constexpr int kPerThread = 3000;
    std::vector<std::thread> pool;
    for (int i = 0; i < 4; i++) {
        pool.emplace_back([&] {
            Caller me(*f.kernel, "producer");
            for (int k = 0; k < kPerThread; k++) THREAD_REQUIRE(me.call(kSemPost, s, 1) == CELL_OK);
        });
        pool.emplace_back([&] {
            Caller me(*f.kernel, "consumer");
            for (int k = 0; k < kPerThread; k++) THREAD_REQUIRE(me.call(kSemWait, s, 0) == CELL_OK);
        });
    }
    for (auto& t : pool) t.join();
    const u32 v = f.alloc(4);
    REQUIRE(c.call(kSemGetValue, s, v) == CELL_OK);
    CHECK(f.r32(v) == 0);
    CHECK(c.call(kSemTryWait, s) == rc(CELL_EBUSY));
}
