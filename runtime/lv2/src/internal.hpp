#pragma once

// Internal to runtime/lv2: guest memory helpers, the syscall table and binding, and the kernel's subsystems.

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/rsx/backend.hpp"
#include "pspocket/rsx/rsx.hpp"

namespace pspocket::lv2 {

inline constexpr std::string_view kChan = "lv2";

// ---------------------------------------------------------------------------------------------------- guest memory

// Guest pointer checks and big-endian access for syscall arguments. A pointer is valid when it is non-null, inside
// the 4 GB space and every host page it touches is committed (an lv2 call on a bad pointer returns CELL_EFAULT
// instead of faulting the host).
class Guest {
public:
    explicit Guest(GuestMemory& memory) : mem_(memory), base_(memory.base()) {}

    bool valid(u32 addr, u64 size) const;
    u8* ptr(u32 addr) const { return base_ + addr; }
    u8* base() const { return base_; }

    u8 r8(u32 a) const { return ps_read8(base_, a); }
    u16 r16(u32 a) const { return ps_read16(base_, a); }
    u32 r32(u32 a) const { return ps_read32(base_, a); }
    u64 r64(u32 a) const { return ps_read64(base_, a); }
    void w8(u32 a, u8 v) const { ps_write8(base_, a, v); }
    void w16(u32 a, u16 v) const { ps_write16(base_, a, v); }
    void w32(u32 a, u32 v) const { ps_write32(base_, a, v); }
    void w64(u32 a, u64 v) const { ps_write64(base_, a, v); }

    // Checked single writes: false (and nothing written) when the pointer is invalid.
    bool put32(u32 a, u32 v) const;
    bool put64(u32 a, u64 v) const;

