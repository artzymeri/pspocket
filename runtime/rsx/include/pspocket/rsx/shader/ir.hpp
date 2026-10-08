#pragma once

// RSX shader microcode IR: decoded vertex program (VP) and fragment program (FP) instructions.
//
// Encodings follow RPCS3 rpcs3/Emu/RSX/Program/RSXVertexProgram.h (D0-D3, SRC) and RSXFragmentProgram.h (OPDEST,
// SRC0-SRC2), operand semantics follow VertexProgramDecompiler.cpp and FragmentProgramDecompiler.cpp. The IR keeps
// every field the hardware encodes (raw words included) so later translation waves (flow control, condition codes,
// address registers, pack/unpack) slot in without re-decoding.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::rsx::shader {

struct Swizzle {
    std::array<u8, 4> c{0, 1, 2, 3}; // component selected for x, y, z, w (0 = x .. 3 = w)
    bool identity() const { return c == std::array<u8, 4>{0, 1, 2, 3}; }
    std::string str() const; // ".xyzw" style; empty for identity
};

// ---- vertex programs --------------------------------------------------------------------------------------------

enum class VpRegType : u8 { Invalid = 0, Temp = 1, Input = 2, Constant = 3 };

enum class VpVecOp : u8 {
    NOP = 0x00, MOV = 0x01, MUL = 0x02, ADD = 0x03, MAD = 0x04, DP3 = 0x05, DPH = 0x06, DP4 = 0x07,
    DST = 0x08, MIN = 0x09, MAX = 0x0a, SLT = 0x0b, SGE = 0x0c, ARL = 0x0d, FRC = 0x0e, FLR = 0x0f,
    SEQ = 0x10, SFL = 0x11, SGT = 0x12, SLE = 0x13, SNE = 0x14, STR = 0x15, SSG = 0x16, TXL = 0x19,
};

enum class VpScaOp : u8 {
    NOP = 0x00, MOV = 0x01, RCP = 0x02, RCC = 0x03, RSQ = 0x04, EXP = 0x05, LOG = 0x06, LIT = 0x07,
    BRA = 0x08, BRI = 0x09, CAL = 0x0a, CLI = 0x0b, RET = 0x0c, LG2 = 0x0d, EX2 = 0x0e, SIN = 0x0f,
    COS = 0x10, BRB = 0x11, CLB = 0x12, PSH = 0x13, POP = 0x14,
};

const char* vp_vec_op_name(u32 op); // "MAD", or "VEC?<n>"
const char* vp_sca_op_name(u32 op);

struct VpSource {
    VpRegType type = VpRegType::Invalid;
    u8 temp = 0; // temp register for Temp
    Swizzle swizzle;
    bool neg = false;
    bool abs = false;
};

struct VpInstruction {
    std::array<u32, 4> words{}; // D0-D3, host order
    u32 slot = 0;               // instruction index in vertex program memory
    u8 vec_op = 0;              // VpVecOp
    u8 sca_op = 0;              // VpScaOp
    std::array<VpSource, 3> src{};
    u8 input = 0;               // D1.input_src: v[] register for Input sources
    u16 constant = 0;           // D1.const_src: c[] register for Constant sources
    bool constant_relative = false; // D3.index_const: c[constant + A0.x]
    u8 addr_reg = 0, addr_swizzle = 0;
    // Destinations. The output register o[out] is written by the vector op when vec_result is set, otherwise by
    // the scalar op; out == 0x1f means none. Temps 0x3f mean none.
    bool vec_result = false;
    u8 out = 0x1f;
    u8 vec_temp = 0x3f;
    u8 sca_temp = 0x3f;
    u8 vec_mask = 0; // bit 0 = x .. bit 3 = w
    u8 sca_mask = 0;
    bool saturate = false;
    // Condition codes
    u8 cond = 7;             // 0 = never, 7 = always, else a lt/eq/gt mask tested against the CC register
    bool cond_test = false;  // D0.cond_test_enable
    bool cond_update = false;
    u8 cond_reg = 0;
    Swizzle cond_swizzle;
    // Flow control and textures
    u32 branch_target = 0;
    u8 tex_num = 0;
    bool end = false;

    bool writes_output() const;
};

struct VertexProgram {
    std::vector<VpInstruction> instructions; // in program order, from `start` to the instruction with the end bit
    u32 start = 0;
    u16 input_mask = 0;   // v[] registers read
    u16 output_mask = 0;  // o[] registers written
    bool has_branches = false;
    u64 hash = 0;         // of the ucode words (VP constants are registers, nothing to mask)
};

// Decodes the vertex program starting at `start` in vertex program memory (kMaxVertexProgramInstructions x 4
// words). Walks linearly to the first instruction with the end bit. nullopt + error if there is none.
std::optional<VertexProgram> decode_vertex_program(const u32* memory, u32 instruction_count, u32 start,
    std::string* error = nullptr);
VpInstruction decode_vp_instruction(const u32* words, u32 slot);

std::string disassemble(const VpInstruction& inst);
std::string disassemble(const VertexProgram& vp);

// ---- fragment programs ------------------------------------------------------------------------------------------

enum class FpRegType : u8 { Temp = 0, Input = 1, Constant = 2, Unknown = 3 };

