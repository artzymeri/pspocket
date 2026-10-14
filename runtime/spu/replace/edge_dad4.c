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

/* ===================================================================================================================
 * 0x0F008: the culling stage (entered by the get-pc idiom from 0x0EFF8, which saved $126 at sp - 0x40). Arguments: $3 the
 * batch context (as 0x0ECF8's), $4 the mode (1: cull one facing, 2 and 3 the other; 0 culls nothing), $5 an offset in
 * vertices into the output of the callback. It calls the vertex-output callback (the pointer at ctx + 0x174, its
 * argument at ctx + 0x178; none: the transform at 0x0E8D8 directly), then culls the batch's triangles four at a time:
 * the indices (u16, three a triangle, at the word ctx + 0x50) pick the callback's 16-byte vertices (s_x, s_y,
 * outcode << 24, packed); a triangle is culled when
 *   - its facing (the sign of (B - A) x (C - A) in screen space, flipped by the winding flag of ctx byte 6) is not the
 *     mode's, or a vertex is behind the near plane (outcode bit 31) or has w <= 0 (packed bit 31 clear) — combined by
 *     the mode's fnms — or all three share an outcode bit;
 *   - or (when ctx + 0x13D is not zero) it covers no pixel: its integer coordinates all equal in x or in y, or its
 *     x + y or x - y (as cflts of the floats) all equal.
 * The indices of the triangles kept are written back over the index buffer, packed, through the job's tables of
 * shuffles (by the four triangles' culled mask) and of byte shifts (by the output's alignment), three quadwords a group
 * (the last two to a scratch quadword pair on the stack near the buffer's end). The count of indices kept goes to the
 * halfword at ctx + 0x17E and is the result ($3); the buffer's new aligned end to the word at ctx + 0x58.
 *
 * Natively: the same sequence of local-store reads and writes, group by group (the job's loop overlaps three groups;
 * here the loads and stores of each happen in the job's order relative to the others: index loads, then the stores of
 * the group three behind, then the vertex loads), the tests four triangles a vector, one triangle a lane.
 * =================================================================================================================== */

static const edge_const k_0f008[] = {
    {0x126F0, {0xFFFFFFEFu, 0xFFFFFFDFu, 0x00000000u, 0x00000000u}}, /* the buffer-end limits of the 2nd/3rd store */
    {0x12700, {0x08080808u, 0x08080808u, 0x18181818u, 0x18181818u}}, /* toggles the index shuffle by 8 bytes */
    {0x12AA0, {0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u}}, /* (a0, b0, a1, b1): x and y of two vertices */
    {0x12AF0, {0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu}}, /* identity */
    {0x13240, {0x08090A0Bu, 0x18191A1Bu, 0x80808080u, 0x80808080u}}, /* outcodes */
    {0x13250, {0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x80808080u, 0x80808080u}}, /* packed coordinates */
    {0x13260, {0x00010203u, 0x04050607u, 0x10111213u, 0x14151617u}}, /* (a0, a1, b0, b1) */
};

/* Four vertices (16 bytes each, big-endian words) to (x, y, outcode, packed) of each, one vertex a lane. */
static inline __attribute__((always_inline)) uint32x4x4_t vertices4(const uint8_t* ls, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    const uint32x4_t r0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + a0)));
    const uint32x4_t r1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + a1)));
    const uint32x4_t r2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + a2)));
    const uint32x4_t r3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + a3)));
    const uint32x4_t t0 = vtrn1q_u32(r0, r1), t1 = vtrn2q_u32(r0, r1), t2 = vtrn1q_u32(r2, r3), t3 = vtrn2q_u32(r2, r3);
    uint32x4x4_t v;
    v.val[0] = vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2)));
    v.val[1] = vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3)));
    v.val[2] = vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2)));
    v.val[3] = vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3)));
    return v;
}

static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }

typedef struct {
    uint32_t vbase;                      /* the vertices' LS address (the callback's output + 16 * $5) */
    uint32x4_t flip, mode, pad; /* xor into the area's sign; the fnms factor; the last group's padding triangles */
    int nopix_on;               /* the "no pixel" test (ctx byte 0x13D not zero) */
} cull_0f008;

