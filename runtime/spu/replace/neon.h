/*
 * Building blocks of the native replacements (D-965): SPU operations four lanes at a time in NEON, each giving what
 * the lifted code's helper (pspocket/spu/lifted.h) gives, bit for bit; tests/spu/test_spu_replace.cpp checks them
 * against those helpers. Lane-wise operations do not care about the lane order: the native code may keep a vector in
 * the order the data has in local store (guest element i in lane i) rather than the registers' (D-004: lane 3 - i).
 */
#ifndef PSPOCKET_SPU_REPLACE_NEON_H
#define PSPOCKET_SPU_REPLACE_NEON_H

#include <stdint.h>

#include "pspocket/spu/lifted.h"

#if !defined(__aarch64__)
#error "the native SPU replacements are AArch64 code"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PSN static inline __attribute__((always_inline))

/* The tables' bytes by plane (byte k of entry i at [k][i]): RPCS3's spu_frest_fraction_lut and
 * spu_frsqest_fraction_lut (rpcs3/Emu/Cell/SPUThread.cpp), as pspocket/spu/fp.h has them (replace_tables.c). */
extern const uint8_t ps_spu_frest_planes[3][32];
extern const uint8_t ps_spu_frsqest_planes[3][64];

/* frest (psl_frest): the fraction from RPCS3's 32-entry table by bits 18-22, the exponent by its closed form (fp.cpp:
 * exponent 0 -> 0x7F800000, else max(253 - e, 0) << 23), the sign kept. The table lookup is two TBLs on the byte
 * planes: byte k of a lane reads plane k at the lane's index. */
typedef struct {
    uint8x16x4_t p01; /* planes 0 and 1: bytes 0-63 */
    uint8x16x2_t p2;
} psn_frest_tables;
/* The planes in registers: a loop loads them once (stores to local store may alias the tables for the compiler). */
PSN psn_frest_tables psn_frest_load(void)
{
    psn_frest_tables t;
    t.p01 = vld1q_u8_x4(&ps_spu_frest_planes[0][0]);
    t.p2 = vld1q_u8_x2(&ps_spu_frest_planes[2][0]);
    return t;
}
PSN uint32x4_t psn_frest_t(uint32x4_t x, const psn_frest_tables* t)
{
    const uint32x4_t idx = vandq_u32(vshrq_n_u32(x, 18), vdupq_n_u32(0x1F));
    const uint32x4_t spread = vmulq_u32(idx, vdupq_n_u32(0x01010101u)); /* the index in each byte */
    /* byte 0: plane 0 (idx), byte 1: plane 1 (idx + 32), bytes 2, 3: out of range (0) */
    const uint8x16_t i01 = vreinterpretq_u8_u32(vaddq_u32(spread, vdupq_n_u32(0xC0C02000u)));
    /* byte 2: plane 2 (idx), the others out of range */
    const uint8x16_t i2 = vreinterpretq_u8_u32(vorrq_u32(spread, vdupq_n_u32(0xC000C0C0u)));
    const uint32x4_t frac = vorrq_u32(vreinterpretq_u32_u8(vqtbl4q_u8(t->p01, i01)), vreinterpretq_u32_u8(vqtbl2q_u8(t->p2, i2)));
    const uint32x4_t e = vandq_u32(vshrq_n_u32(x, 23), vdupq_n_u32(0xFF));
    const uint32x4_t expo = vbslq_u32(vceqzq_u32(e), vdupq_n_u32(0x7F800000u), vshlq_n_u32(vqsubq_u32(vdupq_n_u32(253), e), 23));
    return vorrq_u32(vorrq_u32(frac, expo), vandq_u32(x, vdupq_n_u32(0x80000000u)));
}
PSN uint32x4_t psn_frest(uint32x4_t x)
{
    const psn_frest_tables t = psn_frest_load();
    return psn_frest_t(x, &t);
}

/* frsqest (psl_frsqest): the fraction from RPCS3's 64-entry table by bits 18-23, the exponent by its closed form
 * (exponent 0 -> 0x7F800000, else (190 - (e + 1) / 2) << 23), no sign. Three lookups, one a plane (TBL, then TBX). */
