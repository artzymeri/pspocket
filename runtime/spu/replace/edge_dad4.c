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

/* ===================================================================================================================
 * Loop 0x080F8 of 0x07A38 (to its exit at 0x082E8): matrix-palette skinning with four bones a vertex, two vertices an
 * iteration. For each vertex, its 8 bytes in the bone stream are (weight, bone) four times; a bone byte 0xFF at the
 * first place stands for (weight 255, bone 0, then zeros). With the weights w_j = fm(cuflt(byte), 1/255) and the bones'
 * three rows of 16 bytes (at the three row bases + 48 * bone):
 *   R_r  = fma(w3, M3_r, fma(w2, M2_r, fma(w1, M1_r, fm(w0, M0_r))))     rows r = 0, 1, 2
 *   R0'  = R0 with its last word 1.0, R1' and R2' with it 0: stored (48 bytes a vertex) for the normals that follow
 *   p'   = fma(p.zxyw, R2', fma(p.yzxw, R1', fma(p, R0', (R0.w, R1.w, R2.w, 0))))  over the position, in place
 * The loop is software-pipelined (the next pair's bones and positions read ahead): the native one reads each pair as
 * the job does, from the bone stream at $49 - 16 and the positions at $48 - 32 on, and leaves what the code after the
 * loop reads ($86, the output pointer, advanced; spu-live: nothing else it writes is read again).
 * =================================================================================================================== */

/* The registers the job sets before the loop and the loop only reads (shuffle controls, masks, 1/255, 1.0), checked. */
static int skin_invariants_hold(const SPUContext* ctx)
{
    static const struct {
        uint8_t r;
        uint32_t w[4];
    } k[] = {
        {81, {0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u}}, {93, {0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu}},
        {94, {0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu}}, {95, {0x04050607u, 0x04050607u, 0x04050607u, 0x04050607u}},
        {77, {0x80808000u, 0x80808002u, 0x80808004u, 0x80808006u}}, {102, {0x80808008u, 0x8080800Au, 0x8080800Cu, 0x8080800Eu}},
        {36, {0x04050607u, 0x08090A0Bu, 0x00010203u, 0x0C0D0E0Fu}}, {82, {0x08090A0Bu, 0x00010203u, 0x04050607u, 0x0C0D0E0Fu}},
        {70, {0x80808080u, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x80808080u}}, {69, {0x1C1D1E1Fu, 0x04050607u, 0x08090A0Bu, 0x80808080u}},
        {84, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u}}, {97, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
        {91, {0x3B808080u, 0x3B808080u, 0x3B808080u, 0x3B808080u}}, {55, {0xFF000000u, 0x00000000u, 0xFF000000u, 0x00000000u}},
        {54, {0x01010101u, 0x01010101u, 0x09090909u, 0x09090909u}},
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        for (int w = 0; w < 4; w++)
            if (ctx->gpr[k[i].r].u32[3 - w] != k[i].w[w]) return 0;
    return 1;
}

/* A quadword of local store as four host words (lane i = guest word i) and back. */
static inline uint32x4_t ld_words(const uint8_t* ls, uint32_t addr)
{
    return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + (addr & PS_SPU_QW_MASK))));
}
static inline void st_words(uint8_t* ls, uint32_t addr, uint32x4_t v)
{
    vst1q_u8(ls + (addr & PS_SPU_QW_MASK), vrev32q_u8(vreinterpretq_u8_u32(v)));
}

/* The palette as the loop uses it: each row of each bone named (host words) and its lanes that are not zeros (an fma
 * with a zero factor is its addend); the weights by byte (fm(cuflt(b), 1/255), as the job computes them). */
typedef struct {
    uint32x4_t row[3][256], nz[3][256];
    uint32_t weight[256];
} skin_palette;

/* One vertex: its four (weight, bone) bytes, its position at `pos` (transformed in place), its three rows to `mat`. */
static inline __attribute__((always_inline)) void skin_vertex(uint8_t* ls, const skin_palette* pal, const uint8_t q[8],
    uint32_t mat, uint32_t pos)
{
    uint32x4_t r[3];
    {
        const unsigned b = q[1];
        if (q[0]) {
            const float32x4_t w = vreinterpretq_f32_u32(vdupq_n_u32(pal->weight[q[0]]));
            for (int k = 0; k < 3; k++)
                r[k] = vandq_u32(vreinterpretq_u32_f32(vmulq_f32(w, vreinterpretq_f32_u32(pal->row[k][b]))), pal->nz[k][b]);
        } else {
            r[0] = r[1] = r[2] = vdupq_n_u32(0); /* fm by a zero weight */
        }
    }
    for (int j = 1; j < 4; j++) {
        if (!q[2 * j]) continue; /* fma by a zero weight: its addend */
        const unsigned b = q[2 * j + 1];
        const float32x4_t w = vreinterpretq_f32_u32(vdupq_n_u32(pal->weight[q[2 * j]]));
        for (int k = 0; k < 3; k++) {
            const float32x4_t f = vfmaq_f32(vreinterpretq_f32_u32(r[k]), w, vreinterpretq_f32_u32(pal->row[k][b]));
            r[k] = vbslq_u32(pal->nz[k][b], vreinterpretq_u32_f32(f), r[k]);
        }
    }
    const uint32x4_t r0 = vsetq_lane_u32(0x3F800000u, r[0], 3), r1 = vsetq_lane_u32(0, r[1], 3), r2 = vsetq_lane_u32(0, r[2], 3);
    st_words(ls, mat, r0);
    st_words(ls, mat + 16, r1);
    st_words(ls, mat + 32, r2);
    uint32x4_t t = vdupq_n_u32(0);
    t = vcopyq_laneq_u32(t, 0, r[0], 3);
    t = vcopyq_laneq_u32(t, 1, r[1], 3);
    t = vcopyq_laneq_u32(t, 2, r[2], 3);
    const uint32x4_t p = ld_words(ls, pos);
    const uint8x16_t yzxw = {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 12, 13, 14, 15};
    const uint8x16_t zxyw = {8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};
    const uint32x4_t py = vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(p), yzxw));
    const uint32x4_t pz = vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(p), zxyw));
    uint32x4_t o = psn_fma(p, r0, t);
    o = psn_fma(py, r1, o);
    o = psn_fma(pz, r2, o);
    st_words(ls, pos, o);
}

