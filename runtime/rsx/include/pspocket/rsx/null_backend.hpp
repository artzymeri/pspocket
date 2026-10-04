#pragma once

// A backend that renders nothing: the RSX front end still runs the whole command stream (FIFO, state, labels,
// semaphores, reports, flips), so a game that synchronises with the GPU keeps running. For hosts without a GPU
// surface: pspocket-boot on the Mac during bring-up, and the lv2 default when no backend is configured. It counts
// what it was asked to do and logs the first flips and the first draw, so a boot log shows that the game reached them.

#include <atomic>

#include "pspocket/rsx/backend.hpp"

namespace pspocket::rsx {

class NullBackend final : public Backend {
public:
    void attach(Rsx& rsx) override;
    void draw(const RsxState& state) override;
    void clear(u32 mask, const RsxState& state) override;
    void flip(const FlipRequest& request, const RsxState& state) override;
    void sync() override {}
    void invalidate_memory(u32, u32) override {}
    bool flush_memory(u32, u32) override { return false; }

    u64 draws() const { return draws_.load(std::memory_order_relaxed); }
    u64 clears() const { return clears_.load(std::memory_order_relaxed); }
    u64 flips() const { return flips_.load(std::memory_order_relaxed); }

private:
    std::atomic<u64> draws_{0};
    std::atomic<u64> clears_{0};
    std::atomic<u64> flips_{0};
};

} // namespace pspocket::rsx
