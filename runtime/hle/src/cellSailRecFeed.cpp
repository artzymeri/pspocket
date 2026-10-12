// cellSailRec's feeders (D-918, part 7), as libsail_rec (fw 4.92) drives a game's feeder (CellSailFeederAudio,
// CellSailFeederVideo, cellSailRec.hpp fd::): the eleven callbacks libsail_rec puts in front of the game's
// (cellSailFeederAudio/VideoInitialize, cellSailRecObjects.cpp) and the stream's side the recorder runs through them,
// each step's end a completion for its handle. Audio and video feeders are the same but for the format kept, the
// diagnostic handler's place and the notifications' codes.
//
// The stream's side's states (+0x40): 0 none, 1 made (Boot's makeup), 2 opening, 3 open, 4 starting, 5 recording, 6
// stopping (the game's stop asked), 7 recording up to the end time, 8 the end time reached (the game's stop asked),
// 9 closing, 10 draining, 11 ended (the session's step completes once the pipeline gives its last frame back).
// A frame is taken for the pipeline through the game's checkout: one before the start time or not after the last
// taken is given straight back; one at or past the end time ends a recording session (the session-end
// notification); the frames the game says are out and the pipeline does not want are taken and given back at once.

#include <cstring>

#include "cellSailRec.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

// ------------------------------------------------------------------------------------------------- notifications

// The notifications libsail_rec makes itself (the game's cellSailFeederAudio/VideoNotify* are cellSailRecObjects.cpp's
// and the same): the diagnostic handler(argument, kind, value, 0) when the feeder holds one, else the recorder's
// message {code, feeder, value}.
struct Kind {
    u32 diag, completed, session_end;
};
constexpr Kind kAudio{fd::kAudioDiag, 0xF, 0x11};
constexpr Kind kVideo{fd::kVideoDiag, 0x19, 0x1B};

u32 rec_of(PPUContext& ctx, u32 f) { return r32(ctx, f + fd::kRecorder); }

void notify(PPUContext& ctx, u32 f, const Kind& k, u32 kind, u32 code, s32 value)
{
    if (const u32 diag = r32(ctx, f + k.diag)) {
        call_guest(ctx, diag, {r32(ctx, f + k.diag + 4), kind, sx32(static_cast<u32>(value)), 0});
        return;
    }
    post(ctx, rec_of(ctx, f), code, f, static_cast<u32>(value));
}

// ------------------------------------------------------------------------------------------------- the own table

// libsail_rec's eleven callbacks in front of the game's (argument the feeder): each passes on the registers it does
// not set, as libsail_rec's do. 0 makeup and 1 cleanup (0 without the game's); 2 open (the format kept, the game's with
// the format and 0); 3 close, 4 start and 5 stop (the game's with 0); each of 2 to 5 without the game's callback the
// call completed at once with 0; 6 cancel (the game's when it has one); 7 the format (the game's with 0 and the place
// for it, or the one kept); 8 checkout, 9 checkin and 10 clear (the game's always). Makeup, cleanup, the format and 8
// to 10 sign-extend the answer.
template <bool Video>
struct Own {
    static constexpr u32 kFuncs = Video ? fd::kVideoFuncs : fd::kAudioFuncs, kArg = Video ? fd::kVideoArg : fd::kAudioArg;
    static constexpr u32 kKept = Video ? fd::kVideoFormat : fd::kAudioFormat, kKeptSize = Video ? 0x20 : 0x18;
    static constexpr const Kind& kind() { return Video ? kVideo : kAudio; }

