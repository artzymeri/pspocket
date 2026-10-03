#pragma once

// SPU assembler helper: a generic encoder driven by the op table (runtime/spu/include/pspocket/spu/ops.inc), one
// named encoder per instruction, and a small label-based Assembler for synthetic programs and test cases.
// Header-only. Branch and LS-relative offsets are in bytes (multiples of 4, or 16 for lqd/stqd); absolute targets
// are LS addresses.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/spu/decoder.hpp"

namespace pspocket::recomp::spu::as {

using ::pspocket::spu::Fmt;
using ::pspocket::spu::Op;
using ::pspocket::spu::OpInfo;

// Operands by role. Which ones an op uses depends on its format (decoder.hpp).
struct Fields {
    u32 rt = 0;  // target (RRR: bits 4-10), or the channel/branch-condition register
    u32 ra = 0;  // also the channel number (rdch/wrch/rchcnt) and SPR number
    u32 rb = 0;
    u32 rc = 0;  // RRR only
    s32 imm = 0; // I7/I8/I10/I16/I18 field value (not scaled), or the stop signal
    s32 hint = 0; // hbr/hbra/hbrr: word offset of the hinted branch (9-bit signed)
    bool e = false, d = false; // indirect branches: enable / disable interrupts
    bool c = false;            // sync C bit, hbr P bit
};

// Encodes op with the given fields (each masked to its width).
inline u32 encode(Op op, const Fields& f)
{
    const OpInfo& info = ::pspocket::spu::op_info(op);
    const u32 opc = static_cast<u32>(info.opcode) << (32 - info.opbits);
    const u32 rt = f.rt & 0x7F, ra = f.ra & 0x7F, rb = f.rb & 0x7F, rc = f.rc & 0x7F;
    const u32 imm = static_cast<u32>(f.imm);
    switch (info.fmt) {
    case Fmt::NONE: return opc | (f.c ? 1u << 20 : 0u);
    case Fmt::RR:
    case Fmt::RR_RT_RA:
    case Fmt::RR_RT:
    case Fmt::RR_RA:
    case Fmt::CH_READ:
    case Fmt::CH_WRITE:
    case Fmt::SPR_READ:
    case Fmt::SPR_WRITE: return opc | (rb << 14) | (ra << 7) | rt;
    case Fmt::RRR: return opc | (rt << 21) | (rb << 14) | (ra << 7) | rc;
    case Fmt::RI7: return opc | ((imm & 0x7F) << 14) | (ra << 7) | rt;
    case Fmt::RI8: return opc | ((imm & 0xFF) << 14) | (ra << 7) | rt;
    case Fmt::RI10:
    case Fmt::RI10_LS: return opc | ((imm & 0x3FF) << 14) | (ra << 7) | rt;
    case Fmt::RI16:
    case Fmt::RI16_LS:
    case Fmt::RI16_BR: return opc | ((imm & 0xFFFF) << 7) | rt;
    case Fmt::RI18: return opc | ((imm & 0x3FFFF) << 7) | rt;
    case Fmt::BI: return opc | (f.d ? 1u << 19 : 0u) | (f.e ? 1u << 18 : 0u) | (ra << 7) | rt;
    case Fmt::HBR: {
        const u32 h = static_cast<u32>(f.hint);
        return opc | (f.c ? 1u << 20 : 0u) | (((h >> 7) & 3) << 14) | (ra << 7) | (h & 0x7F);
    }
    case Fmt::HBRI: {
        const u32 h = static_cast<u32>(f.hint);
        return opc | (((h >> 7) & 3) << 23) | ((imm & 0xFFFF) << 7) | (h & 0x7F);
    }
    case Fmt::STOP: return opc | (imm & 0x3FFF);
    }
    return opc;
}

namespace detail {

inline u32 rr(Op op, u32 rt, u32 ra, u32 rb) { return encode(op, Fields{.rt = rt, .ra = ra, .rb = rb}); }
inline u32 rrr(Op op, u32 rt, u32 ra, u32 rb, u32 rc) { return encode(op, Fields{.rt = rt, .ra = ra, .rb = rb, .rc = rc}); }
inline u32 ri7(Op op, u32 rt, u32 ra, s32 i) { return encode(op, Fields{.rt = rt, .ra = ra, .imm = i}); }
inline u32 ri8(Op op, u32 rt, u32 ra, u32 i) { return encode(op, Fields{.rt = rt, .ra = ra, .imm = static_cast<s32>(i)}); }
inline u32 ri10(Op op, u32 rt, u32 ra, s32 i) { return encode(op, Fields{.rt = rt, .ra = ra, .imm = i}); }
inline u32 ri16(Op op, u32 rt, s32 i) { return encode(op, Fields{.rt = rt, .imm = i}); }
inline u32 ri18(Op op, u32 rt, u32 i) { return encode(op, Fields{.rt = rt, .imm = static_cast<s32>(i)}); }
inline u32 bi(Op op, u32 rt, u32 ra, bool e, bool d) { return encode(op, Fields{.rt = rt, .ra = ra, .e = e, .d = d}); }
inline u32 hbr_form(s32 brinst_offset, u32 ra, bool p)
{
    return encode(Op::HBR, Fields{.ra = ra, .hint = brinst_offset >> 2, .c = p});
}
inline u32 hbri(Op op, s32 brinst_offset, s32 i16) { return encode(op, Fields{.imm = i16, .hint = brinst_offset >> 2}); }

} // namespace detail

using namespace detail;

// ---- one encoder per instruction (generated from ops.inc; "and", "or", "xor" get a trailing underscore) ----
inline u32 stop(u32 signal) { return signal & 0x3FFF; }
inline u32 lnop() { return rr(Op::LNOP, 0, 0, 0); }
inline u32 sync() { return rr(Op::SYNC, 0, 0, 0); }
inline u32 dsync() { return rr(Op::DSYNC, 0, 0, 0); }
inline u32 mfspr(u32 rt, u32 spr) { return rr(Op::MFSPR, rt, spr, 0); }
inline u32 rdch(u32 rt, u32 channel) { return rr(Op::RDCH, rt, channel, 0); }
inline u32 rchcnt(u32 rt, u32 channel) { return rr(Op::RCHCNT, rt, channel, 0); }
inline u32 mtspr(u32 spr, u32 rt) { return rr(Op::MTSPR, rt, spr, 0); }
inline u32 wrch(u32 channel, u32 rt) { return rr(Op::WRCH, rt, channel, 0); }
inline u32 stopd(u32 rt, u32 ra, u32 rb) { return rr(Op::STOPD, rt, ra, rb); }
inline u32 nop(u32 rt) { return rr(Op::NOP, rt, 0, 0); }
inline u32 sf(u32 rt, u32 ra, u32 rb) { return rr(Op::SF, rt, ra, rb); }
inline u32 or_(u32 rt, u32 ra, u32 rb) { return rr(Op::OR, rt, ra, rb); }
inline u32 bg(u32 rt, u32 ra, u32 rb) { return rr(Op::BG, rt, ra, rb); }
inline u32 sfh(u32 rt, u32 ra, u32 rb) { return rr(Op::SFH, rt, ra, rb); }
inline u32 nor(u32 rt, u32 ra, u32 rb) { return rr(Op::NOR, rt, ra, rb); }
inline u32 absdb(u32 rt, u32 ra, u32 rb) { return rr(Op::ABSDB, rt, ra, rb); }
inline u32 rot(u32 rt, u32 ra, u32 rb) { return rr(Op::ROT, rt, ra, rb); }
inline u32 rotm(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTM, rt, ra, rb); }
inline u32 rotma(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTMA, rt, ra, rb); }
inline u32 shl(u32 rt, u32 ra, u32 rb) { return rr(Op::SHL, rt, ra, rb); }
inline u32 roth(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTH, rt, ra, rb); }
inline u32 rothm(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTHM, rt, ra, rb); }
inline u32 rotmah(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTMAH, rt, ra, rb); }
inline u32 shlh(u32 rt, u32 ra, u32 rb) { return rr(Op::SHLH, rt, ra, rb); }
inline u32 roti(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTI, rt, ra, i7); }
inline u32 rotmi(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTMI, rt, ra, i7); }
inline u32 rotmai(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTMAI, rt, ra, i7); }
inline u32 shli(u32 rt, u32 ra, s32 i7) { return ri7(Op::SHLI, rt, ra, i7); }
inline u32 rothi(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTHI, rt, ra, i7); }
inline u32 rothmi(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTHMI, rt, ra, i7); }
inline u32 rotmahi(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTMAHI, rt, ra, i7); }
inline u32 shlhi(u32 rt, u32 ra, s32 i7) { return ri7(Op::SHLHI, rt, ra, i7); }
inline u32 a(u32 rt, u32 ra, u32 rb) { return rr(Op::A, rt, ra, rb); }
inline u32 and_(u32 rt, u32 ra, u32 rb) { return rr(Op::AND, rt, ra, rb); }
inline u32 cg(u32 rt, u32 ra, u32 rb) { return rr(Op::CG, rt, ra, rb); }
inline u32 ah(u32 rt, u32 ra, u32 rb) { return rr(Op::AH, rt, ra, rb); }
inline u32 nand(u32 rt, u32 ra, u32 rb) { return rr(Op::NAND, rt, ra, rb); }
inline u32 avgb(u32 rt, u32 ra, u32 rb) { return rr(Op::AVGB, rt, ra, rb); }
inline u32 gb(u32 rt, u32 ra) { return rr(Op::GB, rt, ra, 0); }
inline u32 gbh(u32 rt, u32 ra) { return rr(Op::GBH, rt, ra, 0); }
inline u32 gbb(u32 rt, u32 ra) { return rr(Op::GBB, rt, ra, 0); }
inline u32 fsm(u32 rt, u32 ra) { return rr(Op::FSM, rt, ra, 0); }
inline u32 fsmh(u32 rt, u32 ra) { return rr(Op::FSMH, rt, ra, 0); }
inline u32 fsmb(u32 rt, u32 ra) { return rr(Op::FSMB, rt, ra, 0); }
inline u32 rotqbybi(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTQBYBI, rt, ra, rb); }
inline u32 rotqmbybi(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTQMBYBI, rt, ra, rb); }
inline u32 shlqbybi(u32 rt, u32 ra, u32 rb) { return rr(Op::SHLQBYBI, rt, ra, rb); }
inline u32 cbx(u32 rt, u32 ra, u32 rb) { return rr(Op::CBX, rt, ra, rb); }
inline u32 chx(u32 rt, u32 ra, u32 rb) { return rr(Op::CHX, rt, ra, rb); }
inline u32 cwx(u32 rt, u32 ra, u32 rb) { return rr(Op::CWX, rt, ra, rb); }
inline u32 cdx(u32 rt, u32 ra, u32 rb) { return rr(Op::CDX, rt, ra, rb); }
inline u32 rotqbi(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTQBI, rt, ra, rb); }
inline u32 rotqmbi(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTQMBI, rt, ra, rb); }
inline u32 shlqbi(u32 rt, u32 ra, u32 rb) { return rr(Op::SHLQBI, rt, ra, rb); }
inline u32 rotqby(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTQBY, rt, ra, rb); }
inline u32 rotqmby(u32 rt, u32 ra, u32 rb) { return rr(Op::ROTQMBY, rt, ra, rb); }
inline u32 shlqby(u32 rt, u32 ra, u32 rb) { return rr(Op::SHLQBY, rt, ra, rb); }
inline u32 orx(u32 rt, u32 ra) { return rr(Op::ORX, rt, ra, 0); }
inline u32 cbd(u32 rt, u32 ra, s32 i7) { return ri7(Op::CBD, rt, ra, i7); }
inline u32 chd(u32 rt, u32 ra, s32 i7) { return ri7(Op::CHD, rt, ra, i7); }
inline u32 cwd(u32 rt, u32 ra, s32 i7) { return ri7(Op::CWD, rt, ra, i7); }
inline u32 cdd(u32 rt, u32 ra, s32 i7) { return ri7(Op::CDD, rt, ra, i7); }
inline u32 rotqbii(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTQBII, rt, ra, i7); }
inline u32 rotqmbii(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTQMBII, rt, ra, i7); }
inline u32 shlqbii(u32 rt, u32 ra, s32 i7) { return ri7(Op::SHLQBII, rt, ra, i7); }
inline u32 rotqbyi(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTQBYI, rt, ra, i7); }
inline u32 rotqmbyi(u32 rt, u32 ra, s32 i7) { return ri7(Op::ROTQMBYI, rt, ra, i7); }
inline u32 shlqbyi(u32 rt, u32 ra, s32 i7) { return ri7(Op::SHLQBYI, rt, ra, i7); }
inline u32 cgt(u32 rt, u32 ra, u32 rb) { return rr(Op::CGT, rt, ra, rb); }
inline u32 xor_(u32 rt, u32 ra, u32 rb) { return rr(Op::XOR, rt, ra, rb); }
inline u32 cgth(u32 rt, u32 ra, u32 rb) { return rr(Op::CGTH, rt, ra, rb); }
inline u32 eqv(u32 rt, u32 ra, u32 rb) { return rr(Op::EQV, rt, ra, rb); }
inline u32 cgtb(u32 rt, u32 ra, u32 rb) { return rr(Op::CGTB, rt, ra, rb); }
inline u32 sumb(u32 rt, u32 ra, u32 rb) { return rr(Op::SUMB, rt, ra, rb); }
inline u32 hgt(u32 rt, u32 ra, u32 rb) { return rr(Op::HGT, rt, ra, rb); }
inline u32 clz(u32 rt, u32 ra) { return rr(Op::CLZ, rt, ra, 0); }
inline u32 xswd(u32 rt, u32 ra) { return rr(Op::XSWD, rt, ra, 0); }
inline u32 xshw(u32 rt, u32 ra) { return rr(Op::XSHW, rt, ra, 0); }
inline u32 cntb(u32 rt, u32 ra) { return rr(Op::CNTB, rt, ra, 0); }
inline u32 xsbh(u32 rt, u32 ra) { return rr(Op::XSBH, rt, ra, 0); }
inline u32 clgt(u32 rt, u32 ra, u32 rb) { return rr(Op::CLGT, rt, ra, rb); }
inline u32 andc(u32 rt, u32 ra, u32 rb) { return rr(Op::ANDC, rt, ra, rb); }
inline u32 clgth(u32 rt, u32 ra, u32 rb) { return rr(Op::CLGTH, rt, ra, rb); }
inline u32 orc(u32 rt, u32 ra, u32 rb) { return rr(Op::ORC, rt, ra, rb); }
inline u32 clgtb(u32 rt, u32 ra, u32 rb) { return rr(Op::CLGTB, rt, ra, rb); }
inline u32 hlgt(u32 rt, u32 ra, u32 rb) { return rr(Op::HLGT, rt, ra, rb); }
inline u32 ceq(u32 rt, u32 ra, u32 rb) { return rr(Op::CEQ, rt, ra, rb); }
inline u32 mpyhhu(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYHHU, rt, ra, rb); }
inline u32 addx(u32 rt, u32 ra, u32 rb) { return rr(Op::ADDX, rt, ra, rb); }
inline u32 sfx(u32 rt, u32 ra, u32 rb) { return rr(Op::SFX, rt, ra, rb); }
inline u32 cgx(u32 rt, u32 ra, u32 rb) { return rr(Op::CGX, rt, ra, rb); }
inline u32 bgx(u32 rt, u32 ra, u32 rb) { return rr(Op::BGX, rt, ra, rb); }
inline u32 mpyhha(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYHHA, rt, ra, rb); }
inline u32 mpyhhau(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYHHAU, rt, ra, rb); }
inline u32 mpy(u32 rt, u32 ra, u32 rb) { return rr(Op::MPY, rt, ra, rb); }
inline u32 mpyh(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYH, rt, ra, rb); }
inline u32 mpyhh(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYHH, rt, ra, rb); }
inline u32 mpys(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYS, rt, ra, rb); }
inline u32 ceqh(u32 rt, u32 ra, u32 rb) { return rr(Op::CEQH, rt, ra, rb); }
inline u32 mpyu(u32 rt, u32 ra, u32 rb) { return rr(Op::MPYU, rt, ra, rb); }
inline u32 ceqb(u32 rt, u32 ra, u32 rb) { return rr(Op::CEQB, rt, ra, rb); }
inline u32 heq(u32 rt, u32 ra, u32 rb) { return rr(Op::HEQ, rt, ra, rb); }
inline u32 stqx(u32 rt, u32 ra, u32 rb) { return rr(Op::STQX, rt, ra, rb); }
inline u32 lqx(u32 rt, u32 ra, u32 rb) { return rr(Op::LQX, rt, ra, rb); }
// LS address (absolute)
inline u32 stqa(u32 rt, u32 ls_addr) { return ri16(Op::STQA, rt, static_cast<s32>(ls_addr >> 2)); }
// byte offset from this instruction
inline u32 stqr(u32 rt, s32 byte_offset) { return ri16(Op::STQR, rt, byte_offset >> 2); }
// LS address (absolute)
inline u32 lqa(u32 rt, u32 ls_addr) { return ri16(Op::LQA, rt, static_cast<s32>(ls_addr >> 2)); }
// byte offset from this instruction
inline u32 lqr(u32 rt, s32 byte_offset) { return ri16(Op::LQR, rt, byte_offset >> 2); }
// byte_offset: multiple of 16 in [-8192, 8176]
inline u32 stqd(u32 rt, s32 byte_offset, u32 ra) { return ri10(Op::STQD, rt, ra, byte_offset >> 4); }
// byte_offset: multiple of 16 in [-8192, 8176]
inline u32 lqd(u32 rt, s32 byte_offset, u32 ra) { return ri10(Op::LQD, rt, ra, byte_offset >> 4); }
inline u32 biz(u32 rt, u32 ra, bool e = false, bool d = false) { return bi(Op::BIZ, rt, ra, e, d); }
inline u32 binz(u32 rt, u32 ra, bool e = false, bool d = false) { return bi(Op::BINZ, rt, ra, e, d); }
inline u32 bihz(u32 rt, u32 ra, bool e = false, bool d = false) { return bi(Op::BIHZ, rt, ra, e, d); }
inline u32 bihnz(u32 rt, u32 ra, bool e = false, bool d = false) { return bi(Op::BIHNZ, rt, ra, e, d); }
inline u32 bi(u32 ra, bool e = false, bool d = false) { return bi(Op::BI, 0, ra, e, d); }
inline u32 bisl(u32 rt, u32 ra, bool e = false, bool d = false) { return bi(Op::BISL, rt, ra, e, d); }
inline u32 iret(u32 ra, bool e = false, bool d = false) { return bi(Op::IRET, 0, ra, e, d); }
inline u32 bisled(u32 rt, u32 ra, bool e = false, bool d = false) { return bi(Op::BISLED, rt, ra, e, d); }
// brinst_offset: byte offset of the hinted branch from this instruction
inline u32 hbr(s32 brinst_offset, u32 ra, bool p = false) { return hbr_form(brinst_offset, ra, p); }
inline u32 brz(u32 rt, s32 byte_offset) { return ri16(Op::BRZ, rt, byte_offset >> 2); }
inline u32 brnz(u32 rt, s32 byte_offset) { return ri16(Op::BRNZ, rt, byte_offset >> 2); }
inline u32 brhz(u32 rt, s32 byte_offset) { return ri16(Op::BRHZ, rt, byte_offset >> 2); }
inline u32 brhnz(u32 rt, s32 byte_offset) { return ri16(Op::BRHNZ, rt, byte_offset >> 2); }
inline u32 bra(u32 ls_addr) { return ri16(Op::BRA, 0, static_cast<s32>(ls_addr >> 2)); }
inline u32 brasl(u32 rt, u32 ls_addr) { return ri16(Op::BRASL, rt, static_cast<s32>(ls_addr >> 2)); }
inline u32 br(s32 byte_offset) { return ri16(Op::BR, 0, byte_offset >> 2); }
inline u32 brsl(u32 rt, s32 byte_offset) { return ri16(Op::BRSL, rt, byte_offset >> 2); }
inline u32 hbra(s32 brinst_offset, u32 target_ls_addr) { return hbri(Op::HBRA, brinst_offset, static_cast<s32>(target_ls_addr >> 2)); }
inline u32 hbrr(s32 brinst_offset, s32 target_byte_offset) { return hbri(Op::HBRR, brinst_offset, target_byte_offset >> 2); }
inline u32 fsmbi(u32 rt, s32 i16) { return ri16(Op::FSMBI, rt, i16); }
inline u32 il(u32 rt, s32 i16) { return ri16(Op::IL, rt, i16); }
inline u32 ilhu(u32 rt, s32 i16) { return ri16(Op::ILHU, rt, i16); }
inline u32 ilh(u32 rt, s32 i16) { return ri16(Op::ILH, rt, i16); }
inline u32 iohl(u32 rt, s32 i16) { return ri16(Op::IOHL, rt, i16); }
inline u32 ila(u32 rt, u32 i18) { return ri18(Op::ILA, rt, i18); }
inline u32 ori(u32 rt, u32 ra, s32 i10) { return ri10(Op::ORI, rt, ra, i10); }
inline u32 orhi(u32 rt, u32 ra, s32 i10) { return ri10(Op::ORHI, rt, ra, i10); }
inline u32 orbi(u32 rt, u32 ra, s32 i10) { return ri10(Op::ORBI, rt, ra, i10); }
inline u32 sfi(u32 rt, u32 ra, s32 i10) { return ri10(Op::SFI, rt, ra, i10); }
inline u32 sfhi(u32 rt, u32 ra, s32 i10) { return ri10(Op::SFHI, rt, ra, i10); }
inline u32 andi(u32 rt, u32 ra, s32 i10) { return ri10(Op::ANDI, rt, ra, i10); }
inline u32 andhi(u32 rt, u32 ra, s32 i10) { return ri10(Op::ANDHI, rt, ra, i10); }
inline u32 andbi(u32 rt, u32 ra, s32 i10) { return ri10(Op::ANDBI, rt, ra, i10); }
inline u32 ai(u32 rt, u32 ra, s32 i10) { return ri10(Op::AI, rt, ra, i10); }
inline u32 ahi(u32 rt, u32 ra, s32 i10) { return ri10(Op::AHI, rt, ra, i10); }
inline u32 xori(u32 rt, u32 ra, s32 i10) { return ri10(Op::XORI, rt, ra, i10); }
inline u32 xorhi(u32 rt, u32 ra, s32 i10) { return ri10(Op::XORHI, rt, ra, i10); }
inline u32 xorbi(u32 rt, u32 ra, s32 i10) { return ri10(Op::XORBI, rt, ra, i10); }
inline u32 cgti(u32 rt, u32 ra, s32 i10) { return ri10(Op::CGTI, rt, ra, i10); }
inline u32 cgthi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CGTHI, rt, ra, i10); }
inline u32 cgtbi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CGTBI, rt, ra, i10); }
inline u32 hgti(u32 rt, u32 ra, s32 i10) { return ri10(Op::HGTI, rt, ra, i10); }
inline u32 clgti(u32 rt, u32 ra, s32 i10) { return ri10(Op::CLGTI, rt, ra, i10); }
inline u32 clgthi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CLGTHI, rt, ra, i10); }
inline u32 clgtbi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CLGTBI, rt, ra, i10); }
inline u32 hlgti(u32 rt, u32 ra, s32 i10) { return ri10(Op::HLGTI, rt, ra, i10); }
inline u32 mpyi(u32 rt, u32 ra, s32 i10) { return ri10(Op::MPYI, rt, ra, i10); }
inline u32 mpyui(u32 rt, u32 ra, s32 i10) { return ri10(Op::MPYUI, rt, ra, i10); }
inline u32 ceqi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CEQI, rt, ra, i10); }
inline u32 ceqhi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CEQHI, rt, ra, i10); }
inline u32 ceqbi(u32 rt, u32 ra, s32 i10) { return ri10(Op::CEQBI, rt, ra, i10); }
inline u32 heqi(u32 rt, u32 ra, s32 i10) { return ri10(Op::HEQI, rt, ra, i10); }
inline u32 selb(u32 rt, u32 ra, u32 rb, u32 rc) { return rrr(Op::SELB, rt, ra, rb, rc); }
inline u32 shufb(u32 rt, u32 ra, u32 rb, u32 rc) { return rrr(Op::SHUFB, rt, ra, rb, rc); }
inline u32 mpya(u32 rt, u32 ra, u32 rb, u32 rc) { return rrr(Op::MPYA, rt, ra, rb, rc); }
inline u32 fnms(u32 rt, u32 ra, u32 rb, u32 rc) { return rrr(Op::FNMS, rt, ra, rb, rc); }
inline u32 fma(u32 rt, u32 ra, u32 rb, u32 rc) { return rrr(Op::FMA, rt, ra, rb, rc); }
inline u32 fms(u32 rt, u32 ra, u32 rb, u32 rc) { return rrr(Op::FMS, rt, ra, rb, rc); }
inline u32 frest(u32 rt, u32 ra) { return rr(Op::FREST, rt, ra, 0); }
inline u32 frsqest(u32 rt, u32 ra) { return rr(Op::FRSQEST, rt, ra, 0); }
inline u32 fcgt(u32 rt, u32 ra, u32 rb) { return rr(Op::FCGT, rt, ra, rb); }
inline u32 fa(u32 rt, u32 ra, u32 rb) { return rr(Op::FA, rt, ra, rb); }
inline u32 fs(u32 rt, u32 ra, u32 rb) { return rr(Op::FS, rt, ra, rb); }
inline u32 fm(u32 rt, u32 ra, u32 rb) { return rr(Op::FM, rt, ra, rb); }
inline u32 fcmgt(u32 rt, u32 ra, u32 rb) { return rr(Op::FCMGT, rt, ra, rb); }
inline u32 fscrrd(u32 rt) { return rr(Op::FSCRRD, rt, 0, 0); }
inline u32 fscrwr(u32 ra) { return rr(Op::FSCRWR, 0, ra, 0); }
inline u32 fceq(u32 rt, u32 ra, u32 rb) { return rr(Op::FCEQ, rt, ra, rb); }
inline u32 fcmeq(u32 rt, u32 ra, u32 rb) { return rr(Op::FCMEQ, rt, ra, rb); }
inline u32 fi(u32 rt, u32 ra, u32 rb) { return rr(Op::FI, rt, ra, rb); }
inline u32 cflts(u32 rt, u32 ra, u32 scale) { return ri8(Op::CFLTS, rt, ra, scale); }
inline u32 cfltu(u32 rt, u32 ra, u32 scale) { return ri8(Op::CFLTU, rt, ra, scale); }
inline u32 csflt(u32 rt, u32 ra, u32 scale) { return ri8(Op::CSFLT, rt, ra, scale); }
inline u32 cuflt(u32 rt, u32 ra, u32 scale) { return ri8(Op::CUFLT, rt, ra, scale); }
inline u32 dfa(u32 rt, u32 ra, u32 rb) { return rr(Op::DFA, rt, ra, rb); }
inline u32 dfs(u32 rt, u32 ra, u32 rb) { return rr(Op::DFS, rt, ra, rb); }
inline u32 dfm(u32 rt, u32 ra, u32 rb) { return rr(Op::DFM, rt, ra, rb); }
inline u32 dfma(u32 rt, u32 ra, u32 rb) { return rr(Op::DFMA, rt, ra, rb); }
inline u32 dfms(u32 rt, u32 ra, u32 rb) { return rr(Op::DFMS, rt, ra, rb); }
inline u32 dfnms(u32 rt, u32 ra, u32 rb) { return rr(Op::DFNMS, rt, ra, rb); }
inline u32 dfnma(u32 rt, u32 ra, u32 rb) { return rr(Op::DFNMA, rt, ra, rb); }
inline u32 fesd(u32 rt, u32 ra) { return rr(Op::FESD, rt, ra, 0); }
inline u32 frds(u32 rt, u32 ra) { return rr(Op::FRDS, rt, ra, 0); }
inline u32 dfcgt(u32 rt, u32 ra, u32 rb) { return rr(Op::DFCGT, rt, ra, rb); }
inline u32 dfcmgt(u32 rt, u32 ra, u32 rb) { return rr(Op::DFCMGT, rt, ra, rb); }
inline u32 dftsv(u32 rt, u32 ra, s32 i7) { return ri7(Op::DFTSV, rt, ra, i7); }
inline u32 dfceq(u32 rt, u32 ra, u32 rb) { return rr(Op::DFCEQ, rt, ra, rb); }
inline u32 dfcmeq(u32 rt, u32 ra, u32 rb) { return rr(Op::DFCMEQ, rt, ra, rb); }

