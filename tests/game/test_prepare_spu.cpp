// pspocket-prepare's SPU lifting across runs (D-204): the seeds a lift used carry over to the next one, not its
// program's entries (every label, also of code only discovery found), so a second prepare does not count discovered
// code as code a DMA may not write. Synthetic image only.

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/prepare/spu.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/runtime.h"
#include "pspocket/prepare/spu_static.hpp"
#include "synthetic.hpp"

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

TEST_CASE("prepare: the SPU programs an executable carries are saved under the runtime's names (D-226)")
{
    const fs::path dir = fs::temp_directory_path() / ("pspocket-prepare-static-" + std::to_string(getpid()));
    fs::remove_all(dir);
    const fs::path statics = dir / "static", coverage = dir / "coverage", out = dir / "spu";
    fs::create_directories(statics);
    fs::create_directories(coverage);

    // The synthetic executable carries two SPU ELFs entered below 0x3000 (code at 0, entries 0 and 0x40), with the same
    // code: position independent, one program, a module lifted relocatable.
    const test::SyntheticPpuExec exe = test::build_ppu_exec();
    auto elf = recomp::ElfFile::parse(exe.bytes);
    REQUIRE(elf);
    prepare::SpuStaticCount count;
    prepare::SpuStaticOptions o;
    o.low_entry_relocatable = true;
    o.who = "test";
    CHECK(prepare::save_spu_images(prepare::executable_segments(*elf), "the executable", statics, o, count));
    CHECK(count.images == 2);
    CHECK(count.programs == 1);
    const std::span<const u8> code(exe.spu_images[0].data() + 0x100, 0x80);
    const std::string pic = std::format("spu_{:016x}_00000", ps_spu_hash64(code.data(), code.size()));
    REQUIRE(fs::exists(statics / (pic + ".bin")));
    CHECK(read_file(statics / (pic + ".txt")).starts_with("kind module\n"));

    // A task: its code at 0x3000, entered at 0x3040, as SPURS starts it; named by the hash of that segment at its
    // vaddr, as the task start looks it up (spurs_taskset.cpp attach_code), and lifted where it sits.
    test::SyntheticSpuElf task = test::build_spu_elf({"synthetic_task", "", 0x3040, true});
    ps_st32(task.bytes.data() + 0x18, 0x3040);                  // e_entry
    ps_st32(task.bytes.data() + 52 + 8, 0x3000);                // code segment vaddr and paddr
    ps_st32(task.bytes.data() + 52 + 12, 0x3000);
    ps_st32(task.bytes.data() + 52 + 32 + 8, 0x3080);           // data segment
    ps_st32(task.bytes.data() + 52 + 32 + 12, 0x3080);
    std::vector<u8> carrier(0x1000, 0);
    std::copy(task.bytes.begin(), task.bytes.end(), carrier.begin() + 0x200);
    const prepare::SpuSegment segment{0x20000, carrier};
    prepare::SpuStaticCount count2;
    CHECK(prepare::save_spu_images(std::span(&segment, 1), "the executable", statics, o, count2));
    CHECK(count2.programs == 1);
    const std::string task_name = std::format("spu_{:016x}_03000", ps_spu_hash64(task.bytes.data() + 0x100, 0x80));
    REQUIRE(fs::exists(statics / (task_name + ".txt")));
    const std::string txt = read_file(statics / (task_name + ".txt"));
    CHECK(txt.starts_with("kind task\nguest 0x20300\nbase 0x3000\nsize 0x80\nentry 0x3040\n"));

    // A run met the task too and saved it, with where execution entered it: the run's copy is the one lifted (it knows
    // how the game used it), with the seeds of both.
    fs::copy_file(statics / (task_name + ".bin"), coverage / (task_name + ".bin"));
    std::ofstream(coverage / (task_name + ".txt")) << txt << "seed 0x3010\n";
    std::ofstream(statics / (task_name + ".txt"), std::ios::app) << "seed 0x3020\n";
    std::string error;
    prepare::SpuLiftReport report;
    REQUIRE(prepare::lift_spu_images({coverage.string(), statics.string()}, out.string(), {}, &report, &error, true));
    CHECK(report.lifted == 2);
    const std::string seeds = read_file(out / task_name / (task_name + ".seeds"));
    CHECK(seeds.find("0x3010") != std::string::npos);
    CHECK(seeds.find("0x3020") != std::string::npos);
    fs::remove_all(dir);
}