    static u32 game(PPUContext& ctx, u32 f, u32 i) { return r32(ctx, f + kFuncs + 4 * i); }
    static u32 arg(PPUContext& ctx, u32 f) { return r32(ctx, f + kArg); }
    static u64 completed(PPUContext& ctx, u32 f)
    {
        notify(ctx, f, kind(), 0, kind().completed, 0);
        return ctx.gpr[3];
    }
    static u64 optional(PPUContext& ctx, u32 f, u32 i)
    {
        const u32 fn = game(ctx, f, i);
        return fn ? sx32(static_cast<u32>(call_guest(ctx, fn, {arg(ctx, f)}))) : 0;
    }
    static u64 open(PPUContext& ctx, u32 f, u32 format)
    {
        std::memmove(gb(ctx) + f + kKept, gb(ctx) + format, kKeptSize);
        const u32 fn = game(ctx, f, fd::kOpen);
        return fn ? call_guest(ctx, fn, {arg(ctx, f), format, 0}) : completed(ctx, f);
    }
    static u64 with_zero(PPUContext& ctx, u32 f, u32 i)
    {
        const u32 fn = game(ctx, f, i);
        return fn ? call_guest(ctx, fn, {arg(ctx, f), 0}) : completed(ctx, f);
    }
    static u64 cancel(PPUContext& ctx, u32 f)
    {
        const u32 fn = game(ctx, f, fd::kCancel);
        return fn ? call_guest(ctx, fn, {arg(ctx, f)}) : f;
    }
    static u64 format(PPUContext& ctx, u32 f, u32 out)
    {
        if (const u32 fn = game(ctx, f, fd::kFormat)) return sx32(static_cast<u32>(call_guest(ctx, fn, {arg(ctx, f), 0, out})));
        std::memmove(gb(ctx) + out, gb(ctx) + f + kKept, kKeptSize);
        return 0;
    }
    static u64 always(PPUContext& ctx, u32 f, u32 i)
    {
        return sx32(static_cast<u32>(call_guest(ctx, game(ctx, f, i), {arg(ctx, f)})));
    }
};

using A = Own<false>;
using V = Own<true>;

u64 _pspocket_cellSailRec_AudioMakeup(PPUContext& ctx, u32 f) { return A::optional(ctx, f, fd::kMakeup); }
u64 _pspocket_cellSailRec_AudioCleanup(PPUContext& ctx, u32 f) { return A::optional(ctx, f, fd::kCleanup); }
u64 _pspocket_cellSailRec_AudioOpen(PPUContext& ctx, u32 f, u32 format) { return A::open(ctx, f, format); }
u64 _pspocket_cellSailRec_AudioClose(PPUContext& ctx, u32 f) { return A::with_zero(ctx, f, fd::kClose); }
u64 _pspocket_cellSailRec_AudioStart(PPUContext& ctx, u32 f) { return A::with_zero(ctx, f, fd::kStartCall); }
u64 _pspocket_cellSailRec_AudioStop(PPUContext& ctx, u32 f) { return A::with_zero(ctx, f, fd::kStop); }
u64 _pspocket_cellSailRec_AudioCancel(PPUContext& ctx, u32 f) { return A::cancel(ctx, f); }
u64 _pspocket_cellSailRec_AudioFormat(PPUContext& ctx, u32 f, u32, u32 out) { return A::format(ctx, f, out); }
u64 _pspocket_cellSailRec_AudioCheckout(PPUContext& ctx, u32 f) { return A::always(ctx, f, fd::kCheckout); }
u64 _pspocket_cellSailRec_AudioCheckin(PPUContext& ctx, u32 f) { return A::always(ctx, f, fd::kCheckin); }
u64 _pspocket_cellSailRec_AudioClear(PPUContext& ctx, u32 f) { return A::always(ctx, f, fd::kClear); }
u64 _pspocket_cellSailRec_VideoMakeup(PPUContext& ctx, u32 f) { return V::optional(ctx, f, fd::kMakeup); }
u64 _pspocket_cellSailRec_VideoCleanup(PPUContext& ctx, u32 f) { return V::optional(ctx, f, fd::kCleanup); }
u64 _pspocket_cellSailRec_VideoOpen(PPUContext& ctx, u32 f, u32 format) { return V::open(ctx, f, format); }
u64 _pspocket_cellSailRec_VideoClose(PPUContext& ctx, u32 f) { return V::with_zero(ctx, f, fd::kClose); }
u64 _pspocket_cellSailRec_VideoStart(PPUContext& ctx, u32 f) { return V::with_zero(ctx, f, fd::kStartCall); }
u64 _pspocket_cellSailRec_VideoStop(PPUContext& ctx, u32 f) { return V::with_zero(ctx, f, fd::kStop); }
u64 _pspocket_cellSailRec_VideoCancel(PPUContext& ctx, u32 f) { return V::cancel(ctx, f); }
u64 _pspocket_cellSailRec_VideoFormat(PPUContext& ctx, u32 f, u32, u32 out) { return V::format(ctx, f, out); }
u64 _pspocket_cellSailRec_VideoCheckout(PPUContext& ctx, u32 f) { return V::always(ctx, f, fd::kCheckout); }
u64 _pspocket_cellSailRec_VideoCheckin(PPUContext& ctx, u32 f) { return V::always(ctx, f, fd::kCheckin); }
u64 _pspocket_cellSailRec_VideoClear(PPUContext& ctx, u32 f) { return V::always(ctx, f, fd::kClear); }

