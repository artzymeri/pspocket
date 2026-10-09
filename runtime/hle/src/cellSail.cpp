// cellSail: Sony's media player framework (libsail.sprx, fw 4.92), pspocket's own (D-562), read from libsail's code and
// compared with it running on pspocket (tests/hle/test_sail.cpp). RPCS3's cellSail.h gives the public structures' names
// and the error codes (rpcs3/Emu/Cell/Modules/cellSail.h); its functions answer without playing.
//
// Part 1 (this file so far): the objects a game fills in and hands to the player, and their calls into it.
// - CellSailMemAllocator: the two callbacks (alloc, free) copied, the argument after them.
// - CellSailFuture (0x18 bytes): an lv2 mutex at +0 and condition at +4 (both "_saifu0"), +8 the state (bit 31 set,
//   the waiters below it), +0xC the result. Get counts itself a waiter, waits until the result is set and passes the
//   wake-up on; Reset waits (in 100 ms slices) until no one waits.
// - CellSailSource (0x80 bytes): +0 the player, +4 the argument, +8 the 14 callbacks (CellSailSourceFuncs), +0x48 1
//   while initialized, +0x60 a pending call's handle (u64), +0x74 a diagnostic handler and +0x78 its argument. Its
//   notifications go to the diagnostic handler when there is one, else as messages to the player.
// - CellSailRendererAudio/Video: +0 the callbacks, +4 the argument, +8 the index the player gave it, +0x10 the player's
//   +8, +0x14 the player, +0x1C its state (9 finalized), +0x38 (audio, 0x18 bytes) or +0x40 (video, 0x20 bytes) the
//   format it wants. Its notifications are messages to the player.
// - The player's message queue (at the player's +0x170): +0 capacity, +4 messages queued, +8 the ring of 16-byte
//   messages {code, 0, object, argument} or {0, result, call handle (u64)}, +0xC where the next goes, +0x14 an lv2
//   mutex, +0x18 an lv2 condition. A post waits while the ring is full.
// - The MP4 and AVI calls go through the function tables libsail gets from the firmware's MP4 library (cellMp4) and
//   AVI library (cellSailAvi) when the player first opens such a file.
// - Eighteen exports RPCS3 does not name: sixteen answer -1, two are renderer notifications (cellSail_C00A450C,
//   cellSail_3E9B9F80). Five names RPCS3 lists (cellSailSourceNotify{Open,Close,Read,Start,Stop}Completed) fw 4.92's
//   libsail does not export: they log and answer CELL_SAIL_ERROR_NOT_AVAILABLE.

#include <cstring>
#include <format>
#include <string_view>

#include "cellSail.hpp"
#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace sail {

