// cellSysutil and cellSysutilAvconfExt (synthetic: no game data): system events delivered to a guest callback by
// cellSysutilCheckCallback, system parameters, the system cache on host directories, video and audio output, the
// on-screen keyboard and message dialogs; and sysPrxForUser's sys_prx_register_library.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "sysutil.hpp" // runtime/hle/src (internal): the event queue other HLE modules use
#include "hle_test_util.hpp"
#include "pspocket/core/log.hpp"
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

constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

// A guest sysutil callback that keeps every call, behind the descriptor kLogOpd: it appends the low words of its
// (status, param, userdata) to a log at kLog + 16, 16 bytes an entry, and counts the entries at kLog.
constexpr u32 kLogCode = kData + 0x20000, kLogOpd = kData + 0x20800, kLog = kData + 0x21000;

void write_log_callback(u8* base)
{
    const std::vector<u32> code = {
        as::lis(6, static_cast<s32>(kLog >> 16)),
        as::ori(6, 6, kLog & 0xFFFF),
        as::lwz(7, 0, 6),
        as::slwi(8, 7, 4),
        as::addi(8, 8, 16),
        as::stwx(3, 6, 8),
        as::addi(9, 8, 4),
        as::stwx(4, 6, 9),
        as::addi(9, 8, 8),
        as::stwx(5, 6, 9),
        as::addi(7, 7, 1),
        as::stw(7, 0, 6),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kLogCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kLogOpd, kLogCode);
    ps_write32(base, kLogOpd + 4, kToc);
    std::memset(base + kLog, 0, 0x400);
}

struct Event {
    u32 status = 0, param = 0, userdata = 0;
    bool operator==(const Event&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Event& e)
{
    return os << std::hex << "{status 0x" << e.status << ", param 0x" << e.param << ", userdata 0x" << e.userdata
              << "}" << std::dec;
}

// The calls the log callback got, in order.
std::vector<Event> events(u8* base)
{
    std::vector<Event> v;
    for (u32 i = 0; i < ps_read32(base, kLog); i++) {
        v.push_back({ps_read32(base, kLog + 16 + 16 * i), ps_read32(base, kLog + 20 + 16 * i),
            ps_read32(base, kLog + 24 + 16 * i)});
    }
    return v;
}

// The log lines written while it lives (from the minimum level up).
struct CapturedLog {
    struct Line {
        log::Level level;
        std::string channel, message;
    };
    std::mutex mutex;
    std::vector<Line> lines;

    CapturedLog()
    {
        log::set_sink(
            [](void* user, log::Level level, std::string_view channel, std::string_view message) {
                auto* self = static_cast<CapturedLog*>(user);
                std::lock_guard lock(self->mutex);
                self->lines.push_back({level, std::string(channel), std::string(message)});
            },
            this);
    }
    ~CapturedLog() { log::set_sink(nullptr, nullptr); }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    // The lines at `level` on `channel` that contain `text`.
    usize count(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        usize n = 0;
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) n++;
        return n;
    }
};

// cellOskDialog's guest structures: CellOskDialogParam at kOskParam, CellOskDialogInputFieldInfo at kOskField (its
// message at kOskMessage, its initial text at kOskInit), CellOskDialogCallbackReturnParam at kOskOut and the result
// buffer at kOskString.
constexpr u32 kOskParam = kData + 0x30000, kOskField = kData + 0x30100, kOskMessage = kData + 0x30200,
              kOskInit = kData + 0x30600, kOskOut = kData + 0x30A00, kOskString = kData + 0x30B00;
const u64 kOskParamError = r3_of_code(0x8002b504); // CELL_OSKDIALOG_ERROR_PARAM
const u64 kNotOpened = r3_of_code(0x8002b302);     // CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED
const u64 kSysutilBusy = r3_of_code(0x8002b105);   // CELL_SYSUTIL_ERROR_BUSY
constexpr u32 kOskLoaded = 0x0502, kOskFinished = 0x0503, kOskUnloaded = 0x0504; // CELL_SYSUTIL_OSKDIALOG_*

void put_utf16(u8* base, u32 addr, std::u16string_view s)
{
    for (usize i = 0; i < s.size(); i++) ps_write16(base, addr + 2 * static_cast<u32>(i), s[i]);
    ps_write16(base, addr + 2 * static_cast<u32>(s.size()), 0);
}

std::u16string get_utf16(u8* base, u32 addr, u32 max)
{
    std::u16string s;
    for (u32 i = 0; i < max; i++) {
        const char16_t c = ps_read16(base, addr + 2 * i);
        if (!c) break;
        s += c;
    }
    return s;
}

// A dialog asking for a "Name", "Niko" in its field, room for `limit` units.
void write_osk_load_args(u8* base, u32 limit = 16)
{
    ps_write32(base, kOskParam + 0, 0x2);                        // allowOskPanelFlg: CELL_OSKDIALOG_PANELMODE_ENGLISH
    ps_write32(base, kOskParam + 4, 0x2);                        // firstViewPanel
    ps_write32(base, kOskParam + 8, std::bit_cast<u32>(0.5f));   // controlPoint
    ps_write32(base, kOskParam + 12, std::bit_cast<u32>(0.25f)); //
    ps_write32(base, kOskParam + 16, 0x1);                       // prohibitFlgs: CELL_OSKDIALOG_NO_SPACE
    put_utf16(base, kOskMessage, u"Name");
    put_utf16(base, kOskInit, u"Niko");
    ps_write32(base, kOskField + 0, kOskMessage);
    ps_write32(base, kOskField + 4, kOskInit);
    ps_write32(base, kOskField + 8, limit);
}

