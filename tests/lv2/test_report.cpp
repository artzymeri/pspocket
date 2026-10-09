// Thread and wait reports (lv2/report.hpp): a thread blocked on each kind of object appears with the object, its
// state and the syscall; sleeping, joining, not started and exited threads; guest backtraces from the stack back
// chain, and walks that stop at garbage.

#include <format>

#include "lv2_test_util.hpp"
#include "pspocket/lv2/report.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using State = ThreadSnapshot::State;

namespace {

struct ReportFixture {
    HostEntries entries;
    Fixture f{entries.config()};
    Caller main{*f.kernel, "main"};

    // A kernel thread (guest stack, own context) running the host function registered for `entry`.
    u32 spawn(u32 entry, std::string name, bool start = true)
    {
        ThreadParams p;
        p.name = std::move(name);
        p.entry = entry;
        p.stack_size = 0x4000;
        u32 id = 0;
        REQUIRE(f.kernel->create_thread(p, id) == CELL_OK);
        if (start) REQUIRE(f.kernel->start_thread(id) == CELL_OK);
        return id;
    }

    // Blocks until thread `id` is parked on something of kind `kind`.
    bool parked(u32 id, WaitKind kind)
    {
        return eventually([&] {
            const auto t = f.kernel->thread(id);
            return t && t->waiter.waiting_on().kind == kind;
        });
    }

    u32 create(u32 number, u32 attr, u64 a = 0, u64 b = 0)
    {
        const u32 out = f.alloc(4);
        REQUIRE(main.call(number, out, attr, a, b) == CELL_OK);
        return f.r32(out);
    }
};

const ThreadSnapshot& find(const std::vector<ThreadSnapshot>& threads, u32 id)
{
    const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadSnapshot& t) { return t.id == id; });
    REQUIRE(it != threads.end());
    return *it;
}

constexpr u64 name8(const char (&s)[5]) // a 4-character object name as the guest's name[8] reads (big-endian)
{
    u64 v = 0;
    for (int i = 0; i < 4; i++) v |= u64{static_cast<u8>(s[i])} << (56 - 8 * i);
    return v;
}

} // namespace

TEST_CASE("thread report: a receiver blocked on an event queue names the syscall, the queue and its state",
    "[lv2][report]")
{
    ReportFixture t;
    const u32 attr = queue_attr(t.f, SYS_SYNC_FIFO);
    t.f.w64(attr + 8, name8("evq0"));
    const u32 q = t.create(kQueueCreate, attr, 0, 16);

    std::atomic<u32> receiver_id{0};
    std::thread receiver([&] {
        Caller me(*t.f.kernel, "receiver", 700);
        receiver_id = me.id();
        THREAD_CHECK(me.call(kQueueReceive, q, 0, 0) == CELL_OK);
    });
    REQUIRE(eventually([&] { return receiver_id != 0; }));
    REQUIRE(t.parked(receiver_id, WaitKind::EventQueue));

    const auto threads = snapshot_threads(*t.f.kernel);
    REQUIRE(threads.size() == 2);
    CHECK(threads[0].id < threads[1].id); // sorted by ID
    const ThreadSnapshot& r = find(threads, receiver_id);
    CHECK(r.name == "receiver");
    CHECK(r.priority == 700);
    CHECK(r.state == State::Blocked);
    CHECK(r.wait.kind == WaitKind::EventQueue);
    CHECK(r.wait.id == q);
    CHECK(r.syscall == kQueueReceive);
    CHECK(r.waited >= 0ns);
    CHECK(r.remaining < 0ns); // no timeout
    CHECK(r.waits_on == std::format("event queue {:#x} \"evq0\": 0 of 16 events queued, 1 waiter", q));
    CHECK(r.attached);
    CHECK_FALSE(r.registers); // a host thread with an lv2 identity: no guest stack to walk

    const ThreadSnapshot& m = find(threads, t.main.id());
    CHECK(m.state == State::Running);
    CHECK(m.current);

    const std::string report = format_report(*t.f.kernel, {.title = "test", .describe_address = {}});
    CHECK_THAT(report, ContainsSubstring("==== thread report: test ====\n2 PPU threads, 1 blocked, 1 running\n"));
    CHECK_THAT(report, ContainsSubstring(std::format("{:#010x}  receiver                     prio  700  blocked      "
                                                     "in sys_event_queue_receive (syscall 130) for ",
                                           receiver_id.load())));
    CHECK_THAT(report, ContainsSubstring(std::format("\n    on event queue {:#x} \"evq0\": 0 of 16 events queued", q)));
    CHECK_THAT(report, ContainsSubstring("RSX: not created"));

    REQUIRE(t.f.kernel->event_queue(q)->send(Event{1, 2, 3, 4}) == CELL_OK);
    receiver.join();
    CHECK(snapshot_threads(*t.f.kernel).size() == 1); // the receiver detached
}