// ---- common idioms ----
inline u32 mr(u32 rt, u32 ra) { return ori(rt, ra, 0); } // as spu-gcc: ori rt, ra, 0
inline u32 ret() { return bi(0); }                       // bi $lr

// Loads a 32-bit constant into every word of rt (one or two instructions).
inline std::vector<u32> load_imm32(u32 rt, u32 value)
{
    const s32 sv = static_cast<s32>(value);
    if (sv >= -32768 && sv <= 32767) return {il(rt, sv)};
    if (value < 0x40000) return {ila(rt, value)};
    std::vector<u32> v{ilhu(rt, static_cast<s32>(value >> 16))};
    if (value & 0xFFFF) v.push_back(iohl(rt, static_cast<s32>(value & 0xFFFF)));
    return v;
}

// Label-based assembler. Code starts at `origin` (an LS address). Branches to labels are fixed up by finish().
class Assembler {
public:
    struct Label {
        u32 id = ~0u;
    };

    explicit Assembler(u32 origin = 0) : origin_(origin) {}

    u32 origin() const { return origin_; }
    u32 pc() const { return origin_ + static_cast<u32>(words_.size()) * 4; }

    void emit(u32 insn) { words_.push_back(insn); }
    void emit(const std::vector<u32>& insns) { words_.insert(words_.end(), insns.begin(), insns.end()); }
    void word(u32 data) { words_.push_back(data); }
    void align(u32 bytes)
    {
        while ((pc() % bytes) != 0) emit(lnop());
    }

