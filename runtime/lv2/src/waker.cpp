// The waker goes on first (D-999 for event queues, D-1041 for every wake and for thread start).
//
// On the PS3, lv2's scheduler leaves a PPU thread that wakes another one running: the woken thread, of the same or a
// lower priority, runs after it, and games count on what the waker does next. Assassin's Creed's worker threads
// signal "done" through an event queue, then mark their object idle, take it out of the game's thread registry and
// exit; the thread woken by "done" starts the object again only if it is idle, or tears it down (D-999). The same
// holds for every other way one PPU thread wakes another: a mutex or lwmutex handed over at unlock, a condition
// variable or lwcond signalled, a semaphore posted, an rwlock released, an event flag set, a thread's exit waking its
// joiner; and a new thread that its creator starts. Here every PPU thread is a host thread: a waker the host does
// not run for a while (another thread takes its core) lets the woken thread get there first. So:
// - wake() notes the PPU thread that wakes (Waiter::waker; only when a waiter is woken: an uncontended lwmutex never
//   enters the kernel and costs nothing), and a thread starting another notes itself on the new one;
// - the woken thread, if it is not more urgent than its waker (a more urgent one preempts it on the PS3), goes back to
//   guest code (return_point: the end of its lv2 call or library function; a new thread before its first guest
//   instruction) once the waker has begun its next call into lv2 or a library, waits in lv2 (blocks or sleeps; not
//   for a lock), or is gone, or 10 ms have passed since the wake (defer_to_waker);
// - before that thread destroys an event queue or creates a thread, under 100 ms after, the waker goes on until it
//   waits in lv2 or is gone, 20 ms at most (sender_barrier): what it does after its next call is done by then.
// PSPOCKET_LV2_SENDER_FIRST=0 turns it all off, =events keeps D-999's event queues only (A/B measurements).
// PSPOCKET_LV2_WAKER_LIGHT names the kinds held only until the waker is back from its waking call (light_kinds).
// PSPOCKET_LV2_SEND_DELAY_US=N (debugging) has every sys_event_port_send sleep N us before it returns, and
// PSPOCKET_LV2_WAKE_DELAY_US=N every lv2 call or library function that woke a PPU thread: the races these rules
// close show then.

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "internal.hpp"
#include "pspocket/core/chaos.hpp"