TEST_CASE("thread report: an event flag waiter shows the bits it wants against the pattern, and its timeout",
    "[lv2][report]")
{
    ReportFixture t;
    const u32 attr = sync_attr(t.f, SYS_SYNC_FIFO, SYS_SYNC_WAITER_MULTIPLE);
    t.f.w64(attr + 24, name8("flag"));
    const u32 flag = t.create(kFlagCreate, attr, 0x2);

    std::atomic<u32> waiter_id{0};
    std::thread waiter([&] {
        Caller me(*t.f.kernel, "flag waiter");
        waiter_id = me.id();
        const u32 result = t.f.alloc(8);
        THREAD_CHECK(me.call(kFlagWait, flag, 0x5, SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR, result,
                         10'000'000) == CELL_OK);
    });
    REQUIRE(eventually([&] { return waiter_id != 0; }));
    REQUIRE(t.parked(waiter_id, WaitKind::EventFlag));

    const ThreadSnapshot w = find(snapshot_threads(*t.f.kernel), waiter_id);
    CHECK(w.state == State::Blocked);
    CHECK(w.wait.id == flag);
    CHECK(w.syscall == kFlagWait);
    CHECK(w.waits_on == std::format("event flag {:#x} \"flag\": wants 0x5 (AND, clear), pattern 0x2, 1 waiter", flag));
    CHECK(w.remaining > 0ns);
    CHECK(w.remaining <= 10s);
    CHECK_THAT(format_report(*t.f.kernel), ContainsSubstring(", times out in "));

    REQUIRE(t.main.call(kFlagSet, flag, 0x5) == CELL_OK);
    waiter.join();
}

TEST_CASE("thread report: an lwmutex sleeper shows the guest's sys_lwmutex_t owner", "[lv2][report]")
{
    ReportFixture t;
    const u32 control = t.f.alloc(24, 8); // sys_lwmutex_t: owner, waiter count, attribute, recursion, sleep queue
    t.f.w32(control, t.main.id());
    t.f.w32(control + 4, 1);
    const u32 out = t.f.alloc(4);
    REQUIRE(t.main.call(kLwMutexCreate, out, SYS_SYNC_PRIORITY, control, -1, name8("lwm0")) == CELL_OK);
    const u32 lw = t.f.r32(out);

    std::atomic<u32> sleeper_id{0};
    std::thread sleeper([&] {
        Caller me(*t.f.kernel, "sleeper");
        sleeper_id = me.id();
        THREAD_CHECK(me.call(kLwMutexLock, lw, 0) == CELL_OK);
    });
    REQUIRE(eventually([&] { return sleeper_id != 0; }));
    REQUIRE(t.parked(sleeper_id, WaitKind::LwMutex));

    const ThreadSnapshot s = find(snapshot_threads(*t.f.kernel), sleeper_id);
    CHECK(s.wait.id == lw);
    CHECK(s.syscall == kLwMutexLock);
    CHECK(s.waits_on == std::format("lwmutex {:#x} \"lwm0\" (sys_lwmutex_t {:#010x}: owner {:#x} \"main\", waiter "
                                    "count 1), 1 sleeper in the kernel",
                            lw, control, t.main.id()));

    t.f.w32(control, 0xFFFFFFFD);
    CHECK_THAT(find(snapshot_threads(*t.f.kernel), sleeper_id).waits_on,
        ContainsSubstring("owner handed to a woken waiter"));

    REQUIRE(t.main.call(kLwMutexUnlock, lw) == CELL_OK);
    sleeper.join();
}

