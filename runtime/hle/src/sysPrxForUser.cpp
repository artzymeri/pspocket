// sysPrxForUser: the liblv2 user-side library (thread and TLS setup, lightweight sync, process exit, ...).
// References: RPCS3 rpcs3/Emu/Cell/Modules/sysPrxForUser.cpp, sys_ppu_thread_.cpp, sys_lwmutex_.cpp, sys_prx_.cpp.
//
// Implemented: what GTA V's first boot log called (docs/runs/004), plus the rest of the lwmutex set that a created
// lwmutex is used through. Everything else stays unimplemented until a log shows the game calls it (CLAUDE.md rule 4).

#include <atomic>

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

} // namespace

void register_sysPrxForUser(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x744680A2, sys_initialize_tls),
        PS_HLE_FUNC(0x350D454E, sys_ppu_thread_get_id),
        PS_HLE_FUNC(0x24A1EA07, sys_ppu_thread_create),
        PS_HLE_FUNC(0xAFF080A4, sys_ppu_thread_exit),
        PS_HLE_FUNC(0xE6F2C1E7, sys_process_exit),
        PS_HLE_FUNC(0x2C847572, _sys_process_atexitspawn),
        PS_HLE_FUNC(0x96328741, _sys_process_at_Exitspawn),
        PS_HLE_FUNC(0xA2C7BA64, sys_prx_exitspawn_with_level),
        PS_HLE_FUNC(0x8461E528, sys_time_get_system_time),
        PS_HLE_FUNC(0x2F85C0EF, sys_lwmutex_create),
        PS_HLE_FUNC(0xC3476D0C, sys_lwmutex_destroy),
        PS_HLE_FUNC(0x1573DC3F, sys_lwmutex_lock),
        PS_HLE_FUNC(0xAEB78725, sys_lwmutex_trylock),
        PS_HLE_FUNC(0x1BC200F4, sys_lwmutex_unlock),
    });
}

} // namespace pspocket::hle
