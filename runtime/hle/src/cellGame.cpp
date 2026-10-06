// cellGame: the game's content directories (the disc's PS3_GAME, game data under /dev_hdd0/game) and their
// PARAM.SFO. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellGame.cpp/.h (content_permission and the check / permit
// session; the PS3's delays are not reproduced).
//
// A check (cellGameBootCheck or cellGameDataCheck) opens a session that cellGameContentPermit closes; a second check
// while one is open is CELL_GAME_ERROR_BUSY. The disc's PARAM.SFO comes from the host (ProcessConfig::game), game
// data lives in host directories under the /dev_hdd0 mount. Deviation: cellGameContentErrorDialog logs its message
// (there is no dialog UI yet) and returns CELL_OK; for the "exit" types the game is expected to quit by itself.
//
// cellSysutil's game data functions live here too, as in RPCS3 (cellSysutil_GameData_init): cellHddGameCheck and
// cellGameDataCheckCreate2 hand the game's funcStat RPCS3's structures (its cellGame globals: one guest block per
// process here) and act on its answer; cellDiscGameGetBootDiscInfo reads the disc's PARAM.SFO; the disc change
// callbacks are kept for a disc change the hosts can't make yet. The error dialog errDialog asks for is logged, as if
// the user closed it at once (D-044).
//
// Every function RPCS3 registers is here (D-183): the param ints and strings, sizes (Kernel::vfs_tree_size, which
// sizes a disc image too), the patch check, deleting game data, themes (to /dev_hdd0/theme, named by their SHA-256),
// and cellSysutil's HddGameCheck2, CheckCreate, sizes, system versions, exit dialogs and the disc change callbacks.

#include <algorithm>
#include <vector>
#include <span>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "psf.hpp"
#include "sysutil.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellGame";

