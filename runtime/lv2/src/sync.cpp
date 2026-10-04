// sys_mutex, sys_cond, sys_rwlock, sys_semaphore.
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_mutex.h/.cpp, sys_cond.cpp, sys_rwlock.cpp, sys_semaphore.cpp,
// sys_sync.h (lv2_obj::create, protocols).
//
// RPCS3 spins briefly before sleeping and emulates the PS3 scheduler around wake-ups; here a blocked thread parks
// on its host condition variable and the waker hands the object over directly, which gives the same observable
// results (ownership order by protocol, timeouts, error codes).

#include "internal.hpp"

namespace pspocket::lv2 {

// ------------------------------------------------------------------------------------------------- Mutex

bool Mutex::own_or_queue_locked(Waiter& w)
{
    if (owner == 0) {
        owner = w.id;
        return true;
    }
    waiters.push(&w);
    return false;
}

void Mutex::hand_off_locked()
{
    if (Waiter* next = waiters.pop(protocol)) {
        owner = next->id;
        wake(*next, CELL_OK);
    } else {
        owner = 0;
    }
}

Result Mutex::lock_wait(Waiter& w, u64 timeout_us)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (owner == w.id) {
        if (!recursive) return err(CELL_EDEADLK);
        if (lock_count == 0xFFFFFFFFu) return err(CELL_EKRESOURCE);
        lock_count++;
        return CELL_OK;
    }
    prepare_wait(w);
    if (own_or_queue_locked(w)) return CELL_OK;
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::Mutex, id},
        [&](bool) { return waiters.remove(&w) ? CancelResult::Removed : CancelResult::Woken; });
    switch (ws) {
    case WaitStatus::Woken: return CELL_OK; // the unlocking thread made us the owner
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

Result Mutex::try_lock(Waiter& w)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (owner == 0) {
        owner = w.id;
        return CELL_OK;
    }
    if (owner == w.id) {
        if (!recursive) return err(CELL_EDEADLK);
        if (lock_count == 0xFFFFFFFFu) return err(CELL_EKRESOURCE);
        lock_count++;
        return CELL_OK;
    }
    return err(CELL_EBUSY);
}

Result Mutex::unlock(Waiter& w)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (owner != w.id) return err(CELL_EPERM);
    if (lock_count) {
        lock_count--;
        return CELL_OK;
    }
    hand_off_locked();
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- Cond

Result Cond::wait(Waiter& w, u64 timeout_us)
{
    Mutex& m = *mutex;
    std::unique_lock lk(m.lock);
    if (dead) return err(CELL_ESRCH);
    if (m.owner != w.id) return err(CELL_EPERM);

    prepare_wait(w);
    waiters.push(&w);
    // Release the mutex completely, remembering the recursion depth (restored when the mutex comes back).
    const u32 saved_count = m.lock_count;
    m.lock_count = 0;
    m.hand_off_locked();

    bool timed_out = false;
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::Cond, id}, [&](bool aborting) {
        if (waiters.remove(&w)) {
            // Still waiting on the condition: the timeout applies; then the mutex must be re-acquired.
            timed_out = true;
            if (aborting || m.own_or_queue_locked(w)) return CancelResult::Removed;
            return CancelResult::Requeued;
        }
        if (m.waiters.contains(&w)) {
            // Signaled and now queued for the mutex: re-acquiring it has no timeout (RPCS3 sys_cond_wait).
            if (aborting) {
                m.waiters.remove(&w);
                return CancelResult::Removed;
            }
            return CancelResult::Requeued;
        }
        return CancelResult::Woken;
    });
    if (ws == WaitStatus::Aborted) return err(CELL_EABORT);

    std::lock_guard relock(m.lock);
    m.lock_count = saved_count;
    return timed_out ? err(CELL_ETIMEDOUT) : CELL_OK;
}

Result Cond::signal()
{
    std::lock_guard lk(mutex->lock);
    if (dead) return err(CELL_ESRCH);
    if (Waiter* w = waiters.pop(mutex->protocol)) {
        if (mutex->own_or_queue_locked(*w)) wake(*w, CELL_OK);
    }
    return CELL_OK;
}

Result Cond::signal_all()
{
    std::lock_guard lk(mutex->lock);
    if (dead) return err(CELL_ESRCH);
    // RPCS3 moves the waiters in priority order: the first takes a free mutex, the rest queue on it.
    for (Waiter* w : waiters.take_all(SYS_SYNC_PRIORITY)) {
        if (mutex->own_or_queue_locked(*w)) wake(*w, CELL_OK);
    }
    return CELL_OK;
}

