// cellFiber (D-568): pspocket's libfiber (made from RPCS3's checks and libfiber's documented model; no Sony module to
// compare with). Fibers and contexts run a small guest routine that follows a script: log a marker, call an HLE
// function (logging its answer, or not), end with a status. The log shows the order the scheduler ran them in. No game
// data.

#include "hle_test_util.hpp"

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <thread>
#include <vector>

#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kCode = kData, kRoutine = kData + 0x200, kLog = kData + 0x1000, kScripts = kData + 0x3000,
              kSched = kData + 0x8000, kAttr = kData + 0x8400, kFibers = kData + 0x9000, kContexts = kData + 0xD000,
              kOut = kData + 0x10000, kStacks0 = kData + 0x20000, kControl = kData + 0x130000,
              kBuffer = kData + 0x131000;
constexpr u32 kStackSize = 0x8000;
const u64 kInval = r3_of_code(0x80760002), kDeadlk = r3_of_code(0x80760008), kPerm = r3_of_code(0x80760009),
          kBusy = r3_of_code(0x8076000A), kStat = r3_of_code(0x8076000F), kAlign = r3_of_code(0x80760010),
          kNull = r3_of_code(0x80760011), kNoSysInit = r3_of_code(0x80760020);

u32 fiber_at(u32 i) { return kFibers + 0x400 * i; }
u32 context_at(u32 i) { return kContexts + 0x300 * i; }
u32 stack_at(u32 i) { return kStacks0 + kStackSize * i; }
u32 out(u32 i) { return kOut + 8 * i; }

enum Op : u32 { END = 0, LOG = 1, CALL = 2, CALLQ = 3 };
struct Step {
    u32 op, a = 0, b = 0, c = 0;
};

// The script routine: r3 is the script, 16 bytes a step {op, a, b, c}. END returns a; LOG appends a to the log; CALL
// calls the function whose descriptor is a with r3 = b, r4 = c (r5 to r7 zero) and appends what it answers (CALLQ:
// does not).
void write_routine(u8* base)
{
    std::vector<u32> c;
    const auto load = [&](u32 reg, u32 value) {
        c.push_back(as::lis(reg, static_cast<s32>(value >> 16)));
        c.push_back(as::ori(reg, reg, value & 0xFFFF));
    };
    c.push_back(as::mflr(0));
    c.push_back(as::std_(0, 16, 1));
    c.push_back(as::stdu(1, -0x80, 1));
    c.push_back(as::std_(31, 0x70, 1));
    c.push_back(as::std_(30, 0x68, 1));
    c.push_back(as::mr(31, 3));
    const usize loop = c.size();
    c.push_back(as::lwz(0, 0, 31));
    c.push_back(as::cmpwi(0, 0, END));
    const usize to_end = c.size();
    c.push_back(0); // beq end
    c.push_back(as::cmpwi(0, 0, LOG));
    const usize to_log = c.size();
    c.push_back(0); // beq log
    c.push_back(as::mr(30, 0)); // the op, to tell CALL from CALLQ after the call
    c.push_back(as::lwz(12, 4, 31));
    c.push_back(as::lwz(3, 8, 31));
    c.push_back(as::lwz(4, 12, 31));
    c.push_back(as::li(5, 0));
    c.push_back(as::li(6, 0));
    c.push_back(as::li(7, 0));
    c.push_back(as::lwz(0, 0, 12));
    c.push_back(as::lwz(2, 4, 12));
    c.push_back(as::mtctr(0));
    c.push_back(as::bctrl());
    c.push_back(as::cmpwi(0, 30, CALLQ));
    const usize to_next = c.size();
    c.push_back(0); // beq next
    c.push_back(as::mr(30, 3));
    const usize to_append = c.size();
    c.push_back(0); // b append
    const usize log = c.size();
    c.push_back(as::lwz(30, 4, 31));
    const usize append = c.size();
    load(11, kLog);
    c.push_back(as::lwz(12, 0, 11));
    c.push_back(as::addi(10, 12, 1));
    c.push_back(as::stw(10, 0, 11));
    c.push_back(as::slwi(12, 12, 2));
    c.push_back(as::add(12, 11, 12));
    c.push_back(as::stw(30, 4, 12));
    const usize next = c.size();
    c.push_back(as::addi(31, 31, 16));
    c.push_back(as::b_rel(static_cast<s32>(4 * (static_cast<s64>(loop) - static_cast<s64>(c.size())))));
    const usize end = c.size();
    c.push_back(as::lwz(3, 4, 31));
    c.push_back(as::ld(30, 0x68, 1));
    c.push_back(as::ld(31, 0x70, 1));
    c.push_back(as::addi(1, 1, 0x80));
    c.push_back(as::ld(0, 16, 1));
    c.push_back(as::mtlr(0));
    c.push_back(as::blr());
    const auto rel = [&](usize from, usize to) { return static_cast<s32>(4 * (static_cast<s64>(to) - static_cast<s64>(from))); };
    c[to_end] = as::bc_rel(12, 2, rel(to_end, end));
    c[to_log] = as::bc_rel(12, 2, rel(to_log, log));
    c[to_next] = as::bc_rel(12, 2, rel(to_next, next));
    c[to_append] = as::b_rel(rel(to_append, append));
    REQUIRE(4 * c.size() <= kRoutine - kCode);
    for (usize i = 0; i < c.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), c[i]);
    ps_write32(base, kRoutine, kCode);
    ps_write32(base, kRoutine + 4, 0);
}

