// The system library (D-178): a synthetic firmware module (a debug SELF in a /dev_flash, no keys) translated once
// with the device's compiler, as the phone translates the user's firmware; the loader places the module at its fixed
// address, checks its code against the translation and runs its module_start translated, with no interpreter
// fallback. A module whose code differs from its translation stays under the interpreter.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "hle_test_util.hpp"
#include "pspocket/core/be.h"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/system.hpp"
#include "pspocket/ppu/prx_dispatch.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/prepare/system.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "synthetic.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::ppu::as;

namespace {

namespace fs = std::filesystem;
constexpr u32 kMarker = hle::test::kData + 0x1000;

// The synthetic PRX exporting `library` (its name at file offset 0x300, its function's NID at 0x408), its
// module_start (segment 0 + 0: prologue, nops, epilogue) storing 0x77 at `marker` in words 3..6 (as test_lle.cpp).
std::vector<u8> module_bytes(std::string_view library, u32 nid, u32 marker)
{
    std::vector<u8> bytes = test::build_ppu_prx().bytes;
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
    const prepare::Result r = prepare::prepare_system(o);
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK_FALSE(fs::exists(dir / "ppu")); // the translated C is not kept

    ps_ppu_dispatch_clear();
    hle::test::Fixture f;
    loader::LleModules lle(flash.string());
    REQUIRE(lle.index());
    std::string why;
    CHECK(prepare::system_library_is_current(dir.string(), lle.identity_text(), &why));
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

    // Code that is not what was translated (another firmware, a patch) is refused: the interpreter runs it.
    ps_write32(base, m->segments[0].first + 0x40, 0x60000000);
    system->check(*m);
    CHECK(system->modules_refused() == 1);

    // Another firmware: the library is not for it.
    CHECK_FALSE(prepare::system_library_is_current(dir.string(), lle.identity_text() + "x", &why));
    ps_ppu_dispatch_clear();
    fs::remove_all(root);
}
