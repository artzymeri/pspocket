// Kernel side of sys_lwmutex / sys_lwcond: sleep queues driven by the user-space halves in sysPrxForUser
// (sys_lwmutex_lock etc. do the atomics on the guest's sys_lwmutex_t and only enter the kernel on contention).
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_lwmutex.h/.cpp, sys_lwcond.h/.cpp, and for the protocol the user-space
// halves in Emu/Cell/Modules/sys_lwmutex_.cpp and sys_lwcond_.cpp.
//
// Deviation: an unknown _sys_lwcond_signal / _signal_all mode returns CELL_EINVAL (RPCS3 throws).

#include <climits>

#include "internal.hpp"

namespace pspocket::lv2 {

// ------------------------------------------------------------------------------------------------- LwMutex

s32 LwMutex::own_or_queue_locked(Waiter& w)
{
    if (signaled) {
        const s32 s = signaled;
        signaled = 0;
        return s;
    }
    sleepers.push(&w);
    if (destroying) destroy_cv.notify_all(); // a destroyer waiting on lwcond waiters must now fail with EBUSY
    return 0;
}

void LwMutex::release_locked(bool unlock2)
{
    if (Waiter* next = sleepers.pop(protocol)) {
        wake(*next, unlock2 ? err(CELL_EBUSY) : CELL_OK);
    } else {
        signaled |= unlock2 ? INT_MIN : 1;
    }
}

Result LwMutex::lock_wait(Waiter& w, u64 timeout_us)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    prepare_wait(w);
    if (const s32 s = own_or_queue_locked(w)) return (s & 1) ? CELL_OK : err(CELL_EBUSY); // bit 31 only: unlock2
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::LwMutex, id},
        [&](bool) { return sleepers.remove(&w) ? CancelResult::Removed : CancelResult::Woken; });
    switch (ws) {
    case WaitStatus::Woken: return w.result; // CELL_OK, or CELL_EBUSY after _sys_lwmutex_unlock2
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

Result LwMutex::try_lock()
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (signaled & 1) {
        signaled = 0;
        return CELL_OK;
    }
    return err(CELL_EBUSY);
}

Result LwMutex::unlock(bool unlock2)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    release_locked(unlock2);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- LwCond

Result LwCond::queue_wait(Waiter& w, LwMutex& mutex, u64 timeout_us)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    {
        std::lock_guard mlk(mutex.lock);
        if (mutex.dead) return err(CELL_ESRCH);
        mutex.lwcond_waiters++;
        lwmutex_waiters++;
        prepare_wait(w);
        waiters.push(&w);
        // Release the lwmutex's kernel side: wake its next sleeper, or leave the signal for the next locker.
        mutex.release_locked(false);
    }

    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::LwCond, id}, [&](bool) {
        if (waiters.remove(&w)) return CancelResult::Removed;
        std::lock_guard mlk(mutex.lock);
        if (mutex.sleepers.remove(&w)) return CancelResult::Removed;
        return CancelResult::Woken;
    });

    {
        std::lock_guard mlk(mutex.lock);
        if (--mutex.lwcond_waiters == 0 && mutex.destroying) mutex.destroy_cv.notify_all();
    }
    {
        std::lock_guard relock(lock);
        if (--lwmutex_waiters == 0 && destroying) destroy_cv.notify_all();
    }

    switch (ws) {
    case WaitStatus::Woken: return w.result; // CELL_OK: lwmutex handed over; CELL_EBUSY: re-lock in user space
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

int LwCond::signal(LwMutex* mutex, const Waiter* target, u32 mode)
{
    std::lock_guard lk(lock);
    if (dead) return 0;
    Waiter* w = nullptr;
    if (target) {
        for (Waiter* it : waiters.items()) {
            if (it == target) w = it;
        }
        if (w) waiters.remove(w);
    } else {
        w = waiters.pop(protocol);
    }
    if (!w) return 0;

    if (mode == 2) {
        // The signaler didn't own the lwmutex: the waiter re-locks it in user space.
        wake(*w, err(CELL_EBUSY));
        return 1;
    }
    std::lock_guard mlk(mutex->lock);
    if (mode == 1) {
        // The signaler owns the lwmutex: the waiter sleeps on it until the owner unlocks.
        if (mutex->signaled) PS_LOG_WARN(kChan, "_sys_lwcond_signal mode 1: lwmutex {:#x} is signaled", mutex->id);
        mutex->sleepers.push(w);
        return 1;
    }
    // Mode 3: the lwmutex was forcibly owned for the waiter; respect the order of its sleep queue.
    if (!mutex->sleepers.empty()) {
        mutex->sleepers.push(w);
        w = mutex->sleepers.pop(mutex->protocol);
    }
    wake(*w, CELL_OK);
    return 1;
}

u32 LwCond::signal_all(LwMutex* mutex, u32 mode)
{
    std::lock_guard lk(lock);
    if (dead) return 0;
    const std::vector<Waiter*> all = waiters.take_all(protocol);
    if (mode == 2) {
        for (Waiter* w : all) wake(*w, err(CELL_EBUSY));
    } else {
        std::lock_guard mlk(mutex->lock);
        for (Waiter* w : all) mutex->sleepers.push(w);
    }
    return static_cast<u32>(all.size());
}

// ------------------------------------------------------------------------------------------------- syscalls

Result _sys_lwmutex_create(PpuThread& t, u32 lwmutex_id, u32 protocol, u32 control, s32 has_name, u64 name)
{
    Kernel::Impl& s = t.kernel.impl();
    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_RETRY && protocol != SYS_SYNC_PRIORITY) {
        PS_LOG_ERROR(kChan, "_sys_lwmutex_create: unknown protocol {:#x}", protocol);
        return err(CELL_EINVAL);
    }
    if (!s.guest.valid(lwmutex_id, 4)) return err(CELL_EFAULT);
    auto m = std::make_shared<LwMutex>(protocol, control, has_name < 0 ? name : 0);
    m->kind = static_cast<u32>(has_name) & 0x0FFFFFFF;
    m->refs = 1;
    const u32 id = s.objects.insert(LwMutex::kIdBase, m);
    if (!id) return err(CELL_EAGAIN);
    m->id = id;
    s.guest.w32(lwmutex_id, id);
    return CELL_OK;
}

