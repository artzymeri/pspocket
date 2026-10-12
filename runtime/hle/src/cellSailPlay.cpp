// cellSail's playback engine (D-919 part 3), as libsail (fw 4.92) has it: Start, Stop, the stream's end and Cancel, and
// the data path between the source, the dmux pipeline, the decoders' pipelines and the renderers (or AU receivers).
//
// The stream's phase (the player's +0x160): 2 open, 3 starting, 4 its streams started and the source starting, 5
// ending, 6 stopping. Start (message 0x6C, call 10) starts every open elementary stream (its decoder's sequence, its
// renderer or AU receiver, its dmux part enabled; steps 0x70-0x7B on the record at +0xE0), then the source from the
// position the start command gives (0x6F; the session word at the player's +4 counts sessions and carries the flags
// the decoders and renderers read), and completes when the source answered (0x6D, state 6). Stop (0x7D, call 11)
// stops the source, resets the dmux pipeline and stops every started stream (0x7F-0x83; each stream's decoder flushed,
// its renderer stopped), state 4 again (0x7E). The source's end of input (0x17), without a repeat or a next descriptor,
// ends the stream (phase 5): the source drains (0x18), the dmux pipeline drains (each elementary stream with nothing
// left: 0x1A), each decoder ends its sequence and each renderer plays out what it holds (0xBE-0xC7); then the stream is
// open again (state 4). A call made while another step runs is deferred (+0x260 the record running, +0x264 what it is;
// the deferred call's bit in the record's flags: 2 start, 4 next, 8 stop, 0x10 the input's end).
//
// The data: the source's items (StreamOut, 1) are checked out and fed to cellDmux while it wants them (2, 3); each AU
// found (4) goes to the decoder while it takes AUs (5, 6; video 0xB, 0xC) or to the AU receiver; decoded PCM and
// pictures (7, 0xD) are copied into the renderers' frames by the copy threads, a frame at a time (9-0x14), the
// renderers giving the frames back once played (0x12, 0x14).

#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sail {

namespace {

// The player's playback fields.
constexpr u32 kSession = 4;           // the session word
constexpr u32 kCallRecord = 0xD0;     // the running call's record (+0xE0 its steps', +0x110 the end's, +0x120 a repeat's)
constexpr u32 kPhase = 0x160, kStarts = 0x161, kAudioOpen = 0x164, kVideoOpen = 0x165,
              kFirstFrames = 0x166, kStartInfo = 0x1D0, kStartFlags = 0x1E0, kTimePos = 0x1F0, kEpIndex = 0x208,
              kRate2 = 0x20C, kRate = 0x210, kStartType = 0x214, kRunType = 0x218, kVideoIndex = 0x21C,
              kSeekMode = 0x220, kCallRunning = 0x250, kDeferred = 0x260, kDeferredKind = 0x264, kRepeatMode = 0x198,
              kRepeatInfo = 0x1A0, kRepeatSet = 0x1C8, kPaused = 0x1C9, kAudioModeOpen = 0x1CA, kVideoModeOpen = 0x1CB,
              kNextDescriptor = 0x228;

u32 dmux(u32 p) { return p + pl::kDmux; }
u32 current(PPUContext& ctx, u32 p) { return r32(ctx, p + pl::kCurrentDescriptor); }
u8 phase(PPUContext& ctx, u32 p) { return r8(ctx, p + kPhase); }
bool playing(PPUContext& ctx, u32 p) { return static_cast<u8>(phase(ctx, p) - 3) <= 2; }
bool slot_playing(PPUContext& ctx, u32 slot, u32 state_at) { return static_cast<u8>(r8(ctx, slot + state_at) - 3) <= 2; }
bool has_next(PPUContext& ctx, u32 p) { return r32(ctx, p + kNextDescriptor) != 0; }
u32 entry(PPUContext& ctx, u32 d, u32 table, u32 stride, u32 i) { return r32(ctx, d + table) + i * stride; }

void inc32(PPUContext& ctx, u32 a, u32 by = 1) { w32(ctx, a, r32(ctx, a) + by); }
void inc8(PPUContext& ctx, u32 a, s32 by = 1) { w8(ctx, a, static_cast<u8>(r8(ctx, a) + by)); }

// A record laid out for a step: +0 flags (0x20 in use, 1 failed, 0x1E a deferred call), +4 the steps out, +8 the handle
// of what it belongs to.
void record_begin(PPUContext& ctx, u32 rec, u32 size, u64 handle)
{
    std::memset(gb(ctx) + rec, 0, size);
    w32(ctx, rec, r32(ctx, rec) | 0x20);
    w64(ctx, rec + 8, handle);
}
void mark_failed(PPUContext& ctx, u32 rec) { w32(ctx, rec, r32(ctx, rec) | 1); }
u32 count_down(PPUContext& ctx, u32 rec)
{
    const u32 left = r32(ctx, rec + 4) - 1;
    w32(ctx, rec + 4, left);
    return left;
}
// The record's deferred call (its flags' bits 1-4) replaced by `kind`.
void defer(PPUContext& ctx, u32 rec, u32 kind) { w32(ctx, rec, (r32(ctx, rec) & ~0x1Eu) | kind); }

} // namespace

// ------------------------------------------------------------------------------------------------- the source

// One more item ready while starting or started.
void source_item_ready(PPUContext& ctx, u32 s)
{
    if (r32(ctx, s + src::kState) - 4 <= 1) inc32(ctx, s + src::kReady);
}

bool source_has_items(PPUContext& ctx, u32 s) { return static_cast<s32>(r32(ctx, s + src::kReady)) > 0; }

// An item taken (the source's checkout(arg, out)): one more out, one fewer ready; the last of a drain's items drained
// (9, message 0x18). Its answer.
s32 source_checkout(PPUContext& ctx, u32 s, u32 out)
{
    const s32 r = static_cast<s32>(call_guest(ctx, source_func(ctx, s, src::kCheckout), {r32(ctx, s + src::kArg), out}));
    if (r < 0) return r;
    const u32 drain = r32(ctx, s + src::kDrain);
    inc32(ctx, s + src::kOut);
    w32(ctx, s + src::kReady, r32(ctx, s + src::kReady) - 1);
    if (drain == 0) return r;
    w32(ctx, s + src::kDrain, drain - 1);
    if (drain - 1 == 0 && r32(ctx, s + src::kState) == 8) {
        w32(ctx, s + src::kState, 9);
        post(ctx, r32(ctx, s + src::kPlayer), 0x18, s, 0);
    }
    return r;
}

// An item given back (the source's checkin(arg, item)): the last one out of a drained or stopped source clears it
// (its clear(arg)), open (3), and the call kept completes with the result kept. The checkin's answer, or the clear's.
s32 source_checkin(PPUContext& ctx, u32 s, u32 item)
{
    s32 r = static_cast<s32>(call_guest(ctx, source_func(ctx, s, src::kCheckin), {r32(ctx, s + src::kArg), item}));
    if (r < 0) return r;
    const u32 out = r32(ctx, s + src::kOut) - 1;
    w32(ctx, s + src::kOut, out);
    if (out != 0 || r32(ctx, s + src::kState) - 9 > 1) return r;
    r = static_cast<s32>(call_guest(ctx, source_func(ctx, s, src::kClear), {r32(ctx, s + src::kArg)}));
    const s32 kept = static_cast<s32>(r32(ctx, s + src::kResult));
    w32(ctx, s + src::kState, 3);
    post_completion(ctx, r32(ctx, s + src::kPlayer), kept, r64(ctx, s + src::kHandle));
    return r;
}

// The input's end (libsail's): a started source drains what is ready (8, or at once: message 0x18 and, with nothing
// out, cleared and open, the call completing); `flush` stops it first (its stop(arg)).
void source_end(PPUContext& ctx, u32 s, u64 handle, u32 flush)
{
    if (r32(ctx, s + src::kState) != 5) return;
    if (flush) call_guest(ctx, source_func(ctx, s, src::kStop), {r32(ctx, s + src::kArg), handle, flush});
    const u32 ready = r32(ctx, s + src::kReady);
    w64(ctx, s + src::kHandle, handle);
    w32(ctx, s + src::kDrain, ready);
    if (ready != 0) {
        w32(ctx, s + src::kState, 8);
        return;
    }
    post(ctx, r32(ctx, s + src::kPlayer), 0x18, s, 0);
    if (r32(ctx, s + src::kOut) == 0) {
        call_guest(ctx, source_func(ctx, s, src::kClear), {r32(ctx, s + src::kArg)});
        w32(ctx, s + src::kState, 3);
        post_completion(ctx, r32(ctx, s + src::kPlayer), 0, handle);
    } else {
        w32(ctx, s + src::kState, 9);
    }
}

// Stop (libsail's, by state): closed or open, the call completes with 1; started, stopping (6) with its stop(arg),
// nothing ready or draining any more; draining, done at once when nothing is out (open, the call completing), else
// stopped with items out (10); drained with items out, stopped with items out; others nothing.
void source_stop(PPUContext& ctx, u32 s, u64 handle)
{
    const u32 p = r32(ctx, s + src::kPlayer);
    switch (r32(ctx, s + src::kState)) {
    case 1:
    case 3: post_completion(ctx, p, 1, handle); return;
    case 5:
        w64(ctx, s + src::kHandle, handle);
        w32(ctx, s + src::kDrain, 0);
        w32(ctx, s + src::kState, 6);
        w32(ctx, s + src::kReady, 0);
        call_guest(ctx, source_func(ctx, s, src::kStop), {r32(ctx, s + src::kArg), handle});
        return;
    case 8:
        w64(ctx, s + src::kHandle, handle);
        w32(ctx, s + src::kDrain, 0);
        w32(ctx, s + src::kReady, 0);
        if (r32(ctx, s + src::kOut) != 0) break;
        w32(ctx, s + src::kState, 3);
        w64(ctx, s + src::kHandle, 0);
        post_completion(ctx, p, 0, handle);
        return;
    case 9: w64(ctx, s + src::kHandle, handle); break;
    default: return;
    }
    w32(ctx, s + src::kState, 10);
}

// Start (libsail's): a command of a positive length starts it (4) with its start(arg, {1, position, length, 0, 0},
// session word); else the call completes with INVALID_ARG.
void source_start(PPUContext& ctx, u32 s, u32 cmd, u32 word, u64 handle)
{
    if (static_cast<s64>(r64(ctx, cmd + 8)) <= 0) {
        post_completion(ctx, r32(ctx, s + src::kPlayer), error(CELL_SAIL_ERROR_INVALID_ARG), handle);
        return;
    }
    StackVar start(ctx, gb(ctx), 0x30);
    w64(ctx, s + src::kHandle, handle);
    w32(ctx, s + src::kDrain, 0);
    w32(ctx, s + src::kState, 4);
    w64(ctx, start.addr(), 1);
    w64(ctx, start.addr() + 8, r64(ctx, cmd));
    w64(ctx, start.addr() + 16, r64(ctx, cmd + 8));
    call_guest(ctx, source_func(ctx, s, src::kStart), {r32(ctx, s + src::kArg), start.addr(), word, handle});
}

void source_cancel(PPUContext& ctx, u32 s) { call_guest(ctx, source_func(ctx, s, src::kCancel), {r32(ctx, s + src::kArg)}); }

