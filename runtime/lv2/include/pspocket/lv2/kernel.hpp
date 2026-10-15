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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/ppu/context.h"
#include "pspocket/spu/context.h"

namespace pspocket::rsx {
class Backend;
class Rsx;
} // namespace pspocket::rsx

namespace pspocket::lv2 {

class EventFlag;
class EventQueue;
class Kernel;

// A read-only file system image mounted in the VFS: a disc image read in place (plan section 9, "read straight from
// the ISO"). The host adapts its reader to this interface (the loader does it for ISO 9660 images); lv2 only reads.
// Paths are relative to the image root, '/'-separated, without a leading slash ("" is the root). Lookups may ignore
// case (a PS3 disc's names), and every member may be called from several threads at once.
class VfsImage {
public:
    struct Stat {
        bool dir = false;
        u64 size = 0;
    };
    class File {
    public:
        virtual ~File() = default;
        virtual u64 size() const = 0;
        // Reads exactly `size` bytes at `offset` (within the file); false on an I/O error.
        virtual bool read(u64 offset, void* dst, usize size) const = 0;
    };
    virtual ~VfsImage() = default;
    virtual std::optional<Stat> stat(std::string_view path) const = 0;
    // The entries of a directory (without "." and ".."), or nullopt if `path` isn't one.
    virtual std::optional<std::vector<std::pair<std::string, Stat>>> list(std::string_view path) const = 0;
    // A regular file, or null if `path` doesn't name one.
    virtual std::shared_ptr<const File> open(std::string_view path) const = 0;
};

// Thrown to unwind a PPU thread's host stack: sys_ppu_thread_exit, process exit and kernel shutdown. It passes
// through lifted C code, which therefore must keep unwind tables (the clang default on Darwin; never build lifted
// code with -fno-unwind-tables). Deliberately not derived from std::exception.
struct ThreadExit {};

// Translated code returned with r1 other than at its entry: a longjmp or a fiber switch moved it elsewhere (D-161).
// The lifter makes a pseudo-syscall there, marked by cia kNonLocalReturnCia (no guest register touched); the kernel
// goes back to the innermost guest entry (run_guest), where the interpreter goes on from the restored context. It
// jumps there with _longjmp (D-213): a C++ exception through the translated frames cost Assassin's Creed's task
// threads most of their time in the unwinder (12,000 fiber switches a second). Between the two there are only
// translated C frames and the interpreter's, which hold nothing to destroy; every way from host code into guest code
// goes through run_guest. NonLocalJump is thrown only when no run_guest is on the thread (not expected).
struct NonLocalJump {};
inline constexpr u32 kNonLocalReturnCia = 0xFFFFFFF0; // recomp/ppu lifter.cpp emits it

// The same jump made by host code: the C library's longjmp (D-572), with the context already holding the registers it
// restores and LR the landing. It goes to the thread's innermost run_guest as the lifter's does, so the caller and every
// frame between must hold nothing to destroy: an HLE function's stub and thunk qualify, and the function must not own
// anything (a lock, a string) when it calls this.
[[noreturn]] void non_local_jump();

// The r1 the thread's innermost run_guest was entered with, 0 with none. A non-local jump lands in the frames that
// run_guest runs, at or below it; a landing above is in guest code that called host code, which cannot be resumed.
u64 innermost_guest_entry_sp();

// Runs the guest function at `entry` on the calling host thread until it returns. `ctx` already holds the entry
// state: r1 (stack), r2 (TOC), r3.. (arguments), r13 (TLS) and LR = kThreadReturnAddress.
using Executor = void (*)(void* user, PPUContext& ctx, u8* base, u32 entry);

// The default: the lifted function registered for `entry` (ps_ppu_call_indirect), or, when there is none, the PPU
// miss policy (abort in dev builds, interpret in release builds; tests may select interpretation).
void default_executor(void* user, PPUContext& ctx, u8* base, u32 entry);

// Runs guest code from `entry` through the kernel's executor until it returns: a thread's entry (`thread_top`: until
// its return to kThreadReturnAddress, whatever r1 is) or a callback (until the return to the LR and r1 it was called
// with). A non-local jump in translated code (NonLocalJump, D-161) goes on in the interpreter from where the jump
// went, with the same end.
void run_guest(Kernel& k, PPUContext& ctx, u8* base, u32 entry, bool thread_top);

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

// PpuThread::current_syscall outside a syscall.
inline constexpr u32 kNoSyscall = ~0u;

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

