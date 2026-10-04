#pragma once

// PPC assembler helper: encoders for every PPU instruction (generic encode() driven by the op table in
// runtime/ppu/include/pspocket/ppu/ops.inc, plus named helpers and extended mnemonics), and a small label-based
// Assembler for synthetic programs and fuzz cases. Header-only.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/decoder.hpp"

namespace pspocket::recomp::ppu::as {

using ::pspocket::ppu::Fmt;
using ::pspocket::ppu::Op;
using ::pspocket::ppu::OpInfo;

// Operands by position. Which ones an op uses depends on its format (decoder.hpp).
struct Fields {
    u32 d = 0;       // bits 6-10: rT rS frT frS vD vS BO TO crbD
    u32 a = 0;       // bits 11-15: rA frA vA BI crbA
    u32 b = 0;       // bits 16-20: rB frB vB crbB SH NB
    u32 c = 0;       // bits 21-25: frC vC MB (32-bit rotates)
    u32 e = 0;       // bits 26-30: ME (32-bit rotates)
    s32 imm = 0;     // SIMM/UIMM/D/DS, or a branch displacement in bytes
    u32 sh = 0;      // 6-bit SH (MD/XS forms)
    u32 mb = 0;      // 6-bit MB/ME (MD/MDS forms)
    u32 crfd = 0;    // compare / mcrf / mtfsfi target field
    u32 crfs = 0;
    u32 l = 0;       // compare L, sync L
    u32 spr = 0;     // SPR / TBR number (un-swapped)
    u32 fxm = 0;     // mtcrf / mfocrf field mask
    u32 flm = 0;     // mtfsf field mask
    u32 u4 = 0;      // mtfsfi immediate
    u32 bh = 0;      // bclr/bcctr hint
    u32 strm = 0;    // dst/dss stream
    u32 t = 0;       // dst transient / dss all
    bool oe = false;
    bool rc = false;
    bool lk = false;
    bool aa = false;
};

namespace detail {

constexpr u32 f6(u32 v) { return (v & 31) << 21; }
constexpr u32 f11(u32 v) { return (v & 31) << 16; }
constexpr u32 f16(u32 v) { return (v & 31) << 11; }
constexpr u32 f21(u32 v) { return (v & 31) << 6; }
constexpr u32 f26(u32 v) { return (v & 31) << 1; }
constexpr u32 spr_field(u32 spr) { return ((spr & 31) << 16) | (((spr >> 5) & 31) << 11); }

} // namespace detail

// Encodes op with the given operands. The Rc/OE/LK/AA bits are only applied when the op has them.
inline u32 encode(Op op, const Fields& f)
{
    using namespace detail;
    const OpInfo& info = ::pspocket::ppu::op_info(op);
    const u32 p = u32{info.primary} << 26;
    const u32 xo = info.xo;
    const u32 rc = ((info.flags & ::pspocket::ppu::OPF_RC) && f.rc) ? 1u : 0u;
    const u32 oe = ((info.flags & ::pspocket::ppu::OPF_OE) && f.oe) ? (1u << 10) : 0u;
    const u32 lk = f.lk ? 1u : 0u;
    const u32 aa = f.aa ? 2u : 0u;
    const u32 uimm = static_cast<u32>(f.imm) & 0xFFFF;

    switch (info.fmt) {
    case Fmt::D_RT_RA_SI:
    case Fmt::D_RA_RS_UI:
    case Fmt::D_TO_RA_SI:
    case Fmt::D_LOAD:
    case Fmt::D_STORE:
    case Fmt::D_FLOAD:
    case Fmt::D_FSTORE: return p | f6(f.d) | f11(f.a) | uimm;
    case Fmt::D_CMPI:
    case Fmt::D_CMPLI: return p | ((f.crfd & 7) << 23) | ((f.l & 1) << 21) | f11(f.a) | uimm;
    case Fmt::DS_LOAD:
    case Fmt::DS_STORE: return p | f6(f.d) | f11(f.a) | (uimm & 0xFFFC) | (xo & 3);
    case Fmt::I_BRANCH: return p | (static_cast<u32>(f.imm) & 0x03FFFFFC) | aa | lk;
    case Fmt::B_BRANCH: return p | f6(f.d) | f11(f.a) | (uimm & 0xFFFC) | aa | lk;
    case Fmt::XL_BRANCH: return p | f6(f.d) | f11(f.a) | ((f.bh & 3) << 11) | (xo << 1) | lk;
    case Fmt::SC: return 0x44000002u;
    case Fmt::XL_CRLOG: return p | f6(f.d) | f11(f.a) | f16(f.b) | (xo << 1);
    case Fmt::XL_MCRF: return p | ((f.crfd & 7) << 23) | ((f.crfs & 7) << 18) | (xo << 1);
    case Fmt::XL_NONE:
    case Fmt::X_NONE: return p | (xo << 1);
    case Fmt::XO_RT_RA_RB:
    case Fmt::XO_RT_RA: return p | f6(f.d) | f11(f.a) | f16(f.b) | oe | (xo << 1) | rc;
    case Fmt::X_RA_RS_RB:
    case Fmt::X_RA_RS:
    case Fmt::X_RA_RS_SH:
    case Fmt::X_LOAD:
    case Fmt::X_STORE:
    case Fmt::X_FLOAD:
    case Fmt::X_FSTORE:
    case Fmt::X_LSWI:
    case Fmt::X_RA_RB:
    case Fmt::X_TO_RA_RB:
    case Fmt::X_VLOAD:
    case Fmt::X_VSTORE: {
        // stwcx./stdcx. have their Rc bit fixed to 1.
        const u32 fixed_rc = (op == Op::STWCX_ || op == Op::STDCX_) ? 1u : rc;
        return p | f6(f.d) | f11(f.a) | f16(f.b) | (xo << 1) | fixed_rc;
    }
    case Fmt::XS_RA_RS_SH:
        return p | f6(f.d) | f11(f.a) | f16(f.sh) | ((xo | ((f.sh >> 5) & 1)) << 1) | rc;
    case Fmt::M_RA_RS_SH_MB_ME:
    case Fmt::M_RA_RS_RB_MB_ME: return p | f6(f.d) | f11(f.a) | f16(f.b) | f21(f.c) | f26(f.e) | rc;
    case Fmt::MD_RA_RS_SH_MB:
        return p | f6(f.d) | f11(f.a) | f16(f.sh) | f21(f.mb) | (((f.mb >> 5) & 1) << 5) | ((xo & 7) << 2) |
            (((f.sh >> 5) & 1) << 1) | rc;
    case Fmt::MDS_RA_RS_RB_MB:
        return p | f6(f.d) | f11(f.a) | f16(f.b) | f21(f.mb) | (((f.mb >> 5) & 1) << 5) | ((xo & 15) << 1) | rc;
    case Fmt::X_CMP: return p | ((f.crfd & 7) << 23) | ((f.l & 1) << 21) | f11(f.a) | f16(f.b) | (xo << 1);
    case Fmt::X_DST: return p | ((f.t & 1) << 25) | ((f.strm & 3) << 21) | f11(f.a) | f16(f.b) | (xo << 1);
    case Fmt::X_DSS: return p | ((f.t & 1) << 25) | ((f.strm & 3) << 21) | (xo << 1);
    case Fmt::X_SYNC: return p | ((f.l & 3) << 21) | (xo << 1);
    case Fmt::XFX_MFSPR:
    case Fmt::XFX_MFTB:
    case Fmt::XFX_MTSPR: return p | f6(f.d) | spr_field(f.spr) | (xo << 1);
    case Fmt::X_MFCR: return p | f6(f.d) | (xo << 1);
    case Fmt::X_MFOCRF: return p | f6(f.d) | (1u << 20) | ((f.fxm & 0xFF) << 12) | (xo << 1);
    case Fmt::XFX_MTCRF:
        return p | f6(f.d) | (op == Op::MTOCRF ? (1u << 20) : 0u) | ((f.fxm & 0xFF) << 12) | (xo << 1);
    case Fmt::A_FRT_FRA_FRB:
    case Fmt::A_FRT_FRA_FRC:
    case Fmt::A_FRT_FRA_FRC_FRB:
    case Fmt::A_FRT_FRB: return p | f6(f.d) | f11(f.a) | f16(f.b) | f21(f.c) | ((xo & 31) << 1) | rc;
    case Fmt::X_FRT_FRB:
    case Fmt::X_MFFS: return p | f6(f.d) | f16(f.b) | (xo << 1) | rc;
    case Fmt::X_FCMP: return p | ((f.crfd & 7) << 23) | f11(f.a) | f16(f.b) | (xo << 1);
    case Fmt::XFL_MTFSF: return p | ((f.flm & 0xFF) << 17) | f16(f.b) | (xo << 1) | rc;
    case Fmt::X_MTFSFI: return p | ((f.crfd & 7) << 23) | ((f.u4 & 15) << 12) | (xo << 1) | rc;
    case Fmt::X_MTFSB: return p | f6(f.d) | (xo << 1) | rc;
    case Fmt::X_MCRFS: return p | ((f.crfd & 7) << 23) | ((f.crfs & 7) << 18) | (xo << 1);
    case Fmt::VX_VD_VA_VB:
    case Fmt::VX_VD_VB:
    case Fmt::VX_VD_VB_UIMM:
    case Fmt::VX_VD_SIMM:
    case Fmt::VX_VD:
    case Fmt::VX_VB: return p | f6(f.d) | f11(f.a) | f16(f.b) | xo;
    case Fmt::VXR_VD_VA_VB: return p | f6(f.d) | f11(f.a) | f16(f.b) | (rc << 10) | xo;
    case Fmt::VA_VD_VA_VB_VC: return p | f6(f.d) | f11(f.a) | f16(f.b) | f21(f.c) | xo;
    case Fmt::VA_VSLDOI: return p | f6(f.d) | f11(f.a) | f16(f.b) | ((f.sh & 15) << 6) | xo;
    case Fmt::NONE: break;
    }
    throw std::logic_error("as::encode: op has no encoding");
}

// ------------------------------------------------------------------------------------------- named encoders
// Operand order follows the assembler syntax.

inline u32 d_form(Op op, u32 d, u32 a, s32 imm) { return encode(op, Fields{.d = d, .a = a, .imm = imm}); }
inline u32 xo_form(Op op, u32 rt, u32 ra, u32 rb, bool oe, bool rc)
{
    return encode(op, Fields{.d = rt, .a = ra, .b = rb, .oe = oe, .rc = rc});
}
inline u32 x_logic(Op op, u32 ra, u32 rs, u32 rb, bool rc) { return encode(op, Fields{.d = rs, .a = ra, .b = rb, .rc = rc}); }

// integer immediates
inline u32 addi(u32 rt, u32 ra, s32 si) { return d_form(Op::ADDI, rt, ra, si); }
inline u32 addis(u32 rt, u32 ra, s32 si) { return d_form(Op::ADDIS, rt, ra, si); }
inline u32 addic(u32 rt, u32 ra, s32 si) { return d_form(Op::ADDIC, rt, ra, si); }
inline u32 addic_(u32 rt, u32 ra, s32 si) { return d_form(Op::ADDIC_, rt, ra, si); }
inline u32 subfic(u32 rt, u32 ra, s32 si) { return d_form(Op::SUBFIC, rt, ra, si); }
inline u32 mulli(u32 rt, u32 ra, s32 si) { return d_form(Op::MULLI, rt, ra, si); }
inline u32 li(u32 rt, s32 si) { return addi(rt, 0, si); }
inline u32 lis(u32 rt, s32 si) { return addis(rt, 0, si); }
inline u32 ori(u32 ra, u32 rs, u32 ui) { return d_form(Op::ORI, rs, ra, static_cast<s32>(ui)); }
inline u32 oris(u32 ra, u32 rs, u32 ui) { return d_form(Op::ORIS, rs, ra, static_cast<s32>(ui)); }
inline u32 xori(u32 ra, u32 rs, u32 ui) { return d_form(Op::XORI, rs, ra, static_cast<s32>(ui)); }
inline u32 xoris(u32 ra, u32 rs, u32 ui) { return d_form(Op::XORIS, rs, ra, static_cast<s32>(ui)); }
inline u32 andi_(u32 ra, u32 rs, u32 ui) { return d_form(Op::ANDI_, rs, ra, static_cast<s32>(ui)); }
inline u32 andis_(u32 ra, u32 rs, u32 ui) { return d_form(Op::ANDIS_, rs, ra, static_cast<s32>(ui)); }
inline u32 nop() { return ori(0, 0, 0); }
inline u32 twi(u32 to, u32 ra, s32 si) { return d_form(Op::TWI, to, ra, si); }
inline u32 tdi(u32 to, u32 ra, s32 si) { return d_form(Op::TDI, to, ra, si); }

// compares
inline u32 cmpi(u32 crf, u32 l, u32 ra, s32 si) { return encode(Op::CMPI, Fields{.a = ra, .imm = si, .crfd = crf, .l = l}); }
inline u32 cmpli(u32 crf, u32 l, u32 ra, u32 ui)
{
    return encode(Op::CMPLI, Fields{.a = ra, .imm = static_cast<s32>(ui), .crfd = crf, .l = l});
}
inline u32 cmp(u32 crf, u32 l, u32 ra, u32 rb) { return encode(Op::CMP, Fields{.a = ra, .b = rb, .crfd = crf, .l = l}); }
inline u32 cmpl(u32 crf, u32 l, u32 ra, u32 rb) { return encode(Op::CMPL, Fields{.a = ra, .b = rb, .crfd = crf, .l = l}); }
inline u32 cmpwi(u32 crf, u32 ra, s32 si) { return cmpi(crf, 0, ra, si); }
inline u32 cmpdi(u32 crf, u32 ra, s32 si) { return cmpi(crf, 1, ra, si); }
inline u32 cmplwi(u32 crf, u32 ra, u32 ui) { return cmpli(crf, 0, ra, ui); }
inline u32 cmpldi(u32 crf, u32 ra, u32 ui) { return cmpli(crf, 1, ra, ui); }
inline u32 cmpw(u32 crf, u32 ra, u32 rb) { return cmp(crf, 0, ra, rb); }
inline u32 cmpd(u32 crf, u32 ra, u32 rb) { return cmp(crf, 1, ra, rb); }
inline u32 cmplw(u32 crf, u32 ra, u32 rb) { return cmpl(crf, 0, ra, rb); }
inline u32 cmpld(u32 crf, u32 ra, u32 rb) { return cmpl(crf, 1, ra, rb); }

// XO arithmetic
inline u32 add(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::ADD, rt, ra, rb, oe, rc); }
inline u32 addc(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::ADDC, rt, ra, rb, oe, rc); }
inline u32 adde(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::ADDE, rt, ra, rb, oe, rc); }
inline u32 addze(u32 rt, u32 ra, bool oe = false, bool rc = false) { return xo_form(Op::ADDZE, rt, ra, 0, oe, rc); }
inline u32 addme(u32 rt, u32 ra, bool oe = false, bool rc = false) { return xo_form(Op::ADDME, rt, ra, 0, oe, rc); }
inline u32 subf(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::SUBF, rt, ra, rb, oe, rc); }
inline u32 subfc(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::SUBFC, rt, ra, rb, oe, rc); }
inline u32 subfe(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::SUBFE, rt, ra, rb, oe, rc); }
inline u32 subfze(u32 rt, u32 ra, bool oe = false, bool rc = false) { return xo_form(Op::SUBFZE, rt, ra, 0, oe, rc); }
inline u32 subfme(u32 rt, u32 ra, bool oe = false, bool rc = false) { return xo_form(Op::SUBFME, rt, ra, 0, oe, rc); }
inline u32 neg(u32 rt, u32 ra, bool oe = false, bool rc = false) { return xo_form(Op::NEG, rt, ra, 0, oe, rc); }
inline u32 mullw(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::MULLW, rt, ra, rb, oe, rc); }
inline u32 mulld(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::MULLD, rt, ra, rb, oe, rc); }
inline u32 mulhw(u32 rt, u32 ra, u32 rb, bool rc = false) { return xo_form(Op::MULHW, rt, ra, rb, false, rc); }
inline u32 mulhwu(u32 rt, u32 ra, u32 rb, bool rc = false) { return xo_form(Op::MULHWU, rt, ra, rb, false, rc); }
inline u32 mulhd(u32 rt, u32 ra, u32 rb, bool rc = false) { return xo_form(Op::MULHD, rt, ra, rb, false, rc); }
inline u32 mulhdu(u32 rt, u32 ra, u32 rb, bool rc = false) { return xo_form(Op::MULHDU, rt, ra, rb, false, rc); }
inline u32 divw(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::DIVW, rt, ra, rb, oe, rc); }
inline u32 divwu(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::DIVWU, rt, ra, rb, oe, rc); }
inline u32 divd(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::DIVD, rt, ra, rb, oe, rc); }
inline u32 divdu(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return xo_form(Op::DIVDU, rt, ra, rb, oe, rc); }

