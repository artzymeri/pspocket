// cellFiber: user-level fibers on PPU threads (libfiber), pspocket's own (D-568).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellFiber.cpp/.h: the signatures, the structures' sizes and alignments
// (scheduler 512 / 128, fiber 896 / 128, context 640 / 16, worker control 768 / 128, attributes 8), the error codes and
// the checks RPCS3 makes (null pointers, alignments, priorities above 3); RPCS3 runs no fiber. No Sony module was at
// hand to compare with: the library is firmware-first (D-563). The rest is libfiber's documented model, its details
// pspocket's own (the decision lists them).
//
// How a fiber runs. Each fiber or context has a host thread of its own, started the first time it runs, with its own
// registers and its guest stack. Only one host thread of a PPU thread runs guest code at a time: a baton passes
// between the PPU thread's own host thread (in RunFibers, ContextRun, RunScheduler or a worker's RunFibers) and its
// fibers' and contexts' host threads, each waiting for it otherwise. Whoever holds the baton runs as the PPU thread:
// the lv2 thread of its host thread, the thread's ctx pointing at its registers, r13 the thread's. So a fiber's lv2
// calls are its thread's, blocking the thread as on a PS3, and a fiber of a worker control runs as whichever worker
// thread picked it. A fiber's end, a context's return to the thread, a thread exit inside a fiber (its host thread
// unwinds and the thread's own host thread unwinds after it) and the process's end all pass the baton back. A switch
// costs two condition-variable hand-overs.

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <pthread.h>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellFiber";
constexpr const char* kChan = "cellFiber";

enum : u32 {
    CELL_FIBER_ERROR_AGAIN = 0x80760001,
    CELL_FIBER_ERROR_INVAL = 0x80760002,
    CELL_FIBER_ERROR_DEADLK = 0x80760008,
    CELL_FIBER_ERROR_PERM = 0x80760009,
    CELL_FIBER_ERROR_BUSY = 0x8076000A,
    CELL_FIBER_ERROR_STAT = 0x8076000F,
    CELL_FIBER_ERROR_ALIGN = 0x80760010,
    CELL_FIBER_ERROR_NULL_POINTER = 0x80760011,
    CELL_FIBER_ERROR_NOSYSINIT = 0x80760020,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kSchedulerSize = 512, kFiberSize = 896, kContextSize = 640, kControlSize = 768;
constexpr u32 kSchedulerAttrSize = 256, kFiberAttrSize = 256, kContextAttrSize = 128, kControlAttrSize = 384;
constexpr u32 kSchedulerAlign = 128, kFiberAlign = 128, kContextAlign = 16, kAttrAlign = 8, kControlAlign = 128;
constexpr u32 kStackAlign = 16, kMinStack = 0x200, kRedZone = 288;
constexpr u32 kPriorities = 4; // 0 (first) to 3

bool aligned(u32 addr, u32 n) { return (addr & (n - 1)) == 0; }

// ------------------------------------------------------------------------------------------------- the baton

// Something that can hold a PPU thread's baton: a fiber's or context's host thread, or the PPU thread's own.
struct Runner {
    bool go = false;                 // it has the baton (taken when it wakes)
    bool kill = false;               // it ends instead (a parked fiber or context finalized, or the process's end)
    lv2::PpuThread* owner = nullptr; // the PPU thread it runs as, given with the baton
    u32 from = 0;                    // the context that gave it the baton (0: the PPU thread)
};

// A PPU thread's own host thread while it runs fibers or contexts.
struct ThreadSide : Runner {
    s32 cause = 0;            // ReturnToThread's cause
    u32 returned = 0;         // the context that came back to the thread
    bool thread_exit = false; // the PPU thread ended inside one of them
};

struct Scheduler;

enum class Kind : u8 { Fiber, Context };
enum class State : u8 { Ready, Running, WaitSignal, WaitFlag, WaitJoin, Exited };

struct Lib;

// A fiber (CellFiberPpu) or a context (CellFiberPpuContext): guest code with its own stack and registers.
struct Coroutine : Runner {
    Lib* lib = nullptr;
    Kind kind = Kind::Context;
    u32 addr = 0, entry = 0, stack = 0, stack_size = 0;
    u64 arg = 0;
    std::string name;
    PPUContext ctx{};
    bool started = false;   // its host thread was made
    bool primed = false;    // its registers were set up (at its first run)
    bool host_done = false; // its host thread has ended
    bool ended = false;     // its guest code has ended
    bool holding = false;   // it holds the baton
    bool exiting = false;   // cellFiberPpuExit: its host thread unwinds with exit_status
    ThreadSide* side = nullptr; // the PPU thread side running it, where it goes back to
    // Fibers.
    Scheduler* sched = nullptr;
    State state = State::Ready;
    u32 priority = 0;
    bool signal = false;
    u32 flag_addr = 0;
    bool flag_value = false;
    Coroutine* join_target = nullptr;
    u32 joiners = 0;
    s32 exit_status = 0;
    u32 on_exit = 0;
    u64 on_exit_arg = 0;
};

struct Scheduler {
    u32 addr = 0;
    bool control = false; // a worker control's
    bool auto_check = false;
    u32 auto_interval_us = 0;
    std::array<std::deque<Coroutine*>, kPriorities> ready;
    std::vector<Coroutine*> fibers; // created and not yet released
    u32 runners = 0;                // threads running its fibers (one at most unless a worker control)
    u32 idle = 0;                   // workers waiting for a fiber
    bool shutdown = false;
    s32 polling_mode = 0, polling_timeout = 0;
    u32 trace = 0; // 0 none, 1 initialized, 2 started
};

struct Lib {
    std::mutex m;
    std::condition_variable cv;
    bool initialized = false;
    u8* base = nullptr;
    std::map<u32, std::unique_ptr<Scheduler>> schedulers; // by the scheduler's (or worker control's) address
    std::map<u32, std::unique_ptr<Coroutine>> coroutines; // fibers and contexts by their address
    u32 hosts = 0;                                         // their host threads alive
    bool trace_logged = false;

    ~Lib()
    {
        std::unique_lock lock(m);
        for (auto& [addr, co] : coroutines)
            if (co->started && !co->host_done) co->kill = true;
        cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return hosts == 0; }))
            PS_LOG_ERROR(kChan, "{} fiber host thread(s) still running guest code at the process's end", hosts);
    }
};

Lib& lib() { return HleProcess::current()->state<Lib>(); }

thread_local Coroutine* t_current = nullptr; // the fiber or context whose host thread this is

// Gives `to` the baton, to run as `owner`, from the context `from` (0: the PPU thread). Under lib.m.
void give(Lib& l, Runner& to, lv2::PpuThread* owner, u32 from)
{
    to.owner = owner;
    to.from = from;
    to.go = true;
    l.cv.notify_all();
}

