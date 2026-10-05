// The SPU analyzer's discovery (D-155): code reached only through a register (a function pointer in position
// independent code, where constants are not taken as code addresses) is found after the instruction before it that
// never falls through, padding skipped; and the lifter leaves such code out of the words a DMA may not write (it can
// be data the program writes: GTA V's jobs were dropped and interpreted, run 096). Synthetic code only.

#include <catch_amalgamated.hpp>

#include <cstring>
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

std::vector<u8> code(const std::vector<u32>& words)
{
    std::vector<u8> out(words.size() * 4);
    for (size_t i = 0; i < words.size(); i++) ps_st32(out.data() + 4 * i, words[i]);
    return out;
}

// 0x00: the entry jumps through $3 (its address formed from a constant, not followed in relocatable code);
// 0x08: padding; 0x0C: the function only that jump reaches.
const std::vector<u32> kProgram = {
    as::ila(3, 0x0C), as::bi(3),     // the entry
    as::lnop(),                      // padding
    as::il(4, 7), as::a(5, 4, 4), as::bi(0), // reached only through $3
};

Program analyse(bool discover)
{
    auto image = load_raw_image(code(kProgram), 0, {0}, "discover_test");
    REQUIRE(image);
    AnalyzerOptions o;
    o.relocatable = true;
    o.constant_entries = false;
    o.discover = discover;
    return analyze(*image, o);
}

} // namespace

TEST_CASE("SPU discovery: a function only a register reaches is found, and stays out of the overwrite check")
{
    const Program plain = analyse(false);
    CHECK(plain.find(0x0C) == nullptr);

    const Program found = analyse(true);
    const Function* f = found.find(0x0C);
    REQUIRE(f);
    CHECK(f->discovered);
    CHECK_FALSE(found.find(0x00)->discovered);
    CHECK(found.find(0x08) == nullptr); // padding is not a start

    // The program's code words: the entry's instructions, not the discovered function's.
    auto image = load_raw_image(code(kProgram), 0, {0}, "discover_test");
    REQUIRE(image);
    LiftOptions lift;
    lift.name = "discover_test";
    lift.relocatable = true;
    const LiftResult r = recomp::spu::lift(*image, found, lift);
    std::string reg;
    for (const auto& file : r.files)
        if (file.name == "discover_test_reg.c") reg = file.content;
    REQUIRE_FALSE(reg.empty());
    // Words 0 and 1 (the entry) set, words 3..5 (the discovered function) not: 0x00000003.
    CHECK(reg.find("0x00000003u") != std::string::npos);
    CHECK(reg.find("{0x0000Cu, ") != std::string::npos); // and it is an entry of the program
}
