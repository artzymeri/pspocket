// PS3 packages (D-583): synthetic packages built here (test::build_pkg) and encrypted with made-up keys the way Sony's
// are, installed by recomp::install_pkg into a temporary dev_hdd0 folder: where each kind goes, the files as they
// were, SDATA files decrypted, what is kept and what replaced, names and links that would leave the install folder,
// updates that do not suit the game installed, split packages, and packages refused. With the build's keys, a package
// made with them (skipped without them); where the build has RPCS3's source, random packages are also read by RPCS3's
// own package_reader (rpcs3_ref). No real keys and no game data in the tests (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/pkg.hpp"
#include "synthetic.hpp"

#if defined(PSPOCKET_HAVE_RPCS3_REF)
#include "rpcs3_ref/rpcs3_ref.hpp"
#endif

using namespace pspocket;
using namespace pspocket::recomp;
using pspocket::test::build_pkg;
using pspocket::test::PkgItem;
using pspocket::test::PkgSpec;
namespace fs = std::filesystem;

namespace {

Key128 key_of(u8 seed)
{
    Key128 k{};
    for (int i = 0; i < 16; i++) k[i] = static_cast<u8>(seed + 37 * i);
    return k;
}

PkgKeys made_up_pkg_keys()
{
    PkgKeys k;
    k.ps3 = key_of(0x31);
    k.idu = key_of(0x47);
    k.present = true;
    return k;
}

NpdKeys made_up_npd_keys(u8 seed = 0x10)
{
    NpdKeys k;
    for (int i = 0; i < 16; i++) {
        k.sdat[i] = static_cast<u8>(seed + 7 * i);
        k.edat_key0[i] = static_cast<u8>(seed + 0x10 + 11 * i);
        k.edat_key1[i] = static_cast<u8>(seed + 0x20 + 13 * i);
        k.edat_hash0[i] = static_cast<u8>(seed + 0x30 + 17 * i);
        k.edat_hash1[i] = static_cast<u8>(seed + 0x40 + 19 * i);
        k.edat_iv[i] = static_cast<u8>(seed + 0x50 + 23 * i);
    }
    k.present = true;
    return k;
}

std::vector<u8> pattern(usize size, u32 seed)
{
    std::vector<u8> v(size);
    u32 x = seed;
    for (u8& b : v) {
        x = x * 1103515245u + 12345u;
        b = static_cast<u8>(x >> 16);
    }
    return v;
}

std::vector<u8> bytes_of(std::string_view text) { return std::vector<u8>(text.begin(), text.end()); }

fs::path temp_dir(std::string_view what)
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-pkg-{}-{}", what, static_cast<long>(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, std::span<const u8> bytes)
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

constexpr std::string_view kContent = "UP0001-NPXX00000_00-0000000000000001";

PkgItem file(std::string name, std::vector<u8> bytes, u32 type = kPkgEntryRegular)
{
    return PkgItem{std::move(name), type, std::move(bytes)};
}

PkgItem folder(std::string name) { return PkgItem{std::move(name), kPkgEntryFolder, {}}; }

std::vector<u8> game_sfo(std::string_view title_id, std::string_view app_ver, std::string_view category = "HG",
    std::string_view target = "")
{
    std::vector<std::pair<std::string, std::string>> s{{"CATEGORY", std::string(category)},
        {"TITLE_ID", std::string(title_id)}, {"APP_VER", std::string(app_ver)}, {"TITLE", "A synthetic game"}};
    if (!target.empty()) s.emplace_back("TARGET_APP_VER", std::string(target));
    return test::build_param_sfo(s);
}

// A game: its PARAM.SFO, EBOOT.BIN, a file across several of the installer's 1 MB pieces, an empty one, an icon.
PkgSpec game_spec(const Key128& key)
{
    PkgSpec spec;
    spec.content_id = std::string(kContent);
    spec.key = key;
    // A counter block whose low 64 bits are about to carry: the data's blocks count across them.
    spec.counter = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
    spec.items = {file("PARAM.SFO", game_sfo("NPXX00000", "01.00")), folder("USRDIR"),
        file("USRDIR/EBOOT.BIN", pattern(0x1234, 1), kPkgEntryNpdrm), folder("USRDIR/data"),
        file("USRDIR/data/big.bin", pattern((5u << 19) + 7, 2)), file("USRDIR/data/empty.bin", {}),
        file("ICON0.PNG", pattern(1000, 3))};
    return spec;
}

std::string message(const Result<PkgInstalled>& r) { return r ? std::string("installed") : r.error().message; }

struct Install {
    Result<PkgInstalled> result;
    std::vector<std::string> lines;
};

Install install(const fs::path& pkg, const fs::path& hdd0, const PkgKeys& keys = made_up_pkg_keys(),
    const NpdKeys& npd_keys = made_up_npd_keys())
{
    std::vector<std::string> lines;
    auto r = install_pkg(pkg.string(), hdd0.string(), [&lines](std::string_view l) { lines.emplace_back(l); }, keys,
        npd_keys);
    return Install{std::move(r), std::move(lines)};
}

bool has_line(const std::vector<std::string>& lines, std::string_view text)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

// Every item of `spec` under `dir`: files with their bytes, folders made.
void check_installed(const PkgSpec& spec, const fs::path& dir)
{
    for (const PkgItem& item : spec.items) {
        INFO(item.name);
        if ((item.type & 0xFF) == kPkgEntryFolder) {
            CHECK(fs::is_directory(dir / item.name));
        } else {
            REQUIRE(fs::is_regular_file(dir / item.name));
            CHECK(read_file(dir / item.name) == item.bytes);
        }
    }
}

} // namespace

