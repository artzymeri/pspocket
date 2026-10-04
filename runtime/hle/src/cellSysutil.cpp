// cellSysutil, cellSysutilAvconfExt: system utility callbacks, system parameters, the system cache (/dev_hdd1),
// video and audio output state, and background music playback. Reference: RPCS3 rpcs3/Emu/Cell/Modules/
// cellSysutil.cpp/.h, cellVideoOut.cpp/.h, cellAudioOut.cpp/.h, cellSysCache.cpp, cellAvconfExt.cpp, with RPCS3's
// default configuration (Emu/system_config.h, rsx::avconf in Emu/RSX/Utils/rsx_utils.h).
//
// Implemented: what GTA V imports from these modules except the dialog and on-screen keyboard functions (save data:
// cellSaveData.cpp), which nothing has called yet (CLAUDE.md rule 4).
//
// The console reported is RPCS3's default one, which also suits a fixed iPhone target: one HDMI display at 720p
// (1280x720 progressive, 16:9: the PS3 mode closest to a phone screen, and the resolution most games render at), RGB,
// no 3D; stereo LPCM at 48 kHz (a phone has stereo speakers; with RPCS3's "stereo" audio format PARAM.SFO's
// SOUND_FORMAT doesn't matter); English (US), enter on cross, DD/MM/YYYY, 24-hour clock, RPCS3's fixed time zone
// value (180) without daylight saving time, parental control off, no PSN account (D-024). Two values differ from
// RPCS3: the system nickname is "PSPocket" (RPCS3 makes up "RPCS3-nnn"), and the user name comes from
// /dev_hdd0/home/00000001/localusername if the host provides that file, else it is "User", which RPCS3 writes there.
//
// System events reach the game through the callbacks it registers: sysutil.hpp queues them (nothing sends one yet,
// there is no system menu) and cellSysutilCheckCallback calls them on the game's thread with call_guest.
//
// The system cache follows RPCS3: the cache is the directory caches/<TITLE_ID>_<cacheId> under the hdd1 root, and
// /dev_hdd1 is mounted on it. The root is the host directory the host mounted as /dev_hdd1, or else "hdd1" next to
// the host directory of /dev_hdd0 (inventory/<game>/hdd1 for scripts/boot.sh), looked up at the first call. A cache
// with the same ID is relayed within a process, and to the next run if the game exited by itself (sys_process_exit);
// a run stopped from outside removes its cache when the process ends, and a crashed run leaves RPCS3's in-use marker
// behind, which clears the cache at the next mount. Deviations from RPCS3: the marker is also written when a cache is
// relayed or cleared (RPCS3 writes it only for a new cache, so a crash after a relay or a cellSysCacheClear would
// relay a half-written cache); files the game still has open in a replaced cache keep working (RPCS3 makes them fail
// with EIO); a host I/O error returns CELL_SYSCACHE_ERROR_ACCESS_ERROR with an error log (RPCS3 throws).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kChan = "cellSysutil";
constexpr std::string_view kAvconfChan = "cellAvconfExt";

enum : u32 {
    CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM = 0x8002b101,
    CELL_SYSUTIL_ERROR_VALUE = 0x8002b102,
    CELL_SYSUTIL_ERROR_SIZE = 0x8002b103,
    CELL_SYSUTIL_ERROR_BUSY = 0x8002b105,

    CELL_SYSCACHE_ERROR_ACCESS_ERROR = 0x8002bc01,
    CELL_SYSCACHE_ERROR_PARAM = 0x8002bc03,
    CELL_SYSCACHE_ERROR_NOTMOUNTED = 0x8002bc04,

    CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION = 0x8002b221,
    CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER = 0x8002b222,
    CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE = 0x8002b223,
    CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND = 0x8002b224,
    CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT = 0x8002b225,
    CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE = 0x8002b226,

    CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER = 0x8002b242,
    CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE = 0x8002b243,
    CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT = 0x8002b245,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM: return "CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM";
    case CELL_SYSUTIL_ERROR_VALUE: return "CELL_SYSUTIL_ERROR_VALUE";
    case CELL_SYSUTIL_ERROR_SIZE: return "CELL_SYSUTIL_ERROR_SIZE";
    case CELL_SYSUTIL_ERROR_BUSY: return "CELL_SYSUTIL_ERROR_BUSY";
    case CELL_SYSCACHE_ERROR_ACCESS_ERROR: return "CELL_SYSCACHE_ERROR_ACCESS_ERROR";
    case CELL_SYSCACHE_ERROR_PARAM: return "CELL_SYSCACHE_ERROR_PARAM";
    case CELL_SYSCACHE_ERROR_NOTMOUNTED: return "CELL_SYSCACHE_ERROR_NOTMOUNTED";
    case CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION: return "CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION";
    case CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER: return "CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER";
    case CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE: return "CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE";
    case CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND: return "CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND";
    case CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT: return "CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT";
    case CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE: return "CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE";
    case CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER: return "CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER";
    case CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE: return "CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE";
    case CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT: return "CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT";
    }
    return r > 0 ? std::to_string(r) : std::format("{:#010x}", static_cast<u32>(r));
}

template <class T>
T& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSysutil called without an HLE process");
    return p->state<T>();
}

// ---------------------------------------------------------------------------------------------------- call logs

enum class Fn : u8 {
    RegisterCallback,
    UnregisterCallback,
    CheckCallback,
    GetSystemParamInt,
    GetSystemParamString,
    SysCacheMount,
    SysCacheClear,
    VideoOutGetState,
    VideoOutGetResolution,
    VideoOutConfigure,
    VideoOutGetDeviceInfo,
    VideoOutGetResolutionAvailability,
    AudioOutGetState,
    AudioOutConfigure,
    AudioOutGetConfiguration,
    AudioOutGetSoundAvailability,
    VideoOutSetGamma,
    AudioOutGetAvailableDeviceInfo,
    EnableBgmPlayback,
    DisableBgmPlayback,
    GetBgmPlaybackStatus,
    Count,
};

struct SysutilCalls {
    std::array<std::atomic<u32>, static_cast<usize>(Fn::Count)> calls{};
};

constexpr u32 kInfoCalls = 8;       // calls logged at info, per function and process
constexpr u32 kPolledInfoCalls = 1; // the same for functions games call every frame

// One call's log line, "name(arguments): result", written when the function returns: at info for the function's
// first calls in this process, so a boot log shows what the game asked for, then at debug, so polled functions don't
// flood the log. An error result is a warning while the call is logged at info.
class Call {
public:
    template <class... A>
    Call(Fn fn, u32 info_calls, std::string_view chan, std::format_string<A...> fmt, A&&... args) : chan_(chan)
    {
        const u32 n = state<SysutilCalls>().calls[static_cast<usize>(fn)].fetch_add(1, std::memory_order_relaxed);
        level_ = n < info_calls ? log::Level::Info : log::Level::Debug;
        if (logs()) text_ = std::format(fmt, std::forward<A>(args)...);
    }

    // Returns `result`, logged by its name (or value).
    s32 operator()(s32 result) const
    {
        const log::Level l = result < 0 && level_ == log::Level::Info ? log::Level::Warn : level_;
        if (l >= log::min_level()) log::print(l, chan_, "{}: {}", text_, result_name(result));
        return result;
    }

