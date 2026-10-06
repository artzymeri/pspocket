// cellRec: the PS3's game video recording (a game records its own screen and sound to a movie on the HDD).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellRec.cpp/.h (rec_info, rec_param, set_video_params, the encoder's use).
//
// RPCS3 records through FFmpeg's encoders and muxers. pspocket's FFmpeg build has decoders only (D-181), so nothing
// can be encoded: cellRecStart answers through the game's callback with CELL_REC_STATUS_ERR and
// CELL_REC_ERROR_FILE_OPEN, RPCS3's answer when its encoder fails to start, and the recorder stays stopped. Everything
// else is RPCS3's: cellRecOpen's checks in RPCS3's order (the formats and their pairings, every option, the path on
// /dev_hdd0 or /dev_hdd1, the memory container), the input buffers a game can fill itself (allocated from the main
// area when a container is given, cellRecQueryMemSize's sizes), the open and close events through the callback at the
// game's next cellSysutilCheckCallback, cellRecGetInfo's values (the input frame's size for the format, for a 720p 16:9
// output, pspocket's) and cellRecSetInfo's metadata. Closing reports CLOSE, or ERR with FILE_NO_DATA when the scene's
// end lies past what was recorded (nothing), as RPCS3 checks it.
//
// Differences from RPCS3: pointers and strings are checked before use (D-042: logged and refused with
// CELL_REC_ERROR_INVALID_VALUE, where RPCS3 faults); the input buffers are allocated after the checks, so a refused
// open leaves none behind (RPCS3 allocates them first and keeps them).

#include <cmath>
#include <format>
#include <mutex>
#include <optional>
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

constexpr std::string_view kChan = "cellRec";

enum : u32 {
    CELL_REC_ERROR_OUT_OF_MEMORY = 0x8002C501,
    CELL_REC_ERROR_INVALID_VALUE = 0x8002C503,
    CELL_REC_ERROR_FILE_OPEN = 0x8002C504,
    CELL_REC_ERROR_INVALID_STATE = 0x8002C506,
    CELL_REC_ERROR_FILE_NO_DATA = 0x8002C507,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

s32 finish(s32 r, const std::string& call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, kChan, "{}: {}", call,
        r < 0 ? std::format("{:#010x}", static_cast<u32>(r)) : std::string("CELL_OK"));
    return r;
}

constexpr s32 CELL_REC_STATUS_OPEN = 1, CELL_REC_STATUS_CLOSE = 4, CELL_REC_STATUS_ERR = 10;
constexpr u32 SYS_MEMORY_CONTAINER_ID_INVALID = 0xFFFFFFFF, CELL_REC_MAX_PATH_LEN = 1023;
constexpr s32 VIDEO_TYPE_AVC_MP = 0x1000, VIDEO_TYPE_AVC_BL = 0x2000, VIDEO_TYPE_MJPEG = 0x3000,
              VIDEO_TYPE_M4HD = 0x4000;
constexpr s32 VIDEO_QUALITY_0 = 0x000, VIDEO_QUALITY_1 = 0x100, VIDEO_QUALITY_2 = 0x200, VIDEO_QUALITY_3 = 0x300,
              VIDEO_QUALITY_4 = 0x400, VIDEO_QUALITY_5 = 0x500, VIDEO_QUALITY_6 = 0x600;
constexpr s32 CELL_REC_PARAM_VIDEO_INPUT_DISABLE = 0, CELL_REC_PARAM_VIDEO_INPUT_YUV420PLANAR_16_9 = 5,
              CELL_REC_PARAM_AUDIO_INPUT_DISABLE = 0, CELL_REC_PARAM_REDUCE_MEMSIZE_DISABLE = 0;
