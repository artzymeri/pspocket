// The small sysutil libraries, as RPCS3 has them with no device, service or overlay behind them (D-183): user info,
// screenshots, storage data, system settings, the web browser's setup, AV chat (cellSysutilAvc), the licence area,
// the NP EULA, the access point, remote play, video upload, printing, the auth dialog, the cross controller and
// cellGameExec. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellUserInfo.cpp, cellScreenshot.cpp, cellStorage.cpp,
// cellSysconf.cpp, cellWebBrowser.cpp, cellSysutilAvc.cpp, cellSysutilMisc.cpp, cellSysutilNpEula.cpp,
// cellSysutilAp.cpp, cellRemotePlay.cpp, cellVideoUpload.cpp, cellPrint.cpp, cellAuthDialog.cpp,
// cellCrossController.cpp, cellGameExec.cpp. The functions RPCS3 leaves as stubs there are answered from
// rpcs3_stubs.inc (D-182).
//
// Where the PS3 shows a dialog, there is no UI: the user takes what the cursor starts on, as the message dialog's
// (cellSysutil.cpp). The user list picks the current user (RPCS3's fallback without its overlay), the cross
// controller's "waiting for PS Vita" dialog is cancelled, and the system's callbacks reach the game at its
// cellSysutilCheckCallback.

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
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

// ------------------------------------------------------------------------------------- cellSysutil: web browser

// RPCS3's setup of the web browser (the browser itself is a stub there): 1 MB of memory, and the system callback
// (status, userdata) hears INITIALIZING_FINISHED (1) and SHUTDOWN_FINISHED (4).
struct BrowserState {
    std::mutex mutex;
    u32 system_cb = 0;
};

s32 cellWebBrowserEstimate2(PPUContext& ctx, u32 config, u32 mem_size)
{
    const std::string call = std::format("cellWebBrowserEstimate2({:#x}, {:#x})", config, mem_size);
    if (mem_size) ps_write32(guest_base(ctx), mem_size, 1024 * 1024);
    return logged(kSysutilChan, lv2::CELL_OK, call);
}

s32 cellWebBrowserInitialize(PPUContext&, u32 system_cb, u32 container)
{
    const std::string call = std::format("cellWebBrowserInitialize({:#x}, container {:#x})", system_cb, container);
    BrowserState& b = state<BrowserState>();
    {
        std::lock_guard lock(b.mutex);
        b.system_cb = system_cb;
    }
    if (system_cb) queue_callback(system_cb, {1, 0});
    return logged(kSysutilChan, lv2::CELL_OK, call);
}

void cellWebBrowserShutdown(PPUContext&)
{
    BrowserState& b = state<BrowserState>();
    std::lock_guard lock(b.mutex);
    if (b.system_cb) queue_callback(b.system_cb, {4, 0});
    PS_LOG_INFO(kSysutilChan, "cellWebBrowserShutdown()");
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

// cellSysutilAp: the PS3 as a Wi-Fi access point for a PSP (remote play): 1 MB, and on and off do nothing (RPCS3).
s32 cellSysutilApGetRequiredMemSize(PPUContext&)
{
    return logged("cellSysutilAp", 1024 * 1024, "cellSysutilApGetRequiredMemSize()");
}

// cellRemotePlay: no remote play; the comparative volume is 1.0 (RPCS3).
s32 cellRemotePlayGetComparativeVolume(PPUContext& ctx, u32 volume)
{
    if (volume) ps_write32(guest_base(ctx), volume, std::bit_cast<u32>(1.0f));
    return logged("cellRemotePlay", lv2::CELL_OK, std::format("cellRemotePlayGetComparativeVolume({:#x})", volume));
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
s32 cellAuthDialogOpen(PPUContext&, u64 arg1)
{
    const std::string call = std::format("cellAuthDialogOpen({:#x})", arg1);
    return logged("cellAuthDialog", arg1 == 0 ? error(0x8002d202) : lv2::CELL_OK, call);
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
    CELL_GAME_ERROR_PARAM = 0x8002cb07,
    CELL_GAME_ERROR_NOAPP = 0x8002cb08,
};

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

} // namespace

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
    });
    r.add("cellRemotePlay", {
        PS_HLE_FUNC(0xE12C8C19, cellRemotePlayGetComparativeVolume),
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
    });
    r.add("cellCrossController", {
        PS_HLE_FUNC(0x37E1F502, cellCrossControllerInitialize),
    });
    r.add("cellGameExec", {
        PS_HLE_FUNC(0xF6ACD0BC, cellGameGetBootGameInfo),
        PS_HLE_FUNC(0x38579EC9, cellGameSetExitParam),
        PS_HLE_FUNC(0x59B1EDE1, cellGameGetHomeDataExportPath),
        PS_HLE_FUNC(0x72CC6CF7, cellGameGetHomeDataImportPath),
        PS_HLE_FUNC(0x59BBEBD4, cellGameGetHomePath),
        PS_HLE_FUNC(0x94E9F81D, cellGameGetHomeLaunchOptionPath),
    });
    r.add("cellSysutil", {
        PS_HLE_FUNC(0x27AC51E4, cellStorageDataImportMove),
        PS_HLE_FUNC(0x918288FB, cellStorageDataImport),
        PS_HLE_FUNC(0xABC3CD2C, cellStorageDataExport),
        PS_HLE_FUNC(0xB2B6CDD0, cellSysconfOpen),
        PS_HLE_FUNC(0x6D087930, cellWebBrowserEstimate2),
        PS_HLE_FUNC(0x749C9B5F, cellWebBrowserInitialize),
        PS_HLE_FUNC(0x93CED48D, cellWebBrowserShutdown),
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
