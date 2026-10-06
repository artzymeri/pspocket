#pragma once

// A tiny RSX shader microcode assembler for synthetic test programs. Encodings mirror the decoders
// (runtime/rsx/src/shader/vp.cpp, fp.cpp), which follow RPCS3's RSXVertexProgram.h / RSXFragmentProgram.h.

#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

namespace pspocket::rsx::test {

inline std::array<u8, 4> swz(std::string_view s)
{
    std::array<u8, 4> r{0, 1, 2, 3};
    for (usize i = 0; i < 4; i++) {
        const char ch = s[i < s.size() ? i : s.size() - 1];
        r[i] = static_cast<u8>(ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : 3);
    }
    return r;
}

inline u8 mask(std::string_view s)
{
    u8 m = 0;
    for (char ch : s) m |= static_cast<u8>(1u << (ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : 3));
    return m;
}

// ---- vertex programs ----

struct VpSrc {
    u32 type = 1; // 1 temp, 2 input, 3 constant
    u32 temp = 0;
    std::array<u8, 4> sw{0, 1, 2, 3};
    bool neg = false;
    bool abs = false;
};

inline VpSrc vp_temp(u32 r, std::string_view s = "xyzw") { return {1, r, swz(s)}; }
inline VpSrc vp_in(std::string_view s = "xyzw") { return {2, 0, swz(s)}; }
inline VpSrc vp_const(std::string_view s = "xyzw") { return {3, 0, swz(s)}; }

struct VpInst {
    u32 vec_op = 0, sca_op = 0;
    VpSrc src[3] = {vp_in(), vp_in(), vp_in()};
    u32 input = 0;
    u32 constant = 0;
    bool vec_result = true; // the vector op writes o[out]
    u32 out = 0x1f;
    u32 vec_temp = 0x3f, sca_temp = 0x3f;
    u8 vec_mask = 0, sca_mask = 0;
    bool saturate = false;
    u32 cond = 7;
    bool cond_test = false;   // D0.cond_test_enable: a conditional write
    bool cond_update = false; // D0.cond_update_enable_0
    u32 cond_reg = 0;         // CC0 or CC1
    std::array<u8, 4> cond_sw{0, 1, 2, 3};
    bool end = false;
    bool constant_relative = false;  // D3.index_const: c[constant + A.addr_swizzle]
    u32 addr_reg = 0, addr_swizzle = 0; // D0.addr_reg_sel_1, D0.addr_swz
    int branch_target = -1;          // BRI/CAL target (shares its bits with src2)
    int branch_index = -1;           // BRB/CLB: the bit of NV4097_SET_TRANSFORM_BRANCH_BITS tested (D3 23-27)
    bool branch_if_true = true;      // BRB/CLB: taken when that bit is set (D3 28), else when it is clear
};

inline u32 vp_src_bits(const VpSrc& s)
{
    return s.type | (s.temp << 2) | (u32{s.sw[3]} << 8) | (u32{s.sw[2]} << 10) | (u32{s.sw[1]} << 12) |
        (u32{s.sw[0]} << 14) | (u32{s.neg} << 16);
}

inline std::array<u32, 4> encode_vp(const VpInst& in)
{
    const u32 s0 = vp_src_bits(in.src[0]), s1 = vp_src_bits(in.src[1]), s2 = vp_src_bits(in.src[2]);
    const u32 d0 = (u32{in.cond_sw[3]} << 2) | (u32{in.cond_sw[2]} << 4) | (u32{in.cond_sw[1]} << 6) |
        (u32{in.cond_sw[0]} << 8) | (in.cond << 10) | (u32{in.cond_test} << 13) | (u32{in.cond_update} << 14) |
        (in.vec_temp << 15) | (u32{in.src[0].abs} << 21) | (u32{in.src[1].abs} << 22) | (u32{in.src[2].abs} << 23) |
        (in.cond_reg << 25) | (u32{in.saturate} << 26) | (u32{in.vec_result} << 30);
    const u32 d1 = ((s0 >> 9) & 0xff) | (in.input << 8) | (in.constant << 12) | (in.vec_op << 22) | (in.sca_op << 27);
    const u32 d2 = ((s2 >> 11) & 0x3f) | (s1 << 6) | ((s0 & 0x1ff) << 23);
    auto m = [](u8 v, u32 base) {
        return (u32{(v >> 3) & 1u} << base) | (u32{(v >> 2) & 1u} << (base + 1)) | (u32{(v >> 1) & 1u} << (base + 2)) |
            (u32{v & 1u} << (base + 3));
    };
    u32 d3 = u32{in.end} | (u32{in.constant_relative} << 1) | (in.out << 2) | (in.sca_temp << 7) | m(in.vec_mask, 13) |
        m(in.sca_mask, 17) | ((s2 & 0x7ff) << 21);
    u32 d0x = d0 | (in.addr_swizzle & 3) | ((in.addr_reg & 1) << 24);
    u32 d2x = d2;
    if (in.branch_target >= 0) {
        const u32 t = static_cast<u32>(in.branch_target);
        d0x = (d0x & ~(1u << 23)) | (((t >> 9) & 1) << 23);
        d2x = (d2x & ~0x3fu) | ((t >> 3) & 0x3f);
        d3 = (d3 & ~(7u << 29)) | ((t & 7) << 29);
    }
    if (in.branch_index >= 0) {
        d3 = (d3 & ~(0x3fu << 23)) | ((static_cast<u32>(in.branch_index) & 31) << 23) | (u32{in.branch_if_true} << 28);
    }
    return {d0x, d1, d2x, d3};
}

inline std::vector<u32> assemble_vp(const std::vector<VpInst>& prog)
{
    std::vector<u32> words;
    for (const VpInst& in : prog) {
        const auto w = encode_vp(in);
        words.insert(words.end(), w.begin(), w.end());
    }
    return words;
}

// ---- fragment programs ----

struct FpSrc {
    u32 type = 0; // 0 temp, 1 input, 2 constant
    u32 index = 0;
    bool fp16 = false;
    std::array<u8, 4> sw{0, 1, 2, 3};
    bool neg = false;
    bool abs = false;
};

inline FpSrc fp_r(u32 r, std::string_view s = "xyzw") { return {0, r, false, swz(s)}; }
inline FpSrc fp_h(u32 r, std::string_view s = "xyzw") { return {0, r, true, swz(s)}; }
inline FpSrc fp_in(std::string_view s = "xyzw") { return {1, 0, false, swz(s)}; }
inline FpSrc fp_const(std::string_view s = "xyzw") { return {2, 0, false, swz(s)}; }

struct FpInst {
    u32 op = 0;
    u32 dst = 0;
    bool fp16 = false;
    u8 mask = 0xf;
    bool saturate = false;
    u32 input = 0;
    u32 tex = 0;
    FpSrc src[3] = {fp_r(0), fp_r(0), fp_r(0)};
    std::optional<std::array<float, 4>> constant;
    bool no_dest = false;
    u32 prec = 0;
    u32 scale = 0;
    bool exec_lt = true, exec_eq = true, exec_gt = true;
    std::array<u8, 4> cond_sw{0, 1, 2, 3}; // condition swizzle
    u32 cond_reg = 0;                       // RC tested by a conditional instruction
    u32 cond_mod_reg = 0;                   // RC written by set_cond
    bool set_cond = false;
    bool end = false;
    // Flow control (IFE, LOOP, REP): SRC1 bits 0-30 (IFE: else offset; LOOP/REP: counters) and SRC2 bits 0-30 (the
    // end offset), in place of sources 1 and 2.
    std::optional<u32> flow_src1, flow_src2;
    bool texture_expand = false; // OPDEST.exp_tex: _bx2
    bool indexed = false;        // SRC2.use_index_reg: inputs at TEX(A0.x + index_offset)
    u32 index_offset = 0;        // SRC2.addr_reg
    bool perspective = false;    // SRC2.perspective_corr
};

// IFE's operands: else and end at these byte offsets of the program.
inline void fp_if(FpInst& in, u32 else_offset, u32 end_offset)
{
    in.flow_src1 = else_offset >> 2;
    in.flow_src2 = end_offset >> 2;
}
// LOOP/REP's operands (SRC1, NV_fragment_program2 LOOP {count, init, increment}): the body runs `count` times up to the
// end offset; LOOP's index starts at `init` and goes up by `inc` (signed 8-bit) each time.
inline void fp_loop(FpInst& in, u32 count, u32 init, s32 inc, u32 end_offset)
{
    in.flow_src1 = (count << 2) | (init << 10) | ((static_cast<u32>(inc) & 0xff) << 19);
    in.flow_src2 = end_offset >> 2;
}

// An input read through the loop index: f[TEX(A0.x + offset)] (SRC2.addr_reg, use_index_reg).
inline void fp_indexed(FpInst& in, u32 offset)
{
    in.index_offset = offset;
    in.indexed = true;
}

inline u32 fp_src_bits(const FpSrc& s)
{
    return s.type | (s.index << 2) | (u32{s.fp16} << 8) | (u32{s.sw[0]} << 9) | (u32{s.sw[1]} << 11) |
        (u32{s.sw[2]} << 13) | (u32{s.sw[3]} << 15) | (u32{s.neg} << 17);
}

// Appends the instruction (and its constant) to `bytes` in guest memory order: each word big-endian with its
// 16-bit halves exchanged.
inline void encode_fp(const FpInst& in, std::vector<u8>& bytes)
{
    const u32 dst = u32{in.end} | (in.dst << 1) | (u32{in.fp16} << 7) | (u32{in.set_cond} << 8) | (u32{in.mask} << 9) |
        (in.input << 13) | (in.tex << 17) | (u32{in.texture_expand} << 21) | (in.prec << 22) | ((in.op & 0x3f) << 24) |
        (u32{in.no_dest} << 30) | (u32{in.saturate} << 31);
    const u32 s0 = fp_src_bits(in.src[0]) | (u32{in.exec_lt} << 18) | (u32{in.exec_eq} << 19) | (u32{in.exec_gt} << 20) |
        (u32{in.cond_sw[0]} << 21) | (u32{in.cond_sw[1]} << 23) | (u32{in.cond_sw[2]} << 25) | (u32{in.cond_sw[3]} << 27) |
        (u32{in.src[0].abs} << 29) | (in.cond_mod_reg << 30) | (in.cond_reg << 31);
    const u32 s1 = (in.flow_src1 ? (*in.flow_src1 & 0x7fffffffu)
                                 : (fp_src_bits(in.src[1]) | (u32{in.src[1].abs} << 18) | (in.scale << 28))) |
        (((in.op >> 6) & 1) << 31);
    const u32 s2 = in.flow_src2 ? (*in.flow_src2 & 0x7fffffffu)
                                : (fp_src_bits(in.src[2]) | (u32{in.src[2].abs} << 18) | ((in.index_offset & 0x7ff) << 19) |
                                      (u32{in.indexed} << 30) | (u32{in.perspective} << 31));
    auto put = [&](u32 w) {
        const u32 be = (w << 16) | (w >> 16);
        u8 b[4];
        ps_st32(b, be);
        bytes.insert(bytes.end(), b, b + 4);
    };
    put(dst);
    put(s0);
    put(s1);
    put(s2);
    if (in.constant) {
        for (float f : *in.constant) {
            u32 u;
            std::memcpy(&u, &f, 4);
            put(u);
        }
    }
}

inline std::vector<u8> assemble_fp(const std::vector<FpInst>& prog)
{
    std::vector<u8> bytes;
    for (const FpInst& in : prog) encode_fp(in, bytes);
    return bytes;
}

} // namespace pspocket::rsx::test