// Options (CellRecOption: be s32 option, then an 8-byte union at 8; 16 bytes).
enum : s32 {
    CELL_REC_OPTION_PPU_THREAD_PRIORITY = 1,
    CELL_REC_OPTION_SPU_THREAD_PRIORITY = 2,
    CELL_REC_OPTION_CAPTURE_PRIORITY = 3,
    CELL_REC_OPTION_USE_SYSTEM_SPU = 4,
    CELL_REC_OPTION_FIT_TO_YOUTUBE = 5,
    CELL_REC_OPTION_XMB_BGM = 6,
    CELL_REC_OPTION_RING_SEC = 7,
    CELL_REC_OPTION_MPEG4_FAST_ENCODE = 8,
    CELL_REC_OPTION_VIDEO_INPUT = 9,
    CELL_REC_OPTION_AUDIO_INPUT = 10,
    CELL_REC_OPTION_AUDIO_INPUT_MIX_VOL = 11,
    CELL_REC_OPTION_REDUCE_MEMSIZE = 12,
    CELL_REC_OPTION_SHOW_XMB = 13,
    CELL_REC_OPTION_METADATA_FILENAME = 14,
    CELL_REC_OPTION_SPURS = 15,
};
constexpr s32 CELL_REC_INFO_VIDEO_INPUT_ADDR = 0, CELL_REC_INFO_VIDEO_INPUT_WIDTH = 1,
              CELL_REC_INFO_VIDEO_INPUT_PITCH = 2, CELL_REC_INFO_VIDEO_INPUT_HEIGHT = 3,
              CELL_REC_INFO_AUDIO_INPUT_ADDR = 4, CELL_REC_INFO_MOVIE_TIME_MSEC = 5,
              CELL_REC_INFO_SPURS_SYSTEMWORKLOAD_ID = 6;
constexpr s32 CELL_REC_SETINFO_MOVIE_START_TIME_MSEC = 100, CELL_REC_SETINFO_MOVIE_END_TIME_MSEC = 101,
              CELL_REC_SETINFO_MOVIE_META = 200, CELL_REC_SETINFO_SCENE_META = 201;
constexpr u32 CELL_REC_MOVIE_META_GAME_TITLE_LEN = 128, CELL_REC_MOVIE_META_MOVIE_TITLE_LEN = 128,
              CELL_REC_MOVIE_META_DESCRIPTION_LEN = 384, CELL_REC_MOVIE_META_USERDATA_LEN = 64,
              CELL_REC_SCENE_META_TYPE_CLIP_USER = 2, CELL_REC_SCENE_META_TITLE_LEN = 128,
              CELL_REC_SCENE_META_TAG_NUM = 6, CELL_REC_SCENE_META_TAG_LEN = 64;
constexpr u32 kParamSize = 16, kOptionSize = 16, kSceneMetaSize = 40, kMovieMetaSize = 16;

// RPCS3 rec_state.
enum class RecState : u8 { Closed, Stopped, Started };

// RPCS3 rec_info, without its encoder.
struct Rec {
    std::mutex mutex;
    RecState state = RecState::Closed;
    u32 callback = 0, user_data = 0;
    u32 video_input_buffer = 0, audio_input_buffer = 0;
    u32 width = 0, height = 0, pitch = 0; // the input frame (RPCS3 input_format)
    s32 video_input = CELL_REC_PARAM_VIDEO_INPUT_DISABLE;
    u64 scene_start_ms = 0, scene_end_ms = 0;
    std::string filename, metadata_filename, game_title, movie_title;
};

Rec& rec()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellRec called without an HLE process");
    return p->state<Rec>();
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// A guest string of at most `max` bytes before its NUL (nullopt, logged, when it leaves guest memory).
std::optional<std::string> guest_text(PPUContext& ctx, u32 addr, u32 max, std::string_view what)
{
    const GuestString s = read_cstr(thread_of(ctx), addr, max);
    if (!s.faulted) return s.text;
    log_call_site(ctx, guest_base(ctx), std::format("{} at {:#x} runs out of guest memory", what, addr));
    return std::nullopt;
}

