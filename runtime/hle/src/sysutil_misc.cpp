// The small sysutil libraries, as RPCS3 has them with no device, service or overlay behind them (D-183): user info,
// screenshots, storage data, system settings, AV chat (cellSysutilAvc), the licence area,
// the NP EULA, the access point, remote play, video upload, printing, the auth dialog, the cross controller and
// cellGameExec. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellUserInfo.cpp, cellScreenshot.cpp, cellStorage.cpp,
// cellSysconf.cpp, cellSysutilAvc.cpp, cellSysutilMisc.cpp, cellSysutilNpEula.cpp,
// cellSysutilAp.cpp, cellRemotePlay.cpp, cellVideoUpload.cpp, cellPrint.cpp, cellAuthDialog.cpp,
// cellCrossController.cpp, cellGameExec.cpp. The functions RPCS3 leaves as stubs there are answered from
// rpcs3_stubs.inc (D-182).
//
// Where the PS3 shows a dialog, there is no UI: the user takes what the cursor starts on, as the message dialog's
// (cellSysutil.cpp). The user list picks the current user (RPCS3's fallback without its overlay), the cross
// controller's "waiting for PS Vita" dialog is cancelled, and the system's callbacks reach the game at its
// cellSysutilCheckCallback.
//
// pspocket's own where RPCS3 has stubs (D-671 to D-673), each unverified (D-594; these modules are always native, so
// the marker only records that no Sony module was compared): background downloads (cellBGDLUtility, RPCS3
// cellBgdl.cpp: none, no PSN), the access point mode's On and Off (never on: no wired LAN to bridge, no wireless access
// point), the auth dialog's Abort and Close (of the dialog cellAuthDialogOpen opened), cellGameExec's game list,
// deletion, execution and exit information (pspocket runs one game: no other is booted or deleted from it), and the
// system's RTC alarms (cellRtcAlarm, RPCS3 cellRtcAlarm.cpp: registrations kept, none ever rings).

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "psf.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

namespace fs = std::filesystem;

constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// A call's log line with its result: info, a warning for an error.
s32 logged(std::string_view chan, s32 r, std::string_view call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, chan, "{}: {}", call,
        r < 0 ? std::format("{:#010x}", static_cast<u32>(r)) : std::to_string(r));
    return r;
}

template <class T>
T& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL("sysutil", "a sysutil library called without an HLE process");
    return p->state<T>();
}

// Queues a guest callback for the game's next cellSysutilCheckCallback (RPCS3 sysutil_register_cb).
void queue_callback(u32 func, std::vector<u64> args)
{
    sysutil_queue_call([func, args = std::move(args)](PPUContext& ctx) -> s32 {
        switch (args.size()) {
        case 1: call_guest(ctx, func, {args[0]}); break;
        case 2: call_guest(ctx, func, {args[0], args[1]}); break;
        case 3: call_guest(ctx, func, {args[0], args[1], args[2]}); break;
        case 4: call_guest(ctx, func, {args[0], args[1], args[2], args[3]}); break;
        default: call_guest(ctx, func, {args[0], args[1], args[2], args[3], args[4]}); break;
        }
        return lv2::CELL_OK;
    });
}

void put_string(u8* base, u32 addr, u32 capacity, std::string_view s)
{
    if (!capacity) return;
    const usize n = std::min<usize>(s.size(), capacity - 1);
    std::memcpy(base + addr, s.data(), n);
    std::memset(base + addr + n, 0, capacity - n);
}

// Whether [addr, addr + size) is committed guest memory, page by page (guest_readable looks at the two ends only, for
// a page at most; libc_guest.hpp's guest_range, which sys_libc keeps to itself).
bool guest_span(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 p = addr & ~(page - 1); p < u64{addr} + size; p += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(p))) return false;
    return true;
}

