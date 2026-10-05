// Firmware modules (LLE, D-157): a synthetic PRX (tests/common/synthetic.cpp) placed, relocated and linked; the
// module index of a /dev_flash holding it (a debug SELF, so no keys); and a synthetic executable whose import library
// only the module has, booted end to end with the module's module_start run before the entry. No game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

#include "pspocket/core/be.h"
#include "pspocket/core/memory_map.h"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "hle_test_util.hpp"
#include "synthetic.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::ppu::as;

namespace {

namespace fs = std::filesystem;

// The synthetic PRX with its one library renamed and its function's NID replaced (file offsets: segment 0 starts at
// 0x100; the library name at +0x200, the function NID at +0x308).
std::vector<u8> prx_exporting(const test::SyntheticPrx& x, std::string_view library, u32 nid)
{
    REQUIRE(library.size() < 16);
    std::vector<u8> bytes = x.bytes;
    std::fill(bytes.begin() + 0x300, bytes.begin() + 0x310, u8{0});
    std::copy(library.begin(), library.end(), bytes.begin() + 0x300);
    ps_st32(bytes.data() + 0x408, nid);
    return bytes;
}

// module_start (segment 0 + 0, 16 words: prologue, nops, epilogue) stores 0x77 at `marker` in words 3..6.
void mark_module_start(std::vector<u8>& bytes, u32 marker)
{
    const u32 code[] = {as::lis(5, static_cast<s32>(marker >> 16)), as::ori(5, 5, marker & 0xFFFF), as::li(6, 0x77),
        as::stw(6, 0, 5)};
    for (u32 i = 0; i < 4; i++) ps_st32(bytes.data() + 0x100 + 4 * (3 + i), code[i]);
}

fs::path fresh_flash(const char* name)
{
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root / "dev_flash" / "sys" / "external");
    return root;
}

void write(const fs::path& p, const std::vector<u8>& bytes)
{
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> debug_self(const std::vector<u8>& elf)
{
    test::SelfOptions o;
    o.debug = true;
    o.npdrm = false;
    return test::wrap_in_self(elf, o);
}

} // namespace

TEST_CASE("LLE: a PRX placed in main memory, relocated, its exports read and its imports linked", "[hle][lle]")
{
    hle::test::Fixture f;
    const auto x = test::build_ppu_prx();
    std::map<std::pair<std::string, u32>, u32> asked;
    u32 next = 0x00ABC000;
    loader::PrxModule m;
    std::string error;
    REQUIRE(loader::load_prx(x.bytes, *f.hle,
        [&](std::string_view library, u32 nid) { return asked[{std::string(library), nid}] = next += 8; }, &m, &error));
    INFO(error);
    REQUIRE(m.segments.size() == 2);
    const u32 seg0 = m.segments[0].first, seg1 = m.segments[1].first;
    CHECK(m.segments[0].second == 0x700);
    CHECK(m.segments[1].second == 0x300);
    CHECK(m.name == "synthetic_prx");
    CHECK(m.toc == seg1 + 0x8100);
    CHECK(m.relocations == x.relocations - 2);
    CHECK(m.relocation_errors == 2); // the unknown type and the segment that does not exist

    u8* base = f.base();
    CHECK(ps_read32(base, seg0 + 0x6F0) == 0x12345678u);              // ADDR32, absolute
    CHECK(ps_read16(base, seg0 + 0x680) == 0x8000);                   // ADDR16_LO
    CHECK(ps_read16(base, seg0 + 0x684) == 0x1234);                   // ADDR16_HI
    CHECK(ps_read16(base, seg0 + 0x688) == 0x1235);                   // ADDR16_HA
    CHECK(ps_read64(base, seg0 + 0x690) == seg1 + 0x10);              // ADDR64 into segment 1
    CHECK(ps_read16(base, seg0 + 0x698) == ((0x567C & 0xFFFC) | 3)); // ADDR16_LO_DS keeps the low two bits
    CHECK(ps_read32(base, seg0 + 0x6A0) == (0x48000000u | ((0x80u - 0x6A0u) & 0x03FFFFFCu))); // REL24: b to +0x80
    // lis/lwz of the first import stub reach its slot.
    const u32 slot0 = seg1 + 0x80;
    CHECK(ps_read16(base, seg0 + 0x102) == static_cast<u16>((slot0 >> 16) + ((slot0 & 0x8000) ? 1 : 0)));
    CHECK(ps_read16(base, seg0 + 0x106) == static_cast<u16>(slot0));

    // Exports: module_start and module_stop from the management library; synthPrxLib's function.
    CHECK(m.start == seg1 + 0x00);
    CHECK(m.stop == seg1 + 0x08);
    CHECK(ps_read32(base, m.start) == seg0 + 0x00);
    CHECK(ps_read32(base, m.start + 4) == m.toc);
    const auto fn = m.functions.find({"synthPrxLib", recomp::nid_of("synthPrxFunc")});
    REQUIRE(fn != m.functions.end());
    CHECK(fn->second == seg1 + 0x10);
    CHECK(ps_read32(base, fn->second) == seg0 + x.prx_func_code);
    CHECK(m.functions.size() == 1);

    // Imports: both of sysPrxForUser's, each slot holding what the linker gave.
    REQUIRE(asked.size() == 2);
    CHECK(ps_read32(base, slot0) == asked.at({"sysPrxForUser", recomp::nid_of("sys_lwmutex_lock")}));
    CHECK(ps_read32(base, slot0 + 4) == asked.at({"sysPrxForUser", recomp::nid_of("_sys_printf")}));
}

