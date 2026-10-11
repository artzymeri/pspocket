// The small sysutil libraries (synthetic: no game data): user info on a temporary /dev_hdd0, screenshots, storage
// data, system settings, the web browser's setup, AV chat, the licence area, the NP EULA, the access point, remote
// play, video upload, printing, the auth dialog, the cross controller and cellGameExec (D-183); the access point mode,
// background downloads, the auth dialog's Abort and Close, cellGameExec's games and the RTC alarms (D-671 to D-673).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "hle_test_util.hpp"
#include "psf.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;

namespace {

struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest callback that appends the low words of r3..r6 to a log (kLog: count, then 16 bytes an entry).
constexpr u32 kCode = kData + 0x20000, kOpd = kData + 0x20800, kLog = kData + 0x21000;

void write_log_callback(u8* base)
{
    const std::vector<u32> code = {
        as::lis(7, static_cast<s32>(kLog >> 16)), as::ori(7, 7, kLog & 0xFFFF), as::lwz(8, 0, 7), as::slwi(9, 8, 4),
        as::addi(9, 9, 16), as::stwx(3, 7, 9), as::addi(10, 9, 4), as::stwx(4, 7, 10), as::addi(10, 9, 8),
        as::stwx(5, 7, 10), as::addi(10, 9, 12), as::stwx(6, 7, 10), as::addi(8, 8, 1), as::stw(8, 0, 7), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, 0x1234);
    std::memset(base + kLog, 0, 0x400);
}

struct Entry {
    u32 r3, r4, r5, r6;
};

std::vector<Entry> logged(u8* base)
{
    std::vector<Entry> v;
    for (u32 i = 0; i < ps_read32(base, kLog); i++) {
        const u32 at = kLog + 16 + 16 * i;
        v.push_back({ps_read32(base, at), ps_read32(base, at + 4), ps_read32(base, at + 8), ps_read32(base, at + 12)});
    }
    return v;
}

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

} // namespace

TEST_CASE("cellUserInfo: the current user made as RPCS3 makes it, the list, the stat, selection (D-183)",
    "[hle][sysutil][userinfo]")
{
    InterpretMisses interpret;
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-users-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root);
    Fixture f;
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    constexpr u32 kStat = kData + 0x100, kNum = kData + 0x200, kList = kData + 0x204, kCurrent = kData + 0x250,
                  kTypeSet = kData + 0x300, kSetList = kData + 0x320, kFixed = kData + 0x340;
    const u64 kParam = r3_of_code(0x8002c303), kNoUser = r3_of_code(0x8002c304);

    // A second user, besides the current one made at the first call.
    fsys::create_directories(root / "home" / "00000002");
    std::ofstream(root / "home" / "00000002" / "localusername") << "Second";
    fsys::create_directories(root / "home" / "notauser");

    REQUIRE(a.call("cellUserInfoGetStat", 0, kStat) == 0);
    CHECK(ps_read32(base, kStat) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(base + kStat + 4)) == "User");
    CHECK(fsys::exists(root / "home" / "00000001" / "localusername"));
    REQUIRE(a.call("cellUserInfoGetStat", 2, kStat) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(base + kStat + 4)) == "Second");
    CHECK(a.call("cellUserInfoGetStat", 3, kStat) == kNoUser);
    CHECK(a.call("cellUserInfoGetStat", 100000000, kStat) == kParam);

    std::memset(base + kList, 0xAA, 64);
    REQUIRE(a.call("cellUserInfoGetList", kNum, kList, kCurrent) == 0);
    CHECK(ps_read32(base, kNum) == 2);
    CHECK(ps_read32(base, kList) == 1);
    CHECK(ps_read32(base, kList + 4) == 2);
    CHECK(ps_read32(base, kList + 8) == 0);
    CHECK(ps_read32(base, kCurrent) == 1);
    CHECK(a.call("cellUserInfoGetList", 0, kList, kCurrent) == kParam);
    CHECK(a.call("cellUserInfoGetList", 0, 0, kCurrent) == 0);

    // Selection: no list UI, so the current user, at the next check; an empty fixed list is NOUSER.
    std::memset(base + kTypeSet, 0, 16);
    REQUIRE(a.call("cellUserInfoSelectUser_ListType", kTypeSet, kOpd, 0, 0x77) == 0);
    REQUIRE(a.call("cellSysutilCheckCallback") == 0);
    std::vector<Entry> e = logged(base);
    REQUIRE(e.size() == 1);
    CHECK(e[0].r3 == 0); // CELL_USERINFO_RET_OK
    CHECK(ps_read32(base, e[0].r4) == 1);
    CHECK(e[0].r5 == 0x77);
    std::memset(base + kSetList, 0, 20);
    std::memset(base + kFixed, 0, 64);
    ps_write32(base, kSetList + 8, 1);
    ps_write32(base, kSetList + 12, kFixed);
    REQUIRE(a.call("cellUserInfoSelectUser_SetList", kSetList, kOpd, 0, 0x78) == 0);
    REQUIRE(a.call("cellSysutilCheckCallback") == 0);
    e = logged(base);
    REQUIRE(e.size() == 2);
    CHECK(e[1].r3 == 0x8002c304); // NOUSER to funcSelect
    CHECK(a.call("cellUserInfoSelectUser_ListType", 0, kOpd, 0, 0) == kParam);
    a.call("cellUserInfoEnableOverlay", 1);
    fsys::remove_all(root);
}

