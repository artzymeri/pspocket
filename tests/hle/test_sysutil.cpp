// cellSysutil and cellSysutilAvconfExt (synthetic: no game data): system events delivered to a guest callback by
// cellSysutilCheckCallback, system parameters, the system cache on host directories, video and audio output.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "sysutil.hpp" // runtime/hle/src (internal): the event queue other HLE modules use
#include "hle_test_util.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;

namespace {

const u64 kValue = r3_of_code(0x8002b102); // CELL_SYSUTIL_ERROR_VALUE
const u64 kSize = r3_of_code(0x8002b103);  // CELL_SYSUTIL_ERROR_SIZE
constexpr s32 kBusy = static_cast<s32>(0x8002b105u); // CELL_SYSUTIL_ERROR_BUSY
const u64 kCacheAccess = r3_of_code(0x8002bc01);
const u64 kCacheParam = r3_of_code(0x8002bc03);
const u64 kCacheNotMounted = r3_of_code(0x8002bc04);
const u64 kVideoIllegalConfiguration = r3_of_code(0x8002b221);
const u64 kVideoIllegal = r3_of_code(0x8002b222);
const u64 kVideoOutOfRange = r3_of_code(0x8002b223);
const u64 kVideoNoDevice = r3_of_code(0x8002b224);
const u64 kVideoUnsupportedOut = r3_of_code(0x8002b225);
const u64 kVideoUnsupportedMode = r3_of_code(0x8002b226);
const u64 kAudioIllegal = r3_of_code(0x8002b242);
const u64 kAudioOutOfRange = r3_of_code(0x8002b243);
const u64 kAudioUnsupportedOut = r3_of_code(0x8002b245);

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest callback(status, param, userdata) behind the descriptor kOpd {kCode, kToc}. It records its arguments and
// r2 at kOut (+0 status, +8 param, +16 userdata, +20 r2) and counts its calls (+24), then returns 7.
constexpr u32 kCode = kData + 0x10000, kOpd = kData + 0x10800, kOut = kData + 0x11000, kToc = 0x00123400;

void write_callback(u8* base)
{
    const std::vector<u32> code = {
        as::lis(6, static_cast<s32>(kOut >> 16)),
        as::ori(6, 6, kOut & 0xFFFF),
        as::std_(3, 0, 6),
        as::std_(4, 8, 6),
        as::stw(5, 16, 6),
        as::stw(2, 20, 6),
        as::lwz(7, 24, 6),
        as::addi(7, 7, 1),
        as::stw(7, 24, 6),
        as::li(3, 7),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, kToc);
}

std::string guest_string(u8* base, u32 addr) { return std::string(reinterpret_cast<const char*>(base + addr)); }

// CellSysCacheParam {char cacheId[32]; char getCachePath[1055]; be32 reserved} for the cache tests.
constexpr u32 kSysCacheParam = kData + 0x1000;

} // namespace

