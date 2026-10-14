/*
 * The sound mixer's voice job (spu_bb12970d56fb7b8a: "PCMSource", "SrcDownsampleBuf", "Mp3InputBuffer"), natively
 * (D-982). The contract and the approach as rage_audio.c's.
 */
#include "rage_audio.h"

#include <string.h>

#include "neon.h"
#include "pspocket/core/be.h"
#include "pspocket/spu/lifted.h"

static inline uint32_t pref(const SPUContext* ctx, int r) { return ctx->gpr[r].u32[3]; }
/* A register's words in guest order (word k in lane k), and back. */
static inline ps_spu_vec reg_g(const SPUContext* ctx, int r) { return (ps_spu_vec)psn_rev4(vld1q_u32(ctx->gpr[r].u32)); }
static inline void set_reg_g(SPUContext* ctx, int r, ps_spu_vec v) { vst1q_u32(ctx->gpr[r].u32, psn_rev4((uint32x4_t)v)); }
static inline int reg_is(const SPUContext* ctx, int r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    return ctx->gpr[r].u32[3] == w0 && ctx->gpr[r].u32[2] == w1 && ctx->gpr[r].u32[1] == w2 && ctx->gpr[r].u32[0] == w3;
}

/* ===================================================================================================================
 * 0x08000 to 0x080D8: the voice's sample-rate converter, its loop (a region: entered at its head, left at its exit,
 * D-966). Four outputs an iteration from int16 samples by linear interpolation. Its state: $80 the four outputs'
 * positions as floats (lane k one step after lane k - 1), $84 four steps, $10 the position as 16.48 fixed point (words 0
 * and 1), $82 four steps of it, $83 the samples, $11 the output, $12 the outputs so far and $85 the count (it runs while
 * count > outputs, at least once). Per iteration:
 *   T = P with its fraction's bits cleared (|P| < 1: +0); frac = fs(P, T); d_k = cflts(fs(T_k, T_0), 171) (4 per sample)
 *   the 16 bytes at $83 + 2 * (pos >> 48) as eight samples s (csflt 140); lane k takes s[(d_k + j) / 4] for its two
 *   neighbours through the job's controls (word adds of 0x00010203 and 0x04050607 to d_k's low byte in every byte,
 *   each byte & 31, as shuffle indexes over the eight samples' bytes); out_k = fma(fs(s1, s0), frac_k, s0)
 *   P = fa(P, step); pos += step (64 bits); the 16-byte output stored.
 * The registers the region uses as constants ($13-$18, $87, $88, set before it) must be the job's: else declined.
 * After the exit the job reloads $80-$89 and $126 and returns; live there (spu-live): $1, $79, $90-$107, $112-$125,
 * $127, none of which the loop writes. $80 is left as the loop leaves it all the same (the contract keeps it exact).
 * =================================================================================================================== */
