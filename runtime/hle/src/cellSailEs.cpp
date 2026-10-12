// cellSail's elementary streams opened and closed (D-919), as libsail (fw 4.92) has it. An open stream of the current
// descriptor has its part in the dmux pipeline (the slot's +0x10), its decoders' pipeline and renderer (audio, video)
// or its AU receiver (user). Opening one (state 1, its descriptor entry 2) adds its part to the dmux pipeline (SDKs past
// 0x21FFFF; enabled when the stream starts) and opens the renderer and the pipeline (or the receiver) at once, the
// slot's record counting them (messages 0x54-0x56 audio, 0x57-0x59 video, 0x5A-0x5B user); when all are in the stream
// is open (2, event 9), or, failed, closed again. Closing one (7) closes the pipeline and the renderer (or the
// receiver) and removes its dmux part (0x64-0x66, 0x67-0x69, 0x6A-0x6B); when all are in it is closed (0), its entry
// chosen (1) or, after a failed open, cleared (0), event 10.
//
// The game's OpenEsAudio/Video/User (messages 0x8C, 0x92, 0x98; ReopenEs the same with its flag set) and
// CloseEsAudio/Video/User (0x9E, 0xA1, 0xA4) need an open stream (player state 4 or 6) with no call running; their
// completion events are calls 4-6 (open), 13-15 (reopen) and 7-9 (close).

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sail {

namespace {

constexpr u32 kStreamPhase = 0x160;  // the stream: 0 none, 1 opening, 2 open, 3-5 starting and started, 7 closing
constexpr u32 kAudioOpen = 0x164, kVideoOpen = 0x165, kAudioModeOpen = 0x1CA, kVideoModeOpen = 0x1CB;
constexpr u32 kCallRunning = 0x250, kPending = 0x260, kCallRecord = 0xD0;

// The record's flags (+0) and steps (+4).
u32 flags(PPUContext& ctx, u32 rec) { return r32(ctx, rec); }
void mark_failed(PPUContext& ctx, u32 rec) { w32(ctx, rec, r32(ctx, rec) | 1); }
u32 count_down(PPUContext& ctx, u32 rec)
{
    const u32 left = r32(ctx, rec + 4) - 1;
    w32(ctx, rec + 4, left);
    return left;
}

void begin(PPUContext& ctx, u32 slot, u64 handle, bool failed)
{
    std::memset(gb(ctx) + slot, 0, 0x10);
    if (failed) mark_failed(ctx, slot);
    w64(ctx, slot + 8, handle);
}

u32 dmux(u32 p) { return p + pl::kDmux; }

// The pixel aspect ratios of the aspect codes (0 to 5: 4:3 square, 1:1, and BT.601's 16:15, 8:9, 64:45, 32:27), and
// the doubled frame rate of an interlaced one when the pictures are made progressive (frame rate codes 2-4: 25,
// 29.97 and 30 frames become 50, 59.94 and 60), libsail's two small tables.
constexpr u8 kAspect[6][2] = {{4, 3}, {1, 1}, {16, 15}, {8, 9}, {64, 45}, {32, 27}};
constexpr u32 kDoubled[9] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 5, 6, 7, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};

} // namespace

// The audio format the renderer is given (the entry's +0x78): the stream's, ATRAC3plus's channels folded to two for a
// renderer that wants two, 48 kHz with audio parameter 32 when the renderer wants it (its frame 640 samples for the
// 44.1 kHz family, else 480).
void audio_out_format(PPUContext& ctx, u32 e, u32 slot)
{
    const u32 r = r32(ctx, slot + as::kRenderer);
    if (r32(ctx, e + 0x28) == 0x10 && static_cast<s8>(r8(ctx, e + 0x61)) > 2 && r8(ctx, r + 0x39) == 2) {
        w8(ctx, e + 0x61, 2);
        w32(ctx, e + 0x38, 1);
    }
    std::memmove(gb(ctx) + e + 0x78, gb(ctx) + e + 0x60, 0x18);
    if (r8(ctx, slot + as::kSrc) && r32(ctx, r + 0x3C) == 48000) {
        w32(ctx, e + 0x7C, 48000);
        const u32 fs = r32(ctx, e + 0x64);
        w16(ctx, e + 0x7A, fs == 22050 || fs == 44100 || fs == 11025 ? 0x280 : 0x1E0);
    }
}