/* The culled mask of one group (bit 3 - t: triangle t), from its indices (two quadwords as the job shuffles them). */
static unsigned cull_group_0f008(const uint8_t* ls, const cull_0f008* c, ps_spu_vec i56, ps_spu_vec i55, int last)
{
    /* The vertex of each index: the job adds index * 16 (a word of two indices rotated by 4, or 20, bits) to the base. */
    uint32_t a[12];
    for (int k = 0; k < 4; k++) {
        const uint32_t w = w_of(i56, k);
        a[2 * k] = (c->vbase + rotl32(w, 20)) & PS_SPU_QW_MASK;
        a[2 * k + 1] = (c->vbase + rotl32(w, 4)) & PS_SPU_QW_MASK;
    }
    for (int k = 0; k < 2; k++) {
        const uint32_t w = w_of(i55, k);
        a[8 + 2 * k] = (c->vbase + rotl32(w, 20)) & PS_SPU_QW_MASK;
        a[9 + 2 * k] = (c->vbase + rotl32(w, 4)) & PS_SPU_QW_MASK;
    }
    /* triangle t: A = vertex 3t, B = 3t + 1, C = 3t + 2 */
    const uint32x4x4_t va = vertices4(ls, a[0], a[3], a[6], a[9]);
    const uint32x4x4_t vb = vertices4(ls, a[1], a[4], a[7], a[10]);
    const uint32x4x4_t vc = vertices4(ls, a[2], a[5], a[8], a[11]);
#define V(x) ((ps_spu_vec)(x))
    const ps_spu_vec ax = V(va.val[0]), ay = V(va.val[1]), bx = V(vb.val[0]), by = V(vb.val[1]);
    const ps_spu_vec cx = V(vc.val[0]), cy = V(vc.val[1]);
    const ps_spu_vec aoc = V(va.val[2]), boc = V(vb.val[2]), coc = V(vc.val[2]);
    const ps_spu_vec ap = V(va.val[3]), bp = V(vb.val[3]), cp = V(vc.val[3]);
    /* facing, depth and w */
    const ps_spu_vec ex1 = psl_fs(bx, ax), ex2 = psl_fs(cx, ax), ey1 = psl_fs(by, ay), ey2 = psl_fs(cy, ay);
    const ps_spu_vec area = psl_fms(ex1, ey2, psl_fm(ey1, ex2));
    const ps_spu_vec facing = (ps_spu_vec)psn_fcgt_k(psn_fkey((uint32x4_t)psl_xor(V(c->flip), area)), vdupq_n_s32(0));
    const ps_spu_vec uni = psl_or(psl_or(aoc, boc), coc), inter = psl_and(psl_and(aoc, boc), coc);
    const ps_spu_vec in_front = psl_clgt(psl_splat32(0x80000000u), uni);
    const ps_spu_vec w_pos = psl_srai(psl_and(cp, psl_and(bp, ap)), 31);
    const ps_spu_vec keep = psl_and(psl_and(facing, in_front), w_pos);
    const ps_spu_vec out_side = psl_clgt(inter, psl_splat32(0));
    const ps_spu_vec geom = psl_fnms(keep, V(c->mode), out_side);
    ps_spu_vec culled = geom;
    if (c->nopix_on) {
        /* no pixel: equal integer x or y (the packed halfwords), or equal x + y or x - y (switched off, it is 0) */
        const ps_spu_vec same_xy = psl_clgt(psl_and(psl_ceqh(ap, cp), psl_ceqh(ap, bp)), psl_splat32(0));
        const ps_spu_vec sa = psl_cflts(psl_fa(ax, ay), 173), da = psl_cflts(psl_fs(ax, ay), 173);
        const ps_spu_vec sb = psl_cflts(psl_fa(bx, by), 173), db = psl_cflts(psl_fs(bx, by), 173);
        const ps_spu_vec sc = psl_cflts(psl_fa(cx, cy), 173), dc = psl_cflts(psl_fs(cx, cy), 173);
        const ps_spu_vec same_d = psl_or(psl_and(psl_ceq(sc, sa), psl_ceq(sb, sa)), psl_and(psl_ceq(dc, da), psl_ceq(db, da)));
        culled = psl_or(psl_or(same_d, same_xy), geom);
    }
#undef V
    unsigned mask = 0;
    for (int t = 0; t < 4; t++) mask |= (culled[t] & 1u) << (3 - t); /* natural lanes: lane t is triangle t */
    if (last) mask |= vgetq_lane_u32(c->pad, 0);
    return mask & 15u;
}

