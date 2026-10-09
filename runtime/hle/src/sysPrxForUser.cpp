// sysPrxForUser: the liblv2 user-side library (thread and TLS setup, lightweight sync, process exit, ...).
// References: RPCS3 rpcs3/Emu/Cell/Modules/sysPrxForUser.cpp, sys_ppu_thread_.cpp, sys_lwmutex_.cpp, sys_lwcond_.cpp,
// sys_prx_.cpp, sys_spinlock.cpp, sys_mmapper_.cpp, sys_mempool.cpp, sys_spu_.cpp, sys_game_.cpp.
//
// Implemented: what GTA V's first boot log called (docs/runs/004), plus the rest of the lwmutex set that a created
// lwmutex is used through, and sys_prx_register_library (a GTA V import); Assassin's Creed's imports: the spinlocks,
// the sys_mmapper wrappers, sys_process_is_stack, sys_spu_image_close and sys_game_process_exitspawn; and, as RPCS3 has
// them for every firmware module importing them (D-166, D-168): lwcond, mempool, the atexit list, console output,
// sys_process_get_paramsfo, sys_get_random_number, sys_mmapper_allocate_memory_from_container and
// sys_game_process_exitspawn2. The C library and the heaps are in sysPrxForUser_libc.cpp, the PRX functions in
// sys_prx.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "idm.hpp"
#include "prx.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sysPrxForUser";

void init_lv2_objects(PPUContext& ctx);

// RPCS3 sys_ppu_thread_.cpp sys_initialize_tls. The loader calls it before the executable's entry with the
// executable's PT_TLS segment (RPCS3 ppu_load_exec queues the same HLE call); then, as RPCS3, the library's lv2
// objects (init_lv2_objects).
void sys_initialize_tls(PPUContext& ctx, u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size, u32 tls_mem_size)
{
    PS_LOG_INFO(kChan, "sys_initialize_tls(thread {:#x}, image {:#010x}, size {:#x}, mem_size {:#x})", main_thread_id,
        tls_seg_addr, tls_seg_size, tls_mem_size);
    if (ctx.gpr[13] != 0) return; // already set up

    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sys_initialize_tls without an HLE process");
    if (tls_seg_size > tls_mem_size) {
        PS_LOG_ERROR(kChan, "sys_initialize_tls: image size {:#x} > memory size {:#x}", tls_seg_size, tls_mem_size);
        return;
    }
    if (!p->initialize_tls({tls_seg_addr, tls_seg_size, tls_mem_size})) {
        PS_LOG_ERROR(kChan, "sys_initialize_tls: TLS already initialized or out of memory");
        return;
    }
    const u32 block = p->alloc_tls();
    if (!block) PS_FATAL(kChan, "sys_initialize_tls: no memory for the main thread's TLS");
    ctx.gpr[13] = u64{block} + kTlsR13Offset;
    init_lv2_objects(ctx);
}

// ------------------------------------------------------------------------------------------------- threads, process

// RPCS3 sys_ppu_thread_.cpp sys_ppu_thread_get_id: no syscall, the ID is known to the library.
s32 sys_ppu_thread_get_id(PPUContext& ctx, u32 thread_id)
{
    lv2::PpuThread& t = thread_of(ctx);
    ps_write64(t.kernel.base(), thread_id, lv2::sys_ppu_thread_get_id(t));
    return lv2::CELL_OK;
}

constexpr u64 SYS_PPU_THREAD_CREATE_INTERRUPT = 0x2;

// RPCS3 sys_ppu_thread_.cpp sys_ppu_thread_create: a TLS block for the new thread, then the kernel's
// _sys_ppu_thread_create with {entry descriptor, r13} and sys_ppu_thread_start. Deviation: the TLS block is released
// when the kernel refuses the thread (RPCS3 keeps it; games retry thread creation in a loop when it fails).
s32 sys_ppu_thread_create(PPUContext& ctx, u32 thread_id, u32 entry, u64 arg, s32 prio, u32 stacksize, u64 flags,
    u32 threadname)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    HleProcess* p = HleProcess::current();
    const u32 tls = p ? p->alloc_tls() : 0;
    if (!tls) return lv2::err(lv2::CELL_ENOMEM);
    StackVar param(ctx, base, 8); // ppu_thread_param_t {entry, tls}
    ps_write32(base, param.addr(), entry);
    ps_write32(base, param.addr() + 4, tls + kTlsR13Offset);
    if (const s32 r = lv2::_sys_ppu_thread_create(t, thread_id, param.addr(), arg, 0, prio, stacksize, flags, threadname);
        r != lv2::CELL_OK) {
        p->free_tls(tls);
        return r;
    }
    if (flags & SYS_PPU_THREAD_CREATE_INTERRUPT) return lv2::CELL_OK;
    return lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, thread_id)));
}

// liblv2's internal variables, by the names and NIDs RPCS3 gives them (hidden variables of its sysPrxForUser, D-520):
// the state below lives in them, in guest memory.
u32 prx_variable(std::string_view name)
{
    return HleProcess::current()->link_variable("sysPrxForUser", recomp::nid_of(name));
}

s32 sys_lwmutex_create(PPUContext& ctx, u32 lwmutex, u32 attr);
s32 sys_lwmutex_lock(PPUContext& ctx, u32 lwmutex, u64 timeout);
s32 sys_lwmutex_unlock(PPUContext& ctx, u32 lwmutex);

// A recursive, priority-ordered lwmutex named `name` in the variable `var` (RPCS3 sys_initialize_tls's lwa).
void create_lwmutex(PPUContext& ctx, std::string_view var, const char (&name)[8])
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x10); // sys_lwmutex_attribute_t: protocol, recursive, name
    ps_write32(base, attr.addr() + 0, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr.addr() + 4, lv2::SYS_SYNC_RECURSIVE);
    std::memcpy(base + attr.addr() + 8, name, 8);
    if (const s32 r = sys_lwmutex_create(ctx, prx_variable(var), attr.addr()); r != lv2::CELL_OK)
        PS_FATAL(kChan, "the {} lwmutex: {}", name, lv2::error_name(r));
}

// RPCS3 sys_ppu_thread_.cpp g_ppu_atexit: up to eight functions (descriptors) for the whole process, which every
// sys_ppu_thread_exit calls, in slot order on the exiting thread, under the recursive "atexit!" lwmutex
// g_ppu_atexit_lwm. RPCS3 creates it in sys_initialize_tls; so does this, or at the first registration when the
// process had no TLS set up (an exit before then has nothing to call).
struct AtexitState {
    std::mutex create;
    std::atomic<bool> ready{false};
};

u32 atexit_lwmutex(PPUContext& ctx)
{
    AtexitState& s = HleProcess::current()->state<AtexitState>();
    if (!s.ready.load(std::memory_order_acquire)) {
        std::lock_guard lock(s.create);
        if (!s.ready.load(std::memory_order_acquire)) {
            create_lwmutex(ctx, "g_ppu_atexit_lwm", "atexit!");
            s.ready.store(true, std::memory_order_release);
        }
    }
    return prx_variable("g_ppu_atexit_lwm");
}

// RPCS3: CELL_EPERM when `func` is in the list already (a null one matches a free slot), CELL_ENOMEM when the eight
// slots are taken.
s32 sys_ppu_thread_register_atexit(PPUContext& ctx, u32 func)
{
    PS_LOG_DEBUG(kChan, "sys_ppu_thread_register_atexit({:#x})", func);
    const u32 lwm = atexit_lwmutex(ctx);
    u8* base = guest_base(ctx);
    const u32 list = prx_variable("g_ppu_atexit");
    sys_lwmutex_lock(ctx, lwm, 0);
    s32 r = lv2::err(lv2::CELL_ENOMEM);
    s32 free = -1;
    for (u32 i = 0; i < 8; i++) {
        const u32 f = ps_read32(base, list + 4 * i);
        if (f == func) {
            r = lv2::err(lv2::CELL_EPERM);
            free = -1;
            break;
        }
        if (!f && free < 0) free = static_cast<s32>(i);
    }
    if (free >= 0) {
        ps_write32(base, list + 4 * static_cast<u32>(free), func);
        r = lv2::CELL_OK;
    }
    sys_lwmutex_unlock(ctx, lwm);
    return r;
}

// RPCS3: CELL_ESRCH when `func` is not in the list.
s32 sys_ppu_thread_unregister_atexit(PPUContext& ctx, u32 func)
{
    PS_LOG_DEBUG(kChan, "sys_ppu_thread_unregister_atexit({:#x})", func);
    const u32 lwm = atexit_lwmutex(ctx);
    u8* base = guest_base(ctx);
    const u32 list = prx_variable("g_ppu_atexit");
    sys_lwmutex_lock(ctx, lwm, 0);
    s32 r = lv2::err(lv2::CELL_ESRCH);
    for (u32 i = 0; i < 8; i++) {
        if (ps_read32(base, list + 4 * i) == func) {
            ps_write32(base, list + 4 * i, 0);
            r = lv2::CELL_OK;
            break;
        }
    }
    sys_lwmutex_unlock(ctx, lwm);
    return r;
}

// RPCS3 sys_ppu_thread_.cpp sys_ppu_thread_exit: the atexit functions, the TLS block, then the syscall. A function
// whose descriptor is not guest memory is skipped with the call site logged (RPCS3 faults).
void sys_ppu_thread_exit(PPUContext& ctx, u64 val)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (HleProcess* p = HleProcess::current()) {
        if (p->state<AtexitState>().ready.load(std::memory_order_acquire)) {
            const u32 lwm = prx_variable("g_ppu_atexit_lwm"), list = prx_variable("g_ppu_atexit");
            sys_lwmutex_lock(ctx, lwm, 0);
            for (u32 i = 0; i < 8; i++) {
                const u32 func = ps_read32(t.kernel.base(), list + 4 * i);
                if (!func) continue;
                if (guest_readable(t, func, 8)) {
                    call_guest(ctx, func, {});
                } else {
                    log_call_site(ctx, t.kernel.base(), std::format("sys_ppu_thread_exit: the atexit function {:#x} is "
                                                                    "not guest memory; skipped", func));
                }
            }
            sys_lwmutex_unlock(ctx, lwm);
        }
        if (ctx.gpr[13] >= kTlsR13Offset) p->free_tls(static_cast<u32>(ctx.gpr[13]) - kTlsR13Offset);
    }
    lv2::_sys_ppu_thread_exit(t, val); // does not return
}

