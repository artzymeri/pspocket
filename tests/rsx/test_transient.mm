// The Metal backend's transient allocator (metal_internal.hpp, D-1047): what a command buffer used comes back when it
// completes, and a frame after frame of large uploads keeps the GPU's memory bounded.

#include <catch_amalgamated.hpp>

#import <Metal/Metal.h>

#include "../../runtime/rsx/metal/src/metal_internal.hpp"

using namespace pspocket;
using namespace pspocket::rsx::metal;

TEST_CASE("metal transient: a large upload's own buffer of a chunk's length is released, not kept as a chunk (D-1047)")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) SKIP("no Metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    TransientAllocator t;
    t.init(device);
    // Assassin's Creed's garden: each frame carries 4 MB targets (D-1025), whose staging copies are buffers of their
    // own of exactly a chunk's length; they went back to the free list as chunks, and the next frame made new ones
    // (four a frame, the run's footprint 56 GB when it died).
    // A frame in an autorelease pool, as the backend's draw, clear and flip run.
    const auto frame = [&](u32 large) {
        @autoreleasepool {
        for (u32 i = 0; i < 4; i++) {
            const TransientAllocator::Allocation a = t.alloc(large);
            REQUIRE(a.buffer != nil);
            a.data[0] = static_cast<u8>(i);
        }
        for (u32 i = 0; i < 6; i++) REQUIRE(t.alloc(600u << 10).buffer != nil); // small ones share chunks
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        t.retire(cb);
        [cb commit];
        [cb waitUntilCompleted];
        }
    };
    for (const u32 large : {4u << 20, (4u << 20) - 4096, 6u << 20}) {
        INFO("large uploads of " << large << " bytes");
        for (int f = 0; f < 10; f++) frame(large);
        const u64 warm = device.currentAllocatedSize;
        const usize free_warm = t.free_chunks();
        for (int f = 0; f < 200; f++) {
            frame(large);
            if (std::getenv("D1047_DEBUG") && f % 50 == 0)
                std::printf("frame %d: %llu MB, %zu free, %zu in use\n", f,
                    static_cast<unsigned long long>(device.currentAllocatedSize >> 20), t.free_chunks(), t.in_use());
        }
        CHECK(t.free_chunks() <= free_warm + 1); // the shared chunks only, reused
        CHECK(t.free_chunks() <= 4);
        CHECK(device.currentAllocatedSize < warm + (32u << 20));
    }
}

TEST_CASE("metal transient: a peak of chunks is given back past the free list's cap (D-1047)")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) SKIP("no Metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    TransientAllocator t;
    t.init(device);
    @autoreleasepool {
        for (int i = 0; i < 600; i++) REQUIRE(t.alloc(1u << 20).buffer != nil); // 150 chunks in one command buffer
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        t.retire(cb);
        [cb commit];
        [cb waitUntilCompleted];
    }
    for (int i = 0; i < 100 && t.free_chunks() == 0; i++) [NSThread sleepForTimeInterval:0.01];
    CHECK(t.free_chunks() == 128);
}