    // Returns a successful `result`, logged as `fmt` (formatted only when the line is written).
    template <class... A>
    s32 ok(s32 result, std::format_string<A...> fmt, A&&... args) const
    {
        if (logs()) log::print(level_, chan_, "{}: {}", text_, std::format(fmt, std::forward<A>(args)...));
        return result;
    }

    bool logs() const { return level_ >= log::min_level(); }

private:
    std::string_view chan_;
    log::Level level_ = log::Level::Debug;
    std::string text_;
};

// ------------------------------------------------------------------------------------------------ guest strings

std::string read_string(u8* base, u32 addr, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// RPCS3 strcpy_trunc: at most capacity - 1 bytes of `s`, the rest of the buffer zeroed.
void put_string(u8* base, u32 addr, u32 capacity, std::string_view s)
{
    if (!capacity) return;
    const usize n = std::min<usize>(s.size(), capacity - 1);
    std::memcpy(base + addr, s.data(), n);
    std::memset(base + addr + n, 0, capacity - n);
}

// RPCS3 sysutil_check_name_string: [A-Z0-9_-], not starting with '_', minlen to maxlen characters. Games built with
// SDK 3.7 or newer must end the string inside its maxlen bytes; older ones may fill them (RPCS3 then finds the
// terminator of its own copy, as `at` does here). 0, -1 for a bad character, -2 for a bad length.
s32 check_name_string(const std::string& s, s32 minlen, s32 maxlen, u32 sdk_version)
{
    const s32 lastpos = sdk_version > 0x36FFFF ? std::max(maxlen - 1, 0) : maxlen;
    const auto at = [&](s32 i) { return i < static_cast<s32>(s.size()) ? s[static_cast<usize>(i)] : '\0'; };
    char cur = at(0);
    if (cur == '_') return -1;
    for (s32 index = 0;; cur = at(++index)) {
        if (cur == '\0' || index == maxlen) return minlen > index || (maxlen == index && at(lastpos)) ? -2 : 0;
        if ((cur >= 'A' && cur <= 'Z') || (cur >= '0' && cur <= '9') || cur == '-' || cur == '_') continue;
        return -1;
    }
}

// --------------------------------------------------------------------- callbacks and system events (cellSysutil.cpp)

using QueuedCall = std::function<s32(PPUContext&)>;

// RPCS3 sysutil_cb_manager: four callback slots, and the calls queued for the next cellSysutilCheckCallback.
struct SysutilCallbacks {
    struct Slot {
        u32 func = 0; // the callback's descriptor; 0 = empty
        u32 userdata = 0;
    };
    std::mutex mutex;
    std::array<Slot, 4> slots{};
    std::deque<QueuedCall> queue;
    bool drawing = false; // RPCS3 draw_cb_started: DRAWING_BEGIN sent, DRAWING_END not yet
};

std::string_view event_name(u64 status)
{
    switch (status) {
    case CELL_SYSUTIL_REQUEST_EXITGAME: return "REQUEST_EXITGAME";
    case CELL_SYSUTIL_DRAWING_BEGIN: return "DRAWING_BEGIN";
    case CELL_SYSUTIL_DRAWING_END: return "DRAWING_END";
    case CELL_SYSUTIL_SYSTEM_MENU_OPEN: return "SYSTEM_MENU_OPEN";
    case CELL_SYSUTIL_SYSTEM_MENU_CLOSE: return "SYSTEM_MENU_CLOSE";
    case CELL_SYSUTIL_BGMPLAYBACK_PLAY: return "BGMPLAYBACK_PLAY";
    case CELL_SYSUTIL_BGMPLAYBACK_STOP: return "BGMPLAYBACK_STOP";
    case 0x0801: return "NET_CTL_NETSTART_LOADED"; // sent by cellNetCtl
    case 0x0802: return "NET_CTL_NETSTART_FINISHED";
    case 0x0803: return "NET_CTL_NETSTART_UNLOADED";
    }
    return "?";
}

s32 cellSysutilRegisterCallback(PPUContext&, u32 slot, u32 func, u32 userdata)
{
    const Call call(Fn::RegisterCallback, kInfoCalls, kChan,
        "cellSysutilRegisterCallback(slot {}, func {:#x}, userdata {:#x})", slot, func, userdata);
    if (slot >= 4) return call(error(CELL_SYSUTIL_ERROR_VALUE));
    SysutilCallbacks& cb = state<SysutilCallbacks>();
    std::lock_guard lock(cb.mutex);
    cb.slots[slot] = {func, userdata};
    return call(lv2::CELL_OK);
}

s32 cellSysutilUnregisterCallback(PPUContext&, u32 slot)
{
    const Call call(Fn::UnregisterCallback, kInfoCalls, kChan, "cellSysutilUnregisterCallback(slot {})", slot);
    if (slot >= 4) return call(error(CELL_SYSUTIL_ERROR_VALUE));
    SysutilCallbacks& cb = state<SysutilCallbacks>();
    std::lock_guard lock(cb.mutex);
    cb.slots[slot] = {};
    return call(lv2::CELL_OK);
}

// Runs what was queued before the call, in order, on the calling thread (RPCS3); calls queued meanwhile wait for the
// next check. Deviation: when one returns non-zero (a system event never does), RPCS3 drops the rest of the batch;
// here it stays queued.
s32 cellSysutilCheckCallback(PPUContext& ctx)
{
    const Call call(Fn::CheckCallback, kPolledInfoCalls, kChan, "cellSysutilCheckCallback()");
    SysutilCallbacks& cb = state<SysutilCallbacks>();
    std::deque<QueuedCall> batch;
    {
        std::lock_guard lock(cb.mutex);
        batch.swap(cb.queue);
    }
    while (!batch.empty()) {
        const QueuedCall fn = std::move(batch.front());
        batch.pop_front();
        if (const s32 r = fn(ctx)) {
            std::lock_guard lock(cb.mutex);
            cb.queue.insert(cb.queue.begin(), std::make_move_iterator(batch.begin()),
                std::make_move_iterator(batch.end()));
            return call(r);
        }
    }
    return call(lv2::CELL_OK);
}

// ------------------------------------------------------------------------- system parameters (cellSysutil.cpp)

constexpr s32 CELL_SYSUTIL_SYSTEMPARAM_ID_LICENSE_AREA = 0x0122;

struct IntParam {
    s32 id;
    std::string_view name;
    s32 value;
};

// RPCS3's answers with its default configuration.
constexpr IntParam kIntParams[] = {
    {0x0111, "LANG", 1},                                           // CELL_SYSUTIL_LANG_ENGLISH_US
    {0x0112, "ENTER_BUTTON_ASSIGN", 1},                            // CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS
    {0x0114, "DATE_FORMAT", 1},                                    // CELL_SYSUTIL_DATE_FMT_DDMMYYYY
    {0x0115, "TIME_FORMAT", 1},                                    // CELL_SYSUTIL_TIME_FMT_CLOCK24
    {0x0116, "TIMEZONE", 180},                                     // RPCS3's fixed value
    {0x0117, "SUMMERTIME", 0},                                     //
    {0x0121, "GAME_PARENTAL_LEVEL", 0},                            // CELL_SYSUTIL_GAME_PARENTAL_OFF
    {CELL_SYSUTIL_SYSTEMPARAM_ID_LICENSE_AREA, "LICENSE_AREA", 1}, // CELL_SYSUTIL_LICENSE_AREA_A (SDK 2.4 and older)
    {0x0123, "GAME_PARENTAL_LEVEL0_RESTRICT", 0},                  // CELL_SYSUTIL_GAME_PARENTAL_LEVEL0_RESTRICT_OFF
    {0x0141, "CURRENT_USER_HAS_NP_ACCOUNT", 0},                    // no PSN (D-024)
    {0x0151, "CAMERA_PLFREQ", 0},                                  // CELL_SYSUTIL_CAMERA_PLFREQ_DISABLED
    {0x0152, "PAD_RUMBLE", 1},                                     // CELL_SYSUTIL_PAD_RUMBLE_ON
    {0x0153, "KEYBOARD_TYPE", 0},                                  // CELL_KB_MAPPING_101 (US)
    {0x0154, "JAPANESE_KEYBOARD_ENTRY_METHOD", 0},                 // CELL_SYSUTIL_KEYBOARD_ENTRY_METHOD_ROMAJI_INPUT
    {0x0155, "CHINESE_KEYBOARD_ENTRY_METHOD", 0},                  // CELL_SYSUTIL_KEYBOARD_ENTRY_METHOD_ZHUYIN_INPUT
    {0x0156, "PAD_AUTOOFF", 0},                                    // CELL_SYSUTIL_PAD_AUTOOFF_OFF
    {0x0157, "MAGNETOMETER", 0},                                   // CELL_SYSUTIL_MAGNETOMETER_OFF
};

const IntParam* int_param(s32 id)
{
    for (const IntParam& p : kIntParams)
        if (p.id == id) return &p;
    return nullptr;
}

enum : s32 {
    CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME = 0x0113,
    CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME = 0x0131,
    CELL_SYSUTIL_SYSTEMPARAM_ID_x1008 = 0x1008, // the x IDs: games ask for them, their meaning is unknown
    CELL_SYSUTIL_SYSTEMPARAM_ID_x1011 = 0x1011,
    CELL_SYSUTIL_SYSTEMPARAM_ID_x1012 = 0x1012,
    CELL_SYSUTIL_SYSTEMPARAM_ID_x1024 = 0x1024,
};
constexpr std::string_view kNickname = "PSPocket";
constexpr std::string_view kDefaultUserName = "User";

s32 cellSysutilGetSystemParamInt(PPUContext& ctx, s32 id, u32 value)
{
    const IntParam* p = int_param(id);
    const Call call(Fn::GetSystemParamInt, kInfoCalls, kChan, "cellSysutilGetSystemParamInt({:#x} {}, value {:#x})", id,
        p ? p->name : "?", value);
    if (!value || !p) return call(error(CELL_SYSUTIL_ERROR_VALUE));
    // RPCS3: games built with SDK 2.4 or newer ask cellSysutilGetLicenseArea instead.
    if (id == CELL_SYSUTIL_SYSTEMPARAM_ID_LICENSE_AREA && thread_of(ctx).kernel.config().sdk_version > 0x23FFFFu)
        return call(error(CELL_SYSUTIL_ERROR_VALUE));
    ps_write32(guest_base(ctx), value, static_cast<u32>(p->value));
    return call.ok(lv2::CELL_OK, "{}", p->value);
}

// RPCS3 reads the current user's name from /dev_hdd0/home/<user ID>/localusername (user 1), a file it creates with
// "User" in it.
std::string current_user_name(lv2::Kernel& kernel)
{
    const std::string host = kernel.vfs_host_path("/dev_hdd0/home/00000001/localusername");
    if (host.empty()) return std::string(kDefaultUserName);
    std::ifstream in(host, std::ios::binary);
    if (!in) return std::string(kDefaultUserName);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

s32 cellSysutilGetSystemParamString(PPUContext& ctx, s32 id, u32 buf, u32 bufsize)
{
    const Call call(Fn::GetSystemParamString, kInfoCalls, kChan,
        "cellSysutilGetSystemParamString({:#x}, buf {:#x}, bufsize {:#x})", id, buf, bufsize);
    if (!buf) return call(error(CELL_SYSUTIL_ERROR_VALUE));
    u32 size = 0;
    std::string value = "Unknown";
    bool unknown = false;
    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME:
        size = 0x80; // CELL_SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE
        value = kNickname;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME:
        size = 0x40; // CELL_SYSUTIL_SYSTEMPARAM_CURRENT_USERNAME_SIZE
        value = current_user_name(thread_of(ctx).kernel);
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_x1011:
    case CELL_SYSUTIL_SYSTEMPARAM_ID_x1012:
        size = 0x400;
        unknown = true;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_x1024:
        size = 0x100;
        unknown = true;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_x1008:
        size = 0x4;
        unknown = true;
        break;
    default: return call(error(CELL_SYSUTIL_ERROR_VALUE));
    }
    if (bufsize != size) return call(error(CELL_SYSUTIL_ERROR_SIZE));
    if (unknown) {
        PS_LOG_ERROR(kChan, "cellSysutilGetSystemParamString: unknown parameter {:#x}, answered \"Unknown\" (RPCS3)",
            id);
    }
    put_string(guest_base(ctx), buf, size, value);
    return call.ok(lv2::CELL_OK, "\"{}\"", value.substr(0, size - 1));
}

// ----------------------------------------------------------------------------------- system cache (cellSysCache.cpp)

constexpr s32 CELL_SYSCACHE_RET_OK_CLEARED = 0, CELL_SYSCACHE_RET_OK_RELAYED = 1;
constexpr u32 CELL_SYSCACHE_ID_SIZE = 32, CELL_SYSCACHE_PATH_MAX = 1055;
// CellSysCacheParam: char cacheId[32], char getCachePath[1055], be32 reserved (at 0x440).
constexpr u32 kParamCachePath = 0x20;
// RPCS3's in-use marker (get_syscache_state_corruption_indicator_file_path): there while a process may write the
// cache. Its name starts with a fullwidth dollar sign, which can't clash with a game's (ASCII) file names.
constexpr std::string_view kInUseMarker = "\xEF\xBC\x84"
                                          "hdd0_temp_state_indicator";

// The title part of a cache name (RPCS3 Emu.GetTitleID()), kept to characters that are safe in a host file name.
std::string cache_title(lv2::Kernel& kernel)
{
    std::string title = HleProcess::current()->config().game.title_id;
    if (title.empty()) title = kernel.config().title_id;
    for (char& c : title) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!safe) c = '_';
    }
    return title;
}

