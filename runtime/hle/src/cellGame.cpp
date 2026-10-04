// cellGame: the game's content directories (the disc's PS3_GAME, game data under /dev_hdd0/game) and their
// PARAM.SFO. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellGame.cpp/.h (content_permission and the check / permit
// session; the PS3's delays are not reproduced).
//
// A check (cellGameBootCheck or cellGameDataCheck) opens a session that cellGameContentPermit closes; a second check
// while one is open is CELL_GAME_ERROR_BUSY. The disc's PARAM.SFO comes from the host (ProcessConfig::game), game
// data lives in host directories under the /dev_hdd0 mount. Deviation: cellGameContentErrorDialog logs its message
// (there is no dialog UI yet) and returns CELL_OK; for the "exit" types the game is expected to quit by itself.

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

#include "guest.hpp"
#include "psf.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellGame";

enum : u32 {
    CELL_GAME_RET_NONE = 2,
    CELL_GAME_ERROR_BROKEN = 0x8002CB05,
    CELL_GAME_ERROR_PARAM = 0x8002CB07,
    CELL_GAME_ERROR_ACCESS_ERROR = 0x8002CB09,
    CELL_GAME_ERROR_NOTSUPPORTED = 0x8002CB21,
    CELL_GAME_ERROR_FAILURE = 0x8002CB22,
    CELL_GAME_ERROR_BUSY = 0x8002CB23,
    CELL_GAME_ERROR_INVALID_ID = 0x8002CB25,
    CELL_GAME_ERROR_EXIST = 0x8002CB26,
};
constexpr u32 CELL_GAME_GAMETYPE_DISC = 1, CELL_GAME_GAMETYPE_HDD = 2, CELL_GAME_GAMETYPE_GAMEDATA = 3;
constexpr s32 CELL_GAME_SIZEKB_NOTCALC = -1;
constexpr u32 CELL_GAME_PATH_MAX = 128, CELL_GAME_DIRNAME_SIZE = 32;
constexpr u32 CELL_GAME_SYSP_TITLE_SIZE = 128, CELL_GAME_SYSP_TITLEID_SIZE = 10, CELL_GAME_SYSP_VERSION_SIZE = 6;
constexpr u32 TITLEID_SFO_ENTRY_SIZE = 16; // RPCS3 cellGame.h: the size a PS3 writes
// RPCS3 reports 40 GB free less 256 KB (see its cellHddGameCheck) and these system sizes.
constexpr s32 kHddFreeSizeKB = 40 * 1024 * 1024 - 256;

s32 error(u32 code) { return static_cast<s32>(code); }

// RPCS3 content_permission.
struct Permission {
    enum class Mode { NotSet, GameData, Patch, HddGame, DiscGame };
    std::mutex mutex;
    bool open = false; // a check opened the session (RPCS3's init_mutex)
    Mode mode = Mode::NotSet;
    std::string dir;  // game data directory name; empty for the disc
    psf::Registry sfo;
    std::string temp; // host path of the directory cellGameCreateGameData made, until ContentPermit moves it
    bool can_create = false;
    bool exists = false;

    void reset()
    {
        open = false;
        mode = Mode::NotSet;
        dir.clear();
        sfo.clear();
        temp.clear();
        can_create = false;
        exists = false;
    }
};

Permission& permission()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellGame called without an HLE process");
    return p->state<Permission>();
}

const GameInfo& game() { return HleProcess::current()->config().game; }

void write_size(u8* base, u32 size, s32 size_kb, s32 sys_size_kb)
{
    if (!size) return;
    ps_write32(base, size + 0, static_cast<u32>(kHddFreeSizeKB));
    ps_write32(base, size + 4, static_cast<u32>(size_kb));
    ps_write32(base, size + 8, static_cast<u32>(sys_size_kb));
}

// strcpy_trunc into a guest buffer of `capacity` bytes.
void put_string(u8* base, u32 addr, u32 capacity, std::string_view s)
{
    if (!capacity) return;
    const usize n = std::min<usize>(s.size(), capacity - 1);
    std::memcpy(base + addr, s.data(), n);
    base[addr + n] = 0;
}

std::string get_string(lv2::PpuThread& t, u32 addr, u32 max)
{
    std::string out;
    for (u32 i = 0; i < max && guest_readable(t, addr + i, 1); i++) {
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        out += c;
    }
    return out;
}

enum class SfoLoad { Ok, Missing, Corrupt };

// A PARAM.SFO from a host file: Missing when there is no file (RPCS3 psf::error::stream).
SfoLoad load_sfo(const std::string& host_path, psf::Registry& out)
{
    out.clear();
    std::ifstream in(host_path, std::ios::binary);
    if (host_path.empty() || !in) return SfoLoad::Missing;
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = psf::parse(bytes);
    if (!parsed) return SfoLoad::Corrupt;
    out = std::move(*parsed);
    return SfoLoad::Ok;
}

