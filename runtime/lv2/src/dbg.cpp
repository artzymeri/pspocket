// lv2's debugging calls a retail console has (D-937), ported from the retail kernel's own handlers (lv2_kernel.self of
// the 4.93 system software, read in inventory/firmware/kernel493 and never committed; the handler addresses below are
// the kernel's). RPCS3 answers these with null_func (CELL_OK, nothing written).
//
// What a retail kernel checks first (see docs/decisions.md D-937):
// - A process's privileges are its SELF's control flags (supplemental header 1: 256 bits), which some calls compare as
//   a number with a level: the system software's modules have 0x4000... and the kernel 0x8000...; a game has 0 (its
//   EBOOT's flags are zero). pspocket's root process (tests, the system software's place) stands for the system
//   software's level; a game's process for a game's.
// - A call that names a process (0x800000000008CB4C): on a retail console (target ID 0x83 to 0x9F, read from the
//   hypervisor's repository) a process may name itself and no other, whatever its privileges; any other ID is
//   CELL_LV2DBG_ERROR_DEINVALIDARGUMENTS. pspocket runs one process, ID 1 (sys_process_getpid).
//
// The information calls (mutex, condition variable, rwlock, lwmutex, event queue, semaphore, lwcond, event flag) copy
// the caller's structure in, fill it and copy it back whole, so the words they do not set keep what the caller had
// there. Each list in it comes as an address and a capacity: the kernel writes as many entries as fit (thread IDs, 64
// bits each, in queue order) and turns the capacity into the count written; no address or no room writes nothing and
// a count of 0. A list it cannot write fails the call (CELL_EFAULT) with the structure left alone. Names are kept to
// seven characters and a NUL at creation, as the kernel keeps them.

#include <algorithm>
#include <array>
#include <cstring>

#include "internal.hpp"
#include "spu.hpp"

namespace pspocket::lv2 {

namespace {

constexpr u32 kProcessId = 1; // sys_process_getpid

// CELL_LV2DBG_ERROR_* (RPCS3 Modules/sys_lv2dbg.h).
constexpr CellError kDeInvalidThreadId = static_cast<CellError>(0x80010402);
constexpr CellError kDeNoMem = static_cast<CellError>(0x80010408);
constexpr CellError kDeInvalidArguments = static_cast<CellError>(0x80010409);
constexpr CellError kDeHandlerAlreadyRegistered = static_cast<CellError>(0x80010429);
constexpr CellError kDeInvalidHandler = static_cast<CellError>(0x8001042A);
constexpr CellError kDeHandleNotRegistered = static_cast<CellError>(0x8001042B);
constexpr CellError kDeInvalidThreadType = static_cast<CellError>(0x80010415);

// The calling process's control flags reach the system software's level (0x4000... and above).
bool system_level(PpuThread& t) { return t.kernel.config().root; }

// The process a call names, then the process looked up (CELL_LV2DBG_ERROR_DEINVALIDPROCESSID when it is gone, which
// the process itself never is).
Result inspect(u32 pid) { return pid == kProcessId ? CELL_OK : err(kDeInvalidArguments); }

// A name as the kernel keeps it: seven characters and a NUL.
u64 kept_name(u64 name) { return name & ~u64{0xFF}; }

// pshared and the IPC key as the kernel reports them (0x800000000026D9F4): SYS_SYNC_NOT_PROCESS_SHARED with key 0,
// or SYS_SYNC_PROCESS_SHARED with the key the object is registered under.
u32 pshared_of(const Object& o) { return o.ipc_key ? SYS_SYNC_PROCESS_SHARED : SYS_SYNC_NOT_PROCESS_SHARED; }

std::vector<u32> ids_of(const std::vector<Waiter*>& waiters)
{
    std::vector<u32> ids;
    ids.reserve(waiters.size());
    for (const Waiter* w : waiters) ids.push_back(w->id);
    return ids;
}

// The caller's structure, copied in and back whole.
class InfoBlock {
public:
    InfoBlock(const Guest& g, u32 addr, u32 size) : g_(g), addr_(addr), size_(size) {}