// Whether a pointer argument can take `size` bytes: non-null and guest memory. One that is not guest memory is logged
// with the call site (D-042: a PS3 would fault).
bool guest_out(PPUContext& ctx, u32 addr, u64 size, std::string_view what)
{
    if (!addr) return false;
    if (guest_span(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// A polled call's log line: its first kPolledInfoCalls at info (a warning for an error), later ones at debug.
constexpr u32 kPolledInfoCalls = 4;
s32 logged_polled(std::atomic<u32>& calls, std::string_view chan, s32 r, std::string_view call)
{
    if (calls.fetch_add(1, std::memory_order_relaxed) < kPolledInfoCalls) return logged(chan, r, call);
    log::print(log::Level::Debug, chan, "{}: {}", call,
        r < 0 ? std::format("{:#010x}", static_cast<u32>(r)) : std::to_string(r));
    return r;
}

// The argument registers of a call whose signature is not known (RPCS3 registers the name only): logged, not read.
std::string argument_registers(std::string_view name, const PPUContext& ctx)
{
    return std::format("{}(r3 {:#x}, r4 {:#x}, r5 {:#x}, r6 {:#x})", name, ctx.gpr[3], ctx.gpr[4], ctx.gpr[5],
        ctx.gpr[6]);
}

// ------------------------------------------------------------------------------------- cellUserInfo

constexpr std::string_view kUserChan = "cellUserInfo";
enum : u32 {
    CELL_USERINFO_ERROR_BUSY = 0x8002c301,
    CELL_USERINFO_ERROR_INTERNAL = 0x8002c302,
    CELL_USERINFO_ERROR_PARAM = 0x8002c303,
    CELL_USERINFO_ERROR_NOUSER = 0x8002c304,
};
constexpr u32 CELL_USERINFO_USER_MAX = 16, CELL_USERINFO_USERNAME_SIZE = 64, CELL_SYSUTIL_USERID_MAX = 99999999;
constexpr u32 kCurrentUser = 1;
constexpr u32 kUserStatSize = 4 + CELL_USERINFO_USERNAME_SIZE; // CellUserInfoUserStat: be32 id; char name[64]

struct UserInfoState {
    std::mutex mutex;
    bool overlay = false;
    bool dialog_open = false;
};

// The host directory of /dev_hdd0/home, or "" when /dev_hdd0 is not mounted on one. The current user (00000001) is
// made with RPCS3's name "User" when it isn't there, as RPCS3 makes it at boot.
std::string home_dir(lv2::Kernel& kernel)
{
    const std::string home = kernel.vfs_host_path("/dev_hdd0/home");
    if (home.empty()) return home;
    std::error_code ec;
    const fs::path current = fs::path(home) / std::format("{:08}", kCurrentUser);
    if (!fs::exists(current / "localusername", ec)) {
        fs::create_directories(current, ec);
        std::ofstream(current / "localusername", std::ios::binary) << "User";
    }
    return home;
}

// RPCS3 check_user: a directory named with exactly 8 digits, not 0, with a localusername file.
std::vector<u32> users(const std::string& home)
{
    std::vector<u32> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(home, ec)) {
        const std::string name = e.path().filename().string();
        if (!e.is_directory(ec) || name.size() != 8 || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        const u32 id = static_cast<u32>(std::stoul(name));
        if (id && fs::is_regular_file(e.path() / "localusername", ec)) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string user_name(const std::string& home, u32 id)
{
    std::ifstream in(fs::path(home) / std::format("{:08}", id) / "localusername", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

s32 cellUserInfoGetStat(PPUContext& ctx, u32 id, u32 stat)
{
    const std::string call = std::format("cellUserInfoGetStat({}, {:#x})", id, stat);
    if (id > CELL_SYSUTIL_USERID_MAX) return logged(kUserChan, error(CELL_USERINFO_ERROR_PARAM), call);
    if (id == 0) id = kCurrentUser;
    lv2::PpuThread& t = thread_of(ctx);
    const std::string home = home_dir(t.kernel);
    std::error_code ec;
    const fs::path dir = fs::path(home) / std::format("{:08}", id);
    if (home.empty() || !fs::is_directory(dir, ec)) return logged(kUserChan, error(CELL_USERINFO_ERROR_NOUSER), call);
    if (!fs::is_regular_file(dir / "localusername", ec)) return logged(kUserChan, error(CELL_USERINFO_ERROR_INTERNAL), call);
    if (stat) {
        if (!guest_readable(t, stat, kUserStatSize)) return logged(kUserChan, error(CELL_USERINFO_ERROR_PARAM), call);
        ps_write32(t.kernel.base(), stat, id);
        put_string(t.kernel.base(), stat + 4, CELL_USERINFO_USERNAME_SIZE, user_name(home, id));
    }
    return logged(kUserChan, lv2::CELL_OK, call);
}

// CellUserInfoUserList: be32 userId[16], 0 past the users.
s32 cellUserInfoGetList(PPUContext& ctx, u32 list_num, u32 list_buf, u32 current_user_id)
{
    const std::string call = std::format("cellUserInfoGetList({:#x}, {:#x}, {:#x})", list_num, list_buf, current_user_id);
    if (!list_num && (list_buf || !current_user_id)) return logged(kUserChan, error(CELL_USERINFO_ERROR_PARAM), call);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const std::string home = home_dir(t.kernel);
    std::vector<u32> ids = home.empty() ? std::vector<u32>{} : users(home);
    if (ids.size() > CELL_USERINFO_USER_MAX) ids.resize(CELL_USERINFO_USER_MAX);
    if (list_num) ps_write32(base, list_num, static_cast<u32>(ids.size()));
    if (list_buf)
        for (u32 i = 0; i < CELL_USERINFO_USER_MAX; i++) ps_write32(base, list_buf + 4 * i, i < ids.size() ? ids[i] : 0);
    if (current_user_id) ps_write32(base, current_user_id, kCurrentUser);
    return logged(kUserChan, lv2::CELL_OK, call + std::format(": {} user(s)", ids.size()));
}

// The user list dialog: no UI, so the current user is picked (RPCS3 without its overlay). funcSelect(result,
// selectedUser, userdata) then gets CELL_USERINFO_RET_OK and the user's CellUserInfoUserStat, at the next check.
s32 select_user(PPUContext& ctx, std::string_view call, u32 func_select, u32 userdata, bool none)
{
    lv2::PpuThread& t = thread_of(ctx);
    UserInfoState& u = state<UserInfoState>();
    {
        std::lock_guard lock(u.mutex);
        if (u.dialog_open) return logged(kUserChan, error(CELL_USERINFO_ERROR_BUSY), call);
    }
    const std::string name = user_name(home_dir(t.kernel), kCurrentUser);
    sysutil_queue_call([func_select, userdata, name, none](PPUContext& c) -> s32 {
        StackVar stat(c, guest_base(c), kUserStatSize);
        if (!none) {
            ps_write32(guest_base(c), stat.addr(), kCurrentUser);
            put_string(guest_base(c), stat.addr() + 4, CELL_USERINFO_USERNAME_SIZE, name);
        }
        call_guest(c, func_select, {none ? static_cast<u64>(static_cast<s64>(error(CELL_USERINFO_ERROR_NOUSER))) : 0,
                                       stat.addr(), userdata});
        return lv2::CELL_OK;
    });
    PS_LOG_WARN(kUserChan, "{}: no user list UI; {}", call, none ? "the list is empty (NOUSER)" : "the current user is picked");
    return logged(kUserChan, lv2::CELL_OK, call);
}

// CellUserInfoTypeSet: be32 title, focus, type, reserved.
s32 cellUserInfoSelectUser_ListType(PPUContext& ctx, u32 list_type, u32 func_select, u32 container, u32 userdata)
{
    const std::string call = std::format("cellUserInfoSelectUser_ListType({:#x}, {:#x}, container {:#x}, {:#x})",
        list_type, func_select, container, userdata);
    if (!list_type || !func_select) return logged(kUserChan, error(CELL_USERINFO_ERROR_PARAM), call);
    return select_user(ctx, call, func_select, userdata, false);
}

// CellUserInfoListSet: be32 title, focus, fixedListNum, fixedList, reserved. No user in the list: NOUSER to funcSelect.
s32 cellUserInfoSelectUser_SetList(PPUContext& ctx, u32 set_list, u32 func_select, u32 container, u32 userdata)
{
    const std::string call = std::format("cellUserInfoSelectUser_SetList({:#x}, {:#x}, container {:#x}, {:#x})",
        set_list, func_select, container, userdata);
    if (!set_list || !func_select) return logged(kUserChan, error(CELL_USERINFO_ERROR_PARAM), call);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, set_list, 20)) return logged(kUserChan, error(CELL_USERINFO_ERROR_PARAM), call);
    u8* base = t.kernel.base();
    const u32 num = ps_read32(base, set_list + 8), list = ps_read32(base, set_list + 12);
    bool any = false;
    for (u32 i = 0; i < CELL_USERINFO_USER_MAX && i < num && list && guest_readable(t, list + 4 * i, 4); i++)
        any = any || ps_read32(base, list + 4 * i) != 0;
    return select_user(ctx, call, func_select, userdata, !any);
}

void cellUserInfoEnableOverlay(PPUContext&, s32 enable)
{
    UserInfoState& u = state<UserInfoState>();
    std::lock_guard lock(u.mutex);
    u.overlay = enable != 0;
    PS_LOG_INFO(kUserChan, "cellUserInfoEnableOverlay({})", enable);
}

// ------------------------------------------------------------------------------------- cellScreenShotUtility

constexpr std::string_view kShotChan = "cellScreenShot";
constexpr u32 CELL_SCREENSHOT_ERROR_PARAM = 0x8002d102;

// RPCS3 screenshot_manager: the titles and overlay a screenshot would carry (the iPhone takes its own).
struct ScreenshotState {
    std::mutex mutex;
    bool enabled = false;
    std::string photo_title, game_title, game_comment, overlay_dir, overlay_file;
    s32 overlay_x = 0, overlay_y = 0;
};

// CellScreenShotSetParam: be32 photo_title, game_title, game_comment, reserved. Each string must end within its
// size (64, 64, 1024).
s32 cellScreenShotSetParameter(PPUContext& ctx, u32 param)
{
    const std::string call = std::format("cellScreenShotSetParameter({:#x})", param);
    lv2::PpuThread& t = thread_of(ctx);
    if (!param || !guest_readable(t, param, 16)) return logged(kShotChan, error(CELL_SCREENSHOT_ERROR_PARAM), call);
    u8* base = t.kernel.base();
    std::string text[3];
    const u32 sizes[3] = {64, 64, 1024};
    for (u32 i = 0; i < 3; i++) {
        const u32 at = ps_read32(base, param + 4 * i);
        if (!at) continue;
        const GuestString s = read_cstr(t, at, sizes[i]);
        if (!s.terminated) return logged(kShotChan, error(CELL_SCREENSHOT_ERROR_PARAM), call);
        text[i] = s.text;
    }
    ScreenshotState& s = state<ScreenshotState>();
    std::lock_guard lock(s.mutex);
    s.photo_title = text[0];
    s.game_title = text[1];
    s.game_comment = text[2];
    return logged(kShotChan, lv2::CELL_OK, call + std::format(" {{\"{}\", \"{}\"}}", text[0], text[1]));
}

// The directory must be on /dev_hdd0, /dev_bdvd, /app_home or /host_root.
s32 cellScreenShotSetOverlayImage(PPUContext& ctx, u32 src_dir, u32 src_file, s32 offset_x, s32 offset_y)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::string dir = read_cstr(t, src_dir, 1024).text, file = read_cstr(t, src_file, 64).text;
    const std::string call = std::format("cellScreenShotSetOverlayImage(\"{}\", \"{}\", {}, {})", dir, file, offset_x, offset_y);
    if (!src_dir || !src_file) return logged(kShotChan, error(CELL_SCREENSHOT_ERROR_PARAM), call);
    if (!dir.starts_with("/dev_hdd0") && !dir.starts_with("/dev_bdvd") && !dir.starts_with("/app_home") &&
        !dir.starts_with("/host_root")) {
        return logged(kShotChan, error(CELL_SCREENSHOT_ERROR_PARAM), call);
    }
    ScreenshotState& s = state<ScreenshotState>();
    std::lock_guard lock(s.mutex);
    s.overlay_dir = dir;
    s.overlay_file = file;
    s.overlay_x = offset_x;
    s.overlay_y = offset_y;
    return logged(kShotChan, lv2::CELL_OK, call);
}

s32 set_screenshots(bool enable)
{
    ScreenshotState& s = state<ScreenshotState>();
    std::lock_guard lock(s.mutex);
    if (std::exchange(s.enabled, enable) != enable)
        PS_LOG_INFO(kShotChan, "screenshots {}", enable ? "enabled" : "disabled");
    return lv2::CELL_OK;
}

s32 cellScreenShotEnable(PPUContext&) { return set_screenshots(true); }
s32 cellScreenShotDisable(PPUContext&) { return set_screenshots(false); }

// ------------------------------------------------------------------------------------- cellSysutil: storage, sysconf

constexpr std::string_view kSysutilChan = "cellSysutil";

// Import and export to and from removable media (cellStorageData*): RPCS3 does nothing and tells funcFinish(result,
// userdata) CELL_OK at the next check.
s32 storage(std::string_view name, u32 version, u32 src, u32 dst, u32 param, u32 func_finish, u32 container,
    u32 userdata)
{
    const std::string call = std::format("{}(version {:#x}, {:#x}, {:#x}, param {:#x}, funcFinish {:#x}, container "
                                         "{:#x}, userdata {:#x})",
        name, version, src, dst, param, func_finish, container, userdata);
    if (func_finish) queue_callback(func_finish, {0, userdata});
    PS_LOG_WARN(kSysutilChan, "{}: nothing is copied (RPCS3); funcFinish hears CELL_OK", name);
    return logged(kSysutilChan, lv2::CELL_OK, call);
}

s32 cellStorageDataImportMove(PPUContext&, u32 version, u32 src, u32 dst, u32 param, u32 func_finish, u32 container,
    u32 userdata)
{
    return storage("cellStorageDataImportMove", version, src, dst, param, func_finish, container, userdata);
}

s32 cellStorageDataImport(PPUContext&, u32 version, u32 src, u32 dst, u32 param, u32 func_finish, u32 container,
    u32 userdata)
{
    return storage("cellStorageDataImport", version, src, dst, param, func_finish, container, userdata);
}

s32 cellStorageDataExport(PPUContext&, u32 version, u32 src, u32 dst, u32 param, u32 func_finish, u32 container,
    u32 userdata)
{
    return storage("cellStorageDataExport", version, src, dst, param, func_finish, container, userdata);
}

// The system settings screen (cellSysconfOpen): none here; func(result, userdata) hears CELL_OK (RPCS3).
s32 cellSysconfOpen(PPUContext&, u32 type, u32 func, u32 userdata, u32 extparam, u32 id)
{
    const std::string call = std::format("cellSysconfOpen(type {}, func {:#x}, userdata {:#x}, extparam {:#x}, id {})",
        type, func, userdata, extparam, id);
    if (func) queue_callback(func, {0, userdata});
    return logged(kSysutilChan, lv2::CELL_OK, call);
}

// cellSysconfExtUtility: the paired Bluetooth devices (none: nothing written).
s32 cellSysconfBtGetDeviceList(PPUContext&, u32 device_list)
{
    const std::string call = std::format("cellSysconfBtGetDeviceList({:#x})", device_list);
    if (!device_list) return logged("cellSysconf", error(0x8002bb01), call); // CELL_SYSCONF_ERROR_PARAM
    return logged("cellSysconf", lv2::CELL_OK, call);
}

// ------------------------------------------------------------------------------------- cellSysutil: AV chat

// RPCS3 avc_settings: one loaded AV chat, its callback(requestId, eventId, eventParam, userdata) told each request
// succeeded at the next check (load, join, bye, unload); there are no other players.
enum : u32 {
    CELL_AVC_ERROR_ALREADY_INITIALIZED = 0x8002b704,
    CELL_AVC_ERROR_INVALID_ARGUMENT = 0x8002b705,
};
constexpr u32 CELL_AVC_EVENT_LOAD_SUCCEEDED = 1, CELL_AVC_EVENT_UNLOAD_SUCCEEDED = 3, CELL_AVC_EVENT_JOIN_SUCCEEDED = 5,
              CELL_AVC_EVENT_BYE_SUCCEEDED = 7;

struct AvcState {
    std::mutex mutex;
    u32 callback = 0, userdata = 0;
    u32 next_request = 0;
    u32 media = 1; // the media of the last load (libsysutil's starts as voice, 1); its panel and video need video (2)
};

// A request: its ID written, the event to the callback at the next check (RPCS3 register_cb_call); the unload's event
// drops the callback first.
s32 avc_request(PPUContext& ctx, std::string_view call, u32 request_id, u32 event)
{
    if (!request_id) return logged(kSysutilChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call);
    AvcState& a = state<AvcState>();
    u32 id = 0;
    {
        std::lock_guard lock(a.mutex);
        id = a.next_request++;
    }
    ps_write32(guest_base(ctx), request_id, id);
    sysutil_queue_call([id, event](PPUContext& c) -> s32 {
        AvcState& s = state<AvcState>();
        u32 cb = 0, arg = 0;
        {
            std::lock_guard lock(s.mutex);
            cb = s.callback;
            arg = s.userdata;
            if (event == CELL_AVC_EVENT_UNLOAD_SUCCEEDED) s.callback = s.userdata = 0;
        }
        if (cb) call_guest(c, cb, {id, event, 0, arg});
        return lv2::CELL_OK;
    });
    return logged(kSysutilChan, lv2::CELL_OK, std::string(call) + std::format(": request {}", id));
}

// cellSysutilAvcLoadAsync(func, userdata, container, media, videoQuality, voiceQuality, request_id).
s32 cellSysutilAvcLoadAsync(PPUContext& ctx, u32 func, u32 userdata, u32 container, u32 media, u32 video_quality,
    u32 voice_quality, u32 request_id)
{
    const std::string call = std::format("cellSysutilAvcLoadAsync(func {:#x}, userdata {:#x}, container {:#x}, media {}, "
                                         "quality {}/{}, {:#x})",
        func, userdata, container, media, video_quality, voice_quality, request_id);
    if ((media != 1 && media != 2) || !func || !request_id || video_quality != 1 || voice_quality != 1)
        return logged(kSysutilChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call);
    AvcState& a = state<AvcState>();
    {
        std::lock_guard lock(a.mutex);
        if (a.callback) return logged(kSysutilChan, error(CELL_AVC_ERROR_ALREADY_INITIALIZED), call);
        a.callback = func;
        a.userdata = userdata;
        a.media = media;
    }
    return avc_request(ctx, call, request_id, CELL_AVC_EVENT_LOAD_SUCCEEDED);
}

s32 cellSysutilAvcUnloadAsync(PPUContext& ctx, u32 request_id)
{
    return avc_request(ctx, std::format("cellSysutilAvcUnloadAsync({:#x})", request_id), request_id,
        CELL_AVC_EVENT_UNLOAD_SUCCEEDED);
}

s32 cellSysutilAvcJoinRequest(PPUContext& ctx, u32 ctx_id, u32 room_id, u32 request_id)
{
    const std::string call = std::format("cellSysutilAvcJoinRequest({:#x}, {:#x}, {:#x})", ctx_id, room_id, request_id);
    if (!room_id) return logged(kSysutilChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call);
    return avc_request(ctx, call, request_id, CELL_AVC_EVENT_JOIN_SUCCEEDED);
}

s32 cellSysutilAvcByeRequest(PPUContext& ctx, u32 request_id)
{
    return avc_request(ctx, std::format("cellSysutilAvcByeRequest({:#x})", request_id), request_id,
        CELL_AVC_EVENT_BYE_SUCCEEDED);
}

// The rest checks its pointer (or value) and answers CELL_OK, writing nothing but the player count (0), as RPCS3.
s32 avc_checked(std::string_view name, u32 ptr)
{
    return logged(kSysutilChan, ptr ? lv2::CELL_OK : error(CELL_AVC_ERROR_INVALID_ARGUMENT), std::format("{}({:#x})", name, ptr));
}

s32 cellSysutilAvcCancelByeRequest(PPUContext&, u32 r) { return avc_checked("cellSysutilAvcCancelByeRequest", r); }
s32 cellSysutilAvcCancelJoinRequest(PPUContext&, u32 r) { return avc_checked("cellSysutilAvcCancelJoinRequest", r); }
s32 cellSysutilAvcGetLayoutMode(PPUContext&, u32 p) { return avc_checked("cellSysutilAvcGetLayoutMode", p); }
s32 cellSysutilAvcGetShowStatus(PPUContext&, u32 p) { return avc_checked("cellSysutilAvcGetShowStatus", p); }
s32 cellSysutilAvcGetSpeakerVolumeLevel(PPUContext&, u32 p)
{
    return avc_checked("cellSysutilAvcGetSpeakerVolumeLevel", p);
}
s32 cellSysutilAvcGetVideoMuting(PPUContext&, u32 p) { return avc_checked("cellSysutilAvcGetVideoMuting", p); }
s32 cellSysutilAvcGetVoiceMuting(PPUContext&, u32 p) { return avc_checked("cellSysutilAvcGetVoiceMuting", p); }
s32 cellSysutilAvcGetAttribute(PPUContext&, u32 attr, u32 p)
{
    return avc_checked(std::format("cellSysutilAvcGetAttribute(attr {:#x}, ...)", attr), p);
}
s32 cellSysutilAvcSetAttribute(PPUContext&, u32 attr, u32 p)
{
    return avc_checked(std::format("cellSysutilAvcSetAttribute(attr {:#x}, ...)", attr), p);
}

s32 cellSysutilAvcEnumPlayers(PPUContext& ctx, u32 players_id, u32 players_num)
{
    const std::string call = std::format("cellSysutilAvcEnumPlayers({:#x}, {:#x})", players_id, players_num);
    if (!players_num) return logged(kSysutilChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call);
    if (!players_id) ps_write32(guest_base(ctx), players_num, 0);
    return logged(kSysutilChan, lv2::CELL_OK, call);
}

s32 cellSysutilAvcSetLayoutMode(PPUContext&, u32 layout)
{
    const std::string call = std::format("cellSysutilAvcSetLayoutMode({})", layout);
    return logged(kSysutilChan, layout > 3 ? error(CELL_AVC_ERROR_INVALID_ARGUMENT) : lv2::CELL_OK, call);
}

s32 cellSysutilAvcSetSpeakerVolumeLevel(PPUContext&, s32 level)
{
    const std::string call = std::format("cellSysutilAvcSetSpeakerVolumeLevel({})", level);
    return logged(kSysutilChan, level < 0 || level > 10 ? error(CELL_AVC_ERROR_INVALID_ARGUMENT) : lv2::CELL_OK, call);
}

// ------------------------------------------------------------------------------------- cellSysutilAvcExt

// RPCS3 cellSysutilAvcExt.cpp: the AV chat's windows, panel and name plates. Nothing is shown, so the getters check
// their pointers and write nothing (RPCS3's TODOs), the setters check the player and the transition (at most
// EXPONENT, so NONE is refused too, as RPCS3) or the z-order; cellSysutilAvcExtLoadAsyncEx checks its option and loads
// through cellSysutilAvcLoadAsync. Float arguments come in f1.. and take no general register (RPCS3's binder). RPCS3's
// stubs here are answered from rpcs3_stubs.inc.
constexpr std::string_view kAvcExtChan = "cellSysutilAvcExt";
constexpr u32 CELL_AVC_ERROR_UNKNOWN = 0x8002b701;
constexpr u32 CELL_SYSUTIL_AVC_TRANSITION_EXPONENT = 4, CELL_SYSUTIL_AVC_ZORDER_FORWARD_MOST = 2,
              CELL_SYSUTIL_AVC_ZORDER_BEHIND_MOST = 3, CELL_SYSUTIL_AVC_VOICE_CHAT = 1;
constexpr s32 CELL_SYSUTIL_AVC_OPTION_PARAM_VERSION = 100, kAvcOptionParamVersion180 = 180;
// CellSysutilAvcOptionParam (12 bytes): be s32 avcOptionParamVersion, b8 sharingVideoBuffer at 4, be s32 maxPlayers
// at 8.
constexpr u32 kAvcOptionSize = 12;

// The getters: every pointer non-null, nothing written.
s32 avc_ext_checked(std::string_view name, std::initializer_list<u32> ptrs)
{
    std::string call = std::string(name) + "(";
    bool ok = true;
    for (const u32 p : ptrs) {
        call += std::format("{}{:#x}", call.back() == '(' ? "" : ", ", p);
        ok = ok && p;
    }
    return logged(kAvcExtChan, ok ? lv2::CELL_OK : error(CELL_AVC_ERROR_INVALID_ARGUMENT), call + ")");
}

// The window setters: a player and a transition up to EXPONENT.
s32 avc_ext_window(std::string_view name, u32 player_id, u32 transition, const std::string& detail = {})
{
    const bool ok = player_id && transition <= CELL_SYSUTIL_AVC_TRANSITION_EXPONENT;
    return logged(kAvcExtChan, ok ? lv2::CELL_OK : error(CELL_AVC_ERROR_INVALID_ARGUMENT),
        std::format("{}(player {:#x}{}, transition {:#x})", name, player_id, detail, transition));
}

// RPCS3 ensure()s the pointer ("Not actually checked") and writes nothing: here a null one is refused.
s32 avc_ext_attached(PPUContext& ctx, std::string_view name, u32 status)
{
    if (!status) log_call_site(ctx, guest_base(ctx), std::format("{}: a null status (RPCS3 aborts)", name));
    return logged(kAvcExtChan, status ? lv2::CELL_OK : error(CELL_AVC_ERROR_INVALID_ARGUMENT),
        std::format("{}({:#x}): nothing written", name, status));
}

s32 cellSysutilAvcExtIsMicAttached(PPUContext& ctx, u32 status)
{
    return avc_ext_attached(ctx, "cellSysutilAvcExtIsMicAttached", status);
}

s32 cellSysutilAvcExtIsCameraAttached(PPUContext& ctx, u32 status)
{
    return avc_ext_attached(ctx, "cellSysutilAvcExtIsCameraAttached", status);
}

s32 cellSysutilAvcExtGetWindowPosition(PPUContext&, u32 player_id, u32 x, u32 y, u32 z)
{
    return avc_ext_checked("cellSysutilAvcExtGetWindowPosition", {player_id, x, y, z});
}

s32 cellSysutilAvcExtGetWindowSize(PPUContext&, u32 player_id, u32 x, u32 y)
{
    return avc_ext_checked("cellSysutilAvcExtGetWindowSize", {player_id, x, y});
}

s32 cellSysutilAvcExtGetWindowShowStatus(PPUContext&, u32 player_id, u32 visible)
{
    return avc_ext_checked("cellSysutilAvcExtGetWindowShowStatus", {player_id, visible});
}

s32 cellSysutilAvcExtGetNamePlateShowStatus(PPUContext&, u32 visible)
{
    return avc_ext_checked("cellSysutilAvcExtGetNamePlateShowStatus", {visible});
}

s32 cellSysutilAvcExtGetWindowRotation(PPUContext&, u32 player_id, u32 x, u32 y, u32 z)
{
    return avc_ext_checked("cellSysutilAvcExtGetWindowRotation", {player_id, x, y, z});
}

s32 cellSysutilAvcExtGetWindowAlpha(PPUContext&, u32 player_id, u32 alpha)
{
    return avc_ext_checked("cellSysutilAvcExtGetWindowAlpha", {player_id, alpha});
}

s32 cellSysutilAvcExtGetSurfacePointer(PPUContext&, u32 player_id, u32 surface, u32 size, u32 size_x, u32 size_y)
{
    return avc_ext_checked("cellSysutilAvcExtGetSurfacePointer", {player_id, surface, size, size_x, size_y});
}

// (player_id, float alpha in f1, transition).
s32 cellSysutilAvcExtSetWindowAlpha(PPUContext& ctx, u32 player_id, u32 transition)
{
    return avc_ext_window("cellSysutilAvcExtSetWindowAlpha", player_id, transition,
        std::format(", alpha {}", ctx.fpr[1]));
}

// (player_id, float size_x in f1, float size_y in f2, transition).
s32 cellSysutilAvcExtSetWindowSize(PPUContext& ctx, u32 player_id, u32 transition)
{
    return avc_ext_window("cellSysutilAvcExtSetWindowSize", player_id, transition,
        std::format(", {}x{}", ctx.fpr[1], ctx.fpr[2]));
}

s32 cellSysutilAvcExtShowWindow(PPUContext&, u32 player_id, u32 transition)
{
    return avc_ext_window("cellSysutilAvcExtShowWindow", player_id, transition);
}

s32 cellSysutilAvcExtHideWindow(PPUContext&, u32 player_id, u32 transition)
{
    return avc_ext_window("cellSysutilAvcExtHideWindow", player_id, transition);
}

s32 cellSysutilAvcExtSetWindowZorder(PPUContext&, u32 player_id, u32 zorder)
{
    const bool ok = player_id && zorder >= CELL_SYSUTIL_AVC_ZORDER_FORWARD_MOST &&
                    zorder <= CELL_SYSUTIL_AVC_ZORDER_BEHIND_MOST;
    return logged(kAvcExtChan, ok ? lv2::CELL_OK : error(CELL_AVC_ERROR_INVALID_ARGUMENT),
        std::format("cellSysutilAvcExtSetWindowZorder(player {:#x}, {})", player_id, zorder));
}

// RPCS3 cellSysutilAvcExtInitOptionParam(version, option): the version written, then 100, or 180 with 16 players;
// another version is ERROR_UNKNOWN. The video buffer is not shared.
s32 cellSysutilAvcExtInitOptionParam(PPUContext& ctx, s32 version, u32 option)
{
    const std::string call = std::format("cellSysutilAvcExtInitOptionParam({}, {:#x})", version, option);
    if (!option || !guest_readable(thread_of(ctx), option, kAvcOptionSize)) {
        if (option) log_call_site(ctx, guest_base(ctx), call + ": option is not guest memory");
        return logged(kAvcExtChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call);
    }
    u8* base = guest_base(ctx);
    ps_write32(base, option, static_cast<u32>(version));
    if (version == kAvcOptionParamVersion180) ps_write32(base, option + 8, 16);
    else if (version != CELL_SYSUTIL_AVC_OPTION_PARAM_VERSION) {
        return logged(kAvcExtChan, error(CELL_AVC_ERROR_UNKNOWN), call);
    }
    ps_write8(base, option + 4, 0);
    return logged(kAvcExtChan, lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvcExtLoadAsyncEx(func, userdata, container, media, videoQuality, voiceQuality, option,
// request_id): an option of version 100 or 180 (a shared video buffer only for a video chat), then
// cellSysutilAvcLoadAsync.
s32 cellSysutilAvcExtLoadAsyncEx(PPUContext& ctx, u32 func, u32 userdata, u32 container, u32 media, u32 video_quality,
    u32 voice_quality, u32 option, u32 request_id)
{
    const std::string call = std::format("cellSysutilAvcExtLoadAsyncEx(func {:#x}, media {}, option {:#x})", func,
        media, option);
    if (!option || !guest_readable(thread_of(ctx), option, kAvcOptionSize)) {
        if (option) log_call_site(ctx, guest_base(ctx), call + ": option is not guest memory");
        return logged(kAvcExtChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call);
    }
    u8* base = guest_base(ctx);
    const s32 version = static_cast<s32>(ps_read32(base, option));
    if (version != CELL_SYSUTIL_AVC_OPTION_PARAM_VERSION && version != kAvcOptionParamVersion180) {
        return logged(kAvcExtChan, error(CELL_AVC_ERROR_UNKNOWN), call);
    }
    if (ps_read8(base, option + 4) && media == CELL_SYSUTIL_AVC_VOICE_CHAT) {
        return logged(kAvcExtChan, error(CELL_AVC_ERROR_INVALID_ARGUMENT), call + ": a shared video buffer");
    }
    return cellSysutilAvcLoadAsync(ctx, func, userdata, container, media, video_quality, voice_quality, request_id);
}

// ------------------------------------------------------------------------------------- cellSubDisplay

// RPCS3 cellSubDisplay.cpp: a PSP or PS Vita as a second screen (remote play). Its init keeps the parameters and
// handler, then answers CELL_SUBDISPLAY_ERROR_ZERO_REGISTERED (no device registered) without starting anything, so
// every call that needs it started answers NOT_INITIALIZED, as in RPCS3; GetRequiredMemory gives the container size
// for the parameters' version; touch information, for version 3, is empty.
constexpr std::string_view kSubDisplayChan = "cellSubDisplay";
enum : u32 {
    CELL_SUBDISPLAY_ERROR_INVALID_VALUE = 0x80029854,
    CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED = 0x80029855,
    CELL_SUBDISPLAY_ERROR_NOT_SUPPORTED = 0x80029856,
    CELL_SUBDISPLAY_ERROR_SET_SAMPLE = 0x80029860,
    CELL_SUBDISPLAY_ERROR_ZERO_REGISTERED = 0x80029813,
};
// CellSubDisplayParam (48 bytes): be s32 version, mode, nGroup, nPeer; video: format at 16, width, height, pitch,
// aspectRatio, videoMode; audio: ch at 40, audioMode at 44. CellSubDisplayTouchInfo: 6 bytes.
constexpr u32 kSubDisplayParamSize = 48, kTouchInfoSize = 6, kMaxTouchInfo = 6;

enum class SubDisplayStatus : u8 { Uninitialized, Stopped, Started };

struct SubDisplayState {
    std::mutex mutex;
    SubDisplayStatus status = SubDisplayStatus::Uninitialized; // RPCS3's: init never changes it
    s32 version = 0;
    u32 handler = 0, userdata = 0;
};

// RPCS3 check_param.
s32 subdisplay_check_param(PPUContext& ctx, u32 param)
{
    if (!param) return error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE);
    if (!guest_readable(thread_of(ctx), param, kSubDisplayParamSize)) {
        log_call_site(ctx, guest_base(ctx), std::format("cellSubDisplay: param {:#x} is not guest memory", param));
        return error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE);
    }
    u8* b = guest_base(ctx);
    const auto at = [&](u32 off) { return static_cast<s32>(ps_read32(b, param + off)); };
    const s32 version = at(0);
    if (version < 1 || version > 3 || at(4) != 1 || at(8) != 1 || at(12) != 1 || at(40) != 2 || at(44) < 0 ||
        at(44) > 1) {
        return error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE);
    }
    switch (at(16)) {
    case 1: // A8R8G8B8
    case 2: // R8G8B8A8
        if (version == 3 || at(20) != 480 || at(24) != 272 || (at(28) != 1920 && at(28) != 2048) || at(32) < 0 ||
            at(32) > 1 || at(36) < 0 || at(36) > 1) {
            return error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE);
        }
        return lv2::CELL_OK;
    case 3: // YUV420
        if (version != 3 || at(20) != 864 || at(24) != 480 || at(28) != 864 || at(32) != 0 || at(36) != 0) {
            return error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE);
        }
        return lv2::CELL_OK;
    default: return error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE);
    }
}

s32 cellSubDisplayInit(PPUContext& ctx, u32 param, u32 handler, u32 userdata, u32 container)
{
    const std::string call =
        std::format("cellSubDisplayInit({:#x}, {:#x}, {:#x}, {:#x})", param, handler, userdata, container);
    SubDisplayState& s = state<SubDisplayState>();
    std::lock_guard lock(s.mutex);
    if (s.handler) return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED), call);
    if (const s32 r = subdisplay_check_param(ctx, param)) return logged(kSubDisplayChan, r, call);
    if (!handler) return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE), call);
    s.version = static_cast<s32>(ps_read32(guest_base(ctx), param));
    s.handler = handler;
    s.userdata = userdata;
    return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_ZERO_REGISTERED), call + ": no device registered");
}

