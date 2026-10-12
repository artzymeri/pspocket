// cellSailRec: Sony's recorder framework (libsail_rec.sprx, fw 4.92), the recorder side of libsail, pspocket's own
// (D-918), read from libsail_rec's code in a scratch area and compared with it running on pspocket
// (tests/hle/test_sail_rec_compare.cpp). RPCS3's cellSailRec.cpp names the 58 functions and answers each with a log;
// its structures are libsail's (cellSail.h). Nothing of libsail_rec's code is copied here.
//
// The recorder (cellSailRec.hpp gives its layout) takes frames from the game's feeders (CellSailFeederAudio and
// CellSailFeederVideo: the game's callbacks, which libsail_rec drives), converts and encodes them on the game's SPURS
// (its own SPU programs and the firmware's encoders) and hands the encoded streams to a composer: the game's
// (CellSailComposer, type 3) or libsail_rec's MP4 writer (type 1, through the firmware's MP4 library). A control thread
// runs it, taking messages from a queue laid out as libsail's (the game's commands, the feeders' and composers'
// notifications, the SPU tasks' messages, which a second thread brings from a SPURS queue). The recorder has no
// camera or microphone of its own: what it records is what the game's feeders give it.
//
// This file: the recorder's set-up and parameters, events, DumpImage and the commands' sending side;
// cellSailRecObjects.cpp: the game's objects (feeders, composer, profiles, video converters); cellSailRecControl.cpp:
// the control thread.

#include "cellSailRec.hpp"

#include <cstring>
#include <format>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

// ------------------------------------------------------------------------------------------------- shared helpers

u32 internal(std::string_view name)
{
    return HleProcess::current()->link_function(kChan, recomp::nid_of(name), name);
}

void guest_strncpy(PPUContext& ctx, u32 dst, u32 src, u32 n) { sail::lv2_lib(ctx, "_sys_strncpy", {dst, src, n}); }

u32 guest_strlen(PPUContext& ctx, u32 s) { return static_cast<u32>(sail::lv2_lib(ctx, "_sys_strlen", {s})); }

u32 alloc(PPUContext& ctx, u32 rec, u32 boundary, u32 size)
{
    return static_cast<u32>(
        call_guest(ctx, r32(ctx, rec + rc::kAlloc), {r32(ctx, rec + rc::kAllocArg), boundary, size}));
}

void dealloc(PPUContext& ctx, u32 rec, u32 boundary, u32 ptr)
{
    call_guest(ctx, r32(ctx, rec + rc::kFree), {r32(ctx, rec + rc::kAllocArg), boundary, ptr});
}

namespace {

// The queue's fields from the recorder's +0x4A8 (libsail's layout).
constexpr u32 kQCapacity = 0, kQCount = 4, kQRing = 8, kQWrite = 0xC, kQRead = 0x10, kQMutex = 0x14, kQCond = 0x18;

u32 queue_tail(PPUContext& ctx, u32 q)
{
    while (r32(ctx, q + kQCapacity) <= r32(ctx, q + kQCount)) sc(ctx, 107, {r32(ctx, q + kQCond), 0});
    return r32(ctx, q + kQRing) + 16 * r32(ctx, q + kQWrite);
}

s32 queue_commit_tail(PPUContext& ctx, u32 q)
{
    const u32 cap = r32(ctx, q + kQCapacity), next = r32(ctx, q + kQWrite) + 1;
    w32(ctx, q + kQWrite, cap ? next % cap : 0);
    w32(ctx, q + kQCount, r32(ctx, q + kQCount) + 1);
    sc(ctx, 108, {r32(ctx, q + kQCond)});
    return sc(ctx, 104, {r32(ctx, q + kQMutex)});
}

} // namespace

s32 post(PPUContext& ctx, u32 rec, u32 code, u32 object, u32 arg)
{
    const u32 q = rec + rc::kQueue;
    const s32 r = sc(ctx, 102, {r32(ctx, q + kQMutex), 0});
    if (r < 0) return r;
    const u32 at = queue_tail(ctx, q);
    w32(ctx, at + 4, 0);
    w32(ctx, at + 12, arg);
    w32(ctx, at, code);
    w32(ctx, at + 8, object);
    return queue_commit_tail(ctx, q);
}

s32 post_completion(PPUContext& ctx, u32 rec, s32 result, u64 handle)
{
    if (!handle) return static_cast<s32>(rec);
    const u32 q = rec + rc::kQueue;
    const s32 r = sc(ctx, 102, {r32(ctx, q + kQMutex), 0});
    if (r < 0) return r;
    const u32 at = queue_tail(ctx, q);
    w32(ctx, at, 0);
    w64(ctx, at + 8, handle);
    w32(ctx, at + 4, static_cast<u32>(result));
    return queue_commit_tail(ctx, q);
}

namespace {

// A message at the queue's head {code, a, b, c} (the commands' and Cancel's); its lock's error, or the unlock's answer.
s32 post_head(PPUContext& ctx, u32 rec, u32 code, u32 a, u32 b, u32 c)
{
    const u32 q = rec + rc::kQueue;
    const s32 r = sc(ctx, 102, {r32(ctx, q + kQMutex), 0});
    if (r < 0) return r;
    const u32 cap = r32(ctx, q + kQCapacity);
    while (cap <= r32(ctx, q + kQCount)) sc(ctx, 107, {r32(ctx, q + kQCond), 0});
    const u32 prev = cap + r32(ctx, q + kQRead) - 1;
    w32(ctx, q + kQRead, cap ? prev % cap : 0);
    const u32 at = r32(ctx, q + kQRing) + 16 * r32(ctx, q + kQRead);
    w32(ctx, at + 12, c);
    w32(ctx, at, code);
    w32(ctx, at + 4, a);
    w32(ctx, at + 8, b);
    w32(ctx, q + kQCount, r32(ctx, q + kQCount) + 1);
    sc(ctx, 108, {r32(ctx, q + kQCond)});
    return sc(ctx, 104, {r32(ctx, q + kQMutex)});
}

} // namespace