    bool load()
    {
        if (!g_.valid(addr_, size_)) return false;
        std::memcpy(b_.data(), g_.ptr(addr_), size_);
        return true;
    }
    Result store() const
    {
        if (!g_.valid(addr_, size_)) return err(CELL_EFAULT);
        std::memcpy(g_.ptr(addr_), b_.data(), size_);
        return CELL_OK;
    }
    u32 r32(u32 at) { return ps_read32(b_.data(), at); }
    void w32(u32 at, u32 v) { ps_write32(b_.data(), at, v); }
    void w64(u32 at, u64 v) { ps_write64(b_.data(), at, v); }

private:
    const Guest& g_;
    u32 addr_;
    u32 size_;
    std::array<u8, 0x48> b_{};
};

// The kernel takes a list's room from its heap, whose pieces go up to 128 KB (0x8000000000064824): a longer list
// panics a retail console's kernel. Here the call fails as when the heap has no room, and says why.
constexpr u64 kKernelHeapPiece = 0x20000;

Result list_room(std::string_view call, u32 capacity, u32 entry)
{
    if (u64{capacity} * entry <= kKernelHeapPiece) return CELL_OK;
    PS_LOG_ERROR(kChan,
        "{}: a list of {} entries of {} bytes is more than the kernel's largest heap piece (a retail console's kernel "
        "panics): CELL_LV2DBG_ERROR_DENOMEM",
        call, capacity, entry);
    return err(kDeNoMem);
}

// One list of thread IDs: its address at `at`, its capacity at `at + 4`.
Result put_ids(std::string_view call, const Guest& g, InfoBlock& info, u32 at, const std::vector<u32>& ids)
{
    const u32 list = info.r32(at);
    u32 n = info.r32(at + 4);
    if (!list || !n) {
        info.w32(at + 4, 0);
        return CELL_OK;
    }
    if (const Result r = list_room(call, n, 8); r != CELL_OK) return r;
    n = static_cast<u32>(std::min<usize>(n, ids.size()));
    if (n && !g.valid(list, u64{n} * 8)) return err(CELL_EFAULT);
    for (u32 i = 0; i < n; i++) g.w64(list + i * 8, ids[i]);
    info.w32(at + 4, n);
    return CELL_OK;
}

template <class T>
std::shared_ptr<T> object(PpuThread& t, u32 id)
{
    return t.kernel.impl().objects.get<T>(id);
}

// A string into a buffer the caller gave (the kernel's names and paths): `copy` characters at most, then zeros to
// `size` bytes. False when the buffer is not guest memory.
bool put_text(const Guest& g, u32 addr, const std::string& text, u32 copy, u32 size)
{
    if (!g.valid(addr, size)) return false;
    std::memset(g.ptr(addr), 0, size);
    std::memcpy(g.ptr(addr), text.data(), std::min<usize>(text.size(), copy));
    return true;
}

// The process's PPU threads as the kernel lists them (0x8000000000090DA0): those alive, then those that ended and wait
// for a join; each in creation order.
std::vector<std::shared_ptr<PpuThread>> ppu_threads(Kernel::Impl& s)
{
    std::vector<std::shared_ptr<PpuThread>> alive, ended;
    std::lock_guard lk(s.join_mutex);
    for (const auto& [id, obj] : s.threads.list(PpuThread::kIdBase)) {
        auto th = std::static_pointer_cast<PpuThread>(obj);
        (th->finished ? ended : alive).push_back(std::move(th));
    }
    alive.insert(alive.end(), ended.begin(), ended.end());
    return alive;
}

// A PPU thread's state as the debugging calls number them: the kernel's own state less one (0x800000000008F91C),
// 0 created and not started, 1 runnable, 2 on a processor, 3 sleeping, 6 stopped by a debugger, 8 ended and not yet
// joined. pspocket's threads run on host threads of their own: the caller is on a processor, a thread waiting in a
// call sleeps, any other is runnable.
u32 ppu_state(Kernel::Impl& s, const PpuThread& caller, PpuThread& th)
{
    {
        std::lock_guard lk(s.join_mutex);
        if (th.finished) return 8;
    }
    {
        std::lock_guard lk(th.lock);
        if (!th.started) return 0;
    }
    if (&th == &caller) return 2;
    if (th.waiter.waiting_on().kind != WaitKind::None) return 3;
    return 1;
}

// An SPU thread group's state as the debugging calls number it (0x800000000008FA28): the kernel's run flags read as
// SYS_SPU_THREAD_GROUP_STATUS, a group that runs reading as ready (2); RUNNING (6) is never given.
u32 group_state(SpuGroupState state)
{
    switch (state) {
    case SpuGroupState::NotInitialized: return 0;
    case SpuGroupState::Initialized: return 1;
    case SpuGroupState::Ready:
    case SpuGroupState::Running: return 2;
    case SpuGroupState::Waiting: return 3;
    case SpuGroupState::Suspended: return 4;
    case SpuGroupState::WaitingAndSuspended: return 5;
    case SpuGroupState::Stopped:
    case SpuGroupState::Destroyed: return 7;
    }
    return 7;
}

} // namespace

// sys_dbg_get_console_type(u64* type), the kernel's 0x8000000000089AE0: a null pointer is CELL_EFAULT; a process below
// the system software's level gets CELL_ENOSYS; else 1 on a retail console (2 a development one, 3 a debug one).
Result sys_dbg_get_console_type(PpuThread& t, u32 type)
{
    if (!type) return err(CELL_EFAULT);
    if (!system_level(t)) return err(CELL_ENOSYS);
    if (!t.kernel.impl().guest.put64(type, 1)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_dbg_get_mutex_information(pid, id, info), 0x800000000008CF20. The structure (0x48 bytes): the attribute
// (protocol, recursive, pshared, adaptive, IPC key; its flags and padding as the caller had them; name), the owner
// (64 bits), the re-locks beyond the first, the condition variables bound, 0, then the waiters' list (address,
// capacity) and how many wait in all.
Result sys_dbg_get_mutex_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto m = object<Mutex>(t, id);
    if (!m) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x48);
    if (!info.load()) return err(CELL_EFAULT);
    u32 owner = 0, lock_count = 0, cond_count = 0;
    std::vector<u32> waiting;
    {
        std::lock_guard lk(m->lock);
        owner = m->owner;
        lock_count = m->lock_count;
        cond_count = m->cond_count;
        waiting = ids_of(m->waiters.ordered(m->protocol));
    }
    const u32 flags = (m->recursive ? SYS_SYNC_RECURSIVE : SYS_SYNC_NOT_RECURSIVE) | pshared_of(*m) | m->adaptive;
    info.w32(0x00, m->protocol);
    info.w32(0x04, flags & 0x30);
    info.w32(0x08, pshared_of(*m));
    info.w32(0x0C, flags & 0x3000);
    info.w64(0x10, m->ipc_key);
    info.w64(0x20, kept_name(m->name));
    info.w64(0x28, owner);
    info.w32(0x30, lock_count);
    info.w32(0x34, cond_count);
    if (const Result r = put_ids("sys_dbg_get_mutex_information", g, info, 0x3C, waiting); r != CELL_OK) return r;
    info.w32(0x44, static_cast<u32>(waiting.size()));
    info.w32(0x38, 0);
    return info.store();
}

// sys_dbg_get_cond_information(pid, id, info), 0x800000000008E088. The structure (0x28 bytes): the attribute
// (pshared; its flags as the caller had them; IPC key, name), the mutex's ID, the waiters' list and how many wait.
Result sys_dbg_get_cond_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto c = object<Cond>(t, id);
    if (!c) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x28);
    if (!info.load()) return err(CELL_EFAULT);
    std::vector<u32> waiting;
    {
        std::lock_guard lk(c->mutex->lock);
        waiting = ids_of(c->waiters.ordered(c->mutex->protocol));
    }
    info.w32(0x00, pshared_of(*c));
    info.w64(0x08, c->ipc_key);
    info.w64(0x10, kept_name(c->name));
    info.w32(0x18, c->mutex_id);
    if (const Result r = put_ids("sys_dbg_get_cond_information", g, info, 0x1C, waiting); r != CELL_OK) return r;
    info.w32(0x24, static_cast<u32>(waiting.size()));
    return info.store();
}