namespace {

// ------------------------------------------------------------------------------------------------- feeding

// The source's next item fed to the dmux pipeline (libsail checks neither the checkout's answer nor whether it gave an
// item: the pointer is whatever its stack variable held).
void feed_item(PPUContext& ctx, u32 s, u32 dm)
{
    StackVar item(ctx, gb(ctx), 16);
    source_checkout(ctx, s, item.addr());
    dmux_feed(ctx, dm, r32(ctx, item.addr()));
}

// An audio AU to the decoder (put back when it refuses).
void feed_audio(PPUContext& ctx, u32 es, u32 a)
{
    StackVar au(ctx, gb(ctx), 0x30);
    dmux_es_get_au_audio(ctx, es, au.addr());
    if (adec_decode(ctx, a, au.addr()) < 0) dmux_es_unget(ctx, es, au.addr());
}

// A video AU to the decoder: peeked at, taken from cellDmux once the decoder took it.
void feed_video(PPUContext& ctx, u32 es, u32 v)
{
    StackVar au(ctx, gb(ctx), 0x40);
    dmux_es_peek(ctx, es, au.addr());
    if (vdec_decode(ctx, v, au.addr()) == 0) dmux_es_skip_au(ctx, es);
}

// A renderer's free frame to the copy thread.
void give_audio_frame(PPUContext& ctx, u32 a)
{
    StackVar frame(ctx, gb(ctx), 16);
    renderer_checkout(ctx, r32(ctx, a + 0xAC), frame.addr());
    adec_frame(ctx, a, r32(ctx, frame.addr()));
}

void give_video_frame(PPUContext& ctx, u32 v)
{
    StackVar frame(ctx, gb(ctx), 16);
    renderer_checkout(ctx, r32(ctx, v + 0x78), frame.addr());
    vdec_frame(ctx, v, r32(ctx, frame.addr()));
}

void maybe_feed_item(PPUContext& ctx, u32 p)
{
    const u32 dm = dmux(p), s = r32(ctx, dm + dmx::kSource);
    if (source_has_items(ctx, s) && dmux_wants(ctx, dm)) feed_item(ctx, s, dm);
}

// ------------------------------------------------------------------------------------------------- flags

// Paused or not (libsail's): a change sets or clears every renderer's pause flag (8) and every AU receiver's, and is
// event 7; unpausing forgets the first frames shown.
void set_paused(PPUContext& ctx, u32 p, u8 paused)
{
    if (r8(ctx, p + kPaused) == paused) return;
    if (!paused) w8(ctx, p + kFirstFrames, 0);
    w8(ctx, p + kPaused, paused);
    const u32 bit = paused ? 8 : 0;
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        const u32 r = r32(ctx, audio_slot(ctx, p, i) + as::kRenderer);
        w32(ctx, r + 0x34, bit | (r32(ctx, r + 0x34) & ~8u));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        const u32 r = r32(ctx, video_slot(ctx, p, i) + vs::kRenderer);
        w32(ctx, r + 0x34, bit | (r32(ctx, r + 0x34) & ~8u));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) w8(ctx, r32(ctx, user_slot(ctx, p, i) + us::kReceiver) + 0x48, paused);
    notify_event(ctx, p, 7, paused, 0);
}

// The video renderers of the open video streams asked to pause at their first frame (0x100) or not.
void first_frame_pause(PPUContext& ctx, u32 p, u32 d, bool on)
{
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        if (r8(ctx, entry(ctx, d, ds::kVideo, ds::kVideoStride, i)) != 2) continue;
        const u32 r = r32(ctx, video_slot(ctx, p, i) + vs::kRenderer);
        w32(ctx, r + 0x34, (on ? 0x100u : 0u) | (r32(ctx, r + 0x34) & ~0x100u));
    }
}

// The media flag (0x10) of each chosen stream's renderer or receiver.
void media_flags(PPUContext& ctx, u32 p, u32 d, bool on)
{
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        if (!r8(ctx, entry(ctx, d, ds::kAudio, ds::kAudioStride, i))) continue;
        const u32 r = r32(ctx, audio_slot(ctx, p, i) + as::kRenderer);
        w32(ctx, r + 0x34, (on ? 0x10u : 0u) | (r32(ctx, r + 0x34) & ~0x10u));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        if (!r8(ctx, entry(ctx, d, ds::kVideo, ds::kVideoStride, i))) continue;
        const u32 r = r32(ctx, video_slot(ctx, p, i) + vs::kRenderer);
        w32(ctx, r + 0x34, (on ? 0x10u : 0u) | (r32(ctx, r + 0x34) & ~0x10u));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
        if (!r8(ctx, entry(ctx, d, ds::kUser, ds::kUserStride, i))) continue;
        w8(ctx, r32(ctx, user_slot(ctx, p, i) + us::kReceiver) + 0x4A, on ? 1 : 0);
    }
}

[[noreturn]] void container(PPUContext& ctx, u32 d, std::string_view what)
{
    later_part(ctx, std::format("{} of an {} descriptor (the firmware library's table)", what,
        r32(ctx, d + ds::kType) == 1 ? "MP4" : "AVI"));
}

} // namespace

// ------------------------------------------------------------------------------------------------- elementary streams

// Started (libsail's): starting (3) with the handle; the dmux part added and enabled (SDKs up to 0x21FFFF) or enabled;
// then the renderer started (its flag: the stream was only open) and the decoder's sequence (audio 0x75/0x76, video
// 0x78/0x79), or the AU receiver (0x7B); a refusal completes the first step (0x74, 0x77, 0x7A).
void es_start_audio(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u64 handle)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    w64(ctx, slot + 8, handle);
    w8(ctx, slot + as::kState, 3);
    const u32 e = entry(ctx, d, ds::kAudio, ds::kAudioStride, index);
    const s32 r = sdk_after_22(ctx) ? dmux_es_enable(ctx, slot + as::kDmuxEs)
                                    : dmux_es_add(ctx, dmux(p), 0, e + 0xC, e + 0x1C, slot, slot + as::kDmuxEs, true);
    if (r < 0) {
        w32(ctx, slot + 4, 1);
        post_completion(ctx, p, r, step(0x74, slot));
        return;
    }
    w32(ctx, slot + 4, 2);
    audio_renderer_start(ctx, r32(ctx, slot + as::kRenderer), phase(ctx, p) == 2 ? 1 : 0, step(0x76, slot));
    adec_start(ctx, slot + as::kPipeline, step(0x75, slot));
}

void es_start_video(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u64 handle)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    w64(ctx, slot + 8, handle);
    w8(ctx, slot + vs::kState, 3);
    const u32 e = entry(ctx, d, ds::kVideo, ds::kVideoStride, index);
    const s32 r = sdk_after_22(ctx) ? dmux_es_enable(ctx, slot + vs::kDmuxEs)
                                    : dmux_es_add(ctx, dmux(p), 1, e + 0xC, e + 0x1C, slot, slot + vs::kDmuxEs, true);
    if (r < 0) {
        w32(ctx, slot + 4, 1);
        post_completion(ctx, p, r, step(0x77, slot));
        return;
    }
    w32(ctx, slot + 4, 2);
    video_renderer_start(ctx, r32(ctx, slot + vs::kRenderer), phase(ctx, p) == 2 ? 1 : 0, step(0x79, slot));
    // libsail sets the decoder's mode (+0x10 of the pipeline) to 1 for EP skipping or a time scale past 2, then to 0.
    const u32 type = r32(ctx, p + kStartType);
    const float rate2 = std::bit_cast<float>(r32(ctx, p + kRate2));
    w32(ctx, slot + vs::kPipeline + 0x10, (type == 16 || (type == 4 && rate2 > 2.0f)) ? 1 : 0);
    w32(ctx, slot + vs::kPipeline + 0x10, 0);
    vdec_start(ctx, slot + vs::kPipeline, r8(ctx, e + 0x3B), step(0x78, slot));
}

void es_start_user(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 index, u64 handle)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    w64(ctx, slot + 8, handle);
    w8(ctx, slot + us::kState, 3);
    const u32 e = entry(ctx, d, ds::kUser, ds::kUserStride, index);
    const s32 r = sdk_after_22(ctx) ? dmux_es_enable(ctx, slot + us::kDmuxEs)
                                    : dmux_es_add(ctx, dmux(p), 2, e + 0x3C, e + 0xC, slot, slot + us::kDmuxEs, true);
    w32(ctx, slot + 4, 1);
    if (r < 0) {
        post_completion(ctx, p, r, step(0x7A, slot));
        return;
    }
    receiver_start(ctx, r32(ctx, slot + us::kReceiver), phase(ctx, p) == 2 ? 1 : 0, step(0x7B, slot));
}

// Stopped (libsail's): stopping (6), the decoder's pipeline flushed and the renderer stopped (audio 0x85/0x86, video
// 0x88/0x89), or the AU receiver (0x8B); `failed` marks the record (a failed start being undone).
void es_stop_audio(PPUContext& ctx, u32, u32, u32 slot, u32, u32, u64 handle, bool failed)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    if (failed) mark_failed(ctx, slot);
    w64(ctx, slot + 8, handle);
    w8(ctx, slot + as::kState, 6);
    adec_stop(ctx, slot + as::kPipeline, step(0x85, slot));
    audio_renderer_stop(ctx, r32(ctx, slot + as::kRenderer), step(0x86, slot));
    w32(ctx, slot + 4, 2);
}

void es_stop_video(PPUContext& ctx, u32, u32, u32 slot, u32, u32 keep, u64 handle, bool failed)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    if (failed) mark_failed(ctx, slot);
    w64(ctx, slot + 8, handle);
    w8(ctx, slot + vs::kState, 6);
    vdec_stop(ctx, slot + vs::kPipeline, step(0x88, slot));
    video_renderer_stop(ctx, r32(ctx, slot + vs::kRenderer), keep, step(0x89, slot));
    w32(ctx, slot + 4, 2);
}

void es_stop_user(PPUContext& ctx, u32, u32, u32 slot, u32, u32, u64 handle, bool failed)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    if (failed) mark_failed(ctx, slot);
    w64(ctx, slot + 8, handle);
    w32(ctx, slot + 4, 1);
    w8(ctx, slot + us::kState, 6);
    receiver_stop(ctx, r32(ctx, slot + us::kReceiver), step(0x8B, slot));
}

namespace {

// ------------------------------------------------------------------------------------------------- the start command

// The start information (the player's +0x1D0, libsail's) from a CellSailStartCommand {start type, seek type, terminus
// type, flags, start argument, 0, seek argument (u64), terminus argument (u64)} or none: +0 the byte to start from,
// +8 the byte to end at, +0x10 the flags, +0x20 a time to start from (-1 none), +0x3C and +0x40 the rate, +0x44 the
// start type (1 normal, 4 time scale, 16 EP skip; 32 is 16 backwards, 64 a frame step: 4 with flag 2), +0x50 the seek
// mode (0 from the byte, 1 where it is, 2 where it is, by EP). `started`: the stream is playing (start type 0 keeps the
// type running, seek type 0 where it is). The terminus by time looks up the entry point at that time in the stream
// libsail keeps at +0x4F (its first video stream).
void start_info(PPUContext& ctx, u32 d, u32 cmd, u32 info, bool started)
{
    u64 pos = r64(ctx, info), end = r64(ctx, info + 8);
    float rate = std::bit_cast<float>(r32(ctx, info + 0x3C));
    const u32 running = r32(ctx, info + 0x48);
    const u64 size = r64(ctx, d + ds::kStreamSize);
    const bool pamf = r32(ctx, d + ds::kType) == 0;
    u64 flags = 0, time = ~u64{0};
    u32 type = 1, mode = 0;
    if (cmd) {
        type = r32(ctx, cmd);
        flags = r32(ctx, cmd + 0xC);
        bool backwards = false;
        if (type == 4 || type == 16 || type == 32) {
            const u32 arg = r32(ctx, cmd + 0x10);
            rate = arg == 0 ? 0.0625f : static_cast<float>(static_cast<double>(arg)) * 0.00390625f;
            backwards = type == 32;
        } else if (type == 0) {
            if (!started) {
                type = 1;
                rate = 1.0f;
            } else {
                type = running;
                backwards = running == 32;
            }
        } else if (type == 1) {
            rate = 1.0f;
        } else if (type == 64) {
            flags |= 2;
            rate = 1.0f;
            type = 4;
        }
        if (backwards) {
            rate = -rate;
            type = 16;
        }
        // The seek type.
        const u32 seek = r32(ctx, cmd + 4);
        bool by_byte = false, here = false;
        if (seek == 1) {
            by_byte = true;
        } else if (seek == 0) {
            if (started) here = true;
            else pos = 0;
        } else if (seek == 16) {
            if (!pamf) by_byte = true;
            else time = r64(ctx, cmd + 0x18);
        } else if (seek == 64) {
            here = true;
        }
        if (by_byte) pos = r64(ctx, cmd + 0x18);
        if (here) mode = type == 16 || running == 16 ? 2 : 1;
        // The terminus type.
        const u32 terminus = r32(ctx, cmd + 8);
        if (terminus == 1) {
            end = size;
        } else if (terminus == 0) {
            end = rate < 0.0f ? 0 : size;
        } else if (terminus == 2 || (terminus == 32 && !pamf)) {
            end = r64(ctx, cmd + 0x20);
        } else if (terminus == 32) {
            const u32 reader = d + ds::kReader;
            const u64 ts = r64(ctx, cmd + 0x20);
            StackVar v(ctx, gb(ctx), 0x40); // +0 the time, +8 the iterator, +0x18 the entry point
            end = ~u64{0};
            if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderSetStreamWithIndex",
                    {reader, r8(ctx, info + 0x4F)})) >= 0) {
                w32(ctx, v.addr(), static_cast<u32>(ts >> 32));
                w32(ctx, v.addr() + 4, static_cast<u32>(ts));
                if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderGetEpIteratorWithTimeStamp",
                        {reader, v.addr(), v.addr() + 8})) >= 0 &&
                    static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfEpIteratorGetEp",
                        {v.addr() + 8, v.addr() + 0x18})) >= 0)
                    end = r64(ctx, v.addr() + 0x28);
            }
        }
    } else {
        end = size;
        rate = 1.0f;
        pos = 0;
    }
    w64(ctx, info + 0x20, time);
    w64(ctx, info, pos);
    w64(ctx, info + 0x10, flags);
    w32(ctx, info + 0x44, type);
    w32(ctx, info + 0x3C, std::bit_cast<u32>(rate));
    w32(ctx, info + 0x50, mode);
    w32(ctx, info + 0x40, std::bit_cast<u32>(rate));
    w64(ctx, info + 8, end);
}

