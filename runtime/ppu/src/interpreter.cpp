// PPU interpreter (oracle and fallback).
//
// Semantics follow RPCS3's interpreter, rpcs3/Emu/Cell/PPUInterpreter.cpp (GPLv2, D-001); each case names the
// RPCS3 function it mirrors when the semantics come from there. Deviations, all deliberate and documented:
// - D-form loads/stores with rA = 0 use the literal 0 as the architecture says (RPCS3's "op.ra || 1" uses r0).
// - Update forms always use rA (RPCS3 mixes "rA" and "rA|0" for the invalid rA = 0 form).
// - FPSCR, Rc=1 FP record forms, fctiw rounding mode: see pspocket/ppu/fpu.h.
// - VMX: interpreter_vmx.cpp, model in pspocket/ppu/vmx.h.
// - Reservations: single-thread model, pspocket/ppu/runtime.h.

#include "pspocket/ppu/interpreter.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/fpu.h"
#include "pspocket/ppu/runtime.h"

#pragma clang fp contract(off)

namespace pspocket::ppu {
namespace {

constexpr std::string_view kChan = "ppu";

inline u64 dup32(u32 x) { return x | (u64{x} << 32); }

// rpcs3/Emu/Cell/PPUOpcodes.h ppu_rotate_mask: MASK(mb, me) in IBM bit numbering, wrapping when mb > me.
constexpr u64 rotate_mask(u32 mb, u32 me)
{
    const u64 mask = ~0ull << (~(me - mb) & 63);
    return (mask >> (mb & 63)) | (mask << ((64 - mb) & 63));
}

struct AddResult {
    u64 result;
    bool carry;
};

// PPUInterpreter.cpp add_flags_result_t: a + b + c with carry out.
inline AddResult add64_flags(u64 a, u64 b, bool c = false)
{
    const u64 r1 = a + b;
    bool carry = r1 < a;
    const u64 r2 = r1 + (c ? 1 : 0);
    carry |= r2 < r1;
    return {r2, carry};
}

inline u64 sext16(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

inline void set_cr(PPUContext& ctx, u32 field, bool lt, bool gt, bool eq, bool so)
{
    u8* f = &ctx.cr[field * 4];
    f[PS_CR_LT] = lt;
    f[PS_CR_GT] = gt;
    f[PS_CR_EQ] = eq;
    f[PS_CR_SO] = so;
}

template <class T>
inline void set_cr_cmp(PPUContext& ctx, u32 field, T a, T b)
{
    set_cr(ctx, field, a < b, a > b, a == b, ctx.xer_so != 0);
}

// Record form of an integer instruction: CR0 from the 64-bit result as signed, SO from XER.
inline void record(PPUContext& ctx, u64 r) { set_cr_cmp<s64>(ctx, 0, static_cast<s64>(r), 0); }

// PPUInterpreter.cpp ppu_ov_set
inline void set_ov(PPUContext& ctx, bool ov)
{
    ctx.xer_ov = ov;
    ctx.xer_so |= static_cast<u8>(ov);
}

// Record form of an FP instruction: CR1 = FPSCR[FX FEX VX OX] (fpu.h).
inline void record_fp(PPUContext& ctx)
{
    const u32 f = ctx.fpscr;
    set_cr(ctx, 1, (f & PS_FPSCR_FX) != 0, (f & PS_FPSCR_FEX) != 0, (f & PS_FPSCR_VX) != 0, (f & PS_FPSCR_OX) != 0);
}

inline void set_fpscr(PPUContext& ctx, u32 value)
{
    static std::atomic<bool> warned{false};
    if ((value & PS_FPSCR_RN) != 0 && !warned.exchange(true)) {
        PS_LOG_WARN(kChan, "guest set FPSCR[RN] = {} at {:#010x}: fctiw/fctid honour it, arithmetic still rounds to "
                           "nearest (pspocket/ppu/fpu.h)", value & PS_FPSCR_RN, ctx.cia);
    }
    ctx.fpscr = value;
}

[[noreturn]] void fatal_unsupported(const PPUContext& ctx, const Inst& in)
{
    PS_FATAL(kChan, "unsupported PPU instruction at {:#010x}: {:08x} '{}' ({})", ctx.cia, in.raw,
        disassemble(in, ctx.cia), unsupported_reason(in));
}

inline u64 timebase() { return ps_ppu_timebase(); }

} // namespace

void execute(PPUContext& ctx, u8* base, const Inst& in)
{
    const u32 pc = ctx.cia;
    u32 next = pc + 4;
    u64* gpr = ctx.gpr;
    double* fpr = ctx.fpr;
    u8* cr = ctx.cr;
    const u32 rt = in.rt(), ra = in.ra(), rb = in.rb();
    const u32 rs = rt;

    // Effective addresses: (rA|0) + d, rA + d (update forms), (rA|0) + rB, rA + rB (update forms).
    auto ea_d = [&] { return (ra ? gpr[ra] : 0) + sext16(in.simm()); };
    auto ea_du = [&] { return gpr[ra] + sext16(in.simm()); };
    auto ea_ds = [&] { return (ra ? gpr[ra] : 0) + sext16(in.ds()); };
    auto ea_dsu = [&] { return gpr[ra] + sext16(in.ds()); };
    auto ea_x = [&] { return (ra ? gpr[ra] : 0) + gpr[rb]; };
    auto ea_xu = [&] { return gpr[ra] + gpr[rb]; };
    auto g32 = [](u64 ea) { return static_cast<u32>(ea); };

    switch (in.op) {
    // ---------------------------------------------------------------- integer arithmetic (ADDI ... DIVW)
    case Op::ADDI: gpr[rt] = (ra ? gpr[ra] : 0) + sext16(in.simm()); break;
    case Op::ADDIS: gpr[rt] = (ra ? gpr[ra] : 0) + static_cast<u64>(static_cast<s64>(in.simm() * 65536)); break;
    case Op::ADDIC:
    case Op::ADDIC_: { // ADDIC
        const AddResult r = add64_flags(gpr[ra], sext16(in.simm()));
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.op == Op::ADDIC_) record(ctx, r.result);
        break;
    }
    case Op::SUBFIC: { // SUBFIC
        const AddResult r = add64_flags(~gpr[ra], sext16(in.simm()), true);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        break;
    }
    case Op::MULLI: gpr[rt] = gpr[ra] * sext16(in.simm()); break; // MULLI (low 64 bits, no signed overflow UB)
    case Op::ADD: {
        const u64 a = gpr[ra], b = gpr[rb], r = a + b;
        gpr[rt] = r;
        if (in.oe()) set_ov(ctx, (a >> 63 == b >> 63) && (a >> 63 != r >> 63));
        if (in.rc()) record(ctx, r);
        break;
    }
    case Op::ADDC: {
        const u64 a = gpr[ra], b = gpr[rb];
        const AddResult r = add64_flags(a, b);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (a >> 63 == b >> 63) && (a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::ADDE: {
        const u64 a = gpr[ra], b = gpr[rb];
        const AddResult r = add64_flags(a, b, ctx.xer_ca != 0);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (a >> 63 == b >> 63) && (a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::ADDZE: {
        const u64 a = gpr[ra];
        const AddResult r = add64_flags(a, 0, ctx.xer_ca != 0);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (a >> 63 == 0) && (a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::ADDME: {
        const u64 a = gpr[ra];
        const AddResult r = add64_flags(a, ~0ull, ctx.xer_ca != 0);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (a >> 63 == 1) && (a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::SUBF: {
        const u64 a = gpr[ra], b = gpr[rb], r = b - a;
        gpr[rt] = r;
        if (in.oe()) set_ov(ctx, (~a >> 63 == b >> 63) && (~a >> 63 != r >> 63));
        if (in.rc()) record(ctx, r);
        break;
    }
    case Op::SUBFC: {
        const u64 a = gpr[ra], b = gpr[rb];
        const AddResult r = add64_flags(~a, b, true);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (~a >> 63 == b >> 63) && (~a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::SUBFE: {
        const u64 a = gpr[ra], b = gpr[rb];
        const AddResult r = add64_flags(~a, b, ctx.xer_ca != 0);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (~a >> 63 == b >> 63) && (~a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::SUBFZE: {
        const u64 a = gpr[ra];
        const AddResult r = add64_flags(~a, 0, ctx.xer_ca != 0);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (~a >> 63 == 0) && (~a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::SUBFME: {
        const u64 a = gpr[ra];
        const AddResult r = add64_flags(~a, ~0ull, ctx.xer_ca != 0);
        gpr[rt] = r.result;
        ctx.xer_ca = r.carry;
        if (in.oe()) set_ov(ctx, (~a >> 63 == 1) && (~a >> 63 != r.result >> 63));
        if (in.rc()) record(ctx, r.result);
        break;
    }
    case Op::NEG: {
        const u64 a = gpr[ra], r = 0 - a;
        gpr[rt] = r;
        if (in.oe()) set_ov(ctx, a == (1ull << 63));
        if (in.rc()) record(ctx, r);
        break;
    }
    case Op::MULLW: {
        const s64 p = s64{static_cast<s32>(gpr[ra])} * static_cast<s32>(gpr[rb]);
        gpr[rt] = static_cast<u64>(p);
        if (in.oe()) set_ov(ctx, p < INT32_MIN || p > INT32_MAX);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::MULLD: { // MULLD: OV when the high doubleword is not the sign extension of the low one
        const s64 a = static_cast<s64>(gpr[ra]), b = static_cast<s64>(gpr[rb]);
        const s128 p = static_cast<s128>(a) * b;
        const u64 lo = static_cast<u64>(p);
        gpr[rt] = lo;
        if (in.oe()) set_ov(ctx, static_cast<s64>(p >> 64) != (static_cast<s64>(lo) >> 63));
        if (in.rc()) record(ctx, lo);
        break;
    }
    case Op::MULHW: {
        const s32 a = static_cast<s32>(gpr[ra]), b = static_cast<s32>(gpr[rb]);
        gpr[rt] = static_cast<u64>((s64{a} * b) >> 32);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::MULHWU: {
        const u32 a = static_cast<u32>(gpr[ra]), b = static_cast<u32>(gpr[rb]);
        gpr[rt] = (u64{a} * b) >> 32;
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::MULHD: {
        const s128 p = static_cast<s128>(static_cast<s64>(gpr[ra])) * static_cast<s64>(gpr[rb]);
        gpr[rt] = static_cast<u64>(p >> 64);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::MULHDU: {
        const u128 p = static_cast<u128>(gpr[ra]) * gpr[rb];
        gpr[rt] = static_cast<u64>(p >> 64);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::DIVW: { // DIVW: undefined results (x/0, INT_MIN/-1) are 0, as RPCS3
        const s32 a = static_cast<s32>(gpr[ra]), b = static_cast<s32>(gpr[rb]);
        const bool o = b == 0 || (a == INT32_MIN && b == -1);
        gpr[rt] = o ? 0 : static_cast<u32>(a / b);
        if (in.oe()) set_ov(ctx, o);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::DIVWU: {
        const u32 a = static_cast<u32>(gpr[ra]), b = static_cast<u32>(gpr[rb]);
        gpr[rt] = b == 0 ? 0 : a / b;
        if (in.oe()) set_ov(ctx, b == 0);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::DIVD: {
        const s64 a = static_cast<s64>(gpr[ra]), b = static_cast<s64>(gpr[rb]);
        const bool o = b == 0 || (a == INT64_MIN && b == -1);
        gpr[rt] = o ? 0 : static_cast<u64>(a / b);
        if (in.oe()) set_ov(ctx, o);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }
    case Op::DIVDU: {
        const u64 a = gpr[ra], b = gpr[rb];
        gpr[rt] = b == 0 ? 0 : a / b;
        if (in.oe()) set_ov(ctx, b == 0);
        if (in.rc()) record(ctx, gpr[rt]);
        break;
    }

    // ---------------------------------------------------------------- logical
    case Op::AND: gpr[ra] = gpr[rs] & gpr[rb]; if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::ANDC: gpr[ra] = gpr[rs] & ~gpr[rb]; if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::OR: gpr[ra] = gpr[rs] | gpr[rb]; if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::ORC: gpr[ra] = gpr[rs] | ~gpr[rb]; if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::XOR: gpr[ra] = gpr[rs] ^ gpr[rb]; if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::NOR: gpr[ra] = ~(gpr[rs] | gpr[rb]); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::NAND: gpr[ra] = ~(gpr[rs] & gpr[rb]); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::EQV: gpr[ra] = ~(gpr[rs] ^ gpr[rb]); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::ORI: gpr[ra] = gpr[rs] | in.uimm(); break;
    case Op::ORIS: gpr[ra] = gpr[rs] | (u64{in.uimm()} << 16); break;
    case Op::XORI: gpr[ra] = gpr[rs] ^ in.uimm(); break;
    case Op::XORIS: gpr[ra] = gpr[rs] ^ (u64{in.uimm()} << 16); break;
    case Op::ANDI_: gpr[ra] = gpr[rs] & in.uimm(); record(ctx, gpr[ra]); break;
    case Op::ANDIS_: gpr[ra] = gpr[rs] & (u64{in.uimm()} << 16); record(ctx, gpr[ra]); break;
    case Op::EXTSB: gpr[ra] = static_cast<u64>(static_cast<s64>(static_cast<s8>(gpr[rs]))); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::EXTSH: gpr[ra] = static_cast<u64>(static_cast<s64>(static_cast<s16>(gpr[rs]))); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::EXTSW: gpr[ra] = static_cast<u64>(static_cast<s64>(static_cast<s32>(gpr[rs]))); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::CNTLZW: gpr[ra] = static_cast<u64>(std::countl_zero(static_cast<u32>(gpr[rs]))); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::CNTLZD: gpr[ra] = static_cast<u64>(std::countl_zero(gpr[rs])); if (in.rc()) record(ctx, gpr[ra]); break;

    // ---------------------------------------------------------------- shifts (SLW SRW SLD SRD SRAW SRAD SRAWI SRADI)
    case Op::SLW: gpr[ra] = static_cast<u32>(gpr[rs] << (gpr[rb] & 0x3f)); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::SRW: gpr[ra] = (gpr[rs] & 0xffffffff) >> (gpr[rb] & 0x3f); if (in.rc()) record(ctx, gpr[ra]); break;
    case Op::SLD: {
        const u32 n = static_cast<u32>(gpr[rb] & 0x7f);
        gpr[ra] = (n & 0x40) ? 0 : gpr[rs] << n;
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::SRD: {
        const u32 n = static_cast<u32>(gpr[rb] & 0x7f);
        gpr[ra] = (n & 0x40) ? 0 : gpr[rs] >> n;
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::SRAW: {
        const s32 v = static_cast<s32>(gpr[rs]);
        const u32 shift = static_cast<u32>(gpr[rb] & 63);
        if (shift > 31) {
            gpr[ra] = v < 0 ? ~0ull : 0;
            ctx.xer_ca = v < 0;
        } else {
            gpr[ra] = static_cast<u64>(static_cast<s64>(v >> shift));
            ctx.xer_ca = v < 0 && (gpr[ra] << shift) != static_cast<u64>(static_cast<s64>(v));
        }
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::SRAD: {
        const s64 v = static_cast<s64>(gpr[rs]);
        const u32 shift = static_cast<u32>(gpr[rb] & 127);
        if (shift > 63) {
            gpr[ra] = v < 0 ? ~0ull : 0;
            ctx.xer_ca = v < 0;
        } else {
            gpr[ra] = static_cast<u64>(v >> shift);
            ctx.xer_ca = v < 0 && (gpr[ra] << shift) != static_cast<u64>(v);
        }
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::SRAWI: {
        const s32 v = static_cast<s32>(gpr[rs]);
        const u32 sh = in.sh32();
        gpr[ra] = static_cast<u64>(static_cast<s64>(v >> sh));
        ctx.xer_ca = v < 0 && static_cast<u32>(gpr[ra] << sh) != static_cast<u32>(v);
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::SRADI: {
        const s64 v = static_cast<s64>(gpr[rs]);
        const u32 sh = in.sh64();
        gpr[ra] = static_cast<u64>(v >> sh);
        ctx.xer_ca = v < 0 && (gpr[ra] << sh) != static_cast<u64>(v);
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }

    // ---------------------------------------------------------------- rotates (RLWINM ... RLDCR)
    case Op::RLWINM:
        gpr[ra] = dup32(std::rotl(static_cast<u32>(gpr[rs]), static_cast<int>(in.sh32()))) &
            rotate_mask(32 + in.mb32(), 32 + in.me32());
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    case Op::RLWNM:
        gpr[ra] = dup32(std::rotl(static_cast<u32>(gpr[rs]), static_cast<int>(gpr[rb] & 0x1f))) &
            rotate_mask(32 + in.mb32(), 32 + in.me32());
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    case Op::RLWIMI: {
        const u64 mask = rotate_mask(32 + in.mb32(), 32 + in.me32());
        gpr[ra] = (gpr[ra] & ~mask) | (dup32(std::rotl(static_cast<u32>(gpr[rs]), static_cast<int>(in.sh32()))) & mask);
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::RLDICL:
        gpr[ra] = std::rotl(gpr[rs], static_cast<int>(in.sh64())) & (~0ull >> in.mb64());
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    case Op::RLDICR:
        gpr[ra] = std::rotl(gpr[rs], static_cast<int>(in.sh64())) & (~0ull << (in.mb64() ^ 63));
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    case Op::RLDIC:
        gpr[ra] = std::rotl(gpr[rs], static_cast<int>(in.sh64())) & rotate_mask(in.mb64(), in.sh64() ^ 63);
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    case Op::RLDIMI: {
        const u64 mask = rotate_mask(in.mb64(), in.sh64() ^ 63);
        gpr[ra] = (gpr[ra] & ~mask) | (std::rotl(gpr[rs], static_cast<int>(in.sh64())) & mask);
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    }
    case Op::RLDCL:
        gpr[ra] = std::rotl(gpr[rs], static_cast<int>(gpr[rb] & 0x3f)) & (~0ull >> in.mb64());
        if (in.rc()) record(ctx, gpr[ra]);
        break;
    case Op::RLDCR:
        gpr[ra] = std::rotl(gpr[rs], static_cast<int>(gpr[rb] & 0x3f)) & (~0ull << (in.mb64() ^ 63));
        if (in.rc()) record(ctx, gpr[ra]);
        break;

    // ---------------------------------------------------------------- compares
    case Op::CMPI:
        if (in.l10())
            set_cr_cmp<s64>(ctx, in.crfd(), static_cast<s64>(gpr[ra]), in.simm());
        else
            set_cr_cmp<s32>(ctx, in.crfd(), static_cast<s32>(gpr[ra]), in.simm());
        break;
    case Op::CMPLI:
        if (in.l10())
            set_cr_cmp<u64>(ctx, in.crfd(), gpr[ra], in.uimm());
        else
            set_cr_cmp<u32>(ctx, in.crfd(), static_cast<u32>(gpr[ra]), in.uimm());
        break;
    case Op::CMP:
        if (in.l10())
            set_cr_cmp<s64>(ctx, in.crfd(), static_cast<s64>(gpr[ra]), static_cast<s64>(gpr[rb]));
        else
            set_cr_cmp<s32>(ctx, in.crfd(), static_cast<s32>(gpr[ra]), static_cast<s32>(gpr[rb]));
        break;
    case Op::CMPL:
        if (in.l10())
            set_cr_cmp<u64>(ctx, in.crfd(), gpr[ra], gpr[rb]);
        else
            set_cr_cmp<u32>(ctx, in.crfd(), static_cast<u32>(gpr[ra]), static_cast<u32>(gpr[rb]));
        break;

    // ---------------------------------------------------------------- traps (TW TWI TD TDI)
    case Op::TW:
    case Op::TWI:
    case Op::TD:
    case Op::TDI: {
        const bool is64 = in.op == Op::TD || in.op == Op::TDI;
        const bool imm = in.op == Op::TWI || in.op == Op::TDI;
        const u64 bv = imm ? sext16(in.simm()) : gpr[rb];
        s64 a, b;
        u64 au, bu;
        if (is64) {
            a = static_cast<s64>(gpr[ra]);
            b = static_cast<s64>(bv);
            au = gpr[ra];
            bu = bv;
        } else {
            a = static_cast<s32>(gpr[ra]);
            b = static_cast<s32>(bv);
            au = static_cast<u32>(gpr[ra]);
            bu = static_cast<u32>(bv);
        }
        const u32 to = in.to();
        if (((to & 0x10) && a < b) || ((to & 0x08) && a > b) || ((to & 0x04) && a == b) || ((to & 0x02) && au < bu) ||
            ((to & 0x01) && au > bu)) {
            ps_ppu_trap(&ctx, base, pc);
        }
        break;
    }

    // ---------------------------------------------------------------- CR logical
    case Op::CRAND: cr[in.crbd()] = cr[in.crba()] & cr[in.crbb()]; break;
    case Op::CRANDC: cr[in.crbd()] = cr[in.crba()] & (cr[in.crbb()] ^ 1); break;
    case Op::CREQV: cr[in.crbd()] = (cr[in.crba()] ^ cr[in.crbb()]) ^ 1; break;
    case Op::CRNAND: cr[in.crbd()] = (cr[in.crba()] & cr[in.crbb()]) ^ 1; break;
    case Op::CRNOR: cr[in.crbd()] = (cr[in.crba()] | cr[in.crbb()]) ^ 1; break;
    case Op::CROR: cr[in.crbd()] = cr[in.crba()] | cr[in.crbb()]; break;
    case Op::CRORC: cr[in.crbd()] = cr[in.crba()] | (cr[in.crbb()] ^ 1); break;
    case Op::CRXOR: cr[in.crbd()] = cr[in.crba()] ^ cr[in.crbb()]; break;
    case Op::MCRF: std::memmove(&cr[in.crfd() * 4], &cr[in.crfs() * 4], 4); break;

    // ---------------------------------------------------------------- branches (B BC BCLR BCCTR)
    case Op::B:
        if (in.lk()) ctx.lr = pc + 4;
        next = branch_target(in, pc);
        break;
    case Op::BC:
    case Op::BCLR: {
        const u32 bo = in.bo();
        const bool bo0 = (bo & 0x10) != 0, bo1 = (bo & 0x08) != 0, bo2 = (bo & 0x04) != 0, bo3 = (bo & 0x02) != 0;
        ctx.ctr -= bo2 ? 0 : 1;
        const bool ctr_ok = bo2 || ((ctx.ctr != 0) != bo3);
        const bool cond_ok = bo0 || ((cr[in.bi()] != 0) == bo1);
        const u32 target = in.op == Op::BC ? branch_target(in, pc) : static_cast<u32>(ctx.lr) & ~3u;
        if (in.lk()) ctx.lr = pc + 4;
        if (ctr_ok && cond_ok) next = target;
        break;
    }
    case Op::BCCTR: // BCCTR never decrements CTR (RPCS3)
        if (in.lk()) ctx.lr = pc + 4;
        if ((in.bo() & 0x10) || (cr[in.bi()] != 0) == ((in.bo() & 0x08) != 0)) next = static_cast<u32>(ctx.ctr) & ~3u;
        break;

    // ---------------------------------------------------------------- system
    case Op::SC:
        if (in.raw != 0x44000002u) fatal_unsupported(ctx, in);
        ps_ppu_syscall(&ctx, base);
        break;
    case Op::SYNC:
    case Op::EIEIO: std::atomic_thread_fence(std::memory_order_seq_cst); break;
    case Op::ISYNC: std::atomic_thread_fence(std::memory_order_acquire); break;
    case Op::DCBT:
    case Op::DCBTST:
    case Op::DCBF:
    case Op::DCBST:
    case Op::DCBI:
    case Op::ICBI:
    case Op::DST:
    case Op::DSTST:
    case Op::DSS: break; // cache and stream hints
    case Op::DCBZ: { // DCBZ: 128-byte line on Cell; guest stores stay volatile (D-013)
        const u32 line = g32(ea_x()) & ~127u;
        for (u32 i = 0; i < 128; i += 8) ps_write64(base, line + i, 0);
        break;
    }

    // ---------------------------------------------------------------- SPRs and CR transfer
    case Op::MFSPR:
        switch (in.spr()) {
        case kSprXer:
            gpr[rt] = (u64{ctx.xer_so} << 31) | (u64{ctx.xer_ov} << 30) | (u64{ctx.xer_ca} << 29) | ctx.xer_bc;
            break;
        case kSprLr: gpr[rt] = ctx.lr; break;
        case kSprCtr: gpr[rt] = ctx.ctr; break;
        case kSprVrsave: gpr[rt] = ctx.vrsave; break;
        case kSprTbl: gpr[rt] = timebase(); break;
        case kSprTbu: gpr[rt] = timebase() >> 32; break;
        default: fatal_unsupported(ctx, in);
        }
        break;
    case Op::MTSPR:
        switch (in.spr()) {
        case kSprXer: {
            const u64 v = gpr[rs];
            ctx.xer_so = (v >> 31) & 1;
            ctx.xer_ov = (v >> 30) & 1;
            ctx.xer_ca = (v >> 29) & 1;
            ctx.xer_bc = v & 0x7f;
            break;
        }
        case kSprLr: ctx.lr = gpr[rs]; break;
        case kSprCtr: ctx.ctr = gpr[rs]; break;
        case kSprVrsave: ctx.vrsave = static_cast<u32>(gpr[rs]); break;
        default: fatal_unsupported(ctx, in);
        }
        break;
    case Op::MFTB:
        switch (in.spr()) {
        case kSprTbl: gpr[rt] = timebase(); break;
        case kSprTbu: gpr[rt] = timebase() >> 32; break;
        default: fatal_unsupported(ctx, in);
        }
        break;
    case Op::MFCR: {
        u32 v = 0;
        for (u32 i = 0; i < 32; i++) v = (v << 1) | (cr[i] & 1);
        gpr[rt] = v;
        break;
    }
    case Op::MFOCRF: { // MFOCRF: the selected field, other bits zero
        if (std::popcount(in.fxm()) != 1) fatal_unsupported(ctx, in);
        const u32 p = (static_cast<u32>(std::countl_zero(in.fxm())) & 7) * 4;
        const u32 v = (u32{cr[p]} << 3) | (u32{cr[p + 1]} << 2) | (u32{cr[p + 2]} << 1) | cr[p + 3];
        gpr[rt] = u64{v} << (p ^ 0x1c);
        break;
    }
    case Op::MTCRF:
    case Op::MTOCRF: { // MTOCRF: mtocrf takes the first selected field (countl_zero & 7), mtcrf every selected one
        const u64 s = gpr[rs];
        auto put = [&](u32 i) {
            const u32 v = static_cast<u32>(s >> ((i * 4) ^ 0x1c)) & 0xf;
            cr[i * 4 + 0] = (v >> 3) & 1;
            cr[i * 4 + 1] = (v >> 2) & 1;
            cr[i * 4 + 2] = (v >> 1) & 1;
            cr[i * 4 + 3] = v & 1;
        };
        if (in.op == Op::MTOCRF) {
            put(static_cast<u32>(std::countl_zero(in.fxm())) & 7);
        } else {
            for (u32 i = 0; i < 8; i++)
                if (in.fxm() & (0x80u >> i)) put(i);
        }
        break;
    }

    // ---------------------------------------------------------------- integer loads
    case Op::LBZ: gpr[rt] = ps_read8(base, g32(ea_d())); break;
    case Op::LHZ: gpr[rt] = ps_read16(base, g32(ea_d())); break;
    case Op::LHA: gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s16>(ps_read16(base, g32(ea_d()))))); break;
    case Op::LWZ: gpr[rt] = ps_read32(base, g32(ea_d())); break;
    case Op::LBZU: { const u64 ea = ea_du(); gpr[rt] = ps_read8(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LHZU: { const u64 ea = ea_du(); gpr[rt] = ps_read16(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LHAU: {
        const u64 ea = ea_du();
        gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s16>(ps_read16(base, g32(ea)))));
        gpr[ra] = ea;
        break;
    }
    case Op::LWZU: { const u64 ea = ea_du(); gpr[rt] = ps_read32(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LD: gpr[rt] = ps_read64(base, g32(ea_ds())); break;
    case Op::LDU: { const u64 ea = ea_dsu(); gpr[rt] = ps_read64(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LWA: gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s32>(ps_read32(base, g32(ea_ds()))))); break;
    case Op::LBZX: gpr[rt] = ps_read8(base, g32(ea_x())); break;
    case Op::LHZX: gpr[rt] = ps_read16(base, g32(ea_x())); break;
    case Op::LHAX: gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s16>(ps_read16(base, g32(ea_x()))))); break;
    case Op::LWZX: gpr[rt] = ps_read32(base, g32(ea_x())); break;
    case Op::LWAX: gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s32>(ps_read32(base, g32(ea_x()))))); break;
    case Op::LDX: gpr[rt] = ps_read64(base, g32(ea_x())); break;
    case Op::LBZUX: { const u64 ea = ea_xu(); gpr[rt] = ps_read8(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LHZUX: { const u64 ea = ea_xu(); gpr[rt] = ps_read16(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LHAUX: {
        const u64 ea = ea_xu();
        gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s16>(ps_read16(base, g32(ea)))));
        gpr[ra] = ea;
        break;
    }
    case Op::LWZUX: { const u64 ea = ea_xu(); gpr[rt] = ps_read32(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LWAUX: {
        const u64 ea = ea_xu();
        gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<s32>(ps_read32(base, g32(ea)))));
        gpr[ra] = ea;
        break;
    }
    case Op::LDUX: { const u64 ea = ea_xu(); gpr[rt] = ps_read64(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LHBRX: gpr[rt] = ps_bswap16(ps_read16(base, g32(ea_x()))); break;
    case Op::LWBRX: gpr[rt] = ps_bswap32(ps_read32(base, g32(ea_x()))); break;
    case Op::LDBRX: gpr[rt] = ps_bswap64(ps_read64(base, g32(ea_x()))); break;
    case Op::LWARX: gpr[rt] = ps_ppu_lwarx(&ctx, base, g32(ea_x())); break;
    case Op::LDARX: gpr[rt] = ps_ppu_ldarx(&ctx, base, g32(ea_x())); break;
    case Op::LMW: { // LMW
        u64 ea = ea_d();
        for (u32 i = rt; i < 32; i++, ea += 4) gpr[i] = ps_read32(base, g32(ea));
        break;
    }
    case Op::LSWI: { // LSWI
        u64 ea = ra ? gpr[ra] : 0;
        u32 n = in.nb() ? in.nb() : 32;
        u32 reg = rt;
        while (n > 0) {
            if (n > 3) {
                gpr[reg] = ps_read32(base, g32(ea));
                ea += 4;
                n -= 4;
            } else {
                u32 buf = 0;
                u32 shift = 24;
                while (n > 0) {
                    buf |= u32{ps_read8(base, g32(ea))} << shift;
                    ea++;
                    n--;
                    shift -= 8;
                }
                gpr[reg] = buf;
            }
            reg = (reg + 1) & 31;
        }
        break;
    }
    case Op::LSWX: ps_ppu_lswx(&ctx, base, g32(ea_x()), rt); break;

    // ---------------------------------------------------------------- integer stores
    case Op::STB: ps_write8(base, g32(ea_d()), static_cast<u8>(gpr[rs])); break;
    case Op::STH: ps_write16(base, g32(ea_d()), static_cast<u16>(gpr[rs])); break;
    case Op::STW: ps_write32(base, g32(ea_d()), static_cast<u32>(gpr[rs])); break;
    case Op::STBU: { const u64 ea = ea_du(); ps_write8(base, g32(ea), static_cast<u8>(gpr[rs])); gpr[ra] = ea; break; }
    case Op::STHU: { const u64 ea = ea_du(); ps_write16(base, g32(ea), static_cast<u16>(gpr[rs])); gpr[ra] = ea; break; }
    case Op::STWU: { const u64 ea = ea_du(); ps_write32(base, g32(ea), static_cast<u32>(gpr[rs])); gpr[ra] = ea; break; }
    case Op::STD: ps_write64(base, g32(ea_ds()), gpr[rs]); break;
    case Op::STDU: { const u64 ea = ea_dsu(); ps_write64(base, g32(ea), gpr[rs]); gpr[ra] = ea; break; }
    case Op::STBX: ps_write8(base, g32(ea_x()), static_cast<u8>(gpr[rs])); break;
    case Op::STHX: ps_write16(base, g32(ea_x()), static_cast<u16>(gpr[rs])); break;
    case Op::STWX: ps_write32(base, g32(ea_x()), static_cast<u32>(gpr[rs])); break;
    case Op::STDX: ps_write64(base, g32(ea_x()), gpr[rs]); break;
    case Op::STBUX: { const u64 ea = ea_xu(); ps_write8(base, g32(ea), static_cast<u8>(gpr[rs])); gpr[ra] = ea; break; }
    case Op::STHUX: { const u64 ea = ea_xu(); ps_write16(base, g32(ea), static_cast<u16>(gpr[rs])); gpr[ra] = ea; break; }
    case Op::STWUX: { const u64 ea = ea_xu(); ps_write32(base, g32(ea), static_cast<u32>(gpr[rs])); gpr[ra] = ea; break; }
    case Op::STDUX: { const u64 ea = ea_xu(); ps_write64(base, g32(ea), gpr[rs]); gpr[ra] = ea; break; }
    case Op::STHBRX: ps_write16(base, g32(ea_x()), ps_bswap16(static_cast<u16>(gpr[rs]))); break;
    case Op::STWBRX: ps_write32(base, g32(ea_x()), ps_bswap32(static_cast<u32>(gpr[rs]))); break;
    case Op::STDBRX: ps_write64(base, g32(ea_x()), ps_bswap64(gpr[rs])); break;
    case Op::STWCX_: {
        const bool ok = ps_ppu_stwcx(&ctx, base, g32(ea_x()), static_cast<u32>(gpr[rs])) != 0;
        set_cr(ctx, 0, false, false, ok, ctx.xer_so != 0);
        break;
    }
    case Op::STDCX_: {
        const bool ok = ps_ppu_stdcx(&ctx, base, g32(ea_x()), gpr[rs]) != 0;
        set_cr(ctx, 0, false, false, ok, ctx.xer_so != 0);
        break;
    }
    case Op::STMW: { // STMW
        u64 ea = ea_d();
        for (u32 i = rs; i < 32; i++, ea += 4) ps_write32(base, g32(ea), static_cast<u32>(gpr[i]));
        break;
    }
    case Op::STSWI: { // STSWI
        u64 ea = ra ? gpr[ra] : 0;
        u32 n = in.nb() ? in.nb() : 32;
        u32 reg = rs;
        while (n > 0) {
            if (n > 3) {
                ps_write32(base, g32(ea), static_cast<u32>(gpr[reg]));
                ea += 4;
                n -= 4;
            } else {
                u32 buf = static_cast<u32>(gpr[reg]);
                while (n > 0) {
                    ps_write8(base, g32(ea), static_cast<u8>(buf >> 24));
                    buf <<= 8;
                    ea++;
                    n--;
                }
            }
            reg = (reg + 1) & 31;
        }
        break;
    }
    case Op::STSWX: ps_ppu_stswx(&ctx, base, g32(ea_x()), rs); break;

    // ---------------------------------------------------------------- FP loads and stores
    case Op::LFS: fpr[rt] = static_cast<double>(ps_readf32(base, g32(ea_d()))); break;
    case Op::LFD: fpr[rt] = ps_readf64(base, g32(ea_d())); break;
    case Op::LFSU: { const u64 ea = ea_du(); fpr[rt] = static_cast<double>(ps_readf32(base, g32(ea))); gpr[ra] = ea; break; }
    case Op::LFDU: { const u64 ea = ea_du(); fpr[rt] = ps_readf64(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::LFSX: fpr[rt] = static_cast<double>(ps_readf32(base, g32(ea_x()))); break;
    case Op::LFDX: fpr[rt] = ps_readf64(base, g32(ea_x())); break;
    case Op::LFSUX: { const u64 ea = ea_xu(); fpr[rt] = static_cast<double>(ps_readf32(base, g32(ea))); gpr[ra] = ea; break; }
    case Op::LFDUX: { const u64 ea = ea_xu(); fpr[rt] = ps_readf64(base, g32(ea)); gpr[ra] = ea; break; }
    case Op::STFS: ps_writef32(base, g32(ea_d()), static_cast<float>(fpr[rs])); break;
    case Op::STFD: ps_writef64(base, g32(ea_d()), fpr[rs]); break;
    case Op::STFSU: { const u64 ea = ea_du(); ps_writef32(base, g32(ea), static_cast<float>(fpr[rs])); gpr[ra] = ea; break; }
    case Op::STFDU: { const u64 ea = ea_du(); ps_writef64(base, g32(ea), fpr[rs]); gpr[ra] = ea; break; }
    case Op::STFSX: ps_writef32(base, g32(ea_x()), static_cast<float>(fpr[rs])); break;
    case Op::STFDX: ps_writef64(base, g32(ea_x()), fpr[rs]); break;
    case Op::STFSUX: { const u64 ea = ea_xu(); ps_writef32(base, g32(ea), static_cast<float>(fpr[rs])); gpr[ra] = ea; break; }
    case Op::STFDUX: { const u64 ea = ea_xu(); ps_writef64(base, g32(ea), fpr[rs]); gpr[ra] = ea; break; }
    case Op::STFIWX: ps_write32(base, g32(ea_x()), static_cast<u32>(ps_f64_bits(fpr[rs]))); break;

    // ---------------------------------------------------------------- FP arithmetic (FADD ... FNMADDS)
    case Op::FADD: fpr[rt] = fpr[ra] + fpr[rb]; if (in.rc()) record_fp(ctx); break;
    case Op::FSUB: fpr[rt] = fpr[ra] - fpr[rb]; if (in.rc()) record_fp(ctx); break;
    case Op::FMUL: fpr[rt] = fpr[ra] * fpr[in.rc_reg()]; if (in.rc()) record_fp(ctx); break;
    case Op::FDIV: fpr[rt] = fpr[ra] / fpr[rb]; if (in.rc()) record_fp(ctx); break;
    case Op::FSQRT: fpr[rt] = std::sqrt(fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FADDS: fpr[rt] = ps_ppu_round_single(fpr[ra] + fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FSUBS: fpr[rt] = ps_ppu_round_single(fpr[ra] - fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FMULS: fpr[rt] = ps_ppu_round_single(fpr[ra] * fpr[in.rc_reg()]); if (in.rc()) record_fp(ctx); break;
    case Op::FDIVS: fpr[rt] = ps_ppu_round_single(fpr[ra] / fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FSQRTS: fpr[rt] = ps_ppu_round_single(std::sqrt(fpr[rb])); if (in.rc()) record_fp(ctx); break;
    case Op::FMADD: fpr[rt] = std::fma(fpr[ra], fpr[in.rc_reg()], fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FMSUB: fpr[rt] = std::fma(fpr[ra], fpr[in.rc_reg()], -fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FNMADD: fpr[rt] = -std::fma(fpr[ra], fpr[in.rc_reg()], fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FNMSUB: fpr[rt] = -std::fma(fpr[ra], fpr[in.rc_reg()], -fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FMADDS:
        fpr[rt] = ps_ppu_round_single(std::fma(fpr[ra], fpr[in.rc_reg()], fpr[rb]));
        if (in.rc()) record_fp(ctx);
        break;
    case Op::FMSUBS:
        fpr[rt] = ps_ppu_round_single(std::fma(fpr[ra], fpr[in.rc_reg()], -fpr[rb]));
        if (in.rc()) record_fp(ctx);
        break;
    case Op::FNMADDS:
        fpr[rt] = ps_ppu_round_single(-std::fma(fpr[ra], fpr[in.rc_reg()], fpr[rb]));
        if (in.rc()) record_fp(ctx);
        break;
    case Op::FNMSUBS:
        fpr[rt] = ps_ppu_round_single(-std::fma(fpr[ra], fpr[in.rc_reg()], -fpr[rb]));
        if (in.rc()) record_fp(ctx);
        break;
    case Op::FSEL: fpr[rt] = fpr[ra] >= 0.0 ? fpr[in.rc_reg()] : fpr[rb]; if (in.rc()) record_fp(ctx); break;
    case Op::FRES: fpr[rt] = ps_ppu_fres(fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FRSQRTE: fpr[rt] = ps_ppu_frsqrte(fpr[rb]); if (in.rc()) record_fp(ctx); break;

    // ---------------------------------------------------------------- FP moves and conversions
    case Op::FMR: fpr[rt] = fpr[rb]; if (in.rc()) record_fp(ctx); break;
    case Op::FNEG: fpr[rt] = ps_f64_from_bits(ps_f64_bits(fpr[rb]) ^ 0x8000000000000000ull); if (in.rc()) record_fp(ctx); break;
    case Op::FABS: fpr[rt] = ps_f64_from_bits(ps_f64_bits(fpr[rb]) & ~0x8000000000000000ull); if (in.rc()) record_fp(ctx); break;
    case Op::FNABS: fpr[rt] = ps_f64_from_bits(ps_f64_bits(fpr[rb]) | 0x8000000000000000ull); if (in.rc()) record_fp(ctx); break;
    case Op::FRSP: fpr[rt] = ps_ppu_round_single(fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FCTIW: fpr[rt] = ps_ppu_fctiw(fpr[rb], ctx.fpscr & PS_FPSCR_RN); if (in.rc()) record_fp(ctx); break;
    case Op::FCTIWZ: fpr[rt] = ps_ppu_fctiw(fpr[rb], PS_FPSCR_RN_ZERO); if (in.rc()) record_fp(ctx); break;
    case Op::FCTID: fpr[rt] = ps_ppu_fctid(fpr[rb], ctx.fpscr & PS_FPSCR_RN); if (in.rc()) record_fp(ctx); break;
    case Op::FCTIDZ: fpr[rt] = ps_ppu_fctid(fpr[rb], PS_FPSCR_RN_ZERO); if (in.rc()) record_fp(ctx); break;
    case Op::FCFID: fpr[rt] = ps_ppu_fcfid(fpr[rb]); if (in.rc()) record_fp(ctx); break;
    case Op::FCMPU:
    case Op::FCMPO: { // FCMPU (fcmpo identical here: VXVC/VXSNAN are not modelled)
        const u32 c = ps_fp_compare_bits(fpr[ra], fpr[rb]);
        set_cr(ctx, in.crfd(), (c & 8) != 0, (c & 4) != 0, (c & 2) != 0, (c & 1) != 0);
        ctx.fpscr = ps_fpscr_set_fpcc(ctx.fpscr, c);
        break;
    }

    // ---------------------------------------------------------------- FPSCR
    case Op::MFFS: fpr[rt] = ps_f64_from_bits(ctx.fpscr); if (in.rc()) record_fp(ctx); break;
    case Op::MTFSF: set_fpscr(ctx, ps_fpscr_mtfsf(ctx.fpscr, in.flm(), ps_f64_bits(fpr[rb]))); if (in.rc()) record_fp(ctx); break;
    case Op::MTFSFI: set_fpscr(ctx, ps_fpscr_mtfsfi(ctx.fpscr, in.crfd(), in.fpimm())); if (in.rc()) record_fp(ctx); break;
    case Op::MTFSB0: set_fpscr(ctx, ps_fpscr_mtfsb0(ctx.fpscr, in.crbd())); if (in.rc()) record_fp(ctx); break;
    case Op::MTFSB1: set_fpscr(ctx, ps_fpscr_mtfsb1(ctx.fpscr, in.crbd())); if (in.rc()) record_fp(ctx); break;
    case Op::MCRFS: {
        const u32 v = ps_fpscr_field(ctx.fpscr, in.crfs());
        set_cr(ctx, in.crfd(), (v & 8) != 0, (v & 4) != 0, (v & 2) != 0, (v & 1) != 0);
        ctx.fpscr = ps_fpscr_mcrfs_clear(ctx.fpscr, in.crfs());
        break;
    }

    default:
        if ((in.flags() & OPF_VMX) && is_supported(in)) {
            execute_vmx(ctx, base, in);
            break;
        }
        fatal_unsupported(ctx, in);
    }

    ctx.cia = next;
}

void step(PPUContext& ctx, u8* base) { execute(ctx, base, decode(ps_read32(base, ctx.cia))); }

RunResult run(PPUContext& ctx, u8* base, const RunOptions& opt)
{
    RunResult res;
    const u32 stop_pc = static_cast<u32>(opt.stop_lr) & ~3u;
    auto at_stop = [&] { return opt.stop_at_return && ctx.cia == stop_pc && ctx.gpr[1] == opt.stop_sp; };

    for (;;) {
        if (at_stop()) {
            res.reason = StopReason::Returned;
            return res;
        }
        if (res.steps >= opt.max_steps) {
            res.reason = StopReason::StepLimit;
            return res;
        }
        const Inst inst = decode(ps_read32(base, ctx.cia));
        execute(ctx, base, inst);
        res.steps++;

        // Interop: a branch that lands on a lifted function runs it. Lifted code returns like the guest's blr, so
        // execution continues at LR afterwards (also correct for tail calls and local branches to an entry).
        if (opt.call_lifted && (inst.flags() & OPF_BRANCH)) {
            while (!at_stop()) {
                const ps_ppu_fn fn = ps_ppu_dispatch_find(ctx.cia);
                if (!fn) break;
                fn(&ctx, base);
                res.lifted_calls++;
                ctx.cia = static_cast<u32>(ctx.lr) & ~3u;
            }
        }
    }
}

std::vector<Form> supported_forms()
{
    std::vector<Form> forms;
    for (const OpInfo& info : all_ops()) {
        if (info.flags & OPF_NOSUPPORT) continue;
        for (u32 v : op_variants(info.op)) forms.push_back({info.op, v});
    }
    return forms;
}

} // namespace pspocket::ppu
