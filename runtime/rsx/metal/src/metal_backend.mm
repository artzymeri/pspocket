// Metal backend core: device setup, command buffers, render passes, draws, clears, flips, sync and memory hooks.
// The surface, texture and pipeline caches live in surface_cache.mm, texture_cache.mm and pipeline_cache.mm.

#include "metal_internal.hpp"

#include <cstdlib>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <span>
#include <thread>
#include <unordered_set>

#include "kernels.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/index_scan.hpp"

namespace pspocket::rsx::metal {

// A depth-format texture exactly over a depth buffer that the draw does not render to: its depth texture is sampled
// directly (depth2d<float>), as RPCS3 samples the depth image, without a conversion that would end the render pass
// (D-106). The draw's own depth buffer keeps the conversion's snapshot (D-105).
Surface* MetalBackend::Impl::depth_sampled_directly(const TextureInfo& t)
{
    const u32 bf = t.base_format();
    if (bf != tex::DEPTH16 && bf != tex::DEPTH24_D8 && bf != tex::DEPTH16_FLOAT && bf != tex::DEPTH24_D8_FLOAT) return nullptr;
    if (t.dimension != 2 || t.cubemap) return nullptr;
    const auto ea = rsx->translate(t.offset, t.location);
    if (!ea) return nullptr;
    Surface* d = find_surface(*ea, true);
    if (!d || !d->initialized || d->cpu_newer || d->swizzled) return nullptr;
    if (t.width != d->width || t.height != d->height || d == draw_targets.depth) return nullptr;
    // A clear of it that waits runs before a draw reads it: now, when the pass drawing would go on (D-945; else the
    // next pass's start runs it).
    if (pass_open && pass_started && clear_waits_on(d)) end_pass_for(kEndFlush);
    return d;
}

// RPCS3 RSXThread.cpp (shadow_textures): a depth format sampled with a compare function other than never and always,
// and no alpha kill, returns the depth comparison instead of the depth.
static bool is_shadow_texture(const TextureInfo& t)
{
    const u32 bf = t.base_format();
    const bool depth = bf == tex::DEPTH16 || bf == tex::DEPTH24_D8 || bf == tex::DEPTH16_FLOAT || bf == tex::DEPTH24_D8_FLOAT;
    const u32 func = t.zfunc & 7;
    return depth && func != 0 && func != 7 && !t.alpha_kill;
}

static_assert(static_cast<u32>(TexelDecode::ARGB8) == 1 && static_cast<u32>(TexelDecode::X16) == 14 &&
                  static_cast<u32>(TexelDecode::Y16X16) == 15 && static_cast<u32>(TexelDecode::DXT45) == 18 &&
                  static_cast<u32>(TexelDecode::D16) == 19 && static_cast<u32>(TexelDecode::D24) == 21 &&
                  static_cast<u32>(TexelDecode::D24F) == 22 && static_cast<u32>(TexelDecode::HILOS8) == 24,
    "TexelDecode values are mirrored in kernels.hpp");

// ---- transient allocator ------------------------------------------------------------------------------------------

TransientAllocator::Allocation TransientAllocator::alloc(u32 size, u32 align)
{
    size = std::max<u32>(size, 4);
    if (size > kChunkSize / 2) {
        // Large uploads get a buffer of their own (still retired with the command buffer).
        id<MTLBuffer> b = [device_ newBufferWithLength:size options:MTLResourceStorageModeShared];
        in_use_.push_back(b);
        bytes_in_use_ += size;
        return {b, 0, static_cast<u8*>(b.contents)};
    }
    u32 offset = (offset_ + align - 1) & ~(align - 1);
    if (!current_ || offset + size > kChunkSize) {
        {
            std::lock_guard lock(*free_mutex_);
            if (!free_->empty()) {
                current_ = free_->back();
                free_->pop_back();
            } else {
                current_ = nil;
            }
        }
        if (!current_) current_ = [device_ newBufferWithLength:kChunkSize options:MTLResourceStorageModeShared];
        in_use_.push_back(current_);
        bytes_in_use_ += kChunkSize;
        offset = 0;
    }
    offset_ = offset + size;
    return {current_, offset, static_cast<u8*>(current_.contents) + offset};
}

void TransientAllocator::retire(id<MTLCommandBuffer> cb)
{
    generation_++;
    bytes_in_use_ = 0;
    if (in_use_.empty()) return;
    auto chunks = std::make_shared<std::vector<id<MTLBuffer>>>(std::move(in_use_));
    in_use_.clear();
    current_ = nil;
    offset_ = 0;
    auto free_list = free_;
    auto mutex = free_mutex_;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        std::lock_guard lock(*mutex);
        for (id<MTLBuffer> b : *chunks) {
            if (b.length == kChunkSize) free_list->push_back(b);
        }
    }];
}

// ---- setup --------------------------------------------------------------------------------------------------------

bool MetalBackend::Impl::init()
{
    visibility_slots = std::max<u32>(1, config.occlusion_counters); // D-1024
    if (const char* v = std::getenv("PSPOCKET_RSX_NO_CARRY"); v && *v && std::strcmp(v, "0") != 0) carry_on = false; // D-1025
    if (const char* v = std::getenv("PSPOCKET_RSX_EXACT"); v && *v && std::strcmp(v, "0") != 0) config.exact = true;
    exact = config.exact;
    if (exact) { // D-1029
        config.merge_passes = false;
        carry_on = false;
        PS_LOG_INFO(kChan, "the exact mode (PSPOCKET_RSX_EXACT): targets in guest memory at every label, no merging, "
            "no feedback cache, no surface pool, synchronous copies");
    }
    device = config.device ? (__bridge id<MTLDevice>)config.device : MTLCreateSystemDefaultDevice();
    if (!device) {
        PS_LOG_ERROR(kChan, "no Metal device: the RSX backend drops all GPU work");
        return false;
    }
    queue = [device newCommandQueue];
    queue.label = @"pspocket.rsx";
    frames_in_flight_limit = std::max<u32>(config.frames_in_flight, 1);
    frames_in_flight = dispatch_semaphore_create(frames_in_flight_limit);
    bc_supported = false;
    if (@available(macOS 11.0, iOS 16.4, *)) bc_supported = device.supportsBCTextureCompression;
    transient.init(device);

    NSError* error = nil;
    kernels = [device newLibraryWithSource:[NSString stringWithUTF8String:kKernelSource] options:nil error:&error];
    if (!kernels) {
        PS_LOG_ERROR(kChan, "backend kernels failed to compile: {}", error.localizedDescription.UTF8String);
        return false;
    }
    decode_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_decode_texture"]
                                                            error:&error];
    read_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_read_texture"]
                                                          error:&error];
    depth_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_depth_to_texture"]
                                                           error:&error];
    color_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_color_to_texture"]
                                                           error:&error];
    decode3d_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_decode_texture3d"]
                                                              error:&error];
    color_memory_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_color_to_memory"]
                                                                  error:&error];
    float_memory_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_float_to_memory"]
                                                                  error:&error];
    depth_memory_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_depth_to_memory"]
                                                                  error:&error];
    memory_surface_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_memory_to_surface"]
                                                                    error:&error];
    thumbnail_pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"rsx_thumbnail"]
                                                               error:&error];
    if (!edge_init()) edge_pipeline = nil; // EDGE geometry stays on the SPUs (D-988)
    MTLRenderPipelineDescriptor* blit = [MTLRenderPipelineDescriptor new];
    blit.vertexFunction = [kernels newFunctionWithName:@"rsx_fullscreen_vs"];
    blit.fragmentFunction = [kernels newFunctionWithName:@"rsx_blit_fs"];
    blit.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    blit_pipeline = [device newRenderPipelineStateWithDescriptor:blit error:&error];
    if (!decode_pipeline || !read_pipeline || !depth_pipeline || !color_pipeline || !decode3d_pipeline || !blit_pipeline ||
        !color_memory_pipeline || !float_memory_pipeline || !depth_memory_pipeline || !memory_surface_pipeline ||
        !thumbnail_pipeline) {
        PS_LOG_ERROR(kChan, "backend pipelines failed: {}", error ? error.localizedDescription.UTF8String : "?");
        return false;
    }

    auto make_sampler = [&](MTLSamplerMinMagFilter f) {
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.minFilter = f;
        d.magFilter = f;
        d.sAddressMode = MTLSamplerAddressModeClampToEdge;
        d.tAddressMode = MTLSamplerAddressModeClampToEdge;
        return [device newSamplerStateWithDescriptor:d];
    };
    nearest_sampler = make_sampler(MTLSamplerMinMagFilterNearest);
    linear_sampler = make_sampler(MTLSamplerMinMagFilterLinear);

    // Unbound or failed texture units sample transparent black.
    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    dummy_texture = [device newTextureWithDescriptor:dd];
    const u32 zero = 0;
    [dummy_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
    MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    dummy_stencil = [device newTextureWithDescriptor:sd];
    MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     size:1
                                                                                mipmapped:NO];
    dummy_cube = [device newTextureWithDescriptor:cd];
    for (u32 face = 0; face < 6; face++) {
        [dummy_cube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:face withBytes:&zero bytesPerRow:4 bytesPerImage:4];
    }
    MTLTextureDescriptor* zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    zd.usage = MTLTextureUsageShaderRead;
    zd.storageMode = MTLStorageModePrivate;
    dummy_depth = [device newTextureWithDescriptor:zd]; // depth units without their buffer read 0
    MTLTextureDescriptor* vd = [MTLTextureDescriptor new];
    vd.textureType = MTLTextureType3D;
    vd.pixelFormat = MTLPixelFormatRGBA8Unorm;
    vd.width = vd.height = vd.depth = 1;
    dummy_volume = [device newTextureWithDescriptor:vd];
    [dummy_volume replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&zero bytesPerRow:4
                  bytesPerImage:4];

    frame_event = [device newSharedEvent];
    for (u32 i = 0; i < std::max<u32>(config.frames_in_flight, 1); i++) {
        MTLTextureDescriptor* od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:config.output_width
                                                                                     height:config.output_height
                                                                                  mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        od.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [device newTextureWithDescriptor:od];
        t.label = @"rsx output";
        outputs.push_back(t);
    }
    check_surfaces = std::getenv("PSPOCKET_RSX_CHECK_SURFACES") != nullptr;
    if (const char* f = std::getenv("PSPOCKET_SKIP_FP")) skip_fp = std::strtoull(f, nullptr, 16); // a measuring aid
    if (const char* f = std::getenv("PSPOCKET_GPU_AB")) ab_period = static_cast<u32>(std::max(1, std::atoi(f)));
    if (edge_pipeline && edge::mode() == edge::Mode::ab) {
        // EDGE's own halves (D-988), apart from PSPOCKET_GPU_AB's (whose ab_on() switches other experiments too).
        const char* f = std::getenv("PSPOCKET_EDGE_AB");
        edge_ab_period = static_cast<u32>(std::max(1, f ? std::atoi(f) : 30));
        PS_LOG_INFO(kChan, "EDGE A/B: on the GPU in every other {} frames", edge_ab_period);
    }
    if (const char* f = std::getenv("PSPOCKET_RSX_CPU_AB")) { // D-953: N[:MASK of FastPath items]
        ab_period = static_cast<u32>(std::max(1, std::atoi(f)));
        ab_cpu = true;
        if (const char* m = std::strchr(f, ':')) ab_items = static_cast<u32>(std::strtoul(m + 1, nullptr, 0));
        PS_LOG_INFO(kChan, "RSX CPU A/B: fast paths {:#x} off in every other {} frames", ab_items, ab_period);
    }
    if (std::getenv("PSPOCKET_GPU_PASS_TIMES")) {
        pass_times = std::make_shared<PassTimes>();
        if ([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            for (id<MTLCounterSet> set in device.counterSets)
                if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) pass_times->timestamps = set;
        }
        // GPU ticks in nanoseconds: two GPU timestamps 20 ms apart by the host clock.
        MTLTimestamp c0 = 0, g0 = 0, c1 = 0, g1 = 0;
        const auto t0 = std::chrono::steady_clock::now();
        [device sampleTimestamps:&c0 gpuTimestamp:&g0];
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        [device sampleTimestamps:&c1 gpuTimestamp:&g1];
        const double wall_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        if (g1 > g0) pass_times->gpu_tick_ns = wall_ns / static_cast<double>(g1 - g0);
        PS_LOG_INFO(kChan, "GPU pass times on: stage timestamps {}, a GPU tick {:.3f} ns",
            pass_times->timestamps ? "yes" : "no", pass_times->gpu_tick_ns);
    }
    if (std::getenv("PSPOCKET_RSX_WRITE_BACK_TARGETS")) config.overlay_targets = false; // D-137's comparison
    if (std::getenv("PSPOCKET_RSX_NO_PASS_MERGING")) config.merge_passes = false;       // D-945's comparison
    if (std::getenv("PSPOCKET_RSX_STRICT_FEEDBACK") || exact) feedback_cached = false;   // D-947's comparison, D-1029
    if (!config.shader_cache.empty() || !config.shader_seeds.empty()) {
        shader_cache = std::make_unique<ShaderCache>(device, config.shader_cache, config.shader_seeds,
            config.shader_cache_threads, config.shader_archives);
    }
    if (config.async_shaders) {
        async = std::make_unique<AsyncCompiler>(device, config.async_threads);
        compile_wait_left_ns = static_cast<s64>(config.compile_wait_ms * 1e6);
    }
    // The shader interpreter (D-940): its vertex function and the commonest fragment variant (one colour target, no
    // depth export) made ahead in the background; the others when a draw first needs them.
    force_interpreter = config.force_interpreter || std::getenv("PSPOCKET_RSX_INTERPRETER") != nullptr;
    if (interpreter_on()) {
        interpreter_program(true, 1, false);
        interpreter_program(false, 1, false);
    }
    PS_LOG_INFO(kChan, "Metal backend on {} (BC textures: {}), output {}x{}", device.name.UTF8String,
        bc_supported ? "yes" : "no", config.output_width, config.output_height);
    return true;
}

void MetalBackend::Impl::fail(std::string message)
{
    last_error = message;
    if (rsx) {
        rsx->fail(FailKind::Unsupported, 0, std::move(message));
    } else {
        PS_LOG_ERROR(kChan, "{}", message);
    }
}

void MetalBackend::Impl::unsupported(std::string message) { fail(std::move(message)); }

// ---- command buffers --------------------------------------------------------------------------------------------

id<MTLCommandBuffer> MetalBackend::Impl::render_buffer()
{
    // Anything recorded now that is not the next pass may read what the closing pass drew: it stores all of it first
    // (D-945; start_encoder ends it knowing more).
    if (closing.encoder) finish_closing(false);
    if (!render_cb) {
        render_cb = [queue commandBuffer];
        render_cb.label = @"rsx render";
        stats.command_buffers++;
        // Its occlusion counters (D-102), zeroed: a pass only writes the counters it counted into.
        visibility = [device newBufferWithLength:visibility_slots * 8 options:MTLResourceStorageModeShared];
        std::memset(visibility.contents, 0, visibility_slots * 8);
        visibility_next = 0;
        pass_zpass_slot = -1;
    }
    return render_cb;
}

id<MTLCommandBuffer> MetalBackend::Impl::upload_buffer()
{
    if (!upload_cb) {
        upload_cb = [queue commandBuffer];
        upload_cb.label = @"rsx upload";
        stats.command_buffers++;
    }
    return upload_cb;
}

void MetalBackend::Impl::submit(bool wait)
{
    end_pass_for(kEndSubmit);  // the clears waiting run too (D-945)
    finish_closing(false);     // a buffer commits with no encoder open
    edge_flush();              // the EDGE jobs its draws read, before it (D-988)
    // Uploads first: the render buffer samples what they decode (same queue, so in order).
    id<MTLCommandBuffer> last = nil;
    if (upload_cb) {
        last = upload_cb;
        if (!render_cb) transient.retire(upload_cb);
        [upload_cb commit];
        upload_cb = nil;
    }
    // Occlusion reports (D-102): written when the render buffer that counted the last of their draws completes. The
    // queue runs buffers in order, so the earlier buffers their other counters are in have completed too.
    if (!pending_reports.empty()) {
        auto reports = std::make_shared<std::vector<PendingReport>>(std::move(pending_reports));
        pending_reports.clear();
        u8* const guest = mem.base();
        auto in_flight = reports_in_flight;
        [render_buffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
            for (const PendingReport& r : *reports) {
                u64 count = 0;
                for (const VisibilitySlot& s : r.slots) count += static_cast<const u64*>(s.buffer.contents)[s.index];
                const u32 value = static_cast<u32>(std::min<u64>(count, 0xFFFFFFFFull));
                // The whole report (D-1021): {timestamp, count, pad} as RPCS3's get_report lays it out, the pad (the
                // ready word GTA V polls) last, released after the rest.
                if (r.whole) ps_write64(guest, r.value_addr - 8, Rsx::timestamp_ns());
                __atomic_store_n(reinterpret_cast<u32*>(guest + r.value_addr), ps_bswap32(value), __ATOMIC_RELEASE);
                if (r.whole) __atomic_store_n(reinterpret_cast<u32*>(guest + r.value_addr + 4), 0u, __ATOMIC_RELEASE);
                std::lock_guard lock(in_flight->mutex);
                if (auto it = in_flight->count.find(r.value_addr); it != in_flight->count.end() && --it->second == 0)
                    in_flight->count.erase(it);
            }
        }];
    }
    if (render_cb) {
        last = render_cb;
        transient.retire(render_cb);
        auto gpu = gpu_ns;
        auto ab = ab_period ? ab_ns : nullptr;
        const usize half = submit_half >= 0 ? static_cast<usize>(submit_half) : ab_half();
        auto edge_ab = edge_ab_period ? edge_ab_gpu_ns : nullptr;
        const usize edge_half = edge_ab_half;
        [render_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            const double s = cb.GPUEndTime - cb.GPUStartTime;
            if (s > 0) gpu->fetch_add(static_cast<u64>(s * 1e9), std::memory_order_relaxed);
            if (ab && s > 0) (*ab)[half].fetch_add(static_cast<u64>(s * 1e9), std::memory_order_relaxed);
            if (edge_ab && s > 0) (*edge_ab)[edge_half].fetch_add(static_cast<u64>(s * 1e9), std::memory_order_relaxed);
        }];
        [render_cb commit];
        render_cb = nil;
        visibility = nil;
    }
    if (last) last_committed = last;
    if (wait && last_committed) {
        [last_committed waitUntilCompleted];
        if (last_committed.status == MTLCommandBufferStatusError) {
            PS_LOG_ERROR(kChan, "command buffer failed: {}", last_committed.error.localizedDescription.UTF8String);
        }
    }
}

// ---- render passes ----------------------------------------------------------------------------------------------

bool MetalBackend::Impl::select_targets(const RsxState& state, PassTargets& out, u32 clear_mask)
{
    const bool for_clear = clear_mask != ~0u;
    // RPCS3 RSXThread.cpp get_framebuffer_layout: size from the surface clip, colour buffers for the active
    // targets, the depth buffer only when the draw (or clear) uses it, misconfigured pitches dropped.
    const SurfaceInfo si = state.surface();
    draw_surface = si; // the draw's, for do_draw (D-954)
    out = {};
    draw_targets = {};
    // Multisampled targets (D-250): the RSX keeps a pixel's samples side by side in memory, two across for AA modes 3 to
    // 5 and two down too for 4 and 5 (RPCS3 RSXThread.cpp aa_factor_u/v, surface_utils.h samples_x/y). Such a target is
    // drawn at that size, one texel per sample: its memory image, a texture over it (the game's own resolve reads it
    // at its sample layout) and a write-back need no conversion. Draws scale their scissor (do_draw, clear).
    const AaFactors aa = aa_factors(si.antialias);
    if (aa.unknown) {
        static std::unordered_set<u32> seen;
        if (seen.size() < 16 && seen.insert(si.antialias).second)
            unsupported(std::format("surface AA mode {} is not one the RSX defines: drawn with one sample", si.antialias));
    }
    u32 width = si.clip_width * aa.x, height = si.clip_height * aa.y;
    if (width == 0 || height == 0) return false;
    // NV4097_SET_WINDOW_OFFSET moves where the targets start in memory (D-265; RPCS3 RSXThread.cpp
    // get_framebuffer_layout: "Window offset is what affects the raster position", Turbo: Super Stunt Squad declares
    // its framebuffers that way).
    const u32 window_x = si.window_offset_x, window_y = si.window_offset_y;
    const bool swizzled = si.type == 2;

    const DepthStencilInfo ds = state.depth_stencil();
    bool depth_used = false;
    if (for_clear) {
        // Only a clear of depth or stencil: a colour clear must not set up (and so replace) whatever depth buffer the
        // stale zeta registers describe.
        depth_used = (clear_mask & clear::kDepthStencil) != 0;
    } else {
        depth_used = ds.depth_test || (si.depth_format == 2 && ds.stencil_test) ||
            (ds.depth_bounds_test && (ds.depth_bounds_min > 0.f || ds.depth_bounds_max < 1.f));
    }

    std::array<u32, kMaxColorTargets> idx{};
    const u32 n = si.active_targets(idx);
    const u32 bpp = surface_color_bpp(si.color_format);
    if (n && bpp == 0) {
        unsupported(std::format("invalid surface colour format {}", si.color_format));
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        const ColorBuffer& cb = si.color[idx[i]];
        u32 pitch = cb.pitch;
        if (!swizzled && pitch < bpp * width) {
            if (pitch <= 64) continue; // "could not fit colour buffer": dropped
            width = std::min(width, pitch / bpp);
        }
        const u32 offset = cb.offset + cb.pitch * window_y + bpp * window_x;
        const auto ea = rsx->translate(offset, cb.dma == loc::kDmaHostBuffer ? loc::kMain : loc::kLocal);
        if (!ea) {
            unsupported(std::format("colour surface {} at offset {:#x} is not mapped", idx[i], offset));
            continue;
        }
        before_access(*ea, swizzled ? bpp * width * height : pitch * height, false); // a deferred copy there lands first
        // A swizzled target's pitch is its row of texels (RPCS3 get_framebuffer_layout): what it covers is its size.
        Surface* s = get_color_surface(*ea, swizzled ? bpp * width : pitch, width, height, si.color_format, swizzled);
        if (!s) continue;
        out.rsx_index[out.color_count] = idx[i];
        out.color[out.color_count++] = s;
        draw_targets = out; // later attachments of this draw must not evict earlier ones
    }
    if (depth_used && (si.depth_format == 1 || si.depth_format == 2)) {
        const u32 dbpp = si.depth_format == 1 ? 2 : 4;
        if (swizzled || si.depth.pitch >= dbpp * width || si.depth.pitch > 64) {
            const u32 offset = si.depth.offset + si.depth.pitch * window_y + dbpp * window_x;
            const auto ea = rsx->translate(offset, si.depth.dma == loc::kDmaHostBuffer ? loc::kMain : loc::kLocal);
            if (ea) before_access(*ea, swizzled ? dbpp * width * height : si.depth.pitch * height, false);
            if (ea) out.depth = get_depth_surface(*ea, si.depth.pitch, width, height, si.depth_format, si.depth_float);
        }
    }
    draw_targets = out;
    // The pass draws in (width, height), the top-left corner of its surfaces: they were made at that size, or are a
    // larger target at the same place (D-120).
    out.width = width;
    out.height = height;
    return out.color_count > 0 || out.depth != nullptr;
}

