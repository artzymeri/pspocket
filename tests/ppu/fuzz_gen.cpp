// ppu-fuzz-gen: writes the differential fuzz cases for every instruction form the interpreter supports.
//
//   ppu-fuzz-gen --out DIR --cases K --tus N
//
// For each form (op + Rc/OE/LK/AA variant) it draws K random valid encodings, lifts each into a step function with
// the real lifter (lift_step_function: same code generation as ppu-lift) and records overrides that keep effective
// addresses inside the test window and provoke edge cases. Output: DIR/fuzz_lifted_<i>.c (the functions) and
// DIR/fuzz_cases.c (the table, see fuzz_case.h). The generator seed is fixed, so the output is stable.

#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "fuzz_case.h"
#include "ppu_test_util.hpp"
#include "pspocket/ppu/decoder.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"

using namespace pspocket;
using namespace pspocket::ppu;
namespace as = pspocket::recomp::ppu::as;
using test::Rng;

namespace {

struct GenCase {
    Op op;
    u32 variant;
    u32 insn;
    u32 pc;
    std::vector<ps_fuzz_override> overrides;
};

u32 access_size(Op op)
{
    switch (op) {
    case Op::LBZ: case Op::LBZU: case Op::LBZX: case Op::LBZUX: case Op::STB: case Op::STBU: case Op::STBX: case Op::STBUX: return 1;
    case Op::LHZ: case Op::LHZU: case Op::LHZX: case Op::LHZUX: case Op::LHA: case Op::LHAU: case Op::LHAX: case Op::LHAUX:
    case Op::STH: case Op::STHU: case Op::STHX: case Op::STHUX: case Op::LHBRX: case Op::STHBRX: return 2;
    case Op::LD: case Op::LDU: case Op::LDX: case Op::LDUX: case Op::STD: case Op::STDU: case Op::STDX: case Op::STDUX:
    case Op::LDBRX: case Op::STDBRX: case Op::LDARX: case Op::STDCX_: case Op::LFD: case Op::LFDU: case Op::LFDX:
    case Op::LFDUX: case Op::STFD: case Op::STFDU: case Op::STFDX: case Op::STFDUX: return 8;
    default: return 4;
    }
}

bool is_int_load(const OpInfo& info) { return (info.flags & OPF_LOAD) && !(info.flags & OPF_FP); }

class Generator {
public:
    explicit Generator(u64 seed) : rng_(seed) {}

