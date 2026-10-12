// cellSailRec's composers (D-918, part 6), as libsail_rec (fw 4.92) drives them: the recorder's composers table
// (+0x770), through which every stream operation goes to the composer in use by its table of 24 functions, and the
// table libsail_rec gives the game's composer (CellSailComposer, type 3), whose functions call the game's callbacks and
// keep each stream's ring of access units for cellSailComposerGet/TryGet/Release (cellSailRecObjects.cpp).
//
// The composers table holds a table and an argument per type: the game's composer is registered when OpenStream
// attaches it (its makeup callback first), the MP4 composer (type 1, libmp4's) when libmp4 makes it. Opening allocates
// the composer's instance (the size its table asks) and keeps the type in use; each stream added allocates the
// composer's stream object for its entry (0x1C bytes in the recorder's slot) and links the entry; closing frees them.
// Every call is asynchronous: it ends with a completion posted to the recorder's queue for the handle it was given
// (libsail_rec's own, or the game's composer through cellSailComposerNotifyCallCompleted), which the control thread's
// continuation engine takes (cellSailRecControl.cpp).
//
// The game's composer's states: 1 closed, 2 open, 3 recording, 4 opening, 5 closing, 6 starting, 7 stopping; its
// streams' (cellSailRec.hpp so::): 10 added, 12 recording, 11 ended, 13 ended with no access unit left, 9 removed. A
// stream's access units arrive from the pipelines (the composer table's AU functions): put in the stream's ring while
// it records and the game told (its callback for the kind), or given straight back (a release message) when the game
// has no callback or the stream does not record. Stopping ends every recording stream; the stop completes, with the
// game's stop callback, when the last stream has ended and given back its last unit.

#include <cstring>
#include <format>

#include "cellSailRec.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

// A composer table's function `index` (its descriptor at table + 4 * index), called with `args`.
u64 table_call(PPUContext& ctx, u32 table, u32 index, std::initializer_list<u64> args)
{
    const u32 at = table + 4 * index;
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, at, 4) || !guest_readable(t, r32(ctx, at), 8)) {
        log_call_site(ctx, gb(ctx),
            std::format("cellSailRec: the composer table at {:#x} has no function {} (libsail_rec faults here)", table,
                index));
        PS_FATAL(kChan, "the composer table at {:#x} has no function {}: libsail_rec faults here", table, index);
    }
    return call_guest(ctx, r32(ctx, at), args);
}

// The table of the composer in use (the type at +0x2C; libsail_rec reads the word before the tables for type -1).
u32 table_in_use(PPUContext& ctx, u32 cs)
{
    const s32 type = static_cast<s32>(r32(ctx, cs + cs::kType));
    return r32(ctx, cs + static_cast<u32>(type * 4));
}

u32 recorder_of(PPUContext& ctx, u32 cs) { return r32(ctx, cs + cs::kRecorder); }

} // namespace

// ------------------------------------------------------------------------------------------------- the composers table

s32 composer_register(PPUContext& ctx, u32 cs, u32 type, u64 table, u32 arg)
{
    if (!table || type >= 4) return error(sail::CELL_SAIL_ERROR_INVALID_ARG);
    if (r32(ctx, cs + 4 * type)) return error(CELL_SAIL_ERROR_USING);
    w32(ctx, cs + cs::kArgs + 4 * type, arg);
    w32(ctx, cs + 4 * type, static_cast<u32>(table));
    return 0;
}

s32 composer_unregister(PPUContext& ctx, u32 cs, u32 type, u32 table_out, u32 arg_out)
{
    if (type >= 4) return error(sail::CELL_SAIL_ERROR_INVALID_ARG);
    if (!r32(ctx, cs + 4 * type)) return error(CELL_SAIL_ERROR_EMPTY);
    if (table_out) w32(ctx, table_out, r32(ctx, cs + 4 * type));
    if (arg_out) w32(ctx, arg_out, r32(ctx, cs + cs::kArgs + 4 * type));
    w32(ctx, cs + 4 * type, 0);
    w32(ctx, cs + cs::kArgs + 4 * type, 0);
    return 0;
}

void composers_reset(PPUContext& ctx, u32 cs, u32 rec)
{
    std::memset(gb(ctx) + cs, 0, cs::kSize);
    w32(ctx, cs + cs::kType, 0xFFFFFFFF);
    w32(ctx, cs + cs::kRecorder, rec);
}

void composer_completed(PPUContext& ctx, u32 cs, s32 status)
{
    table_call(ctx, table_in_use(ctx, cs), cs::kCompleted, {r32(ctx, cs + cs::kInstance), sx32(static_cast<u32>(status))});
}

