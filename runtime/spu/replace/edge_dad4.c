/*
 * EDGE geometry (Sony's libedgegeom), the build of SPU job spu_dad457863220b1cc, natively (D-965).
 *
 * Each function here does what the job's function does to local store and to the registers of the contract
 * (pspocket/spu/replace.hpp): its outputs bit for bit, every floating-point operation the SPU's (fp.h, through the
 * lifted code's own helpers, so each gives what the lifted code gives) in the SPU's order. It is not a transcription
 * of the compiled code: vectors are laid out as the data is (one vertex a lane), and the loops are not the job's
 * software-pipelined ones. It declines (returns 0, nothing changed) whatever it does not handle, and the lifted
 * function runs: tools/spu-native-check checks both against states captured from the game.
 */
#include "native.h"

#include <string.h>

#include "neon.h"
#include "pspocket/spu/lifted.h"

/* ---- shared helpers ---- */

/* Guest word i of a register value (D-004: host lane 3 - i) and the vector of that word in every lane. */
static inline uint32_t w_of(ps_spu_vec v, int i) { return v[3 - i]; }
static inline uint32x4_t splat_w(ps_spu_vec v, int i) { return vdupq_n_u32(v[3 - i]); }
static inline uint32_t pref(const SPUContext* ctx, int r) { return ctx->gpr[r].u32[3]; }

/* The quadword constants the job's code reads from its own image (its shuffle controls and the like), at the
 * program's addresses: the native code computes what the job computes with them, so it checks they are still these. */
typedef struct {
    uint32_t addr;
    uint32_t w[4]; /* guest words */
} edge_const;

static int constants_hold(const uint8_t* ls, uint32_t rel, const edge_const* k, int n)
{
    for (int i = 0; i < n; i++) {
        const uint8_t* q = ls + ((k[i].addr + rel) & PS_SPU_QW_MASK);
        for (int w = 0; w < 4; w++)
            if (ps_ld32(q + 4 * w) != k[i].w[w]) return 0;
    }
    return 1;
}

/* An LS range [start, start + size) inside local store without wrapping. */
static inline int in_ls(uint32_t start, uint32_t size) { return start <= PS_SPU_LS_SIZE && size <= PS_SPU_LS_SIZE - start; }

/* ===================================================================================================================
 * 0x0ECF8: the custom vertex-output callback the culling stage calls (through a pointer) for each batch. An attribute
 * whose output format byte (descriptor + 183) is 3 takes a path that normalises and projects; every other batch goes on
 * to the screen-space transform below (the only one GTA V's walk ever takes).
 *
 * The transform, for `count` positions (x, y, z, ignored w) in groups of four:
 *   C       = M1 * M0' (M0' = M0's first three rows and (0, 0, 0, 1)), M1 at ctx + 0xE0, M0 at ctx + 0xA0
 *   w       = C3 . (x, y, z, 1); clip coordinates a, b, c likewise with C0, C1, C2 (each a chain of fma: x first)
 *   r       = fi(w, frest(w)); 1/w = fma(fnms(r, w, 1), r, r)            (the estimate and one Newton step)
 *   n_i     = fm(clip_i, 1/w) with w's sign bit flipped into it
 *   s_i     = fma(n_i, scale_i, offset_i)      (viewport: scales at ctx + 0x120, offsets at ctx + 0x130; x and y
 *                                               offsets + 0.5 unless byte 13 of ctx + 0x130 is zero)
 *   x_i/y_i = cflts(s_x/s_y, 173)
 *   packed  = x_i << 16 | (y_i & 0xFFFF), bit 31 replaced by w > 0
 *   outcode = z<zmin, x<xmin, y<ymin, x>xmax, y>ymax, z>zmax in bits 7 to 2 (fcgt), against the scissor rectangle
 *             (u16 x, y, width, height at ctx + 0xD0: maxima x + width, y + height, as floats) and the depth range
 *             (two floats after it, either order)
 *   out     = (s_x, s_y, outcode << 24, packed), 16 bytes a vertex
 * read from the attribute's buffer (the word at ctx + 0x5C) and written to the output buffer (the word at ctx + 0x10
 * + 4 * (low nibble of ctx byte 0)). The job's loop runs three iterations past the end (its pipeline), which store at
 * the output's start before the first vertex's store and read past the input's end: nothing of that is left.
 *
 * Registers: everything the function keeps ($0, $1, $80-$83, $126: saved on the stack and restored) is unchanged; the
 * volatile ones are left as they were (dead where it returns, spu-live). Local store: the outputs and the six
 * quadwords it saves below the stack pointer.
 * =================================================================================================================== */

