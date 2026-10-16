// Scripted controller presses (pspocket-boot --press): parsing, when a press is down, and the controller state they
// make together.

#include <catch_amalgamated.hpp>

#include "pspocket/hle/pad_script.hpp"

using namespace pspocket;
using namespace pspocket::hle;

TEST_CASE("pad script: BUTTON@START[+EVERY][:HOLD]", "[hle][pad]")
{
    const auto cross = parse_pad_press("cross@60");
    REQUIRE(cross.has_value());
    CHECK(cross->digital2 == 0x40);
    CHECK(cross->pressure == 6);
    CHECK(cross->at == 60.0);
    CHECK(cross->every == 0.0);
    CHECK(cross->hold == 0.25);
    CHECK_FALSE(cross->down_at(59.9));
    CHECK(cross->down_at(60.0));
    CHECK(cross->down_at(60.2));
    CHECK_FALSE(cross->down_at(60.3));
    CHECK_FALSE(cross->down_at(70.1));

    const auto again = parse_pad_press("start@30+10:1.5");
    REQUIRE(again.has_value());
    CHECK(again->digital1 == 0x08);
    CHECK(again->pressure == -1);
    CHECK(again->down_at(31.4));
    CHECK_FALSE(again->down_at(31.6));
    CHECK(again->down_at(40.5));
    CHECK(again->down_at(50.0));
    CHECK_FALSE(again->down_at(29.9));

    std::string error;
    for (const char* bad : {"cross", "cros@60", "cross@", "cross@-1", "cross@60+0", "cross@60:0", "cross@60+1:2",
             "cross@6x"}) {
        INFO(bad);
        CHECK_FALSE(parse_pad_press(bad, &error).has_value());
        CHECK(error.find("--press") != std::string::npos);
    }
}

TEST_CASE("pad script: the controller state is every press that is down", "[hle][pad]")
{
    std::vector<PadPress> presses;
    for (const char* text : {"cross@1", "left@1:2", "r2@5"}) presses.push_back(*parse_pad_press(text));
    PadInput at_rest = pad_state_at(presses, 0.5);
    CHECK(at_rest.digital1 == 0);
    CHECK(at_rest.digital2 == 0);
    CHECK(at_rest.pressure[6] == 0);
    CHECK(at_rest.left_x == 128);

    const PadInput both = pad_state_at(presses, 1.1);
    CHECK(both.digital1 == 0x80);    // left
    CHECK(both.digital2 == 0x40);    // cross
    CHECK(both.pressure[1] == 255);  // left
    CHECK(both.pressure[6] == 255);  // cross
    CHECK(both.pressure[11] == 0);   // R2 not yet

    const PadInput later = pad_state_at(presses, 5.1);
    CHECK(later.digital1 == 0);
    CHECK(later.digital2 == 0x02); // R2
    CHECK(later.pressure[11] == 255);
}

TEST_CASE("pad script: stick directions push one axis all the way while they are down")
{
    std::vector<PadPress> presses;
    for (const char* text : {"ls-up@1:2", "rs-right@1.5", "cross@1"}) presses.push_back(*parse_pad_press(text));
    const PadInput rest = pad_state_at(presses, 0.5);
    CHECK(rest.left_y == 128);
    CHECK(rest.right_x == 128);
    const PadInput walking = pad_state_at(presses, 1.6);
    CHECK(walking.left_y == 0);   // up
    CHECK(walking.left_x == 128); // the other axis stays at rest
    CHECK(walking.right_x == 255);
    CHECK(walking.digital1 == 0);
    CHECK(walking.digital2 == 0); // cross was let go at 1.25 s
    const PadInput later = pad_state_at(presses, 3.5);
    CHECK(later.left_y == 128);
    CHECK_FALSE(parse_pad_press("ls-forward@1").has_value());
}

