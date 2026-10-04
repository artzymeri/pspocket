// Vertex program microcode: decoder and disassembler.
// Bit layouts: RPCS3 rpcs3/Emu/RSX/Program/RSXVertexProgram.h (D0, D1, D2, D3, SRC). Operand roles and destination
// rules: VertexProgramDecompiler.cpp (GetSRC, GetDST, SetDST, Decompile).

#include <algorithm>
#include <format>

#include "pspocket/rsx/shader/ir.hpp"

namespace pspocket::rsx::shader {
namespace {

constexpr u32 bits(u32 v, u32 lo, u32 n) { return (v >> lo) & ((1u << n) - 1); }

VpSource decode_source(u32 s, bool abs)
{
    VpSource src;
    src.type = static_cast<VpRegType>(bits(s, 0, 2));
    src.temp = static_cast<u8>(bits(s, 2, 6));
    // SRC: swz_w bits 8-9, swz_z 10-11, swz_y 12-13, swz_x 14-15, neg 16
    src.swizzle.c = {static_cast<u8>(bits(s, 14, 2)), static_cast<u8>(bits(s, 12, 2)), static_cast<u8>(bits(s, 10, 2)),
        static_cast<u8>(bits(s, 8, 2))};
    src.neg = bits(s, 16, 1);
    src.abs = abs;
    return src;
}

constexpr const char* kVecNames[] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX",
    "SLT", "SGE", "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", nullptr, nullptr, "TXL"};
constexpr const char* kScaNames[] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL",
    "CLI", "RET", "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP"};

std::string mask_str(u8 mask)
{
    if (mask == 0xf) return "";
    std::string s = ".";
    if (mask & 1) s += 'x';
    if (mask & 2) s += 'y';
    if (mask & 4) s += 'z';
    if (mask & 8) s += 'w';
    return s;
}

std::string source_str(const VpInstruction& in, const VpSource& s)
{
    std::string r;
    switch (s.type) {
    case VpRegType::Temp: r = std::format("R{}", s.temp); break;
    case VpRegType::Input: r = std::format("v[{}]", in.input); break;
    case VpRegType::Constant:
        r = in.constant_relative ? std::format("c[A{}.{} + {}]", in.addr_reg, "xyzw"[in.addr_swizzle], in.constant)
                                 : std::format("c[{}]", in.constant);
        break;
    default: r = "INVALID"; break;
    }
    r += s.swizzle.str();
    if (s.abs) r = "|" + r + "|";
    if (s.neg) r = "-" + r;
    return r;
}

} // namespace

std::string Swizzle::str() const
{
    if (identity()) return "";
    std::string s = ".";
    for (u8 i : c) s += "xyzw"[i & 3];
    return s;
}

const char* vp_vec_op_name(u32 op)
{
    if (op < std::size(kVecNames) && kVecNames[op]) return kVecNames[op];
    return "VEC?";
}

const char* vp_sca_op_name(u32 op)
{
    if (op < std::size(kScaNames)) return kScaNames[op];
    return "SCA?";
}

bool VpInstruction::writes_output() const
{
    if (out == 0x1f) return false;
    return vec_result ? vec_op != 0 : sca_op != 0;
}

