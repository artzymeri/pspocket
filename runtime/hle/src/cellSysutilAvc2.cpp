// cellSysutilAvc2: the PS3's voice and video chat for matching 2 rooms. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellSysutilAvc2.cpp/.h (avc2_settings and its callback dispatcher), and RPCS3's
// sysutil_register_cb_with_id / sysutil_unregister_cb_with_id (cellSysutil.cpp).
//
// As RPCS3, with no camera, no microphone and no network behind it:
// - Loading checks RPCS3's init parameters (voice: 2 to 64 players, up to 16 speakers, normal quality, the streaming
//   modes of each parameter version; video: windows, bitrate and frame rate) and keeps the voice chat's callback.
//   cellSysutilAvc2LoadAsync, UnloadAsync(2), JoinChatRequest and LeaveChatRequest answer through that callback at the
//   game's next cellSysutilCheckCallback: load, unload, join and leave all succeed (RPCS3's TODOs). An unload's event
//   drops the callback after it runs; cellSysutilAvc2Unload(2) drops it at once, and the events still waiting with
//   it (RPCS3 unregisters them; here a generation count skips them).
// - The settings are kept: voice, video and speaker muting (all on at first), the speaker volume (40), the players
//   whose voice is muted. Camera and microphone are detached; the microphone's stream has no data; nothing is drawn
//   (the windows and the screen are shown only as the game switches them, below); the player list is one player.
// - RPCS3's estimate of the memory container a chat needs, with its arithmetic.
//
// The functions RPCS3 leaves as stubs are pspocket's own (D-675, D-676; unverified, D-594: made without Sony's module),
// as this library on a console that never reaches a peer: no PSN, the network disconnected, no camera or microphone.
// - The chat. The join pspocket answers as succeeded (above) leaves a chat with nobody else in it, until LeaveChat,
//   LeaveChatRequest or an unload of the voice chat. Streaming starts and stops in it (sending to nobody); outside one
//   StartStreaming and StopStreaming answer INVALID_STATUS. The streaming target and priority are kept as given, the
//   target's lists checked; voice detection is switched on and off and never detects anything (no microphone).
// - The video chat. Loading it keeps its window limit and resolution; its windows are the game's: CreateWindow makes
//   one for a member (any member ID: matching 2 is offline, so pspocket has no room to check it against), at most the
//   load's max_video_windows (TOO_MANY_WINDOWS), one each (WINDOW_ALREADY_EXISTS); Show, Hide, SetWindowPosition,
//   SetWindowSize and DestroyWindow act on one the game made (WINDOW_NOT_FOUND otherwise); ShowScreen and HideScreen
//   switch the screen; ChangeVideoResolution goes up to the load's maximum. The getters report what the game set
//   (shown or not; a position or size once it set one: the system's own layout is not known here, so nothing is
//   written before); nothing is drawn. Unloading the video chat drops its windows.
// - Every call that needs the library loaded answers NOT_INITIALIZED without it (as cellSysutilAvc2Unload); the video
//   chat's calls need the video chat.
//
// Difference from RPCS3: pointers are checked before use (D-042): memory that isn't guest memory is logged with the call
// site and refused with CELL_AVC2_ERROR_INVALID_ARGUMENT before anything changes (RPCS3 faults, or aborts where it
// ensure()s a pointer).

#include <bit>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
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

constexpr std::string_view kChan = "cellSysutilAvc2";

enum : u32 {
    CELL_AVC2_ERROR_NOT_SUPPORTED = 0x8002B702,
    CELL_AVC2_ERROR_NOT_INITIALIZED = 0x8002B703,
    CELL_AVC2_ERROR_ALREADY_INITIALIZED = 0x8002B704,
    CELL_AVC2_ERROR_INVALID_ARGUMENT = 0x8002B705,
    CELL_AVC2_ERROR_INVALID_STATUS = 0x8002B70A,
    CELL_AVC2_ERROR_WINDOW_ALREADY_EXISTS = 0x8002B70F,
    CELL_AVC2_ERROR_TOO_MANY_WINDOWS = 0x8002B710,
    CELL_AVC2_ERROR_WINDOW_NOT_FOUND = 0x8002B712,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

s32 finish(s32 r, const std::string& call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, kChan, "{}: {}", call,
        r < 0 ? std::format("{:#010x}", static_cast<u32>(r)) : std::string("CELL_OK"));
    return r;
}

constexpr u32 CELL_SYSUTIL_AVC2_VOICE_CHAT = 0x1, CELL_SYSUTIL_AVC2_VIDEO_CHAT = 0x10;
constexpr u32 CELL_SYSUTIL_AVC2_VOICE_QUALITY_NORMAL = 1, CELL_SYSUTIL_AVC2_FRAME_MODE_NORMAL = 1,
              CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA = 1, CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QVGA = 2;
constexpr u32 CELL_SYSUTIL_AVC2_STREAMING_MODE_NORMAL = 0, CELL_SYSUTIL_AVC2_STREAMING_MODE_DIRECT_WAN = 1,
              CELL_SYSUTIL_AVC2_STREAMING_MODE_DIRECT_LAN = 2, CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_2 = 2;
constexpr u32 CELL_AVC2_EVENT_LOAD_SUCCEEDED = 1, CELL_AVC2_EVENT_LOAD_FAILED = 2, CELL_AVC2_EVENT_UNLOAD_SUCCEEDED = 3,
              CELL_AVC2_EVENT_UNLOAD_FAILED = 4, CELL_AVC2_EVENT_JOIN_SUCCEEDED = 5,
              CELL_AVC2_EVENT_LEAVE_SUCCEEDED = 7;
constexpr u8 CELL_AVC2_CAMERA_STATUS_DETACHED = 0, CELL_AVC2_MIC_STATUS_DETACHED = 0;

// CellSysutilAvc2InitParam (132 bytes): be16 version at 0, max_players 2, spu_load_average 4, direct_streaming_mode 6
// (also streaming_mode.mode); be32 media_type 40; voice_param at 44 (be32 voice_quality, be16 max_speakers at 48,
// mic_out_stream_sharing at 50); video_param at 76 (be32 frame_mode at 80, max_video_resolution 84, be16
// max_video_windows 88, max_video_framerate 90, be32 max_video_bitrate 92, video_stream_sharing byte at 100).
namespace init_param {
constexpr u32 version = 0, max_players = 2, spu_load_average = 4, direct_streaming_mode = 6, media_type = 40,
              voice_quality = 44, max_speakers = 48, mic_out_stream_sharing = 50, frame_mode = 80,
              max_video_resolution = 84, max_video_windows = 88, max_video_framerate = 90, max_video_bitrate = 92,
              video_stream_sharing = 100, kSize = 132;
} // namespace init_param
// CellSysutilAvc2PlayerInfo (16 bytes): be16 member_id, joined, connected, mic_attached, reserved.
constexpr u32 kPlayerInfoSize = 16;

