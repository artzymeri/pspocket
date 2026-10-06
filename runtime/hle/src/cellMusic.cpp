// cellMusic and cellMusicDecode: the music the user picks from the PS3's library, played by the system (cellMusic, a
// game's custom soundtrack) or decoded to PCM for the game (cellMusicDecode). Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellMusic.cpp/.h, cellMusicDecode.cpp/.h, cellMusicSelectionContext.cpp
// (music_selection_context) and the media list dialog (rpcs3/Emu/RSX/Overlays/overlay_media_list_dialog.cpp).
//
// pspocket has no media library (cellSearch.cpp, D-186) and no media dialog. RPCS3 shows its media list dialog only
// with its native UI and cancels the selection without one; so does pspocket, at once: the game's callback hears
// SELECT_CONTENTS_RESULT with CELL_MUSIC_CANCELED. So no music is ever selected: nothing plays (the playback status
// stays STOP, as RPCS3's null music handler reports it), nothing decodes, and a selection context the game saved
// names no playlist pspocket knows (RPCS3 keeps playlists in its own cache): setting one answers INVALID_CONTEXT.
// Everything else follows RPCS3: the checks in RPCS3's order, results through the game's callback at its next
// cellSysutilCheckCallback, the volume kept (it starts at 1.0, as RPCS3's Qt player's), cellMusicDecode's states.
// cellMusic and cellMusic2 share their state as in RPCS3; cellMusicDecode and cellMusicDecode2 each have their own.
//
// Difference from RPCS3: pointers are checked before use (D-042): memory that isn't guest memory is logged with the call
// site and refused with the library's PARAM error before anything changes (RPCS3 faults).

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>
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

constexpr std::string_view kMusic = "cellMusic", kDecode = "cellMusicDecode";

// CellMusicError, CellMusic2Error and CellMusicDecodeError share these values.
enum : u32 {
    CELL_MUSIC_CANCELED = 1,
    CELL_MUSIC_ERROR_PARAM = 0x8002C102,
    CELL_MUSIC_ERROR_NO_ACTIVE_CONTENT = 0x8002C104,
    CELL_MUSIC_ERROR_INVALID_CONTEXT = 0x8002C106,
    CELL_MUSIC_DECODE_ERROR_DECODE_FAILURE = 0x8002C107,
    CELL_MUSIC_ERROR_NO_MORE_CONTENT = 0x8002C108,
    CELL_MUSIC_DECODE_ERROR_NO_LPCM_DATA = 0x8002C10B,
    CELL_MUSIC_ERROR_GENERIC = 0x8002C1FF,
    CELL_SEARCH_ERROR_CONTENT_NOT_FOUND = 0x8002C80A,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_MUSIC_ERROR_PARAM: return "ERROR_PARAM";
    case CELL_MUSIC_ERROR_NO_ACTIVE_CONTENT: return "ERROR_NO_ACTIVE_CONTENT";
    case CELL_MUSIC_ERROR_INVALID_CONTEXT: return "ERROR_INVALID_CONTEXT";
    case CELL_MUSIC_DECODE_ERROR_DECODE_FAILURE: return "ERROR_DECODE_FAILURE";
    case CELL_MUSIC_ERROR_NO_MORE_CONTENT: return "ERROR_NO_MORE_CONTENT";
    case CELL_MUSIC_DECODE_ERROR_NO_LPCM_DATA: return "ERROR_NO_LPCM_DATA";
    case CELL_MUSIC_ERROR_GENERIC: return "ERROR_GENERIC";
    case CELL_SEARCH_ERROR_CONTENT_NOT_FOUND: return "CELL_SEARCH_ERROR_CONTENT_NOT_FOUND";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

s32 finish(std::string_view chan, s32 r, const std::string& call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, chan, "{}: {}", call, result_name(r));
    return r;
}

// Events (CellMusic(2)Callback and CellMusicDecode(2)Callback: event, param, userData).
constexpr u32 CELL_MUSIC_EVENT_INITIALIZE_RESULT = 1, CELL_MUSIC_EVENT_FINALIZE_RESULT = 2,
              CELL_MUSIC_EVENT_SELECT_CONTENTS_RESULT = 3, CELL_MUSIC_EVENT_SET_PLAYBACK_COMMAND_RESULT = 4,
              CELL_MUSIC_EVENT_SET_VOLUME_RESULT = 5, CELL_MUSIC_EVENT_SET_SELECTION_CONTEXT_RESULT = 6;
constexpr u32 CELL_MUSIC_DECODE_EVENT_SET_DECODE_COMMAND_RESULT = 4,
              CELL_MUSIC_DECODE_EVENT_SET_SELECTION_CONTEXT_RESULT = 5;
constexpr s32 CELL_MUSIC_PLAYER_MODE_NORMAL = 0, CELL_MUSIC_PB_CMD_STOP = 0, CELL_MUSIC_PB_CMD_FASTREVERSE = 6,
              CELL_MUSIC_PB_STATUS_STOP = 0;
