// SPU lifter (pspocket/recomp/spu/lifter.hpp). Every instruction is lowered to calls of the helpers in
// runtime/spu/include/pspocket/spu/lifted.h; control flow and runtime calls are generated here.

#include "pspocket/recomp/spu/lifter.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>

#include "pspocket/spu/context.h"
#include "pspocket/spu/decoder.hpp"
#include "pspocket/spu/disasm.hpp"

namespace pspocket::recomp::spu {

using namespace ::pspocket::spu;

namespace {

std::string R(u32 n) { return std::format("r{}", n); }
std::string P(u32 n) { return std::format("psl_u32p(r{})", n); }
std::string hex32(u32 v) { return std::format("0x{:X}u", v); }

// How control leaves an instruction. Full mode jumps to labels of the current function and calls lifted functions;
// step mode (fuzz) always stores the target in ctx->pc and returns.
//
// Relocatable code (D-081) runs ctx->reloc bytes past where it was lifted: every LS address it derives from its own
// (the pc it stores, links, relative branch and load targets, the pc it passes to the runtime) adds the local rel,
// and absolute targets (bra, brasl) are the same wherever it sits, so they leave to the run loop.
class Flow {
public:
    explicit Flow(bool relocatable) : relocatable_(relocatable) {}
    virtual ~Flow() = default;
    // A constant target; backward = loop back edge (polls ctx->attention).
    virtual std::string jump(u32 target, u32 from) const = 0;
    // ctx->pc is already set to a computed target.
    virtual std::string jump_dynamic() const = 0;
    // Direct call: link already written.
    virtual std::string call(u32 target, u32 ret) const = 0;
    // Indirect call, ctx->pc already set to the target.
    virtual std::string call_dynamic(u32 ret) const = 0;

    // bra and brasl.
    std::string jump_absolute(u32 target, u32 from) const { return relocatable_ ? leave_absolute(target) : jump(target, from); }
    std::string call_absolute(u32 target, u32 ret) const { return relocatable_ ? leave_absolute(target) : call(target, ret); }

    // An LS address the code derives from its own address (a pc, a link): as is, or relocated.
    std::string pc(u32 addr) const
    {
        return relocatable_ ? std::format("((0x{:05X}u + rel) & 0x3FFFCu)", addr) : std::format("0x{:05X}u", addr);
    }
    // The address of a relative load or store (psl_lq and psl_stq align it).
    std::string ls_addr(u32 addr) const
    {
        return relocatable_ ? std::format("0x{:05X}u + rel", addr & PS_SPU_LS_MASK) : std::format("0x{:05X}u", addr & 0x3FFF0u);
    }
    bool relocatable() const { return relocatable_; }

protected:
    std::string leave(u32 target) const { return std::format("    ctx->pc = {};\n    goto exit_spill;\n", pc(target)); }

private:
    static std::string leave_absolute(u32 target) { return std::format("    ctx->pc = 0x{:05X}u;\n    goto exit_spill;\n", target); }

