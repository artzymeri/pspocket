// PPU disassembler. Mnemonic choices follow GNU binutils (objdump -M ppc64) and RPCS3's
// rpcs3/Emu/Cell/PPUDisAsm.cpp for the extended forms.

#include "pspocket/ppu/disasm.hpp"

#include <bit>
#include <format>

namespace pspocket::ppu {
namespace {

std::string r(u32 n) { return std::format("r{}", n); }
std::string fr(u32 n) { return std::format("f{}", n); }
std::string vr(u32 n) { return std::format("v{}", n); }
std::string crf(u32 n) { return std::format("cr{}", n); }

std::string hex(u32 v) { return std::format("0x{:x}", v); }

// "crN," prefix, omitted for cr0 (GNU style).
std::string crf_prefix(u32 n) { return n == 0 ? std::string{} : crf(n) + ","; }

std::string suffixes(const Inst& inst, std::string base)
{
    const u32 f = inst.flags();
    if ((f & OPF_OE) && inst.oe()) base += 'o';
    if (f & OPF_RC) {
        const u32 rc = (f & OPF_VMX) ? inst.vrc() : inst.rc();
        if (rc) base += '.';
    }
    return base;
}

std::string with(std::string mnemonic, const std::string& operands)
{
    if (operands.empty()) return mnemonic;
    return mnemonic + " " + operands;
}

const char* cond_name(u32 bit, bool if_true)
{
    static const char* const t[4] = {"lt", "gt", "eq", "so"};
    static const char* const f[4] = {"ge", "le", "ne", "ns"};
    return if_true ? t[bit & 3] : f[bit & 3];
}

// Simplified conditional-branch mnemonic for (BO, BI) or empty when there is none. `kind` is "" (bc), "lr" or "ctr".
// Writes the CR operand prefix ("crN," or "") into cr_operand.
std::string bc_mnemonic(u32 bo, u32 bi, const char* kind, bool lk, bool aa, std::string& cr_operand)
{
    const u32 b = bo & 0x1E; // ignore the y/at hint bit
    std::string m;
    cr_operand.clear();
    if ((b & 0x14) == 0x14) {
        m = "b"; // branch always
    } else if (b == 0x04 || b == 0x06 || b == 0x0C || b == 0x0E) {
        const bool if_true = (b & 0x08) != 0;
        m = std::string("b") + cond_name(bi, if_true);
        cr_operand = crf_prefix(bi / 4);
    } else if (b == 0x10 || b == 0x18) {
        m = "bdnz";
    } else if (b == 0x12 || b == 0x1A) {
        m = "bdz";
    } else if (b == 0x00 || b == 0x02 || b == 0x08 || b == 0x0A) {
        m = std::string(b & 0x02 ? "bdz" : "bdnz") + (b & 0x08 ? "t" : "f");
        cr_operand = std::format("{},", bi);
    } else {
        return {};
    }
    m += kind;
    if (lk) m += 'l';
    if (aa) m += 'a';
    return m;
}

std::string spr_name(u32 spr)
{
    switch (spr) {
    case kSprXer: return "xer";
    case kSprLr: return "lr";
    case kSprCtr: return "ctr";
    case kSprVrsave: return "vrsave";
    default: return {};
    }
}

} // namespace

std::string disassemble(u32 raw, u32 pc) { return disassemble(decode(raw), pc); }

std::string disassemble(const Inst& in, u32 pc)
{
    if (in.op == Op::UNKNOWN) return std::format(".long 0x{:08x}", in.raw);

    const OpInfo& info = in.info();
    const std::string mn = info.mnemonic;
    const u32 rt = in.rt(), ra = in.ra(), rb = in.rb();

    switch (info.fmt) {
    case Fmt::D_RT_RA_SI:
        if (in.op == Op::ADDI && ra == 0) return with("li", std::format("{},{}", r(rt), in.simm()));
        if (in.op == Op::ADDIS && ra == 0) return with("lis", std::format("{},{}", r(rt), in.simm()));
        return with(mn, std::format("{},{},{}", r(rt), r(ra), in.simm()));
    case Fmt::D_RA_RS_UI:
        if (in.op == Op::ORI && rt == 0 && ra == 0 && in.uimm() == 0) return "nop";
        return with(mn, std::format("{},{},{}", r(ra), r(rt), hex(in.uimm())));
    case Fmt::D_CMPI:
        return with(in.l10() ? "cmpdi" : "cmpwi", std::format("{}{},{}", crf_prefix(in.crfd()), r(ra), in.simm()));
    case Fmt::D_CMPLI:
        return with(in.l10() ? "cmpldi" : "cmplwi", std::format("{}{},{}", crf_prefix(in.crfd()), r(ra), in.uimm()));
    case Fmt::D_TO_RA_SI: return with(mn, std::format("{},{},{}", rt, r(ra), in.simm()));
    case Fmt::D_LOAD:
    case Fmt::D_STORE: return with(mn, std::format("{},{}({})", r(rt), in.simm(), r(ra)));
    case Fmt::D_FLOAD:
    case Fmt::D_FSTORE: return with(mn, std::format("{},{}({})", fr(rt), in.simm(), r(ra)));
    case Fmt::DS_LOAD:
    case Fmt::DS_STORE: return with(mn, std::format("{},{}({})", r(rt), in.ds(), r(ra)));

    case Fmt::I_BRANCH: {
        std::string m = "b";
        if (in.lk()) m += 'l';
        if (in.aa()) m += 'a';
        return with(m, hex(branch_target(in, pc)));
    }
    case Fmt::B_BRANCH: {
        std::string cr;
        const std::string m = bc_mnemonic(in.bo(), in.bi(), "", in.lk(), in.aa(), cr);
        const std::string target = hex(branch_target(in, pc));
        // "Branch always" as a bc keeps the raw form so it is not confused with the I-form b.
        if (!m.empty() && m != "b" && m != "bl" && m != "ba" && m != "bla") return with(m, cr + target);
        std::string raw_m = "bc";
        if (in.lk()) raw_m += 'l';
        if (in.aa()) raw_m += 'a';
        return with(raw_m, std::format("{},{},{}", in.bo(), in.bi(), target));
    }
    case Fmt::XL_BRANCH: {
        const bool is_lr = in.op == Op::BCLR;
        const char* kind = is_lr ? "lr" : "ctr";
        if ((in.bo() & 0x14) == 0x14) return std::string("b") + kind + (in.lk() ? "l" : "");
        std::string cr;
        const std::string m = bc_mnemonic(in.bo(), in.bi(), kind, in.lk(), false, cr);
        if (!m.empty() && !(in.op == Op::BCCTR && m.starts_with("bd"))) {
            std::string ops = cr;
            if (!ops.empty() && ops.back() == ',') ops.pop_back();
            return with(m, ops);
        }
        return with(suffixes(in, std::string(mn) + (in.lk() ? "l" : "")), std::format("{},{},{}", in.bo(), in.bi(), in.bh()));
    }
    case Fmt::SC: return in.lev() ? std::format("sc {}", in.lev()) : "sc";
    case Fmt::XL_CRLOG: {
        const u32 d = in.crbd(), a = in.crba(), b = in.crbb();
        if (in.op == Op::CRXOR && a == b && a == d) return with("crclr", std::format("{}", d));
        if (in.op == Op::CREQV && a == b && a == d) return with("crset", std::format("{}", d));
        if (in.op == Op::CROR && a == b) return with("crmove", std::format("{},{}", d, a));
        if (in.op == Op::CRNOR && a == b) return with("crnot", std::format("{},{}", d, a));
        return with(mn, std::format("{},{},{}", d, a, b));
    }
    case Fmt::XL_MCRF: return with(mn, std::format("{},{}", crf(in.crfd()), crf(in.crfs())));
    case Fmt::XL_NONE:
    case Fmt::X_NONE: return mn;

    case Fmt::XO_RT_RA_RB: return with(suffixes(in, mn), std::format("{},{},{}", r(rt), r(ra), r(rb)));
    case Fmt::XO_RT_RA: return with(suffixes(in, mn), std::format("{},{}", r(rt), r(ra)));
    case Fmt::X_RA_RS_RB:
        if (in.op == Op::OR && rt == rb) return with(suffixes(in, "mr"), std::format("{},{}", r(ra), r(rt)));
        if (in.op == Op::NOR && rt == rb) return with(suffixes(in, "not"), std::format("{},{}", r(ra), r(rt)));
        return with(suffixes(in, mn), std::format("{},{},{}", r(ra), r(rt), r(rb)));
    case Fmt::X_RA_RS: return with(suffixes(in, mn), std::format("{},{}", r(ra), r(rt)));
    case Fmt::X_RA_RS_SH: return with(suffixes(in, mn), std::format("{},{},{}", r(ra), r(rt), in.sh32()));
    case Fmt::XS_RA_RS_SH: return with(suffixes(in, mn), std::format("{},{},{}", r(ra), r(rt), in.sh64()));
    case Fmt::M_RA_RS_SH_MB_ME: {
        const u32 sh = in.sh32(), mb = in.mb32(), me = in.me32();
        if (in.op == Op::RLWINM) {
            if (mb == 0 && me == 31 - sh) return with(suffixes(in, "slwi"), std::format("{},{},{}", r(ra), r(rt), sh));
            if (me == 31 && sh != 0 && sh == 32 - mb)
                return with(suffixes(in, "srwi"), std::format("{},{},{}", r(ra), r(rt), mb));
            if (sh == 0 && me == 31) return with(suffixes(in, "clrlwi"), std::format("{},{},{}", r(ra), r(rt), mb));
            if (mb == 0 && me == 31) return with(suffixes(in, "rotlwi"), std::format("{},{},{}", r(ra), r(rt), sh));
            if (sh == 0 && mb == 0) return with(suffixes(in, "clrrwi"), std::format("{},{},{}", r(ra), r(rt), 31 - me));
        }
        return with(suffixes(in, mn), std::format("{},{},{},{},{}", r(ra), r(rt), sh, mb, me));
    }
    case Fmt::M_RA_RS_RB_MB_ME:
        if (in.mb32() == 0 && in.me32() == 31) return with(suffixes(in, "rotlw"), std::format("{},{},{}", r(ra), r(rt), r(rb)));
        return with(suffixes(in, mn), std::format("{},{},{},{},{}", r(ra), r(rt), r(rb), in.mb32(), in.me32()));
    case Fmt::MD_RA_RS_SH_MB: {
        const u32 sh = in.sh64(), mb = in.mb64();
        if (in.op == Op::RLDICL) {
            if (sh == 0) return with(suffixes(in, "clrldi"), std::format("{},{},{}", r(ra), r(rt), mb));
            if (mb == 0) return with(suffixes(in, "rotldi"), std::format("{},{},{}", r(ra), r(rt), sh));
            if (mb == 64 - sh) return with(suffixes(in, "srdi"), std::format("{},{},{}", r(ra), r(rt), mb));
        } else if (in.op == Op::RLDICR) {
            if (mb == 63 - sh) return with(suffixes(in, "sldi"), std::format("{},{},{}", r(ra), r(rt), sh));
            if (sh == 0) return with(suffixes(in, "clrrdi"), std::format("{},{},{}", r(ra), r(rt), 63 - mb));
        }
        return with(suffixes(in, mn), std::format("{},{},{},{}", r(ra), r(rt), sh, mb));
    }
    case Fmt::MDS_RA_RS_RB_MB:
        if (in.op == Op::RLDCL && in.mb64() == 0) return with(suffixes(in, "rotld"), std::format("{},{},{}", r(ra), r(rt), r(rb)));
        return with(suffixes(in, mn), std::format("{},{},{},{}", r(ra), r(rt), r(rb), in.mb64()));
    case Fmt::X_CMP: {
        const char* m = in.op == Op::CMP ? (in.l10() ? "cmpd" : "cmpw") : (in.l10() ? "cmpld" : "cmplw");
        return with(m, std::format("{}{},{}", crf_prefix(in.crfd()), r(ra), r(rb)));
    }
    case Fmt::X_TO_RA_RB:
        if (in.op == Op::TW && rt == 31 && ra == 0 && rb == 0) return "trap";
        return with(mn, std::format("{},{},{}", rt, r(ra), r(rb)));
    case Fmt::X_LOAD:
    case Fmt::X_STORE: return with(mn, std::format("{},{},{}", r(rt), r(ra), r(rb)));
    case Fmt::X_FLOAD:
    case Fmt::X_FSTORE: return with(mn, std::format("{},{},{}", fr(rt), r(ra), r(rb)));
    case Fmt::X_LSWI: return with(mn, std::format("{},{},{}", r(rt), r(ra), in.nb()));
    case Fmt::X_RA_RB: return with(mn, std::format("{},{}", r(ra), r(rb)));
    case Fmt::X_DST: return with(mn + (in.dst_t() ? "t" : ""), std::format("{},{},{}", r(ra), r(rb), in.strm()));
    case Fmt::X_DSS: return in.dst_t() ? "dssall" : with(mn, std::format("{}", in.strm()));
    case Fmt::X_SYNC:
        switch (in.sync_l()) {
        case 0: return "sync";
        case 1: return "lwsync";
        case 2: return "ptesync";
        default: return std::format("sync {}", in.sync_l());
        }
    case Fmt::XFX_MFSPR: {
        const u32 spr = in.spr();
        if (spr == kSprTbl) return with("mftb", r(rt));
        if (spr == kSprTbu) return with("mftbu", r(rt));
        const std::string name = spr_name(spr);
        if (!name.empty()) return with("mf" + name, r(rt));
        return with(mn, std::format("{},{}", r(rt), spr));
    }
    case Fmt::XFX_MFTB:
        if (in.spr() == kSprTbu) return with("mftbu", r(rt));
        if (in.spr() == kSprTbl) return with("mftb", r(rt));
        return with(mn, std::format("{},{}", r(rt), in.spr()));
    case Fmt::XFX_MTSPR: {
        const std::string name = spr_name(in.spr());
        if (!name.empty()) return with("mt" + name, r(rt));
        return with(mn, std::format("{},{}", in.spr(), r(rt)));
    }
    case Fmt::X_MFCR: return with(mn, r(rt));
    case Fmt::X_MFOCRF: {
        const u32 fxm = in.fxm();
        if (std::popcount(fxm) == 1) return with(mn, std::format("{},{}", r(rt), crf(static_cast<u32>(std::countl_zero(fxm) - 24))));
        return with(mn, std::format("{},{}", r(rt), hex(fxm)));
    }
    case Fmt::XFX_MTCRF:
        if (in.op == Op::MTCRF && in.fxm() == 0xFF) return with("mtcr", r(rt));
        if (in.op == Op::MTOCRF && std::popcount(in.fxm()) == 1)
            return with(mn, std::format("{},{}", crf(static_cast<u32>(std::countl_zero(in.fxm()) - 24)), r(rt)));
        return with(mn, std::format("{},{}", hex(in.fxm()), r(rt)));

    case Fmt::A_FRT_FRA_FRB: return with(suffixes(in, mn), std::format("{},{},{}", fr(rt), fr(ra), fr(rb)));
    case Fmt::A_FRT_FRA_FRC: return with(suffixes(in, mn), std::format("{},{},{}", fr(rt), fr(ra), fr(in.rc_reg())));
    case Fmt::A_FRT_FRA_FRC_FRB:
        return with(suffixes(in, mn), std::format("{},{},{},{}", fr(rt), fr(ra), fr(in.rc_reg()), fr(rb)));
    case Fmt::A_FRT_FRB:
    case Fmt::X_FRT_FRB: return with(suffixes(in, mn), std::format("{},{}", fr(rt), fr(rb)));
    case Fmt::X_FCMP: return with(mn, std::format("{},{},{}", crf(in.crfd()), fr(ra), fr(rb)));
    case Fmt::X_MFFS: return with(suffixes(in, mn), fr(rt));
    case Fmt::XFL_MTFSF: return with(suffixes(in, mn), std::format("{},{}", hex(in.flm()), fr(rb)));
    case Fmt::X_MTFSFI: return with(suffixes(in, mn), std::format("{},{}", in.crfd(), in.fpimm()));
    case Fmt::X_MTFSB: return with(suffixes(in, mn), std::format("{}", in.crbd()));
    case Fmt::X_MCRFS: return with(mn, std::format("{},{}", crf(in.crfd()), in.crfs()));

    case Fmt::VX_VD_VA_VB: return with(mn, std::format("{},{},{}", vr(rt), vr(ra), vr(rb)));
    case Fmt::VX_VD_VB: return with(mn, std::format("{},{}", vr(rt), vr(rb)));
    case Fmt::VX_VD_VB_UIMM: return with(mn, std::format("{},{},{}", vr(rt), vr(rb), in.vuimm()));
    case Fmt::VX_VD_SIMM: return with(mn, std::format("{},{}", vr(rt), in.vsimm()));
    case Fmt::VA_VD_VA_VB_VC:
        if (in.op == Op::VMADDFP || in.op == Op::VNMSUBFP) // operand order vD,vA,vC,vB
            return with(mn, std::format("{},{},{},{}", vr(rt), vr(ra), vr(in.rc_reg()), vr(rb)));
        return with(mn, std::format("{},{},{},{}", vr(rt), vr(ra), vr(rb), vr(in.rc_reg())));
    case Fmt::VA_VSLDOI: return with(mn, std::format("{},{},{},{}", vr(rt), vr(ra), vr(rb), in.vsh()));
    case Fmt::VXR_VD_VA_VB: return with(suffixes(in, mn), std::format("{},{},{}", vr(rt), vr(ra), vr(rb)));
    case Fmt::VX_VD: return with(mn, vr(rt));
    case Fmt::VX_VB: return with(mn, vr(rb));
    case Fmt::X_VLOAD:
    case Fmt::X_VSTORE: return with(mn, std::format("{},{},{}", vr(rt), r(ra), r(rb)));
    case Fmt::NONE: break;
    }
    return std::format(".long 0x{:08x}", in.raw);
}

} // namespace pspocket::ppu
