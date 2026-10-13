// The RSX shader interpreter (D-940). See pspocket/rsx/shader/interp.hpp. The MSL runs each operation as msl.cpp
// translates it (whose comments cite RPCS3 VertexProgramDecompiler.cpp and FragmentProgramDecompiler.cpp per
// operation); what the translator decides at compile time from VpOptions and FpOptions it reads at run time.

#include "pspocket/rsx/shader/interp.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace pspocket::rsx::shader {
namespace {

u32 swizzle8(const Swizzle& s)
{
    return (s.c[0] & 3u) | ((s.c[1] & 3u) << 2) | ((s.c[2] & 3u) << 4) | ((s.c[3] & 3u) << 6);
}

u32 bit(bool b) { return b ? 1u : 0u; }

// ---- vertex programs ----------------------------------------------------------------------------------------------

u32 vp_source_word(const VpSource& s)
{
    return (static_cast<u32>(s.type) & 3u) | ((s.temp & 0x3fu) << 2) | (swizzle8(s.swizzle) << 8) | (bit(s.neg) << 16) |
           (bit(s.abs) << 17);
}

// ---- fragment programs --------------------------------------------------------------------------------------------

u32 fp_source_word(const FpSource& s)
{
    return (static_cast<u32>(s.type) & 3u) | ((s.index & 0x3fu) << 2) | (bit(s.fp16) << 8) | (swizzle8(s.swizzle) << 9) |
           (bit(s.neg) << 17) | (bit(s.abs) << 18) | ((s.prec_mod & 7u) << 19);
}

// The sources each operation reads (msl.cpp evaluates src0 always, src1 and src2 as the operation needs them): an
// indexed input past SSA outside a loop fails the translation only where it is read.
u32 fp_sources_read(FpOp op)
{
    switch (op) {
    case FpOp::MUL: case FpOp::ADD: case FpOp::DP2: case FpOp::DP3: case FpOp::DP4: case FpOp::MIN: case FpOp::MAX:
    case FpOp::DIV: case FpOp::DIVSQ: case FpOp::DST: case FpOp::REFL: case FpOp::SLT: case FpOp::SGE: case FpOp::SLE:
    case FpOp::SGT: case FpOp::SNE: case FpOp::SEQ: return 3;
    case FpOp::MAD: case FpOp::DP2A: case FpOp::BEM: case FpOp::LRP: case FpOp::TXD: case FpOp::TEXBEM:
    case FpOp::TXPBEM: return 7;
    default: return 1;
    }
}

constexpr const char* kInterpreter = R"MSL(
// ---- the shader interpreter (D-940) ----------------------------------------------------------------------------
// Mirrors pspocket::rsx::shader::InterpVsOptions / InterpFsOptions.
struct rsx_vi_options {
    uint start; uint count; uint input_mask; uint output_mask;
    uint user_clip; uint masked_outputs; uint point_size; uint texture_kind;
};
struct rsx_fi_options {
    uint shader_control; uint texcoord_2d_mask; uint flags; uint texture_convert_mask;
    uint texture_alpha_kill_mask; uint point_coord_mask; uint mrt_count; uint pad;
    uint texture_kind[16];
    uint output_swizzle[4];
};

static inline float4 rsx_i_swizzle(float4 v, uint s)
{
    return float4(v[s & 3u], v[(s >> 2) & 3u], v[(s >> 4) & 3u], v[(s >> 6) & 3u]);
}

static inline void rsx_i_assign(thread float4& reg, float4 value, uint mask, bool4 cm)
{
    for (uint c = 0u; c < 4u; c++) {
        if (((mask >> c) & 1u) != 0u && cm[c]) reg[c] = value[c];
    }
}

// A vertex program condition (msl.cpp vp_condition): CC through the swizzle against 0, cond 1 lt, 2 eq, 4 gt.
static inline bool4 rsx_i_vp_cond(float4 cc, uint swz, uint cond)
{
    const float4 v = rsx_i_swizzle(cc, swz);
    switch (cond & 7u) {
    case 2u: return v == 0.0;
    case 3u: return v <= 0.0;
    case 4u: return v > 0.0;
    case 5u: return v != 0.0;
    case 6u: return v >= 0.0;
    default: return v < 0.0; // 1, and 0 and 7 as the translator's table has them
    }
}

static inline float4 rsx_i_vp_src(uint d, thread const float4* r, thread const float4* v, constant float4* vc,
    uint input, uint constant_, bool relative, int address)
{
    float4 x;
    switch (d & 3u) {
    case 1u: x = r[(d >> 2) & 63u]; break;
    case 2u: x = v[input & 15u]; break;
    case 3u: x = relative ? vc[clamp(int(constant_) + address, 0, 467)] : vc[min(constant_, 467u)]; break;
    default: x = float4(0.0); break;
    }
    x = rsx_i_swizzle(x, (d >> 8) & 255u);
    if (((d >> 17) & 1u) != 0u) x = abs(x);
    if (((d >> 16) & 1u) != 0u) x = -x;
    return x;
}

// msl.cpp's write(): the output and a temp, or the condition register.
static inline void rsx_i_vp_write(float4 value, uint mask, bool is_result, uint temp, uint out, uint cond_reg,
    bool cond_update, bool4 cm, thread float4* r, thread float4* o, thread float4* cc)
{
    if (mask == 0u) return;
    if (out != 31u || temp != 63u) {
        if (is_result && out != 31u) rsx_i_assign(o[out], value, mask, cm);
        if (temp != 63u) rsx_i_assign(r[temp], value, mask, cm);
        else if (!is_result) rsx_i_assign(cc[cond_reg], value, mask, cm);
    } else if (cond_update) {
        rsx_i_assign(cc[cond_reg], value, mask, cm);
    }
}

struct rsx_vs_out_i {
    float4 position [[position]];
    float4 col0 [[user(col0)]]; float4 col1 [[user(col1)]];
    float4 bfc0 [[user(bfc0)]]; float4 bfc1 [[user(bfc1)]];
    float4 col0f [[user(col0f)]]; float4 col1f [[user(col1f)]];
    float4 bfc0f [[user(bfc0f)]]; float4 bfc1f [[user(bfc1f)]];
    float4 fogc [[user(fogc)]];
    float4 tc0 [[user(tc0)]]; float4 tc1 [[user(tc1)]]; float4 tc2 [[user(tc2)]]; float4 tc3 [[user(tc3)]];
    float4 tc4 [[user(tc4)]]; float4 tc5 [[user(tc5)]]; float4 tc6 [[user(tc6)]]; float4 tc7 [[user(tc7)]];
    float4 tc8 [[user(tc8)]]; float4 tc9 [[user(tc9)]];
    float point_size [[point_size]];
    float clip [[clip_distance]] [6];
};

vertex rsx_vs_out_i rsx_vs_interp(uint vid [[vertex_id]], device const uchar* vb [[buffer(0)]],
    constant rsx_vs_params& p [[buffer(1)]], constant float4* vc [[buffer(2)]], device const uchar* ib [[buffer(3)]],
    constant uint* prog [[buffer(4)]], constant rsx_vi_options& o [[buffer(5)]],
    array<texture2d<float>, 4> vt2 [[texture(0)]], array<texturecube<float>, 4> vtc [[texture(4)]],
    array<texture3d<float>, 4> vt3 [[texture(8)]], array<sampler, 4> vsmp [[sampler(0)]])
{
    const uint vi = rsx_vertex_index(vid, p, ib);
    float4 v[16];
    for (uint i = 0u; i < 16u; i++) v[i] = ((o.input_mask >> i) & 1u) != 0u ? rsx_fetch(p.attribs[i], vi, vb) : float4(0.0);
    float4 r[64];
    for (uint i = 0u; i < 64u; i++) r[i] = float4(0.0);
    float4 outs[16];
    for (uint i = 0u; i < 16u; i++) outs[i] = float4(0.0, 0.0, 0.0, 1.0);
    float4 cc[2] = {float4(0.0), float4(0.0)};
    int4 a[2] = {int4(0), int4(0)};
    int calls[8];
    int depth = 0;
    int pc = 0;
    uint jumps = 0u; // the translation's flow loop gives up after 65536 jumps
    while (pc >= 0 && uint(pc) < o.count) {
        constant uint* w = prog + uint(pc) * 8u;
        const uint w0 = w[0], w1 = w[1], w2 = w[2];
        const uint vec_op = w0 & 31u, sca_op = (w0 >> 5) & 31u;
        const bool saturate = ((w0 >> 10) & 1u) != 0u, vec_result = ((w0 >> 11) & 1u) != 0u;
        const bool cond_test = ((w0 >> 12) & 1u) != 0u, cond_update = ((w0 >> 13) & 1u) != 0u;
        const bool relative = ((w0 >> 14) & 1u) != 0u, end = ((w0 >> 15) & 1u) != 0u;
        const uint cond = (w0 >> 16) & 7u, cond_reg = (w0 >> 19) & 1u, addr_reg = (w0 >> 20) & 1u;
        const uint addr_swz = (w0 >> 21) & 3u, tex_num = (w0 >> 23) & 3u;
        const uint vec_mask = w1 & 15u, sca_mask = (w1 >> 4) & 15u, outr = (w1 >> 8) & 31u;
        const uint vec_temp = (w1 >> 13) & 63u, sca_temp = (w1 >> 19) & 63u, input = (w1 >> 25) & 15u;
        const uint constant_ = w2 & 1023u, cond_swz = (w2 >> 10) & 255u, target = (w2 >> 18) & 1023u;
        // BRA, BRI, CAL, CLI, RET, BRB, CLB
        const bool branch = (sca_op >= 8u && sca_op <= 12u) || sca_op == 17u || sca_op == 18u;
        const bool vec = vec_op != 0u, sca = sca_op != 0u && !branch;
        if ((!vec && !sca && !branch) || (cond == 0u && !branch)) { // NOP, or never written
            if (end) break;
            pc++;
            continue;
        }
        const bool4 cm = cond_test && cond != 7u ? rsx_i_vp_cond(cc[cond_reg], cond_swz, cond) : bool4(true);
        const int address = a[addr_reg][addr_swz];
        const float4 s0 = rsx_i_vp_src(w[3], r, v, vc, input, constant_, relative, address);
        const float4 s1 = rsx_i_vp_src(w[4], r, v, vc, input, constant_, relative, address);
        const float4 s2 = rsx_i_vp_src(w[5], r, v, vc, input, constant_, relative, address);
        float4 vecv = float4(0.0), scav = float4(0.0);
        if (vec && vec_op == 13u) { // ARL: floor, as NV_vertex_program2 (D-174)
            a[addr_reg] = int4(floor(s0));
        } else if (vec) {
            switch (vec_op) {
            case 1u: vecv = s0; break;                                             // MOV
            case 2u: vecv = s0 * s1; break;                                        // MUL
            case 3u: vecv = s0 + s2; break;                                        // ADD: src0 and src2
            case 4u: vecv = fma(s0, s1, s2); break;                                // MAD
            case 5u: vecv = float4(dot(s0.xyz, s1.xyz)); break;                    // DP3
            case 6u: vecv = float4(dot(s0.xyz, s1.xyz) + s1.w); break;             // DPH
            case 7u: vecv = float4(dot(s0, s1)); break;                            // DP4
            case 8u: vecv = float4(1.0, s0.y * s1.y, s0.z, s1.w); break;           // DST
            case 9u: vecv = min(s0, s1); break;                                    // MIN
            case 10u: vecv = max(s0, s1); break;                                   // MAX
            case 11u: vecv = select(float4(0.0), float4(1.0), s0 < s1); break;     // SLT
            case 12u: vecv = select(float4(0.0), float4(1.0), s0 >= s1); break;    // SGE
            case 14u: vecv = fract(s0); break;                                     // FRC
            case 15u: vecv = floor(s0); break;                                     // FLR
            case 16u: vecv = select(float4(0.0), float4(1.0), s0 == s1); break;    // SEQ
            case 17u: vecv = float4(0.0); break;                                   // SFL
            case 18u: vecv = select(float4(0.0), float4(1.0), s0 > s1); break;     // SGT
            case 19u: vecv = select(float4(0.0), float4(1.0), s0 <= s1); break;    // SLE
            case 20u: vecv = select(float4(0.0), float4(1.0), s0 != s1); break;    // SNE
            case 21u: vecv = float4(1.0); break;                                   // STR
            case 22u: vecv = sign(s0); break;                                      // SSG
            case 25u: {                                                            // TXL at level 0 (D-274)
                const uint k = (o.texture_kind >> (tex_num * 8u)) & 255u;
                if (k == 4u) vecv = vt3[tex_num].sample(vsmp[tex_num], s0.xyz, level(0.0));
                else if (k == 1u) vecv = vtc[tex_num].sample(vsmp[tex_num], s0.xyz, level(0.0));
                else vecv = vt2[tex_num].sample(vsmp[tex_num], k == 2u ? float2(s0.x, 0.5) : s0.xy, level(0.0));
                break;
            }
            default: break;
            }
            if (saturate) vecv = clamp(vecv, 0.0, 1.0);
        }
        if (sca) {
            const float s = s2.x; // scalar ops read src2
            switch (sca_op) {
            case 1u: scav = float4(s); break;                                      // MOV
            case 2u: scav = float4(1.0 / s); break;                                // RCP
            case 3u: scav = float4(clamp(1.0 / s, 5.42101e-20, 1.884467e19)); break; // RCC
            case 4u: scav = float4(rsqrt(max(s, 1e-10))); break;                   // RSQ
            case 5u: scav = float4(exp(s)); break;                                 // EXP
            case 6u: scav = float4(log(s)); break;                                 // LOG
            case 7u:                                                               // LIT (lit_legacy)
                scav = float4(1.0, max(s2.x, 0.0), s2.x > 0.0 ? exp(s2.w * log(max(s2.y, 1e-10))) : 0.0, 1.0);
                break;
            case 13u: scav = float4(log2(max(s, 1e-10))); break;                   // LG2
            case 14u: scav = float4(exp2(s)); break;                               // EX2
            case 15u: scav = float4(sin(s)); break;                                // SIN
            case 16u: scav = float4(cos(s)); break;                                // COS
            default: break;
            }
            if (saturate) scav = clamp(scav, 0.0, 1.0);
        }
        // Both ops read their sources before either writes (dual issue).
        if (vec && vec_op != 13u) rsx_i_vp_write(vecv, vec_mask, vec_result, vec_temp, outr, cond_reg, cond_update, cm, r, outs, cc);
        if (sca) rsx_i_vp_write(scav, sca_mask, !vec_result, sca_temp, outr, cond_reg, cond_update, cm, r, outs, cc);
        if (branch) {
            bool taken;
            if (sca_op == 17u || sca_op == 18u) { // BRB, CLB: a bit of NV4097_SET_TRANSFORM_BRANCH_BITS (D-113)
                taken = (((p.branch_bits >> ((w[6] >> 10) & 31u)) & 1u) != 0u) == (((w[6] >> 15) & 1u) != 0u);
            } else {
                taken = cond == 0u ? false : cond == 7u ? true : any(rsx_i_vp_cond(cc[cond_reg], cond_swz, cond));
            }
            if (taken) {
                if (++jumps >= 65536u) break;
                switch (sca_op) {
                case 8u: pc = address - int(o.start); break;                        // BRA
                case 9u: case 17u: pc = int(target) - int(o.start); break;          // BRI, BRB
                case 12u: pc = depth > 0 ? calls[--depth] : -1; break;              // RET
                default:                                                            // CAL, CLI, CLB
                    if (depth < 8) calls[depth++] = pc + 1;
                    pc = int(target) - int(o.start);
                    break;
                }
                continue;
            }
        }
        if (end) break;
        pc++;
    }

    rsx_vs_out_i res;
    res.position = float4(dot(p.pos_x, outs[0]), dot(p.pos_y, outs[0]), dot(p.pos_z, outs[0]), outs[0].w);
    // An output the output mask leaves out is (0, 0, 0, 1) (D-267).
    const float4 none = float4(0.0, 0.0, 0.0, 1.0);
    const uint m = o.masked_outputs;
    res.col0 = ((m >> 1) & 1u) != 0u ? none : outs[1];
    res.col1 = ((m >> 2) & 1u) != 0u ? none : outs[2];
    res.bfc0 = ((m >> 3) & 1u) != 0u ? none : outs[3];
    res.bfc1 = ((m >> 4) & 1u) != 0u ? none : outs[4];
    res.col0f = res.col0; res.col1f = res.col1; res.bfc0f = res.bfc0; res.bfc1f = res.bfc1;
    res.fogc = ((m >> 5) & 1u) != 0u ? none : outs[5].xxxx;
    res.tc0 = ((m >> 7) & 1u) != 0u ? none : outs[7];
    res.tc1 = ((m >> 8) & 1u) != 0u ? none : outs[8];
    res.tc2 = ((m >> 9) & 1u) != 0u ? none : outs[9];
    res.tc3 = ((m >> 10) & 1u) != 0u ? none : outs[10];
    res.tc4 = ((m >> 11) & 1u) != 0u ? none : outs[11];
    res.tc5 = ((m >> 12) & 1u) != 0u ? none : outs[12];
    res.tc6 = ((m >> 13) & 1u) != 0u ? none : outs[13];
    res.tc7 = ((m >> 14) & 1u) != 0u ? none : outs[14];
    res.tc8 = ((m >> 15) & 1u) != 0u ? none : outs[15];
    res.tc9 = ((m >> 6) & 1u) != 0u ? none : outs[6];
    // Points (D-262, D-275).
    res.point_size = o.point_size == 0u ? 1.0
        : ((o.output_mask >> 6) & 1u) != 0u ? (signbit(p.point_size) ? outs[6].x : p.point_size) * p.point_scale
                                             : abs(p.point_size) * p.point_scale;
    // User clip planes (D-259): a plane that is off keeps everything.
    for (uint i = 0u; i < 6u; i++) {
        const uint op = (o.user_clip >> (4u * i)) & 15u;
        const float d = (i < 3u ? outs[5] : outs[6])[1u + i % 3u];
        res.clip[i] = op == 1u ? -d : op == 2u ? d : 1.0;
    }
    return res;
}

// ---- fragment programs ----

#ifndef RSX_MRT
#define RSX_MRT 1
#endif
#ifndef RSX_DEPTH
#define RSX_DEPTH 0
#endif

// Metal's pack_float_to_unorm4x8 and its kin, by hand (D-942): the compiler gets them wrong next to another write to
// the components of the register they are written to. round(x * n) of the exact product, as the intrinsics round it
// (checked bit for bit against them): a float product that lands on k + 0.5 is decided by its rounding error.
static float rsx_round_product(float x, float n)
{
    const float p = x * n, e = fma(x, n, -p), f = floor(p);
    return p - f == 0.5 ? (e < 0.0 ? f : e > 0.0 ? f + 1.0 : rint(p)) : rint(p);
}
static uint rsx_pack_unorm4x8(float4 v)
{
    const float4 c = clamp(v, 0.0, 1.0);
    return uint(rsx_round_product(c.x, 255.0)) | (uint(rsx_round_product(c.y, 255.0)) << 8) |
           (uint(rsx_round_product(c.z, 255.0)) << 16) | (uint(rsx_round_product(c.w, 255.0)) << 24);
}
static uint rsx_pack_snorm8(float x) { return uint(int(sign(x) * rsx_round_product(abs(x), 127.0))) & 0xffu; }
static uint rsx_pack_snorm4x8(float4 v)
{
    const float4 c = clamp(v, -1.0, 1.0);
    return rsx_pack_snorm8(c.x) | (rsx_pack_snorm8(c.y) << 8) | (rsx_pack_snorm8(c.z) << 16) | (rsx_pack_snorm8(c.w) << 24);
}
static uint rsx_pack_unorm2x16(float2 v)
{
    const float2 c = clamp(v, 0.0, 1.0);
    return uint(rsx_round_product(c.x, 65535.0)) | (uint(rsx_round_product(c.y, 65535.0)) << 16);
}
static float4 rsx_nrm(float4 v) { const float l = length(v.xyz); return (l > 0.0 ? float4(v.xyz / l, 0.0) : v).xyzz; }
static float4 rsx_divsq(float4 a, float b) { return select(a, a / sqrt(abs(b)), abs(a) > 0.0); }
static float4 rsx_lit(float4 v)
{
    const float x = max(v.x, 0.0), y = max(v.y, 1e-10);
    return float4(1.0, x, x > 0.0 ? exp2(v.w * log2(y)) : 0.0, 1.0);
}
static float4 rsx_lif(float4 v) { return float4(1.0, v.y, v.y > 0.0 ? exp2(v.w) : 0.0, 1.0); }
static float4 rsx_refl(float4 i, float4 n) { return i - 2.0 * dot(n, i) * n; }
static float4 rsx_linear_to_srgb(float4 c)
{
    const float4 low = c * 12.92, high = 1.055 * pow(c, float4(1.0 / 2.4)) - 0.055;
    return clamp(select(high, low, c < 0.0031308), 0.0, 1.0);
}
static float4 rsx_srgb_to_linear(float4 c) { return select(c / 12.92, pow((c + 0.055) / 1.055, float4(2.4)), c > 0.04045); }
static bool4 rsx_channels(uint m) { return bool4((m & 2u) != 0u, (m & 4u) != 0u, (m & 8u) != 0u, (m & 1u) != 0u); }
static float4 rsx_texel(float4 v, uint c, bool bx2)
{
    const bool wide = (c & (1u << 28)) != 0u;
    const bool expand_bx2 = bx2 && (c & (1u << 26)) != 0u;
    if (expand_bx2) c = (c & ~0xfu) | (((c >> 16) & 0xfu) << 6);
    uint left = 0xfu;
    uint m = (c >> 10) & 0xfu;
    if (m != 0u) {
        const float4 s = wide ? float4(as_type<int4>(uint4(floor(fma(v, float4(65535.0), float4(0.5)))) << uint4(16u)) >> int4(16)) / 32767.0
                              : float4(as_type<int4>(uint4(floor(fma(v, float4(255.0), float4(0.5)))) << uint4(24u)) >> int4(24)) / 127.0;
        v = select(v, s, rsx_channels(m));
        left &= ~m;
    }
    m = c & 0xfu & left;
    if (m != 0u) {
        v = select(v, rsx_srgb_to_linear(v), rsx_channels(m));
        left &= ~m;
    }
    m = (c >> 6) & 0xfu & left;
    if (m != 0u) {
        const float4 x = expand_bx2 ? v * 2.0 - 1.0
                       : wide ? (floor(fma(v, float4(65535.0), float4(0.5))) - 32768.0) / 32767.0
                              : (floor(fma(v, float4(255.0), float4(0.5))) - 128.0) / 127.0;
        v = select(v, x, rsx_channels(m));
    }
    return v;
}

// msl.cpp clamp_precision: 1 half, 2 fixed12, 3 fixed9, 4 saturate.
static inline float4 rsx_i_prec(float4 v, uint prec)
{
    switch (prec) {
    case 1u: return clamp(v, -65504.0, 65504.0);
    case 2u: return clamp(v, -2.0, 2.0);
    case 3u: return clamp(v, -1.0, 1.0);
    case 4u: return clamp(v, 0.0, 1.0);
    default: return v;
    }
}

// msl.cpp fp_condition: RC through the swizzle against 0, by the instruction's lt, eq and gt.
static inline bool4 rsx_i_fp_cond(float4 cc, uint swz, bool lt, bool eq, bool gt)
{
    const float4 v = rsx_i_swizzle(cc, swz);
    if (gt && eq) return v >= 0.0;
    if (lt && eq) return v <= 0.0;
    if (gt && lt) return v != 0.0;
    if (gt) return v > 0.0;
    if (lt) return v < 0.0;
    return v == 0.0;
}

static inline float4 rsx_i_out_swizzle(float4 v, uint sw)
{
    float4 r;
    for (uint c = 0u; c < 4u; c++) {
        const uint k = (sw >> (8u * c)) & 255u;
        r[c] = k < 4u ? v[k] : (k == 4u ? 0.0 : 1.0);
    }
    return r;
}

// One texture lookup of a unit of the given kind (msl.cpp's TEX, TXP, TXL, TXB and TXD; TEXBEM and TXPBEM arrive as
// TEX and TXP with their transformed coordinate). sop: the lookup's opcode. d1, d2: TXD's derivatives.
static float4 rsx_i_sample(uint kind, uint sop, uint t, float4 cs, float4 d1, float4 d2, constant rsx_fs_params& fp,
    texture2d<float> tx2, texturecube<float> txc, texture3d<float> tx3, depth2d<float> txd, sampler s)
{
    const bool proj = sop == 0x18u;
    const float lb = fp.tex_bias[t];
    const float2 scale = fp.tex_scale[t].xy;
    if (kind == 3u || kind == 6u) { // Shadow2D, DepthShadow2D: no LOD, bias or derivatives
        const float2 uv = proj ? cs.xy / cs.w : cs.xy;
        const float ref = proj ? cs.z / cs.w : cs.z;
        return kind == 3u ? rsx_shadow(tx2, s, uv * scale, ref, fp.tex_scale[t])
                          : rsx_shadow_depth(txd, s, uv * scale, ref, fp.tex_scale[t]);
    }
    if (kind == 5u) { // Depth2D: the depth in every channel
        const float2 c2 = (proj ? cs.xy / cs.w : cs.xy) * scale;
        switch (sop) {
        case 0x2Fu: return float4(txd.sample(s, c2, level(cs.w + lb)));
        case 0x31u: return float4(txd.sample(s, c2, bias(cs.w + lb)));
        case 0x19u: return float4(txd.sample(s, c2, gradient2d(d1.xy * scale * exp2(lb), d2.xy * scale * exp2(lb))));
        default: return float4(txd.sample(s, c2, bias(lb)));
        }
    }
    if (kind == 4u) { // Tex3D
        const float3 s3 = float3(scale, 1.0);
        const float3 c3 = (proj ? cs.xyz / cs.w : cs.xyz) * s3;
        switch (sop) {
        case 0x2Fu: return tx3.sample(s, c3, level(cs.w + lb));
        case 0x31u: return tx3.sample(s, c3, bias(cs.w + lb));
        case 0x19u: return tx3.sample(s, c3, gradient3d(d1.xyz * s3 * exp2(lb), d2.xyz * s3 * exp2(lb)));
        default: return tx3.sample(s, c3, bias(lb));
        }
    }
    if (kind == 1u) { // Cube
        const float3 dir = proj ? cs.xyz / cs.w : cs.xyz;
        switch (sop) {
        case 0x2Fu: return txc.sample(s, dir, level(cs.w + lb));
        case 0x31u: return txc.sample(s, dir, bias(cs.w + lb));
        case 0x19u: return txc.sample(s, dir, gradientcube(d1.xyz * exp2(lb), d2.xyz * exp2(lb)));
        default: return txc.sample(s, dir, bias(lb));
        }
    }
    float2 coord = proj ? cs.xy / cs.w : cs.xy; // Tex2D, Tex1D
    if (kind == 2u) coord = float2(coord.x, 0.5);
    coord = coord * scale;
    switch (sop) {
    case 0x2Fu: return tx2.sample(s, coord, level(cs.w + lb));
    case 0x31u: return tx2.sample(s, coord, bias(cs.w + lb));
    case 0x19u:
        return kind == 2u ? tx2.sample(s, coord, gradient2d(float2(d1.x, 0.0) * scale * exp2(lb), float2(d2.x, 0.0) * scale * exp2(lb)))
                          : tx2.sample(s, coord, gradient2d(d1.xy * scale * exp2(lb), d2.xy * scale * exp2(lb)));
    default: return tx2.sample(s, coord, bias(lb));
    }
}

struct rsx_fs_in_i {
    float4 position [[position]];
    float4 col0 [[user(col0)]]; float4 col1 [[user(col1)]];
    float4 bfc0 [[user(bfc0)]]; float4 bfc1 [[user(bfc1)]];
    float4 col0f [[user(col0f), flat]]; float4 col1f [[user(col1f), flat]];
    float4 bfc0f [[user(bfc0f), flat]]; float4 bfc1f [[user(bfc1f), flat]];
    float4 fogc [[user(fogc)]];
    float4 tc0 [[user(tc0)]]; float4 tc1 [[user(tc1)]]; float4 tc2 [[user(tc2)]]; float4 tc3 [[user(tc3)]];
    float4 tc4 [[user(tc4)]]; float4 tc5 [[user(tc5)]]; float4 tc6 [[user(tc6)]]; float4 tc7 [[user(tc7)]];
    float4 tc8 [[user(tc8)]]; float4 tc9 [[user(tc9)]];
};

// The colour outputs and depth export, compiled as variants (RSX_MRT, RSX_DEPTH), as the translator writes them.
struct rsx_fs_out_i {
#if RSX_MRT > 0 || !RSX_DEPTH
    float4 c0 [[color(0)]];
#endif
#if RSX_MRT > 1
    float4 c1 [[color(1)]];
#endif
#if RSX_MRT > 2
    float4 c2 [[color(2)]];
#endif
#if RSX_MRT > 3
    float4 c3 [[color(3)]];
#endif
#if RSX_DEPTH
    float depth [[depth(any)]];
#endif
};

fragment rsx_fs_out_i rsx_fs_interp(rsx_fs_in_i in [[stage_in]], constant float4* fc [[buffer(0)]],
    constant rsx_fs_params& fp [[buffer(1)]], constant uint* stipple [[buffer(2)]], constant uint* prog [[buffer(3)]],
    constant rsx_fi_options& o [[buffer(4)]], bool front [[front_facing]], float2 point_coord [[point_coord]],
    array<texture2d<float>, 16> t2 [[texture(0)]], array<texturecube<float>, 16> tcube [[texture(16)]],
    array<texture3d<float>, 16> t3 [[texture(32)]], array<depth2d<float>, 16> td [[texture(48)]],
    depth2d<float> depth_bounds [[texture(64)]], array<sampler, 16> smp [[sampler(0)]])
{
    // The pixel's centre (D-250, D-276), polygon stipple (D-263), the depth bounds test (D-261).
    const float2 pixel = floor(in.position.xy / float2(fp.aa & 0xffu, (fp.aa >> 8) & 0xffu)) + 0.5;
    if ((o.flags & 64u) != 0u) {
        const uint2 at = uint2(pixel) % 32u;
        if (((stipple[at.y] >> at.x) & 1u) == 0u) discard_fragment();
    }
    if ((o.flags & 32u) != 0u) {
        const float stored = depth_bounds.read(uint2(in.position.xy));
        if (stored < fp.depth_bounds_min || stored > fp.depth_bounds_max) discard_fragment();
    }
    const float4 wpos = float4(pixel.x + fp.wpos.z, pixel.y * fp.wpos.x + fp.wpos.y + fp.wpos.z, in.position.z,
                               in.position.w);
    const float in_w = 1.0 / in.position.w;
    const float4 fogc = rsx_fog(in.fogc.x, fp); // FOGC (D-256)
    const float4 ssa = float4(front ? 1.0 : -1.0);
    // COL0 and COL1: flat or not, the back colours on back faces with two-sided lighting (D-257), clamped.
    const bool flat_ = (o.flags & 2u) != 0u, two = (o.flags & 16u) != 0u;
    const float4 c0 = flat_ ? in.col0f : in.col0, c1 = flat_ ? in.col1f : in.col1;
    const float4 b0 = flat_ ? in.bfc0f : in.bfc0, b1 = flat_ ? in.bfc1f : in.bfc1;
    const float4 col0 = clamp(two && !front ? b0 : c0, 0.0, 1.0), col1 = clamp(two && !front ? b1 : c1, 0.0, 1.0);
    // The texture coordinates before the perspective multiply: a point sprite's place in its point (D-262), or with
    // the 2D control (x, y, 0, 1/w).
    const float4 tcin[10] = {in.tc0, in.tc1, in.tc2, in.tc3, in.tc4, in.tc5, in.tc6, in.tc7, in.tc8, in.tc9};
    float4 tcs[10];
    for (uint t = 0u; t < 10u; t++) {
        const bool pcoord = ((o.point_coord_mask >> t) & 1u) != 0u, d2 = ((o.texcoord_2d_mask >> t) & 1u) != 0u;
        tcs[t] = pcoord ? (d2 ? float4(point_coord, 0.0, in_w) : float4(point_coord, 1.0, 0.0))
                        : d2 ? float4(tcin[t].xy, 0.0, in_w) : tcin[t];
    }
    float4 R[64], H[64];
    for (uint i = 0u; i < 64u; i++) { R[i] = float4(0.0); H[i] = float4(0.0); }
    float4 cc[2] = {float4(0.0), float4(0.0)};
    // Flow-control blocks open now, innermost last: kind 0 IFE, 1 LOOP, 2 REP inside a loop (its index the
    // enclosing loop's), 3 REP outside one (its index its own counter).
    uint f_kind[16], f_end[16], f_else[16], f_body[16];
    int f_count[16], f_iter[16], f_init[16], f_inc[16], f_index[16];
    bool f_then[16];
    int sp = 0;
    const uint n = prog[0];
    uint pc = 0u;
    for (uint steps = 0u; steps < (1u << 22); steps++) {
        // Blocks that end here: a loop runs again or is done, an IFE is done (msl.cpp close_blocks).
        bool again = false;
        while (sp > 0 && f_end[sp - 1] == pc) {
            const int f = sp - 1;
            if (f_kind[f] != 0u && ++f_iter[f] < f_count[f]) {
                if (f_kind[f] == 1u) f_index[f] = f_init[f] + f_iter[f] * f_inc[f];
                else if (f_kind[f] == 3u) f_index[f] = f_iter[f];
                pc = f_body[f];
                again = true;
                break;
            }
            sp--;
        }
        if (again) continue;
        // An IFE whose condition held reaches its else part: on to its end.
        if (sp > 0 && f_kind[sp - 1] == 0u && f_then[sp - 1] && f_else[sp - 1] == pc) {
            pc = f_end[sp - 1];
            continue;
        }
        if (pc >= n) break;
        constant uint* w = prog + 4u + pc * 12u;
        const uint w0 = w[0], w1 = w[1];
        const uint op = w0 & 127u;
        const bool lt = ((w1 >> 11) & 1u) != 0u, eq = ((w1 >> 12) & 1u) != 0u, gt = ((w1 >> 13) & 1u) != 0u;
        const bool always = lt && eq && gt, never = !lt && !eq && !gt;
        const uint cond_reg = (w1 >> 14) & 1u, cond_mod_reg = (w1 >> 15) & 1u, cond_swz = (w1 >> 16) & 255u;
        if (op == 0x00u || op == 0x3Du || op == 0x3Eu) { pc++; continue; } // NOP, FENCT, FENCB
        if (op == 0x42u || op == 0x43u || op == 0x44u) { // IFE, LOOP, REP
            const bool c = always ? true : never ? false : any(rsx_i_fp_cond(cc[cond_reg], cond_swz, lt, eq, gt));
            const uint end_i = w[7];
            if (op == 0x42u) {
                const bool has_else = ((w[6] >> 16) & 1u) != 0u;
                if (sp < 16) {
                    f_kind[sp] = 0u; f_end[sp] = end_i; f_else[sp] = has_else ? (w[6] & 0xffffu) : 0xffffffffu;
                    f_then[sp] = c; sp++;
                }
                pc = c ? pc + 1u : (has_else ? (w[6] & 0xffffu) : end_i);
                continue;
            }
            const int count = int(w[6] & 255u), init = int((w[6] >> 8) & 255u), inc = (int((w[6] >> 16) & 255u) << 24) >> 24;
            if (!c || count == 0) { pc = end_i; continue; }
            if (sp < 16) {
                int outer = sp - 1;
                while (outer >= 0 && f_kind[outer] == 0u) outer--;
                f_kind[sp] = op == 0x43u ? 1u : outer >= 0 ? 2u : 3u;
                f_index[sp] = op == 0x43u ? init : outer >= 0 ? f_index[outer] : 0;
                f_end[sp] = end_i; f_body[sp] = pc + 1u; f_count[sp] = count; f_iter[sp] = 0;
                f_init[sp] = init; f_inc[sp] = inc; f_then[sp] = true;
                sp++;
            }
            pc++;
            continue;
        }
        int loop = sp - 1; // the innermost loop
        while (loop >= 0 && f_kind[loop] == 0u) loop--;
        const bool4 cond = rsx_i_fp_cond(cc[cond_reg], cond_swz, lt, eq, gt);
        if (op == 0x40u) { // BRK: out of the innermost loop (none: no effect, as RPCS3)
            if (loop >= 0 && !never && (always || any(cond))) {
                pc = f_end[loop];
                sp = loop;
            } else {
                pc++;
            }
            continue;
        }
        if (never) { pc++; continue; }
        if (op == 0x12u) { // KIL
            if (always || any(cond)) discard_fragment();
            pc++;
            continue;
        }
        if (op == 0x45u) { // RET: the program ends here
            if (always || any(cond)) break;
            pc++;
            continue;
        }
        // Sources (msl.cpp fp_source).
        const uint input = (w0 >> 26) & 15u, addr_reg = w[8];
        const bool use_index = ((w1 >> 6) & 1u) != 0u, persp = ((w1 >> 5) & 1u) != 0u;
        float4 src[3];
        for (uint k = 0u; k < 3u; k++) {
            const uint d = w[2u + k];
            uint prec = (d >> 19) & 7u;
            float4 x = float4(0.0);
            switch (d & 3u) {
            case 0u: // a temp, R or H
                if (((d >> 8) & 1u) != 0u) {
                    x = H[(d >> 2) & 63u];
                    if (prec == 1u) prec = 0u; // half precision on a half register is a no-op
                } else {
                    x = R[(d >> 2) & 63u];
                }
                break;
            case 1u: { // an input
                uint id = input;
                bool done = false;
                if (use_index) {
                    if (loop >= 0) { // the loop index picks the texture coordinate (RPCS3 _indexed_load)
                        const int i = f_index[loop] + int(addr_reg);
                        x = uint(i) < 10u ? tcs[i] : float4(0.0, 0.0, 0.0, 1.0);
                        if (persp) x = x * in.position.w;
                        done = true;
                    } else {
                        id = addr_reg + 4u;
                    }
                }
                if (done) break;
                if (id == 0u) {
                    x = wpos;
                } else if (id == 1u || id == 2u) {
                    x = id == 1u ? col0 : col1;
                    prec = 0u;
                } else if (id == 3u) {
                    x = fogc;
                } else if (id >= 4u && id <= 13u) {
                    x = tcs[id - 4u];
                    if (((o.point_coord_mask >> (id - 4u)) & 1u) == 0u && persp) x = x * in.position.w;
                } else if (id == 14u) {
                    x = ssa;
                    prec = 0u;
                }
                break;
            }
            case 2u: x = fc[w[5]]; break; // the instruction's embedded constant
            default: break;
            }
            x = rsx_i_swizzle(x, (d >> 9) & 255u);
            // Modifier order: saturate before abs, the other precisions after it, negation last (Naruto UNS).
            if (prec == 4u) x = rsx_i_prec(x, 4u);
            if (((d >> 18) & 1u) != 0u) x = abs(x);
            if (prec != 0u && prec != 4u) x = rsx_i_prec(x, prec);
            if (((d >> 17) & 1u) != 0u) x = -x;
            src[k] = x;
        }
        const float4 s0 = src[0], s1 = src[1], s2 = src[2];
        const float4 bem = s0.xyxy + s1.xxxx * s2.xzxz + s1.yyyy * s2.ywyw; // BEM (X2D)
        float4 e = float4(0.0);
        bool exempt = false; // ops RPCS3 SetDst does not precision-clamp
        switch (op) {
        case 0x01u: e = s0; break;                                                    // MOV
        case 0x02u: e = s0 * s1; break;                                               // MUL
        case 0x03u: e = s0 + s1; break;                                               // ADD
        case 0x04u: e = fma(s0, s1, s2); break;                                       // MAD
        case 0x38u: e = float4(dot(s0.xy, s1.xy)); break;                             // DP2
        case 0x05u: e = float4(dot(s0.xyz, s1.xyz)); break;                           // DP3
        case 0x06u: e = float4(dot(s0, s1)); break;                                   // DP4
        case 0x08u: e = min(s0, s1); exempt = true; break;                            // MIN
        case 0x09u: e = max(s0, s1); exempt = true; break;                            // MAX
        case 0x3Au: e = s0 / s1.x; break;                                             // DIV
        case 0x3Bu: e = rsx_divsq(s0, s1.x); break;                                   // DIVSQ
        case 0x29u: e = float4(as_type<float>(rsx_pack_unorm2x16(s0.xy))); break;      // PK16 (D-942)
        case 0x2Au: e = unpack_unorm2x16_to_float(as_type<uint>(s0.x)).xyxy; break;    // UP16
        case 0x27u: e = float4(as_type<float>(rsx_pack_unorm4x8(s0))); break;          // PKB
        case 0x28u: e = unpack_unorm4x8_to_float(as_type<uint>(s0.x)); break;          // UPB
        case 0x13u: e = float4(as_type<float>(rsx_pack_snorm4x8(s0))); break;          // PK4
        case 0x14u: e = unpack_snorm4x8_to_float(as_type<uint>(s0.x)); break;          // UP4
        case 0x24u: e = float4(as_type<float>(half2(s0.xy))); break;                  // PK2
        case 0x25u: e = float2(as_type<half2>(as_type<uint>(s0.x))).xyxy; break;       // UP2
        case 0x2Cu: e = float4(as_type<float>(rsx_pack_unorm4x8(rsx_linear_to_srgb(s0)))); break; // PKG
        case 0x2Du: e = rsx_srgb_to_linear(unpack_unorm4x8_to_float(as_type<uint>(s0.x))); break;      // UPG
        case 0x1Eu: e = rsx_lit(s0); exempt = true; break;                            // LIT
        case 0x3Cu: e = rsx_lif(s0); exempt = true; break;                            // LIF
        case 0x07u: e = float4(1.0, s0.y * s1.y, s0.z, s1.w); break;                  // DST
        case 0x2Eu: e = float4(dot(s0.xy, s1.xy) + s2.x); break;                      // DP2A
        case 0x36u: e = rsx_refl(s0, s1); exempt = true; break;                       // REFL
        case 0x2Bu: e = bem; break;                                                   // BEM
        case 0x1Au: e = float4(1.0 / s0.x); break;                                    // RCP
        case 0x1Bu: e = float4(1.0 / sqrt(abs(s0.x))); break;                         // RSQ: the sign ignored
        case 0x1Cu: e = float4(exp2(s0.x)); break;                                    // EX2
        case 0x1Du: e = float4(log2(s0.x)); exempt = true; break;                     // LG2
        case 0x23u: e = float4(sin(s0.x)); exempt = true; break;                      // SIN
        case 0x22u: e = float4(cos(s0.x)); exempt = true; break;                      // COS
        case 0x10u: e = fract(s0); exempt = true; break;                              // FRC
        case 0x11u: e = floor(s0); break;                                             // FLR
        case 0x0Au: e = select(float4(0.0), float4(1.0), s0 < s1); break;             // SLT
        case 0x0Bu: e = select(float4(0.0), float4(1.0), s0 >= s1); break;            // SGE
        case 0x0Cu: e = select(float4(0.0), float4(1.0), s0 <= s1); break;            // SLE
        case 0x0Du: e = select(float4(0.0), float4(1.0), s0 > s1); break;             // SGT
        case 0x0Eu: e = select(float4(0.0), float4(1.0), s0 != s1); break;            // SNE
        case 0x0Fu: e = select(float4(0.0), float4(1.0), s0 == s1); break;            // SEQ
        case 0x20u: e = float4(1.0); break;                                           // STR
        case 0x21u: e = float4(0.0); break;                                           // SFL
        case 0x1Fu: e = mix(s2, s1, s0); break;                                       // LRP
        case 0x39u: e = rsx_nrm(s0); exempt = true; break;                            // NRM
        case 0x15u: e = dfdx(s0); break;                                              // DDX
        case 0x16u: e = dfdy(s0); break;                                              // DDY
        case 0x17u: case 0x18u: case 0x2Fu: case 0x31u: case 0x19u: case 0x33u: case 0x34u: { // texture lookups
            const uint t = w1 & 15u;
            const bool bx2 = ((w1 >> 4) & 1u) != 0u;
            const uint kind = o.texture_kind[t];
            const float4 cs = op == 0x33u || op == 0x34u ? bem : s0; // TEXBEM, TXPBEM: BEM's coordinate
            const uint sop = op == 0x33u ? 0x17u : op == 0x34u ? 0x18u : op;
            e = rsx_i_sample(kind, sop, t, cs, s1, s2, fp, t2[t], tcube[t], t3[t], td[t], smp[t]);
            if (kind != 3u && kind != 6u) {
                // RPCS3 _process_texel: the alpha kill on the texel as sampled, then its conversions (D-174).
                if (((o.texture_alpha_kill_mask >> t) & 1u) != 0u && e.w < 0.000001) discard_fragment();
                if (((o.texture_convert_mask >> t) & 1u) != 0u || bx2) e = rsx_texel(e, fp.tex_control[t], bx2);
            }
            break;
        }
        default: break;
        }
        // Result modifiers (SetDst): the scale, then with a destination saturate or the destination precision.
        const uint scale = (w0 >> 23) & 7u;
        if (scale == 1u) e = e * 2.0;
        else if (scale == 2u) e = e * 4.0;
        else if (scale == 3u) e = e * 8.0;
        else if (scale == 5u) e = e / 2.0;
        else if (scale == 6u) e = e / 4.0;
        else if (scale == 7u) e = e / 8.0;
        const bool no_dest = ((w0 >> 18) & 1u) != 0u, dst_fp16 = ((w0 >> 13) & 1u) != 0u;
        const uint prec = (w0 >> 20) & 3u;
        if (!no_dest && ((w0 >> 19) & 1u) != 0u) e = rsx_i_prec(e, 4u);
        else if (!no_dest && prec != 0u && !exempt && !(prec == 1u && !dst_fp16)) e = rsx_i_prec(e, prec);
        const uint mask = (w0 >> 14) & 15u, dst = (w0 >> 7) & 63u;
        const bool set_cond = ((w0 >> 22) & 1u) != 0u;
        const bool4 cm = always ? bool4(true) : cond;
        if (!no_dest && mask != 0u) {
            thread float4& reg = dst_fp16 ? H[dst] : R[dst];
            rsx_i_assign(reg, e, mask, cm);
            if (set_cond) rsx_i_assign(cc[cond_mod_reg], reg, mask, bool4(true));
        } else if (no_dest && set_cond && mask != 0u && (always || any(cond))) {
            rsx_i_assign(cc[cond_mod_reg], e, mask, bool4(true));
        }
        pc++;
    }

    // The outputs: R0/R2/R3/R4 with 32-bit exports, H0/H4/H6/H8 otherwise, the other file's register when the program
    // wrote only that one (msl.cpp); depth from R1.z.
    const bool fp32 = (o.shader_control & 0x40u) != 0u;
    const uint rw = prog[1], hw = prog[2];
    const uint kr[4] = {0u, 2u, 3u, 4u}, kh[4] = {0u, 4u, 6u, 8u};
    float4 outs[4];
    for (uint i = 0u; i < 4u; i++) {
        const bool r_ok = ((rw >> kr[i]) & 1u) != 0u, h_ok = ((hw >> kh[i]) & 1u) != 0u;
        outs[i] = (fp32 ? (r_ok || !h_ok) : !(h_ok || !r_ok)) ? R[kr[i]] : H[kh[i]];
    }
    // The ROP in RPCS3's order (D-174): sRGB, the alpha test and alpha to coverage on colour 0's alpha, the outputs.
    const uint mrt = min(o.mrt_count, 4u);
    if ((o.flags & 4u) != 0u) {
        for (uint i = 0u; i < mrt; i++) outs[i].xyz = rsx_linear_to_srgb(outs[i]).xyz;
    }
    if ((o.flags & 1u) != 0u && !rsx_compare(fp.alpha_func, outs[0].w, fp.alpha_ref)) discard_fragment();
    if ((o.flags & 8u) != 0u && !(outs[0].w > fract(sin(dot(in.position.xy, float2(12.9898, 78.233))) * 43758.5453))) {
        discard_fragment(); // alpha to coverage
    }
    rsx_fs_out_i res;
#if RSX_MRT > 0
    res.c0 = rsx_i_out_swizzle(outs[0], o.output_swizzle[0]);
#elif !RSX_DEPTH
    res.c0 = float4(0.0);
#endif
#if RSX_MRT > 1
    res.c1 = rsx_i_out_swizzle(outs[1], o.output_swizzle[1]);
#endif
#if RSX_MRT > 2
    res.c2 = rsx_i_out_swizzle(outs[2], o.output_swizzle[2]);
#endif
#if RSX_MRT > 3
    res.c3 = rsx_i_out_swizzle(outs[3], o.output_swizzle[3]);
#endif
#if RSX_DEPTH
    // An exported depth, or under the depth clamp the rasterized one, for Metal to clamp (D-941).
    res.depth = (o.shader_control & 0xeu) != 0u ? R[1].z : in.position.z;
#endif
    return res;
}
)MSL";

} // namespace