// The video format the renderer is given (the entry's +0x58): the picture's, with the renderer's pitch and alpha when
// it set them; without post-processing the renderer's coding (0-3); with it (video parameter 0) progressive pictures
// when the renderer wants them (an interlaced rate doubled), its aspect code, and the size scaled to it (the
// renderer's own size, else the picture's, kept on the side the aspect ratios allow and the other rounded up, both
// even), its coding 1 or 3. The pitch: the width in bytes, or the renderer's when larger (without post-processing only
// a multiple of 16).
void video_out_format(PPUContext& ctx, u32 e, u32 slot)
{
    u8* b = gb(ctx);
    const u32 f = e + 0x58, pic = e + 0x38;
    std::memmove(b + f, b + pic, 0x20);
    const u32 r = r32(ctx, slot + vs::kRenderer) + 0x40; // the format the renderer wants
    if (static_cast<s32>(r32(ctx, r + 12)) >= 0) w32(ctx, f + 12, r32(ctx, r + 12));
    if (r8(ctx, r + 16) <= 1) w8(ctx, f + 16, r8(ctx, r + 16));
    if (!r8(ctx, slot + vs::kVpost)) {
        const u8 coding = r8(ctx, r);
        if (static_cast<u32>(static_cast<s32>(static_cast<s8>(coding))) <= 3) w8(ctx, f, coding);
    } else {
        const u8 scan = r8(ctx, r + 1);
        if (scan <= 1 && r8(ctx, f + 1) == 1 && scan == 0) {
            w8(ctx, f + 1, 0);
            const u32 rate = static_cast<u32>(static_cast<s32>(static_cast<s8>(r8(ctx, f + 3))) + 1);
            w8(ctx, f + 3, static_cast<u8>(kDoubled[rate % 9]));
        }
        if (r8(ctx, r + 17) != 0xFF) w8(ctx, f + 17, r8(ctx, r + 17));
        s64 oc = static_cast<s8>(r8(ctx, f + 17));
        oc = oc < 0 ? 1 : oc > 5 ? 0 : oc;
        s64 ic = static_cast<s8>(r8(ctx, pic + 17));
        ic = ic < 0 ? 1 : ic > 5 ? 0 : ic;
        const u64 on = kAspect[oc][0], od = kAspect[oc][1], in = kAspect[ic][0], id = kAspect[ic][1];
        s64 w = static_cast<s16>(ps_read16(b, r + 4)), h = static_cast<s16>(ps_read16(b, r + 6));
        const s64 fw = static_cast<s16>(ps_read16(b, f + 4)), fh = static_cast<s16>(ps_read16(b, f + 6));
        if (w <= 0 && h <= 0) {
            w = fw;
            h = fh;
        }
        const u64 dh = od * static_cast<u64>(h), iw = in * static_cast<u64>(fw), ow = on * static_cast<u64>(w);
        const s64 a = static_cast<s64>(dh * iw), ih = static_cast<s64>(id * static_cast<u64>(fh));
        s64 bb = a + 1;
        if (static_cast<s64>(ow) > 0) bb = static_cast<s64>(ow) * ih;
        u64 nw, nh;
        if (static_cast<s64>(ow) > 0 && static_cast<s64>(dh) <= 0) bb = a; // straight to the second form
        if (bb > a) {
            nh = dh / od;
            const s64 d = static_cast<s64>(on) * ih;
            nw = static_cast<u64>((a + d - 1) / d);
        } else {
            nw = ow / on;
            const s64 d = static_cast<s64>(od * iw);
            nh = static_cast<u64>((bb + d - 1) / d);
        }
        ps_write16(b, f + 4, static_cast<u16>(nw & ~1ull));
        ps_write16(b, f + 6, static_cast<u16>(nh & ~1ull));
        const u8 coding = r8(ctx, r);
        if (coding == 1 || coding == 3) w8(ctx, f, coding);
    }
    const s8 kind = static_cast<s8>(r8(ctx, f));
    s32 pitch = static_cast<s16>(ps_read16(b, f + 4));
    if (kind >= 0) {
        if (kind <= 1) pitch <<= 2;
        else if (kind == 2) pitch <<= 1;
    }
    const s32 want = static_cast<s32>(r32(ctx, r + 8));
    s32 out = pitch;
    if (want > 0 && pitch < want) {
        out = want;
        if (!r8(ctx, slot + vs::kVpost) && (pitch & 0xF) != 0) out = pitch;
    }
    w32(ctx, f + 8, static_cast<u32>(out));
}

