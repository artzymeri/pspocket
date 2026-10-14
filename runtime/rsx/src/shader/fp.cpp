// Fragment program microcode: decoder and disassembler.
// Bit layouts: RPCS3 rpcs3/Emu/RSX/Program/RSXFragmentProgram.h (OPDEST, SRC0, SRC1, SRC2). Each 32-bit word is
// stored big-endian with its 16-bit halves exchanged (OPDEST::from_be32, Assembler/FPToCFG.cpp decode_instruction).
// An instruction whose source is a constant is followed by the constant itself (16 bytes, same shuffle); flow
// control instructions never carry one (FPToCFG.cpp deconstruct_fragment_program).

#include <format>

#include "pspocket/core/be.h"
#include "pspocket/rsx/shader/ir.hpp"

namespace pspocket::rsx::shader {
namespace {

constexpr u32 bits(u32 v, u32 lo, u32 n) { return (v >> lo) & ((1u << n) - 1); }

Swizzle swizzle_at(u32 v, u32 lo)
{
    return Swizzle{{static_cast<u8>(bits(v, lo, 2)), static_cast<u8>(bits(v, lo + 2, 2)), static_cast<u8>(bits(v, lo + 4, 2)),
        static_cast<u8>(bits(v, lo + 6, 2))}};
}

FpSource decode_source(u32 s, bool abs, u8 prec_mod)
{
    FpSource src;
    src.type = static_cast<FpRegType>(bits(s, 0, 2));
    src.index = static_cast<u8>(bits(s, 2, 6));
    src.fp16 = bits(s, 8, 1);
    src.swizzle = swizzle_at(s, 9);
    src.neg = bits(s, 17, 1);
    src.abs = abs;
    src.prec_mod = prec_mod;
    return src;
}

struct OpName {
    u8 op;
    const char* name;
};

constexpr OpName kOps[] = {{0x00, "NOP"}, {0x01, "MOV"}, {0x02, "MUL"}, {0x03, "ADD"}, {0x04, "MAD"}, {0x05, "DP3"},
    {0x06, "DP4"}, {0x07, "DST"}, {0x08, "MIN"}, {0x09, "MAX"}, {0x0A, "SLT"}, {0x0B, "SGE"}, {0x0C, "SLE"},
    {0x0D, "SGT"}, {0x0E, "SNE"}, {0x0F, "SEQ"}, {0x10, "FRC"}, {0x11, "FLR"}, {0x12, "KIL"}, {0x13, "PK4"},
    {0x14, "UP4"}, {0x15, "DDX"}, {0x16, "DDY"}, {0x17, "TEX"}, {0x18, "TXP"}, {0x19, "TXD"}, {0x1A, "RCP"},
    {0x1B, "RSQ"}, {0x1C, "EX2"}, {0x1D, "LG2"}, {0x1E, "LIT"}, {0x1F, "LRP"}, {0x20, "STR"}, {0x21, "SFL"},
    {0x22, "COS"}, {0x23, "SIN"}, {0x24, "PK2"}, {0x25, "UP2"}, {0x26, "POW"}, {0x27, "PKB"}, {0x28, "UPB"},
    {0x29, "PK16"}, {0x2A, "UP16"}, {0x2B, "BEM"}, {0x2C, "PKG"}, {0x2D, "UPG"}, {0x2E, "DP2A"}, {0x2F, "TXL"},
    {0x31, "TXB"}, {0x33, "TEXBEM"}, {0x34, "TXPBEM"}, {0x35, "BEMLUM"}, {0x36, "REFL"}, {0x37, "TIMESWTEX"},
    {0x38, "DP2"}, {0x39, "NRM"}, {0x3A, "DIV"}, {0x3B, "DIVSQ"}, {0x3C, "LIF"}, {0x3D, "FENCT"}, {0x3E, "FENCB"},
    {0x40, "BRK"}, {0x41, "CAL"}, {0x42, "IFE"}, {0x43, "LOOP"}, {0x44, "REP"}, {0x45, "RET"}};

constexpr const char* kInputs[] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3", "TEX4", "TEX5",
    "TEX6", "TEX7", "TEX8", "TEX9", "SSA"};

u32 load_fp_word(const u8* data, u32 offset) { return fp_word(ps_ld32(data + offset)); }

} // namespace

