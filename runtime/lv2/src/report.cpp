// Thread and wait reports (report.hpp): each PPU thread's state, what it waits on, its guest backtrace, and the RSX's
// FIFO. Built for bring-up: a run that stops making progress ends with this report instead of a silent timeout.
//
// Locking: a snapshot takes one lock at a time (a thread's lock, the join lock, one object's lock, then the memory
// manager's lock for the stack walks), and looks objects up by ID only while it holds none, as everywhere in lv2.
// The memory manager's lock keeps thread stacks from being freed (thread_finish) while they are walked.

#include "pspocket/lv2/report.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

using Clock = std::chrono::steady_clock;
using State = ThreadSnapshot::State;

// An object's name (the name[8] of its sys_*_attribute_t, read as a big-endian u64), quoted, or "" without one.
std::string quoted_name(u64 name)
{
    std::string s;
    for (int shift = 56; shift >= 0; shift -= 8) {
        const char c = static_cast<char>(name >> shift);
        if (c == 0) break;
        s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return s.empty() ? s : std::format(" \"{}\"", s);
}

std::string count(usize n, std::string_view what) { return std::format("{} {}{}", n, what, n == 1 ? "" : "s"); }

// Thread IDs as `0x1000002 "name"` (owners, joined threads).
struct ThreadNames {
    std::map<u32, std::string> names;

    std::string operator()(u32 id) const
    {
        if (id == 0) return "none";
        const auto it = names.find(id);
        return it == names.end() ? std::format("{:#x}", id) : std::format("{:#x} \"{}\"", id, it->second);
    }
};

std::string flag_mode(u64 mode)
{
    std::string s = (mode & 0xF) == SYS_EVENT_FLAG_WAIT_AND ? "AND" : "OR";
    if ((mode & ~u64{0xF}) == SYS_EVENT_FLAG_WAIT_CLEAR) s += ", clear";
    if ((mode & ~u64{0xF}) == SYS_EVENT_FLAG_WAIT_CLEAR_ALL) s += ", clear all";
    return s;
}

// The user-space half of an lwmutex: the guest's sys_lwmutex_t {owner, waiter, attribute, recursive_count,
// sleep_queue} (RPCS3 sys_lwmutex.h), which sysPrxForUser changes with atomics.
std::string lwmutex_user_side(const Guest& g, u32 control, const ThreadNames& thread)
{
    if (!g.valid(control, 8)) return std::format("sys_lwmutex_t {:#010x} not readable", control);
    const u32 owner = g.r32(control);
    std::string who;
    switch (owner) {
    case 0xFFFFFFFF: who = "free"; break;
    case 0xFFFFFFFE: who = "destroyed"; break;
    case 0xFFFFFFFD: who = "handed to a woken waiter"; break;
    default: who = thread(owner); break;
    }
    return std::format("sys_lwmutex_t {:#010x}: owner {}, waiter count {}", control, who, g.r32(control + 4));
}

// What a blocked thread waits on, with the object's state. ` (being woken)`: the waiter has just left the queue.
std::string describe_wait(Kernel::Impl& s, const Waiter& w, WaitOn on, const ThreadNames& thread)
{
    const auto gone = [&] { return std::format("{} {:#x}, which no longer exists", wait_kind_name(on.kind), on.id); };
    const auto woken = [](bool queued) { return queued ? "" : " (being woken)"; };
    switch (on.kind) {
    case WaitKind::None:
    case WaitKind::Sleep: return {};
    case WaitKind::Mutex: {
        const auto m = s.objects.get<Mutex>(on.id);
        if (!m) return gone();
        std::lock_guard lk(m->lock);
        return std::format("mutex {:#x}{}: owner {}{}, {}{}", on.id, quoted_name(m->name), thread(m->owner),
            m->lock_count ? std::format(" (re-locked {} times)", m->lock_count) : "",
            count(m->waiters.size(), "waiter"), woken(m->waiters.contains(&w)));
    }
    case WaitKind::Cond: {
        const auto c = s.objects.get<Cond>(on.id);
        if (!c) return gone();
        std::lock_guard lk(c->mutex->lock); // a condition variable shares its mutex's lock
        if (!c->waiters.contains(&w) && c->mutex->waiters.contains(&w)) {
            // Signalled (or timed out) while another thread held the mutex.
            return std::format("cond {:#x}{}: woken, re-taking mutex {:#x}{} (owner {})", on.id, quoted_name(c->name),
                c->mutex_id, quoted_name(c->mutex->name), thread(c->mutex->owner));
        }
        return std::format("cond {:#x}{} with mutex {:#x}{} (owner {}): {}{}", on.id, quoted_name(c->name), c->mutex_id,
            quoted_name(c->mutex->name), thread(c->mutex->owner), count(c->waiters.size(), "waiter"),
            woken(c->waiters.contains(&w)));
    }
    case WaitKind::RwLockRead:
    case WaitKind::RwLockWrite: {
        const auto rw = s.objects.get<RwLock>(on.id);
        if (!rw) return gone();
        std::lock_guard lk(rw->lock);
        const bool read = on.kind == WaitKind::RwLockRead;
        return std::format("rwlock {:#x}{} to {}: writer {}, {}; {} and {} queued{}", on.id, quoted_name(rw->name),
            read ? "read" : "write", thread(rw->writer), count(rw->readers, "reader"),
            count(rw->read_waiters.size(), "reader"), count(rw->write_waiters.size(), "writer"),
            woken((read ? rw->read_waiters : rw->write_waiters).contains(&w)));
    }
    case WaitKind::Semaphore: {
        const auto sem = s.objects.get<Semaphore>(on.id);
        if (!sem) return gone();
        std::lock_guard lk(sem->lock);
        return std::format("semaphore {:#x}{}: count {} (max {}), {}{}", on.id, quoted_name(sem->name),
            std::max(sem->value, 0), sem->max, count(sem->waiters.size(), "waiter"), woken(sem->waiters.contains(&w)));
    }
    case WaitKind::EventQueue: {
        const auto q = s.objects.get<EventQueue>(on.id);
        if (!q) return gone();
        std::lock_guard lk(q->lock);
        return std::format("event queue {:#x}{}: {} of {} events queued, {}{}", on.id, quoted_name(q->name),
            q->events.size(), q->size, count(q->waiters.size(), "waiter"), woken(q->waiters.contains(&w)));
    }
    case WaitKind::EventFlag: {
        const auto f = s.objects.get<EventFlag>(on.id);
        if (!f) return gone();
        std::lock_guard lk(f->lock);
        if (!f->waiters.contains(&w)) {
            return std::format("event flag {:#x}{}: pattern {:#x} (being woken)", on.id, quoted_name(f->name),
                f->pattern);
        }
        // want[] is written under the flag's lock before the waiter queues.
        return std::format("event flag {:#x}{}: wants {:#x} ({}), pattern {:#x}, {}", on.id, quoted_name(f->name),
            w.want[0], flag_mode(w.want[1]), f->pattern, count(f->waiters.size(), "waiter"));
    }
    case WaitKind::LwMutex: {
        const auto m = s.objects.get<LwMutex>(on.id);
        if (!m) return gone();
        usize sleepers = 0;
        bool queued = false;
        {
            std::lock_guard lk(m->lock);
            sleepers = m->sleepers.size();
            queued = m->sleepers.contains(&w);
        }
        return std::format("lwmutex {:#x}{} ({}), {} in the kernel{}", on.id, quoted_name(m->name),
            lwmutex_user_side(s.guest, m->control, thread), count(sleepers, "sleeper"), woken(queued));
    }
    case WaitKind::LwCond: {
        const auto c = s.objects.get<LwCond>(on.id);
        if (!c) return gone();
        const auto m = s.objects.get<LwMutex>(c->lwmutex_id);
        usize waiters = 0;
        bool queued = false;
        {
            std::lock_guard lk(c->lock);
            waiters = c->waiters.size();
            queued = c->waiters.contains(&w);
        }
        if (!queued && m) {
            bool on_mutex = false;
            {
                std::lock_guard lk(m->lock);
                on_mutex = m->sleepers.contains(&w);
            }
            if (on_mutex) {
                return std::format("lwcond {:#x}{}: signalled, waits for lwmutex {:#x} ({})", on.id,
                    quoted_name(c->name), c->lwmutex_id, lwmutex_user_side(s.guest, m->control, thread));
            }
        }
        return std::format("lwcond {:#x}{} (sys_lwcond_t {:#010x}, lwmutex {:#x}): {}{}", on.id, quoted_name(c->name),
            c->control, c->lwmutex_id, count(waiters, "waiter"), woken(queued));
    }
    case WaitKind::Join: return std::format("thread {} to exit", thread(on.id));
    }
    return {};
}

template <class T>
T relaxed_load(const T& v)
{
    return __atomic_load_n(&v, __ATOMIC_RELAXED);
}

std::string duration_text(std::chrono::nanoseconds d)
{
    const double s = std::chrono::duration<double>(d).count();
    if (s >= 1) return std::format("{:.2f} s", s);
    if (s >= 1e-3) return std::format("{:.1f} ms", s * 1e3);
    return std::format("{:.0f} us", s * 1e6);
}

const char* state_text(const ThreadSnapshot& t)
{
    switch (t.state) {
    case State::NotStarted: return "not started";
    case State::Running: return "running";
    case State::Syscall: return "in syscall";
    case State::Blocked: return t.wait.kind == WaitKind::Sleep ? "sleeping" : "blocked";
    case State::Exited: return "exited";
    }
    return "?";
}

// The code the thread is in: a syscall, an HLE function, or "" (guest code, or not known).
std::string code_text(const ThreadSnapshot& t, const ReportOptions& o)
{
    if (t.syscall != kNoSyscall) {
        const char* name = syscall_name(t.syscall);
        return std::format("{} (syscall {})", name ? name : "?", t.syscall);
    }
    if (t.registers && o.describe_address) {
        if (std::string d = o.describe_address(t.cia); !d.empty()) return "HLE " + d;
    }
    return {};
}

std::string state_detail(const ThreadSnapshot& t, const ReportOptions& o)
{
    const std::string code = code_text(t, o);
    switch (t.state) {
    case State::NotStarted: return std::format("entry {:#010x}", t.cia);
    case State::Exited: return std::format("exit status {:#x}, not joined yet", t.exit_status);
    case State::Running: {
        std::string s = code.empty() ? (t.attached ? "host code" : "guest code") : code;
        if (t.current) s += " (this thread)";
        else if (t.racy) s += " (registers read while it runs)";
        return s;
    }
    case State::Syscall: return code + ", not parked";
    case State::Blocked: {
        std::string s = code.empty() ? std::string("outside a syscall") : "in " + code;
        if (t.wait.kind == WaitKind::Sleep) {
            if (t.remaining.count() >= 0) s += std::format(", {} left", duration_text(t.remaining));
            return s;
        }
        s += std::format(" for {}", duration_text(t.waited));
        if (t.remaining.count() >= 0) s += std::format(", times out in {}", duration_text(t.remaining));
        return s;
    }
    }
    return {};
}

std::string address_text(u32 addr, const ReportOptions& o)
{
    std::string s = std::format("{:#010x}", addr);
    if (o.describe_address) {
        if (const std::string d = o.describe_address(addr); !d.empty()) s += " [" + d + "]";
    }
    return s;
}

std::string rsx_text(Kernel& kernel)
{
    rsx::Rsx* rsx = kernel.rsx();
    if (!rsx) return "RSX: not created (no sys_rsx_context_allocate yet)\n";
    using Block = rsx::Rsx::BlockReason;
    const rsx::Rsx::Status st = rsx->status();
    // Until a FIFO setup decides it (sys_rsx_context_attribute 0x001), offsets are IO offsets.
    const char* where = st.fifo_location == rsx::FifoLocation::Local ? "local memory"
        : st.fifo_location == rsx::FifoLocation::Io                  ? "IO"
                                                                     : "IO, the default";
    std::string out = std::format("RSX: FIFO get {:#010x}, put {:#010x} ({}), {} words processed, {}, {}{}\n", st.get,
        st.put, where, st.words, count(st.flips, "flip"), count(st.failures, "failure"),
        st.running ? "" : "; the RSX thread is not running");
    switch (st.block) {
    case Block::Semaphore:
        if (st.semaphore_addr) {
            out += std::format("RSX: blocked on a semaphore acquire at {:#010x} (context {:#x}, offset {:#x}): holds "
                               "{:#010x}, wants {:#010x}\n",
                *st.semaphore_addr, st.semaphore_context, st.semaphore_offset, st.semaphore_value, st.block_value);
        } else {
            out += std::format("RSX: blocked on a semaphore acquire (context {:#x}, offset {:#x}, not translatable), "
                               "wants {:#010x}\n",
                st.semaphore_context, st.semaphore_offset, st.block_value);
        }
        break;
    case Block::JumpToSelf:
        out += std::format("RSX: parked on a jump to itself at FIFO offset {:#010x} (word {:#010x}), waiting for the "
                           "guest to patch it\n",
            st.get, st.block_value);
        break;
    case Block::None:
        out += st.get == st.put && st.idle ? "RSX: idle, the FIFO is empty\n" : "RSX: processing commands\n";
        break;
    }
    return out;
}

} // namespace

