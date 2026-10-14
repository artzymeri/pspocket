/*
 * Oracles for the native replacements of GTA V's audio job functions (games/gta5/spu, D-979): each function's
 * computation as the SPU does it, on whole quadwords with the lifted code's own helpers (pspocket/spu/lifted.h, which
 * the differential fuzz test checks against the interpreter): a sample's quadword loaded, rotated to the sample,
 * computed on all four words, the result inserted and the quadword stored. Written from what the functions compute,
 * for synthetic inputs (CLAUDE.md rule 1). Run in the SPU FP environment.
 */
#include <stdint.h>

#include "pspocket/spu/lifted.h"

/* A word of quadword `rb` replaced through the job's controls: shufb(splat(v), rb, control & 0x1F). */
static ps_spu_vec put_word(const uint8_t* ls, uint32_t control, uint32_t v, ps_spu_vec rb)
{
    const ps_spu_vec c = psl_lq(ls, control) & psl_splat8(0x1F);
    return psl_shufb(psl_splat32(v), rb, c);
}

/* 0x062C8 of spu_012bf13511b9f1b0: one biquad section over $10 samples at $3, state words 0 and 1 of $9's quadword. */
void ps_test_oracle_012b_062c8(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc;
    const ps_spu_vec B0 = psl_ld(ctx, 4), B1 = psl_ld(ctx, 5), B2 = psl_ld(ctx, 6), A1 = psl_ld(ctx, 7), A2 = psl_ld(ctx, 8);
    const uint32_t st = psl_u32p(psl_ld(ctx, 9)), n = psl_u32p(psl_ld(ctx, 10));
    uint32_t a = psl_u32p(psl_ld(ctx, 3));
    const ps_spu_vec state = psl_lq(ls, st);
    ps_spu_vec S0 = state, S1 = psl_rotqby(state, 4);
    for (uint32_t i = 0; i < n; i++, a += 4) {
        const ps_spu_vec Q = psl_lq(ls, a), m = psl_cmask(a, 4), X = psl_rotqby(Q, a & 15);
        const ps_spu_vec Y = psl_fma(B0, X, S0);
        const ps_spu_vec T1 = psl_fm(A1, Y), T2 = psl_fm(A2, Y);
        psl_stq(ls, a, psl_shufb(Y, Q, m));
        S0 = psl_fa(S1, psl_fms(B1, X, T1));
        S1 = psl_fms(B2, X, T2);
        psl_st(ctx, 6, m);
    }
    ps_spu_vec q = psl_lq(ls, st);
    q = put_word(ls, 0x15900 + rel, psl_u32p(S0), q);
    q = put_word(ls, 0x158F0 + rel, psl_u32p(S1), q);
    psl_stq(ls, st, q);
    ctx->pc = psl_u32p(psl_ld(ctx, 0)) & PS_SPU_PC_MASK;
}

/* 0x05B20 of spu_012bf13511b9f1b0: two sections in series, the state the four words of $9's quadword. */
void ps_test_oracle_012b_05b20(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc;
    const ps_spu_vec B0 = psl_ld(ctx, 4), B1 = psl_ld(ctx, 5), B2 = psl_ld(ctx, 6), A1 = psl_ld(ctx, 7), A2 = psl_ld(ctx, 8);
    const uint32_t st = psl_u32p(psl_ld(ctx, 9)), n = psl_u32p(psl_ld(ctx, 10));
    uint32_t a = psl_u32p(psl_ld(ctx, 3));
    const ps_spu_vec state = psl_lq(ls, st);
    ps_spu_vec S0 = state, S1 = psl_rotqby(state, 4), S2 = psl_rotqby(state, 8), S3 = psl_rotqby(state, 12);
    for (uint32_t i = 0; i < n; i++, a += 4) {
        const ps_spu_vec Q = psl_lq(ls, a), m = psl_cmask(a, 4), X = psl_rotqby(Q, a & 15);
        const ps_spu_vec Y = psl_fma(X, B0, S0);
        const ps_spu_vec O = psl_fma(B0, Y, S2);
        const ps_spu_vec T1 = psl_fm(Y, A1), T2 = psl_fm(Y, A2), T3 = psl_fm(O, A1), T4 = psl_fm(O, A2);
        psl_stq(ls, a, psl_shufb(O, Q, m));
        S0 = psl_fa(S1, psl_fms(X, B1, T1));
        S1 = psl_fms(X, B2, T2);
        S2 = psl_fa(S3, psl_fms(Y, B1, T3));
        S3 = psl_fms(Y, B2, T4);
        psl_st(ctx, 6, Y);
    }
    ps_spu_vec q = psl_lq(ls, st);
    q = put_word(ls, 0x15900 + rel, psl_u32p(S0), q);
    q = put_word(ls, 0x158F0 + rel, psl_u32p(S1), q);
    q = put_word(ls, 0x15AE0 + rel, psl_u32p(S2), q);
    q = put_word(ls, 0x15AF0 + rel, psl_u32p(S3), q);
    psl_stq(ls, st, q);
    ctx->pc = psl_u32p(psl_ld(ctx, 0)) & PS_SPU_PC_MASK;
}