s32 cellSubDisplayEnd(PPUContext&)
{
    SubDisplayState& s = state<SubDisplayState>();
    std::lock_guard lock(s.mutex);
    if (!s.handler) return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED), "cellSubDisplayEnd()");
    s.version = 0;
    s.handler = s.userdata = 0;
    s.status = SubDisplayStatus::Uninitialized;
    return logged(kSubDisplayChan, lv2::CELL_OK, "cellSubDisplayEnd()");
}

// RPCS3 cellSubDisplayGetRequiredMemory: 8 MB for version 1, 10 MB for 2 and 3, returned as a value.
s32 cellSubDisplayGetRequiredMemory(PPUContext& ctx, u32 param)
{
    const std::string call = std::format("cellSubDisplayGetRequiredMemory({:#x})", param);
    if (const s32 r = subdisplay_check_param(ctx, param)) return logged(kSubDisplayChan, r, call);
    const s32 version = static_cast<s32>(ps_read32(guest_base(ctx), param));
    return logged(kSubDisplayChan, version == 1 ? 8 * 1024 * 1024 : 10 * 1024 * 1024, call);
}

// The calls that need the sub display set up: never, as init leaves it uninitialized (RPCS3).
s32 subdisplay_status(const std::string& call)
{
    SubDisplayState& s = state<SubDisplayState>();
    std::lock_guard lock(s.mutex);
    return logged(kSubDisplayChan,
        s.status == SubDisplayStatus::Uninitialized ? error(CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED) : lv2::CELL_OK, call);
}

