// PPU threads: create/start/join/detach, entry state, stacks, priorities, and a guest program run by the PPU
// interpreter through the default executor.

#include <pthread.h>

#include "lv2_test_util.hpp"
#include "pspocket/core/memory_map.h"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u64 kJoinable = 1;
constexpr u32 kToc = 0x00ABC000;
constexpr u32 kTls = 0x00DEF000;

struct ThreadFixture {
    HostEntries entries;
    Fixture f{entries.config()};
    Caller main{*f.kernel, "main"};

    // _sys_ppu_thread_create through the dispatcher; returns the new thread's ID.
    s32 create(u32 entry, u64 arg, s32 prio, u64 stack, u64 flags, const char* name, u32& id)
    {
        const u32 opd = f.alloc(8);
        f.w32(opd, entry);
        f.w32(opd + 4, kToc);
        const u32 param = f.alloc(8);
        f.w32(param, opd);
        f.w32(param + 4, kTls);
        const u32 idp = f.alloc(8);
        const u32 namep = name ? f.cstr(name) : 0;
        const s32 r = main.call(kThreadCreate, idp, param, arg, 0x77, prio, stack, flags, namep);
        id = static_cast<u32>(f.r64(idp));
        return r;
    }

    u32 spawn(u32 entry, u64 arg = 0, u64 flags = kJoinable, s32 prio = 1000)
    {
        u32 id = 0;
        REQUIRE(create(entry, arg, prio, 0x4000, flags, "worker", id) == CELL_OK);
        REQUIRE(main.call(kThreadStart, id) == CELL_OK);
        return id;
    }

    bool finished(u32 id)
    {
        const auto th = f.kernel->thread(id);
        if (!th) return true;
        std::lock_guard lock(f.kernel->impl().join_mutex);
        return th->finished;
    }

    s32 join(u32 id, u64& status)
    {
        const u32 out = f.alloc(8);
        const s32 r = main.call(kThreadJoin, id, out);
        status = f.r64(out);
        return r;
    }
};

// Stack size of the calling thread (pthread_get_stacksize_np is Apple-only).
usize current_stack_size()
{
#if defined(__APPLE__)
    return pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t attr;
    usize size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstacksize(&attr, &size);
        pthread_attr_destroy(&attr);
    }
    return size;
#endif
}

} // namespace

TEST_CASE("lv2 threads: create, start and join run the entry with lv2's initial registers", "[lv2][thread]")
{
    ThreadFixture t;
    PPUContext seen{};
    usize host_stack = 0;
    t.entries.add(0x10000, [&](PPUContext& ctx) {
        seen = ctx;
        host_stack = current_stack_size();
        syscall(ctx, kThreadExit, ctx.gpr[3] + 1);
    });
    u32 id = 0;
    REQUIRE(t.create(0x10000, 41, 1500, 0x4000, kJoinable, "worker", id) == CELL_OK);
    CHECK(id == 0x01000001); // after the attached main thread
    CHECK(t.f.kernel->thread(id)->name == "worker");
    REQUIRE(t.main.call(kThreadStart, id) == CELL_OK);
    CHECK(t.main.call(kThreadStart, id) == rc(CELL_EBUSY));

    u64 status = 0;
    REQUIRE(t.join(id, status) == CELL_OK);
    CHECK(status == 42);
    CHECK(seen.gpr[3] == 41);
    CHECK(seen.gpr[4] == 0x77);
    CHECK(seen.gpr[2] == kToc);
    CHECK(seen.gpr[13] == kTls);
    CHECK(seen.lr == kThreadReturnAddress);
    CHECK(seen.cia == 0x10000);
    CHECK(seen.gpr[1] >= PS_MEM_STACK_BASE);
    CHECK(seen.gpr[1] < u64{PS_MEM_STACK_BASE} + PS_MEM_STACK_SIZE);
    CHECK((seen.gpr[1] + kStackStartOffset) % 0x4000 == 0); // stack_addr + stack_size - 0x70
    CHECK(host_stack >= (8u << 20)); // D-015

    // The thread is reaped by the join.
    CHECK(t.join(id, status) == rc(CELL_ESRCH));
    CHECK(t.f.kernel->thread(id) == nullptr);
}