TEST_CASE("cellSysutil, cellSysutilAvconfExt: the functions GTA V imports, in their modules", "[hle][sysutil]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"cellSysutil", "cellSysutilRegisterCallback", 0x9D98AFA0},
        {"cellSysutil", "cellSysutilUnregisterCallback", 0x02FF3C1B},
        {"cellSysutil", "cellSysutilCheckCallback", 0x189A74DA},
        {"cellSysutil", "cellSysutilGetSystemParamInt", 0x40E895D3},
        {"cellSysutil", "cellSysutilGetSystemParamString", 0x938013A0},
        {"cellSysutil", "cellSysCacheMount", 0x1E7BFF94},
        {"cellSysutil", "cellSysCacheClear", 0x744C1544},
        {"cellSysutil", "cellVideoOutGetState", 0x887572D5},
        {"cellSysutil", "cellVideoOutGetResolution", 0xE558748D},
        {"cellSysutil", "cellVideoOutConfigure", 0x0BAE8772},
        {"cellSysutil", "cellVideoOutGetDeviceInfo", 0x1E930EEF},
        {"cellSysutil", "cellVideoOutGetResolutionAvailability", 0xA322DB75},
        {"cellSysutil", "cellAudioOutGetState", 0xF4E3CAA0},
        {"cellSysutil", "cellAudioOutConfigure", 0x4692AB35},
        {"cellSysutil", "cellAudioOutGetConfiguration", 0xED5D96AF},
        {"cellSysutil", "cellAudioOutGetSoundAvailability", 0xC01B4E7C},
        {"cellSysutilAvconfExt", "cellVideoOutSetGamma", 0xC7020F62},
        {"cellSysutilAvconfExt", "cellAudioOutGetAvailableDeviceInfo", 0xC3273D9B},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("cellSysutil: events reach the registered guest callbacks through cellSysutilCheckCallback", "[hle][sysutil]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_callback(base);
    const auto calls = [&] { return ps_read32(base, kOut + 24); };

    CHECK(a.call("cellSysutilRegisterCallback", 4, kOpd, 0) == kValue);
    CHECK(a.call("cellSysutilUnregisterCallback", 4) == kValue);
    // An event goes to the callbacks registered when it is sent: none yet.
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_REQUEST_EXITGAME, 0) == 0);
    REQUIRE(a.call("cellSysutilRegisterCallback", 0, kOpd, 0xCAFE0000u) == 0);
    CHECK(a.call("cellSysutilCheckCallback") == 0);
    CHECK(calls() == 0);

    // Queued until the game checks; then called on the checking thread with (status, param, userdata) and the TOC
    // from the descriptor. The caller's r2 and LR survive the call (r1 is checked by GuestThread::call), and the
    // callback's own result (7) is dropped: its type returns void.
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_REQUEST_EXITGAME, 0x1122334455667788ull) == 1);
    CHECK(calls() == 0);
    a.ctx.gpr[2] = 0x2222;
    a.ctx.lr = 0x4444;
    CHECK(a.call("cellSysutilCheckCallback") == 0);
    CHECK(calls() == 1);
    CHECK(ps_read64(base, kOut + 0) == 0x0101);
    CHECK(ps_read64(base, kOut + 8) == 0x1122334455667788ull);
    CHECK(ps_read32(base, kOut + 16) == 0xCAFE0000u);
    CHECK(ps_read32(base, kOut + 20) == kToc);
    CHECK(a.ctx.gpr[2] == 0x2222);
    CHECK(a.ctx.lr == 0x4444);
    CHECK(a.call("cellSysutilCheckCallback") == 0);
    CHECK(calls() == 1); // delivered once

    // One queued call per registered slot, in slot order.
    REQUIRE(a.call("cellSysutilRegisterCallback", 3, kOpd, 0xD0D0) == 0);
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_SYSTEM_MENU_OPEN, 1) == 2);
    REQUIRE(a.call("cellSysutilUnregisterCallback", 0) == 0);
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_SYSTEM_MENU_CLOSE, 2) == 1);
    CHECK(a.call("cellSysutilCheckCallback") == 0);
    CHECK(calls() == 4);
    CHECK(ps_read64(base, kOut + 0) == 0x0132); // the last one: SYSTEM_MENU_CLOSE, to slot 3
    CHECK(ps_read64(base, kOut + 8) == 2);
    CHECK(ps_read32(base, kOut + 16) == 0xD0D0);

    // DRAWING_BEGIN and DRAWING_END come in pairs.
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_BEGIN, 0) == 1);
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_BEGIN, 0) == kBusy);
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_END, 0) == 1);
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_END, 0) == -1);
    CHECK(a.call("cellSysutilCheckCallback") == 0);
    CHECK(calls() == 6);

    // Host functions queued for the check run on the checking thread, in order. A non-zero result ends the check
    // with that result; what was queued after it runs at the next check.
    std::vector<int> order;
    hle::sysutil_queue_call([&](PPUContext& c) {
        CHECK(&c == &a.ctx);
        order.push_back(1);
        return 0;
    });
    hle::sysutil_queue_call([&](PPUContext&) {
        order.push_back(2);
        return 5;
    });
    hle::sysutil_queue_call([&](PPUContext&) {
        order.push_back(3);
        return 0;
    });
    CHECK(a.call("cellSysutilCheckCallback") == 5);
    CHECK(order == std::vector<int>{1, 2});
    CHECK(a.call("cellSysutilCheckCallback") == 0);
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("cellSysutil: system parameters", "[hle][sysutil]")
{
    constexpr u32 kValueAddr = kData + 0x100, kBuf = kData + 0x1000;
    {
        // LICENSE_AREA answers only games built with SDK 2.4 or older.
        lv2::Config old_sdk;
        old_sdk.sdk_version = 0x00230001;
        Fixture f({}, old_sdk);
        GuestThread a(f, 0, "a");
        CHECK(a.call("cellSysutilGetSystemParamInt", 0x0122, kValueAddr) == 0);
        CHECK(ps_read32(f.base(), kValueAddr) == 1); // CELL_SYSUTIL_LICENSE_AREA_A
    }

    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-sysutil-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");

    struct Expected {
        s32 id;
        u32 value;
    };
    const Expected ints[] = {
        {0x0111, 1},   // LANG: English (US)
        {0x0112, 1},   // ENTER_BUTTON_ASSIGN: cross
        {0x0114, 1},   // DATE_FORMAT: DD/MM/YYYY
        {0x0115, 1},   // TIME_FORMAT: 24 hours
        {0x0116, 180}, // TIMEZONE
        {0x0117, 0},   // SUMMERTIME
        {0x0121, 0},   // GAME_PARENTAL_LEVEL: off
        {0x0123, 0},   // GAME_PARENTAL_LEVEL0_RESTRICT
        {0x0141, 0},   // CURRENT_USER_HAS_NP_ACCOUNT
        {0x0151, 0},   // CAMERA_PLFREQ
        {0x0152, 1},   // PAD_RUMBLE: on
        {0x0153, 0},   // KEYBOARD_TYPE: US
        {0x0154, 0},   // JAPANESE_KEYBOARD_ENTRY_METHOD
        {0x0155, 0},   // CHINESE_KEYBOARD_ENTRY_METHOD
        {0x0156, 0},   // PAD_AUTOOFF
        {0x0157, 0},   // MAGNETOMETER
    };
    for (const Expected& e : ints) {
        INFO("id " << e.id);
        ps_write32(base, kValueAddr, 0xDEADBEEF);
        CHECK(a.call("cellSysutilGetSystemParamInt", e.id, kValueAddr) == 0);
        CHECK(ps_read32(base, kValueAddr) == e.value);
    }
    CHECK(a.call("cellSysutilGetSystemParamInt", 0x0122, kValueAddr) == kValue); // the fixture's SDK is 3.6
    CHECK(a.call("cellSysutilGetSystemParamInt", 0x0111, 0) == kValue);
    CHECK(a.call("cellSysutilGetSystemParamInt", 0x0999, kValueAddr) == kValue);
    CHECK(a.call("cellSysutilGetSystemParamInt", 0x0113, kValueAddr) == kValue); // a string parameter

    // Strings: the buffer size must be the parameter's, and it is filled up with zeros.
    std::memset(base + kBuf, 0xAA, 0x500);
    CHECK(a.call("cellSysutilGetSystemParamString", 0x0113, kBuf, 0x80) == 0); // NICKNAME
    CHECK(guest_string(base, kBuf) == "PSPocket");
    CHECK(base[kBuf + 0x7F] == 0);
    CHECK(base[kBuf + 0x80] == 0xAA);
    CHECK(a.call("cellSysutilGetSystemParamString", 0x0113, kBuf, 0x40) == kSize);
    CHECK(a.call("cellSysutilGetSystemParamString", 0x0131, kBuf, 0x40) == 0); // CURRENT_USERNAME
    CHECK(guest_string(base, kBuf) == "User");                                 // no /dev_hdd0: RPCS3's default name
    CHECK(a.call("cellSysutilGetSystemParamString", 0x0131, 0, 0x40) == kValue);
    CHECK(a.call("cellSysutilGetSystemParamString", 0x0111, kBuf, 0x40) == kValue); // an integer parameter
    // An ID games ask for without a known meaning: "Unknown", cut to the size.
    CHECK(a.call("cellSysutilGetSystemParamString", 0x1008, kBuf, 4) == 0);
    CHECK(guest_string(base, kBuf) == "Unk");
    CHECK(a.call("cellSysutilGetSystemParamString", 0x1024, kBuf, 4) == kSize);

    // The user name from /dev_hdd0/home/00000001/localusername, as RPCS3 reads it.
    fsys::create_directories(root / "home" / "00000001");
    std::ofstream(root / "home" / "00000001" / "localusername") << "Niko";
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    CHECK(a.call("cellSysutilGetSystemParamString", 0x0131, kBuf, 0x40) == 0);
    CHECK(guest_string(base, kBuf) == "Niko");
    fsys::remove_all(root);
}

