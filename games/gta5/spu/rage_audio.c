/*
 * GTA V's RAGE audio jobs, natively (D-979 to D-983): the functions of the sound mixer's jobs where the mixer (SPURS
 * "Secondary") spends its time, each doing what the job's function does to local store and to the registers of the
 * contract (pspocket/spu/replace.hpp, D-965), bit for bit: every floating-point operation the SPU's as the lifted code
 * models it (fp.h: round toward zero, denormals flushed, a zero factor in fm and fma, the clamp of fs, fms and fnms), in
 * the job's order, so the sound is exactly what the translated job makes. Not a transcription of the compiled code: one
 * sample at a time as the data is, the quadword loads, rotates and inserts of each sample left out. Whatever an
 * implementation does not handle it declines (returns 0 having changed nothing) and the lifted function runs;
 * tools/spu-native-check checks both against calls captured from the game.
 */
#include "rage_audio.h"

#include <math.h>
#include <string.h>

#include "neon.h"
#include "pspocket/core/be.h"
#include "pspocket/spu/lifted.h"

/* ---- the SPU's single-precision operations on one value (pspocket/spu/lifted.h's psl_f*, one lane) ---- */

#define EXPO 0x7F800000u

static inline uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline float ffrom(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}
/* A zero of the SPU: exponent 0 (zeros and denormals). */
static inline int s_zc(float f) { return (fbits(f) & EXPO) == 0; }
/* Exponent 255 (beyond the host's range) becomes the largest number of its sign (psl_clamp). */
static inline float s_clamp(float f)
{
    const uint32_t u = fbits(f);
    return (u & EXPO) == EXPO ? ffrom((u & 0x80000000u) | 0x7F7FFFFFu) : f;
}
static inline float s_fa(float a, float b) { return a + b; }
static inline float s_fs(float a, float b) { return a - s_clamp(b); }
static inline float s_fm(float a, float b) { return (s_zc(a) || s_zc(b)) ? 0.0f : a * b; }
static inline float s_fma(float a, float b, float c) { return (s_zc(a) || s_zc(b)) ? c : fmaf(a, b, c); }
static inline float s_fms(float a, float b, float c) { return fmaf(s_clamp(a), s_clamp(b), -c); }
static inline float s_fnms(float a, float b, float c) { return fmaf(-s_clamp(a), s_clamp(b), c); }

/* ---- the same on four lanes (NEON, guest word i in lane i; neon.h's blocks) ---- */

static inline uint32x4_t v_clamp(uint32x4_t x)
{
    const uint32x4_t e255 = vceqq_u32(vandq_u32(x, vdupq_n_u32(EXPO)), vdupq_n_u32(EXPO));
    return vbslq_u32(e255, vorrq_u32(vandq_u32(x, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7F7FFFFFu)), x);
}
static inline uint32x4_t v_fa(uint32x4_t a, uint32x4_t b)
{
    return vreinterpretq_u32_f32(vaddq_f32(vreinterpretq_f32_u32(a), vreinterpretq_f32_u32(b)));
}
/* a * b - c with a and b clamped already (psl_fms) */
static inline uint32x4_t v_fms_c(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{
    return vreinterpretq_u32_f32(vfmaq_f32(vnegq_f32(vreinterpretq_f32_u32(c)), vreinterpretq_f32_u32(a), vreinterpretq_f32_u32(b)));
}
/* Big-endian quadword of local store to guest-order lanes and back. */
static inline uint32x4_t v_ldq(const uint8_t* ls, uint32_t addr)
{
    return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + (addr & PS_SPU_QW_MASK))));
}
/* Guest-order lanes to a register (D-004) and back. */
static inline void v_st_reg(SPUContext* ctx, int r, uint32x4_t v) { vst1q_u32(ctx->gpr[r].u32, psn_rev4(v)); }
static inline uint32x4_t v_ld_reg(const SPUContext* ctx, int r) { return psn_rev4(vld1q_u32(ctx->gpr[r].u32)); }

/* ---- registers and local store ---- */

/* The preferred word of a register (D-004: host lane 3 is guest word 0). */
static inline uint32_t pref(const SPUContext* ctx, int r) { return ctx->gpr[r].u32[3]; }
static inline float preff(const SPUContext* ctx, int r) { return ffrom(pref(ctx, r)); }
static inline void ret(SPUContext* ctx) { ctx->pc = pref(ctx, 0) & PS_SPU_PC_MASK; }

