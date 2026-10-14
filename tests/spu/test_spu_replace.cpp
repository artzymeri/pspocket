// Native replacements of SPU middleware functions (D-965): the NEON building blocks against the lifted code's helpers,
// the derived programs a known build gets (dispatch to native code, the lifted function when it declines), the
// lifter's calls through the table into listed functions, and the interprocedural liveness that says which registers
// a replacement must leave exact. Synthetic programs only (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu/lifter.hpp"
#include "pspocket/recomp/spu/liveness.hpp"
#include "pspocket/spu/replace.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_test_util.hpp"

extern "C" {
unsigned ps_test_replace_frest(void);
unsigned ps_test_replace_frsqest(void);
unsigned ps_test_replace_arith(uint32_t rounds);
unsigned ps_test_replace_ld4(void);
}

using namespace pspocket;
namespace rp = pspocket::spu::replace;
namespace as = pspocket::recomp::spu::as;
using pspocket::recomp::spu::AnalyzerOptions;
using pspocket::recomp::spu::LiftOptions;
using pspocket::recomp::spu::Liveness;
using pspocket::recomp::spu::LivenessOptions;
using pspocket::recomp::spu::LsImage;

TEST_CASE("SPU replace: NEON building blocks give what the lifted code's helpers give (D-965)", "[spu]")
{
    const spu::test::FpEnv fp;
    CHECK(ps_test_replace_frest() == 0);
    CHECK(ps_test_replace_frsqest() == 0);
    CHECK(ps_test_replace_arith(20000) == 0);
    CHECK(ps_test_replace_ld4() == 0);
}

namespace {

// A program of two functions, hand-written: E at kStart, F at kStart + 0x10 ("lifted": $3 + 1, then return).
constexpr u32 kStart = 0x3000, kSize = 0x40;
constexpr u32 kAllCode[(kSize / 4 + 31) / 32] = {~0u};
void fn_e(SPUContext* ctx, uint8_t*) { ctx->pc = ctx->gpr[0].u32[3]; }
void fn_f(SPUContext* ctx, uint8_t*)
{
    ctx->gpr[3].u32[3] += 1;
    ctx->pc = ctx->gpr[0].u32[3];
}
// F natively: $3 + 100, and declines $3 = 0.
int native_f(SPUContext* ctx, uint8_t*)
{
    if (ctx->gpr[3].u32[3] == 0) return 0;
    ctx->gpr[3].u32[3] += 100;
    ctx->pc = ctx->gpr[0].u32[3];
    return 1;
}

struct TestProgram {
    std::vector<u8> image = std::vector<u8>(kSize);
    std::vector<ps_spu_entry> entries;
    ps_spu_program program{};
    explicit TestProgram(u32 seed)
    {
        for (u32 b = 0; b < kSize; b += 4) ps_st32(image.data() + b, seed * 0x01000193u ^ b);
        entries = {{kStart, fn_e}, {kStart + 0x10, fn_f}};
        const u64 hash = ps_spu_hash64(image.data(), kSize);
        program = ps_spu_program{"spu_replace_test", hash, kStart, kSize, PS_SPU_XFLOAT_APPROXIMATE, 1, 2, entries.data(),
            nullptr, kAllCode, ps_spu_code_hash(image.data(), kSize, kAllCode)};
    }
    ~TestProgram() { ps_spu_unregister_program(&program); }
};

u32 call(const ps_spu_program* p, u32 entry, u32 r3)
{
    SPUContext ctx{};
    ps_spu_attach_program_at(&ctx, p, kStart);
    ctx.gpr[3].u32[3] = r3;
    ctx.gpr[0].u32[3] = 0x100; // the return address
    ctx.pc = entry;
    const ps_spu_fn fn = ps_spu_lookup(&ctx, entry);
    REQUIRE(fn);
    fn(&ctx, nullptr);
    CHECK(ctx.pc == 0x100);
    return ctx.gpr[3].u32[3];
}

} // namespace

