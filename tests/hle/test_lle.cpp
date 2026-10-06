// Firmware modules (LLE, D-157): a synthetic PRX (tests/common/synthetic.cpp) placed, relocated and linked; the
// module index of a /dev_flash holding it (a debug SELF, so no keys); and a synthetic executable whose import library
// only the module has, booted end to end with the module's module_start run before the entry. No game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
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


TEST_CASE("Run-time modules: by descriptor at an offset, on a memory container; the list, info and lookup by address "
          "(D-184)", "[hle][lle][prx]")
{
    const fs::path root = fresh_flash("pspocket-test-prx-info");
    const auto prx = test::build_ppu_prx();
    std::vector<u8> module = prx_exporting(prx, "synthLib", 0xDEADBEEF);
    const std::vector<u8> self = debug_self(module);
    std::vector<u8> archive(0x40, 0xA5); // a header of the game's own before the module
    archive.insert(archive.end(), self.begin(), self.end());
    fs::create_directories(root / "usrdir");
    write(root / "usrdir" / "pack.bin", archive);
    write(root / "usrdir" / "libsynth.sprx", self);

    const auto x = test::build_ppu_exec();
    auto exe = loader::Executable::parse(x.bytes);
    REQUIRE(exe);
    hle::test::Fixture f;
    REQUIRE(f.kernel->vfs_mount("/app_home", (root / "usrdir").string(), true));
    REQUIRE(loader::load(**exe, *f.kernel, *f.hle) == lv2::CELL_OK);
    loader::RuntimeModules modules(*f.hle, *exe, nullptr);
    f.hle->set_module_loader(&modules);
    u8* base = f.base();
    hle::test::GuestThread t(f, 0, "main");
    const u32 path = hle::test::kData, fd = hle::test::kData + 0x100, list = hle::test::kData + 0x200,
              ids = hle::test::kData + 0x240, info = hle::test::kData + 0x300, name = hle::test::kData + 0x400,
              segs = hle::test::kData + 0x500;
    const auto put = [&](u32 at, std::string_view text) {
        std::memcpy(base + at, text.data(), text.size());
        base[at + text.size()] = 0;
    };

    // By descriptor, 0x40 bytes in; and by path on a memory container (no containers for modules: as usual).
    put(path, "/app_home/pack.bin");
    REQUIRE(t.call("cellFsOpen", path, 0, fd, 0, 0) == 0);
    const u64 id1 = t.call("sys_prx_load_module_by_fd", ps_read32(base, fd), 0x40, 0, 0);
    REQUIRE(static_cast<s32>(id1) > 0);
    CHECK(t.call("sys_prx_load_module_by_fd", 99, 0, 0, 0) == hle::test::r3_of(lv2::CELL_EBADF));
    put(path, "/app_home/libsynth.sprx");
    const u64 id2 = t.call("sys_prx_load_module_on_memcontainer", path, 0x12345, 0, 0);
    REQUIRE(static_cast<s32>(id2) > 0);
    CHECK(id2 != id1);

    // The list: both, oldest first.
    std::memset(base + list, 0, 0x20);
    ps_write64(base, list, 0x20);
    ps_write32(base, list + 8, 8);
    ps_write32(base, list + 16, ids);
    REQUIRE(t.call("sys_prx_get_module_list", 2, list) == 0);
    CHECK(ps_read32(base, list + 12) == 2);
    CHECK(ps_read32(base, ids) == id1);
    CHECK(ps_read32(base, ids + 4) == id2);

    // The info: name, the path, segments where they went; lookup by an address inside one.
    std::memset(base + info, 0, 0x58);
    ps_write64(base, info, 0x48);
    ps_write32(base, info + 0x38, name);
    ps_write32(base, info + 0x3C, 0x100);
    ps_write32(base, info + 0x40, segs);
    ps_write32(base, info + 0x44, 4);
    REQUIRE(t.call("sys_prx_get_module_info", id1, 0, info) == 0);
    const loader::PrxModule* loaded = modules.module(static_cast<u32>(id1));
    REQUIRE(loaded != nullptr);
    CHECK(std::string(reinterpret_cast<const char*>(base + info + 8)) == loaded->name);
    CHECK(std::string(reinterpret_cast<const char*>(base + name)) == "/app_home/pack.bin");
    CHECK(ps_read32(base, info + 0x34) == loaded->segments.size());
    CHECK(ps_read32(base, info + 0x44) == std::min<usize>(4, loaded->segments.size()));
    CHECK(ps_read64(base, segs) == loaded->segments[0].first);
    CHECK(ps_read64(base, segs + 0x10) == loaded->segments[0].second);
    CHECK(t.call("sys_prx_get_module_id_by_address", loaded->segments[0].first + 4) == id1);
    CHECK(t.call("sys_prx_get_module_id_by_address", 0x00010000) == hle::test::r3_of_code(0x8001112E)); // the game's
    ps_write64(base, info, 0x40);
    CHECK(t.call("sys_prx_get_module_info", id1, 0, info) == hle::test::r3_of(lv2::CELL_EINVAL));
    ps_write64(base, info, 0x48);
    CHECK(t.call("sys_prx_get_module_info", 0x23FFFFFF, 0, info) == hle::test::r3_of_code(0x8001112E));
    CHECK(t.call("sys_prx_unregister_library", 0x1000) == 0);
    f.hle->set_module_loader(nullptr);
    fs::remove_all(root);
}

