// Analyzer and lifter tests on synthetic programs: function discovery, local control flow, tail calls,
// jump-table recognition, unsupported/illegal instructions, and the shape of the generated C.

#include <catch_amalgamated.hpp>

#include <algorithm>

#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"
#include "pspocket/recomp/ppu/synthetic.hpp"

using namespace pspocket;
using namespace pspocket::recomp::ppu;
namespace rt = pspocket::ppu;

namespace {

constexpr u32 kBase = 0x00100000;

const synthetic::Program& programs()
{
    static const synthetic::Program p = synthetic::build_test_programs(kBase);
    return p;
}

const Program& analysis()
{
    static const Program a = [] {
        AnalyzerOptions opt;
        return analyze(programs().image(), programs().seeds, opt);
    }();
    return a;
}

const Function& fn(const std::string& name)
{
    const Function* f = analysis().find(programs().symbol(name));
    REQUIRE(f != nullptr);
    return *f;
}

// A small image from a list of words at kBase.
CodeImage words_image(const std::vector<u32>& words)
{
    std::vector<u8> bytes;
    for (u32 w : words) {
        bytes.push_back(static_cast<u8>(w >> 24));
        bytes.push_back(static_cast<u8>(w >> 16));
        bytes.push_back(static_cast<u8>(w >> 8));
        bytes.push_back(static_cast<u8>(w));
    }
    return CodeImage(kBase, std::move(bytes));
}

std::string all_code(const LiftResult& r)
{
    std::string s;
    for (const auto& f : r.files) s += f.content;
    return s;
}

} // namespace

TEST_CASE("analyzer finds every seeded function and nothing unseeded")
{
    const Program& a = analysis();
    CHECK(a.functions.size() == programs().seeds.size());
    CHECK(a.find(programs().symbol("secret_fn")) == nullptr);
    CHECK(a.unsupported.empty());
    CHECK(a.illegal.empty());
    for (const Function& f : a.functions) CHECK(f.end > f.entry);
}

TEST_CASE("analyzer: calls, recursion, PC-relative bcl is not a call")
{
    CHECK(fn("fib").calls == std::set<u32>{programs().symbol("fib")});
    CHECK(fn("driver").calls ==
          std::set<u32>{programs().symbol("sum_to"), programs().symbol("fib"), programs().symbol("sw_rel")});
    const Function& pc_rel = fn("pc_rel");
    CHECK(pc_rel.calls.empty());
    // The data word after the blr is not decoded as code.
    CHECK(pc_rel.insns.size() == 6);
}

TEST_CASE("analyzer: local loops, a branch to the own entry, tail calls")
{
    const Function& gcd = fn("gcd");
    CHECK(gcd.labels.contains(gcd.entry)); // b gcd is a loop, not a call
    CHECK(gcd.tail_calls.empty());
    const Function& tc = fn("tail_calls");
    CHECK(tc.tail_calls == std::set<u32>{programs().symbol("add_one"), programs().symbol("times_three")});
    CHECK(tc.insns.size() == 4);
    const Function& sum = fn("sum_to");
    CHECK(sum.labels.size() == 2); // loop head and the exit
}

TEST_CASE("analyzer: basic blocks and edges")
{
    // sum_to: [li cmpwi beq] -> {done, mtctr}; [mtctr li] -> loop; [add addi bdnz] -> {loop, done}; [mr blr]
    const Function& sum = fn("sum_to");
    REQUIRE(sum.blocks.size() == 4);
    const u32 e = sum.entry;
    CHECK(sum.blocks[0].start == e);
    CHECK(sum.blocks[0].kind == BlockEnd::Conditional);
    CHECK(sum.blocks[0].succs == std::vector<u32>{e + 0x20, e + 0x0C});
    CHECK(sum.blocks[1].kind == BlockEnd::FallThrough);
    CHECK(sum.blocks[1].succs == std::vector<u32>{e + 0x14});
    CHECK(sum.blocks[2].kind == BlockEnd::Conditional);
    CHECK(sum.blocks[2].succs == std::vector<u32>{e + 0x14, e + 0x20});
    CHECK(sum.blocks[3].kind == BlockEnd::Return);
    CHECK(sum.blocks[3].succs.empty());

    const Function& sw = fn("sw_rel");
    const auto sw_block = std::find_if(sw.blocks.begin(), sw.blocks.end(), [](const Block& b) { return b.kind == BlockEnd::Switch; });
    REQUIRE(sw_block != sw.blocks.end());
    CHECK(sw_block->succs.size() == 6); // 7 entries, one shared target
    const Function& tc = fn("tail_calls");
    REQUIRE(tc.blocks.size() == 2);
    CHECK(tc.blocks[0].kind == BlockEnd::CondExit);
    CHECK(tc.blocks[1].kind == BlockEnd::TailCall);
    const Function& fib = fn("fib");
    CHECK(std::count_if(fib.blocks.begin(), fib.blocks.end(), [](const Block& b) { return b.kind == BlockEnd::Return; }) == 1);
    for (const Function& f : analysis().functions) {
        u32 covered = 0;
        for (const Block& b : f.blocks) covered += (b.end - b.start) / 4;
        CHECK(covered == f.insns.size());
    }
}