// sys_dbg_get_rwlock_information(pid, id, info), 0x800000000008DD40. The structure (0x40 bytes): the attribute
// (protocol, pshared, IPC key; flags and padding as the caller had them; name), the writer that owns it (64 bits),
// then the readers' list and how many readers wait, the writers' list and how many writers wait. The kernel keeps
// waiting readers in arrival order whatever the protocol (0x8000000000028C28), writers by the protocol.
Result sys_dbg_get_rwlock_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto l = object<RwLock>(t, id);
    if (!l) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x40);
    if (!info.load()) return err(CELL_EFAULT);
    u32 writer = 0;
    std::vector<u32> readers, writers;
    {
        std::lock_guard lk(l->lock);
        writer = l->writer;
        readers = ids_of(l->read_waiters.ordered(SYS_SYNC_FIFO));
        writers = ids_of(l->write_waiters.ordered(l->protocol));
    }
    info.w32(0x00, l->protocol);
    info.w32(0x04, pshared_of(*l));
    info.w64(0x08, l->ipc_key);
    info.w64(0x18, kept_name(l->name));
    info.w64(0x20, writer);
    if (const Result r = put_ids("sys_dbg_get_rwlock_information", g, info, 0x28, readers); r != CELL_OK) return r;
    info.w32(0x30, static_cast<u32>(readers.size()));
    if (const Result r = put_ids("sys_dbg_get_rwlock_information", g, info, 0x34, writers); r != CELL_OK) return r;
    info.w32(0x3C, static_cast<u32>(writers.size()));
    return info.store();
}

