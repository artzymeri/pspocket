// cellFiber (D-914): pspocket's libfiber, read from Sony's and compared with it in test_fiber_compare.cpp; these are
// the same scenarios with Sony's answers, checked without the firmware. Fibers and contexts run a small guest routine
// that follows a script (fiber_script.hpp): log a marker, call a function (logging its answer, or not), end with a
// status. The log shows the order the scheduler ran them in. Every call goes through the export's descriptor, as a
// game's import stub makes it: a context switch goes back to the thread's innermost guest entry (D-161). Each PPU
// thread has its TLS block, where libfiber keeps its per-thread state (at the offset _cellFiberPpuInitialize takes from
// the game's TLS variable). No game data.

#include "hle_test_util.hpp"

#include "guest.hpp"

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <thread>
#include <vector>

#include "fiber_script.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::fiber;

namespace {

constexpr u32 kCode = kData, kRoutine = kData + 0x200, kLog = kData + 0x1000, kScripts = kData + 0x3000,
              kSched = kData + 0x8000, kAttr = kData + 0x8400, kFibers = kData + 0x9000, kContexts = kData + 0xD000,
              kOut = kData + 0x10000, kStacks0 = kData + 0x20000, kControl = kData + 0x130000,
              kBuffer = kData + 0x131000;
constexpr u32 kStackSize = 0x8000, kSdk = 0x330000;
const u64 kInval = r3_of_code(0x80760002), kDeadlk = r3_of_code(0x80760008), kPerm = r3_of_code(0x80760009),
          kBusy = r3_of_code(0x8076000A), kStat = r3_of_code(0x8076000F), kAlign = r3_of_code(0x80760010),
          kNull = r3_of_code(0x80760011), kNoSysInit = r3_of_code(0x80760020);

u32 fiber_at(u32 i) { return kFibers + 0x400 * i; }
u32 context_at(u32 i) { return kContexts + 0x300 * i; }
u32 stack_at(u32 i) { return kStacks0 + kStackSize * i; }
u32 out(u32 i) { return kOut + 8 * i; }
u32 code(u64 r3) { return static_cast<u32>(r3); }

struct Env {
    Fixture f;
    GuestThread g;
    u8* b;
    u32 next_script = kScripts;
    u32 tls_var = 0; // the game's TLS variable on g: _cellFiberPpuInitialize's argument

    Env() : g(f, 0, "game"), b(f.base())
    {
        REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
        tls_var = give_tls(g);
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        write_routine(b, kCode, kRoutine, kLog);
        ps_write32(b, kLog, 0);
    }

    // A PPU thread's TLS block (r13 into it, as lv2 starts a thread); the address of its TLS variable.
    u32 give_tls(GuestThread& t)
    {
        const u32 tls = f.hle->alloc_tls();
        REQUIRE(tls);
        t.ctx.gpr[13] = tls + hle::kTlsR13Offset;
        return tls + hle::kTlsSystemArea;
    }

    u32 fn(std::string_view name, std::string_view module = "cellFiber")
    {
        const u32 d = f.hle->link_function(module, recomp::nid_of(name), name);
        REQUIRE(d);
        return d;
    }

    // A call through the export's descriptor, r3 as the caller reads it.
    u64 call(std::string_view name, std::initializer_list<u64> args = {})
    {
        return hle::call_guest(g.ctx, fn(name), args);
    }
    u64 init() { return call("_cellFiberPpuInitialize", {tls_var}); }

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

    u64 create(u32 sched, u32 i, u32 script_addr, u32 prio, u32 attr = 0, u32 stack_size = kStackSize)
    {
        return call("cellFiberPpuCreateFiber",
            {sched, fiber_at(i), kRoutine, script_addr, prio, stack_at(i), stack_size, attr});
    }
};

} // namespace