bool known_video_format(s32 f)
{
    switch (f) {
    case 0x0000: case 0x0010: case 0x0100: case 0x0110: case 0x0200: case 0x0210: case 0x0220: case 0x0230:
    case 0x0240: case 0x1000: case 0x1010: case 0x1100: case 0x1110: case 0x1120: case 0x1130: case 0x2000:
    case 0x2010: case 0x2100: case 0x2110: case 0x2120: case 0x2130: case 0x3060: case 0x3160: case 0x3270:
    case 0x3670: case 0x3680: case 0x3690: case 0x3791: case 0x37A1: case 0x3790: case 0x37A0: case 0x4010:
    case 0x4110: case 0x4230: case 0x4240: case 0x4640: case 0x4660: case 0x4670: case 0x0310: return true;
    default: return false;
    }
}

// RPCS3 create_path: `dir`/`file`, at most 1023 bytes in all.
std::optional<std::string> make_path(std::string dir, const std::string& file)
{
    if (dir.size() + file.size() > CELL_REC_MAX_PATH_LEN) return std::nullopt;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    return dir + file;
}

// RPCS3 cellRecQueryMemSize(pParam): the memory a recording needs by video type and quality, plus 1 MB for an
// external input of MJPEG or M4HD (but HD720), less what REDUCE_MEMSIZE saves. `options` were checked to be guest
// memory.
u32 query_mem_size(u8* base, u32 param)
{
    if (!param) return 0x900000;
    const s32 fmt = static_cast<s32>(ps_read32(base, param));
    const s32 type = fmt & 0xF000, quality = fmt & 0xF00;
    u32 video = 0x600000, audio = 0x100000;
    switch (type) {
    case 0x0000:
        video = quality == VIDEO_QUALITY_0 || quality == VIDEO_QUALITY_3 ? 0x600000
                : quality == VIDEO_QUALITY_1                            ? 0x700000
                : quality == VIDEO_QUALITY_2                            ? 0x800000
                                                                        : 0x900000;
        break;
    case VIDEO_TYPE_AVC_BL:
    case VIDEO_TYPE_AVC_MP:
        video = quality == VIDEO_QUALITY_0 || quality == VIDEO_QUALITY_3 ? 0x800000 : 0x900000;
        break;
    case VIDEO_TYPE_M4HD:
        video = quality == VIDEO_QUALITY_0 || quality == VIDEO_QUALITY_1 ? 0x600000
                : quality == VIDEO_QUALITY_2                            ? 0x700000
                                                                        : 0x1000000;
        break;
    default:
        video = quality == VIDEO_QUALITY_0                               ? 0x300000
                : quality == VIDEO_QUALITY_1 || quality == VIDEO_QUALITY_2 ? 0x400000
                : quality == VIDEO_QUALITY_6                             ? 0x700000
                                                                         : 0xD00000;
        audio = 0;
        break;
    }
    const bool external_kind = type == VIDEO_TYPE_MJPEG || (type == VIDEO_TYPE_M4HD && quality != VIDEO_QUALITY_6);
    u32 external = 0, reduce = 0;
    const s32 count = static_cast<s32>(ps_read32(base, param + 8));
    const u32 options = ps_read32(base, param + 12);
    for (s32 i = 0; i < count; i++) {
        const u32 opt = options + kOptionSize * static_cast<u32>(i);
        const s32 value = static_cast<s32>(ps_read32(base, opt + 8));
        switch (static_cast<s32>(ps_read32(base, opt))) {
        case CELL_REC_OPTION_REDUCE_MEMSIZE:
            reduce = value == CELL_REC_PARAM_REDUCE_MEMSIZE_DISABLE                                     ? 0
                     : type == VIDEO_TYPE_M4HD && (quality == VIDEO_QUALITY_1 || quality == VIDEO_QUALITY_2) ? 0x200000
                                                                                                         : 0x300000;
            break;
        case CELL_REC_OPTION_VIDEO_INPUT:
        case CELL_REC_OPTION_AUDIO_INPUT: // RPCS3 reads the audio input field for both (the same word)
            if (value != CELL_REC_PARAM_AUDIO_INPUT_DISABLE && external_kind) external = 0x100000;
            break;
        case CELL_REC_OPTION_AUDIO_INPUT_MIX_VOL:
            if (external_kind) external = 0x100000;
            break;
        default: break;
        }
    }
    const u32 size = video + audio + external;
    return size > reduce ? size - reduce : 0;
}

