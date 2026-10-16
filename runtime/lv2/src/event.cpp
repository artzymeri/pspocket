// sys_event_queue, sys_event_port, sys_event_flag.
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_event.h/.cpp, sys_event_flag.h/.cpp.
//
// Deviations: RPCS3 marks a port "busy" while a PPU sends to a lower-priority receiver (an artefact of its scheduler
// emulation, which makes sys_event_port_disconnect return EBUSY meanwhile); there is no such window here.
//
// The sender goes on first (D-999): a PPU thread woken by another PPU thread's event, or taking one that thread queued
// under 10 ms ago, lets the sender go on first; waker.cpp, which since D-1041 does the same for every wake.

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr u32 kEventSize = 32;     // sys_event_t: source, data1, data2, data3
constexpr u32 kMaxQueueSize = 127; // sys_event_queue_create size limit

u64 process_pid() { return 1; } // RPCS3 process_getpid()

} // namespace

// ------------------------------------------------------------------------------------------------- EventQueue

Result EventQueue::send(const Event& event, const WakerMark& sender)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ENOTCONN);
    if (Waiter* w = waiters.pop(protocol)) {
        w->data[0] = event.source;
        w->data[1] = event.data1;
        w->data[2] = event.data2;
        w->data[3] = event.data3;
        w->waker = sender;
        w->waker.kind = static_cast<u8>(WaitKind::EventQueue);
        wake(*w, CELL_OK);
        return CELL_OK;
    }
    if (events.size() < size) {
        events.push_back(event);
        senders.push_back(sender);
        return CELL_OK;
    }
    return err(CELL_EBUSY);
}