void composer_open(PPUContext& ctx, u32 cs, u32 type, u32 path, u32 profile_type, u64 handle)
{
    const u32 rec = recorder_of(ctx, cs);
    if (type > 3) {
        post_completion(ctx, rec, error(sail::CELL_SAIL_ERROR_INVALID_ARG), handle);
        return;
    }
    const u32 table = r32(ctx, cs + 4 * type), arg = r32(ctx, cs + cs::kArgs + 4 * type);
    const u32 size = static_cast<u32>(
        table_call(ctx, table, cs::kInstanceSize, {arg, profile_type, handle, path, profile_type, handle}));
    const u32 instance = alloc(ctx, rec, 8, size);
    w32(ctx, cs + cs::kInstance, instance);
    if (!instance) {
        post_completion(ctx, rec, error(CELL_SAIL_ERROR_MEMORY), handle);
        return;
    }
    w32(ctx, cs + cs::kType, type);
    table_call(ctx, r32(ctx, cs + 4 * type), cs::kOpen, {arg, instance, size, 0, path, profile_type, handle});
}

void composer_close(PPUContext& ctx, u32 cs, u64 handle)
{
    const u32 instance = r32(ctx, cs + cs::kInstance);
    if (static_cast<s32>(r32(ctx, cs + cs::kType)) >= 0 && instance) {
        table_call(ctx, table_in_use(ctx, cs), cs::kClose, {instance, handle, handle});
        return;
    }
    post_completion(ctx, recorder_of(ctx, cs), 0, handle);
}

void composer_remove(PPUContext& ctx, u32 cs, u32 entry, u64 handle, bool video)
{
    const u32 instance = r32(ctx, cs + cs::kInstance);
    if (static_cast<s32>(r32(ctx, cs + cs::kType)) >= 0 && instance && r32(ctx, entry + se::kObject)) {
        table_call(ctx, table_in_use(ctx, cs), video ? cs::kRemoveVideo : cs::kRemoveAudio, {instance, entry, handle});
        return;
    }
    post_completion(ctx, recorder_of(ctx, cs), 0, handle);
}

void composer_free_stream(PPUContext& ctx, u32 cs, u32 entry)
{
    if (static_cast<s32>(r32(ctx, cs + cs::kType)) < 0 || !r32(ctx, cs + cs::kInstance) || !r32(ctx, entry + se::kObject))
        return;
    dealloc(ctx, recorder_of(ctx, cs), 8, r32(ctx, entry + se::kObject));
    u32 cur = r32(ctx, cs + cs::kStreams);
    if (entry == cur) {
        w32(ctx, cs + cs::kStreams, r32(ctx, entry + se::kNext));
        w32(ctx, cs + cs::kCount, r32(ctx, cs + cs::kCount) - 1);
        return;
    }
    // libsail_rec walks the list from its head (an entry not in it would read past the list's end: never so).
    while (cur) {
        const u32 next = r32(ctx, cur + se::kNext);
        if (entry == next) {
            w32(ctx, cur + se::kNext, r32(ctx, entry + se::kNext));
            w32(ctx, cs + cs::kCount, r32(ctx, cs + cs::kCount) - 1);
            return;
        }
        cur = next;
    }
}

void composer_detach_instance(PPUContext& ctx, u32 cs)
{
    if (static_cast<s32>(r32(ctx, cs + cs::kType)) >= 0) w32(ctx, cs + cs::kType, 0xFFFFFFFF);
    if (const u32 instance = r32(ctx, cs + cs::kInstance)) {
        dealloc(ctx, recorder_of(ctx, cs), 8, instance);
        w32(ctx, cs + cs::kInstance, 0);
    }
}

void composer_add(PPUContext& ctx, u32 cs, u32 entry, u32 params, u32 pipeline, u32 flag, u64 handle, bool video)
{
    const u32 rec = recorder_of(ctx, cs);
    std::memset(gb(ctx) + entry, 0, se::kSize);
    const u32 size = static_cast<u32>(table_call(ctx, table_in_use(ctx, cs), cs::kStreamSize,
        {r32(ctx, cs + cs::kInstance), video ? 1u : 0u, params}));
    const u32 object = alloc(ctx, rec, 8, size);
    if (!object) {
        post_completion(ctx, rec, error(CELL_SAIL_ERROR_MEMORY), handle);
        return;
    }
    std::memset(gb(ctx) + object, 0, size);
    w32(ctx, entry + se::kNext, r32(ctx, cs + cs::kStreams));
    w32(ctx, entry + se::kObject, object);
    w32(ctx, entry + se::kPipeline, pipeline);
    w32(ctx, entry + se::kFlag, flag);
    w32(ctx, cs + cs::kStreams, entry);
    w32(ctx, cs + cs::kCount, r32(ctx, cs + cs::kCount) + 1);
    table_call(ctx, table_in_use(ctx, cs), video ? cs::kAddVideo : cs::kAddAudio,
        {r32(ctx, cs + cs::kInstance), entry, params, handle});
}