u64 lv2_lib(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find("sysPrxForUser", recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "sysPrxForUser::{}, which libsail imports, is not pspocket's", name);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 3;
    for (const u64 a : args) ctx.gpr[i++] = a;
    f->fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// ------------------------------------------------------------------------------------------------- the player's queue

// Waits for room in the queue (the mutex held).
void queue_wait_room(PPUContext& ctx, u32 qa)
{
    while (r32(ctx, qa + q::kCapacity) <= r32(ctx, qa + q::kCount)) sc(ctx, 107, {r32(ctx, qa + q::kCond), 0});
}

// The message written: the count raised, the player woken, the mutex released (its answer returned).
s32 queue_commit(PPUContext& ctx, u32 qa)
{
    w32(ctx, qa + q::kCount, r32(ctx, qa + q::kCount) + 1);
    sc(ctx, 108, {r32(ctx, qa + q::kCond)});
    return sc(ctx, 104, {r32(ctx, qa + q::kMutex)});
}

// The next message at the tail; the write index moved on.
u32 queue_tail(PPUContext& ctx, u32 qa)
{
    queue_wait_room(ctx, qa);
    const u32 at = r32(ctx, qa + q::kRing) + 16 * r32(ctx, qa + q::kWrite);
    return at;
}

void queue_advance_tail(PPUContext& ctx, u32 qa)
{
    const u32 cap = r32(ctx, qa + q::kCapacity), next = r32(ctx, qa + q::kWrite) + 1;
    w32(ctx, qa + q::kWrite, cap ? next % cap : 0);
}

s32 post(PPUContext& ctx, u32 player, u32 code, u32 object, u32 arg)
{
    const u32 qa = player + pl::kQueue;
    const s32 r = sc(ctx, 102, {r32(ctx, qa + q::kMutex), 0});
    if (r < 0) return r;
    const u32 at = queue_tail(ctx, qa);
    w32(ctx, at + 4, 0);
    w32(ctx, at + 12, arg);
    w32(ctx, at, code);
    w32(ctx, at + 8, object);
    queue_advance_tail(ctx, qa);
    return queue_commit(ctx, qa);
}

s32 post_urgent(PPUContext& ctx, u32 player, u32 code, u32 a, u32 b, u32 c)
{
    const u32 qa = player + pl::kQueue;
    const s32 r = sc(ctx, 102, {r32(ctx, qa + q::kMutex), 0});
    if (r < 0) return r;
    queue_wait_room(ctx, qa);
    const u32 cap = r32(ctx, qa + q::kCapacity), prev = cap + r32(ctx, qa + q::kRead) - 1;
    w32(ctx, qa + q::kRead, cap ? prev % cap : 0);
    const u32 at = r32(ctx, qa + q::kRing) + 16 * r32(ctx, qa + q::kRead);
    w32(ctx, at + 12, c);
    w32(ctx, at, code);
    w32(ctx, at + 4, a);
    w32(ctx, at + 8, b);
    return queue_commit(ctx, qa);
}

s32 post_completion(PPUContext& ctx, u32 player, s32 result, u64 handle)
{
    if (!handle) return static_cast<s32>(player);
    const u32 qa = player + pl::kQueue;
    const s32 r = sc(ctx, 102, {r32(ctx, qa + q::kMutex), 0});
    if (r < 0) return r;
    const u32 at = queue_tail(ctx, qa);
    w32(ctx, at, 0);
    w64(ctx, at + 8, handle);
    w32(ctx, at + 4, static_cast<u32>(result));
    queue_advance_tail(ctx, qa);
    return queue_commit(ctx, qa);
}

} // namespace sail