enum class FpOp : u8 {
    NOP = 0x00, MOV = 0x01, MUL = 0x02, ADD = 0x03, MAD = 0x04, DP3 = 0x05, DP4 = 0x06, DST = 0x07,
    MIN = 0x08, MAX = 0x09, SLT = 0x0A, SGE = 0x0B, SLE = 0x0C, SGT = 0x0D, SNE = 0x0E, SEQ = 0x0F,
    FRC = 0x10, FLR = 0x11, KIL = 0x12, PK4 = 0x13, UP4 = 0x14, DDX = 0x15, DDY = 0x16, TEX = 0x17,
    TXP = 0x18, TXD = 0x19, RCP = 0x1A, RSQ = 0x1B, EX2 = 0x1C, LG2 = 0x1D, LIT = 0x1E, LRP = 0x1F,
    STR = 0x20, SFL = 0x21, COS = 0x22, SIN = 0x23, PK2 = 0x24, UP2 = 0x25, POW = 0x26, PKB = 0x27,
    UPB = 0x28, PK16 = 0x29, UP16 = 0x2A, BEM = 0x2B, PKG = 0x2C, UPG = 0x2D, DP2A = 0x2E, TXL = 0x2F,
    TXB = 0x31, TEXBEM = 0x33, TXPBEM = 0x34, BEMLUM = 0x35, REFL = 0x36, TIMESWTEX = 0x37, DP2 = 0x38,
    NRM = 0x39, DIV = 0x3A, DIVSQ = 0x3B, LIF = 0x3C, FENCT = 0x3D, FENCB = 0x3E, BRK = 0x40, CAL = 0x41,
    IFE = 0x42, LOOP = 0x43, REP = 0x44, RET = 0x45,
};

const char* fp_op_name(u32 op); // "MAD", or "OP?<n>"
bool fp_op_is_flow_control(u32 op);

// Fragment input attributes (OPDEST.src_attr_reg_num)
enum class FpInput : u8 { WPOS = 0, COL0 = 1, COL1 = 2, FOGC = 3, TEX0 = 4, TEX9 = 13, SSA = 14 };
const char* fp_input_name(u32 index);

struct FpSource {
    FpRegType type = FpRegType::Temp;
    u8 index = 0; // temp register (R or H)
    bool fp16 = false;
    Swizzle swizzle;
    bool neg = false;
    bool abs = false;
    u8 prec_mod = 0; // SRC1.srcN_prec_mod
};

struct FpInstruction {
    std::array<u32, 4> words{}; // OPDEST, SRC0, SRC1, SRC2 after the half-word shuffle
    u32 offset = 0;             // byte offset in the program
    u8 op = 0;                  // FpOp (7 bits: OPDEST.opcode | SRC1.opcode_hi << 6)
    // Destination
    u8 dst = 0;
    bool dst_fp16 = false;
    u8 mask = 0xf; // bit 0 = x .. bit 3 = w
    bool no_dest = false;
    bool saturate = false;
    u8 prec = 0;   // 0 full, 1 half, 2 fixed12, 3 fixed9
    bool set_cond = false;
    u8 scale = 0;  // SRC1.scale: 1/2/3 = x2/x4/x8, 5/6/7 = /2,/4,/8
    std::array<FpSource, 3> src{};
    u8 input = 0;  // input attribute for Input sources
    u8 tex = 0;
    bool tex_bx2 = false;
    bool perspective_correct = false;
    bool use_index_reg = false;
    u16 addr_reg = 0;
    // Condition (SRC0)
    bool exec_lt = true, exec_eq = true, exec_gt = true;
    u8 cond_reg = 0;
    u8 cond_mod_reg = 0;
    Swizzle cond_swizzle;
    // Embedded constant (the next 16 bytes when any source is a constant)
    bool has_constant = false;
    std::array<u32, 4> constant{}; // float bits, decoded
    u32 constant_offset = 0;       // byte offset of the constant in the program
    bool end = false;

    bool always() const { return exec_lt && exec_eq && exec_gt; }
    bool never() const { return !exec_lt && !exec_eq && !exec_gt; }
};

struct FragmentProgram {
    std::vector<FpInstruction> instructions;
    u32 ucode_length = 0;               // bytes from the program start to past the last instruction or constant
    std::vector<u32> constant_offsets;  // byte offsets of embedded constants, in order (fc[i] in translated code)
    u32 input_mask = 0;                 // bit per FpInput read
    u16 texture_mask = 0;               // texture units sampled
    bool has_branches = false;
    u64 hash = 0;                       // of the ucode with embedded constants masked out
};

// Decodes a fragment program from guest memory (data = host pointer to the program's first byte, big-endian
// words). Stops after the instruction with the end bit; fails if max_length bytes contain none.
std::optional<FragmentProgram> decode_fragment_program(const u8* data, u32 max_length, std::string* error = nullptr);
// What tells a fragment program from another, without decoding it (D-240): its length, its hash (as
// FragmentProgram::hash) and where its embedded constants are. False when no end instruction comes within max_length.
// `constant_offsets` is reused (no allocation once it has grown).
struct FpScan {
    u32 ucode_length = 0;
    u64 hash = 0;
    std::vector<u32> constant_offsets;
};
bool scan_fragment_program(const u8* data, u32 max_length, FpScan& out);
FpInstruction decode_fp_instruction(const u8* data, u32 offset);
// The half-word shuffle of FP words: big-endian word -> instruction word (RPCS3 OPDEST::from_be32).
constexpr u32 fp_word(u32 be_word) { return (be_word << 16) | (be_word >> 16); }

std::string disassemble(const FpInstruction& inst);
std::string disassemble(const FragmentProgram& fp);

// Program hashing. VP: the instruction words from start to end. FP: every instruction word, skipping embedded
// constants (games patch those per draw; they reach the shader through a buffer), plus the length.
u64 hash_words(const u32* words, usize count, u64 seed);

} // namespace pspocket::rsx::shader
