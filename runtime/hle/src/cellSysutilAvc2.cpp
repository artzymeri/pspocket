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
//   whose voice is muted. Camera and microphone are detached; the microphone's stream has no data; windows and the
//   screen are never shown; the player list is one player.
// - RPCS3's estimate of the memory container a chat needs, with its arithmetic.
// RPCS3's stubs here (windows, streaming, voice detection) are answered from rpcs3_stubs.inc (D-182).
//
// Difference from RPCS3: pointers are checked before use (D-042): memory that isn't guest memory is logged with the call
// site and refused with CELL_AVC2_ERROR_INVALID_ARGUMENT before anything changes (RPCS3 faults, or aborts where it
// ensure()s a pointer).

#include <bit>
#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

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

bool known_version(u32 v) { return v == 100 || v == 110 || v == 120 || v == 130 || v == 140; }

// RPCS3 avc2_settings.
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
};

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

// RPCS3 cellSysutilAvc2Unload: the voice chat's callback goes, with its waiting events.
s32 cellSysutilAvc2Unload(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (!s.callback) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), "cellSysutilAvc2Unload()");
    drop_callback(s);
    return finish(lv2::CELL_OK, "cellSysutilAvc2Unload()");
}

// RPCS3 cellSysutilAvc2Unload2(mediaType): the voice chat as Unload; the video chat has nothing to drop (a TODO).
s32 cellSysutilAvc2Unload2(PPUContext&, u32 media)
{
    const std::string call = std::format("cellSysutilAvc2Unload2({:#x})", media);
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    if (media == CELL_SYSUTIL_AVC2_VOICE_CHAT) {
        if (!s.callback) return finish(error(CELL_AVC2_ERROR_NOT_INITIALIZED), call);
        drop_callback(s);
        return finish(lv2::CELL_OK, call);
    }
    return finish(media == CELL_SYSUTIL_AVC2_VIDEO_CHAT ? lv2::CELL_OK : error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
}

// RPCS3 cellSysutilAvc2UnloadAsync: UNLOAD_SUCCEEDED to the callback, which then goes.
s32 cellSysutilAvc2UnloadAsync(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_UNLOAD_SUCCEEDED, 0);
    return finish(lv2::CELL_OK, "cellSysutilAvc2UnloadAsync()");
}

// RPCS3 cellSysutilAvc2UnloadAsync2(mediaType): the voice chat as UnloadAsync; the video chat's event carries 2, so
// the callback stays.
s32 cellSysutilAvc2UnloadAsync2(PPUContext&, u32 media)
{
    const std::string call = std::format("cellSysutilAvc2UnloadAsync2({:#x})", media);
    if (media != CELL_SYSUTIL_AVC2_VOICE_CHAT && media != CELL_SYSUTIL_AVC2_VIDEO_CHAT) {
        return finish(error(CELL_AVC2_ERROR_INVALID_ARGUMENT), call);
    }
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_UNLOAD_SUCCEEDED, media == CELL_SYSUTIL_AVC2_VOICE_CHAT ? 0 : 2);
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

// RPCS3 cellSysutilAvc2LeaveChatRequest: LEAVE_SUCCEEDED to the callback.
s32 cellSysutilAvc2LeaveChatRequest(PPUContext&)
{
    Avc2State& s = avc2();
    std::lock_guard lock(s.mutex);
    queue_event(s, CELL_AVC2_EVENT_LEAVE_SUCCEEDED, 0);
    return finish(lv2::CELL_OK, "cellSysutilAvc2LeaveChatRequest()");
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

s32 cellSysutilAvc2GetScreenShowStatus(PPUContext& ctx, u32 visible)
{
    return put_byte(ctx, "cellSysutilAvc2GetScreenShowStatus", visible, 0);
}

s32 cellSysutilAvc2GetWindowShowStatus(PPUContext& ctx, u32 member_id, u32 visible)
{
    return put_byte(ctx, std::format("cellSysutilAvc2GetWindowShowStatus({:#x})", member_id & 0xFFFF), visible, 0);
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
s32 cellSysutilAvc2GetWindowSize(PPUContext&, u32, u32 width, u32 height)
{
    return checked("cellSysutilAvc2GetWindowSize", {width, height});
}
s32 cellSysutilAvc2GetWindowPosition(PPUContext&, u32, u32 x, u32 y, u32 z)
{
    return checked("cellSysutilAvc2GetWindowPosition", {x, y, z});
}
s32 cellSysutilAvc2GetWindowString(PPUContext&, u32, u32 string, u32 len)
{
    return checked("cellSysutilAvc2GetWindowString", {string, len});
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
    });
}

} // namespace pspocket::hle
