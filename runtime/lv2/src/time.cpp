// sys_time and sys_timer.
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_time.cpp, sys_timer.h/.cpp.
//
// Deviations:
// - sys_time_get_timebase_frequency returns 79.8 MHz (PS_TIMEBASE_HZ, the real Cell timebase and what mftb counts
//   at here, pspocket/core/timebase.h); RPCS3 returns 80 MHz.
// - Guest system time (sys_time_get_system_time, timer expiry) is the timebase converted to microseconds, so it
//   starts near 0 at process start (as RPCS3's guest time does) and agrees with mftb. No clock scaling.
// - sys_timer_usleep has no configurable addend (RPCS3 usleep_addend).
// - sys_time_get_rtc is not implemented (RPCS3 leaves it a todo that returns CELL_OK without writing).

#include <ctime>

#include "internal.hpp"
#include "pspocket/core/timebase.h"

namespace pspocket::lv2 {

namespace {

u64 process_pid() { return 1; }

u64 add_saturate(u64 a, u64 b) { return a + b < a ? ~0ull : a + b; }

bool queue_alive(const std::shared_ptr<EventQueue>& q)
{
    if (!q) return false;
    std::lock_guard lk(q->lock);
    return !q->dead;
}

} // namespace

u64 guest_system_time_us()
{
    const unsigned __int128 ticks = ps_timebase_now();
    return static_cast<u64>(ticks * 1'000'000u / PS_TIMEBASE_HZ);
}

// ------------------------------------------------------------------------------------------------- Timer

u64 Timer::check_locked(u64 now)
{
    // RPCS3 lv2_timer::check_unlocked plus the timer thread's loop: every missed period fires once.
    while (running && now >= expire) {
        if (queue) queue->send(Event{source, data1, data2, expire});
        if (period) {
            expire = add_saturate(expire, period);
        } else {
            running = false;
        }
    }
    return running ? expire : ~0ull;
}

TimerService::~TimerService() { stop(); }

void TimerService::add(std::shared_ptr<Timer> timer)
{
    std::lock_guard lock(mutex_);
    timers_.push_back(std::move(timer));
}

void TimerService::remove(const Timer* timer)
{
    std::lock_guard lock(mutex_);
    std::erase_if(timers_, [&](const std::shared_ptr<Timer>& t) { return t.get() == timer; });
}

void TimerService::kick()
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        kicked_ = true;
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }
    cv_.notify_all();
}

void TimerService::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TimerService::run()
{
#if defined(__APPLE__)
    pthread_setname_np("lv2 timers");
#endif
    for (;;) {
        std::vector<std::shared_ptr<Timer>> snapshot;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            snapshot = timers_;
            kicked_ = false;
        }
        // Timer locks are taken without the service lock (start/stop take a timer lock, then kick()).
        const u64 now = guest_system_time_us();
        u64 next = ~0ull;
        for (const auto& timer : snapshot) {
            std::lock_guard tl(timer->lock);
            if (!timer->dead) next = std::min(next, timer->check_locked(now));
        }
        std::unique_lock lock(mutex_);
        const auto ready = [&] { return stopping_ || kicked_; };
        if (next == ~0ull) {
            cv_.wait(lock, ready);
        } else {
            const u64 cur = guest_system_time_us();
            if (next > cur) cv_.wait_for(lock, std::chrono::microseconds(next - cur), ready);
        }
    }
}

// ------------------------------------------------------------------------------------------------- syscalls

Result sys_timer_create(PpuThread& t, u32 timer_id)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(timer_id, 4)) return err(CELL_EFAULT);
    auto timer = std::make_shared<Timer>();
    timer->refs = 1;
    const u32 id = s.objects.insert(Timer::kIdBase, timer);
    if (!id) return err(CELL_EAGAIN);
    timer->id = id;
    s.timers.add(timer);
    s.guest.w32(timer_id, id);
    return CELL_OK;
}