// logical, shifts, extends
inline u32 and_(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::AND, ra, rs, rb, rc); }
inline u32 andc(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::ANDC, ra, rs, rb, rc); }
inline u32 or_(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::OR, ra, rs, rb, rc); }
inline u32 orc(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::ORC, ra, rs, rb, rc); }
inline u32 xor_(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::XOR, ra, rs, rb, rc); }
inline u32 nor(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::NOR, ra, rs, rb, rc); }
inline u32 nand(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::NAND, ra, rs, rb, rc); }
inline u32 eqv(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::EQV, ra, rs, rb, rc); }
inline u32 mr(u32 ra, u32 rs) { return or_(ra, rs, rs); }
inline u32 not_(u32 ra, u32 rs) { return nor(ra, rs, rs); }
inline u32 slw(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::SLW, ra, rs, rb, rc); }
inline u32 srw(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::SRW, ra, rs, rb, rc); }
inline u32 sraw(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::SRAW, ra, rs, rb, rc); }
inline u32 sld(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::SLD, ra, rs, rb, rc); }
inline u32 srd(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::SRD, ra, rs, rb, rc); }
inline u32 srad(u32 ra, u32 rs, u32 rb, bool rc = false) { return x_logic(Op::SRAD, ra, rs, rb, rc); }
inline u32 srawi(u32 ra, u32 rs, u32 sh, bool rc = false) { return encode(Op::SRAWI, Fields{.d = rs, .a = ra, .b = sh, .rc = rc}); }
inline u32 sradi(u32 ra, u32 rs, u32 sh, bool rc = false) { return encode(Op::SRADI, Fields{.d = rs, .a = ra, .sh = sh, .rc = rc}); }
inline u32 cntlzw(u32 ra, u32 rs, bool rc = false) { return x_logic(Op::CNTLZW, ra, rs, 0, rc); }
inline u32 cntlzd(u32 ra, u32 rs, bool rc = false) { return x_logic(Op::CNTLZD, ra, rs, 0, rc); }
inline u32 extsb(u32 ra, u32 rs, bool rc = false) { return x_logic(Op::EXTSB, ra, rs, 0, rc); }
inline u32 extsh(u32 ra, u32 rs, bool rc = false) { return x_logic(Op::EXTSH, ra, rs, 0, rc); }
inline u32 extsw(u32 ra, u32 rs, bool rc = false) { return x_logic(Op::EXTSW, ra, rs, 0, rc); }