// sys_dbg_get_lwmutex_information(pid, id, info), 0x800000000008DA9C. The structure (0x28 bytes): the attribute
// (protocol; recursive from the sys_lwmutex_t's attribute word; name), the owner and the recursion count from the
// sys_lwmutex_t in the process's memory (CELL_EFAULT when it cannot be read), the sleepers' list and how many sleep.
// An lwmutex not made the way sysPrxForUser makes them (kind 1) is CELL_EINVAL.
Result sys_dbg_get_lwmutex_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto m = object<LwMutex>(t, id);
    if (!m) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x28);
    if (!info.load()) return err(CELL_EFAULT);
    if (m->kind != 1) return err(CELL_EINVAL);
    // sys_lwmutex_t: lock_var (owner, waiter), attribute, recursive_count, sleep_queue, pad.
    if (!g.valid(m->control, 0x18)) return err(CELL_EFAULT);
    const u32 owner = g.r32(m->control);
    const u32 attribute = g.r32(m->control + 8);
    const u32 recursive_count = g.r32(m->control + 12);
    std::vector<u32> sleeping;
    {
        std::lock_guard lk(m->lock);
        sleeping = ids_of(m->sleepers.ordered(m->protocol));
    }
    info.w32(0x00, m->protocol);
    info.w32(0x04, attribute & 0x30);
    info.w64(0x08, kept_name(m->name));
    info.w64(0x10, owner);
    info.w32(0x18, recursive_count);
    if (const Result r = put_ids("sys_dbg_get_lwmutex_information", g, info, 0x1C, sleeping); r != CELL_OK) return r;
    info.w32(0x24, static_cast<u32>(sleeping.size()));
    return info.store();
}

// sys_dbg_get_event_queue_information(pid, id, info), 0x800000000008D778. The structure (0x38 bytes): the attribute
// (protocol, type, name), the IPC key (0 for a local queue), the size, the receivers' list and how many wait, then
// the queued events' list (32 bytes each: source, data1, data2, data3) and how many are queued.
Result sys_dbg_get_event_queue_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto q = object<EventQueue>(t, id);
    if (!q) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x38);
    if (!info.load()) return err(CELL_EFAULT);
    std::vector<u32> waiting;
    std::vector<Event> events;
    {
        std::lock_guard lk(q->lock);
        waiting = ids_of(q->waiters.ordered(q->protocol));
        events.assign(q->events.begin(), q->events.end());
    }
    constexpr std::string_view kCall = "sys_dbg_get_event_queue_information";
    info.w32(0x00, q->protocol);
    info.w32(0x04, q->type);
    info.w64(0x08, kept_name(q->name));
    info.w64(0x10, q->ipc_key);
    info.w32(0x18, q->size);
    if (const Result r = put_ids(kCall, g, info, 0x1C, waiting); r != CELL_OK) return r;
    info.w32(0x24, static_cast<u32>(waiting.size()));
    const u32 list = info.r32(0x28);
    u32 n = info.r32(0x2C);
    if (!list || !n) {
        n = 0;
    } else {
        if (const Result r = list_room(kCall, n, 32); r != CELL_OK) return r;
        n = static_cast<u32>(std::min<usize>(n, events.size()));
        if (n && !g.valid(list, u64{n} * 32)) return err(CELL_EFAULT);
        for (u32 i = 0; i < n; i++) {
            g.w64(list + i * 32, events[i].source);
            g.w64(list + i * 32 + 8, events[i].data1);
            g.w64(list + i * 32 + 16, events[i].data2);
            g.w64(list + i * 32 + 24, events[i].data3);
        }
    }
    info.w32(0x2C, n);
    info.w32(0x30, static_cast<u32>(events.size()));
    return info.store();
}

// sys_dbg_get_semaphore_information(pid, id, info), 0x800000000008D4F4. The structure (0x38 bytes): the attribute
// (protocol, pshared, IPC key; flags and padding as the caller had them; name), the maximum and current values, the
// waiters' list and how many wait.
Result sys_dbg_get_semaphore_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto s = object<Semaphore>(t, id);
    if (!s) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x38);
    if (!info.load()) return err(CELL_EFAULT);
    s32 value = 0;
    std::vector<u32> waiting;
    {
        std::lock_guard lk(s->lock);
        value = std::max(s->value, 0); // pspocket's count goes below 0 by the waiters; the kernel's stays at 0
        waiting = ids_of(s->waiters.ordered(s->protocol));
    }
    info.w32(0x00, s->protocol);
    info.w32(0x04, pshared_of(*s));
    info.w64(0x08, s->ipc_key);
    info.w64(0x18, kept_name(s->name));
    info.w32(0x20, static_cast<u32>(s->max));
    info.w32(0x24, static_cast<u32>(value));
    if (const Result r = put_ids("sys_dbg_get_semaphore_information", g, info, 0x28, waiting); r != CELL_OK) return r;
    info.w32(0x30, static_cast<u32>(waiting.size()));
    return info.store();
}

