// cellSailRec's streams (D-918, part 6 on), as libsail_rec (fw 4.92) runs them on its control thread: OpenStream,
// CloseStream, Start, Stop and Finalize's stops by the state, each a tree of asynchronous steps over the composer in use
// (cellSailRecCompose.cpp), each stream's feeder (the game's, cellSailRecFeed.cpp) and pipeline (the encoders).
//
// A step's record is {flags (2 in use, 1 a step failed), the steps still out, the handle of the step it belongs to},
// more after it where a step needs it; a handle is the record's address << 32 | the continuation's descriptor (one of
// the internal functions below). A step's end posts {0, result, handle}; the control thread's engine (message 0) counts
// the record's steps down and runs its continuation with 0 or CANCEL (cellSailRecControl.cpp). The records: +0x400 the
// game's command (OpenStream, CloseStream, Start, the flushing stop), +0x410 its step over the streams (+0x420 the next
// step's), +0x450 Finalize's (+0x460 its stop), +0x470 a composer's session error (+0x480 its stop); each slot's record
// at its start (its open, close and start steps; +0x18 the close after a failed open) and at +0x30 (its stop).
//
// The streams' state (+0x768): 0 none, 1 opening, 2 open, 3 starting, 4 started, 5 stopping, 6 stopping by the game's
// Stop, 7 closing. A slot's (its byte at +0x295 audio, +0x42D video): 0 closed, 1 open, 2 closing, 3 opening, 4
// starting, 5 recording, 6 stopping (Stop), 7 stopping (everything).

#include <cstring>
#include <format>

#include "cellSailRec.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

constexpr u32 kCancel = 0x80610709;

u64 handle(u32 record, std::string_view continuation) { return u64{record} << 32 | internal(continuation); }

u32 audio_slot(PPUContext& ctx, u32 rec, u32 i) { return r32(ctx, rec + rc::kAudio) + i * rc::kAudioStride; }
u32 video_slot(PPUContext& ctx, u32 rec, u32 i) { return r32(ctx, rec + rc::kVideo) + i * rc::kVideoStride; }
u32 audio_count(PPUContext& ctx, u32 rec) { return r32(ctx, rec + rc::kAudioCount); }
u32 video_count(PPUContext& ctx, u32 rec) { return r32(ctx, rec + rc::kVideoCount); }
u32 cs_of(u32 rec) { return rec + rc::kComposers; }
bool subscribed(PPUContext& ctx, u32 rec, u64 bit) { return (r64(ctx, rec + rc::kEventMask) & bit) != 0; }

// A record begun: `size` bytes cleared, in use, its step's handle.
void record_begin(PPUContext& ctx, u32 record, u32 size, u64 parent)
{
    std::memset(gb(ctx) + record, 0, size);
    w32(ctx, record, r32(ctx, record) | 2);
    w64(ctx, record + 8, parent);
}

void set_state(PPUContext& ctx, u32 rec, u32 state)
{
    w32(ctx, rec + rc::kState, state);
    if (subscribed(ctx, rec, 8)) event(ctx, rec, 3, 0, state, 0);
}

// ------------------------------------------------------------------------------------------------- the slots

// A slot closed: out of the composer, its pipeline and its feeder closed (three steps on the slot's record `record`).
void close_slot(PPUContext& ctx, u32 rec, u32 record, u64 parent, u32 slot, bool video)
{
    std::memset(gb(ctx) + record, 0, 0x18);
    w32(ctx, record, r32(ctx, record) | 2);
    const u64 h = handle(record, video ? "_pspocket_cellSailRec_SlotClosedVideo" : "_pspocket_cellSailRec_SlotClosedAudio");
    w64(ctx, record + 8, parent);
    w32(ctx, record + 0x10, slot);
    w8(ctx, slot + (video ? vs::kState : as::kState), 2);
    composer_remove(ctx, cs_of(rec), slot + (video ? vs::kComposerStream : as::kComposerStream), h, video);
    if (video) {
        video_pipeline_close(ctx, slot + vs::kPipeline, h);
        feeder_close(ctx, r32(ctx, slot + vs::kFeeder), h);
    } else {
        audio_pipeline_close(ctx, slot + as::kPipeline, h);
        feeder_close(ctx, r32(ctx, slot + as::kFeeder), h);
    }
    w32(ctx, record + 4, 3);
}

} // namespace

// Every open slot closed and the composer closed (record `record`, its parent's handle).
void close_all(PPUContext& ctx, u32 rec, u32 record, u64 parent)
{
    record_begin(ctx, record, 0x10, parent);
    w32(ctx, rec + rc::kStreamState, 7);
    const u64 h = handle(record, "_pspocket_cellSailRec_AllClosed");
    u32 steps = 1;
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (!r8(ctx, slot + as::kState)) continue;
        close_slot(ctx, rec, slot, h, slot, false);
        steps++;
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (!r8(ctx, slot + vs::kState)) continue;
        close_slot(ctx, rec, slot, h, slot, true);
        steps++;
    }
    composer_close(ctx, cs_of(rec), h);
    w32(ctx, record + 4, steps);
}