struct Env {
    Fixture f;
    GuestThread g;
    u8* b;
    u32 next_script = kScripts;

    Env() : g(f, 0, "game"), b(f.base())
    {
        REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        write_routine(b);
        ps_write32(b, kLog, 0);
    }

    u32 fn(std::string_view name, std::string_view module = "cellFiber")
    {
        const u32 d = f.hle->link_function(module, recomp::nid_of(name), name);
        REQUIRE(d);
        return d;
    }

    u32 script(std::initializer_list<Step> steps)
    {
        const u32 at = next_script;
        u32 i = 0;
        for (const Step& s : steps) {
            ps_write32(b, at + 16 * i, s.op);
            ps_write32(b, at + 16 * i + 4, s.a);
            ps_write32(b, at + 16 * i + 8, s.b);
            ps_write32(b, at + 16 * i + 12, s.c);
            i++;
        }
        next_script += 0x100;
        REQUIRE(i <= 16);
        return at;
    }

    std::vector<u32> log()
    {
        std::vector<u32> v;
        for (u32 i = 0; i < ps_read32(b, kLog); i++) v.push_back(ps_read32(b, kLog + 4 + 4 * i));
        ps_write32(b, kLog, 0);
        return v;
    }

    u64 create(u32 sched, u32 i, u32 script_addr, u32 prio, u32 attr = 0)
    {
        return g.call("cellFiberPpuCreateFiber", sched, fiber_at(i), kRoutine, script_addr, prio, stack_at(i),
            kStackSize, attr);
    }
};

u32 code(u64 r3) { return static_cast<u32>(r3); }

} // namespace

TEST_CASE("cellFiber: the 47 functions, firmware first", "[hle][fiber]")
{
    u32 n = 0;
    for (const hle::Function& fn : hle::registry().functions())
        if (fn.module == "cellFiber") {
            CHECK(fn.nid == recomp::nid_of(fn.name));
            n++;
        }
    CHECK(n == 47);
    CHECK(hle::registry().is_firmware_first("cellFiber"));
}

