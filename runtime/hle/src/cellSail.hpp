#pragma once

// cellSail's parts share these (D-562): libsail's error codes, its calls into lv2 and sysPrxForUser, the player's
// layout and its message queue.

#include <initializer_list>
#include <string_view>

#include "guest.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle::sail {

inline constexpr std::string_view kChan = "cellSail";

enum : u32 {
    CELL_SAIL_ERROR_INVALID_ARG = 0x80610701,
    CELL_SAIL_ERROR_INVALID_STATE = 0x80610702,
    CELL_SAIL_ERROR_UNSUPPORTED_STREAM = 0x80610703,
    CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE = 0x80610704,
    CELL_SAIL_ERROR_EMPTY = 0x80610705,
    CELL_SAIL_ERROR_FULLED = 0x80610706,
    CELL_SAIL_ERROR_USING = 0x80610707,
    CELL_SAIL_ERROR_NOT_AVAILABLE = 0x80610708,
    CELL_SAIL_ERROR_CANCEL = 0x80610709,
    CELL_SAIL_ERROR_NOT_FOUND = 0x80610710,
    CELL_SAIL_ERROR_MEMORY = 0x806107F0,
    CELL_SAIL_ERROR_INVALID_FD = 0x806107F1,
    CELL_SAIL_ERROR_FATAL = 0x806107FF,
};
inline s32 error(u32 code) { return static_cast<s32>(code); }

inline u8* gb(PPUContext& ctx) { return guest_base(ctx); }
inline u32 r32(PPUContext& ctx, u32 a) { return ps_read32(gb(ctx), a); }
inline void w32(PPUContext& ctx, u32 a, u32 v) { ps_write32(gb(ctx), a, v); }
inline u64 r64(PPUContext& ctx, u32 a) { return ps_read64(gb(ctx), a); }
inline void w64(PPUContext& ctx, u32 a, u64 v) { ps_write64(gb(ctx), a, v); }
inline u8 r8(PPUContext& ctx, u32 a) { return gb(ctx)[a]; }
inline void w8(PPUContext& ctx, u32 a, u8 v) { gb(ctx)[a] = v; }
inline void w16(PPUContext& ctx, u32 a, u16 v) { ps_write16(gb(ctx), a, v); }

// An lv2 system call as libsail makes it.
inline s32 sc(PPUContext& ctx, u32 n, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, n, args));
}

// sysPrxForUser's `name` (libsail's import), pspocket's own.
u64 lv2_lib(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args);

// The player (0x400 bytes, the game's): the offsets libsail uses.
namespace pl {
constexpr u32 kVersion = 0, kSync = 8, kState = 0x163, kThread = 0x168, kQueue = 0x170, kEventMask = 0x190,
              kRepeatMode = 0x198, kRepeatInfo = 0x1A0, kRepeatSet = 0x1C8, kPaused = 0x1C9, kCurrentDescriptor = 0x22C,
              kDescriptorCount = 0x230, kHandler = 0x234, kHandlerArg = 0x238, kAlloc = 0x23C, kFree = 0x240,
              kAllocArg = 0x244, kLock = 0x248, kRunning = 0x250, kBootArg = 0x258, kProtocols = 0x268,
              kProtocolSources = 0x2E8, kProtocolCount = 0x2F8, kDmux = 0x300, kParams = 0x358, kAudioCount = 0x378,
              kVideoCount = 0x37C, kUserCount = 0x380, kAudio = 0x384, kVideo = 0x388, kUser = 0x38C, kThreadPrio = 0x390,
              kThreadStack = 0x394, kSpursSpus = 0x398, kSpursSpuPrio = 0x39C, kSpursPpuPrio = 0x3A0, kSpursExit = 0x3A4,
              kSource = 0x3B0, kMp4 = 0x3B4, kAvi = 0x3B8, kSpurs = 0x3C4, kFrame = 0x3C8, kSize = 0x400;
constexpr u32 kAudioStride = 0x180, kVideoStride = 0x238, kUserStride = 0x88;
constexpr u8 kInitialized = 0, kBooted = 1, kFinalized = 9, kBootFailed = 10;
} // namespace pl