// Every starting, recording or stopping slot stopped (its feeder and its pipeline, the slot 7) and the composer stopped
// with `flag`.
void stop_all(PPUContext& ctx, u32 rec, u32 record, u32 flag, u64 parent)
{
    record_begin(ctx, record, 0x10, parent);
    w32(ctx, rec + rc::kStreamState, 5);
    const u64 h = handle(record, "_pspocket_cellSailRec_AllStopped");
    u32 steps = 1;
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + as::kState) - 4) > 2) continue;
        steps += 2;
        w8(ctx, slot + as::kState, 7);
        feeder_stop(ctx, r32(ctx, slot + as::kFeeder), 1, 0, h);
        audio_pipeline_stop(ctx, slot + as::kPipeline, 1, h);
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + vs::kState) - 4) > 2) continue;
        steps += 2;
        w8(ctx, slot + vs::kState, 7);
        feeder_stop(ctx, r32(ctx, slot + vs::kFeeder), 1, 0, h);
        video_pipeline_stop(ctx, slot + vs::kPipeline, 1, h);
    }
    composer_stop(ctx, cs_of(rec), flag, h);
    w32(ctx, record + 4, steps);
}

namespace {

// Finalize's stop of a recorder whose stream is open: everything closed, then the loop ended.
s32 close_for_end(PPUContext& ctx, u32 rec, u32 record)
{
    w32(ctx, rec + rc::kState, rc::kClosing);
    w32(ctx, record + 4, 1);
    close_all(ctx, rec, record + 0x10, handle(record, "_pspocket_cellSailRec_EndLoop"));
    return 0;
}

} // namespace

// The profile's composer let go at the stream's close: the MP4 composer's user less (libmp4 destroys it after the last;
// its modules unloaded), or the game's composer detached; another type 0x80610703.
s32 profile_composer_release(PPUContext& ctx, u32 rec, u32 profile)
{
    const u32 type = r32(ctx, profile + pf::kType);
    if (type == 1) {
        const u32 mp4 = r32(ctx, rec + rc::kMp4);
        if (!mp4) return 0;
        const u32 users = r32(ctx, mp4 + 0x58) - 1;
        w32(ctx, mp4 + 0x58, users);
        if (!users) {
            mp4_composer_destroy(ctx, rec, mp4);
            dealloc(ctx, rec, 8, mp4);
            w32(ctx, rec + rc::kMp4, users);
        }
        unload_module(ctx, 0xF031);
        unload_module(ctx, 0xE);
        return 0;
    }
    if (type == 3 && static_cast<s32>(r32(ctx, rec + rc::kComposerSet)) > 0) {
        composer_detach(ctx, r32(ctx, rec + rc::kComposer), cs_of(rec));
        return 0;
    }
    return error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
}

namespace {

// The command completed under the recorder's lwmutex (its lock's error an error event instead).
void command_completed_locked(PPUContext& ctx, u32 rec, u32 command, s32 result)
{
    const s32 r = lock(ctx, rec);
    if (r < 0) {
        error_event(ctx, rec, r);
        return;
    }
    command_completed(ctx, rec, command, result);
    unlock(ctx, rec);
}

void finalize_if_ended(PPUContext& ctx, u32 rec)
{
    if (!r32(ctx, rec + rc::kRunning)) finalize_by_state(ctx, rec);
}

// ------------------------------------------------------------------------------------------------- continuations

// Each continuation(recorder, record, status): its answer is the steps it sent out again (above 0) or the record's own
// result.

s32 _pspocket_cellSailRec_OpenCancelled(PPUContext&) { return error(kCancel); }
s32 _pspocket_cellSailRec_StartCancelled(PPUContext&) { return error(kCancel); }
s32 _pspocket_cellSailRec_AudioOpenCancelled(PPUContext&) { return error(kCancel); }
s32 _pspocket_cellSailRec_VideoOpenCancelled(PPUContext&) { return error(kCancel); }

// Finalize's last step: the loop ends (message 0x31), the state booted.
s32 _pspocket_cellSailRec_EndLoop(PPUContext& ctx, u32 rec)
{
    w32(ctx, rec + rc::kState, rc::kBooted);
    post(ctx, rec, 0x31, 0, 0);
    return 0;
}

s32 _pspocket_cellSailRec_EndClose(PPUContext& ctx, u32 rec, u32 record) { return close_for_end(ctx, rec, record); }

// stop_all's end: the composer told, the streams open, every stopping slot open.
s32 _pspocket_cellSailRec_AllStopped(PPUContext& ctx, u32 rec, u32, s32 status)
{
    composer_completed(ctx, cs_of(rec), status);
    w32(ctx, rec + rc::kStreamState, 2);
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + as::kState) - 6) <= 1) w8(ctx, slot + as::kState, 1);
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + vs::kState) - 6) <= 1) w8(ctx, slot + vs::kState, 1);
    }
    return 0;
}

// Stop's steps for a slot: its feeder stopped, then its pipeline, then its composer stream ended.
s32 _pspocket_cellSailRec_AudioFeederStopped(PPUContext& ctx, u32, u32 record)
{
    audio_pipeline_stop(ctx, r32(ctx, record + 0x10) + as::kPipeline, 0,
        handle(record, "_pspocket_cellSailRec_AudioPipelineStopped"));
    return 1;
}

