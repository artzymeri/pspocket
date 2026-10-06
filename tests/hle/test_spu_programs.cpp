// SPU programs as SPURS loads them (runtime/hle/src/spu_programs.cpp, D-076): the lifted program a load attaches, the
// dump of the images that have none, a DMA over an attached program's code dropping it, the modules programs load
// themselves (D-079) and relocatable programs (D-081). Synthetic: the "code" is a byte pattern and the lifted program a
// hand-made descriptor.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_programs.hpp"

using namespace pspocket;
using hle::spurs::ImageKind;

namespace {

constexpr u32 kGuest = 0x10000; // where the image sits in guest memory
constexpr u32 kSize = 0x200;
constexpr u32 kLs = 0x3000; // where SPURS put it

void lifted_entry(SPUContext*, uint8_t*) {}

// An SPU context over its own local store and the given guest memory.
struct Spu {
    std::unique_ptr<u8[], void (*)(u8*)> ls{static_cast<u8*>(std::aligned_alloc(128, PS_SPU_LS_SIZE)),
        [](u8* p) { std::free(p); }};
    SPUContext ctx{};

    explicit Spu(u8* membase)
    {
        std::memset(ls.get(), 0, PS_SPU_LS_SIZE);
        ps_spu_context_init(&ctx, ls.get(), membase);
    }
    ~Spu() { ps_spu_host_detach(&ctx); }
};

std::vector<u8> read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("SPU programs: a load attaches the lifted program with the image's key", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    for (u32 i = 0; i < kSize; i++) base[kGuest + i] = static_cast<u8>(i * 7 + 3);

    const ps_spu_entry entries[] = {{kLs, &lifted_entry}};
    ps_spu_program program{"spu_test", ps_spu_hash64(base + kGuest, kSize), kLs, kSize, PS_SPU_XFLOAT_APPROXIMATE, 0, 1,
        entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    Spu spu(base);
    const hle::SpuProgramStats before = hle::spu_program_stats();

    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Task, kGuest, kLs, kSize, kLs});
    CHECK(spu.ctx.program == &program);
    CHECK(ps_spu_lookup(&spu.ctx, kLs) == &lifted_entry);

    // The same code at another LS address is another program (fixed lifted code is specific to its address)...
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Task, kGuest, kLs + 0x400, kSize, kLs + 0x400});
    CHECK(spu.ctx.program == nullptr);
    // ...and other code at the same address too. The key is the guest copy's, whatever the LS copy holds.
    base[kGuest + 0x40] ^= 1;
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Task, kGuest, kLs, kSize, kLs});
    CHECK(spu.ctx.program == nullptr);
    base[kGuest + 0x40] ^= 1;
    std::memset(spu.ls.get() + kLs, 0xAB, kSize);
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Task, kGuest, kLs, kSize, kLs});
    CHECK(spu.ctx.program == &program);

    const hle::SpuProgramStats after = hle::spu_program_stats();
    CHECK(after.attached - before.attached == 2);
    CHECK(after.unknown - before.unknown == 2);
    CHECK(after.images - before.images == 2);
    ps_spu_unregister_program(&program);
}