// sys_ppu_thread_once(once_ctrl, init), as RPCS3 (sys_ppu_thread_.cpp): under the process's "_lv2ppu" lv2 mutex
// (priority order, not recursive; its ID in g_ppu_once_mutex; made with the TLS, as RPCS3, or at the first call), a
// control word still SYS_PPU_THREAD_ONCE_INIT (0) runs init (a function descriptor) on the calling thread, then becomes
// SYS_PPU_THREAD_DONE_INIT (1). Other threads asking meanwhile wait on the mutex until it has run.
struct OnceState {
    std::mutex mutex; // the lv2 mutex's creation
};

// A priority-ordered lv2 mutex named `name`, its ID written to `id_addr`.
void create_mutex(PPUContext& ctx, u32 id_addr, u32 recursive, const char (&name)[8])
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar v(ctx, base, 0x30); // the ID, then sys_mutex_attribute_t
    const u32 id = v.addr(), attr = v.addr() + 8;
    std::memset(base + attr, 0, 0x28);
    ps_write32(base, attr + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr + 0x04, recursive);
    ps_write32(base, attr + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, attr + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    std::memcpy(base + attr + 0x20, name, 8);
    if (const s32 r = lv2::sys_mutex_create(t, id, attr); r != lv2::CELL_OK)
        PS_FATAL(kChan, "the {} mutex: {}", name, lv2::error_name(r));
    atomic_store_be32(base, id_addr, ps_read32(base, id));
}

void sys_ppu_thread_once(PPUContext& ctx, u32 once_ctrl, u32 init)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, once_ctrl, 4)) {
        log_call_site(ctx, base, std::format("sys_ppu_thread_once: control word {:#x} is not guest memory", once_ctrl));
        return;
    }
    OnceState& s = HleProcess::current()->state<OnceState>();
    const u32 var = prx_variable("g_ppu_once_mutex");
    u32 m = atomic_load_be32(base, var);
    if (!m) {
        std::lock_guard lock(s.mutex);
        if (!atomic_load_be32(base, var)) create_mutex(ctx, var, lv2::SYS_SYNC_NOT_RECURSIVE, "_lv2ppu");
        m = atomic_load_be32(base, var);
    }
    if (const s32 r = lv2::sys_mutex_lock(t, m, 0); r != lv2::CELL_OK)
        PS_FATAL(kChan, "sys_ppu_thread_once({:#x}, {:#x}): locking _lv2ppu: {} (init called itself?)", once_ctrl, init,
            lv2::error_name(r));
    if (ps_read32(base, once_ctrl) == 0) {
        PS_LOG_DEBUG(kChan, "sys_ppu_thread_once({:#x}): running {:#x}", once_ctrl, init);
        call_guest(ctx, init, {});
        ps_write32(base, once_ctrl, 1);
    }
    lv2::sys_mutex_unlock(t, m);
}

// RPCS3 sysPrxForUser.cpp _sys_process_atexitspawn / _sys_process_at_Exitspawn: remember the first function given, in
// g_ppu_atexitspawn and g_ppu_at_Exitspawn. They run before an exitspawn (sys_game_process_exitspawn).
void _sys_process_atexitspawn(PPUContext& ctx, u32 func)
{
    atomic_cas_be32(guest_base(ctx), prx_variable("g_ppu_atexitspawn"), 0, func);
    PS_LOG_DEBUG(kChan, "_sys_process_atexitspawn({:#x})", func);
}

void _sys_process_at_Exitspawn(PPUContext& ctx, u32 func)
{
    atomic_cas_be32(guest_base(ctx), prx_variable("g_ppu_at_Exitspawn"), 0, func);
    PS_LOG_DEBUG(kChan, "_sys_process_at_Exitspawn({:#x})", func);
}

// liblv2's process exit list (read from the firmware's module, D-501; RPCS3 has a TODO): sys_process_atexit appends a
// function (a descriptor) under an lv2 mutex, the table growing by 8 entries from _sys_malloc; sys_process_exit calls
// them from the last to the first, each taken off the list before its call, then ends the process. Before that it
// takes an exit mutex it never gives back, so a second thread exiting at the same time waits there until the process
// ends. Deviations: the table lives on the host (a game cannot see liblv2's), so no _sys_malloc and no ENOMEM; both
// mutexes are made at their first use (liblv2 makes them as it starts).
struct ProcessAtexitState {
    std::mutex create; // the lv2 mutexes' creation
    std::atomic<u32> list_mutex{0};
    std::vector<u32> funcs; // guarded by list_mutex
};

u32 process_mutex(PPUContext& ctx, ProcessAtexitState& s, std::atomic<u32>& slot, const char (&name)[8])
{
    if (const u32 m = slot.load(std::memory_order_acquire)) return m;
    std::lock_guard lock(s.create);
    if (const u32 m = slot.load(std::memory_order_acquire)) return m;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar v(ctx, base, 0x30); // the ID, then sys_mutex_attribute_t
    const u32 id = v.addr(), attr = v.addr() + 8;
    ps_write32(base, attr + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr + 0x04, lv2::SYS_SYNC_RECURSIVE);
    ps_write32(base, attr + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, attr + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    std::memcpy(base + attr + 0x20, name, 8);
    if (const s32 r = lv2::sys_mutex_create(t, id, attr); r != lv2::CELL_OK)
        PS_FATAL(kChan, "the {} mutex: {}", name, lv2::error_name(r));
    slot.store(ps_read32(base, id), std::memory_order_release);
    return ps_read32(base, id);
}

// liblv2's exit mutex ("exitprc", recursive), whose ID RPCS3 names g_ppu_exit_mutex: made with the TLS or at first use.
u32 exit_mutex(PPUContext& ctx)
{
    u8* base = guest_base(ctx);
    const u32 var = prx_variable("g_ppu_exit_mutex");
    if (const u32 m = atomic_load_be32(base, var)) return m;
    ProcessAtexitState& s = HleProcess::current()->state<ProcessAtexitState>();
    std::lock_guard lock(s.create);
    if (!atomic_load_be32(base, var)) create_mutex(ctx, var, lv2::SYS_SYNC_RECURSIVE, "exitprc");
    return atomic_load_be32(base, var);
}

s32 sys_process_atexit(PPUContext& ctx, u32 func)
{
    PS_LOG_DEBUG(kChan, "sys_process_atexit({:#x})", func);
    if (!func) return lv2::err(lv2::CELL_EINVAL);
    lv2::PpuThread& t = thread_of(ctx);
    ProcessAtexitState& s = HleProcess::current()->state<ProcessAtexitState>();
    const u32 m = process_mutex(ctx, s, s.list_mutex, "atexitp");
    lv2::sys_mutex_lock(t, m, 0);
    s.funcs.push_back(func);
    lv2::sys_mutex_unlock(t, m);
    return lv2::CELL_OK;
}

// liblv2's sys_process_exit: the exit mutex, the exit list, then _sys_process_exit(status, 0, 0).
void sys_process_exit(PPUContext& ctx, s32 status)
{
    PS_LOG_INFO(kChan, "sys_process_exit({})", status);
    lv2::PpuThread& t = thread_of(ctx);
    ProcessAtexitState& s = HleProcess::current()->state<ProcessAtexitState>();
    lv2::sys_mutex_lock(t, exit_mutex(ctx), 0);
    if (const u32 m = s.list_mutex.load(std::memory_order_acquire)) {
        lv2::sys_mutex_lock(t, m, 0);
        while (!s.funcs.empty()) {
            const u32 func = s.funcs.back();
            s.funcs.pop_back();
            call_guest(ctx, func, {});
        }
        lv2::sys_mutex_unlock(t, m);
    }
    lv2::_sys_process_exit(t, status, 0, 0); // does not return
}

// RPCS3 sysPrxForUser.cpp: the library's copy of the syscall.
u64 sys_time_get_system_time(PPUContext& ctx) { return lv2::sys_time_get_system_time(thread_of(ctx)); }

// RPCS3 sys_prx_.cpp: a TODO that returns CELL_OK.
s32 sys_prx_exitspawn_with_level(PPUContext&)
{
    PS_LOG_WARN(kChan, "sys_prx_exitspawn_with_level(): not implemented, returns CELL_OK (as RPCS3)");
    return lv2::CELL_OK;
}

// A library entry as an executable exports it (RPCS3 PPUModule.h ppu_prx_module_info; the syscall reads these 0x1c
// bytes): u8 size, unk0; be16 version, attributes, num_func, num_var, num_tlsvar; u8 info_hash, info_tlshash, unk1[2];
// be32 name, nids, addrs.
constexpr u32 kLibEntrySize = 0x1C, kLibAttributes = 4, kLibNumFunc = 6, kLibNumVar = 8, kLibName = 16;

struct PrxLogState {
    std::atomic<bool> register_library_logged{false};
};

// A guest C string for a log: up to `max` characters, as far as guest memory goes.
std::string read_log_string(lv2::PpuThread& t, u32 addr, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max && u64{addr} + i < GuestMemory::kGuestSize; i++) {
        if (!guest_readable(t, addr + i, 1)) break;
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// RPCS3 sys_prx_.cpp sys_prx_register_library: under the "_lv2prx" lwmutex, the syscall _sys_prx_register_library
// (lv2/sys_prx.cpp), which returns CELL_EFAULT for a library entry that is not guest memory, and otherwise registers
// the entry's exports (ppu_load_exports) for the modules loaded after it to import, and returns CELL_OK. No PRX module
// is ever loaded here (the loader runs the executable alone, and GTA V ships none), so nothing could import the
// library: the call is logged (at warning the first time in a process, then at debug) and returns CELL_OK. There is no
// "_lv2prx" lwmutex to take either: sys_initialize_tls doesn't create it, as nothing else would use it.
s32 sys_prx_register_library(PPUContext& ctx, u32 lib_entry)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, lib_entry, 1)) {
        PS_LOG_WARN(kChan, "sys_prx_register_library({:#x}): not guest memory: CELL_EFAULT", lib_entry);
        return lv2::err(lv2::CELL_EFAULT);
    }
    std::string library;
    if (guest_readable(t, lib_entry, kLibEntrySize)) {
        u8* base = t.kernel.base();
        library = std::format(" (\"{}\", attributes {:#x}, {} functions, {} variables)",
            read_log_string(t, ps_read32(base, lib_entry + kLibName), 64), ps_read16(base, lib_entry + kLibAttributes),
            ps_read16(base, lib_entry + kLibNumFunc), ps_read16(base, lib_entry + kLibNumVar));
    }
    const bool first = !HleProcess::current()->state<PrxLogState>().register_library_logged.exchange(true);
    log::print(first ? log::Level::Warn : log::Level::Debug, kChan,
        "sys_prx_register_library({:#x}){}: not registered, as no PRX module is loaded to import it; CELL_OK (RPCS3 "
        "registers its exports)",
        lib_entry, library);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- lwmutex