s32 _pspocket_cellSailRec_VideoFeederStopped(PPUContext& ctx, u32, u32 record)
{
    video_pipeline_stop(ctx, r32(ctx, record + 0x10) + vs::kPipeline, 0,
        handle(record, "_pspocket_cellSailRec_VideoPipelineStopped"));
    return 1;
}

s32 _pspocket_cellSailRec_AudioPipelineStopped(PPUContext& ctx, u32 rec, u32 record)
{
    composer_end(ctx, cs_of(rec), r32(ctx, record + 0x10) + as::kComposerStream,
        handle(record, "_pspocket_cellSailRec_Stopped"), false);
    return 1;
}

s32 _pspocket_cellSailRec_VideoPipelineStopped(PPUContext& ctx, u32 rec, u32 record)
{
    composer_end(ctx, cs_of(rec), r32(ctx, record + 0x10) + vs::kComposerStream,
        handle(record, "_pspocket_cellSailRec_Stopped"), true);
    return 1;
}

// The feeders started: recording (a failure stops everything).
s32 _pspocket_cellSailRec_FeedersStarted(PPUContext& ctx, u32 rec, u32 record, s32 status)
{
    if (status < 0) {
        stop_all(ctx, rec, record + 0x10, 0, handle(record, "_pspocket_cellSailRec_StartCancelled"));
        return 1;
    }
    w32(ctx, rec + rc::kStreamState, 4);
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (r8(ctx, slot + as::kState) == 4) w8(ctx, slot + as::kState, 5);
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (r8(ctx, slot + vs::kState) == 4) w8(ctx, slot + vs::kState, 5);
    }
    return 0;
}

// The composer and the pipelines started: the composer told, then each starting slot's feeder started with the session
// and the recording's start and end times.
s32 _pspocket_cellSailRec_Started(PPUContext& ctx, u32 rec, u32 record, s32 status)
{
    composer_completed(ctx, cs_of(rec), status);
    if (status < 0) {
        stop_all(ctx, rec, record + 0x10, 0, handle(record, "_pspocket_cellSailRec_StartCancelled"));
        return 1;
    }
    const u64 h = handle(record, "_pspocket_cellSailRec_FeedersStarted");
    const u32 session = r32(ctx, rec + rc::kStreamArg);
    const u64 start = r64(ctx, rec + rc::kStartTime), end = r64(ctx, rec + rc::kEndTime);
    s32 steps = 0;
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (r8(ctx, slot + as::kState) != 4) continue;
        steps++;
        feeder_start(ctx, r32(ctx, slot + as::kFeeder), session, start, end, h, false);
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (r8(ctx, slot + vs::kState) != 4) continue;
        steps++;
        feeder_start(ctx, r32(ctx, slot + vs::kFeeder), session, start, end, h, true);
    }
    return steps;
}

// A composer's session error's stop done: open again.
s32 _pspocket_cellSailRec_ErrorStopped(PPUContext& ctx, u32 rec)
{
    w32(ctx, rec + rc::kStreamState, 2);
    set_state(ctx, rec, rc::kOpen);
    return 0;
}

// A slot closed: its composer stream freed, closed (0), event 10 (stream closed: the kind, the index) when subscribed.
s32 slot_closed(PPUContext& ctx, u32 rec, u32 record, bool video)
{
    const u32 slot = r32(ctx, record + 0x10);
    composer_free_stream(ctx, cs_of(rec), slot + (video ? vs::kComposerStream : as::kComposerStream));
    w8(ctx, slot + (video ? vs::kState : as::kState), 0);
    if (subscribed(ctx, rec, 0x400)) event(ctx, rec, 10, 0, video ? 1 : 0, r8(ctx, slot + (video ? vs::kIndex : as::kIndex)));
    return 0;
}

s32 _pspocket_cellSailRec_SlotClosedAudio(PPUContext& ctx, u32 rec, u32 record) { return slot_closed(ctx, rec, record, false); }
s32 _pspocket_cellSailRec_SlotClosedVideo(PPUContext& ctx, u32 rec, u32 record) { return slot_closed(ctx, rec, record, true); }

// A slot opened (its feeder, pipeline and composer stream): its composer stream's capacity asked, open (1), event 9
// (stream opened) when subscribed; a failure closes the slot again (its record's +0x18) and answers CANCEL.
s32 slot_opened(PPUContext& ctx, u32 rec, u32 record, s32 status, bool video)
{
    const u32 slot = r32(ctx, record + 0x10);
    if (status < 0) {
        close_slot(ctx, rec, record + 0x18,
            handle(record, video ? "_pspocket_cellSailRec_VideoOpenCancelled" : "_pspocket_cellSailRec_AudioOpenCancelled"),
            slot, video);
        return 1;
    }
    composer_capacity(ctx, cs_of(rec), slot + (video ? vs::kComposerStream : as::kComposerStream));
    w8(ctx, slot + (video ? vs::kState : as::kState), 1);
    if (subscribed(ctx, rec, 0x200)) event(ctx, rec, 9, 0, video ? 1 : 0, r8(ctx, slot + (video ? vs::kIndex : as::kIndex)));
    return 0;
}