TEST_CASE("SPU programs: an image loaded again is the program found before, unless its bytes or the registry changed (D-109)",
    "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    for (u32 i = 0; i < kSize; i++) base[kGuest + i] = static_cast<u8>(i * 11 + 5);
    const ps_spu_entry entries[] = {{kLs, &lifted_entry}};
    ps_spu_program program{"spu_test_again", ps_spu_hash64(base + kGuest, kSize), kLs, kSize,
        PS_SPU_XFLOAT_APPROXIMATE, 1, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    Spu spu(base);
    const hle::spurs::SpuImage job{ImageKind::Job, kGuest, kLs, kSize, kLs};
    const hle::SpuProgramStats before = hle::spu_program_stats();

    for (int i = 0; i < 3; i++) CHECK(hle::spurs::identify_program(base, job) == &program);
    base[kGuest + kSize - 1] ^= 0x80; // another job binary at the same address
    CHECK(hle::spurs::identify_program(base, job) == nullptr);
    base[kGuest + kSize - 1] ^= 0x80;
    CHECK(hle::spurs::identify_program(base, job) == &program);
    CHECK(hle::spu_program_stats().attached - before.attached == 4);

    // Unregistered, it is not found again; another program with the key is.
    ps_spu_unregister_program(&program);
    CHECK(hle::spurs::identify_program(base, job) == nullptr);
    ps_spu_program again = program;
    again.name = "spu_test_again_2";
    ps_spu_register_program(&again);
    CHECK(hle::spurs::identify_program(base, job) == &again);
    ps_spu_unregister_program(&again);
}

TEST_CASE("SPU programs: a relocatable program attaches wherever its image sits (D-081)", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    for (u32 i = 0; i < kSize; i++) base[kGuest + i] = static_cast<u8>(i * 13 + 1);

    const ps_spu_entry entries[] = {{kLs, &lifted_entry}};
    ps_spu_program program{"spu_test_reloc", ps_spu_hash64(base + kGuest, kSize), kLs, kSize, PS_SPU_XFLOAT_APPROXIMATE,
        1, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    Spu spu(base);

    for (const u32 at : {kLs, kLs + 0x400, kLs - 0x1000, 0x3FF00u}) {
        INFO(std::format("job at LS {:#x}", at));
        hle::spurs::attach_program(spu.ctx, base, {ImageKind::Job, kGuest, at, kSize, at});
        REQUIRE(spu.ctx.program == &program);
        CHECK(spu.ctx.home == &program);
        CHECK(spu.ctx.reloc == ((at - kLs) & PS_SPU_LS_MASK));
        CHECK(ps_spu_lookup(&spu.ctx, at) == &lifted_entry);
        CHECK(ps_spu_lookup(&spu.ctx, at + 4) == nullptr);
        if (at != kLs) CHECK(ps_spu_lookup(&spu.ctx, kLs) == nullptr);
    }
    // The program decides, not the kind of image: a relocatable program runs wherever any image with its code sits.
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Task, kGuest, kLs + 0x400, kSize, kLs + 0x400});
    CHECK(spu.ctx.program == &program);
    CHECK(spu.ctx.reloc == 0x400);
    ps_spu_unregister_program(&program);
}

TEST_CASE("SPU programs: images without a lifted program are saved once, with where they go", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    for (u32 i = 0; i < kSize; i++) base[kGuest + i] = static_cast<u8>(i ^ 0x5A);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("pspocket-spu-dump-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    hle::set_spu_dump_dir(dir.string());
    Spu spu(base);

    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Job, kGuest, 0x4C00, kSize, 0x4C00});
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Job, kGuest, 0x4C00, kSize, 0x4C00});
    // A job binary is position independent: the same one elsewhere is the same image (D-081).
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Job, kGuest, 0x9000, kSize, 0x9000});
    hle::set_spu_dump_dir({});

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) files.push_back(e.path());
    REQUIRE(files.size() == 2); // once
    const std::string name = std::format("spu_{:016x}_04c00", ps_spu_hash64(base + kGuest, kSize));
    const std::vector<u8> code = read_file(dir / (name + ".bin"));
    REQUIRE(code.size() == kSize);
    CHECK(std::memcmp(code.data(), base + kGuest, kSize) == 0);
    const std::vector<u8> meta = read_file(dir / (name + ".txt"));
    CHECK(std::string(meta.begin(), meta.end()) == "kind job\nguest 0x10000\nbase 0x4c00\nsize 0x200\nentry 0x4c00\n");
    std::filesystem::remove_all(dir);
}

