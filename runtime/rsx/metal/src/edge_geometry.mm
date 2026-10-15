// EDGE geometry on the GPU, the Metal side (D-988; pspocket/core/edge_registry.hpp).
//
// A draw whose index array starts at a ring allocation an SPU job published a work item for takes it here: its blob
// (the job's inputs and parameters, pspocket/core/edge_job.h) goes into the draw's transient allocation next to the
// allocation's stand-in (the "shadow": indices at its start, vertices after them, at the offsets the job's command
// buffer hole gives the RSX), a scratch for the screen-space vertices and the indirect draw's arguments. At submit the
// jobs of the command buffer being recorded run in one compute dispatch per transient buffer, one threadgroup a job
// (pspocket/core/edge_kernels.inc), in a command buffer committed before it: no render pass is broken, and the draws
// read what the kernels wrote. The draw itself is indirect, its count the kept indices'.

#include "metal_internal.hpp"

#include "pspocket/core/edge_job.h"
#include "pspocket/core/edge_registry.hpp"

namespace pspocket::rsx::metal {
namespace {

#define PS_EDGE_STR2(...) #__VA_ARGS__
#define PS_EDGE_STR(...) PS_EDGE_STR2(__VA_ARGS__)

// The job's declarations and the kernels' code as text (stringified: no generated file, D-014).
const char* const kEdgeDecls = PS_EDGE_STR(PS_EDGE_JOB_DECLS(uint, int));
#define EK_CODE(...) #__VA_ARGS__
const char* const kEdgeCode =
#include "pspocket/core/edge_kernels.inc"
    ;
#undef EK_CODE

// The Metal meanings of the kernels' words, and the kernel: one threadgroup a job.
constexpr const char* kEdgePrefix = R"MSL(#include <metal_stdlib>
using namespace metal;
#define EK_DEV device
#define EK_IN device const
#define EK_THR thread
#define EK_TABLE constant
#define EK_INLINE static inline
typedef uint ek_u32;
typedef int ek_s32;
typedef uchar ek_u8;
#define ek_as_u32(f) as_type<uint>(f)
#define ek_as_f32(u) as_type<float>(u)
#define ek_fma(a, b, c) fma(a, b, c)
#define ek_mul(a, b) fma(a, b, -0.0f)
#define ek_min(a, b) min(a, b)
)MSL";

constexpr const char* kEdgeKernel = R"MSL(
// One job: its blob, the shadow of its ring allocation (indices at 0, vertices at index_bytes), the screen-space
// vertices' scratch and the indirect draw's arguments, as offsets into the buffer.
struct ek_dispatch {
    uint blob, shadow, index_bytes, scratch, args, pad0, pad1, pad2;
};

kernel void edge_jobs(device uchar* buf [[buffer(0)]], constant ek_dispatch* jobs [[buffer(1)]],
    uint tg [[threadgroup_position_in_grid]], uint tid [[thread_position_in_threadgroup]],
    uint tcount [[threads_per_threadgroup]], uint lane [[thread_index_in_simdgroup]],
    uint sg [[simdgroup_index_in_threadgroup]], uint sgs [[simdgroups_per_threadgroup]])
{
    const ek_dispatch d = jobs[tg];
    device const uchar* blob = buf + d.blob;
    device const ps_edge_job* j = (device const ps_edge_job*)blob;
    device uchar* shadow = buf + d.shadow;
    device ek_screen* scr = (device ek_screen*)(buf + d.scratch);
    const uint nv = j->num_vertexes;
    for (uint v = tid; v < nv; v += tcount) scr[v] = ek_vertex(blob, j, v, shadow + d.index_bytes + v * j->out_stride);
    threadgroup_barrier(mem_flags::mem_device);

    // The triangles, culled; the kept ones' indices packed in their order (EDGE's), big-endian.
    threadgroup uint sums[32];
    threadgroup uint total;
    uint base = 0;
    const uint ntri = j->num_indexes / 3;
    device const uchar* idx = blob + j->indices;
    for (uint t0 = 0; t0 < ntri; t0 += tcount) {
        const uint t = t0 + tid;
        uint a = 0, b = 0, c = 0;
        bool keep = false;
        if (t < ntri) {
            a = (uint(idx[6 * t]) << 8) | idx[6 * t + 1];
            b = (uint(idx[6 * t + 2]) << 8) | idx[6 * t + 3];
            c = (uint(idx[6 * t + 4]) << 8) | idx[6 * t + 5];
            keep = a < nv && b < nv && c < nv && !ek_culled(j, scr[a], scr[b], scr[c]);
        }
        const uint k = keep ? 1u : 0u;
        const uint before = simd_prefix_exclusive_sum(k);
        const uint in_simd = simd_sum(k);
        if (lane == 0) sums[sg] = in_simd;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (sg == 0) {
            const uint s = lane < sgs ? sums[lane] : 0u;
            const uint p = simd_prefix_exclusive_sum(s);
            if (lane < sgs) sums[lane] = p;
            if (lane == sgs - 1) total = p + s;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (keep) {
            const uint at = 6 * (base + sums[sg] + before);
            shadow[at] = uchar(a >> 8);
            shadow[at + 1] = uchar(a);
            shadow[at + 2] = uchar(b >> 8);
            shadow[at + 3] = uchar(b);
            shadow[at + 4] = uchar(c >> 8);
            shadow[at + 5] = uchar(c);
        }
        base += total;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) {
        device uint* args = (device uint*)(buf + d.args);
        args[0] = 3 * base; // MTLDrawIndexedPrimitivesIndirectArguments: index count, instances, first index, base vertex, base instance
        args[1] = 1;
        args[2] = 0;
        args[3] = 0;
        args[4] = 0;
    }
}
)MSL";

struct Dispatch {
    u32 blob, shadow, index_bytes, scratch, args, pad0, pad1, pad2;
};
static_assert(sizeof(Dispatch) == 32);

} // namespace

