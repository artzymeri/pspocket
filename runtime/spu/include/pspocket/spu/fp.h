/*
 * SPU floating-point model, shared by the interpreter (C++) and lifted code (C11).
 *
 * Host environment ("SPU FP environment"): every thread that executes SPU code runs with FPCR.FZ = 1 (denormal
 * inputs and outputs flush to zero), FPCR.RMode = round toward zero and FPCR.DN = 1 (NaN results are the default
 * NaN, independent of operand order). ps_spu_run and the SPU thread runtime enter it; tests that call the
 * interpreter or lifted functions directly enter it with ps_spu_fpenv_enter. The interpreter and lifted code
 * are compiled with FENV_ACCESS ON, so the compiler never folds an inexact operation with the host's default
 * rounding.
 *
 * Single precision (fa fs fm fma fnms fms csflt cuflt; ctx->xfloat selects the model):
 *
 *   APPROXIMATE (default, as RPCS3's default "approximate xfloat", rpcs3/Emu/Cell/SPULLVMRecompiler.cpp without its
 *   pattern rewrites): host IEEE single precision in the SPU environment, so denormals are zero and rounding is
 *   toward zero, and overflow gives +-FLT_MAX. Exponent-255 inputs are IEEE Inf/NaN (extended range is not
 *   modelled). Like RPCS3: fm returns +0 when an operand is zero or denormal; fma returns the addend unchanged when
 *   a or b is zero or denormal; fs, fnms and fms clamp exponent-255 operands (fs: b only) to +-FLT_MAX.
 *
 *   ACCURATE (extended range, as RPCS3's "accurate xfloat"): operands are SPU single-precision numbers (exponent 255
 *   is a normal exponent, denormals are zero), the operation is done in double precision rounding toward zero
 *   (exact for a*b, and toward-zero double rounding is innocuous), the result is truncated to 24 bits, magnitudes
 *   above 0x7FFFFFFF saturate to it and results below 2^-126 become +0.
 *
 *   Both models: fcgt fcmgt fceq fcmeq compare as SPU numbers (denormals equal zero, exponent 255 is largest);
 *   cflts/cfltu scale exactly in double and saturate (exponent-255 inputs saturate by sign); csflt/cuflt scale
 *   exactly and round toward zero. frest/frsqest/fi are RPCS3's table and fixed-point model (spu_frest_*_lut,
 *   "spu_fi" in SPULLVMRecompiler.cpp), bit-exact.
 *
 * Double precision (dfa dfs dfm dfma dfms dfnms dfnma frds): IEEE binary64 rounded to nearest-even (the FPSCR
 * rounding-mode bits are stored but not honoured), fused multiply-add, denormal inputs and outputs flushed to zero
 * and NaN results are the default NaN (FPCR FZ and DN stay set). Because FPCR.RMode applies to both widths, each
 * double-precision operation runs in one inline-assembly block that switches FPCR to round-to-nearest and back, so
 * the compiler cannot move other floating-point work into the window. fesd is exact (no rounding).
 *
 * FPSCR: fscrwr stores the architected bits, fscrrd reads them back; arithmetic does not set exception flags.
 */
#ifndef PSPOCKET_SPU_FP_H
#define PSPOCKET_SPU_FP_H

#include <stdint.h>
#include <string.h>

#include "pspocket/spu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- host floating-point environment ---- */

#if defined(__aarch64__)
#define PS_SPU_FPCR_DN (1ull << 25)
#define PS_SPU_FPCR_FZ (1ull << 24)
#define PS_SPU_FPCR_RMODE (3ull << 22)
#define PS_SPU_FPCR_RZ (3ull << 22)

static inline uint64_t ps_spu_fpcr_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}
static inline void ps_spu_fpcr_write(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v) : "memory"); }
static inline uint64_t ps_spu_fpcr_env(uint64_t current)
{
    return (current & ~PS_SPU_FPCR_RMODE) | PS_SPU_FPCR_RZ | PS_SPU_FPCR_FZ | PS_SPU_FPCR_DN;
}
#endif

/* Enters the SPU FP environment and returns a token for ps_spu_fpenv_leave (0 when nothing changed). */
uint64_t ps_spu_fpenv_enter(void);
void ps_spu_fpenv_leave(uint64_t token);
/* True when the calling thread is in the SPU FP environment. */
int ps_spu_fpenv_active(void);

