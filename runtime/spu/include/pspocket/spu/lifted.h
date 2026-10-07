/*
 * The one header lifted (generated) SPU code includes, besides its own generated header.
 *
 * Generated functions have the signature  void spu_<hash8>_<entry>(SPUContext* ctx, uint8_t* ls)  (runtime.h).
 * They keep the guest registers they use in locals of type ps_spu_vec (loaded at entry, spilled around calls into
 * other lifted functions and the runtime, and at exits), access the local store through ls, and implement every
 * instruction with the helpers below.
 *
 * These helpers are vectorised (clang vector extensions, NEON where it pays) and written independently of the
 * scalar interpreter (src/interpreter.cpp): the differential fuzz test compares the two implementations. Register
 * values use the D-004 lane order: host lane N-1-i holds guest element i, so the preferred word is lane 3 and the
 * register is the guest's 128-bit value as a little-endian integer.
 *
 * Floating point follows pspocket/spu/fp.h. This header turns on FENV_ACCESS for the translation unit, so the
 * compiler never folds an inexact operation assuming the default rounding (lifted code runs with FPCR in round
 * toward zero), and FP_CONTRACT OFF, so no a*b+c is fused unless the instruction says so.
 */
#ifndef PSPOCKET_SPU_LIFTED_H
#define PSPOCKET_SPU_LIFTED_H

#include <stdint.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "pspocket/core/be.h"
#include "pspocket/spu/context.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

#ifdef __cplusplus
extern "C" {
#endif

#define PSL static inline __attribute__((always_inline))

/* ---- vector types (all 16 bytes; casts between them are free bit casts) ---- */
typedef uint32_t ps_spu_vec __attribute__((vector_size(16), aligned(16)));
typedef uint8_t psv_u8 __attribute__((vector_size(16)));
typedef int8_t psv_s8 __attribute__((vector_size(16)));
typedef uint16_t psv_u16 __attribute__((vector_size(16)));
typedef int16_t psv_s16 __attribute__((vector_size(16)));
typedef int32_t psv_s32 __attribute__((vector_size(16)));
typedef uint64_t psv_u64 __attribute__((vector_size(16)));
typedef int64_t psv_s64 __attribute__((vector_size(16)));
typedef float psv_f32 __attribute__((vector_size(16)));
typedef double psv_f64 __attribute__((vector_size(16)));
__extension__ typedef unsigned __int128 psl_u128;

/* ---- register file, local store, constants ---- */

PSL ps_spu_vec psl_ld(const SPUContext* ctx, unsigned r)
{
    ps_spu_vec v;
    memcpy(&v, &ctx->gpr[r], 16);
    return v;
}
PSL void psl_st(SPUContext* ctx, unsigned r, ps_spu_vec v) { memcpy(&ctx->gpr[r], &v, 16); }

PSL ps_spu_vec psl_rev(ps_spu_vec v)
{
    const psv_u8 b = (psv_u8)v;
    return (ps_spu_vec)__builtin_shufflevector(b, b, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
}
PSL ps_spu_vec psl_lq(const uint8_t* ls, uint32_t addr)
{
    ps_spu_vec v;
    memcpy(&v, ls + (addr & PS_SPU_QW_MASK), 16);
    return psl_rev(v);
}
PSL void psl_stq(uint8_t* ls, uint32_t addr, ps_spu_vec v)
{
    v = psl_rev(v);
    memcpy(ls + (addr & PS_SPU_QW_MASK), &v, 16);
}

PSL ps_spu_vec psl_splat32(uint32_t x) { return (ps_spu_vec){x, x, x, x}; }
PSL ps_spu_vec psl_splat16(uint16_t x) { return (ps_spu_vec)(psv_u16){x, x, x, x, x, x, x, x}; }
PSL ps_spu_vec psl_splat8(uint8_t x) { return (ps_spu_vec)(psv_u8){x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x}; }
/* value in the preferred slot (guest word 0 = lane 3), other words zero */
PSL ps_spu_vec psl_pref(uint32_t x) { return (ps_spu_vec){0, 0, 0, x}; }
PSL uint32_t psl_u32p(ps_spu_vec v) { return v[3]; }
PSL uint16_t psl_u16p(ps_spu_vec v) { return ((psv_u16)v)[6]; }
PSL ps_spu_vec psl_const(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) { return (ps_spu_vec){w3, w2, w1, w0}; }

/* select: bits of b where m is set, else a (selb semantics: rt = (rc & rb) | (~rc & ra)) */
PSL ps_spu_vec psl_bsel(ps_spu_vec m, ps_spu_vec a, ps_spu_vec b) { return (m & b) | (~m & a); }

/* ---- integer arithmetic ---- */

PSL ps_spu_vec psl_a(ps_spu_vec x, ps_spu_vec y) { return x + y; }
PSL ps_spu_vec psl_ah(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_u16)x + (psv_u16)y); }
PSL ps_spu_vec psl_sf(ps_spu_vec x, ps_spu_vec y) { return y - x; }
PSL ps_spu_vec psl_sfh(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_u16)y - (psv_u16)x); }
PSL ps_spu_vec psl_addx(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return x + y + (t & 1); }
PSL ps_spu_vec psl_sfx(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return y - x - (~t & 1); }
PSL ps_spu_vec psl_cg(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((x + y) < x) & 1; }
PSL ps_spu_vec psl_cgx(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t)
{
    const ps_spu_vec s = x + y;
    const ps_spu_vec c1 = (ps_spu_vec)(s < x);
    const ps_spu_vec r = s + (t & 1);
    const ps_spu_vec c2 = (ps_spu_vec)(r < s);
    return (c1 | c2) & 1;
}
/* bg: 1 when y - x does not borrow */
PSL ps_spu_vec psl_bg(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)(y >= x) & 1; }
PSL ps_spu_vec psl_bgx(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t)
{
    const ps_spu_vec cin = (ps_spu_vec)((t & 1) != 0);
    return psl_bsel(cin, (ps_spu_vec)(y > x), (ps_spu_vec)(y >= x)) & 1;
}

