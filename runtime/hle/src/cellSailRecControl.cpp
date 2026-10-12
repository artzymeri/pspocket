// cellSailRec's control thread (D-918, part 4): libsail_rec's own thread that runs a booted recorder, its task handler
// thread, and the internal functions it hands the game's objects (the feeders' tables, the MP4 composer's file calls).
//
// The control thread ("_libsail-control-ex", made by Boot): it records its stack's top less 2000 bytes (+0x870), takes
// the recorder's lwmutex, makes a SPURS queue of 16 messages of 16 bytes at +0x580 (the SPU tasks talk to the PPU
// through it) and attaches an lv2 event queue to it, starts the task handler thread ("_libsail-task-handler"), makes
// the recorder's clock (mode 0 only, "_saisr0"), sets up the composers, then each audio and video stream's pipeline and
// its feeder (the feeder learns the recorder and its index; its makeup, the game's callback). Then the recorder runs
// (+0x700 1, the state 2, the state event and Boot's completion event when subscribed), the lwmutex is released and the
// messages are taken: the game's commands (with libsail's reply protocol: the answer stored, the caller signalled with
// sys_lwcond_signal_to), the feeders', pipelines' and composers' notifications, the SPU tasks' messages, and
// completions, which drive libsail_rec's continuation engine (a step's record: flags, 0x20... in use and 1 failed, the
// steps still out, the handle of the step it belongs to; a handle is the record's address << 32 | the continuation's
// descriptor). Finalize's message (0x30) unsubscribes every event, stops what runs by the state and ends the loop
// (0x31). Then everything is taken down in the reverse order and the state is 10.
// The task handler pops the SPURS queue as libsail_rec does it inline (its pop index marked while it waits on the lv2
// event queue in 10 ms slices, unmarked when a wait times out, so it notices the recorder's end) and posts each message
// to the control thread's queue (a converter's result handled first).

#include <cstring>
#include <format>

#include "cellSailRec.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/ppu/fpu.h"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

s32 lv2(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return static_cast<s32>(sail::lv2_lib(ctx, name, args));
}

} // namespace

// ------------------------------------------------------------------------------------------------- events

// The command the game waits on completed (event 2, the command's code): sent when it is the one recorded at +0x500.
void command_completed(PPUContext& ctx, u32 rec, u32 command, s32 result)
{
    if (r32(ctx, rec + rc::kCommand) != command) return;
    const u64 arg = r64(ctx, rec + rc::kCommandArg);
    w32(ctx, rec + rc::kCommand, 0);
    w64(ctx, rec + rc::kCommandArg, 0);
    if (!(r64(ctx, rec + rc::kEventMask) & 4)) return;
    event(ctx, rec, 2, command, static_cast<u64>(static_cast<s64>(result)), arg);
}

namespace {

// The state changed (event 3), when subscribed.
void state_event(PPUContext& ctx, u32 rec, u32 state)
{
    if (r64(ctx, rec + rc::kEventMask) & 8) event(ctx, rec, 3, 0, state, 0);
}

} // namespace

// An error (event 1): "error 0x%X" printed first, as libsail_rec does, when subscribed.
void error_event(PPUContext& ctx, u32 rec, s32 err)
{
    if (!(r64(ctx, rec + rc::kEventMask) & 2)) return;
    StackVar text(ctx, gb(ctx), 16);
    std::memcpy(gb(ctx) + text.addr(), "error 0x%X\n", 12);
    sail::lv2_lib(ctx, "_sys_printf", {text.addr(), static_cast<u64>(static_cast<s64>(err))});
    event(ctx, rec, 1, 0, static_cast<u64>(static_cast<s64>(err)), 0);
}