// Waits for the baton; false when killed instead. Under lib.m (released while waiting).
bool park(Lib& l, std::unique_lock<std::mutex>& lock, Runner& self)
{
    l.cv.wait(lock, [&] { return self.go || self.kill; });
    if (self.kill) return false;
    self.go = false;
    return true;
}

// The calling host thread (a fiber's or context's, just given the baton) runs as the PPU thread that gave it: its lv2
// thread, its ctx, its r13. At the first run, the registers too: the thread's, with the entry's r1 (the stack's top
// less the ABI's 0x70), r2, r3 (the argument), r4 (the context that switched to it), and LR at the thread return
// address, so the entry's return ends run_guest. Under lib.m.
void adopt(Coroutine& co)
{
    lv2::PpuThread* t = co.owner;
    if (!co.primed) {
        co.ctx = *t->ctx;
        u8* base = co.lib->base;
        const u32 top = (co.stack + co.stack_size) & ~(kStackAlign - 1);
        co.ctx.gpr[1] = top - lv2::kStackStartOffset;
        ps_write64(base, top - lv2::kStackStartOffset, 0); // the back chain
        co.ctx.gpr[2] = ps_read32(base, co.entry + 4);
        co.ctx.gpr[3] = co.arg;
        co.ctx.gpr[4] = co.from;
        co.ctx.lr = lv2::kThreadReturnAddress;
        co.ctx.cia = ps_read32(base, co.entry);
        co.primed = true;
    }
    co.ctx.gpr[13] = t->ctx->gpr[13];
    co.ctx.runtime = t;
    t->ctx = &co.ctx;
    co.holding = true;
    lv2::set_current_thread(t);
}

void finish(Lib& l, Coroutine& co, s32 status, bool thread_exit);

// A fiber's or context's host thread.
void* coroutine_main(void* raw)
{
    Coroutine& co = *static_cast<Coroutine*>(raw);
    Lib& l = *co.lib;
    t_current = &co;
    bool killed = false, thread_exit = false;
    s32 status = 0;
    {
        std::unique_lock lock(l.m);
        if (park(l, lock, co)) adopt(co);
        else killed = true;
    }
    if (!killed) {
        lv2::Kernel& k = co.owner->kernel;
        try {
            lv2::run_guest(k, co.ctx, k.base(), ps_read32(k.base(), co.entry), true);
            status = static_cast<s32>(static_cast<u32>(co.ctx.gpr[3]));
            if (co.kind == Kind::Fiber && co.on_exit)
                call_guest(co.ctx, co.on_exit, {co.on_exit_arg, static_cast<u64>(static_cast<s64>(status))});
        } catch (const lv2::ThreadExit&) {
            std::lock_guard lock(l.m);
            if (co.kill) killed = true;
            else if (co.exiting) status = co.exit_status;
            else thread_exit = true;
        }
    }
    {
        std::lock_guard lock(l.m);
        if (!killed) finish(l, co, status, thread_exit);
        co.holding = false;
        co.host_done = true;
        l.hosts--;
        l.cv.notify_all();
    }
    lv2::set_current_thread(nullptr);
    t_current = nullptr;
    return nullptr;
}

// Makes the host thread of `co` (it waits for the baton). Under lib.m.
bool start_host(Lib& l, Coroutine& co, lv2::Kernel& k)
{
    if (co.started) return true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, k.config().host_stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t handle;
    const int rc = pthread_create(&handle, &attr, coroutine_main, &co);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        PS_LOG_ERROR(kChan, "no host thread for the fiber or context {:#x}: {}", co.addr, rc);
        return false;
    }
    co.started = true;
    l.hosts++;
    return true;
}

// ------------------------------------------------------------------------------------------------- scheduling

void make_ready(Coroutine& f)
{
    f.state = State::Ready;
    f.sched->ready[f.priority].push_back(&f);
}

Coroutine* pick(Scheduler& s)
{
    for (auto& q : s.ready) {
        while (!q.empty()) {
            Coroutine* f = q.front();
            q.pop_front();
            if (f->state == State::Ready) return f;
        }
    }
    return nullptr;
}

bool has_ready(const Scheduler& s)
{
    for (const auto& q : s.ready)
        for (const Coroutine* f : q)
            if (f->state == State::Ready) return true;
    return false;
}

// The fibers waiting on a flag whose flag has the value they wait for become ready. Under lib.m. Other PPU threads
// or SPUs set the flags without the lock, so the words are read atomically (D-571).
u32 check_flags(Lib& l, Scheduler& s)
{
    u32 woken = 0;
    for (Coroutine* f : s.fibers) {
        if (f->state != State::WaitFlag) continue;
        if ((atomic_load_be32(l.base, f->flag_addr) != 0) == f->flag_value) {
            make_ready(*f);
            woken++;
        }
    }
    return woken;
}

// A fiber's or context's end: its guest code returned (or the fiber called Exit), or its PPU thread exited inside it.
// The baton goes back to the thread side that ran it. Under lib.m.
void finish(Lib& l, Coroutine& co, s32 status, bool thread_exit)
{
    co.ended = true;
    ThreadSide* side = co.side;
    if (co.kind == Kind::Fiber) {
        co.state = State::Exited;
        co.exit_status = status;
        for (Coroutine* f : co.sched->fibers)
            if (f->state == State::WaitJoin && f->join_target == &co) make_ready(*f);
    } else if (side) {
        side->cause = 0;
        side->returned = co.addr;
    }
    if (side) {
        side->thread_exit = thread_exit;
        give(l, *side, co.owner, co.addr);
    }
    l.cv.notify_all(); // outside joiners and idle workers
}

// The calling fiber gives the baton back to its thread's scheduler loop and waits to run again. Under lib.m.
void suspend_fiber(Lib& l, std::unique_lock<std::mutex>& lock, Coroutine& f)
{
    f.holding = false;
    give(l, *f.side, f.owner, f.addr);
    if (!park(l, lock, f)) throw lv2::ThreadExit{}; // finalized or the process's end: the host thread unwinds
    adopt(f);
}

// Runs `co` on the calling PPU thread (from its own host thread) until the baton comes back. Under lib.m.
s32 run_on_side(Lib& l, std::unique_lock<std::mutex>& lock, PPUContext& ctx, ThreadSide& side, Coroutine& co)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!start_host(l, co, t.kernel)) return error(CELL_FIBER_ERROR_AGAIN);
    co.side = &side;
    give(l, co, &t, 0);
    park(l, lock, side); // the side is never killed
    t.ctx = &ctx;
    lv2::set_current_thread(&t);
    if (side.thread_exit) throw lv2::ThreadExit{};
    return lv2::CELL_OK;
}