// The queue's fields from the player's +0x170.
namespace q {
constexpr u32 kCapacity = 0, kCount = 4, kRing = 8, kWrite = 0xC, kRead = 0x10, kMutex = 0x14, kCond = 0x18;
}

// The built-in "x-cell-fs" file source (0x138 bytes in the player's block; cellSailFileSource.cpp, D-901): +0x40 the
// CellSailSource it registers, +0x100 its file calls (14 words).
namespace fsrc {
constexpr u32 kSource = 0x40, kCalls = 0x100, kSize = 0x138;
}
// Lays the source out at `s` with libsail's file calls and initializes its CellSailSource (cellSailSourceInitialize's
// answer).
s32 builtin_source_init(PPUContext& ctx, u32 s);
// cellSailSourceFinalize of its CellSailSource.
void builtin_source_fini(PPUContext& ctx, u32 s);
// Player parameter 33: the game's file calls (0x28 bytes: 0, open, a call after open or 0, close, fstat, read, lseek,
// cancel, two more words; INVALID_ARG unless all are there), or libsail's own back for NULL.
s32 builtin_source_set_file_calls(PPUContext& ctx, u32 s, u32 calls);

// libsail's lv2 mutex and condition pair at `at` (priority, not recursive), its lwmutexes (priority, not recursive).
s32 make_pair(PPUContext& ctx, u32 at, const char* name);
void destroy_pair(PPUContext& ctx, u32 at);
s32 make_lwmutex(PPUContext& ctx, u32 at, const char* name);
s32 destroy_lwmutex(PPUContext& ctx, u32 at);

// A pspocket module's function by name (as libsail imports it): its answer; the registers past the call kept.
u64 module_call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args);

// The elementary stream `index` of `kind` (0 audio, 1 video, 2 user) chosen in an open descriptor (cellSailDescriptor.cpp):
// through `es` (for PAMF, a reader set to the stream) or, without, the descriptor's reader set to the kind's `index`-th
// stream. 1 when the stream's format changed, 0 when not, or the error.
s32 select_es(PPUContext& ctx, u32 d, s32 kind, u32 index, u32 es);
// A chosen (1) elementary stream's entry back to 0 (cellSailDescriptorClearEs without its descriptor check).
s32 clear_es(PPUContext& ctx, u32 d, s32 kind, u32 index);

// The per-stream pipelines (cellSailPipelines.cpp): the audio one at an audio slot's +0x80, the video one at a video
// slot's +0x80; the renderers' and AU receivers' makeup and cleanup through their callbacks.
s32 audio_pipeline_init(PPUContext& ctx, u32 a, u32 p);
void audio_pipeline_stop(PPUContext& ctx, u32 a);
void audio_pipeline_fini(PPUContext& ctx, u32 a);
s32 video_pipeline_init(PPUContext& ctx, u32 v, u32 p);
void video_pipeline_stop(PPUContext& ctx, u32 v);
void video_pipeline_fini(PPUContext& ctx, u32 v);
s32 renderer_makeup(PPUContext& ctx, u32 r);
s32 renderer_cleanup(PPUContext& ctx, u32 r);
s32 receiver_makeup(PPUContext& ctx, u32 t, u32 p, u32 clock, u32 index);
s32 receiver_cleanup(PPUContext& ctx, u32 t);

// Posts {code, 0, object, arg} at the queue's tail (libsail's post): the unlock's answer, or the lock's error.
s32 post(PPUContext& ctx, u32 player, u32 code, u32 object, u32 arg);
// Posts {code, a, b, c} at the queue's head (libsail's urgent post).
s32 post_urgent(PPUContext& ctx, u32 player, u32 code, u32 a, u32 b, u32 c);
// Posts {code, a, b, c} at the queue's tail.
s32 post_message(PPUContext& ctx, u32 player, u32 code, u32 a, u32 b, u32 c);
// Posts a call's completion {0, result, handle} (nothing when the handle is 0).
s32 post_completion(PPUContext& ctx, u32 player, s32 result, u64 handle);