bool MetalBackend::Impl::newer_in_guest(const PassTargets& t) const
{
    bool newer = t.depth && t.depth->cpu_newer;
    for (u32 i = 0; i < t.color_count; i++) newer |= t.color[i]->cpu_newer;
    return newer;
}

void MetalBackend::Impl::open_pass(const PassTargets& t)
{
    // The same targets: the pass goes on, unless guest memory under one of them is newer (the CPU or the RSX wrote it
    // since): then it ends, and the new one reloads it (D-110).
    if (pass_open && pass == t && !newer_in_guest(t)) return;
    end_pass_for(kEndTargets);
    // A target the CPU wrote is reloaded below: the clears still waiting were made before that write, so they run
    // first (D-945).
    if (newer_in_guest(t)) flush_pending_clears();
    pass = t;
    pass_open = true;
    pass_started = false;
    // Its store actions are set when it ends, once what the next pass does to its attachments is known (D-945).
    pass_store_unknown = merging();
    const MTLStoreAction store = pass_store_unknown ? MTLStoreActionUnknown : MTLStoreActionStore;
    pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    pass_width = t.width ? t.width : ~0u;
    pass_height = t.height ? t.height : ~0u;
    pass_partial = false;
    for (u32 i = 0; i < t.color_count; i++) {
        Surface* s = t.color[i];
        if (s->cpu_newer) upload_surface_from_guest(*s);
        MTLRenderPassColorAttachmentDescriptor* a = pass_desc.colorAttachments[i];
        a.texture = s->texture;
        // Load what is there; a surface nobody wrote yet starts cleared (deterministic) and is stored: render targets
        // stay on the GPU (plan section 7), unless the next pass clears them whole (D-945).
        a.loadAction = s->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
        a.clearColor = MTLClearColorMake(0, 0, 0, 0);
        a.storeAction = store;
        pass_width = std::min(pass_width, s->width);
        pass_height = std::min(pass_height, s->height);
        pass_partial |= s->width != t.width || s->height != t.height;
    }
    if (Surface* d = t.depth) {
        if (d->cpu_newer) upload_surface_from_guest(*d); // D-110
        pass_desc.depthAttachment.texture = d->texture;
        pass_desc.depthAttachment.loadAction = d->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
        pass_desc.depthAttachment.clearDepth = 1.0;
        pass_desc.depthAttachment.storeAction = store;
        if (d->pixel_format == MTLPixelFormatDepth32Float_Stencil8) {
            pass_desc.stencilAttachment.texture = d->texture;
            pass_desc.stencilAttachment.loadAction = d->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
            pass_desc.stencilAttachment.clearStencil = 0;
            pass_desc.stencilAttachment.storeAction = store;
        }
        pass_width = std::min(pass_width, d->width);
        pass_height = std::min(pass_height, d->height);
        pass_partial |= d->width != t.width || d->height != t.height;
    }
    pass_desc.renderTargetWidth = pass_width;
    pass_desc.renderTargetHeight = pass_height;
    fold_pending_clears(); // D-945
}

id<MTLRenderCommandEncoder> MetalBackend::Impl::start_encoder()
{
    if (!pass_open) return nil;
    if (!pass_started) {
        settle_before_pass(); // the pass before ends knowing what this one clears; the clears waiting run (D-945)
        if (pass_times && !pass_times_submitting) {
            // What was recorded since the last pass (conversions, copies) in a command buffer of its own, timed
            // apart from the pass (PSPOCKET_GPU_PASS_TIMES).
            if (render_cb) {
                auto times = pass_times;
                const u32 place = pass_place;
                [render_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                    const double s = cb.GPUEndTime - cb.GPUStartTime;
                    std::lock_guard lock(times->mutex);
                    times->by_place[place].before_ns += s > 0 ? static_cast<u64>(s * 1e9) : 0;
                }];
                pass_times_submitting = true;
                const bool was_open = pass_open;
                pass_open = false; // submit() must not end the pass being started
                submit(true);
                pass_open = was_open;
                pass_times_submitting = false;
            }
            pass_samples = nil;
            if (pass_times->timestamps) {
                MTLCounterSampleBufferDescriptor* sd = [MTLCounterSampleBufferDescriptor new];
                sd.counterSet = pass_times->timestamps;
                sd.storageMode = MTLStorageModeShared;
                sd.sampleCount = 4;
                pass_samples = [device newCounterSampleBufferWithDescriptor:sd error:nil];
            }
            MTLRenderPassSampleBufferAttachmentDescriptor* at = pass_desc.sampleBufferAttachments[0];
            at.sampleBuffer = pass_samples;
            at.startOfVertexSampleIndex = pass_samples ? 0 : MTLCounterDontSample;
            at.endOfVertexSampleIndex = pass_samples ? 1 : MTLCounterDontSample;
            at.startOfFragmentSampleIndex = pass_samples ? 2 : MTLCounterDontSample;
            at.endOfFragmentSampleIndex = pass_samples ? 3 : MTLCounterDontSample;
        }
        id<MTLCommandBuffer> cb = render_buffer();
        pass_desc.visibilityResultBuffer = visibility;
        pass_zpass_slot = -1;
        encoder_zpass_slot = -1;
        encoder = [cb renderCommandEncoderWithDescriptor:pass_desc];
        encoder.label = @"rsx pass";
        pass_started = true;
        stats.render_passes++;
        pass_draws_start = stats.draws;
        // What the pass moves between memory and the tile memory (D-273): attachments loaded at its start, stored at
        // its end, over its area (stores left unknown are counted when it ends, end_encoder).
        const u64 area = u64{pass_width} * pass_height;
        for (u32 i = 0; i < pass.color_count; i++) {
            const u64 bytes = area * surface_bytes_per_pixel(pass.color[i]->gcm_format, false);
            if (pass_desc.colorAttachments[i].loadAction == MTLLoadActionLoad) stats.attachment_load_bytes += bytes;
            if (pass_desc.colorAttachments[i].storeAction == MTLStoreActionStore) stats.attachment_store_bytes += bytes;
        }
        if (pass.depth) {
            const u64 bytes = area * surface_bytes_per_pixel(pass.depth->gcm_format, true);
            if (pass_desc.depthAttachment.loadAction == MTLLoadActionLoad) stats.attachment_load_bytes += bytes;
            if (pass_desc.depthAttachment.storeAction == MTLStoreActionStore) stats.attachment_store_bytes += bytes;
        }
        // A surface's generation counts the passes and draws that write it, not the passes it is attached to (D-105):
        // GTA V's lighting pass samples the depth buffer it tests against without writing it, and every conversion
        // of it ended the pass, which the next draw restarted, which made the depth buffer new again (30 passes a
        // frame).
        for (u32 i = 0; i < pass.color_count; i++) {
            pass.color[i]->initialized = true;
            pass.color[i]->gpu_newer = true;
            pass.color[i]->last_used = tick;
            pass.color[i]->last_frame = stats.flips;
            if (pass_desc.colorAttachments[i].loadAction == MTLLoadActionClear) pass.color[i]->generation++;
            surface_written(*pass.color[i]); // D-122
        }
        if (pass.depth) {
            pass.depth->initialized = true;
            pass.depth->gpu_newer = true;
            pass.depth->last_used = tick;
            pass.depth->last_frame = stats.flips;
            if (pass_desc.depthAttachment.loadAction == MTLLoadActionClear) pass.depth->generation++;
            if (pass_desc.stencilAttachment.loadAction == MTLLoadActionClear) pass.depth->stencil_generation++;
        }
        // Clears of part of its attachments that waited, as its first draws (D-945; their surfaces' state was set when
        // they were made).
        for (const ClearOp& op : pass_quads) {
            u32 cleared = 0;
            for (u32 i = 0; i < op.targets.color_count; i++)
                for (u32 j = 0; j < pass.color_count; j++)
                    if (pass.color[j] == op.targets.color[i]) cleared |= 1u << j;
            encode_clear_quad(encoder, pass, cleared, pass_width, pass_height, op.x0, op.y0, op.x1, op.y1, op.colors[0],
                op.channels, op.depth, op.stencil, op.clear_depth && op.targets.depth, op.clear_stencil && op.targets.depth);
        }
        pass_quads.clear();
    }
    return encoder;
}

void MetalBackend::Impl::end_pass()
{
    // When the targets change (D-945), a pass that only cleared leaves its clears waiting for the next pass, and one
    // that drew leaves its encoder open until the next encoder is made: its store actions wait for what that one does.
    // Anything else (a copy, a conversion, a readback, a flip, a submit) runs the clears waiting first.
    const bool to_next = pass_end_reason == kEndTargets && merging();
    clears_waiting_on_pass = 0; // they did not keep this pass going
    if (!pass_open) {
        if (!to_next) flush_pending_clears();
        return;
    }
    if (pass_started) pass_ends[pass_end_reason]++;
    bool clear_only = false;
    u64 stored = 0;
    if (!pass_started) {
        // A pass that only clears (load-action clears) must still run.
        bool clears = false;
        for (u32 i = 0; i < pass.color_count; i++) {
            clears |= pass_desc.colorAttachments[i].loadAction == MTLLoadActionClear && pass.color[i]->initialized;
        }
        if (pass.depth) clears |= pass_desc.depthAttachment.loadAction == MTLLoadActionClear && pass.depth->initialized;
        // A Z24S8 buffer's depth and stencil load alike (open_pass, do_clear): a clear of both waits as one.
        const bool stencil_alike = !pass_desc.stencilAttachment.texture ||
            (pass_desc.depthAttachment.loadAction == MTLLoadActionClear) ==
                (pass_desc.stencilAttachment.loadAction == MTLLoadActionClear);
        if ((clears || !pass_quads.empty()) && to_next && stencil_alike) {
            // Its load actions' clears wait, then the clears it was to draw first, in that order (D-945).
            if (clears) wait_pass_clears();
            for (ClearOp& op : pass_quads) push_pending_clear(std::move(op));
            pass_quads.clear();
        } else if (clears || !pass_quads.empty()) {
            stored = stats.attachment_store_bytes;
            start_encoder();
            clear_only = true;
            stats.clear_only_passes++;
        }
    }
    const bool timed = pass_times && encoder && !pass_times_submitting;
    if (encoder) {
        if (to_next && pass_store_unknown) {
            if (closing.encoder) finish_closing(false); // never two: start_encoder ends the one before
            closing.encoder = encoder;
            closing.targets = pass;
            closing.width = pass_width;
            closing.height = pass_height;
        } else {
            end_encoder(encoder, pass, pass_width, pass_height, pass_store_unknown, 0);
        }
        encoder = nil;
    }
    if (clear_only) stats.clear_only_store_bytes += stats.attachment_store_bytes - stored;
    pass_open = false;
    pass_started = false;
    if (!to_next) flush_pending_clears(); // made while this pass drew: after its draws, as recorded
    if (timed) {
        // The pass alone in a command buffer, timed (PSPOCKET_GPU_PASS_TIMES).
        auto times = pass_times;
        const u32 place = pass_place++ + (ab_half() ? 1000u : 0u); // with PSPOCKET_GPU_AB: 1000 + place when on
        const u64 draws = stats.draws - pass_draws_start;
        std::string targets = describe_targets(pass) + std::format("; last fp {:016x}", pass_last_fp);
        for (u32 i = 0; i < pass.color_count; i++) {
            id<MTLTexture> t = pass.color[i]->texture;
            targets += std::format("; texture {}x{} x{} parent {}", t.width, t.height, t.sampleCount,
                t.parentTexture ? std::format("{}x{}", t.parentTexture.width, t.parentTexture.height) : "none");
        }
        targets += std::format("; area {}x{}", pass_width, pass_height);
        id<MTLCounterSampleBuffer> samples = pass_samples;
        pass_samples = nil;
        [render_buffer() addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            const double s = cb.GPUEndTime - cb.GPUStartTime;
            u64 stage[4] = {};
            if (samples) {
                NSData* data = [samples resolveCounterRange:NSMakeRange(0, 4)];
                if (data.length >= sizeof(stage)) std::memcpy(stage, data.bytes, sizeof(stage));
            }
            std::lock_guard lock(times->mutex);
            PassTimes::Entry& e = times->by_place[place];
            e.ns += s > 0 ? static_cast<u64>(s * 1e9) : 0;
            const auto span = [&](u64 a, u64 b) {
                return b > a && a != MTLCounterErrorValue && b != MTLCounterErrorValue
                    ? static_cast<u64>(static_cast<double>(b - a) * times->gpu_tick_ns) : 0;
            };
            e.vertex_ns += span(stage[0], stage[1]);
            e.fragment_ns += span(stage[2], stage[3]);
            e.frames++;
            e.draws += draws;
            if (e.targets.empty()) e.targets = targets;
        }];
        pass = {};
        pass_times_submitting = true;
        submit(true); // finished before the next one starts: the GPU overlaps command buffers otherwise
        pass_times_submitting = false;
        return;
    }
    pass = {};
}

// ---- clears that wait, and stores the next pass makes dead (D-945) ------------------------------------------------

void MetalBackend::Impl::target_area(const PassTargets& t, u32& width, u32& height, bool& partial)
{
    width = t.width ? t.width : ~0u;
    height = t.height ? t.height : ~0u;
    partial = false;
    const auto take = [&](const Surface* s) {
        width = std::min(width, s->width);
        height = std::min(height, s->height);
        partial |= s->width != t.width || s->height != t.height;
    };
    for (u32 i = 0; i < t.color_count; i++) take(t.color[i]);
    if (t.depth) take(t.depth);
}

bool MetalBackend::Impl::clear_waits_on(const Surface* s) const
{
    for (const ClearOp& op : pending_clears)
        if (op.targets.contains(s)) return true;
    return false;
}

bool MetalBackend::Impl::clear_waits_over(u32 addr, u32 size) const
{
    const u64 end = u64{addr} + size;
    const auto over = [&](const Surface* s) { return s && u64{s->address} < end && addr < u64{s->address} + s->bytes(); };
    for (const ClearOp& op : pending_clears) {
        for (u32 i = 0; i < op.targets.color_count; i++)
            if (over(op.targets.color[i])) return true;
        if (over(op.targets.depth)) return true;
    }
    return false;
}

void MetalBackend::Impl::drop_target(ClearOp& op, const Surface* s)
{
    u32 k = 0;
    for (u32 i = 0; i < op.targets.color_count; i++) {
        if (op.targets.color[i] == s) continue;
        op.targets.color[k] = op.targets.color[i];
        op.targets.rsx_index[k] = op.targets.rsx_index[i];
        op.colors[k] = op.colors[i];
        op.color_loaded[k] = op.color_loaded[i];
        k++;
    }
    op.targets.color_count = k;
    if (op.targets.depth == s) {
        op.targets.depth = nullptr;
        op.clear_depth = op.clear_stencil = false;
    }
}

void MetalBackend::Impl::push_pending_clear(ClearOp op)
{
    if (op.whole) {
        // What clears made before it would do to these surfaces it overwrites whole, and nothing read them since
        // (reads run the waiting clears first). So each surface has at most one clear of all of it waiting, before any
        // of part of it: a pass that takes them does that one as its load action and the others as its first draws.
        for (ClearOp& before : pending_clears) {
            for (u32 i = 0; i < op.targets.color_count; i++) drop_target(before, op.targets.color[i]);
            if (op.targets.depth) drop_target(before, op.targets.depth);
        }
        std::erase_if(pending_clears, [](const ClearOp& o) { return !o.targets.color_count && !o.targets.depth; });
    }
    pending_clears.push_back(std::move(op));
}

// A pass that only cleared, ended by a change of targets: what it clears at its start waits as one clear, its
// attachments' state set as start_encoder would have, for the next pass to do as its load actions or to run before
// it. The attachments it would only have loaded are left as they are.
void MetalBackend::Impl::wait_pass_clears()
{
    ClearOp op;
    op.whole = true;
    op.width = op.x1 = pass_width;
    op.height = op.y1 = pass_height;
    op.targets.width = pass.width;
    op.targets.height = pass.height;
    for (u32 i = 0; i < pass.color_count; i++) {
        if (pass_desc.colorAttachments[i].loadAction != MTLLoadActionClear) continue;
        const u32 k = op.targets.color_count++;
        op.targets.color[k] = pass.color[i];
        op.targets.rsx_index[k] = pass.rsx_index[i];
        op.colors[k] = pass_desc.colorAttachments[i].clearColor;
    }
    if (pass.depth && pass_desc.depthAttachment.loadAction == MTLLoadActionClear) {
        op.targets.depth = pass.depth;
        op.depth = pass_desc.depthAttachment.clearDepth;
        op.clear_depth = true;
        if (pass_desc.stencilAttachment.texture) { // cleared with the depth (end_pass checks)
            op.stencil = pass_desc.stencilAttachment.clearStencil;
            op.clear_stencil = true;
        }
    }
    for (u32 i = 0; i < op.targets.color_count; i++) {
        Surface* s = op.targets.color[i];
        s->initialized = s->gpu_newer = true;
        s->last_used = tick;
        s->last_frame = stats.flips;
        s->generation++;
        surface_written(*s); // D-122
    }
    if (Surface* d = op.targets.depth) {
        d->initialized = d->gpu_newer = true;
        d->last_used = tick;
        d->last_frame = stats.flips;
        d->generation++;
        if (op.clear_stencil) d->stencil_generation++;
    }
    if (op.targets.color_count || op.targets.depth) push_pending_clear(std::move(op));
}

// The waiting clears this pass does itself (open_pass): those over attachments of it only, unless a clear made after one
// that this pass cannot do touches the same surfaces (that one runs before this pass, so it would come first). A clear
// of whole surfaces this pass draws whole is its load action, a clear of part of them inside its area one of its first
// draws (in order, after the load actions: each surface has its clear of all of it first, push_pending_clear).
void MetalBackend::Impl::fold_pending_clears()
{
    if (pending_clears.empty()) return;
    const usize n = pending_clears.size();
    std::vector<const Surface*> blocked;
    std::vector<u8> fold(n, 0); // 1: a load action, 2: a first draw
    for (usize k = n; k-- > 0;) {
        const ClearOp& op = pending_clears[k];
        bool mine = true, drawn_whole = true, starts_alike = true;
        const auto take = [&](const Surface* s, bool loaded) {
            mine &= pass.contains(s) && !s->cpu_newer && std::find(blocked.begin(), blocked.end(), s) == blocked.end();
            drawn_whole &= s->width == pass_width && s->height == pass_height;
            // A surface that held nothing before the clear starts cleared in the clear's own pass: this one must start
            // it so over the same area.
            starts_alike &= loaded || (op.width == pass_width && op.height == pass_height);
        };
        for (u32 i = 0; i < op.targets.color_count; i++) take(op.targets.color[i], op.color_loaded[i]);
        if (op.targets.depth) take(op.targets.depth, op.depth_loaded);
        const bool quad = !op.whole && op.x1 <= pass_width && op.y1 <= pass_height && starts_alike;
        fold[k] = !mine ? 0 : op.whole ? (drawn_whole ? 1 : 0) : quad ? 2 : 0;
        if (!fold[k]) {
            for (u32 i = 0; i < op.targets.color_count; i++) blocked.push_back(op.targets.color[i]);
            if (op.targets.depth) blocked.push_back(op.targets.depth);
        }
    }
    std::vector<ClearOp> left;
    for (usize k = 0; k < n; k++) {
        ClearOp& op = pending_clears[k];
        if (!fold[k]) {
            left.push_back(std::move(op));
            continue;
        }
        stats.clears_folded++;
        if (tracing) {
            PS_LOG_INFO("rsx.trace", "  a clear that waited is this pass's {}:{}", fold[k] == 1 ? "load action" : "first draw",
                describe_targets(op.targets));
        }
        if (fold[k] == 2) {
            // What held nothing before it starts cleared, as open_pass starts a surface nobody wrote.
            for (u32 i = 0; i < op.targets.color_count; i++) {
                if (op.color_loaded[i]) continue;
                for (u32 j = 0; j < pass.color_count; j++) {
                    if (pass.color[j] != op.targets.color[i]) continue;
                    pass_desc.colorAttachments[j].loadAction = MTLLoadActionClear;
                    pass_desc.colorAttachments[j].clearColor = MTLClearColorMake(0, 0, 0, 0);
                }
            }
            if (op.targets.depth && !op.depth_loaded) {
                pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
                pass_desc.depthAttachment.clearDepth = 1.0;
                if (pass_desc.stencilAttachment.texture) {
                    pass_desc.stencilAttachment.loadAction = MTLLoadActionClear;
                    pass_desc.stencilAttachment.clearStencil = 0;
                }
            }
            pass_quads.push_back(std::move(op));
            continue;
        }
        for (u32 i = 0; i < op.targets.color_count; i++) {
            for (u32 j = 0; j < pass.color_count; j++) {
                if (pass.color[j] != op.targets.color[i]) continue;
                pass_desc.colorAttachments[j].loadAction = MTLLoadActionClear;
                pass_desc.colorAttachments[j].clearColor = op.colors[i];
            }
        }
        if (op.targets.depth) {
            pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
            pass_desc.depthAttachment.clearDepth = op.depth;
            if (pass_desc.stencilAttachment.texture) {
                pass_desc.stencilAttachment.loadAction = MTLLoadActionClear;
                pass_desc.stencilAttachment.clearStencil = op.stencil;
            }
        }
    }
    pending_clears = std::move(left);
}

void MetalBackend::Impl::flush_pending_clears()
{
    if (pending_clears.empty()) return;
    PS_CHECK(encoder == nil); // their passes are recorded between others
    std::vector<ClearOp> ops = std::move(pending_clears);
    pending_clears.clear();
    for (const ClearOp& op : ops) run_clear_op(op);
}

void MetalBackend::Impl::settle_before_pass()
{
    finish_closing(true);
    flush_pending_clears();
}

