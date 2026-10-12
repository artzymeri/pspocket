// cellSailRec's objects (D-918): the game's feeders, its composer, the profiles and the video converters, as
// libsail_rec (fw 4.92) keeps and checks them (cellSailRec.hpp gives their layouts).
//
// - Feeders (CellSailFeederAudio/Video, 0x200 bytes): Initialize takes the game's eleven callbacks (makeup, cleanup,
//   open, start, stop, cancel and the rest: four of them required) and their argument and puts libsail_rec's own
//   eleven callbacks in front of them, their argument the feeder; the notifications are messages to the recorder
//   (audio 0x0F call completed, 0x10 frames out, 0x11 session end, 0x12 session error; video 0x19 to 0x1C), or calls
//   to a diagnostic handler the feeder may hold (none of libsail_rec's functions sets one).
// - The composer (CellSailComposer, 0x140 bytes): the game's seventeen callbacks and argument, two locks; the access
//   units it gets from the recorder's streams are taken (Get, TryGet: a stream's ring read in place, times in 90 kHz)
//   and given back (Release); its notifications are messages or a call's completion.
// - Profiles: their stream, audio and video parameters, checked and kept (the profile itself is the control thread's).
// - Video converters: the queue of requests and results (the conversion runs on the control thread's SPU tasks).

#include <cstring>
#include <format>

