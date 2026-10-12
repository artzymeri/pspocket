// cellSail's sound and graphics adapters (D-562 part 4, D-919): libsail's own renderers that the game reads frames from
// (GetFrame) instead of writing its own renderer, and the user streams' AU receivers, as libsail (fw 4.92) has them:
// - Sound adapter (0xD8 bytes): a CellSailRendererAudio (its callbacks libsail's own) with +0x50 the game's 3 callbacks
//   and +0x5C their argument, +0x60 an lwmutex "_saisa0", +0x78 its state (7 initialized, 6 finalized, 0 closed, 1
//   open), +0x90 the format it delivers, +0xA8 its frames, +0xAC a silent buffer, +0xB0 the frames' buffers, +0xB4
//   their records, +0xC0 its AV-sync record, +0xD0 the first PTS (-1 none).
// - Graphics adapter (0x100 bytes): a CellSailRendererVideo with +0x60 the game's 5 callbacks and +0x74 their argument,
//   +0x78 an lwmutex "_saiga0", +0x90 its state, +0x98 the first PTS, +0xA8 the format it delivers, +0xC8 a frame's
//   duration, +0xCC its pictures (the game's), +0xD0 their records, +0xDC its AV-sync record.
// UpdateAvSync feeds the player's clock: the audio and video halves measure the time per frame from the game's (frame,
// time) reports, smoothed by a quarter. The adapters' open and close are here; their start, stop and frames, and the
// AU receiver's, are a later part and fail loudly until then.

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace sail;

namespace sa {
constexpr u32 kFuncs = 0x50, kArg = 0x5C, kMutex = 0x60, kState = 0x78, kSession = 0x7C, kWord = 0x80, kFormat = 0x90,
              kCount = 0xA8, kSilence = 0xAC, kBuffers = 0xB0, kRecords = 0xB4, kFrameBytes = 0xB8, kRead = 0xC4,
              kWrite = 0xC8, kGet = 0xCC, kSync = 0xC0, kFirstPts = 0xD0, kSize = 0xD8, kPreferred = 0x38;
}
namespace ga {
constexpr u32 kFuncs = 0x60, kArg = 0x74, kMutex = 0x78, kState = 0x90, kSession = 0x94, kFirstPts = 0x98,
              kFormat = 0xA8, kDuration = 0xC8, kCount = 0xCC, kDelay = 0xCE, kRecords = 0xD0, kSync = 0xDC,
              kFree = 0xE0, kSize = 0x100, kPreferred = 0x40;
}
constexpr u32 kInitialized = 7, kFinalized = 6;

// ------------------------------------------------------------------------------------------------- renderer callbacks

// The adapters' and the AU receiver's makeup and cleanup: the game's own callback (its funcs, with its argument) when it
// gave one; makeup also clears the adapter's play state. Cancel: nothing for the adapters, the game's for the receiver.
s32 call_game(PPUContext& ctx, u32 fn_at, u32 arg_at)
{
    const u32 f = r32(ctx, fn_at);
    return f ? static_cast<s32>(call_guest(ctx, f, {r32(ctx, arg_at)})) : 0;
}

s32 _pspocket_cellSail_saMakeup(PPUContext& ctx, u32 a)
{
    w32(ctx, a + sa::kState, 0);
    return call_game(ctx, a + sa::kFuncs, a + sa::kArg);
}
s32 _pspocket_cellSail_saCleanup(PPUContext& ctx, u32 a) { return call_game(ctx, a + sa::kFuncs + 4, a + sa::kArg); }
void _pspocket_cellSail_saCancel(PPUContext&) {}
s32 _pspocket_cellSail_gaMakeup(PPUContext& ctx, u32 a)
{
    w32(ctx, a + ga::kState, 0);
    return call_game(ctx, a + ga::kFuncs, a + ga::kArg);
}
s32 _pspocket_cellSail_gaCleanup(PPUContext& ctx, u32 a) { return call_game(ctx, a + ga::kFuncs + 4, a + ga::kArg); }
void _pspocket_cellSail_gaCancel(PPUContext&) {}
s32 _pspocket_cellSail_auMakeup(PPUContext& ctx, u32 a) { return call_game(ctx, a + 0xC4, a + 0xE8); }
s32 _pspocket_cellSail_auCleanup(PPUContext& ctx, u32 a) { return call_game(ctx, a + 0xC8, a + 0xE8); }
void _pspocket_cellSail_auCancel(PPUContext& ctx, u32 a) { call_game(ctx, a + 0xDC, a + 0xE8); }

u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

u32 adapter_player(PPUContext& ctx, u32 a) { return r32(ctx, a + 0x14); }

void audio_done(PPUContext& ctx, u32 a, s32 result) { post(ctx, adapter_player(ctx, a), 0x33, a, static_cast<u32>(result)); }
void video_done(PPUContext& ctx, u32 a, s32 result) { post(ctx, adapter_player(ctx, a), 0x34, a, static_cast<u32>(result)); }

// The sound adapter's open(format, frames): the frame buffers, their records and a silent buffer in one block from the
// player's allocator (the silence after them before SDK 4.0, first since), the game told of the format (the session's
// low 10 bits after it); answers the frames, or MEMORY.
void _pspocket_cellSail_saOpen(PPUContext& ctx, u32 a, u32 format, u32 n)
{
    s32 result = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + sa::kMutex, 0}));
    if (result >= 0) {
        const s32 ch = static_cast<s8>(r8(ctx, format + 1));
        const u32 frame = static_cast<u32>(static_cast<s16>(ps_read16(gb(ctx), format + 2)) * ch * 4);
        std::memmove(gb(ctx) + a + sa::kFormat, gb(ctx) + format, 0x18);
        w32(ctx, a + sa::kCount, n);
        w32(ctx, a + sa::kFrameBytes, frame);
        const u32 size = (n * frame + 0x7F) & ~0x7Fu;
        const u32 mem = player_alloc(ctx, adapter_player(ctx, a), 0x80,
            size + static_cast<u32>(ch) * 0x2000 + n * 0x20 - n * 4);
        result = error(CELL_SAIL_ERROR_MEMORY);
        if (mem) {
            u32 ptrs, recs, silence;
            if (sdk_version(ctx) < 0x400000) {
                ptrs = mem + size;
                for (u32 i = 0; i < n; i++) w32(ctx, ptrs + 4 * i, mem + i * frame);
                recs = ptrs + 4 * n;
                silence = recs + 0x18 * n;
            } else {
                const u32 base = mem + static_cast<u32>(ch) * 0x2000;
                ptrs = base + size;
                for (u32 i = 0; i < n; i++) w32(ctx, ptrs + 4 * i, base + i * frame);
                recs = ptrs + 4 * n;
                silence = mem;
            }
            w32(ctx, a + sa::kSilence, silence);
            w32(ctx, a + sa::kState, 1);
            w32(ctx, a + sa::kWord, 0);
            w32(ctx, a + sa::kWrite, 0);
            w32(ctx, a + sa::kRead, 0);
            w32(ctx, a + sa::kGet, 0);
            w32(ctx, a + sa::kSession, 0);
            w32(ctx, a + sa::kBuffers, ptrs);
            w32(ctx, a + sa::kRecords, recs);
            StackVar copy(ctx, gb(ctx), 0x20);
            std::memmove(gb(ctx) + copy.addr(), gb(ctx) + format, 0x18);
            call_guest(ctx, r32(ctx, a + sa::kFuncs + 8),
                {r32(ctx, a + sa::kArg), copy.addr(), (r32(ctx, a + sa::kSession) + 1) & 0x3FF});
            result = static_cast<s32>(n);
        }
        lv2_lib(ctx, "sys_lwmutex_unlock", {a + sa::kMutex});
    }
    audio_done(ctx, a, result);
}

// Close: the block freed, closed (0); answers 0.
void _pspocket_cellSail_saClose(PPUContext& ctx, u32 a)
{
    if (static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + sa::kMutex, 0})) >= 0) {
        const u32 ptrs = r32(ctx, a + sa::kBuffers);
        if (ptrs && r32(ctx, a + sa::kCount) && r32(ctx, a + sa::kSilence)) {
            const u32 block = sdk_version(ctx) < 0x400000 ? r32(ctx, ptrs) : r32(ctx, a + sa::kSilence);
            player_free(ctx, adapter_player(ctx, a), 0x80, block);
            w32(ctx, a + sa::kBuffers, 0);
        }
        w32(ctx, a + sa::kState, 0);
        lv2_lib(ctx, "sys_lwmutex_unlock", {a + sa::kMutex});
    }
    audio_done(ctx, a, 0);
}

// libsail's division: the PPU's divdu answers 0 for a divisor of 0.
u64 divu(u64 a, u64 b) { return b != 0 ? a / b : 0; }

// The presentation start time of the player's current descriptor (libsail's, -1 without one): PAMF's from its reader,
// MP4's and AVI's 90000.
u64 first_pts(PPUContext& ctx, u32 r)
{
    const u32 d = r32(ctx, r32(ctx, r + 0x14) + pl::kCurrentDescriptor);
    if (!d) return ~u64{0};
    const s32 type = static_cast<s32>(r32(ctx, d + ds::kType));
    if (type == 0) {
        StackVar t(ctx, gb(ctx), 16);
        if (static_cast<s32>(module_call(ctx, "cellPamf", "cellPamfReaderGetPresentationStartTime",
                {d + ds::kReader, t.addr()})) < 0)
            return ~u64{0};
        return r64(ctx, t.addr());
    }
    if (type < 0 || type > 2) return 0xFFFFFFFF80610703ull;
    return 90000;
}

// The rate flag (0x20) from a session word's bits 16 and 20 (not the normal rate, or EP skipping).
void session_rate_flag(PPUContext& ctx, u32 r, u32 word)
{
    w32(ctx, r + 0x34, (r32(ctx, r + 0x34) & ~0x20u) | ((word & 0x110000) ? 0x20u : 0u));
}

