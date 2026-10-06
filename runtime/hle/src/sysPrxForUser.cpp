// sysPrxForUser: the liblv2 user-side library (thread and TLS setup, lightweight sync, process exit, ...).
// References: RPCS3 rpcs3/Emu/Cell/Modules/sysPrxForUser.cpp, sys_ppu_thread_.cpp, sys_lwmutex_.cpp, sys_prx_.cpp,
// sys_spinlock.cpp, sys_mmapper_.cpp, sys_spu_.cpp, sys_game_.cpp.
//
// Implemented: what GTA V's first boot log called (docs/runs/004), plus the rest of the lwmutex set that a created
// lwmutex is used through, and sys_prx_register_library (a GTA V import); Assassin's Creed's imports: the spinlocks,
// the sys_mmapper wrappers, sys_process_is_stack, sys_spu_image_close and sys_game_process_exitspawn. Everything else
// stays unimplemented until a log shows the game calls it (CLAUDE.md rule 4).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sysPrxForUser";

// RPCS3 sys_ppu_thread_.cpp sys_initialize_tls. The loader calls it before the executable's entry with the
// executable's PT_TLS segment (RPCS3 ppu_load_exec queues the same HLE call). Deviation: RPCS3 also creates the
// "atexit!" lwmutex and the "_lv2ppu" mutex here; they back functions that are not implemented yet.
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

// RPCS3 sys_ppu_thread_.cpp sys_ppu_thread_exit: atexit handlers, the TLS block, then the syscall. Deviation: no atexit
// list (sys_ppu_thread_register_atexit is not implemented, so nothing can be registered).
void sys_ppu_thread_exit(PPUContext& ctx, u64 val)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (HleProcess* p = HleProcess::current(); p && ctx.gpr[13] >= kTlsR13Offset) {
        p->free_tls(static_cast<u32>(ctx.gpr[13]) - kTlsR13Offset);
    }
    lv2::_sys_ppu_thread_exit(t, val); // does not return
}

// sys_ppu_thread_once(once_ctrl, init), as RPCS3 (sys_ppu_thread_.cpp): under the process's "_lv2ppu" lv2 mutex
// (priority order, not recursive; RPCS3 creates it with the TLS, here at the first call), a control word still
// SYS_PPU_THREAD_ONCE_INIT (0) runs init (a function descriptor) on the calling thread, then becomes
// SYS_PPU_THREAD_DONE_INIT (1). Other threads asking meanwhile wait on the mutex until it has run.
struct OnceState {
    std::mutex mutex; // the lv2 mutex's creation
    u32 lv2_mutex = 0;
};