Result _sys_lwmutex_destroy(PpuThread& t, u32 lwmutex_id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto m = s.objects.get<LwMutex>(lwmutex_id);
    if (!m) return err(CELL_ESRCH);
    {
        std::unique_lock lk(m->lock);
        if (m->dead) return err(CELL_ESRCH);
        // Threads inside _sys_lwcond_queue_wait still use the lwmutex: wait for them to leave (RPCS3), but fail if
        // one of them (or anyone) starts sleeping on the lwmutex meanwhile.
        while (m->sleepers.empty() && m->lwcond_waiters > 0) {
            m->destroying = true;
            m->destroy_cv.wait_for(lk, std::chrono::milliseconds(50));
            if (wait_aborted(t.waiter)) {
                m->destroying = false;
                throw ThreadExit{};
            }
        }
        m->destroying = false;
        if (!m->sleepers.empty()) return err(CELL_EBUSY);
        m->dead = true;
    }
    s.objects.remove(LwMutex::kIdBase, lwmutex_id);
    return CELL_OK;
}

Result _sys_lwmutex_lock(PpuThread& t, u32 lwmutex_id, u64 timeout)
{
    const auto m = t.kernel.impl().objects.get<LwMutex>(lwmutex_id);
    if (!m) return err(CELL_ESRCH);
    return finish(t, m->lock_wait(t.waiter, timeout));
}

Result _sys_lwmutex_unlock(PpuThread& t, u32 lwmutex_id)
{
    const auto m = t.kernel.impl().objects.get<LwMutex>(lwmutex_id);
    if (!m) return err(CELL_ESRCH);
    return m->unlock(false);
}

Result _sys_lwmutex_unlock2(PpuThread& t, u32 lwmutex_id)
{
    const auto m = t.kernel.impl().objects.get<LwMutex>(lwmutex_id);
    if (!m) return err(CELL_ESRCH);
    return m->unlock(true);
}

Result _sys_lwmutex_trylock(PpuThread& t, u32 lwmutex_id)
{
    const auto m = t.kernel.impl().objects.get<LwMutex>(lwmutex_id);
    if (!m) return err(CELL_ESRCH);
    return m->try_lock();
}

Result _sys_lwcond_create(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u32 control, u64 name)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto m = s.objects.get<LwMutex>(lwmutex_id);
    if (!m) return err(CELL_ESRCH);
    if (!s.guest.valid(lwcond_id, 4)) return err(CELL_EFAULT);
    const u32 protocol = m->protocol == SYS_SYNC_RETRY ? SYS_SYNC_PRIORITY : m->protocol; // lwcond can't RETRY
    auto c = std::make_shared<LwCond>(name, lwmutex_id, protocol, control);
    c->refs = 1;
    const u32 id = s.objects.insert(LwCond::kIdBase, c);
    if (!id) return err(CELL_EAGAIN);
    c->id = id;
    s.guest.w32(lwcond_id, id);
    return CELL_OK;
}