TEST_CASE("cellFiber: the 47 functions, pspocket's own (no longer firmware-first, D-914)", "[hle][fiber]")
{
    u32 n = 0;
    for (const hle::Function& fn : hle::registry().functions())
        if (fn.module == "cellFiber" && !fn.internal) {
            CHECK(fn.nid == recomp::nid_of(fn.name));
            n++;
        }
    CHECK(n == 47);
    CHECK_FALSE(hle::registry().is_firmware_first("cellFiber"));
}

TEST_CASE("cellFiber: a scheduler runs its fibers by priority, each until it yields or ends; joins from the thread",
    "[hle][fiber]")
{
    Env e;
    CHECK(e.call("cellFiberPpuInitializeScheduler", {kSched, 0}) == kNoSysInit);
    // The offset of the TLS variable from r13: the segment starts 0x7000 below it.
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    std::memset(e.b + kAttr, 0xAA, 256);
    REQUIRE(e.call("_cellFiberPpuSchedulerAttributeInitialize", {kAttr, kSdk}) == 0);
    CHECK(ps_read64(e.b, kAttr) == 0x44007515864E5F4Cull); // its magic, the SDK version, the rest zero
    CHECK(ps_read32(e.b, kAttr + 8) == kSdk);
    CHECK(ps_read8(e.b, kAttr + 16) == 0); // autoCheckFlags
    CHECK(ps_read32(e.b, kAttr + 20) == 0);
    CHECK(ps_read8(e.b, kAttr + 255) == 0);
    REQUIRE(e.call("cellFiberPpuInitializeScheduler", {kSched, kAttr}) == 0);

    const u32 yield = e.fn("cellFiberPpuYield"), self = e.fn("cellFiberPpuSelf");
    REQUIRE(e.create(kSched, 0, e.script({{LOG, 0xA1}, {CALL, yield}, {LOG, 0xA2}, {END, 7}}), 1) == 0);
    REQUIRE(e.create(kSched, 1, e.script({{LOG, 0xB1}, {CALL, yield}, {LOG, 0xB2}, {END, 8}}), 1) == 0);
    REQUIRE(e.create(kSched, 2, e.script({{LOG, 0xC1}, {CALL, self}, {END, 9}}), 0) == 0);
    REQUIRE(e.call("cellFiberPpuHasRunnableFiber", {kSched, out(0)}) == 0);
    CHECK(ps_read8(e.b, out(0)) == 1);
    CHECK(e.call("cellFiberPpuSelf") == 0); // not a fiber

    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log() == std::vector<u32>{0xC1, fiber_at(2), 0xA1, 0xB1, 0, 0xA2, 0, 0xB2});
    REQUIRE(e.call("cellFiberPpuHasRunnableFiber", {kSched, out(0)}) == 0);
    CHECK(ps_read8(e.b, out(0)) == 0);

    // Their ends: joined from the thread once ended; a joined fiber is gone.
    REQUIRE(e.call("cellFiberPpuGetScheduler", {fiber_at(1), out(2)}) == 0);
    CHECK(ps_read32(e.b, out(2)) == kSched);
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(0), out(1)}) == 0);
    CHECK(ps_read32(e.b, out(1)) == 7);
    CHECK(e.call("cellFiberPpuJoinFiber", {fiber_at(0), out(1)}) == kStat);
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(2), out(1)}) == 0);
    CHECK(ps_read32(e.b, out(1)) == 9);
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(1), out(1)}) == 0);
    CHECK(ps_read32(e.b, out(1)) == 8);
    // A fiber's memory, ended and joined, makes a new fiber; the scheduler holds a fiber until it is joined, so
    // FinalizeScheduler is BUSY till then and the scheduler goes on running.
    REQUIRE(e.create(kSched, 1, e.script({{LOG, 0xD1}, {END, 10}}), 3) == 0);
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log() == std::vector<u32>{0xD1});
    CHECK(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == kBusy);
    CHECK(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(1), out(1)}) == 0);
    CHECK(ps_read32(e.b, out(1)) == 10);
    REQUIRE(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == 0);
    CHECK(e.call("cellFiberPpuRunFibers", {kSched}) == kStat);
}