TEST_CASE("SPU programs: a DMA over the attached program's code drops it", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    const ps_spu_entry entries[] = {{kLs, &lifted_entry}};
    ps_spu_program program{"spu_test", 1, kLs, kSize, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    Spu spu(base);
    ps_spu_attach_program(&spu.ctx, &program);

    const auto get = [&](u32 lsa, u32 size) {
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_LSA, lsa) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAH, 0) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAL, kGuest) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Size, size) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_TagID, 1) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
    };
    get(kLs + kSize, 0x100); // right after the code: stays
    CHECK(spu.ctx.program == &program);
    get(kLs - 0x100, 0x100); // right before
    CHECK(spu.ctx.program == &program);
    get(kLs + 0x80, 0x10); // over it: the lifted code would be stale
    CHECK(spu.ctx.program == nullptr);
    CHECK(spu.ctx.home == nullptr);

    // A program that says which words are its instructions keeps them through DMAs into the rest of its range: a job
    // copies its buffers into its own data and bss (regression, run 057: every job ran interpreted after its first
    // run). Instructions in the first 0x100 bytes, data after.
    uint32_t code_words[kSize / 4 / 32] = {};
    for (u32 w = 0; w < 0x100 / 4; w++) code_words[w / 32] |= 1u << (w % 32);
    ps_spu_program partial{"spu_test_data", 2, kLs, kSize, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, code_words};
    ps_spu_register_program(&partial);
    ps_spu_attach_program(&spu.ctx, &partial);
    get(kLs + 0x100, 0x100); // its data: stays
    CHECK(spu.ctx.program == &partial);
    get(kLs + 0xF0, 0x10); // the last instruction words
    CHECK(spu.ctx.program == nullptr);
    ps_spu_unregister_program(&partial);
    ps_spu_unregister_program(&program);
}

namespace {

void module_entry(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, ctx->pc, 0x42); }

} // namespace

TEST_CASE("SPU programs: code a program copies in itself runs lifted when execution reaches it (D-079)", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    constexpr u32 kModule = 0x8000, kModuleSize = 0x800;
    // The module's code as the interpreter would run it: `stop 0x7` at its entry.
    ps_write32(base, kGuest, 0x00000007);
    for (u32 i = 4; i < kModuleSize; i += 4) ps_write32(base, kGuest + i, 0x40200000 | i); // nops with distinct bits
    const ps_spu_entry entries[] = {{kModule, &module_entry}};
    ps_spu_program program{"spu_module", ps_spu_hash64(base + kGuest, kModuleSize), kModule, kModuleSize,
        PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    hle::enable_spu_modules();
    Spu spu(base);
    const auto get = [&](u32 lsa, u32 eal, u32 size) {
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_LSA, lsa) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAH, 0) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAL, eal) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Size, size) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_TagID, 2) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
    };
    const auto run_at_module = [&] {
        spu.ctx.pc = kModule;
        REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
        return spu.ctx.stop_code;
    };
    const hle::SpuProgramStats before = hle::spu_program_stats();

    // Copied in two pieces, as one module: its lifted program runs.
    get(kModule, kGuest, 0x400);
    get(kModule + 0x400, kGuest + 0x400, 0x400);
    CHECK(run_at_module() == 0x42);
    CHECK(spu.ctx.program == &program);
    CHECK(hle::spu_program_stats().modules - before.modules == 1);

    // Data the program copies in afterwards, more pieces than the run table holds, leaves the module known: it ran
    // (regression, run 054: the oldest run was replaced, and modules that were still in local store were interpreted).
    for (u32 i = 0; i < 300; i++) get(0x20000 + i * 0x20, kGuest + 0x1000 + i * 0x40, 0x10);
    ps_spu_attach_program(&spu.ctx, nullptr); // back in the program's own code: the module is found again
    CHECK(run_at_module() == 0x42);
    CHECK(spu.ctx.program == &program);

    // Copied again, then patched in local store before it runs: its lifted code could be stale, so it is interpreted.
    ps_spu_attach_program(&spu.ctx, nullptr);
    get(kModule, kGuest, 0x400);
    get(kModule + 0x400, kGuest + 0x400, 0x400);
    ps_write32(spu.ls.get(), kModule + 0x100, 0x40200000);
    CHECK(run_at_module() == 0x7);
    CHECK(spu.ctx.program == nullptr);

    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&program);
}