enum : u32 {
    CELL_GAME_RET_CANCEL = 1,
    CELL_GAME_RET_NONE = 2,
    CELL_GAME_ERROR_NOTFOUND = 0x8002CB04,
    CELL_GAME_ERROR_BROKEN = 0x8002CB05,
    CELL_GAME_ERROR_PARAM = 0x8002CB07,
    CELL_GAME_ERROR_ACCESS_ERROR = 0x8002CB09,
    CELL_GAME_ERROR_NOTSUPPORTED = 0x8002CB21,
    CELL_GAME_ERROR_FAILURE = 0x8002CB22,
    CELL_GAME_ERROR_BUSY = 0x8002CB23,
    CELL_GAME_ERROR_INVALID_ID = 0x8002CB25,
    CELL_GAME_ERROR_EXIST = 0x8002CB26,
    CELL_GAME_ERROR_NOTPATCH = 0x8002CB27,
    CELL_GAME_ERROR_INVALID_THEME_FILE = 0x8002CB28,
    CELL_GAME_ERROR_NOSPACE = 0x8002CB20,
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

// RPCS3 strcpy_trunc into a guest buffer of `capacity` bytes: at most capacity - 1 bytes of `s`, the rest zeroed.
void put_string(u8* base, u32 addr, u32 capacity, std::string_view s)
{
    if (!capacity) return;
    const usize n = std::min<usize>(s.size(), capacity - 1);
    std::memcpy(base + addr, s.data(), n);
    std::memset(base + addr + n, 0, capacity - n);
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

// RPCS3 get_param_string_key: the PARAM.SFO key of an ID, its size, whether game data (or the disc / HDD game) may
// read it, and whether game data may write it (cellGameSetParamString).
struct StringKey {
    std::string_view name;
    bool game_data = false;
    bool other = false;
    bool set = false;
    u32 max_size = 0;
};

StringKey param_string_key(s32 id)
{
    static constexpr std::string_view kTitles[] = {"TITLE_00", "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04",
        "TITLE_05", "TITLE_06", "TITLE_07", "TITLE_08", "TITLE_09", "TITLE_10", "TITLE_11", "TITLE_12", "TITLE_13",
        "TITLE_14", "TITLE_15", "TITLE_16", "TITLE_17", "TITLE_18", "TITLE_19"};
    if (id == 0 || id == 1) return {"TITLE", true, true, true, CELL_GAME_SYSP_TITLE_SIZE}; // TITLE, TITLE_DEFAULT
    if (id >= 2 && id <= 21) return {kTitles[id - 2], true, false, true, CELL_GAME_SYSP_TITLE_SIZE}; // localized
    if (id == 100) return {"TITLE_ID", true, true, false, CELL_GAME_SYSP_TITLEID_SIZE};
    if (id == 101) return {"VERSION", true, false, false, CELL_GAME_SYSP_VERSION_SIZE};
    if (id == 105) return {"PS3_SYSTEM_VER", false, false, false, 8};
    if (id == 106) return {"APP_VER", true, true, false, 6};
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

s32 check_name_string(std::string_view s, s32 minlen, s32 maxlen, u32 sdk); // below

// RPCS3 cellGameSetParamString: game data the check may create (can_create) takes the strings game data may set;
// they go into its PARAM.SFO at cellGameContentPermit.
s32 cellGameSetParamString(PPUContext& ctx, s32 id, u32 buf)
{
    if (!buf) return error(CELL_GAME_ERROR_PARAM);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (!perm.open || perm.mode == Permission::Mode::NotSet) return error(CELL_GAME_ERROR_FAILURE);
    const StringKey key = param_string_key(id);
    if (key.name.empty()) return error(CELL_GAME_ERROR_INVALID_ID);
    const bool game_data_mode = perm.mode == Permission::Mode::GameData || perm.mode == Permission::Mode::Patch;
    if (!perm.can_create || !game_data_mode || !key.set) return error(CELL_GAME_ERROR_NOTSUPPORTED);
    const std::string value = get_string(thread_of(ctx), buf, key.max_size);
    perm.sfo[std::string(key.name)] = psf::string(key.max_size, value.substr(0, key.max_size - 1));
    PS_LOG_INFO(kChan, "cellGameSetParamString: {} = \"{}\"", key.name, value);
    return lv2::CELL_OK;
}

// RPCS3 cellGameGetParamInt: PARENTAL_LEVEL (102), RESOLUTION (103) or SOUND_FORMAT (104) of the checked PARAM.SFO,
// 0 when it has none.
s32 cellGameGetParamInt(PPUContext& ctx, s32 id, u32 value)
{
    if (!value) return error(CELL_GAME_ERROR_PARAM);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (!perm.open) return error(CELL_GAME_ERROR_FAILURE);
    std::string_view key;
    switch (id) {
    case 102: key = "PARENTAL_LEVEL"; break;
    case 103: key = "RESOLUTION"; break;
    case 104: key = "SOUND_FORMAT"; break;
    default: return error(CELL_GAME_ERROR_INVALID_ID);
    }
    const auto it = perm.sfo.find(std::string(key));
    const u32 v = it != perm.sfo.end() && it->second.format == psf::Format::Integer ? it->second.integer : 0;
    if (it == perm.sfo.end()) PS_LOG_WARN(kChan, "cellGameGetParamInt: {} not found", key);
    ps_write32(guest_base(ctx), value, v);
    return lv2::CELL_OK;
}

// RPCS3 cellGameGetSizeKB: the checked directory's size in KB, each file rounded up to 1 KB (0 while it doesn't
// exist yet); the directory cellGameCreateGameData made while it is the temporary one.
s32 cellGameGetSizeKB(PPUContext& ctx, u32 size)
{
    if (!size) return error(CELL_GAME_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    ps_write32(t.kernel.base(), size, 0);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (!perm.open) return error(CELL_GAME_ERROR_FAILURE);
    std::optional<u64> bytes;
    std::string where;
    if (!perm.temp.empty()) {
        where = perm.temp;
        std::error_code ec;
        u64 total = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(perm.temp, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) total += (it->file_size(ec) + 1023) / 1024 * 1024;
        }
        if (!ec) bytes = total;
    } else {
        // RPCS3 sizes /dev_hdd0/game/<dir>, the whole of /dev_hdd0/game for the disc (whose dir is empty).
        where = "/dev_hdd0/game/" + perm.dir;
        bytes = t.kernel.vfs_tree_size(where, 1024);
    }
    if (!bytes) {
        PS_LOG_INFO(kChan, "cellGameGetSizeKB: {} doesn't exist: 0 KB", where);
        return lv2::CELL_OK;
    }
    ps_write32(t.kernel.base(), size, static_cast<u32>(*bytes / 1024));
    return lv2::CELL_OK;
}

// RPCS3 cellGamePatchCheck: only a game booted from a patch (category GD) has one; the session then reads its
// PARAM.SFO. The booted game's category comes from its PARAM.SFO.
s32 cellGamePatchCheck(PPUContext& ctx, u32 size, u32 reserved)
{
    (void)reserved;
    const GameInfo& g = game();
    psf::Registry sfo;
    if (auto parsed = psf::parse(g.param_sfo)) sfo = std::move(*parsed);
    if (psf::get_string(sfo, "CATEGORY") != "GD") return error(CELL_GAME_ERROR_NOTPATCH);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (perm.open) return error(CELL_GAME_ERROR_BUSY);
    write_size(guest_base(ctx), size, CELL_GAME_SIZEKB_NOTCALC, 0);
    perm.open = true;
    perm.mode = Permission::Mode::Patch;
    perm.dir = g.title_id;
    perm.sfo = std::move(sfo);
    perm.exists = true;
    return lv2::CELL_OK;
}

// RPCS3 cellGameDeleteGameData: game data (category GD) under /dev_hdd0/game goes; not while the open session may
// create it, nor what isn't game data.
s32 cellGameDeleteGameData(PPUContext& ctx, u32 dir_name)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::string name = dir_name ? get_string(t, dir_name, CELL_GAME_DIRNAME_SIZE + 1) : std::string();
    if (!dir_name || check_name_string(name, 1, CELL_GAME_DIRNAME_SIZE, t.kernel.config().sdk_version) != 0)
        return error(CELL_GAME_ERROR_PARAM);
    Permission& perm = permission();
    std::lock_guard lock(perm.mutex);
    if (perm.open && perm.dir == name && perm.can_create) return error(CELL_GAME_ERROR_NOTSUPPORTED);
    const std::string dir = "/dev_hdd0/game/" + name;
    psf::Registry sfo;
    const SfoLoad load = load_sfo(t.kernel.vfs_host_path(dir + "/PARAM.SFO"), sfo);
    if (load != SfoLoad::Missing && psf::get_string(sfo, "CATEGORY") != "GD") return error(CELL_GAME_ERROR_NOTSUPPORTED);
    if (sfo.empty()) return error(CELL_GAME_ERROR_NOTFOUND);
    if (const std::string id = psf::get_string(sfo, "TITLE_ID"); !id.empty() && id != game().title_id)
        PS_LOG_WARN(kChan, "cellGameDeleteGameData({}): its title ID {} is not this game's", name, id);
    std::error_code ec;
    std::filesystem::remove_all(t.kernel.vfs_host_path(dir), ec);
    if (ec) {
        PS_LOG_ERROR(kChan, "cellGameDeleteGameData: cannot remove {} ({})", dir, ec.message());
        return error(CELL_GAME_ERROR_ACCESS_ERROR);
    }
    PS_LOG_INFO(kChan, "cellGameDeleteGameData: {} removed", dir);
    return lv2::CELL_OK;
}

// RPCS3 writes nothing to these paths and answers CELL_OK (the PS3's update and web content paths).
s32 cellGameGetDiscContentInfoUpdatePath(PPUContext&, u32 update_path)
{
    if (!update_path) return error(CELL_GAME_ERROR_PARAM);
    PS_LOG_WARN(kChan, "cellGameGetDiscContentInfoUpdatePath({:#x}): nothing written (RPCS3)", update_path);
    return lv2::CELL_OK;
}

s32 cellGameGetLocalWebContentPath(PPUContext&, u32 content_path)
{
    if (!content_path) return error(CELL_GAME_ERROR_PARAM);
    PS_LOG_WARN(kChan, "cellGameGetLocalWebContentPath({:#x}): nothing written (RPCS3)", content_path);
    return lv2::CELL_OK;
}

// A theme (.p3t, "P3TF") installed as RPCS3 installs it: /dev_hdd0/theme/<title ID>_<SHA-256 of the file>.p3t, kept
// if it is there already. Applying it (option 1) does nothing, as in RPCS3.
std::string theme_path(std::span<const u8> bytes)
{
    const recomp::Sha256Digest digest = recomp::Sha256::hash(bytes);
    std::string hex;
    for (const u8 b : digest) hex += std::format("{:02x}", b);
    return std::format("/dev_hdd0/theme/{}_{}.p3t", game().title_id, hex);
}

bool is_theme(std::span<const u8> bytes) { return bytes.size() >= 4 && std::memcmp(bytes.data(), "P3TF", 4) == 0; }

s32 cellGameThemeInstall(PPUContext& ctx, u32 usrdir_path, u32 file_name, u32 option)
{
    lv2::PpuThread& t = thread_of(ctx);
    const GuestString usrdir = read_cstr(t, usrdir_path, CELL_GAME_PATH_MAX);
    if (!usrdir_path || !file_name || !usrdir.terminated || option > 1) return error(CELL_GAME_ERROR_PARAM);
    const std::string src = usrdir.text + "/" + get_string(t, file_name, 0x100);
    std::vector<u8> bytes;
    if (!t.kernel.vfs_read_file(src, bytes)) return error(CELL_GAME_ERROR_NOTFOUND);
    std::string lower = src;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!lower.ends_with(".p3t") || !is_theme(bytes)) return error(CELL_GAME_ERROR_INVALID_THEME_FILE);
    const std::string dst = theme_path(bytes), host = t.kernel.vfs_host_path(dst);
    std::error_code ec;
    if (std::filesystem::is_regular_file(host, ec)) {
        PS_LOG_INFO(kChan, "cellGameThemeInstall: {} is installed already", dst);
        return lv2::CELL_OK;
    }
    std::filesystem::create_directories(std::filesystem::path(host).parent_path(), ec);
    std::ofstream out(host, std::ios::binary);
    if (host.empty() || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        PS_LOG_ERROR(kChan, "cellGameThemeInstall: cannot write {}", dst);
        return error(CELL_GAME_ERROR_ACCESS_ERROR);
    }
    PS_LOG_INFO(kChan, "cellGameThemeInstall: {} installed as {}", src, dst);
    return lv2::CELL_OK;
}

// From a buffer: whole, or (with func and a buffer smaller than the file) piece by piece, func(fileOffset, readSize,
// buf) after each piece; CELL_GAME_RET_CANCEL from it stops the install. The name hashes what is in the buffer
// first, as RPCS3's (its whole file only when it fits).
s32 cellGameThemeInstallFromBuffer(PPUContext& ctx, u32 file_size, u32 buf_size, u32 buf, u32 func, u32 option)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!buf || !file_size || (file_size > buf_size && !func) || buf_size < 4096 || option > 1)
        return error(CELL_GAME_ERROR_PARAM);
    const u32 first = std::min(file_size, buf_size);
    if (!guest_readable(t, buf, first)) {
        log_call_site(ctx, t.kernel.base(), "cellGameThemeInstallFromBuffer: buf is not guest memory");
        return error(CELL_GAME_ERROR_PARAM);
    }
    const std::span<const u8> head(t.kernel.base() + buf, first);
    const std::string dst = theme_path(head), host = t.kernel.vfs_host_path(dst);
    std::error_code ec;
    if (std::filesystem::exists(host, ec)) {
        PS_LOG_INFO(kChan, "cellGameThemeInstallFromBuffer: {} is installed already", dst);
        return lv2::CELL_OK;
    }
    if (!is_theme(head)) return error(CELL_GAME_ERROR_INVALID_THEME_FILE);
    std::filesystem::create_directories(std::filesystem::path(host).parent_path(), ec);
    std::ofstream out(host, std::ios::binary);
    if (host.empty() || !out) return error(CELL_GAME_ERROR_ACCESS_ERROR);
    if (!func || buf_size >= file_size) {
        if (!out.write(reinterpret_cast<const char*>(t.kernel.base() + buf), file_size)) return error(CELL_GAME_ERROR_NOSPACE);
        return lv2::CELL_OK;
    }
    for (u32 offset = 0; offset < file_size;) {
        const u32 piece = std::min(buf_size, file_size - offset);
        if (!out.write(reinterpret_cast<const char*>(t.kernel.base() + buf), piece)) return error(CELL_GAME_ERROR_NOSPACE);
        offset += piece;
        if (static_cast<s32>(call_guest(ctx, func, {offset, piece, buf})) == static_cast<s32>(CELL_GAME_RET_CANCEL)) {
            PS_LOG_INFO(kChan, "cellGameThemeInstallFromBuffer: cancelled by the game at {} bytes", offset);
            return static_cast<s32>(CELL_GAME_RET_CANCEL);
        }
    }
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