namespace {

using namespace sail;

// ------------------------------------------------------------------------------------------------- allocator

s32 cellSailMemAllocatorInitialize(PPUContext& ctx, u32 self, u32 callbacks, u32 arg)
{
    if (!self || !callbacks || !r32(ctx, callbacks) || !r32(ctx, callbacks + 4)) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + self, gb(ctx) + callbacks, 8);
    w32(ctx, self + 8, arg);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- future

namespace fut {
constexpr u32 kMutex = 0, kCond = 4, kState = 8, kResult = 0xC, kSize = 0x18;
constexpr u32 kDone = 0x80000000;
}

s32 cellSailFutureInitialize(PPUContext& ctx, u32 f)
{
    u8* b = gb(ctx);
    std::memset(b + f, 0, fut::kSize);
    StackVar v(ctx, b, 0x40);
    // sys_mutex_attribute_t (priority, not recursive, not shared, not adaptive; libsail leaves the IPC key as its stack
    // holds it) and sys_cond_attribute_t, both named "_saifu0".
    const u32 m = v.addr() + 0x18, c = v.addr();
    ps_write32(b, m, 2);
    ps_write32(b, m + 4, 0x20);
    ps_write32(b, m + 8, 0x200);
    ps_write32(b, m + 12, 0x2000);
    ps_write32(b, m + 0x18, 0);
    std::memcpy(b + m + 0x20, "_saifu0", 8);
    ps_write32(b, c, 0x200);
    ps_write32(b, c + 4, 0);
    std::memcpy(b + c + 0x10, "_saifu0", 8);
    s32 r = sc(ctx, 100, {f + fut::kMutex, m}); // sys_mutex_create
    if (r < 0) return r;
    r = sc(ctx, 105, {f + fut::kCond, r32(ctx, f + fut::kMutex), c}); // sys_cond_create
    if (r < 0) sc(ctx, 101, {r32(ctx, f + fut::kMutex)});
    return r;
}

s32 cellSailFutureFinalize(PPUContext& ctx, u32 f)
{
    sc(ctx, 106, {r32(ctx, f + fut::kCond)});
    return sc(ctx, 101, {r32(ctx, f + fut::kMutex)});
}

s32 cellSailFutureReset(PPUContext& ctx, u32 f, u8 wait)
{
    s32 r = sc(ctx, 102, {r32(ctx, f + fut::kMutex), 0});
    if (r < 0) return r;
    for (;;) {
        if ((r32(ctx, f + fut::kState) & ~fut::kDone) == 0) {
            w32(ctx, f + fut::kState, 0);
            break;
        }
        if (!wait) {
            r = error(CELL_SAIL_ERROR_INVALID_STATE);
            break;
        }
        r = sc(ctx, 107, {r32(ctx, f + fut::kCond), 100000}); // waits for the waiters to leave, 100 ms at a time
        if (r < 0) break;
    }
    sc(ctx, 104, {r32(ctx, f + fut::kMutex)});
    return r;
}

s32 cellSailFutureSet(PPUContext& ctx, u32 f, s32 result)
{
    s32 r = sc(ctx, 102, {r32(ctx, f + fut::kMutex), 0});
    if (r < 0) return r;
    const u32 state = r32(ctx, f + fut::kState);
    if (state & fut::kDone) {
        r = error(CELL_SAIL_ERROR_INVALID_STATE);
    } else {
        w32(ctx, f + fut::kResult, static_cast<u32>(result));
        w32(ctx, f + fut::kState, state | fut::kDone);
    }
    sc(ctx, 108, {r32(ctx, f + fut::kCond)});
    sc(ctx, 104, {r32(ctx, f + fut::kMutex)});
    return r;
}

s32 cellSailFutureGet(PPUContext& ctx, u32 f, u64 timeout, u32 result)
{
    s32 r = sc(ctx, 102, {r32(ctx, f + fut::kMutex), 0});
    if (r < 0) return r;
    w32(ctx, f + fut::kState, r32(ctx, f + fut::kState) + 1);
    for (;;) {
        if (r32(ctx, f + fut::kState) & fut::kDone) {
            r = lv2::CELL_OK;
            if (result) w32(ctx, result, r32(ctx, f + fut::kResult));
            break;
        }
        r = sc(ctx, 107, {r32(ctx, f + fut::kCond), timeout});
        if (r < 0) break;
    }
    w32(ctx, f + fut::kState, r32(ctx, f + fut::kState) - 1);
    sc(ctx, 108, {r32(ctx, f + fut::kCond)}); // the next waiter, or Reset
    sc(ctx, 104, {r32(ctx, f + fut::kMutex)});
    return r;
}

// 1 when the result is set (and it written to *result), 0 when not, or the lock's error.
s32 cellSailFutureIsDone(PPUContext& ctx, u32 f, u32 result)
{
    const s32 r = sc(ctx, 102, {r32(ctx, f + fut::kMutex), 0});
    if (r < 0) return r;
    const u32 done = r32(ctx, f + fut::kState) >> 31;
    sc(ctx, 104, {r32(ctx, f + fut::kMutex)});
    if (done && result) w32(ctx, result, r32(ctx, f + fut::kResult));
    return static_cast<s32>(done);
}

// ------------------------------------------------------------------------------------------------- source

namespace src {
constexpr u32 kPlayer = 0, kArg = 4, kFuncs = 8, kFuncsSize = 0x38, kState = 0x48, kPending = 0x60, kDiag = 0x74,
              kDiagArg = 0x78, kSize = 0x80;
}

s32 cellSailSourceInitialize(PPUContext& ctx, u32 s, u32 funcs, u32 arg)
{
    if (!funcs) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memset(gb(ctx) + s, 0, src::kSize);
    std::memmove(gb(ctx) + s + src::kFuncs, gb(ctx) + funcs, src::kFuncsSize);
    w32(ctx, s + src::kState, 1);
    w32(ctx, s + src::kArg, arg);
    return lv2::CELL_OK;
}

s32 cellSailSourceFinalize(PPUContext& ctx, u32 s)
{
    w32(ctx, s + src::kState, 0);
    return lv2::CELL_OK;
}

// The previous handler.
u32 cellSailSourceSetDiagHandler(PPUContext& ctx, u32 s, u32 handler, u32 arg)
{
    const u32 old = r32(ctx, s + src::kDiag);
    w32(ctx, s + src::kDiagArg, arg);
    w32(ctx, s + src::kDiag, handler);
    return old;
}

// A notification: to the diagnostic handler (arg, kind, a, b) when set, else the message to the player.
void source_notify(PPUContext& ctx, u32 s, u32 kind, s64 a, u64 b, u32 code, u32 arg)
{
    if (const u32 diag = r32(ctx, s + src::kDiag)) {
        call_guest(ctx, diag, {r32(ctx, s + src::kDiagArg), kind, static_cast<u64>(a), b});
        return;
    }
    post(ctx, r32(ctx, s + src::kPlayer), code, s, arg);
}

void cellSailSourceNotifyCallCompleted(PPUContext& ctx, u32 s, s32 result)
{
    if (const u32 diag = r32(ctx, s + src::kDiag)) {
        call_guest(ctx, diag, {r32(ctx, s + src::kDiagArg), 0, static_cast<u64>(static_cast<s64>(result)), 0});
        return;
    }
    const u64 pending = ps_read64(gb(ctx), s + src::kPending);
    if (!pending) {
        post(ctx, r32(ctx, s + src::kPlayer), 0x29, s, static_cast<u32>(result));
        return;
    }
    ps_write64(gb(ctx), s + src::kPending, 0);
    post_completion(ctx, r32(ctx, s + src::kPlayer), result, pending);
}

void cellSailSourceNotifyInputEos(PPUContext& ctx, u32 s) { source_notify(ctx, s, 1, 0, 0, 0x17, 0); }

void cellSailSourceNotifyStreamOut(PPUContext& ctx, u32 s, s32 streams)
{
    if (streams != 1) {
        // libsail stores to address 0 here, its assertion that one stream went out: the process faults.
        log_call_site(ctx, gb(ctx), std::format("cellSailSourceNotifyStreamOut({:#x}, {}): libsail's assertion (one "
            "stream) fails and it faults on a store to NULL", s, streams));
        PS_FATAL(kChan, "cellSailSourceNotifyStreamOut: {} streams (libsail faults)", streams);
    }
    source_notify(ctx, s, 2, streams, 0, 1, static_cast<u32>(streams));
}

void cellSailSourceNotifySessionError(PPUContext& ctx, u32 s, s32 err, u8 terminated)
{
    source_notify(ctx, s, 3, err, terminated, 0x20, static_cast<u32>(err) & ~(u32{terminated} << 31));
}

void cellSailSourceNotifyMediaStateChanged(PPUContext& ctx, u32 s, s32 state)
{
    source_notify(ctx, s, 4, state, 0, 0x2D, static_cast<u32>(state));
}

// RPCS3's names that fw 4.92's libsail does not export.
s32 not_exported(PPUContext& ctx, std::string_view name)
{
    log_call_site(ctx, gb(ctx), std::format("{}: fw 4.92's libsail has no such export: CELL_SAIL_ERROR_NOT_AVAILABLE",
        name));
    return error(CELL_SAIL_ERROR_NOT_AVAILABLE);
}
s32 cellSailSourceNotifyOpenCompleted(PPUContext& ctx) { return not_exported(ctx, __func__); }
s32 cellSailSourceNotifyStartCompleted(PPUContext& ctx) { return not_exported(ctx, __func__); }
s32 cellSailSourceNotifyStopCompleted(PPUContext& ctx) { return not_exported(ctx, __func__); }
s32 cellSailSourceNotifyReadCompleted(PPUContext& ctx) { return not_exported(ctx, __func__); }
s32 cellSailSourceNotifyCloseCompleted(PPUContext& ctx) { return not_exported(ctx, __func__); }

// ------------------------------------------------------------------------------------------------- renderers

namespace rnd {
constexpr u32 kFuncs = 0, kArg = 4, kPlayer = 0x14, kState = 0x1C, kAudioFormat = 0x38, kVideoFormat = 0x40;
constexpr u32 kFinalized = 9;
}

// Initialize(renderer, funcs, arg, attr): attr NULL or {8, the format or NULL}; no format: all 0xFF (video: the first
// byte 0 after).
s32 renderer_initialize(PPUContext& ctx, u32 r, u32 funcs, u32 arg, u32 attr, u32 format_at, u32 format_size, bool video)
{
    u8* b = gb(ctx);
    u32 format = 0;
    if (attr) {
        if (r32(ctx, attr) != 8) return error(CELL_SAIL_ERROR_INVALID_ARG);
        format = r32(ctx, attr + 4);
    }
    w32(ctx, r + rnd::kFuncs, funcs);
    w32(ctx, r + rnd::kArg, arg);
    if (format) {
        std::memmove(b + r + format_at, b + format, format_size);
        return lv2::CELL_OK;
    }
    std::memset(b + r + format_at, 0xFF, format_size);
    if (video) b[r + format_at] = 0;
    return lv2::CELL_OK;
}

s32 cellSailRendererAudioInitialize(PPUContext& ctx, u32 r, u32 funcs, u32 arg, u32 attr)
{
    return renderer_initialize(ctx, r, funcs, arg, attr, rnd::kAudioFormat, 0x18, false);
}

s32 cellSailRendererVideoInitialize(PPUContext& ctx, u32 r, u32 funcs, u32 arg, u32 attr)
{
    return renderer_initialize(ctx, r, funcs, arg, attr, rnd::kVideoFormat, 0x20, true);
}

s32 cellSailRendererAudioFinalize(PPUContext& ctx, u32 r)
{
    w32(ctx, r + rnd::kState, rnd::kFinalized);
    return lv2::CELL_OK;
}

s32 cellSailRendererVideoFinalize(PPUContext& ctx, u32 r)
{
    w32(ctx, r + rnd::kState, rnd::kFinalized);
    return lv2::CELL_OK;
}

void renderer_post(PPUContext& ctx, u32 r, u32 code, u32 arg) { post(ctx, r32(ctx, r + rnd::kPlayer), code, r, arg); }

void cellSailRendererAudioNotifyCallCompleted(PPUContext& ctx, u32 r, s32 result)
{
    renderer_post(ctx, r, 0x33, static_cast<u32>(result));
}
void cellSailRendererAudioNotifyFrameDone(PPUContext& ctx, u32 r) { renderer_post(ctx, r, 0x12, 0); }
void cellSailRendererAudioNotifyOutputEos(PPUContext& ctx, u32 r) { renderer_post(ctx, r, 0x1D, 0); }
void cellSail_C00A450C(PPUContext& ctx, u32 r, u64 arg) { renderer_post(ctx, r, 0x11, static_cast<u32>(arg)); }
void cellSailRendererVideoNotifyCallCompleted(PPUContext& ctx, u32 r, s32 result)
{
    renderer_post(ctx, r, 0x34, static_cast<u32>(result));
}
void cellSailRendererVideoNotifyFrameDone(PPUContext& ctx, u32 r) { renderer_post(ctx, r, 0x14, 0); }
void cellSailRendererVideoNotifyOutputEos(PPUContext& ctx, u32 r) { renderer_post(ctx, r, 0x1E, 0); }
void cellSail_3E9B9F80(PPUContext& ctx, u32 r, u64 arg) { renderer_post(ctx, r, 0x13, static_cast<u32>(arg)); }

// ------------------------------------------------------------------------------------------------- MP4 and AVI

// The function tables libsail keeps for the firmware's MP4 library (cellMp4) and AVI library (cellSailAvi), filled when
// the player first opens such a file: guest words, so a call goes through the entry at `slot`.
struct SailTables {
    u32 mp4 = 0, avi = 0;
};

s32 forward(PPUContext& ctx, bool avi, u32 slot, std::string_view name)
{
    const SailTables& t = HleProcess::current()->state<SailTables>();
    const u32 table = avi ? t.avi : t.mp4;
    if (!table) {
        log_call_site(ctx, gb(ctx), std::format("{}: no {} library opened yet (libsail calls through a NULL table and "
            "faults): CELL_SAIL_ERROR_INVALID_STATE", name, avi ? "AVI" : "MP4"));
        return error(CELL_SAIL_ERROR_INVALID_STATE);
    }
    return static_cast<s32>(call_guest(ctx, r32(ctx, table + slot),
        {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8]}));
}