TEST_CASE("cellScreenShot, storage, sysconf, web browser, licence area, access point, remote play (D-183)",
    "[hle][sysutil]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    constexpr u32 kParam = kData + 0x100, kStr = kData + 0x200, kOut = kData + 0x400;
    const u64 kShotParam = r3_of_code(0x8002d102);

    // Screenshots: the titles must end within their sizes; the overlay directory must be a PS3 device.
    std::memset(base + kParam, 0, 16);
    put(base, kStr, "Photo");
    ps_write32(base, kParam, kStr);
    CHECK(a.call("cellScreenShotSetParameter", kParam) == 0);
    std::memset(base + kStr, 'x', 100);
    CHECK(a.call("cellScreenShotSetParameter", kParam) == kShotParam);
    CHECK(a.call("cellScreenShotSetParameter", 0) == kShotParam);
    put(base, kStr, "/dev_hdd0/game");
    put(base, kStr + 0x80, "OVERLAY.PNG");
    CHECK(a.call("cellScreenShotSetOverlayImage", kStr, kStr + 0x80, 10, 20) == 0);
    put(base, kStr, "/etc");
    CHECK(a.call("cellScreenShotSetOverlayImage", kStr, kStr + 0x80, 10, 20) == kShotParam);
    CHECK(a.call("cellScreenShotEnable") == 0);
    CHECK(a.call("cellScreenShotDisable") == 0);

    // Storage data and the settings screen: funcFinish/func hears CELL_OK at the next check.
    CHECK(a.call("cellStorageDataImport", 0, kStr, kStr, 0, kOpd, 0, 0x51) == 0);
    CHECK(a.call("cellSysconfOpen", 0, kOpd, 0x52, 0, 0) == 0);
    check();
    std::vector<Entry> e = logged(base);
    REQUIRE(e.size() == 2);
    CHECK(e[0].r3 == 0);
    CHECK(e[0].r4 == 0x51);
    CHECK(e[1].r4 == 0x52);
    CHECK(a.call("cellSysconfBtGetDeviceList", 0) == r3_of_code(0x8002bb01));

    // The web browser is the system software's: libsysutil's requests to it answer as its closed channels do (D-534),
    // and no system callback comes.
    CHECK(a.call("cellWebBrowserEstimate2", 0, kOut) == r3_of_code(0x8002b902));
    CHECK(a.call("cellWebBrowserInitialize", kOpd, 0) == r3_of_code(0x8002b007));
    CHECK(a.call("cellWebBrowserShutdown") == r3_of_code(0x8002b007));
    check();
    CHECK(logged(base).size() == 2);

    CHECK(a.call("cellSysutilGetLicenseArea") == 1); // A, RPCS3's default
    CHECK(a.call("cellSysutilApGetRequiredMemSize") == 1024 * 1024);
    REQUIRE(a.call("cellRemotePlayGetComparativeVolume", kOut) == 0);
    CHECK(std::bit_cast<f32>(ps_read32(base, kOut)) == 1.0f);
    CHECK(a.call("cellAuthDialogOpen", 0) == r3_of_code(0x8002d202));
    CHECK(a.call("cellAuthDialogOpen", 1) == 0);
}