// Releases an exited fiber nobody waits for any more. Under lib.m.
void release(Lib& l, Coroutine& f)
{
    if (f.state != State::Exited || f.joiners || !f.host_done) return;
    auto& v = f.sched->fibers;
    v.erase(std::remove(v.begin(), v.end(), &f), v.end());
    for (auto& q : f.sched->ready) q.erase(std::remove(q.begin(), q.end(), &f), q.end());
    l.coroutines.erase(f.addr);
}

Coroutine* current_fiber()
{
    Coroutine* co = t_current;
    return co && co->kind == Kind::Fiber && co->holding ? co : nullptr;
}

Coroutine* current_context()
{
    Coroutine* co = t_current;
    return co && co->kind == Kind::Context && co->holding ? co : nullptr;
}

Scheduler* find_scheduler(Lib& l, u32 addr, bool control)
{
    const auto it = l.schedulers.find(addr);
    return it != l.schedulers.end() && it->second->control == control ? it->second.get() : nullptr;
}

Coroutine* find(Lib& l, u32 addr, Kind kind)
{
    const auto it = l.coroutines.find(addr);
    return it != l.coroutines.end() && it->second->kind == kind ? it->second.get() : nullptr;
}

// ------------------------------------------------------------------------------------------------- the library

s32 _cellFiberPpuInitialize(PPUContext& ctx)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    l.initialized = true;
    l.base = guest_base(ctx);
    return lv2::CELL_OK;
}

// CellFiberPpuSchedulerAttribute {privateHeader[16], autoCheckFlags (b8, +16), debuggerSupport, padding[2],
// autoCheckFlagsIntervalUsec (+20)}.
s32 _cellFiberPpuSchedulerAttributeInitialize(PPUContext& ctx, u32 attr, u32)
{
    if (!attr) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), attr, kSchedulerAttrSize)) return error(CELL_FIBER_ERROR_INVAL);
    std::memset(guest_base(ctx) + attr, 0, kSchedulerAttrSize);
    return lv2::CELL_OK;
}

// A scheduler's (or worker control's) set-up. Under lib.m.
s32 init_scheduler(PPUContext& ctx, Lib& l, u32 addr, u32 attr, bool control)
{
    u8* base = guest_base(ctx);
    if (!l.initialized) return error(CELL_FIBER_ERROR_NOSYSINIT);
    if (const auto it = l.schedulers.find(addr); it != l.schedulers.end()) {
        if (!it->second->fibers.empty() || it->second->runners) return error(CELL_FIBER_ERROR_BUSY);
        l.schedulers.erase(it);
    }
    auto s = std::make_unique<Scheduler>();
    s->addr = addr;
    s->control = control;
    if (attr) {
        s->auto_check = ps_read8(base, attr + 16) != 0;
        s->auto_interval_us = ps_read32(base, attr + 20);
    }
    l.schedulers[addr] = std::move(s);
    return lv2::CELL_OK;
}

s32 cellFiberPpuInitializeScheduler(PPUContext& ctx, u32 scheduler, u32 attr)
{
    if (!scheduler) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign) || !aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, scheduler, kSchedulerSize) || (attr && !guest_readable(t, attr, kSchedulerAttrSize)))
        return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    return init_scheduler(ctx, l, scheduler, attr, false);
}

// A scheduler's (or worker control's) end: no thread running its fibers, every fiber exited. Under lib.m.
s32 finalize_scheduler(Lib& l, Scheduler& s)
{
    if (s.runners) return error(CELL_FIBER_ERROR_BUSY);
    for (const Coroutine* f : s.fibers)
        if (f->state != State::Exited || !f->host_done) return error(CELL_FIBER_ERROR_BUSY);
    for (Coroutine* f : s.fibers) l.coroutines.erase(f->addr);
    l.schedulers.erase(s.addr);
    return lv2::CELL_OK;
}

s32 cellFiberPpuFinalizeScheduler(PPUContext&, u32 scheduler)
{
    if (!scheduler) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    Scheduler* s = find_scheduler(l, scheduler, false);
    if (!s) return error(CELL_FIBER_ERROR_STAT);
    return finalize_scheduler(l, *s);
}

// The scheduler loop of RunFibers and a worker's RunFibers: the first ready fiber of the most urgent priority runs
// until it yields, waits or ends. A plain scheduler's loop ends when no fiber is ready (with autoCheckFlags, it first
// checks the flags of the fibers waiting on one every interval while there are such fibers); a worker's waits for
// work until the control is shut down. Under lib.m.
s32 run_fibers(PPUContext& ctx, Lib& l, std::unique_lock<std::mutex>& lock, Scheduler& s)
{
    lv2::PpuThread& t = thread_of(ctx);
    ThreadSide side;
    s.runners++;
    struct Done {
        Scheduler& s;
        ~Done() { s.runners--; }
    } done{s};
    for (;;) {
        Coroutine* f = pick(s);
        if (f) {
            f->state = State::Running;
            const s32 r = run_on_side(l, lock, ctx, side, *f);
            if (r != lv2::CELL_OK) {
                make_ready(*f);
                return r;
            }
            continue;
        }
        const bool flag_waiters = std::any_of(s.fibers.begin(), s.fibers.end(),
            [](const Coroutine* c) { return c->state == State::WaitFlag; });
        if (!s.control) {
            if (!s.auto_check || !flag_waiters) return lv2::CELL_OK;
        } else if (s.shutdown) {
            return lv2::CELL_OK;
        }
        // Wait for work: the auto-check interval, or a worker's polling timeout (1 ms when none), checking the
        // flags with autoCheckFlags; a worker is woken early by new work.
        const u32 us = s.auto_check && s.auto_interval_us ? s.auto_interval_us
                       : s.control && s.polling_timeout > 0 ? static_cast<u32>(s.polling_timeout)
                                                             : 1000;
        s.idle++;
        l.cv.wait_for(lock, std::chrono::microseconds(std::max(us, 50u)));
        s.idle--;
        if (lv2::wait_aborted(t.waiter)) throw lv2::ThreadExit{};
        if (s.auto_check) check_flags(l, s);
    }
}

s32 cellFiberPpuRunFibers(PPUContext& ctx, u32 scheduler)
{
    if (!scheduler) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    if (t_current) return error(CELL_FIBER_ERROR_PERM); // from a fiber or context
    Scheduler* s = find_scheduler(l, scheduler, false);
    if (!s) return error(CELL_FIBER_ERROR_STAT);
    if (s->runners) return error(CELL_FIBER_ERROR_BUSY);
    return run_fibers(ctx, l, lock, *s);
}