    Label label()
    {
        bound_.push_back(~0u);
        return Label{static_cast<u32>(bound_.size() - 1)};
    }
    void bind(Label l)
    {
        if (bound_.at(l.id) != ~0u) throw std::logic_error("label bound twice");
        bound_[l.id] = pc();
    }
    Label here()
    {
        Label l = label();
        bind(l);
        return l;
    }
    u32 address(Label l) const
    {
        const u32 a = bound_.at(l.id);
        if (a == ~0u) throw std::logic_error("label not bound");
        return a;
    }

    // pc-relative branches and loads to labels (I16 field, fixed up by finish())
    void br(Label l) { fixup(Op::BR, 0, l); }
    void brsl(u32 rt, Label l) { fixup(Op::BRSL, rt, l); }
    void brz(u32 rt, Label l) { fixup(Op::BRZ, rt, l); }
    void brnz(u32 rt, Label l) { fixup(Op::BRNZ, rt, l); }
    void brhz(u32 rt, Label l) { fixup(Op::BRHZ, rt, l); }
    void brhnz(u32 rt, Label l) { fixup(Op::BRHNZ, rt, l); }
    void lqr(u32 rt, Label l) { fixup(Op::LQR, rt, l); }
    void stqr(u32 rt, Label l) { fixup(Op::STQR, rt, l); }
    // absolute forms
    void bra(Label l) { fixup(Op::BRA, 0, l); }
    void brasl(u32 rt, Label l) { fixup(Op::BRASL, rt, l); }
    void lqa(u32 rt, Label l) { fixup(Op::LQA, rt, l); }
    void stqa(u32 rt, Label l) { fixup(Op::STQA, rt, l); }
    void ila(u32 rt, Label l) { fixup(Op::ILA, rt, l); } // rt = label address

