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
#include "pspocket/core/memory_map.h"
#include "pspocket/loader/game_modules.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/system.hpp"
#include "pspocket/ppu/prx_dispatch.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/prepare/modules.hpp"
#include "pspocket/prepare/system.hpp"
#include "pspocket/recomp/prx_place.hpp"
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

namespace {

prepare::Toolchain mac_toolchain()
{
    prepare::Toolchain t;
    t.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    t.sysroot = PSPOCKET_CODEGEN_SYSROOT;
    t.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    t.entry_source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE;
    t.target = "arm64-apple-macos13.0";
    t.cpu = "native";
    t.platform = "macos";
    t.min_os = "13.0";
    return t;
}

// The game's module loaded from /app_home as sys_prx_load_module loads it: the module, or null.
const loader::PrxModule* load_game_module(loader::RuntimeModules& modules, std::string_view path)
{
    u32 id = 0;
    if (modules.load(path, &id) != 0) return nullptr;
    return modules.module(id);
}

// Its module_start called: whether any call went to the interpreter, and its marker.
bool start_runs_translated(hle::test::Fixture& f, const loader::PrxModule& m)
{
    u8* base = f.base();
    ps_write32(base, kMarker, 0);
    PPUContext ctx;
    ps_ppu_context_init(&ctx);
    ctx.gpr[1] = hle::test::kStacks + 0x10000 - 0x100;
    ctx.gpr[2] = ps_read32(base, m.start + 4);
    ctx.lr = 0x00000100;
    const u64 misses = ps_ppu_dispatch_miss_count();
    ps_ppu_call_indirect(&ctx, base, ps_read32(base, m.start));
    CHECK(ps_read32(base, kMarker) == 0x77); // as the interpreter runs it (CLAUDE.md rule 3)
    return ps_ppu_dispatch_miss_count() == misses;
}

} // namespace