Result EventQueue::receive(Waiter& w, u64 timeout_us, Event& out, WakerMark* sender)
{
    std::unique_lock lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (!events.empty()) {
        out = events.front();
        events.pop_front();
        if (sender) *sender = senders.front();
        w.waker = senders.front(); // its sender goes on first (D-999, waker.cpp)
        senders.pop_front();
        return CELL_OK;
    }
    prepare_wait(w);
    waiters.push(&w);
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::EventQueue, id},
        [&](bool) { return waiters.remove(&w) ? CancelResult::Removed : CancelResult::Woken; });
    switch (ws) {
    case WaitStatus::Woken:
        if (w.result == CELL_OK) {
            out = Event{w.data[0], w.data[1], w.data[2], w.data[3]};
            if (sender) *sender = w.waker;
        }
        return w.result; // CELL_OK or CELL_ECANCELED
    case WaitStatus::TimedOut: return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

usize EventQueue::try_receive(Event* out, usize max)
{
    std::lock_guard lk(lock);
    usize n = 0;
    while (n < max && !events.empty()) {
        out[n++] = events.front();
        events.pop_front();
        senders.pop_front();
    }
    return n;
}

void EventQueue::drain()
{
    std::lock_guard lk(lock);
    events.clear();
    senders.clear();
}

Result EventQueue::destroy(bool force)
{
    std::lock_guard lk(lock);
    if (dead) return err(CELL_ESRCH);
    if (!force && !waiters.empty()) return err(CELL_EBUSY);
    for (Waiter* w : waiters.take_all(SYS_SYNC_FIFO)) wake(*w, err(CELL_ECANCELED));
    if (!events.empty()) PS_LOG_WARN(kChan, "event queue {:#x} destroyed with {} unread event(s)", id, events.size());
    events.clear();
    senders.clear();
    dead = true;
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- EventFlag

bool EventFlag::valid_mode(u32 mode)
{
    switch (mode & 0xF) {
    case SYS_EVENT_FLAG_WAIT_AND:
    case SYS_EVENT_FLAG_WAIT_OR: break;
    default: return false;
    }
    switch (mode & ~0xFu) {
    case 0:
    case SYS_EVENT_FLAG_WAIT_CLEAR:
    case SYS_EVENT_FLAG_WAIT_CLEAR_ALL: break;
    default: return false;
    }
    return true;
}

bool EventFlag::check_pattern(u64& pat, u64 bitptn, u32 mode, u64* result)
{
    // RPCS3 lv2_event_flag::check_pattern.
    if (result) *result = pat;
    if (((mode & 0xF) == SYS_EVENT_FLAG_WAIT_AND && (pat & bitptn) != bitptn) ||
        ((mode & 0xF) == SYS_EVENT_FLAG_WAIT_OR && (pat & bitptn) == 0)) {
        return false;
    }
    if ((mode & ~0xFu) == SYS_EVENT_FLAG_WAIT_CLEAR) {
        pat &= ~bitptn;
    } else if ((mode & ~0xFu) == SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        pat = 0;
    }
    return true;
}

Result EventFlag::wait(Waiter& w, u64 bitptn, u32 mode, u64 timeout_us, u64& result)
{
    std::unique_lock lk(lock);
    result = 0;
    if (dead) return err(CELL_ESRCH);
    if (check_pattern(pattern, bitptn, mode, &result)) return CELL_OK;
    result = 0;
    if (type == SYS_SYNC_WAITER_SINGLE && !waiters.empty()) return err(CELL_EPERM);
    w.want[0] = bitptn;
    w.want[1] = mode;
    prepare_wait(w);
    waiters.push(&w);
    u64 timeout_pattern = 0;
    const WaitStatus ws = park(w, lk, timeout_us, {WaitKind::EventFlag, id}, [&](bool) {
        if (!waiters.remove(&w)) return CancelResult::Woken;
        timeout_pattern = pattern;
        return CancelResult::Removed;
    });
    switch (ws) {
    case WaitStatus::Woken:
        result = w.data[0];
        return w.result; // CELL_OK, or CELL_ECANCELED from sys_event_flag_cancel
    case WaitStatus::TimedOut: result = timeout_pattern; return err(CELL_ETIMEDOUT);
    case WaitStatus::Aborted: break;
    }
    return err(CELL_EABORT);
}

Result EventFlag::try_wait(u64 bitptn, u32 mode, u64& result)
{
    std::lock_guard lk(lock);
    result = 0;
    if (dead) return err(CELL_ESRCH);
    u64 res = 0;
    if (!check_pattern(pattern, bitptn, mode, &res)) return err(CELL_EBUSY);
    result = res;
    return CELL_OK;
}

void EventFlag::set(u64 bitptn)
{
    std::lock_guard lk(lock);
    if (dead) return;
    // RPCS3 sys_event_flag_set: waiters are tried in protocol order against the running pattern, each satisfied
    // waiter's clear mode applies before the next is tested, and the scan stops once the pattern is empty. The
    // pattern only changes under `lock` here, so RPCS3's retry loop for concurrent updates isn't needed.
    u64 to_write = pattern | bitptn;
    std::vector<Waiter*> order(waiters.items().begin(), waiters.items().end());
    if (protocol == SYS_SYNC_PRIORITY) {
        std::stable_sort(order.begin(), order.end(),
            [](const Waiter* a, const Waiter* b) { return a->prio.load(std::memory_order_relaxed) < b->prio.load(std::memory_order_relaxed); });
    }
    std::vector<Waiter*> satisfied;
    for (Waiter* w : order) {
        u64 res = 0;
        if (check_pattern(to_write, w->want[0], static_cast<u32>(w->want[1]), &res)) {
            w->data[0] = res;
            satisfied.push_back(w);
            if (!to_write) break;
        }
    }
    pattern = to_write;
    for (Waiter* w : satisfied) {
        waiters.remove(w);
        wake(*w, CELL_OK);
    }
}

void EventFlag::clear(u64 bitptn)
{
    std::lock_guard lk(lock);
    pattern &= bitptn;
}

u32 EventFlag::cancel()
{
    std::lock_guard lk(lock);
    if (dead) return 0;
    u32 n = 0;
    for (Waiter* w : waiters.take_all(SYS_SYNC_FIFO)) {
        w->data[0] = pattern;
        wake(*w, err(CELL_ECANCELED));
        n++;
    }
    return n;
}

u64 EventFlag::get()
{
    std::lock_guard lk(lock);
    return pattern;
}

// ------------------------------------------------------------------------------------------------- syscalls

namespace {

bool queue_alive(const std::shared_ptr<EventQueue>& q)
{
    if (!q) return false;
    std::lock_guard lk(q->lock);
    return !q->dead;
}

} // namespace

Result sys_event_queue_create(PpuThread& t, u32 equeue_id, u32 attr, u64 event_queue_key, s32 size)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    if (size <= 0 || size > static_cast<s32>(kMaxQueueSize)) return err(CELL_EINVAL);
    if (!g.valid(equeue_id, 4) || !g.valid(attr, 16)) return err(CELL_EFAULT);
    // sys_event_queue_attribute_t: protocol, type, name[8]
    const u32 protocol = g.r32(attr);
    const u32 type = g.r32(attr + 4);
    const u64 name = g.r64(attr + 8);
    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY) {
        PS_LOG_ERROR(kChan, "sys_event_queue_create: unknown protocol {:#x}", protocol);
        return err(CELL_EINVAL);
    }
    if (type != SYS_PPU_QUEUE && type != SYS_SPU_QUEUE) {
        PS_LOG_ERROR(kChan, "sys_event_queue_create: unknown type {:#x}", type);
        return err(CELL_EINVAL);
    }
    const u32 pshared = event_queue_key == 0 ? SYS_SYNC_NOT_PROCESS_SHARED : SYS_SYNC_PROCESS_SHARED;
    u32 id = 0;
    const Result r = create_object<EventQueue>(k, pshared, event_queue_key, SYS_SYNC_NEWLY_CREATED,
        [&] { return std::make_shared<EventQueue>(protocol, type, static_cast<u32>(size), name, event_queue_key); }, id);
    if (r != CELL_OK) return r;
    g.w32(equeue_id, id);
    return CELL_OK;
}