// D-1042: presses on game time. The old seconds keep working (above); an anchor makes a press wait for the frame, the
// call or the play it names, then count from when it happened.
TEST_CASE("pad script: anchors on game time (flip, call, play) and wait steps", "[hle][pad]")
{
    const auto flip = parse_pad_press("cross@flip:2200");
    REQUIRE(flip.has_value());
    CHECK(flip->anchor.kind == PadAnchor::Kind::Flip);
    CHECK(flip->anchor.flip == 2200);
    CHECK_FALSE(flip->anchored.has_value());
    CHECK_FALSE(flip->down_at(1000)); // not yet anchored: never down

    const auto save = parse_pad_press("circle@after:call:cellSaveDataAutoSave2+2s");
    REQUIRE(save.has_value());
    CHECK(save->anchor.kind == PadAnchor::Kind::Call);
    CHECK(save->anchor.call == "cellSaveDataAutoSave2");
    CHECK(save->anchor.offset == 2.0);
    CHECK(save->every == 0.0);

    const auto again = parse_pad_press("start@flip:100+2s+10:0.5");
    REQUIRE(again.has_value());
    CHECK(again->anchor.offset == 2.0);
    CHECK(again->every == 10.0);
    CHECK(again->hold == 0.5);
    const auto repeat = parse_pad_press("start@play+10:0.5"); // a plain +N is the repeat, as before
    REQUIRE(repeat.has_value());
    CHECK(repeat->anchor.kind == PadAnchor::Kind::Play);
    CHECK(repeat->anchor.offset == 0.0);
    CHECK(repeat->every == 10.0);
    CHECK(parse_pad_press("cross@call:cellPadGetData")->anchor.call == "cellPadGetData"); // "after:" is optional

    std::string error;
    for (const char* bad : {"cross@flip:", "cross@flip:x", "cross@call:", "cross@after:", "cross@after:soon",
             "cross@flip:10+xs", "cross@playx", "cross@flip:10+0", "cross@flip:1.5"}) {
        INFO(bad);
        CHECK_FALSE(parse_pad_press(bad, &error).has_value());
        CHECK(error.find("--press") != std::string::npos);
    }

    // Resolved as they happen: each press counts from the moment its anchor did.
    std::vector<PadPress> presses{*flip, *save, *parse_pad_press("square@play+1s"), *parse_pad_press("triangle@5")};
    std::vector<std::string> calls;
    PadClock clock;
    clock.called = [&](std::string_view n) { return std::find(calls.begin(), calls.end(), n) != calls.end(); };
    clock.t = 10;
    clock.flips = 2000;
    auto fired = resolve_pad_anchors(presses, clock);
    CHECK(fired.empty()); // the seconds since the boot need no anchor; the flip is not there yet
    CHECK(presses[3].down_at(5.1));
    CHECK_FALSE(presses[0].down_at(10.1));
    clock.t = 20;
    clock.flips = 2200;
    calls.push_back("cellSaveDataAutoSave2");
    fired = resolve_pad_anchors(presses, clock);
    CHECK(fired == std::vector<std::string>{"flip:2200", "call:cellSaveDataAutoSave2"});
    CHECK(presses[0].down_at(20.1));
    CHECK_FALSE(presses[1].down_at(21.0));
    CHECK(presses[1].down_at(22.1)); // 2 s after the call was seen
    CHECK_FALSE(presses[2].down_at(30));
    clock.t = 40;
    clock.playing = true;
    CHECK(resolve_pad_anchors(presses, clock) == std::vector<std::string>{"play"});
    CHECK(presses[2].down_at(41.1));
    CHECK(pad_state_at(presses, 41.1).digital2 == 0x80); // square
    CHECK(resolve_pad_anchors(presses, clock).empty()); // once

    // A press file's "wait:" step: plain seconds after it count from its anchor.
    const auto step = parse_pad_anchor("call:cellSaveDataAutoLoad2+1.5s");
    REQUIRE(step.has_value());
    CHECK(step->offset == 1.5);
    const auto after = parse_pad_press("cross@3:1", nullptr, &*step);
    REQUIRE(after.has_value());
    CHECK(after->anchor.call == "cellSaveDataAutoLoad2");
    CHECK(after->at == 3.0);
    std::vector<PadPress> one{*after};
    clock.t = 100;
    calls.push_back("cellSaveDataAutoLoad2");
    resolve_pad_anchors(one, clock);
    CHECK_FALSE(one[0].down_at(104.4));
    CHECK(one[0].down_at(104.6)); // 100 + 1.5 + 3
    CHECK_FALSE(parse_pad_anchor("soon", &error).has_value());
    CHECK(error.find("wait:") != std::string::npos);
    CHECK(parse_pad_anchor("play")->kind == PadAnchor::Kind::Play);
}

TEST_CASE("pad script: the game is in play after 3 s of frames while it reads the controller", "[hle][pad]")
{
    PlayDetector d;
    u64 flips = 0, reads = 0;
    double t = 0;
    for (; t < 10; t += 0.02) { // a loading screen: frames, no controller
        flips += t - static_cast<int>(t) < 0.5 ? 1 : 0;
        CHECK_FALSE(d.update(t, flips, reads));
    }
    const double from = t;
    for (; t < from + 5; t += 0.02) { // 25 frames a second and 50 reads
        if (static_cast<int>(t * 50) % 2 == 0) flips++;
        reads++;
        d.update(t, flips, reads);
    }
    CHECK(d.playing());
    CHECK(d.since() >= from + 2.0); // the first second may reach back into the loading screen
    CHECK(d.since() <= from + 3.2);
    CHECK(d.update(t + 100, flips, reads)); // stays
}