TEST_CASE("cellFiber: signals, joins between fibers, flags, the exit callback, Exit", "[hle][fiber]")
{
    Env e;
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    REQUIRE(e.call("cellFiberPpuInitializeScheduler", {kSched, 0}) == 0);
    const u32 wait_signal = e.fn("cellFiberPpuWaitSignal"), send_signal = e.fn("cellFiberPpuSendSignal"),
              join = e.fn("cellFiberPpuJoinFiber"), wait_flag = e.fn("cellFiberPpuWaitFlag"),
              exit = e.fn("cellFiberPpuExit"), stack_limit = e.fn("cellFiberPpuCheckStackLimit");
    const u32 flag = out(10);
    ps_write32(e.b, flag, 0);
    std::memset(e.b + kOut, 0xEE, 0x50);
    // W's exit callback logs 0xEE (the routine with W's callback argument as its script). A fiber with an exit
    // callback cannot be joined (INVAL): the callback takes the join's place, and the fiber is gone at its end.
    REQUIRE(e.call("_cellFiberPpuAttributeInitialize", {kAttr, kSdk}) == 0);
    CHECK(ps_read64(e.b, kAttr) == 0xF784E782F9EA9359ull);
    CHECK(std::string_view(reinterpret_cast<const char*>(e.b + kAttr + 16)) == "*NO NAME*");
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

    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log() ==
        std::vector<u32>{0x11, 0x21, 0, 0x22, code(kInval), 0x23, 0, 0, 0x12, 0xEE, 0x31, code(kDeadlk)});
    CHECK(ps_read32(e.b, out(0)) == 1);          // numWorker: the signalled fiber is runnable again
    CHECK(ps_read32(e.b, out(1)) == 0xEEEEEEEE); // the refused join wrote nothing
    CHECK(ps_read32(e.b, out(2)) == 0xEEEEEEEE);

    // The flag: CheckFlags makes its waiter ready once it holds the value.
    REQUIRE(e.call("cellFiberPpuCheckFlags", {kSched}) == 0);
    REQUIRE(e.call("cellFiberPpuHasRunnableFiber", {kSched, out(3)}) == 0);
    CHECK(ps_read8(e.b, out(3)) == 0);
    ps_write32(e.b, flag, 1);
    REQUIRE(e.call("cellFiberPpuCheckFlags", {kSched}) == 0);
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0x32});
    CHECK(e.call("cellFiberPpuSendSignal", {fiber_at(0), 0}) == kStat); // ended and gone

    // Fiber-only calls from the thread: PERM before any check of their arguments.
    CHECK(e.call("cellFiberPpuYield") == kPerm);
    CHECK(e.call("cellFiberPpuWaitSignal") == kPerm);
    CHECK(e.call("cellFiberPpuWaitFlag", {flag, 1}) == kPerm);
    CHECK(e.call("cellFiberPpuExit", {0}) == kPerm);
    CHECK(e.call("cellFiberPpuSetPriority", {1}) == kPerm);
    CHECK(e.call("cellFiberPpuSetPriority", {4}) == kPerm);
    CHECK(e.call("cellFiberPpuCheckStackLimit") == kPerm);
    for (const u32 i : {1u, 2u, 3u}) {
        ps_write32(e.b, out(4), 0xEEEEEEEE);
        REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(i), out(4)}) == 0);
        CHECK(ps_read32(e.b, out(4)) == 0);
    }
    REQUIRE(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == 0);
}