// User-space half of the lightweight mutex (RPCS3 sys_lwmutex_.cpp, without its hle_lwmutex option): the owner word
// of the guest's sys_lwmutex_t is taken with atomics, the kernel's sleep queue (_sys_lwmutex_*) only on contention.

// sys_lwmutex_t (RPCS3 sys_lwmutex.h): {owner, waiter} (one 64-bit word), attribute, recursive_count, sleep_queue.
constexpr u32 kOwner = 0x0, kAllInfo = 0x0, kAttribute = 0x8, kRecursiveCount = 0xC, kSleepQueue = 0x10;
// sys_lwmutex_attribute_t: protocol, recursive, name[8].
constexpr u32 kAttrProtocol = 0x0, kAttrRecursive = 0x4, kAttrName = 0x8;

constexpr u32 kLwFree = 0xFFFFFFFF, kLwDead = 0xFFFFFFFE, kLwReserved = 0xFFFFFFFD;

using lv2::err;

// RPCS3 busy_wait(): a short pause before re-checking the owner.
void busy_wait()
{
    for (int i = 0; i < 64; i++) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
}

s32 sys_lwmutex_trylock(PPUContext& ctx, u32 lwmutex);
s32 sys_lwmutex_unlock(PPUContext& ctx, u32 lwmutex);

s32 sys_lwmutex_create(PPUContext& ctx, u32 lwmutex, u32 attr)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 recursive = ps_read32(base, attr + kAttrRecursive);
    if (recursive != lv2::SYS_SYNC_RECURSIVE && recursive != lv2::SYS_SYNC_NOT_RECURSIVE) {
        PS_LOG_ERROR(kChan, "sys_lwmutex_create: invalid recursive attribute {:#x}", recursive);
        return err(lv2::CELL_EINVAL);
    }
    const u32 protocol = ps_read32(base, attr + kAttrProtocol);
    if (protocol != lv2::SYS_SYNC_FIFO && protocol != lv2::SYS_SYNC_RETRY && protocol != lv2::SYS_SYNC_PRIORITY) {
        PS_LOG_ERROR(kChan, "sys_lwmutex_create: invalid protocol {:#x}", protocol);
        return err(lv2::CELL_EINVAL);
    }
    StackVar out_id(ctx, base, 4);
    if (const s32 r = lv2::_sys_lwmutex_create(t, out_id.addr(), protocol, lwmutex, static_cast<s32>(0x80000001),
            ps_read64(base, attr + kAttrName));
        r != lv2::CELL_OK) {
        return r;
    }
    ps_write64(base, lwmutex + kAllInfo, u64{kLwFree} << 32);
    ps_write32(base, lwmutex + kAttribute, recursive | protocol);
    ps_write32(base, lwmutex + kRecursiveCount, 0);
    ps_write32(base, lwmutex + kSleepQueue, ps_read32(base, out_id.addr()));
    return lv2::CELL_OK;
}

s32 sys_lwmutex_destroy(PPUContext& ctx, u32 lwmutex)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (atomic_load_be32(base, lwmutex + kOwner) == t.id) return err(lv2::CELL_EBUSY); // would lock recursively
    if (const s32 r = sys_lwmutex_trylock(ctx, lwmutex); r != lv2::CELL_OK) return r;
    if (const s32 r = lv2::_sys_lwmutex_destroy(t, ps_read32(base, lwmutex + kSleepQueue)); r != lv2::CELL_OK) {
        sys_lwmutex_unlock(ctx, lwmutex);
        return r;
    }
    atomic_store_be32(base, lwmutex + kOwner, kLwDead);
    return lv2::CELL_OK;
}

// The owner is this thread already: recursion rules shared by lock and trylock.
s32 relock(u8* base, u32 lwmutex)
{
    if ((ps_read32(base, lwmutex + kAttribute) & lv2::SYS_SYNC_RECURSIVE) == 0) return err(lv2::CELL_EDEADLK);
    const u32 count = ps_read32(base, lwmutex + kRecursiveCount);
    if (count == 0xFFFFFFFF) return err(lv2::CELL_EKRESOURCE);
    ps_write32(base, lwmutex + kRecursiveCount, count + 1);
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return lv2::CELL_OK;
}

s32 sys_lwmutex_lock(PPUContext& ctx, u32 lwmutex, u64 timeout)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 tid = t.id;

    const u32 old_owner = atomic_cas_be32(base, lwmutex + kOwner, kLwFree, tid);
    if (old_owner == kLwFree) return lv2::CELL_OK;
    if (old_owner == tid) return relock(base, lwmutex);
    if (old_owner == kLwDead) return err(lv2::CELL_EINVAL);

    for (int i = 0; i < 10; i++) {
        busy_wait();
        if (atomic_load_be32(base, lwmutex + kOwner) == kLwFree &&
            atomic_cas_be32(base, lwmutex + kOwner, kLwFree, tid) == kLwFree) {
            return lv2::CELL_OK;
        }
    }

    atomic_add_be64(base, lwmutex + kAllInfo, 1); // one more waiter
    if (atomic_cas_be32(base, lwmutex + kOwner, kLwFree, tid) == kLwFree) {
        atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
        return lv2::CELL_OK;
    }

    const u32 queue = ps_read32(base, lwmutex + kSleepQueue);
    const s32 res = lv2::_sys_lwmutex_lock(t, queue, timeout);
    atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
    if (res == lv2::CELL_OK) {
        // The unlocking thread left the owner reserved for us.
        const u32 old = atomic_exchange_be32(base, lwmutex + kOwner, tid);
        if (old != kLwReserved) PS_FATAL(kChan, "sys_lwmutex_lock({:#010x}): owner {:#x}, expected reserved", lwmutex, old);
        return lv2::CELL_OK;
    }

    if (res == err(lv2::CELL_EBUSY) && (ps_read32(base, lwmutex + kAttribute) & lv2::SYS_SYNC_RETRY)) {
        for (;;) {
            for (int i = 0; i < 10; i++) {
                busy_wait();
                if (atomic_load_be32(base, lwmutex + kOwner) == kLwFree &&
                    atomic_cas_be32(base, lwmutex + kOwner, kLwFree, tid) == kLwFree) {
                    return lv2::CELL_OK;
                }
            }
            atomic_add_be64(base, lwmutex + kAllInfo, 1);
            if (atomic_cas_be32(base, lwmutex + kOwner, kLwFree, tid) == kLwFree) {
                atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
                return lv2::CELL_OK;
            }
            const u64 time0 = timeout ? lv2::sys_time_get_system_time(t) : 0;
            const s32 res2 = lv2::_sys_lwmutex_lock(t, queue, timeout);
            if (res2 == lv2::CELL_OK) {
                atomic_store_be32(base, lwmutex + kOwner, tid);
            } else if (timeout && res2 != err(lv2::CELL_ETIMEDOUT)) {
                const u64 elapsed = lv2::sys_time_get_system_time(t) - time0;
                if (timeout <= elapsed) {
                    atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
                    return err(lv2::CELL_ETIMEDOUT);
                }
                timeout -= elapsed;
            }
            atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
            if (res2 != err(lv2::CELL_EBUSY)) return res2;
        }
    }
    return res;
}

s32 sys_lwmutex_trylock(PPUContext& ctx, u32 lwmutex)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 tid = t.id;

    const u32 old_owner = atomic_cas_be32(base, lwmutex + kOwner, kLwFree, tid);
    if (old_owner == kLwFree) return lv2::CELL_OK;
    if (old_owner == tid) return relock(base, lwmutex);
    if (old_owner == kLwDead) return err(lv2::CELL_EINVAL);
    if (old_owner == kLwReserved) {
        const s32 res = lv2::_sys_lwmutex_trylock(t, ps_read32(base, lwmutex + kSleepQueue));
        if (res == lv2::CELL_OK) {
            const u32 old = atomic_exchange_be32(base, lwmutex + kOwner, tid);
            if (old != kLwReserved) {
                PS_FATAL(kChan, "sys_lwmutex_trylock({:#010x}): owner {:#x}, expected reserved", lwmutex, old);
            }
        }
        return res;
    }
    return err(lv2::CELL_EBUSY); // held by another thread (not an error to log)
}