InterpVsOptions interp_vs_options(const VertexProgram& vp, const VpOptions& options)
{
    InterpVsOptions o;
    o.start = vp.start;
    o.input_mask = vp.input_mask;
    o.output_mask = vp.output_mask;
    o.user_clip = options.user_clip;
    o.masked_outputs = options.masked_outputs;
    o.point_size = bit(options.point_size);
    for (u32 i = 0; i < 4; i++) o.texture_kind |= static_cast<u32>(options.texture_kind[i]) << (8 * i);
    return o;
}

InterpFsOptions interp_fs_options(const FpOptions& options)
{
    InterpFsOptions o;
    o.shader_control = options.shader_control;
    o.texcoord_2d_mask = options.texcoord_2d_mask;
    o.flags = (options.alpha_test ? kFiAlphaTest : 0) | (options.flat_colors ? kFiFlatColors : 0) |
              (options.srgb_output ? kFiSrgbOutput : 0) | (options.alpha_to_coverage ? kFiAlphaToCoverage : 0) |
              (options.two_sided_lighting ? kFiTwoSidedLighting : 0) | (options.depth_bounds ? kFiDepthBounds : 0) |
              (options.polygon_stipple ? kFiPolygonStipple : 0);
    o.texture_convert_mask = options.texture_convert_mask;
    o.texture_alpha_kill_mask = options.texture_alpha_kill_mask;
    o.point_coord_mask = options.point_coord_mask;
    o.mrt_count = options.mrt_count;
    for (u32 t = 0; t < 16; t++) o.texture_kind[t] = static_cast<u32>(options.texture_kind[t]);
    for (u32 i = 0; i < 4; i++) {
        const auto& sw = options.output_swizzle[i];
        o.output_swizzle[i] = u32{sw[0]} | (u32{sw[1]} << 8) | (u32{sw[2]} << 16) | (u32{sw[3]} << 24);
    }
    return o;
}