TEST_CASE("thread report: mutex owners, and a signalled cond waiter re-taking its mutex", "[lv2][report]")
{
    ReportFixture t;
    const u32 mutex = t.create(kMutexCreate, mutex_attr(t.f, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE));
    const u32 cattr = cond_attr(t.f);
    t.f.w64(cattr + 16, name8("cond"));
    const u32 out = t.f.alloc(4);
    REQUIRE(t.main.call(kCondCreate, out, mutex, cattr) == CELL_OK);
    const u32 cond = t.f.r32(out);

    // A locker blocked on the mutex main owns.
    REQUIRE(t.main.call(kMutexLock, mutex, 0) == CELL_OK);
    std::atomic<u32> locker_id{0};
    std::thread locker([&] {
        Caller me(*t.f.kernel, "locker");
        locker_id = me.id();
        THREAD_CHECK(me.call(kMutexLock, mutex, 0) == CELL_OK);
        THREAD_CHECK(me.call(kMutexUnlock, mutex) == CELL_OK);
    });
    REQUIRE(eventually([&] { return locker_id != 0; }));
    REQUIRE(t.parked(locker_id, WaitKind::Mutex));
    CHECK(find(snapshot_threads(*t.f.kernel), locker_id).waits_on ==
        std::format("mutex {:#x} \"mutex\": owner {:#x} \"main\", 1 waiter", mutex, t.main.id()));
    REQUIRE(t.main.call(kMutexUnlock, mutex) == CELL_OK);
    locker.join();

    // A cond waiter (the mutex free meanwhile), then signalled while main holds the mutex.
    std::atomic<u32> waiter_id{0};
    std::thread waiter([&] {
        Caller me(*t.f.kernel, "cond waiter");
        waiter_id = me.id();
        THREAD_REQUIRE(me.call(kMutexLock, mutex, 0) == CELL_OK);
        THREAD_CHECK(me.call(kCondWait, cond, 0) == CELL_OK);
        THREAD_CHECK(me.call(kMutexUnlock, mutex) == CELL_OK);
    });
    REQUIRE(eventually([&] { return waiter_id != 0; }));
    REQUIRE(t.parked(waiter_id, WaitKind::Cond));
    CHECK(find(snapshot_threads(*t.f.kernel), waiter_id).waits_on ==
        std::format("cond {:#x} \"cond\" with mutex {:#x} \"mutex\" (owner none): 1 waiter", cond, mutex));

    REQUIRE(t.main.call(kMutexLock, mutex, 0) == CELL_OK);
    REQUIRE(t.main.call(kCondSignal, cond) == CELL_OK);
    const ThreadSnapshot w = find(snapshot_threads(*t.f.kernel), waiter_id);
    CHECK(w.wait.kind == WaitKind::Cond);
    CHECK(w.waits_on ==
        std::format("cond {:#x} \"cond\": woken, re-taking mutex {:#x} \"mutex\" (owner {:#x} \"main\")", cond, mutex,
            t.main.id()));
    REQUIRE(t.main.call(kMutexUnlock, mutex) == CELL_OK);
    waiter.join();
}