constexpr s32 CELL_MUSIC_DECODE_CMD_STOP = 0, CELL_MUSIC_DECODE_CMD_START = 1, CELL_MUSIC_DECODE_CMD_PREV = 3,
              CELL_MUSIC_DECODE_STATUS_DORMANT = 0, CELL_MUSIC_DECODE_STATUS_DECODING = 1,
              CELL_MUSIC_DECODE_POSITION_NONE = 0, CELL_MUSIC_DECODE2_SPEED_MAX = 0, CELL_MUSIC_DECODE2_SPEED_2 = 2;
constexpr s32 CELL_MUSIC_DECODE2_MIN_BUFFER_SIZE = 448 * 1024;
// CellMusicSelectionContext: 2048 bytes. RPCS3 fills it with "SUS\0" and the hash naming the playlist it saved.
constexpr u32 kContextSize = 2048;
constexpr char kContextMagic[4] = {'S', 'U', 'S', '\0'};

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// The game's callback(event, param, userData) at its next cellSysutilCheckCallback (RPCS3 sysutil_register_cb).
void queue_callback(u32 func, u32 event, u32 param, u32 user_data)
{
    sysutil_queue_call([=](PPUContext& ctx) -> s32 {
        call_guest(ctx, func, {event, param, user_data});
        return lv2::CELL_OK;
    });
}

// RPCS3 music_selection_context::set: "SUS\0" and a hash of letters, digits, '-' and '_' name a playlist RPCS3 saved.
// The hash, when there is one, is returned (cellMusicDecode keeps it, as RPCS3's set does before loading the playlist).
// pspocket never saves a playlist (no selection is ever made), so the context is never valid.
std::string context_hash(const u8* context)
{
    if (std::memcmp(context, kContextMagic, sizeof(kContextMagic)) != 0) return {};
    const char* data = reinterpret_cast<const char*>(context) + sizeof(kContextMagic);
    const std::string_view hash(data, strnlen(data, kContextSize - sizeof(kContextMagic)));
    const bool valid = !hash.empty() && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
    return valid ? std::string(hash) : std::string();
}

// ---------------------------------------------------------------------------------------------- cellMusic(2)

// RPCS3 music_state, with its handler's volume and status. The selection context is never valid here.
struct MusicState {
    std::mutex mutex;
    u32 func = 0, user_data = 0;
    f32 volume = 1.0f;
};

MusicState& music()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kMusic, "cellMusic called without an HLE process");
    return p->state<MusicState>();
}

// Initialize, Initialize2 and the SystemWorkload forms after their checks: the callback hears INITIALIZE_RESULT.
s32 music_initialize(u32 func, u32 user_data)
{
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    m.func = func;
    m.user_data = user_data;
    queue_callback(func, CELL_MUSIC_EVENT_INITIALIZE_RESULT, lv2::CELL_OK, user_data);
    return lv2::CELL_OK;
}

