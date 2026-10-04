// cellAudio's buffering mode, the decisions (runtime/hle/src/audio_buffering.hpp, D-074), against RPCS3's
// count_port_buffer_tags and the buffering branch of cell_audio_thread::operator(): where the tags go, what a block
// looks like after the game wrote all, part or none of it, which ports the game feeds, the period length, each turn's
// choice (with D-074's per-port waits instead of RPCS3's count of untouched ports), and the time stretcher's tempo
// (D-075, the ps3-ios-engine port's follow-the-game rule).

#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "audio_buffering.hpp"

using namespace pspocket;
using namespace pspocket::hle::audio_buffering;

namespace {

// A guest memory stand-in holding one block at address 0x100.
struct Mem {
    static constexpr u32 kBlock = 0x100;
    std::vector<u8> bytes = std::vector<u8>(0x100 + 8 * 256 * 4 + 0x100, 0);
    u8* base() { return bytes.data(); }
    f32 at(u32 i) { return ps_readf32(base(), kBlock + 4 * i); }
    void set(u32 i, f32 v) { ps_writef32(base(), kBlock + 4 * i, v); }
    void fill(u32 floats, f32 v)
    {
        for (u32 i = 0; i < floats; i++) set(i, v);
    }
};

bool negative_zero(f32 v) { return v == 0.0f && std::signbit(v); }

// The tag count for these ports' blocks, all fed by the game unless `fed` says otherwise.
TagCount counted(std::initializer_list<Block> blocks, u32 active, bool fed = true)
{
    TagCount c;
    c.active = active;
    for (const Block b : blocks) count(c, b, fed);
    return c;
}

} // namespace

TEST_CASE("audio buffering: the tags and marks sit where RPCS3 puts them", "[hle][audio]")
{
    // Stereo: the last float of the block and every 102nd before it.
    const u32 stereo[] = {1, 103, 205, 307, 409, 511};
    // 8 channels: every 409th float back from the last, on channels 2 to 7.
    const u32 surround[] = {2, 411, 820, 1229, 1638, 2047};
    for (u32 nr = 0; nr < kTagCount; nr++) {
        CHECK(tag_position(2, nr) == stereo[nr]);
        CHECK(tag_position(8, nr) == surround[nr]);
        CHECK(tag_position(8, nr) % 8 == nr + 2);
        CHECK(mark_position(8, nr) == nr * 51 * 8 + 1); // front right of samples 0, 51, ..., 255
    }

    Mem m;
    PortTags t;
    m.fill(8 * 256, 0.5f);
    tag_block(m.base(), Mem::kBlock, 8, t);
    u32 tagged = 0, others = 0;
    for (u32 i = 0; i < 8 * 256; i++) {
        bool is_tag = false;
        for (u32 nr = 0; nr < kTagCount; nr++) is_tag |= i == tag_position(8, nr) || i == mark_position(8, nr);
        if (is_tag) tagged += negative_zero(m.at(i));
        else others += m.at(i) == 0.5f; // tag_block writes the tags only (the thread clears the block first)
    }
    CHECK(tagged == 2 * kTagCount);
    CHECK(others == 8 * 256 - 2 * kTagCount);
    CHECK(t.prev_touched == kNoTag);
    CHECK(t.periods_without_tag == 1);
}

TEST_CASE("audio buffering: a block's state from its tags", "[hle][audio]")
{
    Mem m;
    PortTags t;

    // A port's first blocks were cleared at cellAudioPortStart, never tagged: played without waiting.
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Incomplete);

    // Tagged and not written: untouched, as long as the game doesn't write.
    tag_block(m.base(), Mem::kBlock, 2, t);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Untouched);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Untouched);

    // cellAudioAddData writes the whole block at once: complete at the next look, and still complete after.
    m.fill(512, 0.25f);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Complete);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Complete);

    // Silence written over the tags counts as written: +0.0f is not the -0.0f tag.
    tag_block(m.base(), Mem::kBlock, 2, t);
    m.fill(512, 0.0f);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Complete);

    // Written in two halves: in progress while a tag moves, incomplete once nothing moves, complete with the rest.
    tag_block(m.base(), Mem::kBlock, 2, t);
    m.fill(256, 0.5f); // tags 0 to 2
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::InProgress);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Incomplete);
    m.fill(512, 0.5f);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Complete);

    // Written again from the start after it was complete (a retouch): in progress.
    m.set(tag_position(2, 0), 0.75f);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::InProgress);
}

TEST_CASE("audio buffering: a port is fed from the game's first write until 340 ms without one", "[hle][audio]")
{
    Mem m;
    PortTags t;
    CHECK_FALSE(t.fed); // a port just started: not waited for
    tag_block(m.base(), Mem::kBlock, 2, t);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Untouched);
    CHECK_FALSE(t.fed);
    m.fill(512, 0.5f);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Complete);
    CHECK(t.fed);
    // The game stops: still fed for kQuietPeriods periods, then a stopped stream.
    for (u64 period = 1; period <= kQuietPeriods; period++) {
        tag_block(m.base(), Mem::kBlock, 2, t);
        CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Untouched);
    }
    CHECK(t.fed);
    tag_block(m.base(), Mem::kBlock, 2, t);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Untouched);
    CHECK_FALSE(t.fed);
    // One write and it is fed again.
    m.fill(512, 0.25f);
    CHECK(classify_block(m.base(), Mem::kBlock, 2, t) == Block::Complete);
    CHECK(t.fed);
}