// ------------------------------------------------------------ cellSysutil's game data functions (RPCS3 cellGame.cpp)

enum : u32 {
    CELL_GAMEDATA_ERROR_CBRESULT = 0x8002B601,
    CELL_GAMEDATA_ERROR_ACCESS_ERROR = 0x8002B602,
    CELL_GAMEDATA_ERROR_INTERNAL = 0x8002B603,
    CELL_GAMEDATA_ERROR_PARAM = 0x8002B604,
    CELL_GAMEDATA_ERROR_BROKEN = 0x8002B606,
    CELL_HDDGAME_ERROR_CBRESULT = 0x8002BA01,
    CELL_HDDGAME_ERROR_INTERNAL = 0x8002BA03,
    CELL_HDDGAME_ERROR_PARAM = 0x8002BA04,
    CELL_HDDGAME_ERROR_FAILURE = 0x8002BA07,
    CELL_DISCGAME_ERROR_NOT_DISCBOOT = 0x8002BD02,
    CELL_DISCGAME_ERROR_PARAM = 0x8002BD03,
};
constexpr u32 CELL_GAMEDATA_VERSION_CURRENT = 0;
constexpr u32 CELL_GAMEDATA_ERRDIALOG_ALWAYS = 1;
// funcStat's results: CELL_GAMEDATA_CBRESULT_* (the CELL_HDDGAME_CBRESULT_* values are the same).
constexpr s32 CBRESULT_OK_CANCEL = 1, CBRESULT_OK = 0, CBRESULT_ERR_NOSPACE = -1, CBRESULT_ERR_BROKEN = -3,
              CBRESULT_ERR_NODATA = -4, CBRESULT_ERR_INVALID = -5;
constexpr u32 CELL_GAMEDATA_ISNEWDATA_NO = 0, CELL_GAMEDATA_ISNEWDATA_YES = 1;
constexpr u32 CELL_HDDGAME_ISNEWDATA_EXIST = 0, CELL_HDDGAME_ISNEWDATA_NODIR = 1;
constexpr u32 CELL_GAMEDATA_ATTR_NORMAL = 0;
constexpr u32 CELL_GAMEDATA_PATH_MAX = 1055, CELL_GAMEDATA_INVALIDMSG_MAX = 256, CELL_GAMEDATA_SYSP_LANGUAGE_NUM = 20;
constexpr u32 CELL_DISCGAME_SYSP_TITLEID_SIZE = 10;

// CellGameDataStatGet (= CellHddGameStatGet, 0x1448 bytes): be s32 hddFreeSizeKB; be32 isNewData; char
// contentInfoPath[1055], gameDataPath[1055], reserved0[2]; be s64 st_atime, st_mtime, st_ctime; getParam; be s32
// sizeKB, sysSizeKB; char reserved1[68].
namespace stat_get {
constexpr u32 hddFreeSizeKB = 0x0, isNewData = 0x4, contentInfoPath = 0x8, gameDataPath = 0x427, getParam = 0x860,
              sizeKB = 0x13FC, kSize = 0x1448;
}
// CellGameDataSystemFileParam (= CellHddGameSystemFileParam, 0xB9C bytes): char title[128], titleLang[20][128],
// titleId[10], reserved0[2], dataVersion[6], reserved1[2]; be32 parentalLevel, attribute, resolution, soundFormat;
// char reserved2[248].
namespace sys_param {
constexpr u32 title = 0x0, titleLang = 0x80, titleId = 0xA80, dataVersion = 0xA8C, parentalLevel = 0xA94,
              attribute = 0xA98, resolution = 0xA9C, soundFormat = 0xAA0, kSize = 0xB9C;
}
// CellGameDataCBResult (16 bytes): be s32 result, errNeedSizeKB; be32 invalidMsg, reserved. CellGameDataStatSet
// (8 bytes): be32 setParam, reserved.
namespace cb_result {
constexpr u32 result = 0x0, errNeedSizeKB = 0x4, invalidMsg = 0x8, kSize = 0x10;
}
constexpr u32 kStatSetSize = 8;
// CellDiscGameSystemFileParam (32 bytes): char titleId[10], reserved0[2]; be32 parentalLevel; char reserved1[16].
namespace disc_param {
constexpr u32 titleId = 0x0, parentalLevel = 0xC, kSize = 0x20;
}

// RPCS3's cellGame globals for funcStat (vm::gvar g_stat_get, g_stat_set, g_file_param, g_cb_result): one guest block
// per process, in the main area, made at the first check.
namespace check_vars {
constexpr u32 stat_get = 0x0, stat_set = 0x1448, cb_result = 0x1450, file_param = 0x1460, kSize = 0x2000;
}
static_assert(check_vars::stat_set >= check_vars::stat_get + stat_get::kSize);
static_assert(check_vars::file_param + sys_param::kSize <= check_vars::kSize);

struct CheckVars {
    explicit CheckVars(HleProcess& p) : process(p) {}
    HleProcess& process;
    std::mutex mutex;
    u32 block = 0;
};

// The block's guest address; 0 when the main area is full.
u32 check_vars_block()
{
    CheckVars& v = HleProcess::current()->state<CheckVars>();
    std::lock_guard lock(v.mutex);
    if (!v.block) v.block = v.process.main_memory().alloc(check_vars::kSize, 0x100);
    return v.block;
}