VpInstruction decode_vp_instruction(const u32* w, u32 slot)
{
    VpInstruction in;
    in.slot = slot;
    in.words = {w[0], w[1], w[2], w[3]};
    const u32 d0 = w[0], d1 = w[1], d2 = w[2], d3 = w[3];

    in.vec_op = static_cast<u8>(bits(d1, 22, 5));
    in.sca_op = static_cast<u8>(bits(d1, 27, 5));
    in.input = static_cast<u8>(bits(d1, 8, 4));
    in.constant = static_cast<u16>(bits(d1, 12, 10));

    // SRC fields are split over the words: src0 = D2.src0l (9 bits) | D1.src0h (8 bits) << 9,
    // src1 = D2 bits 6-22, src2 = D3.src2l (11 bits) | D2.src2h (6 bits) << 11.
    const u32 s0 = bits(d2, 23, 9) | (bits(d1, 0, 8) << 9);
    const u32 s1 = bits(d2, 6, 17);
    const u32 s2 = bits(d3, 21, 11) | (bits(d2, 0, 6) << 11);
    in.src[0] = decode_source(s0, bits(d0, 21, 1));
    in.src[1] = decode_source(s1, bits(d0, 22, 1));
    in.src[2] = decode_source(s2, bits(d0, 23, 1));

    in.addr_swizzle = static_cast<u8>(bits(d0, 0, 2));
    in.cond_swizzle.c = {static_cast<u8>(bits(d0, 8, 2)), static_cast<u8>(bits(d0, 6, 2)), static_cast<u8>(bits(d0, 4, 2)),
        static_cast<u8>(bits(d0, 2, 2))};
    in.cond = static_cast<u8>(bits(d0, 10, 3));
    in.cond_test = bits(d0, 13, 1);
    in.cond_update = bits(d0, 14, 1) || bits(d0, 29, 1);
    in.vec_temp = static_cast<u8>(bits(d0, 15, 6));
    in.addr_reg = static_cast<u8>(bits(d0, 24, 1));
    in.cond_reg = static_cast<u8>(bits(d0, 25, 1));
    in.saturate = bits(d0, 26, 1);
    in.vec_result = bits(d0, 30, 1);

    in.end = bits(d3, 0, 1);
    in.constant_relative = bits(d3, 1, 1);
    in.out = static_cast<u8>(bits(d3, 2, 5));
    in.sca_temp = static_cast<u8>(bits(d3, 7, 6));
    in.vec_mask = static_cast<u8>(bits(d3, 16, 1) | (bits(d3, 15, 1) << 1) | (bits(d3, 14, 1) << 2) | (bits(d3, 13, 1) << 3));
    in.sca_mask = static_cast<u8>(bits(d3, 20, 1) | (bits(d3, 19, 1) << 1) | (bits(d3, 18, 1) << 2) | (bits(d3, 17, 1) << 3));

    in.branch_target = (bits(d0, 23, 1) << 9) | (bits(d2, 0, 6) << 3) | bits(d3, 29, 3);
    in.tex_num = static_cast<u8>(bits(d2, 8, 2));
    return in;
}

std::optional<VertexProgram> decode_vertex_program(const u32* memory, u32 instruction_count, u32 start,
    std::string* error)
{
    VertexProgram vp;
    vp.start = start;
    u64 h = 0x76657274ull; // "vert"
    u32 furthest = start; // the last instruction a branch or call reaches: subroutines can follow the main END
    for (u32 slot = start; slot < instruction_count; slot++) {
        VpInstruction in = decode_vp_instruction(memory + slot * 4, slot);
        for (const VpSource& s : in.src) {
            if (s.type == VpRegType::Input) vp.input_mask = static_cast<u16>(vp.input_mask | (1u << in.input));
        }
        if (in.writes_output()) vp.output_mask = static_cast<u16>(vp.output_mask | (1u << in.out));
        switch (static_cast<VpScaOp>(in.sca_op)) {
        case VpScaOp::BRI:
        case VpScaOp::CAL:
        case VpScaOp::CLI:
        case VpScaOp::BRB:
        case VpScaOp::CLB: furthest = std::max(furthest, in.branch_target); [[fallthrough]];
        case VpScaOp::BRA:
        case VpScaOp::RET: vp.has_branches = true; break;
        default: break;
        }
        h = hash_words(in.words.data(), 4, h);
        const bool end = in.end;
        vp.instructions.push_back(in);
        if (end && slot >= furthest) {
            vp.hash = h ^ vp.instructions.size();
            return vp;
        }
    }
    if (error) *error = std::format("vertex program at {} has no end instruction", start);
    return std::nullopt;
}