const char* fp_op_name(u32 op)
{
    for (const OpName& o : kOps) {
        if (o.op == op) return o.name;
    }
    return "OP?";
}

bool fp_op_is_flow_control(u32 op)
{
    switch (static_cast<FpOp>(op)) {
    case FpOp::BRK:
    case FpOp::CAL:
    case FpOp::IFE:
    case FpOp::LOOP:
    case FpOp::REP:
    case FpOp::RET:
    case FpOp::FENCT:
    case FpOp::FENCB: return true;
    default: return false;
    }
}

const char* fp_input_name(u32 index) { return index < std::size(kInputs) ? kInputs[index] : "UNK"; }

FpInstruction decode_fp_instruction(const u8* data, u32 offset)
{
    FpInstruction in;
    in.offset = offset;
    for (u32 i = 0; i < 4; i++) in.words[i] = load_fp_word(data, offset + i * 4);
    const u32 dst = in.words[0], s0 = in.words[1], s1 = in.words[2], s2 = in.words[3];

    in.end = bits(dst, 0, 1);
    in.dst = static_cast<u8>(bits(dst, 1, 6));
    in.dst_fp16 = bits(dst, 7, 1);
    in.set_cond = bits(dst, 8, 1);
    in.mask = static_cast<u8>(bits(dst, 9, 4));
    in.input = static_cast<u8>(bits(dst, 13, 4));
    in.tex = static_cast<u8>(bits(dst, 17, 4));
    in.tex_bx2 = bits(dst, 21, 1);
    in.prec = static_cast<u8>(bits(dst, 22, 2));
    in.op = static_cast<u8>(bits(dst, 24, 6) | (bits(s1, 31, 1) << 6));
    in.no_dest = bits(dst, 30, 1);
    in.saturate = bits(dst, 31, 1);

    // SRC0's abs bit is bit 29 (its bits 18-28 hold the condition); SRC1/SRC2 keep abs at bit 18.
    in.src[0] = decode_source(s0, bits(s0, 29, 1), static_cast<u8>(bits(s1, 19, 3)));
    in.src[1] = decode_source(s1, bits(s1, 18, 1), static_cast<u8>(bits(s1, 22, 3)));
    in.src[2] = decode_source(s2, bits(s2, 18, 1), static_cast<u8>(bits(s1, 25, 3)));
    in.scale = static_cast<u8>(bits(s1, 28, 3));

    in.exec_lt = bits(s0, 18, 1);
    in.exec_eq = bits(s0, 19, 1);
    in.exec_gt = bits(s0, 20, 1);
    in.cond_swizzle = swizzle_at(s0, 21);
    in.cond_mod_reg = static_cast<u8>(bits(s0, 30, 1));
    in.cond_reg = static_cast<u8>(bits(s0, 31, 1));

    in.addr_reg = static_cast<u16>(bits(s2, 19, 11));
    in.use_index_reg = bits(s2, 30, 1);
    in.perspective_correct = bits(s2, 31, 1);

    if (!fp_op_is_flow_control(in.op)) {
        for (const FpSource& s : in.src) {
            if (s.type == FpRegType::Constant) in.has_constant = true;
        }
    }
    if (in.has_constant) {
        in.constant_offset = offset + 16;
        for (u32 i = 0; i < 4; i++) in.constant[i] = load_fp_word(data, offset + 16 + i * 4);
    }
    return in;
}