/* ---- spu_28281614459ce75e ---- */

/* The job's signed division, its way: magnitudes, a divisor shifted to the dividend's leading bit, shift and subtract;
 * the remainder takes the dividend's sign. Returns the remainder. */
static uint32_t job_rem(uint32_t a, uint32_t b)
{
    const uint32_t ua = (int32_t)a > -1 ? a : 0u - a, ub = (int32_t)b > -1 ? b : 0u - b;
    const int diff = (int)psl_u32p(psl_clz(psl_splat32(ub))) - (int)psl_u32p(psl_clz(psl_splat32(ua)));
    const unsigned n = (unsigned)diff & 63u;
    uint32_t bit = n < 32 ? 1u << n : 0u, d = n < 32 ? ub << n : 0u, rem = ua;
    do {
        if (!(d > rem)) rem -= d;
        bit >>= 1;
        d >>= 1;
    } while (bit);
    return (int32_t)a > -1 ? rem : 0u - rem;
}

/* A word read as the job reads it (a quadword rotated to it) and replaced as it replaces it (an insert). */
static uint32_t word_at(const uint8_t* ls, uint32_t at) { return psl_u32p(psl_rotqby(psl_lq(ls, at), at & 15)); }
static void put_at(uint8_t* ls, uint32_t at, uint32_t v) { psl_stq(ls, at, psl_shufb(psl_splat32(v), psl_lq(ls, at), psl_cmask(at, 4))); }

/* A delay line: its slot's halves (shufb, xshw, csflt 140), then the slot written with two vectors (cflts 158, clamped
 * to int16, packed), the position stepped (pos + 1, the counter + 1, pos % len). */
typedef struct {
    uint32_t base_at, pos_at, len_at, count_at, slot;
} line;
static void line_read(const uint8_t* ls, uint32_t rel, line* l, ps_spu_vec* lo, ps_spu_vec* hi)
{
    l->slot = word_at(ls, l->pos_at) * 16u + word_at(ls, l->base_at);
    const ps_spu_vec q = psl_lq(ls, l->slot);
    *lo = psl_csflt(psl_xshw(psl_shufb(q, q, psl_lq(ls, 0x189F0 + rel))), 140, 0);
    *hi = psl_csflt(psl_xshw(psl_shufb(q, q, psl_lq(ls, 0x18A00 + rel))), 140, 0);
}
static ps_spu_vec clamp16(ps_spu_vec v)
{
    const ps_spu_vec hi = psl_splat32(32767), lo = psl_splat32(0xFFFF8000u);
    const ps_spu_vec m = psl_bsel(psl_cgt(v, hi), v, hi);
    return psl_bsel(psl_cgt(v, lo), lo, m);
}
static void line_write(uint8_t* ls, uint32_t rel, const line* l, ps_spu_vec lo, ps_spu_vec hi)
{
    psl_stq(ls, l->slot, psl_shufb(clamp16(psl_cflts(lo, 158)), clamp16(psl_cflts(hi, 158)), psl_lq(ls, 0x18A10 + rel)));
    const uint32_t next = word_at(ls, l->pos_at) + 1u;
    put_at(ls, l->pos_at, next);
    put_at(ls, l->count_at, word_at(ls, l->count_at) + 1u);
    put_at(ls, l->pos_at, job_rem(next, word_at(ls, l->len_at)));
}

