#pragma once

// Single precision as the CELP libraries' SPU programs compute it (D-932), in pspocket's model of the SPU (pspocket/spu/
// fp.h, the one Sony's programs run on in the comparisons): every result the exact one truncated toward zero, denormal
// operands and results zero, a product with a zero operand +0, a multiply-add whose product has a zero operand its
// addend. The codec's arithmetic runs inside an spu::Scope (the host in the SPU's floating-point environment: rounding
// toward zero, flush to zero) in translation units that include this header first: FENV_ACCESS ON keeps the compiler
// from folding or reordering an inexact operation, FP_CONTRACT OFF from fusing a multiply and an add the SPU program
// does in two steps. Each helper is one SPU instruction (fa, fs, fm, fma, fnms, fms, frest, fi, cflts, csflt); the
// double precision ones (dfa, dfm, dfma, ...) round to nearest, as the SPU's double precision does: they run inside a
// spu::Nearest scope.

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

#include <bit>
#include <cfenv>
#include <cmath>

#include "pspocket/core/types.hpp"
#include "pspocket/spu/fp.h"

namespace pspocket::hle::celp::spu {

// The host in the SPU's floating-point environment for the scope's life.
struct Scope {
    u64 token;
    Scope() : token(ps_spu_fpenv_enter()) {}
    ~Scope() { ps_spu_fpenv_leave(token); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// Double precision rounds to nearest (inside a Scope): the SPU's dfa, dfm, dfma and frds.
struct Nearest {
    int saved;
    Nearest() : saved(std::fegetround()) { std::fesetround(FE_TONEAREST); }
    ~Nearest() { std::fesetround(saved); }
    Nearest(const Nearest&) = delete;
    Nearest& operator=(const Nearest&) = delete;
};

inline u32 bits(float x) { return std::bit_cast<u32>(x); }
inline float from_bits(u32 u) { return std::bit_cast<float>(u); }

// Exponent 0: zero or denormal (the SPU's zero).
inline bool zero_class(float x) { return (bits(x) & 0x7F800000u) == 0; }
// Exponent 255 (an SPU number past FLT_MAX) held to +-FLT_MAX, where the model's operations clamp it.
inline float clamp_smax(float x)
{
    const u32 u = bits(x);
    return (u & 0x7F800000u) == 0x7F800000u ? from_bits((u & 0x80000000u) | 0x7F7FFFFFu) : x;
}

inline float fa(float a, float b) { return a + b; }
inline float fs(float a, float b) { return a - clamp_smax(b); }
inline float fm(float a, float b) { return zero_class(a) || zero_class(b) ? 0.0f : a * b; }
// a * b + c, one rounding
inline float fma(float a, float b, float c) { return zero_class(a) || zero_class(b) ? c : std::fmaf(a, b, c); }
// c - a * b
inline float fnms(float a, float b, float c) { return std::fmaf(-clamp_smax(a), clamp_smax(b), c); }
// a * b - c
inline float fms(float a, float b, float c) { return std::fmaf(clamp_smax(a), clamp_smax(b), -c); }

// a > b as the SPU compares (fcgt): denormals equal zero, sign-magnitude order.
inline bool fcgt(float a, float b)
{
    const u32 x = bits(a), y = bits(b);
    if (zero_class(a) && zero_class(b)) return false;
    const s32 xi = static_cast<s32>(x), yi = static_cast<s32>(y);
    if (xi < 0 && yi < 0) return xi < yi;
    return xi > yi;
}
// |a| > |b| (fcmgt)
inline bool fcmgt(float a, float b)
{
    if (zero_class(a) && zero_class(b)) return false;
    return (bits(a) & 0x7FFFFFFFu) > (bits(b) & 0x7FFFFFFFu);
}

// cflts x, 173: toward zero, saturating.
inline s32 cflts(float x)
{
    if (zero_class(x)) return 0;
    const double d = static_cast<double>(x);
    if (d >= 2147483648.0) return 0x7FFFFFFF;
    if (d < -2147483648.0) return static_cast<s32>(0x80000000u);
    return static_cast<s32>(d);
}
// csflt i, 155 (exact below 2^24, else toward zero)
inline float csflt(s32 i) { return static_cast<float>(i); }
// cuflt i, 155
inline float cuflt(u32 i) { return static_cast<float>(i); }

// frest and fi: the SPU's reciprocal estimate and its interpolation (RPCS3's table and fixed-point model,
// rpcs3/Emu/Cell/SPUThread.cpp and SPULLVMRecompiler.cpp "spu_fi", as pspocket/spu/fp.h has them).
inline float frest(float x)
{
    const u32 u = bits(x);
    return from_bits(ps_spu_frest_fraction_lut[(u >> 18) & 0x1F] | ps_spu_frest_exponent_lut[(u >> 23) & 0xFF] |
        (u & 0x80000000u));
}
inline float frsqest(float x)
{
    const u32 u = bits(x);
    return from_bits(ps_spu_frsqest_fraction_lut[(u >> 18) & 0x3F] | ps_spu_frsqest_exponent_lut[(u >> 23) & 0xFF]);
}
inline float fi(float a, float b)
{
    const u32 ua = bits(a), ub = bits(b);
    const u32 base = (ub & 0x007FFC00u) << 9, ymul = (ub & 0x3FFu) * (ua & 0x7FFFFu);
    const bool adjust = ymul > base;
    const u32 bnew = (base - ymul) >> (adjust ? 8 : 9);
    u32 expo = (ub & 0xFF800000u) - (adjust ? 0x00800000u : 0u);
    if ((expo & 0x7F800000u) == 0x7F800000u) expo = (expo & 0x80000000u) | 0x7F7FFFFFu;
    return from_bits(expo | (bnew & 0x007FFFFFu));
}

// Double precision (inside a Nearest scope): IEEE binary64, fused multiply-add.
inline double dfma(double a, double b, double c) { return std::fma(a, b, c); }
// frds: to single precision, rounded to nearest (inside a Nearest scope)
inline float frds(double a) { return static_cast<float>(a); }

} // namespace pspocket::hle::celp::spu