// Whether the options of `param` (count at 8, array at 12) are guest memory.
bool options_usable(PPUContext& ctx, u32 param, std::string_view what)
{
    if (!usable(ctx, param, kParamSize, what)) return false;
    u8* base = guest_base(ctx);
    const s32 count = static_cast<s32>(ps_read32(base, param + 8));
    return count <= 0 || usable(ctx, ps_read32(base, param + 12), kOptionSize * static_cast<u32>(count), what);
}

// RPCS3 set_video_params: the input frame's size for the format's quality on a 720p 16:9 output (pspocket's); with no
// external video input, 1280x720.
void set_video_params(Rec& r, s32 fmt)
{
    switch (fmt & 0xF00) {
    case VIDEO_QUALITY_0:
        r.width = 368;
        r.height = 208;
        break;
    case VIDEO_QUALITY_1:
        r.width = 480;
        r.height = 272;
        break;
    case VIDEO_QUALITY_2:
        r.width = 640;
        r.height = 368;
        break;
    case VIDEO_QUALITY_3:
        r.width = 320;
        r.height = 240;
        break;
    case VIDEO_QUALITY_4:
    case VIDEO_QUALITY_5: break; // RPCS3's TODO: unchanged
    case VIDEO_QUALITY_6:
    default:
        r.width = 1280;
        r.height = 720;
        break;
    }
    if (r.video_input == CELL_REC_PARAM_VIDEO_INPUT_DISABLE || r.video_input > CELL_REC_PARAM_VIDEO_INPUT_YUV420PLANAR_16_9) {
        r.width = 1280;
        r.height = 720;
    }
    r.pitch = r.width * 4;
}