constexpr std::string_view kAudioCalls[11] = {"_pspocket_cellSailRec_AudioMakeup", "_pspocket_cellSailRec_AudioCleanup",
    "_pspocket_cellSailRec_AudioOpen", "_pspocket_cellSailRec_AudioClose", "_pspocket_cellSailRec_AudioStart",
    "_pspocket_cellSailRec_AudioStop", "_pspocket_cellSailRec_AudioCancel", "_pspocket_cellSailRec_AudioFormat",
    "_pspocket_cellSailRec_AudioCheckout", "_pspocket_cellSailRec_AudioCheckin", "_pspocket_cellSailRec_AudioClear"};
constexpr std::string_view kVideoCalls[11] = {"_pspocket_cellSailRec_VideoMakeup", "_pspocket_cellSailRec_VideoCleanup",
    "_pspocket_cellSailRec_VideoOpen", "_pspocket_cellSailRec_VideoClose", "_pspocket_cellSailRec_VideoStart",
    "_pspocket_cellSailRec_VideoStop", "_pspocket_cellSailRec_VideoCancel", "_pspocket_cellSailRec_VideoFormat",
    "_pspocket_cellSailRec_VideoCheckout", "_pspocket_cellSailRec_VideoCheckin", "_pspocket_cellSailRec_VideoClear"};

// ------------------------------------------------------------------------------------------------- the stream's side

u32 own(PPUContext& ctx, u32 f, u32 i) { return r32(ctx, f + 4 * i); }

// The own callback `i` with the feeder's argument and `rest` in r4 on.
template <typename... T>
u64 call_own(PPUContext& ctx, u32 f, u32 i, T... rest)
{
    return call_guest(ctx, own(ctx, f, i), {u64{r32(ctx, f + fd::kArgument)}, static_cast<u64>(rest)...});
}

u32 state(PPUContext& ctx, u32 f) { return r32(ctx, f + fd::kState); }
void set(PPUContext& ctx, u32 f, u32 s) { w32(ctx, f + fd::kState, s); }

void complete(PPUContext& ctx, u32 f, s32 result) { post_completion(ctx, rec_of(ctx, f), result, r64(ctx, f + fd::kHandle)); }

// `n` frames the game has out taken and given straight back (none recorded).
void drop(PPUContext& ctx, u32 f, s32 n)
{
    StackVar frame(ctx, gb(ctx), 16);
    for (s32 i = 0; i < n; i++) {
        call_own(ctx, f, fd::kCheckout, frame.addr());
        call_own(ctx, f, fd::kCheckin, r32(ctx, frame.addr()));
    }
}

// The session's end once the pipeline holds no frame: the frames still out dropped, the game's clear, open again
// (3), the step completed with the session's result.
void session_end(PPUContext& ctx, u32 f)
{
    if (r32(ctx, f + fd::kHeld)) return;
    const s32 n = static_cast<s32>(r32(ctx, f + fd::kFrames));
    w32(ctx, f + fd::kFrames, 0);
    drop(ctx, f, n);
    call_own(ctx, f, fd::kClear);
    set(ctx, f, 3);
    complete(ctx, f, static_cast<s32>(r32(ctx, f + fd::kResult)));
}

void drain_check(PPUContext& ctx, u32 f)
{
    if (r32(ctx, f + fd::kDrain)) return;
    set(ctx, f, 11);
    session_end(ctx, f);
}

// Draining (10): the frames out now are the last the pipeline takes.
void drain(PPUContext& ctx, u32 f)
{
    set(ctx, f, 10);
    w32(ctx, f + fd::kDrain, r32(ctx, f + fd::kFrames));
    drain_check(ctx, f);
}

// The end time reached by the last frame taken: the game's stop asked (8), `r5` passed on in r5.
void end_check(PPUContext& ctx, u32 f, u64 r5)
{
    if (r64(ctx, f + fd::kEnd) > r64(ctx, f + fd::kLast)) return;
    set(ctx, f, 8);
    if (own(ctx, f, fd::kStop)) {
        call_own(ctx, f, fd::kStop, 0, r5);
        return;
    }
    drain(ctx, f);
}

} // namespace

std::string_view feeder_call_name(bool video, u32 i) { return video ? kVideoCalls[i] : kAudioCalls[i]; }

// ------------------------------------------------------------------------------------------------- the steps

