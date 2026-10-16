// The waker goes on first, for every way one PPU thread wakes another and for thread start (D-1041; D-999 for event
// queues): a contract matrix, each primitive against each priority relation. A thread woken by another PPU thread, of
// the same or a lower priority, goes back to guest code only once its waker has begun its next call into lv2 (or
// waits there, or is gone; 10 ms at most), as lv2's scheduler leaves the waker running on the PS3; a more urgent one
// comes back at once. The waker here is a host thread the host does not run (it sleeps) between its wake and its next
// call. A light kind (the semaphore by default, PSPOCKET_LV2_WAKER_LIGHT) waits only until the waker is back from the
// call that woke. PSPOCKET_WAKER_MATRIX_ROUNDS=N runs the matrix N times (default 2). Fails with
// PSPOCKET_LV2_SENDER_FIRST=0.

#include "lv2_test_util.hpp"

#include <cstdlib>
#include <format>
#include <functional>

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

using namespace std::chrono_literals;

constexpr u64 kJoinable = 1;
constexpr u32 kAndClear = SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR;

u32 out_id(Fixture& f, Caller&, const std::function<s32(u32 out)>& make)
{
    const u32 out = f.alloc(4);
    THREAD_REQUIRE(make(out) == CELL_OK);
    return f.r32(out);
}

// One primitive: what the waker does before the woken thread waits (it runs on the waker), the wait, the wake, what
// the woken thread does after (its cleanup), and the wait's answer.
struct Primitive {
    std::string name;
    WaitKind kind;
    std::function<void(Caller& waker)> prepare = [](Caller&) {};
    std::function<s32(Caller& woken)> wait;
    std::function<void(Caller& waker)> wake;
    std::function<void(Caller& woken)> after = [](Caller&) {};
    s32 answer = CELL_OK;
    bool light = false; // held until the waker is back from its call only (PSPOCKET_LV2_WAKER_LIGHT's default)
};