void ps_test_oracle_2828_0a920(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc, cx = psl_u32p(psl_ld(ctx, 3)), frames = psl_u32p(psl_ld(ctx, 6)) >> 1;
    uint32_t in = psl_u32p(psl_ld(ctx, 4)), out = psl_u32p(psl_ld(ctx, 5));
    const ps_spu_vec minus1 = psl_splat32(0xBF800000u);
    for (uint32_t f = 0; f < frames; f++, in += 32, out += 32) {
        ps_spu_vec x0 = psl_lq(ls, in), x1 = psl_lq(ls, in + 16), y0 = x0, y1 = x1;
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t r = cx + 2368 + 64 * k;
            line l = {r + 44, r + 52, r + 56, r + 36, 0};
            const ps_spu_vec g = psl_lq(ls, r), c = psl_lq(ls, r + 16);
            ps_spu_vec d0, d1;
            line_read(ls, rel, &l, &d0, &d1);
            const ps_spu_vec ng = psl_fm(g, minus1);
            y1 = psl_fma(x1, ng, psl_fm(d1, c));
            y0 = psl_fma(x0, ng, psl_fm(d0, c));
            line_write(ls, rel, &l, psl_fma(y0, g, x0), psl_fma(y1, g, x1));
            x0 = y0, x1 = y1;
        }
        ps_spu_vec lo[3], hi[3];
        for (int t = 0; t < 3; t++) lo[t] = hi[t] = psl_splat32(0);
        for (uint32_t i = 0; i < 6; i++)
            for (uint32_t t = 0; t < 3; t++) {
                const uint32_t r = cx + 1504 + 288 * t + 48 * i;
                line l = {r + 28, r + 36, r + 40, r + 20, 0};
                ps_spu_vec dl, dh;
                line_read(ls, rel, &l, &dl, &dh);
                const ps_spu_vec g = psl_lq(ls, r);
                lo[t] = psl_fma(dl, g, lo[t]);
                hi[t] = psl_fma(dh, g, hi[t]);
            }
        const ps_spu_vec mlo[3] = {psl_fa(psl_fa(y0, lo[1]), lo[2]), psl_fa(psl_fa(y0, lo[0]), lo[2]), psl_fa(psl_fa(y0, lo[0]), lo[1])};
        const ps_spu_vec mhi[3] = {psl_fa(psl_fa(y1, hi[1]), hi[2]), psl_fa(psl_fa(y1, hi[0]), hi[2]), psl_fa(psl_fa(y1, hi[0]), hi[1])};
        ps_spu_vec acc_lo = psl_splat32(0), acc_hi = psl_splat32(0);
        for (uint32_t i = 0; i < 6; i++) {
            ps_spu_vec plo[3], phi[3];
            for (uint32_t t = 0; t < 3; t++) {
                const uint32_t r = cx + 1504 + 288 * t + 48 * i;
                line l = {r + 28, r + 36, r + 40, r + 20, 0};
                ps_spu_vec dl, dh;
                line_read(ls, rel, &l, &dl, &dh);
                line_write(ls, rel, &l, mlo[t], mhi[t]);
                const ps_spu_vec g = psl_lq(ls, r), tl = psl_fm(dl, g), th = psl_fm(dh, g);
                if (t == 0) {
                    acc_hi = psl_fma(th, psl_lq(ls, cx + 0x10), acc_hi);
                    acc_lo = psl_fma(tl, psl_lq(ls, cx + 0x10), acc_lo);
                    continue;
                }
                /* the bank's filter: four steps on the lo tap, four on the hi tap */
                const uint32_t s = cx + (t == 1 ? 544 : 1024) + 80 * i;
                const ps_spu_vec a = psl_lq(ls, s + 0x10), b = psl_lq(ls, s);
                ps_spu_vec z = psl_lq(ls, s + 0x40), p = psl_lq(ls, s + 0x20), o = psl_splat32(0);
                for (int h = 0; h < 2; h++) {
                    for (int step = 0; step < 4; step++) {
                        p = psl_fma(a, z, p);
                        o = psl_fma(b, z, psl_fs(h ? th : tl, p));
                        z = psl_fma(a, o, z);
                    }
                    if (h == 0) plo[t] = p;
                    psl_stq(ls, s + 0x30, o);
                    psl_stq(ls, s + 0x40, z);
                    psl_stq(ls, s + 0x20, p);
                }
                phi[t] = p;
                if (t == 1) {
                    acc_hi = psl_fma(phi[1], psl_lq(ls, cx + 0x20), acc_hi);
                    acc_lo = psl_fma(plo[1], psl_lq(ls, cx + 0x20), acc_lo);
                } else {
                    acc_hi = psl_fma(phi[2], psl_lq(ls, cx + 0x30), acc_hi);
                    acc_lo = psl_fma(plo[2], psl_lq(ls, cx + 0x30), acc_lo);
                }
            }
        }
        psl_stq(ls, out, acc_lo);
        psl_stq(ls, out + 16, acc_hi);
    }
    ctx->pc = psl_u32p(psl_ld(ctx, 0)) & PS_SPU_PC_MASK;
}