TEST_CASE("cellFiber: a scheduler runs its fibers by priority, each until it yields or ends; joins from the thread",
    "[hle][fiber]")
{
    Env e;
    CHECK(e.g.call("cellFiberPpuInitializeScheduler", kSched, 0) == kNoSysInit);
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    std::memset(e.b + kAttr, 0xAA, 256);
    REQUIRE(e.g.call("_cellFiberPpuSchedulerAttributeInitialize", kAttr, 0x330000) == 0);
    CHECK(ps_read8(e.b, kAttr + 16) == 0); // autoCheckFlags
    CHECK(ps_read32(e.b, kAttr + 20) == 0);
    REQUIRE(e.g.call("cellFiberPpuInitializeScheduler", kSched, kAttr) == 0);

    const u32 yield = e.fn("cellFiberPpuYield"), self = e.fn("cellFiberPpuSelf");
    REQUIRE(e.create(kSched, 0, e.script({{LOG, 0xA1}, {CALL, yield}, {LOG, 0xA2}, {END, 7}}), 1) == 0);
    REQUIRE(e.create(kSched, 1, e.script({{LOG, 0xB1}, {CALL, yield}, {LOG, 0xB2}, {END, 8}}), 1) == 0);
    REQUIRE(e.create(kSched, 2, e.script({{LOG, 0xC1}, {CALL, self}, {END, 9}}), 0) == 0);
    REQUIRE(e.g.call("cellFiberPpuHasRunnableFiber", kSched, out(0)) == 0);
    CHECK(ps_read8(e.b, out(0)) == 1);
    CHECK(e.g.call("cellFiberPpuSelf") == 0); // not a fiber

    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    CHECK(e.log() == std::vector<u32>{0xC1, fiber_at(2), 0xA1, 0xB1, 0, 0xA2, 0, 0xB2});
    REQUIRE(e.g.call("cellFiberPpuHasRunnableFiber", kSched, out(0)) == 0);
    CHECK(ps_read8(e.b, out(0)) == 0);

    // Their ends: joined from the thread once ended; a joined fiber is gone.
    REQUIRE(e.g.call("cellFiberPpuJoinFiber", fiber_at(0), out(1)) == 0);
    CHECK(ps_read32(e.b, out(1)) == 7);
    CHECK(e.g.call("cellFiberPpuJoinFiber", fiber_at(0), out(1)) == kStat);
    REQUIRE(e.g.call("cellFiberPpuGetScheduler", fiber_at(1), out(2)) == 0);
    CHECK(ps_read32(e.b, out(2)) == kSched);
    REQUIRE(e.g.call("cellFiberPpuJoinFiber", fiber_at(2), out(1)) == 0);
    CHECK(ps_read32(e.b, out(1)) == 9);
    // A fiber's memory, ended, makes a new fiber.
    REQUIRE(e.create(kSched, 1, e.script({{LOG, 0xD1}, {END, 10}}), 3) == 0);
    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    CHECK(e.log() == std::vector<u32>{0xD1});
    REQUIRE(e.g.call("cellFiberPpuFinalizeScheduler", kSched) == 0);
    CHECK(e.g.call("cellFiberPpuRunFibers", kSched) == kStat);
}