int ps_gta5_bb12_08000(SPUContext* ctx, uint8_t* ls)
{
    if (!reg_is(ctx, 13, 0x007FFFFFu, 0x007FFFFFu, 0x007FFFFFu, 0x007FFFFFu) ||
        !reg_is(ctx, 14, 0x03030303u, 0x07070707u, 0x0B0B0B0Bu, 0x0F0F0F0Fu) ||
        !reg_is(ctx, 15, 0x00000809u, 0x00000A0Bu, 0x00000C0Du, 0x00000E0Fu) ||
        !reg_is(ctx, 16, 0x00000001u, 0x00000203u, 0x00000405u, 0x00000607u) ||
        !reg_is(ctx, 17, 0x04050607u, 0x80808080u, 0x0C0D0E0Fu, 0x80808080u) ||
        !reg_is(ctx, 18, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u) ||
        !reg_is(ctx, 87, 0x04050607u, 0x04050607u, 0x04050607u, 0x04050607u) ||
        !reg_is(ctx, 88, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u))
        return 0;
    const uint32_t src = pref(ctx, 83), count = pref(ctx, 85);
    uint32_t out = pref(ctx, 11), done = pref(ctx, 12);
    /* Outputs over the count, as one run: stores that wrap past local store's end or overrun the samples' reach are
     * left to the lifted code (the samples are read as they are when each iteration runs). */
    const uint32_t iters = count > done + 4 ? (count - done + 3) / 4 : 1;
    if (iters > 4096 || (out & PS_SPU_QW_MASK) + 16 * iters > PS_SPU_LS_SIZE) return 0;
    uint32_t pos_hi = ctx->gpr[10].u32[3], pos_lo = ctx->gpr[10].u32[2];
    const uint32_t step_hi = ctx->gpr[82].u32[3], step_lo = ctx->gpr[82].u32[2];
    ps_spu_vec P = reg_g(ctx, 80);
    const ps_spu_vec step = reg_g(ctx, 84);
    /* Every iteration's 32 bytes of samples (the quadword at the address and the next) must not be a later
     * iteration's output: the job reads them after its earlier stores, so an overlap would be order dependent. */
    {
        uint32_t ph = pos_hi, pl = pos_lo;
        const uint32_t out_lo = out & PS_SPU_QW_MASK, out_hi = out_lo + 16 * iters;
        for (uint32_t t = 0; t < iters; t++) {
            const uint32_t a = (2u * (ph >> 16) + src) & PS_SPU_QW_MASK, b = (a + 16) & PS_SPU_QW_MASK;
            if ((a < out_hi && out_lo < a + 16) || (b < out_hi && out_lo < b + 16)) return 0;
            const uint32_t lo = pl + step_lo;
            ph += step_hi + (lo < pl);
            pl = lo;
        }
    }
    const uint32x4_t add0 = vdupq_n_u32(0x00010203u), add1 = vdupq_n_u32(0x04050607u), b31 = vdupq_n_u8(31);
    do {
        const uint32x4_t p = (uint32x4_t)P;
        /* T: the fraction's bits of each lane cleared, as the job masks them */
        const int32x4_t e = vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(p, 23), vdupq_n_u32(255)));
        const int32x4_t sh = vsubq_s32(e, vdupq_n_s32(127)); /* the right shift of 0x7FFFFF, mod 64 */
        const uint32x4_t amount = vandq_u32(vreinterpretq_u32_s32(sh), vdupq_n_u32(63));
        const uint32x4_t frac_bits = vbicq_u32(vshlq_u32(vdupq_n_u32(0x007FFFFFu), vnegq_s32(vreinterpretq_s32_u32(vminq_u32(amount, vdupq_n_u32(32))))),
            vcgeq_u32(amount, vdupq_n_u32(32)));
        const uint32x4_t below158 = vcltq_s32(e, vdupq_n_s32(158));
        const uint32x4_t below127 = vcltq_s32(e, vdupq_n_s32(127));
        const uint32x4_t mask = vorrq_u32(vandq_u32(frac_bits, below158), below127);
        const ps_spu_vec T = (ps_spu_vec)vbicq_u32(p, mask);
        const ps_spu_vec frac = psl_fs(P, T);
        const ps_spu_vec d = psl_cflts(psl_fs(T, (ps_spu_vec)vdupq_n_u32(vgetq_lane_u32((uint32x4_t)T, 0))), 171);
        /* the 16 bytes of samples */
        const uint32_t a = 2u * (pos_hi >> 16) + src, o = a & 15;
        uint8_t win[32];
        memcpy(win, ls + (a & PS_SPU_QW_MASK), 16);
        memcpy(win + 16, ls + ((a + 16) & PS_SPU_QW_MASK), 16);
        const int16x8_t h = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(win + o)));
        const float32x4_t f03 = vcvtq_n_f32_s32(vmovl_s16(vget_low_s16(h)), 15);
        const float32x4_t f47 = vcvtq_n_f32_s32(vmovl_s16(vget_high_s16(h)), 15);
        /* the eight samples' bytes as the job's shuffle sees them (big-endian words), and each lane's two controls */
        const uint8x16x2_t tbl = {{vrev32q_u8(vreinterpretq_u8_f32(f03)), vrev32q_u8(vreinterpretq_u8_f32(f47))}};
        const uint32x4_t low = vmulq_u32(vandq_u32((uint32x4_t)d, vdupq_n_u32(255)), vdupq_n_u32(0x01010101u));
        const uint8x16_t c0 = vandq_u8(vrev32q_u8(vreinterpretq_u8_u32(vaddq_u32(low, add0))), b31);
        const uint8x16_t c1 = vandq_u8(vrev32q_u8(vreinterpretq_u8_u32(vaddq_u32(low, add1))), b31);
        const ps_spu_vec s0 = (ps_spu_vec)vreinterpretq_u32_u8(vrev32q_u8(vqtbl2q_u8(tbl, c0)));
        const ps_spu_vec s1 = (ps_spu_vec)vreinterpretq_u32_u8(vrev32q_u8(vqtbl2q_u8(tbl, c1)));
        const ps_spu_vec y = psl_fma(psl_fs(s1, s0), frac, s0);
        vst1q_u8(ls + (out & PS_SPU_QW_MASK), vrev32q_u8(vreinterpretq_u8_u32((uint32x4_t)y)));
        out += 16;
        P = psl_fa(P, step);
        const uint32_t lo = pos_lo + step_lo;
        pos_hi += step_hi + (lo < pos_lo);
        pos_lo = lo;
        done += 4;
    } while (count > done);
    set_reg_g(ctx, 80, P);
    ctx->pc = (0x080D8u + ctx->reloc) & PS_SPU_PC_MASK;
    return 1;
}
