/*
 * PPU floating-point helpers shared by the interpreter (C++) and lifted code (C11). Pure functions, no state.
 *
 * FPSCR model (minimal but consistent; the interpreter and lifted code implement exactly this):
 *
 * - ctx->fpscr holds the architected 32-bit FPSCR (IBM bit n = mask 1u << (31 - n)).
 * - mtfsf, mtfsfi, mtfsb0, mtfsb1 and mcrfs write it with the architected rules: FEX and VX (bits 1-2) are summary
 *   bits recomputed after every write and cannot be set or cleared directly; mtfsb1 of an exception bit that was 0
 *   also sets FX; mcrfs clears the exception bits it copied. Reserved bit 20 always reads 0.
 * - fcmpu/fcmpo write FPCC (FL FG FE FU, bits 16-19) and the target CR field. They do not set VXSNAN/VXVC.
 * - Arithmetic does NOT update FPRF, FR, FI or any exception bit (as RPCS3 with its default settings), and no
 *   floating-point exception is ever raised. VE/OE/UE/ZE/XE and NI are stored but have no effect.
 * - Rounding: arithmetic always rounds to nearest-even (the host default; RPCS3 does the same). FPSCR[RN] is
 *   stored, read back by mffs, and honoured by fctiw/fctid (the conversions that round "per FPSCR[RN]").
 *   fctiwz/fctidz truncate.
 * - Record forms (Rc=1) of FP instructions set CR1 = FPSCR[FX FEX VX OX] after the instruction, as the
 *   architecture defines. (RPCS3 instead writes the result's sign class into CR1; see docs/decisions.md.)
 * - mffs returns FPSCR in the low word with a zero high word (as RPCS3).
 *
 * Single precision: frsp and the single-precision arithmetic forms compute in double and round to float
 * ((double)(float)x), and fmadds & co. use fma() then round, exactly as RPCS3 (Emu/Cell/PPUInterpreter.cpp).
 */
#ifndef PSPOCKET_PPU_FPU_H
#define PSPOCKET_PPU_FPU_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS_FPSCR_FX 0x80000000u     /* 0  exception summary */
#define PS_FPSCR_FEX 0x40000000u    /* 1  enabled exception summary (computed) */
#define PS_FPSCR_VX 0x20000000u     /* 2  invalid operation summary (computed) */
#define PS_FPSCR_OX 0x10000000u     /* 3 */
#define PS_FPSCR_UX 0x08000000u     /* 4 */
#define PS_FPSCR_ZX 0x04000000u     /* 5 */
#define PS_FPSCR_XX 0x02000000u     /* 6 */
#define PS_FPSCR_VXSNAN 0x01000000u /* 7 */
#define PS_FPSCR_VXISI 0x00800000u  /* 8 */
#define PS_FPSCR_VXIDI 0x00400000u  /* 9 */
#define PS_FPSCR_VXZDZ 0x00200000u  /* 10 */
#define PS_FPSCR_VXIMZ 0x00100000u  /* 11 */
#define PS_FPSCR_VXVC 0x00080000u   /* 12 */
#define PS_FPSCR_FR 0x00040000u     /* 13 */
#define PS_FPSCR_FI 0x00020000u     /* 14 */
#define PS_FPSCR_C 0x00010000u      /* 15 */
#define PS_FPSCR_FL 0x00008000u     /* 16 */
#define PS_FPSCR_FG 0x00004000u     /* 17 */
#define PS_FPSCR_FE 0x00002000u     /* 18 */
#define PS_FPSCR_FU 0x00001000u     /* 19 */
#define PS_FPSCR_FPCC 0x0000F000u
#define PS_FPSCR_RESERVED 0x00000800u /* 20 */
#define PS_FPSCR_VXSOFT 0x00000400u /* 21 */
#define PS_FPSCR_VXSQRT 0x00000200u /* 22 */
#define PS_FPSCR_VXCVI 0x00000100u  /* 23 */
#define PS_FPSCR_VE 0x00000080u     /* 24 */
#define PS_FPSCR_OE 0x00000040u     /* 25 */
#define PS_FPSCR_UE 0x00000020u     /* 26 */
#define PS_FPSCR_ZE 0x00000010u     /* 27 */
#define PS_FPSCR_XE 0x00000008u     /* 28 */
#define PS_FPSCR_NI 0x00000004u     /* 29 */
#define PS_FPSCR_RN 0x00000003u     /* 30-31 */

