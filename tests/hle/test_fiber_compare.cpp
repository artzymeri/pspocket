// cellFiber against Sony's libfiber (D-568, D-597): test_fiber.cpp's scripts (fiber_script.hpp: fibers and contexts
// running a guest routine that logs markers and the answers of the calls it makes) sent to pspocket's library and to
// Sony's module, each scenario in a process of its own, the scripts calling the half's own exports. Compared: every
// answer, the log (the order the fibers and contexts ran in, and what each call answered inside them), every output
// word (statuses, causes, the context that switched, the scheduler, numWorker), and the attribute structures as the
// initializers leave them (a game sets its fields over them). The schedulers', fibers', contexts' and worker controls'
// own bytes are notes: the SDK keeps them opaque, pspocket keeps libfiber's state on the host (D-568), Sony's keeps it
// there. Worker threads take fibers in an order of their own: only the joins' statuses are compared there. Not sent:
// cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs and DisconnectEventQueue (their arguments are not known). No
// game data; skipped without the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "fiber_script.hpp"
#include "pspocket/lv2/kernel.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace pspocket::hle::test::fiber;

namespace {

constexpr u32 kCode = kData, kRoutine = kData + 0x200, kMissing = kData + 0x280, kLog = kData + 0x1000,
              kScripts = kData + 0x3000, kSched = kData + 0x8000, kAttr = kData + 0x8400, kFibers = kData + 0x9000,
              kContexts = kData + 0xD000, kOut = kData + 0x10000, kStacks0 = kData + 0x20000,
              kControl = kData + 0x130000, kBuffer = kData + 0x131000;
constexpr u32 kStackSize = 0x8000, kSdk = 0x330000;

u32 fiber_at(u32 i) { return kFibers + 0x400 * i; }
u32 context_at(u32 i) { return kContexts + 0x300 * i; }
u32 stack_at(u32 i) { return kStacks0 + kStackSize * i; }
u32 out(u32 i) { return kOut + 8 * i; }

struct Run : Half<> {
    u32 next_script = kScripts;

    Run(const Setup& s, bool sony_half) : Half(s, sony_half, "cellFiber")
    {
        write_routine(b, kCode, kRoutine, kLog);
        // A function a half does not export answers 0x8076ffff when a script calls it (rather than jumping to 0).
        namespace as = pspocket::recomp::ppu::as;
        const u32 missing[] = {as::lis(3, static_cast<s32>(0x8076)), as::ori(3, 3, 0xFFFF), as::blr()};
        for (u32 i = 0; i < 3; i++) ps_write32(b, kMissing + 8 + 4 * i, missing[i]);
        ps_write32(b, kMissing, kMissing + 8);
        ps_write32(b, kMissing + 4, 0);
        ps_write32(b, kLog, 0);
    }