namespace {

// The dmux part added for an open (SDKs past 0x21FFFF): 0, or the error (INVALID_STATE, a pipeline not open, reads as
// CANCEL) completing the open's first step with the record's one step.
bool add_dmux_part(PPUContext& ctx, u32 p, u32 kind, u32 filter, u32 specific, u32 slot, u32 code)
{
    if (!sdk_after_22(ctx)) return true;
    s32 r = dmux_es_add(ctx, dmux(p), kind, filter, specific, slot, slot + as::kDmuxEs, false);
    if (r >= 0) return true;
    if (static_cast<u32>(r) == CELL_SAIL_ERROR_INVALID_STATE) r = error(CELL_SAIL_ERROR_CANCEL);
    w32(ctx, slot + 4, 1);
    post_completion(ctx, p, r, step(code, slot));
    return false;
}

} // namespace

// ------------------------------------------------------------------------------------------------- open

void es_open_audio(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 e, u64 handle)
{
    begin(ctx, slot, handle, false);
    w8(ctx, slot + as::kState, 1);
    audio_out_format(ctx, e, slot);
    w8(ctx, e, 2);
    if (!add_dmux_part(ctx, p, 0, e + 0xC, e + 0x1C, slot, 0x54)) return;
    w32(ctx, slot + 4, 2);
    const u32 r = r32(ctx, slot + as::kRenderer);
    if (sdk_version(ctx) - 0x310000 < 0x20000) {
        // SDKs 3.1 and 3.2: the renderer flagged (0x40) for a PAMF stream.
        w32(ctx, r + 0x34, (r32(ctx, r + 0x34) & ~0x40u) | (r32(ctx, d + ds::kType) == 0 ? 0x40u : 0u));
    }
    audio_renderer_open(ctx, r, e + 0x78, slot, step(0x55, slot));
    audio_pipeline_open(ctx, slot + as::kPipeline, e + 0x28, e + 0x2C, e + 0x60, slot + as::kDmuxEs, r, slot,
        step(0x56, slot));
}

void es_open_video(PPUContext& ctx, u32 p, u32, u32 slot, u32 e, u64 handle)
{
    begin(ctx, slot, handle, false);
    w8(ctx, slot + vs::kState, 1);
    video_out_format(ctx, e, slot);
    w8(ctx, e, 2);
    if (!add_dmux_part(ctx, p, 1, e + 0xC, e + 0x1C, slot, 0x57)) return;
    w32(ctx, slot + 4, 2);
    const u32 r = r32(ctx, slot + vs::kRenderer);
    video_renderer_open(ctx, r, e + 0x58, slot, step(0x59, slot));
    video_pipeline_open(ctx, slot + vs::kPipeline, e + 0x20, e + 0x38, e + 0x58, slot + vs::kDmuxEs, r, slot,
        step(0x58, slot));
}

void es_open_user(PPUContext& ctx, u32 p, u32 d, u32 slot, u32 e, u64 handle)
{
    begin(ctx, slot, handle, false);
    w8(ctx, slot + us::kState, 1);
    w8(ctx, e, 2);
    if (!add_dmux_part(ctx, p, 2, e + 0x3C, e + 0xC, slot, 0x5A)) return;
    w32(ctx, slot + 4, 1);
    receiver_open(ctx, r32(ctx, slot + us::kReceiver), r32(ctx, d + ds::kType), e + 0x3C, slot, step(0x5B, slot));
}

// ------------------------------------------------------------------------------------------------- close

// The pipeline and renderer closed and the dmux part removed (a removal's failure one more step, completing at once).
void es_close_audio(PPUContext& ctx, u32 p, u32 slot, u64 handle, bool failed)
{
    begin(ctx, slot, handle, failed);
    w32(ctx, slot + 4, 2);
    w8(ctx, slot + as::kState, 7);
    audio_pipeline_close(ctx, slot + as::kPipeline, step(0x65, slot));
    audio_renderer_close(ctx, r32(ctx, slot + as::kRenderer), step(0x66, slot));
    const s32 r = dmux_es_remove(ctx, dmux(p), slot + as::kDmuxEs);
    if (r >= 0) return;
    w32(ctx, slot + 4, 3);
    post_completion(ctx, p, r, step(0x64, slot));
}

