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

// The built-in "x-cell-fs" file source (0x138 bytes in the player's block; cellSailFileSource.cpp, D-564): +0x40 the
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

} // namespace pspocket::hle::sail