// ------------------------------------------------------------------------------------------------- the playback engine

// What the playback's parts share (D-919; cellSailControl.cpp unless noted).

// The game's allocator (alloc(arg, boundary, size), free(arg, boundary, ptr)) and the player's lock.
u32 player_alloc(PPUContext& ctx, u32 p, u32 boundary, u32 size);
void player_free(PPUContext& ctx, u32 p, u32 boundary, u32 ptr);
s32 player_lock(PPUContext& ctx, u32 p);
s32 player_unlock(PPUContext& ctx, u32 p);

// Events: the game's handler (major << 32, a, b) when it subscribed to `major`; an error (major 1, "error 0x%X" printed
// first); a call's completion (major 2) when the call is the one running, and the same under the player's lock.
void notify_event(PPUContext& ctx, u32 p, u32 major, u64 a, u64 b);
void notify_error(PPUContext& ctx, u32 p, s32 err);
void notify_completed(PPUContext& ctx, u32 p, u32 command, s32 result);
void call_completed(PPUContext& ctx, u32 p, u32 call, s32 result);
// A pipeline's error for the player (message 0xD2 {error, kind (3 dmux, 4 audio, 5 video), index}).
s32 post_error(PPUContext& ctx, u32 p, s32 err, u32 kind, u32 index);
// Stops the process at a path a later part of D-919 makes pspocket's (the call site logged).
[[noreturn]] void later_part(PPUContext& ctx, std::string_view what);

// libsail's command protocol: the game's thread posts the command at the queue's head with a reply record and waits
// (the lock's error, or 0 with the answer at `reply`); the control thread answers under the player's lock (false when
// the signal failed: the lock kept).
s32 command(PPUContext& ctx, u32 p, u32 code, u32 arg, u32 reply);
s32 command_answer(PPUContext& ctx, u32 p, u32 code, u32 arg);
bool answer(PPUContext& ctx, u32 p, u32 record, s32 result);

// Whether libsail loads the modules it needs (not in system software, as its module start reads the authority).
bool loads_modules(PPUContext& ctx);
// cellSysmodule's loads and unloads as libsail makes them (none in system software; internal ones from 0xF000).
s32 load_module(PPUContext& ctx, u32 id);
s32 unload_module(PPUContext& ctx, u32 id);
// The process's SDK version as libsail's module start reads it, and libsail's flag for SDKs past 0x21FFFF.
u32 sdk_version(PPUContext& ctx);
bool sdk_after_22(PPUContext& ctx);

// A step's handle: the message its completion becomes (code << 32) and its record (+0 flags: 0x20 in use, 1 failed; +4
// the steps still out; +8 the handle of what it belongs to).
inline u64 step(u32 code, u32 record) { return u64{code} << 32 | record; }

// The descriptor's fields.
namespace ds {
constexpr u32 kType = 0, kHeader = 4, kUri = 0x10, kMediaInfo = 0x14, kPlayer = 0x1C, kProtocol = 0x20,
              kSource = 0x24, kAudio = 0x28, kVideo = 0x2C, kUser = 0x30, kNext = 0x34, kState = 0x38, kNoAuto = 0x39,
              kReader = 0x40, kMuxRate = 0xC4, kModule = 0xC0, kBuffer = 0xC8, kHeaderSize = 0xCC, kStreamSize = 0xD0,
              kDuration = 0xD8;
constexpr u32 kAudioStride = 0x90, kVideoStride = 0x78, kUserStride = 0x4C;
} // namespace ds