int ps_edge_dad4_0f008(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t rel = ctx->reloc;
    const uint32_t s = pref(ctx, 3), mode = pref(ctx, 4), sp = pref(ctx, 1);
    if (mode < 1 || mode > 3) return 0;               /* 0 culls nothing, above 3 skips the loop: the lifted paths */
    if (!(psl_rotqby(psl_lq(ls, s), 13)[3] & 0x80u)) return 0; /* ctx byte 0's top bit clear: the job asserts */
    if (!constants_hold(ls, rel, k_0f008, (int)(sizeof k_0f008 / sizeof k_0f008[0]))) return 0;

    /* The frame: $80-$82 and the link saved, the stack pointer below the 144 bytes it takes. The job's base as it
     * computes it ($126, the link of the get-pc call, less the offset 0x0EFF8 put in $2). */
    const ps_spu_vec in0 = psl_ld(ctx, 0), in1 = psl_ld(ctx, 1);
    const ps_spu_vec in4 = psl_ld(ctx, 4), in3 = psl_ld(ctx, 3), in5 = psl_ld(ctx, 5);
    const ps_spu_vec v126 = psl_sf(psl_ld(ctx, 2), psl_ld(ctx, 126));
    const uint32_t base = psl_u32p(v126);
    psl_stq(ls, sp - 0x10, psl_ld(ctx, 80));
    psl_stq(ls, sp - 0x20, psl_ld(ctx, 81));
    psl_stq(ls, sp - 0x30, psl_ld(ctx, 82));
    psl_stq(ls, sp + 0x10, in0);
    psl_stq(ls, sp - 0x90, in1);
    const uint32_t sp2 = sp - 144;

    /* The callback through the program's table (native or lifted), with the registers the job calls it with. */
    {
        const ps_spu_vec cbq = psl_lq(ls, s + 0x170);
        const uint32_t cb = w_of(cbq, 1);
        const uint32_t target = cb ? cb & PS_SPU_PC_MASK : (0x0E8D8u + rel) & PS_SPU_PC_MASK;
        const uint32_t link = ((cb ? 0x0F0A8u : 0x0F0B0u) + rel) & PS_SPU_PC_MASK;
        psl_st(ctx, 2, psl_rotqby(cbq, 4));
        if (cb) psl_st(ctx, 4, psl_rotqby(cbq, 8));
        psl_st(ctx, 0, psl_pref(link));
        psl_st(ctx, 1, psl_a(in1, psl_splat32((uint32_t)-144)));
        psl_st(ctx, 80, in4);
        psl_st(ctx, 81, in3);
        psl_st(ctx, 82, in5);
        psl_st(ctx, 126, v126);
        ctx->pc = target;
        /* With no lifted code there, or when the callback stops before it returns (a stop request at a loop's back
         * edge), the run loop takes over from where it is: the registers are the job's at its call, and the lifted
         * code resumes the culling at the return address. */
        const ps_spu_fn fn = ps_spu_lookup(ctx, target);
        if (!fn) return 1;
        fn(ctx, ls);
        if (ctx->pc != link || ctx->stop_reason != PS_SPU_STOP_NONE) return 1;
    }

    /* The batch: triangles (a third of the index count, the low half of the word at ctx + 0x17C), the index buffer
     * (the word at ctx + 0x50), the vertices (the callback's output: the word at ctx + 0x10 + 4 * (ctx byte 0 & 15),
     * plus 16 * $5). */
    const ps_spu_vec q170 = psl_lq(ls, s + 0x170), q0 = psl_lq(ls, s), q130 = psl_lq(ls, s + 0x130), q50 = psl_lq(ls, s + 0x50);
    const uint32_t count = w_of(psl_rotqby(q170, 12), 0) & 0xFFFFu;
    const uint32_t tris = (count / 3) & 0xFFFFu;
    const uint32_t which = (w_of(psl_rotqby(q0, 13), 0) & 15u) << 2;
    const uint32_t outp = w_of(psl_rotqby(psl_lq(ls, s + which + 0x10), (s + which) & 15u), 0);
    const uint32_t idx = w_of(q50, 0);
    const uint32_t groups = (tris + 3) >> 2;
    const uint32_t end = idx + ((6 * tris + 15) & ~15u);
    const uint32_t scratch = sp2 + 32;

    cull_0f008 c;
    c.vbase = (in5[3] << 4) + outp;
    /* the winding flag (ctx byte 6's high nibble 0 or 2) flips the area's sign; mode 2 flips it back */
    const uint32_t b6 = (w_of(psl_rotqby(q0, 3), 0) & 255u) >> 4;
    const uint32_t flip = ((b6 == 2 || b6 == 0) ? 0x80000000u : 0u) ^ (mode == 2 ? 0u : 0x80000000u);
    c.flip = vdupq_n_u32(flip);
    c.mode = vdupq_n_u32(mode == 1 ? 0u : 0xFFFFFFFFu);
    c.nopix_on = (psl_rotqby(q130, 10)[3] & 0xFFu) != 0;
    c.pad = vdupq_n_u32((tris & 3) ? 15u >> (tris & 3) : 0u);

    /* The shuffles of the indices (two quadwords a group, 24 bytes: the job alternates by 8 bytes), the compaction and
     * shift tables (read where the job reads them, as it does). */
    const ps_spu_vec ident = psl_lq(ls, 0x12AF0u + rel), toggle = psl_lq(ls, 0x12700u + rel);
    const ps_spu_vec pat[2] = {ident, psl_xor(ident, toggle)};
    const uint32_t t66 = base + 0xE050u, t67 = base + 0xE150u, t68 = base + 0xE250u;
    const ps_spu_vec all = psl_splat32(0xFFFFFFFFu);
    const ps_spu_vec lim = psl_a(psl_splat32(end), psl_lq(ls, 0x126F0u + rel));

    /* The job's loop, group by group in its order of loads and stores: iteration k loads group k's indices, stores the
     * packed indices of group k - 3 (none kept for the two before the first), loads group k - 1's vertices. */
    ps_spu_vec ring56[4], ring55[4];
    unsigned ringmask[4];
    ring56[0] = psl_shufb(psl_lq(ls, idx), psl_lq(ls, idx + 16), pat[0]);
    ring55[0] = psl_shufb(psl_lq(ls, idx + 16), psl_lq(ls, idx + 16), pat[0]);
    uint32_t pos = idx, kept6 = 0, align = 0, total = 0;
    for (uint32_t k = 1; k <= groups + 2; k++) {
        if (k < groups) {
            const uint32_t off = 24 * k;
            const ps_spu_vec q1 = psl_lq(ls, idx + off), q2 = psl_lq(ls, idx + 16 + off);
            ring56[k & 3] = psl_shufb(q1, q2, pat[k & 1]);
            ring55[k & 3] = psl_shufb(q2, q2, pat[k & 1]);
        }
        /* the store of group k - 3 */
        {
            const int virt = k < 3;
            const uint32_t g = k - 3;
            const unsigned m = virt ? 15u : ringmask[g & 3];
            const ps_spu_vec i7 = virt ? psl_splat32(0) : ring56[g & 3], i6 = virt ? psl_splat32(0) : ring55[g & 3];
            pos += kept6;
            const uint32_t kept = 4u - (uint32_t)__builtin_popcount(m);
            const ps_spu_vec p66 = psl_lq(ls, t66 + 16 * m), p67 = psl_lq(ls, t67 + 16 * m);
            const ps_spu_vec d10 = psl_shufb(i7, i6, p66), d16 = psl_shufb(i6, i6, p67);
            const ps_spu_vec shift = psl_lq(ls, t68 + 16 * align);
            const ps_spu_vec sel = psl_selb(ident, shift, psl_rotqmby(all, align));
            psl_stq(ls, pos, psl_shufb(psl_lq(ls, pos), d10, sel));
            const ps_spu_vec over = psl_cgt(psl_splat32(pos), lim);
            const ps_spu_vec at = psl_selb(psl_splat32(pos), psl_splat32(scratch), over);
            psl_stq(ls, at[3] + 0x10, psl_shufb(d10, d16, shift));
            psl_stq(ls, at[2] + 0x20, psl_shufb(d16, d16, shift));
            kept6 = (kept * 6) & 0xFFFFu;
            align = (align + kept6) & 15u;
            total += kept;
        }
        if (k - 1 < groups) {
            const uint32_t g = k - 1;
            ringmask[g & 3] = cull_group_0f008(ls, &c, ring56[g & 3], ring55[g & 3], g == groups - 1);
        }
    }

    /* The count of indices kept to the halfword at ctx + 0x17E (where the job's chd puts it), the new end to ctx + 0x58,
     * then the frame undone. */
    const uint32_t n = 3 * total;
    psl_stq(ls, s + 0x170, psl_shufb(psl_splat32(n), psl_lq(ls, s + 0x170), psl_cmask(sp2 + 14, 2)));
    const uint32_t new_end = (w_of(psl_lq(ls, s + 0x50), 0) + 2 * n + 15) & ~15u;
    psl_stq(ls, s + 0x50, psl_shufb(psl_splat32(new_end), psl_lq(ls, s + 0x50), psl_cmask(sp2 + 8, 4)));
    psl_st(ctx, 0, psl_lq(ls, sp + 0x10));
    psl_st(ctx, 1, in1);
    psl_st(ctx, 3, psl_splat32(n));
    psl_st(ctx, 80, psl_lq(ls, sp - 0x10));
    psl_st(ctx, 81, psl_lq(ls, sp - 0x20));
    psl_st(ctx, 82, psl_lq(ls, sp - 0x30));
    psl_st(ctx, 126, psl_lq(ls, sp - 0x40));
    ctx->pc = pref(ctx, 0) & PS_SPU_PC_MASK;
    return 1;
}