TEST_CASE("cellFiber: signals, joins between fibers, flags, the exit callback, Exit", "[hle][fiber]")
{
    Env e;
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    REQUIRE(e.g.call("cellFiberPpuInitializeScheduler", kSched, 0) == 0);
    const u32 wait_signal = e.fn("cellFiberPpuWaitSignal"), send_signal = e.fn("cellFiberPpuSendSignal"),
              join = e.fn("cellFiberPpuJoinFiber"), wait_flag = e.fn("cellFiberPpuWaitFlag"),
              exit = e.fn("cellFiberPpuExit"), stack_limit = e.fn("cellFiberPpuCheckStackLimit");
    const u32 flag = out(10);
    ps_write32(e.b, flag, 0);
    // W's exit callback logs 0xEE (the routine with W's callback argument as its script).
    REQUIRE(e.g.call("_cellFiberPpuAttributeInitialize", kAttr, 0) == 0);
    ps_write32(e.b, kAttr + 48, kRoutine);
    ps_write64(e.b, kAttr + 56, e.script({{LOG, 0xEE}, {END, 0}}));
    std::memcpy(e.b + kAttr + 16, "waiter", 7);
    REQUIRE(e.create(kSched, 0, e.script({{LOG, 0x11}, {CALL, wait_signal}, {LOG, 0x12}, {CALL, exit, 5}}), 0, kAttr) ==
        0);
    REQUIRE(e.create(kSched, 1,
                e.script({{LOG, 0x21}, {CALL, send_signal, fiber_at(0), out(0)}, {LOG, 0x22},
                    {CALL, join, fiber_at(0), out(1)}, {LOG, 0x23}, {CALL, stack_limit}, {END, 0}}),
                1) == 0);
    REQUIRE(e.create(kSched, 2, e.script({{LOG, 0x31}, {CALL, wait_flag, flag, 1}, {LOG, 0x32}, {END, 0}}), 2) == 0);
    REQUIRE(e.create(kSched, 3, e.script({{CALL, join, fiber_at(3), out(2)}, {END, 0}}), 3) == 0);

    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    CHECK(e.log() == std::vector<u32>{0x11, 0x21, 0, 0x22, 0, 0x12, 0xEE, 0, 0x23, 0, 0x31, code(kDeadlk)});
    CHECK(ps_read32(e.b, out(0)) == 0); // numWorker: none for a plain scheduler
    CHECK(ps_read32(e.b, out(1)) == 5); // W's status through Exit

    // The flag: CheckFlags makes its waiter ready once it holds the value.
    REQUIRE(e.g.call("cellFiberPpuCheckFlags", kSched) == 0);
    REQUIRE(e.g.call("cellFiberPpuHasRunnableFiber", kSched, out(3)) == 0);
    CHECK(ps_read8(e.b, out(3)) == 0);
    ps_write32(e.b, flag, 1);
    REQUIRE(e.g.call("cellFiberPpuCheckFlags", kSched) == 0);
    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0x32});
    CHECK(e.g.call("cellFiberPpuSendSignal", fiber_at(0), 0) == kStat); // ended

    // Fiber-only calls from the thread.
    CHECK(e.g.call("cellFiberPpuYield") == kPerm);
    CHECK(e.g.call("cellFiberPpuWaitSignal") == kPerm);
    CHECK(e.g.call("cellFiberPpuWaitFlag", flag, 1) == kPerm);
    CHECK(e.g.call("cellFiberPpuExit", 0) == kPerm);
    CHECK(e.g.call("cellFiberPpuSetPriority", 1) == kPerm);
    CHECK(e.g.call("cellFiberPpuSetPriority", 4) == kInval);
    CHECK(e.g.call("cellFiberPpuCheckStackLimit") == kPerm);
    REQUIRE(e.g.call("cellFiberPpuFinalizeScheduler", kSched) == 0);
}

TEST_CASE("cellFiber: autoCheckFlags; a waiting fiber keeps the scheduler busy; a signal kept for the next wait",
    "[hle][fiber]")
{
    Env e;
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    REQUIRE(e.g.call("_cellFiberPpuSchedulerAttributeInitialize", kAttr, 0) == 0);
    ps_write8(e.b, kAttr + 16, 1);
    ps_write32(e.b, kAttr + 20, 200);
    REQUIRE(e.g.call("cellFiberPpuInitializeScheduler", kSched, kAttr) == 0);
    const u32 wait_flag = e.fn("cellFiberPpuWaitFlag"), wait_signal = e.fn("cellFiberPpuWaitSignal");
    const u32 flag = out(10);
    ps_write32(e.b, flag, 0);
    REQUIRE(e.create(kSched, 0, e.script({{CALL, wait_flag, flag, 1}, {LOG, 0x41}, {END, 0}}), 0) == 0);
    // With autoCheckFlags, RunFibers waits for the flag (set from another host thread here).
    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ps_write32(e.b, flag, 1);
    });
    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    setter.join();
    CHECK(e.log() == std::vector<u32>{0, 0x41});

    // A signal sent before the wait is kept; a fiber waiting for a second one keeps the scheduler from finishing.
    REQUIRE(e.create(kSched, 1, e.script({{CALL, wait_signal}, {LOG, 0x51}, {CALL, wait_signal}, {END, 0}}), 0) ==
        0);
    REQUIRE(e.g.call("cellFiberPpuSendSignal", fiber_at(1), out(0)) == 0);
    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0x51});
    CHECK(e.g.call("cellFiberPpuFinalizeScheduler", kSched) == kBusy);
    REQUIRE(e.g.call("cellFiberPpuSendSignal", fiber_at(1), 0) == 0);
    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);
    CHECK(e.log() == std::vector<u32>{0});
    REQUIRE(e.g.call("cellFiberPpuFinalizeScheduler", kSched) == 0);
}