TEST_CASE("lv2 threads: stack information, guard page and stack release", "[lv2][thread]")
{
    ThreadFixture t;
    u32 stack_addr = 0, stack_size = 0;
    bool committed = false, guard = true;
    t.entries.add(0x20000, [&](PPUContext& ctx) {
        const u32 sp = t.f.alloc(8);
        REQUIRE(static_cast<s32>(syscall(ctx, kThreadGetStackInfo, sp)) == CELL_OK);
        stack_addr = t.f.r32(sp);
        stack_size = t.f.r32(sp + 4);
        committed = t.f.mem.is_committed(stack_addr) && t.f.mem.is_committed(stack_addr + stack_size - 1);
        guard = t.f.mem.is_committed(stack_addr - 1);
        syscall(ctx, kThreadExit, 0);
    });
    u32 id = 0;
    REQUIRE(t.create(0x20000, 0, 1000, 0x1234, kJoinable, nullptr, id) == CELL_OK);
    REQUIRE(t.main.call(kThreadStart, id) == CELL_OK);
    u64 status = 0;
    REQUIRE(t.join(id, status) == CELL_OK);
    CHECK(stack_size == 0x2000); // 4 KB granularity (RPCS3)
    CHECK(stack_addr >= PS_MEM_STACK_BASE);
    CHECK(committed);
    CHECK_FALSE(guard); // an uncommitted host page below every stack
    CHECK_FALSE(t.f.mem.is_committed(stack_addr)); // released at exit
}

TEST_CASE("lv2 threads: returning from the entry function exits with r3", "[lv2][thread]")
{
    ThreadFixture t;
    LogCapture log;
    t.entries.add(0x30000, [](PPUContext& ctx) { ctx.gpr[3] = 7; });
    const u32 id = t.spawn(0x30000);
    u64 status = 0;
    REQUIRE(t.join(id, status) == CELL_OK);
    CHECK(status == 7);
    CHECK(log.contains("lv2", "returned from its entry function"));
}

TEST_CASE("lv2 threads: join and detach states", "[lv2][thread]")
{
    ThreadFixture t;
    std::atomic<bool> release{false};
    t.entries.add(0x40000, [&](PPUContext& ctx) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        syscall(ctx, kThreadExit, 5);
    });
    t.entries.add(0x41000, [](PPUContext& ctx) { syscall(ctx, kThreadExit, 9); });

    u64 status = 0;
    CHECK(t.join(t.main.id(), status) == rc(CELL_EDEADLK));
    CHECK(t.join(0x01000055, status) == rc(CELL_ESRCH));

    SECTION("a detached running thread can't be joined and cleans itself up")
    {
        const u32 id = t.spawn(0x40000);
        CHECK(t.main.call(kThreadDetach, id) == CELL_OK);
        CHECK(t.main.call(kThreadDetach, id) == rc(CELL_EINVAL));
        CHECK(t.join(id, status) == rc(CELL_EINVAL));
        release = true;
        CHECK(eventually([&] { return t.f.kernel->thread(id) == nullptr; }));
        CHECK(t.join(id, status) == rc(CELL_ESRCH));
    }
    SECTION("a thread created detached")
    {
        release = true;
        const u32 id = t.spawn(0x40000, 0, 0);
        CHECK(eventually([&] { return t.f.kernel->thread(id) == nullptr; }));
    }
    SECTION("an exited joinable thread stays a zombie until joined")
    {
        const u32 id = t.spawn(0x41000);
        CHECK(eventually([&] { return t.finished(id); }));
        CHECK(t.f.kernel->thread(id) != nullptr);
        REQUIRE(t.join(id, status) == CELL_OK);
        CHECK(status == 9);
    }
    SECTION("detaching a zombie reaps it")
    {
        const u32 id = t.spawn(0x41000);
        CHECK(eventually([&] { return t.finished(id); }));
        CHECK(t.main.call(kThreadDetach, id) == CELL_OK);
        CHECK(t.f.kernel->thread(id) == nullptr);
        CHECK(t.join(id, status) == rc(CELL_ESRCH));
    }
    SECTION("a second joiner gets EINVAL, detaching a joined thread EBUSY")
    {
        const u32 id = t.spawn(0x40000);
        std::thread joiner([&] {
            Caller other(*t.f.kernel, "joiner");
            const u32 out = t.f.alloc(8);
            CHECK(other.call(kThreadJoin, id, out) == CELL_OK);
            CHECK(t.f.r64(out) == 5);
        });
        CHECK(eventually([&] {
            std::lock_guard lock(t.f.kernel->impl().join_mutex);
            return t.f.kernel->thread(id)->join == JoinState::Joining;
        }));
        CHECK(t.join(id, status) == rc(CELL_EINVAL));
        CHECK(t.main.call(kThreadDetach, id) == rc(CELL_EBUSY));
        release = true;
        joiner.join();
    }
    release = true;
}