std::optional<FragmentProgram> decode_fragment_program(const u8* data, u32 max_length, std::string* error)
{
    FragmentProgram fp;
    u64 h = 0x66726167ull; // "frag"
    u32 offset = 0;
    while (offset + 16 <= max_length) {
        FpInstruction in = decode_fp_instruction(data, offset);
        if (in.has_constant && offset + 32 > max_length) break;
        for (const FpSource& s : in.src) {
            if (s.type == FpRegType::Input && in.op != 0 && !fp_op_is_flow_control(in.op)) fp.input_mask |= 1u << in.input;
        }
        switch (static_cast<FpOp>(in.op)) {
        case FpOp::TEX:
        case FpOp::TXP:
        case FpOp::TXD:
        case FpOp::TXL:
        case FpOp::TXB:
        case FpOp::TEXBEM:
        case FpOp::TXPBEM: fp.texture_mask = static_cast<u16>(fp.texture_mask | (1u << in.tex)); break;
        case FpOp::BRK:
        case FpOp::CAL:
        case FpOp::IFE:
        case FpOp::LOOP:
        case FpOp::REP:
        case FpOp::RET: fp.has_branches = true; break;
        default: break;
        }
        // The hash covers the instruction words, never the constants that follow them.
        h = hash_words(in.words.data(), 4, h);
        offset += in.has_constant ? 32 : 16;
        if (in.has_constant) fp.constant_offsets.push_back(in.constant_offset);
        const bool end = in.end;
        fp.instructions.push_back(in);
        if (end) {
            fp.ucode_length = offset;
            fp.hash = h ^ (u64{fp.ucode_length} << 32);
            return fp;
        }
    }
    if (error) *error = std::format("fragment program has no end instruction within {} bytes", max_length);
    return std::nullopt;
}

bool scan_fragment_program(const u8* data, u32 max_length, FpScan& out)
{
    // Only what tells the program's end and its constants, as decode_fp_instruction finds them: the end bit, the
    // opcode (a flow control instruction has no constant) and the three sources' register types (D-953: the full
    // decode of every instruction was most of a draw's program lookup in GTA V, which writes programs over each other).
    out.constant_offsets.clear();
    u64 h = 0x66726167ull; // as decode_fragment_program
    u32 offset = 0;
    while (offset + 16 <= max_length) {
        u32 words[4];
        for (u32 i = 0; i < 4; i++) words[i] = load_fp_word(data, offset + i * 4);
        const u32 op = bits(words[0], 24, 6) | (bits(words[2], 31, 1) << 6);
        const auto constant = [](u32 s) { return static_cast<FpRegType>(bits(s, 0, 2)) == FpRegType::Constant; };
        const bool has_constant =
            !fp_op_is_flow_control(op) && (constant(words[1]) || constant(words[2]) || constant(words[3]));
        if (has_constant && offset + 32 > max_length) break;
        h = hash_words(words, 4, h);
        if (has_constant) out.constant_offsets.push_back(offset + 16);
        offset += has_constant ? 32 : 16;
        if (bits(words[0], 0, 1)) {
            out.ucode_length = offset;
            out.hash = h ^ (u64{offset} << 32);
            return true;
        }
    }
    return false;
}