namespace {

// The recorder's clock (0x58 bytes, mode 0): its lwmutex "_saisr0" at +0x10.
s32 clock_init(PPUContext& ctx, u32 c)
{
    std::memset(gb(ctx) + c, 0, 0x58);
    const s32 r = sail::make_lwmutex(ctx, c + 0x10, "_saisr0");
    if (r < 0) return r;
    w32(ctx, c + 8, 0);
    w32(ctx, c + 4, 0);
    return 0;
}

void clock_fini(PPUContext& ctx, u32 c)
{
    sail::destroy_lwmutex(ctx, c + 0x10);
    std::memset(gb(ctx) + c, 0, 0x58);
}

} // namespace

void clock_reset(PPUContext& ctx, u32 c)
{
    w32(ctx, c + 8, 0);
    w32(ctx, c + 4, 0);
}

// The clock (+4 the stream that set it, +8 its owner, +0x28 and +0x30 the source's and the stream's last times, +0x40
// and +0x38 their first, +0x48 the timescale, +0x50 the times running the prediction was kept).
namespace {

// Set by stream `owner` at its time `a` (the source's `b`, `timescale` its).
void clock_set(PPUContext& ctx, u32 c, u32 owner, u64 a, u64 b, u64 timescale)
{
    w32(ctx, c + 4, owner);
    w32(ctx, c + 0x50, 0);
    w64(ctx, c + 0x30, a);
    w64(ctx, c + 0x28, b);
    w64(ctx, c + 0x48, timescale);
    w64(ctx, c + 0x38, a);
    w64(ctx, c + 0x40, b);
}

// The setting stream's next time `a`: the source's time predicted from the times so far (a step `d` on from the
// last); `b` is taken when it lies within `d` of the prediction, else the prediction is kept, at most three times
// running.
void clock_follow(PPUContext& ctx, u32 c, u64 a, u64 b)
{
    const s64 last_a = static_cast<s64>(r64(ctx, c + 0x30)), span_a = last_a - static_cast<s64>(r64(ctx, c + 0x38));
    const s64 last_b = static_cast<s64>(r64(ctx, c + 0x28));
    bool take = span_a == 0;
    s64 predicted = 0;
    if (!take) {
        const double step = static_cast<double>(static_cast<s64>(a) - last_a) *
                            static_cast<double>(last_b - static_cast<s64>(r64(ctx, c + 0x40))) /
                            static_cast<double>(span_a);
        const s64 d = static_cast<s64>(ps_f64_bits(ps_ppu_fctid(step, PS_FPSCR_RN_ZERO)));
        predicted = last_b + d;
        const bool far = static_cast<s64>(b) + d <= predicted || d + predicted <= static_cast<s64>(b);
        if (far) {
            const u32 kept = r32(ctx, c + 0x50) + 1;
            w32(ctx, c + 0x50, kept);
            take = static_cast<s32>(kept) > 3;
        } else {
            take = true;
        }
        if (take) w32(ctx, c + 0x50, 0);
    }
    w64(ctx, c + 0x28, take ? b : static_cast<u64>(predicted));
    w64(ctx, c + 0x30, a);
}

s64 fctidz(double x) { return static_cast<s64>(ps_f64_bits(ps_ppu_fctid(x, PS_FPSCR_RN_ZERO))); }

} // namespace

// An audio stream's time (libsail_rec's audio side): the first stream to tell the clock owns it (+8) and sets it.
void clock_update(PPUContext& ctx, u32 c, u32 owner, u64 a, u64 b, u64 timescale)
{
    if (lv2(ctx, "sys_lwmutex_lock", {c + 0x10, 0}) < 0) return;
    const u32 held = r32(ctx, c + 8);
    if (!held) {
        w32(ctx, c + 8, owner);
        clock_set(ctx, c, owner, a, b, timescale);
    } else if (held == owner) {
        clock_follow(ctx, c, a, b);
    }
    lv2(ctx, "sys_lwmutex_unlock", {c + 0x10});
}

