// The cellSysutil, sys_fs and sys_io functions Assassin's Creed imports beyond GTA V's (synthetic: no game data): the
// message dialog answered at once without a UI, the HDD game and game data checks with a guest funcStat, the boot
// disc's information and the disc change callbacks, the audio copy control; cellFs's fstat, free space, stream read
// finish and asynchronous reads on their own PPU thread; cellPad's old info, capability info and buffer clearing.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "guest.hpp"   // runtime/hle/src (internal): atomic_load_be32
#include "psf.hpp"     // runtime/hle/src (internal): the PARAM.SFO reader and writer
#include "sysutil.hpp" // runtime/hle/src (internal): the sysutil event statuses
#include "hle_test_util.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;
namespace psf = pspocket::hle::psf;

namespace {

const u64 kMsgParam = r3_of_code(0x8002b301);         // CELL_MSGDIALOG_ERROR_PARAM
const u64 kSysutilBusy = r3_of_code(0x8002b105);      // CELL_SYSUTIL_ERROR_BUSY
const u64 kAudioIllegal = r3_of_code(0x8002b242);     // CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER
const u64 kAudioUnsupported = r3_of_code(0x8002b245); // CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT
const u64 kGameDataCbResult = r3_of_code(0x8002b601);
const u64 kGameDataParam = r3_of_code(0x8002b604);
const u64 kGameDataBroken = r3_of_code(0x8002b606);
const u64 kHddCbResult = r3_of_code(0x8002ba01);
const u64 kHddParam = r3_of_code(0x8002ba04);
const u64 kNotDiscBoot = r3_of_code(0x8002bd02);
const u64 kDiscParam = r3_of_code(0x8002bd03);
constexpr u64 kPadInval = 0xFFFFFFFF80121102ull, kPadUninit = 0xFFFFFFFF80121104ull,
              kPadNoDevice = 0xFFFFFFFF80121107ull;
constexpr s32 kHddFreeSizeKB = 40 * 1024 * 1024 - 256;
constexpr u64 kGiB = 1ull << 30;

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// The log lines written while it lives.
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

    usize count(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        usize n = 0;
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) n++;
        return n;
    }
};

// Guest memory, in the fixture's committed area below the test threads' stacks (kStacks).
constexpr u32 kToc = 0x00123400;
constexpr u32 kEventCode = kData + 0x10000, kDialogCode = kData + 0x10100, kStatCode = kData + 0x10200,
              kAioCode = kData + 0x10300;
constexpr u32 kEventOpd = kData + 0x10800, kDialogOpd = kData + 0x10808, kStatOpd = kData + 0x10810,
              kAioOpd = kData + 0x10818;
constexpr u32 kLog = kData + 0x11000;    // the recorders' log: count at +0, 16-byte entries from +16
constexpr u32 kAioLog = kData + 0x12000; // the AIO callback's log: count at +0, 32-byte entries from +32
constexpr u32 kCtl = kData + 0x13000;    // funcStat's script and record
constexpr u32 kStrings = kData + 0x20000;
constexpr u32 kSetParam = kData + 0x30000; // a CellGameDataSystemFileParam (0xB9C bytes)
constexpr u32 kOut = kData + 0x40000;
constexpr u32 kBuf = kData + 0x50000;
constexpr u32 kEnd = kData + 0x300000;  // the end of the fixture's committed memory
constexpr u32 kUnmapped = 0x0FFF0000;   // in the main area, never committed by these tests

s32 hi(u32 v) { return static_cast<s32>(v >> 16); }
u32 lo(u32 v) { return v & 0xFFFF; }

void write_code(u8* base, u32 at, const std::vector<u32>& code)
{
    for (usize i = 0; i < code.size(); i++) ps_write32(base, at + 4 * static_cast<u32>(i), code[i]);
}

void write_opd(u8* base, u32 at, u32 code)
{
    ps_write32(base, at, code);
    ps_write32(base, at + 4, kToc);
}

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

std::string str(u8* base, u32 addr) { return std::string(reinterpret_cast<const char*>(base + addr)); }

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

// A guest callback that appends {tag, r3, r4, r5} (low words) to the log at kLog. The sysutil callback(status, param,
// userdata) records its three arguments, the message dialog's callback(button, userData) two and a 0.
constexpr u32 kTagEvent = 1, kTagDialog = 2;

void write_recorder(u8* base, u32 code, u32 opd, u32 tag)
{
    std::vector<u32> c = {
        as::lis(6, hi(kLog)), as::ori(6, 6, lo(kLog)),
        as::lwz(7, 0, 6), as::slwi(8, 7, 4), as::addi(8, 8, 16), as::add(8, 8, 6), // r8 = the next entry
        as::li(9, static_cast<s32>(tag)), as::stw(9, 0, 8),
        as::stw(3, 4, 8), as::stw(4, 8, 8), as::stw(5, 12, 8),
        as::addi(7, 7, 1), as::stw(7, 0, 6),
        as::blr(),
    };
    if (tag == kTagDialog) c.insert(c.begin(), as::li(5, 0));
    write_code(base, code, c);
    write_opd(base, opd, code);
}