TEST_CASE("cellSysutilAvc: load, join, bye and unload succeed at the next check; the checks (D-183)", "[hle][sysutil]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    const u64 kInvalid = r3_of_code(0x8002b705), kAlready = r3_of_code(0x8002b704);
    constexpr u32 kReq = kData + 0x100, kRoom = kData + 0x200;

    CHECK(a.call("cellSysutilAvcLoadAsync", kOpd, 0x99, 0, 3, 1, 1, kReq) == kInvalid); // media
    CHECK(a.call("cellSysutilAvcLoadAsync", kOpd, 0x99, 0, 1, 2, 1, kReq) == kInvalid); // video quality
    REQUIRE(a.call("cellSysutilAvcLoadAsync", kOpd, 0x99, 0, 1, 1, 1, kReq) == 0);
    CHECK(a.call("cellSysutilAvcLoadAsync", kOpd, 0x99, 0, 1, 1, 1, kReq) == kAlready);
    REQUIRE(a.call("cellSysutilAvcJoinRequest", 0, kRoom, kReq) == 0);
    REQUIRE(a.call("cellSysutilAvcByeRequest", kReq) == 0);
    REQUIRE(a.call("cellSysutilAvcUnloadAsync", kReq) == 0);
    REQUIRE(a.call("cellSysutilCheckCallback") == 0);
    const std::vector<Entry> e = logged(base);
    REQUIRE(e.size() == 4);
    CHECK(e[0].r4 == 1); // LOAD_SUCCEEDED
    CHECK(e[1].r4 == 5); // JOIN_SUCCEEDED
    CHECK(e[2].r4 == 7); // BYE_SUCCEEDED
    CHECK(e[3].r4 == 3); // UNLOAD_SUCCEEDED
    CHECK(e[3].r3 == 3); // request IDs count up
    CHECK(e[0].r6 == 0x99);
    REQUIRE(a.call("cellSysutilAvcLoadAsync", kOpd, 0x99, 0, 2, 1, 1, kReq) == 0); // unloaded: loads again

    CHECK(a.call("cellSysutilAvcJoinRequest", 0, 0, kReq) == kInvalid);
    CHECK(a.call("cellSysutilAvcGetShowStatus", 0) == kInvalid);
    CHECK(a.call("cellSysutilAvcGetShowStatus", kReq) == 0);
    CHECK(a.call("cellSysutilAvcSetLayoutMode", 4) == kInvalid);
    CHECK(a.call("cellSysutilAvcSetSpeakerVolumeLevel", 11) == kInvalid);
    CHECK(a.call("cellSysutilAvcSetSpeakerVolumeLevel", 10) == 0);
    ps_write32(base, kReq, 5);
    REQUIRE(a.call("cellSysutilAvcEnumPlayers", 0, kReq) == 0);
    CHECK(ps_read32(base, kReq) == 0);
}