// CellOskDialogCallbackReturnParam {result: 0xAAAAAAAA until written, numCharsResultString, pResultString}, and the
// result buffer at kOskString filled with 0xAA.
void write_osk_output(u8* base, s32 num_chars, u32 result_string = kOskString)
{
    ps_write32(base, kOskOut + 0, 0xAAAAAAAA);
    ps_write32(base, kOskOut + 4, static_cast<u32>(num_chars));
    ps_write32(base, kOskOut + 8, result_string);
    std::memset(base + kOskString, 0xAA, 0x400);
}

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

TEST_CASE("cellSysutil dialogs, sysPrxForUser: the functions GTA V imports, in their modules", "[hle][sysutil]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"cellSysutil", "cellOskDialogLoadAsync", 0x7FCFC915},
        {"cellSysutil", "cellOskDialogUnloadAsync", 0x3D1E1931},
        {"cellSysutil", "cellOskDialogAbort", 0xB6D84526},
        {"cellSysutil", "cellOskDialogSetDeviceMask", 0xD3790A86},
        {"cellSysutil", "cellOskDialogSetKeyLayoutOption", 0xB53C54FA},
        {"cellSysutil", "cellOskDialogAddSupportLanguage", 0x7F21C918},
        {"cellSysutil", "cellOskDialogSetLayoutMode", 0xF0EC3CCC},
        {"cellSysutil", "cellMsgDialogAbort", 0x62B0F803},
        {"sysPrxForUser", "sys_prx_register_library", 0x42B23552},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("cellOskDialog without a keyboard: LOADED, cancelled at the check after (FINISHED), unload, UNLOADED",
    "[hle][sysutil][osk]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    REQUIRE(a.call("cellSysutilRegisterCallback", 0, kLogOpd, 0x05C0) == 0);
    write_osk_load_args(base);
    write_osk_output(base, 32);
    const auto load = [&] { return a.call("cellOskDialogLoadAsync", 0, kOskParam, kOskField); };
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    const Event loaded{kOskLoaded, 0, 0x05C0}, finished{kOskFinished, 0, 0x05C0}, unloaded{kOskUnloaded, 0, 0x05C0};

    // Nothing is loaded yet.
    CHECK(a.call("cellOskDialogAbort") == kNotOpened);
    CHECK(a.call("cellOskDialogUnloadAsync", kOskOut) == kNotOpened);
    CHECK(ps_read32(base, kOskOut) == 0xAAAAAAAA);

    CapturedLog logs;
    REQUIRE(load() == 0);
    CHECK(logs.count(log::Level::Warn, "cellOskDialog", "no keyboard UI") == 1);
    CHECK(load() == kSysutilBusy); // one dialog at a time
    CHECK(events(base).empty());   // the game's callbacks run at its checks

    // The first check delivers LOADED, then the missing keyboard cancels the dialog: FINISHED comes at the next one.
    check();
    CHECK(events(base) == std::vector<Event>{loaded});
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished});
    check();
    CHECK(events(base).size() == 2);
    CHECK(load() == kSysutilBusy); // loaded until the unload

    // The unload reports the cancel; as in RPCS3, a cancelled dialog's text isn't copied. UNLOADED follows.
    REQUIRE(a.call("cellOskDialogUnloadAsync", kOskOut) == 0);
    CHECK(ps_read32(base, kOskOut + 0) == 1); // CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED
    CHECK(ps_read32(base, kOskOut + 4) == 32);
    CHECK(ps_read32(base, kOskOut + 8) == kOskString);
    CHECK(all_bytes(base + kOskString, 0x400, 0xAA));
    CHECK(events(base).size() == 2);
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished, unloaded});

    // Gone: nothing to abort or unload, and the next dialog loads.
    CHECK(a.call("cellOskDialogAbort") == kNotOpened);
    CHECK(a.call("cellOskDialogUnloadAsync", kOskOut) == kNotOpened);
    REQUIRE(load() == 0);
    check();
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished, unloaded, loaded, finished});
    REQUIRE(a.call("cellOskDialogUnloadAsync", kOskOut) == 0);
    CHECK(ps_read32(base, kOskOut) == 1);
}