void MetalBackend::Impl::finish_closing(bool next_pass)
{
    if (!closing.encoder) return;
    u32 dont_store = 0;
    if (next_pass) {
        // What happens first to each of its attachments from here: the waiting clears, in order, then the open pass's
        // load actions. Overwritten whole before anything reads it: not stored.
        const auto cleared_whole = [&](const Surface* s, bool stencil) {
            for (const ClearOp& op : pending_clears)
                if (op.targets.contains(s)) return op.whole;
            if (!pass_open || !pass.contains(s) || s->width != pass_width || s->height != pass_height) return false;
            for (u32 j = 0; j < pass.color_count; j++)
                if (pass.color[j] == s) return pass_desc.colorAttachments[j].loadAction == MTLLoadActionClear;
            return (stencil ? pass_desc.stencilAttachment.loadAction : pass_desc.depthAttachment.loadAction) ==
                   MTLLoadActionClear;
        };
        for (u32 i = 0; i < closing.targets.color_count; i++)
            if (cleared_whole(closing.targets.color[i], false)) dont_store |= 1u << i;
        if (const Surface* d = closing.targets.depth) {
            if (cleared_whole(d, false)) dont_store |= 1u << 4;
            if (cleared_whole(d, true)) dont_store |= 1u << 5;
        }
        if (tracing && dont_store) {
            PS_LOG_INFO("rsx.trace", "  the pass before ({}) does not store {:#x}: this pass clears it whole",
                describe_targets(closing.targets), dont_store);
        }
    }
    end_encoder(closing.encoder, closing.targets, closing.width, closing.height, true, dont_store);
    closing = {};
}

void MetalBackend::Impl::end_encoder(id<MTLRenderCommandEncoder> enc, const PassTargets& t, u32 width, u32 height,
    bool unknown, u32 dont_store)
{
    if (unknown) {
        const u64 area = u64{width} * height;
        for (u32 i = 0; i < t.color_count; i++) {
            const bool skip = (dont_store >> i) & 1;
            [enc setColorStoreAction:skip ? MTLStoreActionDontCare : MTLStoreActionStore atIndex:i];
            (skip ? stats.attachment_store_skipped_bytes : stats.attachment_store_bytes) +=
                area * surface_bytes_per_pixel(t.color[i]->gcm_format, false);
        }
        if (const Surface* d = t.depth) {
            const bool stencil = d->pixel_format == MTLPixelFormatDepth32Float_Stencil8;
            const bool skip_depth = (dont_store >> 4) & 1, skip_stencil = (dont_store >> 5) & 1;
            [enc setDepthStoreAction:skip_depth ? MTLStoreActionDontCare : MTLStoreActionStore];
            if (stencil) [enc setStencilStoreAction:skip_stencil ? MTLStoreActionDontCare : MTLStoreActionStore];
            // D-273 counts a Z24S8 buffer's four bytes a pixel once, with its depth.
            (skip_depth && (!stencil || skip_stencil) ? stats.attachment_store_skipped_bytes : stats.attachment_store_bytes) +=
                area * surface_bytes_per_pixel(d->gcm_format, true);
        }
    }
    [enc endEncoding];
}