/* multiplies: s = signed 16-bit halves, u = unsigned; lo = guest low (right) halfword of each word */
PSL psv_s32 psl_slo(ps_spu_vec x) { return (psv_s32)(x << 16) >> 16; }
PSL psv_s32 psl_shi(ps_spu_vec x) { return (psv_s32)x >> 16; }
PSL ps_spu_vec psl_mpy(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)(psl_slo(x) * psl_slo(y)); }
PSL ps_spu_vec psl_mpyu(ps_spu_vec x, ps_spu_vec y) { return (x & 0xFFFF) * (y & 0xFFFF); }
PSL ps_spu_vec psl_mpyh(ps_spu_vec x, ps_spu_vec y) { return ((x >> 16) * (y & 0xFFFF)) << 16; }
PSL ps_spu_vec psl_mpys(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psl_slo(x) * psl_slo(y)) >> 16); }
PSL ps_spu_vec psl_mpyhh(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)(psl_shi(x) * psl_shi(y)); }
PSL ps_spu_vec psl_mpyhha(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return t + (ps_spu_vec)(psl_shi(x) * psl_shi(y)); }
PSL ps_spu_vec psl_mpyhhu(ps_spu_vec x, ps_spu_vec y) { return (x >> 16) * (y >> 16); }
PSL ps_spu_vec psl_mpyhhau(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return t + (x >> 16) * (y >> 16); }
PSL ps_spu_vec psl_mpya(ps_spu_vec x, ps_spu_vec y, ps_spu_vec c) { return c + (ps_spu_vec)(psl_slo(x) * psl_slo(y)); }

/* ---- logical ---- */

PSL ps_spu_vec psl_and(ps_spu_vec x, ps_spu_vec y) { return x & y; }
PSL ps_spu_vec psl_andc(ps_spu_vec x, ps_spu_vec y) { return x & ~y; }
PSL ps_spu_vec psl_or(ps_spu_vec x, ps_spu_vec y) { return x | y; }
PSL ps_spu_vec psl_orc(ps_spu_vec x, ps_spu_vec y) { return x | ~y; }
PSL ps_spu_vec psl_xor(ps_spu_vec x, ps_spu_vec y) { return x ^ y; }
PSL ps_spu_vec psl_nand(ps_spu_vec x, ps_spu_vec y) { return ~(x & y); }
PSL ps_spu_vec psl_nor(ps_spu_vec x, ps_spu_vec y) { return ~(x | y); }
PSL ps_spu_vec psl_eqv(ps_spu_vec x, ps_spu_vec y) { return ~(x ^ y); }
PSL ps_spu_vec psl_orx(ps_spu_vec x) { return psl_pref(x[0] | x[1] | x[2] | x[3]); }
PSL ps_spu_vec psl_selb(ps_spu_vec a, ps_spu_vec b, ps_spu_vec c) { return psl_bsel(c, a, b); }

/* ---- compares ---- */

PSL ps_spu_vec psl_ceq(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)(x == y); }
PSL ps_spu_vec psl_ceqh(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_u16)x == (psv_u16)y); }
PSL ps_spu_vec psl_ceqb(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_u8)x == (psv_u8)y); }
PSL ps_spu_vec psl_cgt(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_s32)x > (psv_s32)y); }
PSL ps_spu_vec psl_cgth(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_s16)x > (psv_s16)y); }
PSL ps_spu_vec psl_cgtb(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_s8)x > (psv_s8)y); }
PSL ps_spu_vec psl_clgt(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)(x > y); }
PSL ps_spu_vec psl_clgth(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_u16)x > (psv_u16)y); }
PSL ps_spu_vec psl_clgtb(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_u8)x > (psv_u8)y); }

/* ---- bit counting, extension, byte arithmetic ---- */

