// The small sysutil libraries (synthetic: no game data): user info on a temporary /dev_hdd0, screenshots, storage
// data, system settings, the web browser's setup, AV chat, the licence area, the NP EULA, the access point, remote
// play, video upload, printing, the auth dialog, the cross controller and cellGameExec (D-183).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hle_test_util.hpp"
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

    // The web browser: 1 MB, INITIALIZING_FINISHED then SHUTDOWN_FINISHED to its system callback.
    REQUIRE(a.call("cellWebBrowserEstimate2", 0, kOut) == 0);
    CHECK(ps_read32(base, kOut) == 1024 * 1024);
    CHECK(a.call("cellWebBrowserInitialize", kOpd, 0) == 0);
    a.call("cellWebBrowserShutdown");
    check();
    e = logged(base);
    REQUIRE(e.size() == 4);
    CHECK(e[2].r3 == 1);
    CHECK(e[3].r3 == 4);

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