// The sound adapter's renderer callbacks while playing (libsail's): start (from open, the play state reset: frames,
// records and the first PTS; buffering (2) when the stream was only open, else playing (3)), stop (a flush gives every
// frame back, the play state reset, the clock told of the stream's end, the output's end (0x1D), open (1); else it
// drains (4) through GetFrame; open answers 1), checkout (the next frame's buffer in the next record to fill) and
// checkin (the record's session word without bit 19; a frame of status -1 or flagged (bit 19) given back at once (its
// buffer reused), else ready, buffering done when all are full). Each answers through NotifyCallCompleted (0x33).
void _pspocket_cellSail_saStart(PPUContext& ctx, u32 a, u32 flag)
{
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + sa::kMutex, 0}));
    if (r < 0) {
        audio_done(ctx, a, r);
        return;
    }
    if (r32(ctx, a + sa::kState) == 1) {
        w32(ctx, a + 0x88, 0);
        w64(ctx, a + sa::kFirstPts, ~u64{0});
        w32(ctx, a + sa::kSession, 0);
        w32(ctx, a + sa::kWord, 0);
        w32(ctx, a + 0xBC, 0);
        w32(ctx, a + sa::kWrite, 0);
        w32(ctx, a + sa::kRead, 0);
        w32(ctx, a + sa::kGet, 0);
    }
    w32(ctx, a + sa::kState, flag != 0 ? 2 : 3);
    lv2_lib(ctx, "sys_lwmutex_unlock", {a + sa::kMutex});
    audio_done(ctx, a, 0);
}

void _pspocket_cellSail_saStop(PPUContext& ctx, u32 a, u32 flush)
{
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + sa::kMutex, 0}));
    if (r < 0) {
        audio_done(ctx, a, r);
        return;
    }
    const s32 st = static_cast<s32>(r32(ctx, a + sa::kState));
    const u32 p = adapter_player(ctx, a);
    if (st == 1) {
        audio_done(ctx, a, 1);
    } else if (st > 1 && st <= 4) {
        if (flush == 0) {
            w32(ctx, a + sa::kState, 4);
        } else {
            for (u32 n = r32(ctx, a + 0xBC); n; n--) post(ctx, p, 0x12, a, 0);
            w32(ctx, a + 0x88, 0);
            w64(ctx, a + sa::kFirstPts, ~u64{0});
            w32(ctx, a + sa::kState, 1);
            w32(ctx, a + 0xBC, 0);
            w32(ctx, a + sa::kGet, 0);
            w32(ctx, a + sa::kWrite, 0);
            w32(ctx, a + sa::kRead, 0);
            clock_audio_end(ctx, r32(ctx, a + 0x10), r32(ctx, a + 8), a + 0xC, r32(ctx, a + sa::kWord));
            post(ctx, p, 0x1D, a, 0);
            audio_done(ctx, a, 0);
        }
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {a + sa::kMutex});
}

s32 _pspocket_cellSail_saCheckout(PPUContext& ctx, u32 a, u32 out)
{
    const u32 n = r32(ctx, a + sa::kCount), buf_index = r32(ctx, a + sa::kRead);
    const u32 rec = r32(ctx, a + sa::kRecords) + r32(ctx, a + sa::kWrite) * 0x18;
    w32(ctx, out, rec);
    w32(ctx, rec, r32(ctx, r32(ctx, a + sa::kBuffers) + 4 * buf_index));
    w32(ctx, a + sa::kRead, n ? (buf_index + 1) % n : 0);
    return 0;
}

s32 _pspocket_cellSail_saCheckin(PPUContext& ctx, u32 a, u32 rec)
{
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + sa::kMutex, 0}));
    if (r < 0) return r;
    const u32 word = r32(ctx, rec + 0x14), st = r32(ctx, a + sa::kState), p = adapter_player(ctx, a);
    const u32 session = word & ~0x80000u;
    w64(ctx, rec + 0x10, session);
    if (st - 2 > 1) {
        post(ctx, p, 0x12, a, 0);
    } else {
        if (r32(ctx, a + sa::kWord) != session) w32(ctx, a + sa::kWord, session);
        const u32 n = r32(ctx, a + sa::kCount);
        if (r32(ctx, rec + 4) == 0xFFFFFFFF || (word & 0x80000)) {
            const u32 back = r32(ctx, a + sa::kRead) + n - 1;
            w32(ctx, a + sa::kRead, n ? back % n : 0);
            post(ctx, p, 0x12, a, 0);
        } else {
            const u32 next = r32(ctx, a + sa::kWrite) + 1, ready = r32(ctx, a + 0xBC) + 1;
            w32(ctx, a + 0xBC, ready);
            w32(ctx, a + sa::kWrite, n ? next % n : 0);
            if (st == 2 && n <= ready) w32(ctx, a + sa::kState, 3);
        }
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {a + sa::kMutex});
    return 0;
}

// The frame's duration in 90 kHz ticks by frame rate code (23.976, 24, 25, 29.97, 30, 50, 59.94, 60 Hz; others 30).
constexpr u16 kDuration[9] = {3753, 3750, 3600, 3003, 3000, 1800, 1501, 1500, 3000};

// The graphics adapter's frames (0x28-byte records: +0 the picture, +4 status, +8 PTS, +0x10 the session word, +0x18
// the duration, +0x1A the structure, +0x1B the fields, +0x20 the next record, +0x24 the sequence it is given back at,
// +0x27 its index) go from the free list (+0xE0) to the renderer (checkout), back filled (checkin) to the ready list
// (+0xE8 head, +0xE4 tail; +0xD8 counted), to the game (GetFrame2: the frame shown, +0xEC) and, once the game has moved
// past it by the delay (+0xCE), from the frames given (+0xF0) back to the free list (+0xD4 those not free).
namespace gr {
constexpr u32 kHave = 0xD4, kReady = 0xD8, kTag = 0xDC, kFreeList = 0xE0, kTail = 0xE4, kHead = 0xE8, kShown = 0xEC,
              kGiven = 0xF0, kHeld = 0xF4, kLate = 0xF5, kWord = 0xF8, kLast = 0xA0, kSeq = 0xCD;
}

void video_done(PPUContext& ctx, u32 a, s32 result);

// A frame back on the free list, the player told a frame was played (0x14).
void ga_free(PPUContext& ctx, u32 g, u32 rec)
{
    w32(ctx, rec + 0x20, r32(ctx, g + gr::kFreeList));
    w32(ctx, g + gr::kHave, r32(ctx, g + gr::kHave) - 1);
    w32(ctx, g + gr::kFreeList, rec);
    post(ctx, adapter_player(ctx, g), 0x14, g, 0);
}

// Start (libsail's): from open the play state reset (the first PTS, the frames: every record free, linked in order);
// buffering (2) when the stream was only open and not all frames are in, else playing (3).
void _pspocket_cellSail_gaStart(PPUContext& ctx, u32 g, u32 flag)
{
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {g + ga::kMutex, 0}));
    if (r < 0) {
        video_done(ctx, g, r);
        return;
    }
    if (r32(ctx, g + ga::kState) == 1) {
        const u32 n = r8(ctx, g + ga::kCount), recs = r32(ctx, g + ga::kRecords);
        w64(ctx, g + ga::kFirstPts, ~u64{0});
        w64(ctx, g + gr::kLast, 0);
        w8(ctx, g + gr::kLate, 0);
        w32(ctx, g + ga::kSession, 0);
        w32(ctx, g + gr::kWord, 0);
        w32(ctx, g + gr::kHave, 0);
        w32(ctx, g + gr::kReady, 0);
        w32(ctx, g + gr::kFreeList, recs);
        w32(ctx, g + gr::kTail, 0);
        w32(ctx, g + gr::kShown, 0);
        w32(ctx, g + gr::kHead, 0);
        w32(ctx, g + gr::kGiven, 0);
        w8(ctx, g + gr::kHeld, 0);
        for (u32 i = 0; i + 1 < n; i++) w32(ctx, recs + 40 * i + 0x20, recs + 40 * (i + 1));
        w32(ctx, recs + 40 * n - 40 + 0x20, 0);
    }
    u32 st = 3;
    if (flag != 0 && r32(ctx, g + gr::kHave) < r8(ctx, g + ga::kCount)) st = 2;
    w32(ctx, g + ga::kState, st);
    lv2_lib(ctx, "sys_lwmutex_unlock", {g + ga::kMutex});
    video_done(ctx, g, 0);
}

// Stop (libsail's): a flush gives back the ready frames (each 0x14) and the held one; keeping the last frame stops at
// once (5: the clock told of the stream's end, the stop completing), else it drains through GetFrame2 (4). Open
// answers 1.
void _pspocket_cellSail_gaStop(PPUContext& ctx, u32 g, u32 flush, u32 keep)
{
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {g + ga::kMutex, 0}));
    if (r < 0) {
        video_done(ctx, g, r);
        return;
    }
    const s32 st = static_cast<s32>(r32(ctx, g + ga::kState));
    if (st == 1) {
        video_done(ctx, g, 1);
    } else if (st > 1 && st <= 4) {
        if (flush != 0) {
            const u32 head = r32(ctx, g + gr::kHead);
            const u32 ready = r32(ctx, g + gr::kReady);
            w32(ctx, g + gr::kReady, 0);
            w32(ctx, g + gr::kHave, r32(ctx, g + gr::kHave) - ready);
            if (head != 0) {
                w32(ctx, r32(ctx, g + gr::kTail) + 0x20, r32(ctx, g + gr::kFreeList));
                w32(ctx, g + gr::kFreeList, r32(ctx, g + gr::kHead));
            }
            w32(ctx, g + gr::kTail, 0);
            w32(ctx, g + gr::kHead, 0);
            for (s32 i = static_cast<s32>(ready); i > 0; i--) post(ctx, adapter_player(ctx, g), 0x14, g, 0);
            w8(ctx, g + gr::kHeld, 0);
        }
        if (keep == 0) {
            w32(ctx, g + ga::kState, 4);
        } else {
            w64(ctx, g + gr::kLast, 0x7FFFFFFFFFFFFFFFull);
            w32(ctx, g + ga::kState, 5);
            w8(ctx, g + gr::kHeld, 0);
            clock_video_end(ctx, r32(ctx, g + 0x10), r32(ctx, g + 8), g + 0xC, r32(ctx, g + gr::kWord));
            video_done(ctx, g, 0);
        }
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {g + ga::kMutex});
}

// Checkout: the free list's first record.
s32 _pspocket_cellSail_gaCheckout(PPUContext& ctx, u32 g, u32 out)
{
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {g + ga::kMutex, 0}));
    if (r < 0) return r;
    const u32 rec = r32(ctx, g + gr::kFreeList);
    w32(ctx, out, rec);
    w32(ctx, g + gr::kFreeList, r32(ctx, rec + 0x20));
    lv2_lib(ctx, "sys_lwmutex_unlock", {g + ga::kMutex});
    return 0;
}

