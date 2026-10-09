// lv2 kernel core: ID tables, wait queues, parking, guest pointer checks, the syscall dispatcher, process exit.
// References: RPCS3 rpcs3/Emu/IdManager.h/.cpp (IDs), Emu/Cell/lv2/sys_sync.h (lv2_obj, protocols, schedule),
// Emu/Cell/lv2/lv2.cpp (syscall table, null_func/uns_func), Emu/Cell/ErrorCodes.h.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <setjmp.h>
#include <thread>

#include "internal.hpp"
#include "spu.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::lv2 {

// The innermost run_guest of this thread: where a non-local jump in translated code goes back to (D-213).
struct NonLocalTarget {
    jmp_buf buf;
    NonLocalTarget* outer = nullptr;
};
thread_local NonLocalTarget* t_nonlocal = nullptr;

// ------------------------------------------------------------------------------------------------- error names

const char* error_name(Result r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_EAGAIN: return "CELL_EAGAIN";
    case CELL_EINVAL: return "CELL_EINVAL";
    case CELL_ENOSYS: return "CELL_ENOSYS";
    case CELL_ENOMEM: return "CELL_ENOMEM";
    case CELL_ESRCH: return "CELL_ESRCH";
    case CELL_ENOENT: return "CELL_ENOENT";
    case CELL_ENOEXEC: return "CELL_ENOEXEC";
    case CELL_EDEADLK: return "CELL_EDEADLK";
    case CELL_EPERM: return "CELL_EPERM";
    case CELL_EBUSY: return "CELL_EBUSY";
    case CELL_ETIMEDOUT: return "CELL_ETIMEDOUT";
    case CELL_EABORT: return "CELL_EABORT";
    case CELL_EFAULT: return "CELL_EFAULT";
    case CELL_ENOCHILD: return "CELL_ENOCHILD";
    case CELL_ESTAT: return "CELL_ESTAT";
    case CELL_EALIGN: return "CELL_EALIGN";
    case CELL_EKRESOURCE: return "CELL_EKRESOURCE";
    case CELL_EISDIR: return "CELL_EISDIR";
    case CELL_ECANCELED: return "CELL_ECANCELED";
    case CELL_EEXIST: return "CELL_EEXIST";
    case CELL_EISCONN: return "CELL_EISCONN";
    case CELL_ENOTCONN: return "CELL_ENOTCONN";
    case CELL_EAUTHFAIL: return "CELL_EAUTHFAIL";
    case CELL_ENOTMSELF: return "CELL_ENOTMSELF";
    case CELL_ESYSVER: return "CELL_ESYSVER";
    case CELL_EAUTHFATAL: return "CELL_EAUTHFATAL";
    case CELL_EDOM: return "CELL_EDOM";
    case CELL_ERANGE: return "CELL_ERANGE";
    case CELL_EILSEQ: return "CELL_EILSEQ";
    case CELL_EFPOS: return "CELL_EFPOS";
    case CELL_EINTR: return "CELL_EINTR";
    case CELL_EFBIG: return "CELL_EFBIG";
    case CELL_EMLINK: return "CELL_EMLINK";
    case CELL_ENFILE: return "CELL_ENFILE";
    case CELL_ENOSPC: return "CELL_ENOSPC";
    case CELL_ENOTTY: return "CELL_ENOTTY";
    case CELL_EPIPE: return "CELL_EPIPE";
    case CELL_EROFS: return "CELL_EROFS";
    case CELL_ESPIPE: return "CELL_ESPIPE";
    case CELL_E2BIG: return "CELL_E2BIG";
    case CELL_EACCES: return "CELL_EACCES";
    case CELL_EBADF: return "CELL_EBADF";
    case CELL_EIO: return "CELL_EIO";
    case CELL_EMFILE: return "CELL_EMFILE";
    case CELL_ENODEV: return "CELL_ENODEV";
    case CELL_ENOTDIR: return "CELL_ENOTDIR";
    case CELL_ENXIO: return "CELL_ENXIO";
    case CELL_EXDEV: return "CELL_EXDEV";
    case CELL_EBADMSG: return "CELL_EBADMSG";
    case CELL_EINPROGRESS: return "CELL_EINPROGRESS";
    case CELL_EMSGSIZE: return "CELL_EMSGSIZE";
    case CELL_ENAMETOOLONG: return "CELL_ENAMETOOLONG";
    case CELL_ENOLCK: return "CELL_ENOLCK";
    case CELL_ENOTEMPTY: return "CELL_ENOTEMPTY";
    case CELL_ENOTSUP: return "CELL_ENOTSUP";
    case CELL_EFSSPECIFIC: return "CELL_EFSSPECIFIC";
    case CELL_EOVERFLOW: return "CELL_EOVERFLOW";
    case CELL_ENOTMOUNTED: return "CELL_ENOTMOUNTED";
    case CELL_ENOTSDATA: return "CELL_ENOTSDATA";
    case CELL_ESDKVER: return "CELL_ESDKVER";
    case CELL_ENOLICDISC: return "CELL_ENOLICDISC";
    case CELL_ENOLICENT: return "CELL_ENOLICENT";
    default: return "CELL_E?";
    }
}