// The source's start command from the start information (libsail's): from a time, the byte of the entry point at it
// (the EP's index kept for an EP skip); an EP skip goes on in ep_skip_start; else {the byte past the header, the length
// to the end, 0}. 1, or an error.
s32 source_command(PPUContext& ctx, u32 d, u32 info, u32 out)
{
    const u64 time = r64(ctx, info + 0x20);
    u64 pos = r64(ctx, info);
    const u64 end = r64(ctx, info + 8);
    const u32 header = r32(ctx, d + ds::kHeaderSize);
    u32 ep_index = 0xFFFFFFFF;
    if (time != ~u64{0}) {
        const u32 type = r32(ctx, d + ds::kType);
        if (type == 1 || type == 2) container(ctx, d, "a start from a time");
        if (type == 0) {
            const u32 reader = d + ds::kReader;
            StackVar v(ctx, gb(ctx), 0x50); // +4 the time, +0xC the iterator, +0x20 the entry point
            u64 found = ~u64{0};
            ep_index = 0;
            if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderSetStreamWithIndex",
                    {reader, r32(ctx, info + 0x4C) & 0xFF})) >= 0) {
                w32(ctx, v.addr() + 4, static_cast<u32>(time >> 32));
                w32(ctx, v.addr() + 8, static_cast<u32>(time));
                if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderGetEpIteratorWithTimeStamp",
                        {reader, v.addr() + 4, v.addr() + 0xC})) >= 0) {
                    ep_index = r32(ctx, v.addr() + 0x10);
                    if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfEpIteratorGetEp",
                            {v.addr() + 0xC, v.addr() + 0x20})) >= 0)
                        found = r64(ctx, v.addr() + 0x30);
                }
            }
            if (found != ~u64{0}) pos = found;
        }
    }
    if (r32(ctx, info + 0x44) == 16) {
        w32(ctx, info + 0x38, ep_index);
        w64(ctx, info + 0x30, 0);
        w64(ctx, info + 0x28, 0);
        later_part(ctx, "an EP skip's start (libsail's entry point walk)");
    }
    w32(ctx, out + 0x1C, 0);
    w32(ctx, out + 0x18, 0);
    w64(ctx, out, pos + header);
    w64(ctx, out + 8, end - pos);
    return 1;
}

// The source started for a session (libsail's): the repeat information's start command checked again when asked;
// the frame step's pause (flag 2) when no video stream is open; the session word: its low 10 bits count sessions (by
// 128, then by one; a repeat by one), 0x10000 not the normal start type, 0x20000 the complete-stream flag (8), 0x40000
// EP skipping, 0x100000 a frame step, 0x200000 the end pausing (flag 4) with nothing to repeat into, bits 12-15 and
// 10-11 the audio and video streams open (less those of mode 1), bits 22-31 the rate (x256: a 6-bit mantissa and a
// 4-bit exponent). The source's start (its command from the start information) completes `handle`.
void source_session(PPUContext& ctx, u32 p, u32 d, bool recheck, bool repeat, u64 handle)
{
    const u32 info = p + kStartInfo;
    if (recheck && r8(ctx, p + kRepeatSet)) {
        u32 cmd = p + kRepeatInfo;
        if (r32(ctx, cmd + 4) == 64 || static_cast<s32>(module_call(ctx, "cellSail", "cellSailDescriptorInquireCapability",
                                            {d, cmd})) < 0)
            cmd = 0;
        start_info(ctx, d, cmd, info, false);
    }
    const u32 old = r32(ctx, p + kSession);
    const bool step_flag = (r64(ctx, p + kStartFlags) & 2) != 0;
    if (step_flag && r8(ctx, p + kVideoOpen) == 0) set_paused(ctx, p, 1);
    StackVar cmd(ctx, gb(ctx), 0x30);
    u32 count, extra = 0, normal = 0;
    s32 r = 0;
    if (!repeat) {
        count = old + 128;
        if ((count & 0x380) == 0) count = (count & 0x7F) == 0x7F ? 1 : old + 129;
        if (step_flag) {
            w64(ctx, p + kStartFlags, r64(ctx, p + kStartFlags) & ~u64{2});
            extra = 0x100000;
        }
        r = source_command(ctx, d, info, cmd.addr());
        normal = 1;
    } else {
        count = old + 1;
        later_part(ctx, "an EP skip going on (libsail's entry point walk)");
    }
    if (r != 0) {
        if (r < 0) {
            post_completion(ctx, p, r, handle);
            return;
        }
        const u64 flags = r64(ctx, p + kStartFlags);
        if (flags & 4) {
            const u32 mode = r32(ctx, p + kRepeatMode);
            if (mode == 0 || (mode == 2 && !has_next(ctx, p))) {
                w64(ctx, p + kStartFlags, flags & ~u64{4});
                extra |= 0x200000;
            }
        }
        w32(ctx, p + kEpIndex, 0xFFFFFFFF);
    }
    u32 low = count & 0x3FF;
    if (low == 0) low = 1;
    const u32 type = r32(ctx, p + kStartType);
    const u32 audio = r8(ctx, p + kAudioOpen), audio_mode = r8(ctx, p + kAudioModeOpen);
    const u32 video = r8(ctx, p + kVideoOpen), video_mode = r8(ctx, p + kVideoModeOpen);
    bool check_complete = true;
    if (type != 1) {
        extra |= 0x10000;
        if (type == 16) {
            extra |= 0x40000;
            check_complete = false;
        }
    }
    if (check_complete && (r64(ctx, p + kStartFlags) & 8)) extra |= 0x20000;
    // The rate: |rate| x 256 truncated, its low 16 bits (1 when 0) as a 6-bit mantissa below its leading 1 and the
    // leading 1's position.
    const double scaled = std::fabs(static_cast<double>(std::bit_cast<float>(r32(ctx, p + kRate)))) * 256.0;
    const s64 whole = std::isnan(scaled) || scaled >= 9223372036854775808.0 ? static_cast<s64>(0x8000000000000000ull)
                    : scaled <= -9223372036854775808.0                     ? static_cast<s64>(0x8000000000000000ull)
                                                                            : static_cast<s64>(scaled);
    u32 v = static_cast<u32>(whole) & 0xFFFF;
    if (v == 0) v = 1;
    const u32 lz = static_cast<u32>(std::countl_zero(v));
    const s32 shift = 25 - static_cast<s32>(lz);
    const u32 mant = shift >= 0 ? v >> shift : v << -shift;
    const u32 word = low | extra | (((audio - audio_mode) << 12) & 0xF000) | (((video - video_mode) << 10) & 0xC00) |
                     (((mant & 0x3F) | ((31 - lz) << 6)) << 22);
    w32(ctx, cmd.addr() + 0x10, normal);
    w32(ctx, cmd.addr() + 0x14, 0);
    w32(ctx, p + kSession, word);
    const u32 type_d = r32(ctx, d + ds::kType);
    if (type_d == 1 || type_d == 2) container(ctx, d, "the source's start");
    source_start(ctx, r32(ctx, d + ds::kSource), cmd.addr(), word, handle);
}

// The stream stopped (libsail's): stopping (6), the video renderers' first-frame pause off, the source stopped (0x7F),
// the dmux pipeline reset (0x80) and every open elementary stream stopped (0x81-0x83; `keep` the video renderers' last
// frame), the record at `rec` counting them and completing `handle`.
void stream_stop(PPUContext& ctx, u32 p, u32 d, u32 rec, u32 keep, u64 handle)
{
    record_begin(ctx, rec, 0x10, handle);
    w8(ctx, p + kPhase, 6);
    first_frame_pause(ctx, p, d, false);
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) container(ctx, d, "the stream's stop");
    source_stop(ctx, r32(ctx, d + ds::kSource), step(0x7F, rec));
    dmux_reset(ctx, dmux(p), step(0x80, rec));
    u32 n = 2;
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        if (r8(ctx, entry(ctx, d, ds::kAudio, ds::kAudioStride, i)) != 2) continue;
        n++;
        es_stop_audio(ctx, p, d, audio_slot(ctx, p, i), i, keep, step(0x81, rec), false);
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        if (r8(ctx, entry(ctx, d, ds::kVideo, ds::kVideoStride, i)) != 2) continue;
        n++;
        es_stop_video(ctx, p, d, video_slot(ctx, p, i), i, keep, step(0x82, rec), false);
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
        if (r8(ctx, entry(ctx, d, ds::kUser, ds::kUserStride, i)) != 2) continue;
        n++;
        es_stop_user(ctx, p, d, user_slot(ctx, p, i), i, keep, step(0x83, rec), false);
    }
    w32(ctx, rec + 4, n & 0xFF);
}

// Stop (message 0x7D's work, libsail's): the media flags off, the stream stopped, completing 0x7E.
void stop_call(PPUContext& ctx, u32 p, u32 d)
{
    std::memset(gb(ctx) + p + kCallRecord, 0, 0x20);
    media_flags(ctx, p, d, false);
    stream_stop(ctx, p, d, p + 0xE0, 0, step(0x7E, p + kCallRecord));
}

// The stream ending (libsail's): ending (5) with one step (the source's end, 0xBE; `flush` stops the source first);
// ending already, only the handle is replaced.
void stream_end(PPUContext& ctx, u32 p, u32 d, u32 rec, u64 handle, u32 flush)
{
    if (phase(ctx, p) == 5 && r32(ctx, d + ds::kType) == 0) {
        w64(ctx, rec + 8, handle);
        return;
    }
    record_begin(ctx, rec, 0x10, handle);
    w32(ctx, rec + 4, 1);
    w8(ctx, p + kPhase, 5);
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) container(ctx, d, "the stream's end");
    source_end(ctx, r32(ctx, d + ds::kSource), step(0xBE, rec), flush & 0xFF);
}

