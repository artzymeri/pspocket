// Game libraries (route B, D-152): the synthetic self-test program (recomp/ppu's benchmark, lifted by ppu-lift at
// build time) made into a game library with the device's compiler and build, the way the phone makes one, then loaded
// through the host's runtime table and run: it must give the result computed natively. A library made for another
// runtime, or for another executable, must be refused before anything in it runs.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/codegen/build.hpp"
#include "pspocket/codesign/codesign.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/ppu/context.h"
#include "pspocket/ppu/runtime.h"
#include "ps_selftest_ppu_image.h"

using namespace pspocket;

namespace {

namespace fs = std::filesystem;

constexpr u32 kBuffer = 0x00100000;
constexpr u32 kStackBase = 0x00E00000;
constexpr u32 kStackSize = 0x00100000;

fs::path temp_dir()
{
    const fs::path d = fs::temp_directory_path() / std::format("pspocket-game-library-{}", getpid());
    fs::create_directories(d);
    return d;
}

// Makes a game library from the lifted self-test program, with `abi_hash` written into it as its runtime's.
std::string make_library(const fs::path& dir, const std::string& name, uint64_t abi_hash)
{
    const fs::path info = dir / (name + "-info");
    fs::create_directories(info);
    std::ofstream(info / "game_library_info.h") << std::format(
        "#define PS_GAME_ABI_HASH 0x{:016x}ull\n#define PS_GAME_TRANSLATOR_HASH 0x{:016x}ull\n"
        "#define PS_GAME_TITLE_ID \"TEST00000\"\n#define PS_GAME_MADE_BY \"test_game_library\"\n"
        "#define PS_GAME_EXECUTABLE_SHA256 {{0}}\n#define PS_GAME_HAS_PPU 1\n#define PS_GAME_HAS_SPU 0\n"
        "#define PS_GAME_SPU_PROGRAM_COUNT 0u\n#define PS_GAME_PPU_NAME ps_selftest_ppu\n",
        abi_hash, ps_game_translator_hash);
    const std::string generated = PSPOCKET_SELFTEST_GENERATED_DIR;
    std::vector<codegen::BuildJob> jobs;
    for (const char* file : {"ps_selftest_ppu_0.c", "ps_selftest_ppu_reg.c"})
        jobs.push_back({.source = generated + "/" + file, .include_dirs = {generated}, .cost = 1, .memory_mb = 100});
    jobs.push_back({.source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE, .include_dirs = {info.string()},
        .defines = {std::format("PSPOCKET_INFO_HASH=0x{:016x}ull", abi_hash)}, .cost = 0.1, .memory_mb = 50});

    codegen::BuildOptions o;
    o.compile.target = "arm64-apple-macos13.0";
    o.compile.cpu = "apple-m1";
    o.compile.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    o.compile.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    o.compile.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    o.compile.defines = {"PSPOCKET_GAME_LIBRARY"};
    o.compile.extra_args = {"-fvisibility=hidden"};
    o.link.platform = "macos";
    o.link.min_os = "13.0";
    o.link.exports = {"_pspocket_game_library"};
    o.link.libraries = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/lib/libSystem.tbd"};
    o.object_dir = (dir / (name + "-objects")).string();
    o.cache_key = std::format("{:016x}", abi_hash);
    int progress_calls = 0;
    bool finished = false;
    o.progress = [&](const codegen::BuildProgress& p) {
        progress_calls++;
        finished = finished || p.phase == codegen::BuildProgress::Phase::Done;
    };
    const fs::path library = dir / (name + ".dylib");
    const codegen::BuildResult r = codegen::build_library(jobs, {}, library.string(), o);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.compiled == 3);
    CHECK(finished);
    CHECK(progress_calls >= 4);