// CellSysutilAvc2StreamingTarget (20 bytes, RPCS3 cellSysutilAvc2.h): be32 target_mode, then a union at 4: the team ID
// (a byte), a CellSysutilAvc2RoomMemberList (be32 member_id list, u8 member_num at 8) or a
// CellSysutilAvc2MemberIpAndPortList (be32 member_id, dst_addr and dst_port lists at 4, 8 and 12, be16 my_member_id at
// 16, u8 member_num at 18).
constexpr u32 CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_ROOM = 0x100, CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_TEAM = 0x200,
              CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_PRIVATE = 0x300, CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_DIRECT = 0x1000;
namespace target {
constexpr u32 mode = 0, team_id = 4, list_members = 4, list_num = 8, ip_members = 4, ip_addrs = 8, ip_ports = 12,
              ip_my_member = 16, ip_num = 18;
} // namespace target

bool known_version(u32 v) { return v == 100 || v == 110 || v == 120 || v == 130 || v == 140; }

// A video chat window the game made (CreateWindow): shown or not (hidden at first, as D-187's getter reported every
// window), and its position and size once the game set them.
struct Avc2Window {
    bool shown = false, positioned = false, sized = false;
    f32 x = 0, y = 0, z = 0, width = 0, height = 0;
};

// The streaming target the game set (SetStreamingTarget), as given.
struct Avc2Target {
    u32 mode = 0; // 0: none set
    u8 team = 0;
    u16 my_member = 0;
    std::vector<u16> members;
    std::vector<std::pair<u32, u16>> addresses; // a direct target's (in_addr, port), each member's
};

// RPCS3 avc2_settings, with the chat and the video chat's state (D-675, D-676).
struct Avc2State {
    std::mutex mutex;
    u32 callback = 0, arg = 0; // the voice chat's CellSysutilAvc2Callback(event, param, userdata)
    u64 generation = 0;        // bumped when RPCS3 unregisters the waiting callbacks
    u32 streaming_mode = CELL_SYSUTIL_AVC2_STREAMING_MODE_NORMAL;
    u8 mic_out_stream_sharing = 0, video_stream_sharing = 0;
    u32 total_video_bitrate = 0;
    std::set<u16> voice_muting_players;
    bool voice_muting = true, video_muting = true, speaker_muting = true;
    f32 speaker_volume_level = 40.0f;
    // The chat: joined (with nobody else in it), streaming in it, its target and priority; voice detection.
    bool joined = false, streaming = false, voice_detection = false;
    Avc2Target target;
    u8 stream_priority = 0;
    // The video chat: loaded, its window limit, its maximum and current resolution, the screen, the windows.
    bool video_loaded = false, screen_shown = false;
    u32 max_video_windows = 0, max_video_resolution = 0, video_resolution = 0;
    std::map<u16, Avc2Window> windows; // by member ID
};

// Whether the library is loaded: a voice chat (its callback) or a video chat.
bool loaded(const Avc2State& s) { return s.callback || s.video_loaded; }

// The chat ends with the voice chat (its join, streaming, target and priority, and voice detection).
void end_voice_chat(Avc2State& s)
{
    s.joined = s.streaming = s.voice_detection = false;
    s.target = {};
    s.stream_priority = 0;
}

// The video chat's windows, screen and resolution go with it.
void end_video_chat(Avc2State& s)
{
    s.video_loaded = s.screen_shown = false;
    s.max_video_windows = s.max_video_resolution = s.video_resolution = 0;
    s.windows.clear();
}

Avc2State& avc2()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSysutilAvc2 called without an HLE process");
    return p->state<Avc2State>();
}

// RPCS3 avc2_settings::register_cb_call: at the game's next check the callback hears (event, error_code); a load
// failure or an unload with an error code below 2 drops the callback and every event still waiting.
void queue_event(Avc2State& s, u32 event, u64 error_code)
{
    const u64 generation = s.generation;
    sysutil_queue_call([event, error_code, generation](PPUContext& ctx) -> s32 {
        Avc2State& a = avc2();
        u32 callback = 0, arg = 0;
        {
            std::lock_guard lock(a.mutex);
            if (a.generation != generation) return lv2::CELL_OK; // unregistered meanwhile
            callback = a.callback;
            arg = a.arg;
        }
        if (!callback) return lv2::CELL_OK;
        call_guest(ctx, callback, {event, error_code, arg});
        if ((event == CELL_AVC2_EVENT_LOAD_FAILED || event == CELL_AVC2_EVENT_UNLOAD_SUCCEEDED ||
                event == CELL_AVC2_EVENT_UNLOAD_FAILED) &&
            error_code < 2) {
            std::lock_guard lock(a.mutex);
            a.generation++;
            a.callback = a.arg = 0;
        }
        return lv2::CELL_OK;
    });
}