TEST_CASE("cellSysCache: mount, relay and clear; what the end of a process leaves", "[hle][sysutil]")
{
    const fsys::path root = fsys::temp_directory_path() / ("pspocket-hle-syscache-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "hdd0");
    hle::ProcessConfig config;
    config.game.title_id = "BLES01807";
    const fsys::path caches = root / "hdd1" / "caches"; // next to /dev_hdd0's host directory
    const fsys::path cache1 = caches / "BLES01807_CACHE01";
    const std::string marker = "\xEF\xBC\x84"
                               "hdd0_temp_state_indicator"; // RPCS3's in-use marker
    const auto set_id = [&](u8* base, std::string_view id) {
        std::memset(base + kSysCacheParam, 0, 0x20);
        std::memset(base + kSysCacheParam + 0x20, 0xAA, 1055);
        std::memcpy(base + kSysCacheParam, id.data(), id.size());
    };
    struct Process {
        Fixture f;
        GuestThread a;
        Process(const hle::ProcessConfig& c, const fsys::path& hdd0) : f(c), a(f, 0, "a")
        {
            if (!hdd0.empty()) REQUIRE(f.kernel->vfs_mount("/dev_hdd0", hdd0.string(), false));
        }
        u64 mount() { return a.call("cellSysCacheMount", kSysCacheParam); }
        // sys_process_exit: the game ends the process itself.
        void guest_exit() { CHECK_THROWS_AS(lv2::_sys_process_exit(*a.t, 0, 0, 0), lv2::ThreadExit); }
    };

    {
        // Nowhere to put a cache.
        Process p(config, {});
        set_id(p.f.base(), "CACHE01");
        CHECK(p.mount() == kCacheAccess);
        CHECK(p.a.call("cellSysCacheClear") == kCacheNotMounted);
    }
    {
        Process p(config, root / "hdd0");
        u8* base = p.f.base();
        CHECK(p.a.call("cellSysCacheClear") == kCacheNotMounted);
        CHECK(p.a.call("cellSysCacheMount", 0) == kCacheParam);
        set_id(base, "cache01"); // [A-Z0-9_-] only
        CHECK(p.mount() == kCacheParam);
        set_id(base, "_CACHE01"); // not '_' first
        CHECK(p.mount() == kCacheParam);

        set_id(base, "CACHE01");
        CHECK(p.mount() == 0); // CELL_SYSCACHE_RET_OK_CLEARED
        CHECK(guest_string(base, kSysCacheParam + 0x20) == "/dev_hdd1");
        CHECK(base[kSysCacheParam + 0x20 + 1054] == 0); // the rest of getCachePath zeroed
        CHECK(fsys::is_directory(cache1));
        CHECK(fsys::exists(cache1 / marker));
        CHECK(p.f.kernel->vfs_host_path("/dev_hdd1") == cache1.string());
        std::ofstream(cache1 / "data.bin") << "cached";
        CHECK(p.mount() == 1); // the same ID: CELL_SYSCACHE_RET_OK_RELAYED
        CHECK(fsys::exists(cache1 / "data.bin"));
        CHECK(p.a.call("cellSysCacheClear") == 0);
        CHECK_FALSE(fsys::exists(cache1 / "data.bin"));
        CHECK(fsys::exists(cache1 / marker)); // still in use

        set_id(base, "CACHE02"); // another ID replaces the cache
        CHECK(p.mount() == 0);
        CHECK_FALSE(fsys::exists(cache1));
        CHECK(fsys::is_directory(caches / "BLES01807_CACHE02"));
        set_id(base, ""); // an empty ID is never relayed
        CHECK(p.mount() == 0);
        std::ofstream(caches / "BLES01807_" / "data.bin") << "cached";
        CHECK(p.mount() == 0);
        CHECK_FALSE(fsys::exists(caches / "BLES01807_" / "data.bin"));
        CHECK_FALSE(fsys::exists(caches / "BLES01807_CACHE02"));

        set_id(base, "CACHE01");
        CHECK(p.mount() == 0);
        std::ofstream(cache1 / "data.bin") << "cached";
    }
    // Stopped from outside: the cache may be half written, so it goes.
    CHECK_FALSE(fsys::exists(cache1));
    {
        Process p(config, root / "hdd0");
        set_id(p.f.base(), "CACHE01");
        CHECK(p.mount() == 0);
        std::ofstream(cache1 / "data.bin") << "cached";
        p.guest_exit();
    }
    // The game exited by itself: the cache stays, no longer marked in use, and the next run gets it back.
    CHECK(fsys::exists(cache1 / "data.bin"));
    CHECK_FALSE(fsys::exists(cache1 / marker));
    {
        Process p(config, root / "hdd0");
        set_id(p.f.base(), "CACHE01");
        CHECK(p.mount() == 1);
        CHECK(fsys::exists(cache1 / "data.bin"));
        CHECK(fsys::exists(cache1 / marker));
        p.guest_exit();
    }
    // A run that crashed (its host process died: nothing removed the marker) leaves a cache that is cleared.
    std::ofstream(cache1 / marker).put('\0');
    {
        Process p(config, root / "hdd0");
        set_id(p.f.base(), "CACHE01");
        CHECK(p.mount() == 0);
        CHECK_FALSE(fsys::exists(cache1 / "data.bin"));
    }
    {
        // A host that mounts /dev_hdd1 itself chooses the root.
        fsys::create_directories(root / "host_hdd1");
        Process p(config, {});
        REQUIRE(p.f.kernel->vfs_mount("/dev_hdd1", (root / "host_hdd1").string(), false));
        set_id(p.f.base(), "CACHE01");
        CHECK(p.mount() == 0);
        CHECK(p.f.kernel->vfs_host_path("/dev_hdd1") == (root / "host_hdd1" / "caches" / "BLES01807_CACHE01").string());
    }
    fsys::remove_all(root);
}

TEST_CASE("cellVideoOut: the 720p display, configuration, gamma", "[hle][sysutil]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kState = kData + 0x100, kRes = kData + 0x200, kConfig = kData + 0x300, kInfo = kData + 0x400;

    // CellVideoOutState: enabled, RGB; 720p, progressive, no conversion, 16:9, 59.94 Hz.
    std::memset(base + kState, 0xEE, 16);
    REQUIRE(a.call("cellVideoOutGetState", 0, 0, kState) == 0);
    CHECK(base[kState + 0] == 0);
    CHECK(base[kState + 1] == 1);
    CHECK(base[kState + 8] == 2);
    CHECK(base[kState + 9] == 1);
    CHECK(base[kState + 10] == 0);
    CHECK(base[kState + 11] == 2);
    CHECK(ps_read16(base, kState + 14) == 1);
    CHECK(a.call("cellVideoOutGetState", 0, 0, 0) == kVideoIllegal);
    CHECK(a.call("cellVideoOutGetState", 0, 1, kState) == kVideoNoDevice);
    CHECK(a.call("cellVideoOutGetState", 1, 0, kState) == kVideoNoDevice); // nothing on the secondary output
    CHECK(a.call("cellVideoOutGetState", 2, 0, kState) == kVideoNoDevice);

    // Resolutions by ID.
    REQUIRE(a.call("cellVideoOutGetResolution", 2, kRes) == 0);
    CHECK(ps_read16(base, kRes) == 1280);
    CHECK(ps_read16(base, kRes + 2) == 720);
    REQUIRE(a.call("cellVideoOutGetResolution", 1, kRes) == 0);
    CHECK(ps_read16(base, kRes) == 1920);
    CHECK(ps_read16(base, kRes + 2) == 1080);
    REQUIRE(a.call("cellVideoOutGetResolution", 0x102, kRes) == 0); // the ID is taken as a byte, as RPCS3 does
    CHECK(ps_read16(base, kRes) == 1280);
    CHECK(a.call("cellVideoOutGetResolution", 3, kRes) == kVideoIllegal);
    CHECK(a.call("cellVideoOutGetResolution", 2, 0) == kVideoIllegal);

    // CellVideoOutDeviceInfo: HDMI, RGB, one mode (the display's).
    std::memset(base + kInfo, 0, 0x1C8);
    REQUIRE(a.call("cellVideoOutGetDeviceInfo", 0, 0, kInfo) == 0);
    CHECK(base[kInfo + 0] == 1);
    CHECK(base[kInfo + 1] == 1);
    CHECK(ps_read16(base, kInfo + 2) == 100);
    CHECK(base[kInfo + 4] == 1);
    CHECK(base[kInfo + 5] == 1);
    CHECK(base[kInfo + 6] == 1);
    CHECK(ps_read16(base, kInfo + 12) == 0xFFFF);
    CHECK(ps_read16(base, kInfo + 26) == 0xFFFF);
    CHECK(ps_read32(base, kInfo + 28) == 100);
    CHECK(base[kInfo + 32] == 2);
    CHECK(base[kInfo + 33] == 1);
    CHECK(base[kInfo + 34] == 0);
    CHECK(base[kInfo + 35] == 2);
    CHECK(ps_read16(base, kInfo + 38) == 5); // 60 Hz and 59.94 Hz
    CHECK(base[kInfo + 40] == 0);            // no second mode
    CHECK(a.call("cellVideoOutGetDeviceInfo", 0, 0, 0) == kVideoIllegal);
    CHECK(a.call("cellVideoOutGetDeviceInfo", 1, 0, kInfo) == kVideoNoDevice);

    // Availability is a boolean result.
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 0, 2, 0, 0) == 1); // 720p, any aspect
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 0, 2, 2, 0) == 1); // 720p, 16:9
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 0, 2, 1, 0) == 0); // 720p, 4:3
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 0, 1, 0, 0) == 0); // 1080
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 0, 0x81, 0, 0) == 0); // 3D
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 1, 2, 0, 0) == 0);
    CHECK(a.call("cellVideoOutGetResolutionAvailability", 2, 2, 0, 0) == kVideoUnsupportedOut);

    // CellVideoOutConfiguration: checked, then reported by the state.
    const auto configure = [&](u8 resolution, u8 aspect, u32 pitch) {
        std::memset(base + kConfig, 0, 16);
        base[kConfig + 0] = resolution;
        base[kConfig + 2] = aspect;
        ps_write32(base, kConfig + 12, pitch);
        return a.call("cellVideoOutConfigure", 0, kConfig, 0, 0);
    };
    const auto state_mode = [&] {
        REQUIRE(a.call("cellVideoOutGetState", 0, 0, kState) == 0);
        return std::pair<u32, u32>(base[kState + 8], base[kState + 11]);
    };
    CHECK(a.call("cellVideoOutConfigure", 0, 0, 0, 0) == kVideoIllegal);
    CHECK(configure(2, 2, 0) == kVideoOutOfRange); // no pitch
    CHECK(configure(0x92, 2, 0x1400) == kVideoIllegalConfiguration);
    CHECK(configure(0x81, 2, 0x1400) == kVideoUnsupportedMode); // 3D
    CHECK(configure(3, 2, 0x1400) == kVideoUnsupportedMode);    // no such resolution
    CHECK(state_mode() == std::pair<u32, u32>(2, 2));
    REQUIRE(configure(1, 1, 1920 * 4) == 0); // 1080, 4:3
    CHECK(state_mode() == std::pair<u32, u32>(1, 1));
    REQUIRE(configure(2, 0, 1280 * 4) == 0); // auto aspect: the display's
    CHECK(state_mode() == std::pair<u32, u32>(2, 2));
    REQUIRE(configure(4, 7, 720 * 4) == 0); // 480, an unknown aspect: the display's
    CHECK(state_mode() == std::pair<u32, u32>(4, 2));

    // cellSysutilAvconfExt: the gamma is a float argument (f1), 0.8 to 1.2, for the primary output only.
    a.ctx.fpr[1] = 1.0;
    CHECK(a.call("cellVideoOutSetGamma", 0) == 0);
    a.ctx.fpr[1] = 1.2;
    CHECK(a.call("cellVideoOutSetGamma", 0) == 0);
    a.ctx.fpr[1] = 0.8;
    CHECK(a.call("cellVideoOutSetGamma", 0) == 0);
    a.ctx.fpr[1] = 0.5;
    CHECK(a.call("cellVideoOutSetGamma", 0) == kVideoOutOfRange);
    a.ctx.fpr[1] = 1.3;
    CHECK(a.call("cellVideoOutSetGamma", 0) == kVideoOutOfRange);
    a.ctx.fpr[1] = 1.0;
    CHECK(a.call("cellVideoOutSetGamma", 1) == kVideoUnsupportedOut);
    a.ctx.fpr[1] = 2.0;
    CHECK(a.call("cellVideoOutSetGamma", 1) == kVideoOutOfRange); // the range is checked first
}