// rotates
inline u32 rlwinm(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, bool rc = false)
{
    return encode(Op::RLWINM, Fields{.d = rs, .a = ra, .b = sh, .c = mb, .e = me, .rc = rc});
}
inline u32 rlwimi(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, bool rc = false)
{
    return encode(Op::RLWIMI, Fields{.d = rs, .a = ra, .b = sh, .c = mb, .e = me, .rc = rc});
}
inline u32 rlwnm(u32 ra, u32 rs, u32 rb, u32 mb, u32 me, bool rc = false)
{
    return encode(Op::RLWNM, Fields{.d = rs, .a = ra, .b = rb, .c = mb, .e = me, .rc = rc});
}
inline u32 rldicl(u32 ra, u32 rs, u32 sh, u32 mb, bool rc = false)
{
    return encode(Op::RLDICL, Fields{.d = rs, .a = ra, .sh = sh, .mb = mb, .rc = rc});
}
inline u32 rldicr(u32 ra, u32 rs, u32 sh, u32 me, bool rc = false)
{
    return encode(Op::RLDICR, Fields{.d = rs, .a = ra, .sh = sh, .mb = me, .rc = rc});
}
inline u32 rldic(u32 ra, u32 rs, u32 sh, u32 mb, bool rc = false)
{
    return encode(Op::RLDIC, Fields{.d = rs, .a = ra, .sh = sh, .mb = mb, .rc = rc});
}
inline u32 rldimi(u32 ra, u32 rs, u32 sh, u32 mb, bool rc = false)
{
    return encode(Op::RLDIMI, Fields{.d = rs, .a = ra, .sh = sh, .mb = mb, .rc = rc});
}
inline u32 rldcl(u32 ra, u32 rs, u32 rb, u32 mb, bool rc = false)
{
    return encode(Op::RLDCL, Fields{.d = rs, .a = ra, .b = rb, .mb = mb, .rc = rc});
}
inline u32 rldcr(u32 ra, u32 rs, u32 rb, u32 me, bool rc = false)
{
    return encode(Op::RLDCR, Fields{.d = rs, .a = ra, .b = rb, .mb = me, .rc = rc});
}
inline u32 slwi(u32 ra, u32 rs, u32 n) { return rlwinm(ra, rs, n, 0, 31 - n); }
inline u32 srwi(u32 ra, u32 rs, u32 n) { return rlwinm(ra, rs, (32 - n) & 31, n, 31); }
inline u32 rotlwi(u32 ra, u32 rs, u32 n) { return rlwinm(ra, rs, n, 0, 31); }
inline u32 clrlwi(u32 ra, u32 rs, u32 n) { return rlwinm(ra, rs, 0, n, 31); }
inline u32 sldi(u32 ra, u32 rs, u32 n) { return rldicr(ra, rs, n, 63 - n); }
inline u32 srdi(u32 ra, u32 rs, u32 n) { return rldicl(ra, rs, (64 - n) & 63, n); }
inline u32 clrldi(u32 ra, u32 rs, u32 n) { return rldicl(ra, rs, 0, n); }
inline u32 rotldi(u32 ra, u32 rs, u32 n) { return rldicl(ra, rs, n, 0); }

