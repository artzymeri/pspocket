#pragma once

// A single-producer, single-consumer ring of stereo float frames between cellAudio's thread (which pushes one 256-frame
// period every 5.33 ms) and the host's audio callback (which pulls whatever the device asks for). Lock-free; the
// producer never waits and the consumer never blocks the device.
//
// Playback starts once `prebuffer` frames are queued, so the first callbacks don't run dry; after running dry it
// waits for `prebuffer` frames again (an underrun plays silence instead of crackling through a near-empty ring). A
// push that doesn't fit is dropped whole (an overrun): the game produced more than the device played.

#include <algorithm>
#include <atomic>
#include <bit>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket {

class AudioRing {
public:
    // `capacity` frames (rounded up to a power of two), playing once `prebuffer` frames are queued.
    explicit AudioRing(u32 capacity = 16384, u32 prebuffer = 2048)
        : mask_(std::bit_ceil(std::max<u32>(capacity, 2)) - 1), prebuffer_(std::min(prebuffer, mask_ + 1)),
          samples_(2 * (usize{mask_} + 1))
    {
    }

    // Producer: `frames` interleaved stereo frames (left, right). False when they didn't fit (dropped).
    bool push(const f32* interleaved, u32 frames)
    {
        const u64 write = write_.load(std::memory_order_relaxed);
        const u64 read = read_.load(std::memory_order_acquire);
        if (write - read + frames > u64{mask_} + 1) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (u32 i = 0; i < frames; i++) {
            const usize at = 2 * static_cast<usize>((write + i) & mask_);
            samples_[at] = interleaved[2 * i];
            samples_[at + 1] = interleaved[2 * i + 1];
        }
        write_.store(write + frames, std::memory_order_release);
        return true;
    }

    // Consumer: `frames` frames into the two channel buffers, silence where the ring has none (or is prebuffering).
    // Returns the frames that came from the ring.
    u32 pull(f32* left, f32* right, u32 frames)
    {
        if (frames > largest_pull_.load(std::memory_order_relaxed)) largest_pull_.store(frames, std::memory_order_relaxed);
        const u64 read = read_.load(std::memory_order_relaxed);
        const u64 queued = write_.load(std::memory_order_acquire) - read;
        if (!playing_) {
            if (queued < prebuffer_) {
                std::fill(left, left + frames, 0.0f);
                std::fill(right, right + frames, 0.0f);
                return 0;
            }
            playing_ = true;
        }
        const u32 n = static_cast<u32>(std::min<u64>(queued, frames));
        for (u32 i = 0; i < n; i++) {
            const usize at = 2 * static_cast<usize>((read + i) & mask_);
            left[i] = samples_[at];
            right[i] = samples_[at + 1];
        }
        std::fill(left + n, left + frames, 0.0f);
        std::fill(right + n, right + frames, 0.0f);
        read_.store(read + n, std::memory_order_release);
        played_.fetch_add(n, std::memory_order_relaxed);
        if (n < frames) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            playing_ = false;
        }
        return n;
    }

    u64 underruns() const { return underruns_.load(std::memory_order_relaxed); }
    u64 overruns() const { return overruns_.load(std::memory_order_relaxed); }
    u64 played() const { return played_.load(std::memory_order_relaxed); }
    // The most frames one pull has asked for: the device's callback length (0 before the first pull).
    u32 largest_pull() const { return largest_pull_.load(std::memory_order_relaxed); }
    u32 queued() const
    {
        return static_cast<u32>(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
    }

private:
    const u32 mask_;
    const u32 prebuffer_;
    std::vector<f32> samples_;
    std::atomic<u64> write_{0}, read_{0};
    bool playing_ = false; // consumer only
    std::atomic<u64> underruns_{0}, overruns_{0}, played_{0};
    std::atomic<u32> largest_pull_{0};
};

} // namespace pspocket
