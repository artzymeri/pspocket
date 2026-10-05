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