struct Entry {
    u32 tag = 0, a = 0, b = 0, c = 0;
    bool operator==(const Entry&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Entry& e)
{
    return os << std::hex << "{tag " << e.tag << ", 0x" << e.a << ", 0x" << e.b << ", 0x" << e.c << "}" << std::dec;
}

std::vector<Entry> entries(u8* base)
{
    std::vector<Entry> v;
    for (u32 i = 0; i < ps_read32(base, kLog); i++) {
        const u32 e = kLog + 16 + 16 * i;
        v.push_back({ps_read32(base, e), ps_read32(base, e + 4), ps_read32(base, e + 8), ps_read32(base, e + 12)});
    }
    return v;
}

// funcStat(cbResult, get, set) for the game checks: records its arguments at kCtl (+0, +4, +8) and counts its calls
// (+24); writes the result at kCtl + 12 to cbResult->result; when kCtl + 16 is non-zero, sets set->setParam to the
// word at kCtl + 20.
void write_func_stat(u8* base)
{
    write_code(base, kStatCode, {
        as::lis(6, hi(kCtl)), as::ori(6, 6, lo(kCtl)),
        as::stw(3, 0, 6), as::stw(4, 4, 6), as::stw(5, 8, 6),
        as::lwz(7, 12, 6), as::stw(7, 0, 3),
        as::lwz(7, 24, 6), as::addi(7, 7, 1), as::stw(7, 24, 6),
        as::lwz(7, 16, 6), as::cmpwi(0, 7, 0), as::bclr(12, 2), // beqlr: setParam stays
        as::lwz(7, 20, 6), as::stw(7, 0, 5),
        as::blr(),
    });
    write_opd(base, kStatOpd, kStatCode);
    std::memset(base + kCtl, 0, 0x40);
}

// The next funcStat answer: `result`, and setParam replaced by `set_param` when `replace` is set.
void script(u8* base, s32 result, bool replace = false, u32 set_param = 0)
{
    ps_write32(base, kCtl + 12, static_cast<u32>(result));
    ps_write32(base, kCtl + 16, replace ? 1 : 0);
    ps_write32(base, kCtl + 20, set_param);
}

u32 stat_calls(u8* base) { return ps_read32(base, kCtl + 24); }
u32 stat_result(u8* base) { return ps_read32(base, kCtl + 0); }
u32 stat_get(u8* base) { return ps_read32(base, kCtl + 4); }
u32 stat_set(u8* base) { return ps_read32(base, kCtl + 8); }

// A PARAM.SFO with the given entries: strings of the given size, or integers (size 0).
struct SfoEntry {
    std::string key, text;
    u32 max = 0; // 0: an integer entry
    u32 integer = 0;
};

std::vector<u8> make_sfo(const std::vector<SfoEntry>& entries)
{
    psf::Registry r;
    for (const SfoEntry& e : entries) {
        r[e.key] = e.max ? psf::string(e.max, e.text) : psf::Entry{psf::Format::Integer, 4, {}, e.integer};
    }
    return psf::save(r);
}

void write_file(const fsys::path& path, const std::vector<u8>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> read_file(const fsys::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fsys::path temp_root(std::string_view what)
{
    const fsys::path root = fsys::temp_directory_path() / std::format("pspocket-hle-ac-{}-{}", what, ::getpid());
    fsys::remove_all(root);
    fsys::create_directories(root);
    return root;
}

// CellGameDataStatGet and CellGameDataSystemFileParam offsets (cellGame.h).
constexpr u32 kGetIsNewData = 0x4, kGetContentInfoPath = 0x8, kGetGameDataPath = 0x427, kGetParam = 0x860,
              kGetSizeKB = 0x13FC, kGetSysSizeKB = 0x1400;
constexpr u32 kParamTitleLang = 0x80, kParamTitleId = 0xA80, kParamDataVersion = 0xA8C, kParamParentalLevel = 0xA94,
              kParamAttribute = 0xA98, kParamResolution = 0xA9C, kParamSoundFormat = 0xAA0, kParamSize = 0xB9C;

} // namespace

TEST_CASE("cellSysutil, sys_fs, sys_io: the functions Assassin's Creed adds, in their modules", "[hle][ac]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
        bool internal;
    };
    const Expected expected[] = {
        {"cellSysutil", "cellMsgDialogOpen2", 0x7603D3DB, false},
        {"cellSysutil", "cellHddGameCheck", 0x9117DF20, false},
        {"cellSysutil", "cellDiscGameGetBootDiscInfo", 0xB72BC4E6, false},
        {"cellSysutil", "cellGameDataCheckCreate2", 0xC9645C41, false},
        {"cellSysutil", "cellAudioOutSetCopyControl", 0xC96E89E9, false},
        {"cellSysutil", "cellDiscGameRegisterDiscChangeCallback", 0xDFDD302E, false},
        {"sys_fs", "cellFsAioInit", 0xDB869F20, false},
        {"sys_fs", "cellFsAioRead", 0xC1C507E7, false},
        {"sys_fs", "cellFsAioCancel", 0x7F13FC8C, false},
        {"sys_fs", "cellFsAioFinish", 0x9F951810, false},
        {"sys_fs", "cellFsStReadFinish", 0xD73938DF, false},
        {"sys_fs", "cellFsFstat", 0xEF3EFA34, false},
        {"sys_fs", "cellFsGetFreeSize", 0xAA3B4BCD, false},
        {"sys_fs", "_pspocket_fs_aio_thread", 0xFFB98633, true}, // the AIO thread's entry
        {"sys_io", "cellPadClearBuf", 0x0D5F2C14, false},
        {"sys_io", "cellPadGetInfo", 0x3AAAD464, false},
        {"sys_io", "cellPadGetCapabilityInfo", 0xDBF4C59C, false},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
        CHECK(fn->internal == e.internal);
    }
}

// ------------------------------------------------------------------------------------------------ cellSysutil

TEST_CASE("cellMsgDialogOpen2: RPCS3's checks; with no UI a dialog with buttons is answered at once",
    "[hle][ac][msgdialog]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    write_recorder(base, kEventCode, kEventOpd, kTagEvent);
    write_recorder(base, kDialogCode, kDialogOpd, kTagDialog);
    std::memset(base + kLog, 0, 0x400);
    REQUIRE(a.call("cellSysutilRegisterCallback", 0, kEventOpd, 0x5150) == 0);
    constexpr u32 kMsg = kStrings, kLongMsg = kStrings + 0x100;
    put(base, kMsg, "Saving.");
    const auto open = [&](u32 type, u32 msg, u32 callback = kDialogOpd, u32 userdata = 0xD1A1) {
        return a.call("cellMsgDialogOpen2", type, msg, callback, userdata, 0);
    };
    const auto check = [&] { REQUIRE(a.call("cellSysutilCheckCallback") == 0); };
    const Entry begin{kTagEvent, static_cast<u32>(hle::CELL_SYSUTIL_DRAWING_BEGIN), 0, 0x5150};
    const Entry end{kTagEvent, static_cast<u32>(hle::CELL_SYSUTIL_DRAWING_END), 0, 0x5150};
    const auto answer = [](u32 button, u32 userdata) { return Entry{kTagDialog, button, userdata, 0}; };
    constexpr u32 kOk = 0x21, kYesNo = 0x11, kNone = 0x01; // SE_TYPE_NORMAL with each button type

    // RPCS3's checks, in its order: the message, the type's bits, then what each button type allows.
    CHECK(open(kOk, 0) == kMsgParam);
    CHECK(open(kOk | 0x8, kMsg) == kMsgParam);   // a bit outside the fields
    CHECK(open(kOk | 0x400, kMsg) == kMsgParam);
    CHECK(open(0x31, kMsg) == kMsgParam);        // button type 3
    CHECK(open(kOk | 0x100, kMsg) == kMsgParam); // OK with the NO cursor
    CHECK(open(kOk | 0x1000, kMsg) == kMsgParam); // OK with a progress bar
    CHECK(open(kYesNo | 0x200, kMsg) == kMsgParam);
    CHECK(open(kYesNo | 0x1000, kMsg) == kMsgParam);
    CHECK(open(kNone | 0x100, kMsg) == kMsgParam);
    CHECK(open(kNone | 0x3000, kMsg) == kMsgParam); // three progress bars
    std::memset(base + kLongMsg, 'A', 512);         // no terminator within 512 bytes
    base[kLongMsg + 512] = 0;
    CHECK(open(kOk, kLongMsg) == kMsgParam);
    // Not guest memory (RPCS3 faults): the message, one that runs out of it, the callback.
    CHECK(open(kOk, kUnmapped) == kMsgParam);
    ps_write32(base, kEnd - 4, 0x41424344);
    CHECK(open(kOk, kEnd - 4) == kMsgParam);
    CHECK(open(kOk, kMsg, kUnmapped) == kMsgParam);
    check();
    CHECK(entries(base).empty()); // none of them opened a dialog

    // An OK dialog: DRAWING_BEGIN reaches the game at its next check, which also answers OK; DRAWING_END and the
    // callback(OK, userData) follow at the check after.
    REQUIRE(open(kOk, kMsg) == 0);
    CHECK(open(kOk, kMsg) == kSysutilBusy); // one at a time
    CHECK(entries(base).empty());
    check();
    CHECK(entries(base) == std::vector<Entry>{begin});
    check();
    CHECK(entries(base) == std::vector<Entry>{begin, end, answer(1, 0xD1A1)});
    check();
    CHECK(entries(base).size() == 3);

    // YES/NO: the default cursor's button. An error dialog (SE type 0) and a 511-byte message work the same.
    std::memset(base + kLog, 0, 0x400);
    REQUIRE(open(kYesNo, kMsg, kDialogOpd, 1) == 0);
    check();
    check();
    REQUIRE(open(kYesNo | 0x100, kMsg, kDialogOpd, 2) == 0);
    check();
    check();
    base[kLongMsg + 511] = 0;
    REQUIRE(open(0x20, kLongMsg, kDialogOpd, 3) == 0);
    check();
    check();
    CHECK(entries(base) == std::vector<Entry>{begin, end, answer(1, 1), begin, end, answer(2, 2), begin, end,
                               answer(1, 3)});

    // Without a callback the dialog closes all the same.
    std::memset(base + kLog, 0, 0x400);
    REQUIRE(open(kOk, kMsg, 0, 0) == 0);
    check();
    check();
    CHECK(entries(base) == std::vector<Entry>{begin, end});

    // A dialog without buttons has nothing to answer: it stays open until the game aborts it, without its callback.
    std::memset(base + kLog, 0, 0x400);
    REQUIRE(open(kNone | 0x1000, kMsg) == 0); // one progress bar
    check();
    check();
    CHECK(entries(base) == std::vector<Entry>{begin});
    CHECK(open(kOk, kMsg) == kSysutilBusy);
    REQUIRE(a.call("cellMsgDialogAbort") == 0);
    check();
    check();
    CHECK(entries(base) == std::vector<Entry>{begin, end});

    // Aborted before its answer: no callback. The answer queued for it finds the next dialog, which is not its own,
    // and leaves it alone; that one gets its own answer.
    std::memset(base + kLog, 0, 0x400);
    REQUIRE(open(kOk, kMsg, kDialogOpd, 0xAB) == 0);
    REQUIRE(a.call("cellMsgDialogAbort") == 0);
    REQUIRE(open(kOk, kMsg, kDialogOpd, 0xCD) == 0);
    check();
    CHECK(entries(base) == std::vector<Entry>{begin, end, begin});
    check();
    CHECK(entries(base) == std::vector<Entry>{begin, end, begin, end, answer(1, 0xCD)});
}

TEST_CASE("cellAudioOutSetCopyControl: three settings, the primary output only", "[hle][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    for (const u32 control : {0u, 1u, 2u}) CHECK(a.call("cellAudioOutSetCopyControl", 0, control) == 0);
    CHECK(a.call("cellAudioOutSetCopyControl", 0, 3) == kAudioIllegal);
    CHECK(a.call("cellAudioOutSetCopyControl", 1, 3) == kAudioIllegal); // the setting is checked first
    CHECK(a.call("cellAudioOutSetCopyControl", 1, 0) == kAudioUnsupported);
    CHECK(a.call("cellAudioOutSetCopyControl", 2, 0) == kAudioIllegal);
}

// ------------------------------------------------------------------------------------- cellSysutil's game data

TEST_CASE("cellHddGameCheck: RPCS3's checks, funcStat's structures and its answers", "[hle][ac][game]")
{
    InterpretMisses interpret;
    const fsys::path root = temp_root("hddgame");
    fsys::create_directories(root / "game");
    const fsys::path dir = root / "game" / "BLES00158";
    Fixture f;
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    GuestThread a(f, 0, "a");
    write_func_stat(base);
    constexpr u32 kName = kStrings, kBadName = kStrings + 0x40;
    put(base, kName, "BLES00158");
    const auto check = [&](u32 version, u32 name, u32 err_dialog, u32 func) {
        return a.call("cellHddGameCheck", version, name, err_dialog, func, 0xFFFFFFFF);
    };

    // The arguments: the version, the name and funcStat, the name's characters and length (9 for an HDD game).
    CHECK(check(1, kName, 0, kStatOpd) == kHddParam);
    CHECK(check(0, 0, 0, kStatOpd) == kHddParam);
    CHECK(check(0, kName, 0, 0) == kHddParam);
    CHECK(check(0, kUnmapped, 0, kStatOpd) == kHddParam);
    CHECK(check(0, kName, 0, kUnmapped) == kHddParam);
    // Lower case, '_' first, 8 and 10 characters, 32 (the most, taken as a name here), 33 (too long).
    for (const std::string_view bad : {"bles00158", "_BLES0015", "BLES0015", "BLES00158X",
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456"}) {
        INFO(bad);
        put(base, kBadName, bad);
        CHECK(check(0, kBadName, 0, kStatOpd) == kHddParam);
    }
    CHECK(stat_calls(base) == 0);

    // No directory: NODIR, the paths, 40 GB free less 256 KB, the size not calculated, getParam empty; setParam
    // points at a zeroed parameter block. OK leaves the disk alone (RPCS3).
    script(base, 0);
    REQUIRE(check(0, kName, 0, kStatOpd) == 0);
    REQUIRE(stat_calls(base) == 1);
    const u32 get = stat_get(base), set = stat_set(base);
    CHECK(ps_read32(base, get) == static_cast<u32>(kHddFreeSizeKB));
    CHECK(ps_read32(base, get + kGetIsNewData) == 1);
    CHECK(str(base, get + kGetContentInfoPath) == "/dev_hdd0/game/BLES00158");
    CHECK(str(base, get + kGetGameDataPath) == "/dev_hdd0/game/BLES00158/USRDIR");
    CHECK(static_cast<s32>(ps_read32(base, get + kGetSizeKB)) == -1);
    CHECK(ps_read32(base, get + kGetSysSizeKB) == 0);
    CHECK(all_bytes(base + get + kGetParam, kParamSize, 0));
    const u32 file_param = ps_read32(base, set);
    REQUIRE(file_param != 0);
    CHECK(all_bytes(base + file_param, kParamSize, 0));
    CHECK(ps_read32(base, stat_result(base)) == 0);
    CHECK_FALSE(fsys::exists(dir));
    // The structures are cellGame's variables under RPCS3's names for them (D-567).
    const auto var = [&](std::string_view name) {
        return f.hle->link_variable("cellGame", recomp::nid_of(name));
    };
    CHECK(get == var("g_stat_get"));
    CHECK(set == var("g_stat_set"));
    CHECK(stat_result(base) == var("g_cb_result"));
    CHECK(file_param == var("g_file_param"));

    // New data that funcStat answers OK without a setParam: CELL_GAMEDATA_ERROR_PARAM (RPCS3's code). OK_CANCEL is
    // CELL_OK, an error result CELL_HDDGAME_ERROR_CBRESULT; the error dialog errDialog asks for is logged.
    script(base, 0, true, 0);
    CHECK(check(0, kName, 0, kStatOpd) == kGameDataParam);
    script(base, 1);
    CHECK(check(0, kName, 0, kStatOpd) == 0);
    {
        CapturedLog logs;
        script(base, -3); // ERR_BROKEN
        CHECK(check(0, kName, 1, kStatOpd) == kHddCbResult);
        CHECK(logs.count(log::Level::Error, "cellGame", "error dialog (not shown): \"HDD boot game BLES00158 is "
                                                         "corrupt!\"") == 1);
        script(base, -1); // ERR_NOSPACE
        CHECK(check(0, kName, 0, kStatOpd) == kHddCbResult);
        CHECK(logs.count(log::Level::Error, "cellGame", "error dialog") == 1);
    }
    script(base, -42);
    CHECK(check(0, kName, 0, kStatOpd) == kHddCbResult);

    // The directory with an HDD game's PARAM.SFO: EXIST, and getParam from it (dataVersion from APP_VER).
    fsys::create_directories(dir);
    write_file(dir / "PARAM.SFO",
        make_sfo({{"APP_VER", "01.02", 8}, {"ATTRIBUTE", "", 0, 0x20}, {"CATEGORY", "HG", 4},
            {"PARENTAL_LEVEL", "", 0, 5}, {"RESOLUTION", "", 0, 0x3F}, {"SOUND_FORMAT", "", 0, 0x1},
            {"TITLE", "Test HDD Game", 128}, {"TITLE_01", "Test HDD Game (EN)", 128}, {"TITLE_ID", "BLES00158", 16},
            {"VERSION", "01.00", 8}}));
    script(base, 0, true, 0); // not new: setParam isn't needed
    REQUIRE(check(0, kName, 0, kStatOpd) == 0);
    const u32 param = get + kGetParam;
    CHECK(ps_read32(base, get + kGetIsNewData) == 0);
    CHECK(str(base, param) == "Test HDD Game");
    CHECK(str(base, param + kParamTitleLang) == "");
    CHECK(str(base, param + kParamTitleLang + 128) == "Test HDD Game (EN)");
    CHECK(str(base, param + kParamTitleId) == "BLES00158");
    CHECK(str(base, param + kParamDataVersion) == "01.02");
    CHECK(ps_read32(base, param + kParamParentalLevel) == 5);
    CHECK(ps_read32(base, param + kParamAttribute) == 0x20);
    CHECK(ps_read32(base, param + kParamResolution) == 0x3F);
    CHECK(ps_read32(base, param + kParamSoundFormat) == 0x1);

    // A PARAM.SFO that isn't an HDD game's, or not a PARAM.SFO: CELL_GAMEDATA_ERROR_BROKEN, without funcStat.
    const u32 calls = stat_calls(base);
    write_file(dir / "PARAM.SFO", make_sfo({{"CATEGORY", "GD", 4}, {"TITLE", "Game data", 128}}));
    CHECK(check(0, kName, 0, kStatOpd) == kGameDataBroken);
    write_file(dir / "PARAM.SFO", make_sfo({{"CATEGORY", "ZZ", 4}})); // an unknown category (RPCS3 psf::load)
    CHECK(check(0, kName, 0, kStatOpd) == kGameDataBroken);
    write_file(dir / "PARAM.SFO", {'n', 'o', 't', ' ', 'a', ' ', 'p', 's', 'f'});
    CHECK(check(0, kName, 0, kStatOpd) == kGameDataBroken);
    CHECK(stat_calls(base) == calls);

    // The directory without a PARAM.SFO: EXIST, yet new data, so OK needs setParam.
    fsys::remove(dir / "PARAM.SFO");
    script(base, 0, true, 0);
    CHECK(check(0, kName, 0, kStatOpd) == kGameDataParam);
    CHECK(ps_read32(base, get + kGetIsNewData) == 0);
    fsys::remove_all(root);
}

TEST_CASE("cellGameDataCheckCreate2: game data made with funcStat's setParam, then found and updated",
    "[hle][ac][game]")
{
    InterpretMisses interpret;
    const fsys::path root = temp_root("gamedata");
    fsys::create_directories(root / "game");
    const fsys::path dir = root / "game" / "BLES00158SAVE";
    Fixture f;
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    GuestThread a(f, 0, "a");
    write_func_stat(base);
    constexpr u32 kName = kStrings;
    put(base, kName, "BLES00158SAVE");
    const auto check = [&](u32 version, u32 name, u32 func) {
        return a.call("cellGameDataCheckCreate2", version, name, 0, func, 0xFFFFFFFF);
    };
    const auto sfo = [&] {
        auto parsed = psf::parse(read_file(dir / "PARAM.SFO"));
        REQUIRE(parsed);
        return *parsed;
    };
    // setParam: a title, a second localized title, the title ID, the version, the parental level.
    std::memset(base + kSetParam, 0, kParamSize);
    put(base, kSetParam, "Game Data");
    put(base, kSetParam + kParamTitleLang + 128, "Game Data (EN)");
    put(base, kSetParam + kParamTitleId, "BLES00158");
    put(base, kSetParam + kParamDataVersion, "01.00");
    ps_write32(base, kSetParam + kParamParentalLevel, 3);

    CHECK(check(1, kName, kStatOpd) == kGameDataParam);
    CHECK(check(0, 0, kStatOpd) == kGameDataParam);
    CHECK(check(0, kName, 0) == kGameDataParam);
    put(base, kStrings + 0x40, "lower");
    CHECK(check(0, kStrings + 0x40, kStatOpd) == kGameDataParam);
    CHECK(stat_calls(base) == 0);

    // New game data: isNewData YES, no setParam. OK without one: CELL_GAMEDATA_ERROR_PARAM; OK_CANCEL: CELL_OK. Neither
    // creates anything.
    script(base, 0);
    CHECK(check(0, kName, kStatOpd) == kGameDataParam);
    const u32 get = stat_get(base), set = stat_set(base);
    CHECK(ps_read32(base, get) == static_cast<u32>(kHddFreeSizeKB));
    CHECK(ps_read32(base, get + kGetIsNewData) == 1);
    CHECK(str(base, get + kGetContentInfoPath) == "/dev_hdd0/game/BLES00158SAVE");
    CHECK(str(base, get + kGetGameDataPath) == "/dev_hdd0/game/BLES00158SAVE/USRDIR");
    CHECK(static_cast<s32>(ps_read32(base, get + kGetSizeKB)) == -1);
    CHECK(all_bytes(base + get + kGetParam, kParamSize, 0));
    CHECK(ps_read32(base, set) == 0);
    script(base, 1);
    CHECK(check(0, kName, kStatOpd) == 0);
    CHECK_FALSE(fsys::exists(dir));

    // OK with setParam: USRDIR and a PARAM.SFO of category GD with setParam's fields (empty localized titles left out).
    script(base, 0, true, kSetParam);
    REQUIRE(check(0, kName, kStatOpd) == 0);
    CHECK(fsys::is_directory(dir / "USRDIR"));
    psf::Registry r = sfo();
    CHECK(psf::get_string(r, "CATEGORY") == "GD");
    CHECK(psf::get_string(r, "TITLE") == "Game Data");
    CHECK(psf::get_string(r, "TITLE_01") == "Game Data (EN)");
    CHECK_FALSE(r.contains("TITLE_00"));
    CHECK(psf::get_string(r, "TITLE_ID") == "BLES00158");
    CHECK(r.at("TITLE_ID").max == 16);
    CHECK(psf::get_string(r, "VERSION") == "01.00");
    REQUIRE(r.at("PARENTAL_LEVEL").format == psf::Format::Integer);
    CHECK(r.at("PARENTAL_LEVEL").integer == 3);

    // Found: isNewData NO and getParam from the PARAM.SFO (attribute NORMAL, the version from VERSION: no APP_VER).
    script(base, 0);
    REQUIRE(check(0, kName, kStatOpd) == 0);
    const u32 param = get + kGetParam;
    CHECK(ps_read32(base, get + kGetIsNewData) == 0);
    CHECK(str(base, param) == "Game Data");
    CHECK(str(base, param + kParamTitleLang + 128) == "Game Data (EN)");
    CHECK(str(base, param + kParamTitleId) == "BLES00158");
    CHECK(str(base, param + kParamDataVersion) == "01.00");
    CHECK(ps_read32(base, param + kParamParentalLevel) == 3);
    CHECK(ps_read32(base, param + kParamAttribute) == 0);
    CHECK(ps_read32(base, set) == 0);

    // An update through setParam: the new title, the category kept.
    put(base, kSetParam, "Game Data 2");
    script(base, 0, true, kSetParam);
    REQUIRE(check(0, kName, kStatOpd) == 0);
    CHECK(psf::get_string(sfo(), "TITLE") == "Game Data 2");
    CHECK(psf::get_string(sfo(), "CATEGORY") == "GD");
    // A title filling all 128 bytes doesn't fit its entry: RPCS3 writes nothing and returns CELL_OK.
    std::memset(base + kSetParam, 'T', 128);
    REQUIRE(check(0, kName, kStatOpd) == 0);
    CHECK(psf::get_string(sfo(), "TITLE") == "Game Data 2");
    // A setParam outside guest memory (RPCS3 faults).
    script(base, 0, true, kUnmapped);
    CHECK(check(0, kName, kStatOpd) == kGameDataParam);

    // An error result: CELL_GAMEDATA_ERROR_CBRESULT. A PARAM.SFO that isn't game data's: CELL_GAMEDATA_ERROR_BROKEN.
    script(base, -4); // ERR_NODATA
    CHECK(check(0, kName, kStatOpd) == kGameDataCbResult);
    const u32 calls = stat_calls(base);
    write_file(dir / "PARAM.SFO", make_sfo({{"CATEGORY", "HG", 4}}));
    CHECK(check(0, kName, kStatOpd) == kGameDataBroken);
    CHECK(stat_calls(base) == calls);
    // A disc game's (DG) is accepted.
    write_file(dir / "PARAM.SFO", make_sfo({{"CATEGORY", "DG", 4}, {"TITLE", "Disc", 128}}));
    script(base, 1);
    CHECK(check(0, kName, kStatOpd) == 0);
    fsys::remove_all(root);
}

TEST_CASE("cellDiscGameGetBootDiscInfo, cellDiscGameRegisterDiscChangeCallback: the boot disc", "[hle][ac][game]")
{
    const fsys::path root = temp_root("disc");
    fsys::create_directories(root / "bdvd" / "PS3_GAME");
    hle::ProcessConfig config;
    config.game.title_id = "BLES00158";
    config.game.param_sfo = make_sfo({{"CATEGORY", "DG", 4}, {"PARENTAL_LEVEL", "", 0, 5},
        {"TITLE", "Test Game", 128}, {"TITLE_ID", "BLES00158XY", 16}});
    Fixture f(config);
    u8* base = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kInfo = kOut; // CellDiscGameSystemFileParam: titleId[10], reserved0[2], parentalLevel, reserved1[16]

    CHECK(a.call("cellDiscGameGetBootDiscInfo", 0) == kDiscParam);
    CHECK(a.call("cellDiscGameGetBootDiscInfo", kUnmapped) == kDiscParam);
    // No disc: NOT_DISCBOOT, with titleId's first word cleared all the same.
    std::memset(base + kInfo, 0xAA, 32);
    CHECK(a.call("cellDiscGameGetBootDiscInfo", kInfo) == kNotDiscBoot);
    CHECK(ps_read32(base, kInfo) == 0);
    CHECK(all_bytes(base + kInfo + 4, 28, 0xAA));
    // Registering the disc change callbacks takes anything, null ones too.
    CHECK(a.call("cellDiscGameRegisterDiscChangeCallback", kEventOpd, kDialogOpd) == 0);

    // The disc: the title ID (cut to 9 characters and a terminator) and the parental level; the rest untouched.
    REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (root / "bdvd").string(), true));
    std::memset(base + kInfo, 0xAA, 32);
    REQUIRE(a.call("cellDiscGameGetBootDiscInfo", kInfo) == 0);
    CHECK(str(base, kInfo) == "BLES00158");
    CHECK(all_bytes(base + kInfo + 10, 2, 0xAA));
    CHECK(ps_read32(base, kInfo + 12) == 5);
    CHECK(all_bytes(base + kInfo + 16, 16, 0xAA));
    CHECK(a.call("cellDiscGameRegisterDiscChangeCallback", 0, 0) == 0);
    fsys::remove_all(root);
}