Result sys_event_queue_destroy(PpuThread& t, u32 equeue_id, s32 mode)
{
    sender_barrier(t); // D-999
    if (mode != 0 && mode != SYS_EVENT_QUEUE_DESTROY_FORCE) return err(CELL_EINVAL);
    return destroy_object<EventQueue>(t.kernel, equeue_id, [&](EventQueue& q) -> Result {
        if (mode == 0 && !q.waiters.empty()) return err(CELL_EBUSY);
        for (Waiter* w : q.waiters.take_all(SYS_SYNC_FIFO)) wake(*w, err(CELL_ECANCELED));
        if (!q.events.empty()) PS_LOG_WARN(kChan, "event queue {:#x} destroyed with {} unread event(s)", equeue_id, q.events.size());
        q.events.clear();
        return CELL_OK;
    });
}

Result sys_event_queue_receive(PpuThread& t, u32 equeue_id, u32, u64 timeout)
{
    const auto q = t.kernel.impl().objects.get<EventQueue>(equeue_id);
    if (!q) return err(CELL_ESRCH);
    if (q->type != SYS_PPU_QUEUE) return err(CELL_EINVAL);
    Event ev;
    const Result r = finish(t, q->receive(t.waiter, timeout, ev));
    if (r == CELL_OK) { // the sender goes on first as the call returns (D-999, return_point)
        // lv2 returns the event in r4..r7; the liblv2 wrapper stores them to its sys_event_t.
        t.ctx->gpr[4] = ev.source;
        t.ctx->gpr[5] = ev.data1;
        t.ctx->gpr[6] = ev.data2;
        t.ctx->gpr[7] = ev.data3;
    }
    return r;
}