// The callback dropped, with the events still waiting (RPCS3 sysutil_unregister_cb_with_id).
void drop_callback(Avc2State& s)
{
    s.generation++;
    s.callback = s.arg = 0;
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// A byte setting written to `out` (muting flags, show status, attached devices).
s32 put_byte(PPUContext& ctx, std::string_view name, u32 out, u8 value)
{
    const std::string call = std::format("{}({:#x})", name, out);
    if (!out || !usable(ctx, out, 1, name)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    ps_write8(guest_base(ctx), out, value);
    return finish(lv2::CELL_OK, call + std::format(": {}", value));
}

// The calls that only check their pointers (RPCS3's TODOs): every one in `ptrs` non-null.
s32 checked(std::string_view name, std::initializer_list<u32> ptrs)
{
    std::string call = std::string(name) + "(";
    bool ok = true;
    for (const u32 p : ptrs) {
        call += std::format("{}{:#x}", call.back() == '(' ? "" : ", ", p);
        ok = ok && p;
    }
    call += ")";
    return finish(ok ? lv2::CELL_OK : error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
}

// RPCS3 cellSysutilAvc2Load_shared(ctx_id, container, callback_func, user_data, init_param).
s32 load_shared(PPUContext& ctx, u32 callback, u32 user_data, u32 param)
{
    if (!param) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
    if (!usable(ctx, param, init_param::kSize, "cellSysutilAvc2Load: init_param")) {
        return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    const u32 version = ps_read16(base, param + init_param::version);
    if (!known_version(version)) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    switch (ps_read32(base, param + init_param::media_type)) {
    case CELL_SYSUTIL_AVC2_VOICE_CHAT: {
        const u32 players = ps_read16(base, param + init_param::max_players);
        const u32 speakers = ps_read16(base, param + init_param::max_speakers);
        if (players < 2 || players > 64 || ps_read16(base, param + init_param::spu_load_average) > 100 ||
            ps_read32(base, param + init_param::voice_quality) != CELL_SYSUTIL_AVC2_VOICE_QUALITY_NORMAL ||
            speakers == 0 || speakers > 16) {
            return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
        }
        const u32 direct = ps_read16(base, param + init_param::direct_streaming_mode);
        u32 mode = direct;
        if (version >= 120) {
            // DIRECT_LAN is refused when streaming_mode.mode is NORMAL, but that is the same field, so never (RPCS3).
            if (direct > CELL_SYSUTIL_AVC2_STREAMING_MODE_DIRECT_LAN) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
        } else if (version >= 110) {
            if (direct != CELL_SYSUTIL_AVC2_STREAMING_MODE_NORMAL && direct != CELL_SYSUTIL_AVC2_STREAMING_MODE_DIRECT_WAN) {
                return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
            }
        } else {
            mode = s.streaming_mode;
        }
        s.streaming_mode = mode;
        s.mic_out_stream_sharing = ps_read8(base, param + init_param::mic_out_stream_sharing);
        if (!callback) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
        if (s.callback) return error(CELL_AVC2_ERROR_ALREADY_INITIALIZED);
        s.callback = callback;
        s.arg = user_data;
        return lv2::CELL_OK;
    }
    case CELL_SYSUTIL_AVC2_VIDEO_CHAT: {
        if (callback) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT); // RPCS3's check, as it reads
        const u32 windows = ps_read16(base, param + init_param::max_video_windows);
        const bool normal = ps_read32(base, param + init_param::frame_mode) == CELL_SYSUTIL_AVC2_FRAME_MODE_NORMAL;
        if (windows == 0 || windows > (normal ? 6u : 16u)) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
        const u32 bitrate_max = ps_read32(base, param + init_param::max_video_bitrate);
        if (bitrate_max < 1000 || bitrate_max > 512000) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
        const u32 framerate = ps_read16(base, param + init_param::max_video_framerate);
        if (framerate == 0 || framerate > 30) return error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
        const u32 resolution = ps_read32(base, param + init_param::max_video_resolution);
        const u32 bitrate = resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA ? 76800u
                            : resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QVGA ? 307200u
                                                                                    : 0u;
        const u8 sharing = ps_read8(base, param + init_param::video_stream_sharing);
        u32 total = 0;
        if (bitrate) {
            const u32 count = windows + (sharing == CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_2 ? 1 : 0);
            total = ((count * bitrate + 0xFFFFF) & ~0xFFFFFu) + 0x100000;
        }
        s.video_stream_sharing = sharing;
        s.total_video_bitrate = total;
        // The video chat loaded (D-676): its window limit, and its resolution, the most it may change to. Loaded again,
        // it keeps its windows (RPCS3 refuses no second load).
        s.video_loaded = true;
        s.max_video_windows = windows;
        s.max_video_resolution = s.video_resolution = resolution;
        return lv2::CELL_OK;
    }
    default: return error(CELL_AVC2_ERROR_NOT_SUPPORTED);
    }
}

s32 cellSysutilAvc2Load(PPUContext& ctx, u32 ctx_id, u32 container, u32 callback, u32 user_data, u32 param)
{
    return finish(load_shared(ctx, callback, user_data, param),
        std::format("cellSysutilAvc2Load(ctx_id {:#x}, container {:#x}, callback {:#x}, user_data {:#x}, init_param "
                    "{:#x})",
            ctx_id, container, callback, user_data, param));
}

// RPCS3 cellSysutilAvc2LoadAsync: the load, then LOAD_SUCCEEDED to the callback (for a video chat there is none).
s32 cellSysutilAvc2LoadAsync(PPUContext& ctx, u32 ctx_id, u32 container, u32 callback, u32 user_data, u32 param)
{
    const std::string call = std::format("cellSysutilAvc2LoadAsync(ctx_id {:#x}, container {:#x}, callback {:#x}, "
                                         "user_data {:#x}, init_param {:#x})",
        ctx_id, container, callback, user_data, param);
    if (const s32 r = load_shared(ctx, callback, user_data, param)) return finish(r, call);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_LOAD_SUCCEEDED, 0);
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvc2Unload: the voice chat's callback goes, with its waiting events; the chat and the video chat go
// too (D-675, D-676). With neither chat loaded, NOT_INITIALIZED (RPCS3 looks at the callback only, as it does not keep
// the video chat).
s32 cellSysutilAvc2Unload(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!loaded(s)) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), "cellSysutilAvc2Unload()");
    drop_callback(s);
    end_voice_chat(s);
    end_video_chat(s);
    return finish(lv2::CELL_OK, "cellSysutilAvc2Unload()");
}

// RPCS3 cellSysutilAvc2Unload2(mediaType): the voice chat as Unload, the chat with it; the video chat's windows,
// screen and resolution (RPCS3: a TODO with nothing to drop).
s32 cellSysutilAvc2Unload2(PPUContext&, u32 media)
{
    const std::string call = std::format("cellSysutilAvc2Unload2({:#x})", media);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (media == CELL_SYSUTIL_AVC2_VOICE_CHAT) {
        if (!s.callback) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call);
        drop_callback(s);
        end_voice_chat(s);
        return finish(lv2::CELL_OK, call);
    }
    if (media != CELL_SYSUTIL_AVC2_VIDEO_CHAT) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    end_video_chat(s);
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvc2UnloadAsync: UNLOAD_SUCCEEDED to the callback, which then goes. The chat and the video chat
// end at the call.
s32 cellSysutilAvc2UnloadAsync(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_UNLOAD_SUCCEEDED, 0);
    end_voice_chat(s);
    end_video_chat(s);
    return finish(lv2::CELL_OK, "cellSysutilAvc2UnloadAsync()");
}

// RPCS3 cellSysutilAvc2UnloadAsync2(mediaType): the voice chat as UnloadAsync; the video chat's event carries 2, so
// the callback stays. The chat, or the video chat, ends at the call.
s32 cellSysutilAvc2UnloadAsync2(PPUContext&, u32 media)
{
    const std::string call = std::format("cellSysutilAvc2UnloadAsync2({:#x})", media);
    if (media != CELL_SYSUTIL_AVC2_VOICE_CHAT && media != CELL_SYSUTIL_AVC2_VIDEO_CHAT) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_UNLOAD_SUCCEEDED, media == CELL_SYSUTIL_AVC2_VOICE_CHAT ? 0 : 2);
    if (media == CELL_SYSUTIL_AVC2_VOICE_CHAT) end_voice_chat(s);
    else end_video_chat(s);
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvc2JoinChat(room_id, eventId, eventParam): a room, or none in a direct streaming mode; nothing to
// join (RPCS3's TODO).
s32 join_chat(PPUContext& ctx, const std::string& call, u32 room_id, bool request)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    u64 room = 0;
    if (room_id) {
        if (!usable(ctx, room_id, 8, call)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
        room = ps_read64(guest_base(ctx), room_id);
    } else if (s.streaming_mode != CELL_SYSUTIL_AVC2_STREAMING_MODE_NORMAL) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    if (request) queue_event(s, CELL_AVC2_EVENT_JOIN_SUCCEEDED, 0);
    s.joined = true; // a chat with nobody else in it (D-675)
    return finish(lv2::CELL_OK, call + std::format(": room {:#x}, nobody to talk to", room));
}

