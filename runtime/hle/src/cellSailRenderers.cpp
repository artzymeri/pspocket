// cellSail's renderers as the player drives them (D-919), as libsail (fw 4.92) has it: the game's audio and video
// renderers (or libsail's sound and graphics adapters, renderers too) and the user streams' AU receivers. Each call
// the player makes keeps the handle its completion becomes; the renderer answers through its NotifyCallCompleted
// (messages 0x33 audio, 0x34 video), the AU receiver through its own messages (0x35 opened, 0x36 closed, 0x37 started,
// 0x38 stopped).
//
// A renderer: +0 its callbacks (makeup, cleanup, open, close, start, stop, cancel, checkout, checkin), +4 their
// argument, +8 the stream's index, +0x10 the player's clock, +0x14 the player, +0x18 the stream's slot, +0x1C its state
// (0 made up, 1 opening, 2 open, 3 starting, 4 started, 5 and 7 stopping, 6 closing, 8 stopped with frames out, 9
// cleaned up), +0x20 the frames it gave, +0x24 the frames free, +0x28 the handle (u64), +0x30 the frames checked out,
// +0x34 its flags (1 muted, 8 paused, 0x10 the stream shown), +0x38 a video renderer's "keep the last frame".

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle::sail {

namespace {

namespace rd {
constexpr u32 kFuncs = 0, kArg = 4, kPlayer = 0x14, kSlot = 0x18, kState = 0x1C, kFrames = 0x20, kFree = 0x24,
              kHandle = 0x28, kOut = 0x30, kKeep = 0x38;
constexpr u32 kOpen = 2, kClose = 3;
} // namespace rd

u32 rfunc(PPUContext& ctx, u32 r, u32 index) { return r32(ctx, r32(ctx, r + rd::kFuncs) + 4 * index); }

// The handle taken (cleared) and completed with `result`.
void complete(PPUContext& ctx, u32 r, s32 result)
{
    const u64 handle = r64(ctx, r + rd::kHandle);
    w64(ctx, r + rd::kHandle, 0);
    post_completion(ctx, r32(ctx, r + rd::kPlayer), result, handle);
}

// Stopped with nothing out (every frame given back, none checked out): open again (2) and the stop completes.
void stopped_if_idle(PPUContext& ctx, u32 r)
{
    if (r32(ctx, r + rd::kOut) != 0 || r32(ctx, r + rd::kFrames) != r32(ctx, r + rd::kFree)) return;
    const u64 handle = r64(ctx, r + rd::kHandle);
    w32(ctx, r + rd::kState, 2);
    w64(ctx, r + rd::kHandle, 0);
    post_completion(ctx, r32(ctx, r + rd::kPlayer), 0, handle);
}

void set_frames(PPUContext& ctx, u32 r, s32 n)
{
    w32(ctx, r + rd::kFree, static_cast<u32>(n));
    w32(ctx, r + rd::kFrames, static_cast<u32>(n));
}

// The answer to a call (libsail's, the audio and video renderers alike but for a stop's "keep the last frame" and the
// message telling the player how many frames are free): opening with frames is open (2) with them, without them closed
// (0); open stays open; starting is started (4, and the free frames announced) or, refused, open; stopping goes on until
// the frames are back (8); closing is closed. Every answer but a stop's completes the call.
void renderer_completed(PPUContext& ctx, u32 r, s32 result, bool video)
{
    const u32 st = r32(ctx, r + rd::kState);
    switch (st) {
    case 1:
        if (result > 0) {
            w32(ctx, r + rd::kState, 2);
            set_frames(ctx, r, result);
        } else {
            w32(ctx, r + rd::kState, 0);
        }
        break;
    case 2: w32(ctx, r + rd::kState, 2); break;
    case 3:
        if (result < 0) {
            w32(ctx, r + rd::kState, 2);
            break;
        }
        w32(ctx, r + rd::kState, 4);
        if (const u32 free = r32(ctx, r + rd::kFree); static_cast<s32>(free) > 0)
            post(ctx, r32(ctx, r + rd::kPlayer), video ? 0x13 : 0x11, r, free);
        break;
    case 5:
    case 7:
        if (video && r8(ctx, r + rd::kKeep)) {
            w32(ctx, r + rd::kState, 2);
            break;
        }
        w32(ctx, r + rd::kState, 8);
        stopped_if_idle(ctx, r);
        return;
    case 6: w32(ctx, r + rd::kState, 0); break;
    default: break;
    }
    complete(ctx, r, result);
}

} // namespace

// Open (libsail's): opening (1), the slot and handle kept, the renderer's open(arg, format, frames): audio a sixth of a
// second of frames, video the stream's frames by its rate (video parameters) and at least the minimum, which it is
// also given.
void audio_renderer_open(PPUContext& ctx, u32 r, u32 format, u32 slot, u64 handle)
{
    w32(ctx, r + rd::kState, 1);
    const s32 fs = static_cast<s32>(r32(ctx, format + 4));
    const s32 samples = static_cast<s16>(ps_read16(gb(ctx), format + 2));
    w32(ctx, r + rd::kSlot, slot);
    w64(ctx, r + rd::kHandle, handle);
    const s32 frames = (samples + fs / 6 - 1) / samples;
    call_guest(ctx, rfunc(ctx, r, rd::kOpen), {r32(ctx, r + rd::kArg), format, static_cast<u64>(static_cast<s64>(frames))});
}

void video_renderer_open(PPUContext& ctx, u32 r, u32 format, u32 slot, u64 handle)
{
    w64(ctx, r + rd::kHandle, handle);
    w32(ctx, r + rd::kSlot, slot);
    w32(ctx, r + rd::kState, 1);
    const u32 least = u32{r8(ctx, slot + vs::kExtraFrames)} + 1;
    u32 frames = static_cast<s8>(r8(ctx, format + 3)) < 6 ? r8(ctx, slot + vs::kFrames) : r8(ctx, slot + vs::kFramesMany);
    if (frames < least) frames = least;
    call_guest(ctx, rfunc(ctx, r, rd::kOpen), {r32(ctx, r + rd::kArg), format, frames, least});
}

// Close: closing (6), the renderer's close(arg).
void audio_renderer_close(PPUContext& ctx, u32 r, u64 handle)
{
    w64(ctx, r + rd::kHandle, handle);
    w32(ctx, r + rd::kState, 6);
    call_guest(ctx, rfunc(ctx, r, rd::kClose), {r32(ctx, r + rd::kArg)});
}

void video_renderer_close(PPUContext& ctx, u32 r, u64 handle) { audio_renderer_close(ctx, r, handle); }

void audio_renderer_completed(PPUContext& ctx, u32 r, s32 result) { renderer_completed(ctx, r, result, false); }
void video_renderer_completed(PPUContext& ctx, u32 r, s32 result) { renderer_completed(ctx, r, result, true); }

// ------------------------------------------------------------------------------------------------- AU receivers

// The AU receiver's object (+0x20 of the receiver): +0 its 9 callbacks, +0x24 their argument (the receiver), +0x28 the
// stream's index, +0x2C the clock, +0x30 the player, +0x34 the slot, +0x38 its state (0 closed, 1 opening, 2 open, 3
// starting, 4 started, 5 and 7 stopping, 6 closing, 8 cleaned up), +0x40 the handle (u64), +0x48 paused, +0x49 muted,
// +0x4A the stream shown.
namespace {
namespace ar {
constexpr u32 kArg = 0x24, kPlayer = 0x30, kSlot = 0x34, kState = 0x38, kHandle = 0x40;
}
void receiver_call(PPUContext& ctx, u32 t, u32 index, std::initializer_list<u64> rest)
{
    u64 args[8] = {r32(ctx, t + ar::kArg)};
    u32 n = 1;
    for (const u64 a : rest) args[n++] = a;
    const u32 f = r32(ctx, t + 4 * index);
    switch (n) {
    case 1: call_guest(ctx, f, {args[0]}); break;
    case 2: call_guest(ctx, f, {args[0], args[1]}); break;
    default: call_guest(ctx, f, {args[0], args[1], args[2]}); break;
    }
}
} // namespace

// Open: opening (1), the slot and handle kept, the receiver's open(arg, the stream type, the stream's filter).
void receiver_open(PPUContext& ctx, u32 t, u32 type, u32 filter, u32 slot, u64 handle)
{
    w64(ctx, t + ar::kHandle, handle);
    w32(ctx, t + ar::kSlot, slot);
    w32(ctx, t + ar::kState, 1);
    receiver_call(ctx, t, 2, {type, filter});
}

// Close: closing (6), the receiver's close(arg).
void receiver_close(PPUContext& ctx, u32 t, u64 handle)
{
    w64(ctx, t + ar::kHandle, handle);
    w32(ctx, t + ar::kState, 6);
    receiver_call(ctx, t, 3, {});
}

// The receiver's answers: 0x35 opened (open, or closed when refused), 0x36 closed, 0x37 started (started, or open when
// refused), 0x38 stopped (open); each completes the call.
void receiver_answered(PPUContext& ctx, u32 t, u32 code, s32 result)
{
    u32 st = 2;
    switch (code) {
    case 0x35: st = result >= 0 ? 2 : 0; break;
    case 0x36: st = 0; break;
    case 0x37: st = result >= 0 ? 4 : 2; break;
    default: break;
    }
    w32(ctx, t + ar::kState, st);
    post_completion(ctx, r32(ctx, t + ar::kPlayer), result, r64(ctx, t + ar::kHandle));
}

} // namespace pspocket::hle::sail