TEST_CASE("SPU programs: a module shares its 1 KB block with data, and outlasts hundreds of data copies", "[hle][spu]")
{
    // Regression, run 066: GTA V's dependency tasks copy a small module to LS 0x71f0 and their data to 0x7310, in the
    // same 1 KB block; the data's run took the block, and the module was interpreted every time it ran.
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    constexpr u32 kModule = 0x71F0, kModuleSize = 0x110;
    ps_write32(base, kGuest, 0x00000007); // `stop 0x7` at its entry, for the interpreter
    for (u32 i = 4; i < kModuleSize; i += 4) ps_write32(base, kGuest + i, 0x40200000 | i);
    const ps_spu_entry entries[] = {{kModule, &module_entry}};
    ps_spu_program program{"spu_small_module", ps_spu_hash64(base + kGuest, kModuleSize), kModule, kModuleSize,
        PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    hle::enable_spu_modules();
    Spu spu(base);
    const auto get = [&](u32 lsa, u32 eal, u32 size) {
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_LSA, lsa) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAH, 0) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAL, eal) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Size, size) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_TagID, 2) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
    };
    get(kModule, kGuest, kModuleSize);
    get(0x7310, kGuest + 0x2000, 0x2FB0);
    spu.ctx.pc = kModule;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x42);
    CHECK(spu.ctx.program == &program);

    // Loaded again, then more pieces of data than the old table held before it first runs (regression, run 068: the
    // module, the oldest run that had not run yet, was replaced).
    ps_spu_attach_program(&spu.ctx, nullptr);
    get(kModule, kGuest, kModuleSize);
    for (u32 i = 0; i < 300; i++) get(0x20000 + i * 0x20, kGuest + 0x6000 + i * 0x20, 0x10);
    spu.ctx.pc = kModule;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x42);
    CHECK(spu.ctx.program == &program);

    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&program);
}

TEST_CASE("SPU programs: a module with data copied over its tail still runs lifted at its head", "[hle][spu]")
{
    // Regression, run 109: Assassin's Creed's tasks copy a 42 KB module to 0x4c00, then data over its last 10 KB, and
    // run the module's head; the data's copy dropped the module, which then ran interpreted (64 billion instructions in
    // six minutes).
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    constexpr u32 kModule = 0x4C00, kModuleSize = 0x1000;
    ps_write32(base, kGuest, 0x00000007); // `stop 0x7` at its entry, for the interpreter
    for (u32 i = 4; i < kModuleSize; i += 4) ps_write32(base, kGuest + i, 0x40200000 | i);
    const ps_spu_entry entries[] = {{kModule, &module_entry}, {kModule + 0x400, &module_entry}};
    std::array<u32, kModuleSize / 128> code_words{}; // its first three quarters are instructions, the rest data
    for (u32 w = 0; w < kModuleSize * 3 / 16; w++) code_words[w >> 5] |= 1u << (w & 31);
    ps_spu_program program{"spu_tail_module", ps_spu_hash64(base + kGuest, kModuleSize), kModule, kModuleSize,
        PS_SPU_XFLOAT_APPROXIMATE, 0, 2, entries, nullptr, code_words.data()};
    ps_spu_register_program(&program);
    hle::enable_spu_modules();
    Spu spu(base);
    const auto get = [&](u32 lsa, u32 eal, u32 size) {
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_LSA, lsa) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAH, 0) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAL, eal) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Size, size) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_TagID, 2) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
    };
    const auto run_at = [&](u32 pc) {
        ps_spu_attach_program(&spu.ctx, nullptr);
        spu.ctx.pc = pc;
        REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
        return spu.ctx.stop_code;
    };
    // The module in four pieces, then data over its last quarter from elsewhere: its head runs lifted.
    for (u32 i = 0; i < 4; i++) get(kModule + i * 0x400, kGuest + i * 0x400, 0x400);
    get(kModule + 0xC00, kGuest + 0x8000, 0x400);
    CHECK(run_at(kModule) == 0x42);
    CHECK(spu.ctx.program == &program);
    // Data over its code: the rest is interpreted (a branch into the copied part would run stale code), also when
    // the module ran lifted before the copy.
    get(kModule + 0x800, kGuest + 0x8000, 0x400);
    CHECK(run_at(kModule) == 0x7);
    CHECK(spu.ctx.program == nullptr);
    for (u32 i = 0; i < 4; i++) get(kModule + i * 0x400, kGuest + i * 0x400, 0x400);
    get(kModule, kGuest + 0x8000, 0x400);
    CHECK(run_at(kModule) == 0x0); // the data (zeros: `stop 0`) at the entry, interpreted
    CHECK(spu.ctx.program == nullptr);
    CHECK(run_at(kModule + 0x400) == 0x0); // its second quarter, interpreted to the zeros past it
    CHECK(spu.ctx.program == nullptr);
    // Covered entirely: gone.
    for (u32 i = 0; i < 4; i++) get(kModule + i * 0x400, kGuest + i * 0x400, 0x400);
    get(kModule, kGuest + 0x8000, kModuleSize);
    std::memcpy(spu.ls.get() + kModule, base + kGuest, 4); // even with its first word back, not a module any more
    CHECK(run_at(kModule) == 0x7);
    CHECK(spu.ctx.program == nullptr);

    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&program);
}