// sys_dbg_get_lwcond_information(pid, id, info), 0x800000000008E330. The structure (0x18 bytes): the name, the
// sys_lwmutex_t address from the sys_lwcond_t in the process's memory (CELL_EFAULT when it cannot be read), the
// waiters' list and how many wait.
Result sys_dbg_get_lwcond_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto c = object<LwCond>(t, id);
    if (!c) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x18);
    if (!info.load()) return err(CELL_EFAULT);
    // sys_lwcond_t: lwmutex, lwcond_queue.
    if (!g.valid(c->control, 8)) return err(CELL_EFAULT);
    const u32 lwmutex = g.r32(c->control);
    std::vector<u32> waiting;
    {
        std::lock_guard lk(c->lock);
        waiting = ids_of(c->waiters.ordered(c->protocol));
    }
    info.w64(0x00, kept_name(c->name));
    info.w32(0x08, lwmutex);
    if (const Result r = put_ids("sys_dbg_get_lwcond_information", g, info, 0x0C, waiting); r != CELL_OK) return r;
    info.w32(0x14, static_cast<u32>(waiting.size()));
    return info.store();
}

// sys_dbg_get_event_flag_information(pid, id, info), 0x800000000008D1CC. The structure (0x38 bytes): the attribute
// (protocol, pshared, IPC key; its flags as the caller had them; type, name), the bit pattern, then two lists that go
// together, the waiters' IDs and what each waits for (16 bytes: the bits, 64 bits, then the mode), their count, and
// how many wait. Without either list's address, or without room, neither is written.
Result sys_dbg_get_event_flag_information(PpuThread& t, u32 pid, u32 id, u32 info_addr)
{
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    const auto f = object<EventFlag>(t, id);
    if (!f) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    InfoBlock info(g, info_addr, 0x38);
    if (!info.load()) return err(CELL_EFAULT);
    u64 pattern = 0;
    std::vector<std::array<u64, 3>> waiting; // ID, bits, mode
    {
        std::lock_guard lk(f->lock);
        pattern = f->pattern;
        for (const Waiter* w : f->waiters.ordered(f->protocol)) waiting.push_back({w->id, w->want[0], w->want[1]});
    }
    constexpr std::string_view kCall = "sys_dbg_get_event_flag_information";
    info.w32(0x00, f->protocol);
    info.w32(0x04, pshared_of(*f));
    info.w64(0x08, f->ipc_key);
    info.w32(0x14, f->type);
    info.w64(0x18, kept_name(f->name));
    const u32 ids = info.r32(0x28);
    const u32 wants = info.r32(0x2C);
    u32 n = info.r32(0x30);
    if (!n || !ids || !wants) {
        n = 0;
    } else {
        if (const Result r = list_room(kCall, n, 8); r != CELL_OK) return r;
        if (const Result r = list_room(kCall, n, 16); r != CELL_OK) return r;
        n = static_cast<u32>(std::min<usize>(n, waiting.size()));
        if (n && !g.valid(ids, u64{n} * 8)) return err(CELL_EFAULT);
        for (u32 i = 0; i < n; i++) g.w64(ids + i * 8, waiting[i][0]);
        if (n && !g.valid(wants, u64{n} * 16)) return err(CELL_EFAULT);
        for (u32 i = 0; i < n; i++) {
            g.w64(wants + i * 16, waiting[i][1]);
            g.w32(wants + i * 16 + 8, static_cast<u32>(waiting[i][2]));
            g.w32(wants + i * 16 + 12, 0); // the kernel's own heap leaves these four bytes as they were
        }
    }
    info.w64(0x20, pattern);
    info.w32(0x30, n);
    info.w32(0x34, static_cast<u32>(waiting.size()));
    return info.store();
}

// sys_dbg_initialize_ppu_exception_handler(queue), 0x800000000008E5CC: the process's PPU exceptions are to be sent to
// this event queue (the kernel connects two ports of its own to it, named 0x8000111100000001 and ...02). It asks no
// privilege. CELL_ESRCH without such a queue; a second registration is CELL_LV2DBG_ERROR_DEHANDLERALREADYREGISTERED.
// pspocket keeps the queue; a fault in translated code still stops the run (D-937: delivering exceptions is engine
// work to come).
Result sys_dbg_initialize_ppu_exception_handler(PpuThread& t, u32 queue_id)
{
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard lk(s.dbg_mutex);
    auto q = s.objects.get<EventQueue>(queue_id);
    if (!q) return err(CELL_ESRCH);
    if (s.dbg_exception_queue) return err(kDeHandlerAlreadyRegistered);
    s.dbg_exception_queue = std::move(q);
    return CELL_OK;
}