// ------------------------------------------------------------------------------------------------- IdTable

IdTable::IdTable(IdSpec spec) : spec_(spec), slots_(spec.count) {}

static u32 invl_mask(const IdSpec& spec)
{
    return spec.invl_bits ? ((1u << spec.invl_bits) - 1) << spec.invl_shift : 0;
}

u32 IdTable::index_of(u32 base, u32 id) const
{
    // RPCS3 id_manager::get_index: an ID below the base wraps to a huge index and is rejected.
    const u32 diff = (id & ~invl_mask(spec_)) - base;
    if (diff % spec_.step) return spec_.count;
    return diff / spec_.step;
}

u32 IdTable::insert(u32 base, std::shared_ptr<Object> obj)
{
    std::lock_guard lock(mutex_);
    const u32 mask = invl_mask(spec_);
    u32 index = spec_.count;
    u32 id = 0;
    if (!spec_.lowest && highest_ < spec_.count) {
        // Fresh slots first (idm::allocate_id "emplace back"): IDs are not reused until the table has filled once.
        index = highest_++;
        id = base + spec_.step * index;
    } else {
        for (u32 i = 0; i < spec_.count; i++) {
            if (!slots_[i].used) {
                index = i;
                id = (base + spec_.step * i) | ((slots_[i].key + (1u << spec_.invl_shift)) & mask);
                break;
            }
        }
        if (index == spec_.count) return 0;
    }
    Slot& slot = slots_[index];
    slot.obj = std::move(obj);
    slot.base = base;
    slot.key = id;
    slot.used = true;
    return id;
}

std::shared_ptr<Object> IdTable::find(u32 base, u32 id) const
{
    const u32 index = index_of(base, id);
    if (index >= spec_.count) return nullptr;
    std::lock_guard lock(mutex_);
    const Slot& slot = slots_[index];
    if (!slot.used || slot.base != base || slot.key != id) return nullptr;
    return slot.obj;
}

bool IdTable::remove(u32 base, u32 id)
{
    const u32 index = index_of(base, id);
    if (index >= spec_.count) return false;
    std::shared_ptr<Object> doomed; // destroyed outside the table lock
    std::lock_guard lock(mutex_);
    Slot& slot = slots_[index];
    if (!slot.used || slot.base != base || slot.key != id) return false;
    doomed = std::move(slot.obj);
    slot.used = false;
    slot.base = 0;
    return true;
}

u32 IdTable::count(u32 base) const
{
    std::lock_guard lock(mutex_);
    return static_cast<u32>(std::count_if(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.used && s.base == base; }));
}

std::vector<std::pair<u32, std::shared_ptr<Object>>> IdTable::list(u32 base) const
{
    std::vector<std::pair<u32, std::shared_ptr<Object>>> out;
    std::lock_guard lock(mutex_);
    for (const Slot& s : slots_) {
        if (s.used && s.base == base) out.emplace_back(s.key, s.obj);
    }
    return out;
}

