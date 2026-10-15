// Guest ranges nothing writes after the executable loaded (pspocket/core/immutable_memory.hpp, D-1013).

#include "pspocket/core/immutable_memory.hpp"

#include <array>
#include <atomic>

namespace pspocket {
namespace {

constexpr u32 kMaxRanges = 8;
std::array<std::atomic<u64>, kMaxRanges> g_ranges{}; // lo in the high half, hi in the low half
std::atomic<u32> g_count{0};

} // namespace

void add_immutable_range(u32 lo, u32 hi)
{
    if (hi <= lo) return;
    const u32 n = g_count.load(std::memory_order_relaxed);
    if (n >= kMaxRanges) return;
    g_ranges[n].store((static_cast<u64>(lo) << 32) | hi, std::memory_order_relaxed);
    g_count.store(n + 1, std::memory_order_release);
}

bool immutable_range(u32 addr, u32 size)
{
    const u64 end = static_cast<u64>(addr) + size;
    const u32 n = g_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < n; i++) {
        const u64 r = g_ranges[i].load(std::memory_order_relaxed);
        if (addr >= (r >> 32) && end <= (r & 0xFFFFFFFFu)) return true;
    }
    return false;
}

void clear_immutable_ranges() { g_count.store(0, std::memory_order_release); }

} // namespace pspocket