TEST_CASE("audio buffering: an 8-channel port fed on its front channels only", "[hle][audio]")
{
    Mem m;
    PortTags t;
    const auto front_only_write = [&] {
        for (u32 i = 0; i < 256; i++) {
            m.set(8 * i, 0.5f);
            m.set(8 * i + 1, -0.5f);
        }
    };
    // No tag ever moves (they sit on channels 2 to 7): untouched until the port has gone 8 periods without one, then
    // the marks on front right show the block holds audio.
    for (u32 period = 1; period <= kFrontOnlySettlePeriods; period++) {
        tag_block(m.base(), Mem::kBlock, 8, t);
        front_only_write();
        CHECK(classify_block(m.base(), Mem::kBlock, 8, t) == Block::Untouched);
    }
    tag_block(m.base(), Mem::kBlock, 8, t);
    front_only_write();
    CHECK(classify_block(m.base(), Mem::kBlock, 8, t) == Block::Incomplete);
    CHECK(t.front_only);
    // It stays so for the blocks after, even while a block is still empty.
    tag_block(m.base(), Mem::kBlock, 8, t);
    CHECK(classify_block(m.base(), Mem::kBlock, 8, t) == Block::Incomplete);
    // A tag that moves makes it an ordinary port again.
    m.fill(8 * 256, 0.25f);
    CHECK(classify_block(m.base(), Mem::kBlock, 8, t) == Block::Complete);
    CHECK_FALSE(t.front_only);
}