s32 _pspocket_cellSailRec_AudioSlotOpened(PPUContext& ctx, u32 rec, u32 record, s32 status)
{
    return slot_opened(ctx, rec, record, status, false);
}

// An audio slot's feeder open: the stream configured, its pipeline opened and the stream added to the composer (the
// slot opened after both); a failure here is an error event, then the slot closed again.
s32 _pspocket_cellSailRec_AudioFeederOpened(PPUContext& ctx, u32 rec, u32 record, s32 status)
{
    if (status < 0) return slot_opened(ctx, rec, record, status, false);
    const s32 r = audio_stream_open(ctx, rec, record, handle(record, "_pspocket_cellSailRec_AudioSlotOpened"));
    if (r >= 0) return r;
    error_event(ctx, rec, r);
    return slot_opened(ctx, rec, record, r, false);
}

s32 _pspocket_cellSailRec_VideoSlotOpened(PPUContext& ctx, u32 rec, u32 record, s32 status)
{
    return slot_opened(ctx, rec, record, status, true);
}

// Everything closed: the composer told and its instance freed, the profile's composer let go, the profile no longer in
// use, event 5 (stream closed, the profile) when subscribed, no stream.
s32 _pspocket_cellSailRec_AllClosed(PPUContext& ctx, u32 rec, u32, s32 status)
{
    composer_completed(ctx, cs_of(rec), status);
    composer_detach_instance(ctx, cs_of(rec));
    const u32 profile = r32(ctx, rec + rc::kProfile);
    profile_composer_release(ctx, rec, profile);
    w32(ctx, profile, 1);
    w32(ctx, rec + rc::kProfile, 0);
    if (subscribed(ctx, rec, 0x20)) event(ctx, rec, 5, 0, profile, 0);
    w32(ctx, rec + rc::kStreamState, 0);
    return 0;
}

// Every slot opened: the streams open, event 4 (stream opened, the profile) when subscribed.
s32 _pspocket_cellSailRec_AllOpened(PPUContext& ctx, u32 rec)
{
    w32(ctx, rec + rc::kStreamState, 2);
    if (subscribed(ctx, rec, 0x10)) event(ctx, rec, 4, 0, r32(ctx, rec + rc::kProfile), 0);
    return 0;
}

// The composer opened: the composer told (a failure closes everything); each closed slot opened: video first, then
// audio, each slot its own record (its parent the streams' step), its feeder asked to open with the format the
// profile's parameters give (for the MP4 composer and the game's, the defaults: video coding 3 and MPEG-4, audio coding
// 1, 1024 samples a frame, 48 kHz and AAC); the first stream (and each user stream after none) marked as the one the
// composer follows.
s32 _pspocket_cellSailRec_ComposerOpened(PPUContext& ctx, u32 rec, u32 record, s32 status)
{
    composer_completed(ctx, cs_of(rec), status);
    if (status < 0) {
        close_all(ctx, rec, record + 0x10, handle(record, "_pspocket_cellSailRec_OpenCancelled"));
        return 1;
    }
    const u32 profile = r32(ctx, rec + rc::kProfile);
    const u32 type = r32(ctx, profile + pf::kType);
    const bool defaults = type == 1 || type == 3;
    const u64 parent = handle(record, "_pspocket_cellSailRec_AllOpened");
    u32 first = 1;
    s32 steps = 0;
    for (u32 i = 0; i < video_count(ctx, rec); i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (r8(ctx, slot + vs::kState)) continue;
        steps++;
        const u32 params = r32(ctx, profile + pf::kVideo) + i * 0x220;
        w32(ctx, params + 0x10, first);
        std::memset(gb(ctx) + slot, 0, 0x30);
        w32(ctx, slot, r32(ctx, slot) | 2);
        w32(ctx, slot + 0x10, slot);
        w64(ctx, slot + 8, parent);
        w8(ctx, slot + vs::kState, 3);
        StackVar format(ctx, gb(ctx), 0x20);
        std::memmove(gb(ctx) + format.addr(), gb(ctx) + params + 0x40, 0x20);
        if (defaults) {
            u8 coding = r8(ctx, params + 0x40);
            u32 codec = r32(ctx, params + 0x14);
            if (coding == 0xFF) coding = 3;
            if (codec == 0xFFFFFFFF) codec = 0x21;
            w32(ctx, params + 0x14, codec);
            w8(ctx, format.addr(), coding);
        }
        feeder_open(ctx, r32(ctx, slot + vs::kFeeder), slot + vs::kPipeline, slot, format.addr(),
            handle(slot, "_pspocket_cellSailRec_VideoFeederOpened"), true);
        first = 0;
        w32(ctx, slot + 4, 1);
    }
    for (u32 i = 0; i < audio_count(ctx, rec); i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (r8(ctx, slot + as::kState)) continue;
        const u32 params = r32(ctx, profile + pf::kAudio) + i * 0x108;
        w32(ctx, params + 0x10, first);
        std::memset(gb(ctx) + slot, 0, 0x30);
        w32(ctx, slot, r32(ctx, slot) | 2);
        w32(ctx, slot + 0x10, slot);
        w64(ctx, slot + 8, parent);
        w8(ctx, slot + as::kState, 3);
        StackVar format(ctx, gb(ctx), 0x20);
        std::memmove(gb(ctx) + format.addr(), gb(ctx) + params + 0x38, 0x18);
        if (defaults) {
            u8 coding = r8(ctx, params + 0x38);
            u32 codec = r32(ctx, params + 0x14);
            u16 samples = static_cast<u16>(ps_read16(gb(ctx), params + 0x3A));
            u32 rate = r32(ctx, params + 0x3C);
            if (coding == 0xFF) coding = 1;
            if (codec == 0xFFFFFFFF) {
                codec = 1;
                samples = 0x400;
            } else if (static_cast<s16>(samples) == -1) {
                samples = 0x400;
            }
            if (rate == 0xFFFFFFFF) rate = 48000;
            w32(ctx, params + 0x14, codec);
            w16(ctx, format.addr() + 2, samples);
            w8(ctx, format.addr(), coding);
            w32(ctx, format.addr() + 4, rate);
        }
        feeder_open(ctx, r32(ctx, slot + as::kFeeder), slot + as::kPipeline, slot, format.addr(),
            handle(slot, "_pspocket_cellSailRec_AudioFeederOpened"), false);
        steps++;
        first = 0;
        w32(ctx, slot + 4, 1);
    }
    for (u32 i = 0, n = r32(ctx, rec + rc::kUserCount); i < n; i++) {
        w32(ctx, r32(ctx, profile + pf::kUser) + i * 0x18 + 0xC, first);
        first = 0;
    }
    return steps;
}