// loads and stores
inline u32 lbz(u32 rt, s32 d, u32 ra) { return d_form(Op::LBZ, rt, ra, d); }
inline u32 lbzu(u32 rt, s32 d, u32 ra) { return d_form(Op::LBZU, rt, ra, d); }
inline u32 lhz(u32 rt, s32 d, u32 ra) { return d_form(Op::LHZ, rt, ra, d); }
inline u32 lhzu(u32 rt, s32 d, u32 ra) { return d_form(Op::LHZU, rt, ra, d); }
inline u32 lha(u32 rt, s32 d, u32 ra) { return d_form(Op::LHA, rt, ra, d); }
inline u32 lhau(u32 rt, s32 d, u32 ra) { return d_form(Op::LHAU, rt, ra, d); }
inline u32 lwz(u32 rt, s32 d, u32 ra) { return d_form(Op::LWZ, rt, ra, d); }
inline u32 lwzu(u32 rt, s32 d, u32 ra) { return d_form(Op::LWZU, rt, ra, d); }
inline u32 stb(u32 rs, s32 d, u32 ra) { return d_form(Op::STB, rs, ra, d); }
inline u32 stbu(u32 rs, s32 d, u32 ra) { return d_form(Op::STBU, rs, ra, d); }
inline u32 sth(u32 rs, s32 d, u32 ra) { return d_form(Op::STH, rs, ra, d); }
inline u32 sthu(u32 rs, s32 d, u32 ra) { return d_form(Op::STHU, rs, ra, d); }
inline u32 stw(u32 rs, s32 d, u32 ra) { return d_form(Op::STW, rs, ra, d); }
inline u32 stwu(u32 rs, s32 d, u32 ra) { return d_form(Op::STWU, rs, ra, d); }
inline u32 lmw(u32 rt, s32 d, u32 ra) { return d_form(Op::LMW, rt, ra, d); }
inline u32 stmw(u32 rs, s32 d, u32 ra) { return d_form(Op::STMW, rs, ra, d); }
inline u32 ld(u32 rt, s32 ds, u32 ra) { return d_form(Op::LD, rt, ra, ds); }
inline u32 ldu(u32 rt, s32 ds, u32 ra) { return d_form(Op::LDU, rt, ra, ds); }
inline u32 lwa(u32 rt, s32 ds, u32 ra) { return d_form(Op::LWA, rt, ra, ds); }
inline u32 std_(u32 rs, s32 ds, u32 ra) { return d_form(Op::STD, rs, ra, ds); }
inline u32 stdu(u32 rs, s32 ds, u32 ra) { return d_form(Op::STDU, rs, ra, ds); }
inline u32 x_mem(Op op, u32 rt, u32 ra, u32 rb) { return encode(op, Fields{.d = rt, .a = ra, .b = rb}); }
inline u32 lbzx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LBZX, rt, ra, rb); }
inline u32 lhzx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LHZX, rt, ra, rb); }
inline u32 lhax(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LHAX, rt, ra, rb); }
inline u32 lwzx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LWZX, rt, ra, rb); }
inline u32 lwax(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LWAX, rt, ra, rb); }
inline u32 ldx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LDX, rt, ra, rb); }
inline u32 stbx(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STBX, rs, ra, rb); }
inline u32 sthx(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STHX, rs, ra, rb); }
inline u32 stwx(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STWX, rs, ra, rb); }
inline u32 stdx(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STDX, rs, ra, rb); }
inline u32 lwbrx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LWBRX, rt, ra, rb); }
inline u32 stwbrx(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STWBRX, rs, ra, rb); }
inline u32 lwarx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LWARX, rt, ra, rb); }
inline u32 ldarx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LDARX, rt, ra, rb); }
inline u32 stwcx_(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STWCX_, rs, ra, rb); }
inline u32 stdcx_(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STDCX_, rs, ra, rb); }
inline u32 lswi(u32 rt, u32 ra, u32 nb) { return x_mem(Op::LSWI, rt, ra, nb); }
inline u32 stswi(u32 rs, u32 ra, u32 nb) { return x_mem(Op::STSWI, rs, ra, nb); }
inline u32 lswx(u32 rt, u32 ra, u32 rb) { return x_mem(Op::LSWX, rt, ra, rb); }
inline u32 stswx(u32 rs, u32 ra, u32 rb) { return x_mem(Op::STSWX, rs, ra, rb); }
inline u32 dcbz(u32 ra, u32 rb) { return x_mem(Op::DCBZ, 0, ra, rb); }
inline u32 dcbt(u32 ra, u32 rb) { return x_mem(Op::DCBT, 0, ra, rb); }

