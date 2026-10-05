// PPU threads: creation, host threads, entry, exit, join, detach, priorities.
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_ppu_thread.cpp, Emu/Cell/PPUThread.cpp (ppu_thread constructor,
// fast_call, exec_task), Emu/Cell/Modules/sys_ppu_thread_.cpp (sysPrxForUser wrappers).
//
// Deviations from lv2 / RPCS3:
// - Every PPU thread is a host thread; there is no PS3 scheduler emulation (sys_ppu_thread_start does not wait for
//   a higher-priority thread to run first; sys_ppu_thread_yield is a host yield and returns CELL_OK).
// - A thread whose entry function returns (instead of calling sys_ppu_thread_exit) exits with r3 as its status
//   and an error is logged; RPCS3 logs the same error and leaves the thread asleep forever.
// - Interrupt threads (SYS_PPU_THREAD_CREATE_INTERRUPT) are created, but nothing establishes them yet
//   (sys_interrupt_thread_establish is not implemented).

#include <pthread.h>

#include <algorithm>

#include "internal.hpp"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace pspocket::lv2 {

namespace {

enum : u64 {
    SYS_PPU_THREAD_CREATE_JOINABLE = 0x1,
    SYS_PPU_THREAD_CREATE_INTERRUPT = 0x2,
};

constexpr usize kMaxThreadName = 27; // RPCS3 c_max_ppu_name_size - 1

bool valid_priority(const Kernel& k, s32 prio) { return prio >= (k.config().root ? -512 : 0) && prio <= 3071; }

struct StartArgs {
    std::shared_ptr<PpuThread> thread;
    std::shared_ptr<HostThreads> host;
};

void* host_thread_main(void* raw)
{
    std::unique_ptr<StartArgs> args(static_cast<StartArgs*>(raw));
    PpuThread& t = *args->thread;
    Kernel& k = t.kernel;
    set_current_thread(&t);
#if defined(__APPLE__)
    {
        std::string name;
        {
            std::lock_guard lock(t.lock);
            name = std::format("PPU {:#x} {}", t.id, t.name);
        }
        pthread_setname_np(name.c_str());
    }
#endif
    try {
        k.config().executor(k.config().executor_user, *t.ctx, k.base(), t.entry);
        PS_LOG_ERROR(kChan, "PPU thread {:#x} returned from its entry function {:#010x} (r3 = {:#x}) instead of calling "
                            "sys_ppu_thread_exit; treating it as an exit",
            t.id, t.entry, t.ctx->gpr[3]);
        thread_finish(t, t.ctx->gpr[3]);
    } catch (const ThreadExit&) {
        thread_finish(t, t.ctx->gpr[3]); // no-op after _sys_ppu_thread_exit; records an aborted thread
    }
    set_current_thread(nullptr);

    std::shared_ptr<HostThreads> host = std::move(args->host);
    args.reset(); // drops this host thread's reference to the PpuThread
    std::lock_guard lock(host->mutex);
    host->live--;
    host->cv.notify_all();
    return nullptr;
}

} // namespace

PpuThread::PpuThread(Kernel& k) : kernel(k), ctx(&own_ctx_) { ps_ppu_context_init(&own_ctx_); }

// ------------------------------------------------------------------------------------------------- kernel API

Result Kernel::create_thread(const ThreadParams& p, u32& id_out)
{
    Impl& s = *impl_;
    if (s.exiting) return err(CELL_EAGAIN);
    if (!valid_priority(*this, p.prio)) return err(CELL_EINVAL);

    // RPCS3 _sys_ppu_thread_create: 4 KB granularity, 0 means 4 KB; the size is taken from the default container.
    const u32 stack_size = p.stack_size ? (p.stack_size + 0xFFFu) & ~0xFFFu : 0x1000;
    if (stack_size == 0) return err(CELL_ENOMEM); // wrapped
    u32 stack_addr = 0, committed = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        if (!s.memory.default_container.take(stack_size)) return err(CELL_ENOMEM);
        stack_addr = s.memory.allocate_stack(stack_size, committed);
        if (!stack_addr) {
            s.memory.default_container.give_back(stack_size);
            return err(CELL_ENOMEM);
        }
    }

    auto t = std::make_shared<PpuThread>(*this);
    t->name = p.name.substr(0, kMaxThreadName);
    t->entry = p.entry;
    t->rtoc = p.rtoc;
    t->tls = p.tls;
    t->arg0 = p.arg0;
    t->arg1 = p.arg1;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    t->stack_commit = committed;
    t->is_main = p.main;
    t->join = p.joinable ? JoinState::Joinable : JoinState::Detached;
    t->waiter.prio = p.prio;

    // Entry state (RPCS3 ppu_thread::ppu_thread and fast_call).
    PPUContext& c = *t->ctx;
    c.gpr[1] = stack_addr + stack_size - kStackStartOffset;
    c.gpr[2] = p.rtoc;
    c.gpr[3] = p.arg0;
    c.gpr[4] = p.arg1;
    c.gpr[13] = p.tls;
    c.lr = kThreadReturnAddress;
    c.cia = p.entry;
    c.runtime = t.get();

    const u32 id = s.threads.insert(PpuThread::kIdBase, t);
    if (!id) {
        std::lock_guard lock(s.memory.mutex);
        s.memory.free_stack(stack_addr, committed);
        s.memory.default_container.give_back(stack_size);
        return err(CELL_EAGAIN);
    }
    t->id = id;
    t->waiter.id = id;
    t->refs = 1;
    id_out = id;
    PS_LOG_DEBUG(kChan, "thread {:#x} '{}' created: entry {:#010x} toc {:#x} stack {:#010x}+{:#x} prio {}", id, t->name,
        p.entry, p.rtoc, stack_addr, stack_size, p.prio);
    return CELL_OK;
}