TEST_CASE("LLE: a module's variables: exported, and imported through their reference tables", "[hle][lle]")
{
    hle::test::Fixture f;
    test::PrxOptions o;
    o.variables = true;
    const auto x = test::build_ppu_prx(o);
    loader::PrxModule m;
    std::string error;
    std::map<std::pair<std::string, u32>, int> asked;
    REQUIRE(loader::load_prx(
        x.bytes, *f.hle, [](std::string_view, u32) { return 0x00ABC000u; }, &m, &error,
        [&](std::string_view library, u32 nid) {
            asked[{std::string(library), nid}]++;
            return 0x00DEF000u;
        }));
    INFO(error);
    REQUIRE(m.segments.size() == 2);
    const u32 seg1 = m.segments[1].first;
    u8* base = f.base();
    const auto var = m.variables.find({"synthPrxLib", test::kPrxVariableNid});
    REQUIRE(var != m.variables.end());
    CHECK(var->second == seg1 + 0x60);
    CHECK(ps_read32(base, var->second) == test::kPrxVariableValue);
    CHECK(asked.size() == 1);
    CHECK(asked.count({"synthVarLib", test::kPrxVariableNid}) == 1);
    CHECK(m.variables_linked == 1);
    CHECK(ps_read32(base, seg1 + 0xC0) == 0x00DEF000u); // the reference got the variable's address
}

TEST_CASE("LLE: the firmware's modules indexed; a library goes LLE only when the host has nothing of its module",
    "[hle][lle]")
{
    const fs::path root = fresh_flash("pspocket-test-lle-index");
    const fs::path external = root / "dev_flash" / "sys" / "external";
    const auto x = test::build_ppu_prx();
    write(external / "libsynth.sprx", debug_self(prx_exporting(x, "synthLib", 0xDEADBEEF)));
    write(external / "libgcm_synth.sprx", debug_self(prx_exporting(x, "cellGcmSys", 0x12345678)));
    write(external / "notes.txt", {'h', 'i'});

    for (int pass = 0; pass < 2; pass++) { // the second from lle-index.txt
        loader::LleModules lle((root / "dev_flash").string());
        std::string error;
        REQUIRE(lle.index(&error));
        CHECK(fs::exists(root / "lle-index.txt"));
        CHECK(lle.libraries() == 2);
        CHECK(lle.file_for("synthLib") == "libsynth.sprx");
        CHECK(lle.file_for("cellGcmSys") == "libgcm_synth.sprx");
        CHECK_FALSE(lle.file_for("sysPrxForUser"));
        CHECK(lle.chooses("synthLib"));          // the host has nothing of it
        CHECK_FALSE(lle.chooses("cellGcmSys"));  // the host implements it
        CHECK_FALSE(lle.chooses("sysPrxForUser")); // no module has it
        lle.force("cellGcmSys");
        CHECK(lle.chooses("cellGcmSys"));
    }
    loader::LleModules none((root / "nowhere").string());
    std::string error;
    CHECK_FALSE(none.index(&error));
    CHECK_FALSE(error.empty());
    fs::remove_all(root);
}