    std::ifstream in(library, std::ios::binary);
    std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    std::string error;
    REQUIRE(codesign::sign(bytes, {.identifier = "com.example.game"}, &error));
    std::ofstream(library, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return library.string();
}

struct Guest {
    GuestMemory mem;
    Guest()
    {
        REQUIRE(mem.reserve());
        REQUIRE(mem.commit(PS_SELFTEST_PPU_IMAGE_BASE, PS_SELFTEST_PPU_IMAGE_SIZE));
        REQUIRE(mem.commit(kBuffer, PS_SELFTEST_PPU_WORDS * 4));
        REQUIRE(mem.commit(kStackBase, kStackSize));
        mem.write_bytes(PS_SELFTEST_PPU_IMAGE_BASE, ps_selftest_ppu_image, PS_SELFTEST_PPU_IMAGE_SIZE);
        for (u32 i = 0; i < PS_SELFTEST_PPU_WORDS; i++) mem.write<u32>(kBuffer + i * 4, PS_SELFTEST_PPU_PATTERN(i));
    }
};

} // namespace

TEST_CASE("game library: made with the device's compiler, loaded through the runtime table, runs the program")
{
    const fs::path dir = temp_dir();
    const std::string library = make_library(dir, "good", ps_game_abi_hash);
    Guest guest;
    game::LoadedLibrary loaded;
    std::string error;
    const size_t before = ps_ppu_dispatch_count();
    const bool ok = game::load_library(library, guest.mem, &loaded, &error);
    INFO(error);
    REQUIRE(ok);
    REQUIRE(loaded.library);
    CHECK(std::string(loaded.library->title_id) == "TEST00000");
    CHECK(loaded.translator_current);
    CHECK(ps_ppu_dispatch_count() == before + loaded.library->ppu_function_count);

    // The benchmark, entered through the dispatch table as the interpreter would enter it.
    const ps_ppu_fn entry = ps_ppu_dispatch_find(PS_SELFTEST_PPU_ENTRY);
    REQUIRE(entry);
    PPUContext c;
    ps_ppu_context_init(&c);
    c.gpr[1] = kStackBase + kStackSize - 0x1000;
    c.gpr[3] = kBuffer;
    c.gpr[4] = PS_SELFTEST_PPU_WORDS;
    c.gpr[5] = PS_SELFTEST_PPU_ITERATIONS;
    c.gpr[6] = PS_SELFTEST_PPU_FIB_N;
    c.lr = 0x100;
    entry(&c, guest.mem.base());
    CHECK(c.gpr[3] == PS_SELFTEST_PPU_EXPECTED);

    loaded.library->ppu_unregister();
    CHECK(ps_ppu_dispatch_count() == before);
    fs::remove_all(dir);
}

TEST_CASE("game library: one made for another runtime, or another executable, is refused before it runs")
{
    const fs::path dir = temp_dir();
    std::string error;
    const size_t before = ps_ppu_dispatch_count();
    {
        const std::string library = make_library(dir, "other-abi", ps_game_abi_hash ^ 1);
        Guest guest;
        CHECK_FALSE(game::load_library(library, guest.mem, nullptr, &error));
        CHECK(error.find("another runtime") != std::string::npos);
        CHECK(ps_ppu_dispatch_count() == before);
    }
    {
        const std::string library = make_library(dir, "good", ps_game_abi_hash);
        Guest guest;
        // Another executable: one of the words the library sampled differs.
        guest.mem.write<u32>(PS_SELFTEST_PPU_IMAGE_BASE, guest.mem.read<u32>(PS_SELFTEST_PPU_IMAGE_BASE) ^ 1);
        CHECK_FALSE(game::load_library(library, guest.mem, nullptr, &error));
        CHECK(error.find("another executable") != std::string::npos);
        CHECK(ps_ppu_dispatch_count() == before);
    }
    fs::remove_all(dir);
}