void es_close_video(PPUContext& ctx, u32 p, u32 slot, u64 handle, bool failed)
{
    begin(ctx, slot, handle, failed);
    w32(ctx, slot + 4, 2);
    w8(ctx, slot + vs::kState, 7);
    video_pipeline_close(ctx, slot + vs::kPipeline, step(0x68, slot));
    video_renderer_close(ctx, r32(ctx, slot + vs::kRenderer), step(0x69, slot));
    const s32 r = dmux_es_remove(ctx, dmux(p), slot + vs::kDmuxEs);
    if (r >= 0) return;
    w32(ctx, slot + 4, 3);
    post_completion(ctx, p, r, step(0x67, slot));
}

void es_close_user(PPUContext& ctx, u32 p, u32 slot, u64 handle, bool failed)
{
    begin(ctx, slot, handle, failed);
    w32(ctx, slot + 4, 1);
    w8(ctx, slot + us::kState, 7);
    receiver_close(ctx, r32(ctx, slot + us::kReceiver), step(0x6B, slot));
    const s32 r = dmux_es_remove(ctx, dmux(p), slot + us::kDmuxEs);
    if (r >= 0) return;
    w32(ctx, slot + 4, 2);
    post_completion(ctx, p, r, step(0x6A, slot));
}

// ------------------------------------------------------------------------------------------------- the steps

namespace {

// A step of an open done: a failure marks the record and is an error event (unless CANCEL with the later SDKs' dmux
// part); with none out the stream is open (2), counted (and in mode 1 counted again), event 9 and the open completes,
// or, failed, it is closed again (its open's handle kept, the record marked).
void open_counted(PPUContext& ctx, u32 p, u32 kind, s32 result, u32 slot)
{
    const u32 state = kind == 0 ? as::kState : kind == 1 ? vs::kState : us::kState;
    const u32 index = r8(ctx, slot + (kind == 0 ? as::kIndex : kind == 1 ? vs::kIndex : us::kIndex));
    if (result < 0) {
        mark_failed(ctx, slot);
        if (!sdk_after_22(ctx) || static_cast<u32>(result) != CELL_SAIL_ERROR_CANCEL) notify_error(ctx, p, result);
    }
    if (count_down(ctx, slot)) return;
    w8(ctx, slot + state, 2);
    if (kind == 0) w8(ctx, p + kAudioOpen, r8(ctx, p + kAudioOpen) + 1);
    if (kind == 1) w8(ctx, p + kVideoOpen, r8(ctx, p + kVideoOpen) + 1);
    if (flags(ctx, slot) & 1) {
        const u64 handle = r64(ctx, slot + 8);
        if (kind == 0) es_close_audio(ctx, p, slot, handle, true);
        else if (kind == 1) es_close_video(ctx, p, slot, handle, true);
        else es_close_user(ctx, p, slot, handle, true);
        return;
    }
    if (kind == 0 && ps_read16(gb(ctx), slot + as::kMode)) w8(ctx, p + kAudioModeOpen, r8(ctx, p + kAudioModeOpen) + 1);
    if (kind == 1 && ps_read16(gb(ctx), slot + vs::kMode)) w8(ctx, p + kVideoModeOpen, r8(ctx, p + kVideoModeOpen) + 1);
    notify_event(ctx, p, 9, kind, index);
    post_completion(ctx, p, 0, r64(ctx, slot + 8));
}

// A step of a close done (a failure an error event): with none out the stream is closed (0), its entry chosen again (1)
// or, after a failed open, cleared (0), uncounted, event 10, and the close completes.
void close_counted(PPUContext& ctx, u32 p, u32 kind, s32 result, u32 slot)
{
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor);
    const u32 index = r8(ctx, slot + (kind == 0 ? as::kIndex : kind == 1 ? vs::kIndex : us::kIndex));
    if (result < 0) notify_error(ctx, p, result);
    if (count_down(ctx, slot)) return;
    const u32 table = kind == 0 ? ds::kAudio : kind == 1 ? ds::kVideo : ds::kUser;
    const u32 stride = kind == 0 ? ds::kAudioStride : kind == 1 ? ds::kVideoStride : ds::kUserStride;
    w8(ctx, r32(ctx, d + table) + index * stride, (flags(ctx, slot) & 1) ? 0 : 1);
    if (kind == 0) {
        w8(ctx, p + kAudioOpen, r8(ctx, p + kAudioOpen) - 1);
        if (ps_read16(gb(ctx), slot + as::kMode)) w8(ctx, p + kAudioModeOpen, r8(ctx, p + kAudioModeOpen) - 1);
        w8(ctx, slot + as::kState, 0);
    } else if (kind == 1) {
        w8(ctx, p + kVideoOpen, r8(ctx, p + kVideoOpen) - 1);
        if (ps_read16(gb(ctx), slot + vs::kMode)) w8(ctx, p + kVideoModeOpen, r8(ctx, p + kVideoModeOpen) - 1);
        w8(ctx, slot + vs::kState, 0);
    } else {
        w8(ctx, slot + us::kState, 0);
    }
    // libsail's event for a user stream closed names the kind as video (1).
    notify_event(ctx, p, 10, kind == 2 ? 1 : kind, index);
    post_completion(ctx, p, 0, r64(ctx, slot + 8));
}