namespace {

constexpr u32 kHome = 0x1000;  // a task's code, lifted where it runs
constexpr u32 kLifted = 0x8000; // where the module was lifted
constexpr u32 kLoaded = 0xA400; // where the task copies it
u32 g_home_runs = 0;

// The task's code: branches to the module, which returns to kHome + 4.
void home_entry(SPUContext* ctx, uint8_t*)
{
    g_home_runs++;
    ctx->pc = kLoaded;
}
void home_return(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, ctx->pc, 0x43); }
// The module's code (relocatable): records where it runs, then returns to its caller.
u32 g_module_reloc = ~0u;
void reloc_module_entry(SPUContext* ctx, uint8_t*)
{
    g_module_reloc = ctx->reloc;
    ctx->pc = kHome + 4;
}

} // namespace

TEST_CASE("SPU programs: a relocatable module runs where it was copied, then its caller's code runs lifted again (D-081)",
    "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    constexpr u32 kModuleSize = 0x400;
    for (u32 i = 0; i < kModuleSize; i += 4) ps_write32(base, kGuest + i, 0x40200000 | i); // nops with distinct bits
    const ps_spu_entry module_entries[] = {{kLifted, &reloc_module_entry}};
    ps_spu_program module{"spu_module_reloc", ps_spu_hash64(base + kGuest, kModuleSize), kLifted, kModuleSize,
        PS_SPU_XFLOAT_APPROXIMATE, 1, 1, module_entries, nullptr, nullptr};
    const ps_spu_entry home_entries[] = {{kHome, &home_entry}, {kHome + 4, &home_return}};
    ps_spu_program home{"spu_home", 2, kHome, 0x100, PS_SPU_XFLOAT_APPROXIMATE, 0, 2, home_entries, nullptr, nullptr};
    ps_spu_register_program(&module);
    ps_spu_register_program(&home);
    hle::enable_spu_modules();
    Spu spu(base);

    // The task copies the module in (one GET), then calls it.
    ps_spu_attach_program(&spu.ctx, &home);
    REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_LSA, kLoaded) == 0);
    REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAH, 0) == 0);
    REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAL, kGuest) == 0);
    REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Size, kModuleSize) == 0);
    REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_TagID, 3) == 0);
    REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
    CHECK(spu.ctx.home == &home); // the GET was not over the task's code
    spu.ctx.pc = kHome;
    g_home_runs = 0;
    const u64 misses = ps_spu_miss_count();
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x43); // the task's lifted code took the return
    CHECK(g_home_runs == 1);
    CHECK(g_module_reloc == kLoaded - kLifted);
    CHECK(spu.ctx.program == &home);
    CHECK(spu.ctx.reloc == 0);
    CHECK(ps_spu_miss_count() == misses);

    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&module);
    ps_spu_unregister_program(&home);
}