#include "cellSailRec.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {
namespace {

// libsail_rec's message to the recorder a feeder or composer holds (+0x34 for a feeder, +0 for a composer): before the
// recorder sets it there is none, and libsail_rec reads the queue at address 0x4A8 and faults.
s32 post_from(PPUContext& ctx, std::string_view what, u32 rec, u32 code, u32 object, u32 arg)
{
    if (!rec) {
        log_call_site(ctx, gb(ctx),
            std::format("{}: the object has no recorder yet (libsail_rec reads its queue at 0x4bc and faults there)",
                what));
        PS_FATAL(kChan, "{} before the recorder holds the object: libsail_rec faults here", what);
    }
    return post(ctx, rec, code, object, arg);
}

// ------------------------------------------------------------------------------------------------- feeders

constexpr std::string_view kAudioFeederCalls[11] = {"_pspocket_cellSailRec_AudioMakeup",
    "_pspocket_cellSailRec_AudioCleanup", "_pspocket_cellSailRec_AudioOpen", "_pspocket_cellSailRec_AudioStart",
    "_pspocket_cellSailRec_AudioStop", "_pspocket_cellSailRec_AudioCancel", "_pspocket_cellSailRec_AudioCheckout",
    "_pspocket_cellSailRec_AudioFormat", "_pspocket_cellSailRec_AudioGet", "_pspocket_cellSailRec_AudioRelease",
    "_pspocket_cellSailRec_AudioClose"};
constexpr std::string_view kVideoFeederCalls[11] = {"_pspocket_cellSailRec_VideoMakeup",
    "_pspocket_cellSailRec_VideoCleanup", "_pspocket_cellSailRec_VideoOpen", "_pspocket_cellSailRec_VideoStart",
    "_pspocket_cellSailRec_VideoStop", "_pspocket_cellSailRec_VideoCancel", "_pspocket_cellSailRec_VideoCheckout",
    "_pspocket_cellSailRec_VideoFormat", "_pspocket_cellSailRec_VideoGet", "_pspocket_cellSailRec_VideoRelease",
    "_pspocket_cellSailRec_VideoClose"};

// cellSailFeederAudio/VideoInitialize(feeder, funcs, arg, attribute): the callbacks at +0x1C, +0x20, +0x24 and +0x28 are
// required, the attribute must be NULL. The stream's side is cleared to 0xA0 (audio) or 0xA8 (video) bytes.
s32 feeder_init(PPUContext& ctx, u32 f, u32 funcs, u32 arg, u32 attr, bool video)
{
    if (!funcs || !r32(ctx, funcs + 0x20) || !r32(ctx, funcs + 0x24) || !r32(ctx, funcs + 0x28) ||
        !r32(ctx, funcs + 0x1C) || attr)
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memset(gb(ctx) + f, 0, fd::kSize);
    std::memcpy(gb(ctx) + f + (video ? fd::kVideoFuncs : fd::kAudioFuncs), gb(ctx) + funcs, 0x2C);
    w32(ctx, f + (video ? fd::kVideoArg : fd::kAudioArg), arg);
    std::memset(gb(ctx) + f, 0, video ? 0xA8 : 0xA0);
    for (u32 i = 0; i < 11; i++) w32(ctx, f + 4 * i, internal(video ? kVideoFeederCalls[i] : kAudioFeederCalls[i]));
    w32(ctx, f + 0x2C, f);
    return 0;
}

s32 cellSailFeederAudioInitialize(PPUContext& ctx, u32 f, u32 funcs, u32 arg, u32 attr)
{
    return feeder_init(ctx, f, funcs, arg, attr, false);
}

s32 cellSailFeederVideoInitialize(PPUContext& ctx, u32 f, u32 funcs, u32 arg, u32 attr)
{
    return feeder_init(ctx, f, funcs, arg, attr, true);
}

// Finalize: the stream's state cleared, then the whole feeder.
s32 feeder_fini(PPUContext& ctx, u32 f)
{
    w32(ctx, f + fd::kState, 0);
    std::memset(gb(ctx) + f, 0, fd::kSize);
    return 0;
}

s32 cellSailFeederAudioFinalize(PPUContext& ctx, u32 f) { return feeder_fini(ctx, f); }
s32 cellSailFeederVideoFinalize(PPUContext& ctx, u32 f) { return feeder_fini(ctx, f); }

// A feeder's notification: the diagnostic handler (kind, value sign-extended, flag) when the feeder holds one, else the
// message; what r3 holds afterwards (the handler's answer, or the queue's unlock).
u64 feeder_notify(PPUContext& ctx, std::string_view what, u32 f, bool video, u32 kind, u32 code, s32 value, u32 flag,
    u32 message_arg)
{
    const u32 diag = r32(ctx, f + (video ? fd::kVideoDiag : fd::kAudioDiag));
    if (diag)
        return call_guest(ctx, diag, {r32(ctx, f + (video ? fd::kVideoDiagArg : fd::kAudioDiagArg)), kind,
                                         static_cast<u64>(static_cast<s64>(value)), flag});
    return static_cast<u64>(static_cast<s64>(post_from(ctx, what, r32(ctx, f + fd::kRecorder), code, f, message_arg)));
}

u64 cellSailFeederAudioNotifyCallCompleted(PPUContext& ctx, u32 f, s32 result)
{
    return feeder_notify(ctx, "cellSailFeederAudioNotifyCallCompleted", f, false, 0, 0xF, result, 0,
        static_cast<u32>(result));
}

u64 cellSailFeederAudioNotifyFrameOut(PPUContext& ctx, u32 f, u32 n)
{
    return feeder_notify(ctx, "cellSailFeederAudioNotifyFrameOut", f, false, 2, 0x10, static_cast<s32>(n), 0, n);
}

u64 cellSailFeederAudioNotifySessionEnd(PPUContext& ctx, u32 f)
{
    return feeder_notify(ctx, "cellSailFeederAudioNotifySessionEnd", f, false, 1, 0x11, 0, 0, 0);
}

u64 cellSailFeederAudioNotifySessionError(PPUContext& ctx, u32 f, s32 err, u32 terminated)
{
    return feeder_notify(ctx, "cellSailFeederAudioNotifySessionError", f, false, 3, 0x12, err, terminated & 0xFF,
        static_cast<u32>(err) & ~(terminated << 31));
}

u64 cellSailFeederVideoNotifyCallCompleted(PPUContext& ctx, u32 f, s32 result)
{
    return feeder_notify(ctx, "cellSailFeederVideoNotifyCallCompleted", f, true, 0, 0x19, result, 0,
        static_cast<u32>(result));
}

u64 cellSailFeederVideoNotifyFrameOut(PPUContext& ctx, u32 f, u32 n)
{
    return feeder_notify(ctx, "cellSailFeederVideoNotifyFrameOut", f, true, 2, 0x1A, static_cast<s32>(n), 0, n);
}

u64 cellSailFeederVideoNotifySessionEnd(PPUContext& ctx, u32 f)
{
    return feeder_notify(ctx, "cellSailFeederVideoNotifySessionEnd", f, true, 1, 0x1B, 0, 0, 0);
}

u64 cellSailFeederVideoNotifySessionError(PPUContext& ctx, u32 f, s32 err, u32 terminated)
{
    return feeder_notify(ctx, "cellSailFeederVideoNotifySessionError", f, true, 3, 0x1C, err, terminated & 0xFF,
        static_cast<u32>(err) & ~(terminated << 31));
}

// ------------------------------------------------------------------------------------------------- the composer

// libsail_rec's lwmutex and lwcond pair at `at` (the lwcond at +0x18) named `name`; the lwmutex destroyed again when
// the lwcond cannot be made.
s32 make_lock(PPUContext& ctx, u32 at, const char* name)
{
    s32 r = sail::make_lwmutex(ctx, at, name);
    if (r < 0) return r;
    StackVar attr(ctx, gb(ctx), 16);
    std::memcpy(gb(ctx) + attr.addr(), name, 8);
    r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwcond_create", {at + 0x18, at, attr.addr()}));
    if (r < 0) sail::destroy_lwmutex(ctx, at);
    return r;
}

s32 destroy_lock(PPUContext& ctx, u32 at)
{
    sail::lv2_lib(ctx, "sys_lwcond_destroy", {at + 0x18});
    return sail::destroy_lwmutex(ctx, at);
}

// cellSailComposerInitialize(composer, funcs, arg): the game's callbacks (0x44 bytes; the last three words must be
// 0), the two locks, the state 1.
s32 cellSailComposerInitialize(PPUContext& ctx, u32 c, u32 funcs, u32 arg)
{
    if (!funcs || r32(ctx, funcs + 0x34) || r32(ctx, funcs + 0x38) || r32(ctx, funcs + 0x3C))
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memset(gb(ctx) + c, 0, cp::kSize);
    std::memcpy(gb(ctx) + c + cp::kFuncs, gb(ctx) + funcs, 0x44);
    s32 r = make_lock(ctx, c + cp::kLockA, "_saimx0");
    if (r < 0) return r;
    r = make_lock(ctx, c + cp::kLockB, "_saimx1");
    if (r < 0) {
        destroy_lock(ctx, c + cp::kLockA);
        return r;
    }
    w32(ctx, c + cp::kState, 1);
    w32(ctx, c + cp::kTable, composer_table(ctx));
    w32(ctx, c + cp::kArg, arg);
    return 0;
}

// cellSailComposerFinalize(composer): only in state 1 (no stream open).
s32 cellSailComposerFinalize(PPUContext& ctx, u32 c)
{
    if (r32(ctx, c + cp::kState) != 1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    destroy_lock(ctx, c + cp::kLockB);
    destroy_lock(ctx, c + cp::kLockA);
    std::memset(gb(ctx) + c, 0, cp::kSize);
    return 0;
}

// cellSailComposerGetStreamParameter(composer, param): 0 once the composer holds the profile's stream parameters,
// INVALID_STATE before; nothing is written (libsail_rec's function only checks).
s32 cellSailComposerGetStreamParameter(PPUContext& ctx, u64 c)
{
    if (c && r32(ctx, static_cast<u32>(c) + cp::kStreamParam)) return 0;
    return error(CELL_SAIL_ERROR_INVALID_STATE);
}

s32 cellSailComposerGetEsUserParameter(PPUContext&) { return 0; }

// cellSailComposerGetEsVideo/AudioParameter(composer, index, out, size): an open stream's parameters (0x50 video,
// 0x48 audio bytes, the size exactly that). The index is checked only against 3 (a negative one reads before the
// table, as libsail_rec does).
s32 es_parameter(PPUContext& ctx, u32 c, s32 index, u32 out, s32 size, u32 table, s32 want)
{
    if (!c || index > 3) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 stream = r32(ctx, c + static_cast<u32>(index * 4 + static_cast<s32>(table) - 8) + 8);
    if (!stream) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 st = r32(ctx, stream + 8);
    if (!st || r32(ctx, st + 8) - 10 > 3) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (!out || size != want) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + out, gb(ctx) + st + 0x18, static_cast<u32>(want));
    return 0;
}

s32 cellSailComposerGetEsVideoParameter(PPUContext& ctx, u32 c, s32 index, u32 out, s32 size)
{
    return es_parameter(ctx, c, index, out, size, cp::kVideo, 0x50);
}

s32 cellSailComposerGetEsAudioParameter(PPUContext& ctx, u32 c, s32 index, u32 out, s32 size)
{
    return es_parameter(ctx, c, index, out, size, cp::kAudio, 0x48);
}

// cellSailComposerNotifyCallCompleted(composer, result): to the composer's diagnostic handler (0, the result
// sign-extended, 0) when it holds one, else the pending call's completion (once); r3 is then the handler's answer, the
// queue's unlock, or the composer as given (none pending).
u64 cellSailComposerNotifyCallCompleted(PPUContext& ctx, u64 composer, s32 result)
{
    return composer_notify_completed(ctx, composer, result);
}

s32 cellSailComposerNotifySessionError(PPUContext& ctx, u32 c, s32 err, u32 terminated)
{
    return post_from(ctx, "cellSailComposerNotifySessionError", r32(ctx, c + cp::kRecorder), 0x2E, c,
        static_cast<u32>(err) & ~(terminated << 31));
}

// A stream's state (the composer's stream table entry's +8): +0x68 its timescale, +0x6C its ring of access units,
// +0x70 the ring's size, +0x74 the units in it, +0x7C where the next is read.
namespace st {
constexpr u32 kTimescale = 0x68, kRing = 0x6C, kRingSize = 0x70, kCount = 0x74, kRead = 0x7C;
}

// cellSailComposerReleaseEsUser/Video/AudioAu(composer, index, n): at most the units there given back (the read index
// moved on) and a message to the recorder (user 0x2D with n, video 0x2C and audio 0x2B with the number given back);
// the number given back.
s32 release_au(PPUContext& ctx, u32 c, u32 index, s32 n, u32 count, u32 table, u32 code, bool pass_n)
{
    if (index >= r32(ctx, c + count) || n < 0) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 stream = r32(ctx, c + table + index * 4), s = r32(ctx, stream + 8);
    s32 r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_lock", {c + cp::kLockA, 0}));
    if (r < 0) return r;
    const s32 there = static_cast<s32>(r32(ctx, s + st::kCount));
    const s32 k = there < n ? there : n;
    const u32 size = r32(ctx, s + st::kRingSize), next = r32(ctx, s + st::kRead) + static_cast<u32>(k);
    w32(ctx, s + st::kCount, static_cast<u32>(there - k));
    w32(ctx, s + st::kRead, size ? next % size : 0);
    if (k > 0) post(ctx, r32(ctx, c + cp::kRecorder), code, stream, static_cast<u32>(pass_n ? n : k));
    sail::lv2_lib(ctx, "sys_lwmutex_unlock", {c + cp::kLockA});
    return k;
}

s32 cellSailComposerReleaseEsUserAu(PPUContext& ctx, u32 c, u32 index, s32 n)
{
    return release_au(ctx, c, index, n, cp::kUserCount, cp::kUser, 0x2D, true);
}

s32 cellSailComposerReleaseEsVideoAu(PPUContext& ctx, u32 c, u32 index, s32 n)
{
    return release_au(ctx, c, index, n, cp::kVideoCount, cp::kVideo, 0x2C, false);
}

s32 cellSailComposerReleaseEsAudioAu(PPUContext& ctx, u32 c, u32 index, s32 n)
{
    return release_au(ctx, c, index, n, cp::kAudioCount, cp::kAudio, 0x2B, false);
}

// A time of the stream's timescale in 90 kHz units, as libsail_rec computes it (the product wraps in 64 bits, the
// quotient is signed).
u64 to_90k(u64 t, u32 timescale)
{
    if (!timescale) return 0; // a division by zero (libsail_rec never sets a timescale of 0)
    return static_cast<u64>(static_cast<s64>(t * 90000) / static_cast<s64>(u64{timescale}));
}

// The Get/TryGet functions: `wait` (Get) waits while the stream has none with the timeout (TryGet: no wait).
// libsail_rec's user stream copies the game's first entry into the rest (its units are never read); video units are
// 0x38 bytes (four times in 90 kHz and the unit's address +0x34), audio 0x20 (one time in 90 kHz). The number taken
// is n whenever any are there (more than there may be); *remaining gets the units there less those.
s32 get_au(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining, bool wait, u64 timeout, int kind)
{
    const u32 count = kind == 0 ? cp::kUserCount : kind == 1 ? cp::kVideoCount : cp::kAudioCount;
    const u32 table = kind == 0 ? cp::kUser : kind == 1 ? cp::kVideo : cp::kAudio;
    if (index >= r32(ctx, c + count) || n < 0 || (n != 0 && !out)) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 s = r32(ctx, r32(ctx, c + static_cast<u32>(static_cast<s32>(index * 4 + table) - 8) + 8) + 8);
    const u32 lk = kind == 2 ? cp::kLockA : cp::kLockB, cond = kind == 2 ? cp::kCondA : cp::kCondB;
    s32 r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_lock", {c + lk, 0}));
    if (r < 0) return r;
    s32 there = 0, taken = 0;
    for (;;) {
        there = static_cast<s32>(r32(ctx, s + st::kCount));
        if (!wait || n == 0 || there > 0) break;
        r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwcond_wait", {c + cond, timeout}));
        if (r < 0) {
            sail::lv2_lib(ctx, "sys_lwmutex_unlock", {c + lk});
            return kind == 2 ? r : 0;
        }
    }
    if (n > 0 && there > 0) {
        u32 rd = r32(ctx, s + st::kRead);
        const u32 ts = r32(ctx, s + st::kTimescale), size = r32(ctx, s + st::kRingSize);
        for (s32 i = 0; i < n; i++) {
            if (kind == 0) {
                const u32 o = out + 0x18 * static_cast<u32>(i);
                const u32 w0 = r32(ctx, out), w1 = r32(ctx, out + 4);
                const u64 d = r64(ctx, out + 8);
                w32(ctx, o, w0);
                w64(ctx, o + 8, d);
                w32(ctx, o + 4, w1);
                continue;
            }
            const u32 au = r32(ctx, r32(ctx, s + st::kRing) + 4 * rd);
            rd = size ? (rd + 1) % size : 0;
            if (kind == 1) {
                const u32 o = out + 0x38 * static_cast<u32>(i);
                w32(ctx, o + 0x34, au + 0x34);
                w32(ctx, o + 4, r32(ctx, au + 4));
                w32(ctx, o + 0x30, r32(ctx, au + 0x30));
                w32(ctx, o, r32(ctx, au));
                for (u32 k = 0; k < 4; k++) w64(ctx, o + 8 + 8 * k, to_90k(r64(ctx, au + 8 + 8 * k), ts));
                w64(ctx, o + 0x28, r64(ctx, au + 0x28));
            } else {
                const u32 o = out + 0x20 * static_cast<u32>(i);
                w32(ctx, o + 4, r32(ctx, au + 4));
                w32(ctx, o + 0x18, r32(ctx, au + 0x18));
                w32(ctx, o, r32(ctx, au));
                w64(ctx, o + 8, to_90k(r64(ctx, au + 8), ts));
                w64(ctx, o + 0x10, r64(ctx, au + 0x10));
                w32(ctx, o + 0x1C, r32(ctx, au + 0x1C));
            }
        }
        taken = n;
    }
    if (remaining) w32(ctx, remaining, static_cast<u32>(there - taken));
    sail::lv2_lib(ctx, "sys_lwmutex_unlock", {c + lk});
    return taken;
}

s32 cellSailComposerTryGetEsUserAu(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining)
{
    return get_au(ctx, c, index, out, n, remaining, false, 0, 0);
}

s32 cellSailComposerGetEsUserAu(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining, u64 timeout)
{
    return get_au(ctx, c, index, out, n, remaining, true, timeout, 0);
}

s32 cellSailComposerTryGetEsVideoAu(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining)
{
    return get_au(ctx, c, index, out, n, remaining, false, 0, 1);
}

s32 cellSailComposerGetEsVideoAu(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining, u64 timeout)
{
    return get_au(ctx, c, index, out, n, remaining, true, timeout, 1);
}

s32 cellSailComposerTryGetEsAudioAu(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining)
{
    return get_au(ctx, c, index, out, n, remaining, false, 0, 2);
}

s32 cellSailComposerGetEsAudioAu(PPUContext& ctx, u32 c, u32 index, u32 out, s32 n, u32 remaining, u64 timeout)
{
    return get_au(ctx, c, index, out, n, remaining, true, timeout, 2);
}

// ------------------------------------------------------------------------------------------------- profiles

// The video encoders' option IDs a profile may carry (the MPEG-4 encoder's four and the AVC encoder's seventeen, as
// libsail_rec accepts them).
constexpr u32 kM4vOptions[] = {0x7ED4, 0xFA01, 0xFA02, 0xFA03};
constexpr u32 kAvcOptions[] = {0x3E83, 0xA781, 0xA771, 0xA780, 0xA770, 0x7A36, 0x3947, 0x1E00, 0x1E01, 0x1E02,
    0x410F, 0x281, 0x412, 0x1285, 0x8592, 0x154E, 0x3D94};

// cellSailProfileSetStreamParameter(profile, size param {8, buffer size}, stream param): the buffer size (all ones:
// 1 MB; less than 1 MB refused); the stream parameters by the composer type (MP4: 0x38 bytes with a flag of 0, 1 or all
// ones; the game's: 0xC bytes), NULL for the defaults.
s32 cellSailProfileSetStreamParameter(PPUContext& ctx, u32 p, u32 size_param, u32 stream_param)
{
    if (r32(ctx, p + pf::kState) == 2) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (size_param) {
        if (r32(ctx, size_param) != 8) return error(CELL_SAIL_ERROR_INVALID_ARG);
        u32 v = r32(ctx, size_param + 4);
        if (v == 0xFFFFFFFF) v = 0x100000;
        else if (v < 0x100000) return error(CELL_SAIL_ERROR_INVALID_ARG);
        w32(ctx, p + pf::kBuffer, v);
        w32(ctx, p + pf::kBuffer2, 0);
    }
    if (!stream_param) {
        w32(ctx, p + pf::kDefaults, 1);
        return 0;
    }
    const u32 type = r32(ctx, p + pf::kType);
    if (type == 1) {
        const u32 flag = r32(ctx, stream_param + 4);
        if (r32(ctx, stream_param) == 0x38 && (flag == 0xFFFFFFFF || (flag & 0xFFFFFFFE) == 0)) {
            std::memmove(gb(ctx) + p + pf::kStream, gb(ctx) + stream_param, 0x38);
            w32(ctx, p + pf::kDefaults, 0);
            return 0;
        }
    } else if (type == 3 && r32(ctx, stream_param) == 0xC) {
        std::memmove(gb(ctx) + p + pf::kStream, gb(ctx) + stream_param, 0xC);
        w32(ctx, p + pf::kDefaults, 0);
        return 0;
    }
    return error(CELL_SAIL_ERROR_INVALID_ARG);
}

// cellSailProfileSetEsAudioParameter(profile, index, param (0x48 bytes), extension (0x84 bytes, or NULL)): NULL for the
// defaults (the entry's parameters all ones). Fields the game may not choose must be all ones; the codec all ones (the
// default) or 1 (AAC: at most 2 channels, a channel layout 1 only for mono, a sample rate of 22.05, 24, 32, 44.1 or 48
// kHz or the default, two equal bit rates within half to six times the rate); the extension's encoder settings must be
// the defaults (it is copied into the entry first, so a refused one stays there).
s32 cellSailProfileSetEsAudioParameter(PPUContext& ctx, u32 p, u32 index, u32 param, u32 ext)
{
    if (r32(ctx, p + pf::kState) == 2) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (r32(ctx, r32(ctx, p + pf::kRecorder) + rc::kAudioCount) <= index) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    const u32 e = index * 0x108 + r32(ctx, p + pf::kAudio);
    if (!param) {
        std::memset(gb(ctx) + e + 0x10, 0xFF, 0xF8);
        return 0;
    }
    const auto bad = [] { return error(CELL_SAIL_ERROR_INVALID_ARG); };
    const auto unsupported = [] { return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM); };
    if (r32(ctx, param) != 0x48) return bad();
    if (ext && r32(ctx, ext) != 0x84) return bad();
    for (const u32 off : {8u, 0xCu, 0x10u, 0x20u, 0x24u, 0x28u})
        if (r32(ctx, param + off) != 0xFFFFFFFF) return bad();
    if (r8(ctx, param + 0x30) != 0xFF || static_cast<s16>(r16(ctx, param + 0x32)) != -1 ||
        r32(ctx, param + 0x3C) != 0xFFFFFFFF || r64(ctx, param + 0x40) != ~u64{0})
        return bad();
    const u32 codec = r32(ctx, param + 4);
    const s64 rate_a = static_cast<s32>(r32(ctx, param + 0x14)), rate_b = static_cast<s32>(r32(ctx, param + 0x18));
    const u32 layout = r32(ctx, param + 0x38);
    const s8 channels = static_cast<s8>(r8(ctx, param + 0x31));
    const s32 rate = static_cast<s32>(r32(ctx, param + 0x34));
    const auto store = [&] {
        w32(ctx, e + 0x14, codec);
        w32(ctx, e + 0x28, static_cast<u32>(rate_a));
        w32(ctx, e + 0x2C, static_cast<u32>(rate_b));
        std::memmove(gb(ctx) + e + 0x38, gb(ctx) + param + 0x30, 0x18);
        w32(ctx, e, 2);
        return 0;
    };
    if (codec == 0xFFFFFFFF) return store();
    if (codec != 1) return unsupported();
    if (ext) {
        const u32 x = e + 0x88;
        std::memmove(gb(ctx) + x, gb(ctx) + ext + 4, 0x80);
        for (const u32 off : {0u, 4u, 8u, 0x44u})
            if (r32(ctx, x + off)) return bad();
        const float f = std::bit_cast<float>(r32(ctx, x + 0xC));
        if (f != 0.0f) return bad();
        if (r32(ctx, x + 0x10) || static_cast<s32>(r32(ctx, x + 0x14)) > 1 || r32(ctx, x + 0x18) ||
            static_cast<s32>(r32(ctx, x + 0x1C)) > 1)
            return bad();
        for (const u32 off : {0x20u, 0x24u, 0x28u, 0x2Cu})
            if (r32(ctx, x + off)) return bad();
        if (r8(ctx, x + 0x30)) return bad();
        for (const u32 off : {0x3Cu, 0x40u, 0x48u, 0x4Cu, 0x50u, 0x54u})
            if (r32(ctx, x + off)) return bad();
        w32(ctx, e + 0x18, 0);
    } else {
        w32(ctx, e + 0x18, codec);
    }
    if (channels > 2) return unsupported();
    if (layout != 0xFFFFFFFF && !(channels == 1 && layout == 1)) return bad();
    const s64 r = rate;
    if (!(static_cast<u64>(r + 1) <= 1 || rate == 22050 || rate == 24000 || rate == 32000 || rate == 44100 ||
            rate == 48000))
        return unsupported();
    if (rate_a > 0 || rate_b > 0) {
        if (rate == -1) return bad();
        if (rate_a != rate_b) return unsupported();
        if ((rate_b << 1) < r) return unsupported();
        if (r * 6 < rate_b) return unsupported();
    }
    return store();
}