// ------------------------------------------------------------------------------------------------- the calls

constexpr u32 kAudioCall = 0, kVideoCall = 1, kUserCall = 2;

u32 count_of(PPUContext& ctx, u32 p, u32 kind)
{
    return r32(ctx, p + (kind == 0 ? pl::kAudioCount : kind == 1 ? pl::kVideoCount : pl::kUserCount));
}

u32 slot_of(PPUContext& ctx, u32 p, u32 kind, u32 i)
{
    return kind == 0 ? audio_slot(ctx, p, i) : kind == 1 ? video_slot(ctx, p, i) : user_slot(ctx, p, i);
}

u32 entry_of(PPUContext& ctx, u32 d, u32 kind, u32 i)
{
    const u32 table = kind == 0 ? ds::kAudio : kind == 1 ? ds::kVideo : ds::kUser;
    const u32 stride = kind == 0 ? ds::kAudioStride : kind == 1 ? ds::kVideoStride : ds::kUserStride;
    return r32(ctx, d + table) + i * stride;
}

u32 slot_state(PPUContext& ctx, u32 slot, u32 kind)
{
    return r8(ctx, slot + (kind == 0 ? as::kState : kind == 1 ? vs::kState : us::kState));
}

bool open_or_ready(PPUContext& ctx, u32 p)
{
    const u8 st = r8(ctx, p + pl::kState);
    return (st == 4 || st == 6) && r32(ctx, p + kCallRunning) == 0;
}

// The stream of `kind` opened, closed (`failed` marks the record) for the current call.
void open_kind(PPUContext& ctx, u32 p, u32 kind, u32 d, u32 slot, u32 e, u64 handle)
{
    if (kind == 0) es_open_audio(ctx, p, d, slot, e, handle);
    else if (kind == 1) es_open_video(ctx, p, d, slot, e, handle);
    else es_open_user(ctx, p, d, slot, e, handle);
}

void close_kind(PPUContext& ctx, u32 p, u32 kind, u32 slot, u64 handle, bool failed)
{
    if (kind == 0) es_close_audio(ctx, p, slot, handle, failed);
    else if (kind == 1) es_close_video(ctx, p, slot, handle, failed);
    else es_close_user(ctx, p, slot, handle, failed);
}

u32 slot_index(PPUContext& ctx, u32 slot, u32 kind)
{
    return r8(ctx, slot + (kind == 0 ? as::kIndex : kind == 1 ? vs::kIndex : us::kIndex));
}

// The codes of an OpenEs call's steps by kind: started, opened, closed, stopped.
constexpr u32 kOpenSteps[3] = {0x8D, 0x93, 0x99};
// And of a CloseEs call's: stopped, closed.
constexpr u32 kCloseSteps[3] = {0x9F, 0xA2, 0xA5};