// FP loads/stores and arithmetic
inline u32 lfs(u32 frt, s32 d, u32 ra) { return d_form(Op::LFS, frt, ra, d); }
inline u32 lfd(u32 frt, s32 d, u32 ra) { return d_form(Op::LFD, frt, ra, d); }
inline u32 lfdu(u32 frt, s32 d, u32 ra) { return d_form(Op::LFDU, frt, ra, d); }
inline u32 stfs(u32 frs, s32 d, u32 ra) { return d_form(Op::STFS, frs, ra, d); }
inline u32 stfd(u32 frs, s32 d, u32 ra) { return d_form(Op::STFD, frs, ra, d); }
inline u32 stfdu(u32 frs, s32 d, u32 ra) { return d_form(Op::STFDU, frs, ra, d); }
inline u32 lfdx(u32 frt, u32 ra, u32 rb) { return x_mem(Op::LFDX, frt, ra, rb); }
inline u32 lfsx(u32 frt, u32 ra, u32 rb) { return x_mem(Op::LFSX, frt, ra, rb); }
inline u32 stfdx(u32 frs, u32 ra, u32 rb) { return x_mem(Op::STFDX, frs, ra, rb); }
inline u32 stfiwx(u32 frs, u32 ra, u32 rb) { return x_mem(Op::STFIWX, frs, ra, rb); }
inline u32 fa3(Op op, u32 frt, u32 fra, u32 frb, bool rc) { return encode(op, Fields{.d = frt, .a = fra, .b = frb, .rc = rc}); }
inline u32 fadd(u32 frt, u32 fra, u32 frb, bool rc = false) { return fa3(Op::FADD, frt, fra, frb, rc); }
inline u32 fadds(u32 frt, u32 fra, u32 frb, bool rc = false) { return fa3(Op::FADDS, frt, fra, frb, rc); }
inline u32 fsub(u32 frt, u32 fra, u32 frb, bool rc = false) { return fa3(Op::FSUB, frt, fra, frb, rc); }
inline u32 fdiv(u32 frt, u32 fra, u32 frb, bool rc = false) { return fa3(Op::FDIV, frt, fra, frb, rc); }
inline u32 fdivs(u32 frt, u32 fra, u32 frb, bool rc = false) { return fa3(Op::FDIVS, frt, fra, frb, rc); }
inline u32 fmul(u32 frt, u32 fra, u32 frc, bool rc = false) { return encode(Op::FMUL, Fields{.d = frt, .a = fra, .c = frc, .rc = rc}); }
inline u32 fmuls(u32 frt, u32 fra, u32 frc, bool rc = false) { return encode(Op::FMULS, Fields{.d = frt, .a = fra, .c = frc, .rc = rc}); }
inline u32 fa4(Op op, u32 frt, u32 fra, u32 frc, u32 frb, bool rc)
{
    return encode(op, Fields{.d = frt, .a = fra, .b = frb, .c = frc, .rc = rc});
}
inline u32 fmadd(u32 frt, u32 fra, u32 frc, u32 frb, bool rc = false) { return fa4(Op::FMADD, frt, fra, frc, frb, rc); }
inline u32 fmadds(u32 frt, u32 fra, u32 frc, u32 frb, bool rc = false) { return fa4(Op::FMADDS, frt, fra, frc, frb, rc); }
inline u32 fmsub(u32 frt, u32 fra, u32 frc, u32 frb, bool rc = false) { return fa4(Op::FMSUB, frt, fra, frc, frb, rc); }
inline u32 fnmadd(u32 frt, u32 fra, u32 frc, u32 frb, bool rc = false) { return fa4(Op::FNMADD, frt, fra, frc, frb, rc); }
inline u32 fsel(u32 frt, u32 fra, u32 frc, u32 frb, bool rc = false) { return fa4(Op::FSEL, frt, fra, frc, frb, rc); }
inline u32 fx2(Op op, u32 frt, u32 frb, bool rc) { return encode(op, Fields{.d = frt, .b = frb, .rc = rc}); }
inline u32 fsqrt(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FSQRT, frt, frb, rc); }
inline u32 fsqrts(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FSQRTS, frt, frb, rc); }
inline u32 fres(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FRES, frt, frb, rc); }
inline u32 frsqrte(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FRSQRTE, frt, frb, rc); }
inline u32 fmr(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FMR, frt, frb, rc); }
inline u32 fneg(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FNEG, frt, frb, rc); }
inline u32 fabs_(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FABS, frt, frb, rc); }
inline u32 frsp(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FRSP, frt, frb, rc); }
inline u32 fctiw(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FCTIW, frt, frb, rc); }
inline u32 fctiwz(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FCTIWZ, frt, frb, rc); }
inline u32 fctid(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FCTID, frt, frb, rc); }
inline u32 fctidz(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FCTIDZ, frt, frb, rc); }
inline u32 fcfid(u32 frt, u32 frb, bool rc = false) { return fx2(Op::FCFID, frt, frb, rc); }
inline u32 fcmpu(u32 crf, u32 fra, u32 frb) { return encode(Op::FCMPU, Fields{.a = fra, .b = frb, .crfd = crf}); }
inline u32 fcmpo(u32 crf, u32 fra, u32 frb) { return encode(Op::FCMPO, Fields{.a = fra, .b = frb, .crfd = crf}); }
inline u32 mffs(u32 frt, bool rc = false) { return encode(Op::MFFS, Fields{.d = frt, .rc = rc}); }
inline u32 mtfsf(u32 flm, u32 frb, bool rc = false) { return encode(Op::MTFSF, Fields{.b = frb, .flm = flm, .rc = rc}); }
inline u32 mtfsfi(u32 crf, u32 imm, bool rc = false) { return encode(Op::MTFSFI, Fields{.crfd = crf, .u4 = imm, .rc = rc}); }
inline u32 mtfsb0(u32 bt, bool rc = false) { return encode(Op::MTFSB0, Fields{.d = bt, .rc = rc}); }
inline u32 mtfsb1(u32 bt, bool rc = false) { return encode(Op::MTFSB1, Fields{.d = bt, .rc = rc}); }

