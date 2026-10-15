/*
 * EDGE geometry's work on the GPU (D-988): one source for the Metal compute kernel and for C++ (PSPOCKET_EDGE_GPU=cpu,
 * the tests and edge-gpu-check run the same functions on the CPU). The code is the argument of EK_CODE: C++ includes it
 * through pspocket/core/edge_kernels.h (EK_CODE expands to the code, with the C++ meanings of EK_DEV and the like);
 * the Metal backend stringifies it (EK_CODE(...) #__VA_ARGS__) and puts the Metal meanings in front: no generated
 * file, as the iOS app compiles the runtime's sources as they are (D-014). So no preprocessor directive inside. What each function computes is what GTA V's EDGE build (spu_dad457863220b1cc) computes, in
 * the same order of operations (runtime/spu/replace/edge_dad4.c has the SPU's own versions of most of it), each
 * operation the SPU's.
 *
 * A job: num_vertexes vertices, each decompressed (positions: bit-packed fixed point; normals and tangents: unit
 * vectors in 3 bytes), skinned (none, four bones, one bone), compressed into the output vertex (F32, X11Y11Z10N,
 * I16N) and transformed to the screen for culling (x, y, an outcode, the integer coordinates); then its triangles
 * culled with EDGE's rules and the kept ones' indices packed in order. The arithmetic is the SPU's (below).
 */