Result Kernel::start_thread(u32 id)
{
    Impl& s = *impl_;
    const std::shared_ptr<PpuThread> t = s.threads.get<PpuThread>(id);
    if (!t || t->attached) return err(CELL_ESRCH);
    {
        std::lock_guard lock(t->lock);
        if (t->started) return err(CELL_EBUSY);
        t->started = true;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, config_.host_stack_size); // D-015
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#if defined(__APPLE__)
    // Plan section 6 "Threading": the PPU main thread runs at userInteractive; other PPU threads at userInitiated.
    pthread_attr_set_qos_class_np(&attr, t->is_main ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
#endif
    {
        std::lock_guard lock(s.host_threads->mutex);
        s.host_threads->live++;
    }
    auto* args = new StartArgs{t, s.host_threads};
    pthread_t handle;
    const int rc = pthread_create(&handle, &attr, host_thread_main, args);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete args;
        {
            std::lock_guard lock(s.host_threads->mutex);
            s.host_threads->live--;
        }
        std::lock_guard lock(t->lock);
        t->started = false;
        PS_LOG_ERROR(kChan, "pthread_create failed for thread {:#x}: {}", id, rc);
        return err(CELL_EAGAIN);
    }
    return CELL_OK;
}

PpuThread* Kernel::attach_thread(PPUContext& ctx, std::string_view name, s32 prio)
{
    auto t = std::make_shared<PpuThread>(*this);
    t->attached = true;
    t->ctx = &ctx;
    t->name = std::string(name.substr(0, kMaxThreadName));
    t->started = true;
    t->join = JoinState::Detached;
    t->waiter.prio = prio;
    const u32 id = impl_->threads.insert(PpuThread::kIdBase, t);
    if (!id) PS_FATAL(kChan, "attach_thread: out of PPU thread IDs");
    t->id = id;
    t->waiter.id = id;
    t->refs = 1;
    ctx.runtime = t.get();
    set_current_thread(t.get());
    if (impl_->exiting) abort_wait(t->waiter);
    return t.get();
}

