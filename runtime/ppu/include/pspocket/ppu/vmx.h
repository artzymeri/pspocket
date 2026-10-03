/*
 * VMX (AltiVec) for lifted code: one static inline NEON function per instruction (AArch64 only, D-003).
 *
 * Registers are uint32x4_t locals in the project lane order (D-004): host byte j = guest byte 15 - j, so guest
 * element i of an N-element vector is NEON lane N-1-i and element-wise arithmetic maps straight onto NEON.
 * Element-position operations (merges, packs, unpacks, even/odd multiplies, sums, splats, element loads/stores,
 * lvlx/lvrx, VSCR) translate indices; whole-register byte shifts are native shifts of the reversed register.
 *
 * Semantics: rpcs3/Emu/Cell/PPUInterpreter.cpp with its accuracy options on (set_sat, use_nj, set_vnan) and the
 * vector helpers of rpcs3/util/simd.hpp (its ARM64 paths, which use the same lane order). The interpreter
 * (runtime/ppu/src/interpreter_vmx.cpp) implements every instruction again with scalar code; the differential
 * fuzz test compares the two bit for bit, NaN payloads included:
 *
 * - VSCR[NJ] (1 after reset): instructions RPCS3 marks use_nj flush denormal inputs to signed zero, and every FP
 *   result that RPCS3 flushes (fix_nj) is flushed when NJ = 1 (ppu_flush_denormal; the "jm mask" is 0x7f800000
 *   with NJ = 1, 0x7fffffff with NJ = 0, which makes the flush the identity).
 * - NaNs (ppu_set_vnan): a NaN result is the first NaN operand, in vA, vB order, quieted; otherwise the default
 *   NaN 0x7fc00000. vmaddfp, vnmsubfp, vexptefp and vlogefp only replace a NaN result by the default NaN.
 * - VSCR[SAT] is sticky. Lifted code keeps it as a vector accumulator ("vsat", nonzero = set), as RPCS3 does;
 *   saturating instructions OR their saturation mask into it. The context holds the 0/1 bit.
 * - vexptefp/vlogefp are exp2f/log2f of the host libm (RPCS3 gv_exp2_approxfs/gv_log2_approxfs), vrefp is 1/x
 *   and vrsqrtefp 1/sqrt(x), all correctly rounded rather than the hardware estimates.
 * - mfvscr/mtvscr use guest word 3 (host lane 0), as the architecture and RPCS3's LLVM recompiler do (RPCS3's
 *   interpreter uses word 0).
 */
#ifndef PSPOCKET_PPU_VMX_H
#define PSPOCKET_PPU_VMX_H

#if !defined(__aarch64__)
#error "pspocket/ppu/vmx.h needs AArch64 NEON (lifted code is built for Apple silicon, D-003)"
#endif

#include <arm_neon.h>
#include <math.h>
#include <stdint.h>

#include "pspocket/core/be.h"
#include "pspocket/ppu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reinterpretations of the canonical uint32x4_t register type (free: no instructions). */
#define PS_VU8(x) vreinterpretq_u8_u32(x)
#define PS_VS8(x) vreinterpretq_s8_u32(x)
#define PS_VU16(x) vreinterpretq_u16_u32(x)
#define PS_VS16(x) vreinterpretq_s16_u32(x)
#define PS_VS32(x) vreinterpretq_s32_u32(x)
#define PS_VF32(x) vreinterpretq_f32_u32(x)
#define PS_VU64(x) vreinterpretq_u64_u32(x)
#define PS_VS64(x) vreinterpretq_s64_u32(x)
#define PS_FROM_U8(x) vreinterpretq_u32_u8(x)
#define PS_FROM_S8(x) vreinterpretq_u32_s8(x)
#define PS_FROM_U16(x) vreinterpretq_u32_u16(x)
#define PS_FROM_S16(x) vreinterpretq_u32_s16(x)
#define PS_FROM_S32(x) vreinterpretq_u32_s32(x)
#define PS_FROM_F32(x) vreinterpretq_u32_f32(x)
#define PS_FROM_U64(x) vreinterpretq_u32_u64(x)
#define PS_FROM_S64(x) vreinterpretq_u32_s64(x)

#define PS_VMX_INLINE static inline __attribute__((always_inline))

/* ------------------------------------------------------------------------------------------- registers and VSCR */

PS_VMX_INLINE uint32x4_t ps_vmx_get(const PPUContext* ctx, unsigned n) { return vld1q_u32(ctx->vr[n].u32); }
PS_VMX_INLINE void ps_vmx_set(PPUContext* ctx, unsigned n, uint32x4_t v) { vst1q_u32(ctx->vr[n].u32, v); }

/* SAT accumulator <-> the context bit. */
PS_VMX_INLINE uint32x4_t ps_vmx_sat_load(uint8_t sat) { return vdupq_n_u32(sat); }
PS_VMX_INLINE uint8_t ps_vmx_sat_bit(uint32x4_t sat) { return (uint8_t)(vmaxvq_u32(sat) != 0); }
PS_VMX_INLINE void ps_vmx_sat_or(uint32x4_t* sat, uint32x4_t m) { *sat = vorrq_u32(*sat, m); }

/* CR6 of the record forms of the compares: all elements true / all false. */
PS_VMX_INLINE uint8_t ps_vmx_all_ones(uint32x4_t v) { return (uint8_t)(vminvq_u32(v) == 0xFFFFFFFFu); }
PS_VMX_INLINE uint8_t ps_vmx_all_zero(uint32x4_t v) { return (uint8_t)(vmaxvq_u32(v) == 0); }

/* mfvscr: VSCR (SAT bit 0, NJ bit 16) in guest word 3 = host lane 0, the rest zero. */
PS_VMX_INLINE uint32x4_t ps_vmx_mfvscr(uint32x4_t sat, uint8_t nj)
{
    return vsetq_lane_u32((uint32_t)ps_vmx_sat_bit(sat) | ((uint32_t)nj << 16), vdupq_n_u32(0), 0);
}

/* ------------------------------------------------------------------------------------------- guest memory */

/* Guest memory is accessed through may_alias views, as in be.h. */
typedef uint8x16_t ps_vmx_q16 __attribute__((may_alias));

/* 16-byte aligned guest access (ea already masked), volatile like every guest access (D-013), reversed (D-004). */
PS_VMX_INLINE uint32x4_t ps_vmx_read128(uint8_t* base, uint32_t ea)
{
    const uint8x16_t raw = *(volatile const ps_vmx_q16*)__builtin_assume_aligned(base + ea, 16);
    const uint8x16_t r = vrev64q_u8(raw);
    return PS_FROM_U8(vextq_u8(r, r, 8));
}