s32 cellFiberPpuCheckFlags(PPUContext&, u32 scheduler)
{
    if (!scheduler) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    Scheduler* s = find_scheduler(l, scheduler, false);
    if (!s) return error(CELL_FIBER_ERROR_STAT);
    check_flags(l, *s);
    return lv2::CELL_OK;
}

s32 cellFiberPpuHasRunnableFiber(PPUContext& ctx, u32 scheduler, u32 flag)
{
    if (!scheduler || !flag) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    Scheduler* s = find_scheduler(l, scheduler, false);
    if (!s) return error(CELL_FIBER_ERROR_STAT);
    if (!guest_readable(thread_of(ctx), flag, 1)) return error(CELL_FIBER_ERROR_INVAL);
    ps_write8(guest_base(ctx), flag, has_ready(*s) ? 1 : 0);
    return lv2::CELL_OK;
}

// CellFiberPpuAttribute {privateHeader[16], name[32] (+16), onExitCallback (+48), reserved, onExitCallbackArg (u64,
// +56), reserved}.
s32 _cellFiberPpuAttributeInitialize(PPUContext& ctx, u32 attr, u32)
{
    if (!attr) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), attr, kFiberAttrSize)) return error(CELL_FIBER_ERROR_INVAL);
    std::memset(guest_base(ctx) + attr, 0, kFiberAttrSize);
    return lv2::CELL_OK;
}

// The checks of a fiber's or context's stack: there, 16-byte aligned, at least 0x200 bytes, in guest memory.
s32 check_stack(PPUContext& ctx, u32 stack, u32 size)
{
    if (!aligned(stack, kStackAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (size < kMinStack || u64{stack} + size > 0x100000000ull) return error(CELL_FIBER_ERROR_INVAL);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, stack, 1) || !guest_readable(t, stack + size - 1, 1)) return error(CELL_FIBER_ERROR_INVAL);
    return lv2::CELL_OK;
}

// A fiber in a scheduler: ready at the end of its priority's queue. Under lib.m.
s32 create_fiber(PPUContext& ctx, Lib& l, Scheduler& s, u32 fiber, u32 entry, u64 arg, u32 priority, u32 stack,
    u32 size, u32 attr)
{
    u8* base = guest_base(ctx);
    if (const auto it = l.coroutines.find(fiber); it != l.coroutines.end()) {
        Coroutine& old = *it->second;
        if (old.kind != Kind::Fiber || old.state != State::Exited || old.joiners || !old.host_done)
            return error(CELL_FIBER_ERROR_BUSY);
        auto& v = old.sched->fibers;
        v.erase(std::remove(v.begin(), v.end(), &old), v.end());
        for (auto& q : old.sched->ready) q.erase(std::remove(q.begin(), q.end(), &old), q.end());
        l.coroutines.erase(it);
    }
    auto f = std::make_unique<Coroutine>();
    f->lib = &l;
    f->kind = Kind::Fiber;
    f->addr = fiber;
    f->entry = entry;
    f->arg = arg;
    f->stack = stack;
    f->stack_size = size;
    f->sched = &s;
    f->priority = priority;
    if (attr) {
        f->name = read_cstr(thread_of(ctx), attr + 16, 32).text;
        f->on_exit = ps_read32(base, attr + 48);
        f->on_exit_arg = ps_read64(base, attr + 56);
    }
    Coroutine& ref = *f;
    l.coroutines[fiber] = std::move(f);
    s.fibers.push_back(&ref);
    make_ready(ref);
    l.cv.notify_all(); // an idle worker
    return lv2::CELL_OK;
}

// The checks CreateFiber and a worker control's CreateFiber share.
s32 check_fiber_args(PPUContext& ctx, u32 fiber, u32 entry, u32 priority, u32 stack, u32 size, u32 attr)
{
    if (!fiber || !entry || !stack) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(fiber, kFiberAlign) || !aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (priority >= kPriorities) return error(CELL_FIBER_ERROR_INVAL);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, fiber, kFiberSize) || !guest_readable(t, entry, 8) ||
        (attr && !guest_readable(t, attr, kFiberAttrSize)))
        return error(CELL_FIBER_ERROR_INVAL);
    return check_stack(ctx, stack, size);
}

s32 cellFiberPpuCreateFiber(PPUContext& ctx, u32 scheduler, u32 fiber, u32 entry, u64 arg, u32 priority, u32 stack,
    u32 size, u32 attr)
{
    if (!scheduler) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (const s32 r = check_fiber_args(ctx, fiber, entry, priority, stack, size, attr); r != lv2::CELL_OK) return r;
    Lib& l = lib();
    std::lock_guard lock(l.m);
    Scheduler* s = find_scheduler(l, scheduler, false);
    if (!s) return error(CELL_FIBER_ERROR_STAT);
    return create_fiber(ctx, l, *s, fiber, entry, arg, priority, stack, size, attr);
}

// cellFiberPpuExit(status): the calling fiber ends: its exit callback, then its host thread unwinds.
s32 cellFiberPpuExit(PPUContext& ctx, s32 status)
{
    Lib& l = lib();
    Coroutine* f = nullptr;
    {
        std::lock_guard lock(l.m);
        f = current_fiber();
        if (!f) return error(CELL_FIBER_ERROR_PERM);
    }
    if (f->on_exit) call_guest(ctx, f->on_exit, {f->on_exit_arg, static_cast<u64>(static_cast<s64>(status))});
    {
        std::lock_guard lock(l.m);
        f->exiting = true;
        f->exit_status = status;
    }
    throw lv2::ThreadExit{};
}

s32 cellFiberPpuYield(PPUContext&)
{
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* f = current_fiber();
    if (!f) return error(CELL_FIBER_ERROR_PERM);
    make_ready(*f);
    suspend_fiber(l, lock, *f);
    return lv2::CELL_OK;
}

// cellFiberPpuJoinFiber(fiber, int *status): waits for the fiber's end, from a fiber; from a thread, only a fiber
// already ended. The last joiner releases it.
s32 join(PPUContext& ctx, Lib& l, std::unique_lock<std::mutex>& lock, Coroutine& target, u32 status)
{
    Coroutine* self = current_fiber();
    if (self == &target) return error(CELL_FIBER_ERROR_DEADLK);
    target.joiners++;
    if (target.state != State::Exited) {
        if (!self) {
            target.joiners--;
            return error(CELL_FIBER_ERROR_PERM);
        }
        self->state = State::WaitJoin;
        self->join_target = &target;
        suspend_fiber(l, lock, *self);
        self->join_target = nullptr;
    }
    l.cv.wait(lock, [&] { return target.host_done; });
    ps_write32(guest_base(ctx), status, static_cast<u32>(target.exit_status));
    target.joiners--;
    release(l, target);
    return lv2::CELL_OK;
}