// fs::pending_file: write next to the target, then rename over it.
bool save_sfo(const std::string& host_path, const psf::Registry& sfo)
{
    const std::vector<u8> bytes = psf::save(sfo);
    const std::string tmp = host_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, host_path, ec);
    return !ec;
}

const char* type_name(u32 type)
{
    return type == CELL_GAME_GAMETYPE_DISC ? "DISC" : type == CELL_GAME_GAMETYPE_HDD ? "HDD" : "GAMEDATA";
}

// RPCS3 cellGameBootCheck.
s32 cellGameBootCheck(PPUContext& ctx, u32 type, u32 attributes, u32 size, u32 dir_name)
{
    if (!type || !attributes) return error(CELL_GAME_ERROR_PARAM);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (perm.open) return error(CELL_GAME_ERROR_BUSY);
    u8* base = guest_base(ctx);
    const GameInfo& g = game();
    psf::Registry sfo;
    if (auto parsed = psf::parse(g.param_sfo)) sfo = std::move(*parsed);
    std::string dir;
    if (g.boot == GameInfo::Boot::Disc) {
        perm.mode = Permission::Mode::DiscGame;
        ps_write32(base, type, CELL_GAME_GAMETYPE_DISC);
    } else {
        perm.mode = Permission::Mode::HddGame;
        ps_write32(base, type, CELL_GAME_GAMETYPE_HDD);
        dir = g.title_id;
        if (dir_name) put_string(base, dir_name, CELL_GAME_DIRNAME_SIZE, dir);
    }
    ps_write32(base, attributes, 0);
    write_size(base, size, CELL_GAME_SIZEKB_NOTCALC, 4);
    perm.open = true;
    perm.dir = std::move(dir);
    perm.sfo = std::move(sfo);
    perm.exists = true;
    PS_LOG_INFO(kChan, "cellGameBootCheck: {} game {}", g.boot == GameInfo::Boot::Disc ? "disc" : "HDD",
        psf::get_string(perm.sfo, "TITLE_ID"));
    return lv2::CELL_OK;
}