/* Quadword constants of the job's image (its shuffle controls) at the program's addresses: the native code does what
 * the job does with them, so it checks they are still these. */
typedef struct {
    uint32_t addr;
    uint32_t w[4]; /* guest words */
} img_const;

static int constants_hold(const uint8_t* ls, uint32_t rel, const img_const* k, int n)
{
    for (int i = 0; i < n; i++) {
        const uint8_t* q = ls + ((k[i].addr + rel) & PS_SPU_QW_MASK);
        for (int w = 0; w < 4; w++)
            if (ps_ld32(q + 4 * w) != k[i].w[w]) return 0;
    }
    return 1;
}

/* [start, start + size) inside local store, without wrapping. */
static inline int in_ls(uint32_t start, uint32_t size) { return start < PS_SPU_LS_SIZE && size <= PS_SPU_LS_SIZE - start; }

/* ===================================================================================================================
 * The synthesizer's job (spu_012bf13511b9f1b0): its biquad filter, in transposed direct form II, over a buffer of
 * floats in place.
 * =================================================================================================================== */

/* The job's controls that put a value into one word of a quadword, the other words from another (word 0 to 3). */
static const img_const k_012b_words[] = {
    {0x15900, {0x00010203u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu}},
    {0x158F0, {0x10111213u, 0x04050607u, 0x18191A1Bu, 0x1C1D1E1Fu}},
    {0x15AE0, {0x10111213u, 0x14151617u, 0x08090A0Bu, 0x1C1D1E1Fu}},
    {0x15AF0, {0x10111213u, 0x14151617u, 0x18191A1Bu, 0x0C0D0E0Fu}},
};

/* One section: y = fma(b0, x, s0); s0 = s1 + fms(b1, x, fm(a1, y)); s1 = fms(b2, x, fm(a2, y)). */
typedef struct {
    float b0, b1, b2, a1, a2;
} biquad;

/* The section over n samples at buf (big-endian floats, in place), exactly as the SPU does it. The zero tests of fm
 * and fma become branches the host predicts (the samples are almost never zeros): a loop without them measured no
 * faster. */
static void biquad_exact(const biquad* q, uint8_t* ls, uint32_t buf, uint32_t n, float* s0p, float* s1p)
{
    float s0 = *s0p, s1 = *s1p;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* p = ls + buf + 4 * i;
        const float x = ps_ldf32(p);
        const float y = s_fma(q->b0, x, s0);
        ps_stf32(p, y);
        const float u = s_fms(q->b1, x, s_fm(q->a1, y));
        s0 = s_fa(s1, u);
        s1 = s_fms(q->b2, x, s_fm(q->a2, y));
    }
    *s0p = s0;
    *s1p = s1;
}

/* 0x062C8: the biquad. $3 the buffer, $4-$8 b0, b1, b2, a1, a2 (preferred words), $9 the state (words 0 and 1 of its
 * quadword: s0, s1), $10 the count. Writes y over each sample, then s0 and s1 into words 0 and 1 of the state's
 * quadword (read at the start, written at the end, as the job does). Callers may read $6 after it (the synthesizer's
 * program interpreter can store it as a module's result, spu-live): the job leaves there the insertion control (cwd)
 * of the last sample's address. */
int ps_gta5_012b_062c8(SPUContext* ctx, uint8_t* ls)
{
    if (!constants_hold(ls, ctx->reloc, k_012b_words, 2)) return 0;
    const uint32_t buf = pref(ctx, 3), n = pref(ctx, 10), st = pref(ctx, 9) & PS_SPU_QW_MASK;
    if (n == 0) { /* the state written back as it was: nothing changes */
        ret(ctx);
        return 1;
    }
    if ((buf & 3) || n > 0x10000 || !in_ls(buf, 4 * n)) return 0;
    const biquad q = {preff(ctx, 4), preff(ctx, 5), preff(ctx, 6), preff(ctx, 7), preff(ctx, 8)};
    float s0 = ps_ldf32(ls + st), s1 = ps_ldf32(ls + st + 4);
    biquad_exact(&q, ls, buf, n, &s0, &s1);
    ps_stf32(ls + st, s0);
    ps_stf32(ls + st + 4, s1);
    psl_st(ctx, 6, psl_cmask(buf + 4 * (n - 1), 4));
    ret(ctx);
    return 1;
}