int ps_edge_dad4_080f8(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t count = pref(ctx, 46);
    if (count == 0 || (count & 1) || count > 0x10000u) return 0;
    if (!skin_invariants_hold(ctx)) return 0;
    const uint32_t base[3] = {pref(ctx, 38), pref(ctx, 37), pref(ctx, 35)}; /* the rows' bases */
    uint32_t bones = pref(ctx, 49) - 16, positions = pref(ctx, 48) - 32, mat = pref(ctx, 47);
    const uint32_t out = pref(ctx, 86);
    if ((out & PS_SPU_QW_MASK) != (positions & PS_SPU_QW_MASK)) return 0; /* the job transforms in place */
    /* The job reads a pair's bones and positions before it writes the pair before: here each vertex reads and writes
     * in turn, the same only while the matrices written, the positions and the bone rows read do not overlap. */
    const uint32_t m0 = mat & PS_SPU_QW_MASK, p0 = positions & PS_SPU_QW_MASK, b0 = bones & PS_SPU_QW_MASK;
    const uint32_t mlen = 48u * count, plen = 16u * count, blen = 8u * count + 16;
    if (!in_ls(m0, mlen) || !in_ls(p0, plen) || !in_ls(b0, blen)) return 0;
    /* the largest bone named (a vertex whose first bone is 0xFF names bone 0 only), sixteen bytes at a time */
    unsigned top;
    {
        const uint8x16_t odd = vreinterpretq_u8_u16(vdupq_n_u16(0xFF00u)); /* bytes 1, 3, 5, ... in memory order */
        const uint8x16_t first = {1, 1, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 9, 9};
        uint8x16_t m = vdupq_n_u8(0);
        for (uint32_t i = 0; i < count / 2; i++) {
            const uint8x16_t q = vld1q_u8(ls + ((bones + 16 * i) & PS_SPU_QW_MASK));
            const uint8x16_t one = vceqq_u8(vqtbl1q_u8(q, first), vdupq_n_u8(0xFF));
            m = vmaxq_u8(m, vbicq_u8(vandq_u8(q, odd), one));
        }
        top = vmaxvq_u8(m);
    }
#define OVERLAP(a, alen, b, blen) ((a) < (b) + (blen) && (b) < (a) + (alen))
    if (OVERLAP(m0, mlen, p0, plen) || OVERLAP(m0, mlen, b0, blen) || OVERLAP(p0, plen, b0, blen)) return 0;
    const uint32_t lo = base[0] < base[2] ? base[0] : base[2], hi = (base[0] > base[2] ? base[0] : base[2]) + (top + 1) * 48u;
    if ((lo & ~PS_SPU_LS_MASK) || hi > PS_SPU_LS_SIZE) return 0;
    if (OVERLAP(m0, mlen, lo, hi - lo) || OVERLAP(p0, plen, lo, hi - lo)) {
        /* the palette's span reaches an output: the rows of the bones named must not */
        uint64_t used[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* v = ls + ((bones + 8 * i) & PS_SPU_LS_MASK);
            for (int j = 0; j < 4; j++) {
                const unsigned b = v[1] == 0xFF ? 0u : v[2 * j + 1];
                used[b >> 6] |= 1ull << (b & 63);
            }
        }
        for (unsigned b = 0; b <= top; b++) {
            if (!((used[b >> 6] >> (b & 63)) & 1)) continue;
            for (int k = 0; k < 3; k++) {
                const uint32_t row = (base[k] + 48u * b) & PS_SPU_QW_MASK;
                if (OVERLAP(m0, mlen, row, 16u) || OVERLAP(p0, plen, row, 16u)) return 0;
            }
        }
    }
#undef OVERLAP
    /* The rows of the bones named, the weights by byte. */
    static _Thread_local skin_palette pal;
    for (unsigned b = 0; b <= top; b++)
        for (int k = 0; k < 3; k++) {
            pal.row[k][b] = ld_words(ls, base[k] + 48u * b);
            pal.nz[k][b] = psn_nz(pal.row[k][b]);
        }
    static _Thread_local int weights_made;
    if (!weights_made) { /* the same for every call (the SPU FP environment) */
        for (unsigned v = 1; v < 256; v++) /* weight 0 is never used: its fma is left out */
            pal.weight[v] = psl_fm(psl_splat32(ps_spu_u32f((float)v)), psl_splat32(0x3B808080u))[3];
        weights_made = 1;
    }
    for (uint32_t pair = 0; pair < count / 2; pair++) {
        uint8_t q[16];
        memcpy(q, ls + (bones & PS_SPU_QW_MASK), 16);
        for (int h = 0; h < 2; h++)
            if (q[8 * h + 1] == 0xFF) { /* one bone: weight 255 for bone 0 */
                q[8 * h] = 0xFF;
                memset(q + 8 * h + 1, 0, 7);
            }
        skin_vertex(ls, &pal, q, mat, positions);
        skin_vertex(ls, &pal, q + 8, mat + 48, positions + 16);
        bones += 16;
        positions += 32;
        mat += 96;
    }
    psl_st(ctx, 86, psl_a(psl_ld(ctx, 86), psl_splat32(32u * (count / 2))));
    ctx->pc = (0x082E8u + ctx->reloc) & PS_SPU_PC_MASK;
    return 1;
}