// RPCS3 cellGameDataCheck: a missing directory is CELL_GAME_RET_NONE (not an error), with the session left open so
// the game can create it.
s32 cellGameDataCheck(PPUContext& ctx, u32 type, u32 dir_name, u32 size)
{
    if (type - 1 >= 3 || (type != CELL_GAME_GAMETYPE_DISC && !dir_name)) return error(CELL_GAME_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    const std::string name = type == CELL_GAME_GAMETYPE_DISC ? std::string() : get_string(t, dir_name, CELL_GAME_DIRNAME_SIZE);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (perm.open) return error(CELL_GAME_ERROR_BUSY);

    psf::Registry sfo;
    SfoLoad load = SfoLoad::Missing;
    if (type == CELL_GAME_GAMETYPE_DISC) {
        if (auto parsed = psf::parse(game().param_sfo)) {
            sfo = std::move(*parsed);
            load = SfoLoad::Ok;
        }
    } else {
        const std::string host = t.kernel.vfs_host_path("/dev_hdd0/game/" + name + "/PARAM.SFO");
        if (host.empty()) PS_LOG_WARN(kChan, "cellGameDataCheck: /dev_hdd0 is not mounted on a host directory");
        load = load_sfo(host, sfo);
    }
    const std::string cat = psf::get_string(sfo, "CATEGORY");
    const bool wrong_category = type == CELL_GAME_GAMETYPE_HDD ? !(cat.size() == 2 && cat[1] != 'D' && cat != "MS")
        : type == CELL_GAME_GAMETYPE_GAMEDATA                  ? cat != "GD"
                                                               : cat != "DG";
    if (wrong_category && load != SfoLoad::Missing) {
        PS_LOG_ERROR(kChan, "cellGameDataCheck({}, \"{}\"): PARAM.SFO {} (category \"{}\")", type_name(type), name,
            load == SfoLoad::Corrupt ? "is corrupt" : "has the wrong category", cat);
        return error(CELL_GAME_ERROR_BROKEN);
    }
    write_size(guest_base(ctx), size, sfo.empty() ? 0 : CELL_GAME_SIZEKB_NOTCALC, 0);
    perm.open = true;
    perm.dir = name;
    perm.can_create = type == CELL_GAME_GAMETYPE_GAMEDATA;
    perm.mode = Permission::Mode::GameData;
    if (sfo.empty()) {
        PS_LOG_INFO(kChan, "cellGameDataCheck({}, \"{}\"): not found (CELL_GAME_RET_NONE)", type_name(type), name);
        return static_cast<s32>(CELL_GAME_RET_NONE);
    }
    perm.exists = true;
    perm.sfo = std::move(sfo);
    PS_LOG_INFO(kChan, "cellGameDataCheck({}, \"{}\"): found", type_name(type), name);
    return lv2::CELL_OK;
}

// RPCS3 cellGameContentPermit: ends the session. New game data (cellGameCreateGameData) gets its PARAM.SFO and moves
// to its final directory; existing game data that may be written gets its PARAM.SFO rewritten.
s32 cellGameContentPermit(PPUContext& ctx, u32 content_info_path, u32 usrdir_path)
{
    if (!content_info_path || !usrdir_path) return error(CELL_GAME_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (!perm.open) return error(CELL_GAME_ERROR_FAILURE);
    const std::string dir = perm.dir.empty() ? "/dev_bdvd/PS3_GAME" : "/dev_hdd0/game/" + perm.dir;
    if (perm.temp.empty() && !perm.exists) {
        perm.reset();
        put_string(base, content_info_path, CELL_GAME_PATH_MAX, "");
        put_string(base, usrdir_path, CELL_GAME_PATH_MAX, "");
        PS_LOG_INFO(kChan, "cellGameContentPermit: nothing to permit ({} does not exist)", dir);
        return lv2::CELL_OK;
    }
    if (!perm.temp.empty()) {
        const std::string real = t.kernel.vfs_host_path(dir);
        std::error_code ec;
        if (!save_sfo(perm.temp + "/PARAM.SFO", perm.sfo)) {
            PS_LOG_ERROR(kChan, "cellGameContentPermit: cannot write {}/PARAM.SFO", perm.temp);
        } else if (std::filesystem::rename(perm.temp, real, ec); ec) {
            PS_LOG_ERROR(kChan, "cellGameContentPermit: cannot create {} ({})", dir, ec.message());
        } else {
            PS_LOG_INFO(kChan, "cellGameContentPermit: game data {} created", dir);
        }
    } else if (perm.can_create) {
        if (!save_sfo(t.kernel.vfs_host_path(dir + "/PARAM.SFO"), perm.sfo)) {
            PS_LOG_ERROR(kChan, "cellGameContentPermit: cannot update {}/PARAM.SFO", dir);
        }
    }
    perm.reset();
    put_string(base, content_info_path, CELL_GAME_PATH_MAX, dir);
    put_string(base, usrdir_path, CELL_GAME_PATH_MAX, dir + "/USRDIR");
    PS_LOG_INFO(kChan, "cellGameContentPermit: {} and {}/USRDIR", dir, dir);
    return lv2::CELL_OK;
}

// RPCS3 cellGameCreateGameData: a temporary directory (with USRDIR) that cellGameContentPermit renames to the
// checked name. CellGameSetInitParams: title[128], titleId[10], reserved[2], version[6], reserved[66].
s32 cellGameCreateGameData(PPUContext& ctx, u32 init, u32 tmp_content_info_path, u32 tmp_usrdir_path)
{
    if (!init) return error(CELL_GAME_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (!perm.open || perm.dir.empty()) return error(CELL_GAME_ERROR_FAILURE);
    if (!perm.can_create) return error(CELL_GAME_ERROR_NOTSUPPORTED);
    if (perm.exists) return error(CELL_GAME_ERROR_EXIST);

    const std::string name =
        "_GDATA_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string tmp_content = "/dev_hdd0/game/" + name, tmp_usrdir = tmp_content + "/USRDIR";
    const std::string host = t.kernel.vfs_host_path(tmp_content);
    std::error_code ec;
    if (host.empty() || !std::filesystem::create_directory(host, ec)) {
        PS_LOG_ERROR(kChan, "cellGameCreateGameData: cannot create {}{}", tmp_content,
            host.empty() ? " (/dev_hdd0 is not mounted)" : "");
        return error(CELL_GAME_ERROR_ACCESS_ERROR);
    }
    if (tmp_content_info_path) put_string(base, tmp_content_info_path, CELL_GAME_PATH_MAX, tmp_content);
    if (!std::filesystem::create_directory(host + "/USRDIR", ec)) {
        PS_LOG_ERROR(kChan, "cellGameCreateGameData: cannot create {}", tmp_usrdir);
        return error(CELL_GAME_ERROR_ACCESS_ERROR);
    }
    if (tmp_usrdir_path) put_string(base, tmp_usrdir_path, CELL_GAME_PATH_MAX, tmp_usrdir);
    perm.temp = host;
    const auto field = [&](u32 offset, u32 size) { return get_string(t, init + offset, size); };
    perm.sfo.clear();
    perm.sfo["CATEGORY"] = psf::string(3, "GD");
    perm.sfo["TITLE_ID"] = psf::string(TITLEID_SFO_ENTRY_SIZE, field(CELL_GAME_SYSP_TITLE_SIZE, CELL_GAME_SYSP_TITLEID_SIZE));
    perm.sfo["TITLE"] = psf::string(CELL_GAME_SYSP_TITLE_SIZE, field(0, CELL_GAME_SYSP_TITLE_SIZE));
    perm.sfo["VERSION"] = psf::string(CELL_GAME_SYSP_VERSION_SIZE, field(140, CELL_GAME_SYSP_VERSION_SIZE));
    PS_LOG_INFO(kChan, "cellGameCreateGameData: {} for \"{}\" ({})", tmp_content, perm.dir,
        psf::get_string(perm.sfo, "TITLE"));
    return lv2::CELL_OK;
}

// RPCS3 get_param_string_key: the PARAM.SFO key of an ID, and whether game data (or the disc / HDD game) may read it.
struct StringKey {
    std::string_view name;
    bool game_data = false;
    bool other = false;
};

StringKey param_string_key(s32 id)
{
    static constexpr std::string_view kTitles[] = {"TITLE_00", "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04",
        "TITLE_05", "TITLE_06", "TITLE_07", "TITLE_08", "TITLE_09", "TITLE_10", "TITLE_11", "TITLE_12", "TITLE_13",
        "TITLE_14", "TITLE_15", "TITLE_16", "TITLE_17", "TITLE_18", "TITLE_19"};
    if (id == 0 || id == 1) return {"TITLE", true, true};             // TITLE, TITLE_DEFAULT
    if (id >= 2 && id <= 21) return {kTitles[id - 2], true, false};   // the localized titles
    if (id == 100) return {"TITLE_ID", true, true};
    if (id == 101) return {"VERSION", true, false};
    if (id == 105) return {"PS3_SYSTEM_VER", false, false};
    if (id == 106) return {"APP_VER", true, true};
    return {};
}

s32 cellGameGetParamString(PPUContext& ctx, s32 id, u32 buf, u32 bufsize)
{
    if (!buf || !bufsize) return error(CELL_GAME_ERROR_PARAM);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (!perm.open || perm.mode == Permission::Mode::NotSet) return error(CELL_GAME_ERROR_FAILURE);
    const StringKey key = param_string_key(id);
    if (key.name.empty()) return error(CELL_GAME_ERROR_INVALID_ID);
    const bool game_data_mode = perm.mode == Permission::Mode::GameData || perm.mode == Permission::Mode::Patch;
    if (!(game_data_mode ? key.game_data : key.other)) return error(CELL_GAME_ERROR_NOTSUPPORTED);
    const std::string value = psf::get_string(perm.sfo, key.name);
    if (value.empty() && !perm.sfo.contains(key.name)) PS_LOG_WARN(kChan, "cellGameGetParamString: {} not found", key.name);
    put_string(guest_base(ctx), buf, bufsize, value);
    return lv2::CELL_OK;
}

// RPCS3 opens an error dialog; the exit types (100..102) end the game once it closes.
s32 cellGameContentErrorDialog(PPUContext& ctx, s32 type, s32 need_size_kb, u32 dir_name)
{
    std::string_view message;
    switch (type) {
    case 0: message = "Game data is corrupted. The application will continue."; break;
    case 1: message = "HDD boot game is corrupted. The application will continue."; break;
    case 2: message = "Not enough available space. The application will continue."; break;
    case 100: message = "Game data is corrupted. The application will be terminated."; break;
    case 101: message = "HDD boot game is corrupted. The application will be terminated."; break;
    case 102: message = "Not enough available space. The application will be terminated."; break;
    default: return error(CELL_GAME_ERROR_PARAM);
    }
    std::string dir;
    if (dir_name) dir = get_string(thread_of(ctx), dir_name, CELL_GAME_DIRNAME_SIZE);
    PS_LOG_ERROR(kChan, "cellGameContentErrorDialog: {} (needs {} KB, directory \"{}\")", message, need_size_kb, dir);
    return lv2::CELL_OK;
}

} // namespace

void register_cellGame(Registry& r)
{
    r.add("cellGame", {
        PS_HLE_FUNC(0xF52639EA, cellGameBootCheck),
        PS_HLE_FUNC(0xDB9819F3, cellGameDataCheck),
        PS_HLE_FUNC(0x70ACEC67, cellGameContentPermit),
        PS_HLE_FUNC(0x42A2E133, cellGameCreateGameData),
        PS_HLE_FUNC(0x3A5D726A, cellGameGetParamString),
        PS_HLE_FUNC(0xB0A1F8C6, cellGameContentErrorDialog),
    });
}

} // namespace pspocket::hle
