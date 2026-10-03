#pragma once

// PPU instruction decoder: 32-bit instruction word -> Op + field accessors. Covers the whole Cell PPE instruction
// set including VMX (which this milestone decodes and disassembles but does not execute or lift).

#include <span>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"

namespace pspocket::ppu {

// Operand formats. They drive decoding, disassembly, register usage and the generic encoder (recomp/ppu/asm.hpp).
enum class Fmt : u8 {
    NONE,
    // integer immediates
    D_RT_RA_SI,  // rT, rA, SIMM          addi addis addic addic. subfic mulli
    D_RA_RS_UI,  // rA, rS, UIMM          ori oris xori xoris andi. andis.
    D_CMPI,      // crfD, L, rA, SIMM
    D_CMPLI,     // crfD, L, rA, UIMM
    D_TO_RA_SI,  // TO, rA, SIMM          twi tdi
    D_LOAD,      // rT, d(rA)
    D_STORE,     // rS, d(rA)
    D_FLOAD,     // frT, d(rA)
    D_FSTORE,    // frS, d(rA)
    DS_LOAD,     // rT, ds(rA)
    DS_STORE,    // rS, ds(rA)
    // branches and system
    I_BRANCH,    // LI, AA, LK
    B_BRANCH,    // BO, BI, BD, AA, LK
    XL_BRANCH,   // BO, BI, BH, LK        bclr bcctr
    SC,
    XL_CRLOG,    // crbD, crbA, crbB
    XL_MCRF,     // crfD, crfS
    XL_NONE,     // isync
    // integer X/XO/M/MD forms
    XO_RT_RA_RB, // rT, rA, rB [OE] [Rc]
    XO_RT_RA,    // rT, rA [OE] [Rc]
    X_RA_RS_RB,  // rA, rS, rB [Rc]
    X_RA_RS,     // rA, rS [Rc]
    X_RA_RS_SH,  // rA, rS, SH [Rc]       srawi
    XS_RA_RS_SH, // rA, rS, SH64 [Rc]     sradi
    M_RA_RS_SH_MB_ME,
    M_RA_RS_RB_MB_ME,
    MD_RA_RS_SH_MB, // rA, rS, SH64, MB64/ME64
    MDS_RA_RS_RB_MB,
    X_CMP,       // crfD, L, rA, rB
    X_TO_RA_RB,  // TO, rA, rB
    X_LOAD,      // rT, rA, rB
    X_STORE,     // rS, rA, rB
    X_FLOAD,     // frT, rA, rB
    X_FSTORE,    // frS, rA, rB
    X_LSWI,      // rT/rS, rA, NB
    X_RA_RB,     // rA, rB                cache management
    X_DST,       // rA, rB, STRM, T
    X_DSS,       // STRM, A
    X_SYNC,      // L
    X_NONE,      // eieio
    XFX_MFSPR,   // rT, SPR
    XFX_MFTB,    // rT, TBR
    XFX_MTSPR,   // SPR, rS
    X_MFCR,      // rT
    X_MFOCRF,    // rT, FXM
    XFX_MTCRF,   // FXM, rS
    // floating point
    A_FRT_FRA_FRB,
    A_FRT_FRA_FRC,
    A_FRT_FRA_FRC_FRB,
    A_FRT_FRB,
    X_FRT_FRB,
    X_FCMP,      // crfD, frA, frB
    X_MFFS,      // frT
    XFL_MTFSF,   // FLM, frB
    X_MTFSFI,    // crfD, IMM
    X_MTFSB,     // crbD
    X_MCRFS,     // crfD, crfS
    // VMX
    VX_VD_VA_VB,
    VX_VD_VB,
    VX_VD_VB_UIMM,
    VX_VD_SIMM,
    VA_VD_VA_VB_VC,
    VA_VSLDOI,
    VXR_VD_VA_VB,
    VX_VD,
    VX_VB,
    X_VLOAD,
    X_VSTORE,
};

// Op flags.
inline constexpr u32 OPF_RC = 1u << 0;        // Rc bit (bit 31; bit 21 for VXR compares) selects a record form
inline constexpr u32 OPF_OE = 1u << 1;        // OE bit (bit 21) selects an overflow-recording form
inline constexpr u32 OPF_LK = 1u << 2;        // LK bit (bit 31)
inline constexpr u32 OPF_AA = 1u << 3;        // AA bit (bit 30)
inline constexpr u32 OPF_BRANCH = 1u << 4;
inline constexpr u32 OPF_LOAD = 1u << 5;
inline constexpr u32 OPF_STORE = 1u << 6;
inline constexpr u32 OPF_UPDATE = 1u << 7;    // writes the effective address back to rA
inline constexpr u32 OPF_INDEXED = 1u << 8;   // EA = (rA|0) + rB
inline constexpr u32 OPF_MULTI = 1u << 9;     // lmw stmw lswi lswx stswi stswx
inline constexpr u32 OPF_FP = 1u << 10;
inline constexpr u32 OPF_FPARITH = 1u << 11;  // FP arithmetic: NaN payload propagation is host-defined (as RPCS3)
inline constexpr u32 OPF_VMX = 1u << 12;      // decoded and disassembled; not executed this milestone
inline constexpr u32 OPF_NOSUPPORT = 1u << 13; // decoded; not supported (eciwx, ecowx)
inline constexpr u32 OPF_TRAP = 1u << 14;
inline constexpr u32 OPF_HINT = 1u << 15;     // cache / stream hint: architecturally a no-op here

enum class Op : u16 {
    UNKNOWN = 0,
#define PS_PPU_OP(id, mn, fmt, pri, xo, fl) id,
#include "pspocket/ppu/ops.inc"
#undef PS_PPU_OP
    COUNT
};

inline constexpr usize kOpCount = static_cast<usize>(Op::COUNT);

struct OpInfo {
    Op op;
    const char* mnemonic;
    Fmt fmt;
    u8 primary;
    u16 xo;
    u32 flags;
};

const OpInfo& op_info(Op op);
std::span<const OpInfo> all_ops(); // every op except UNKNOWN, in ops.inc order

// SPR numbers (already un-swapped).
inline constexpr u32 kSprXer = 1;
inline constexpr u32 kSprLr = 8;
inline constexpr u32 kSprCtr = 9;
inline constexpr u32 kSprVrsave = 256;
inline constexpr u32 kSprTbl = 268;
inline constexpr u32 kSprTbu = 269;

// A decoded instruction. Fields use IBM bit numbering in the comments (bit 0 = MSB).
struct Inst {
    u32 raw = 0;
    Op op = Op::UNKNOWN;