s32 cellFiberPpuJoinFiber(PPUContext& ctx, u32 fiber, u32 status)
{
    if (!fiber || !status) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(fiber, kFiberAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), status, 4)) return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* target = find(l, fiber, Kind::Fiber);
    if (!target) return error(CELL_FIBER_ERROR_STAT);
    return join(ctx, l, lock, *target, status);
}

// cellFiberPpuSelf(): the calling fiber, 0 outside one.
u32 cellFiberPpuSelf(PPUContext&)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    const Coroutine* f = current_fiber();
    return f ? f->addr : 0;
}

// A signal for the fiber: it runs again if it waits for one, else it is kept for its next wait. *numWorker (when
// given): the idle workers there are to take it up (0 or 1; 0 for a plain scheduler). Under lib.m.
s32 send_signal(PPUContext& ctx, Lib& l, u32 fiber, u32 num_worker)
{
    Coroutine* f = find(l, fiber, Kind::Fiber);
    if (!f || f->state == State::Exited) return error(CELL_FIBER_ERROR_STAT);
    u32 woken = 0;
    if (f->state == State::WaitSignal) {
        make_ready(*f);
        woken = f->sched->control && f->sched->idle ? 1 : 0;
        l.cv.notify_all();
    } else {
        f->signal = true;
    }
    if (num_worker && guest_readable(thread_of(ctx), num_worker, 4)) ps_write32(guest_base(ctx), num_worker, woken);
    return lv2::CELL_OK;
}

s32 cellFiberPpuSendSignal(PPUContext& ctx, u32 fiber, u32 num_worker)
{
    if (!fiber) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(fiber, kFiberAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    return send_signal(ctx, l, fiber, num_worker);
}

s32 cellFiberPpuWaitSignal(PPUContext&)
{
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* f = current_fiber();
    if (!f) return error(CELL_FIBER_ERROR_PERM);
    if (f->signal) {
        f->signal = false;
        return lv2::CELL_OK;
    }
    f->state = State::WaitSignal;
    suspend_fiber(l, lock, *f);
    return lv2::CELL_OK;
}

// cellFiberPpuWaitFlag(uint32_t *eaFlag, bool flagValue): waits until the word is non-zero (true) or zero (false), as
// CheckFlags (or autoCheckFlags) finds it.
s32 cellFiberPpuWaitFlag(PPUContext& ctx, u32 flag, u32 value)
{
    if (!flag) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(flag, 4)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), flag, 4)) return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* f = current_fiber();
    if (!f) return error(CELL_FIBER_ERROR_PERM);
    const bool want = (value & 0xFF) != 0;
    if ((atomic_load_be32(guest_base(ctx), flag) != 0) == want) return lv2::CELL_OK;
    f->state = State::WaitFlag;
    f->flag_addr = flag;
    f->flag_value = want;
    suspend_fiber(l, lock, *f);
    return lv2::CELL_OK;
}

s32 cellFiberPpuGetScheduler(PPUContext& ctx, u32 fiber, u32 scheduler)
{
    if (!fiber || !scheduler) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(fiber, kFiberAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), scheduler, 4)) return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    const Coroutine* f = find(l, fiber, Kind::Fiber);
    if (!f) return error(CELL_FIBER_ERROR_STAT);
    ps_write32(guest_base(ctx), scheduler, f->sched->addr);
    return lv2::CELL_OK;
}

// The calling fiber's priority, used when it is next queued.
s32 cellFiberPpuSetPriority(PPUContext&, u32 priority)
{
    if (priority >= kPriorities) return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    Coroutine* f = current_fiber();
    if (!f) return error(CELL_FIBER_ERROR_PERM);
    f->priority = priority;
    return lv2::CELL_OK;
}

// r1 still in the stack with the ABI's red zone below it (288 bytes), else STAT.
s32 check_stack_limit(PPUContext& ctx, const Coroutine& co)
{
    const u64 sp = ctx.gpr[1];
    return sp >= u64{co.stack} + kRedZone && sp <= u64{co.stack} + co.stack_size ? lv2::CELL_OK
                                                                                 : error(CELL_FIBER_ERROR_STAT);
}

s32 cellFiberPpuCheckStackLimit(PPUContext& ctx)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    const Coroutine* f = current_fiber();
    if (!f) return error(CELL_FIBER_ERROR_PERM);
    return check_stack_limit(ctx, *f);
}

// ------------------------------------------------------------------------------------------------- contexts

// CellFiberPpuContextAttribute {privateHeader[16], name[32], debuggerSupport (b8, +48), ...}.
s32 _cellFiberPpuContextAttributeInitialize(PPUContext& ctx, u32 attr, u32)
{
    if (!attr) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), attr, kContextAttrSize)) return error(CELL_FIBER_ERROR_INVAL);
    std::memset(guest_base(ctx) + attr, 0, kContextAttrSize);
    return lv2::CELL_OK;
}

// cellFiberPpuContextInitialize(context, entry (arg, fiberFrom), arg, eaStack, sizeStack, attr).
s32 cellFiberPpuContextInitialize(PPUContext& ctx, u32 context, u32 entry, u64 arg, u32 stack, u32 size, u32 attr)
{
    if (!context || !entry || !stack) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(context, kContextAlign) || !aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, context, kContextSize) || !guest_readable(t, entry, 8) ||
        (attr && !guest_readable(t, attr, kContextAttrSize)))
        return error(CELL_FIBER_ERROR_INVAL);
    if (const s32 r = check_stack(ctx, stack, size); r != lv2::CELL_OK) return r;
    Lib& l = lib();
    std::lock_guard lock(l.m);
    if (!l.initialized) return error(CELL_FIBER_ERROR_NOSYSINIT);
    if (const auto it = l.coroutines.find(context); it != l.coroutines.end()) {
        if (it->second->kind != Kind::Context || (it->second->started && !it->second->host_done))
            return error(CELL_FIBER_ERROR_BUSY);
        l.coroutines.erase(it);
    }
    auto c = std::make_unique<Coroutine>();
    c->lib = &l;
    c->kind = Kind::Context;
    c->addr = context;
    c->entry = entry;
    c->arg = arg;
    c->stack = stack;
    c->stack_size = size;
    if (attr) c->name = read_cstr(t, attr + 16, 32).text;
    l.coroutines[context] = std::move(c);
    return lv2::CELL_OK;
}