TEST_CASE("SPU replace: a known build is found as a program derived from it, its listed functions native", "[spu]")
{
    rp::set_mode(rp::Mode::on);
    TestProgram t(1);
    rp::add_build("replace test build", t.program.hash, kStart, kSize, {{kStart + 0x10, native_f, {}}});
    ps_spu_register_program(&t.program);
    const ps_spu_program* found = ps_spu_find_program(t.program.hash, kStart, kSize);
    REQUIRE(found);
    CHECK(found != &t.program);
    CHECK(rp::original_of(found) == &t.program);
    CHECK(rp::derived_of(&t.program) == found);
    CHECK(std::string(found->name) == t.program.name);
    // F runs natively; a declined call runs the lifted function; E (not listed) is the lifted function as it was.
    CHECK(call(found, kStart + 0x10, 5) == 105);
    CHECK(call(found, kStart + 0x10, 0) == 1);
    CHECK(call(found, kStart, 7) == 7);
    // The original, found by nothing now, still runs the lifted code.
    CHECK(call(&t.program, kStart + 0x10, 5) == 6);
    bool seen = false;
    for (const rp::FunctionStats& s : rp::stats())
        if (s.build == "replace test build") {
            seen = true;
            CHECK(s.native == 1);
            CHECK(s.declined == 1);
        }
    CHECK(seen);
    // Taking the original out takes the derived one out with it.
    ps_spu_unregister_program(&t.program);
    CHECK(ps_spu_find_program(t.program.hash, kStart, kSize) == nullptr);
}

TEST_CASE("SPU replace: the lifter calls a listed function through the program's table", "[spu]")
{
    // E calls F directly (brsl $lr), then returns; F returns.
    as::Assembler a(0x2000);
    const auto f = a.label();
    a.emit(as::il(5, 7));
    a.brsl(0, f);
    a.emit(as::a(3, 3, 5));
    a.emit(as::bi(0));
    a.bind(f);
    a.emit(as::ai(3, 3, 1));
    a.emit(as::bi(0));
    const std::vector<u8> code = a.finish();
    auto image = pspocket::recomp::spu::load_raw_image(code, 0x2000, {0x2000});
    REQUIRE(image);
    const u32 f_addr = a.address(f);
    const auto program = pspocket::recomp::spu::analyze(*image, AnalyzerOptions{});
    LiftOptions options;
    options.name = "replace_lift_test";
    const std::string direct = pspocket::recomp::spu::function_symbol(image->hash(), 0x2000, f_addr) + "(ctx, ls);";
    const auto unit = [&] {
        const auto lifted = pspocket::recomp::spu::lift(*image, program, options);
        for (const auto& file : lifted.files)
            if (file.name == "replace_lift_test_0.c") return file.content;
        return std::string();
    };
    CHECK(unit().find(direct) != std::string::npos); // not listed: a C call
    rp::add_build("replace lift test", image->hash(), 0x2000, image->code_size(), {{f_addr, nullptr, {}}});
    const std::string listed = unit();
    CHECK(listed.find(direct) == std::string::npos); // listed: leaves for the run loop at F
    CHECK(listed.find(std::format("ctx->pc = 0x{:05X}u;", f_addr)) != std::string::npos);
}

