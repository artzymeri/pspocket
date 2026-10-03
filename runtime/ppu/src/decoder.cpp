// PPU decoder tables, built at compile time from ops.inc. Opcode layout follows RPCS3's decoder,
// rpcs3/Emu/Cell/PPUOpcodes.h (ppu_decoder::ppu_decoder).

#include "pspocket/ppu/decoder.hpp"

#include <array>
#include <bit>

namespace pspocket::ppu {
namespace {

constexpr OpInfo kOps[] = {
    {Op::UNKNOWN, "?", Fmt::NONE, 0, 0, 0},
#define PS_PPU_OP(id, mn, fmt, pri, xo, fl) {Op::id, mn, Fmt::fmt, pri, xo, fl},
#include "pspocket/ppu/ops.inc"
#undef PS_PPU_OP
};
static_assert(std::size(kOps) == kOpCount, "ops.inc and Op enum out of sync");

constexpr bool is_a_form(Fmt f)
{
    return f == Fmt::A_FRT_FRA_FRB || f == Fmt::A_FRT_FRA_FRC || f == Fmt::A_FRT_FRA_FRC_FRB || f == Fmt::A_FRT_FRB;
}

struct Tables {
    std::array<Op, 64> primary{};
    std::array<Op, 1024> g19{};
    std::array<Op, 1024> g31{};
    std::array<Op, 16> g30{};
    std::array<Op, 4> g58{};
    std::array<Op, 4> g62{};
    std::array<Op, 32> g59{};
    std::array<Op, 32> g63a{};
    std::array<Op, 1024> g63x{};
    std::array<Op, 2048> g4{};
};

constexpr Tables build_tables()
{
    Tables t{};
    for (const OpInfo& o : kOps) {
        if (o.op == Op::UNKNOWN) continue;
        // mfocrf / mtocrf share their xo with mfcr / mtcrf; decode() splits them on bit 11.
        if (o.op == Op::MFOCRF || o.op == Op::MTOCRF) continue;
        switch (o.primary) {
        case 4:
            if (o.fmt == Fmt::VA_VD_VA_VB_VC || o.fmt == Fmt::VA_VSLDOI) {
                for (u32 vc = 0; vc < 32; vc++) t.g4[(vc << 6) | o.xo] = o.op;
            } else if (o.fmt == Fmt::VXR_VD_VA_VB) {
                t.g4[o.xo] = o.op;
                t.g4[o.xo | 0x400] = o.op;
            } else {
                t.g4[o.xo] = o.op;
            }
            break;
        case 19: t.g19[o.xo] = o.op; break;
        case 30:
            if (o.fmt == Fmt::MD_RA_RS_SH_MB) {
                t.g30[o.xo * 2] = o.op; // bit 30 is sh[5]
                t.g30[o.xo * 2 + 1] = o.op;
            } else {
                t.g30[o.xo] = o.op;
            }
            break;
        case 31:
            t.g31[o.xo] = o.op;
            if (o.flags & OPF_OE) t.g31[o.xo | 0x200] = o.op;
            if (o.op == Op::SRADI) t.g31[o.xo | 1] = o.op; // bit 30 is sh[5]
            break;
        case 58: t.g58[o.xo] = o.op; break;
        case 62: t.g62[o.xo] = o.op; break;
        case 59: t.g59[o.xo] = o.op; break;
        case 63:
            if (is_a_form(o.fmt))
                t.g63a[o.xo] = o.op;
            else
                t.g63x[o.xo] = o.op;
            break;
        default: t.primary[o.primary] = o.op; break;
        }
    }
    return t;
}

constexpr Tables kTables = build_tables();

// Variant lists per op (computed once).
struct VariantTable {
    std::array<std::array<u32, 4>, kOpCount> values{};
    std::array<u8, kOpCount> count{};
};

constexpr VariantTable build_variants()
{
    VariantTable v{};
    for (const OpInfo& o : kOps) {
        const auto i = static_cast<usize>(o.op);
        u32 bits = 0;
        if (o.flags & OPF_RC) bits |= kVarRc;
        if (o.flags & OPF_OE) bits |= kVarOe;
        if (o.flags & OPF_LK) bits |= kVarLk;
        if (o.flags & OPF_AA) bits |= kVarAa;
        u8 n = 0;
        // Enumerate all subsets of `bits` (at most two bits are ever set together).
        for (u32 s = 0; s < 16; s++) {
            if ((s & ~bits) != 0) continue;
            if (n < 4) v.values[i][n++] = s;
        }
        v.count[i] = n;
    }
    return v;
}

constexpr VariantTable kVariants = build_variants();

} // namespace

const OpInfo& op_info(Op op)
{
    const auto i = static_cast<usize>(op);
    return kOps[i < kOpCount ? i : 0];
}

std::span<const OpInfo> all_ops() { return std::span<const OpInfo>(kOps).subspan(1); }

Inst decode(u32 raw)
{
    Inst inst;
    inst.raw = raw;
    const u32 primary = raw >> 26;
    Op op = Op::UNKNOWN;
    switch (primary) {
    case 4: op = kTables.g4[raw & 0x7FF]; break;
    case 19: op = kTables.g19[(raw >> 1) & 0x3FF]; break;
    case 30: op = kTables.g30[(raw >> 1) & 0xF]; break;
    case 31:
        op = kTables.g31[(raw >> 1) & 0x3FF];
        if (op == Op::MFCR && ((raw >> 20) & 1)) op = Op::MFOCRF;
        if (op == Op::MTCRF && ((raw >> 20) & 1)) op = Op::MTOCRF;
        break;
    case 58: op = kTables.g58[raw & 3]; break;
    case 62: op = kTables.g62[raw & 3]; break;
    case 59: op = kTables.g59[(raw >> 1) & 31]; break;
    case 63:
        op = kTables.g63a[(raw >> 1) & 31];
        if (op == Op::UNKNOWN) op = kTables.g63x[(raw >> 1) & 0x3FF];
        break;
    default: op = kTables.primary[primary]; break;
    }
    inst.op = op;
    return inst;
}

namespace {

bool supported_spr(u32 spr, bool read)
{
    switch (spr) {
    case kSprXer:
    case kSprLr:
    case kSprCtr:
    case kSprVrsave: return true;
    case kSprTbl:
    case kSprTbu: return read; // RPCS3 MFSPR accepts the timebase SPRs (Emu/Cell/PPUInterpreter.cpp)
    default: return false;
    }
}

} // namespace

std::string_view unsupported_reason(const Inst& inst)
{
    const OpInfo& info = inst.info();
    if (inst.op == Op::UNKNOWN) return "illegal or unknown instruction";
    if (info.flags & OPF_NOSUPPORT) return "external control (eciwx/ecowx) is not supported";
    switch (inst.op) {
    case Op::SC:
        if (inst.raw != 0x44000002u) return "sc with LEV != 0 (hypervisor call) or reserved bits";
        break;
    case Op::MFSPR:
        if (!supported_spr(inst.spr(), true)) return "mfspr of an unsupported SPR";
        break;
    case Op::MTSPR:
        if (!supported_spr(inst.spr(), false)) return "mtspr to an unsupported SPR";
        break;
    case Op::MFTB:
        if (inst.spr() != kSprTbl && inst.spr() != kSprTbu) return "mftb of an unsupported TBR";
        break;
    case Op::MFOCRF:
        if (std::popcount(inst.fxm()) != 1) return "mfocrf with FXM not selecting exactly one field";
        break;
    default: break;
    }
    return {};
}

bool is_supported(const Inst& inst) { return unsupported_reason(inst).empty(); }

u32 form_variant(const Inst& inst)
{
    const u32 f = inst.flags();
    u32 v = 0;
    if (f & OPF_RC) {
        const u32 rc = (f & OPF_VMX) ? inst.vrc() : inst.rc();
        if (rc) v |= kVarRc;
    }
    if ((f & OPF_OE) && inst.oe()) v |= kVarOe;
    if ((f & OPF_LK) && inst.lk()) v |= kVarLk;
    if ((f & OPF_AA) && inst.aa()) v |= kVarAa;
    return v;
}

std::string form_name(Op op, u32 variant)
{
    std::string s = op_info(op).mnemonic;
    if (variant & kVarOe) s += 'o';
    if (variant & kVarLk) s += 'l';
    if (variant & kVarAa) s += 'a';
    if (variant & kVarRc) s += '.';
    return s;
}

std::span<const u32> op_variants(Op op)
{
    const auto i = static_cast<usize>(op);
    if (i >= kOpCount) return {};
    return std::span<const u32>(kVariants.values[i].data(), kVariants.count[i]);
}

bool vmx_sets_sat(Op op)
{
    switch (op) {
    case Op::VADDUBS: case Op::VADDUHS: case Op::VADDUWS: case Op::VADDSBS: case Op::VADDSHS: case Op::VADDSWS:
    case Op::VSUBUBS: case Op::VSUBUHS: case Op::VSUBUWS: case Op::VSUBSBS: case Op::VSUBSHS: case Op::VSUBSWS:
    case Op::VMHADDSHS: case Op::VMHRADDSHS: case Op::VMSUMUHS: case Op::VMSUMSHS:
    case Op::VPKUHUS: case Op::VPKUWUS: case Op::VPKSHSS: case Op::VPKSHUS: case Op::VPKSWSS: case Op::VPKSWUS:
    case Op::VSUMSWS: case Op::VSUM2SWS: case Op::VSUM4SBS: case Op::VSUM4SHS: case Op::VSUM4UBS:
    case Op::VCTSXS: case Op::VCTUXS: return true;
    default: return false;
    }
}

bool vmx_uses_nj(Op op)
{
    switch (op) {
    case Op::VADDFP: case Op::VSUBFP: case Op::VMADDFP: case Op::VNMSUBFP: case Op::VMAXFP: case Op::VMINFP:
    case Op::VREFP: case Op::VRSQRTEFP: case Op::VRFIM: case Op::VRFIN: case Op::VRFIP: case Op::VRFIZ: return true;
    default: return false;
    }
}

u32 lswi_reg_mask(u32 rt, u32 nb)
{
    const u32 bytes = nb == 0 ? 32 : nb;
    const u32 regs = (bytes + 3) / 4;
    u32 mask = 0;
    for (u32 i = 0; i < regs; i++) mask |= 1u << ((rt + i) & 31);
    return mask;
}

RegUse reg_use(const Inst& inst)
{
    RegUse u;
    if (!is_supported(inst)) return u;

    const OpInfo& info = inst.info();
    const u32 f = info.flags;
    auto g = [](u32 r) { return 1u << r; };
    auto field = [](u32 crf) { return 0xFu << (4 * crf); };
    const u32 rt = inst.rt(), ra = inst.ra(), rb = inst.rb();
    const bool update = (f & OPF_UPDATE) != 0;
    // (rA|0): rA is read unless it is r0 in a non-update form.
    const u32 ra_or0 = (ra != 0 || update) ? g(ra) : 0;

    auto record_cr0 = [&] {
        u.cr_write |= field(0);
        u.spr_read |= SPR_XER_SO;
    };
    auto record_cr1 = [&] {
        u.cr_write |= field(1);
        u.spr_read |= SPR_FPSCR;
    };

    switch (info.fmt) {
    case Fmt::D_RT_RA_SI:
        if (inst.op == Op::ADDI || inst.op == Op::ADDIS)
            u.gpr_read |= ra != 0 ? g(ra) : 0;
        else
            u.gpr_read |= g(ra);
        u.gpr_write |= g(rt);
        if (inst.op == Op::ADDIC || inst.op == Op::ADDIC_ || inst.op == Op::SUBFIC) u.spr_write |= SPR_XER_CA;
        if (inst.op == Op::ADDIC_) record_cr0();
        break;
    case Fmt::D_RA_RS_UI:
        u.gpr_read |= g(rt);
        u.gpr_write |= g(ra);
        if (inst.op == Op::ANDI_ || inst.op == Op::ANDIS_) record_cr0();
        break;
    case Fmt::D_CMPI:
    case Fmt::D_CMPLI:
        u.gpr_read |= g(ra);
        u.cr_write |= field(inst.crfd());
        u.spr_read |= SPR_XER_SO;
        break;
    case Fmt::D_TO_RA_SI: u.gpr_read |= g(ra); break;
    case Fmt::D_LOAD:
        u.mem_read = true;
        u.gpr_read |= ra_or0;
        if (inst.op == Op::LMW) {
            for (u32 r = rt; r < 32; r++) u.gpr_write |= g(r);
        } else {
            u.gpr_write |= g(rt);
        }
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::D_STORE:
        u.mem_write = true;
        u.gpr_read |= ra_or0;
        if (inst.op == Op::STMW) {
            for (u32 r = rt; r < 32; r++) u.gpr_read |= g(r);
        } else {
            u.gpr_read |= g(rt);
        }
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::D_FLOAD:
    case Fmt::X_FLOAD:
        u.mem_read = true;
        u.gpr_read |= ra_or0 | (info.fmt == Fmt::X_FLOAD ? g(rb) : 0);
        u.fpr_write |= g(rt);
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::D_FSTORE:
    case Fmt::X_FSTORE:
        u.mem_write = true;
        u.gpr_read |= ra_or0 | (info.fmt == Fmt::X_FSTORE ? g(rb) : 0);
        u.fpr_read |= g(rt);
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::DS_LOAD:
        u.mem_read = true;
        u.gpr_read |= ra_or0;
        u.gpr_write |= g(rt);
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::DS_STORE:
        u.mem_write = true;
        u.gpr_read |= ra_or0 | g(rt);
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::I_BRANCH:
        if (inst.lk()) u.spr_write |= SPR_LR;
        break;
    case Fmt::B_BRANCH:
    case Fmt::XL_BRANCH: {
        const u32 bo = inst.bo();
        if (!(bo & 0x10)) u.cr_read |= g(inst.bi());
        const bool decrements = !(bo & 0x04) && inst.op != Op::BCCTR; // bcctr never decrements (as RPCS3)
        if (decrements) {
            u.spr_read |= SPR_CTR;
            u.spr_write |= SPR_CTR;
        }
        if (inst.op == Op::BCLR) u.spr_read |= SPR_LR;
        if (inst.op == Op::BCCTR) u.spr_read |= SPR_CTR;
        if (inst.lk()) u.spr_write |= SPR_LR;
        break;
    }
    case Fmt::SC: break; // a runtime call: the lifter spills and reloads everything around it
    case Fmt::XL_CRLOG:
        u.cr_read |= g(inst.crba()) | g(inst.crbb());
        u.cr_write |= g(inst.crbd());
        break;
    case Fmt::XL_MCRF:
        u.cr_read |= field(inst.crfs());
        u.cr_write |= field(inst.crfd());
        break;
    case Fmt::XL_NONE:
    case Fmt::X_NONE:
    case Fmt::X_SYNC:
    case Fmt::X_DST:
    case Fmt::X_DSS: break;
    case Fmt::XO_RT_RA_RB:
    case Fmt::XO_RT_RA:
        u.gpr_read |= g(ra) | (info.fmt == Fmt::XO_RT_RA_RB ? g(rb) : 0);
        u.gpr_write |= g(rt);
        switch (inst.op) {
        case Op::ADDC:
        case Op::SUBFC: u.spr_write |= SPR_XER_CA; break;
        case Op::ADDE:
        case Op::SUBFE:
        case Op::ADDZE:
        case Op::SUBFZE:
        case Op::ADDME:
        case Op::SUBFME:
            u.spr_read |= SPR_XER_CA;
            u.spr_write |= SPR_XER_CA;
            break;
        default: break;
        }
        if ((f & OPF_OE) && inst.oe()) {
            u.spr_read |= SPR_XER_SO;
            u.spr_write |= SPR_XER_OV | SPR_XER_SO;
        }
        if (inst.rc()) record_cr0();
        break;
    case Fmt::X_RA_RS_RB:
    case Fmt::X_RA_RS:
    case Fmt::X_RA_RS_SH:
    case Fmt::XS_RA_RS_SH:
        u.gpr_read |= g(rt) | (info.fmt == Fmt::X_RA_RS_RB ? g(rb) : 0);
        u.gpr_write |= g(ra);
        if (inst.op == Op::SRAW || inst.op == Op::SRAD || inst.op == Op::SRAWI || inst.op == Op::SRADI)
            u.spr_write |= SPR_XER_CA;
        if (inst.rc()) record_cr0();
        break;
    case Fmt::M_RA_RS_SH_MB_ME:
    case Fmt::M_RA_RS_RB_MB_ME:
    case Fmt::MD_RA_RS_SH_MB:
    case Fmt::MDS_RA_RS_RB_MB:
        u.gpr_read |= g(rt);
        if (info.fmt == Fmt::M_RA_RS_RB_MB_ME || info.fmt == Fmt::MDS_RA_RS_RB_MB) u.gpr_read |= g(rb);
        if (inst.op == Op::RLWIMI || inst.op == Op::RLDIMI) u.gpr_read |= g(ra);
        u.gpr_write |= g(ra);
        if (inst.rc()) record_cr0();
        break;
    case Fmt::X_CMP:
        u.gpr_read |= g(ra) | g(rb);
        u.cr_write |= field(inst.crfd());
        u.spr_read |= SPR_XER_SO;
        break;
    case Fmt::X_TO_RA_RB: u.gpr_read |= g(ra) | g(rb); break;
    case Fmt::X_LOAD:
        u.mem_read = true;
        u.gpr_read |= ra_or0 | g(rb);
        if (inst.op == Op::LSWX) {
            // Dynamic register range (XER[BC]): done by a runtime helper on the spilled context.
            u.spr_read |= SPR_XER_BC;
            u.gpr_read |= 0xFFFFFFFFu;
            u.gpr_write |= 0xFFFFFFFFu;
        } else {
            u.gpr_write |= g(rt);
        }
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::X_STORE:
        u.mem_write = true;
        u.gpr_read |= ra_or0 | g(rb);
        if (inst.op == Op::STSWX) {
            u.spr_read |= SPR_XER_BC;
            u.gpr_read |= 0xFFFFFFFFu;
        } else {
            u.gpr_read |= g(rt);
        }
        if (inst.op == Op::STWCX_ || inst.op == Op::STDCX_) record_cr0();
        if (update) u.gpr_write |= g(ra);
        break;
    case Fmt::X_LSWI:
        u.gpr_read |= ra != 0 ? g(ra) : 0;
        if (inst.op == Op::LSWI) {
            u.mem_read = true;
            u.gpr_write |= lswi_reg_mask(rt, inst.nb());
        } else {
            u.mem_write = true;
            u.gpr_read |= lswi_reg_mask(rt, inst.nb());
        }
        break;
    case Fmt::X_RA_RB:
        if (inst.op == Op::DCBZ) {
            u.mem_write = true;
            u.gpr_read |= (ra != 0 ? g(ra) : 0) | g(rb);
        }
        break;
    case Fmt::XFX_MFSPR:
        u.gpr_write |= g(rt);
        switch (inst.spr()) {
        case kSprXer: u.spr_read |= SPR_XER_SO | SPR_XER_OV | SPR_XER_CA | SPR_XER_BC; break;
        case kSprLr: u.spr_read |= SPR_LR; break;
        case kSprCtr: u.spr_read |= SPR_CTR; break;
        case kSprVrsave: u.spr_read |= SPR_VRSAVE; break;
        default: break;
        }
        break;
    case Fmt::XFX_MFTB: u.gpr_write |= g(rt); break;
    case Fmt::XFX_MTSPR:
        u.gpr_read |= g(rt);
        switch (inst.spr()) {
        case kSprXer: u.spr_write |= SPR_XER_SO | SPR_XER_OV | SPR_XER_CA | SPR_XER_BC; break;
        case kSprLr: u.spr_write |= SPR_LR; break;
        case kSprCtr: u.spr_write |= SPR_CTR; break;
        case kSprVrsave: u.spr_write |= SPR_VRSAVE; break;
        default: break;
        }
        break;
    case Fmt::X_MFCR:
        u.gpr_write |= g(rt);
        u.cr_read = 0xFFFFFFFFu;
        break;
    case Fmt::X_MFOCRF:
        u.gpr_write |= g(rt);
        u.cr_read |= field(static_cast<u32>(std::countl_zero(inst.fxm()) - 24) & 7);
        break;
    case Fmt::XFX_MTCRF:
        u.gpr_read |= g(rt);
        if (inst.op == Op::MTOCRF) {
            u.cr_write |= field(static_cast<u32>(std::countl_zero(inst.fxm())) & 7);
        } else {
            for (u32 i = 0; i < 8; i++)
                if (inst.fxm() & (0x80u >> i)) u.cr_write |= field(i);
        }
        break;
    case Fmt::A_FRT_FRA_FRB:
        u.fpr_read |= g(ra) | g(rb);
        u.fpr_write |= g(rt);
        if (inst.rc()) record_cr1();
        break;
    case Fmt::A_FRT_FRA_FRC:
        u.fpr_read |= g(ra) | g(inst.rc_reg());
        u.fpr_write |= g(rt);
        if (inst.rc()) record_cr1();
        break;
    case Fmt::A_FRT_FRA_FRC_FRB:
        u.fpr_read |= g(ra) | g(rb) | g(inst.rc_reg());
        u.fpr_write |= g(rt);
        if (inst.rc()) record_cr1();
        break;
    case Fmt::A_FRT_FRB:
    case Fmt::X_FRT_FRB:
        u.fpr_read |= g(rb);
        u.fpr_write |= g(rt);
        if (inst.op == Op::FCTIW || inst.op == Op::FCTID) u.spr_read |= SPR_FPSCR; // rounding mode
        if (inst.rc()) record_cr1();
        break;
    case Fmt::X_FCMP:
        u.fpr_read |= g(ra) | g(rb);
        u.cr_write |= field(inst.crfd());
        u.spr_read |= SPR_FPSCR;
        u.spr_write |= SPR_FPSCR;
        break;
    case Fmt::X_MFFS:
        u.fpr_write |= g(rt);
        u.spr_read |= SPR_FPSCR;
        if (inst.rc()) record_cr1();
        break;
    case Fmt::XFL_MTFSF:
    case Fmt::X_MTFSFI:
    case Fmt::X_MTFSB:
        if (info.fmt == Fmt::XFL_MTFSF) u.fpr_read |= g(rb);
        u.spr_read |= SPR_FPSCR;
        u.spr_write |= SPR_FPSCR;
        if (inst.rc()) record_cr1();
        break;
    case Fmt::X_MCRFS:
        u.cr_write |= field(inst.crfd());
        u.spr_read |= SPR_FPSCR;
        u.spr_write |= SPR_FPSCR;
        break;
    // ---- VMX (vD = rt, vA = ra, vB = rb, vC = rc_reg; loads/stores use rA/rB as GPRs)
    case Fmt::VX_VD_VA_VB:
    case Fmt::VA_VSLDOI:
        u.vr_read |= g(ra) | g(rb);
        u.vr_write |= g(rt);
        break;
    case Fmt::VXR_VD_VA_VB:
        u.vr_read |= g(ra) | g(rb);
        u.vr_write |= g(rt);
        if (inst.vrc()) u.cr_write |= field(6);
        break;
    case Fmt::VX_VD_VB:
    case Fmt::VX_VD_VB_UIMM:
        u.vr_read |= g(rb);
        u.vr_write |= g(rt);
        break;
    case Fmt::VX_VD_SIMM: u.vr_write |= g(rt); break;
    case Fmt::VA_VD_VA_VB_VC:
        u.vr_read |= g(ra) | g(rb) | g(inst.rc_reg());
        u.vr_write |= g(rt);
        break;
    case Fmt::VX_VD:
        u.vr_write |= g(rt);
        u.spr_read |= SPR_VSCR_SAT | SPR_VSCR_NJ;
        break;
    case Fmt::VX_VB:
        u.vr_read |= g(rb);
        u.spr_write |= SPR_VSCR_SAT | SPR_VSCR_NJ;
        break;
    case Fmt::X_VLOAD:
        u.gpr_read |= (ra != 0 ? g(ra) : 0) | g(rb);
        u.vr_write |= g(rt);
        u.mem_read = inst.op != Op::LVSL && inst.op != Op::LVSR;
        break;
    case Fmt::X_VSTORE:
        u.gpr_read |= (ra != 0 ? g(ra) : 0) | g(rb);
        u.vr_read |= g(rt);
        u.mem_write = true;
        break;
    default: break;
    }
    if (vmx_sets_sat(inst.op)) {
        u.spr_read |= SPR_VSCR_SAT;
        u.spr_write |= SPR_VSCR_SAT;
    }
    if (vmx_uses_nj(inst.op)) u.spr_read |= SPR_VSCR_NJ;
    return u;
}

} // namespace pspocket::ppu