    // The export's descriptor for a script; a line when the half has none.
    u32 fn(std::string_view name)
    {
        const u32 d = side.descriptor(name);
        if (!d) o.line(std::string(name) + " for a script", "(not exported)");
        return d ? d : kMissing;
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

    // A word as the test knows it: one of its fibers, contexts or objects, else the number.
    static std::string word(u32 v)
    {
        for (u32 i = 0; i < 8; i++)
            if (v == fiber_at(i)) return std::format("fiber {}", i);
        for (u32 i = 0; i < 4; i++)
            if (v == context_at(i)) return std::format("context {}", i);
        if (v == kSched) return "the scheduler";
        if (v == kControl) return "the worker control";
        return std::format("{:#x}", v);
    }

    // The log since the last look, emptied.
    void log(const std::string& what)
    {
        std::string s;
        const u32 n = ps_read32(b, kLog);
        for (u32 i = 0; i < n && i < 1000; i++) s += (i ? " " : "") + word(ps_read32(b, kLog + 4 + 4 * i));
        ps_write32(b, kLog, 0);
        o.line("the log after " + what, s.empty() ? "(empty)" : s);
    }

    void word_at(const std::string& what, u32 addr) { o.line(what, word(ps_read32(b, addr))); }

    u64 create(u32 sched, u32 i, u32 script_addr, u32 prio, u32 attr = 0)
    {
        return r("cellFiberPpuCreateFiber",
            {sched, fiber_at(i), kRoutine, script_addr, prio, stack_at(i), kStackSize, attr},
            std::format("CreateFiber(fiber {}, priority {}{})", i, prio, attr ? ", with an attribute" : ""));
    }

    // An attribute initializer: the structure (filled with 0xAA first) as it leaves it.
    void attribute(const char* function, u32 size, u32 sdk)
    {
        std::memset(b + kAttr, 0xAA, 0x200);
        const std::string key = std::format("{}(attr, {:#x})", function, sdk);
        r(function, {kAttr, sdk}, key);
        o.line(key + ": the attribute", hex(b + kAttr, size));
    }

    void object(const std::string& what, u32 addr, u32 size) { o.note(what + ": " + hex(b + addr, size)); }
};

// Each scenario of test_fiber.cpp, in a process of its own.
using Scenario = void (*)(Run&);

void by_priority(Run& h)
{
    h.r("cellFiberPpuInitializeScheduler", {kSched, 0}, "InitializeScheduler before _cellFiberPpuInitialize");
    h.r("_cellFiberPpuInitialize", {});
    h.attribute("_cellFiberPpuSchedulerAttributeInitialize", 256, kSdk);
    h.r("cellFiberPpuInitializeScheduler", {kSched, kAttr}, "InitializeScheduler");
    h.object("the scheduler after InitializeScheduler", kSched, 512);
    const u32 yield = h.fn("cellFiberPpuYield"), self = h.fn("cellFiberPpuSelf");
    h.create(kSched, 0, h.script({{LOG, 0xA1}, {CALL, yield}, {LOG, 0xA2}, {END, 7}}), 1);
    h.create(kSched, 1, h.script({{LOG, 0xB1}, {CALL, yield}, {LOG, 0xB2}, {END, 8}}), 1);
    h.create(kSched, 2, h.script({{LOG, 0xC1}, {CALL, self}, {END, 9}}), 0);
    h.object("fiber 0 after CreateFiber", fiber_at(0), 896);
    std::memset(h.b + kOut, 0xEE, 0x100);
    h.r("cellFiberPpuHasRunnableFiber", {kSched, out(0)}, "HasRunnableFiber before RunFibers");
    h.o.line("HasRunnableFiber before RunFibers: the flag", hex(h.b + out(0), 4));
    h.r("cellFiberPpuSelf", {}, "Self from the thread");
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers");
    h.log("RunFibers");
    h.r("cellFiberPpuHasRunnableFiber", {kSched, out(0)}, "HasRunnableFiber after RunFibers");
    h.o.line("HasRunnableFiber after RunFibers: the flag", hex(h.b + out(0), 4));
    h.object("fiber 0 after its end", fiber_at(0), 896);
    // Their ends: joined from the thread once ended; a joined fiber is gone.
    for (const auto& [i, key] : {std::pair{0u, "JoinFiber(fiber 0) from the thread"},
             std::pair{0u, "JoinFiber(fiber 0) from the thread, again"},
             std::pair{2u, "JoinFiber(fiber 2) from the thread"}}) {
        ps_write32(h.b, out(1), 0xEEEEEEEE);
        h.r("cellFiberPpuJoinFiber", {fiber_at(i), out(1)}, key);
        h.word_at(std::string(key) + ": the status", out(1));
    }
    ps_write32(h.b, out(2), 0xEEEEEEEE);
    h.r("cellFiberPpuGetScheduler", {fiber_at(1), out(2)}, "GetScheduler(fiber 1)");
    h.word_at("GetScheduler(fiber 1): the scheduler", out(2));
    // A fiber's memory, ended and joined, makes a new fiber.
    h.create(kSched, 1, h.script({{LOG, 0xD1}, {END, 10}}), 3);
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers of the new fiber");
    h.log("RunFibers of the new fiber");
    h.r("cellFiberPpuFinalizeScheduler", {kSched}, "FinalizeScheduler");
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers after FinalizeScheduler");
}

void signals(Run& h)
{
    h.r("_cellFiberPpuInitialize", {});
    h.r("cellFiberPpuInitializeScheduler", {kSched, 0}, "InitializeScheduler without an attribute");
    const u32 wait_signal = h.fn("cellFiberPpuWaitSignal"), send_signal = h.fn("cellFiberPpuSendSignal"),
              join = h.fn("cellFiberPpuJoinFiber"), wait_flag = h.fn("cellFiberPpuWaitFlag"),
              exit = h.fn("cellFiberPpuExit"), stack_limit = h.fn("cellFiberPpuCheckStackLimit");
    const u32 flag = out(10);
    ps_write32(h.b, flag, 0);
    std::memset(h.b + kOut, 0xEE, 0x50);
    // W's exit callback logs 0xEE (the routine with W's callback argument as its script).
    h.attribute("_cellFiberPpuAttributeInitialize", 256, kSdk);
    ps_write32(h.b, kAttr + 48, kRoutine);
    ps_write64(h.b, kAttr + 56, h.script({{LOG, 0xEE}, {END, 0}}));
    std::memcpy(h.b + kAttr + 16, "waiter", 7);
    h.create(kSched, 0, h.script({{LOG, 0x11}, {CALL, wait_signal}, {LOG, 0x12}, {CALL, exit, 5}}), 0, kAttr);
    h.create(kSched, 1,
        h.script({{LOG, 0x21}, {CALL, send_signal, fiber_at(0), out(0)}, {LOG, 0x22}, {CALL, join, fiber_at(0), out(1)},
            {LOG, 0x23}, {CALL, stack_limit}, {END, 0}}),
        1);
    h.create(kSched, 2, h.script({{LOG, 0x31}, {CALL, wait_flag, flag, 1}, {LOG, 0x32}, {END, 0}}), 2);
    h.create(kSched, 3, h.script({{CALL, join, fiber_at(3), out(2)}, {END, 0}}), 3);
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers");
    h.log("RunFibers");
    h.o.line("SendSignal's numWorker", hex(h.b + out(0), 4));
    h.word_at("JoinFiber(fiber 0) in fiber 1: the status", out(1));
    h.o.line("JoinFiber(itself) in fiber 3: the status word", hex(h.b + out(2), 4));
    // The flag: CheckFlags readies its waiter once it holds the value.
    h.r("cellFiberPpuCheckFlags", {kSched}, "CheckFlags with the flag 0");
    h.r("cellFiberPpuHasRunnableFiber", {kSched, out(3)}, "HasRunnableFiber with the flag 0");
    h.o.line("HasRunnableFiber with the flag 0: the flag", hex(h.b + out(3), 4));
    ps_write32(h.b, flag, 1);
    h.r("cellFiberPpuCheckFlags", {kSched}, "CheckFlags with the flag 1");
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers with the flag 1");
    h.log("RunFibers with the flag 1");
    h.r("cellFiberPpuSendSignal", {fiber_at(0), 0}, "SendSignal to an ended fiber");
    // Fiber-only calls from the thread.
    h.r("cellFiberPpuYield", {}, "Yield from the thread");
    h.r("cellFiberPpuWaitSignal", {}, "WaitSignal from the thread");
    h.r("cellFiberPpuWaitFlag", {flag, 1}, "WaitFlag from the thread");
    h.r("cellFiberPpuExit", {0}, "Exit from the thread");
    h.r("cellFiberPpuSetPriority", {1}, "SetPriority(1) from the thread");
    h.r("cellFiberPpuSetPriority", {4}, "SetPriority(4) from the thread");
    h.r("cellFiberPpuCheckStackLimit", {}, "CheckStackLimit from the thread");
    for (u32 i = 1; i < 4; i++) {
        ps_write32(h.b, out(4), 0xEEEEEEEE);
        const std::string key = std::format("JoinFiber(fiber {}) from the thread", i);
        h.r("cellFiberPpuJoinFiber", {fiber_at(i), out(4)}, key);
        h.word_at(key + ": the status", out(4));
    }
    h.r("cellFiberPpuFinalizeScheduler", {kSched}, "FinalizeScheduler");
}

void auto_check(Run& h)
{
    h.r("_cellFiberPpuInitialize", {});
    h.attribute("_cellFiberPpuSchedulerAttributeInitialize", 256, kSdk);
    ps_write8(h.b, kAttr + 16, 1);
    ps_write32(h.b, kAttr + 20, 200);
    h.r("cellFiberPpuInitializeScheduler", {kSched, kAttr}, "InitializeScheduler with autoCheckFlags every 200 us");
    const u32 wait_flag = h.fn("cellFiberPpuWaitFlag"), wait_signal = h.fn("cellFiberPpuWaitSignal");
    const u32 flag = out(10);
    ps_write32(h.b, flag, 0);
    h.create(kSched, 0, h.script({{CALL, wait_flag, flag, 1}, {LOG, 0x41}, {END, 0}}), 0);
    // With autoCheckFlags RunFibers waits for the flag, set from another host thread (as a PPU word store is).
    u8* b = h.b;
    std::thread setter([b, flag] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        hle::atomic_store_be32(b, flag, 1);
    });
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers waiting for the flag");
    setter.join();
    h.log("RunFibers waiting for the flag");
    // A signal sent before the wait is kept; a fiber waiting for a second keeps the scheduler from finishing.
    h.create(kSched, 1, h.script({{CALL, wait_signal}, {LOG, 0x51}, {CALL, wait_signal}, {END, 0}}), 0);
    ps_write32(h.b, out(0), 0xEEEEEEEE);
    h.r("cellFiberPpuSendSignal", {fiber_at(1), out(0)}, "SendSignal before the wait");
    h.o.line("SendSignal before the wait: numWorker", hex(h.b + out(0), 4));
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers with a signal kept");
    h.log("RunFibers with a signal kept");
    h.r("cellFiberPpuFinalizeScheduler", {kSched}, "FinalizeScheduler with a fiber waiting");
    h.r("cellFiberPpuSendSignal", {fiber_at(1), 0}, "SendSignal to the waiting fiber");
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers after the second signal");
    h.log("RunFibers after the second signal");
    for (const u32 i : {0u, 1u}) h.r("cellFiberPpuJoinFiber", {fiber_at(i), out(1)}, std::format("JoinFiber(fiber {})", i));
    h.r("cellFiberPpuFinalizeScheduler", {kSched}, "FinalizeScheduler");
}

void contexts(Run& h)
{
    h.r("_cellFiberPpuInitialize", {});
    const u32 sw = h.fn("cellFiberPpuContextSwitch"), back = h.fn("cellFiberPpuContextReturnToThread"),
              self = h.fn("cellFiberPpuContextSelf"), limit = h.fn("cellFiberPpuContextCheckStackLimit");
    const u32 c1 = context_at(0), c2 = context_at(1);
    std::memset(h.b + kOut, 0xEE, 0x50);
    h.attribute("_cellFiberPpuContextAttributeInitialize", 128, kSdk);
    h.r("cellFiberPpuContextInitialize",
        {c1, kRoutine,
            h.script({{LOG, 0xA}, {CALL, self}, {CALL, sw, c2, out(0)}, {LOG, 0xC}, {CALL, limit}, {CALL, back, 33},
                {LOG, 0xE}, {CALL, back, 44}, {END, 0}}),
            stack_at(0), kStackSize, kAttr},
        "ContextInitialize(context 0, with an attribute)");
    h.r("cellFiberPpuContextInitialize",
        {c2, kRoutine, h.script({{LOG, 0xB}, {CALL, sw, c1, out(1)}, {LOG, 0xD}, {END, 0}}), stack_at(1), kStackSize, 0},
        "ContextInitialize(context 1)");
    h.object("context 0 after ContextInitialize", c1, 640);
    h.r("cellFiberPpuContextSelf", {}, "ContextSelf from the thread");
    h.r("cellFiberPpuContextSwitch", {c2, out(0), 0}, "ContextSwitch from the thread");
    h.r("cellFiberPpuContextReturnToThread", {1}, "ContextReturnToThread from the thread");
    const auto run_context = [&](u32 c, const std::string& what) {
        ps_write32(h.b, out(2), 0xEEEEEEEE);
        ps_write32(h.b, out(3), 0xEEEEEEEE);
        h.r("cellFiberPpuContextRun", {c, out(2), out(3), 0}, "ContextRun " + what);
        h.log("ContextRun " + what);
        h.word_at("ContextRun " + what + ": the cause", out(2));
        h.word_at("ContextRun " + what + ": who came back", out(3));
    };
    run_context(c1, "(context 0, first)");
    h.word_at("who switched to context 0", out(0));
    run_context(c1, "(context 0, again)");
    // Context 1 resumed by the thread: its switch says 0 (the thread); its entry returns; it has ended.
    run_context(c2, "(context 1)");
    h.word_at("who context 1's switch came back from", out(1));
    run_context(c2, "(context 1, ended)");
    h.r("cellFiberPpuContextFinalize", {c1}, "ContextFinalize(context 0, parked)");
    h.r("cellFiberPpuContextFinalize", {c2}, "ContextFinalize(context 1, ended)");
    run_context(c1, "(context 0, finalized)");
    h.r("cellFiberPpuContextFinalize", {c1}, "ContextFinalize(context 0) again");
}

void schedulers(Run& h)
{
    h.r("_cellFiberPpuInitialize", {});
    const u32 enter = h.fn("cellFiberPpuContextEnterScheduler"), back = h.fn("cellFiberPpuContextReturnToThread"),
              sw = h.fn("cellFiberPpuContextSwitch");
    const u32 c3 = context_at(2), c4 = context_at(3);
    // The callback is the routine too: its first argument a script that ends with the context to run.
    const u32 pick3 = h.script({{END, c3}}), pick4 = h.script({{END, c4}}), none = h.script({{END, 0}});
    std::memset(h.b + kOut, 0xEE, 0x50);
    h.r("cellFiberPpuContextInitialize",
        {c3, kRoutine, h.script({{LOG, 0x3}, {CALL, enter, kRoutine, pick4}, {LOG, 0x33}, {CALL, back, 5}, {END, 0}}),
            stack_at(2), kStackSize, 0},
        "ContextInitialize(context 2)");
    h.r("cellFiberPpuContextInitialize",
        {c4, kRoutine, h.script({{LOG, 0x4}, {CALL, sw, c3, out(0)}, {END, 0}}), stack_at(3), kStackSize, 0},
        "ContextInitialize(context 3)");
    h.r("cellFiberPpuContextRunScheduler", {kRoutine, pick3, 0, out(2), out(3), 0}, "ContextRunScheduler picking 2");
    h.log("ContextRunScheduler picking 2");
    h.word_at("ContextRunScheduler picking 2: the cause", out(2));
    h.word_at("ContextRunScheduler picking 2: who came back", out(3));
    h.word_at("who switched to context 2", out(0));
    ps_write32(h.b, out(2), 0xFFFF);
    ps_write32(h.b, out(3), 0xEEEEEEEE);
    h.r("cellFiberPpuContextRunScheduler", {kRoutine, none, 0, out(2), out(3), 0}, "ContextRunScheduler picking none");
    h.log("ContextRunScheduler picking none");
    h.word_at("ContextRunScheduler picking none: the cause", out(2));
    h.word_at("ContextRunScheduler picking none: who came back", out(3));
    // Contexts 2 and 3 stay parked: the process's end unwinds them.
}

void workers(Run& h)
{
    h.r("_cellFiberPpuInitialize", {});
    h.attribute("_cellFiberPpuUtilWorkerControlAttributeInitialize", 384, 0);
    h.r("cellFiberPpuUtilWorkerControlInitializeWithAttribute", {kControl, kAttr}, "WorkerControlInitializeWithAttribute");
    h.object("the worker control after its initialization", kControl, 768);
    h.r("cellFiberPpuUtilWorkerControlSetPollingMode", {kControl, 0, 500}, "SetPollingMode(0, 500)");
    const u32 yield = h.fn("cellFiberPpuYield");
    // The workers' RunFibers, called from their own host threads (their descriptor taken here, on the test's).
    const u32 run_fibers = h.side.descriptor("cellFiberPpuUtilWorkerControlRunFibers");
    std::atomic<u64> answers[2] = {~0ull, ~0ull};
    std::vector<std::thread> threads;
    if (run_fibers)
        for (u32 w = 0; w < 2; w++)
            threads.emplace_back([&, w] {
                GuestThread worker(h.f, 1 + w, "worker");
                answers[w] = hle::call_guest(worker.ctx, run_fibers, {kControl});
            });
    for (u32 i = 0; i < 6; i++) {
        const u32 sc = h.script({{CALLQ, yield}, {CALLQ, yield}, {END, 100 + i}});
        h.r("cellFiberPpuUtilWorkerControlCreateFiber",
            {kControl, fiber_at(i), kRoutine, sc, i % 4, stack_at(i), kStackSize, 0},
            std::format("WorkerControlCreateFiber(fiber {})", i));
    }
    h.r("cellFiberPpuUtilWorkerControlWakeup", {kControl}, "WorkerControlWakeup");
    for (u32 i = 0; i < 6; i++) {
        ps_write32(h.b, out(i), 0xEEEEEEEE);
        const std::string key = std::format("WorkerControlJoinFiber(fiber {})", i);
        h.r("cellFiberPpuUtilWorkerControlJoinFiber", {kControl, fiber_at(i), out(i)}, key);
        h.word_at(key + ": the status", out(i));
    }
    h.r("cellFiberPpuUtilWorkerControlFinalize", {kControl}, "WorkerControlFinalize with the workers running");
    h.r("cellFiberPpuUtilWorkerControlShutdown", {kControl}, "WorkerControlShutdown");
    for (std::thread& t : threads) t.join();
    for (u32 w = 0; w < 2; w++) h.o.line(std::format("worker {}'s RunFibers", w), answer(answers[w]));
    h.r("cellFiberPpuUtilWorkerControlCreateFiber",
        {kControl, fiber_at(7), kRoutine, kScripts, 0, stack_at(7), kStackSize, 0},
        "WorkerControlCreateFiber after Shutdown");
    h.r("cellFiberPpuUtilWorkerControlFinalize", {kControl}, "WorkerControlFinalize");
    h.r("cellFiberPpuUtilWorkerControlInitialize", {kControl}, "WorkerControlInitialize without an attribute");
    h.r("cellFiberPpuUtilWorkerControlFinalize", {kControl}, "WorkerControlFinalize of that one");
}

void checks(Run& h)
{
    h.r("cellFiberPpuContextInitialize", {context_at(0), kRoutine, 0, stack_at(0), kStackSize, 0},
        "ContextInitialize before _cellFiberPpuInitialize");
    h.r("_cellFiberPpuInitialize", {});
    h.r("_cellFiberPpuInitialize", {}, "_cellFiberPpuInitialize again");
    // The attribute initializers, with the SDK version and without; their checks.
    for (const u32 sdk : {kSdk, 0u}) {
        h.attribute("_cellFiberPpuSchedulerAttributeInitialize", 256, sdk);
        h.attribute("_cellFiberPpuAttributeInitialize", 256, sdk);
        h.attribute("_cellFiberPpuContextAttributeInitialize", 128, sdk);
        h.attribute("_cellFiberPpuUtilWorkerControlAttributeInitialize", 384, sdk);
    }
    h.r("_cellFiberPpuAttributeInitialize", {0, kSdk}, "_cellFiberPpuAttributeInitialize(NULL)");
    h.r("_cellFiberPpuAttributeInitialize", {kAttr + 4, kSdk}, "_cellFiberPpuAttributeInitialize(misaligned)");
    h.r("_cellFiberPpuContextAttributeInitialize", {kAttr + 2, kSdk},
        "_cellFiberPpuContextAttributeInitialize(misaligned)");
    h.r("_cellFiberPpuSchedulerAttributeInitialize", {0, kSdk}, "_cellFiberPpuSchedulerAttributeInitialize(NULL)");
    h.r("cellFiberPpuInitializeScheduler", {0, 0}, "InitializeScheduler(NULL)");
    h.r("cellFiberPpuInitializeScheduler", {kSched + 8, 0}, "InitializeScheduler(misaligned)");
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers of a scheduler not made");
    std::memset(h.b + kSched, 0, 512);
    h.create(kSched, 0, kScripts, 0);
    h.r("cellFiberPpuInitializeScheduler", {kSched, 0}, "InitializeScheduler");
    h.create(kSched, 0, kScripts, 4);
    h.r("cellFiberPpuCreateFiber", {kSched, fiber_at(0) + 64, kRoutine, 0, 0, stack_at(0), kStackSize, 0},
        "CreateFiber(a misaligned fiber)");
    h.r("cellFiberPpuCreateFiber", {kSched, fiber_at(0), kRoutine, 0, 0, stack_at(0) + 4, kStackSize, 0},
        "CreateFiber(a misaligned stack)");
    h.r("cellFiberPpuCreateFiber", {kSched, fiber_at(0), kRoutine, 0, 0, stack_at(0), 0x100, 0},
        "CreateFiber(a stack of 0x100 bytes)");
    h.r("cellFiberPpuCreateFiber", {kSched, fiber_at(0), 0, 0, 0, stack_at(0), kStackSize, 0},
        "CreateFiber(no entry)");
    h.create(kSched, 0, h.script({{END, 1}}), 0);
    h.create(kSched, 0, h.script({{END, 1}}), 0);
    h.r("cellFiberPpuJoinFiber", {fiber_at(0), out(0)}, "JoinFiber from the thread before its end");
    h.r("cellFiberPpuFinalizeScheduler", {kSched}, "FinalizeScheduler with a fiber not ended");
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers");
    h.log("RunFibers");
    // The trace: initialize, start, stop, finalize, each in its turn.
    h.r("cellFiberPpuSchedulerTraceStart", {kSched}, "SchedulerTraceStart before its initialization");
    h.r("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer + 8, 0x1000, 0}, "SchedulerTraceInitialize(misaligned)");
    h.r("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0, 0}, "SchedulerTraceInitialize(no size)");
    std::memset(h.b + kBuffer, 0xEE, 0x1000);
    h.r("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0x1000, 0}, "SchedulerTraceInitialize");
    h.r("cellFiberPpuSchedulerTraceInitialize", {kSched, kBuffer, 0x1000, 0}, "SchedulerTraceInitialize again");
    h.r("cellFiberPpuSchedulerTraceStart", {kSched}, "SchedulerTraceStart");
    h.r("cellFiberPpuSchedulerTraceStart", {kSched}, "SchedulerTraceStart again");
    // A fiber run while tracing: the records (pspocket writes none; the format is the debugger's).
    h.create(kSched, 1, h.script({{LOG, 0x61}, {CALLQ, h.fn("cellFiberPpuYield")}, {END, 0}}), 0);
    h.r("cellFiberPpuRunFibers", {kSched}, "RunFibers while tracing");
    h.log("RunFibers while tracing");
    h.object("the trace buffer's first 256 bytes", kBuffer, 256);
    h.r("cellFiberPpuSchedulerTraceStop", {kSched}, "SchedulerTraceStop");
    h.r("cellFiberPpuSchedulerTraceFinalize", {kSched}, "SchedulerTraceFinalize");
    h.r("cellFiberPpuSchedulerTraceStop", {kSched}, "SchedulerTraceStop after SchedulerTraceFinalize");
    h.r("cellFiberPpuFinalizeScheduler", {kSched}, "FinalizeScheduler");
}

void thread_exit(Run& h)
{
    h.r("_cellFiberPpuInitialize", {});
    h.r("cellFiberPpuInitializeScheduler", {kSched, 0}, "InitializeScheduler");
    const u32 exit = h.f.hle->link_function("sysPrxForUser", recomp::nid_of("sys_ppu_thread_exit"), "sys_ppu_thread_exit");
    h.create(kSched, 0, h.script({{LOG, 0x77}, {CALL, exit, 77}, {END, 0}}), 0);
    std::string what = "returned";
    try {
        const std::optional<u64> v = h.side.call("cellFiberPpuRunFibers", {kSched});
        what = v ? "returned " + answer(*v) : "(not exported)";
    } catch (const lv2::ThreadExit&) {
        what = "the thread exited";
    }
    h.o.line("RunFibers with a fiber that exits the thread", what);
    h.log("RunFibers with a fiber that exits the thread");
}

Observed half(const Setup& s, bool sony_half, Scenario scenario)
{
    Run h(s, sony_half);
    scenario(h);
    return std::move(h.o);
}

struct Named {
    const char* name;
    Scenario scenario;
};
constexpr Named kScenarios[] = {{"fibers by priority, yields, joins from the thread", by_priority},
    {"signals, joins between fibers, a flag, the exit callback, Exit", signals},
    {"autoCheckFlags, a signal kept, a scheduler kept busy", auto_check},
    {"contexts: Run, Switch, ReturnToThread, an entry's return, Finalize", contexts},
    {"RunScheduler and EnterScheduler", schedulers}, {"a worker control's fibers on two workers", workers},
    {"the argument checks, the attributes and the trace", checks}, {"a thread exit inside a fiber", thread_exit}};

} // namespace

TEST_CASE("cellFiber's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][fiber]")
{
    check_names("cellFiber",
        {"_cellFiberPpuInitialize", "_cellFiberPpuSchedulerAttributeInitialize", "_cellFiberPpuAttributeInitialize",
            "_cellFiberPpuContextAttributeInitialize", "_cellFiberPpuUtilWorkerControlAttributeInitialize",
            "cellFiberPpuInitializeScheduler", "cellFiberPpuFinalizeScheduler", "cellFiberPpuRunFibers",
            "cellFiberPpuCheckFlags", "cellFiberPpuHasRunnableFiber", "cellFiberPpuCreateFiber", "cellFiberPpuExit",
            "cellFiberPpuYield", "cellFiberPpuJoinFiber", "cellFiberPpuSelf", "cellFiberPpuSendSignal",
            "cellFiberPpuWaitSignal", "cellFiberPpuWaitFlag", "cellFiberPpuGetScheduler", "cellFiberPpuSetPriority",
            "cellFiberPpuCheckStackLimit", "cellFiberPpuContextInitialize", "cellFiberPpuContextFinalize",
            "cellFiberPpuContextRun", "cellFiberPpuContextSwitch", "cellFiberPpuContextSelf",
            "cellFiberPpuContextReturnToThread", "cellFiberPpuContextCheckStackLimit",
            "cellFiberPpuContextRunScheduler", "cellFiberPpuContextEnterScheduler",
            "cellFiberPpuSchedulerTraceInitialize", "cellFiberPpuSchedulerTraceFinalize",
            "cellFiberPpuSchedulerTraceStart", "cellFiberPpuSchedulerTraceStop",
            "cellFiberPpuUtilWorkerControlInitialize", "cellFiberPpuUtilWorkerControlInitializeWithAttribute",
            "cellFiberPpuUtilWorkerControlSetPollingMode", "cellFiberPpuUtilWorkerControlRunFibers",
            "cellFiberPpuUtilWorkerControlCreateFiber", "cellFiberPpuUtilWorkerControlWakeup",
            "cellFiberPpuUtilWorkerControlJoinFiber", "cellFiberPpuUtilWorkerControlShutdown",
            "cellFiberPpuUtilWorkerControlFinalize"});
}

TEST_CASE("cellFiber against Sony's libfiber: schedulers, fibers, contexts, worker controls, the checks",
    "[.][hle][lle][compare][fiber]")
{
    const Setup s = setup();
    for (const Named& sc : kScenarios) {
        INFO(sc.name);
        run(s, std::format("cellFiber ({}) against Sony's libfiber", sc.name),
            [&](bool sony) { return half(s, sony, sc.scenario); },
            [&](const Observed& o) {
                // pspocket's own half: test_fiber.cpp's logs and answers.
                if (sc.scenario == by_priority) {
                    CHECK(o.value("the log after RunFibers") == "0xc1 fiber 2 0xa1 0xb1 0x0 0xa2 0x0 0xb2");
                    CHECK(o.value("JoinFiber(fiber 0) from the thread: the status") == "0x7");
                    CHECK(o.value("GetScheduler(fiber 1): the scheduler") == "the scheduler");
                    CHECK(o.value("RunFibers after FinalizeScheduler") == "0x8076000f");
                } else if (sc.scenario == signals) {
                    CHECK(o.value("the log after RunFibers") ==
                        "0x11 0x21 0x0 0x22 0x0 0x12 0xee 0x0 0x23 0x0 0x31 0x80760008");
                    CHECK(o.value("JoinFiber(fiber 0) in fiber 1: the status") == "0x5");
                    CHECK(o.value("Yield from the thread") == "0x80760009");
                } else if (sc.scenario == auto_check) {
                    CHECK(o.value("the log after RunFibers waiting for the flag") == "0x0 0x41");
                    CHECK(o.value("FinalizeScheduler with a fiber waiting") == "0x8076000a");
                } else if (sc.scenario == contexts) {
                    CHECK(o.value("the log after ContextRun (context 0, first)") == "0xa context 0 0xb 0x0 0xc 0x0");
                    CHECK(o.value("ContextRun (context 0, first): the cause") == "0x21");
                    CHECK(o.value("who switched to context 0") == "context 1");
                    CHECK(o.value("ContextRun (context 1, ended)") == "0x8076000f");
                } else if (sc.scenario == schedulers) {
                    CHECK(o.value("the log after ContextRunScheduler picking 2") == "0x3 0x4 0x0 0x33");
                    CHECK(o.value("ContextRunScheduler picking none: the cause") == "0x0");
                } else if (sc.scenario == workers) {
                    for (u32 i = 0; i < 6; i++)
                        CHECK(o.value(std::format("WorkerControlJoinFiber(fiber {}): the status", i)) ==
                            std::format("{:#x}", 100 + i));
                    CHECK(o.value("WorkerControlFinalize with the workers running") == "0x8076000a");
                    CHECK(o.value("worker 0's RunFibers") == "0x0");
                } else if (sc.scenario == checks) {
                    CHECK(o.value("ContextInitialize before _cellFiberPpuInitialize") == "0x80760020");
                    CHECK(o.value("CreateFiber(fiber 0, priority 4)") == "0x80760002");
                    CHECK(o.value("SchedulerTraceStart again") == "0x8076000f");
                } else {
                    CHECK(o.value("RunFibers with a fiber that exits the thread") == "the thread exited");
                    CHECK(o.value("the log after RunFibers with a fiber that exits the thread") == "0x77");
                }
            });
    }
}