// RPCS3 cellRecOpen(pDirName, pFileName, pParam, container, cb, cbUserData).
s32 cellRecOpen(PPUContext& ctx, u32 dir, u32 file, u32 param, u32 container, u32 callback, u32 user_data)
{
    std::string call = std::format("cellRecOpen(dir {:#x}, file {:#x}, param {:#x}, container {:#x}, cb {:#x}, "
                                   "cbUserData {:#x})",
        dir, file, param, container, callback, user_data);
    Rec& r = rec();
    std::lock_guard lock(r.mutex);
    if (r.state != RecState::Closed) return finish(error(CELL_REC_ERROR_INVALID_STATE), call);
    if (!param || !dir || !file) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
    const std::optional<std::string> dir_name = guest_text(ctx, dir, CELL_REC_MAX_PATH_LEN + 1, "cellRecOpen: dir");
    const std::optional<std::string> file_name = guest_text(ctx, file, CELL_REC_MAX_PATH_LEN + 1, "cellRecOpen: file");
    if (!dir_name || !file_name || !options_usable(ctx, param, "cellRecOpen: param")) {
        return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
    }
    call = std::format("cellRecOpen(\"{}\", \"{}\")", *dir_name, *file_name);
    u8* base = guest_base(ctx);
    const u32 mem_size = query_mem_size(base, param);
    if (container == SYS_MEMORY_CONTAINER_ID_INVALID && mem_size != 0) {
        return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
    }
    const s32 video_fmt = static_cast<s32>(ps_read32(base, param));
    const s32 audio_fmt = static_cast<s32>(ps_read32(base, param + 4));
    if (!known_video_format(video_fmt)) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
    const s32 type = video_fmt & 0xF000;
    switch (audio_fmt) {
    case 0x0000: // AAC 96K, 128K, 64K: not with MJPEG or AVC main profile
    case 0x0001:
    case 0x0002:
        if (type == VIDEO_TYPE_AVC_MP || type == VIDEO_TYPE_MJPEG) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        break;
    case 0x1007: // u-law: not with AVC main profile
    case 0x1008:
        if (type == VIDEO_TYPE_AVC_MP) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        break;
    case 0x2007: // PCM: MJPEG only
    case 0x2008:
    case 0x2009:
        if (type != VIDEO_TYPE_MJPEG) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        break;
    default: return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
    }

    // The options, in order (RPCS3 resets its parameters first).
    s32 video_input = CELL_REC_PARAM_VIDEO_INPUT_DISABLE;
    std::string metadata_filename;
    bool abort_loop = false; // RPCS3 jumps to the callback part on a null metadata file name
    const s32 count = static_cast<s32>(ps_read32(base, param + 8));
    const u32 options = ps_read32(base, param + 12);
    for (s32 i = 0; i < count && !abort_loop; i++) {
        const u32 opt = options + kOptionSize * static_cast<u32>(i);
        const s32 option = static_cast<s32>(ps_read32(base, opt));
        const u32 value = ps_read32(base, opt + 8);
        switch (option) {
        case CELL_REC_OPTION_PPU_THREAD_PRIORITY:
            if (static_cast<s32>(value) > 0xBFF) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            break;
        case CELL_REC_OPTION_SPU_THREAD_PRIORITY:
            if (value - 0x10u > 0xEF) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            break;
        case CELL_REC_OPTION_VIDEO_INPUT:
            if ((value & 0xFF) > CELL_REC_PARAM_VIDEO_INPUT_YUV420PLANAR_16_9) {
                return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            }
            video_input = static_cast<s32>(value & 0xFF);
            break;
        case CELL_REC_OPTION_METADATA_FILENAME:
            if (value) {
                const std::optional<std::string> name =
                    guest_text(ctx, value, CELL_REC_MAX_PATH_LEN + 1, "cellRecOpen: metadata_filename");
                if (!name) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
                const std::optional<std::string> path = make_path(*dir_name, *name);
                if (!path) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
                metadata_filename = *path;
            } else {
                abort_loop = true;
            }
            break;
        case CELL_REC_OPTION_SPURS: {
            if (!value || !usable(ctx, value, 8, "cellRecOpen: pSpursParam") || !ps_read32(base, value)) {
                return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            }
            const s32 rate = static_cast<s32>(ps_read32(base, value + 4));
            if (rate < 1 || rate > 100) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            break;
        }
        case CELL_REC_OPTION_CAPTURE_PRIORITY:
        case CELL_REC_OPTION_USE_SYSTEM_SPU:
        case CELL_REC_OPTION_FIT_TO_YOUTUBE:
        case CELL_REC_OPTION_XMB_BGM:
        case CELL_REC_OPTION_RING_SEC:
        case CELL_REC_OPTION_MPEG4_FAST_ENCODE:
        case CELL_REC_OPTION_AUDIO_INPUT:
        case CELL_REC_OPTION_AUDIO_INPUT_MIX_VOL:
        case CELL_REC_OPTION_REDUCE_MEMSIZE:
        case CELL_REC_OPTION_SHOW_XMB:
        case 100: break; // kept by RPCS3 for its encoder; nothing to keep here
        default: PS_LOG_WARN(kChan, "cellRecOpen: unknown option {}", option); break;
        }
    }
    std::string filename;
    if (!abort_loop) {
        if (video_input != CELL_REC_PARAM_VIDEO_INPUT_DISABLE) {
            const bool yuv = video_input == CELL_REC_PARAM_VIDEO_INPUT_YUV420PLANAR_16_9;
            if ((type == VIDEO_TYPE_MJPEG || type == VIDEO_TYPE_M4HD) != yuv) {
                return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            }
        }
        const std::optional<std::string> path = make_path(*dir_name, *file_name);
        if (!path || (!path->starts_with("/dev_hdd0") && !path->starts_with("/dev_hdd1"))) {
            return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        }
        filename = *path;
    }
    if (!callback) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);

    // The input buffers a game may fill (RPCS3 allocates mem_size each, from the main area).
    HleProcess* process = HleProcess::current();
    for (u32* b : {&r.video_input_buffer, &r.audio_input_buffer}) {
        if (*b) process->main_memory().free(*b);
        *b = 0;
    }
    if (container != SYS_MEMORY_CONTAINER_ID_INVALID && mem_size) {
        r.video_input_buffer = process->main_memory().alloc(mem_size);
        r.audio_input_buffer = process->main_memory().alloc(mem_size);
        if (!r.video_input_buffer || !r.audio_input_buffer) return finish(error(CELL_REC_ERROR_OUT_OF_MEMORY), call);
    }
    r.callback = callback;
    r.user_data = user_data;
    r.video_input = video_input;
    r.filename = filename;
    r.metadata_filename = metadata_filename;
    r.scene_start_ms = r.scene_end_ms = 0;
    r.game_title.clear();
    r.movie_title.clear();
    set_video_params(r, video_fmt);
    sysutil_queue_call([](PPUContext& cb) -> s32 {
        Rec& s = rec();
        u32 func = 0, arg = 0;
        {
            std::lock_guard l(s.mutex);
            s.state = RecState::Stopped;
            func = s.callback;
            arg = s.user_data;
        }
        call_guest(cb, func, {static_cast<u64>(CELL_REC_STATUS_OPEN), lv2::CELL_OK, arg});
        return lv2::CELL_OK;
    });
    return finish(lv2::CELL_OK, call + std::format(": {:#x} bytes of input buffers each", mem_size));
}