    const OpInfo& info() const { return op_info(op); }
    u32 flags() const { return info().flags; }

    u32 primary() const { return raw >> 26; }
    u32 rt() const { return (raw >> 21) & 31; } // bits 6-10: rT/rS/frT/frS/vD/vS/BO/TO/crbD
    u32 rs() const { return rt(); }
    u32 ra() const { return (raw >> 16) & 31; } // bits 11-15: rA/frA/vA/BI/crbA
    u32 rb() const { return (raw >> 11) & 31; } // bits 16-20: rB/frB/vB/crbB/SH/NB
    u32 rc_reg() const { return (raw >> 6) & 31; } // bits 21-25: frC/vC/MB
    u32 bo() const { return rt(); }
    u32 bi() const { return ra(); }
    u32 to() const { return rt(); }
    u32 crbd() const { return rt(); }
    u32 crba() const { return ra(); }
    u32 crbb() const { return rb(); }
    u32 sh32() const { return rb(); }
    u32 nb() const { return rb(); }
    u32 mb32() const { return (raw >> 6) & 31; }
    u32 me32() const { return (raw >> 1) & 31; }
    // MD/MDS/XS forms: 6-bit SH = sh[5] (bit 30) : sh[0:4] (bits 16-20); MB/ME = bit 26 : bits 21-25
    u32 sh64() const { return ((raw >> 11) & 31) | (((raw >> 1) & 1) << 5); }
    u32 mb64() const { return ((raw >> 6) & 31) | (((raw >> 5) & 1) << 5); }
    s32 simm() const { return static_cast<s16>(raw & 0xFFFF); }
    u32 uimm() const { return raw & 0xFFFF; }
    s32 ds() const { return static_cast<s16>(raw & 0xFFFC); } // DS-form displacement, already << 2
    u32 ds_xo() const { return raw & 3; }
    s32 li() const { return static_cast<s32>((raw & 0x03FFFFFCu) << 6) >> 6; } // I-form displacement
    s32 bd() const { return static_cast<s16>(raw & 0xFFFC); }                   // B-form displacement
    u32 aa() const { return (raw >> 1) & 1; }
    u32 lk() const { return raw & 1; }
    u32 rc() const { return raw & 1; }
    u32 oe() const { return (raw >> 10) & 1; }
    u32 vrc() const { return (raw >> 10) & 1; } // VXR compares: Rc at bit 21
    u32 crfd() const { return (raw >> 23) & 7; }
    u32 crfs() const { return (raw >> 18) & 7; }
    u32 l10() const { return (raw >> 21) & 1; } // compare L (64-bit)
    u32 l11() const { return (raw >> 20) & 1; } // mfocrf / mtocrf selector
    u32 fxm() const { return (raw >> 12) & 0xFF; }
    u32 spr() const { return ((raw >> 16) & 31) | (((raw >> 11) & 31) << 5); }
    u32 bh() const { return (raw >> 11) & 3; }
    u32 flm() const { return (raw >> 17) & 0xFF; }
    u32 fpimm() const { return (raw >> 12) & 0xF; } // mtfsfi U
    u32 lev() const { return (raw >> 5) & 0x7F; }
    u32 sync_l() const { return (raw >> 21) & 3; }
    u32 vsh() const { return (raw >> 6) & 15; }
    s32 vsimm() const { return static_cast<s32>(ra() << 27) >> 27; }
    u32 vuimm() const { return ra(); }
    u32 strm() const { return (raw >> 21) & 3; }
    u32 dst_t() const { return (raw >> 25) & 1; }
};

Inst decode(u32 raw);

// True when this milestone executes and lifts the instruction (interpreter and lifter agree on this list).
// VMX, eciwx/ecowx, sc with LEV != 0, unknown SPRs and invalid mfocrf masks are not supported.
bool is_supported(const Inst& inst);
// Why is_supported() is false (empty when supported).
std::string_view unsupported_reason(const Inst& inst);

// Branch target for I-form / B-form instructions at address pc.
inline u32 branch_target(const Inst& inst, u32 pc)
{
    const s32 disp = inst.op == Op::B ? inst.li() : inst.bd();
    return (inst.aa() ? 0u : pc) + static_cast<u32>(disp);
}

// Unconditional trap (TO = 31): never falls through unless the trap handler resumes.
inline bool is_unconditional_trap(const Inst& inst)
{
    return (inst.op == Op::TW || inst.op == Op::TD || inst.op == Op::TWI || inst.op == Op::TDI) && inst.to() == 31;
}

// ---- instruction forms (for coverage accounting: every supported form gets fuzz cases) ----

// Variant bits of a form: which of the Rc/OE/LK/AA bits are set (only the ones the op has).
inline constexpr u32 kVarRc = 1, kVarOe = 2, kVarLk = 4, kVarAa = 8;
u32 form_variant(const Inst& inst);
std::string form_name(Op op, u32 variant); // "addo.", "bcla", "fadds."
// All variants an op has (e.g. {0, Rc, OE, OE|Rc} for add).
std::span<const u32> op_variants(Op op);

// ---- register usage (what the lifter keeps in locals, loads at entry and spills) ----

inline constexpr u32 SPR_LR = 1u << 0;
inline constexpr u32 SPR_CTR = 1u << 1;
inline constexpr u32 SPR_XER_SO = 1u << 2;
inline constexpr u32 SPR_XER_OV = 1u << 3;
inline constexpr u32 SPR_XER_CA = 1u << 4;
inline constexpr u32 SPR_XER_BC = 1u << 5;
inline constexpr u32 SPR_FPSCR = 1u << 6;
inline constexpr u32 SPR_VRSAVE = 1u << 7;
inline constexpr u32 SPR_ALL = 0xFF;

struct RegUse {
    u32 gpr_read = 0, gpr_write = 0; // bit n = rn
    u32 fpr_read = 0, fpr_write = 0;
    u32 cr_read = 0, cr_write = 0;   // bit n = CR bit n (IBM numbering: bit 0 = cr0.lt)
    u32 spr_read = 0, spr_write = 0; // SPR_* bits
    bool mem_read = false, mem_write = false;

    RegUse& operator|=(const RegUse& o)
    {
        gpr_read |= o.gpr_read;
        gpr_write |= o.gpr_write;
        fpr_read |= o.fpr_read;
        fpr_write |= o.fpr_write;
        cr_read |= o.cr_read;
        cr_write |= o.cr_write;
        spr_read |= o.spr_read;
        spr_write |= o.spr_write;
        mem_read |= o.mem_read;
        mem_write |= o.mem_write;
        return *this;
    }
};

// Conservative (never under-approximates) register usage of a supported instruction. Unsupported instructions
// report nothing: callers treat them as "may touch anything" (the lifter spills and reloads around them).
RegUse reg_use(const Inst& inst);

// Registers lswi/stswi touch: rT .. rT + ceil(NB/4) - 1, wrapping at 32 (NB = 0 means 32 bytes).
u32 lswi_reg_mask(u32 rt, u32 nb);

} // namespace pspocket::ppu