void sys_ppu_thread_once(PPUContext& ctx, u32 once_ctrl, u32 init)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, once_ctrl, 4)) {
        log_call_site(ctx, base, std::format("sys_ppu_thread_once: control word {:#x} is not guest memory", once_ctrl));
        return;
    }
    OnceState& s = HleProcess::current()->state<OnceState>();
    u32 m = 0;
    {
        std::lock_guard lock(s.mutex);
        if (!s.lv2_mutex) {
            StackVar v(ctx, base, 0x30); // the ID, then sys_mutex_attribute_t
            const u32 id = v.addr(), attr = v.addr() + 8;
            std::memset(base + attr, 0, 0x28);
            ps_write32(base, attr + 0x00, lv2::SYS_SYNC_PRIORITY);
            ps_write32(base, attr + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
            ps_write32(base, attr + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
            ps_write32(base, attr + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
            std::memcpy(base + attr + 0x20, "_lv2ppu", 8);
            if (const s32 r = lv2::sys_mutex_create(t, id, attr); r != lv2::CELL_OK)
                PS_FATAL(kChan, "sys_ppu_thread_once: the _lv2ppu mutex: {}", lv2::error_name(r));
            s.lv2_mutex = ps_read32(base, id);
        }
        m = s.lv2_mutex;
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

// RPCS3 sysPrxForUser.cpp _sys_process_atexitspawn / _sys_process_at_Exitspawn: remember the first function given.
// They would run around an exitspawn (launching another executable), which pspocket does not do (D-031).
struct ExitspawnState {
    std::atomic<u32> atexitspawn{0};
    std::atomic<u32> at_exitspawn{0};
};

void _sys_process_atexitspawn(PPUContext&, u32 func)
{
    u32 none = 0;
    HleProcess::current()->state<ExitspawnState>().atexitspawn.compare_exchange_strong(none, func);
    PS_LOG_DEBUG(kChan, "_sys_process_atexitspawn({:#x})", func);
}

void _sys_process_at_Exitspawn(PPUContext&, u32 func)
{
    u32 none = 0;
    HleProcess::current()->state<ExitspawnState>().at_exitspawn.compare_exchange_strong(none, func);
    PS_LOG_DEBUG(kChan, "_sys_process_at_Exitspawn({:#x})", func);
}

// RPCS3 sysPrxForUser.cpp sys_process_exit. Deviation: RPCS3 first locks the "_lv2ppu" exit mutex that its
// sys_initialize_tls creates; atexit handlers are a TODO there too.
void sys_process_exit(PPUContext& ctx, s32 status)
{
    PS_LOG_INFO(kChan, "sys_process_exit({})", status);
    lv2::_sys_process_exit(thread_of(ctx), status, 0, 0); // does not return
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

// RPCS3 sys_game_.cpp sys_game_process_exitspawn(path, argv, envp, data, data_size, prio, flags): the game ends itself
// and asks lv2 to start the executable `path` with these arguments. As RPCS3's exitspawn, the parameter block of
// _sys_process_exit2 goes in the main area: sys_exit2_param {0x85, 0x30, the size after it, prio, flags, the strings'
// EA} (u64 each); then the path, argv and envp as arrays of u64 pointers that end in 0, and the strings, 16-byte
// rounded (argv and envp are left out when the block would pass 0x1000 bytes); the game's data (up to 4 KB) in the
// last 0x1000 bytes. The flags are (flags & 0xf0) with bit 63 set, for which neither the atexitspawn nor the
// at_Exitspawn handler runs. Then _sys_process_exit2(0, block, size, 0x10000000), which in pspocket ends the process:
// starting another executable is not implemented (D-031), so the request is logged as an error with its path and
// arguments. Deviation: RPCS3 first locks the "_lv2ppu" exit mutex, which pspocket's sys_initialize_tls does not create
// (as sys_process_exit).
void sys_game_process_exitspawn(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags)
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
    PS_LOG_ERROR(kChan, "sys_game_process_exitspawn(path \"{}\", argv [{}], envp [{}], data {:#x} ({:#x} bytes{}), "
                        "prio {}, flags {:#x}): starting another executable is not implemented (D-031); the process "
                        "ends (parameter block {:#010x}, {:#x} bytes{})",
        file, quoted_list(asked_args), quoted_list(asked_env), data, data_size, data_ok ? "" : ", not readable", prio,
        flags, block, size, args.size() < asked_args.size() || env.size() < asked_env.size() ?
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
    ps_write64(base, block + 0x20, (flags & 0xF0) | (u64{1} << 63));
    ps_write64(base, block + 0x28, block + 0x30);
    lv2::_sys_process_exit2(t, 0, block, size, 0x10000000); // does not return
}

} // namespace

void register_sysPrxForUser(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x744680A2, sys_initialize_tls),
        PS_HLE_FUNC(0x350D454E, sys_ppu_thread_get_id),
        PS_HLE_FUNC(0x24A1EA07, sys_ppu_thread_create),
        PS_HLE_FUNC(0xAFF080A4, sys_ppu_thread_exit),
        PS_HLE_FUNC(0xA3E3BE68, sys_ppu_thread_once),
        PS_HLE_FUNC(0xE6F2C1E7, sys_process_exit),
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
        PS_HLE_FUNC(0x8C2BB498, sys_spinlock_initialize),
        PS_HLE_FUNC(0xA285139D, sys_spinlock_lock),
        PS_HLE_FUNC(0x722A0254, sys_spinlock_trylock),
        PS_HLE_FUNC(0x5267CB35, sys_spinlock_unlock),
        PS_HLE_FUNC(0xB257540B, sys_mmapper_allocate_memory),
        PS_HLE_FUNC(0xDC578057, sys_mmapper_map_memory),
        PS_HLE_FUNC(0x4643BA6E, sys_mmapper_unmap_memory),
        PS_HLE_FUNC(0x409AD939, sys_mmapper_free_memory),
        PS_HLE_FUNC(0x4F7172C9, sys_process_is_stack),
        PS_HLE_FUNC(0xE0DA8EFD, sys_spu_image_close),
        PS_HLE_FUNC(0xFC52A7A9, sys_game_process_exitspawn),
    });
}

} // namespace pspocket::hle