PSL ps_spu_vec psl_clz(ps_spu_vec x)
{
#if defined(__aarch64__)
    return (ps_spu_vec)vclzq_u32((uint32x4_t)x);
#else
    ps_spu_vec r;
    for (int i = 0; i < 4; i++) r[i] = x[i] ? (uint32_t)__builtin_clz(x[i]) : 32u;
    return r;
#endif
}
PSL ps_spu_vec psl_cntb(ps_spu_vec x)
{
#if defined(__aarch64__)
    return (ps_spu_vec)vcntq_u8((uint8x16_t)x);
#else
    psv_u8 b = (psv_u8)x;
    b = b - ((b >> 1) & 0x55);
    b = (b & 0x33) + ((b >> 2) & 0x33);
    return (ps_spu_vec)((b + (b >> 4)) & 0x0F);
#endif
}
PSL ps_spu_vec psl_xsbh(ps_spu_vec x) { return (ps_spu_vec)(((psv_s16)((psv_u16)x << 8)) >> 8); }
PSL ps_spu_vec psl_xshw(ps_spu_vec x) { return (ps_spu_vec)((psv_s32)(x << 16) >> 16); }
PSL ps_spu_vec psl_xswd(ps_spu_vec x) { return (ps_spu_vec)(((psv_s64)((psv_u64)x << 32)) >> 32); }
/* (a + b + 1) >> 1 without overflow: (a | b) - ((a ^ b) >> 1) */
PSL ps_spu_vec psl_avgb(ps_spu_vec x, ps_spu_vec y)
{
    const psv_u8 a = (psv_u8)x, b = (psv_u8)y;
    return (ps_spu_vec)((a | b) - ((a ^ b) >> 1));
}
PSL ps_spu_vec psl_absdb(ps_spu_vec x, ps_spu_vec y)
{
    const psv_u8 a = (psv_u8)x, b = (psv_u8)y;
    const psv_u8 gt = (psv_u8)(a > b);
    return (ps_spu_vec)(((a - b) & gt) | ((b - a) & ~gt));
}
/* sumb: each word = (sum of y's 4 bytes) << 16 | (sum of x's 4 bytes) */
PSL ps_spu_vec psl_sum4(ps_spu_vec v)
{
    const psv_u16 h = (psv_u16)v;
    const ps_spu_vec pairs = (ps_spu_vec)((h & 0xFF) + (h >> 8));
    return (pairs & 0xFFFF) + (pairs >> 16);
}
PSL ps_spu_vec psl_sumb(ps_spu_vec x, ps_spu_vec y) { return (psl_sum4(y) << 16) | psl_sum4(x); }

/* ---- form select mask / gather bits (host element j <-> result bit j) ---- */

PSL ps_spu_vec psl_fsm(ps_spu_vec x)
{
    const ps_spu_vec bits = psl_splat32(x[3]) >> (ps_spu_vec){0, 1, 2, 3};
    return 0 - (bits & 1);
}
PSL ps_spu_vec psl_fsmh(ps_spu_vec x)
{
    const psv_u16 bits = (psv_u16)psl_splat16((uint16_t)x[3]) >> (psv_u16){0, 1, 2, 3, 4, 5, 6, 7};
    return (ps_spu_vec)(0 - (bits & 1));
}
PSL ps_spu_vec psl_fsmb(ps_spu_vec x)
{
    const psv_u16 v = (psv_u16)psl_splat16((uint16_t)x[3]);
    const psv_u16 lo = (v >> (psv_u16){0, 1, 2, 3, 4, 5, 6, 7}) & 1;
    const psv_u16 hi = (v >> (psv_u16){8, 9, 10, 11, 12, 13, 14, 15}) & 1;
    const psv_u8 b = __builtin_shufflevector((psv_u8)lo, (psv_u8)hi, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30);
    return (ps_spu_vec)(0 - b);
}
PSL ps_spu_vec psl_gb(ps_spu_vec x)
{
    const ps_spu_vec b = (x & 1) << (ps_spu_vec){0, 1, 2, 3};
    return psl_pref(b[0] | b[1] | b[2] | b[3]);
}
PSL ps_spu_vec psl_gbh(ps_spu_vec x)
{
    const psv_u16 b = ((psv_u16)x & 1) << (psv_u16){0, 1, 2, 3, 4, 5, 6, 7};
    return psl_pref((uint32_t)(b[0] | b[1] | b[2] | b[3] | b[4] | b[5] | b[6] | b[7]));
}
PSL ps_spu_vec psl_gbb(ps_spu_vec x)
{
    const psv_u8 bits = (psv_u8)x & 1;
    uint32_t r = 0;
    for (int j = 0; j < 16; j++) r |= (uint32_t)bits[j] << j;
    return psl_pref(r);
}

/* ---- shufb (D-004: vqtbl2q over {rb, ra} with index 0x1F & ~rc, RPCS3's PANDN trick) ---- */