/* Rotations of a quadword's lanes by 0 to 3 words (rotqby by 4 j) and a lane's mask, as byte tables. */
static const uint8_t k_rot[4][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3},
    {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7},
    {12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
};
static const uint32_t k_lane[4][4] = {{~0u, 0, 0, 0}, {0, ~0u, 0, 0}, {0, 0, ~0u, 0}, {0, 0, 0, ~0u}};

static inline uint32x4_t v_rot(uint32x4_t q, uint32_t j)
{
    return vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(q), vld1q_u8(k_rot[j])));
}

/* 0x05B20's first section on four lanes, exactly (psl_fma, psl_fm, psl_fms, psl_fa), from the quadwords of samples as
 * the job loads them: before sample i is processed its quadword holds the outputs of the samples before it and the
 * inputs of the others (and whatever else the quadword holds). in and out: the quadwords' words from the first one's
 * start (inputs and outputs), the first sample at word lead. Returns y's last value. */
typedef struct {
    uint32x4_t B0, B1c, B2c, A1, A2, S0, S1;
} lanes4;

static uint32x4_t first_section_exact(lanes4* l, const uint32_t* in, const uint32_t* out, uint32_t lead, uint32_t n)
{
    uint32x4_t Y = vdupq_n_u32(0);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t g = lead + i, j = g & 3;
        uint32_t w[4];
        for (uint32_t k = 0; k < 4; k++) w[k] = k < j && g - j + k >= lead ? out[g - j + k] : in[g - j + k];
        const uint32x4_t X = v_rot(vld1q_u32(w), j), Xc = v_clamp(X);
        Y = psn_fma(X, l->B0, l->S0);
        l->S0 = v_fa(l->S1, v_fms_c(Xc, l->B1c, psn_fm(Y, l->A1)));
        l->S1 = v_fms_c(Xc, l->B2c, psn_fm(Y, l->A2));
    }
    return Y;
}

/* 0x05B20: two sections in series with the same coefficients (a 24 dB filter). Arguments as 0x062C8's, the state the
 * four words of its quadword (s0, s1 for the first section, s2, s3 for the second). Per sample:
 *   y = fma(x, b0, s0); s0 = s1 + fms(x, b1, fm(y, a1)); s1 = fms(x, b2, fm(y, a2))
 *   o = fma(b0, y, s2); s2 = s3 + fms(y, b1, fm(o, a1)); s3 = fms(y, b2, fm(o, a2)); x := o
 * The first section's y is left in $6, which callers may read (the synthesizer's program interpreter): the job computes
 * it on whole quadwords, so its other three words are a computation of their own (lane k: word k of each
 * coefficient's register, the state's words rotated by k, the words of the sample's quadword rotated to the sample),
 * done here on four lanes beside the samples' (lane 0, on its own: the other lanes never reach local store). */