// ------------------------------------------------------------------------------------------------- sys_fs

TEST_CASE("cellFsFstat, cellFsStReadFinish, cellFsGetFreeSize", "[hle][ac][fs]")
{
    const fsys::path root = temp_root("fs");
    for (const char* d : {"hdd0/game/TEST00000", "hdd1", "bdvd/PS3_GAME", "app_home"}) {
        fsys::create_directories(root / d);
    }
    write_file(root / "hdd0" / "game" / "TEST00000" / "data.bin", {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'});
    Fixture f;
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    REQUIRE(f.kernel->vfs_mount("/dev_hdd1", (root / "hdd1").string(), false));
    REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (root / "bdvd").string(), true));
    REQUIRE(f.kernel->vfs_mount("/app_home", (root / "app_home").string(), false));
    GuestThread a(f, 0, "a");
    constexpr u32 kPath = kStrings, kFd = kOut, kStat = kOut + 0x100, kBlockSize = kOut + 0x200,
                  kBlockCount = kOut + 0x208;
    const u64 kEbadf = r3_of(lv2::CELL_EBADF), kEfault = r3_of(lv2::CELL_EFAULT);

    put(base, kPath, "/dev_hdd0/game/TEST00000/data.bin");
    REQUIRE(a.call("cellFsOpen", kPath, 0, kFd, 0, 0) == 0);
    const u32 fd = ps_read32(base, kFd);
    put(base, kPath, "/dev_hdd0/game");
    REQUIRE(a.call("cellFsOpendir", kPath, kFd) == 0);
    const u32 dir_fd = ps_read32(base, kFd);

    // cellFsFstat: sys_fs_fstat's CellFsStat (size at 36, block size at 44).
    REQUIRE(a.call("cellFsFstat", fd, kStat) == 0);
    CHECK(ps_read64(base, kStat + 36) == 10);
    CHECK(ps_read64(base, kStat + 44) == 4096);
    CHECK(a.call("cellFsFstat", 99, kStat) == kEbadf);
    CHECK(a.call("cellFsFstat", dir_fd, kStat) == kEbadf);
    CHECK(a.call("cellFsFstat", fd, 0) == kEfault);

    // cellFsStReadFinish: an open file is all it asks for (RPCS3).
    CHECK(a.call("cellFsStReadFinish", fd) == 0);
    CHECK(a.call("cellFsStReadFinish", 99) == kEbadf);
    CHECK(a.call("cellFsStReadFinish", dir_fd) == kEbadf);

    // cellFsGetFreeSize.
    const auto free_size = [&](std::string_view path) {
        put(base, kPath, path);
        ps_write32(base, kBlockSize, 0xAAAAAAAA);
        ps_write64(base, kBlockCount, 0xAAAAAAAAAAAAAAAAull);
        return a.call("cellFsGetFreeSize", kPath, kBlockSize, kBlockCount);
    };
    const auto blocks = [&] { return std::pair<u32, u64>(ps_read32(base, kBlockSize), ps_read64(base, kBlockCount)); };
    CHECK(a.call("cellFsGetFreeSize", 0, kBlockSize, kBlockCount) == kEfault);
    CHECK(a.call("cellFsGetFreeSize", kPath, 0, kBlockCount) == kEfault);
    CHECK(a.call("cellFsGetFreeSize", kPath, kBlockSize, 0) == kEfault);
    // Under /dev_hdd0: 40 GB less a byte in 4 KB blocks, whether the path exists or not.
    const std::pair<u32, u64> hdd0{4096, (40 * kGiB - 1) / 4096};
    REQUIRE(free_size("/dev_hdd0") == 0);
    CHECK(blocks() == hdd0);
    REQUIRE(free_size("/dev_hdd0/game/NOT_THERE/at/all/in/a/rather/long/path") == 0);
    CHECK(blocks() == hdd0);
    // /dev_hdd1: 2 GB less a sector in its 32 KB blocks; the path must exist. Results are cleared first.
    REQUIRE(free_size("/dev_hdd1") == 0);
    CHECK(blocks() == std::pair<u32, u64>(32768, (2 * kGiB - 512) / 32768));
    CHECK(free_size("/dev_hdd1/none") == r3_of(lv2::CELL_ENOENT));
    CHECK(blocks() == std::pair<u32, u64>(0, 0));
    // A read-only device has no free space: CELL_ENOTDIR. /app_home and "/": CELL_ENOTSUP. Not mounted, empty or not
    // absolute: CELL_EINVAL. Over 31 characters: CELL_ENAMETOOLONG.
    CHECK(free_size("/dev_bdvd") == r3_of(lv2::CELL_ENOTDIR));
    CHECK(free_size("/dev_bdvd/PS3_GAME") == r3_of(lv2::CELL_ENOTDIR));
    CHECK(free_size("/app_home") == r3_of(lv2::CELL_ENOTSUP));
    CHECK(free_size("/") == r3_of(lv2::CELL_ENOTSUP));
    CHECK(free_size("/dev_usb000") == r3_of(lv2::CELL_EINVAL));
    CHECK(free_size("") == r3_of(lv2::CELL_EINVAL));
    CHECK(free_size("dev_hdd1") == r3_of(lv2::CELL_EINVAL));
    CHECK(free_size("//dev_hdd1") == r3_of(lv2::CELL_EINVAL));
    CHECK(free_size("/dev_hdd1/a_name_that_is_too_long") == r3_of(lv2::CELL_ENAMETOOLONG));
    REQUIRE(a.call("cellFsClose", fd) == 0);
    REQUIRE(a.call("cellFsClosedir", dir_fd) == 0);
    fsys::remove_all(root);
}