TEST_CASE("cellFiber: contexts: Run, Switch, ReturnToThread, an entry's return, Finalize", "[hle][fiber]")
{
    Env e;
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    const u32 sw = e.fn("cellFiberPpuContextSwitch"), back = e.fn("cellFiberPpuContextReturnToThread"),
              self = e.fn("cellFiberPpuContextSelf"), limit = e.fn("cellFiberPpuContextCheckStackLimit");
    const u32 c1 = context_at(0), c2 = context_at(1);
    REQUIRE(e.g.call("_cellFiberPpuContextAttributeInitialize", kAttr, 0) == 0);
    REQUIRE(e.g.call("cellFiberPpuContextInitialize", c1, kRoutine,
                e.script({{LOG, 0xA}, {CALL, self}, {CALL, sw, c2, out(0)}, {LOG, 0xC}, {CALL, limit},
                    {CALL, back, 33}, {LOG, 0xE}, {CALL, back, 44}, {END, 0}}),
                stack_at(0), kStackSize, kAttr) == 0);
    REQUIRE(e.g.call("cellFiberPpuContextInitialize", c2, kRoutine,
                e.script({{LOG, 0xB}, {CALL, sw, c1, out(1)}, {LOG, 0xD}, {END, 0}}), stack_at(1), kStackSize, 0) ==
        0);
    CHECK(e.g.call("cellFiberPpuContextSelf") == 0);
    CHECK(e.g.call("cellFiberPpuContextSwitch", c2, out(0), 0) == kPerm);
    CHECK(e.g.call("cellFiberPpuContextReturnToThread", 1) == kPerm);

    REQUIRE(e.g.call("cellFiberPpuContextRun", c1, out(2), out(3), 0) == 0);
    CHECK(e.log() == std::vector<u32>{0xA, c1, 0xB, 0, 0xC, 0});
    CHECK(static_cast<s32>(ps_read32(e.b, out(2))) == 33); // the cause
    CHECK(ps_read32(e.b, out(3)) == c1);                    // who came back
    CHECK(ps_read32(e.b, out(0)) == c2);                    // who switched to c1
    REQUIRE(e.g.call("cellFiberPpuContextRun", c1, out(2), out(3), 0) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0xE});
    CHECK(ps_read32(e.b, out(2)) == 44);
    // c2 resumed by the thread: its switch says 0 (the thread); its entry returns: cause 0, from c2; it has ended.
    REQUIRE(e.g.call("cellFiberPpuContextRun", c2, out(2), out(3), 0) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0xD});
    CHECK(ps_read32(e.b, out(1)) == 0);
    CHECK(ps_read32(e.b, out(2)) == 0);
    CHECK(ps_read32(e.b, out(3)) == c2);
    CHECK(e.g.call("cellFiberPpuContextRun", c2, out(2), out(3), 0) == kStat);
    // Finalize: c1 is parked in its ReturnToThread (its host thread unwinds), c2 has ended.
    REQUIRE(e.g.call("cellFiberPpuContextFinalize", c1) == 0);
    REQUIRE(e.g.call("cellFiberPpuContextFinalize", c2) == 0);
    CHECK(e.g.call("cellFiberPpuContextRun", c1, out(2), out(3), 0) == kStat);
    CHECK(e.g.call("cellFiberPpuContextFinalize", c1) == kStat);
}