std::vector<u32> guest_backtrace(const GuestMemory& memory, u32 cia, u64 lr, u64 sp, u32 stack_lo, u32 stack_hi,
    usize max_frames)
{
    std::vector<u32> out;
    if (max_frames == 0) return out;
    out.push_back(cia);
    const auto committed = [&](u64 addr, u64 size) {
        return addr != 0 && addr + size <= GuestMemory::kGuestSize && memory.is_committed(static_cast<u32>(addr)) &&
            memory.is_committed(static_cast<u32>(addr + size - 1));
    };
    const auto code = [&](u64 addr) { return addr % 4 == 0 && committed(addr, 4); };
    const auto stack = [&](u64 addr, u64 size) {
        return addr % 16 == 0 && (!stack_hi || (addr >= stack_lo && addr + size <= stack_hi)) && committed(addr, size);
    };
    const bool have_lr = code(lr);
    if (have_lr && out.size() < max_frames) out.push_back(static_cast<u32>(lr));
    // Frame: [frame] = the caller's frame; [caller's frame + 16] = the return address the callee saved there.
    u64 frame = sp;
    for (bool first = true; out.size() < max_frames; first = false) {
        if (!stack(frame, 8)) break;
        const u64 caller = ps_read64(memory.base(), static_cast<u32>(frame));
        if (caller <= frame || !stack(caller, 24)) break;
        const u64 ret = ps_read64(memory.base(), static_cast<u32>(caller + 16));
        if (!code(ret)) break;
        if (!(first && have_lr && ret == lr)) out.push_back(static_cast<u32>(ret));
        frame = caller;
    }
    return out;
}