// RPCS3 cellRecStart: the encoder starts at the callback; pspocket has none, so the game hears STATUS_ERR with
// FILE_OPEN and the recorder stays stopped.
s32 cellRecStart(PPUContext&)
{
    Rec& r = rec();
    std::lock_guard lock(r.mutex);
    if (r.state != RecState::Stopped) return finish(error(CELL_REC_ERROR_INVALID_STATE), "cellRecStart()");
    sysutil_queue_call([](PPUContext& cb) -> s32 {
        Rec& s = rec();
        u32 func = 0, arg = 0;
        {
            std::lock_guard l(s.mutex);
            func = s.callback;
            arg = s.user_data;
        }
        call_guest(cb, func,
            {static_cast<u64>(CELL_REC_STATUS_ERR), static_cast<u64>(static_cast<s64>(error(CELL_REC_ERROR_FILE_OPEN))),
                arg});
        return lv2::CELL_OK;
    });
    return finish(lv2::CELL_OK, "cellRecStart(): no video encoder, the callback hears FILE_OPEN");
}

// RPCS3 cellRecStop: only while recording, which never happens here.
s32 cellRecStop(PPUContext&)
{
    Rec& r = rec();
    std::lock_guard lock(r.mutex);
    if (r.state != RecState::Started) return finish(error(CELL_REC_ERROR_INVALID_STATE), "cellRecStop()");
    return finish(lv2::CELL_OK, "cellRecStop()");
}