TEST_CASE("cellOskDialog: the load's and the unload's argument checks", "[hle][sysutil][osk]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    const auto load = [&](u32 param, u32 field) { return a.call("cellOskDialogLoadAsync", 0, param, field); };
    write_osk_load_args(base);

    // RPCS3's checks: both pointers, the message and initial text pointers, limit_length up to 512.
    CHECK(load(0, kOskField) == kOskParamError);
    CHECK(load(kOskParam, 0) == kOskParamError);
    ps_write32(base, kOskField + 0, 0);
    CHECK(load(kOskParam, kOskField) == kOskParamError);
    ps_write32(base, kOskField + 0, kOskMessage);
    ps_write32(base, kOskField + 4, 0);
    CHECK(load(kOskParam, kOskField) == kOskParamError);
    ps_write32(base, kOskField + 4, kOskInit);
    ps_write32(base, kOskField + 8, 513);
    CHECK(load(kOskParam, kOskField) == kOskParamError);

    // Not guest memory (RPCS3 faults): the structures, or strings that run into such memory before they end.
    ps_write32(base, kOskField + 8, 16);
    CHECK(load(kUnmapped, kOskField) == kOskParamError);
    CHECK(load(kOskParam, kUnmapped) == kOskParamError);
    ps_write32(base, kOskField + 0, kUnmapped);
    CHECK(load(kOskParam, kOskField) == kOskParamError);
    constexpr u32 kEnd = kData + 0x300000; // the end of the fixture's committed memory
    REQUIRE_FALSE(f.mem.is_committed(kEnd));
    ps_write16(base, kEnd - 4, u'A');
    ps_write16(base, kEnd - 2, u'B');
    ps_write32(base, kOskField + 0, kEnd - 4); // a message without a terminator
    CHECK(load(kOskParam, kOskField) == kOskParamError);
    ps_write32(base, kOskField + 0, kOskMessage);
    ps_write32(base, kOskField + 4, kEnd - 4); // an initial text, read up to the limit (16 units)
    CHECK(load(kOskParam, kOskField) == kOskParamError);
    CHECK(a.call("cellOskDialogAbort") == kNotOpened); // none of these loaded a dialog
    ps_write32(base, kOskField + 8, 2);                 // read only as far as the limit
    REQUIRE(load(kOskParam, kOskField) == 0);

    // The unload: the pointer, then numCharsResultString (not negative), then a loaded dialog.
    CHECK(a.call("cellOskDialogUnloadAsync", 0) == kOskParamError);
    CHECK(a.call("cellOskDialogUnloadAsync", kUnmapped) == kOskParamError);
    write_osk_output(base, -1);
    CHECK(a.call("cellOskDialogUnloadAsync", kOskOut) == kOskParamError);
    CHECK(ps_read32(base, kOskOut) == 0xAAAAAAAA);
    // No check ran yet, so the dialog is still open and its result so far is OK: its text is copied, and a result
    // buffer that is not guest memory is refused, the dialog kept.
    write_osk_output(base, 32, kUnmapped);
    CHECK(a.call("cellOskDialogUnloadAsync", kOskOut) == kOskParamError);
    CHECK(ps_read32(base, kOskOut) == 0xAAAAAAAA);
    write_osk_output(base, 32);
    REQUIRE(a.call("cellOskDialogUnloadAsync", kOskOut) == 0);
    CHECK(ps_read32(base, kOskOut) == 0); // CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK
    CHECK(get_utf16(base, kOskString, 64) == u"AB");
    CHECK(all_bytes(base + kOskString + 6, 0x400 - 6, 0xAA));
    CHECK(a.call("cellOskDialogUnloadAsync", kOskOut) == kNotOpened);

    // limit_length 512 is allowed (511 units).
    write_osk_load_args(base, 512);
    REQUIRE(load(kOskParam, kOskField) == 0);
    write_osk_output(base, 32);
    REQUIRE(a.call("cellOskDialogUnloadAsync", kOskOut) == 0);
    CHECK(get_utf16(base, kOskString, 64) == u"Niko");
}