void composer_start(PPUContext& ctx, u32 cs, u32 path, u32 session, u64 handle)
{
    table_call(ctx, table_in_use(ctx, cs), cs::kStart, {r32(ctx, cs + cs::kInstance), path, session, handle});
}

void composer_stop(PPUContext& ctx, u32 cs, u32 flag, u64 handle)
{
    table_call(ctx, table_in_use(ctx, cs), cs::kStop, {r32(ctx, cs + cs::kInstance), flag, handle});
}

void composer_end(PPUContext& ctx, u32 cs, u32 entry, u64 handle, bool video)
{
    table_call(ctx, table_in_use(ctx, cs), video ? cs::kEndVideo : cs::kEndAudio,
        {r32(ctx, cs + cs::kInstance), entry, handle});
}

void composer_capacity(PPUContext& ctx, u32 cs, u32 entry)
{
    w32(ctx, entry + se::kCapacity,
        static_cast<u32>(table_call(ctx, table_in_use(ctx, cs), cs::kCapacity, {r32(ctx, cs + cs::kInstance), entry})));
}

void composer_released(PPUContext& ctx, u32 cs, u32 entry, u32 n)
{
    w32(ctx, entry + se::kCapacity, r32(ctx, entry + se::kCapacity) + n);
    table_call(ctx, table_in_use(ctx, cs), cs::kReleased, {r32(ctx, cs + cs::kInstance), entry, n});
}

s32 composer_au(PPUContext& ctx, u32 cs, u32 entry, u32 au, bool video)
{
    const s32 r = static_cast<s32>(static_cast<u32>(table_call(ctx, table_in_use(ctx, cs),
        video ? cs::kVideoAu : cs::kAudioAu, {r32(ctx, cs + cs::kInstance), entry, au})));
    if (r >= 0) w32(ctx, entry + se::kCapacity, r32(ctx, entry + se::kCapacity) - 1);
    return r;
}

// ------------------------------------------------------------------------------------------------- the game's composer