namespace pspocket::lv2 {

namespace {

constexpr auto kDeferLimit = std::chrono::milliseconds(10);
constexpr auto kSpinFor = std::chrono::microseconds(50); // yielding at first, then sleeping

// The calling host thread, for host_thread_alive.
u64 host_thread_self()
{
#if defined(__APPLE__)
    return pthread_mach_thread_np(pthread_self());
#else
    clockid_t clock{};
    return pthread_getcpuclockid(pthread_self(), &clock) == 0 ? static_cast<u64>(clock) : ~u64{0};
#endif
}

// Whether a host thread is still there.
bool host_thread_alive(u64 thread)
{
#if defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    return thread_info(static_cast<thread_act_t>(thread), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info),
               &count) == KERN_SUCCESS;
#else
    timespec ts{};
    return thread != ~u64{0} && clock_gettime(static_cast<clockid_t>(thread), &ts) == 0;
#endif
}

std::atomic<u64> g_deferred{0}, g_by_wait{0}, g_by_calls{0}, g_by_limit{0}, g_deferred_ns{0};
std::atomic<u64> g_barriers{0}, g_barrier_limit{0}, g_barrier_ns{0};
std::array<std::atomic<u64>, kWakeKinds> g_kind_deferred{}, g_kind_ns{};

// Its next call into lv2 or a library has begun: the call that woke counts its return, the next one its beginning.
// A light kind (light_kinds) waits for the return only.
constexpr u64 kSenderNextCall = 2, kSenderReturned = 1;

enum class SenderState { Turn, Waits, NextCall, Gone };

// Where the waker of `m` is: still in its turn, waiting in lv2 (for anything but a lock: a lock it waits for is part
// of its turn, as the worker's registry lwmutex above), past its next call's beginning (or, `calls` 1, back from the
// call that woke), or gone.
SenderState sender_state(Kernel& k, const WakerMark& m, u64 calls = kSenderNextCall)
{
    const std::shared_ptr<PpuThread> sender = k.thread(m.thread);
    if (!sender || !host_thread_alive(m.host_thread)) return SenderState::Gone;
    const WaitKind waits = sender->waiter.waiting_on().kind;
    if (waits != WaitKind::None && waits != WaitKind::LwMutex && waits != WaitKind::Mutex) return SenderState::Waits;
    if (sender->lv2_calls.load(std::memory_order_acquire) - m.calls >= calls) return SenderState::NextCall;
    return SenderState::Turn;
}

// The kinds whose woken thread waits only until its waker is back from the call that woke it (PSPOCKET_LV2_WAKER_LIGHT,
// names as wake_kind_name gives them, comma apart; "none" for none): by default the semaphore, whose posts GTA V makes
// 220 times a second, each followed by long stretches of guest code (D-1041: the full rule cost its gate walk 11 s
// of waits and its frame rate).
u32 light_kinds()
{
    static const u32 mask = [] {
        const char* v = std::getenv("PSPOCKET_LV2_WAKER_LIGHT");
        const std::string_view list = v ? std::string_view(v) : std::string_view("semaphore");
        u32 m = 0;
        for (usize k = 0; k < kWakeKinds; k++) {
            const std::string_view name = wake_kind_name(k);
            for (usize at = 0; at <= list.size();) {
                const usize end = std::min(list.find(',', at), list.size());
                if (list.substr(at, end - at) == name) m |= 1u << k;
                at = end + 1;
            }
        }
        return m;
    }();
    return mask;
}

WakerFirst read_mode()
{
    const char* v = std::getenv("PSPOCKET_LV2_SENDER_FIRST");
    if (!v) return WakerFirst::All;
    const std::string_view s(v);
    if (s == "0" || s == "off") return WakerFirst::Off;
    if (s == "events") return WakerFirst::Events;
    return WakerFirst::All;
}

u32 wake_delay_us()
{
    static const u32 us = [] {
        const char* v = std::getenv("PSPOCKET_LV2_WAKE_DELAY_US");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return us;
}

// Waits until `done(state)`, or `limit` has passed since `from`; yields at first, then sleeps 20 us at a time.
// Returns whether it ended by `done`.
template <class Done>
bool wait_for_sender(Kernel& k, const WakerMark& m, std::chrono::steady_clock::time_point from,
    std::chrono::steady_clock::duration limit, u64 calls, Done&& done)
{
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (done(sender_state(k, m, calls))) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - from >= limit) return false;
        if (now - start < kSpinFor) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}

} // namespace

WakerFirst waker_first()
{
    static const WakerFirst mode = read_mode();
    return mode;
}

WakerMark waker_mark(PpuThread& t, u8 kind)
{
    WakerMark m;
    m.thread = t.id;
    m.prio = t.priority();
    m.calls = t.lv2_calls.load(std::memory_order_relaxed);
    m.host_thread = host_thread_self();
    m.at = std::chrono::steady_clock::now();
    m.kind = kind;
    return m;
}

void note_waker(Waiter& w)
{
    PpuThread* c = current_thread();
    if (!c || c->id == w.id) return; // not a PPU thread (an SPU, a timer, the RSX), or itself
    c->woke = true;
    if (waker_first() != WakerFirst::All) return;
    w.waker = waker_mark(*c, static_cast<u8>(w.waiting_on().kind));
}

void note_starter(PpuThread& creator, PpuThread& started)
{
    if (waker_first() != WakerFirst::All || creator.id == started.id) return;
    started.waiter.waker = waker_mark(creator, kWakeStart);
}