namespace {

std::string fp_mask_str(u8 mask)
{
    if (mask == 0xf) return "";
    std::string s = ".";
    if (mask & 1) s += 'x';
    if (mask & 2) s += 'y';
    if (mask & 4) s += 'z';
    if (mask & 8) s += 'w';
    return s;
}

std::string fp_source_str(const FpInstruction& in, const FpSource& s)
{
    std::string r;
    switch (s.type) {
    case FpRegType::Temp: r = std::format("{}{}", s.fp16 ? 'H' : 'R', s.index); break;
    case FpRegType::Input:
        // SRC2.use_index_reg: the texture coordinate A0.x + addr_reg (NV_fragment_program2 fragment.texcoord[A0.x + n])
        r = in.use_index_reg ? std::format("f[TEX(A0.x + {})]", in.addr_reg) : std::format("f[{}]", fp_input_name(in.input));
        break;
    case FpRegType::Constant: {
        float f[4];
        for (u32 i = 0; i < 4; i++) __builtin_memcpy(&f[i], &in.constant[i], 4);
        r = std::format("{{{}, {}, {}, {}}}", f[0], f[1], f[2], f[3]);
        break;
    }
    default: r = "SRC3"; break;
    }
    r += s.swizzle.str();
    if (s.abs) r = "|" + r + "|";
    if (s.neg) r = "-" + r;
    return r;
}

u32 fp_operand_count(u32 op)
{
    switch (static_cast<FpOp>(op)) {
    case FpOp::NOP:
    case FpOp::KIL:
    case FpOp::FENCT:
    case FpOp::FENCB:
    case FpOp::BRK:
    case FpOp::RET:
    case FpOp::IFE:
    case FpOp::LOOP:
    case FpOp::REP:
    case FpOp::CAL:
    case FpOp::STR:
    case FpOp::SFL: return 0;
    case FpOp::MAD:
    case FpOp::LRP:
    case FpOp::TXD:
    case FpOp::BEM:
    case FpOp::TEXBEM:
    case FpOp::TXPBEM:
    case FpOp::DP2A: return 3;
    case FpOp::MUL:
    case FpOp::ADD:
    case FpOp::DP3:
    case FpOp::DP4:
    case FpOp::DP2:
    case FpOp::DST:
    case FpOp::MIN:
    case FpOp::MAX:
    case FpOp::SLT:
    case FpOp::SGE:
    case FpOp::SLE:
    case FpOp::SGT:
    case FpOp::SNE:
    case FpOp::SEQ:
    case FpOp::POW:
    case FpOp::DIV:
    case FpOp::DIVSQ:
    case FpOp::REFL: return 2;
    default: return 1;
    }
}

} // namespace

std::string disassemble(const FpInstruction& in)
{
    std::string s = fp_op_name(in.op);
    if (!fp_op_is_flow_control(in.op) && in.op != 0) {
        s += in.dst_fp16 ? 'H' : 'R';
        if (in.saturate) s += "_sat";
        static constexpr const char* kScale[] = {"", "_x2", "_x4", "_x8", "", "_d2", "_d4", "_d8"};
        s += kScale[in.scale & 7];
        if (in.no_dest) {
            s += in.set_cond ? std::format(" RC{}", in.cond_mod_reg) : " (no dest)";
        } else {
            s += std::format(" {}{}{}", in.dst_fp16 ? 'H' : 'R', in.dst, fp_mask_str(in.mask));
        }
        if (!in.always()) {
            std::string cc = in.never() ? "FL" : std::format("{}{}{}", in.exec_lt ? "LT" : "", in.exec_eq ? "EQ" : "", in.exec_gt ? "GT" : "");
            s += std::format(" ({}{}{})", cc, in.cond_reg, in.cond_swizzle.str());
        }
        const u32 n = fp_operand_count(in.op);
        for (u32 i = 0; i < n; i++) s += (i ? ", " : ", ") + fp_source_str(in, in.src[i]);
        switch (static_cast<FpOp>(in.op)) {
        case FpOp::TEX:
        case FpOp::TXP:
        case FpOp::TXD:
        case FpOp::TXL:
        case FpOp::TXB:
        case FpOp::TEXBEM:
        case FpOp::TXPBEM: s += std::format(", TEX{}", in.tex); break;
        default: break;
        }
    } else if (in.op == static_cast<u8>(FpOp::KIL) || in.op == static_cast<u8>(FpOp::RET) || in.op == static_cast<u8>(FpOp::BRK)) {
        if (!in.always()) s += std::format(" ({}{}{})", in.exec_lt ? "LT" : "", in.exec_eq ? "EQ" : "", in.exec_gt ? "GT" : "");
    }
    s += ';';
    if (in.end) s += " END";
    return s;
}

std::string disassemble(const FragmentProgram& fp)
{
    std::string s;
    for (const FpInstruction& in : fp.instructions) s += std::format("{:04x}: {}\n", in.offset, disassemble(in));
    return s;
}

} // namespace pspocket::rsx::shader
