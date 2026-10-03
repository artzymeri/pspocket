#pragma once

// Helpers for HLE function bodies: the calling lv2 thread, guest variables on its stack, and atomics on big-endian
// guest words.

#include <atomic>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
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

} // namespace pspocket::hle
