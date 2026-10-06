// cellSaveData (synthetic: no game data): the save data functions GTA V imports, against a temporary /dev_hdd0. The
// game's callbacks are one scripted PPU function in guest memory: each call takes the next step of a script (the result
// to report and the words to put in its `set` structure) and logs what it was called with. Saves made by hand on the
// host stand for saves another run (or RPCS3) left.

#include <catch_amalgamated.hpp>

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hle_test_util.hpp"
#include "psf.hpp" // runtime/hle/src (internal): the PARAM.SFO reader and writer
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;
namespace psf = pspocket::hle::psf;

namespace {

const u64 kCbResult = r3_of_code(0x8002b401);
const u64 kAccessError = r3_of_code(0x8002b402);
const u64 kInternal = r3_of_code(0x8002b403);
const u64 kParam = r3_of_code(0x8002b404);
const u64 kFailure = r3_of_code(0x8002b407);
const u64 kBusy = r3_of_code(0x8002b408);
const u64 kNoUser = r3_of_code(0x8002b409);
const u64 kNoData = r3_of_code(0x8002b40b);
constexpr u64 kCancel = 1; // CELL_CANCEL

constexpr u32 kSdk36 = 0x00360001, kSdk47 = 0x00470001;
constexpr s32 kHddFreeSizeKB = 40 * 1024 * 1024 - 256;

// Callback results, file operations and types, options.
enum : s32 { kOkLastNoConfirm = 2, kOkLast = 1, kOkNext = 0, kErrFailure = -2, kErrBroken = -3, kErrNoData = -4 };
enum : u32 { kRead = 0, kWrite = 1, kDelete = 2, kWriteNoTrunc = 3 };
enum : u32 { kSecureFile = 0, kNormalFile = 1, kIcon0 = 2 };
enum : u32 { kOptionNone = 0, kOptionNoConfirm = 1 };

// Offsets in the structures the callbacks get (cellSaveData.h).
namespace stat_get {
enum : u32 {
    hddFreeSizeKB = 0,
    isNewData = 4,
    dirMtime = 16,
    dirName = 32,
    title = 64,
    subTitle = 64 + 0x80,
    detail = 64 + 0x100,
    listParam = 64 + 0x508,
    sizeKB = 1620,
    sysSizeKB = 1624,
    fileNum = 1628,
    fileListNum = 1632,
    fileList = 1636,
};
}
namespace file_stat {
enum : u32 { fileType = 0, size = 8, mtime = 24, fileName = 40, kSize = 56 };
}
namespace list_get {
enum : u32 { dirNum = 0, dirListNum = 4, dirList = 8 };
}
namespace done_get {
enum : u32 { excResult = 0, dirName = 4, sizeKB = 36, hddFreeSizeKB = 40 };
}

// Guest memory (the fixture's committed kData area).
constexpr u32 kCode = kData + 0x10000;        // the scripted callback
constexpr u32 kReenterCode = kData + 0x10400; // a funcStat that calls cellSaveDataAutoLoad2 again
constexpr u32 kOpdStat = kData + 0x10800, kOpdFile = kOpdStat + 8, kOpdFixed = kOpdStat + 16,
              kOpdDone = kOpdStat + 24, kOpdReenter = kOpdStat + 32, kOpdList = kOpdStat + 40;
// The descriptors' TOCs: the callback logs r2, which tells which of them the game's side ran as.
constexpr u32 kTagStat = 0x5A7, kTagFile = 0xF11E, kTagFixed = 0xF1C5, kTagDone = 0xD0E, kTagList = 0x115;
constexpr u32 kScript = kData + 0x20000; // +0 calls so far, +4 steps; step i at +0x100 + 0x80 * i
constexpr u32 kLog = kData + 0x30000;    // call i at +0x20 * i
constexpr u32 kMaxSteps = 64;
constexpr u32 kStrings = kData + 0x40000;
constexpr u32 kSetBuf = kData + 0x41000;
constexpr u32 kSetList = kData + 0x41100;
constexpr u32 kReenterOut = kData + 0x41200;
constexpr u32 kParamBlock = kData + 0x42000; // a CellSaveDataSystemFileParam
constexpr u32 kBuf = kData + 0x50000;        // setBuf->buf (64 KB)
constexpr u32 kFileData = kData + 0x60000;
constexpr u32 kReadBuf = kData + 0x70000;

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

s32 hi(u32 v) { return static_cast<s32>(v >> 16); }
u32 lo(u32 v) { return v & 0xFFFF; }

void write_code(u8* base, u32 at, const std::vector<u32>& code)
{
    for (usize i = 0; i < code.size(); i++) ps_write32(base, at + 4 * static_cast<u32>(i), code[i]);
}

void write_opd(u8* base, u32 at, u32 code, u32 toc)
{
    ps_write32(base, at, code);
    ps_write32(base, at + 4, toc);
}

// callback(cbResult r3, get r4, set r5): counts its call, logs {r2, r3, r4, r5, get[0], get[1], cbResult->userdata},
// then takes the next step of the script: its result goes to cbResult->result and its words to *set. Past the
// script's end it reports ERR_FAILURE, which ends every loop.
void write_script_callback(u8* base)
{
    write_code(base, kCode, {
        as::lis(6, hi(kScript)), as::ori(6, 6, lo(kScript)),
        as::lwz(7, 0, 6), as::addi(8, 7, 1), as::stw(8, 0, 6),             // r7 = call number
        as::slwi(9, 7, 5), as::lis(10, hi(kLog)), as::ori(10, 10, lo(kLog)), as::add(9, 9, 10),
        as::stw(2, 0, 9), as::stw(3, 4, 9), as::stw(4, 8, 9), as::stw(5, 12, 9),
        as::lwz(10, 0, 4), as::stw(10, 16, 9), as::lwz(10, 4, 4), as::stw(10, 20, 9),
        as::lwz(10, 16, 3), as::stw(10, 24, 9),
        as::lwz(10, 4, 6), as::cmplw(0, 7, 10), as::bc_rel(12, 0, 16),    // blt: a step is left
        as::li(10, -2), as::stw(10, 0, 3), as::blr(),
        as::slwi(9, 7, 7), as::add(9, 9, 6), as::addi(9, 9, 0x100),       // r9 = the step
        as::lwz(10, 0, 9), as::stw(10, 0, 3), as::lwz(11, 4, 9), as::addi(9, 9, 8),
        as::cmpwi(0, 11, 0), as::bclr(12, 2),                              // beqlr: all words copied
        as::lwz(10, 0, 9), as::stw(10, 0, 5), as::addi(9, 9, 4), as::addi(5, 5, 4), as::addi(11, 11, -1),
        as::b_rel(-28),
    });
    write_opd(base, kOpdStat, kCode, kTagStat);
    write_opd(base, kOpdFile, kCode, kTagFile);
    write_opd(base, kOpdFixed, kCode, kTagFixed);
    write_opd(base, kOpdDone, kCode, kTagDone);
    write_opd(base, kOpdList, kCode, kTagList);
}

// A funcStat that calls cellSaveDataAutoLoad2 (through its import descriptor) for `dir` while its own operation runs,
// stores what that returned at kReenterOut and reports OK_LAST.
void write_reenter_callback(u8* base, u32 descriptor, u32 dir)
{
    write_code(base, kReenterCode, {
        as::mflr(0), as::std_(0, 16, 1), as::stdu(1, -128, 1), as::std_(31, 120, 1), as::std_(2, 40, 1),
        as::mr(31, 3),
        as::li(3, 1), as::lis(4, hi(dir)), as::ori(4, 4, lo(dir)), as::li(5, 0),
        as::lis(6, hi(kSetBuf)), as::ori(6, 6, lo(kSetBuf)), as::lis(7, hi(kOpdStat)), as::ori(7, 7, lo(kOpdStat)),
        as::lis(8, hi(kOpdFile)), as::ori(8, 8, lo(kOpdFile)), as::li(9, -1), as::li(10, 0),
        as::lis(12, hi(descriptor)), as::ori(12, 12, lo(descriptor)), as::lwz(0, 0, 12), as::lwz(2, 4, 12),
        as::mtctr(0), as::bctrl(), as::ld(2, 40, 1),
        as::lis(12, hi(kReenterOut)), as::ori(12, 12, lo(kReenterOut)), as::stw(3, 0, 12),
        as::li(0, 1), as::stw(0, 0, 31),
        as::ld(31, 120, 1), as::addi(1, 1, 128), as::ld(0, 16, 1), as::mtlr(0), as::blr(),
    });
    write_opd(base, kOpdReenter, kReenterCode, kTagStat);
}

struct Call {
    u32 tag, result, get, set, get0, get1, userdata;
};

class Script {
public:
    explicit Script(u8* base) : base_(base) { clear(); }

    void clear()
    {
        std::memset(base_ + kScript, 0, 0x100 + kMaxSteps * 0x80);
        std::memset(base_ + kLog, 0, kMaxSteps * 0x20);
        steps_ = 0;
    }