TEST_CASE("NP EULA, video upload, print, cross controller, cellGameExec (D-183)", "[hle][sysutil]")
{
    InterpretMisses interpret;
    hle::ProcessConfig config;
    config.game.title_id = "NPEB00001";
    config.game.boot = hle::GameInfo::Boot::Hdd;
    Fixture f(config);
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    constexpr u32 kComm = kData + 0x100, kType = kData + 0x200, kDir = kData + 0x210, kExec = kData + 0x240,
                  kParam = kData + 0x300, kPkg = kData + 0x320, kStr = kData + 0x400;

    // EULA: accepted already (2), one check at a time; an abort turns it into ABORTED (4).
    CHECK(a.call("sceNpEulaCheckEulaStatus", 0, 0, 0, kOpd, 0) == r3_of_code(0x8002e501));
    CHECK(a.call("sceNpEulaAbort") == r3_of_code(0x8002e502));
    REQUIRE(a.call("sceNpEulaCheckEulaStatus", kComm, 0, 0, kOpd, 0x31) == 0);
    CHECK(a.call("sceNpEulaCheckEulaStatus", kComm, 0, 0, kOpd, 0x31) == r3_of_code(0x8002e503));
    check();
    REQUIRE(a.call("sceNpEulaCheckEulaStatus", kComm, 0, 0, kOpd, 0x32) == 0);
    REQUIRE(a.call("sceNpEulaAbort") == 0);
    check();
    std::vector<Entry> e = logged(base);
    REQUIRE(e.size() == 2);
    CHECK(e[0].r3 == 2);
    CHECK(e[0].r6 == 0x31);
    CHECK(e[1].r3 == 4);

    // Video upload: INITIALIZED then FINALIZED. Print: each step CELL_OK.
    REQUIRE(a.call("cellVideoUploadInitialize", 0, kOpd, 0x41) == 0);
    REQUIRE(a.call("cellPrintLoadAsync2", kOpd, 0x42, 0) == 0);
    check();
    e = logged(base);
    REQUIRE(e.size() == 5);
    CHECK(e[2].r3 == 1);
    CHECK(e[3].r3 == 2);
    CHECK(e[3].r6 == 0x41);
    CHECK(e[4].r4 == 0x42);

    // The cross controller: INITIALIZED, then the user cancels waiting for a PS Vita.
    std::memset(base + kParam, 0, 16);
    CHECK(a.call("cellCrossControllerInitialize", kParam, kPkg, kOpd, 0) == r3_of_code(0x8002cd92));
    put(base, kStr, "x");
    for (u32 i = 0; i < 3; i++) ps_write32(base, kParam + 4 * i, kStr);
    for (u32 i = 0; i < 3; i++) ps_write32(base, kPkg + 4 * i, kStr);
    REQUIRE(a.call("cellCrossControllerInitialize", kParam, kPkg, kOpd, 0x43) == 0);
    CHECK(a.call("cellCrossControllerInitialize", kParam, kPkg, kOpd, 0x43) == r3_of_code(0x8002cd99));
    check();
    e = logged(base);
    REQUIRE(e.size() == 7);
    CHECK(e[5].r3 == 1);
    CHECK(e[6].r3 == 2);
    CHECK(e[6].r4 == 0x8002cd80);

    // cellGameExec: an HDD game's directory, the exit parameter, and no PlayStation Home.
    REQUIRE(a.call("cellGameSetExitParam", 0xE7) == 0);
    REQUIRE(a.call("cellGameGetBootGameInfo", kType, kDir, kExec) == 0);
    CHECK(ps_read32(base, kType) == 2);
    CHECK(std::string(reinterpret_cast<const char*>(base + kDir)) == "NPEB00001");
    CHECK(ps_read32(base, kExec) == 0xE7);
    CHECK(a.call("cellGameGetBootGameInfo", 0, kDir, kExec) == r3_of_code(0x8002cb07));
    CHECK(a.call("cellGameGetHomeDataExportPath", kStr) == r3_of_code(0x8002cb08));
    CHECK(a.call("cellGameGetHomePath", kStr) == 0);
    CHECK(a.call("cellGameGetHomeLaunchOptionPath", kStr, 0) == r3_of_code(0x8002cb07));
}

// ---- pspocket's own where RPCS3 has stubs (D-671 to D-673)