PSL ps_spu_vec psl_shufb(ps_spu_vec a, ps_spu_vec b, ps_spu_vec c)
{
    const psv_u8 cc = (psv_u8)c;
#if defined(__aarch64__)
    const uint8x16x2_t table = {{(uint8x16_t)b, (uint8x16_t)a}};
    const uint8x16_t picked = vqtbl2q_u8(table, vbicq_u8(vdupq_n_u8(0x1F), (uint8x16_t)cc));
    /* The special controls in one table lookup (D-218): (c ^ 0x80) >> 3 is 0-7 for 10xxxxxx (0x00), 8-11 for
       110xxxxx (0xFF), 12-15 for 111xxxxx (0x80), and 16 or more for the other bytes, which tbx leaves as picked. */
    const uint8x16_t special = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    return (ps_spu_vec)vqtbx1q_u8(picked, special, vshrq_n_u8(veorq_u8((uint8x16_t)cc, vdupq_n_u8(0x80)), 3));
#else
    psv_u8 picked;
    const psv_u8 bb = (psv_u8)b, aa = (psv_u8)a;
    for (int j = 0; j < 16; j++) {
        const unsigned idx = 0x1Fu & ~(unsigned)cc[j];
        picked[j] = idx < 16 ? bb[idx] : aa[idx - 16];
    }
    /* control 10xxxxxx -> 0x00, 110xxxxx -> 0xFF, 111xxxxx -> 0x80 */
    const psv_u8 special = (psv_u8)((psv_s8)cc < 0);
    const psv_u8 top3 = cc & 0xE0;
    const psv_u8 ff = (psv_u8)(top3 == 0xC0);
    const psv_u8 x80 = (psv_u8)(top3 == 0xE0) & 0x80;
    return (ps_spu_vec)((picked & ~special) | ((ff | x80) & special));
#endif
}