TEST_CASE("audio buffering: the target and the period length", "[hle][audio]")
{
    CHECK(kPeriodUs == 5333);
    CHECK(desired_buffer_us(0) == 34'000);    // RPCS3's default
    CHECK(desired_buffer_us(1024) == 34'000); // 21 ms callbacks + 2 blocks = 32 ms
    CHECK(desired_buffer_us(4096) == 96'000); // 85 ms callbacks + 2 blocks
    CHECK(restart_blocks(34'000) == 9);

    // RPCS3 computes in floats: within 2 us.
    const auto period = [](u64 enqueued_us, f32 average_us) {
        return static_cast<double>(dynamic_period_us(enqueued_us, average_us, 34'000));
    };
    const u64 target = 34'000 + kPeriodUs / 2; // what the rate compares the host's buffer with
    const f32 settled = 30'000.0f;             // an average that leaves the target alone
    CHECK(period(0, settled) == kMinPeriodUs); // empty: twice as fast
    CHECK(period(target, settled) == Catch::Approx(kPeriodUs).margin(2)); // on target: real time
    CHECK(period(target / 2, settled) == Catch::Approx(kMinPeriodUs + (kPeriodUs - kMinPeriodUs) / 4.0).margin(2));
    CHECK(period(2 * target, settled) == Catch::Approx(kMaxPeriodUs - (kMaxPeriodUs - kPeriodUs) / 2.0).margin(2));
    CHECK(period(1'000'000, settled) <= kMaxPeriodUs); // full: at most 1.2 periods
    CHECK(period(1'000'000, settled) > kPeriodUs + 1000);
    // A buffer that has stayed all but empty (an average under 512 us) aims up to 4 times higher.
    CHECK(period(target, 0.0f) == Catch::Approx(kMinPeriodUs + (kPeriodUs - kMinPeriodUs) / 16.0).margin(2));
}

TEST_CASE("audio buffering: each turn waits for the period, then for the game, then plays", "[hle][audio]")
{
    // An empty host buffer: the shortest period, half a block period.
    const f32 avg = 30'000.0f;
    const auto turn = [&](u64 since_us, const TagCount& tags) { return decide(since_us, 0, avg, 34'000, tags); };

    // The period isn't over: wait for the rest of it (less the margin).
    Decision d = turn(1000, counted({Block::Complete}, 1));
    CHECK(d.step == Step::Wait);
    CHECK(d.wait_us == kMinPeriodUs - 1000 - 5);

    // No started port: silence. Every block written: mix.
    CHECK(turn(kPeriodUs, TagCount{}).step == Step::Silence);
    CHECK(turn(kPeriodUs, counted({Block::Complete, Block::Incomplete}, 2)).step == Step::Mix);

    // The game is writing: half a millisecond more.
    d = turn(kPeriodUs, counted({Block::InProgress}, 1));
    CHECK(d.step == Step::WaitForWrites);
    CHECK(d.wait_us == 500);

    // A port the game feeds, untouched: wait 1 ms at a time for up to 2 periods, then skip the period (nothing played;
    // the game's late write lands on the next block). The next period waits for it again.
    const TagCount untouched = counted({Block::Untouched}, 1);
    d = turn(kPeriodUs + 100, untouched);
    CHECK(d.step == Step::WaitForGame);
    CHECK(d.wait_us == 1000);
    CHECK(turn(2 * kPeriodUs, untouched).step == Step::WaitForGame);
    CHECK(turn(2 * kPeriodUs + 1, untouched).step == Step::Skip);
    CHECK(turn(kPeriodUs, untouched).step == Step::WaitForGame);

    // A port the game doesn't feed (never written, or not for 340 ms): not waited for. Alone: silence; with a ready
    // port: mixed at once (D-074; GTA V's unused second port).
    CHECK(turn(kPeriodUs, counted({Block::Untouched}, 1, false)).step == Step::Silence);
    TagCount quiet_and_ready = counted({Block::Untouched}, 2, false);
    count(quiet_and_ready, Block::Complete, true);
    CHECK(turn(kPeriodUs, quiet_and_ready).step == Step::Mix);

    // A fed port untouched beside a ready one: wait up to 4 periods, then mix what is there; having waited that long,
    // a block still being written is played as it is.
    const TagCount half = counted({Block::Untouched, Block::Complete}, 2);
    CHECK(turn(3 * kPeriodUs, half).step == Step::WaitForGame);
    CHECK(turn(4 * kPeriodUs + 1, half).step == Step::Mix);
    CHECK(turn(4 * kPeriodUs + 1, counted({Block::Untouched, Block::InProgress}, 2)).step == Step::Mix);
}

TEST_CASE("audio buffering: the game's pace, from how its periods went", "[hle][audio]")
{
    // On time, whatever length the buffer management chose: real time.
    Stretch st;
    for (u32 i = 0; i < 400; i++) st.period(6 * kPeriodUs / 5, false, true);
    CHECK(st.pace == Catch::Approx(1.0f));
    // A game that takes two periods for each block: the pace goes to one half over about 0.7 s.
    for (u32 i = 0; i < 66; i++) st.period(2 * kPeriodUs, true, true); // 0.7 s
    CHECK(st.pace == Catch::Approx(0.5f + 0.5f * std::exp(-1.0f)).margin(0.01));
    for (u32 i = 0; i < 400; i++) st.period(2 * kPeriodUs, true, true);
    CHECK(st.pace == Catch::Approx(0.5f).margin(0.001));
    // Skipped periods delivered nothing; a period of half a second or more is a pause and doesn't count.
    const f32 before = st.pace;
    st.period(500'000, true, true);
    CHECK(st.pace == before);
    for (u32 i = 0; i < 400; i++) st.period(2 * kPeriodUs, true, false);
    CHECK(st.pace < 0.01f);
}

TEST_CASE("audio buffering: stretching starts behind the game, follows its pace and stops once it keeps up",
    "[hle][audio]")
{
    Stretch st;
    u64 now = 1'000'000;
    const auto turn = [&](f32 fill) {
        now += kPeriodUs;
        return st.update(fill, now);
    };

    // The game keeps pace with the buffer at its target: no stretching.
    CHECK_FALSE(turn(1.0f));
    CHECK_FALSE(st.on);

    // The pace drops to 0.6: on, at the pace (the fill on target).
    st.pace = 0.6f;
    CHECK(turn(1.0f));
    CHECK(st.on);
    CHECK(st.tempo == Catch::Approx(0.6f));
    // Trimmed toward the target: 30% slower per 100% of missing fill, never below 0.25.
    CHECK(turn(0.5f));
    CHECK(st.tempo == Catch::Approx(0.6f * 0.85f));
    st.pace = 0.2f;
    CHECK(turn(1.0f));
    CHECK(st.tempo == Catch::Approx(0.25f));
    // Changes under 0.02 aren't passed on.
    st.pace = 0.6f;
    CHECK(turn(1.0f));
    st.pace = 0.61f;
    CHECK_FALSE(turn(1.0f));
    CHECK(st.tempo == Catch::Approx(0.6f));

    // Back to real time: the tempo goes to 1 at once (0.98 and up are 1), and stretching stops after 2 s of it with
    // the buffer at least 60% full.
    st.pace = 0.99f;
    CHECK(turn(1.0f));
    CHECK(st.tempo == 1.0f);
    CHECK(st.on);
    u32 turns = 0;
    while (st.on && turns < 1000) {
        turn(1.0f);
        turns++;
    }
    CHECK_FALSE(st.on);
    CHECK(static_cast<double>(turns) * kPeriodUs >= 2'000'000.0);
    CHECK(static_cast<double>(turns) * kPeriodUs < 2'100'000.0);

    // A buffer running low while the game isn't ahead also starts it; one ahead of real time doesn't.
    Stretch low;
    low.pace = 0.985f;
    low.fill = 0.2f;
    CHECK(low.update(0.2f, 2'000'000));
    CHECK(low.on);
    Stretch ahead;
    ahead.fill = 0.2f;
    CHECK_FALSE(ahead.update(0.2f, 2'000'000));
}