TEST_CASE("PKG: a game's package installs into game/<TITLE_ID>, its files as they were packed", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const PkgSpec spec = game_spec(keys.ps3);
    const fs::path root = temp_dir("game");
    write_file(root / "game.pkg", build_pkg(spec));
    const Install in = install(root / "game.pkg", root / "hdd0", keys);
    INFO(message(in.result));
    REQUIRE(in.result);
    const PkgInstalled& r = *in.result;
    const fs::path dir = root / "hdd0" / "game" / "NPXX00000";
    CHECK(r.install_path == dir.string());
    check_installed(spec, dir);
    CHECK(r.files == 5);
    CHECK(r.folders == 2);
    CHECK(r.kept == 0);
    CHECK(r.bytes == spec.items[0].bytes.size() + 0x1234 + (5u << 19) + 7 + 1000);
    CHECK(r.eboot == (dir / "USRDIR" / "EBOOT.BIN").string());
    CHECK(r.info.content_id == kContent);
    CHECK(r.info.retail);
    CHECK(r.info.content_type == kPkgContentGameExec);
    CHECK(r.info.drm_type == 3);
    CHECK(r.info.install_dir == "NPXX00000");
    CHECK(r.info.file_count == 7);
    CHECK(r.info.parts == 1);
    CHECK(has_line(in.lines, std::string(kContent) + " (7 entries) into "));
    CHECK(has_line(in.lines, "file USRDIR/EBOOT.BIN (4660 bytes)"));
    CHECK(has_line(in.lines, "folder USRDIR/data"));
    CHECK_FALSE(has_line(in.lines, "IDU"));

    // What read_pkg_info says without keys.
    const auto info = read_pkg_info((root / "game.pkg").string());
    REQUIRE(info);
    CHECK(info->content_id == kContent);
    CHECK(info->content_type == kPkgContentGameExec);
    CHECK(info->install_dir == "NPXX00000");
    CHECK(info->file_count == 7);
    CHECK(info->package_flags == 0);
    fs::remove_all(root);
}

TEST_CASE("PKG: the key, as RPCS3 picks it: PKG_AES_KEY, else the IDU key; no keys, a debug package", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("keys");

    // An in-store demo unit's package opens with the second key, and says so.
    PkgSpec idu = game_spec(keys.idu);
    write_file(root / "idu.pkg", build_pkg(idu));
    Install in = install(root / "idu.pkg", root / "hdd0", keys);
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(has_line(in.lines, "in-store demo (IDU)"));
    check_installed(idu, root / "hdd0" / "game" / "NPXX00000");
    fs::remove_all(root / "hdd0");

    // A package neither key opens is refused, nothing made.
    PkgSpec other = game_spec(key_of(0x99));
    write_file(root / "other.pkg", build_pkg(other));
    in = install(root / "other.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("does not decrypt with either package key") != std::string::npos);
    CHECK_FALSE(fs::exists(root / "hdd0"));

    // A build without the keys refuses a retail package...
    in = install(root / "idu.pkg", root / "hdd0", PkgKeys{});
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("no package keys") != std::string::npos);
    CHECK_FALSE(fs::exists(root / "hdd0"));

    // ...and installs a debug one, whose key stream comes from its QA digest.
    PkgSpec debug = game_spec({});
    debug.retail = false;
    debug.qa_digest = key_of(0x5D);
    write_file(root / "debug.pkg", build_pkg(debug));
    in = install(root / "debug.pkg", root / "hdd0", PkgKeys{});
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK_FALSE(in.result->info.retail);
    check_installed(debug, root / "hdd0" / "game" / "NPXX00000");
    fs::remove_all(root);
}

