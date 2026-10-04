// SPU interpreter. Semantics: IBM SPU ISA v1.2, cross-checked against RPCS3 rpcs3/Emu/Cell/SPUInterpreter.cpp
// (integer, shuffle, branch, load/store forms) and rpcs3/Emu/Cell/SPULLVMRecompiler.cpp (the default "approximate"
// floating-point model, FI's fixed-point form). Deviations are listed in pspocket/spu/fp.h.
//
// Written scalar and in guest element order on purpose: lifted code uses the vectorised helpers of
// pspocket/spu/lifted.h, and the differential fuzz test compares the two.

#include "pspocket/spu/interpreter.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

#include "internal.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/disasm.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"

// Floating-point instructions run in the SPU environment (fp.h); the compiler must not fold or contract them.
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace pspocket::spu {
namespace {

using V = ps_spu_v128;

// ---- guest element access (D-004: guest element i of an N-element vector is host lane N-1-i) ----
inline u8 gb(const V& v, u32 i) { return v.u8[15 - i]; }
inline void sb(V& v, u32 i, u8 x) { v.u8[15 - i] = x; }
inline u16 gh(const V& v, u32 i) { return v.u16[7 - i]; }
inline void sh(V& v, u32 i, u16 x) { v.u16[7 - i] = x; }
inline u32 gw(const V& v, u32 i) { return v.u32[3 - i]; }
inline void sw(V& v, u32 i, u32 x) { v.u32[3 - i] = x; }
inline float gf(const V& v, u32 i) { return v.f32[3 - i]; }
inline void sf(V& v, u32 i, float x) { v.f32[3 - i] = x; }
inline double gd(const V& v, u32 i) { return v.f64[1 - i]; }
inline void sd(V& v, u32 i, double x) { v.f64[1 - i] = x; }

inline V zero_v()
{
    V v;
    std::memset(&v, 0, sizeof v);
    return v;
}

// Value in the preferred slot (guest word 0), other words zero.
inline V preferred(u32 x)
{
    V v = zero_v();
    sw(v, 0, x);
    return v;
}

inline u32 f2u(float f) { return std::bit_cast<u32>(f); }
inline float u2f(u32 u) { return std::bit_cast<float>(u); }
inline double u2d(u64 u) { return std::bit_cast<double>(u); }

// SPU single precision: exponent 0 is zero (denormals included).
inline bool zero_class(u32 x) { return (x & 0x7F800000u) == 0; }
inline bool exp255(u32 x) { return (x & 0x7F800000u) == 0x7F800000u; }
// RPCS3 clamp_smax: exponent-255 values become +-FLT_MAX.
inline u32 clamp_smax(u32 x) { return exp255(x) ? ((x & 0x80000000u) | 0x7F7FFFFFu) : x; }

// 2^k as a double (|k| well inside the double exponent range).
inline double pow2(s32 k) { return u2d(static_cast<u64>(1023 + k) << 52); }

V load_ls(const SPUContext& ctx, u32 addr)
{
    V v;
    const u8* p = ctx.ls + (addr & PS_SPU_QW_MASK);
    for (u32 i = 0; i < 16; i++) sb(v, i, p[i]);
    return v;
}

void store_ls(SPUContext& ctx, u32 addr, const V& v)
{
    u8* p = ctx.ls + (addr & PS_SPU_QW_MASK);
    for (u32 i = 0; i < 16; i++) p[i] = gb(v, i);
}

// ---- single precision (fp.h) ----

float approx_fs(float a, float b) { return a - u2f(clamp_smax(f2u(b))); }

u32 approx_fm(u32 a, u32 b)
{
    if (zero_class(a) || zero_class(b)) return 0;
    return f2u(u2f(a) * u2f(b));
}

u32 approx_fma(u32 a, u32 b, u32 c)
{
    if (zero_class(a) || zero_class(b)) return c;
    return f2u(std::fmaf(u2f(a), u2f(b), u2f(c)));
}

u32 approx_fnms(u32 a, u32 b, u32 c)
{
    return f2u(std::fmaf(-u2f(clamp_smax(a)), u2f(clamp_smax(b)), u2f(c)));
}

u32 approx_fms(u32 a, u32 b, u32 c) { return f2u(std::fmaf(u2f(clamp_smax(a)), u2f(clamp_smax(b)), -u2f(c))); }

inline double xd(u32 x) { return ps_spu_xf_to_f64(x); }
inline u32 xf(double d) { return ps_spu_f64_to_xf(d); }

// csflt / cuflt: exact scaling in double, then the model's conversion to single precision.
u32 int_to_sp(double exact, bool accurate) { return accurate ? xf(exact) : f2u(static_cast<float>(exact)); }

s32 cflts_one(u32 x, u32 i8)
{
    if (zero_class(x)) return 0;
    const double d = xd(x) * pow2(173 - static_cast<s32>(i8));
    if (d >= 2147483648.0) return 0x7FFFFFFF;
    if (d < -2147483648.0) return static_cast<s32>(0x80000000u);
    return static_cast<s32>(static_cast<s64>(d));
}

u32 cfltu_one(u32 x, u32 i8)
{
    if (zero_class(x) || (x & 0x80000000u)) return 0;
    const double d = xd(x) * pow2(173 - static_cast<s32>(i8));
    if (d >= 4294967296.0) return 0xFFFFFFFFu;
    return static_cast<u32>(static_cast<u64>(d));
}

// FCGT/FCMGT/FCEQ/FCMEQ on SPU numbers: denormals equal zero; ordering is the sign-magnitude integer order, which
// puts exponent 255 above everything (RPCS3 SPULLVMRecompiler.cpp, approximate path: both models agree).
bool sp_gt(u32 a, u32 b)
{
    if (zero_class(a) && zero_class(b)) return false;
    const s32 ai = static_cast<s32>(a), bi = static_cast<s32>(b);
    if (ai < 0 && bi < 0) return ai < bi;
    return ai > bi;
}
bool sp_mgt(u32 a, u32 b)
{
    if (zero_class(a) && zero_class(b)) return false;
    return (a & 0x7FFFFFFFu) > (b & 0x7FFFFFFFu);
}
bool sp_eq(u32 a, u32 b) { return (zero_class(a) && zero_class(b)) || a == b; }
bool sp_meq(u32 a, u32 b) { return (zero_class(a) && zero_class(b)) || (a & 0x7FFFFFFFu) == (b & 0x7FFFFFFFu); }

// frest / frsqest: RPCS3 tables (SPUThread.cpp). fi: RPCS3 "spu_fi" fixed-point model (SPULLVMRecompiler.cpp).
u32 frest_one(u32 a)
{
    return ps_spu_frest_fraction_lut[(a >> 18) & 0x1F] | ps_spu_frest_exponent_lut[(a >> 23) & 0xFF] |
        (a & 0x80000000u);
}
u32 frsqest_one(u32 a) { return ps_spu_frsqest_fraction_lut[(a >> 18) & 0x3F] | ps_spu_frsqest_exponent_lut[(a >> 23) & 0xFF]; }
u32 fi_one(u32 a, u32 b)
{
    const u32 base = (b & 0x007FFC00u) << 9;
    const u32 ymul = (b & 0x3FFu) * (a & 0x7FFFFu);
    const bool adjust = ymul > base;
    const u32 bnew = (base - ymul) >> (adjust ? 8 : 9);
    u32 expo = (b & 0xFF800000u) - (adjust ? 0x00800000u : 0u);
    expo = clamp_smax(expo);
    return expo | (bnew & 0x007FFFFFu);
}

// ---- quadword shifts in guest byte order ----

V shl_bytes(const V& a, u32 n)
{
    V r = zero_v();
    for (u32 i = 0; i < 16; i++) sb(r, i, i + n < 16 ? gb(a, i + n) : 0);
    return r;
}
V rot_bytes(const V& a, u32 n)
{
    V r;
    for (u32 i = 0; i < 16; i++) sb(r, i, gb(a, (i + n) & 15));
    return r;
}
V shr_bytes(const V& a, u32 n)
{
    V r = zero_v();
    for (u32 i = 0; i < 16; i++) sb(r, i, i >= n ? gb(a, i - n) : 0);
    return r;
}
V shl_bits(const V& a, u32 n) // n < 8
{
    V r;
    for (u32 i = 0; i < 16; i++) {
        const u32 next = i < 15 ? gb(a, i + 1) : 0;
        sb(r, i, static_cast<u8>((gb(a, i) << n) | (n ? next >> (8 - n) : 0)));
    }
    return r;
}
V rot_bits(const V& a, u32 n)
{
    V r;
    for (u32 i = 0; i < 16; i++) {
        const u32 next = gb(a, (i + 1) & 15);
        sb(r, i, static_cast<u8>((gb(a, i) << n) | (n ? next >> (8 - n) : 0)));
    }
    return r;
}
V shr_bits(const V& a, u32 n)
{
    V r;
    for (u32 i = 0; i < 16; i++) {
        const u32 prev = i > 0 ? gb(a, i - 1) : 0;
        sb(r, i, static_cast<u8>((gb(a, i) >> n) | (n ? prev << (8 - n) : 0)));
    }
    return r;
}

// cbd/chd/cwd/cdd and the x forms: identity shuffle mask selecting rb bytes (0x10 + i), with the insertion slot for
// a byte/halfword/word/doubleword at address addr (selecting ra's preferred-slot bytes).
V insertion_mask(u32 addr, u32 size)
{
    V r;
    for (u32 i = 0; i < 16; i++) sb(r, i, static_cast<u8>(0x10 + i));
    const u32 t = addr & 0xF & ~(size - 1);
    const u32 first = size == 1 ? 3 : size == 2 ? 2 : 0; // preferred slot of ra for the element size
    for (u32 k = 0; k < size; k++) sb(r, t + k, static_cast<u8>(first + k));
    return r;
}

bool branch_to(SPUContext& ctx, const Inst& inst, u32 target)
{
    ctx.pc = target & PS_SPU_PC_MASK;
    if (inst.e() || inst.d()) ps_spu_set_interrupts(&ctx, inst.e() ? 1 : 0);
    return true;
}

[[maybe_unused]] void fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }

} // namespace

