/*
 * The native replacements' NEON building blocks (runtime/spu/replace/neon.h) against the lifted code's helpers
 * (pspocket/spu/lifted.h), which the differential fuzz test checks against the interpreter: synthetic inputs, every
 * class of value the SPU's float model treats apart (zeros, denormals, exponent 255, both signs). Called from
 * test_spu_replace.cpp; each returns the number of lanes that differ. Run in the SPU FP environment.
 */
#include <stdint.h>
#include <string.h>

#include "neon.h"
#include "pspocket/spu/lifted.h"

static uint64_t g_state = 0x9E3779B97F4A7C15ull;
static uint32_t next32(void)
{
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return (uint32_t)(g_state >> 16);
}

/* A float of every class in turn, then random bits. */
static uint32_t pick(uint32_t i)
{
    static const uint32_t special[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x80800000u,
        0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFFFFFFFu,
        0x7F800001u, 0x3F7FFFFFu, 0x4B000000u, 0xCF000000u, 0x4F000000u, 0x34000000u};
    const uint32_t n = (uint32_t)(sizeof special / sizeof special[0]);
    if (i % 4 == 0) return special[next32() % n];
    if (i % 4 == 1) return (next32() & 0x807FFFFFu) | ((next32() % 3 == 0 ? 0u : 0x7Fu + next32() % 60 - 30) << 23);
    return next32();
}

static unsigned differ(uint32x4_t a, ps_spu_vec b)
{
    unsigned n = 0;
    uint32_t x[4], y[4];
    memcpy(x, &a, 16);
    memcpy(y, &b, 16);
    for (int l = 0; l < 4; l++) n += x[l] != y[l];
    return n;
}

static ps_spu_vec vec(uint32_t i)
{
    return (ps_spu_vec){pick(i), pick(i + 1), pick(i + 2), pick(i + 3)};
}

unsigned ps_test_replace_frest(void)
{
    unsigned bad = 0;
    /* frest reads the sign, the exponent and five fraction bits (18-22): every combination, the other bits random */
    for (uint32_t hi = 0; hi < (1u << 14); hi += 4) {
        ps_spu_vec x;
        for (int l = 0; l < 4; l++) x[l] = ((hi + (uint32_t)l) << 18) | (next32() & 0x3FFFFu);
        bad += differ(psn_frest((uint32x4_t)x), psl_frest(x));
    }
    return bad;
}

unsigned ps_test_replace_frsqest(void)
{
    unsigned bad = 0;
    for (uint32_t hi = 0; hi < (1u << 14); hi += 4) {
        ps_spu_vec x;
        for (int l = 0; l < 4; l++) x[l] = ((hi + (uint32_t)l) << 18) | (next32() & 0x3FFFFu);
        bad += differ(psn_frsqest((uint32x4_t)x), psl_frsqest(x));
    }
    return bad;
}

unsigned ps_test_replace_arith(uint32_t rounds)
{
    unsigned bad = 0;
    for (uint32_t i = 0; i < rounds; i++) {
        const ps_spu_vec a = vec(4 * i), b = vec(4 * i + 7), c = vec(4 * i + 13);
        const uint32x4_t na = (uint32x4_t)a, nb = (uint32x4_t)b, nc = (uint32x4_t)c;
        bad += differ(psn_zc(na), psl_zero_class(a));
        bad += differ(vmvnq_u32(psn_nz(na)), psl_zero_class(a));
        bad += differ(psn_fm(na, nb), psl_fm(a, b));
        bad += differ(psn_fma(na, nb, nc), psl_fma(a, b, c));
        bad += differ(psn_fm_n(na, nb, vandq_u32(psn_nz(na), psn_nz(nb))), psl_fm(a, b));
        bad += differ(psn_fma_n(na, nb, nc, vandq_u32(psn_nz(na), psn_nz(nb))), psl_fma(a, b, c));
        bad += differ(psn_fcgt_k(psn_fkey(na), psn_fkey(nb)), psl_fcgt(a, b));
        bad += differ(psn_fcgt_k(psn_fkey(nb), psn_fkey(na)), psl_fcgt(b, a));
        bad += differ(psn_fcgt_k(psn_fkey(na), psn_fkey(na)), psl_fcgt(a, a));
        bad += differ(psn_rev4(na), (ps_spu_vec){a[3], a[2], a[1], a[0]});
    }
    return bad;
}

/* Four quadwords of local store as big-endian words, de-interleaved, and back: as psl_lq reads them. */
unsigned ps_test_replace_ld4(void)
{
    static uint8_t ls[256] __attribute__((aligned(16)));
    for (int i = 0; i < 256; i++) ls[i] = (uint8_t)next32();
    unsigned bad = 0;
    const uint32x4x4_t v = psn_ld4_be(ls, 64);
    for (int k = 0; k < 4; k++) {
        const ps_spu_vec q = psl_lq(ls, 64 + 16 * (uint32_t)k); /* guest word w is host lane 3 - w */
        for (int w = 0; w < 4; w++) {
            uint32_t lanes[4];
            memcpy(lanes, &v.val[w], 16);
            bad += lanes[k] != q[3 - w];
        }
    }
    static uint8_t back[256] __attribute__((aligned(16)));
    memcpy(back, ls, sizeof back);
    psn_st4_be(back, 128, v);
    bad += memcmp(back + 128, ls + 64, 64) != 0;
    bad += memcmp(back, ls, 128) != 0 || memcmp(back + 192, ls + 192, 64) != 0;
    return bad;
}