// The stream slots in the player's block: +0 the record of the stream's open or close (0x10 bytes), +0x10 its part of
// the dmux pipeline (0x70 bytes), then its pipeline, renderer or AU receiver, and its settings.
namespace as {
constexpr u32 kDmuxEs = 0x10, kPipeline = 0x80, kRenderer = 0x150, kIndex = 0x154, kState = 0x155, kMode = 0x156,
              kSrc = 0x158, kAdecResource = 0x15C;
}
namespace vs {
constexpr u32 kDmuxEs = 0x10, kPipeline = 0x80, kRenderer = 0x180, kIndex = 0x184, kState = 0x185, kMode = 0x186,
              kExtraFrames = 0x188, kFrames = 0x189, kFramesMany = 0x18A, kSpus = 0x18B, kSpusHigh = 0x18C,
              kVpost = 0x18D, kVc1 = 0x18E, kVpostCtrl = 0x190, kVdecResource = 0x1F0, kSpursResource = 0x20C,
              kVpostResource = 0x21C;
}
namespace us {
constexpr u32 kDmuxEs = 0x10, kReceiver = 0x80, kIndex = 0x84, kState = 0x85;
}
inline u32 audio_slot(PPUContext& ctx, u32 p, u32 i) { return r32(ctx, p + pl::kAudio) + i * pl::kAudioStride; }
inline u32 video_slot(PPUContext& ctx, u32 p, u32 i) { return r32(ctx, p + pl::kVideo) + i * pl::kVideoStride; }
inline u32 user_slot(PPUContext& ctx, u32 p, u32 i) { return r32(ctx, p + pl::kUser) + i * pl::kUserStride; }

// The dmux pipeline (0x58 bytes at the player's +0x300) and the elementary streams in it (cellSailDmux.cpp).
namespace dmx {
constexpr u32 kHandleCall = 0, kState = 0x10, kWanted = 0x14, kWord = 0x18, kModule = 0x1C, kField20 = 0x20,
              kPending = 0x24, kEsList = 0x28, kEsCount = 0x2C, kEnabled = 0x30, kPlayer = 0x34, kSource = 0x38,
              kMemory = 0x3C, kDeferred = 0x40, kHandle = 0x44, kType = 0x48, kSize = 0x58;
}
// An elementary stream's part (0x70 bytes): +0 cellDmux's ES handle, +4 the kind, +8 AUs found and not taken, +0xC AUs
// not released, +0x10 an AU put back (0x28 bytes), +0x38 the slot, +0x3C the next on the pipeline's list, +0x40 the
// pipeline, +0x44 its memory, +0x48 the memory's size, +0x4C the ES filter (16 bytes), +0x5C the ES specific info (12),
// +0x68 1 while an AU is put back.
namespace des {
constexpr u32 kHandle = 0, kKind = 4, kFound = 8, kHeld = 0xC, kSaved = 0x10, kSlot = 0x38, kNext = 0x3C,
              kPipeline = 0x40, kMemory = 0x44, kMemorySize = 0x48, kFilter = 0x4C, kSpecific = 0x5C, kHasSaved = 0x68,
              kSize = 0x70;
}
void dmux_init(PPUContext& ctx, u32 dm, u32 p);
void dmux_open(PPUContext& ctx, u32 dm, u32 type, u32 source, u64 handle);
void dmux_close(PPUContext& ctx, u32 dm, u64 handle);
void dmux_fini(PPUContext& ctx, u32 dm);
s32 dmux_es_add(PPUContext& ctx, u32 dm, u32 kind, u32 filter, u32 specific, u32 slot, u32 es, bool enable);
s32 dmux_es_remove(PPUContext& ctx, u32 dm, u32 es);
s32 dmux_es_readd(PPUContext& ctx, u32 dm, u32 kind, u32 filter, u32 specific, u32 slot, u32 es);
// The data path: whether the pipeline takes an item (started, items wanted), whether a stream has an AU found; stop
// (mode 0 drain, 1 flush, 2 a new session word) and start (the call's handle), an item demultiplexed (message 3), an AU
// found (4), a stream flushed (0x2F), reset; an item fed {address, size, session word}; a stream's AUs taken (audio
// 0x18 bytes, user 0x28), peeked at, put back, skipped and released.
bool dmux_wants(PPUContext& ctx, u32 dm);
bool dmux_es_has_au(PPUContext& ctx, u32 es);
void dmux_stop(PPUContext& ctx, u32 dm, u32 mode, u64 handle);
void dmux_stop_by_word(PPUContext& ctx, u32 dm, u64 handle);
void dmux_start(PPUContext& ctx, u32 dm, u64 handle);
void dmux_item_done(PPUContext& ctx, u32 dm);
void dmux_es_au_found(PPUContext& ctx, u32 es);
void dmux_es_flushed(PPUContext& ctx, u32 dm);
void dmux_reset(PPUContext& ctx, u32 dm, u64 handle);
s32 dmux_feed(PPUContext& ctx, u32 dm, u32 rec);
s32 dmux_set_stream(PPUContext& ctx, u32 dm, u32 addr, u32 size, u32 discontinuity, u64 user);
s32 dmux_es_enable(PPUContext& ctx, u32 es);
s32 dmux_es_get_au_audio(PPUContext& ctx, u32 es, u32 out);
s32 dmux_es_get_au_user(PPUContext& ctx, u32 es, u32 out);
s32 dmux_es_peek(PPUContext& ctx, u32 es, u32 out);
void dmux_es_unget(PPUContext& ctx, u32 es, u32 rec);
s32 dmux_es_skip_au(PPUContext& ctx, u32 es);
s32 dmux_es_release_au(PPUContext& ctx, u32 es);