// The game's commands completed.

s32 _pspocket_cellSailRec_Opened(PPUContext& ctx, u32 rec, u32, s32 status)
{
    set_state(ctx, rec, status < 0 ? rc::kBooted : rc::kOpen);
    w32(ctx, rec + rc::kOpenArg, 0);
    command_completed_locked(ctx, rec, 0x21, status);
    finalize_if_ended(ctx, rec);
    return 0;
}

s32 _pspocket_cellSailRec_Closed(PPUContext& ctx, u32 rec)
{
    set_state(ctx, rec, rc::kBooted);
    command_completed_locked(ctx, rec, 0x22, 0);
    finalize_if_ended(ctx, rec);
    return 0;
}

s32 _pspocket_cellSailRec_StartDone(PPUContext& ctx, u32 rec, u32, s32 status)
{
    set_state(ctx, rec, status < 0 ? rc::kOpen : rc::kStarted);
    command_completed_locked(ctx, rec, 0x29, status);
    finalize_if_ended(ctx, rec);
    return 0;
}

// Stop's end (the last slot's composer stream ended): the composer told, every recording or stopping slot open, the
// stream open (4), Stop's completion when it was stopping (7).
s32 _pspocket_cellSailRec_Stopped(PPUContext& ctx, u32 rec, u32, s32 status)
{
    w32(ctx, rec + rc::kStreamState, 2);
    composer_completed(ctx, cs_of(rec), status);
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + as::kState) - 5) <= 2) w8(ctx, slot + as::kState, 1);
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + vs::kState) - 5) <= 2) w8(ctx, slot + vs::kState, 1);
    }
    const u32 old = r32(ctx, rec + rc::kState);
    set_state(ctx, rec, rc::kOpen);
    if (old == rc::kStopping) command_completed_locked(ctx, rec, 0x2A, status);
    finalize_if_ended(ctx, rec);
    return 0;
}

// The stop with the composer's flush (message 0xC) done.
s32 _pspocket_cellSailRec_FlushStopped(PPUContext& ctx, u32 rec, u32, s32 status)
{
    set_state(ctx, rec, rc::kOpen);
    command_completed_locked(ctx, rec, 0x2A, status);
    finalize_if_ended(ctx, rec);
    return 0;
}

} // namespace

// Finalize's stop by the state: recording (6) stopped and then closed, an open stream (4) closed, a booted recorder (2)
// only told to end its loop.
void finalize_by_state(PPUContext& ctx, u32 rec)
{
    switch (r32(ctx, rec + rc::kState)) {
    case rc::kStarted:
        w32(ctx, rec + rc::kState, rc::kStopping);
        w32(ctx, rec + rc::kStopRecord + 4, 1);
        stop_all(ctx, rec, rec + rc::kStopRecord + 0x10, 0, handle(rec + rc::kStopRecord, "_pspocket_cellSailRec_EndClose"));
        break;
    case rc::kOpen: close_for_end(ctx, rec, rec + rc::kStopRecord); break;
    case rc::kBooted: _pspocket_cellSailRec_EndLoop(ctx, rec); break;
    default: break;
    }
}

// ------------------------------------------------------------------------------------------------- the commands