// OpenEs / ReopenEs {index, the elementary stream, the game's u64, reopen}: the stream chosen through select_es (one
// chosen already needs reopen: USING), its call running (4-6, or 13-15 for a reopen). A stream not chosen is opened; a
// reopened one has its entry chosen and, not started and its format unchanged, its dmux part set again (events 10 and
// 9, completing at once), else it is closed and opened again (started streams: a later part).
void open_es_call(PPUContext& ctx, u32 p, u32 kind, u32 args, u32 record)
{
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor);
    s32 r;
    u32 index = 0xFFFFFFFF;
    bool chosen = false;
    s32 changed = 0;
    if ((r = player_lock(ctx, p)) >= 0) {
        index = r32(ctx, args);
        const bool reopen = r8(ctx, args + 0x10) != 0;
        r = error(CELL_SAIL_ERROR_INVALID_STATE);
        if (open_or_ready(ctx, p)) {
            r = error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
            if (index < count_of(ctx, p, kind)) {
                chosen = r8(ctx, entry_of(ctx, d, kind, index)) != 0;
                r = error(CELL_SAIL_ERROR_USING);
                if (reopen || !chosen) {
                    r = error(CELL_SAIL_ERROR_INVALID_STATE);
                    if (r32(ctx, p + kPending) == 0) {
                        r = changed = select_es(ctx, d, static_cast<s32>(kind), index, r32(ctx, args + 4));
                        if (changed >= 0) {
                            r = 0;
                            w64(ctx, p + pl::kBootArg, r64(ctx, args + 8));
                            w32(ctx, p + kCallRunning, (reopen ? 13 : 4) + kind);
                        }
                    }
                }
            }
        }
    }
    if (!answer(ctx, p, record, r) || r < 0) return;
    const u32 rec = p + kCallRecord;
    std::memset(gb(ctx) + rec, 0, 0x10);
    w32(ctx, rec + 4, index);
    const u32 slot = slot_of(ctx, p, kind, index), e = entry_of(ctx, d, kind, index);
    const u32 steps = kOpenSteps[kind];
    if (!chosen) {
        open_kind(ctx, p, kind, d, slot, e, step(steps + 1, rec));
        return;
    }
    w8(ctx, e, 2);
    const bool open_only = r8(ctx, p + kStreamPhase) == 2;
    if (!open_only) later_part(ctx, "reopening an elementary stream of a started stream");
    if (changed != 0) {
        close_kind(ctx, p, kind, slot, step(steps + 2, rec), false);
        return;
    }
    notify_event(ctx, p, 10, kind, slot_index(ctx, slot, kind));
    const u32 filter = kind == 2 ? e + 0x3C : e + 0xC, specific = kind == 2 ? e + 0xC : e + 0x1C;
    const s32 a = dmux_es_readd(ctx, dmux(p), kind, filter, specific, slot, slot + as::kDmuxEs);
    notify_event(ctx, p, 9, kind, slot_index(ctx, slot, kind));
    post_completion(ctx, p, a, step(steps + 1, rec));
}

// An OpenEs call's steps (`which` 0 started, 1 opened, 2 closed, 3 stopped; the record's +4 the stream's index).
void open_es_step(PPUContext& ctx, u32 p, u32 kind, u32 which, s32 result, u32 rec)
{
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor), index = r32(ctx, rec + 4);
    const u32 slot = slot_of(ctx, p, kind, index), steps = kOpenSteps[kind];
    const u8 phase = r8(ctx, p + kStreamPhase);
    const u32 call = r32(ctx, p + kCallRunning);
    switch (which) {
    case 0: // started
        if (result < 0) close_kind(ctx, p, kind, slot, step(steps + 2, rec), true);
        else call_completed(ctx, p, call, 0);
        break;
    case 1: // opened
        if (result < 0) clear_es(ctx, d, static_cast<s32>(kind), index);
        else if (phase == 4) later_part(ctx, "starting an elementary stream opened in a started stream");
        else call_completed(ctx, p, call, 0);
        break;
    case 2: // closed (to open again)
        if (result < 0) {
            clear_es(ctx, d, static_cast<s32>(kind), index);
            call_completed(ctx, p, call, result);
        } else if (phase == 2 || phase == 4 || phase == 5) {
            open_kind(ctx, p, kind, d, slot, entry_of(ctx, d, kind, index), step(steps + 1, rec));
        }
        break;
    default: later_part(ctx, "an elementary stream of a started stream stopped for a reopen"); break;
    }
}