TEST_CASE("analyzer: GCC switch patterns become bounded jump tables")
{
    const Function& rel = fn("sw_rel");
    REQUIRE(rel.jump_tables.size() == 1);
    const JumpTable& t1 = rel.jump_tables.begin()->second;
    CHECK(t1.relative);
    CHECK(t1.bounded);
    REQUIRE(t1.targets.size() == 7);
    CHECK(t1.targets[2] == t1.targets[4]); // shared case body
    CHECK(t1.table == t1.bctr + 4);         // table right after the bctr
    for (u32 t : t1.targets) CHECK(rel.labels.contains(t));

    const Function& toc = fn("sw_toc");
    REQUIRE(toc.jump_tables.size() == 1);
    const JumpTable& t2 = toc.jump_tables.begin()->second;
    CHECK(t2.relative);
    CHECK(t2.bounded);
    CHECK(t2.table == programs().symbol("sw_toc.table"));
    REQUIRE(t2.targets.size() == 5);
    for (u32 i = 0; i < 5; i++) CHECK(t2.targets[i] == programs().symbol("sw_toc." + std::to_string(i)));

    const Function& abs = fn("sw_abs");
    REQUIRE(abs.jump_tables.size() == 1);
    const JumpTable& t3 = abs.jump_tables.begin()->second;
    CHECK_FALSE(t3.relative);
    CHECK(t3.bounded);
    CHECK(t3.targets.size() == 4);
}

TEST_CASE("analyzer: the SNC switch pattern becomes a bounded jump table")
{
    const Function& snc = fn("sw_snc");
    REQUIRE(snc.jump_tables.size() == 1);
    const JumpTable& t = snc.jump_tables.begin()->second;
    CHECK(t.relative);
    CHECK(t.bounded);
    CHECK(t.table == programs().symbol("sw_snc.table"));
    REQUIRE(t.targets.size() == 4);
    for (u32 i = 0; i < 4; i++) CHECK(t.targets[i] == programs().symbol("sw_snc." + std::to_string(i)));
    CHECK_FALSE(snc.needs_entry_state);
}

TEST_CASE("analyzer: a table whose base the prologue sets, indexed by a loaded byte")
{
    const Function& vm = fn("vm");
    REQUIRE(vm.jump_tables.size() == 3); // every dispatch site but the one across a call
    for (const auto& [bctr, t] : vm.jump_tables) {
        CHECK_FALSE(t.relative);
        CHECK_FALSE(t.bounded); // read until the zero after the five entries
        CHECK(t.table == programs().symbol("vm.table"));
        REQUIRE(t.targets.size() == 5);
        for (u32 i = 0; i < 5; i++) CHECK(t.targets[i] == programs().symbol("vm." + std::to_string(i)));
    }
    CHECK(vm.labels.contains(programs().symbol("vm_exit"))); // its register restore is vm's own code
}