// A guest string up to its NUL, `max` bytes at most; nullopt when it runs into memory that is not guest memory first.
std::optional<std::string> read_guest_string(lv2::PpuThread& t, u32 addr, u32 max)
{
    std::string out;
    for (u32 i = 0; i < max; i++) {
        if (!guest_readable(t, addr + i, 1)) return std::nullopt;
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        out += c;
    }
    return out;
}

// A char array of `size` bytes, up to its first NUL (RPCS3 psf::string of a char[N]).
std::string read_chars(u8* base, u32 addr, u32 size)
{
    std::string s;
    for (u32 i = 0; i < size; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// RPCS3 sysutil_check_name_string (cellSysutil.cpp): [A-Z0-9_-], not starting with '_', minlen to maxlen characters;
// 0, -1 for a bad character, -2 for a bad length. Games built with SDK 3.7 or newer must end the string inside its
// maxlen bytes; older ones may fill them. `s` is the guest string up to its NUL (maxlen + 1 bytes at most): past its
// end reads as NUL, as RPCS3's pointer would at the terminator.
s32 check_name_string(std::string_view s, s32 minlen, s32 maxlen, u32 sdk)
{
    const s32 lastpos = sdk > 0x36FFFF ? std::max(maxlen - 1, 0) : maxlen;
    const auto at = [&](s32 i) { return i < static_cast<s32>(s.size()) ? s[static_cast<usize>(i)] : '\0'; };
    char cur = at(0);
    if (cur == '_') return -1;
    for (s32 index = 0;; cur = at(++index)) {
        if (cur == '\0' || index == maxlen) return minlen > index || (maxlen == index && at(lastpos)) ? -2 : 0;
        if ((cur >= 'A' && cur <= 'Z') || (cur >= '0' && cur <= '9') || cur == '-' || cur == '_') continue;
        return -1;
    }
}

// RPCS3 psf::load's last check (Loader/PSF.cpp valid_cats): a PARAM.SFO whose category is unknown, or missing, is
// corrupt.
bool known_category(std::string_view cat)
{
    static constexpr std::string_view kCategories[] = {"GD", "DG", "HG", "AM", "AP", "AS", "AT", "AV", "BV", "WT",
        "HM", "CB", "SF", "2P", "2G", "1P", "PP", "MN", "PE", "2D", "SD", "MS"};
    return std::find(std::begin(kCategories), std::end(kCategories), cat) != std::end(kCategories);
}

// RPCS3 psf::load of a PARAM.SFO for the checks below: Missing (psf::error::stream) when there is no file; a file that
// doesn't parse, or whose category is unknown, is Corrupt and reads as empty.
SfoLoad load_checked_sfo(const std::string& host_path, psf::Registry& out)
{
    const SfoLoad load = load_sfo(host_path, out);
    if (load != SfoLoad::Ok || known_category(psf::get_string(out, "CATEGORY"))) return load;
    out.clear();
    return SfoLoad::Corrupt;
}

// RPCS3 psf::is_cat_hdd: the category of an HDD boot game.
bool is_hdd_category(std::string_view cat) { return cat.size() == 2 && cat[1] != 'D' && cat != "DG" && cat != "MS"; }

// RPCS3 psf::get_string / psf::get_integer: the value of `key` when it is of that type, else `def`.
std::string sfo_string(const psf::Registry& sfo, std::string_view key, std::string_view def = {})
{
    const auto it = sfo.find(key);
    if (it == sfo.end() || it->second.format == psf::Format::Integer) return std::string(def);
    return it->second.text;
}

u32 sfo_integer(const psf::Registry& sfo, std::string_view key, u32 def = 0)
{
    const auto it = sfo.find(key);
    if (it == sfo.end() || it->second.format != psf::Format::Integer) return def;
    return it->second.integer;
}

// RPCS3 psf::check_registry: every string or array fits its entry (a string with its terminator).
bool sfo_valid(const psf::Registry& sfo)
{
    for (const auto& [key, e] : sfo) {
        if (e.format == psf::Format::Integer) continue;
        if (u64{e.text.size()} + (e.format == psf::Format::String ? 1 : 0) > e.max) {
            PS_LOG_ERROR(kChan, "PARAM.SFO entry {} (\"{}\") doesn't fit its {} bytes", key, e.text, e.max);
            return false;
        }
    }
    return true;
}

// What both checks put in funcStat's structures before the call (RPCS3): get, set and the result cleared; then 40 GB
// free less 256 KB (see kHddFreeSizeKB), the content and USRDIR paths, the size not calculated, the times 0.
void begin_stat(u8* base, u32 block, const std::string& dir, const std::string& usrdir)
{
    const u32 get = block + check_vars::stat_get;
    std::memset(base + get, 0, stat_get::kSize);
    std::memset(base + block + check_vars::stat_set, 0, kStatSetSize);
    std::memset(base + block + check_vars::cb_result, 0, cb_result::kSize);
    ps_write32(base, get + stat_get::hddFreeSizeKB, static_cast<u32>(kHddFreeSizeKB));
    put_string(base, get + stat_get::contentInfoPath, CELL_GAMEDATA_PATH_MAX, dir);
    put_string(base, get + stat_get::gameDataPath, CELL_GAMEDATA_PATH_MAX, usrdir);
    ps_write32(base, get + stat_get::sizeKB, static_cast<u32>(CELL_GAME_SIZEKB_NOTCALC));
}

// getParam's strings from a PARAM.SFO (RPCS3): the title, the localized titles TITLE_00..19, the title ID, and the data
// version from APP_VER, or VERSION when there is none (old games).
void put_param_strings(u8* base, u32 param, const psf::Registry& sfo)
{
    put_string(base, param + sys_param::title, CELL_GAME_SYSP_TITLE_SIZE, sfo_string(sfo, "TITLE"));
    for (u32 i = 0; i < CELL_GAMEDATA_SYSP_LANGUAGE_NUM; i++) {
        put_string(base, param + sys_param::titleLang + CELL_GAME_SYSP_TITLE_SIZE * i, CELL_GAME_SYSP_TITLE_SIZE,
            sfo_string(sfo, std::format("TITLE_{:02}", i)));
    }
    put_string(base, param + sys_param::titleId, CELL_GAME_SYSP_TITLEID_SIZE, sfo_string(sfo, "TITLE_ID"));
    put_string(base, param + sys_param::dataVersion, CELL_GAME_SYSP_VERSION_SIZE,
        sfo_string(sfo, "APP_VER", sfo_string(sfo, "VERSION")));
}

// funcStat's error result. RPCS3 logs it and, for errDialog CELL_GAMEDATA_ERRDIALOG_ALWAYS, shows an error dialog with
// this text (its English one, `hdd_game` choosing cellHddGameCheck's) until the user closes it; here the dialog is only
// logged, as if closed at once (D-044). The check fails with `cbresult`, its module's CBRESULT error.
s32 funcstat_failed(lv2::PpuThread& t, std::string_view call, u32 result, u32 err_dialog, bool hdd_game,
    std::string_view dir_name, u32 cbresult)
{
    u8* base = t.kernel.base();
    const s32 res = static_cast<s32>(ps_read32(base, result + cb_result::result));
    const u32 msg = ps_read32(base, result + cb_result::invalidMsg);
    const std::string invalid = msg ? read_guest_string(t, msg, CELL_GAMEDATA_INVALIDMSG_MAX).value_or("?") : "";
    const std::string_view what = hdd_game ? "HDD boot game" : "game data";
    std::string name, text;
    switch (res) {
    case CBRESULT_ERR_NOSPACE:
        name = "ERR_NOSPACE";
        text = std::format("Not enough space to create {}. Space Needed: {} KB", what,
            static_cast<s32>(ps_read32(base, result + cb_result::errNeedSizeKB)));
        break;
    case CBRESULT_ERR_BROKEN:
        name = "ERR_BROKEN";
        text = hdd_game ? std::format("HDD boot game {} is corrupt!", dir_name)
                        : std::format("The game data in {} is corrupt!", dir_name);
        break;
    case CBRESULT_ERR_NODATA:
        name = "ERR_NODATA";
        text = hdd_game ? std::format("HDD boot game {} could not be found!", dir_name)
                        : std::format("The game data in {} could not be found!", dir_name);
        break;
    case CBRESULT_ERR_INVALID:
        name = "ERR_INVALID";
        text = std::format("Error: {}", invalid);
        break;
    default:
        name = std::format("an unknown result ({:#x})", static_cast<u32>(res));
        text = std::format("Error: {}", invalid);
        break;
    }
    if (err_dialog == CELL_GAMEDATA_ERRDIALOG_ALWAYS) {
        PS_LOG_ERROR(kChan, "{}: funcStat returned {}; the PS3's error dialog (not shown): \"{}\"", call, name, text);
    } else {
        PS_LOG_ERROR(kChan, "{}: funcStat returned {} (\"{}\")", call, name, text);
    }
    return error(cbresult);
}

// The arguments both checks take, refused with their module's parameter error (named by `param_error`, for the log)
// for a version other than CELL_GAMEDATA_VERSION_CURRENT, a null funcStat or dirName, or a name
// sysutil_check_name_string refuses (RPCS3). Deviation (D-042): a dirName or funcStat outside guest memory is refused
// too, with the call site logged (RPCS3 faults). The name on success, else nullopt.
std::optional<std::string> check_args(PPUContext& ctx, std::string_view call, u32 version, u32 dir_name, u32 func_stat,
    std::string_view param_error)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (version != CELL_GAMEDATA_VERSION_CURRENT || !func_stat || !dir_name) {
        PS_LOG_WARN(kChan, "{}: {}", call, param_error);
        return std::nullopt;
    }
    // As sysutil_check_name_string reads it: up to 32 characters and the byte after.
    const std::optional<std::string> name = read_guest_string(t, dir_name, CELL_GAME_DIRNAME_SIZE + 1);
    if (!name || !guest_readable(t, func_stat, 8)) {
        log_call_site(ctx, t.kernel.base(), std::format("{}: dirName or funcStat is not guest memory", call));
        return std::nullopt;
    }
    if (check_name_string(*name, 1, CELL_GAME_DIRNAME_SIZE, t.kernel.config().sdk_version) != 0) {
        PS_LOG_WARN(kChan, "{}: \"{}\" is not a valid directory name: {}", call, *name, param_error);
        return std::nullopt;
    }
    return name;
}

// cellHddGameCheck(version, dirName, errDialog, funcStat, container): RPCS3's check of an HDD boot game's directory,
// /dev_hdd0/game/<dirName>. The arguments as check_args says (CELL_HDDGAME_ERROR_PARAM); a PARAM.SFO that is there
// but not an HDD game's is CELL_GAMEDATA_ERROR_BROKEN (RPCS3's code). funcStat(cbResult, get, set) then gets
// CellHddGameStatGet: begin_stat's fields, isNewData EXIST or NODIR (by the directory), and with the directory the
// PARAM.SFO's parental level, attribute, resolution, sound format and strings; set->setParam points at a zeroed
// CellHddGameSystemFileParam. Its answer: OK is CELL_OK, but CELL_GAMEDATA_ERROR_PARAM when there is no PARAM.SFO yet
// and funcStat cleared setParam; RPCS3 creates and writes nothing for it (that code is disabled there until the PS3's
// behaviour is known). OK_CANCEL is CELL_OK, an error result CELL_HDDGAME_ERROR_CBRESULT (funcstat_failed).
// Deviations: a name that isn't 9 characters long is a parameter error (RPCS3 asserts), and an entry of the wrong type
// in the PARAM.SFO reads as missing (RPCS3 aborts). RPCS3's delays are left out (D-044).
s32 cellHddGameCheck(PPUContext& ctx, u32 version, u32 dir_name, u32 err_dialog, u32 func_stat, u32 container)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const std::string call = std::format("cellHddGameCheck(version {}, dirName {:#x}, errDialog {}, funcStat {:#x}, "
                                         "container {:#x})",
        version, dir_name, err_dialog, func_stat, container);
    const std::optional<std::string> name =
        check_args(ctx, call, version, dir_name, func_stat, "CELL_HDDGAME_ERROR_PARAM");
    if (!name) return error(CELL_HDDGAME_ERROR_PARAM);
    if (name->size() != 9) {
        PS_LOG_ERROR(kChan, "{}: \"{}\" is not 9 characters long, as an HDD game's directory is (RPCS3 asserts): "
                            "CELL_HDDGAME_ERROR_PARAM",
            call, *name);
        return error(CELL_HDDGAME_ERROR_PARAM);
    }
    const std::string dir = "/dev_hdd0/game/" + *name, usrdir = dir + "/USRDIR";
    psf::Registry sfo;
    const bool new_data = load_checked_sfo(t.kernel.vfs_host_path(dir + "/PARAM.SFO"), sfo) == SfoLoad::Missing;
    if (!new_data && !is_hdd_category(psf::get_string(sfo, "CATEGORY"))) {
        PS_LOG_ERROR(kChan, "{}: {}/PARAM.SFO is not an HDD game's (category \"{}\"): CELL_GAMEDATA_ERROR_BROKEN",
            call, dir, psf::get_string(sfo, "CATEGORY"));
        return error(CELL_GAMEDATA_ERROR_BROKEN);
    }
    const u32 block = check_vars_block();
    if (!block) {
        PS_LOG_ERROR(kChan, "{}: no main-area memory for funcStat's structures", call);
        return error(CELL_HDDGAME_ERROR_INTERNAL);
    }
    const u32 get = block + check_vars::stat_get, set = block + check_vars::stat_set;
    const u32 result = block + check_vars::cb_result, file_param = block + check_vars::file_param;
    begin_stat(base, block, dir, usrdir);
    std::memset(base + file_param, 0, sys_param::kSize);
    ps_write32(base, set, file_param);
    std::error_code ec;
    const std::string host_dir = t.kernel.vfs_host_path(dir);
    const bool exists = !host_dir.empty() && std::filesystem::is_directory(host_dir, ec);
    ps_write32(base, get + stat_get::isNewData, exists ? CELL_HDDGAME_ISNEWDATA_EXIST : CELL_HDDGAME_ISNEWDATA_NODIR);
    if (exists) {
        const u32 param = get + stat_get::getParam;
        ps_write32(base, param + sys_param::parentalLevel, sfo_integer(sfo, "PARENTAL_LEVEL"));
        ps_write32(base, param + sys_param::attribute, sfo_integer(sfo, "ATTRIBUTE"));
        ps_write32(base, param + sys_param::resolution, sfo_integer(sfo, "RESOLUTION"));
        ps_write32(base, param + sys_param::soundFormat, sfo_integer(sfo, "SOUND_FORMAT"));
        put_param_strings(base, param, sfo);
    }
    PS_LOG_INFO(kChan, "{}: \"{}\" {}, PARAM.SFO {}; calling funcStat", call, *name,
        exists ? "exists" : "has no directory", new_data ? "missing" : "found");
    call_guest(ctx, func_stat, {result, get, set});

    switch (static_cast<s32>(ps_read32(base, result + cb_result::result))) {
    case CBRESULT_OK:
        if (new_data && !ps_read32(base, set)) {
            PS_LOG_ERROR(kChan, "{}: funcStat returned OK for new data without setParam: CELL_GAMEDATA_ERROR_PARAM",
                call);
            return error(CELL_GAMEDATA_ERROR_PARAM);
        }
        PS_LOG_INFO(kChan, "{}: funcStat returned OK", call);
        return lv2::CELL_OK;
    case CBRESULT_OK_CANCEL: PS_LOG_INFO(kChan, "{}: funcStat returned OK_CANCEL", call); return lv2::CELL_OK;
    default: return funcstat_failed(t, call, result, err_dialog, true, *name, CELL_HDDGAME_ERROR_CBRESULT);
    }
}