TEST_CASE("cellFsAio: reads complete on the AIO thread, which calls back with the bytes in place", "[hle][ac][fs]")
{
    InterpretMisses interpret;
    const fsys::path root = temp_root("aio");
    fsys::create_directories(root / "tmp");
    write_file(root / "tmp" / "data.bin",
        {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'});
    write_file(root / "tmp" / "out.bin", {});
    // What the loader and the game's CRT do first: link the module's internal functions (the AIO thread's entry),
    // initialize TLS (the thread gets a TLS block like any other).
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"sys_fs"}));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    GuestThread a(f, 0, "a");
    constexpr u32 kPath = kStrings, kMount = kStrings + 0x100, kFd = kOut, kId = kOut + 0x10, kPos = kOut + 0x20;
    constexpr u32 kAio = kOut + 0x100; // CellFsAio blocks, 0x40 apart
    put(base, kMount, "/dev_hdd0");

    // callback(aio, error, xid, size): appends {aio, error, xid, r1, size} (32 bytes) to kAioLog, then counts it.
    write_code(base, kAioCode, {
        as::lis(7, hi(kAioLog)), as::ori(7, 7, lo(kAioLog)),
        as::lwz(8, 0, 7), as::slwi(9, 8, 5), as::addi(9, 9, 32), as::add(9, 9, 7),
        as::stw(3, 0, 9), as::stw(4, 4, 9), as::stw(5, 8, 9), as::stw(1, 12, 9), as::std_(6, 16, 9),
        as::sync(), as::addi(8, 8, 1), as::stw(8, 0, 7),
        as::blr(),
    });
    write_opd(base, kAioOpd, kAioCode);
    std::memset(base + kAioLog, 0, 0x400);
    struct Done {
        u32 aio, error, xid, r1;
        u64 size;
    };
    const auto done = [&](u32 i) {
        const u32 e = kAioLog + 32 + 32 * i;
        return Done{ps_read32(base, e), ps_read32(base, e + 4), ps_read32(base, e + 8), ps_read32(base, e + 12),
            ps_read64(base, e + 16)};
    };
    const auto wait_for = [&](u32 n) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (hle::atomic_load_be32(base, kAioLog) < n && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return hle::atomic_load_be32(base, kAioLog) == n;
    };
    const auto request = [&](u32 slot, u32 fd, u64 offset, u32 buf, u64 size) {
        const u32 aio = kAio + 0x40 * slot;
        std::memset(base + aio, 0, 40);
        ps_write32(base, aio + 0, fd);
        ps_write64(base, aio + 8, offset);
        ps_write32(base, aio + 16, buf);
        ps_write64(base, aio + 24, size);
        ps_write64(base, aio + 32, 0x1234);
        return aio;
    };

    put(base, kPath, "/dev_hdd0/tmp/data.bin");
    REQUIRE(a.call("cellFsOpen", kPath, 0, kFd, 0, 0) == 0); // read-only
    const u32 fd = ps_read32(base, kFd);
    put(base, kPath, "/dev_hdd0/tmp/out.bin");
    REQUIRE(a.call("cellFsOpen", kPath, 01, kFd, 0, 0) == 0); // write-only
    const u32 write_fd = ps_read32(base, kFd);
    std::memset(base + kBuf, 0xEE, 0x100);

    // Before cellFsAioInit there is no AIO thread: CELL_ENXIO.
    const u32 aio0 = request(0, fd, 3, kBuf, 5);
    CHECK(a.call("cellFsAioRead", aio0, kId, kAioOpd) == r3_of(lv2::CELL_ENXIO));
    REQUIRE(a.call("cellFsAioInit", kMount) == 0);
    REQUIRE(a.call("cellFsAioInit", kMount) == 0); // the thread runs already
    const u64 kEfault = r3_of(lv2::CELL_EFAULT);
    CHECK(a.call("cellFsAioRead", 0, kId, kAioOpd) == kEfault);
    CHECK(a.call("cellFsAioRead", aio0, 0, kAioOpd) == kEfault);
    CHECK(a.call("cellFsAioRead", aio0, kId, 0) == kEfault);
    CHECK(a.call("cellFsAioRead", kUnmapped, kId, kAioOpd) == kEfault);

    // A read: ID 1, at once; then the callback, on the AIO thread (its own stack), with the bytes in place.
    REQUIRE(a.call("cellFsAioRead", aio0, kId, kAioOpd) == 0);
    CHECK(ps_read32(base, kId) == 1);
    REQUIRE(wait_for(1));
    const Done d0 = done(0);
    CHECK(d0.aio == aio0);
    CHECK(d0.error == 0);
    CHECK(d0.xid == 1);
    CHECK(d0.size == 5);
    CHECK(std::string(reinterpret_cast<const char*>(base + kBuf), 5) == "34567");
    CHECK(base[kBuf + 5] == 0xEE);
    CHECK((d0.r1 < kStacks || d0.r1 >= kStacks + 0x100000)); // not a test thread's stack
    // The file position didn't move.
    REQUIRE(a.call("cellFsLseek", fd, 0, 1, kPos) == 0);
    CHECK(ps_read64(base, kPos) == 0);

    // Three more, in order with IDs 2 to 4: a read past the end gets what there is; a descriptor that isn't open, or
    // a file open for writing only: CELL_EBADF and 0 bytes.
    const u32 aio1 = request(1, fd, 14, kBuf + 0x20, 8);
    const u32 aio2 = request(2, 77, 0, kBuf + 0x40, 4);
    const u32 aio3 = request(3, write_fd, 0, kBuf + 0x60, 4);
    for (const u32 aio : {aio1, aio2, aio3}) REQUIRE(a.call("cellFsAioRead", aio, kId, kAioOpd) == 0);
    CHECK(ps_read32(base, kId) == 4);
    REQUIRE(wait_for(4));
    const u32 ebadf = static_cast<u32>(lv2::CELL_EBADF);
    CHECK((done(1).aio == aio1 && done(1).error == 0 && done(1).xid == 2 && done(1).size == 2));
    CHECK((done(2).aio == aio2 && done(2).error == ebadf && done(2).xid == 3 && done(2).size == 0));
    CHECK((done(3).aio == aio3 && done(3).error == ebadf && done(3).xid == 4 && done(3).size == 0));
    CHECK(std::string(reinterpret_cast<const char*>(base + kBuf + 0x20), 2) == "EF");
    CHECK(base[kBuf + 0x22] == 0xEE);
    CHECK(all_bytes(base + kBuf + 0x40, 0x40, 0xEE));

    // Cancelling isn't possible (RPCS3): CELL_EINVAL. Finishing is CELL_OK; requests still complete.
    CHECK(a.call("cellFsAioCancel", 4) == r3_of(lv2::CELL_EINVAL));
    REQUIRE(a.call("cellFsAioFinish", kMount) == 0);
    REQUIRE(a.call("cellFsAioRead", request(4, fd, 0, kBuf + 0x80, 2), kId, kAioOpd) == 0);
    REQUIRE(wait_for(5));
    CHECK(done(4).xid == 5);
    CHECK(std::string(reinterpret_cast<const char*>(base + kBuf + 0x80), 2) == "01");

    // cellFsAioWrite: the same thread writes at the offset; a file open for reading only is CELL_EBADF.
    put(base, kBuf + 0xA0, "WXYZ");
    REQUIRE(a.call("cellFsAioWrite", request(5, write_fd, 2, kBuf + 0xA0, 4), kId, kAioOpd) == 0);
    REQUIRE(a.call("cellFsAioWrite", request(6, fd, 0, kBuf + 0xA0, 4), kId, kAioOpd) == 0);
    REQUIRE(wait_for(7));
    CHECK((done(5).xid == 6 && done(5).error == 0 && done(5).size == 4));
    CHECK((done(6).xid == 7 && done(6).error == ebadf && done(6).size == 0));
    REQUIRE(a.call("cellFsClose", fd) == 0);
    REQUIRE(a.call("cellFsClose", write_fd) == 0);
    std::ifstream written(root / "tmp" / "out.bin", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
    CHECK(bytes == std::string("\0\0WXYZ", 6));
    fsys::remove_all(root);
}