/* cbd/chd/cwd/cdd and x forms: identity mask over rb bytes with an insertion slot for `size` bytes at addr */
PSL ps_spu_vec psl_cmask(uint32_t addr, uint32_t size)
{
    const psv_u8 j = (psv_u8){0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const psv_u8 g = 15 - j; /* guest byte index of host byte j */
    const uint8_t t = (uint8_t)(addr & 0xF & ~(size - 1));
    const uint8_t first = (uint8_t)(size == 1 ? 3 : size == 2 ? 2 : 0);
    const psv_u8 in = (psv_u8)(g >= t) & (psv_u8)(g < (uint8_t)(t + size));
    const psv_u8 ins = g + (uint8_t)(first - t);
    return (ps_spu_vec)((ins & in) | ((g + 0x10) & ~in));
}

/* ---- element shifts and rotates: count vectors (register forms) ---- */

PSL ps_spu_vec psl_shl(ps_spu_vec x, ps_spu_vec y)
{
    const ps_spu_vec n = y & 0x3F;
    return (x << (n & 31)) & (ps_spu_vec)(n < 32);
}
PSL ps_spu_vec psl_shlh(ps_spu_vec x, ps_spu_vec y)
{
    const psv_u16 n = (psv_u16)y & 0x1F;
    return (ps_spu_vec)(((psv_u16)x << (n & 15)) & (psv_u16)(n < 16));
}
PSL ps_spu_vec psl_rot(ps_spu_vec x, ps_spu_vec y)
{
    const ps_spu_vec n = y & 31;
    return (x << n) | (x >> ((32 - n) & 31));
}
PSL ps_spu_vec psl_roth(ps_spu_vec x, ps_spu_vec y)
{
    const psv_u16 v = (psv_u16)x, n = (psv_u16)y & 15;
    return (ps_spu_vec)((v << n) | (v >> ((16 - n) & 15)));
}
PSL ps_spu_vec psl_rotm(ps_spu_vec x, ps_spu_vec y)
{
    const ps_spu_vec n = (0 - y) & 0x3F;
    return (x >> (n & 31)) & (ps_spu_vec)(n < 32);
}
PSL ps_spu_vec psl_rothm(ps_spu_vec x, ps_spu_vec y)
{
    const psv_u16 n = (0 - (psv_u16)y) & 0x1F;
    return (ps_spu_vec)(((psv_u16)x >> (n & 15)) & (psv_u16)(n < 16));
}
PSL ps_spu_vec psl_rotma(ps_spu_vec x, ps_spu_vec y)
{
    const ps_spu_vec n = (0 - y) & 0x3F;
    const ps_spu_vec big = (ps_spu_vec)(n > 31);
    return (ps_spu_vec)((psv_s32)x >> (psv_s32)((n & ~big) | (31 & big)));
}
PSL ps_spu_vec psl_rotmah(ps_spu_vec x, ps_spu_vec y)
{
    const psv_u16 n = (0 - (psv_u16)y) & 0x1F;
    const psv_u16 big = (psv_u16)(n > 15);
    return (ps_spu_vec)((psv_s16)x >> (psv_s16)((n & ~big) | (15 & big)));
}

/* constant counts (immediate forms; the lifter passes the effective count, so >= width is decided here) */
PSL ps_spu_vec psl_shli(ps_spu_vec x, unsigned n) { return n < 32 ? x << n : (ps_spu_vec){0, 0, 0, 0}; }
PSL ps_spu_vec psl_shlhi(ps_spu_vec x, unsigned n) { return n < 16 ? (ps_spu_vec)((psv_u16)x << n) : (ps_spu_vec){0, 0, 0, 0}; }
PSL ps_spu_vec psl_shri(ps_spu_vec x, unsigned n) { return n < 32 ? x >> n : (ps_spu_vec){0, 0, 0, 0}; }
PSL ps_spu_vec psl_shrhi(ps_spu_vec x, unsigned n) { return n < 16 ? (ps_spu_vec)((psv_u16)x >> n) : (ps_spu_vec){0, 0, 0, 0}; }
PSL ps_spu_vec psl_srai(ps_spu_vec x, unsigned n) { return (ps_spu_vec)((psv_s32)x >> (n < 31 ? n : 31)); }
PSL ps_spu_vec psl_srahi(ps_spu_vec x, unsigned n) { return (ps_spu_vec)((psv_s16)x >> (n < 15 ? n : 15)); }
PSL ps_spu_vec psl_roti(ps_spu_vec x, unsigned n) { return n ? (x << n) | (x >> (32 - n)) : x; }
PSL ps_spu_vec psl_rothi(ps_spu_vec x, unsigned n)
{
    const psv_u16 v = (psv_u16)x;
    return n ? (ps_spu_vec)((v << n) | (v >> (16 - n))) : x;
}

/* ---- quadword shifts: the register is the guest's 128-bit value as a little-endian integer (D-004) ---- */

PSL psl_u128 psl_to128(ps_spu_vec v)
{
    psl_u128 r;
    memcpy(&r, &v, 16);
    return r;
}
PSL ps_spu_vec psl_from128(psl_u128 r)
{
    ps_spu_vec v;
    memcpy(&v, &r, 16);
    return v;
}
#if defined(__aarch64__)
/* Byte shifts and rotates as one table lookup (D-127, again in D-217): the 128-bit integer forms moved the value
 * through general registers (6 instructions for a constant count, about 30 and a branch for a register count; rotqby
 * was 5% of GTA V's SPU samples, run 134). Host byte k of the result is host byte k - n (left: shlqby, rotqby) or
 * k + n (right: rotqmby) of x; an index past 15 reads zero. n < 32. */
PSL uint8x16_t psl_iota8(void) { return (uint8x16_t){0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}; }
PSL ps_spu_vec psl_shlqby(ps_spu_vec x, unsigned n)
{
    return (ps_spu_vec)vqtbl1q_u8((uint8x16_t)x, vsubq_u8(psl_iota8(), vdupq_n_u8((uint8_t)n)));
}
PSL ps_spu_vec psl_rotqmby(ps_spu_vec x, unsigned n)
{
    return (ps_spu_vec)vqtbl1q_u8((uint8x16_t)x, vaddq_u8(psl_iota8(), vdupq_n_u8((uint8_t)n)));
}
PSL ps_spu_vec psl_rotqby(ps_spu_vec x, unsigned n)
{
    return (ps_spu_vec)vqtbl1q_u8((uint8x16_t)x, vandq_u8(vsubq_u8(psl_iota8(), vdupq_n_u8((uint8_t)n)), vdupq_n_u8(15)));
}
#else
PSL ps_spu_vec psl_shlqby(ps_spu_vec x, unsigned n) { return n < 16 ? psl_from128(psl_to128(x) << (8 * n)) : (ps_spu_vec){0, 0, 0, 0}; }
PSL ps_spu_vec psl_rotqmby(ps_spu_vec x, unsigned n) { return n < 16 ? psl_from128(psl_to128(x) >> (8 * n)) : (ps_spu_vec){0, 0, 0, 0}; }
PSL ps_spu_vec psl_rotqby(ps_spu_vec x, unsigned n)
{
    const psl_u128 v = psl_to128(x);
    n &= 15;
    return n ? psl_from128((v << (8 * n)) | (v >> (128 - 8 * n))) : x;
}
#endif
PSL ps_spu_vec psl_shlqbi(ps_spu_vec x, unsigned n) { return psl_from128(psl_to128(x) << (n & 7)); }
PSL ps_spu_vec psl_rotqmbi(ps_spu_vec x, unsigned n) { return psl_from128(psl_to128(x) >> (n & 7)); }
PSL ps_spu_vec psl_rotqbi(ps_spu_vec x, unsigned n)
{
    const psl_u128 v = psl_to128(x);
    n &= 7;
    return n ? psl_from128((v << n) | (v >> (128 - n))) : x;
}

/* ---- single precision (fp.h) ---- */

PSL ps_spu_vec psl_zero_class(ps_spu_vec x) { return (ps_spu_vec)((x & 0x7F800000u) == 0); }
PSL ps_spu_vec psl_exp255(ps_spu_vec x) { return (ps_spu_vec)((x & 0x7F800000u) == 0x7F800000u); }
/* RPCS3 clamp_smax: exponent-255 values become +-FLT_MAX */
PSL ps_spu_vec psl_clamp(ps_spu_vec x) { return psl_bsel(psl_exp255(x), x, (x & 0x80000000u) | 0x7F7FFFFFu); }
PSL ps_spu_vec psl_vfma(ps_spu_vec a, ps_spu_vec b, ps_spu_vec c)
{
#if defined(__clang__)
    return (ps_spu_vec)__builtin_elementwise_fma((psv_f32)a, (psv_f32)b, (psv_f32)c);
#else
    psv_f32 r;
    for (int i = 0; i < 4; i++) r[i] = __builtin_fmaf(((psv_f32)a)[i], ((psv_f32)b)[i], ((psv_f32)c)[i]);
    return (ps_spu_vec)r;
#endif
}
PSL ps_spu_vec psl_fneg(ps_spu_vec x) { return x ^ 0x80000000u; }

/* approximate model */
PSL ps_spu_vec psl_fa(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_f32)x + (psv_f32)y); }
PSL ps_spu_vec psl_fs(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)((psv_f32)x - (psv_f32)psl_clamp(y)); }
PSL ps_spu_vec psl_fm(ps_spu_vec x, ps_spu_vec y)
{
    const ps_spu_vec r = (ps_spu_vec)((psv_f32)x * (psv_f32)y);
    return r & ~(psl_zero_class(x) | psl_zero_class(y));
}
PSL ps_spu_vec psl_fma(ps_spu_vec a, ps_spu_vec b, ps_spu_vec c)
{
    return psl_bsel(psl_zero_class(a) | psl_zero_class(b), psl_vfma(a, b, c), c);
}
PSL ps_spu_vec psl_fnms(ps_spu_vec a, ps_spu_vec b, ps_spu_vec c) { return psl_vfma(psl_fneg(psl_clamp(a)), psl_clamp(b), c); }
PSL ps_spu_vec psl_fms(ps_spu_vec a, ps_spu_vec b, ps_spu_vec c) { return psl_vfma(psl_clamp(a), psl_clamp(b), psl_fneg(c)); }