// The streams started (libsail's): `rec` (its handle completing when all are in) becomes the record running (+0x260,
// kind 1); every open elementary stream started (0x71-0x73) and the dmux pipeline (0x70), or, the streams started
// already (phase 4), the source (0x6F); starting (3). The first open video stream's index into the descriptor's
// reader is kept (+0x21C).
void streams_start(PPUContext& ctx, u32 p, u32 d, u32 rec, u64 handle)
{
    std::memset(gb(ctx) + rec, 0, 0x20);
    w64(ctx, rec + 8, handle);
    w32(ctx, p + kDeferred, rec);
    w32(ctx, rec, r32(ctx, rec) | 0x20);
    const bool start_es = phase(ctx, p) != 4;
    w32(ctx, p + kDeferredKind, 1);
    u32 n = 0;
    const auto open = [&](u8 st) { return st == 2 || st == 4 || st == 5; };
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        const u32 slot = audio_slot(ctx, p, i);
        if (!open(r8(ctx, slot + as::kState)) || !start_es) continue;
        n++;
        es_start_audio(ctx, p, d, slot, i, step(0x71, rec));
    }
    bool first = true;
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        const u32 slot = video_slot(ctx, p, i);
        if (!open(r8(ctx, slot + vs::kState))) continue;
        if (first) w32(ctx, p + kVideoIndex, r32(ctx, entry(ctx, d, ds::kVideo, ds::kVideoStride, i) + 4));
        first = false;
        if (!start_es) continue;
        n++;
        es_start_video(ctx, p, d, slot, i, step(0x72, rec));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
        const u32 slot = user_slot(ctx, p, i);
        if (!open(r8(ctx, slot + us::kState)) || !start_es) continue;
        n++;
        es_start_user(ctx, p, d, slot, i, step(0x73, rec));
    }
    u32 steps = 0;
    if (start_es) {
        dmux_start(ctx, dmux(p), step(0x70, rec));
        steps = (n + 1) & 0xFF;
    } else {
        source_session(ctx, p, d, false, false, step(0x6F, rec));
    }
    w8(ctx, p + kPhase, 3);
    w32(ctx, rec + 4, steps);
}

// Start (message 0x6C's work, libsail's): not playing, the clock gets the rate and forgets its play state; seeking by
// time where it is (mode 2), the clock's time kept as the time to start from. Then unpaused and the streams started
// (0x6D), or, playing and not flagged to keep going (flag 1), stopped first (0x6E). Seeking where it is (mode 1) while
// playing only changes the rate and the flags: the audio renderers' rate flag (0x20), a frame step's video renderers
// (unpaused, 0x20), the end's pause, the clock running unless at the normal rate, paused when no video stream is open,
// and the start completes at once.
void start_call(PPUContext& ctx, u32 p, u32 d)
{
    const float rate = std::bit_cast<float>(r32(ctx, p + kRate));
    if (!playing(ctx, p)) {
        clock_set_rate(ctx, p + pl::kSync, rate);
        clock_reset_play(ctx, p + pl::kSync);
    } else {
        const u32 mode = r32(ctx, p + kSeekMode);
        if (mode == 1) {
            u64 flags = r64(ctx, p + kStartFlags);
            const bool not_normal = r32(ctx, p + kStartType) != 1;
            const bool frame_step = (flags >> 1) & 1;
            bool end_pause = false;
            if (flags & 4) {
                const u32 repeat = r32(ctx, p + kRepeatMode);
                end_pause = repeat == 0 || (repeat == 2 && !has_next(ctx, p));
            }
            const bool rate_flag = not_normal || frame_step;
            for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
                const u32 slot = audio_slot(ctx, p, i);
                const u8 st = r8(ctx, slot + as::kState);
                if (st != 2 && st != 4 && st != 5) continue;
                const u32 r = r32(ctx, slot + as::kRenderer);
                w32(ctx, r + 0x34, (rate_flag ? 0x20u : 0u) | (r32(ctx, r + 0x34) & ~0x20u));
            }
            if (frame_step) {
                flags &= ~u64{2};
                w64(ctx, p + kStartFlags, flags);
                for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
                    const u32 slot = video_slot(ctx, p, i);
                    const u8 st = r8(ctx, slot + vs::kState);
                    if (st != 2 && st != 4 && st != 5) continue;
                    const u32 r = r32(ctx, slot + vs::kRenderer);
                    w32(ctx, r + 0x34, (r32(ctx, r + 0x34) & ~8u) | 0x20);
                }
            }
            if (end_pause) {
                w64(ctx, p + kStartFlags, flags & ~u64{4});
                first_frame_pause(ctx, p, d, true);
            }
            clock_run(ctx, p + pl::kSync, not_normal ? 1 : 0);
            clock_set_rate(ctx, p + pl::kSync, rate);
            set_paused(ctx, p, r8(ctx, p + kVideoOpen) == 0 ? 1 : 0);
            post_completion(ctx, p, 0, step(0x6D, 0));
            return;
        }
        if (mode == 2) {
            const u64 t = clock_time(ctx, p + pl::kSync);
            w64(ctx, p + kTimePos, t == ~u64{0} ? 0 : t);
        }
    }
    set_paused(ctx, p, 0);
    std::memset(gb(ctx) + p + kCallRecord, 0, 0x30);
    if (static_cast<u8>(phase(ctx, p) - 4) <= 1 && !(r64(ctx, p + kStartFlags) & 1)) {
        stream_stop(ctx, p, d, p + 0xE0, 1, step(0x6E, p + kCallRecord));
        return;
    }
    streams_start(ctx, p, d, p + 0xE0, step(0x6D, p + kCallRecord));
}

// The source's end of input (message 0x17's work, libsail's): an EP skip goes on from the next entry point (0xC8); else,
// repeating the stream (mode 1) the source starts again from the start information (0xCA), repeating into the next
// descriptor (mode 2, one held) that one is made ready (0xCB); else the stream ends.
void input_end(PPUContext& ctx, u32 p, u32 d)
{
    if (!playing(ctx, p)) return;
    if (r32(ctx, p + kRunType) == 16) {
        const u32 rec = p + 0x120;
        std::memset(gb(ctx) + rec, 0, 0x30);
        w32(ctx, p + kDeferred, rec);
        w32(ctx, rec, r32(ctx, rec) | 0x20);
        w32(ctx, p + kDeferredKind, 3);
        if (r32(ctx, p + kEpIndex) != 0xFFFFFFFF) {
            source_session(ctx, p, d, false, true, step(0xC8, rec));
            return;
        }
        w32(ctx, p + kDeferred, 0);
        w32(ctx, p + kDeferredKind, 0);
    }
    if (r32(ctx, p + kSeekMode) == 2) w64(ctx, p + kTimePos, ~u64{0});
    const u32 mode = r32(ctx, p + kRepeatMode);
    if (mode == 1) {
        const u32 rec = p + 0x120;
        std::memset(gb(ctx) + rec, 0, 0x30);
        w32(ctx, rec, r32(ctx, rec) | 0x20);
        w32(ctx, p + kDeferred, rec);
        w32(ctx, p + kDeferredKind, 3);
        source_session(ctx, p, d, true, false, step(0xCA, rec));
        return;
    }
    if (mode == 2 && has_next(ctx, p)) later_part(ctx, "the stream repeated into the next descriptor");
    if (mode != 0 && mode != 2) return;
    stream_end(ctx, p, d, p + 0x110, 0, 0);
}

// ------------------------------------------------------------------------------------------------- Next

// Next moves the player on to the next descriptor (+0x228; libsail's): the stream running stopped (or, with the start
// flag 1, ended; 0xA9), the next descriptor made ready (0xAA, its start information from the repeat command), then
// switched to (the records at +0xD0 and +0xE0): the source closed and the next one's opened (0xAB, 0xB2) or the same
// source told of it (0xB2), the dmux pipeline reopened when the kind differs (0xAC, 0xB3), each stream slot kept when the
// next descriptor's stream is compatible (its dmux part, decoder and renderer each compared) or its decoder and
// renderer closed and reopened for the new format (0xAD-0xB1, 0xB7-0xBB; their completions 0xB4-0xB6 on the switch's
// record); once all are in, the current descriptor's streams are chosen again (event 10), it is closed for the player
// (1, event 5), the next one is current (event 9 for each stream kept, event 4) and its streams start (or a call deferred
// meanwhile runs). Next completes (0xA8) in state 6 with the run type from the start information.

// The next descriptor's chosen streams given the formats of the current slots' renderers.
void next_out_formats(PPUContext& ctx, u32 p, u32 nd)
{
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        const u32 e = entry(ctx, nd, ds::kAudio, ds::kAudioStride, i);
        if (r8(ctx, e)) audio_out_format(ctx, e, audio_slot(ctx, p, i));
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        const u32 e = entry(ctx, nd, ds::kVideo, ds::kVideoStride, i);
        if (r8(ctx, e)) video_out_format(ctx, e, video_slot(ctx, p, i));
    }
}

// The comparisons of a descriptor's stream entry with the next one's (libsail's): the dmux part (the ES filter's four
// words; for PAMF, by the audio coding: ATRAC3plus +28/+32/+36, LPCM +28, others the coding and +28; video +28; user
// the filter at +60 and +72), the decoder (audio: coding, channels, rate, frame; video: coding, profile, the specific
// information's bytes (8 for coding 5, else as many as +44 says), the picture and its format out), the renderer's
// format (audio the stream's 16 bytes at +96; video the 18 at +88).
bool same_words(PPUContext& ctx, u32 a, u32 b, u32 at, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (r32(ctx, a + at + 4 * i) != r32(ctx, b + at + 4 * i)) return false;
    return true;
}
bool same_audio_part(PPUContext& ctx, u32 type, u32 a, u32 b)
{
    if (!same_words(ctx, a, b, 12, 4)) return false;
    if (type != 0) return true;
    const u32 coding = r32(ctx, a + 40);
    if (coding == 1) return same_words(ctx, a, b, 28, 3);
    if (coding == 2) return r32(ctx, a + 28) == r32(ctx, b + 28);
    return r32(ctx, b + 40) == coding && r32(ctx, a + 28) == r32(ctx, b + 28);
}
bool same_video_part(PPUContext& ctx, u32 type, u32 a, u32 b)
{
    if (!same_words(ctx, a, b, 12, 4)) return false;
    return type != 0 || r32(ctx, a + 28) == r32(ctx, b + 28);
}
bool same_user_part(PPUContext& ctx, u32 a, u32 b) { return same_words(ctx, a, b, 60, 4); }
bool same_audio_decoder(PPUContext& ctx, u32 a, u32 b)
{
    return r32(ctx, a + 40) == r32(ctx, b + 40) && r8(ctx, a + 97) == r8(ctx, b + 97) &&
           r32(ctx, a + 100) == r32(ctx, b + 100) && ps_read16(gb(ctx), a + 98) == ps_read16(gb(ctx), b + 98);
}
bool same_video_decoder(PPUContext& ctx, u32 a, u32 b)
{
    u8* m = gb(ctx);
    if (!same_words(ctx, a, b, 32, 2)) return false;
    const u32 n = r32(ctx, b + 32) == 5 ? 8 : r32(ctx, b + 44);
    if (n != 0 && std::memcmp(m + a + 44, m + b + 44, std::max<u32>(n, 1)) != 0) return false;
    for (const u32 f : {56u, 88u})
        if (ps_read16(m, a + f + 4) != ps_read16(m, b + f + 4) || ps_read16(m, a + f + 6) != ps_read16(m, b + f + 6) ||
            m[a + f + 2] != m[b + f + 2] || m[a + f] != m[b + f])
            return false;
    return true;
}
bool same_audio_format(PPUContext& ctx, u32 a, u32 b)
{
    return std::memcmp(gb(ctx) + a + 96, gb(ctx) + b + 96, 4) == 0 && same_words(ctx, a, b, 100, 2);
}
bool same_video_format(PPUContext& ctx, u32 a, u32 b) { return std::memcmp(gb(ctx) + a + 88, gb(ctx) + b + 88, 18) == 0; }

