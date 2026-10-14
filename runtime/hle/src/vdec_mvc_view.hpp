#pragma once

// libmvcdec's second view (D-933): the second view of a stereo MVC stream decoded with FFmpeg's H.264 decoder, which
// has no MVC (vdec_mvc_view.cpp says how). Nothing here is Sony's code or data.

#include <memory>
#include <span>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::vdec::avc {

// The base view's picture of an access unit, as decoded: planar 4:2:0 at its coded size, and its order count.
struct BaseView {
    std::vector<u8> y, u, v;
    u32 width = 0, height = 0;
    s32 poc = 0;
};

// A picture's samples: planar 4:2:0 at the coded size.
struct ViewSamples {
    std::vector<u8> y, u, v;
};

class SecondView {
public:
    SecondView();
    ~SecondView();
    SecondView(const SecondView&) = delete;
    SecondView& operator=(const SecondView&) = delete;

    // Everything forgotten (a reset, a new sequence).
    void reset();
    // One access unit's second view (its NAL units: subset SPS, PPS, coded slice extensions) after its base view's
    // picture: the second view's samples, or false when they cannot be decoded so (said once in the log).
    bool decode(std::span<const u8> au, const BaseView& base, ViewSamples& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pspocket::hle::vdec::avc
