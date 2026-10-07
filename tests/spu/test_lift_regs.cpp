// The SPU lifter's register traffic (D-219): a function called at its entry loads only the registers that may be read
// before they are written, and each way out stores only the registers that may have been written. Synthetic code
// only; the program-level fuzz test (test_fuzz.cpp) runs such functions against the interpreter.

#include <catch_amalgamated.hpp>

#include <format>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu/lifter.hpp"

using namespace pspocket;
using namespace pspocket::recomp::spu;
namespace as = pspocket::recomp::spu::as;

namespace {

// 0x00: r3 written before it is read, r5 and $lr read, r4 written.
// 0x0C: r41 written on one path only, then both paths return.
const std::vector<u32> kProgram = {
    as::il(3, 5), as::a(4, 3, 5), as::ret(),
    as::brz(40, 8), as::il(41, 1), as::ret(),
};

std::string lifted_text()
{
    std::vector<u8> bytes(kProgram.size() * 4);
    for (size_t i = 0; i < kProgram.size(); i++) ps_st32(bytes.data() + 4 * i, kProgram[i]);
    auto image = load_raw_image(bytes, 0, {0x00, 0x0C}, "regs_test");
    REQUIRE(image);
    const Program program = analyze(*image);
    REQUIRE(program.functions.size() == 2);
    LiftOptions options;
    options.name = "regs_test";
    const LiftResult result = lift(*image, program, options);
    for (const OutputFile& f : result.files)
        if (f.name == "regs_test_0.c") return f.content;
    FAIL("no function unit");
    return {};
}

// The text of the function whose entry is `entry`.
std::string function_text(const std::string& unit, u32 entry)
{
    const std::string head = std::format("_{:05x}(SPUContext* ctx, uint8_t* ls)\n{{", entry);
    const size_t at = unit.find(head);
    REQUIRE(at != std::string::npos);
    const size_t end = unit.find("\n}\n", at);
    return unit.substr(at, end - at);
}

// The fast entry: the loads between "if (ctx->pc == ...) {" and its goto.
std::string entry_loads(const std::string& fn)
{
    const size_t at = fn.find("if (ctx->pc == ");
    REQUIRE(at != std::string::npos);
    return fn.substr(at, fn.find("goto L_", at) - at);
}

} // namespace

TEST_CASE("SPU lifter: a called function loads what it reads first and stores what it wrote (D-219)")
{
    const std::string unit = lifted_text();

    const std::string first = function_text(unit, 0x00);
    INFO(first);
    const std::string loads = entry_loads(first);
    CHECK(loads.find("r0 = psl_ld(ctx, 0);") != std::string::npos);
    CHECK(loads.find("r5 = psl_ld(ctx, 5);") != std::string::npos);
    CHECK(loads.find("r3 = psl_ld(ctx, 3);") == std::string::npos); // written before it is read
    CHECK(loads.find("r4 = psl_ld(ctx, 4);") == std::string::npos); // only written
    CHECK(first.find("exit_0:\n    psl_st(ctx, 3, r3); psl_st(ctx, 4, r4);\n    return;") != std::string::npos);
    CHECK(first.find("psl_st(ctx, 5, r5)") == std::string::npos);
    CHECK(first.find("psl_st(ctx, 0, r0)") == std::string::npos);
    // Resumed anywhere else, everything it uses.
    CHECK(first.find("PSL_RELOAD();\n    goto dispatch;") != std::string::npos);

    // r41 is stored on both ways back (the one that skips its write too), so it must be loaded.
    const std::string second = function_text(unit, 0x0C);
    INFO(second);
    const std::string loads2 = entry_loads(second);
    CHECK(loads2.find("r40 = psl_ld(ctx, 40);") != std::string::npos);
    CHECK(loads2.find("r41 = psl_ld(ctx, 41);") != std::string::npos);
}