// OpenStream (message 5, {profile, a, path, b, c, 0, argument}): booted, no command pending, the profile not in use;
// the path kept in the profile with a '/' after it (0x421 bytes at most), a, b; the command 0x21 pending with the
// argument, c kept (+0x4D4), opening (3); answered at once. Then the profile in use, its composer made or attached, its
// stream parameters' defaults, the composer opened (its continuation opens the slots). A profile whose composer cannot
// be had is an error event and everything closed (the completion carries CANCEL).
void open_stream(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    lock(ctx, rec);
    const u32 profile = r32(ctx, block);
    if (r32(ctx, rec + rc::kState) != rc::kBooted || r32(ctx, rec + rc::kCommand) || r32(ctx, profile) != 1)
        return reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_STATE));
    const u32 path = r32(ctx, block + 8);
    guest_strncpy(ctx, profile + 8, path, 0x421);
    u32 len = guest_strlen(ctx, path);
    if (len && r8(ctx, path + len - 1) != '/' && len <= 0x41F) {
        w8(ctx, profile + 8 + len, '/');
        len++;
    }
    w8(ctx, profile + 0x428, 0);
    w32(ctx, profile + 4, r32(ctx, block + 4));
    if (len > 0x420) len = 0x420;
    w32(ctx, profile + 0x42C, len);
    w32(ctx, profile + 0x430, r32(ctx, block + 0xC));
    w32(ctx, rec + rc::kCommand, 0x21);
    w64(ctx, rec + rc::kCommandArg, r64(ctx, block + 0x18));
    w32(ctx, rec + rc::kOpenArg, r32(ctx, block + 0x10));
    set_state(ctx, rec, rc::kOpening);
    reply(ctx, rec, record, 0);
    const u32 a = rec + rc::kOpenRecord, b = rec + 0x410;
    std::memset(gb(ctx) + a, 0, 0x30);
    w64(ctx, a + 8, 0);
    w32(ctx, a, r32(ctx, a) | 2);
    w32(ctx, a + 4, 1);
    record_begin(ctx, b, 0x20, handle(a, "_pspocket_cellSailRec_Opened"));
    w32(ctx, rec + rc::kStreamState, 1);
    s32 type = static_cast<s32>(r32(ctx, profile + pf::kType));
    w32(ctx, profile, 2);
    w32(ctx, rec + rc::kProfile, profile);
    s32 r = 0;
    const auto fail = [&](s32 err) {
        error_event(ctx, rec, err);
        w32(ctx, b + 4, 1);
        close_all(ctx, rec, rec + 0x420, handle(b, "_pspocket_cellSailRec_OpenCancelled"));
    };
    if (type == 1) {
        r = mp4_composer_attach(ctx, rec);
        if (r < 0) return fail(r);
    } else if (type == 3 && static_cast<s32>(r32(ctx, rec + rc::kComposerSet)) > 0) {
        r = composer_attach(ctx, r32(ctx, rec + rc::kComposer), rec, cs_of(rec));
        if (r < 0) return fail(r);
    } else {
        return fail(error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM));
    }
    type = static_cast<s32>(r32(ctx, profile + pf::kType));
    const bool mp4 = type == -1 || type == 1;
    if (type == -1) type = 1;
    else if (!mp4 && !(type == 3 && static_cast<s32>(r32(ctx, rec + rc::kComposerSet)) > 0))
        return fail(error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM));
    w32(ctx, profile + pf::kType, static_cast<u32>(type));
    if (r32(ctx, profile + 0x444) - 0x100000 > 0xFFEFFFFEu) w32(ctx, profile + 0x444, 0x100000);
    const u32 sp = profile + 0x450;
    if (mp4) {
        if (r32(ctx, profile + 0x43C)) std::memset(gb(ctx) + sp, 0xFF, 0x38);
        if (r32(ctx, sp + 4) == 0xFFFFFFFF) w32(ctx, sp + 4, 0);
        if (r32(ctx, sp + 0xC) == 0xFFFFFFFF) w32(ctx, sp + 0xC, 0);
        if (r32(ctx, sp + 0x10) > 8) w32(ctx, sp + 0x10, 0);
        u32 scale = r32(ctx, sp + 8);
        if (static_cast<s32>(scale) < 0) {
            scale = 90000;
            w32(ctx, sp + 8, scale);
        }
        u32 delay = r32(ctx, sp + 0x34);
        if (static_cast<s32>(delay) < 0) delay = (scale + 1) >> 1;
        w32(ctx, profile + 0x440, ((scale + 7) >> 3) + delay);
        w32(ctx, sp + 0x34, delay);
    } else {
        if (r32(ctx, profile + 0x43C)) std::memset(gb(ctx) + sp, 0xFF, 0xC);
        u32 scale = r32(ctx, sp + 4);
        if (static_cast<s32>(scale) < 0) {
            scale = 90000;
            w32(ctx, sp + 4, scale);
        }
        u32 delay = r32(ctx, sp + 8);
        if (static_cast<s32>(delay) < 0) delay = (scale + 1) >> 1;
        w32(ctx, profile + 0x440, ((scale + 7) >> 3) + delay);
        w32(ctx, sp + 8, delay);
    }
    composer_open(ctx, cs_of(rec), static_cast<u32>(type), profile + 8, profile + pf::kType,
        handle(b, "_pspocket_cellSailRec_ComposerOpened"));
    w32(ctx, b + 4, 1);
}