void Kernel::detach_thread(PpuThread* t)
{
    if (!t) return;
    if (current_thread() == t) set_current_thread(nullptr);
    if (t->ctx && t->ctx->runtime == t) t->ctx->runtime = nullptr;
    impl_->threads.remove(PpuThread::kIdBase, t->id); // may destroy *t
}

// ------------------------------------------------------------------------------------------------- exit

void thread_finish(PpuThread& t, u64 status)
{
    Kernel::Impl& s = t.kernel.impl();
    if (t.attached) {
        // A host thread with an lv2 identity: it keeps the identity until detach_thread().
        std::lock_guard jl(s.join_mutex);
        t.exit_status = status;
        return;
    }
    bool reap = false;
    Waiter* joiner = nullptr;
    {
        std::lock_guard jl(s.join_mutex);
        if (t.finished) return;
        t.finished = true;
        t.exit_status = status;
        switch (t.join) {
        case JoinState::Joinable: t.join = JoinState::Zombie; break;
        case JoinState::Joining:
            // Claim the joiner (the cancel side of its park sees t.joiner cleared and waits for the wake), but wake
            // it only once the stack is released and the ID is gone, so a join that returned leaves nothing behind.
            t.join = JoinState::Exited;
            joiner = t.joiner;
            t.joiner = nullptr;
            reap = true;
            break;
        case JoinState::Detached:
            t.join = JoinState::Exited;
            reap = true;
            break;
        case JoinState::Zombie:
        case JoinState::Exited: break;
        }
    }
    if (!t.attached && t.stack_addr) {
        std::lock_guard lock(s.memory.mutex);
        s.memory.free_stack(t.stack_addr, t.stack_commit);
        s.memory.default_container.give_back(t.stack_size);
        t.stack_addr = 0;
    }
    PS_LOG_DEBUG(kChan, "thread {:#x} exited with {:#x}", t.id, status);
    if (reap) s.threads.remove(PpuThread::kIdBase, t.id); // a joiner holds a reference, so *t outlives this
    if (joiner) {
        std::lock_guard jl(s.join_mutex);
        wake(*joiner, CELL_OK);
    }
}

void _sys_ppu_thread_exit(PpuThread& t, u64 errorcode)
{
    t.ctx->gpr[3] = errorcode;
    thread_finish(t, errorcode);
    throw ThreadExit{};
}

// ------------------------------------------------------------------------------------------------- syscalls

s32 sys_ppu_thread_yield(PpuThread& t)
{
    if (!yield_wait(t)) throw ThreadExit{};
    return CELL_OK;
}

Result sys_ppu_thread_join(PpuThread& t, u32 thread_id, u32 vptr)
{
    Kernel::Impl& s = t.kernel.impl();
    if (thread_id == t.id) return err(CELL_EDEADLK);
    const std::shared_ptr<PpuThread> target = s.threads.get<PpuThread>(thread_id);
    if (!target) return err(CELL_ESRCH);

    u64 status = 0;
    std::unique_lock jl(s.join_mutex);
    switch (target->join) {
    case JoinState::Joinable: {
        target->join = JoinState::Joining;
        target->joiner = &t.waiter;
        prepare_wait(t.waiter);
        const WaitStatus ws = park(t.waiter, jl, 0, {WaitKind::Join, thread_id}, [&](bool) {
            if (target->joiner == &t.waiter) {
                target->joiner = nullptr;
                target->join = JoinState::Joinable;
                return CancelResult::Removed;
            }
            return CancelResult::Woken;
        });
        if (ws != WaitStatus::Woken) return finish(t, err(CELL_EABORT));
        std::lock_guard relock(s.join_mutex);
        status = target->exit_status;
        break;
    }
    case JoinState::Zombie:
        target->join = JoinState::Exited;
        status = target->exit_status;
        jl.unlock();
        s.threads.remove(PpuThread::kIdBase, thread_id);
        break;
    case JoinState::Exited: return err(CELL_ESRCH);
    case JoinState::Detached:
    case JoinState::Joining: return err(CELL_EINVAL);
    }

    if (!s.guest.put64(vptr, status)) return err(CELL_EFAULT);
    return CELL_OK;
}