s32 cellSubDisplayStart(PPUContext&) { return subdisplay_status("cellSubDisplayStart()"); }
s32 cellSubDisplayStop(PPUContext&) { return subdisplay_status("cellSubDisplayStop()"); }

s32 cellSubDisplayGetVideoBuffer(PPUContext&, s32 group, u32 buf, u32 size)
{
    return subdisplay_status(std::format("cellSubDisplayGetVideoBuffer({}, {:#x}, {:#x})", group, buf, size));
}

s32 cellSubDisplayAudioOutBlocking(PPUContext&, s32 group, u32 data, s32 samples)
{
    return subdisplay_status(std::format("cellSubDisplayAudioOutBlocking({}, {:#x}, {})", group, data, samples));
}

s32 cellSubDisplayAudioOutNonBlocking(PPUContext&, s32 group, u32 data, s32 samples)
{
    return subdisplay_status(std::format("cellSubDisplayAudioOutNonBlocking({}, {:#x}, {})", group, data, samples));
}

s32 cellSubDisplayGetPeerNum(PPUContext&, s32 group)
{
    return subdisplay_status(std::format("cellSubDisplayGetPeerNum({})", group));
}

s32 cellSubDisplayGetPeerList(PPUContext&, s32 group, u32 info, u32 num)
{
    return subdisplay_status(std::format("cellSubDisplayGetPeerList({}, {:#x}, {:#x})", group, info, num));
}

// RPCS3 cellSubDisplayGetTouchInfo(groupId, pTouchInfo, pNumTouchInfo): version 3 only; at most six, all empty.
s32 cellSubDisplayGetTouchInfo(PPUContext& ctx, s32 group, u32 info, u32 num)
{
    const std::string call = std::format("cellSubDisplayGetTouchInfo({}, {:#x}, {:#x})", group, info, num);
    if (group != 0 || !num || !info || !guest_readable(thread_of(ctx), num, 4)) {
        return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE), call);
    }
    SubDisplayState& s = state<SubDisplayState>();
    std::lock_guard lock(s.mutex);
    if (s.version != 3) return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_NOT_SUPPORTED), call);
    u8* b = guest_base(ctx);
    s32 n = static_cast<s32>(ps_read32(b, num));
    if (n > static_cast<s32>(kMaxTouchInfo)) {
        n = kMaxTouchInfo;
        ps_write32(b, num, kMaxTouchInfo);
    }
    if (n > 0) {
        if (!guest_readable(thread_of(ctx), info, kTouchInfoSize * static_cast<u32>(n))) {
            return logged(kSubDisplayChan, error(CELL_SUBDISPLAY_ERROR_INVALID_VALUE), call);
        }
        std::memset(b + info, 0, kTouchInfoSize * static_cast<u32>(n));
    }
    return logged(kSubDisplayChan, lv2::CELL_OK, call);
}