// cellGameDataCheckCreate2(version, dirName, errDialog, funcStat, container): RPCS3's check of game data,
// /dev_hdd0/game/<dirName>. The arguments as check_args says (CELL_GAMEDATA_ERROR_PARAM); a PARAM.SFO that is there
// but neither game data's (GD) nor a disc game's (DG) is CELL_GAMEDATA_ERROR_BROKEN. funcStat(cbResult, get, set) gets
// begin_stat's fields, isNewData YES when there is no PARAM.SFO, and getParam from it: attribute NORMAL, the parental
// level and the strings; set->setParam is null. Its answer: OK_CANCEL is CELL_OK. OK with new data needs setParam
// (else CELL_GAMEDATA_ERROR_PARAM) and creates the USRDIR directory (CELL_GAME_ERROR_ACCESS_ERROR when it can't). OK
// with setParam then writes PARAM.SFO: category GD for new data, setParam's title ID, title, version, parental level
// and the localized titles it fills; when a string doesn't fit its entry RPCS3 writes nothing and still returns
// CELL_OK. An error result is CELL_GAMEDATA_ERROR_CBRESULT (funcstat_failed). Deviations: a setParam outside guest
// memory is CELL_GAMEDATA_ERROR_PARAM (RPCS3 faults); a PARAM.SFO the host can't write is
// CELL_GAMEDATA_ERROR_ACCESS_ERROR (RPCS3 aborts). RPCS3's delays are left out (D-044).
// The game data directory cellGameDataCheckCreate2 checked last (RPCS3 game_data_check), for cellGameDataGetSizeKB.
struct GameDataDir {
    std::mutex mutex;
    std::string dir;
};