namespace {

constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

// <dir>/PARAM.SFO of `category` for `title_id`.
void write_sfo(const fsys::path& dir, std::string_view category, std::string_view title_id)
{
    fsys::create_directories(dir);
    hle::psf::Registry sfo;
    sfo["CATEGORY"] = hle::psf::string(4, category);
    sfo["TITLE_ID"] = hle::psf::string(16, title_id);
    sfo["TITLE"] = hle::psf::string(128, "Synthetic");
    const std::vector<u8> bytes = hle::psf::save(sfo);
    std::ofstream(dir / "PARAM.SFO", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void check_unverified(std::string_view module, std::initializer_list<u32> nids)
{
    for (const u32 nid : nids) {
        INFO(module << " " << std::hex << nid);
        CHECK(hle::registry().is_unverified(module, nid));
    }
}

} // namespace

TEST_CASE("cellSysutilAp: the access point mode never comes on; off is off (D-671)", "[hle][sysutil][sysutil-ap]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    REQUIRE_FALSE(f.mem.is_committed(kData + 0x300000));
    constexpr u32 kParam = kData + 0x100;
    const u64 kInvalidValue = r3_of_code(0x8002cd02), kNoCable = r3_of_code(0x8002cd15);

    // CellSysutilApParam: type, wlanFlag, titleId[9]+3, ssid[33]+3, wpakey[65]+3 (124 bytes).
    std::memset(base + kParam, 0, 124);
    ps_write32(base, kParam, 1);
    ps_write32(base, kParam + 4, 1);
    put(base, kParam + 8, "NPEB00001");
    put(base, kParam + 20, "pspocket");
    put(base, kParam + 56, "secret-key");
    std::vector<u8> before(base + kParam, base + kParam + 124);

    CHECK(a.call("cellSysutilApOn", 0, 0) == kInvalidValue);
    CHECK(a.call("cellSysutilApOn", kUnmapped, 0) == kInvalidValue);
    CHECK(a.call("cellSysutilApOn", kData + 0x300000 - 64, 0) == kInvalidValue); // runs out of guest memory
    CHECK(a.call("cellSysutilApOn", kParam, 0x12345678) == kNoCable);
    CHECK(a.call("cellSysutilApOn", kParam, 0xFFFFFFFF) == kNoCable); // the same each time
    CHECK(std::equal(before.begin(), before.end(), base + kParam));
    CHECK(a.call("cellSysutilApOff") == 0);
    CHECK(a.call("cellSysutilApOff") == 0);
    CHECK(a.call("cellSysutilApGetRequiredMemSize") == 1024 * 1024);
    check_unverified("cellSysutilAp", {0x3343824C, 0x90C2BB19});
}

TEST_CASE("cellAuthDialog: Abort and Close of the dialog cellAuthDialogOpen opened (D-672)",
    "[hle][sysutil][authdialog]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    const u64 kNoDialog = r3_of_code(0x8002d203);

    CHECK(a.call("cellAuthDialogAbort") == kNoDialog);
    CHECK(a.call("cellAuthDialogClose") == kNoDialog);
    CHECK(a.call("cellAuthDialogOpen", 0) == r3_of_code(0x8002d202)); // refused: no dialog
    CHECK(a.call("cellAuthDialogAbort") == kNoDialog);

    // Abort leaves the dialog to Close; Close ends it.
    REQUIRE(a.call("cellAuthDialogOpen", 1) == 0);
    CHECK(a.call("cellAuthDialogAbort") == 0);
    CHECK(a.call("cellAuthDialogAbort") == 0);
    CHECK(a.call("cellAuthDialogClose", 0) == 0);
    CHECK(a.call("cellAuthDialogClose", 0) == kNoDialog);
    CHECK(a.call("cellAuthDialogAbort") == kNoDialog);

    // Close without an abort; a second Open while one is open is still one dialog (RPCS3's CELL_OK).
    REQUIRE(a.call("cellAuthDialogOpen", 0x100000000ull) == 0);
    CHECK(a.call("cellAuthDialogClose", 0) == 0);
    REQUIRE(a.call("cellAuthDialogOpen", 2) == 0);
    REQUIRE(a.call("cellAuthDialogOpen", 3) == 0);
    CHECK(a.call("cellAuthDialogClose", 0) == 0);
    CHECK(a.call("cellAuthDialogClose", 0) == kNoDialog);
    check_unverified("cellAuthDialogUtility", {0xEB27885B, 0x975D4824});
}

TEST_CASE("cellGameExec: no other game booted, listed or deleted; no exit information (D-672)",
    "[hle][sysutil][gameexec]")
{
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-gameexec-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    // This game (an HDD game), another installed one, and game data that is no game.
    write_sfo(root / "hdd0" / "game" / "NPEB00001", "HG", "NPEB00001");
    write_sfo(root / "hdd0" / "game" / "NPEB00002", "HG", "NPEB00002");
    write_sfo(root / "hdd0" / "game" / "SAVEDATA1", "GD", "NPEB00001");
    write_sfo(root / "bdvd" / "PS3_GAME", "DG", "BLES00001");

    hle::ProcessConfig config;
    config.game.title_id = "NPEB00001";
    config.game.boot = hle::GameInfo::Boot::Hdd;
    Fixture f(config);
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    constexpr u32 kName = kData + 0x100, kNum = kData + 0x200, kGot = kData + 0x204, kBuf = kData + 0x300,
                  kStatus = kData + 0x800, kType = kData + 0x804, kDir = kData + 0x810, kExec = kData + 0x830,
                  kUser = kData + 0x834;
    const u64 kNotFound = r3_of_code(0x8002cb04), kParam = r3_of_code(0x8002cb07), kNoApp = r3_of_code(0x8002cb08),
              kNotSupported = r3_of_code(0x8002cb21);
    const auto name = [&](std::string_view s) {
        put(base, kName, s);
        return kName;
    };

    // The list: no game, both counts 0, the buffer untouched; the counts' pointers are checked.
    std::memset(base + kBuf, 0xA5, 0x400);
    ps_write32(base, kNum, 0xA5A5A5A5);
    ps_write32(base, kGot, 0xA5A5A5A5);
    REQUIRE(a.call("cellGameGetList", 16, kBuf, kNum, kGot, 0) == 0);
    CHECK(ps_read32(base, kNum) == 0);
    CHECK(ps_read32(base, kGot) == 0);
    CHECK(all_bytes(base + kBuf, 0x400, 0xA5));
    CHECK(a.call("cellGameGetList", 16, kBuf, 0, kGot, 0) == kParam);
    CHECK(a.call("cellGameGetList", 16, kBuf, kNum, 0, 0) == kParam);
    CHECK(a.call("cellGameGetList", 16, kBuf, kUnmapped, kGot, 0) == kParam);

    // Deleting: an installed game stays (another's, or this one), what is no game or not there is NOTFOUND, and a
    // name that is no directory name is PARAM.
    CHECK(a.call("cellGameDeleteGame", name("NPEB00002"), 0) == kNotSupported);
    CHECK(a.call("cellGameDeleteGame", name("NPEB00001"), 0) == kNotSupported);
    CHECK(fsys::exists(root / "hdd0" / "game" / "NPEB00002" / "PARAM.SFO"));
    CHECK(fsys::exists(root / "hdd0" / "game" / "NPEB00001" / "PARAM.SFO"));
    CHECK(a.call("cellGameDeleteGame", name("SAVEDATA1"), 0) == kNotFound);
    CHECK(a.call("cellGameDeleteGame", name("NOSUCHGAME"), 0) == kNotFound);
    CHECK(a.call("cellGameDeleteGame", 0, 0) == kParam);
    CHECK(a.call("cellGameDeleteGame", kUnmapped, 0) == kParam);
    for (const char* bad : {"", "../game", "_NPEB0000", "npeb00002", "NPEB/0002"}) {
        INFO(bad);
        CHECK(a.call("cellGameDeleteGame", name(bad), 0) == kParam);
    }
    std::memset(base + kName, 'A', 32); // no NUL within CELL_GAME_DIRNAME_SIZE
    CHECK(a.call("cellGameDeleteGame", kName, 0) == kParam);

    // Booting another game: HDD and DISC; Home is gone; other types are PARAM.
    CHECK(a.call("cellGameExecGame", 2, name("NPEB00002"), 0, 0, 0x11, 0x22) == kNotSupported);
    CHECK(a.call("cellGameExecGame", 2, name("NPEB00001"), 0, 0, 0, 0) == kNotSupported); // this game
    CHECK(a.call("cellGameExecGame", 2, name("NOSUCHGAME"), 0, 0, 0, 0) == kNotFound);
    CHECK(a.call("cellGameExecGame", 2, name("SAVEDATA1"), 0, 0, 0, 0) == kNotFound);
    CHECK(a.call("cellGameExecGame", 2, 0, 0, 0, 0, 0) == kParam);
    CHECK(a.call("cellGameExecGame", 2, name("../x"), 0, 0, 0, 0) == kParam);
    CHECK(a.call("cellGameExecGame", 1, 0, 0, 0, 0, 0) == kNotFound); // an HDD game, no disc in the drive
    REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (root / "bdvd").string(), true));
    CHECK(a.call("cellGameExecGame", 1, 0, 0, 0, 0, 0) == kNotSupported);
    CHECK(a.call("cellGameExecGame", 4, 0, 0, 0, 0, 0) == kNoApp);
    for (const u32 type : {0u, 3u, 5u, 0xFFFFFFFFu}) {
        INFO("type " << type);
        CHECK(a.call("cellGameExecGame", type, name("NPEB00002"), 0, 0, 0, 0) == kParam);
    }
    CHECK(fsys::exists(root / "hdd0" / "game" / "NPEB00002" / "PARAM.SFO"));