Result sys_timer_destroy(PpuThread& t, u32 timer_id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto timer = s.objects.get<Timer>(timer_id);
    const Result r = destroy_object<Timer>(t.kernel, timer_id,
        [](Timer& tm) -> Result { return queue_alive(tm.queue) ? err(CELL_EISCONN) : CELL_OK; });
    if (r == CELL_OK && timer) s.timers.remove(timer.get());
    return r;
}

Result sys_timer_get_information(PpuThread& t, u32 timer_id, u32 info)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto timer = s.objects.get<Timer>(timer_id);
    if (!timer) return err(CELL_ESRCH);
    if (!s.guest.valid(info, 24)) return err(CELL_EFAULT);
    u64 next = 0, period = 0;
    u32 state = 0;
    {
        std::lock_guard lk(timer->lock);
        if (timer->dead) return err(CELL_ESRCH);
        timer->check_locked(guest_system_time_us());
        if (timer->running) {
            next = timer->expire;
            period = timer->period;
            state = 1; // SYS_TIMER_STATE_RUN
        }
    }
    // sys_timer_information_t: next_expire, period, timer_state, pad
    s.guest.w64(info, next);
    s.guest.w64(info + 8, period);
    s.guest.w32(info + 16, state);
    s.guest.w32(info + 20, 0);
    return CELL_OK;
}

Result _sys_timer_start(PpuThread& t, u32 timer_id, u64 base_time, u64 period)
{
    Kernel::Impl& s = t.kernel.impl();
    const u64 now = guest_system_time_us();
    if (period && period < 100) return err(CELL_EINVAL); // invalid periodic timer
    const auto timer = s.objects.get<Timer>(timer_id);
    if (!timer) return err(CELL_ESRCH);
    {
        std::lock_guard lk(timer->lock);
        if (timer->dead) return err(CELL_ESRCH);
        if (!timer->queue) return err(CELL_ENOTCONN); // lv2: a plain null check, not a liveness check
        timer->check_locked(now);
        if (timer->running) return err(CELL_EBUSY);
        if (!period && now >= base_time) return err(CELL_ETIMEDOUT); // one-shot in the past (not an error)
        u64 expire = 0;
        if (period == 0) {
            expire = base_time;
        } else if (base_time == 0) {
            expire = add_saturate(now, period);
        } else if (now < add_saturate(base_time, period)) {
            expire = add_saturate(base_time, period);
        } else {
            // Base in the past: the first multiple of the period after now, in the base's phase (RPCS3).
            const u64 aligned = add_saturate(now - now % period, base_time % period);
            expire = add_saturate(aligned, aligned < now ? period : 0);
        }
        timer->expire = expire;
        timer->period = period;
        timer->running = true;
    }
    s.timers.kick();
    return CELL_OK;
}

Result sys_timer_stop(PpuThread& t, u32 timer_id)
{
    const auto timer = t.kernel.impl().objects.get<Timer>(timer_id);
    if (!timer) return err(CELL_ESRCH);
    std::lock_guard lk(timer->lock);
    if (timer->dead) return err(CELL_ESRCH);
    timer->check_locked(guest_system_time_us());
    timer->running = false;
    return CELL_OK;
}

Result sys_timer_connect_event_queue(PpuThread& t, u32 timer_id, u32 queue_id, u64 name, u64 data1, u64 data2)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto timer = s.objects.get<Timer>(timer_id);
    if (!timer) return err(CELL_ESRCH);
    const auto queue = s.objects.get<EventQueue>(queue_id);
    if (!queue) return err(CELL_ESRCH);
    std::lock_guard lk(timer->lock);
    if (timer->dead) return err(CELL_ESRCH);
    if (queue_alive(timer->queue)) return err(CELL_EISCONN);
    timer->queue = queue;
    timer->source = name ? name : (process_pid() << 32) | timer_id;
    timer->data1 = data1;
    timer->data2 = data2;
    return CELL_OK;
}