std::vector<ThreadSnapshot> snapshot_threads(Kernel& kernel, usize max_frames)
{
    Kernel::Impl& s = kernel.impl();
    const PpuThread* self = current_thread();
    const auto now = Clock::now();

    std::vector<std::shared_ptr<PpuThread>> threads;
    for (const auto& [id, obj] : s.threads.list(PpuThread::kIdBase)) {
        threads.push_back(std::static_pointer_cast<PpuThread>(obj));
    }
    std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a->id < b->id; });

    // 1. Identity and state.
    std::vector<ThreadSnapshot> out(threads.size());
    ThreadNames names;
    for (usize i = 0; i < threads.size(); i++) {
        PpuThread& t = *threads[i];
        ThreadSnapshot& r = out[i];
        r.id = t.id;
        bool started = false;
        {
            std::lock_guard lk(t.lock);
            r.name = t.name;
            started = t.started;
        }
        r.priority = t.priority();
        r.attached = t.attached;
        r.current = &t == self;
        bool finished = false;
        {
            std::lock_guard jl(s.join_mutex);
            finished = t.finished;
            r.exit_status = t.exit_status;
        }
        r.wait = t.waiter.waiting_on(); // acquire: the registers as they were when the wait began
        r.syscall = t.current_syscall.load(std::memory_order_acquire);
        if (!started) {
            r.state = State::NotStarted;
            r.cia = t.entry;
        } else if (finished) {
            r.state = State::Exited;
        } else if (r.wait.kind != WaitKind::None) {
            r.state = State::Blocked;
            r.waited = now - t.waiter.wait_since();
            if (const auto until = t.waiter.wait_until(); until != Clock::time_point{}) r.remaining = until - now;
        } else {
            r.state = r.syscall != kNoSyscall ? State::Syscall : State::Running;
        }
        names.names[r.id] = r.name;
    }

    // 2. What the blocked threads wait on.
    for (usize i = 0; i < threads.size(); i++) {
        if (out[i].state == State::Blocked) out[i].waits_on = describe_wait(s, threads[i]->waiter, out[i].wait, names);
    }

    // 3. Registers and backtraces, under the memory manager's lock (stacks are freed under it).
    std::lock_guard ml(s.memory.mutex);
    for (usize i = 0; i < threads.size(); i++) {
        PpuThread& t = *threads[i];
        ThreadSnapshot& r = out[i];
        if (t.attached || r.state == State::NotStarted || r.state == State::Exited || !t.stack_addr) continue;
        const PPUContext& c = *t.ctx;
        r.registers = true;
        r.racy = r.state == State::Running && !r.current;
        r.cia = relaxed_load(c.cia);
        r.lr = relaxed_load(c.lr);
        r.sp = relaxed_load(c.gpr[1]);
        // A stack pointer outside the thread's own stack (a game that switches stacks) is followed through any
        // committed memory.
        const bool own = r.sp >= t.stack_addr && r.sp < u64{t.stack_addr} + t.stack_size;
        r.backtrace = guest_backtrace(kernel.memory(), r.cia, r.lr, r.sp, own ? t.stack_addr : 0,
            own ? t.stack_addr + t.stack_size : 0, max_frames);
    }
    return out;
}