// SPRs, CR
inline u32 mfspr(u32 rt, u32 spr) { return encode(Op::MFSPR, Fields{.d = rt, .spr = spr}); }
inline u32 mtspr(u32 spr, u32 rs) { return encode(Op::MTSPR, Fields{.d = rs, .spr = spr}); }
inline u32 mflr(u32 rt) { return mfspr(rt, ::pspocket::ppu::kSprLr); }
inline u32 mtlr(u32 rs) { return mtspr(::pspocket::ppu::kSprLr, rs); }
inline u32 mfctr(u32 rt) { return mfspr(rt, ::pspocket::ppu::kSprCtr); }
inline u32 mtctr(u32 rs) { return mtspr(::pspocket::ppu::kSprCtr, rs); }
inline u32 mfxer(u32 rt) { return mfspr(rt, ::pspocket::ppu::kSprXer); }
inline u32 mtxer(u32 rs) { return mtspr(::pspocket::ppu::kSprXer, rs); }
inline u32 mftb(u32 rt, u32 tbr = ::pspocket::ppu::kSprTbl) { return encode(Op::MFTB, Fields{.d = rt, .spr = tbr}); }
inline u32 mfcr(u32 rt) { return encode(Op::MFCR, Fields{.d = rt}); }
inline u32 mfocrf(u32 rt, u32 fxm) { return encode(Op::MFOCRF, Fields{.d = rt, .fxm = fxm}); }
inline u32 mtcrf(u32 fxm, u32 rs) { return encode(Op::MTCRF, Fields{.d = rs, .fxm = fxm}); }
inline u32 mtocrf(u32 fxm, u32 rs) { return encode(Op::MTOCRF, Fields{.d = rs, .fxm = fxm}); }
inline u32 mcrf(u32 crfd, u32 crfs) { return encode(Op::MCRF, Fields{.crfd = crfd, .crfs = crfs}); }
inline u32 crlog(Op op, u32 bt, u32 ba, u32 bb) { return encode(op, Fields{.d = bt, .a = ba, .b = bb}); }
inline u32 sc() { return 0x44000002u; }
inline u32 sync(u32 l = 0) { return encode(Op::SYNC, Fields{.l = l}); }
inline u32 lwsync() { return sync(1); }
inline u32 isync() { return encode(Op::ISYNC, Fields{}); }
inline u32 eieio() { return encode(Op::EIEIO, Fields{}); }
inline u32 tw(u32 to, u32 ra, u32 rb) { return encode(Op::TW, Fields{.d = to, .a = ra, .b = rb}); }
inline u32 td(u32 to, u32 ra, u32 rb) { return encode(Op::TD, Fields{.d = to, .a = ra, .b = rb}); }
inline u32 trap() { return tw(31, 0, 0); }