static const edge_const k_0ecf8[] = {
    {0x12710, {0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u}}, /* (0, 0, 0, 1) */
    {0x129B0, {0x00011213u, 0x04051617u, 0x08091A1Bu, 0x0C0D1E1Fu}}, /* (a.hi, b.lo) per word: packing */
    {0x129C0, {0x80808004u, 0x05060707u, 0x80808080u, 0x80808080u}}, /* the pipeline's store-offset shift */
    {0x12A40, {0x80800001u, 0x80800203u, 0x80801415u, 0x80801617u}}, /* halfwords to words: the rectangle */
    {0x12AA0, {0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u}}, /* transposes */
    {0x12AB0, {0x00010203u, 0x10111213u, 0x08090A0Bu, 0x18191A1Bu}},
    {0x12AD0, {0x00010203u, 0x04050607u, 0x10111213u, 0x80808080u}}, /* (xmin, ymin, zmin, 0) */
    {0x12B00, {0x00010203u, 0x04050607u, 0x13808080u, 0x0C0D0E0Fu}}, /* (x, y, outcode, packed) */
    {0x12BB0, {0x04050607u, 0x14151617u, 0x0C0D0E0Fu, 0x1C1D1E1Fu}},
    {0x12BE0, {0x08090A0Bu, 0x18191A1Bu, 0x08090A0Bu, 0x18191A1Bu}},
    {0x12C00, {0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u, 0x80808080u}}, /* (xmax, ymax, zmax, 0) */
    {0x12C10, {0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u}},
    {0x12C40, {0x0A0B0203u, 0x06071213u, 0x16171A1Bu, 0x80808080u}}, /* the outcode bits' order */
};

/* What every group of four vertices uses: the matrix rows, which of their elements are not zeros, the viewport and
 * the outcode bounds as fcgt keys. */
typedef struct {
    float32x4_t row[4];
    unsigned live; /* bit 4i + j: element (i, j) is not a zero */
    int live_sx, live_sy, live_sz;
    uint32x4_t sc_x, sc_y, sc_z, of_x, of_y, of_z;
    int32x4_t k_xmin, k_ymin, k_zmin, k_xmax, k_ymax, k_zmax;
} xform_0ecf8;

/* t = fma(v, C[i][j], t) as the SPU does it, with v's "not a zero" mask nv (the lane of row i's column j) */
#define CLIP_FMA(t, v, nv, i, j)                                                                                   \
    if (x->live & (1u << (4 * (i) + (j))))                                                                        \
    t = vbslq_u32(nv, vreinterpretq_u32_f32(vfmaq_laneq_f32(vreinterpretq_f32_u32(t), vreinterpretq_f32_u32(v), x->row[i], j)), t)
#define CLIP(t, i)                                                                                                 \
    uint32x4_t t = vreinterpretq_u32_f32(vdupq_laneq_f32(x->row[i], 3));                                          \
    CLIP_FMA(t, px, nx, i, 0);                                                                                    \
    CLIP_FMA(t, py, ny, i, 1);                                                                                    \
    CLIP_FMA(t, pz, nz, i, 2)

/* Four vertices (positions in lanes, host order) to their outputs (s_x, s_y, outcode << 24, packed). */
static inline __attribute__((always_inline)) uint32x4x4_t xform_group_0ecf8(const xform_0ecf8* x, uint32x4x4_t v)
{
    const uint32x4_t px = v.val[0], py = v.val[1], pz = v.val[2];
    const uint32x4_t nx = psn_nz(px), ny = psn_nz(py), nz = psn_nz(pz);
    const uint32x4_t one = vdupq_n_u32(0x3F800000u), sign = vdupq_n_u32(0x80000000u);
    CLIP(w, 3);
    CLIP(ca, 0);
    CLIP(cb, 1);
    CLIP(cc, 2);
    const uint32x4_t est = (uint32x4_t)psl_fi((ps_spu_vec)w, (ps_spu_vec)psn_frest(w));
    const uint32x4_t err = (uint32x4_t)psl_fnms((ps_spu_vec)est, (ps_spu_vec)w, (ps_spu_vec)one);
    const uint32x4_t inv = psn_fma(err, est, est);
    const uint32x4_t ninv = psn_nz(inv), wsign = vandq_u32(w, sign);
    const uint32x4_t qx = veorq_u32(wsign, psn_fm_n(ca, inv, vandq_u32(psn_nz(ca), ninv)));
    const uint32x4_t qy = veorq_u32(wsign, psn_fm_n(cb, inv, vandq_u32(psn_nz(cb), ninv)));
    const uint32x4_t qz = veorq_u32(wsign, psn_fm_n(cc, inv, vandq_u32(psn_nz(cc), ninv)));
    const uint32x4_t sx = x->live_sx ? psn_fma_n(qx, x->sc_x, x->of_x, psn_nz(qx)) : x->of_x;
    const uint32x4_t sy = x->live_sy ? psn_fma_n(qy, x->sc_y, x->of_y, psn_nz(qy)) : x->of_y;
    const uint32x4_t sz = x->live_sz ? psn_fma_n(qz, x->sc_z, x->of_z, psn_nz(qz)) : x->of_z;
    const uint32x4_t ix = (uint32x4_t)psl_cflts((ps_spu_vec)sx, 173), iy = (uint32x4_t)psl_cflts((ps_spu_vec)sy, 173);
    const int32x4_t kx = psn_fkey(sx), ky = psn_fkey(sy), kz = psn_fkey(sz);
    const uint32x4_t front = psn_fcgt_k(psn_fkey(w), vdupq_n_s32(0));
    const uint32x4_t packed = vbslq_u32(sign, front, vsliq_n_u32(vandq_u32(iy, vdupq_n_u32(0xFFFFu)), ix, 16));
    /* the outcode's bits, already in the top byte */
    const uint32x4_t code = vorrq_u32(
        vorrq_u32(vandq_u32(psn_fcgt_k(x->k_zmin, kz), vdupq_n_u32(0x80000000u)),
            vandq_u32(psn_fcgt_k(x->k_xmin, kx), vdupq_n_u32(0x40000000u))),
        vorrq_u32(vorrq_u32(vandq_u32(psn_fcgt_k(x->k_ymin, ky), vdupq_n_u32(0x20000000u)),
                      vandq_u32(psn_fcgt_k(kx, x->k_xmax), vdupq_n_u32(0x10000000u))),
            vorrq_u32(vandq_u32(psn_fcgt_k(ky, x->k_ymax), vdupq_n_u32(0x08000000u)),
                vandq_u32(psn_fcgt_k(kz, x->k_zmax), vdupq_n_u32(0x04000000u)))));
    const uint32x4x4_t o = {{sx, sy, code, packed}};
    return o;
}
#undef CLIP
#undef CLIP_FMA

