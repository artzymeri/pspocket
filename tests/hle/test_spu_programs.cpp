// SPU programs as SPURS loads them (runtime/hle/src/spu_programs.cpp, D-076): the lifted program a load attaches, the
// dump of the images that have none, and a DMA over an attached program's code dropping it. Synthetic: the "code" is
// a byte pattern and the lifted program a hand-made descriptor.

#include <catch_amalgamated.hpp>

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
    ps_spu_program program{"spu_test", ps_spu_hash64(base + kGuest, kSize), kLs, kSize, PS_SPU_XFLOAT_APPROXIMATE, 1,
        entries, nullptr};
    ps_spu_register_program(&program);
    Spu spu(base);
    const hle::SpuProgramStats before = hle::spu_program_stats();

    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Task, kGuest, kLs, kSize, kLs});
    CHECK(spu.ctx.program == &program);
    CHECK(ps_spu_lookup(&spu.ctx, kLs) == &lifted_entry);

    // The same code at another LS address is another program (lifted code is specific to its address)...
    hle::spurs::attach_program(spu.ctx, base, {ImageKind::Job, kGuest, kLs + 0x400, kSize, kLs + 0x400});
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
    ps_spu_program program{"spu_test", 1, kLs, kSize, PS_SPU_XFLOAT_APPROXIMATE, 1, entries, nullptr};
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
    ps_spu_unregister_program(&program);
}