TEST_CASE("SPU replace: the lifter leaves at a loop boundary entered forward, loops locally on its back edge", "[spu]")
{
    // $3 = 0; $4 = 10; head: $3 += 1; $4 -= 1; brnz $4, head; exit: return.
    as::Assembler a(0x6000);
    a.emit(as::il(3, 0));
    a.emit(as::il(4, 10));
    const auto head = a.here();
    a.emit(as::ai(3, 3, 1));
    a.emit(as::ai(4, 4, -1));
    a.brnz(4, head);
    const auto exit = a.here();
    a.emit(as::bi(0));
    const std::vector<u8> code = a.finish();
    auto image = pspocket::recomp::spu::load_raw_image(code, 0x6000, {0x6000});
    REQUIRE(image);
    const u32 h = a.address(head), x = a.address(exit);
    const auto program = pspocket::recomp::spu::analyze(*image, AnalyzerOptions{});
    LiftOptions options;
    options.name = "replace_loop_test";
    options.comments = false;
    const auto unit = [&] {
        const auto lifted = pspocket::recomp::spu::lift(*image, program, options);
        for (const auto& file : lifted.files)
            if (file.name == "replace_loop_test_0.c") return file.content;
        return std::string();
    };
    const std::string leave_head = std::format("ctx->pc = 0x{:05X}u;", h), leave_exit = std::format("ctx->pc = 0x{:05X}u;", x);
    const std::string plain = unit();
    CHECK(plain.find(std::format("goto L_{:05X};", h)) != std::string::npos);
    options.boundaries = {h, x};
    const std::string bounded = unit();
    CHECK(bounded.find(std::format("goto L_{:05X};", h)) != std::string::npos); // the back edge: local
    CHECK(bounded.find(leave_exit) != std::string::npos);                        // falls into the exit: leaves
    // Falling into the head from the entry leaves there (plain code only writes the head's pc in its attention poll).
    const auto count = [](const std::string& text, const std::string& what) {
        usize n = 0;
        for (usize at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n++;
        return n;
    };
    CHECK(count(bounded, leave_head) == count(plain, leave_head) + 1);
}

namespace {

Liveness liveness_of(as::Assembler& a, const LivenessOptions& options = {})
{
    const std::vector<u8> code = a.finish();
    auto image = pspocket::recomp::spu::load_raw_image(code, a.origin(), {a.origin()});
    REQUIRE(image);
    const auto program = pspocket::recomp::spu::analyze(*image, AnalyzerOptions{});
    return pspocket::recomp::spu::analyze_liveness(*image, program, options);
}

std::vector<u32> regs_of(const spu::RegSet& s)
{
    std::vector<u32> out;
    for (u32 r = 0; r < 128; r++)
        if (s.test(r)) out.push_back(r);
    return out;
}

} // namespace

TEST_CASE("SPU liveness: across calls and returns, to the end of the program (D-965)", "[spu]")
{
    // E: $5 = 7; call F; $3 = $6 + $7; return (out of the program). F: $6 = $5 + $5; return.
    as::Assembler a(0x4000);
    const auto f = a.label();
    a.emit(as::il(5, 7));
    a.brsl(0, f);
    const u32 ret = a.pc();
    a.emit(as::a(3, 6, 7));
    a.emit(as::bi(0));
    a.bind(f);
    a.emit(as::a(6, 5, 5));
    a.emit(as::bi(0));
    const Liveness live = liveness_of(a);
    const u32 fa = a.address(f);
    // After F returns: $6 (F's result) and $7 (from E's caller) are read, $0 by E's return.
    CHECK(regs_of(live.at(ret)) == std::vector<u32>{0, 6, 7});
    // F reads $5 and its link; it writes $6 on every path; at its entry $7 is still live (E reads it after).
    CHECK(regs_of(live.uses.at(fa)) == std::vector<u32>{0, 5});
    CHECK(regs_of(live.kills.at(fa)) == std::vector<u32>{6});
    CHECK(regs_of(live.at(fa)) == std::vector<u32>{0, 5, 7});
    // A path to the read, for the report.
    CHECK(live.why(ret, 7).find("reads it") != std::string::npos);
    CHECK(live.why(ret, 9) == "not live");
}

TEST_CASE("SPU liveness: a variadic callee reads only the arguments its caller wrote", "[spu]")
{
    // E: $4 = 1; call P (saves $4-$9, as a printf to the PPU does); return. Is $8 live at E's entry?
    u32 p_addr = 0;
    const auto program = [&] {
        as::Assembler a(0x5000);
        const auto p = a.label();
        a.emit(as::il(4, 1));
        a.brsl(0, p);
        a.emit(as::bi(0));
        a.bind(p);
        for (u32 r = 4; r < 10; r++) a.emit(as::stqd(r, -16 * static_cast<s32>(r), 1));
        a.emit(as::bi(0));
        p_addr = a.address(p);
        return a;
    };
    as::Assembler plain = program();
    CHECK(liveness_of(plain).at(0x5000).test(8)); // P stores it: live
    as::Assembler variadic = program();
    LivenessOptions options;
    options.abi_callees.insert(p_addr);
    const Liveness live = liveness_of(variadic, options);
    CHECK_FALSE(live.at(0x5000).test(8)); // only $4 is an argument there
    CHECK_FALSE(live.at(0x5000).test(4)); // written before the call
}
