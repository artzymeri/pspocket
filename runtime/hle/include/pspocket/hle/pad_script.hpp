#pragma once

// Scripted presses of the virtual controller (pspocket-boot --press), for runs on the Mac where nobody holds a
// controller: "cross@60" holds Cross from 60 s for a quarter of a second, "cross@60+10" does it again every 10 s,
// "start@30:1.5" holds Start for 1.5 s. Times are seconds since the game started. The sticks are pushed all the way
// with the pseudo-buttons ls-up, ls-down, ls-left, ls-right (left stick) and rs-up ... rs-right (right stick):
// "ls-up@100:5" walks forward for 5 s.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/hle/input.hpp"

namespace pspocket::hle {

struct PadPress {
    std::string button; // as written: cross, circle, square, triangle, start, select, up, down, left, right, l1, r1, l2, r2, l3, r3
    u16 digital1 = 0, digital2 = 0; // its CellPadData bits
    int pressure = -1;              // its index in PadInput::pressure, or -1 (start, select, L3, R3)
    int axis = -1;                  // a stick direction: right x, right y, left x, left y (PadInput order), or -1
    u8 axis_value = 128;            // where the stick is pushed to: 0 (left, up) or 255 (right, down)
    double at = 0, every = 0, hold = 0.25;

    // Whether the button is down at `t` seconds.
    bool down_at(double t) const;
};

// "BUTTON@START[+EVERY][:HOLD]", or nullopt (with `error` set) when the button or a number is not understood.
std::optional<PadPress> parse_pad_press(std::string_view text, std::string* error = nullptr);

// The controller's state at `t` seconds: every press that is down, the rest at rest.
PadInput pad_state_at(const std::vector<PadPress>& presses, double t);

} // namespace pspocket::hle
