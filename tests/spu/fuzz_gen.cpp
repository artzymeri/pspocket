// spu-fuzz-gen: writes the SPU differential fuzz cases for every instruction the interpreter supports.
//
//   spu-fuzz-gen --out DIR --cases K --tus N
//
// For each supported op it draws K random valid encodings (K per floating-point model for the ops whose result
// depends on it), lifts each into a step function with the real lifter (lift_step_function: the same code
// generation as spu-lift) and records overrides that steer operands to their edges (shift counts, conditions, DMA
// parameters in the guest window, reservations). Every fourth case is lifted relocatable (D-081) and runs at another
// address of the code window. A small synthetic program is also lifted whole (lift(), relocatable and fixed) for the
// program-level test: DIR/ps_spu_fzp*.c, DIR/ps_spu_fzq*.c and its image in DIR/fuzz_program.c. Output: DIR/fuzz_lifted_<i>.c and DIR/fuzz_cases.c. The generator
// seed is fixed, so the output is stable.

#include <bit>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "fuzz_case.h"
#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu/lifter.hpp"
#include "pspocket/spu/decoder.hpp"
#include "pspocket/spu/interpreter.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
namespace as = pspocket::recomp::spu::as;
using test::Rng;

namespace {

struct GenCase {
    Op op;
    u8 xfloat;
    u32 insn;
    u32 pc;    // where it is lifted
    u32 reloc; // relocatable cases (D-081) run reloc bytes past pc; 0 for the others
    std::vector<ps_spu_fuzz_override> overrides;
};

constexpr u32 kReadChannels[] = {PS_SPU_RdEventStat, PS_SPU_RdSigNotify1, PS_SPU_RdSigNotify2, PS_SPU_RdDec,
    PS_SPU_RdEventMask, PS_MFC_RdTagMask, PS_SPU_RdMachStat, PS_SPU_RdSRR0, PS_MFC_RdTagStat, PS_MFC_RdListStallStat,
    PS_MFC_RdAtomicStat, PS_SPU_RdInMbox};
constexpr u32 kWriteChannels[] = {PS_SPU_WrEventMask, PS_SPU_WrEventAck, PS_SPU_WrDec, PS_MFC_WrMSSyncReq,
    PS_SPU_WrSRR0, PS_MFC_LSA, PS_MFC_EAH, PS_MFC_EAL, PS_MFC_Size, PS_MFC_TagID, PS_MFC_Cmd, PS_MFC_Cmd, PS_MFC_Cmd,
    PS_MFC_Cmd, PS_MFC_WrTagMask, PS_MFC_WrTagUpdate, PS_MFC_WrListStallAck, PS_SPU_WrOutMbox, PS_SPU_WrOutIntrMbox,
    PS_SPU_Set_Bkmk_Tag, PS_SPU_PM_Start_Ev};
constexpr u32 kCountChannels[] = {PS_SPU_WrOutMbox, PS_SPU_WrOutIntrMbox, PS_SPU_RdInMbox, PS_MFC_RdTagStat,
    PS_MFC_RdListStallStat, PS_SPU_RdSigNotify1, PS_SPU_RdSigNotify2, PS_MFC_RdAtomicStat, PS_SPU_RdEventStat,
    PS_MFC_Cmd, PS_MFC_WrTagUpdate, PS_SPU_WrDec, PS_MFC_LSA};
constexpr u32 kMfcCommands[] = {PS_MFC_GET, PS_MFC_GETB, PS_MFC_GETF, PS_MFC_PUT, PS_MFC_PUTB, PS_MFC_PUTF, PS_MFC_PUTR,
    PS_MFC_GETL, PS_MFC_GETLB, PS_MFC_PUTL, PS_MFC_PUTLF, PS_MFC_GETLLAR, PS_MFC_GETLLAR, PS_MFC_PUTLLC, PS_MFC_PUTLLC,
    PS_MFC_PUTLLUC, PS_MFC_PUTQLLUC, PS_MFC_BARRIER, PS_MFC_EIEIO, PS_MFC_SYNC, PS_MFC_SNDSIG, PS_MFC_SDCRZ,
    PS_MFC_SDCRT, 0x99};
constexpr u32 kSizes[] = {0, 1, 2, 4, 8, 16, 32, 48, 64, 128, 256, 1024, 4096, 16384};

class Generator {
public:
    explicit Generator(u64 seed) : rng_(seed) {}