#define PS_FPSCR_VX_ALL                                                                                          \
    (PS_FPSCR_VXSNAN | PS_FPSCR_VXISI | PS_FPSCR_VXIDI | PS_FPSCR_VXZDZ | PS_FPSCR_VXIMZ | PS_FPSCR_VXVC |         \
        PS_FPSCR_VXSOFT | PS_FPSCR_VXSQRT | PS_FPSCR_VXCVI)
/* Sticky exception bits (a 0 -> 1 transition sets FX) */
#define PS_FPSCR_EXCEPTIONS (PS_FPSCR_OX | PS_FPSCR_UX | PS_FPSCR_ZX | PS_FPSCR_XX | PS_FPSCR_VX_ALL)

enum { PS_FPSCR_RN_NEAR = 0, PS_FPSCR_RN_ZERO = 1, PS_FPSCR_RN_PINF = 2, PS_FPSCR_RN_MINF = 3 };

static inline uint64_t ps_f64_bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static inline double ps_f64_from_bits(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

static inline double ps_ppu_round_single(double x) { return (double)(float)x; }

/* ---- FPSCR writes ---- */

/* Recomputes the summary bits FEX and VX and clears the reserved bit. */
static inline uint32_t ps_fpscr_summarize(uint32_t v)
{
    v &= ~(PS_FPSCR_FEX | PS_FPSCR_VX | PS_FPSCR_RESERVED);
    if (v & PS_FPSCR_VX_ALL) v |= PS_FPSCR_VX;
    const uint32_t enabled = ((v & PS_FPSCR_VX) && (v & PS_FPSCR_VE)) || ((v & PS_FPSCR_OX) && (v & PS_FPSCR_OE)) ||
        ((v & PS_FPSCR_UX) && (v & PS_FPSCR_UE)) || ((v & PS_FPSCR_ZX) && (v & PS_FPSCR_ZE)) ||
        ((v & PS_FPSCR_XX) && (v & PS_FPSCR_XE));
    if (enabled) v |= PS_FPSCR_FEX;
    return v;
}

/* Mask of the FPSCR nibbles selected by an 8-bit field mask (bit 0x80 = field 0). */
static inline uint32_t ps_fpscr_field_mask(uint32_t flm)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++)
        if (flm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
    return mask;
}

/* mtfsf FLM, frB: fields selected by FLM take the low word of frB. */
static inline uint32_t ps_fpscr_mtfsf(uint32_t fpscr, uint32_t flm, uint64_t frb_bits)
{
    const uint32_t mask = ps_fpscr_field_mask(flm);
    return ps_fpscr_summarize((fpscr & ~mask) | ((uint32_t)frb_bits & mask));
}

/* mtfsfi BF, U */
static inline uint32_t ps_fpscr_mtfsfi(uint32_t fpscr, uint32_t bf, uint32_t imm)
{
    const uint32_t shift = 28 - 4 * (bf & 7);
    return ps_fpscr_summarize((fpscr & ~(0xFu << shift)) | ((imm & 0xFu) << shift));
}

/* mtfsb0 BT */
static inline uint32_t ps_fpscr_mtfsb0(uint32_t fpscr, uint32_t bt)
{
    const uint32_t bit = 0x80000000u >> (bt & 31);
    if (bit & (PS_FPSCR_FEX | PS_FPSCR_VX)) return fpscr; /* summary bits cannot be cleared explicitly */
    return ps_fpscr_summarize(fpscr & ~bit);
}

/* mtfsb1 BT */
static inline uint32_t ps_fpscr_mtfsb1(uint32_t fpscr, uint32_t bt)
{
    const uint32_t bit = 0x80000000u >> (bt & 31);
    if (bit & (PS_FPSCR_FEX | PS_FPSCR_VX)) return fpscr; /* summary bits cannot be set explicitly */
    uint32_t v = fpscr | bit;
    if ((bit & PS_FPSCR_EXCEPTIONS) && !(fpscr & bit)) v |= PS_FPSCR_FX;
    return ps_fpscr_summarize(v);
}