// ------------------------------------------------------------------------------------- small libraries

// cellSysutilMisc: the console's licence area, RPCS3's default (CELL_SYSUTIL_LICENSE_AREA_A).
s32 cellSysutilGetLicenseArea(PPUContext&) { return logged("cellSysutilMisc", 1, "cellSysutilGetLicenseArea()"); }

// cellSysutilNpEula: the EULA counts as accepted (RPCS3), the status told to cbFunc(status, errorCode, version, arg) at
// the next check; one check or display at a time.
enum : u32 {
    SCE_NP_EULA_ERROR_INVALID_ARGUMENT = 0x8002e501,
    SCE_NP_EULA_ERROR_NOT_INITIALIZED = 0x8002e502,
    SCE_NP_EULA_ERROR_ALREADY_INITIALIZED = 0x8002e503,
};
constexpr u32 SCE_NP_EULA_ALREADY_ACCEPTED = 2, SCE_NP_EULA_ABORTED = 4;

struct EulaState {
    std::mutex mutex;
    bool checking = false;
    u32 status = 0;
};

s32 sceNpEulaCheckEulaStatus(PPUContext&, u32 communication_id, u32 arg2, u64 arg3, u32 cb_func, u32 cb_arg)
{
    const std::string call = std::format("sceNpEulaCheckEulaStatus({:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
        communication_id, arg2, arg3, cb_func, cb_arg);
    if (!communication_id || !cb_func) return logged("cellSysutilNpEula", error(SCE_NP_EULA_ERROR_INVALID_ARGUMENT), call);
    EulaState& e = state<EulaState>();
    {
        std::lock_guard lock(e.mutex);
        if (e.checking) return logged("cellSysutilNpEula", error(SCE_NP_EULA_ERROR_ALREADY_INITIALIZED), call);
        e.checking = true;
        e.status = SCE_NP_EULA_ALREADY_ACCEPTED;
    }
    sysutil_queue_call([cb_func, cb_arg](PPUContext& c) -> s32 {
        EulaState& s = state<EulaState>();
        u32 status = 0;
        {
            std::lock_guard lock(s.mutex);
            status = s.status;
        }
        call_guest(c, cb_func, {status, 0, 1, cb_arg});
        std::lock_guard lock(s.mutex);
        s.checking = false;
        return lv2::CELL_OK;
    });
    return logged("cellSysutilNpEula", lv2::CELL_OK, call);
}

s32 sceNpEulaAbort(PPUContext&)
{
    EulaState& e = state<EulaState>();
    std::lock_guard lock(e.mutex);
    if (!e.checking) return logged("cellSysutilNpEula", error(SCE_NP_EULA_ERROR_NOT_INITIALIZED), "sceNpEulaAbort()");
    e.status = SCE_NP_EULA_ABORTED;
    return logged("cellSysutilNpEula", lv2::CELL_OK, "sceNpEulaAbort()");
}

// RPCS3 checks the arguments and calls nothing (the EULA screen's callback is unknown).
s32 sceNpEulaShowCurrentEula(PPUContext&, u32 communication_id, u64 arg2, u32 cb_func, u32 cb_arg)
{
    const std::string call = std::format("sceNpEulaShowCurrentEula({:#x}, {:#x}, {:#x}, {:#x})", communication_id, arg2,
        cb_func, cb_arg);
    if (!communication_id || !cb_func) return logged("cellSysutilNpEula", error(SCE_NP_EULA_ERROR_INVALID_ARGUMENT), call);
    EulaState& e = state<EulaState>();
    std::lock_guard lock(e.mutex);
    if (e.checking) return logged("cellSysutilNpEula", error(SCE_NP_EULA_ERROR_ALREADY_INITIALIZED), call);
    return logged("cellSysutilNpEula", lv2::CELL_OK, call);
}

// cellSysutilAp: the PS3 as a Wi-Fi access point for a PSP (remote play): 1 MB (RPCS3).
s32 cellSysutilApGetRequiredMemSize(PPUContext&)
{
    return logged("cellSysutilAp", 1024 * 1024, "cellSysutilApGetRequiredMemSize()");
}

// The access point mode (D-671; RPCS3's On and Off are todos answering CELL_OK). It makes the console's wireless LAN an
// access point bridged to its wired LAN, which it needs connected. pspocket's console has neither (an iPhone has no
// Ethernet and cannot be made an access point), so the mode never comes on: On answers NETIF_NO_CABLE once its
// parameters are checked, and Off, asked for a mode that is off, has nothing to do.
enum : u32 {
    CELL_SYSUTIL_AP_ERROR_INVALID_VALUE = 0x8002cd02,
    CELL_SYSUTIL_AP_ERROR_NETIF_NO_CABLE = 0x8002cd15,
};
// CellSysutilApParam (RPCS3 cellSysutilAp.cpp, 124 bytes): be s32 type, wlanFlag; char titleId[9] + 3 at 8; char
// ssid[33] + 3 at 20; char wpakey[65] + 3 at 56 (never logged).
constexpr u32 kApParamSize = 124, kApTitleIdAt = 8, kApTitleIdLen = 9, kApSsidAt = 20, kApSsidLen = 32;