// A video stream's time (libsail_rec's video side): the clock unset (+4), it sets it (the owner cleared) and keeps its
// frame's time `b`; the setting stream follows it; another stream's time `a` is mapped through the setting stream's
// times so far into the source's (in timescale `timescale`, double arithmetic as libsail_rec's). A lock failure answers
// the error.
u64 clock_time(PPUContext& ctx, u32 c, u32 owner, u64 a, u64 b, u64 timescale)
{
    const s32 lr = static_cast<s32>(lv2(ctx, "sys_lwmutex_lock", {c + 0x10, 0}));
    if (lr < 0) return static_cast<u64>(static_cast<s64>(lr));
    u64 t = b;
    const u32 set_by = r32(ctx, c + 4);
    if (!set_by) {
        w32(ctx, c + 8, 0);
        clock_set(ctx, c, owner, a, b, timescale);
    } else if (set_by == owner) {
        clock_follow(ctx, c, a, b);
    } else {
        const u64 last_a = r64(ctx, c + 0x30);
        const double ts = static_cast<double>(static_cast<s64>(r64(ctx, c + 0x48)));
        const double scale = static_cast<double>(static_cast<s64>(timescale));
        const double span = scale * (static_cast<double>(static_cast<s64>(last_a - r64(ctx, c + 0x38))) / ts);
        if (span != 0.0) {
            const double from = static_cast<double>(static_cast<s64>(last_a)) / ts * scale;
            // fctidz, and from 2^63 the PPU's unsigned conversion (libsail_rec's).
            const u64 origin = from >= 9223372036854775808.0
                                   ? static_cast<u64>(fctidz(from - 9223372036854775808.0)) ^ 0x8000000000000000ull
                                   : static_cast<u64>(fctidz(from));
            const double source = scale * (static_cast<double>(static_cast<s64>(r64(ctx, c + 0x28) - r64(ctx, c + 0x40))) / ts);
            t = origin + static_cast<u64>(
                             fctidz(source * static_cast<double>(static_cast<s64>(a - origin)) / span));
        }
    }
    lv2(ctx, "sys_lwmutex_unlock", {c + 0x10});
    return t;
}

namespace {

// The composers' table (+0x770, 0x34 bytes): none in use (-1), the recorder.
void composers_init(PPUContext& ctx, u32 rec)
{
    std::memset(gb(ctx) + rec + rc::kComposers, 0, 0x34);
    w32(ctx, rec + rc::kComposers + 0x2C, 0xFFFFFFFF);
    w32(ctx, rec + rc::kComposers + 0x30, rec);
}

// ------------------------------------------------------------------------------------------------- replies

} // namespace


// The control thread's side of a command: the answer stored at the record's address, the waiting thread signalled
// (sys_lwcond_signal_to), the lwmutex released.
void reply(PPUContext& ctx, u32 rec, u32 record, s32 answer)
{
    w32(ctx, r32(ctx, record), static_cast<u32>(answer));
    lv2(ctx, "sys_lwcond_signal_to", {rec + rc::kCond, r64(ctx, record + 8)});
    unlock(ctx, rec);
}