EncodeResult encode_vertex_program(const VertexProgram& vp)
{
    EncodeResult r;
    for (const VpInstruction& in : vp.instructions) {
        // RPCS3 Decompile, as msl.cpp: an instruction with an invalid source type ends the program.
        if (in.src[0].type == VpRegType::Invalid || in.src[1].type == VpRegType::Invalid ||
            in.src[2].type == VpRegType::Invalid) {
            break;
        }
        const auto fail = [&](const char* name) {
            r.error = std::format("unsupported vertex program opcode {} at instruction {} ({})", name, in.slot,
                disassemble(in));
            r.words.clear();
            return r;
        };
        if (!vp_vec_op_supported(in.vec_op)) return fail(vp_vec_op_name(in.vec_op));
        if (!vp_sca_op_supported(in.sca_op)) return fail(vp_sca_op_name(in.sca_op));
        const u32 w0 = (in.vec_op & 31u) | ((in.sca_op & 31u) << 5) | (bit(in.saturate) << 10) |
                       (bit(in.vec_result) << 11) | (bit(in.cond_test) << 12) | (bit(in.cond_update) << 13) |
                       (bit(in.constant_relative) << 14) | (bit(in.end) << 15) | ((in.cond & 7u) << 16) |
                       ((in.cond_reg & 1u) << 19) | ((in.addr_reg & 1u) << 20) | ((in.addr_swizzle & 3u) << 21) |
                       ((in.tex_num & 3u) << 23);
        const u32 w1 = (in.vec_mask & 15u) | ((in.sca_mask & 15u) << 4) | ((in.out & 31u) << 8) |
                       ((in.vec_temp & 63u) << 13) | ((in.sca_temp & 63u) << 19) | ((in.input & 15u) << 25);
        const u32 w2 = (in.constant & 1023u) | (swizzle8(in.cond_swizzle) << 10) | ((in.branch_target & 1023u) << 18);
        // BRB and CLB: bit D3.branch_index (23-27) of the branch bits, taken when set or, with D3.brb_cond_true (28)
        // clear, when clear (msl.cpp vp_boolean_condition).
        const u32 w6 = (in.slot & 1023u) | (((in.words[3] >> 23) & 31u) << 10) | (((in.words[3] >> 28) & 1u) << 15);
        r.words.insert(r.words.end(), {w0, w1, w2, vp_source_word(in.src[0]), vp_source_word(in.src[1]),
                                          vp_source_word(in.src[2]), w6, 0u});
    }
    r.ok = true;
    return r;
}