namespace {

// An SNC-style switch on (r3 - 37) & 0xFF with four relative entries, and one way to break its bounds check.
enum class SncFlaw { None, NarrowCompare, ClobberedField, IndexChanged };

CodeImage snc_switch(SncFlaw flaw)
{
    using L = as::Assembler::Label;
    as::Assembler a(kBase);
    const L table = a.new_label(), def = a.new_label();
    L cases[4];
    for (L& l : cases) l = a.new_label();
    a.emit(as::addi(9, 3, -37));
    a.emit(as::clrlwi(0, 9, flaw == SncFlaw::NarrowCompare ? 28 : 24)); // the compare sees 4 index bits, not 8
    a.emit(as::cmplwi(7, 0, 3));
    a.emit(flaw == SncFlaw::ClobberedField ? as::cmpwi(7, 5, 0) : as::nop());
    a.emit(flaw == SncFlaw::IndexChanged ? as::addi(9, 9, 1) : as::nop());
    a.bgt(7, def);
    a.load_address(11, table);
    a.emit(as::rlwinm(9, 9, 2, 22, 29));
    a.emit(as::lwzx(0, 9, 11));
    a.emit(as::extsw(0, 0));
    a.emit(as::add(0, 0, 11));
    a.emit(as::mtctr(0));
    a.emit(as::bctr());
    a.bind(def);
    a.emit(as::li(3, -1));
    a.emit(as::blr());
    for (u32 i = 0; i < 4; i++) {
        a.bind(cases[i]);
        a.emit(as::li(3, static_cast<s32>(i)));
        a.emit(as::blr());
    }
    a.bind(table);
    for (const L& l : cases) a.label_offset_word(l, table);
    a.word(0x7FFFFFF0); // not a code offset: ends a scan of the table
    return CodeImage(kBase, a.finish());
}

} // namespace

TEST_CASE("analyzer: a bounds check that does not cover the whole index leaves the table unbounded")
{
    const Seed seeds[] = {{kBase, {}, "f"}};
    for (const SncFlaw flaw : {SncFlaw::None, SncFlaw::NarrowCompare, SncFlaw::ClobberedField, SncFlaw::IndexChanged}) {
        INFO("flaw " << static_cast<int>(flaw));
        const CodeImage img = snc_switch(flaw);
        const Program p = analyze(img, seeds);
        REQUIRE(p.functions.size() == 1);
        REQUIRE(p.functions[0].jump_tables.size() == 1);
        const JumpTable& t = p.functions[0].jump_tables.begin()->second;
        CHECK(t.relative);
        CHECK(t.bounded == (flaw == SncFlaw::None));
        CHECK(t.targets.size() >= 4);
    }
}

TEST_CASE("lifter output: files, symbols, registration, switch for jump tables, no fallbacks")
{
    LiftOptions opt;
    opt.name = "t";
    opt.prefix = "p_";
    opt.tu_count = 2;
    const LiftResult r = lift(programs().image(), analysis(), opt);
    std::vector<std::string> names;
    for (const auto& f : r.files) names.push_back(f.name);
    CHECK(names == std::vector<std::string>{"t.h", "t_0.c", "t_1.c", "t_reg.c"});
    const std::string code = all_code(r);
    for (const Function& f : analysis().functions)
        CHECK(code.find(std::format("void p_f_{:08X}(PPUContext* PS_RESTRICT ctx, uint8_t* base)", f.entry)) != std::string::npos);
    CHECK(code.find("switch ((uint32_t)ctr & ~3u)") != std::string::npos);
    CHECK(code.find("ps_ppu_jump_table_miss") != std::string::npos);
    CHECK(code.find("__builtin_unreachable") == std::string::npos);
    CHECK(code.find("__builtin_assume_aligned(base, PS_PPU_BASE_ALIGN)") != std::string::npos);
    CHECK(code.find("ps_ppu_interp_one") == std::string::npos);
    CHECK(code.find("ps_ppu_dispatch_register_table(t_entries") != std::string::npos);
    CHECK(code.find("goto L_") != std::string::npos);
    CHECK(r.stats.fallbacks == 0);
    CHECK(r.stats.jump_tables == 7);
    // One dispatch switch per function, shared by its table sites (vm has three).
    auto count = [&](std::string_view needle) {
        usize n = 0;
        for (usize at = code.find(needle); at != std::string::npos; at = code.find(needle, at + 1)) n++;
        return n;
    };
    CHECK(count("D_tables: switch") == 5);
    CHECK(count("goto D_tables;") == 7);
    CHECK(count("D_known: switch") == 1);
    CHECK(r.stats.entry_not_first == 1); // entry_above
    CHECK(r.stats.functions == analysis().functions.size());
}