TEST_CASE("cellAudioOut: stereo LPCM at 48 kHz, configuration", "[hle][sysutil]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kState = kData + 0x100, kConfig = kData + 0x200, kInfo = kData + 0x400;

    // CellAudioOutState: enabled, LPCM, no downmixer; sound mode LPCM, 2 ch, 48 kHz, 2-channel layout.
    std::memset(base + kState, 0xEE, 20);
    REQUIRE(a.call("cellAudioOutGetState", 0, 0, kState) == 0);
    CHECK(base[kState + 0] == 0);
    CHECK(base[kState + 1] == 0);
    CHECK(base[kState + 2] == 0); // the whole structure is written
    CHECK(ps_read32(base, kState + 8) == 0);
    CHECK(base[kState + 12] == 0);
    CHECK(base[kState + 13] == 2);
    CHECK(base[kState + 14] == 4);
    CHECK(base[kState + 15] == 0);
    CHECK(ps_read32(base, kState + 16) == 1);
    CHECK(a.call("cellAudioOutGetState", 0, 0, 0) == kAudioIllegal);
    CHECK(a.call("cellAudioOutGetState", 0, 1, kState) == kAudioOutOfRange);
    CHECK(a.call("cellAudioOutGetState", 2, 0, kState) == kAudioIllegal);
    // No device on the secondary output, yet no error: what a PS3 returns (RPCS3).
    REQUIRE(a.call("cellAudioOutGetState", 1, 0, kState) == 0);
    CHECK(base[kState + 0] == 0x10);
    CHECK(ps_read32(base, kState + 16) == 0xD00C1680);

    // The most channels for a coding type and sample rate.
    CHECK(a.call("cellAudioOutGetSoundAvailability", 0, 0, 4, 0) == 2); // LPCM, 48 kHz
    CHECK(a.call("cellAudioOutGetSoundAvailability", 0, 1, 4, 0) == 0); // AC3
    CHECK(a.call("cellAudioOutGetSoundAvailability", 0, 0, 2, 0) == 0); // LPCM, 44.1 kHz
    CHECK(a.call("cellAudioOutGetSoundAvailability", 1, 0, 4, 0) == 0); // the secondary output

    // CellAudioOutConfiguration.
    std::memset(base + kConfig, 0xEE, 16);
    REQUIRE(a.call("cellAudioOutGetConfiguration", 0, kConfig, 0) == 0);
    CHECK(base[kConfig + 0] == 2);
    CHECK(base[kConfig + 1] == 0);
    CHECK(base[kConfig + 2] == 0);
    CHECK(ps_read32(base, kConfig + 12) == 0);
    CHECK(a.call("cellAudioOutGetConfiguration", 0, 0, 0) == kAudioIllegal);
    CHECK(a.call("cellAudioOutGetConfiguration", 1, kConfig, 0) == kAudioUnsupportedOut);
    CHECK(a.call("cellAudioOutGetConfiguration", 2, kConfig, 0) == kAudioIllegal);

    const auto configure = [&](u32 out, u8 channel, u32 down_mixer) {
        std::memset(base + kConfig, 0, 16);
        base[kConfig + 0] = channel; // LPCM (encoder 0)
        ps_write32(base, kConfig + 12, down_mixer);
        return a.call("cellAudioOutConfigure", out, kConfig, 0, 1);
    };
    const auto configuration = [&] {
        REQUIRE(a.call("cellAudioOutGetConfiguration", 0, kConfig, 0) == 0);
        return std::pair<u32, u32>(base[kConfig + 0], ps_read32(base, kConfig + 12));
    };
    CHECK(a.call("cellAudioOutConfigure", 0, 0, 0, 0) == kAudioIllegal);
    CHECK(configure(1, 2, 0) == kAudioUnsupportedOut);
    CHECK(configure(2, 2, 0) == kAudioIllegal);
    REQUIRE(configure(0, 2, 1) == 0); // stereo, downmixer type A
    CHECK(configuration() == std::pair<u32, u32>(2, 1));
    // 5.1 isn't offered: the configuration says 6 channels, the output stays in its stereo mode. An invalid downmixer
    // keeps the previous one.
    REQUIRE(configure(0, 6, 7) == 0);
    CHECK(configuration() == std::pair<u32, u32>(6, 1));
    REQUIRE(a.call("cellAudioOutGetState", 0, 0, kState) == 0);
    CHECK(base[kState + 13] == 2);
    CHECK(ps_read32(base, kState + 8) == 1);

    // cellSysutilAvconfExt: no audio devices besides the outputs.
    CHECK(a.call("cellAudioOutGetAvailableDeviceInfo", 16, kInfo) == 0);
    CHECK(a.call("cellAudioOutGetAvailableDeviceInfo", 17, kInfo) == kAudioIllegal);
    CHECK(a.call("cellAudioOutGetAvailableDeviceInfo", 1, 0) == kAudioIllegal);
}