TEST_CASE("PKG: where each content type goes under dev_hdd0 (RPCS3 set_install_path)", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("types");
    const fs::path hdd0 = root / "hdd0";
    const auto where = [&](u32 content_type, std::string install_dir = {}) -> std::string {
        fs::remove_all(hdd0);
        PkgSpec spec;
        spec.key = keys.ps3;
        spec.content_type = content_type;
        spec.install_dir = std::move(install_dir);
        spec.items = {file("thing.bin", pattern(40, content_type))};
        write_file(root / "p.pkg", build_pkg(spec));
        const Install in = install(root / "p.pkg", hdd0, keys);
        if (!in.result) return "refused: " + in.result.error().message;
        CHECK(read_file(fs::path(in.result->install_path) / "thing.bin") == spec.items[0].bytes);
        return fs::relative(in.result->install_path, hdd0).string();
    };
    CHECK(where(kPkgContentGameExec) == "game/NPXX00000");
    CHECK(where(kPkgContentGameData) == "game/NPXX00000");
    CHECK(where(0) == "game/NPXX00000"); // no content type in the metadata
    CHECK(where(kPkgContentGameData, "NPYY00001") == "game/NPYY00001"); // an add-on's own folder
    CHECK(where(kPkgContentLicense) == "home/00000001/exdata");
    CHECK(where(kPkgContentTheme) == "theme");
    CHECK(where(kPkgContentWidget) == "widget/NPXX00000");
    CHECK(where(kPkgContentVshModule) == "vsh/modules/NPXX00000");
    CHECK(where(kPkgContentAvatar) == "home/00000001/psn_avatar/NPXX00000");
    CHECK(where(kPkgContentVmc) == "tmp/vmc/NPXX00000");
    // An install folder that is not a folder's name is refused.
    CHECK(where(kPkgContentGameData, "../NPYY00001").find("is not a folder name") != std::string::npos);
    CHECK(where(kPkgContentGameData, "A/B").find("is not a folder name") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("PKG: names and links that would leave the install folder refuse the package", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("names");
    const fs::path hdd0 = root / "hdd0", dir = hdd0 / "game" / "NPXX00000";
    const auto install_names = [&](std::vector<PkgItem> items) {
        PkgSpec spec;
        spec.key = keys.ps3;
        spec.items = std::move(items);
        write_file(root / "p.pkg", build_pkg(spec));
        return install(root / "p.pkg", hdd0, keys);
    };
    for (const char* name : {"../evil.bin", "/evil.bin", "USRDIR/../../evil.bin", "./evil.bin", "USRDIR/./evil.bin",
             "USRDIR/.."}) {
        INFO(name);
        const Install in = install_names({file("ok.bin", pattern(10, 1)), file(name, pattern(10, 2))});
        REQUIRE_FALSE(in.result);
        CHECK(in.result.error().message.find("would leave the install folder") != std::string::npos);
        CHECK_FALSE(fs::exists(dir)); // checked before anything is written
        CHECK_FALSE(fs::exists(root / "evil.bin"));
        CHECK_FALSE(fs::exists(hdd0 / "game" / "evil.bin"));
    }
    // Dots inside a name are a name.
    const Install dots = install_names({file("a..b", pattern(3, 1)), file("...", pattern(3, 2)), file(".hidden", {}),
        folder("USRDIR"), file("USRDIR/.x", pattern(3, 3))});
    INFO(message(dots.result));
    REQUIRE(dots.result);
    CHECK(fs::exists(dir / "..."));
    CHECK(fs::exists(dir / "USRDIR" / ".x"));
    fs::remove_all(hdd0);

    // A NUL inside a name, a file without a name: refused. A folder without one is the install folder.
    Install in = install_names({file(std::string("a\0b", 3), pattern(3, 1))});
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("has a NUL inside") != std::string::npos);
    in = install_names({file("", pattern(3, 1))});
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("has no name") != std::string::npos);
    in = install_names({folder(""), file("x.bin", pattern(3, 1))});
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(in.result->folders == 0);
    fs::remove_all(hdd0);

    // A link already in the install folder that leads out of it is not followed: refused, the folder and the link left.
    const fs::path outside = root / "outside";
    fs::create_directories(outside);
    fs::create_directories(dir);
    fs::create_directory_symlink(outside, dir / "USRDIR");
    in = install_names({folder("USRDIR"), file("USRDIR/EBOOT.BIN", pattern(64, 4))});
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("leaves the install folder") != std::string::npos);
    CHECK(fs::is_empty(outside));
    CHECK(fs::is_symlink(dir / "USRDIR"));
    in = install_names({file("USRDIR/EBOOT.BIN", pattern(64, 4))}); // a file through it, no folder entry
    REQUIRE_FALSE(in.result);
    CHECK(fs::is_empty(outside));
    // One that stays inside is followed, as RPCS3 does.
    fs::create_directories(dir / "real");
    fs::create_directory_symlink(dir / "real", dir / "inside");
    in = install_names({folder("inside"), file("inside/x.bin", pattern(64, 5))});
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(read_file(dir / "real" / "x.bin") == pattern(64, 5));
    fs::remove_all(root);
}