// A clear that waited, in a render pass of its own: its load actions when it clears its surfaces whole, else a quad
// through its region over what they held (the ones that held nothing start cleared, as open_pass has them).
void MetalBackend::Impl::run_clear_op(const ClearOp& op)
{
    const PassTargets& t = op.targets;
    MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
    const u64 area = u64{op.width} * op.height;
    u64 loaded = 0, stored = 0;
    for (u32 i = 0; i < t.color_count; i++) {
        MTLRenderPassColorAttachmentDescriptor* a = d.colorAttachments[i];
        const bool load = !op.whole && op.color_loaded[i];
        a.texture = t.color[i]->texture;
        a.loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
        a.clearColor = op.whole ? op.colors[i] : MTLClearColorMake(0, 0, 0, 0);
        a.storeAction = MTLStoreActionStore;
        const u64 bytes = area * surface_bytes_per_pixel(t.color[i]->gcm_format, false);
        loaded += load ? bytes : 0;
        stored += bytes;
    }
    if (const Surface* z = t.depth) {
        const bool load = !op.whole && op.depth_loaded;
        d.depthAttachment.texture = z->texture;
        d.depthAttachment.loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
        d.depthAttachment.clearDepth = op.whole ? op.depth : 1.0;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        if (z->pixel_format == MTLPixelFormatDepth32Float_Stencil8) {
            d.stencilAttachment.texture = z->texture;
            d.stencilAttachment.loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
            d.stencilAttachment.clearStencil = op.whole ? op.stencil : 0;
            d.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        const u64 bytes = area * surface_bytes_per_pixel(z->gcm_format, true);
        loaded += load ? bytes : 0;
        stored += bytes;
    }
    d.renderTargetWidth = op.width;
    d.renderTargetHeight = op.height;
    id<MTLRenderCommandEncoder> enc = [render_buffer() renderCommandEncoderWithDescriptor:d];
    enc.label = @"rsx clear";
    if (!op.whole) {
        encode_clear_quad(enc, t, (1u << t.color_count) - 1, op.width, op.height, op.x0, op.y0, op.x1, op.y1,
            op.colors[0], op.channels, op.depth, op.stencil, op.clear_depth, op.clear_stencil);
    }
    [enc endEncoding];
    stats.render_passes++;
    stats.clear_passes++;
    pass_ends[kEndClear]++;
    stats.attachment_load_bytes += loaded;
    stats.attachment_store_bytes += stored;
    if (op.whole) {
        stats.clear_only_passes++;
        stats.clear_only_store_bytes += stored;
    }
    if (tracing) PS_LOG_INFO("rsx.trace", "  a clear that waited runs in a pass of its own:{}", describe_targets(t));
}

void MetalBackend::Impl::encode_clear_quad(id<MTLRenderCommandEncoder> enc, const PassTargets& t, u32 cleared, u32 width,
    u32 height, u32 x0, u32 y0, u32 x1, u32 y1, MTLClearColor color, u32 channels, double depth, u32 stencil,
    bool clear_depth, bool clear_stencil)
{
    id<MTLRenderPipelineState> pso = clear_pipeline_for(t, cleared, channels, clear_depth, clear_stencil);
    if (!pso) return;
    __strong id<MTLDepthStencilState>& ds = clear_depth_states[(clear_depth ? 1u : 0u) | (clear_stencil ? 2u : 0u)];
    if (!ds) {
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = clear_depth;
        if (clear_stencil) {
            MTLStencilDescriptor* st = [MTLStencilDescriptor new];
            st.stencilCompareFunction = MTLCompareFunctionAlways;
            st.depthStencilPassOperation = MTLStencilOperationReplace;
            st.writeMask = 0xff;
            dd.frontFaceStencil = st;
            dd.backFaceStencil = st;
        }
        ds = [device newDepthStencilStateWithDescriptor:dd];
    }
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:ds];
    [enc setStencilReferenceValue:stencil];
    [enc setViewport:MTLViewport{0, 0, static_cast<double>(width), static_cast<double>(height), 0, 1}];
    [enc setScissorRect:MTLScissorRect{x0, y0, x1 - x0, y1 - y0}];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    const float z = static_cast<float>(depth);
    [enc setVertexBytes:&z length:sizeof(z) atIndex:0];
    const float c[4] = {static_cast<float>(color.red), static_cast<float>(color.green), static_cast<float>(color.blue),
        static_cast<float>(color.alpha)};
    [enc setFragmentBytes:c length:sizeof(c) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    forget_encoder_state(); // the next draw sets all of its own again (D-954)
}

// The passes that took most GPU time (PSPOCKET_GPU_PASS_TIMES): by their place in the frame, since the game draws its
// frames the same way each time.
void MetalBackend::Impl::log_pass_times()
{
    struct Row {
        u32 place;
        double ms, draws, vertex_ms, fragment_ms, before_ms;
        std::string targets;
    };
    std::vector<Row> rows;
    double passes_ms = 0, before_ms = 0;
    u64 frames = 0;
    {
        std::lock_guard lock(pass_times->mutex);
        for (const auto& [place, e] : pass_times->by_place) {
            if (!e.frames) continue;
            const double n = static_cast<double>(e.frames);
            rows.push_back({place, static_cast<double>(e.ns) / 1e6 / n, static_cast<double>(e.draws) / n,
                static_cast<double>(e.vertex_ns) / 1e6 / n, static_cast<double>(e.fragment_ns) / 1e6 / n,
                static_cast<double>(e.before_ns) / 1e6 / n, e.targets});
            passes_ms += rows.back().ms;
            before_ms += rows.back().before_ms;
            frames = std::max(frames, e.frames);
        }
        pass_times->by_place.clear();
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
    PS_LOG_INFO(kChan, "GPU time by pass (last {} frames): {:.2f} ms a frame in {} passes, {:.2f} ms between them", frames,
        passes_ms, rows.size(), before_ms);
    for (usize i = 0; i < rows.size() && i < 24; i++) {
        PS_LOG_INFO(kChan, "  pass {:3}: {:6.3f} ms ({:4.1f}%; vertex {:.3f}, fragment {:.3f}; before it {:.3f}), "
                           "{:5.1f} draws ->{}", rows[i].place, rows[i].ms,
            passes_ms > 0 ? 100.0 * rows[i].ms / passes_ms : 0.0, rows[i].vertex_ms, rows[i].fragment_ms,
            rows[i].before_ms, rows[i].draws, rows[i].targets);
    }
}

// ---- clears -----------------------------------------------------------------------------------------------------

std::string MetalBackend::Impl::describe_targets(const PassTargets& t)
{
    std::string d;
    for (u32 i = 0; i < t.color_count; i++) {
        const Surface* c = t.color[i];
        d += std::format(" {}{:#010x} fmt {} {}x{} pitch {:#x}", i ? ", " : "", c->address, c->gcm_format, c->width,
            c->height, c->pitch);
    }
    if (t.depth) d += std::format("{} depth {:#010x} fmt {}", t.color_count ? ";" : "", t.depth->address, t.depth->gcm_format);
    return d.empty() ? " nothing" : d;
}

void MetalBackend::Impl::trace_targets_of(const PassTargets& t)
{
    for (u32 i = 0; i < t.color_count; i++) {
        const u32 a = t.color[i]->address;
        if (std::find(trace_targets.begin(), trace_targets.end(), a) == trace_targets.end()) trace_targets.push_back(a);
    }
}

void MetalBackend::Impl::trace_dump()
{
    // Every colour target the frame used, as it is now, converted to 8-bit BGRA.
    for (const u32 address : trace_targets) {
        Surface* s = find_surface(address, false);
        if (!s || !s->texture || !config.trace_surface) continue;
        std::vector<u8> raw;
        if (!read_back(*s, raw)) continue;
        std::vector<u8> out(static_cast<usize>(s->width) * s->height * 4);
        const auto unit = [](float v) { return static_cast<u8>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        for (usize i = 0, n = static_cast<usize>(s->width) * s->height; i < n; i++) {
            u8* o = &out[i * 4];
            switch (s->pixel_format) {
            case MTLPixelFormatBGRA8Unorm: std::memcpy(o, &raw[i * 4], 4); break;
            case MTLPixelFormatRGBA8Unorm: o[0] = raw[i * 4 + 2], o[1] = raw[i * 4 + 1], o[2] = raw[i * 4], o[3] = raw[i * 4 + 3]; break;
            case MTLPixelFormatR8Unorm: o[0] = o[1] = o[2] = raw[i], o[3] = 255; break;
            case MTLPixelFormatRG8Unorm: o[0] = 0, o[1] = raw[i * 2 + 1], o[2] = raw[i * 2], o[3] = 255; break;
            case MTLPixelFormatR32Float: {
                float v;
                std::memcpy(&v, &raw[i * 4], 4);
                o[0] = o[1] = o[2] = unit(v), o[3] = 255;
                break;
            }
            case MTLPixelFormatRGBA16Float: {
                __fp16 h[4];
                std::memcpy(h, &raw[i * 8], 8);
                o[0] = unit(static_cast<float>(h[2])), o[1] = unit(static_cast<float>(h[1]));
                o[2] = unit(static_cast<float>(h[0])), o[3] = unit(static_cast<float>(h[3]));
                break;
            }
            case MTLPixelFormatRGBA32Float: {
                float f[4];
                std::memcpy(f, &raw[i * 16], 16);
                o[0] = unit(f[2]), o[1] = unit(f[1]), o[2] = unit(f[0]), o[3] = unit(f[3]);
                break;
            }
            default: break;
            }
        }
        config.trace_surface(std::format("{:08x}-fmt{}-{}x{}", s->address, s->gcm_format, s->width, s->height), out.data(),
            s->width, s->height);
    }
    trace_targets.clear();
    // And every 2D texture it sampled, as the shader saw it: tex-<address>-fmt<format>-t<unit>-<width>x<height>.
    for (const TracedTexture& x : trace_textures) {
        if (!config.trace_surface) break;
        const u32 w = static_cast<u32>(x.texture.width), h = static_cast<u32>(x.texture.height);
        std::vector<std::array<float, 4>> texels;
        if (!read_texture(x.texture, 0, 0, w, h, texels)) continue;
        std::vector<u8> out(static_cast<usize>(w) * h * 4);
        const auto unit = [](float v) { return static_cast<u8>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        for (usize i = 0; i < texels.size(); i++) {
            out[i * 4 + 0] = unit(texels[i][2]), out[i * 4 + 1] = unit(texels[i][1]);
            out[i * 4 + 2] = unit(texels[i][0]), out[i * 4 + 3] = unit(texels[i][3]);
        }
        config.trace_surface(std::format("tex-{:08x}-fmt{:x}-t{}-{}x{}", x.address, x.format, x.unit, w, h),
            out.data(), w, h);
    }
    trace_textures.clear();
}

void MetalBackend::Impl::do_clear(u32 mask, const RsxState& state)
{
    // RPCS3 VKGSRender.cpp clear_surface: the region is the scissor clipped to the surface; stencil is not cleared
    // when its write mask is zero.
    mask &= clear::kColor | clear::kDepthStencil;
    if (!state.depth_stencil().front.write_mask) mask &= ~clear::kStencil;
    if (!mask) return;
    stats.clears++;

    PassTargets t;
    if (!select_targets(state, t, mask)) return;
    if (!(mask & clear::kColor)) t.color_count = 0;
    if (!(mask & clear::kDepthStencil)) t.depth = nullptr;

    const SurfaceInfo si = state.surface();
    const ScissorInfo sc = state.scissor();
    const AaFactors aa = aa_factors(si.antialias); // the scissor is in pixels, the pass in samples (D-250)
    // The region: the scissor within an area.
    const auto region = [&](u32 w, u32 h, u32& rx0, u32& ry0, u32& rx1, u32& ry1) {
        rx0 = std::min<u32>(sc.x * aa.x, w), ry0 = std::min<u32>(sc.y * aa.y, h);
        rx1 = std::min<u32>((u32{sc.x} + sc.width) * aa.x, w), ry1 = std::min<u32>((u32{sc.y} + sc.height) * aa.y, h);
    };

    // The clear writes its 32-bit value into each pixel as it is. A8B8G8R8 targets keep r g b a in their RGBA8Unorm
    // copy, so the value's red byte is their blue: red and blue trade places, in the value and in the channel mask
    // (RPCS3 VKGSRender.cpp clear_surface, get_abgr8_clear_color, get_abgr8_clearmask). Without it GTA V's characters
    // turned red: their damage target is cleared to 0x00ff0000 (D-119).
    const auto cf = static_cast<SurfaceColorFormat>(si.color_format);
    const bool abgr = cf == SurfaceColorFormat::A8B8G8R8 || cf == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8 ||
                      cf == SurfaceColorFormat::X8B8G8R8_O8B8G8R8;
    const u32 register_argb = state.color_clear_value();
    const u32 argb = abgr ? (register_argb & 0xff00ff00u) | ((register_argb >> 16) & 0xffu) | ((register_argb & 0xffu) << 16)
                          : register_argb;
    const MTLClearColor color = MTLClearColorMake(((argb >> 16) & 0xff) / 255.0, ((argb >> 8) & 0xff) / 255.0,
        (argb & 0xff) / 255.0, (argb >> 24) / 255.0);
    const u32 zs = state.zstencil_clear_value();
    const bool z24 = si.depth_format == 2;
    const double depth = z24 ? (zs >> 8) / 16777215.0 : (zs & 0xffff) / 65535.0;
    const u32 stencil = zs & 0xff;
    const u32 register_channels = (mask >> 4) & 0xf; // R G B A
    const u32 channels =
        abgr ? (register_channels & 0xau) | ((register_channels & 1u) << 2) | ((register_channels >> 2) & 1u)
             : register_channels;
    // The clear's own targets only: in a pass with more colour targets than the clear's (its first ones, below), the
    // others keep what they hold, and a depth buffer the clear has none of is not cleared (RPCS3 VKGSRender.cpp
    // clear_surface clears the targets the surface registers name).
    const bool clear_depth = (mask & clear::kDepth) && t.depth;
    const bool clear_stencil = (mask & clear::kStencil) && t.depth && z24;

    // What the clear covers: the scissor within its own area, its surface clip within its surfaces (as a pass of its own
    // would have it, open_pass).
    u32 area_width = 0, area_height = 0;
    bool area_partial = false;
    target_area(t, area_width, area_height, area_partial);
    u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    region(area_width, area_height, x0, y0, x1, y1);

    // Keep the open pass if it already has these surfaces and its area holds the clear's region (clearing mid-pass),
    // else switch. A pass over a smaller target with the same depth buffer has less of it: GTA V draws a 320x128 target
    // with its 1280x720 depth buffer, then clears that depth buffer alone; done in that pass, the clear reached its
    // 320x128 corner, and the scene was drawn against old depth everywhere else (run 327's fight, D-951).
    bool here = false;
    if (pass_open) {
        bool subset = true;
        for (u32 i = 0; i < t.color_count; i++) {
            subset &= i < pass.color_count && pass.color[i] == t.color[i];
        }
        if (t.depth && t.depth != pass.depth) subset = false;
        here = subset && !newer_in_guest(t) && x1 <= pass_width && y1 <= pass_height;
    }
    if (!here && merging() && !newer_in_guest(t) && (t.color_count || t.depth)) {
        // D-945: a clear of targets the pass drawing does not have waits, and the pass goes on (the clear runs before
        // anything reads its surfaces); so does a clear of whole surfaces, which the next pass that has them all does
        // as its load actions, without a pass of its own that stores them for that pass to load.
        ClearOp op;
        op.targets = t;
        if (t.depth && !clear_depth && !clear_stencil) op.targets.depth = nullptr; // attached, not cleared
        bool partial = false;
        target_area(t, op.width, op.height, partial);
        region(op.width, op.height, op.x0, op.y0, op.x1, op.y1);
        if (op.x1 <= op.x0 || op.y1 <= op.y0 || (!op.targets.color_count && !op.targets.depth)) return; // nothing to do
        const bool full = op.x0 == 0 && op.y0 == 0 && op.x1 == op.width && op.y1 == op.height;
        op.whole = full && !partial && (!op.targets.color_count || channels == 0xf) &&
                   (!op.targets.depth || (clear_depth && (clear_stencil || !z24)));
        bool shared = false;
        for (u32 i = 0; i < op.targets.color_count; i++) shared |= pass_open && pass.contains(op.targets.color[i]);
        shared |= pass_open && op.targets.depth && pass.contains(op.targets.depth);
        const bool goes_on = pass_open && pass_started && !shared;
        if (goes_on || op.whole) {
            op.colors.fill(color);
            op.channels = channels;
            op.depth = depth;
            op.stencil = stencil;
            op.clear_depth = clear_depth;
            op.clear_stencil = clear_stencil;
            for (u32 i = 0; i < op.targets.color_count; i++) op.color_loaded[i] = op.targets.color[i]->initialized;
            op.depth_loaded = op.targets.depth && op.targets.depth->initialized;
            if (!goes_on && pass_open) end_pass_for(kEndTargets); // what that pass cleared waits too, before this
            // Its surfaces' state as start_encoder and the clear set it.
            for (u32 i = 0; i < op.targets.color_count; i++) {
                Surface* s = op.targets.color[i];
                s->initialized = s->gpu_newer = true;
                s->last_used = tick;
                s->last_frame = stats.flips;
                if (channels) s->generation++;
                surface_written(*s); // D-122
            }
            if (Surface* d = op.targets.depth) {
                d->initialized = d->gpu_newer = true;
                d->last_used = tick;
                d->last_frame = stats.flips;
                if (clear_depth) d->generation++;
                if (clear_stencil) d->stencil_generation++;
            }
            if (op.whole) stats.load_action_clears++;
            if (goes_on) clears_waiting_on_pass++;
            if (tracing) {
                trace_targets_of(t);
                PS_LOG_INFO("rsx.trace", "#{} clear {:#x} ->{}: colour {:#010x}, depth/stencil {:#010x}, region {},{} to {},{} "
                    "(waits{})", ++trace_count, mask, describe_targets(t), register_argb, zs, op.x0, op.y0, op.x1, op.y1,
                    goes_on ? "; the pass drawing goes on" : "");
            }
            push_pending_clear(std::move(op));
            return;
        }
    }
    if (!here) open_pass(t);
    if (!pass_open) return;

    if (x1 <= x0 || y1 <= y0) return;
    const bool full = x0 == 0 && y0 == 0 && x1 == pass_width && y1 == pass_height;
    if (tracing) {
        trace_targets_of(t);
        PS_LOG_INFO("rsx.trace", "#{} clear {:#x} ->{}: colour {:#010x}, depth/stencil {:#010x}, region {},{} to {},{}",
            ++trace_count, mask, describe_targets(t), register_argb, zs, x0, y0, x1, y1);
    }

    if (!pass_started && full && !pass_partial && pass_quads.empty()) {
        // At the start of a pass a full clear is free on a tile-based GPU: make it the load action. Not in a pass that
        // draws in part of a larger target: the load action clears all of it (D-120). Not after clears this pass does
        // as its first draws (D-945): its load action would run before them.
        bool done_color = channels == 0xf || channels == 0;
        if (channels == 0xf) {
            for (u32 i = 0; i < t.color_count; i++) {
                pass_desc.colorAttachments[i].loadAction = MTLLoadActionClear;
                pass_desc.colorAttachments[i].clearColor = color;
                pass.color[i]->initialized = true;
            }
        }
        bool done_depth = !clear_depth && !clear_stencil;
        if (clear_depth && (clear_stencil || !z24)) {
            pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
            pass_desc.depthAttachment.clearDepth = depth;
            if (clear_stencil) {
                pass_desc.stencilAttachment.loadAction = MTLLoadActionClear;
                pass_desc.stencilAttachment.clearStencil = stencil;
            }
            t.depth->initialized = true;
            done_depth = true;
        }
        if (done_color && done_depth) {
            stats.load_action_clears++;
            return;
        }
    }

    // A partial clear: draw a quad with the clear values through the scissor.
    id<MTLRenderCommandEncoder> enc = start_encoder();
    if (channels) {
        for (u32 i = 0; i < t.color_count; i++) pass.color[i]->generation++;
    }
    if (clear_depth) t.depth->generation++;
    if (clear_stencil) t.depth->stencil_generation++;
    encode_clear_quad(enc, pass, (1u << t.color_count) - 1, pass_width, pass_height, x0, y0, x1, y1, color, channels,
        depth, stencil, clear_depth, clear_stencil);
}

// ---- draws ------------------------------------------------------------------------------------------------------

namespace {

// The clip range as Metal's viewport takes it (D-258): its ends in order, inside [0, 1]; [0, 1] when they are not
// numbers.
std::pair<float, float> depth_range(const ViewportInfo& v)
{
    if (!(v.clip_min == v.clip_min) || !(v.clip_max == v.clip_max)) return {0.f, 1.f};
    return {std::clamp(std::min(v.clip_min, v.clip_max), 0.f, 1.f), std::clamp(std::max(v.clip_min, v.clip_max), 0.f, 1.f)};
}

// Primitives Metal draws natively; the rest become index lists (positions relative to a range).
bool native_primitive(Primitive p, MTLPrimitiveType& type)
{
    switch (p) {
    case Primitive::Points: type = MTLPrimitiveTypePoint; return true;
    case Primitive::Lines: type = MTLPrimitiveTypeLine; return true;
    case Primitive::LineStrip: type = MTLPrimitiveTypeLineStrip; return true;
    case Primitive::Triangles: type = MTLPrimitiveTypeTriangle; return true;
    case Primitive::TriangleStrip: type = MTLPrimitiveTypeTriangleStrip; return true;
    default: return false;
    }
}

// Index list for line loops, fans, quads, quad strips and polygons, in positions 0..count-1 plus `add`, appended to
// `out` (kept from draw to draw, D-956).
void convert_primitive(Primitive p, u32 count, MTLPrimitiveType& type, std::vector<u32>& out, u32 add = 0)
{
    switch (p) {
    case Primitive::LineLoop:
        type = MTLPrimitiveTypeLine;
        for (u32 i = 0; i + 1 < count; i++) out.insert(out.end(), {add + i, add + i + 1});
        if (count > 2) out.insert(out.end(), {add + count - 1, add});
        break;
    case Primitive::TriangleFan:
    case Primitive::Polygon:
        type = MTLPrimitiveTypeTriangle;
        for (u32 i = 1; i + 1 < count; i++) out.insert(out.end(), {add, add + i, add + i + 1});
        break;
    case Primitive::Quads:
        type = MTLPrimitiveTypeTriangle;
        for (u32 q = 0; q + 3 < count; q += 4)
            out.insert(out.end(), {add + q, add + q + 1, add + q + 2, add + q, add + q + 2, add + q + 3});
        break;
    case Primitive::QuadStrip:
        type = MTLPrimitiveTypeTriangle;
        for (u32 q = 0; q + 3 < count; q += 2)
            out.insert(out.end(), {add + q, add + q + 1, add + q + 3, add + q, add + q + 3, add + q + 2});
        break;
    default: break;
    }
}

// Index list (positions from `first`) for a range whose indices include the restart index at `restarts` (positions in
// the range, ascending), as RPCS3 uploads them (Emu/RSX/Common/BufferUtils.cpp upload_untouched,
// expand_indexed_triangle_fan, expand_indexed_quads): strips restart at Metal's own restart index (0xFFFFFFFF, always
// on for strips); points, lines and triangles drop the restart indices and go on; fans, polygons, quads, quad strips
// and line loops start again after each one. Appended to `out`; nothing appended: nothing to draw.
void restart_list(Primitive p, u32 first, u32 count, std::span<const u32> restarts, MTLPrimitiveType& type,
    std::vector<u32>& out)
{
    if (native_primitive(p, type)) {
        const bool strip = type == MTLPrimitiveTypeTriangleStrip || type == MTLPrimitiveTypeLineStrip;
        usize next = 0;
        for (u32 k = 0; k < count; k++) {
            if (next < restarts.size() && restarts[next] == k) {
                next++;
                if (strip) out.push_back(0xFFFFFFFFu);
                continue;
            }
            out.push_back(first + k);
        }
        return;
    }
    u32 start = 0;
    const auto segment = [&](u32 end) {
        if (end <= start) return;
        convert_primitive(p, end - start, type, out, first + start);
    };
    for (u32 r : restarts) {
        segment(r);
        start = r + 1;
    }
    segment(count);
}

} // namespace

// The draw's fragment texture units, once (D-954).
void MetalBackend::Impl::decode_textures(const RsxState& state)
{
    for (u32 t = 0; t < kFragmentTextureCount; t++) {
        frag_tex[t] = state.fragment_texture(t);
        frag_tex_control[t] = frag_tex[t].enabled ? texture_control(frag_tex[t]) : 0u;
    }
    vert_tex_decoded = 0;
}

void MetalBackend::Impl::do_draw(const RsxState& state)
{
    tick++;
    if (fast(kFastScratch)) draw_scratch.clear();
    else draw_scratch = {}; // allocated anew each draw, as before D-956 (PSPOCKET_RSX_CPU_AB's other half)
    const DrawClause& d = state.draw;
    // A skipped draw names why in the frame trace too (D-084): a draw the trace does not show cannot be told apart from
    // one the game never made.
    const char* skip_reason = "";
    auto skip = [&](std::string why) {
        stats.draws_skipped++;
        if (tracing) {
            PS_LOG_INFO("rsx.trace", "#{} draw SKIPPED ({}{}) ->{}", ++trace_count, why.empty() ? skip_reason : why.c_str(),
                why.empty() && !*skip_reason ? "?" : "", describe_targets(draw_targets));
        }
        if (!why.empty()) unsupported(std::move(why));
    };

    // Programs and pipelines made in the background since the last draw (D-114, D-158), once before its lookups
    // (D-953; they took them three times a draw).
    if (fast(kFastLookups)) {
        take_shader_cache();
        take_async();
    }
    decode_textures(state); // D-954
    // Occlusion counters (D-102): a query with none left in this render buffer's visibility buffer would count nothing
    // (and conditional rendering would skip what it guards). The buffer so far goes to the GPU, before this draw
    // allocates anything in it, and the next one has twice the counters (D-1024).
    if (zpass_on && render_cb && visibility_next >= visibility_slots && !(pass_started && pass_zpass_slot >= 0)) {
        if (rsx) rsx->alarms().add(Alarm::OcclusionOverflow);
        if (visibility_slots < (1u << 20)) {
            visibility_slots *= 2;
            PS_LOG_INFO(kChan, "occlusion queries: {} counters in one command buffer were not enough (flip {}): the "
                "buffer goes to the GPU, the next ones have {}", visibility_slots / 2, stats.flips, visibility_slots);
        }
        submit(false);
    }

    // ---- targets and programs ----
    PassTargets targets;
    skip_reason = "no render target";
    if (!select_targets(state, targets, ~0u)) return skip("");
    // The texture cache a target this draw samples while drawing to it is read through (D-947): what its copy was
    // made under, the targets and the game's invalidations.
    if (!(targets == bind_targets)) {
        bind_targets = targets;
        bind_epoch++;
    }
    cache_epoch = state.texture_cache_epoch;
    const ShaderProgramInfo shader_program = state.shader_program();
    feedback_fp = shader_program.offset;
    const bool points = d.primitive == Primitive::Points;
    compile_pending = false;
    shader::FpOptions fo;
    fo.shader_control = shader_program.control;
    fo.mrt_count = targets.color_count;
    for (u32 i = 0; i < targets.color_count; i++) fo.output_swizzle[i] = surface_output_swizzle(targets.color[i]->gcm_format);
    std::array<Surface*, kFragmentTextureCount> depth_source{}; // depth buffers sampled directly (D-106)
    for (u32 t = 0; t < kFragmentTextureCount; t++) {
        const TextureInfo& ti = frag_tex[t];
        fo.texture_kind[t] = ti.cubemap && ti.dimension == 2 ? shader::TextureKind::Cube
            : ti.dimension == 1                               ? shader::TextureKind::Tex1D
            : ti.dimension == 3                               ? shader::TextureKind::Tex3D
                                                              : shader::TextureKind::Tex2D;
        if (fo.texture_kind[t] == shader::TextureKind::Tex2D && is_shadow_texture(ti)) fo.texture_kind[t] = shader::TextureKind::Shadow2D;
        if ((fo.texture_kind[t] == shader::TextureKind::Tex2D || fo.texture_kind[t] == shader::TextureKind::Shadow2D) &&
            ti.enabled) {
            if (Surface* zs = depth_sampled_directly(ti)) {
                depth_source[t] = zs;
                fo.texture_kind[t] = fo.texture_kind[t] == shader::TextureKind::Shadow2D ? shader::TextureKind::DepthShadow2D
                                                                                         : shader::TextureKind::Depth2D;
            }
        }
        if (t < 10 && (state.texcoord_control(t) & 1)) fo.texcoord_2d_mask |= 1u << t;
        // Texel conversions and the alpha kill (D-174): compiled in for the units that have them, as RPCS3's
        // TEXTURE_FORMAT_CONVERT and TEXTURE_ALPHA_KILL; the bits reach the shader in fs.tex_control.
        if (ti.enabled) {
            const u32 control = frag_tex_control[t];
            if (control & texctl::kConvertMask) fo.texture_convert_mask = static_cast<u16>(fo.texture_convert_mask | (1u << t));
            if (control & texctl::kAlphaKill) fo.texture_alpha_kill_mask = static_cast<u16>(fo.texture_alpha_kill_mask | (1u << t));
        }
    }
    const RasterInfo raster = state.raster();
    fo.alpha_test = raster.alpha_test;
    fo.flat_colors = raster.shade_mode == static_cast<u32>(ShadeMode::Flat);
    fo.two_sided_lighting = raster.two_side_light; // D-257
    // As setDepthClipMode below, where there is a depth buffer to store the clamped depth (D-941).
    fo.clamp_depth = targets.depth && (raster.depth_clamp || !raster.depth_clip);
    // Polygon stipple (D-263; RPCS3 RSXThread.cpp: only on polygons).
    fo.polygon_stipple = raster.polygon_stipple && d.primitive >= Primitive::Triangles;
    if (points) { // point sprites (D-262; RPCS3 RSXThread.cpp: only on point draws)
        const u32 sprite = state.reg(m::NV4097_SET_POINT_SPRITE_CONTROL);
        if (sprite & 1) fo.point_coord_mask = static_cast<u16>((sprite >> 8) & 0x3ff);
    }
    // The depth bounds test (D-261), when its bounds can leave something out.
    const DepthStencilInfo ds = state.depth_stencil();
    const DepthStencilInfo& bounds = ds;
    fo.depth_bounds = targets.depth && bounds.depth_bounds_test &&
        (bounds.depth_bounds_min > 0.f || bounds.depth_bounds_max < 1.f || !(bounds.depth_bounds_min <= bounds.depth_bounds_max));
    // D-174 (RPCS3 RSXThread.cpp): NV4097_SET_SHADER_PACKER turns the colours sRGB on the 8-bit targets, never from
    // 32-bit exports; alpha to coverage is emulated (one sample here, as RPCS3 without hardware support).
    const u32 colour_format = draw_surface.color_format;
    const bool float_target = colour_format == static_cast<u32>(SurfaceColorFormat::F_W16Z16Y16X16) ||
                              colour_format == static_cast<u32>(SurfaceColorFormat::F_W32Z32Y32X32) ||
                              colour_format == static_cast<u32>(SurfaceColorFormat::F_X32);
    fo.srgb_output = state.reg(m::NV4097_SET_SHADER_PACKER) != 0 && !(fo.shader_control & 0x40) && !float_target &&
                     targets.color_count > 0;
    fo.alpha_to_coverage = ((state.reg(m::NV4097_SET_ANTI_ALIASING_CONTROL) >> 4) & 1) != 0;
    // The signed blend equations, which Metal's blending has not, are done in the shader, which reads the target's
    // pixel (D-948; RPCS3 RSXThread.cpp evaluate_programmable_blending_state): on four-channel targets.
    const BlendInfo draw_blend = state.blend();
    const auto is_signed = [](u32 e) { return e == 0xf005 || e == 0xf006 || e == 0xf007; };
    if (!draw_blend.logic_op_enable && (is_signed(draw_blend.equation_rgb) || is_signed(draw_blend.equation_a))) {
        for (u32 i = 0; i < targets.color_count; i++) {
            if (!draw_blend.enabled_for(targets.rsx_index[i])) continue;
            const MTLPixelFormat pf = targets.color[i]->pixel_format;
            const bool unorm = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatBGRA8Unorm ||
                               pf == MTLPixelFormatR8Unorm || pf == MTLPixelFormatRG8Unorm; // B8, G8B8 too
            if (!unorm && pf != MTLPixelFormatRGBA16Float && pf != MTLPixelFormatRGBA32Float) continue;
            fo.blend_in_shader = static_cast<u8>(fo.blend_in_shader | (1u << i));
            if (unorm) fo.blend_unorm = static_cast<u8>(fo.blend_unorm | (1u << i));
        }
    }
    const shader::FragmentProgram* fprog = nullptr;
    u32 fp_ea = 0;
    const CompiledProgram* fp = fragment_program_for(state, fo, &fprog, fp_ea);
    // A draw whose programs or pipeline are still compiling is drawn by the shader interpreter (D-940); with
    // force_interpreter every draw is.
    bool interpret = force_interpreter && fprog;
    skip_reason = compile_pending ? "fragment program compiling" : "fragment program";
    if (!fp) {
        if (!(compile_pending && fprog && interpreter_on())) {
            if (compile_pending) stats.draws_compiling++;
            return skip("");
        }
        interpret = true;
    }
    if (pass_times && fprog) pass_last_fp = fprog->hash;
    if (skip_fp && fprog && fprog->hash == skip_fp) return skip("PSPOCKET_SKIP_FP");
    // The vertex program after the fragment program: an output the output mask leaves out matters only when the
    // fragment program reads it (D-267).
    const CompiledProgram* vp = vertex_program_for(state, points, fp ? fp->shader.input_mask : fprog->input_mask,
        fo.two_sided_lighting);
    skip_reason = compile_pending ? "vertex program compiling" : "vertex program";
    if (!vp) {
        if (!(compile_pending && interp_vp && interpreter_on())) {
            if (compile_pending) stats.draws_compiling++;
            return skip("");
        }
        interpret = true;
    }
    if (interpret && !interp_vp) return skip("");
    // PSPOCKET_SKIP_VTF_FP_AT=EA (a debugging aid): draws with the fragment program at EA that sample a vertex texture
    // are skipped.
    static const u32 skip_vtf_fp_at = [] {
        const char* v = std::getenv("PSPOCKET_SKIP_VTF_FP_AT");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 0u;
    }();
    if (skip_vtf_fp_at && fp_ea == skip_vtf_fp_at && (interp_vp ? interp_vp_textures : (vp ? vp->shader.texture_mask : 0u))) return skip("PSPOCKET_SKIP_VTF_FP_AT");
    // The interpreter blends as Metal can (D-940): a signed equation, which the translation blends in its shader
    // (D-948), would be drawn as add. Such a draw waits for its programs, as draws did before the interpreter (D-1026).
    if (interpret && fo.blend_in_shader && !force_interpreter) {
        skip_reason = "signed blending waits for the program";
        stats.draws_compiling++;
        return skip("");
    }
    // The units the programs sample and the attributes they read: the decoded programs' when the interpreter draws.
    const u32 fp_textures = interpret ? fprog->texture_mask : fp->shader.texture_mask;
    const u32 vp_textures = interpret ? interp_vp_textures : vp->shader.texture_mask;
    const u32 vp_inputs = interpret ? interp_vp->input_mask : vp->shader.input_mask;

    // ---- scissor: RSX scissor and viewport rectangles within the surface (RPCS3 get_scissor, clip_viewport) ----
    u32 surf_w = ~0u, surf_h = ~0u;
    for (u32 i = 0; i < targets.color_count; i++) {
        surf_w = std::min(surf_w, targets.color[i]->width);
        surf_h = std::min(surf_h, targets.color[i]->height);
    }
    if (targets.depth) {
        surf_w = std::min(surf_w, targets.depth->width);
        surf_h = std::min(surf_h, targets.depth->height);
    }
    // A multisampled target is drawn in samples (D-250): the scissor and the viewport are in pixels.
    const AaFactors aa = aa_factors(draw_surface.antialias);
    const u32 pix_w = surf_w / aa.x, pix_h = surf_h / aa.y;
    const ScissorInfo sc = state.scissor();
    const ViewportInfo vpi = state.viewport();
    const u32 sx0 = std::max<u32>(sc.x, vpi.x), sy0 = std::max<u32>(sc.y, vpi.y);
    const u32 sx1 = std::min({u32{sc.x} + sc.width, u32{vpi.x} + vpi.width, pix_w});
    const u32 sy1 = std::min({u32{sc.y} + sc.height, u32{vpi.y} + vpi.height, pix_h});
    skip_reason = "empty scissor or viewport";
    if (sx1 <= sx0 || sy1 <= sy0) return skip("");
    if (raster.cull_enable && static_cast<CullFace>(raster.cull_face) == CullFace::FrontAndBack &&
        d.primitive >= Primitive::Triangles) {
        skip_reason = "front and back culled";
        return skip(""); // every polygon is culled
    }

    // ---- textures: resolved before the pass opens (copying a bound render target may end a pass) ----
    std::array<id<MTLTexture>, kFragmentTextureCount> bound{};
    std::string trace_tex;
    shader::MslFsParams fs{};
    // Blending in the shader (D-948): the translation's only; the interpreter's draws blend as Metal can.
    shader_blend = interpret ? 0 : fo.blend_in_shader;
    if (shader_blend) {
        fs.blend_equation = state.reg(m::NV4097_SET_BLEND_EQUATION);
        fs.blend_sfactor = state.reg(m::NV4097_SET_BLEND_FUNC_SFACTOR);
        fs.blend_dfactor = state.reg(m::NV4097_SET_BLEND_FUNC_DFACTOR);
        fs.blend_color[0] = static_cast<float>((draw_blend.color >> 16) & 0xff) / 255.f;
        fs.blend_color[1] = static_cast<float>((draw_blend.color >> 8) & 0xff) / 255.f;
        fs.blend_color[2] = static_cast<float>(draw_blend.color & 0xff) / 255.f;
        fs.blend_color[3] = static_cast<float>(draw_blend.color >> 24) / 255.f;
    }
    for (u32 t = 0; t < kFragmentTextureCount; t++) {
        fs.tex_scale[t][0] = fs.tex_scale[t][1] = 1.f;
        if (!(fp_textures & (1u << t))) continue;
        const TextureInfo& ti = frag_tex[t];
        if (ti.is_unnormalized() && ti.width && ti.height) {
            fs.tex_scale[t][0] = 1.f / ti.width;
            fs.tex_scale[t][1] = 1.f / ti.height;
        }
        if (fo.texture_kind[t] == shader::TextureKind::Shadow2D || fo.texture_kind[t] == shader::TextureKind::DepthShadow2D) {
            const u32 bf = ti.base_format();
            fs.tex_scale[t][2] = static_cast<float>(ti.zfunc & 7);
            fs.tex_scale[t][3] = static_cast<float>((ti.mag_filter != static_cast<u32>(tex::MagFilter::Nearest) ? 1u : 0u) |
                                                    (bf == tex::DEPTH16 || bf == tex::DEPTH24_D8 ? 2u : 0u));
        }
        fs.tex_control[t] = ti.enabled ? frag_tex_control[t] : 0u; // D-174
        if (Surface* zs = depth_source[t]) {
            bound[t] = zs->texture;
            zs->last_used = tick;
            zs->last_frame = stats.flips;
            stats.surface_samples++;
            if (tracing) {
                trace_tex += std::format(" t{} {:#010x} fmt {:#04x} {}x{} (depth buffer, sampled)", t, zs->address,
                    ti.base_format(), ti.width, ti.height);
            }
            continue;
        }
        bool ok = true;
        id<MTLTexture> tex = ti.enabled ? bind_texture(ti, t, ok) : nil;
        if (tracing) {
            trace_tex += std::format(" t{} {:#010x} fmt {:#04x} {}x{}{} ({})", t,
                rsx->translate(ti.offset, ti.location).value_or(0), ti.base_format(), ti.width, ti.height,
                ti.enabled ? "" : " disabled", ti.enabled ? bind_source : "-");
        }
        const shader::TextureKind kind = fo.texture_kind[t];
        const MTLTextureType wanted = kind == shader::TextureKind::Cube ? MTLTextureTypeCube
                                    : kind == shader::TextureKind::Tex3D ? MTLTextureType3D
                                                                         : MTLTextureType2D;
        const bool depth_kind = kind == shader::TextureKind::Depth2D || kind == shader::TextureKind::DepthShadow2D;
        if (tex && tex.textureType == wanted && !depth_kind) {
            bound[t] = tex;
            if (tracing && wanted == MTLTextureType2D && trace_textures.size() < 512) {
                const u32 at = rsx->translate(ti.offset, ti.location).value_or(0);
                const auto seen = std::find_if(trace_textures.begin(), trace_textures.end(),
                    [&](const TracedTexture& x) { return x.address == at && x.format == ti.format; });
                if (seen == trace_textures.end()) trace_textures.push_back({t, at, ti.format, tex});
            }
        } else {
            bound[t] = depth_kind                         ? dummy_depth
                     : wanted == MTLTextureTypeCube       ? dummy_cube
                     : wanted == MTLTextureType3D         ? dummy_volume
                                                          : dummy_texture;
        }
        // The LOD bias (D-174), for a texture with mipmaps its filter reads, as RPCS3 sets it on the sampler only then
        // (VKDraw.cpp, GL sampler_state::apply): on one level it would only move magnification to minification.
        if (bound[t].mipmapLevelCount > 1 && ti.min_filter >= static_cast<u32>(tex::MinFilter::NearestNearest)) {
            fs.tex_bias[t] = ti.bias;
        }
    }

    // ---- vertex textures (VP TXL, D-093): resolved before the pass opens, like the fragment textures ----
    std::array<id<MTLTexture>, 4> vbound{};
    for (u32 t = 0; t < 4; t++) {
        if (!(vp_textures & (1u << t))) continue;
        const TextureInfo& vt = vertex_texture(state, t);
        bool ok = true;
        id<MTLTexture> tex = vt.enabled ? bind_texture(vt, 16 + t, ok) : nil;
        // Of the kind the program was translated for (D-274: 1D textures are 2D ones a texel high).
        const MTLTextureType wanted = vt.cubemap && vt.dimension == 2 ? MTLTextureTypeCube
                                    : vt.dimension == 3               ? MTLTextureType3D
                                                                      : MTLTextureType2D;
        vbound[t] = tex && tex.textureType == wanted ? tex
                  : wanted == MTLTextureTypeCube      ? dummy_cube
                  : wanted == MTLTextureType3D        ? dummy_volume
                                                      : dummy_texture;
        if (tracing) {
            trace_tex += std::format(" vt{} {:#010x} fmt {:#04x} {}x{}{} ({})", t,
                rsx->translate(vt.offset, vt.location).value_or(0), vt.base_format(), vt.width, vt.height,
                vt.enabled ? "" : " disabled", vt.enabled ? bind_source : "-");
        }
    }

    if (tracing) {
        trace_targets_of(targets);
        const BlendInfo b = state.blend();
        const ColorMaskInfo cm = state.color_mask();
        u32 vertices = 0;
        for (const DrawRange& r : d.ranges) vertices += r.count;
        vertices += static_cast<u32>(d.array_elements.size()) + d.immediate_vertex_count;
        std::string blend = "off";
        if (b.enable || b.enable_mrt) {
            blend = std::format("eq {:#x}/{:#x} src {:#x}/{:#x} dst {:#x}/{:#x} mrt {:#x}", b.equation_rgb, b.equation_a,
                b.sfactor_rgb, b.sfactor_a, b.dfactor_rgb, b.dfactor_a, b.enable_mrt);
        }
        const std::string zs = !ds.depth_test ? std::string("off")
            : std::format("{:#x}{}", ds.depth_func, ds.depth_write ? "" : " no write");
        PS_LOG_INFO("rsx.trace", "#{} draw prim {} x{}{} ->{}; fp {:#010x} ({:016x}) vp {:#x}; mask {:x}{:x}{:x}{:x}; z {}{}; blend {};{}",
            ++trace_count, static_cast<u32>(d.primitive), vertices,
            d.inline_words.empty() ? std::string() : std::format(" (inline array, {} words)", d.inline_words.size()),
            describe_targets(targets), fp_ea, fprog ? fprog->hash : 0, state.reg(m::NV4097_SET_TRANSFORM_PROGRAM_START), cm.mask[0], cm.mask[1],
            cm.mask[2], cm.mask[3], zs,
            (ds.stencil_test ? std::format("; stencil {:#x} ref {:#x} mask {:#x} write {:#x} ops {:#x}/{:#x}/{:#x}{}",
                                   ds.front.func, ds.front.ref, ds.front.func_mask, ds.front.write_mask, ds.front.op_fail,
                                   ds.front.op_zfail, ds.front.op_zpass, ds.two_sided_stencil ? " two-sided" : "")
                             : std::string()) +
                (raster.poly_offset_fill ? std::format("; offset {} {}", raster.poly_offset_scale, raster.poly_offset_bias)
                                         : std::string()) +
                (raster.cull_enable ? std::format("; cull {:#x}", raster.cull_face) : std::string()) +
                (zpass_on ? std::string("; counted") : std::string()),
            blend, trace_tex);
    }

    // ---- vertex parameters: viewport transform into Metal NDC for this surface
    // (RPCS3 RSXDrawCommands.cpp fill_scale_offset_data, y flipped for Metal's y-up NDC) ----
    shader::MslVsParams vs{};
    // In pixels: normalized device coordinates cover the target whatever its samples (D-250).
    const float cw = static_cast<float>(pix_w) / 2.f, ch = static_cast<float>(pix_h) / 2.f;
    vs.pos_x[0] = vpi.scale[0] / cw;
    vs.pos_x[3] = (vpi.offset[0] - cw) / cw;
    vs.pos_y[1] = -vpi.scale[1] / ch;
    vs.pos_y[3] = -(vpi.offset[1] - ch) / ch;
    // Depth range (D-258): the RSX clips window z to [NV4097_SET_CLIP_MIN, _MAX] and stores it as it is. z goes from
    // that range to [0, 1] here, where Metal clips, and the viewport's depth range takes it back (RPCS3
    // VKGSRender.cpp set_viewport with an unrestricted depth range, RSXVertexPrologue.glsl apply_zclip_xform). Metal's
    // depth range stays inside [0, 1].
    const auto [z_near, z_far] = depth_range(vpi);
    const float z_span = z_far - z_near;
    if (z_span > 1e-6f) {
        vs.pos_z[2] = vpi.scale[2] / z_span;
        vs.pos_z[3] = (vpi.offset[2] - z_near) / z_span;
    } else {
        vs.pos_z[2] = vs.pos_z[3] = 0.f; // an empty range: every depth is its near end, as RPCS3 has it
    }
    vs.base_index = state.vertex_data_base_index();
    vs.restart_enable = raster.restart_index_enable;
    vs.branch_bits = state.transform_branch_bits(); // BRB and CLB (D-113)
    // The point size: the register, or the program's when the output mask says so (negative, D-262).
    vs.point_size = (state.reg(m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK) & (1u << 5)) ? -std::abs(raster.point_size)
                                                                                   : std::abs(raster.point_size);
    // A multisampled target is drawn one texel per sample (D-250): a point's pixels are that many texels (D-275). A
    // point is square, so with two samples across and one down it takes the larger count.
    {
        const AaFactors point_aa = aa_factors(draw_surface.antialias);
        vs.point_scale = static_cast<float>(std::max(point_aa.x, point_aa.y));
    }
    vs.restart_index = raster.restart_index;

    // ---- vertex and index data: raw guest bytes, decoded by the vertex shader (vertex pulling) ----
    // The draw's lists, kept from draw to draw (D-956).
    auto& ranges = draw_scratch.ranges;
    auto& index_offsets = draw_scratch.index_offsets; // per range, for pulled indices
    auto& restarts = draw_scratch.restarts; // per indexed range: where its restart indices are (D-249)
    auto& restart_begin = draw_scratch.restart_begin;
    auto& drawn_indices = draw_scratch.drawn_indices; // per indexed range
    restart_begin.push_back(0);
    TransientAllocator::Allocation vertex_data{}, index_data{};
    u32 min_vi = ~0u, max_vi = 0;
    // EDGE geometry on the GPU (D-988): the job's work item when the index array starts at its ring allocation; its
    // vertices and indices come from the kernels, its count from their indirect arguments.
    std::unique_ptr<edge::WorkItem> edge_item;
    EdgeDraw edge_draw;
    bool edge = false; // this draw is EDGE's: its item queued, its draw indirect
    const u32 inputs = vp_inputs & state.vertex_attrib_input_mask();
    auto guest_ok = [&](u32 ea, u32 size) { return size != 0 && mem.is_committed(ea) && mem.is_committed(ea + size - 1); };
    auto register_value = [&](shader::MslAttrib& at, u32 i) {
        at = {};
        at.flags = shader::kAttribRegister;
        std::memcpy(at.value, state.vertex_registers[i].value.data(), 16);
    };

    switch (d.command) {
    case DrawCommand::Array:
        vs.index_mode = shader::kIndexNone;
        for (const DrawRange& r : d.ranges) {
            ranges.push_back({r.first, r.count});
            min_vi = std::min(min_vi, r.first);
            max_vi = std::max(max_vi, r.first + r.count - 1);
        }
        break;
    case DrawCommand::Indexed: {
        const IndexArrayInfo ia = state.index_array();
        if (edge_pipeline && ia.u16_indices && !d.ranges.empty() && d.ranges[0].first == 0) {
            if (const auto ea = rsx->translate(ia.offset, ia.location, 2)) edge_item = edge_take(*ea);
        }
        if (edge_item && edge_item->guest_index_bytes) {
            // verify: the job's own output is drawn; the GPU's is made beside it and compared (D-988)
            u32 drawn = 0;
            for (const DrawRange& r : d.ranges) drawn += r.count;
            edge_verify(std::move(edge_item), drawn);
        }
        if (edge_item) {
            // The job's hole draws every index from the first (the culling's stand-in kept them all): one range, its
            // count the kernels', the vertices all of the job's.
            u32 count = 0;
            bool contiguous = true;
            for (const DrawRange& r : d.ranges) {
                contiguous &= r.first == count;
                count += r.count;
            }
            const auto& j = *reinterpret_cast<const ps_edge_job*>(edge_item->blob.data());
            if (!contiguous || count > j.num_indexes || j.num_vertexes == 0) {
                PS_LOG_WARN(kChan, "EDGE draw at {:#x}: ranges not one run from 0 ({} indices, the job's {}): drawn from "
                    "the GPU's indices all the same", edge_item->alloc_ea, count, j.num_indexes);
            }
            vs.index_mode = shader::kIndexU16BE;
            min_vi = vs.base_index & 0xfffff;
            max_vi = (vs.base_index + std::max<u32>(j.num_vertexes, 1) - 1) & 0xfffff;
            ranges.push_back({0, count});
            index_offsets.push_back(0);
            restart_begin.push_back(0);
            drawn_indices.push_back({true, 0, 2}); // its offset once the allocation is made
            stats_edge.draws++;
            edge = true;
            break;
        }
        const u32 isize = ia.u16_indices ? 2 : 4;
        // The restart index is compared with the index as stored, before the base index (RPCS3 upload_untouched); a
        // 16-bit index never equals one above 0xFFFF.
        const bool restart_on = raster.restart_index_enable && (isize == 4 || raster.restart_index <= 0xffff);
        vs.index_mode = ia.u16_indices ? shader::kIndexU16BE : shader::kIndexU32BE;
        u32 total = 0;
        for (const DrawRange& r : d.ranges) total += (r.count * isize + 3) & ~3u; // each range 4-byte aligned (D-271)
        index_data = transient.alloc(total, 16);
        u32 at = 0;
        for (const DrawRange& r : d.ranges) {
            const u32 bytes = r.count * isize;
            const auto ea = rsx->translate(ia.offset + r.first * isize, ia.location, bytes);
            if (!ea || !guest_ok(*ea, bytes)) return skip(std::format("index array at {:#x} is not mapped", ia.offset));
            before_access(*ea, bytes, false); // a deferred copy to it lands first (D-107)
            u8* dst = index_data.data + at;
            std::memcpy(dst, mem.host(*ea), bytes);
            bool has_all_ones = false;
            if (fast(kFastIndexScan)) { // in NEON (D-957)
                IndexScan scan{min_vi, max_vi, false};
                scan_indices_be(dst, r.count, isize, vs.base_index, restart_on, raster.restart_index, scan, restarts);
                min_vi = scan.min_vi;
                max_vi = scan.max_vi;
                has_all_ones = scan.has_all_ones;
            } else {
                const u32 all_ones = isize == 2 ? 0xffffu : 0xffffffffu;
                for (u32 i = 0; i < r.count; i++) {
                    const u32 raw = isize == 2 ? ps_ld16(dst + i * 2) : ps_ld32(dst + i * 4);
                    has_all_ones |= raw == all_ones;
                    if (restart_on && raw == raster.restart_index) {
                        restarts.push_back(i); // not a vertex: it must not widen the vertex range
                        continue;
                    }
                    const u32 vi = (raw + vs.base_index) & 0xfffff;
                    min_vi = std::min(min_vi, vi);
                    max_vi = std::max(max_vi, vi);
                }
            }
            const bool no_restarts = restarts.size() == restart_begin.back();
            restart_begin.push_back(static_cast<u32>(restarts.size()));
            // Metal runs it over these very bytes (D-271), unless an index of all ones would restart a strip there.
            drawn_indices.push_back({no_restarts && !has_all_ones, index_data.offset + at, isize});
            ranges.push_back({r.first, r.count});
            index_offsets.push_back(index_data.offset + at - r.first * isize); // wraps; so does the shader
            at += (bytes + 3) & ~3u;
        }
        break;
    }
    case DrawCommand::None:
        if (!d.array_elements.empty()) {
            // ARRAY_ELEMENT16/32: immediate indices into the vertex arrays (host order)
            vs.index_mode = shader::kIndexU32Host;
            const u32 n = static_cast<u32>(d.array_elements.size());
            index_data = transient.alloc(n * 4, 16);
            std::memcpy(index_data.data, d.array_elements.data(), n * 4);
            for (u32 e : d.array_elements) {
                const u32 vi = (e + vs.base_index) & 0xfffff;
                min_vi = std::min(min_vi, vi);
                max_vi = std::max(max_vi, vi);
            }
            ranges.push_back({0, n});
            index_offsets.push_back(index_data.offset);
        } else {
            // Immediate mode (SET_VERTEX_DATA*_M between BEGIN and END): float4 per vertex per attribute.
            const u32 n = d.immediate_vertex_count;
            vertex_data = transient.alloc(n * 16 * kVertexAttributeCount, 16);
            for (u32 i = 0; i < kVertexAttributeCount; i++) {
                std::memcpy(vertex_data.data + i * n * 16, d.immediate[i].data(), n * 16);
                vs.attribs[i] = {vertex_data.offset + i * n * 16, 16, static_cast<u32>(VertexType::F), 4, 0, 0, 0, 0, {}};
            }
            vs.index_mode = shader::kIndexNone;
            ranges.push_back({0, n});
        }
        break;
    case DrawCommand::InlineArray: {
        // RPCS3 nv4097.cpp draw_inline_array: interleaved vertices of every enabled array, in attribute order, as
        // big-endian words.
        u32 stride = 0;
        std::array<u32, kVertexAttributeCount> attr_offset{};
        for (u32 i = 0; i < kVertexAttributeCount; i++) {
            const VertexArrayInfo a = state.vertex_array(i);
            if (!a.enabled()) continue;
            attr_offset[i] = stride;
            stride += vertex_attribute_bytes(a.type, a.size);
        }
        if (stride == 0) return skip("inline array draw with no enabled vertex array");
        const u32 bytes = static_cast<u32>(d.inline_words.size() * 4);
        vertex_data = transient.alloc(bytes, 16);
        for (usize i = 0; i < d.inline_words.size(); i++) ps_st32(vertex_data.data + i * 4, d.inline_words[i]);
        for (u32 i = 0; i < kVertexAttributeCount; i++) {
            const VertexArrayInfo a = state.vertex_array(i);
            if (a.enabled()) {
                vs.attribs[i] = {vertex_data.offset + attr_offset[i], stride, static_cast<u32>(a.type), a.size, 0,
                    shader::kAttribSwap, 0, 0, {}};
            } else {
                register_value(vs.attribs[i], i);
            }
        }
        vs.index_mode = shader::kIndexNone;
        ranges.push_back({0, bytes / stride});
        break;
    }
    }

    if (!vertex_data.buffer) {
        // Arrays (plain, indexed or by element): copy each attribute's referenced range of guest memory.
        skip_reason = "no vertices";
        if (ranges.empty()) return skip("");
        if (max_vi - min_vi > 0x100000) return skip(std::format("vertex range {}..{} is too large", min_vi, max_vi));
        using Copy = DrawScratch::Copy;
        auto& copies = draw_scratch.copies;
        u32 total = 0;
        struct EdgeAttr {
            u32 attr, rel; // the attribute, its first element's offset in the allocation
        };
        std::array<EdgeAttr, kVertexAttributeCount> edge_attrs{};
        u32 edge_attr_count = 0;
        const u32 freq_mask = state.frequency_divider_mask();
        for (u32 i = 0; i < kVertexAttributeCount; i++) {
            const VertexArrayInfo a = state.vertex_array(i);
            if (!(inputs & (1u << i)) || !a.enabled()) {
                register_value(vs.attribs[i], i);
                continue;
            }
            // Element range: stride 0 = one element; frequency > 1 divides or (frequency divider) wraps the index.
            const bool modulo = (freq_mask >> i) & 1;
            u32 first = min_vi, last = max_vi;
            if (a.stride == 0) {
                first = last = 0;
            } else if (a.frequency > 1) {
                if (modulo) {
                    first = 0;
                    last = std::min<u32>(max_vi, a.frequency - 1u);
                } else {
                    first = min_vi / a.frequency;
                    last = max_vi / a.frequency;
                }
            }
            const u32 bytes = (last - first) * a.stride + vertex_attribute_bytes(a.type, a.size);
            const u32 base = (state.vertex_data_base_offset() + a.offset) & 0x0fffffff; // rsx_utils.h
            const auto ea = rsx->translate(base + first * a.stride, a.location, bytes);
            vs.attribs[i] = {0, a.stride, static_cast<u32>(a.type), a.size, a.frequency,
                shader::kAttribSwap | (modulo ? shader::kAttribModulo : 0u), first, 0, {}};
            if (edge_item && ea && *ea - edge_item->alloc_ea < edge_item->alloc_size) {
                // An EDGE output stream: the kernels write it into the allocation's shadow (D-988).
                edge_attrs[edge_attr_count++] = {i, *ea - edge_item->alloc_ea};
                continue;
            }
            if (!ea || !guest_ok(*ea, bytes)) return skip(std::format("vertex array {} at {:#x} is not mapped", i, base));
            copies.push_back({i, *ea, bytes});
        }
        // Interleaved attributes read overlapping ranges of one buffer (position, normal, texture coordinates in one
        // vertex): each overlapping or touching group is copied once, and its attributes point into that copy
        // (D-211). The shader reads bytes at any offset.
        std::sort(copies.begin(), copies.end(), [](const Copy& x, const Copy& y) { return x.ea < y.ea; });
        using Span = DrawScratch::Span;
        auto& spans = draw_scratch.spans;
        for (const Copy& c : copies) {
            if (!spans.empty() && c.ea <= spans.back().end) spans.back().end = std::max(spans.back().end, c.ea + c.size);
            else spans.push_back({c.ea, c.ea + c.size, 0});
        }
        for (Span& sp : spans) {
            sp.offset = total;
            total += (sp.end - sp.ea + 15) & ~15u;
        }
        const u32 edge_end = edge_item ? edge_layout(*edge_item, total, edge_draw) : 0;
        vertex_data = transient.alloc(std::max<u32>(std::max(total, edge_end), 16), 16);
        if (edge_item) {
            edge_queue(*edge_item, vertex_data, edge_draw);
            edge::recycle(std::move(edge_item)); // its blob is in the allocation now
            for (u32 k = 0; k < edge_attr_count; k++)
                vs.attribs[edge_attrs[k].attr].offset = vertex_data.offset + edge_draw.shadow + edge_attrs[k].rel;
            drawn_indices[0].offset = vertex_data.offset + edge_draw.indices;
        }
        for (const Span& sp : spans) {
            before_access(sp.ea, sp.end - sp.ea, false);
            std::memcpy(vertex_data.data + sp.offset, mem.host(sp.ea), sp.end - sp.ea);
            stats.vertex_bytes_copied += sp.end - sp.ea;
        }
        for (const Copy& c : copies) {
            const Span& sp = *std::prev(std::upper_bound(spans.begin(), spans.end(), c.ea,
                [](u32 ea, const Span& x) { return ea < x.ea; }));
            vs.attribs[c.attr].offset = vertex_data.offset + sp.offset + (c.ea - sp.ea);
        }
    }
    if (!index_data.buffer) index_data = vertex_data; // bound but unused

    // The transform constants, uploaded again only after a write to one or a new command buffer (D-129): copying all
    // 7.5 KB for every draw was the RSX thread's biggest copy on the iPhone (1,000 and more draws a frame). Only those
    // the vertex program reads are copied (D-278): Assassin's Creed writes one between nearly every two draws. The
    // upload keeps their places (c[n] at n * 16), so the program reads them as before; the rest of it is not written.
    {
        const DecodedVertexProgram& decoded = vertex_programs.front(); // the program vertex_program_for found
        const u16 lo = decoded.const_lo, hi = decoded.const_hi;
        const bool reads = lo <= hi;
        if (!consts_upload.buffer || consts_generation != transient.generation() ||
            (reads && ((state.dirty_bits & dirty::kVertexConstants) || lo < consts_lo || hi > consts_hi))) {
            const u16 copy_lo = reads ? lo : 0, copy_hi = reads ? hi : 0;
            consts_upload = transient.alloc((copy_hi + 1u) * 16u, 16);
            std::memcpy(consts_upload.data + copy_lo * 16u, state.transform_constants.data() + copy_lo,
                (copy_hi - copy_lo + 1u) * 16u);
            stats.vertex_constant_bytes_copied += (copy_hi - copy_lo + 1u) * 16u;
            consts_generation = transient.generation();
            consts_lo = copy_lo;
            consts_hi = copy_hi;
            state.dirty_bits &= ~dirty::kVertexConstants;
        }
    }
    const TransientAllocator::Allocation& consts = consts_upload;

    // Fragment constants: patched by games between draws, so read every draw.
    auto& fconst = draw_scratch.fconst;
    shader::read_fragment_constants(*fprog, mem.host(fp_ea), fconst);
    if (fconst.empty()) fconst.push_back({0.f, 0.f, 0.f, 0.f});
    const ShaderWindow win = state.shader_window();
    if (win.origin_bottom) {
        fs.wpos[0] = -1.f;
        fs.wpos[1] = static_cast<float>(win.height);
    }
    if (win.pixel_center_integer) fs.wpos[2] = -0.5f;
    fs.aa = aa.x | (aa.y << 8); // WPOS in pixels on a multisampled target (D-276)
    fs.alpha_func = raster.alpha_func;
    // The reference as the colour format reads it (D-264; RPCS3 rsx_methods.h alpha_ref): a float for 32-bit float
    // targets, a half for 16-bit ones, else the low byte as unorm.
    fs.alpha_ref = colour_format == static_cast<u32>(SurfaceColorFormat::F_X32) ||
                           colour_format == static_cast<u32>(SurfaceColorFormat::F_W32Z32Y32X32)
                       ? bits_to_float(raster.alpha_ref)
                   : colour_format == static_cast<u32>(SurfaceColorFormat::F_W16Z16Y16X16)
                       ? half_to_float(static_cast<u16>(raster.alpha_ref & 0xffff))
                       : static_cast<float>(raster.alpha_ref & 0xff) / 255.f;
    // FOGC's fog factor (D-256; RPCS3 RSXDrawCommands.cpp fill_fragment_state_buffer).
    fs.fog_mode = state.reg(m::NV4097_SET_FOG_MODE);
    fs.fog_param[0] = state.reg_f(m::NV4097_SET_FOG_PARAMS);
    fs.fog_param[1] = state.reg_f(m::NV4097_SET_FOG_PARAMS + 1);

    // The depth bounds test (D-261; RPCS3 VKDraw.cpp): a fragment whose pixel's stored depth is outside the bounds
    // goes. Metal has no such test: the shader reads the depth buffer as it was before the draw, a copy made again
    // only after depth was written (as D-148's copies of a target a draw samples).
    id<MTLTexture> bounds_texture = nil;
    if (fo.depth_bounds) {
        bounds_texture = feedback_copy(*targets.depth, true);
        fs.depth_bounds[0] = bounds.depth_bounds_min;
        fs.depth_bounds[1] = bounds.depth_bounds_max;
        stats.depth_bounds_draws++;
    }

    // ---- encode ----
    open_pass(targets);
    if (pass_started && clears_waiting_on_pass) { // the pass draws on past clears of other targets (D-945)
        stats.clears_waited += clears_waiting_on_pass;
        clears_waiting_on_pass = 0;
    }
    const ColorMaskInfo cm = state.color_mask();
    id<MTLRenderPipelineState> pso = interpret ? nil : pipeline_for(*vp, *fp, draw_blend, cm);
    skip_reason = compile_pending ? "pipeline state compiling" : "pipeline state";
    if (!interpret && !pso) {
        if (!(compile_pending && interpreter_on())) {
            if (compile_pending) stats.draws_compiling++;
            return skip("");
        }
        // Programs there, the pipeline still compiling: the interpreter would blend a signed equation as add, so the
        // draw waits here too (D-1026; the gate's loaded run drew it through this path, D-1071).
        if (shader_blend && !force_interpreter) {
            skip_reason = "signed blending waits for the pipeline";
            stats.draws_compiling++;
            return skip("");
        }
        interpret = true;
        shader_blend = 0;
    }
    if (shader_blend) stats.shader_blend_draws++;
    // The interpreter's functions, the programs encoded for it (once each) and its pipeline for these targets (D-940).
    id<MTLBuffer> ivp_code = nil, ifp_code = nil;
    shader::InterpVsOptions ivo{};
    shader::InterpFsOptions ifo{};
    if (interpret) {
        const u64 vkey = interp_vp->hash ^ (u64{interp_vp->start} * 0x9e3779b97f4a7c15ull) ^ 0x5650ull;
        const u64 fkey = fprog->hash ^ (u64{fprog->ucode_length} << 40) ^ 0x4650ull;
        if (interp_refused.contains(vkey) || interp_refused.contains(fkey)) return skip("");
        ivp_code = interp_code.contains(vkey) ? interp_code[vkey] : nil;
        if (!ivp_code) {
            const shader::EncodeResult e = shader::encode_vertex_program(*interp_vp);
            if (!e.ok) {
                interp_refused.insert(vkey);
                return skip(e.error);
            }
            ivp_code = interpreter_code(vkey, e.words);
        }
        ifp_code = interp_code.contains(fkey) ? interp_code[fkey] : nil;
        if (!ifp_code) {
            const shader::EncodeResult e = shader::encode_fragment_program(*fprog);
            if (!e.ok) {
                interp_refused.insert(fkey);
                return skip(e.error);
            }
            ifp_code = interpreter_code(fkey, e.words);
        }
        ivo = shader::interp_vs_options(*interp_vp, interp_vp_options);
        ivo.count = static_cast<u32>(ivp_code.length / (shader::kVpWords * 4));
        ifo = shader::interp_fs_options(fo);
        const CompiledProgram* ivs = interpreter_program(true, 1, false);
        // Its fragment function can discard: under the depth clamp it exports the depth too (D-941).
        const CompiledProgram* ifs =
            interpreter_program(false, std::min<u32>(fo.mrt_count, 4), (fo.shader_control & 0xe) != 0 || fo.clamp_depth);
        skip_reason = "shader interpreter compiling";
        if (!ivs || !ifs) {
            stats.draws_compiling++;
            return skip("");
        }
        pso = pipeline_for(*ivs, *ifs, draw_blend, cm);
        if (!pso) {
            stats.draws_compiling++;
            return skip("");
        }
        stats.draws_interpreted++;
    }
    id<MTLRenderCommandEncoder> enc = start_encoder();
    if (fprog) fp_draws[fprog->hash]++; // the draw signature (D-1029)
    // Occlusion counting (D-102): this pass's counter for the current query.
    if (zpass_on) {
        if (pass_zpass_slot < 0 && visibility_next < visibility_slots) {
            pass_zpass_slot = static_cast<s32>(visibility_next++);
            zpass_slots.push_back({visibility, static_cast<u32>(pass_zpass_slot)});
        } else if (pass_zpass_slot < 0 && rsx) {
            rsx->alarms().add(Alarm::OcclusionOverflow); // D-1022: the query counts nothing in this pass
        }
        if (pass_zpass_slot >= 0 && encoder_zpass_slot != pass_zpass_slot) {
            [enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:static_cast<NSUInteger>(pass_zpass_slot) * 8];
            encoder_zpass_slot = pass_zpass_slot;
        }
    } else if (encoder_zpass_slot >= 0) {
        [enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        encoder_zpass_slot = -1;
    }
    // The encoder's state, each part set only when it changes (D-954).
    EncoderState& es = enc_state;
    if (!fast(kFastSetters) || es.encoder != enc) {
        forget_encoder_state();
        es.encoder = enc;
    }
    if (es.pso != pso) {
        [enc setRenderPipelineState:pso];
        es.pso = pso;
    }
    // PSPOCKET_RSX_TRACE_ZALWAYS_FP=HASH (a debugging aid, D-1028): in a traced frame, draws with the fragment program HASH
    // pass the depth test always and write their depth, so the probe shows where they land and at what depth.
    static const u64 zalways_fp = [] {
        const char* v = std::getenv("PSPOCKET_RSX_TRACE_ZALWAYS_FP");
        return v ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    DepthStencilInfo ds_bound = ds;
    if (tracing && zalways_fp && fprog && fprog->hash == zalways_fp && ds.depth_test) {
        ds_bound.depth_func = static_cast<u32>(CompareFunc::Always);
        ds_bound.depth_write = true;
    }
    if (id<MTLDepthStencilState> depth = depth_state_for(ds_bound); es.depth != depth) {
        [enc setDepthStencilState:depth];
        es.depth = depth;
    }
    if (es.stencil_front != ds.front.ref || es.stencil_back != ds.back.ref) {
        [enc setStencilFrontReferenceValue:ds.front.ref backReferenceValue:ds.back.ref];
        es.stencil_front = ds.front.ref;
        es.stencil_back = ds.back.ref;
    }
    const MTLViewport viewport{0, 0, static_cast<double>(surf_w), static_cast<double>(surf_h), static_cast<double>(z_near),
        static_cast<double>(z_far)};
    if (!es.viewport_set || std::memcmp(&es.viewport, &viewport, sizeof(viewport)) != 0) {
        [enc setViewport:viewport];
        es.viewport = viewport;
        es.viewport_set = true;
    }
    // NV4097_SET_ZMIN_MAX_CONTROL: depth clamped rather than clipped (RPCS3 VKGSRender.cpp: clamp, or no clip).
    const MTLDepthClipMode clip = raster.depth_clamp || !raster.depth_clip ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
    if (es.clip_mode != static_cast<s32>(clip)) {
        [enc setDepthClipMode:clip];
        es.clip_mode = static_cast<s32>(clip);
    }
    const MTLScissorRect scissor{sx0 * aa.x, sy0 * aa.y, (sx1 - sx0) * aa.x, (sy1 - sy0) * aa.y};
    if (!es.scissor_set || es.scissor.x != scissor.x || es.scissor.y != scissor.y || es.scissor.width != scissor.width ||
        es.scissor.height != scissor.height) {
        [enc setScissorRect:scissor];
        es.scissor = scissor;
        es.scissor_set = true;
    }
    const MTLCullMode cull = !raster.cull_enable                                           ? MTLCullModeNone
                           : static_cast<CullFace>(raster.cull_face) == CullFace::Front ? MTLCullModeFront
                                                                                        : MTLCullModeBack;
    if (es.cull != static_cast<s32>(cull)) {
        [enc setCullMode:cull];
        es.cull = static_cast<s32>(cull);
    }
    // Facing is judged in window coordinates with memory row 0 at the top, in Metal as for the RSX (the viewport
    // transform keeps that orientation), so the winding passes through.
    const MTLWinding winding =
        raster.front_face == static_cast<u32>(FrontFace::CW) ? MTLWindingClockwise : MTLWindingCounterClockwise;
    if (es.winding != static_cast<s32>(winding)) {
        [enc setFrontFacingWinding:winding];
        es.winding = static_cast<s32>(winding);
    }
    const MTLTriangleFillMode fill = raster.front_polygon_mode == static_cast<u32>(PolygonMode::Line)
        ? MTLTriangleFillModeLines
        : MTLTriangleFillModeFill;
    if (es.fill != static_cast<s32>(fill)) {
        [enc setTriangleFillMode:fill];
        es.fill = static_cast<s32>(fill);
    }
    const float bias = raster.poly_offset_fill ? raster.poly_offset_bias : 0.f;
    const float slope = raster.poly_offset_fill ? raster.poly_offset_scale : 0.f;
    if (!es.bias_set || std::bit_cast<u32>(es.bias) != std::bit_cast<u32>(bias) ||
        std::bit_cast<u32>(es.slope) != std::bit_cast<u32>(slope)) {
        [enc setDepthBias:bias slopeScale:slope clamp:0];
        es.bias = bias;
        es.slope = slope;
        es.bias_set = true;
    }
    const BlendInfo& blend = draw_blend;
    if (!es.blend_color_set || es.blend_color != blend.color) {
        [enc setBlendColorRed:((blend.color >> 16) & 0xff) / 255.f
                        green:((blend.color >> 8) & 0xff) / 255.f
                         blue:(blend.color & 0xff) / 255.f
                        alpha:(blend.color >> 24) / 255.f];
        es.blend_color = blend.color;
        es.blend_color_set = true;
    }
    const auto vertex_buffer = [&](id<MTLBuffer> buffer, NSUInteger offset, u32 slot) {
        if (es.vbuf[slot] == buffer && es.voff[slot] == offset) return;
        [enc setVertexBuffer:buffer offset:offset atIndex:slot];
        es.vbuf[slot] = buffer;
        es.voff[slot] = offset;
    };
    vertex_buffer(vertex_data.buffer, 0, shader::slot::kVsVertexData);
    vertex_buffer(index_data.buffer, 0, shader::slot::kVsIndexData);
    vertex_buffer(consts.buffer, consts.offset, shader::slot::kVsConstants);
    static constexpr std::array<float, 4> kNoConstants{}; // the interpreter reads the buffer by index: always bound
    {
        const void* fc = fconst.empty() ? static_cast<const void*>(kNoConstants.data()) : fconst.data();
        const usize fc_bytes = fconst.empty() ? sizeof(kNoConstants) : fconst.size() * 16;
        if (es.fconst.size() != fc_bytes || std::memcmp(es.fconst.data(), fc, fc_bytes) != 0) {
            [enc setFragmentBytes:fc length:fc_bytes atIndex:shader::slot::kFsConstants];
            es.fconst.assign(static_cast<const u8*>(fc), static_cast<const u8*>(fc) + fc_bytes);
        }
    }
    if (!es.fs_set || std::memcmp(es.fs.data(), &fs, sizeof(fs)) != 0) {
        [enc setFragmentBytes:&fs length:sizeof(fs) atIndex:shader::slot::kFsParams];
        std::memcpy(es.fs.data(), &fs, sizeof(fs));
        es.fs_set = true;
    }
    if (interpret) {
        // The interpreter's programs and options; its textures by kind (a unit's at its kind's slots, D-940), the
        // other slots 1x1 stand-ins of their type, every sampler and the stipple pattern bound (it reads by index).
        [enc setVertexBuffer:ivp_code offset:0 atIndex:shader::islot::kVsProgram];
        [enc setVertexBytes:&ivo length:sizeof(ivo) atIndex:shader::islot::kVsOptions];
        [enc setFragmentBuffer:ifp_code offset:0 atIndex:shader::islot::kFsProgram];
        [enc setFragmentBytes:&ifo length:sizeof(ifo) atIndex:shader::islot::kFsOptions];
        std::array<id<MTLTexture>, shader::islot::kFsDepthBounds + 1> ft;
        for (u32 t = 0; t < kFragmentTextureCount; t++) {
            ft[shader::islot::kFsTex2D + t] = dummy_texture;
            ft[shader::islot::kFsTexCube + t] = dummy_cube;
            ft[shader::islot::kFsTex3D + t] = dummy_volume;
            ft[shader::islot::kFsTexDepth + t] = dummy_depth;
            const shader::TextureKind k = fo.texture_kind[t];
            const u32 base = k == shader::TextureKind::Cube  ? shader::islot::kFsTexCube
                           : k == shader::TextureKind::Tex3D ? shader::islot::kFsTex3D
                           : k == shader::TextureKind::Depth2D || k == shader::TextureKind::DepthShadow2D
                               ? shader::islot::kFsTexDepth
                               : shader::islot::kFsTex2D;
            if (bound[t]) ft[base + t] = bound[t];
            [enc setFragmentSamplerState:sampler_for(frag_tex[t]) atIndex:t];
        }
        ft[shader::islot::kFsDepthBounds] = bounds_texture ? bounds_texture : dummy_depth;
        [enc setFragmentTextures:ft.data() withRange:NSMakeRange(0, ft.size())];
        static constexpr std::array<u32, 32> kNoStipple{};
        [enc setFragmentBytes:fo.polygon_stipple ? &state.regs[m::NV4097_SET_POLYGON_STIPPLE_PATTERN] : kNoStipple.data()
                       length:32 * sizeof(u32) atIndex:shader::slot::kFsStipple];
        std::array<id<MTLTexture>, 12> vt;
        for (u32 t = 0; t < 4; t++) {
            vt[shader::islot::kVsTex2D + t] = dummy_texture;
            vt[shader::islot::kVsTexCube + t] = dummy_cube;
            vt[shader::islot::kVsTex3D + t] = dummy_volume;
            const shader::TextureKind k = interp_vp_options.texture_kind[t];
            const u32 base = k == shader::TextureKind::Cube ? shader::islot::kVsTexCube
                           : k == shader::TextureKind::Tex3D ? shader::islot::kVsTex3D
                                                             : shader::islot::kVsTex2D;
            if (vbound[t]) vt[base + t] = vbound[t];
            [enc setVertexSamplerState:sampler_for(vertex_texture(state, t)) atIndex:t];
        }
        [enc setVertexTextures:vt.data() withRange:NSMakeRange(0, vt.size())];
        // Its bindings are not the translation's: the next draw binds its own again.
        es.ftex = {};
        es.fsamp = {};
        es.bounds_tex = nil;
        es.vtex = {};
        es.vsamp = {};
    } else {
        for (u32 t = 0; t < kFragmentTextureCount; t++) {
            if (!bound[t]) continue;
            if (es.ftex[t] != bound[t]) {
                [enc setFragmentTexture:bound[t] atIndex:t];
                es.ftex[t] = bound[t];
            }
            if (id<MTLSamplerState> sampler = sampler_for(frag_tex[t]); es.fsamp[t] != sampler) {
                [enc setFragmentSamplerState:sampler atIndex:t];
                es.fsamp[t] = sampler;
            }
        }
        if (bounds_texture && es.bounds_tex != bounds_texture) {
            [enc setFragmentTexture:bounds_texture atIndex:shader::slot::kFsDepthBounds];
            es.bounds_tex = bounds_texture;
        }
        if (fo.polygon_stipple) {
            [enc setFragmentBytes:&state.regs[m::NV4097_SET_POLYGON_STIPPLE_PATTERN] length:32 * sizeof(u32)
                          atIndex:shader::slot::kFsStipple];
        }
        for (u32 t = 0; t < 4; t++) {
            if (!vbound[t]) continue;
            if (es.vtex[t] != vbound[t]) {
                [enc setVertexTexture:vbound[t] atIndex:t];
                es.vtex[t] = vbound[t];
            }
            if (id<MTLSamplerState> sampler = sampler_for(vertex_texture(state, t)); es.vsamp[t] != sampler) {
                [enc setVertexSamplerState:sampler atIndex:t];
                es.vsamp[t] = sampler;
            }
        }
    }

    MTLPrimitiveType ptype = MTLPrimitiveTypeTriangle;
    const bool native = native_primitive(d.primitive, ptype);
    const u32 pulled_mode = vs.index_mode;
    if (edge) {
        // EDGE's draw (D-988): over the indices the kernels kept, as many as their arguments say.
        vs.index_offset = 0;
        vs.index_mode = shader::kIndexDrawnU16BE;
        [enc setVertexBytes:&vs length:sizeof(vs) atIndex:shader::slot::kVsParams];
        if (native) {
            [enc drawIndexedPrimitives:ptype indexType:MTLIndexTypeUInt16 indexBuffer:index_data.buffer
                     indexBufferOffset:drawn_indices[0].offset indirectBuffer:vertex_data.buffer
                  indirectBufferOffset:vertex_data.offset + edge_draw.args];
        } else {
            unsupported(std::format("EDGE draw of primitive {}", static_cast<u32>(d.primitive)));
        }
        ranges.clear(); // drawn
    }
    for (usize r = 0; r < ranges.size(); r++) {
        if (r < index_offsets.size()) vs.index_offset = index_offsets[r];
        const DrawScratch::Range rg = ranges[r];
        // An indexed range Metal runs over the game's indices: each vertex they share runs once (D-271).
        const bool drawn = native && r < drawn_indices.size() && drawn_indices[r].ok;
        vs.index_mode = !drawn ? pulled_mode
                      : drawn_indices[r].size == 2 ? shader::kIndexDrawnU16BE : shader::kIndexDrawnU32BE;
        [enc setVertexBytes:&vs length:sizeof(vs) atIndex:shader::slot::kVsParams];
        if (drawn) {
            stats.ranges_indexed_by_metal++;
            [enc drawIndexedPrimitives:ptype indexCount:rg.count
                             indexType:drawn_indices[r].size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:index_data.buffer indexBufferOffset:drawn_indices[r].offset];
            continue;
        }
        const bool restart = r + 1 < restart_begin.size() && restart_begin[r + 1] > restart_begin[r];
        if (native && !restart) {
            [enc drawPrimitives:ptype vertexStart:rg.first vertexCount:rg.count];
            continue;
        }
        MTLPrimitiveType ctype = MTLPrimitiveTypeTriangle;
        auto& list = draw_scratch.list;
        list.clear();
        if (restart) { // positions, restarts (D-249)
            restart_list(d.primitive, rg.first, rg.count,
                std::span<const u32>(restarts).subspan(restart_begin[r], restart_begin[r + 1] - restart_begin[r]), ctype,
                list);
        } else {
            convert_primitive(d.primitive, rg.count, ctype, list, rg.first); // vertex_id = position in the range
        }
        if (list.empty()) continue;
        auto ib = transient.alloc(static_cast<u32>(list.size() * 4), 16);
        std::memcpy(ib.data, list.data(), list.size() * 4);
        [enc drawIndexedPrimitives:ctype indexCount:list.size() indexType:MTLIndexTypeUInt32 indexBuffer:ib.buffer
                 indexBufferOffset:ib.offset];
    }
    stats.draws++;
    // What the draw wrote is new (D-105): colour targets it does not mask out, the depth buffer when it writes depth or
    // stencil.
    for (u32 i = 0; i < targets.color_count; i++)
        if (cm.mask[targets.rsx_index[i]] != 0) targets.color[i]->generation++;
    if (targets.depth) {
        const DepthStencilInfo& z = ds;
        const bool stencil_writes = z.stencil_test && (z.front.write_mask || (z.two_sided_stencil && z.back.write_mask));
        if (z.depth_test && z.depth_write) targets.depth->generation++;
        if (stencil_writes) targets.depth->stencil_generation++;
    }
}

// ---- flips --------------------------------------------------------------------------------------------------------

void MetalBackend::Impl::do_flip(const FlipRequest& request)
{
    tick++;
    end_pass_for(kEndFlip);
    finish_closing(false); // the frame's stores are counted in it (D-945)
    const usize half = ab_half(); // the PSPOCKET_GPU_AB half of the frame that ends here
    if (ab_period) {
        // The RSX thread's CPU time of the frame that ends here, to the half its draws were in (D-278): with GPU time
        // alone a change on the CPU side could not be told within one run.
        const u64 now = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        if (ab_cpu_last) ab_cpu_ns[half] += now - ab_cpu_last;
        ab_cpu_last = now;
        // Its render passes and what they loaded and stored (D-945).
        ab_passes[half] += stats.render_passes - ab_last[0];
        ab_load_bytes[half] += stats.attachment_load_bytes - ab_last[1];
        ab_store_bytes[half] += stats.attachment_store_bytes - ab_last[2];
        ab_last = {stats.render_passes, stats.attachment_load_bytes, stats.attachment_store_bytes};
        ab_draws[half] += stats.draws - ab_last_draws;
        ab_last_draws = stats.draws;
    }
    stats.flips++;
    if (edge_ab_period) edge_ab_flip();
    if (pass_times) {
        pass_place = 0;
        if (stats.flips % 300 == 0) log_pass_times();
    }
    if (ab_period) {
        ab_frames[half]++; // the frame that ends here (D-272)
        if (stats.flips % (ab_period * 20) == 0) {
            const double off = static_cast<double>((*ab_ns)[0].exchange(0)), on = static_cast<double>((*ab_ns)[1].exchange(0));
            const auto per = [&](u64 v, usize h) { return ab_frames[h] ? static_cast<double>(v) / static_cast<double>(ab_frames[h]) : 0.0; };
            PS_LOG_INFO(kChan, "GPU A/B (PSPOCKET_GPU_AB): change off {:.3f} ms a frame ({} frames), on {:.3f} ms ({} frames); "
                "RSX thread CPU off {:.3f} ms a frame, on {:.3f} ms; render passes off {:.1f}, on {:.1f}; attachments "
                "loaded off {:.1f} MB, on {:.1f} MB, stored off {:.1f} MB, on {:.1f} MB",
                per(static_cast<u64>(off), 0) / 1e6, ab_frames[0], per(static_cast<u64>(on), 1) / 1e6, ab_frames[1],
                per(ab_cpu_ns[0], 0) / 1e6, per(ab_cpu_ns[1], 1) / 1e6, per(ab_passes[0], 0), per(ab_passes[1], 1),
                per(ab_load_bytes[0], 0) / 1048576.0, per(ab_load_bytes[1], 1) / 1048576.0,
                per(ab_store_bytes[0], 0) / 1048576.0, per(ab_store_bytes[1], 1) / 1048576.0);
            if (ab_cpu) {
                const auto us = [&](usize h) {
                    return ab_draws[h] ? static_cast<double>(ab_cpu_ns[h]) / 1e3 / static_cast<double>(ab_draws[h]) : 0.0;
                };
                PS_LOG_INFO(kChan, "RSX CPU A/B (PSPOCKET_RSX_CPU_AB): fast paths off {:.3f} us a draw ({:.0f} draws a "
                    "frame), on {:.3f} us a draw ({:.0f} draws a frame): {:+.1f}%; a frame off and on: fragment programs "
                    "{:.1f} and {:.1f} found in their lists, {:.1f} and {:.1f} not; vertex programs {:.1f} and {:.1f} "
                    "looked up, {:.1f} and {:.1f} decoded, {:.1f} and {:.1f} compared", us(0), per(ab_draws[0], 0), us(1),
                    per(ab_draws[1], 1), us(0) > 0 ? (us(1) / us(0) - 1) * 100 : 0.0, per(fp_variant_hits[0], 0),
                    per(fp_variant_hits[1], 1), per(fp_variant_misses[0], 0), per(fp_variant_misses[1], 1),
                    per(vp_lookups[0], 0), per(vp_lookups[1], 1), per(vp_decodes[0], 0), per(vp_decodes[1], 1),
                    per(vp_compares[0], 0), per(vp_compares[1], 1));
                fp_variant_hits = fp_variant_misses = vp_lookups = vp_decodes = vp_compares = {};
            }
            ab_frames = {};
            ab_cpu_ns = {};
            ab_draws = {};
            ab_passes = ab_load_bytes = ab_store_bytes = {};
        }
    }
    compile_wait_left_ns = static_cast<s64>(config.compile_wait_ms * 1e6); // each frame's budget (D-158)
    if (stats.flips % std::clamp<u64>(config.evict_after_flips / 2, 1, 150) == 0) evict_unused(config.evict_after_flips); // D-116
    trim_texture_pool(false); // D-956
    if (trim_requested.exchange(false, std::memory_order_acq_rel)) trim();
    if (tracing && request.flip_index >= config.trace_frame) {
        PS_LOG_INFO("rsx.trace", "frame {} ends: flip {} shows {:#010x}; {} clears and draws, {} colour targets",
            config.trace_frame, request.flip_index,
            request.buffer ? PS_MEM_RSX_LOCAL_BASE + request.buffer->offset : 0u, trace_count, trace_targets.size());
        trace_dump();
        tracing = false;
    }
    id<MTLTexture> source = nil;
    if (request.buffer) {
        const u32 ea = PS_MEM_RSX_LOCAL_BASE + request.buffer->offset;
        before_access(ea, request.buffer->pitch * request.buffer->height, false); // a deferred copy there lands first
        if (Surface* s = find_surface(ea, false); s && !s->cpu_newer) {
            source = s->texture;
        } else if (request.buffer->width && request.buffer->height) {
            // Not rendered by the GPU: show what the CPU put in guest memory (A8R8G8B8, linear).
            TextureInfo t;
            t.format = tex::A8R8G8B8 | tex::kLinearFlag;
            t.width = static_cast<u16>(request.buffer->width);
            t.height = static_cast<u16>(request.buffer->height);
            t.pitch = request.buffer->pitch;
            t.dimension = 2;
            t.mipmaps = 1;
            t.border_type = true;
            std::string why;
            if (CachedTexture* c = decode_texture(t, ea, false, &why)) source = c->texture;
        }
    }

    id<MTLTexture> output = outputs[next_output];
    next_output = (next_output + 1) % static_cast<u32>(outputs.size());
    id<MTLCommandBuffer> cb = render_buffer();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = output;
    rp.colorAttachments[0].loadAction = source ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    enc.label = @"rsx present";
    if (source) {
        [enc setRenderPipelineState:blit_pipeline];
        const float depth = 0.f;
        [enc setVertexBytes:&depth length:sizeof(depth) atIndex:0];
        const float params[4] = {1.f, 0.f, 0.f, 0.f}; // display buffers are X8R8G8B8: alpha is not shown
        [enc setFragmentBytes:params length:sizeof(params) atIndex:0];
        [enc setFragmentTexture:source atIndex:0];
        [enc setFragmentSamplerState:linear_sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];
    if (config.capture && config.capture_every && request.flip_index % config.capture_every == 0) {
        // Copy the output to a shared buffer and hand it over once the GPU has finished the frame.
        const u32 w = config.output_width, h = config.output_height;
        id<MTLBuffer> readback = [device newBufferWithLength:NSUInteger{w} * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:output
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:NSUInteger{w} * 4
        destinationBytesPerImage:NSUInteger{w} * h * 4];
        [blit endEncoding];
        CaptureCallback capture = config.capture;
        const u64 index = request.flip_index;
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
            capture(static_cast<const u8*>(readback.contents), w, h, index);
        }];
    }
    if (config.thumbnail) {
        // The frame as 64x36, every flip (D-1027): a box filter on the GPU into a small shared buffer.
        id<MTLBuffer> thumb = [device newBufferWithLength:kThumbnailWidth * kThumbnailHeight * 4
                                                  options:MTLResourceStorageModeShared];
        id<MTLComputeCommandEncoder> te = [cb computeCommandEncoder];
        [te setComputePipelineState:thumbnail_pipeline];
        [te setTexture:output atIndex:0];
        [te setBuffer:thumb offset:0 atIndex:0];
        const u32 size[4] = {kThumbnailWidth, kThumbnailHeight, 0, 0};
        [te setBytes:size length:sizeof(size) atIndex:1];
        [te dispatchThreads:MTLSizeMake(kThumbnailWidth, kThumbnailHeight, 1) threadsPerThreadgroup:MTLSizeMake(8, 4, 1)];
        [te endEncoding];
        ThumbnailCallback callback = config.thumbnail;
        const u64 index = request.flip_index;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.status == MTLCommandBufferStatusCompleted) callback(static_cast<const u8*>(thumb.contents), index);
        }];
    }
    frame_event_value++;
    [cb encodeSignalEvent:frame_event value:frame_event_value];
    if (trace_requested.exchange(false, std::memory_order_acq_rel) && !tracing) config.trace_frame = request.flip_index + 1;
    if (config.trace_frame && request.flip_index + 1 == config.trace_frame) {
        tracing = true;
        trace_count = 0;
        trace_targets.clear();
        PS_LOG_INFO("rsx.trace", "frame {} begins", config.trace_frame);
    }

    if (config.present) {
        PresentFrame frame;
        frame.texture = (__bridge void*)output;
        frame.command_buffer = (__bridge void*)cb;
        frame.shared_event = (__bridge void*)frame_event;
        frame.event_value = frame_event_value;
        frame.width = config.output_width;
        frame.height = config.output_height;
        frame.buffer_id = request.buffer_id;
        frame.flip_index = request.flip_index;
        config.present(frame);
    }
    // The frame's last buffer gives its place back when the GPU has run it (completion handlers run on error too).
    dispatch_semaphore_t slots = frames_in_flight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(slots); }];
    submit_half = static_cast<s32>(half); // its GPU time to the half of the frame it holds, not the next one's
    submit(false);
    submit_half = -1;
    // At most frames_in_flight frames queued (D-148): wait for the oldest before the RSX goes on to the next.
    const auto t0 = std::chrono::steady_clock::now();
    dispatch_semaphore_wait(frames_in_flight, DISPATCH_TIME_FOREVER);
    stats.gpu_wait_ns += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

// MetalBackend::trim_memory, at a flip (D-148): what can be made again goes, and readbacks the GPU has finished land.
void MetalBackend::Impl::trim()
{
    const usize textures_before = textures.size(), surfaces_before = surfaces.size();
    const u64 texture_mb = texture_bytes() >> 20, surface_mb = surface_bytes() >> 20;
    evict_unused(60);
    trim_texture_pool(true);
    surface_texture_pool.clear();
    for (auto& s : surfaces) {
        if (!draw_targets.contains(s.get()) && !(pass_open && pass.contains(s.get()))) s->feedback = {};
    }
    run_deferred(false);
    stats.memory_trims++;
    PS_LOG_WARN(kChan, "memory low: {} of {} textures ({} MB) and {} of {} surfaces ({} MB) freed, unused for 60 "
        "flips; the GPU holds {} MB", textures_before - textures.size(), textures_before,
        texture_mb - (texture_bytes() >> 20), surfaces_before - surfaces.size(), surfaces_before,
        surface_mb - (surface_bytes() >> 20), static_cast<u64>(device.currentAllocatedSize) >> 20);
}

// ---- MetalBackend ---------------------------------------------------------------------------------------------------

MetalBackend::MetalBackend(GuestMemory& mem, WriteTracker& tracker, MetalBackendConfig config)
    : impl_(std::make_unique<Impl>(mem, tracker, std::move(config)))
{
    @autoreleasepool {
        if (!impl_->init()) impl_->device = nil;
    }
}

MetalBackend::~MetalBackend()
{
    @autoreleasepool {
        if (impl_->device) impl_->submit(true);
        // Every queued frame's completion handler has given its place back before the semaphore goes (libdispatch
        // aborts on one released below its starting count).
        if (impl_->frames_in_flight) {
            for (u32 i = 0; i < impl_->frames_in_flight_limit; i++)
                dispatch_semaphore_wait(impl_->frames_in_flight, DISPATCH_TIME_FOREVER);
            for (u32 i = 0; i < impl_->frames_in_flight_limit; i++) dispatch_semaphore_signal(impl_->frames_in_flight);
        }
        for (auto& [key, t] : impl_->textures) impl_->tracker.unwatch(t->watch);
        for (auto& s : impl_->surfaces) impl_->tracker.unwatch(s->watch);
    }
}

MetalBackend::DrawSignature MetalBackend::draw_signature() const
{
    std::lock_guard lock(impl_->fp_draws_mutex);
    DrawSignature d;
    d.flips = impl_->fp_draws_flips;
    d.draws.assign(impl_->fp_draws_snapshot.begin(), impl_->fp_draws_snapshot.end());
    std::sort(d.draws.begin(), d.draws.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return d;
}

void MetalBackend::trim_memory() { impl_->trim_requested.store(true, std::memory_order_release); }
void MetalBackend::trace_next_frame() { impl_->trace_requested.store(true, std::memory_order_release); }

bool MetalBackend::valid() const { return impl_->device != nil; }
void* MetalBackend::device() const { return (__bridge void*)impl_->device; }
bool MetalBackend::supports_bc() const { return impl_->bc_supported; }
const BackendStats& MetalBackend::stats() const { return impl_->stats; }
const std::string& MetalBackend::last_error() const { return impl_->last_error; }

void MetalBackend::attach(Rsx& rsx) { impl_->rsx = &rsx; }

void MetalBackend::draw(const RsxState& state)
{
    if (!impl_->device) return;
    @autoreleasepool {
        impl_->do_draw(state);
        if (impl_->tracing) impl_->trace_probe(); // PSPOCKET_RSX_TRACE_PROBE (D-1028)
        // A frame's uploads stay allocated until the GPU has run its command buffer: past the budget it goes now, so
        // the GPU starts on it and gives it back sooner (D-148).
        if (impl_->transient.bytes_in_use() > impl_->config.transient_budget) {
            impl_->submit(false);
            impl_->stats.budget_submits++;
        }
    }
}

void MetalBackend::clear(u32 mask, const RsxState& state)
{
    if (!impl_->device) return;
    @autoreleasepool {
        impl_->do_clear(mask, state);
    }
}

void MetalBackend::flip(const FlipRequest& request, const RsxState&)
{
    if (!impl_->device) return;
    @autoreleasepool {
        impl_->do_flip(request);
        if (impl_->exact) impl_->exact_write_back(); // the CPU may read what the frame drew once it flipped (D-1029)
    }
    Impl& m = *impl_;
    if (++m.report_flips == Impl::kGpuReportFlips) {
        // The window's numbers (D-148): logged, and handed to config.on_window.
        const u64 gpu = m.gpu_ns->load(std::memory_order_relaxed);
        const double n = static_cast<double>(m.report_flips);
        const auto per_frame = [n](u64 now, u64 before) { return static_cast<double>(now - before) / n; };
        WindowStats w;
        w.flips = static_cast<u32>(m.report_flips);
        w.last_flip = m.stats.flips;
        w.gpu_ms = static_cast<double>(gpu - m.report_gpu_ns) / 1e6 / n;
        w.draws = per_frame(m.stats.draws, m.report_draws);
        w.render_passes = per_frame(m.stats.render_passes, m.report_passes);
        std::string ends;
        for (u32 i = 0; i < Impl::kEndCount; i++) {
            w.pass_ends[i] = per_frame(m.pass_ends[i], m.report_pass_ends[i]);
            if (w.pass_ends[i] > 0) ends += std::format("{}{} {:.1f}", ends.empty() ? "" : ", ", kPassEndNames[i], w.pass_ends[i]);
            m.report_pass_ends[i] = m.pass_ends[i];
        }
        w.readbacks = per_frame(m.stats.readbacks, m.report_readbacks);
        w.deferred_readbacks = per_frame(m.stats.deferred_readbacks, m.report_deferred);
        w.deferred_conflicts = per_frame(m.stats.deferred_conflicts, m.report_conflicts);
        w.readback_wait_ms = per_frame(m.stats.readback_wait_ns, m.report_readback_ns) / 1e6;
        w.texture_overlays = per_frame(m.stats.texture_overlays, m.report_overlays);
        w.texture_checks = per_frame(m.stats.texture_checks, m.report_texture_checks);
        w.texture_mb_hashed = per_frame(m.stats.texture_check_bytes, m.report_texture_bytes) / 1048576.0;
        w.write_faults = per_frame(m.tracker.faults_handled(), m.report_faults);
        w.feedback_copies = per_frame(m.stats.feedback_copies, m.report_feedback);
        w.feedback_reused = per_frame(m.stats.feedback_reused, m.report_feedback_reused);
        w.texture_cache_invalidations = per_frame(m.cache_epoch, m.report_cache_epoch);
        w.depth_bounds_draws = per_frame(m.stats.depth_bounds_draws, m.report_depth_bounds);
        w.attachment_load_mb = per_frame(m.stats.attachment_load_bytes, m.report_load_bytes) / 1048576.0;
        w.attachment_store_mb = per_frame(m.stats.attachment_store_bytes, m.report_store_bytes) / 1048576.0;
        w.attachment_store_skipped_mb = per_frame(m.stats.attachment_store_skipped_bytes, m.report_store_skipped) / 1048576.0;
        w.clears_folded = per_frame(m.stats.clears_folded, m.report_clears_folded);
        w.clears_waited = per_frame(m.stats.clears_waited, m.report_clears_waited);
        w.corner_textures = per_frame(m.stats.corner_textures, m.report_corners);
        w.surface_reloads = per_frame(m.stats.surface_reloads, m.report_reloads);
        w.gpu_wait_ms = per_frame(m.stats.gpu_wait_ns, m.report_gpu_wait_ns) / 1e6;
        w.programs_made = m.stats.programs_compiled - m.report_programs;
        w.pipelines_made = m.stats.pipelines_created - m.report_pipelines;
        w.compile_ms =
            static_cast<double>(m.stats.compile_ns + m.stats.pipeline_ns + m.stats.compile_wait_ns - m.report_compile_ns) / 1e6;
        w.draws_compiling = per_frame(m.stats.draws_compiling, m.report_draws_compiling);
        w.draws_interpreted = per_frame(m.stats.draws_interpreted, m.report_draws_interpreted);
        w.textures = m.textures.size();
        w.texture_mb = static_cast<double>(m.texture_bytes()) / 1048576.0;
        w.surfaces = m.surfaces.size();
        w.surface_mb = static_cast<double>(m.surface_bytes()) / 1048576.0;
        w.gpu_allocated_mb = static_cast<double>(m.device.currentAllocatedSize) / 1048576.0;
        w.deferred_queue = m.deferred.size();
        if (m.rsx) {
            const AlarmCounts now = m.rsx->alarms().snapshot();
            w.alarms = now - m.report_alarms;
            m.report_alarms = now;
        }
        PS_LOG_INFO(kChan, "per frame (last {}): GPU {:.2f} ms, {:.0f} draws, {:.0f} render passes (ended by {}; "
                           "attachments {:.1f} MB loaded, {:.1f} MB stored, {:.1f} MB not (the next pass clears them); "
                           "{:.1f} passes only clearing, {:.1f} MB stored by them; {:.1f} clears a later pass's load "
                           "action, {:.1f} letting the pass drawing go on, {:.1f} in passes of their own); "
                           "{:.2f} readbacks ({:.2f} deferred, {:.2f} accesses waited for one), {:.2f} ms waiting for them; "
                           "{:.2f} targets laid over memory on the GPU; "
                           "{:.0f} texture checks ({:.2f} MB hashed, {:.1f} write faults); {:.1f} textures decoded "
                           "({:.1f} over a target drawn to again, {:.1f} written); {} programs and {} pipelines "
                           "made ({:.1f} ms, {:.2f} draws waiting for one, {:.2f} drawn by the interpreter); "
                           "{} textures cached ({:.0f} MB in guest memory), {} surfaces ({:.0f} MB; {:.2f} made a frame, {:.2f} "
                           "of them with a removed one's texture); "
                           "{:.2f} feedback copies ({:.1f} draws with depth bounds), {:.2f} corner textures ({:.2f} "
                           "copies into pooled textures), {:.2f} surface reloads; {:.2f} ms waiting for the GPU; GPU "
                           "memory {:.0f} MB, {} deferred",
            w.flips, w.gpu_ms, w.draws, w.render_passes, ends, w.attachment_load_mb, w.attachment_store_mb,
            w.attachment_store_skipped_mb, per_frame(m.stats.clear_only_passes, m.report_clear_only),
            per_frame(m.stats.clear_only_store_bytes, m.report_clear_only_bytes) / 1048576.0, w.clears_folded,
            w.clears_waited, per_frame(m.stats.clear_passes, m.report_clear_passes),
            w.readbacks, w.deferred_readbacks, w.deferred_conflicts,
            w.readback_wait_ms, w.texture_overlays, w.texture_checks, w.texture_mb_hashed, w.write_faults,
            per_frame(m.stats.textures_decoded, m.report_decoded), per_frame(m.stats.textures_decoded_overlay,
                m.report_decoded_overlay), per_frame(m.stats.textures_decoded_written, m.report_decoded_written),
            w.programs_made, w.pipelines_made, w.compile_ms, w.draws_compiling, w.draws_interpreted, w.textures, w.texture_mb,
            w.surfaces, w.surface_mb, per_frame(m.stats.surfaces_created, m.report_surfaces_made),
            per_frame(m.stats.surface_textures_reused, m.report_surfaces_reused),
            w.feedback_copies, w.depth_bounds_draws, w.corner_textures,
            per_frame(m.stats.pooled_textures_taken, m.report_pooled), w.surface_reloads, w.gpu_wait_ms, w.gpu_allocated_mb,
            w.deferred_queue);
        {
            // The correctness alarms (D-1022): each line that is not zero, in the window.
            std::string raised;
            for (usize i = 0; i < kAlarmNames.size(); i++)
                if (w.alarms.v[i]) raised += std::format("{}{} {}", raised.empty() ? "" : ", ", kAlarmNames[i], w.alarms.v[i]);
            PS_LOG_INFO(kChan, "alarms (last {}): {}; {:.2f} targets a frame started with the bytes of those under them",
                w.flips, raised.empty() ? "none" : raised, per_frame(m.stats.targets_carried, m.report_carried));
            m.report_carried = m.stats.targets_carried;
        }
        // The draws that sampled a target they drew to (D-947): copies made and why, copies kept as the texture
        // cache, the game's invalidations, and the draws behind most of them.
        if (w.feedback_copies + w.feedback_reused >= 1.0) {
            std::vector<std::pair<Impl::FeedbackKey, Impl::FeedbackSource>> top(m.feedback_sources.begin(),
                m.feedback_sources.end());
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
                return a.second.made + a.second.reused > b.second.made + b.second.reused;
            });
            std::string most;
            for (usize i = 0; i < top.size() && i < 3; i++) {
                most += std::format("{}{} ({:.1f} copies, {:.1f} kept a frame)", i ? "; " : "",
                    Impl::feedback_source_name(top[i].first),
                    static_cast<double>(top[i].second.made) / n, static_cast<double>(top[i].second.reused) / n);
            }
            PS_LOG_INFO(kChan, "feedback (last {}): {:.2f} copies a frame ({:.2f} the first, {:.2f} after a write and "
                               "a texture cache invalidation, {:.2f} after the targets changed, {:.2f} with the cache "
                               "rule off), {:.2f} draws a frame read a kept copy; {:.2f} texture cache invalidations a "
                               "frame; most: {}",
                w.flips, w.feedback_copies, per_frame(m.stats.feedback_first, m.report_feedback_first),
                per_frame(m.stats.feedback_invalidated, m.report_feedback_invalidated),
                per_frame(m.stats.feedback_rebound, m.report_feedback_rebound),
                per_frame(m.stats.feedback_strict, m.report_feedback_strict), w.feedback_reused,
                w.texture_cache_invalidations, most);
        }
        if (m.edge_pipeline) {
            // EDGE geometry on the GPU (D-988): the draws and their kernels' GPU time; the SPU jobs, deferred and run
            // whole, and their host time a job (the A/B's halves are the two kinds with PSPOCKET_EDGE_GPU=ab).
            const edge::Stats es = edge::stats();
            const u64 edge_ns = m.edge_gpu_ns->load(std::memory_order_relaxed);
            const auto us_a_job = [](u64 ns, u64 jobs) { return jobs ? static_cast<double>(ns) / 1e3 / static_cast<double>(jobs) : 0.0; };
            PS_LOG_INFO(kChan, "EDGE on the GPU (last {}): {:.0f} draws a frame, {:.1f} dispatches, kernels {:.3f} ms a frame, "
                               "{:.0f} KB of work items a frame; SPU jobs deferred {:.0f} a frame at {:.2f} us, run whole {:.0f} "
                               "a frame at {:.2f} us; {} items replaced before their draw; ring-full waits (label "
                               "GETs) {:.1f} a frame in deferred jobs, {:.1f} in jobs run whole; jobs a frame by route: {}",
                w.flips, per_frame(m.stats_edge.draws, m.report_edge.draws),
                per_frame(m.stats_edge.dispatches, m.report_edge.dispatches),
                per_frame(edge_ns, m.report_edge_gpu_ns) / 1e6,
                per_frame(m.stats_edge.blob_bytes, m.report_edge.blob_bytes) / 1024.0,
                per_frame(es.jobs_deferred, m.report_edge_stats.jobs_deferred),
                us_a_job(es.ns_deferred - m.report_edge_stats.ns_deferred, es.jobs_deferred - m.report_edge_stats.jobs_deferred),
                per_frame(es.jobs_spu, m.report_edge_stats.jobs_spu),
                us_a_job(es.ns_spu - m.report_edge_stats.ns_spu, es.jobs_spu - m.report_edge_stats.jobs_spu),
                es.replaced - m.report_edge_stats.replaced,
                per_frame(es.label_gets[1], m.report_edge_stats.label_gets[1]),
                per_frame(es.label_gets[0], m.report_edge_stats.label_gets[0]),
                edge::describe_routes(es, m.report_edge_stats, static_cast<double>(w.flips)));
            m.report_edge = m.stats_edge;
            m.report_edge_gpu_ns = edge_ns;
            m.report_edge_stats = es;
            if (edge::mode() == edge::Mode::verify) {
                const auto& v = *m.edge_verify_stats;
                PS_LOG_INFO(kChan, "EDGE verify (so far): {} jobs, {} the same count of kept indices and {} the same "
                                   "list, {} of {} vertices the same bytes; F32 at most {} ulps apart (of 2^-10 or more; {:.3g} "
                                   "of the value or 1), the packed formats {} steps; triangles kept {} by the GPU, {} by "
                                   "the SPUs",
                    v.jobs.load(), v.counts_same.load(), v.lists_same.load(), v.vertices_same.load(), v.vertices.load(),
                    v.f32_ulps.load(), static_cast<double>(v.f32_rel_e9.load()) / 1e9, v.int_diff.load(), v.kept_gpu.load(),
                    v.kept_spu.load());
                // by kind: each route, and each reason a route was taken for (D-1006)
                std::string kinds;
                for (int k = 0; k < 64; k++) {
                    const u64 jobs = v.kind_jobs[static_cast<usize>(k)].load();
                    if (!jobs) continue;
                    const bool route = k >= edge::kind_route(edge::Route::spu);
                    const char* name = route ? edge::route_name(static_cast<edge::Route>(k - edge::kind_route(edge::Route::spu)))
                                             : edge::reason_name(static_cast<edge::Reason>(k));
                    if (!route && k == 0) continue; // no reason: the routes say it
                    const u64 lists = v.kind_lists_same[static_cast<usize>(k)].load(), verts = v.kind_vertices[static_cast<usize>(k)].load();
                    const u64 vsame = v.kind_vertices_same[static_cast<usize>(k)].load();
                    kinds += std::format("{}{}: {} jobs, {} lists{}, {} of {} vertices{}", kinds.empty() ? "" : "; ", name, jobs,
                        lists, lists == jobs ? " (all)" : " (MISMATCH)", vsame, verts, vsame == verts ? "" : " (MISMATCH)");
                }
                PS_LOG_INFO(kChan, "EDGE verify by kind (so far): {}", kinds);
            }
        }
        {
            std::lock_guard lock(m.fp_draws_mutex); // the draws by fragment program so far, for draw_signature()
            m.fp_draws_snapshot = m.fp_draws;
            m.fp_draws_flips = m.stats.flips;
        }
        m.feedback_sources.clear();
        m.report_feedback_reused = m.stats.feedback_reused;
        m.report_feedback_first = m.stats.feedback_first;
        m.report_feedback_invalidated = m.stats.feedback_invalidated;
        m.report_feedback_rebound = m.stats.feedback_rebound;
        m.report_feedback_strict = m.stats.feedback_strict;
        m.report_cache_epoch = m.cache_epoch;
        if (m.config.on_window) m.config.on_window(w);
        m.report_programs = m.stats.programs_compiled;
        m.report_pipelines = m.stats.pipelines_created;
        m.report_compile_ns = m.stats.compile_ns + m.stats.pipeline_ns + m.stats.compile_wait_ns;
        m.report_draws_compiling = m.stats.draws_compiling;
        m.report_draws_interpreted = m.stats.draws_interpreted;
        m.report_texture_checks = m.stats.texture_checks;
        m.report_decoded = m.stats.textures_decoded;
        m.report_decoded_overlay = m.stats.textures_decoded_overlay;
        m.report_decoded_written = m.stats.textures_decoded_written;
        m.report_texture_bytes = m.stats.texture_check_bytes;
        m.report_faults = m.tracker.faults_handled();
        m.report_readback_ns = m.stats.readback_wait_ns;
        m.report_overlays = m.stats.texture_overlays;
        m.report_readbacks = m.stats.readbacks;
        m.report_deferred = m.stats.deferred_readbacks;
        m.report_conflicts = m.stats.deferred_conflicts;
        m.report_feedback = m.stats.feedback_copies;
        m.report_depth_bounds = m.stats.depth_bounds_draws;
        m.report_load_bytes = m.stats.attachment_load_bytes;
        m.report_store_bytes = m.stats.attachment_store_bytes;
        m.report_clear_only = m.stats.clear_only_passes;
        m.report_clear_only_bytes = m.stats.clear_only_store_bytes;
        m.report_store_skipped = m.stats.attachment_store_skipped_bytes;
        m.report_clears_folded = m.stats.clears_folded;
        m.report_clears_waited = m.stats.clears_waited;
        m.report_clear_passes = m.stats.clear_passes;
        m.report_corners = m.stats.corner_textures;
        m.report_pooled = m.stats.pooled_textures_taken;
        m.report_reloads = m.stats.surface_reloads;
        m.report_surfaces_made = m.stats.surfaces_created;
        m.report_surfaces_reused = m.stats.surface_textures_reused;
        m.report_gpu_wait_ns = m.stats.gpu_wait_ns;
        m.report_flips = 0;
        m.report_gpu_ns = gpu;
        m.report_draws = m.stats.draws;
        m.report_passes = m.stats.render_passes;
    }
}