// Checkin (libsail's): the record's session word without bit 19; while buffering or playing a good frame of the stream
// goes on the ready list (buffering done when all are in), one older than the last time shown in its session dropped
// (answered 1: the next one is skipped; a late mark), a failed or flagged one given back; otherwise given back.
s32 _pspocket_cellSail_gaCheckin(PPUContext& ctx, u32 g, u32 rec)
{
    const s32 lr = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {g + ga::kMutex, 0}));
    if (lr < 0) return lr;
    const u32 word = r32(ctx, rec + 0x14), st = r32(ctx, g + ga::kState);
    const u32 session = word & ~0x80000u;
    w64(ctx, rec + 0x10, session);
    s32 r = 0;
    bool give_back = true;
    if (st - 2 <= 1) {
        if (r32(ctx, g + gr::kWord) != session) w32(ctx, g + gr::kWord, session);
        if (static_cast<s32>(r32(ctx, rec + 4)) >= 0) {
            bool late = false;
            if (static_cast<s64>(r64(ctx, g + gr::kLast)) > static_cast<s64>(r64(ctx, rec + 8)) &&
                r32(ctx, g + ga::kSession) == word) {
                late = true;
                w8(ctx, g + gr::kLate, static_cast<u8>(((r8(ctx, g + gr::kLate) << 1) | 1) & 3));
                r = 1;
            }
            if (!late && !(word & 0x80000)) {
                give_back = false;
                w32(ctx, rec + 0x20, 0);
                if (const u32 tail = r32(ctx, g + gr::kTail)) w32(ctx, tail + 0x20, rec);
                const u32 head = r32(ctx, g + gr::kHead);
                w32(ctx, g + gr::kTail, rec);
                if (head == 0) w32(ctx, g + gr::kHead, rec);
                const u32 have = r32(ctx, g + gr::kHave) + 1;
                w32(ctx, g + gr::kHave, have);
                w32(ctx, g + gr::kReady, r32(ctx, g + gr::kReady) + 1);
                if (st == 2 && have >= r8(ctx, g + ga::kCount)) w32(ctx, g + ga::kState, 3);
            }
        }
    }
    if (give_back) {
        w32(ctx, rec + 0x20, r32(ctx, g + gr::kFreeList));
        w32(ctx, g + gr::kFreeList, rec);
        post(ctx, adapter_player(ctx, g), 0x14, g, 0);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {g + ga::kMutex});
    return r;
}

// The graphics adapter's open(format, frames, least): the game's allocator gives the pictures (fewer than asked, not
// fewer than the least), their records (0x28 bytes each) from the player's allocator, the game told of the format;
// answers the pictures, or MEMORY (the pictures given back).
void _pspocket_cellSail_gaOpen(PPUContext& ctx, u32 a, u32 format, u32 n, u32 least)
{
    StackVar arr(ctx, gb(ctx), (n * 4 + 30) & ~15u);
    std::memset(gb(ctx) + arr.addr(), 0, n * 4);
    s32 result = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + ga::kMutex, 0}));
    if (result < 0) {
        video_done(ctx, a, result);
        return;
    }
    std::memmove(gb(ctx) + a + ga::kFormat, gb(ctx) + format, 0x20);
    w32(ctx, a + ga::kDuration, kDuration[static_cast<u32>(static_cast<s32>(static_cast<s8>(r8(ctx, format + 3)))) % 9]);
    u32 size = static_cast<u32>(static_cast<s16>(ps_read16(gb(ctx), format + 6))) * r32(ctx, format + 8);
    if (r8(ctx, format) == 3) size = (size * 3) >> 1;
    const s32 got = static_cast<s32>(call_guest(ctx, r32(ctx, a + ga::kFuncs + 12),
        {r32(ctx, a + ga::kArg), (size + 127) & ~127u, static_cast<u64>(static_cast<s64>(static_cast<s32>(n))), arr.addr()}));
    bool ok = got >= 0;
    if (ok && got != 0) {
        if (static_cast<s32>(least) > got) ok = false;
        else n = static_cast<u32>(got);
    }
    u32 recs = 0;
    if (ok) ok = (recs = player_alloc(ctx, adapter_player(ctx, a), 8, (n * 40) & ~7u)) != 0;
    for (u32 i = 0; ok && i < n; i++) {
        const u32 buf = r32(ctx, arr.addr() + 4 * i);
        w32(ctx, recs + 40 * i, buf);
        if (!buf) {
            player_free(ctx, adapter_player(ctx, a), 8, recs);
            ok = false;
            break;
        }
        w8(ctx, recs + 40 * i + 0x27, static_cast<u8>(i));
    }
    if (!ok) {
        if (r32(ctx, arr.addr()))
            call_guest(ctx, r32(ctx, a + ga::kFuncs + 16),
                {r32(ctx, a + ga::kArg), static_cast<u64>(static_cast<s64>(static_cast<s32>(n))), arr.addr()});
        lv2_lib(ctx, "sys_lwmutex_unlock", {a + ga::kMutex});
        video_done(ctx, a, error(CELL_SAIL_ERROR_MEMORY));
        return;
    }
    w32(ctx, a + ga::kState, 1);
    for (const u32 f : {0xF8u, 0xE4u, 0xECu, 0xE8u, 0xF0u}) w32(ctx, a + f, 0);
    w32(ctx, a + ga::kSession, 0);
    w8(ctx, a + ga::kCount, static_cast<u8>(n));
    w32(ctx, a + ga::kFree, recs);
    w32(ctx, a + ga::kRecords, recs);
    StackVar copy(ctx, gb(ctx), 0x20);
    std::memmove(gb(ctx) + copy.addr(), gb(ctx) + format, 0x20);
    call_guest(ctx, r32(ctx, a + ga::kFuncs + 8),
        {r32(ctx, a + ga::kArg), copy.addr(), (r32(ctx, a + ga::kSession) + 1) & 0x3FF});
    w8(ctx, a + ga::kDelay, static_cast<u8>(least - 1));
    result = r8(ctx, a + ga::kCount);
    lv2_lib(ctx, "sys_lwmutex_unlock", {a + ga::kMutex});
    video_done(ctx, a, result);
}

// Close: the pictures given back to the game, the records freed, closed (0); answers 0.
void _pspocket_cellSail_gaClose(PPUContext& ctx, u32 a)
{
    const u32 n = r8(ctx, a + ga::kCount);
    StackVar arr(ctx, gb(ctx), (n * 4 + 30) & ~15u);
    if (static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a + ga::kMutex, 0})) >= 0) {
        if (const u32 recs = r32(ctx, a + ga::kRecords)) {
            if (n) {
                for (u32 i = 0; i < n; i++) w32(ctx, arr.addr() + 4 * i, r32(ctx, recs + 40 * i));
                call_guest(ctx, r32(ctx, a + ga::kFuncs + 16), {r32(ctx, a + ga::kArg), n, arr.addr()});
            }
            player_free(ctx, adapter_player(ctx, a), 8, recs);
            w32(ctx, a + ga::kRecords, 0);
        }
        w32(ctx, a + ga::kState, 0);
        lv2_lib(ctx, "sys_lwmutex_unlock", {a + ga::kMutex});
    }
    video_done(ctx, a, 0);
}

// The AU receiver's open(type, filter): the game's open (none in fw 4.92), open (2) with its queue of four AUs empty;
// answers through message 0x35. Close: the game's close, closed (1), message 0x36.
s32 receiver_post(PPUContext& ctx, u32 rcv, u32 code, u32 value)
{
    return post(ctx, r32(ctx, rcv + 0x20 + 0x30), code, rcv + 0x20, value);
}

void _pspocket_cellSail_auOpen(PPUContext& ctx, u32 rcv, u32 type, u32 filter)
{
    if (const u32 f = r32(ctx, rcv + 0xCC)) call_guest(ctx, f, {r32(ctx, rcv + 0xE8), type, filter});
    w32(ctx, rcv, 2);
    w32(ctx, rcv + 0xF4, 4);
    for (const u32 f : {0x1ACu, 0xF8u, 0xFCu, 0xF0u, 0x1A4u, 0x1A8u}) w32(ctx, rcv + f, 0);
    receiver_post(ctx, rcv, 0x35, 0);
}

void _pspocket_cellSail_auClose(PPUContext& ctx, u32 rcv)
{
    if (const u32 f = r32(ctx, rcv + 0xD0)) call_guest(ctx, f, {r32(ctx, rcv + 0xE8)});
    w32(ctx, rcv, 1);
    receiver_post(ctx, rcv, 0x36, 0);
}

// The AU receiver while playing (libsail's): a queue of four AUs (records of 0x28 bytes at +0x100, as the dmux gives a
// user stream's AU: address, size, its answer, the session (low 10 bits), PTS, DTS, a mark) with +0xF0 the AUs found
// at the player and not yet taken, +0xF4 the free records, +0xF8 the next record to fill and +0xFC the next to read,
// +0x1A4 the AUs queued. Get hands them out double buffered: two slots (+0x1A0 the one to fill next, +0x1A8 and +0x1AC
// whether each holds an AU, +0x1B0 and +0x1B4 their records) so the AU handed out stays the game's until the next Get
// gives it back (message 0x16). +0x1B8 is the mode: 0 not playing, 1 playing, 2 ending.
namespace au {
constexpr u32 kFound = 0xF0, kFree = 0xF4, kWrite = 0xF8, kRead = 0xFC, kRecords = 0x100, kCurrent = 0x1A0,
              kQueued = 0x1A4, kHeld = 0x1A8, kHeldRecord = 0x1B0, kMode = 0x1B8;
}

u32 receiver_es(PPUContext& ctx, u32 rcv) { return r32(ctx, rcv + 0x20 + 0x34) + us::kDmuxEs; }

// One AU from the player into the next free record when one is found and a record free (one per call; the lock's
// answer not checked, as libsail).
void au_fill(PPUContext& ctx, u32 rcv)
{
    lv2_lib(ctx, "sys_lwmutex_lock", {rcv + 8, 0});
    if (r32(ctx, rcv + au::kFree) != 0 && r32(ctx, rcv + au::kFound) != 0) {
        const u32 i = r32(ctx, rcv + au::kWrite);
        dmux_es_get_au_user(ctx, receiver_es(ctx, rcv), rcv + au::kRecords + 0x28 * i);
        w32(ctx, rcv + au::kWrite, (i + 1) & 3);
        w32(ctx, rcv + au::kQueued, r32(ctx, rcv + au::kQueued) + 1);
        w32(ctx, rcv + au::kFree, r32(ctx, rcv + au::kFree) - 1);
        w32(ctx, rcv + au::kFound, r32(ctx, rcv + au::kFound) - 1);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {rcv + 8});
}