    // Resolves fixups; returns the program as big-endian bytes (starting at origin()).
    std::vector<u8> finish()
    {
        for (const Fixup& f : fixups_) {
            const u32 target = address(f.label);
            const u32 at = origin_ + f.index * 4;
            u32 insn = 0;
            switch (f.op) {
            case Op::ILA: insn = ::pspocket::recomp::spu::as::ila(f.rt, target); break;
            case Op::BRA:
            case Op::BRASL:
            case Op::LQA:
            case Op::STQA: insn = ri16(f.op, f.rt, static_cast<s32>(target >> 2)); break;
            default: {
                const s32 delta = static_cast<s32>(target - at);
                if (delta < -0x20000 || delta > 0x1FFFC) throw std::out_of_range("branch out of range");
                insn = ri16(f.op, f.rt, delta >> 2);
                break;
            }
            }
            words_[f.index] = insn;
        }
        std::vector<u8> out;
        out.reserve(words_.size() * 4);
        for (u32 w : words_) {
            out.push_back(static_cast<u8>(w >> 24));
            out.push_back(static_cast<u8>(w >> 16));
            out.push_back(static_cast<u8>(w >> 8));
            out.push_back(static_cast<u8>(w));
        }
        return out;
    }

    const std::vector<u32>& words() const { return words_; }

private:
    struct Fixup {
        Op op;
        u32 rt;
        Label label;
        u32 index;
    };
    void fixup(Op op, u32 rt, Label l)
    {
        fixups_.push_back(Fixup{op, rt, l, static_cast<u32>(words_.size())});
        words_.push_back(0);
    }

    u32 origin_;
    std::vector<u32> words_;
    std::vector<u32> bound_;
    std::vector<Fixup> fixups_;
};

} // namespace pspocket::recomp::spu::as