// ------------------------------------------------------------------------------------------------- sys_io

TEST_CASE("cellPad: the old info, capability info, clearing the buffer", "[hle][ac][io]")
{
    Fixture f;
    GuestThread g(f, 0, "pad");
    u8* b = f.base();
    constexpr u32 kInfo = kOut, kCaps = kOut + 0x400, kPad = kOut + 0x600;
    const auto len = [&] { return static_cast<s32>(ps_read32(b, kPad)); };
    const auto button = [&](u32 i) { return ps_read16(b, kPad + 4 + 2 * i); };

    CHECK(g.call("cellPadGetInfo", kInfo) == kPadUninit);
    CHECK(g.call("cellPadGetCapabilityInfo", 0, kCaps) == kPadUninit);
    CHECK(g.call("cellPadClearBuf", 0) == kPadUninit);
    REQUIRE(g.call("cellPadInit", 10) == 0); // more than the 7 ports

    // CellPadInfo: max_connect as given, port 0's controller with the IDs of Sony's PS3 controller; the
    // assignment-changed flag is cleared before it is reported, and cellPadGetInfo2 no longer sees it either.
    CHECK(g.call("cellPadGetInfo", 0) == kPadInval);
    std::memset(b + kInfo, 0xEE, 0x300);
    REQUIRE(g.call("cellPadGetInfo", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x0) == 10); // max_connect
    CHECK(ps_read32(b, kInfo + 0x4) == 1);  // now_connect
    CHECK(ps_read32(b, kInfo + 0x8) == 0);  // system_info
    CHECK(ps_read16(b, kInfo + 0xC) == 0x054C);   // vendor_id[0]
    CHECK(ps_read16(b, kInfo + 0x10A) == 0x0268); // product_id[0]
    CHECK(b[kInfo + 0x208] == 1);                 // status[0]: connected
    CHECK(ps_read16(b, kInfo + 0xE) == 0);        // vendor_id[1]
    CHECK(b[kInfo + 0x209] == 0);
    CHECK(all_bytes(b + kInfo + 0x209, 0x288 - 0x209, 0)); // the whole structure is written
    CHECK(b[kInfo + 0x288] == 0xEE);
    constexpr u32 kInfo2 = kOut + 0x300;
    REQUIRE(g.call("cellPadGetInfo2", kInfo2) == 0);
    CHECK(ps_read32(b, kInfo2 + 0x0) == 7); // cellPadGetInfo2's is limited to the ports
    CHECK(ps_read32(b, kInfo2 + 0xC) == 1); // port_status[0]: the flag is gone

    // CellPadCapabilityInfo: the device capability, then zeros.
    std::memset(b + kCaps, 0xEE, 0x90);
    REQUIRE(g.call("cellPadGetCapabilityInfo", 0, kCaps) == 0);
    CHECK(ps_read32(b, kCaps) == 0x1F);
    CHECK(all_bytes(b + kCaps + 4, 124, 0));
    CHECK(b[kCaps + 128] == 0xEE);
    CHECK(g.call("cellPadGetCapabilityInfo", 0, 0) == kPadInval);
    CHECK(g.call("cellPadGetCapabilityInfo", 127, kCaps) == kPadInval);
    CHECK(g.call("cellPadGetCapabilityInfo", 1, kCaps) == kPadNoDevice);
    CHECK(g.call("cellPadGetCapabilityInfo", 8, kCaps) == kPadNoDevice); // past the 7 ports

    // cellPadClearBuf: the next read reports in full again, from the rest state.
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len() == 8);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len() == 0);
    REQUIRE(g.call("cellPadClearBuf", 0) == 0);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len() == 8);
    CHECK(button(6) == 128); // left stick x at rest
    // With a button held: reported, then no change; after clearing, reported again.
    hle::PadInput in;
    in.digital2 = 0x40; // cross
    hle::set_pad_input(*f.hle, in);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len() == 8);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len() == 0);
    REQUIRE(g.call("cellPadClearBuf", 0) == 0);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len() == 8);
    CHECK(button(3) == 0x40);
    CHECK(g.call("cellPadClearBuf", 127) == kPadInval);
    CHECK(g.call("cellPadClearBuf", 1) == kPadNoDevice);
    CHECK(g.call("cellPadClearBuf", 8) == kPadNoDevice);
    REQUIRE(g.call("cellPadEnd") == 0);

    // A smaller max_connect: the ports past it have no device.
    REQUIRE(g.call("cellPadInit", 1) == 0);
    REQUIRE(g.call("cellPadGetInfo", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x0) == 1);
    CHECK(g.call("cellPadGetCapabilityInfo", 1, kCaps) == kPadNoDevice);
    CHECK(g.call("cellPadClearBuf", 1) == kPadNoDevice);
}