/* ===================================================================================================================
 * Loop 0x0B850 of the decompression fragment 0x0B778 (to its exit at 0x0B9B0): four fixed-point vertex streams to
 * floats. The fragment's code before the loop reads the attribute's format (component widths at most 23 bits; wider
 * ones take another path) and sets up, constant through the loop: a byte pattern from two halfword patterns ($28, $27)
 * by a shuffle control ($112), to which each stream adds its pointer's offset in its quadword; shift counts ($26); an
 * exponent ($30) put in under a mask ($25); a bias ($29). For each block (counter $32 up to $56):
 *   stream k's pointer = word 0 of $44, $43, $42, $45 + mpya(counter, $51, $50), advancing by word 0 of $35
 *   output position    = word 0 of $31 (16 more each block) + $49, advancing by $41; word k of it, or of the scratch
 *                        quadword $37 where it is past the limit $39, is where stream k's quadword goes
 *   value              = fs(selb(rotm(shufb(lq(p), lq(p + 16), pattern(p & 15)), $26), $30, $25), $29)
 * until word 0 of the output position is past the limit (that vertex's values are not stored). The loop is
 * software-pipelined: each vertex's values are stored at the start of the next iteration, after the first one's zeros to
 * the scratch quadword. The native loop stores each vertex's values after it reads them, in the same order, so local
 * store sees the same reads and writes even where outputs and inputs overlap. Registers: spu-live, nothing it writes is
 * read after the exit; it writes none.
 * =================================================================================================================== */

/* The shuffle for each of the 16 offsets of a stream's pointer in its quadword, as a table lookup on the two quadwords
 * in local store order (the 32 bytes the job's shufb picks from) giving host words (lane i = guest word i); the special
 * controls (10xxxxxx, 110xxxxx, 111xxxxx: 0x00, 0xFF, 0x80) out of the table's range and or'ed in. Made from the
 * patterns' registers, kept while they are the same. */
typedef struct {
    ps_spu_vec key[3]; /* $27, $28, $112 */
    int made;
    uint8x16_t idx[16], orr[16];
} decode_shuffles;

static const decode_shuffles* decode_shuffles_for(const SPUContext* ctx)
{
    static _Thread_local decode_shuffles s;
    const ps_spu_vec r27 = psl_ld(ctx, 27), r28 = psl_ld(ctx, 28), r112 = psl_ld(ctx, 112);
    if (s.made && !memcmp(&s.key[0], &r27, 16) && !memcmp(&s.key[1], &r28, 16) && !memcmp(&s.key[2], &r112, 16))
        return &s;
    for (unsigned off = 0; off < 16; off++) {
        const ps_spu_vec o = psl_splat8((uint8_t)off);
        const psv_u8 pat = (psv_u8)psl_shufb(psl_ah(r28, o), psl_ah(r27, o), r112);
        uint8_t idx[16], orr[16];
        for (int h = 0; h < 16; h++) {
            const int g = 4 * (h / 4) + 3 - h % 4; /* the guest byte host byte h of a host word holds */
            const uint8_t c = pat[15 - g];          /* D-004: guest byte g is the register's host byte 15 - g */
            idx[h] = (c & 0x80) ? 0xFF : (uint8_t)(c & 0x1F);
            orr[h] = !(c & 0x80) ? 0x00 : (c & 0xE0) == 0xE0 ? 0x80 : (c & 0xE0) == 0xC0 ? 0xFF : 0x00;
        }
        s.idx[off] = vld1q_u8(idx);
        s.orr[off] = vld1q_u8(orr);
    }
    s.key[0] = r27;
    s.key[1] = r28;
    s.key[2] = r112;
    s.made = 1;
    return &s;
}

typedef struct {
    const decode_shuffles* sh;
    int32x4_t shift;     /* -(rotm count) */
    uint32x4_t mask, exponent;
    float32x4_t bias;    /* clamped, as fs reads its second operand */
} decode_0b850;