/* accurate (extended-range) model: per element through double precision */
#define PSL_XF2(name, expr)                                                                                      \
    PSL ps_spu_vec name(ps_spu_vec x, ps_spu_vec y)                                                              \
    {                                                                                                            \
        ps_spu_vec r;                                                                                            \
        for (int i = 0; i < 4; i++) {                                                                            \
            const double a = ps_spu_xf_to_f64(x[i]), b = ps_spu_xf_to_f64(y[i]);                                 \
            r[i] = ps_spu_f64_to_xf(expr);                                                                       \
        }                                                                                                        \
        return r;                                                                                                \
    }
#define PSL_XF3(name, expr)                                                                                      \
    PSL ps_spu_vec name(ps_spu_vec x, ps_spu_vec y, ps_spu_vec z)                                                \
    {                                                                                                            \
        ps_spu_vec r;                                                                                            \
        for (int i = 0; i < 4; i++) {                                                                            \
            const double a = ps_spu_xf_to_f64(x[i]), b = ps_spu_xf_to_f64(y[i]), c = ps_spu_xf_to_f64(z[i]);     \
            r[i] = ps_spu_f64_to_xf(expr);                                                                       \
        }                                                                                                        \
        return r;                                                                                                \
    }
PSL_XF2(psl_fa_x, a + b)
PSL_XF2(psl_fs_x, a - b)
PSL_XF2(psl_fm_x, a * b)
PSL_XF3(psl_fma_x, __builtin_fma(a, b, c))
PSL_XF3(psl_fnms_x, __builtin_fma(-a, b, c))
PSL_XF3(psl_fms_x, __builtin_fma(a, b, -c))
#undef PSL_XF2
#undef PSL_XF3

/* compares on SPU numbers (both models): denormals equal zero, sign-magnitude integer order */
PSL ps_spu_vec psl_fcgt(ps_spu_vec x, ps_spu_vec y)
{
    const ps_spu_vec both_zero = psl_zero_class(x) & psl_zero_class(y);
    const ps_spu_vec both_neg = (ps_spu_vec)((psv_s32)(x & y) < 0);
    const ps_spu_vec gt = psl_bsel(both_neg, (ps_spu_vec)((psv_s32)x > (psv_s32)y), (ps_spu_vec)((psv_s32)x < (psv_s32)y));
    return gt & ~both_zero;
}
PSL ps_spu_vec psl_fcmgt(ps_spu_vec x, ps_spu_vec y)
{
    return (ps_spu_vec)((x & 0x7FFFFFFFu) > (y & 0x7FFFFFFFu)) & ~(psl_zero_class(x) & psl_zero_class(y));
}
PSL ps_spu_vec psl_fceq(ps_spu_vec x, ps_spu_vec y) { return (ps_spu_vec)(x == y) | (psl_zero_class(x) & psl_zero_class(y)); }
PSL ps_spu_vec psl_fcmeq(ps_spu_vec x, ps_spu_vec y)
{
    return (ps_spu_vec)((x & 0x7FFFFFFFu) == (y & 0x7FFFFFFFu)) | (psl_zero_class(x) & psl_zero_class(y));
}

/* estimates: RPCS3 tables and fixed-point fi ("spu_fi", SPULLVMRecompiler.cpp) */
PSL ps_spu_vec psl_frest(ps_spu_vec x)
{
    ps_spu_vec r;
    for (int i = 0; i < 4; i++)
        r[i] = ps_spu_frest_fraction_lut[(x[i] >> 18) & 0x1F] | ps_spu_frest_exponent_lut[(x[i] >> 23) & 0xFF] |
            (x[i] & 0x80000000u);
    return r;
}
PSL ps_spu_vec psl_frsqest(ps_spu_vec x)
{
    ps_spu_vec r;
    for (int i = 0; i < 4; i++) r[i] = ps_spu_frsqest_fraction_lut[(x[i] >> 18) & 0x3F] | ps_spu_frsqest_exponent_lut[(x[i] >> 23) & 0xFF];
    return r;
}
PSL ps_spu_vec psl_fi(ps_spu_vec a, ps_spu_vec b)
{
    const ps_spu_vec base = (b & 0x007FFC00u) << 9;
    const ps_spu_vec ymul = (b & 0x3FFu) * (a & 0x7FFFFu);
    const ps_spu_vec adjust = (ps_spu_vec)(ymul > base);
    const ps_spu_vec bnew = (base - ymul) >> (9 + adjust); /* adjust = all ones: shift by 8 */
    const ps_spu_vec expo = psl_clamp((b & 0xFF800000u) - (adjust & 0x00800000u));
    return expo | (bnew & 0x007FFFFFu);
}