s32 cellSysutilAvc2JoinChat(PPUContext& ctx, u32 room_id, u32 event_id, u32 event_param)
{
    return join_chat(ctx,
        std::format("cellSysutilAvc2JoinChat(room_id {:#x}, eventId {:#x}, eventParam {:#x})", room_id, event_id,
            event_param),
        room_id, false);
}

// RPCS3 cellSysutilAvc2JoinChatRequest: the same, then JOIN_SUCCEEDED to the callback.
s32 cellSysutilAvc2JoinChatRequest(PPUContext& ctx, u32 room_id)
{
    return join_chat(ctx, std::format("cellSysutilAvc2JoinChatRequest({:#x})", room_id), room_id, true);
}

// The chat left: its streaming stops (D-675). The windows are the video chat's and stay.
void leave_chat(Avc2State& s) { s.joined = s.streaming = false; }

// RPCS3 cellSysutilAvc2LeaveChatRequest: LEAVE_SUCCEEDED to the callback.
s32 cellSysutilAvc2LeaveChatRequest(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_LEAVE_SUCCEEDED, 0);
    leave_chat(s);
    return finish(lv2::CELL_OK, "cellSysutilAvc2LeaveChatRequest()");
}

// cellSysutilAvc2LeaveChat: LeaveChatRequest's synchronous form (D-675; RPCS3's stub), the chat left at once with no
// event, as the synchronous JoinChat joins with none. Its arguments are not read: RPCS3 registers it with none, and
// by JoinChat's they would be an event ID and parameter to write (to check on the Mac), which a wrong guess would
// write into the game's own data.
s32 cellSysutilAvc2LeaveChat(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    const bool was = s.joined;
    leave_chat(s);
    return finish(lv2::CELL_OK, std::format("cellSysutilAvc2LeaveChat(){}", was ? "" : ": no chat joined"));
}

