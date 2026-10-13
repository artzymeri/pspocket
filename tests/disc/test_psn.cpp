// PSN games through the app's C API (pspocket/disc/psn.h, D-580 to D-583): packages, licences, executables. Made-up
// licences and synthetic packages only.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/disc/psn.h"
#include "pspocket/loader/hdd_game.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/pkg.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "synthetic.hpp"

using namespace pspocket;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view what)
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-psn-{}-{}", what, static_cast<long>(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, const std::vector<u8>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

constexpr const char* kContent = "UP0001-NPXX00000_00-0000000000000001";

} // namespace

TEST_CASE("ps_psn_rap_path: where pspocket looks for a content ID's licence", "[disc][psn]")
{
    char out[512] = {};
    REQUIRE(ps_psn_rap_path("/x/hdd0", kContent, out, sizeof out) == 0);
    CHECK(std::string(out) == "/x/hdd0/home/00000001/exdata/UP0001-NPXX00000_00-0000000000000001.rap");
    CHECK(ps_psn_rap_path("/x/hdd0", "../evil", out, sizeof out) == 1);
    CHECK(ps_psn_rap_path("/x/hdd0", kContent, out, 20) == 1); // does not fit
    CHECK(ps_psn_rap_path(nullptr, kContent, out, sizeof out) == 1);
}

TEST_CASE("ps_psn_install_rap: a player's .rap goes where pspocket looks for it; other files are refused", "[disc][psn]")
{
    const fs::path root = temp_dir("rap");
    const fs::path hdd0 = root / "hdd0", picked = root / "picked";
    std::vector<u8> rap(16);
    for (usize i = 0; i < rap.size(); i++) rap[i] = static_cast<u8>(0x40 + i);
    write_file(picked / (std::string(kContent) + ".rap"), rap);
    char error[256] = {};
    REQUIRE(ps_psn_install_rap(hdd0.c_str(), (picked / (std::string(kContent) + ".rap")).c_str(), error, sizeof error) ==
        0);
    char where[512] = {};
    REQUIRE(ps_psn_rap_path(hdd0.c_str(), kContent, where, sizeof where) == 0);
    CHECK(read_file(where) == rap);

    // Again, with another licence and the extension in capitals: the new one replaces it.
    std::vector<u8> other(16, 0x99);
    write_file(picked / "UP0001-NPXX00000_00-0000000000000001.RAP", other);
    REQUIRE(ps_psn_install_rap(hdd0.c_str(), (picked / "UP0001-NPXX00000_00-0000000000000001.RAP").c_str(), error,
                sizeof error) == 0);
    CHECK(read_file(where) == other);

    const auto refused = [&](const std::string& name, const std::vector<u8>& bytes) {
        write_file(picked / name, bytes);
        std::memset(error, 0, sizeof error);
        const int r = ps_psn_install_rap(hdd0.c_str(), (picked / name).c_str(), error, sizeof error);
        return r == 1 ? std::string(error) : std::string("installed");
    };
    CHECK(refused("licence.bin", rap).find("named <CONTENT_ID>.rap") != std::string::npos);
    CHECK(refused("bad id!.rap", rap).find("is not a content ID") != std::string::npos);
    CHECK(refused(std::string(kContent).substr(0, 20) + ".rap", std::vector<u8>(15, 1)).find("15 bytes") !=
        std::string::npos);
    CHECK(refused(std::string(kContent).substr(0, 21) + ".rap", std::vector<u8>(17, 1)).find("17 bytes") !=
        std::string::npos);
    CHECK(ps_psn_install_rap(hdd0.c_str(), (picked / "missing.rap").c_str(), error, sizeof error) == 1);
    CHECK(std::string(error).find("cannot read") != std::string::npos);
    fs::remove_all(root);
}

