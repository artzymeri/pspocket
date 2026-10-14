/*
 * The sound mixer's submix job (spu_28281614459ce75e: "Submixes", "BiquadFilter", "BufferConversion",
 * "R360OutputBuffer"), natively (D-980). The contract and the approach as rage_audio.c's: every value the job leaves in
 * local store, bit for bit, each SPU operation through the lifted code's own helpers (pspocket/spu/lifted.h) in the
 * job's order. Vectors here are in guest order (word k in lane k): the helpers used are lane-wise.
 */
#include "rage_audio.h"

#include <string.h>

#include "neon.h"
#include "pspocket/core/be.h"
#include "pspocket/spu/lifted.h"

static inline uint32_t pref(const SPUContext* ctx, int r) { return ctx->gpr[r].u32[3]; }

/* A quadword of local store in guest order (word k in lane k) and back. */
static inline ps_spu_vec ldq(const uint8_t* ls, uint32_t addr)
{
    return (ps_spu_vec)vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + (addr & PS_SPU_QW_MASK))));
}
static inline void stq(uint8_t* ls, uint32_t addr, ps_spu_vec v)
{
    vst1q_u8(ls + (addr & PS_SPU_QW_MASK), vrev32q_u8(vreinterpretq_u8_u32((uint32x4_t)v)));
}
/* The word at a word-aligned address, as the job reads it (a quadword rotated to it), and its write (an insert). */
static inline uint32_t ldw(const uint8_t* ls, uint32_t addr) { return ps_ld32(ls + (addr & 0x3FFFCu)); }
static inline void stw(uint8_t* ls, uint32_t addr, uint32_t v) { ps_st32(ls + (addr & 0x3FFFCu), v); }

/* The job's signed remainder (its shift-and-subtract division of magnitudes, the dividend's sign given back). */
static inline uint32_t srem(uint32_t a, uint32_t b)
{
    const uint32_t ua = (int32_t)a < 0 ? 0u - a : a, ub = (int32_t)b < 0 ? 0u - b : b;
    const uint32_t r = ua % ub;
    return (int32_t)a < 0 ? 0u - r : r;
}

/* Overlap of [a, a + an) and [b, b + bn). */
static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

/* The job's shuffle controls the code relies on: halfwords 0-3 and 4-7 to sign-extendable words, and the low halfwords
 * of two vectors' words packed. */
typedef struct {
    uint32_t addr;
    uint32_t w[4];
} img_const;
static const img_const k_2828_reverb[] = {
    {0x189F0, {0x00000001u, 0x00000203u, 0x00000405u, 0x00000607u}},
    {0x18A00, {0x00000809u, 0x00000A0Bu, 0x00000C0Du, 0x00000E0Fu}},
    {0x18A10, {0x02030607u, 0x0A0B0E0Fu, 0x12131617u, 0x1A1B1E1Fu}},
};

static int constants_hold(const uint8_t* ls, uint32_t rel, const img_const* k, int n)
{
    for (int i = 0; i < n; i++)
        for (int w = 0; w < 4; w++)
            if (ldw(ls, k[i].addr + rel + 4 * (uint32_t)w) != k[i].w[w]) return 0;
    return 1;
}

/* ===================================================================================================================
 * 0x0A920: the submix's reverb (RAGE's "Small/Medium/Large Reverb"), N = $6 / 2 frames of two quadwords (four lanes
 * each, "lo" and "hi") from $4 to $5, the reverb's context at $3. Per frame:
 *  - four all-pass sections in series (records at ctx + 2368 + 64 k: g at +0, c at +16, a counter at +36, the delay
 *    line's base at +44, its position at +52, its length at +56): the delay line's slot (base + 16 pos) holds both
 *    halves as eight int16 (lo in halfwords 0-3); d = csflt(slot half, 140); y = fma(x, fm(g, -1), fm(d, c)); the slot
 *    gets clamp16(cflts(fma(y, g, x), 158)) of both halves, pos = (pos + 1) % len, the counter + 1; x := y.
 *  - three banks of six delay lines (records at ctx + 1504, 1792, 2080 + 48 i: gain g at +0, counter +20, base +28,
 *    position +36, length +40): the taps sum(fm-free fma(half(slot), g)) per bank and half, in i's order; three mixes of
 *    y and the taps (each bank's mix leaves its own taps out) packed as int16 into each bank's lines.
 *  - for each i: each bank's line read again and written with its mix (positions and counters stepped); bank 2's and
 *    bank 3's taps times their gains through a filter each (states at ctx + 544 + 80 i and ctx + 1024 + 80 i: b +0,
 *    a +16, p +32, o +48, z +64), run four times on the lo tap, then four times on the hi tap: p = fma(a, z, p);
 *    o = fma(b, z, fs(t, p)); z = fma(a, o, z). The outputs: per half, fma chains of bank 1's taps (times ctx + 16),
 *    the filters' last p (times ctx + 32 and ctx + 48), in i's order.
 * The twelve filters of a frame are independent: they run side by side here (the job runs them one after the other).
 * Nothing in registers is read after it (spu-live at its four call sites). Declined: a delay line's slot inside the
 * context, the input or the output (or the job's controls) in any frame, a length 0 (the job halts), input or output
 * inside the context, a context not on a quadword boundary or not inside local store.
 * =================================================================================================================== */