// The switch to the next descriptor (libsail's): `rec` (+0x260 the record running, kind 2) counts the source's and the
// dmux pipeline's steps and each slot that is not kept; its slots' records (+0 cleared, their handles the switch's
// steps 0xB4-0xB6) count their closes. One step at least (the source's), so the record always waits. The player's
// descriptor count drops by one.
void descriptor_switch(PPUContext& ctx, u32 p, u32 rec)
{
    const u32 nd = r32(ctx, p + kNextDescriptor), od = current(ctx, p), dm = dmux(p);
    w32(ctx, p + kDeferredKind, 2);
    w32(ctx, p + kDeferred, rec);
    next_out_formats(ctx, p, nd);
    clock_reset(ctx, p + pl::kSync);
    if (r32(ctx, od + ds::kSource) != r32(ctx, nd + ds::kSource)) {
        if (const u32 t = r32(ctx, od + ds::kType); t == 1 || t == 2) container(ctx, od, "Next's source close");
        source_close(ctx, r32(ctx, od + ds::kSource), step(0xAB, rec));
    } else {
        if (const u32 t = r32(ctx, nd + ds::kType); t == 1 || t == 2) container(ctx, nd, "Next's source open");
        source_open(ctx, r32(ctx, nd + ds::kSource), nd, step(0xB2, rec));
    }
    const bool same_dmux = r32(ctx, od + ds::kHeader) == r32(ctx, nd + ds::kHeader);
    u8 steps = 1;
    if (!same_dmux) {
        steps = 2;
        dmux_close(ctx, dm, step(0xAC, rec));
    } else {
        w32(ctx, dm + dmx::kSource, r32(ctx, nd + ds::kSource));
    }
    const u32 type = r32(ctx, od + ds::kType);
    for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
        const u32 slot = audio_slot(ctx, p, i), oe = entry(ctx, od, ds::kAudio, ds::kAudioStride, i),
                  ne = entry(ctx, nd, ds::kAudio, ds::kAudioStride, i);
        std::memset(gb(ctx) + slot, 0, 0x10);
        w32(ctx, rec, r32(ctx, rec) | 0x20);
        w64(ctx, slot + 8, step(0xB4, rec));
        const bool gone = r8(ctx, slot + as::kState) != 2 || r8(ctx, oe) == 0, chosen = r8(ctx, ne) == 1;
        w8(ctx, slot + as::kState, 0);
        const u64 hp = step(0xAD, slot), hr = step(0xAE, slot);
        if (gone) {
            post_completion(ctx, p, 0, hp);
            post_completion(ctx, p, 0, hr);
            steps++;
            w32(ctx, slot + 4, 2);
            continue;
        }
        if (same_dmux && (!chosen || !same_audio_part(ctx, type, oe, ne))) dmux_es_remove(ctx, dm, slot + as::kDmuxEs);
        u32 n = 0;
        if (!chosen || !same_audio_decoder(ctx, oe, ne)) {
            audio_pipeline_close(ctx, slot + as::kPipeline, hp);
            n = chosen ? 1 : 2;
        }
        if (!chosen) audio_renderer_close(ctx, r32(ctx, slot + as::kRenderer), hr);
        else if (!same_audio_format(ctx, oe, ne)) {
            n++;
            audio_renderer_close(ctx, r32(ctx, slot + as::kRenderer), hr);
        }
        if (n == 0) {
            if (chosen) w8(ctx, slot + as::kState, 2);
        } else {
            steps++;
        }
        w32(ctx, slot + 4, n);
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
        const u32 slot = video_slot(ctx, p, i), oe = entry(ctx, od, ds::kVideo, ds::kVideoStride, i),
                  ne = entry(ctx, nd, ds::kVideo, ds::kVideoStride, i);
        std::memset(gb(ctx) + slot, 0, 0x10);
        w32(ctx, rec, r32(ctx, rec) | 0x20);
        w64(ctx, slot + 8, step(0xB5, rec));
        const bool gone = r8(ctx, slot + vs::kState) != 2 || r8(ctx, oe) == 0, chosen = r8(ctx, ne) == 1;
        w8(ctx, slot + vs::kState, 0);
        const u64 hp = step(0xAF, slot), hr = step(0xB0, slot);
        if (gone) {
            post_completion(ctx, p, 0, hp);
            post_completion(ctx, p, 0, hr);
            steps++;
            w32(ctx, slot + 4, 2);
            continue;
        }
        if (same_dmux && (!chosen || !same_video_part(ctx, type, oe, ne))) dmux_es_remove(ctx, dm, slot + vs::kDmuxEs);
        u32 n = 0;
        if (!chosen || !same_video_decoder(ctx, oe, ne)) {
            video_pipeline_close(ctx, slot + vs::kPipeline, hp);
            n = chosen ? 1 : 2;
        }
        if (!chosen) video_renderer_close(ctx, r32(ctx, slot + vs::kRenderer), hr);
        else if (!same_video_format(ctx, oe, ne)) {
            n++;
            video_renderer_close(ctx, r32(ctx, slot + vs::kRenderer), hr);
        }
        if (n == 0) {
            if (chosen) w8(ctx, slot + vs::kState, 2);
        } else {
            steps++;
        }
        w32(ctx, slot + 4, n);
    }
    for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
        const u32 slot = user_slot(ctx, p, i), oe = entry(ctx, od, ds::kUser, ds::kUserStride, i),
                  ne = entry(ctx, nd, ds::kUser, ds::kUserStride, i);
        std::memset(gb(ctx) + slot, 0, 0x10);
        w32(ctx, rec, r32(ctx, rec) | 0x20);
        w64(ctx, slot + 8, step(0xB6, rec));
        const bool gone = r8(ctx, slot + us::kState) != 2 || r8(ctx, oe) == 0, chosen = r8(ctx, ne) == 1;
        w8(ctx, slot + us::kState, 0);
        const u64 h = step(0xB1, slot);
        if (gone) {
            post_completion(ctx, p, 0, h);
            steps++;
            w32(ctx, slot + 4, 1);
            continue;
        }
        if (same_dmux && (!chosen || !same_user_part(ctx, oe, ne))) dmux_es_remove(ctx, dm, slot + us::kDmuxEs);
        if (!chosen) {
            receiver_close(ctx, r32(ctx, slot + us::kReceiver), h);
            steps++;
            w32(ctx, slot + 4, 1);
            continue;
        }
        w32(ctx, slot + 4, 0);
        w8(ctx, slot + us::kState, 2);
    }
    w32(ctx, rec + 4, steps);
    w32(ctx, p + pl::kDescriptorCount, r32(ctx, p + pl::kDescriptorCount) - 1);
}

// The next descriptor made ready (libsail's): `rec` (0x30 bytes) with its handle, the descriptor's preparation on its
// +0x10 completing 0xAA.
void next_prepare(PPUContext& ctx, u32 p, u32 rec, u64 handle)
{
    record_begin(ctx, rec, 0x30, handle);
    prepare_descriptor(ctx, p, r32(ctx, p + kNextDescriptor), rec + 0x10, step(0xAA, rec));
}

// Next's work (libsail's, the call's record at +0xD0, 0x40 bytes cleared): an open stream moves on at once; a started or
// ending one is stopped first (or ended, with the start flag 1), completing 0xA9.
void next_call(PPUContext& ctx, u32 p, u32 d)
{
    const u32 rec = p + kCallRecord;
    std::memset(gb(ctx) + rec, 0, 0x40);
    w32(ctx, rec, r32(ctx, rec) | 0x20);
    const u8 ph = phase(ctx, p);
    if (ph == 2) {
        next_prepare(ctx, p, p + 0xE0, step(0xA8, rec));
    } else if (static_cast<u8>(ph - 4) <= 1) {
        if (r64(ctx, p + kStartFlags) & 1) stream_end(ctx, p, d, p + 0x110, step(0xA9, rec), 1);
        else stream_stop(ctx, p, d, p + 0xE0, 0, step(0xA9, rec));
    }
}

// A deferred call run once the step running is done (libsail's, by the record's flags): start, next, stop, the input's
// end.
void run_deferred(PPUContext& ctx, u32 p, u32 d, u32 rec)
{
    switch (r32(ctx, rec) & 0x1E) {
    case 2: start_call(ctx, p, d); return;
    case 4: next_call(ctx, p, d); return;
    case 8: stop_call(ctx, p, d); return;
    case 0x10: input_end(ctx, p, d); return;
    default: return;
    }
}

// The answer to the game's Start, Stop or Next (libsail's): stored, the game's thread signalled and the lock released,
// neither's error looked at.
void answer_command(PPUContext& ctx, u32 p, u32 record, s32 result)
{
    w32(ctx, r32(ctx, record), static_cast<u32>(result));
    sc(ctx, 110, {r32(ctx, p + pl::kLock + 4), r64(ctx, record + 8)});
    player_unlock(ctx, p);
}

// ------------------------------------------------------------------------------------------------- the messages

// An elementary stream's start or stop counted on its slot (+0 flags, +4 steps, +8 handle); `started` 4 or open 2.
void es_step(PPUContext& ctx, u32 p, u32 kind, s32 result, u32 slot, bool start)
{
    const u32 state_at = kind == 0 ? as::kState : kind == 1 ? vs::kState : us::kState;
    const u32 index = r8(ctx, slot + (kind == 0 ? as::kIndex : kind == 1 ? vs::kIndex : us::kIndex));
    if (result < 0) {
        mark_failed(ctx, slot);
        notify_error(ctx, p, result);
    }
    if (count_down(ctx, slot)) return;
    const u32 d = current(ctx, p);
    if (start) {
        w8(ctx, slot + state_at, 4);
        if (!(r32(ctx, slot) & 1)) {
            post_completion(ctx, p, 0, r64(ctx, slot + 8));
            return;
        }
        // A failed start stopped again, the step it belongs to never completing (libsail's: the handle given is 0).
        const u32 keep = r32(ctx, slot + 0xC);
        if (kind == 0) es_stop_audio(ctx, p, d, slot, index, keep, 0, true);
        else if (kind == 1) es_stop_video(ctx, p, d, slot, index, keep, 0, true);
        else es_stop_user(ctx, p, d, slot, index, keep, 0, true);
        return;
    }
    w8(ctx, slot + state_at, 2);
    if (phase(ctx, p) != 6) {
        const u32 es = slot + (kind == 0 ? as::kDmuxEs : kind == 1 ? vs::kDmuxEs : us::kDmuxEs);
        if (dmux_es_remove(ctx, dmux(p), es) < 0) mark_failed(ctx, slot);
    }
    if (!(r32(ctx, slot) & 1)) {
        post_completion(ctx, p, 0, r64(ctx, slot + 8));
        return;
    }
    const u64 handle = r64(ctx, slot + 8);
    if (kind == 0) es_close_audio(ctx, p, slot, handle, true);
    else if (kind == 1) es_close_video(ctx, p, slot, handle, true);
    else es_close_user(ctx, p, slot, handle, true);
}

// An elementary stream's end counted (0xC3-0xC7): ended, open again (2), and its part of the end completes.
void es_end_step(PPUContext& ctx, u32 p, u32 slot, u32 state_at)
{
    if (phase(ctx, p) != 5 || count_down(ctx, slot)) return;
    w8(ctx, slot + state_at, 2);
    post_completion(ctx, p, 0, r64(ctx, slot + 8));
}

// An elementary stream drained (message 0x1A): its part of the end (0xC0-0xC2) on the end's record; its decoder's
// sequence ended (0xC3, 0xC5) or its AU receiver's (0xC7).
void es_drained(PPUContext& ctx, u32 p, u32 es)
{
    const u32 rec = p + 0x110, kind = r32(ctx, es + des::kKind), slot = r32(ctx, es + des::kSlot);
    const u32 state_at = kind == 0 ? as::kState : kind == 1 ? vs::kState : us::kState;
    if (kind > 2) return;
    w8(ctx, slot + state_at, 5);
    inc32(ctx, rec + 4);
    std::memset(gb(ctx) + slot, 0, 0x10);
    w32(ctx, rec, r32(ctx, rec) | 0x20);
    w64(ctx, slot + 8, step(0xC0 + kind, rec));
    w32(ctx, slot + 4, 1);
    if (kind == 0) adec_end(ctx, slot + as::kPipeline, step(0xC3, slot));
    else if (kind == 1) vdec_end(ctx, slot + vs::kPipeline, step(0xC5, slot));
    else receiver_end(ctx, r32(ctx, slot + us::kReceiver), step(0xC7, slot));
}

} // namespace