// RPCS3 cellSysutilAvc2InitParam(version, option): the parameters cleared and the version written, then the version
// checked.
s32 cellSysutilAvc2InitParam(PPUContext& ctx, u32 version, u32 option)
{
    const std::string call = std::format("cellSysutilAvc2InitParam(version {}, option {:#x})", version & 0xFFFF, option);
    if (!option || !usable(ctx, option, init_param::kSize, "cellSysutilAvc2InitParam: option")) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    u8* base = guest_base(ctx);
    std::memset(base + option, 0, init_param::kSize);
    ps_write16(base, option + init_param::version, static_cast<u16>(version));
    return finish(known_version(version & 0xFFFF) ? lv2::CELL_OK : error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
}

// RPCS3 cellSysutilAvc2EstimateMemoryContainerSize(initparam, size), with its arithmetic: version 100 needs 4 MB; the
// later ones 3 MB for a voice chat, and for a video chat a size from the windows, the resolution and the frame mode.
s32 cellSysutilAvc2EstimateMemoryContainerSize(PPUContext& ctx, u32 param, u32 size)
{
    const std::string call = std::format("cellSysutilAvc2EstimateMemoryContainerSize({:#x}, {:#x})", param, size);
    if (!param || !size) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    if (!usable(ctx, param, init_param::kSize, "cellSysutilAvc2EstimateMemoryContainerSize: initparam") ||
        !usable(ctx, size, 4, "cellSysutilAvc2EstimateMemoryContainerSize: size")) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    u8* base = guest_base(ctx);
    const u32 version = ps_read16(base, param + init_param::version);
    if (version == 100) {
        ps_write32(base, size, 0x400000);
        return finish(lv2::CELL_OK, call);
    }
    if (!known_version(version)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    const u32 media = ps_read32(base, param + init_param::media_type);
    if (media == CELL_SYSUTIL_AVC2_VOICE_CHAT) {
        ps_write32(base, size, 0x300000);
        return finish(lv2::CELL_OK, call);
    }
    if (media != CELL_SYSUTIL_AVC2_VIDEO_CHAT) {
        ps_write32(base, size, 0);
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    const u32 resolution = ps_read32(base, param + init_param::max_video_resolution);
    const u32 max_windows = ps_read16(base, param + init_param::max_video_windows);
    s32 window_count = static_cast<s32>(max_windows);
    if (ps_read8(base, param + init_param::video_stream_sharing) == CELL_SYSUTIL_AVC2_VIDEO_SHARING_MODE_2) {
        window_count++;
    }
    u32 estimated = 0x40E666;
    if (resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA) {
        estimated = ((static_cast<u32>(window_count) * 0x12C00) & 0xFFF00000u) + 0x50E666;
    } else if (resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QVGA) {
        estimated += ((static_cast<u32>(window_count) * 0x4B000) & 0xFFF00000u) + 0x100000;
    }
    window_count = ps_read32(base, param + init_param::frame_mode) == CELL_SYSUTIL_AVC2_FRAME_MODE_NORMAL
                       ? static_cast<s32>(max_windows - 1)
                       : 1;
    u32 val = max_windows * 10000;
    if (resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA) {
        val += static_cast<u32>(static_cast<s64>(window_count) * 0x96000) + 0x10C9E0;
    } else {
        val += static_cast<u32>(static_cast<s64>(static_cast<f64>(window_count) * 1258291.2)) + 0x1ED846;
    }
    const s32 blocks = static_cast<s32>(val) / 128; // RPCS3's shift with its rounding toward zero
    estimated = ((estimated + static_cast<u32>(blocks) * 0x80 + 0x80080) & 0xFFF00000u) + 0x100000;
    ps_write32(base, size, estimated);
    return finish(lv2::CELL_OK, call + std::format(": {:#x}", estimated));
}

// RPCS3 cellSysutilAvc2GetPlayerInfo(player_id, player_info): the player, joined and connected, no microphone.
s32 cellSysutilAvc2GetPlayerInfo(PPUContext& ctx, u32 player_id, u32 player_info)
{
    const std::string call = std::format("cellSysutilAvc2GetPlayerInfo({:#x}, {:#x})", player_id, player_info);
    if (!player_id || !player_info) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    if (!usable(ctx, player_id, 2, call) || !usable(ctx, player_info, kPlayerInfoSize, call)) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    u8* base = guest_base(ctx);
    ps_write16(base, player_info, ps_read16(base, player_id));
    ps_write8(base, player_info + 2, 1); // joined
    ps_write8(base, player_info + 3, 1); // connected
    ps_write8(base, player_info + 4, CELL_AVC2_MIC_STATUS_DETACHED);
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvc2EnumPlayers(players_num, players_id): without a list, one player; with one, IDs 1 to
// *players_num.
s32 cellSysutilAvc2EnumPlayers(PPUContext& ctx, u32 players_num, u32 players_id)
{
    const std::string call = std::format("cellSysutilAvc2EnumPlayers({:#x}, {:#x})", players_num, players_id);
    if (!players_num || !usable(ctx, players_num, 4, call)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    u8* base = guest_base(ctx);
    if (!players_id) {
        ps_write32(base, players_num, 1);
        return finish(lv2::CELL_OK, call);
    }
    const s32 n = static_cast<s32>(ps_read32(base, players_num));
    if (n > 0 && !usable(ctx, players_id, 2 * static_cast<u32>(n), call)) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    for (s32 i = 0; i < n; i++) ps_write16(base, players_id + 2 * static_cast<u32>(i), static_cast<u16>(i + 1));
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvc2MicRead(ptr, pSize): only with the microphone's stream shared; it holds no data.
s32 cellSysutilAvc2MicRead(PPUContext& ctx, u32 ptr, u32 size)
{
    const std::string call = std::format("cellSysutilAvc2MicRead({:#x}, {:#x})", ptr, size);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!s.mic_out_stream_sharing) return finish(lv2::CELL_OK, call + ": the stream isn't shared");
    if (size) {
        if (!usable(ctx, size, 4, call)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
        ps_write32(guest_base(ctx), size, 0);
    }
    return finish(lv2::CELL_OK, call + ": no microphone");
}

// Muting and volume settings.
s32 cellSysutilAvc2SetVoiceMuting(PPUContext&, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    s.voice_muting = (muting & 0xFF) != 0;
    return finish(lv2::CELL_OK, std::format("cellSysutilAvc2SetVoiceMuting({})", muting & 0xFF));
}

s32 cellSysutilAvc2GetVoiceMuting(PPUContext& ctx, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    return put_byte(ctx, "cellSysutilAvc2GetVoiceMuting", muting, s.voice_muting);
}

// RPCS3 cellSysutilAvc2SetVideoMuting: 0 or 1 ("Weird check", RPCS3 notes).
s32 cellSysutilAvc2SetVideoMuting(PPUContext&, u32 muting)
{
    const std::string call = std::format("cellSysutilAvc2SetVideoMuting({})", muting & 0xFF);
    if ((muting & 0xFF) > 1) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    s.video_muting = (muting & 0xFF) != 0;
    return finish(lv2::CELL_OK, call);
}

s32 cellSysutilAvc2GetVideoMuting(PPUContext& ctx, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    return put_byte(ctx, "cellSysutilAvc2GetVideoMuting", muting, s.video_muting);
}

s32 cellSysutilAvc2SetSpeakerMuting(PPUContext&, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    s.speaker_muting = (muting & 0xFF) != 0;
    return finish(lv2::CELL_OK, std::format("cellSysutilAvc2SetSpeakerMuting({})", muting & 0xFF));
}

s32 cellSysutilAvc2GetSpeakerMuting(PPUContext& ctx, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    return put_byte(ctx, "cellSysutilAvc2GetSpeakerMuting", muting, s.speaker_muting);
}

// RPCS3 cellSysutilAvc2SetSpeakerVolumeLevel(float level, in f1): kept as given.
s32 cellSysutilAvc2SetSpeakerVolumeLevel(PPUContext& ctx)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    s.speaker_volume_level = static_cast<f32>(ctx.fpr[1]);
    return finish(lv2::CELL_OK, std::format("cellSysutilAvc2SetSpeakerVolumeLevel({})", ctx.fpr[1]));
}

s32 cellSysutilAvc2GetSpeakerVolumeLevel(PPUContext& ctx, u32 level)
{
    const std::string call = std::format("cellSysutilAvc2GetSpeakerVolumeLevel({:#x})", level);
    if (!level || !usable(ctx, level, 4, call)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    ps_write32(guest_base(ctx), level, std::bit_cast<u32>(s.speaker_volume_level));
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellSysutilAvc2SetPlayerVoiceMuting(member_id, muting) and its getter: the muted players.
s32 cellSysutilAvc2SetPlayerVoiceMuting(PPUContext&, u32 member_id, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (muting & 0xFF) s.voice_muting_players.insert(static_cast<u16>(member_id));
    else s.voice_muting_players.erase(static_cast<u16>(member_id));
    return finish(lv2::CELL_OK,
        std::format("cellSysutilAvc2SetPlayerVoiceMuting({:#x}, {})", member_id & 0xFFFF, muting & 0xFF));
}

s32 cellSysutilAvc2GetPlayerVoiceMuting(PPUContext& ctx, u32 member_id, u32 muting)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    return put_byte(ctx, std::format("cellSysutilAvc2GetPlayerVoiceMuting({:#x})", member_id & 0xFFFF), muting,
        s.voice_muting_players.contains(static_cast<u16>(member_id)) ? 1 : 0);
}

// No camera, no microphone; nothing shown.
s32 cellSysutilAvc2IsCameraAttached(PPUContext& ctx, u32 status)
{
    return put_byte(ctx, "cellSysutilAvc2IsCameraAttached", status, CELL_AVC2_CAMERA_STATUS_DETACHED);
}

// RPCS3 ensure()s the pointer (no error code is known); here a null one is refused.
s32 cellSysutilAvc2IsMicAttached(PPUContext& ctx, u32 status)
{
    if (!status) log_call_site(ctx, guest_base(ctx), "cellSysutilAvc2IsMicAttached: a null status (RPCS3 aborts)");
    return put_byte(ctx, "cellSysutilAvc2IsMicAttached", status, CELL_AVC2_MIC_STATUS_DETACHED);
}

// The screen and a window as the game switched them (D-676): hidden until shown; a member with no window, hidden (as
// D-187 answered every member).
s32 cellSysutilAvc2GetScreenShowStatus(PPUContext& ctx, u32 visible)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    return put_byte(ctx, "cellSysutilAvc2GetScreenShowStatus", visible, s.screen_shown ? 1 : 0);
}

s32 cellSysutilAvc2GetWindowShowStatus(PPUContext& ctx, u16 member_id, u32 visible)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    const auto w = s.windows.find(member_id);
    return put_byte(ctx, std::format("cellSysutilAvc2GetWindowShowStatus({:#x})", member_id), visible,
        w != s.windows.end() && w->second.shown ? 1 : 0);
}

// RPCS3 cellSysutilAvc2SetWindowString(member_id, string): shorter than 64 bytes.
s32 cellSysutilAvc2SetWindowString(PPUContext& ctx, u32 member_id, u32 string)
{
    const GuestString text = read_cstr(thread_of(ctx), string, 64);
    const std::string call = std::format("cellSysutilAvc2SetWindowString({:#x}, \"{}\")", member_id & 0xFFFF, text.text);
    if (text.faulted) {
        log_call_site(ctx, guest_base(ctx), call + ": the string runs out of guest memory");
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    return finish(string && text.terminated ? lv2::CELL_OK : error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
}

// The calls RPCS3 only checks (attributes, window geometry and strings, streaming).
s32 cellSysutilAvc2GetAttribute(PPUContext&, u32 attr) { return checked("cellSysutilAvc2GetAttribute", {attr}); }
s32 cellSysutilAvc2SetAttribute(PPUContext&, u32 attr) { return checked("cellSysutilAvc2SetAttribute", {attr}); }
s32 cellSysutilAvc2GetWindowAttribute(PPUContext&, u32, u32 attr)
{
    return checked("cellSysutilAvc2GetWindowAttribute", {attr});
}
s32 cellSysutilAvc2SetWindowAttribute(PPUContext&, u32, u32 attr)
{
    return checked("cellSysutilAvc2SetWindowAttribute", {attr});
}
s32 cellSysutilAvc2GetWindowString(PPUContext&, u32, u32 string, u32 len)
{
    return checked("cellSysutilAvc2GetWindowString", {string, len});
}

// A window's floats as the game set them (D-676), every pointer non-null (RPCS3's check); written, each checked first,
// once the game set them: before that the system's own layout would be read, which is not known here, so nothing is
// written (as D-187's getters wrote nothing).
s32 put_floats(PPUContext& ctx, const std::string& call, std::initializer_list<std::pair<u32, f32>> out, bool set)
{
    for (const auto& [at, value] : out)
        if (!at) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    if (!set) return finish(lv2::CELL_OK, call + ": not set by the game, nothing written");
    for (const auto& [at, value] : out)
        if (!usable(ctx, at, 4, call)) return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    for (const auto& [at, value] : out) ps_write32(guest_base(ctx), at, std::bit_cast<u32>(value));
    return finish(lv2::CELL_OK, call);
}

s32 cellSysutilAvc2GetWindowSize(PPUContext& ctx, u16 member_id, u32 width, u32 height)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    const auto w = s.windows.find(member_id);
    const Avc2Window win = w != s.windows.end() ? w->second : Avc2Window{};
    return put_floats(ctx, std::format("cellSysutilAvc2GetWindowSize({:#x}, {:#x}, {:#x})", member_id, width, height),
        {{width, win.width}, {height, win.height}}, win.sized);
}

s32 cellSysutilAvc2GetWindowPosition(PPUContext& ctx, u16 member_id, u32 x, u32 y, u32 z)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    const auto w = s.windows.find(member_id);
    const Avc2Window win = w != s.windows.end() ? w->second : Avc2Window{};
    return put_floats(ctx,
        std::format("cellSysutilAvc2GetWindowPosition({:#x}, {:#x}, {:#x}, {:#x})", member_id, x, y, z),
        {{x, win.x}, {y, win.y}, {z, win.z}}, win.positioned);
}

// ---- The video chat (D-676): its windows, the screen, the resolution. Each needs the video chat loaded.

// The window the call acts on: NOT_INITIALIZED without the video chat, WINDOW_NOT_FOUND without one for the member.
s32 find_window(Avc2State& s, u16 member_id, Avc2Window*& w)
{
    if (!s.video_loaded) return error(CELL_AVC2_ERROR_NOT_INITIALIZED);
    const auto it = s.windows.find(member_id);
    if (it == s.windows.end()) return error(CELL_AVC2_ERROR_WINDOW_NOT_FOUND);
    w = &it->second;
    return lv2::CELL_OK;
}

// cellSysutilAvc2CreateWindow(member_id): a window for the member, hidden; one each, at most the load's
// max_video_windows. Any member ID: pspocket has no room to check it against (matching 2 is offline), as its
// GetPlayerInfo answers any member (D-187).
s32 cellSysutilAvc2CreateWindow(PPUContext&, u16 member_id)
{
    const std::string call = std::format("cellSysutilAvc2CreateWindow({:#x})", member_id);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!s.video_loaded) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call + ": no video chat loaded");
    if (s.windows.contains(member_id)) return finish(error(CELL_AVC2_ERROR_WINDOW_ALREADY_EXISTS), call);
    if (s.windows.size() >= s.max_video_windows) {
        return finish(error(CELL_AVC2_ERROR_TOO_MANY_WINDOWS),
            call + std::format(": {} windows already (max_video_windows)", s.windows.size()));
    }
    s.windows.emplace(member_id, Avc2Window{});
    return finish(lv2::CELL_OK, call + std::format(": {} windows, nothing drawn here", s.windows.size()));
}

// cellSysutilAvc2DestroyWindow(member_id)
s32 cellSysutilAvc2DestroyWindow(PPUContext&, u16 member_id)
{
    const std::string call = std::format("cellSysutilAvc2DestroyWindow({:#x})", member_id);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    Avc2Window* w = nullptr;
    if (const s32 r = find_window(s, member_id, w)) return finish(r, call);
    s.windows.erase(member_id);
    return finish(lv2::CELL_OK, call);
}

s32 show_window(u16 member_id, bool shown, std::string_view name)
{
    const std::string call = std::format("{}({:#x})", name, member_id);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    Avc2Window* w = nullptr;
    if (const s32 r = find_window(s, member_id, w)) return finish(r, call);
    w->shown = shown;
    return finish(lv2::CELL_OK, call);
}

// cellSysutilAvc2ShowWindow(member_id) and HideWindow(member_id): the window's show status (nothing is drawn here).
s32 cellSysutilAvc2ShowWindow(PPUContext&, u16 member_id)
{
    return show_window(member_id, true, "cellSysutilAvc2ShowWindow");
}
s32 cellSysutilAvc2HideWindow(PPUContext&, u16 member_id)
{
    return show_window(member_id, false, "cellSysutilAvc2HideWindow");
}

// cellSysutilAvc2SetWindowPosition(member_id, float x, float y, float z): kept as given (the coordinates' form, virtual
// or absolute, is the load's; no range is known to check).
s32 cellSysutilAvc2SetWindowPosition(PPUContext&, u16 member_id, float x, float y, float z)
{
    const std::string call = std::format("cellSysutilAvc2SetWindowPosition({:#x}, {}, {}, {})", member_id, x, y, z);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    Avc2Window* w = nullptr;
    if (const s32 r = find_window(s, member_id, w)) return finish(r, call);
    w->x = x;
    w->y = y;
    w->z = z;
    w->positioned = true;
    return finish(lv2::CELL_OK, call);
}

// cellSysutilAvc2SetWindowSize(member_id, float width, float height): kept as given.
s32 cellSysutilAvc2SetWindowSize(PPUContext&, u16 member_id, float width, float height)
{
    const std::string call = std::format("cellSysutilAvc2SetWindowSize({:#x}, {}, {})", member_id, width, height);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    Avc2Window* w = nullptr;
    if (const s32 r = find_window(s, member_id, w)) return finish(r, call);
    w->width = width;
    w->height = height;
    w->sized = true;
    return finish(lv2::CELL_OK, call);
}

s32 show_screen(bool shown, std::string_view name)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!s.video_loaded) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), std::string(name) + "(): no video chat");
    s.screen_shown = shown;
    return finish(lv2::CELL_OK, std::string(name) + "()");
}