TEST_CASE("SPU programs: a relocatable module is saved once, its seeds where the saved copy has them (D-081)", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    constexpr u32 kModuleSize = 0x400, kFirst = 0x6000, kSecond = 0x9800;
    ps_write32(base, kGuest, 0x00000007);        // stop 0x7 at its start...
    ps_write32(base, kGuest + 0x10, 0x00000008); // ...and stop 0x8 at +0x10
    for (u32 i = 0x20; i < kModuleSize; i += 4) ps_write32(base, kGuest + i, 0x40200000 | i);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("pspocket-spu-modules-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    hle::set_spu_dump_dir(dir.string());
    hle::enable_spu_modules();
    Spu spu(base);
    const auto load_and_run = [&](u32 at, u32 entry) {
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_LSA, at) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAH, 0) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_EAL, kGuest) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Size, kModuleSize) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_TagID, 4) == 0);
        REQUIRE(ps_spu_wrch(&spu.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
        spu.ctx.pc = at + entry;
        REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
        return spu.ctx.stop_code;
    };
    CHECK(load_and_run(kFirst, 0) == 0x7);
    CHECK(load_and_run(kSecond, 0x10) == 0x8); // entered elsewhere, at another address
    hle::set_spu_dump_dir({});

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) files.push_back(e.path());
    REQUIRE(files.size() == 2);
    const std::string name = std::format("spu_{:016x}_{:05x}", ps_spu_hash64(base + kGuest, kModuleSize), kFirst);
    const std::vector<u8> meta = read_file(dir / (name + ".txt"));
    CHECK(std::string(meta.begin(), meta.end()) ==
          std::format("kind module\nguest {:#x}\nbase {:#x}\nsize {:#x}\nentry {:#x}\nseed {:#x}\nseed {:#x}\n", kGuest,
              kFirst, kModuleSize, kFirst, kFirst, kFirst + 0x10));
    std::filesystem::remove_all(dir);
    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
}

TEST_CASE("SPU programs: a task's modules stay known across its context switch to another SPU (D-082)", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(kGuest, 0x10000));
    u8* base = mem.base();
    constexpr u32 kModule = 0x8000, kModuleSize = 0x800, kSaveArea = 0x40000;
    ps_write32(base, kGuest, 0x00000007); // interpreted, the module stops with 0x7; lifted, with 0x42
    for (u32 i = 4; i < kModuleSize; i += 4) ps_write32(base, kGuest + i, 0x40200000 | i);
    const ps_spu_entry entries[] = {{kModule, &module_entry}};
    ps_spu_program program{"spu_module_switch", ps_spu_hash64(base + kGuest, kModuleSize), kModule, kModuleSize,
        PS_SPU_XFLOAT_APPROXIMATE, 1, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&program);
    hle::enable_spu_modules();
    const auto run_at_module = [](Spu& spu) {
        spu.ctx.pc = kModule;
        REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
        return spu.ctx.stop_code;
    };

    Spu first(base);
    REQUIRE(ps_spu_wrch(&first.ctx, 0, PS_MFC_LSA, kModule) == 0);
    REQUIRE(ps_spu_wrch(&first.ctx, 0, PS_MFC_EAH, 0) == 0);
    REQUIRE(ps_spu_wrch(&first.ctx, 0, PS_MFC_EAL, kGuest) == 0);
    REQUIRE(ps_spu_wrch(&first.ctx, 0, PS_MFC_Size, kModuleSize) == 0);
    REQUIRE(ps_spu_wrch(&first.ctx, 0, PS_MFC_TagID, 5) == 0);
    REQUIRE(ps_spu_wrch(&first.ctx, 0, PS_MFC_Cmd, PS_MFC_GET) == 0);
    CHECK(run_at_module(first) == 0x42);
    // The task yields: its context (local store included) is saved, the module runs with it.
    hle::spurs::save_module_runs(first.ctx, kSaveArea, [](u32, u32) { return true; });

    // It resumes on another SPU: local store comes back by copy, not by DMA.
    Spu second(base);
    std::memcpy(second.ls.get() + kModule, first.ls.get() + kModule, kModuleSize);
    CHECK(run_at_module(second) == 0x7); // without the saved runs: unknown code, interpreted
    std::memcpy(second.ls.get() + kModule, first.ls.get() + kModule, kModuleSize);
    hle::spurs::forget_module_runs(second.ctx);
    hle::spurs::restore_module_runs(second.ctx, kSaveArea);
    ps_spu_attach_program(&second.ctx, nullptr);
    CHECK(run_at_module(second) == 0x42);
    CHECK(second.ctx.program == &program);

    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&program);
}

namespace {

void jump_to_kernel_exit(SPUContext* ctx, uint8_t*) { ctx->pc = 0x808; }
void call_task_syscall(SPUContext* ctx, uint8_t*) { ctx->pc = 0xA70; }
void jump_to_unlifted(SPUContext* ctx, uint8_t*) { ctx->pc = 0xA10; }

} // namespace