// branches (displacements in bytes)
inline u32 b_rel(s32 disp, bool lk = false) { return encode(Op::B, Fields{.imm = disp, .lk = lk}); }
inline u32 b_abs(u32 target, bool lk = false) { return encode(Op::B, Fields{.imm = static_cast<s32>(target), .lk = lk, .aa = true}); }
inline u32 bc_rel(u32 bo, u32 bi, s32 disp, bool lk = false) { return encode(Op::BC, Fields{.d = bo, .a = bi, .imm = disp, .lk = lk}); }
inline u32 bclr(u32 bo, u32 bi, bool lk = false) { return encode(Op::BCLR, Fields{.d = bo, .a = bi, .lk = lk}); }
inline u32 bcctr(u32 bo, u32 bi, bool lk = false) { return encode(Op::BCCTR, Fields{.d = bo, .a = bi, .lk = lk}); }
inline u32 blr() { return bclr(20, 0); }
inline u32 blrl() { return bclr(20, 0, true); }
inline u32 bctr() { return bcctr(20, 0); }
inline u32 bctrl() { return bcctr(20, 0, true); }

// BO values and CR bit helpers for conditional branches
inline constexpr u32 BO_TRUE = 12, BO_FALSE = 4, BO_ALWAYS = 20, BO_DNZ = 16, BO_DZ = 18;
inline constexpr u32 CR_LT = 0, CR_GT = 1, CR_EQ = 2, CR_SO = 3;
constexpr u32 crbit(u32 crf, u32 bit) { return crf * 4 + bit; }

// ------------------------------------------------------------------------------------------- Assembler

// Emits instructions at increasing guest addresses; labels can be used before they are bound.
class Assembler {
public:
    struct Label {
        u32 id = ~0u;
    };

    explicit Assembler(u32 base) : base_(base) {}

    u32 base() const { return base_; }
    u32 pc() const { return base_ + static_cast<u32>(words_.size() * 4); }
    usize size_bytes() const { return words_.size() * 4; }

    void emit(u32 word) { words_.push_back(word); }
    void word(u32 value) { words_.push_back(value); } // data
    void align(u32 bytes)
    {
        while (pc() % bytes) words_.push_back(0);
    }