// Deletes everything inside `dir`, keeping `dir` (RPCS3 vfs::host::remove_all without remove_root).
bool clear_dir(const fs::path& dir)
{
    std::error_code ec;
    std::vector<fs::path> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
    if (ec) return false;
    for (const fs::path& p : entries) {
        fs::remove_all(p, ec);
        if (ec) return false;
    }
    return true;
}

// RPCS3 syscache_info.
struct SysCache {
    explicit SysCache(HleProcess& p) : process(p) {}
    ~SysCache();
    SysCache(const SysCache&) = delete;
    SysCache& operator=(const SysCache&) = delete;

    HleProcess& process;
    std::mutex mutex;
    bool looked_up = false; // the root and the caches of earlier runs were looked up
    bool mounted = false;   // a cellSysCacheMount succeeded (RPCS3's init_mutex)
    std::string root;       // the host directory holding caches/; empty when there is none
    std::string id;         // the current cache, root/caches/<id>: "<title>_<cacheId>", or empty

    fs::path dir() const { return fs::path(root) / "caches" / id; }
};

// At process end (RPCS3 ~syscache_info): a game that exited by itself keeps its cache for its next run (RPCS3's
// _sys_process_exit calls signal_system_cache_can_stay); otherwise the cache may be half written and goes. HLE state
// dies with its HleProcess, before the kernel: every host destroys them in that order.
SysCache::~SysCache()
{
    if (root.empty() || id.empty()) return;
    std::error_code ec;
    if (process.kernel().exited()) {
        fs::remove(dir() / kInUseMarker, ec);
        PS_LOG_INFO(kChan, "cellSysCache: the game exited by itself; {} stays for its next run", dir().string());
    } else {
        fs::remove_all(dir(), ec);
        PS_LOG_INFO(kChan, "cellSysCache: the process was stopped from outside; {} removed", dir().string());
    }
}