// cellSysutilAvc2ShowScreen() and HideScreen(): the screen the windows are on, switched (nothing is drawn here).
s32 cellSysutilAvc2ShowScreen(PPUContext&) { return show_screen(true, "cellSysutilAvc2ShowScreen"); }
s32 cellSysutilAvc2HideScreen(PPUContext&) { return show_screen(false, "cellSysutilAvc2HideScreen"); }

// cellSysutilAvc2ChangeVideoResolution(resolution): QQVGA or QVGA, up to the load's max_video_resolution (when the load
// named one of the two; RPCS3's load takes any). No camera sends at either here; the setting is kept.
s32 cellSysutilAvc2ChangeVideoResolution(PPUContext&, u32 resolution)
{
    const std::string call = std::format("cellSysutilAvc2ChangeVideoResolution({:#x})", resolution);
    if (resolution != CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA &&
        resolution != CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QVGA) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!s.video_loaded) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call + ": no video chat loaded");
    const bool max_known = s.max_video_resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QQVGA ||
                           s.max_video_resolution == CELL_SYSUTIL_AVC2_VIDEO_RESOLUTION_QVGA;
    if (max_known && resolution > s.max_video_resolution) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT),
            call + std::format(": past the load's maximum {:#x}", s.max_video_resolution));
    }
    s.video_resolution = resolution;
    return finish(lv2::CELL_OK, call);
}