// A context's end: not the one running; its host thread, parked, unwinds first.
s32 cellFiberPpuContextFinalize(PPUContext&, u32 context)
{
    if (!context) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(context, kContextAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* c = find(l, context, Kind::Context);
    if (!c) return error(CELL_FIBER_ERROR_STAT);
    if (c->holding || c->go) return error(CELL_FIBER_ERROR_BUSY);
    if (c->started && !c->host_done) {
        c->kill = true;
        l.cv.notify_all();
        l.cv.wait(lock, [&] { return c->host_done; });
    }
    l.coroutines.erase(context);
    return lv2::CELL_OK;
}

// A context that can be run or switched to: not ended, not running.
s32 runnable(const Coroutine* c)
{
    if (!c || c->ended) return error(CELL_FIBER_ERROR_STAT);
    if (c->holding || c->go) return error(CELL_FIBER_ERROR_BUSY);
    return lv2::CELL_OK;
}

// From the thread: runs `context` until a context returns to the thread (or a context's entry returns: cause 0).
s32 run_context(PPUContext& ctx, Lib& l, std::unique_lock<std::mutex>& lock, u32 context, u32 cause, u32 from)
{
    Coroutine* c = find(l, context, Kind::Context);
    if (const s32 r = runnable(c); r != lv2::CELL_OK) return r;
    ThreadSide side;
    if (const s32 r = run_on_side(l, lock, ctx, side, *c); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    if (cause && guest_readable(t, cause, 4)) ps_write32(base, cause, static_cast<u32>(side.cause));
    if (from && guest_readable(t, from, 4)) ps_write32(base, from, side.returned);
    return lv2::CELL_OK;
}

// cellFiberPpuContextRun(context, int *cause, CellFiberPpuContext **fiberFrom, option).
s32 cellFiberPpuContextRun(PPUContext& ctx, u32 context, u32 cause, u32 from, u32)
{
    if (!context) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(context, kContextAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    if (t_current) return error(CELL_FIBER_ERROR_PERM);
    return run_context(ctx, l, lock, context, cause, from);
}

// From a context: `next` runs; this one goes on when something switches back to it (*fiberFrom: the context that did,
// 0 for the thread).
s32 switch_context(PPUContext& ctx, Lib& l, std::unique_lock<std::mutex>& lock, Coroutine& self, u32 next, u32 from)
{
    Coroutine* n = find(l, next, Kind::Context);
    if (const s32 r = runnable(n); r != lv2::CELL_OK) return r;
    lv2::PpuThread* t = self.owner;
    if (!start_host(l, *n, t->kernel)) return error(CELL_FIBER_ERROR_AGAIN);
    n->side = self.side;
    self.holding = false;
    give(l, *n, t, self.addr);
    if (!park(l, lock, self)) throw lv2::ThreadExit{};
    adopt(self);
    if (from && guest_readable(thread_of(ctx), from, 4)) ps_write32(guest_base(ctx), from, self.from);
    return lv2::CELL_OK;
}

s32 cellFiberPpuContextSwitch(PPUContext& ctx, u32 context, u32 from, u32)
{
    if (!context) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(context, kContextAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* self = current_context();
    if (!self) return error(CELL_FIBER_ERROR_PERM);
    return switch_context(ctx, l, lock, *self, context, from);
}

// cellFiberPpuContextSelf(): the calling context (a fiber: itself), 0 outside one.
u32 cellFiberPpuContextSelf(PPUContext&)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    const Coroutine* c = t_current;
    return c && c->holding ? c->addr : 0;
}

// From a context: the thread's Run (or RunScheduler) returns with `cause`; this context goes on when run again.
s32 return_to_thread(Lib& l, std::unique_lock<std::mutex>& lock, Coroutine& self, s32 cause)
{
    ThreadSide* side = self.side;
    side->cause = cause;
    side->returned = self.addr;
    side->thread_exit = false;
    self.holding = false;
    give(l, *side, self.owner, self.addr);
    if (!park(l, lock, self)) throw lv2::ThreadExit{};
    adopt(self);
    return lv2::CELL_OK;
}

s32 cellFiberPpuContextReturnToThread(PPUContext&, s32 cause)
{
    Lib& l = lib();
    std::unique_lock lock(l.m);
    Coroutine* self = current_context();
    if (!self) return error(CELL_FIBER_ERROR_PERM);
    return return_to_thread(l, lock, *self, cause);
}

s32 cellFiberPpuContextCheckStackLimit(PPUContext& ctx)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    const Coroutine* c = current_context();
    if (!c) return error(CELL_FIBER_ERROR_PERM);
    return check_stack_limit(ctx, *c);
}

// cellFiberPpuContextRunScheduler(scheduler (arg0, arg1) -> context, arg0, arg1, cause *, fiberFrom **, option): from
// the thread, the callback picks the context to run (none: Run returns at once, cause 0, fiberFrom 0), which runs as
// with ContextRun.
s32 cellFiberPpuContextRunScheduler(PPUContext& ctx, u32 callback, u64 arg0, u64 arg1, u32 cause, u32 from, u32)
{
    if (!callback) return error(CELL_FIBER_ERROR_NULL_POINTER);
    Lib& l = lib();
    {
        std::lock_guard lock(l.m);
        if (t_current) return error(CELL_FIBER_ERROR_PERM);
    }
    const u32 next = static_cast<u32>(call_guest(ctx, callback, {arg0, arg1}));
    if (!next) {
        u8* base = guest_base(ctx);
        lv2::PpuThread& t = thread_of(ctx);
        if (cause && guest_readable(t, cause, 4)) ps_write32(base, cause, 0);
        if (from && guest_readable(t, from, 4)) ps_write32(base, from, 0);
        return lv2::CELL_OK;
    }
    std::unique_lock lock(l.m);
    return run_context(ctx, l, lock, next, cause, from);
}

// cellFiberPpuContextEnterScheduler(scheduler, arg0, arg1, fiberFrom **, option): from a context, the callback picks
// the next context, which this one switches to (itself: it goes on; none: back to the thread, cause 0).
s32 cellFiberPpuContextEnterScheduler(PPUContext& ctx, u32 callback, u64 arg0, u64 arg1, u32 from, u32)
{
    if (!callback) return error(CELL_FIBER_ERROR_NULL_POINTER);
    Lib& l = lib();
    Coroutine* self = nullptr;
    {
        std::lock_guard lock(l.m);
        self = current_context();
        if (!self) return error(CELL_FIBER_ERROR_PERM);
    }
    const u32 next = static_cast<u32>(call_guest(ctx, callback, {arg0, arg1}));
    std::unique_lock lock(l.m);
    if (next == self->addr) {
        if (from && guest_readable(thread_of(ctx), from, 4)) ps_write32(guest_base(ctx), from, self->addr);
        return lv2::CELL_OK;
    }
    if (!next) return return_to_thread(l, lock, *self, 0);
    return switch_context(ctx, l, lock, *self, next, from);
}

// ------------------------------------------------------------------------------------------------- traces

// The scheduler's trace: the state machine (initialize, start, stop, finalize) with its checks; the records, which
// are the debugger's format, are not written (logged once).
s32 trace(PPUContext& ctx, u32 scheduler, u32 want, u32 next, u32 buffer = 1, u32 size = 1)
{
    if (!scheduler || !buffer) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(scheduler, kSchedulerAlign) || (want == 0 && !aligned(buffer, 16)))
        return error(CELL_FIBER_ERROR_ALIGN);
    if (!size) return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    Scheduler* s = find_scheduler(l, scheduler, false);
    if (!s) return error(CELL_FIBER_ERROR_STAT);
    if (want == 0 && !guest_readable(thread_of(ctx), buffer, size)) return error(CELL_FIBER_ERROR_INVAL);
    if (want == 3) {
        if (s->trace == 0) return error(CELL_FIBER_ERROR_STAT);
    } else if (s->trace != want) {
        return error(CELL_FIBER_ERROR_STAT);
    }
    s->trace = next;
    if (want == 0 && !l.trace_logged) {
        l.trace_logged = true;
        PS_LOG_INFO(kChan, "cellFiberPpuSchedulerTraceInitialize: trace records are not written");
    }
    return lv2::CELL_OK;
}

s32 cellFiberPpuSchedulerTraceInitialize(PPUContext& ctx, u32 scheduler, u32 buffer, u32 size, u32)
{
    return trace(ctx, scheduler, 0, 1, buffer, size);
}
s32 cellFiberPpuSchedulerTraceStart(PPUContext& ctx, u32 scheduler) { return trace(ctx, scheduler, 1, 2); }
s32 cellFiberPpuSchedulerTraceStop(PPUContext& ctx, u32 scheduler) { return trace(ctx, scheduler, 2, 1); }
s32 cellFiberPpuSchedulerTraceFinalize(PPUContext& ctx, u32 scheduler) { return trace(ctx, scheduler, 3, 0); }

// ------------------------------------------------------------------------------------------------- worker control

// CellFiberPpuUtilWorkerControlAttribute {scheduler attribute (256 bytes), privateHeader[2] (u64), reserved}.
s32 _cellFiberPpuUtilWorkerControlAttributeInitialize(PPUContext& ctx, u32 attr, u32)
{
    if (!attr) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    if (!guest_readable(thread_of(ctx), attr, kControlAttrSize)) return error(CELL_FIBER_ERROR_INVAL);
    std::memset(guest_base(ctx) + attr, 0, kControlAttrSize);
    return lv2::CELL_OK;
}

s32 init_control(PPUContext& ctx, u32 control, u32 attr)
{
    if (!control) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(control, kControlAlign) || !aligned(attr, kAttrAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, control, kControlSize) || (attr && !guest_readable(t, attr, kControlAttrSize)))
        return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::lock_guard lock(l.m);
    return init_scheduler(ctx, l, control, attr, true);
}