// Start(arg, 0, flag): the game's start, started (3), +0xEC cleared; answers 0x37.
s32 _pspocket_cellSail_auStart(PPUContext& ctx, u32 rcv, u32 x, u32 flag)
{
    if (const u32 f = r32(ctx, rcv + 0xD4)) call_guest(ctx, f, {r32(ctx, rcv + 0xE8), x, flag & 0xFF});
    w32(ctx, rcv, 3);
    w32(ctx, rcv + 0xEC, 0);
    return receiver_post(ctx, rcv, 0x37, 0);
}

// Stop(arg, flush): the game's stop, ending (mode 2): draining (5) through Get, or with flush every queued AU given
// back (0x16 each), the queue emptied, flushed (4). Get answers 0x38 once nothing is queued (a lock refused answers it
// at once with the error).
s32 _pspocket_cellSail_auStop(PPUContext& ctx, u32 rcv, u32 flush)
{
    if (const u32 f = r32(ctx, rcv + 0xD8)) call_guest(ctx, f, {r32(ctx, rcv + 0xE8), flush & 0xFF});
    const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {rcv + 8, 0}));
    if (r < 0) return receiver_post(ctx, rcv, 0x38, static_cast<u32>(r));
    w32(ctx, rcv + au::kMode, 2);
    if (flush == 0) {
        w32(ctx, rcv, 5);
    } else {
        u32 n = r32(ctx, rcv + au::kQueued);
        w32(ctx, rcv + au::kFound, 0);
        while (n != 0) {
            n--;
            receiver_post(ctx, rcv, 0x16, 1);
        }
        w32(ctx, rcv + au::kQueued, 0);
        w32(ctx, rcv, 4);
    }
    return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_unlock", {rcv + 8}));
}

// Checkout (an AU found at the player): started, one more found and playing (mode 1), the queue filled; otherwise the
// AU taken into the first record and given straight back (0x16). Checkin (an AU given back taken by the player): a
// record more free (also after those given straight back, as libsail), the queue filled. Both answer 0.
s32 _pspocket_cellSail_auCheckout(PPUContext& ctx, u32 rcv)
{
    if (r32(ctx, rcv) == 3) {
        w32(ctx, rcv + au::kFound, r32(ctx, rcv + au::kFound) + 1);
        w32(ctx, rcv + au::kMode, 1);
        au_fill(ctx, rcv);
        return 0;
    }
    dmux_es_get_au_user(ctx, receiver_es(ctx, rcv), rcv + au::kRecords);
    receiver_post(ctx, rcv, 0x16, 1);
    return 0;
}

s32 _pspocket_cellSail_auCheckin(PPUContext& ctx, u32 rcv)
{
    w32(ctx, rcv + au::kFree, r32(ctx, rcv + au::kFree) + 1);
    au_fill(ctx, rcv);
    return 0;
}

// Get's slots: the other one (libsail's (|slot| - 1) >> 31), whether one holds an AU and its record, the next queued AU
// taken into one, its AU or the next one taken, and its AU given back to the player (0x16).
u32 au_other(u32 slot) { return static_cast<s32>(slot) == 0 ? 1 : 0; }
bool au_held(PPUContext& ctx, u32 rcv, u32 i) { return r32(ctx, rcv + au::kHeld + 4 * i) != 0; }
u32 au_held_record(PPUContext& ctx, u32 rcv, u32 i) { return r32(ctx, rcv + au::kHeldRecord + 4 * i); }

u32 au_take(PPUContext& ctx, u32 rcv, u32 i)
{
    const u32 n = r32(ctx, rcv + au::kQueued);
    if (n == 0) return 0;
    const u32 idx = r32(ctx, rcv + au::kRead);
    w32(ctx, rcv + au::kQueued, n - 1);
    const u32 rec = rcv + au::kRecords + 0x28 * idx;
    w32(ctx, rcv + au::kHeld + 4 * i, 1);
    w32(ctx, rcv + au::kRead, (idx + 1) & 3);
    w32(ctx, rcv + au::kHeldRecord + 4 * i, rec);
    return rec;
}

u32 au_slot(PPUContext& ctx, u32 rcv, u32 i)
{
    return au_held(ctx, rcv, i) ? au_held_record(ctx, rcv, i) : au_take(ctx, rcv, i);
}

void au_release(PPUContext& ctx, u32 rcv, u32 i)
{
    if (!au_held(ctx, rcv, i)) return;
    receiver_post(ctx, rcv, 0x16, 1);
    w32(ctx, rcv + au::kHeld + 4 * i, 0);
}

// The adapters' renderer callback tables (libsail's data): 9 descriptors each.
void init_sound_table(u8* b, u32 at)
{
    static constexpr std::string_view kNames[9] = {"_pspocket_cellSail_saMakeup", "_pspocket_cellSail_saCleanup",
        "_pspocket_cellSail_saOpen", "_pspocket_cellSail_saClose", "_pspocket_cellSail_saStart",
        "_pspocket_cellSail_saStop", "_pspocket_cellSail_saCancel", "_pspocket_cellSail_saCheckout",
        "_pspocket_cellSail_saCheckin"};
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, internal(kNames[i]));
}

void init_graphics_table(u8* b, u32 at)
{
    static constexpr std::string_view kNames[9] = {"_pspocket_cellSail_gaMakeup", "_pspocket_cellSail_gaCleanup",
        "_pspocket_cellSail_gaOpen", "_pspocket_cellSail_gaClose", "_pspocket_cellSail_gaStart",
        "_pspocket_cellSail_gaStop", "_pspocket_cellSail_gaCancel", "_pspocket_cellSail_gaCheckout",
        "_pspocket_cellSail_gaCheckin"};
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, internal(kNames[i]));
}

// The AU receiver's (9 descriptors, copied into each receiver).
void init_receiver_table(u8* b, u32 at)
{
    static constexpr std::string_view kNames[9] = {"_pspocket_cellSail_auMakeup", "_pspocket_cellSail_auCleanup",
        "_pspocket_cellSail_auOpen", "_pspocket_cellSail_auClose", "_pspocket_cellSail_auStart",
        "_pspocket_cellSail_auStop", "_pspocket_cellSail_auCancel", "_pspocket_cellSail_auCheckout",
        "_pspocket_cellSail_auCheckin"};
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, internal(kNames[i]));
}

u32 table(std::string_view name)
{
    return HleProcess::current()->link_variable("cellSail", recomp::nid_of(name));
}

// ------------------------------------------------------------------------------------------------- helpers

s32 lwmutex_create(PPUContext& ctx, u32 at, const char* name)
{
    u8* b = gb(ctx);
    StackVar v(ctx, b, 16);
    ps_write32(b, v.addr(), 2);
    ps_write32(b, v.addr() + 4, 0x20);
    std::memcpy(b + v.addr() + 8, name, 8);
    return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_create", {at, v.addr()}));
}

s32 lwmutex_destroy(PPUContext& ctx, u32 at) { return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_destroy", {at})); }

s32 renderer_initialize(PPUContext& ctx, bool video, u32 r, u32 funcs, u32 arg, u32 attr)
{
    const char* name = video ? "cellSailRendererVideoInitialize" : "cellSailRendererAudioInitialize";
    const Function* f = registry().find("cellSail", recomp::nid_of(name));
    u64 saved[4] = {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]};
    ctx.gpr[3] = r;
    ctx.gpr[4] = funcs;
    ctx.gpr[5] = arg;
    ctx.gpr[6] = attr;
    f->fn(ctx, gb(ctx));
    const s32 res = static_cast<s32>(ctx.gpr[3]);
    for (u32 i = 0; i < 4; i++) ctx.gpr[3 + i] = saved[i];
    return res;
}

// The adapter's lwmutex destroyed, again every `wait` microseconds while busy, ten times at most.
void destroy_retrying(PPUContext& ctx, u32 at, u32 wait)
{
    for (u32 n = 0; n < 10; n++) {
        if (static_cast<u32>(lwmutex_destroy(ctx, at)) != 0x8001000A) return;
        sc(ctx, 141, {wait});
    }
}

// ------------------------------------------------------------------------------------------------- sound adapter