/* 4-bit FPSCR field BF (field 0 = bits 0-3) */
static inline uint32_t ps_fpscr_field(uint32_t fpscr, uint32_t bf) { return (fpscr >> (28 - 4 * (bf & 7))) & 0xFu; }

/* mcrfs BF, BFA: after copying field BFA to CR, its exception bits (and FX) are cleared. */
static inline uint32_t ps_fpscr_mcrfs_clear(uint32_t fpscr, uint32_t bfa)
{
    const uint32_t field = 0xF0000000u >> (4 * (bfa & 7));
    return ps_fpscr_summarize(fpscr & ~(field & (PS_FPSCR_FX | PS_FPSCR_EXCEPTIONS)));
}

/* fcmpu / fcmpo: FPCC from the comparison of a and b. Returns the 4-bit field value FL FG FE FU (FL = 8). */
static inline uint32_t ps_fp_compare_bits(double a, double b)
{
    if (a < b) return 8;
    if (a > b) return 4;
    if (a == b) return 2;
    return 1; /* unordered */
}

static inline uint32_t ps_fpscr_set_fpcc(uint32_t fpscr, uint32_t fpcc4)
{
    return (fpscr & ~PS_FPSCR_FPCC) | ((fpcc4 & 0xFu) << 12);
}

/* ---- estimates (bit-exact with RPCS3's tables, rpcs3/Emu/Cell/Common.h) ---- */

/* fres: 1/x estimate, rounded to single. */
static inline double ps_ppu_fres(double a)
{
    static const uint32_t mantissas[128] = {
        0x007f0000, 0x007d0800, 0x007b1800, 0x00793000, 0x00775000, 0x00757000, 0x0073a000, 0x0071e000,
        0x00700000, 0x006e4000, 0x006ca000, 0x006ae000, 0x00694000, 0x00678000, 0x00660000, 0x00646000,
        0x0062c000, 0x00614000, 0x005fc000, 0x005e4000, 0x005cc000, 0x005b4000, 0x0059c000, 0x00584000,
        0x00570000, 0x00558000, 0x00540000, 0x0052c000, 0x00518000, 0x00500000, 0x004ec000, 0x004d8000,
        0x004c0000, 0x004b0000, 0x00498000, 0x00488000, 0x00474000, 0x00460000, 0x0044c000, 0x00438000,
        0x00428000, 0x00418000, 0x00400000, 0x003f0000, 0x003e0000, 0x003d0000, 0x003bc000, 0x003ac000,
        0x00398000, 0x00388000, 0x00378000, 0x00368000, 0x00358000, 0x00348000, 0x00338000, 0x00328000,
        0x00318000, 0x00308000, 0x002f8000, 0x002ec000, 0x002e0000, 0x002d0000, 0x002c0000, 0x002b0000,
        0x002a0000, 0x00298000, 0x00288000, 0x00278000, 0x0026c000, 0x00260000, 0x00250000, 0x00244000,
        0x00238000, 0x00228000, 0x00220000, 0x00210000, 0x00200000, 0x001f8000, 0x001e8000, 0x001e0000,
        0x001d0000, 0x001c8000, 0x001b8000, 0x001b0000, 0x001a0000, 0x00198000, 0x00190000, 0x00180000,
        0x00178000, 0x00168000, 0x00160000, 0x00158000, 0x00148000, 0x00140000, 0x00138000, 0x00128000,
        0x00120000, 0x00118000, 0x00108000, 0x00100000, 0x000f8000, 0x000f0000, 0x000e0000, 0x000d8000,
        0x000d0000, 0x000c8000, 0x000b8000, 0x000b0000, 0x000a8000, 0x000a0000, 0x00098000, 0x00090000,
        0x00080000, 0x00078000, 0x00070000, 0x00068000, 0x00060000, 0x00058000, 0x00050000, 0x00048000,
        0x00040000, 0x00038000, 0x00030000, 0x00028000, 0x00020000, 0x00018000, 0x00010000, 0x00000000,
    };
    const uint64_t b = ps_f64_bits(a);
    const uint64_t e = (b >> 52) & 0x7FF;
    const uint64_t i = (b >> 45) & 0x7F;
    const uint64_t r = e >= (0x3FF + 0x80) ? 0 : ((0x7FF - 2 - e) << 52) | ((uint64_t)mantissas[i] << (32 - 3));
    const uint64_t bits = a == a ? (b & 0x8000000000000000ull) | r : (0x0008000000000000ull | b);
    return ps_ppu_round_single(ps_f64_from_bits(bits));
}