// CloseStream (message 7, {argument}): open (4), no command pending; the command 0x22 pending with the argument,
// closing (8); answered at once, then everything closed.
void close_stream(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    lock(ctx, rec);
    if (r32(ctx, rec + rc::kState) != rc::kOpen || r32(ctx, rec + rc::kCommand))
        return reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_STATE));
    w32(ctx, rec + rc::kCommand, 0x22);
    w64(ctx, rec + rc::kCommandArg, r64(ctx, block));
    set_state(ctx, rec, rc::kClosing);
    reply(ctx, rec, record, 0);
    const u32 a = rec + rc::kOpenRecord;
    std::memset(gb(ctx) + a, 0, 0x20);
    w32(ctx, a + 4, 1);
    w64(ctx, a + 8, 0);
    w32(ctx, a, r32(ctx, a) | 2);
    close_all(ctx, rec, rec + 0x410, handle(a, "_pspocket_cellSailRec_Closed"));
}

// Start (message 9, {start parameter, file name, argument}): open (4), no command pending; the start parameter {0x18,
// 0, start time, duration} (its second word set to 0x100 in the game's copy) or none, the end time (all ones without a
// duration); the file name after the profile's path (a leading '/' dropped when there is a path); the command 0x29
// pending with the argument, starting (5); answered at once. Then a new session, the clock reset, the composer started
// and each open slot's pipeline started (their continuation starts the feeders).
void start(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    lock(ctx, rec);
    if (r32(ctx, rec + rc::kState) != rc::kOpen || r32(ctx, rec + rc::kCommand))
        return reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_STATE));
    const u32 param = r32(ctx, block);
    if (param) {
        if (r32(ctx, param) != 0x18 || r32(ctx, param + 4)) return reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_ARG));
        w32(ctx, param + 4, 0x100);
        std::memmove(gb(ctx) + rec + 0x800, gb(ctx) + param, 0x18);
        const u64 duration = r64(ctx, param + 0x10);
        w64(ctx, rec + rc::kEndTime, duration ? duration + r64(ctx, param + 8) : ~u64{0});
    } else {
        std::memset(gb(ctx) + rec + 0x800, 0, 0x18);
        w32(ctx, rec + 0x804, 0x100);
        w64(ctx, rec + rc::kStartTime, 0);
        w64(ctx, rec + rc::kEndTime, ~u64{0});
    }
    const u32 profile = r32(ctx, rec + rc::kProfile);
    const u32 len = r32(ctx, profile + 0x42C), name = r32(ctx, block + 4);
    const u32 skip = len && r8(ctx, name) == '/' ? 1 : 0;
    guest_strncpy(ctx, profile + 8 + len, name + skip, 0x421 - len);
    w8(ctx, profile + 0x428, 0);
    w32(ctx, rec + rc::kCommand, 0x29);
    w64(ctx, rec + rc::kCommandArg, r64(ctx, block + 8));
    set_state(ctx, rec, rc::kStarting);
    reply(ctx, rec, record, 0);
    const u32 a = rec + rc::kOpenRecord, b = rec + 0x410;
    std::memset(gb(ctx) + a, 0, 0x30);
    w32(ctx, a, r32(ctx, a) | 2);
    w32(ctx, a + 4, 1);
    w64(ctx, a + 8, 0);
    if (r32(ctx, rec + rc::kState) == rc::kStarted) return; // libsail_rec's check, never true here
    record_begin(ctx, b, 0x20, handle(a, "_pspocket_cellSailRec_StartDone"));
    w32(ctx, rec + rc::kStreamState, 3);
    u32 session = r32(ctx, rec + rc::kStreamArg) + 1;
    if (!session) session = 1;
    const u64 h = handle(b, "_pspocket_cellSailRec_Started");
    w32(ctx, rec + rc::kStreamArg, session);
    if (const u32 clock = r32(ctx, rec + rc::kClockInUse)) clock_reset(ctx, clock);
    composer_start(ctx, cs_of(rec), profile + 8, session, h);
    u32 steps = 1;
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (r8(ctx, slot + as::kState) != 1) continue;
        steps++;
        w8(ctx, slot + as::kState, 4);
        audio_pipeline_start(ctx, slot + as::kPipeline, h);
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (r8(ctx, slot + vs::kState) != 1) continue;
        steps++;
        w8(ctx, slot + vs::kState, 4);
        video_pipeline_start(ctx, slot + vs::kPipeline, slot,
            r32(ctx, profile + pf::kVideo) + r8(ctx, slot + vs::kIndex) * 0x220, h);
    }
    w32(ctx, b + 4, steps);
}

namespace {

// Stop's checks (recording, no command pending; the stop parameter {0x10, 0, the end time} or none: the end time 0),
// the command 0x2A pending with the argument, stopping (7), answered; false when refused (answered with the error).
bool stop_begin(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    lock(ctx, rec);
    if (r32(ctx, rec + rc::kState) != rc::kStarted || r32(ctx, rec + rc::kCommand)) {
        reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_STATE));
        return false;
    }
    const u32 param = r32(ctx, block);
    if (!param) {
        std::memset(gb(ctx) + rec + 0x818, 0, 0x10);
        w64(ctx, rec + rc::kEndTime, 0);
    } else if (r32(ctx, param) != 0x10 || r32(ctx, param + 4)) {
        reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_ARG));
        return false;
    } else {
        std::memmove(gb(ctx) + rec + 0x818, gb(ctx) + param, 0x10);
    }
    w32(ctx, rec + rc::kCommand, 0x2A);
    w64(ctx, rec + rc::kCommandArg, r64(ctx, block + 8));
    set_state(ctx, rec, rc::kStopping);
    reply(ctx, rec, record, 0);
    return true;
}

} // namespace