    enum class Str { Ok, Fault, TooLong };
    // Reads a NUL-terminated string of at most `max_len` characters.
    Str read_cstr(u32 addr, usize max_len, std::string& out) const;

private:
    GuestMemory& mem_;
    u8* base_;
};

// ---------------------------------------------------------------------------------------------------- syscalls

using SyscallFn = void (*)(PpuThread& t, PPUContext& ctx);

struct SyscallEntry {
    SyscallFn fn = nullptr;
    const char* name = nullptr;
};

inline constexpr u32 kSyscallCount = 1024; // RPCS3 lv2.cpp g_ppu_syscall_table

template <class T>
T reg_arg(u64 v)
{
    if constexpr (std::is_same_v<T, u64>) {
        return v;
    } else if constexpr (std::is_same_v<T, s64>) {
        return static_cast<s64>(v);
    } else if constexpr (std::is_same_v<T, u32>) {
        return static_cast<u32>(v);
    } else if constexpr (std::is_same_v<T, s32>) {
        return static_cast<s32>(static_cast<u32>(v));
    } else {
        static_assert(sizeof(T) == 0, "unsupported syscall argument type");
    }
}

template <class R, class... A, usize... I>
void invoke_syscall(R (*fn)(PpuThread&, A...), PpuThread& t, PPUContext& ctx, std::index_sequence<I...>)
{
    static_assert(sizeof...(A) <= 8, "syscalls take at most 8 register arguments (r3..r10)");
    if constexpr (std::is_void_v<R>) {
        fn(t, reg_arg<A>(ctx.gpr[3 + I])...);
    } else if constexpr (std::is_same_v<R, s32>) {
        ctx.gpr[3] = to_r3(fn(t, reg_arg<A>(ctx.gpr[3 + I])...));
    } else if constexpr (std::is_same_v<R, u32>) {
        ctx.gpr[3] = fn(t, reg_arg<A>(ctx.gpr[3 + I])...);
    } else {
        static_assert(std::is_same_v<R, u64>, "unsupported syscall return type");
        ctx.gpr[3] = fn(t, reg_arg<A>(ctx.gpr[3 + I])...);
    }
}

template <auto Fn>
void syscall_thunk(PpuThread& t, PPUContext& ctx)
{
    using F = decltype(Fn);
    [&]<class R, class... A>(R (*fn)(PpuThread&, A...)) {
        invoke_syscall(fn, t, ctx, std::index_sequence_for<A...>{});
    }(static_cast<F>(Fn));
}

class SyscallTable {
public:
    SyscallTable();
    // Binds a syscall RPCS3 answers with null_func (lv2.cpp): it logs the first call and returns CELL_OK. Only for
    // numbers a game is seen calling; `what` describes the syscall (RPCS3's table comment) for the log.
    void bind_null(u32 number, const char* what);
    // Binds `fn` to `number`; `name` must match the generated name table (catches a wrong number at startup).
    void bind(u32 number, const char* name, SyscallFn fn);
    const SyscallEntry& operator[](u32 number) const { return entries_[number]; }

private:
    std::array<SyscallEntry, kSyscallCount> entries_{};
};

#define PS_LV2_BIND(table, number, fn) (table).bind(number, #fn, &::pspocket::lv2::syscall_thunk<&::pspocket::lv2::fn>)

void register_process_syscalls(SyscallTable& table);
void register_thread_syscalls(SyscallTable& table);
void register_sync_syscalls(SyscallTable& table);
void register_event_syscalls(SyscallTable& table);
void register_lw_syscalls(SyscallTable& table);
void register_time_syscalls(SyscallTable& table);
void register_memory_syscalls(SyscallTable& table);
void register_fs_syscalls(SyscallTable& table);
void register_vm_syscalls(SyscallTable& table);
void register_spu_syscalls(SyscallTable& table);

// ---------------------------------------------------------------------------------------------------- helpers

// A blocking call returned because the kernel aborted the wait (process exit, shutdown): unwind the PPU thread.
inline Result finish(PpuThread& t, Result r)
{
    if (r == err(CELL_EABORT) && wait_aborted(t.waiter)) throw ThreadExit{};
    return r;
}

// lv2_obj::create (RPCS3 sys_sync.h): validates pshared / flags and registers a new object, or another ID for an
// existing process-shared one. `make` builds a new object. On success `id_out` receives the ID.
template <class T, class Make>
Result create_object(Kernel& k, u32 pshared, u64 ipc_key, s32 flags, Make&& make, u32& id_out);

// Destroys the object behind `id` if `check` (called with the object lock held) returns CELL_OK.
template <class T, class Check>
Result destroy_object(Kernel& k, u32 id, Check&& check);

// ---------------------------------------------------------------------------------------------------- memory

// A set of allocated ranges inside [lo, hi), first-fit.
class RangeSet {
public:
    RangeSet(u64 lo, u64 hi) : lo_(lo), hi_(hi) {}
    // Allocates `size` bytes aligned to `align` (power of two). Returns 0 when no gap fits.
    u32 alloc(u64 size, u64 align);
    bool alloc_at(u32 addr, u64 size);
    // Frees the range starting at addr; returns its size (0 if none starts there).
    u64 free(u32 addr);
    // The allocated range containing addr: {start, size}, size 0 if none.
    std::pair<u32, u64> find(u32 addr) const;
    bool empty() const { return used_.empty(); }
    u64 lo() const { return lo_; }
    u64 hi() const { return hi_; }

private:
    u64 lo_, hi_;
    std::map<u64, u64> used_; // start -> size
};

class MemoryContainer final : public Object {
public:
    static constexpr u32 kIdBase = 0x3F000000; // RPCS3 sys_memory.h lv2_memory_container
    u32 id_base() const override { return kIdBase; }
    explicit MemoryContainer(u32 size) : size(size) {}

    const u32 size;
    u32 used = 0; // guarded by the memory manager's mutex

    bool take(u64 amount);
    void give_back(u64 amount);
};

// lv2_memory (sys_mmapper shared memory). The backing pages live in a host memory object (a Mach memory entry on
// Apple platforms) that is mapped into guest memory at each sys_mmapper_map_shared_memory.
class SharedMemory final : public Object {
public:
    static constexpr u32 kIdBase = 0x08000000; // RPCS3 sys_mmapper.h lv2_memory
    u32 id_base() const override { return kIdBase; }
    SharedMemory(u32 size, u32 align, u64 flags, std::shared_ptr<MemoryContainer> ct)
        : size(size), align(align), flags(flags), container(std::move(ct))
    {
    }
    ~SharedMemory() override;