// PSPOCKET_RSX_TRACE_PROBE=X,Y (a debugging aid, D-1028): in a traced frame, after each draw, the pixel at (X, Y) of a
// 1280x720 screen (scaled to each target's size) of the draw's targets, read back at once: which draw made it what.
void MetalBackend::Impl::trace_probe()
{
    static const std::pair<int, int> at = [] {
        const char* v = std::getenv("PSPOCKET_RSX_TRACE_PROBE");
        int x = -1, y = -1;
        if (v) std::sscanf(v, "%d,%d", &x, &y);
        return std::pair<int, int>{x, y};
    }();
    if (at.first < 0 || (!draw_targets.color_count && !draw_targets.depth)) return;
    end_pass_for(kEndFlush);
    submit(true);
    std::string line;
    const auto probe = [&](Surface* s) {
        if (!s || !s->initialized) return;
        const u32 x = std::min<u32>(s->width - 1, static_cast<u32>(at.first) * s->width / 1280);
        const u32 y = std::min<u32>(s->height - 1, static_cast<u32>(at.second) * s->height / 720);
        u32 bpp = 4;
        MTLBlitOption o = MTLBlitOptionNone;
        switch (s->pixel_format) {
        case MTLPixelFormatR8Unorm: bpp = 1; break;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatDepth16Unorm: bpp = 2; break;
        case MTLPixelFormatRGBA16Float: bpp = 8; break;
        case MTLPixelFormatRGBA32Float: bpp = 16; break;
        case MTLPixelFormatDepth32Float_Stencil8: o = MTLBlitOptionDepthFromDepthStencil; break;
        default: break;
        }
        id<MTLBuffer> buf = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:s->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
                   sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:bpp
      destinationBytesPerImage:bpp options:o];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const u8* b = static_cast<const u8*>(buf.contents);
        line += std::format(" {:#010x}{}:", s->address, s->depth ? "(z)" : "");
        if (s->depth && bpp == 4) {
            float z;
            std::memcpy(&z, b, 4);
            line += std::format("{:.6f}", z);
        } else {
            for (u32 i = 0; i < bpp; i++) line += std::format("{:02x}", b[i]);
        }
    };
    for (u32 i = 0; i < draw_targets.color_count; i++) probe(draw_targets.color[i]);
    probe(draw_targets.depth);
    PS_LOG_INFO("rsx.trace", "  probe ({},{}) after #{}:{}", at.first, at.second, trace_count, line);
}