Result sys_event_queue_tryreceive(PpuThread& t, u32 equeue_id, u32 event_array, s32 size, u32 number)
{
    Guest& g = t.kernel.impl().guest;
    const auto q = t.kernel.impl().objects.get<EventQueue>(equeue_id);
    if (!q) return err(CELL_ESRCH);
    if (q->type != SYS_PPU_QUEUE) return err(CELL_EINVAL);
    const u32 max = static_cast<u32>(std::clamp<s32>(size, 0, static_cast<s32>(kMaxQueueSize)));
    if (!g.valid(number, 4) || (max && !g.valid(event_array, u64{max} * kEventSize))) return err(CELL_EFAULT);
    {
        std::lock_guard lk(q->lock);
        if (q->dead) return err(CELL_ESRCH);
    }
    Event events[kMaxQueueSize];
    const usize n = q->try_receive(events, max);
    for (usize i = 0; i < n; i++) {
        const u32 a = event_array + static_cast<u32>(i) * kEventSize;
        g.w64(a, events[i].source);
        g.w64(a + 8, events[i].data1);
        g.w64(a + 16, events[i].data2);
        g.w64(a + 24, events[i].data3);
    }
    g.w32(number, static_cast<u32>(n));
    return CELL_OK;
}

Result sys_event_queue_drain(PpuThread& t, u32 equeue_id)
{
    const auto q = t.kernel.impl().objects.get<EventQueue>(equeue_id);
    if (!q) return err(CELL_ESRCH);
    q->drain();
    return CELL_OK;
}

Result sys_event_port_create(PpuThread& t, u32 eport_id, s32 port_type, u64 name)
{
    Kernel::Impl& s = t.kernel.impl();
    if (port_type != SYS_EVENT_PORT_LOCAL && port_type != SYS_EVENT_PORT_IPC) {
        PS_LOG_ERROR(kChan, "sys_event_port_create: unknown port type {}", port_type);
        return err(CELL_EINVAL);
    }
    if (!s.guest.valid(eport_id, 4)) return err(CELL_EFAULT);
    auto port = std::make_shared<EventPort>(port_type, name);
    port->refs = 1;
    const u32 id = s.objects.insert(EventPort::kIdBase, port);
    if (!id) return err(CELL_EAGAIN);
    port->id = id;
    s.guest.w32(eport_id, id);
    return CELL_OK;
}

Result sys_event_port_destroy(PpuThread& t, u32 eport_id)
{
    return destroy_object<EventPort>(t.kernel, eport_id,
        [](EventPort& p) -> Result { return queue_alive(p.queue) ? err(CELL_EISCONN) : CELL_OK; });
}

static Result connect_port(Kernel& k, u32 eport_id, s32 required_type, std::shared_ptr<EventQueue> queue)
{
    const auto port = k.impl().objects.get<EventPort>(eport_id);
    if (!port || !queue) return err(CELL_ESRCH);
    std::lock_guard lk(port->lock);
    if (port->dead) return err(CELL_ESRCH);
    if (port->type != required_type) return err(CELL_EINVAL);
    if (queue_alive(port->queue)) return err(CELL_EISCONN);
    port->queue = std::move(queue);
    return CELL_OK;
}

Result sys_event_port_connect_local(PpuThread& t, u32 eport_id, u32 equeue_id)
{
    return connect_port(t.kernel, eport_id, SYS_EVENT_PORT_LOCAL, t.kernel.impl().objects.get<EventQueue>(equeue_id));
}

Result sys_event_port_connect_ipc(PpuThread& t, u32 eport_id, u64 ipc_key)
{
    if (ipc_key == 0) return err(CELL_EINVAL);
    Kernel::Impl& s = t.kernel.impl();
    std::shared_ptr<EventQueue> queue;
    {
        std::lock_guard lk(s.ipc_mutex);
        if (auto it = s.ipc.find({EventQueue::kIdBase, ipc_key}); it != s.ipc.end()) {
            queue = std::static_pointer_cast<EventQueue>(it->second.lock());
        }
    }
    return connect_port(t.kernel, eport_id, SYS_EVENT_PORT_IPC, std::move(queue));
}

