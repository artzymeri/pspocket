// cellSail's AV clock (D-562 part 4, D-919 part 3), as libsail (fw 4.92) has it: 200 bytes at the player's +8, its
// lwmutex at +0xB0 ("_saiav0"), shared by the player and the sound and graphics adapters. It keeps an audio and a video
// half (their measured time per frame from UpdateAvSync, cellSailAdapters.cpp), the expected time of each half's next
// frame and its clock, and which stream is each half's master. A session (the session word of the source's start, its
// low 10 bits) is synced once the frames it needs have been seen (the sync computing the clocks from the earliest PTS
// of each half); each GetFrame then asks whether its frame is due (audio) or what times it covers (video).
//
// The clock: +0 the ticks per millisecond (90 x the rate), +8/+0xC/+0x10/+0x18/+0x20 the audio half's state, frame,
// time, time per frame and its ticks (+0x28.. the video half's), +0x48/+0x4C the stream counting each half, +0x50/+0x58
// the audio half's expected time and clock (+0x60/+0x68 the video's), +0x70/+0x78 the earliest PTS of each half in the
// session being synced, +0x80 the audio half counts samples (x256) not frames, +0x84 the audio streams, +0x88 the frames
// to see before syncing, +0x8A those seen, +0x8C a round counter, +0x8E the video streams, +0x90 running, +0x94/+0x98
// each half's master (-1 none), +0x9C/+0xA0 the streams of the earliest PTS, +0xA4 the session synced, +0xA8 the
// session being synced.

#include "cellSail.hpp"

