// pspocket-prepare's SPU lifting across runs (D-204): the seeds a lift used carry over to the next one, not its
// program's entries (every label, also of code only discovery found), so a second prepare does not count discovered
// code as code a DMA may not write. Synthetic image only.

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/prepare/spu.hpp"
#include "pspocket/recomp/spu/asm.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::spu::as;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("prepare: discovered SPU code stays out of the code words when the game is prepared again")
{
    const fs::path dir = fs::temp_directory_path() / ("pspocket-prepare-spu-" + std::to_string(getpid()));
    fs::remove_all(dir);
    const fs::path dump = dir / "images", out = dir / "spu";
    fs::create_directories(dump);
    // A job: its entry jumps through $3 to code nothing else reaches (found by discovery only), as in
    // tests/spu/test_discover.cpp.
    const std::vector<u32> words = {as::ila(3, 0x0C), as::bi(3), as::lnop(), as::il(4, 7), as::a(5, 4, 4), as::bi(0)};
    std::string bin(words.size() * 4, '\0');
    for (size_t i = 0; i < words.size(); i++) ps_st32(reinterpret_cast<u8*>(bin.data()) + 4 * i, words[i]);
    const std::string name = "spu_00000000000000aa_00000";
    std::ofstream(dump / (name + ".bin"), std::ios::binary) << bin;
    std::ofstream(dump / (name + ".txt")) << "kind job\nguest 0x10000\nbase 0x0\nsize 0x18\nentry 0x0\n";

    for (int round = 1; round <= 2; round++) {
        INFO("prepare " << round);
        std::string error;
        prepare::SpuLiftReport report;
        REQUIRE(prepare::lift_spu_images(dump.string(), out.string(), {}, &report, &error, true));
        CHECK(report.lifted == 1);
        const std::string reg = read_file(out / name / (name + "_reg.c"));
        CHECK(reg.find("{0x0000Cu, ") != std::string::npos); // the discovered code is lifted and entered
        // Its code words: the entry's two instructions only (before D-204 the second prepare took 0x0C as a seed,
        // from the first one's entries: 0x0000003B).
        CHECK(reg.find("0x00000003u") != std::string::npos);
        // The seeds it used: none (the entry is the image's own, and no run entered it elsewhere).
        CHECK(fs::exists(out / name / (name + ".seeds")));
        CHECK(read_file(out / name / (name + ".seeds")).empty());
    }
    fs::remove_all(dir);
}

TEST_CASE("prepare: copies of a job that differ only in data are lifted once (D-208)")
{
    const fs::path dir = fs::temp_directory_path() / ("pspocket-prepare-spu-copies-" + std::to_string(getpid()));
    fs::remove_all(dir);
    const fs::path dump = dir / "images", out = dir / "spu";
    fs::create_directories(dump);
    // The entry (code words 0 and 1) and three words the job never runs (its data, as far as the program knows).
    const auto save = [&](const std::string& name, u32 data, u32 entry_word) {
        const std::vector<u32> words = {entry_word, as::bi(0), as::lnop(), data, data + 1, data + 2};
        std::string bin(words.size() * 4, '\0');
        for (size_t i = 0; i < words.size(); i++) ps_st32(reinterpret_cast<u8*>(bin.data()) + 4 * i, words[i]);
        std::ofstream(dump / (name + ".bin"), std::ios::binary) << bin;
        std::ofstream(dump / (name + ".txt")) << "kind job\nguest 0x10000\nbase 0x0\nsize 0x18\nentry 0x0\n";
    };
    save("spu_00000000000000a1_00000", 0x1000, as::il(3, 1));
    save("spu_00000000000000a2_00000", 0x2000, as::il(3, 1)); // the same code, other data
    save("spu_00000000000000a3_00000", 0x3000, as::il(3, 2)); // other code
    std::string error;
    prepare::SpuLiftReport report;
    REQUIRE(prepare::lift_spu_images(dump.string(), out.string(), {}, &report, &error, true));
    CHECK(report.lifted == 2);
    CHECK(report.merged == 1);
    CHECK(fs::exists(out / "spu_00000000000000a1_00000"));
    CHECK_FALSE(fs::exists(out / "spu_00000000000000a2_00000"));
    CHECK(fs::exists(out / "spu_00000000000000a3_00000"));
    fs::remove_all(dir);
}