Result Cond::signal_to(const Waiter* target)
{
    std::lock_guard lk(mutex->lock);
    if (dead) return err(CELL_ESRCH);
    for (Waiter* w : waiters.items()) {
        if (w == target) {
            waiters.remove(w);
            if (mutex->own_or_queue_locked(*w)) wake(*w, CELL_OK);
            return CELL_OK;
        }
    }
    return err(CELL_EPERM);
}

// ------------------------------------------------------------------------------------------------- RwLock

Result RwLock::rlock(Waiter& w, u64 timeout_us)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (!writer && read_waiters.empty() && write_waiters.empty()) {
        readers++;
        return CELL_OK;
    }
    prepare_wait(w);
    read_waiters.push(&w);
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::RwLockRead, id},
        [&](bool) { return read_waiters.remove(&w) ? CancelResult::Removed : CancelResult::Woken; });
    switch (ws) {
    case WaitStatus::Woken: return CELL_OK; // the waker counted us as a reader
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

Result RwLock::try_rlock()
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (!writer && read_waiters.empty() && write_waiters.empty()) {
        readers++;
        return CELL_OK;
    }
    return err(CELL_EBUSY);
}

Result RwLock::runlock()
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (readers == 0) return err(CELL_EPERM);
    if (--readers == 0) {
        if (Waiter* next = write_waiters.pop(protocol)) {
            writer = next->id;
            wake(*next, CELL_OK);
        }
    }
    return CELL_OK;
}

Result RwLock::wlock(Waiter& w, u64 timeout_us)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (writer == w.id) return err(CELL_EDEADLK);
    if (!busy_locked()) {
        writer = w.id;
        return CELL_OK;
    }
    prepare_wait(w);
    write_waiters.push(&w);
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::RwLockWrite, id}, [&](bool) {
        if (!write_waiters.remove(&w)) return CancelResult::Woken;
        // The last waiting writer left while readers hold the lock: the readers queued behind it may enter.
        if (write_waiters.empty() && !writer && !read_waiters.empty()) {
            for (Waiter* r : read_waiters.take_all(SYS_SYNC_FIFO)) {
                readers++;
                wake(*r, CELL_OK);
            }
        }
        return CancelResult::Removed;
    });
    switch (ws) {
    case WaitStatus::Woken: return CELL_OK;
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

Result RwLock::try_wlock(Waiter& w)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (writer == w.id) return err(CELL_EDEADLK);
    if (busy_locked()) return err(CELL_EBUSY);
    writer = w.id;
    return CELL_OK;
}

Result RwLock::wunlock(Waiter& w)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (writer != w.id) return err(CELL_EPERM);
    writer = 0;
    if (Waiter* next = write_waiters.pop(protocol)) {
        writer = next->id;
        wake(*next, CELL_OK);
    } else {
        for (Waiter* r : read_waiters.take_all(SYS_SYNC_FIFO)) {
            readers++;
            wake(*r, CELL_OK);
        }
    }
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- Semaphore

Result Semaphore::wait(Waiter& w, u64 timeout_us)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (value-- > 0) return CELL_OK;
    prepare_wait(w);
    waiters.push(&w);
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::Semaphore, id}, [&](bool) {
        if (!waiters.remove(&w)) return CancelResult::Woken;
        value++;
        return CancelResult::Removed;
    });
    switch (ws) {
    case WaitStatus::Woken: return CELL_OK;
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

Result Semaphore::try_wait()
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (value <= 0) return err(CELL_EBUSY);
    value--;
    return CELL_OK;
}

Result Semaphore::post(s32 count)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (count <= 0) return err(CELL_EINVAL);
    if (s64{count} > s64{max} - value) return err(CELL_EBUSY);
    const s32 old = value;
    value += count;
    const s32 to_wake = std::min(std::max(-old, 0), count);
    for (s32 i = 0; i < to_wake; i++) {
        Waiter* w = waiters.pop(protocol);
        if (!w) break;
        wake(*w, CELL_OK);
    }
    return CELL_OK;
}

s32 Semaphore::get_value()
{
    std::lock_guard lk(lock);
    return std::max(value, 0);
}

// ------------------------------------------------------------------------------------------------- syscalls

namespace {

bool valid_protocol(u32 protocol) { return protocol == SYS_SYNC_FIFO || protocol == SYS_SYNC_PRIORITY; }

} // namespace