std::string format_report(Kernel& kernel, const ReportOptions& options)
{
    const std::vector<ThreadSnapshot> threads = snapshot_threads(kernel, options.max_frames);

    std::map<std::string, usize> states;
    for (const ThreadSnapshot& t : threads) states[state_text(t)]++;
    std::string summary;
    for (const char* name : {"blocked", "sleeping", "in syscall", "running", "not started", "exited"}) {
        if (const auto it = states.find(name); it != states.end()) summary += std::format(", {} {}", it->second, name);
    }

    std::string out = std::format("==== thread report{}{} ====\n", options.title.empty() ? "" : ": ", options.title);
    out += std::format("{}{}\n", count(threads.size(), "PPU thread"), summary);
    for (const ThreadSnapshot& t : threads) {
        out += std::format("{:#010x}  {:<27}  prio {:>4}  {:<11}  {}\n", t.id, t.name, t.priority, state_text(t),
            state_detail(t, options));
        if (!t.waits_on.empty()) out += std::format("    on {}\n", t.waits_on);
        if (!t.backtrace.empty()) {
            out += "    at ";
            for (usize i = 0; i < t.backtrace.size(); i++) {
                out += (i ? " <- " : "") + address_text(t.backtrace[i], options);
            }
            out += std::format("  (sp {:#010x})\n", t.sp);
        }
    }
    out += rsx_text(kernel);
    out += "==== end of thread report ====\n";
    return out;
}

} // namespace pspocket::lv2
