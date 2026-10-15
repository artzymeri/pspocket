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
#define EK_PAL threadgroup const
#define EK_TABLE constant
#define EK_INLINE static inline
typedef uint ek_u32;
typedef int ek_s32;
typedef uchar ek_u8;
#define ek_as_u32(f) as_type<uint>(f)
#define ek_as_f32(u) as_type<float>(u)
#define ek_hfma(a, b, c) fma(a, b, c)
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
    // the bones' matrices once, as floats (at most 256 bones)
    threadgroup float pal[256 * 12];
    const uint words = 12 * min(j->matrix_count, 256u);
    for (uint i = tid; i < words; i += tcount) pal[i] = ek_palette_word(blob, j, i);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint v = tid; v < nv; v += tcount) scr[v] = ek_vertex(blob, j, pal, v, shadow + d.index_bytes + v * j->out_stride);
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

std::string edge_kernel_source() { return std::string(kEdgePrefix) + kEdgeDecls + "\n" + kEdgeCode + kEdgeKernel; }

void* edge_kernel_options()
{
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
    return (__bridge_retained void*)options;
}

EdgeKernelRun edge_run_kernel(const std::vector<std::vector<uint8_t>>& blobs, uint32_t threads, int repeat)
{
    EdgeKernelRun run;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        run.error = "no Metal device";
        return run;
    }
    NSError* error = nil;
    MTLCompileOptions* options = (__bridge_transfer MTLCompileOptions*)edge_kernel_options();
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:edge_kernel_source().c_str()]
                                              options:options
                                                error:&error];
    id<MTLComputePipelineState> pipeline =
        lib ? [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"edge_jobs"] error:&error] : nil;
    if (!pipeline) {
        run.error = error ? error.localizedDescription.UTF8String : "the kernel did not compile";
        return run;
    }
    std::vector<Dispatch> list;
    u32 at = 0;
    const auto align = [](u32 v) { return (v + 15u) & ~15u; };
    for (const auto& b : blobs) {
        const auto& j = *reinterpret_cast<const ps_edge_job*>(b.data());
        Dispatch d{};
        d.blob = at;
        at = align(at + static_cast<u32>(b.size()));
        d.shadow = at;
        d.index_bytes = align(2 * j.num_indexes);
        at = align(at + d.index_bytes + j.num_vertexes * j.out_stride);
        d.scratch = at;
        at = align(at + 16 * j.num_vertexes);
        d.args = at;
        at += 32;
        list.push_back(d);
    }
    id<MTLBuffer> buf = [device newBufferWithLength:std::max<u32>(at, 16) options:MTLResourceStorageModeShared];
    id<MTLBuffer> jobs = [device newBufferWithBytes:list.data() length:std::max<size_t>(list.size() * sizeof(Dispatch), 16)
                                            options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    run.gpu_ms = 1e30;
    for (int r = 0; r < std::max(repeat, 1); r++) {
        for (size_t k = 0; k < blobs.size(); k++)
            std::memcpy(static_cast<u8*>(buf.contents) + list[k].blob, blobs[k].data(), blobs[k].size());
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc setBuffer:jobs offset:0 atIndex:1];
        const NSUInteger t = std::min<NSUInteger>(threads, pipeline.maxTotalThreadsPerThreadgroup);
        [enc dispatchThreadgroups:MTLSizeMake(list.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(t, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        run.gpu_ms = std::min(run.gpu_ms, (cb.GPUEndTime - cb.GPUStartTime) * 1e3);
    }
    const u8* out = static_cast<const u8*>(buf.contents);
    for (size_t k = 0; k < blobs.size(); k++) {
        const auto& j = *reinterpret_cast<const ps_edge_job*>(blobs[k].data());
        u32 count = 0;
        std::memcpy(&count, out + list[k].args, 4);
        run.counts.push_back(count);
        run.indices.emplace_back(out + list[k].shadow, out + list[k].shadow + 2 * std::min(count, j.num_indexes));
        const u8* v = out + list[k].shadow + list[k].index_bytes;
        run.vertices.emplace_back(v, v + j.num_vertexes * j.out_stride);
    }
    return run;
}

bool MetalBackend::Impl::edge_init()
{
    if (edge::mode() == edge::Mode::off) return true;
    const std::string source = edge_kernel_source();
    MTLCompileOptions* options = (__bridge_transfer MTLCompileOptions*)edge_kernel_options();
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
    edge_threads = std::min<u32>(256, static_cast<u32>(edge_pipeline.maxTotalThreadsPerThreadgroup));
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

// verify: the item's job queued in an allocation of its own, the job's own output copied for the comparison.
void MetalBackend::Impl::edge_verify(std::unique_ptr<edge::WorkItem> item, u32 drawn)
{
    EdgeDraw e;
    const u32 size = edge_layout(*item, 0, e);
    const TransientAllocator::Allocation a = transient.alloc(size, 16);
    edge_queue(*item, a, e);
    EdgeCheck c;
    c.buffer = a.buffer;
    c.base = a.offset;
    c.e = e;
    c.drawn = drawn;
    c.guest_index_bytes = item->guest_index_bytes;
    c.guest.assign(mem.host(item->alloc_ea), mem.host(item->alloc_ea) + item->alloc_size);
    std::memcpy(&c.job, item->blob.data(), sizeof c.job);
    edge_checks.push_back(std::move(c));
    edge::recycle(std::move(item));
}

namespace {

u64 ulps_apart(float a, float b)
{
    s32 ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    if (ia < 0) ia = static_cast<s32>(0x80000000u - static_cast<u32>(ia));
    if (ib < 0) ib = static_cast<s32>(0x80000000u - static_cast<u32>(ib));
    const s64 d = static_cast<s64>(ia) - ib;
    return static_cast<u64>(d < 0 ? -d : d);
}

void atomic_max(std::atomic<u64>& a, u64 v)
{
    u64 cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

} // namespace

// One job's GPU output against what its SPU put (verify).
void MetalBackend::Impl::edge_check(const EdgeCheck& c, EdgeVerify& v)
{
    const u8* gpu = static_cast<const u8*>(c.buffer.contents) + c.base;
    const ps_edge_job& j = c.job;
    u32 args[5];
    std::memcpy(args, gpu + c.e.args, sizeof args);
    v.jobs.fetch_add(1, std::memory_order_relaxed);
    v.kept_gpu.fetch_add(args[0] / 3, std::memory_order_relaxed);
    v.kept_spu.fetch_add(c.drawn / 3, std::memory_order_relaxed);
    if (args[0] == c.drawn) {
        v.counts_same.fetch_add(1, std::memory_order_relaxed);
        if (2 * c.drawn <= c.guest.size() && std::memcmp(gpu + c.e.shadow, c.guest.data(), 2 * c.drawn) == 0)
            v.lists_same.fetch_add(1, std::memory_order_relaxed);
    }
    const u8* gv = gpu + c.e.shadow + c.e.index_bytes;
    if (c.guest_index_bytes + j.num_vertexes * j.out_stride > c.guest.size()) return;
    const u8* sv = c.guest.data() + c.guest_index_bytes;
    u64 same = 0, worst_ulps = 0, worst_rel = 0, worst_int = 0;
    for (u32 n = 0; n < j.num_vertexes; n++) {
        const u8* a = gv + n * j.out_stride;
        const u8* b = sv + n * j.out_stride;
        if (std::memcmp(a, b, j.out_stride) == 0) {
            same++;
            continue;
        }
        for (u32 k = 0; k < j.out_count; k++) {
            const ps_edge_out_attr& o = j.out[k];
            if (o.format == PS_EDGE_FMT_F32) {
                for (u32 i = 0; i < o.components; i++) {
                    float x, y;
                    const u32 ux = ps_ld32(a + o.offset + 4 * i), uy = ps_ld32(b + o.offset + 4 * i);
                    std::memcpy(&x, &ux, 4);
                    std::memcpy(&y, &uy, 4);
                    if (std::fabs(y) >= 0x1p-10f) worst_ulps = std::max(worst_ulps, ulps_apart(x, y)); // not near zero
                    const double mag = std::max(std::fabs(static_cast<double>(y)), 1.0);
                    worst_rel = std::max(worst_rel, static_cast<u64>(std::fabs(static_cast<double>(x) - static_cast<double>(y)) / mag * 1e9));
                }
            } else if (o.format == PS_EDGE_FMT_X11Y11Z10N) {
                const u32 x = ps_ld32(a + o.offset), y = ps_ld32(b + o.offset);
                const u32 bits[3] = {11, 11, 10};
                for (u32 i = 0, sh = 0; i < 3; sh += bits[i], i++) { // the steps, as unsigned (the top bit flipped)
                    const u32 top = 1u << (bits[i] - 1);
                    const s64 dx = ((x >> sh) & ((1u << bits[i]) - 1)) ^ top, dy = ((y >> sh) & ((1u << bits[i]) - 1)) ^ top;
                    worst_int = std::max<u64>(worst_int, static_cast<u64>(dx > dy ? dx - dy : dy - dx));
                }
            } else if (o.format == PS_EDGE_FMT_I16N) {
                for (u32 i = 0; i < o.components; i++) {
                    const s64 dx = ps_ld16(a + o.offset + 2 * i) ^ 0x8000u, dy = ps_ld16(b + o.offset + 2 * i) ^ 0x8000u;
                    worst_int = std::max<u64>(worst_int, static_cast<u64>(dx > dy ? dx - dy : dy - dx));
                }
            }
        }
    }
    v.vertices.fetch_add(j.num_vertexes, std::memory_order_relaxed);
    v.vertices_same.fetch_add(same, std::memory_order_relaxed);
    atomic_max(v.f32_ulps, worst_ulps);
    atomic_max(v.f32_rel_e9, worst_rel);
    atomic_max(v.int_diff, worst_int);
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
    auto edge_ab = edge_ab_period ? edge_ab_gpu_ns : nullptr;
    const usize half = submit_half >= 0 ? static_cast<usize>(submit_half) : ab_half();
    const usize edge_half = edge_ab_half;
    if (!edge_checks.empty()) {
        auto checks = std::make_shared<std::vector<EdgeCheck>>(std::move(edge_checks));
        edge_checks.clear();
        auto verify = edge_verify_stats;
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
            for (const EdgeCheck& c : *checks) edge_check(c, *verify);
        }];
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        const double s = done.GPUEndTime - done.GPUStartTime;
        if (s <= 0) return;
        const u64 ns = static_cast<u64>(s * 1e9);
        gpu->fetch_add(ns, std::memory_order_relaxed);
        edge_ns->fetch_add(ns, std::memory_order_relaxed);
        if (ab) (*ab)[half].fetch_add(ns, std::memory_order_relaxed);
        if (edge_ab) (*edge_ab)[edge_half].fetch_add(ns, std::memory_order_relaxed);
    }];
    [cb commit];
}

// A flip with PSPOCKET_EDGE_GPU=ab: the frame that ends here counted in its half, the next one's half set for the jobs.
void MetalBackend::Impl::edge_ab_flip()
{
    const usize h = edge_ab_half;
    edge_ab_frames[h]++;
    const u64 cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    if (edge_ab_cpu_last) edge_ab_cpu_ns[h] += cpu - edge_ab_cpu_last;
    edge_ab_cpu_last = cpu;
    const edge::Stats es = edge::stats();
    const u64 spu = es.ns_deferred + es.ns_spu, jobs = es.jobs_deferred + es.jobs_spu;
    edge_ab_spu_ns[h] += spu - edge_ab_spu_last;
    edge_ab_jobs[h] += jobs - edge_ab_jobs_last;
    edge_ab_draws[h] += stats.draws - edge_ab_draws_last;
    edge_ab_spu_last = spu;
    edge_ab_jobs_last = jobs;
    edge_ab_draws_last = stats.draws;
    edge_ab_half = (stats.flips / edge_ab_period) % 2;
    edge::set_ab_half(edge_ab_half == 1);
    if (stats.flips % (edge_ab_period * 20) == 0) {
        const auto per = [&](u64 v, usize k) {
            return edge_ab_frames[k] ? static_cast<double>(v) / static_cast<double>(edge_ab_frames[k]) : 0.0;
        };
        const double g0 = static_cast<double>((*edge_ab_gpu_ns)[0].exchange(0)), g1 = static_cast<double>((*edge_ab_gpu_ns)[1].exchange(0));
        PS_LOG_INFO(kChan, "EDGE A/B (PSPOCKET_EDGE_GPU=ab): SPUs ({} frames) GPU {:.3f} ms a frame, RSX thread {:.3f} ms, "
                           "EDGE SPU jobs {:.3f} ms ({:.0f} a frame), {:.0f} draws; GPU ({} frames) GPU {:.3f} ms, RSX "
                           "thread {:.3f} ms, EDGE SPU jobs {:.3f} ms ({:.0f} a frame), {:.0f} draws",
            edge_ab_frames[0], per(static_cast<u64>(g0), 0) / 1e6, per(edge_ab_cpu_ns[0], 0) / 1e6,
            per(edge_ab_spu_ns[0], 0) / 1e6, per(edge_ab_jobs[0], 0), per(edge_ab_draws[0], 0), edge_ab_frames[1],
            per(static_cast<u64>(g1), 1) / 1e6, per(edge_ab_cpu_ns[1], 1) / 1e6, per(edge_ab_spu_ns[1], 1) / 1e6,
            per(edge_ab_jobs[1], 1), per(edge_ab_draws[1], 1));
        edge_ab_frames = edge_ab_cpu_ns = edge_ab_spu_ns = edge_ab_jobs = edge_ab_draws = {};
    }
}

} // namespace pspocket::rsx::metal