s32 cellSailSoundAdapterInitialize(PPUContext& ctx, u32 a, u32 funcs, u32 arg)
{
    std::memset(gb(ctx) + a, 0, sa::kSize);
    s32 r = lwmutex_create(ctx, a + sa::kMutex, "_saisa0");
    if (r < 0) return r;
    r = renderer_initialize(ctx, false, a, table("_pspocket_cellSailSoundAdapterRendererFuncs"), a, 0);
    if (r < 0) {
        lwmutex_destroy(ctx, a + sa::kMutex);
        return r;
    }
    w64(ctx, a + sa::kFirstPts, ~u64{0});
    std::memmove(gb(ctx) + a + sa::kFuncs, gb(ctx) + funcs, 12);
    w32(ctx, a + sa::kArg, arg);
    w32(ctx, a + sa::kState, kInitialized);
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterFinalize(PPUContext& ctx, u32 a)
{
    w32(ctx, a + 0x1C, 9); // cellSailRendererAudioFinalize
    destroy_retrying(ctx, a + sa::kMutex, 5000);
    w32(ctx, a + sa::kState, kFinalized);
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterGetFormat(PPUContext& ctx, u32 a, u32 out)
{
    std::memmove(gb(ctx) + out, gb(ctx) + a + sa::kFormat, 0x18);
    w16(ctx, out + 2, 0x100);
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterSetPreferredFormat(PPUContext& ctx, u32 a, u32 format)
{
    if (!format) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + a + sa::kPreferred, gb(ctx) + format, 0x18);
    return lv2::CELL_OK;
}

// GetFrame(adapter, samples, info) (libsail's): the game's next `samples` of PCM, copied from the frames ready into the
// adapter's output buffer, with the session (its low 10 bits), the clock's tag, the frame's status and its PTS. Playing
// (3) or draining (4) with frames ready and neither paused nor hidden (flags 8, 0x10): without AV sync (flag 4) the
// frames are taken as they come (a new session sets the rate flag and the first PTS), answering 0x214; with it the clock
// decides: a new session's frames counted, the frames of a session left behind dropped (each given back, 0x12), not due
// (28: nothing), due (0x218), or, with nothing synced, by the clock's time: early (12: nothing), late by more than one
// GetFrame (the bytes behind skipped, whole frames given back), else in time; the frames used up given back (0x12), the
// rest of the output zeroed when they run out (bit 0 or 2 of the answer's flags 0x200 + frames << 8). Otherwise the
// clock counts the samples: paused or hidden 36, playing with nothing ready 1 (frame stepping: 32), else 28; a drained
// stream ends (the clock told, the output's end 0x1D, the stop completing, open again: 48). Muted or not at the normal
// rate (flags 1, 0x20) gives no buffer (44); a frame step (flag 2) 32.
s32 cellSailSoundAdapterGetFrame(PPUContext& ctx, u32 a, u32 samples, u32 info)
{
    const u32 lw = a + sa::kMutex;
    const s32 lr = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0}));
    if (lr < 0) {
        w32(ctx, info + 0xC, 0);
        w64(ctx, info + 0x10, 0);
        w32(ctx, info, 0);
        w32(ctx, info + 4, 0);
        w32(ctx, info + 8, 0);
        return lr;
    }
    const u32 p = adapter_player(ctx, a), flags = r32(ctx, a + 0x34), st = r32(ctx, a + sa::kState);
    const u32 frame_bytes = r32(ctx, a + sa::kFrameBytes), fs = r32(ctx, a + sa::kFormat + 4);
    u32 ready = r32(ctx, a + 0xBC), offset = r32(ctx, a + 0x88);
    const u32 bpf = static_cast<u32>(static_cast<s32>(static_cast<s8>(r8(ctx, a + sa::kFormat + 1))) * 4);
    const bool hidden = (flags & 0x18) != 0;
    const u32 clock = r32(ctx, a + 0x10), index = r32(ctx, a + 8), n = r32(ctx, a + sa::kCount);
    StackVar now_v(ctx, gb(ctx), 16);
    w64(ctx, now_v.addr(), 0);
    u32 buf = 0, status = 0;
    s32 ret;
    u64 pts = ~u64{0};
    if (!(st - 3 <= 1 && ready != 0 && !hidden)) {
        const u32 hold = (flags & 0x40) ? (hidden ? 1 : 0) : 1;
        clock_audio_tick(ctx, clock, a + sa::kSync, samples, index, now_v.addr(), hold & 1);
        if (hidden) {
            ret = 36;
        } else if (st == 3) {
            ret = (flags & 2) ? 32 : 1;
        } else {
            ret = 28;
            if (st == 4) {
                w32(ctx, a + sa::kState, 1);
                w64(ctx, a + sa::kFirstPts, ~u64{0});
                clock_audio_end(ctx, clock, index, a + 0xC, r32(ctx, a + sa::kWord));
                post(ctx, p, 0x1D, a, 0);
                audio_done(ctx, a, 0);
                ret = 48;
            }
        }
    } else {
        // Playing.
        u32 idx = r32(ctx, a + sa::kGet), done = 0, bits = 0;
        const u32 records = r32(ctx, a + sa::kRecords);
        u32 rec = records + idx * 0x18;
        const u64 rec_session = r64(ctx, rec + 0x10);
        status = r32(ctx, rec + 4);
        const u64 bps = static_cast<u32>(bpf * fs);
        pts = divu(u64{offset} * 90000, bps) + r64(ctx, rec + 8);
        // `copy`: fill the output from the frames; `finish`: the frames used up given back.
        enum class Next { Copy, Timing, Finish, Exit };
        Next next;
        ret = 0;
        if (flags & 4) {
            if (r32(ctx, a + sa::kSession) != static_cast<u32>(rec_session)) {
                session_rate_flag(ctx, a, static_cast<u32>(rec_session));
                const u64 first = first_pts(ctx, a);
                w32(ctx, a + sa::kSession, static_cast<u32>(rec_session));
                w64(ctx, a + sa::kFirstPts, first);
            }
            clock_audio_tick(ctx, clock, a + sa::kSync, samples, index, now_v.addr(), 0);
            bits = 0x200;
            ret = 532;
            next = Next::Copy;
        } else {
            const u32 r = clock_audio_frame(ctx, clock, pts, static_cast<u32>(rec_session), r32(ctx, a + sa::kSession),
                a + sa::kSync, samples, now_v.addr(), 0, a + 0xC, index);
            next = Next::Timing;
            if (r != 0) {
                if (r & 1) {
                    session_rate_flag(ctx, a, static_cast<u32>(rec_session));
                    const u64 first = first_pts(ctx, a);
                    w64(ctx, a + sa::kFirstPts, first);
                    w32(ctx, a + sa::kSession, static_cast<u32>(rec_session));
                }
                if (r & 4) {
                    // The frames of the session left behind given back.
                    for (;;) {
                        if (static_cast<u32>(r64(ctx, records + idx * 0x18 + 0x10)) != static_cast<u32>(rec_session)) break;
                        ready--;
                        idx = n ? (idx + 1) % n : 0;
                        post(ctx, p, 0x12, a, 0);
                        if (ready == 0) break;
                    }
                    offset = 0;
                    buf = 0;
                    ret = 0;
                    w32(ctx, a + sa::kGet, idx);
                    next = Next::Exit;
                } else if (r & 2) {
                    buf = 0;
                    ret = 28;
                    next = Next::Exit;
                } else {
                    done = 0;
                    bits = 0x200;
                    ret = 536;
                    if (r & 8) next = Next::Copy;
                }
            }
        }
        if (next == Next::Timing) {
            const u64 now = r64(ctx, now_v.addr());
            buf = 0;
            ret = 12;
            next = Next::Exit;
            if (pts <= now) {
                const u64 behind = now - pts;
                bits = 0x200;
                ret = 0;
                done = 0;
                next = Next::Copy;
                if (divu(u64{samples} * 90000, fs) + 1 < behind) {
                    // Late: the bytes behind skipped.
                    const u64 x = behind * (u64{fs} * bpf);
                    u32 skip = static_cast<u32>(static_cast<u64>((static_cast<unsigned __int128>(x) *
                                                                  0xBA69DBDD3AC13D7Dull) >> 64) >> 16) & ~3u;
                    u32 count = ready;
                    for (;;) {
                        const u32 avail = frame_bytes - offset, after = idx + 1;
                        if (skip < avail) {
                            offset += skip;
                            ret = 0;
                            bits = (done << 8) + 0x200;
                            if (pts != ~u64{0}) pts += divu(u64{offset} * 90000, bps);
                            break;
                        }
                        skip -= avail;
                        offset = 0;
                        done++;
                        ready--;
                        if (--count == 0) {
                            offset = 0;
                            buf = 0;
                            ret = 0;
                            next = Next::Finish;
                            break;
                        }
                        idx = n ? after % n : 0;
                        rec = records + idx * 0x18;
                        if (r32(ctx, a + sa::kSession) != static_cast<u32>(r64(ctx, rec + 0x10))) {
                            offset = 0;
                            buf = 0;
                            ret = 0;
                            next = Next::Finish;
                            break;
                        }
                    }
                }
            }
        }
        if (next == Next::Copy) {
            buf = r32(ctx, a + sa::kSilence);
            u32 dst = buf, want = bpf * samples, result_bits = 0;
            for (;;) {
                const u32 avail = frame_bytes - offset, take = want <= avail ? want : avail;
                std::memmove(gb(ctx) + dst, gb(ctx) + r32(ctx, rec) + offset, take);
                want -= take;
                offset += take;
                dst += take;
                const u32 after = idx + 1;
                if (frame_bytes > offset) {
                    if (want != 0) continue;
                    result_bits = bits | 4;
                    break;
                }
                ready--;
                done++;
                offset = 0;
                if (ready == 0) {
                    result_bits = bits | 5;
                    if (want != 0) std::memset(gb(ctx) + dst, 0, want);
                    break;
                }
                idx = n ? after % n : 0;
                rec = records + idx * 0x18;
                if (r32(ctx, a + sa::kSession) != static_cast<u32>(r64(ctx, rec + 0x10))) {
                    result_bits = bits | 4;
                    if (want != 0) std::memset(gb(ctx) + dst, 0, want);
                    break;
                }
                if (want == 0) {
                    result_bits = bits | 4;
                    break;
                }
            }
            if (ret == 0) ret = static_cast<s32>(result_bits);
            next = Next::Finish;
        }
        if (next == Next::Finish) {
            const u32 at = r32(ctx, a + sa::kGet) + done;
            w32(ctx, a + sa::kGet, n ? at % n : 0);
            for (; done; done--) post(ctx, p, 0x12, a, 0);
            if (flags & 2) {
                buf = 0;
                status = 0;
                ret = 32;
                pts = ~u64{0};
            }
        }
    }
    const u32 session = r32(ctx, a + sa::kSession);
    w32(ctx, a + 0xBC, ready);
    w32(ctx, a + 0x88, offset);
    const u32 tag = r32(ctx, a + sa::kSync);
    lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
    if (flags & 0x21) {
        buf = 0;
        ret = 44;
    }
    w64(ctx, info + 0x10, pts);
    w32(ctx, info, buf);
    w32(ctx, info + 4, session & 0x3FF);
    w32(ctx, info + 8, tag);
    w32(ctx, info + 0xC, status);
    return ret;
}