    GenCase make(Op op, u8 xfloat, bool relocated)
    {
        GenCase c{op, xfloat, 0, 0, 0, {}};
        ov_ = &c.overrides;
        const OpInfo& info = op_info(op);
        c.pc = PS_SPU_FUZZ_CODE_BASE + rng_.below(PS_SPU_FUZZ_CODE_SIZE / 4) * 4;
        if (relocated) { // runs elsewhere in the code window, before or after where it was lifted
            const u32 run_at = PS_SPU_FUZZ_CODE_BASE + rng_.below(PS_SPU_FUZZ_CODE_SIZE / 4) * 4;
            c.reloc = (run_at - c.pc) & PS_SPU_LS_MASK;
        }

        as::Fields f;
        f.rt = reg();
        f.ra = reg();
        f.rb = reg();
        f.rc = reg();
        if (rng_.chance(10)) f.ra = f.rt; // aliasing between target and sources
        if (rng_.chance(10)) f.rb = f.ra;
        f.imm = static_cast<s32>(rng_.next());

        switch (info.fmt) {
        case Fmt::BI: {
            const u32 k = rng_.below(100);
            f.e = k < 15;
            f.d = k >= 15 && k < 30;
            if (rng_.chance(50)) pref(f.rt, rng_.chance(50) ? 0 : static_cast<u32>(rng_.next())); // branch conditions
            if (op == Op::BIHZ || op == Op::BIHNZ) {
                if (rng_.chance(50)) pref(f.rt, static_cast<u32>(rng_.next()) & 0xFFFF0000u);
            }
            break;
        }
        case Fmt::RI16_BR:
            if (rng_.chance(50)) pref(f.rt, rng_.chance(50) ? 0 : static_cast<u32>(rng_.next()));
            if ((op == Op::BRHZ || op == Op::BRHNZ) && rng_.chance(50)) pref(f.rt, static_cast<u32>(rng_.next()) & 0xFFFF0000u);
            break;
        case Fmt::STOP: f.imm = static_cast<s32>(rng_.below(0x4000)); break;
        case Fmt::CH_READ:
            if (op == Op::RCHCNT)
                f.ra = rng_.chance(90) ? pick(kCountChannels) : rng_.below(128);
            else
                f.ra = rng_.chance(92) ? pick(kReadChannels) : rng_.below(128);
            break;
        case Fmt::CH_WRITE:
            f.ra = rng_.chance(92) ? pick(kWriteChannels) : rng_.below(128);
            channel_write(f.ra, f.rt);
            break;
        case Fmt::RI8: f.imm = static_cast<s32>(rng_.chance(70) ? 120 + rng_.below(80) : rng_.below(256)); break;
        default: break;
        }
        edges(op, f);

        c.insn = as::encode(op, f);
        const Inst check = decode(c.insn);
        if (check.op != op || !is_supported(check)) {
            std::fprintf(stderr, "spu-fuzz-gen: bad encoding %08x for %s (decodes as %s)\n", c.insn, info.mnemonic,
                op_info(check.op).mnemonic);
            std::exit(1);
        }
        return c;
    }

private:
    u32 reg() { return rng_.chance(30) ? rng_.below(8) : rng_.below(128); }
    template <class T, usize N>
    u32 pick(const T (&list)[N])
    {
        return static_cast<u32>(list[rng_.below(N)]);
    }
    void add(u8 kind, u32 index, u64 value, u64 value2 = 0)
    {
        ov_->push_back(ps_spu_fuzz_override{kind, static_cast<u8>(index), {}, value, value2});
    }
    void pref(u32 r, u32 v)
    {
        for (auto& o : *ov_)
            if (o.kind == PS_SPU_FZ_PREF && o.index == r) {
                o.value = v;
                return;
            }
        add(PS_SPU_FZ_PREF, r, v);
    }
    // Four small signed counts (shift edges around 0, 16, 32, 64).
    void small_counts(u32 r, s32 range)
    {
        u64 lanes[2] = {};
        for (u32 i = 0; i < 4; i++) {
            const u32 v = static_cast<u32>(static_cast<s32>(rng_.below(2 * range + 1)) - range);
            lanes[i / 2] |= u64{v} << (32 * (i % 2));
        }
        add(PS_SPU_FZ_REG, r, lanes[0], lanes[1]);
    }