std::vector<std::shared_ptr<Object>> IdTable::all() const
{
    std::vector<std::shared_ptr<Object>> out;
    std::lock_guard lock(mutex_);
    for (const Slot& s : slots_) {
        if (s.used) out.push_back(s.obj);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- WaitQueue

namespace {
std::atomic<u64> g_wait_order{0};
}

void WaitQueue::push(Waiter* w)
{
    w->order = ++g_wait_order;
    items_.push_back(w);
}

usize WaitQueue::select(u32 protocol) const
{
    // RPCS3 lv2_obj::schedule: FIFO takes the earliest waiter; every other protocol (PRIORITY, PRIORITY_INHERIT,
    // RETRY) takes the lowest priority value, the earliest among equals. Items stay in push order.
    if (protocol == SYS_SYNC_FIFO) return 0;
    usize best = 0;
    s32 best_prio = items_[0]->prio.load(std::memory_order_relaxed);
    for (usize i = 1; i < items_.size(); i++) {
        const s32 p = items_[i]->prio.load(std::memory_order_relaxed);
        if (p < best_prio) {
            best = i;
            best_prio = p;
        }
    }
    return best;
}

Waiter* WaitQueue::pop(u32 protocol)
{
    if (items_.empty()) return nullptr;
    const usize i = select(protocol);
    Waiter* w = items_[i];
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
    return w;
}

Waiter* WaitQueue::peek(u32 protocol) const
{
    if (items_.empty()) return nullptr;
    return items_[select(protocol)];
}

bool WaitQueue::remove(const Waiter* w)
{
    const auto it = std::find(items_.begin(), items_.end(), w);
    if (it == items_.end()) return false;
    items_.erase(it);
    return true;
}

bool WaitQueue::contains(const Waiter* w) const { return std::find(items_.begin(), items_.end(), w) != items_.end(); }

std::vector<Waiter*> WaitQueue::take_all(u32 protocol)
{
    std::vector<Waiter*> out;
    out.reserve(items_.size());
    while (Waiter* w = pop(protocol)) out.push_back(w);
    return out;
}

// ------------------------------------------------------------------------------------------------- parking

void prepare_wait(Waiter& w)
{
    std::lock_guard lock(w.park_mutex);
    w.signaled = false;
}

void wake(Waiter& w, Result result)
{
    // Notify under the lock: once the waiter can observe `signaled` it may return and its thread may end, so the
    // waker must not touch the Waiter after releasing the lock.
    std::lock_guard lock(w.park_mutex);
    w.result = result;
    w.signaled = true;
    w.park_cv.notify_all();
}

void abort_wait(Waiter& w)
{
    std::lock_guard lock(w.park_mutex);
    w.aborted = true;
    w.park_cv.notify_all();
}

bool wait_aborted(Waiter& w)
{
    std::lock_guard lock(w.park_mutex);
    return w.aborted;
}

std::chrono::steady_clock::time_point deadline_after_us(std::chrono::steady_clock::time_point now, u64 timeout_us)
{
    constexpr u64 kMaxUs = 1'000'000'000'000'000ull; // ~31 years
    return now + std::chrono::microseconds(static_cast<s64>(std::min(timeout_us, kMaxUs)));
}

std::chrono::steady_clock::time_point deadline_after_us(u64 timeout_us)
{
    return deadline_after_us(std::chrono::steady_clock::now(), timeout_us);
}

u32 yield_backoff(PpuThread& t, u64 now_ns)
{
    if (now_ns - t.last_yield_ns < 50'000) {
        t.yield_streak++;
    } else {
        t.yield_streak = 0;
        t.yield_backoff_us = 0;
    }
    if (t.yield_streak < 16) return 0;
    t.yield_backoff_us = t.yield_backoff_us ? std::min<u32>(t.yield_backoff_us * 2, 200) : 20;
    return t.yield_backoff_us;
}

bool yield_wait(PpuThread& t)
{
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    bool alive = true;
    if (const u32 us = yield_backoff(t, now)) alive = sleep_us(t.waiter, us);
    else std::this_thread::yield();
    t.last_yield_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    return alive;
}

bool sleep_us(Waiter& w, u64 us)
{
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = deadline_after_us(now, us);
    w.begin_wait({WaitKind::Sleep, 0}, now, deadline);
    std::unique_lock lock(w.park_mutex);
    w.park_cv.wait_until(lock, deadline, [&] { return w.aborted; });
    w.end_wait();
    return !w.aborted;
}

const char* wait_kind_name(WaitKind kind)
{
    switch (kind) {
    case WaitKind::None: return "nothing";
    case WaitKind::Mutex: return "mutex";
    case WaitKind::Cond: return "cond";
    case WaitKind::RwLockRead: return "rwlock (read)";
    case WaitKind::RwLockWrite: return "rwlock (write)";
    case WaitKind::Semaphore: return "semaphore";
    case WaitKind::EventQueue: return "event queue";
    case WaitKind::EventFlag: return "event flag";
    case WaitKind::LwMutex: return "lwmutex";
    case WaitKind::LwCond: return "lwcond";
    case WaitKind::Join: return "thread join";
    case WaitKind::Sleep: return "sleep";
    case WaitKind::Usbd: return "USB events";
    case WaitKind::Net: return "network";
    }
    return "?";
}

// ------------------------------------------------------------------------------------------------- Guest

bool Guest::valid(u32 addr, u64 size) const
{
    if (addr == 0) return false;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    static const u64 page = GuestMemory::host_page_size();
    const u64 end = u64{addr} + std::max<u64>(size, 1);
    for (u64 p = addr & ~(page - 1); p < end; p += page) {
        if (!mem_.is_committed(static_cast<u32>(p))) return false;
    }
    return true;
}

bool Guest::put32(u32 a, u32 v) const
{
    if (!valid(a, 4)) return false;
    w32(a, v);
    return true;
}

bool Guest::put64(u32 a, u64 v) const
{
    if (!valid(a, 8)) return false;
    w64(a, v);
    return true;
}

Guest::Str Guest::read_cstr(u32 addr, usize max_len, std::string& out) const
{
    out.clear();
    if (addr == 0) return Str::Fault;
    static const u64 page = GuestMemory::host_page_size();
    u64 checked_end = 0; // first address not yet known committed
    for (usize i = 0; i <= max_len; i++) {
        const u64 a = u64{addr} + i;
        if (a >= GuestMemory::kGuestSize) return Str::Fault;
        if (a >= checked_end) {
            if (!mem_.is_committed(static_cast<u32>(a))) return Str::Fault;
            checked_end = (a & ~(page - 1)) + page;
        }
        const char c = static_cast<char>(r8(static_cast<u32>(a)));
        if (c == '\0') return Str::Ok;
        if (i == max_len) return Str::TooLong;
        out.push_back(c);
    }
    return Str::TooLong;
}

// ------------------------------------------------------------------------------------------------- syscall table

namespace {

struct NamedSyscall {
    u32 number;
    const char* name;
};

constexpr NamedSyscall kSyscallNames[] = {
#include "syscall_table.inc"
};

const std::array<const char*, kSyscallCount>& name_table()
{
    static const std::array<const char*, kSyscallCount> table = [] {
        std::array<const char*, kSyscallCount> t{};
        for (const NamedSyscall& n : kSyscallNames) t[n.number] = n.name;
        return t;
    }();
    return table;
}

std::atomic<Kernel*> g_current_kernel{nullptr};
thread_local PpuThread* g_current_thread = nullptr;

bool should_log_repeat(u32 n) { return n <= 8 || (n % 1024) == 0; }

} // namespace

const char* syscall_name(u32 number) { return number < kSyscallCount ? name_table()[number] : nullptr; }

SyscallTable::SyscallTable()
{
    for (u32 i = 0; i < kSyscallCount; i++) entries_[i].name = syscall_name(i);
}

void SyscallTable::bind(u32 number, const char* name, SyscallFn fn)
{
    if (number >= kSyscallCount) PS_FATAL(kChan, "syscall {} ({}) out of range", number, name);
    const char* expected = syscall_name(number);
    if (!expected || std::strcmp(expected, name) != 0) {
        PS_FATAL(kChan, "syscall {} is '{}' in the name table but is bound to '{}'", number, expected ? expected : "?", name);
    }
    entries_[number].fn = fn;
}

namespace {

std::array<const char*, kSyscallCount> g_null_what{};
std::array<std::atomic<bool>, kSyscallCount> g_null_logged{};

void null_syscall(PpuThread& t, PPUContext& ctx)
{
    const u32 number = static_cast<u32>(ctx.gpr[11]);
    if (!g_null_logged[number].exchange(true)) {
        PS_LOG_WARN(kChan, "syscall {} ({}) is not implemented; returns CELL_OK as RPCS3's null_func does (first call "
                           "at {:#010x}, r3={:#x} r4={:#x}, thread {:#x})",
            number, g_null_what[number], ctx.cia, ctx.gpr[3], ctx.gpr[4], t.id);
    }
    ctx.gpr[3] = 0;
}

std::array<u64, kSyscallCount> g_stub_r3{};

void stub_syscall(PpuThread& t, PPUContext& ctx)
{
    const u32 number = static_cast<u32>(ctx.gpr[11]);
    if (!g_null_logged[number].exchange(true)) {
        PS_LOG_WARN(kChan, "syscall {} ({}) is a stub in RPCS3; returns {:#x} as RPCS3 does (first call at {:#010x}, "
                           "r3={:#x} r4={:#x}, thread {:#x})",
            number, g_null_what[number], g_stub_r3[number], ctx.cia, ctx.gpr[3], ctx.gpr[4], t.id);
    }
    ctx.gpr[3] = g_stub_r3[number];
}

} // namespace

void SyscallTable::bind_stub(u32 number, const char* name, u64 r3)
{
    if (number >= kSyscallCount) PS_FATAL(kChan, "syscall {} ({}) out of range", number, name);
    if (entries_[number].fn) PS_FATAL(kChan, "syscall {} is already bound", number);
    g_null_what[number] = name;
    g_stub_r3[number] = r3;
    entries_[number].fn = stub_syscall;
    if (!entries_[number].name) entries_[number].name = name;
}

void SyscallTable::bind_null(u32 number, const char* what)
{
    if (number >= kSyscallCount) PS_FATAL(kChan, "syscall {} ({}) out of range", number, what);
    if (entries_[number].fn) PS_FATAL(kChan, "syscall {} is already bound", number);
    g_null_what[number] = what;
    entries_[number].fn = null_syscall;
    if (!entries_[number].name) entries_[number].name = what;
}

PpuThread* current_thread() { return g_current_thread; }
void set_current_thread(PpuThread* t) { g_current_thread = t; }

// ------------------------------------------------------------------------------------------------- Kernel

u32 default_user_memory_size(u32 sdk_version)
{
    // RPCS3 PPUModule.cpp ppu_load_exec ("Initialize memory stats (according to sdk version)").
    if (sdk_version > 0x0021FFFF) return 0xD500000;
    if (sdk_version > 0x0019FFFF) return 0xD300000;
    if (sdk_version > 0x0018FFFF) return 0xD100000;
    if (sdk_version > 0x0017FFFF) return 0xD000000;
    if (sdk_version > 0x00154FFF) return 0xCC00000;
    return 0xC800000;
}

Kernel::Impl::Impl(Kernel& k)
    : kernel(k)
    , guest(k.memory())
    , memory(k, k.config().user_memory_size ? k.config().user_memory_size : default_user_memory_size(k.config().sdk_version),
          k.config().main_area_end)
{
    register_process_syscalls(table);
    register_thread_syscalls(table);
    register_sync_syscalls(table);
    register_event_syscalls(table);
    register_lw_syscalls(table);
    register_time_syscalls(table);
    register_memory_syscalls(table);
    register_fs_syscalls(table);
    register_vm_syscalls(table);
    register_spu_syscalls(table);
    register_system_syscalls(table);
    register_rsx_syscalls(table);
    register_usbd_syscalls(table);
    register_net_syscalls(table);
    register_device_syscalls(table);
    register_service_syscalls(table);
    register_misc_syscalls(table);
    register_prx_syscalls(table);
    register_rsxaudio_syscalls(table);
    register_raw_spu_syscalls(table);
    // Every other slot RPCS3 answers with null_func (a log, then CELL_OK) is answered so too (D-166): a game calling
    // one goes on instead of meeting ENOSYS. GTA V's CRT, for one, registers a debug exception handler (988).
    static constexpr struct {
        u32 number;
        const char* what;
    } kNull[] = {
#include "syscall_null.inc"
    };
    for (const auto& n : kNull)
        if (!table[n.number].fn) table.bind_null(n.number, n.what);
    // And every syscall RPCS3 binds to a stub (a log, then a constant) answers that constant (D-184).
    static constexpr struct {
        u32 number;
        const char* name;
        u64 r3;
    } kStub[] = {
#include "syscall_stub.inc"
    };
    for (const auto& n : kStub)
        if (!table[n.number].fn) table.bind_stub(n.number, n.name, n.r3);
}

Kernel::Impl::~Impl() = default;

void Kernel::Impl::abort_all_waits()
{
    for (const auto& [id, obj] : threads.list(PpuThread::kIdBase)) abort_wait(static_cast<PpuThread&>(*obj).waiter);
    abort_spu_threads(*this);
    abort_raw_spus(*this);
}

void Kernel::Impl::process_exit(s32 status)
{
    bool first = false;
    {
        std::lock_guard lock(exit_mutex);
        if (!exit_requested) {
            exit_requested = true;
            exit_status = status;
            first = true;
        }
    }
    exiting = true;
    if (first) {
        PS_LOG_INFO(kChan, "process exit (status {})", status);
        // The host hears about the exit before wait_for_exit() returns.
        if (kernel.config().on_process_exit) kernel.config().on_process_exit(status);
    }
    abort_all_waits();
    {
        std::lock_guard lock(exit_mutex);
        exited = true;
    }
    exit_cv.notify_all();
}

Kernel::Kernel(GuestMemory& memory, Config config) : memory_(memory), config_(std::move(config))
{
    PS_CHECK(memory_.reserved());
    if (!config_.executor) config_.executor = default_executor;
    impl_ = std::make_unique<Impl>(*this);
}

Kernel::~Kernel()
{
    if (!shutdown()) {
        PS_FATAL(kChan, "kernel destroyed while guest threads still run guest code (they never reached a syscall)");
    }
    Kernel* self = this;
    if (g_current_kernel.compare_exchange_strong(self, nullptr)) ps_ppu_set_syscall_handler(nullptr);
}

void Kernel::install()
{
    g_current_kernel = this;
    ps_ppu_set_syscall_handler(ps_lv2_syscall);
}

Kernel* Kernel::current() { return g_current_kernel.load(); }

bool Kernel::vfs_mount(std::string_view point, std::string_view host_dir, bool read_only)
{
    return impl_->vfs.mount(point, host_dir, read_only);
}

bool Kernel::vfs_mount_image(std::string_view point, std::shared_ptr<const VfsImage> image, std::string_view subdir)
{
    return impl_->vfs.mount_image(point, std::move(image), subdir);
}

void Kernel::vfs_unmount(std::string_view point) { impl_->vfs.unmount(point); }

std::string Kernel::vfs_host_path(std::string_view vpath) const
{
    const Vfs::Resolved r = impl_->vfs.resolve(vpath);
    if (!r.mount || r.mount->image) return {};
    return r.host;
}

void Kernel::set_npd_opener(NpdOpener opener)
{
    std::lock_guard lock(impl_->npd_mutex);
    impl_->npd_opener = std::move(opener);
}

void Kernel::set_spu_program_identifier(SpuProgramIdentifier identifier)
{
    std::lock_guard lock(impl_->spu_hooks_mutex);
    impl_->spu_identifier = std::move(identifier);
}

void Kernel::set_self_decrypter(SelfDecrypter decrypter)
{
    std::lock_guard lock(impl_->spu_hooks_mutex);
    impl_->self_decrypter = std::move(decrypter);
}

bool Kernel::vfs_read_file(std::string_view vpath, std::vector<u8>& out) const
{
    const Vfs::Resolved r = impl_->vfs.resolve(vpath);
    if (!r.mount) return false;
    if (r.mount->image) {
        const auto file = r.mount->image->open(r.image_path);
        if (!file) return false;
        out.resize(static_cast<usize>(file->size()));
        return file->read(0, out.data(), out.size());
    }
    std::FILE* f = std::fopen(r.host.c_str(), "rb");
    if (!f) return false;
    out.clear();
    u8 buf[1 << 16];
    for (usize n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

std::optional<u64> Kernel::vfs_tree_size(std::string_view vpath, u64 rounding) const
{
    const auto align = [rounding](u64 size) { return rounding > 1 ? (size + rounding - 1) / rounding * rounding : size; };
    const Vfs::Resolved r = impl_->vfs.resolve(vpath);
    if (!r.mount) return std::nullopt;
    if (r.mount->image) {
        const VfsImage& image = *r.mount->image;
        const auto walk = [&](const auto& self, const std::string& path) -> std::optional<u64> {
            const auto entries = image.list(path);
            if (!entries) return std::nullopt;
            u64 total = 0;
            for (const auto& [name, st] : *entries) {
                if (!st.dir) {
                    total += align(st.size);
                } else if (const auto sub = self(self, path.empty() ? name : path + "/" + name)) {
                    total += *sub;
                } else {
                    return std::nullopt;
                }
            }
            return total;
        };
        return walk(walk, std::string(r.image_path));
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(r.host, ec)) return std::nullopt;
    u64 total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(r.host, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += align(it->file_size(ec));
    }
    if (ec) return std::nullopt;
    return total;
}

std::shared_ptr<PpuThread> Kernel::thread(u32 id) const { return impl_->threads.get<PpuThread>(id); }

std::shared_ptr<EventQueue> Kernel::event_queue(u32 id) const
{
    auto q = impl_->objects.get<EventQueue>(id);
    if (!q) return nullptr;
    std::lock_guard lk(q->lock);
    return q->dead ? nullptr : q;
}

std::shared_ptr<EventQueue> Kernel::event_queue_by_key(u64 key) const
{
    if (key == 0) return nullptr;
    std::shared_ptr<EventQueue> q;
    {
        std::lock_guard lk(impl_->ipc_mutex);
        const auto it = impl_->ipc.find({EventQueue::kIdBase, key});
        if (it == impl_->ipc.end()) return nullptr;
        q = std::static_pointer_cast<EventQueue>(it->second.lock());
    }
    if (!q) return nullptr;
    std::lock_guard lk(q->lock);
    return q->dead ? nullptr : q;
}

std::shared_ptr<EventFlag> Kernel::event_flag(u32 id) const { return impl_->objects.get<EventFlag>(id); }

bool Kernel::exited() const
{
    std::lock_guard lock(impl_->exit_mutex);
    return impl_->exited;
}

s32 Kernel::exit_status() const
{
    std::lock_guard lock(impl_->exit_mutex);
    return impl_->exit_status;
}

bool Kernel::wait_for_exit(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->exit_mutex);
    if (timeout == std::chrono::milliseconds::max()) {
        impl_->exit_cv.wait(lock, [&] { return impl_->exited; });
    } else {
        impl_->exit_cv.wait_for(lock, timeout, [&] { return impl_->exited; });
    }
    return impl_->exited;
}

bool Kernel::shutdown(std::chrono::milliseconds timeout)
{
    impl_->exiting = true;
    impl_->abort_all_waits();
    impl_->timers.stop();
    impl_->rsx.stop();
    HostThreads& host = *impl_->host_threads;
    std::unique_lock lock(host.mutex);
    const bool done = host.cv.wait_for(lock, timeout, [&] { return host.live == 0; });
    if (!done) PS_LOG_ERROR(kChan, "shutdown: {} guest thread(s) still running", host.live);
    return done;
}

u64 Kernel::unimplemented_count() const { return impl_->unimplemented.load(); }

bool Kernel::syscall_bound(u32 number) const { return number < kSyscallCount && impl_->table[number].fn != nullptr; }

rsx::Rsx* Kernel::rsx()
{
    std::lock_guard lock(impl_->rsx.mutex);
    return impl_->rsx.rsx.get();
}

void Kernel::set_module_host(ModuleHost* host) { impl_->module_host.store(host, std::memory_order_release); }

ModuleHost* Kernel::module_host() const { return impl_->module_host.load(std::memory_order_acquire); }

void Kernel::set_rsx_flip_listener(std::function<void(u32 buffer_id)> fn)
{
    std::lock_guard lock(impl_->rsx.listener_mutex);
    impl_->rsx.on_flip = std::move(fn);
}

void Kernel::set_rsx_user_command_listener(std::function<void(u32 arg)> fn)
{
    std::lock_guard lock(impl_->rsx.listener_mutex);
    impl_->rsx.on_user_command = std::move(fn);
}

void Kernel::set_rsx_queue_listener(std::function<void(u32 head, u32 buffer_id)> fn)
{
    std::lock_guard lock(impl_->rsx.listener_mutex);
    impl_->rsx.on_queue = std::move(fn);
}

void Kernel::set_syscall_observer(SyscallObserver fn, void* user)
{
    impl_->syscall_observer_user = user;
    impl_->syscall_observer.store(fn, std::memory_order_release);
}

void Kernel::syscall(PPUContext& ctx)
{
    auto* t = static_cast<PpuThread*>(ctx.runtime);
    if (!t) PS_FATAL(kChan, "syscall {} at {:#010x} from a PPU context with no lv2 thread", ctx.gpr[11], ctx.cia);
    if (impl_->exiting.load(std::memory_order_relaxed)) throw ThreadExit{};

    if (ctx.cia == kNonLocalReturnCia) { // D-161: the lifter's, not the guest's
        if (t_nonlocal) _longjmp(t_nonlocal->buf, 1); // D-213: nothing between here and run_guest to destroy
        throw NonLocalJump{};
    }
    const u64 number = ctx.gpr[11];
    if (const auto observer = impl_->syscall_observer.load(std::memory_order_relaxed)) [[unlikely]] {
        observer(impl_->syscall_observer_user, static_cast<u32>(number), &ctx.gpr[3]);
    }
    if (number < kSyscallCount) {
        const SyscallEntry& entry = impl_->table[static_cast<u32>(number)];
        if (entry.fn) {
            PS_LOG_TRACE(kChan, "{} ({}) r3={:#x} r4={:#x} r5={:#x} r6={:#x} [thread {:#x}]", entry.name, number,
                ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], t->id);
            // For reports: the syscall this thread is in (cleared on return and when the thread unwinds). Release
            // order publishes the registers as they were at the `sc`.
            const struct InSyscall {
                PpuThread& thread;
                InSyscall(PpuThread& th, u32 n) : thread(th) { th.current_syscall.store(n, std::memory_order_release); }
                ~InSyscall() { thread.current_syscall.store(kNoSyscall, std::memory_order_relaxed); }
            } in_syscall(*t, static_cast<u32>(number));
            entry.fn(*t, ctx);
            PS_LOG_TRACE(kChan, "{} -> r3={:#x}", entry.name, ctx.gpr[3]);
            return;
        }
    }

    // CLAUDE.md rule 4: log the number and name, return an error, never succeed silently.
    impl_->unimplemented++;
    const char* name = syscall_name(static_cast<u32>(std::min<u64>(number, kSyscallCount)));
    const u32 n = number < kSyscallCount ? ++impl_->unimplemented_by_number[number] : 1;
    if (should_log_repeat(n)) {
        PS_LOG_ERROR(kChan, "UNIMPLEMENTED syscall {} ({}) at {:#010x}: r3={:#x} r4={:#x} r5={:#x} r6={:#x} -> "
                            "CELL_ENOSYS (call #{})",
            number, name ? name : (number < kSyscallCount ? "unnamed" : "invalid"), ctx.cia, ctx.gpr[3], ctx.gpr[4],
            ctx.gpr[5], ctx.gpr[6], n);
    }
    if (config_.abort_on_unimplemented) {
        PS_FATAL(kChan, "UNIMPLEMENTED syscall {} ({}) (abort_on_unimplemented)", number, name ? name : "?");
    }
    ctx.gpr[3] = to_r3(err(CELL_ENOSYS));
}

void default_executor(void*, PPUContext& ctx, u8* base, u32 entry) { ps_ppu_call_indirect(&ctx, base, entry); }

void run_guest(Kernel& k, PPUContext& ctx, u8* base, u32 entry, bool thread_top)
{
    const u64 stop_lr = ctx.lr, stop_sp = ctx.gpr[1];
    // Where a non-local jump in the guest code below comes back to (D-213); the outer one again when this returns or
    // a C++ exception (ThreadExit) leaves.
    NonLocalTarget target;
    target.outer = t_nonlocal;
    t_nonlocal = &target;
    const struct Pop {
        NonLocalTarget* outer;
        ~Pop() { t_nonlocal = outer; }
    } pop{target.outer};
    if (_setjmp(target.buf) == 0) {
        k.config().executor(k.config().executor_user, ctx, base, entry);
        return;
    }
    // The context the jump restored is in ctx: it continues at LR, in the landing's continuation when the game's
    // preparation translated one (D-255), else interpreted (translated callees still run as such), until the code
    // returns where this call would have; a further jump in translated code lands here again.
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        PS_LOG_INFO(kChan, "a non-local jump (longjmp or a fiber switch) to {:#010x}, r1 {:#x}: interpreting from there "
                           "(D-161), or in its continuation (D-255)", static_cast<u32>(ctx.lr), ctx.gpr[1]);
    ppu::RunOptions options;
    options.stop_at_return = true;
    options.stop_lr = stop_lr;
    options.stop_sp = stop_sp;
    options.stop_any_sp = thread_top;
    options.call_lifted = true;
    options.lifted_first = true; // the landing's continuation, when it was translated (D-255)
    for (;;) {
        ctx.cia = static_cast<u32>(ctx.lr) & ~3u;
        if (_setjmp(target.buf) == 0) {
            ppu::run(ctx, base, options);
            return;
        }
    }
}

} // namespace pspocket::lv2

extern "C" void ps_lv2_syscall(PPUContext* ctx, uint8_t*)
{
    using namespace pspocket::lv2;
    auto* t = static_cast<PpuThread*>(ctx->runtime);
    Kernel* k = t ? &t->kernel : Kernel::current();
    if (!k) PS_FATAL(kChan, "syscall {} at {:#010x}: no lv2 kernel installed", ctx->gpr[11], ctx->cia);
    k->syscall(*ctx);
}
