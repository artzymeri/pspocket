// Metal backend core: device setup, command buffers, render passes, draws, clears, flips, sync and memory hooks.
// The surface, texture and pipeline caches live in surface_cache.mm, texture_cache.mm and pipeline_cache.mm.

#include "metal_internal.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "kernels.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx::metal {

static_assert(static_cast<u32>(TexelDecode::ARGB8) == 1 && static_cast<u32>(TexelDecode::X16) == 14 &&
                  static_cast<u32>(TexelDecode::Y16X16) == 15 && static_cast<u32>(TexelDecode::DXT45) == 18 &&
                  static_cast<u32>(TexelDecode::D16) == 19,
    "TexelDecode values are mirrored in kernels.hpp");

// ---- transient allocator ------------------------------------------------------------------------------------------

TransientAllocator::Allocation TransientAllocator::alloc(u32 size, u32 align)
{
    size = std::max<u32>(size, 4);
    if (size > kChunkSize / 2) {
        // Large uploads get a buffer of their own (still retired with the command buffer).
        id<MTLBuffer> b = [device_ newBufferWithLength:size options:MTLResourceStorageModeShared];
        in_use_.push_back(b);
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
        offset = 0;
    }
    offset_ = offset + size;
    return {current_, offset, static_cast<u8*>(current_.contents) + offset};
}