// cellSailProfileSetEsVideoParameter(profile, index, param (0x50 bytes), extension (MPEG-4 and AVC: {16, 0, options,
// count}, or NULL)): NULL for the defaults. Fields the game may not choose must be all ones; the frame rate code (byte
// 0x33) all ones or 0 to 7 (23.976 to 60 frames a second); the codec MPEG-4 (0x21), AVC (0x22, 0x23) or 0x24 (others
// UNSUPPORTED_STREAM); an extension's options (up to 32 {ID, value} pairs, each ID one the encoder knows) are kept.
s32 cellSailProfileSetEsVideoParameter(PPUContext& ctx, u32 p, u32 index, u32 param, u32 ext)
{
    if (r32(ctx, p + pf::kState) == 2) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (r32(ctx, r32(ctx, p + pf::kRecorder) + rc::kVideoCount) <= index)
        return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    const u32 e = index * 0x220 + r32(ctx, p + pf::kVideo);
    if (!param) {
        std::memset(gb(ctx) + e + 0x10, 0xFF, 0x210);
        return 0;
    }
    const auto bad = [] { return error(CELL_SAIL_ERROR_INVALID_ARG); };
    const auto unsupported = [] { return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM); };
    if (r32(ctx, param) != 0x50) return bad();
    for (const u32 off : {8u, 0x24u, 0x28u})
        if (r32(ctx, param + off) != 0xFFFFFFFF) return bad();
    for (const u32 off : {0x30u, 0x31u, 0x32u})
        if (r8(ctx, param + off) != 0xFF) return bad();
    for (const u32 off : {0x38u, 0x3Cu})
        if (r32(ctx, param + off) != 0xFFFFFFFF) return bad();
    for (const u32 off : {0x40u, 0x42u, 0x43u})
        if (r8(ctx, param + off) != 0xFF) return bad();
    if (r64(ctx, param + 0x48) != ~u64{0}) return bad();
    const u8 code = r8(ctx, param + 0x33);
    static constexpr u32 kScale[8][2] = {{1001, 24000}, {1, 24}, {1, 25}, {1001, 30000}, {1, 30}, {1, 50},
        {1001, 60000}, {1, 60}};
    u32 num = 0xFFFFFFFF, den = 0xFFFFFFFF;
    if (code != 0xFF) {
        if (code > 7) return unsupported();
        num = kScale[code][0];
        den = kScale[code][1];
    }
    const u32 codec = r32(ctx, param + 4);
    const auto store = [&] {
        w32(ctx, e + 0x14, codec);
        w32(ctx, e + 0x20, r32(ctx, param + 0xC));
        w32(ctx, e + 0x24, r32(ctx, param + 0x10));
        w32(ctx, e + 0x2C, r32(ctx, param + 0x14));
        w32(ctx, e + 0x30, r32(ctx, param + 0x18));
        w32(ctx, e + 0x38, r32(ctx, param + 0x20));
        const u32 ts = r32(ctx, param + 0x1C);
        w32(ctx, e + 0x34, ts);
        w32(ctx, e + 0xA0, den ? (num * ts) / den : 0);
        std::memmove(gb(ctx) + e + 0x40, gb(ctx) + param + 0x30, 0x20);
        w32(ctx, e, 2);
        return 0;
    };
    if (codec == 0xFFFFFFFF) return ext ? bad() : unsupported();
    if (codec == 0x24) return store();
    if (codec != 0x21 && codec != 0x22 && codec != 0x23) return unsupported();
    if (!ext) {
        w32(ctx, e + 0x18, 1);
        return store();
    }
    if (r32(ctx, ext) != 16 || r32(ctx, ext + 4)) return bad();
    std::memset(gb(ctx) + e + 0xA8, 0, 0x1C);
    const u32 options = r32(ctx, ext + 8);
    if (options) {
        const u32 n = r32(ctx, ext + 12);
        if (n > 32) return bad();
        const bool m4v = codec == 0x21;
        const u32* known = m4v ? kM4vOptions : kAvcOptions;
        const u32 known_n = m4v ? 4 : 17;
        for (u32 i = 0; i < n; i++) {
            const u32 id = r32(ctx, options + 8 * i);
            bool ok = false;
            for (u32 k = 0; k < known_n && !ok; k++) ok = known[k] == id;
            if (!ok) return bad();
        }
        const u32 kept = e + (m4v ? 0xE8 : 0xE4);
        std::memmove(gb(ctx) + kept, gb(ctx) + options, n * 8);
        w32(ctx, e + 0xA8 + 0x14, n);
        w32(ctx, e + 0xA8 + 0x10, kept);
    }
    w32(ctx, e + 0x18, 0);
    return store();
}