/* frsqrte: 1/sqrt(x) estimate (double, not rounded), computed like RPCS3's ppu_frsqrte_lut entry for b >> 49. */
static inline double ps_ppu_frsqrte(double x)
{
    static const uint32_t mantissas[16] = {
        0x000f1000u, 0x000d8000u, 0x000c0000u, 0x000a8000u, 0x00098000u, 0x00088000u, 0x00080000u, 0x00070000u,
        0x00060000u, 0x0004c000u, 0x0003c000u, 0x00030000u, 0x00020000u, 0x00018000u, 0x00010000u, 0x00008000u,
    };
    const uint64_t i = ps_f64_bits(x) >> 49; /* sign, exponent, top 3 mantissa bits */
    const uint64_t sign = i >> 14;
    const uint64_t expv = (i >> 3) & 0x7FF;
    uint32_t r;
    if (expv == 0) {
        r = 0x7FF00000u | (uint32_t)(sign << 31); /* +-inf on zero/denormal (as RPCS3) */
    } else if (expv == 0x7FF) {
        r = i == (0x7FFu << 3) ? 0 : 0x7FF80000u; /* +inf -> 0, NaN / -inf -> QNaN */
    } else if (sign) {
        r = 0x7FF80000u; /* negative -> QNaN */
    } else {
        const uint32_t exp = 0x3FE00000u - (uint32_t)(((expv + 0x1C01) >> 1) << 20);
        r = mantissas[8 ^ (i & 0xF)] | exp;
    }
    return ps_f64_from_bits((uint64_t)r << 32);
}

/* ---- conversions ---- */

static inline double ps_ppu_round_mode(double x, uint32_t rn)
{
    switch (rn & 3) {
    case PS_FPSCR_RN_ZERO: return __builtin_trunc(x);
    case PS_FPSCR_RN_PINF: return __builtin_ceil(x);
    case PS_FPSCR_RN_MINF: return __builtin_floor(x);
    default: return __builtin_roundeven(x);
    }
}

/*
 * fctiw / fctiwz: round (per rn), saturate to int32, NaN -> 0x80000000. The FPR holds the result sign-extended to
 * 64 bits (RPCS3 PPUInterpreter.cpp FCTIW; the architecture leaves the high word undefined).
 */
static inline double ps_ppu_fctiw(double b, uint32_t rn)
{
    int64_t r;
    if (b != b) {
        r = INT32_MIN;
    } else {
        const double t = ps_ppu_round_mode(b, rn);
        if (t >= 2147483648.0)
            r = INT32_MAX;
        else if (t < -2147483648.0)
            r = INT32_MIN;
        else
            r = (int32_t)t;
    }
    return ps_f64_from_bits((uint64_t)r);
}

/* fctid / fctidz: round (per rn), saturate to int64, NaN -> 0x8000000000000000. */
static inline double ps_ppu_fctid(double b, uint32_t rn)
{
    int64_t r;
    if (b != b) {
        r = INT64_MIN;
    } else {
        const double t = ps_ppu_round_mode(b, rn);
        if (t >= 9223372036854775808.0)
            r = INT64_MAX;
        else if (t < -9223372036854775808.0)
            r = INT64_MIN;
        else
            r = (int64_t)t;
    }
    return ps_f64_from_bits((uint64_t)r);
}

/* fcfid: the FPR's bits as a signed 64-bit integer, converted to double (round to nearest). */
static inline double ps_ppu_fcfid(double b) { return (double)(int64_t)ps_f64_bits(b); }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PPU_FPU_H */