Result sys_mutex_create(PpuThread& t, u32 mutex_id, u32 attr)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    if (!g.valid(mutex_id, 4) || !g.valid(attr, 40)) return err(CELL_EFAULT);
    // sys_mutex_attribute_t: protocol, recursive, pshared, adaptive, ipc_key (u64), flags, pad, name[8]
    const u32 protocol = g.r32(attr);
    const u32 recursive = g.r32(attr + 4);
    const u32 pshared = g.r32(attr + 8);
    const u32 adaptive = g.r32(attr + 12);
    const u64 ipc_key = g.r64(attr + 16);
    const s32 flags = static_cast<s32>(g.r32(attr + 24));
    const u64 name = g.r64(attr + 32);

    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY && protocol != SYS_SYNC_PRIORITY_INHERIT) {
        PS_LOG_ERROR(kChan, "sys_mutex_create: unknown protocol {:#x}", protocol);
        return err(CELL_EINVAL);
    }
    if (recursive != SYS_SYNC_RECURSIVE && recursive != SYS_SYNC_NOT_RECURSIVE) {
        PS_LOG_ERROR(kChan, "sys_mutex_create: unknown recursive attribute {:#x}", recursive);
        return err(CELL_EINVAL);
    }
    if (adaptive != SYS_SYNC_NOT_ADAPTIVE) PS_LOG_WARN(kChan, "sys_mutex_create: adaptive attribute {:#x} ignored", adaptive);

    u32 id = 0;
    const Result r = create_object<Mutex>(k, pshared, pshared == SYS_SYNC_PROCESS_SHARED ? ipc_key : 0, flags,
        [&] { return std::make_shared<Mutex>(protocol, recursive == SYS_SYNC_RECURSIVE, name); }, id);
    if (r != CELL_OK) return r;
    g.w32(mutex_id, id);
    return CELL_OK;
}

Result sys_mutex_destroy(PpuThread& t, u32 mutex_id)
{
    return destroy_object<Mutex>(t.kernel, mutex_id, [](Mutex& m) -> Result {
        if (m.owner) return err(CELL_EBUSY);
        if (m.cond_count) return err(CELL_EPERM);
        return CELL_OK;
    });
}

Result sys_mutex_lock(PpuThread& t, u32 mutex_id, u64 timeout)
{
    const auto m = t.kernel.impl().objects.get<Mutex>(mutex_id);
    if (!m) return err(CELL_ESRCH);
    return finish(t, m->lock_wait(t.waiter, timeout));
}

Result sys_mutex_trylock(PpuThread& t, u32 mutex_id)
{
    const auto m = t.kernel.impl().objects.get<Mutex>(mutex_id);
    if (!m) return err(CELL_ESRCH);
    return m->try_lock(t.waiter);
}

Result sys_mutex_unlock(PpuThread& t, u32 mutex_id)
{
    const auto m = t.kernel.impl().objects.get<Mutex>(mutex_id);
    if (!m) return err(CELL_ESRCH);
    return m->unlock(t.waiter);
}

Result sys_cond_create(PpuThread& t, u32 cond_id, u32 mutex_id, u32 attr)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    const auto m = k.impl().objects.get<Mutex>(mutex_id);
    if (!m) return err(CELL_ESRCH);
    if (!g.valid(cond_id, 4) || !g.valid(attr, 24)) return err(CELL_EFAULT);
    // sys_cond_attribute_t: pshared, flags, ipc_key (u64), name[8]
    const u32 pshared = g.r32(attr);
    const s32 flags = static_cast<s32>(g.r32(attr + 4));
    const u64 ipc_key = g.r64(attr + 8);
    const u64 name = g.r64(attr + 16);

    // Bind to the mutex first (RPCS3 lv2_cond::on_id_create): a destroyed mutex fails, a bound one can't be
    // destroyed (EPERM) until the condition variable is.
    {
        std::lock_guard lk(m->lock);
        if (m->dead) return err(CELL_ESRCH);
        m->cond_count++;
    }
    u32 id = 0;
    const Result r = create_object<Cond>(k, pshared, pshared == SYS_SYNC_PROCESS_SHARED ? ipc_key : 0, flags,
        [&] { return std::make_shared<Cond>(m, mutex_id, name); }, id);
    if (r != CELL_OK) {
        std::lock_guard lk(m->lock);
        m->cond_count--;
        return r;
    }
    g.w32(cond_id, id);
    return CELL_OK;
}

Result sys_cond_destroy(PpuThread& t, u32 cond_id)
{
    return destroy_object<Cond>(t.kernel, cond_id, [](Cond& c) -> Result {
        if (!c.waiters.empty()) return err(CELL_EBUSY);
        c.mutex->cond_count--;
        return CELL_OK;
    });
}

Result sys_cond_wait(PpuThread& t, u32 cond_id, u64 timeout)
{
    const auto c = t.kernel.impl().objects.get<Cond>(cond_id);
    if (!c) return err(CELL_ESRCH);
    return finish(t, c->wait(t.waiter, timeout));
}