TEST_CASE("unsupported instructions are listed and lifted as interpreter fallbacks")
{
    const CodeImage img = words_image({as::encode(rt::Op::ECIWX, as::Fields{.d = 3, .a = 0, .b = 4}), as::addi(3, 3, 1), as::blr()});
    const Seed seeds[] = {{kBase, {}, "f"}};
    const Program p = analyze(img, seeds);
    REQUIRE(p.unsupported.size() == 1);
    CHECK(p.unsupported[0].addr == kBase);
    const LiftResult r = lift(img, p, LiftOptions{});
    CHECK(r.stats.fallbacks == 1);
    CHECK(all_code(r).find(std::format("ps_ppu_interp_one(ctx, base, 0x{:x}u)", kBase)) != std::string::npos);
}

TEST_CASE("illegal words end the block and call ps_ppu_illegal")
{
    const CodeImage img = words_image({as::li(3, 1), 0x00000000, as::blr()});
    const Seed seeds[] = {{kBase, {}, "f"}};
    const Program p = analyze(img, seeds);
    REQUIRE(p.illegal.size() == 1);
    CHECK(p.illegal[0].addr == kBase + 4);
    CHECK(p.functions[0].insns.size() == 2); // the blr after it is not reached
    const LiftResult r = lift(img, p, LiftOptions{});
    CHECK(all_code(r).find("ps_ppu_illegal(ctx, base, 0x100004u)") != std::string::npos);
}

TEST_CASE("fall-through into the next function is a tail call; unknown bctr is an indirect jump")
{
    // f1: li r3,1 ; (falls into f2)   f2: blr      f3: mtctr r3 ; bctr
    const CodeImage img = words_image({as::li(3, 1), as::blr(), as::mtctr(3), as::bctr()});
    const Seed seeds[] = {{kBase, {}, "f1"}, {kBase + 4, {}, "f2"}, {kBase + 8, {}, "f3"}};
    const Program p = analyze(img, seeds);
    const Function* f1 = p.find(kBase);
    REQUIRE(f1);
    CHECK(f1->fallthrough_calls == std::set<u32>{kBase + 4});
    CHECK(f1->insns.size() == 1);
    const Function* f3 = p.find(kBase + 8);
    REQUIRE(f3);
    CHECK(f3->jump_tables.empty());
    CHECK(f3->needs_entry_state);
    const std::string code = all_code(lift(img, p, LiftOptions{}));
    CHECK(code.find("f_00100004(ctx, base); return; /* falls through") != std::string::npos);
    CHECK(code.find("ps_ppu_jump_indirect(ctx, base, (uint32_t)ctr & ~3u, entry_lr, entry_sp)") != std::string::npos);
}

TEST_CASE("branches leaving the image are dispatched at runtime")
{
    const CodeImage img = words_image({as::b_abs(0x00F00000)});
    const Seed seeds[] = {{kBase, {}, "f"}};
    const Program p = analyze(img, seeds);
    CHECK_FALSE(p.warnings.empty());
    CHECK(p.functions[0].needs_entry_state);
    CHECK(all_code(lift(img, p, LiftOptions{})).find("ps_ppu_jump_indirect(ctx, base, 0xf00000u") != std::string::npos);
}

TEST_CASE("step-mode lifting stores the next pc")
{
    const std::string s = lift_step_function("step_fn", as::bc_rel(16, 0, 0x40), kBase);
    CHECK(s.find("ctr -= 1;") != std::string::npos);
    CHECK(s.find("next = 0x100040u") != std::string::npos);
    CHECK(s.find("ctx->cia = next;") != std::string::npos);
    CHECK_THROWS(lift_step_function("bad", 0x00000000, kBase));
}