// Stop (message 0xB, {stop parameter, argument}): each starting or recording slot's feeder told to stop at the end time
// (its slot's stop record at +0x30), stopping by Stop (6); the last slot's composer stream ending completes it.
void stop(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    if (!stop_begin(ctx, rec, block, record)) return;
    const u64 end = r64(ctx, rec + rc::kEndTime);
    w32(ctx, rec + rc::kStreamState, 6);
    for (u32 i = 0, n = audio_count(ctx, rec); i < n; i++) {
        const u32 slot = audio_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + as::kState) - 4) >= 2) continue;
        const u32 r = slot + 0x30;
        w8(ctx, slot + as::kState, 6);
        std::memset(gb(ctx) + r, 0, 0x18);
        w32(ctx, r, r32(ctx, r) | 2);
        w32(ctx, r + 0x10, slot);
        w32(ctx, r + 4, 1);
        w64(ctx, r + 8, 0);
        feeder_stop(ctx, r32(ctx, slot + as::kFeeder), 0, end, handle(r, "_pspocket_cellSailRec_AudioFeederStopped"));
    }
    for (u32 i = 0, n = video_count(ctx, rec); i < n; i++) {
        const u32 slot = video_slot(ctx, rec, i);
        if (static_cast<u8>(r8(ctx, slot + vs::kState) - 4) >= 2) continue;
        const u32 r = slot + 0x30;
        w8(ctx, slot + vs::kState, 6);
        std::memset(gb(ctx) + r, 0, 0x18);
        w32(ctx, r + 0x10, slot);
        w32(ctx, r, r32(ctx, r) | 2);
        w32(ctx, r + 4, 1);
        w64(ctx, r + 8, 0);
        feeder_stop(ctx, r32(ctx, slot + vs::kFeeder), 0, end, handle(r, "_pspocket_cellSailRec_VideoFeederStopped"));
    }
}

// The stop with the composer's flush (message 0xC, libmp4's): Stop's checks, then everything stopped with the flag 1.
void stop_flush(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    if (!stop_begin(ctx, rec, block, record)) return;
    const u32 a = rec + rc::kOpenRecord;
    std::memset(gb(ctx) + a, 0, 0x20);
    w32(ctx, a + 4, 1);
    w64(ctx, a + 8, 0);
    w32(ctx, a, r32(ctx, a) | 2);
    stop_all(ctx, rec, rec + 0x410, 1, handle(a, "_pspocket_cellSailRec_FlushStopped"));
}

// A composer's session error (message 0x2E, the error with its terminated flag cleared, bit 31 set again): an error
// event; while recording (the streams 4) everything is stopped (the record at +0x470).
void composer_session_error(PPUContext& ctx, u32 rec, u32 err)
{
    error_event(ctx, rec, static_cast<s32>(err | 0x80000000u));
    if (static_cast<s32>(err) < 0 || r32(ctx, rec + rc::kStreamState) != 4) return;
    const u32 r = rec + 0x470;
    std::memset(gb(ctx) + r, 0, 0x20);
    w32(ctx, r + 4, 1);
    w64(ctx, r + 8, 0);
    w32(ctx, r, r32(ctx, r) | 2);
    stop_all(ctx, rec, rec + 0x480, 0, handle(r, "_pspocket_cellSailRec_ErrorStopped"));
}

#define SAILREC_INTERNAL(fn) PS_HLE_INTERNAL(recomp::nid_of(#fn), fn)

void register_stream(Registry& r)
{
    r.add(kChan, {
        SAILREC_INTERNAL(_pspocket_cellSailRec_OpenCancelled),
        SAILREC_INTERNAL(_pspocket_cellSailRec_StartCancelled),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioOpenCancelled),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoOpenCancelled),
        SAILREC_INTERNAL(_pspocket_cellSailRec_EndLoop),
        SAILREC_INTERNAL(_pspocket_cellSailRec_EndClose),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AllStopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioFeederStopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoFeederStopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioPipelineStopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoPipelineStopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FeedersStarted),
        SAILREC_INTERNAL(_pspocket_cellSailRec_Started),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ErrorStopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_SlotClosedAudio),
        SAILREC_INTERNAL(_pspocket_cellSailRec_SlotClosedVideo),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioSlotOpened),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AudioFeederOpened),
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoSlotOpened),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AllClosed),
        SAILREC_INTERNAL(_pspocket_cellSailRec_AllOpened),
        SAILREC_INTERNAL(_pspocket_cellSailRec_ComposerOpened),
        SAILREC_INTERNAL(_pspocket_cellSailRec_Opened),
        SAILREC_INTERNAL(_pspocket_cellSailRec_Closed),
        SAILREC_INTERNAL(_pspocket_cellSailRec_StartDone),
        SAILREC_INTERNAL(_pspocket_cellSailRec_Stopped),
        SAILREC_INTERNAL(_pspocket_cellSailRec_FlushStopped),
    });
}

#undef SAILREC_INTERNAL

} // namespace pspocket::hle::sailrec