namespace {

// ------------------------------------------------------------------------------------------------- profiles

// CreateProfile (message 1, {type, profile*}): one block from the allocator ((2 * video + audio + user) * 512 + 4096
// bytes, boundary 8): the profile (0x438.. all ones), its audio parameters at +0x1000 (0x108 bytes each, 0x200 apart),
// video (0x220 each, 0x400 apart) and user (0x18 each) after, each entry {1, index, profile, all ones}; the recorder
// kept, the profile put first on the recorder's list (+0x828, linked at +0x560) and given to the game.
void create_profile(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    lock(ctx, rec);
    if (!r32(ctx, rec + rc::kState)) return reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_STATE));
    const u32 a = r32(ctx, rec + rc::kAudioCount), v = r32(ctx, rec + rc::kVideoCount), u = r32(ctx, rec + rc::kUserCount);
    const u32 type = r32(ctx, block);
    const u32 size = ((v * 2 + a + u) << 9) + 0x1000;
    const u32 p = alloc(ctx, rec, 8, size);
    if (!p) return reply(ctx, rec, record, error(CELL_SAIL_ERROR_MEMORY));
    std::memset(gb(ctx) + p, 0, size);
    const u32 pa = p + 0x1000, pv = pa + (a << 9), pu = pv + (v << 10);
    w32(ctx, p + 0x558, pu);
    w32(ctx, p + 0x554, pv);
    w32(ctx, p + 0x550, pa);
    std::memset(gb(ctx) + p + 0x438, 0xFF, 0x118);
    w32(ctx, p + 0x438, type);
    w32(ctx, p, 1);
    w32(ctx, p + 0x55C, rec);
    for (u32 i = 0; i < a; i++) {
        const u32 e = pa + i * 0x108;
        w32(ctx, e + 4, i);
        w32(ctx, e, 1);
        w32(ctx, e + 8, p);
        std::memset(gb(ctx) + e + 0x10, 0xFF, 0xF8);
    }
    for (u32 i = 0; i < v; i++) {
        const u32 e = pv + i * 0x220;
        w32(ctx, e + 4, i);
        w32(ctx, e, 1);
        w32(ctx, e + 8, p);
        std::memset(gb(ctx) + e + 0x10, 0xFF, 0x210);
    }
    for (u32 i = 0; i < u; i++) {
        const u32 e = pu + i * 0x18;
        w32(ctx, e + 4, i);
        w32(ctx, e, 1);
        w32(ctx, e + 8, p);
        std::memset(gb(ctx) + e + 0xC, 0xFF, 0xC);
    }
    w32(ctx, p + 0x560, r32(ctx, rec + rc::kProfiles));
    w32(ctx, rec + rc::kProfiles, p);
    w32(ctx, r32(ctx, block + 4), p);
    reply(ctx, rec, record, 0);
}

// A profile freed (libsail_rec's): its first 4 KB cleared, then given back.
s32 free_profile(PPUContext& ctx, u32 rec, u32 p)
{
    std::memset(gb(ctx) + p, 0, 0x1000);
    dealloc(ctx, rec, 8, p);
    return 0;
}

// DestroyProfile (message 2, the profile): taken off the list and freed; 0 also for a profile not there.
void destroy_profile(PPUContext& ctx, u32 rec, u32 p, u32 record)
{
    lock(ctx, rec);
    for (u32 link = rec + rc::kProfiles, cur; (cur = r32(ctx, link)) != 0; link = cur + 0x560) {
        if (cur != p) continue;
        w32(ctx, link, r32(ctx, p + 0x560));
        const s32 r = free_profile(ctx, rec, p);
        if (r < 0) error_event(ctx, rec, r);
        break;
    }
    reply(ctx, rec, record, 0);
}

// ------------------------------------------------------------------------------------------------- the loop

// The continuation engine's completion {0, result, handle}: a failure marks the step's record (and is an error event
// unless it is a cancellation); when the record's last step is in, its continuation runs with 0 or CANCEL (any step
// failed); an answer above 0 is the number of steps it sent out again, else the record's own completion is posted to
// the step it belongs to.
void completion(PPUContext& ctx, u32 rec, s32 result, u32 record, u32 continuation)
{
    if (result < 0) {
        w32(ctx, record, r32(ctx, record) | 1);
        if (static_cast<u32>(result) != CELL_SAIL_ERROR_CANCEL) error_event(ctx, rec, result);
    }
    const u32 left = r32(ctx, record + 4) - 1;
    w32(ctx, record + 4, left);
    if (left) return;
    const s32 status = (r32(ctx, record) & 1) ? error(CELL_SAIL_ERROR_CANCEL) : 0;
    const s32 answer = static_cast<s32>(static_cast<u32>(call_guest(ctx, continuation, {rec, record,
        static_cast<u64>(static_cast<s64>(status))})));
    if (answer > 0) {
        w32(ctx, record + 4, static_cast<u32>(answer));
        return;
    }
    post_completion(ctx, rec, answer, r64(ctx, record + 8));
}