    Script& step(s32 result, const std::vector<u32>& set = {})
    {
        REQUIRE(steps_ < kMaxSteps);
        REQUIRE(set.size() <= 30);
        const u32 at = kScript + 0x100 + 0x80 * steps_;
        ps_write32(base_, at, static_cast<u32>(result));
        ps_write32(base_, at + 4, static_cast<u32>(set.size()));
        for (usize i = 0; i < set.size(); i++) ps_write32(base_, at + 8 + 4 * static_cast<u32>(i), set[i]);
        ps_write32(base_, kScript + 4, ++steps_);
        return *this;
    }

    u32 calls() const { return ps_read32(base_, kScript); }

    Call call(u32 i) const
    {
        const u32 at = kLog + 0x20 * i;
        return {ps_read32(base_, at), ps_read32(base_, at + 4), ps_read32(base_, at + 8), ps_read32(base_, at + 12),
            ps_read32(base_, at + 16), ps_read32(base_, at + 20), ps_read32(base_, at + 24)};
    }

    std::vector<u32> tags() const
    {
        std::vector<u32> out;
        for (u32 i = 0; i < calls(); i++) out.push_back(call(i).tag);
        return out;
    }

private:
    u8* base_;
    u32 steps_ = 0;
};

// CellSaveDataStatSet, CellSaveDataFileSet, CellSaveDataFixedSet as script words.
std::vector<u32> stat_set(u32 set_param, u32 recreate = 0) { return {set_param, recreate, 0}; }
std::vector<u32> file_set(u32 op, u32 type, u32 name, u32 offset, u32 size, u32 buf_size, u32 buf, u32 secure_id = 0)
{
    return {op, 0, type, secure_id, 0, 0, 0, name, offset, size, buf_size, buf};
}
std::vector<u32> fixed_set(u32 dir, u32 option) { return {dir, 0, option}; }

std::string guest_string(u8* base, u32 addr) { return std::string(reinterpret_cast<const char*>(base + addr)); }

void write_bytes(u8* base, u32 addr, std::string_view s) { std::memcpy(base + addr, s.data(), s.size()); }

// CellSaveDataSystemFileParam.
void write_param(u8* base, std::string_view title, std::string_view subtitle, std::string_view detail,
    std::string_view list_param, u32 attribute = 0, u32 parental_level = 0)
{
    std::memset(base + kParamBlock, 0, 0x610);
    write_bytes(base, kParamBlock, title);
    write_bytes(base, kParamBlock + 0x80, subtitle);
    write_bytes(base, kParamBlock + 0x100, detail);
    ps_write32(base, kParamBlock + 0x500, attribute);
    ps_write32(base, kParamBlock + 0x504, parental_level);
    write_bytes(base, kParamBlock + 0x508, list_param);
}

// CellSaveDataSetBuf and CellSaveDataSetList.
void write_set_buf(u8* base, u32 dir_list_max, u32 file_list_max, u32 buf_size, u32 buf = kBuf)
{
    std::memset(base + kSetBuf, 0, 40);
    ps_write32(base, kSetBuf + 0, dir_list_max);
    ps_write32(base, kSetBuf + 4, file_list_max);
    ps_write32(base, kSetBuf + 32, buf_size);
    ps_write32(base, kSetBuf + 36, buf);
}

void write_set_list(u8* base, u32 sort_type, u32 sort_order, u32 prefix)
{
    std::memset(base + kSetList, 0, 16);
    ps_write32(base, kSetList + 0, sort_type);
    ps_write32(base, kSetList + 4, sort_order);
    ps_write32(base, kSetList + 8, prefix);
}

// ---- the host side ----

fsys::path temp_root(std::string_view name)
{
    const fsys::path root =
        fsys::temp_directory_path() / ("pspocket-hle-savedata-" + std::string(name) + "-" + std::to_string(::getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root);
    return root;
}

fsys::path savedata(const fsys::path& root) { return root / "home" / "00000001" / "savedata"; }

std::string read_file(const fsys::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fsys::path& p, std::string_view data)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

psf::Registry read_sfo(const fsys::path& p)
{
    const std::string bytes = read_file(p);
    auto parsed = psf::parse(std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
    REQUIRE(parsed);
    return *parsed;
}

u32 sfo_integer(const psf::Registry& sfo, const std::string& key)
{
    const auto it = sfo.find(key);
    REQUIRE(it != sfo.end());
    REQUIRE(it->second.format == psf::Format::Integer);
    return it->second.integer;
}

std::vector<std::string> entries(const fsys::path& dir)
{
    std::vector<std::string> out;
    for (const auto& e : fsys::directory_iterator(dir)) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

void set_times(const fsys::path& p, s64 t)
{
    const struct ::timeval tv[2] = {{static_cast<time_t>(t), 0}, {static_cast<time_t>(t), 0}};
    REQUIRE(::utimes(p.c_str(), tv) == 0);
}

s64 host_mtime(const fsys::path& p)
{
    struct ::stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return static_cast<s64>(st.st_mtime);
}

// A save as this module (and RPCS3) writes it, its directory's times set to `time`.
void make_save(const fsys::path& root, std::string_view dir, std::string_view subtitle, std::string_view list_param,
    const std::vector<std::pair<std::string, std::string>>& files, s64 time)
{
    const fsys::path d = savedata(root) / dir;
    fsys::create_directories(d);
    psf::Registry sfo;
    sfo["CATEGORY"] = psf::string(4, "SD");
    sfo["TITLE"] = psf::string(128, "Grand Theft Auto V");
    sfo["SUB_TITLE"] = psf::string(128, subtitle);
    sfo["DETAIL"] = psf::string(1024, "A detail");
    sfo["SAVEDATA_DIRECTORY"] = psf::string(32, dir);
    sfo["SAVEDATA_LIST_PARAM"] = psf::string(8, list_param);
    sfo["ATTRIBUTE"] = psf::Entry{psf::Format::Integer, 4, {}, 0};
    sfo["PARENTAL_LEVEL"] = psf::Entry{psf::Format::Integer, 4, {}, 0};
    std::string order;
    for (const auto& [name, data] : files) {
        write_file(d / name, data);
        sfo["*" + name] = psf::Entry{psf::Format::Integer, 4, {}, 0};
        order += (order.empty() ? "" : "/") + name;
    }
    sfo["RPCS3_BLIST"] = psf::string(static_cast<u32>((order.size() + 4) & ~usize{3}), order);
    const std::vector<u8> bytes = psf::save(sfo);
    write_file(d / "PARAM.SFO", std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    set_times(d, time);
}

// A process with /dev_hdd0 on `hdd0` (none when empty), the scripted callbacks and one guest thread.
struct SaveProcess {
    InterpretMisses interpret;
    Fixture f;
    GuestThread a;
    Script script;
    u32 next_string = kStrings;

    static hle::ProcessConfig process_config()
    {
        hle::ProcessConfig c;
        c.game.title_id = "BLES01807";
        return c;
    }
    static lv2::Config kernel_config(u32 sdk)
    {
        lv2::Config c;
        c.sdk_version = sdk;
        return c;
    }

    explicit SaveProcess(const fsys::path& hdd0, u32 sdk = kSdk47)
        : f(process_config(), kernel_config(sdk)), a(f, 0, "main"), script(f.base())
    {
        if (!hdd0.empty()) REQUIRE(f.kernel->vfs_mount("/dev_hdd0", hdd0.string(), false));
        write_script_callback(f.base());
        write_set_buf(f.base(), 0, 16, 16 * file_stat::kSize);
    }

    u8* base() { return f.base(); }

    // A guest string.
    u32 str(std::string_view s)
    {
        const u32 at = next_string;
        write_bytes(base(), at, s);
        base()[at + s.size()] = 0;
        next_string += (static_cast<u32>(s.size()) + 16) & ~15u;
        return at;
    }

    u64 auto_load(u32 dir, u32 err_dialog = 0, u32 userdata = 0, u32 version = 1)
    {
        return a.call("cellSaveDataAutoLoad2", version, dir, err_dialog, kSetBuf, kOpdStat, kOpdFile, 0xFFFFFFFFu,
            userdata);
    }
    u64 auto_save(u32 dir, u32 err_dialog = 0)
    {
        return a.call("cellSaveDataAutoSave2", 1, dir, err_dialog, kSetBuf, kOpdStat, kOpdFile, 0xFFFFFFFFu, 0);
    }
    u64 list_auto_load(u32 err_dialog = 0, u32 func_fixed = kOpdFixed)
    {
        return a.call("cellSaveDataListAutoLoad", 1, err_dialog, kSetList, kSetBuf, func_fixed, kOpdStat, kOpdFile,
            0xFFFFFFFFu, 0x5EED);
    }
    u64 fixed_delete(u32 func_fixed = kOpdFixed, u32 func_done = kOpdDone)
    {
        return a.call("cellSaveDataFixedDelete", kSetList, kSetBuf, func_fixed, func_done, 0xFFFFFFFFu, 0);
    }
};

} // namespace

TEST_CASE("cellSaveData: the functions GTA V imports, in their modules", "[hle][savedata]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"cellSysutil", "cellSaveDataAutoLoad2", 0xFBD5C856},
        {"cellSysutil", "cellSaveDataAutoSave2", 0x8B7ED64B},
        {"cellSysutil", "cellSaveDataListAutoLoad", 0x21425307},
        {"cellSysutil", "cellSaveDataDelete2", 0xEDADD797},
        {"cellSysutil", "cellSaveDataFixedDelete", 0xCED17573},
        {"cellSaveData", "cellSaveDataGetListItem", 0x04C06FC2},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("cellSaveData: every function RPCS3 registers, in its module (D-183)", "[hle][savedata]")
{
    const char* sysutil[] = {"cellSaveDataGetEnableOverlay", "cellSaveDataEnableOverlay", "cellSaveDataDelete2",
        "cellSaveDataDelete", "cellSaveDataUserFixedDelete", "cellSaveDataFixedDelete", "cellSaveDataUserFixedLoad",
        "cellSaveDataUserFixedSave", "cellSaveDataFixedLoad2", "cellSaveDataFixedSave2", "cellSaveDataFixedLoad",
        "cellSaveDataFixedSave", "cellSaveDataUserListLoad", "cellSaveDataUserListSave", "cellSaveDataListLoad2",
        "cellSaveDataListSave2", "cellSaveDataListLoad", "cellSaveDataListSave", "cellSaveDataUserListAutoLoad",
        "cellSaveDataUserListAutoSave", "cellSaveDataListAutoLoad", "cellSaveDataListAutoSave",
        "cellSaveDataUserAutoLoad", "cellSaveDataUserAutoSave", "cellSaveDataAutoLoad2", "cellSaveDataAutoSave2",
        "cellSaveDataAutoLoad", "cellSaveDataAutoSave"};
    const char* savedata[] = {"cellSaveDataUserGetListItem", "cellSaveDataGetListItem", "cellSaveDataUserListDelete",
        "cellSaveDataListDelete", "cellSaveDataUserFixedExport", "cellSaveDataUserFixedImport",
        "cellSaveDataUserListExport", "cellSaveDataUserListImport", "cellSaveDataFixedExport",
        "cellSaveDataFixedImport", "cellSaveDataListExport", "cellSaveDataListImport"};
    for (const char* name : sysutil) {
        INFO(name);
        CHECK(hle::registry().find("cellSysutil", recomp::nid_of(name)) != nullptr);
    }
    for (const char* name : savedata) {
        INFO(name);
        CHECK(hle::registry().find("cellSaveData", recomp::nid_of(name)) != nullptr);
    }
}

TEST_CASE("cellSaveDataAutoLoad2 with no save data: funcStat sees new data, nothing is created", "[hle][savedata]")
{
    const fsys::path root = temp_root("first");
    SaveProcess p(root);
    u8* base = p.base();
    const u32 dir = p.str("BLES01807-AUTOSAVE");

    // What a game does the first time: funcStat finds new data and reports there is nothing to load.
    p.script.step(kErrNoData);
    CHECK(p.auto_load(dir, 0, 0xCAFE) == kCbResult);
    REQUIRE(p.script.calls() == 1);
    const Call stat = p.script.call(0);
    CHECK(stat.tag == kTagStat);
    CHECK(stat.get0 == static_cast<u32>(kHddFreeSizeKB));
    CHECK(stat.get1 == 1); // isNewData
    CHECK(stat.userdata == 0xCAFE);
    CHECK(guest_string(base, stat.get + stat_get::dirName) == "BLES01807-AUTOSAVE");
    CHECK(guest_string(base, stat.get + stat_get::title).empty());
    CHECK(ps_read32(base, stat.get + stat_get::sizeKB) == 0);
    CHECK(ps_read32(base, stat.get + stat_get::sysSizeKB) == 35);
    CHECK(ps_read32(base, stat.get + stat_get::fileNum) == 0);
    CHECK(ps_read32(base, stat.get + stat_get::fileListNum) == 0);
    CHECK(ps_read32(base, stat.get + stat_get::fileList) == kBuf);
    CHECK(ps_read64(base, stat.get + stat_get::dirMtime) == 0); // no directory
    // The user's savedata directory exists from the first call (RPCS3 makes it at boot), the save doesn't.
    REQUIRE(fsys::is_directory(savedata(root)));
    CHECK(fsys::is_empty(savedata(root)));

    // OK_LAST from funcStat ends the operation without an error, before funcFile.
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(p.auto_load(dir) == 0);
    CHECK(p.script.calls() == 1);

    // Going on to the files of new data needs PARAM.SFO's fields (setParam).
    p.script.clear();
    p.script.step(kOkNext, stat_set(0));
    CHECK(p.auto_load(dir) == kParam);
    CHECK(p.script.calls() == 1);
    CHECK(fsys::is_empty(savedata(root)));

    // Without /dev_hdd0 there is no place for save data.
    {
        SaveProcess none({});
        none.script.step(kOkLast);
        CHECK(none.auto_load(none.str("BLES01807-AUTOSAVE")) == kAccessError);
        CHECK(none.script.calls() == 0);
    }
    fsys::remove_all(root);
}

TEST_CASE("cellSaveDataAutoSave2 creates the save; cellSaveDataAutoLoad2 reads it back", "[hle][savedata]")
{
    const fsys::path root = temp_root("roundtrip");
    const fsys::path save = savedata(root) / "BLES01807-AUTOSAVE";
    std::string progress(100, '\0');
    for (usize i = 0; i < progress.size(); i++) progress[i] = static_cast<char>('A' + i % 26);
    const std::string secure = "secure data, forty bytes long..........."; // 40
    const std::string icon = "\x89PNG icon bytes!";                        // 16
    REQUIRE(secure.size() == 40);
    REQUIRE(icon.size() == 16);
    {
        SaveProcess p(root);
        u8* base = p.base();
        const u32 dir = p.str("BLES01807-AUTOSAVE");
        write_param(base, "Grand Theft Auto V", "Progress", "Mission 3 of 69", "SLOT01");
        write_bytes(base, kFileData, progress);
        write_bytes(base, kFileData + 0x1000, secure);
        write_bytes(base, kFileData + 0x2000, icon);
        p.script.step(kOkNext, stat_set(kParamBlock))
            .step(kOkNext, file_set(kWrite, kNormalFile, p.str("SAVE.BIN"), 0, 100, 128, kFileData))
            .step(kOkNext, file_set(kWrite, kSecureFile, p.str("SECURE.DAT"), 0, 40, 40, kFileData + 0x1000, 0x1122))
            .step(kOkNext, file_set(kWrite, kIcon0, 0, 0, 16, 16, kFileData + 0x2000))
            .step(kOkLast);
        CHECK(p.auto_save(dir) == 0);
        CHECK(p.script.tags() == std::vector<u32>{kTagStat, kTagFile, kTagFile, kTagFile, kTagFile});
        CHECK(p.script.call(0).get1 == 1); // new data
        CHECK(p.script.call(1).get0 == 0); // fileGet->excSize before the first file
        CHECK(p.script.call(2).get0 == 100);
        CHECK(p.script.call(3).get0 == 40);
        CHECK(p.script.call(4).get0 == 16);
    }
    // On the host, as RPCS3 lays it out: the save's directory with PARAM.SFO and the files, nothing else.
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-AUTOSAVE"});
    CHECK(entries(save) == std::vector<std::string>{"ICON0.PNG", "PARAM.SFO", "SAVE.BIN", "SECURE.DAT"});
    CHECK(read_file(save / "SAVE.BIN") == progress);
    CHECK(read_file(save / "SECURE.DAT") == secure);
    CHECK(read_file(save / "ICON0.PNG") == icon);
    {
        const psf::Registry sfo = read_sfo(save / "PARAM.SFO");
        CHECK(psf::get_string(sfo, "CATEGORY") == "SD");
        CHECK(psf::get_string(sfo, "TITLE") == "Grand Theft Auto V");
        CHECK(psf::get_string(sfo, "SUB_TITLE") == "Progress");
        CHECK(psf::get_string(sfo, "DETAIL") == "Mission 3 of 69");
        CHECK(psf::get_string(sfo, "SAVEDATA_DIRECTORY") == "BLES01807-AUTOSAVE");
        CHECK(psf::get_string(sfo, "SAVEDATA_LIST_PARAM") == "SLOT01");
        CHECK(psf::get_string(sfo, "ACCOUNT_ID") == "0000000000000000");
        CHECK(sfo.at("ACCOUNT_ID").format == psf::Format::Array);
        CHECK(sfo_integer(sfo, "ATTRIBUTE") == 0);
        CHECK(sfo_integer(sfo, "PARENTAL_LEVEL") == 0);
        CHECK(sfo_integer(sfo, "*SAVE.BIN") == 0);
        CHECK(sfo_integer(sfo, "*SECURE.DAT") == 1);
        CHECK(sfo_integer(sfo, "*ICON0.PNG") == 0);
        CHECK(psf::get_string(sfo, "RPCS3_BLIST") == "SAVE.BIN/SECURE.DAT/ICON0.PNG");
    }
    const std::string sfo_bytes = read_file(save / "PARAM.SFO");

    // The next run loads it: funcStat sees the save and its files, funcFile reads them.
    {
        SaveProcess p(root);
        u8* base = p.base();
        const u32 dir = p.str("BLES01807-AUTOSAVE");
        const u32 save_bin = p.str("SAVE.BIN");
        p.script.step(kOkNext, stat_set(0))
            .step(kOkNext, file_set(kRead, kNormalFile, save_bin, 0, 100, 128, kReadBuf))
            .step(kOkNext, file_set(kRead, kNormalFile, save_bin, 60, 100, 128, kReadBuf + 0x200)) // 40 left
            .step(kOkNext, file_set(kRead, kSecureFile, p.str("SECURE.DAT"), 0, 40, 64, kReadBuf + 0x400))
            .step(kOkLast);
        CHECK(p.auto_load(dir) == 0);
        REQUIRE(p.script.calls() == 5);
        const Call stat = p.script.call(0);
        CHECK(stat.get1 == 0); // not new
        CHECK(guest_string(base, stat.get + stat_get::title) == "Grand Theft Auto V");
        CHECK(guest_string(base, stat.get + stat_get::subTitle) == "Progress");
        CHECK(guest_string(base, stat.get + stat_get::detail) == "Mission 3 of 69");
        CHECK(guest_string(base, stat.get + stat_get::listParam) == "SLOT01");
        CHECK(static_cast<s64>(ps_read64(base, stat.get + stat_get::dirMtime)) == host_mtime(save));
        CHECK(ps_read32(base, stat.get + stat_get::fileNum) == 3);
        CHECK(ps_read32(base, stat.get + stat_get::fileListNum) == 3);
        CHECK(ps_read32(base, stat.get + stat_get::sizeKB) == 3 + 35); // each file rounded up to 1 KB, + 35
        // The files in the order they were written (RPCS3_BLIST), with their types.
        const auto file = [&](u32 i) { return kBuf + i * file_stat::kSize; };
        CHECK(guest_string(base, file(0) + file_stat::fileName) == "SAVE.BIN");
        CHECK(ps_read32(base, file(0) + file_stat::fileType) == kNormalFile);
        CHECK(ps_read64(base, file(0) + file_stat::size) == 100);
        CHECK(static_cast<s64>(ps_read64(base, file(0) + file_stat::mtime)) == host_mtime(save / "SAVE.BIN"));
        CHECK(guest_string(base, file(1) + file_stat::fileName) == "SECURE.DAT");
        CHECK(ps_read32(base, file(1) + file_stat::fileType) == kSecureFile);
        CHECK(ps_read64(base, file(1) + file_stat::size) == 40);
        CHECK(guest_string(base, file(2) + file_stat::fileName) == "ICON0.PNG");
        CHECK(ps_read32(base, file(2) + file_stat::fileType) == kIcon0);
        // What was read, and how much (fileGet->excSize, seen by the next call).
        CHECK(p.script.call(2).get0 == 100);
        CHECK(p.script.call(3).get0 == 40);
        CHECK(p.script.call(4).get0 == 40);
        CHECK(std::string(reinterpret_cast<const char*>(base + kReadBuf), 100) == progress);
        CHECK(std::string(reinterpret_cast<const char*>(base + kReadBuf + 0x200), 40) == progress.substr(60));
        CHECK(std::string(reinterpret_cast<const char*>(base + kReadBuf + 0x400), 40) == secure);
    }
    // A load changes nothing.
    CHECK(read_file(save / "PARAM.SFO") == sfo_bytes);
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-AUTOSAVE"});
    fsys::remove_all(root);
}

TEST_CASE("cellSaveDataAutoSave2 over a save: offsets, truncation, deletes, file times, recreation", "[hle][savedata]")
{
    const fsys::path root = temp_root("update");
    const fsys::path save = savedata(root) / "BLES01807-AUTOSAVE";
    make_save(root, "BLES01807-AUTOSAVE", "Progress", "SLOT01",
        {{"SAVE.BIN", std::string(100, 'a')}, {"SECURE.DAT", std::string(40, 's')}, {"ICON0.PNG", "icon"}}, 1'500'000'000);
    set_times(save / "ICON0.PNG", 1'000'000'000);
    SaveProcess p(root);
    u8* base = p.base();
    const u32 dir = p.str("BLES01807-AUTOSAVE");
    const u32 save_bin = p.str("SAVE.BIN"), secure_dat = p.str("SECURE.DAT"), new_bin = p.str("NEW.BIN");
    write_bytes(base, kFileData, "0123456789");

    // WRITE at an offset cuts the file there; WRITE_NOTRUNC keeps the rest. Untouched files keep their times.
    p.script.step(kOkNext, stat_set(0))
        .step(kOkNext, file_set(kWrite, kNormalFile, save_bin, 50, 10, 10, kFileData))
        .step(kOkNext, file_set(kWriteNoTrunc, kSecureFile, secure_dat, 0, 4, 10, kFileData))
        .step(kOkLast);
    CHECK(p.auto_save(dir) == 0);
    CHECK(read_file(save / "SAVE.BIN") == std::string(50, 'a') + "0123456789");
    CHECK(read_file(save / "SECURE.DAT") == "0123" + std::string(36, 's'));
    CHECK(read_file(save / "ICON0.PNG") == "icon");
    CHECK(host_mtime(save / "ICON0.PNG") == 1'000'000'000);
    {
        const psf::Registry sfo = read_sfo(save / "PARAM.SFO");
        CHECK(psf::get_string(sfo, "SUB_TITLE") == "Progress"); // no setParam: the fields stay
        CHECK(psf::get_string(sfo, "RPCS3_BLIST") == "SAVE.BIN/SECURE.DAT/ICON0.PNG");
        CHECK(sfo_integer(sfo, "*SECURE.DAT") == 0); // the first write's type stays (RPCS3)
    }

    // A write past the end leaves zeros in the gap; DELETE frees its place in the order for the next new file.
    p.script.clear();
    p.script.step(kOkNext, stat_set(0))
        .step(kOkNext, file_set(kDelete, kIcon0, 0, 0, 0, 0, 0))
        .step(kOkNext, file_set(kWrite, kNormalFile, new_bin, 4, 2, 2, kFileData))
        .step(kOkLast);
    CHECK(p.auto_save(dir) == 0);
    CHECK(entries(save) == std::vector<std::string>{"NEW.BIN", "PARAM.SFO", "SAVE.BIN", "SECURE.DAT"});
    CHECK(read_file(save / "NEW.BIN") == std::string(4, '\0') + "01");
    {
        const psf::Registry sfo = read_sfo(save / "PARAM.SFO");
        CHECK(psf::get_string(sfo, "RPCS3_BLIST") == "SAVE.BIN/SECURE.DAT/NEW.BIN");
        CHECK_FALSE(sfo.contains("*ICON0.PNG"));
        CHECK(sfo_integer(sfo, "*NEW.BIN") == 0);
    }

    // Recreating drops the old files and their secure flags.
    write_param(base, "Grand Theft Auto V", "Fresh", "Started over", "SLOT02");
    p.script.clear();
    p.script.step(kOkNext, stat_set(kParamBlock, 2)) // CELL_SAVEDATA_RECREATE_YES
        .step(kOkNext, file_set(kWrite, kNormalFile, p.str("ONLY.BIN"), 0, 3, 3, kFileData))
        .step(kOkLast);
    CHECK(p.auto_save(dir) == 0);
    CHECK(entries(save) == std::vector<std::string>{"ONLY.BIN", "PARAM.SFO"});
    {
        const psf::Registry sfo = read_sfo(save / "PARAM.SFO");
        CHECK(psf::get_string(sfo, "SUB_TITLE") == "Fresh");
        CHECK(psf::get_string(sfo, "SAVEDATA_LIST_PARAM") == "SLOT02");
        CHECK(psf::get_string(sfo, "RPCS3_BLIST") == "ONLY.BIN");
        CHECK_FALSE(sfo.contains("*SAVE.BIN"));
        CHECK(sfo_integer(sfo, "*ONLY.BIN") == 0);
    }
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-AUTOSAVE"}); // no .working_ or .backup_
    fsys::remove_all(root);
}

TEST_CASE("cellSaveData: the argument and callback checks RPCS3 makes", "[hle][savedata]")
{
    const fsys::path root = temp_root("errors");
    const fsys::path save = savedata(root) / "BLES01807-AUTOSAVE";
    make_save(root, "BLES01807-AUTOSAVE", "Progress", "SLOT01", {{"SAVE.BIN", "data"}}, 1'500'000'000);
    const std::string sfo_bytes = read_file(save / "PARAM.SFO");
    SaveProcess p(root);
    u8* base = p.base();
    const u32 dir = p.str("BLES01807-AUTOSAVE");
    const u32 save_bin = p.str("SAVE.BIN");

    // A call that must fail before any callback.
    const auto refused = [&](u64 result) {
        p.script.clear();
        p.script.step(kOkLast);
        return result;
    };
    // Arguments (savedata_check_args).
    refused(0);
    CHECK(p.auto_load(dir, 0, 0, 2) == kParam); // version
    CHECK(p.auto_load(dir, 3) == kParam);       // errDialog
    CHECK(p.auto_load(0) == kParam);
    CHECK(p.auto_load(p.str("bles01807")) == kParam);                        // [A-Z0-9_-] only
    CHECK(p.auto_load(p.str("_BLES01807")) == kParam);                       // not '_' first
    CHECK(p.auto_load(p.str("BLES01807-0123456789ABCDEFGHIJKL")) == kParam); // 32 characters: SDK 3.7+ ends inside 32
    CHECK(p.script.calls() == 0);
    CHECK(p.a.call("cellSaveDataAutoLoad2", 1, dir, 0, 0, kOpdStat, kOpdFile, 0, 0) == kParam);  // no setBuf
    CHECK(p.a.call("cellSaveDataAutoLoad2", 1, dir, 0, kSetBuf, 0, kOpdFile, 0, 0) == kParam);   // no funcStat
    write_set_buf(base, 0, 16, 16 * file_stat::kSize - 1); // fileListMax doesn't fit
    CHECK(p.auto_load(dir) == kParam);
    write_set_buf(base, 0, 16, 16 * file_stat::kSize, 0); // no buffer
    CHECK(p.auto_load(dir) == kParam);
    write_set_buf(base, 0, 16, 16 * file_stat::kSize);
    ps_write32(base, kSetBuf + 8, 1); // reserved
    CHECK(p.auto_load(dir) == kParam);
    write_set_buf(base, 0, 16, 16 * file_stat::kSize);
    CHECK(p.script.calls() == 0);

    // Callback results.
    const auto stat_only = [&](s32 result, u32 err_dialog = 0) {
        p.script.clear();
        p.script.step(result);
        return p.auto_load(dir, err_dialog);
    };
    CHECK(stat_only(5) == kParam);                // not a callback result
    CHECK(stat_only(kOkLastNoConfirm) == kParam); // not allowed from funcStat
    CHECK(stat_only(kErrBroken, 1) == kCbResult); // ERRDIALOG_ALWAYS: the dialog is only logged
    CHECK(stat_only(kErrBroken, 2) == kCbResult); // NOREPEAT
    CHECK(stat_only(kErrBroken, 2) == kCbResult);
    CHECK(stat_only(kErrNoData) == kCbResult);

    // PARAM.SFO's fields (setParam) and reCreateMode.
    const auto with_param = [&](u32 recreate = 0) {
        p.script.clear();
        p.script.step(kOkNext, stat_set(kParamBlock, recreate)).step(kOkLast);
        return p.auto_save(dir);
    };
    write_param(base, "T", "S", "D", "SLOT01", 2);
    CHECK(with_param() == kParam); // attribute
    write_param(base, "T", "S", "D", "SLOT01", 0, 12);
    CHECK(with_param() == kParam); // parental level
    write_param(base, "T", "S", "D", "slot");
    CHECK(with_param() == kParam); // listParam characters
    write_param(base, "T", "S", "D", "ABCDEFGH");
    CHECK(with_param() == kParam); // listParam must end inside its 8 bytes (SDK 3.7+)
    write_param(base, "T", "S", "D", "SLOT01");
    base[kParamBlock + 0x510 + 7] = 1;
    CHECK(with_param() == kParam); // reserved
    write_param(base, "T", "S", "D", "SLOT01");
    CHECK(with_param(4) == kParam); // reCreateMode
    p.script.clear();
    p.script.step(kOkNext, stat_set(0, 2)).step(kOkLast);
    CHECK(p.auto_save(dir) == kParam); // recreating needs setParam
    CHECK(read_file(save / "PARAM.SFO") == sfo_bytes);

    // funcFile's requests; nothing was changed before them, so nothing is written.
    const auto file_op = [&](const std::vector<u32>& set, s32 result = kOkNext) {
        p.script.clear();
        p.script.step(kOkNext, stat_set(0)).step(result, set).step(kOkLast);
        return p.auto_save(dir);
    };
    CHECK(file_op(file_set(kRead, 9, save_bin, 0, 4, 4, kReadBuf)) == kParam);           // fileType
    CHECK(file_op(file_set(7, kNormalFile, save_bin, 0, 4, 4, kReadBuf)) == kParam);     // fileOperation
    CHECK(file_op(file_set(kRead, kNormalFile, 0, 0, 4, 4, kReadBuf)) == kParam);        // no file name
    CHECK(file_op(file_set(kWrite, kNormalFile, p.str("PARAM.SFO"), 0, 4, 4, kFileData)) == kParam);
    CHECK(file_op(file_set(kWrite, kNormalFile, p.str("ICON0.PNG"), 0, 4, 4, kFileData)) == kParam); // system name
    CHECK(file_op(file_set(kWrite, kNormalFile, p.str("save.bin"), 0, 4, 4, kFileData)) == kParam);
    CHECK(file_op(file_set(kWrite, kNormalFile, p.str("ABCDEFGHI.B"), 0, 4, 4, kFileData)) == kParam);   // 9.1
    CHECK(file_op(file_set(kWrite, kNormalFile, p.str("ABCDEFGH.BINX"), 0, 4, 4, kFileData)) == kParam); // 13 chars
    CHECK(file_op(file_set(kRead, kNormalFile, save_bin, 0, 8, 4, kReadBuf)) == kParam); // buffer < size
    CHECK(file_op(file_set(kRead, kNormalFile, save_bin, 0, 0, 4, 0)) == kParam);        // no buffer
    CHECK(file_op(file_set(kRead, kNormalFile, p.str("MISSING.BIN"), 0, 4, 4, kReadBuf)) == kFailure);
    CHECK(file_op(file_set(kRead, kNormalFile, save_bin, 4, 4, 4, kReadBuf)) == kFailure); // at the end
    CHECK(file_op(file_set(kDelete, kNormalFile, p.str("MISSING.BIN"), 0, 0, 0, 0)) == kFailure);
    CHECK(file_op({}, kErrNoData) == kCbResult);
    CHECK(file_op({}, -6) == kParam);
    CHECK(read_file(save / "PARAM.SFO") == sfo_bytes);
    CHECK(entries(save) == std::vector<std::string>{"PARAM.SFO", "SAVE.BIN"});

    // A callback error after a change: the change is still written (RPCS3).
    write_param(base, "T", "Changed", "D", "SLOT01");
    p.script.clear();
    p.script.step(kOkNext, stat_set(kParamBlock)).step(kErrFailure);
    CHECK(p.auto_save(dir) == kCbResult);
    CHECK(psf::get_string(read_sfo(save / "PARAM.SFO"), "SUB_TITLE") == "Changed");

    // One operation at a time: a callback starting another gets CELL_SAVEDATA_ERROR_BUSY.
    const u32 descriptor = p.f.hle->link_function("cellSysutil", 0xFBD5C856, "cellSaveDataAutoLoad2");
    REQUIRE(descriptor != 0);
    write_reenter_callback(base, descriptor, dir);
    ps_write32(base, kReenterOut, 0);
    CHECK(p.a.call("cellSaveDataAutoLoad2", 1, dir, 0, kSetBuf, kOpdReenter, kOpdFile, 0, 0) == 0);
    CHECK(ps_read32(base, kReenterOut) == static_cast<u32>(kBusy));
    fsys::remove_all(root);

    // Games built with SDKs before 3.7 may fill all 32 bytes of a directory name.
    {
        const fsys::path old_root = temp_root("oldsdk");
        SaveProcess old(old_root, kSdk36);
        old.script.step(kOkLast);
        CHECK(old.auto_load(old.str("BLES01807-0123456789ABCDEFGHIJKL")) == 0);
        CHECK(old.script.calls() == 1);
        fsys::remove_all(old_root);
    }
}

TEST_CASE("cellSaveDataListAutoLoad: funcFixed picks one of the listed saves", "[hle][savedata]")
{
    const fsys::path root = temp_root("list");
    make_save(root, "BLES01807-AUTOSAVE", "Progress", "SLOT01", {{"SAVE.BIN", "progress"}}, 2'000'000'000);
    make_save(root, "BLES01807-PROFILE", "Settings", "PROF", {{"PROF.BIN", "settings"}}, 2'000'001'000);
    make_save(root, "BLUS31156-AUTOSAVE", "Other", "X", {{"SAVE.BIN", "other"}}, 2'000'002'000);
    SaveProcess p(root);
    u8* base = p.base();
    const u32 prefix = p.str("BLES01807");
    write_set_buf(base, 4, 4, 4 * file_stat::kSize); // room for 4 directories (48 bytes) or 4 files (56)

    const auto listed = [&](u32 i) {
        return std::pair<std::string, std::string>(guest_string(base, kBuf + 48 * i), guest_string(base, kBuf + 48 * i + 32));
    };
    using Item = std::pair<std::string, std::string>;

    // Newest first (modified time, descending). funcFixed sees the list; OK_LAST ends the operation there.
    write_set_list(base, 0, 0, prefix);
    p.script.step(kOkLast);
    CHECK(p.list_auto_load() == 0);
    REQUIRE(p.script.calls() == 1);
    Call fixed = p.script.call(0);
    CHECK(fixed.tag == kTagFixed);
    CHECK(fixed.get0 == 2); // dirNum
    CHECK(fixed.get1 == 2); // dirListNum
    CHECK(fixed.userdata == 0x5EED);
    CHECK(ps_read32(base, fixed.get + list_get::dirList) == kBuf);
    CHECK(listed(0) == Item("BLES01807-PROFILE", "PROF"));
    CHECK(listed(1) == Item("BLES01807-AUTOSAVE", "SLOT01"));

    // By subtitle, ascending.
    write_set_list(base, 1, 1, prefix);
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(p.list_auto_load() == 0);
    CHECK(listed(0) == Item("BLES01807-AUTOSAVE", "SLOT01"));
    CHECK(listed(1) == Item("BLES01807-PROFILE", "PROF"));

    // Several prefixes; every match counts, the list holds dirListMax of them.
    write_set_list(base, 1, 1, p.str("BLES01807|BLUS31156"));
    write_set_buf(base, 2, 0, 2 * 48);
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(p.list_auto_load() == 0);
    fixed = p.script.call(0);
    CHECK(fixed.get0 == 3);
    CHECK(fixed.get1 == 2);
    write_set_buf(base, 4, 4, 4 * file_stat::kSize);

    // funcFixed's choice is loaded as an auto load would (no dialog for the list-auto variants).
    write_set_list(base, 0, 0, prefix);
    p.script.clear();
    p.script.step(kOkNext, fixed_set(p.str("BLES01807-PROFILE"), kOptionNone))
        .step(kOkNext, stat_set(0))
        .step(kOkNext, file_set(kRead, kNormalFile, p.str("PROF.BIN"), 0, 8, 8, kReadBuf))
        .step(kOkLast);
    CHECK(p.list_auto_load() == 0);
    CHECK(p.script.tags() == std::vector<u32>{kTagFixed, kTagStat, kTagFile, kTagFile});
    const Call stat = p.script.call(1);
    CHECK(stat.get1 == 0);
    CHECK(guest_string(base, stat.get + stat_get::dirName) == "BLES01807-PROFILE");
    CHECK(guest_string(base, stat.get + stat_get::subTitle) == "Settings");
    CHECK(ps_read32(base, stat.get + stat_get::fileNum) == 1);
    CHECK(guest_string(base, kBuf + file_stat::fileName) == "PROF.BIN"); // the buffer was cleared for the file list
    CHECK(std::string(reinterpret_cast<const char*>(base + kReadBuf), 8) == "settings");

    // A name that isn't listed is new data.
    p.script.clear();
    p.script.step(kOkNext, fixed_set(p.str("BLES01807-NEW"), kOptionNoConfirm)).step(kErrNoData);
    CHECK(p.list_auto_load() == kCbResult);
    CHECK(p.script.call(1).get1 == 1);

    // funcFixed's answer is checked.
    const auto fixed_answer = [&](s32 result, const std::vector<u32>& set) {
        p.script.clear();
        p.script.step(result, set).step(kOkLast);
        return p.list_auto_load();
    };
    CHECK(fixed_answer(kOkNext, fixed_set(0, kOptionNone)) == kParam);
    CHECK(fixed_answer(kOkNext, fixed_set(p.str("bad"), kOptionNone)) == kParam);
    CHECK(fixed_answer(kOkNext, fixed_set(p.str("BLES01807-PROFILE"), 2)) == kParam); // option
    CHECK(fixed_answer(kErrNoData, {}) == kCbResult);
    CHECK(fixed_answer(kOkLastNoConfirm, {}) == kParam);

    // And the arguments.
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(p.list_auto_load(0, 0) == kParam); // no funcFixed
    write_set_list(base, 2, 0, prefix);       // sortType
    CHECK(p.list_auto_load() == kParam);
    write_set_list(base, 0, 2, prefix); // sortOrder
    CHECK(p.list_auto_load() == kParam);
    write_set_list(base, 0, 0, 0); // no prefix
    CHECK(p.list_auto_load() == kParam);
    write_set_list(base, 0, 0, p.str("bles")); // a bad prefix
    CHECK(p.list_auto_load() == kParam);
    write_set_list(base, 0, 0, p.str("")); // empty (SDK 4.0+)
    CHECK(p.list_auto_load() == kParam);
    write_set_list(base, 0, 0, prefix);
    write_set_buf(base, 2049, 0, 2049 * 48); // more than CELL_SAVEDATA_DIRLIST_MAX
    CHECK(p.list_auto_load() == kParam);
    write_set_buf(base, 4, 0, 4 * 48 - 1); // the list doesn't fit
    CHECK(p.list_auto_load() == kParam);
    CHECK(p.a.call("cellSaveDataListAutoLoad", 1, 0, 0, kSetBuf, kOpdFixed, kOpdStat, kOpdFile, 0, 0) == kParam);
    CHECK(p.script.calls() == 0);
    fsys::remove_all(root);
}

TEST_CASE("cellSaveDataFixedDelete, cellSaveDataDelete2: deleting without dialogs", "[hle][savedata]")
{
    const fsys::path root = temp_root("delete");
    make_save(root, "BLES01807-AUTOSAVE", "Progress", "SLOT01",
        {{"SAVE.BIN", std::string(1500, 'x')}, {"MORE.BIN", std::string(10, 'y')}}, 2'000'000'000);
    make_save(root, "BLES01807-PROFILE", "Settings", "PROF", {{"PROF.BIN", "settings"}}, 2'000'001'000);
    SaveProcess p(root);
    u8* base = p.base();
    const u32 autosave = p.str("BLES01807-AUTOSAVE");
    write_set_list(base, 0, 0, p.str("BLES01807"));
    write_set_buf(base, 4, 0, 4 * 48);

    // The system's list to delete from (cellSaveDataDelete2) needs a dialog: the user cancels.
    CHECK(p.a.call("cellSaveDataDelete2", 0xFFFFFFFFu) == kCancel);
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-AUTOSAVE", "BLES01807-PROFILE"});

    // Without OPTION_NOCONFIRM the user has to confirm: there is no dialog UI, so the answer is YES (D-183).
    make_save(root, "BLES01807-EXTRA", "Extra", "X", {{"X.BIN", "x"}}, 2'000'000'500);
    p.script.step(kOkNext, fixed_set(p.str("BLES01807-EXTRA"), kOptionNone)).step(kOkNext);
    CHECK(p.fixed_delete() == 0);
    CHECK(p.script.tags() == std::vector<u32>{kTagFixed, kTagDone});
    CHECK_FALSE(fsys::exists(savedata(root) / "BLES01807-EXTRA"));
    CHECK(fsys::is_directory(savedata(root) / "BLES01807-AUTOSAVE"));

    // NOCONFIRM: deleted, and funcDone hears it. Its size counts every file, PARAM.SFO too, each rounded up to 1 KB
    // (RPCS3 delete_save).
    const u64 sfo_kb = (fsys::file_size(savedata(root) / "BLES01807-AUTOSAVE" / "PARAM.SFO") + 1023) / 1024;
    p.script.clear();
    p.script.step(kOkNext, fixed_set(autosave, kOptionNoConfirm)).step(kOkNext);
    CHECK(p.fixed_delete() == 0);
    CHECK(p.script.tags() == std::vector<u32>{kTagFixed, kTagDone});
    const Call done = p.script.call(1);
    CHECK(done.get0 == 0); // excResult
    CHECK(guest_string(base, done.get + done_get::dirName) == "BLES01807-AUTOSAVE");
    CHECK(ps_read32(base, done.get + done_get::sizeKB) == 2 + 1 + sfo_kb); // 1500 and 10 bytes, and PARAM.SFO
    CHECK(ps_read32(base, done.get + done_get::hddFreeSizeKB) == static_cast<u32>(kHddFreeSizeKB));
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-PROFILE"});

    // A save that isn't there: funcDone gets CELL_SAVEDATA_ERROR_NODATA.
    p.script.clear();
    p.script.step(kOkNext, fixed_set(autosave, kOptionNoConfirm)).step(kOkLast);
    CHECK(p.fixed_delete() == 0);
    REQUIRE(p.script.calls() == 2);
    CHECK(p.script.call(1).get0 == static_cast<u32>(kNoData));

    // funcDone's error; funcFixed's OK_LAST (nothing deleted); the arguments.
    p.script.clear();
    p.script.step(kOkNext, fixed_set(p.str("BLES01807-PROFILE"), kOptionNoConfirm)).step(kErrFailure);
    CHECK(p.fixed_delete() == kCbResult);
    CHECK(fsys::is_empty(savedata(root)));
    make_save(root, "BLES01807-PROFILE", "Settings", "PROF", {{"PROF.BIN", "settings"}}, 2'000'001'000);
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(p.fixed_delete() == 0);
    CHECK(p.fixed_delete(0) == kParam);            // no funcFixed
    write_set_buf(base, 4, 1, 4 * file_stat::kSize); // fileListMax must be 0
    CHECK(p.fixed_delete() == kParam);
    write_set_buf(base, 4, 0, 4 * 48);
    p.script.clear();
    p.script.step(kOkNext, fixed_set(p.str("BLES01807-PROFILE"), kOptionNoConfirm));
    CHECK(p.fixed_delete(kOpdFixed, 0) == kParam); // no funcDone: refused before deleting
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-PROFILE"});
    fsys::remove_all(root);
}

TEST_CASE("cellSaveDataGetListItem: one save's directory, PARAM.SFO fields and size", "[hle][savedata]")
{
    const fsys::path root = temp_root("item");
    make_save(root, "BLES01807-PROFILE", "Settings", "PROF",
        {{"PROF.BIN", std::string(2000, 'p')}, {"ICON0.PNG", "icon"}}, 2'000'001'000);
    SaveProcess p(root);
    u8* base = p.base();
    constexpr u32 kDir = kData + 0x43000, kParamOut = kData + 0x44000, kBind = kData + 0x45000, kSize = kBind + 4;
    std::memset(base + kDir, 0xAA, 56);
    std::memset(base + kParamOut, 0xAA, 0x610);
    ps_write32(base, kBind, 0xAAAAAAAA);
    const u32 name = p.str("BLES01807-PROFILE");
    CHECK(p.a.call("cellSaveDataGetListItem", name, kDir, kParamOut, kBind, kSize) == 0);
    CHECK(static_cast<s64>(ps_read64(base, kDir + 8)) == 2'000'001'000); // mtime
    CHECK(static_cast<s64>(ps_read64(base, kDir + 0)) == 2'000'001'000); // atime (at least mtime)
    CHECK(static_cast<s64>(ps_read64(base, kDir + 16)) == 2'000'001'000); // ctime: reported as mtime (RPCS3)
    CHECK(guest_string(base, kDir + 24) == "BLES01807-PROFILE");
    CHECK(guest_string(base, kParamOut + 0x000) == "Grand Theft Auto V");
    CHECK(guest_string(base, kParamOut + 0x080) == "Settings");
    CHECK(guest_string(base, kParamOut + 0x100) == "A detail");
    CHECK(guest_string(base, kParamOut + 0x508) == "PROF");
    CHECK(ps_read32(base, kBind) == 0);
    CHECK(ps_read32(base, kSize) == 2 + 1 + 35); // 2000 bytes and 4 bytes rounded up, + 35 KB
    CHECK(p.a.call("cellSaveDataGetListItem", name, 0, 0, 0, 0) == 0); // every output is optional

    CHECK(p.a.call("cellSaveDataGetListItem", p.str("BLES01807-NONE"), kDir, 0, 0, 0) == kNoData);
    CHECK(p.a.call("cellSaveDataGetListItem", 0, kDir, 0, 0, 0) == kParam);
    CHECK(p.a.call("cellSaveDataGetListItem", p.str("profile"), kDir, 0, 0, 0) == kParam);
    {
        SaveProcess none({});
        CHECK(none.a.call("cellSaveDataGetListItem", none.str("BLES01807-PROFILE"), 0, 0, 0, 0) == kNoUser);
    }
    fsys::remove_all(root);
}

namespace {

// CellSaveDataListSet as script words, and a CellSaveDataListNewData and a fixedList in guest memory.
constexpr u32 kNewData = kData + 0x46000, kFixedList = kData + 0x46100;
enum : u32 { kFocusDirName = 0, kFocusHead = 1, kFocusTail = 2, kFocusLatest = 3, kFocusOldest = 4, kFocusNew = 5 };
std::vector<u32> list_set(u32 focus, u32 focus_dir = 0, u32 fixed_num = 0, u32 fixed_list = 0, u32 new_data = 0)
{
    return {focus, focus_dir, fixed_num, fixed_list, new_data, 0};
}

void write_new_data(u8* base, u32 icon_position, u32 dir)
{
    ps_write32(base, kNewData, icon_position);
    ps_write32(base, kNewData + 4, dir);
    ps_write32(base, kNewData + 8, 0);
    ps_write32(base, kNewData + 12, 0);
}

void write_fixed_list(u8* base, const std::vector<std::string_view>& names)
{
    std::memset(base + kFixedList, 0, 48 * names.size());
    for (usize i = 0; i < names.size(); i++) write_bytes(base, kFixedList + 48 * static_cast<u32>(i), names[i]);
}

} // namespace

TEST_CASE("cellSaveDataListLoad2, cellSaveDataListSave2: funcList's list, the save the cursor is on (D-183)",
    "[hle][savedata]")
{
    const fsys::path root = temp_root("listsel");
    make_save(root, "BLES01807-SLOT1", "One", "A", {{"S.BIN", "one"}}, 2'000'000'000);
    make_save(root, "BLES01807-SLOT2", "Two", "B", {{"S.BIN", "two"}}, 2'000'002'000);
    make_save(root, "BLES01807-SLOT3", "Three", "C", {{"S.BIN", "three"}}, 2'000'001'000);
    SaveProcess p(root);
    u8* base = p.base();
    write_set_list(base, 0, 0, p.str("BLES01807"));
    write_set_buf(base, 4, 4, 4 * file_stat::kSize);
    const auto list_load = [&](u32 func_list = kOpdList) {
        return p.a.call("cellSaveDataListLoad2", 1, kSetList, kSetBuf, func_list, kOpdStat, kOpdFile, 0xFFFFFFFFu,
            0x77);
    };
    const auto list_save = [&]() {
        return p.a.call("cellSaveDataListSave2", 1, kSetList, kSetBuf, kOpdList, kOpdStat, kOpdFile, 0xFFFFFFFFu, 0);
    };
    // The save funcStat is called for, after funcList set `set` (funcStat then ends the operation).
    const auto chosen = [&](const std::vector<u32>& set, bool save = false) -> std::string {
        p.script.clear();
        p.script.step(kOkNext, set).step(kOkLast);
        const u64 r = save ? list_save() : list_load();
        if (r != 0 || p.script.calls() != 2) return "result " + std::to_string(r);
        return guest_string(base, p.script.call(1).get + stat_get::dirName);
    };

    // funcList sees the list (dirNum, dirListNum, the userdata). Games show what it lists by naming those saves in
    // fixedList (an empty fixedList shows none, as RPCS3's); its order is the list's, newest first here.
    write_fixed_list(base, {"BLES01807-SLOT2", "BLES01807-SLOT3", "BLES01807-SLOT1"});
    const auto all = [&](u32 focus, u32 focus_dir = 0) { return list_set(focus, focus_dir, 3, kFixedList); };
    CHECK(chosen(all(kFocusHead)) == "BLES01807-SLOT2");
    Call list = p.script.call(0);
    CHECK(list.tag == kTagList);
    CHECK(list.get0 == 3);
    CHECK(list.get1 == 3);
    CHECK(list.userdata == 0x77);
    CHECK(p.script.call(1).tag == kTagStat);
    CHECK(chosen(all(kFocusTail)) == "BLES01807-SLOT1");
    CHECK(chosen(all(kFocusLatest)) == "BLES01807-SLOT2");
    CHECK(chosen(all(kFocusOldest)) == "BLES01807-SLOT1");
    CHECK(chosen(all(kFocusDirName, p.str("BLES01807-SLOT3"))) == "BLES01807-SLOT3");
    CHECK(chosen(all(kFocusDirName, p.str("BLES01807-NONE"))) == "BLES01807-SLOT2"); // not shown: the first
    CHECK(chosen(list_set(kFocusHead)) == "result 1"); // nothing shown, no new save: the user cancels

    // fixedList: only the saves it names are shown.
    write_fixed_list(base, {"BLES01807-SLOT3", "BLES01807-SLOT1"});
    CHECK(chosen(list_set(kFocusHead, 0, 2, kFixedList)) == "BLES01807-SLOT3");
    CHECK(chosen(list_set(kFocusDirName, p.str("BLES01807-SLOT2"), 2, kFixedList)) == "BLES01807-SLOT3");

    // newData: the new save when the cursor is on it, or on nothing listed.
    const u32 fresh = p.str("BLES01807-SLOT9");
    write_new_data(base, 0, fresh);
    CHECK(chosen(list_set(kFocusNew, 0, 0, 0, kNewData), true) == "BLES01807-SLOT9");
    CHECK(ps_read32(base, p.script.call(1).get + stat_get::isNewData) == 1);
    CHECK(chosen(list_set(kFocusHead, 0, 0, 0, kNewData), true) == "BLES01807-SLOT9"); // nothing shown
    write_fixed_list(base, {"BLES01807-SLOT1"});
    CHECK(chosen(list_set(kFocusHead, 0, 1, kFixedList, kNewData), true) == "BLES01807-SLOT1");

    // A save made through the list.
    write_param(base, "Title", "Sub", "Detail", "LP");
    p.script.clear();
    p.script.step(kOkNext, list_set(kFocusNew, 0, 0, 0, kNewData))
        .step(kOkNext, stat_set(kParamBlock))
        .step(kOkNext, file_set(kWrite, kNormalFile, p.str("NEW.BIN"), 0, 3, 3, p.str("new")))
        .step(kOkLast);
    CHECK(list_save() == 0);
    CHECK(read_file(savedata(root) / "BLES01807-SLOT9" / "NEW.BIN") == "new");

    // Nothing to pick: the user cancels. funcList's OK_LAST ends the operation with CELL_OK.
    write_fixed_list(base, {"BLES01807-GONE"});
    CHECK(chosen(list_set(kFocusHead, 0, 1, kFixedList)) == "result 1");
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(list_load() == 0);
    CHECK(p.script.calls() == 1);
    p.script.clear();
    p.script.step(kErrFailure);
    CHECK(list_load() == kCbResult);

    // funcList's answer is checked (RPCS3's parameter errors 34 to 46).
    const auto answer = [&](const std::vector<u32>& set) {
        p.script.clear();
        p.script.step(kOkNext, set).step(kOkLast);
        return list_load();
    };
    CHECK(answer(list_set(6)) == kParam);                               // 34: focusPosition
    CHECK(answer(list_set(kFocusNew)) == kParam);                       // 34: NEWDATA without newData
    CHECK(answer(list_set(kFocusDirName)) == kParam);                   // 35
    CHECK(answer(list_set(kFocusDirName, p.str("bad"))) == kParam);     // 36
    CHECK(answer(list_set(kFocusHead, 0, 2049, kFixedList)) == kParam); // 38
    CHECK(answer(list_set(kFocusHead, 0, 1, 0)) == kParam);             // 39
    write_fixed_list(base, {"bad"});
    CHECK(answer(list_set(kFocusHead, 0, 1, kFixedList)) == kParam); // 40
    write_new_data(base, 2, fresh);
    CHECK(answer(list_set(kFocusHead, 0, 0, 0, kNewData)) == kParam); // 43
    write_new_data(base, 1, 0);
    CHECK(answer(list_set(kFocusHead, 0, 0, 0, kNewData)) == kParam); // 44
    write_new_data(base, 1, p.str("bad"));
    CHECK(answer(list_set(kFocusHead, 0, 0, 0, kNewData)) == kParam); // 45
    CHECK(list_load(0) == kParam);                                    // 18: no funcList
    fsys::remove_all(root);
}

TEST_CASE("cellSaveDataFixedLoad2/FixedSave2, the user variants, list delete, import and export (D-183)",
    "[hle][savedata]")
{
    const fsys::path root = temp_root("fixed");
    make_save(root, "BLES01807-SLOT1", "One", "A", {{"S.BIN", "one"}}, 2'000'000'000);
    SaveProcess p(root);
    u8* base = p.base();
    write_set_list(base, 0, 0, p.str("BLES01807"));
    write_set_buf(base, 4, 4, 4 * file_stat::kSize);
    const u32 slot1 = p.str("BLES01807-SLOT1");

    // Fixed: funcFixed names the save; without NOCONFIRM the user confirms (YES, no dialog UI).
    p.script.step(kOkNext, fixed_set(slot1, kOptionNone))
        .step(kOkNext, stat_set(0))
        .step(kOkNext, file_set(kRead, kNormalFile, p.str("S.BIN"), 0, 3, 3, kReadBuf))
        .step(kOkLast);
    CHECK(p.a.call("cellSaveDataFixedLoad2", 1, kSetList, kSetBuf, kOpdFixed, kOpdStat, kOpdFile, 0xFFFFFFFFu, 0) == 0);
    CHECK(p.script.tags() == std::vector<u32>{kTagFixed, kTagStat, kTagFile, kTagFile});
    CHECK(std::string(reinterpret_cast<const char*>(base + kReadBuf), 3) == "one");
    write_param(base, "Title", "Sub", "Detail", "LP");
    p.script.clear();
    p.script.step(kOkNext, fixed_set(p.str("BLES01807-SLOT2"), kOptionNoConfirm))
        .step(kOkNext, stat_set(kParamBlock))
        .step(kOkNext, file_set(kWrite, kNormalFile, p.str("T.BIN"), 0, 3, 3, p.str("two")))
        .step(kOkLast);
    CHECK(p.a.call("cellSaveDataFixedSave", 1, kSetList, kSetBuf, kOpdFixed, kOpdStat, kOpdFile, 0xFFFFFFFFu) == 0);
    CHECK(read_file(savedata(root) / "BLES01807-SLOT2" / "T.BIN") == "two");

    // The user variants: user 1 is the current user; a user with no directory is NOUSER; past the maximum, PARAM.
    p.script.clear();
    p.script.step(kOkLast);
    CHECK(p.a.call("cellSaveDataUserAutoLoad", 1, 1, slot1, 0, kSetBuf, kOpdStat, kOpdFile, 0xFFFFFFFFu, 0) == 0);
    CHECK(p.script.calls() == 1);
    CHECK(guest_string(base, p.script.call(0).get + stat_get::dirName) == "BLES01807-SLOT1");
    CHECK(p.a.call("cellSaveDataUserAutoLoad", 1, 2, slot1, 0, kSetBuf, kOpdStat, kOpdFile, 0xFFFFFFFFu, 0) ==
        kNoUser);
    CHECK(p.a.call("cellSaveDataUserAutoLoad", 1, 100000000, slot1, 0, kSetBuf, kOpdStat, kOpdFile, 0xFFFFFFFFu, 0) ==
        kParam);
    p.script.clear();
    p.script.step(kOkNext, fixed_set(slot1, kOptionNoConfirm)).step(kOkLast);
    CHECK(p.a.call("cellSaveDataUserFixedLoad", 1, 1, kSetList, kSetBuf, kOpdFixed, kOpdStat, kOpdFile, 0xFFFFFFFFu,
              0) == 0);
    CHECK(p.script.tags() == std::vector<u32>{kTagFixed, kTagStat});
    constexpr u32 kSizeOut = kData + 0x45100;
    CHECK(p.a.call("cellSaveDataUserGetListItem", 1, slot1, 0, 0, 0, kSizeOut) == 0);
    CHECK(p.a.call("cellSaveDataUserGetListItem", 3, slot1, 0, 0, 0, kSizeOut) == kNoUser);

    // The list delete is left at once: nothing deleted. '*' lists the game's saves there.
    write_set_list(base, 0, 0, p.str("*"));
    write_set_buf(base, 4, 0, 4 * 48);
    p.script.clear();
    p.script.step(kOkNext, list_set(kFocusHead));
    CHECK(p.a.call("cellSaveDataListDelete", kSetList, kSetBuf, kOpdList, kOpdDone, 0xFFFFFFFFu, 0) == kCancel);
    CHECK(p.script.tags() == std::vector<u32>{kTagList});
    CHECK(p.script.call(0).get0 == 2);
    CHECK(entries(savedata(root)) == std::vector<std::string>{"BLES01807-SLOT1", "BLES01807-SLOT2"});
    CHECK(p.a.call("cellSaveDataDelete", 0xFFFFFFFFu) == kCancel);

    // Import and export: RPCS3's checks, then CELL_OK; funcDone is not called.
    write_set_list(base, 0, 0, p.str("BLES01807"));
    p.script.clear();
    CHECK(p.a.call("cellSaveDataListImport", kSetList, 1024, kOpdDone, 0xFFFFFFFFu, 0) == 0);
    CHECK(p.a.call("cellSaveDataListExport", kSetList, 1024, 0, 0xFFFFFFFFu, 0) == kParam); // 137: no funcDone
    CHECK(p.a.call("cellSaveDataFixedExport", slot1, 1024, kOpdDone, 0xFFFFFFFFu, 0) == 0);
    CHECK(p.a.call("cellSaveDataFixedImport", p.str("bad"), 1024, kOpdDone, 0xFFFFFFFFu, 0) == kParam);
    CHECK(p.a.call("cellSaveDataUserFixedImport", 1, slot1, 1024, kOpdDone, 0xFFFFFFFFu, 0) == 0);
    CHECK(p.a.call("cellSaveDataUserListExport", 100000000, kSetList, 1024, kOpdDone, 0xFFFFFFFFu, 0) == kParam);
    CHECK(p.script.calls() == 0);

    // The overlay switch.
    p.a.call("cellSaveDataEnableOverlay", 1);
    CHECK(p.a.call("cellSaveDataGetEnableOverlay") == 0);
    fsys::remove_all(root);
}