TEST_CASE("lv2 threads: priorities, names, join state and creation errors", "[lv2][thread]")
{
    ThreadFixture t;
    s32 join_state = -1;
    t.entries.add(0x50000, [&](PPUContext& ctx) {
        const u32 p = t.f.alloc(4);
        REQUIRE(static_cast<s32>(syscall(ctx, kThreadGetJoinState, p)) == CELL_OK);
        join_state = static_cast<s32>(t.f.r32(p));
        syscall(ctx, kThreadExit, 0);
    });
    u32 id = 0;
    REQUIRE(t.create(0x50000, 0, 500, 0x10000, kJoinable, "prio", id) == CELL_OK);
    const u32 p = t.f.alloc(4);
    REQUIRE(t.main.call(kThreadGetPriority, id, p) == CELL_OK);
    CHECK(t.f.r32(p) == 500);
    CHECK(t.main.call(kThreadSetPriority, id, 3071) == CELL_OK);
    REQUIRE(t.main.call(kThreadGetPriority, id, p) == CELL_OK);
    CHECK(t.f.r32(p) == 3071);
    CHECK(t.main.call(kThreadSetPriority, id, 3072) == rc(CELL_EINVAL));
    CHECK(t.main.call(kThreadSetPriority, id, -1) == rc(CELL_EINVAL)); // negative needs root
    CHECK(t.main.call(kThreadSetPriority, 0x01000077, 100) == rc(CELL_ESRCH));
    CHECK(t.main.call(kThreadGetPriority, 0x01000077, p) == rc(CELL_ESRCH));
    CHECK(t.main.call(kThreadSetPriority, t.main.id(), 1234) == CELL_OK);
    REQUIRE(t.main.call(kThreadGetPriority, t.main.id(), p) == CELL_OK);
    CHECK(t.f.r32(p) == 1234);

    CHECK(t.main.call(kThreadRename, id, t.f.cstr("a much longer thread name than allowed")) == CELL_OK);
    CHECK(t.f.kernel->thread(id)->name == "a much longer thread name t"); // 27 characters
    CHECK(t.main.call(kThreadRename, 0x01000077, t.f.cstr("x")) == rc(CELL_ESRCH));

    REQUIRE(t.main.call(kThreadGetJoinState, p) == CELL_OK);
    CHECK(t.f.r32(p) == 0); // an attached host thread is detached
    REQUIRE(t.main.call(kThreadStart, id) == CELL_OK);
    u64 status = 0;
    REQUIRE(t.join(id, status) == CELL_OK);
    CHECK(join_state == 1);

    u32 bad = 0;
    CHECK(t.create(0x50000, 0, 3072, 0x4000, kJoinable, nullptr, bad) == rc(CELL_EINVAL));
    CHECK(t.create(0x50000, 0, 1000, 0x4000, 3, nullptr, bad) == rc(CELL_EPERM)); // joinable + interrupt
    const u32 idp = t.f.alloc(8);
    CHECK(t.main.call(kThreadCreate, idp, 0, 0, 0, 1000, 0x4000, kJoinable, 0) == rc(CELL_EFAULT));
    const u32 param = t.f.alloc(8); // entry descriptor pointer 0
    CHECK(t.main.call(kThreadCreate, idp, param, 0, 0, 1000, 0x4000, kJoinable, 0) == rc(CELL_EFAULT));
    CHECK(t.main.call(kThreadStart, 0x01000077) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 threads: many threads run in parallel", "[lv2][thread]")
{
    ThreadFixture t;
    std::atomic<u32> ran{0};
    std::atomic<u32> concurrent{0}, peak{0};
    t.entries.add(0x60000, [&](PPUContext& ctx) {
        const u32 now = ++concurrent;
        u32 p = peak.load();
        while (now > p && !peak.compare_exchange_weak(p, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        --concurrent;
        ran++;
        syscall(ctx, kThreadExit, ctx.gpr[3] * 3);
    });
    std::vector<u32> ids;
    for (u32 i = 0; i < 32; i++) ids.push_back(t.spawn(0x60000, i));
    for (u32 i = 0; i < 32; i++) {
        u64 status = 0;
        REQUIRE(t.join(ids[i], status) == CELL_OK);
        CHECK(status == i * 3);
    }
    CHECK(ran.load() == 32);
    CHECK(peak.load() > 1);
}

TEST_CASE("lv2 threads: a guest program runs through the PPU interpreter and makes syscalls", "[lv2][thread][ppu]")
{
    Fixture f; // default executor: lifted dispatch with the interpreter fallback
    Caller main(*f.kernel, "main");
    LogCapture log;
    const ps_ppu_miss_policy old_policy = ps_ppu_get_miss_policy();
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);

    auto li = [](u32 rd, s16 v) { return (14u << 26) | (rd << 21) | static_cast<u16>(v); };
    auto lis = [](u32 rd, u16 v) { return (15u << 26) | (rd << 21) | v; };
    auto ori = [](u32 ra, u32 rs, u16 v) { return (24u << 26) | (rs << 21) | (ra << 16) | v; };
    auto addi = [](u32 rd, u32 ra, s16 v) { return (14u << 26) | (rd << 21) | (ra << 16) | static_cast<u16>(v); };
    constexpr u32 kSc = 0x44000002, kBlr = 0x4E800020;

    const std::string_view text = "hello from guest code\n";
    const u32 msg = f.cstr(text);
    const u32 written = f.alloc(4);
    const u32 code = f.alloc(64, 4);
    const u32 program[] = {
        lis(4, static_cast<u16>(msg >> 16)), ori(4, 4, static_cast<u16>(msg)), // r4 = buf
        li(3, 0),                                                                   // r3 = channel 0
        li(5, static_cast<s16>(text.size())),                                       // r5 = len
        lis(6, static_cast<u16>(written >> 16)), ori(6, 6, static_cast<u16>(written)),
        li(11, 403), kSc,                                                           // sys_tty_write
        addi(3, 3, 42), li(11, 41), kSc,                                            // _sys_ppu_thread_exit(42 + r3)
        kBlr,
    };
    for (u32 i = 0; i < std::size(program); i++) f.w32(code + i * 4, program[i]);

    const u32 returner = f.alloc(16, 4); // li r3, 9; blr
    f.w32(returner, li(3, 9));
    f.w32(returner + 4, kBlr);

    auto run = [&](u32 entry) {
        const u32 opd = f.alloc(8);
        f.w32(opd, entry);
        const u32 param = f.alloc(8);
        f.w32(param, opd);
        const u32 idp = f.alloc(8);
        REQUIRE(main.call(kThreadCreate, idp, param, 0, 0, 1000, 0x10000, kJoinable, f.cstr("guest")) == CELL_OK);
        const u32 id = static_cast<u32>(f.r64(idp));
        REQUIRE(main.call(kThreadStart, id) == CELL_OK);
        const u32 out = f.alloc(8);
        REQUIRE(main.call(kThreadJoin, id, out) == CELL_OK);
        return f.r64(out);
    };

    CHECK(run(code) == 42);
    CHECK(f.r32(written) == text.size());
    CHECK(log.contains("tty", "hello from guest code"));
    CHECK(run(returner) == 9); // blr to the entry LR ends the thread with r3

    ps_ppu_set_miss_policy(old_policy);
}

namespace {

// Stands in for translated code whose return finds r1 moved (a longjmp or a fiber switch, D-161): the jump restored
// LR to `landing` and r1 elsewhere, then the lifter's marker syscall. Counts its entries; never gets control back.
struct JumpingExecutor {
    u32 landing = 0;
    std::atomic<u32> entries{0}, returned{0};
};

void jumping_executor(void* user, PPUContext& ctx, u8* base, u32)
{
    auto& j = *static_cast<JumpingExecutor*>(user);
    j.entries++;
    ctx.lr = j.landing;
    ctx.gpr[1] -= 0x200;
    ctx.cia = kNonLocalReturnCia;
    ps_lv2_syscall(&ctx, base);
    j.returned++; // not reached: the kernel goes back to run_guest
}

} // namespace

TEST_CASE("lv2 threads: a non-local jump in translated code goes on in the interpreter, by _longjmp (D-161, D-213)",
    "[lv2][thread][ppu]")
{
    JumpingExecutor j;
    Config config;
    config.executor = jumping_executor;
    config.executor_user = &j;
    Fixture f(std::move(config));
    Caller main(*f.kernel, "main");
    auto li = [](u32 rd, s16 v) { return (14u << 26) | (rd << 21) | static_cast<u16>(v); };
    constexpr u32 kMtlrR0 = 0x7C0803A6, kBlr = 0x4E800020;
    // Where the jump lands: r3 = 77, then return to the thread's end (LR 0) whatever r1 is.
    j.landing = f.alloc(16, 4);
    f.w32(j.landing, li(3, 77));
    f.w32(j.landing + 4, li(0, 0));
    f.w32(j.landing + 8, kMtlrR0);
    f.w32(j.landing + 12, kBlr);
    for (int round = 0; round < 3; round++) {
        const u32 opd = f.alloc(8);
        f.w32(opd, 0x00200000); // the entry: the executor never runs it
        const u32 param = f.alloc(8);
        f.w32(param, opd);
        const u32 idp = f.alloc(8);
        REQUIRE(main.call(kThreadCreate, idp, param, 0, 0, 1000, 0x10000, kJoinable, f.cstr("jumper")) == CELL_OK);
        const u32 id = static_cast<u32>(f.r64(idp));
        REQUIRE(main.call(kThreadStart, id) == CELL_OK);
        const u32 out = f.alloc(8);
        REQUIRE(main.call(kThreadJoin, id, out) == CELL_OK);
        CHECK(f.r64(out) == 77);
    }
    CHECK(j.entries == 3);
    CHECK(j.returned == 0);
}