#define CTX_SIZE 0xA40u
#define AP_LINES 4u
#define BANK_LINES 6u
#define MAX_FRAMES 256u

typedef struct {
    ps_spu_vec b, a, p, o, z;
} svf;

static inline ps_spu_vec clamp16(ps_spu_vec v)
{
    const ps_spu_vec hi = psl_splat32(32767u), lo = psl_splat32(0xFFFF8000u);
    const ps_spu_vec m = psl_bsel((ps_spu_vec)((psv_s32)v > (psv_s32)hi), v, hi);
    return psl_bsel((ps_spu_vec)((psv_s32)v > (psv_s32)lo), lo, m);
}

/* A slot's halves as floats (xshw then csflt 140: an int16 times 2^-15, exact in any rounding). */
static inline void unpack(const uint8_t* ls, uint32_t slot, ps_spu_vec* lo, ps_spu_vec* hi)
{
    const int16x8_t h = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ls + slot)));
    *lo = (ps_spu_vec)vreinterpretq_u32_f32(vcvtq_n_f32_s32(vmovl_s16(vget_low_s16(h)), 15));
    *hi = (ps_spu_vec)vreinterpretq_u32_f32(vcvtq_n_f32_s32(vmovl_s16(vget_high_s16(h)), 15));
}
/* Two vectors' clamped cflts 158 as a slot (lo in halfwords 0-3). */
static inline void pack(uint8_t* ls, uint32_t slot, ps_spu_vec lo, ps_spu_vec hi)
{
    const int16x8_t h = vcombine_s16(vmovn_s32(vreinterpretq_s32_u32((uint32x4_t)clamp16(psl_cflts(lo, 158)))),
        vmovn_s32(vreinterpretq_s32_u32((uint32x4_t)clamp16(psl_cflts(hi, 158)))));
    vst1q_u8(ls + slot, vrev16q_u8(vreinterpretq_u8_s16(h)));
}

/* A delay line's record: where its slot is now, and its step. */
static inline uint32_t slot_of(const uint8_t* ls, uint32_t base_at, uint32_t pos_at)
{
    return (ldw(ls, base_at) + 16u * ldw(ls, pos_at)) & PS_SPU_QW_MASK;
}
static inline void step_line(uint8_t* ls, uint32_t pos_at, uint32_t count_at, uint32_t len_at)
{
    stw(ls, pos_at, srem(ldw(ls, pos_at) + 1u, ldw(ls, len_at)));
    stw(ls, count_at, ldw(ls, count_at) + 1u);
}

/* The records of every line (base, position, length): all-pass lines first, then banks 1-3 by i. */
static void line_records(uint32_t ctx, uint32_t base[22], uint32_t pos[22], uint32_t len[22], uint32_t count[22])
{
    for (uint32_t k = 0; k < AP_LINES; k++) {
        const uint32_t r = ctx + 2368 + 64 * k;
        base[k] = r + 44, pos[k] = r + 52, len[k] = r + 56, count[k] = r + 36;
    }
    for (uint32_t i = 0; i < BANK_LINES; i++)
        for (uint32_t t = 0; t < 3; t++) {
            const uint32_t r = ctx + 1504 + 288 * t + 48 * i, n = AP_LINES + 3 * i + t;
            base[n] = r + 28, pos[n] = r + 36, len[n] = r + 40, count[n] = r + 20;
        }
}