static inline __attribute__((always_inline)) uint32x4_t decode_qw(const uint8_t* ls, const decode_0b850* d, uint32_t p)
{
    const uint8x16x2_t t = {{vld1q_u8(ls + (p & PS_SPU_QW_MASK)), vld1q_u8(ls + ((p + 16) & PS_SPU_QW_MASK))}};
    const unsigned o = p & 15;
    uint32x4_t v = vreinterpretq_u32_u8(vorrq_u8(vqtbl2q_u8(t, d->sh->idx[o]), d->sh->orr[o]));
    v = vshlq_u32(v, d->shift); /* rotm: right by (0 - n) & 63, nothing left from 32 on (ushl by -32 or less) */
    v = vbslq_u32(d->mask, d->exponent, v);
    return vreinterpretq_u32_f32(vsubq_f32(vreinterpretq_f32_u32(v), d->bias));
}

int ps_edge_dad4_0b850(SPUContext* ctx, uint8_t* ls)
{
    /* the controls the code before the loop sets: word 0 into every word ($34), byte 3 into every byte ($33) */
    for (int w = 0; w < 4; w++)
        if (ctx->gpr[34].u32[w] != 0x00010203u || ctx->gpr[33].u32[w] != 0x03030303u) return 0;
    const uint32_t blocks = pref(ctx, 56) - pref(ctx, 32);
    if (blocks > 0x1000u) return 0;
    const ps_spu_vec r49 = psl_ld(ctx, 49);
    const int32_t step = (int32_t)pref(ctx, 41), limit = (int32_t)pref(ctx, 39);
    const uint32_t first = pref(ctx, 31);
    /* each block's loop ends (word 0 of the position passes the limit without wrapping), in a bounded number of
     * iterations: else the lifted loop runs */
    uint64_t work = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        const int32_t s0 = (int32_t)(first + 16u * b + w_of(r49, 0));
        if (s0 > limit) continue;
        if (step <= 0) return 0;
        const int64_t n = ((int64_t)limit - s0) / step + 1;
        if ((int64_t)s0 + n * step > INT32_MAX) return 0;
        work += (uint64_t)n;
        if (work > (1u << 20)) return 0;
    }
    /* the loop's constants, host words (lane i = guest word i) */
    decode_0b850 d;
    d.sh = decode_shuffles_for(ctx);
    const uint32x4_t count = psn_rev4((uint32x4_t)psl_ld(ctx, 26));
    d.shift = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(vsubq_u32(vdupq_n_u32(0), count), vdupq_n_u32(63))));
    d.mask = psn_rev4((uint32x4_t)psl_ld(ctx, 25));
    d.exponent = psn_rev4((uint32x4_t)psl_ld(ctx, 30));
    d.bias = vreinterpretq_f32_u32(psn_rev4((uint32x4_t)psl_clamp(psl_ld(ctx, 29))));
    const uint32x4_t offsets = psn_rev4((uint32x4_t)r49), steps = psn_rev4((uint32x4_t)psl_ld(ctx, 41));
    const int32x4_t limits = vreinterpretq_s32_u32(psn_rev4((uint32x4_t)psl_ld(ctx, 39)));
    const uint32x4_t scratch = psn_rev4((uint32x4_t)psl_ld(ctx, 37));
    const uint32_t stride = pref(ctx, 35), counter = pref(ctx, 32), scale = pref(ctx, 51), add = pref(ctx, 50);
    const uint32_t stream[4] = {pref(ctx, 44), pref(ctx, 43), pref(ctx, 42), pref(ctx, 45)};
    const uint32x4_t zero = vdupq_n_u32(0);
    for (uint32_t b = 0; b < blocks; b++) {
        st_words(ls, vgetq_lane_u32(scratch, 0), zero); /* the pipeline's first stores */
        const uint32_t m = (uint32_t)((int32_t)(int16_t)(counter + b) * (int32_t)(int16_t)scale) + add; /* mpya */
        uint32_t p0 = stream[0] + m, p1 = stream[1] + m, p2 = stream[2] + m, p3 = stream[3] + m;
        uint32x4_t pos = vaddq_u32(vdupq_n_u32(first + 16u * b), offsets);
        for (;;) {
            const uint32x4_t past = vcgtq_s32(vreinterpretq_s32_u32(pos), limits);
            if (vgetq_lane_u32(past, 0)) break;
            const uint32x4_t at = vbslq_u32(past, scratch, pos);
            const uint32x4_t v0 = decode_qw(ls, &d, p0), v1 = decode_qw(ls, &d, p1);
            const uint32x4_t v2 = decode_qw(ls, &d, p2), v3 = decode_qw(ls, &d, p3);
            st_words(ls, vgetq_lane_u32(at, 0), v0);
            st_words(ls, vgetq_lane_u32(at, 1), v1);
            st_words(ls, vgetq_lane_u32(at, 2), v2);
            st_words(ls, vgetq_lane_u32(at, 3), v3);
            p0 += stride;
            p1 += stride;
            p2 += stride;
            p3 += stride;
            pos = vaddq_u32(pos, steps);
        }
    }
    ctx->pc = (0x0B9B0u + ctx->reloc) & PS_SPU_PC_MASK;
    return 1;
}