/* Process default for ctx->xfloat of new contexts (PS_SPU_XFLOAT_APPROXIMATE unless changed). */
void ps_spu_set_default_xfloat(uint8_t mode);
uint8_t ps_spu_default_xfloat(void);

/* ---- bit casts ---- */

static inline float ps_spu_f32(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline uint32_t ps_spu_u32f(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline double ps_spu_f64(uint64_t u)
{
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static inline uint64_t ps_spu_u64f(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

/* ---- tables: RPCS3 Emu/Cell/SPUThread.cpp (spu_frest_fraction_lut and friends) ---- */
extern const uint32_t ps_spu_frest_fraction_lut[32];
extern const uint32_t ps_spu_frest_exponent_lut[256];
extern const uint32_t ps_spu_frsqest_fraction_lut[64];
extern const uint32_t ps_spu_frsqest_exponent_lut[256];

/* ---- extended-range single precision (ACCURATE model), RPCS3 SPULLVMRecompiler.cpp xfloat_to_double,
 * xfloat_in_double and double_to_xfloat ---- */

/* Exact value of an SPU single-precision number: exponent 0 is a signed zero, exponent 255 is a normal exponent. */
static inline double ps_spu_xf_to_f64(uint32_t x)
{
    const uint64_t sign = (uint64_t)(x & 0x80000000u) << 32;
    const uint64_t a = x & 0x7FFFFFFFu;
    if (a <= 0x7FFFFFu) return ps_spu_f64(sign);
    return ps_spu_f64(sign | ((a + 0x1C0000000ull) << 29));
}

/* Double (finite) to SPU single precision: truncate to 24 bits, saturate above 0x7FFFFFFF, below 2^-126 -> +0. */
static inline uint32_t ps_spu_f64_to_xf(double d)
{
    const uint64_t bits = ps_spu_u64f(d);
    uint64_t a = bits & 0x7FFFFFFFE0000000ull;
    if (a < 0x3810000000000000ull) return 0;
    if (a > 0x47FFFFFFE0000000ull) a = 0x47FFFFFFE0000000ull;
    const uint32_t sign = (uint32_t)(bits >> 32) & 0x80000000u;
    return sign | (((uint32_t)(a >> 29) ^ 0x40000000u) & 0x7FFFFFFFu);
}

/* ---- double precision: one FPCR round-to-nearest window per operation ---- */

#if defined(__aarch64__)
/* FPCR for double-precision work: the caller's value with RMode = nearest, FZ and DN set. */
#define PS_SPU_DP_WINDOW(insn, ...)                                                                              \
    __asm__ volatile("mrs %[sv], fpcr\n\t"                                                                       \
                     "bic %[nw], %[sv], #0xc00000\n\t"                                                           \
                     "orr %[nw], %[nw], #0x3000000\n\t"                                                          \
                     "msr fpcr, %[nw]\n\t" insn "\n\t"                                                           \
                     "msr fpcr, %[sv]" __VA_ARGS__)

static inline double ps_spu_dp_add(double a, double b)
{
    double r;
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fadd %d[r], %d[a], %d[b]", : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(a), [b] "w"(b));
    return r;
}
static inline double ps_spu_dp_sub(double a, double b)
{
    double r;
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fsub %d[r], %d[a], %d[b]", : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(a), [b] "w"(b));
    return r;
}
static inline double ps_spu_dp_mul(double a, double b)
{
    double r;
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fmul %d[r], %d[a], %d[b]", : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(a), [b] "w"(b));
    return r;
}
/* a * b + c, one rounding */
static inline double ps_spu_dp_fma(double a, double b, double c)
{
    double r;
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fmadd %d[r], %d[a], %d[b], %d[c]",
        : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(a), [b] "w"(b), [c] "w"(c));
    return r;
}
/* double -> single, rounded to nearest */
static inline float ps_spu_dp_to_sp(double a)
{
    float r;
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fcvt %s[r], %d[a]", : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(a));
    return r;
}
#else
/* Other hosts (toolchain builds): best effort through <fenv.h>; the compiler must honour FENV_ACCESS. */
double ps_spu_dp_add(double a, double b);
double ps_spu_dp_sub(double a, double b);
double ps_spu_dp_mul(double a, double b);
double ps_spu_dp_fma(double a, double b, double c);
float ps_spu_dp_to_sp(double a);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_SPU_FP_H */