/* conversions: exact scaling through double, then the model's rounding (csflt/cuflt) or saturation */
PSL double psl_pow2(int k)
{
    const uint64_t bits = (uint64_t)(1023 + k) << 52;
    return ps_spu_f64(bits);
}
PSL ps_spu_vec psl_csflt(ps_spu_vec x, unsigned i8, int accurate)
{
    const double scale = psl_pow2((int)i8 - 155);
    ps_spu_vec r;
    for (int i = 0; i < 4; i++) {
        const double d = (double)(int32_t)x[i] * scale;
        r[i] = accurate ? ps_spu_f64_to_xf(d) : ps_spu_u32f((float)d);
    }
    return r;
}
PSL ps_spu_vec psl_cuflt(ps_spu_vec x, unsigned i8, int accurate)
{
    const double scale = psl_pow2((int)i8 - 155);
    ps_spu_vec r;
    for (int i = 0; i < 4; i++) {
        const double d = (double)x[i] * scale;
        r[i] = accurate ? ps_spu_f64_to_xf(d) : ps_spu_u32f((float)d);
    }
    return r;
}
#if defined(__aarch64__)
/* cflts/cfltu in NEON (D-215) when the scale 2^(173 - i8) is a normal float: x times it is exact or past the integer
 * range, and FCVTZS/FCVTZU truncate and saturate as the SPU does. The SPU's own float range: exponent 0 is zero, and
 * exponent 255 (infinity or NaN to the host) is a huge number, saturated by its sign. GTA V's busiest geometry job
 * converts four lanes at a time through double precision lane by lane in the scalar form (run 133's profile). */
PSL int psl_cflt_neon_scale(unsigned i8) { return (int)i8 >= 173 - 126 && (int)i8 <= 173 + 126; }
PSL ps_spu_vec psl_cflt_neon(ps_spu_vec x, unsigned i8, int is_signed)
{
    const uint32x4_t v = (uint32x4_t)x;
    const uint32x4_t expo = vandq_u32(v, vdupq_n_u32(0x7F800000u));
    const uint32x4_t is_zero = vceqq_u32(expo, vdupq_n_u32(0));
    const uint32x4_t is_huge = vceqq_u32(expo, vdupq_n_u32(0x7F800000u));
    float32x4_t f = vreinterpretq_f32_u32(v);
    if (i8 != 173) f = vmulq_f32(f, vdupq_n_f32(ps_spu_f32((uint32_t)(127 + 173 - (int)i8) << 23)));
    const uint32x4_t negative = vcltq_s32(vreinterpretq_s32_u32(v), vdupq_n_s32(0));
    uint32x4_t r, huge;
    if (is_signed) {
        r = vreinterpretq_u32_s32(vcvtq_s32_f32(f));
        huge = vaddq_u32(vdupq_n_u32(0x7FFFFFFFu), vshrq_n_u32(v, 31)); /* 0x7FFFFFFF, or 0x80000000 if negative */
    } else {
        r = vcvtq_u32_f32(f);
        huge = vbicq_u32(vdupq_n_u32(0xFFFFFFFFu), negative); /* 0xFFFFFFFF, or 0 if negative */
    }
    r = vbslq_u32(is_huge, huge, r);
    return (ps_spu_vec)vbicq_u32(r, is_zero);
}
#endif
PSL ps_spu_vec psl_cflts(ps_spu_vec x, unsigned i8)
{
#if defined(__aarch64__)
    if (psl_cflt_neon_scale(i8)) return psl_cflt_neon(x, i8, 1);
#endif
    const double scale = psl_pow2(173 - (int)i8);
    ps_spu_vec r;
    for (int i = 0; i < 4; i++) {
        if ((x[i] & 0x7F800000u) == 0) {
            r[i] = 0;
            continue;
        }
        const double d = ps_spu_xf_to_f64(x[i]) * scale;
        r[i] = d >= 2147483648.0 ? 0x7FFFFFFFu : d < -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)(int64_t)d;
    }
    return r;
}
PSL ps_spu_vec psl_cfltu(ps_spu_vec x, unsigned i8)
{
#if defined(__aarch64__)
    if (psl_cflt_neon_scale(i8)) return psl_cflt_neon(x, i8, 0);
#endif
    const double scale = psl_pow2(173 - (int)i8);
    ps_spu_vec r;
    for (int i = 0; i < 4; i++) {
        if ((x[i] & 0x7F800000u) == 0 || (x[i] & 0x80000000u)) {
            r[i] = 0;
            continue;
        }
        const double d = ps_spu_xf_to_f64(x[i]) * scale;
        r[i] = d >= 4294967296.0 ? 0xFFFFFFFFu : (uint32_t)(uint64_t)d;
    }
    return r;
}

/* ---- double precision: one round-to-nearest FPCR window per instruction (fp.h) ---- */

