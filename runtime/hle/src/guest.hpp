#pragma once

// Helpers for HLE function bodies: the calling lv2 thread, guest variables on its stack, and atomics on big-endian
// guest words.

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/core/types.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {

// The lv2 thread running this HLE call (every guest thread is one; tests attach theirs).
inline lv2::PpuThread& thread_of(PPUContext& ctx)
{
    auto* t = static_cast<lv2::PpuThread*>(ctx.runtime);
    if (!t) PS_FATAL("hle", "HLE call at {:#010x} on a context with no lv2 thread", ctx.cia);
    return *t;
}

// The guest memory base of the calling thread's process.
inline u8* guest_base(PPUContext& ctx) { return thread_of(ctx).kernel.base(); }

// A guest variable on the calling thread's stack (RPCS3 vm::var): r1 moves below it for the variable's lifetime, so
// guest code called meanwhile can't overwrite it. 16-byte aligned, zeroed.
class StackVar {
public:
    StackVar(PPUContext& ctx, u8* base, u32 size) : ctx_(ctx), saved_sp_(ctx.gpr[1])
    {
        ctx.gpr[1] = (saved_sp_ - size) & ~u64{15};
        addr_ = static_cast<u32>(ctx.gpr[1]);
        for (u32 i = 0; i < size; i++) ps_write8(base, addr_ + i, 0);
    }
    ~StackVar() { ctx_.gpr[1] = saved_sp_; }
    StackVar(const StackVar&) = delete;
    StackVar& operator=(const StackVar&) = delete;

    u32 addr() const { return addr_; }

private:
    PPUContext& ctx_;
    const u64 saved_sp_;
    u32 addr_ = 0;
};

// Calls the guest function whose descriptor (OPD: entry, TOC) is at `opd` on the calling thread, with up to eight
// arguments in r3..r10, and returns r3: what RPCS3 does with ppu_execute and its callback calls (cellGcmCallback
// through the context, cellSysutilCheckCallback, interrupt handlers). The callee gets a fresh 0x80-byte frame below r1
// with a back chain, because the PPC64 ABI lets it write its caller's LR, TOC and parameter save slots
// (r1 + 16/40/48..) and a StackVar sits right at r1. The caller's r1, r2 and LR are restored afterwards; the callee
// keeps r13..r31 as the ABI requires. Runs through the kernel's executor (lifted code, or the interpreter fallback).
inline u64 call_guest(PPUContext& ctx, u32 opd, std::initializer_list<u64> args)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u64 saved_r1 = ctx.gpr[1], saved_r2 = ctx.gpr[2], saved_lr = ctx.lr;
    const u32 saved_cia = ctx.cia;
    const u32 entry = ps_read32(base, opd);
    ctx.gpr[2] = ps_read32(base, opd + 4);
    u32 r = 3;
    for (const u64 a : args) {
        if (r > 10) PS_FATAL("hle", "call_guest: more than eight arguments");
        ctx.gpr[r++] = a;
    }
    ctx.gpr[1] = (saved_r1 - 0x80) & ~u64{15};
    ps_write64(base, static_cast<u32>(ctx.gpr[1]), saved_r1);
    ctx.lr = lv2::kThreadReturnAddress; // returning lands here; the interpreter fallback stops at it
    lv2::run_guest(t.kernel, ctx, base, entry, false); // a longjmp inside goes on interpreted (D-161)
    const u64 result = ctx.gpr[3];
    ctx.gpr[1] = saved_r1;
    ctx.gpr[2] = saved_r2;
    ctx.lr = saved_lr;
    ctx.cia = saved_cia;
    return result;
}

// Whether [addr, addr + size) is committed guest memory, for sizes up to one host page: a pointer argument checked
// before an HLE function reads it, so a wrong guess about a firmware signature gives a logged error, not a host crash.
inline bool guest_readable(lv2::PpuThread& t, u32 addr, u32 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    return t.kernel.memory().is_committed(addr) && t.kernel.memory().is_committed(addr + size - 1);
}

// Logs (as errors) why an HLE call is refused, the argument registers r3..r10 and the caller's last instructions
// before the call, so a log shows what the game put in each register (implemented in process.cpp).
void log_call_site(PPUContext& ctx, u8* base, std::string_view why);

// Atomics on naturally aligned big-endian guest words (the host is little-endian; values are swapped at the edges).
inline u32* host_u32(u8* base, u32 addr) { return reinterpret_cast<u32*>(base + addr); }
inline u64* host_u64(u8* base, u32 addr) { return reinterpret_cast<u64*>(base + addr); }

inline u32 atomic_load_be32(u8* base, u32 addr)
{
    return ps_bswap32(__atomic_load_n(host_u32(base, addr), __ATOMIC_ACQUIRE));
}

inline void atomic_store_be32(u8* base, u32 addr, u32 value)
{
    __atomic_store_n(host_u32(base, addr), ps_bswap32(value), __ATOMIC_RELEASE);
}

inline u32 atomic_exchange_be32(u8* base, u32 addr, u32 value)
{
    return ps_bswap32(__atomic_exchange_n(host_u32(base, addr), ps_bswap32(value), __ATOMIC_SEQ_CST));
}

// Compare-and-swap; returns the old value (== expected on success).
inline u32 atomic_cas_be32(u8* base, u32 addr, u32 expected, u32 desired)
{
    u32 e = ps_bswap32(expected);
    __atomic_compare_exchange_n(host_u32(base, addr), &e, ps_bswap32(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return ps_bswap32(e);
}

inline bool atomic_cas_be64(u8* base, u32 addr, u64 expected, u64 desired)
{
    u64 e = ps_bswap64(expected);
    return __atomic_compare_exchange_n(host_u64(base, addr), &e, ps_bswap64(desired), false, __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
}

// Adds `delta` to a big-endian u64 (CAS loop: the stored bytes are swapped, so fetch_add can't be used).
inline void atomic_add_be64(u8* base, u32 addr, u64 delta)
{
    u64* p = host_u64(base, addr);
    u64 old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(p, &old, ps_bswap64(ps_bswap64(old) + delta), true, __ATOMIC_SEQ_CST,
        __ATOMIC_RELAXED)) {
    }
}

// An atomic update of the 128-byte guest line at `line` (RPCS3 vm::reservation_op), made through the shared
// reservation system so it is atomic against PPU lwarx/stwcx. and SPU GETLLAR/PUTLLC on the same line. `fn(u8* copy)`
// gets a copy of the line (guest byte order: use ps_read*/ps_write* with offsets from 0) and returns true to store it,
// false to leave the line as it is. It runs again whenever another writer got in first, so it must compute only from
// the copy. Returns what the last `fn` returned.
template <typename Fn>
bool reservation_op(u8* base, u32 line, Fn&& fn)
{
    alignas(16) u8 snapshot[PS_RES_LINE_SIZE];
    alignas(16) u8 copy[PS_RES_LINE_SIZE];
    for (;;) {
        const u64 version = ps_res_acquire(base, line, snapshot);
        std::memcpy(copy, snapshot, sizeof copy);
        if (!fn(copy)) return false;
        if (ps_res_store_line(base, line, version, snapshot, copy)) return true;
    }
}

} // namespace pspocket::hle