// The renderers as the player drives them (cellSailRenderers.cpp), and the user streams' AU receivers (their object at
// the receiver's +0x20): each step completes the handle given.
void audio_renderer_open(PPUContext& ctx, u32 r, u32 format, u32 slot, u64 handle);
void video_renderer_open(PPUContext& ctx, u32 r, u32 format, u32 slot, u64 handle);
void audio_renderer_close(PPUContext& ctx, u32 r, u64 handle);
void video_renderer_close(PPUContext& ctx, u32 r, u64 handle);
void audio_renderer_completed(PPUContext& ctx, u32 r, s32 result);
void video_renderer_completed(PPUContext& ctx, u32 r, s32 result);
void receiver_open(PPUContext& ctx, u32 t, u32 type, u32 filter, u32 slot, u64 handle);
void receiver_close(PPUContext& ctx, u32 t, u64 handle);
void receiver_answered(PPUContext& ctx, u32 t, u32 code, s32 result);
// Playing: a renderer's free frames, a frame checked out to fill and back in, start, stop, the stream's end, a frame
// played, cancel, a video renderer's pause at its first frame; the AU receivers' start, end, stop, cancel and AUs.
bool renderer_has_free(PPUContext& ctx, u32 r);
s32 renderer_checkout(PPUContext& ctx, u32 r, u32 out);
s32 renderer_checkin(PPUContext& ctx, u32 r, u32 frame);
void audio_renderer_start(PPUContext& ctx, u32 r, u32 flag, u64 handle);
void video_renderer_start(PPUContext& ctx, u32 r, u32 flag, u64 handle);
void audio_renderer_stop(PPUContext& ctx, u32 r, u64 handle);
void video_renderer_stop(PPUContext& ctx, u32 r, u32 keep, u64 handle);
void audio_renderer_end(PPUContext& ctx, u32 r, u64 handle);
void video_renderer_end(PPUContext& ctx, u32 r, u32 keep, u64 handle);
void renderer_frame_done(PPUContext& ctx, u32 r);
void renderer_cancel(PPUContext& ctx, u32 r);
bool video_renderer_first_frame(PPUContext& ctx, u32 r);
void receiver_start(PPUContext& ctx, u32 t, u32 flag, u64 handle);
void receiver_end(PPUContext& ctx, u32 t, u64 handle);
void receiver_stop(PPUContext& ctx, u32 t, u64 handle);
void receiver_cancel(PPUContext& ctx, u32 t);
void receiver_au_found(PPUContext& ctx, u32 t, u32 es);
void receiver_au_taken(PPUContext& ctx, u32 t, u32 es);