std::vector<Primitive> primitives(Fixture& f, Caller& c)
{
    std::vector<Primitive> list;
    const u32 io = f.alloc(64);
    {
        const u32 m = out_id(f, c, [&](u32 o) { return c.call(kMutexCreate, o, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)); });
        list.push_back({"mutex handed over at unlock", WaitKind::Mutex,
            [m](Caller& w) { THREAD_REQUIRE(w.call(kMutexLock, m, 0) == CELL_OK); },
            [m](Caller& r) { return r.call(kMutexLock, m, 0); },
            [m](Caller& w) { THREAD_REQUIRE(w.call(kMutexUnlock, m) == CELL_OK); },
            [m](Caller& r) { THREAD_REQUIRE(r.call(kMutexUnlock, m) == CELL_OK); }});
    }
    {
        const u32 m = out_id(f, c, [&](u32 o) { return c.call(kMutexCreate, o, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)); });
        const u32 cv = out_id(f, c, [&](u32 o) { return c.call(kCondCreate, o, m, cond_attr(f)); });
        list.push_back({"cond signalled", WaitKind::Cond, [](Caller&) {},
            [m, cv](Caller& r) {
                THREAD_REQUIRE(r.call(kMutexLock, m, 0) == CELL_OK);
                return r.call(kCondWait, cv, 0);
            },
            [cv](Caller& w) { THREAD_REQUIRE(w.call(kCondSignal, cv) == CELL_OK); },
            [m](Caller& r) { THREAD_REQUIRE(r.call(kMutexUnlock, m) == CELL_OK); }});
    }
    {
        const u32 rw = out_id(f, c, [&](u32 o) { return c.call(kRwCreate, o, sync_attr(f, SYS_SYNC_FIFO)); });
        list.push_back({"rwlock readers after wunlock", WaitKind::RwLockRead,
            [rw](Caller& w) { THREAD_REQUIRE(w.call(kRwWLock, rw, 0) == CELL_OK); },
            [rw](Caller& r) { return r.call(kRwRLock, rw, 0); },
            [rw](Caller& w) { THREAD_REQUIRE(w.call(kRwWUnlock, rw) == CELL_OK); },
            [rw](Caller& r) { THREAD_REQUIRE(r.call(kRwRUnlock, rw) == CELL_OK); }});
    }
    {
        const u32 rw = out_id(f, c, [&](u32 o) { return c.call(kRwCreate, o, sync_attr(f, SYS_SYNC_FIFO)); });
        list.push_back({"rwlock writer after runlock", WaitKind::RwLockWrite,
            [rw](Caller& w) { THREAD_REQUIRE(w.call(kRwRLock, rw, 0) == CELL_OK); },
            [rw](Caller& r) { return r.call(kRwWLock, rw, 0); },
            [rw](Caller& w) { THREAD_REQUIRE(w.call(kRwRUnlock, rw) == CELL_OK); },
            [rw](Caller& r) { THREAD_REQUIRE(r.call(kRwWUnlock, rw) == CELL_OK); }});
    }
    {
        const u32 s = out_id(f, c, [&](u32 o) { return c.call(kSemCreate, o, sync_attr(f, SYS_SYNC_FIFO), 0, 4); });
        list.push_back({"semaphore posted", WaitKind::Semaphore, [](Caller&) {},
            [s](Caller& r) { return r.call(kSemWait, s, 0); },
            [s](Caller& w) { THREAD_REQUIRE(w.call(kSemPost, s, 1) == CELL_OK); }, [](Caller&) {}, CELL_OK,
            std::getenv("PSPOCKET_LV2_WAKER_LIGHT") == nullptr});
    }
    {
        const u32 l = out_id(f, c, [&](u32 o) { return c.call(kLwMutexCreate, o, SYS_SYNC_FIFO, f.alloc(24, 8), 0x80000001, 0); });
        list.push_back({"lwmutex handed over at unlock", WaitKind::LwMutex, [](Caller&) {},
            [l](Caller& r) { return r.call(kLwMutexLock, l, 0); },
            [l](Caller& w) { THREAD_REQUIRE(w.call(kLwMutexUnlock, l) == CELL_OK); }});
    }
    for (const u32 mode : {2u, 3u}) {
        const u32 l = out_id(f, c, [&](u32 o) { return c.call(kLwMutexCreate, o, SYS_SYNC_FIFO, f.alloc(24, 8), 0x80000001, 0); });
        const u32 cv = out_id(f, c, [&](u32 o) { return c.call(kLwCondCreate, o, l, f.alloc(8, 8), 0); });
        list.push_back({std::format("lwcond signalled (mode {})", mode), WaitKind::LwCond, [](Caller&) {},
            [l, cv](Caller& r) { return r.call(kLwCondQueueWait, cv, l, 0); },
            [l, cv, mode](Caller& w) { THREAD_REQUIRE(w.call(kLwCondSignal, cv, l, 0xFFFFFFFFull, mode) == CELL_OK); },
            [](Caller&) {}, mode == 2 ? rc(CELL_EBUSY) : CELL_OK});
    }
    {
        const u32 e = out_id(f, c, [&](u32 o) {
            return c.call(kFlagCreate, o, sync_attr(f, SYS_SYNC_FIFO, SYS_SYNC_WAITER_MULTIPLE), 0);
        });
        list.push_back({"event flag set", WaitKind::EventFlag, [](Caller&) {},
            [e, io](Caller& r) { return r.call(kFlagWait, e, 1, kAndClear, io, 0); },
            [e](Caller& w) { THREAD_REQUIRE(w.call(kFlagSet, e, 1) == CELL_OK); }});
    }
    {
        const u32 q = out_id(f, c, [&](u32 o) { return c.call(kQueueCreate, o, queue_attr(f, SYS_SYNC_FIFO), 0, 4); });
        const u32 p = out_id(f, c, [&](u32 o) { return c.call(kPortCreate, o, SYS_EVENT_PORT_LOCAL, 0); });
        THREAD_REQUIRE(c.call(kPortConnectLocal, p, q) == CELL_OK);
        list.push_back({"event sent", WaitKind::EventQueue, [](Caller&) {},
            [q](Caller& r) { return r.call(kQueueReceive, q, 0, 0); },
            [p](Caller& w) { THREAD_REQUIRE(w.call(kPortSend, p, 1, 2, 3) == CELL_OK); }});
    }
    return list;
}

int rounds()
{
    const char* v = std::getenv("PSPOCKET_WAKER_MATRIX_ROUNDS");
    return v ? std::max(1, std::atoi(v)) : 2;
}

} // namespace