void MetalBackend::sync()
{
    if (!impl_->device) return;
    @autoreleasepool {
        impl_->submit(true);
        impl_->run_deferred(true);
        if (impl_->exact) impl_->exact_write_back(); // D-1029
    }
}

// ---- the exact mode (D-1029) ----------------------------------------------------------------------------------

id<MTLBuffer> MetalBackend::Impl::guest_window(u32 addr, u32 size, u32& base)
{
    if (addr < PS_MEM_RSX_LOCAL_BASE || u64{addr} + size > u64{PS_MEM_RSX_LOCAL_BASE} + PS_MEM_RSX_LOCAL_SIZE) return nil;
    const u64 page = GuestMemory::host_page_size();
    const u64 lo = addr & ~(page - 1), hi = (u64{addr} + size + page - 1) & ~(page - 1);
    for (const GuestWindow& w : guest_windows) {
        if (lo >= w.base && hi <= u64{w.base} + w.size) {
            base = w.base;
            return w.buffer;
        }
    }
    const auto committed = [&](u64 a, u64 b) {
        for (u64 p = a; p < b; p += page)
            if (!mem.is_committed(static_cast<u32>(p))) return false;
        return true;
    };
    constexpr u64 kWindow = 16u << 20;
    u64 wl = lo, wh = hi;
    const u64 cl = addr & ~(kWindow - 1);
    if (hi <= cl + kWindow && committed(cl, cl + kWindow)) {
        wl = cl;
        wh = cl + kWindow;
    } else if (!committed(lo, hi)) {
        return nil;
    }
    id<MTLBuffer> b = [device newBufferWithBytesNoCopy:mem.host(static_cast<u32>(wl)) length:wh - wl
                                               options:MTLResourceStorageModeShared deallocator:nil];
    if (!b) return nil;
    guest_windows.push_back({b, static_cast<u32>(wl), static_cast<u32>(wh - wl)});
    base = static_cast<u32>(wl);
    return b;
}