Result sys_cond_signal(PpuThread& t, u32 cond_id)
{
    const auto c = t.kernel.impl().objects.get<Cond>(cond_id);
    if (!c) return err(CELL_ESRCH);
    return c->signal();
}

Result sys_cond_signal_all(PpuThread& t, u32 cond_id)
{
    const auto c = t.kernel.impl().objects.get<Cond>(cond_id);
    if (!c) return err(CELL_ESRCH);
    return c->signal_all();
}

Result sys_cond_signal_to(PpuThread& t, u32 cond_id, u32 thread_id)
{
    const auto c = t.kernel.impl().objects.get<Cond>(cond_id);
    if (!c) return err(CELL_ESRCH);
    const auto target = t.kernel.impl().threads.get<PpuThread>(thread_id);
    if (!target) return err(CELL_ESRCH);
    return c->signal_to(&target->waiter);
}

Result sys_rwlock_create(PpuThread& t, u32 rw_lock_id, u32 attr)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    if (!g.valid(rw_lock_id, 4) || !g.valid(attr, 32)) return err(CELL_EFAULT);
    // sys_rwlock_attribute_t: protocol, pshared, ipc_key (u64), flags, pad, name[8]
    const u32 protocol = g.r32(attr);
    const u32 pshared = g.r32(attr + 4);
    const u64 ipc_key = g.r64(attr + 8);
    const s32 flags = static_cast<s32>(g.r32(attr + 16));
    const u64 name = g.r64(attr + 24);
    if (!valid_protocol(protocol)) {
        PS_LOG_ERROR(kChan, "sys_rwlock_create: unknown protocol {:#x}", protocol);
        return err(CELL_EINVAL);
    }
    u32 id = 0;
    const Result r = create_object<RwLock>(k, pshared, pshared == SYS_SYNC_PROCESS_SHARED ? ipc_key : 0, flags,
        [&] { return std::make_shared<RwLock>(protocol, name); }, id);
    if (r != CELL_OK) return r;
    g.w32(rw_lock_id, id);
    return CELL_OK;
}

Result sys_rwlock_destroy(PpuThread& t, u32 rw_lock_id)
{
    return destroy_object<RwLock>(t.kernel, rw_lock_id,
        [](RwLock& rw) -> Result { return rw.busy_locked() ? err(CELL_EBUSY) : CELL_OK; });
}

Result sys_rwlock_rlock(PpuThread& t, u32 rw_lock_id, u64 timeout)
{
    const auto rw = t.kernel.impl().objects.get<RwLock>(rw_lock_id);
    if (!rw) return err(CELL_ESRCH);
    return finish(t, rw->rlock(t.waiter, timeout));
}

Result sys_rwlock_tryrlock(PpuThread& t, u32 rw_lock_id)
{
    const auto rw = t.kernel.impl().objects.get<RwLock>(rw_lock_id);
    if (!rw) return err(CELL_ESRCH);
    return rw->try_rlock();
}

Result sys_rwlock_runlock(PpuThread& t, u32 rw_lock_id)
{
    const auto rw = t.kernel.impl().objects.get<RwLock>(rw_lock_id);
    if (!rw) return err(CELL_ESRCH);
    return rw->runlock();
}

Result sys_rwlock_wlock(PpuThread& t, u32 rw_lock_id, u64 timeout)
{
    const auto rw = t.kernel.impl().objects.get<RwLock>(rw_lock_id);
    if (!rw) return err(CELL_ESRCH);
    return finish(t, rw->wlock(t.waiter, timeout));
}

Result sys_rwlock_trywlock(PpuThread& t, u32 rw_lock_id)
{
    const auto rw = t.kernel.impl().objects.get<RwLock>(rw_lock_id);
    if (!rw) return err(CELL_ESRCH);
    return rw->try_wlock(t.waiter);
}

Result _sys_rwlock_trywlock(PpuThread& t, u32 rw_lock_id) { return sys_rwlock_trywlock(t, rw_lock_id); }

Result sys_rwlock_wunlock(PpuThread& t, u32 rw_lock_id)
{
    const auto rw = t.kernel.impl().objects.get<RwLock>(rw_lock_id);
    if (!rw) return err(CELL_ESRCH);
    return rw->wunlock(t.waiter);
}