s32 cellGameDataCheckCreate2(PPUContext& ctx, u32 version, u32 dir_name, u32 err_dialog, u32 func_stat, u32 container)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const std::string call = std::format("cellGameDataCheckCreate2(version {}, dirName {:#x}, errDialog {}, funcStat "
                                         "{:#x}, container {:#x})",
        version, dir_name, err_dialog, func_stat, container);
    const std::optional<std::string> name =
        check_args(ctx, call, version, dir_name, func_stat, "CELL_GAMEDATA_ERROR_PARAM");
    if (!name) return error(CELL_GAMEDATA_ERROR_PARAM);
    const std::string dir = "/dev_hdd0/game/" + *name, usrdir = dir + "/USRDIR";
    {
        GameDataDir& g = HleProcess::current()->state<GameDataDir>();
        std::lock_guard lock(g.mutex);
        g.dir = dir;
    }
    psf::Registry sfo;
    const bool new_data = load_checked_sfo(t.kernel.vfs_host_path(dir + "/PARAM.SFO"), sfo) == SfoLoad::Missing;
    const std::string cat = psf::get_string(sfo, "CATEGORY");
    if (!new_data && cat != "GD" && cat != "DG") {
        PS_LOG_ERROR(kChan, "{}: {}/PARAM.SFO is not game data's (category \"{}\"): CELL_GAMEDATA_ERROR_BROKEN", call,
            dir, cat);
        return error(CELL_GAMEDATA_ERROR_BROKEN);
    }
    const u32 block = check_vars_block();
    if (!block) {
        PS_LOG_ERROR(kChan, "{}: no main-area memory for funcStat's structures", call);
        return error(CELL_GAMEDATA_ERROR_INTERNAL);
    }
    const u32 get = block + check_vars::stat_get, set = block + check_vars::stat_set;
    const u32 result = block + check_vars::cb_result;
    begin_stat(base, block, dir, usrdir);
    ps_write32(base, get + stat_get::isNewData, new_data ? CELL_GAMEDATA_ISNEWDATA_YES : CELL_GAMEDATA_ISNEWDATA_NO);
    const u32 param = get + stat_get::getParam;
    ps_write32(base, param + sys_param::attribute, CELL_GAMEDATA_ATTR_NORMAL);
    ps_write32(base, param + sys_param::parentalLevel, sfo_integer(sfo, "PARENTAL_LEVEL"));
    put_param_strings(base, param, sfo);
    PS_LOG_INFO(kChan, "{}: \"{}\", PARAM.SFO {}; calling funcStat", call, *name, new_data ? "missing" : "found");
    call_guest(ctx, func_stat, {result, get, set});

    switch (static_cast<s32>(ps_read32(base, result + cb_result::result))) {
    case CBRESULT_OK_CANCEL: PS_LOG_INFO(kChan, "{}: funcStat returned OK_CANCEL", call); return lv2::CELL_OK;
    case CBRESULT_OK: break;
    default: return funcstat_failed(t, call, result, err_dialog, false, *name, CELL_GAMEDATA_ERROR_CBRESULT);
    }
    const u32 set_param = ps_read32(base, set);
    if (new_data) {
        if (!set_param) {
            PS_LOG_ERROR(kChan, "{}: funcStat returned OK for new data without setParam: CELL_GAMEDATA_ERROR_PARAM",
                call);
            return error(CELL_GAMEDATA_ERROR_PARAM);
        }
        const std::string host = t.kernel.vfs_host_path(usrdir);
        std::error_code ec;
        const bool made = !host.empty() &&
            (std::filesystem::create_directories(host, ec) || std::filesystem::is_directory(host, ec));
        if (!made) {
            PS_LOG_ERROR(kChan, "{}: cannot create {}{}: CELL_GAME_ERROR_ACCESS_ERROR", call, usrdir,
                host.empty() ? " (/dev_hdd0 is not mounted on a host directory)" : "");
            return error(CELL_GAME_ERROR_ACCESS_ERROR);
        }
    }
    if (!set_param) {
        PS_LOG_INFO(kChan, "{}: funcStat returned OK; PARAM.SFO stays as it is", call);
        return lv2::CELL_OK;
    }
    if (!guest_readable(t, set_param, sys_param::kSize)) {
        log_call_site(ctx, base, std::format("{}: setParam {:#x} is not guest memory", call, set_param));
        return error(CELL_GAMEDATA_ERROR_PARAM);
    }
    const auto field = [&](u32 offset, u32 size) { return read_chars(base, set_param + offset, size); };
    if (new_data) sfo["CATEGORY"] = psf::string(3, "GD");
    // The title ID may be cut to its entry (RPCS3 passes allow_truncate), a char[10] always fits.
    sfo["TITLE_ID"] = psf::string(TITLEID_SFO_ENTRY_SIZE,
        field(sys_param::titleId, CELL_GAME_SYSP_TITLEID_SIZE).substr(0, TITLEID_SFO_ENTRY_SIZE - 1));
    sfo["TITLE"] = psf::string(CELL_GAME_SYSP_TITLE_SIZE, field(sys_param::title, CELL_GAME_SYSP_TITLE_SIZE));
    sfo["VERSION"] = psf::string(CELL_GAME_SYSP_VERSION_SIZE,
        field(sys_param::dataVersion, CELL_GAME_SYSP_VERSION_SIZE));
    sfo["PARENTAL_LEVEL"] =
        psf::Entry{psf::Format::Integer, 4, {}, ps_read32(base, set_param + sys_param::parentalLevel)};
    for (u32 i = 0; i < CELL_GAMEDATA_SYSP_LANGUAGE_NUM; i++) {
        const u32 lang = sys_param::titleLang + CELL_GAME_SYSP_TITLE_SIZE * i;
        const std::string title = field(lang, CELL_GAME_SYSP_TITLE_SIZE);
        if (!title.empty()) sfo[std::format("TITLE_{:02}", i)] = psf::string(CELL_GAME_SYSP_TITLE_SIZE, title);
    }
    if (!sfo_valid(sfo)) {
        PS_LOG_ERROR(kChan, "{}: setParam doesn't make a valid PARAM.SFO; nothing written, CELL_OK (RPCS3)", call);
        return lv2::CELL_OK;
    }
    if (!save_sfo(t.kernel.vfs_host_path(dir + "/PARAM.SFO"), sfo)) {
        PS_LOG_ERROR(kChan, "{}: cannot write {}/PARAM.SFO: CELL_GAMEDATA_ERROR_ACCESS_ERROR", call, dir);
        return error(CELL_GAMEDATA_ERROR_ACCESS_ERROR);
    }
    PS_LOG_INFO(kChan, "{}: funcStat returned OK; {}/PARAM.SFO written (\"{}\")", call, dir,
        psf::get_string(sfo, "TITLE"));
    return lv2::CELL_OK;
}