// CloseEs {index, the game's u64}: a chosen and open stream (its entry 2) closed (started ones stopped first: a later
// part), its call 7-9 running.
void close_es_call(PPUContext& ctx, u32 p, u32 kind, u32 args, u32 record)
{
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor);
    s32 r;
    u32 index = 0xFFFFFFFF;
    if ((r = player_lock(ctx, p)) >= 0) {
        index = r32(ctx, args);
        r = error(CELL_SAIL_ERROR_INVALID_STATE);
        if (open_or_ready(ctx, p)) {
            r = error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
            if (index < count_of(ctx, p, kind)) {
                r = error(CELL_SAIL_ERROR_INVALID_STATE);
                if (r8(ctx, entry_of(ctx, d, kind, index)) == 2) {
                    r = 0;
                    w64(ctx, p + pl::kBootArg, r64(ctx, args + 8));
                    w32(ctx, p + kCallRunning, 7 + kind);
                }
            }
        }
    }
    if (!answer(ctx, p, record, r) || r < 0) return;
    const u32 rec = p + kCallRecord;
    std::memset(gb(ctx) + rec, 0, 0x10);
    w32(ctx, rec + 4, index);
    const u32 slot = slot_of(ctx, p, kind, index);
    const u32 st = slot_state(ctx, slot, kind);
    if (st == 4) later_part(ctx, "closing an elementary stream of a started stream");
    else if (st == 2) close_kind(ctx, p, kind, slot, step(kCloseSteps[kind] + 1, rec), false);
}

// A CloseEs call's steps: stopped (then closed), closed (its entry cleared and the call completed).
void close_es_step(PPUContext& ctx, u32 p, u32 kind, bool closed, s32 result, u32 rec)
{
    const u32 d = r32(ctx, p + pl::kCurrentDescriptor), index = r32(ctx, rec + 4);
    if (result < 0) notify_error(ctx, p, result);
    if (!closed) {
        close_kind(ctx, p, kind, slot_of(ctx, p, kind, index), step(kCloseSteps[kind] + 1, rec), false);
        return;
    }
    w8(ctx, entry_of(ctx, d, kind, index), 0);
    call_completed(ctx, p, 7 + kind, result);
}

} // namespace

bool es_message(PPUContext& ctx, u32 p, u32 code, u32 a, u32 b, u32 c)
{
    const s32 result = static_cast<s32>(a);
    switch (code) {
    case 0x33: audio_renderer_completed(ctx, b, static_cast<s32>(c)); return true;
    case 0x34: video_renderer_completed(ctx, b, static_cast<s32>(c)); return true;
    case 0x35: case 0x36: case 0x37: case 0x38: receiver_answered(ctx, b, code, static_cast<s32>(c)); return true;
    case 0x54: case 0x55: case 0x56: open_counted(ctx, p, 0, result, c); return true;
    case 0x57: case 0x58: case 0x59: open_counted(ctx, p, 1, result, c); return true;
    case 0x5A: case 0x5B: open_counted(ctx, p, 2, result, c); return true;
    case 0x64: case 0x65: case 0x66: close_counted(ctx, p, 0, result, c); return true;
    case 0x67: case 0x68: case 0x69: close_counted(ctx, p, 1, result, c); return true;
    case 0x6A: case 0x6B: close_counted(ctx, p, 2, result, c); return true;
    case 0x8C: open_es_call(ctx, p, kAudioCall, b, c); return true;
    case 0x92: open_es_call(ctx, p, kVideoCall, b, c); return true;
    case 0x98: open_es_call(ctx, p, kUserCall, b, c); return true;
    case 0x8D: case 0x8E: case 0x8F: case 0x90: open_es_step(ctx, p, 0, code - 0x8D, result, c); return true;
    case 0x93: case 0x94: case 0x95: case 0x96: open_es_step(ctx, p, 1, code - 0x93, result, c); return true;
    case 0x99: case 0x9A: case 0x9B: case 0x9C: open_es_step(ctx, p, 2, code - 0x99, result, c); return true;
    case 0x9E: close_es_call(ctx, p, kAudioCall, b, c); return true;
    case 0xA1: close_es_call(ctx, p, kVideoCall, b, c); return true;
    case 0xA4: close_es_call(ctx, p, kUserCall, b, c); return true;
    case 0x9F: case 0xA0: close_es_step(ctx, p, 0, code == 0xA0, result, c); return true;
    case 0xA2: case 0xA3: close_es_step(ctx, p, 1, code == 0xA3, result, c); return true;
    case 0xA5: case 0xA6: close_es_step(ctx, p, 2, code == 0xA6, result, c); return true;
    default: return false;
    }
}

} // namespace pspocket::hle::sail