    GenCase make(Op op, u32 variant)
    {
        GenCase c{op, variant, 0, 0, {}};
        ov_ = &c.overrides;
        const OpInfo& info = op_info(op);
        as::Fields f;
        f.rc = (variant & kVarRc) != 0;
        f.oe = (variant & kVarOe) != 0;
        f.lk = (variant & kVarLk) != 0;
        f.aa = (variant & kVarAa) != 0;
        c.pc = PS_FUZZ_CODE_BASE + rng_.below(PS_FUZZ_CODE_SIZE / 4) * 4;

        switch (info.fmt) {
        case Fmt::D_LOAD:
        case Fmt::D_STORE:
        case Fmt::D_FLOAD:
        case Fmt::D_FSTORE:
        case Fmt::DS_LOAD:
        case Fmt::DS_STORE: d_form_memory(op, info, f); break;
        case Fmt::X_LOAD:
        case Fmt::X_STORE:
        case Fmt::X_FLOAD:
        case Fmt::X_FSTORE: x_form_memory(op, info, f); break;
        case Fmt::X_LSWI: {
            f.d = reg();
            f.b = rng_.below(32); // NB (0 = 32 bytes)
            const u32 range = lswi_reg_mask(f.d, f.b);
            do f.a = reg(); while (op == Op::LSWI && ((range >> f.a) & 1));
            const u32 ea = window_ea(32, 1);
            if (f.a) gpr(f.a, ea + high());
            break;
        }
        case Fmt::X_RA_RB:
            f.a = reg();
            f.b = reg();
            if (op == Op::DCBZ) (void)ea_ra0_rb(f, 1, 1);
            else f.d = rng_.below(32); // TH/CT field of dcbt & co.: ignored
            break;
        case Fmt::X_DST:
        case Fmt::X_DSS:
            f.a = reg();
            f.b = reg();
            f.strm = rng_.below(4);
            f.t = rng_.below(2);
            break;
        case Fmt::X_SYNC: f.l = rng_.below(3); break;
        case Fmt::XL_NONE:
        case Fmt::X_NONE:
        case Fmt::SC: break;
        case Fmt::I_BRANCH: f.imm = static_cast<s32>(rng_.next()); break;
        case Fmt::B_BRANCH:
            f.d = rng_.below(32);
            f.a = rng_.below(32);
            f.imm = static_cast<s32>(rng_.next());
            ctr_edge();
            break;
        case Fmt::XL_BRANCH:
            f.d = op == Op::BCCTR ? (rng_.below(32) | 4) : rng_.below(32); // bcctr with BO[2] = 0 is an invalid form
            f.a = rng_.below(32);
            f.bh = rng_.below(4);
            ctr_edge();
            break;
        case Fmt::D_TO_RA_SI:
            f.d = rng_.below(32);
            f.a = reg();
            f.imm = static_cast<s16>(rng_.next());
            if (rng_.chance(40)) gpr(f.a, static_cast<u64>(static_cast<s64>(f.imm) + static_cast<s64>(rng_.below(3)) - 1));
            break;
        case Fmt::X_TO_RA_RB: {
            f.d = rng_.below(32);
            f.a = reg();
            f.b = reg();
            if (rng_.chance(40)) {
                const u64 x = test::random_gpr(rng_);
                gpr(f.a, x);
                if (f.b != f.a) gpr(f.b, x + rng_.below(3) - 1);
            }
            break;
        }
        case Fmt::XL_CRLOG:
            f.d = rng_.below(32);
            f.a = rng_.below(32);
            f.b = rng_.below(32);
            break;
        case Fmt::XL_MCRF:
        case Fmt::X_MCRFS:
            f.crfd = rng_.below(8);
            f.crfs = rng_.below(8);
            break;
        case Fmt::D_RT_RA_SI:
        case Fmt::D_RA_RS_UI:
            f.d = reg();
            f.a = reg();
            f.imm = static_cast<s16>(rng_.next());
            break;
        case Fmt::D_CMPI:
        case Fmt::D_CMPLI:
            f.crfd = rng_.below(8);
            f.l = rng_.below(2);
            f.a = reg();
            f.imm = static_cast<s16>(rng_.next());
            if (rng_.chance(30)) {
                const s64 v = op == Op::CMPI ? f.imm : static_cast<s64>(static_cast<u16>(f.imm));
                gpr(f.a, static_cast<u64>(v + rng_.below(3) - 1) ^ (rng_.chance(50) ? (rng_.next() << 32) : 0));
            }
            break;
        case Fmt::X_CMP:
            f.crfd = rng_.below(8);
            f.l = rng_.below(2);
            f.a = reg();
            f.b = reg();
            if (rng_.chance(30)) {
                const u64 x = test::random_gpr(rng_);
                gpr(f.a, x);
                if (f.b != f.a) gpr(f.b, (x + rng_.below(3) - 1) ^ (rng_.chance(50) ? (rng_.next() << 32) : 0));
            }
            break;
        case Fmt::XO_RT_RA_RB:
        case Fmt::XO_RT_RA:
            f.d = reg();
            f.a = reg();
            f.b = info.fmt == Fmt::XO_RT_RA_RB ? reg() : 0;
            arithmetic_edges(op, f);
            break;
        case Fmt::X_RA_RS_RB:
            f.d = reg();
            f.a = reg();
            f.b = reg();
            if (rng_.chance(40)) gpr(f.b, rng_.below(130)); // shift amounts around 32/64
            break;
        case Fmt::X_RA_RS:
            f.d = reg();
            f.a = reg();
            break;
        case Fmt::X_RA_RS_SH:
            f.d = reg();
            f.a = reg();
            f.b = rng_.below(32);
            break;
        case Fmt::XS_RA_RS_SH:
            f.d = reg();
            f.a = reg();
            f.sh = rng_.below(64);
            break;
        case Fmt::M_RA_RS_SH_MB_ME:
        case Fmt::M_RA_RS_RB_MB_ME:
            f.d = reg();
            f.a = reg();
            f.b = reg();
            f.c = rng_.below(32);
            f.e = rng_.below(32);
            break;
        case Fmt::MD_RA_RS_SH_MB:
        case Fmt::MDS_RA_RS_RB_MB:
            f.d = reg();
            f.a = reg();
            f.b = reg();
            f.sh = rng_.below(64);
            f.mb = rng_.below(64);
            if (info.fmt == Fmt::MDS_RA_RS_RB_MB && rng_.chance(40)) gpr(f.b, rng_.below(130));
            break;
        case Fmt::XFX_MFSPR: {
            static constexpr u32 kSprs[] = {kSprXer, kSprLr, kSprCtr, kSprVrsave, kSprTbl, kSprTbu};
            f.d = reg();
            f.spr = kSprs[rng_.below(std::size(kSprs))];
            break;
        }
        case Fmt::XFX_MFTB:
            f.d = reg();
            f.spr = rng_.chance(50) ? kSprTbl : kSprTbu;
            break;
        case Fmt::XFX_MTSPR: {
            static constexpr u32 kSprs[] = {kSprXer, kSprLr, kSprCtr, kSprVrsave};
            f.d = reg();
            f.spr = kSprs[rng_.below(std::size(kSprs))];
            break;
        }
        case Fmt::X_MFCR: f.d = reg(); break;
        case Fmt::X_MFOCRF:
            f.d = reg();
            f.fxm = 0x80u >> rng_.below(8);
            break;
        case Fmt::XFX_MTCRF:
            f.d = reg();
            f.fxm = rng_.below(256);
            break;
        case Fmt::A_FRT_FRA_FRB:
        case Fmt::A_FRT_FRA_FRC:
        case Fmt::A_FRT_FRA_FRC_FRB:
        case Fmt::A_FRT_FRB:
        case Fmt::X_FRT_FRB:
            f.d = reg();
            f.a = reg();
            f.b = reg();
            f.c = reg();
            if (info.fmt == Fmt::A_FRT_FRB || info.fmt == Fmt::X_FRT_FRB) f.a = f.c = 0;
            if (info.fmt == Fmt::A_FRT_FRA_FRB) f.c = 0;
            if (info.fmt == Fmt::A_FRT_FRA_FRC) f.b = 0;
            break;
        case Fmt::X_FCMP:
            f.crfd = rng_.below(8);
            f.a = reg();
            f.b = reg();
            break;
        case Fmt::X_MFFS: f.d = reg(); break;
        case Fmt::XFL_MTFSF:
            f.flm = rng_.below(256);
            f.b = reg();
            break;
        case Fmt::X_MTFSFI:
            f.crfd = rng_.below(8);
            f.u4 = rng_.below(16);
            break;
        case Fmt::X_MTFSB: f.d = rng_.below(32); break;
        default:
            std::fprintf(stderr, "ppu-fuzz-gen: no generator for %s\n", info.mnemonic);
            std::exit(1);
        }

        c.insn = as::encode(op, f);
        const Inst check = decode(c.insn);
        if (check.op != op || form_variant(check) != variant || !is_supported(check)) {
            std::fprintf(stderr, "ppu-fuzz-gen: bad encoding %08x for %s (decodes as %s, variant %u)\n", c.insn,
                form_name(op, variant).c_str(), op_info(check.op).mnemonic, form_variant(check));
            std::exit(1);
        }
        return c;
    }

private:
    u32 reg() { return rng_.below(32); }
    u64 high() { return rng_.chance(30) ? (rng_.next() << 32) : 0; }
    void add(u8 kind, u32 index, u64 value) { ov_->push_back(ps_fuzz_override{kind, static_cast<u8>(index), value}); }
    void gpr(u32 r, u64 v)
    {
        // A later override of the same register wins; keep only one.
        for (auto& o : *ov_)
            if (o.kind == PS_FZ_GPR && o.index == r) {
                o.value = v;
                return;
            }
        add(PS_FZ_GPR, r, v);
    }
    u32 window_ea(u32 size, u32 align) { return (PS_FUZZ_WINDOW_BASE + rng_.below(PS_FUZZ_WINDOW_SIZE - size + 1)) & ~(align - 1); }