    bool relocatable_;
};

// The unaligned target of lqr/stqr at a (ls_target aligns it; a relocated address is aligned after adding rel).
u32 relative_ls_target(const Inst& inst, u32 a) { return (a + (inst.i16() << 2)) & PS_SPU_LS_MASK; }

// Lowers one instruction at address a. Returns true when the instruction can fall through to a + 4.
bool lower(std::string& o, const Inst& inst, u32 a, const Flow& flow, u8 xfloat, bool spill_around_stop)
{
    const u32 rt = inst.rt(), ra = inst.ra(), rb = inst.rb(), rc = inst.rc(), rt4 = inst.rt4();
    const u32 next = a + 4;
    const u32 ret = next & PS_SPU_PC_MASK;
    const bool acc = xfloat == PS_SPU_XFLOAT_ACCURATE;
    const auto set = [&](const std::string& expr) { o += std::format("    {} = {};\n", R(rt), expr); };
    const auto bin = [&](const char* fn) { set(std::format("{}({}, {})", fn, R(ra), R(rb))); };
    const auto un = [&](const char* fn) { set(std::format("{}({})", fn, R(ra))); };
    const auto tri = [&](const char* fn) { set(std::format("{}({}, {}, {})", fn, R(ra), R(rb), R(rt))); };
    const auto quad = [&](const char* fn) {
        o += std::format("    {} = {}({}, {}, {});\n", R(rt4), fn, R(ra), R(rb), R(rc));
    };
    const auto imm32 = [&](const char* fn, u32 v) { set(std::format("{}({}, psl_splat32({}))", fn, R(ra), hex32(v))); };
    const auto imm16 = [&](const char* fn, u32 v) {
        set(std::format("{}({}, psl_splat16(0x{:X}))", fn, R(ra), v & 0xFFFF));
    };
    const auto imm8 = [&](const char* fn, u32 v) { set(std::format("{}({}, psl_splat8(0x{:X}))", fn, R(ra), v & 0xFF)); };
    const auto interrupts = [&]() -> std::string {
        if (!inst.e() && !inst.d()) return {};
        return std::format("    if (ps_spu_set_interrupts(ctx, {})) goto exit_spill;\n", inst.e() ? 1 : 0);
    };
    const u32 si10 = static_cast<u32>(inst.si10());
    const u32 i7 = inst.i7();

    switch (inst.op) {
    // ---- control ----
    case Op::LNOP:
    case Op::NOP:
    case Op::HBR:
    case Op::HBRA:
    case Op::HBRR:
    case Op::MTSPR: return true;
    case Op::SYNC:
    case Op::DSYNC: o += "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"; return true;
    case Op::STOP:
    case Op::STOPD: {
        const u32 code = inst.op == Op::STOP ? inst.stop_signal() : 0x3FFF;
        if (spill_around_stop) {
            o += std::format("    PSL_SPILL();\n    if (ps_spu_stop(ctx, {}, 0x{:X}u)) goto exit_now;\n    PSL_RELOAD();\n", flow.pc(a), code);
        } else {
            o += std::format("    if (ps_spu_stop(ctx, {}, 0x{:X}u)) goto exit_spill;\n", flow.pc(a), code);
        }
        return true;
    }
    case Op::MFSPR: set("psl_splat32(0u)"); return true;
    case Op::RDCH:
    case Op::RCHCNT:
        o += std::format("    {{\n        uint32_t v;\n        if ({}(ctx, {}, {}u, &v)) goto exit_spill;\n        {} = psl_pref(v);\n    }}\n",
            inst.op == Op::RDCH ? "ps_spu_rdch" : "ps_spu_rchcnt", flow.pc(a), inst.channel(), R(rt));
        return true;
    case Op::WRCH:
        o += std::format("    if (ps_spu_wrch(ctx, {}, {}u, {})) goto exit_spill;\n", flow.pc(a), inst.channel(), P(rt));
        return true;

    // ---- halts ----
    case Op::HEQ:
    case Op::HGT:
    case Op::HLGT:
    case Op::HEQI:
    case Op::HGTI:
    case Op::HLGTI: {
        std::string cond;
        switch (inst.op) {
        case Op::HEQ: cond = std::format("(int32_t){} == (int32_t){}", P(ra), P(rb)); break;
        case Op::HGT: cond = std::format("(int32_t){} > (int32_t){}", P(ra), P(rb)); break;
        case Op::HLGT: cond = std::format("{} > {}", P(ra), P(rb)); break;
        case Op::HEQI: cond = std::format("(int32_t){} == {}", P(ra), inst.si10()); break;
        case Op::HGTI: cond = std::format("(int32_t){} > {}", P(ra), inst.si10()); break;
        default: cond = std::format("{} > {}", P(ra), hex32(si10)); break;
        }
        o += std::format("    if ({}) {{\n        ps_spu_halt(ctx, {});\n        goto exit_spill;\n    }}\n", cond, flow.pc(a));
        return true;
    }

    // ---- loads and stores ----
    case Op::LQD: set(std::format("psl_lq(ls, {} + {})", P(ra), hex32(si10 * 16))); return true;
    case Op::LQX: set(std::format("psl_lq(ls, {} + {})", P(ra), P(rb))); return true;
    case Op::LQA: set(std::format("psl_lq(ls, 0x{:05X}u)", ls_target(inst, a))); return true;
    case Op::LQR: set(std::format("psl_lq(ls, {})", flow.ls_addr(relative_ls_target(inst, a)))); return true;
    case Op::STQD: o += std::format("    psl_stq(ls, {} + {}, {});\n", P(ra), hex32(si10 * 16), R(rt)); return true;
    case Op::STQX: o += std::format("    psl_stq(ls, {} + {}, {});\n", P(ra), P(rb), R(rt)); return true;
    case Op::STQA: o += std::format("    psl_stq(ls, 0x{:05X}u, {});\n", ls_target(inst, a), R(rt)); return true;
    case Op::STQR: o += std::format("    psl_stq(ls, {}, {});\n", flow.ls_addr(relative_ls_target(inst, a)), R(rt)); return true;

    // ---- branches ----
    case Op::BR: o += flow.jump(branch_target(inst, a), a); return false;
    case Op::BRA: o += flow.jump_absolute(branch_target(inst, a), a); return false;
    case Op::BRZ:
    case Op::BRNZ:
    case Op::BRHZ:
    case Op::BRHNZ: {
        const char* cond = inst.op == Op::BRZ ? "psl_u32p({}) == 0" : inst.op == Op::BRNZ ? "psl_u32p({}) != 0"
            : inst.op == Op::BRHZ                                    ? "psl_u16p({}) == 0"
                                                                     : "psl_u16p({}) != 0";
        const std::string reg = R(rt);
        o += std::format("    if ({}) {{\n", std::vformat(cond, std::make_format_args(reg)));
        o += flow.jump(branch_target(inst, a), a);
        o += "    }\n";
        return true;
    }
    case Op::BRSL:
        set(std::format("psl_pref({})", flow.pc(ret)));
        o += flow.call(branch_target(inst, a), ret);
        return false;
    case Op::BRASL:
        set(std::format("psl_pref({})", flow.pc(ret)));
        o += flow.call_absolute(branch_target(inst, a), ret);
        return false;
    case Op::BI:
        o += std::format("    ctx->pc = {} & 0x3FFFCu;\n", P(ra));
        o += interrupts();
        o += flow.jump_dynamic();
        return false;
    case Op::IRET:
        o += "    ctx->pc = ctx->srr0 & 0x3FFFCu;\n";
        o += interrupts();
        o += flow.jump_dynamic();
        return false;
    case Op::BISL:
        o += std::format("    ctx->pc = {} & 0x3FFFCu;\n", P(ra)); // target read before the link is written
        set(std::format("psl_pref({})", flow.pc(ret)));
        o += interrupts();
        o += flow.call_dynamic(ret);
        return false;
    case Op::BISLED:
        o += std::format("    {{\n        const uint32_t target = {} & 0x3FFFCu;\n", P(ra));
        o += std::format("        {} = psl_pref({});\n", R(rt), flow.pc(ret));
        o += "        if (ps_spu_events_pending(ctx)) {\n            ctx->pc = target;\n";
        o += interrupts();
        o += flow.jump_dynamic();
        o += "        }\n    }\n";
        return true;
    case Op::BIZ:
    case Op::BINZ:
    case Op::BIHZ:
    case Op::BIHNZ: {
        const char* cond = inst.op == Op::BIZ ? "psl_u32p({}) == 0" : inst.op == Op::BINZ ? "psl_u32p({}) != 0"
            : inst.op == Op::BIHZ                                    ? "psl_u16p({}) == 0"
                                                                     : "psl_u16p({}) != 0";
        const std::string reg = R(rt);
        o += std::format("    if ({}) {{\n", std::vformat(cond, std::make_format_args(reg)));
        o += std::format("        ctx->pc = {} & 0x3FFFCu;\n", P(ra));
        o += interrupts();
        o += flow.jump_dynamic();
        o += "    }\n";
        return true;
    }

    // ---- constant formation ----
    case Op::IL: set(std::format("psl_splat32({})", hex32(static_cast<u32>(inst.si16())))); return true;
    case Op::ILHU: set(std::format("psl_splat32({})", hex32(inst.i16() << 16))); return true;
    case Op::ILH: set(std::format("psl_splat16(0x{:X})", inst.i16())); return true;
    case Op::ILA: set(std::format("psl_splat32({})", hex32(inst.i18()))); return true;
    case Op::IOHL: set(std::format("psl_or({}, psl_splat32({}))", R(rt), hex32(inst.i16()))); return true;
    case Op::FSMBI: {
        u32 w[4] = {};
        for (u32 i = 0; i < 16; i++)
            if ((inst.i16() >> (15 - i)) & 1) w[i / 4] |= 0xFFu << (8 * (3 - i % 4));
        set(std::format("psl_const({}, {}, {}, {})", hex32(w[0]), hex32(w[1]), hex32(w[2]), hex32(w[3])));
        return true;
    }

    // ---- integer ----
    case Op::A: bin("psl_a"); return true;
    case Op::AH: bin("psl_ah"); return true;
    case Op::SF: bin("psl_sf"); return true;
    case Op::SFH: bin("psl_sfh"); return true;
    case Op::AI: imm32("psl_a", si10); return true;
    case Op::AHI: imm16("psl_ah", si10); return true;
    case Op::SFI: imm32("psl_sf", si10); return true;
    case Op::SFHI: imm16("psl_sfh", si10); return true;
    case Op::ADDX: tri("psl_addx"); return true;
    case Op::SFX: tri("psl_sfx"); return true;
    case Op::CG: bin("psl_cg"); return true;
    case Op::CGX: tri("psl_cgx"); return true;
    case Op::BG: bin("psl_bg"); return true;
    case Op::BGX: tri("psl_bgx"); return true;
    case Op::MPY: bin("psl_mpy"); return true;
    case Op::MPYU: bin("psl_mpyu"); return true;
    case Op::MPYI: imm32("psl_mpy", si10 & 0xFFFF); return true;
    case Op::MPYUI: imm32("psl_mpyu", si10 & 0xFFFF); return true;
    case Op::MPYH: bin("psl_mpyh"); return true;
    case Op::MPYS: bin("psl_mpys"); return true;
    case Op::MPYHH: bin("psl_mpyhh"); return true;
    case Op::MPYHHA: tri("psl_mpyhha"); return true;
    case Op::MPYHHU: bin("psl_mpyhhu"); return true;
    case Op::MPYHHAU: tri("psl_mpyhhau"); return true;
    case Op::MPYA: quad("psl_mpya"); return true;
    case Op::CLZ: un("psl_clz"); return true;
    case Op::CNTB: un("psl_cntb"); return true;
    case Op::FSM: un("psl_fsm"); return true;
    case Op::FSMH: un("psl_fsmh"); return true;
    case Op::FSMB: un("psl_fsmb"); return true;
    case Op::GB: un("psl_gb"); return true;
    case Op::GBH: un("psl_gbh"); return true;
    case Op::GBB: un("psl_gbb"); return true;
    case Op::AVGB: bin("psl_avgb"); return true;
    case Op::ABSDB: bin("psl_absdb"); return true;
    case Op::SUMB: bin("psl_sumb"); return true;
    case Op::XSBH: un("psl_xsbh"); return true;
    case Op::XSHW: un("psl_xshw"); return true;
    case Op::XSWD: un("psl_xswd"); return true;

    // ---- logical ----
    case Op::AND: bin("psl_and"); return true;
    case Op::ANDC: bin("psl_andc"); return true;
    case Op::OR: bin("psl_or"); return true;
    case Op::ORC: bin("psl_orc"); return true;
    case Op::XOR: bin("psl_xor"); return true;
    case Op::NAND: bin("psl_nand"); return true;
    case Op::NOR: bin("psl_nor"); return true;
    case Op::EQV: bin("psl_eqv"); return true;
    case Op::ANDI: imm32("psl_and", si10); return true;
    case Op::ORI: imm32("psl_or", si10); return true;
    case Op::XORI: imm32("psl_xor", si10); return true;
    case Op::ANDHI: imm16("psl_and", si10); return true;
    case Op::ORHI: imm16("psl_or", si10); return true;
    case Op::XORHI: imm16("psl_xor", si10); return true;
    case Op::ANDBI: imm8("psl_and", inst.i10()); return true;
    case Op::ORBI: imm8("psl_or", inst.i10()); return true;
    case Op::XORBI: imm8("psl_xor", inst.i10()); return true;
    case Op::ORX: un("psl_orx"); return true;
    case Op::SELB: quad("psl_selb"); return true;
    case Op::SHUFB: quad("psl_shufb"); return true;

    // ---- compares ----
    case Op::CEQ: bin("psl_ceq"); return true;
    case Op::CEQH: bin("psl_ceqh"); return true;
    case Op::CEQB: bin("psl_ceqb"); return true;
    case Op::CGT: bin("psl_cgt"); return true;
    case Op::CGTH: bin("psl_cgth"); return true;
    case Op::CGTB: bin("psl_cgtb"); return true;
    case Op::CLGT: bin("psl_clgt"); return true;
    case Op::CLGTH: bin("psl_clgth"); return true;
    case Op::CLGTB: bin("psl_clgtb"); return true;
    case Op::CEQI: imm32("psl_ceq", si10); return true;
    case Op::CEQHI: imm16("psl_ceqh", si10); return true;
    case Op::CEQBI: imm8("psl_ceqb", inst.i10()); return true;
    case Op::CGTI: imm32("psl_cgt", si10); return true;
    case Op::CGTHI: imm16("psl_cgth", si10); return true;
    case Op::CGTBI: imm8("psl_cgtb", inst.i10()); return true;
    case Op::CLGTI: imm32("psl_clgt", si10); return true;
    case Op::CLGTHI: imm16("psl_clgth", si10); return true;
    case Op::CLGTBI: imm8("psl_clgtb", inst.i10()); return true;

    // ---- shifts and rotates ----
    case Op::SHL: bin("psl_shl"); return true;
    case Op::SHLH: bin("psl_shlh"); return true;
    case Op::ROT: bin("psl_rot"); return true;
    case Op::ROTH: bin("psl_roth"); return true;
    case Op::ROTM: bin("psl_rotm"); return true;
    case Op::ROTHM: bin("psl_rothm"); return true;
    case Op::ROTMA: bin("psl_rotma"); return true;
    case Op::ROTMAH: bin("psl_rotmah"); return true;
    case Op::SHLI: set(std::format("psl_shli({}, {}u)", R(ra), i7 & 0x3F)); return true;
    case Op::SHLHI: set(std::format("psl_shlhi({}, {}u)", R(ra), i7 & 0x1F)); return true;
    case Op::ROTI: set(std::format("psl_roti({}, {}u)", R(ra), i7 & 0x1F)); return true;
    case Op::ROTHI: set(std::format("psl_rothi({}, {}u)", R(ra), i7 & 0xF)); return true;
    case Op::ROTMI: set(std::format("psl_shri({}, {}u)", R(ra), (0u - i7) & 0x3F)); return true;
    case Op::ROTHMI: set(std::format("psl_shrhi({}, {}u)", R(ra), (0u - i7) & 0x1F)); return true;
    case Op::ROTMAI: set(std::format("psl_srai({}, {}u)", R(ra), (0u - i7) & 0x3F)); return true;
    case Op::ROTMAHI: set(std::format("psl_srahi({}, {}u)", R(ra), (0u - i7) & 0x1F)); return true;
    case Op::SHLQBI: set(std::format("psl_shlqbi({}, {} & 7u)", R(ra), P(rb))); return true;
    case Op::ROTQBI: set(std::format("psl_rotqbi({}, {} & 7u)", R(ra), P(rb))); return true;
    case Op::ROTQMBI: set(std::format("psl_rotqmbi({}, (0u - {}) & 7u)", R(ra), P(rb))); return true;
    case Op::SHLQBII: set(std::format("psl_shlqbi({}, {}u)", R(ra), i7 & 7)); return true;
    case Op::ROTQBII: set(std::format("psl_rotqbi({}, {}u)", R(ra), i7 & 7)); return true;
    case Op::ROTQMBII: set(std::format("psl_rotqmbi({}, {}u)", R(ra), (0u - i7) & 7)); return true;
    case Op::SHLQBY: set(std::format("psl_shlqby({}, {} & 0x1Fu)", R(ra), P(rb))); return true;
    case Op::ROTQBY: set(std::format("psl_rotqby({}, {} & 0xFu)", R(ra), P(rb))); return true;
    case Op::ROTQMBY: set(std::format("psl_rotqmby({}, (0u - {}) & 0x1Fu)", R(ra), P(rb))); return true;
    case Op::SHLQBYI: set(std::format("psl_shlqby({}, {}u)", R(ra), i7 & 0x1F)); return true;
    case Op::ROTQBYI: set(std::format("psl_rotqby({}, {}u)", R(ra), i7 & 0xF)); return true;
    case Op::ROTQMBYI: set(std::format("psl_rotqmby({}, {}u)", R(ra), (0u - i7) & 0x1F)); return true;
    case Op::SHLQBYBI: set(std::format("psl_shlqby({}, ({} >> 3) & 0x1Fu)", R(ra), P(rb))); return true;
    case Op::ROTQBYBI: set(std::format("psl_rotqby({}, ({} >> 3) & 0xFu)", R(ra), P(rb))); return true;
    case Op::ROTQMBYBI: set(std::format("psl_rotqmby({}, (0u - ({} >> 3)) & 0x1Fu)", R(ra), P(rb))); return true;
    case Op::CBD:
    case Op::CHD:
    case Op::CWD:
    case Op::CDD: {
        const u32 size = inst.op == Op::CBD ? 1 : inst.op == Op::CHD ? 2 : inst.op == Op::CWD ? 4 : 8;
        set(std::format("psl_cmask({} + {}, {}u)", P(ra), hex32(static_cast<u32>(inst.si7())), size));
        return true;
    }
    case Op::CBX:
    case Op::CHX:
    case Op::CWX:
    case Op::CDX: {
        const u32 size = inst.op == Op::CBX ? 1 : inst.op == Op::CHX ? 2 : inst.op == Op::CWX ? 4 : 8;
        set(std::format("psl_cmask({} + {}, {}u)", P(ra), P(rb), size));
        return true;
    }

    // ---- single precision ----
    case Op::FA: bin(acc ? "psl_fa_x" : "psl_fa"); return true;
    case Op::FS: bin(acc ? "psl_fs_x" : "psl_fs"); return true;
    case Op::FM: bin(acc ? "psl_fm_x" : "psl_fm"); return true;
    case Op::FMA: quad(acc ? "psl_fma_x" : "psl_fma"); return true;
    case Op::FNMS: quad(acc ? "psl_fnms_x" : "psl_fnms"); return true;
    case Op::FMS: quad(acc ? "psl_fms_x" : "psl_fms"); return true;
    case Op::FCGT: bin("psl_fcgt"); return true;
    case Op::FCMGT: bin("psl_fcmgt"); return true;
    case Op::FCEQ: bin("psl_fceq"); return true;
    case Op::FCMEQ: bin("psl_fcmeq"); return true;
    case Op::FREST: un("psl_frest"); return true;
    case Op::FRSQEST: un("psl_frsqest"); return true;
    case Op::FI: bin("psl_fi"); return true;
    case Op::CSFLT: set(std::format("psl_csflt({}, {}u, {})", R(ra), inst.i8(), acc ? 1 : 0)); return true;
    case Op::CUFLT: set(std::format("psl_cuflt({}, {}u, {})", R(ra), inst.i8(), acc ? 1 : 0)); return true;
    case Op::CFLTS: set(std::format("psl_cflts({}, {}u)", R(ra), inst.i8())); return true;
    case Op::CFLTU: set(std::format("psl_cfltu({}, {}u)", R(ra), inst.i8())); return true;
    case Op::FSCRRD: o += std::format("    memcpy(&{}, &ctx->fpscr, 16);\n", R(rt)); return true;
    case Op::FSCRWR:
        o += std::format("    {{\n        ps_spu_v128 v;\n        memcpy(&v, &{}, 16);\n        ps_spu_fscrwr(ctx, &v);\n    }}\n", R(ra));
        return true;

    // ---- double precision ----
    case Op::DFA: bin("psl_dfa"); return true;
    case Op::DFS: bin("psl_dfs"); return true;
    case Op::DFM: bin("psl_dfm"); return true;
    case Op::DFMA: tri("psl_dfma"); return true;
    case Op::DFMS: tri("psl_dfms"); return true;
    case Op::DFNMS: tri("psl_dfnms"); return true;
    case Op::DFNMA: tri("psl_dfnma"); return true;
    case Op::FESD: un("psl_fesd"); return true;
    case Op::FRDS: un("psl_frds"); return true;

    case Op::UNKNOWN:
    case Op::DFCGT:
    case Op::DFCMGT:
    case Op::DFTSV:
    case Op::DFCEQ:
    case Op::DFCMEQ:
    case Op::COUNT: break;
    }
    // Not supported: raise the exception the interpreter would raise.
    o += std::format("    ps_spu_illegal(ctx, {}, 0x{:08X}u);\n    goto exit_spill;\n", flow.pc(a), inst.raw);
    return false;
}

std::string reg_macros(const RegSet& used, const RegSet& written)
{
    std::string spill = "#define PSL_SPILL() do {", reload = "#define PSL_RELOAD() do {";
    for (u32 r = 0; r < 128; r++) {
        if (written.test(r)) spill += std::format(" psl_st(ctx, {}, r{});", r, r);
        if (used.test(r)) reload += std::format(" r{} = psl_ld(ctx, {});", r, r);
    }
    return spill + " } while (0)\n" + reload + " } while (0)\n";
}

std::string reg_decls(const RegSet& used)
{
    std::string decl;
    u32 n = 0;
    for (u32 r = 0; r < 128; r++) {
        if (!used.test(r)) continue;
        decl += n % 12 == 0 ? (n ? ";\n    ps_spu_vec " : "    ps_spu_vec ") : ", ";
        decl += std::format("r{}", r);
        n++;
    }
    return n ? decl + ";\n" : "";
}

// ---- step mode ----

class StepFlow final : public Flow {
public:
    using Flow::Flow;
    std::string jump(u32 target, u32) const override { return leave(target); }
    std::string jump_dynamic() const override { return "    goto exit_spill;\n"; }
    std::string call(u32 target, u32) const override { return leave(target); }
    std::string call_dynamic(u32) const override { return "    goto exit_spill;\n"; }
};

// ---- full mode ----

class FunctionFlow final : public Flow {
public:
    FunctionFlow(const Function& f, const std::map<u32, std::string>& symbols, bool relocatable)
        : Flow(relocatable), f_(f), symbols_(symbols)
    {
    }