// The message loop: until message 0x31. +0x510 is 200 while it waits for a message, then the message's first word.
void message_loop(PPUContext& ctx, u32 rec)
{
    const u32 q = rec + rc::kQueue;
    for (;;) {
        w32(ctx, rec + rc::kMessage, 200);
        u32 m0 = 0, m1 = 0, m2 = 0, m3 = 0;
        if (sc(ctx, 102, {r32(ctx, q + 0x14), 0}) >= 0) {
            while (!r32(ctx, q + 4)) sc(ctx, 107, {r32(ctx, q + 0x18), 3000000});
            const u32 read = r32(ctx, q + 0x10), at = r32(ctx, q + 8) + 16 * read, cap = r32(ctx, q);
            m1 = r32(ctx, at + 4);
            m3 = r32(ctx, at + 12);
            m0 = r32(ctx, at);
            m2 = r32(ctx, at + 8);
            w32(ctx, q + 0x10, cap ? (read + 1) % cap : 0);
            w32(ctx, q + 4, r32(ctx, q + 4) - 1);
            sc(ctx, 108, {r32(ctx, q + 0x18)});
            sc(ctx, 104, {r32(ctx, q + 0x14)});
        }
        w32(ctx, rec + rc::kMessage, m0);
        const u32 code = m0 & 0xFFFF;
        switch (code) {
        case 0: completion(ctx, rec, static_cast<s32>(m1), m2, m3); break;
        case 1: create_profile(ctx, rec, m2, m3); break;
        case 2: destroy_profile(ctx, rec, m2, m3); break;
        case 3: create_converter(ctx, rec, m2, m3); break;
        case 4: destroy_converter(ctx, rec, m2, m3); break;
        case 5: open_stream(ctx, rec, m2, m3); break;
        case 7: close_stream(ctx, rec, m2, m3); break;
        case 9: start(ctx, rec, m2, m3); break;
        case 0xB: stop(ctx, rec, m2, m3); break;
        case 0xC: stop_flush(ctx, rec, m2, m3); break;
        case 0x2E: composer_session_error(ctx, rec, m3); break;
        case 0xF: case 0x19: feeder_completed(ctx, m2, static_cast<s32>(m3)); break;
        case 0x10: case 0x11: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x2B:
            audio_message(ctx, rec, code, m2, m3);
            break;
        case 0x1A: case 0x1B: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x2C:
            video_message(ctx, rec, code, m2, m3);
            break;
        case 0xE:
            // Cancel: each feeder's cancel (the game's callback 6).
            for (u32 i = 0; i < r32(ctx, rec + rc::kAudioCount); i++)
                feeder_cancel(ctx, r32(ctx, r32(ctx, rec + rc::kAudio) + i * rc::kAudioStride + as::kFeeder));
            for (u32 i = 0; i < r32(ctx, rec + rc::kVideoCount); i++)
                feeder_cancel(ctx, r32(ctx, r32(ctx, rec + rc::kVideo) + i * rc::kVideoStride + vs::kFeeder));
            break;
        case 0x30: {
            // Finalize: the stop record at +0x450 in use, the recorder no longer running, every event unsubscribed.
            const u32 r = rec + rc::kStopRecord;
            std::memset(gb(ctx) + r, 0, 0x20);
            w32(ctx, r, r32(ctx, r) | 2);
            w64(ctx, r + 8, 0);
            w32(ctx, rec + rc::kRunning, 0);
            w64(ctx, rec + rc::kEventMask, 0);
            finalize_by_state(ctx, rec);
            break;
        }
        case 0x31: return;
        case 0x32: case 0x33: case 0x34: case 0x35: converter_notified(ctx, m2); break;
        // Messages libsail_rec's loop passes over.
        case 6: case 8: case 0xA: case 0xD: case 0x12: case 0x13: case 0x1C: case 0x1D: case 0x24: case 0x25:
        case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2D: case 0x2F: break;
        default: break;
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------- the threads

void control_thread(PPUContext& ctx, u64 recorder)
{
    const u32 rec = static_cast<u32>(recorder);
    w32(ctx, rec + rc::kCommand, 0x20);
    {
        StackVar info(ctx, gb(ctx), 16);
        sc(ctx, 49, {info.addr()});
        w64(ctx, rec + rc::kStackTop, static_cast<u32>(r32(ctx, info.addr()) + r32(ctx, info.addr() + 4) - 2000));
    }
    bool failed = true;
    s32 r = lock(ctx, rec);
    if (r < 0) {
        w32(ctx, rec + rc::kState, rc::kEnded);
    } else {
        // What the end takes down: the queue's lv2 event queue (detached once it was attached and the task handler
        // creation was tried), the task handler (joined), the clock (also after its set-up failed), the slots made.
        bool detach = false, join = false;
        u32 clock = 0, audio_made = 0, video_made = 0;
        const u32 queue = rec + rc::kSpursQueue;
        r = static_cast<s32>(sail::module_call(ctx, "cellSpurs", "_cellSpursQueueInitialize",
            {r32(ctx, rec + rc::kSpurs), 0, queue, rec + rc::kSpursQueueBuffer, 16, 16, 1}));
        if (r >= 0)
            r = static_cast<s32>(sail::module_call(ctx, "cellSpurs", "cellSpursQueueAttachLv2EventQueue", {queue}));
        if (r >= 0) {
            detach = true;
            w32(ctx, rec + rc::kTaskRunning, 1);
            StackVar name(ctx, gb(ctx), 32);
            std::memcpy(gb(ctx) + name.addr(), "_libsail-task-handler", 22);
            r = lv2(ctx, "sys_ppu_thread_create", {rec + rc::kTaskThread, internal("_pspocket_cellSailRec_task"), rec,
                r32(ctx, rec + rc::kTaskPrio), r32(ctx, rec + rc::kTaskStack), 1, name.addr()});
        }
        if (r >= 0) {
            join = true;
            if (r32(ctx, rec + rc::kMode) == 0) {
                clock = rec + rc::kClock;
                r = clock_init(ctx, clock);
            }
        }
        if (r >= 0) {
            w32(ctx, rec + rc::kClockInUse, clock);
            composers_init(ctx, rec);
            for (; audio_made < r32(ctx, rec + rc::kAudioCount); audio_made++) {
                const u32 s = r32(ctx, rec + rc::kAudio) + audio_made * rc::kAudioStride;
                audio_pipeline_init(ctx, s + as::kPipeline, rec, clock);
                r = feeder_makeup(ctx, r32(ctx, s + as::kFeeder), rec, audio_made);
                if (r < 0) {
                    audio_pipeline_fini(ctx, s + as::kPipeline);
                    break;
                }
            }
            if (r >= 0) {
                for (; video_made < r32(ctx, rec + rc::kVideoCount); video_made++) {
                    const u32 s = r32(ctx, rec + rc::kVideo) + video_made * rc::kVideoStride;
                    video_pipeline_init(ctx, s + vs::kPipeline, rec, clock);
                    r = feeder_makeup(ctx, r32(ctx, s + vs::kFeeder), rec, video_made);
                    if (r < 0) {
                        video_pipeline_fini(ctx, s + vs::kPipeline);
                        break;
                    }
                }
            }
            if (r >= 0) {
                failed = false;
                w32(ctx, rec + rc::kRunning, 1);
                w32(ctx, rec + rc::kState, rc::kBooted);
                state_event(ctx, rec, rc::kBooted);
                command_completed(ctx, rec, 0x20, 0);
                lv2(ctx, "sys_lwcond_signal", {rec + rc::kCond});
                unlock(ctx, rec);
                message_loop(ctx, rec);
                lock(ctx, rec);
            }
        }
        // The end, in the reverse order (libsail_rec's; the composers' end does nothing).
        for (u32 i = video_made; i-- > 0;) {
            const u32 s = r32(ctx, rec + rc::kVideo) + i * rc::kVideoStride;
            feeder_cleanup(ctx, r32(ctx, s + vs::kFeeder));
            video_pipeline_fini(ctx, s + vs::kPipeline);
        }
        for (u32 i = audio_made; i-- > 0;) {
            const u32 s = r32(ctx, rec + rc::kAudio) + i * rc::kAudioStride;
            feeder_cleanup(ctx, r32(ctx, s + as::kFeeder));
            audio_pipeline_fini(ctx, s + as::kPipeline);
        }
        if (clock) clock_fini(ctx, clock);
        if (const u32 profile = r32(ctx, rec + rc::kProfile)) {
            profile_composer_release(ctx, rec, profile);
            w32(ctx, rec + rc::kProfile, 0);
        }
        for (u32 p = r32(ctx, rec + rc::kProfiles); p;) {
            const u32 next = r32(ctx, p + 0x560);
            free_profile(ctx, rec, p);
            p = next;
        }
        w32(ctx, rec + rc::kProfiles, 0);
        destroy_converters(ctx, rec);
        w32(ctx, rec + rc::kTaskRunning, 0);
        w32(ctx, rec + rc::kConverters, 0);
        if (join) {
            StackVar status(ctx, gb(ctx), 16);
            sc(ctx, 44, {r64(ctx, rec + rc::kTaskThread), status.addr()});
        }
        if (detach) sail::module_call(ctx, "cellSpurs", "cellSpursQueueDetachLv2EventQueue", {queue});
        unlock(ctx, rec);
        w32(ctx, rec + rc::kState, rc::kEnded);
    }
    if (failed) {
        state_event(ctx, rec, rc::kFinalized);
        command_completed(ctx, rec, 0x20, r);
    }
    lv2(ctx, "sys_ppu_thread_exit", {0});
}

// The task handler: libsail_rec's own pop of the SPURS queue (SPU to PPU, as libsre's but waiting in 10 ms slices), each
// message posted to the control thread's queue as {code, 0, a, b} (a converter's result, codes above 0x32, handled
// first); it ends when the recorder no longer has it run (+0x514) or a pop fails other than by a wait timing out.
void task_thread(PPUContext& ctx, u64 recorder)
{
    const u32 rec = static_cast<u32>(recorder);
    const u32 queue = rec + rc::kSpursQueue;
    u8* base = gb(ctx);
    constexpr u32 kTimedOut = 0x8001000B, kPerm = 0x80410909, kBusy = 0x8041090A, kStat = 0x8041090F;
    StackVar msg(ctx, base, 16);
    StackVar ev(ctx, base, 32);
    for (;;) {
        s32 r = static_cast<s32>(kPerm);
        if (r32(ctx, queue + 0x1C) == 1) {
            bool waited = false;
            for (;;) {
                // Claim the entry at the pop index (mark it).
                u32 index = 0, count = 0;
                r = 0;
                reservation_op(base, queue, [&](u8* qq) {
                    u32 pop = ps_read32(qq, 0);
                    if (!waited && static_cast<s32>(pop) < 0) {
                        r = static_cast<s32>(kBusy);
                        return false;
                    }
                    if (waited) pop = ~pop;
                    if (ps_read32(qq, 0x18) == 0xFFFFFFFF) {
                        r = static_cast<s32>(kStat);
                        return false;
                    }
                    u32 push = ps_read32(qq, 4);
                    if (static_cast<s32>(push) < 0) push = ~push;
                    const u32 depth = ps_read32(qq, 0xC);
                    count = pop <= push ? push - pop : push + depth - (pop - depth);
                    index = pop < depth ? pop : pop - depth;
                    ps_write32(qq, 0, ~pop);
                    return true;
                });
                if (r < 0) break;
                if (count) {
                    const u32 size = r32(ctx, queue + 8);
                    std::memmove(base + msg.addr(), base + static_cast<u32>(r64(ctx, queue + 0x10)) + size * index,
                        size < 16 ? size : 16);
                    bool wake = false;
                    u8 waiter = 0, wid = 0;
                    reservation_op(base, queue, [&](u8* qq) {
                        const u32 pop = ~ps_read32(qq, 0), depth = ps_read32(qq, 0xC);
                        u32 push = ps_read32(qq, 4);
                        if (static_cast<s32>(push) < 0) push = ~push;
                        const u32 n = pop <= push ? push - pop : push + depth - (pop - depth);
                        wake = ps_read8(qq, 0x30) != 0 && n == depth;
                        waiter = ps_read8(qq, 0x31);
                        wid = ps_read8(qq, 0x51);
                        ps_write32(qq, 0, pop + 1 < 2 * depth ? pop + 1 : 0);
                        return true;
                    });
                    if (wake) {
                        u32 taskset = static_cast<u32>(r64(ctx, queue + 0x60));
                        if (!taskset) {
                            StackVar t(ctx, base, 16);
                            sail::module_call(ctx, "cellSpurs", "cellSpursLookUpTasksetAddress",
                                {r32(ctx, queue + 0x6C), t.addr(), wid});
                            taskset = r32(ctx, t.addr());
                        }
                        sail::module_call(ctx, "cellSpurs", "_cellSpursSendSignal", {taskset, waiter});
                    }
                    r = 0;
                    break;
                }
                // Empty: wait for the SPU push's event, 10 ms at a time; a wait that times out gives the claim back.
                r = sc(ctx, 130, {r32(ctx, queue + 0x74), ev.addr(), 10000});
                if (static_cast<u32>(r) == kTimedOut) {
                    reservation_op(base, queue, [&](u8* qq) {
                        ps_write32(qq, 0, ~ps_read32(qq, 0));
                        return true;
                    });
                    break;
                }
                waited = true;
            }
        }
        if (!r32(ctx, rec + rc::kTaskRunning)) break;
        if (r < 0) {
            if (static_cast<u32>(r) == kTimedOut) continue;
            break;
        }
        const u32 code = r32(ctx, msg.addr()), a = r32(ctx, msg.addr() + 4), b = r32(ctx, msg.addr() + 8);
        if (code > 0x32) converter_done(ctx, a, b);
        post(ctx, rec, code, a, b);
    }
    lv2(ctx, "sys_ppu_thread_exit", {0});
}

namespace {

void _pspocket_cellSailRec_control(PPUContext& ctx, u64 rec) { control_thread(ctx, rec); }
void _pspocket_cellSailRec_task(PPUContext& ctx, u64 rec) { task_thread(ctx, rec); }

#define SAILREC_LATER(name, what) \
    s32 name(PPUContext& ctx) { later(ctx, what); }

SAILREC_LATER(_pspocket_cellSailRec_FsOpen, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsClose, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsFstat, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsRead, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsLseek, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsNone, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsWrite, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsRename, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsUnlink, "the MP4 composer's file calls")

#undef SAILREC_LATER

#define SAILREC_INTERNAL(fn) PS_HLE_INTERNAL(recomp::nid_of(#fn), fn)

} // namespace

void register_control(Registry& r)
{
    r.add(kChan, {
        SAILREC_INTERNAL(_pspocket_cellSailRec_control),
        SAILREC_INTERNAL(_pspocket_cellSailRec_task),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsOpen),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsClose),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsFstat),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsRead),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsLseek),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsNone),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsWrite),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsRename),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FsUnlink),
    });
}

#undef SAILREC_INTERNAL

} // namespace pspocket::hle::sailrec