bool play_message(PPUContext& ctx, u32 p, u32 code, s32 result, u32 b, u32 c)
{
    const u32 dm = dmux(p);
    switch (code) {
    // ---- the source's items to the dmux pipeline
    case 0x01: { // StreamOut {source}
        source_item_ready(ctx, b);
        if (playing(ctx, p) && source_has_items(ctx, b) && dmux_wants(ctx, dm)) feed_item(ctx, b, dm);
        return true;
    }
    case 0x02: { // the pipeline wants items {pipeline, count}
        const u32 s = r32(ctx, b + dmx::kSource);
        if (!playing(ctx, p) || c == 0) return true;
        for (u32 i = 0;;) {
            if (!source_has_items(ctx, s) || !dmux_wants(ctx, b)) break;
            i++;
            feed_item(ctx, s, b);
            if (c <= i) break;
        }
        return true;
    }
    case 0x03: { // an item demultiplexed {pipeline, item}
        const u32 s = r32(ctx, b + dmx::kSource);
        dmux_item_done(ctx, b);
        source_checkin(ctx, s, c);
        if (playing(ctx, p) && source_has_items(ctx, s) && dmux_wants(ctx, b)) feed_item(ctx, s, b);
        return true;
    }
    case 0x2E: { // an item demultiplexed, not fed again {pipeline, item}
        const u32 s = r32(ctx, b + dmx::kSource);
        dmux_item_done(ctx, b);
        source_checkin(ctx, s, c);
        return true;
    }
    // ---- AUs to the decoders and receivers
    case 0x04: { // an AU found {pipeline, stream}
        dmux_es_au_found(ctx, c);
        if (!playing(ctx, p)) return true;
        const u32 kind = r32(ctx, c + des::kKind), slot = r32(ctx, c + des::kSlot);
        if (kind == 0) {
            if (r32(ctx, p + kStartType) == 16) {
                StackVar au(ctx, gb(ctx), 0x30);
                dmux_es_get_au_audio(ctx, c, au.addr());
                dmux_es_release_au(ctx, c);
                return true;
            }
            if (slot_playing(ctx, slot, as::kState) && dmux_es_has_au(ctx, c) && adec_takes(ctx, slot + as::kPipeline))
                feed_audio(ctx, c, slot + as::kPipeline);
        } else if (kind == 1) {
            if (slot_playing(ctx, slot, vs::kState) && dmux_es_has_au(ctx, c) && vdec_takes(ctx, slot + vs::kPipeline))
                feed_video(ctx, c, slot + vs::kPipeline);
        } else if (kind == 2) {
            const u32 t = r32(ctx, slot + us::kReceiver);
            if (slot_playing(ctx, slot, us::kState) && dmux_es_has_au(ctx, c)) receiver_au_found(ctx, t, c);
        }
        return true;
    }
    case 0x05: { // the audio decoder takes AUs {pipeline, count}
        const u32 es = r32(ctx, b + 0xA8);
        if (c == 0) return true;
        for (u32 i = 0;;) {
            if (!dmux_es_has_au(ctx, es) || !adec_takes(ctx, b)) break;
            i++;
            feed_audio(ctx, es, b);
            if (c <= i) break;
        }
        return true;
    }
    case 0x06: { // an audio AU decoded {pipeline}
        const u32 es = r32(ctx, b + 0xA8);
        adec_au_done(ctx, b);
        dmux_es_release_au(ctx, es);
        return true;
    }
    case 0x07: adec_pcm_out(ctx, b); return true;
    case 0x08: { // PCM taken by the copy thread {pipeline}
        const u32 slot = r32(ctx, b + 0xB0);
        adec_pcm_taken(ctx, b);
        if (!playing(ctx, p) || !slot_playing(ctx, slot, as::kState)) return true;
        const u32 es = r32(ctx, b + 0xA8);
        if (dmux_es_has_au(ctx, es) && adec_takes(ctx, b)) feed_audio(ctx, es, b);
        return true;
    }
    case 0x09: { // {audio pipeline}
        const u32 r = r32(ctx, b + 0xAC);
        if (adec_wants_frame(ctx, b) && renderer_has_free(ctx, r)) give_audio_frame(ctx, b);
        return true;
    }
    case 0x0A: { // an audio frame filled {pipeline, frame}
        const u32 r = r32(ctx, b + 0xAC), slot = r32(ctx, b + 0xB0);
        adec_frame_back(ctx, b);
        renderer_checkin(ctx, r, c);
        if (playing(ctx, p) && slot_playing(ctx, slot, as::kState) && adec_wants_frame(ctx, b) &&
            renderer_has_free(ctx, r))
            give_audio_frame(ctx, b);
        return true;
    }
    case 0x0B: { // the video decoder takes AUs {pipeline, count}
        const u32 es = r32(ctx, b + 0x74);
        if (c == 0) return true;
        for (u32 i = 0;;) {
            if (!dmux_es_has_au(ctx, es) || !vdec_takes(ctx, b)) break;
            i++;
            feed_video(ctx, es, b);
            if (c <= i) break;
        }
        return true;
    }
    case 0x0C: { // a video AU decoded {pipeline, its answer}
        const u32 es = r32(ctx, b + 0x74);
        vdec_au_done(ctx, b, static_cast<s32>(c));
        dmux_es_release_au(ctx, es);
        const u32 slot = r32(ctx, b + 0x7C);
        if (playing(ctx, p) && slot_playing(ctx, slot, vs::kState) && dmux_es_has_au(ctx, es) && vdec_takes(ctx, b))
            feed_video(ctx, es, b);
        return true;
    }
    case 0x0D: { // a picture out {pipeline}
        const u32 slot = r32(ctx, b + 0x7C), r = r32(ctx, slot + vs::kRenderer);
        vdec_picture_out(ctx, b);
        if (playing(ctx, p) && slot_playing(ctx, slot, vs::kState) && vdec_has_picture(ctx, b) &&
            renderer_has_free(ctx, r))
            give_video_frame(ctx, b);
        return true;
    }
    case 0x0E: return true;
    case 0x0F: { // {video pipeline}
        const u32 r = r32(ctx, b + 0x78);
        if (vdec_has_picture(ctx, b) && renderer_has_free(ctx, r)) give_video_frame(ctx, b);
        return true;
    }
    case 0x10: { // a video frame filled {pipeline, keep pictures}
        const u32 slot = r32(ctx, b + 0x7C);
        StackVar out(ctx, gb(ctx), 16);
        vdec_frame_back(ctx, b, c, out.addr());
        if (renderer_checkin(ctx, r32(ctx, b + 0x78), r32(ctx, out.addr())) == 1) inc32(ctx, b + 0x14);
        if (!playing(ctx, p) || !slot_playing(ctx, slot, vs::kState)) return true;
        const u32 r = r32(ctx, b + 0x78);
        if (vdec_has_picture(ctx, b) && renderer_has_free(ctx, r)) give_video_frame(ctx, b);
        return true;
    }
    case 0x11: { // an audio renderer's frames free {renderer, count}
        const u32 slot = r32(ctx, b + 0x18), a = slot + as::kPipeline;
        if (!playing(ctx, p) || !slot_playing(ctx, slot, as::kState) || !renderer_has_free(ctx, b) || c == 0)
            return true;
        for (u32 i = 0;;) {
            if (!adec_wants_frame(ctx, a) || !renderer_has_free(ctx, b)) break;
            i++;
            give_audio_frame(ctx, a);
            if (c <= i) break;
        }
        return true;
    }
    case 0x12: { // an audio frame played {renderer}
        const u32 slot = r32(ctx, b + 0x18);
        renderer_frame_done(ctx, b);
        if (!playing(ctx, p) || !slot_playing(ctx, slot, as::kState)) return true;
        if (adec_wants_frame(ctx, slot + as::kPipeline)) give_audio_frame(ctx, slot + as::kPipeline);
        return true;
    }
    case 0x13: { // a video renderer's frames free {renderer, count}
        const u32 slot = r32(ctx, b + 0x18), v = slot + vs::kPipeline;
        if (!playing(ctx, p) || !slot_playing(ctx, slot, vs::kState) || !renderer_has_free(ctx, b) || c == 0)
            return true;
        for (u32 i = 0;;) {
            if (!vdec_has_picture(ctx, v) || !renderer_has_free(ctx, b)) break;
            i++;
            give_video_frame(ctx, v);
            if (c <= i) break;
        }
        return true;
    }
    case 0x14: { // a video frame played {renderer}
        const u32 slot = r32(ctx, b + 0x18);
        renderer_frame_done(ctx, b);
        if (!playing(ctx, p) || !slot_playing(ctx, slot, vs::kState)) return true;
        if (vdec_has_picture(ctx, slot + vs::kPipeline)) give_video_frame(ctx, slot + vs::kPipeline);
        return true;
    }
    case 0x15: return true;
    case 0x16: { // an AU taken from an AU receiver {receiver's object}
        const u32 es = r32(ctx, b + 0x34) + us::kDmuxEs;
        receiver_au_taken(ctx, b, es);
        dmux_es_release_au(ctx, es);
        return true;
    }
    case 0x2F: dmux_es_flushed(ctx, b); return true;
    case 0x30: adec_seq_done(ctx, b); return true;
    case 0x31: dmux_es_release_au(ctx, r32(ctx, b + 0x74)); return true;
    case 0x32: vdec_seq_done(ctx, b); return true;
    // ---- the stream's end
    case 0x17: { // the source's end of input {source}
        if (r32(ctx, p + kEpIndex) == 0xFFFFFFFF) notify_event(ctx, p, 8, 0, 0);
        if (!playing(ctx, p)) return true;
        const u32 rec = r32(ctx, p + kDeferred);
        if (rec == 0) input_end(ctx, p, current(ctx, p));
        else if ((r32(ctx, rec) & 0x1E) == 0) defer(ctx, rec, 0x10);
        return true;
    }
    case 0x18: { // the source drained {source}
        if (phase(ctx, p) != 5) return true;
        const u32 rec = p + 0x110;
        inc32(ctx, rec + 4);
        dmux_stop_by_word(ctx, dm, step(0xBF, rec));
        return true;
    }
    case 0x19: return true;
    case 0x1A: // an elementary stream drained {pipeline, stream}
        if (phase(ctx, p) == 5) es_drained(ctx, p, c);
        return true;
    case 0x1B: { // an audio decoder's sequence ended {pipeline, renderer}
        if (phase(ctx, p) != 5) return true;
        const u32 slot = r32(ctx, b + 0xB0);
        inc32(ctx, slot + 4);
        audio_renderer_end(ctx, r32(ctx, b + 0xAC), step(0xC4, slot));
        return true;
    }
    case 0x1C: { // a video decoder's sequence ended {pipeline, renderer}
        if (phase(ctx, p) != 5) return true;
        const u32 slot = r32(ctx, b + 0x7C);
        inc32(ctx, slot + 4);
        video_renderer_end(ctx, r32(ctx, b + 0x78), 0, step(0xC6, slot));
        return true;
    }
    case 0x1D: case 0x1E: case 0x1F: return true; // the renderers' and receivers' output ends: nothing
    case 0xBE: case 0xBF: case 0xC0: case 0xC1: case 0xC2: { // a step of the end done
        if (phase(ctx, p) != 5 || count_down(ctx, c)) return true;
        w8(ctx, p + kRepeatSet, 0);
        w8(ctx, p + kPhase, 2);
        const u64 handle = r64(ctx, c + 8);
        if (handle != 0) {
            post_completion(ctx, p, 0, handle);
            return true;
        }
        w32(ctx, p + kRunType, 0);
        set_paused(ctx, p, 0);
        w8(ctx, p + pl::kState, 4);
        notify_event(ctx, p, 3, 4, 0);
        return true;
    }
    case 0xC3: case 0xC4: es_end_step(ctx, p, c, as::kState); return true;
    case 0xC5: case 0xC6: es_end_step(ctx, p, c, vs::kState); return true;
    case 0xC7: es_end_step(ctx, p, c, us::kState); return true;
    case 0xC8: // an EP skip's next start
        w32(ctx, p + kDeferredKind, 0);
        w32(ctx, p + kDeferred, 0);
        if (r32(ctx, c) & 0x1E) run_deferred(ctx, p, current(ctx, p), c);
        return true;
    case 0xC9:
        w32(ctx, p + kDeferredKind, 0);
        w32(ctx, p + kDeferred, 0);
        if (r32(ctx, c) & 0x1E) run_deferred(ctx, p, current(ctx, p), c);
        else streams_start(ctx, p, current(ctx, p), c + 0x10, 0);
        return true;
    case 0xCA: // a repeat's start
        w32(ctx, p + kDeferred, 0);
        w32(ctx, p + kDeferredKind, 0);
        notify_event(ctx, p, 6, r32(ctx, p + kSession) & 0x3FF, 0);
        if (r32(ctx, c) & 0x1E) run_deferred(ctx, p, current(ctx, p), c);
        return true;
    // ---- errors and media state
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
        notify_error(ctx, p, static_cast<s32>(c | 0x80000000));
        if (static_cast<s32>(c) < 0) return true;
        const u8 ph = phase(ctx, p);
        if (static_cast<u8>(ph - 4) <= 1 && r32(ctx, p + kCallRunning) == 0) {
            w32(ctx, p + kCallRunning, 11);
            stream_stop(ctx, p, current(ctx, p), p + 0x110, 0, step(0x28, 0));
            return true;
        }
        if (ph == 3 && r32(ctx, p + kCallRunning) != 0)
            post_message(ctx, p, code, static_cast<u32>(result), b, c); // later, once the start is done
        return true;
    }
    case 0x28: // stopped after an error
        w32(ctx, p + kCallRunning, 0);
        w8(ctx, p + pl::kState, 4);
        notify_event(ctx, p, 3, 4, 0);
        return true;
    case 0x2D: { // the source's media state {source, state}
        const u8 ph = phase(ctx, p);
        if (ph != 7 && ph != 0) set_media_state(ctx, p, c, current(ctx, p));
        return true;
    }
    case 0x3A: { // a video stream paused at its first frame
        const u8 n = static_cast<u8>(r8(ctx, p + kFirstFrames) + 1);
        w8(ctx, p + kFirstFrames, n);
        if (r8(ctx, p + kVideoOpen) != n) return true;
        u32 run = 0;
        if (c == 0) {
            const u32 type = r32(ctx, p + kStartType);
            if (type == 1) run = 0;
            else if (type != 64) run = 1;
            else run = r32(ctx, p + kRunType) != 1 ? 1 : 0;
        }
        clock_run(ctx, p + pl::kSync, run & 1);
        for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
            const u32 slot = audio_slot(ctx, p, i);
            const u8 st = r8(ctx, slot + as::kState);
            if (st != 2 && st != 4 && st != 5) continue;
            const u32 r = r32(ctx, slot + as::kRenderer);
            const u32 v = (r32(ctx, r + 0x34) & ~8u) | 8;
            w32(ctx, r + 0x34, v);
            w32(ctx, r + 0x34, (v & ~0x20u) | ((run & 0xFF) ? 0x20u : 0u));
        }
        if (r8(ctx, p + kPaused) != 0) return true;
        w8(ctx, p + kPaused, 1);
        notify_event(ctx, p, 7, 1, 0);
        return true;
    }
    // ---- Start
    case 0x6C: { // Start {the command, 0, the game's u64}
        player_lock(ctx, p);
        const u8 st = r8(ctx, p + pl::kState);
        const u32 d = current(ctx, p);
        s32 r = error(CELL_SAIL_ERROR_INVALID_STATE);
        if ((st == 4 || st == 6) && r32(ctx, p + kCallRunning) == 0) {
            const u32 cmd = r32(ctx, b);
            bool go = true;
            if (cmd) {
                if (!playing(ctx, p) && r32(ctx, cmd + 4) == 64) {
                    go = false;
                } else {
                    r = static_cast<s32>(module_call(ctx, "cellSail", "cellSailDescriptorInquireCapability", {d, cmd}));
                    if (r < 0) go = false;
                    else if ((r32(ctx, cmd + 0xC) & 0x10) && !r8(ctx, p + kRepeatSet)) {
                        w8(ctx, p + kRepeatSet, 1);
                        std::memmove(gb(ctx) + p + kRepeatInfo, gb(ctx) + cmd, 0x28);
                    }
                }
            }
            if (go) {
                if (r32(ctx, p + kDeferred) != 0 && r32(ctx, p + kDeferredKind) != 3) {
                    r = error(CELL_SAIL_ERROR_INVALID_STATE);
                } else {
                    w32(ctx, p + kCallRunning, 10);
                    w64(ctx, p + pl::kBootArg, r64(ctx, b + 8));
                    start_info(ctx, d, cmd, p + kStartInfo, playing(ctx, p));
                    r = 0;
                    w8(ctx, p + pl::kState, 5);
                    notify_event(ctx, p, 3, 5, 0);
                }
            }
        }
        answer_command(ctx, p, c, r);
        if (r < 0) return true;
        if (const u32 rec = r32(ctx, p + kDeferred)) defer(ctx, rec, 2);
        else start_call(ctx, p, d);
        return true;
    }
    case 0x6D: { // Start done
        if (result < 0) {
            w32(ctx, p + kRunType, 0);
            w8(ctx, p + pl::kState, 4);
            notify_event(ctx, p, 3, 4, 0);
        } else {
            const u32 type = r32(ctx, p + kStartType);
            if (type == 0) w32(ctx, p + kRunType, 1);
            else if (type != 64) w32(ctx, p + kRunType, type);
            w8(ctx, p + pl::kState, 6);
            notify_event(ctx, p, 3, 6, 0);
        }
        call_completed(ctx, p, 10, result);
        return true;
    }
    case 0x6E: // stopped to start again
        if (result < 0) post_completion(ctx, p, result, r64(ctx, c + 8));
        else streams_start(ctx, p, current(ctx, p), c + 0x10, step(0x6D, c));
        return true;
    case 0x6F: { // the source started
        const u32 d = current(ctx, p);
        w32(ctx, p + kDeferred, 0);
        w8(ctx, p + kPhase, 4);
        w32(ctx, p + kDeferredKind, 0);
        if (result < 0) {
            notify_error(ctx, p, result);
            stream_stop(ctx, p, d, c + 0x10, 0, step(0x7C, c));
            return true;
        }
        inc8(ctx, p + kStarts);
        notify_event(ctx, p, 6, r32(ctx, p + kSession) & 0x3FF, 0);
        const u32 flags = r32(ctx, c);
        if ((flags & 0x1E) == 0) {
            const u64 handle = r64(ctx, c + 8);
            if (r32(ctx, d + ds::kType) == 0 && playing(ctx, p)) maybe_feed_item(ctx, p);
            post_completion(ctx, p, 0, handle);
            return true;
        }
        if (flags & 0x10) post_completion(ctx, p, 0, r64(ctx, c + 8));
        run_deferred(ctx, p, d, c);
        return true;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: { // a step of the streams' start done
        if (result < 0) {
            mark_failed(ctx, c);
            notify_error(ctx, p, result);
        }
        inc8(ctx, p + kStarts);
        if (count_down(ctx, c)) return true;
        const u32 d = current(ctx, p);
        if (r32(ctx, c) & 1) {
            w32(ctx, p + kDeferred, 0);
            w8(ctx, p + kPhase, 2);
            w32(ctx, p + kDeferredKind, 0);
            stream_stop(ctx, p, d, c + 0x10, 0, step(0x7C, c));
            return true;
        }
        const u32 call = r32(ctx, p + kCallRunning);
        if (call == 10 || call == 12) {
            source_session(ctx, p, d, false, false, step(0x6F, c));
            return true;
        }
        source_session(ctx, p, d, true, false, step(0x6F, c));
        if (r8(ctx, p + kRepeatSet) && !(r32(ctx, p + kRepeatInfo + 0xC) & 0x10)) w8(ctx, p + kRepeatSet, 0);
        return true;
    }
    case 0x74: case 0x75: case 0x76: es_step(ctx, p, 0, result, c, true); return true;
    case 0x77: case 0x78: case 0x79: es_step(ctx, p, 1, result, c, true); return true;
    case 0x7A: case 0x7B: es_step(ctx, p, 2, result, c, true); return true;
    case 0x7C: post_completion(ctx, p, error(CELL_SAIL_ERROR_CANCEL), r64(ctx, c + 8)); return true;
    // ---- Stop
    case 0x7D: { // Stop {the game's u64}
        player_lock(ctx, p);
        s32 r = error(CELL_SAIL_ERROR_INVALID_STATE);
        if (r8(ctx, p + pl::kState) == 6 && r32(ctx, p + kCallRunning) == 0) {
            r = 0;
            w32(ctx, p + kCallRunning, 11);
            w64(ctx, p + pl::kBootArg, r64(ctx, b));
            w8(ctx, p + pl::kState, 7);
            notify_event(ctx, p, 3, 7, 0);
        }
        answer_command(ctx, p, c, r);
        if (r < 0) return true;
        if (const u32 rec = r32(ctx, p + kDeferred)) defer(ctx, rec, 8);
        else stop_call(ctx, p, current(ctx, p));
        return true;
    }
    case 0x7E: // Stop done
        w32(ctx, p + kRunType, 0);
        w8(ctx, p + pl::kState, 4);
        notify_event(ctx, p, 3, 4, 0);
        call_completed(ctx, p, 11, result);
        return true;
    case 0x7F: case 0x80: case 0x81: case 0x82: case 0x83: { // a step of the stream's stop done
        if (result < 0) {
            mark_failed(ctx, c);
            notify_error(ctx, p, result);
        }
        inc8(ctx, p + kStarts, -1);
        if (count_down(ctx, c)) return true;
        w8(ctx, p + kPhase, 2);
        w8(ctx, p + kRepeatSet, 0);
        w8(ctx, p + kStarts, 0);
        set_paused(ctx, p, 0);
        post_completion(ctx, p, 0, r64(ctx, c + 8));
        return true;
    }
    case 0x84: case 0x85: case 0x86: es_step(ctx, p, 0, result, c, false); return true;
    case 0x87: case 0x88: case 0x89: es_step(ctx, p, 1, result, c, false); return true;
    case 0x8A: case 0x8B: es_step(ctx, p, 2, result, c, false); return true;
    // ---- Cancel
    // ---- Next
    case 0xA7: { // Next {the command, 0, the game's u64}
        player_lock(ctx, p);
        const u8 st = r8(ctx, p + pl::kState);
        s32 r = error(CELL_SAIL_ERROR_INVALID_STATE);
        if ((st == 4 || st == 6) && r32(ctx, p + kCallRunning) == 0 && has_next(ctx, p)) {
            const u32 cmd = r32(ctx, b);
            s32 q = 0;
            if (cmd) q = static_cast<s32>(module_call(ctx, "cellSail", "cellSailDescriptorInquireCapability",
                         {r32(ctx, p + kNextDescriptor), cmd}));
            if (q < 0) {
                r = q;
            } else if (r32(ctx, p + kDeferred) == 0 || r32(ctx, p + kDeferredKind) == 3) {
                w64(ctx, p + pl::kBootArg, r64(ctx, b + 8));
                w32(ctx, p + kCallRunning, 12);
                if (cmd) std::memmove(gb(ctx) + p + kRepeatInfo, gb(ctx) + cmd, 0x28);
                w8(ctx, p + kRepeatSet, cmd ? 1 : 0);
                w8(ctx, p + pl::kState, 5);
                notify_event(ctx, p, 3, 5, 0);
                r = q;
            }
        }
        answer_command(ctx, p, c, r);
        if (r < 0) return true;
        set_paused(ctx, p, 0);
        if (const u32 rec = r32(ctx, p + kDeferred)) defer(ctx, rec, 4);
        else next_call(ctx, p, current(ctx, p));
        return true;
    }
    case 0xA8: { // Next done
        if (r8(ctx, p + kRepeatSet) && !(r32(ctx, p + kRepeatInfo + 0xC) & 0x10)) w8(ctx, p + kRepeatSet, 0);
        if (result < 0) {
            w8(ctx, p + pl::kState, 4);
            notify_event(ctx, p, 3, 4, 0);
        } else {
            const u32 type = r32(ctx, p + kStartType);
            if (type == 0) w32(ctx, p + kRunType, 1);
            else if (type != 64) w32(ctx, p + kRunType, type);
            w8(ctx, p + pl::kState, 6);
            notify_event(ctx, p, 3, 6, 0);
        }
        call_completed(ctx, p, 12, result);
        return true;
    }
    case 0xA9: // the stream stopped (or ended) for Next
        w32(ctx, p + kRunType, 0);
        if (result < 0) post_completion(ctx, p, result, r64(ctx, c + 8));
        else next_prepare(ctx, p, c + 0x10, step(0xA8, c));
        return true;
    case 0xAA: { // the next descriptor made ready
        if (result < 0) {
            post_completion(ctx, p, result, r64(ctx, c + 8));
            return true;
        }
        u32 cmd = 0;
        if (r8(ctx, p + kRepeatSet)) {
            cmd = p + kRepeatInfo;
            if (!(r32(ctx, cmd + 0xC) & 0x10)) w8(ctx, p + kRepeatSet, 0);
        }
        start_info(ctx, r32(ctx, p + kNextDescriptor), cmd, p + kStartInfo, false);
        descriptor_switch(ctx, p, c);
        return true;
    }
    case 0xAB: { // the current source closed: the next one opened
        if (result < 0) {
            post_completion(ctx, p, result, r64(ctx, c + 8));
            return true;
        }
        const u32 nd = r32(ctx, p + kNextDescriptor);
        if (const u32 t = r32(ctx, nd + ds::kType); t == 1 || t == 2) container(ctx, nd, "Next's source open");
        source_open(ctx, r32(ctx, nd + ds::kSource), nd, step(0xB2, c));
        return true;
    }
    case 0xAC: { // the dmux pipeline closed: opened for the next descriptor
        if (result < 0) {
            post_completion(ctx, p, result, r64(ctx, c + 8));
            return true;
        }
        const u32 nd = r32(ctx, p + kNextDescriptor);
        dmux_open(ctx, dm, nd + ds::kHeader, r32(ctx, nd + ds::kSource), step(0xB3, c));
        return true;
    }
    case 0xAD: case 0xAE: case 0xAF: case 0xB0: case 0xB1: { // a slot's decoder, renderer or receiver closed
        const u32 nd = r32(ctx, p + kNextDescriptor);
        u32 e;
        if (code <= 0xAE) e = entry(ctx, nd, ds::kAudio, ds::kAudioStride, r8(ctx, c + as::kIndex));
        else if (code <= 0xB0) e = entry(ctx, nd, ds::kVideo, ds::kVideoStride, r8(ctx, c + vs::kIndex));
        else e = entry(ctx, nd, ds::kUser, ds::kUserStride, r8(ctx, c + us::kIndex));
        if (result >= 0 && r8(ctx, e) == 1) {
            switch (code) {
            case 0xAD:
                audio_pipeline_open(ctx, c + as::kPipeline, e + 0x28, e + 0x2C, e + 0x60, c + as::kDmuxEs,
                    r32(ctx, c + as::kRenderer), c, step(0xB7, c));
                break;
            case 0xAE: audio_renderer_open(ctx, r32(ctx, c + as::kRenderer), e + 0x78, c, step(0xB8, c)); break;
            case 0xAF:
                video_pipeline_open(ctx, c + vs::kPipeline, e + 0x20, e + 0x38, e + 0x58, c + vs::kDmuxEs,
                    r32(ctx, c + vs::kRenderer), c, step(0xB9, c));
                break;
            case 0xB0: video_renderer_open(ctx, r32(ctx, c + vs::kRenderer), e + 0x58, c, step(0xBA, c)); break;
            default:
                receiver_open(ctx, r32(ctx, c + us::kReceiver), r32(ctx, nd + ds::kType), e + 0x3C, c, step(0xBB, c));
                break;
            }
            return true;
        }
        if (count_down(ctx, c)) return true;
        post_completion(ctx, p, result, r64(ctx, c + 8));
        return true;
    }
    case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: { // a step of the switch done
        if (result < 0) {
            mark_failed(ctx, c);
            notify_error(ctx, p, result);
        }
        if (count_down(ctx, c)) return true;
        const u32 od = current(ctx, p);
        for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
            const u32 e = entry(ctx, od, ds::kAudio, ds::kAudioStride, i);
            if (r8(ctx, e) != 2) continue;
            w8(ctx, e, 1);
            inc8(ctx, p + kAudioOpen, -1);
            if (ps_read16(gb(ctx), audio_slot(ctx, p, i) + as::kMode)) inc8(ctx, p + kAudioModeOpen, -1);
            notify_event(ctx, p, 10, 0, i);
        }
        for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
            const u32 e = entry(ctx, od, ds::kVideo, ds::kVideoStride, i);
            if (r8(ctx, e) != 2) continue;
            w8(ctx, e, 1);
            inc8(ctx, p + kVideoOpen, -1);
            if (ps_read16(gb(ctx), video_slot(ctx, p, i) + vs::kMode)) inc8(ctx, p + kVideoModeOpen, -1);
            notify_event(ctx, p, 10, 1, i);
        }
        for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
            const u32 e = entry(ctx, od, ds::kUser, ds::kUserStride, i);
            if (r8(ctx, e) != 2) continue;
            w8(ctx, e, 1);
            notify_event(ctx, p, 10, 2, i);
        }
        w8(ctx, od + ds::kState, 1);
        notify_event(ctx, p, 5, od, 0);
        const u32 nd = r32(ctx, p + kNextDescriptor);
        const bool failed = (r32(ctx, c) & 1) != 0;
        w32(ctx, p + pl::kCurrentDescriptor, nd);
        w32(ctx, p + kNextDescriptor, 0);
        if (failed) w8(ctx, nd + ds::kState, 3);
        w32(ctx, p + kDeferredKind, 0);
        w32(ctx, p + kDeferred, 0);
        if (failed) {
            if (r8(ctx, p + kStarts) == 0) close_stream(ctx, p, nd, c + 0x10, step(0xBC, c));
            else stream_stop(ctx, p, nd, c + 0x10, 0, step(0xBD, c));
            return true;
        }
        const bool later = sdk_after_22(ctx);
        for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++) {
            const u32 e = entry(ctx, nd, ds::kAudio, ds::kAudioStride, i), slot = audio_slot(ctx, p, i);
            if (r8(ctx, e) != 1 || r8(ctx, slot + as::kState) != 2) continue;
            if (later) dmux_es_add(ctx, dm, 0, e + 0xC, e + 0x1C, slot, slot + as::kDmuxEs, false);
            w8(ctx, e, 2);
            inc8(ctx, p + kAudioOpen);
            if (ps_read16(gb(ctx), slot + as::kMode)) inc8(ctx, p + kAudioModeOpen);
            notify_event(ctx, p, 9, 0, i);
        }
        for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++) {
            const u32 e = entry(ctx, nd, ds::kVideo, ds::kVideoStride, i), slot = video_slot(ctx, p, i);
            if (r8(ctx, e) != 1 || r8(ctx, slot + vs::kState) != 2) continue;
            if (later) dmux_es_add(ctx, dm, 1, e + 0xC, e + 0x1C, slot, slot + vs::kDmuxEs, false);
            w8(ctx, e, 2);
            inc8(ctx, p + kVideoOpen);
            if (ps_read16(gb(ctx), slot + vs::kMode)) inc8(ctx, p + kVideoModeOpen);
            notify_event(ctx, p, 9, 1, i);
        }
        for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++) {
            const u32 e = entry(ctx, nd, ds::kUser, ds::kUserStride, i), slot = user_slot(ctx, p, i);
            if (r8(ctx, e) != 1 || r8(ctx, slot + us::kState) != 2) continue;
            if (later) dmux_es_add(ctx, dm, 2, e + 0x3C, e + 0xC, slot, slot + us::kDmuxEs, false);
            w8(ctx, e, 2);
            notify_event(ctx, p, 9, 2, i);
        }
        notify_event(ctx, p, 4, nd, 0);
        if (r32(ctx, c) & 0x1E) run_deferred(ctx, p, nd, c);
        else streams_start(ctx, p, nd, c + 0x10, r64(ctx, c + 8));
        return true;
    }
    case 0xB7: case 0xB8: case 0xB9: case 0xBA: case 0xBB: { // a slot's decoder, renderer or receiver reopened
        const u32 kind = code <= 0xB8 ? 0 : code <= 0xBA ? 1 : 2;
        if (result < 0) {
            mark_failed(ctx, c);
            notify_error(ctx, p, result);
        }
        if (count_down(ctx, c)) return true;
        w8(ctx, c + (kind == 0 ? as::kState : kind == 1 ? vs::kState : us::kState), 2);
        if (!(r32(ctx, c) & 1)) {
            post_completion(ctx, p, 0, r64(ctx, c + 8));
            return true;
        }
        if (kind == 0) es_close_audio(ctx, p, c, r64(ctx, c + 8), true);
        else if (kind == 1) es_close_video(ctx, p, c, r64(ctx, c + 8), true);
        else es_close_user(ctx, p, c, r64(ctx, c + 8), true);
        return true;
    }
    case 0xBC: case 0xBD: // the next descriptor's stream taken down after a failed switch
        post_completion(ctx, p, error(CELL_SAIL_ERROR_CANCEL), r64(ctx, c + 8));
        return true;
    case 0xD0: {
        const u32 d = current(ctx, p);
        const u32 type = r32(ctx, d + ds::kType);
        if (type != 1 && type != 2) source_cancel(ctx, r32(ctx, d + ds::kSource));
        if (const u32 kind = r32(ctx, dm + dmx::kType); kind == 0x81 || kind == 0x82)
            later_part(ctx, "Cancel of an MP4 or AVI stream (the firmware library's table)");
        for (u32 i = 0; i < r32(ctx, p + pl::kAudioCount); i++)
            if (r8(ctx, entry(ctx, d, ds::kAudio, ds::kAudioStride, i)) == 2)
                renderer_cancel(ctx, r32(ctx, audio_slot(ctx, p, i) + as::kRenderer));
        for (u32 i = 0; i < r32(ctx, p + pl::kVideoCount); i++)
            if (r8(ctx, entry(ctx, d, ds::kVideo, ds::kVideoStride, i)) == 2)
                renderer_cancel(ctx, r32(ctx, video_slot(ctx, p, i) + vs::kRenderer));
        for (u32 i = 0; i < r32(ctx, p + pl::kUserCount); i++)
            if (r8(ctx, entry(ctx, d, ds::kUser, ds::kUserStride, i)) == 2)
                receiver_cancel(ctx, r32(ctx, user_slot(ctx, p, i) + us::kReceiver));
        return true;
    }
    default: return false;
    }
}

} // namespace pspocket::hle::sail