TEST_CASE("PKG: files already there stay unless marked to overwrite; of two entries, the later one wins only so marked",
    "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("overwrite");
    const fs::path dir = root / "hdd0" / "game" / "NPXX00000";
    write_file(dir / "keep.bin", bytes_of("old"));
    write_file(dir / "replace.bin", bytes_of("old"));
    PkgSpec spec;
    spec.key = keys.ps3;
    spec.items = {file("keep.bin", bytes_of("new")), file("replace.bin", bytes_of("new"), kPkgEntryRegular | kPkgEntryOverwrite),
        file("first.bin", bytes_of("A")), file("first.bin", bytes_of("B")),
        file("later.bin", bytes_of("A")), file("later.bin", bytes_of("B"), kPkgEntryRegular | kPkgEntryOverwrite),
        file("marked.bin", bytes_of("A"), kPkgEntryRegular | kPkgEntryOverwrite), file("marked.bin", bytes_of("B"))};
    write_file(root / "p.pkg", build_pkg(spec));
    Install in = install(root / "p.pkg", root / "hdd0", keys);
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(read_file(dir / "keep.bin") == bytes_of("old"));
    CHECK(read_file(dir / "replace.bin") == bytes_of("new"));
    CHECK(read_file(dir / "first.bin") == bytes_of("A"));
    CHECK(read_file(dir / "later.bin") == bytes_of("B"));
    CHECK(read_file(dir / "marked.bin") == bytes_of("A"));
    CHECK(in.result->kept == 1);
    CHECK(in.result->files == 4);
    CHECK(has_line(in.lines, "kept keep.bin"));
    CHECK(has_line(in.lines, "replaced replace.bin"));

    // A file where a folder is: refused, and the folder (not new) is left as it was.
    fs::create_directories(dir / "taken");
    spec.items = {file("x.bin", bytes_of("x")), file("taken", bytes_of("y"), kPkgEntryRegular | kPkgEntryOverwrite)};
    write_file(root / "p.pkg", build_pkg(spec));
    in = install(root / "p.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("cannot write") != std::string::npos);
    CHECK(fs::is_directory(dir / "taken"));
    CHECK(read_file(dir / "x.bin") == bytes_of("x")); // what it wrote stays: the folder was there before
    // A folder where a file is; an install folder where a file is: refused, the files left.
    spec.items = {folder("keep.bin"), file("keep.bin/y.bin", bytes_of("y"))};
    write_file(root / "p.pkg", build_pkg(spec));
    in = install(root / "p.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("something that is not a folder is there") != std::string::npos);
    CHECK(read_file(dir / "keep.bin") == bytes_of("old"));
    fs::remove_all(dir);
    write_file(dir, bytes_of("a file"));
    spec.items = {file("z.bin", bytes_of("z"))};
    write_file(root / "p.pkg", build_pkg(spec));
    in = install(root / "p.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("is there, and is not a folder") != std::string::npos);
    CHECK(read_file(dir) == bytes_of("a file"));
    fs::remove_all(root);
}

TEST_CASE("PKG: SDATA files are written decrypted, EDATA files and executables as they are", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const NpdKeys npd = made_up_npd_keys();
    const fs::path root = temp_dir("sdat");
    const fs::path dir = root / "hdd0" / "game" / "NPXX00000";
    const std::vector<u8> plain = pattern(5000, 7);
    test::NpdOptions edata;
    edata.edata = true;
    edata.key = key_of(0x66);
    PkgSpec spec;
    spec.key = keys.ps3;
    spec.items = {folder("USRDIR"), file("USRDIR/data.sdat", test::build_sdata(plain, npd), kPkgEntrySdat),
        file("USRDIR/data.edat", test::build_npd(plain, npd, edata), kPkgEntryNpdrmEdat),
        file("USRDIR/EBOOT.BIN", pattern(300, 8), kPkgEntryNpdrm)};
    write_file(root / "p.pkg", build_pkg(spec));
    Install in = install(root / "p.pkg", root / "hdd0", keys, npd);
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(read_file(dir / "USRDIR" / "data.sdat") == plain);
    CHECK(read_file(dir / "USRDIR" / "data.edat") == spec.items[2].bytes);
    CHECK(read_file(dir / "USRDIR" / "EBOOT.BIN") == spec.items[3].bytes);
    CHECK(has_line(in.lines, "file USRDIR/data.sdat (5000 bytes, decrypted)"));
    CHECK(in.result->bytes == 5000 + spec.items[2].bytes.size() + 300);
    fs::remove_all(root / "hdd0");

    // An SDATA file that does not decrypt (other keys; none) fails the install; the new folder is removed.
    in = install(root / "p.pkg", root / "hdd0", keys, made_up_npd_keys(0x80));
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("the SDATA file USRDIR/data.sdat does not decrypt") != std::string::npos);
    CHECK(in.result.error().message.find("the partial install was removed") != std::string::npos);
    CHECK_FALSE(fs::exists(dir));
    CHECK(fs::is_directory(root / "hdd0" / "game"));
    in = install(root / "p.pkg", root / "hdd0", keys, NpdKeys{});
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("no NPD keys") != std::string::npos);
    CHECK_FALSE(fs::exists(dir));

    // Into a folder that was there (an update), what was written before the failure stays.
    write_file(dir / "PARAM.SFO", game_sfo("NPXX00000", "01.00"));
    spec.items = {file("first.bin", bytes_of("first")), file("bad.sdat", test::build_sdata(plain, npd), kPkgEntrySdat)};
    write_file(root / "p.pkg", build_pkg(spec));
    in = install(root / "p.pkg", root / "hdd0", keys, made_up_npd_keys(0x80));
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("removed") == std::string::npos);
    CHECK(read_file(dir / "first.bin") == bytes_of("first"));
    CHECK(fs::exists(dir / "PARAM.SFO"));
    fs::remove_all(root);
}