/* 0x06048: eight combs and four all-passes on four lanes, frame by frame (the records' fields read and written as the
 * job does, each frame). */
void ps_test_oracle_2828_06048(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t cx = psl_u32p(psl_ld(ctx, 3)), n = psl_u32p(psl_ld(ctx, 6));
    const uint32_t in = psl_u32p(psl_ld(ctx, 4)), out = psl_u32p(psl_ld(ctx, 5));
    const ps_spu_vec gain = psl_lq(ls, 0x18E30 + ctx->reloc), damp = psl_lq(ls, cx + 0x10), fb = psl_lq(ls, cx + 0x20);
    const ps_spu_vec undamp = psl_fs(psl_splat32(0x3F800000u), damp);
    for (uint32_t i = 0; i < n; i++) {
        const ps_spu_vec inp = psl_fm(psl_lq(ls, in + 16 * i), gain);
        ps_spu_vec acc = psl_splat32(0);
        for (uint32_t k = 0; k < 8; k++) {
            const uint32_t r = cx + 48 + 48 * k, pos = word_at(ls, r + 16), slot = word_at(ls, r + 12) + 16 * pos;
            const ps_spu_vec b = psl_lq(ls, slot);
            const ps_spu_vec f = psl_fma(psl_lq(ls, r + 0x20), damp, psl_fm(b, undamp));
            acc = psl_fa(acc, b);
            psl_stq(ls, r + 0x20, f);
            psl_stq(ls, slot, psl_fma(f, fb, inp));
            put_at(ls, r + 16, pos + 1);
        }
        ps_spu_vec v = acc;
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t r = cx + 432 + 24 * k, pos = word_at(ls, r + 16), slot = word_at(ls, r + 12) + 16 * pos;
            const ps_spu_vec b = psl_lq(ls, slot);
            psl_stq(ls, slot, psl_fma(b, psl_splat32(0x3F000000u), v));
            v = psl_fma(v, psl_splat32(0xBF800000u), b);
            put_at(ls, r + 16, pos + 1);
        }
        psl_stq(ls, out + 16 * i, psl_fm(v, psl_splat32(0x40400000u)));
    }
    ctx->pc = psl_u32p(psl_ld(ctx, 0)) & PS_SPU_PC_MASK;
}

/* 0x08748: a delay line with feedback, frame by frame. */
void ps_test_oracle_2828_08748(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc, cx = psl_u32p(psl_ld(ctx, 3)), frames = psl_u32p(psl_ld(ctx, 6)) >> 1;
    const uint32_t in = psl_u32p(psl_ld(ctx, 4)), out = psl_u32p(psl_ld(ctx, 5)), r = cx + 0x20;
    for (uint32_t f = 0; f < frames; f++) {
        const ps_spu_vec x0 = psl_lq(ls, in + 32 * f), x1 = psl_lq(ls, in + 32 * f + 16);
        const ps_spu_vec g = psl_lq(ls, r), fb = psl_lq(ls, cx + 0x10);
        line l = {r + 28, r + 36, r + 40, r + 20, 0};
        ps_spu_vec dl, dh;
        line_read(ls, rel, &l, &dl, &dh);
        const ps_spu_vec eh = psl_fm(dh, g), el = psl_fm(dl, g);
        line_write(ls, rel, &l, psl_fma(el, fb, x0), psl_fma(eh, fb, x1));
        psl_stq(ls, out + 32 * f, el);
        psl_stq(ls, out + 32 * f + 16, eh);
    }
    ctx->pc = psl_u32p(psl_ld(ctx, 0)) & PS_SPU_PC_MASK;
}