// A fixed-size char field of a guest structure (checked already), up to its NUL.
std::string fixed_string(u8* base, u32 addr, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// cellSysutilApOn(const CellSysutilApParam* pParam, sys_memory_container_t container).
s32 cellSysutilApOn(PPUContext& ctx, u32 param, u32 container)
{
    std::string call = std::format("cellSysutilApOn({:#x}, container {:#x})", param, container);
    if (!guest_out(ctx, param, kApParamSize, "cellSysutilApOn: pParam"))
        return logged("cellSysutilAp", error(CELL_SYSUTIL_AP_ERROR_INVALID_VALUE), call);
    u8* base = guest_base(ctx);
    call += std::format(" {{type {}, wlanFlag {}, titleId \"{}\", ssid \"{}\"}}",
        static_cast<s32>(ps_read32(base, param)), static_cast<s32>(ps_read32(base, param + 4)),
        fixed_string(base, param + kApTitleIdAt, kApTitleIdLen), fixed_string(base, param + kApSsidAt, kApSsidLen));
    PS_LOG_WARN("cellSysutilAp", "cellSysutilApOn: the access point mode needs a wired LAN to bridge and a wireless "
                                 "LAN to be the access point; this console has neither");
    return logged("cellSysutilAp", error(CELL_SYSUTIL_AP_ERROR_NETIF_NO_CABLE), call);
}

s32 cellSysutilApOff(PPUContext&)
{
    return logged("cellSysutilAp", lv2::CELL_OK, "cellSysutilApOff(): the access point mode is off");
}

// cellVideoUpload: the callback(status, errorCode, resultURL, userdata) hears INITIALIZED (1) then FINALIZED (2), both
// CELL_OK with an empty URL, at the next check (RPCS3); nothing is uploaded.
s32 cellVideoUploadInitialize(PPUContext&, u32 param, u32 cb, u32 userdata)
{
    const std::string call = std::format("cellVideoUploadInitialize({:#x}, {:#x}, {:#x})", param, cb, userdata);
    if (cb) {
        sysutil_queue_call([cb, userdata](PPUContext& c) -> s32 {
            StackVar url(c, guest_base(c), 128);
            call_guest(c, cb, {1, 0, url.addr(), userdata});
            call_guest(c, cb, {2, 0, url.addr(), userdata});
            return lv2::CELL_OK;
        });
    }
    return logged("cellVideoUpload", lv2::CELL_OK, call);
}

// cellPrintUtility: no printer; each step tells function(result, userdata) CELL_OK at the next check (RPCS3).
s32 print_step(std::string_view call, u32 function, u32 userdata)
{
    if (function) queue_callback(function, {0, userdata});
    return logged("cellPrint", lv2::CELL_OK, call);
}

s32 cellPrintLoadAsync(PPUContext&, u32 function, u32 userdata, u32 param, u32 container)
{
    return print_step(std::format("cellPrintLoadAsync({:#x}, {:#x}, {:#x}, {:#x})", function, userdata, param, container),
        function, userdata);
}

s32 cellPrintLoadAsync2(PPUContext&, u32 function, u32 userdata, u32 param)
{
    return print_step(std::format("cellPrintLoadAsync2({:#x}, {:#x}, {:#x})", function, userdata, param), function,
        userdata);
}

s32 cellPrintOpenConfig(PPUContext&, u32 function, u32 userdata)
{
    return print_step(std::format("cellPrintOpenConfig({:#x}, {:#x})", function, userdata), function, userdata);
}

s32 cellPrintUnloadAsync(PPUContext&, u32 function, u32 userdata)
{
    return print_step(std::format("cellPrintUnloadAsync({:#x}, {:#x})", function, userdata), function, userdata);
}

// cellAuthDialogUtility: a zero argument is 0x8002d202 (RPCS3's CELL_AUTHDIALOG_ARG1_IS_ZERO).
//
// D-672: the dialog cellAuthDialogOpen opens stays open (there is no UI to answer it) until cellAuthDialogClose.
// Abort and Close of a dialog that is not open answer 0x8002d203: RPCS3's reading of libsysutil_authdialog
// (cellAuthDialog.cpp: "If it fails the first if condition (not init cond?) return CELL_AUTHDIALOG_UNKNOWN_203").
// Abort ends the dialog's interaction and leaves it to Close, as the other sysutil dialogs' aborts leave theirs to
// their unload; Close's argument (RPCS3: "arg1", not known) is logged, not read.
enum : u32 {
    CELL_AUTHDIALOG_ARG1_IS_ZERO = 0x8002d202,
    CELL_AUTHDIALOG_UNKNOWN_203 = 0x8002d203,
};

struct AuthDialogState {
    std::mutex mutex;
    bool open = false;
    bool aborted = false;
};

s32 cellAuthDialogOpen(PPUContext&, u64 arg1)
{
    const std::string call = std::format("cellAuthDialogOpen({:#x})", arg1);
    if (arg1 == 0) return logged("cellAuthDialog", error(CELL_AUTHDIALOG_ARG1_IS_ZERO), call);
    AuthDialogState& d = state<AuthDialogState>();
    std::lock_guard lock(d.mutex);
    d.open = true;
    d.aborted = false;
    return logged("cellAuthDialog", lv2::CELL_OK, call);
}

s32 cellAuthDialogAbort(PPUContext&)
{
    AuthDialogState& d = state<AuthDialogState>();
    std::lock_guard lock(d.mutex);
    if (!d.open)
        return logged("cellAuthDialog", error(CELL_AUTHDIALOG_UNKNOWN_203), "cellAuthDialogAbort(): no dialog");
    d.aborted = true;
    return logged("cellAuthDialog", lv2::CELL_OK, "cellAuthDialogAbort(): aborted, open until cellAuthDialogClose");
}

s32 cellAuthDialogClose(PPUContext& ctx)
{
    const std::string call = std::format("cellAuthDialogClose(r3 {:#x})", ctx.gpr[3]);
    AuthDialogState& d = state<AuthDialogState>();
    std::lock_guard lock(d.mutex);
    if (!d.open) return logged("cellAuthDialog", error(CELL_AUTHDIALOG_UNKNOWN_203), call + ": no dialog");
    const bool aborted = std::exchange(d.aborted, false);
    d.open = false;
    return logged("cellAuthDialog", lv2::CELL_OK, call + (aborted ? ": closed after an abort" : ": closed"));
}

// cellCrossController (LittleBigPlanet 2 and 3 talk to a PS Vita with it): RPCS3's checks, then the callback(status,
// errorCode, option, userdata) hears INITIALIZED; the PS3 then waits for a PS Vita in a dialog the user can only
// cancel, and there is none here, so the user cancels: FINALIZED with CELL_CROSS_CONTROLLER_ERROR_CANCEL.
enum : u32 {
    CELL_CROSS_CONTROLLER_ERROR_CANCEL = 0x8002cd80,
    CELL_CROSS_CONTROLLER_ERROR_INVALID_PKG_FILENAME = 0x8002cd92,
    CELL_CROSS_CONTROLLER_ERROR_INVALID_SIG_FILENAME = 0x8002cd93,
    CELL_CROSS_CONTROLLER_ERROR_INVALID_ICON_FILENAME = 0x8002cd94,
    CELL_CROSS_CONTROLLER_ERROR_INVALID_VALUE = 0x8002cd95,
    CELL_CROSS_CONTROLLER_ERROR_INVALID_STATE = 0x8002cd99,
};

struct CrossControllerState {
    std::mutex mutex;
    bool initialized = false;
};

// CellCrossControllerParam: be32 pPackageFileName, pSignatureFileName, pIconFileName, option.
// CellCrossControllerPackageInfo: be32 pTitle, pTitleId, pAppVer.
s32 cellCrossControllerInitialize(PPUContext& ctx, u32 param, u32 pkg_info, u32 cb, u32 userdata)
{
    const std::string call = std::format("cellCrossControllerInitialize({:#x}, {:#x}, {:#x}, {:#x})", param, pkg_info, cb,
        userdata);
    constexpr std::string_view chan = "cellCrossController";
    CrossControllerState& s = state<CrossControllerState>();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return logged(chan, error(CELL_CROSS_CONTROLLER_ERROR_INVALID_STATE), call);
    lv2::PpuThread& t = thread_of(ctx);
    if (!param || !pkg_info || !guest_readable(t, param, 16) || !guest_readable(t, pkg_info, 12))
        return logged(chan, error(CELL_CROSS_CONTROLLER_ERROR_INVALID_VALUE), call);
    u8* base = t.kernel.base();
    const auto ends = [&](u32 ptr, u32 len) { return ptr && read_cstr(t, ptr, len + 1).terminated; };
    if (!ends(ps_read32(base, param + 0), 255)) return logged(chan, error(CELL_CROSS_CONTROLLER_ERROR_INVALID_PKG_FILENAME), call);
    if (!ends(ps_read32(base, param + 4), 255)) return logged(chan, error(CELL_CROSS_CONTROLLER_ERROR_INVALID_SIG_FILENAME), call);
    if (!ends(ps_read32(base, param + 8), 255)) return logged(chan, error(CELL_CROSS_CONTROLLER_ERROR_INVALID_ICON_FILENAME), call);
    if (!ends(ps_read32(base, pkg_info + 8), 6) || !ends(ps_read32(base, pkg_info + 4), 10) ||
        !ends(ps_read32(base, pkg_info + 0), 52) || !cb) {
        return logged(chan, error(CELL_CROSS_CONTROLLER_ERROR_INVALID_VALUE), call);
    }
    s.initialized = true;
    queue_callback(cb, {1, 0, 0, userdata});
    queue_callback(cb, {2, static_cast<u64>(static_cast<s64>(error(CELL_CROSS_CONTROLLER_ERROR_CANCEL))), 0, userdata});
    PS_LOG_WARN(chan, "no PS Vita and no dialog UI: the user cancels waiting for one");
    return logged(chan, lv2::CELL_OK, call);
}

// cellGameExec: how the game was booted, and PlayStation Home's paths (Home is gone: NOAPP, as RPCS3).
enum : u32 {
    CELL_GAME_ERROR_NOTFOUND = 0x8002cb04,
    CELL_GAME_ERROR_PARAM = 0x8002cb07,
    CELL_GAME_ERROR_NOAPP = 0x8002cb08,
    CELL_GAME_ERROR_NOTSUPPORTED = 0x8002cb21,
};
constexpr u32 CELL_GAME_GAMETYPE_DISC = 1, CELL_GAME_GAMETYPE_HDD = 2, CELL_GAME_GAMETYPE_HOME = 4;
constexpr u32 CELL_GAME_DIRNAME_SIZE = 32;

struct GameExecState {
    std::mutex mutex;
    u32 execdata = 0;
};

// cellGameGetBootGameInfo(type, dirName, execdata): DISC (1) or HDD (2) and an HDD game's directory name, and what
// cellGameSetExitParam last set.
s32 cellGameGetBootGameInfo(PPUContext& ctx, u32 type, u32 dir_name, u32 execdata)
{
    const std::string call = std::format("cellGameGetBootGameInfo({:#x}, {:#x}, {:#x})", type, dir_name, execdata);
    if (!type || !dir_name) return logged("cellGameExec", error(CELL_GAME_ERROR_PARAM), call);
    u8* base = guest_base(ctx);
    const GameInfo& g = HleProcess::current()->config().game;
    const bool hdd = g.boot == GameInfo::Boot::Hdd;
    ps_write32(base, type, hdd ? 2 : 1);
    if (execdata) {
        GameExecState& s = state<GameExecState>();
        std::lock_guard lock(s.mutex);
        ps_write32(base, execdata, s.execdata);
    }
    if (hdd) put_string(base, dir_name, 32, g.title_id);
    return logged("cellGameExec", lv2::CELL_OK, call);
}

s32 cellGameSetExitParam(PPUContext&, u32 execdata)
{
    GameExecState& s = state<GameExecState>();
    std::lock_guard lock(s.mutex);
    s.execdata = execdata;
    return logged("cellGameExec", lv2::CELL_OK, std::format("cellGameSetExitParam({:#x})", execdata));
}

s32 home_path(std::string_view name, u32 path, s32 result)
{
    return logged("cellGameExec", path ? result : error(CELL_GAME_ERROR_PARAM), std::format("{}({:#x})", name, path));
}

s32 cellGameGetHomeDataExportPath(PPUContext&, u32 p)
{
    return home_path("cellGameGetHomeDataExportPath", p, error(CELL_GAME_ERROR_NOAPP));
}
s32 cellGameGetHomeDataImportPath(PPUContext&, u32 p)
{
    return home_path("cellGameGetHomeDataImportPath", p, error(CELL_GAME_ERROR_NOAPP));
}
s32 cellGameGetHomePath(PPUContext&, u32 p) { return home_path("cellGameGetHomePath", p, lv2::CELL_OK); }

s32 cellGameGetHomeLaunchOptionPath(PPUContext&, u32 common, u32 personal)
{
    return home_path("cellGameGetHomeLaunchOptionPath", common && personal ? 1 : 0, error(CELL_GAME_ERROR_NOAPP));
}

// D-672: the games a PS3 has installed, deleted or booted through cellGameExec (RPCS3's signatures; its four are
// todos answering CELL_OK). pspocket runs one game, the one it was built for (on iOS each game is an app of its own,
// D-036), so no other game is booted or deleted from it. What is installed is read from pspocket's /dev_hdd0 as
// cellGame reads it: /dev_hdd0/game/<dirName> with a PARAM.SFO of an HDD game's category (RPCS3 psf::is_cat_hdd); the
// disc is in the drive when the game booted from it or /dev_bdvd has PS3_GAME/PARAM.SFO.
//
// A directory name: 1 to 31 characters of [A-Z0-9_-], not starting with '_' (RPCS3's sysutil_check_name_string, as
// cellGameDeleteGameData checks its name), ending within CELL_GAME_DIRNAME_SIZE bytes; nullopt otherwise (logged when
// it is not guest memory).
std::optional<std::string> game_dir_name(PPUContext& ctx, u32 addr, std::string_view what)
{
    if (!addr) return std::nullopt;
    const GuestString s = read_cstr(thread_of(ctx), addr, CELL_GAME_DIRNAME_SIZE);
    if (s.faulted) {
        log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}) is not guest memory", what, addr));
        return std::nullopt;
    }
    const bool name_chars = std::all_of(s.text.begin(), s.text.end(),
        [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
    if (!s.terminated || s.text.empty() || s.text[0] == '_' || !name_chars) return std::nullopt;
    return s.text;
}

// Whether /dev_hdd0/game/<name> holds an installed HDD game.
bool hdd_game_installed(lv2::Kernel& kernel, const std::string& name)
{
    std::vector<u8> bytes;
    if (!kernel.vfs_read_file("/dev_hdd0/game/" + name + "/PARAM.SFO", bytes)) return false;
    const std::optional<psf::Registry> sfo = psf::parse(bytes);
    if (!sfo) return false;
    const std::string cat = psf::get_string(*sfo, "CATEGORY");
    return cat.size() == 2 && cat[1] != 'D' && cat != "DG" && cat != "MS";
}

bool disc_in_drive(lv2::Kernel& kernel)
{
    if (HleProcess::current()->config().game.boot == GameInfo::Boot::Disc) return true;
    std::vector<u8> bytes;
    return kernel.vfs_read_file("/dev_bdvd/PS3_GAME/PARAM.SFO", bytes);
}

// cellGameGetList(listBufNum, listBuf, u32* listNum, u32* getListNum, memContainer): no game is listed. The list's
// entries are not known (RPCS3: "unk"), so none is written even where /dev_hdd0 holds games: both counts are 0.
s32 cellGameGetList(PPUContext& ctx, u32 list_buf_num, u32 list_buf, u32 list_num, u32 get_list_num, u32 container)
{
    const std::string call = std::format("cellGameGetList(listBufNum {}, {:#x}, listNum {:#x}, getListNum {:#x}, "
                                         "memContainer {:#x})",
        list_buf_num, list_buf, list_num, get_list_num, container);
    if (!guest_out(ctx, list_num, 4, "cellGameGetList: listNum") ||
        !guest_out(ctx, get_list_num, 4, "cellGameGetList: getListNum")) {
        return logged("cellGameExec", error(CELL_GAME_ERROR_PARAM), call);
    }
    u8* base = guest_base(ctx);
    ps_write32(base, list_num, 0);
    ps_write32(base, get_list_num, 0);
    return logged("cellGameExec", lv2::CELL_OK, call + ": no game listed");
}

// cellGameDeleteGame(dirName, memContainer): an installed HDD game is not deleted at a game's request (the PS3 asks
// the user in a dialog; there is none, and pspocket deletes no installation); one not installed is NOTFOUND.
s32 cellGameDeleteGame(PPUContext& ctx, u32 dir_name, u32 container)
{
    const std::string shown =
        dir_name ? read_cstr(thread_of(ctx), dir_name, CELL_GAME_DIRNAME_SIZE).text : std::string();
    const std::string call =
        std::format("cellGameDeleteGame({:#x} \"{}\", memContainer {:#x})", dir_name, shown, container);
    const std::optional<std::string> name = game_dir_name(ctx, dir_name, "cellGameDeleteGame: dirName");
    if (!name) return logged("cellGameExec", error(CELL_GAME_ERROR_PARAM), call);
    if (!hdd_game_installed(thread_of(ctx).kernel, *name))
        return logged("cellGameExec", error(CELL_GAME_ERROR_NOTFOUND), call + ": no such game installed");
    PS_LOG_WARN("cellGameExec", "cellGameDeleteGame: /dev_hdd0/game/{} is not deleted at a game's request", *name);
    return logged("cellGameExec", error(CELL_GAME_ERROR_NOTSUPPORTED), call);
}

// cellGameExecGame(type, dirName, options, memContainer, execData, userData): DISC or HDD (HOME: Home is gone, NOAPP);
// a game that is not there is NOTFOUND, and one that is (this game too) is NOTSUPPORTED: pspocket boots no other
// program from a running one (lv2's exitspawn, too, ends the process without the program it names).
s32 cellGameExecGame(PPUContext& ctx, u32 type, u32 dir_name, u32 options, u32 container, u32 exec_data, u32 user_data)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::string shown = dir_name ? read_cstr(t, dir_name, CELL_GAME_DIRNAME_SIZE).text : std::string();
    const std::string call = std::format("cellGameExecGame(type {}, dirName {:#x} \"{}\", options {:#x}, memContainer "
                                         "{:#x}, execData {:#x}, userData {:#x})",
        type, dir_name, shown, options, container, exec_data, user_data);
    switch (type) {
    case CELL_GAME_GAMETYPE_HOME:
        return logged("cellGameExec", error(CELL_GAME_ERROR_NOAPP), call + ": PlayStation Home is gone");
    case CELL_GAME_GAMETYPE_DISC:
        if (!disc_in_drive(t.kernel))
            return logged("cellGameExec", error(CELL_GAME_ERROR_NOTFOUND), call + ": no disc in the drive");
        break;
    case CELL_GAME_GAMETYPE_HDD: {
        const std::optional<std::string> name = game_dir_name(ctx, dir_name, "cellGameExecGame: dirName");
        if (!name) return logged("cellGameExec", error(CELL_GAME_ERROR_PARAM), call);
        if (!hdd_game_installed(t.kernel, *name))
            return logged("cellGameExec", error(CELL_GAME_ERROR_NOTFOUND), call + ": no such game installed");
        break;
    }
    default: return logged("cellGameExec", error(CELL_GAME_ERROR_PARAM), call);
    }
    PS_LOG_WARN("cellGameExec", "cellGameExecGame: pspocket runs this game only; it boots no other program from it");
    return logged("cellGameExec", error(CELL_GAME_ERROR_NOTSUPPORTED), call);
}