Result sys_ppu_thread_detach(PpuThread& t, u32 thread_id)
{
    Kernel::Impl& s = t.kernel.impl();
    const std::shared_ptr<PpuThread> target = s.threads.get<PpuThread>(thread_id);
    if (!target) return err(CELL_ESRCH);
    std::unique_lock jl(s.join_mutex);
    switch (target->join) {
    case JoinState::Joinable: target->join = JoinState::Detached; return CELL_OK;
    case JoinState::Detached: return err(CELL_EINVAL);
    case JoinState::Zombie:
        target->join = JoinState::Exited;
        jl.unlock();
        s.threads.remove(PpuThread::kIdBase, thread_id);
        return CELL_OK;
    case JoinState::Exited: return err(CELL_ESRCH);
    case JoinState::Joining: return err(CELL_EBUSY);
    }
    return err(CELL_ESRCH);
}

Result sys_ppu_thread_get_join_state(PpuThread& t, u32 isjoinable)
{
    Kernel::Impl& s = t.kernel.impl();
    s32 joinable = 0;
    {
        std::lock_guard jl(s.join_mutex);
        joinable = t.join != JoinState::Detached ? 1 : 0;
    }
    if (!s.guest.put32(isjoinable, static_cast<u32>(joinable))) return err(CELL_EFAULT);
    return CELL_OK;
}

Result sys_ppu_thread_set_priority(PpuThread& t, u32 thread_id, s32 prio)
{
    if (!valid_priority(t.kernel, prio)) return err(CELL_EINVAL);
    if (thread_id == t.id) {
        t.waiter.prio = prio;
        return CELL_OK;
    }
    const std::shared_ptr<PpuThread> target = t.kernel.impl().threads.get<PpuThread>(thread_id);
    if (!target) return err(CELL_ESRCH);
    // Queued waiters are ordered when dequeued, so a new priority takes effect at the next wake-up.
    target->waiter.prio = prio;
    return CELL_OK;
}

Result sys_ppu_thread_get_priority(PpuThread& t, u32 thread_id, u32 priop)
{
    Kernel::Impl& s = t.kernel.impl();
    s32 prio = 0;
    if (thread_id == t.id) {
        prio = t.priority();
    } else {
        const std::shared_ptr<PpuThread> target = s.threads.get<PpuThread>(thread_id);
        if (!target) return err(CELL_ESRCH);
        prio = target->priority();
    }
    if (!s.guest.put32(priop, static_cast<u32>(prio))) return err(CELL_EFAULT);
    return CELL_OK;
}

Result sys_ppu_thread_get_stack_information(PpuThread& t, u32 sp)
{
    Guest& g = t.kernel.impl().guest;
    if (!g.valid(sp, 8)) return err(CELL_EFAULT);
    g.w32(sp, t.stack_addr);
    g.w32(sp + 4, t.stack_size);
    return CELL_OK;
}

Result sys_ppu_thread_stop(PpuThread& t, u32 thread_id)
{
    if (!t.kernel.config().root) return err(CELL_ENOSYS);
    if (!t.kernel.impl().threads.get<PpuThread>(thread_id)) return err(CELL_ESRCH);
    return CELL_OK; // RPCS3: todo, no effect
}

Result sys_ppu_thread_restart(PpuThread& t)
{
    if (!t.kernel.config().root) return err(CELL_ENOSYS);
    return CELL_OK; // RPCS3: todo, no effect
}