/* ===================================================================================================================
 * The decompression fragment 0x0AC10 (to 0x0BF60, where its blocks are done): unit normals (or tangents) from two
 * stored components, a block of vertices at a time ($47 from 0 up to $56). For each vertex, four streams (two quadword
 * pointers each: $44/$55, $42/$53, $43/$54, $45/$52, at the block's offset mpya(block, $51, $50) plus the vertex's
 * stride $35 times its index) are each
 *   s = selb($65, rotm(shufb(lq(p1), lq(p2), pattern), $67), $66)     (one pattern a block: $72 plus its offset's
 *                                                                      bytes by $61, then by the image's control at
 *                                                                      0x12A20)
 * and, with the shuffle $107 pairing the first and third and the second and fourth,
 *   y = fma(shufb(s1, s3, $107), $93, $92)               two components a lane pair, scaled and biased
 *   n = fm(t, fi(t, frsqest(t))) with t = fs($114, fa(shlqbyi(fm(y, y), 4), fm(y, y)))     the third: sqrt(1 - a^2 - b^2)
 *   out_k = shufb(selb(n, T_k, $108), y, selb($87, T_k, $86) (+ $99 for the second and fourth))
 * where T_k is the quadword of the table at the word at $sp + 0x30 (read each block) indexed by word 0 of s_k: where
 * the third component goes and its sign. The four outputs go to word k of the output position (16 a block plus $49,
 * advancing by $41), or to the scratch quadword $37 where that is past the limit $39; the block ends after the vertex
 * whose word 0 is past it (stored to the scratch). The job's loop runs three vertices deep: a vertex's streams are read
 * two vertices ahead and its table quadwords one ahead of its stores. The native loop keeps that order of reads and
 * writes (exact where outputs overlap inputs) and computes each vertex at once, in the job's operations (pspocket/spu/
 * lifted.h helpers) in the register order. Nothing it writes is read after 0x0BF60 (spu-live): its contract is local
 * store and the pc.
 * =================================================================================================================== */

/* Vectors in local-store word order here (lane i = guest word i: ld_words), the registers' constants turned to it once,
 * shuffles by psn_shufb_w (controls known ahead as lookups). */
typedef struct {
    uint32x4_t s[4]; /* the four streams decoded: 0 = $44/$55, 1 = $42/$53, 2 = $43/$54, 3 = $45/$52 */
    uint32x4_t y[2]; /* fma'd pairs: 0 = streams 0 and 2 ($23), 1 = streams 1 and 3 ($22) */
} normal_vertex;

typedef struct {
    uint32_t p1[4], p2[4]; /* the streams' pointers (word 0) */
    psn_shufb_key_t pattern, pair;
    int32x4_t shift; /* -(rotm count) */
    uint32x4_t r65, r66, r93, r92, r93nz, r114, r108, r87, r86, r99;
    psn_frsqest_tables rsqrt;
} normal_0ac10;

static inline uint32x4_t reg_w(const SPUContext* ctx, int r) { return psn_rev4((uint32x4_t)psl_ld(ctx, r)); }

static inline __attribute__((always_inline)) void normal_read(const uint8_t* ls, const normal_0ac10* n, uint32_t x,
    normal_vertex* v)
{
    for (int k = 0; k < 4; k++) {
        const uint8x16x2_t t = {{vld1q_u8(ls + ((n->p1[k] + x) & PS_SPU_QW_MASK)), vld1q_u8(ls + ((n->p2[k] + x) & PS_SPU_QW_MASK))}};
        /* the pattern's lookup reads the quadwords as local store has them: its indices are in guest byte order */
        const uint32x4_t raw = vreinterpretq_u32_u8(vorrq_u8(vqtbl2q_u8(t, n->pattern.idx), n->pattern.orr));
        v->s[k] = vbslq_u32(n->r66, vshlq_u32(raw, n->shift), n->r65);
    }
    for (int i = 0; i < 2; i++) {
        const uint32x4_t p = psn_shufb_k(v->s[i], v->s[i + 2], n->pair);
        v->y[i] = psn_fma_n(p, n->r93, n->r92, vandq_u32(psn_nz(p), n->r93nz));
    }
}

/* sqrt(1 - a^2 - b^2) as the job computes it, for each pair of lanes of y: fm, fa of the pair, fs from $114, then
 * fm(t, fi(t, frsqest(t))) */
static inline __attribute__((always_inline)) uint32x4_t normal_third(const normal_0ac10* n, uint32x4_t y)
{
    const uint32x4_t sq = psn_fm(y, y);
    const uint32x4_t sum = vreinterpretq_u32_f32(
        vaddq_f32(vreinterpretq_f32_u32(vextq_u32(sq, vdupq_n_u32(0), 1)), vreinterpretq_f32_u32(sq))); /* shlqbyi 4 */
    const uint32x4_t t = vreinterpretq_u32_f32(vsubq_f32(vreinterpretq_f32_u32(n->r114), vreinterpretq_f32_u32(psn_clamp(sum))));
    return psn_fm(t, (uint32x4_t)psl_fi((ps_spu_vec)t, (ps_spu_vec)psn_frsqest_t(t, &n->rsqrt)));
}