// cellGameGetExitGameInfo(u32* status, u32* type, char* dirName, u32* execData, u32* userData): what the game this
// one booted through cellGameExecGame left when it ended. None was ever booted, so there is nothing to tell: RPCS3's
// CELL_OK with nothing written, once status, type and dirName are checked (execData and userData may be null, as
// cellGameGetBootGameInfo's execdata).
s32 cellGameGetExitGameInfo(PPUContext& ctx, u32 status, u32 type, u32 dir_name, u32 exec_data, u32 user_data)
{
    const std::string call = std::format("cellGameGetExitGameInfo({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", status, type,
        dir_name, exec_data, user_data);
    if (!guest_out(ctx, status, 4, "cellGameGetExitGameInfo: status") ||
        !guest_out(ctx, type, 4, "cellGameGetExitGameInfo: type") ||
        !guest_out(ctx, dir_name, CELL_GAME_DIRNAME_SIZE, "cellGameGetExitGameInfo: dirName")) {
        return logged("cellGameExec", error(CELL_GAME_ERROR_PARAM), call);
    }
    return logged("cellGameExec", lv2::CELL_OK, call + ": no game was booted from this one; nothing written");
}

// ------------------------------------------------------------------------------------- cellBGDLUtility

// Background downloads (D-671; RPCS3 cellBgdl.cpp/.h: four todos answering CELL_OK): the PS3 downloads a game's PSN
// content in the background while it runs, and the game reads how far each download has come. There is no PSN here
// (D-139), so nothing is ever downloading:
// - GetInfo(const char* content_id, CellBGDLInfo* info, s32 num) and GetInfo2 (a service ID) find no download: 0
//   entries, RPCS3's CELL_OK read as the count, nothing written. The ID may be null (left as RPCS3 takes it); a buffer
//   for num entries must be guest memory, and num must not be negative (CELL_BGDL_UTIL_ERROR_PARAM).
// - SetMode keeps the mode, AUTO or ALWAYS_ALLOW (another is PARAM), and GetMode writes it; AUTO at first.
// Polled calls are logged at info the first few times.
enum : u32 {
    CELL_BGDL_UTIL_ERROR_PARAM = 0x8002ce03,
};
constexpr u32 CELL_BGDL_MODE_AUTO = 0, CELL_BGDL_MODE_ALWAYS_ALLOW = 1;
constexpr u32 kBgdlInfoSize = 24; // CellBGDLInfo: be64 received_size, content_size; be s32 state; be32 reserved
constexpr u32 kBgdlIdMax = 48;    // read for the log: a content ID is 36 characters, a service ID fewer

struct BgdlState {
    std::mutex mutex;
    u32 mode = CELL_BGDL_MODE_AUTO;
    std::atomic<u32> info_calls{0}, mode_calls{0};
};

s32 bgdl_info(PPUContext& ctx, std::string_view name, u32 id, u32 info, s32 num)
{
    BgdlState& s = state<BgdlState>();
    const GuestString text = id ? read_cstr(thread_of(ctx), id, kBgdlIdMax) : GuestString{};
    const std::string call = std::format("{}(\"{}\", info {:#x}, num {})", name, text.text, info, num);
    if (text.faulted) {
        log_call_site(ctx, guest_base(ctx), std::format("{}: the ID ({:#x}) is not guest memory", name, id));
        return logged_polled(s.info_calls, "cellBGDL", error(CELL_BGDL_UTIL_ERROR_PARAM), call);
    }
    if (num < 0 ||
        (num > 0 && !guest_out(ctx, info, u64{kBgdlInfoSize} * static_cast<u32>(num), std::format("{}: info", name)))) {
        return logged_polled(s.info_calls, "cellBGDL", error(CELL_BGDL_UTIL_ERROR_PARAM), call);
    }
    return logged_polled(s.info_calls, "cellBGDL", 0, call + ": no background download (no PSN), 0 entries");
}

s32 cellBGDLGetInfo(PPUContext& ctx, u32 content_id, u32 info, s32 num)
{
    return bgdl_info(ctx, "cellBGDLGetInfo", content_id, info, num);
}

s32 cellBGDLGetInfo2(PPUContext& ctx, u32 service_id, u32 info, s32 num)
{
    return bgdl_info(ctx, "cellBGDLGetInfo2", service_id, info, num);
}

s32 cellBGDLSetMode(PPUContext&, u32 mode)
{
    const std::string call = std::format("cellBGDLSetMode({})", mode);
    if (mode != CELL_BGDL_MODE_AUTO && mode != CELL_BGDL_MODE_ALWAYS_ALLOW)
        return logged("cellBGDL", error(CELL_BGDL_UTIL_ERROR_PARAM), call);
    BgdlState& s = state<BgdlState>();
    std::lock_guard lock(s.mutex);
    s.mode = mode;
    return logged("cellBGDL", lv2::CELL_OK, call);
}

s32 cellBGDLGetMode(PPUContext& ctx, u32 mode)
{
    BgdlState& s = state<BgdlState>();
    const std::string call = std::format("cellBGDLGetMode({:#x})", mode);
    if (!guest_out(ctx, mode, 4, "cellBGDLGetMode: mode"))
        return logged_polled(s.mode_calls, "cellBGDL", error(CELL_BGDL_UTIL_ERROR_PARAM), call);
    std::lock_guard lock(s.mutex);
    ps_write32(guest_base(ctx), mode, s.mode);
    return logged_polled(s.mode_calls, "cellBGDL", lv2::CELL_OK, call + std::format(": {}", s.mode));
}

// ------------------------------------------------------------------------------------- cellRtcAlarm

// The system's RTC alarms (D-673; libsysutil_rtcalarm, an internal module, 0xf032 in RPCS3's cellSysmodule.cpp): what
// an application registers to have the console woken from standby at a time (the system manager's RTC alarm, lv2's
// sys_sm_set_rtc_alarm, which only the system may call). RPCS3 registers the five names with no arguments (each
// UNIMPLEMENTED_FUNC answering CELL_OK) and nothing public describes them, so none of their arguments is read or
// written: each call logs its argument registers. pspocket's console has no standby to wake from, so no alarm ever
// rings or runs: Register keeps a registration under its first argument (the low word; what it names its alarm by is
// not known), Unregister forgets the one kept under its first argument (one not kept is logged, and still CELL_OK:
// the keys are a guess), Notification, StopRunning and GetStatus have no rung or running alarm to tell of. Every answer
// is RPCS3's CELL_OK. Each function's first calls are logged at info.
constexpr usize kMaxAlarms = 256; // registrations kept; more are answered and not kept (logged)

enum class Alarm : u8 { Register, Unregister, Notification, StopRunning, GetStatus, Count };

struct RtcAlarmState {
    std::mutex mutex;
    std::map<u32, std::array<u64, 4>> alarms; // by the first argument's low word: r3..r6 as given
    std::array<std::atomic<u32>, static_cast<usize>(Alarm::Count)> calls{};
};

s32 alarm_logged(RtcAlarmState& s, Alarm fn, std::string_view call)
{
    return logged_polled(s.calls[static_cast<usize>(fn)], "cellRtcAlarm", lv2::CELL_OK, call);
}

s32 cellRtcAlarmRegister(PPUContext& ctx)
{
    std::string call = argument_registers("cellRtcAlarmRegister", ctx);
    RtcAlarmState& s = state<RtcAlarmState>();
    std::lock_guard lock(s.mutex);
    const u32 key = static_cast<u32>(ctx.gpr[3]);
    if (s.alarms.size() >= kMaxAlarms && !s.alarms.contains(key)) {
        call += std::format(": not kept (already {} registrations)", kMaxAlarms);
    } else {
        s.alarms[key] = {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]};
        call += std::format(": kept ({} in all); it never rings (no standby to wake from)", s.alarms.size());
    }
    return alarm_logged(s, Alarm::Register, call);
}

s32 cellRtcAlarmUnregister(PPUContext& ctx)
{
    const std::string call = argument_registers("cellRtcAlarmUnregister", ctx);
    RtcAlarmState& s = state<RtcAlarmState>();
    std::lock_guard lock(s.mutex);
    const bool kept = s.alarms.erase(static_cast<u32>(ctx.gpr[3])) != 0;
    return alarm_logged(s, Alarm::Unregister,
        call + (kept ? std::format(": forgotten ({} left)", s.alarms.size()) : std::string(": none kept under it")));
}