s32 sys_lwmutex_unlock(PPUContext& ctx, u32 lwmutex)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 tid = t.id;

    if (atomic_load_be32(base, lwmutex + kOwner) != tid) return err(lv2::CELL_EPERM);
    if (const u32 count = ps_read32(base, lwmutex + kRecursiveCount); count != 0) {
        ps_write32(base, lwmutex + kRecursiveCount, count - 1);
        return lv2::CELL_OK;
    }
    // No waiters: {tid, 0} -> {free, 0}.
    if (atomic_cas_be64(base, lwmutex + kAllInfo, u64{tid} << 32, u64{kLwFree} << 32)) return lv2::CELL_OK;

    const u32 queue = ps_read32(base, lwmutex + kSleepQueue);
    if (ps_read32(base, lwmutex + kAttribute) & lv2::SYS_SYNC_RETRY) {
        atomic_store_be32(base, lwmutex + kOwner, kLwFree);
        if (lv2::_sys_lwmutex_unlock2(t, queue) == err(lv2::CELL_ESRCH)) return err(lv2::CELL_ESRCH);
        return lv2::CELL_OK;
    }
    atomic_store_be32(base, lwmutex + kOwner, kLwReserved); // the woken waiter takes it from here
    if (lv2::_sys_lwmutex_unlock(t, queue) == err(lv2::CELL_ESRCH)) return err(lv2::CELL_ESRCH);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- lwcond
// User-space half of the lightweight condition variable (RPCS3 sys_lwcond_.cpp, without its hle_lwmutex option): the
// guest's sys_lwcond_t and the kernel's _sys_lwcond_* (runtime/lv2/src/lw.cpp), with the lwmutex's owner and waiter
// words kept as the lwmutex functions above keep them. The signal modes: 1 when the caller owns the lwmutex (the
// waiter moves to the lwmutex's sleep queue, one more waiter counted), 2 when it does not (the waiter locks it again
// itself), 3 when the caller took it free (it is handed to the waiter); a RETRY lwmutex always uses 2. RPCS3 reads the
// structures unchecked; here an lwcond, or the lwmutex it names, that is not guest memory (or an lwmutex not 8-byte
// aligned, as its atomics need) is refused with CELL_EFAULT and the call site logged (D-042).

constexpr u32 kCondMutex = 0x0, kCondQueue = 0x4; // sys_lwcond_t (RPCS3 sys_lwcond.h)
constexpr u32 kLwmutexSize = 0x18;
constexpr u64 kAnyThread = 0xFFFFFFFF; // RPCS3's u32{umax}: the first waiter

// RPCS3 lock_var.atomic_op: the owner and waiter words of a sys_lwmutex_t changed in one step.
template <class Fn>
void lwmutex_update(u8* base, u32 lwmutex, Fn&& fn)
{
    u64* word = host_u64(base, lwmutex + kAllInfo);
    u64 old = __atomic_load_n(word, __ATOMIC_RELAXED);
    for (;;) {
        const u64 v = ps_bswap64(old);
        u32 owner = static_cast<u32>(v >> 32), waiter = static_cast<u32>(v);
        fn(owner, waiter);
        if (__atomic_compare_exchange_n(word, &old, ps_bswap64((u64{owner} << 32) | waiter), true, __ATOMIC_SEQ_CST,
                __ATOMIC_RELAXED)) {
            return;
        }
    }
}

// The lwmutex of `lwcond` when both are guest memory, else 0 (refused, logged).
u32 lwcond_mutex(PPUContext& ctx, u32 lwcond, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (guest_readable(t, lwcond, 8)) {
        const u32 lwmutex = ps_read32(base, lwcond + kCondMutex);
        if (lwmutex % 8 == 0 && guest_readable(t, lwmutex, kLwmutexSize)) return lwmutex;
    }
    log_call_site(ctx, base, std::format("{}({:#x}): the lwcond or its lwmutex is not guest memory; CELL_EFAULT", what,
        lwcond));
    return 0;
}

bool lwmutex_retry(u8* base, u32 lwmutex)
{
    return (ps_read32(base, lwmutex + kAttribute) & lv2::SYS_SYNC_ATTR_PROTOCOL_MASK) == lv2::SYS_SYNC_RETRY;
}

s32 sys_lwcond_create(PPUContext& ctx, u32 lwcond, u32 lwmutex, u32 attr)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, lwcond, 8) || !guest_readable(t, lwmutex, kLwmutexSize) || !guest_readable(t, attr, 8)) {
        log_call_site(ctx, base, std::format("sys_lwcond_create({:#x}, {:#x}, {:#x}): not guest memory; CELL_EFAULT",
            lwcond, lwmutex, attr));
        return err(lv2::CELL_EFAULT);
    }
    StackVar out_id(ctx, base, 4);
    if (const s32 r = lv2::_sys_lwcond_create(t, out_id.addr(), ps_read32(base, lwmutex + kSleepQueue), lwcond,
            ps_read64(base, attr));
        r != lv2::CELL_OK) {
        return r;
    }
    ps_write32(base, lwcond + kCondMutex, lwmutex);
    ps_write32(base, lwcond + kCondQueue, ps_read32(base, out_id.addr()));
    return lv2::CELL_OK;
}

// RPCS3: the queue ID becomes lwmutex_dead once the kernel has let it go.
s32 sys_lwcond_destroy(PPUContext& ctx, u32 lwcond)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, lwcond, 8)) {
        log_call_site(ctx, base, std::format("sys_lwcond_destroy({:#x}): not guest memory; CELL_EFAULT", lwcond));
        return err(lv2::CELL_EFAULT);
    }
    if (const s32 r = lv2::_sys_lwcond_destroy(t, ps_read32(base, lwcond + kCondQueue)); r != lv2::CELL_OK) return r;
    ps_write32(base, lwcond + kCondQueue, kLwDead);
    return lv2::CELL_OK;
}