    // The syscall the thread is executing (set by Kernel::syscall), or kNoSyscall; for reports. HLE functions call
    // lv2 directly, so a thread inside an HLE function is not in a syscall.
    std::atomic<u32> current_syscall{kNoSyscall};

    // The thread's calls into lv2 or a library, counted as each begins and as it returns (dbg_stop_point): another
    // thread reads it to see this one get on (defer_to_sender, event.cpp, D-999). Written by the owning thread only.
    std::atomic<u64> lv2_calls{0};
    // The PPU thread whose event last woke this one, for sender_barrier (D-999). The owning thread only.
    WakerMark woken_by;

    // A debugger's stop (sys_dbg_stop_threads, D-937): once asked, the thread stops at its next call into lv2 or a
    // library, or as such a call returns (dbg_stop_point), and waits there until sys_dbg_continue_threads.
    // `dbg_stopped` while it waits; guarded by the kernel's debugging lock.
    std::atomic<bool> dbg_stop{false};
    bool dbg_stopped = false;

    // Yields back to back (usleep(0), sys_ppu_thread_yield) are a wait loop: after a run of them the thread sleeps a
    // little, longer each time (yield_wait, D-104). The owning host thread only.
    u64 last_yield_ns = 0;
    u32 yield_streak = 0;
    u32 yield_backoff_us = 0;

    s32 priority() const { return waiter.prio.load(std::memory_order_relaxed); }

private:
    PPUContext own_ctx_{};
};

// The lv2 thread running on the calling host thread, or nullptr.
PpuThread* current_thread();
// Where a thread a debugger asked to stop stops (D-937): lv2's calls and pspocket's library functions call it as they
// begin and as they return to guest code. Throws ThreadExit when the process ends while the thread is stopped.
void dbg_park(PpuThread& t);
inline void dbg_stop_point(PpuThread& t)
{
    t.lv2_calls.store(t.lv2_calls.load(std::memory_order_relaxed) + 1, std::memory_order_release); // D-999
    if (t.dbg_stop.load(std::memory_order_acquire)) [[unlikely]] dbg_park(t);
}
// Makes `t` the lv2 thread of the calling host thread (nullptr: none): a PPU thread's host thread at its start, and the
// host thread of one of its fibers while that runs as the PPU thread (cellFiber, D-568).
void set_current_thread(PpuThread* t);

// The process's module loader as lv2's PRX calls (460..499) reach it (D-527): the HLE layer gives one with its
// hle::ModuleLoader (HleProcess::set_module_loader); a kernel without one answers those calls
// CELL_PRX_ERROR_NO_LIBLV2. IDs and error codes are the loader's.
class ModuleHost {
public:
    virtual ~ModuleHost() = default;
    // Places and links the module at `path` (`offset` bytes into the file); its ID in *id.
    virtual u32 load(std::string_view path, u64 offset, u32* id) = 0;
    // Links its exports and starts the firmware modules loaded for it (on the calling thread `t`); *entry its
    // module_start descriptor, *entry2 its module_prologue's (0: none; liblv2 calls the prologue, when there is one,
    // with the entry and the arguments, D-916 part 1).
    virtual u32 start(PpuThread& t, u32 id, u32* entry, u32* entry2) = 0;
    virtual void started(u32 id, s32 result) = 0;
    // *entry its module_stop descriptor, *entry2 its module_epilogue's (0: none).
    virtual u32 stop(u32 id, u32* entry, u32* entry2) = 0;
    virtual void stopped(u32 id, s32 result) = 0;
    virtual u32 unload(u32 id) = 0;
    virtual u32 id_by_name(std::string_view name) = 0;
    virtual u32 id_by_address(u32 addr) = 0;
    virtual std::vector<u32> list() = 0;
    // The module's sys_prx_module_info_t at `info` (its size, 0x48 or 0x58, read there), as liblv2's
    // sys_prx_get_module_info writes it; CELL_OK or an error.
    virtual s32 write_info(PpuThread& t, u32 id, u32 info) = 0;
    // The module's start and stop descriptors (0: none); false for an unknown ID.
    virtual bool entries(u32 id, u32* start, u32* stop) = 0;
};

