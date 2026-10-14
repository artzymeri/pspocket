#pragma once

// The screen's refresh as the host sees it (D-971). A platform with a display link reports each refresh; the GCM
// interrupt thread then times the game's vblanks on the screen's own clock instead of a free-running 59.94 Hz timer.
// That timer drifts against a 60 Hz screen: a game flipping at every second vblank had a frame held a refresh longer
// about every 8 to 17 s, a hitch at full speed. Without reports (pspocket-boot on the Mac, a display link stopped) the
// vblanks run on their own as before.

#include <chrono>

namespace pspocket::hle {

// A refresh of the screen at `at` (steady_clock), `period` apart from the next. Thread-safe; any thread.
void display_vsync(std::chrono::steady_clock::time_point at, std::chrono::nanoseconds period);

// Forgets the reported refreshes (tests; a platform whose display link stopped need not call it: reports older than
// 100 ms are not used).
void display_vsync_reset();

// The vblank after `previous`: `nominal` (previous plus the vblank period) moved onto the screen's refresh nearest to
// it while a refresh was reported in the 100 ms before `now`, else `nominal` itself. Never within half a vblank period
// of `previous` (no two vblanks for one refresh).
std::chrono::steady_clock::time_point next_vblank(std::chrono::steady_clock::time_point previous,
    std::chrono::steady_clock::time_point nominal, std::chrono::steady_clock::time_point now);

} // namespace pspocket::hle