void TransientAllocator::retire(id<MTLCommandBuffer> cb)
{
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
    device = config.device ? (__bridge id<MTLDevice>)config.device : MTLCreateSystemDefaultDevice();
    if (!device) {
        PS_LOG_ERROR(kChan, "no Metal device: the RSX backend drops all GPU work");
        return false;
    }
    queue = [device newCommandQueue];
    queue.label = @"pspocket.rsx";
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
    MTLRenderPipelineDescriptor* blit = [MTLRenderPipelineDescriptor new];
    blit.vertexFunction = [kernels newFunctionWithName:@"rsx_fullscreen_vs"];
    blit.fragmentFunction = [kernels newFunctionWithName:@"rsx_blit_fs"];
    blit.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    blit_pipeline = [device newRenderPipelineStateWithDescriptor:blit error:&error];
    if (!decode_pipeline || !read_pipeline || !depth_pipeline || !color_pipeline || !blit_pipeline) {
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
    if (!render_cb) {
        render_cb = [queue commandBuffer];
        render_cb.label = @"rsx render";
        stats.command_buffers++;
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
    end_pass();
    // Uploads first: the render buffer samples what they decode (same queue, so in order).
    id<MTLCommandBuffer> last = nil;
    if (upload_cb) {
        last = upload_cb;
        if (!render_cb) transient.retire(upload_cb);
        [upload_cb commit];
        upload_cb = nil;
    }
    if (render_cb) {
        last = render_cb;
        transient.retire(render_cb);
        [render_cb commit];
        render_cb = nil;
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
    out = {};
    draw_targets = {};
    u32 width = si.clip_width, height = si.clip_height;
    if (width == 0 || height == 0) return false;
    if (si.antialias != 0) unsupported(std::format("multisampled surfaces (AA mode {}) render with one sample", si.antialias));
    if (si.window_offset_x || si.window_offset_y) unsupported("NV4097_SET_WINDOW_OFFSET is ignored");
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
        const auto ea = rsx->translate(cb.offset, cb.dma == loc::kDmaHostBuffer ? loc::kMain : loc::kLocal);
        if (!ea) {
            unsupported(std::format("colour surface {} at offset {:#x} is not mapped", idx[i], cb.offset));
            continue;
        }
        Surface* s = get_color_surface(*ea, pitch, width, height, si.color_format, swizzled);
        if (!s) continue;
        out.rsx_index[out.color_count] = idx[i];
        out.color[out.color_count++] = s;
        draw_targets = out; // later attachments of this draw must not evict earlier ones
    }
    if (depth_used && (si.depth_format == 1 || si.depth_format == 2)) {
        const u32 dbpp = si.depth_format == 1 ? 2 : 4;
        if (swizzled || si.depth.pitch >= dbpp * width || si.depth.pitch > 64) {
            const auto ea = rsx->translate(si.depth.offset, si.depth.dma == loc::kDmaHostBuffer ? loc::kMain : loc::kLocal);
            if (ea) out.depth = get_depth_surface(*ea, si.depth.pitch, width, height, si.depth_format, si.depth_float);
        }
    }
    draw_targets = out;
    // All attachments of a pass share a size: surfaces were created at (width, height).
    return out.color_count > 0 || out.depth != nullptr;
}

void MetalBackend::Impl::open_pass(const PassTargets& t)
{
    if (pass_open && pass == t) return;
    end_pass();
    pass = t;
    pass_open = true;
    pass_started = false;
    pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    pass_width = ~0u;
    pass_height = ~0u;
    for (u32 i = 0; i < t.color_count; i++) {
        Surface* s = t.color[i];
        if (s->cpu_newer) upload_surface_from_guest(*s);
        MTLRenderPassColorAttachmentDescriptor* a = pass_desc.colorAttachments[i];
        a.texture = s->texture;
        // Load what is there; a surface nobody wrote yet starts cleared (deterministic) and is always stored:
        // render targets stay on the GPU (plan section 7).
        a.loadAction = s->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
        a.clearColor = MTLClearColorMake(0, 0, 0, 0);
        a.storeAction = MTLStoreActionStore;
        pass_width = std::min(pass_width, s->width);
        pass_height = std::min(pass_height, s->height);
    }
    if (Surface* d = t.depth) {
        pass_desc.depthAttachment.texture = d->texture;
        pass_desc.depthAttachment.loadAction = d->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
        pass_desc.depthAttachment.clearDepth = 1.0;
        pass_desc.depthAttachment.storeAction = MTLStoreActionStore;
        if (d->pixel_format == MTLPixelFormatDepth32Float_Stencil8) {
            pass_desc.stencilAttachment.texture = d->texture;
            pass_desc.stencilAttachment.loadAction = d->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
            pass_desc.stencilAttachment.clearStencil = 0;
            pass_desc.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        pass_width = std::min(pass_width, d->width);
        pass_height = std::min(pass_height, d->height);
    }
}

id<MTLRenderCommandEncoder> MetalBackend::Impl::start_encoder()
{
    if (!pass_open) return nil;
    if (!pass_started) {
        encoder = [render_buffer() renderCommandEncoderWithDescriptor:pass_desc];
        encoder.label = @"rsx pass";
        pass_started = true;
        stats.render_passes++;
        for (u32 i = 0; i < pass.color_count; i++) {
            pass.color[i]->initialized = true;
            pass.color[i]->gpu_newer = true;
            pass.color[i]->last_used = tick;
            pass.color[i]->generation++;
        }
        if (pass.depth) {
            pass.depth->initialized = true;
            pass.depth->gpu_newer = true;
            pass.depth->last_used = tick;
            pass.depth->generation++;
        }
    }
    return encoder;
}

void MetalBackend::Impl::end_pass()
{
    if (!pass_open) return;
    if (!pass_started) {
        // A pass that only clears (load-action clears) must still run.
        bool clears = false;
        for (u32 i = 0; i < pass.color_count; i++) {
            clears |= pass_desc.colorAttachments[i].loadAction == MTLLoadActionClear && pass.color[i]->initialized;
        }
        if (pass.depth) clears |= pass_desc.depthAttachment.loadAction == MTLLoadActionClear && pass.depth->initialized;
        if (clears) start_encoder();
    }
    if (encoder) {
        [encoder endEncoding];
        encoder = nil;
    }
    pass_open = false;
    pass_started = false;
    pass = {};
}

// ---- clears -----------------------------------------------------------------------------------------------------

std::string MetalBackend::Impl::describe_targets(const PassTargets& t)
{
    std::string d;
    for (u32 i = 0; i < t.color_count; i++) {
        const Surface* c = t.color[i];
        d += std::format(" {}{:#010x} fmt {} {}x{}", i ? ", " : "", c->address, c->gcm_format, c->width, c->height);
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
    // Keep the open pass if it already has these surfaces (clearing mid-pass), else switch.
    if (pass_open) {
        bool subset = true;
        for (u32 i = 0; i < t.color_count; i++) {
            subset &= i < pass.color_count && pass.color[i] == t.color[i];
        }
        if (t.depth && t.depth != pass.depth) subset = false;
        if (!subset) open_pass(t);
    } else {
        open_pass(t);
    }
    if (!pass_open) return;

    const SurfaceInfo si = state.surface();
    const ScissorInfo sc = state.scissor();
    const u32 x0 = std::min<u32>(sc.x, pass_width), y0 = std::min<u32>(sc.y, pass_height);
    const u32 x1 = std::min<u32>(u32{sc.x} + sc.width, pass_width), y1 = std::min<u32>(u32{sc.y} + sc.height, pass_height);
    if (x1 <= x0 || y1 <= y0) return;
    const bool full = x0 == 0 && y0 == 0 && x1 == pass_width && y1 == pass_height;

    const u32 argb = state.color_clear_value();
    const MTLClearColor color = MTLClearColorMake(((argb >> 16) & 0xff) / 255.0, ((argb >> 8) & 0xff) / 255.0,
        (argb & 0xff) / 255.0, (argb >> 24) / 255.0);
    const u32 zs = state.zstencil_clear_value();
    const bool z24 = si.depth_format == 2;
    const double depth = z24 ? (zs >> 8) / 16777215.0 : (zs & 0xffff) / 65535.0;
    const u32 stencil = zs & 0xff;
    const u32 channels = (mask >> 4) & 0xf; // R G B A
    if (tracing) {
        trace_targets_of(t);
        PS_LOG_INFO("rsx.trace", "#{} clear {:#x} ->{}: colour {:#010x}, depth/stencil {:#010x}, region {},{} to {},{}",
            ++trace_count, mask, describe_targets(t), argb, zs, x0, y0, x1, y1);
    }
    const bool clear_depth = (mask & clear::kDepth) && pass.depth;
    const bool clear_stencil = (mask & clear::kStencil) && pass.depth && z24;

    if (!pass_started && full) {
        // At the start of a pass a full clear is free on a tile-based GPU: make it the load action.
        bool done_color = channels == 0xf || channels == 0;
        if (channels == 0xf) {
            for (u32 i = 0; i < pass.color_count; i++) {
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
            pass.depth->initialized = true;
            done_depth = true;
        }
        if (done_color && done_depth) {
            stats.load_action_clears++;
            return;
        }
    }

    // A partial clear: draw a quad with the clear values through the scissor.
    id<MTLRenderCommandEncoder> enc = start_encoder();
    const u32 color_mask = channels ? channels : 0;
    id<MTLRenderPipelineState> pso = clear_pipeline_for(pass.color_count ? color_mask : 0, clear_depth, clear_stencil);
    if (!pso) return;
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
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:[device newDepthStencilStateWithDescriptor:dd]];
    [enc setStencilReferenceValue:stencil];
    [enc setViewport:MTLViewport{0, 0, static_cast<double>(pass_width), static_cast<double>(pass_height), 0, 1}];
    [enc setScissorRect:MTLScissorRect{x0, y0, x1 - x0, y1 - y0}];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    const float z = static_cast<float>(depth);
    [enc setVertexBytes:&z length:sizeof(z) atIndex:0];
    const float c[4] = {static_cast<float>(color.red), static_cast<float>(color.green), static_cast<float>(color.blue),
        static_cast<float>(color.alpha)};
    [enc setFragmentBytes:c length:sizeof(c) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// ---- draws ------------------------------------------------------------------------------------------------------

namespace {

struct Range {
    u32 first;
    u32 count;
};

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

// Index list for line loops, fans, quads, quad strips and polygons, in positions 0..count-1.
std::vector<u32> convert_primitive(Primitive p, u32 count, MTLPrimitiveType& type)
{
    std::vector<u32> out;
    switch (p) {
    case Primitive::LineLoop:
        type = MTLPrimitiveTypeLine;
        for (u32 i = 0; i + 1 < count; i++) out.insert(out.end(), {i, i + 1});
        if (count > 2) out.insert(out.end(), {count - 1, 0});
        break;
    case Primitive::TriangleFan:
    case Primitive::Polygon:
        type = MTLPrimitiveTypeTriangle;
        for (u32 i = 1; i + 1 < count; i++) out.insert(out.end(), {0, i, i + 1});
        break;
    case Primitive::Quads:
        type = MTLPrimitiveTypeTriangle;
        for (u32 q = 0; q + 3 < count; q += 4) out.insert(out.end(), {q, q + 1, q + 2, q, q + 2, q + 3});
        break;
    case Primitive::QuadStrip:
        type = MTLPrimitiveTypeTriangle;
        for (u32 q = 0; q + 3 < count; q += 2) out.insert(out.end(), {q, q + 1, q + 3, q, q + 3, q + 2});
        break;
    default: break;
    }
    return out;
}

} // namespace

void MetalBackend::Impl::do_draw(const RsxState& state)
{
    tick++;
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

    // ---- targets and programs ----
    PassTargets targets;
    skip_reason = "no render target";
    if (!select_targets(state, targets, ~0u)) return skip("");
    const bool points = d.primitive == Primitive::Points;
    const CompiledProgram* vp = vertex_program_for(state, points);
    skip_reason = "vertex program";
    if (!vp) return skip("");
    shader::FpOptions fo;
    fo.shader_control = state.shader_program().control;
    fo.mrt_count = targets.color_count;
    for (u32 i = 0; i < targets.color_count; i++) fo.output_swizzle[i] = surface_output_swizzle(targets.color[i]->gcm_format);
    for (u32 t = 0; t < kFragmentTextureCount; t++) {
        const TextureInfo ti = state.fragment_texture(t);
        fo.texture_kind[t] = ti.cubemap && ti.dimension == 2 ? shader::TextureKind::Cube
            : ti.dimension == 1                               ? shader::TextureKind::Tex1D
                                                              : shader::TextureKind::Tex2D;
        if (t < 10 && (state.texcoord_control(t) & 1)) fo.texcoord_2d_mask |= 1u << t;
    }
    const RasterInfo raster = state.raster();
    fo.alpha_test = raster.alpha_test;
    fo.flat_colors = raster.shade_mode == static_cast<u32>(ShadeMode::Flat);
    const shader::FragmentProgram* fprog = nullptr;
    u32 fp_ea = 0;
    const CompiledProgram* fp = fragment_program_for(state, fo, &fprog, fp_ea);
    skip_reason = "fragment program";
    if (!fp) return skip("");

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
    const ScissorInfo sc = state.scissor();
    const ViewportInfo vpi = state.viewport();
    const u32 sx0 = std::max<u32>(sc.x, vpi.x), sy0 = std::max<u32>(sc.y, vpi.y);
    const u32 sx1 = std::min({u32{sc.x} + sc.width, u32{vpi.x} + vpi.width, surf_w});
    const u32 sy1 = std::min({u32{sc.y} + sc.height, u32{vpi.y} + vpi.height, surf_h});
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
    for (u32 t = 0; t < kFragmentTextureCount; t++) {
        fs.tex_scale[t][0] = fs.tex_scale[t][1] = 1.f;
        if (!(fp->shader.texture_mask & (1u << t))) continue;
        const TextureInfo ti = state.fragment_texture(t);
        if (ti.is_unnormalized() && ti.width && ti.height) {
            fs.tex_scale[t][0] = 1.f / ti.width;
            fs.tex_scale[t][1] = 1.f / ti.height;
        }
        bool ok = true;
        id<MTLTexture> tex = ti.enabled ? bind_texture(ti, t, ok) : nil;
        if (tracing) {
            trace_tex += std::format(" t{} {:#010x} fmt {:#04x} {}x{}{} ({})", t,
                rsx->translate(ti.offset, ti.location).value_or(0), ti.base_format(), ti.width, ti.height,
                ti.enabled ? "" : " disabled", ti.enabled ? bind_source : "-");
        }
        if (tex && tex.textureType == MTLTextureTypeCube && fo.texture_kind[t] == shader::TextureKind::Cube) {
            bound[t] = tex;
        } else if (tex && tex.textureType == MTLTextureType2D && fo.texture_kind[t] != shader::TextureKind::Cube) {
            bound[t] = tex;
        } else {
            bound[t] = fo.texture_kind[t] == shader::TextureKind::Cube ? dummy_cube : dummy_texture;
        }
    }

    // ---- vertex textures (VP TXL, D-093): resolved before the pass opens, like the fragment textures ----
    std::array<id<MTLTexture>, 4> vbound{};
    for (u32 t = 0; t < 4; t++) {
        if (!(vp->shader.texture_mask & (1u << t))) continue;
        const TextureInfo vt = state.vertex_texture(t);
        bool ok = true;
        id<MTLTexture> tex = vt.enabled ? bind_texture(vt, 16 + t, ok) : nil;
        vbound[t] = tex && tex.textureType == MTLTextureType2D ? tex : dummy_texture;
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
        std::string blend = "off";
        if (b.enable || b.enable_mrt) {
            blend = std::format("eq {:#x}/{:#x} src {:#x}/{:#x} dst {:#x}/{:#x} mrt {:#x}", b.equation_rgb, b.equation_a,
                b.sfactor_rgb, b.sfactor_a, b.dfactor_rgb, b.dfactor_a, b.enable_mrt);
        }
        PS_LOG_INFO("rsx.trace", "#{} draw prim {} x{} ->{}; fp {:#010x}; mask {:x}{:x}{:x}{:x}; blend {};{}", ++trace_count,
            static_cast<u32>(d.primitive), vertices, describe_targets(targets), fp_ea, cm.mask[0], cm.mask[1], cm.mask[2],
            cm.mask[3], blend, trace_tex);
    }

    // ---- vertex parameters: viewport transform into Metal NDC for this surface
    // (RPCS3 RSXDrawCommands.cpp fill_scale_offset_data, y flipped for Metal's y-up NDC) ----
    shader::MslVsParams vs{};
    const float cw = static_cast<float>(surf_w) / 2.f, ch = static_cast<float>(surf_h) / 2.f;
    vs.pos_x[0] = vpi.scale[0] / cw;
    vs.pos_x[3] = (vpi.offset[0] - cw) / cw;
    vs.pos_y[1] = -vpi.scale[1] / ch;
    vs.pos_y[3] = -(vpi.offset[1] - ch) / ch;
    vs.pos_z[2] = vpi.scale[2];
    vs.pos_z[3] = vpi.offset[2];
    vs.base_index = state.vertex_data_base_index();
    vs.restart_enable = raster.restart_index_enable;
    vs.restart_index = raster.restart_index;
    if (raster.restart_index_enable && d.command == DrawCommand::Indexed) unsupported("primitive restart is not implemented");

    // ---- vertex and index data: raw guest bytes, decoded by the vertex shader (vertex pulling) ----
    std::vector<Range> ranges;
    std::vector<u32> index_offsets; // per range, for pulled indices
    TransientAllocator::Allocation vertex_data{}, index_data{};
    u32 min_vi = ~0u, max_vi = 0;
    const u32 inputs = vp->shader.input_mask & state.vertex_attrib_input_mask();
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
        const u32 isize = ia.u16_indices ? 2 : 4;
        vs.index_mode = ia.u16_indices ? shader::kIndexU16BE : shader::kIndexU32BE;
        u32 total = 0;
        for (const DrawRange& r : d.ranges) total += r.count * isize;
        index_data = transient.alloc(total, 16);
        u32 at = 0;
        for (const DrawRange& r : d.ranges) {
            const u32 bytes = r.count * isize;
            const auto ea = rsx->translate(ia.offset + r.first * isize, ia.location, bytes);
            if (!ea || !guest_ok(*ea, bytes)) return skip(std::format("index array at {:#x} is not mapped", ia.offset));
            u8* dst = index_data.data + at;
            std::memcpy(dst, mem.host(*ea), bytes);
            for (u32 i = 0; i < r.count; i++) {
                const u32 raw = isize == 2 ? ps_ld16(dst + i * 2) : ps_ld32(dst + i * 4);
                const u32 vi = (raw + vs.base_index) & 0xfffff;
                min_vi = std::min(min_vi, vi);
                max_vi = std::max(max_vi, vi);
            }
            ranges.push_back({r.first, r.count});
            index_offsets.push_back(index_data.offset + at - r.first * isize); // wraps; so does the shader
            at += bytes;
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
        struct Copy {
            u32 attr, ea, size;
        };
        std::vector<Copy> copies;
        u32 total = 0;
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
            if (!ea || !guest_ok(*ea, bytes)) return skip(std::format("vertex array {} at {:#x} is not mapped", i, base));
            vs.attribs[i] = {total, a.stride, static_cast<u32>(a.type), a.size, a.frequency,
                shader::kAttribSwap | (modulo ? shader::kAttribModulo : 0u), first, 0, {}};
            copies.push_back({i, *ea, bytes});
            total += (bytes + 15) & ~15u;
        }
        vertex_data = transient.alloc(std::max<u32>(total, 16), 16);
        for (const Copy& c : copies) {
            std::memcpy(vertex_data.data + vs.attribs[c.attr].offset, mem.host(c.ea), c.size);
            vs.attribs[c.attr].offset += vertex_data.offset;
        }
    }
    if (!index_data.buffer) index_data = vertex_data; // bound but unused

    auto consts = transient.alloc(kTransformConstantCount * 16, 16);
    std::memcpy(consts.data, state.transform_constants.data(), kTransformConstantCount * 16);

    // Fragment constants: patched by games between draws, so read every draw.
    std::vector<std::array<float, 4>> fconst;
    shader::read_fragment_constants(*fprog, mem.host(fp_ea), fconst);
    if (fconst.empty()) fconst.push_back({0.f, 0.f, 0.f, 0.f});
    const ShaderWindow win = state.shader_window();
    if (win.origin_bottom) {
        fs.wpos[0] = -1.f;
        fs.wpos[1] = static_cast<float>(win.height);
    }
    if (win.pixel_center_integer) fs.wpos[2] = -0.5f;
    fs.alpha_func = raster.alpha_func;
    fs.alpha_ref = static_cast<float>(raster.alpha_ref & 0xff) / 255.f;

    // ---- encode ----
    open_pass(targets);
    id<MTLRenderPipelineState> pso = pipeline_for(*vp, *fp, state);
    skip_reason = "pipeline state";
    if (!pso) return skip("");
    id<MTLRenderCommandEncoder> enc = start_encoder();
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:depth_state_for(state)];
    const DepthStencilInfo ds = state.depth_stencil();
    [enc setStencilFrontReferenceValue:ds.front.ref backReferenceValue:ds.back.ref];
    [enc setViewport:MTLViewport{0, 0, static_cast<double>(surf_w), static_cast<double>(surf_h), 0, 1}];
    [enc setScissorRect:MTLScissorRect{sx0, sy0, sx1 - sx0, sy1 - sy0}];
    if (raster.cull_enable) {
        [enc setCullMode:static_cast<CullFace>(raster.cull_face) == CullFace::Front ? MTLCullModeFront : MTLCullModeBack];
    } else {
        [enc setCullMode:MTLCullModeNone];
    }
    // Facing is judged in window coordinates with memory row 0 at the top, in Metal as for the RSX (the viewport
    // transform keeps that orientation), so the winding passes through.
    [enc setFrontFacingWinding:raster.front_face == static_cast<u32>(FrontFace::CW) ? MTLWindingClockwise
                                                                                   : MTLWindingCounterClockwise];
    [enc setTriangleFillMode:raster.front_polygon_mode == static_cast<u32>(PolygonMode::Line) ? MTLTriangleFillModeLines
                                                                                               : MTLTriangleFillModeFill];
    if (raster.poly_offset_fill) {
        [enc setDepthBias:raster.poly_offset_bias slopeScale:raster.poly_offset_scale clamp:0];
    } else {
        [enc setDepthBias:0 slopeScale:0 clamp:0];
    }
    const BlendInfo blend = state.blend();
    [enc setBlendColorRed:((blend.color >> 16) & 0xff) / 255.f
                    green:((blend.color >> 8) & 0xff) / 255.f
                     blue:(blend.color & 0xff) / 255.f
                    alpha:(blend.color >> 24) / 255.f];
    [enc setVertexBuffer:vertex_data.buffer offset:0 atIndex:shader::slot::kVsVertexData];
    [enc setVertexBuffer:index_data.buffer offset:0 atIndex:shader::slot::kVsIndexData];
    [enc setVertexBuffer:consts.buffer offset:consts.offset atIndex:shader::slot::kVsConstants];
    [enc setFragmentBytes:fconst.data() length:fconst.size() * 16 atIndex:shader::slot::kFsConstants];
    [enc setFragmentBytes:&fs length:sizeof(fs) atIndex:shader::slot::kFsParams];
    for (u32 t = 0; t < kFragmentTextureCount; t++) {
        if (!bound[t]) continue;
        [enc setFragmentTexture:bound[t] atIndex:t];
        [enc setFragmentSamplerState:sampler_for(state.fragment_texture(t)) atIndex:t];
    }
    for (u32 t = 0; t < 4; t++) {
        if (!vbound[t]) continue;
        [enc setVertexTexture:vbound[t] atIndex:t];
        [enc setVertexSamplerState:sampler_for(state.vertex_texture(t)) atIndex:t];
    }

    MTLPrimitiveType ptype = MTLPrimitiveTypeTriangle;
    const bool native = native_primitive(d.primitive, ptype);
    for (usize r = 0; r < ranges.size(); r++) {
        if (r < index_offsets.size()) vs.index_offset = index_offsets[r];
        [enc setVertexBytes:&vs length:sizeof(vs) atIndex:shader::slot::kVsParams];
        const Range rg = ranges[r];
        if (native) {
            [enc drawPrimitives:ptype vertexStart:rg.first vertexCount:rg.count];
            continue;
        }
        MTLPrimitiveType ctype = MTLPrimitiveTypeTriangle;
        std::vector<u32> list = convert_primitive(d.primitive, rg.count, ctype);
        if (list.empty()) continue;
        for (u32& v : list) v += rg.first; // vertex_id = position in the range, as for native draws
        auto ib = transient.alloc(static_cast<u32>(list.size() * 4), 16);
        std::memcpy(ib.data, list.data(), list.size() * 4);
        [enc drawIndexedPrimitives:ctype indexCount:list.size() indexType:MTLIndexTypeUInt32 indexBuffer:ib.buffer
                 indexBufferOffset:ib.offset];
    }
    stats.draws++;
}

// ---- flips --------------------------------------------------------------------------------------------------------

void MetalBackend::Impl::do_flip(const FlipRequest& request)
{
    tick++;
    end_pass();
    stats.flips++;
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
    frame_event_value++;
    [cb encodeSignalEvent:frame_event value:frame_event_value];
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
    submit(false);
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
        for (auto& [key, t] : impl_->textures) impl_->tracker.unwatch(t->watch);
        for (auto& [ea, f] : impl_->fragment_programs) impl_->tracker.unwatch(f.watch);
    }
}

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
    }
}

void MetalBackend::sync()
{
    if (!impl_->device) return;
    @autoreleasepool {
        impl_->submit(true);
    }
}

void MetalBackend::invalidate_memory(u32 addr, u32 size)
{
    // The RSX's own CPU copies: textures over that memory re-upload, surfaces reload from guest memory before use.
    impl_->tracker.notify_write(addr, size);
    const u64 end = u64{addr} + size;
    for (auto& s : impl_->surfaces) {
        if (u64{s->address} < end && addr < u64{s->address} + s->bytes()) s->cpu_newer = true;
    }
}

bool MetalBackend::flush_memory(u32 addr, u32 size, const char* why)
{
    if (!impl_->device) return false;
    bool any = false;
    @autoreleasepool {
        const u64 end = u64{addr} + size;
        // A target of the open pass with only a clear so far (a load action) is not yet newer on the GPU: end the
        // pass so the clear runs before the target is written back.
        Impl& m = *impl_;
        if (m.pass_open) {
            bool overlaps = false;
            const auto check = [&](const Surface* s) {
                overlaps |= s && u64{s->address} < end && addr < u64{s->address} + s->bytes();
            };
            for (u32 i = 0; i < m.pass.color_count; i++) check(m.pass.color[i]);
            check(m.pass.depth);
            if (overlaps) m.end_pass();
        }
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
