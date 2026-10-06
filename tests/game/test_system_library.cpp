// The system library (D-178): a synthetic firmware module (a debug SELF in a /dev_flash, no keys) translated once
// with the device's compiler, as the phone translates the user's firmware; the loader places the module at its fixed
// address, checks its code against the translation and runs its module_start translated, with no interpreter
// fallback. A module whose code differs from its translation stays under the interpreter. The SPU program the module
// carries, laid out as the firmware's overlay programs are, is translated with it: each code segment is a program
// under the key the runtime computes from the module in guest memory, and runs translated (D-191).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/core/be.h"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/system.hpp"
#include "pspocket/ppu/prx_dispatch.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/prepare/system.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"
#include "synthetic.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::ppu::as;
namespace spu_as = pspocket::recomp::spu::as;

namespace {

namespace fs = std::filesystem;
constexpr u32 kMarker = hle::test::kData + 0x1000;

// An SPU ELF laid out as the firmware's programs with overlays are: a small executable segment first (0x3000), the
// one it is entered in (0x3100: $3 = 5 + 1, then stop 0x123) and an overlay (0x3400: $4 = 9, then stop 0x124).
constexpr u32 kSpuMain = 0x180, kSpuOverlay = 0x200, kSpuCodeSize = 0x10; // file offsets in the ELF; bytes of code

std::vector<u8> spu_elf()
{
    std::vector<u8> e(0x220, 0);
    const u8 ident[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::copy(std::begin(ident), std::end(ident), e.begin());
    ps_st16(e.data() + 0x10, 2);  // ET_EXEC
    ps_st16(e.data() + 0x12, 23); // EM_SPU
    ps_st32(e.data() + 0x14, 1);
    ps_st32(e.data() + 0x18, 0x3100); // entry
    ps_st32(e.data() + 0x1C, 0x34);   // phoff
    ps_st16(e.data() + 0x28, 0x34);
    ps_st16(e.data() + 0x2A, 0x20);
    ps_st16(e.data() + 0x2C, 3);
    const u32 segments[3][2] = {{0x100, 0x3000}, {kSpuMain, 0x3100}, {kSpuOverlay, 0x3400}};
    for (u32 i = 0; i < 3; i++) {
        u8* ph = e.data() + 0x34 + 0x20 * i;
        ps_st32(ph + 0x00, 1); // PT_LOAD
        ps_st32(ph + 0x04, segments[i][0]);
        ps_st32(ph + 0x08, segments[i][1]);
        ps_st32(ph + 0x0C, segments[i][1]);
        ps_st32(ph + 0x10, kSpuCodeSize);
        ps_st32(ph + 0x14, kSpuCodeSize);
        ps_st32(ph + 0x18, 5); // R X
        ps_st32(ph + 0x1C, 0x80);
    }
    const u32 code[3][4] = {
        {spu_as::lnop(), spu_as::lnop(), spu_as::lnop(), spu_as::ret()},
        {spu_as::il(3, 5), spu_as::ai(3, 3, 1), spu_as::stop(0x123), spu_as::ret()},
        {spu_as::il(4, 9), spu_as::lnop(), spu_as::stop(0x124), spu_as::ret()},
    };
    for (u32 i = 0; i < 3; i++)
        for (u32 w = 0; w < 4; w++) ps_st32(e.data() + segments[i][0] + 4 * w, code[i][w]);
    return e;
}

// The synthetic PRX exporting `library` (its name at file offset 0x300, its function's NID at 0x408), its
// module_start (segment 0 + 0: prologue, nops, epilogue) storing 0x77 at `marker` in words 3..6 (as test_lle.cpp),
// carrying spu_elf() at segment 1 + test::kPrxExtraData.
std::vector<u8> module_bytes(std::string_view library, u32 nid, u32 marker)
{
    test::PrxOptions prx;
    prx.extra_data = spu_elf();
    std::vector<u8> bytes = test::build_ppu_prx(prx).bytes;
    std::fill(bytes.begin() + 0x300, bytes.begin() + 0x310, u8{0});
    std::copy(library.begin(), library.end(), bytes.begin() + 0x300);
    ps_st32(bytes.data() + 0x408, nid);
    const u32 code[] = {as::lis(5, static_cast<s32>(marker >> 16)), as::ori(5, 5, marker & 0xFFFF), as::li(6, 0x77),
        as::stw(6, 0, 5)};
    for (u32 i = 0; i < 4; i++) ps_st32(bytes.data() + 0x100 + 4 * (3 + i), code[i]);
    test::SelfOptions o;
    o.debug = true;
    o.npdrm = false;
    return test::wrap_in_self(bytes, o);
}

// Runs the code at `ls` of the guest copy at `guest` (kSpuCodeSize bytes) on an SPU, with the program the runtime
// finds for it: the stop code it reaches, and whether lifted code ran it all (no miss).
struct SpuRun {
    const ps_spu_program* program = nullptr;
    u32 stop = 0;
    bool no_miss = false;
    u32 r3 = 0, r4 = 0;
};

SpuRun run_spu(u8* base, u32 guest, u32 ls_addr)
{
    SpuRun out;
    out.program = ps_spu_find_program(ps_spu_hash64(base + guest, kSpuCodeSize), ls_addr, kSpuCodeSize);
    if (!out.program) return out;
    std::vector<u8> ls(PS_SPU_LS_SIZE + 128);
    u8* aligned = ls.data() + ((128 - reinterpret_cast<uintptr_t>(ls.data()) % 128) % 128);
    std::memcpy(aligned + ls_addr, base + guest, kSpuCodeSize);
    SPUContext ctx{};
    ps_spu_context_init(&ctx, aligned, base);
    ps_spu_attach_program_at(&ctx, out.program, ls_addr);
    ctx.pc = ls_addr;
    const u64 misses = ps_spu_miss_count();
    if (ps_spu_run(&ctx) == PS_SPU_STOP_SIGNAL) out.stop = ctx.stop_code;
    out.no_miss = ps_spu_miss_count() == misses;
    out.r3 = ctx.gpr[3].u32[3];
    out.r4 = ctx.gpr[4].u32[3];
    ps_spu_host_detach(&ctx);
    return out;
}

} // namespace

TEST_CASE("system library: a firmware module translated once runs translated where the loader places it")
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-system-library-{}", getpid());
    fs::remove_all(root);
    const fs::path flash = root / "dev_flash", dir = root / "system";
    fs::create_directories(flash / "sys" / "external");
    const std::vector<u8> self = module_bytes("synthLib", 0xDEADBEEF, kMarker);
    std::ofstream(flash / "sys" / "external" / "libsynth.sprx", std::ios::binary)
        .write(reinterpret_cast<const char*>(self.data()), static_cast<std::streamsize>(self.size()));