struct Config {
    // From the executable's sys_process_param (RPCS3 PPUModule.cpp). Selects the default user memory size.
    u32 sdk_version = 0x00360001;
    // Size of the default memory container; 0 = RPCS3's table by SDK version (0xD500000 for SDK > 2.2).
    u32 user_memory_size = 0;
    // End of the main executable's area (vm::main): 0x10000000, or 0x20000000 for SDK < 2.0 executables.
    u32 main_area_end = 0x10000000;
    // Title ID for _sys_process_get_paramsfo (empty: ENOENT).
    std::string title_id;
    // The executable's sys_process_param crash_dump_param_addr (SYS_PROCESS_CRASH_DUMP_ENABLED's declaration; 0 for
    // none): the retail kernel's _sys_process_get_paramsfo gives it to liblv2coredump (D-943).
    u32 crash_dump_param_addr = 0;
    // The executable's guest path, as lv2 keeps it for the process (sys_dbg_get_process_info): argv[0].
    std::string executable_path;
    // Debug/root process permissions (allows negative thread priorities and root-only syscalls).
    bool root = false;
    // The executable's sys_process_param ppc_seg: a process that may load overlay modules (RPCS3
    // g_ps3_process_info.ppc_seg); sys_overlay's calls are CELL_ENOSYS without it.
    u32 ppc_seg = 0;
    // The console's OpenPSID (sys_ss_get_open_psid). RPCS3 makes a random one per installation; pspocket's default is
    // a fixed value a host can replace with one of its own.
    u64 console_psid_high = 0x50535047'4F434B45ull, console_psid_low = 0x54000000'00000001ull;
    // The system software version (_sys_game_get_system_sw_version): RPCS3 reads the installed firmware's
    // version.txt and multiplies it by 10000 (4.92 is 49200). The host sets it from the installed firmware.
    u64 system_sw_version = 49200;
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
    // The GPU backend sys_rsx_context_allocate gives the RSX front end. Unset, or returning null: rsx::NullBackend
    // (the commands run, nothing is drawn).
    std::function<std::unique_ptr<rsx::Backend>()> rsx_backend;
    // Unhandled RSX methods and FIFO errors are logged instead of aborting (tests, release builds; CLAUDE.md rule 5).
    bool rsx_log_failures = false;
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
    // Mounts a read-only image at `point`, from the image's directory `subdir` ("" = its root; e.g. /dev_bdvd from
    // the root of a disc image, /app_home from its PS3_GAME/USRDIR).
    bool vfs_mount_image(std::string_view point, std::shared_ptr<const VfsImage> image, std::string_view subdir = {});
    void vfs_unmount(std::string_view point);
    // The host path a guest path maps to on a host-directory mount, for HLE modules that manage files for the game
    // (cellGame creating game data). Empty for the root, an image mount or a path outside every mount.
    std::string vfs_host_path(std::string_view vpath) const;
    // A whole guest file read into `out` (a host-directory mount or an image), for the host's own use (the loader
    // reading a PRX module the game loads, D-159). False when it is not a readable regular file.
    bool vfs_read_file(std::string_view vpath, std::vector<u8>& out) const;
    // The bytes of every file under a guest directory (a host-directory mount or an image), each rounded up to
    // `rounding` (RPCS3 fs::get_dir_size, for cellGame's sizes). Nullopt when the path isn't a readable directory.
    std::optional<u64> vfs_tree_size(std::string_view vpath, u64 rounding = 1) const;
    // Opens an NPD-encrypted file (D-165): sys_fs_open's 8-byte type argument asks for SDATA (0x18000000010) or
    // EDATA (2) and the file starts "NPD\0". Returns the plain file read through `encrypted`, or null with `error`
    // set; sys_fs_open then fails with CELL_EFSSPECIFIC, as RPCS3's does. Unset (the default), every such file fails.
    using NpdOpener = std::function<std::shared_ptr<const VfsImage::File>(
        std::shared_ptr<const VfsImage::File> encrypted, bool edata, std::string& error)>;
    void set_npd_opener(NpdOpener opener);
    // SPU thread groups (D-167). The identifier attaches to an SPU thread's context the lifted program for the image
    // sys_spu_thread_group_start just copied into its local store (`guest`: the code's segment in guest memory,
    // loaded at `ls_start`, `size` bytes; `entry`); unset, the interpreter runs every SPU thread. The decrypter turns
    // an SPU SELF that sys_spu_image_open reads into its ELF (false with `error` set); unset, SELF images fail with
    // CELL_ENOEXEC.
    using SpuProgramIdentifier = std::function<void(SPUContext& ctx, u32 guest, u32 ls_start, u32 size, u32 entry)>;
    void set_spu_program_identifier(SpuProgramIdentifier identifier);
    using SelfDecrypter = std::function<bool(std::vector<u8>& bytes, std::string& error)>;
    void set_self_decrypter(SelfDecrypter decrypter);