#if defined(__aarch64__)
#define PSL_DP2(name, insn)                                                                                      \
    PSL ps_spu_vec name(ps_spu_vec x, ps_spu_vec y)                                                              \
    {                                                                                                            \
        ps_spu_vec r;                                                                                            \
        uint64_t sv, nw;                                                                                         \
        PS_SPU_DP_WINDOW(insn " %[r].2d, %[a].2d, %[b].2d",                                                      \
            : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(x), [b] "w"(y));                            \
        return r;                                                                                                \
    }
PSL_DP2(psl_dfa, "fadd")
PSL_DP2(psl_dfs, "fsub")
PSL_DP2(psl_dfm, "fmul")
#undef PSL_DP2
/* t + a * b, one rounding */
PSL ps_spu_vec psl_dfmla(ps_spu_vec t, ps_spu_vec x, ps_spu_vec y)
{
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fmla %[t].2d, %[a].2d, %[b].2d", : [t] "+w"(t), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(x), [b] "w"(y));
    return t;
}
/* t - a * b, one rounding */
PSL ps_spu_vec psl_dfmls(ps_spu_vec t, ps_spu_vec x, ps_spu_vec y)
{
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fmls %[t].2d, %[a].2d, %[b].2d", : [t] "+w"(t), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(x), [b] "w"(y));
    return t;
}
/* frds: DP lanes rounded to nearest into SP lanes 0 and 1 of the result */
PSL ps_spu_vec psl_dp_narrow(ps_spu_vec x)
{
    ps_spu_vec r;
    uint64_t sv, nw;
    PS_SPU_DP_WINDOW("fcvtn %[r].2s, %[a].2d", : [r] "=&w"(r), [sv] "=&r"(sv), [nw] "=&r"(nw) : [a] "w"(x));
    return r;
}
#else
PSL ps_spu_vec psl_dfa(ps_spu_vec x, ps_spu_vec y)
{
    psv_f64 r;
    for (int i = 0; i < 2; i++) r[i] = ps_spu_dp_add(((psv_f64)x)[i], ((psv_f64)y)[i]);
    return (ps_spu_vec)r;
}
PSL ps_spu_vec psl_dfs(ps_spu_vec x, ps_spu_vec y)
{
    psv_f64 r;
    for (int i = 0; i < 2; i++) r[i] = ps_spu_dp_sub(((psv_f64)x)[i], ((psv_f64)y)[i]);
    return (ps_spu_vec)r;
}
PSL ps_spu_vec psl_dfm(ps_spu_vec x, ps_spu_vec y)
{
    psv_f64 r;
    for (int i = 0; i < 2; i++) r[i] = ps_spu_dp_mul(((psv_f64)x)[i], ((psv_f64)y)[i]);
    return (ps_spu_vec)r;
}
PSL ps_spu_vec psl_dfmla(ps_spu_vec t, ps_spu_vec x, ps_spu_vec y)
{
    psv_f64 r;
    for (int i = 0; i < 2; i++) r[i] = ps_spu_dp_fma(((psv_f64)x)[i], ((psv_f64)y)[i], ((psv_f64)t)[i]);
    return (ps_spu_vec)r;
}
PSL ps_spu_vec psl_dfmls(ps_spu_vec t, ps_spu_vec x, ps_spu_vec y)
{
    psv_f64 r;
    for (int i = 0; i < 2; i++) r[i] = ps_spu_dp_fma(-((psv_f64)x)[i], ((psv_f64)y)[i], ((psv_f64)t)[i]);
    return (ps_spu_vec)r;
}
PSL ps_spu_vec psl_dp_narrow(ps_spu_vec x)
{
    psv_f32 r = {ps_spu_dp_to_sp(((psv_f64)x)[0]), ps_spu_dp_to_sp(((psv_f64)x)[1]), 0.0f, 0.0f};
    return (ps_spu_vec)r;
}
#endif

PSL ps_spu_vec psl_dneg(ps_spu_vec x) { return (ps_spu_vec)((psv_u64)x ^ 0x8000000000000000ull); }
PSL ps_spu_vec psl_dfma(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return psl_dfmla(t, x, y); }
PSL ps_spu_vec psl_dfms(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return psl_dfmla(psl_dneg(t), x, y); }
PSL ps_spu_vec psl_dfnms(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return psl_dfmls(t, x, y); }
PSL ps_spu_vec psl_dfnma(ps_spu_vec x, ps_spu_vec y, ps_spu_vec t) { return psl_dneg(psl_dfmla(t, x, y)); }
/* fesd: DP lane k <- SP lane 2k+1 (guest DP element i <- SP element 2i), exact */
PSL ps_spu_vec psl_fesd(ps_spu_vec x)
{
    const psv_f32 f = (psv_f32)x;
    return (ps_spu_vec)(psv_f64){(double)f[1], (double)f[3]};
}
/* frds: SP lane 2k+1 <- DP lane k rounded to nearest, even SP lanes zero */
PSL ps_spu_vec psl_frds(ps_spu_vec x)
{
    const ps_spu_vec n = psl_dp_narrow(x);
    return (ps_spu_vec){0, n[0], 0, n[1]};
}

#undef PSL

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_SPU_LIFTED_H */
