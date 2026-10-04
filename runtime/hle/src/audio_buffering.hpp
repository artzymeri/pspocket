#pragma once

// cellAudio's buffering mode (cellAudio.cpp, D-074), the parts that decide, kept apart so the tests can drive them:
// RPCS3's buffer tags, which tell whether the game has written the block a port plays next, and the loop's choice
// each turn (wait for the period's end, wait for the game, play, play silence or skip), whose period length keeps the
// host's buffer at its target.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAudio.cpp/.h (upstream 431b169): audio_port::tag,
// cell_audio_thread::count_port_buffer_tags, cell_audio_config::reset and the buffering branch of
// cell_audio_thread::operator(), with RPCS3's default settings (buffering on, a 34 ms buffer, no time stretching).
// The output is 48 kHz stereo, so the block period is 5333 us.
//
// Which untouched ports to wait for differs from RPCS3 (D-074). RPCS3 counts the untouched ports it has given up on
// (untouched_expected, held for 340 ms after each match) and waits only while more are untouched. GTA V's game thread
// answers late often here, its PPU and SPUs being far slower than under RPCS3's recompilers, and after the game closed
// a second port it never wrote, that count stayed at one: the main port's late blocks were played silent without
// waiting, and the game's late cellAudioAddData landed on the next block (run 043 on the Mac: half the periods silent,
// the buzz D-072 left on the iPhone). Here each port says whether the game feeds it (a tag moved in the last 340 ms,
// like the ps3-ios-engine port's "quiet" ports), and the loop waits for every untouched port that is fed, for RPCS3's
// 2 periods (4 when other ports are ready).

#include <algorithm>
#include <array>
#include <cmath>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