// The source (CellSailSource, 0x80 bytes) as the player drives it: +0 the player using it, +4 its argument, +8 its 14
// callbacks, +0x40 the items ready, +0x44 the items checked out, +0x48 its state (1 closed, 2 opening, 3 open, 4
// starting, 5 started, 6 stopping, 7 closing, 8 draining, 9 drained with items out, 10 stopped with items out), +0x4C
// the items still to drain, +0x50 a result kept, +0x58 the call's handle, +0x70 the descriptors open through it.
namespace src {
constexpr u32 kPlayer = 0, kArg = 4, kFuncs = 8, kReady = 0x40, kOut = 0x44, kState = 0x48, kDrain = 0x4C,
              kResult = 0x50, kHandle = 0x58, kUsers = 0x70;
constexpr u32 kMakeup = 0, kCleanup = 1, kOpen = 2, kClose = 3, kStart = 4, kStop = 5, kCancel = 6, kCheckout = 7,
              kCheckin = 8, kClear = 9, kRead = 10, kReadSync = 11;
} // namespace src
inline u32 source_func(PPUContext& ctx, u32 s, u32 index) { return r32(ctx, s + src::kFuncs + 4 * index); }
void source_open(PPUContext& ctx, u32 s, u32 d, u64 handle);
void source_close(PPUContext& ctx, u32 s, u64 handle);
void source_call_completed(PPUContext& ctx, u32 s, s32 r);
void source_item_ready(PPUContext& ctx, u32 s);
bool source_has_items(PPUContext& ctx, u32 s);
s32 source_checkout(PPUContext& ctx, u32 s, u32 out);
s32 source_checkin(PPUContext& ctx, u32 s, u32 item);
void source_end(PPUContext& ctx, u32 s, u64 handle, u32 flush);
void source_stop(PPUContext& ctx, u32 s, u64 handle);
void source_start(PPUContext& ctx, u32 s, u32 cmd, u32 word, u64 handle);
void source_cancel(PPUContext& ctx, u32 s);

// The AV clock (200 bytes at the player's +8, cellSailControl.cpp): reset when a stream opens, its play state reset,
// running or not, its ticks per millisecond from the play rate, the time it holds.
void clock_reset(PPUContext& ctx, u32 c);
void clock_reset_play(PPUContext& ctx, u32 c);
u32 clock_run(PPUContext& ctx, u32 c, u32 run);
void clock_set_rate(PPUContext& ctx, u32 c, float rate);
u64 clock_time(PPUContext& ctx, u32 c);
// Its sync engine (cellSailClock.cpp): the frames of the sound and graphics adapters (synced: whether due, the times
// covered; outside the sync: counted), a stream's end, a session's sync.
u32 clock_run_locked(PPUContext& ctx, u32 c, u32 run);
u32 clock_sync(PPUContext& ctx, u32 c);
void clock_session(PPUContext& ctx, u32 c, u32 word);
u32 clock_audio_frame(PPUContext& ctx, u32 c, u64 pts, u32 session, u32 current, u32 tag, u32 samples, u32 out,
    u32 drop, u32 phase, u32 index);
u32 clock_video_frame(PPUContext& ctx, u32 c, u64 pts, u32 session, u32 current, u32 tag, u32 from, u32 to, u32 hold,
    u32 phase, u32 index);
u32 clock_audio_tick(PPUContext& ctx, u32 c, u32 tag, u32 samples, u32 index, u32 out, u32 hold);
u32 clock_video_tick(PPUContext& ctx, u32 c, u32 tag, u32 index, u32 from, u32 to, u32 hold);
void clock_audio_end(PPUContext& ctx, u32 c, u32 index, u32 seen_at, u32 word);
void clock_video_end(PPUContext& ctx, u32 c, u32 index, u32 seen_at, u32 word);

// The stream's media state (the source's, message 0x2D): a change is event 11 and sets the 0x10 flag of each chosen
// stream's renderer or AU receiver.
void set_media_state(PPUContext& ctx, u32 p, u32 value, u32 d);