int ps_edge_dad4_0ac10(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t blocks = pref(ctx, 56); /* $47 counts from 0 */
    if (blocks > 0x1000u) return 0;
    const ps_spu_vec r49 = psl_ld(ctx, 49), r41 = psl_ld(ctx, 41), r39 = psl_ld(ctx, 39);
    const int32_t step = (int32_t)w_of(r41, 0), limit = (int32_t)w_of(r39, 0);
    /* each block ends (word 0 of the position passes the limit without wrapping) within a bound */
    uint64_t work = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        const int32_t s0 = (int32_t)(16u * b + w_of(r49, 0));
        uint64_t n = 1;
        if (s0 <= limit) {
            if (step <= 0) return 0;
            const int64_t j = ((int64_t)limit - s0) / step + 1;
            if ((int64_t)s0 + j * step > INT32_MAX) return 0;
            n = (uint64_t)j + 1;
        }
        work += n;
        if (work > (1u << 20)) return 0;
    }
    normal_0ac10 n;
    static const uint8_t first[4] = {44, 42, 43, 45}, second[4] = {55, 53, 54, 52};
    for (int k = 0; k < 4; k++) {
        n.p1[k] = pref(ctx, first[k]);
        n.p2[k] = pref(ctx, second[k]);
    }
    n.shift = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(vsubq_u32(vdupq_n_u32(0), reg_w(ctx, 67)), vdupq_n_u32(63))));
    n.r65 = reg_w(ctx, 65);
    n.r66 = reg_w(ctx, 66);
    n.pair = psn_shufb_key(reg_w(ctx, 107));
    n.r93 = reg_w(ctx, 93);
    n.r93nz = psn_nz(n.r93);
    n.r92 = reg_w(ctx, 92);
    n.r114 = reg_w(ctx, 114);
    n.r108 = reg_w(ctx, 108);
    n.r87 = reg_w(ctx, 87);
    n.r86 = reg_w(ctx, 86);
    n.r99 = reg_w(ctx, 99);
    n.rsqrt = psn_frsqest_load();
    const uint32x4_t pos0 = reg_w(ctx, 49), steps = reg_w(ctx, 41), limits = reg_w(ctx, 39), scratch = reg_w(ctx, 37);
    const ps_spu_vec r51 = psl_ld(ctx, 51), r50 = psl_ld(ctx, 50), r61 = psl_ld(ctx, 61), r72 = psl_ld(ctx, 72);
    const uint32_t stride = pref(ctx, 35), sp = pref(ctx, 1);
    for (uint32_t b = 0; b < blocks; b++) {
        const uint32_t table = psl_u32p(psl_lq(ls, sp + 0x30));
        const ps_spu_vec control = psl_lq(ls, 0x12A20u + ctx->reloc);
        const ps_spu_vec m = psl_mpya(psl_splat32(b), r51, r50);
        const ps_spu_vec t = psl_a(r72, psl_shufb(psl_and(m, psl_splat32(15)), m, r61));
        /* the block's pattern on the quadwords as local store has them: guest byte order, so (c & 31) unchanged */
        {
            const uint32x4_t c = psn_rev4((uint32x4_t)psl_shufb(t, t, control));
            const psn_shufb_key_t k = psn_shufb_key(c);
            /* psn_shufb_key indexes {a, b} in local-store word order (guest byte g at g ^ 3); these quadwords are in
             * guest byte order: the ^ 3 undone on the bytes picked */
            n.pattern.idx = veorq_u8(k.idx, vandq_u8(vcltq_u8(k.idx, vdupq_n_u8(32)), vdupq_n_u8(3)));
            n.pattern.orr = k.orr;
        }
        uint32x4_t pos = vaddq_u32(vdupq_n_u32(16u * b), pos0);
        uint32_t x = psl_u32p(m);
        normal_vertex v[3]; /* vertex j, j + 1, j + 2 by j % 3 */
        normal_read(ls, &n, x, &v[0]);
        normal_read(ls, &n, x + stride, &v[1]);
        uint32x4_t tq[4]; /* vertex j's table quadwords */
        for (int k = 0; k < 4; k++) tq[k] = ld_words(ls, table + vgetq_lane_u32(v[0].s[k], 0));
        for (uint32_t j = 0;; j++) {
            const normal_vertex* cur = &v[j % 3];
            const uint32x4_t n0 = normal_third(&n, cur->y[0]), n1 = normal_third(&n, cur->y[1]);
            const uint32x4_t past = vcgtq_s32(vreinterpretq_s32_u32(pos), vreinterpretq_s32_u32(limits));
            const uint32x4_t at = vbslq_u32(past, scratch, pos);
            /* streams 0 and 2 with the first pair, 1 and 3 with the second: table quadwords by stream 0, 2, 1, 3 */
            const uint32x4_t o0 = psn_shufb_w(vbslq_u32(n.r108, tq[0], n0), cur->y[0], vbslq_u32(n.r86, tq[0], n.r87));
            const uint32x4_t o1 = psn_shufb_w(vbslq_u32(n.r108, tq[2], n0), cur->y[0],
                vaddq_u32(vbslq_u32(n.r86, tq[2], n.r87), n.r99));
            const uint32x4_t o2 = psn_shufb_w(vbslq_u32(n.r108, tq[1], n1), cur->y[1], vbslq_u32(n.r86, tq[1], n.r87));
            const uint32x4_t o3 = psn_shufb_w(vbslq_u32(n.r108, tq[3], n1), cur->y[1],
                vaddq_u32(vbslq_u32(n.r86, tq[3], n.r87), n.r99));
            st_words(ls, vgetq_lane_u32(at, 0), o0);
            st_words(ls, vgetq_lane_u32(at, 1), o1);
            st_words(ls, vgetq_lane_u32(at, 2), o2);
            st_words(ls, vgetq_lane_u32(at, 3), o3);
            if (vgetq_lane_u32(past, 0)) break;
            normal_read(ls, &n, x + 2 * stride, &v[(j + 2) % 3]);
            const normal_vertex* next = &v[(j + 1) % 3];
            for (int k = 0; k < 4; k++) tq[k] = ld_words(ls, table + vgetq_lane_u32(next->s[k], 0));
            x += stride;
            pos = vaddq_u32(pos, steps);
        }
    }
    ctx->pc = (0x0BF60u + ctx->reloc) & PS_SPU_PC_MASK;
    return 1;
}