int ps_edge_dad4_0ecf8(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc;
    const uint32_t s = pref(ctx, 3), attr = pref(ctx, 4), sp = pref(ctx, 1);
    if (ls[(attr + 183) & PS_SPU_LS_MASK] == 3) return 0; /* the projecting path */
    if (!constants_hold(ls, rel, k_0ecf8, (int)(sizeof k_0ecf8 / sizeof k_0ecf8[0]))) return 0;

    /* The batch: count, buffers (each read as the SPU reads it: a quadword and a rotate). */
    const ps_spu_vec q0 = psl_lq(ls, s);
    const uint32_t count = w_of(psl_rotqby(q0, 6), 0) & 0xFFFFu;
    const uint32_t which = (w_of(psl_rotqby(q0, 13), 0) & 15u) << 2;
    const uint32_t out = w_of(psl_rotqby(psl_lq(ls, s + which + 0x10), (s + which) & 15u), 0);
    const uint32_t in = w_of(psl_rotqby(psl_lq(ls, s + 0x50), 12), 0);
    if (count == 0) return 0; /* the pipeline's garbage would be the output */
    const uint32_t groups = (count + 3) >> 2;
    const uint32_t in_base = in & PS_SPU_QW_MASK, out_base = out & PS_SPU_QW_MASK;
    /* The job stores each group three iterations after it loads it (and garbage at the output's start before the
     * first): an output that starts inside the input above its start would be read in another order than here. Below
     * the input, at the input, or past its end (the loads past the end only make dead values) the order changes
     * nothing. */
    if ((in & ~PS_SPU_LS_MASK) || (out & ~PS_SPU_LS_MASK) || !in_ls(in_base, groups * 64) || !in_ls(out_base, groups * 64))
        return 0;
    if (out_base > in_base && out_base < in_base + groups * 64) return 0;

    /* The registers it saves on its stack frame (and restores: they are unchanged at its return). */
    psl_stq(ls, sp - 0x10, psl_ld(ctx, 80));
    psl_stq(ls, sp - 0x20, psl_ld(ctx, 81));
    psl_stq(ls, sp - 0x30, psl_ld(ctx, 82));
    psl_stq(ls, sp - 0x40, psl_ld(ctx, 83));
    psl_stq(ls, sp - 0x50, psl_ld(ctx, 126));
    psl_stq(ls, sp - 0x70, psl_ld(ctx, 1));

    /* C = M1 * M0': row i = M1[i][0] * M0r0 + M1[i][1] * M0r1 + M1[i][2] * M0r2 + M1[i][3] * (0, 0, 0, 1). */
    const ps_spu_vec m0r0 = psl_lq(ls, s + 0xA0), m0r1 = psl_lq(ls, s + 0xB0), m0r2 = psl_lq(ls, s + 0xC0);
    const ps_spu_vec unit_w = psl_const(0, 0, 0, 0x3F800000u);
    ps_spu_vec c[4];
    for (int i = 0; i < 4; i++) {
        const ps_spu_vec m1 = psl_lq(ls, s + 0xE0 + 16u * (uint32_t)i);
        const ps_spu_vec e0 = psl_splat32(w_of(m1, 0)), e1 = psl_splat32(w_of(m1, 1));
        const ps_spu_vec e2 = psl_splat32(w_of(m1, 2)), e3 = psl_splat32(w_of(m1, 3));
        c[i] = psl_fma(e3, unit_w, psl_fma(e2, m0r2, psl_fma(e1, m0r1, psl_fm(e0, m0r0))));
    }

    /* Viewport scales and offsets; the offsets of x and y take 0.5 unless byte 13 of ctx + 0x130 is zero. */
    const ps_spu_vec vs = psl_lq(ls, s + 0x120), vo = psl_lq(ls, s + 0x130);
    const int half = (psl_rotqby(vo, 10)[3] & 0xFFu) != 0;
    const ps_spu_vec half_v = psl_splat32(half ? 0x3F000000u : 0u);
    const uint32x4_t sc_x = splat_w(vs, 0), sc_y = splat_w(vs, 1), sc_z = splat_w(vs, 2);
    const uint32x4_t of_x = (uint32x4_t)psl_fa(psl_splat32(w_of(vo, 0)), half_v);
    const uint32x4_t of_y = (uint32x4_t)psl_fa(psl_splat32(w_of(vo, 1)), half_v);
    const uint32x4_t of_z = splat_w(vo, 2);

    /* The rectangle and the depth range: (x, y, x + width, y + height) as floats (16-bit sums, unsigned), the depth
     * range's two words ordered by fcgt. */
    const ps_spu_vec r = psl_lq(ls, s + 0xD0);
    const uint32_t rx = r[3] >> 16, ry = r[3] & 0xFFFFu, rw = r[2] >> 16, rh = r[2] & 0xFFFFu;
    const ps_spu_vec rect = psl_cuflt(psl_const(rx, ry, (rx + rw) & 0xFFFFu, (ry + rh) & 0xFFFFu), 155, 0);
    const ps_spu_vec z0 = psl_splat32(w_of(r, 2)), z1 = psl_splat32(w_of(r, 3));
    const ps_spu_vec z0_gt_z1 = psl_fcgt(z0, z1);
    const uint32x4_t zmax = (uint32x4_t)psl_bsel(z0_gt_z1, z1, z0), zmin = (uint32x4_t)psl_bsel(z0_gt_z1, z0, z1);
    const uint32x4_t xmin = splat_w(rect, 0), ymin = splat_w(rect, 1), xmax = splat_w(rect, 2), ymax = splat_w(rect, 3);

    /* The matrix as rows (lane j = column j, native float order), and which elements are not zeros: an fma with a zero
     * factor is its addend (fp.h), so a zero element's fma is left out, the same in every lane and every group. */
    xform_0ecf8 t;
    t.live = 0;
    for (int i = 0; i < 4; i++) {
        t.row[i] = vreinterpretq_f32_u32(psn_rev4((uint32x4_t)c[i]));
        for (int j = 0; j < 4; j++)
            if (w_of(c[i], j) & 0x7F800000u) t.live |= 1u << (4 * i + j);
    }
    t.live_sx = (vgetq_lane_u32(sc_x, 0) & 0x7F800000u) != 0;
    t.live_sy = (vgetq_lane_u32(sc_y, 0) & 0x7F800000u) != 0;
    t.live_sz = (vgetq_lane_u32(sc_z, 0) & 0x7F800000u) != 0;
    t.sc_x = sc_x, t.sc_y = sc_y, t.sc_z = sc_z, t.of_x = of_x, t.of_y = of_y, t.of_z = of_z;
    t.k_xmin = psn_fkey(xmin), t.k_ymin = psn_fkey(ymin), t.k_zmin = psn_fkey(zmin);
    t.k_xmax = psn_fkey(xmax), t.k_ymax = psn_fkey(ymax), t.k_zmax = psn_fkey(zmax);

    /* Two groups at a time: their chains are independent (the SPU code overlaps four, software-pipelined). */
    uint32_t g = 0;
    for (; g + 2 <= groups; g += 2) {
        const uint32x4x4_t a = xform_group_0ecf8(&t, psn_ld4_be(ls, in_base + 64 * g));
        const uint32x4x4_t b = xform_group_0ecf8(&t, psn_ld4_be(ls, in_base + 64 * g + 64));
        psn_st4_be(ls, out_base + 64 * g, a);
        psn_st4_be(ls, out_base + 64 * g + 64, b);
    }
    if (g < groups) psn_st4_be(ls, out_base + 64 * g, xform_group_0ecf8(&t, psn_ld4_be(ls, in_base + 64 * g)));
    ctx->pc = pref(ctx, 0) & PS_SPU_PC_MASK;
    return 1;
}
