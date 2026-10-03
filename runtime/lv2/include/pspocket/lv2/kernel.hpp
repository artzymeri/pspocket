#pragma once

// The lv2 kernel (plan section 8): kernel objects, PPU threads, the syscall dispatcher, memory, time, the
// filesystem. Semantics follow RPCS3 rpcs3/Emu/Cell/lv2/*.cpp (GPLv2, D-001); each source file cites its reference.
//
// Threads: every PPU thread is a host thread with an 8 MB stack (D-015) that runs guest code through an Executor
// (lifted functions, interpreter fallback). Blocking syscalls block the host thread. There is no emulation of the
// PS3 scheduler: guest threads run in parallel on host cores, and lv2 priorities only order wait queues
// (SYS_SYNC_PRIORITY) and choose host QoS.
//
// Usage (host):
//     GuestMemory mem; mem.reserve();
//     lv2::Kernel kernel(mem, config);
//     kernel.vfs_mount("/dev_hdd0", "/path/to/hdd0", false);
//     kernel.install();                         // routes PPU `sc` to this kernel
//     u32 id; kernel.create_thread(params, id); kernel.start_thread(id);
//     kernel.wait_for_exit();                   // sys_process_exit, or shutdown()

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::lv2 {

class Kernel;

// Thrown to unwind a PPU thread's host stack: sys_ppu_thread_exit, process exit and kernel shutdown. It passes
// through lifted C code, which therefore must keep unwind tables (the clang default on Darwin; never build lifted
// code with -fno-unwind-tables). Deliberately not derived from std::exception.
struct ThreadExit {};

// Runs the guest function at `entry` on the calling host thread until it returns. `ctx` already holds the entry
// state: r1 (stack), r2 (TOC), r3.. (arguments), r13 (TLS) and LR = kThreadReturnAddress.
using Executor = void (*)(void* user, PPUContext& ctx, u8* base, u32 entry);

// The default: the lifted function registered for `entry` (ps_ppu_call_indirect), or, when there is none, the PPU
// miss policy (abort in dev builds, interpret in release builds; tests may select interpretation).
void default_executor(void* user, PPUContext& ctx, u8* base, u32 entry);

// LR at thread entry: returning from the entry function lands here (the interpreter stops at it).
inline constexpr u32 kThreadReturnAddress = 0;
// r1 = stack top - 0x70 at entry (RPCS3 PPUThread.h ppu_stack_start_offset).
inline constexpr u32 kStackStartOffset = 0x70;

enum class JoinState : u8 {
    Joinable, // running, nobody joined yet
    Detached, // running, will clean itself up
    Joining,  // running, a joiner is blocked on it
    Zombie,   // exited, waiting for sys_ppu_thread_join / detach
    Exited,   // exited and reaped (only a stale handle can see this)
};

class PpuThread final : public Object {
public:
    static constexpr u32 kIdBase = 0x01000000; // RPCS3 PPUThread.h ppu_thread::id_base
    u32 id_base() const override { return kIdBase; }

    explicit PpuThread(Kernel& kernel);

    Kernel& kernel;
    Waiter waiter;
    PPUContext* ctx;   // the registers the thread runs with (own_ctx, or the caller's for attached threads)

    // Creation parameters.
    u32 entry = 0;     // code address (from the OPD)
    u32 rtoc = 0;
    u32 tls = 0;
    u64 arg0 = 0;
    u64 arg1 = 0;
    u32 stack_addr = 0;
    u32 stack_size = 0;
    u32 stack_commit = 0; // committed bytes from stack_addr (host-page rounded)
    bool is_main = false;
    bool interrupt = false;
    bool attached = false; // a host thread that adopted this identity (attach_thread); no stack, no host thread

    // Guarded by `lock`.
    std::string name;
    bool started = false;

    // Guarded by the kernel's join lock (thread exit / join / detach).
    JoinState join = JoinState::Joinable;
    Waiter* joiner = nullptr;
    u64 exit_status = 0;
    bool finished = false;

    s32 priority() const { return waiter.prio.load(std::memory_order_relaxed); }

private:
    PPUContext own_ctx_{};
};

// The lv2 thread running on the calling host thread, or nullptr.
PpuThread* current_thread();

struct Config {
    // From the executable's sys_process_param (RPCS3 PPUModule.cpp). Selects the default user memory size.
    u32 sdk_version = 0x00360001;
    // Size of the default memory container; 0 = RPCS3's table by SDK version (0xD500000 for SDK > 2.2).
    u32 user_memory_size = 0;
    // End of the main executable's area (vm::main): 0x10000000, or 0x20000000 for SDK < 2.0 executables.
    u32 main_area_end = 0x10000000;
    // Title ID for _sys_process_get_paramsfo (empty: ENOENT).
    std::string title_id;
    // Debug/root process permissions (allows negative thread priorities and root-only syscalls).
    bool root = false;
    // Abort (instead of logging and returning ENOSYS) on an unimplemented syscall. CLAUDE.md rule 4 asks for the
    // error return; this flag is for bring-up sessions that want to stop at the first gap.
    bool abort_on_unimplemented = false;
    // Host stack of every PPU thread (D-015: sized for guest call depth).
    usize host_stack_size = 8u << 20;
    // How threads run guest code.
    Executor executor = default_executor;
    void* executor_user = nullptr;
    // Called once when the guest exits the process (sys_process_exit*), from the exiting thread.
    std::function<void(s32 status)> on_process_exit;
};

// RPCS3 PPUModule.cpp: default container size by SDK version.
u32 default_user_memory_size(u32 sdk_version);

// lv2 syscall name for a number (from recomp/common/data/syscalls.txt via the generated syscall_table.inc), or
// nullptr for an unnamed or out-of-range number.
const char* syscall_name(u32 number);

struct ThreadParams {
    std::string name;
    u32 entry = 0;          // code address (not an OPD)
    u32 rtoc = 0;
    u64 arg0 = 0;
    u64 arg1 = 0;
    u32 tls = 0;
    s32 prio = 1000;
    u32 stack_size = 0x10000;
    bool joinable = true;
    bool main = false;      // userInteractive QoS (other PPU threads run at userInitiated)
};

class Kernel {
public:
    explicit Kernel(GuestMemory& memory, Config config = {});
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // Routes the PPU `sc` hook (ps_ppu_set_syscall_handler) to this kernel and makes it Kernel::current().
    void install();
    static Kernel* current();

    GuestMemory& memory() { return memory_; }
    u8* base() const { return memory_.base(); }
    const Config& config() const { return config_; }

    // Filesystem mapping (plan section 8): `point` is "/dev_bdvd", "/dev_hdd0", "/dev_hdd1", "/app_home" or
    // "/host_root" (dev only); `host_dir` is an existing host directory.
    bool vfs_mount(std::string_view point, std::string_view host_dir, bool read_only);
    void vfs_unmount(std::string_view point);

    // PPU threads. create_thread allocates the guest stack and an ID and leaves the thread stopped.
    Result create_thread(const ThreadParams& params, u32& id_out);
    Result start_thread(u32 id);
    std::shared_ptr<PpuThread> thread(u32 id) const;

    // Gives the calling host thread an lv2 thread identity bound to `ctx` (ctx.runtime is set), so it can make
    // syscalls: tests, HLE helper threads. No guest stack is allocated. Undo with detach_thread().
    PpuThread* attach_thread(PPUContext& ctx, std::string_view name, s32 prio = 1000);
    void detach_thread(PpuThread* thread);

    // Dispatches the syscall in ctx.gpr[11] for the thread bound to ctx (ctx.runtime). Result in r3 (and r4.. for
    // the few syscalls that return more).
    void syscall(PPUContext& ctx);

    // Process state.
    bool exited() const;
    s32 exit_status() const;
    // Blocks until the guest exits the process or `timeout` passes. Returns exited().
    bool wait_for_exit(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    // Ends the process from the host: aborts blocked syscalls (their threads unwind) and waits up to `timeout` for
    // every host thread to finish. Returns false if some guest thread is still running guest code.
    bool shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    // Number of syscalls dispatched to the unimplemented handler since start (for tests and reports).
    u64 unimplemented_count() const;

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    GuestMemory& memory_;
    Config config_;
    std::unique_ptr<Impl> impl_;
};

} // namespace pspocket::lv2

extern "C" {
// The PPU syscall hook: dispatches through the kernel bound to the calling thread (ctx->runtime), falling back to
// Kernel::current().
void ps_lv2_syscall(PPUContext* ctx, uint8_t* base);
}