// sys_lwcond_signal (`target` kAnyThread, `any` true) and sys_lwcond_signal_to, which RPCS3 writes out twice and which
// differ only in what an error of the syscall becomes: signal ignores "no waiter" (CELL_EPERM in mode 1, CELL_ENOENT
// in mode 3), signal_to returns it.
s32 lwcond_signal(PPUContext& ctx, u32 lwcond, u64 target, bool any, std::string_view what)
{
    const u32 lwmutex = lwcond_mutex(ctx, lwcond, what);
    if (!lwmutex) return err(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 queue = ps_read32(base, lwcond + kCondQueue), sleep_queue = ps_read32(base, lwmutex + kSleepQueue);
    if (lwmutex_retry(base, lwmutex)) return lv2::_sys_lwcond_signal(t, queue, 0, target, 2);

    if (atomic_load_be32(base, lwmutex + kOwner) == t.id) {
        atomic_add_be64(base, lwmutex + kAllInfo, 1); // the waiter will sleep on the lwmutex
        if (const s32 r = lv2::_sys_lwcond_signal(t, queue, sleep_queue, target, 1); r != lv2::CELL_OK) {
            atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
            if (!any || r != err(lv2::CELL_EPERM)) return r;
        }
        return lv2::CELL_OK;
    }

    if (const s32 r = sys_lwmutex_trylock(ctx, lwmutex); r != lv2::CELL_OK) {
        if (r != err(lv2::CELL_EBUSY)) return err(lv2::CELL_ESRCH);
        return lv2::_sys_lwcond_signal(t, queue, 0, target, 2);
    }
    // Locked: reserved for the waiter, who is handed the lwmutex.
    lwmutex_update(base, lwmutex, [](u32& owner, u32& waiter) {
        waiter++;
        owner = kLwReserved;
    });
    if (const s32 r = lv2::_sys_lwcond_signal(t, queue, sleep_queue, target, 3); r != lv2::CELL_OK) {
        lwmutex_update(base, lwmutex, [&](u32& owner, u32& waiter) {
            waiter--;
            owner = t.id;
        });
        sys_lwmutex_unlock(ctx, lwmutex);
        if (!any || r != err(lv2::CELL_ENOENT)) return r;
    }
    return lv2::CELL_OK;
}

s32 sys_lwcond_signal(PPUContext& ctx, u32 lwcond)
{
    return lwcond_signal(ctx, lwcond, kAnyThread, true, "sys_lwcond_signal");
}

s32 sys_lwcond_signal_to(PPUContext& ctx, u32 lwcond, u64 ppu_thread_id)
{
    return lwcond_signal(ctx, lwcond, ppu_thread_id, false, "sys_lwcond_signal_to");
}

// RPCS3: in mode 1 the syscall returns how many waiters moved to the lwmutex's sleep queue, all counted as waiters.
s32 sys_lwcond_signal_all(PPUContext& ctx, u32 lwcond)
{
    const u32 lwmutex = lwcond_mutex(ctx, lwcond, "sys_lwcond_signal_all");
    if (!lwmutex) return err(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 queue = ps_read32(base, lwcond + kCondQueue), sleep_queue = ps_read32(base, lwmutex + kSleepQueue);
    if (lwmutex_retry(base, lwmutex)) return lv2::_sys_lwcond_signal_all(t, queue, sleep_queue, 2);

    if (atomic_load_be32(base, lwmutex + kOwner) == t.id) {
        const s32 r = lv2::_sys_lwcond_signal_all(t, queue, sleep_queue, 1);
        if (r <= 0) return r; // an error, or none moved
        atomic_add_be64(base, lwmutex + kAllInfo, static_cast<u64>(r));
        return lv2::CELL_OK;
    }

    if (const s32 r = sys_lwmutex_trylock(ctx, lwmutex); r != lv2::CELL_OK) {
        if (r != err(lv2::CELL_EBUSY)) return err(lv2::CELL_ESRCH);
        return lv2::_sys_lwcond_signal_all(t, queue, sleep_queue, 2);
    }
    s32 r = lv2::_sys_lwcond_signal_all(t, queue, sleep_queue, 1);
    if (r > 0) {
        atomic_add_be64(base, lwmutex + kAllInfo, static_cast<u64>(r));
        r = lv2::CELL_OK;
    }
    sys_lwmutex_unlock(ctx, lwmutex);
    return r;
}

// RPCS3 sys_lwcond_wait: the caller must own the lwmutex (CELL_EPERM). Its recursion count is kept aside and the owner
// set to reserved while the kernel sleeps; then CELL_OK (the lwmutex handed over, one waiter less) or CELL_ESRCH give
// it back to the caller at once, CELL_EBUSY (signalled in mode 2) and CELL_ETIMEDOUT lock it again first (CELL_EBUSY
// then returns CELL_OK), and CELL_EDEADLK gives it back and returns CELL_ETIMEDOUT. RPCS3 throws on an owner that is
// free or dead after the handover, and on any other result; pspocket aborts (CLAUDE.md rule 5).
s32 sys_lwcond_wait(PPUContext& ctx, u32 lwcond, u64 timeout)
{
    const u32 lwmutex = lwcond_mutex(ctx, lwcond, "sys_lwcond_wait");
    if (!lwmutex) return err(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 tid = t.id;
    if (atomic_load_be32(base, lwmutex + kOwner) != tid) return err(lv2::CELL_EPERM);

    const u32 recursive_value = ps_read32(base, lwmutex + kRecursiveCount);
    atomic_store_be32(base, lwmutex + kOwner, kLwReserved);
    ps_write32(base, lwmutex + kRecursiveCount, 0);
    const s32 res = lv2::_sys_lwcond_queue_wait(t, ps_read32(base, lwcond + kCondQueue),
        ps_read32(base, lwmutex + kSleepQueue), timeout);

    const auto take_back = [&] {
        const u32 old = atomic_exchange_be32(base, lwmutex + kOwner, tid);
        ps_write32(base, lwmutex + kRecursiveCount, recursive_value);
        if (old == kLwFree || old == kLwDead) {
            PS_FATAL(kChan, "sys_lwcond_wait({:#010x}): locking failed (lwmutex {:#010x}, owner {:#x})", lwcond,
                lwmutex, old);
        }
    };
    if (res == lv2::CELL_OK || res == err(lv2::CELL_ESRCH)) {
        if (res == lv2::CELL_OK) atomic_add_be64(base, lwmutex + kAllInfo, ~u64{0});
        take_back();
        return res;
    }
    if (res == err(lv2::CELL_EBUSY) || res == err(lv2::CELL_ETIMEDOUT)) {
        if (const s32 r2 = sys_lwmutex_lock(ctx, lwmutex, 0); r2 != lv2::CELL_OK) return r2;
        ps_write32(base, lwmutex + kRecursiveCount, recursive_value);
        return res == err(lv2::CELL_EBUSY) ? lv2::CELL_OK : res;
    }
    if (res == err(lv2::CELL_EDEADLK)) {
        take_back();
        return err(lv2::CELL_ETIMEDOUT);
    }
    PS_FATAL(kChan, "sys_lwcond_wait({:#010x}): unexpected result {} from _sys_lwcond_queue_wait", lwcond,
        lv2::error_name(res));
}

// ------------------------------------------------------------------------------------------------- spinlocks
// RPCS3 sys_spinlock.cpp: a spinlock is one big-endian guest word, 0 when free; lock and trylock exchange 0xabadcafe
// into it and own the lock when the exchange found 0. Every store goes through the reservation system (one update of
// the word's 128-byte line), so it is atomic against guest lwarx/stwcx. and SPU GETLLAR/PUTLLC on the word and wakes
// what waits on the line.

constexpr u32 kSpinlockTaken = 0xABADCAFE;

// The lock word must be a naturally aligned word of guest memory (the PS3's lwarx needs the alignment; RPCS3 reads it
// unchecked); otherwise the call is refused with a log of the call site (D-042).
bool spinlock_word_ok(PPUContext& ctx, u32 lock, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (lock % 4 == 0 && guest_readable(t, lock, 4)) return true;
    log_call_site(ctx, t.kernel.base(),
        std::format("{}({:#x}): not an aligned word of guest memory; refused", what, lock));
    return false;
}

// Stores `value` into the lock word in one reservation update and returns what the word held (RPCS3's exchange).
u32 spinlock_exchange(u8* base, u32 lock, u32 value)
{
    const u32 line = PS_RES_LINE_ADDR(lock), offset = lock - line;
    u32 old = 0;
    reservation_op(base, line, [&](u8* copy) {
        old = ps_read32(copy, offset);
        ps_write32(copy, offset, value);
        return true;
    });
    return old;
}

// RPCS3: a word that is not 0 is cleared; a free lock is left alone.
void sys_spinlock_initialize(PPUContext& ctx, u32 lock)
{
    if (!spinlock_word_ok(ctx, lock, "sys_spinlock_initialize")) return;
    u8* base = guest_base(ctx);
    if (atomic_load_be32(base, lock) != 0) spinlock_exchange(base, lock, 0);
}

// RPCS3 `while (*lock || lock->exchange(0xabadcafe))`: spins until the word reads 0 and the exchange found 0. The wait
// is polite: short pauses first, as sys_lwmutex_lock's, then lv2's yield, which backs off to sleeps (D-104) and reports
// the end of the process, where RPCS3's loop leaves on test_stopped(): the thread then unwinds.
s32 sys_spinlock_lock(PPUContext& ctx, u32 lock)
{
    if (!spinlock_word_ok(ctx, lock, "sys_spinlock_lock")) return err(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    for (u32 round = 0;; round++) {
        if (atomic_load_be32(base, lock) == 0 && spinlock_exchange(base, lock, kSpinlockTaken) == 0) {
            return lv2::CELL_OK;
        }
        if (round < 10) {
            busy_wait();
        } else if (!lv2::yield_wait(t)) {
            throw lv2::ThreadExit{};
        }
    }
}

// RPCS3: CELL_EBUSY (not an error for the guest) when the word is not 0 or the exchange found the lock taken.
s32 sys_spinlock_trylock(PPUContext& ctx, u32 lock)
{
    if (!spinlock_word_ok(ctx, lock, "sys_spinlock_trylock")) return err(lv2::CELL_EFAULT);
    u8* base = guest_base(ctx);
    if (atomic_load_be32(base, lock) != 0 || spinlock_exchange(base, lock, kSpinlockTaken) != 0) {
        return err(lv2::CELL_EBUSY);
    }
    return lv2::CELL_OK;
}

// RPCS3: the word becomes 0, whoever holds the lock.
void sys_spinlock_unlock(PPUContext& ctx, u32 lock)
{
    if (!spinlock_word_ok(ctx, lock, "sys_spinlock_unlock")) return;
    spinlock_exchange(guest_base(ctx), lock, 0);
}

// ------------------------------------------------------------------------------------------------- sys_mmapper
// RPCS3 sys_mmapper_.cpp: the library's wrappers of the lv2 shared memory syscalls (runtime/lv2/src/memory.cpp), with
// no IPC key: memory only this process maps.

constexpr u64 SYS_MMAPPER_NO_SHM_KEY = 0xFFFF000000000000ull;

s32 sys_mmapper_allocate_memory(PPUContext& ctx, u32 size, u64 flags, u32 mem_id)
{
    PS_LOG_DEBUG(kChan, "sys_mmapper_allocate_memory(size {:#x}, flags {:#x}, mem_id {:#x})", size, flags, mem_id);
    return lv2::sys_mmapper_allocate_shared_memory(thread_of(ctx), SYS_MMAPPER_NO_SHM_KEY, size, flags, mem_id);
}

s32 sys_mmapper_allocate_memory_from_container(PPUContext& ctx, u32 size, u32 cid, u64 flags, u32 mem_id)
{
    PS_LOG_DEBUG(kChan, "sys_mmapper_allocate_memory_from_container(size {:#x}, container {:#x}, flags {:#x}, mem_id "
                        "{:#x})", size, cid, flags, mem_id);
    return lv2::sys_mmapper_allocate_shared_memory_from_container(thread_of(ctx), SYS_MMAPPER_NO_SHM_KEY, size, cid,
        flags, mem_id);
}

s32 sys_mmapper_map_memory(PPUContext& ctx, u32 addr, u32 mem_id, u64 flags)
{
    PS_LOG_DEBUG(kChan, "sys_mmapper_map_memory(addr {:#x}, mem_id {:#x}, flags {:#x})", addr, mem_id, flags);
    return lv2::sys_mmapper_map_shared_memory(thread_of(ctx), addr, mem_id, flags);
}

s32 sys_mmapper_unmap_memory(PPUContext& ctx, u32 addr, u32 mem_id)
{
    PS_LOG_DEBUG(kChan, "sys_mmapper_unmap_memory(addr {:#x}, mem_id {:#x})", addr, mem_id);
    return lv2::sys_mmapper_unmap_shared_memory(thread_of(ctx), addr, mem_id);
}

s32 sys_mmapper_free_memory(PPUContext& ctx, u32 mem_id)
{
    PS_LOG_DEBUG(kChan, "sys_mmapper_free_memory(mem_id {:#x})", mem_id);
    return lv2::sys_mmapper_free_shared_memory(thread_of(ctx), mem_id);
}

// ------------------------------------------------------------------------------------------------- SFO, random, TTY

// RPCS3 sysPrxForUser.cpp: the syscall _sys_process_get_paramsfo (0x40 bytes, the title ID at +1; CELL_ENOENT when the
// title is not known).
s32 sys_process_get_paramsfo(PPUContext& ctx, u32 buffer)
{
    PS_LOG_DEBUG(kChan, "sys_process_get_paramsfo({:#x})", buffer);
    return lv2::_sys_process_get_paramsfo(thread_of(ctx), buffer);
}

constexpr u64 RANDOM_NUMBER_MAX_SIZE = 4096;

// RPCS3 sysPrxForUser.cpp sys_get_random_number: CELL_EINVAL above 4096 bytes, else lv2's
// sys_ss_random_number_generator(2, addr, size) (lv2/sys_ss.cpp), which fills the buffer from the host's random
// source; its errors, which RPCS3 maps to CELL_ENOMEM, CELL_EAGAIN, CELL_EINVAL or CELL_EABORT, do not happen for these
// sizes. lv2 here has no syscall 865 yet, so the library fills the buffer itself, from std::random_device. A buffer
// that is not guest memory is refused with CELL_EFAULT (D-042; RPCS3 faults).
s32 sys_get_random_number(PPUContext& ctx, u32 addr, u64 size)
{
    PS_LOG_DEBUG(kChan, "sys_get_random_number({:#x}, {})", addr, size);
    if (size > RANDOM_NUMBER_MAX_SIZE) return err(lv2::CELL_EINVAL);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, addr, static_cast<u32>(size))) {
        log_call_site(ctx, base, std::format("sys_get_random_number({:#x}, {}): not guest memory; CELL_EFAULT", addr,
            size));
        return err(lv2::CELL_EFAULT);
    }
    std::random_device random;
    for (u32 i = 0; i < size; i += 4) {
        const u32 bits = random();
        std::memcpy(base + addr + i, &bits, std::min<u64>(4, size - i));
    }
    return lv2::CELL_OK;
}

// RPCS3 sysPrxForUser.cpp console_putc / console_write: the TTY's channel 0 through sys_tty_write (lv2 logs it on the
// "tty" channel), console_putc's character from a stack variable as RPCS3's vm::var. console_write returns CELL_OK
// whatever the write returned (a failure is logged here).
void console_putc(PPUContext& ctx, u8 ch)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar v(ctx, base, 16); // the character, then the written length
    ps_write8(base, v.addr(), ch);
    lv2::sys_tty_write(t, 0, v.addr(), 1, v.addr() + 8);
}

s32 console_write(PPUContext& ctx, u32 data, u32 len)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar written(ctx, base, 4);
    if (const s32 r = lv2::sys_tty_write(t, 0, data, len, written.addr()); r != lv2::CELL_OK) {
        log_call_site(ctx, base, std::format("console_write({:#x}, {}): the TTY write returned {}; CELL_OK as RPCS3",
            data, len, lv2::error_name(r)));
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- sys_mempool
// RPCS3 sys_mempool.cpp: a pool of fixed-size blocks in a chunk of the game's memory, with an lv2 mutex (priority
// order, not recursive) and a condition variable of its own, named "mp_m<ID>" and "mp_c<ID>", that
// sys_mempool_allocate_block waits on for a block. IDs 1..1023 as idm hands them out (memory_pool_t). The pool's
// fields are the host's (MempoolState's lock, held only briefly); the lv2 mutex orders the guest's calls, as in RPCS3.

struct Mempool {
    u32 mutex_id = 0, cond_id = 0;
    u32 chunk = 0;
    u64 chunk_size = 0, block_size = 0, alignment = 0;
    // RPCS3 keeps the free blocks' addresses in a vector, the chunk's blocks in order to begin with, and takes from its
    // end. The same stack in two parts: the chunk's first `fresh` blocks, not taken yet, under the blocks given back
    // (`returned`), so a chunk of many small blocks costs no host memory.
    u64 fresh = 0;
    std::vector<u32> returned;

    u64 count() const { return fresh + returned.size(); }
    u32 take() // count() > 0
    {
        if (!returned.empty()) {
            const u32 block = returned.back();
            returned.pop_back();
            return block;
        }
        fresh--;
        return chunk + static_cast<u32>(fresh) * static_cast<u32>(block_size); // RPCS3's 32-bit arithmetic
    }
};

struct MempoolState {
    std::mutex mutex; // the table and every pool's fields
    IdTable<std::shared_ptr<Mempool>, 1, 1023> pools;

    std::shared_ptr<Mempool> find(u32 id)
    {
        std::lock_guard lock(mutex);
        const std::shared_ptr<Mempool>* p = pools.find(id);
        return p ? *p : nullptr;
    }
    // The pool's lv2 mutex and condition variable.
    std::pair<u32, u32> sync_ids(const Mempool& pool)
    {
        std::lock_guard lock(mutex);
        return {pool.mutex_id, pool.cond_id};
    }
};

MempoolState& mempools() { return HleProcess::current()->state<MempoolState>(); }

// An lv2 object name (8 bytes) as RPCS3's strcpy_trunc writes it: at most 7 characters, NUL-padded.
void put_object_name(u8* base, u32 at, std::string_view name)
{
    std::memcpy(base + at, name.data(), std::min<usize>(name.size(), 7));
}

// RPCS3: CELL_EINVAL for a block larger than the chunk, an alignment that is not a power of two (0 and 2 mean 4), or a
// chunk that is not 8-byte aligned; then the pool with chunk_size / block_size blocks at chunk + i * block_size, its ID
// in *mempool, its mutex and condition variable. Deviations: a block size of 0 is CELL_EINVAL (RPCS3 divides by it),
// a mempool pointer that is not guest memory CELL_EFAULT; with all IDs in use CELL_EAGAIN, and an lv2 object that
// cannot be made returns lv2's error with nothing left behind (RPCS3 crashes or throws).
s32 sys_mempool_create(PPUContext& ctx, u32 mempool, u32 chunk, u64 chunk_size, u64 block_size, u64 ralignment)
{
    PS_LOG_DEBUG(kChan, "sys_mempool_create({:#x}, chunk {:#x}, size {:#x}, block size {:#x}, alignment {})", mempool,
        chunk, chunk_size, block_size, ralignment);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (block_size > chunk_size) return err(lv2::CELL_EINVAL);
    const u64 alignment = ralignment == 0 || ralignment == 2 ? 4 : ralignment;
    if (alignment & (alignment - 1)) return err(lv2::CELL_EINVAL);
    if (chunk % 8) return err(lv2::CELL_EINVAL);
    if (block_size == 0) {
        PS_LOG_ERROR(kChan, "sys_mempool_create: block size 0; CELL_EINVAL (RPCS3 divides by it)");
        return err(lv2::CELL_EINVAL);
    }
    if (!guest_readable(t, mempool, 4)) {
        log_call_site(ctx, base, std::format("sys_mempool_create({:#x}): not guest memory; CELL_EFAULT", mempool));
        return err(lv2::CELL_EFAULT);
    }

    auto pool = std::make_shared<Mempool>();
    pool->chunk = chunk;
    pool->chunk_size = chunk_size;
    pool->block_size = block_size;
    pool->alignment = alignment;
    pool->fresh = chunk_size / block_size;
    MempoolState& s = mempools();
    u32 id = 0;
    {
        std::lock_guard lock(s.mutex);
        id = s.pools.make(pool);
    }
    if (!id) {
        PS_LOG_ERROR(kChan, "sys_mempool_create: all 1023 mempool IDs in use; CELL_EAGAIN");
        return err(lv2::CELL_EAGAIN);
    }

    StackVar v(ctx, base, 0x60); // the mutex ID, the condition variable's, sys_mutex_attribute_t, sys_cond_attribute_t
    const u32 mutex_id = v.addr(), cond_id = v.addr() + 4, mutex_attr = v.addr() + 0x10, cond_attr = v.addr() + 0x40;
    ps_write32(base, mutex_attr + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, mutex_attr + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, mutex_attr + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, mutex_attr + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    put_object_name(base, mutex_attr + 0x20, std::format("mp_m{}", id));
    ps_write32(base, cond_attr + 0x00, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    put_object_name(base, cond_attr + 0x10, std::format("mp_c{}", id));
    s32 r = lv2::sys_mutex_create(t, mutex_id, mutex_attr);
    if (r == lv2::CELL_OK) {
        r = lv2::sys_cond_create(t, cond_id, ps_read32(base, mutex_id), cond_attr);
        if (r != lv2::CELL_OK) lv2::sys_mutex_destroy(t, ps_read32(base, mutex_id));
    }
    std::lock_guard lock(s.mutex);
    if (r != lv2::CELL_OK) {
        s.pools.remove(id);
        PS_LOG_ERROR(kChan, "sys_mempool_create: its mutex or condition variable: {}", lv2::error_name(r));
        return r;
    }
    pool->mutex_id = ps_read32(base, mutex_id);
    pool->cond_id = ps_read32(base, cond_id);
    ps_write32(base, mempool, id);
    return lv2::CELL_OK;
}

// RPCS3: the pool goes under its mutex, then the mutex and the condition variable; an ID that is no pool is logged.
// Deviation: the condition variable is destroyed first (in RPCS3's order lv2 refuses to destroy the mutex, a
// condition variable still bound to it, and it stays).
void sys_mempool_destroy(PPUContext& ctx, u32 mempool)
{
    PS_LOG_DEBUG(kChan, "sys_mempool_destroy({})", mempool);
    lv2::PpuThread& t = thread_of(ctx);
    MempoolState& s = mempools();
    const std::shared_ptr<Mempool> pool = s.find(mempool);
    if (!pool) {
        PS_LOG_ERROR(kChan, "sys_mempool_destroy({}): no such mempool (destroyed already?)", mempool);
        return;
    }
    const auto [m, c] = s.sync_ids(*pool);
    lv2::sys_mutex_lock(t, m, 0);
    {
        std::lock_guard lock(s.mutex);
        if (const std::shared_ptr<Mempool>* p = s.pools.find(mempool); p && *p == pool) s.pools.remove(mempool);
    }
    lv2::sys_mutex_unlock(t, m);
    lv2::sys_cond_destroy(t, c);
    lv2::sys_mutex_destroy(t, m);
}

// RPCS3: CELL_EINVAL for an ID that is no pool or a block past the chunk's end (all it checks); else the block goes
// back on the stack and a waiting sys_mempool_allocate_block is signalled.
s32 sys_mempool_free_block(PPUContext& ctx, u32 mempool, u32 block)
{
    PS_LOG_DEBUG(kChan, "sys_mempool_free_block({}, {:#x})", mempool, block);
    lv2::PpuThread& t = thread_of(ctx);
    MempoolState& s = mempools();
    const std::shared_ptr<Mempool> pool = s.find(mempool);
    if (!pool) return err(lv2::CELL_EINVAL);
    const auto [m, c] = s.sync_ids(*pool);
    lv2::sys_mutex_lock(t, m, 0);
    bool outside = false;
    {
        std::lock_guard lock(s.mutex);
        outside = u64{block} > u64{pool->chunk} + pool->chunk_size;
        if (!outside) pool->returned.push_back(block);
    }
    if (outside) {
        lv2::sys_mutex_unlock(t, m);
        return err(lv2::CELL_EINVAL);
    }
    lv2::sys_cond_signal(t, c);
    lv2::sys_mutex_unlock(t, m);
    return lv2::CELL_OK;
}

// RPCS3: the number of free blocks; CELL_EINVAL for an ID that is no pool, as a u64 (not sign-extended).
u64 sys_mempool_get_count(PPUContext& ctx, u32 mempool)
{
    lv2::PpuThread& t = thread_of(ctx);
    MempoolState& s = mempools();
    const std::shared_ptr<Mempool> pool = s.find(mempool);
    if (!pool) return lv2::CELL_EINVAL;
    const u32 m = s.sync_ids(*pool).first;
    lv2::sys_mutex_lock(t, m, 0);
    u64 count = 0;
    {
        std::lock_guard lock(s.mutex);
        count = pool->count();
    }
    lv2::sys_mutex_unlock(t, m);
    return count;
}

// RPCS3: NULL for an ID that is no pool; else the last free block, waiting on the pool's condition variable while
// there is none (NULL when the pool is destroyed meanwhile, its mutex with it).
u32 sys_mempool_allocate_block(PPUContext& ctx, u32 mempool)
{
    PS_LOG_DEBUG(kChan, "sys_mempool_allocate_block({})", mempool);
    lv2::PpuThread& t = thread_of(ctx);
    MempoolState& s = mempools();
    std::shared_ptr<Mempool> pool = s.find(mempool);
    if (!pool) return 0;
    auto [m, c] = s.sync_ids(*pool);
    lv2::sys_mutex_lock(t, m, 0);
    for (;;) {
        std::optional<u32> block;
        {
            std::lock_guard lock(s.mutex);
            if (pool->count()) block = pool->take();
        }
        if (block) {
            lv2::sys_mutex_unlock(t, m);
            return *block;
        }
        lv2::sys_cond_wait(t, c, 0);
        pool = s.find(mempool);
        if (!pool) return 0;
        std::tie(m, c) = s.sync_ids(*pool);
    }
}

// RPCS3: NULL for an ID that is no pool or a pool without a free block; else its last free block, taken under the
// pool's mutex (waiting for the mutex if another thread holds it). Deviation: the pool is looked at again under the
// mutex, where RPCS3 takes a block without looking again (from an empty vector if another thread took the last).
u32 sys_mempool_try_allocate_block(PPUContext& ctx, u32 mempool)
{
    PS_LOG_DEBUG(kChan, "sys_mempool_try_allocate_block({})", mempool);
    lv2::PpuThread& t = thread_of(ctx);
    MempoolState& s = mempools();
    const std::shared_ptr<Mempool> pool = s.find(mempool);
    if (!pool) return 0;
    {
        std::lock_guard lock(s.mutex);
        if (!pool->count()) return 0;
    }
    const u32 m = s.sync_ids(*pool).first;
    lv2::sys_mutex_lock(t, m, 0);
    u32 block = 0;
    {
        std::lock_guard lock(s.mutex);
        if (pool->count()) block = pool->take();
    }
    lv2::sys_mutex_unlock(t, m);
    return block;
}

// ------------------------------------------------------------------------------------------------- process, SPU images

// RPCS3 sysPrxForUser.cpp sys_process_is_stack: whether the address's top four bits are 0xD, the PPU stack area
// (0xD0000000..0xE0000000 here as well, D-031).
s32 sys_process_is_stack(PPUContext&, u32 addr) { return (addr >> 28) == 0xD ? 1 : 0; }

// sys_spu_image {u32 type, u32 entry_point, u32 segs, s32 nsegs} (RPCS3 sys_spu.h).
constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0, SYS_SPU_IMAGE_TYPE_KERNEL = 1;
constexpr u32 kSyscallSpuImageClose = 158; // _sys_spu_image_close

// Makes lv2 syscall `number` with r3 = `arg` as the guest's `sc` does (Kernel::syscall), for a library function that
// ends in a syscall: one lv2 does not implement is logged there and returns CELL_ENOSYS (CLAUDE.md rule 4).
s32 lv2_syscall(PPUContext& ctx, u32 number, u64 arg)
{
    const u64 saved_r11 = ctx.gpr[11];
    ctx.gpr[11] = number;
    ctx.gpr[3] = arg;
    thread_of(ctx).kernel.syscall(ctx);
    ctx.gpr[11] = saved_r11;
    return static_cast<s32>(ctx.gpr[3]);
}

// RPCS3 sys_spu_.cpp sys_spu_image_close: a user image's segment table, which sys_spu_image_import allocates in the
// main area, is freed there (a table that is no such allocation is logged by the allocator; RPCS3 ignores it); a
// kernel image (its lv2 ID in entry_point) is closed by the syscall _sys_spu_image_close, which lv2 does not have yet
// (nothing opens kernel images: sys_spu_image_open is not implemented either), so the dispatcher reports it and it
// returns CELL_ENOSYS; any other type is CELL_EINVAL. An image that is not guest memory is refused with CELL_EFAULT
// and a log of the call site (D-042; RPCS3 reads it unchecked).
s32 sys_spu_image_close(PPUContext& ctx, u32 img)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, img, 16)) {
        log_call_site(ctx, base, std::format("sys_spu_image_close({:#x}): the image is not readable guest memory; "
            "refused with CELL_EFAULT", img));
        return err(lv2::CELL_EFAULT);
    }
    const u32 type = ps_read32(base, img), segs = ps_read32(base, img + 8);
    PS_LOG_INFO(kChan, "sys_spu_image_close({:#x}): type {}, entry point {:#x}, segments {:#x} ({})", img, type,
        ps_read32(base, img + 4), segs, static_cast<s32>(ps_read32(base, img + 12)));
    if (type == SYS_SPU_IMAGE_TYPE_USER) {
        HleProcess::current()->main_memory().free(segs);
        return lv2::CELL_OK;
    }
    if (type == SYS_SPU_IMAGE_TYPE_KERNEL) return lv2_syscall(ctx, kSyscallSpuImageClose, img);
    return err(lv2::CELL_EINVAL);
}

// ------------------------------------------------------------------------------------------------- exitspawn

// How much of the exitspawn strings is read: more strings or longer ones make the block pass 0x1000 bytes, which
// leaves argv and envp out anyway; only a path longer than 4 KB is cut short.
constexpr u32 kExitspawnMaxStrings = 0x200, kExitspawnMaxString = 0x1000;

// A guest array of string pointers (RPCS3 vm::cpptr<char>) up to its NULL, as far as guest memory goes.
std::vector<std::string> read_string_list(lv2::PpuThread& t, u32 list)
{
    std::vector<std::string> out;
    for (u32 i = 0; list && i < kExitspawnMaxStrings && guest_readable(t, list + 4 * i, 4); i++) {
        const u32 s = ps_read32(t.kernel.base(), list + 4 * i);
        if (!s) break;
        out.push_back(read_log_string(t, s, kExitspawnMaxString));
    }
    return out;
}

// The size of the path and string arrays in the parameter block (RPCS3 sys_game_.cpp get_exitspawn_size): each string
// 16-byte rounded with its NUL plus its 8-byte pointer, 8 bytes for each array's terminating NULL, and 8 more when the
// strings (the path counts as the first argument) are odd in number.
u32 exitspawn_size(const std::string& path, const std::vector<std::string>& args, const std::vector<std::string>& env)
{
    const auto string_size = [](const std::string& s) { return ((static_cast<u32>(s.size()) + 0x10) & ~0xFu) + 8; };
    u32 size = string_size(path) + 8 + 8;
    for (const std::string& s : args) size += string_size(s);
    for (const std::string& s : env) size += string_size(s);
    if ((1 + args.size() + env.size()) % 2) size += 8;
    return size;
}

std::string quoted_list(const std::vector<std::string>& v)
{
    std::string out;
    for (const std::string& s : v) out += std::format("{}\"{}\"", out.empty() ? "" : ", ", s);
    return out;
}

// RPCS3 sys_game_.cpp exitspawn(path, argv, envp, data, data_size, prio, flags), behind sys_game_process_exitspawn and
// sys_game_process_exitspawn2 with the flags each makes (`spawn_flags`; `flags` is what the game passed): the game ends
// itself and asks lv2 to start the executable `path` with these arguments. The parameter block of _sys_process_exit2
// goes in the main area: sys_exit2_param {0x85, 0x30, the size after it, prio, flags, the strings' EA} (u64 each);
// then the path, argv and envp as arrays of u64 pointers that end in 0, and the strings, 16-byte rounded (argv and
// envp are left out when the block would pass 0x1000 bytes); the game's data (up to 4 KB) in the last 0x1000 bytes.
// The flags' top two bits choose a handler to run on this thread first: 0 the one _sys_process_atexitspawn registered,
// 1 the one _sys_process_at_Exitspawn did. Then _sys_process_exit2(0, block, size, 0x10000000), which in pspocket ends
// the process: starting another executable is not implemented (D-031), so the request is logged as an error with its
// path and arguments. Deviation: RPCS3 first locks the "_lv2tls" exit mutex, which pspocket's sys_initialize_tls does
// not create (as sys_process_exit).
void exitspawn(PPUContext& ctx, std::string_view what, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags, u64 spawn_flags)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const std::string file = read_log_string(t, path, kExitspawnMaxString);
    const std::vector<std::string> asked_args = read_string_list(t, argv), asked_env = read_string_list(t, envp);
    std::vector<std::string> args = asked_args, env = asked_env;
    u32 size = exitspawn_size(file, args, env);
    if (size > 0x1000) {
        args.clear();
        env.clear();
        size = exitspawn_size(file, args, env);
    }
    size += 0x30;
    if (data_size > 0) size += 0x1030;
    const u32 copied = std::min<u32>(data_size, 0x1000);
    const bool data_ok = guest_readable(t, data, copied);
    const u32 block = HleProcess::current()->main_memory().alloc(size);
    PS_LOG_ERROR(kChan, "{}(path \"{}\", argv [{}], envp [{}], data {:#x} ({:#x} bytes{}), prio {}, flags {:#x}): "
                        "starting another executable is not implemented (D-031); the process ends (parameter block "
                        "{:#010x}, {:#x} bytes{})",
        what, file, quoted_list(asked_args), quoted_list(asked_env), data, data_size, data_ok ? "" : ", not readable",
        prio, flags, block, size, args.size() < asked_args.size() || env.size() < asked_env.size() ?
            "; argv and envp left out, as RPCS3 does past 0x1000 bytes" : "");
    if (!block) {
        lv2::_sys_process_exit(t, err(lv2::CELL_ENOMEM), 0, 0); // RPCS3: no memory for the block; does not return
        return;
    }

    // RPCS3 put_exitspawn: the pointers from block + 0x30 (path, argv, 0, envp, 0), then the strings.
    const u32 argc = static_cast<u32>(1 + args.size()), envc = static_cast<u32>(env.size());
    u32 slot = block + 0x30;
    u32 str = slot + (argc + envc + (argc + envc) % 2) * 8 + 0x10;
    const auto put = [&](const std::string& s) {
        std::memcpy(base + str, s.data(), s.size());
        ps_write8(base, str + static_cast<u32>(s.size()), 0);
        ps_write64(base, slot, str);
        slot += 8;
        str += (static_cast<u32>(s.size()) + 0x10) & ~0xFu;
    };
    put(file);
    for (const std::string& s : args) put(s);
    ps_write64(base, slot, 0);
    slot += 8;
    for (const std::string& s : env) put(s);
    ps_write64(base, slot, 0);
    if (copied && data_ok) std::memcpy(base + block + size - 0x1000, base + data, copied);

    ps_write64(base, block + 0x00, 0x85);
    ps_write64(base, block + 0x08, 0x30);
    ps_write64(base, block + 0x10, size - 0x30);
    ps_write64(base, block + 0x18, static_cast<u64>(static_cast<s64>(prio)));
    ps_write64(base, block + 0x20, spawn_flags);
    ps_write64(base, block + 0x28, block + 0x30);
    const u64 kind = spawn_flags >> 62;
    const u32 handler = kind == 0 ? atomic_load_be32(base, prx_variable("g_ppu_atexitspawn"))
                        : kind == 1 ? atomic_load_be32(base, prx_variable("g_ppu_at_Exitspawn")) : 0;
    if (handler) {
        PS_LOG_INFO(kChan, "{}: the {} handler {:#x} runs", what, kind == 0 ? "atexitspawn" : "at_Exitspawn", handler);
        if (guest_readable(t, handler, 8)) call_guest(ctx, handler, {});
        else PS_LOG_ERROR(kChan, "{}: the handler {:#x} is not guest memory; skipped", what, handler);
    }
    lv2::_sys_process_exit2(t, 0, block, size, 0x10000000); // does not return
}