std::string disassemble(const VpInstruction& in)
{
    std::string out;
    auto dst_str = [&](bool is_sca) {
        std::string d;
        const u8 temp = is_sca ? in.sca_temp : in.vec_temp;
        const u8 mask = is_sca ? in.sca_mask : in.vec_mask;
        const bool result = is_sca ? !in.vec_result : in.vec_result;
        if (result && in.out != 0x1f) d = std::format("o[{}]{}", in.out, mask_str(mask));
        if (temp != 0x3f) {
            if (!d.empty()) d += ", ";
            d += std::format("R{}{}", temp, mask_str(mask));
        }
        if (d.empty()) d = std::format("CC{}{}", in.cond_reg, mask_str(mask));
        return d;
    };
    auto cond_str = [&]() -> std::string {
        if (!in.cond_test || in.cond == 7) return "";
        static constexpr const char* kCond[] = {"FL", "LT", "EQ", "LE", "GT", "NE", "GE", "TR"};
        return std::format(" ({}{}{})", kCond[in.cond], in.cond_reg ? "1" : "0", in.cond_swizzle.str());
    };
    const char* sat = in.saturate ? "_sat" : "";
    if (in.vec_op != 0) {
        std::string ops;
        switch (static_cast<VpVecOp>(in.vec_op)) {
        case VpVecOp::MOV:
        case VpVecOp::FRC:
        case VpVecOp::FLR:
        case VpVecOp::SSG:
        case VpVecOp::ARL: ops = source_str(in, in.src[0]); break;
        case VpVecOp::ADD: ops = source_str(in, in.src[0]) + ", " + source_str(in, in.src[2]); break;
        case VpVecOp::MAD:
            ops = source_str(in, in.src[0]) + ", " + source_str(in, in.src[1]) + ", " + source_str(in, in.src[2]);
            break;
        case VpVecOp::TXL: ops = std::format("{}, TEX{}", source_str(in, in.src[0]), in.tex_num); break;
        default: ops = source_str(in, in.src[0]) + ", " + source_str(in, in.src[1]); break;
        }
        out += std::format("{}{} {}{}, {};", vp_vec_op_name(in.vec_op), sat, dst_str(false), cond_str(), ops);
    }
    if (in.sca_op != 0) {
        if (!out.empty()) out += " + ";
        switch (static_cast<VpScaOp>(in.sca_op)) {
        case VpScaOp::BRA:
        case VpScaOp::BRI:
        case VpScaOp::CAL:
        case VpScaOp::CLI:
        case VpScaOp::BRB:
        case VpScaOp::CLB: out += std::format("{} L{}{};", vp_sca_op_name(in.sca_op), in.branch_target, cond_str()); break;
        case VpScaOp::RET: out += std::format("RET{};", cond_str()); break;
        default:
            out += std::format("{}{} {}{}, {};", vp_sca_op_name(in.sca_op), sat, dst_str(true), cond_str(),
                source_str(in, in.src[2]));
            break;
        }
    }
    if (out.empty()) out = "NOP;";
    if (in.end) out += " END";
    return out;
}

std::string disassemble(const VertexProgram& vp)
{
    std::string s;
    for (const VpInstruction& in : vp.instructions) s += std::format("{:3}: {}\n", in.slot, disassemble(in));
    return s;
}

u64 hash_words(const u32* words, usize count, u64 seed)
{
    // 64-bit multiply-rotate mix per word with a splitmix finaliser. No dependency; good enough for cache keys
    // (the caches compare the full ucode on a hit).
    u64 h = seed ^ (0x9E3779B97F4A7C15ull * (count + 1));
    for (usize i = 0; i < count; i++) {
        h ^= words[i];
        h *= 0xff51afd7ed558ccdull;
        h = (h << 31) | (h >> 33);
    }
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

} // namespace pspocket::rsx::shader
