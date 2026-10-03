// RSX vertex and fragment programs -> MSL. See msl.hpp for the supported subset. Operation semantics cite RPCS3
// rpcs3/Emu/RSX/Program/VertexProgramDecompiler.cpp (VP) and FragmentProgramDecompiler.cpp (FP).

#include "pspocket/rsx/shader/msl.hpp"

#include <format>
#include <set>

#include "pspocket/core/be.h"

namespace pspocket::rsx::shader {
namespace {

constexpr const char* kPrologue = R"MSL(#include <metal_stdlib>
using namespace metal;

// Mirrors pspocket::rsx::shader::MslAttrib / MslVsParams / MslFsParams.
struct rsx_attrib {
    uint offset; uint stride; uint type; uint size; uint divisor; uint flags; uint first; uint pad;
    float4 value;
};

struct rsx_vs_params {
    float4 pos_x; float4 pos_y; float4 pos_z;
    uint index_mode; uint index_offset; uint base_index; uint first_vertex;
    uint restart_enable; uint restart_index; uint pad0; uint pad1;
    rsx_attrib attribs[16];
};

struct rsx_fs_params {
    float4 wpos;
    float4 tex_scale[16];
    uint alpha_func; float alpha_ref; uint pad0; uint pad1;
};

static inline uint rsx_ld8(device const uchar* b, uint o) { return uint(b[o]); }
static inline uint rsx_ld16(device const uchar* b, uint o, bool swap)
{
    const uint b0 = b[o], b1 = b[o + 1];
    return swap ? ((b0 << 8) | b1) : (b0 | (b1 << 8));
}
static inline uint rsx_ld32(device const uchar* b, uint o, bool swap)
{
    const uint b0 = b[o], b1 = b[o + 1], b2 = b[o + 2], b3 = b[o + 3];
    return swap ? ((b0 << 24) | (b1 << 16) | (b2 << 8) | b3) : (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
}
static inline float rsx_sext16(uint v) { return float(int(v << 16) >> 16); }

// Vertex pulling: RPCS3 GLSLSnippets/RSXProg/RSXVertexFetch.glsl (fetch_attribute, read_location).
static float4 rsx_fetch(constant rsx_attrib& a, uint vertex_index, device const uchar* vb)
{
    if ((a.flags & 4u) != 0u || a.type == 0u) return a.value;
    uint element;
    if (a.stride == 0u) element = a.first;
    else if (a.divisor > 1u) element = (a.flags & 1u) != 0u ? (vertex_index % a.divisor) : (vertex_index / a.divisor);
    else element = vertex_index;
    const uint base = a.offset + (element - a.first) * a.stride;
    const bool swap = (a.flags & 2u) != 0u;
    float4 r = float4(0.0, 0.0, 0.0, 1.0);
    switch (a.type) {
    case 1u: // S1: signed normalised 16-bit, [-32768, 32767] -> [-1, 1]
        for (uint i = 0; i < a.size; i++) r[i] = (rsx_sext16(rsx_ld16(vb, base + i * 2u, swap)) + 0.5) / 32767.5;
        break;
    case 2u: // F: float32
        for (uint i = 0; i < a.size; i++) r[i] = as_type<float>(rsx_ld32(vb, base + i * 4u, swap));
        break;
    case 3u: // SF: float16
        for (uint i = 0; i < a.size; i++) r[i] = float(as_type<half>(ushort(rsx_ld16(vb, base + i * 2u, swap))));
        break;
    case 4u: // UB: unsigned normalised 8-bit (bytes are never swapped)
        for (uint i = 0; i < a.size; i++) r[i] = float(rsx_ld8(vb, base + i)) / 255.0;
        break;
    case 5u: // S32K: signed 16-bit
        for (uint i = 0; i < a.size; i++) r[i] = rsx_sext16(rsx_ld16(vb, base + i * 2u, swap));
        break;
    case 6u: { // CMP: 11:11:10 signed normalised in one word, w = 1
        const uint v = rsx_ld32(vb, base, swap);
        r = float4(rsx_sext16((v & 0x7ffu) << 5), rsx_sext16(((v >> 11) & 0x7ffu) << 5),
                   rsx_sext16(((v >> 22) & 0x3ffu) << 6), 32767.0) / 32767.0;
        break;
    }
    case 7u: // UB256: unsigned 8-bit
        for (uint i = 0; i < a.size; i++) r[i] = float(rsx_ld8(vb, base + i));
        break;
    }
    return r;
}

// Index pulling: big-endian 16/32-bit indices, plus the base index (rsx_utils.h get_index_from_base).
static uint rsx_vertex_index(uint vid, constant rsx_vs_params& p, device const uchar* ib)
{
    switch (p.index_mode) {
    case 1u: return (rsx_ld16(ib, p.index_offset + vid * 2u, true) + p.base_index) & 0xfffffu;
    case 2u: return (rsx_ld32(ib, p.index_offset + vid * 4u, true) + p.base_index) & 0xfffffu;
    case 3u: return (rsx_ld32(ib, p.index_offset + vid * 4u, false) + p.base_index) & 0xfffffu;
    default: return p.first_vertex + vid;
    }
}

// CompareFunc low bits: never, less, equal, lequal, greater, notequal, gequal, always.
static bool rsx_compare(uint func, float a, float b)
{
    switch (func & 7u) {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    default: return true;
    }
}
)MSL";

constexpr const char* kXyzw = "xyzw";

std::string mask_suffix(u8 mask)
{
    if (mask == 0xf) return "";
    std::string s = ".";
    for (u32 i = 0; i < 4; i++) {
        if (mask & (1u << i)) s += kXyzw[i];
    }
    return s;
}

std::string swizzle_suffix(const Swizzle& s)
{
    if (s.identity()) return "";
    std::string r = ".";
    for (u8 c : s.c) r += kXyzw[c & 3];
    return r;
}

// ---- vertex programs --------------------------------------------------------------------------------------------

bool vp_vec_supported(u32 op)
{
    switch (static_cast<VpVecOp>(op)) {
    case VpVecOp::NOP: case VpVecOp::MOV: case VpVecOp::MUL: case VpVecOp::ADD: case VpVecOp::MAD:
    case VpVecOp::DP3: case VpVecOp::DPH: case VpVecOp::DP4: case VpVecOp::DST: case VpVecOp::MIN:
    case VpVecOp::MAX: case VpVecOp::SLT: case VpVecOp::SGE: case VpVecOp::SEQ: case VpVecOp::SNE:
    case VpVecOp::SGT: case VpVecOp::SLE: case VpVecOp::SFL: case VpVecOp::STR: case VpVecOp::FRC:
    case VpVecOp::FLR: case VpVecOp::SSG: return true;
    default: return false;
    }
}

bool vp_sca_supported(u32 op)
{
    switch (static_cast<VpScaOp>(op)) {
    case VpScaOp::NOP: case VpScaOp::MOV: case VpScaOp::RCP: case VpScaOp::RCC: case VpScaOp::RSQ:
    case VpScaOp::EX2: case VpScaOp::LG2: case VpScaOp::SIN: case VpScaOp::COS: return true;
    default: return false;
    }
}

std::string vp_source(const VpInstruction& in, u32 n, std::set<u32>& temps)
{
    const VpSource& s = in.src[n];
    std::string r;
    switch (s.type) {
    case VpRegType::Temp:
        temps.insert(s.temp);
        r = std::format("r{}", s.temp);
        break;
    case VpRegType::Input: r = std::format("v{}", in.input); break;
    case VpRegType::Constant: r = std::format("vc[{}]", in.constant); break;
    default: r = "float4(0.0)"; break;
    }
    r += swizzle_suffix(s.swizzle);
    if (s.abs) r = "abs(" + r + ")";
    if (s.neg) r = "-(" + r + ")";
    return r;
}

std::string vp_compare(const char* op, const std::string& a, const std::string& b)
{
    return std::format("select(float4(0.0), float4(1.0), {} {} {})", a, op, b);
}

} // namespace

const std::string& msl_prologue()
{
    static const std::string s = kPrologue;
    return s;
}

TranslateResult translate_vertex_program(const VertexProgram& vp, const VpOptions& options)
{
    TranslateResult result;
    std::set<u32> temps;
    std::string body;

    for (const VpInstruction& in : vp.instructions) {
        auto fail = [&](std::string what) {
            result.error = std::format("unsupported vertex program {} at instruction {} ({})", what, in.slot, disassemble(in));
            return result;
        };
        // RPCS3 Decompile: an instruction with an invalid source type ends the program as a NOP.
        if (in.src[0].type == VpRegType::Invalid || in.src[1].type == VpRegType::Invalid ||
            in.src[2].type == VpRegType::Invalid) {
            body += std::format("    // {}: invalid source type, program ends\n", in.slot);
            break;
        }
        if (!vp_vec_supported(in.vec_op)) return fail(std::format("opcode {}", vp_vec_op_name(in.vec_op)));
        if (!vp_sca_supported(in.sca_op)) return fail(std::format("opcode {}", vp_sca_op_name(in.sca_op)));
        if (in.constant_relative) return fail("relative constant addressing (A0)");
        if (in.cond_update) return fail("condition code update");
        if (in.cond_test && in.cond != 0 && in.cond != 7) return fail("conditional write");

        const bool vec = in.vec_op != 0;
        const bool sca = in.sca_op != 0;
        if ((!vec && !sca) || in.cond == 0) continue; // NOP, or never written (SetDST: cond == 0)

        body += std::format("    {{ // {}: {}\n", in.slot, disassemble(in));
        if (vec) {
            const std::string s0 = vp_source(in, 0, temps), s1 = vp_source(in, 1, temps), s2 = vp_source(in, 2, temps);
            std::string e;
            switch (static_cast<VpVecOp>(in.vec_op)) {
            case VpVecOp::MOV: e = s0; break;
            case VpVecOp::MUL: e = std::format("({}) * ({})", s0, s1); break;
            case VpVecOp::ADD: e = std::format("({}) + ({})", s0, s2); break; // VP ADD reads src0 and src2
            case VpVecOp::MAD: e = std::format("fma({}, {}, {})", s0, s1, s2); break;
            case VpVecOp::DP3: e = std::format("float4(dot(({}).xyz, ({}).xyz))", s0, s1); break;
            case VpVecOp::DPH: e = std::format("float4(dot(({}).xyz, ({}).xyz) + ({}).w)", s0, s1, s1); break;
            case VpVecOp::DP4: e = std::format("float4(dot({}, {}))", s0, s1); break;
            case VpVecOp::DST: e = std::format("float4(1.0, ({}).y * ({}).y, ({}).z, ({}).w)", s0, s1, s0, s1); break;
            case VpVecOp::MIN: e = std::format("min({}, {})", s0, s1); break;
            case VpVecOp::MAX: e = std::format("max({}, {})", s0, s1); break;
            case VpVecOp::SLT: e = vp_compare("<", s0, s1); break;
            case VpVecOp::SGE: e = vp_compare(">=", s0, s1); break;
            case VpVecOp::SEQ: e = vp_compare("==", s0, s1); break;
            case VpVecOp::SNE: e = vp_compare("!=", s0, s1); break;
            case VpVecOp::SGT: e = vp_compare(">", s0, s1); break;
            case VpVecOp::SLE: e = vp_compare("<=", s0, s1); break;
            case VpVecOp::SFL: e = "float4(0.0)"; break;
            case VpVecOp::STR: e = "float4(1.0)"; break;
            case VpVecOp::FRC: e = std::format("fract({})", s0); break;
            case VpVecOp::FLR: e = std::format("floor({})", s0); break;
            case VpVecOp::SSG: e = std::format("sign({})", s0); break;
            default: break;
            }
            body += std::format("        float4 vec = {};\n", e);
            if (in.saturate) body += "        vec = clamp(vec, 0.0, 1.0);\n";
        }
        if (sca) {
            const std::string s = "(" + vp_source(in, 2, temps) + ").x"; // scalar ops read src2 (RPCS3 "$s")
            std::string e;
            switch (static_cast<VpScaOp>(in.sca_op)) {
            case VpScaOp::MOV: e = s; break;
            case VpScaOp::RCP: e = std::format("1.0 / {}", s); break;
            case VpScaOp::RCC: e = std::format("clamp(1.0 / {}, 5.42101e-20, 1.884467e19)", s); break;
            case VpScaOp::RSQ: e = std::format("rsqrt(max({}, 1e-10))", s); break;
            case VpScaOp::EX2: e = std::format("exp2({})", s); break;
            case VpScaOp::LG2: e = std::format("log2(max({}, 1e-10))", s); break;
            case VpScaOp::SIN: e = std::format("sin({})", s); break;
            case VpScaOp::COS: e = std::format("cos({})", s); break;
            default: break;
            }
            body += std::format("        float4 sca = float4({});\n", e);
            if (in.saturate) body += "        sca = clamp(sca, 0.0, 1.0);\n";
        }
        // Both ops read their sources before either writes (dual issue).
        auto write = [&](const char* value, u8 mask, bool to_output, u8 temp) {
            if (mask == 0) return;
            const std::string m = mask_suffix(mask);
            if (to_output && in.out != 0x1f) body += std::format("        o{}{} = {}{};\n", in.out, m, value, m);
            if (temp != 0x3f) {
                temps.insert(temp);
                body += std::format("        r{}{} = {}{};\n", temp, m, value, m);
            }
        };
        if (vec) write("vec", in.vec_mask, in.vec_result, in.vec_temp);
        if (sca) write("sca", in.sca_mask, !in.vec_result, in.sca_temp);
        body += "    }\n";
    }

    std::string src = msl_prologue();
    src += "\nstruct rsx_vs_out {\n    float4 position [[position]];\n";
    src += "    float4 col0 [[user(col0)]];\n    float4 col1 [[user(col1)]];\n";
    src += "    float4 bfc0 [[user(bfc0)]];\n    float4 bfc1 [[user(bfc1)]];\n";
    src += "    float4 fogc [[user(fogc)]];\n";
    for (u32 i = 0; i < 10; i++) src += std::format("    float4 tc{} [[user(tc{})]];\n", i, i);
    if (options.point_size) src += "    float point_size [[point_size]];\n";
    src += "};\n\n";

    result.shader.entry = std::format("rsx_vp_{:016x}", vp.hash);
    src += std::format(
        "vertex rsx_vs_out {}(uint vid [[vertex_id]], device const uchar* vb [[buffer({})]],\n"
        "    constant rsx_vs_params& p [[buffer({})]], constant float4* vc [[buffer({})]],\n"
        "    device const uchar* ib [[buffer({})]])\n{{\n",
        result.shader.entry, slot::kVsVertexData, slot::kVsParams, slot::kVsConstants, slot::kVsIndexData);
    src += "    const uint vi = rsx_vertex_index(vid, p, ib);\n";
    for (u32 i = 0; i < 16; i++) {
        if (vp.input_mask & (1u << i)) {
            src += std::format("    const float4 v{} = rsx_fetch(p.attribs[{}], vi, vb);\n", i, i);
            result.shader.input_mask |= 1u << i;
        }
    }
    for (u32 t : temps) src += std::format("    float4 r{} = float4(0.0);\n", t);
    for (u32 i = 0; i < 16; i++) src += std::format("    float4 o{} = float4(0.0, 0.0, 0.0, 1.0);\n", i);
    src += body;
    // Outputs: o0 position, o1/o2 front colours, o3/o4 back colours, o5.x fog, o6.x point size (o6 also TEX9),
    // o7-o14 TEX0-TEX7, o15 TEX8 (RPCS3 GL/GLVertexProgram.cpp register table).
    src += "    rsx_vs_out out;\n";
    src += "    out.position = float4(dot(p.pos_x, o0), dot(p.pos_y, o0), dot(p.pos_z, o0), o0.w);\n";
    src += "    out.col0 = o1;\n    out.col1 = o2;\n    out.bfc0 = o3;\n    out.bfc1 = o4;\n    out.fogc = o5.xxxx;\n";
    for (u32 i = 0; i < 8; i++) src += std::format("    out.tc{} = o{};\n", i, 7 + i);
    src += "    out.tc8 = o15;\n    out.tc9 = o6;\n";
    if (options.point_size) src += "    out.point_size = o6.x;\n";
    src += "    return out;\n}\n";

    result.shader.source = std::move(src);
    result.ok = true;
    return result;
}

// ---- fragment programs ------------------------------------------------------------------------------------------

namespace {

struct FpContext {
    const FragmentProgram& fp;
    const FpOptions& opt;
    std::set<u32> r_regs, h_regs;   // registers referenced
    std::set<u32> r_written, h_written;
    u32 inputs = 0;
    u32 textures = 0;
    u32 constant_index = 0; // fc[] slot of the current instruction's constant
};

bool fp_supported(u32 op)
{
    switch (static_cast<FpOp>(op)) {
    case FpOp::NOP: case FpOp::MOV: case FpOp::MUL: case FpOp::ADD: case FpOp::MAD: case FpOp::DP2:
    case FpOp::DP3: case FpOp::DP4: case FpOp::MIN: case FpOp::MAX: case FpOp::DIV: case FpOp::RCP:
    case FpOp::RSQ: case FpOp::EX2: case FpOp::LG2: case FpOp::SIN: case FpOp::COS: case FpOp::FRC:
    case FpOp::FLR: case FpOp::SLT: case FpOp::SGE: case FpOp::SLE: case FpOp::SGT: case FpOp::SNE:
    case FpOp::SEQ: case FpOp::STR: case FpOp::SFL: case FpOp::LRP: case FpOp::NRM: case FpOp::DDX:
    case FpOp::DDY: case FpOp::KIL: case FpOp::TEX: case FpOp::TXP: case FpOp::TXL: case FpOp::TXB:
    case FpOp::FENCT: case FpOp::FENCB: return true;
    default: return false;
    }
}

// Precision clamps (FragmentProgramDecompiler.cpp ClampValue): 1 half, 2 fixed12 [-2, 2], 3 fixed9 [-1, 1],
// 4 saturate [0, 1].
std::string clamp_precision(const std::string& v, u32 prec)
{
    switch (prec) {
    case 1: return std::format("clamp({}, -65504.0, 65504.0)", v);
    case 2: return std::format("clamp({}, -2.0, 2.0)", v);
    case 3: return std::format("clamp({}, -1.0, 1.0)", v);
    case 4: return std::format("clamp({}, 0.0, 1.0)", v);
    default: return v;
    }
}

std::string fp_source(FpContext& c, const FpInstruction& in, u32 n)
{
    const FpSource& s = in.src[n];
    std::string r;
    u32 prec = s.prec_mod;
    switch (s.type) {
    case FpRegType::Temp:
        if (s.fp16) {
            c.h_regs.insert(s.index);
            r = std::format("h{}", s.index);
            if (prec == 1) prec = 0; // half precision on a half register is a no-op
        } else {
            c.r_regs.insert(s.index);
            r = std::format("r{}", s.index);
        }
        break;
    case FpRegType::Input: {
        // FragmentProgramDecompiler.cpp GetSRC: COL0/COL1 read clamped to [0, 1]; TEXn with the 2D texcoord control
        // bit read (x, y, 0, w).
        const u32 id = in.input;
        c.inputs |= 1u << id;
        if (id == 0) {
            r = "wpos";
        } else if (id == 1 || id == 2) {
            r = std::format("clamp(in.col{}, 0.0, 1.0)", id - 1);
            prec = 0;
        } else if (id == 3) {
            r = "in.fogc";
        } else if (id >= 4 && id <= 13) {
            const u32 t = id - 4;
            if (c.opt.texcoord_2d_mask & (1u << t)) {
                r = std::format("float4(in.tc{}.xy, 0.0, in_w)", t);
            } else {
                r = std::format("in.tc{}", t);
            }
            if (in.perspective_correct) r = std::format("({} * in.position.w)", r);
        } else if (id == 14) {
            r = "ssa";
            prec = 0;
        } else {
            r = "float4(0.0)";
        }
        break;
    }
    case FpRegType::Constant: r = std::format("fc[{}]", c.constant_index); break;
    default: r = "float4(0.0)"; break;
    }
    r += swizzle_suffix(s.swizzle);
    // Modifier order matters: saturate precision before abs, others after abs, negation last (Naruto UNS).
    if (prec == 4) r = clamp_precision(r, prec);
    if (s.abs) r = "abs(" + r + ")";
    if (prec && prec != 4) r = clamp_precision(r, prec);
    if (s.neg) r = "-(" + r + ")";
    return r;
}

std::string fp_compare(const char* op, const std::string& a, const std::string& b)
{
    return std::format("select(float4(0.0), float4(1.0), {} {} {})", a, op, b);
}

} // namespace

TranslateResult translate_fragment_program(const FragmentProgram& fp, const FpOptions& opt)
{
    TranslateResult result;
    FpContext c{fp, opt, {}, {}, {}, {}, 0, 0, 0};
    std::string body;
    u32 const_slot = 0;

    for (const FpInstruction& in : fp.instructions) {
        auto fail = [&](std::string what) {
            result.error = std::format("unsupported fragment program {} at offset {:#06x} ({})", what, in.offset, disassemble(in));
            return result;
        };
        c.constant_index = const_slot;
        if (in.has_constant) const_slot++;

        if (!fp_supported(in.op)) return fail(std::format("opcode {}", fp_op_name(in.op)));
        const FpOp op = static_cast<FpOp>(in.op);
        if (op == FpOp::NOP || op == FpOp::FENCT || op == FpOp::FENCB) continue;
        if (in.never()) continue; // conditional execution that never executes
        if (!in.always()) return fail("conditional execution");
        if (in.set_cond) return fail("condition code update");
        if (in.use_index_reg) return fail("indexed input (loop counter)");

        body += std::format("    {{ // {:04x}: {}\n", in.offset, disassemble(in));
        if (op == FpOp::KIL) {
            body += "        discard_fragment();\n    }\n";
            continue;
        }

        const std::string s0 = fp_source(c, in, 0);
        auto s1 = [&] { return fp_source(c, in, 1); };
        auto s2 = [&] { return fp_source(c, in, 2); };
        std::string e;
        bool exempt_prec = false; // ops RPCS3 SetDst does not precision-clamp
        switch (op) {
        case FpOp::MOV: e = s0; break;
        case FpOp::MUL: e = std::format("({}) * ({})", s0, s1()); break;
        case FpOp::ADD: e = std::format("({}) + ({})", s0, s1()); break;
        case FpOp::MAD: e = std::format("fma({}, {}, {})", s0, s1(), s2()); break;
        case FpOp::DP2: e = std::format("float4(dot(({}).xy, ({}).xy))", s0, s1()); break;
        case FpOp::DP3: e = std::format("float4(dot(({}).xyz, ({}).xyz))", s0, s1()); break;
        case FpOp::DP4: e = std::format("float4(dot({}, {}))", s0, s1()); break;
        case FpOp::MIN: e = std::format("min({}, {})", s0, s1()); exempt_prec = true; break;
        case FpOp::MAX: e = std::format("max({}, {})", s0, s1()); exempt_prec = true; break;
        case FpOp::DIV: e = std::format("({}) / ({}).x", s0, s1()); break;
        case FpOp::RCP: e = std::format("float4(1.0 / ({}).x)", s0); break;
        case FpOp::RSQ: e = std::format("float4(1.0 / sqrt(abs(({}).x)))", s0); break; // RSQ ignores the sign (GTA4)
        case FpOp::EX2: e = std::format("float4(exp2(({}).x))", s0); break;
        case FpOp::LG2: e = std::format("float4(log2(({}).x))", s0); exempt_prec = true; break;
        case FpOp::SIN: e = std::format("float4(sin(({}).x))", s0); exempt_prec = true; break;
        case FpOp::COS: e = std::format("float4(cos(({}).x))", s0); exempt_prec = true; break;
        case FpOp::FRC: e = std::format("fract({})", s0); exempt_prec = true; break;
        case FpOp::FLR: e = std::format("floor({})", s0); break;
        case FpOp::SLT: e = fp_compare("<", s0, s1()); break;
        case FpOp::SGE: e = fp_compare(">=", s0, s1()); break;
        case FpOp::SLE: e = fp_compare("<=", s0, s1()); break;
        case FpOp::SGT: e = fp_compare(">", s0, s1()); break;
        case FpOp::SNE: e = fp_compare("!=", s0, s1()); break;
        case FpOp::SEQ: e = fp_compare("==", s0, s1()); break;
        case FpOp::STR: e = "float4(1.0)"; break;
        case FpOp::SFL: e = "float4(0.0)"; break;
        case FpOp::LRP: e = std::format("mix({}, {}, {})", s2(), s1(), s0); break; // _builtin_lerp($2, $1, $0)
        case FpOp::NRM:
            e = std::format("rsx_nrm({})", s0);
            exempt_prec = true;
            break;
        case FpOp::DDX: e = std::format("dfdx({})", s0); break;
        case FpOp::DDY: e = std::format("dfdy({})", s0); break;
        case FpOp::TEX:
        case FpOp::TXP:
        case FpOp::TXL:
        case FpOp::TXB: {
            const u32 t = in.tex;
            c.textures |= 1u << t;
            const TextureKind kind = opt.texture_kind[t];
            if (kind == TextureKind::Cube) {
                if (op != FpOp::TEX) return fail(std::format("{} on a cube map", fp_op_name(in.op)));
                e = std::format("t{}.sample(s{}, ({}).xyz)", t, t, s0);
            } else {
                std::string coord = std::format("({}).xy", s0);
                if (op == FpOp::TXP) coord = std::format("(({}).xy / ({}).w)", s0, s0);
                if (kind == TextureKind::Tex1D) coord = std::format("float2(({}).x, 0.5)", coord);
                coord = std::format("({} * fp.tex_scale[{}].xy)", coord, t);
                switch (op) {
                case FpOp::TXL: e = std::format("t{}.sample(s{}, {}, level(({}).w))", t, t, coord, s0); break;
                case FpOp::TXB: e = std::format("t{}.sample(s{}, {}, bias(({}).w))", t, t, coord, s0); break;
                default: e = std::format("t{}.sample(s{}, {})", t, t, coord); break;
                }
            }
            if (in.tex_bx2) e = std::format("(({}) * 2.0 - 1.0)", e); // _bx2 texture expansion
            break;
        }
        default: return fail(std::format("opcode {}", fp_op_name(in.op)));
        }

        // Result modifiers (SetDst): scale, then saturate or the destination precision.
        static constexpr const char* kScale[] = {"", " * 2.0", " * 4.0", " * 8.0", "", " / 2.0", " / 4.0", " / 8.0"};
        if (in.scale & 7) e = std::format("(({}){})", e, kScale[in.scale & 7]);
        if (in.saturate) {
            e = clamp_precision(e, 4);
        } else if (in.prec && !exempt_prec && !(in.prec == 1 && !in.dst_fp16)) {
            e = clamp_precision(e, in.prec);
        }
        body += std::format("        const float4 res = {};\n", e);
        if (!in.no_dest && in.mask) {
            const std::string m = mask_suffix(in.mask);
            if (in.dst_fp16) {
                c.h_regs.insert(in.dst);
                c.h_written.insert(in.dst);
                body += std::format("        h{}{} = res{};\n", in.dst, m, m);
            } else {
                c.r_regs.insert(in.dst);
                c.r_written.insert(in.dst);
                body += std::format("        r{}{} = res{};\n", in.dst, m, m);
            }
        }
        body += "    }\n";
    }

    // Outputs: R0/R2/R3/R4 with 32-bit exports, H0/H4/H6/H8 otherwise; depth from R1.z
    // (FragmentProgramDecompiler.cpp get_fragment_program_output_set). H and R are kept as separate files; when the
    // program only wrote the other file's aliasing register, that one is exported.
    const bool fp32_out = (opt.shader_control & 0x40) != 0;
    const bool depth_export = (opt.shader_control & 0xe) != 0;
    static constexpr u32 kR[4] = {0, 2, 3, 4};
    static constexpr u32 kH[4] = {0, 4, 6, 8};
    std::array<std::string, 4> outputs;
    for (u32 i = 0; i < 4; i++) {
        const bool r_ok = c.r_written.count(kR[i]) != 0, h_ok = c.h_written.count(kH[i]) != 0;
        if (fp32_out) outputs[i] = (r_ok || !h_ok) ? std::format("r{}", kR[i]) : std::format("h{}", kH[i]);
        else outputs[i] = (h_ok || !r_ok) ? std::format("h{}", kH[i]) : std::format("r{}", kR[i]);
        if (outputs[i][0] == 'r') c.r_regs.insert(kR[i]);
        else c.h_regs.insert(kH[i]);
    }
    if (depth_export) c.r_regs.insert(1);

    std::string src = msl_prologue();
    src += "\nstruct rsx_fs_in {\n    float4 position [[position]];\n";
    const char* interp = opt.flat_colors ? " [[flat]]" : "";
    if (c.inputs & (1u << 1)) src += std::format("    float4 col0 [[user(col0)]]{};\n", interp);
    if (c.inputs & (1u << 2)) src += std::format("    float4 col1 [[user(col1)]]{};\n", interp);
    if (c.inputs & (1u << 3)) src += "    float4 fogc [[user(fogc)]];\n";
    for (u32 t = 0; t < 10; t++) {
        if (c.inputs & (1u << (4 + t))) src += std::format("    float4 tc{} [[user(tc{})]];\n", t, t);
    }
    src += "};\n\nstruct rsx_fs_out {\n";
    const u32 mrt = std::min<u32>(opt.mrt_count, 4);
    for (u32 i = 0; i < mrt; i++) src += std::format("    float4 c{} [[color({})]];\n", i, i);
    if (depth_export) src += "    float depth [[depth(any)]];\n";
    if (mrt == 0 && !depth_export) src += "    float4 c0 [[color(0)]];\n";
    src += "};\n\n";
    src += "static float4 rsx_nrm(float4 v) { const float l = length(v.xyz); return (l > 0.0 ? float4(v.xyz / l, 0.0) : v).xyzz; }\n\n";

    result.shader.entry = std::format("rsx_fp_{:016x}", fp.hash);
    std::string params = std::format("rsx_fs_in in [[stage_in]], constant float4* fc [[buffer({})]], "
                                     "constant rsx_fs_params& fp [[buffer({})]], bool front [[front_facing]]",
        slot::kFsConstants, slot::kFsParams);
    for (u32 t = 0; t < 16; t++) {
        if (!(c.textures & (1u << t))) continue;
        const char* type = opt.texture_kind[t] == TextureKind::Cube ? "texturecube<float>" : "texture2d<float>";
        params += std::format(",\n    {} t{} [[texture({})]], sampler s{} [[sampler({})]]", type, t, t, t, t);
    }
    src += std::format("fragment rsx_fs_out {}({})\n{{\n", result.shader.entry, params);
    // WPOS (x, y, z, 1/w): origin and pixel centre adjusted by fp.wpos (shader window registers).
    src += "    const float4 wpos = float4(in.position.x + fp.wpos.z, in.position.y * fp.wpos.x + fp.wpos.y + fp.wpos.z,"
           " in.position.z, in.position.w);\n";
    src += "    const float in_w = 1.0 / in.position.w;\n";
    src += "    const float4 ssa = float4(front ? 1.0 : -1.0);\n";
    src += "    (void)wpos; (void)in_w; (void)ssa; (void)fc;\n";
    for (u32 r : c.r_regs) src += std::format("    float4 r{} = float4(0.0);\n", r);
    for (u32 h : c.h_regs) src += std::format("    float4 h{} = float4(0.0);\n", h);
    src += body;
    src += "    rsx_fs_out out;\n";
    static constexpr const char* kChan[6] = {"x", "y", "z", "w", "0.0", "1.0"};
    for (u32 i = 0; i < mrt; i++) {
        const auto& sw = opt.output_swizzle[i];
        if (sw == std::array<u8, 4>{0, 1, 2, 3}) {
            src += std::format("    out.c{} = {};\n", i, outputs[i]);
        } else {
            auto ch = [&](u8 k) { return k < 4 ? std::format("{}.{}", outputs[i], kChan[k]) : std::string(kChan[std::min<u8>(k, 5)]); };
            src += std::format("    out.c{} = float4({}, {}, {}, {});\n", i, ch(sw[0]), ch(sw[1]), ch(sw[2]), ch(sw[3]));
        }
    }
    if (mrt == 0 && !depth_export) src += "    out.c0 = float4(0.0);\n";
    if (depth_export) src += "    out.depth = r1.z;\n";
    if (opt.alpha_test && mrt > 0) {
        src += "    if (!rsx_compare(fp.alpha_func, out.c0.w, fp.alpha_ref)) discard_fragment();\n";
    }
    src += "    return out;\n}\n";

    result.shader.source = std::move(src);
    result.shader.input_mask = c.inputs;
    result.shader.texture_mask = c.textures;
    result.shader.constant_count = static_cast<u32>(fp.constant_offsets.size());
    result.ok = true;
    return result;
}

void read_fragment_constants(const FragmentProgram& fp, const u8* program_data, std::vector<std::array<float, 4>>& out)
{
    // RPCS3 ProgramStateCache.cpp write_fragment_constants_to_buffer: the same half-word shuffle as instructions.
    out.resize(fp.constant_offsets.size());
    for (usize i = 0; i < fp.constant_offsets.size(); i++) {
        for (u32 k = 0; k < 4; k++) {
            const u32 w = fp_word(ps_ld32(program_data + fp.constant_offsets[i] + k * 4));
            __builtin_memcpy(&out[i][k], &w, 4);
        }
    }
}

} // namespace pspocket::rsx::shader