TEST_CASE("game library: a second build keeps every object it can")
{
    const fs::path dir = temp_dir();
    make_library(dir, "cached", ps_game_abi_hash);
    // The same jobs again: nothing to compile (make_library checks compiled == 3 only the first time).
    const std::string generated = PSPOCKET_SELFTEST_GENERATED_DIR;
    codegen::BuildOptions o;
    o.compile.target = "arm64-apple-macos13.0";
    o.compile.cpu = "apple-m1";
    o.compile.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    o.compile.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    o.compile.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    o.compile.defines = {"PSPOCKET_GAME_LIBRARY"};
    o.compile.extra_args = {"-fvisibility=hidden"};
    o.link.platform = "macos";
    o.link.min_os = "13.0";
    o.link.exports = {"_pspocket_game_library"};
    o.link.libraries = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/lib/libSystem.tbd"};
    o.object_dir = (dir / "cached-objects").string();
    o.cache_key = std::format("{:016x}", ps_game_abi_hash);
    std::vector<codegen::BuildJob> jobs;
    for (const char* file : {"ps_selftest_ppu_0.c", "ps_selftest_ppu_reg.c"})
        jobs.push_back({.source = generated + "/" + file, .include_dirs = {generated}, .cost = 1, .memory_mb = 100});
    jobs.push_back({.source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE, .include_dirs = {(dir / "cached-info").string()},
        .defines = {std::format("PSPOCKET_INFO_HASH=0x{:016x}ull", ps_game_abi_hash)}, .cost = 0.1, .memory_mb = 50});
    const codegen::BuildResult r = codegen::build_library(jobs, {}, (dir / "again.dylib").string(), o);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.compiled == 0);
    CHECK(r.reused == 3);
    fs::remove_all(dir);
}

TEST_CASE("time left: follows the files done, per kind, and lays the rest out over the workers")
{
    using File = codegen::TimeLeft::File;
    codegen::TimeLeft left(2, 1.0, 10.0);
    CHECK(left.seconds_left({}, {{0, 4, 0}, {0, 4, 0}, {0, 2, 0}, {0, 2, 0}}) == Catch::Approx(6.0)); // 4+2 each
    // PPU files take 1.5 times their prediction here: the PPU rate follows; the SPU rate keeps its own prior (run 095:
    // leaning on the others made PPU units look 20% slower than they were).
    for (int i = 0; i < 20; i++) left.finished(0, 1.0, 1.5);
    CHECK(left.rate(0) == Catch::Approx((10.0 + 30.0) / (10.0 + 20.0)));
    CHECK(left.rate(1) == Catch::Approx(1.0));
    // Priors per kind, from the last build on this machine.
    codegen::TimeLeft primed(1, {0.9, 1.2, 0.5}, 100.0);
    CHECK(primed.rate(0) == Catch::Approx(0.9));
    CHECK(primed.rate(1) == Catch::Approx(1.2));
    // SPU files at twice their prediction, PPU at their prediction: each kind keeps its own rate.
    codegen::TimeLeft mixed(1, 1.0, 1.0);
    for (int i = 0; i < 100; i++) {
        mixed.finished(1, 10.0, 20.0);
        mixed.finished(0, 1.0, 1.0);
    }
    CHECK(mixed.rate(1) == Catch::Approx(2.0).epsilon(0.02));
    CHECK(mixed.rate(0) == Catch::Approx(1.0).epsilon(0.02));
    // One file running (predicted 10, 3 s in) and three waiting, at rate 1.5: 15-3=12 on one worker, the rest laid out.
    codegen::TimeLeft one(2, 1.0, 0.0);
    one.finished(0, 10, 15);
    CHECK(one.seconds_left({File{0, 10.0, 3.0}}, {{0, 4, 0}, {0, 4, 0}, {0, 2, 0}}) == Catch::Approx(15.0));
    // A file far past its prediction is given a tenth of it more, not a negative time.
    CHECK(one.seconds_left({File{0, 1.0, 50.0}}, {}) == Catch::Approx(0.15));
}