TEST_CASE("LLE: a module's imported variable comes from the module that exports it, loaded for it", "[hle][lle]")
{
    const fs::path root = fresh_flash("pspocket-test-lle-variables");
    const fs::path external = root / "dev_flash" / "sys" / "external";
    test::PrxOptions o;
    o.variables = true;
    const auto x = test::build_ppu_prx(o);
    // libvar exports synthVarLib (its variable too); libsynth exports synthLib and imports synthVarLib's variable.
    write(external / "libvar.sprx", debug_self(prx_exporting(x, "synthVarLib", 0x11111111)));
    write(external / "libsynth.sprx", debug_self(prx_exporting(x, "synthLib", 0xDEADBEEF)));
    hle::test::Fixture f;
    loader::LleModules lle((root / "dev_flash").string());
    REQUIRE(lle.index());
    std::string error;
    const loader::PrxModule* synth = lle.load("synthLib", *f.hle, &error);
    INFO(error);
    REQUIRE(synth != nullptr);
    REQUIRE(lle.loaded().size() == 2);
    const loader::PrxModule* var = lle.loaded()[1].get();
    CHECK(var->file == "libvar.sprx");
    const auto exported = var->variables.find({"synthVarLib", test::kPrxVariableNid});
    REQUIRE(exported != var->variables.end());
    CHECK(synth->variables_linked == 1);
    CHECK(ps_read32(f.base(), synth->segments[1].first + 0xC0) == exported->second);
    CHECK(ps_read32(f.base(), exported->second) == test::kPrxVariableValue);
    fs::remove_all(root);
}

TEST_CASE("LLE: a game's import served by a firmware module, its module_start run before the entry", "[hle][lle][loader]")
{
    const fs::path root = fresh_flash("pspocket-test-lle-boot");
    constexpr u32 kOut = 0x21000, kMarker = 0x21100; // the executable's bss
    const auto prx = test::build_ppu_prx();
    std::vector<u8> module = prx_exporting(prx, "synthLib", 0xDEADBEEF);
    mark_module_start(module, kMarker);
    write(root / "dev_flash" / "sys" / "external" / "libsynth.sprx", debug_self(module));

    // _start: two calls of synthLib 0xDEADBEEF (now the module's function, which returns r3 as it came), then the
    // marker module_start left, then exit.
    const auto x = test::build_ppu_exec();
    const u32 stub12 = x.first_stub_code + 12 * 0x20;
    std::vector<u32> code;
    auto at = [&] { return x.entry_func + static_cast<u32>(code.size() * 4); };
    code.push_back(as::mflr(0));
    code.push_back(as::std_(0, 16, 1));
    code.push_back(as::stdu(1, -128, 1));
    code.push_back(as::lis(31, kOut >> 16));
    code.push_back(as::ori(31, 31, kOut & 0xFFFF));
    code.push_back(as::li(3, 5));
    code.push_back(as::b_rel(static_cast<s32>(stub12 - at()), true));
    code.push_back(as::ld(2, 40, 1));
    code.push_back(as::std_(3, 0, 31));
    code.push_back(as::lwz(5, 0x100, 31)); // the marker
    code.push_back(as::std_(5, 8, 31));
    code.push_back(as::li(3, 42));
    code.push_back(as::li(11, 22)); // _sys_process_exit(42)
    code.push_back(as::sc());
    REQUIRE(x.entry_func == 0x10200);
    std::vector<u8> bytes = x.bytes;
    for (usize i = 0; i < code.size(); i++) ps_st32(bytes.data() + 0x200 + 4 * i, code[i]);
    auto exe = loader::Executable::parse(std::move(bytes));
    REQUIRE(exe);

    GuestMemory memory;
    REQUIRE(memory.reserve());
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    lv2::Config cfg = (*exe)->kernel_config();
    loader::BootState state;
    loader::prepare_kernel_config(cfg, state);
    auto kernel = std::make_unique<lv2::Kernel>(memory, cfg);
    kernel->install();
    auto hle = std::make_unique<hle::HleProcess>(*kernel, PS_MEM_MAIN_BASE, cfg.main_area_end);
    hle->install();

    loader::LleModules lle((root / "dev_flash").string());
    REQUIRE(lle.index());
    loader::LoadReport report;
    REQUIRE(loader::load(**exe, *kernel, *hle, &report, &lle) == lv2::CELL_OK);
    CHECK(report.lle_modules == 1);
    CHECK(report.functions_lle == 1); // 0xDEADBEEF; synthLib's other import is not in the module: the host stub
    CHECK(report.functions_linked == 14);
    REQUIRE(lle.starts().size() == 1);
    state.module_starts = lle.starts();
    u32 main_id = 0;
    REQUIRE(loader::start_main_thread(*exe, *kernel, state, {}, main_id) == lv2::CELL_OK);
    REQUIRE(kernel->wait_for_exit(std::chrono::seconds(10)));
    CHECK(kernel->exit_status() == 42);
    u8* base = memory.base();
    CHECK(ps_read64(base, kOut) == 5);       // the module's function ran, not the ENOSYS stub
    CHECK(ps_read64(base, kOut + 8) == 0x77); // module_start ran first
    CHECK(hle->unimplemented_calls() == 0);
    hle.reset();
    kernel.reset();
    fs::remove_all(root);
}

