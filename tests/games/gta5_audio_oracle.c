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