// RPCS3 cellRecClose(isDiscard): at the callback the buffers go and the recorder closes; CLOSE, or ERR with
// FILE_NO_DATA when the scene's end lies past the last frame recorded (none: frame 0).
s32 cellRecClose(PPUContext&, s32 discard)
{
    const std::string call = std::format("cellRecClose({})", discard);
    Rec& r = rec();
    std::lock_guard lock(r.mutex);
    if (r.state == RecState::Closed) return finish(error(CELL_REC_ERROR_INVALID_STATE), call);
    sysutil_queue_call([discard](PPUContext& cb) -> s32 {
        Rec& s = rec();
        u32 func = 0, arg = 0;
        bool valid = true;
        {
            std::lock_guard l(s.mutex);
            if (!discard) {
                const auto pts = [](u64 ms) { return static_cast<s64>(std::round(static_cast<f64>(ms) * 30 / 1000.0)); };
                valid = pts(s.scene_start_ms) >= 0 && pts(s.scene_end_ms) <= 0;
            }
            HleProcess* process = HleProcess::current();
            for (u32* b : {&s.video_input_buffer, &s.audio_input_buffer}) {
                if (*b) process->main_memory().free(*b);
                *b = 0;
            }
            s.state = RecState::Closed;
            func = s.callback;
            arg = s.user_data;
        }
        if (valid) call_guest(cb, func, {static_cast<u64>(CELL_REC_STATUS_CLOSE), lv2::CELL_OK, arg});
        else {
            call_guest(cb, func,
                {static_cast<u64>(CELL_REC_STATUS_ERR),
                    static_cast<u64>(static_cast<s64>(error(CELL_REC_ERROR_FILE_NO_DATA))), arg});
        }
        return lv2::CELL_OK;
    });
    return finish(lv2::CELL_OK, call);
}

// RPCS3 cellRecQueryMemSize(pParam): a size, not an error.
s32 cellRecQueryMemSize(PPUContext& ctx, u32 param)
{
    const std::string call = std::format("cellRecQueryMemSize({:#x})", param);
    if (param && !options_usable(ctx, param, "cellRecQueryMemSize: param")) return finish(0, call + ": 0");
    const u32 size = query_mem_size(guest_base(ctx), param);
    PS_LOG_INFO(kChan, "{}: {:#x}", call, size);
    return static_cast<s32>(size);
}

// RPCS3 cellRecGetInfo(info, u64* pValue) (void): 0, or the value asked for.
s32 cellRecGetInfo(PPUContext& ctx, s32 info, u32 value)
{
    const std::string call = std::format("cellRecGetInfo({}, {:#x})", info, value);
    if (!value || !usable(ctx, value, 8, call)) return finish(lv2::CELL_OK, call + ": nothing written");
    Rec& r = rec();
    std::lock_guard lock(r.mutex);
    u64 v = 0;
    switch (info) {
    case CELL_REC_INFO_VIDEO_INPUT_ADDR: v = r.video_input_buffer; break;
    case CELL_REC_INFO_VIDEO_INPUT_WIDTH: v = r.width; break;
    case CELL_REC_INFO_VIDEO_INPUT_PITCH: v = r.pitch; break;
    case CELL_REC_INFO_VIDEO_INPUT_HEIGHT: v = r.height; break;
    case CELL_REC_INFO_AUDIO_INPUT_ADDR: v = r.audio_input_buffer; break;
    case CELL_REC_INFO_MOVIE_TIME_MSEC: v = 0; break; // nothing recorded
    case CELL_REC_INFO_SPURS_SYSTEMWORKLOAD_ID: v = 0xFFFFFFFF; break;
    default: break;
    }
    ps_write64(guest_base(ctx), value, v);
    return finish(lv2::CELL_OK, call + std::format(": {:#x}", v));
}

// A metadata string within `max` bytes (its NUL included).
bool short_text(PPUContext& ctx, u32 addr, u32 max, std::string* out = nullptr)
{
    const GuestString s = read_cstr(thread_of(ctx), addr, max);
    if (s.faulted) log_call_site(ctx, guest_base(ctx), std::format("cellRecSetInfo: a string at {:#x} runs out", addr));
    if (s.faulted || !s.terminated) return false;
    if (out) *out = s.text;
    return true;
}