// RPCS3 sys_game_.cpp: (flags & 0xf0) with bit 63 set, so neither handler runs.
void sys_game_process_exitspawn(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags)
{
    exitspawn(ctx, "sys_game_process_exitspawn", path, argv, envp, data, data_size, prio, flags,
        (flags & 0xF0) | (u64{1} << 63));
}

// RPCS3 sys_game_.cpp: the flags' top two bits kept when they are 0 or 1 (the atexitspawn or the at_Exitspawn handler
// runs), else cleared with everything else above bit 7 (the atexitspawn handler runs); bits 4..7 kept.
void sys_game_process_exitspawn2(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags)
{
    exitspawn(ctx, "sys_game_process_exitspawn2", path, argv, envp, data, data_size, prio, flags,
        (flags >> 62) >= 2 ? flags & 0xF0 : flags & 0xC0000000000000F0ull);
}

// RPCS3 sys_initialize_tls: the library's lv2 objects, made with the TLS: the "atexit!" lwmutex (g_ppu_atexit_lwm),
// the "_lv2ppu" once mutex (g_ppu_once_mutex), the exit mutex (g_ppu_exit_mutex; liblv2's name "exitprc", RPCS3's
// "_lv2tls") and the "_lv2prx" lwmutex (g_ppu_prx_lwm, which RPCS3 makes and nothing locks).
void init_lv2_objects(PPUContext& ctx)
{
    if (!ctx.runtime) return; // a context without an lv2 thread (tests): made at their first use instead
    atexit_lwmutex(ctx);
    u8* base = guest_base(ctx);
    if (const u32 var = prx_variable("g_ppu_once_mutex"); !atomic_load_be32(base, var)) {
        create_mutex(ctx, var, lv2::SYS_SYNC_NOT_RECURSIVE, "_lv2ppu");
    }
    exit_mutex(ctx);
    create_lwmutex(ctx, "g_ppu_prx_lwm", "_lv2prx");
}

} // namespace