    Label new_label()
    {
        bound_.push_back(~0u);
        return Label{static_cast<u32>(bound_.size() - 1)};
    }
    Label here()
    {
        Label l = new_label();
        bind(l);
        return l;
    }
    void bind(Label l)
    {
        if (bound_.at(l.id) != ~0u) throw std::logic_error("Assembler: label bound twice");
        bound_[l.id] = pc();
    }
    bool is_bound(Label l) const { return bound_.at(l.id) != ~0u; }
    u32 address(Label l) const
    {
        const u32 a = bound_.at(l.id);
        if (a == ~0u) throw std::logic_error("Assembler: label not bound");
        return a;
    }

    // Branches to labels (resolved by finish()).
    void b(Label l) { fixup(Fix::Branch24, l, as::b_rel(0)); }
    void bl(Label l) { fixup(Fix::Branch24, l, as::b_rel(0, true)); }
    void bc(u32 bo, u32 bi, Label l, bool lk = false) { fixup(Fix::Branch14, l, as::bc_rel(bo, bi, 0, lk)); }
    void beq(u32 crf, Label l) { bc(BO_TRUE, crbit(crf, CR_EQ), l); }
    void bne(u32 crf, Label l) { bc(BO_FALSE, crbit(crf, CR_EQ), l); }
    void blt(u32 crf, Label l) { bc(BO_TRUE, crbit(crf, CR_LT), l); }
    void bge(u32 crf, Label l) { bc(BO_FALSE, crbit(crf, CR_LT), l); }
    void bgt(u32 crf, Label l) { bc(BO_TRUE, crbit(crf, CR_GT), l); }
    void ble(u32 crf, Label l) { bc(BO_FALSE, crbit(crf, CR_GT), l); }
    void bdnz(Label l) { bc(BO_DNZ, 0, l); }
    void bdz(Label l) { bc(BO_DZ, 0, l); }

    // Data words referring to labels: absolute address, or offset from another label (jump tables).
    void label_word(Label l) { fixup(Fix::Abs32, l, 0); }
    void label_offset_word(Label target, Label from)
    {
        fixups_.push_back({Fix::Rel32, target, from, static_cast<u32>(words_.size())});
        words_.push_back(0);
    }
    // lis/ori pair loading a label's absolute address into rt (resolved by finish()).
    void load_address(u32 rt, Label l)
    {
        fixups_.push_back({Fix::Hi16, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(as::lis(rt, 0));
        fixups_.push_back({Fix::Lo16, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(as::ori(rt, rt, 0));
    }
    // lis rt, label@ha ; addi rt, rt, label@l (the GCC form; addi sign-extends, @ha compensates).
    void load_address_ha(u32 rt, Label l)
    {
        fixups_.push_back({Fix::Ha16, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(as::lis(rt, 0));
        fixups_.push_back({Fix::Lo16, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(as::addi(rt, rt, 0));
    }
    // lis rt, label@ha ; addic rt, rt, label@l (SNC's form).
    void load_address_hac(u32 rt, Label l)
    {
        fixups_.push_back({Fix::Ha16, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(as::lis(rt, 0));
        fixups_.push_back({Fix::Lo16, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(as::addic(rt, rt, 0));
    }
    // 64-bit big-endian data word holding a label's address (TOC entries).
    void label_dword(Label l)
    {
        words_.push_back(0);
        label_word(l);
    }

    // Resolves fixups; returns the image as big-endian bytes.
    std::vector<u8> finish()
    {
        for (const Fixup& f : fixups_) {
            const u32 at = base_ + f.index * 4;
            const u32 target = address(f.label);
            u32& w = words_[f.index];
            switch (f.kind) {
            case Fix::Branch24: {
                const s64 disp = static_cast<s64>(target) - at;
                if (disp < -(1 << 25) || disp >= (1 << 25)) throw std::runtime_error("Assembler: branch out of range");
                w |= static_cast<u32>(disp) & 0x03FFFFFC;
                break;
            }
            case Fix::Branch14: {
                const s64 disp = static_cast<s64>(target) - at;
                if (disp < -(1 << 15) || disp >= (1 << 15)) throw std::runtime_error("Assembler: bc out of range");
                w |= static_cast<u32>(disp) & 0xFFFC;
                break;
            }
            case Fix::Abs32: w = target; break;
            case Fix::Rel32: w = target - address(f.from); break;
            case Fix::Hi16: w |= target >> 16; break;
            case Fix::Ha16: w |= ((target + 0x8000) >> 16) & 0xFFFF; break;
            case Fix::Lo16: w |= target & 0xFFFF; break;
            }
        }
        std::vector<u8> bytes(words_.size() * 4);
        for (usize i = 0; i < words_.size(); i++) {
            bytes[i * 4 + 0] = static_cast<u8>(words_[i] >> 24);
            bytes[i * 4 + 1] = static_cast<u8>(words_[i] >> 16);
            bytes[i * 4 + 2] = static_cast<u8>(words_[i] >> 8);
            bytes[i * 4 + 3] = static_cast<u8>(words_[i]);
        }
        return bytes;
    }

private:
    enum class Fix : u8 { Branch24, Branch14, Abs32, Rel32, Hi16, Ha16, Lo16 };
    struct Fixup {
        Fix kind;
        Label label;
        Label from;
        u32 index;
    };

    void fixup(Fix kind, Label l, u32 word)
    {
        fixups_.push_back({kind, l, {}, static_cast<u32>(words_.size())});
        words_.push_back(word);
    }

    u32 base_;
    std::vector<u32> words_;
    std::vector<u32> bound_;
    std::vector<Fixup> fixups_;
};

} // namespace pspocket::recomp::ppu::as