int ps_gta5_2828_0a920(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc;
    const uint32_t frames = pref(ctx, 6) >> 1;
    if (frames == 0) {
        ctx->pc = pref(ctx, 0) & PS_SPU_PC_MASK;
        return 1;
    }
    const uint32_t cx = pref(ctx, 3), in = pref(ctx, 4), out = pref(ctx, 5);
    if ((cx & 15) || cx >= PS_SPU_LS_SIZE || CTX_SIZE > PS_SPU_LS_SIZE - cx || frames > MAX_FRAMES) return 0;
    if ((in & 15) || (out & 15) || in >= PS_SPU_LS_SIZE || out >= PS_SPU_LS_SIZE || 32 * frames > PS_SPU_LS_SIZE - in ||
        32 * frames > PS_SPU_LS_SIZE - out)
        return 0;
    if (overlaps(in, 32 * frames, cx, CTX_SIZE) || overlaps(out, 32 * frames, cx, CTX_SIZE)) return 0;
    if (!constants_hold(ls, rel, k_2828_reverb, 3)) return 0;

    /* Every slot every frame will use, before anything changes. */
    uint32_t base_at[22], pos_at[22], len_at[22], count_at[22];
    line_records(cx, base_at, pos_at, len_at, count_at);
    for (uint32_t n = 0; n < 22; n++) {
        const uint32_t base = ldw(ls, base_at[n]), len = ldw(ls, len_at[n]);
        uint32_t pos = ldw(ls, pos_at[n]);
        if (len == 0) return 0;
        for (uint32_t f = 0; f < frames; f++) {
            const uint32_t slot = (base + 16u * pos) & PS_SPU_QW_MASK;
            if (overlaps(slot, 16, cx, CTX_SIZE) || overlaps(slot, 16, in, 32 * frames) || overlaps(slot, 16, out, 32 * frames))
                return 0;
            for (int k = 0; k < 3; k++)
                if (overlaps(slot, 16, (k_2828_reverb[k].addr + rel) & PS_SPU_QW_MASK, 16)) return 0;
            pos = srem(pos + 1u, len);
        }
    }

    const ps_spu_vec minus1 = psl_splat32(0xBF800000u);
    for (uint32_t f = 0; f < frames; f++) {
        const ps_spu_vec c10 = ldq(ls, cx + 0x10), c20 = ldq(ls, cx + 0x20), c30 = ldq(ls, cx + 0x30);
        ps_spu_vec x0 = ldq(ls, in + 32 * f), x1 = ldq(ls, in + 32 * f + 16), y0 = x0, y1 = x1;

        /* The all-pass sections. */
        for (uint32_t k = 0; k < AP_LINES; k++) {
            const uint32_t r = cx + 2368 + 64 * k;
            const ps_spu_vec g = ldq(ls, r), c = ldq(ls, r + 16), ng = psl_fm(g, minus1);
            const uint32_t slot = slot_of(ls, base_at[k], pos_at[k]);
            ps_spu_vec d0, d1;
            unpack(ls, slot, &d0, &d1);
            y1 = psl_fma(x1, ng, psl_fm(d1, c));
            y0 = psl_fma(x0, ng, psl_fm(d0, c));
            pack(ls, slot, psl_fma(y0, g, x0), psl_fma(y1, g, x1));
            step_line(ls, pos_at[k], count_at[k], len_at[k]);
            x0 = y0, x1 = y1;
        }

        /* The banks' taps (as they are before this frame writes them). */
        ps_spu_vec tap_lo[3] = {psl_splat32(0), psl_splat32(0), psl_splat32(0)};
        ps_spu_vec tap_hi[3] = {psl_splat32(0), psl_splat32(0), psl_splat32(0)};
        for (uint32_t i = 0; i < BANK_LINES; i++)
            for (uint32_t t = 0; t < 3; t++) {
                const uint32_t n = AP_LINES + 3 * i + t;
                const ps_spu_vec g = ldq(ls, cx + 1504 + 288 * t + 48 * i);
                ps_spu_vec dl, dh;
                unpack(ls, slot_of(ls, base_at[n], pos_at[n]), &dl, &dh);
                tap_lo[t] = psl_fma(dl, g, tap_lo[t]);
                tap_hi[t] = psl_fma(dh, g, tap_hi[t]);
            }
        const ps_spu_vec lo1 = psl_fa(y0, tap_lo[0]), hi1 = psl_fa(y1, tap_hi[0]);
        const ps_spu_vec mix_lo[3] = {psl_fa(psl_fa(y0, tap_lo[1]), tap_lo[2]), psl_fa(lo1, tap_lo[2]), psl_fa(lo1, tap_lo[1])};
        const ps_spu_vec mix_hi[3] = {psl_fa(psl_fa(y1, tap_hi[1]), tap_hi[2]), psl_fa(hi1, tap_hi[2]), psl_fa(hi1, tap_hi[1])};

        /* Each line read again and written with its bank's mix; the filters' inputs and states. */
        ps_spu_vec t1lo[BANK_LINES], t1hi[BANK_LINES], tin_lo[2][BANK_LINES], tin_hi[2][BANK_LINES];
        svf st[2][BANK_LINES];
        for (uint32_t i = 0; i < BANK_LINES; i++)
            for (uint32_t t = 0; t < 3; t++) {
                const uint32_t n = AP_LINES + 3 * i + t;
                const ps_spu_vec g = ldq(ls, cx + 1504 + 288 * t + 48 * i);
                const uint32_t slot = slot_of(ls, base_at[n], pos_at[n]);
                ps_spu_vec dl, dh;
                unpack(ls, slot, &dl, &dh);
                pack(ls, slot, mix_lo[t], mix_hi[t]);
                step_line(ls, pos_at[n], count_at[n], len_at[n]);
                if (t == 0) {
                    t1lo[i] = psl_fm(dl, g), t1hi[i] = psl_fm(dh, g);
                } else {
                    tin_lo[t - 1][i] = psl_fm(dl, g), tin_hi[t - 1][i] = psl_fm(dh, g);
                    const uint32_t s = cx + (t == 1 ? 544 : 1024) + 80 * i;
                    svf* v = &st[t - 1][i];
                    v->b = ldq(ls, s), v->a = ldq(ls, s + 0x10), v->p = ldq(ls, s + 0x20), v->z = ldq(ls, s + 0x40);
                }
            }

        /* The twelve filters, side by side: four steps on the lo tap (its last p kept), four on the hi tap. */
        ps_spu_vec p_lo[2][BANK_LINES];
        for (uint32_t h = 0; h < 2; h++) {
            for (uint32_t step = 0; step < 4; step++)
                for (uint32_t b = 0; b < 2; b++)
                    for (uint32_t i = 0; i < BANK_LINES; i++) {
                        svf* v = &st[b][i];
                        v->p = psl_fma(v->a, v->z, v->p);
                        v->o = psl_fma(v->b, v->z, psl_fs(h ? tin_hi[b][i] : tin_lo[b][i], v->p));
                        v->z = psl_fma(v->a, v->o, v->z);
                    }
            if (h == 0)
                for (uint32_t b = 0; b < 2; b++)
                    for (uint32_t i = 0; i < BANK_LINES; i++) p_lo[b][i] = st[b][i].p;
        }

        /* The outputs, in the job's order, and the filters' states. */
        ps_spu_vec acc_lo = psl_splat32(0), acc_hi = psl_splat32(0);
        for (uint32_t i = 0; i < BANK_LINES; i++) {
            acc_hi = psl_fma(t1hi[i], c10, acc_hi);
            acc_lo = psl_fma(t1lo[i], c10, acc_lo);
            acc_hi = psl_fma(st[0][i].p, c20, acc_hi);
            acc_lo = psl_fma(p_lo[0][i], c20, acc_lo);
            acc_hi = psl_fma(st[1][i].p, c30, acc_hi);
            acc_lo = psl_fma(p_lo[1][i], c30, acc_lo);
            for (uint32_t b = 0; b < 2; b++) {
                const uint32_t s = cx + (b == 0 ? 544 : 1024) + 80 * i;
                stq(ls, s + 0x20, st[b][i].p);
                stq(ls, s + 0x30, st[b][i].o);
                stq(ls, s + 0x40, st[b][i].z);
            }
        }
        stq(ls, out + 32 * f, acc_lo);
        stq(ls, out + 32 * f + 16, acc_hi);
    }
    ctx->pc = pref(ctx, 0) & PS_SPU_PC_MASK;
    return 1;
}