TEST_CASE("Run-time modules: a game's own PRX loaded by path, started (its exports reach the game's imports) and "
          "found by name (D-159)", "[hle][lle][prx]")
{
    // The game ships libsynth.sprx in its directory and loads it with sys_prx_load_module while it runs.
    const fs::path root = fresh_flash("pspocket-test-prx-runtime");
    constexpr u32 kMarker = 0x21100; // the executable's bss: module_start writes it
    const auto prx = test::build_ppu_prx();
    std::vector<u8> module = prx_exporting(prx, "synthLib", 0xDEADBEEF);
    mark_module_start(module, kMarker);
    fs::create_directories(root / "usrdir");
    write(root / "usrdir" / "libsynth.sprx", debug_self(module));

    const auto x = test::build_ppu_exec();
    auto exe = loader::Executable::parse(x.bytes);
    REQUIRE(exe);
    hle::test::Fixture f;
    REQUIRE(f.kernel->vfs_mount("/app_home", (root / "usrdir").string(), true));
    REQUIRE(loader::load(**exe, *f.kernel, *f.hle) == lv2::CELL_OK);
    loader::RuntimeModules modules(*f.hle, *exe, nullptr);
    f.hle->set_module_loader(&modules);

    u8* base = f.base();
    const u32 slot = x.first_stub_slot + 12 * 4; // synthLib 0xDEADBEEF
    const u32 host_stub = ps_read32(base, slot);
    hle::test::GuestThread t(f, 0, "main");
    const u32 path = hle::test::kData, missing = hle::test::kData + 0x100, name = hle::test::kData + 0x200,
              result = hle::test::kData + 0x300;
    const std::string p = "/app_home/libsynth.sprx", m = "/app_home/none.sprx", n = "synthetic_prx";
    std::memcpy(base + path, p.c_str(), p.size() + 1);
    std::memcpy(base + missing, m.c_str(), m.size() + 1);
    std::memcpy(base + name, n.c_str(), n.size() + 1);

    CHECK(t.call("sys_prx_load_module", missing, 0, 0) == hle::test::r3_of(lv2::CELL_ENOENT));
    CHECK(t.call("sys_prx_load_module", path, 1, 0) == hle::test::r3_of(lv2::CELL_ENOSYS)); // a fixed address
    const u64 id = t.call("sys_prx_load_module", path, 0, 0);
    REQUIRE(static_cast<s32>(id) > 0);
    CHECK(t.call("sys_prx_get_module_id_by_name", name, 0, 0) == id);
    CHECK(ps_read32(base, slot) == host_stub); // not linked before it starts
    CHECK(t.call("sys_prx_start_module", id, 0, 0, result, 0, 0) == lv2::CELL_OK);
    CHECK(ps_read32(base, result) == 0);           // module_start's result: resident
    CHECK(ps_read32(base, kMarker) == 0x77);       // module_start ran on the calling thread
    const loader::PrxModule* loaded = modules.module(static_cast<u32>(id));
    REQUIRE(loaded != nullptr);
    CHECK(ps_read32(base, slot) == loaded->functions.at({"synthLib", 0xDEADBEEF})); // the game's import now goes to it
    CHECK(t.call("sys_prx_start_module", id, 0, 0, result, 0, 0) == hle::test::r3_of_code(0x80011133)); // started
    CHECK(t.call("sys_prx_unload_module", id, 0, 0) == hle::test::r3_of_code(0x80011138)); // stop it first
    CHECK(t.call("sys_prx_stop_module", id, 0, 0, result, 0, 0) == lv2::CELL_OK);
    CHECK(t.call("sys_prx_unload_module", id, 0, 0) == lv2::CELL_OK);
    CHECK(t.call("sys_prx_get_module_id_by_name", name, 0, 0) == hle::test::r3_of_code(0x8001112E));
    f.hle->set_module_loader(nullptr);
    fs::remove_all(root);
}