TEST_CASE("PKG: an update must suit the game installed (RPCS3 check_target_app_version)", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("update");
    const fs::path hdd0 = root / "hdd0", dir = hdd0 / "game" / "NPXX00000";
    const auto package = [&](std::vector<u8> sfo, u32 flags, std::string_view eboot) {
        PkgSpec spec;
        spec.key = keys.ps3;
        spec.content_type = kPkgContentGameData;
        spec.package_flags = flags;
        spec.items = {file("PARAM.SFO", std::move(sfo), kPkgEntryRegular | kPkgEntryOverwrite), folder("USRDIR"),
            file("USRDIR/EBOOT.BIN", bytes_of(eboot), kPkgEntryNpdrm | kPkgEntryOverwrite)};
        write_file(root / "p.pkg", build_pkg(spec));
        return install(root / "p.pkg", hdd0, keys);
    };
    const auto refused = [](const Install& in) { return in.result ? std::string("installed") : in.result.error().message; };

    // A patch for 01.00 before the game is installed: refused, nothing made.
    Install in = package(game_sfo("NPXX00000", "01.01", "GD", "01.00"), kPkgFlagPatch, "patch 1");
    CHECK(refused(in).find("for version 01.00, which is not installed") != std::string::npos);
    CHECK_FALSE(fs::exists(dir));

    // The game (category HG), then its first patch for 01.00.
    in = package(game_sfo("NPXX00000", "01.00"), 0, "game");
    REQUIRE(in.result);
    in = package(game_sfo("NPXX00000", "01.01", "GD", "01.00"), kPkgFlagPatch, "patch 1");
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(read_file(dir / "USRDIR" / "EBOOT.BIN") == bytes_of("patch 1"));

    // A patch for 01.00 again, now that 01.01 is installed: refused, the executable left as it was.
    in = package(game_sfo("NPXX00000", "01.02", "GD", "01.00"), kPkgFlagPatch, "patch 2 for 01.00");
    CHECK(refused(in).find("an update to version 01.02 of NPXX00000 for version 01.00, and version 01.01 is installed") !=
        std::string::npos);
    CHECK(read_file(dir / "USRDIR" / "EBOOT.BIN") == bytes_of("patch 1"));
    // For 1.010 (the same version): installed.
    in = package(game_sfo("NPXX00000", "01.02", "GD", "1.010"), kPkgFlagPatch, "patch 2");
    INFO(message(in.result));
    REQUIRE(in.result);

    // A patch with no target must not be older than what is installed; an add-on (no patch flag) may be.
    in = package(game_sfo("NPXX00000", "01.01", "GD"), kPkgFlagPatch, "old patch");
    CHECK(refused(in).find("older than the version installed (01.02)") != std::string::npos);
    in = package(game_sfo("NPXX00000", "01.02", "GD"), kPkgFlagPatch, "same patch");
    CHECK(in.result);
    in = package(game_sfo("NPXX00000", "01.00", "GD"), 0, "add-on");
    CHECK(in.result);
    // Without an APP_VER, or for another game's folder, or not an update (HG): nothing to check.
    in = package(test::build_param_sfo({{"CATEGORY", "GD"}, {"TITLE_ID", "NPXX00000"}, {"TARGET_APP_VER", "09.00"}}),
        kPkgFlagPatch, "unversioned");
    CHECK(in.result);
    in = package(game_sfo("NPXX00000", "00.50", "HG"), 0, "a game again");
    CHECK(in.result);
    // A version that does not read is refused, as RPCS3's strtod refuses it.
    write_file(dir / "PARAM.SFO", game_sfo("NPXX00000", "1.0x"));
    in = package(game_sfo("NPXX00000", "01.03", "GD", "01.02"), kPkgFlagPatch, "patch 3");
    CHECK(refused(in).find("has a version that does not read (\"1.0x\")") != std::string::npos);
    // An installed PARAM.SFO RPCS3 would not load (an unknown category) counts as none.
    write_file(dir / "PARAM.SFO", game_sfo("NPXX00000", "01.02", "ZZ"));
    in = package(game_sfo("NPXX00000", "01.03", "GD", "05.00"), kPkgFlagPatch, "patch 3");
    CHECK(in.result);
    fs::remove_all(root);
}