s32 cellRtcAlarmNotification(PPUContext& ctx)
{
    return alarm_logged(state<RtcAlarmState>(), Alarm::Notification,
        argument_registers("cellRtcAlarmNotification", ctx) + ": no alarm has rung; nothing written");
}

s32 cellRtcAlarmStopRunning(PPUContext& ctx)
{
    return alarm_logged(state<RtcAlarmState>(), Alarm::StopRunning,
        argument_registers("cellRtcAlarmStopRunning", ctx) + ": no alarm is running");
}

s32 cellRtcAlarmGetStatus(PPUContext& ctx)
{
    RtcAlarmState& s = state<RtcAlarmState>();
    std::lock_guard lock(s.mutex);
    return alarm_logged(s, Alarm::GetStatus,
        argument_registers("cellRtcAlarmGetStatus", ctx) +
            std::format(": {} registered, none rung; nothing written", s.alarms.size()));
}

} // namespace

bool sysutil_avc_loaded(u32& media)
{
    AvcState& a = state<AvcState>();
    std::lock_guard lock(a.mutex);
    media = a.media;
    return a.callback != 0;
}

void register_sysutil_misc(Registry& r)
{
    r.add("cellUserInfo", {
        PS_HLE_FUNC(0x2B761140, cellUserInfoGetStat),
        PS_HLE_FUNC(0x3097CC1C, cellUserInfoSelectUser_ListType),
        PS_HLE_FUNC(0x55123A25, cellUserInfoSelectUser_SetList),
        PS_HLE_FUNC(0xB3516536, cellUserInfoEnableOverlay),
        PS_HLE_FUNC(0xC55E338B, cellUserInfoGetList),
    });
    r.add("cellScreenShotUtility", {
        PS_HLE_FUNC(0xD3AD63E4, cellScreenShotSetParameter),
        PS_HLE_FUNC(0x7A9C2243, cellScreenShotSetOverlayImage),
        PS_HLE_FUNC(0x9E33AB8F, cellScreenShotEnable),
        PS_HLE_FUNC(0xFC6F4E74, cellScreenShotDisable),
    });
    r.add("cellSysconfExtUtility", {
        PS_HLE_FUNC(0x08DB2ADF, cellSysconfBtGetDeviceList),
    });
    r.add("cellSysutilMisc", {
        PS_HLE_FUNC(0x571DC686, cellSysutilGetLicenseArea),
    });
    r.add("cellSysutilNpEula", {
        PS_HLE_FUNC(0x59D1629A, sceNpEulaCheckEulaStatus),
        PS_HLE_FUNC(0x5EC05AD8, sceNpEulaAbort),
        PS_HLE_FUNC(0x6599500D, sceNpEulaShowCurrentEula),
    });
    r.add("cellSysutilAp", {
        PS_HLE_FUNC(0x9E67E0DD, cellSysutilApGetRequiredMemSize),
        PS_HLE_UNVERIFIED(0x3343824C, cellSysutilApOn), // D-671
        PS_HLE_UNVERIFIED(0x90C2BB19, cellSysutilApOff),
    });
    r.add("cellVideoUpload", {
        PS_HLE_FUNC(0x122E0D0F, cellVideoUploadInitialize),
    });
    r.add("cellPrintUtility", {
        PS_HLE_FUNC(0xC9C3EF14, cellPrintLoadAsync),
        PS_HLE_FUNC(0xF0865182, cellPrintLoadAsync2),
        PS_HLE_FUNC(0xF9A53F35, cellPrintOpenConfig),
        PS_HLE_FUNC(0xEB51AA38, cellPrintUnloadAsync),
    });
    r.add("cellAuthDialogUtility", {
        PS_HLE_FUNC(0x395EF8D7, cellAuthDialogOpen),
        PS_HLE_UNVERIFIED(0xEB27885B, cellAuthDialogAbort), // D-672
        PS_HLE_UNVERIFIED(0x975D4824, cellAuthDialogClose),
    });
    r.add("cellCrossController", {
        PS_HLE_FUNC(0x37E1F502, cellCrossControllerInitialize),
    });
    r.add("cellSysutilAvcExt", {
        PS_HLE_FUNC(0x001074D0, cellSysutilAvcExtIsMicAttached),
        PS_HLE_FUNC(0x22A36B23, cellSysutilAvcExtGetWindowPosition),
        PS_HLE_FUNC(0x28F0DC04, cellSysutilAvcExtGetWindowSize),
        PS_HLE_FUNC(0x2D9A1997, cellSysutilAvcExtGetWindowShowStatus),
        PS_HLE_FUNC(0x326FAB55, cellSysutilAvcExtGetNamePlateShowStatus),
        PS_HLE_FUNC(0x332EE22C, cellSysutilAvcExtSetWindowAlpha),
        PS_HLE_FUNC(0x40BC33C8, cellSysutilAvcExtSetWindowSize),
        PS_HLE_FUNC(0x51CCBE09, cellSysutilAvcExtLoadAsyncEx),
        PS_HLE_FUNC(0x61B1CB18, cellSysutilAvcExtShowWindow),
        PS_HLE_FUNC(0x646C6C5F, cellSysutilAvcExtIsCameraAttached),
        PS_HLE_FUNC(0x739C2F63, cellSysutilAvcExtHideWindow),
        PS_HLE_FUNC(0x8ED310E5, cellSysutilAvcExtGetWindowRotation),
        PS_HLE_FUNC(0xAED82C21, cellSysutilAvcExtGetWindowAlpha),
        PS_HLE_FUNC(0xCBE0C10A, cellSysutilAvcExtGetSurfacePointer),
        PS_HLE_FUNC(0xD8685383, cellSysutilAvcExtInitOptionParam),
        PS_HLE_FUNC(0xE8B1C18D, cellSysutilAvcExtSetWindowZorder),
    });
    r.add("cellSubDisplay", {
        PS_HLE_FUNC(0xF9A7E8A5, cellSubDisplayInit),
        PS_HLE_FUNC(0x551D80A5, cellSubDisplayEnd),
        PS_HLE_FUNC(0x6595CE22, cellSubDisplayGetRequiredMemory),
        PS_HLE_FUNC(0xA5BCCB47, cellSubDisplayStart),
        PS_HLE_FUNC(0x6D85DDB3, cellSubDisplayStop),
        PS_HLE_FUNC(0x938AC642, cellSubDisplayGetVideoBuffer),
        PS_HLE_FUNC(0xAEE1E0C2, cellSubDisplayAudioOutBlocking),
        PS_HLE_FUNC(0x5468D6B0, cellSubDisplayAudioOutNonBlocking),
        PS_HLE_FUNC(0x8A264D71, cellSubDisplayGetPeerNum),
        PS_HLE_FUNC(0xE2485F79, cellSubDisplayGetPeerList),
        PS_HLE_FUNC(0x415ED9DC, cellSubDisplayGetTouchInfo),
    });
    r.add("cellGameExec", {
        PS_HLE_FUNC(0xF6ACD0BC, cellGameGetBootGameInfo),
        PS_HLE_FUNC(0x38579EC9, cellGameSetExitParam),
        PS_HLE_FUNC(0x59B1EDE1, cellGameGetHomeDataExportPath),
        PS_HLE_FUNC(0x72CC6CF7, cellGameGetHomeDataImportPath),
        PS_HLE_FUNC(0x59BBEBD4, cellGameGetHomePath),
        PS_HLE_FUNC(0x94E9F81D, cellGameGetHomeLaunchOptionPath),
        PS_HLE_UNVERIFIED(0x93D4AE69, cellGameGetList), // D-672
        PS_HLE_UNVERIFIED(0x34B74160, cellGameDeleteGame),
        PS_HLE_UNVERIFIED(0xCA872C41, cellGameExecGame),
        PS_HLE_UNVERIFIED(0x7767C4E3, cellGameGetExitGameInfo),
    });
    r.add("cellBGDLUtility", { // D-671
        PS_HLE_UNVERIFIED(0x4E9BB95B, cellBGDLGetInfo),
        PS_HLE_UNVERIFIED(0x2AB0D183, cellBGDLGetInfo2),
        PS_HLE_UNVERIFIED(0x7E134A90, cellBGDLSetMode),
        PS_HLE_UNVERIFIED(0x74E57BDF, cellBGDLGetMode),
    });
    r.add("cellRtcAlarm", { // D-673
        PS_HLE_UNVERIFIED(0x122D3DB1, cellRtcAlarmRegister),
        PS_HLE_UNVERIFIED(0x559234A3, cellRtcAlarmUnregister),
        PS_HLE_UNVERIFIED(0xAD8D9839, cellRtcAlarmNotification),
        PS_HLE_UNVERIFIED(0xB287748C, cellRtcAlarmStopRunning),
        PS_HLE_UNVERIFIED(0xDB215455, cellRtcAlarmGetStatus),
    });
    r.add("cellSysutil", {
        PS_HLE_FUNC(0x27AC51E4, cellStorageDataImportMove),
        PS_HLE_FUNC(0x918288FB, cellStorageDataImport),
        PS_HLE_FUNC(0xABC3CD2C, cellStorageDataExport),
        PS_HLE_FUNC(0xB2B6CDD0, cellSysconfOpen),
        PS_HLE_FUNC(0x15DF71ED, cellSysutilAvcLoadAsync),
        PS_HLE_FUNC(0x6C673F78, cellSysutilAvcUnloadAsync),
        PS_HLE_FUNC(0x17DBE8B3, cellSysutilAvcJoinRequest),
        PS_HLE_FUNC(0xF2C4A425, cellSysutilAvcByeRequest),
        PS_HLE_FUNC(0xF4AA8B27, cellSysutilAvcCancelByeRequest),
        PS_HLE_FUNC(0xBCCD70DD, cellSysutilAvcCancelJoinRequest),
        PS_HLE_FUNC(0xEB9D78D8, cellSysutilAvcGetLayoutMode),
        PS_HLE_FUNC(0x08269F37, cellSysutilAvcGetShowStatus),
        PS_HLE_FUNC(0x073B89D5, cellSysutilAvcGetSpeakerVolumeLevel),
        PS_HLE_FUNC(0x7D94CA36, cellSysutilAvcGetVideoMuting),
        PS_HLE_FUNC(0x5EE3BC26, cellSysutilAvcGetVoiceMuting),
        PS_HLE_FUNC(0xF481967F, cellSysutilAvcGetAttribute),
        PS_HLE_FUNC(0xECA938CA, cellSysutilAvcSetAttribute),
        PS_HLE_FUNC(0x5E91BC26, cellSysutilAvcEnumPlayers),
        PS_HLE_FUNC(0x018A1381, cellSysutilAvcSetLayoutMode),
        PS_HLE_FUNC(0xAFFDADC0, cellSysutilAvcSetSpeakerVolumeLevel),
    });
}

} // namespace pspocket::hle
