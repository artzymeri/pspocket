// EDGE geometry on the GPU (D-988): the Metal kernel against the same code on the CPU (pspocket/core/edge_kernels.h),
// on synthetic meshes (tests/rsx/edge_synthetic.hpp): the output vertices, the kept indices in their order and the
// indirect draw's count, bit for bit (both round to nearest; the kernels make every product explicit, so neither
// compiler fuses or reorders). Skipped without a Metal device.

#include <catch_amalgamated.hpp>

#import <Metal/Metal.h>

#include <cstring>
#include <vector>

#include "edge_synthetic.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/edge_kernels.h"
#include "pspocket/rsx/metal/metal_backend.hpp"

using namespace pspocket;
namespace et = pspocket::edge_test;

namespace {

struct Dispatch {
    uint32_t blob, shadow, index_bytes, scratch, args, pad0, pad1, pad2;
};

struct CpuResult {
    std::vector<uint8_t> vertices;
    std::vector<uint8_t> indices; // big-endian, kept, in order
};

CpuResult on_cpu(const std::vector<uint8_t>& blob)
{
    const auto& j = *reinterpret_cast<const ps_edge_job*>(blob.data());
    CpuResult r;
    r.vertices.assign(j.num_vertexes * j.out_stride, 0);
    std::vector<ek_screen> scr(j.num_vertexes);
    const std::vector<float> pal = ek_palette(blob.data());
    for (uint32_t v = 0; v < j.num_vertexes; v++) scr[v] = ek_vertex(blob.data(), &j, pal.data(), v, r.vertices.data() + v * j.out_stride);
    const uint8_t* idx = blob.data() + j.indices;
    for (uint32_t i = 0; i + 2 < j.num_indexes; i += 3) {
        const uint32_t a = ps_ld16(idx + 2 * i), b = ps_ld16(idx + 2 * i + 2), c = ps_ld16(idx + 2 * i + 4);
        if (a < j.num_vertexes && b < j.num_vertexes && c < j.num_vertexes && !ek_culled(&j, scr[a], scr[b], scr[c]))
            r.indices.insert(r.indices.end(), idx + 2 * i, idx + 2 * i + 6);
    }
    return r;
}

} // namespace

TEST_CASE("edge gpu: the Metal kernel makes what the CPU makes of synthetic meshes (D-988)")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) SKIP("no Metal device");
    NSError* error = nil;
    MTLCompileOptions* options = (__bridge_transfer MTLCompileOptions*)rsx::metal::edge_kernel_options();
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:rsx::metal::edge_kernel_source().c_str()]
                                              options:options
                                                error:&error];
    INFO((error ? error.localizedDescription.UTF8String : ""));
    REQUIRE(lib != nil);
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"edge_jobs"]
                                                                                 error:&error];
    REQUIRE(pipeline != nil);

    // Jobs of every kind the walk has: no skinning, four bones, one bone; positions alone, or with normals and
    // tangents; a mesh larger than a threadgroup (its compaction across rounds).
    std::vector<std::vector<uint8_t>> blobs;
    blobs.push_back(et::make_blob(et::grid(0, 0)));
    blobs.push_back(et::make_blob(et::grid(1, 2)));
    blobs.push_back(et::make_blob(et::grid(4, 2)));
    blobs.push_back(et::make_blob(et::grid(1, 0, 23)));
    {
        et::Mesh m = et::grid(4, 2, 9);
        m.cull_mode = 0; // every triangle kept
        blobs.push_back(et::make_blob(m));
    }
    // One buffer: each job's blob, shadow (its indices' room, then its vertices), scratch and arguments.
    std::vector<Dispatch> list;
    uint32_t at = 0;
    const auto align = [](uint32_t v) { return (v + 15u) & ~15u; };
    for (const auto& b : blobs) {
        const auto& j = *reinterpret_cast<const ps_edge_job*>(b.data());
        Dispatch d{};
        d.blob = at;
        at = align(at + static_cast<uint32_t>(b.size()));
        d.shadow = at;
        d.index_bytes = align(2 * j.num_indexes);
        at = align(at + d.index_bytes + j.num_vertexes * j.out_stride);
        d.scratch = at;
        at = align(at + 16 * j.num_vertexes);
        d.args = at;
        at += 32;
        list.push_back(d);
    }
    id<MTLBuffer> buf = [device newBufferWithLength:at options:MTLResourceStorageModeShared];
    std::memset(buf.contents, 0xCD, at);
    for (size_t k = 0; k < blobs.size(); k++) std::memcpy(static_cast<uint8_t*>(buf.contents) + list[k].blob, blobs[k].data(), blobs[k].size());
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pipeline];
    [enc setBuffer:buf offset:0 atIndex:0];
    [enc setBytes:list.data() length:list.size() * sizeof(Dispatch) atIndex:1];
    const NSUInteger threads = std::min<NSUInteger>(256, pipeline.maxTotalThreadsPerThreadgroup);
    [enc dispatchThreadgroups:MTLSizeMake(list.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    REQUIRE(cb.status == MTLCommandBufferStatusCompleted);

    const uint8_t* out = static_cast<const uint8_t*>(buf.contents);
    for (size_t k = 0; k < blobs.size(); k++) {
        INFO("job " << k);
        const auto& j = *reinterpret_cast<const ps_edge_job*>(blobs[k].data());
        const CpuResult want = on_cpu(blobs[k]);
        const Dispatch& d = list[k];
        uint32_t args[5];
        std::memcpy(args, out + d.args, sizeof args);
        CHECK(args[0] == want.indices.size() / 2);
        CHECK(args[1] == 1);
        CHECK(std::memcmp(out + d.shadow, want.indices.data(), want.indices.size()) == 0);
        CHECK(std::memcmp(out + d.shadow + d.index_bytes, want.vertices.data(), want.vertices.size()) == 0);
        // something culled where culling is on, nothing where it is off
        if (j.cull_mode) CHECK(want.indices.size() / 2 < j.num_indexes);
        else CHECK(want.indices.size() / 2 == j.num_indexes - j.num_indexes % 3);
    }
}