int ps_gta5_012b_05b20(SPUContext* ctx, uint8_t* ls)
{
    if (!constants_hold(ls, ctx->reloc, k_012b_words, 4)) return 0;
    const uint32_t buf = pref(ctx, 3), n = pref(ctx, 10), st = pref(ctx, 9) & PS_SPU_QW_MASK;
    if (n == 0) { /* the state written back as it was: nothing changes */
        ret(ctx);
        return 1;
    }
    if ((buf & 3) || n > 1024 || !in_ls(buf & ~15u, (4 * n + (buf & 15) + 15) & ~15u)) return 0;
    const uint32x4_t state = v_ldq(ls, st);
    /* The lanes' s0 is always a sum (never a denormal: the host flushes them, as the SPU's model) but the first. With no
     * denormal in the state, an fma whose x or b0 is a zero is fma(-0, 1, s0) = s0, and a y that is a zero is +-0. */
    if (vmaxvq_u32(vandq_u32(psn_zc(state), vtstq_u32(state, vdupq_n_u32(0x007FFFFFu))))) return 0;

    /* The samples' inputs, and their quadwords' other words (for the lanes of $6), as host words. */
    uint32_t in[1024 + 8], out[1024 + 8];
    const uint32_t lead = (buf >> 2) & 3, words = (lead + n + 3) & ~3u;
    for (uint32_t k = 0; k < words; k++) in[k] = ps_ld32(ls + (buf & ~15u) + 4 * k);
    const uint32_t* x_in = in + lead;
    uint32_t* x_out = out + lead;

    /* Lane 0: both sections, exactly. */
    const float b0 = preff(ctx, 4), b1 = preff(ctx, 5), b2 = preff(ctx, 6), a1 = preff(ctx, 7), a2 = preff(ctx, 8);
    float s0 = ffrom(vgetq_lane_u32(state, 0)), s1 = ffrom(vgetq_lane_u32(state, 1));
    float s2 = ffrom(vgetq_lane_u32(state, 2)), s3 = ffrom(vgetq_lane_u32(state, 3));
    /* Lanes 0-3 of the first section for $6, fast: fma's zeros by the substitution above, fm's zero factor y tested at
     * the end (any y that is a zero: the lanes again, exactly). */
    lanes4 l = {v_ld_reg(ctx, 4), v_clamp(v_ld_reg(ctx, 5)), v_clamp(v_ld_reg(ctx, 6)), v_ld_reg(ctx, 7), v_ld_reg(ctx, 8),
        state, v_rot(state, 1)};
    const lanes4 l0 = l;
    const uint32x4_t nzB0 = psn_nz(l.B0), nzA1 = psn_nz(l.A1), nzA2 = psn_nz(l.A2);
    const float32x4_t nA1 = vnegq_f32(vreinterpretq_f32_u32(l.A1)), nA2 = vnegq_f32(vreinterpretq_f32_u32(l.A2));
    const float32x4_t one = vdupq_n_f32(1.0f), mzero = vdupq_n_f32(-0.0f);
    const float32x4_t B0f = vreinterpretq_f32_u32(l.B0), B1f = vreinterpretq_f32_u32(l.B1c), B2f = vreinterpretq_f32_u32(l.B2c);
    float32x4_t S0 = vreinterpretq_f32_u32(l.S0), S1 = vreinterpretq_f32_u32(l.S1), Y = vdupq_n_f32(0.0f);
    uint32x4_t zy = vdupq_n_u32(0);
    uint32x4_t Q = vld1q_u32(in);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t j = (lead + i) & 3;
        if (j == 0 && i) Q = vld1q_u32(in + lead + i);
        /* lane 0 */
        const float x = ffrom(x_in[i]);
        const float y = s_fma(x, b0, s0);
        s0 = s_fa(s1, s_fms(x, b1, s_fm(y, a1)));
        s1 = s_fms(x, b2, s_fm(y, a2));
        const float o = s_fma(b0, y, s2);
        s2 = s_fa(s3, s_fms(y, b1, s_fm(o, a1)));
        s3 = s_fms(y, b2, s_fm(o, a2));
        x_out[i] = fbits(o);
        /* the four lanes */
        const uint32x4_t X = v_rot(Q, j);
        const float32x4_t Xc = vreinterpretq_f32_u32(v_clamp(X));
        const uint32x4_t mx = vandq_u32(psn_nz(X), nzB0);
        Y = vfmaq_f32(S0, vbslq_f32(mx, vreinterpretq_f32_u32(X), mzero), vbslq_f32(mx, B0f, one));
        zy = vorrq_u32(zy, psn_zc(vreinterpretq_u32_f32(Y)));
        S0 = vaddq_f32(S1, vfmaq_f32(vbslq_f32(nzA1, vmulq_f32(Y, nA1), mzero), Xc, B1f));
        S1 = vfmaq_f32(vbslq_f32(nzA2, vmulq_f32(Y, nA2), mzero), Xc, B2f);
        Q = vbslq_u32(vld1q_u32(k_lane[j]), vdupq_n_u32(x_out[i]), Q);
    }
    uint32x4_t Yv = vreinterpretq_u32_f32(Y);
    if (vmaxvq_u32(zy)) {
        l = l0;
        Yv = first_section_exact(&l, in, out, lead, n);
    }
    for (uint32_t i = 0; i < n; i++) ps_st32(ls + buf + 4 * i, x_out[i]);
    ps_stf32(ls + st, s0);
    ps_stf32(ls + st + 4, s1);
    ps_stf32(ls + st + 8, s2);
    ps_stf32(ls + st + 12, s3);
    v_st_reg(ctx, 6, Yv);
    ret(ctx);
    return 1;
}