TEST_CASE("PKG: a split package is read across its parts", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const PkgSpec spec = game_spec(keys.ps3);
    const std::vector<u8> whole = build_pkg(spec);
    const fs::path root = temp_dir("split");
    const usize cut1 = 0x100, cut2 = whole.size() / 2 + 3;
    write_file(root / "game_00.pkg", std::span(whole).subspan(0, cut1));
    write_file(root / "game_01.pkg", std::span(whole).subspan(cut1, cut2 - cut1));
    write_file(root / "game_02.pkg", std::span(whole).subspan(cut2));
    Install in = install(root / "game_00.pkg", root / "hdd0", keys);
    INFO(message(in.result));
    REQUIRE(in.result);
    CHECK(in.result->info.parts == 3);
    check_installed(spec, root / "hdd0" / "game" / "NPXX00000");
    fs::remove_all(root / "hdd0");

    // A part missing; an empty part; a short file that is not a first part.
    fs::rename(root / "game_02.pkg", root / "moved.pkg");
    in = install(root / "game_00.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("a part of the package is missing") != std::string::npos);
    CHECK(in.result.error().message.find("game_02.pkg") != std::string::npos);
    write_file(root / "game_02.pkg", std::span<const u8>());
    in = install(root / "game_00.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("game_02.pkg is empty") != std::string::npos);
    write_file(root / "short.pkg", std::span(whole).subspan(0, whole.size() - 1));
    in = install(root / "short.pkg", root / "hdd0", keys);
    REQUIRE_FALSE(in.result);
    CHECK(in.result.error().message.find("NAME_00.pkg") != std::string::npos);
    CHECK_FALSE(fs::exists(root / "hdd0"));
    fs::remove_all(root);
}

TEST_CASE("PKG: packages and entries pspocket does not install are refused, each with its reason", "[recomp][pkg]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("refused");
    const auto attempt = [&](const std::vector<u8>& bytes) {
        write_file(root / "p.pkg", bytes);
        const Install in = install(root / "p.pkg", root / "hdd0", keys);
        if (in.result) return std::string("installed");
        CHECK_FALSE(fs::exists(root / "hdd0" / "game" / "NPXX00000")); // refused before anything is made
        return in.result.error().message;
    };
    PkgSpec spec;
    spec.key = keys.ps3;
    spec.items = {file("a.bin", pattern(100, 1))};
    const std::vector<u8> good = build_pkg(spec);
    REQUIRE(attempt(good) == "installed");
    fs::remove_all(root / "hdd0");

    CHECK(attempt(pattern(0x200, 9)).find("not a PS3 package") != std::string::npos);
    CHECK(attempt(std::vector<u8>(good.begin(), good.begin() + 0x40)).find("too short for a package") != std::string::npos);
    std::vector<u8> bad = good;
    ps_st16(bad.data() + 4, 0x1234);
    CHECK(attempt(bad).find("unknown package type 0x1234") != std::string::npos);
    bad = good;
    ps_st16(bad.data() + 6, 2);
    CHECK(attempt(bad).find("a PSP or PS Vita package") != std::string::npos);
    bad = good;
    ps_st16(bad.data() + 6, 3);
    CHECK(attempt(bad).find("unknown package platform 0x3") != std::string::npos);
    bad = good;
    ps_st32(bad.data() + 0x14, 0x7FFFFFFF);
    CHECK(attempt(bad).find("cannot have their table") != std::string::npos);
    bad = good;
    ps_st64(bad.data() + 0x28, ~u64{0} - 4);
    CHECK(attempt(bad).find("past the package's end") != std::string::npos);
    bad = good;
    ps_st64(bad.data() + 0x18, good.size() + 1);
    CHECK(attempt(bad).find("the first part of a split one") != std::string::npos);

    spec.items = {file("psp.bin", pattern(10, 2), kPkgEntryRegular | kPkgEntryPsp)};
    CHECK(attempt(build_pkg(spec)).find("psp.bin is a PSP entry") != std::string::npos);
    spec.items = {file("odd.bin", pattern(10, 2), 7)};
    CHECK(attempt(build_pkg(spec)).find("a type RPCS3 does not install (0x7)") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("PKG: entries anywhere in the data, not only where Sony's packager puts them", "[recomp][pkg]")
{
    // Names and files packed one after another, so most start inside a 16-byte block: the key stream is taken from
    // where each starts (RPCS3 starts it at the block, which suits Sony's packages, all aligned).
    const PkgKeys keys = made_up_pkg_keys();
    const NpdKeys npd = made_up_npd_keys();
    const fs::path root = temp_dir("packed");
    for (const bool retail : {true, false}) {
        INFO(retail);
        PkgSpec spec = game_spec(keys.ps3);
        spec.retail = retail;
        spec.qa_digest = key_of(0x3C);
        spec.align = 1;
        spec.items.push_back(file("a", pattern(1, 4)));
        spec.items.push_back(file("bc", pattern(17, 5)));
        spec.items.push_back(file("USRDIR/data.sdat", test::build_sdata(pattern(3000, 6), npd), kPkgEntrySdat));
        write_file(root / "packed.pkg", build_pkg(spec));
        const Install in = install(root / "packed.pkg", root / "hdd0", keys, npd);
        INFO(message(in.result));
        REQUIRE(in.result);
        spec.items.back().bytes = pattern(3000, 6); // the SDATA file is written decrypted
        check_installed(spec, root / "hdd0" / "game" / "NPXX00000");
        fs::remove_all(root / "hdd0");
    }
    fs::remove_all(root);
}

TEST_CASE("PKG: a package made with the build's keys installs (keys from RPCS3's source, at build time only)",
    "[recomp][pkg]")
{
    const PkgKeys& keys = builtin_pkg_keys();
    if (!keys.present) SKIP("this build has no package keys");
#if defined(PSPOCKET_HAVE_RPCS3_REF)
    CHECK(test::rpcs3_ref::same_pkg_keys(keys));
#endif
    const fs::path root = temp_dir("build-keys");
    for (const Key128* key : {&keys.ps3, &keys.idu}) {
        const PkgSpec spec = game_spec(*key);
        write_file(root / "game.pkg", build_pkg(spec)); // in a temporary folder only
        std::vector<std::string> lines;
        const auto r = install_pkg((root / "game.pkg").string(), (root / "hdd0").string(),
            [&lines](std::string_view l) { lines.emplace_back(l); });
        INFO(message(r));
        REQUIRE(r);
        check_installed(spec, root / "hdd0" / "game" / "NPXX00000");
        CHECK(has_line(lines, "IDU") == (key == &keys.idu));
        fs::remove_all(root / "hdd0");
    }
    fs::remove_all(root);
}

#if defined(PSPOCKET_HAVE_RPCS3_REF)

TEST_CASE("PKG: random packages read the same by RPCS3's package_reader and installed the same", "[recomp][pkg][rpcs3]")
{
    std::mt19937_64 rng(0x5EED0583);
    const auto random_key = [&] {
        Key128 k{};
        for (u8& b : k) b = static_cast<u8>(rng());
        return k;
    };
    const fs::path root = temp_dir("rpcs3");
    for (int round = 0; round < 60; round++) {
        PkgSpec spec;
        spec.retail = round % 4 != 0;
        spec.key = random_key();
        spec.counter = random_key();
        if (round % 3 == 0) std::fill(spec.counter.begin() + 8, spec.counter.end(), u8{0xFF}); // carries past 64 bits
        spec.qa_digest = random_key();
        const int count = 1 + static_cast<int>(rng() % 12);
        for (int i = 0; i < count; i++) {
            const u32 types[] = {kPkgEntryNpdrm, kPkgEntryNpdrmEdat, kPkgEntryRegular, 5, 6, 0xE, 0x19, kPkgEntryFolder};
            const u32 type = types[rng() % std::size(types)];
            const std::string name = std::format("d{}/f{}{}", i % 3, i, std::string(rng() % 40, 'x'));
            if (type == kPkgEntryFolder) spec.items.push_back(folder(std::format("d{}", i % 3)));
            else spec.items.push_back(file(name, pattern(static_cast<usize>(rng() % 3000), static_cast<u32>(rng())), type));
        }
        const std::vector<u8> pkg = build_pkg(spec);
        INFO("round " << round);

        // RPCS3 finds the table sound with the package's key and not with another (a debug package has none).
        CHECK(test::rpcs3_ref::pkg_table_sound(pkg, spec.key));
        if (spec.retail) CHECK_FALSE(test::rpcs3_ref::pkg_table_sound(pkg, random_key()));
        // Its decrypt gives back the table, every name and every file.
        const auto table = test::rpcs3_ref::pkg_decrypt(pkg, 0, spec.items.size() * 0x20, spec.key);
        REQUIRE(table);
        for (usize i = 0; i < spec.items.size(); i++) {
            const PkgItem& item = spec.items[i];
            const u8* e = table->data() + i * 0x20;
            CHECK(ps_ld32(e + 0x18) == item.type);
            REQUIRE(ps_ld32(e + 4) == item.name.size());
            REQUIRE(ps_ld64(e + 0x10) == item.bytes.size());
            const auto name = test::rpcs3_ref::pkg_decrypt(pkg, ps_ld32(e), item.name.size(), spec.key);
            REQUIRE(name);
            CHECK(std::string(name->begin(), name->end()) == item.name);
            if (!item.bytes.empty()) {
                const auto bytes = test::rpcs3_ref::pkg_decrypt(pkg, ps_ld64(e + 8), item.bytes.size(), spec.key);
                REQUIRE(bytes);
                CHECK(*bytes == item.bytes);
            }
        }
        // pspocket installs those bytes (the last entry for a file, none marked to overwrite: the first).
        PkgKeys keys;
        keys.ps3 = random_key();
        keys.idu = spec.key; // the second key, as an IDU package's
        keys.present = true;
        write_file(root / "p.pkg", pkg);
        const Install in = install(root / "p.pkg", root / "hdd0", keys);
        INFO(message(in.result));
        REQUIRE(in.result);
        std::map<std::string, std::vector<u8>> first;
        for (const PkgItem& item : spec.items)
            if (item.type != kPkgEntryFolder) first.try_emplace(item.name, item.bytes);
        for (const auto& [name, bytes] : first) CHECK(read_file(root / "hdd0" / "game" / "NPXX00000" / name) == bytes);
        CHECK(has_line(in.lines, "IDU") == spec.retail);
        fs::remove_all(root / "hdd0");
    }
    fs::remove_all(root);
}

TEST_CASE("PKG: names refused where RPCS3's leaves_directory refuses them", "[recomp][pkg][rpcs3]")
{
    const PkgKeys keys = made_up_pkg_keys();
    const fs::path root = temp_dir("rpcs3-names");
    for (const char* name : {"a", "a/b", "../a", "a/../b", "./a", "a/.", "/a", "a//b", "...", "..a", "a..", ".a",
             "a/.b/c", "a/..", "a/./b", "a/b/", ".", "..", "//"}) {
        INFO(name);
        PkgSpec spec;
        spec.key = keys.ps3;
        spec.items = {folder(name)};
        write_file(root / "p.pkg", build_pkg(spec));
        const Install in = install(root / "p.pkg", root / "hdd0", keys);
        CHECK(!in.result == test::rpcs3_ref::pkg_leaves_directory(name));
        fs::remove_all(root / "hdd0");
    }
    fs::remove_all(root);
}

#endif