PS_VMX_INLINE void ps_vmx_write128(uint8_t* base, uint32_t ea, uint32x4_t v)
{
    const uint8x16_t r = vrev64q_u8(PS_VU8(v));
    *(volatile ps_vmx_q16*)__builtin_assume_aligned(base + ea, 16) = vextq_u8(r, r, 8);
}

/* lvx lvxl lvebx lvehx lvewx: the aligned quadword (RPCS3 loads the whole quadword for the element loads too). */
PS_VMX_INLINE uint32x4_t ps_vmx_lvx(uint8_t* base, uint32_t ea) { return ps_vmx_read128(base, ea & ~15u); }
PS_VMX_INLINE void ps_vmx_stvx(uint8_t* base, uint32_t ea, uint32x4_t v) { ps_vmx_write128(base, ea & ~15u, v); }

/* stvebx/stvehx/stvewx: the element the (aligned) address selects. */
PS_VMX_INLINE void ps_vmx_stvebx(uint8_t* base, uint32_t ea, uint32x4_t v)
{
    uint8_t b[16];
    vst1q_u8(b, PS_VU8(v));
    ps_write8(base, ea, b[15 - (ea & 15)]);
}

PS_VMX_INLINE void ps_vmx_stvehx(uint8_t* base, uint32_t ea, uint32x4_t v)
{
    uint16_t h[8];
    vst1q_u16(h, PS_VU16(v));
    ea &= ~1u;
    ps_write16(base, ea, h[7 - ((ea & 15) >> 1)]);
}

PS_VMX_INLINE void ps_vmx_stvewx(uint8_t* base, uint32_t ea, uint32x4_t v)
{
    uint32_t w[4];
    vst1q_u32(w, v);
    ea &= ~3u;
    ps_write32(base, ea, w[3 - ((ea & 15) >> 2)]);
}