    // PPU threads. create_thread allocates the guest stack and an ID and leaves the thread stopped.
    Result create_thread(const ThreadParams& params, u32& id_out);
    Result start_thread(u32 id);
    std::shared_ptr<PpuThread> thread(u32 id) const;
    // A live event queue by ID, or null (HLE modules that connect queues on the guest's behalf: SPURS ports).
    std::shared_ptr<EventQueue> event_queue(u32 id) const;
    // A live event queue by its IPC key (RPCS3 lv2_event_queue::find), or null: HLE modules that take queue keys.
    std::shared_ptr<EventQueue> event_queue_by_key(u64 key) const;
    // An event flag by ID, or null (SPU code sets event flag bits through its interrupt mailbox).
    std::shared_ptr<EventFlag> event_flag(u32 id) const;
    // A shared memory of the system software's under the IPC key `key` (RPCS3 init_system_shared_memory: an
    // lv2_memory with a system handle), as the system software makes the ones a process opens by key: `size` bytes in
    // the page size of `flags` (SYS_MEMORY_PAGE_SIZE_64K or 1M), charged to none of the process's containers, kept by
    // the kernel. The process's sys_mmapper_allocate_shared_memory of the key, other than as newly created, then opens
    // it, one handle at a time. For a system side a test plays (pspocket's console has no system software). False when
    // the key is taken or the size does not suit the page size.
    bool create_system_shared_memory(u64 key, u32 size, u64 flags);
    // Whether lv2 has a shared memory under the IPC key `key` (HLE modules that would wait for one the system software
    // makes).
    bool shared_memory_key_exists(u64 key) const;

    // Gives the calling host thread an lv2 thread identity bound to `ctx` (ctx.runtime is set), so it can make
    // syscalls: tests, HLE helper threads. No guest stack is allocated. Undo with detach_thread().
    PpuThread* attach_thread(PPUContext& ctx, std::string_view name, s32 prio = 1000);
    void detach_thread(PpuThread* thread);

    // Dispatches the syscall in ctx.gpr[11] for the thread bound to ctx (ctx.runtime). Result in r3 (and r4.. for
    // the few syscalls that return more).
    void syscall(PPUContext& ctx);

    // Tests: called before every dispatched syscall with its number and r3..r10 (comparing a library with Sony's
    // module: the same calls into lv2, D-519). Null removes it; set it while no guest thread runs.
    using SyscallObserver = void (*)(void* user, u32 number, const u64* args);
    void set_syscall_observer(SyscallObserver fn, void* user);

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
    // Whether the syscall table answers `number` (implemented, or answered with CELL_OK as RPCS3's null_func, D-166):
    // the firmware module choice checks the syscalls a module makes (D-175).
    bool syscall_bound(u32 number) const;

    // The RSX front end, once sys_rsx_context_allocate created it; null before.
    rsx::Rsx* rsx();
    // Called on the RSX thread after each flip (HLE cellGcmSys: the game's flip handler). Replaces the previous one.
    void set_rsx_flip_listener(std::function<void(u32 buffer_id)> fn);
    // The module loader lv2's PRX calls use (null: none); the caller keeps it alive while it is set.
    void set_module_host(ModuleHost* host);
    ModuleHost* module_host() const;
    // Called on the RSX thread when it runs GCM_SET_USER_COMMAND, after the drain it makes (HLE cellGcmSys: the
    // game's user handler, with the command's argument). Replaces the previous one.
    void set_rsx_user_command_listener(std::function<void(u32 arg)> fn);
    // Called on the RSX thread when it queues a buffer for a flip on a head (GCM_DRIVER_QUEUE, and the HLE flip
    // command, which queues on head 1 before it flips), after lv2 noted it in the driver information (HLE cellGcmSys:
    // the game's queue handler). Replaces the previous one.
    void set_rsx_queue_listener(std::function<void(u32 head, u32 buffer_id)> fn);

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
