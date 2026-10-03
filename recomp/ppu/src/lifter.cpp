// PPU lifter: emits C11 for analysed functions. Instruction semantics mirror rpcs3/Emu/Cell/PPUInterpreter.cpp
// (the same reference as the interpreter), written as independent C expressions so the differential fuzz test
// compares two implementations. Masks and immediates are computed here, at lift time.

#include "pspocket/recomp/ppu/lifter.hpp"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "pspocket/ppu/decoder.hpp"
#include "pspocket/ppu/disasm.hpp"

namespace pspocket::recomp::ppu {
namespace {

namespace rt = ::pspocket::ppu;
using rt::Inst;
using rt::Op;

// ------------------------------------------------------------------------------------------ names and literals

std::string G(u32 n) { return std::format("r{}", n); }
std::string F(u32 n) { return std::format("f{}", n); }
std::string CR(u32 bit)
{
    static constexpr const char* kBits[4] = {"lt", "gt", "eq", "so"};
    return std::format("cr{}_{}", bit / 4, kBits[bit & 3]);
}

struct SprLocal {
    u32 bit;
    const char* name; // local variable and PPUContext field
    const char* type;
};
constexpr SprLocal kSprLocals[] = {
    {rt::SPR_LR, "lr", "uint64_t"},
    {rt::SPR_CTR, "ctr", "uint64_t"},
    {rt::SPR_XER_SO, "xer_so", "uint8_t"},
    {rt::SPR_XER_OV, "xer_ov", "uint8_t"},
    {rt::SPR_XER_CA, "xer_ca", "uint8_t"},
    {rt::SPR_XER_BC, "xer_bc", "uint8_t"},
    {rt::SPR_FPSCR, "fpscr", "uint32_t"},
    {rt::SPR_VRSAVE, "vrsave", "uint32_t"},
};

std::string hex64(u64 v) { return std::format("0x{:x}ull", v); }
std::string hex32(u32 v) { return std::format("0x{:x}u", v); }
std::string sext64(s64 v) { return hex64(static_cast<u64>(v)); }

// x + k in uint64_t arithmetic.
std::string add_const(const std::string& x, s64 k)
{
    if (k == 0) return x;
    if (k > 0) return std::format("{} + {}ull", x, k);
    return std::format("{} - {}ull", x, static_cast<u64>(-k));
}

// MASK(mb, me) in IBM bit numbering, wrapping when mb > me. Built bit by bit: deliberately not the interpreter's
// closed form (rpcs3 ppu_rotate_mask), so the fuzz test cross-checks both.
u64 mask64(u32 mb, u32 me)
{
    u64 m = 0;
    for (u32 i = mb & 63;; i = (i + 1) & 63) {
        m |= 1ull << (63 - i);
        if (i == (me & 63)) break;
    }
    return m;
}

// ------------------------------------------------------------------------------------------ generator state

struct RegSet {
    u32 gpr = 0, fpr = 0, cr = 0, spr = 0;
};

struct Gen {
    RegSet ref; // declared, loaded at entry, reloaded after calls
    RegSet wr;  // spilled before calls and at exits
    bool step = false;
    bool uses_entry_state = false;
    const CodeImage* image = nullptr;
    const Program* program = nullptr;
    const Function* fn = nullptr;
    const LiftOptions* opt = nullptr;
    LiftStats* stats = nullptr;
    std::string spill;  // statements, ending in a space
    std::string reload;
};

void add_use(Gen& g, const rt::RegUse& u)
{
    g.ref.gpr |= u.gpr_read | u.gpr_write;
    g.ref.fpr |= u.fpr_read | u.fpr_write;
    g.ref.cr |= u.cr_read | u.cr_write;
    g.ref.spr |= u.spr_read | u.spr_write;
    g.wr.gpr |= u.gpr_write;
    g.wr.fpr |= u.fpr_write;
    g.wr.cr |= u.cr_write;
    g.wr.spr |= u.spr_write;
}

void build_spill_reload(Gen& g)
{
    std::string s, r;
    for (u32 n = 0; n < 32; n++) {
        if (g.wr.gpr >> n & 1) s += std::format("ctx->gpr[{0}] = r{0}; ", n);
        if (g.ref.gpr >> n & 1) r += std::format("r{0} = ctx->gpr[{0}]; ", n);
    }
    for (u32 n = 0; n < 32; n++) {
        if (g.wr.fpr >> n & 1) s += std::format("ctx->fpr[{0}] = f{0}; ", n);
        if (g.ref.fpr >> n & 1) r += std::format("f{0} = ctx->fpr[{0}]; ", n);
    }
    for (u32 n = 0; n < 32; n++) {
        if (g.wr.cr >> n & 1) s += std::format("ctx->cr[{}] = {}; ", n, CR(n));
        if (g.ref.cr >> n & 1) r += std::format("{} = ctx->cr[{}]; ", CR(n), n);
    }
    for (const SprLocal& sp : kSprLocals) {
        if (g.wr.spr & sp.bit) s += std::format("ctx->{0} = {0}; ", sp.name);
        if (g.ref.spr & sp.bit) r += std::format("{0} = ctx->{0}; ", sp.name);
    }
    g.spill = s;
    g.reload = r;
}

std::string declarations(const Gen& g)
{
    std::string o;
    auto group = [&](const char* type, std::vector<std::string> decls) {
        for (usize i = 0; i < decls.size(); i += 6) {
            o += std::format("    {} ", type);
            for (usize j = i; j < std::min(decls.size(), i + 6); j++) o += (j > i ? ", " : "") + decls[j];
            o += ";\n";
        }
    };
    std::vector<std::string> d;
    for (u32 n = 0; n < 32; n++)
        if (g.ref.gpr >> n & 1) d.push_back(std::format("r{0} = ctx->gpr[{0}]", n));
    group("uint64_t", std::move(d));
    d.clear();
    for (u32 n = 0; n < 32; n++)
        if (g.ref.fpr >> n & 1) d.push_back(std::format("f{0} = ctx->fpr[{0}]", n));
    group("double", std::move(d));
    d.clear();
    for (u32 n = 0; n < 32; n++)
        if (g.ref.cr >> n & 1) d.push_back(std::format("{} = ctx->cr[{}]", CR(n), n));
    group("uint8_t", std::move(d));
    for (const SprLocal& sp : kSprLocals)
        if (g.ref.spr & sp.bit) o += std::format("    {0} {1} = ctx->{1};\n", sp.type, sp.name);
    return o;
}

// ------------------------------------------------------------------------------------------ snippets

std::string rec0(const std::string& v)
{
    return std::format("cr0_lt = (int64_t){0} < 0; cr0_gt = (int64_t){0} > 0; cr0_eq = {0} == 0; cr0_so = xer_so;", v);
}

constexpr std::string_view kRec1 = "cr1_lt = (uint8_t)(fpscr >> 31); cr1_gt = (uint8_t)((fpscr >> 30) & 1); "
                                   "cr1_eq = (uint8_t)((fpscr >> 29) & 1); cr1_so = (uint8_t)((fpscr >> 28) & 1);";

std::string set_ov(const std::string& expr) { return std::format("xer_ov = (uint8_t)({}); xer_so |= xer_ov;", expr); }

std::string ea_d(const Inst& in, s32 disp)
{
    if (in.ra() == 0) return hex32(static_cast<u32>(disp));
    return std::format("(uint32_t)({})", add_const(G(in.ra()), disp));
}

std::string ea_x(const Inst& in)
{
    if (in.ra() == 0) return std::format("(uint32_t){}", G(in.rb()));
    return std::format("(uint32_t)({} + {})", G(in.ra()), G(in.rb()));
}

// rotl64(x, sh) & mask without the no-op parts.
std::string rot64_masked(const std::string& x, u32 sh, u64 mask)
{
    const std::string r = sh ? std::format("ps_rotl64({}, {})", x, sh) : x;
    return mask == ~0ull ? r : std::format("{} & {}", r, hex64(mask));
}

// Value loaded by an integer load of `op` from guest address `ea` (a uint32_t expression).
std::string int_load(Op op, const std::string& ea)
{
    switch (op) {
    case Op::LBZ: case Op::LBZU: case Op::LBZX: case Op::LBZUX: return std::format("(uint64_t)ps_read8(base, {})", ea);
    case Op::LHZ: case Op::LHZU: case Op::LHZX: case Op::LHZUX: return std::format("(uint64_t)ps_read16(base, {})", ea);
    case Op::LHA: case Op::LHAU: case Op::LHAX: case Op::LHAUX:
        return std::format("(uint64_t)(int64_t)(int16_t)ps_read16(base, {})", ea);
    case Op::LWZ: case Op::LWZU: case Op::LWZX: case Op::LWZUX: return std::format("(uint64_t)ps_read32(base, {})", ea);
    case Op::LWA: case Op::LWAX: case Op::LWAUX: return std::format("(uint64_t)(int64_t)(int32_t)ps_read32(base, {})", ea);
    case Op::LD: case Op::LDU: case Op::LDX: case Op::LDUX: return std::format("ps_read64(base, {})", ea);
    case Op::LHBRX: return std::format("(uint64_t)ps_bswap16(ps_read16(base, {}))", ea);
    case Op::LWBRX: return std::format("(uint64_t)ps_bswap32(ps_read32(base, {}))", ea);
    case Op::LDBRX: return std::format("ps_bswap64(ps_read64(base, {}))", ea);
    default: return {};
    }
}

std::string int_store(Op op, const std::string& ea, const std::string& v)
{
    switch (op) {
    case Op::STB: case Op::STBU: case Op::STBX: case Op::STBUX: return std::format("ps_write8(base, {}, (uint8_t){});", ea, v);
    case Op::STH: case Op::STHU: case Op::STHX: case Op::STHUX: return std::format("ps_write16(base, {}, (uint16_t){});", ea, v);
    case Op::STW: case Op::STWU: case Op::STWX: case Op::STWUX: return std::format("ps_write32(base, {}, (uint32_t){});", ea, v);
    case Op::STD: case Op::STDU: case Op::STDX: case Op::STDUX: return std::format("ps_write64(base, {}, {});", ea, v);
    case Op::STHBRX: return std::format("ps_write16(base, {}, ps_bswap16((uint16_t){}));", ea, v);
    case Op::STWBRX: return std::format("ps_write32(base, {}, ps_bswap32((uint32_t){}));", ea, v);
    case Op::STDBRX: return std::format("ps_write64(base, {}, ps_bswap64({}));", ea, v);
    default: return {};
    }
}

std::string fp_load(Op op, u32 frt, const std::string& ea)
{
    const bool single = op == Op::LFS || op == Op::LFSU || op == Op::LFSX || op == Op::LFSUX;
    return single ? std::format("{} = (double)ps_readf32(base, {});", F(frt), ea)
                  : std::format("{} = ps_readf64(base, {});", F(frt), ea);
}

std::string fp_store(Op op, u32 frs, const std::string& ea)
{
    if (op == Op::STFIWX) return std::format("ps_write32(base, {}, (uint32_t)ps_f64_bits({}));", ea, F(frs));
    const bool single = op == Op::STFS || op == Op::STFSU || op == Op::STFSX || op == Op::STFSUX;
    return single ? std::format("ps_writef32(base, {}, (float){});", ea, F(frs))
                  : std::format("ps_writef64(base, {}, {});", ea, F(frs));
}

// Branch condition for BO/BI (CTR already decremented when BO[2] = 0). Empty = always.
std::string cond_expr(u32 bo, u32 bi, bool test_ctr)
{
    std::string c;
    if (test_ctr && !(bo & 0x04)) c = (bo & 0x02) ? "ctr == 0" : "ctr != 0";
    if (!(bo & 0x10)) {
        if (!c.empty()) c += " && ";
        c += (bo & 0x08) ? CR(bi) : "!" + CR(bi);
    }
    return c;
}

// Trap condition (TW TWI TD TDI), as a C expression over a/b (signed) and au/bu (unsigned) temporaries.
std::string trap_cond(u32 to)
{
    std::vector<std::string> t;
    if (to & 0x10) t.push_back("a < b");
    if (to & 0x08) t.push_back("a > b");
    if (to & 0x04) t.push_back("a == b");
    if (to & 0x02) t.push_back("au < bu");
    if (to & 0x01) t.push_back("au > bu");
    if (t.empty()) return "0";
    std::string c;
    for (usize i = 0; i < t.size(); i++) c += (i ? " || " : "") + t[i];
    return c;
}

// ------------------------------------------------------------------------------------------ instruction semantics

// Emits a supported non-control instruction. Returns false when the op is not handled here.
bool emit_semantics(const Inst& in, std::string& o)
{
    const u32 rt_ = in.rt(), ra = in.ra(), rb = in.rb();
    const std::string T = G(rt_), A = G(ra), B = G(rb), S = G(rt_);
    const bool rc = in.rc() != 0;
    const bool oe = (in.flags() & rt::OPF_OE) && in.oe();
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    auto rec_int = [&](const std::string& v) {
        if (rc) line(rec0(v));
    };
    auto rec_fp = [&] {
        if (rc) line(std::string(kRec1));
    };
    const s64 si = in.simm();

    switch (in.op) {
    // ---- integer arithmetic
    case Op::ADDI: line(ra ? std::format("{} = {};", T, add_const(A, si)) : std::format("{} = {};", T, sext64(si))); return true;
    case Op::ADDIS:
        line(ra ? std::format("{} = {};", T, add_const(A, si * 65536)) : std::format("{} = {};", T, sext64(si * 65536)));
        return true;
    case Op::ADDIC:
    case Op::ADDIC_:
        line(std::format("{{ const uint64_t a = {}, r = a + {}; xer_ca = (uint8_t)(r < a); {} = r; }}", A, sext64(si), T));
        if (in.op == Op::ADDIC_) line(rec0(T));
        return true;
    case Op::SUBFIC: line(std::format("{} = ps_add_carry(~{}, {}, 1, &xer_ca);", T, A, sext64(si))); return true;
    case Op::MULLI: line(std::format("{} = {} * {};", T, A, sext64(si))); return true;
    case Op::ADD:
    case Op::ADDC:
    case Op::ADDE:
    case Op::SUBF:
    case Op::SUBFC:
    case Op::SUBFE: {
        // r = x + y (+ carry), with x = rA or ~rA
        const bool sub = in.op == Op::SUBF || in.op == Op::SUBFC || in.op == Op::SUBFE;
        std::string body = std::format("const uint64_t a = {}{}, b = {}; ", sub ? "~" : "", A, B);
        switch (in.op) {
        case Op::ADD: body += "const uint64_t r = a + b; "; break;
        case Op::SUBF: body += "const uint64_t r = a + b + 1; "; break;
        case Op::ADDC: body += "const uint64_t r = ps_add_carry(a, b, 0, &xer_ca); "; break;
        case Op::SUBFC: body += "const uint64_t r = ps_add_carry(a, b, 1, &xer_ca); "; break;
        default: body += "const uint64_t r = ps_add_carry(a, b, xer_ca, &xer_ca); "; break;
        }
        if (oe) body += set_ov("ps_add_ov(a, b, r)") + " ";
        line(std::format("{{ {}{} = r; }}", body, T));
        rec_int(T);
        return true;
    }
    case Op::ADDZE:
    case Op::ADDME:
    case Op::SUBFZE:
    case Op::SUBFME: {
        const bool sub = in.op == Op::SUBFZE || in.op == Op::SUBFME;
        const bool minus1 = in.op == Op::ADDME || in.op == Op::SUBFME;
        std::string body = std::format("const uint64_t a = {}{}, b = {}; const uint64_t r = ps_add_carry(a, b, xer_ca, &xer_ca); ",
            sub ? "~" : "", A, minus1 ? "~0ull" : "0");
        if (oe) body += set_ov("ps_add_ov(a, b, r)") + " ";
        line(std::format("{{ {}{} = r; }}", body, T));
        rec_int(T);
        return true;
    }
    case Op::NEG:
        line(std::format("{{ const uint64_t a = {}; {}{} = 0 - a; }}", A, oe ? set_ov("a == 0x8000000000000000ull") + " " : "", T));
        rec_int(T);
        return true;
    case Op::MULLW:
        line(std::format("{{ const int64_t p = (int64_t)(int32_t){} * (int64_t)(int32_t){}; {}{} = (uint64_t)p; }}", A, B,
            oe ? set_ov("p < INT32_MIN || p > INT32_MAX") + " " : "", T));
        rec_int(T);
        return true;
    case Op::MULLD:
        line(std::format("{{ const uint64_t a = {}, b = {}; {}{} = a * b; }}", A, B, oe ? set_ov("ps_mulld_ov(a, b)") + " " : "", T));
        rec_int(T);
        return true;
    case Op::MULHW:
        line(std::format("{} = (uint64_t)(((int64_t)(int32_t){} * (int64_t)(int32_t){}) >> 32);", T, A, B));
        rec_int(T);
        return true;
    case Op::MULHWU:
        line(std::format("{} = ((uint64_t)(uint32_t){} * (uint64_t)(uint32_t){}) >> 32;", T, A, B));
        rec_int(T);
        return true;
    case Op::MULHD: line(std::format("{} = ps_mulhd({}, {});", T, A, B)); rec_int(T); return true;
    case Op::MULHDU: line(std::format("{} = ps_mulhdu({}, {});", T, A, B)); rec_int(T); return true;
    case Op::DIVW:
        line(std::format("{{ const int32_t a = (int32_t){}, b = (int32_t){}; const uint8_t o = b == 0 || (a == INT32_MIN && b == -1); "
                         "{}{} = o ? 0 : (uint64_t)(uint32_t)(a / b); }}",
            A, B, oe ? "xer_ov = o; xer_so |= o; " : "", T));
        rec_int(T);
        return true;
    case Op::DIVWU:
        line(std::format("{{ const uint32_t a = (uint32_t){}, b = (uint32_t){}; {}{} = b == 0 ? 0 : (uint64_t)(a / b); }}", A, B,
            oe ? set_ov("b == 0") + " " : "", T));
        rec_int(T);
        return true;
    case Op::DIVD:
        line(std::format("{{ const int64_t a = (int64_t){}, b = (int64_t){}; const uint8_t o = b == 0 || (a == INT64_MIN && b == -1); "
                         "{}{} = o ? 0 : (uint64_t)(a / b); }}",
            A, B, oe ? "xer_ov = o; xer_so |= o; " : "", T));
        rec_int(T);
        return true;
    case Op::DIVDU:
        line(std::format("{{ const uint64_t a = {}, b = {}; {}{} = b == 0 ? 0 : a / b; }}", A, B, oe ? set_ov("b == 0") + " " : "", T));
        rec_int(T);
        return true;

    // ---- logical (destination rA, source rS)
    case Op::AND: line(std::format("{} = {} & {};", A, S, B)); rec_int(A); return true;
    case Op::ANDC: line(std::format("{} = {} & ~{};", A, S, B)); rec_int(A); return true;
    case Op::OR:
        if (rt_ == rb) {
            if (rt_ != ra) line(std::format("{} = {};", A, S)); // mr (or rX,rX,rX is a priority hint: no-op)
        } else {
            line(std::format("{} = {} | {};", A, S, B));
        }
        rec_int(A);
        return true;
    case Op::ORC: line(std::format("{} = {} | ~{};", A, S, B)); rec_int(A); return true;
    case Op::XOR: line(std::format("{} = {} ^ {};", A, S, B)); rec_int(A); return true;
    case Op::NOR: line(std::format("{} = ~({} | {});", A, S, B)); rec_int(A); return true;
    case Op::NAND: line(std::format("{} = ~({} & {});", A, S, B)); rec_int(A); return true;
    case Op::EQV: line(std::format("{} = ~({} ^ {});", A, S, B)); rec_int(A); return true;
    case Op::ORI:
        if (!(rt_ == ra && in.uimm() == 0)) line(std::format("{} = {} | {};", A, S, hex64(in.uimm())));
        return true;
    case Op::ORIS: line(std::format("{} = {} | {};", A, S, hex64(u64{in.uimm()} << 16))); return true;
    case Op::XORI: line(std::format("{} = {} ^ {};", A, S, hex64(in.uimm()))); return true;
    case Op::XORIS: line(std::format("{} = {} ^ {};", A, S, hex64(u64{in.uimm()} << 16))); return true;
    case Op::ANDI_: line(std::format("{} = {} & {};", A, S, hex64(in.uimm()))); line(rec0(A)); return true;
    case Op::ANDIS_: line(std::format("{} = {} & {};", A, S, hex64(u64{in.uimm()} << 16))); line(rec0(A)); return true;
    case Op::EXTSB: line(std::format("{} = (uint64_t)(int64_t)(int8_t){};", A, S)); rec_int(A); return true;
    case Op::EXTSH: line(std::format("{} = (uint64_t)(int64_t)(int16_t){};", A, S)); rec_int(A); return true;
    case Op::EXTSW: line(std::format("{} = (uint64_t)(int64_t)(int32_t){};", A, S)); rec_int(A); return true;
    case Op::CNTLZW: line(std::format("{} = ps_cntlzw({});", A, S)); rec_int(A); return true;
    case Op::CNTLZD: line(std::format("{} = ps_cntlzd({});", A, S)); rec_int(A); return true;

    // ---- shifts
    case Op::SLW: line(std::format("{} = (uint64_t)(uint32_t)({} << ({} & 63));", A, S, B)); rec_int(A); return true;
    case Op::SRW: line(std::format("{} = ({} & 0xffffffffull) >> ({} & 63);", A, S, B)); rec_int(A); return true;
    case Op::SLD:
        line(std::format("{{ const uint64_t n = {} & 0x7f; {} = (n & 0x40) ? 0 : {} << n; }}", B, A, S));
        rec_int(A);
        return true;
    case Op::SRD:
        line(std::format("{{ const uint64_t n = {} & 0x7f; {} = (n & 0x40) ? 0 : {} >> n; }}", B, A, S));
        rec_int(A);
        return true;
    case Op::SRAW:
        line(std::format("{{ const int32_t v = (int32_t){}; const uint32_t n = (uint32_t)({} & 63); "
                         "{} = (uint64_t)(int64_t)(n > 31 ? (v >> 31) : (v >> n)); "
                         "xer_ca = (uint8_t)(v < 0 && (n > 31 || ((uint32_t)v & ((1u << n) - 1u)) != 0)); }}",
            S, B, A));
        rec_int(A);
        return true;
    case Op::SRAD:
        line(std::format("{{ const int64_t v = (int64_t){}; const uint32_t n = (uint32_t)({} & 127); "
                         "{} = (uint64_t)(n > 63 ? (v >> 63) : (v >> n)); "
                         "xer_ca = (uint8_t)(v < 0 && (n > 63 || ((uint64_t)v & ((1ull << n) - 1ull)) != 0)); }}",
            S, B, A));
        rec_int(A);
        return true;
    case Op::SRAWI: {
        const u32 sh = in.sh32();
        line(std::format("{{ const int32_t v = (int32_t){}; {} = (uint64_t)(int64_t)(v >> {}); xer_ca = (uint8_t)(v < 0 && "
                         "((uint32_t)v & {}) != 0); }}",
            S, A, sh, hex32((1u << sh) - 1u)));
        rec_int(A);
        return true;
    }
    case Op::SRADI: {
        const u32 sh = in.sh64();
        line(std::format("{{ const int64_t v = (int64_t){}; {} = (uint64_t)(v >> {}); xer_ca = (uint8_t)(v < 0 && "
                         "((uint64_t)v & {}) != 0); }}",
            S, A, sh, hex64(sh ? (1ull << sh) - 1ull : 0)));
        rec_int(A);
        return true;
    }

    // ---- rotates
    case Op::RLWINM:
    case Op::RLWNM: {
        const u64 m = mask64(32 + in.mb32(), 32 + in.me32());
        const std::string amount = in.op == Op::RLWINM ? std::to_string(in.sh32()) : std::format("(uint32_t){} & 31", B);
        if ((m >> 32) == 0)
            line(std::format("{} = (uint64_t)(ps_rotl32((uint32_t){}, {}) & {});", A, S, amount, hex32(static_cast<u32>(m))));
        else
            line(std::format("{} = ps_dup32(ps_rotl32((uint32_t){}, {})) & {};", A, S, amount, hex64(m)));
        rec_int(A);
        return true;
    }
    case Op::RLWIMI: {
        const u64 m = mask64(32 + in.mb32(), 32 + in.me32());
        line(std::format("{0} = ({0} & {1}) | (ps_dup32(ps_rotl32((uint32_t){2}, {3})) & {4});", A, hex64(~m), S, in.sh32(), hex64(m)));
        rec_int(A);
        return true;
    }
    case Op::RLDICL:
        line(std::format("{} = {};", A, rot64_masked(S, in.sh64(), mask64(in.mb64(), 63))));
        rec_int(A);
        return true;
    case Op::RLDICR:
        line(std::format("{} = {};", A, rot64_masked(S, in.sh64(), mask64(0, in.mb64()))));
        rec_int(A);
        return true;
    case Op::RLDIC:
        line(std::format("{} = {};", A, rot64_masked(S, in.sh64(), mask64(in.mb64(), 63 - in.sh64()))));
        rec_int(A);
        return true;
    case Op::RLDIMI: {
        const u64 m = mask64(in.mb64(), 63 - in.sh64());
        line(std::format("{0} = ({0} & {1}) | (ps_rotl64({2}, {3}) & {4});", A, hex64(~m), S, in.sh64(), hex64(m)));
        rec_int(A);
        return true;
    }
    case Op::RLDCL:
        line(std::format("{} = ps_rotl64({}, (uint32_t){} & 63) & {};", A, S, B, hex64(mask64(in.mb64(), 63))));
        rec_int(A);
        return true;
    case Op::RLDCR:
        line(std::format("{} = ps_rotl64({}, (uint32_t){} & 63) & {};", A, S, B, hex64(mask64(0, in.mb64()))));
        rec_int(A);
        return true;

    // ---- compares
    case Op::CMPI:
    case Op::CMPLI:
    case Op::CMP:
    case Op::CMPL: {
        const bool is64 = in.l10() != 0;
        const bool logical = in.op == Op::CMPL || in.op == Op::CMPLI;
        const char* type = logical ? (is64 ? "uint64_t" : "uint32_t") : (is64 ? "int64_t" : "int32_t");
        std::string bval;
        if (in.op == Op::CMPI) bval = is64 ? std::format("(int64_t){}", si) : std::format("(int32_t){}", si);
        else if (in.op == Op::CMPLI) bval = is64 ? hex64(in.uimm()) : hex32(in.uimm());
        else bval = std::format("({}){}", type, B);
        const u32 f = in.crfd() * 4;
        line(std::format("{{ const {0} a = ({0}){1}, b = {2}; {3} = a < b; {4} = a > b; {5} = a == b; {6} = xer_so; }}", type, A,
            bval, CR(f), CR(f + 1), CR(f + 2), CR(f + 3)));
        return true;
    }

    // ---- CR logical
    case Op::CRAND: line(std::format("{} = {} & {};", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CRANDC: line(std::format("{} = {} & ({} ^ 1);", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CREQV: line(std::format("{} = ({} ^ {}) ^ 1;", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CRNAND: line(std::format("{} = ({} & {}) ^ 1;", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CRNOR: line(std::format("{} = ({} | {}) ^ 1;", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CROR: line(std::format("{} = {} | {};", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CRORC: line(std::format("{} = {} | ({} ^ 1);", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::CRXOR: line(std::format("{} = {} ^ {};", CR(in.crbd()), CR(in.crba()), CR(in.crbb()))); return true;
    case Op::MCRF:
        if (in.crfd() != in.crfs())
            for (u32 i = 0; i < 4; i++) line(std::format("{} = {};", CR(in.crfd() * 4 + i), CR(in.crfs() * 4 + i)));
        return true;

    // ---- CR / SPR transfers
    case Op::MFCR: {
        std::string v;
        for (u32 i = 0; i < 32; i++) v += std::format("{}((uint64_t){} << {})", i ? " | " : "", CR(i), 31 - i);
        line(std::format("{} = {};", T, v));
        return true;
    }
    case Op::MFOCRF: {
        const u32 n = static_cast<u32>(std::countl_zero(in.fxm()) - 24) & 7;
        std::string v;
        for (u32 i = 0; i < 4; i++) v += std::format("{}((uint64_t){} << {})", i ? " | " : "", CR(n * 4 + i), 31 - (n * 4 + i));
        line(std::format("{} = {};", T, v));
        return true;
    }
    case Op::MTCRF:
    case Op::MTOCRF:
        for (u32 n = 0; n < 8; n++) {
            const bool sel = in.op == Op::MTOCRF ? n == (static_cast<u32>(std::countl_zero(in.fxm())) & 7)
                                                 : (in.fxm() & (0x80u >> n)) != 0;
            if (!sel) continue;
            for (u32 i = 0; i < 4; i++) line(std::format("{} = (uint8_t)(({} >> {}) & 1);", CR(n * 4 + i), S, 31 - (n * 4 + i)));
        }
        return true;
    case Op::MFSPR:
    case Op::MFTB:
        switch (in.spr()) {
        case rt::kSprXer:
            line(std::format("{} = ((uint64_t)xer_so << 31) | ((uint64_t)xer_ov << 30) | ((uint64_t)xer_ca << 29) | xer_bc;", T));
            return true;
        case rt::kSprLr: line(std::format("{} = lr;", T)); return true;
        case rt::kSprCtr: line(std::format("{} = ctr;", T)); return true;
        case rt::kSprVrsave: line(std::format("{} = vrsave;", T)); return true;
        case rt::kSprTbl: line(std::format("{} = ps_ppu_timebase();", T)); return true;
        case rt::kSprTbu: line(std::format("{} = ps_ppu_timebase() >> 32;", T)); return true;
        default: return false;
        }
    case Op::MTSPR:
        switch (in.spr()) {
        case rt::kSprXer:
            line(std::format("xer_so = (uint8_t)(({0} >> 31) & 1); xer_ov = (uint8_t)(({0} >> 30) & 1); xer_ca = (uint8_t)(({0} >> 29) & 1); "
                             "xer_bc = (uint8_t)({0} & 0x7f);",
                S));
            return true;
        case rt::kSprLr: line(std::format("lr = {};", S)); return true;
        case rt::kSprCtr: line(std::format("ctr = {};", S)); return true;
        case rt::kSprVrsave: line(std::format("vrsave = (uint32_t){};", S)); return true;
        default: return false;
        }

    // ---- integer loads and stores
    case Op::LBZ: case Op::LHZ: case Op::LHA: case Op::LWZ:
        line(std::format("{} = {};", T, int_load(in.op, ea_d(in, in.simm()))));
        return true;
    case Op::LD: case Op::LWA:
        line(std::format("{} = {};", T, int_load(in.op, ea_d(in, in.ds()))));
        return true;
    case Op::LBZX: case Op::LHZX: case Op::LHAX: case Op::LWZX: case Op::LWAX: case Op::LDX:
    case Op::LHBRX: case Op::LWBRX: case Op::LDBRX:
        line(std::format("{} = {};", T, int_load(in.op, ea_x(in))));
        return true;
    case Op::LBZU: case Op::LHZU: case Op::LHAU: case Op::LWZU: case Op::LDU: {
        const s32 d = in.op == Op::LDU ? in.ds() : in.simm();
        line(std::format("{{ const uint64_t ea = {}; {} = {}; {} = ea; }}", add_const(A, d), T, int_load(in.op, "(uint32_t)ea"), A));
        return true;
    }
    case Op::LBZUX: case Op::LHZUX: case Op::LHAUX: case Op::LWZUX: case Op::LWAUX: case Op::LDUX:
        line(std::format("{{ const uint64_t ea = {} + {}; {} = {}; {} = ea; }}", A, B, T, int_load(in.op, "(uint32_t)ea"), A));
        return true;
    case Op::STB: case Op::STH: case Op::STW: line(int_store(in.op, ea_d(in, in.simm()), S)); return true;
    case Op::STD: line(int_store(in.op, ea_d(in, in.ds()), S)); return true;
    case Op::STBX: case Op::STHX: case Op::STWX: case Op::STDX: case Op::STHBRX: case Op::STWBRX: case Op::STDBRX:
        line(int_store(in.op, ea_x(in), S));
        return true;
    case Op::STBU: case Op::STHU: case Op::STWU: case Op::STDU: {
        const s32 d = in.op == Op::STDU ? in.ds() : in.simm();
        line(std::format("{{ const uint64_t ea = {}; {} {} = ea; }}", add_const(A, d), int_store(in.op, "(uint32_t)ea", S), A));
        return true;
    }
    case Op::STBUX: case Op::STHUX: case Op::STWUX: case Op::STDUX:
        line(std::format("{{ const uint64_t ea = {} + {}; {} {} = ea; }}", A, B, int_store(in.op, "(uint32_t)ea", S), A));
        return true;
    case Op::LMW: {
        std::string s = std::format("{{ const uint32_t ea = {}; ", ea_d(in, in.simm()));
        for (u32 r = rt_; r < 32; r++) s += std::format("{} = ps_read32(base, ea + {}u); ", G(r), (r - rt_) * 4);
        line(s + "}");
        return true;
    }
    case Op::STMW: {
        std::string s = std::format("{{ const uint32_t ea = {}; ", ea_d(in, in.simm()));
        for (u32 r = rt_; r < 32; r++) s += std::format("ps_write32(base, ea + {}u, (uint32_t){}); ", (r - rt_) * 4, G(r));
        line(s + "}");
        return true;
    }
    case Op::LSWI:
    case Op::STSWI: {
        const u32 bytes = in.nb() ? in.nb() : 32;
        std::string s = std::format("{{ const uint32_t ea = {}; ", ra ? std::format("(uint32_t){}", A) : std::string("0u"));
        for (u32 off = 0, reg = rt_; off < bytes; off += 4, reg = (reg + 1) & 31) {
            const u32 n = std::min(4u, bytes - off);
            if (in.op == Op::LSWI) {
                if (n == 4) {
                    s += std::format("{} = ps_read32(base, ea + {}u); ", G(reg), off);
                } else {
                    std::string v;
                    for (u32 k = 0; k < n; k++) v += std::format("{}((uint64_t)ps_read8(base, ea + {}u) << {})", k ? " | " : "", off + k, 24 - 8 * k);
                    s += std::format("{} = {}; ", G(reg), v);
                }
            } else {
                if (n == 4) {
                    s += std::format("ps_write32(base, ea + {}u, (uint32_t){}); ", off, G(reg));
                } else {
                    for (u32 k = 0; k < n; k++)
                        s += std::format("ps_write8(base, ea + {}u, (uint8_t)((uint32_t){} >> {})); ", off + k, G(reg), 24 - 8 * k);
                }
            }
        }
        line(s + "}");
        return true;
    }
    case Op::LWARX: line(std::format("{} = ps_ppu_lwarx(ctx, base, {});", T, ea_x(in))); return true;
    case Op::LDARX: line(std::format("{} = ps_ppu_ldarx(ctx, base, {});", T, ea_x(in))); return true;
    case Op::STWCX_:
    case Op::STDCX_:
        line(std::format("{{ const int ok = {}(ctx, base, {}, {}{}); cr0_lt = 0; cr0_gt = 0; cr0_eq = (uint8_t)(ok != 0); cr0_so = xer_so; }}",
            in.op == Op::STWCX_ ? "ps_ppu_stwcx" : "ps_ppu_stdcx", ea_x(in), in.op == Op::STWCX_ ? "(uint32_t)" : "", S));
        return true;
    case Op::DCBZ:
        line(std::format("{{ const uint32_t line = {} & ~127u; for (uint32_t i = 0; i < 128; i += 8) ps_write64(base, line + i, 0); }}",
            ea_x(in)));
        return true;
    case Op::DCBT: case Op::DCBTST: case Op::DCBF: case Op::DCBST: case Op::DCBI: case Op::ICBI:
    case Op::DST: case Op::DSTST: case Op::DSS: line("/* cache hint: no-op */"); return true;
    case Op::SYNC: case Op::EIEIO: line("PS_PPU_SYNC();"); return true;
    case Op::ISYNC: line("PS_PPU_ISYNC();"); return true;

    // ---- FP loads and stores
    case Op::LFS: case Op::LFD: line(fp_load(in.op, rt_, ea_d(in, in.simm()))); return true;
    case Op::LFSX: case Op::LFDX: line(fp_load(in.op, rt_, ea_x(in))); return true;
    case Op::LFSU: case Op::LFDU:
        line(std::format("{{ const uint64_t ea = {}; {} {} = ea; }}", add_const(A, in.simm()), fp_load(in.op, rt_, "(uint32_t)ea"), A));
        return true;
    case Op::LFSUX: case Op::LFDUX:
        line(std::format("{{ const uint64_t ea = {} + {}; {} {} = ea; }}", A, B, fp_load(in.op, rt_, "(uint32_t)ea"), A));
        return true;
    case Op::STFS: case Op::STFD: line(fp_store(in.op, rt_, ea_d(in, in.simm()))); return true;
    case Op::STFSX: case Op::STFDX: case Op::STFIWX: line(fp_store(in.op, rt_, ea_x(in))); return true;
    case Op::STFSU: case Op::STFDU:
        line(std::format("{{ const uint64_t ea = {}; {} {} = ea; }}", add_const(A, in.simm()), fp_store(in.op, rt_, "(uint32_t)ea"), A));
        return true;
    case Op::STFSUX: case Op::STFDUX:
        line(std::format("{{ const uint64_t ea = {} + {}; {} {} = ea; }}", A, B, fp_store(in.op, rt_, "(uint32_t)ea"), A));
        return true;

    // ---- FP arithmetic
    case Op::FADD: line(std::format("{} = {} + {};", F(rt_), F(ra), F(rb))); rec_fp(); return true;
    case Op::FSUB: line(std::format("{} = {} - {};", F(rt_), F(ra), F(rb))); rec_fp(); return true;
    case Op::FMUL: line(std::format("{} = {} * {};", F(rt_), F(ra), F(in.rc_reg()))); rec_fp(); return true;
    case Op::FDIV: line(std::format("{} = {} / {};", F(rt_), F(ra), F(rb))); rec_fp(); return true;
    case Op::FSQRT: line(std::format("{} = __builtin_sqrt({});", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FADDS: line(std::format("{} = ps_ppu_round_single({} + {});", F(rt_), F(ra), F(rb))); rec_fp(); return true;
    case Op::FSUBS: line(std::format("{} = ps_ppu_round_single({} - {});", F(rt_), F(ra), F(rb))); rec_fp(); return true;
    case Op::FMULS: line(std::format("{} = ps_ppu_round_single({} * {});", F(rt_), F(ra), F(in.rc_reg()))); rec_fp(); return true;
    case Op::FDIVS: line(std::format("{} = ps_ppu_round_single({} / {});", F(rt_), F(ra), F(rb))); rec_fp(); return true;
    case Op::FSQRTS: line(std::format("{} = ps_ppu_round_single(__builtin_sqrt({}));", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FMADD: case Op::FMSUB: case Op::FNMADD: case Op::FNMSUB:
    case Op::FMADDS: case Op::FMSUBS: case Op::FNMADDS: case Op::FNMSUBS: {
        const bool negate = in.op == Op::FNMADD || in.op == Op::FNMSUB || in.op == Op::FNMADDS || in.op == Op::FNMSUBS;
        const bool sub = in.op == Op::FMSUB || in.op == Op::FNMSUB || in.op == Op::FMSUBS || in.op == Op::FNMSUBS;
        const bool single = in.op == Op::FMADDS || in.op == Op::FMSUBS || in.op == Op::FNMADDS || in.op == Op::FNMSUBS;
        std::string e = std::format("{}__builtin_fma({}, {}, {}{})", negate ? "-" : "", F(ra), F(in.rc_reg()), sub ? "-" : "", F(rb));
        if (single) e = std::format("ps_ppu_round_single({})", e);
        line(std::format("{} = {};", F(rt_), e));
        rec_fp();
        return true;
    }
    case Op::FSEL: line(std::format("{} = {} >= 0.0 ? {} : {};", F(rt_), F(ra), F(in.rc_reg()), F(rb))); rec_fp(); return true;
    case Op::FRES: line(std::format("{} = ps_ppu_fres({});", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FRSQRTE: line(std::format("{} = ps_ppu_frsqrte({});", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FMR:
        if (rt_ != rb) line(std::format("{} = {};", F(rt_), F(rb)));
        rec_fp();
        return true;
    case Op::FNEG: line(std::format("{} = ps_f64_from_bits(ps_f64_bits({}) ^ 0x8000000000000000ull);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FABS: line(std::format("{} = ps_f64_from_bits(ps_f64_bits({}) & 0x7fffffffffffffffull);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FNABS: line(std::format("{} = ps_f64_from_bits(ps_f64_bits({}) | 0x8000000000000000ull);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FRSP: line(std::format("{} = ps_ppu_round_single({});", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FCTIW: line(std::format("{} = ps_ppu_fctiw({}, fpscr & 3u);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FCTIWZ: line(std::format("{} = ps_ppu_fctiw({}, PS_FPSCR_RN_ZERO);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FCTID: line(std::format("{} = ps_ppu_fctid({}, fpscr & 3u);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FCTIDZ: line(std::format("{} = ps_ppu_fctid({}, PS_FPSCR_RN_ZERO);", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FCFID: line(std::format("{} = ps_ppu_fcfid({});", F(rt_), F(rb))); rec_fp(); return true;
    case Op::FCMPU:
    case Op::FCMPO: {
        const u32 f = in.crfd() * 4;
        line(std::format("{{ const double a = {}, b = {}; {} = a < b; {} = a > b; {} = a == b; {} = (uint8_t)__builtin_isunordered(a, b); "
                         "fpscr = (fpscr & ~0xf000u) | ((uint32_t){} << 15) | ((uint32_t){} << 14) | ((uint32_t){} << 13) | ((uint32_t){} << 12); }}",
            F(ra), F(rb), CR(f), CR(f + 1), CR(f + 2), CR(f + 3), CR(f), CR(f + 1), CR(f + 2), CR(f + 3)));
        return true;
    }
    case Op::MFFS: line(std::format("{} = ps_f64_from_bits((uint64_t)fpscr);", F(rt_))); rec_fp(); return true;
    case Op::MTFSF: line(std::format("fpscr = ps_fpscr_mtfsf(fpscr, {}u, ps_f64_bits({}));", in.flm(), F(rb))); rec_fp(); return true;
    case Op::MTFSFI: line(std::format("fpscr = ps_fpscr_mtfsfi(fpscr, {}u, {}u);", in.crfd(), in.fpimm())); rec_fp(); return true;
    case Op::MTFSB0: line(std::format("fpscr = ps_fpscr_mtfsb0(fpscr, {}u);", in.crbd())); rec_fp(); return true;
    case Op::MTFSB1: line(std::format("fpscr = ps_fpscr_mtfsb1(fpscr, {}u);", in.crbd())); rec_fp(); return true;
    case Op::MCRFS: {
        const u32 f = in.crfd() * 4;
        line(std::format("{{ const uint32_t v = ps_fpscr_field(fpscr, {}u); {} = (uint8_t)(v >> 3); {} = (uint8_t)((v >> 2) & 1); "
                         "{} = (uint8_t)((v >> 1) & 1); {} = (uint8_t)(v & 1); fpscr = ps_fpscr_mcrfs_clear(fpscr, {}u); }}",
            in.crfs(), CR(f), CR(f + 1), CR(f + 2), CR(f + 3), in.crfs()));
        return true;
    }
    default: return false;
    }
}

// ------------------------------------------------------------------------------------------ control flow

std::string sym(const Gen& g, u32 addr) { return function_symbol(g.opt ? g.opt->prefix : std::string{}, addr); }

// Unconditional transfer to target (normal mode).
std::string transfer(Gen& g, u32 target)
{
    if (g.fn->labels.contains(target)) return std::format("goto L_{:08X};", target);
    if (g.program->entries.contains(target)) return std::format("{{ {}{}(ctx, base); return; }}", g.spill, sym(g, target));
    g.uses_entry_state = true;
    return std::format("{{ {}ps_ppu_jump_indirect(ctx, base, {}, entry_lr, entry_sp); return; }}", g.spill, hex32(target));
}

std::string call(Gen& g, u32 target)
{
    if (g.program->entries.contains(target)) return std::format("{}{}(ctx, base); {}", g.spill, sym(g, target), g.reload);
    return std::format("{}ps_ppu_call_indirect(ctx, base, {}); {}", g.spill, hex32(target), g.reload);
}

std::string guarded(const std::string& cond, const std::string& stmt)
{
    if (cond.empty()) return stmt;
    return std::format("if ({}) {{ {} }}", cond, stmt);
}

void emit_control(Gen& g, u32 pc, const Inst& in, std::string& o)
{
    auto line = [&](const std::string& s) {
        if (!s.empty()) o += "    " + s + "\n";
    };
    const std::string link = hex64(u64{pc} + 4);
    switch (in.op) {
    case Op::B: {
        const u32 target = rt::branch_target(in, pc);
        if (g.step) {
            line(std::format("{}next = {};", in.lk() ? std::format("lr = {}; ", link) : "", hex32(target)));
        } else if (in.lk()) {
            line(std::format("lr = {};", link));
            if (target != pc + 4) line(call(g, target));
        } else {
            line(transfer(g, target));
        }
        return;
    }
    case Op::BC: {
        const u32 target = rt::branch_target(in, pc);
        const bool dec = !(in.bo() & 0x04);
        const std::string cond = cond_expr(in.bo(), in.bi(), true);
        if (dec) line("ctr -= 1;");
        if (in.lk()) line(std::format("lr = {};", link));
        if (g.step) {
            line(guarded(cond, std::format("next = {};", hex32(target))));
        } else if (in.lk()) {
            if (target != pc + 4) line(guarded(cond, call(g, target)));
        } else {
            line(guarded(cond, transfer(g, target)));
        }
        return;
    }
    case Op::BCLR: {
        const bool dec = !(in.bo() & 0x04);
        const std::string cond = cond_expr(in.bo(), in.bi(), true);
        if (in.lk() || g.step) {
            std::string s = "{ const uint32_t t = (uint32_t)lr & ~3u; ";
            if (dec) s += "ctr -= 1; ";
            if (in.lk()) s += std::format("lr = {}; ", link);
            if (g.step)
                s += guarded(cond, "next = t;");
            else
                s += guarded(cond, std::format("{}ps_ppu_call_indirect(ctx, base, t); {}", g.spill, g.reload));
            line(s + " }");
        } else {
            if (dec) line("ctr -= 1;");
            line(guarded(cond, std::format("{}return;", g.spill)));
        }
        return;
    }
    case Op::BCCTR: {
        const std::string cond = cond_expr(in.bo(), in.bi(), false); // bcctr never tests or decrements CTR
        if (in.lk()) line(std::format("lr = {};", link));
        if (g.step) {
            line(guarded(cond, "next = (uint32_t)ctr & ~3u;"));
            return;
        }
        if (in.lk()) {
            line(guarded(cond, std::format("{}ps_ppu_call_indirect(ctx, base, (uint32_t)ctr & ~3u); {}", g.spill, g.reload)));
            return;
        }
        g.uses_entry_state = true;
        if (const auto jt = g.fn->jump_tables.find(pc); jt != g.fn->jump_tables.end() && cond.empty()) {
            std::vector<u32> targets = jt->second.targets;
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            std::string s = std::format("switch ((uint32_t)ctr & ~3u) {{ /* jump table at {:#010x}, {} entries */\n", jt->second.table,
                jt->second.targets.size());
            for (u32 t : targets) s += std::format("    case {}: goto L_{:08X};\n", hex32(t), t);
            s += std::format("    default: {}ps_ppu_jump_table_miss(ctx, base, {}, (uint32_t)ctr & ~3u, entry_lr, entry_sp); return;\n    }}",
                g.spill, hex32(pc));
            line(s);
            if (g.stats) g.stats->jump_tables++;
            return;
        }
        if (g.stats) g.stats->indirect_jumps++;
        line(guarded(cond, std::format("{}ps_ppu_jump_indirect(ctx, base, (uint32_t)ctr & ~3u, entry_lr, entry_sp); return;", g.spill)));
        return;
    }
    case Op::SC: line(std::format("{}ctx->cia = {}; ps_ppu_syscall(ctx, base); {}", g.spill, hex32(pc), g.reload)); return;
    case Op::TW:
    case Op::TWI:
    case Op::TD:
    case Op::TDI: {
        const bool is64 = in.op == Op::TD || in.op == Op::TDI;
        const std::string b = (in.op == Op::TWI || in.op == Op::TDI) ? sext64(in.simm()) : G(in.rb());
        const std::string vals = is64
            ? std::format("const int64_t a = (int64_t){0}, b = (int64_t){1}; const uint64_t au = {0}, bu = {1}; ", G(in.ra()), b)
            : std::format("const int32_t a = (int32_t){0}, b = (int32_t){1}; const uint32_t au = (uint32_t){0}, bu = (uint32_t){1}; ",
                  G(in.ra()), b);
        const std::string fire = std::format("{}ctx->cia = {}; ps_ppu_trap(ctx, base, {}); {}", g.spill, hex32(pc), hex32(pc), g.reload);
        if (!g.step && rt::is_unconditional_trap(in)) {
            // Nothing after an unconditional trap was lifted: if the handler returns, interpret the rest.
            g.uses_entry_state = true;
            line(std::format("{}ctx->cia = {}; ps_ppu_trap(ctx, base, {}); ps_ppu_interp_until(ctx, base, {}, entry_lr, entry_sp); return;",
                g.spill, hex32(pc), hex32(pc), hex32(pc + 4)));
            return;
        }
        line(std::format("{{ {}if ({}) {{ {} }} }}", vals, trap_cond(in.to()), fire));
        return;
    }
    default: break;
    }
}

bool is_control(Op op)
{
    switch (op) {
    case Op::B: case Op::BC: case Op::BCLR: case Op::BCCTR: case Op::SC:
    case Op::TW: case Op::TWI: case Op::TD: case Op::TDI: return true;
    default: return false;
    }
}

// Emits one instruction (any kind) at pc.
void emit_instruction(Gen& g, u32 pc, const Inst& in, std::string& o)
{
    if (in.op == Op::UNKNOWN) {
        o += std::format("    {}ps_ppu_illegal(ctx, base, {});\n", g.spill, hex32(pc));
        if (g.stats) g.stats->illegal++;
        return;
    }
    if (!rt::is_supported(in)) {
        o += std::format("    {}ps_ppu_interp_one(ctx, base, {}); {}\n", g.spill, hex32(pc), g.reload);
        if (g.stats) g.stats->fallbacks++;
        return;
    }
    if (is_control(in.op)) {
        emit_control(g, pc, in, o);
        return;
    }
    if (in.op == Op::LSWX || in.op == Op::STSWX) {
        o += std::format("    {{ const uint32_t ea = {}; {}{}(ctx, base, ea, {}u); {}}}\n", ea_x(in), g.spill,
            in.op == Op::LSWX ? "ps_ppu_lswx" : "ps_ppu_stswx", in.rt(), g.reload);
        return;
    }
    if (!emit_semantics(in, o)) throw std::logic_error(std::format("lifter: no semantics for {}", in.info().mnemonic));
}

bool falls_through(const Inst& in)
{
    if (in.op == Op::UNKNOWN || rt::is_unconditional_trap(in)) return false;
    switch (in.op) {
    case Op::B: return in.lk() != 0;
    case Op::BC: case Op::BCLR: return in.lk() || (in.bo() & 0x14) != 0x14;
    case Op::BCCTR: return in.lk() || !(in.bo() & 0x10);
    default: return true;
    }
}

std::string comment(u32 pc, const Inst& in)
{
    std::string text = rt::disassemble(in, pc);
    // Keep comments well-formed whatever the disassembly contains.
    for (usize p; (p = text.find("*/")) != std::string::npos;) text.replace(p, 2, "* /");
    return std::format("    /* {:08x}: {:08x}  {} */\n", pc, in.raw, text);
}

std::string signature(const std::string& symbol) { return std::format("void {}(PPUContext* PS_RESTRICT ctx, uint8_t* base)", symbol); }

std::string emit_function(const CodeImage& image, const Program& prog, const Function& fn, const LiftOptions& opt, LiftStats& stats)
{
    Gen g;
    g.image = &image;
    g.program = &prog;
    g.fn = &fn;
    g.opt = &opt;
    g.stats = &stats;

    std::vector<Inst> insts;
    insts.reserve(fn.insns.size());
    for (u32 pc : fn.insns) {
        insts.push_back(rt::decode(image.read32(pc).value_or(0)));
        add_use(g, rt::reg_use(insts.back()));
    }
    build_spill_reload(g);

    std::string body;
    for (usize i = 0; i < fn.insns.size(); i++) {
        const u32 pc = fn.insns[i];
        const Inst& in = insts[i];
        if (fn.labels.contains(pc)) body += std::format("L_{:08X}: ;\n", pc);
        if (opt.comments) body += comment(pc, in);
        emit_instruction(g, pc, in, body);
        stats.instructions++;
        if (!falls_through(in)) continue;
        const u32 next = pc + 4;
        const bool next_emitted = i + 1 < fn.insns.size() && fn.insns[i + 1] == next;
        if (next_emitted) continue;
        // Flow leaves this function's code: into another function (tail call) or somewhere unknown.
        if (prog.entries.contains(next)) {
            body += std::format("    {}{}(ctx, base); return; /* falls through into {:#010x} */\n", g.spill, sym(g, next), next);
        } else {
            g.uses_entry_state = true;
            body += std::format("    {}ps_ppu_jump_indirect(ctx, base, {}, entry_lr, entry_sp); return;\n", g.spill, hex32(next));
        }
    }

    std::string o = std::format("{}\n{{\n", signature(function_symbol(opt.prefix, fn.entry)));
    o += "    base = (uint8_t*)__builtin_assume_aligned(base, PS_PPU_BASE_ALIGN);\n";
    o += declarations(g);
    if (g.uses_entry_state) o += "    const uint64_t entry_lr = ctx->lr, entry_sp = ctx->gpr[1];\n";
    o += body;
    o += "}\n\n";
    stats.functions++;
    return o;
}

constexpr std::string_view kBanner = "/* Generated by ppu-lift (pspocket recomp/ppu). Do not edit. */\n";

} // namespace

std::string function_symbol(std::string_view prefix, u32 addr) { return std::format("{}f_{:08X}", prefix, addr); }

std::string lift_step_function(std::string_view symbol, u32 insn, u32 pc)
{
    const Inst in = rt::decode(insn);
    if (!rt::is_supported(in)) throw std::invalid_argument(std::format("lift_step_function: unsupported instruction {:08x}", insn));
    Gen g;
    g.step = true;
    add_use(g, rt::reg_use(in));
    build_spill_reload(g);

    std::string body;
    body += comment(pc, in);
    emit_instruction(g, pc, in, body);

    std::string o = std::format("{}\n{{\n", signature(std::string(symbol)));
    o += "    base = (uint8_t*)__builtin_assume_aligned(base, PS_PPU_BASE_ALIGN);\n";
    o += declarations(g);
    o += std::format("    uint32_t next = {};\n", hex32(pc + 4));
    o += body;
    o += std::format("    {}ctx->cia = next;\n}}\n", g.spill);
    return o;
}

LiftResult lift(const CodeImage& image, const Program& program, const LiftOptions& options)
{
    LiftResult result;
    const std::string& name = options.name;
    const u32 tus = std::max(1u, options.tu_count);

    // Header
    {
        std::string h(kBanner);
        h += std::format("#ifndef {0}_H\n#define {0}_H\n\n#include \"pspocket/ppu/lifted.h\"\n\n#ifdef __cplusplus\nextern \"C\" {{\n#endif\n\n", name);
        for (const Function& f : program.functions) h += signature(function_symbol(options.prefix, f.entry)) + ";\n";
        h += std::format("\n/* Registers / unregisters every function above in the dispatch table (ps_ppu_dispatch_*). */\n"
                         "void {0}_register(void);\nvoid {0}_unregister(void);\nextern const uint32_t {0}_function_count;\n\n"
                         "#ifdef __cplusplus\n}}\n#endif\n\n#endif\n",
            name);
        result.files.push_back({name + ".h", std::move(h)});
    }

    // Translation units: contiguous runs of functions with balanced instruction counts.
    usize total = 0;
    for (const Function& f : program.functions) total += f.insns.size();
    const usize per_tu = total / tus + 1;
    std::vector<std::string> units(tus);
    usize unit = 0, in_unit = 0;
    for (const Function& f : program.functions) {
        if (in_unit >= per_tu && unit + 1 < tus) {
            unit++;
            in_unit = 0;
        }
        units[unit] += emit_function(image, program, f, options, result.stats);
        in_unit += f.insns.size();
    }
    for (u32 i = 0; i < tus; i++) {
        std::string c(kBanner);
        c += std::format("#include \"{}.h\"\n\n", name);
        c += units[i];
        result.files.push_back({std::format("{}_{}.c", name, i), std::move(c)});
    }

    // Registration unit
    {
        std::string r(kBanner);
        r += std::format("#include \"{}.h\"\n\n", name);
        const usize n = program.functions.size();
        r += std::format("const uint32_t {}_function_count = {}u;\n\n", name, n);
        if (n) {
            r += std::format("static const ps_ppu_dispatch_entry {}_entries[{}] = {{\n", name, n);
            for (const Function& f : program.functions) r += std::format("    {{{}, {}}},\n", hex32(f.entry), function_symbol(options.prefix, f.entry));
            r += "};\n\n";
            r += std::format("void {0}_register(void) {{ ps_ppu_dispatch_register_table({0}_entries, {1}u); }}\n\n", name, n);
            r += std::format("void {0}_unregister(void)\n{{\n    for (uint32_t i = 0; i < {1}u; i++) ps_ppu_dispatch_unregister({0}_entries[i].addr);\n}}\n",
                name, n);
        } else {
            r += std::format("void {0}_register(void) {{}}\nvoid {0}_unregister(void) {{}}\n", name);
        }
        result.files.push_back({name + "_reg.c", std::move(r)});
    }
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
        const std::filesystem::path path = std::filesystem::path(dir) / f.name;
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::ostringstream old;
                old << in.rdbuf();
                if (old.str() == f.content) continue; // unchanged: keep the timestamp, avoid rebuilds
            }
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << f.content;
        if (!out) {
            error = "cannot write " + path.string();
            return false;
        }
    }
    return true;
}

} // namespace pspocket::recomp::ppu