TEST_CASE("cellOskDialog: abort; an unload before the keyboard answered copies the text (RPCS3)", "[hle][sysutil][osk]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    REQUIRE(a.call("cellSysutilRegisterCallback", 1, kLogOpd, 0xAB) == 0);
    write_osk_load_args(base);
    const auto load = [&] { return a.call("cellOskDialogLoadAsync", 0, kOskParam, kOskField); };
    const auto unload = [&] { return a.call("cellOskDialogUnloadAsync", kOskOut); };
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    const Event loaded{kOskLoaded, 0, 0xAB}, finished{kOskFinished, 0, 0xAB}, unloaded{kOskUnloaded, 0, 0xAB};

    // An abort right after the load sends FINISHED, the result ABORT. The keyboard's cancel, which the load queued,
    // then finds the dialog closed and does nothing.
    REQUIRE(load() == 0);
    REQUIRE(a.call("cellOskDialogAbort") == 0);
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished});
    check();
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished});
    // Every abort of a loaded dialog sends FINISHED (RPCS3), closed or not.
    REQUIRE(a.call("cellOskDialogAbort") == 0);
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished, finished});
    write_osk_output(base, 32);
    REQUIRE(unload() == 0);
    CHECK(ps_read32(base, kOskOut) == 2);              // CELL_OSKDIALOG_INPUT_FIELD_RESULT_ABORT
    CHECK(all_bytes(base + kOskString, 0x400, 0xAA)); // not copied
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished, finished, unloaded});

    // An unload while the dialog is still open: the result so far is OK, and RPCS3 copies the text (here the initial
    // one): numCharsResultString units at most, then a terminator.
    REQUIRE(load() == 0);
    write_osk_output(base, 2);
    REQUIRE(unload() == 0);
    CHECK(ps_read32(base, kOskOut) == 0); // CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK
    CHECK(get_utf16(base, kOskString, 64) == u"Ni");
    CHECK(all_bytes(base + kOskString + 6, 0x400 - 6, 0xAA));
    // Without a result buffer only the result is written; with numCharsResultString 0, only a terminator.
    REQUIRE(load() == 0);
    write_osk_output(base, 32, 0);
    REQUIRE(unload() == 0);
    CHECK(ps_read32(base, kOskOut) == 0);
    CHECK(all_bytes(base + kOskString, 0x400, 0xAA));
    REQUIRE(load() == 0);
    write_osk_output(base, 0);
    REQUIRE(unload() == 0);
    CHECK(ps_read16(base, kOskString) == 0);
    CHECK(all_bytes(base + kOskString + 2, 0x400 - 2, 0xAA));

    // The keyboard's answers to those three dialogs come at the next check, find them unloaded, and leave alone the
    // dialog loaded since: that one is cancelled by its own answer, its FINISHED delivered at the check after.
    REQUIRE(load() == 0);
    check();
    CHECK(events(base) == std::vector<Event>{loaded, finished, finished, unloaded, loaded, unloaded, loaded, unloaded,
                              loaded, unloaded, loaded});
    check();
    CHECK(events(base).back() == finished);
    write_osk_output(base, 32);
    REQUIRE(unload() == 0);
    CHECK(ps_read32(base, kOskOut) == 1); // CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED
}

TEST_CASE("cellOskDialog settings: RPCS3's checks, with or without a dialog", "[hle][sysutil][osk]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    for (int loaded = 0; loaded < 2; loaded++) {
        INFO("a dialog loaded: " << loaded);
        if (loaded) {
            write_osk_load_args(f.base());
            REQUIRE(a.call("cellOskDialogLoadAsync", 0, kOskParam, kOskField) == 0);
        }
        // The device mask: up to CELL_OSKDIALOG_DEVICE_MASK_PAD.
        CHECK(a.call("cellOskDialogSetDeviceMask", 0x100) == kOskParamError);
        CHECK(a.call("cellOskDialogSetDeviceMask", 0xFFFFFFFFu) == kOskParamError);
        CHECK(a.call("cellOskDialogSetDeviceMask", 0xFF) == 0);
        CHECK(a.call("cellOskDialogSetDeviceMask", 0) == 0);
        // The key layout option: the 10-key panel (1), the full keyboard (2), or both.
        CHECK(a.call("cellOskDialogSetKeyLayoutOption", 0) == kOskParamError);
        CHECK(a.call("cellOskDialogSetKeyLayoutOption", 4) == kOskParamError);
        CHECK(a.call("cellOskDialogSetKeyLayoutOption", 0xFFFFFFFFu) == kOskParamError);
        for (const u32 option : {1u, 2u, 3u}) CHECK(a.call("cellOskDialogSetKeyLayoutOption", option) == 0);
        // The languages and the layout mode: anything (RPCS3 checks nothing).
        CHECK(a.call("cellOskDialogAddSupportLanguage", 0) == 0);
        CHECK(a.call("cellOskDialogAddSupportLanguage", 0xFFFFFFFFu) == 0);
        CHECK(a.call("cellOskDialogSetLayoutMode", 0x2400) == 0); // centred
        CHECK(a.call("cellOskDialogSetLayoutMode", 0xFFFFFFFFu) == 0);
    }
}

TEST_CASE("cellMsgDialogAbort: closes the dialog cellMsgDialog opened, without its callback; CELL_OK without one",
    "[hle][sysutil][msgdialog]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    REQUIRE(a.call("cellSysutilRegisterCallback", 0, kLogOpd, 0x5150) == 0);
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    const Event begin{0x0121, 0, 0x5150}, end{0x0122, 0, 0x5150}; // DRAWING_BEGIN, DRAWING_END

    // No dialog: CELL_OK all the same, not CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED (RPCS3: tested on hardware).
    CHECK(a.call("cellMsgDialogAbort") == 0);
    check();
    CHECK(events(base).empty());

    // A dialog the game opened begins drawing. The abort closes it without its callback (here the log callback too:
    // an answer would show in the log as a call with userdata 0xD1A1) and ends the drawing.
    constexpr u32 kTypeOk = 0x21; // CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL | CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK
    REQUIRE(hle::open_msg_dialog(hle::MsgDialogSource::CellMsgDialog, kTypeOk, "Saving.", kLogOpd, 0xD1A1) == 0);
    CHECK(hle::open_msg_dialog(hle::MsgDialogSource::CellMsgDialog, kTypeOk, "Again.", kLogOpd, 0xD1A1) == kBusy);
    check();
    CHECK(events(base) == std::vector<Event>{begin});
    CHECK(a.call("cellMsgDialogAbort") == 0);
    check();
    CHECK(events(base) == std::vector<Event>{begin, end});
    CHECK(a.call("cellMsgDialogAbort") == 0); // closed: nothing more happens
    check();
    CHECK(events(base) == std::vector<Event>{begin, end});

    // While a system dialog draws, none opens.
    REQUIRE(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_BEGIN, 0) == 1);
    CHECK(hle::open_msg_dialog(hle::MsgDialogSource::CellMsgDialog, kTypeOk, "Saving.", kLogOpd, 0xD1A1) == kBusy);
    REQUIRE(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_END, 0) == 1);
    CHECK(a.call("cellMsgDialogAbort") == 0);
    CHECK(hle::sysutil_send_event(hle::CELL_SYSUTIL_DRAWING_END, 0) == -1); // nothing draws
    check();
    CHECK(events(base) == std::vector<Event>{begin, end, begin, end});

    // A dialog another module opened stays open.
    REQUIRE(hle::open_msg_dialog(hle::MsgDialogSource::CellSaveData, kTypeOk, "Overwrite?", 0, 0) == 0);
    CHECK(a.call("cellMsgDialogAbort") == 0);
    CHECK(hle::open_msg_dialog(hle::MsgDialogSource::CellMsgDialog, kTypeOk, "Saving.", kLogOpd, 0xD1A1) == kBusy);
    check();
    CHECK(events(base) == std::vector<Event>{begin, end, begin, end, begin});
}