s32 lock(PPUContext& ctx, u32 rec) { return static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_lock", {rec + rc::kLock, 0})); }

s32 unlock(PPUContext& ctx, u32 rec) { return static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_unlock", {rec + rc::kLock})); }

void event(PPUContext& ctx, u32 rec, u32 major, u32 minor, u64 a, u64 b)
{
    const u32 handler = r32(ctx, rec + rc::kHandler);
    if (!handler) return;
    // libsail_rec keeps the highest stack pointer the handler was called with, and lowers the handler's frame so it
    // starts past the last 2000 bytes before the stack's top (+0x870, the control thread's).
    const u64 sp = ctx.gpr[1];
    if (r64(ctx, rec + rc::kStackHigh) < sp) w64(ctx, rec + rc::kStackHigh, sp);
    const s64 over = static_cast<s64>(sp - r64(ctx, rec + rc::kStackTop));
    const u32 frame = (static_cast<u32>(over < 0 ? 0 : over) + 30) & ~15u;
    StackVar room(ctx, gb(ctx), frame);
    call_guest(ctx, handler, {r32(ctx, rec + rc::kHandlerArg), u64{major} << 32 | minor, a, b});
}

void later(PPUContext& ctx, std::string_view what)
{
    log_call_site(ctx, gb(ctx), std::format("cellSailRec: {} is a later part of D-918", what));
    PS_FATAL(kChan, "libsail_rec's {} is a later part of D-918", what);
}

u32 modules_flag(PPUContext& ctx)
{
    const u32 pid = static_cast<u32>(sc(ctx, 1, {}));
    StackVar v(ctx, gb(ctx), 8);
    if (sc(ctx, 871, {1, pid, v.addr()}) != 0) return 0;
    return (r64(ctx, v.addr()) & 0xFFFFFF) - 1 <= 1 ? 1 : 0;
}

namespace {

// ------------------------------------------------------------------------------------------------- the commands

// The game's side of a command (libsail_rec's): with the recorder running (+0x700), under its lwmutex, a record {the
// answer's address, the calling thread} posted at the queue's head with the command and its argument block, then one
// wait on the recorder's lwcond, which the control thread signals to this thread once it has answered. The lock's
// error, or 0 with the answer at `reply`. (A queue whose mutex cannot be locked gets no message, and the wait is
// made all the same, as libsail_rec does.)
s32 command(PPUContext& ctx, u32 rec, u32 code, u32 arg, u32 reply)
{
    if (!r32(ctx, rec + rc::kRunning)) return error(CELL_SAIL_ERROR_INVALID_STATE);
    StackVar record(ctx, gb(ctx), 16);
    w32(ctx, record.addr(), reply);
    sail::lv2_lib(ctx, "sys_ppu_thread_get_id", {record.addr() + 8});
    const s32 r = lock(ctx, rec);
    if (r < 0) return r;
    post_head(ctx, rec, code, 0, arg, record.addr());
    sail::lv2_lib(ctx, "sys_lwcond_wait", {rec + rc::kCond, 0});
    unlock(ctx, rec);
    return 0;
}

// A command and its answer word: the command's error, or the answer.
s32 command_answer(PPUContext& ctx, u32 rec, u32 code, u32 arg)
{
    StackVar reply(ctx, gb(ctx), 16);
    const s32 r = command(ctx, rec, code, arg, reply.addr());
    return r < 0 ? r : static_cast<s32>(r32(ctx, reply.addr()));
}

// ------------------------------------------------------------------------------------------------- Initialize, Finalize

// The MP4 composer's file calls a recorder starts with (libsail_rec's own, through cellFs): 14 words, the first all
// ones, the empty ones 0.
constexpr std::string_view kFileCalls[14] = {"", "_pspocket_cellSailRec_FsOpen", "", "_pspocket_cellSailRec_FsClose",
    "_pspocket_cellSailRec_FsFstat", "_pspocket_cellSailRec_FsRead", "_pspocket_cellSailRec_FsLseek",
    "_pspocket_cellSailRec_FsNone", "", "", "_pspocket_cellSailRec_FsWrite", "_pspocket_cellSailRec_FsRename",
    "_pspocket_cellSailRec_FsUnlink", ""};

// cellSailRecorderInitialize(recorder, allocator, handler, handler argument, attribute, resource): the attribute is
// {size 0x1C, mode (0 or 1), audio, video and user streams, messages, timescale}, the resource {size 0xC, SPURS, 0}.
// The recorder is cleared once the two sizes are right, then the allocator, SPURS and the resource's last word are
// checked.
s32 cellSailRecorderInitialize(PPUContext& ctx, u32 rec, u32 allocator, u32 handler, u32 handler_arg, u32 attr,
    u32 res)
{
    if (!rec || !attr || r32(ctx, attr) != 0x1C || !res || r32(ctx, res) != 0xC)
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memset(gb(ctx) + rec, 0, rc::kSize);
    if (!allocator || !r32(ctx, res + 4) || r32(ctx, res + 8)) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + rec + rc::kAlloc, gb(ctx) + allocator, 0xC);
    const u32 spurs = r32(ctx, res + 4);
    w32(ctx, rec + rc::kSpurs, spurs);
    sail::module_call(ctx, "cellSpurs", "cellSpursGetNumSpuThread", {spurs, rec + rc::kSpuCount});
    if (r32(ctx, attr + 4) >= 2) return error(CELL_SAIL_ERROR_INVALID_ARG);
    w32(ctx, rec + rc::kMode, r32(ctx, attr + 4));
    const u32 audio = r32(ctx, attr + 8), video = r32(ctx, attr + 12), user = r32(ctx, attr + 16);
    w32(ctx, rec + rc::kAudioCount, audio);
    w32(ctx, rec + rc::kVideoCount, video);
    w32(ctx, rec + rc::kUserCount, user);
    w32(ctx, rec + rc::kHandler, handler);
    w32(ctx, rec + rc::kHandlerArg, handler_arg);
    w64(ctx, rec + rc::kEventMask, ~u64{0});
    const u32 timescale = r32(ctx, attr + 24);
    w32(ctx, rec + rc::kTimescale, timescale ? timescale : 1000000);
    u32 messages = r32(ctx, attr + 20);
    if (!messages) messages = (user + audio + video) * 50 + 50;
    const u32 size = messages * 16 + (user + video + audio) * 0x8000;
    s32 r = error(CELL_SAIL_ERROR_MEMORY);
    const u32 block = alloc(ctx, rec, 8, size);
    if (block) {
        std::memset(gb(ctx) + block, 0, size);
        w32(ctx, rec + rc::kQueue, messages);
        w32(ctx, rec + rc::kAudio, block);
        const u32 vslots = block + audio * rc::kAudioStride, uslots = vslots + video * rc::kVideoStride;
        w32(ctx, rec + rc::kVideo, vslots);
        w32(ctx, rec + rc::kUser, uslots);
        w32(ctx, rec + rc::kQueue + kQRing, uslots + user * rc::kUserStride);
        r = sail::make_pair(ctx, rec + rc::kQueueMutex, "_saiqu0");
        if (r >= 0) {
            // libsail_rec's lwmutex and lwcond pair: {priority, not recursive, name}, the lwcond on it.
            r = sail::make_lwmutex(ctx, rec + rc::kLock, "_saicx0");
            if (r >= 0) {
                StackVar cattr(ctx, gb(ctx), 16);
                std::memcpy(gb(ctx) + cattr.addr(), "_saicx0", 8);
                r = static_cast<s32>(
                    sail::lv2_lib(ctx, "sys_lwcond_create", {rec + rc::kCond, rec + rc::kLock, cattr.addr()}));
                if (r < 0) sail::destroy_lwmutex(ctx, rec + rc::kLock);
            }
        }
        if (r >= 0) {
            w32(ctx, rec + rc::kThreadPrio, 900);
            w32(ctx, rec + rc::kTaskPrio, 0x370);
            w32(ctx, rec + rc::kThreadStack, 0x3000);
            w32(ctx, rec + rc::kTasksetParam, 1);
            w32(ctx, rec + rc::kTaskStack, 0x8000);
            w32(ctx, rec + rc::kMp4Param1, 600);
            w32(ctx, rec + rc::kMp4Param2, 0x2000);
            w64(ctx, rec + rc::kTasksetPriorities, 0x0101010101010101ull);
            for (u32 i = 0; i < audio; i++) {
                const u32 s = block + i * rc::kAudioStride;
                w8(ctx, s + as::kIndex, static_cast<u8>(i));
                w64(ctx, s + as::kP70, 0x0101010101010101ull);
                w32(ctx, s + as::kP71, 1);
                w64(ctx, s + as::kP72, 0x0101010101010101ull);
                w32(ctx, s + as::kP69, 1);
            }
            const u32 spus = r32(ctx, rec + rc::kSpuCount);
            for (u32 i = 0; i < video; i++) {
                const u32 s = vslots + i * rc::kVideoStride;
                w8(ctx, s + vs::kIndex, static_cast<u8>(i));
                const u32 n = spus == 1 ? 1 : spus - 1;
                w32(ctx, s + vs::kP77, n);
                w64(ctx, s + vs::kP75, 0x0101010101010101ull);
                w32(ctx, s + vs::kP76, n);
                w64(ctx, s + vs::kP79, 0x0101010101010101ull);
                w32(ctx, s + vs::kP80, 1);
                w64(ctx, s + vs::kP81, 0x0101010101010101ull);
                w32(ctx, s + vs::kP74, 1);
            }
            for (u32 i = 0; i < 14; i++) {
                u32 v = 0;
                if (i == 0) v = 0xFFFFFFFF;
                else if (!kFileCalls[i].empty())
                    v = HleProcess::current()->link_function(kChan, recomp::nid_of(kFileCalls[i]), kFileCalls[i]);
                w32(ctx, rec + rc::kFileCalls + 4 * i, v);
            }
            return 0;
        }
        if (r32(ctx, rec + rc::kAudio)) dealloc(ctx, rec, 8, r32(ctx, rec + rc::kAudio));
    }
    w32(ctx, rec + rc::kState, rc::kFinalized);
    return r;
}

// cellSailRecorderFinalize(recorder): a booted recorder is told to end (message 0x30) and its control thread joined;
// then the lwcond and lwmutex, the queue's pair (unless its count reads -1) and the block go.
s32 cellSailRecorderFinalize(PPUContext& ctx, u32 rec)
{
    const u32 state = r32(ctx, rec + rc::kState);
    if (state == rc::kFinalized) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (state != rc::kEnded && state != rc::kInitialized) {
        post(ctx, rec, 0x30, rec, 0);
        StackVar status(ctx, gb(ctx), 16);
        sc(ctx, 44, {r64(ctx, rec + rc::kThread), status.addr()});
    }
    sail::lv2_lib(ctx, "sys_lwcond_destroy", {rec + rc::kCond});
    const s32 r = static_cast<s32>(sail::lv2_lib(ctx, "sys_lwmutex_destroy", {rec + rc::kLock}));
    if (r >= 0) {
        s32 q = 0;
        if (static_cast<s32>(r32(ctx, rec + rc::kQueue + kQCount)) != -1) {
            sc(ctx, 106, {r32(ctx, rec + rc::kQueueMutex + 4)});
            q = sc(ctx, 101, {r32(ctx, rec + rc::kQueueMutex)});
        }
        if (q >= 0 && r32(ctx, rec + rc::kAudio)) {
            dealloc(ctx, rec, 8, r32(ctx, rec + rc::kAudio));
            w32(ctx, rec + rc::kAudio, 0);
        }
    }
    w32(ctx, rec + rc::kState, rc::kFinalized);
    return 0;
}

// ------------------------------------------------------------------------------------------------- parameters

bool recorder_wide(s32 type)
{
    return type == 2 || type == 0x1D || (type >= 0x40 && type <= 0x43) || type == 0x52 || type == 0x53;
}

// cellSailRecorderSetParameter(recorder, type, param0, param1): only before Boot. The recorder's own (2 the control
// thread's priority, 29 its stack, 64 and 65 the task handler's, 66 and 67 the common taskset's, 82 and 83 the MP4
// composer's) take param0 and need a nonzero param1; the per-stream ones take the stream's index in param0 and the value
// in param1 (audio 68 to 72, video 73 to 77 and 79 to 81); 78 and the rest are refused.
s32 cellSailRecorderSetParameter(PPUContext& ctx, u32 rec, s32 type, u64 p0, u64 p1)
{
    if (r32(ctx, rec + rc::kState) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 v = static_cast<u32>(p0);
    if (recorder_wide(type)) {
        if (!p1) return error(CELL_SAIL_ERROR_INVALID_ARG);
        switch (type) {
        case 2: w32(ctx, rec + rc::kThreadPrio, v); break;
        case 0x1D: w32(ctx, rec + rc::kThreadStack, v); break;
        case 0x40: w32(ctx, rec + rc::kTaskPrio, v); break;
        case 0x41: w32(ctx, rec + rc::kTaskStack, v); break;
        case 0x42: w32(ctx, rec + rc::kTasksetParam, v); break;
        case 0x43: w64(ctx, rec + rc::kTasksetPriorities, p0); break;
        case 0x52: w32(ctx, rec + rc::kMp4Param1, v); break;
        case 0x53: w32(ctx, rec + rc::kMp4Param2, v); break;
        default: break;
        }
        return 0;
    }
    const u32 k = static_cast<u32>(type - 0x44);
    if (k > 13) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u64 bit = u64{1} << k;
    if (bit & 0x3BE0) {
        if (r32(ctx, rec + rc::kVideoCount) <= v) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        const u32 s = static_cast<u32>(p0 * rc::kVideoStride + r32(ctx, rec + rc::kVideo));
        switch (type) {
        case 0x49: w8(ctx, s + vs::kP73, p1 != 0); break;
        case 0x4A: w32(ctx, s + vs::kP74, static_cast<u32>(p1)); break;
        case 0x4B: w64(ctx, s + vs::kP75, p1); break;
        case 0x4C: w32(ctx, s + vs::kP76, static_cast<u32>(p1)); break;
        case 0x4D: w32(ctx, s + vs::kP77, static_cast<u32>(p1)); break;
        case 0x4F: w64(ctx, s + vs::kP79, p1); break;
        case 0x50: w32(ctx, s + vs::kP80, static_cast<u32>(p1)); break;
        case 0x51: w64(ctx, s + vs::kP81, p1); break;
        default: break;
        }
        return 0;
    }
    if (!(bit & 0x1F)) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (v >= r32(ctx, rec + rc::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    const u32 s = static_cast<u32>(p0 * rc::kAudioStride + r32(ctx, rec + rc::kAudio));
    switch (type) {
    case 0x44: w8(ctx, s + as::kP68, p1 != 0); break;
    case 0x45: w32(ctx, s + as::kP69, static_cast<u32>(p1)); break;
    case 0x46: w64(ctx, s + as::kP70, p1); break;
    case 0x47: w32(ctx, s + as::kP71, static_cast<u32>(p1)); break;
    case 0x48: w64(ctx, s + as::kP72, p1); break;
    default: break;
    }
    return 0;
}

// cellSailRecorderGetParameter(recorder, type, u64* param0, u64* param1): the recorder's own to *param0 (param1 must be
// NULL; some read back sign-extended), the per-stream ones with the index in *param0's low word to *param1.
s32 cellSailRecorderGetParameter(PPUContext& ctx, u32 rec, s32 type, u32 p0, u32 p1)
{
    if (r32(ctx, rec + rc::kState) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (!p0) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const auto sx = [](u32 v) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); };
    if (recorder_wide(type)) {
        if (p1) return error(CELL_SAIL_ERROR_INVALID_ARG);
        switch (type) {
        case 2: w64(ctx, p0, sx(r32(ctx, rec + rc::kThreadPrio))); break;
        case 0x1D: w64(ctx, p0, r32(ctx, rec + rc::kThreadStack)); break;
        case 0x40: w64(ctx, p0, sx(r32(ctx, rec + rc::kTaskPrio))); break;
        case 0x41: w64(ctx, p0, r32(ctx, rec + rc::kTaskStack)); break;
        case 0x42: w64(ctx, p0, r32(ctx, rec + rc::kTasksetParam)); break;
        case 0x43: w64(ctx, p0, r64(ctx, rec + rc::kTasksetPriorities)); break;
        case 0x52: w64(ctx, p0, sx(r32(ctx, rec + rc::kMp4Param1))); break;
        case 0x53: w64(ctx, p0, r32(ctx, rec + rc::kMp4Param2)); break;
        default: break;
        }
        return 0;
    }
    if (!p1) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 k = static_cast<u32>(type - 0x44);
    if (k > 13) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u64 bit = u64{1} << k;
    const u32 index = r32(ctx, p0 + 4);
    if (bit & 0x3BE0) {
        if (r32(ctx, rec + rc::kVideoCount) <= index) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        const u32 s = index * rc::kVideoStride + r32(ctx, rec + rc::kVideo);
        u64 v = 0;
        switch (type) {
        case 0x49: v = r8(ctx, s + vs::kP73); break;
        case 0x4A: v = r32(ctx, s + vs::kP74); break;
        case 0x4B: v = r64(ctx, s + vs::kP75); break;
        case 0x4C: v = r32(ctx, s + vs::kP76); break;
        case 0x4D: v = r32(ctx, s + vs::kP77); break;
        case 0x4F: v = r64(ctx, s + vs::kP79); break;
        case 0x50: v = r32(ctx, s + vs::kP80); break;
        case 0x51: v = r64(ctx, s + vs::kP81); break;
        default: break;
        }
        w64(ctx, p1, v);
        return 0;
    }
    if (!(bit & 0x1F)) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (index >= r32(ctx, rec + rc::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    const u32 s = index * rc::kAudioStride + r32(ctx, rec + rc::kAudio);
    u64 v = 0;
    switch (type) {
    case 0x44: v = r8(ctx, s + as::kP68); break;
    case 0x45: v = r32(ctx, s + as::kP69); break;
    case 0x46: v = r64(ctx, s + as::kP70); break;
    case 0x47: v = r32(ctx, s + as::kP71); break;
    case 0x48: v = r64(ctx, s + as::kP72); break;
    default: break;
    }
    w64(ctx, p1, v);
    return 0;
}

// ------------------------------------------------------------------------------------------------- feeders, composer

// cellSailRecorderSetFeederAudio/Video(recorder, index, feeder): before Boot, once per stream.
s32 set_feeder(PPUContext& ctx, u32 rec, u32 index, u64 feeder, u32 count, u32 slots, u32 stride)
{
    if (r32(ctx, rec + rc::kState) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (index >= r32(ctx, rec + count)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    if (!feeder) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 s = index * stride + r32(ctx, rec + slots);
    if (r32(ctx, s + as::kFeeder)) return error(CELL_SAIL_ERROR_USING);
    w32(ctx, s + as::kFeeder, static_cast<u32>(feeder));
    return 0;
}

s32 cellSailRecorderSetFeederAudio(PPUContext& ctx, u32 rec, u32 index, u64 feeder)
{
    return set_feeder(ctx, rec, index, feeder, rc::kAudioCount, rc::kAudio, rc::kAudioStride);
}

s32 cellSailRecorderSetFeederVideo(PPUContext& ctx, u32 rec, u32 index, u64 feeder)
{
    return set_feeder(ctx, rec, index, feeder, rc::kVideoCount, rc::kVideo, rc::kVideoStride);
}

// cellSailRecorderRegisterComposer(recorder, type, composer): before Boot, type 3 (the game's) only, once.
s32 cellSailRecorderRegisterComposer(PPUContext& ctx, u32 rec, s32 type, u32 composer)
{
    if (r32(ctx, rec + rc::kState) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (!composer || type != 3) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (r32(ctx, rec + rc::kComposer)) return error(CELL_SAIL_ERROR_FULLED);
    w32(ctx, composer + cp::kType, 3);
    w32(ctx, rec + rc::kComposer, composer);
    w32(ctx, rec + rc::kComposerSet, 1);
    return 0;
}

// cellSailRecorderUnregisterComposer(recorder, type, composer*): before Boot or once booted (state 2), type 3: the
// composer given back (when asked), none registered.
s32 cellSailRecorderUnregisterComposer(PPUContext& ctx, u32 rec, s32 type, u32 out)
{
    const u32 state = r32(ctx, rec + rc::kState);
    if (state != 0 && state != rc::kBooted) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (type != 3) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (out) w32(ctx, out, r32(ctx, rec + rc::kComposer));
    w32(ctx, rec + rc::kComposer, 0);
    w32(ctx, rec + rc::kComposerSet, 0);
    return 0;
}

// ------------------------------------------------------------------------------------------------- events

// cellSailRecorderReplaceEventHandler(recorder, handler, arg, old handler*, old arg*): under the recorder's lock (taken
// as held when the call comes from inside the handler: EDEADLK).
s32 cellSailRecorderReplaceEventHandler(PPUContext& ctx, u32 rec, u32 handler, u32 arg, u32 old_handler, u32 old_arg)
{
    if (!handler) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const s32 r = lock(ctx, rec);
    bool locked = true;
    if (r < 0) {
        if (static_cast<u32>(r) != 0x80010008) return r;
        locked = false;
    }
    if (old_handler) w32(ctx, old_handler, r32(ctx, rec + rc::kHandler));
    if (old_arg) w32(ctx, old_arg, r32(ctx, rec + rc::kHandlerArg));
    w32(ctx, rec + rc::kHandlerArg, arg);
    w32(ctx, rec + rc::kHandler, handler);
    if (locked) unlock(ctx, rec);
    return 0;
}

// cellSailRecorderSubscribeEvent(recorder, event, param) and UnsubscribeEvent(recorder, event): events below 12 (a
// subscription's parameter must be 0).
s32 cellSailRecorderSubscribeEvent(PPUContext& ctx, u32 rec, u64 type, u64 param)
{
    s32 r = lock(ctx, rec);
    if (r < 0) return r;
    if (static_cast<u32>(type) < 12 && param == 0)
        w64(ctx, rec + rc::kEventMask, r64(ctx, rec + rc::kEventMask) | u64{1} << (type & 0x7F));
    else r = error(CELL_SAIL_ERROR_INVALID_ARG);
    unlock(ctx, rec);
    return r;
}

s32 cellSailRecorderUnsubscribeEvent(PPUContext& ctx, u32 rec, u64 type)
{
    s32 r = lock(ctx, rec);
    if (r < 0) return r;
    if (static_cast<u32>(type) < 12)
        w64(ctx, rec + rc::kEventMask, r64(ctx, rec + rc::kEventMask) & ~(u64{1} << (type & 0x7F)));
    else r = error(CELL_SAIL_ERROR_INVALID_ARG);
    unlock(ctx, rec);
    return r;
}

// ------------------------------------------------------------------------------------------------- Boot and commands

// cellSailRecorderBoot(recorder, argument): before Boot, every audio and video stream with its feeder: state 1, the
// argument kept for Boot's completion event, the control thread started ("_libsail-control-ex", joinable, at the
// recorder's priority and stack); a thread that cannot be made puts the state back.
s32 cellSailRecorderBoot(PPUContext& ctx, u32 rec, u64 arg)
{
    s32 r = lock(ctx, rec);
    if (r < 0) return r;
    bool ok = r32(ctx, rec + rc::kState) == 0;
    for (u32 i = 0; ok && i < r32(ctx, rec + rc::kAudioCount); i++)
        ok = r32(ctx, r32(ctx, rec + rc::kAudio) + i * rc::kAudioStride + as::kFeeder) != 0;
    for (u32 i = 0; ok && i < r32(ctx, rec + rc::kVideoCount); i++)
        ok = r32(ctx, r32(ctx, rec + rc::kVideo) + i * rc::kVideoStride + vs::kFeeder) != 0;
    if (!ok) {
        r = error(CELL_SAIL_ERROR_INVALID_STATE);
    } else {
        w64(ctx, rec + rc::kCommandArg, arg);
        w32(ctx, rec + rc::kState, rc::kBooting);
        StackVar name(ctx, gb(ctx), 32);
        std::memcpy(gb(ctx) + name.addr(), "_libsail-control-ex", 20);
        const u32 entry = HleProcess::current()->link_function(kChan, recomp::nid_of("_pspocket_cellSailRec_control"),
            "_pspocket_cellSailRec_control");
        const s32 prio = static_cast<s32>(r32(ctx, rec + rc::kThreadPrio));
        r = static_cast<s32>(sail::lv2_lib(ctx, "sys_ppu_thread_create",
            {rec + rc::kThread, entry, rec, static_cast<u64>(static_cast<s64>(prio)), r32(ctx, rec + rc::kThreadStack),
                1, name.addr()}));
        if (r < 0) w32(ctx, rec + rc::kState, rc::kInitialized);
    }
    unlock(ctx, rec);
    return r;
}

// The commands (the control thread answers them): CreateProfile (1), DestroyProfile (2), CreateVideoConverter (3),
// DestroyVideoConverter (4), OpenStream (5), CloseStream (7), Start (9), Stop (0xB), each with an argument block on the
// caller's stack.

// cellSailRecorderCreateProfile(recorder, type, profile*): type 1 (MP4), or 3 with the game's composer registered.
s32 cellSailRecorderCreateProfile(PPUContext& ctx, u32 rec, s32 type, u64 out)
{
    if (!out) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (type != 1 && !(type == 3 && static_cast<s32>(r32(ctx, rec + rc::kComposerSet)) > 0))
        return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    StackVar block(ctx, gb(ctx), 16);
    w32(ctx, block.addr(), static_cast<u32>(type));
    w32(ctx, block.addr() + 4, static_cast<u32>(out));
    return command_answer(ctx, rec, 1, block.addr());
}

// The Destroy commands: an object given, a recorder past Initialize; 0 or the command's error.
s32 destroy_object(PPUContext& ctx, u32 rec, u32 code, u32 object)
{
    if (!object) return error(CELL_SAIL_ERROR_INVALID_ARG);
    if (!r32(ctx, rec + rc::kState)) return error(CELL_SAIL_ERROR_INVALID_STATE);
    StackVar reply(ctx, gb(ctx), 16);
    const s32 r = command(ctx, rec, code, object, reply.addr());
    return r < 0 ? r : 0;
}

s32 cellSailRecorderDestroyProfile(PPUContext& ctx, u32 rec, u32 profile) { return destroy_object(ctx, rec, 2, profile); }

s32 cellSailRecorderDestroyVideoConverter(PPUContext& ctx, u32 rec, u32 converter)
{
    return destroy_object(ctx, rec, 4, converter);
}

// cellSailRecorderCreateVideoConverter(recorder, mode, in format, out format, 0, a, b, converter*): mode 0 to 2; the
// formats CellSailVideoFormat (0x20 bytes) with only the coding (byte 0), width and height (the out's all ones or the
// in's) and the scan and aspect fields (bytes 0x10, 0x12) given, every other field all ones; conversions from 4 or 9 to
// 1, and from 11 or 12 to 1, 4 or 9 (else UNSUPPORTED_STREAM).
s32 cellSailRecorderCreateVideoConverter(PPUContext& ctx, u32 rec, u32 mode, u32 in, u32 out, u32 zero, u32 a, u32 b,
    u32 converter)
{
    const auto bad = [] { return error(CELL_SAIL_ERROR_INVALID_ARG); };
    if (!converter || !in || !out || zero || mode > 2) return bad();
    const s8 cin = static_cast<s8>(r8(ctx, in));
    if (cin == -1 || r8(ctx, in + 1) != 0xFF || r8(ctx, in + 2) != 0xFF || r8(ctx, in + 3) != 0xFF) return bad();
    const s16 win = static_cast<s16>(r16(ctx, in + 4)), hin = static_cast<s16>(r16(ctx, in + 6));
    if (win == -1 || hin == -1 || r32(ctx, in + 8) != 0xFFFFFFFF || r8(ctx, in + 0x11) != 0xFF ||
        r8(ctx, in + 0x13) != 0xFF || r32(ctx, in + 0x14) != 0xFFFFFFFF || r64(ctx, in + 0x18) != ~u64{0})
        return bad();
    const s8 cout = static_cast<s8>(r8(ctx, out));
    if (cout == -1 || r8(ctx, out + 1) != 0xFF || r8(ctx, out + 2) != 0xFF || r8(ctx, out + 3) != 0xFF ||
        r32(ctx, out + 8) != 0xFFFFFFFF || r8(ctx, out + 0x11) != 0xFF || r8(ctx, out + 0x13) != 0xFF ||
        r32(ctx, out + 0x14) != 0xFFFFFFFF || r64(ctx, out + 0x18) != ~u64{0})
        return bad();
    const s16 wout = static_cast<s16>(r16(ctx, out + 4)), hout = static_cast<s16>(r16(ctx, out + 6));
    if ((wout != -1 && wout != win) || (hout != -1 && hout != hin)) return bad();
    bool supported = false;
    if (cin == 4 || cin == 9) supported = cout == 1;
    else if (cin == 11 || cin == 12) supported = cout == 1 || cout == 9 || cout == 4;
    if (!supported) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    StackVar block(ctx, gb(ctx), 32);
    const u32 k = block.addr();
    w32(ctx, k, mode);
    w32(ctx, k + 4, in);
    w32(ctx, k + 8, out);
    w32(ctx, k + 12, 0);
    w32(ctx, k + 16, a);
    w32(ctx, k + 20, b);
    w32(ctx, k + 24, converter);
    return command_answer(ctx, rec, 3, k);
}

// cellSailRecorderOpenStream(recorder, profile, a, path, b, c, argument): the profile must be given.
s32 cellSailRecorderOpenStream(PPUContext& ctx, u32 rec, u64 profile, u32 a, u32 path, u32 b, u32 c, u64 arg)
{
    if (!profile) return error(CELL_SAIL_ERROR_INVALID_ARG);
    StackVar block(ctx, gb(ctx), 32);
    const u32 k = block.addr();
    w32(ctx, k, static_cast<u32>(profile));
    w32(ctx, k + 4, a);
    w32(ctx, k + 8, path);
    w32(ctx, k + 12, b);
    w32(ctx, k + 16, c);
    w64(ctx, k + 24, arg);
    return command_answer(ctx, rec, 5, k);
}

s32 cellSailRecorderCloseStream(PPUContext& ctx, u32 rec, u64 arg)
{
    StackVar block(ctx, gb(ctx), 16);
    w64(ctx, block.addr(), arg);
    return command_answer(ctx, rec, 7, block.addr());
}

s32 cellSailRecorderStart(PPUContext& ctx, u32 rec, u32 a, u32 b, u64 arg)
{
    StackVar block(ctx, gb(ctx), 16);
    w32(ctx, block.addr(), a);
    w32(ctx, block.addr() + 4, b);
    w64(ctx, block.addr() + 8, arg);
    return command_answer(ctx, rec, 9, block.addr());
}

s32 cellSailRecorderStop(PPUContext& ctx, u32 rec, u32 a, u64 arg)
{
    StackVar block(ctx, gb(ctx), 16);
    w32(ctx, block.addr(), a);
    w64(ctx, block.addr() + 8, arg);
    return command_answer(ctx, rec, 0xB, block.addr());
}

// cellSailRecorderCancel(recorder): a booted recorder that runs gets message 0xE at the queue's head; 0 even when the
// queue's mutex could not be taken.
s32 cellSailRecorderCancel(PPUContext& ctx, u32 rec)
{
    const u32 state = r32(ctx, rec + rc::kState);
    if (state == 0 || state == rc::kEnded || !r32(ctx, rec + rc::kRunning)) return error(CELL_SAIL_ERROR_INVALID_STATE);
    post_head(ctx, rec, 0xE, 0, 0, 0);
    return 0;
}

// ------------------------------------------------------------------------------------------------- DumpImage

// cellSailRecorderDumpImage(recorder, buffer, size): libsail_rec's image of a recorder for Sony's tools, each block at
// the next 64-byte boundary of the buffer: the version (u64 0x492000), libsail_rec's "modules are the process's" flag
// (u32), the recorder's first 0x880 bytes; with a profile in use, the profile (0x568 bytes), its audio, video and user
// parameters (0x108, 0x220 and 0x18 bytes each, one per stream); with the MP4 composer, the composer (0x550) and its
// streams (0x90 each, linked at +0x18); every audio slot (0x2B8) with its feeder's first 0xA0 bytes, every video slot
// (0x460) with its converter (0x18) and its feeder's first 0xA8. It answers the bytes it got through, the padding
// included; a block that does not fit ends the image (the padding before it counted when it fits).
class Dump {
public:
    Dump(PPUContext& ctx, u32 buffer, u32 size) : ctx_(ctx), at_(buffer), left_(size) {}

    // The next block at a 64-byte boundary, `size` bytes from `src` (guest memory) or `host`; false when its padding
    // or the block does not fit.
    bool block(u32 src, u32 size, const u8* host = nullptr)
    {
        const u32 aligned = (at_ + 0x3F) & ~0x3Fu, pad = aligned - at_;
        if (left_ < pad) return false;
        left_ -= pad;
        done_ += pad;
        at_ = aligned;
        if (left_ < size) return false;
        if (host) std::memcpy(gb(ctx_) + at_, host, size);
        else std::memmove(gb(ctx_) + at_, gb(ctx_) + src, size);
        at_ += size;
        left_ -= size;
        done_ += size;
        return true;
    }
    s32 done() const { return static_cast<s32>(done_); }

private:
    PPUContext& ctx_;
    u32 at_, left_, done_ = 0;
};

s32 cellSailRecorderDumpImage(PPUContext& ctx, u32 rec, u32 buffer, u32 size)
{
    Dump d(ctx, buffer, size);
    const u8 version[8] = {0, 0, 0, 0, 0, 0x49, 0x20, 0};
    u8 flag[4];
    ps_st32(flag, modules_flag(ctx));
    if (!d.block(0, 8, version) || !d.block(0, 4, flag) || !d.block(rec, rc::kDumpSize)) return d.done();
    if (const u32 profile = r32(ctx, rec + rc::kProfile)) {
        if (!d.block(profile, 0x568)) return d.done();
        for (u32 i = 0; i < r32(ctx, rec + rc::kAudioCount); i++)
            if (!d.block(r32(ctx, profile + 0x550) + i * 0x108, 0x108)) return d.done();
        for (u32 i = 0; i < r32(ctx, rec + rc::kVideoCount); i++)
            if (!d.block(r32(ctx, profile + 0x554) + i * 0x220, 0x220)) return d.done();
        for (u32 i = 0; i < r32(ctx, rec + rc::kUserCount); i++)
            if (!d.block(r32(ctx, profile + 0x558) + i * 0x18, 0x18)) return d.done();
    }
    if (const u32 mp4 = r32(ctx, rec + rc::kMp4)) {
        if (!d.block(mp4, 0x550)) return d.done();
        for (u32 s = r32(ctx, mp4 + 0x4C0); s; s = r32(ctx, s + 0x18))
            if (!d.block(s, 0x90)) return d.done();
    }
    for (u32 i = 0; i < r32(ctx, rec + rc::kAudioCount); i++) {
        const u32 s = r32(ctx, rec + rc::kAudio) + i * rc::kAudioStride;
        if (!d.block(s, rc::kAudioStride)) return d.done();
        if (const u32 f = r32(ctx, s + as::kFeeder))
            if (!d.block(f, 0xA0)) return d.done();
    }
    for (u32 i = 0; i < r32(ctx, rec + rc::kVideoCount); i++) {
        const u32 s = r32(ctx, rec + rc::kVideo) + i * rc::kVideoStride;
        if (!d.block(s, rc::kVideoStride)) return d.done();
        if (const u32 c = r32(ctx, s + vs::kConverter))
            if (!d.block(c, 0x18)) return d.done();
        if (const u32 f = r32(ctx, s + vs::kFeeder))
            if (!d.block(f, 0xA8)) return d.done();
    }
    return d.done();
}

} // namespace

// The objects' functions (cellSailRecObjects.cpp) and the control thread's (cellSailRecControl.cpp).
void register_objects(Registry& r);
void register_control(Registry& r);
void register_convert(Registry& r);
void register_compose(Registry& r);
void register_stream(Registry& r);
void register_feed(Registry& r);
void register_audio(Registry& r);
void register_video(Registry& r);

} // namespace pspocket::hle::sailrec

namespace pspocket::hle {

void register_cellSailRec(Registry& r)
{
    using namespace sailrec;
    r.add(kChan, {
        PS_HLE_FUNC(0xC4617DDC, cellSailRecorderBoot),
        PS_HLE_FUNC(0xD37FB694, cellSailRecorderCancel),
        PS_HLE_FUNC(0xE3F56F62, cellSailRecorderCloseStream),
        PS_HLE_FUNC(0x50AFFDC1, cellSailRecorderCreateProfile),
        PS_HLE_FUNC(0x49476A3D, cellSailRecorderCreateVideoConverter),
        PS_HLE_FUNC(0x376C3926, cellSailRecorderDestroyProfile),
        PS_HLE_FUNC(0x455C4709, cellSailRecorderDestroyVideoConverter),
        PS_HLE_FUNC(0x37AAD85F, cellSailRecorderDumpImage),
        PS_HLE_FUNC(0xF57D74E3, cellSailRecorderFinalize),
        PS_HLE_FUNC(0xFF20157B, cellSailRecorderGetParameter),
        PS_HLE_FUNC(0x7A52BF69, cellSailRecorderInitialize),
        PS_HLE_FUNC(0x10C81457, cellSailRecorderOpenStream),
        PS_HLE_FUNC(0x80B711CE, cellSailRecorderRegisterComposer),
        PS_HLE_FUNC(0xE12A2FBE, cellSailRecorderReplaceEventHandler),
        PS_HLE_FUNC(0x3DEAE857, cellSailRecorderSetFeederAudio),
        PS_HLE_FUNC(0x4FEC43A9, cellSailRecorderSetFeederVideo),
        PS_HLE_FUNC(0x0A3EA2A9, cellSailRecorderSetParameter),
        PS_HLE_FUNC(0x4830FAF8, cellSailRecorderStart),
        PS_HLE_FUNC(0x18ECC741, cellSailRecorderStop),
        PS_HLE_FUNC(0x551A95FC, cellSailRecorderSubscribeEvent),
        PS_HLE_FUNC(0xEA7E20FC, cellSailRecorderUnregisterComposer),
        PS_HLE_FUNC(0x54CFB78B, cellSailRecorderUnsubscribeEvent),
    });
    register_objects(r);
    register_control(r);
    register_convert(r);
    register_compose(r);
    register_stream(r);
    register_feed(r);
    register_audio(r);
    register_video(r);
    // Until the control thread and the recording are pspocket's too (later parts of D-918), a game gets Sony's
    // libsail_rec where the system software is installed (D-563).
    r.firmware_first(kChan);
}

} // namespace pspocket::hle