// ------------------------------------------------------------------------------------------------- video converters

// A video converter (made by the control thread): +4 its kind, +0x30 its lwmutex, +0x48 and +0x50 its lwconds (room
// for a request, a result), +0x58 1 while a request is being converted, +0x5C its capacity, +0x60 the requests queued,
// +0x64 the results ready, +0x68, +0x6C, +0x70, +0x74 where the next request goes, which runs next, where its result
// goes, which result is read next; requests of 0x18 bytes from +0x78, results of 8 from +0xA8.
s32 conv_lock(PPUContext& ctx, u32 v) { return static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_lock", {v + cv::kLock, 0})); }
void conv_unlock(PPUContext& ctx, u32 v) { sail::lv2_lib(ctx, "sys_lwmutex_unlock", {v + cv::kLock}); }

s32 cellSailVideoConverterCanGetResult(PPUContext& ctx, u32 v)
{
    s32 r = conv_lock(ctx, v);
    if (r < 0) return r;
    r = static_cast<s32>(r32(ctx, v + cv::kResults));
    conv_unlock(ctx, v);
    return r;
}

s32 cellSailVideoConverterCanProcess(PPUContext& ctx, u32 v)
{
    s32 r = conv_lock(ctx, v);
    if (r < 0) return r;
    const u32 queued = r32(ctx, v + cv::kQueued);
    conv_unlock(ctx, v);
    return static_cast<s32>(r32(ctx, v + cv::kCapacity) - queued);
}

// cellSailVideoConverterGetResult(converter, timeout, out a, out b): the next result; with none, EMPTY at once (a
// timeout of 0), or after a wait for one (still EMPTY when woken: libsail_rec does not look again), or the wait's
// error.
s32 cellSailVideoConverterGetResult(PPUContext& ctx, u32 v, u64 timeout, u32 out_a, u32 out_b)
{
    s32 r = conv_lock(ctx, v);
    if (r < 0) return r;
    const u32 results = r32(ctx, v + cv::kResults);
    r = 0;
    if (!results) {
        if (timeout) r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwcond_wait", {v + cv::kReady, timeout}));
        if (r >= 0) r = error(CELL_SAIL_ERROR_EMPTY);
    } else {
        const u32 i = r32(ctx, v + cv::kRead), at = v + cv::kResultTable + 8 * i;
        w32(ctx, out_a, r32(ctx, at + 4));
        w32(ctx, out_b, r32(ctx, at));
        const u32 cap = r32(ctx, v + cv::kCapacity);
        w32(ctx, v + cv::kResults, results - 1);
        w32(ctx, v + cv::kRead, cap ? (i + 1) % cap : 0);
        converter_start_next(ctx, v);
    }
    conv_unlock(ctx, v);
    return r;
}

// cellSailVideoConverterProcess(converter, a, b, c, d, timeout, tag): a request queued (FULLED at once with no room and
// no timeout, else after one wait for room; the number queued is read once, before the wait, as libsail_rec reads it)
// and the next one started.
s32 cellSailVideoConverterProcess(PPUContext& ctx, u32 v, u32 a, u32 b, u32 c, u32 d, u64 timeout, u32 tag)
{
    s32 r = conv_lock(ctx, v);
    if (r < 0) return r;
    const u32 queued = r32(ctx, v + cv::kQueued);
    for (;;) {
        if (queued < r32(ctx, v + cv::kCapacity)) {
            const u32 w = r32(ctx, v + cv::kWrite), at = v + cv::kRequests + 0x18 * w;
            w32(ctx, at + 0x14, 0);
            w32(ctx, at + 4, b);
            w32(ctx, at + 0x10, tag);
            const u32 cap = r32(ctx, v + cv::kCapacity);
            w32(ctx, at, a);
            w32(ctx, at + 8, c);
            w32(ctx, at + 12, d);
            w32(ctx, v + cv::kQueued, queued + 1);
            w32(ctx, v + cv::kWrite, cap ? (w + 1) % cap : 0);
            converter_start_next(ctx, v);
            r = 0;
            break;
        }
        if (!timeout) {
            r = error(CELL_SAIL_ERROR_FULLED);
            break;
        }
        r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwcond_wait", {v + cv::kRoom, timeout}));
        timeout = 0;
        if (r < 0) break;
    }
    conv_unlock(ctx, v);
    return r;
}

} // namespace