s32 cellFiberPpuUtilWorkerControlInitialize(PPUContext& ctx, u32 control) { return init_control(ctx, control, 0); }

s32 cellFiberPpuUtilWorkerControlInitializeWithAttribute(PPUContext& ctx, u32 control, u32 attr)
{
    if (!attr) return error(CELL_FIBER_ERROR_NULL_POINTER);
    return init_control(ctx, control, attr);
}

// The worker control's scheduler, with the shared checks; nullptr and *r set when there is none.
Scheduler* control_of(Lib& l, u32 control, s32& r)
{
    r = lv2::CELL_OK;
    if (!control) r = error(CELL_FIBER_ERROR_NULL_POINTER);
    else if (!aligned(control, kControlAlign)) r = error(CELL_FIBER_ERROR_ALIGN);
    Scheduler* s = r == lv2::CELL_OK ? find_scheduler(l, control, true) : nullptr;
    if (r == lv2::CELL_OK && !s) r = error(CELL_FIBER_ERROR_STAT);
    return s;
}

// A worker thread runs the control's fibers until the control is shut down.
s32 cellFiberPpuUtilWorkerControlRunFibers(PPUContext& ctx, u32 control)
{
    Lib& l = lib();
    std::unique_lock lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    if (t_current) return error(CELL_FIBER_ERROR_PERM);
    return run_fibers(ctx, l, lock, *s);
}

// SetPollingMode(control, mode, timeout): kept; an idle worker waits `timeout` microseconds (when positive) between
// looks for work, else 1 ms; new work wakes it at once.
s32 cellFiberPpuUtilWorkerControlSetPollingMode(PPUContext&, u32 control, s32 mode, s32 timeout)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    if (mode < 0) return error(CELL_FIBER_ERROR_INVAL);
    s->polling_mode = mode;
    s->polling_timeout = timeout;
    return lv2::CELL_OK;
}

// JoinFiber(control, fiber, int *exitCode): from a fiber, as cellFiberPpuJoinFiber; from another thread, waits (as
// that thread) for the fiber's end.
s32 cellFiberPpuUtilWorkerControlJoinFiber(PPUContext& ctx, u32 control, u32 fiber, u32 exit_code)
{
    if (!fiber || !exit_code) return error(CELL_FIBER_ERROR_NULL_POINTER);
    if (!aligned(fiber, kFiberAlign)) return error(CELL_FIBER_ERROR_ALIGN);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, exit_code, 4)) return error(CELL_FIBER_ERROR_INVAL);
    Lib& l = lib();
    std::unique_lock lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    Coroutine* target = find(l, fiber, Kind::Fiber);
    if (!target || target->sched != s) return error(CELL_FIBER_ERROR_STAT);
    if (current_fiber()) return join(ctx, l, lock, *target, exit_code);
    target->joiners++;
    while (!(target->state == State::Exited && target->host_done)) {
        l.cv.wait_for(lock, std::chrono::milliseconds(1));
        if (lv2::wait_aborted(t.waiter)) {
            target->joiners--;
            throw lv2::ThreadExit{};
        }
    }
    ps_write32(guest_base(ctx), exit_code, static_cast<u32>(target->exit_status));
    target->joiners--;
    release(l, *target);
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlSendSignal(PPUContext& ctx, u32 fiber, u32 num_worker)
{
    return cellFiberPpuSendSignal(ctx, fiber, num_worker);
}

s32 cellFiberPpuUtilWorkerControlFinalize(PPUContext&, u32 control)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    return finalize_scheduler(l, *s);
}