void game_process_exitspawn(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags)
{
    sys_game_process_exitspawn(ctx, path, argv, envp, data, data_size, prio, flags);
}

void game_process_exitspawn2(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags)
{
    sys_game_process_exitspawn2(ctx, path, argv, envp, data, data_size, prio, flags);
}

void register_sysPrxForUser(Registry& r)
{
    // liblv2's internal state under RPCS3's names for it (D-520): the TLS initialisation fills them.
    const auto var = [](std::string_view name, u32 size, u32 align) {
        return Variable{{}, name, recomp::nid_of(name), size, align, nullptr};
    };
    r.add_variables("sysPrxForUser", {var("g_ppu_atexit", 32, 4), var("g_ppu_atexit_lwm", 24, 8),
        var("g_ppu_exit_mutex", 4, 4), var("g_ppu_once_mutex", 4, 4), var("g_ppu_prx_lwm", 24, 8),
        var("g_ppu_atexitspawn", 4, 4), var("g_ppu_at_Exitspawn", 4, 4), var("s_shared_memory_area", 4, 4)});
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x744680A2, sys_initialize_tls),
        PS_HLE_FUNC(0x350D454E, sys_ppu_thread_get_id),
        PS_HLE_FUNC(0x24A1EA07, sys_ppu_thread_create),
        PS_HLE_FUNC(0xAFF080A4, sys_ppu_thread_exit),
        PS_HLE_FUNC(0xA3E3BE68, sys_ppu_thread_once),
        PS_HLE_FUNC(0x3DD4A957, sys_ppu_thread_register_atexit),
        PS_HLE_FUNC(0xAC6FC404, sys_ppu_thread_unregister_atexit),
        PS_HLE_FUNC(0xE6F2C1E7, sys_process_exit),
        PS_HLE_FUNC(0xDE2F9C85, sys_process_atexit),
        PS_HLE_FUNC(0x2C847572, _sys_process_atexitspawn),
        PS_HLE_FUNC(0x96328741, _sys_process_at_Exitspawn),
        PS_HLE_FUNC(0xA2C7BA64, sys_prx_exitspawn_with_level),
        PS_HLE_FUNC(0x42B23552, sys_prx_register_library),
        PS_HLE_FUNC(0x8461E528, sys_time_get_system_time),
        PS_HLE_FUNC(0x2F85C0EF, sys_lwmutex_create),
        PS_HLE_FUNC(0xC3476D0C, sys_lwmutex_destroy),
        PS_HLE_FUNC(0x1573DC3F, sys_lwmutex_lock),
        PS_HLE_FUNC(0xAEB78725, sys_lwmutex_trylock),
        PS_HLE_FUNC(0x1BC200F4, sys_lwmutex_unlock),
        PS_HLE_FUNC(0xDA0EB71A, sys_lwcond_create),
        PS_HLE_FUNC(0x1C9A942C, sys_lwcond_destroy),
        PS_HLE_FUNC(0x2A6D9D51, sys_lwcond_wait),
        PS_HLE_FUNC(0xEF87A695, sys_lwcond_signal),
        PS_HLE_FUNC(0xE9A1BD84, sys_lwcond_signal_all),
        PS_HLE_FUNC(0x52AADADF, sys_lwcond_signal_to),
        PS_HLE_FUNC(0x8C2BB498, sys_spinlock_initialize),
        PS_HLE_FUNC(0xA285139D, sys_spinlock_lock),
        PS_HLE_FUNC(0x722A0254, sys_spinlock_trylock),
        PS_HLE_FUNC(0x5267CB35, sys_spinlock_unlock),
        PS_HLE_FUNC(0xB257540B, sys_mmapper_allocate_memory),
        PS_HLE_FUNC(0x70258515, sys_mmapper_allocate_memory_from_container),
        PS_HLE_FUNC(0xDC578057, sys_mmapper_map_memory),
        PS_HLE_FUNC(0x4643BA6E, sys_mmapper_unmap_memory),
        PS_HLE_FUNC(0x409AD939, sys_mmapper_free_memory),
        PS_HLE_FUNC(0xE75C40F2, sys_process_get_paramsfo),
        PS_HLE_FUNC(0x71A8472A, sys_get_random_number),
        PS_HLE_FUNC(0xE66BAC36, console_putc),
        PS_HLE_FUNC(0xF57E1D6F, console_write),
        PS_HLE_FUNC(0xCA9A60BF, sys_mempool_create),
        PS_HLE_FUNC(0x9D3C0F81, sys_mempool_destroy),
        PS_HLE_FUNC(0xA146A143, sys_mempool_allocate_block),
        PS_HLE_FUNC(0x05C65656, sys_mempool_try_allocate_block),
        PS_HLE_FUNC(0x608212FC, sys_mempool_free_block),
        PS_HLE_FUNC(0x25596F51, sys_mempool_get_count),
        PS_HLE_FUNC(0x67F9FEDB, sys_game_process_exitspawn2),
        PS_HLE_FUNC(0x4F7172C9, sys_process_is_stack),
        PS_HLE_FUNC(0xE0DA8EFD, sys_spu_image_close),
        PS_HLE_FUNC(0xFC52A7A9, sys_game_process_exitspawn),
    });
}

} // namespace pspocket::hle