TEST_CASE("game modules: a module the game loads is saved, translated at its slot, and runs translated next time "
          "(D-238)")
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-game-modules-{}", getpid());
    fs::remove_all(root);
    const fs::path usrdir = root / "usrdir", saved = root / "prx", dir = root / "modules";
    fs::create_directories(usrdir);
    const std::vector<u8> self = module_bytes("synthLib", 0xDEADBEEF, kMarker);
    std::ofstream(usrdir / "libgame.sprx", std::ios::binary)
        .write(reinterpret_cast<const char*>(self.data()), static_cast<std::streamsize>(self.size()));
    const std::string exe_sha(64, 'e');
    const std::string path = "/app_home/libgame.sprx";
    ps_ppu_dispatch_clear();
    const ps_ppu_miss_policy policy = ps_ppu_get_miss_policy();
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);

    // Session 1: nothing translated. The module goes to main memory, runs under the interpreter and is saved once.
    {
        hle::test::Fixture f;
        REQUIRE(f.kernel->vfs_mount("/app_home", usrdir.string(), true));
        loader::RuntimeModules modules(*f.hle, nullptr, nullptr);
        modules.set_capture_dir(saved.string());
        const loader::PrxModule* m = load_game_module(modules, path);
        REQUIRE(m);
        CHECK_FALSE(ppu::in_prx_region(m->segments[0].first));
        CHECK(m->identity.size() == 64);
        CHECK(fs::exists(saved / (m->identity + ".elf")));
        CHECK_FALSE(start_runs_translated(f, *m));
        REQUIRE(load_game_module(modules, path));
        size_t files = 0;
        for ([[maybe_unused]] const auto& e : fs::directory_iterator(saved)) files++;
        CHECK(files == 2); // its .elf and .txt, once
    }
    std::string why;
    CHECK_FALSE(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, PS_MEM_PRX_BASE, &why));

    // Prepared: a slot at the top of the PRX region, its code and its SPU program translated.
    prepare::ModulesOptions o;
    o.inputs = {saved.string()};
    o.title_id = "TEST00000";
    o.executable_sha256 = exe_sha;
    o.floor = PS_MEM_PRX_BASE;
    o.dir = dir.string();
    o.toolchain = mac_toolchain();
    const prepare::ModulesResult r = prepare::prepare_game_modules(o);
    INFO(r.library.error);
    REQUIRE(r.library.ok);
    CHECK(r.modules == 1);
    CHECK(r.without_slot == 0);
    CHECK(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, PS_MEM_PRX_BASE, &why));
    CHECK_FALSE(prepare::game_modules_are_current(dir.string(), std::string(64, 'f'), {saved.string()}, PS_MEM_PRX_BASE,
        &why)); // another executable
    CHECK_FALSE(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, 0x3FFF0000, &why));

    // Session 2: placed at its slot, checked before linking, its module_start and its SPU program run translated.
    {
        hle::test::Fixture f;
        REQUIRE(f.kernel->vfs_mount("/app_home", usrdir.string(), true));
        loader::RuntimeModules modules(*f.hle, nullptr, nullptr);
        modules.set_capture_dir(saved.string());
        auto library = loader::GameModules::load(dir.string(), exe_sha, modules, nullptr, f.mem, &why);
        INFO(why);
        REQUIRE(library);
        CHECK(loader::GameModules::load(dir.string(), std::string(64, 'f'), modules, nullptr, f.mem, &why) == nullptr);
        modules.set_game_translations(library.get());
        const loader::PrxModule* m = load_game_module(modules, path);
        REQUIRE(m);
        CHECK(ppu::in_prx_region(m->segments[0].first));
        CHECK(m->segments[0].first >= 0x3FF00000);
        CHECK(library->modules_translated() == 1);
        CHECK(start_runs_translated(f, *m));
        const u32 elf = m->segments[1].first + test::kPrxExtraData;
        const SpuRun main = run_spu(f.base(), elf + kSpuMain, 0x3100);
        REQUIRE(main.program);
        CHECK(main.no_miss);
        CHECK(main.r3 == 6);

        // Loaded again while it is still loaded: not at its slot (taken), interpreted.
        const loader::PrxModule* again = load_game_module(modules, path);
        REQUIRE(again);
        CHECK_FALSE(ppu::in_prx_region(again->segments[0].first));
        CHECK(library->modules_translated() == 1);
    }

    // A module with one changed byte is another module: main memory, interpreted, saved for the next preparation.
    {
        std::vector<u8> changed = self;
        changed[changed.size() - 1] ^= 1; // its data (the SPU program's last word)
        std::ofstream(usrdir / "libgame2.sprx", std::ios::binary)
            .write(reinterpret_cast<const char*>(changed.data()), static_cast<std::streamsize>(changed.size()));
        hle::test::Fixture f;
        REQUIRE(f.kernel->vfs_mount("/app_home", usrdir.string(), true));
        loader::RuntimeModules modules(*f.hle, nullptr, nullptr);
        modules.set_capture_dir(saved.string());
        auto library = loader::GameModules::load(dir.string(), exe_sha, modules, nullptr, f.mem, &why);
        REQUIRE(library);
        const loader::PrxModule* m = load_game_module(modules, "/app_home/libgame2.sprx");
        REQUIRE(m);
        CHECK_FALSE(ppu::in_prx_region(m->segments[0].first));
        CHECK(library->modules_translated() == 0);
        CHECK(fs::exists(saved / (m->identity + ".elf")));
        CHECK_FALSE(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, PS_MEM_PRX_BASE, &why));
        CHECK(why.find("1 of its modules to translate") != std::string::npos);
    }
    // Prepared again: the first module keeps its slot, the second gets one below it.
    const std::string before = [&] {
        std::ifstream in(dir / "identity.txt");
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    const auto first = recomp::parse_game_module_layout(before);
    REQUIRE(first);
    REQUIRE(first->modules.size() == 1);
    const prepare::ModulesResult again = prepare::prepare_game_modules(o);
    REQUIRE(again.library.ok);
    CHECK(again.modules == 2);
    std::ifstream in(dir / "identity.txt");
    const auto second = recomp::parse_game_module_layout(
        std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    REQUIRE(second);
    REQUIRE(second->modules.size() == 2);
    REQUIRE(second->find(first->modules[0].identity));
    CHECK(second->find(first->modules[0].identity)->addrs == first->modules[0].addrs);
    CHECK(second->lowest() < first->lowest());

    ps_ppu_set_miss_policy(policy);
    ps_ppu_dispatch_clear();
    fs::remove_all(root);
}

namespace {

// A modules library as RuntimeModules sees it: one slot for every module, and the answers the test sets.
struct StandIn final : loader::GameModuleTranslations {
    std::vector<u32> addrs;
    bool code_matches = true;
    std::set<std::string, std::less<>> on;
    std::vector<u32> slot(std::string_view) const override { return addrs; }
    bool matches(std::string_view, const loader::PrxModule&) const override { return code_matches; }
    void enable(std::string_view identity, const loader::PrxModule&) override { on.emplace(identity); }
    bool enabled(std::string_view identity) const override { return on.contains(identity); }
};

} // namespace

TEST_CASE("game modules: a copy that is not what runs translated at its slot goes to main memory (D-238)")
{
    // Review finding: an enabled range is never turned off, so a later copy at the slot that linking patched (or that
    // differs) would have run the earlier copy's translation.
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-game-modules-reload-{}", getpid());
    fs::remove_all(root);
    fs::create_directories(root / "usrdir");
    const std::vector<u8> self = module_bytes("synthLib", 0xDEADBEEF, kMarker);
    std::ofstream(root / "usrdir" / "libgame.sprx", std::ios::binary)
        .write(reinterpret_cast<const char*>(self.data()), static_cast<std::streamsize>(self.size()));
    hle::test::Fixture f;
    REQUIRE(f.kernel->vfs_mount("/app_home", (root / "usrdir").string(), true));
    loader::RuntimeModules modules(*f.hle, nullptr, nullptr);
    StandIn translations;
    translations.addrs = {0x3FFE0000, 0x3FFF0000};
    modules.set_game_translations(&translations);
    u32 id = 0;
    REQUIRE(modules.load("/app_home/libgame.sprx", &id) == 0);
    const loader::PrxModule* first = modules.module(id);
    REQUIRE(first);
    CHECK(first->segments[0].first == 0x3FFE0000);
    CHECK(translations.on.size() == 1);
    REQUIRE(modules.unload(id) == 0);
    // Loaded again, and this time its code is not the translation's: not at the slot.
    translations.code_matches = false;
    REQUIRE(modules.load("/app_home/libgame.sprx", &id) == 0);
    const loader::PrxModule* second = modules.module(id);
    REQUIRE(second);
    CHECK_FALSE(ppu::in_prx_region(second->segments[0].first));
    // A module whose translation never ran stays at its slot, interpreted (nothing of it is enabled).
    translations.on.clear();
    REQUIRE(modules.unload(id) == 0);
    REQUIRE(modules.load("/app_home/libgame.sprx", &id) == 0);
    CHECK(modules.module(id)->segments[0].first == 0x3FFE0000);
    modules.set_game_translations(nullptr);
    fs::remove_all(root);
}

TEST_CASE("game modules: a failed preparation leaves them interpreted without preparing the game on every start "
          "(D-238)")
{
    // Review finding: the app's check asked for the modules library every time, so a preparation that kept failing
    // made every start prepare the whole game again.
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-game-modules-failed-{}", getpid());
    fs::remove_all(root);
    const fs::path saved = root / "prx", dir = root / "modules";
    fs::create_directories(saved);
    // A saved module whose code segment claims more bytes in the file than in memory: it cannot be placed.
    const auto prx = test::build_ppu_prx();
    std::vector<u8> bad = prx.bytes;
    auto elf = recomp::ElfFile::parse(bad);
    REQUIRE(elf);
    const u64 phoff = elf->header().phoff;
    ps_st64(bad.data() + phoff + 0x20, ps_ld64(bad.data() + phoff + 0x28) + 0x1000); // p_filesz > p_memsz
    auto parsed = recomp::ElfFile::parse(bad);
    REQUIRE(parsed);
    const std::string identity = recomp::prx_identity(*parsed);
    std::ofstream(saved / (identity + ".elf"), std::ios::binary)
        .write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size()));
    const std::string exe_sha(64, 'e');
    std::string why;
    CHECK_FALSE(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, prepare::kGameModuleFloor,
        &why));
    prepare::ModulesOptions o;
    o.inputs = {saved.string()};
    o.title_id = "TEST00000";
    o.executable_sha256 = exe_sha;
    o.dir = dir.string();
    o.toolchain = mac_toolchain();
    CHECK_FALSE(prepare::prepare_game_modules(o).library.ok);
    CHECK(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, prepare::kGameModuleFloor, &why));
    CHECK(why.find("stay interpreted") != std::string::npos);
    // Another executable, or another module saved since: tried again.
    CHECK_FALSE(prepare::game_modules_are_current(dir.string(), std::string(64, 'f'), {saved.string()},
        prepare::kGameModuleFloor, &why));
    std::ofstream(saved / (std::string(64, 'a') + ".elf")) << "x";
    CHECK_FALSE(prepare::game_modules_are_current(dir.string(), exe_sha, {saved.string()}, prepare::kGameModuleFloor,
        &why));
    fs::remove_all(root);
}