/* ===================================================================================================================
 * 0x084AC of 0x07A38 (to 0x088F8): the skinned vertices' normals, and tangents, through the rows the skinning loop
 * (0x080F8) left on the stack, by the count in $64 (a multiple of 4), four vertices an iteration:
 *   v' = fma(v.zxyw, R2, fma(v.yzxw, R1, fm(v, R0)))          rows R0-R2 of the vertex, 48 bytes a vertex
 * in place: normals at $62, tangents at $65, by the mode in $66 (1: normals, 2: both, 3: another path, declined;
 * anything else: neither). The first four vertices' rows are at $39, the next ones' at $73 on. The job's loop reads the
 * next four vertices' normals and rows before it stores this iteration's (and the tangents just before), in the orders
 * kept here, so local store sees the same reads and writes. After it the job reads the pointers $62 and $65 advanced
 * (spu-live: nothing else the code here writes).
 * =================================================================================================================== */

static inline __attribute__((always_inline)) uint32x4_t rows_xform(uint32x4_t v, const uint32x4_t r[3])
{
    const uint8x16_t yzxw = {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 12, 13, 14, 15};
    const uint8x16_t zxyw = {8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};
    const uint32x4_t vy = vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(v), yzxw));
    const uint32x4_t vz = vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(v), zxyw));
    uint32x4_t o = psn_fm(v, r[0]);
    o = psn_fma(vy, r[1], o);
    return psn_fma(vz, r[2], o);
}

int ps_edge_dad4_084ac(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t mode = pref(ctx, 66);
    if (mode == 3) return 0;
    const uint32_t exit = (0x088F8u + ctx->reloc) & PS_SPU_PC_MASK;
    if (mode != 1 && mode != 2) { /* neither: what the code reads on the way is dead */
        ctx->pc = exit;
        return 1;
    }
    const uint32_t count = pref(ctx, 64);
    if (count == 0 || (count & 3) || count > 0x10000u) return 0;
    /* the shuffles to (y, z, x, w) and (z, x, y, w) */
    static const uint32_t yzx[4] = {0x04050607u, 0x08090A0Bu, 0x00010203u, 0x0C0D0E0Fu};
    static const uint32_t zxy[4] = {0x08090A0Bu, 0x00010203u, 0x04050607u, 0x0C0D0E0Fu};
    for (int w = 0; w < 4; w++)
        if (ctx->gpr[36].u32[3 - w] != yzx[w] || ctx->gpr[82].u32[3 - w] != zxy[w]) return 0;
    uint32_t normals = pref(ctx, 62), tangents = pref(ctx, 65), rows = pref(ctx, 73);
    const uint32_t first = pref(ctx, 39);
    uint32x4_t n[4], r[4][3];
    for (int i = 0; i < 4; i++) {
        n[i] = ld_words(ls, normals + 16u * (uint32_t)i);
        for (int k = 0; k < 3; k++) r[i][k] = ld_words(ls, first + 48u * (uint32_t)i + 16u * (uint32_t)k);
    }
    for (uint32_t it = 0; it < count / 4; it++) {
        uint32x4_t t[4];
        if (mode == 2)
            for (int i = 0; i < 4; i++) t[i] = ld_words(ls, tangents + 16u * (uint32_t)i);
        uint32x4_t no[4], to[4];
        for (int i = 0; i < 4; i++) {
            no[i] = rows_xform(n[i], r[i]);
            if (mode == 2) to[i] = rows_xform(t[i], r[i]);
        }
        /* the next four vertices' normals and rows, read before this iteration's stores */
        for (int i = 0; i < 4; i++) {
            n[i] = ld_words(ls, normals + 0x40u + 16u * (uint32_t)i);
            for (int k = 0; k < 3; k++) r[i][k] = ld_words(ls, rows + 48u * (uint32_t)i + 16u * (uint32_t)k);
        }
        if (mode == 2) {
            st_words(ls, normals + 0x30u, no[3]);
            st_words(ls, normals, no[0]);
            st_words(ls, normals + 0x10u, no[1]);
            st_words(ls, normals + 0x20u, no[2]);
            st_words(ls, tangents + 0x10u, to[1]);
            st_words(ls, tangents + 0x20u, to[2]);
            st_words(ls, tangents, to[0]);
            st_words(ls, tangents + 0x30u, to[3]);
            tangents += 64;
        } else {
            st_words(ls, normals + 0x20u, no[2]);
            st_words(ls, normals, no[0]);
            st_words(ls, normals + 0x10u, no[1]);
            st_words(ls, normals + 0x30u, no[3]);
        }
        normals += 64;
        rows += 192;
    }
    const uint32_t moved = 16u * count;
    psl_st(ctx, 62, psl_a(psl_ld(ctx, 62), psl_splat32(moved)));
    if (mode == 2) psl_st(ctx, 65, psl_a(psl_ld(ctx, 65), psl_splat32(moved)));
    ctx->pc = exit;
    return 1;
}

