#include "pspocket/rsx/null_backend.hpp"

#include "pspocket/core/log.hpp"
#include "pspocket/rsx/state.hpp"

namespace pspocket::rsx {

namespace {
constexpr std::string_view kChan = "rsx";
constexpr u64 kLoggedFlips = 3;
} // namespace

void NullBackend::attach(Rsx&) { PS_LOG_INFO(kChan, "null backend: commands run, nothing is drawn"); }

void NullBackend::draw(const RsxState& state)
{
    if (draws_.fetch_add(1, std::memory_order_relaxed) == 0) {
        PS_LOG_INFO(kChan, "null backend: first draw ({} vertices)", state.draw.vertex_count());
    }
}

void NullBackend::clear(u32, const RsxState&) { clears_.fetch_add(1, std::memory_order_relaxed); }

void NullBackend::flip(const FlipRequest& request, const RsxState&)
{
    const u64 n = flips_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLoggedFlips || (n & (n - 1)) == 0) {
        PS_LOG_INFO(kChan, "null backend: flip {} (display buffer {}{}), {} draws and {} clears so far", n,
            request.buffer_id, request.buffer ? "" : ", not registered", draws(), clears());
    }
}

} // namespace pspocket::rsx