#define SAIL_FORWARD(fn, avi, slot)                                                                                    \
    s32 fn(PPUContext& ctx) { return forward(ctx, avi, slot, #fn); }
SAIL_FORWARD(cellSailMp4MovieGetBrand, false, 0xA4)
SAIL_FORWARD(cellSailMp4MovieIsCompatibleBrand, false, 0xA8)
SAIL_FORWARD(cellSailMp4MovieGetMovieInfo, false, 0xAC)
SAIL_FORWARD(cellSailMp4MovieGetTrackByIndex, false, 0xB0)
SAIL_FORWARD(cellSailMp4MovieGetTrackById, false, 0xB4)
SAIL_FORWARD(cellSailMp4MovieGetTrackByTypeAndIndex, false, 0xB8)
SAIL_FORWARD(cellSailMp4TrackGetTrackInfo, false, 0xBC)
SAIL_FORWARD(cellSailMp4TrackGetTrackReferenceCount, false, 0xC0)
SAIL_FORWARD(cellSailMp4TrackGetTrackReference, false, 0xC4)
SAIL_FORWARD(cellSailAviMovieGetMovieInfo, true, 0xA4)
SAIL_FORWARD(cellSailAviMovieGetStreamByIndex, true, 0xA8)
SAIL_FORWARD(cellSailAviMovieGetStreamByTypeAndIndex, true, 0xAC)
SAIL_FORWARD(cellSailAviMovieGetHeader, true, 0xB0)
SAIL_FORWARD(cellSailAviStreamGetMediaType, true, 0xB4)
SAIL_FORWARD(cellSailAviStreamGetHeader, true, 0xB8)
#undef SAIL_FORWARD

// ------------------------------------------------------------------------------------------------- unnamed

// Exports with no name and no body but an answer of -1.
u64 sail_minus_one(PPUContext&) { return ~u64{0}; }

} // namespace

void register_cellSailPlayer(Registry& r);
void register_cellSailDescriptor(Registry& r);
void register_cellSailAdapters(Registry& r);
void register_cellSailControl(Registry& r);
void register_cellSailPipelines(Registry& r);

void register_cellSail(Registry& r)
{
    register_cellSailPlayer(r);
    register_cellSailDescriptor(r);
    register_cellSailAdapters(r);
    register_cellSailControl(r);
    register_cellSailPipelines(r);
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}
#define SAIL_MINUS_ONE(nid) ::pspocket::hle::Function{{}, "cellSail_" #nid, 0x##nid, &detail::thunk<&sail_minus_one>, false}
    r.add("cellSail", {
        SAIL(cellSailMemAllocatorInitialize),
        SAIL(cellSailFutureInitialize),
        SAIL(cellSailFutureFinalize),
        SAIL(cellSailFutureReset),
        SAIL(cellSailFutureSet),
        SAIL(cellSailFutureGet),
        SAIL(cellSailFutureIsDone),
        SAIL(cellSailSourceInitialize),
        SAIL(cellSailSourceFinalize),
        SAIL(cellSailSourceSetDiagHandler),
        SAIL(cellSailSourceNotifyCallCompleted),
        SAIL(cellSailSourceNotifyInputEos),
        SAIL(cellSailSourceNotifyStreamOut),
        SAIL(cellSailSourceNotifySessionError),
        SAIL(cellSailSourceNotifyMediaStateChanged),
        SAIL(cellSailSourceNotifyOpenCompleted),
        SAIL(cellSailSourceNotifyStartCompleted),
        SAIL(cellSailSourceNotifyStopCompleted),
        SAIL(cellSailSourceNotifyReadCompleted),
        SAIL(cellSailSourceNotifyCloseCompleted),
        SAIL(cellSailRendererAudioInitialize),
        SAIL(cellSailRendererAudioFinalize),
        SAIL(cellSailRendererAudioNotifyCallCompleted),
        SAIL(cellSailRendererAudioNotifyFrameDone),
        SAIL(cellSailRendererAudioNotifyOutputEos),
        SAIL(cellSailRendererVideoInitialize),
        SAIL(cellSailRendererVideoFinalize),
        SAIL(cellSailRendererVideoNotifyCallCompleted),
        SAIL(cellSailRendererVideoNotifyFrameDone),
        SAIL(cellSailRendererVideoNotifyOutputEos),
        SAIL(cellSailMp4MovieGetBrand),
        SAIL(cellSailMp4MovieIsCompatibleBrand),
        SAIL(cellSailMp4MovieGetMovieInfo),
        SAIL(cellSailMp4MovieGetTrackByIndex),
        SAIL(cellSailMp4MovieGetTrackById),
        SAIL(cellSailMp4MovieGetTrackByTypeAndIndex),
        SAIL(cellSailMp4TrackGetTrackInfo),
        SAIL(cellSailMp4TrackGetTrackReferenceCount),
        SAIL(cellSailMp4TrackGetTrackReference),
        SAIL(cellSailAviMovieGetMovieInfo),
        SAIL(cellSailAviMovieGetStreamByIndex),
        SAIL(cellSailAviMovieGetStreamByTypeAndIndex),
        SAIL(cellSailAviMovieGetHeader),
        SAIL(cellSailAviStreamGetMediaType),
        SAIL(cellSailAviStreamGetHeader),
        PS_HLE_FUNC(0xC00A450C, cellSail_C00A450C),
        PS_HLE_FUNC(0x3E9B9F80, cellSail_3E9B9F80),
        SAIL_MINUS_ONE(D5E100E7),
        SAIL_MINUS_ONE(7224C0F1),
        SAIL_MINUS_ONE(4B5A94A5),
        SAIL_MINUS_ONE(197EE602),
        SAIL_MINUS_ONE(86DE3AA7),
        SAIL_MINUS_ONE(5367B57E),
        SAIL_MINUS_ONE(9279F3D9),
        SAIL_MINUS_ONE(EC695E87),
        SAIL_MINUS_ONE(3BE9899B),
        SAIL_MINUS_ONE(A56F7CC5),
        SAIL_MINUS_ONE(5FAC53DD),
        SAIL_MINUS_ONE(2692816B),
        SAIL_MINUS_ONE(CF03D3BA),
        SAIL_MINUS_ONE(D7D96074),
        SAIL_MINUS_ONE(320887CC),
        SAIL_MINUS_ONE(0CCD506A),
    });
#undef SAIL_MINUS_ONE
#undef SAIL
}

} // namespace pspocket::hle