// ---- The chat (D-675): streaming, its target and priority, voice detection. Each needs the library loaded.

// cellSysutilAvc2StartStreaming() and StopStreaming(): in a chat (the one pspocket joins has nobody else in it, so
// nothing is sent); outside one, INVALID_STATUS. Starting twice, or stopping when not streaming, changes nothing.
s32 streaming(bool on, std::string_view name)
{
    const std::string call = std::string(name) + "()";
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!loaded(s)) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call);
    if (!s.joined) return finish(error(CELL_AVC2_ERROR_INVALID_STATUS), call + ": no chat joined");
    s.streaming = on;
    return finish(lv2::CELL_OK, call + (on ? ": nobody else in the chat to stream to" : ""));
}

s32 cellSysutilAvc2StartStreaming(PPUContext&) { return streaming(true, "cellSysutilAvc2StartStreaming"); }
s32 cellSysutilAvc2StopStreaming(PPUContext&) { return streaming(false, "cellSysutilAvc2StopStreaming"); }

// cellSysutilAvc2SetStreamingTarget(target): who hears this player. The whole room, a team, a list of members, or (in
// the direct modes) members at given addresses and ports; the lists at least one member each, every list guest
// memory. Kept as given: no member is reached here, so none is checked against a room.
s32 cellSysutilAvc2SetStreamingTarget(PPUContext& ctx, u32 target_addr)
{
    const std::string call = std::format("cellSysutilAvc2SetStreamingTarget({:#x})", target_addr);
    const s32 inval = error(CELL_AVC2_ERROR_INVALID_ARGUMENT);
    if (!target_addr || !usable(ctx, target_addr, 4, call)) return finish(inval, call);
    u8* base = guest_base(ctx);
    Avc2Target t;
    t.mode = ps_read32(base, target_addr + target::mode);
    const auto read_list = [&](u32 list_at, u32 count, u32 size) {
        return list_at && usable(ctx, list_at, count * size, call + std::format(": a list of {}", count));
    };
    switch (t.mode) {
    case CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_ROOM: break;
    case CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_TEAM:
        if (!usable(ctx, target_addr, target::team_id + 1, call)) return finish(inval, call);
        t.team = ps_read8(base, target_addr + target::team_id);
        break;
    case CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_PRIVATE: {
        if (!usable(ctx, target_addr, target::list_num + 1, call)) return finish(inval, call);
        const u32 list = ps_read32(base, target_addr + target::list_members);
        const u32 n = ps_read8(base, target_addr + target::list_num);
        if (!n || !read_list(list, n, 2)) return finish(inval, call + ": no members");
        for (u32 i = 0; i < n; i++) t.members.push_back(ps_read16(base, list + 2 * i));
        break;
    }
    case CELL_SYSUTIL_AVC2_CHAT_TARGET_MODE_DIRECT: {
        if (!usable(ctx, target_addr, target::ip_num + 1, call)) return finish(inval, call);
        const u32 members = ps_read32(base, target_addr + target::ip_members);
        const u32 addrs = ps_read32(base, target_addr + target::ip_addrs);
        const u32 ports = ps_read32(base, target_addr + target::ip_ports);
        const u32 n = ps_read8(base, target_addr + target::ip_num);
        if (!n || !read_list(members, n, 2) || !read_list(addrs, n, 4) || !read_list(ports, n, 2)) {
            return finish(inval, call + ": no members");
        }
        t.my_member = ps_read16(base, target_addr + target::ip_my_member);
        for (u32 i = 0; i < n; i++) {
            t.members.push_back(ps_read16(base, members + 2 * i));
            t.addresses.emplace_back(ps_read32(base, addrs + 4 * i), ps_read16(base, ports + 2 * i));
        }
        break;
    }
    default: return finish(inval, call + std::format(": target mode {:#x}", t.mode));
    }
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!loaded(s)) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call);
    const std::string kept = std::format(": mode {:#x}, {} members, nobody reached here", t.mode, t.members.size());
    s.target = std::move(t);
    return finish(lv2::CELL_OK, call + kept);
}

// cellSysutilAvc2SetStreamPriority(priority): kept as given (its values are not known: nothing to check them by).
s32 cellSysutilAvc2SetStreamPriority(PPUContext&, u8 priority)
{
    const std::string call = std::format("cellSysutilAvc2SetStreamPriority({:#x})", priority);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!loaded(s)) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call);
    s.stream_priority = priority;
    return finish(lv2::CELL_OK, call);
}

// cellSysutilAvc2StartVoiceDetection() and StopVoiceDetection(): the voice chat's detection of the player speaking,
// switched. There is no microphone, so it never detects anything: no VOICE_DETECTED event is sent.
s32 voice_detection(bool on, std::string_view name)
{
    const std::string call = std::string(name) + "()";
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!s.callback) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call + ": no voice chat loaded");
    s.voice_detection = on;
    return finish(lv2::CELL_OK, call + (on ? ": no microphone, nothing to detect" : ""));
}

s32 cellSysutilAvc2StartVoiceDetection(PPUContext&)
{
    return voice_detection(true, "cellSysutilAvc2StartVoiceDetection");
}
s32 cellSysutilAvc2StopVoiceDetection(PPUContext&)
{
    return voice_detection(false, "cellSysutilAvc2StopVoiceDetection");
}