namespace {

// GCC's switch in Assassin's Creed's 0xa88e78 (run 109): the table base comes through two loads from data (r30 from
// the TOC in the prologue, the table address from r30's block), the bounds check branches to the table code ("ble
// cr7") with the default case falling through to the epilogue, and every case restores r30 and tail-calls. One way to
// break it per variant.
enum class GccFlaw { None, SecondWayIn, CaseLoopsBack };

struct GccSwitch {
    CodeImage image;
    u32 toc = 0, table = 0, other = 0;
    std::vector<u32> cases;
};

GccSwitch gcc_switch(GccFlaw flaw)
{
    using L = as::Assembler::Label;
    as::Assembler a(kBase);
    const L table_code = a.new_label(), table = a.new_label(), other = a.new_label(), toc = a.new_label(),
            pointer = a.new_label();
    L cases[5];
    for (L& l : cases) l = a.new_label();
    const auto epilogue = [&] {
        a.emit(as::ld(0, 160, 1));
        a.emit(as::ld(30, 128, 1));
        a.emit(as::mtlr(0));
        a.emit(as::addi(1, 1, 144));
    };
    a.emit(as::stdu(1, -144, 1));
    a.emit(as::mflr(0));
    a.emit(as::std_(30, 128, 1));
    a.emit(as::std_(0, 160, 1));
    a.emit(as::lwz(30, 0, 2)); // r30: a pointer from the TOC
    a.emit(as::lwz(0, 0, 3));
    a.emit(as::srwi(0, 0, 29));
    a.emit(as::cmplwi(7, 0, 4));
    a.ble(7, table_code);
    if (flaw == GccFlaw::SecondWayIn) {
        a.emit(as::cmpwi(6, 4, 0));
        a.beq(6, table_code); // the index is not checked on this way in
    }
    epilogue();
    a.emit(as::blr());
    a.bind(table_code);
    a.emit(as::lwz(11, 8, 30)); // the table's address, from r30's block
    a.emit(as::rldic(9, 0, 2, 59));
    a.emit(as::lwzx(0, 9, 11));
    a.emit(as::extsw(0, 0));
    a.emit(as::add(0, 0, 11));
    a.emit(as::mtctr(0));
    a.emit(as::bctr());
    a.bind(table);
    for (const L& l : cases) a.label_offset_word(l, table);
    for (u32 i = 0; i < 5; i++) {
        a.bind(cases[i]);
        a.emit(as::li(3, static_cast<s32>(i)));
        if (flaw == GccFlaw::CaseLoopsBack && i == 4) {
            a.emit(as::addi(30, 30, 4)); // r30 changes, and the table code reads it again
            a.b(table_code);
            continue;
        }
        epilogue();
        a.b(other);
    }
    a.bind(other);
    a.emit(as::blr());
    a.bind(toc);
    a.label_word(pointer);
    a.bind(pointer);
    a.word(0);
    a.word(0);
    a.label_word(table);
    std::vector<u8> bytes = a.finish();
    std::vector<u32> case_addresses;
    for (const L& l : cases) case_addresses.push_back(a.address(l));
    return GccSwitch{CodeImage(kBase, std::move(bytes)), a.address(toc), a.address(table), a.address(other),
        std::move(case_addresses)};
}

} // namespace

TEST_CASE("analyzer: a table base loaded through data, a bounds check into the table code, cases that tail-call")
{
    for (const GccFlaw flaw : {GccFlaw::None, GccFlaw::SecondWayIn, GccFlaw::CaseLoopsBack}) {
        INFO("flaw " << static_cast<int>(flaw));
        const GccSwitch s = gcc_switch(flaw);
        const Seed seeds[] = {{kBase, s.toc, "f"}, {s.other, s.toc, "other"}};
        const Program p = analyze(s.image, seeds);
        const Function* f = p.find(kBase);
        REQUIRE(f != nullptr);
        if (flaw == GccFlaw::CaseLoopsBack) {
            CHECK(f->jump_tables.empty()); // r30 is not the prologue's value at the table code
            continue;
        }
        REQUIRE(f->jump_tables.size() == 1);
        const JumpTable& t = f->jump_tables.begin()->second;
        CHECK(t.relative);
        CHECK(t.table == s.table);
        CHECK(t.bounded == (flaw == GccFlaw::None));
        REQUIRE(t.targets.size() == 5);
        for (u32 i = 0; i < 5; i++) CHECK(t.targets[i] == s.cases[i]);
        CHECK(f->tail_calls.contains(s.other));
    }
}