// The hdd1 root and the cache an earlier run of this title left (RPCS3 does this in syscache_info's constructor at
// process start; here at the first call, before the game can have touched /dev_hdd1). A cache still marked in use
// was being written when its run crashed: it is deleted.
void look_up(SysCache& c, lv2::Kernel& kernel, const std::string& title)
{
    c.looked_up = true;
    std::string root = kernel.vfs_host_path("/dev_hdd1");
    if (root.empty()) {
        const std::string hdd0 = kernel.vfs_host_path("/dev_hdd0");
        if (hdd0.empty()) {
            PS_LOG_ERROR(kChan, "cellSysCache: neither /dev_hdd1 nor /dev_hdd0 is mounted on a host directory, so "
                                "there is no place for the system cache");
            return;
        }
        root = (fs::path(hdd0).parent_path() / "hdd1").string();
    }
    const fs::path caches = fs::path(root) / "caches";
    std::error_code ec;
    fs::create_directories(caches, ec);
    if (ec) {
        PS_LOG_ERROR(kChan, "cellSysCache: cannot create {} ({})", caches.string(), ec.message());
        return;
    }
    c.root = root;
    std::vector<fs::path> found;
    for (const fs::directory_entry& e : fs::directory_iterator(caches, ec))
        if (e.is_directory(ec) && e.path().filename().string().starts_with(title + "_")) found.push_back(e.path());
    std::sort(found.begin(), found.end());
    for (const fs::path& p : found) {
        if (fs::exists(p / kInUseMarker, ec)) {
            PS_LOG_WARN(kChan, "cellSysCache: {} was in use when its run ended without exiting; removed", p.string());
            fs::remove_all(p, ec);
            continue;
        }
        c.id = p.filename().string();
        PS_LOG_INFO(kChan, "cellSysCache: {} kept from an earlier run", p.string());
        break;
    }
}