    const u32 size;
    const u32 align; // 0x10000 or 0x100000
    const u64 flags;
    const std::shared_ptr<MemoryContainer> container; // null: the default container
    u32 mappings = 0;           // guarded by the memory manager's mutex
    u64 handle = 0;             // host memory object (created on first map), guarded by the memory manager's mutex
};

// sys_vm block (RPCS3 sys_vm.h sys_vm_t): IDs 1..16 in their own table, one block per 256 MB area.
class VmBlock final : public Object {
public:
    static constexpr u32 kIdBase = 0x1;
    u32 id_base() const override { return kIdBase; }
    VmBlock(u32 addr, u32 size, u32 psize, std::shared_ptr<MemoryContainer> ct)
        : addr(addr), size(size), psize(psize), container(std::move(ct))
    {
    }
    const u32 addr;
    const u32 size;  // virtual size
    u32 psize;       // physical size charged to the container; guarded by the memory manager's mutex
    const std::shared_ptr<MemoryContainer> container; // null: the default container
};
inline constexpr IdSpec kVmIds{1, 16, 0, 0, false};
inline constexpr u32 SYS_MEMORY_CONTAINER_ID_INVALID = 0xFFFFFFFF;
inline constexpr u64 SYS_MEMORY_PAGE_SIZE_64K = 0x200;

// 256 MB areas between the main executable and RSX memory (RPCS3 vm.cpp _find_map, sys_memory.cpp,
// sys_mmapper.cpp): user memory (64 KB / 1 MB pages) and sys_mmapper address ranges.
struct Area {
    enum class Kind { User64k, User1m, Mmapper, Vm, RsxContext };
    Kind kind;
    u32 addr;
    u64 size;
    u32 page_size;
    RangeSet ranges; // allocations (user memory) or mappings (mmapper)
    std::map<u32, std::shared_ptr<SharedMemory>> mapped; // mmapper: mapping start -> memory object
};

class MemoryManager {
public:
    MemoryManager(Kernel& kernel, u32 default_size, u32 main_area_end);
    ~MemoryManager();

    std::mutex mutex; // guards everything below

    MemoryContainer default_container;
    std::vector<std::unique_ptr<Area>> areas;
    struct UserAlloc {
        u64 size;
        std::shared_ptr<MemoryContainer> container; // null: default
        u32 page_size;
    };
    std::map<u32, UserAlloc> user_allocs;
    RangeSet stacks; // PPU thread stacks, PS_MEM_STACK_BASE..+SIZE
    u32 vm_total = 0; // virtual size of all sys_vm blocks
    bool fixed_area_mapped = false;

    // With `mutex` held.
    MemoryContainer& container_or_default(const std::shared_ptr<MemoryContainer>& ct)
    {
        return ct ? *ct : default_container;
    }
    Area* area_at(u32 addr);
    Area* reserve_area(Area::Kind kind, u64 size, u64 align, u32 page_size, u32 fixed_addr = 0);
    // sys_mmapper areas start here: lv2 refuses to free mmapper addresses below it (RPCS3 sys_mmapper_free_address).
    static constexpr u64 kMmapperMin = 0x20000000;
    bool slot_free(u64 addr, u64 size) const;
    u32 allocate_user(u64 size, u32 page_size);

    // Thread stacks: commit [addr, addr + round_up(size)) below a guard page. Returns 0 when the region is full.
    u32 allocate_stack(u32 size, u32& committed);
    void free_stack(u32 addr, u32 committed);

    bool map_shared(SharedMemory& mem, u32 addr);
    void unmap_shared(SharedMemory& mem, u32 addr);

    Kernel& kernel;
    u32 main_area_end;
};

// ---------------------------------------------------------------------------------------------------- RSX

// lv2's RSX state (rsx.cpp, RPCS3 sys_rsx.cpp and the rsx::thread fields it sets): local memory, the context regions,
// tiles and Z-cull regions, and the RSX front end, created by sys_rsx_context_allocate.
struct RsxDevice {
    ~RsxDevice();
    void stop(); // stops the RSX thread (process exit, shutdown)

    std::mutex mutex; // guards everything below except `flips` and the listener
    u32 local_size = 0;    // sys_rsx_memory_allocate (0: no local memory yet)
    u32 area = 0;          // the rsx_context 256 MB area
    u32 device_addr = 0;   // sys_rsx_device_map
    u32 dma_control = 0;   // sys_rsx_context_allocate (0: no context)
    u32 driver_info = 0;
    u32 reports = 0;
    u32 main_mem_size = 0; // IO address space
    bool vsync = false;    // display sync (cellGcmSetFlipMode)
    struct Tile {
        u32 location = 0, offset = 0, size = 0, pitch = 0, comp = 0, base = 0;
        bool bound = false;
    };
    struct Zcull {
        u32 width = 0, height = 0, cull_start = 0, offset = 0;
        bool bound = false;
    };
    std::array<Tile, 15> tiles{};
    std::array<Zcull, 8> zculls{};