s32 cellFiberPpuUtilWorkerControlWakeup(PPUContext&, u32 control)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    s32 r = 0;
    if (!control_of(l, control, r)) return r;
    l.cv.notify_all();
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlCreateFiber(PPUContext& ctx, u32 control, u32 fiber, u32 entry, u64 arg,
    u32 priority, u32 stack, u32 size, u32 attr)
{
    if (const s32 r = check_fiber_args(ctx, fiber, entry, priority, stack, size, attr); r != lv2::CELL_OK) return r;
    Lib& l = lib();
    std::lock_guard lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    if (s->shutdown) return error(CELL_FIBER_ERROR_STAT);
    return create_fiber(ctx, l, *s, fiber, entry, arg, priority, stack, size, attr);
}

// The workers' RunFibers return once no fiber is ready.
s32 cellFiberPpuUtilWorkerControlShutdown(PPUContext&, u32 control)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    s->shutdown = true;
    l.cv.notify_all();
    return lv2::CELL_OK;
}

// CheckFlags(control, bool wakingUp): the flags checked; with wakingUp, idle workers woken for the fibers made ready.
s32 cellFiberPpuUtilWorkerControlCheckFlags(PPUContext&, u32 control, u32 waking_up)
{
    Lib& l = lib();
    std::lock_guard lock(l.m);
    s32 r = 0;
    Scheduler* s = control_of(l, control, r);
    if (!s) return r;
    if (check_flags(l, *s) && (waking_up & 0xFF)) l.cv.notify_all();
    return lv2::CELL_OK;
}

// The SPURS event queue link: its arguments are not known (RPCS3 has none), so not made; logged, and refused.
s32 unknown_spurs_link(PPUContext& ctx, const char* name)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}: its arguments are not known; not made", name));
    return error(CELL_FIBER_ERROR_PERM);
}

s32 cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs(PPUContext& ctx)
{
    return unknown_spurs_link(ctx, "cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs");
}

s32 cellFiberPpuUtilWorkerControlDisconnectEventQueue(PPUContext& ctx)
{
    return unknown_spurs_link(ctx, "cellFiberPpuUtilWorkerControlDisconnectEventQueue");
}

} // namespace

void register_cellFiber(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0xC11F8056, _cellFiberPpuAttributeInitialize),
        PS_HLE_FUNC(0x31252EC3, _cellFiberPpuContextAttributeInitialize),
        PS_HLE_FUNC(0x55870804, _cellFiberPpuInitialize),
        PS_HLE_FUNC(0x9E25C72D, _cellFiberPpuSchedulerAttributeInitialize),
        PS_HLE_FUNC(0x68BA4568, _cellFiberPpuUtilWorkerControlAttributeInitialize),
        PS_HLE_FUNC(0xF6C6900C, cellFiberPpuCheckFlags),
        PS_HLE_FUNC(0xF3E81219, cellFiberPpuCheckStackLimit),
        PS_HLE_FUNC(0xB90C871B, cellFiberPpuContextCheckStackLimit),
        PS_HLE_FUNC(0x0A25B6C8, cellFiberPpuContextEnterScheduler),
        PS_HLE_FUNC(0xB3A48079, cellFiberPpuContextFinalize),
        PS_HLE_FUNC(0x72086315, cellFiberPpuContextInitialize),
        PS_HLE_FUNC(0x01036193, cellFiberPpuContextReturnToThread),
        PS_HLE_FUNC(0xABA1C563, cellFiberPpuContextRun),
        PS_HLE_FUNC(0x081C98BE, cellFiberPpuContextRunScheduler),
        PS_HLE_FUNC(0x34A81091, cellFiberPpuContextSelf),
        PS_HLE_FUNC(0xD0066B17, cellFiberPpuContextSwitch),
        PS_HLE_FUNC(0x7C2F4034, cellFiberPpuCreateFiber),
        PS_HLE_FUNC(0xFA8D5F95, cellFiberPpuExit),
        PS_HLE_FUNC(0x8B6BAA01, cellFiberPpuFinalizeScheduler),
        PS_HLE_FUNC(0xB0594B2D, cellFiberPpuGetScheduler),
        PS_HLE_FUNC(0xE492A675, cellFiberPpuHasRunnableFiber),
        PS_HLE_FUNC(0xEE3B604D, cellFiberPpuInitializeScheduler),
        PS_HLE_FUNC(0xA6004249, cellFiberPpuJoinFiber),
        PS_HLE_FUNC(0x12B1ACF0, cellFiberPpuRunFibers),
        PS_HLE_FUNC(0x3860A12A, cellFiberPpuSchedulerTraceFinalize),
        PS_HLE_FUNC(0xBF9CD933, cellFiberPpuSchedulerTraceInitialize),
        PS_HLE_FUNC(0xADEDBEBF, cellFiberPpuSchedulerTraceStart),
        PS_HLE_FUNC(0xE665F9A9, cellFiberPpuSchedulerTraceStop),
        PS_HLE_FUNC(0x5D9A7034, cellFiberPpuSelf),
        PS_HLE_FUNC(0x8AFB8356, cellFiberPpuSendSignal),
        PS_HLE_FUNC(0xFBF5FE40, cellFiberPpuSetPriority),
        PS_HLE_FUNC(0xEA6DC1AD, cellFiberPpuUtilWorkerControlCheckFlags),
        PS_HLE_FUNC(0x62A20F0D, cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs),
        PS_HLE_FUNC(0xBFCA88D3, cellFiberPpuUtilWorkerControlCreateFiber),
        PS_HLE_FUNC(0x4FC86B2C, cellFiberPpuUtilWorkerControlDisconnectEventQueue),
        PS_HLE_FUNC(0xA27C95CA, cellFiberPpuUtilWorkerControlFinalize),
        PS_HLE_FUNC(0x3204B146, cellFiberPpuUtilWorkerControlInitialize),
        PS_HLE_FUNC(0xF2CCAD4F, cellFiberPpuUtilWorkerControlInitializeWithAttribute),
        PS_HLE_FUNC(0x3B417F82, cellFiberPpuUtilWorkerControlJoinFiber),
        PS_HLE_FUNC(0x1E7A247A, cellFiberPpuUtilWorkerControlRunFibers),
        PS_HLE_FUNC(0x5D3992DD, cellFiberPpuUtilWorkerControlSendSignal),
        PS_HLE_FUNC(0x392C5AA5, cellFiberPpuUtilWorkerControlSetPollingMode),
        PS_HLE_FUNC(0xC04E2438, cellFiberPpuUtilWorkerControlShutdown),
        PS_HLE_FUNC(0xBABF714B, cellFiberPpuUtilWorkerControlWakeup),
        PS_HLE_FUNC(0xA4599CF3, cellFiberPpuWaitFlag),
        PS_HLE_FUNC(0x6C164B3B, cellFiberPpuWaitSignal),
        PS_HLE_FUNC(0x0C44F441, cellFiberPpuYield),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