// The PTS as milliseconds from the first one (90 kHz ticks).
s32 pts_to_time(PPUContext& ctx, u32 first_at, s64 pts, u32 out)
{
    const s64 first = static_cast<s64>(r64(ctx, first_at));
    if (first == -1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    w64(ctx, out, static_cast<u64>(((pts - first) * 1000) / 90));
    return lv2::CELL_OK;
}

s32 cellSailSoundAdapterPtsToTimePosition(PPUContext& ctx, u32 a, s64 pts, u32 out)
{
    return pts_to_time(ctx, a + sa::kFirstPts, pts, out);
}

// The clock's audio half (libsail's): from the game's (frame number, time in microseconds) the time per frame, in
// 1/256 frames where the clock was made with a latency; the first estimate after 100 ms, later ones smoothed by a
// quarter when within a quarter of the last.
void avsync_audio(PPUContext& ctx, u32 c, u32 frame, u64 time, u32 record, u32 index)
{
    if (!frame) return;
    lv2_lib(ctx, "sys_lwmutex_lock", {c + 0xB0, 0});
    const u32 mark = r32(ctx, record);
    bool keep = true;
    if (r32(ctx, c + 8) == 0) {
        if (r64(ctx, c + 0x18) == 0) {
            if (r64(ctx, c + 0x10) != 0) {
                const u64 dt = time - r64(ctx, c + 0x10);
                if (dt < 100000) {
                    keep = false;
                } else {
                    w32(ctx, c + 8, 1);
                    const u64 per = (dt * 0x100) / static_cast<u32>(frame - r32(ctx, c + 0xC));
                    w64(ctx, c + 0x18, per);
                    w64(ctx, c + 0x20, static_cast<u64>(static_cast<s64>(per * r64(ctx, c)) / 1000));
                }
            }
        } else {
            w32(ctx, c + 8, 1);
        }
    } else {
        const u32 frames = frame - r32(ctx, c + 0xC);
        const u64 dt = time - r64(ctx, c + 0x10);
        const u64 per = r32(ctx, c + 0x80) == 0 ? dt / frames : (dt * 0x100) / frames;
        const u64 last = r64(ctx, c + 0x18);
        if (last <= per + (last >> 2) && per <= last + (last >> 2)) {
            const u64 smooth = (per + last * 3) >> 2;
            w64(ctx, c + 0x18, smooth);
            w32(ctx, c + 8, 2);
            w64(ctx, c + 0x50, time + smooth * static_cast<u32>(mark - frame));
            w64(ctx, c + 0x20, static_cast<u64>(static_cast<s64>(smooth * r64(ctx, c)) / 1000));
        }
    }
    if (keep) {
        w32(ctx, c + 0x48, index);
        w64(ctx, c + 0x10, time);
        w32(ctx, c + 0xC, frame);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {c + 0xB0});
}

void cellSailSoundAdapterUpdateAvSync(PPUContext& ctx, u32 a, u32 frame, u64 time)
{
    avsync_audio(ctx, r32(ctx, a + 0x10), frame, time, a + sa::kSync, r32(ctx, a + 8));
}

// ------------------------------------------------------------------------------------------------- graphics adapter

s32 cellSailGraphicsAdapterInitialize(PPUContext& ctx, u32 a, u32 funcs, u32 arg)
{
    std::memset(gb(ctx) + a, 0, ga::kSize);
    s32 r = lwmutex_create(ctx, a + ga::kMutex, "_saiga0");
    if (r < 0) return r;
    r = renderer_initialize(ctx, true, a, table("_pspocket_cellSailGraphicsAdapterRendererFuncs"), a, 0);
    if (r < 0) {
        lwmutex_destroy(ctx, a + ga::kMutex);
        return r;
    }
    w64(ctx, a + ga::kFirstPts, ~u64{0});
    std::memmove(gb(ctx) + a + ga::kFuncs, gb(ctx) + funcs, 20);
    w32(ctx, a + ga::kArg, arg);
    w32(ctx, a + ga::kState, kInitialized);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterFinalize(PPUContext& ctx, u32 a)
{
    w32(ctx, a + 0x1C, 9); // cellSailRendererVideoFinalize
    destroy_retrying(ctx, a + ga::kMutex, 5000);
    w32(ctx, a + ga::kState, kFinalized);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterGetFormat(PPUContext& ctx, u32 a, u32 out)
{
    std::memmove(gb(ctx) + out, gb(ctx) + a + ga::kFormat, 0x20);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterSetPreferredFormat(PPUContext& ctx, u32 a, u32 format)
{
    if (!format) return error(CELL_SAIL_ERROR_INVALID_ARG);
    w64(ctx, a + ga::kFirstPts, ~u64{0});
    std::memmove(gb(ctx) + a + ga::kPreferred, gb(ctx) + format, 0x20);
    return lv2::CELL_OK;
}

s32 cellSailGraphicsAdapterPtsToTimePosition(PPUContext& ctx, u32 a, s64 pts, u32 out)
{
    return pts_to_time(ctx, a + ga::kFirstPts, pts, out);
}

// GetFrame2(adapter, info, previous info, flip time (not read), flags) (libsail's, its labels kept): the picture to show
// now with its session (low 10 bits), the clock's tag, status and PTS; `previous` gets the buffer of the frame shown
// before. The frames given to the game go back to the renderer once the game is the delay past them. Playing with
// frames ready and nothing held: with AV sync the clock decides (a new session (16); not synced: the frame shown before
// again (0x10C) or nothing (12); synced: the latest due frame of the session, the ones before it given back, answered
// 0x200 + those given back << 8 and 0x10 late, 8 in time or 4 early (held for the next call); not due yet: the frame
// shown before (0x10C, 0x104) or 12); without, the next frame (0x214). Paused or hidden: 36, or the frame shown again
// (0x124); buffering: the frame shown again (0x11C) or 28; playing with nothing ready: the frame shown again (0x104,
// late 0x101) or 1; draining: the end once every frame is back (the clock told, the output's end 0x1E, the stop
// completing, open: 48). Muted (flag 1) gives no frame.
s32 cellSailGraphicsAdapterGetFrame2(PPUContext& ctx, u32 g, u32 info, u32 prev, u32, u64 flags_in)
{
    const u32 lw = g + ga::kMutex;
    const s32 lr = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0}));
    if (lr < 0) {
        w32(ctx, info + 0xC, 0);
        w64(ctx, info + 0x10, 0);
        w32(ctx, info, 0);
        w32(ctx, info + 4, 0);
        w32(ctx, info + 8, 0);
        return lr;
    }
    const u32 p = adapter_player(ctx, g), clock = r32(ctx, g + 0x10), index = r32(ctx, g + 8);
    const u32 state = r32(ctx, g + ga::kState);
    const u8 late_old = r8(ctx, g + gr::kLate);
    const u32 flags = r32(ctx, g + 0x34);
    const u32 shown_old = r32(ctx, g + gr::kShown), dur = r32(ctx, g + ga::kDuration);
    u32 head = r32(ctx, g + gr::kHead), given = r32(ctx, g + gr::kGiven);
    w8(ctx, g + gr::kLate, static_cast<u8>(late_old >> 1));
    const bool hidden = (flags & 0x18) != 0;
    StackVar times(ctx, gb(ctx), 16); // +0 the time from, +8 to
    const u32 from_at = times.addr(), to_at = times.addr() + 8;
    w64(ctx, from_at, 0);
    u32 frame = 0, shown = shown_old, r14 = 0, ret = 0, rec = 0, session = 0, base = 0;
    u64 keep = flags_in, head_pts = 0, slack = 0;
    u32 next = 0, cand = 0, dropped = 0;
    u32 buf = 0, status = 0;
    u64 pts = ~u64{0};
    const auto late_mark = [&] { w8(ctx, g + gr::kLate, static_cast<u8>(((r8(ctx, g + gr::kLate) << 1) | 1) & 3)); };
    const auto new_session = [&](u32 word) {
        u32 f = r32(ctx, g + 0x34);
        if (word & 0x100000) {
            f = (f & ~8u) | 0x20;
            w32(ctx, g + 0x34, f);
        }
        w32(ctx, g + 0x34, (f & ~0x100u) | (((word >> 21) & 1) << 8));
        const u64 first = first_pts(ctx, g);
        w64(ctx, g + ga::kFirstPts, first);
        w32(ctx, g + ga::kSession, word);
        w64(ctx, g + gr::kLast, ~u64{0});
    };
    if (state - 3 <= 2 && head != 0 && r8(ctx, g + gr::kHeld) == 0 && (!hidden || shown_old == 0)) goto L75c4;
    // L72d8
    if (state == 7) goto L74b4;
    if (hidden) {
        clock_video_tick(ctx, clock, g + gr::kTag, index, from_at, to_at, 1);
        if (shown_old == 0) goto L78ec;
        goto L7708;
    }
    clock_video_tick(ctx, clock, g + gr::kTag, index, from_at, to_at, late_old != 0 ? 1 : 0);
    if (state == 3 || state == 5) goto L7544;
    if (state == 4) goto L787c;
    if (state == 2) {
        if (shown_old == 0) goto L74b4;
        ret = 284;
        keep = 0;
        r14 = 0;
        goto L7590;
    }
    shown = shown_old; // L7330
    frame = 0;
    ret = 28;
    keep = 0;
    r14 = 0;
    goto L7344;
L74b4:
    ret = 28;
    r14 = 0;
L74bc:
    shown = shown_old;
    frame = 0;
    keep = 0;
L7344:
    if (head == 0) w32(ctx, g + gr::kTail, head);
    goto L7348;
L78ec:
    ret = 36;
    r14 = 0;
    goto L74bc;
L7708:
    ret = 292;
    keep = 0;
    r14 = 0;
    goto L7590;
L7544:
    if (shown_old == 0) {
        late_mark();
        ret = 1;
        r14 = 0;
        goto L74bc;
    }
    {
        const u64 end = u64{dur} + r64(ctx, shown_old + 8);
        ret = 260;
        keep = 1;
        r14 = 0;
        if (end - 100 > r64(ctx, to_at)) goto L7590;
        if (end <= r64(ctx, from_at)) {
            late_mark();
            ret = 257;
            keep = 0;
            goto L7590;
        }
        ret = 260;
        keep = 0;
        r14 = 0;
        goto L7590;
    }
L787c:
    if (r32(ctx, g + gr::kHave) == 0) {
        w32(ctx, g + ga::kState, 1);
        w64(ctx, g + ga::kFirstPts, ~u64{0});
        w64(ctx, g + gr::kLast, 0);
        clock_video_end(ctx, clock, index, g + 0xC, r32(ctx, g + gr::kWord));
        post(ctx, p, 0x1E, g, 0);
        ret = 48;
        video_done(ctx, g, 0);
        r14 = 0;
        goto L74bc;
    }
    if (head == 0 && video_renderer_first_frame(ctx, g)) {
        if (shown_old != 0) goto L7708;
        goto L78ec;
    }
    if (!(flags & 4) && shown_old != 0 && r64(ctx, shown_old + 0x10) == r32(ctx, g + ga::kSession)) {
        if (u64{dur} + r64(ctx, shown_old + 8) - 100 > r64(ctx, from_at)) {
            ret = 260;
            keep = 0;
            r14 = 0;
            goto L7590;
        }
    }
    ret = 48;
    r14 = 0;
    goto L74bc;
L75c4: // playing
    rec = head;
    head_pts = r64(ctx, rec + 8);
    session = r32(ctx, rec + 0x14);
    if (flags & 4) {
        // L7734: without AV sync, the next frame.
        if (r32(ctx, g + ga::kSession) != session) new_session(session);
        clock_video_tick(ctx, clock, g + gr::kTag, index, from_at, to_at, hidden ? 1 : (late_old != 0 ? 1 : 0));
        frame = head;
        w32(ctx, g + gr::kReady, r32(ctx, g + gr::kReady) - 1);
        head = r32(ctx, rec + 0x20);
        ret = 532;
        keep = 0;
        r14 = 0;
        goto L77ec;
    }
    {
        const u32 r = clock_video_frame(ctx, clock, head_pts, session, r32(ctx, g + ga::kSession), g + gr::kTag,
            from_at, to_at, late_old != 0 ? 1 : 0, g + 0xC, index);
        if (r == 0) goto L78f8;
        if (r & 1) new_session(session);
        ret = 16;
        r14 = 0;
        if (r & 4) goto L74bc;
        if ((r & 2) == 0 || r32(ctx, g + ga::kSession) == 0) {
            // L7b64
            if (r64(ctx, from_at) == ~u64{0}) goto L74b4;
            if (head_pts != ~u64{0}) goto L78f8;
            w32(ctx, g + gr::kReady, r32(ctx, g + gr::kReady) - 1);
            frame = head;
            head = r32(ctx, rec + 0x20);
            ret = 536;
            keep = 0;
            r14 = 0;
            goto L77ec;
        }
        if (shown_old == 0) {
            ret = 12;
            r14 = 0;
            goto L74bc;
        }
        ret = 268;
        keep = 0;
        goto L7590;
    }
L78f8: // whether the frame is due
    if (flags != 0 && (flags & 0x40)) {
        ret = 32;
        r14 = 0;
        goto L74bc;
    }
    {
        const u8 st = r8(ctx, rec + 0x1A);
        const u64 bit = u64{1} << (static_cast<s8>(st) & 63);
        bool wide;
        if (keep == 0) wide = st <= 9 && (bit & 0x20A) != 0;
        else if (keep == 1) wide = st <= 10 && ((keep << (static_cast<s8>(st) & 63)) & 0x414) != 0;
        else {
            slack = 100;
            r14 = 0;
            goto L7950;
        }
        if (wide) {
            slack = u64{dur} + 100;
            r14 = 0;
        } else {
            slack = 100;
            r14 = 2;
        }
    }
L7950:
    {
        const u64 from = r64(ctx, from_at);
        if (slack + from < head_pts) {
            // L7b00: not due yet.
            if (shown_old == 0) {
                ret = 12;
                goto L74bc;
            }
            ret = 268;
            keep = 0;
            if (r64(ctx, shown_old + 8) <= from + 100) ret = 260;
            goto L7590;
        }
        next = r32(ctx, rec + 0x20);
        w32(ctx, g + gr::kReady, r32(ctx, g + gr::kReady) - 1);
        if (next == 0 || r32(ctx, g + ga::kSession) != static_cast<u32>(r64(ctx, next + 0x10))) {
            frame = head; // L7c08
            base = 512;
            goto L7a14;
        }
        u32 cur = next;
        u64 cur_pts = r64(ctx, cur + 8);
        u32 kept = head;
        cand = next;
        dropped = 0;
        frame = kept;
        ret = 516;
        if (cur_pts > from) goto L7bb8;
        for (;;) {
            // L79d4: the frame before given back.
            ga_free(ctx, g, kept);
            dropped++;
            const u32 after = r32(ctx, cur + 0x20);
            w32(ctx, g + gr::kReady, r32(ctx, g + gr::kReady) - 1);
            kept = cand;
            cur = after;
            next = after;
            if (after == 0) break;
            // L79b0
            if (r32(ctx, g + ga::kSession) != static_cast<u32>(r64(ctx, cur + 0x10))) break;
            cur_pts = r64(ctx, cur + 8);
            if (cur_pts > r64(ctx, from_at)) {
                // L7ba8
                frame = kept;
                ret = ((dropped << 8) + 512) | 4;
                goto L7bb8;
            }
            cand = after;
        }
        frame = kept; // L7a04
        base = (dropped << 8) + 512;
        goto L7a14;
    L7bb8:
        keep = slack + r64(ctx, to_at) < cur_pts ? 1 : 0;
        head = next;
        w64(ctx, g + gr::kLast, from);
        goto L77ec;
    }
L7a14:
    {
        const u64 from = r64(ctx, from_at), end = u64{dur} + r64(ctx, frame + 8);
        ret = base | 0x10;
        keep = 0;
        if (end >= from) {
            ret = base | 8;
            if (slack + r64(ctx, to_at) < end) {
                ret = base | 4;
                keep = 1;
            }
        }
        head = next;
        w64(ctx, g + gr::kLast, from);
    }
L77ec:
    if (shown_old == frame) {
        // L7c14: the same frame again.
        w8(ctx, shown_old + 0x24, static_cast<u8>(r8(ctx, g + gr::kSeq) + r8(ctx, g + 0xCE)));
        w32(ctx, shown_old + 0x20, frame);
        goto L75b4;
    }
    {
        const u32 f = r32(ctx, g + 0x34);
        if (f & 0x20) {
            w32(ctx, g + 0x34, (f & ~0x20u) | 8);
            post(ctx, p, 0x3A, g, 0);
        }
    }
    goto L7594;
L7590:
    frame = shown_old;
L7594:
    w8(ctx, frame + 0x24, static_cast<u8>(r8(ctx, g + gr::kSeq) + r8(ctx, g + 0xCE)));
    if (shown_old == 0) given = frame;
    else w32(ctx, shown_old + 0x20, frame);
L75b4:
    shown = frame;
    w32(ctx, frame + 0x20, 0);
    goto L7344;
L7348: // the frames given back once the game is past them
    while (given != 0 && r8(ctx, g + gr::kSeq) == r8(ctx, given + 0x24)) {
        const u32 r = given;
        given = r32(ctx, r + 0x20);
        if (given == 0) shown = 0;
        ga_free(ctx, g, r);
        if (given == 0) break;
    }
    if (frame != 0 && !(r32(ctx, g + 0x34) & 1)) {
        buf = r32(ctx, frame);
        pts = r64(ctx, frame + 8);
        status = r32(ctx, frame + 4);
        if (prev != 0) {
            if (shown_old == 0) {
                w32(ctx, prev, buf);
            } else {
                w8(ctx, shown_old + 0x24, static_cast<u8>(r8(ctx, g + gr::kSeq) + r8(ctx, g + 0xCE)));
                w32(ctx, prev, r32(ctx, shown_old));
            }
        }
    } else {
        pts = ~u64{0};
        status = 0;
        buf = 0;
    }
    w32(ctx, g + gr::kGiven, given);
    w32(ctx, g + gr::kHead, head);
    w32(ctx, g + gr::kShown, shown);
    w8(ctx, g + gr::kSeq, static_cast<u8>(r8(ctx, g + gr::kSeq) + 1));
    w8(ctx, g + gr::kHeld, static_cast<u8>(r14 != 0 ? 0 : keep));
    const u32 sess = r32(ctx, g + ga::kSession) & 0x3FF, tag = r32(ctx, g + gr::kTag);
    lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
    w64(ctx, info + 0x10, pts);
    w32(ctx, info, buf);
    w32(ctx, info + 4, sess);
    w32(ctx, info + 8, tag);
    w32(ctx, info + 0xC, status);
    return static_cast<s32>(ret);
}

// GetFrame(adapter, info): GetFrame2 without a previous frame, flags 2.
s32 cellSailGraphicsAdapterGetFrame(PPUContext& ctx, u32 g, u32 info)
{
    StackVar flip(ctx, gb(ctx), 16);
    return cellSailGraphicsAdapterGetFrame2(ctx, g, info, 0, flip.addr(), 2);
}

// The clock's video half: the time per frame from the game's reports, the first after 8 frames, later ones kept when
// from 15001 to 149999 microseconds (libsail's window: per - 0x3A99 below 0x20F57) and smoothed by a quarter.
void avsync_video(PPUContext& ctx, u32 c, u32 frame, u64 time, u32 record, u32 index)
{
    if (!frame) return;
    lv2_lib(ctx, "sys_lwmutex_lock", {c + 0xB0, 0});
    const u32 mark = r32(ctx, record);
    bool keep = true;
    if (r32(ctx, c + 0x28) == 0) {
        if (r64(ctx, c + 0x38) == 0) {
            if (r64(ctx, c + 0x30) != 0) {
                const u32 frames = frame - r32(ctx, c + 0x2C);
                if (frames < 8) {
                    keep = false;
                } else {
                    w32(ctx, c + 0x28, 1);
                    const u64 per = (time - r64(ctx, c + 0x30)) / frames;
                    w64(ctx, c + 0x38, per);
                    w64(ctx, c + 0x40, static_cast<u64>(static_cast<s64>(per * r64(ctx, c)) / 1000));
                }
            }
        } else {
            w32(ctx, c + 0x28, 1);
        }
    } else {
        const u64 per = (time - r64(ctx, c + 0x30)) / static_cast<u32>(frame - r32(ctx, c + 0x2C));
        if (per - 0x3A99 < 0x20F57) {
            const u64 smooth = (per + r64(ctx, c + 0x38) * 3) >> 2;
            w64(ctx, c + 0x38, smooth);
            w32(ctx, c + 0x28, 2);
            w64(ctx, c + 0x60, time + smooth * static_cast<u32>(mark - frame));
            w64(ctx, c + 0x40, static_cast<u64>(static_cast<s64>(smooth * r64(ctx, c)) / 1000));
        }
    }
    if (keep) {
        w32(ctx, c + 0x4C, index);
        w64(ctx, c + 0x30, time);
        w32(ctx, c + 0x2C, frame);
    }
    lv2_lib(ctx, "sys_lwmutex_unlock", {c + 0xB0});
}

void cellSailGraphicsAdapterUpdateAvSync(PPUContext& ctx, u32 a, u32 frame, u64 time)
{
    avsync_video(ctx, r32(ctx, a + 0x10), frame, time, a + ga::kSync, r32(ctx, a + 8));
}

// ------------------------------------------------------------------------------------------------- AU receiver

// The AU receiver (0x200 bytes): +0 its state (1 initialized), +4 the argument, +8 an lwmutex "_saiau0", +0x20 its
// stream callbacks (libsail's 9, copied) and +0x44 itself, +0xE8 the second argument. Both of libsail's optional
// arguments must be NULL: fw 4.92 takes no game callbacks here.
s32 cellSailAuReceiverInitialize(PPUContext& ctx, u32 a, u32 funcs, u32 funcs_arg, u32 arg)
{
    std::memset(gb(ctx) + a, 0, 0x200);
    if (funcs | funcs_arg) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const s32 r = lwmutex_create(ctx, a + 8, "_saiau0");
    if (r < 0) return r;
    std::memmove(gb(ctx) + a + 0x20, gb(ctx) + table("_pspocket_cellSailAuReceiverFuncs"), 0x24);
    w32(ctx, a + 0x44, a);
    w32(ctx, a, 1);
    w32(ctx, a + 0xE8, funcs_arg);
    w32(ctx, a + 4, arg);
    return lv2::CELL_OK;
}

s32 cellSailAuReceiverFinalize(PPUContext& ctx, u32 a)
{
    w32(ctx, a + 0x20 + 0x38, 8);
    lwmutex_destroy(ctx, a + 8);
    std::memset(gb(ctx) + a, 0, 0x200);
    return lv2::CELL_OK;
}

// cellSailAuReceiverGet(receiver, flags, session, time, AU out) (libsail's): the AU of the game's session (the low 16
// bits of the record's session compared) as a pointer to its record. AUs of earlier sessions are given back first.
// Answers: 28 not playing (or ended: once nothing is queued the slots are given back and the stop answered, 0x38), 1
// nothing queued (257 with the AU handed out before when flags bit 0 asks for it), 12 the next AU of a later session
// (268 with the one before). With a time: an AU not due yet is 12 (268 / 260 the one before, by its PTS), or 516 when
// flags bit 2 asks for AUs marked as flags bit 3 says and it is; due, the latest due AU with those before it given back
// (0x200 + given back << 8, | 8 none queued after it, | 4 the next not due; 528 / 784 without flags bit 1, which asks
// for the latest). Without a time (-1): the next AU (532), the one before (276) or the one after it (788) when it is
// not marked as asked.
s32 cellSailAuReceiverGet(PPUContext& ctx, u32 rcv, u32 flags, u32 session, u64 time, u32 out)
{
    const u32 lw = rcv + 8;
    const s32 lr = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {lw, 0}));
    if (lr < 0) return lr;
    const bool before = (flags & 1) != 0, latest = (flags & 2) != 0, marks = (flags & 4) != 0;
    const u64 mark = flags & 8;
    const auto marked = [&](u32 rec) { return r64(ctx, rec + 0x20) == mark; };
    u32 cur = r32(ctx, rcv + au::kCurrent), ret = 28, got = 0, rec = 0;
    const u32 mode = r32(ctx, rcv + au::kMode);
    if (mode == 0) goto done;
    if (mode != 1 && r32(ctx, rcv + au::kQueued) == 0) {
        au_release(ctx, rcv, au_other(cur));
        au_release(ctx, rcv, cur);
        w32(ctx, rcv + au::kMode, 0);
        receiver_post(ctx, rcv, 0x38, 0);
        w32(ctx, rcv, 2);
        goto done;
    }
    for (;;) {
        rec = au_slot(ctx, rcv, cur);
        if (rec == 0) {
            const u32 o = au_other(cur);
            if (before && au_held(ctx, rcv, o)) {
                ret = 257;
                got = au_held_record(ctx, rcv, o);
            } else {
                ret = 1;
            }
            goto done;
        }
        const u32 diff = session - r32(ctx, rec + 0xC);
        if ((diff & 0xFFFF) == 0) break;
        if (static_cast<s16>(diff) <= 0) {
            const u32 o = au_other(cur);
            if (before && au_held(ctx, rcv, o)) {
                ret = 268;
                got = au_held_record(ctx, rcv, o);
            } else {
                ret = 12;
            }
            goto done;
        }
        au_release(ctx, rcv, au_other(cur));
        au_release(ctx, rcv, cur);
    }
    if (time != ~u64{0}) {
        if (r64(ctx, rec + 0x10) > time) {
            if (marks && marked(rec)) {
                got = rec;
                ret = 516;
                cur = au_other(cur);
                au_release(ctx, rcv, cur);
                goto done;
            }
            const u32 o = au_other(cur);
            if (!before || !au_held(ctx, rcv, o)) {
                ret = 12;
                goto done;
            }
            got = au_held_record(ctx, rcv, o);
            ret = r64(ctx, got + 0x10) > time ? 268 : 260;
            goto done;
        }
        u32 prev = rec;
        for (u32 k = 0;; k++) {
            cur = au_other(cur);
            au_release(ctx, rcv, cur);
            const u32 next = au_take(ctx, rcv, cur);
            if (next == 0) {
                got = prev;
                ret = ((k << 8) + 512) | 8;
                goto done;
            }
            if (r64(ctx, next + 0x10) > time) {
                u32 given = k;
                got = prev;
                if (marks && marked(next)) {
                    cur ^= 1;
                    got = next;
                    given++;
                    au_release(ctx, rcv, cur);
                }
                ret = ((given << 8) + 512) | 4;
                goto done;
            }
            if (!latest) {
                if (marks && marked(next)) {
                    cur = au_other(cur);
                    got = next;
                    ret = 784;
                    au_release(ctx, rcv, cur);
                } else {
                    got = prev;
                    ret = 528;
                }
                goto done;
            }
            prev = next;
        }
    }
    if (!marks || marked(rec)) {
        got = rec;
        ret = 532;
        cur = au_other(cur);
        au_release(ctx, rcv, cur);
        goto done;
    }
    {
        const u32 o = au_other(cur);
        if (before && au_held(ctx, rcv, o)) {
            ret = 276;
            got = au_held_record(ctx, rcv, o);
            goto done;
        }
        cur = o;
        if (!latest) {
            got = rec;
            ret = 532;
            au_release(ctx, rcv, cur);
            goto done;
        }
        au_release(ctx, rcv, o);
        const u32 next = au_slot(ctx, rcv, o);
        if (next == 0) {
            ret = 532;
            got = au_held_record(ctx, rcv, au_other(o));
            goto done;
        }
        cur = au_other(o);
        got = next;
        ret = 788;
        au_release(ctx, rcv, cur);
    }