    std::atomic<u64> flips{0};
    std::mutex listener_mutex;
    std::function<void(u32)> on_flip; // Kernel::set_rsx_flip_listener
    std::function<void(u32)> on_user_command; // Kernel::set_rsx_user_command_listener
    std::function<void(u32, u32)> on_queue; // Kernel::set_rsx_queue_listener

    std::unique_ptr<rsx::Backend> backend;
    std::unique_ptr<rsx::Rsx> rsx; // after `backend`, so it is destroyed first
};

// ---------------------------------------------------------------------------------------------------- filesystem

struct Mount {
    std::string point;     // "/dev_hdd0"
    std::string host;      // host directory, no trailing slash (empty for an image mount)
    std::shared_ptr<const VfsImage> image; // set: the mount reads this image (always read-only)
    std::string image_root; // the image directory the mount point shows ("" = the image root)
    bool read_only = false;
    u32 block_size = 4096;
    bool no_uid_gid = false;
};

class Vfs {
public:
    bool mount(std::string_view point, std::string_view host_dir, bool read_only);
    bool mount_image(std::string_view point, std::shared_ptr<const VfsImage> image, std::string_view subdir);
    void unmount(std::string_view point);

    struct Resolved {
        const Mount* mount = nullptr; // null: not mounted (or the root "/")
        std::shared_ptr<const Mount> mount_ref; // keeps `mount` alive across a concurrent unmount
        bool root = false;            // the path is "/" itself
        std::string host;             // host path (empty for an image mount)
        std::string image_path;       // image mounts: the path inside the image ("" = the image root)
        std::string vpath;            // normalized guest path
        bool trailing_dot = false;    // ended with "/." or "/.."
    };
    // Normalizes `vpath` ("." and ".." resolved, "/.." of the root is the root) and maps it. Mount table copied
    // under the lock, so concurrent mounts are safe.
    Resolved resolve(std::string_view vpath) const;
    std::vector<std::string> mount_points() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Mount>, std::less<>> mounts_;
};

// ---------------------------------------------------------------------------------------------------- timers

class Timer final : public Object {
public:
    static constexpr u32 kIdBase = 0x11000000; // RPCS3 sys_timer.h lv2_timer
    u32 id_base() const override { return kIdBase; }

    // Guarded by lock.
    bool running = false;
    std::shared_ptr<EventQueue> queue;
    u64 source = 0;
    u64 data1 = 0;
    u64 data2 = 0;
    u64 expire = 0; // guest system time (us)
    u64 period = 0; // 0 = one-shot

    // With `lock` held: fires if due (sends the event, re-arms or stops). Returns the next expiry or ~0.
    u64 check_locked(u64 now);
};

class TimerService {
public:
    TimerService() = default;
    ~TimerService();
    void add(std::shared_ptr<Timer> timer);
    void remove(const Timer* timer);
    void kick(); // a timer was (re)started
    void stop();

private:
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Timer>> timers_;
    std::thread thread_;
    bool stopping_ = false;
    bool kicked_ = false;
};

// ---------------------------------------------------------------------------------------------------- kernel state

// Host threads still running guest code. Shared with every host thread so it outlives the kernel if needed.
struct HostThreads {
    std::mutex mutex;
    std::condition_variable cv;
    u32 live = 0;
};

struct Kernel::Impl {
    explicit Impl(Kernel& k);
    ~Impl();

    Kernel& kernel;
    Guest guest;

    IdTable objects{kLv2ObjIds};
    IdTable threads{kPpuThreadIds};
    IdTable containers{kMemContainerIds};
    IdTable fds{kFsIds};
    IdTable vms{kVmIds};

    // sys_spu_initialize's split of the six SPUs (RPCS3 spu_limits_t).
    struct {
        std::mutex mutex;
        u32 max_raw = 0;
        u32 max_spu = 6;
        s32 group_log = 0; // sys_spu_thread_group_log: 0 on, 1 off
    } spu_limits;

    std::mutex ipc_mutex;
    std::map<std::pair<u32, u64>, std::weak_ptr<Object>> ipc; // (type, key) -> object