namespace pspocket::hle {
namespace {

using namespace sail;

// Start(player, the start command or NULL, the game's u64) and Next (the same): the command's answer; the call's
// completion event carries the u64. Stop(player, u64). Cancel(player): posted at the queue's head, 0.
s32 start_or_next(PPUContext& ctx, u32 p, u32 code, u32 cmd, u64 arg)
{
    StackVar args(ctx, gb(ctx), 0x10);
    w32(ctx, args.addr(), cmd);
    w64(ctx, args.addr() + 8, arg);
    return command_answer(ctx, p, code, args.addr());
}

s32 cellSailPlayerStart(PPUContext& ctx, u32 p, u32 cmd, u64 arg) { return start_or_next(ctx, p, 0x6C, cmd, arg); }
s32 cellSailPlayerNext(PPUContext& ctx, u32 p, u32 cmd, u64 arg) { return start_or_next(ctx, p, 0xA7, cmd, arg); }

s32 cellSailPlayerStop(PPUContext& ctx, u32 p, u64 arg)
{
    StackVar args(ctx, gb(ctx), 8);
    w64(ctx, args.addr(), arg);
    return command_answer(ctx, p, 0x7D, args.addr());
}

s32 cellSailPlayerCancel(PPUContext& ctx, u32 p)
{
    post_urgent(ctx, p, 0xD0, 0, 0, 0);
    return lv2::CELL_OK;
}

} // namespace

void register_cellSailPlay(Registry& r)
{
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}
    r.add("cellSail", {
        SAIL(cellSailPlayerStart),
        SAIL(cellSailPlayerNext),
        SAIL(cellSailPlayerStop),
        SAIL(cellSailPlayerCancel),
    });
#undef SAIL
}

} // namespace pspocket::hle