// sys_dbg_finalize_ppu_exception_handler(queue), 0x800000000008E7F4: on a retail console any process may call it.
// Nothing registered: CELL_LV2DBG_ERROR_DEHANDLENOTREGISTERED; no such queue: CELL_ESRCH; another queue than the one
// registered: CELL_LV2DBG_ERROR_DEINVALIDHANDLER. The queue gets a last event from the first port (data1 all ones,
// which tells the handler's thread to end) and the registration ends.
Result sys_dbg_finalize_ppu_exception_handler(PpuThread& t, u32 queue_id)
{
    constexpr u64 kExceptionPort = 0x8000111100000001;
    Kernel::Impl& s = t.kernel.impl();
    std::shared_ptr<EventQueue> q;
    {
        std::lock_guard lk(s.dbg_mutex);
        if (!s.dbg_exception_queue) return err(kDeHandleNotRegistered);
        q = s.objects.get<EventQueue>(queue_id);
        if (!q) return err(CELL_ESRCH);
        if (q != s.dbg_exception_queue) return err(kDeInvalidHandler);
        s.dbg_exception_queue.reset();
    }
    q->send(Event{kExceptionPort, ~u64{0}, 0, 0}); // a full or destroyed queue loses it, as on the console
    return CELL_OK;
}

// sys_dbg_get_thread_list(pid, list, count, all), 0x8000000000090DA0. The process's threads: its PPU threads (type 0,
// the ID as 64 bits) then its SPU thread groups (type 2, the ID in the first word of the 64 bits), 16 bytes each.
// *all (CELL_LV2DBG_ERROR_DEINVALIDARGUMENTS without it) gets how many there are; *count (64 bits, CELL_EFAULT
// when unreadable, at most 0x7FFFFFFF) is the room in `list`. With a list and room, as many as fit are written, and
// when all fit *count becomes the number too.
Result sys_dbg_get_thread_list(PpuThread& t, u32 pid, u32 list, u32 count, u32 all)
{
    Kernel::Impl& s = t.kernel.impl();
    const Guest& g = s.guest;
    if (!all) return err(kDeInvalidArguments);
    if (!g.valid(count, 8)) return err(CELL_EFAULT);
    const u64 room = g.r64(count);
    if (room > 0x7FFFFFFF) return err(kDeInvalidArguments);
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    std::vector<std::pair<u32, u32>> entries; // type, ID
    for (const auto& th : ppu_threads(s)) entries.emplace_back(0, th->id);
    if (entries.empty()) return err(kDeInvalidThreadId);
    for (const auto& [id, obj] : s.spu_groups.list(SpuThreadGroup::kIdBase)) entries.emplace_back(2, id);
    const u64 total = entries.size();
    if (list && room) {
        if (const Result r = list_room("sys_dbg_get_thread_list", static_cast<u32>(room), 16); r != CELL_OK) return r;
        const u32 n = static_cast<u32>(std::min<u64>(room, total));
        if (!g.valid(list, u64{n} * 16)) return err(CELL_EFAULT);
        for (u32 i = 0; i < n; i++) {
            const auto [type, id] = entries[i];
            g.w32(list + i * 16, type);
            g.w32(list + i * 16 + 4, 0);
            // A PPU thread's ID fills the 64 bits; an SPU thread group's is their first word.
            g.w64(list + i * 16 + 8, type == 0 ? u64{id} : u64{id} << 32);
        }
        if (n == total && !g.put64(count, total)) return err(CELL_EFAULT);
    }
    if (!g.put64(all, total)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_dbg_get_thread_info(thread, info), 0x8000000000090D5C: `thread` is a list entry (type, then the ID), `info`
// what the type has; CELL_LV2DBG_ERROR_DEINVALIDARGUMENTS without either, CELL_LV2DBG_ERROR_DEINVALIDTHREADTYPE for a
// type other than 0, 1, 2. The kernel writes each field on its own, so a field it cannot write ends the call there.
// - A PPU thread (0x800000000008E9A0): the process ID at +0x10, the priority at +0, the base priority at +0x18, the
//   state at +4, the stack's address and size at +8; then, when +0x14 holds a buffer's address, the name (at most 27
//   characters) in 128 bytes there.
// - An SPU thread (0x800000000008EB90): the process ID at +4, the group's ID at +0; the name in the 128-byte buffer at
//   +0xC, the image's INFO segment in the 512-byte buffer at +8 (each when given).
// - An SPU thread group (0x800000000008ED50): the process ID at +8, the state at +4, the priority at +0, the name in
//   the 128-byte buffer at +0xC; with a list at +0x18 and room at +0x10, its SPU threads' IDs (32 bits each), +0x14
//   how many it has and +0x10 how many were written.
// A thread or group that is not the process's: CELL_LV2DBG_ERROR_DEINVALIDTHREADID.
Result sys_dbg_get_thread_info(PpuThread& t, u32 thread, u32 info)
{
    Kernel::Impl& s = t.kernel.impl();
    const Guest& g = s.guest;
    if (!info || !thread) return err(kDeInvalidArguments);
    if (!g.valid(thread, 16)) return err(CELL_EFAULT);
    const u32 type = g.r32(thread);
    const u64 id = g.r64(thread + 8);
    const auto put = [&](u32 at, u32 v) { return g.put32(info + at, v); };
    if (type == 0) {
        const auto th = s.threads.get<PpuThread>(static_cast<u32>(id));
        if (!th) return err(kDeInvalidThreadId);
        std::string name;
        {
            std::lock_guard lk(th->lock);
            name = th->name;
        }
        if (!put(0x10, kProcessId) || !put(0x00, static_cast<u32>(th->priority())) ||
            !put(0x18, static_cast<u32>(th->priority())) || !put(0x04, ppu_state(s, t, *th)) ||
            !put(0x08, th->stack_addr) || !put(0x0C, th->stack_size) || !g.valid(info + 0x14, 4)) {
            return err(CELL_EFAULT);
        }
        if (const u32 buf = g.r32(info + 0x14); buf && !put_text(g, buf, name, 27, 0x80)) return err(CELL_EFAULT);
        return CELL_OK;
    }
    if (type != 1 && type != 2) return err(kDeInvalidThreadType);
    const u32 sid = static_cast<u32>(id >> 32);
    if (type == 1) {
        const auto grp = s.spu_groups.get<SpuThreadGroup>((sid & 0xFFFFFF) | 0x04000000);
        std::shared_ptr<SpuThread> th;
        if (grp) {
            std::lock_guard lk(grp->lock);
            if ((sid >> 24) < grp->threads.size()) th = grp->threads[sid >> 24];
        }
        if (!th) return err(kDeInvalidThreadId);
        if (!put(0x04, kProcessId) || !put(0x00, grp->id) || !g.valid(info + 0x0C, 4)) return err(CELL_EFAULT);
        if (const u32 buf = g.r32(info + 0x0C); buf && !put_text(g, buf, th->name, 0x80, 0x80)) return err(CELL_EFAULT);
        if (!g.valid(info + 0x08, 4)) return err(CELL_EFAULT);
        if (const u32 buf = g.r32(info + 0x08); buf) {
            if (!put_text(g, buf, std::string(th->info.begin(), th->info.end()), 0x100, 0x200)) return err(CELL_EFAULT);
        }
        return CELL_OK;
    }
    const auto grp = s.spu_groups.get<SpuThreadGroup>(sid);
    if (!grp) return err(kDeInvalidThreadId);
    u32 state = 0, slots = 0;
    s32 prio = 0;
    std::vector<u32> members;
    {
        std::lock_guard lk(grp->lock);
        state = group_state(grp->state);
        prio = grp->prio;
        slots = grp->max_num;
        for (u32 i = 0; i < grp->max_num; i++) {
            if (grp->threads[i]) members.push_back(grp->threads[i]->id);
        }
    }
    if (!put(0x08, kProcessId) || !put(0x04, state) || !put(0x00, static_cast<u32>(prio)) || !g.valid(info + 0x0C, 16)) {
        return err(CELL_EFAULT);
    }
    if (const u32 buf = g.r32(info + 0x0C); buf && !put_text(g, buf, grp->name, 0x80, 0x80)) return err(CELL_EFAULT);
    u32 n = g.r32(info + 0x10);
    const u32 list = g.r32(info + 0x18);
    if (!n || !list) return CELL_OK;
    if (const Result r = list_room("sys_dbg_get_thread_info", n, 4); r != CELL_OK) return r;
    n = static_cast<u32>(std::min<usize>(n, members.size()));
    if (n && !g.valid(list, u64{n} * 4)) return err(CELL_EFAULT);
    for (u32 i = 0; i < n; i++) g.w32(list + i * 4, members[i]);
    if (!put(0x14, slots) || !put(0x10, n)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_dbg_get_process_info(pid, info), 0x8000000000091AE4 (CELL_LV2DBG_ERROR_DEINVALIDARGUMENTS without `info`); each
// field on its own: the process ID at +0, its status at +4 (2, running), its PPU threads at +8 (those ended and not
// joined too), its SPU thread groups at +0xC, its raw SPUs at +0x10, the parent's ID at +0x14 (as
// sys_process_getppid), then, when +0x18 holds a buffer's address, the executable's path in 512 bytes there; +0x1C,
// the byte at +0x20, +0x28 (64 bits: the process's debugging event flags without their top four bits) and +0x30 are
// what the kernel keeps of a process pspocket has none of: 0.
Result sys_dbg_get_process_info(PpuThread& t, u32 pid, u32 info)
{
    Kernel::Impl& s = t.kernel.impl();
    const Guest& g = s.guest;
    if (const Result r = inspect(pid); r != CELL_OK) return r;
    if (!info) return err(kDeInvalidArguments);
    const auto put = [&](u32 at, u32 v) { return g.put32(info + at, v); };
    if (!put(0x00, kProcessId) || !put(0x04, 2) || !put(0x08, static_cast<u32>(ppu_threads(s).size())) ||
        !put(0x0C, s.spu_groups.count(SpuThreadGroup::kIdBase)) || !put(0x10, raw_spu_count(s)) || !put(0x14, 0) ||
        !put(0x1C, 0) || !g.valid(info + 0x18, 4)) {
        return err(CELL_EFAULT);
    }
    if (const u32 buf = g.r32(info + 0x18); buf && !put_text(g, buf, t.kernel.config().executable_path, 0x1FF, 0x200)) {
        return err(CELL_EFAULT);
    }
    if (!g.valid(info + 0x20, 1)) return err(CELL_EFAULT);
    g.w8(info + 0x20, 0);
    if (!put(0x30, 0) || !g.put64(info + 0x28, 0)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_dbg_get_process_memory_container_information(pid, list, count), 0x8000000000091AE8: the process's memory
// containers, its own first, then those made from it, 16 bytes each (the size and what is free in bytes, the ID, the
// parent's ID or -1; pspocket names the process's own container as the memory calls do, -1). *count (CELL_EFAULT when
// unreadable) is the room; fewer than there are is CELL_EINVAL. *count becomes the number, then the entries are
// written. Another process: CELL_ENOSYS on a retail console.
Result sys_dbg_get_process_memory_container_information(PpuThread& t, u32 pid, u32 list, u32 count)
{
    Kernel::Impl& s = t.kernel.impl();
    const Guest& g = s.guest;
    if (pid != kProcessId) return err(CELL_ENOSYS);
    if (!g.valid(count, 4)) return err(CELL_EFAULT);
    const s32 room = static_cast<s32>(g.r32(count));
    std::vector<std::array<u32, 4>> entries;
    {
        std::lock_guard lk(s.memory.mutex);
        const MemoryContainer& root = s.memory.default_container;
        entries.push_back({root.size, root.size - root.used, 0xFFFFFFFF, 0xFFFFFFFF});
        for (const auto& [id, obj] : s.containers.list(MemoryContainer::kIdBase)) {
            const auto& ct = static_cast<const MemoryContainer&>(*obj);
            entries.push_back({ct.size, ct.size - ct.used, id, 0xFFFFFFFF});
        }
    }
    if (room < static_cast<s32>(entries.size())) return err(CELL_EINVAL);
    if (!g.put32(count, static_cast<u32>(entries.size()))) return err(CELL_EFAULT);
    for (usize i = 0; i < entries.size(); i++) {
        const u32 at = list + static_cast<u32>(i) * 16;
        if (!g.valid(at, 16)) return err(CELL_EFAULT);
        for (u32 w = 0; w < 4; w++) g.w32(at + w * 4, entries[i][w]);
    }
    return CELL_OK;
}

void register_dbg_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 909, sys_dbg_get_thread_list);
    PS_LV2_BIND(table, 910, sys_dbg_get_thread_info);
    PS_LV2_BIND(table, 914, sys_dbg_get_process_info);
    PS_LV2_BIND(table, 957, sys_dbg_get_process_memory_container_information);
    PS_LV2_BIND(table, 933, sys_dbg_get_mutex_information);
    PS_LV2_BIND(table, 935, sys_dbg_get_cond_information);
    PS_LV2_BIND(table, 937, sys_dbg_get_rwlock_information);
    PS_LV2_BIND(table, 941, sys_dbg_get_lwmutex_information);
    PS_LV2_BIND(table, 943, sys_dbg_get_event_queue_information);
    PS_LV2_BIND(table, 944, sys_dbg_initialize_ppu_exception_handler);
    PS_LV2_BIND(table, 945, sys_dbg_finalize_ppu_exception_handler);
    PS_LV2_BIND(table, 947, sys_dbg_get_semaphore_information);
    PS_LV2_BIND(table, 951, sys_dbg_get_lwcond_information);
    PS_LV2_BIND(table, 971, sys_dbg_get_event_flag_information);
    PS_LV2_BIND(table, 985, sys_dbg_get_console_type);
}

} // namespace pspocket::lv2