bool mark_in_use(const SysCache& c)
{
    std::ofstream marker(c.dir() / kInUseMarker, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(marker);
}

s32 cellSysCacheMount(PPUContext& ctx, u32 param)
{
    lv2::Kernel& kernel = thread_of(ctx).kernel;
    u8* base = kernel.base();
    const std::string name = param ? read_string(base, param, CELL_SYSCACHE_ID_SIZE) : std::string();
    const Call call(Fn::SysCacheMount, kInfoCalls, kChan, "cellSysCacheMount(param {:#x}, cacheId \"{}\")", param,
        name);
    if (!param) return call(error(CELL_SYSCACHE_ERROR_PARAM));
    if (!name.empty() && check_name_string(name, 1, CELL_SYSCACHE_ID_SIZE, kernel.config().sdk_version) != 0)
        return call(error(CELL_SYSCACHE_ERROR_PARAM));
    const std::string title = cache_title(kernel);
    const std::string id = title + "_" + name;
    put_string(base, param + kParamCachePath, CELL_SYSCACHE_PATH_MAX, "/dev_hdd1");

    SysCache& c = state<SysCache>();
    std::lock_guard lock(c.mutex);
    if (!c.looked_up) look_up(c, kernel, title);
    if (c.root.empty()) return call(error(CELL_SYSCACHE_ERROR_ACCESS_ERROR));
    std::error_code ec;
    // An empty cache ID is never relayed.
    const bool relayed = !name.empty() && id == c.id && fs::is_directory(c.dir(), ec);
    if (!relayed) {
        if (!c.id.empty()) fs::remove_all(c.dir(), ec); // the previous cache goes
        c.id = id;
        if (!fs::create_directory(c.dir(), ec) && (ec || !clear_dir(c.dir()))) {
            PS_LOG_ERROR(kChan, "cellSysCacheMount: cannot create or clear {}", c.dir().string());
            c.id.clear();
            return call(error(CELL_SYSCACHE_ERROR_ACCESS_ERROR));
        }
    }
    if (!mark_in_use(c) || !kernel.vfs_mount("/dev_hdd1", c.dir().string(), false)) {
        PS_LOG_ERROR(kChan, "cellSysCacheMount: cannot use {} as /dev_hdd1", c.dir().string());
        return call(error(CELL_SYSCACHE_ERROR_ACCESS_ERROR));
    }
    c.mounted = true;
    return call.ok(relayed ? CELL_SYSCACHE_RET_OK_RELAYED : CELL_SYSCACHE_RET_OK_CLEARED, "{}, /dev_hdd1 = {}",
        relayed ? "CELL_SYSCACHE_RET_OK_RELAYED" : "CELL_SYSCACHE_RET_OK_CLEARED", c.dir().string());
}

s32 cellSysCacheClear(PPUContext&)
{
    const Call call(Fn::SysCacheClear, kInfoCalls, kChan, "cellSysCacheClear()");
    SysCache& c = state<SysCache>();
    std::lock_guard lock(c.mutex);
    if (!c.mounted) return call(error(CELL_SYSCACHE_ERROR_NOTMOUNTED));
    if (!c.id.empty() && (!clear_dir(c.dir()) || !mark_in_use(c))) {
        PS_LOG_ERROR(kChan, "cellSysCacheClear: cannot clear {}", c.dir().string());
        return call(error(CELL_SYSCACHE_ERROR_ACCESS_ERROR));
    }
    return call.ok(CELL_SYSCACHE_RET_OK_CLEARED, "CELL_SYSCACHE_RET_OK_CLEARED");
}

// ----------------------------------------------------------------------- video output (cellVideoOut.cpp/.h)

constexpr u32 CELL_VIDEO_OUT_PRIMARY = 0, CELL_VIDEO_OUT_SECONDARY = 1;
constexpr u8 CELL_VIDEO_OUT_RESOLUTION_720 = 2;
constexpr u8 CELL_VIDEO_OUT_RESOLUTION_720_3D_FRAME_PACKING = 0x81;
constexpr u8 CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE = 1;
constexpr u8 CELL_VIDEO_OUT_ASPECT_AUTO = 0, CELL_VIDEO_OUT_ASPECT_4_3 = 1, CELL_VIDEO_OUT_ASPECT_16_9 = 2;
constexpr u8 CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED = 0;
constexpr u8 CELL_VIDEO_OUT_COLOR_SPACE_RGB = 1;
constexpr u8 CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE = 0;
constexpr u16 CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ = 1, CELL_VIDEO_OUT_REFRESH_RATE_60HZ = 4;
constexpr u8 CELL_VIDEO_OUT_PORT_HDMI = 1;
constexpr u8 CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE = 1;
constexpr u8 CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_FULL = 1;

// The display: RPCS3's default 720p, progressive, 16:9, without 3D.
constexpr u8 kResolutionId = CELL_VIDEO_OUT_RESOLUTION_720;
constexpr u8 kAspect = CELL_VIDEO_OUT_ASPECT_16_9;

struct Resolution {
    u16 width = 0, height = 0;
};

// RPCS3 _IntGetResolutionInfo (a PS3 also answers some IDs that have no enumerant).
std::optional<Resolution> resolution_of(u8 id)
{
    switch (id) {
    case 0x01: return Resolution{0x780, 0x438}; // 1080
    case 0x02: return Resolution{0x500, 0x2d0}; // 720
    case 0x04: return Resolution{0x2d0, 0x1e0}; // 480
    case 0x05: return Resolution{0x2d0, 0x240}; // 576
    case 0x0a: return Resolution{0x640, 0x438}; // 1600x1080
    case 0x0b: return Resolution{0x5a0, 0x438}; // 1440x1080
    case 0x0c: return Resolution{0x500, 0x438}; // 1280x1080
    case 0x0d: return Resolution{0x3c0, 0x438}; // 960x1080
    case 0x64: return Resolution{0x550, 0x300};
    case 0x81: return Resolution{0x500, 0x5be}; // 720_3D_FRAME_PACKING
    case 0x82: return Resolution{0x780, 0x438};
    case 0x83: return Resolution{0x780, 0x89d};
    case 0x88: return Resolution{0x400, 0x5be}; // 1024x720_3D_FRAME_PACKING
    case 0x89: return Resolution{0x3c0, 0x5be}; // 960x720_3D_FRAME_PACKING
    case 0x8a: return Resolution{0x320, 0x5be}; // 800x720_3D_FRAME_PACKING
    case 0x8b: return Resolution{0x280, 0x5be}; // 640x720_3D_FRAME_PACKING
    case 0x91: return Resolution{0x500, 0x5be}; // 720_DUALVIEW_FRAME_PACKING
    case 0x92: return Resolution{0x780, 0x438};
    case 0x98: return Resolution{0x400, 0x5be}; // 1024x720_DUALVIEW_FRAME_PACKING
    case 0x99: return Resolution{0x3c0, 0x5be}; // 960x720_DUALVIEW_FRAME_PACKING
    case 0x9a: return Resolution{0x320, 0x5be}; // 800x720_DUALVIEW_FRAME_PACKING
    case 0x9b: return Resolution{0x280, 0x5be}; // 640x720_DUALVIEW_FRAME_PACKING
    case 0xa1: return Resolution{0x780, 0x438};
    }
    return std::nullopt;
}

// RPCS3 rsx::avconf: the mode cellVideoOutConfigure chose (the display's own until then), and the gamma.
struct VideoOutState {
    std::mutex mutex;
    bool configured = false;
    u8 resolution_id = kResolutionId;
    u8 format = 0; // CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8
    u8 aspect = kAspect;
    u32 pitch = 0;
    Resolution resolution{0x500, 0x2d0};
    f32 gamma = 1.0f;
};

// RPCS3 cellVideoOutGetNumberOfDevice: one display on the primary output, none on the secondary.
s32 video_out_devices(u32 video_out)
{
    if (video_out == CELL_VIDEO_OUT_PRIMARY) return 1;
    if (video_out == CELL_VIDEO_OUT_SECONDARY) return 0;
    return error(CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT);
}

// CellVideoOutState: u8 state, colorSpace, reserved[6]; CellVideoOutDisplayMode {u8 resolutionId, scanMode,
// conversion, aspect, reserved[2]; be16 refreshRates} at 8. Only these fields are written, as in RPCS3.
s32 cellVideoOutGetState(PPUContext& ctx, u32 video_out, u32 device_index, u32 state_addr)
{
    const Call call(Fn::VideoOutGetState, kPolledInfoCalls, kChan,
        "cellVideoOutGetState(videoOut {}, deviceIndex {}, state {:#x})", video_out, device_index, state_addr);
    if (!state_addr) return call(error(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER));
    // The secondary output has no device, so only the primary display gets past this.
    const s32 devices = video_out_devices(video_out);
    if (devices < 0 || device_index >= static_cast<u32>(devices)) {
        return call(error(CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND));
    }
    VideoOutState& v = state<VideoOutState>();
    std::lock_guard lock(v.mutex);
    u8* base = guest_base(ctx);
    ps_write8(base, state_addr + 0, CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED);
    ps_write8(base, state_addr + 1, CELL_VIDEO_OUT_COLOR_SPACE_RGB);
    ps_write8(base, state_addr + 8, v.resolution_id);
    ps_write8(base, state_addr + 9, CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE);
    ps_write8(base, state_addr + 10, CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE);
    ps_write8(base, state_addr + 11, v.aspect);
    ps_write16(base, state_addr + 14, CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);
    return call.ok(lv2::CELL_OK, "enabled, resolution {:#x} ({}x{}), aspect {}, progressive, 59.94 Hz", v.resolution_id,
        v.resolution.width, v.resolution.height, v.aspect);
}

// CellVideoOutResolution: be16 width, height.
s32 cellVideoOutGetResolution(PPUContext& ctx, u32 resolution_id, u32 resolution)
{
    const Call call(Fn::VideoOutGetResolution, kInfoCalls, kChan, "cellVideoOutGetResolution({:#x}, resolution {:#x})",
        resolution_id, resolution);
    if (!resolution) return call(error(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER));
    const std::optional<Resolution> r = resolution_of(static_cast<u8>(resolution_id)); // RPCS3 takes the ID as a u8
    if (!r) return call(error(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER));
    u8* base = guest_base(ctx);
    ps_write16(base, resolution + 0, r->width);
    ps_write16(base, resolution + 2, r->height);
    return call.ok(lv2::CELL_OK, "{}x{}", r->width, r->height);
}

// CellVideoOutConfiguration: u8 resolutionId, format, aspect, reserved[9]; be32 pitch at 12. RPCS3 doesn't check
// videoOut here. It also turns its renderer's vsync back on; the RSX here has no vsync setting yet.
s32 cellVideoOutConfigure(PPUContext& ctx, u32 video_out, u32 config, u32 option, u32 wait_for_event)
{
    u8* base = guest_base(ctx);
    const u8 resolution_id = config ? ps_read8(base, config + 0) : u8{0};
    const u8 format = config ? ps_read8(base, config + 1) : u8{0};
    u8 aspect = config ? ps_read8(base, config + 2) : u8{0};
    const u32 pitch = config ? ps_read32(base, config + 12) : 0;
    const Call call(Fn::VideoOutConfigure, kInfoCalls, kChan,
        "cellVideoOutConfigure(videoOut {}, config {:#x} {{resolution {:#x}, format {}, aspect {}, pitch {:#x}}}, "
        "option {:#x}, waitForEvent {})",
        video_out, config, resolution_id, format, aspect, pitch, option, wait_for_event);
    if (!config) return call(error(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER));
    if (!pitch) return call(error(CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE));
    if (resolution_id == 0x92 || resolution_id == 0xa1) return call(error(CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION));
    const std::optional<Resolution> r = resolution_of(resolution_id);
    if (!r || resolution_id >= CELL_VIDEO_OUT_RESOLUTION_720_3D_FRAME_PACKING) { // the 3D modes need a 3D display
        PS_LOG_ERROR(kChan, "cellVideoOutConfigure: unusual resolution {:#x} requested", resolution_id);
        return call(error(CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE));
    }
    // RPCS3: auto, or an unknown aspect, becomes the display's.
    if (aspect != CELL_VIDEO_OUT_ASPECT_4_3 && aspect != CELL_VIDEO_OUT_ASPECT_16_9) {
        if (aspect != CELL_VIDEO_OUT_ASPECT_AUTO) {
            PS_LOG_ERROR(kChan, "cellVideoOutConfigure: unknown aspect {}, using the display's (16:9)", aspect);
        }
        aspect = kAspect;
    }
    VideoOutState& v = state<VideoOutState>();
    std::lock_guard lock(v.mutex);
    v.configured = true;
    v.resolution_id = resolution_id;
    v.format = format;
    v.aspect = aspect;
    v.pitch = pitch;
    v.resolution = *r;
    return call.ok(lv2::CELL_OK, "{}x{}, aspect {}", r->width, r->height, aspect);
}

// CellVideoOutDeviceInfo: u8 portType, colorSpace; be16 latency; u8 availableModeCount, state, rgbOutputRange,
// reserved[5]; CellVideoOutColorInfo at 12 {be16 redX, redY, greenX, greenY, blueX, blueY, whiteX, whiteY; be32
// gamma}; CellVideoOutDisplayMode availableModes[32] at 32, 8 bytes each; CellVideoOutKSVList at 288. RPCS3's
// "standard dummy values"; only these fields are written.
s32 cellVideoOutGetDeviceInfo(PPUContext& ctx, u32 video_out, u32 device_index, u32 info)
{
    const Call call(Fn::VideoOutGetDeviceInfo, kInfoCalls, kChan,
        "cellVideoOutGetDeviceInfo(videoOut {}, deviceIndex {}, info {:#x})", video_out, device_index, info);
    if (!info) return call(error(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER));
    const s32 devices = video_out_devices(video_out);
    if (devices < 0 || device_index >= static_cast<u32>(devices)) {
        return call(error(CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND));
    }
    u8* base = guest_base(ctx);
    ps_write8(base, info + 0, CELL_VIDEO_OUT_PORT_HDMI);
    ps_write8(base, info + 1, CELL_VIDEO_OUT_COLOR_SPACE_RGB);
    ps_write16(base, info + 2, 100); // latency
    ps_write8(base, info + 5, CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE);
    ps_write8(base, info + 6, CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_FULL);
    for (u32 i = 0; i < 8; i++) ps_write16(base, info + 12 + 2 * i, 0xFFFF); // chromaticities: unknown
    ps_write32(base, info + 28, 100);                                          // gamma
    // One mode, the display's own (RPCS3 lists more only for its 1080 and SD settings, and 3D modes for a 3D display).
    const u32 mode = info + 32;
    ps_write8(base, mode + 0, kResolutionId);
    ps_write8(base, mode + 1, CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE);
    ps_write8(base, mode + 2, CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE);
    ps_write8(base, mode + 3, kAspect);
    ps_write16(base, mode + 6, CELL_VIDEO_OUT_REFRESH_RATE_60HZ | CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);
    ps_write8(base, info + 4, 1); // availableModeCount
    return call.ok(lv2::CELL_OK, "HDMI, 1 mode: resolution {:#x}, aspect {}, progressive", kResolutionId, kAspect);
}

// The result is a boolean, not an error code.
s32 cellVideoOutGetResolutionAvailability(PPUContext&, u32 video_out, u32 resolution_id, u32 aspect, u32 option)
{
    const Call call(Fn::VideoOutGetResolutionAvailability, kInfoCalls, kChan,
        "cellVideoOutGetResolutionAvailability(videoOut {}, resolution {:#x}, aspect {}, option {})", video_out,
        resolution_id, aspect, option);
    const auto available = [&](bool yes) { return call.ok(yes ? 1 : 0, "{}", yes ? "available" : "not available"); };
    switch (video_out) {
    case CELL_VIDEO_OUT_PRIMARY:
        // The display's own resolution, at its aspect or "auto" (RPCS3 adds the 3D modes for a 3D display).
        return available((aspect == CELL_VIDEO_OUT_ASPECT_AUTO || aspect == kAspect) && resolution_id == kResolutionId);
    case CELL_VIDEO_OUT_SECONDARY: return available(false);
    }
    return call(error(CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT));
}

// cellSysutilAvconfExt. gamma is a float argument, so it arrives in f1 (the typed binding passes integer registers
// only; RPCS3's binder reads ppu.fpr[1] for the first float argument).
s32 cellVideoOutSetGamma(PPUContext& ctx, u32 video_out)
{
    const f32 gamma = static_cast<f32>(ctx.fpr[1]);
    const Call call(Fn::VideoOutSetGamma, kInfoCalls, kAvconfChan, "cellVideoOutSetGamma(videoOut {}, gamma {})",
        video_out, gamma);
    if (gamma < 0.8f || gamma > 1.2f) return call(error(CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE));
    if (video_out != CELL_VIDEO_OUT_PRIMARY) return call(error(CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT));
    VideoOutState& v = state<VideoOutState>();
    std::lock_guard lock(v.mutex);
    v.gamma = gamma; // for the presenter; nothing applies it yet (RPCS3 corrects its output image with it)
    return call(lv2::CELL_OK);
}

// ----------------------------------------------------------------------- audio output (cellAudioOut.cpp/.h)

constexpr u32 CELL_AUDIO_OUT_PRIMARY = 0, CELL_AUDIO_OUT_SECONDARY = 1;
constexpr u32 CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED = 0;
constexpr u8 CELL_AUDIO_OUT_CODING_TYPE_LPCM = 0;
constexpr u8 CELL_AUDIO_OUT_CHNUM_2 = 2;
constexpr u8 CELL_AUDIO_OUT_FS_48KHZ = 0x04;
constexpr u32 CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH = 1;
constexpr u32 CELL_AUDIO_OUT_DOWNMIXER_NONE = 0, CELL_AUDIO_OUT_DOWNMIXER_TYPE_B = 2;

// CellAudioOutSoundMode: u8 type, channel, fs, reserved; be32 layout.
struct SoundMode {
    u8 type = 0, channel = 0, fs = 0;
    u32 layout = 0;
};
constexpr SoundMode kStereo{CELL_AUDIO_OUT_CODING_TYPE_LPCM, CELL_AUDIO_OUT_CHNUM_2, CELL_AUDIO_OUT_FS_48KHZ,
    CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH};

void write_sound_mode(u8* base, u32 addr, const SoundMode& m)
{
    ps_write8(base, addr + 0, m.type);
    ps_write8(base, addr + 1, m.channel);
    ps_write8(base, addr + 2, m.fs);
    ps_write8(base, addr + 3, 0);
    ps_write32(base, addr + 4, m.layout);
}

// RPCS3 audio_out_configuration with its default ("stereo") audio format: each output offers one mode, LPCM 2 ch
// at 48 kHz, selected from the start.
struct AudioOutState {
    struct Out {
        u32 state = CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED;
        u32 channels = CELL_AUDIO_OUT_CHNUM_2;
        u32 encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
        u32 downmixer = CELL_AUDIO_OUT_DOWNMIXER_NONE;
        std::vector<SoundMode> sound_modes{kStereo};
        SoundMode sound_mode = kStereo;
    };
    std::mutex mutex;
    std::array<Out, 2> out{};
};

// RPCS3 cellAudioOutGetNumberOfDevice: one device on the primary output, none on the secondary.
s32 audio_out_devices(u32 audio_out)
{
    if (audio_out == CELL_AUDIO_OUT_PRIMARY) return 1;
    if (audio_out == CELL_AUDIO_OUT_SECONDARY) return 0;
    return error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER);
}

