#pragma once

// Shared helpers for the SPU tests: a local store, guest memory, register access in guest element order, random
// contexts, whole-context comparison, and an RAII guard for the SPU floating-point environment.

#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/interpreter.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::spu::test {

// xoshiro256** (deterministic, fast).
class Rng {
public:
    explicit Rng(u64 seed)
    {
        u64 z = seed + 0x9E3779B97F4A7C15ull;
        for (u64& s : s_) {
            z += 0x9E3779B97F4A7C15ull;
            u64 x = z;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            s = x ^ (x >> 31);
        }
    }
    u64 next()
    {
        const u64 result = std::rotl(s_[1] * 5, 7) * 9;
        const u64 t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = std::rotl(s_[3], 45);
        return result;
    }
    u32 below(u32 n) { return n ? static_cast<u32>(next() % n) : 0; }
    bool chance(u32 percent) { return below(100) < percent; }

private:
    u64 s_[4];
};

// Enters the SPU FP environment for the scope (pspocket/spu/fp.h).
class FpEnv {
public:
    FpEnv() : token_(ps_spu_fpenv_enter()) {}
    ~FpEnv() { ps_spu_fpenv_leave(token_); }
    FpEnv(const FpEnv&) = delete;
    FpEnv& operator=(const FpEnv&) = delete;

private:
    u64 token_;
};

// A 256 KB local store, 16-byte aligned (128-byte aligned for GETLLAR/PUTLLC lines).
struct LocalStore {
    std::unique_ptr<u8[], void (*)(u8*)> mem{nullptr, [](u8* p) { std::free(p); }};
    LocalStore()
    {
        mem.reset(static_cast<u8*>(std::aligned_alloc(128, PS_SPU_LS_SIZE)));
        if (!mem) throw std::bad_alloc();
        std::memset(mem.get(), 0, PS_SPU_LS_SIZE);
    }
    u8* data() { return mem.get(); }
};

// ---- registers in guest element order (D-004) ----
inline u32 gw(const ps_spu_v128& v, u32 i) { return v.u32[3 - i]; }
inline void sw(ps_spu_v128& v, u32 i, u32 x) { v.u32[3 - i] = x; }
inline u16 gh(const ps_spu_v128& v, u32 i) { return v.u16[7 - i]; }
inline void sh(ps_spu_v128& v, u32 i, u16 x) { v.u16[7 - i] = x; }
inline u8 gb(const ps_spu_v128& v, u32 i) { return v.u8[15 - i]; }
inline void sb(ps_spu_v128& v, u32 i, u8 x) { v.u8[15 - i] = x; }
inline u64 gd(const ps_spu_v128& v, u32 i) { return v.u64[1 - i]; }
inline void sd(ps_spu_v128& v, u32 i, u64 x) { v.u64[1 - i] = x; }

inline ps_spu_v128 words(u32 w0, u32 w1, u32 w2, u32 w3)
{
    ps_spu_v128 v{};
    sw(v, 0, w0);
    sw(v, 1, w1);
    sw(v, 2, w2);
    sw(v, 3, w3);
    return v;
}
inline ps_spu_v128 splat(u32 w) { return words(w, w, w, w); }
inline ps_spu_v128 bytes(std::initializer_list<u8> b)
{
    ps_spu_v128 v{};
    u32 i = 0;
    for (u8 x : b) sb(v, i++, x);
    return v;
}
inline ps_spu_v128 dwords(u64 d0, u64 d1)
{
    ps_spu_v128 v{};
    sd(v, 0, d0);
    sd(v, 1, d1);
    return v;
}
inline u32 fbits(float f) { return std::bit_cast<u32>(f); }
inline u64 dbits(double d) { return std::bit_cast<u64>(d); }

inline std::string hex(const ps_spu_v128& v)
{
    return std::format("{:08x} {:08x} {:08x} {:08x}", gw(v, 0), gw(v, 1), gw(v, 2), gw(v, 3));
}

// One SPU with its LS and (optionally) guest memory, for single-instruction interpreter tests.
struct Machine {
    static constexpr u32 kCode = 0x1000;

    LocalStore ls;
    GuestMemory mem;
    SPUContext ctx{};

    explicit Machine(bool with_guest_memory = false)
    {
        if (with_guest_memory && !mem.reserve()) throw std::runtime_error("guest memory");
        ps_spu_context_init(&ctx, ls.data(), with_guest_memory ? mem.base() : nullptr);
        ctx.xfloat = PS_SPU_XFLOAT_APPROXIMATE;
    }
    ~Machine() { ps_spu_host_detach(&ctx); }

    ps_spu_v128& r(u32 n) { return ctx.gpr[n]; }

    // Executes one instruction at pc on the interpreter (in the SPU FP environment). Returns step()'s result.
    bool exec(u32 insn, u32 pc = kCode)
    {
        ps_st32(ls.data() + pc, insn);
        ctx.pc = pc;
        ctx.stop_reason = PS_SPU_STOP_NONE;
        FpEnv fp;
        return step(ctx);
    }

    // Writes a program (big-endian bytes) at addr.
    void load(u32 addr, const std::vector<u8>& code) { std::memcpy(ls.data() + addr, code.data(), code.size()); }
};

// Every field the fuzz test compares, as text ("" when equal). LS and guest memory are compared separately.
inline std::string diff_contexts(const SPUContext& a, const SPUContext& b, bool compare_pc = true)
{
    std::string d;
    for (u32 i = 0; i < 128; i++)
        if (std::memcmp(&a.gpr[i], &b.gpr[i], 16) != 0) d += std::format(" r{}: {} vs {};", i, hex(a.gpr[i]), hex(b.gpr[i]));
    if (std::memcmp(&a.fpscr, &b.fpscr, 16) != 0) d += " fpscr;";
    if (compare_pc && a.pc != b.pc) d += std::format(" pc: {:05x} vs {:05x};", a.pc, b.pc);
    if (a.srr0 != b.srr0) d += std::format(" srr0: {:05x} vs {:05x};", a.srr0, b.srr0);
    if (a.stop_reason != b.stop_reason) d += std::format(" stop_reason: {} vs {};", a.stop_reason, b.stop_reason);
    if (a.stop_code != b.stop_code) d += std::format(" stop_code: {:#x} vs {:#x};", a.stop_code, b.stop_code);
    if (a.exception != b.exception) d += std::format(" exception: {} vs {};", a.exception, b.exception);
    if (a.interrupts_enabled != b.interrupts_enabled) d += " interrupts_enabled;";
    if (a.attention != b.attention) d += " attention;";
    if (std::memcmp(&a.ch, &b.ch, sizeof a.ch) != 0) {
        const u8* x = reinterpret_cast<const u8*>(&a.ch);
        const u8* y = reinterpret_cast<const u8*>(&b.ch);
        for (u32 i = 0; i < sizeof a.ch; i++)
            if (x[i] != y[i]) {
                d += std::format(" channels differ at byte {:#x};", i);
                break;
            }
    }
    return d;
}

} // namespace pspocket::spu::test