TEST_CASE("sys_prx_register_library: CELL_EFAULT outside guest memory, else CELL_OK, logged once at warning",
    "[hle][prx]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    // A library entry as an executable exports it (ppu_prx_module_info, 0x1c bytes): "testlib", one function.
    constexpr u32 kEntry = kData + 0x40000, kName = kEntry + 0x40, kNids = kEntry + 0x60, kAddrs = kEntry + 0x70;
    std::memset(base + kEntry, 0, 0x80);
    base[kEntry + 0] = 0x1C;           // size
    ps_write16(base, kEntry + 2, 1);   // version
    ps_write16(base, kEntry + 4, 0x1); // attributes: a library
    ps_write16(base, kEntry + 6, 1);   // num_func
    ps_write32(base, kEntry + 16, kName);
    ps_write32(base, kEntry + 20, kNids);
    ps_write32(base, kEntry + 24, kAddrs);
    std::memcpy(base + kName, "testlib", 8);
    ps_write32(base, kNids, 0x12345678);
    ps_write32(base, kAddrs, kData + 0x40100);
    const std::vector<u8> before(base + kEntry, base + kEntry + 0x80);

    CapturedLog logs;
    CHECK(a.call("sys_prx_register_library", 0) == r3_of(lv2::CELL_EFAULT));
    CHECK(a.call("sys_prx_register_library", kUnmapped) == r3_of(lv2::CELL_EFAULT));
    CHECK(a.call("sys_prx_register_library", kEntry) == 0);
    CHECK(a.call("sys_prx_register_library", kEntry) == 0);
    CHECK(std::equal(before.begin(), before.end(), base + kEntry)); // nothing written
    CHECK(logs.count(log::Level::Warn, "sysPrxForUser", "not registered") == 1);
    CHECK(logs.count(log::Level::Warn, "sysPrxForUser", "(\"testlib\", attributes 0x1, 1 functions, 0 variables)") ==
        1);
}