TEST_CASE("cellFiber: autoCheckFlags; a waiting fiber keeps the scheduler busy; a signal kept for the next wait",
    "[hle][fiber]")
{
    Env e;
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    REQUIRE(e.call("_cellFiberPpuSchedulerAttributeInitialize", {kAttr, kSdk}) == 0);
    ps_write8(e.b, kAttr + 16, 1);
    ps_write32(e.b, kAttr + 20, 200);
    REQUIRE(e.call("cellFiberPpuInitializeScheduler", {kSched, kAttr}) == 0);
    const u32 wait_flag = e.fn("cellFiberPpuWaitFlag"), wait_signal = e.fn("cellFiberPpuWaitSignal");
    const u32 flag = out(10);
    ps_write32(e.b, flag, 0);
    REQUIRE(e.create(kSched, 0, e.script({{CALL, wait_flag, flag, 1}, {LOG, 0x41}, {END, 0}}), 0) == 0);
    // RunFibers returns once no fiber is runnable, autoCheckFlags or not: the flags are looked at when the scheduler
    // picks a fiber, at most every interval.
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log().empty());
    // The flag set from another host thread, atomically as a PPU word store is (a ThreadSanitizer build checks that
    // the scheduler's reads are too, D-571).
    std::thread setter([&] { hle::atomic_store_be32(e.b, flag, 1); });
    setter.join();

    // A signal sent before the wait is kept; a fiber waiting for a second one keeps the scheduler from finishing. The
    // next pick finds the flag set.
    REQUIRE(e.create(kSched, 1, e.script({{CALL, wait_signal}, {LOG, 0x51}, {CALL, wait_signal}, {END, 0}}), 0) ==
        0);
    ps_write32(e.b, out(0), 0xEEEEEEEE);
    REQUIRE(e.call("cellFiberPpuSendSignal", {fiber_at(1), out(0)}) == 0);
    CHECK(ps_read32(e.b, out(0)) == 0); // not waiting yet: nothing made runnable
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0x51, 0, 0x41});
    CHECK(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == kBusy);
    REQUIRE(e.call("cellFiberPpuSendSignal", {fiber_at(1), 0}) == 0);
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(e.log() == std::vector<u32>{0});
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(0), out(1)}) == 0);
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(1), out(1)}) == 0);
    REQUIRE(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == 0);
}

TEST_CASE("cellFiber: contexts: Run, Switch, ReturnToThread, Finalize", "[hle][fiber]")
{
    Env e;
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    const u32 sw = e.fn("cellFiberPpuContextSwitch"), back = e.fn("cellFiberPpuContextReturnToThread"),
              self = e.fn("cellFiberPpuContextSelf"), limit = e.fn("cellFiberPpuContextCheckStackLimit");
    const u32 c1 = context_at(0), c2 = context_at(1);
    REQUIRE(e.call("_cellFiberPpuContextAttributeInitialize", {kAttr, kSdk}) == 0);
    CHECK(ps_read64(e.b, kAttr) == 0x992D3B630B7DF290ull);
    REQUIRE(e.call("cellFiberPpuContextInitialize",
                {c1, kRoutine,
                    e.script({{LOG, 0xA}, {CALL, self}, {CALL, sw, c2, out(0)}, {LOG, 0xC}, {CALL, limit},
                        {CALL, back, 33}, {LOG, 0xE}, {CALL, back, 44}, {END, 0}}),
                    stack_at(0), kStackSize, kAttr}) == 0);
    // A context's entry never returns (libfiber leaves no return address there): c2 goes back to the thread too.
    REQUIRE(e.call("cellFiberPpuContextInitialize",
                {c2, kRoutine, e.script({{LOG, 0xB}, {CALL, sw, c1, out(1)}, {LOG, 0xD}, {CALL, back, 55}, {END, 0}}),
                    stack_at(1), kStackSize, 0}) == 0);
    CHECK(ps_read32(e.b, stack_at(0)) == 0xDEADBEAF); // the stack's overflow marker
    CHECK(e.call("cellFiberPpuContextSelf") == 0);
    CHECK(e.call("cellFiberPpuContextSwitch", {c2, out(0), 0}) == kPerm);
    CHECK(e.call("cellFiberPpuContextReturnToThread", {1}) == kPerm);

    REQUIRE(e.call("cellFiberPpuContextRun", {c1, out(2), out(3), 0}) == 0);
    CHECK(e.log() == std::vector<u32>{0xA, c1, 0xB, 0, 0xC, 0});
    CHECK(static_cast<s32>(ps_read32(e.b, out(2))) == 33); // the cause
    CHECK(ps_read32(e.b, out(3)) == c1);                    // who came back
    CHECK(ps_read32(e.b, out(0)) == c2);                    // who switched to c1
    REQUIRE(e.call("cellFiberPpuContextRun", {c1, out(2), out(3), 0}) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0xE});
    CHECK(ps_read32(e.b, out(2)) == 44);
    // c2 resumed by the thread: its switch says 0 (the thread).
    REQUIRE(e.call("cellFiberPpuContextRun", {c2, out(2), out(3), 0}) == 0);
    CHECK(e.log() == std::vector<u32>{0, 0xD});
    CHECK(ps_read32(e.b, out(1)) == 0);
    CHECK(ps_read32(e.b, out(2)) == 55);
    CHECK(ps_read32(e.b, out(3)) == c2);
    // Finalize: both suspended.
    REQUIRE(e.call("cellFiberPpuContextFinalize", {c1}) == 0);
    REQUIRE(e.call("cellFiberPpuContextFinalize", {c2}) == 0);
    CHECK(e.call("cellFiberPpuContextRun", {c1, out(2), out(3), 0}) == kStat);
    CHECK(e.call("cellFiberPpuContextFinalize", {c1}) == kStat);
}