TEST_CASE("SPU programs: the worker's stops in local store run as lifted stand-ins, not misses (D-087)", "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    hle::enable_spu_modules();
    Spu spu(mem.base());
    ps_write32(spu.ls.get(), 0x808, 0x808); // `stop 0x808`, as the worker plants it
    const ps_spu_entry entries[] = {{0xA00, &jump_to_kernel_exit}};
    ps_spu_program pm{"spu_pm", 3, 0xA00, 0x100, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&pm);
    const ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    ps_spu_set_miss_policy(PS_SPU_MISS_ABORT); // a miss would end the test
    const u64 misses = ps_spu_miss_count();

    ps_spu_attach_program(&spu.ctx, &pm);
    spu.ctx.pc = 0xA00;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x808);
    CHECK(ps_spu_miss_count() == misses);
    CHECK(spu.ctx.home == &pm);
    // Back to the module's own code, lifted: its entry runs again and stops at the exit again.
    spu.ctx.pc = 0xA00;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x808);
    CHECK(ps_spu_miss_count() == misses);

    // Something else at that address: no stand-in (the stop is checked in local store).
    ps_write32(spu.ls.get(), 0x808, 0x00000123); // `stop 0x123`
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    spu.ctx.pc = 0xA00;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x123);
    CHECK(ps_spu_miss_count() == misses + 1);

    ps_spu_set_miss_policy(policy);
    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&pm);
}

TEST_CASE("SPU programs: a task's syscall entry runs as a lifted stand-in, not a miss", "[hle][spu]")
{
    // Run 066: every task syscall (the stop at 0xA70) missed, logged under the miss lock and interpreted one stop.
    GuestMemory mem;
    REQUIRE(mem.reserve());
    hle::enable_spu_modules();
    Spu spu(mem.base());
    ps_write32(spu.ls.get(), 0xA70, 0xA70); // `stop 0xA70`, as the taskset's policy plants it
    const ps_spu_entry entries[] = {{0x3000, &call_task_syscall}};
    ps_spu_program task{"spu_task", 5, 0x3000, 0x100, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&task);
    const ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    ps_spu_set_miss_policy(PS_SPU_MISS_ABORT);
    const u64 misses = ps_spu_miss_count();

    ps_spu_attach_program(&spu.ctx, &task);
    spu.ctx.pc = 0x3000;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0xA70);
    CHECK(spu.ctx.pc == 0xA74);
    CHECK(ps_spu_miss_count() == misses);

    ps_spu_set_miss_policy(policy);
    ps_spu_set_resolver(nullptr);
    ps_spu_set_get_observer(nullptr);
    ps_spu_unregister_program(&task);
}

TEST_CASE("SPU programs: a miss is also written to the miss log, in the words scripts/lift-spu.sh reads (D-117)",
    "[hle][spu]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    Spu spu(mem.base());
    ps_write32(spu.ls.get(), 0xA10, 0x00000123); // `stop 0x123`, no lifted code for it
    const ps_spu_entry entries[] = {{0xA00, &jump_to_unlifted}};
    ps_spu_program p{"spu_misslog", 7, 0xA00, 0x100, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr, nullptr};
    ps_spu_register_program(&p);
    const auto path = std::filesystem::temp_directory_path() / std::format("pspocket-misses-{}.log", getpid());
    std::filesystem::remove(path);
    const ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    ps_spu_set_miss_log(path.c_str());
    ps_spu_attach_program(&spu.ctx, &p);
    spu.ctx.pc = 0xA00;
    REQUIRE(ps_spu_run(&spu.ctx) == PS_SPU_STOP_SIGNAL);
    CHECK(spu.ctx.stop_code == 0x123);
    ps_spu_set_miss_log(nullptr);
    ps_spu_set_miss_policy(policy);
    ps_spu_unregister_program(&p);
    const std::vector<u8> text = read_file(path);
    const std::string log(text.begin(), text.end());
    CHECK(log.find("no lifted code at 0x00a10 in program spu_misslog") != std::string::npos);
    std::filesystem::remove(path);
}