// D-175: the per-game choice. Synthetic firmware modules (debug SELFs) in a /dev_flash; the "game" is the list of what
// it imports. A module runs as it is when the host lacks a function the game imports from it (or the game imports a
// variable of it) and everything the module needs resolves; else its libraries stay on the host.
TEST_CASE("LLE: per game, a module runs as it is only when the host lacks what the game imports and the module's own "
          "needs resolve", "[hle][lle]")
{
    using Needs = loader::LleModules::LibraryNeeds;
    const fs::path root = fresh_flash("pspocket-test-lle-decide");
    const fs::path external = root / "dev_flash" / "sys" / "external";
    test::PrxOptions o;
    o.variables = true; // synthLib's module imports synthVarLib's variable
    const auto x = test::build_ppu_prx(o);
    const auto plain = test::build_ppu_prx();
    write(external / "libvar.sprx", debug_self(prx_exporting(x, "synthVarLib", 0x11111111))); // exports the variable
    write(external / "libsynth.sprx", debug_self(prx_exporting(x, "synthLib", 0xDEADBEEF)));
    write(external / "libgcm_synth.sprx", debug_self(prx_exporting(plain, "cellGcmSys", 0x12345678)));
    write(external / "libgcm_sys.sprx", debug_self(prx_exporting(plain, "cellResc", 0x22222222)));
    // A module whose code makes syscall 700 (li r11, 700; sc in module_start's padding) and one whose import has no
    // implementation anywhere (sys_lwmutex_lock's NID replaced).
    std::vector<u8> sc = prx_exporting(plain, "synthScLib", 0x33333333);
    ps_st32(sc.data() + 0x100 + 4 * 7, 0x39600000u | 700);
    ps_st32(sc.data() + 0x100 + 4 * 8, 0x44000002u);
    write(external / "libsc.sprx", debug_self(sc));
    std::vector<u8> novar = prx_exporting(x, "synthNoVarLib", 0x55555555); // its variable's library: none has it
    std::fill(novar.begin() + 0x320, novar.begin() + 0x330, u8{0});
    std::memcpy(novar.data() + 0x320, "synthNoVar", 10);
    write(external / "libnovar.sprx", debug_self(novar));
    // A core in sys/internal exporting a variable only it has, and a module in sys/external that needs it.
    const auto renamed_import = [&](std::string_view library, u32 nid, const char* imported) {
        std::vector<u8> m = prx_exporting(x, library, nid);
        std::fill(m.begin() + 0x320, m.begin() + 0x330, u8{0});
        std::memcpy(m.data() + 0x320, imported, std::strlen(imported));
        return m;
    };
    fs::create_directories(root / "dev_flash" / "sys" / "internal");
    write(root / "dev_flash" / "sys" / "internal" / "libcore.sprx",
        debug_self(renamed_import("synthCore", 0x77777777, "synthCore")));
    write(external / "libfront.sprx", debug_self(renamed_import("synthFront", 0x88888888, "synthCore")));
    std::vector<u8> orphan = prx_exporting(plain, "synthOrphan", 0x44444444);
    ps_st32(orphan.data() + 0x100 + 0x310, 0x0BADF00Du);
    write(external / "liborphan.sprx", debug_self(orphan));
    const auto bound = [](u32 n) { return n != 700; };
    const u32 host_gcm = recomp::nid_of("cellGcmGetConfiguration");

    for (int pass = 0; pass < 2; pass++) { // the second from lle-index.txt (format 2)
        INFO("pass " << pass);
        loader::LleModules lle((root / "dev_flash").string());
        REQUIRE(lle.index());
        // The host implements what the game imports: nothing runs as it is.
        lle.decide({Needs{"cellGcmSys", {host_gcm}, {}}}, bound);
        CHECK(lle.chosen_files().empty());
        CHECK_FALSE(lle.chooses("cellGcmSys"));
        // A function the host lacks in a library it partly implements: the module runs (it is not on the native
        // list), and so do the modules it needs (libvar for synthLib's module, by its variable).
        lle.decide({Needs{"cellGcmSys", {host_gcm, 0x12345678}, {}}, Needs{"synthLib", {0xDEADBEEF}, {}}}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"libgcm_synth.sprx", "libsynth.sprx", "libvar.sprx"});
        CHECK(lle.chooses("cellGcmSys"));
        CHECK(lle.chooses("synthVarLib"));
        // A module on the native list never runs as it is, whatever the game imports from it.
        lle.decide({Needs{"cellResc", {0x22222222}, {}}}, bound);
        CHECK(lle.chosen_files().empty());
        CHECK_FALSE(lle.chooses("cellResc"));
        CHECK(loader::LleModules::always_native("libgcm_sys.sprx"));
        CHECK(loader::LleModules::always_native("libsysutil_np2.sprx"));
        CHECK_FALSE(loader::LleModules::always_native("libl10n.sprx"));
        // A module that makes a syscall lv2 lacks, or imports a function nothing implements, still runs (as RPCS3
        // links it): those fail when called, and are listed.
        lle.decide({Needs{"synthScLib", {0x33333333}, {}}, Needs{"synthOrphan", {0x44444444}, {}}}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"liborphan.sprx", "libsc.sprx"});
        REQUIRE(lle.gaps().contains("libsc.sprx"));
        CHECK(lle.gaps().at("libsc.sprx").front().find("syscall 700") != std::string::npos);
        REQUIRE(lle.gaps().contains("liborphan.sprx"));
        CHECK(lle.gaps().at("liborphan.sprx").front().find("0x0badf00d") != std::string::npos);
        // A module whose imported variable no module exports still runs, the variable left unlinked as lv2 leaves it,
        // and the variable is listed (D-177).
        lle.decide({Needs{"synthNoVarLib", {0x55555555}, {}}}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"libnovar.sprx"});
        CHECK(lle.refused_files().empty());
        REQUIRE(lle.gaps().contains("libnovar.sprx"));
        CHECK(lle.gaps().at("libnovar.sprx").front().find("variable synthNoVar") != std::string::npos);
        // A codec core in sys/internal serves the module that needs it (D-177).
        lle.decide({Needs{"synthFront", {0x88888888}, {}}}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"internal/libcore.sprx", "libfront.sprx"});
        CHECK(lle.file_for("synthCore") == "internal/libcore.sprx");
        // A game's imported variable needs the module (the host exports no variables).
        lle.decide({Needs{"synthVarLib", {}, {test::kPrxVariableNid}}}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"libvar.sprx"});
        // A module the game loads while it runs: what it needs joins, what was chosen stays.
        lle.decide({Needs{"cellGcmSys", {host_gcm}, {}}, Needs{"synthVarLib", {}, {test::kPrxVariableNid}}}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"libvar.sprx"});
        lle.extend("/app_home/a.sprx", {Needs{"synthOrphan", {0x44444444}, {}}}, {}, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"liborphan.sprx", "libvar.sprx"});
        // A library something linked to the host (the game's cellGcmSys) stays there: one implementation per library.
        lle.extend("/app_home/b.sprx", {Needs{"cellGcmSys", {0x12345678}, {}}}, {}, bound);
        CHECK_FALSE(lle.chooses("cellGcmSys"));
        // A firmware module loaded by its path joins when nothing linked its libraries to the host.
        lle.extend("/dev_flash/sys/external/libsc.sprx", {}, "libsc.sprx", bound);
        CHECK(lle.chosen("libsc.sprx"));
        // A forced library runs as it is even from a module on the native list.
        lle.force("cellResc");
        lle.decide({Needs{"cellResc", {0x22222222}, {}}}, bound);
        CHECK(lle.chooses("cellResc"));
    }
    fs::remove_all(root);
}