    void edges(Op op, as::Fields& f)
    {
        switch (op) {
        case Op::SHL:
        case Op::ROT:
        case Op::ROTM:
        case Op::ROTMA:
            if (rng_.chance(60)) small_counts(f.rb, 70);
            break;
        case Op::SHLH:
        case Op::ROTH:
        case Op::ROTHM:
        case Op::ROTMAH:
            if (rng_.chance(60)) {
                // halfword counts in both halves of each word
                u64 lanes[2] = {};
                for (u32 i = 0; i < 8; i++) {
                    const u64 v = static_cast<u16>(static_cast<s32>(rng_.below(81)) - 40);
                    lanes[i / 4] |= v << (16 * (i % 4));
                }
                add(PS_SPU_FZ_REG, f.rb, lanes[0], lanes[1]);
            }
            break;
        case Op::SHLQBI:
        case Op::ROTQBI:
        case Op::ROTQMBI:
            if (rng_.chance(60)) pref(f.rb, static_cast<u32>(static_cast<s32>(rng_.below(31)) - 15));
            break;
        case Op::SHLQBY:
        case Op::ROTQBY:
        case Op::ROTQMBY:
            if (rng_.chance(60)) pref(f.rb, static_cast<u32>(static_cast<s32>(rng_.below(81)) - 40));
            break;
        case Op::SHLQBYBI:
        case Op::ROTQBYBI:
        case Op::ROTQMBYBI:
            if (rng_.chance(60)) pref(f.rb, static_cast<u32>(static_cast<s32>(rng_.below(641)) - 320));
            break;
        case Op::HEQ:
        case Op::HGT:
        case Op::HLGT:
            if (rng_.chance(50) && f.ra != f.rb) {
                const u32 v = static_cast<u32>(rng_.next());
                pref(f.ra, v);
                pref(f.rb, v + rng_.below(3) - 1);
            }
            break;
        case Op::HEQI:
        case Op::HGTI:
        case Op::HLGTI:
            if (rng_.chance(50)) {
                const s32 imm = static_cast<s32>(static_cast<u32>(f.imm) << 22) >> 22;
                pref(f.ra, static_cast<u32>(imm + static_cast<s32>(rng_.below(3)) - 1));
            }
            break;
        case Op::CBD:
        case Op::CHD:
        case Op::CWD:
        case Op::CDD:
        case Op::CBX:
        case Op::CHX:
        case Op::CWX:
        case Op::CDX:
            if (rng_.chance(50)) pref(f.ra, rng_.below(64));
            break;
        default: break;
        }
    }

    void channel_write(u32 channel, u32 rt)
    {
        switch (channel) {
        case PS_MFC_Cmd: {
            const u32 cmd = pick(kMfcCommands);
            pref(rt, cmd | (rng_.chance(20) ? (rng_.below(256) << 8) : 0)); // class id bits are ignored
            mfc_params(cmd);
            break;
        }
        case PS_MFC_WrTagUpdate: pref(rt, rng_.chance(90) ? rng_.below(3) : rng_.below(8)); break;
        case PS_MFC_WrListStallAck: pref(rt, rng_.below(32)); break;
        case PS_SPU_WrEventMask:
        case PS_SPU_WrEventAck: pref(rt, static_cast<u32>(rng_.next()) & (rng_.chance(80) ? 0x1FFFu : ~0u)); break;
        default: break;
        }
    }