    void ctr_edge()
    {
        if (rng_.chance(50)) add(PS_FZ_CTR, 0, rng_.chance(70) ? rng_.below(3) : rng_.next());
    }

    // EA = (rA|0) + rB inside the window; returns the EA. With update, rA is always used.
    u32 ea_ra0_rb(as::Fields& f, u32 size, u32 align, bool update = false)
    {
        const u32 ea = window_ea(size, align);
        if (f.a == 0 && !update) {
            gpr(f.b, ea + high());
        } else {
            if (f.b == f.a) f.b = f.a == 31 ? 1 : f.a + 1;
            const u64 x = test::random_gpr(rng_);
            gpr(f.a, x);
            gpr(f.b, static_cast<u64>(ea) - x + high());
        }
        return ea;
    }

    void d_form_memory(Op op, const OpInfo& info, as::Fields& f)
    {
        const bool update = info.flags & OPF_UPDATE;
        const bool ds = info.fmt == Fmt::DS_LOAD || info.fmt == Fmt::DS_STORE;
        u32 size = access_size(op);
        if (op == Op::LMW) {
            f.d = 1 + rng_.below(31);
            f.a = rng_.below(f.d); // rA outside rT..r31 (invalid form otherwise)
            size = 4 * (32 - f.d);
        } else if (op == Op::STMW) {
            f.d = reg();
            f.a = reg();
            size = 4 * (32 - f.d);
        } else {
            f.d = reg();
            f.a = update ? 1 + rng_.below(31) : reg();
            if (update && is_int_load(info) && f.a == f.d) f.a = f.a == 31 ? 1 : f.a + 1; // rA != rT for load-update
        }
        if (f.a == 0 && !update) {
            f.imm = static_cast<s32>(window_ea(size, ds ? 4 : 1)); // EA = d
        } else {
            f.imm = static_cast<s16>(rng_.next());
            if (ds) f.imm &= ~3;
            const u32 ea = window_ea(size, 1);
            gpr(f.a, static_cast<u64>(ea) - static_cast<u64>(static_cast<s64>(f.imm)) + high());
        }
    }