    // The exit information: none (no game was booted from this one), nothing written.
    std::memset(base + kStatus, 0xA5, 0x40);
    REQUIRE(a.call("cellGameGetExitGameInfo", kStatus, kType, kDir, kExec, kUser) == 0);
    CHECK(all_bytes(base + kStatus, 0x40, 0xA5));
    CHECK(a.call("cellGameGetExitGameInfo", kStatus, kType, kDir, 0, 0) == 0);
    CHECK(a.call("cellGameGetExitGameInfo", 0, kType, kDir, kExec, kUser) == kParam);
    CHECK(a.call("cellGameGetExitGameInfo", kStatus, 0, kDir, kExec, kUser) == kParam);
    CHECK(a.call("cellGameGetExitGameInfo", kStatus, kType, 0, kExec, kUser) == kParam);
    CHECK(a.call("cellGameGetExitGameInfo", kStatus, kType, kUnmapped, kExec, kUser) == kParam);
    CHECK(all_bytes(base + kStatus, 0x40, 0xA5));

    check_unverified("cellGameExec", {0x93D4AE69, 0x34B74160, 0xCA872C41, 0x7767C4E3});
    fsys::remove_all(root);
}

TEST_CASE("cellGameExec: a disc game's disc is in the drive (D-672)", "[hle][sysutil][gameexec]")
{
    Fixture f; // a disc boot, no /dev_hdd0
    GuestThread a(f, 0, "a");
    CHECK(a.call("cellGameExecGame", 1, 0, 0, 0, 0, 0) == r3_of_code(0x8002cb21));
    put(f.base(), kData + 0x100, "BLES00001");
    CHECK(a.call("cellGameExecGame", 2, kData + 0x100, 0, 0, 0, 0) == r3_of_code(0x8002cb04));
    CHECK(a.call("cellGameDeleteGame", kData + 0x100, 0) == r3_of_code(0x8002cb04));
}