TEST_CASE("cellFiber: RunScheduler and EnterScheduler pick contexts through a callback", "[hle][fiber]")
{
    Env e;
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    const u32 enter = e.fn("cellFiberPpuContextEnterScheduler"), back = e.fn("cellFiberPpuContextReturnToThread"),
              sw = e.fn("cellFiberPpuContextSwitch");
    const u32 c3 = context_at(2), c4 = context_at(3);
    // The callback is the routine too: its first argument a script that ends with the context to run.
    const u32 pick3 = e.script({{END, c3}}), pick4 = e.script({{END, c4}}), none = e.script({{END, 0}});
    REQUIRE(e.g.call("cellFiberPpuContextInitialize", c3, kRoutine,
                e.script({{LOG, 0x3}, {CALL, enter, kRoutine, pick4}, {LOG, 0x33}, {CALL, back, 5}, {END, 0}}),
                stack_at(2), kStackSize, 0) == 0);
    REQUIRE(e.g.call("cellFiberPpuContextInitialize", c4, kRoutine,
                e.script({{LOG, 0x4}, {CALL, sw, c3, out(0)}, {END, 0}}), stack_at(3), kStackSize, 0) == 0);
    REQUIRE(e.g.call("cellFiberPpuContextRunScheduler", kRoutine, pick3, 0, out(2), out(3), 0) == 0);
    CHECK(e.log() == std::vector<u32>{0x3, 0x4, 0, 0x33});
    CHECK(ps_read32(e.b, out(2)) == 5);
    CHECK(ps_read32(e.b, out(3)) == c3);
    // A callback that picks nothing: Run returns at once.
    ps_write32(e.b, out(2), 0xFFFF);
    REQUIRE(e.g.call("cellFiberPpuContextRunScheduler", kRoutine, none, 0, out(2), out(3), 0) == 0);
    CHECK(ps_read32(e.b, out(2)) == 0);
    CHECK(e.log().empty());
    // c3 and c4 stay parked: the process's end unwinds their host threads.
}

TEST_CASE("cellFiber: a worker control's fibers run on two worker threads; joined from a third", "[hle][fiber]")
{
    Env e;
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    REQUIRE(e.g.call("_cellFiberPpuUtilWorkerControlAttributeInitialize", kAttr, 0) == 0);
    REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlInitializeWithAttribute", kControl, kAttr) == 0);
    REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlSetPollingMode", kControl, 0, 500) == 0);
    const u32 yield = e.fn("cellFiberPpuYield");
    std::atomic<u32> returned{0};
    std::vector<std::thread> workers;
    for (u32 w = 0; w < 2; w++) {
        workers.emplace_back([&, w] {
            GuestThread worker(e.f, 1 + w, "worker");
            if (worker.call("cellFiberPpuUtilWorkerControlRunFibers", kControl) == 0) returned++;
        });
    }
    for (u32 i = 0; i < 6; i++) {
        const u32 s = e.script({{CALLQ, yield}, {CALLQ, yield}, {END, 100 + i}});
        REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlCreateFiber", kControl, fiber_at(i), kRoutine, s, i % 4,
                    stack_at(i), kStackSize, 0) == 0);
    }
    REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlWakeup", kControl) == 0);
    for (u32 i = 0; i < 6; i++) {
        INFO("fiber " << i);
        REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlJoinFiber", kControl, fiber_at(i), out(i)) == 0);
        CHECK(ps_read32(e.b, out(i)) == 100 + i);
    }
    CHECK(e.g.call("cellFiberPpuUtilWorkerControlFinalize", kControl) == kBusy); // workers still in RunFibers
    REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlShutdown", kControl) == 0);
    for (std::thread& t : workers) t.join();
    CHECK(returned == 2);
    CHECK(e.g.call("cellFiberPpuUtilWorkerControlCreateFiber", kControl, fiber_at(7), kRoutine, kScripts, 0,
              stack_at(7), kStackSize, 0) == kStat); // shut down
    REQUIRE(e.g.call("cellFiberPpuUtilWorkerControlFinalize", kControl) == 0);
    CHECK(e.g.call("cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs") == kPerm);
}