bool MetalBackend::Impl::edge_init()
{
    if (edge::mode() == edge::Mode::off) return true;
    const std::string source = std::string(kEdgePrefix) + kEdgeDecls + "\n" + kEdgeCode + kEdgeKernel;
    MTLCompileOptions* options = [MTLCompileOptions new];
    // IEEE arithmetic: the kernels' order of operations is the SPU's (no reassociation, no approximations).
    if (@available(macOS 15.0, iOS 18.0, *)) {
        options.mathMode = MTLMathModeSafe;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if (!lib) {
        PS_LOG_ERROR(kChan, "EDGE kernels failed to compile: {}", error.localizedDescription.UTF8String);
        return false;
    }
    edge_pipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"edge_jobs"] error:&error];
    if (!edge_pipeline) {
        PS_LOG_ERROR(kChan, "EDGE kernel pipeline failed: {}", error ? error.localizedDescription.UTF8String : "?");
        return false;
    }
    edge_threads = std::min<u32>(128, static_cast<u32>(edge_pipeline.maxTotalThreadsPerThreadgroup));
    PS_LOG_INFO(kChan, "EDGE geometry kernels ready ({} threads a job, {} bytes of source)", edge_threads, source.size());
    edge::set_consumer(true); // from now on the SPU jobs may leave their work to the GPU
    return true;
}

// The draw's work item, when its index array starts at an allocation one was published for (do_draw, indexed).
std::unique_ptr<edge::WorkItem> MetalBackend::Impl::edge_take(u32 index_ea)
{
    if (!edge_pipeline || !edge::any_waiting()) return nullptr;
    return edge::take(index_ea);
}

// Room in the draw's transient allocation after `at` for the item: its blob, the shadow, the scratch, the arguments;
// returns the end. Offsets relative to the allocation's start.
u32 MetalBackend::Impl::edge_layout(const edge::WorkItem& item, u32 at, EdgeDraw& out)
{
    const auto& j = *reinterpret_cast<const ps_edge_job*>(item.blob.data());
    const auto align = [](u32 v) { return (v + 15u) & ~15u; };
    out.shadow = align(at);
    out.index_bytes = item.index_bytes;
    at = out.shadow + align(std::max(item.alloc_size, item.index_bytes + j.num_vertexes * j.out_stride));
    out.blob = align(at);
    at = out.blob + align(static_cast<u32>(item.blob.size()));
    out.scratch = at;
    at += 16 * j.num_vertexes;
    out.args = at;
    return at + 32;
}

// The item's blob into the allocation, and its job queued for the command buffer's dispatch.
void MetalBackend::Impl::edge_queue(const edge::WorkItem& item, const TransientAllocator::Allocation& a, const EdgeDraw& e)
{
    std::memcpy(a.data + e.blob, item.blob.data(), item.blob.size());
    // Until the kernel runs, the arguments draw nothing (a dispatch that never ran must not draw garbage).
    std::memset(a.data + e.args, 0, 20);
    const u32 base = a.offset;
    edge_jobs.push_back({a.buffer, {base + e.blob, base + e.shadow, e.index_bytes, base + e.scratch, base + e.args}});
    stats_edge.jobs++;
    stats_edge.blob_bytes += item.blob.size();
}

// The queued jobs, one dispatch per buffer, into a command buffer committed now: before the render buffer (submit).
void MetalBackend::Impl::edge_flush()
{
    if (edge_jobs.empty()) return;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    cb.label = @"rsx edge";
    stats.command_buffers++;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:edge_pipeline];
    std::sort(edge_jobs.begin(), edge_jobs.end(), [](const EdgeJob& x, const EdgeJob& y) {
        return (__bridge void*)x.buffer < (__bridge void*)y.buffer;
    });
    std::vector<Dispatch> list;
    for (usize i = 0; i < edge_jobs.size();) {
        id<MTLBuffer> buffer = edge_jobs[i].buffer;
        list.clear();
        for (; i < edge_jobs.size() && edge_jobs[i].buffer == buffer; i++) {
            const EdgeJob& job = edge_jobs[i];
            list.push_back({job.d[0], job.d[1], job.d[2], job.d[3], job.d[4], 0, 0, 0});
        }
        [enc setBuffer:buffer offset:0 atIndex:0];
        const usize bytes = list.size() * sizeof(Dispatch);
        if (bytes <= 4096) {
            [enc setBytes:list.data() length:bytes atIndex:1];
        } else {
            TransientAllocator::Allocation l = transient.alloc(static_cast<u32>(bytes), 256);
            std::memcpy(l.data, list.data(), bytes);
            [enc setBuffer:l.buffer offset:l.offset atIndex:1];
        }
        [enc dispatchThreadgroups:MTLSizeMake(list.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(edge_threads, 1, 1)];
        stats_edge.dispatches++;
    }
    [enc endEncoding];
    edge_jobs.clear();
    auto gpu = gpu_ns;
    auto edge_ns = edge_gpu_ns;
    auto ab = ab_period ? ab_ns : nullptr;
    const usize half = submit_half >= 0 ? static_cast<usize>(submit_half) : ab_half();
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        const double s = done.GPUEndTime - done.GPUStartTime;
        if (s <= 0) return;
        const u64 ns = static_cast<u64>(s * 1e9);
        gpu->fetch_add(ns, std::memory_order_relaxed);
        edge_ns->fetch_add(ns, std::memory_order_relaxed);
        if (ab) (*ab)[half].fetch_add(ns, std::memory_order_relaxed);
    }];
    [cb commit];
}

} // namespace pspocket::rsx::metal