TEST_CASE("cellBGDL: no background download (no PSN); the mode kept (D-671)", "[hle][sysutil][bgdl]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    REQUIRE_FALSE(f.mem.is_committed(kData + 0x300000));
    constexpr u32 kId = kData + 0x100, kInfo = kData + 0x200, kMode = kData + 0x400;
    const u64 kParam = r3_of_code(0x8002ce03);
    put(base, kId, "EP0001-NPEB00001_00-0000000000000001");

    // The mode: AUTO (0) at first; ALWAYS_ALLOW (1) or AUTO kept; nothing else.
    ps_write32(base, kMode, 0xA5A5A5A5);
    REQUIRE(a.call("cellBGDLGetMode", kMode) == 0);
    CHECK(ps_read32(base, kMode) == 0);
    REQUIRE(a.call("cellBGDLSetMode", 1) == 0);
    REQUIRE(a.call("cellBGDLGetMode", kMode) == 0);
    CHECK(ps_read32(base, kMode) == 1);
    CHECK(a.call("cellBGDLSetMode", 2) == kParam);
    CHECK(a.call("cellBGDLSetMode", 0xFFFFFFFF) == kParam);
    REQUIRE(a.call("cellBGDLGetMode", kMode) == 0);
    CHECK(ps_read32(base, kMode) == 1);
    REQUIRE(a.call("cellBGDLSetMode", 0) == 0);
    REQUIRE(a.call("cellBGDLGetMode", kMode) == 0);
    CHECK(ps_read32(base, kMode) == 0);
    CHECK(a.call("cellBGDLGetMode", 0) == kParam);
    CHECK(a.call("cellBGDLGetMode", kUnmapped) == kParam);

    // No download: 0 entries, nothing written; the buffer for `num` entries (24 bytes each) is checked.
    for (const char* fn : {"cellBGDLGetInfo", "cellBGDLGetInfo2"}) {
        INFO(fn);
        std::memset(base + kInfo, 0xA5, 0x100);
        CHECK(a.call(fn, kId, kInfo, 4) == 0);
        CHECK(a.call(fn, 0, kInfo, 4) == 0); // no ID: none either
        CHECK(a.call(fn, kId, 0, 0) == 0);
        CHECK(all_bytes(base + kInfo, 0x100, 0xA5));
        CHECK(a.call(fn, kId, 0, 1) == kParam);
        CHECK(a.call(fn, kId, kInfo, -1) == kParam);
        CHECK(a.call(fn, kId, kUnmapped, 1) == kParam);
        CHECK(a.call(fn, kUnmapped, kInfo, 1) == kParam);
        CHECK(a.call(fn, kId, kData + 0x300000 - 24, 1) == 0);
        CHECK(a.call(fn, kId, kData + 0x300000 - 24, 2) == kParam); // the second entry is not guest memory
        CHECK(a.call(fn, kId, kInfo, 0x7FFFFFFF) == kParam);
    }
    check_unverified("cellBGDLUtility", {0x4E9BB95B, 0x2AB0D183, 0x7E134A90, 0x74E57BDF});
}