s32 feeder_makeup(PPUContext& ctx, u32 f, u32 rec, u32 index)
{
    w32(ctx, f + fd::kRecorder, rec);
    w32(ctx, f + fd::kIndex, index);
    set(ctx, f, 1);
    if (!own(ctx, f, fd::kMakeup)) return 0;
    return static_cast<s32>(static_cast<u32>(call_own(ctx, f, fd::kMakeup, rec, index)));
}

s32 feeder_cleanup(PPUContext& ctx, u32 f)
{
    set(ctx, f, 0);
    if (!own(ctx, f, fd::kCleanup)) return 0;
    return static_cast<s32>(static_cast<u32>(call_own(ctx, f, fd::kCleanup)));
}

void feeder_cancel(PPUContext& ctx, u32 f)
{
    if (own(ctx, f, fd::kCancel)) call_own(ctx, f, fd::kCancel);
}

// Open (2): the pipeline and the slot kept, the own open with the format (and, for audio, the frames in 8000 samples,
// for video 2: the own open passes 0 on).
void feeder_open(PPUContext& ctx, u32 f, u32 pipeline, u32 slot, u32 format, u64 handle, bool video)
{
    w32(ctx, f + fd::kPipeline, pipeline);
    set(ctx, f, 2);
    w32(ctx, f + fd::kSlot, slot);
    w64(ctx, f + fd::kHandle, handle);
    const Kind& k = video ? kVideo : kAudio;
    if (!own(ctx, f, fd::kOpen)) {
        notify(ctx, f, k, 0, k.completed, 0);
        return;
    }
    u32 third = 2;
    if (!video) {
        const s32 samples = static_cast<s16>(r16(ctx, format + 2));
        third = static_cast<u32>(divs(samples + 7999, samples));
    }
    call_own(ctx, f, fd::kOpen, format, third);
}

// Start (4): the session, the recording's start and end times, no frame taken yet (the last time all ones); the own
// start with the session, the times and the handle.
void feeder_start(PPUContext& ctx, u32 f, u32 session, u64 start, u64 end, u64 handle, bool video)
{
    w64(ctx, f + fd::kHandle, handle);
    w32(ctx, f + fd::kSession, session);
    set(ctx, f, 4);
    w64(ctx, f + fd::kStart, start);
    w64(ctx, f + fd::kEnd, end);
    w64(ctx, f + fd::kLast, ~u64{0});
    if (own(ctx, f, fd::kStartCall)) {
        call_own(ctx, f, fd::kStartCall, session, start, end, handle);
        return;
    }
    const Kind& k = video ? kVideo : kAudio;
    notify(ctx, f, k, 0, k.completed, 0);
}

// Stop(flag, end time, handle), by the state: recording (5, 7) the end time kept: without the flag it stops there (7,
// checked now), with it the game's stop is asked (6, or ended at once without one); the end time reached (8) and
// draining (10) the flag forces; made or open (1, 3) completes with 1.
void feeder_stop(PPUContext& ctx, u32 f, u32 flag, u64 end, u64 handle)
{
    switch (state(ctx, f)) {
    case 1:
    case 3: post_completion(ctx, rec_of(ctx, f), 1, handle); return;
    case 5:
    case 7:
        w64(ctx, f + fd::kEnd, end);
        w64(ctx, f + fd::kHandle, handle);
        if (!flag) {
            set(ctx, f, 7);
            end_check(ctx, f, handle);
            return;
        }
        if (!own(ctx, f, fd::kStop)) {
            set(ctx, f, 11);
            session_end(ctx, f);
            return;
        }
        set(ctx, f, 6);
        call_own(ctx, f, fd::kStop, 1, handle);
        return;
    case 8:
        w64(ctx, f + fd::kHandle, handle);
        if (flag) set(ctx, f, 6);
        return;
    case 10:
        w64(ctx, f + fd::kHandle, handle);
        if (!flag) return;
        set(ctx, f, 11);
        w32(ctx, f + fd::kDrain, 0);
        session_end(ctx, f);
        return;
    case 11: w64(ctx, f + fd::kHandle, handle); return;
    default: return;
    }
}

// Close: the own close (closing, 9) unless only made or without one (completed with 0 at once).
void feeder_close(PPUContext& ctx, u32 f, u64 handle)
{
    if (state(ctx, f) != 1 && own(ctx, f, fd::kClose)) {
        w64(ctx, f + fd::kHandle, handle);
        set(ctx, f, 9);
        call_own(ctx, f, fd::kClose, 0, handle);
        return;
    }
    post_completion(ctx, rec_of(ctx, f), 0, handle);
}