TEST_CASE("thread report: sleeping threads, joiners, threads not started and exited ones", "[lv2][report]")
{
    ReportFixture t;

    std::atomic<u32> sleeper_id{0};
    std::thread sleeper([&] {
        Caller me(*t.f.kernel, "sleeper");
        sleeper_id = me.id();
        THREAD_CHECK(me.call(kUsleep, 300'000) == CELL_OK);
    });
    REQUIRE(eventually([&] { return sleeper_id != 0; }));
    REQUIRE(t.parked(sleeper_id, WaitKind::Sleep));
    {
        const ThreadSnapshot s = find(snapshot_threads(*t.f.kernel), sleeper_id);
        CHECK(s.state == State::Blocked);
        CHECK(s.syscall == kUsleep);
        CHECK(s.waits_on.empty());
        CHECK(s.remaining > 0ns);
        CHECK(s.remaining <= 300ms);
        CHECK_THAT(format_report(*t.f.kernel), ContainsSubstring("sleeping     in sys_timer_usleep (syscall 141), "));
    }
    sleeper.join();

    // A kernel thread blocked on a semaphore, and a joiner waiting for it.
    const u32 sem = t.create(kSemCreate, sync_attr(t.f, SYS_SYNC_FIFO), 0, 1);
    t.entries.add(0x10000, [&](PPUContext& ctx) {
        syscall(ctx, kSemWait, sem, 0);
        syscall(ctx, kThreadExit, 7);
    });
    const u32 worker = t.spawn(0x10000, "worker");
    REQUIRE(t.parked(worker, WaitKind::Semaphore));
    std::atomic<u32> joiner_id{0};
    std::thread joiner([&] {
        Caller me(*t.f.kernel, "joiner");
        joiner_id = me.id();
        const u32 status = t.f.alloc(8);
        THREAD_CHECK(me.call(kThreadJoin, worker, status) == CELL_OK);
        THREAD_CHECK(t.f.r64(status) == 7);
    });
    REQUIRE(eventually([&] { return joiner_id != 0; }));
    REQUIRE(t.parked(joiner_id, WaitKind::Join));

    // A thread never started, and one that exited without being joined.
    const u32 idle = t.spawn(0x30000, "idle", false);
    t.entries.add(0x40000, [](PPUContext& ctx) { syscall(ctx, kThreadExit, 0x42); });
    const u32 quitter = t.spawn(0x40000, "quitter");
    REQUIRE(eventually([&] { return find(snapshot_threads(*t.f.kernel), quitter).state == State::Exited; }));

    const auto threads = snapshot_threads(*t.f.kernel);
    const ThreadSnapshot& w = find(threads, worker);
    CHECK(w.state == State::Blocked);
    CHECK(w.waits_on == std::format("semaphore {:#x}: count 0 (max 1), 1 waiter", sem));
    CHECK(w.registers); // a kernel thread: exact registers, at its entry (the host function is its whole body)
    CHECK_FALSE(w.racy);
    CHECK(w.cia == 0x10000);
    CHECK(w.backtrace == std::vector<u32>{0x10000}); // LR 0 and an empty back chain: nothing above the entry
    CHECK(find(threads, joiner_id).waits_on == std::format("thread {:#x} \"worker\" to exit", worker));
    const ThreadSnapshot& i = find(threads, idle);
    CHECK(i.state == State::NotStarted);
    CHECK(i.cia == 0x30000);
    const ThreadSnapshot& q = find(threads, quitter);
    CHECK(q.exit_status == 0x42);
    CHECK_FALSE(q.registers);

    const std::string report = format_report(*t.f.kernel);
    CHECK_THAT(report, ContainsSubstring("5 PPU threads, 2 blocked, 1 running, 1 not started, 1 exited\n"));
    CHECK_THAT(report, ContainsSubstring("not started  entry 0x00030000\n"));
    CHECK_THAT(report, ContainsSubstring("exited       exit status 0x42, not joined yet\n"));
    CHECK_THAT(report, ContainsSubstring("blocked      in sys_ppu_thread_join (syscall 44) for "));

    REQUIRE(t.main.call(kSemPost, sem, 1) == CELL_OK);
    joiner.join();
    u64 status = 0;
    const u32 out = t.f.alloc(8);
    REQUIRE(t.main.call(kThreadJoin, quitter, out) == CELL_OK);
    status = t.f.r64(out);
    CHECK(status == 0x42);
}

TEST_CASE("thread report: a blocked kernel thread's backtrace follows its stack's back chain", "[lv2][report]")
{
    ReportFixture t;
    const u32 q = t.create(kQueueCreate, queue_attr(t.f, SYS_SYNC_FIFO), 0, 4);
    // Code addresses only have to be committed guest memory: the fixture's scratch area.
    constexpr u32 kSc = 0x00100100, kRetC = 0x00100200, kRetB = 0x00100300, kRetA = 0x00100400;
    std::atomic<u32> frame_c{0};
    t.entries.add(0x20000, [&](PPUContext& ctx) {
        // The frames compiled code leaves: entry A calls B calls C, each callee saving its return address in its
        // caller's frame (+16) and pointing its own frame at the caller's; C calls a syscall wrapper that has no frame.
        const u32 top = static_cast<u32>(ctx.gpr[1]); // stack top - 0x70; [top] and [top + 16] are 0
        const u32 a = top - 0x80, b = a - 0x80, c = b - 0x80;
        t.f.w64(a, top);
        t.f.w64(b, a);
        t.f.w64(a + 16, kRetA);
        t.f.w64(c, b);
        t.f.w64(b + 16, kRetB);
        ctx.gpr[1] = c;
        ctx.lr = kRetC;
        ctx.cia = kSc;
        frame_c = c;
        syscall(ctx, kQueueReceive, q, 0, 0);
        syscall(ctx, kThreadExit, 0);
    });
    const u32 id = t.spawn(0x20000, "walker");
    REQUIRE(t.parked(id, WaitKind::EventQueue));

    const ThreadSnapshot w = find(snapshot_threads(*t.f.kernel), id);
    CHECK(w.registers);
    CHECK_FALSE(w.racy);
    CHECK(w.cia == kSc);
    CHECK(w.lr == kRetC);
    CHECK(w.sp == frame_c);
    CHECK(w.backtrace == std::vector<u32>{kSc, kRetC, kRetB, kRetA});
    CHECK(find(snapshot_threads(*t.f.kernel, 3), id).backtrace == std::vector<u32>{kSc, kRetC, kRetB});

    ReportOptions options;
    options.describe_address = [](u32 addr) {
        return addr == kSc ? std::string("sys_event_queue_receive stub") : std::string();
    };
    CHECK_THAT(format_report(*t.f.kernel, options),
        ContainsSubstring(std::format("    at 0x00100100 [sys_event_queue_receive stub] <- 0x00100200 <- 0x00100300 <- "
                                      "0x00100400  (sp {:#010x})\n",
            frame_c.load())));

    REQUIRE(t.f.kernel->event_queue(q)->send(Event{}) == CELL_OK);
    REQUIRE(eventually([&] { return find(snapshot_threads(*t.f.kernel), id).state == State::Exited; }));
    const u32 out = t.f.alloc(8);
    REQUIRE(t.main.call(kThreadJoin, id, out) == CELL_OK);
}

TEST_CASE("guest_backtrace stops at the first word that is not a frame or a return address", "[lv2][report]")
{
    Fixture f;
    constexpr u32 kSc = 0x00100100, kLr = 0x00100200, kRet1 = 0x00100300, kRet2 = 0x00100400;
    const u32 lo = f.alloc(0x1000, 0x100), hi = lo + 0x1000;
    const u32 sp = lo + 0x100, f1 = sp + 0x80, f2 = f1 + 0x80;
    f.w64(sp, f1);
    f.w64(f1, f2);
    f.w64(f1 + 16, kRet1); // the innermost function's return address, saved in its caller's frame
    f.w64(f2 + 16, kRet2);
    const GuestMemory& mem = f.mem;
    auto walk = [&](u64 lr, u64 from = 0, usize max = 12, bool bounded = true) {
        return guest_backtrace(mem, kSc, lr, from ? from : sp, bounded ? lo : 0, bounded ? hi : 0, max);
    };

    CHECK(walk(kLr) == std::vector<u32>{kSc, kLr, kRet1, kRet2});
    CHECK(walk(kRet1) == std::vector<u32>{kSc, kRet1, kRet2}); // the saved LR is listed once
    CHECK(walk(0) == std::vector<u32>{kSc, kRet1, kRet2});     // no LR: the back chain only
    CHECK(walk(kLr + 2) == std::vector<u32>{kSc, kRet1, kRet2}); // a misaligned LR is not a code address
    CHECK(walk(kLr, 0, 2) == std::vector<u32>{kSc, kLr});
    CHECK(walk(kLr, 0, 0).empty());

    // Stack pointers that are not frames: unmapped, misaligned, above 4 GB.
    CHECK(walk(kLr, 0x80000000) == std::vector<u32>{kSc, kLr});
    CHECK(walk(kLr, sp + 8) == std::vector<u32>{kSc, kLr});
    CHECK(walk(kLr, 0x1'0000'0000ull | sp) == std::vector<u32>{kSc, kLr});

    // A chain that loops, or climbs out of the stack, ends there.
    f.w64(f2, f1);
    CHECK(walk(kLr) == std::vector<u32>{kSc, kLr, kRet1, kRet2});
    const u32 outside = hi + 0x100;
    f.w64(f1, outside);
    f.w64(outside + 16, kRet2);
    CHECK(walk(kLr) == std::vector<u32>{kSc, kLr, kRet1});
    CHECK(walk(kLr, 0, 12, false) == std::vector<u32>{kSc, kLr, kRet1, kRet2}); // no bounds: any committed memory
    f.w64(f1, 0x1'0000'0000ull | f2); // a back chain above 4 GB
    CHECK(walk(kLr, 0, 12, false) == std::vector<u32>{kSc, kLr, kRet1});

    // Return addresses that are not code: unmapped, misaligned.
    f.w64(f1, f2);
    f.w64(f2, 0);
    f.w64(f2 + 16, 0x80000000);
    CHECK(walk(kLr) == std::vector<u32>{kSc, kLr, kRet1});
    f.w64(f2 + 16, kRet2 + 1);
    CHECK(walk(kLr) == std::vector<u32>{kSc, kLr, kRet1});
}