void register_objects(Registry& r)
{
    r.add(kChan, {
        PS_HLE_FUNC(0xE15F38D9, cellSailComposerFinalize),
        PS_HLE_FUNC(0x57390C45, cellSailComposerGetEsAudioAu),
        PS_HLE_FUNC(0xD0EBD14F, cellSailComposerGetEsAudioParameter),
        PS_HLE_FUNC(0x62CCC64A, cellSailComposerGetEsUserAu),
        PS_HLE_FUNC(0x2F7F391F, cellSailComposerGetEsUserParameter),
        PS_HLE_FUNC(0xC3B8F109, cellSailComposerGetEsVideoAu),
        PS_HLE_FUNC(0xB1991509, cellSailComposerGetEsVideoParameter),
        PS_HLE_FUNC(0x9A71E740, cellSailComposerGetStreamParameter),
        PS_HLE_FUNC(0x434C5123, cellSailComposerInitialize),
        PS_HLE_FUNC(0x3C09716B, cellSailComposerNotifyCallCompleted),
        PS_HLE_FUNC(0xF1F0E5DA, cellSailComposerNotifySessionError),
        PS_HLE_FUNC(0x2707FD60, cellSailComposerReleaseEsAudioAu),
        PS_HLE_FUNC(0x25B980A0, cellSailComposerReleaseEsUserAu),
        PS_HLE_FUNC(0x6F867071, cellSailComposerReleaseEsVideoAu),
        PS_HLE_FUNC(0xB9562C1A, cellSailComposerTryGetEsAudioAu),
        PS_HLE_FUNC(0x4C1D59E8, cellSailComposerTryGetEsUserAu),
        PS_HLE_FUNC(0xDFD899B7, cellSailComposerTryGetEsVideoAu),
        PS_HLE_FUNC(0x899D1587, cellSailFeederAudioFinalize),
        PS_HLE_FUNC(0xBD591197, cellSailFeederAudioInitialize),
        PS_HLE_FUNC(0xC2E2F30D, cellSailFeederAudioNotifyCallCompleted),
        PS_HLE_FUNC(0x3C775CEA, cellSailFeederAudioNotifyFrameOut),
        PS_HLE_FUNC(0x999C0DC5, cellSailFeederAudioNotifySessionEnd),
        PS_HLE_FUNC(0xAF310AE6, cellSailFeederAudioNotifySessionError),
        PS_HLE_FUNC(0x81BFEAE8, cellSailFeederVideoFinalize),
        PS_HLE_FUNC(0x57415DD3, cellSailFeederVideoInitialize),
        PS_HLE_FUNC(0xD84DAEB9, cellSailFeederVideoNotifyCallCompleted),
        PS_HLE_FUNC(0xE5E0572A, cellSailFeederVideoNotifyFrameOut),
        PS_HLE_FUNC(0xBFF6E8D3, cellSailFeederVideoNotifySessionEnd),
        PS_HLE_FUNC(0x86CAE679, cellSailFeederVideoNotifySessionError),
        PS_HLE_FUNC(0xE14CAE97, cellSailProfileSetEsAudioParameter),
        PS_HLE_FUNC(0x1422A425, cellSailProfileSetEsVideoParameter),
        PS_HLE_FUNC(0xE8D86C43, cellSailProfileSetStreamParameter),
        PS_HLE_FUNC(0xE16DE678, cellSailVideoConverterCanGetResult),
        PS_HLE_FUNC(0xB3D30B0D, cellSailVideoConverterCanProcess),
        PS_HLE_FUNC(0xE15679FE, cellSailVideoConverterGetResult),
        PS_HLE_FUNC(0x855DA8C6, cellSailVideoConverterProcess),
    });
}

} // namespace pspocket::hle::sailrec