TEST_CASE("lv2: the waking PPU thread goes on first, every primitive and priority (D-1041)", "[lv2][waker]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const SenderDeferrals before = sender_deferrals();
    u64 cases = 0;
    for (int round = 0; round < rounds(); round++) {
        for (const Primitive& p : primitives(f, c)) {
            // The woken thread's priority against the waker's 1000: the same, lower, more urgent.
            for (const s32 prio : {1000, 1500, 500}) {
                const bool urgent = prio < 1000 || p.light; // a light kind's woken thread does not wait for the sleep
                INFO(std::format("round {}: {}, the woken thread's priority {}", round, p.name, prio));
                std::atomic<bool> prepared{false}, past{false}, saw_past{false}, done{false};
                std::atomic<PpuThread*> woken{nullptr};
                std::atomic<s32> answer{-1};
                std::thread waker([&] {
                    Caller me(*f.kernel, "waker", 1000);
                    p.prepare(me);
                    prepared = true;
                    THREAD_REQUIRE(eventually([&] {
                        PpuThread* t = woken.load();
                        return t && t->waiter.waiting_on().kind == p.kind;
                    }));
                    p.wake(me);
                    std::this_thread::sleep_for(prio < 1000 || p.light ? 200ms : 3ms); // not running: the woken waits
                    past = true;
                    me.call(kGetPid); // its next call
                });
                std::thread receiver([&] {
                    THREAD_REQUIRE(eventually([&] { return prepared.load(); }));
                    Caller me(*f.kernel, "woken", prio);
                    woken = me.thread;
                    answer = p.wait(me);
                    saw_past = past.load();
                    p.after(me);
                    done = true;
                });
                receiver.join();
                waker.join();
                CHECK(answer.load() == p.answer);
                CHECK(saw_past.load() == !urgent);
                cases++;
            }
        }
    }
    const SenderDeferrals d = sender_deferrals();
    CHECK(d.deferred - before.deferred >= cases * 2 / 3 - 2 * static_cast<u64>(rounds())); // the same and the lower
    for (const WaitKind k : {WaitKind::Mutex, WaitKind::Cond, WaitKind::RwLockRead, WaitKind::RwLockWrite,
             WaitKind::Semaphore, WaitKind::LwMutex, WaitKind::LwCond, WaitKind::EventFlag, WaitKind::EventQueue}) {
        INFO(wait_kind_name(k));
        CHECK(d.kind_deferred[static_cast<usize>(k)] > before.kind_deferred[static_cast<usize>(k)]);
    }
}

// A wake that wakes nobody costs nothing: no waiter, no mark, no wait.
TEST_CASE("lv2: a post, unlock or set that wakes nobody leaves no wait (D-1041)", "[lv2][waker]")
{
    Fixture f;
    Caller c(*f.kernel, "main");
    const u32 s = out_id(f, c, [&](u32 o) { return c.call(kSemCreate, o, sync_attr(f, SYS_SYNC_FIFO), 0, 1000); });
    const u32 m = out_id(f, c, [&](u32 o) { return c.call(kMutexCreate, o, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)); });
    const SenderDeferrals before = sender_deferrals();
    std::thread other([&] {
        Caller me(*f.kernel, "other", 1000);
        for (int i = 0; i < 100; i++) {
            THREAD_REQUIRE(me.call(kSemPost, s, 1) == CELL_OK);
            THREAD_REQUIRE(me.call(kMutexLock, m, 0) == CELL_OK);
            THREAD_REQUIRE(me.call(kMutexUnlock, m) == CELL_OK);
        }
    });
    other.join();
    for (int i = 0; i < 100; i++) REQUIRE(c.call(kSemWait, s, 0) == CELL_OK); // all posted before: no wait
    CHECK(sender_deferrals().deferred == before.deferred);
    CHECK(c.thread->waiter.waker.thread == 0);
}

// A thread joining one that exits comes back once the exiting thread is gone (its host thread ended), and a new thread
// of the same or a lower priority runs once its creator has begun its next call; a more urgent one at once.
TEST_CASE("lv2: thread start and join let the other thread go on first (D-1041)", "[lv2][waker][thread]")
{
    HostEntries entries;
    Fixture f{entries.config()};
    Caller main(*f.kernel, "main");
    std::atomic<bool> past{false};
    std::atomic<int> saw{-1};
    entries.add(0x50000, [&](PPUContext& ctx) {
        saw = past.load() ? 1 : 0;
        syscall(ctx, kThreadExit, 0);
    });
    const auto create = [&](s32 prio) {
        const u32 opd = f.alloc(8);
        f.w32(opd, 0x50000);
        const u32 param = f.alloc(8);
        f.w32(param, opd);
        const u32 idp = f.alloc(8);
        REQUIRE(main.call(kThreadCreate, idp, param, 0, 0, prio, 0x4000, kJoinable, 0) == CELL_OK);
        return static_cast<u32>(f.r64(idp));
    };
    for (int round = 0; round < rounds(); round++) {
        for (const s32 prio : {1000, 1500, 500}) {
            INFO(std::format("round {}: a new thread of priority {}", round, prio));
            past = false;
            saw = -1;
            const u32 id = create(prio);
            const SenderDeferrals before = sender_deferrals();
            REQUIRE(main.call(kThreadStart, id) == CELL_OK);
            std::this_thread::sleep_for(prio < 1000 ? 200ms : 3ms); // the creator does not run
            past = true;
            const u32 out = f.alloc(8);
            REQUIRE(main.call(kThreadJoin, id, out) == CELL_OK);
            CHECK(saw.load() == (prio < 1000 ? 0 : 1));
            const SenderDeferrals d = sender_deferrals();
            if (prio >= 1000) {
                CHECK(d.kind_deferred[kWakeStart] > before.kind_deferred[kWakeStart]);
            }
        }
    }
    const SenderDeferrals d = sender_deferrals();
    CHECK(d.kind_deferred[static_cast<usize>(WaitKind::Join)] >= 1); // the joiner waited for the exiting thread
}