    void mfc_params(u32 cmd)
    {
        const u32 tag = rng_.below(32);
        u32 lsa = static_cast<u32>(rng_.next()) & PS_SPU_LS_MASK;
        u32 eal = PS_SPU_FUZZ_GUEST_BASE + rng_.below(PS_SPU_FUZZ_GUEST_SIZE);
        u32 size = pick(kSizes);
        const bool valid = rng_.chance(80);
        if (cmd == PS_MFC_GETLLAR || cmd == PS_MFC_PUTLLC || cmd == PS_MFC_PUTLLUC || cmd == PS_MFC_PUTQLLUC) {
            eal &= ~127u;
            add(PS_SPU_FZ_RES, rng_.below(4), PS_SPU_FUZZ_GUEST_BASE + (rng_.chance(70) ? (eal - PS_SPU_FUZZ_GUEST_BASE)
                                                                                     : rng_.below(PS_SPU_FUZZ_GUEST_SIZE)) /
                                                     128 * 128);
        } else if ((cmd & PS_MFC_LIST_MASK) && (cmd & 0xF0) != 0xA0) {
            const u32 elements = 1 + rng_.below(6);
            size = elements * 8;
            eal = 0x10000 + rng_.below(0x1000) * 8; // list in LS
            lsa = 0x30000 + rng_.below(0x400) * 16;
            if (valid) add(PS_SPU_FZ_LIST, 0, eal, elements | (rng_.next() << 8));
            else if (rng_.chance(50)) eal |= 4;
        } else if (cmd == PS_MFC_SNDSIG) {
            size = 4;
            eal = PS_SPU_THREAD_BASE + PS_SPU_THREAD_SNR1;
        } else if (valid) {
            if (size > 16384) size = 16384;
            if (size < 16) {
                eal &= ~(size ? size - 1 : 0);
                lsa = (lsa & ~0xFu) | (eal & 0xF);
            } else {
                eal &= ~0xFu;
                lsa &= ~0xFu;
            }
            if (eal + size > PS_SPU_FUZZ_GUEST_BASE + PS_SPU_FUZZ_GUEST_SIZE)
                eal = PS_SPU_FUZZ_GUEST_BASE + ((PS_SPU_FUZZ_GUEST_SIZE - size) & ~0xFu) + (eal & 0xF);
        } else {
            size = rng_.chance(50) ? 3 + rng_.below(13) : (rng_.chance(50) ? 0x4010 : size);
        }
        add(PS_SPU_FZ_MFC, 0, u64{lsa} | (u64{eal} << 32), size | (tag << 16));
    }