// RPCS3 cellSysutilAvc2StartStreaming2 and StopStreaming2(mediaType): a media type; nothing streams.
s32 streaming2(std::string_view name, u32 media)
{
    const bool ok = media == CELL_SYSUTIL_AVC2_VOICE_CHAT || media == CELL_SYSUTIL_AVC2_VIDEO_CHAT;
    return finish(ok ? lv2::CELL_OK : error(CELL_AVC2_ERROR_INVALID_ARGUMENT), std::format("{}({:#x})", name, media));
}

s32 cellSysutilAvc2StartStreaming2(PPUContext&, u32 media) { return streaming2("cellSysutilAvc2StartStreaming2", media); }
s32 cellSysutilAvc2StopStreaming2(PPUContext&, u32 media) { return streaming2("cellSysutilAvc2StopStreaming2", media); }

} // namespace

void register_cellSysutilAvc2(Registry& r)
{
    r.add("cellSysutilAvc2", {
        PS_HLE_FUNC(0x02C5417A, cellSysutilAvc2GetPlayerInfo),
        PS_HLE_FUNC(0x02DC41EE, cellSysutilAvc2JoinChat),
        PS_HLE_FUNC(0x0F5A2AFB, cellSysutilAvc2GetVideoMuting),
        PS_HLE_FUNC(0x103D6B46, cellSysutilAvc2GetWindowAttribute),
        PS_HLE_FUNC(0x11F071CB, cellSysutilAvc2StopStreaming2),
        PS_HLE_FUNC(0x14937714, cellSysutilAvc2SetVoiceMuting),
        PS_HLE_FUNC(0x17481336, cellSysutilAvc2UnloadAsync),
        PS_HLE_FUNC(0x1BE2B4E0, cellSysutilAvc2GetAttribute),
        PS_HLE_FUNC(0x1D73AB8C, cellSysutilAvc2LoadAsync),
        PS_HLE_FUNC(0x207621A8, cellSysutilAvc2SetSpeakerVolumeLevel),
        PS_HLE_FUNC(0x225142BD, cellSysutilAvc2SetWindowString),
        PS_HLE_FUNC(0x2F280883, cellSysutilAvc2EstimateMemoryContainerSize),
        PS_HLE_FUNC(0x2FC0AB58, cellSysutilAvc2SetVideoMuting),
        PS_HLE_FUNC(0x31FC8B92, cellSysutilAvc2SetPlayerVoiceMuting),
        PS_HLE_FUNC(0x38F33624, cellSysutilAvc2Unload),
        PS_HLE_FUNC(0x3EF4F668, cellSysutilAvc2GetSpeakerVolumeLevel),
        PS_HLE_FUNC(0x4C4B9665, cellSysutilAvc2IsCameraAttached),
        PS_HLE_FUNC(0x55D7BBFD, cellSysutilAvc2MicRead),
        PS_HLE_FUNC(0x5F3811F8, cellSysutilAvc2GetPlayerVoiceMuting),
        PS_HLE_FUNC(0x712D51D6, cellSysutilAvc2JoinChatRequest),
        PS_HLE_FUNC(0x7A69ECC1, cellSysutilAvc2SetWindowAttribute),
        PS_HLE_FUNC(0x82CED772, cellSysutilAvc2GetWindowShowStatus),
        PS_HLE_FUNC(0x89456724, cellSysutilAvc2InitParam),
        PS_HLE_FUNC(0x8A40A618, cellSysutilAvc2GetWindowSize),
        PS_HLE_FUNC(0x97B2DA6A, cellSysutilAvc2LeaveChatRequest),
        PS_HLE_FUNC(0xA26AA437, cellSysutilAvc2IsMicAttached),
        PS_HLE_FUNC(0xA9F5B75C, cellSysutilAvc2GetSpeakerMuting),
        PS_HLE_FUNC(0xB81B9777, cellSysutilAvc2EnumPlayers),
        PS_HLE_FUNC(0xB856B63F, cellSysutilAvc2GetWindowString),
        PS_HLE_FUNC(0xC35ED665, cellSysutilAvc2SetSpeakerMuting),
        PS_HLE_FUNC(0xC6C93D62, cellSysutilAvc2Load),
        PS_HLE_FUNC(0xCCF0AEFF, cellSysutilAvc2SetAttribute),
        PS_HLE_FUNC(0xCE6780C9, cellSysutilAvc2UnloadAsync2),
        PS_HLE_FUNC(0xD42657DD, cellSysutilAvc2StartStreaming2),
        PS_HLE_FUNC(0xDF2ED367, cellSysutilAvc2GetVoiceMuting),
        PS_HLE_FUNC(0xE40E3DFC, cellSysutilAvc2GetScreenShowStatus),
        PS_HLE_FUNC(0xF3B5FF77, cellSysutilAvc2Unload2),
        PS_HLE_FUNC(0xFC2873A9, cellSysutilAvc2GetWindowPosition),
        // RPCS3's stubs, pspocket's own without Sony's module to compare with (D-675, D-676; D-594).
        PS_HLE_UNVERIFIED(0x07236C83, cellSysutilAvc2ChangeVideoResolution),
        PS_HLE_UNVERIFIED(0xA8DC0EFA, cellSysutilAvc2CreateWindow),
        PS_HLE_UNVERIFIED(0x3A37E7F1, cellSysutilAvc2DestroyWindow),
        PS_HLE_UNVERIFIED(0xD7D6272F, cellSysutilAvc2HideScreen),
        PS_HLE_UNVERIFIED(0xDE6AFC37, cellSysutilAvc2HideWindow),
        PS_HLE_UNVERIFIED(0xBF022284, cellSysutilAvc2LeaveChat),
        PS_HLE_UNVERIFIED(0x8C2F5BE3, cellSysutilAvc2SetStreamPriority),
        PS_HLE_UNVERIFIED(0x3447668A, cellSysutilAvc2SetStreamingTarget),
        PS_HLE_UNVERIFIED(0x3C8C827C, cellSysutilAvc2SetWindowPosition),
        PS_HLE_UNVERIFIED(0xAEE75751, cellSysutilAvc2SetWindowSize),
        PS_HLE_UNVERIFIED(0x0B45CD84, cellSysutilAvc2ShowScreen),
        PS_HLE_UNVERIFIED(0xAC170042, cellSysutilAvc2ShowWindow),
        PS_HLE_UNVERIFIED(0x74D22119, cellSysutilAvc2StartStreaming),
        PS_HLE_UNVERIFIED(0x16FF9BA0, cellSysutilAvc2StartVoiceDetection),
        PS_HLE_UNVERIFIED(0x04E1E1E4, cellSysutilAvc2StopStreaming),
        PS_HLE_UNVERIFIED(0x178982D9, cellSysutilAvc2StopVoiceDetection),
    });
}

} // namespace pspocket::hle