// Whether /dev_bdvd/PS3_GAME is a directory, as lv2 sees it (RPCS3: fs::is_dir of its VFS path): a game disc is in.
bool disc_mounted(PPUContext& ctx)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    constexpr std::string_view kGameDir = "/dev_bdvd/PS3_GAME";
    StackVar v(ctx, base, 0x20 + lv2::kCellFsStatSize); // the path (zero-terminated: StackVar clears), then CellFsStat
    std::memcpy(base + v.addr(), kGameDir.data(), kGameDir.size());
    if (lv2::sys_fs_stat(t, v.addr(), v.addr() + 0x20) != lv2::CELL_OK) return false;
    return (ps_read32(base, v.addr() + 0x20) & 0170000) == static_cast<u32>(lv2::CELL_FS_S_IFDIR);
}

// cellDiscGameGetBootDiscInfo(CellDiscGameSystemFileParam* getParam): RPCS3's. CELL_DISCGAME_ERROR_PARAM for a null
// pointer; then the first word of titleId is cleared, always. Without a disc (no /dev_bdvd/PS3_GAME directory: games
// that don't boot from a disc ask too) the result is CELL_DISCGAME_ERROR_NOT_DISCBOOT. Otherwise the disc's PARAM.SFO
// gives parentalLevel and titleId, each only when it has that entry. Deviation (D-042): a pointer outside guest memory
// is CELL_DISCGAME_ERROR_PARAM (RPCS3 faults).
s32 cellDiscGameGetBootDiscInfo(PPUContext& ctx, u32 get_param)
{
    if (!get_param) return error(CELL_DISCGAME_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, get_param, disc_param::kSize)) {
        log_call_site(ctx, base, std::format("cellDiscGameGetBootDiscInfo: {:#x} is not guest memory", get_param));
        return error(CELL_DISCGAME_ERROR_PARAM);
    }
    ps_write32(base, get_param + disc_param::titleId, 0);
    if (!disc_mounted(ctx)) {
        PS_LOG_INFO(kChan, "cellDiscGameGetBootDiscInfo: no /dev_bdvd/PS3_GAME: CELL_DISCGAME_ERROR_NOT_DISCBOOT");
        return error(CELL_DISCGAME_ERROR_NOT_DISCBOOT);
    }
    // The disc's PARAM.SFO, as RPCS3's psf::load reads it (an unknown category makes it empty).
    psf::Registry sfo;
    if (auto parsed = psf::parse(game().param_sfo); parsed && known_category(psf::get_string(*parsed, "CATEGORY"))) {
        sfo = std::move(*parsed);
    }
    if (const auto it = sfo.find("PARENTAL_LEVEL"); it != sfo.end() && it->second.format == psf::Format::Integer) {
        ps_write32(base, get_param + disc_param::parentalLevel, it->second.integer);
    }
    if (const auto it = sfo.find("TITLE_ID"); it != sfo.end() && it->second.format != psf::Format::Integer) {
        put_string(base, get_param + disc_param::titleId, CELL_DISCGAME_SYSP_TITLEID_SIZE, it->second.text);
    }
    PS_LOG_INFO(kChan, "cellDiscGameGetBootDiscInfo: disc {}, parental level {}", psf::get_string(sfo, "TITLE_ID"),
        sfo_integer(sfo, "PARENTAL_LEVEL"));
    return lv2::CELL_OK;
}

// RPCS3 disc_change_manager: the game's disc eject and insert callbacks (CellDiscGameDiscEjectCallback(),
// CellDiscGameDiscInsertCallback(discType, titleId)), and the disc's state as first seen. RPCS3 calls them when the
// user ejects or inserts a disc from its UI; the hosts here can't change discs yet, so they are only kept.
struct DiscChange {
    enum class State : u8 { Unknown, Inserted, Ejected };
    std::mutex mutex;
    u32 eject = 0;  // descriptors; 0 = none
    u32 insert = 0;
    State state = State::Unknown;
};

// cellDiscGameRegisterDiscChangeCallback(funcEject, funcInsert): RPCS3 register_callbacks. Both are kept as given
// (null ones too, nothing is checked), the state becomes inserted or ejected the first time (by disc_mounted), and the
// result is CELL_OK.
s32 cellDiscGameRegisterDiscChangeCallback(PPUContext& ctx, u32 func_eject, u32 func_insert)
{
    const bool mounted = disc_mounted(ctx);
    DiscChange& d = HleProcess::current()->state<DiscChange>();
    std::lock_guard lock(d.mutex);
    d.eject = func_eject;
    d.insert = func_insert;
    if (d.state == DiscChange::State::Unknown) {
        d.state = mounted ? DiscChange::State::Inserted : DiscChange::State::Ejected;
    }
    PS_LOG_INFO(kChan, "cellDiscGameRegisterDiscChangeCallback(funcEject {:#x}, funcInsert {:#x}): kept (the disc was "
                       "{} when first seen; it can't change)",
        func_eject, func_insert, d.state == DiscChange::State::Inserted ? "in" : "out");
    return lv2::CELL_OK;
}

// cellGameRegisterDiscChangeCallback: the same manager, under cellGame's name (RPCS3).
s32 cellGameRegisterDiscChangeCallback(PPUContext& ctx, u32 func_eject, u32 func_insert)
{
    return cellDiscGameRegisterDiscChangeCallback(ctx, func_eject, func_insert);
}

