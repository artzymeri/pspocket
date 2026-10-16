#pragma once

// What a player would call a glitch, from each frame's 64x36 thumbnail and the time between frames (D-1064): the
// same detectors on the Mac (pspocket-boot, the bench) and the iPhone (the app's reports). Plain C++: no platform code.
//
//   partly black  a fifth to 95% of the thumbnail exactly black (all three channels 0) for two frames in a row: a
//                 surface nothing drew (the research bench's black sea: 56%, whole rows). Rendering with fog and light
//                 all but never gives 0 over an area (a night street from the air: 2.5%); a whole black frame is a fade.
//   flicker       A, B, A: a frame far from the one before (mean difference over 20 of 255) and the next close to the
//                 one before that again (under a third of it): something drawn in every other frame. A camera cut is
//                 A, B, B.
//   hitch         over 500 ms between two frames while the game was flipping at least 10 a second.
//   frozen        the same picture (mean difference under 0.5) for 60 s while frames keep coming: the game stuck
//                 drawing one frame (a loop that still flips).
// Events are rate-limited per kind (one per kind every `min_gap_ns`); every one is counted.

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::loader {

class FrameWatch {
public:
    static constexpr u32 kW = 64, kH = 36;
    using Thumb = std::array<u8, kW * kH * 3>; // B G R per pixel

    enum class Kind { PartlyBlack, Flicker, Hitch, Frozen, Flagged };
    struct Event {
        Kind kind;
        u64 flip = 0;
        u64 time_ns = 0;
        double value = 0; // the black share, the difference, the gap in ms, the frozen seconds
    };
    static const char* name(Kind k);

    struct Config {
        double black_min = 0.2, black_max = 0.95;
        double flicker_diff = 20, flicker_back = 1.0 / 3;
        u64 hitch_ns = 500'000'000;
        u64 frozen_ns = 60'000'000'000; // a loading screen holds still 10 s and more (Assassin's Creed's, D-1070)
        u64 min_gap_ns = 120'000'000'000; // one event per kind every 2 minutes
    };

    FrameWatch();
    explicit FrameWatch(const Config& c);
    // Called for each presented frame, in order. Events that pass the rate limit go to `on_event`.
    void frame(const Thumb& t, u64 flip, u64 now_ns);
    // From BGRA pixels of any size (box-filtered into a thumbnail).
    void frame_bgra(const u8* bgra, u32 width, u32 height, usize stride, u64 flip, u64 now_ns);
    // The player's own flag (the app's button).
    void flag(u64 flip, u64 now_ns);

    std::function<void(const Event&)> on_event;
    // Every event detected, rate-limited or not, by kind.
    std::array<u64, 5> counts{};
    // The last thumbnails (oldest first), for a report.
    std::vector<Thumb> recent() const;

    static Thumb thumbnail(const u8* bgra, u32 width, u32 height, usize stride);
    static double black_share(const Thumb& t);
    static double difference(const Thumb& a, const Thumb& b);

private:
    void emit(Kind k, u64 flip, u64 now_ns, double value);
    Config config_;
    std::array<Thumb, 32> ring_{};
    usize filled_ = 0, head_ = 0; // ring_[head_] is the newest
    u64 last_ns_ = 0, fps_window_ns_ = 0, fps_frames_ = 0;
    double fps_ = 0;
    u32 black_run_ = 0;
    u64 same_since_ns_ = 0;
    std::array<u64, 5> last_emit_ns_{};
};

} // namespace pspocket::loader