    prepare::SystemOptions o;
    o.dev_flash = flash.string();
    o.dir = dir.string();
    o.toolchain.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    o.toolchain.sysroot = PSPOCKET_CODEGEN_SYSROOT;
    o.toolchain.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    o.toolchain.entry_source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE;
    o.toolchain.target = "arm64-apple-macos13.0";
    o.toolchain.cpu = "native"; // what the manifest records, as on the phone
    o.toolchain.platform = "macos";
    o.toolchain.min_os = "13.0";
    // First for a game that links none of the firmware's modules: no SPU programs (D-191).
    prepare::Result r = prepare::prepare_system(o);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK_FALSE(fs::exists(dir / "ppu")); // the translated C is not kept
    CHECK_FALSE(fs::exists(dir / "spu"));
    CHECK_FALSE(fs::exists(dir / "spu-images"));
    CHECK(prepare::system_spu_modules(dir.string()).empty());

    loader::LleModules lle(flash.string());
    REQUIRE(lle.index());
    std::string why;
    CHECK(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why));
    // A game importing the module's library, which the host lacks, links the module: worked out without loading it.
    const std::set<std::string, std::less<>> links = lle.choice_for({{"synthLib", {0xDEADBEEF}, {}}});
    CHECK(links == std::set<std::string, std::less<>>{"libsynth.sprx"});
    CHECK_FALSE(lle.decided());
    const std::set<std::string> game(links.begin(), links.end());
    CHECK_FALSE(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why, game));
    CHECK(why.find("adding them") != std::string::npos);
    // A module that carries no SPU programs needs nothing added.
    CHECK(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why, {"libother.sprx"}));
    // Prepared for that game: the module's SPU programs are added, and stay for a game that links nothing.
    o.spu_modules = game;
    r = prepare::prepare_system(o);
    REQUIRE(r.ok);
    CHECK(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why, game));
    o.spu_modules.clear();
    r = prepare::prepare_system(o);
    REQUIRE(r.ok);
    CHECK(prepare::system_spu_modules(dir.string()) == game);
    CHECK(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why, game));

    ps_ppu_dispatch_clear();
    hle::test::Fixture f;
    auto system = loader::SystemLibrary::load(dir.string(), lle, f.mem, &why);
    INFO(why);
    REQUIRE(system);
    CHECK(ppu::prx_dispatch_registered() > 0);

    // Placed at its fixed address, checked against its translation.
    const loader::PrxModule* m = lle.load("synthLib", *f.hle, &why);
    REQUIRE(m);
    REQUIRE(m->segments.size() == 2);
    CHECK(m->segments[0].first == lle.fixed_addresses("libsynth.sprx")[0]);
    CHECK(ppu::in_prx_region(m->segments[0].first));
    CHECK(system->modules_translated() == 1);
    CHECK(system->modules_refused() == 0);

    // module_start runs translated: no miss, and its store happened.
    u8* base = f.base();
    PPUContext ctx;
    ps_ppu_context_init(&ctx);
    ctx.gpr[1] = hle::test::kStacks + 0x10000 - 0x100;
    ctx.gpr[2] = ps_read32(base, m->start + 4);
    ctx.lr = 0x00000100;
    const u64 misses = ps_ppu_dispatch_miss_count();
    ps_ppu_call_indirect(&ctx, base, ps_read32(base, m->start));
    CHECK(ps_ppu_dispatch_miss_count() == misses);
    CHECK(ps_read32(base, kMarker) == 0x77);

    // The SPU program it carries: its code segments are programs of the library, under the keys the runtime computes
    // from the module as placed (a task's or thread's code: the segment holding the entry; an overlay: what the
    // overlay manager copies in), and they run translated. The first, small segment is a program too.
    const u32 elf = m->segments[1].first + test::kPrxExtraData;
    const SpuRun main = run_spu(base, elf + kSpuMain, 0x3100);
    REQUIRE(main.program);
    CHECK(main.stop == 0x123);
    CHECK(main.no_miss);
    CHECK(main.r3 == 6);
    const SpuRun overlay = run_spu(base, elf + kSpuOverlay, 0x3400);
    REQUIRE(overlay.program);
    CHECK(overlay.stop == 0x124);
    CHECK(overlay.no_miss);
    CHECK(overlay.r4 == 9);
    CHECK(ps_spu_find_program(ps_spu_hash64(base + elf + 0x100, kSpuCodeSize), 0x3000, kSpuCodeSize));
    CHECK_FALSE(ps_spu_find_program(ps_spu_hash64(base + elf + kSpuMain, kSpuCodeSize), 0x3200, kSpuCodeSize));

    // Code that is not what was translated (another firmware, a patch) is refused: the interpreter runs it.
    ps_write32(base, m->segments[0].first + 0x40, 0x60000000);
    system->check(*m);
    CHECK(system->modules_refused() == 1);

    // Another firmware: the library is not for it.
    CHECK_FALSE(prepare::system_library_is_current(dir.string(), lle.identity_text() + "x", &why));
    // A library from before its SPU programs were translated (no system_generation): made again.
    {
        std::ifstream in(dir / "manifest.json");
        std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t at = manifest.find("  \"system_generation\": 2,\n");
        REQUIRE(at != std::string::npos);
        manifest.erase(at, std::string("  \"system_generation\": 2,\n").size());
        std::ofstream(dir / "manifest.json", std::ios::trunc) << manifest;
    }
    CHECK_FALSE(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why));
    CHECK(why.find("preparing it again") != std::string::npos);
    ps_ppu_dispatch_clear();
    fs::remove_all(root);
}