namespace pspocket::hle::sail {

namespace {

namespace ck {
constexpr u32 kTicks = 0, kAState = 8, kAPer = 0x18, kAPerTicks = 0x20, kVState = 0x28, kVPer = 0x38,
              kVPerTicks = 0x40, kAIndex = 0x48, kVIndex = 0x4C, kAExpect = 0x50, kAClock = 0x58, kVExpect = 0x60,
              kVClock = 0x68, kAMin = 0x70, kVMin = 0x78, kPerSample = 0x80, kAudioN = 0x84, kNeeded = 0x88,
              kCounted = 0x8A, kRound = 0x8C, kVideoN = 0x8E, kRunning = 0x90, kAMaster = 0x94, kVMaster = 0x98,
              kAMinIndex = 0x9C, kVMinIndex = 0xA0, kSession = 0xA4, kNewSession = 0xA8, kMutex = 0xB0;
}

s16 r16s(PPUContext& ctx, u32 a) { return static_cast<s16>(ps_read16(gb(ctx), a)); }
s32 r32s(PPUContext& ctx, u32 a) { return static_cast<s32>(r32(ctx, a)); }
s64 r64s(PPUContext& ctx, u32 a) { return static_cast<s64>(r64(ctx, a)); }

// x ticks... in libsail's signed division by 1000 (rounding toward 0).
s64 per_ms(s64 x) { return x / 1000; }

// The video half's clock from the audio half's (their expected times' difference in milliseconds by the ticks).
u64 video_from_audio(PPUContext& ctx, u32 c)
{
    const s64 d = static_cast<s64>(r64(ctx, c + ck::kVExpect) - r64(ctx, c + ck::kAExpect));
    return r64(ctx, c + ck::kAClock) + static_cast<u64>(per_ms(static_cast<s64>(static_cast<u64>(d) * r64(ctx, c + ck::kTicks))));
}

u64 audio_from_video(PPUContext& ctx, u32 c)
{
    const s64 d = static_cast<s64>(r64(ctx, c + ck::kAExpect) - r64(ctx, c + ck::kVExpect));
    return r64(ctx, c + ck::kVClock) + static_cast<u64>(per_ms(static_cast<s64>(static_cast<u64>(d) * r64(ctx, c + ck::kTicks))));
}

// One frame's (or `samples` x256's) worth of the audio half's ticks.
u64 audio_step(PPUContext& ctx, u32 c, u32 samples, u32 at)
{
    const u64 per = r64(ctx, c + at);
    return r32(ctx, c + ck::kPerSample) == 0 ? per : (u64{samples} * per) >> 8;
}

void lock(PPUContext& ctx, u32 c) { lv2_lib(ctx, "sys_lwmutex_lock", {c + ck::kMutex, 0}); }
void unlock(PPUContext& ctx, u32 c) { lv2_lib(ctx, "sys_lwmutex_unlock", {c + ck::kMutex}); }

} // namespace

// Running or not (libsail's, the lock held): running needs video streams and forgets the audio master and earliest PTS.
u32 clock_run_locked(PPUContext& ctx, u32 c, u32 run)
{
    const u32 now = r32(ctx, c + ck::kRunning);
    if ((now != 0 ? 1u : 0u) == run) return now;
    u32 v = 0;
    if (run) {
        if (r16s(ctx, c + ck::kVideoN) == 0) return 0;
        v = 1;
        w32(ctx, c + ck::kAMaster, 0xFFFFFFFF);
        w64(ctx, c + ck::kAMin, ~u64{0});
    }
    w32(ctx, c + ck::kRunning, v);
    return v;
}

// The sync (libsail's): once both halves (or the one that has streams) were measured, each half's clock from the
// earliest PTS seen, the later half stepped on by whole frames past the other's expected time; then the session being
// synced becomes the one synced, its rate the ticks, the earliest PTS' streams the masters. 1 when the audio half led,
// 2 the video half, 0 not synced yet.
u32 clock_sync(PPUContext& ctx, u32 c)
{
    const u64 amin = r64(ctx, c + ck::kAMin), vmin = r64(ctx, c + ck::kVMin);
    u32 result;
    if (amin == ~u64{0}) {
        if (vmin == ~u64{0}) {
            result = 1;
        } else {
            if (r32(ctx, c + ck::kVState) != 2) return 0;
            w64(ctx, c + ck::kVClock, vmin - r64(ctx, c + ck::kVPerTicks));
            result = 2;
        }
    } else if (vmin == ~u64{0}) {
        if (r32(ctx, c + ck::kAState) != 2) return 0;
        w64(ctx, c + ck::kAClock, amin - r64(ctx, c + ck::kAPerTicks));
        result = 1;
    } else {
        if (r32(ctx, c + ck::kAState) != 2 || r32(ctx, c + ck::kVState) != 2) return 0;
        const s64 d = static_cast<s64>(vmin - amin);
        const u64 vper = r64(ctx, c + ck::kVPer), ticks = r64(ctx, c + ck::kTicks), vexp = r64(ctx, c + ck::kVExpect);
        const u64 aexp = r64(ctx, c + ck::kAExpect), limit = r64(ctx, c + ck::kAPer) + aexp;
        u64 vnext = vper + vexp;
        const u64 ms = (static_cast<u64>(d) * 1000) / ticks;
        u64 at;
        if (d > 0) {
            at = vnext - ms % vper;
            if (at < limit) {
                at += vper;
                u64 t;
                do {
                    at += vper;
                    t = at - vper;
                } while (t < limit);
                at = t;
            }
            vnext = at + ms;
            result = 1;
        } else {
            at = vnext - ms;
            result = 2;
            if (limit > at) {
                at += vper;
                u64 t;
                do {
                    at += vper;
                    vnext += vper;
                    t = at - vper;
                } while (t < limit);
                at = t;
            }
        }
        const s64 a_ms = per_ms(static_cast<s64>(ticks * (at - aexp)));
        const s64 v_ms = per_ms(static_cast<s64>((vnext - vexp) * ticks));
        w64(ctx, c + ck::kAClock, amin - static_cast<u64>(a_ms));
        w64(ctx, c + ck::kVClock, vmin - static_cast<u64>(v_ms));
    }
    // The session being synced becomes the one synced.
    const u32 word = r32(ctx, c + ck::kNewSession);
    const s32 exp = static_cast<s32>(word >> 28) - 6;
    const u32 mant = ((word >> 22) & 0x3F) | 0x40;
    w64(ctx, c + ck::kAMin, ~u64{0});
    w16(ctx, c + ck::kCounted, 0);
    w64(ctx, c + ck::kVMin, ~u64{0});
    w32(ctx, c + ck::kSession, word);
    const u32 rate = exp >= 0 ? (exp >= 32 ? 0 : mant << exp) : (-exp >= 32 ? 0 : mant >> -exp);
    const s64 t90 = static_cast<s64>(u64{rate} * 90) / 256;
    w32(ctx, c + ck::kSession, word);
    w64(ctx, c + ck::kTicks, t90 != 0 ? static_cast<u64>(t90) : 1);
    w32(ctx, c + ck::kAMaster, r32(ctx, c + ck::kAMinIndex));
    w32(ctx, c + ck::kVMaster, r32(ctx, c + ck::kVMinIndex));
    w32(ctx, c + ck::kVMinIndex, 0xFFFFFFFF);
    w32(ctx, c + ck::kAMinIndex, 0xFFFFFFFF);
    return result;
}

// A new session word (libsail's): its streams (bits 12-15 audio, 10-11 video), the frames to see before syncing (the
// video streams' when EP skipping, else all), running unless at the normal rate (bit 16), nothing seen yet.
void clock_session(PPUContext& ctx, u32 c, u32 word)
{
    const u16 video = (word >> 10) & 3, audio = (word >> 12) & 0xF;
    w16(ctx, c + ck::kVideoN, video);
    w16(ctx, c + ck::kAudioN, audio);
    w16(ctx, c + ck::kNeeded, (word & 0x40000) ? video : static_cast<u16>(audio + video));
    clock_run_locked(ctx, c, (word >> 16) & 1);
    w32(ctx, c + ck::kNewSession, word);
    w16(ctx, c + ck::kCounted, 0);
}

// A stream's end counted (libsail's, the lock held): a new session word taken; a stream not yet seen in this session
// counts (syncing when all were); the round counter moves on (an audio stream's only outside EP skipping).
void clock_count(PPUContext& ctx, u32 c, u32 seen_at, bool audio, u32 word)
{
    const u32 seen = r32(ctx, seen_at);
    if (r32(ctx, c + ck::kSession) != word) clock_session(ctx, c, word);
    s16 needed = r16s(ctx, c + ck::kNeeded);
    if (needed > 0 && seen == 0) {
        const s16 counted = static_cast<s16>(r16s(ctx, c + ck::kCounted) + 1);
        w16(ctx, c + ck::kCounted, static_cast<u16>(counted));
        if (needed <= counted) {
            clock_sync(ctx, c);
            needed = r16s(ctx, c + ck::kNeeded);
        }
    }
    w32(ctx, seen_at, 0);
    s32 round = r16s(ctx, c + ck::kRound);
    if (!(r32(ctx, c + ck::kSession) & 0x40000) || !audio) round++;
    w16(ctx, c + ck::kRound, static_cast<u16>(needed > round ? round : 0));
}

// The audio half's frame (libsail's): `samples` counted at `tag`; in the session synced, the frame is due when it is
// at most half a frame ahead of the master's clock (the master's clock moving on by a frame); a frame of a new
// session counts toward its sync (its PTS the earliest kept). The answer: 1 a new session, 2 not yet, 8 no PTS (due
// at once); `out` the clock (-1 none).
u32 clock_audio_frame(PPUContext& ctx, u32 c, u64 pts, u32 session, u32 current, u32 tag, u32 samples, u32 out,
    u32 drop, u32 phase, u32 index)
{
    lock(ctx, c);
    u32 result = pts == ~u64{0} ? 8 : 0;
    if (r32(ctx, c + ck::kPerSample) == 0) w32(ctx, tag, r32(ctx, tag) + 1);
    else w32(ctx, tag, r32(ctx, tag) + samples);
    const u32 key = session << 22;
    const u32 synced = r32(ctx, c + ck::kSession) << 22;
    u64 clock = ~u64{0};
    // The states of libsail's code below: 0 synced (L1b560), 1 not synced (L1b45c), 2 counting a new session's frame.
    int path;
    if (synced != 0 && key == synced) {
        if (r32(ctx, phase) == 1) w32(ctx, phase, 2);
        path = 0;
    } else if ((current << 22) == key) {
        path = 1;
        if (r16s(ctx, c + ck::kCounted) >= r16s(ctx, c + ck::kNeeded)) {
            if (clock_sync(ctx, c) != 0) {
                w32(ctx, phase, 2);
                path = 0;
            }
        }
    } else {
        bool count = true;
        if ((r32(ctx, c + ck::kNewSession) << 22) != key) {
            const s16 counted = r16s(ctx, c + ck::kCounted);
            if (counted != 0) {
                count = false;
                path = 1;
                if (r16s(ctx, c + ck::kNeeded) <= counted && clock_sync(ctx, c) != 0) {
                    w32(ctx, phase, 2);
                    path = 0;
                }
            } else {
                clock_session(ctx, c, session);
            }
        }
        if (count) {
            const s16 counted = static_cast<s16>(r16s(ctx, c + ck::kCounted) + 1);
            w16(ctx, c + ck::kCounted, static_cast<u16>(counted));
            w32(ctx, phase, 1);
            if (r32(ctx, c + ck::kRunning) == 0 && r64(ctx, c + ck::kAMin) > pts) {
                w64(ctx, c + ck::kAMin, pts);
                w32(ctx, c + ck::kAMinIndex, index);
            }
            result |= 1;
            path = 1;
            if (counted >= r16s(ctx, c + ck::kNeeded)) {
                if (clock_sync(ctx, c) != 0) {
                    w32(ctx, phase, 2);
                    path = 0;
                }
            }
        }
    }
    if (path == 0) {
        if (r32(ctx, c + ck::kAIndex) == index)
            w64(ctx, c + ck::kAExpect, r64(ctx, c + ck::kAExpect) + audio_step(ctx, c, samples, ck::kAPer));
        const s32 master = r32s(ctx, c + ck::kAMaster);
        if (master != -1) {
            u64 now = r64(ctx, c + ck::kAClock);
            if (static_cast<s32>(index) == master) {
                const u64 step_ticks = audio_step(ctx, c, samples, ck::kAPerTicks);
                now += step_ticks;
                if (now + (step_ticks >> 1) >= pts) now = pts;
            }
            clock = now;
            w64(ctx, c + ck::kAClock, now);
        } else if (r32s(ctx, c + ck::kVMaster) != -1) {
            const s64 v = static_cast<s64>(audio_from_video(ctx, c));
            if (v < 0) {
                clock = pts;
            } else {
                const s64 diff = v - static_cast<s64>(pts);
                const s64 dist = diff < 0 ? static_cast<s64>(pts) - v : diff;
                if (r64s(ctx, c + ck::kVPerTicks) / 2 <= dist || r32(ctx, c + ck::kRunning) != 0) {
                    clock = static_cast<u64>(v);
                } else {
                    clock = pts;
                    w32(ctx, c + ck::kAMaster, index);
                    w64(ctx, c + ck::kAClock, pts);
                }
            }
        } else if (pts == ~u64{0} || r32(ctx, c + ck::kRunning) != 0) {
            clock = pts;
        } else {
            clock = pts;
            w32(ctx, c + ck::kAMaster, index);
            w64(ctx, c + ck::kAClock, pts);
        }
    } else {
        result |= 2;
        if (r32(ctx, c + ck::kAIndex) == index)
            w64(ctx, c + ck::kAExpect, r64(ctx, c + ck::kAExpect) + audio_step(ctx, c, samples, ck::kAPer));
        s32 master = r32s(ctx, c + ck::kAMaster);
        if (r32(ctx, c + ck::kRunning) == 0 && master == -1 && r32s(ctx, c + ck::kVMaster) == -1) {
            w32(ctx, c + ck::kAMaster, index);
            master = static_cast<s32>(index);
        }
        if (master == static_cast<s32>(index)) {
            const u64 step_ticks = audio_step(ctx, c, samples, ck::kAPerTicks);
            w64(ctx, c + ck::kAClock, r64(ctx, c + ck::kAClock) + (drop == 0 ? step_ticks : 0));
        } else if (r32s(ctx, c + ck::kVMaster) != -1) {
            w64(ctx, c + ck::kAClock, audio_from_video(ctx, c));
        }
    }
    unlock(ctx, c);
    w64(ctx, out, clock);
    return result;
}

// The video half's frame (libsail's): counted at `tag`; in the session synced, the times the frame covers (from the
// master's clock: its start at `from`, its end at `to`; as master, moved on by a frame, or to the PTS when within 15
// ticks); a frame of a new session counts toward its sync. The answer: 0 synced, 1 a new session synced by it, 2 not
// yet, 3 a new session's frame counted.
u32 clock_video_frame(PPUContext& ctx, u32 c, u64 pts, u32 session, u32 current, u32 tag, u32 from, u32 to, u32 hold,
    u32 phase, u32 index)
{
    lock(ctx, c);
    w32(ctx, tag, r32(ctx, tag) + 1);
    const u32 key = session << 22;
    const u32 synced = r32(ctx, c + ck::kSession) << 22;
    u32 result = 2;
    u64 start = ~u64{0}, end = ~u64{0};
    bool in_sync = false;
    if (synced != 0 && key == synced) {
        result = 0;
        if (r32(ctx, phase) == 1) w32(ctx, phase, 2);
        in_sync = true;
    } else if ((current << 22) == key) {
        if (r16s(ctx, c + ck::kCounted) >= r16s(ctx, c + ck::kNeeded) && clock_sync(ctx, c) != 0) {
            w32(ctx, phase, 2);
            result = 0;
            in_sync = true;
        }
    } else {
        bool count = true;
        if ((r32(ctx, c + ck::kNewSession) << 22) != key) {
            const s16 counted = r16s(ctx, c + ck::kCounted);
            if (counted != 0) {
                count = false;
                if (r16s(ctx, c + ck::kNeeded) <= counted && clock_sync(ctx, c) != 0) {
                    w32(ctx, phase, 2);
                    result = 0;
                    in_sync = true;
                }
            } else {
                clock_session(ctx, c, session);
            }
        }
        if (count) {
            const s16 counted = static_cast<s16>(r16s(ctx, c + ck::kCounted) + 1);
            w16(ctx, c + ck::kCounted, static_cast<u16>(counted));
            w32(ctx, phase, 1);
            if (r64(ctx, c + ck::kVMin) > pts) {
                w64(ctx, c + ck::kVMin, pts);
                w32(ctx, c + ck::kVMinIndex, index);
            }
            result = 3;
            if (counted >= r16s(ctx, c + ck::kNeeded) && clock_sync(ctx, c) != 0) {
                result = 1;
                w32(ctx, phase, 2);
                in_sync = true;
            }
        }
    }
    if (r32(ctx, c + ck::kVIndex) == index)
        w64(ctx, c + ck::kVExpect, r64(ctx, c + ck::kVExpect) + r64(ctx, c + ck::kVPer));
    const u64 per = r64(ctx, c + ck::kVPerTicks);
    if (in_sync) {
        const s32 amaster = r32s(ctx, c + ck::kAMaster), vmaster = r32s(ctx, c + ck::kVMaster);
        if (amaster != -1) {
            if (static_cast<s32>(index) == vmaster) {
                start = video_from_audio(ctx, c);
                w64(ctx, c + ck::kVClock, start);
                end = start + per;
            } else {
                start = r64(ctx, c + ck::kVClock);
                if (vmaster == -1) w32(ctx, c + ck::kVMaster, index);
                end = start + per;
            }
        } else {
            start = r64(ctx, c + ck::kVClock);
            if (static_cast<s32>(index) == vmaster) {
                if (hold != 0) {
                    end = start + per;
                } else {
                    start += per;
                    const s64 diff = static_cast<s64>(pts - start);
                    const s64 dist = diff < 0 ? static_cast<s64>(start - pts) : diff;
                    if (dist <= 15) start = pts;
                    w64(ctx, c + ck::kVClock, start);
                    end = start + per;
                }
            } else if (vmaster != -1) {
                end = start + per;
            } else {
                w32(ctx, c + ck::kVMaster, index);
                start += per;
                w64(ctx, c + ck::kVClock, start);
                end = start + per;
            }
        }
    } else {
        s32 vmaster = r32s(ctx, c + ck::kVMaster);
        if (vmaster == -1) {
            w32(ctx, c + ck::kVMaster, index);
            vmaster = static_cast<s32>(index);
        }
        if (r32s(ctx, c + ck::kAMaster) != -1) {
            if (vmaster == static_cast<s32>(index)) w64(ctx, c + ck::kVClock, video_from_audio(ctx, c));
        } else if (vmaster == static_cast<s32>(index)) {
            w64(ctx, c + ck::kVClock, r64(ctx, c + ck::kVClock) + (hold == 0 ? per : 0));
        }
    }
    unlock(ctx, c);
    w64(ctx, from, start);
    w64(ctx, to, end);
    return result;
}

// The video half's frame outside the sync (libsail's): counted at `tag` (its new count answered), the times it covers
// from the clocks (as master moved on by a frame unless `hold`).
u32 clock_video_tick(PPUContext& ctx, u32 c, u32 tag, u32 index, u32 from, u32 to, u32 hold)
{
    lock(ctx, c);
    const u32 n = r32(ctx, tag) + 1;
    w32(ctx, tag, n);
    if (r32(ctx, c + ck::kVIndex) == index)
        w64(ctx, c + ck::kVExpect, r64(ctx, c + ck::kVExpect) + r64(ctx, c + ck::kVPer));
    const u64 per = r64(ctx, c + ck::kVPerTicks);
    const s32 vmaster = r32s(ctx, c + ck::kVMaster);
    u64 start, end;
    if (r32s(ctx, c + ck::kAMaster) == -1) {
        start = r64(ctx, c + ck::kVClock);
        if (static_cast<s32>(index) == vmaster) {
            start += hold == 0 ? per : 0;
            w64(ctx, c + ck::kVClock, start);
            end = start + per;
        } else if (vmaster != -1) {
            end = start + per;
        } else {
            start = ~u64{0};
            end = ~u64{0};
        }
    } else if (static_cast<s32>(index) == vmaster) {
        start = video_from_audio(ctx, c);
        w64(ctx, c + ck::kVClock, start);
        end = start + per;
    } else if (vmaster == -1) {
        start = r64(ctx, c + ck::kAClock);
        end = start + per;
    } else {
        start = r64(ctx, c + ck::kVClock);
        end = start + per;
    }
    w64(ctx, from, start);
    w64(ctx, to, end);
    unlock(ctx, c);
    return n;
}

// The audio half's frame outside the sync (libsail's): `samples` counted at `tag` (its new count answered), the clock
// (as master moved on by a frame unless `hold`; from the video half when only it has a master).
u32 clock_audio_tick(PPUContext& ctx, u32 c, u32 tag, u32 samples, u32 index, u32 out, u32 hold)
{
    lock(ctx, c);
    u32 n;
    if (r32(ctx, c + ck::kPerSample) == 0) {
        n = r32(ctx, tag) + 1;
        w32(ctx, tag, n);
        if (r32(ctx, c + ck::kAIndex) == index)
            w64(ctx, c + ck::kAExpect, r64(ctx, c + ck::kAExpect) + r64(ctx, c + ck::kAPer));
    } else {
        n = samples + r32(ctx, tag);
        w32(ctx, tag, n);
        if (r32(ctx, c + ck::kAIndex) == index)
            w64(ctx, c + ck::kAExpect, r64(ctx, c + ck::kAExpect) + ((u64{samples} * r64(ctx, c + ck::kAPer)) >> 8));
    }
    const s32 master = r32s(ctx, c + ck::kAMaster);
    u64 now;
    if (master == -1) {
        now = r32s(ctx, c + ck::kVMaster) == -1 ? ~u64{0} : audio_from_video(ctx, c);
    } else if (master == static_cast<s32>(index)) {
        now = r64(ctx, c + ck::kAClock) + (hold == 0 ? audio_step(ctx, c, samples, ck::kAPerTicks) : 0);
        w64(ctx, c + ck::kAClock, now);
    } else {
        now = r64(ctx, c + ck::kAClock);
    }
    w64(ctx, out, now);
    unlock(ctx, c);
    return n;
}

// A stream's end (libsail's): no longer its half's master, counted (clock_count).
void clock_video_end(PPUContext& ctx, u32 c, u32 index, u32 seen_at, u32 word)
{
    lock(ctx, c);
    if (r32(ctx, c + ck::kVMaster) == index) w32(ctx, c + ck::kVMaster, 0xFFFFFFFF);
    clock_count(ctx, c, seen_at, false, word);
    unlock(ctx, c);
}

void clock_audio_end(PPUContext& ctx, u32 c, u32 index, u32 seen_at, u32 word)
{
    lock(ctx, c);
    if (r32(ctx, c + ck::kAMaster) == index) w32(ctx, c + ck::kAMaster, 0xFFFFFFFF);
    clock_count(ctx, c, seen_at, true, word);
    unlock(ctx, c);
}

} // namespace pspocket::hle::sail