// CellAudioOutState: u8 state, encoder, reserved[6]; be32 downMixer at 8; CellAudioOutSoundMode at 12 (20 bytes).
s32 cellAudioOutGetState(PPUContext& ctx, u32 audio_out, u32 device_index, u32 state_addr)
{
    const Call call(Fn::AudioOutGetState, kPolledInfoCalls, kChan,
        "cellAudioOutGetState(audioOut {}, deviceIndex {}, state {:#x})", audio_out, device_index, state_addr);
    if (!state_addr) return call(error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER));
    const s32 devices = audio_out_devices(audio_out);
    if (devices < 0) return call(devices);
    if (device_index >= static_cast<u32>(devices) && audio_out != CELL_AUDIO_OUT_SECONDARY) {
        return call(error(CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE));
    }
    u8* base = guest_base(ctx);
    std::memset(base + state_addr, 0, 20);
    if (device_index >= static_cast<u32>(devices)) {
        // A PS3 returns no error here, and what looks like stale stack data; RPCS3 writes the values it saw.
        ps_write8(base, state_addr + 0, 0x10);
        ps_write32(base, state_addr + 16, 0xD00C1680);
        return call(lv2::CELL_OK);
    }
    AudioOutState& a = state<AudioOutState>();
    std::lock_guard lock(a.mutex);
    const AudioOutState::Out& out = a.out[audio_out];
    ps_write8(base, state_addr + 0, static_cast<u8>(out.state));
    ps_write8(base, state_addr + 1, static_cast<u8>(out.encoder));
    ps_write32(base, state_addr + 8, out.downmixer);
    write_sound_mode(base, state_addr + 12, out.sound_mode);
    return call.ok(lv2::CELL_OK, "state {}, encoder {}, downMixer {}, mode {{type {}, {} ch, fs {:#x}, layout {:#x}}}",
        out.state, out.encoder, out.downmixer, out.sound_mode.type, out.sound_mode.channel, out.sound_mode.fs,
        out.sound_mode.layout);
}