typedef struct {
    uint8x16x4_t p[3];
} psn_frsqest_tables;
/* The planes in registers: a loop loads them once (stores to local store may alias the tables for the compiler). */
PSN psn_frsqest_tables psn_frsqest_load(void)
{
    psn_frsqest_tables t;
    for (int k = 0; k < 3; k++) t.p[k] = vld1q_u8_x4(&ps_spu_frsqest_planes[k][0]);
    return t;
}
PSN uint32x4_t psn_frsqest_t(uint32x4_t x, const psn_frsqest_tables* t)
{
    const uint32x4_t idx = vandq_u32(vshrq_n_u32(x, 18), vdupq_n_u32(0x3F));
    /* byte k of a lane: idx + 64 k; byte 3 always out of range */
    const uint8x16_t i = vreinterpretq_u8_u32(vaddq_u32(vmulq_u32(idx, vdupq_n_u32(0x01010101u)), vdupq_n_u32(0xC0804000u)));
    uint8x16_t r = vqtbl4q_u8(t->p[0], i);
    r = vqtbx4q_u8(r, t->p[1], vsubq_u8(i, vdupq_n_u8(64)));
    r = vqtbx4q_u8(r, t->p[2], vsubq_u8(i, vdupq_n_u8(128)));
    const uint32x4_t e = vandq_u32(vshrq_n_u32(x, 23), vdupq_n_u32(0xFF));
    const uint32x4_t half = vshrq_n_u32(vaddq_u32(e, vdupq_n_u32(1)), 1);
    const uint32x4_t expo = vbslq_u32(vceqzq_u32(e), vdupq_n_u32(0x7F800000u), vshlq_n_u32(vsubq_u32(vdupq_n_u32(190), half), 23));
    return vorrq_u32(vreinterpretq_u32_u8(r), expo);
}
PSN uint32x4_t psn_frsqest(uint32x4_t x)
{
    const psn_frsqest_tables t = psn_frsqest_load();
    return psn_frsqest_t(x, &t);
}

/* Zero class (psl_zero_class): all ones where the exponent is 0 (zero or denormal: the SPU's zeros); psn_nz is its
 * complement in one instruction (all ones where the exponent is not 0). */
PSN uint32x4_t psn_zc(uint32x4_t x) { return vceqzq_u32(vandq_u32(x, vdupq_n_u32(0x7F800000u))); }
PSN uint32x4_t psn_nz(uint32x4_t x) { return vtstq_u32(x, vdupq_n_u32(0x7F800000u)); }

/* fm with the operands' "not a zero" masks combined (nab = psn_nz(a) & psn_nz(b)), as psl_fm: +0 where either is a
 * zero. */
PSN uint32x4_t psn_fm_n(uint32x4_t a, uint32x4_t b, uint32x4_t nab)
{
    return vandq_u32(vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(a), vreinterpretq_f32_u32(b))), nab);
}
PSN uint32x4_t psn_fm(uint32x4_t a, uint32x4_t b) { return psn_fm_n(a, b, vandq_u32(psn_nz(a), psn_nz(b))); }

/* fma a * b + c with nab = psn_nz(a) & psn_nz(b), as psl_fma: c unchanged where either factor is a zero, else fused. */
PSN uint32x4_t psn_fma_n(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t nab)
{
    const float32x4_t r = vfmaq_f32(vreinterpretq_f32_u32(c), vreinterpretq_f32_u32(a), vreinterpretq_f32_u32(b));
    return vbslq_u32(nab, vreinterpretq_u32_f32(r), c);
}
PSN uint32x4_t psn_fma(uint32x4_t a, uint32x4_t b, uint32x4_t c) { return psn_fma_n(a, b, c, vandq_u32(psn_nz(a), psn_nz(b))); }

/* A key whose signed order is fcgt's (psl_fcgt: SPU numbers, sign and magnitude, every zero and denormal equal):
 * zeros to +0, then the magnitude bits flipped for negatives. fcgt(x, y) = key(x) > key(y), signed. */
PSN int32x4_t psn_fkey(uint32x4_t x)
{
    const int32x4_t v = vreinterpretq_s32_u32(vandq_u32(x, psn_nz(x)));
    return veorq_s32(v, vreinterpretq_s32_u32(vshrq_n_u32(vreinterpretq_u32_s32(vshrq_n_s32(v, 31)), 1)));
}
PSN uint32x4_t psn_fcgt_k(int32x4_t kx, int32x4_t ky) { return vcgtq_s32(kx, ky); }

/* Big-endian words of local store and back: four quadwords at ls + addr (16-byte aligned, addr + 64 within local
 * store), word w of quadword k into lane k of val[w], each word's bytes reversed (to host order). */
PSN uint32x4x4_t psn_ld4_be(const uint8_t* ls, uint32_t addr)
{
    uint32x4x4_t v = vld4q_u32((const uint32_t*)(const void*)(ls + addr));
    for (int w = 0; w < 4; w++) v.val[w] = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(v.val[w])));
    return v;
}
PSN void psn_st4_be(uint8_t* ls, uint32_t addr, uint32x4x4_t v)
{
    for (int w = 0; w < 4; w++) v.val[w] = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(v.val[w])));
    vst4q_u32((uint32_t*)(void*)(ls + addr), v);
}

/* Lane order: a vector in local-store order (guest element i in lane i) to the registers' (D-004) and back. */
PSN uint32x4_t psn_rev4(uint32x4_t v)
{
    const uint32x4_t r = vrev64q_u32(v);
    return vextq_u32(r, r, 2);
}