// Holds `t` until the PPU thread that last woke it (t.waiter.waker) is past its next call's beginning, waits or is
// gone (the top of the file); notes that thread for sender_barrier.
void defer_to_waker(PpuThread& t)
{
    const WakerMark m = t.waiter.waker;
    t.waiter.waker = {};
    if (waker_first() == WakerFirst::Off || !m.thread || m.thread == t.id) return; // none or itself
    // Shake mode's S16 (host-adversarial, D-1045): the woken thread overtakes its waker.
    if (chaos::on(chaos::S16_WakeOvertake) && chaos::hit(chaos::S16_WakeOvertake, m.thread)) [[unlikely]] return;
    const bool light = (light_kinds() >> std::min<usize>(m.kind, 31)) & 1;
    if (!light) t.woken_by = m; // a light kind's waker is not waited for at a teardown either
    if (t.priority() < m.prio) return; // a more urgent thread preempts its waker on the PS3
    const auto start = std::chrono::steady_clock::now();
    if (start - m.at >= kDeferLimit) return;
    SenderState last = SenderState::Turn;
    const u64 calls = light ? kSenderReturned : kSenderNextCall;
    const bool ended = wait_for_sender(t.kernel, m, m.at, kDeferLimit, calls, [&](SenderState st) {
        last = st;
        return st != SenderState::Turn;
    });
    const u64 waited = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    g_deferred.fetch_add(1, std::memory_order_relaxed);
    (!ended ? g_by_limit : last == SenderState::NextCall ? g_by_calls : g_by_wait).fetch_add(1, std::memory_order_relaxed);
    g_deferred_ns.fetch_add(waited, std::memory_order_relaxed);
    const usize kind = std::min<usize>(m.kind, kWakeKinds - 1);
    g_kind_deferred[kind].fetch_add(1, std::memory_order_relaxed);
    g_kind_ns[kind].fetch_add(waited, std::memory_order_relaxed);
}

void woke_delay(PpuThread& t)
{
    t.woke = false;
    if (const u32 us = wake_delay_us()) [[unlikely]] std::this_thread::sleep_for(std::chrono::microseconds(us));
    if (chaos::on(chaos::S3_AfterWake)) [[unlikely]] chaos::delay(chaos::S3_AfterWake, t.id); // shake mode (D-1045)
}

// Before thread `t` destroys an event queue or creates a thread, the PPU thread that last woke it, if that was under
// 100 ms ago, finishes what it does next: until it waits in lv2 (not for a lock) or is gone, 20 ms at most. A thread
// that signalled "done" may still be taking its object out of the game's registry, and the object is what is torn
// down or started again (the top of the file).
void sender_barrier(PpuThread& t)
{
    const WakerMark m = t.woken_by;
    if (waker_first() == WakerFirst::Off || !m.thread) return;
    const auto start = std::chrono::steady_clock::now();
    if (start - m.at >= std::chrono::milliseconds(100)) return;
    t.woken_by = {};
    const auto finished = [](SenderState st) { return st == SenderState::Waits || st == SenderState::Gone; };
    if (finished(sender_state(t.kernel, m))) return;
    const bool ended = wait_for_sender(t.kernel, m, start, std::chrono::milliseconds(20), kSenderNextCall, finished);
    g_barriers.fetch_add(1, std::memory_order_relaxed);
    if (!ended) g_barrier_limit.fetch_add(1, std::memory_order_relaxed);
    g_barrier_ns.fetch_add(static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
        std::memory_order_relaxed);
}

SenderDeferrals sender_deferrals()
{
    SenderDeferrals d{g_deferred.load(std::memory_order_relaxed), g_by_wait.load(std::memory_order_relaxed),
        g_by_calls.load(std::memory_order_relaxed), g_by_limit.load(std::memory_order_relaxed),
        g_deferred_ns.load(std::memory_order_relaxed), g_barriers.load(std::memory_order_relaxed),
        g_barrier_limit.load(std::memory_order_relaxed), g_barrier_ns.load(std::memory_order_relaxed), {}, {}};
    for (usize i = 0; i < kWakeKinds; i++) {
        d.kind_deferred[i] = g_kind_deferred[i].load(std::memory_order_relaxed);
        d.kind_ns[i] = g_kind_ns[i].load(std::memory_order_relaxed);
    }
    return d;
}

const char* wake_kind_name(usize kind)
{
    if (kind == kWakeStart) return "thread start";
    if (kind < kWakeStart) {
        if (kind == static_cast<usize>(WaitKind::None)) return "queued event";
        return wait_kind_name(static_cast<WaitKind>(kind));
    }
    return "?";
}

} // namespace pspocket::lv2