void MetalBackend::Impl::exact_write_back()
{
    std::vector<Surface*> on_gpu, by_cpu;
    for (const auto& c : surfaces) {
        Surface* s = c.get();
        if (!s->gpu_newer || s->cpu_newer || !s->initialized || !readback_supported(*s)) continue;
        u32 base = 0;
        (overlay_supported(*s) && guest_window(s->address, s->bytes(), base) ? on_gpu : by_cpu).push_back(s);
    }
    if (on_gpu.empty() && by_cpu.empty()) return;
    end_pass_for(kEndFlush);
    for (Surface* s : on_gpu) {
        sync_surface(*s); // what its children wrote (D-122)
        u32 base = 0;
        id<MTLBuffer> w = guest_window(s->address, s->bytes(), base);
        const GuestWindow* gw = nullptr;
        for (const GuestWindow& g : guest_windows)
            if (g.buffer == w) gw = &g;
        TransientAllocator::Allocation target{w, 0, nullptr};
        encode_overlay(*s, target, base, gw ? gw->size : s->bytes());
    }
    submit(true);
    for (Surface* s : on_gpu) {
        s->gpu_newer = false;
        own_write();
        tracker.notify_write(s->address, s->bytes());
        tracker.unwatch(s->watch);
        s->watch = tracker.watch(s->address, s->bytes()); // a CPU write after this makes guest memory newer (D-110)
        exact_bytes += s->bytes();
        exact_write_backs++;
    }
    for (Surface* s : by_cpu) {
        if (write_back(*s, "the exact mode")) {
            tracker.unwatch(s->watch);
            s->watch = tracker.watch(s->address, s->bytes());
            exact_bytes += s->bytes();
            exact_write_backs++;
        }
    }
}