TEST_CASE("cellFiber: the argument checks, attributes and the trace's states", "[hle][fiber]")
{
    Env e;
    CHECK(e.g.call("cellFiberPpuContextInitialize", context_at(0), kRoutine, 0, stack_at(0), kStackSize, 0) ==
        kNoSysInit);
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    CHECK(e.g.call("cellFiberPpuInitializeScheduler", 0, 0) == kNull);
    CHECK(e.g.call("cellFiberPpuInitializeScheduler", kSched + 8, 0) == kAlign);
    CHECK(e.g.call("_cellFiberPpuAttributeInitialize", 0, 0) == kNull);
    CHECK(e.g.call("_cellFiberPpuAttributeInitialize", kAttr + 4, 0) == kAlign);
    CHECK(e.g.call("_cellFiberPpuContextAttributeInitialize", kAttr + 2, 0) == kAlign);
    CHECK(e.g.call("cellFiberPpuRunFibers", kSched) == kStat); // not made
    CHECK(e.create(kSched, 0, kScripts, 0) == kStat);
    REQUIRE(e.g.call("cellFiberPpuInitializeScheduler", kSched, 0) == 0);
    CHECK(e.create(kSched, 0, kScripts, 4) == kInval); // priorities 0-3
    CHECK(e.g.call("cellFiberPpuCreateFiber", kSched, fiber_at(0) + 64, kRoutine, 0, 0, stack_at(0), kStackSize, 0) ==
        kAlign);
    CHECK(e.g.call("cellFiberPpuCreateFiber", kSched, fiber_at(0), kRoutine, 0, 0, stack_at(0) + 4, kStackSize, 0) ==
        kAlign);
    CHECK(e.g.call("cellFiberPpuCreateFiber", kSched, fiber_at(0), kRoutine, 0, 0, stack_at(0), 0x100, 0) == kInval);
    CHECK(e.g.call("cellFiberPpuCreateFiber", kSched, fiber_at(0), 0, 0, 0, stack_at(0), kStackSize, 0) == kNull);
    REQUIRE(e.create(kSched, 0, e.script({{END, 1}}), 0) == 0);
    CHECK(e.create(kSched, 0, e.script({{END, 1}}), 0) == kBusy); // not ended
    CHECK(e.g.call("cellFiberPpuJoinFiber", fiber_at(0), out(0)) == kPerm); // from the thread, before its end
    CHECK(e.g.call("cellFiberPpuFinalizeScheduler", kSched) == kBusy);
    REQUIRE(e.g.call("cellFiberPpuRunFibers", kSched) == 0);

    // The trace: initialize, start, stop, finalize, each in its turn.
    CHECK(e.g.call("cellFiberPpuSchedulerTraceStart", kSched) == kStat);
    CHECK(e.g.call("cellFiberPpuSchedulerTraceInitialize", kSched, kBuffer + 8, 0x1000, 0) == kAlign);
    CHECK(e.g.call("cellFiberPpuSchedulerTraceInitialize", kSched, kBuffer, 0, 0) == kInval);
    REQUIRE(e.g.call("cellFiberPpuSchedulerTraceInitialize", kSched, kBuffer, 0x1000, 0) == 0);
    CHECK(e.g.call("cellFiberPpuSchedulerTraceInitialize", kSched, kBuffer, 0x1000, 0) == kStat);
    REQUIRE(e.g.call("cellFiberPpuSchedulerTraceStart", kSched) == 0);
    CHECK(e.g.call("cellFiberPpuSchedulerTraceStart", kSched) == kStat);
    REQUIRE(e.g.call("cellFiberPpuSchedulerTraceStop", kSched) == 0);
    REQUIRE(e.g.call("cellFiberPpuSchedulerTraceFinalize", kSched) == 0);
    CHECK(e.g.call("cellFiberPpuSchedulerTraceStop", kSched) == kStat);
    REQUIRE(e.g.call("cellFiberPpuFinalizeScheduler", kSched) == 0);
}

TEST_CASE("cellFiber: the thread exiting inside a fiber unwinds the thread", "[hle][fiber]")
{
    Env e;
    REQUIRE(e.g.call("_cellFiberPpuInitialize") == 0);
    REQUIRE(e.g.call("cellFiberPpuInitializeScheduler", kSched, 0) == 0);
    const u32 thread_exit = e.fn("sys_ppu_thread_exit", "sysPrxForUser");
    REQUIRE(e.create(kSched, 0, e.script({{LOG, 0x77}, {CALL, thread_exit, 77}, {END, 0}}), 0) == 0);
    CHECK_THROWS_AS(e.g.call("cellFiberPpuRunFibers", kSched), lv2::ThreadExit);
    CHECK(e.log() == std::vector<u32>{0x77});
}