    void x_form_memory(Op op, const OpInfo& info, as::Fields& f)
    {
        const bool update = info.flags & OPF_UPDATE;
        f.d = reg();
        f.a = update ? 1 + rng_.below(31) : reg();
        if (update && is_int_load(info) && f.a == f.d) f.a = f.a == 31 ? 1 : f.a + 1;
        f.b = reg();
        if (f.b == f.a) f.b = f.a == 31 ? 1 : f.a + 1;

        u32 size = access_size(op);
        u32 align = 1;
        if (op == Op::LWARX || op == Op::STWCX_) align = 4;
        if (op == Op::LDARX || op == Op::STDCX_) align = 8;
        if (op == Op::LSWX || op == Op::STSWX) {
            const u32 bc = rng_.below(121); // at most 30 registers: room for rA and rB outside the range
            add(PS_FZ_XER_BC, 0, bc);
            size = 128;
            if (op == Op::LSWX) {
                const u32 range = bc ? lswi_reg_mask(f.d, bc) : 0;
                do f.a = reg(); while ((range >> f.a) & 1);
                do f.b = reg(); while (((range >> f.b) & 1) || f.b == f.a);
            }
        }
        const u32 ea = ea_ra0_rb(f, size, align, update);
        if (op == Op::STWCX_ || op == Op::STDCX_) {
            // Reservation states: valid and matching, stale value, other address, none, or the random one.
            const u8 kinds[] = {PS_FZ_RES_MATCH, PS_FZ_RES_MATCH, PS_FZ_RES_STALE_VALUE, PS_FZ_RES_OTHER_ADDR, PS_FZ_RES_NONE, 0};
            const u8 kind = kinds[rng_.below(std::size(kinds))];
            if (kind) add(kind, align, ea);
        }
    }

    void arithmetic_edges(Op op, as::Fields& f)
    {
        constexpr u64 kMax = 0x7FFFFFFFFFFFFFFFull, kMin = 0x8000000000000000ull;
        const bool word = op == Op::DIVW || op == Op::DIVWU;
        const bool dword = op == Op::DIVD || op == Op::DIVDU;
        if (word || dword) {
            const u32 pick = rng_.below(100);
            if (pick < 25 && f.a != f.b) {
                gpr(f.a, word ? (0x80000000ull | high()) : kMin);
                gpr(f.b, word ? (0xFFFFFFFFull | high()) : ~0ull);
            } else if (pick < 40) {
                gpr(f.b, word ? high() : 0);
            }
            return;
        }
        if (!rng_.chance(35)) return;
        // Operands at the signed-overflow boundary (and the carry-in that makes them overflow).
        switch (op) {
        case Op::ADD:
        case Op::ADDC:
        case Op::ADDE:
            if (f.a == f.b) return;
            gpr(f.a, kMax);
            gpr(f.b, 1 + rng_.below(3));
            break;
        case Op::SUBF:
        case Op::SUBFC:
        case Op::SUBFE: // rB - rA
            if (f.a == f.b) return;
            gpr(f.a, 1 + rng_.below(3));
            gpr(f.b, kMin);
            break;
        case Op::ADDZE: gpr(f.a, kMax); add(PS_FZ_XER_CA, 0, 1); break;
        case Op::ADDME: gpr(f.a, kMin); add(PS_FZ_XER_CA, 0, 0); break;
        case Op::SUBFZE: gpr(f.a, kMin); add(PS_FZ_XER_CA, 0, 1); break;
        case Op::SUBFME: gpr(f.a, kMax); add(PS_FZ_XER_CA, 0, 0); break;
        case Op::NEG: gpr(f.a, kMin); break;
        case Op::MULLW:
        case Op::MULHW:
            if (f.a == f.b) return;
            gpr(f.a, 0x10000ull + rng_.below(3) + high());
            gpr(f.b, 0x8000ull + rng_.below(3));
            break;
        case Op::MULLD:
        case Op::MULHD:
            if (f.a == f.b) return;
            gpr(f.a, 0x100000000ull + rng_.below(3));
            gpr(f.b, 0x80000000ull - 1 + rng_.below(3));
            break;
        default: break;
        }
    }