// D-197: what the host's cellAdec imports from the firmware's codec cores is chosen as the game's imports are, and
// linked for the host's code: a core module exporting cellMP3dec's operation table (a synthetic PRX whose library,
// variable and imported variable are renamed).
TEST_CASE("LLE: the host's own imports (cellAdec's codec cores) chosen with the game's and linked for the host",
    "[hle][lle]")
{
    using Needs = loader::LleModules::LibraryNeeds;
    const fs::path root = fresh_flash("pspocket-test-lle-host-imports");
    const fs::path external = root / "dev_flash" / "sys" / "external";
    test::PrxOptions o;
    o.variables = true;
    const auto x = test::build_ppu_prx(o);
    constexpr u32 kMp3Ops = 0x84276A23;
    std::vector<u8> core = prx_exporting(x, "cellMP3dec", 0x12340001);
    std::fill(core.begin() + 0x320, core.begin() + 0x330, u8{0});
    std::memcpy(core.data() + 0x320, "cellMP3dec", 10); // its imported variable: its own
    ps_st32(core.data() + 0x40C, kMp3Ops);              // the variable it exports
    ps_st32(core.data() + 0x418, kMp3Ops);              // the variable it imports
    write(external / "libmp3dec.sprx", debug_self(core));
    const auto bound = [](u32) { return true; };
    const std::vector<Needs> game = {Needs{"cellAdec", {recomp::nid_of("cellAdecQueryAttr")}, {}}};
    for (int pass = 0; pass < 2; pass++) { // the second from the cached index
        INFO("pass " << pass);
        hle::test::Fixture f;
        loader::LleModules lle((root / "dev_flash").string());
        REQUIRE(lle.index());
        lle.decide({Needs{"cellGcmSys", {recomp::nid_of("cellGcmGetConfiguration")}, {}}}, bound);
        CHECK(lle.chosen_files().empty()); // no cellAdec, no core
        lle.decide(game, bound);
        CHECK(lle.chosen_files() == std::set<std::string, std::less<>>{"libmp3dec.sprx"});
        lle.link_host_imports(game, *f.hle);
        REQUIRE(lle.loaded().size() == 1);
        const auto exported = lle.loaded()[0]->variables.find({"cellMP3dec", kMp3Ops});
        REQUIRE(exported != lle.loaded()[0]->variables.end());
        CHECK(f.hle->imported("cellMP3dec", kMp3Ops) == exported->second);
        CHECK(ps_read32(f.base(), exported->second) == test::kPrxVariableValue);
        CHECK(f.hle->imported("cellM2BCdec", 0x300AFE4D) == 0); // no module exports it
        CHECK(lle.take_starts().size() == 1);                   // the core starts before the game
    }
    fs::remove_all(root);
}