    std::string jump(u32 target, u32 from) const override
    {
        if (!f_.labels.contains(target)) return leave(target);
        if (target <= from)
            return std::format("    if (__atomic_load_n(&ctx->attention, __ATOMIC_RELAXED)) {{\n        ctx->pc = {};\n"
                               "        goto exit_spill;\n    }}\n    goto L_{:05X};\n",
                pc(target), target);
        return std::format("    goto L_{:05X};\n", target);
    }
    std::string jump_dynamic() const override { return "    goto dispatch;\n"; }
    std::string call(u32 target, u32 ret) const override
    {
        const auto it = symbols_.find(target);
        if (it == symbols_.end()) return leave(target);
        return std::format("    PSL_SPILL();\n    ctx->pc = {};\n    {}(ctx, ls);\n{}", pc(target), it->second, after_call(ret));
    }
    std::string call_dynamic(u32 ret) const override
    {
        return std::format("    {{\n        const ps_spu_fn fn = ps_spu_lookup(ctx, ctx->pc);\n        if (!fn) goto exit_spill;\n"
                           "        PSL_SPILL();\n        fn(ctx, ls);\n    }}\n{}",
            after_call(ret));
    }

private:
    std::string after_call(u32 ret) const
    {
        // Continue only if the callee returned to us; otherwise ctx is current and the run loop takes over.
        std::string s = std::format("    if (ctx->pc != {} || ctx->stop_reason) goto exit_now;\n    PSL_RELOAD();\n", pc(ret));
        s += f_.labels.contains(ret) ? std::format("    goto L_{:05X};\n", ret) : std::format("    goto exit_spill;\n");
        return s;
    }