// CellAudioOutConfiguration: u8 channel, encoder, reserved[10]; be32 downMixer at 12.
s32 cellAudioOutConfigure(PPUContext& ctx, u32 audio_out, u32 config, u32 option, u32 wait_for_event)
{
    u8* base = guest_base(ctx);
    const u8 channel = config ? ps_read8(base, config + 0) : u8{0};
    const u8 encoder = config ? ps_read8(base, config + 1) : u8{0};
    const u32 down_mixer = config ? ps_read32(base, config + 12) : 0;
    const Call call(Fn::AudioOutConfigure, kInfoCalls, kChan,
        "cellAudioOutConfigure(audioOut {}, config {:#x} {{{} ch, encoder {}, downMixer {}}}, option {:#x}, "
        "waitForEvent {})",
        audio_out, config, channel, encoder, down_mixer, option, wait_for_event);
    if (!config) return call(error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER));
    if (audio_out == CELL_AUDIO_OUT_SECONDARY) return call(error(CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT));
    if (audio_out != CELL_AUDIO_OUT_PRIMARY) return call(error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER));
    AudioOutState& a = state<AudioOutState>();
    std::lock_guard lock(a.mutex);
    AudioOutState::Out& out = a.out[audio_out];
    if (out.channels != channel || out.encoder != encoder || out.downmixer != down_mixer) {
        // The configuration doesn't have to be one of the offered modes.
        out.channels = channel;
        out.encoder = encoder;
        if (down_mixer > CELL_AUDIO_OUT_DOWNMIXER_TYPE_B) { // a PS3 keeps the previous valid one
            PS_LOG_WARN(kChan, "cellAudioOutConfigure: invalid downMixer {}, keeping {}", down_mixer, out.downmixer);
        } else {
            out.downmixer = down_mixer;
        }
        const auto it = std::find_if(out.sound_modes.begin(), out.sound_modes.end(),
            [&](const SoundMode& m) { return m.type == out.encoder && m.channel == out.channels; });
        if (it != out.sound_modes.end()) {
            out.sound_mode = *it;
        } else {
            PS_LOG_WARN(kChan, "cellAudioOutConfigure: no {} ch mode with encoder {}; the output stays at {} ch",
                channel, encoder, out.sound_mode.channel);
        }
        // RPCS3 now restarts its audio backend for the new layout (the output is DISABLED meanwhile). There is no
        // audio output here yet.
    }
    return call(lv2::CELL_OK);
}

s32 cellAudioOutGetConfiguration(PPUContext& ctx, u32 audio_out, u32 config, u32 option)
{
    const Call call(Fn::AudioOutGetConfiguration, kInfoCalls, kChan,
        "cellAudioOutGetConfiguration(audioOut {}, config {:#x}, option {:#x})", audio_out, config, option);
    if (!config) return call(error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER));
    if (audio_out == CELL_AUDIO_OUT_SECONDARY) return call(error(CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT));
    if (audio_out != CELL_AUDIO_OUT_PRIMARY) return call(error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER));
    AudioOutState& a = state<AudioOutState>();
    std::lock_guard lock(a.mutex);
    const AudioOutState::Out& out = a.out[audio_out];
    u8* base = guest_base(ctx);
    std::memset(base + config, 0, 16);
    ps_write8(base, config + 0, static_cast<u8>(out.channels));
    ps_write8(base, config + 1, static_cast<u8>(out.encoder));
    ps_write32(base, config + 12, out.downmixer);
    return call.ok(lv2::CELL_OK, "{} ch, encoder {}, downMixer {}", out.channels, out.encoder, out.downmixer);
}

// The most channels a mode of this coding type and sample rate offers, 0 for none (not an error code). Only the
// primary output answers (RPCS3: there is no peripheral output).
s32 cellAudioOutGetSoundAvailability(PPUContext&, u32 audio_out, u32 type, u32 fs, u32 option)
{
    const Call call(Fn::AudioOutGetSoundAvailability, kInfoCalls, kChan,
        "cellAudioOutGetSoundAvailability(audioOut {}, type {}, fs {:#x}, option {})", audio_out, type, fs, option);
    s32 channels = 0;
    if (audio_out == CELL_AUDIO_OUT_PRIMARY) {
        AudioOutState& a = state<AudioOutState>();
        std::lock_guard lock(a.mutex);
        for (const SoundMode& m : a.out[audio_out].sound_modes)
            if (m.type == type && m.fs == fs) channels = std::max<s32>(channels, m.channel);
    }
    return call.ok(channels, "{} ch", channels);
}

// cellSysutilAvconfExt: audio devices besides the built-in outputs (USB, Bluetooth). There are none (RPCS3), and
// nothing is written.
s32 cellAudioOutGetAvailableDeviceInfo(PPUContext&, u32 count, u32 info)
{
    const Call call(Fn::AudioOutGetAvailableDeviceInfo, kInfoCalls, kAvconfChan,
        "cellAudioOutGetAvailableDeviceInfo(count {}, info {:#x})", count, info);
    if (count > 16 || !info) return call(error(CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER));
    return call.ok(0, "0 devices");
}