// RPCS3 unregister_callbacks: both callbacks go; CELL_OK.
s32 cellDiscGameUnregisterDiscChangeCallback(PPUContext&)
{
    DiscChange& d = HleProcess::current()->state<DiscChange>();
    std::lock_guard lock(d.mutex);
    d.eject = 0;
    d.insert = 0;
    PS_LOG_INFO(kChan, "cellDiscGameUnregisterDiscChangeCallback()");
    return lv2::CELL_OK;
}

s32 cellGameUnregisterDiscChangeCallback(PPUContext& ctx) { return cellDiscGameUnregisterDiscChangeCallback(ctx); }

// RPCS3: cellHddGameCheck2 is cellHddGameCheck, cellGameDataCheckCreate is cellGameDataCheckCreate2 (the PS3's older
// one also computes the size).
s32 cellHddGameCheck2(PPUContext& ctx, u32 version, u32 dir_name, u32 err_dialog, u32 func_stat, u32 container)
{
    return cellHddGameCheck(ctx, version, dir_name, err_dialog, func_stat, container);
}

s32 cellGameDataCheckCreate(PPUContext& ctx, u32 version, u32 dir_name, u32 err_dialog, u32 func_stat, u32 container)
{
    return cellGameDataCheckCreate2(ctx, version, dir_name, err_dialog, func_stat, container);
}

// The directory the game booted from: the disc's PS3_GAME, or an HDD game's /dev_hdd0/game/<title ID>.
std::string boot_dir()
{
    const GameInfo& g = game();
    return g.boot == GameInfo::Boot::Disc ? "/dev_bdvd/PS3_GAME" : "/dev_hdd0/game/" + g.title_id;
}

// RPCS3 cellHddGameGetSizeKB: the booted directory's size in KB, each file rounded up to 1 KB.
s32 cellHddGameGetSizeKB(PPUContext& ctx, u32 size)
{
    if (!size) return error(CELL_HDDGAME_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> bytes = t.kernel.vfs_tree_size(boot_dir(), 1024);
    if (!bytes) {
        PS_LOG_ERROR(kChan, "cellHddGameGetSizeKB: cannot size {}", boot_dir());
        return error(CELL_HDDGAME_ERROR_FAILURE);
    }
    ps_write32(t.kernel.base(), size, static_cast<u32>(*bytes / 1024));
    return lv2::CELL_OK;
}

// RPCS3 cellGameDataGetSizeKB: the directory the last cellGameDataCheckCreate checked (the booted one before any),
// 0 while it doesn't exist.
s32 cellGameDataGetSizeKB(PPUContext& ctx, u32 size)
{
    if (!size) return error(CELL_GAMEDATA_ERROR_PARAM);
    lv2::PpuThread& t = thread_of(ctx);
    std::string dir;
    {
        GameDataDir& g = HleProcess::current()->state<GameDataDir>();
        std::lock_guard lock(g.mutex);
        dir = g.dir.empty() ? boot_dir() : g.dir;
    }
    const std::optional<u64> bytes = t.kernel.vfs_tree_size(dir, 1024);
    ps_write32(t.kernel.base(), size, bytes ? static_cast<u32>(*bytes / 1024) : 0);
    return lv2::CELL_OK;
}

// RPCS3 check_system_ver: a version like "04.8300", seven characters.
bool system_ver_ok(lv2::PpuThread& t, u32 addr)
{
    const GuestString v = read_cstr(t, addr, 8);
    const auto digit = [&](usize i) { return v.text[i] >= '0' && v.text[i] <= '9'; };
    return addr && v.terminated && v.text.size() == 7 && digit(0) && digit(1) && v.text[2] == '.' && digit(3) &&
        digit(4) && digit(5) && digit(6);
}

s32 cellHddGameSetSystemVer(PPUContext& ctx, u32 system_version)
{
    return system_ver_ok(thread_of(ctx), system_version) ? lv2::CELL_OK : error(CELL_HDDGAME_ERROR_PARAM);
}

s32 cellGameDataSetSystemVer(PPUContext& ctx, u32 system_version)
{
    return system_ver_ok(thread_of(ctx), system_version) ? lv2::CELL_OK : error(CELL_GAMEDATA_ERROR_PARAM);
}

// RPCS3 open_exit_dialog: an error dialog with an OK button, and the game is asked to exit when it closes. With no
// dialog UI it closes at once: the message is logged and CELL_SYSUTIL_REQUEST_EXITGAME goes to the game's callbacks.
s32 exit_broken(std::string_view message)
{
    PS_LOG_ERROR(kChan, "{} (the PS3's error dialog; the game is asked to exit)", message);
    sysutil_send_event(CELL_SYSUTIL_REQUEST_EXITGAME, 0);
    return lv2::CELL_OK;
}

s32 cellHddGameExitBroken(PPUContext&)
{
    return exit_broken("There has been an error! Please reinstall the HDD boot game.");
}

s32 cellGameDataExitBroken(PPUContext&)
{
    return exit_broken("There has been an error! Please remove the game data for this title.");
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
        PS_HLE_FUNC(0xCE4374F6, cellGamePatchCheck),
        PS_HLE_FUNC(0xB367C6E3, cellGameDeleteGameData),
        PS_HLE_FUNC(0xB7A45CAF, cellGameGetParamInt),
        PS_HLE_FUNC(0xDAA5CD20, cellGameSetParamString),
        PS_HLE_FUNC(0xEF9D42D5, cellGameGetSizeKB),
        PS_HLE_FUNC(0x2A8E6B92, cellGameGetDiscContentInfoUpdatePath),
        PS_HLE_FUNC(0xA80BF223, cellGameGetLocalWebContentPath),
        PS_HLE_FUNC(0xD24E3928, cellGameThemeInstall),
        PS_HLE_FUNC(0x87406734, cellGameThemeInstallFromBuffer),
    });
    // cellSysutil's game data functions (RPCS3 cellSysutil_GameData_init).
    r.add("cellSysutil", {
        PS_HLE_FUNC(0x9117DF20, cellHddGameCheck),
        PS_HLE_FUNC(0xC9645C41, cellGameDataCheckCreate2),
        PS_HLE_FUNC(0xB72BC4E6, cellDiscGameGetBootDiscInfo),
        PS_HLE_FUNC(0xDFDD302E, cellDiscGameRegisterDiscChangeCallback),
        PS_HLE_FUNC(0x4BDEC82A, cellHddGameCheck2),
        PS_HLE_FUNC(0xF82E2EF7, cellHddGameGetSizeKB),
        PS_HLE_FUNC(0x9CA9FFA7, cellHddGameSetSystemVer),
        PS_HLE_FUNC(0xAFD605B3, cellHddGameExitBroken),
        PS_HLE_FUNC(0xA9B0C1D9, cellGameDataGetSizeKB),
        PS_HLE_FUNC(0x523A96C4, cellGameDataSetSystemVer),
        PS_HLE_FUNC(0x9949BF82, cellGameDataExitBroken),
        PS_HLE_FUNC(0xE7951DEE, cellGameDataCheckCreate),
        PS_HLE_FUNC(0xEDC34E1A, cellDiscGameUnregisterDiscChangeCallback),
        PS_HLE_FUNC(0xF8115D69, cellGameRegisterDiscChangeCallback),
        PS_HLE_FUNC(0x21FC0C71, cellGameUnregisterDiscChangeCallback),
    });
}

} // namespace pspocket::hle