TEST_CASE("cellRtcAlarm: registrations kept, no alarm rings, no argument read or written (D-673)",
    "[hle][sysutil][rtcalarm]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kBuf = kData + 0x100;
    std::memset(base + kBuf, 0xA5, 0x100);

    CHECK(a.call("cellRtcAlarmRegister", kBuf, kBuf + 0x10, kBuf + 0x20, kBuf + 0x30) == 0);
    CHECK(a.call("cellRtcAlarmRegister", kBuf, kBuf + 0x10, kBuf + 0x20, kBuf + 0x30) == 0); // again: replaced
    CHECK(a.call("cellRtcAlarmGetStatus", kBuf, kBuf + 0x40) == 0);
    CHECK(a.call("cellRtcAlarmNotification", kBuf, kBuf + 0x40) == 0);
    CHECK(a.call("cellRtcAlarmStopRunning", kBuf) == 0);
    CHECK(a.call("cellRtcAlarmUnregister", kBuf) == 0);
    CHECK(a.call("cellRtcAlarmUnregister", kBuf) == 0); // none kept under it: still CELL_OK
    // Arguments that are no guest memory are not dereferenced either.
    CHECK(a.call("cellRtcAlarmRegister", kUnmapped, 0xFFFFFFFF, 0, 0) == 0);
    CHECK(a.call("cellRtcAlarmGetStatus", kUnmapped, 0xFFFFFFFF) == 0);
    CHECK(a.call("cellRtcAlarmNotification", 0xFFFFFFFF) == 0);
    CHECK(a.call("cellRtcAlarmStopRunning", 0) == 0);
    CHECK(a.call("cellRtcAlarmUnregister", kUnmapped) == 0);
    // Past the 256 kept, each is answered as the others.
    for (u32 i = 0; i < 300; i++) CHECK(a.call("cellRtcAlarmRegister", i, 0, 0, 0) == 0);
    for (u32 i = 0; i < 300; i++) CHECK(a.call("cellRtcAlarmUnregister", i) == 0);
    CHECK(all_bytes(base + kBuf, 0x100, 0xA5));
    check_unverified("cellRtcAlarm", {0x122D3DB1, 0x559234A3, 0xAD8D9839, 0xB287748C, 0xDB215455});
}