    Rng rng_;
    std::vector<ps_fuzz_override>* ov_ = nullptr;
};

bool write_text(const std::string& path, const std::string& content)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
        std::string old;
        char buf[65536];
        usize n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) old.append(buf, n);
        std::fclose(f);
        if (old == content) return true;
    }
    f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char** argv)
{
    std::string out;
    u32 per_form = 256, tus = 32;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--out") out = argv[i + 1];
        else if (arg == "--cases") per_form = static_cast<u32>(std::stoul(argv[i + 1]));
        else if (arg == "--tus") tus = static_cast<u32>(std::stoul(argv[i + 1]));
    }
    if (out.empty() || tus == 0) {
        std::fprintf(stderr, "usage: ppu-fuzz-gen --out DIR --cases K --tus N\n");
        return 2;
    }

    Generator gen(0x5EEDF00Dull);
    std::vector<GenCase> cases;
    for (const Form& form : supported_forms())
        for (u32 i = 0; i < per_form; i++) cases.push_back(gen.make(form.op, form.variant));

    std::vector<std::string> units(tus, "/* Generated by ppu-fuzz-gen (tests/ppu). Do not edit. */\n#include \"pspocket/ppu/lifted.h\"\n\n");
    std::string table = "/* Generated by ppu-fuzz-gen (tests/ppu). Do not edit. */\n#include \"fuzz_case.h\"\n\n";
    std::string protos, overrides, rows;
    u32 override_index = 0;
    for (usize i = 0; i < cases.size(); i++) {
        const GenCase& c = cases[i];
        const std::string name = std::format("ps_fz_{:06}", i);
        units[i % tus] += recomp::ppu::lift_step_function(name, c.insn, c.pc) + "\n";
        protos += std::format("void {}(PPUContext* ctx, uint8_t* base);\n", name);
        for (const auto& o : c.overrides) overrides += std::format("    {{{}, {}, 0x{:x}ull}},\n", o.kind, o.index, o.value);
        rows += std::format("    {{{}, {}, {}, 0x{:08x}u, 0x{:08x}u, {}u, {}}},\n", static_cast<u32>(c.op), c.variant,
            c.overrides.size(), c.insn, c.pc, override_index, name);
        override_index += static_cast<u32>(c.overrides.size());
    }
    table += protos;
    table += std::format("\nconst uint32_t ps_fuzz_cases_per_form = {}u;\nconst uint32_t ps_fuzz_case_count = {}u;\n\n", per_form, cases.size());
    table += "const ps_fuzz_override ps_fuzz_overrides[] = {\n" + overrides + "    {0, 0, 0},\n};\n\n";
    table += "const ps_fuzz_case ps_fuzz_cases[] = {\n" + rows + "};\n";

    for (u32 i = 0; i < tus; i++)
        if (!write_text(std::format("{}/fuzz_lifted_{}.c", out, i), units[i])) {
            std::fprintf(stderr, "ppu-fuzz-gen: cannot write to %s\n", out.c_str());
            return 1;
        }
    if (!write_text(out + "/fuzz_cases.c", table)) {
        std::fprintf(stderr, "ppu-fuzz-gen: cannot write to %s\n", out.c_str());
        return 1;
    }
    std::printf("ppu-fuzz-gen: %zu forms x %u cases = %zu lifted functions in %u units\n", supported_forms().size(),
        per_form, cases.size(), tus);
    return 0;
}