Result _sys_lwcond_destroy(PpuThread& t, u32 lwcond_id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto c = s.objects.get<LwCond>(lwcond_id);
    if (!c) return err(CELL_ESRCH);
    {
        std::unique_lock lk(c->lock);
        if (c->dead) return err(CELL_ESRCH);
        if (!c->waiters.empty()) return err(CELL_EBUSY);
        // Signaled waiters still sleeping on the lwmutex belong to this lwcond until they return (RPCS3).
        while (c->lwmutex_waiters > 0) {
            c->destroying = true;
            c->destroy_cv.wait_for(lk, std::chrono::milliseconds(50));
            if (wait_aborted(t.waiter)) {
                c->destroying = false;
                throw ThreadExit{};
            }
            if (!c->waiters.empty()) {
                c->destroying = false;
                return err(CELL_EBUSY);
            }
        }
        c->destroying = false;
        c->dead = true;
    }
    s.objects.remove(LwCond::kIdBase, lwcond_id);
    return CELL_OK;
}

Result _sys_lwcond_queue_wait(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u64 timeout)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto c = s.objects.get<LwCond>(lwcond_id);
    const auto m = s.objects.get<LwMutex>(lwmutex_id);
    if (!c || !m) return err(CELL_ESRCH);
    return finish(t, c->queue_wait(t.waiter, *m, timeout));
}

Result _sys_lwcond_signal(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u64 ppu_thread_id, u32 mode)
{
    // Mode 1: the caller owns the lwmutex; 2: it doesn't (waiter re-locks); 3: the lwmutex was forcibly owned.
    if (mode < 1 || mode > 3) {
        PS_LOG_ERROR(kChan, "_sys_lwcond_signal: unknown mode {}", mode);
        return err(CELL_EINVAL);
    }
    Kernel::Impl& s = t.kernel.impl();
    const auto c = s.objects.get<LwCond>(lwcond_id);
    if (!c) return err(CELL_ESRCH);
    const bool any = ppu_thread_id == 0xFFFFFFFFull;
    std::shared_ptr<PpuThread> target;
    if (!any) {
        target = s.threads.get<PpuThread>(static_cast<u32>(ppu_thread_id));
        if (!target) return err(CELL_ESRCH);
    }
    std::shared_ptr<LwMutex> m;
    if (mode != 2) {
        m = s.objects.get<LwMutex>(lwmutex_id);
        if (!m) return err(CELL_ESRCH);
    }
    if (c->signal(m.get(), target ? &target->waiter : nullptr, mode)) return CELL_OK;
    if (any) {
        if (mode == 3) return err(CELL_ENOENT); // not an error for the guest
        if (mode == 2) return CELL_OK;
    }
    return err(CELL_EPERM);
}

Result _sys_lwcond_signal_all(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u32 mode)
{
    if (mode < 1 || mode > 2) {
        PS_LOG_ERROR(kChan, "_sys_lwcond_signal_all: unknown mode {}", mode);
        return err(CELL_EINVAL);
    }
    Kernel::Impl& s = t.kernel.impl();
    const auto c = s.objects.get<LwCond>(lwcond_id);
    if (!c) return err(CELL_ESRCH);
    std::shared_ptr<LwMutex> m;
    if (mode != 2) {
        m = s.objects.get<LwMutex>(lwmutex_id);
        if (!m) return err(CELL_ESRCH);
    }
    const u32 n = c->signal_all(m.get(), mode);
    return mode == 1 ? static_cast<Result>(n) : CELL_OK; // mode 1 returns the count (RPCS3)
}

void register_lw_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 95, _sys_lwmutex_create);
    PS_LV2_BIND(table, 96, _sys_lwmutex_destroy);
    PS_LV2_BIND(table, 97, _sys_lwmutex_lock);
    PS_LV2_BIND(table, 98, _sys_lwmutex_unlock);
    PS_LV2_BIND(table, 99, _sys_lwmutex_trylock);
    PS_LV2_BIND(table, 111, _sys_lwcond_create);
    PS_LV2_BIND(table, 112, _sys_lwcond_destroy);
    PS_LV2_BIND(table, 113, _sys_lwcond_queue_wait);
    PS_LV2_BIND(table, 115, _sys_lwcond_signal);
    PS_LV2_BIND(table, 116, _sys_lwcond_signal_all);
    PS_LV2_BIND(table, 117, _sys_lwmutex_unlock2);
}

} // namespace pspocket::lv2