    std::mutex join_mutex; // PpuThread join state transitions
    std::shared_ptr<HostThreads> host_threads = std::make_shared<HostThreads>();

    std::atomic<bool> exiting{false};
    mutable std::mutex exit_mutex;
    std::condition_variable exit_cv;
    bool exit_requested = false;
    bool exited = false;
    s32 exit_status = 0;

    MemoryManager memory;
    Vfs vfs;
    std::mutex npd_mutex;
    Kernel::NpdOpener npd_opener; // Kernel::set_npd_opener (D-165), under npd_mutex
    RsxDevice rsx;
    TimerService timers;
    std::mutex tty_mutex;

    SyscallTable table;
    std::atomic<u64> unimplemented{0};
    std::array<std::atomic<u32>, kSyscallCount> unimplemented_by_number{};

    // Aborts every parked thread (process exit, shutdown).
    void abort_all_waits();
    // Records the process exit once and notifies the host.
    void process_exit(s32 status);
};

// Thread internals (thread.cpp).
void thread_finish(PpuThread& t, u64 status);
void set_current_thread(PpuThread* t);

// ---------------------------------------------------------------------------------------------------- templates

// The lock that guards an object's `dead` / `refs`: its own, except a condition variable, which shares its mutex's.
inline std::mutex& object_lock(Object& o) { return o.lock; }
inline std::mutex& object_lock(Cond& c) { return c.mutex->lock; }

template <class T, class Make>
Result create_object(Kernel& k, u32 pshared, u64 ipc_key, s32 flags, Make&& make, u32& id_out)
{
    Kernel::Impl& s = k.impl();
    if (pshared == SYS_SYNC_NOT_PROCESS_SHARED) {
        std::shared_ptr<T> obj = make();
        obj->refs = 1;
        const u32 id = s.objects.insert(T::kIdBase, obj);
        if (!id) return err(CELL_EAGAIN);
        obj->id = id;
        id_out = id;
        return CELL_OK;
    }
    if (pshared != SYS_SYNC_PROCESS_SHARED) return err(CELL_EINVAL);
    if (ipc_key == 0) return err(CELL_EINVAL);
    if (flags != SYS_SYNC_NEWLY_CREATED && flags != SYS_SYNC_NOT_CREATE && flags != SYS_SYNC_NOT_CARE) {
        return err(CELL_EINVAL);
    }

    std::lock_guard ipc_lock(s.ipc_mutex);
    const auto key = std::make_pair(T::kIdBase, ipc_key);
    std::shared_ptr<T> existing;
    if (auto it = s.ipc.find(key); it != s.ipc.end()) existing = std::static_pointer_cast<T>(it->second.lock());
    if (existing) {
        std::lock_guard lock(object_lock(*existing));
        if (existing->dead) existing.reset();
    }
    if (flags == SYS_SYNC_NOT_CREATE && !existing) return err(CELL_ESRCH);
    if (flags == SYS_SYNC_NEWLY_CREATED && existing) return err(CELL_EEXIST);

    std::shared_ptr<T> obj = existing ? existing : make();
    const u32 id = s.objects.insert(T::kIdBase, obj);
    if (!id) return err(CELL_EAGAIN);
    {
        std::lock_guard lock(object_lock(*obj));
        obj->refs++;
        if (!existing) {
            obj->id = id;
            obj->ipc_key = ipc_key;
        }
    }
    if (!existing) s.ipc[key] = obj;
    id_out = id;
    return CELL_OK;
}

template <class T, class Check>
Result destroy_object(Kernel& k, u32 id, Check&& check)
{
    Kernel::Impl& s = k.impl();
    std::shared_ptr<T> obj = s.objects.get<T>(id);
    if (!obj) return err(CELL_ESRCH);
    bool last = false;
    {
        std::lock_guard lock(object_lock(*obj));
        if (obj->dead) return err(CELL_ESRCH);
        if (const Result r = check(*obj); r != CELL_OK) return r;
        last = --obj->refs == 0;
        if (last) obj->dead = true;
    }
    if (!s.objects.remove(T::kIdBase, id)) return err(CELL_ESRCH);
    if (last && obj->ipc_key) {
        std::lock_guard ipc_lock(s.ipc_mutex);
        s.ipc.erase(std::make_pair(T::kIdBase, obj->ipc_key));
    }
    return CELL_OK;
}

} // namespace pspocket::lv2