Result sys_timer_disconnect_event_queue(PpuThread& t, u32 timer_id)
{
    const auto timer = t.kernel.impl().objects.get<Timer>(timer_id);
    if (!timer) return err(CELL_ESRCH);
    std::lock_guard lk(timer->lock);
    if (timer->dead) return err(CELL_ESRCH);
    timer->check_locked(guest_system_time_us());
    timer->running = false;
    if (!queue_alive(timer->queue)) return err(CELL_ENOTCONN);
    timer->queue.reset();
    return CELL_OK;
}

Result sys_timer_usleep(PpuThread& t, u64 sleep_time)
{
    if (sleep_time == 0) {
        std::this_thread::yield(); // usleep(0): games use it as a yield (RPCS3)
        return CELL_OK;
    }
    if (!sleep_us(t.waiter, sleep_time)) throw ThreadExit{};
    return CELL_OK;
}

Result sys_timer_sleep(PpuThread& t, u32 sleep_time) { return sys_timer_usleep(t, u64{sleep_time} * 1'000'000u); }

Result sys_time_set_timezone(PpuThread& t, s32, s32)
{
    return t.kernel.config().root ? CELL_OK : err(CELL_ENOSYS); // RPCS3: root only, no effect
}

void host_time_zone(s32& timezone, s32& summertime)
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    summertime = local.tm_isdst > 0 ? 60 : 0;
    timezone = static_cast<s32>(local.tm_gmtoff / 60) - summertime; // minutes east of UTC
}

void host_current_time(s64& sec, s64& nsec)
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    sec = ts.tv_sec;
    nsec = ts.tv_nsec;
}

Result sys_time_get_timezone(PpuThread& t, u32 timezone, u32 summertime)
{
    Guest& g = t.kernel.impl().guest;
    if (!g.valid(timezone, 4) || !g.valid(summertime, 4)) return err(CELL_EFAULT);
    s32 tz = 0, dst = 0;
    host_time_zone(tz, dst);
    g.w32(timezone, static_cast<u32>(tz));
    g.w32(summertime, static_cast<u32>(dst));
    return CELL_OK;
}

Result sys_time_get_current_time(PpuThread& t, u32 sec, u32 nsec)
{
    Guest& g = t.kernel.impl().guest;
    if (!g.valid(sec, 8)) return err(CELL_EFAULT);
    s64 s = 0, ns = 0;
    host_current_time(s, ns);
    g.w64(sec, static_cast<u64>(s));
    if (!g.valid(nsec, 8)) return err(CELL_EFAULT);
    g.w64(nsec, static_cast<u64>(ns));
    return CELL_OK;
}

Result sys_time_set_current_time(PpuThread& t, s64, s64)
{
    return t.kernel.config().root ? CELL_OK : err(CELL_ENOSYS); // RPCS3: root only, no effect
}

u64 sys_time_get_timebase_frequency(PpuThread&) { return PS_TIMEBASE_HZ; }

u64 sys_time_get_system_time(PpuThread&) { return guest_system_time_us(); }

void register_time_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 70, sys_timer_create);
    PS_LV2_BIND(table, 71, sys_timer_destroy);
    PS_LV2_BIND(table, 72, sys_timer_get_information);
    PS_LV2_BIND(table, 73, _sys_timer_start);
    PS_LV2_BIND(table, 74, sys_timer_stop);
    PS_LV2_BIND(table, 75, sys_timer_connect_event_queue);
    PS_LV2_BIND(table, 76, sys_timer_disconnect_event_queue);
    PS_LV2_BIND(table, 141, sys_timer_usleep);
    PS_LV2_BIND(table, 142, sys_timer_sleep);
    PS_LV2_BIND(table, 143, sys_time_set_timezone);
    PS_LV2_BIND(table, 144, sys_time_get_timezone);
    PS_LV2_BIND(table, 145, sys_time_get_current_time);
    PS_LV2_BIND(table, 146, sys_time_set_current_time);
    PS_LV2_BIND(table, 147, sys_time_get_timebase_frequency);
    PS_LV2_BIND(table, 149, sys_time_get_system_time);
}

} // namespace pspocket::lv2