// RPCS3 cellRecSetInfo(setInfo, value): while stopped, the movie's start and end times and its metadata, or the
// scene's.
s32 cellRecSetInfo(PPUContext& ctx, s32 info, u64 value)
{
    const std::string call = std::format("cellRecSetInfo({}, {:#x})", info, value);
    Rec& r = rec();
    std::lock_guard lock(r.mutex);
    if (r.state != RecState::Stopped) return finish(error(CELL_REC_ERROR_INVALID_STATE), call);
    u8* base = guest_base(ctx);
    const u32 addr = static_cast<u32>(value);
    switch (info) {
    case CELL_REC_SETINFO_MOVIE_START_TIME_MSEC: r.scene_start_ms = value; break;
    case CELL_REC_SETINFO_MOVIE_END_TIME_MSEC: r.scene_end_ms = value; break;
    case CELL_REC_SETINFO_MOVIE_META: {
        if (!value || !usable(ctx, addr, kMovieMetaSize, call)) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        const u32 game = ps_read32(base, addr), movie = ps_read32(base, addr + 4), description = ps_read32(base, addr + 8),
                  userdata = ps_read32(base, addr + 12);
        std::string game_title, movie_title;
        if (!game || !movie || !short_text(ctx, game, CELL_REC_MOVIE_META_GAME_TITLE_LEN, &game_title) ||
            !short_text(ctx, movie, CELL_REC_MOVIE_META_MOVIE_TITLE_LEN, &movie_title) ||
            (description && !short_text(ctx, description, CELL_REC_MOVIE_META_DESCRIPTION_LEN)) ||
            (userdata && !short_text(ctx, userdata, CELL_REC_MOVIE_META_USERDATA_LEN))) {
            return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        }
        r.game_title = game_title;
        r.movie_title = movie_title;
        break;
    }
    case CELL_REC_SETINFO_SCENE_META: {
        // CellRecSceneMetadata: be32 type, be64 startTime at 8, endTime at 16, 4 bytes, title at 28, be32 tagNum at
        // 32, char** tag at 36.
        if (!value || !usable(ctx, addr, kSceneMetaSize, call)) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        const u32 title = ps_read32(base, addr + 28), tag_num = ps_read32(base, addr + 32), tags = ps_read32(base, addr + 36);
        if (!title || ps_read32(base, addr) > CELL_REC_SCENE_META_TYPE_CLIP_USER || tag_num > CELL_REC_SCENE_META_TAG_NUM ||
            !short_text(ctx, title, CELL_REC_SCENE_META_TITLE_LEN)) {
            return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        }
        if (tag_num && !usable(ctx, tags, 4 * tag_num, call)) return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
        for (u32 i = 0; i < tag_num; i++) {
            const u32 tag = ps_read32(base, tags + 4 * i);
            if (!tag || !short_text(ctx, tag, CELL_REC_SCENE_META_TAG_LEN)) {
                return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
            }
        }
        r.scene_start_ms = ps_read64(base, addr + 8);
        r.scene_end_ms = ps_read64(base, addr + 16);
        break;
    }
    default: return finish(error(CELL_REC_ERROR_INVALID_VALUE), call);
    }
    return finish(lv2::CELL_OK, call);
}

} // namespace

void register_cellRec(Registry& r)
{
    r.add("cellRec", {
        PS_HLE_FUNC(0x39651E01, cellRecOpen),
        PS_HLE_FUNC(0x4AC76585, cellRecClose),
        PS_HLE_FUNC(0x5A8A8B0F, cellRecGetInfo),
        PS_HLE_FUNC(0x5B45439D, cellRecStop),
        PS_HLE_FUNC(0x964CD1B8, cellRecStart),
        PS_HLE_FUNC(0xDBF22BD1, cellRecQueryMemSize),
        PS_HLE_FUNC(0xF0958F73, cellRecSetInfo),
    });
}

} // namespace pspocket::hle