TEST_CASE("cellMsgDialog: progress bars, a timed close, the error code dialog, the older open (D-183)",
    "[hle][sysutil][msgdialog]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_log_callback(base);
    REQUIRE(a.call("cellSysutilRegisterCallback", 0, kLogOpd, 0x5150) == 0);
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    const Event begin{0x0121, 0, 0x5150}, end{0x0122, 0, 0x5150};
    const u64 kParam = r3_of_code(0x8002b301);
    // The dialog's callback(button, userData): two arguments, the third word logged is whatever r5 held.
    const auto answered = [&](u32 button, u32 userdata) {
        const std::vector<Event> e = events(base);
        REQUIRE(e.size() == 3);
        CHECK(e[0] == begin);
        CHECK(e[1] == end);
        CHECK(e[2].status == button);
        CHECK(e[2].param == userdata);
    };
    constexpr u32 kMsg = kData + 0x40000, kBar = kData + 0x40100;
    std::memcpy(base + kMsg, "Loading", 8);
    std::memcpy(base + kBar, "Step 1", 7);

    // Nothing is open.
    a.ctx.fpr[1] = 0.0;
    CHECK(a.call("cellMsgDialogClose") == kNotOpened);
    CHECK(a.call("cellMsgDialogProgressBarInc", 0, 10) == kNotOpened);

    // A dialog with one progress bar and no buttons stays open; its bar takes messages and increments.
    constexpr u32 kProgress = 0x1001; // SE_TYPE_NORMAL, one progress bar
    REQUIRE(a.call("cellMsgDialogOpen2", kProgress, kMsg, kLogOpd, 0xD1A1, 0) == 0);
    CHECK(a.call("cellMsgDialogProgressBarSetMsg", 0, kBar) == 0);
    CHECK(a.call("cellMsgDialogProgressBarSetMsg", 1, kBar) == kParam); // one bar only
    CHECK(a.call("cellMsgDialogProgressBarSetMsg", 0, 0) == kParam);
    CHECK(a.call("cellMsgDialogProgressBarInc", 0, 50) == 0);
    CHECK(a.call("cellMsgDialogProgressBarReset", 0) == 0);
    CHECK(a.call("cellMsgDialogProgressBarReset", 2) == kParam);
    check();
    CHECK(events(base) == std::vector<Event>{begin});

    // cellMsgDialogClose(delay): a long delay leaves it open; then 0 closes it at the next check, with
    // CELL_MSGDIALOG_BUTTON_NONE (-1) to its callback after the end of the drawing.
    a.ctx.fpr[1] = 60000.0;
    CHECK(a.call("cellMsgDialogClose") == 0);
    check();
    CHECK(events(base) == std::vector<Event>{begin});
    a.ctx.fpr[1] = 0.0;
    CHECK(a.call("cellMsgDialogClose") == 0);
    check();
    answered(0xFFFFFFFF, 0xD1A1);
    CHECK(a.call("cellMsgDialogClose") == kNotOpened);

    // The error code dialog is an OK dialog, answered at once (OK = 1) at the check after the one that begins it.
    std::memset(base + kLog, 0, 0x400);
    REQUIRE(a.call("cellMsgDialogOpenErrorCode", 0x80010006, kLogOpd, 0xE1, 0) == 0);
    check();
    check();
    answered(1, 0xE1);

    // The older cellMsgDialogOpen is cellMsgDialogOpen2, checks included.
    std::memset(base + kLog, 0, 0x400);
    CHECK(a.call("cellMsgDialogOpen", 0x8000000, kMsg, kLogOpd, 0, 0) == kParam);
    REQUIRE(a.call("cellMsgDialogOpen", 0x21, kMsg, kLogOpd, 0x0E, 0) == 0);
    check();
    check();
    answered(1, 0x0E);
    REQUIRE(a.call("cellMsgDialogOpenSimulViewWarning", 0, 0, 0) == 0);
    check();
    check();
}

TEST_CASE("cellSysutil: callback dispatchers, game exits, background music Ex (D-183)", "[hle][sysutil]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    for (u32 i = 0; i < 32; i++) CHECK(a.call("cellSysutilRegisterCallbackDispatcher", 0x100 + i, 0x1000 + i) == 0);
    CHECK(a.call("cellSysutilRegisterCallbackDispatcher", 0x200, 0x2000) == r3_of_code(0x8002b004));
    CHECK(a.call("cellSysutilUnregisterCallbackDispatcher", 0x105) == 0);
    CHECK(a.call("cellSysutilUnregisterCallbackDispatcher", 0x105) == r3_of_code(0x8002b005));
    CHECK(a.call("cellSysutilRegisterCallbackDispatcher", 0x200, 0x2000) == 0);

    CHECK(a.call("cellSysutilGameDataExit", 4) == 0);
    CHECK(a.call("cellSysutilGameDataExit", 5) == r3_of_code(0x8002b604));
    CHECK(a.call("cellSysutilGameExit_I") == 0);
    CHECK(a.call("cellSysutilGamePowerOff_I") == 0);
    CHECK(a.call("cellSysutilGameReboot_I") == 0);
    ps_write32(base, kData, 0xFFFFFFFF);
    CHECK(a.call("cellSysutil_E1EC7B6A", kData) == 0);
    CHECK(ps_read32(base, kData) == 0);

    // CellSysutilBgmPlaybackExtraParam: four fade times, -1 to 60000 ms.
    const u64 kExParam = r3_of_code(0x8002d301);
    constexpr u32 kParamAddr = kData + 0x100, kStatus = kData + 0x200;
    const auto fades = [&](s32 a0, s32 a1, s32 a2, s32 a3) {
        ps_write32(base, kParamAddr + 0, static_cast<u32>(a0));
        ps_write32(base, kParamAddr + 4, static_cast<u32>(a1));
        ps_write32(base, kParamAddr + 8, static_cast<u32>(a2));
        ps_write32(base, kParamAddr + 12, static_cast<u32>(a3));
    };
    fades(-1, 0, 60000, 500);
    CHECK(a.call("cellSysutilEnableBgmPlaybackEx", kParamAddr) == 0);
    REQUIRE(a.call("cellSysutilGetBgmPlaybackStatus", kStatus) == 0);
    CHECK(ps_read8(base, kStatus + 1) == 0); // enabled
    CHECK(a.call("cellSysutilDisableBgmPlaybackEx", kParamAddr) == 0);
    REQUIRE(a.call("cellSysutilGetBgmPlaybackStatus", kStatus) == 0);
    CHECK(ps_read8(base, kStatus + 1) == 1); // disabled
    CHECK(a.call("cellSysutilSetBgmPlaybackExtraParam", kParamAddr) == 0);
    fades(0, 60001, 0, 0);
    CHECK(a.call("cellSysutilEnableBgmPlaybackEx", kParamAddr) == kExParam);
    fades(-2, 0, 0, 0);
    CHECK(a.call("cellSysutilSetBgmPlaybackExtraParam", kParamAddr) == kExParam);
    CHECK(a.call("cellSysutilDisableBgmPlaybackEx", 0) == kExParam);
    std::memset(base + kStatus, 0xAA, 8);
    CHECK(a.call("cellSysutilGetBgmPlaybackStatus2", kStatus) == 0);
    CHECK(ps_read8(base, kStatus) == 1); // stopped
    CHECK(all_bytes(base + kStatus + 1, 7, 0));
    CHECK(a.call("cellSysutilGetBgmPlaybackStatus2", 0) == kExParam);
}

