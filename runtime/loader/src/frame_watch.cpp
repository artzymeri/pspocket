// Glitch detectors over frame thumbnails and frame timing (pspocket/loader/frame_watch.hpp, D-1064).

#include "pspocket/loader/frame_watch.hpp"

#include <algorithm>
#include <cstdlib>

namespace pspocket::loader {

FrameWatch::FrameWatch() : config_() {}
FrameWatch::FrameWatch(const Config& c) : config_(c) {}

const char* FrameWatch::name(Kind k)
{
    switch (k) {
    case Kind::PartlyBlack: return "partly-black";
    case Kind::Flicker: return "flicker";
    case Kind::Hitch: return "hitch";
    case Kind::Frozen: return "frozen";
    case Kind::Flagged: return "flagged";
    }
    return "?";
}

FrameWatch::Thumb FrameWatch::thumbnail(const u8* bgra, u32 w, u32 h, usize stride)
{
    Thumb t{};
    if (!bgra || !w || !h) return t;
    for (u32 ty = 0; ty < kH; ty++) {
        const u32 y0 = ty * h / kH, y1 = std::max(y0 + 1, (ty + 1) * h / kH);
        for (u32 tx = 0; tx < kW; tx++) {
            const u32 x0 = tx * w / kW, x1 = std::max(x0 + 1, (tx + 1) * w / kW);
            u64 sum[3] = {};
            for (u32 y = y0; y < y1; y++) {
                const u8* row = bgra + y * stride + x0 * 4;
                for (u32 x = x0; x < x1; x++, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            const u64 n = u64{y1 - y0} * (x1 - x0);
            for (u32 c = 0; c < 3; c++) t[(ty * kW + tx) * 3 + c] = static_cast<u8>(sum[c] / n);
        }
    }
    return t;
}

double FrameWatch::black_share(const Thumb& t)
{
    u32 black = 0;
    for (usize i = 0; i < t.size(); i += 3) black += t[i] == 0 && t[i + 1] == 0 && t[i + 2] == 0;
    return static_cast<double>(black) / (kW * kH);
}

double FrameWatch::difference(const Thumb& a, const Thumb& b)
{
    u64 total = 0;
    for (usize i = 0; i < a.size(); i++) total += static_cast<u64>(std::abs(int{a[i]} - int{b[i]}));
    return static_cast<double>(total) / static_cast<double>(a.size());
}

void FrameWatch::emit(Kind k, u64 flip, u64 now_ns, double value)
{
    const usize i = static_cast<usize>(k);
    counts[i]++;
    if (last_emit_ns_[i] && now_ns - last_emit_ns_[i] < config_.min_gap_ns) return;
    last_emit_ns_[i] = now_ns ? now_ns : 1;
    if (on_event) on_event({k, flip, now_ns, value});
}

void FrameWatch::frame(const Thumb& t, u64 flip, u64 now_ns)
{
    // The frame rate over the last second: a hitch counts only while the game was flipping.
    if (!fps_window_ns_) fps_window_ns_ = now_ns;
    fps_frames_++;
    if (now_ns - fps_window_ns_ >= 1'000'000'000) {
        fps_ = static_cast<double>(fps_frames_) * 1e9 / static_cast<double>(now_ns - fps_window_ns_);
        fps_window_ns_ = now_ns;
        fps_frames_ = 0;
    }
    if (last_ns_ && now_ns - last_ns_ > config_.hitch_ns && fps_ >= 10)
        emit(Kind::Hitch, flip, now_ns, static_cast<double>(now_ns - last_ns_) / 1e6);
    last_ns_ = now_ns;

    const double black = black_share(t);
    black_run_ = black >= config_.black_min && black < config_.black_max ? black_run_ + 1 : 0;
    if (black_run_ == 2) emit(Kind::PartlyBlack, flip, now_ns, black);

    if (filled_ >= 2) {
        const Thumb& b = ring_[head_];                              // the frame before
        const Thumb& a = ring_[(head_ + ring_.size() - 1) % ring_.size()]; // and the one before it
        const double ab = difference(a, b), bc = difference(b, t), ac = difference(a, t);
        if (ab > config_.flicker_diff && bc > config_.flicker_diff && ac < config_.flicker_back * std::min(ab, bc))
            emit(Kind::Flicker, flip, now_ns, std::min(ab, bc));
    }
    if (filled_ >= 1) {
        if (difference(ring_[head_], t) < 0.5) {
            if (!same_since_ns_) same_since_ns_ = now_ns;
            else if (now_ns - same_since_ns_ >= config_.frozen_ns) {
                emit(Kind::Frozen, flip, now_ns, static_cast<double>(now_ns - same_since_ns_) / 1e9);
                same_since_ns_ = now_ns; // again after another frozen stretch
            }
        } else {
            same_since_ns_ = 0;
        }
    }
    head_ = (head_ + 1) % ring_.size();
    ring_[head_] = t;
    filled_ = std::min(filled_ + 1, ring_.size());
}

void FrameWatch::frame_bgra(const u8* bgra, u32 width, u32 height, usize stride, u64 flip, u64 now_ns)
{
    frame(thumbnail(bgra, width, height, stride), flip, now_ns);
}

void FrameWatch::flag(u64 flip, u64 now_ns)
{
    const usize i = static_cast<usize>(Kind::Flagged);
    counts[i]++;
    last_emit_ns_[i] = now_ns;
    if (on_event) on_event({Kind::Flagged, flip, now_ns, 0});
}

std::vector<FrameWatch::Thumb> FrameWatch::recent() const
{
    std::vector<Thumb> out;
    for (usize k = filled_; k > 0; k--) out.push_back(ring_[(head_ + ring_.size() - (k - 1)) % ring_.size()]);
    return out;
}

} // namespace pspocket::loader