namespace {

// Every byte the original ELF's header and segments put in the file came back.
void check_same_elf(const std::vector<u8>& want, const std::vector<u8>& got)
{
    auto header = recomp::parse_elf_header(want);
    REQUIRE(header);
    REQUIRE(got.size() >= 0x40);
    CHECK(std::equal(want.begin(), want.begin() + 0x40, got.begin()));
    auto phdrs = recomp::parse_program_headers(want, *header, header->phoff, 0);
    REQUIRE(phdrs);
    for (const recomp::ProgramHeader& ph : *phdrs) {
        if (!ph.filesz) continue;
        REQUIRE(got.size() >= ph.offset + ph.filesz);
        CHECK(std::equal(want.begin() + static_cast<std::ptrdiff_t>(ph.offset),
            want.begin() + static_cast<std::ptrdiff_t>(ph.offset + ph.filesz),
            got.begin() + static_cast<std::ptrdiff_t>(ph.offset)));
    }
}

} // namespace

TEST_CASE("ps_psn_decrypt_executable: a PSN game's EBOOT.BIN with the player's licence, or free (D-582)", "[disc][psn]")
{
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    const recomp::SelfKey* npdrm_key = nullptr;
    for (const recomp::SelfKey& k : recomp::builtin_self_keys())
        if (k.type == 8 && !npdrm_key) npdrm_key = &k;
    if (!keys.present || !npdrm_key) SKIP("this build has no NPDRM keys");
    // Synthetic PSN executables encrypted here with the build's keys (in memory and a temporary folder only).
    const std::vector<u8> elf = test::build_ppu_exec().bytes;
    const auto psn_self = [&](s32 license, const recomp::Key128& content_key) {
        test::SelfEncryption e;
        e.key = *npdrm_key;
        e.revision = static_cast<u16>(npdrm_key->revision);
        e.npdrm = true;
        e.npdrm_license = license;
        recomp::crypto::aes_ecb_decrypt(keys.klic_key, content_key.data(), 16, e.npdrm_layer_key.data());
        return test::encrypt_self(elf, e);
    };
    const fs::path root = temp_dir("eboot");
    const fs::path hdd0 = root / "dev_hdd0", game = hdd0 / "game" / "NPXX00000";
    recomp::Key128 rap{};
    rap.fill(0x5C);
    write_file(game / "USRDIR" / "EBOOT.BIN", psn_self(2, recomp::rap_to_rif(rap, keys)));
    char error[512] = {};
    const fs::path out = root / "out" / "EBOOT.elf";

    // Without the player's licence: refused, naming where it was looked for; then with it, from the game's folder.
    REQUIRE(ps_psn_decrypt_executable(game.c_str(), hdd0.c_str(), out.c_str(), error, sizeof error) == 1);
    CHECK(std::string(error).find(std::string(test::kSelfContentId) + ".rap") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
    write_file(root / (std::string(test::kSelfContentId) + ".rap"), std::vector<u8>(rap.begin(), rap.end()));
    REQUIRE(ps_psn_install_rap(hdd0.c_str(), (root / (std::string(test::kSelfContentId) + ".rap")).c_str(), error,
                sizeof error) == 0);
    REQUIRE(ps_psn_decrypt_executable(game.c_str(), hdd0.c_str(), out.c_str(), error, sizeof error) == 0);
    check_same_elf(elf, read_file(out));
    // No hdd0 folder: the licence cannot be looked for.
    CHECK(ps_psn_decrypt_executable(game.c_str(), nullptr, out.c_str(), error, sizeof error) == 1);
    CHECK(std::string(error).find("no /dev_hdd0 folder") != std::string::npos);

    // Free content opens with NP_KLIC_FREE, without licences; a plain ELF is copied; a folder without an EBOOT.BIN
    // is refused.
    write_file(root / "free" / "EBOOT.BIN", psn_self(3, keys.klic_free));
    REQUIRE(ps_psn_decrypt_executable((root / "free" / "EBOOT.BIN").c_str(), nullptr, (root / "free.elf").c_str(), error,
                sizeof error) == 0);
    check_same_elf(elf, read_file(root / "free.elf"));
    write_file(root / "plain" / "EBOOT.BIN", elf);
    REQUIRE(ps_psn_decrypt_executable((root / "plain").c_str(), nullptr, (root / "plain.elf").c_str(), error,
                sizeof error) == 1); // a folder needs USRDIR/EBOOT.BIN or PS3_GAME/USRDIR/EBOOT.BIN
    REQUIRE(ps_psn_decrypt_executable((root / "plain" / "EBOOT.BIN").c_str(), nullptr, (root / "plain.elf").c_str(),
                error, sizeof error) == 0);
    CHECK(read_file(root / "plain.elf") == elf);
    fs::remove_all(root);
}

namespace {

const recomp::SelfKey* build_npdrm_self_key()
{
    for (const recomp::SelfKey& k : recomp::builtin_self_keys())
        if (k.type == 8) return &k;
    return nullptr;
}

// `elf` as a PSN executable of this licence type, encrypted with the build's keys (in memory only).
std::vector<u8> psn_self(const std::vector<u8>& elf, s32 license, const recomp::Key128& content_key)
{
    const recomp::SelfKey* key = build_npdrm_self_key();
    test::SelfEncryption e;
    e.key = *key;
    e.revision = static_cast<u16>(key->revision);
    e.npdrm = true;
    e.npdrm_license = license;
    recomp::crypto::aes_ecb_decrypt(recomp::builtin_npdrm_keys().klic_key, content_key.data(), 16,
        e.npdrm_layer_key.data());
    return test::encrypt_self(elf, e);
}

test::PkgSpec game_package(std::vector<u8> eboot, std::string_view app_ver)
{
    test::PkgSpec spec;
    spec.content_id = kContent;
    spec.counter = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0, 0, 0, 0, 0, 0, 0, 1};
    spec.qa_digest = {0xC0, 0xFF, 0xEE, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    spec.items = {
        {"PARAM.SFO", recomp::kPkgEntryRegular | recomp::kPkgEntryOverwrite,
            test::build_param_sfo({{"CATEGORY", "HG"}, {"TITLE_ID", "NPXX00000"}, {"APP_VER", std::string(app_ver)}})},
        {"USRDIR", recomp::kPkgEntryFolder, {}},
        {"USRDIR/EBOOT.BIN", recomp::kPkgEntryNpdrm | recomp::kPkgEntryOverwrite, std::move(eboot)},
        {"USRDIR/DATA.BIN", recomp::kPkgEntryRegular, std::vector<u8>(5000, 0x42)}};
    return spec;
}

void collect(void* user, const char* line) { static_cast<std::vector<std::string>*>(user)->emplace_back(line); }

bool has_line(const std::vector<std::string>& lines, std::string_view text)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("ps_psn_install_pkg: a game's package into dev_hdd0, what it reports, and what it refuses (D-583)",
    "[disc][psn]")
{
    // A debug package needs no keys: a plain ELF as its executable.
    const std::vector<u8> elf = test::build_ppu_exec().bytes;
    test::PkgSpec spec = game_package(elf, "01.00");
    spec.retail = false;
    // Canonical, as the installer reports the executable's path (macOS's temporary folders are reached through
    // /var -> /private/var).
    const fs::path root = fs::weakly_canonical(temp_dir("pkg"));
    const fs::path hdd0 = root / "dev_hdd0", game = hdd0 / "game" / "NPXX00000";
    write_file(root / "game.pkg", test::build_pkg(spec));
    std::vector<std::string> lines;
    char error[512] = {};
    REQUIRE(ps_psn_install_pkg((root / "game.pkg").c_str(), hdd0.c_str(), collect, &lines, error, sizeof error) == 0);
    CHECK(read_file(game / "USRDIR" / "EBOOT.BIN") == elf);
    CHECK(read_file(game / "USRDIR" / "DATA.BIN") == std::vector<u8>(5000, 0x42));
    CHECK(has_line(lines, std::format("installed 3 files ({} bytes) and 1 folder into {}",
                              spec.items[0].bytes.size() + elf.size() + 5000, game.string())));
    CHECK(has_line(lines, "the game's executable: " + (game / "USRDIR" / "EBOOT.BIN").string()));
    const fs::path out = root / "out" / "EBOOT.elf";
    REQUIRE(ps_psn_decrypt_executable(game.c_str(), hdd0.c_str(), out.c_str(), error, sizeof error) == 0);
    CHECK(read_file(out) == elf);

    // Again: the files not marked to overwrite are kept, and said to be.
    lines.clear();
    REQUIRE(ps_psn_install_pkg((root / "game.pkg").c_str(), hdd0.c_str(), collect, &lines, error, sizeof error) == 0);
    CHECK(has_line(lines, "installed 2 files (" + std::to_string(elf.size() + spec.items[0].bytes.size()) +
                              " bytes) and 1 folder into " + game.string() + "; 1 file already there kept"));

    // Refused, each with its reason: no package, no hdd0 folder, a retail package without keys or another version's
    // update.
    CHECK(ps_psn_install_pkg((root / "missing.pkg").c_str(), hdd0.c_str(), nullptr, nullptr, error, sizeof error) == 1);
    CHECK(std::string(error).find("cannot read") != std::string::npos);
    CHECK(ps_psn_install_pkg((root / "game.pkg").c_str(), nullptr, nullptr, nullptr, error, sizeof error) == 1);
    CHECK(std::string(error).find("no /dev_hdd0 folder") != std::string::npos);
    test::PkgSpec update = game_package(elf, "01.02");
    update.retail = false;
    update.content_type = recomp::kPkgContentGameData;
    update.package_flags = recomp::kPkgFlagPatch;
    update.items[0].bytes = test::build_param_sfo(
        {{"CATEGORY", "GD"}, {"TITLE_ID", "NPXX00000"}, {"APP_VER", "01.02"}, {"TARGET_APP_VER", "01.01"}});
    write_file(root / "update.pkg", test::build_pkg(update));
    CHECK(ps_psn_install_pkg((root / "update.pkg").c_str(), hdd0.c_str(), nullptr, nullptr, error, sizeof error) == 1);
    CHECK(std::string(error).find("for version 01.01, and version 01.00 is installed") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("A PSN game from its package and the player's licence to its ELF (the build's keys)", "[disc][psn]")
{
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    if (!keys.present || !build_npdrm_self_key() || !recomp::builtin_pkg_keys().present)
        SKIP("this build has no NPDRM or package keys");
    // A game of licence type 2 (the player's licence needed), packaged with the build's PKG_AES_KEY (in a temporary
    // folder only).
    const std::vector<u8> elf = test::build_ppu_exec().bytes;
    recomp::Key128 rap{};
    rap.fill(0xA7);
    test::PkgSpec spec = game_package(psn_self(elf, 2, recomp::rap_to_rif(rap, keys)), "01.00");
    spec.key = recomp::builtin_pkg_keys().ps3;
    const fs::path root = temp_dir("pkg-psn");
    const fs::path hdd0 = root / "dev_hdd0", game = hdd0 / "game" / "NPXX00000";
    write_file(root / "game.pkg", test::build_pkg(spec));
    std::vector<std::string> lines;
    char error[512] = {};
    REQUIRE(ps_psn_install_pkg((root / "game.pkg").c_str(), hdd0.c_str(), collect, &lines, error, sizeof error) == 0);
    const std::string rap_name = std::string(test::kSelfContentId) + ".rap";
    CHECK(has_line(lines, "it needs the player's licence " + rap_name + ", which is not there yet"));

    // The executable does not open without the licence; with it (put in place as the app would), it does.
    const fs::path out = root / "EBOOT.elf";
    REQUIRE(ps_psn_decrypt_executable(game.c_str(), hdd0.c_str(), out.c_str(), error, sizeof error) == 1);
    CHECK(std::string(error).find(rap_name) != std::string::npos);
    write_file(root / rap_name, std::vector<u8>(rap.begin(), rap.end()));
    REQUIRE(ps_psn_install_rap(hdd0.c_str(), (root / rap_name).c_str(), error, sizeof error) == 0);
    REQUIRE(ps_psn_decrypt_executable(game.c_str(), hdd0.c_str(), out.c_str(), error, sizeof error) == 0);
    check_same_elf(elf, read_file(out));
    lines.clear();
    REQUIRE(ps_psn_install_pkg((root / "game.pkg").c_str(), hdd0.c_str(), collect, &lines, error, sizeof error) == 0);
    CHECK(has_line(lines, ", which is in place ("));
    fs::remove_all(root);
}

TEST_CASE("Games that start from /dev_hdd0/game: an installed PSN game, a disc game's update (D-939)", "[disc][psn]")
{
    // An installed game, from its package (a debug one: a plain ELF as its executable).
    const std::vector<u8> elf = test::build_ppu_exec().bytes;
    test::PkgSpec spec = game_package(elf, "01.00");
    spec.retail = false;
    const fs::path root = fs::weakly_canonical(temp_dir("hddgame"));
    const fs::path hdd0 = root / "dev_hdd0", game = hdd0 / "game" / "NPXX00000";
    write_file(root / "game.pkg", test::build_pkg(spec));
    char error[512] = {};
    REQUIRE(ps_psn_install_pkg((root / "game.pkg").c_str(), hdd0.c_str(), nullptr, nullptr, error, sizeof error) == 0);
    std::string why;
    const auto installed = loader::read_hdd_game(hdd0.string(), game.string() + "/", &why);
    INFO(why);
    REQUIRE(installed);
    CHECK(installed->dir == "NPXX00000");
    CHECK(installed->title_id == "NPXX00000");
    CHECK(installed->category == "HG");
    CHECK(installed->app_ver == "01.00");
    CHECK_FALSE(installed->update());
    CHECK(installed->boot_path == "/dev_hdd0/game/NPXX00000/USRDIR/EBOOT.BIN");
    const hle::GameInfo info = installed->game_info();
    CHECK(info.boot == hle::GameInfo::Boot::Hdd);
    CHECK(info.hdd_dir() == "NPXX00000");
    CHECK(info.param_sfo == read_file(game / "PARAM.SFO"));
    // By its executable (pspocket-boot), with the executable's own guest path; an executable elsewhere is no HDD
    // game, and a folder outside hdd0/game is refused.
    const auto by_exe = loader::hdd_game_of(hdd0.string(), (game / "USRDIR" / "EBOOT.BIN").string(), &why);
    REQUIRE(by_exe);
    CHECK(by_exe->boot_path == "/dev_hdd0/game/NPXX00000/USRDIR/EBOOT.BIN");
    why.clear();
    CHECK_FALSE(loader::hdd_game_of(hdd0.string(), (root / "game.pkg").string(), &why));
    CHECK(why.empty());
    CHECK_FALSE(loader::hdd_game_of(hdd0.string(), (hdd0 / "game" / "EBOOT.BIN").string(), &why));
    CHECK_FALSE(loader::read_hdd_game(hdd0.string(), root.string(), &why));
    CHECK(why.find("is not a folder of") != std::string::npos);
    fs::create_directories(hdd0 / "game" / "EMPTY" / "USRDIR");
    CHECK_FALSE(loader::hdd_game_of(hdd0.string(), (hdd0 / "game" / "EMPTY" / "USRDIR" / "EBOOT.BIN").string(), &why));
    CHECK(why.find("has no PARAM.SFO") != std::string::npos);
    // Its USRDIR is /app_home.
    GuestMemory memory;
    REQUIRE(memory.reserve());
    lv2::Kernel kernel(memory);
    REQUIRE(loader::mount_app_home(kernel, *installed, &why));
    CHECK(kernel.vfs_host_path("/app_home/DATA.BIN") == (game / "USRDIR" / "DATA.BIN").string());

    // A disc game's update: the folder of the disc's title ID, category GD, with its own executable. Game data of the
    // same title (another folder, or no executable) and an installed game are not updates.
    CHECK_FALSE(loader::find_update(hdd0.string(), "NPXX00000")); // category HG
    const fs::path update = hdd0 / "game" / "BLES01807";
    write_file(update / "PARAM.SFO",
        test::build_param_sfo({{"APP_VER", "01.27"}, {"CATEGORY", "GD"}, {"TITLE", "Test"}, {"TITLE_ID", "BLES01807"}}));
    CHECK_FALSE(loader::find_update(hdd0.string(), "BLES01807")); // no executable yet
    write_file(update / "USRDIR" / "EBOOT.BIN", elf);
    const auto found = loader::find_update(hdd0.string(), "BLES01807");
    REQUIRE(found);
    CHECK(found->update());
    CHECK(found->app_ver == "01.27");
    CHECK(found->host_dir == update.string());
    CHECK(found->boot_path == "/dev_hdd0/game/BLES01807/USRDIR/EBOOT.BIN");
    CHECK_FALSE(loader::find_update(hdd0.string(), "BLES01808"));
    CHECK_FALSE(loader::find_update(hdd0.string(), ""));
    CHECK_FALSE(loader::find_update(hdd0.string(), "../dev_hdd0/game/BLES01807"));
    write_file(update / "PARAM.SFO",
        test::build_param_sfo({{"APP_VER", "01.27"}, {"CATEGORY", "GD"}, {"TITLE_ID", "BLES01808"}}));
    CHECK_FALSE(loader::find_update(hdd0.string(), "BLES01807")); // another title's

    // The installed game's own modules, saved for its preparation as a disc's are (D-239): LIB/GAME.SPRX a module (a
    // debug SELF), SPU/TASK.ELF an SPU program, LIB/NP.SPRX a SELF that does not decrypt, DATA/LEVEL.BIN data.
    const auto prx = test::build_ppu_prx();
    write_file(game / "USRDIR" / "LIB" / "GAME.SPRX",
        test::wrap_in_self(prx.bytes, {.debug = true, .npdrm = false, .self_type = 4}));
    std::vector<u8> spu(0x100, 0);
    const u8 ident[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(spu.data(), ident, 16);
    spu[0x11] = 2;  // ET_EXEC
    spu[0x13] = 23; // EM_SPU
    write_file(game / "USRDIR" / "SPU" / "TASK.ELF", spu);
    std::vector<u8> np(3000, 7);
    std::memcpy(np.data(), "SCE\0", 4);
    write_file(game / "USRDIR" / "LIB" / "NP.SPRX", np);
    write_file(game / "USRDIR" / "DATA" / "LEVEL.BIN", std::vector<u8>(5000, 8));
    const fs::path prx_dir = root / "prepared" / "disc-prx", spu_dir = root / "prepared" / "disc-spu";
    REQUIRE(ps_psn_save_modules(game.c_str(), hdd0.c_str(), prx_dir.c_str(), spu_dir.c_str(), error, sizeof error) == 2);
    auto module_elf = recomp::ElfFile::parse(prx.bytes);
    REQUIRE(module_elf);
    const std::string identity = recomp::prx_identity(*module_elf);
    CHECK(read_file(prx_dir / (identity + ".elf")) == prx.bytes);
    const std::vector<u8> where = read_file(prx_dir / (identity + ".txt"));
    CHECK(std::string(where.begin(), where.end()).starts_with("path /dev_hdd0/game/NPXX00000/USRDIR/LIB/GAME.SPRX\n"));
    CHECK(ps_psn_save_modules((root / "nowhere").c_str(), hdd0.c_str(), prx_dir.c_str(), spu_dir.c_str(), error,
              sizeof error) == -1);
    CHECK(std::string(error).find("has no USRDIR") != std::string::npos);
    fs::remove_all(root);
}