TEST_CASE("cellFiber: RunScheduler and EnterScheduler pick contexts through a callback", "[hle][fiber]")
{
    Env e;
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    const u32 enter = e.fn("cellFiberPpuContextEnterScheduler"), back = e.fn("cellFiberPpuContextReturnToThread"),
              sw = e.fn("cellFiberPpuContextSwitch");
    const u32 c3 = context_at(2), c4 = context_at(3);
    // The callback is the routine too: its first argument a script that ends with the context to run.
    const u32 pick3 = e.script({{END, c3}}), pick4 = e.script({{END, c4}}), none = e.script({{END, 0}});
    REQUIRE(e.call("cellFiberPpuContextInitialize",
                {c3, kRoutine,
                    e.script({{LOG, 0x3}, {CALL, enter, kRoutine, pick4}, {LOG, 0x33}, {CALL, back, 5}, {END, 0}}),
                    stack_at(2), kStackSize, 0}) == 0);
    REQUIRE(e.call("cellFiberPpuContextInitialize",
                {c4, kRoutine, e.script({{LOG, 0x4}, {CALL, sw, c3, out(0)}, {END, 0}}), stack_at(3), kStackSize, 0}) ==
        0);
    ps_write32(e.b, out(0), 0xEEEEEEEE);
    REQUIRE(e.call("cellFiberPpuContextRunScheduler", {kRoutine, pick3, 0, out(2), out(3), 0}) == 0);
    CHECK(e.log() == std::vector<u32>{0x3, 0x4, 0, 0x33});
    CHECK(ps_read32(e.b, out(2)) == 5);
    CHECK(ps_read32(e.b, out(3)) == c3);
    CHECK(ps_read32(e.b, out(0)) == 0xEEEEEEEE); // c4 never resumed
    // A callback that picks nothing: Run returns at once.
    ps_write32(e.b, out(2), 0xFFFF);
    REQUIRE(e.call("cellFiberPpuContextRunScheduler", {kRoutine, none, 0, out(2), out(3), 0}) == 0);
    CHECK(ps_read32(e.b, out(2)) == 0);
    CHECK(ps_read32(e.b, out(3)) == 0);
    CHECK(e.log().empty());
    // c3 and c4 stay suspended: nothing of theirs is outside guest memory.
}