// The decoders' pipelines while playing (cellSailPipelines.cpp): whether the decoder takes AUs and wants frames, a
// frame given, the decoder's and the copy thread's messages, an AU decoded, the stream's end, stop and start.
bool adec_takes(PPUContext& ctx, u32 a);
bool adec_wants_frame(PPUContext& ctx, u32 a);
void adec_frame(PPUContext& ctx, u32 a, u32 frame);
void adec_seq_done(PPUContext& ctx, u32 a);
void adec_frame_back(PPUContext& ctx, u32 a);
void adec_pcm_taken(PPUContext& ctx, u32 a);
void adec_au_done(PPUContext& ctx, u32 a);
s32 adec_decode(PPUContext& ctx, u32 a, u32 au);
void adec_end(PPUContext& ctx, u32 a, u64 handle);
void adec_stop(PPUContext& ctx, u32 a, u64 handle);
void adec_start(PPUContext& ctx, u32 a, u64 handle);
void adec_pcm_out(PPUContext& ctx, u32 a);
bool vdec_takes(PPUContext& ctx, u32 v);
void vdec_frame_wanted(PPUContext& ctx, u32 v);
bool vdec_has_picture(PPUContext& ctx, u32 v);
void vdec_frame(PPUContext& ctx, u32 v, u32 frame);
void vdec_picture_out(PPUContext& ctx, u32 v);
void vdec_frame_back(PPUContext& ctx, u32 v, u32 keep, u32 out);
void vdec_seq_done(PPUContext& ctx, u32 v);
void vdec_au_done(PPUContext& ctx, u32 v, s32 result);
s32 vdec_decode(PPUContext& ctx, u32 v, u32 au);
void vdec_end(PPUContext& ctx, u32 v, u64 handle);
void vdec_stop(PPUContext& ctx, u32 v, u64 handle);
void vdec_start(PPUContext& ctx, u32 v, u8 rate, u64 handle);

// The playback engine (cellSailPlay.cpp): the control thread's messages of the playback (the data path, Start, Stop,
// the stream's end, Cancel): true when `code` was one of them. The elementary streams of a stream started and stopped.
bool play_message(PPUContext& ctx, u32 p, u32 code, s32 result, u32 b, u32 c);
void es_start_audio(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u64 handle);
void es_start_video(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u64 handle);
void es_start_user(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u64 handle);
// Stopped: `keep` is the video renderer's "keep the last frame" (the others' stops ignore it).
void es_stop_audio(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u32 keep, u64 handle, bool failed);
void es_stop_video(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u32 keep, u64 handle, bool failed);
void es_stop_user(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u32 keep, u64 handle, bool failed);

// The decoders' pipelines (cellSailPipelines.cpp): opened for an elementary stream's entry, closed; each completes the
// handle given.
void audio_pipeline_open(PPUContext& ctx, u32 a, u32 type, u32 params, u32 format, u32 es, u32 renderer, u32 slot,
    u64 handle);
void audio_pipeline_close(PPUContext& ctx, u32 a, u64 handle);
void video_pipeline_open(PPUContext& ctx, u32 v, u32 type, u32 picture, u32 format, u32 es, u32 renderer, u32 slot,
    u64 handle);
void video_pipeline_close(PPUContext& ctx, u32 v, u64 handle);

// The elementary streams opened and closed (cellSailEs.cpp): one stream of each kind, the slot's record counting its
// steps (`failed` marks the record: the stream is being closed after a failed open).
void es_open_audio(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 entry, u64 handle);
void es_open_video(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 entry, u64 handle);
void es_open_user(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 entry, u64 handle);
void es_close_audio(PPUContext& ctx, u32 p, u32 slot, u64 handle, bool failed);
void es_close_video(PPUContext& ctx, u32 p, u32 slot, u64 handle, bool failed);
void es_close_user(PPUContext& ctx, u32 p, u32 slot, u64 handle, bool failed);
// The control thread's messages of the elementary streams (their steps and the OpenEs, ReopenEs and CloseEs calls):
// true when `code` was one of them.
bool es_message(PPUContext& ctx, u32 p, u32 code, u32 a, u32 b, u32 c);

} // namespace pspocket::hle::sail
