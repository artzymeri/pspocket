// The SPU analyzer's discovery (D-155): code reached only through a register (a function pointer in position
// independent code, where constants are not taken as code addresses) is found after the instruction before it that
// never falls through, padding skipped; and the lifter leaves such code out of the words a DMA may not write (it can
// be data the program writes: GTA V's jobs were dropped and interpreted, run 096). And constants that point into a
// function's code make labels of it, not copies of it (D-190). Synthetic code only.

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

TEST_CASE("SPU analyzer: a constant pointing into a function is a label of it, not a copy (D-190)")
{
    // 0x00: il constants that point into the function's own body (numbers that happen to be code addresses, as in the
    // firmware's decoders), and one at code nothing else reaches; 0x10..0x1C the body; 0x20 the code only a constant
    // reaches.
    const std::vector<u32> program = {
        as::il(3, 0x10), as::il(4, 0x14), as::il(5, 0x18), as::il(7, 0x20),
        as::a(6, 6, 6), as::a(6, 6, 6), as::a(6, 6, 6), as::bi(0),
        as::il(8, 1), as::bi(0),
    };
    auto image = load_raw_image(code(program), 0, {0}, "constants_test");
    REQUIRE(image);
    const Program p = analyze(*image, {});
    // Two functions: the entry, holding the constants into its body as labels, and the code only a constant reaches.
    // Before, each constant into the body started a function that copied the rest of it (one of the firmware's SPU
    // programs: a function every 16 bytes of a long body, 57 KB of code lifted to 288 MB of C).
    REQUIRE(p.functions.size() == 2);
    const Function* entry = p.find(0x00);
    REQUIRE(entry);
    CHECK(entry->insns.size() == 8);
    for (const u32 label : {0x10u, 0x14u, 0x18u}) CHECK(entry->labels.contains(label));
    const Function* reached = p.find(0x20);
    REQUIRE(reached);
    CHECK(reached->insns.size() == 2);

    // Every label is an entry of the lifted program, entered through its function's dispatch switch.
    LiftOptions lift;
    lift.name = "constants_test";
    const LiftResult r = recomp::spu::lift(*image, p, lift);
    std::string reg;
    for (const auto& file : r.files)
        if (file.name == "constants_test_reg.c") reg = file.content;
    REQUIRE_FALSE(reg.empty());
    for (const char* e : {"{0x00010u, ", "{0x00014u, ", "{0x00018u, ", "{0x00020u, "})
        CHECK(reg.find(e) != std::string::npos);
}
