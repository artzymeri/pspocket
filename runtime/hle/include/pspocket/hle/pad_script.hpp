#pragma once

// Scripted presses of the virtual controller (pspocket-boot --press), for runs on the Mac where nobody holds a
// controller: "cross@60" holds Cross from 60 s for a quarter of a second, "cross@60+10" does it again every 10 s,
// "start@30:1.5" holds Start for 1.5 s. Times are seconds since the game started. The sticks are pushed all the way
// with the pseudo-buttons ls-up, ls-down, ls-left, ls-right (left stick) and rs-up ... rs-right (right stick):
// "ls-up@100:5" walks forward for 5 s.
//
// On game time (D-1042): a press can count from something the game does instead of the boot, so a slower or faster
// run presses at the same point of the game. "cross@flip:2200" at the 2200th frame; "cross@after:call:NAME+2s" 2 s
// after the game's first call of the library function NAME (cellSaveDataAutoSave2); "cross@play+5s" 5 s after the game
// is in play (PlayDetector). "after:" may be left out; "+Ns" (an 's') is the offset, a plain "+N" the repeat as before:
// "cross@flip:100+2s+10:0.5". A press file's "wait:ANCHOR" step (wait:play, wait:flip:N, wait:call:NAME+2s) makes the
// presses after it that give plain seconds count from that anchor.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/hle/input.hpp"

namespace pspocket::hle {

// What a press's time counts from: the boot (seconds), a frame number, the game's first call of a library function, or
// the moment the game is in play; then `offset` seconds.
struct PadAnchor {
    enum class Kind : u8 { Boot, Flip, Call, Play };
    Kind kind = Kind::Boot;
    u64 flip = 0;     // Flip: the frame number
    std::string call; // Call: the function's name
    double offset = 0;
    std::string text() const; // as written: "flip:2200+2s"
};

// "play", "flip:N", "call:NAME", each optionally after "after:" and with "+Ns"; nullopt when not one of them.
std::optional<PadAnchor> parse_pad_anchor(std::string_view text, std::string* error = nullptr);

struct PadPress {
    std::string button; // as written: cross, circle, square, triangle, start, select, up, down, left, right, l1, r1, l2, r2, l3, r3
    u16 digital1 = 0, digital2 = 0; // its CellPadData bits
    int pressure = -1;              // its index in PadInput::pressure, or -1 (start, select, L3, R3)
    int axis = -1;                  // a stick direction: right x, right y, left x, left y (PadInput order), or -1
    u8 axis_value = 128;            // where the stick is pushed to: 0 (left, up) or 255 (right, down)
    double at = 0, every = 0, hold = 0.25; // `at` from the anchor
    PadAnchor anchor;
    // When the anchor happened, in seconds since the boot (resolve_pad_anchors); none yet: the press waits.
    std::optional<double> anchored = 0.0;

    // Whether the button is down at `t` seconds since the boot.
    bool down_at(double t) const;
};

// "BUTTON@WHEN[+Ns][+EVERY][:HOLD]", WHEN seconds or an anchor (parse_pad_anchor); or nullopt (with `error` set) when
// the button or a number is not understood. Plain seconds count from `base` when given (a "wait:" step).
std::optional<PadPress> parse_pad_press(std::string_view text, std::string* error = nullptr,
    const PadAnchor* base = nullptr);

// The controller's state at `t` seconds: every press that is down, the rest at rest.
PadInput pad_state_at(const std::vector<PadPress>& presses, double t);

// What the run knows about the game at `t` seconds since the boot, for the anchors.
struct PadClock {
    double t = 0;
    u64 flips = 0;
    bool playing = false;                         // PlayDetector
    std::function<bool(std::string_view)> called; // whether the game has called the library function
};
// Gives each press whose anchor has happened by `clock` its time (at clock.t); returns the anchors that did (their
// text, once each), for the log.
std::vector<std::string> resolve_pad_anchors(std::vector<PadPress>& presses, const PadClock& clock);

// Whether the game is in play: for 3 s, frames at 20 fps or more while it reads the controller 10 times a second or
// more (cellPadGetData's calls); a loading screen that does both counts too, so scripts that need a precise point use
// a flip or call anchor. Fed every 20 ms or so; once in play it stays.
class PlayDetector {
public:
    bool update(double t, u64 flips, u64 pad_reads);
    bool playing() const { return playing_; }
    double since() const { return since_; }

private:
    struct Sample {
        double t;
        u64 flips, reads;
    };
    std::vector<Sample> window_;
    bool playing_ = false;
    double since_ = 0;
    double streak_ = -1; // when the frames and reads became fast enough; -1: they are not
};

} // namespace pspocket::hle