namespace pspocket::hle::audio_buffering {

inline constexpr u32 kBlockSamples = 256;                                   // AUDIO_BUFFER_SAMPLES
inline constexpr u32 kSampleRate = 48000;
inline constexpr u64 kPeriodUs = u64{kBlockSamples} * 1'000'000 / kSampleRate; // cfg.audio_block_period
inline constexpr u64 kMinPeriodUs = kPeriodUs / 2;                          // cfg.minimum_block_period
inline constexpr u64 kMaxPeriodUs = 6 * kPeriodUs / 5;                      // cfg.maximum_block_period
inline constexpr u64 kDesiredBufferMs = 34;               // the default "Desired Audio Buffer Duration"
inline constexpr u64 kFullyUntouchedTimeoutUs = 2 * kPeriodUs;     // every port untouched: wait this long
inline constexpr u64 kPartiallyUntouchedTimeoutUs = 4 * kPeriodUs; // some ports untouched: wait this long
inline constexpr u64 kQuietPeriods = 340'000 / kPeriodUs; // a port with no tag moved for longer isn't fed
inline constexpr s64 kPeriodComparisonMarginUs = 5;        // period_comparison_margin (not Windows)
inline constexpr f32 kPeriodAverageAlpha = 0.02f;          // period_average_alpha
inline constexpr u64 kWaitForGameUs = 1000, kWaitForWritesUs = 500;

// The tags: six floats per block, spread diagonally with the last one on the block's last float (on an 8-channel
// block they land on channels 2 to 7). The marks: the front right channel of samples 0, 51, ..., 255, for ports
// wider than stereo, whose tags a game that fills only the front channels never reaches (PORT_BUFFER_MARK_CHANNEL).
inline constexpr u32 kTagCount = 6; // PORT_BUFFER_TAG_COUNT
inline constexpr u32 kTagLast2 = 2 * kBlockSamples - 1, kTagDelta2 = kTagLast2 / (kTagCount - 1),
                     kTagFirst2 = kTagLast2 % (kTagCount - 1);
inline constexpr u32 kTagLast8 = 8 * kBlockSamples - 1, kTagDelta8 = kTagLast8 / (kTagCount - 1),
                     kTagFirst8 = kTagLast8 % (kTagCount - 1);
inline constexpr u32 kMarkChannel = 1, kMarkDeltaSample = (kBlockSamples - 1) / (kTagCount - 1);
inline constexpr u8 kFrontOnlySettlePeriods = 8; // PORT_FRONT_ONLY_SETTLE_PERIODS
inline constexpr u32 kNoTag = 0xFFFFFFFF;        // RPCS3's umax: no tag written since the block was tagged

// The float (index into the block) of tag `nr` in a block of `channels` channels (2 or 8).
constexpr u32 tag_position(u32 channels, u32 nr)
{
    return channels == 2 ? kTagFirst2 + nr * kTagDelta2 : kTagFirst8 + nr * kTagDelta8;
}

// RPCS3 audio_port::mark_position.
constexpr u32 mark_position(u32 channels, u32 nr) { return nr * kMarkDeltaSample * channels + kMarkChannel; }

// A port's tag state: RPCS3 audio_port's prev_touched_tag_nr and last_tag_value, and the thread's per-port
// m_periods_without_tag, m_front_only_port and m_front_only_reported; and whether the game feeds the port. A port
// starts with a fresh one.
struct PortTags {
    u32 prev_touched = 0;
    std::array<f32, kTagCount> last_value{};
    u8 periods_without_tag = 0;
    bool front_only = false;
    bool front_only_reported = false;
    bool fed = false; // a tag moved in the last kQuietPeriods periods: the loop waits for this port
};

// RPCS3 audio_port::tag on the block at guest address `block`, just played and cleared (reset_ports): -0.0f on the
// tags (and the marks of a wider port). A game that writes the block overwrites them; one that checks for silence still
// reads zeros (-0.0f == 0.0f).
inline void tag_block(u8* base, u32 block, u32 channels, PortTags& t)
{
    for (u32 nr = 0; nr < kTagCount; nr++) {
        ps_writef32(base, block + 4 * tag_position(channels, nr), -0.0f);
        t.last_value[nr] = -0.0f;
    }
    if (channels != 2) {
        for (u32 nr = 0; nr < kTagCount; nr++) ps_writef32(base, block + 4 * mark_position(channels, nr), -0.0f);
    }
    t.prev_touched = kNoTag;
    if (t.periods_without_tag < 0xFF) t.periods_without_tag++; // one more period about to pass without a tag moving
}

// What the game has done to the block a port plays next. "Since the last look": the state remembers what it saw.
enum class Block : u8 {
    Complete,   // the last tag is written and nothing moved since the last look
    InProgress, // a tag moved since the last look: the game is writing
    Untouched,  // no tag written
    Incomplete, // nothing moved since the last look, but the last tag isn't written (or the block was never tagged);
                // or a port that fills its front channels only
};

// RPCS3 count_port_buffer_tags for one started port with channels, its block at guest address `block`.
inline Block classify_block(u8* base, u32 block, u32 channels, PortTags& t)
{
    u32 last_touched = t.prev_touched;
    bool retouched = false, moved = false;
    for (u32 nr = 0; nr < kTagCount; nr++) {
        const f32 value = ps_readf32(base, block + 4 * tag_position(channels, nr));
        f32& last = t.last_value[nr];
        // -0.0f == 0.0f: a tag the game overwrote with +0.0f moved too.
        if (value != last || (last == -0.0f && std::signbit(last) && !std::signbit(value))) {
            last = value;
            retouched |= nr <= t.prev_touched && t.prev_touched != kNoTag;
            last_touched = nr;
            moved = true;
        }
    }
    if (moved) { // a port that reaches its surround channels: its tags track it, the marks don't matter
        t.periods_without_tag = 0;
        t.front_only = false;
        t.fed = true;
    } else if (t.periods_without_tag > kQuietPeriods) {
        t.fed = false; // gone quiet: a stopped stream
    }
    if (last_touched == kNoTag) {
        // Nothing written, or (a wider port) only the front channels, which no tag sits on: the marks tell, once the
        // port has gone long enough without a tag moving to rule out a game writing one channel at a time.
        if (!t.front_only && channels != 2 && t.periods_without_tag > kFrontOnlySettlePeriods) {
            for (u32 nr = 0; nr < kTagCount; nr++) {
                const f32 value = ps_readf32(base, block + 4 * mark_position(channels, nr));
                if (value != -0.0f || !std::signbit(value)) {
                    t.front_only = true;
                    break;
                }
            }
        }
        return t.front_only ? Block::Incomplete : Block::Untouched;
    }
    if (last_touched == kTagCount - 1) {
        t.prev_touched = last_touched;
        return retouched ? Block::InProgress : Block::Complete; // retouched: look once more
    }
    if (last_touched == t.prev_touched) return retouched ? Block::InProgress : Block::Incomplete;
    t.prev_touched = last_touched;
    return Block::InProgress;
}

// count_port_buffer_tags' totals over the started ports, and the untouched ports the game feeds. A port without
// channels counts as active only.
struct TagCount {
    u32 active = 0;
    u32 in_progress = 0;
    u32 untouched = 0;
    u32 incomplete = 0;
    u32 untouched_fed = 0;
};

inline void count(TagCount& c, Block b, bool fed)
{
    c.in_progress += b == Block::InProgress;
    c.untouched += b == Block::Untouched;
    c.incomplete += b == Block::Incomplete;
    c.untouched_fed += b == Block::Untouched && fed;
}

// cell_audio_config::reset: RPCS3's default 34 ms, or the device's callback length plus two blocks of jitter when
// that is more, in whole milliseconds. `device_frames`: the most frames the device takes at once (0: unknown).
inline u64 desired_buffer_us(u32 device_frames)
{
    const u64 min_ms = (u64{device_frames} + 2 * kBlockSamples) * 1000 / kSampleRate;
    return std::max(min_ms, kDesiredBufferMs) * 1000;
}

// cfg.desired_full_buffers: the silent blocks a (re)start queues.
inline u32 restart_blocks(u64 desired_us) { return static_cast<u32>(desired_us / kPeriodUs) + 3; }

// What the host's buffer is compared with (RPCS3 desired_duration_adjusted): the target plus half a period. RPCS3
// divides the average of what the host held (us) by cfg.audio_buffer_length, a stereo block's sample count (512): only
// a host buffer that has stayed all but empty raises the target, by up to 4 times.
inline f32 adjusted_target_us(f32 average_us, u64 desired_us)
{
    const f32 average_ratio = average_us / static_cast<f32>(2 * kBlockSamples);
    f32 desired = static_cast<f32>(desired_us) + static_cast<f32>(kPeriodUs) / 2.0f;
    if (average_ratio < 1.0f) desired /= std::max(average_ratio, 0.25f);
    return desired;
}

// The period length: shorter while the host holds less than the target (down to half a period, quadratically), longer
// while it holds more (up to 1.2 periods). `average_us`: the rolling average of what the host held after each period.
inline u64 dynamic_period_us(u64 enqueued_us, f32 average_us, u64 desired_us)
{
    const f32 rate = static_cast<f32>(enqueued_us) / adjusted_target_us(average_us, desired_us); // 1: the target
    if (rate >= 1.0f) {
        return kMaxPeriodUs - static_cast<u64>(static_cast<f32>(kMaxPeriodUs - kPeriodUs) * (1.0f / rate));
    }
    return kMinPeriodUs + static_cast<u64>(static_cast<f32>(kPeriodUs - kMinPeriodUs) * (rate * rate));
}

enum class Step : u8 {
    Wait,          // the period isn't over yet
    WaitForGame,   // a block is untouched and the game may still write it
    WaitForWrites, // the game is writing a block
    Silence,       // nothing to mix: play a silent block and advance
    Skip,          // the game left every block untouched too long: advance without playing (an audio "frameskip";
                   // its late cellAudioAddData lands on the next block)
    Mix,           // mix the blocks (untouched ones are silent), play them and advance
};

struct Decision {
    Step step = Step::Wait;
    u64 wait_us = 0; // for the waits
};

// One turn of the buffering loop, after its tag count: `since_us` since the last period ended, the host holding
// `enqueued_us`.
inline Decision decide(u64 since_us, u64 enqueued_us, f32 average_us, u64 desired_us, const TagCount& tags)
{
    const s64 left = static_cast<s64>(dynamic_period_us(enqueued_us, average_us, desired_us)) - static_cast<s64>(since_us);
    if (left > kPeriodComparisonMarginUs) return {Step::Wait, static_cast<u64>(left - kPeriodComparisonMarginUs)};
    if (tags.active == 0) return {Step::Silence};
    bool waited_long_enough = false;
    if (tags.untouched_fed > 0) {
        // Games may skip periods when they fall behind: stop waiting for them after a while.
        if ((tags.untouched == tags.active && since_us > kFullyUntouchedTimeoutUs) ||
            since_us > kPartiallyUntouchedTimeoutUs) {
            if (tags.untouched >= tags.active) return {Step::Skip};
            waited_long_enough = true; // mix what is there: the untouched blocks are silent
        } else {
            return {Step::WaitForGame, kWaitForGameUs};
        }
    }
    if (tags.untouched == tags.active) return {Step::Silence}; // nothing written, and nothing to wait for
    if (!waited_long_enough && tags.in_progress > 0) return {Step::WaitForWrites, kWaitForWritesUs};
    return {Step::Mix};
}

// ---- time stretching (D-075)
//
// While the game delivers its audio slower than real time, the speaker plays it slower (keeping the pitch) instead of
// running dry: the ps3-ios-engine port's follow-the-game tempo (its cellAudio.cpp, g_cellaudio_follow_game), which
// replaced RPCS3's own rule there (a tempo that moves halfway to the buffer's fill in steps of 0.1 swings between the
// game's pace and real time). The game's pace is measured from the periods, the tempo follows it, trimmed toward the
// target fill, and stretching switches on only once the game falls behind and off once it has kept pace for 2 s, since
// every switch splices the audio once.

inline constexpr f32 kPaceTimeConstantUs = 700'000.0f; // the pace's average (Citra's 0.7 s)
inline constexpr u64 kPaceMaxPeriodUs = 500'000;       // a longer period is a pause or a restart, not the game's pace
inline constexpr f32 kFillTimeConstantUs = 500'000.0f; // the switch looks at a 0.5 s average of the fill
inline constexpr f32 kMinTempo = 0.25f;                // slower than this, the speaker runs dry instead
inline constexpr f32 kFillTrim = 0.3f;                 // 30% faster or slower per 100% of fill error
inline constexpr f32 kTempoStep = 0.02f;               // smaller changes are inaudible (and re-plan the stretcher)
inline constexpr f32 kStretchStartPace = 0.95f;        // start below this pace,
inline constexpr f32 kStretchStartFill = 0.3f;         // or with the fill below this while the game isn't ahead
inline constexpr f32 kStretchStartFillPace = 0.99f;
inline constexpr f32 kStretchStopPace = 0.98f;         // stop once the pace and the fill stay above these
inline constexpr f32 kStretchStopFill = 0.6f;
inline constexpr u64 kStretchCalmUs = 2'000'000;       // for this long

struct Stretch {
    f32 pace = 1.0f; // the game's delivery rate as a share of real time
    f32 fill = 1.0f; // the host's buffer vs the adjusted target, 0.5 s average
    u64 fill_at = 0; // when `fill` was last updated (us; 0: never)
    bool on = false; // playing through the stretcher
    u64 calm_since = 0;
    f32 tempo = 1.0f; // the stretcher's tempo while on

    // After each period, `dt_us` after the previous one: whether it waited for the game, and whether it played
    // anything. A period that waited lasted as long as the game took to deliver: its pace is nominal / actual. One
    // that didn't wait was on time: at least 1, whatever length the buffer management chose (1.2 periods when the
    // buffer is full must not read as slowness). A skipped period delivered nothing.
    void period(u64 dt_us, bool waited, bool played)
    {
        if (!dt_us || dt_us >= kPaceMaxPeriodUs) return;
        const f32 pace_now = static_cast<f32>(kPeriodUs) / static_cast<f32>(dt_us);
        const f32 rate = !played ? 0.0f : waited ? std::min(pace_now, 1.0f) : std::clamp(pace_now, 1.0f, 2.0f);
        pace += (1.0f - std::exp(-static_cast<f32>(dt_us) / kPaceTimeConstantUs)) * (rate - pace);
    }

    // Each turn, at `now_us`, with the host holding `fill_now` of the adjusted target. Returns true when the speaker's
    // setting changed (`on`, or `tempo` while on).
    bool update(f32 fill_now, u64 now_us)
    {
        const u64 dt = fill_at ? std::min<u64>(now_us - fill_at, 500'000) : 500'000;
        fill += (1.0f - std::exp(-static_cast<f32>(dt) / kFillTimeConstantUs)) * (fill_now - fill);
        fill_at = now_us;
        const bool was_on = on;
        if (!on) {
            if (pace < kStretchStartPace || (fill < kStretchStartFill && pace < kStretchStartFillPace)) {
                on = true;
                calm_since = 0;
            }
        } else if (pace >= kStretchStopPace && fill >= kStretchStopFill) {
            if (!calm_since) calm_since = now_us;
            else if (now_us - calm_since >= kStretchCalmUs) on = false;
        } else {
            calm_since = 0;
        }
        if (!on) {
            tempo = 1.0f;
            return was_on;
        }
        // The pace the game delivers at, trimmed toward the target fill; near 1, exactly 1.
        const f32 wanted = std::clamp(pace * (1.0f + kFillTrim * (fill_now - 1.0f)), kMinTempo, 1.0f);
        const f32 target = wanted >= 0.98f ? 1.0f : wanted;
        if (was_on && std::abs(target - tempo) < kTempoStep && !(target == 1.0f && tempo != 1.0f)) return false;
        tempo = target;
        return true;
    }
};

} // namespace pspocket::hle::audio_buffering