#ifndef EK_CODE
#error "include pspocket/core/edge_kernels.h, or define EK_CODE to take the code"
#endif
EK_CODE(
/* ---- big-endian access ---- */

EK_INLINE ek_u32 ek_ld32(EK_IN ek_u8* p, ek_u32 o)
{
    return (ek_u32(p[o]) << 24) | (ek_u32(p[o + 1]) << 16) | (ek_u32(p[o + 2]) << 8) | ek_u32(p[o + 3]);
}
EK_INLINE float ek_ldf(EK_IN ek_u8* p, ek_u32 o) { return ek_as_f32(ek_ld32(p, o)); }
EK_INLINE void ek_st32(EK_DEV ek_u8* p, ek_u32 o, ek_u32 v)
{
    p[o] = ek_u8(v >> 24);
    p[o + 1] = ek_u8(v >> 16);
    p[o + 2] = ek_u8(v >> 8);
    p[o + 3] = ek_u8(v);
}
EK_INLINE void ek_st16(EK_DEV ek_u8* p, ek_u32 o, ek_u32 v)
{
    p[o] = ek_u8(v >> 8);
    p[o + 1] = ek_u8(v);
}

/* ---- the SPU's single precision (pspocket/spu/fp.h, lifted.h: what the SPU's code computes, as the CPU path runs it):
 * products, sums and fused products round toward zero, denormals are zeros (in and out), and a product with a zero
 * or denormal factor is +0 (fm) or the addend (fma). The GPU (and C++ by default) rounds to nearest: each result is
 * moved one step toward zero when its exact error says it was rounded away from it (error-free transforms: a
 * product's error by fma, a sum's by TwoSum). Bit for bit the SPU's but where a fused product's error is so much
 * smaller than its parts that their sum's sign rounds wrong (D-988). ---- */

EK_INLINE bool ek_zc(float x) { return (ek_as_u32(x) & 0x7F800000u) == 0u; } /* zero or denormal */
EK_INLINE float ek_signed_zero(float x) { return ek_as_f32(ek_as_u32(x) & 0x80000000u); }
/* r moved toward zero when the exact result is r + e and e points the other way; out of range: the largest finite;
 * denormal: zero (keeping the sign) */
EK_INLINE float ek_round_down(float r, float e)
{
    const ek_u32 u = ek_as_u32(r);
    if ((u & 0x7F800000u) == 0x7F800000u) return ek_as_f32((u & 0x80000000u) | 0x7F7FFFFFu);
    if ((u & 0x7F800000u) == 0u) return ek_signed_zero(r);
    if (e != 0.0f && ((e < 0.0f) != (r < 0.0f))) return ek_as_f32(u - 1u);
    return r;
}
/* a * b once rounded to nearest, never fused into what follows (fma with -0 adds nothing) */
EK_INLINE float ek_hmul(float a, float b) { return ek_hfma(a, b, -0.0f); }
/* fm */
EK_INLINE float ek_fm(float a, float b)
{
    if (ek_zc(a) || ek_zc(b)) return 0.0f;
    const float p = ek_hmul(a, b);
    return ek_round_down(p, ek_hfma(a, b, -p));
}
/* fa (and fs: fa(a, -b)) */
EK_INLINE float ek_fa(float a, float b)
{
    if (ek_zc(a)) a = ek_signed_zero(a);
    if (ek_zc(b)) b = ek_signed_zero(b);
    const float s = a + b, bb = s - a;
    return ek_round_down(s, (a - (s - bb)) + (b - bb));
}
EK_INLINE float ek_fs(float a, float b) { return ek_fa(a, -b); }
/* a * b + c fused, rounded toward zero (fms, fnms: the signs given) */
EK_INLINE float ek_fmaf(float a, float b, float c)
{
    if (ek_zc(c)) c = ek_signed_zero(c);
    if (ek_zc(a) || ek_zc(b)) return ek_fa(ek_fm(a, b), c);
    const float r = ek_hfma(a, b, c);
    const float p = ek_hmul(a, b), pe = ek_hfma(a, b, -p); /* a b = p + pe */
    const float s1 = p + c, bb = s1 - p, e1 = (p - (s1 - bb)) + (c - bb); /* p + c = s1 + e1 */
    return ek_round_down(r, (s1 - r) + (e1 + pe));
}
/* fma: a zero or denormal factor leaves the addend */
EK_INLINE float ek_fma(float a, float b, float c) { return (ek_zc(a) || ek_zc(b)) ? c : ek_fmaf(a, b, c); }
/* fcgt: a > b, every zero and denormal equal */
EK_INLINE ek_s32 ek_key(float x)
{
    const ek_u32 v = ek_zc(x) ? 0u : ek_as_u32(x);
    return ek_s32(v ^ ((v >> 31) ? 0x7FFFFFFFu : 0u));
}
EK_INLINE bool ek_fcgt(float a, float b) { return ek_key(a) > ek_key(b); }

/* n bits (n <= 25) of a big-endian bit stream from bit `bit` of p (it reads the 4 bytes from bit / 8). */
EK_INLINE ek_u32 ek_bits(EK_IN ek_u8* p, ek_u32 bit, ek_u32 n)
{
    const ek_u32 w = ek_ld32(p, bit >> 3);
    return (w << (bit & 7u)) >> (32u - n);
}

/* ---- the SPU's estimates (frest, frsqest, fi): RPCS3's tables (rpcs3/Emu/Cell/SPUThread.cpp, as
 * runtime/spu/src/fp.cpp has them) and its fixed-point interpolation (SPULLVMRecompiler.cpp "spu_fi") ---- */

EK_TABLE ek_u32 ek_frest_fraction[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07};
EK_TABLE ek_u32 ek_frsqest_fraction[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA};

EK_INLINE ek_u32 ek_frest(ek_u32 x)
{
    const ek_u32 e = (x >> 23) & 0xFFu;
    const ek_u32 exponent = e == 0u ? 0x7F800000u : (e < 253u ? (253u - e) << 23 : 0u);
    return ek_frest_fraction[(x >> 18) & 0x1Fu] | exponent | (x & 0x80000000u);
}
EK_INLINE ek_u32 ek_frsqest(ek_u32 x)
{
    const ek_u32 e = (x >> 23) & 0xFFu;
    const ek_u32 exponent = e == 0u ? 0x7F800000u : (190u - (e + 1u) / 2u) << 23;
    return ek_frsqest_fraction[(x >> 18) & 0x3Fu] | exponent;
}
EK_INLINE ek_u32 ek_fi(ek_u32 a, ek_u32 b)
{
    const ek_u32 base = (b & 0x007FFC00u) << 9;
    const ek_u32 ymul = (b & 0x3FFu) * (a & 0x7FFFFu);
    const bool adjust = ymul > base;
    const ek_u32 bnew = (base - ymul) >> (adjust ? 8u : 9u);
    ek_u32 expo = (b & 0xFF800000u) - (adjust ? 0x00800000u : 0u);
    if (((expo >> 23) & 0xFFu) == 0xFFu) expo = (expo & 0x80000000u) | 0x7F7FFFFFu; /* the SPU's floats have no infinity */
    return expo | (bnew & 0x007FFFFFu);
}

/* ---- decompression ---- */

/* Fixed point: (raw + bias) * 2^-frac, exact in floats for the widths EDGE allows. */
EK_INLINE float ek_fixed(ek_u32 raw, ek_s32 bias, ek_u32 frac)
{
    return float(ek_s32(raw) + bias) * ek_as_f32((127u - frac) << 23);
}

/* A unit vector in 24 bits: two components of 10 bits (the lower-numbered first) as fma(A, k, -0.70703125), the index
 * of the one left out (bits 3-2), its sign (bit 0: positive) and, for a tangent, w's sign (bit 1); the one left out
 * is sqrt(1 - (a a + b b)) as the job makes it, x fi(x, frsqest(x)) times x (checked bit for bit on the game's, D-988). */
EK_INLINE void ek_unit_vector(ek_u32 bits, EK_THR float* out /* 4 */)
{
    const float k = ek_as_f32(0x3AB52E7Cu), c0 = ek_as_f32(0xBF350000u);
    const float a = ek_fma(float(bits >> 14), k, c0);
    const float b = ek_fma(float((bits >> 4) & 1023u), k, c0);
    const ek_u32 sel = (bits >> 2) & 3u;
    const float x = ek_fs(1.0f, ek_fa(ek_fm(a, a), ek_fm(b, b)));
    const ek_u32 xb = ek_as_u32(x);
    float c = ek_fm(x, ek_as_f32(ek_fi(xb, ek_frsqest(xb))));
    c = ek_as_f32((ek_as_u32(c) & 0x7FFFFFFFu) | ((bits & 1u) ? 0u : 0x80000000u));
    out[0] = sel == 0u ? c : a;
    out[1] = sel == 1u ? c : (sel == 0u ? a : b);
    out[2] = sel == 2u ? c : b;
    out[3] = (bits & 2u) ? 1.0f : -1.0f;
}

/* ---- skinning ---- */

/* The blended rows of a vertex (3x4, rows of the matrix as stored: m[4r + c]). */
struct ek_rows {
    float m[12];
};

EK_INLINE float ek_weight(ek_u32 byte) { return ek_fm(float(byte), ek_as_f32(0x3B808080u)); /* fm(cuflt(b), 1/255) */ }

EK_INLINE ek_rows ek_skin_rows(EK_IN ek_u8* blob, EK_IN ps_edge_job* j, ek_u32 v)
{
    ek_rows r;
    /* A bone past the palette (which the SPU would read from whatever local store holds after it; nothing in the walk
     * names one) is the palette's last here, so the kernels stay in the blob. */
    const ek_u32 last = j->matrix_count ? j->matrix_count - 1u : 0u;
    if (j->skin == 4u) {
        const ek_u32 b = ek_min(ek_u32(blob[j->weights + v]), last);
        for (ek_u32 i = 0; i < 12u; i++) r.m[i] = ek_ldf(blob, j->matrices + 48u * b + 4u * i);
        return r;
    }
    EK_IN ek_u8* q = blob + j->weights + 8u * v;
    ek_u32 w[4], bone[4];
    for (ek_u32 k = 0; k < 4u; k++) {
        w[k] = q[2u * k];
        bone[k] = q[2u * k + 1u];
    }
    for (ek_u32 k = 0; k < 4u; k++)
        if (bone[k] != 0xFFu || k) bone[k] = ek_min(bone[k], last);
    if (bone[0] == 0xFFu) { /* one bone: weight 255 for bone 0 */
        w[0] = 255u;
        bone[0] = 0u;
        w[1] = w[2] = w[3] = 0u;
    }
    const float w0 = ek_weight(w[0]);
    for (ek_u32 i = 0; i < 12u; i++) r.m[i] = w[0] ? ek_fm(w0, ek_ldf(blob, j->matrices + 48u * bone[0] + 4u * i)) : 0.0f;
    for (ek_u32 k = 1; k < 4u; k++) {
        if (!w[k]) continue;
        const float wk = ek_weight(w[k]);
        for (ek_u32 i = 0; i < 12u; i++) r.m[i] = ek_fma(wk, ek_ldf(blob, j->matrices + 48u * bone[k] + 4u * i), r.m[i]);
    }
    return r;
}

/* p' = R . (p, 1) in the job's order: x' = ((x m00 + m03) + y m01) + z m02, y' = ((y m11 + m13) + z m12) + x m10,
 * z' = ((z m22 + m23) + x m20) + y m21. */
EK_INLINE void ek_skin_position(EK_THR float* p, ek_rows r)
{
    const float x = p[0], y = p[1], z = p[2];
    p[0] = ek_fma(z, r.m[2], ek_fma(y, r.m[1], ek_fma(x, r.m[0], r.m[3])));
    p[1] = ek_fma(x, r.m[4], ek_fma(z, r.m[6], ek_fma(y, r.m[5], r.m[7])));
    p[2] = ek_fma(y, r.m[9], ek_fma(x, r.m[8], ek_fma(z, r.m[10], r.m[11])));
}

/* n' = R3x3 . n: x' = (x m00 + y m01) + z m02 and so on as above; w kept. */
EK_INLINE void ek_skin_vector(EK_THR float* n, ek_rows r)
{
    const float x = n[0], y = n[1], z = n[2];
    n[0] = ek_fma(z, r.m[2], ek_fma(y, r.m[1], ek_fm(x, r.m[0])));
    n[1] = ek_fma(x, r.m[4], ek_fma(z, r.m[6], ek_fm(y, r.m[5])));
    n[2] = ek_fma(y, r.m[9], ek_fma(x, r.m[8], ek_fm(z, r.m[10])));
}

/* ---- output formats ---- */

/* X11Y11Z10N and I16N as EDGE makes them: u = trunc(f * (2^(n-1) - 0.5) + (2^(n-1) - 0.5)) (two roundings), saturated to n bits, then stored as
 * u - 2^(n-1) (the top bit flipped). */
EK_INLINE ek_u32 ek_snorm(float f, ek_u32 n)
{
    const float h = float((1u << (n - 1u))) - 0.5f; /* "half" is a type in Metal */
    const float s = ek_fa(ek_fm(f, h), h);
    const ek_u32 top = (1u << n) - 1u;
    const ek_u32 u = s > 0.0f ? (s >= float(top) ? top : ek_u32(s)) : 0u;
    return u ^ (1u << (n - 1u));
}

EK_INLINE void ek_store_attr(EK_DEV ek_u8* out, ek_u32 at, ek_u32 format, ek_u32 comps, EK_THR float* v)
{
    if (format == PS_EDGE_FMT_F32) {
        for (ek_u32 k = 0; k < comps; k++) ek_st32(out, at + 4u * k, ek_as_u32(v[k]));
    } else if (format == PS_EDGE_FMT_X11Y11Z10N) {
        ek_st32(out, at, ek_snorm(v[0], 11u) | (ek_snorm(v[1], 11u) << 11) | (ek_snorm(v[2], 10u) << 22));
    } else if (format == PS_EDGE_FMT_I16N) {
        for (ek_u32 k = 0; k < comps; k++) ek_st16(out, at + 2u * k, ek_snorm(v[k], 16u));
    }
}

/* ---- the screen transform (EDGE's vertex output for culling, 0x0ECF8) ---- */

struct ek_screen {
    float x, y;
    ek_u32 code;   /* outcode << 24: z < zmin, x < xmin, y < ymin, x > xmax, y > ymax, z > zmax in bits 31 to 26 */
    ek_u32 packed; /* integer x << 16 | integer y & 0xFFFF, bit 31: w > 0 */
};

/* cflts(x, 173): truncated to a signed integer, saturated. */
EK_INLINE ek_s32 ek_cflts(float f)
{
    if (!(f == f)) return 0;
    if (f >= 2147483520.0f) return 0x7FFFFFFF;
    if (f <= -2147483648.0f) return ek_s32(0x80000000u);
    return ek_s32(f);
}

EK_INLINE ek_screen ek_transform(EK_IN ps_edge_job* j, EK_THR float* p)
{
    float c[4];
    for (ek_u32 i = 0; i < 4u; i++) {
        float t = j->combined[4u * i + 3u];
        t = ek_fma(p[0], j->combined[4u * i], t);
        t = ek_fma(p[1], j->combined[4u * i + 1u], t);
        c[i] = ek_fma(p[2], j->combined[4u * i + 2u], t);
    }
    const float w = c[3];
    /* 1/w as the job makes it: the estimate, interpolated, and one Newton step */
    const float r = ek_as_f32(ek_fi(ek_as_u32(w), ek_frest(ek_as_u32(w))));
    const float inv = ek_fma(ek_fmaf(-r, w, 1.0f), r, r); /* fnms, then fma */
    const ek_u32 wsign = ek_as_u32(w) & 0x80000000u;
    float s[3];
    for (ek_u32 i = 0; i < 3u; i++) {
        const float n = ek_as_f32(ek_as_u32(ek_fm(c[i], inv)) ^ wsign);
        s[i] = ek_fma(n, j->scale[i], j->offset[i]);
    }
    ek_screen o;
    o.x = s[0];
    o.y = s[1];
    const ek_u32 ix = ek_u32(ek_cflts(s[0])), iy = ek_u32(ek_cflts(s[1]));
    o.packed = ((ix << 16) | (iy & 0xFFFFu)) & 0x7FFFFFFFu;
    if (ek_fcgt(w, 0.0f)) o.packed |= 0x80000000u;
    ek_u32 code = 0;
    if (ek_fcgt(j->bound_min[2], s[2])) code |= 0x80000000u;
    if (ek_fcgt(j->bound_min[0], s[0])) code |= 0x40000000u;
    if (ek_fcgt(j->bound_min[1], s[1])) code |= 0x20000000u;
    if (ek_fcgt(s[0], j->bound_max[0])) code |= 0x10000000u;
    if (ek_fcgt(s[1], j->bound_max[1])) code |= 0x08000000u;
    if (ek_fcgt(s[2], j->bound_max[2])) code |= 0x04000000u;
    o.code = code;
    return o;
}

/* ---- one vertex: decompress, skin, compress into the output, transform ---- */

EK_INLINE ek_screen ek_vertex(EK_IN ek_u8* blob, EK_IN ps_edge_job* j, ek_u32 v, EK_DEV ek_u8* out_vertex)
{
    float pos[4], nrm[4], tan[4];
    {
        EK_IN ek_u8* s = blob + j->pos_data + v * j->pos_stride;
        ek_u32 bit = j->pos_bit;
        for (ek_u32 k = 0; k < 3u; k++) {
            pos[k] = ek_fixed(ek_bits(s, bit, j->pos_bits[k]), j->pos_bias[k], j->pos_frac[k]);
            bit += j->pos_bits[k];
        }
        pos[3] = 1.0f;
    }
    const bool has_nrm = j->nrm_offset != 0xFFFFFFFFu, has_tan = j->tan_offset != 0xFFFFFFFFu;
    if (has_nrm) {
        EK_IN ek_u8* s = blob + j->uv_data + v * j->uv_stride + j->nrm_offset;
        ek_unit_vector((ek_u32(s[0]) << 16) | (ek_u32(s[1]) << 8) | s[2], nrm);
        nrm[3] = 1.0f;
    }
    if (has_tan) {
        EK_IN ek_u8* s = blob + j->uv_data + v * j->uv_stride + j->tan_offset;
        ek_unit_vector((ek_u32(s[0]) << 16) | (ek_u32(s[1]) << 8) | s[2], tan);
    }
    if (j->skin) {
        const ek_rows r = ek_skin_rows(blob, j, v);
        ek_skin_position(pos, r);
        if (has_nrm) ek_skin_vector(nrm, r);
        if (has_tan) ek_skin_vector(tan, r);
    }
    for (ek_u32 a = 0; a < j->out_count; a++) {
        EK_IN ps_edge_out_attr* o = &j->out[a];
        EK_THR float* src = o->source == PS_EDGE_ATTR_POSITION ? pos : o->source == PS_EDGE_ATTR_NORMAL ? nrm : tan;
        ek_store_attr(out_vertex, o->offset, o->format, o->components, src);
    }
    return ek_transform(j, pos);
}

/* ---- one triangle: culled? (0x0F008's rules) ---- */

EK_INLINE bool ek_culled(EK_IN ps_edge_job* j, ek_screen a, ek_screen b, ek_screen c)
{
    if (!j->cull_mode) return false;
    const float ex1 = ek_fs(b.x, a.x), ex2 = ek_fs(c.x, a.x), ey1 = ek_fs(b.y, a.y), ey2 = ek_fs(c.y, a.y);
    const float area = ek_fmaf(ex1, ey2, -ek_fm(ey1, ex2)); /* fms */
    const bool facing = ek_fcgt(ek_as_f32(ek_as_u32(area) ^ j->flip), 0.0f);
    const ek_u32 uni = a.code | b.code | c.code, inter = a.code & b.code & c.code;
    const bool in_front = uni < 0x80000000u;
    const bool w_pos = ((a.packed & b.packed & c.packed) >> 31) != 0u;
    const ek_u32 keep = (facing && in_front && w_pos) ? 1u : 0u;
    const ek_u32 outside = inter != 0u ? 1u : 0u;
    if (j->culled[keep * 2u + outside]) return true;
    if (j->nopix) {
        const bool same_y = ((a.packed ^ c.packed) & 0xFFFFu) == 0u && ((a.packed ^ b.packed) & 0xFFFFu) == 0u;
        const bool same_x = ((a.packed ^ c.packed) >> 16) == 0u && ((a.packed ^ b.packed) >> 16) == 0u;
        if (same_x || same_y) return true;
        const ek_s32 sa = ek_cflts(ek_fa(a.x, a.y)), da = ek_cflts(ek_fs(a.x, a.y));
        const ek_s32 sb = ek_cflts(ek_fa(b.x, b.y)), db = ek_cflts(ek_fs(b.x, b.y));
        const ek_s32 sc = ek_cflts(ek_fa(c.x, c.y)), dc = ek_cflts(ek_fs(c.x, c.y));
        if ((sc == sa && sb == sa) || (dc == da && db == da)) return true;
    }
    return false;
}
)