void MetalBackend::Impl::end_pass_over(u32 addr, u32 size)
{
    // A target of the open pass with only a clear so far (a load action) is not yet newer on the GPU: end the pass so
    // the clear runs before the target is written back.
    if (!pass_open) return;
    const u64 end = u64{addr} + size;
    bool overlaps = false;
    const auto check = [&](const Surface* s) { overlaps |= s && u64{s->address} < end && addr < u64{s->address} + s->bytes(); };
    for (u32 i = 0; i < pass.color_count; i++) check(pass.color[i]);
    check(pass.depth);
    if (overlaps) end_pass_for(kEndFlush);
}

bool MetalBackend::copy_on_gpu(u32 src, u32 src_pitch, u32 dst, u32 dst_pitch, u32 line_length, u32 line_count)
{
    Impl& m = *impl_;
    if (!m.device || line_length == 0 || line_count == 0 || m.exact) return false; // the exact mode copies in memory
    @autoreleasepool {
        const u32 src_size = src_pitch * (line_count - 1) + line_length;
        const u32 dst_size = dst_pitch * (line_count - 1) + line_length;
        if (u64{src} < u64{dst} + dst_size && dst < u64{src} + src_size) return false; // overlapping: in memory
        // What is deferred and writes either range lands first (a read of the source, a write of the destination
        // that must not be overwritten by an older copy).
        m.before_access(src, src_size, false);
        m.before_access(dst, dst_size, false);
        m.end_pass_over(src, src_size);
        // The source: rows of one colour target newer on the GPU, whole pixels.
        Surface* s = nullptr;
        for (auto& c : m.surfaces) {
            if (c->depth || c->swizzled || c->pitch != src_pitch || src < c->address || src >= c->address + c->bytes()) continue;
            if (!s || (c->gpu_newer && !c->cpu_newer)) s = c.get();
        }
        if (!s || !s->gpu_newer || s->cpu_newer) return false;
        const u32 bpp = surface_color_bpp(s->gcm_format);
        const u32 offset = src - s->address, x0_bytes = offset % s->pitch;
        if (bpp == 0 || x0_bytes % bpp || line_length % bpp) return false;
        const u32 x0 = x0_bytes / bpp, y0 = offset / s->pitch, w = line_length / bpp, h = line_count;
        if (x0 + w > s->width || y0 + h > s->height || !m.readback_supported(*s)) return false;
        // The destination: a target of the same format (the one from the last copy, or a new one). Targets there with
        // newer contents than guest memory would lose them: those copies go through memory.
        Surface* d = nullptr;
        for (auto& c : m.surfaces) {
            const u32 c_row = c->width * surface_bytes_per_pixel(c->gcm_format, c->depth);
            if (!surfaces_overlap(dst, dst_pitch, line_length, line_count, c->address, c->pitch, c_row, c->height)) continue;
            if (c->address == dst && !c->depth && !c->swizzled && c->gcm_format == s->gcm_format && c->width == w &&
                c->height == h && c->pitch == dst_pitch) {
                d = c.get();
            } else if (c->gpu_newer && !c->cpu_newer) {
                return false;
            }
        }
        if (!d) d = m.get_color_surface(dst, dst_pitch, w, h, s->gcm_format, false);
        if (!d) return false;
        m.sync_surface(*s); // what the source's children wrote (D-122)
        m.tracker.unwatch(d->watch); // newer on the GPU again
        m.end_pass_for(Impl::kEndGpuCopy);
        id<MTLBlitCommandEncoder> blit = [m.render_buffer() blitCommandEncoder];
        [blit copyFromTexture:s->texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(x0, y0, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:d->texture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        d->initialized = true;
        d->gpu_newer = true;
        d->cpu_newer = false;
        d->generation++;
        d->last_used = m.tick;
        d->last_frame = m.stats.flips;
        m.surface_written(*d);
        m.stats.gpu_copies++;
        // Guest memory gets the copy once the GPU has it, before what the RSX writes after it (a label the CPU waits
        // for to read it): a deferred readback that writes nothing else, so the GPU's own reads do not wait for it.
        Impl::Deferred later;
        later.readbacks.push_back(m.encode_readback(*d));
        later.readbacks.back().mirror = d->texture;
        later.readbacks.back().generation = d->generation;
        later.buffer = m.render_buffer();
        m.submit(false);
        m.stats.readbacks++;
        m.stats.deferred_readbacks++;
        m.note_deferred(later);
        m.deferred.push_back(std::move(later));
    }
    return true;
}

void MetalBackend::wait_for_async_compiles()
{
    if (impl_->async) impl_->async->wait_idle();
}

void MetalBackend::wait_for_shader_cache()
{
    if (!impl_->shader_cache) return;
    impl_->shader_cache->wait();
    @autoreleasepool {
        impl_->take_shader_cache();
    }
}

bool MetalBackend::flush_memory_later(u32 addr, u32 size, u32 write_addr, u32 write_size, std::function<void()> then)
{
    Impl& m = *impl_;
    if (!m.device || m.exact) return false; // the exact mode waits for the GPU (D-1029)
    @autoreleasepool {
        m.end_pass_over(addr, size);
        m.end_pass_over(write_addr, write_size);
        const u64 end = u64{addr} + size, write_end = u64{write_addr} + write_size;
        std::vector<Surface*> newer;
        for (auto& s : m.surfaces) {
            if (!s->gpu_newer || s->cpu_newer) continue;
            const u64 s_end = u64{s->address} + s->bytes();
            // The source's targets, and the destination's: the copy lands over what they hold then (D-110).
            if ((u64{s->address} < end && addr < s_end) || (u64{s->address} < write_end && write_addr < s_end))
                newer.push_back(s.get());
        }
        // Nothing on the GPU: the copy runs now, unless a queued action touches what it reads or writes (then it queues
        // behind it, and what reads its destination waits: before_access).
        if (newer.empty() && !m.deferred_conflict(addr, size, false) && !m.deferred_conflict(write_addr, write_size, true))
            return false;
        Impl::Deferred d;
        d.then = std::move(then);
        d.read_addr = addr, d.read_size = size, d.write_addr = write_addr, d.write_size = write_size;
        for (Surface* s : newer) {
            if (!m.readback_supported(*s)) {
                m.unsupported(std::format("GPU readback of surface format {}{} is not implemented", s->gcm_format,
                    s->depth ? " (depth)" : s->swizzled ? " (swizzled)" : ""));
                continue;
            }
            d.readbacks.push_back(m.encode_readback(*s));
            m.stats.readbacks++;
            m.stats.deferred_readbacks++;
        }
        if (!d.readbacks.empty()) {
            // Committed now: it completes while the RSX goes on, and waiting for it never commits work half encoded.
            d.buffer = m.render_buffer();
            m.submit(false);
        }
        m.note_deferred(d);
        m.deferred.push_back(std::move(d));
    }
    return true;
}

void MetalBackend::defer(u32 addr, u32 size, std::function<void()> action)
{
    if (impl_->deferred.empty()) {
        action();
        return;
    }
    Impl::Deferred d;
    d.then = std::move(action);
    d.write_addr = addr, d.write_size = size;
    impl_->note_deferred(d);
    impl_->deferred.push_back(std::move(d));
}

bool MetalBackend::deferring() const { return !impl_->deferred.empty(); }

void MetalBackend::before_access(u32 addr, u32 size, bool write)
{
    if (impl_->deferred.empty()) return;
    @autoreleasepool {
        impl_->before_access(addr, size, write);
    }
}

void MetalBackend::run_deferred(bool wait)
{
    if (impl_->deferred.empty()) return;
    @autoreleasepool {
        impl_->run_deferred(wait);
    }
}

void MetalBackend::invalidate_memory(u32 addr, u32 size)
{
    // The RSX's own CPU copies: textures over that memory re-upload, surfaces reload from guest memory before use.
    impl_->tracker.notify_write(addr, size);
    impl_->own_write();
    const u64 end = u64{addr} + size;
    for (auto& s : impl_->surfaces) {
        if (u64{s->address} < end && addr < u64{s->address} + s->bytes()) s->cpu_newer = true;
    }
}

void MetalBackend::set_zpass_counting(bool on) { impl_->zpass_on = on; }

void MetalBackend::clear_zpass()
{
    impl_->zpass_slots.clear();
    impl_->pass_zpass_slot = -1; // a new counter, even in the same pass
}

bool MetalBackend::report_zpass(u32 value_addr, bool whole)
{
    if (!impl_->device) return false;
    impl_->pending_reports.push_back({value_addr, impl_->zpass_slots, whole});
    impl_->stats.occlusion_reports++;
    std::lock_guard lock(impl_->reports_in_flight->mutex);
    impl_->reports_in_flight->count[value_addr]++;
    return true;
}

void MetalBackend::draw_skipped(const RsxState& state, std::string_view why)
{
    Impl& m = *impl_;
    if (!m.tracing) return;
    PS_LOG_INFO("rsx.trace", "#{} draw SKIPPED ({}): prim {} fp {:#x}", ++m.trace_count, why,
        static_cast<u32>(state.draw.primitive), state.shader_program().offset);
}

bool MetalBackend::tracing() const { return impl_->tracing; }

void MetalBackend::trace_note(std::string_view what)
{
    if (impl_->tracing) PS_LOG_INFO("rsx.trace", "  {}", what);
}

bool MetalBackend::report_pending(u32 value_addr)
{
    std::lock_guard lock(impl_->reports_in_flight->mutex);
    return impl_->reports_in_flight->count.contains(value_addr);
}

bool MetalBackend::flush_memory(u32 addr, u32 size, const char* why)
{
    if (!impl_->device) return false;
    bool any = false;
    @autoreleasepool {
        impl_->before_access(addr, size, false); // a copy queued before this read lands first (D-107)
        impl_->end_pass_over(addr, size);
        const u64 end = u64{addr} + size;
        for (auto& s : impl_->surfaces) {
            if (!s->gpu_newer || s->cpu_newer) continue;
            if (u64{s->address} < end && addr < u64{s->address} + s->bytes()) any |= impl_->write_back(*s, why);
        }
    }
    return any;
}

bool MetalBackend::debug_decode_texture(const TextureInfo& t, u32 ea, u32 face, u32 level, bool force_compute_dxt,
    std::vector<std::array<float, 4>>& out, std::string* error)
{
    if (!impl_->device) return false;
    @autoreleasepool {
        CachedTexture* c = impl_->decode_texture(t, ea, force_compute_dxt, error);
        if (!c) return false;
        if (face >= c->layout.faces || level >= c->layout.mips) return false;
        const TextureLevel& l = c->layout.levels[face * c->layout.mips + level];
        impl_->submit(true);
        return impl_->read_texture(c->texture, face, level, l.width, l.height, out);
    }
}

bool MetalBackend::debug_read_surface(u32 ea, std::vector<u8>& rgba, u32& width, u32& height)
{
    if (!impl_->device) return false;
    @autoreleasepool {
        Surface* s = impl_->find_surface(ea, false);
        if (!s) return false;
        impl_->submit(true);
        std::vector<u8> raw;
        if (!impl_->read_back(*s, raw)) return false;
        width = s->width;
        height = s->height;
        rgba.resize(static_cast<usize>(width) * height * 4);
        if (s->pixel_format == MTLPixelFormatBGRA8Unorm) {
            for (usize i = 0; i < static_cast<usize>(width) * height; i++) {
                rgba[i * 4 + 0] = raw[i * 4 + 2];
                rgba[i * 4 + 1] = raw[i * 4 + 1];
                rgba[i * 4 + 2] = raw[i * 4 + 0];
                rgba[i * 4 + 3] = raw[i * 4 + 3];
            }
        } else if (s->pixel_format == MTLPixelFormatRGBA8Unorm) {
            rgba = raw;
        } else {
            return false;
        }
        return true;
    }
}

} // namespace pspocket::rsx::metal