namespace {

u32 game_cb(PPUContext& ctx, u32 c, u32 index) { return r32(ctx, c + cp::kFuncs + 4 * index); }
u32 game_arg(PPUContext& ctx, u32 c) { return r32(ctx, c + cp::kArg); }

// The pending call completed (cellSailComposerNotifyCallCompleted): the diagnostic handler (0, the result, 0) when the
// composer holds one, else the completion for the handle the composer keeps (once); r3 afterwards: the handler's
// answer, the queue's unlock, or the composer as given when nothing is pending.
u64 notify_completed(PPUContext& ctx, u64 composer, s32 result)
{
    const u32 c = static_cast<u32>(composer);
    if (const u32 diag = r32(ctx, c + cp::kDiag))
        return call_guest(ctx, diag, {r32(ctx, c + cp::kDiagArg), 0, sx32(static_cast<u32>(result)), 0});
    const u64 handle = r64(ctx, c + cp::kPending);
    w64(ctx, c + cp::kPending, 0);
    if (!handle) return composer;
    return sx32(static_cast<u32>(post_completion(ctx, r32(ctx, c + cp::kRecorder), result, handle)));
}

// A stream ended (state 11) with no unit left: done (13); the stop completes with the last one (the game's stop
// callback with the stop's result, or at once). `pass` is what the game's stop callback finds in r5 (libsail_rec's
// caller's word: 1 an audio stream ended by the stop, 2 units given back, else 0).
void stream_check_done(PPUContext& ctx, u32 c, u32 obj, u32 pass)
{
    if (r32(ctx, obj + so::kState) != so::kEnded || r32(ctx, obj + so::kCount)) return;
    w32(ctx, obj + so::kState, so::kDone);
    const u32 left = r32(ctx, c + cp::kRecording) - 1;
    w32(ctx, c + cp::kRecording, left);
    if (left) return;
    w32(ctx, c + cp::kState, 7);
    if (const u32 cb = game_cb(ctx, c, cp::kStop)) {
        call_guest(ctx, cb, {game_arg(ctx, c), sx32(r32(ctx, c + cp::kStopResult)), pass});
        return;
    }
    notify_completed(ctx, c, 0);
}

// A stream's ring size by its parameters: their units in the stream parameters' time, and 4 more.
void stream_ring(PPUContext& ctx, u32 c, u32 obj, u32 x, u32 y)
{
    std::memset(gb(ctx) + obj + so::kRing, 0, 0x14);
    const u32 sp = r32(ctx, c + cp::kStreamParam);
    w32(ctx, obj + so::kRing, obj + so::kRingStart);
    const u64 num = u64{y} * r32(ctx, sp + 8);
    const u32 den = x * r32(ctx, sp + 4);
    w32(ctx, obj + so::kRingSize, static_cast<u32>(divu64(num, den)) + 4);
}

// Adding a stream: kept in the composer's table for its kind, its parameters copied for the game, its ring sized, the
// game's callback told (its index and the parameters), completed.
void add_stream(PPUContext& ctx, u32 instance, u32 entry, u32 params, u64 handle, bool video)
{
    const u32 c = r32(ctx, instance);
    const u32 count_at = c + (video ? cp::kVideoCount : cp::kAudioCount);
    const u32 index = r32(ctx, count_at);
    const u32 obj = r32(ctx, entry + se::kObject);
    w32(ctx, c + (video ? cp::kVideo : cp::kAudio) + 4 * index, entry);
    w32(ctx, count_at, index + 1);
    w32(ctx, obj + so::kEntry, entry);
    w32(ctx, obj + so::kIndex, index);
    std::memset(gb(ctx) + obj + so::kParams, 0, 0x50);
    w32(ctx, obj + 0x18, r32(ctx, params + 0x14));
    if (video) {
        w32(ctx, obj + 0x20, r32(ctx, params + 0x20));
        w32(ctx, obj + 0x24, r32(ctx, params + 0x24));
        w32(ctx, obj + 0x28, r32(ctx, params + 0x2C));
        w32(ctx, obj + 0x2C, r32(ctx, params + 0x30));
        w32(ctx, obj + 0x30, r32(ctx, params + 0x34));
        w32(ctx, obj + 0x34, r32(ctx, params + 0x38));
        w64(ctx, obj + 0x40, r64(ctx, params + 0x80));
        w64(ctx, obj + 0x48, r64(ctx, params + 0x88));
        w64(ctx, obj + 0x50, r64(ctx, params + 0x90));
        w32(ctx, obj + 0x60, params + 0xA8);
        w64(ctx, obj + 0x58, r64(ctx, params + 0x98));
        w32(ctx, obj + so::kTimescale, r32(ctx, params + 0x34));
        stream_ring(ctx, c, obj, r32(ctx, params + 0xA0), r32(ctx, params + 0x34));
    } else {
        w32(ctx, obj + 0x20, r32(ctx, params + 0x1C));
        w32(ctx, obj + 0x24, r32(ctx, params + 0x20));
        w32(ctx, obj + 0x28, r32(ctx, params + 0x28));
        w32(ctx, obj + 0x2C, r32(ctx, params + 0x2C));
        w32(ctx, obj + 0x30, r32(ctx, params + 0x30));
        w64(ctx, obj + 0x40, r64(ctx, params + 0x68));
        w64(ctx, obj + 0x48, r64(ctx, params + 0x70));
        w32(ctx, obj + 0x58, params + 0x88);
        w64(ctx, obj + 0x50, r64(ctx, params + 0x78));
        w32(ctx, obj + so::kTimescale, r32(ctx, params + 0x30));
        stream_ring(ctx, c, obj, r32(ctx, params + 0x80), r32(ctx, params + 0x30));
    }
    w32(ctx, obj + so::kState, so::kAdded);
    if (const u32 cb = game_cb(ctx, c, video ? cp::kAddVideo : cp::kAddAudio))
        call_guest(ctx, cb, {game_arg(ctx, c), sx32(index), obj + so::kParams});
    post_completion(ctx, r32(ctx, c + cp::kRecorder), 0, handle);
}

// Removing a stream: removed (9), out of the composer's table at its index, completed.
void remove_stream(PPUContext& ctx, u32 instance, u32 entry, u64 handle, u32 count, u32 table)
{
    const u32 obj = r32(ctx, entry + se::kObject), c = r32(ctx, instance);
    const u32 index = r32(ctx, obj + so::kIndex);
    w32(ctx, obj + so::kState, so::kRemoved);
    w32(ctx, c + count, r32(ctx, c + count) - 1);
    w32(ctx, obj + so::kEntry, entry);
    w32(ctx, c + table + 4 * index, 0);
    post_completion(ctx, r32(ctx, c + cp::kRecorder), 0, handle);
}

// Ending a stream: ended (11), the game's callback for its kind told its index (the handle in r5), done once it has
// no unit left.
void end_stream(PPUContext& ctx, u32 instance, u32 entry, u64 handle, u32 callback, u32 pass)
{
    const u32 c = r32(ctx, instance), obj = r32(ctx, entry + se::kObject);
    w64(ctx, c + cp::kPending, handle);
    w32(ctx, obj + so::kEntry, entry);
    w32(ctx, obj + so::kState, so::kEnded);
    if (callback != ~0u)
        if (const u32 cb = game_cb(ctx, c, callback))
            call_guest(ctx, cb, {game_arg(ctx, c), sx32(r32(ctx, obj + so::kIndex)), handle});
    stream_check_done(ctx, c, obj, pass);
}

// A stream's access unit from the pipeline: in the ring while the stream records, the game told by its callback for
// the kind (none for user streams); given back at once (the release message) otherwise.
s32 stream_au(PPUContext& ctx, u32 instance, u32 entry, u32 au, u32 lock_at, u32 callback, u32 release_code)
{
    const u32 c = r32(ctx, instance), obj = r32(ctx, entry + se::kObject);
    w32(ctx, obj + so::kEntry, entry);
    const s32 r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_lock", {c + lock_at, 0}));
    if (r < 0) return r;
    const u32 rec = r32(ctx, c + cp::kRecorder);
    if (r32(ctx, obj + so::kState) == so::kRecording) {
        const u32 w = r32(ctx, obj + so::kWrite), size = r32(ctx, obj + so::kRingSize), next = w + 1;
        w32(ctx, obj + so::kCount, r32(ctx, obj + so::kCount) + 1);
        w32(ctx, r32(ctx, obj + so::kRing) + 4 * w, au);
        w32(ctx, obj + so::kWrite, next - divu(next, size) * size);
        const u32 cb = callback == ~0u ? 0 : game_cb(ctx, c, callback);
        if (cb) call_guest(ctx, cb, {game_arg(ctx, c), sx32(r32(ctx, obj + so::kIndex)), 1});
        else post(ctx, rec, release_code, entry, 1);
    } else {
        post(ctx, rec, release_code, entry, 1);
    }
    sail::lv2_lib(ctx, "sys_lwmutex_unlock", {c + lock_at});
    return r;
}

// The table's functions for the game's composer.

u64 _pspocket_cellSailRec_ComposerMakeup(PPUContext& ctx, u32 c)
{
    const u32 cb = game_cb(ctx, c, cp::kMakeup);
    return cb ? sx32(static_cast<u32>(call_guest(ctx, cb, {game_arg(ctx, c)}))) : 0;
}

u64 _pspocket_cellSailRec_ComposerCleanup(PPUContext& ctx, u32 c)
{
    const u32 cb = game_cb(ctx, c, cp::kCleanup);
    return cb ? sx32(static_cast<u32>(call_guest(ctx, cb, {game_arg(ctx, c)}))) : 0;
}

u64 _pspocket_cellSailRec_ComposerInstanceSize(PPUContext&) { return 0x10; }

// The stream object's size for a kind (0 audio, 1 video): its ring's units for the parameters' time, 0xD0 more; any
// other kind 0xD0 (libsail_rec divides by zero there, which the PPU answers with 0).
u64 _pspocket_cellSailRec_ComposerStreamSize(PPUContext& ctx, u32 instance, u64 kind, u32 params)
{
    const u32 sp = r32(ctx, r32(ctx, instance) + cp::kStreamParam);
    const u32 a = r32(ctx, sp + 4), b = r32(ctx, sp + 8);
    u64 q = 0;
    if (kind == 0) q = divu64(u64{r32(ctx, params + 0x30)} * b, a * r32(ctx, params + 0x80));
    else if (kind == 1) q = divu64(u64{r32(ctx, params + 0x34)} * b, a * r32(ctx, params + 0xA0));
    return ((static_cast<u32>(q) << 2) + 0xD0) & ~3u;
}

// Open(composer, instance, size, 0, path, the profile's type word, handle): the instance cleared and given the
// composer, the handle kept, the stream parameters (the profile's +0x450), opening (4); the game's open callback
// (argument, 0, path), else completed at once.
u64 _pspocket_cellSailRec_ComposerOpen(PPUContext& ctx, u32 c, u32 instance, u32 size, u32, u32 path, u32 profile_type,
    u64 handle)
{
    std::memset(gb(ctx) + instance, 0, size);
    w32(ctx, instance, c);
    w64(ctx, c + cp::kPending, handle);
    w32(ctx, c + cp::kStreamParam, profile_type + 0x18);
    w32(ctx, c + cp::kState, 4);
    if (const u32 cb = game_cb(ctx, c, cp::kOpen)) return call_guest(ctx, cb, {game_arg(ctx, c), 0, path});
    return notify_completed(ctx, c, 0);
}

// Close(instance, handle, handle): the handle kept, closing (5); the game's close callback (argument, 0, the handle),
// else completed at once.
u64 _pspocket_cellSailRec_ComposerClose(PPUContext& ctx, u32 instance, u64 handle)
{
    const u32 c = r32(ctx, instance);
    w64(ctx, c + cp::kPending, handle);
    w32(ctx, c + cp::kState, 5);
    if (const u32 cb = game_cb(ctx, c, cp::kClose)) return call_guest(ctx, cb, {game_arg(ctx, c), 0, handle});
    return notify_completed(ctx, c, 0);
}

void _pspocket_cellSailRec_ComposerAddAudio(PPUContext& ctx, u32 instance, u32 entry, u32 params, u64 handle)
{
    add_stream(ctx, instance, entry, params, handle, false);
}

void _pspocket_cellSailRec_ComposerAddVideo(PPUContext& ctx, u32 instance, u32 entry, u32 params, u64 handle)
{
    add_stream(ctx, instance, entry, params, handle, true);
}

// A user stream added: no parameters, no ring, no callback (libsail_rec never adds one).
void _pspocket_cellSailRec_ComposerAddUser(PPUContext& ctx, u32 instance, u32 entry, u32, u64 handle)
{
    const u32 c = r32(ctx, instance), obj = r32(ctx, entry + se::kObject);
    const u32 index = r32(ctx, c + cp::kUserCount);
    w32(ctx, c + cp::kUserCount, index + 1);
    w32(ctx, obj + so::kState, so::kAdded);
    w32(ctx, c + cp::kUser + 4 * index, entry);
    w32(ctx, obj + so::kEntry, entry);
    post_completion(ctx, r32(ctx, c + cp::kRecorder), 0, handle);
}

void _pspocket_cellSailRec_ComposerRemoveAudio(PPUContext& ctx, u32 instance, u32 entry, u64 handle)
{
    remove_stream(ctx, instance, entry, handle, cp::kAudioCount, cp::kAudio);
}

void _pspocket_cellSailRec_ComposerRemoveVideo(PPUContext& ctx, u32 instance, u32 entry, u64 handle)
{
    remove_stream(ctx, instance, entry, handle, cp::kVideoCount, cp::kVideo);
}

void _pspocket_cellSailRec_ComposerRemoveUser(PPUContext& ctx, u32 instance, u32 entry, u64 handle)
{
    remove_stream(ctx, instance, entry, handle, cp::kUserCount, cp::kUser);
}

// Start(instance, path, session, handle): the handle and session kept, starting (6), the stop's result cleared; the
// game's start callback (argument, path, session), else completed at once.
u64 _pspocket_cellSailRec_ComposerStart(PPUContext& ctx, u32 instance, u32 path, u32 session, u64 handle)
{
    const u32 c = r32(ctx, instance);
    w64(ctx, c + cp::kPending, handle);
    w32(ctx, c + cp::kSession, session);
    w32(ctx, c + cp::kState, 6);
    w32(ctx, c + cp::kStopResult, 0);
    if (const u32 cb = game_cb(ctx, c, cp::kStart)) return call_guest(ctx, cb, {game_arg(ctx, c), path, session});
    return notify_completed(ctx, c, 0);
}

// Stop(instance, flag, handle): stopping (7), the stop's result 0 with the flag (all ones without); every recording
// stream ended (its units dropped under its kind's lock) and checked for done.
void _pspocket_cellSailRec_ComposerStop(PPUContext& ctx, u32 instance, u64 flag, u64 handle)
{
    const u32 c = r32(ctx, instance);
    w64(ctx, c + cp::kPending, handle);
    w32(ctx, c + cp::kState, 7);
    w32(ctx, c + cp::kStopResult, static_cast<u32>(flag) ? 0 : 0xFFFFFFFF);
    const struct {
        u32 count, table, lock, pass;
    } kinds[] = {{cp::kAudioCount, cp::kAudio, cp::kLockA, 1}, {cp::kVideoCount, cp::kVideo, cp::kLockB, 0},
        {cp::kUserCount, cp::kUser, cp::kLockB, 0}};
    for (const auto& k : kinds) {
        for (s32 i = 0; i < static_cast<s32>(r32(ctx, c + k.count)); i++) {
            const u32 obj = r32(ctx, r32(ctx, c + k.table + 4 * static_cast<u32>(i)) + se::kObject);
            if (r32(ctx, obj + so::kState) != so::kRecording) continue;
            if (static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_lock", {c + k.lock, 0})) < 0) return;
            w32(ctx, obj + so::kState, so::kEnded);
            w32(ctx, obj + so::kCount, 0);
            sail::lv2_lib(ctx, "sys_lwmutex_unlock", {c + k.lock});
            stream_check_done(ctx, c, obj, k.pass);
        }
    }
}

void _pspocket_cellSailRec_ComposerEndAudio(PPUContext& ctx, u32 instance, u32 entry, u64 handle)
{
    end_stream(ctx, instance, entry, handle, cp::kEndAudio, 1);
}

void _pspocket_cellSailRec_ComposerEndVideo(PPUContext& ctx, u32 instance, u32 entry, u64 handle)
{
    end_stream(ctx, instance, entry, handle, cp::kEndVideo, 0);
}

void _pspocket_cellSailRec_ComposerEndUser(PPUContext& ctx, u32 instance, u32 entry, u64 handle)
{
    end_stream(ctx, instance, entry, handle, ~0u, 0);
}

void _pspocket_cellSailRec_ComposerNothing(PPUContext&) {}

// The units a stream's ring holds.
u64 _pspocket_cellSailRec_ComposerCapacity(PPUContext& ctx, u32, u32 entry)
{
    return r32(ctx, r32(ctx, entry + se::kObject) + so::kRingSize);
}

u64 _pspocket_cellSailRec_ComposerAudioAu(PPUContext& ctx, u32 instance, u32 entry, u32 au)
{
    return sx32(static_cast<u32>(stream_au(ctx, instance, entry, au, cp::kLockA, cp::kAudioArrived, 0x2B)));
}

u64 _pspocket_cellSailRec_ComposerVideoAu(PPUContext& ctx, u32 instance, u32 entry, u32 au)
{
    return sx32(static_cast<u32>(stream_au(ctx, instance, entry, au, cp::kLockB, cp::kVideoArrived, 0x2C)));
}

u64 _pspocket_cellSailRec_ComposerUserAu(PPUContext& ctx, u32 instance, u32 entry, u32 au)
{
    return sx32(static_cast<u32>(stream_au(ctx, instance, entry, au, cp::kLockB, ~0u, 0x2D)));
}

// A call of the recorder's completed (the continuation's status): the state moves on: opening to open (closed on a
// failure), closing to closed (no streams), starting to recording (every added stream recording; back to open on a
// failure), stopping to open (every ended stream added again).
void _pspocket_cellSailRec_ComposerCompleted(PPUContext& ctx, u32 instance, u64 status64)
{
    const u32 c = r32(ctx, instance);
    const s32 status = static_cast<s32>(static_cast<u32>(status64));
    const u32 state = r32(ctx, c + cp::kState);
    const u32 tables[3][2] = {{cp::kAudioCount, cp::kAudio}, {cp::kVideoCount, cp::kVideo},
        {cp::kUserCount, cp::kUser}};
    const auto each = [&](auto&& fn) {
        for (const auto& [count, table] : tables)
            for (s32 i = 0; i < static_cast<s32>(r32(ctx, c + count)); i++)
                fn(r32(ctx, r32(ctx, c + table + 4 * static_cast<u32>(i)) + se::kObject));
    };
    switch (state) {
    case 5:
        w32(ctx, c + cp::kUserCount, 0);
        w32(ctx, c + cp::kAudioCount, 0);
        w32(ctx, c + cp::kVideoCount, 0);
        w32(ctx, c + cp::kState, 1);
        return;
    case 4: w32(ctx, c + cp::kState, status < 0 ? 1 : 2); return;
    case 6: {
        if (status < 0) {
            w32(ctx, c + cp::kState, 2);
            return;
        }
        u32 recording = 0;
        each([&](u32 obj) {
            if (r32(ctx, obj + so::kState) != so::kAdded) return;
            recording++;
            w32(ctx, obj + so::kState, so::kRecording);
        });
        w32(ctx, c + cp::kRecording, recording);
        w32(ctx, c + cp::kState, 3);
        return;
    }
    case 7:
        each([&](u32 obj) {
            if (r32(ctx, obj + so::kState) - so::kEnded <= 2) w32(ctx, obj + so::kState, so::kAdded);
        });
        w32(ctx, c + cp::kRecording, 0);
        w32(ctx, c + cp::kState, 2);
        return;
    default: return;
    }
}

// Units given back (the release message's count): the stream checked for done.
void _pspocket_cellSailRec_ComposerReleased(PPUContext& ctx, u32 instance, u32 entry)
{
    stream_check_done(ctx, r32(ctx, instance), r32(ctx, entry + se::kObject), 2);
}

constexpr std::string_view kComposerCalls[24] = {"_pspocket_cellSailRec_ComposerMakeup",
    "_pspocket_cellSailRec_ComposerCleanup", "_pspocket_cellSailRec_ComposerInstanceSize",
    "_pspocket_cellSailRec_ComposerStreamSize", "_pspocket_cellSailRec_ComposerOpen",
    "_pspocket_cellSailRec_ComposerClose", "_pspocket_cellSailRec_ComposerAddAudio",
    "_pspocket_cellSailRec_ComposerAddVideo", "_pspocket_cellSailRec_ComposerAddUser",
    "_pspocket_cellSailRec_ComposerRemoveAudio", "_pspocket_cellSailRec_ComposerRemoveVideo",
    "_pspocket_cellSailRec_ComposerRemoveUser", "_pspocket_cellSailRec_ComposerStart",
    "_pspocket_cellSailRec_ComposerStop", "_pspocket_cellSailRec_ComposerEndAudio",
    "_pspocket_cellSailRec_ComposerEndVideo", "_pspocket_cellSailRec_ComposerEndUser",
    "_pspocket_cellSailRec_ComposerNothing", "_pspocket_cellSailRec_ComposerCapacity",
    "_pspocket_cellSailRec_ComposerAudioAu", "_pspocket_cellSailRec_ComposerVideoAu",
    "_pspocket_cellSailRec_ComposerUserAu", "_pspocket_cellSailRec_ComposerCompleted",
    "_pspocket_cellSailRec_ComposerReleased"};

} // namespace

u32 composer_table(PPUContext& ctx)
{
    struct Table {
        u32 addr = 0;
    };
    Table& t = HleProcess::current()->state<Table>();
    if (!t.addr) {
        t.addr = HleProcess::current()->main_memory().alloc(4 * 24, 16);
        if (!t.addr) PS_FATAL(kChan, "no main memory left for libsail_rec's composer table");
        for (u32 i = 0; i < 24; i++) w32(ctx, t.addr + 4 * i, internal(kComposerCalls[i]));
    }
    return t.addr;
}

u64 composer_notify_completed(PPUContext& ctx, u64 composer, s32 result) { return notify_completed(ctx, composer, result); }

// The game's composer attached at OpenStream: it learns the recorder, its makeup callback runs (an error ends it
// there), then it is registered in the composers table under its type.
s32 composer_attach(PPUContext& ctx, u32 c, u32 rec, u32 cs)
{
    w32(ctx, c + cp::kRecorder, rec);
    if (const u32 cb = game_cb(ctx, c, cp::kMakeup)) {
        const s32 r = static_cast<s32>(static_cast<u32>(call_guest(ctx, cb, {game_arg(ctx, c), rec, cs})));
        if (r < 0) return r;
    }
    return composer_register(ctx, cs, static_cast<u32>(static_cast<s32>(r32(ctx, c + cp::kType))),
        r32(ctx, c + cp::kTable), c);
}

// Detached at the stream's close: unregistered, its cleanup callback, no recorder.
void composer_detach(PPUContext& ctx, u32 c, u32 cs)
{
    composer_unregister(ctx, cs, static_cast<u32>(static_cast<s32>(r32(ctx, c + cp::kType))), 0, 0);
    if (const u32 cb = game_cb(ctx, c, cp::kCleanup)) call_guest(ctx, cb, {game_arg(ctx, c), 0, 0});
    w32(ctx, c + cp::kRecorder, 0);
}

// ------------------------------------------------------------------------------------------------- the MP4 composer

s32 mp4_composer_attach(PPUContext& ctx, u32) { later(ctx, "the MP4 composer"); }
void mp4_composer_destroy(PPUContext& ctx, u32, u32) { later(ctx, "the MP4 composer"); }

#define SAILREC_INTERNAL(fn) PS_HLE_INTERNAL(recomp::nid_of(#fn), fn)

void register_compose(Registry& r)
{
    r.add(kChan, {
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerMakeup),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerCleanup),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerInstanceSize),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerStreamSize),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerOpen),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerClose),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerAddAudio),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerAddVideo),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerAddUser),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerRemoveAudio),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerRemoveVideo),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerRemoveUser),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerStart),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerStop),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerEndAudio),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerEndVideo),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerEndUser),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerNothing),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerCapacity),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerAudioAu),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerVideoAu),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerUserAu),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerCompleted),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerReleased),
    });
}

#undef SAILREC_INTERNAL

} // namespace pspocket::hle::sailrec