s32 feeder_format(PPUContext& ctx, u32 f, u32 out)
{
    return static_cast<s32>(static_cast<u32>(call_own(ctx, f, fd::kFormat, 0, out)));
}

// The game's call completed: by the state the step moves on (open or not, recording or not, the session's end).
void feeder_completed(PPUContext& ctx, u32 f, s32 result)
{
    switch (state(ctx, f)) {
    case 2: set(ctx, f, result < 0 ? 1 : 3); break;
    case 4:
        if (result < 0) {
            w32(ctx, f + fd::kDrain, 0);
            set(ctx, f, 3);
            w32(ctx, f + fd::kFrames, 0);
        } else {
            set(ctx, f, 5);
        }
        break;
    case 6:
        set(ctx, f, 11);
        session_end(ctx, f);
        return;
    case 8: drain(ctx, f); return;
    case 9: set(ctx, f, 1); break;
    default: break;
    }
    complete(ctx, f, result);
}

// Frames out: counted; while nothing records them (not recording nor up to or at the end time, none held, none
// draining) they are dropped at once.
void feeder_frames_out(PPUContext& ctx, u32 f, u32 n)
{
    s32 frames = static_cast<s32>(n + r32(ctx, f + fd::kFrames));
    const u32 s = state(ctx, f);
    if (s != 5 && s != 7 && s != 8 && !r32(ctx, f + fd::kHeld) && !r32(ctx, f + fd::kDrain)) {
        drop(ctx, f, frames);
        frames = 0;
    }
    w32(ctx, f + fd::kFrames, static_cast<u32>(frames));
}

bool feeder_has_frames(PPUContext& ctx, u32 f) { return r32(ctx, f + fd::kFrames) && state(ctx, f) != 11; }

s32 feeder_take(PPUContext& ctx, u32 f, u32 out, bool video)
{
    s32 r = static_cast<s32>(static_cast<u32>(call_own(ctx, f, fd::kCheckout, out)));
    if (r < 0) return r;
    const u32 frame = r32(ctx, out);
    w32(ctx, f + fd::kFrames, r32(ctx, f + fd::kFrames) - 1);
    const u64 t = r64(ctx, frame + 0x10);
    u32 s;
    if (r64(ctx, f + fd::kStart) > t || static_cast<s64>(r64(ctx, f + fd::kLast)) > static_cast<s64>(t)) {
        r = -1;
        call_own(ctx, f, fd::kCheckin, frame);
        s = state(ctx, f);
        if (s == 11) {
            session_end(ctx, f);
            s = state(ctx, f);
        }
    } else {
        w64(ctx, f + fd::kLast, t);
        w32(ctx, frame + 0xC, r32(ctx, f + fd::kSession));
        s = state(ctx, f);
        w32(ctx, f + fd::kHeld, r32(ctx, f + fd::kHeld) + 1);
    }
    if (r64(ctx, f + fd::kEnd) <= t && s == 5) {
        const Kind& k = video ? kVideo : kAudio;
        notify(ctx, f, k, 1, k.session_end, 0);
        s = state(ctx, f);
    }
    if (s == 7) {
        end_check(ctx, f, ctx.gpr[5]);
        s = state(ctx, f);
    }
    if (s == 10) {
        w32(ctx, f + fd::kDrain, r32(ctx, f + fd::kDrain) - 1);
        drain_check(ctx, f);
    }
    return r;
}

s32 feeder_release(PPUContext& ctx, u32 f, u32 frame)
{
    const s32 r = static_cast<s32>(static_cast<u32>(call_own(ctx, f, fd::kCheckin, frame)));
    if (r < 0) return r;
    w32(ctx, f + fd::kHeld, r32(ctx, f + fd::kHeld) - 1);
    if (state(ctx, f) == 11) session_end(ctx, f);
    return r;
}

#define SAILREC_INTERNAL(fn) PS_HLE_INTERNAL(recomp::nid_of(#fn), fn)

void register_feed(Registry& r)
{
    r.add(kChan, {
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioMakeup),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioCleanup),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioOpen),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioClose),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioStart),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioStop),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioCancel),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioFormat),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioCheckout),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioCheckin),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioClear),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoMakeup),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoCleanup),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoOpen),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoClose),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoStart),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoStop),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoCancel),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoFormat),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoCheckout),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoCheckin),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoClear),
    });
}

#undef SAILREC_INTERNAL

} // namespace pspocket::hle::sailrec