Result sys_event_port_disconnect(PpuThread& t, u32 eport_id)
{
    const auto port = t.kernel.impl().objects.get<EventPort>(eport_id);
    if (!port) return err(CELL_ESRCH);
    std::lock_guard lk(port->lock);
    if (port->dead) return err(CELL_ESRCH);
    if (!queue_alive(port->queue)) return err(CELL_ENOTCONN);
    port->queue.reset();
    return CELL_OK;
}

Result sys_event_port_send(PpuThread& t, u32 eport_id, u64 data1, u64 data2, u64 data3)
{
    const auto port = t.kernel.impl().objects.get<EventPort>(eport_id);
    if (!port) return err(CELL_ESRCH);
    std::shared_ptr<EventQueue> queue;
    {
        std::lock_guard lk(port->lock);
        if (port->dead) return err(CELL_ESRCH);
        queue = port->queue;
    }
    if (!queue) return err(CELL_ENOTCONN);
    const u64 source = port->name ? port->name : (process_pid() << 32) | eport_id;
    const Result r = queue->send(Event{source, data1, data2, data3}, waker_mark(t, static_cast<u8>(WaitKind::None)));
    static const u32 delay_us = [] {
        const char* v = std::getenv("PSPOCKET_LV2_SEND_DELAY_US");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (delay_us) [[unlikely]] std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    return r;
}

Result sys_event_flag_create(PpuThread& t, u32 id, u32 attr, u64 init)
{
    Kernel& k = t.kernel;
    Guest& g = k.impl().guest;
    if (!g.valid(id, 4) || !g.valid(attr, 32)) return err(CELL_EFAULT);
    // sys_event_flag_attribute_t: protocol, pshared, ipc_key (u64), flags, type, name[8]
    const u32 protocol = g.r32(attr);
    const u32 pshared = g.r32(attr + 4);
    const u64 ipc_key = g.r64(attr + 8);
    const s32 flags = static_cast<s32>(g.r32(attr + 16));
    const u32 type = g.r32(attr + 20);
    const u64 name = g.r64(attr + 24);
    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY) {
        PS_LOG_ERROR(kChan, "sys_event_flag_create: unknown protocol {:#x}", protocol);
        return err(CELL_EINVAL);
    }
    if (type != SYS_SYNC_WAITER_SINGLE && type != SYS_SYNC_WAITER_MULTIPLE) {
        PS_LOG_ERROR(kChan, "sys_event_flag_create: unknown type {:#x}", type);
        return err(CELL_EINVAL);
    }
    u32 out = 0;
    const Result r = create_object<EventFlag>(k, pshared, pshared == SYS_SYNC_PROCESS_SHARED ? ipc_key : 0, flags,
        [&] { return std::make_shared<EventFlag>(protocol, type, init, name); }, out);
    if (r != CELL_OK) return r;
    g.w32(id, out);
    return CELL_OK;
}

Result sys_event_flag_destroy(PpuThread& t, u32 id)
{
    return destroy_object<EventFlag>(t.kernel, id,
        [](EventFlag& f) -> Result { return f.waiters.empty() ? CELL_OK : err(CELL_EBUSY); });
}

Result sys_event_flag_wait(PpuThread& t, u32 id, u64 bitptn, u32 mode, u32 result, u64 timeout)
{
    // The result pointer is written on every return (RPCS3 sys_event_store_result), 0 unless a pattern applies.
    Guest& g = t.kernel.impl().guest;
    u64 pattern = 0;
    Result r = CELL_OK;
    if (!EventFlag::valid_mode(mode)) {
        PS_LOG_ERROR(kChan, "sys_event_flag_wait: unknown mode {:#x}", mode);
        r = err(CELL_EINVAL);
    } else if (const auto f = t.kernel.impl().objects.get<EventFlag>(id)) {
        r = f->wait(t.waiter, bitptn, mode, timeout, pattern);
    } else {
        r = err(CELL_ESRCH);
    }
    if (result) g.put64(result, pattern);
    return finish(t, r);
}