bool execute(SPUContext& ctx, const Inst& inst)
{
    const u32 pc = ctx.pc;
    if (!is_supported(inst)) return ps_spu_illegal(&ctx, pc, inst.raw) == 0;

    // Operands are copied first: rt may also be a source.
    const V a = ctx.gpr[inst.ra()];
    const V b = ctx.gpr[inst.rb()];
    const V t = ctx.gpr[inst.rt()];
    V r = zero_v();
    u32 next = pc + 4;
    const bool accurate = ctx.xfloat == PS_SPU_XFLOAT_ACCURATE;

    switch (inst.op) {
    // ---- control ----
    case Op::STOP:
        return ps_spu_stop(&ctx, pc, inst.stop_signal()) == 0 ? (ctx.pc = pc + 4, true) : false;
    case Op::STOPD: return ps_spu_stop(&ctx, pc, 0x3FFF) == 0 ? (ctx.pc = pc + 4, true) : false;
    case Op::LNOP:
    case Op::NOP:
    case Op::HBR:
    case Op::HBRA:
    case Op::HBRR:
    case Op::MTSPR: ctx.pc = next; return true;
    case Op::SYNC:
    case Op::DSYNC:
        fence();
        ctx.pc = next;
        return true;
    case Op::MFSPR: ctx.gpr[inst.rt()] = zero_v(); break; // every SPR reads as zero (as RPCS3)
    case Op::RDCH: {
        u32 value = 0;
        if (ps_spu_rdch(&ctx, pc, inst.channel(), &value)) return false;
        ctx.gpr[inst.rt()] = preferred(value);
        break;
    }
    case Op::RCHCNT: {
        u32 count = 0;
        if (ps_spu_rchcnt(&ctx, pc, inst.channel(), &count)) return false;
        ctx.gpr[inst.rt()] = preferred(count);
        break;
    }
    case Op::WRCH:
        if (ps_spu_wrch(&ctx, pc, inst.channel(), gw(t, 0))) return false;
        break;

    // ---- halt ----
    case Op::HEQ:
        if (static_cast<s32>(gw(a, 0)) == static_cast<s32>(gw(b, 0))) return ps_spu_halt(&ctx, pc) == 0;
        break;
    case Op::HGT:
        if (static_cast<s32>(gw(a, 0)) > static_cast<s32>(gw(b, 0))) return ps_spu_halt(&ctx, pc) == 0;
        break;
    case Op::HLGT:
        if (gw(a, 0) > gw(b, 0)) return ps_spu_halt(&ctx, pc) == 0;
        break;
    case Op::HEQI:
        if (static_cast<s32>(gw(a, 0)) == inst.si10()) return ps_spu_halt(&ctx, pc) == 0;
        break;
    case Op::HGTI:
        if (static_cast<s32>(gw(a, 0)) > inst.si10()) return ps_spu_halt(&ctx, pc) == 0;
        break;
    case Op::HLGTI:
        if (gw(a, 0) > static_cast<u32>(inst.si10())) return ps_spu_halt(&ctx, pc) == 0;
        break;

    // ---- loads and stores ----
    case Op::LQD: ctx.gpr[inst.rt()] = load_ls(ctx, static_cast<u32>(static_cast<s32>(gw(a, 0)) + inst.si10() * 16)); break;
    case Op::LQX: ctx.gpr[inst.rt()] = load_ls(ctx, gw(a, 0) + gw(b, 0)); break;
    case Op::LQA:
    case Op::LQR: ctx.gpr[inst.rt()] = load_ls(ctx, ls_target(inst, pc)); break;
    case Op::STQD: store_ls(ctx, static_cast<u32>(static_cast<s32>(gw(a, 0)) + inst.si10() * 16), t); break;
    case Op::STQX: store_ls(ctx, gw(a, 0) + gw(b, 0), t); break;
    case Op::STQA:
    case Op::STQR: store_ls(ctx, ls_target(inst, pc), t); break;

    // ---- branches ----
    case Op::BR:
    case Op::BRA: ctx.pc = branch_target(inst, pc); return true;
    case Op::BRSL:
    case Op::BRASL:
        ctx.gpr[inst.rt()] = preferred((pc + 4) & PS_SPU_PC_MASK);
        ctx.pc = branch_target(inst, pc);
        return true;
    case Op::BRZ: ctx.pc = gw(t, 0) == 0 ? branch_target(inst, pc) : next; return true;
    case Op::BRNZ: ctx.pc = gw(t, 0) != 0 ? branch_target(inst, pc) : next; return true;
    case Op::BRHZ: ctx.pc = gh(t, 1) == 0 ? branch_target(inst, pc) : next; return true;
    case Op::BRHNZ: ctx.pc = gh(t, 1) != 0 ? branch_target(inst, pc) : next; return true;
    case Op::BI: return branch_to(ctx, inst, gw(a, 0));
    case Op::BISL:
        ctx.gpr[inst.rt()] = preferred((pc + 4) & PS_SPU_PC_MASK);
        return branch_to(ctx, inst, gw(a, 0));
    case Op::IRET: return branch_to(ctx, inst, ctx.srr0);
    case Op::BISLED:
        ctx.gpr[inst.rt()] = preferred((pc + 4) & PS_SPU_PC_MASK);
        if (ps_spu_events_pending(&ctx)) return branch_to(ctx, inst, gw(a, 0));
        break;
    case Op::BIZ:
        if (gw(t, 0) == 0) return branch_to(ctx, inst, gw(a, 0));
        break;
    case Op::BINZ:
        if (gw(t, 0) != 0) return branch_to(ctx, inst, gw(a, 0));
        break;
    case Op::BIHZ:
        if (gh(t, 1) == 0) return branch_to(ctx, inst, gw(a, 0));
        break;
    case Op::BIHNZ:
        if (gh(t, 1) != 0) return branch_to(ctx, inst, gw(a, 0));
        break;

    // ---- constant formation ----
    case Op::IL:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>(inst.si16()));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ILHU:
        for (u32 i = 0; i < 4; i++) sw(r, i, inst.i16() << 16);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ILH:
        for (u32 i = 0; i < 8; i++) sh(r, i, static_cast<u16>(inst.i16()));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ILA:
        for (u32 i = 0; i < 4; i++) sw(r, i, inst.i18());
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::IOHL:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(t, i) | inst.i16());
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FSMBI:
        for (u32 i = 0; i < 16; i++) sb(r, i, ((inst.i16() >> (15 - i)) & 1) ? 0xFF : 0x00);
        ctx.gpr[inst.rt()] = r;
        break;

    // ---- integer arithmetic ----
    case Op::A:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(a, i) + gw(b, i));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::AH:
        for (u32 i = 0; i < 8; i++) sh(r, i, static_cast<u16>(gh(a, i) + gh(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::AI:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(a, i) + static_cast<u32>(inst.si10()));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::AHI:
        for (u32 i = 0; i < 8; i++) sh(r, i, static_cast<u16>(gh(a, i) + static_cast<u16>(inst.si10())));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SF:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(b, i) - gw(a, i));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SFH:
        for (u32 i = 0; i < 8; i++) sh(r, i, static_cast<u16>(gh(b, i) - gh(a, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SFI:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>(inst.si10()) - gw(a, i));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SFHI:
        for (u32 i = 0; i < 8; i++) sh(r, i, static_cast<u16>(static_cast<u16>(inst.si10()) - gh(a, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ADDX:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(a, i) + gw(b, i) + (gw(t, i) & 1));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SFX:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(b, i) - gw(a, i) - (~gw(t, i) & 1));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CG:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>((u64{gw(a, i)} + gw(b, i)) >> 32));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CGX:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>((u64{gw(a, i)} + gw(b, i) + (gw(t, i) & 1)) >> 32));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::BG:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(b, i) >= gw(a, i) ? 1 : 0);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::BGX:
        for (u32 i = 0; i < 4; i++) {
            const s64 diff = static_cast<s64>(u64{gw(b, i)}) - static_cast<s64>(u64{gw(a, i)}) - (1 - (gw(t, i) & 1));
            sw(r, i, diff >= 0 ? 1 : 0);
        }
        ctx.gpr[inst.rt()] = r;
        break;

    // ---- multiplies (signedness: mpy/mpyh/mpyhh/mpys/mpya/mpyi/mpyhha signed, *u unsigned) ----
    case Op::MPY:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, static_cast<u32>(s32{static_cast<s16>(gw(a, i))} * s32{static_cast<s16>(gw(b, i))}));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYU:
        for (u32 i = 0; i < 4; i++) sw(r, i, (gw(a, i) & 0xFFFF) * (gw(b, i) & 0xFFFF));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYI:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>(s32{static_cast<s16>(gw(a, i))} * inst.si10()));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYUI:
        for (u32 i = 0; i < 4; i++) sw(r, i, (gw(a, i) & 0xFFFF) * (static_cast<u32>(inst.si10()) & 0xFFFF));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYH:
        for (u32 i = 0; i < 4; i++) sw(r, i, ((gw(a, i) >> 16) * (gw(b, i) & 0xFFFF)) << 16);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYS:
        for (u32 i = 0; i < 4; i++) {
            const s32 p = s32{static_cast<s16>(gw(a, i))} * s32{static_cast<s16>(gw(b, i))};
            sw(r, i, static_cast<u32>(p >> 16)); // arithmetic shift: sign-extended upper half
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYHH:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, static_cast<u32>(s32{static_cast<s16>(gw(a, i) >> 16)} * s32{static_cast<s16>(gw(b, i) >> 16)}));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYHHA:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, gw(t, i) + static_cast<u32>(s32{static_cast<s16>(gw(a, i) >> 16)} * s32{static_cast<s16>(gw(b, i) >> 16)}));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYHHU:
        for (u32 i = 0; i < 4; i++) sw(r, i, (gw(a, i) >> 16) * (gw(b, i) >> 16));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYHHAU:
        for (u32 i = 0; i < 4; i++) sw(r, i, gw(t, i) + (gw(a, i) >> 16) * (gw(b, i) >> 16));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::MPYA: {
        const V c = ctx.gpr[inst.rc()];
        for (u32 i = 0; i < 4; i++)
            sw(r, i, gw(c, i) + static_cast<u32>(s32{static_cast<s16>(gw(a, i))} * s32{static_cast<s16>(gw(b, i))}));
        ctx.gpr[inst.rt4()] = r;
        break;
    }

    // ---- bit counting, masks, gather ----
    case Op::CLZ:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>(std::countl_zero(gw(a, i))));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CNTB:
        for (u32 i = 0; i < 16; i++) sb(r, i, static_cast<u8>(std::popcount(gb(a, i))));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FSMB:
        for (u32 i = 0; i < 16; i++) sb(r, i, ((gw(a, 0) >> (15 - i)) & 1) ? 0xFF : 0x00);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FSMH:
        for (u32 i = 0; i < 8; i++) sh(r, i, ((gw(a, 0) >> (7 - i)) & 1) ? 0xFFFF : 0x0000);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FSM:
        for (u32 i = 0; i < 4; i++) sw(r, i, ((gw(a, 0) >> (3 - i)) & 1) ? 0xFFFFFFFFu : 0);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::GBB: {
        u32 bits = 0;
        for (u32 i = 0; i < 16; i++) bits = (bits << 1) | (gb(a, i) & 1);
        ctx.gpr[inst.rt()] = preferred(bits);
        break;
    }
    case Op::GBH: {
        u32 bits = 0;
        for (u32 i = 0; i < 8; i++) bits = (bits << 1) | (gh(a, i) & 1);
        ctx.gpr[inst.rt()] = preferred(bits);
        break;
    }
    case Op::GB: {
        u32 bits = 0;
        for (u32 i = 0; i < 4; i++) bits = (bits << 1) | (gw(a, i) & 1);
        ctx.gpr[inst.rt()] = preferred(bits);
        break;
    }
    case Op::AVGB:
        for (u32 i = 0; i < 16; i++) sb(r, i, static_cast<u8>((u32{gb(a, i)} + gb(b, i) + 1) >> 1));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ABSDB:
        for (u32 i = 0; i < 16; i++) sb(r, i, static_cast<u8>(gb(a, i) > gb(b, i) ? gb(a, i) - gb(b, i) : gb(b, i) - gb(a, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SUMB:
        for (u32 w = 0; w < 4; w++) {
            u32 sa = 0, sbb = 0;
            for (u32 k = 0; k < 4; k++) {
                sa += gb(a, w * 4 + k);
                sbb += gb(b, w * 4 + k);
            }
            sh(r, w * 2, static_cast<u16>(sbb)); // upper halfword: rb's bytes
            sh(r, w * 2 + 1, static_cast<u16>(sa));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::XSBH:
        for (u32 i = 0; i < 8; i++) sh(r, i, static_cast<u16>(static_cast<s16>(static_cast<s8>(gh(a, i) & 0xFF))));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::XSHW:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>(static_cast<s32>(static_cast<s16>(gw(a, i) & 0xFFFF))));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::XSWD:
        for (u32 i = 0; i < 2; i++) {
            const s64 v = static_cast<s32>(gw(a, i * 2 + 1));
            sw(r, i * 2, static_cast<u32>(static_cast<u64>(v) >> 32));
            sw(r, i * 2 + 1, static_cast<u32>(v));
        }
        ctx.gpr[inst.rt()] = r;
        break;

    // ---- logical ----
    case Op::AND:
    case Op::ANDC:
    case Op::OR:
    case Op::ORC:
    case Op::XOR:
    case Op::NAND:
    case Op::NOR:
    case Op::EQV:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = gw(a, i), y = gw(b, i);
            u32 v = 0;
            switch (inst.op) {
            case Op::AND: v = x & y; break;
            case Op::ANDC: v = x & ~y; break;
            case Op::OR: v = x | y; break;
            case Op::ORC: v = x | ~y; break;
            case Op::XOR: v = x ^ y; break;
            case Op::NAND: v = ~(x & y); break;
            case Op::NOR: v = ~(x | y); break;
            default: v = ~(x ^ y); break;
            }
            sw(r, i, v);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ANDI:
    case Op::ORI:
    case Op::XORI:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = gw(a, i), y = static_cast<u32>(inst.si10());
            sw(r, i, inst.op == Op::ANDI ? x & y : inst.op == Op::ORI ? x | y : x ^ y);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ANDHI:
    case Op::ORHI:
    case Op::XORHI:
        for (u32 i = 0; i < 8; i++) {
            const u16 x = gh(a, i), y = static_cast<u16>(inst.si10());
            sh(r, i, static_cast<u16>(inst.op == Op::ANDHI ? x & y : inst.op == Op::ORHI ? x | y : x ^ y));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ANDBI:
    case Op::ORBI:
    case Op::XORBI:
        for (u32 i = 0; i < 16; i++) {
            const u8 x = gb(a, i), y = static_cast<u8>(inst.i10() & 0xFF);
            sb(r, i, static_cast<u8>(inst.op == Op::ANDBI ? x & y : inst.op == Op::ORBI ? x | y : x ^ y));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ORX:
        ctx.gpr[inst.rt()] = preferred(gw(a, 0) | gw(a, 1) | gw(a, 2) | gw(a, 3));
        break;
    case Op::SELB: {
        const V c = ctx.gpr[inst.rc()];
        for (u32 i = 0; i < 4; i++) sw(r, i, (gw(c, i) & gw(b, i)) | (~gw(c, i) & gw(a, i)));
        ctx.gpr[inst.rt4()] = r;
        break;
    }
    case Op::SHUFB: {
        const V c = ctx.gpr[inst.rc()];
        for (u32 i = 0; i < 16; i++) {
            const u8 x = gb(c, i);
            u8 v;
            if (x & 0x80) {
                v = (x & 0xE0) == 0xC0 ? 0xFF : (x & 0xE0) == 0xE0 ? 0x80 : 0x00;
            } else {
                const u32 sel = x & 0x1F;
                v = sel < 16 ? gb(a, sel) : gb(b, sel - 16);
            }
            sb(r, i, v);
        }
        ctx.gpr[inst.rt4()] = r;
        break;
    }

    // ---- element shifts and rotates ----
    case Op::SHLH:
        for (u32 i = 0; i < 8; i++) {
            const u32 n = gh(b, i) & 0x1F;
            sh(r, i, static_cast<u16>(n < 16 ? gh(a, i) << n : 0));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SHLHI:
        for (u32 i = 0; i < 8; i++) {
            const u32 n = inst.i7() & 0x1F;
            sh(r, i, static_cast<u16>(n < 16 ? gh(a, i) << n : 0));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SHL:
        for (u32 i = 0; i < 4; i++) {
            const u32 n = gw(b, i) & 0x3F;
            sw(r, i, n < 32 ? gw(a, i) << n : 0);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::SHLI:
        for (u32 i = 0; i < 4; i++) {
            const u32 n = inst.i7() & 0x3F;
            sw(r, i, n < 32 ? gw(a, i) << n : 0);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROTH:
        for (u32 i = 0; i < 8; i++) sh(r, i, std::rotl(gh(a, i), static_cast<int>(gh(b, i) & 0xF)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROTHI:
        for (u32 i = 0; i < 8; i++) sh(r, i, std::rotl(gh(a, i), static_cast<int>(inst.i7() & 0xF)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROT:
        for (u32 i = 0; i < 4; i++) sw(r, i, std::rotl(gw(a, i), static_cast<int>(gw(b, i) & 0x1F)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROTI:
        for (u32 i = 0; i < 4; i++) sw(r, i, std::rotl(gw(a, i), static_cast<int>(inst.i7() & 0x1F)));
        ctx.gpr[inst.rt()] = r;
        break;
    // rotm/rotma/rothm/rotmah: shift right by (0 - count) masked to 6 (words) or 5 (halfwords) bits; counts beyond
    // the element width give 0 (logical) or the sign (arithmetic).
    case Op::ROTHM:
    case Op::ROTHMI:
        for (u32 i = 0; i < 8; i++) {
            const u32 n = (0u - (inst.op == Op::ROTHM ? gh(b, i) : inst.i7())) & 0x1F;
            sh(r, i, static_cast<u16>(n < 16 ? gh(a, i) >> n : 0));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROTMAH:
    case Op::ROTMAHI:
        for (u32 i = 0; i < 8; i++) {
            const u32 n = (0u - (inst.op == Op::ROTMAH ? gh(b, i) : inst.i7())) & 0x1F;
            const s32 v = static_cast<s16>(gh(a, i));
            sh(r, i, static_cast<u16>(v >> (n < 16 ? n : 15)));
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROTM:
    case Op::ROTMI:
        for (u32 i = 0; i < 4; i++) {
            const u32 n = (0u - (inst.op == Op::ROTM ? gw(b, i) : inst.i7())) & 0x3F;
            sw(r, i, n < 32 ? gw(a, i) >> n : 0);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::ROTMA:
    case Op::ROTMAI:
        for (u32 i = 0; i < 4; i++) {
            const u32 n = (0u - (inst.op == Op::ROTMA ? gw(b, i) : inst.i7())) & 0x3F;
            const s32 v = static_cast<s32>(gw(a, i));
            sw(r, i, static_cast<u32>(v >> (n < 32 ? n : 31)));
        }
        ctx.gpr[inst.rt()] = r;
        break;

    // ---- quadword shifts and rotates (count from the preferred slot of rb, or the immediate) ----
    case Op::SHLQBI: ctx.gpr[inst.rt()] = shl_bits(a, gw(b, 0) & 7); break;
    case Op::SHLQBII: ctx.gpr[inst.rt()] = shl_bits(a, inst.i7() & 7); break;
    case Op::ROTQBI: ctx.gpr[inst.rt()] = rot_bits(a, gw(b, 0) & 7); break;
    case Op::ROTQBII: ctx.gpr[inst.rt()] = rot_bits(a, inst.i7() & 7); break;
    case Op::ROTQMBI: ctx.gpr[inst.rt()] = shr_bits(a, (0u - gw(b, 0)) & 7); break;
    case Op::ROTQMBII: ctx.gpr[inst.rt()] = shr_bits(a, (0u - inst.i7()) & 7); break;
    case Op::SHLQBY: ctx.gpr[inst.rt()] = shl_bytes(a, gw(b, 0) & 0x1F); break;
    case Op::SHLQBYI: ctx.gpr[inst.rt()] = shl_bytes(a, inst.i7() & 0x1F); break;
    case Op::SHLQBYBI: ctx.gpr[inst.rt()] = shl_bytes(a, (gw(b, 0) >> 3) & 0x1F); break;
    case Op::ROTQBY: ctx.gpr[inst.rt()] = rot_bytes(a, gw(b, 0) & 0xF); break;
    case Op::ROTQBYI: ctx.gpr[inst.rt()] = rot_bytes(a, inst.i7() & 0xF); break;
    case Op::ROTQBYBI: ctx.gpr[inst.rt()] = rot_bytes(a, (gw(b, 0) >> 3) & 0xF); break;
    case Op::ROTQMBY: ctx.gpr[inst.rt()] = shr_bytes(a, (0u - gw(b, 0)) & 0x1F); break;
    case Op::ROTQMBYI: ctx.gpr[inst.rt()] = shr_bytes(a, (0u - inst.i7()) & 0x1F); break;
    case Op::ROTQMBYBI: ctx.gpr[inst.rt()] = shr_bytes(a, (0u - (gw(b, 0) >> 3)) & 0x1F); break;

    // ---- insertion masks ----
    case Op::CBD: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + static_cast<u32>(inst.si7()), 1); break;
    case Op::CHD: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + static_cast<u32>(inst.si7()), 2); break;
    case Op::CWD: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + static_cast<u32>(inst.si7()), 4); break;
    case Op::CDD: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + static_cast<u32>(inst.si7()), 8); break;
    case Op::CBX: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + gw(b, 0), 1); break;
    case Op::CHX: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + gw(b, 0), 2); break;
    case Op::CWX: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + gw(b, 0), 4); break;
    case Op::CDX: ctx.gpr[inst.rt()] = insertion_mask(gw(a, 0) + gw(b, 0), 8); break;

    // ---- integer compares ----
    case Op::CEQB:
    case Op::CEQBI:
    case Op::CGTB:
    case Op::CGTBI:
    case Op::CLGTB:
    case Op::CLGTBI:
        for (u32 i = 0; i < 16; i++) {
            const u8 x = gb(a, i);
            const bool imm = inst.op == Op::CEQBI || inst.op == Op::CGTBI || inst.op == Op::CLGTBI;
            const u8 y = imm ? static_cast<u8>(inst.i10() & 0xFF) : gb(b, i);
            bool c = false;
            if (inst.op == Op::CEQB || inst.op == Op::CEQBI) c = x == y;
            else if (inst.op == Op::CGTB || inst.op == Op::CGTBI) c = static_cast<s8>(x) > static_cast<s8>(y);
            else c = x > y;
            sb(r, i, c ? 0xFF : 0x00);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CEQH:
    case Op::CEQHI:
    case Op::CGTH:
    case Op::CGTHI:
    case Op::CLGTH:
    case Op::CLGTHI:
        for (u32 i = 0; i < 8; i++) {
            const u16 x = gh(a, i);
            const bool imm = inst.op == Op::CEQHI || inst.op == Op::CGTHI || inst.op == Op::CLGTHI;
            const u16 y = imm ? static_cast<u16>(inst.si10()) : gh(b, i);
            bool c = false;
            if (inst.op == Op::CEQH || inst.op == Op::CEQHI) c = x == y;
            else if (inst.op == Op::CGTH || inst.op == Op::CGTHI) c = static_cast<s16>(x) > static_cast<s16>(y);
            else c = x > y;
            sh(r, i, c ? 0xFFFF : 0x0000);
        }
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CEQ:
    case Op::CEQI:
    case Op::CGT:
    case Op::CGTI:
    case Op::CLGT:
    case Op::CLGTI:
        for (u32 i = 0; i < 4; i++) {
            const u32 x = gw(a, i);
            const bool imm = inst.op == Op::CEQI || inst.op == Op::CGTI || inst.op == Op::CLGTI;
            const u32 y = imm ? static_cast<u32>(inst.si10()) : gw(b, i);
            bool c = false;
            if (inst.op == Op::CEQ || inst.op == Op::CEQI) c = x == y;
            else if (inst.op == Op::CGT || inst.op == Op::CGTI) c = static_cast<s32>(x) > static_cast<s32>(y);
            else c = x > y;
            sw(r, i, c ? 0xFFFFFFFFu : 0);
        }
        ctx.gpr[inst.rt()] = r;
        break;

    // ---- single-precision floating point ----
    case Op::FA:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, accurate ? xf(xd(gw(a, i)) + xd(gw(b, i))) : f2u(gf(a, i) + gf(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FS:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, accurate ? xf(xd(gw(a, i)) - xd(gw(b, i))) : f2u(approx_fs(gf(a, i), gf(b, i))));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FM:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, accurate ? xf(xd(gw(a, i)) * xd(gw(b, i))) : approx_fm(gw(a, i), gw(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FMA:
    case Op::FNMS:
    case Op::FMS: {
        const V c = ctx.gpr[inst.rc()];
        for (u32 i = 0; i < 4; i++) {
            const u32 x = gw(a, i), y = gw(b, i), z = gw(c, i);
            u32 v;
            if (accurate) {
                const double dx = xd(x), dy = xd(y), dz = xd(z);
                v = inst.op == Op::FMA ? xf(std::fma(dx, dy, dz))
                    : inst.op == Op::FNMS ? xf(std::fma(-dx, dy, dz))
                                          : xf(std::fma(dx, dy, -dz));
            } else {
                v = inst.op == Op::FMA ? approx_fma(x, y, z) : inst.op == Op::FNMS ? approx_fnms(x, y, z) : approx_fms(x, y, z);
            }
            sw(r, i, v);
        }
        ctx.gpr[inst.rt4()] = r;
        break;
    }
    case Op::FCGT:
        for (u32 i = 0; i < 4; i++) sw(r, i, sp_gt(gw(a, i), gw(b, i)) ? 0xFFFFFFFFu : 0);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FCMGT:
        for (u32 i = 0; i < 4; i++) sw(r, i, sp_mgt(gw(a, i), gw(b, i)) ? 0xFFFFFFFFu : 0);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FCEQ:
        for (u32 i = 0; i < 4; i++) sw(r, i, sp_eq(gw(a, i), gw(b, i)) ? 0xFFFFFFFFu : 0);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FCMEQ:
        for (u32 i = 0; i < 4; i++) sw(r, i, sp_meq(gw(a, i), gw(b, i)) ? 0xFFFFFFFFu : 0);
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FREST:
        for (u32 i = 0; i < 4; i++) sw(r, i, frest_one(gw(a, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FRSQEST:
        for (u32 i = 0; i < 4; i++) sw(r, i, frsqest_one(gw(a, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FI:
        for (u32 i = 0; i < 4; i++) sw(r, i, fi_one(gw(a, i), gw(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CSFLT:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, int_to_sp(static_cast<double>(static_cast<s32>(gw(a, i))) * pow2(static_cast<s32>(inst.i8()) - 155), accurate));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CUFLT:
        for (u32 i = 0; i < 4; i++)
            sw(r, i, int_to_sp(static_cast<double>(gw(a, i)) * pow2(static_cast<s32>(inst.i8()) - 155), accurate));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CFLTS:
        for (u32 i = 0; i < 4; i++) sw(r, i, static_cast<u32>(cflts_one(gw(a, i), inst.i8())));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::CFLTU:
        for (u32 i = 0; i < 4; i++) sw(r, i, cfltu_one(gw(a, i), inst.i8()));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FSCRRD: ctx.gpr[inst.rt()] = ctx.fpscr; break;
    case Op::FSCRWR: ps_spu_fscrwr(&ctx, &a); break;

    // ---- double-precision floating point (fp.h: round to nearest in an FPCR window) ----
    case Op::DFA:
        for (u32 i = 0; i < 2; i++) sd(r, i, ps_spu_dp_add(gd(a, i), gd(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::DFS:
        for (u32 i = 0; i < 2; i++) sd(r, i, ps_spu_dp_sub(gd(a, i), gd(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::DFM:
        for (u32 i = 0; i < 2; i++) sd(r, i, ps_spu_dp_mul(gd(a, i), gd(b, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::DFMA:
        for (u32 i = 0; i < 2; i++) sd(r, i, ps_spu_dp_fma(gd(a, i), gd(b, i), gd(t, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::DFMS:
        for (u32 i = 0; i < 2; i++) sd(r, i, ps_spu_dp_fma(gd(a, i), gd(b, i), -gd(t, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::DFNMS:
        for (u32 i = 0; i < 2; i++) sd(r, i, ps_spu_dp_fma(-gd(a, i), gd(b, i), gd(t, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::DFNMA:
        for (u32 i = 0; i < 2; i++) sd(r, i, -ps_spu_dp_fma(gd(a, i), gd(b, i), gd(t, i)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FESD:
        // DP element i <- SP element 2i (exact; denormal SP inputs are zero, NaN gives the default NaN)
        for (u32 i = 0; i < 2; i++) sd(r, i, static_cast<double>(gf(a, i * 2)));
        ctx.gpr[inst.rt()] = r;
        break;
    case Op::FRDS:
        // SP element 2i <- DP element i rounded to nearest; odd elements zero
        for (u32 i = 0; i < 2; i++) {
            sf(r, i * 2, ps_spu_dp_to_sp(gd(a, i)));
            sw(r, i * 2 + 1, 0);
        }
        ctx.gpr[inst.rt()] = r;
        break;

    case Op::UNKNOWN:
    case Op::DFCGT:
    case Op::DFCMGT:
    case Op::DFTSV:
    case Op::DFCEQ:
    case Op::DFCMEQ:
    case Op::COUNT: return ps_spu_illegal(&ctx, pc, inst.raw) == 0;
    }

    ctx.pc = next;
    return true;
}

bool step(SPUContext& ctx)
{
    const u32 pc = ctx.pc & PS_SPU_PC_MASK;
    ctx.pc = pc;
    return execute(ctx, decode(ps_ld32(ctx.ls + pc)));
}

RunResult run(SPUContext& ctx, const RunOptions& options)
{
    RunResult result;
    ctx.stop_reason = PS_SPU_STOP_NONE;
    for (;;) {
        if (options.max_steps && result.steps >= options.max_steps) {
            ctx.stop_reason = PS_SPU_STOP_LIMIT;
            break;
        }
        const u32 pc = ctx.pc & PS_SPU_PC_MASK;
        ctx.pc = pc;
        const Inst inst = decode(ps_ld32(ctx.ls + pc));
        result.steps++;
        if (!execute(ctx, inst)) break;
        if (ctx.pc != pc + 4) {
            // Taken branch (or an interrupt): the run loop may resume lifted code here, or must stop.
            if (__atomic_load_n(&ctx.attention, __ATOMIC_RELAXED)) break;
            if (options.stop_at_lifted && (ps_spu_lookup(&ctx, ctx.pc) || detail::resolve_program(&ctx, ctx.pc))) {
                result.reached_lifted = true;
                break;
            }
        }
    }
    return result;
}

std::vector<Op> supported_ops()
{
    std::vector<Op> ops;
    for (const OpInfo& info : all_ops())
        if (!(info.flags & OPF_NOSUPPORT)) ops.push_back(info.op);
    return ops;
}

} // namespace pspocket::spu