    Rng rng_;
    std::vector<ps_spu_fuzz_override>* ov_ = nullptr;
};

// ---- the program-level case (D-081)
//
// Direct calls, a loop, a call through an address computed from a link (position-independent code takes its own
// address that way), relative loads and stores, a stop the run loop resumes after, an absolute call out of the
// program (to a stub test_spu_fuzz puts at PS_SPU_FUZZ_PROGRAM_STUB) and a jump through a register, so lifted code
// runs through every address it derives from its own: pcs, links, the dispatch switch, call returns. Then the
// registers around calls (D-127): kept across a call that doesn't touch them, read and written by the callee, written
// two calls deep, and kept across a call that leaves the program and comes back through the run loop.
struct SyntheticProgram {
    std::vector<u8> bytes; // at PS_SPU_FUZZ_PROGRAM_BASE
    u32 entry = 0;
    std::vector<u32> seeds; // reached only through registers: entries of the next lift, as spu-lift --seed
};

SyntheticProgram synthetic_program()
{
    as::Assembler m(PS_SPU_FUZZ_PROGRAM_BASE);
    const auto data = m.here();
    for (u32 i = 0; i < 4; i++) m.word(0x11111111u * (i + 1));
    const auto data2 = m.here();
    for (u32 i = 0; i < 4; i++) m.word(0);
    const auto func1 = m.here(); // r4 += 1
    m.emit(as::ai(4, 4, 1));
    m.emit(as::ret());
    const auto get_pc = m.here(); // returns its caller's return address in $lr: the caller's own position
    m.emit(as::ret());
    const u32 func2 = m.pc(); // called through a register only
    m.emit(as::il(9, 0x77));
    m.emit(as::ret());
    const auto leaf2 = m.here(); // r22 = r21 + 1
    m.emit(as::ai(22, 21, 1));
    m.emit(as::ret());
    const auto mid = m.here(); // calls func1 (r4 += 1), then r27 = 9; its link kept in r28
    m.emit(as::ori(28, 0, 0));
    m.brsl(0, func1);
    m.emit(as::il(27, 9));
    m.emit(as::bi(28));
    const auto leaf_out = m.here(); // leaves the program for the stub and comes back, then r24 = 7
    m.emit(as::ori(26, 0, 0));
    m.emit(as::brasl(0, PS_SPU_FUZZ_PROGRAM_STUB));
    m.emit(as::il(24, 7));
    m.emit(as::bi(26));

    const u32 entry = m.pc();
    m.emit(as::il(3, 5));
    m.emit(as::il(4, 0));
    const auto loop = m.here();
    m.emit(as::a(4, 4, 3));
    m.brsl(0, func1);
    m.emit(as::ai(3, 3, -1));
    m.brnz(3, loop);
    m.brsl(0, get_pc);
    const u32 link1 = m.pc();
    m.emit(as::ai(11, 0, static_cast<s32>(func2 - link1)));
    m.emit(as::bisl(0, 11));
    m.lqr(5, data);
    m.stqr(4, data2);
    m.emit(as::stop(0x10));
    m.emit(as::a(6, 4, 5));
    m.emit(as::brasl(0, PS_SPU_FUZZ_PROGRAM_STUB));
    const u32 link2 = m.pc();
    const u32 target = link2 + 12; // past the next two instructions
    m.emit(as::ai(12, 0, static_cast<s32>(target - link2)));
    m.emit(as::bi(12));
    m.emit(as::il(7, 1)); // jumped over
    if (m.pc() != target) {
        std::fprintf(stderr, "spu-fuzz-gen: synthetic program layout\n");
        std::exit(1);
    }
    m.emit(as::il(8, 2));
    m.emit(as::il(20, 0x7123)); // untouched by every call below
    m.emit(as::il(21, 0x55));   // read by leaf2
    m.brsl(0, leaf2);
    m.emit(as::a(23, 20, 22));  // r22 written by leaf2
    m.brsl(0, mid);
    m.emit(as::a(25, 4, 27));   // r4 written two calls deep, r27 by mid
    m.emit(as::il(29, 0x99));
    m.brsl(0, leaf_out);        // r20, r23, r25 and r29 must reach ctx when it leaves
    m.emit(as::a(30, 29, 24));
    m.emit(as::a(31, 20, 23));
    m.emit(as::stop(0x11));
    return {m.finish(), entry, {func2, target}};
}

bool lift_program(const SyntheticProgram& sp, const std::string& name, bool relocatable, const std::string& out)
{
    auto image = recomp::spu::load_raw_image(sp.bytes, PS_SPU_FUZZ_PROGRAM_BASE, {sp.entry}, name);
    if (!image) return false;
    image->seeds = sp.seeds;
    recomp::spu::AnalyzerOptions analyzer;
    analyzer.relocatable = relocatable;
    analyzer.constant_entries = !relocatable;
    const recomp::spu::Program program = recomp::spu::analyze(*image, analyzer);
    if (!program.unsupported.empty()) return false;
    recomp::spu::LiftOptions options;
    options.name = name;
    options.relocatable = relocatable;
    std::string error;
    return recomp::spu::write_files(out, recomp::spu::lift(*image, program, options).files, error);
}

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
    u32 per_op = 256, tus = 32;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--out") out = argv[i + 1];
        else if (arg == "--cases") per_op = static_cast<u32>(std::stoul(argv[i + 1]));
        else if (arg == "--tus") tus = static_cast<u32>(std::stoul(argv[i + 1]));
    }
    if (out.empty() || tus == 0) {
        std::fprintf(stderr, "usage: spu-fuzz-gen --out DIR --cases K --tus N\n");
        return 2;
    }

    Generator gen(0x5B0F00Dull);
    std::vector<GenCase> cases;
    for (const Op op : supported_ops()) {
        const bool both_models = op_info(op).flags & OPF_XFLOAT;
        for (u8 model = 0; model < (both_models ? 2 : 1); model++)
            for (u32 i = 0; i < per_op; i++) cases.push_back(gen.make(op, model, i % 4 == 3));
    }

    std::vector<std::string> units(tus, "/* Generated by spu-fuzz-gen (tests/spu). Do not edit. */\n#include \"pspocket/spu/lifted.h\"\n\n");
    std::string table = "/* Generated by spu-fuzz-gen (tests/spu). Do not edit. */\n#include \"fuzz_case.h\"\n\n";
    std::string protos, overrides, rows;
    u32 override_index = 0;
    for (usize i = 0; i < cases.size(); i++) {
        const GenCase& c = cases[i];
        const std::string name = std::format("ps_spu_fz_{:06}", i);
        units[i % tus] += recomp::spu::lift_step_function(name, c.insn, c.pc, c.xfloat, c.reloc != 0) + "\n";
        protos += std::format("void {}(SPUContext* ctx, uint8_t* ls);\n", name);
        for (const auto& o : c.overrides)
            overrides += std::format("    {{{}, {}, {{0}}, 0x{:x}ull, 0x{:x}ull}},\n", o.kind, o.index, o.value, o.value2);
        rows += std::format("    {{{}, {}, {}, 0x{:08x}u, 0x{:05x}u, 0x{:05x}u, {}u, {}}},\n", static_cast<u32>(c.op), c.xfloat,
            c.overrides.size(), c.insn, c.pc, c.reloc, override_index, name);
        override_index += static_cast<u32>(c.overrides.size());
    }
    table += protos;
    table += std::format("\nconst uint32_t ps_spu_fuzz_cases_per_op = {}u;\nconst uint32_t ps_spu_fuzz_case_count = {}u;\n\n", per_op,
        cases.size());
    table += "const ps_spu_fuzz_override ps_spu_fuzz_overrides[] = {\n" + overrides + "    {0, 0, {0}, 0, 0},\n};\n\n";
    table += "const ps_spu_fuzz_case ps_spu_fuzz_cases[] = {\n" + rows + "};\n";

    for (u32 i = 0; i < tus; i++)
        if (!write_text(std::format("{}/fuzz_lifted_{}.c", out, i), units[i])) {
            std::fprintf(stderr, "spu-fuzz-gen: cannot write to %s\n", out.c_str());
            return 1;
        }
    if (!write_text(out + "/fuzz_cases.c", table)) {
        std::fprintf(stderr, "spu-fuzz-gen: cannot write to %s\n", out.c_str());
        return 1;
    }

    const SyntheticProgram sp = synthetic_program();
    if (!lift_program(sp, "ps_spu_fzp", true, out) || !lift_program(sp, "ps_spu_fzq", false, out)) {
        std::fprintf(stderr, "spu-fuzz-gen: cannot lift the synthetic program into %s\n", out.c_str());
        return 1;
    }
    std::string bytes;
    for (usize i = 0; i < sp.bytes.size(); i++) bytes += std::format("{}0x{:02x},", i % 16 ? " " : "\n    ", sp.bytes[i]);
    const std::string program = std::format(
        "/* Generated by spu-fuzz-gen (tests/spu). Do not edit. */\n#include \"fuzz_case.h\"\n\n"
        "const uint8_t ps_spu_fuzz_program[] = {{{}\n}};\nconst uint32_t ps_spu_fuzz_program_size = {}u;\n"
        "const uint32_t ps_spu_fuzz_program_entry = 0x{:05x}u;\n",
        bytes, sp.bytes.size(), sp.entry);
    if (!write_text(out + "/fuzz_program.c", program)) {
        std::fprintf(stderr, "spu-fuzz-gen: cannot write to %s\n", out.c_str());
        return 1;
    }

    std::printf("spu-fuzz-gen: %zu ops, %zu cases (%u per op and floating-point model) in %u units\n", supported_ops().size(),
        cases.size(), per_op, tus);
    return 0;
}