// D-175: a game's own PRX loaded while it runs needs a firmware module the boot did not choose: the module joins, is
// loaded for it, and its module_start runs before the PRX's own (on the thread that starts the PRX).
TEST_CASE("Run-time modules: the firmware modules a game's PRX needs join, and start before it", "[hle][lle][prx]")
{
    const fs::path root = fresh_flash("pspocket-test-prx-runtime-deps");
    constexpr u32 kGameMarker = 0x21100, kFirmwareMarker = 0x21108; // the executable's bss
    test::PrxOptions o;
    o.variables = true; // each imports synthVarLib's variable
    const auto x = test::build_ppu_prx(o);
    std::vector<u8> firmware = prx_exporting(x, "synthVarLib", 0x11111111);
    mark_module_start(firmware, kFirmwareMarker);
    write(root / "dev_flash" / "sys" / "external" / "libvar.sprx", debug_self(firmware));
    std::vector<u8> game = prx_exporting(x, "synthGame", 0xDEADBEEF);
    mark_module_start(game, kGameMarker);
    fs::create_directories(root / "usrdir");
    write(root / "usrdir" / "libgame.sprx", debug_self(game));

    const auto exe_bytes = test::build_ppu_exec();
    auto exe = loader::Executable::parse(exe_bytes.bytes);
    REQUIRE(exe);
    hle::test::Fixture f;
    REQUIRE(f.kernel->vfs_mount("/app_home", (root / "usrdir").string(), true));
    loader::LleModules lle((root / "dev_flash").string());
    REQUIRE(lle.index());
    REQUIRE(loader::load(**exe, *f.kernel, *f.hle, nullptr, &lle) == lv2::CELL_OK);
    CHECK(lle.decided());
    CHECK(lle.chosen_files().empty()); // the executable needs nothing of libvar
    CHECK(lle.take_starts().empty());
    loader::RuntimeModules modules(*f.hle, *exe, &lle);
    f.hle->set_module_loader(&modules);

    u8* base = f.base();
    hle::test::GuestThread t(f, 0, "main");
    const u32 path = hle::test::kData, result = hle::test::kData + 0x300;
    const std::string p = "/app_home/libgame.sprx";
    std::memcpy(base + path, p.c_str(), p.size() + 1);
    const u64 id = t.call("sys_prx_load_module", path, 0, 0);
    REQUIRE(static_cast<s32>(id) > 0);
    CHECK(lle.chosen("libvar.sprx"));
    CHECK(lle.loaded().size() == 1);
    CHECK(ps_read32(base, kFirmwareMarker) == 0); // loaded, not started yet
    CHECK(t.call("sys_prx_start_module", id, 0, 0, result, 0, 0) == lv2::CELL_OK);
    CHECK(ps_read32(base, kFirmwareMarker) == 0x77); // the firmware module's module_start ran
    CHECK(ps_read32(base, kGameMarker) == 0x77);
    CHECK(modules.take_dependency_starts().empty()); // once
    f.hle->set_module_loader(nullptr);
    fs::remove_all(root);
}