namespace pspocket::hle {
namespace {

using namespace sail;

// OpenEs / ReopenEs (player, index, the elementary stream, a pointer that must be given, the game's u64): the command's
// answer. The completion event carries the u64.
s32 open_es(PPUContext& ctx, u32 p, u32 code, u32 index, u32 es, u32 required, u64 arg, bool reopen)
{
    if (!required) return error(CELL_SAIL_ERROR_INVALID_ARG);
    StackVar args(ctx, gb(ctx), 0x18);
    w32(ctx, args.addr(), index);
    w32(ctx, args.addr() + 4, es);
    w64(ctx, args.addr() + 8, arg);
    w8(ctx, args.addr() + 0x10, reopen ? 1 : 0);
    return command_answer(ctx, p, code, args.addr());
}

s32 cellSailPlayerOpenEsAudio(PPUContext& ctx, u32 p, u32 i, u32 es, u32 required, u64 arg)
{
    return open_es(ctx, p, 0x8C, i, es, required, arg, false);
}
s32 cellSailPlayerOpenEsVideo(PPUContext& ctx, u32 p, u32 i, u32 es, u32 required, u64 arg)
{
    return open_es(ctx, p, 0x92, i, es, required, arg, false);
}
s32 cellSailPlayerOpenEsUser(PPUContext& ctx, u32 p, u32 i, u32 es, u32 required, u64 arg)
{
    return open_es(ctx, p, 0x98, i, es, required, arg, false);
}
s32 cellSailPlayerReopenEsAudio(PPUContext& ctx, u32 p, u32 i, u32 es, u32 required, u64 arg)
{
    return open_es(ctx, p, 0x8C, i, es, required, arg, true);
}
s32 cellSailPlayerReopenEsVideo(PPUContext& ctx, u32 p, u32 i, u32 es, u32 required, u64 arg)
{
    return open_es(ctx, p, 0x92, i, es, required, arg, true);
}
s32 cellSailPlayerReopenEsUser(PPUContext& ctx, u32 p, u32 i, u32 es, u32 required, u64 arg)
{
    return open_es(ctx, p, 0x98, i, es, required, arg, true);
}

// CloseEs (player, index, the game's u64): the command's answer.
s32 close_es(PPUContext& ctx, u32 p, u32 code, u32 index, u64 arg)
{
    StackVar args(ctx, gb(ctx), 0x10);
    w32(ctx, args.addr(), index);
    w64(ctx, args.addr() + 8, arg);
    return command_answer(ctx, p, code, args.addr());
}

s32 cellSailPlayerCloseEsAudio(PPUContext& ctx, u32 p, u32 i, u64 arg) { return close_es(ctx, p, 0x9E, i, arg); }
s32 cellSailPlayerCloseEsVideo(PPUContext& ctx, u32 p, u32 i, u64 arg) { return close_es(ctx, p, 0xA1, i, arg); }
s32 cellSailPlayerCloseEsUser(PPUContext& ctx, u32 p, u32 i, u64 arg) { return close_es(ctx, p, 0xA4, i, arg); }

} // namespace

void register_cellSailEs(Registry& r)
{
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}
    r.add("cellSail", {
        SAIL(cellSailPlayerOpenEsAudio),
        SAIL(cellSailPlayerOpenEsVideo),
        SAIL(cellSailPlayerOpenEsUser),
        SAIL(cellSailPlayerReopenEsAudio),
        SAIL(cellSailPlayerReopenEsVideo),
        SAIL(cellSailPlayerReopenEsUser),
        SAIL(cellSailPlayerCloseEsAudio),
        SAIL(cellSailPlayerCloseEsVideo),
        SAIL(cellSailPlayerCloseEsUser),
    });
#undef SAIL
}

} // namespace pspocket::hle