EncodeResult encode_fragment_program(const FragmentProgram& fp)
{
    EncodeResult r;
    const u32 n = static_cast<u32>(fp.instructions.size());
    std::map<u32, u32> index_of; // byte offset -> instruction
    for (u32 i = 0; i < n; i++) index_of.emplace(fp.instructions[i].offset, i);
    // A block's end or else at no instruction's offset: the end of the program (msl.cpp closes such blocks there).
    const auto index = [&](u32 offset) {
        const auto it = index_of.find(offset);
        return it == index_of.end() ? n : it->second;
    };
    r.words.assign(kFpHeaderWords, 0u);
    r.words[0] = n;
    struct Block {
        u32 end;
        bool loop;
    };
    std::vector<Block> blocks; // as msl.cpp tracks them: whether an instruction is inside a loop
    u32 const_slot = 0;
    for (const FpInstruction& in : fp.instructions) {
        const u32 constant_index = const_slot;
        if (in.has_constant) const_slot++;
        for (auto it = blocks.end(); it != blocks.begin();) {
            --it;
            if (it->end == in.offset) it = blocks.erase(it);
        }
        if (!fp_op_supported(in.op)) {
            r.error = std::format("unsupported fragment program opcode {} at offset {:#06x} ({})", fp_op_name(in.op),
                in.offset, disassemble(in));
            r.words.clear();
            return r;
        }
        const FpOp op = static_cast<FpOp>(in.op);
        u32 w6 = 0, w7 = 0;
        if (op == FpOp::IFE || op == FpOp::LOOP || op == FpOp::REP) {
            const u32 else_offset = (in.words[2] & 0x7fffffffu) << 2, end_offset = (in.words[3] & 0x7fffffffu) << 2;
            w7 = index(end_offset);
            if (op == FpOp::IFE) {
                if (else_offset != end_offset && index_of.contains(else_offset)) w6 = index(else_offset) | (1u << 16);
            } else {
                // SRC1: the loop count (bits 2-9), the index's initial value (10-17) and increment (19-26, signed).
                const u32 s1w = in.words[2];
                w6 = ((s1w >> 2) & 0xffu) | (((s1w >> 10) & 0xffu) << 8) | (((s1w >> 19) & 0xffu) << 16);
            }
            blocks.push_back({end_offset, op != FpOp::IFE});
        } else if (op != FpOp::NOP && op != FpOp::FENCT && op != FpOp::FENCB && op != FpOp::BRK && !in.never() &&
                   op != FpOp::KIL && op != FpOp::RET) {
            // msl.cpp fails an input indexed outside a loop past SSA, where the operation reads it.
            const bool in_loop = std::any_of(blocks.begin(), blocks.end(), [](const Block& b) { return b.loop; });
            const u32 read = fp_sources_read(op);
            for (u32 k = 0; k < 3; k++) {
                if (!((read >> k) & 1u) || in.src[k].type != FpRegType::Input || !in.use_index_reg || in_loop) continue;
                if (in.addr_reg + 4u > static_cast<u32>(FpInput::SSA)) {
                    r.error = std::format("unsupported fragment program input TEX0 + {} (indexed, outside a loop: past "
                                          "SSA) at offset {:#06x} ({})", in.addr_reg, in.offset, disassemble(in));
                    r.words.clear();
                    return r;
                }
            }
            // The registers the outputs can come from, as msl.cpp counts them written.
            if (!in.no_dest && in.mask && in.dst < 32) r.words[in.dst_fp16 ? 2 : 1] |= 1u << in.dst;
        }
        const u32 w0 = (in.op & 127u) | ((in.dst & 63u) << 7) | (bit(in.dst_fp16) << 13) | ((in.mask & 15u) << 14) |
                       (bit(in.no_dest) << 18) | (bit(in.saturate) << 19) | ((in.prec & 3u) << 20) |
                       (bit(in.set_cond) << 22) | ((in.scale & 7u) << 23) | ((in.input & 15u) << 26) |
                       (bit(in.end) << 30);
        const u32 w1 = (in.tex & 15u) | (bit(in.tex_bx2) << 4) | (bit(in.perspective_correct) << 5) |
                       (bit(in.use_index_reg) << 6) | (bit(in.exec_lt) << 11) | (bit(in.exec_eq) << 12) |
                       (bit(in.exec_gt) << 13) | ((in.cond_reg & 1u) << 14) | ((in.cond_mod_reg & 1u) << 15) |
                       (swizzle8(in.cond_swizzle) << 16) | (bit(in.has_constant) << 24);
        r.words.insert(r.words.end(), {w0, w1, fp_source_word(in.src[0]), fp_source_word(in.src[1]),
                                          fp_source_word(in.src[2]), constant_index, w6, w7, in.addr_reg, 0u, 0u, 0u});
    }
    r.ok = true;
    return r;
}

const std::string& interpreter_source()
{
    static const std::string s = msl_prologue() + kInterpreter;
    return s;
}

} // namespace pspocket::rsx::shader