TEST_CASE("cellVideoOut, cellAudioOut, cellSysutilAvconfExt: the rest of RPCS3's functions (D-183)", "[hle][sysutil]")
{
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kConfig = kData + 0x100, kOption = kData + 0x120, kInfo = kData + 0x200;

    CHECK(a.call("cellVideoOutGetNumberOfDevice", 0) == 1);
    CHECK(a.call("cellVideoOutGetNumberOfDevice", 1) == 0);
    CHECK(a.call("cellVideoOutGetNumberOfDevice", 2) == kVideoUnsupportedOut);

    // Before cellVideoOutConfigure: the display's 720p, X8R8G8B8, 16:9, 4 bytes a pixel.
    std::memset(base + kConfig, 0xAA, 16);
    ps_write32(base, kOption, 0xAAAAAAAA);
    REQUIRE(a.call("cellVideoOutGetConfiguration", 0, kConfig, kOption) == 0);
    CHECK(ps_read8(base, kConfig + 0) == 2);
    CHECK(ps_read8(base, kConfig + 1) == 0);
    CHECK(ps_read8(base, kConfig + 2) == 2);
    CHECK(ps_read32(base, kConfig + 12) == 1280 * 4);
    CHECK(ps_read32(base, kOption) == 0);
    // After: what it set.
    std::memset(base + kConfig, 0, 16);
    ps_write8(base, kConfig + 0, 4); // 480
    ps_write8(base, kConfig + 1, 1); // X8B8G8R8
    ps_write8(base, kConfig + 2, 1); // 4:3
    ps_write32(base, kConfig + 12, 0x1000);
    REQUIRE(a.call("cellVideoOutConfigure", 0, kConfig, 0, 0) == 0);
    std::memset(base + kConfig, 0xAA, 16);
    REQUIRE(a.call("cellVideoOutGetConfiguration", 0, kConfig, 0) == 0);
    CHECK(ps_read8(base, kConfig + 0) == 4);
    CHECK(ps_read8(base, kConfig + 1) == 1);
    CHECK(ps_read8(base, kConfig + 2) == 1);
    CHECK(ps_read32(base, kConfig + 12) == 0x1000);
    REQUIRE(a.call("cellVideoOutGetConfiguration", 1, kConfig, 0) == 0); // the secondary output: zeros
    CHECK(all_bytes(base + kConfig, 16, 0));
    CHECK(a.call("cellVideoOutGetConfiguration", 2, kConfig, 0) == kVideoUnsupportedOut);
    CHECK(a.call("cellVideoOutGetConfiguration", 0, 0, 0) == kVideoIllegal);
    CHECK(a.call("cellVideoOutGetConvertCursorColorInfo", kInfo) == 0);
    CHECK(ps_read8(base, kInfo) == 1); // full range

    // Cursor colours: channels 1 to 3 gamma-corrected (8-bit formats) or copied (float), channel 0 left alone.
    constexpr u32 kSrc = kData + 0x300, kDst = kData + 0x340;
    const u8 pixel[4] = {0x11, 0x80, 0x40, 0xFF};
    std::memcpy(base + kSrc, pixel, 4);
    std::memset(base + kDst, 0xEE, 4);
    a.ctx.fpr[1] = 1.2;
    REQUIRE(a.call("cellVideoOutConvertCursorColor", 0, 0, 0, kSrc, kDst, 1) == 0);
    CHECK(ps_read8(base, kDst) == 0xEE);
    for (u32 c = 1; c < 4; c++) {
        const f32 want = std::clamp(std::pow(static_cast<f32>(pixel[c]) / 255.0f, 1.2f), 0.0f, 1.0f) * 255.0f;
        CHECK(ps_read8(base, kDst + c) == static_cast<u8>(want + 0.5f));
    }
    a.ctx.fpr[1] = 0.5; // out of range, except for the float format
    CHECK(a.call("cellVideoOutConvertCursorColor", 0, 0, 0, kSrc, kDst, 1) == kVideoOutOfRange);
    REQUIRE(a.call("cellVideoOutConvertCursorColor", 0, 2, 0, kSrc, kDst, 1) == 0);
    CHECK(ps_read8(base, kDst + 1) == 0x80);
    CHECK(a.call("cellVideoOutConvertCursorColor", 0, 3, 0, kSrc, kDst, 1) == kVideoOutOfRange);
    CHECK(a.call("cellVideoOutConvertCursorColor", 0, 0, 1, kSrc, kDst, 1) == kVideoOutOfRange);
    CHECK(a.call("cellVideoOutConvertCursorColor", 0, 0, 0, 0, kDst, 1) == kVideoIllegal);

    // Gamma round trip, the screen size of a 3D display (none), copy control, XV colour, the secondary display.
    a.ctx.fpr[1] = 1.1;
    REQUIRE(a.call("cellVideoOutSetGamma", 0) == 0);
    REQUIRE(a.call("cellVideoOutGetGamma", 0, kInfo) == 0);
    CHECK(std::bit_cast<f32>(ps_read32(base, kInfo)) == 1.1f);
    CHECK(a.call("cellVideoOutGetGamma", 1, kInfo) == kVideoUnsupportedOut);
    CHECK(a.call("cellVideoOutGetScreenSize", 0, kInfo) == r3_of_code(0x8002b228));
    CHECK(a.call("cellVideoOutSetCopyControl", 0, 2) == 0);
    CHECK(a.call("cellVideoOutSetCopyControl", 0, 3) == kVideoIllegal);
    CHECK(a.call("cellVideoOutSetXVColor", 1, 0, 0) == r3_of_code(0x8002b220));
    CHECK(a.call("cellVideoOutSetupDisplay", 0) == kVideoUnsupportedOut);
    CHECK(a.call("cellVideoOutSetupDisplay", 1) == 0);
    CHECK(a.call("cellVideoOutConfigure2") == 0);

    // Audio out: the primary device's info and modes.
    CHECK(a.call("cellAudioOutGetNumberOfDevice", 0) == 1);
    CHECK(a.call("cellAudioOutGetNumberOfDevice", 1) == 0);
    CHECK(a.call("cellAudioOutGetNumberOfDevice", 2) == kAudioIllegal);
    std::memset(base + kInfo, 0xAA, 136);
    REQUIRE(a.call("cellAudioOutGetDeviceInfo", 0, 0, kInfo) == 0);
    CHECK(ps_read8(base, kInfo + 0) == 0);  // HDMI
    CHECK(ps_read8(base, kInfo + 1) == 1);  // one mode
    CHECK(ps_read8(base, kInfo + 2) == 1);  // available
    CHECK(ps_read16(base, kInfo + 6) == 13); // latency
    CHECK(ps_read8(base, kInfo + 8 + 1) == 2); // 2 ch
    CHECK(ps_read8(base, kInfo + 8 + 2) == 4); // 48 kHz
    CHECK(all_bytes(base + kInfo + 16, 120, 0));
    CHECK(a.call("cellAudioOutGetDeviceInfo", 0, 1, kInfo) == kAudioOutOfRange);
    REQUIRE(a.call("cellAudioOutGetDeviceInfo", 1, 0, kInfo) == 0);
    CHECK(all_bytes(base + kInfo, 136, 0));
    CHECK(a.call("cellAudioOutGetDeviceInfo", 0, 0, 0) == kAudioIllegal);
    CHECK(a.call("cellAudioOutGetSoundAvailability2", 0, 0, 4, 2, 0) == 2);
    CHECK(a.call("cellAudioOutGetSoundAvailability2", 0, 0, 4, 6, 0) == 0);
    CHECK(a.call("cellAudioOutGetSoundAvailability2", 1, 0, 4, 2, 0) == 0);
    CHECK(a.call("cellAudioOutRegisterDevice", 1, kInfo, 0, 0) == 0);
    CHECK(a.call("cellAudioOutRegisterDevice", 1, 0, 0, 0) == r3_of_code(0x8002b262));
    CHECK(a.call("cellAudioOutSetDeviceMode", 3) == kAudioIllegal);
    CHECK(a.call("cellAudioOutGetDeviceInfo2", 0, 1, kInfo) == kAudioIllegal);

    // Audio in: no microphone. Every entry asked for is disconnected (device number 0xff).
    const u64 kInIllegal = r3_of_code(0x8002b262), kInNotFound = r3_of_code(0x8002b264);
    std::memset(base + kInfo, 0xAA, 2 * 224);
    CHECK(a.call("cellAudioInGetAvailableDeviceInfo", 2, kInfo) == 0);
    CHECK(ps_read8(base, kInfo + 2) == 0);
    CHECK(ps_read8(base, kInfo + 3) == 0xff);
    CHECK(ps_read8(base, kInfo + 224 + 3) == 0xff);
    CHECK(a.call("cellAudioInGetAvailableDeviceInfo", 17, kInfo) == kInIllegal);
    CHECK(a.call("cellAudioInGetDeviceInfo", 0, 0, kInfo) == kInNotFound);
    CHECK(a.call("cellAudioInGetDeviceInfo", 0, 1, kInfo) == kInIllegal);
    ps_write8(base, kConfig, 6);
    CHECK(a.call("cellAudioInRegisterDevice", 1, kInfo, 0, kConfig) == kInIllegal); // volume above 5
    ps_write8(base, kConfig, 3);
    CHECK(a.call("cellAudioInRegisterDevice", 1, kInfo, 0, kConfig) == kInNotFound);
    CHECK(a.call("cellAudioInSetDeviceMode", 3) == kInIllegal);
    CHECK(a.call("cellAudioInSetDeviceMode", 10) == 0);
    CHECK(a.call("cellAudioInUnregisterDevice", 0) == 0);
}