// RPCS3 cellMusicInitialize(mode, container, spuPriority, func, userData).
s32 cellMusicInitialize(PPUContext&, s32 mode, u32 container, s32 spu_priority, u32 func, u32 user_data)
{
    const std::string call = std::format("cellMusicInitialize(mode {}, container {:#x}, spuPriority {}, func {:#x}, "
                                         "userData {:#x})",
        mode, container, spu_priority, func, user_data);
    if (mode != CELL_MUSIC_PLAYER_MODE_NORMAL || !func || spu_priority < 16 || spu_priority > 255) {
        return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return finish(kMusic, music_initialize(func, user_data), call);
}

// RPCS3 cellMusicInitialize2(mode, spuPriority, func, userData).
s32 cellMusicInitialize2(PPUContext&, s32 mode, s32 spu_priority, u32 func, u32 user_data)
{
    const std::string call = std::format("cellMusicInitialize2(mode {}, spuPriority {}, func {:#x}, userData {:#x})",
        mode, spu_priority, func, user_data);
    if (mode != CELL_MUSIC_PLAYER_MODE_NORMAL || !func || spu_priority < 16 || spu_priority > 255) {
        return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return finish(kMusic, music_initialize(func, user_data), call);
}

// RPCS3 cellMusicInitializeSystemWorkload(mode, container, func, userData, spurs, priority, attr): the SPURS
// instance and priorities are only checked.
s32 cellMusicInitializeSystemWorkload(PPUContext&, s32 mode, u32 container, u32 func, u32 user_data, u32 spurs,
    u32 priority, u32 attr)
{
    const std::string call = std::format("cellMusicInitializeSystemWorkload(mode {}, container {:#x}, func {:#x}, "
                                         "userData {:#x}, spurs {:#x}, priority {:#x}, attr {:#x})",
        mode, container, func, user_data, spurs, priority, attr);
    if (mode != CELL_MUSIC_PLAYER_MODE_NORMAL || !func || !spurs || !priority) {
        return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return finish(kMusic, music_initialize(func, user_data), call);
}

// RPCS3 cellMusicInitialize2SystemWorkload(mode, func, userData, spurs, priority, attr).
s32 cellMusicInitialize2SystemWorkload(PPUContext&, s32 mode, u32 func, u32 user_data, u32 spurs, u32 priority,
    u32 attr)
{
    const std::string call = std::format("cellMusicInitialize2SystemWorkload(mode {}, func {:#x}, userData {:#x}, "
                                         "spurs {:#x}, priority {:#x}, attr {:#x})",
        mode, func, user_data, spurs, priority, attr);
    if (mode != CELL_MUSIC_PLAYER_MODE_NORMAL || !func || !spurs || !priority) {
        return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return finish(kMusic, music_initialize(func, user_data), call);
}

// RPCS3 cellMusicFinalize(2): FINALIZE_RESULT to the callback, if there is one (RPCS3 keeps it afterwards).
s32 music_finalize(std::string_view name)
{
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    if (m.func) queue_callback(m.func, CELL_MUSIC_EVENT_FINALIZE_RESULT, lv2::CELL_OK, m.user_data);
    return finish(kMusic, lv2::CELL_OK, std::format("{}()", name));
}

s32 cellMusicFinalize(PPUContext&) { return music_finalize("cellMusicFinalize"); }
s32 cellMusicFinalize2(PPUContext&) { return music_finalize("cellMusicFinalize2"); }

// RPCS3 cell_music_select_contents: the media list dialog, cancelled without RPCS3's native UI (none here).
s32 music_select_contents(const std::string& call)
{
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    if (!m.func) return finish(kMusic, error(CELL_MUSIC_ERROR_GENERIC), call);
    queue_callback(m.func, CELL_MUSIC_EVENT_SELECT_CONTENTS_RESULT, CELL_MUSIC_CANCELED, m.user_data);
    return finish(kMusic, lv2::CELL_OK, call + ": no media dialog, cancelled");
}

s32 cellMusicSelectContents(PPUContext&, u32 container)
{
    return music_select_contents(std::format("cellMusicSelectContents(container {:#x})", container));
}

s32 cellMusicSelectContents2(PPUContext&) { return music_select_contents("cellMusicSelectContents2()"); }

// RPCS3 cellMusicGetSelectionContext(2): only after a selection, which never happens here.
s32 music_get_context(std::string_view name, u32 context)
{
    const std::string call = std::format("{}({:#x})", name, context);
    if (!context) return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    return finish(kMusic, error(CELL_MUSIC_ERROR_NO_ACTIVE_CONTENT), call);
}

s32 cellMusicGetSelectionContext(PPUContext&, u32 context)
{
    return music_get_context("cellMusicGetSelectionContext", context);
}

s32 cellMusicGetSelectionContext2(PPUContext&, u32 context)
{
    return music_get_context("cellMusicGetSelectionContext2", context);
}

// RPCS3 cellMusicSetSelectionContext(2): the context is read now and tried at the callback, which hears
// SET_SELECTION_CONTEXT_RESULT with INVALID_CONTEXT: it names no playlist pspocket has.
s32 music_set_context(PPUContext& ctx, std::string_view name, u32 context)
{
    const std::string call = std::format("{}({:#x})", name, context);
    if (!context) return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    if (!m.func) return finish(kMusic, error(CELL_MUSIC_ERROR_GENERIC), call);
    if (!usable(ctx, context, kContextSize, std::string(name) + ": context")) {
        return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    const std::string hash = context_hash(guest_base(ctx) + context);
    queue_callback(m.func, CELL_MUSIC_EVENT_SET_SELECTION_CONTEXT_RESULT, CELL_MUSIC_ERROR_INVALID_CONTEXT,
        m.user_data);
    return finish(kMusic, lv2::CELL_OK,
        call + (hash.empty() ? std::string(": not a selection context") : std::format(": no playlist \"{}\"", hash)));
}

s32 cellMusicSetSelectionContext(PPUContext& ctx, u32 context)
{
    return music_set_context(ctx, "cellMusicSetSelectionContext", context);
}

s32 cellMusicSetSelectionContext2(PPUContext& ctx, u32 context)
{
    return music_set_context(ctx, "cellMusicSetSelectionContext2", context);
}

// RPCS3 cellMusicGetContentsId(2): only after a selection.
s32 music_get_contents_id(std::string_view name, u32 contents_id)
{
    const std::string call = std::format("{}({:#x})", name, contents_id);
    if (!contents_id) return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    return finish(kMusic, error(CELL_MUSIC_ERROR_NO_ACTIVE_CONTENT), call);
}

s32 cellMusicGetContentsId(PPUContext&, u32 contents_id)
{
    return music_get_contents_id("cellMusicGetContentsId", contents_id);
}

s32 cellMusicGetContentsId2(PPUContext&, u32 contents_id)
{
    return music_get_contents_id("cellMusicGetContentsId2", contents_id);
}

// RPCS3 cellMusicSetPlaybackCommand(2)(command, param): STOP to FASTREVERSE; with nothing selected the command fails
// with ERROR_GENERIC, returned and also given to the callback (SET_PLAYBACK_COMMAND_RESULT).
s32 music_playback_command(std::string_view name, s32 command, u32 param)
{
    const std::string call = std::format("{}(command {}, param {:#x})", name, command, param);
    if (command < CELL_MUSIC_PB_CMD_STOP || command > CELL_MUSIC_PB_CMD_FASTREVERSE) {
        return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    if (!m.func) return finish(kMusic, error(CELL_MUSIC_ERROR_GENERIC), call + ": not initialized");
    queue_callback(m.func, CELL_MUSIC_EVENT_SET_PLAYBACK_COMMAND_RESULT, CELL_MUSIC_ERROR_GENERIC, m.user_data);
    return finish(kMusic, error(CELL_MUSIC_ERROR_GENERIC), call + ": no music selected");
}

s32 cellMusicSetPlaybackCommand(PPUContext&, s32 command, u32 param)
{
    return music_playback_command("cellMusicSetPlaybackCommand", command, param);
}

s32 cellMusicSetPlaybackCommand2(PPUContext&, s32 command, u32 param)
{
    return music_playback_command("cellMusicSetPlaybackCommand2", command, param);
}

// RPCS3 cellMusicGetPlaybackStatus(2): the player's status, STOP since nothing ever plays.
s32 music_get_status(PPUContext& ctx, std::string_view name, u32 status)
{
    const std::string call = std::format("{}({:#x})", name, status);
    if (!status || !usable(ctx, status, 4, name)) return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    ps_write32(guest_base(ctx), status, CELL_MUSIC_PB_STATUS_STOP);
    return finish(kMusic, lv2::CELL_OK, call);
}

s32 cellMusicGetPlaybackStatus(PPUContext& ctx, u32 status)
{
    return music_get_status(ctx, "cellMusicGetPlaybackStatus", status);
}

s32 cellMusicGetPlaybackStatus2(PPUContext& ctx, u32 status)
{
    return music_get_status(ctx, "cellMusicGetPlaybackStatus2", status);
}

// RPCS3 cellMusicSetVolume(2)(float level, in f1): clamped to 0..1 and kept; SET_VOLUME_RESULT to the callback.
s32 music_set_volume(PPUContext& ctx, std::string_view name)
{
    const f32 level = std::clamp(static_cast<f32>(ctx.fpr[1]), 0.0f, 1.0f);
    const std::string call = std::format("{}({})", name, ctx.fpr[1]);
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    if (!m.func) return finish(kMusic, error(CELL_MUSIC_ERROR_GENERIC), call);
    m.volume = level;
    queue_callback(m.func, CELL_MUSIC_EVENT_SET_VOLUME_RESULT, lv2::CELL_OK, m.user_data);
    return finish(kMusic, lv2::CELL_OK, call);
}

s32 cellMusicSetVolume(PPUContext& ctx) { return music_set_volume(ctx, "cellMusicSetVolume"); }
s32 cellMusicSetVolume2(PPUContext& ctx) { return music_set_volume(ctx, "cellMusicSetVolume2"); }

// RPCS3 cellMusicGetVolume(2)(float* level): the player's volume, without the library (RPCS3 reads its handler).
s32 music_get_volume(PPUContext& ctx, std::string_view name, u32 level)
{
    const std::string call = std::format("{}({:#x})", name, level);
    if (!level || !usable(ctx, level, 4, name)) return finish(kMusic, error(CELL_MUSIC_ERROR_PARAM), call);
    MusicState& m = music();
    std::lock_guard lock(m.mutex);
    ps_write32(guest_base(ctx), level, std::bit_cast<u32>(m.volume));
    return finish(kMusic, lv2::CELL_OK, call);
}

s32 cellMusicGetVolume(PPUContext& ctx, u32 level) { return music_get_volume(ctx, "cellMusicGetVolume", level); }
s32 cellMusicGetVolume2(PPUContext& ctx, u32 level) { return music_get_volume(ctx, "cellMusicGetVolume2", level); }

// ----------------------------------------------------------------------------------------- cellMusicDecode(2)

// RPCS3 music_decode (and music_decode2): the callback, the selection context's hash, the decode status and command,
// and its decoder's error flag. A START decodes the selection's playlist, which is empty here: RPCS3's decoder thread
// stops at once with an error ("Can not play empty playlist") until the next STOP, NEXT or PREV clears it.
struct Decoder {
    u32 func = 0, user_data = 0;
    std::string hash;
    s32 status = CELL_MUSIC_DECODE_STATUS_DORMANT, command = CELL_MUSIC_DECODE_CMD_STOP;
    bool failed = false;
    s32 speed = CELL_MUSIC_DECODE2_SPEED_MAX; // cellMusicDecode2 only
};

struct DecodeState {
    std::mutex mutex;
    Decoder v1, v2;
};

Decoder& decoder(bool v2, std::unique_lock<std::mutex>& lock)
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kDecode, "cellMusicDecode called without an HLE process");
    DecodeState& s = p->state<DecodeState>();
    lock = std::unique_lock(s.mutex);
    return v2 ? s.v2 : s.v1;
}

std::string_view decode_name(bool v2, std::string_view v1_name, std::string_view v2_name)
{
    return v2 ? v2_name : v1_name;
}

// The initialize functions after their checks.
s32 decode_initialize(bool v2, const std::string& call, u32 func, u32 user_data, s32 speed = -1)
{
    std::unique_lock<std::mutex> lock;
    Decoder& d = decoder(v2, lock);
    d.func = func;
    d.user_data = user_data;
    if (speed >= 0) d.speed = speed;
    queue_callback(func, CELL_MUSIC_EVENT_INITIALIZE_RESULT, lv2::CELL_OK, user_data);
    return finish(kDecode, lv2::CELL_OK, call);
}

// RPCS3 cellMusicDecodeInitialize(mode, container, spuPriority, func, userData).
s32 cellMusicDecodeInitialize(PPUContext&, s32 mode, u32 container, s32 spu_priority, u32 func, u32 user_data)
{
    const std::string call = std::format("cellMusicDecodeInitialize(mode {}, container {:#x}, spuPriority {}, func "
                                         "{:#x}, userData {:#x})",
        mode, container, spu_priority, func, user_data);
    if (mode != 0 || spu_priority < 16 || spu_priority > 255 || !func) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return decode_initialize(false, call, func, user_data);
}

// RPCS3 cellMusicDecodeInitializeSystemWorkload(mode, container, func, userData, spuUsageRate, spurs, priority, attr):
// a usage rate of 1 to 100 percent.
s32 cellMusicDecodeInitializeSystemWorkload(PPUContext&, s32 mode, u32 container, u32 func, u32 user_data,
    s32 spu_usage_rate, u32 spurs, u32 priority, u32 attr)
{
    const std::string call = std::format("cellMusicDecodeInitializeSystemWorkload(mode {}, container {:#x}, func "
                                         "{:#x}, userData {:#x}, spuUsageRate {}, spurs {:#x}, priority {:#x}, attr "
                                         "{:#x})",
        mode, container, func, user_data, spu_usage_rate, spurs, priority, attr);
    if (mode != 0 || !func || spu_usage_rate < 1 || spu_usage_rate > 100 || !spurs || !priority) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return decode_initialize(false, call, func, user_data);
}

// RPCS3 cellMusicDecodeInitialize2(mode, container, spuPriority, func, userData, speed, bufSize): a buffer of at
// least 448 KB and a speed of MAX or 2.
s32 cellMusicDecodeInitialize2(PPUContext&, s32 mode, u32 container, s32 spu_priority, u32 func, u32 user_data,
    s32 speed, s32 buf_size)
{
    const std::string call = std::format("cellMusicDecodeInitialize2(mode {}, container {:#x}, spuPriority {}, func "
                                         "{:#x}, userData {:#x}, speed {}, bufSize {:#x})",
        mode, container, spu_priority, func, user_data, speed, buf_size);
    if (mode != 0 || spu_priority < 16 || spu_priority > 255 || buf_size < CELL_MUSIC_DECODE2_MIN_BUFFER_SIZE ||
        !func || (speed != CELL_MUSIC_DECODE2_SPEED_MAX && speed != CELL_MUSIC_DECODE2_SPEED_2)) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return decode_initialize(true, call, func, user_data, speed);
}

// RPCS3 cellMusicDecodeInitialize2SystemWorkload(mode, container, func, userData, spuUsageRate, bufSize, spurs,
// priority, attr).
s32 cellMusicDecodeInitialize2SystemWorkload(PPUContext&, s32 mode, u32 container, u32 func, u32 user_data,
    s32 spu_usage_rate, s32 buf_size, u32 spurs, u32 priority, u32 attr)
{
    const std::string call = std::format("cellMusicDecodeInitialize2SystemWorkload(mode {}, container {:#x}, func "
                                         "{:#x}, userData {:#x}, spuUsageRate {}, bufSize {:#x}, spurs {:#x}, priority "
                                         "{:#x}, attr {:#x})",
        mode, container, func, user_data, spu_usage_rate, buf_size, spurs, priority, attr);
    if (mode != 0 || !func || spu_usage_rate < 1 || spu_usage_rate > 100 ||
        buf_size < CELL_MUSIC_DECODE2_MIN_BUFFER_SIZE || !spurs || !priority) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    return decode_initialize(true, call, func, user_data);
}

// RPCS3 cellMusicDecodeFinalize(2): the decoder stops and goes dormant; FINALIZE_RESULT to the callback, if any.
s32 decode_finalize(bool v2)
{
    std::unique_lock<std::mutex> lock;
    Decoder& d = decoder(v2, lock);
    d.failed = false;
    d.status = CELL_MUSIC_DECODE_STATUS_DORMANT;
    d.command = CELL_MUSIC_DECODE_CMD_STOP;
    if (d.func) queue_callback(d.func, CELL_MUSIC_EVENT_FINALIZE_RESULT, lv2::CELL_OK, d.user_data);
    return finish(kDecode, lv2::CELL_OK,
        std::format("{}()", decode_name(v2, "cellMusicDecodeFinalize", "cellMusicDecodeFinalize2")));
}

s32 cellMusicDecodeFinalize(PPUContext&) { return decode_finalize(false); }
s32 cellMusicDecodeFinalize2(PPUContext&) { return decode_finalize(true); }

// RPCS3 cell_music_decode_select_contents: the media list dialog, cancelled (no native UI).
s32 decode_select_contents(bool v2)
{
    const std::string call =
        std::format("{}()", decode_name(v2, "cellMusicDecodeSelectContents", "cellMusicDecodeSelectContents2"));
    std::unique_lock<std::mutex> lock;
    Decoder& d = decoder(v2, lock);
    if (!d.func) return finish(kDecode, error(CELL_MUSIC_ERROR_GENERIC), call);
    queue_callback(d.func, CELL_MUSIC_EVENT_SELECT_CONTENTS_RESULT, CELL_MUSIC_CANCELED, d.user_data);
    return finish(kDecode, lv2::CELL_OK, call + ": no media dialog, cancelled");
}

s32 cellMusicDecodeSelectContents(PPUContext&) { return decode_select_contents(false); }
s32 cellMusicDecodeSelectContents2(PPUContext&) { return decode_select_contents(true); }

// RPCS3 music_decode::set_decode_command: STOP stops the decoder (dormant); START starts it (decoding), and with an
// empty playlist it fails at once; NEXT and PREV stop it and step the playlist, which has no next track
// (ERROR_NO_MORE_CONTENT, dormant). The command's result goes to the callback (SET_DECODE_COMMAND_RESULT); the call
// itself returns CELL_OK after its checks.
s32 decode_set_command(bool v2, s32 command)
{
    const std::string call = std::format("{}({})",
        decode_name(v2, "cellMusicDecodeSetDecodeCommand", "cellMusicDecodeSetDecodeCommand2"), command);
    if (command < CELL_MUSIC_DECODE_CMD_STOP || command > CELL_MUSIC_DECODE_CMD_PREV) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    std::unique_lock<std::mutex> lock;
    Decoder& d = decoder(v2, lock);
    if (!d.func) return finish(kDecode, error(CELL_MUSIC_ERROR_GENERIC), call);
    d.command = command;
    s32 result = lv2::CELL_OK;
    switch (command) {
    case CELL_MUSIC_DECODE_CMD_STOP:
        d.failed = false;
        d.status = CELL_MUSIC_DECODE_STATUS_DORMANT;
        break;
    case CELL_MUSIC_DECODE_CMD_START:
        d.status = CELL_MUSIC_DECODE_STATUS_DECODING;
        d.failed = true; // the empty playlist
        break;
    default: // NEXT, PREV
        d.failed = false;
        d.status = CELL_MUSIC_DECODE_STATUS_DORMANT;
        result = error(CELL_MUSIC_ERROR_NO_MORE_CONTENT);
        break;
    }
    queue_callback(d.func, CELL_MUSIC_DECODE_EVENT_SET_DECODE_COMMAND_RESULT, static_cast<u32>(result), d.user_data);
    return finish(kDecode, lv2::CELL_OK, call + std::format(": {} to the callback", result_name(result)));
}

s32 cellMusicDecodeSetDecodeCommand(PPUContext&, s32 command) { return decode_set_command(false, command); }
s32 cellMusicDecodeSetDecodeCommand2(PPUContext&, s32 command) { return decode_set_command(true, command); }

// RPCS3 cellMusicDecodeGetDecodeStatus(2).
s32 decode_get_status(PPUContext& ctx, bool v2, u32 status)
{
    const std::string_view name = decode_name(v2, "cellMusicDecodeGetDecodeStatus", "cellMusicDecodeGetDecodeStatus2");
    const std::string call = std::format("{}({:#x})", name, status);
    if (!status || !usable(ctx, status, 4, name)) return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    std::unique_lock<std::mutex> lock;
    Decoder& d = decoder(v2, lock);
    ps_write32(guest_base(ctx), status, static_cast<u32>(d.status));
    return finish(kDecode, lv2::CELL_OK, call + std::format(": status {}", d.status));
}

s32 cellMusicDecodeGetDecodeStatus(PPUContext& ctx, u32 status) { return decode_get_status(ctx, false, status); }
s32 cellMusicDecodeGetDecodeStatus2(PPUContext& ctx, u32 status) { return decode_get_status(ctx, true, status); }

// RPCS3 cell_music_decode_read(buf, startTime, reqSize, readSize, position): the outputs are cleared (position NONE,
// nothing read, start time 0), then the decoder has failed (ERROR_DECODE_FAILURE, after a START) or holds no PCM
// (ERROR_NO_LPCM_DATA).
s32 decode_read(PPUContext& ctx, bool v2, u32 buf, u32 start_time, u64 req_size, u32 read_size, u32 position)
{
    const std::string call =
        std::format("{}(buf {:#x}, startTime {:#x}, reqSize {:#x}, readSize {:#x}, position {:#x})",
            decode_name(v2, "cellMusicDecodeRead", "cellMusicDecodeRead2"), buf, start_time, req_size, read_size,
            position);
    if (!buf || !start_time || !position || !req_size || !read_size) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    if (!usable(ctx, position, 4, call) || !usable(ctx, read_size, 8, call) || !usable(ctx, start_time, 4, call)) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    u8* base = guest_base(ctx);
    ps_write32(base, position, CELL_MUSIC_DECODE_POSITION_NONE);
    ps_write64(base, read_size, 0);
    ps_write32(base, start_time, 0);
    std::unique_lock<std::mutex> lock;
    const Decoder& d = decoder(v2, lock);
    return finish(kDecode,
        error(d.failed ? CELL_MUSIC_DECODE_ERROR_DECODE_FAILURE : CELL_MUSIC_DECODE_ERROR_NO_LPCM_DATA), call);
}

s32 cellMusicDecodeRead(PPUContext& ctx, u32 buf, u32 start_time, u64 req_size, u32 read_size, u32 position)
{
    return decode_read(ctx, false, buf, start_time, req_size, read_size, position);
}

s32 cellMusicDecodeRead2(PPUContext& ctx, u32 buf, u32 start_time, u64 req_size, u32 read_size, u32 position)
{
    return decode_read(ctx, true, buf, start_time, req_size, read_size, position);
}

// RPCS3 cellMusicDecodeGetSelectionContext(2): the context as it stands, "SUS\0" and its hash (empty unless a
// context was set), whether or not it is valid.
s32 decode_get_context(PPUContext& ctx, bool v2, u32 context)
{
    const std::string_view name =
        decode_name(v2, "cellMusicDecodeGetSelectionContext", "cellMusicDecodeGetSelectionContext2");
    const std::string call = std::format("{}({:#x})", name, context);
    if (!context || !usable(ctx, context, kContextSize, name)) {
        return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    }
    std::unique_lock<std::mutex> lock;
    const Decoder& d = decoder(v2, lock);
    u8* out = guest_base(ctx) + context;
    std::memset(out, 0, kContextSize);
    std::memcpy(out, kContextMagic, sizeof(kContextMagic));
    std::memcpy(out + sizeof(kContextMagic), d.hash.data(), d.hash.size());
    return finish(kDecode, lv2::CELL_OK, call);
}

s32 cellMusicDecodeGetSelectionContext(PPUContext& ctx, u32 context) { return decode_get_context(ctx, false, context); }
s32 cellMusicDecodeGetSelectionContext2(PPUContext& ctx, u32 context) { return decode_get_context(ctx, true, context); }

// RPCS3 cellMusicDecodeSetSelectionContext(2): tried at once (a valid-looking hash is kept, as RPCS3's set keeps it);
// the callback hears SET_SELECTION_CONTEXT_RESULT with INVALID_CONTEXT, since no playlist goes with it.
s32 decode_set_context(PPUContext& ctx, bool v2, u32 context)
{
    const std::string_view name =
        decode_name(v2, "cellMusicDecodeSetSelectionContext", "cellMusicDecodeSetSelectionContext2");
    const std::string call = std::format("{}({:#x})", name, context);
    if (!context) return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    std::unique_lock<std::mutex> lock;
    Decoder& d = decoder(v2, lock);
    if (!d.func) return finish(kDecode, error(CELL_MUSIC_ERROR_GENERIC), call);
    if (!usable(ctx, context, kContextSize, name)) return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    if (std::string hash = context_hash(guest_base(ctx) + context); !hash.empty()) d.hash = std::move(hash);
    queue_callback(d.func, CELL_MUSIC_DECODE_EVENT_SET_SELECTION_CONTEXT_RESULT, CELL_MUSIC_ERROR_INVALID_CONTEXT,
        d.user_data);
    return finish(kDecode, lv2::CELL_OK, call + ": no playlist for it");
}

s32 cellMusicDecodeSetSelectionContext(PPUContext& ctx, u32 context) { return decode_set_context(ctx, false, context); }
s32 cellMusicDecodeSetSelectionContext2(PPUContext& ctx, u32 context) { return decode_set_context(ctx, true, context); }

// RPCS3 cellMusicDecodeGetContentsId(2) (music_selection_context::find_content_id): the selection's content among
// the library's, which is empty: CELL_SEARCH_ERROR_CONTENT_NOT_FOUND.
s32 decode_get_contents_id(bool v2, u32 contents_id)
{
    const std::string call = std::format("{}({:#x})",
        decode_name(v2, "cellMusicDecodeGetContentsId", "cellMusicDecodeGetContentsId2"), contents_id);
    if (!contents_id) return finish(kDecode, error(CELL_MUSIC_ERROR_PARAM), call);
    return finish(kDecode, error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND), call);
}

s32 cellMusicDecodeGetContentsId(PPUContext&, u32 contents_id) { return decode_get_contents_id(false, contents_id); }
s32 cellMusicDecodeGetContentsId2(PPUContext&, u32 contents_id) { return decode_get_contents_id(true, contents_id); }

} // namespace

void register_cellMusic(Registry& r)
{
    r.add("cellMusicUtility", {
        PS_HLE_FUNC(0x0B461648, cellMusicGetSelectionContext),
        PS_HLE_FUNC(0x2BDC5D6B, cellMusicSetSelectionContext2),
        PS_HLE_FUNC(0x4014C246, cellMusicSetVolume2),
        PS_HLE_FUNC(0x4C188CAA, cellMusicGetContentsId),
        PS_HLE_FUNC(0x5BFF31BF, cellMusicSetSelectionContext),
        PS_HLE_FUNC(0x61865281, cellMusicInitialize2SystemWorkload),
        PS_HLE_FUNC(0x648B7611, cellMusicGetPlaybackStatus2),
        PS_HLE_FUNC(0x6674DE2D, cellMusicGetContentsId2),
        PS_HLE_FUNC(0x6F2104F3, cellMusicFinalize),
        PS_HLE_FUNC(0x72876546, cellMusicInitializeSystemWorkload),
        PS_HLE_FUNC(0x72EC14B5, cellMusicInitialize),
        PS_HLE_FUNC(0x7BE4DC31, cellMusicFinalize2),
        PS_HLE_FUNC(0x8793EF97, cellMusicGetSelectionContext2),
        PS_HLE_FUNC(0x8AA188E3, cellMusicGetVolume),
        PS_HLE_FUNC(0x95F7D9D9, cellMusicGetPlaybackStatus),
        PS_HLE_FUNC(0x98947A6E, cellMusicSetPlaybackCommand2),
        PS_HLE_FUNC(0xA0661626, cellMusicSetPlaybackCommand),
        PS_HLE_FUNC(0xAD04CDDD, cellMusicSelectContents2),
        PS_HLE_FUNC(0xB2336BA7, cellMusicSelectContents),
        PS_HLE_FUNC(0xBE50B11E, cellMusicInitialize2),
        PS_HLE_FUNC(0xE74CE7BD, cellMusicSetVolume),
        PS_HLE_FUNC(0xF9073A24, cellMusicGetVolume2),
    });
    r.add("cellMusicDecodeUtility", {
        PS_HLE_FUNC(0xD55DBC11, cellMusicDecodeInitialize),
        PS_HLE_FUNC(0x84F154B2, cellMusicDecodeInitializeSystemWorkload),
        PS_HLE_FUNC(0xA8615DC8, cellMusicDecodeFinalize),
        PS_HLE_FUNC(0xF24CB963, cellMusicDecodeSelectContents),
        PS_HLE_FUNC(0x066BB1CF, cellMusicDecodeSetDecodeCommand),
        PS_HLE_FUNC(0x5AF74C50, cellMusicDecodeGetDecodeStatus),
        PS_HLE_FUNC(0xA881B744, cellMusicDecodeRead),
        PS_HLE_FUNC(0xDBF70550, cellMusicDecodeGetSelectionContext),
        PS_HLE_FUNC(0xB84F5C81, cellMusicDecodeSetSelectionContext),
        PS_HLE_FUNC(0x58AB1999, cellMusicDecodeGetContentsId),
        PS_HLE_FUNC(0xF80E31E5, cellMusicDecodeInitialize2),
        PS_HLE_FUNC(0xB641168E, cellMusicDecodeInitialize2SystemWorkload),
        PS_HLE_FUNC(0x4AEF2877, cellMusicDecodeFinalize2),
        PS_HLE_FUNC(0xC22563C4, cellMusicDecodeSelectContents2),
        PS_HLE_FUNC(0x2EF701EC, cellMusicDecodeSetDecodeCommand2),
        PS_HLE_FUNC(0x1576E4F2, cellMusicDecodeGetDecodeStatus2),
        PS_HLE_FUNC(0xB2D054DF, cellMusicDecodeRead2),
        PS_HLE_FUNC(0xF91639F9, cellMusicDecodeGetSelectionContext2),
        PS_HLE_FUNC(0x491D6BA5, cellMusicDecodeSetSelectionContext2),
        PS_HLE_FUNC(0x25EA7AC0, cellMusicDecodeGetContentsId2),
    });
}

} // namespace pspocket::hle