    const Function& f_;
    const std::map<u32, std::string>& symbols_;
};

std::string lift_function(const LsImage& image, const Function& f, const std::string& symbol,
    const std::map<u32, std::string>& symbols, const LiftOptions& options)
{
    std::string o;
    o += reg_macros(f.used, f.written);
    o += std::format("void {}(SPUContext* ctx, uint8_t* ls)\n{{\n", symbol);
    o += reg_decls(f.used);
    if (options.relocatable) o += "    const uint32_t rel = ctx->reloc;\n";
    o += "    PSL_RELOAD();\n    goto dispatch;\n";
    const FunctionFlow flow(f, symbols, options.relocatable);
    for (usize i = 0; i < f.insns.size(); i++) {
        const u32 a = f.insns[i];
        const u32 word = image.word(a);
        if (f.labels.contains(a)) o += std::format("L_{:05X}:\n", a);
        if (options.comments) o += std::format("    /* {:05x}: {:08x}  {} */\n", a, word, disassemble(word, a));
        const bool falls = lower(o, decode(word), a, flow, options.xfloat, true);
        if (falls) {
            const bool next_emitted = i + 1 < f.insns.size() && f.insns[i + 1] == a + 4;
            if (!next_emitted) o += std::format("    ctx->pc = {};\n    goto exit_spill;\n", flow.pc((a + 4) & PS_SPU_PC_MASK));
        }
    }
    o += "exit_spill:\n    PSL_SPILL();\nexit_now:\n    return;\ndispatch:\n";
    o += options.relocatable ? "    switch ((ctx->pc - rel) & 0x3FFFFu) {\n" : "    switch (ctx->pc) {\n";
    for (u32 l : f.labels) o += std::format("    case 0x{:05X}u: goto L_{:05X};\n", l, l);
    o += "    default: goto exit_spill;\n    }\n}\n#undef PSL_SPILL\n#undef PSL_RELOAD\n\n";
    return o;
}

bool write_text(const std::string& path, const std::string& content)
{
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::string old;
        char buf[65536];
        usize n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) old.append(buf, n);
        std::fclose(f);
        if (old == content) return true;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

std::string function_symbol(u64 hash, u32 code_start, u32 entry, bool relocatable)
{
    return std::format("spu_{:016x}_{}{:05x}_{:05x}", hash, relocatable ? "r" : "", code_start, entry);
}

std::string lift_step_function(std::string_view symbol, u32 insn, u32 pc, u8 xfloat, bool relocatable)
{
    const Inst inst = decode(insn);
    const RegUse use = reg_use(inst);
    RegSet used = use.read;
    used |= use.write;
    std::string o = reg_macros(used, use.write);
    o += std::format("void {}(SPUContext* ctx, uint8_t* ls)\n{{\n", symbol);
    o += reg_decls(used);
    if (relocatable) o += "    const uint32_t rel = ctx->reloc;\n    (void)rel;\n";
    o += "    PSL_RELOAD();\n";
    const StepFlow flow(relocatable);
    if (lower(o, inst, pc, flow, xfloat, true)) o += std::format("    ctx->pc = {};\n", flow.pc((pc + 4) & PS_SPU_PC_MASK));
    o += "exit_spill:\n    PSL_SPILL();\nexit_now:\n    return;\n}\n#undef PSL_SPILL\n#undef PSL_RELOAD\n";
    return o;
}

LiftResult lift(const LsImage& image, const Program& program, const LiftOptions& options)
{
    LiftResult result;
    result.hash = image.hash();
    const u32 tus = std::max<u32>(1, options.tu_count);

    std::map<u32, std::string> symbols;
    for (const Function& f : program.functions)
        symbols[f.entry] = function_symbol(result.hash, image.code_start, f.entry, options.relocatable);

    // Header
    std::string h = std::format("/* Generated by spu-lift ({}). Do not edit. */\n#ifndef {}_H\n#define {}_H\n", image.name,
        options.name, options.name);
    h += "#include \"pspocket/spu/runtime.h\"\n";
    h += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
    for (const auto& [entry, sym] : symbols) h += std::format("void {}(SPUContext* ctx, uint8_t* ls);\n", sym);
    h += std::format("extern ps_spu_program {}_program;\nvoid {}_register(void);\n", options.name, options.name);
    h += "#ifdef __cplusplus\n}\n#endif\n#endif\n";
    result.files.push_back({options.name + ".h", h});

    // Function units
    std::vector<std::string> units(tus,
        std::format("/* Generated by spu-lift ({}). Do not edit. */\n#include \"pspocket/spu/lifted.h\"\n#include \"{}.h\"\n\n",
            image.name, options.name));
    usize index = 0;
    for (const Function& f : program.functions) {
        units[index++ % tus] += lift_function(image, f, symbols[f.entry], symbols, options);
        result.stats.instructions += f.insns.size();
        result.stats.labels += f.labels.size();
    }
    for (u32 i = 0; i < tus; i++) result.files.push_back({std::format("{}_{}.c", options.name, i), units[i]});

    // Program descriptor: every label is an entry (the function whose entry it is wins)
    std::map<u32, std::string> entries;
    for (const Function& f : program.functions)
        for (u32 l : f.labels) {
            auto [it, inserted] = entries.emplace(l, symbols[f.entry]);
            if (!inserted && l == f.entry) it->second = symbols[f.entry];
        }
    std::string r = std::format("/* Generated by spu-lift ({}). Do not edit. */\n#include \"{}.h\"\n\n", image.name, options.name);
    r += std::format("static const ps_spu_entry {}_entries[] = {{\n", options.name);
    for (const auto& [addr, sym] : entries) r += std::format("    {{0x{:05X}u, {}}},\n", addr, sym);
    if (entries.empty()) r += "    {0u, 0},\n";
    r += "};\n\n";
    // The lifted instruction words, a bit each: a DMA over anything else in the range leaves the program valid. Code
    // only discovery found is left out: it may be data the program writes (GTA V's jobs: the whole job was dropped
    // and interpreted, 12 fps, run 096).
    std::vector<u32> bits((image.code_size() / 4 + 31) / 32, 0);
    for (const Function& f : program.functions)
        for (u32 a : f.discovered ? std::vector<u32>{} : f.insns) {
            const u32 w = (a - image.code_start) / 4;
            bits[w / 32] |= 1u << (w % 32);
        }
    r += std::format("static const uint32_t {}_code[] = {{", options.name);
    for (usize i = 0; i < bits.size(); i++) r += std::format("{}0x{:08X}u,", i % 8 ? " " : "\n    ", bits[i]);
    if (bits.empty()) r += "0u";
    r += "\n};\n\n";
    r += std::format("ps_spu_program {}_program = {{\"{}\", 0x{:016X}ull, 0x{:05X}u, 0x{:X}u, {}, {}, {}u, {}_entries, 0, {}_code}};\n\n",
        options.name, image.name.empty() ? options.name : image.name, result.hash, image.code_start, image.code_size(),
        options.xfloat, options.relocatable ? 1 : 0, entries.size(), options.name, options.name);
    r += std::format("void {}_register(void) {{ ps_spu_register_program(&{}_program); }}\n", options.name, options.name);
    result.files.push_back({options.name + "_reg.c", r});

    result.stats.functions = program.functions.size();
    result.stats.unsupported = program.unsupported.size();
    return result;
}

bool write_files(const std::string& dir, const std::vector<OutputFile>& files, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = std::format("cannot create {}: {}", dir, ec.message());
        return false;
    }
    for (const OutputFile& f : files) {
        const std::string path = dir + "/" + f.name;
        if (!write_text(path, f.content)) {
            error = std::format("cannot write {}", path);
            return false;
        }
    }
    return true;
}

} // namespace pspocket::recomp::spu