Result _sys_ppu_thread_create(PpuThread& t, u32 thread_id, u32 param, u64 arg, u64 unk, s32 prio, u64 stacksize,
    u64 flags, u32 threadname)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    if (!g.valid(param, 8)) return err(CELL_EFAULT);
    const u32 opd = g.r32(param);   // ppu_thread_param_t::entry (function descriptor)
    const u32 tls = g.r32(param + 4);
    if (!opd || !g.valid(opd, 8)) return err(CELL_EFAULT);
    if (!valid_priority(k, prio)) return err(CELL_EINVAL);
    if ((flags & 3) == 3) return err(CELL_EPERM); // joinable + interrupt
    if (!g.valid(thread_id, 8)) return err(CELL_EFAULT);

    // 0 and UINT64_MAX both become 4096 (RPCS3); sizes beyond 4 GB can't be taken from the container.
    const u64 aligned = (stacksize + 0xFFF) & ~u64{0xFFF};
    const u64 stack = aligned ? aligned : 0x1000;
    if (stack > 0xFFFFF000ull) return err(CELL_ENOMEM);

    std::string name;
    if (threadname) {
        if (g.read_cstr(threadname, kMaxThreadName, name) == Guest::Str::Fault) return err(CELL_EFAULT);
    }

    ThreadParams p;
    p.name = std::move(name);
    p.entry = g.r32(opd);
    p.rtoc = g.r32(opd + 4);
    p.arg0 = arg;
    p.arg1 = unk;
    p.tls = tls;
    p.prio = prio;
    p.stack_size = static_cast<u32>(stack);
    p.joinable = (flags & SYS_PPU_THREAD_CREATE_JOINABLE) != 0;
    u32 id = 0;
    if (const Result r = k.create_thread(p, id); r != CELL_OK) return r;
    if (flags & SYS_PPU_THREAD_CREATE_INTERRUPT) {
        const auto created = k.thread(id);
        created->interrupt = true;
        created->ctx->gpr[3] = 0; // interrupt threads get no arguments (RPCS3 ppu_thread constructor)
        created->ctx->gpr[4] = 0;
    }
    g.w64(thread_id, id);
    return CELL_OK;
}

Result sys_ppu_thread_start(PpuThread& t, u32 thread_id) { return t.kernel.start_thread(thread_id); }

Result sys_ppu_thread_rename(PpuThread& t, u32 thread_id, u32 name)
{
    Kernel::Impl& s = t.kernel.impl();
    const std::shared_ptr<PpuThread> target = s.threads.get<PpuThread>(thread_id);
    if (!target) return err(CELL_ESRCH);
    std::string str;
    if (s.guest.read_cstr(name, kMaxThreadName, str) == Guest::Str::Fault) return err(CELL_EFAULT);
    std::lock_guard lock(target->lock);
    target->name = std::move(str);
    return CELL_OK;
}

u32 sys_ppu_thread_get_id(PpuThread& t) { return t.id; }

void register_thread_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 41, _sys_ppu_thread_exit);
    PS_LV2_BIND(table, 43, sys_ppu_thread_yield);
    PS_LV2_BIND(table, 44, sys_ppu_thread_join);
    PS_LV2_BIND(table, 45, sys_ppu_thread_detach);
    PS_LV2_BIND(table, 46, sys_ppu_thread_get_join_state);
    PS_LV2_BIND(table, 47, sys_ppu_thread_set_priority);
    PS_LV2_BIND(table, 48, sys_ppu_thread_get_priority);
    PS_LV2_BIND(table, 49, sys_ppu_thread_get_stack_information);
    PS_LV2_BIND(table, 50, sys_ppu_thread_stop);
    PS_LV2_BIND(table, 51, sys_ppu_thread_restart);
    PS_LV2_BIND(table, 52, _sys_ppu_thread_create);
    PS_LV2_BIND(table, 53, sys_ppu_thread_start);
    PS_LV2_BIND(table, 56, sys_ppu_thread_rename);
}

} // namespace pspocket::lv2