TEST_CASE("cellFs: directory entries, paths, block sizes, truncate, stream reads (D-184)", "[hle][fs]")
{
    InterpretMisses interpret;
    const fsys::path root = temp_root("fs2");
    fsys::create_directories(root / "hdd0" / "game" / "TEST00000" / "sub");
    std::string data;
    for (int i = 0; i < 10000; i++) data += static_cast<char>('a' + i % 26);
    {
        std::ofstream out(root / "hdd0" / "game" / "TEST00000" / "stream.bin", std::ios::binary);
        out << data;
    }
    Fixture f;
    u8* base = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    GuestThread a(f, 0, "a");
    constexpr u32 kPath = kStrings, kFd = kOut, kCount = kOut + 8, kEntries = kOut + 0x100, kSizes = kOut + 0x400,
                  kRing = kOut + 0x500, kAddr = kOut + 0x540, kStatus = kOut + 0x548, kStBuf = kBuf;
    const u64 kEbadf = r3_of(lv2::CELL_EBADF), kEfault = r3_of(lv2::CELL_EFAULT), kEnxio = r3_of(lv2::CELL_ENXIO);

    // cellFsGetDirectoryEntries: one entry a call, then none.
    put(base, kPath, "/dev_hdd0/game/TEST00000");
    REQUIRE(a.call("cellFsOpendir", kPath, kFd) == 0);
    const u32 dir = ps_read32(base, kFd);
    std::vector<std::string> names;
    for (int i = 0; i < 6; i++) {
        REQUIRE(a.call("cellFsGetDirectoryEntries", dir, kEntries, 2 * 312, kCount) == 0);
        if (!ps_read32(base, kCount)) break;
        names.emplace_back(reinterpret_cast<const char*>(base + kEntries + 52 + 2), base[kEntries + 52 + 1]);
    }
    CHECK(names == std::vector<std::string>{".", "..", "stream.bin", "sub"});
    CHECK(a.call("cellFsGetDirectoryEntries", dir, kEntries, 312, 0) == kEfault);
    CHECK(a.call("cellFsGetDirectoryEntries", 2, kEntries, 312, kCount) == kEbadf);

    // cellFsGetPath, the block sizes, cellFsTruncate, cellFsFdatasync.
    put(base, kPath, "/dev_hdd0/game/TEST00000/stream.bin");
    REQUIRE(a.call("cellFsOpen", kPath, 0, kFd, 0, 0) == 0);
    const u32 fd = ps_read32(base, kFd);
    REQUIRE(a.call("cellFsGetPath", fd, kEntries) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(base + kEntries)) == "/dev_hdd0/game/TEST00000/stream.bin");
    CHECK(a.call("cellFsGetPath", fd, 0) == kEfault);
    REQUIRE(a.call("cellFsGetBlockSize", kPath, kSizes, kSizes + 8) == 0);
    CHECK(ps_read64(base, kSizes + 8) == 4096);
    CHECK(a.call("cellFsGetBlockSize", kPath, 0, kSizes + 8) == kEfault);
    REQUIRE(a.call("cellFsFGetBlockSize", fd, kSizes, kSizes + 8) == 0);
    CHECK(ps_read64(base, kSizes) == 512);
    CHECK(a.call("cellFsFdatasync", fd) == kEbadf); // read-only: nothing to sync (RPCS3)
    CHECK(a.call("cellFsSetIoBufferFromDefaultContainer", fd, 0x10000, 0) == 0);
    CHECK(a.call("cellFsSetIoBufferFromDefaultContainer", 99, 0x10000, 0) == kEbadf);
    CHECK(a.call("cellFsSdataOpenByFd", fd, 1, kCount, 0, 0, 0) == r3_of(lv2::CELL_EINVAL));
    CHECK(ps_read32(base, kCount) == 0xFFFFFFFF);
    CHECK(a.call("cellFsSdataOpenByFd", 1, 0, kCount, 0, 0, 0) == kEbadf);

    // Stream reads. CellFsRingBuffer {ringbuf_size, block_size, transfer_rate, copy}.
    const auto ring = [&](u64 size, u64 block, u32 copy) {
        ps_write64(base, kRing, size);
        ps_write64(base, kRing + 8, block);
        ps_write64(base, kRing + 16, 0);
        ps_write32(base, kRing + 24, copy);
    };
    REQUIRE(a.call("cellFsStReadGetStatus", fd, kStatus) == 0);
    CHECK(ps_read64(base, kStatus) == 0x2); // not initialized
    CHECK(a.call("cellFsStRead", fd, kStBuf, 16, kCount) == kEnxio);
    ring(0x4000, 0x1001, 0);
    CHECK(a.call("cellFsStReadInit", fd, kRing) == r3_of(lv2::CELL_EINVAL)); // a block not a multiple of 4 KB
    ring(0x3000, 0x2000, 0);
    CHECK(a.call("cellFsStReadInit", fd, kRing) == r3_of(lv2::CELL_EINVAL)); // a ring not a multiple of the block
    ring(0x4000, 0x1000, 2);
    CHECK(a.call("cellFsStReadInit", fd, kRing) == r3_of(lv2::CELL_EINVAL)); // an unknown copy mode
    ring(0x4000, 0x1000, 1);
    REQUIRE(a.call("cellFsStReadInit", fd, kRing) == 0);
    REQUIRE(a.call("cellFsStReadGetStatus", fd, kStatus) == 0);
    CHECK(ps_read64(base, kStatus) == 0x101); // initialized, stopped
    CHECK(a.call("cellFsStRead", fd, kStBuf, 16, kCount) == kEnxio); // not started
    REQUIRE(a.call("cellFsStReadStart", fd, 100, 5000) == 0);
    REQUIRE(a.call("cellFsStReadGetStatus", fd, kStatus) == 0);
    CHECK(ps_read64(base, kStatus) == 0x201);
    REQUIRE(a.call("cellFsStReadWait", fd, 16) == 0);
    REQUIRE(a.call("cellFsStRead", fd, kStBuf, 16, kCount) == 0);
    CHECK(ps_read64(base, kCount) == 16);
    CHECK(std::string(reinterpret_cast<const char*>(base + kStBuf), 16) == data.substr(100, 16));
    // Copyless: the next block, in the ring buffer, until it is put back.
    REQUIRE(a.call("cellFsStReadGetCurrentAddr", fd, kAddr, kSizes) == 0);
    const u32 at = ps_read32(base, kAddr);
    CHECK(ps_read64(base, kSizes) == 0x1000);
    CHECK(std::string(reinterpret_cast<const char*>(base + at), 8) == data.substr(116, 8));
    REQUIRE(a.call("cellFsStReadPutCurrentAddr", fd, at, 0x1000) == 0);
    // The stream ends at 100 + 5000: what is left, then nothing.
    REQUIRE(a.call("cellFsStRead", fd, kStBuf, 0x2000, kCount) == 0);
    CHECK(ps_read64(base, kCount) == 5000 - 16 - 0x1000);
    CHECK(base[kStBuf] == static_cast<u8>(data[116 + 0x1000]));
    REQUIRE(a.call("cellFsStRead", fd, kStBuf, 16, kCount) == 0);
    CHECK(ps_read64(base, kCount) == 0);
    REQUIRE(a.call("cellFsStReadGetRingBuf", fd, kRing) == 0);
    CHECK(ps_read64(base, kRing) == 0x4000);
    CHECK(ps_read32(base, kRing + 24) == 1);
    REQUIRE(a.call("cellFsStReadGetRegid", fd, kSizes) == 0);
    REQUIRE(a.call("cellFsStReadStop", fd) == 0);
    REQUIRE(a.call("cellFsStReadFinish", fd) == 0);
    REQUIRE(a.call("cellFsStReadGetStatus", fd, kStatus) == 0);
    CHECK(ps_read64(base, kStatus) == 0x2);
    CHECK(a.call("cellFsStReadStart", 99, 0, 0) == kEbadf);
    REQUIRE(a.call("cellFsClose", fd) == 0);

    // cellFsTruncate on a path.
    REQUIRE(a.call("cellFsTruncate", kPath, 10) == 0);
    CHECK(fsys::file_size(root / "hdd0" / "game" / "TEST00000" / "stream.bin") == 10);
    REQUIRE(a.call("cellFsClosedir", dir) == 0);
    fsys::remove_all(root);
}