/* ===================================================================================================================
 * 0x07EF4 of 0x07A38 (to 0x082E8, where the four-bone loop also ends): skinning by one bone a vertex, two vertices an
 * iteration, $64 of them (even). Each vertex's bone is a byte of the stream at $83 (two a halfword, read as the job
 * reads them: rotqby of the quadword by the pointer, its first two bytes); with its rows A, B, C (at $100, $101, $89 +
 * 48 * bone), m = $84 (the lanes kept) and t = shufb(shufb(B, A, $70), C, $69) (the rows' last words),
 *   A' = A & m, B' = B & m, C' = selb($97, C, m)            stored for the normals (C', B', A' a vertex, 48 bytes)
 *   p' = fma(p.zxy, A', fma(p.yzx, B', fma(p, C', t)))       over the position at $86, in place (.yzx, .zxy: shufb
 *                                                              with $81 by $36 and $82)
 * The job reads the next pair's bones and positions before it stores this pair's rows (first vertex 1's A', then
 * vertex 0's C', B', A', vertex 1's C', B') and positions, and the pair's rows after the last pair's stores: kept here.
 * After it the job reads $86 (advanced) and $72 ($83 + $64), nothing else written here (spu-live).
 * =================================================================================================================== */

/* the two bones' row offsets as the job forms them: (b0 << 16 | b1) * 48 in a word, and that rotated by 16 */
static inline void one_bone_offsets(const uint8_t* ls, uint32_t a, uint32_t* o0, uint32_t* o1)
{
    const uint8_t* q = ls + (a & PS_SPU_QW_MASK);
    const uint32_t w = ((uint32_t)q[a & 15] << 16) | q[(a + 1) & 15];
    const uint32_t m = (w << 5) + (w << 4);
    *o1 = m;
    *o0 = (m << 16) | (m >> 16);
}

int ps_edge_dad4_07ef4(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t count = pref(ctx, 64);
    if (count == 0 || (count & 1) || count > 0x10000u) return 0;
    const uint32_t base[3] = {pref(ctx, 100), pref(ctx, 101), pref(ctx, 89)}; /* rows A, B, C */
    const uint32x4_t keep = reg_w(ctx, 84), r97 = reg_w(ctx, 97), r81 = reg_w(ctx, 81);
    const psn_shufb_key_t k70 = psn_shufb_key(reg_w(ctx, 70)), k69 = psn_shufb_key(reg_w(ctx, 69));
    const psn_shufb_key_t yzx = psn_shufb_key(reg_w(ctx, 36)), zxy = psn_shufb_key(reg_w(ctx, 82));
    uint32_t bones = pref(ctx, 83), pos = pref(ctx, 86), next = pref(ctx, 9), rows = pref(ctx, 67);
    uint32_t o[2];
    one_bone_offsets(ls, bones, &o[0], &o[1]);
    uint32x4_t p[2] = {ld_words(ls, pos), ld_words(ls, pos + 16)};
    for (uint32_t pair = 0; pair < count / 2; pair++) {
        uint32x4_t a[2], b[2], c[2];
        for (int v = 0; v < 2; v++) {
            a[v] = ld_words(ls, base[0] + o[v]);
            b[v] = ld_words(ls, base[1] + o[v]);
            c[v] = ld_words(ls, base[2] + o[v]);
        }
        bones += 2;
        one_bone_offsets(ls, bones, &o[0], &o[1]); /* the next pair's bones */
        const uint32x4_t np0 = ld_words(ls, next), np1 = ld_words(ls, next + 16);
        uint32x4_t a2[2], b2[2], c2[2], out[2];
        for (int v = 0; v < 2; v++) {
            a2[v] = vandq_u32(a[v], keep);
            b2[v] = vandq_u32(b[v], keep);
            c2[v] = vbslq_u32(keep, c[v], r97);
            const uint32x4_t t = psn_shufb_k(psn_shufb_k(b[v], a[v], k70), c[v], k69);
            uint32x4_t r = psn_fma(p[v], c2[v], t);
            r = psn_fma(psn_shufb_k(p[v], r81, yzx), b2[v], r);
            out[v] = psn_fma(psn_shufb_k(p[v], r81, zxy), a2[v], r);
        }
        st_words(ls, rows + 0x50, a2[1]);
        st_words(ls, rows + 0x00, c2[0]);
        st_words(ls, rows + 0x10, b2[0]);
        st_words(ls, rows + 0x20, a2[0]);
        st_words(ls, rows + 0x30, c2[1]);
        st_words(ls, rows + 0x40, b2[1]);
        st_words(ls, pos, out[0]);
        st_words(ls, pos + 16, out[1]);
        p[0] = np0;
        p[1] = np1;
        rows += 96;
        pos += 32;
        next += 32;
    }
    psl_st(ctx, 72, psl_a(psl_ld(ctx, 83), psl_ld(ctx, 64)));
    psl_st(ctx, 86, psl_a(psl_ld(ctx, 86), psl_splat32(16u * count)));
    ctx->pc = (0x082E8u + ctx->reloc) & PS_SPU_PC_MASK;
    return 1;
}