PS_VMX_INLINE uint8x16_t ps_vmx_iota(void)
{
    static const uint8_t k[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    return vld1q_u8(k);
}

/* lvsl/lvsr: guest byte i = sh + i / 16 - sh + i, sh = ea & 15 (host byte j = guest byte 15 - j). */
PS_VMX_INLINE uint32x4_t ps_vmx_lvsl(uint32_t ea)
{
    return PS_FROM_U8(vsubq_u8(vdupq_n_u8((uint8_t)(15 + (ea & 15))), ps_vmx_iota()));
}

PS_VMX_INLINE uint32x4_t ps_vmx_lvsr(uint32_t ea)
{
    return PS_FROM_U8(vsubq_u8(vdupq_n_u8((uint8_t)(31 - (ea & 15))), ps_vmx_iota()));
}

/* lvlx: the quadword shifted left by (ea & 15) bytes (guest order): host byte j = line byte j - k, or 0. */
PS_VMX_INLINE uint32x4_t ps_vmx_lvlx(uint8_t* base, uint32_t ea)
{
    const uint8x16_t line = PS_VU8(ps_vmx_read128(base, ea & ~15u));
    return PS_FROM_U8(vqtbl1q_u8(line, vsubq_u8(ps_vmx_iota(), vdupq_n_u8((uint8_t)(ea & 15)))));
}

/* lvrx: the bytes before ea in its line, right-aligned; nothing is read when ea is aligned (the result is 0). */
PS_VMX_INLINE uint32x4_t ps_vmx_lvrx(uint8_t* base, uint32_t ea)
{
    const uint32_t k = ea & 15;
    if (k == 0) return vdupq_n_u32(0);
    const uint8x16_t line = PS_VU8(ps_vmx_read128(base, ea & ~15u));
    return PS_FROM_U8(vqtbl1q_u8(line, vaddq_u8(ps_vmx_iota(), vdupq_n_u8((uint8_t)(16 - k)))));
}

/* stvlx: guest bytes 0 .. 15-k of v to ea .. end of the line. stvrx: guest bytes 16-k .. 15 to line .. ea-1.
 * Byte stores: the other bytes of the line belong to whoever else writes them (RPCS3 STVLX/STVRX). */
PS_VMX_INLINE void ps_vmx_stvlx(uint8_t* base, uint32_t ea, uint32x4_t v)
{
    uint8_t b[16];
    vst1q_u8(b, PS_VU8(v));
    const uint32_t k = ea & 15;
    for (uint32_t j = 0; j < 16 - k; j++) ps_write8(base, ea + j, b[15 - j]);
}

PS_VMX_INLINE void ps_vmx_stvrx(uint8_t* base, uint32_t ea, uint32x4_t v)
{
    uint8_t b[16];
    vst1q_u8(b, PS_VU8(v));
    const uint32_t k = ea & 15;
    const uint32_t line = ea & ~15u;
    for (uint32_t i = 16 - k; i < 16; i++) ps_write8(base, line + i - (16 - k), b[15 - i]);
}

/* ------------------------------------------------------------------------------------------- logical, select */

PS_VMX_INLINE uint32x4_t ps_vmx_vand(uint32x4_t a, uint32x4_t b) { return vandq_u32(a, b); }
PS_VMX_INLINE uint32x4_t ps_vmx_vandc(uint32x4_t a, uint32x4_t b) { return vbicq_u32(a, b); }
PS_VMX_INLINE uint32x4_t ps_vmx_vor(uint32x4_t a, uint32x4_t b) { return vorrq_u32(a, b); }
PS_VMX_INLINE uint32x4_t ps_vmx_vxor(uint32x4_t a, uint32x4_t b) { return veorq_u32(a, b); }
PS_VMX_INLINE uint32x4_t ps_vmx_vnor(uint32x4_t a, uint32x4_t b) { return vmvnq_u32(vorrq_u32(a, b)); }
PS_VMX_INLINE uint32x4_t ps_vmx_vsel(uint32x4_t a, uint32x4_t b, uint32x4_t c) { return vbslq_u32(c, b, a); }

/* vperm: RPCS3's PANDN trick, table {vB, vA} indexed by 31 - (c & 31) (D-004). */
PS_VMX_INLINE uint32x4_t ps_vmx_vperm(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    uint8x16x2_t t;
    t.val[0] = PS_VU8(b);
    t.val[1] = PS_VU8(a);
    return PS_FROM_U8(vqtbl2q_u8(t, vbicq_u8(vdupq_n_u8(0x1f), PS_VU8(c))));
}

/* ------------------------------------------------------------------------------------------- integer add/sub */

PS_VMX_INLINE uint32x4_t ps_vmx_vaddubm(uint32x4_t a, uint32x4_t b) { return PS_FROM_U8(vaddq_u8(PS_VU8(a), PS_VU8(b))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vadduhm(uint32x4_t a, uint32x4_t b) { return PS_FROM_U16(vaddq_u16(PS_VU16(a), PS_VU16(b))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vadduwm(uint32x4_t a, uint32x4_t b) { return vaddq_u32(a, b); }
PS_VMX_INLINE uint32x4_t ps_vmx_vsububm(uint32x4_t a, uint32x4_t b) { return PS_FROM_U8(vsubq_u8(PS_VU8(a), PS_VU8(b))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vsubuhm(uint32x4_t a, uint32x4_t b) { return PS_FROM_U16(vsubq_u16(PS_VU16(a), PS_VU16(b))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vsubuwm(uint32x4_t a, uint32x4_t b) { return vsubq_u32(a, b); }

/* Saturating forms: SAT |= (wrapping result ^ saturated result), as RPCS3. */
#define PS_VMX_SATOP(name, sop, wop, cast, from)                                                                    \
    PS_VMX_INLINE uint32x4_t ps_vmx_##name(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)                             \
    {                                                                                                                \
        const uint32x4_t r = from(sop(cast(a), cast(b)));                                                          \
        ps_vmx_sat_or(sat, veorq_u32(r, from(wop(cast(a), cast(b)))));                                               \
        return r;                                                                                                    \
    }
PS_VMX_SATOP(vaddubs, vqaddq_u8, vaddq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_SATOP(vadduhs, vqaddq_u16, vaddq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_SATOP(vadduws, vqaddq_u32, vaddq_u32, , )
PS_VMX_SATOP(vaddsbs, vqaddq_s8, vaddq_s8, PS_VS8, PS_FROM_S8)
PS_VMX_SATOP(vaddshs, vqaddq_s16, vaddq_s16, PS_VS16, PS_FROM_S16)
PS_VMX_SATOP(vaddsws, vqaddq_s32, vaddq_s32, PS_VS32, PS_FROM_S32)
PS_VMX_SATOP(vsububs, vqsubq_u8, vsubq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_SATOP(vsubuhs, vqsubq_u16, vsubq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_SATOP(vsubuws, vqsubq_u32, vsubq_u32, , )
PS_VMX_SATOP(vsubsbs, vqsubq_s8, vsubq_s8, PS_VS8, PS_FROM_S8)
PS_VMX_SATOP(vsubshs, vqsubq_s16, vsubq_s16, PS_VS16, PS_FROM_S16)
PS_VMX_SATOP(vsubsws, vqsubq_s32, vsubq_s32, PS_VS32, PS_FROM_S32)
#undef PS_VMX_SATOP

/* Carry out of a + b (b > ~a); no borrow out of a - b (a >= b). */
PS_VMX_INLINE uint32x4_t ps_vmx_vaddcuw(uint32x4_t a, uint32x4_t b) { return vshrq_n_u32(vcgtq_u32(b, vmvnq_u32(a)), 31); }
PS_VMX_INLINE uint32x4_t ps_vmx_vsubcuw(uint32x4_t a, uint32x4_t b) { return vshrq_n_u32(vcgeq_u32(a, b), 31); }

/* ------------------------------------------------------------------------------------------- max, min, average */

#define PS_VMX_BINOP(name, op, cast, from) \
    PS_VMX_INLINE uint32x4_t ps_vmx_##name(uint32x4_t a, uint32x4_t b) { return from(op(cast(a), cast(b))); }
PS_VMX_BINOP(vmaxub, vmaxq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_BINOP(vmaxuh, vmaxq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_BINOP(vmaxuw, vmaxq_u32, , )
PS_VMX_BINOP(vmaxsb, vmaxq_s8, PS_VS8, PS_FROM_S8)
PS_VMX_BINOP(vmaxsh, vmaxq_s16, PS_VS16, PS_FROM_S16)
PS_VMX_BINOP(vmaxsw, vmaxq_s32, PS_VS32, PS_FROM_S32)
PS_VMX_BINOP(vminub, vminq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_BINOP(vminuh, vminq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_BINOP(vminuw, vminq_u32, , )
PS_VMX_BINOP(vminsb, vminq_s8, PS_VS8, PS_FROM_S8)
PS_VMX_BINOP(vminsh, vminq_s16, PS_VS16, PS_FROM_S16)
PS_VMX_BINOP(vminsw, vminq_s32, PS_VS32, PS_FROM_S32)
/* (a + b + 1) >> 1 without overflow (RPCS3 gv_avg*). */
PS_VMX_BINOP(vavgub, vrhaddq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_BINOP(vavguh, vrhaddq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_BINOP(vavguw, vrhaddq_u32, , )
PS_VMX_BINOP(vavgsb, vrhaddq_s8, PS_VS8, PS_FROM_S8)
PS_VMX_BINOP(vavgsh, vrhaddq_s16, PS_VS16, PS_FROM_S16)
PS_VMX_BINOP(vavgsw, vrhaddq_s32, PS_VS32, PS_FROM_S32)
/* ---- compares: all ones / all zeros per element */
PS_VMX_BINOP(vcmpequb, vceqq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_BINOP(vcmpequh, vceqq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_BINOP(vcmpequw, vceqq_u32, , )
PS_VMX_BINOP(vcmpgtub, vcgtq_u8, PS_VU8, PS_FROM_U8)
PS_VMX_BINOP(vcmpgtuh, vcgtq_u16, PS_VU16, PS_FROM_U16)
PS_VMX_BINOP(vcmpgtuw, vcgtq_u32, , )
PS_VMX_BINOP(vcmpgtsb, vcgtq_s8, PS_VS8, PS_FROM_U8)
PS_VMX_BINOP(vcmpgtsh, vcgtq_s16, PS_VS16, PS_FROM_U16)
PS_VMX_BINOP(vcmpgtsw, vcgtq_s32, PS_VS32, )
/* FP compares are ordered (false with a NaN) and do not flush denormals (RPCS3). */
PS_VMX_BINOP(vcmpeqfp, vceqq_f32, PS_VF32, )
PS_VMX_BINOP(vcmpgefp, vcgeq_f32, PS_VF32, )
PS_VMX_BINOP(vcmpgtfp, vcgtq_f32, PS_VF32, )
#undef PS_VMX_BINOP

/* vcmpbfp: bit 31 = !(a <= b), bit 30 = !(a >= -b) (both set for NaN). */
PS_VMX_INLINE uint32x4_t ps_vmx_vcmpbfp(uint32x4_t a, uint32x4_t b)
{
    const uint32x4_t gt = vmvnq_u32(vcleq_f32(PS_VF32(a), PS_VF32(b)));
    const uint32x4_t lt = vmvnq_u32(vcgeq_f32(PS_VF32(a), vnegq_f32(PS_VF32(b))));
    return vorrq_u32(vandq_u32(gt, vdupq_n_u32(0x80000000u)), vandq_u32(lt, vdupq_n_u32(0x40000000u)));
}

/* ------------------------------------------------------------------------------------------- shifts, rotates */

/* Per element: count = low bits of the element of b (b & 7, 15, 31). vshl with a negative count shifts right. */
PS_VMX_INLINE uint32x4_t ps_vmx_vslb(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_U8(vshlq_u8(PS_VU8(a), vandq_s8(PS_VS8(b), vdupq_n_s8(7))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vslh(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_U16(vshlq_u16(PS_VU16(a), vandq_s16(PS_VS16(b), vdupq_n_s16(15))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vslw(uint32x4_t a, uint32x4_t b)
{
    return vshlq_u32(a, vandq_s32(PS_VS32(b), vdupq_n_s32(31)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsrb(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_U8(vshlq_u8(PS_VU8(a), vnegq_s8(vandq_s8(PS_VS8(b), vdupq_n_s8(7)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsrh(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_U16(vshlq_u16(PS_VU16(a), vnegq_s16(vandq_s16(PS_VS16(b), vdupq_n_s16(15)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsrw(uint32x4_t a, uint32x4_t b)
{
    return vshlq_u32(a, vnegq_s32(vandq_s32(PS_VS32(b), vdupq_n_s32(31))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsrab(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S8(vshlq_s8(PS_VS8(a), vnegq_s8(vandq_s8(PS_VS8(b), vdupq_n_s8(7)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsrah(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S16(vshlq_s16(PS_VS16(a), vnegq_s16(vandq_s16(PS_VS16(b), vdupq_n_s16(15)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsraw(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S32(vshlq_s32(PS_VS32(a), vnegq_s32(vandq_s32(PS_VS32(b), vdupq_n_s32(31)))));
}
/* Rotates: (a << n) | (a >> (size - n)); a shift by -size is zero (RPCS3 gv_rol*). */
PS_VMX_INLINE uint32x4_t ps_vmx_vrlb(uint32x4_t a, uint32x4_t b)
{
    const int8x16_t n = vandq_s8(PS_VS8(b), vdupq_n_s8(7));
    return PS_FROM_U8(vorrq_u8(vshlq_u8(PS_VU8(a), n), vshlq_u8(PS_VU8(a), vsubq_s8(n, vdupq_n_s8(8)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vrlh(uint32x4_t a, uint32x4_t b)
{
    const int16x8_t n = vandq_s16(PS_VS16(b), vdupq_n_s16(15));
    return PS_FROM_U16(vorrq_u16(vshlq_u16(PS_VU16(a), n), vshlq_u16(PS_VU16(a), vsubq_s16(n, vdupq_n_s16(16)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vrlw(uint32x4_t a, uint32x4_t b)
{
    const int32x4_t n = vandq_s32(PS_VS32(b), vdupq_n_s32(31));
    return vorrq_u32(vshlq_u32(a, n), vshlq_u32(a, vsubq_s32(n, vdupq_n_s32(32))));
}

/* vsl/vsr: 128-bit shift by 0-7 bits. The count comes from each byte of b (the architecture requires them equal;
 * RPCS3 gv_fshl8/gv_fshr8 shift each byte by its own count, and so do we). */
PS_VMX_INLINE uint32x4_t ps_vmx_vsl(uint32x4_t a, uint32x4_t b)
{
    const int8x16_t n = vandq_s8(PS_VS8(b), vdupq_n_s8(7));
    const uint8x16_t lower = vextq_u8(vdupq_n_u8(0), PS_VU8(a), 15); /* host byte i-1 */
    return PS_FROM_U8(vorrq_u8(vshlq_u8(PS_VU8(a), n), vshlq_u8(lower, vsubq_s8(n, vdupq_n_s8(8)))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsr(uint32x4_t a, uint32x4_t b)
{
    const int8x16_t n = vandq_s8(PS_VS8(b), vdupq_n_s8(7));
    const uint8x16_t upper = vextq_u8(PS_VU8(a), vdupq_n_u8(0), 1); /* host byte i+1 */
    return PS_FROM_U8(vorrq_u8(vshlq_u8(PS_VU8(a), vnegq_s8(n)), vshlq_u8(upper, vsubq_s8(vdupq_n_s8(8), n))));
}
/* vslo/vsro: whole-register shift by octets, count = guest byte 15 bits 1-4 (host byte 0). */
PS_VMX_INLINE uint32x4_t ps_vmx_vslo(uint32x4_t a, uint32x4_t b)
{
    const uint8_t n = (uint8_t)((vgetq_lane_u8(PS_VU8(b), 0) >> 3) & 15);
    return PS_FROM_U8(vqtbl1q_u8(PS_VU8(a), vsubq_u8(ps_vmx_iota(), vdupq_n_u8(n))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsro(uint32x4_t a, uint32x4_t b)
{
    const uint8_t n = (uint8_t)((vgetq_lane_u8(PS_VU8(b), 0) >> 3) & 15);
    return PS_FROM_U8(vqtbl1q_u8(PS_VU8(a), vaddq_u8(ps_vmx_iota(), vdupq_n_u8(n))));
}
/* vsldoi with sh = 1..15 (a literal; sh = 0 is a move): bytes sh .. sh+15 of vA || vB. */
#define PS_VMX_VSLDOI(a, b, sh) PS_FROM_U8(vextq_u8(PS_VU8(b), PS_VU8(a), 16 - (sh)))

/* ------------------------------------------------------------------------------------------- splats */

/* uimm is a literal: guest element uimm = host lane N-1-uimm. */
#define PS_VMX_VSPLTB(b, uimm) PS_FROM_U8(vdupq_laneq_u8(PS_VU8(b), 15 - ((uimm) & 15)))
#define PS_VMX_VSPLTH(b, uimm) PS_FROM_U16(vdupq_laneq_u16(PS_VU16(b), 7 - ((uimm) & 7)))
#define PS_VMX_VSPLTW(b, uimm) vdupq_laneq_u32((b), 3 - ((uimm) & 3))
PS_VMX_INLINE uint32x4_t ps_vmx_vspltisb(int32_t simm) { return PS_FROM_S8(vdupq_n_s8((int8_t)simm)); }
PS_VMX_INLINE uint32x4_t ps_vmx_vspltish(int32_t simm) { return PS_FROM_S16(vdupq_n_s16((int16_t)simm)); }
PS_VMX_INLINE uint32x4_t ps_vmx_vspltisw(int32_t simm) { return PS_FROM_S32(vdupq_n_s32(simm)); }

/* ------------------------------------------------------------------------------------------- merges, packs, unpacks */

/* Merge high = the guest-first halves = the host-high halves: zip2(b, a) puts vA's element first in guest order. */
PS_VMX_INLINE uint32x4_t ps_vmx_vmrghb(uint32x4_t a, uint32x4_t b) { return PS_FROM_U8(vzip2q_u8(PS_VU8(b), PS_VU8(a))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vmrghh(uint32x4_t a, uint32x4_t b) { return PS_FROM_U16(vzip2q_u16(PS_VU16(b), PS_VU16(a))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vmrghw(uint32x4_t a, uint32x4_t b) { return vzip2q_u32(b, a); }
PS_VMX_INLINE uint32x4_t ps_vmx_vmrglb(uint32x4_t a, uint32x4_t b) { return PS_FROM_U8(vzip1q_u8(PS_VU8(b), PS_VU8(a))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vmrglh(uint32x4_t a, uint32x4_t b) { return PS_FROM_U16(vzip1q_u16(PS_VU16(b), PS_VU16(a))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vmrglw(uint32x4_t a, uint32x4_t b) { return vzip1q_u32(b, a); }

/* Packs: vA's elements become the guest-first half = the host-high half, so the host-low half comes from vB. */
PS_VMX_INLINE uint32x4_t ps_vmx_vpkuhum(uint32x4_t a, uint32x4_t b) { return PS_FROM_U8(vuzp1q_u8(PS_VU8(b), PS_VU8(a))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vpkuwum(uint32x4_t a, uint32x4_t b) { return PS_FROM_U16(vuzp1q_u16(PS_VU16(b), PS_VU16(a))); }

PS_VMX_INLINE uint32x4_t ps_vmx_vpkuhus(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    ps_vmx_sat_or(sat, PS_FROM_U16(vshrq_n_u16(vorrq_u16(PS_VU16(a), PS_VU16(b)), 8)));
    return PS_FROM_U8(vcombine_u8(vqmovn_u16(PS_VU16(b)), vqmovn_u16(PS_VU16(a))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vpkuwus(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    ps_vmx_sat_or(sat, vshrq_n_u32(vorrq_u32(a, b), 16));
    return PS_FROM_U16(vcombine_u16(vqmovn_u32(b), vqmovn_u32(a)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vpkshss(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const uint16x8_t bias = vdupq_n_u16(0x80);
    ps_vmx_sat_or(sat, PS_FROM_U16(vshrq_n_u16(vorrq_u16(vaddq_u16(PS_VU16(a), bias), vaddq_u16(PS_VU16(b), bias)), 8)));
    return PS_FROM_S8(vcombine_s8(vqmovn_s16(PS_VS16(b)), vqmovn_s16(PS_VS16(a))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vpkshus(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    ps_vmx_sat_or(sat, PS_FROM_U16(vshrq_n_u16(vorrq_u16(PS_VU16(a), PS_VU16(b)), 8)));
    return PS_FROM_U8(vcombine_u8(vqmovun_s16(PS_VS16(b)), vqmovun_s16(PS_VS16(a))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vpkswss(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const uint32x4_t bias = vdupq_n_u32(0x8000);
    ps_vmx_sat_or(sat, vshrq_n_u32(vorrq_u32(vaddq_u32(a, bias), vaddq_u32(b, bias)), 16));
    return PS_FROM_S16(vcombine_s16(vqmovn_s32(PS_VS32(b)), vqmovn_s32(PS_VS32(a))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vpkswus(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    ps_vmx_sat_or(sat, vshrq_n_u32(vorrq_u32(a, b), 16));
    return PS_FROM_U16(vcombine_u16(vqmovun_s32(PS_VS32(b)), vqmovun_s32(PS_VS32(a))));
}
/* vpkpx: per word, bit 24 and the top 5 bits of each colour byte -> 1:5:5:5. */
PS_VMX_INLINE uint32x4_t ps_vmx_pixel(uint32x4_t w)
{
    return vorrq_u32(vorrq_u32(vandq_u32(vshrq_n_u32(w, 9), vdupq_n_u32(0xFC00)), vandq_u32(vshrq_n_u32(w, 6), vdupq_n_u32(0x03E0))),
        vandq_u32(vshrq_n_u32(w, 3), vdupq_n_u32(0x001F)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vpkpx(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_U16(vcombine_u16(vmovn_u32(ps_vmx_pixel(b)), vmovn_u32(ps_vmx_pixel(a))));
}

/* Unpack high = the guest-first half = the host-high half. */
PS_VMX_INLINE uint32x4_t ps_vmx_vupkhsb(uint32x4_t b) { return PS_FROM_S16(vmovl_s8(vget_high_s8(PS_VS8(b)))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vupklsb(uint32x4_t b) { return PS_FROM_S16(vmovl_s8(vget_low_s8(PS_VS8(b)))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vupkhsh(uint32x4_t b) { return PS_FROM_S32(vmovl_s16(vget_high_s16(PS_VS16(b)))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vupklsh(uint32x4_t b) { return PS_FROM_S32(vmovl_s16(vget_low_s16(PS_VS16(b)))); }
/* vupk*px: 1:5:5:5 -> sign-extended bit, then 000 + each 5-bit colour (RPCS3 VUPKHPX). */
PS_VMX_INLINE uint32x4_t ps_vmx_unpixel(int32x4_t x)
{
    const uint32x4_t u = vreinterpretq_u32_s32(x);
    return vorrq_u32(vorrq_u32(vandq_u32(vshlq_n_u32(u, 6), vdupq_n_u32(0x1f0000)), vandq_u32(vshlq_n_u32(u, 3), vdupq_n_u32(0x1f00))),
        vandq_u32(u, vdupq_n_u32(0xff00001fu)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vupkhpx(uint32x4_t b) { return ps_vmx_unpixel(vmovl_s16(vget_high_s16(PS_VS16(b)))); }
PS_VMX_INLINE uint32x4_t ps_vmx_vupklpx(uint32x4_t b) { return ps_vmx_unpixel(vmovl_s16(vget_low_s16(PS_VS16(b)))); }

/* ------------------------------------------------------------------------------------------- multiplies */

/* Guest even elements are host odd lanes (the high half of each wider host lane). */
PS_VMX_INLINE uint32x4_t ps_vmx_vmuleub(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_U16(vmulq_u16(vshrq_n_u16(PS_VU16(a), 8), vshrq_n_u16(PS_VU16(b), 8)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmuloub(uint32x4_t a, uint32x4_t b)
{
    const uint16x8_t m = vdupq_n_u16(0xff);
    return PS_FROM_U16(vmulq_u16(vandq_u16(PS_VU16(a), m), vandq_u16(PS_VU16(b), m)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmulesb(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S16(vmulq_s16(vshrq_n_s16(PS_VS16(a), 8), vshrq_n_s16(PS_VS16(b), 8)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmulosb(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S16(vmulq_s16(vshrq_n_s16(vshlq_n_s16(PS_VS16(a), 8), 8), vshrq_n_s16(vshlq_n_s16(PS_VS16(b), 8), 8)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmuleuh(uint32x4_t a, uint32x4_t b) { return vmulq_u32(vshrq_n_u32(a, 16), vshrq_n_u32(b, 16)); }
PS_VMX_INLINE uint32x4_t ps_vmx_vmulouh(uint32x4_t a, uint32x4_t b)
{
    const uint32x4_t m = vdupq_n_u32(0xffff);
    return vmulq_u32(vandq_u32(a, m), vandq_u32(b, m));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmulesh(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S32(vmulq_s32(vshrq_n_s32(PS_VS32(a), 16), vshrq_n_s32(PS_VS32(b), 16)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmulosh(uint32x4_t a, uint32x4_t b)
{
    return PS_FROM_S32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(PS_VS32(a), 16), 16), vshrq_n_s32(vshlq_n_s32(PS_VS32(b), 16), 16)));
}

/* vmladduhm: a * b + c, modulo 2^16. */
PS_VMX_INLINE uint32x4_t ps_vmx_vmladduhm(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    return PS_FROM_U16(vmlaq_u16(PS_VU16(c), PS_VU16(a), PS_VU16(b)));
}

/* vmhaddshs/vmhraddshs: sat16(((a * b [+ 0x4000]) >> 15) + c). SQ(R)DMULH saturates only for -32768 * -32768;
 * the correction and the SAT rule are RPCS3's (VMHADDSHS). */
PS_VMX_INLINE uint32x4_t ps_vmx_mhadd_fix(int16x8_t m, uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t* sat)
{
    const uint16x8_t f = vcltq_s16(PS_VS16(c), vdupq_n_s16(0));
    const uint16x8_t x = vceqq_s16(vmaxq_s16(PS_VS16(a), PS_VS16(b)), vdupq_n_s16(-32768));
    const int16x8_t r = vsubq_s16(vqaddq_s16(m, PS_VS16(c)), vreinterpretq_s16_u16(vandq_u16(x, f)));
    const int16x8_t s = vaddq_s16(m, PS_VS16(c));
    const uint16x8_t ov = vorrq_u16(vbicq_u16(x, f), vbicq_u16(veorq_u16(vreinterpretq_u16_s16(s), vreinterpretq_u16_s16(r)), x));
    ps_vmx_sat_or(sat, PS_FROM_U16(ov));
    return PS_FROM_S16(r);
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmhaddshs(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t* sat)
{
    return ps_vmx_mhadd_fix(vqdmulhq_s16(PS_VS16(a), PS_VS16(b)), a, b, c, sat);
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmhraddshs(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t* sat)
{
    return ps_vmx_mhadd_fix(vqrdmulhq_s16(PS_VS16(a), PS_VS16(b)), a, b, c, sat);
}

/* ------------------------------------------------------------------------------------------- multiply-sums */

/* vmsumubm: c + the four byte products of each word (modulo 2^32). */
PS_VMX_INLINE uint32x4_t ps_vmx_vmsumubm(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
#if defined(__ARM_FEATURE_DOTPROD)
    return vdotq_u32(c, PS_VU8(a), PS_VU8(b));
#else
    const uint32x4_t l = vpaddlq_u16(vmull_u8(vget_low_u8(PS_VU8(a)), vget_low_u8(PS_VU8(b))));
    const uint32x4_t h = vpaddlq_u16(vmull_high_u8(PS_VU8(a), PS_VU8(b)));
    return vaddq_u32(c, vpaddq_u32(l, h));
#endif
}
/* vmsummbm: signed bytes of vA times unsigned bytes of vB (RPCS3 gv_dotu8s8x4(b, a, c)). */
PS_VMX_INLINE uint32x4_t ps_vmx_vmsummbm(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    const int16x8_t pl = vmulq_s16(vmovl_s8(vget_low_s8(PS_VS8(a))), vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(PS_VU8(b)))));
    const int16x8_t ph = vmulq_s16(vmovl_s8(vget_high_s8(PS_VS8(a))), vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(PS_VU8(b)))));
    return PS_FROM_S32(vaddq_s32(PS_VS32(c), vpaddq_s32(vpaddlq_s16(pl), vpaddlq_s16(ph))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmsumuhm(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    const uint32x4_t l = vmull_u16(vget_low_u16(PS_VU16(a)), vget_low_u16(PS_VU16(b)));
    const uint32x4_t h = vmull_high_u16(PS_VU16(a), PS_VU16(b));
    return vaddq_u32(c, vpaddq_u32(l, h));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vmsumshm(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    const int32x4_t l = vmull_s16(vget_low_s16(PS_VS16(a)), vget_low_s16(PS_VS16(b)));
    const int32x4_t h = vmull_high_s16(PS_VS16(a), PS_VS16(b));
    return vaddq_u32(c, PS_FROM_S32(vpaddq_s32(l, h)));
}
/* vmsumuhs: min(p0 + p1 + c, 0xFFFFFFFF), RPCS3 VMSUMUHS (two carry checks). */
PS_VMX_INLINE uint32x4_t ps_vmx_vmsumuhs(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t* sat)
{
    const uint32x4_t mask = vdupq_n_u32(0xffff);
    const uint32x4_t m1 = vmulq_u32(vandq_u32(a, mask), vandq_u32(b, mask));
    const uint32x4_t m2 = vmulq_u32(vshrq_n_u32(a, 16), vshrq_n_u32(b, 16));
    const uint32x4_t s1 = vaddq_u32(m1, m2);
    const uint32x4_t x1 = vcgtq_u32(m1, s1);
    const uint32x4_t s2 = vorrq_u32(vaddq_u32(s1, c), x1);
    const uint32x4_t x2 = vcgtq_u32(s1, s2);
    ps_vmx_sat_or(sat, vorrq_u32(x1, x2));
    return vorrq_u32(s2, x2);
}
/* vmsumshs: sat32(p0 + p1 + c), computed exactly in 64 bits. */
PS_VMX_INLINE uint32x4_t ps_vmx_vmsumshs(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t* sat)
{
    const int32x4_t l = vmull_s16(vget_low_s16(PS_VS16(a)), vget_low_s16(PS_VS16(b)));
    const int32x4_t h = vmull_high_s16(PS_VS16(a), PS_VS16(b));
    const int64x2_t sl = vaddq_s64(vpaddlq_s32(l), vmovl_s32(vget_low_s32(PS_VS32(c))));
    const int64x2_t sh = vaddq_s64(vpaddlq_s32(h), vmovl_high_s32(PS_VS32(c)));
    const int32x2_t nl = vqmovn_s64(sl), nh = vqmovn_s64(sh);
    const uint64x2_t okl = vceqq_s64(vmovl_s32(nl), sl), okh = vceqq_s64(vmovl_s32(nh), sh);
    ps_vmx_sat_or(sat, vmvnq_u32(vandq_u32(PS_FROM_U64(okl), PS_FROM_U64(okh))));
    return PS_FROM_S32(vcombine_s32(nl, nh));
}

/* ------------------------------------------------------------------------------------------- sums across */

/* vsumsws: sat32(b word 3 + all of a) in guest word 3 (host lane 0), the rest zero. */
PS_VMX_INLINE uint32x4_t ps_vmx_vsumsws(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const int64_t s = vaddlvq_s32(PS_VS32(a)) + (int64_t)vgetq_lane_s32(PS_VS32(b), 0);
    const int32_t r = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
    ps_vmx_sat_or(sat, vdupq_n_u32((uint32_t)(r != s)));
    return vsetq_lane_u32((uint32_t)r, vdupq_n_u32(0), 0);
}
/* vsum2sws: guest words 1 and 3 (host lanes 2 and 0) = sat32(a pair + b word), words 0 and 2 zero (RPCS3 ARM64). */
PS_VMX_INLINE uint32x4_t ps_vmx_vsum2sws(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const int64x2_t x = vaddl_s32(vget_low_s32(vuzp1q_s32(PS_VS32(a), PS_VS32(a))), vget_low_s32(vuzp2q_s32(PS_VS32(a), PS_VS32(a))));
    const int64x2_t y = vaddw_s32(x, vget_low_s32(vuzp1q_s32(PS_VS32(b), PS_VS32(b))));
    ps_vmx_sat_or(sat, PS_FROM_U64(vshrq_n_u64(vaddq_u64(vreinterpretq_u64_s64(y), vdupq_n_u64(0x80000000u)), 32)));
    return PS_FROM_U64(vmovl_u32(vreinterpret_u32_s32(vqmovn_s64(y))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsum4sbs(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const int32x4_t x = vpaddlq_s16(vpaddlq_s8(PS_VS8(a)));
    const int32x4_t r = vqaddq_s32(x, PS_VS32(b));
    ps_vmx_sat_or(sat, PS_FROM_S32(veorq_s32(r, vaddq_s32(x, PS_VS32(b)))));
    return PS_FROM_S32(r);
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsum4shs(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const int32x4_t x = vpaddlq_s16(PS_VS16(a));
    const int32x4_t r = vqaddq_s32(x, PS_VS32(b));
    ps_vmx_sat_or(sat, PS_FROM_S32(veorq_s32(r, vaddq_s32(x, PS_VS32(b)))));
    return PS_FROM_S32(r);
}
PS_VMX_INLINE uint32x4_t ps_vmx_vsum4ubs(uint32x4_t a, uint32x4_t b, uint32x4_t* sat)
{
    const uint32x4_t x = vpaddlq_u16(vpaddlq_u8(PS_VU8(a)));
    const uint32x4_t r = vqaddq_u32(x, b);
    ps_vmx_sat_or(sat, veorq_u32(r, vaddq_u32(x, b)));
    return r;
}

/* ------------------------------------------------------------------------------------------- floating point */

/* NJ mask (RPCS3 ppu_thread::jm_mask) and ppu_flush_denormal: x, or its sign when (x & mask) == 0. */
PS_VMX_INLINE uint32x4_t ps_vmx_jm(uint8_t nj) { return vdupq_n_u32(nj ? 0x7f800000u : 0x7fffffffu); }
PS_VMX_INLINE uint32x4_t ps_vmx_flush(uint32x4_t x, uint32x4_t jm)
{
    return vandq_u32(x, vorrq_u32(vtstq_u32(x, jm), vdupq_n_u32(0x80000000u)));
}
/* ppu_fix_vnan: a NaN result becomes the default NaN. ppu_select_vnan step: x quieted if x is a NaN, else r. */
PS_VMX_INLINE uint32x4_t ps_vmx_nan_fix(uint32x4_t r)
{
    return vbslq_u32(vceqq_f32(PS_VF32(r), PS_VF32(r)), r, vdupq_n_u32(0x7fc00000u));
}
PS_VMX_INLINE uint32x4_t ps_vmx_nan_sel(uint32x4_t x, uint32x4_t r)
{
    return vbslq_u32(vceqq_f32(PS_VF32(x), PS_VF32(x)), r, vorrq_u32(x, vdupq_n_u32(0x7fc00000u)));
}

#define PS_VMX_FP2(name, expr)                                                                                       \
    PS_VMX_INLINE uint32x4_t ps_vmx_##name(uint32x4_t a, uint32x4_t b, uint8_t nj)                                  \
    {                                                                                                                \
        const uint32x4_t m = ps_vmx_jm(nj);                                                                         \
        a = ps_vmx_flush(a, m);                                                                                      \
        b = ps_vmx_flush(b, m);                                                                                      \
        const uint32x4_t r = PS_FROM_F32(expr);                                                                      \
        return ps_vmx_flush(ps_vmx_nan_sel(a, ps_vmx_nan_sel(b, ps_vmx_nan_fix(r))), m);                             \
    }
PS_VMX_FP2(vaddfp, vaddq_f32(PS_VF32(a), PS_VF32(b)))
PS_VMX_FP2(vsubfp, vsubq_f32(PS_VF32(a), PS_VF32(b)))
#undef PS_VMX_FP2

/* vmaxfp/vminfp: inputs are not flushed, only the result (RPCS3). FMAX/FMIN order +0 above -0. */
PS_VMX_INLINE uint32x4_t ps_vmx_vmaxfp(uint32x4_t a, uint32x4_t b, uint8_t nj)
{
    const uint32x4_t r = PS_FROM_F32(vmaxq_f32(PS_VF32(a), PS_VF32(b)));
    return ps_vmx_flush(ps_vmx_nan_sel(a, ps_vmx_nan_sel(b, ps_vmx_nan_fix(r))), ps_vmx_jm(nj));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vminfp(uint32x4_t a, uint32x4_t b, uint8_t nj)
{
    const uint32x4_t r = PS_FROM_F32(vminq_f32(PS_VF32(a), PS_VF32(b)));
    return ps_vmx_flush(ps_vmx_nan_sel(a, ps_vmx_nan_sel(b, ps_vmx_nan_fix(r))), ps_vmx_jm(nj));
}

/* vmaddfp vD = vA * vC + vB, fused (one rounding); vnmsubfp vD = -(vA * vC - vB), fused. */
PS_VMX_INLINE uint32x4_t ps_vmx_vmaddfp(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint8_t nj)
{
    const uint32x4_t m = ps_vmx_jm(nj);
    a = ps_vmx_flush(a, m);
    b = ps_vmx_flush(b, m);
    c = ps_vmx_flush(c, m);
    const uint32x4_t r = PS_FROM_F32(vfmaq_f32(PS_VF32(b), PS_VF32(a), PS_VF32(c)));
    return ps_vmx_flush(ps_vmx_nan_fix(r), m);
}
PS_VMX_INLINE uint32x4_t ps_vmx_vnmsubfp(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint8_t nj)
{
    const uint32x4_t m = ps_vmx_jm(nj);
    a = ps_vmx_flush(a, m);
    b = ps_vmx_flush(b, m);
    c = ps_vmx_flush(c, m);
    const uint32x4_t r = PS_FROM_F32(vnegq_f32(vfmaq_f32(vnegq_f32(PS_VF32(b)), PS_VF32(a), PS_VF32(c))));
    return ps_vmx_flush(ps_vmx_nan_fix(r), m);
}

/* Unary: flush_in selects RPCS3's use_nj (input flush); every one flushes its result. */
#define PS_VMX_FP1(name, flush_in, expr)                                                                             \
    PS_VMX_INLINE uint32x4_t ps_vmx_##name(uint32x4_t b, uint8_t nj)                                                \
    {                                                                                                                \
        const uint32x4_t m = ps_vmx_jm(nj);                                                                         \
        if (flush_in) b = ps_vmx_flush(b, m);                                                                        \
        const uint32x4_t r = PS_FROM_F32(expr);                                                                      \
        return ps_vmx_flush(ps_vmx_nan_sel(b, ps_vmx_nan_fix(r)), m);                                                \
    }
PS_VMX_FP1(vrefp, 1, vdivq_f32(vdupq_n_f32(1.0f), PS_VF32(b)))
PS_VMX_FP1(vrsqrtefp, 1, vdivq_f32(vdupq_n_f32(1.0f), vsqrtq_f32(PS_VF32(b))))
PS_VMX_FP1(vrfim, 1, vrndmq_f32(PS_VF32(b)))
PS_VMX_FP1(vrfip, 1, vrndpq_f32(PS_VF32(b)))
PS_VMX_FP1(vrfin, 0, vrndnq_f32(PS_VF32(b)))
PS_VMX_FP1(vrfiz, 0, vrndq_f32(PS_VF32(b)))
#undef PS_VMX_FP1

/* vexptefp/vlogefp: exp2f/log2f per element (host libm, like the interpreter), NaN results -> default NaN. */
PS_VMX_INLINE uint32x4_t ps_vmx_vexptefp(uint32x4_t b)
{
    float f[4];
    vst1q_f32(f, PS_VF32(b));
    for (int i = 0; i < 4; i++) f[i] = exp2f(f[i]);
    return ps_vmx_nan_fix(PS_FROM_F32(vld1q_f32(f)));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vlogefp(uint32x4_t b)
{
    float f[4];
    vst1q_f32(f, PS_VF32(b));
    for (int i = 0; i < 4; i++) f[i] = log2f(f[i]);
    return ps_vmx_nan_fix(PS_FROM_F32(vld1q_f32(f)));
}

/* vcfsx/vcfux: convert (round to nearest), then scale by 2^-uimm (exact: RPCS3 subtracts uimm from the exponent). */
PS_VMX_INLINE uint32x4_t ps_vmx_vcfsx(uint32x4_t b, uint32_t uimm)
{
    return PS_FROM_F32(vmulq_n_f32(vcvtq_f32_s32(PS_VS32(b)), 1.0f / (float)(1u << (uimm & 31))));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vcfux(uint32x4_t b, uint32_t uimm)
{
    return PS_FROM_F32(vmulq_n_f32(vcvtq_f32_u32(b), 1.0f / (float)(1u << (uimm & 31))));
}
/* vctsxs/vctuxs: scale by 2^uimm, truncate, saturate (SAT on out-of-range, not on NaN; NaN converts to 0).
 * FCVTZS/FCVTZU saturate and return 0 for NaN, which is exactly RPCS3's result. */
PS_VMX_INLINE uint32x4_t ps_vmx_vctsxs(uint32x4_t b, uint32_t uimm, uint32x4_t* sat)
{
    const float32x4_t r = vmulq_f32(PS_VF32(b), vreinterpretq_f32_u32(vdupq_n_u32(((uimm & 31) + 127u) << 23)));
    ps_vmx_sat_or(sat, vorrq_u32(vcltq_f32(r, vdupq_n_f32(-2147483648.0f)), vcgeq_f32(r, vdupq_n_f32(2147483648.0f))));
    return PS_FROM_S32(vcvtq_s32_f32(r));
}
PS_VMX_INLINE uint32x4_t ps_vmx_vctuxs(uint32x4_t b, uint32_t uimm, uint32x4_t* sat)
{
    const float32x4_t r = vmulq_f32(PS_VF32(b), vreinterpretq_f32_u32(vdupq_n_u32(((uimm & 31) + 127u) << 23)));
    ps_vmx_sat_or(sat, vorrq_u32(vcltq_f32(r, vdupq_n_f32(0.0f)), vcgeq_f32(r, vdupq_n_f32(4294967296.0f))));
    return vcvtq_u32_f32(r);
}

#undef PS_VMX_INLINE

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PPU_VMX_H */