TEST_CASE("cellFiber: a worker control's fibers run on two worker threads; joined from a third", "[hle][fiber]")
{
    Env e;
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    REQUIRE(e.call("_cellFiberPpuUtilWorkerControlAttributeInitialize", {kAttr, 0}) == 0);
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlInitializeWithAttribute", {kControl, kAttr}) == 0);
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlSetPollingMode", {kControl, 0, 500}) == 0);
    const u32 yield = e.fn("cellFiberPpuYield"), run = e.fn("cellFiberPpuUtilWorkerControlRunFibers");
    std::atomic<u32> returned{0};
    std::vector<std::thread> workers;
    u32 tls[2] = {};
    for (u32 w = 0; w < 2; w++) {
        tls[w] = e.f.hle->alloc_tls();
        REQUIRE(tls[w]);
    }
    for (u32 w = 0; w < 2; w++) {
        workers.emplace_back([&, w] {
            GuestThread worker(e.f, 1 + w, "worker");
            worker.ctx.gpr[13] = tls[w] + hle::kTlsR13Offset;
            if (hle::call_guest(worker.ctx, run, {kControl}) == 0) returned++;
        });
    }
    for (u32 i = 0; i < 6; i++) {
        const u32 s = e.script({{CALLQ, yield}, {CALLQ, yield}, {END, 100 + i}});
        REQUIRE(e.call("cellFiberPpuUtilWorkerControlCreateFiber",
                    {kControl, fiber_at(i), kRoutine, s, i % 4, stack_at(i), kStackSize, 0}) == 0);
    }
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlWakeup", {kControl}) == 0);
    for (u32 i = 0; i < 6; i++) {
        INFO("fiber " << i);
        REQUIRE(e.call("cellFiberPpuUtilWorkerControlJoinFiber", {kControl, fiber_at(i), out(i)}) == 0);
        CHECK(ps_read32(e.b, out(i)) == 100 + i);
    }
    // Finalize before Shutdown: STAT (the control still runs); Shutdown lets the workers' RunFibers return.
    CHECK(e.call("cellFiberPpuUtilWorkerControlFinalize", {kControl}) == kStat);
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlShutdown", {kControl}) == 0);
    for (std::thread& t : workers) t.join();
    CHECK(returned == 2);
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlFinalize", {kControl}) == 0);
    // Initialize without an attribute, Finalize without Shutdown: STAT; then the two in their turn.
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlInitialize", {kControl}) == 0);
    CHECK(e.call("cellFiberPpuUtilWorkerControlFinalize", {kControl}) == kStat);
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlShutdown", {kControl}) == 0);
    REQUIRE(e.call("cellFiberPpuUtilWorkerControlFinalize", {kControl}) == 0);
}