Result sys_event_flag_trywait(PpuThread& t, u32 id, u64 bitptn, u32 mode, u32 result)
{
    Guest& g = t.kernel.impl().guest;
    u64 pattern = 0;
    Result r = CELL_OK;
    if (!EventFlag::valid_mode(mode)) {
        PS_LOG_ERROR(kChan, "sys_event_flag_trywait: unknown mode {:#x}", mode);
        r = err(CELL_EINVAL);
    } else if (const auto f = t.kernel.impl().objects.get<EventFlag>(id)) {
        r = f->try_wait(bitptn, mode, pattern);
    } else {
        r = err(CELL_ESRCH);
    }
    if (result) g.put64(result, pattern);
    return r;
}

Result sys_event_flag_set(PpuThread& t, u32 id, u64 bitptn)
{
    const auto f = t.kernel.impl().objects.get<EventFlag>(id);
    if (!f) return err(CELL_ESRCH);
    f->set(bitptn);
    return CELL_OK;
}

Result sys_event_flag_clear(PpuThread& t, u32 id, u64 bitptn)
{
    const auto f = t.kernel.impl().objects.get<EventFlag>(id);
    if (!f) return err(CELL_ESRCH);
    f->clear(bitptn);
    return CELL_OK;
}

Result sys_event_flag_cancel(PpuThread& t, u32 id, u32 num)
{
    Guest& g = t.kernel.impl().guest;
    if (num) g.put32(num, 0);
    const auto f = t.kernel.impl().objects.get<EventFlag>(id);
    if (!f) return err(CELL_ESRCH);
    const u32 n = f->cancel();
    if (num) g.put32(num, n);
    return CELL_OK;
}

Result sys_event_flag_get(PpuThread& t, u32 id, u32 flags)
{
    Guest& g = t.kernel.impl().guest;
    const auto f = t.kernel.impl().objects.get<EventFlag>(id);
    if (!f) {
        if (flags) g.put64(flags, 0);
        return err(CELL_ESRCH);
    }
    if (!g.valid(flags, 8)) return err(CELL_EFAULT);
    g.w64(flags, f->get());
    return CELL_OK;
}

void register_event_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 82, sys_event_flag_create);
    PS_LV2_BIND(table, 83, sys_event_flag_destroy);
    PS_LV2_BIND(table, 85, sys_event_flag_wait);
    PS_LV2_BIND(table, 86, sys_event_flag_trywait);
    PS_LV2_BIND(table, 87, sys_event_flag_set);
    PS_LV2_BIND(table, 118, sys_event_flag_clear);
    PS_LV2_BIND(table, 128, sys_event_queue_create);
    PS_LV2_BIND(table, 129, sys_event_queue_destroy);
    PS_LV2_BIND(table, 130, sys_event_queue_receive);
    PS_LV2_BIND(table, 131, sys_event_queue_tryreceive);
    PS_LV2_BIND(table, 132, sys_event_flag_cancel);
    PS_LV2_BIND(table, 133, sys_event_queue_drain);
    PS_LV2_BIND(table, 134, sys_event_port_create);
    PS_LV2_BIND(table, 135, sys_event_port_destroy);
    PS_LV2_BIND(table, 136, sys_event_port_connect_local);
    PS_LV2_BIND(table, 137, sys_event_port_disconnect);
    PS_LV2_BIND(table, 138, sys_event_port_send);
    PS_LV2_BIND(table, 139, sys_event_flag_get);
    PS_LV2_BIND(table, 140, sys_event_port_connect_ipc);
}

} // namespace pspocket::lv2