// ------------------------------------------------------------------------ background music (cellSysutil.cpp)

// CellSysutilBgmPlaybackStatus (32 bytes): u8 playerState, enableState; char contentId[16]; u8 currentFadeRatio;
// char reserved[13].
constexpr u32 kBgmStatusSize = 32;
constexpr u8 CELL_SYSUTIL_BGMPLAYBACK_STATUS_PLAY = 0, CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP = 1;
constexpr u8 CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE = 0, CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE = 1;

// RPCS3 bgm_manager: whether the user's own music (the system's background music player) may play over the game. There
// is no such player (nor in RPCS3), so it stays stopped, with no content and a fade ratio of 0. Playback starts out
// disabled, as in RPCS3. Enabling and disabling only change that state: RPCS3's fade between the system's music and
// the game's is a TODO, and no BGMPLAYBACK event is sent.
struct BgmState {
    std::mutex mutex;
    u8 enable_state = CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE;
    u8 player_state = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
};

s32 set_bgm_enable_state(Fn fn, std::string_view name, u8 enable_state)
{
    const Call call(fn, kInfoCalls, kChan, "{}()", name);
    BgmState& b = state<BgmState>();
    std::lock_guard lock(b.mutex);
    b.enable_state = enable_state;
    return call(lv2::CELL_OK);
}

s32 cellSysutilEnableBgmPlayback(PPUContext&)
{
    return set_bgm_enable_state(Fn::EnableBgmPlayback, "cellSysutilEnableBgmPlayback",
        CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE);
}

s32 cellSysutilDisableBgmPlayback(PPUContext&)
{
    return set_bgm_enable_state(Fn::DisableBgmPlayback, "cellSysutilDisableBgmPlayback",
        CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE);
}

// The whole status is written (RPCS3 copies its bgm_manager::status): the two states, then zeros. Deviation: a pointer
// that is not guest memory is refused with CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM and the call site logged (D-042;
// RPCS3 faults).
s32 cellSysutilGetBgmPlaybackStatus(PPUContext& ctx, u32 status)
{
    const Call call(Fn::GetBgmPlaybackStatus, kPolledInfoCalls, kChan, "cellSysutilGetBgmPlaybackStatus({:#x})",
        status);
    if (!status) return call(error(CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM));
    if (!guest_readable(thread_of(ctx), status, kBgmStatusSize)) {
        log_call_site(ctx, guest_base(ctx),
            std::format("cellSysutilGetBgmPlaybackStatus: {:#x} is not guest memory", status));
        return call(error(CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM));
    }
    BgmState& b = state<BgmState>();
    std::lock_guard lock(b.mutex);
    u8* base = guest_base(ctx);
    std::memset(base + status, 0, kBgmStatusSize);
    ps_write8(base, status + 0, b.player_state);
    ps_write8(base, status + 1, b.enable_state);
    const bool playing = b.player_state == CELL_SYSUTIL_BGMPLAYBACK_STATUS_PLAY;
    const bool enabled = b.enable_state == CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE;
    return call.ok(lv2::CELL_OK, "player {}, playback {}", playing ? "playing" : "stopped",
        enabled ? "enabled" : "disabled");
}

} // namespace

s32 sysutil_send_event(u64 status, u64 param)
{
    HleProcess* p = HleProcess::current();
    if (!p) return 0;
    SysutilCallbacks& cb = p->state<SysutilCallbacks>();
    std::lock_guard lock(cb.mutex);
    if (status == CELL_SYSUTIL_DRAWING_BEGIN) {
        if (cb.drawing) {
            PS_LOG_ERROR(kChan, "sysutil: a second DRAWING_BEGIN event");
            return error(CELL_SYSUTIL_ERROR_BUSY);
        }
        cb.drawing = true;
    } else if (status == CELL_SYSUTIL_DRAWING_END) {
        if (!cb.drawing) {
            PS_LOG_ERROR(kChan, "sysutil: a DRAWING_END event without DRAWING_BEGIN");
            return -1;
        }
        cb.drawing = false;
    }
    // (RPCS3 also stops its exit watchdog on REQUEST_EXITGAME; there is none here.)
    s32 count = 0;
    for (const SysutilCallbacks::Slot& slot : cb.slots) {
        if (!slot.func) continue;
        cb.queue.push_back([status, param, slot](PPUContext& ctx) -> s32 {
            PS_LOG_INFO(kChan, "cellSysutilCheckCallback: event {:#x} ({}, param {:#x}) to {:#x}, userdata {:#x}",
                status, event_name(status), param, slot.func, slot.userdata);
            call_guest(ctx, slot.func, {status, param, slot.userdata});
            return lv2::CELL_OK; // the callback returns void
        });
        count++;
    }
    PS_LOG_INFO(kChan, "sysutil: event {:#x} ({}), param {:#x}, queued for {} callback(s)", status, event_name(status),
        param, count);
    return count;
}

void sysutil_queue_call(std::function<s32(PPUContext& ctx)> fn)
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sysutil_queue_call without an HLE process");
    SysutilCallbacks& cb = p->state<SysutilCallbacks>();
    std::lock_guard lock(cb.mutex);
    cb.queue.push_back(std::move(fn));
}

void register_cellSysutil(Registry& r)
{
    r.add("cellSysutil", {
        PS_HLE_FUNC(0x9D98AFA0, cellSysutilRegisterCallback),
        PS_HLE_FUNC(0x02FF3C1B, cellSysutilUnregisterCallback),
        PS_HLE_FUNC(0x189A74DA, cellSysutilCheckCallback),
        PS_HLE_FUNC(0x40E895D3, cellSysutilGetSystemParamInt),
        PS_HLE_FUNC(0x938013A0, cellSysutilGetSystemParamString),
        PS_HLE_FUNC(0x1E7BFF94, cellSysCacheMount),
        PS_HLE_FUNC(0x744C1544, cellSysCacheClear),
        PS_HLE_FUNC(0x887572D5, cellVideoOutGetState),
        PS_HLE_FUNC(0xE558748D, cellVideoOutGetResolution),
        PS_HLE_FUNC(0x0BAE8772, cellVideoOutConfigure),
        PS_HLE_FUNC(0x1E930EEF, cellVideoOutGetDeviceInfo),
        PS_HLE_FUNC(0xA322DB75, cellVideoOutGetResolutionAvailability),
        PS_HLE_FUNC(0xF4E3CAA0, cellAudioOutGetState),
        PS_HLE_FUNC(0x4692AB35, cellAudioOutConfigure),
        PS_HLE_FUNC(0xED5D96AF, cellAudioOutGetConfiguration),
        PS_HLE_FUNC(0xC01B4E7C, cellAudioOutGetSoundAvailability),
        PS_HLE_FUNC(0x220894E3, cellSysutilEnableBgmPlayback),
        PS_HLE_FUNC(0xCFDD8E87, cellSysutilDisableBgmPlayback),
        PS_HLE_FUNC(0xA11552F6, cellSysutilGetBgmPlaybackStatus),
    });
    r.add("cellSysutilAvconfExt", {
        PS_HLE_FUNC(0xC7020F62, cellVideoOutSetGamma),
        PS_HLE_FUNC(0xC3273D9B, cellAudioOutGetAvailableDeviceInfo),
    });
}

} // namespace pspocket::hle