done:
    lv2_lib(ctx, "sys_lwmutex_unlock", {lw});
    w32(ctx, rcv + au::kCurrent, cur);
    w32(ctx, out, got);
    return static_cast<s32>(ret);
}

} // namespace

void register_cellSailAdapters(Registry& r)
{
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}
#define SAIL_INTERNAL(n)                                                                                               \
    ::pspocket::hle::Function{{}, "_pspocket_cellSail_" #n, recomp::nid_of("_pspocket_cellSail_" #n),                  \
        &detail::thunk<&_pspocket_cellSail_##n>, true}
    r.add("cellSail", {
        SAIL(cellSailSoundAdapterInitialize),
        SAIL(cellSailSoundAdapterFinalize),
        SAIL(cellSailSoundAdapterGetFormat),
        SAIL(cellSailSoundAdapterSetPreferredFormat),
        SAIL(cellSailSoundAdapterPtsToTimePosition),
        SAIL(cellSailSoundAdapterUpdateAvSync),
        SAIL(cellSailSoundAdapterGetFrame),
        SAIL(cellSailGraphicsAdapterInitialize),
        SAIL(cellSailGraphicsAdapterFinalize),
        SAIL(cellSailGraphicsAdapterGetFormat),
        SAIL(cellSailGraphicsAdapterSetPreferredFormat),
        SAIL(cellSailGraphicsAdapterPtsToTimePosition),
        SAIL(cellSailGraphicsAdapterUpdateAvSync),
        SAIL(cellSailGraphicsAdapterGetFrame),
        SAIL(cellSailGraphicsAdapterGetFrame2),
        SAIL(cellSailAuReceiverInitialize),
        SAIL(cellSailAuReceiverFinalize),
        SAIL(cellSailAuReceiverGet),
        SAIL_INTERNAL(auMakeup),
        SAIL_INTERNAL(auCleanup),
        SAIL_INTERNAL(auOpen),
        SAIL_INTERNAL(auClose),
        SAIL_INTERNAL(auStart),
        SAIL_INTERNAL(auStop),
        SAIL_INTERNAL(auCancel),
        SAIL_INTERNAL(auCheckout),
        SAIL_INTERNAL(auCheckin),
        SAIL_INTERNAL(saMakeup),
        SAIL_INTERNAL(saCleanup),
        SAIL_INTERNAL(saOpen),
        SAIL_INTERNAL(saClose),
        SAIL_INTERNAL(saStart),
        SAIL_INTERNAL(saStop),
        SAIL_INTERNAL(saCancel),
        SAIL_INTERNAL(saCheckout),
        SAIL_INTERNAL(saCheckin),
        SAIL_INTERNAL(gaMakeup),
        SAIL_INTERNAL(gaCleanup),
        SAIL_INTERNAL(gaOpen),
        SAIL_INTERNAL(gaClose),
        SAIL_INTERNAL(gaStart),
        SAIL_INTERNAL(gaStop),
        SAIL_INTERNAL(gaCancel),
        SAIL_INTERNAL(gaCheckout),
        SAIL_INTERNAL(gaCheckin),
    });
    r.add_variables("cellSail", {
        ::pspocket::hle::Variable{{}, "_pspocket_cellSailSoundAdapterRendererFuncs",
            recomp::nid_of("_pspocket_cellSailSoundAdapterRendererFuncs"), 36, 4, &init_sound_table},
        ::pspocket::hle::Variable{{}, "_pspocket_cellSailGraphicsAdapterRendererFuncs",
            recomp::nid_of("_pspocket_cellSailGraphicsAdapterRendererFuncs"), 36, 4, &init_graphics_table},
        ::pspocket::hle::Variable{{}, "_pspocket_cellSailAuReceiverFuncs",
            recomp::nid_of("_pspocket_cellSailAuReceiverFuncs"), 36, 4, &init_receiver_table},
    });
#undef SAIL_INTERNAL
#undef SAIL
}

} // namespace pspocket::hle
