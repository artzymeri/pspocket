// The audio ring between cellAudio's thread and the host's audio callback: prebuffering, order, underruns (silence,
// then prebuffering again), overruns (a push that doesn't fit is dropped whole), wrap-around, the largest pull, and one
// producer and one consumer on two threads.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "pspocket/core/audio_ring.hpp"

using namespace pspocket;

namespace {

// `frames` stereo frames whose left samples count up from `first` (right = -left).
std::vector<f32> ramp(u32 first, u32 frames)
{
    std::vector<f32> v(2 * frames);
    for (u32 i = 0; i < frames; i++) {
        v[2 * i] = static_cast<f32>(first + i);
        v[2 * i + 1] = -static_cast<f32>(first + i);
    }
    return v;
}

} // namespace

TEST_CASE("audio ring: prebuffering, order, underruns and overruns", "[core][audio]")
{
    AudioRing ring(64, 32); // 64 frames, plays from 32
    f32 l[64], r[64];

    // Prebuffering: silence, not an underrun.
    REQUIRE(ring.push(ramp(1, 16).data(), 16));
    CHECK(ring.pull(l, r, 8) == 0);
    CHECK(l[0] == 0.0f);
    CHECK(ring.underruns() == 0);
    REQUIRE(ring.push(ramp(17, 16).data(), 16));
    CHECK(ring.queued() == 32);

    // Playing: the frames in order, both channels.
    CHECK(ring.pull(l, r, 10) == 10);
    CHECK(l[0] == 1.0f);
    CHECK(r[0] == -1.0f);
    CHECK(l[9] == 10.0f);
    CHECK(ring.played() == 10);

    // Overrun: 22 queued + 48 > 64, dropped whole.
    CHECK_FALSE(ring.push(ramp(100, 48).data(), 48));
    CHECK(ring.overruns() == 1);
    CHECK(ring.queued() == 22);

    // Running dry: what is left, then silence, one underrun; then prebuffering again.
    CHECK(ring.pull(l, r, 30) == 22);
    CHECK(l[21] == 32.0f);
    CHECK(l[22] == 0.0f);
    CHECK(r[29] == 0.0f);
    CHECK(ring.underruns() == 1);
    REQUIRE(ring.push(ramp(33, 16).data(), 16));
    CHECK(ring.pull(l, r, 4) == 0); // 16 < 32 queued: still prebuffering
    REQUIRE(ring.push(ramp(49, 16).data(), 16));
    CHECK(ring.pull(l, r, 4) == 4);
    CHECK(l[0] == 33.0f);
    CHECK(ring.pull(l, r, 28) == 28); // frames 37..64: the ring is empty but nothing ran dry
    CHECK(l[27] == 64.0f);
    REQUIRE(ring.push(ramp(65, 32).data(), 32)); // positions 64..95: wraps around the 64-frame ring
    CHECK(ring.pull(l, r, 4) == 4);
    CHECK(l[0] == 65.0f);
    CHECK(ring.underruns() == 1);
    CHECK(ring.largest_pull() == 30); // the device's callback length, for cellAudio's target (D-074)

    // Capacity rounds up to a power of two.
    AudioRing odd(100, 0);
    CHECK(odd.push(ramp(0, 128).data(), 128));
    CHECK_FALSE(odd.push(ramp(0, 1).data(), 1));
}

TEST_CASE("audio ring: a producer and a consumer on two threads keep the stream in order", "[core][audio]")
{
    constexpr u32 kBlocks = 4000, kBlock = 256;
    AudioRing ring(4096, 512);
    std::atomic<bool> done{false};
    bool pushed = true; // read after the join (Catch2 asserts only on the test's thread)
    std::thread producer([&] {
        for (u32 b = 0; b < kBlocks; b++) {
            const std::vector<f32> block = ramp(1 + b * kBlock, kBlock);
            while (ring.queued() + kBlock > 4096) std::this_thread::yield(); // wait for room: no overruns
            pushed &= ring.push(block.data(), kBlock);
        }
        done = true;
    });
    u64 expected = 1, got = 0;
    bool ordered = true;
    std::vector<f32> l(300), r(300);
    for (u32 size = 1; got < u64{kBlocks} * kBlock; size = (size + 37) % 300 + 1) { // 1..300 frames
        const u32 n = ring.pull(l.data(), r.data(), size);
        for (u32 i = 0; i < n; i++) {
            ordered &= l[i] == static_cast<f32>(expected) && r[i] == -static_cast<f32>(expected);
            expected++;
        }
        got += n;
        if (n == 0 && done && ring.queued() == 0) break;
        if (n == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(pushed);
    CHECK(ordered);
    CHECK(got == u64{kBlocks} * kBlock);
    CHECK(ring.overruns() == 0);
}