/* shufb (psl_shufb) on vectors in local-store word order (guest word i in lane i, each word's bytes in host order):
 * guest byte g of such a vector is its byte g ^ 3, so a control byte c picks byte (c & 31) ^ 3 of {a, b}; the special
 * controls (10xxxxxx, 110xxxxx, 111xxxxx: 0x00, 0xFF, 0x80) as psl_shufb makes them. */
PSN uint32x4_t psn_shufb_w(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    const uint8x16_t cc = vreinterpretq_u8_u32(c);
    const uint8x16x2_t t = {{vreinterpretq_u8_u32(a), vreinterpretq_u8_u32(b)}};
    const uint8x16_t picked = vqtbl2q_u8(t, veorq_u8(vandq_u8(cc, vdupq_n_u8(0x1F)), vdupq_n_u8(3)));
    const uint8x16_t special = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    return vreinterpretq_u32_u8(vqtbx1q_u8(picked, special, vshrq_n_u8(veorq_u8(cc, vdupq_n_u8(0x80)), 3)));
}

/* A control known ahead (psn_shufb_w's), as a lookup and the special bytes or'ed in: psn_shufb_k(a, b, k) is
 * psn_shufb_w(a, b, c) for k = psn_shufb_key(c). */
typedef struct {
    uint8x16_t idx, orr;
} psn_shufb_key_t;
PSN psn_shufb_key_t psn_shufb_key(uint32x4_t c)
{
    const uint8x16_t cc = vreinterpretq_u8_u32(c);
    const uint8x16_t sel = vcltq_u8(cc, vdupq_n_u8(0x80)); /* a byte picked */
    const uint8x16_t special = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    psn_shufb_key_t k;
    k.idx = vorrq_u8(veorq_u8(vandq_u8(cc, vdupq_n_u8(0x1F)), vdupq_n_u8(3)), vmvnq_u8(sel)); /* 0xFF: out of range */
    k.orr = vbicq_u8(vqtbl1q_u8(special, vshrq_n_u8(veorq_u8(cc, vdupq_n_u8(0x80)), 3)), sel);
    return k;
}
PSN uint32x4_t psn_shufb_k(uint32x4_t a, uint32x4_t b, psn_shufb_key_t k)
{
    const uint8x16x2_t t = {{vreinterpretq_u8_u32(a), vreinterpretq_u8_u32(b)}};
    return vreinterpretq_u32_u8(vorrq_u8(vqtbl2q_u8(t, k.idx), k.orr));
}

/* shufb on vectors in local store's byte order (guest byte i in byte i, as vld1q reads a quadword): control byte c
 * picks byte c & 31 of {a, b}; the special controls as psl_shufb makes them. psn_shufb_key_b makes a control known
 * ahead a lookup and the special bytes; psn_shufb_k applies it (its table is {a, b} in either order's own layout). */
PSN uint32x4_t psn_shufb_b(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    const uint8x16_t cc = vreinterpretq_u8_u32(c);
    const uint8x16x2_t t = {{vreinterpretq_u8_u32(a), vreinterpretq_u8_u32(b)}};
    const uint8x16_t picked = vqtbl2q_u8(t, vandq_u8(cc, vdupq_n_u8(0x1F)));
    const uint8x16_t special = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    return vreinterpretq_u32_u8(vqtbx1q_u8(picked, special, vshrq_n_u8(veorq_u8(cc, vdupq_n_u8(0x80)), 3)));
}
PSN psn_shufb_key_t psn_shufb_key_b(uint32x4_t c)
{
    const uint8x16_t cc = vreinterpretq_u8_u32(c);
    const uint8x16_t sel = vcltq_u8(cc, vdupq_n_u8(0x80));
    const uint8x16_t special = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    psn_shufb_key_t k;
    k.idx = vorrq_u8(vandq_u8(cc, vdupq_n_u8(0x1F)), vmvnq_u8(sel));
    k.orr = vbicq_u8(vqtbl1q_u8(special, vshrq_n_u8(veorq_u8(cc, vdupq_n_u8(0x80)), 3)), sel);
    return k;
}

/* A register's value in local store's byte order and back (D-004: the full reverse). */
PSN uint32x4_t psn_rev16(uint32x4_t v)
{
    const uint8x16_t r = vrev64q_u8(vreinterpretq_u8_u32(v));
    return vreinterpretq_u32_u8(vextq_u8(r, r, 8));
}

/* What fs and the like read of a float operand (psl_clamp): exponent 255 (a large number on the SPU) to the largest
 * finite float of its sign. */
PSN uint32x4_t psn_clamp(uint32x4_t x)
{
    const uint32x4_t e255 = vceqq_u32(vandq_u32(x, vdupq_n_u32(0x7F800000u)), vdupq_n_u32(0x7F800000u));
    return vbslq_u32(e255, vorrq_u32(vandq_u32(x, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7F7FFFFFu)), x);
}

#undef PSN

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_SPU_REPLACE_NEON_H */