TEST_CASE("cellFiber: the argument checks, attributes and the trace's states", "[hle][fiber]")
{
    Env e;
    // Contexts need no _cellFiberPpuInitialize (nor a smallest stack); schedulers do.
    CHECK(e.call("cellFiberPpuContextInitialize", {context_at(0), kRoutine, 0, stack_at(0), kStackSize, 0}) == 0);
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    CHECK(e.call("cellFiberPpuInitializeScheduler", {0, 0}) == kNull);
    CHECK(e.call("cellFiberPpuInitializeScheduler", {kSched + 8, 0}) == kAlign);
    CHECK(e.call("_cellFiberPpuAttributeInitialize", {0, 0}) == kNull);
    CHECK(e.call("_cellFiberPpuAttributeInitialize", {kAttr + 4, 0}) == kAlign);
    CHECK(e.call("_cellFiberPpuContextAttributeInitialize", {kAttr + 2, 0}) == kAlign);
    CHECK(e.call("cellFiberPpuRunFibers", {kSched}) == kStat); // not made
    REQUIRE(e.call("cellFiberPpuInitializeScheduler", {kSched, 0}) == 0);
    CHECK(e.create(kSched, 0, kScripts, 4) == kInval); // priorities 0-3
    CHECK(e.call("cellFiberPpuCreateFiber", {kSched, fiber_at(0) + 64, kRoutine, 0, 0, stack_at(0), kStackSize, 0}) ==
        kAlign);
    CHECK(e.call("cellFiberPpuCreateFiber", {kSched, fiber_at(0), kRoutine, 0, 0, stack_at(0) + 4, kStackSize, 0}) ==
        kAlign);
    CHECK(e.call("cellFiberPpuCreateFiber", {kSched, fiber_at(0), 0, 0, 0, stack_at(0), kStackSize, 0}) == kNull);
    // An attribute that is not one (its magic): INVAL.
    std::memset(e.b + kAttr, 0, 256);
    CHECK(e.create(kSched, 0, kScripts, 0, kAttr) == kInval);
    // No smallest stack: a fiber with 0x100 bytes is made and runs.
    REQUIRE(e.create(kSched, 2, e.script({{END, 2}}), 0, 0, 0x100) == 0);
    REQUIRE(e.create(kSched, 0, e.script({{END, 1}}), 0) == 0);
    CHECK(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == kBusy);
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    for (const u32 i : {0u, 2u}) {
        REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(i), out(0)}) == 0);
        CHECK(ps_read32(e.b, out(0)) == (i == 0 ? 1u : 2u));
    }

    // The trace: a mode is 1 to 3; initialize, start, stop, finalize, each in its turn.
    CHECK(e.call("cellFiberPpuSchedulerTraceStart", {kSched}) == kStat);
    CHECK(e.call("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer + 8, 0x1000, 1}) == kAlign);
    CHECK(e.call("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0, 1}) == kInval);
    CHECK(e.call("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0x1000, 0}) == kInval);
    REQUIRE(e.call("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0x1000, 1}) == 0);
    CHECK(ps_read32(e.b, kBuffer + 4) == (0x1000 - 0x80) / 16); // the records it holds
    CHECK(e.call("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0x1000, 1}) == kStat);
    REQUIRE(e.call("cellFiberPpuSchedulerTraceStart", {kSched}) == 0);
    CHECK(e.call("cellFiberPpuSchedulerTraceStart", {kSched}) == kStat);
    // A fiber that yields once while tracing: a record of each run, its yield and its exit, 16 bytes each from +0x80.
    REQUIRE(e.create(kSched, 1, e.script({{CALLQ, e.fn("cellFiberPpuYield")}, {END, 0}}), 0) == 0);
    REQUIRE(e.call("cellFiberPpuRunFibers", {kSched}) == 0);
    CHECK(ps_read32(e.b, kBuffer) == 4);
    CHECK(ps_read8(e.b, kBuffer + 0x80) == 0x80); // runs
    CHECK(ps_read32(e.b, kBuffer + 0x88) == fiber_at(1));
    CHECK(ps_read8(e.b, kBuffer + 0x90) == 0x83); // yields
    CHECK(ps_read8(e.b, kBuffer + 0xA0) == 0x81); // runs again
    CHECK(ps_read8(e.b, kBuffer + 0xB0) == 0x82); // exits
    REQUIRE(e.call("cellFiberPpuSchedulerTraceStop", {kSched}) == 0);
    REQUIRE(e.call("cellFiberPpuSchedulerTraceFinalize", {kSched}) == 0);
    CHECK(e.call("cellFiberPpuSchedulerTraceStop", {kSched}) == kStat);
    REQUIRE(e.call("cellFiberPpuJoinFiber", {fiber_at(1), out(0)}) == 0);
    REQUIRE(e.call("cellFiberPpuFinalizeScheduler", {kSched}) == 0);
}

TEST_CASE("cellFiber: the thread exiting inside a fiber unwinds the thread", "[hle][fiber]")
{
    Env e;
    REQUIRE(code(e.init()) == 0xFFFF9000u);
    REQUIRE(e.call("cellFiberPpuInitializeScheduler", {kSched, 0}) == 0);
    const u32 thread_exit = e.fn("sys_ppu_thread_exit", "sysPrxForUser");
    REQUIRE(e.create(kSched, 0, e.script({{LOG, 0x77}, {CALL, thread_exit, 77}, {END, 0}}), 0) == 0);
    CHECK_THROWS_AS(e.call("cellFiberPpuRunFibers", {kSched}), lv2::ThreadExit);
    CHECK(e.log() == std::vector<u32>{0x77});
}