Result sys_semaphore_create(PpuThread& t, u32 sem_id, u32 attr, s32 initial_val, s32 max_val)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    if (!g.valid(sem_id, 4) || !g.valid(attr, 32)) return err(CELL_EFAULT);
    if (max_val <= 0 || initial_val > max_val || initial_val < 0) {
        PS_LOG_ERROR(kChan, "sys_semaphore_create: invalid values (initial {}, max {})", initial_val, max_val);
        return err(CELL_EINVAL);
    }
    // sys_semaphore_attribute_t: protocol, pshared, ipc_key (u64), flags, pad, name[8]
    const u32 protocol = g.r32(attr);
    const u32 pshared = g.r32(attr + 4);
    const u64 ipc_key = g.r64(attr + 8);
    const s32 flags = static_cast<s32>(g.r32(attr + 16));
    const u64 name = g.r64(attr + 24);
    if (!valid_protocol(protocol)) {
        PS_LOG_ERROR(kChan, "sys_semaphore_create: unknown protocol {:#x}", protocol);
        return err(CELL_EINVAL);
    }
    u32 id = 0;
    const Result r = create_object<Semaphore>(k, pshared, pshared == SYS_SYNC_PROCESS_SHARED ? ipc_key : 0, flags,
        [&] { return std::make_shared<Semaphore>(protocol, max_val, initial_val, name); }, id);
    if (r != CELL_OK) return r;
    g.w32(sem_id, id);
    return CELL_OK;
}

Result sys_semaphore_destroy(PpuThread& t, u32 sem_id)
{
    return destroy_object<Semaphore>(t.kernel, sem_id,
        [](Semaphore& s) -> Result { return s.value < 0 ? err(CELL_EBUSY) : CELL_OK; });
}

Result sys_semaphore_wait(PpuThread& t, u32 sem_id, u64 timeout)
{
    const auto s = t.kernel.impl().objects.get<Semaphore>(sem_id);
    if (!s) return err(CELL_ESRCH);
    return finish(t, s->wait(t.waiter, timeout));
}

Result sys_semaphore_trywait(PpuThread& t, u32 sem_id)
{
    const auto s = t.kernel.impl().objects.get<Semaphore>(sem_id);
    if (!s) return err(CELL_ESRCH);
    return s->try_wait();
}

Result sys_semaphore_post(PpuThread& t, u32 sem_id, s32 count)
{
    const auto s = t.kernel.impl().objects.get<Semaphore>(sem_id);
    if (!s) return err(CELL_ESRCH);
    return s->post(count);
}

Result sys_semaphore_get_value(PpuThread& t, u32 sem_id, u32 count)
{
    const auto s = t.kernel.impl().objects.get<Semaphore>(sem_id);
    if (!s) return err(CELL_ESRCH);
    const s32 v = s->get_value();
    if (!t.kernel.impl().guest.put32(count, static_cast<u32>(v))) return err(CELL_EFAULT);
    return CELL_OK;
}

void register_sync_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 90, sys_semaphore_create);
    PS_LV2_BIND(table, 91, sys_semaphore_destroy);
    PS_LV2_BIND(table, 92, sys_semaphore_wait);
    PS_LV2_BIND(table, 93, sys_semaphore_trywait);
    PS_LV2_BIND(table, 94, sys_semaphore_post);
    PS_LV2_BIND(table, 100, sys_mutex_create);
    PS_LV2_BIND(table, 101, sys_mutex_destroy);
    PS_LV2_BIND(table, 102, sys_mutex_lock);
    PS_LV2_BIND(table, 103, sys_mutex_trylock);
    PS_LV2_BIND(table, 104, sys_mutex_unlock);
    PS_LV2_BIND(table, 105, sys_cond_create);
    PS_LV2_BIND(table, 106, sys_cond_destroy);
    PS_LV2_BIND(table, 107, sys_cond_wait);
    PS_LV2_BIND(table, 108, sys_cond_signal);
    PS_LV2_BIND(table, 109, sys_cond_signal_all);
    PS_LV2_BIND(table, 110, sys_cond_signal_to);
    PS_LV2_BIND(table, 114, sys_semaphore_get_value);
    PS_LV2_BIND(table, 120, sys_rwlock_create);
    PS_LV2_BIND(table, 121, sys_rwlock_destroy);
    PS_LV2_BIND(table, 122, sys_rwlock_rlock);
    PS_LV2_BIND(table, 123, sys_rwlock_tryrlock);
    PS_LV2_BIND(table, 124, sys_rwlock_runlock);
    PS_LV2_BIND(table, 125, sys_rwlock_wlock);
    PS_LV2_BIND(table, 126, sys_rwlock_trywlock);
    PS_LV2_BIND(table, 127, sys_rwlock_wunlock);
    PS_LV2_BIND(table, 148, _sys_rwlock_trywlock);
}

} // namespace pspocket::lv2
