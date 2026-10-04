// Surface cache: render targets keyed by address, format, pitch and size (plan section 7, "Surfaces"). Surfaces live
// on the GPU; guest memory is only written when the RSX itself reads it (flush_memory: an expensive readback,
// logged) and only read back into the GPU copy when the CPU wrote it (invalidate_memory -> cpu_newer).

#include "metal_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "pspocket/core/be.h"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx::metal {

MTLPixelFormat color_surface_format(u32 f)
{
    switch (static_cast<SurfaceColorFormat>(f)) {
    case SurfaceColorFormat::B8: return MTLPixelFormatR8Unorm;
    case SurfaceColorFormat::G8B8: return MTLPixelFormatRG8Unorm;
    case SurfaceColorFormat::F_W16Z16Y16X16: return MTLPixelFormatRGBA16Float;
    case SurfaceColorFormat::F_W32Z32Y32X32: return MTLPixelFormatRGBA32Float;
    case SurfaceColorFormat::F_X32: return MTLPixelFormatR32Float;
    case SurfaceColorFormat::X8B8G8R8_Z8B8G8R8:
    case SurfaceColorFormat::X8B8G8R8_O8B8G8R8:
    case SurfaceColorFormat::A8B8G8R8: return MTLPixelFormatRGBA8Unorm;
    default:
        // A8R8G8B8, X8R8G8B8 and (for now, at 8 bits per channel) R5G6B5 / X1R5G5B5
        return MTLPixelFormatBGRA8Unorm;
    }
}

MTLPixelFormat depth_surface_format(u32 f)
{
    // Z24S8 has no 24-bit Metal format on Apple GPUs: Depth32Float_Stencil8 holds it exactly.
    return f == static_cast<u32>(SurfaceDepthFormat::Z16) ? MTLPixelFormatDepth16Unorm : MTLPixelFormatDepth32Float_Stencil8;
}

u32 surface_bytes_per_pixel(u32 gcm_format, bool depth)
{
    if (depth) return gcm_format == static_cast<u32>(SurfaceDepthFormat::Z16) ? 2 : 4;
    return surface_color_bpp(gcm_format);
}

std::array<u8, 4> surface_output_swizzle(u32 gcm_format)
{
    // RPCS3 Utils/color_utils.hpp: B8 stores the shader's blue; G8B8 stores blue in R and green in G.
    switch (static_cast<SurfaceColorFormat>(gcm_format)) {
    case SurfaceColorFormat::B8: return {2, 2, 2, 2};
    case SurfaceColorFormat::G8B8: return {2, 1, 4, 4};
    default: return {0, 1, 2, 3};
    }
}

Surface* MetalBackend::Impl::find_surface(u32 address, bool depth) const
{
    Surface* best = nullptr;
    for (const auto& s : surfaces) {
        if (s->address == address && s->depth == depth && (!best || s->last_used > best->last_used)) best = s.get();
    }
    return best;
}

void MetalBackend::Impl::remove_surface(Surface* s)
{
    if (pass_open && pass.contains(s)) end_pass();
    std::erase_if(surfaces, [s](const std::unique_ptr<Surface>& p) { return p.get() == s; });
}

void MetalBackend::Impl::evict_overlapping(u32 address, u32 size, const Surface* keep)
{
    const u64 end = u64{address} + size;
    std::vector<Surface*> victims;
    for (const auto& s : surfaces) {
        if (s.get() == keep) continue;
        if (u64{s->address} < end && address < u64{s->address} + s->bytes()) victims.push_back(s.get());
    }
    for (Surface* s : victims) {
        if (draw_targets.contains(s)) continue; // another attachment of the same draw (MRT): not a redefinition
        PS_LOG_DEBUG(kChan, "surface at {:#010x} ({}x{} fmt {}) replaced by a new surface at {:#010x}", s->address,
            s->width, s->height, s->gcm_format, address);
        remove_surface(s);
    }
}

Surface* MetalBackend::Impl::make_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth,
    bool swizzled, MTLPixelFormat pf)
{
    Impl& impl = *this;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:width height:height mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [impl.device newTextureWithDescriptor:d];
    if (!tex) return nullptr;
    tex.label = [NSString stringWithFormat:@"rsx surface %08x %ux%u", address, width, height];
    auto s = std::make_unique<Surface>();
    s->address = address;
    s->pitch = pitch ? pitch : width * surface_bytes_per_pixel(format, depth);
    s->width = width;
    s->height = height;
    s->gcm_format = format;
    s->depth = depth;
    s->swizzled = swizzled;
    s->pixel_format = pf;
    s->texture = tex;
    s->last_used = impl.tick;
    impl.stats.surfaces_created++;
    impl.surfaces.push_back(std::move(s));
    return impl.surfaces.back().get();
}

Surface* MetalBackend::Impl::get_color_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool swizzled)
{
    for (const auto& s : surfaces) {
        if (s->address == address && !s->depth && s->gcm_format == format && s->width == width &&
            s->height == height && s->pitch == pitch && s->swizzled == swizzled) {
            s->last_used = tick;
            return s.get();
        }
    }
    const u32 size = (pitch ? pitch : width * surface_color_bpp(format)) * height;
    evict_overlapping(address, size, nullptr);
    return make_surface(address, pitch, width, height, format, false, swizzled, color_surface_format(format));
}

Surface* MetalBackend::Impl::get_depth_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth_float)
{
    (void)depth_float; // Depth32Float holds both the fixed and the float 24-bit encodings
    for (const auto& s : surfaces) {
        if (s->address == address && s->depth && s->gcm_format == format && s->width == width && s->height == height &&
            s->pitch == pitch) {
            s->last_used = tick;
            return s.get();
        }
    }
    const u32 size = (pitch ? pitch : width * surface_bytes_per_pixel(format, true)) * height;
    evict_overlapping(address, size, nullptr);
    return make_surface(address, pitch, width, height, format, true, false, depth_surface_format(format));
}

id<MTLRenderPipelineState> MetalBackend::Impl::blit_pipeline_for(MTLPixelFormat format)
{
    const u32 key = static_cast<u32>(format);
    if (auto it = blit_pipelines.find(key); it != blit_pipelines.end()) return it->second;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [kernels newFunctionWithName:@"rsx_fullscreen_vs"];
    d.fragmentFunction = [kernels newFunctionWithName:@"rsx_blit_fs"];
    d.colorAttachments[0].pixelFormat = format;
    NSError* error = nil;
    id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!p) PS_LOG_ERROR(kChan, "blit pipeline: {}", error.localizedDescription.UTF8String);
    blit_pipelines[key] = p;
    return p;
}

id<MTLTexture> MetalBackend::Impl::copy_texture(id<MTLTexture> src)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                                                 width:src.width
                                                                                height:src.height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> dst = [device newTextureWithDescriptor:d];
    end_pass();
    id<MTLBlitCommandEncoder> blit = [render_buffer() blitCommandEncoder];
    [blit copyFromTexture:src toTexture:dst];
    [blit endEncoding];
    return dst;
}

Surface::Conversion* MetalBackend::Impl::depth_as_texture(Surface& d, u32 mode)
{
    const bool z16 = d.pixel_format == MTLPixelFormatDepth16Unorm;
    if (!d.depth || d.swizzled || (!z16 && d.pixel_format != MTLPixelFormatDepth32Float_Stencil8)) return nullptr;
    Surface::Conversion& conv = d.conversions[mode];
    __strong id<MTLTexture>& tex = conv.texture;
    if (tex && conv.generation == d.generation) return &conv;
    if (!tex) {
        // The texture's own decoded formats (rsx_decode_texture): RGBA8 for 8-bit channels, 16-bit unorm for X16 and
        // Y16_X16, the depth as a float for the depth formats.
        const MTLPixelFormat pf = mode == static_cast<u32>(TexelDecode::X16)      ? MTLPixelFormatR16Unorm
                                : mode == static_cast<u32>(TexelDecode::Y16X16)   ? MTLPixelFormatRG16Unorm
                                : mode == kDepthValueMode                         ? MTLPixelFormatR32Float
                                                                                  : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                         width:d.width
                                                                                        height:d.height
                                                                                     mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
        desc.storageMode = MTLStorageModePrivate;
        tex = [device newTextureWithDescriptor:desc];
        if (!tex) return nullptr;
    }
    id<MTLTexture> stencil = z16 ? dummy_stencil : [d.texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    end_pass();
    id<MTLComputeCommandEncoder> enc = [render_buffer() computeCommandEncoder];
    [enc setComputePipelineState:depth_pipeline];
    [enc setTexture:d.texture atIndex:0];
    [enc setTexture:stencil atIndex:1];
    [enc setTexture:tex atIndex:2];
    const u32 params[4] = {mode, z16 ? 1u : 0u, 0, 0};
    [enc setBytes:params length:sizeof(params) atIndex:0];
    [enc dispatchThreads:MTLSizeMake(d.width, d.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    conv.generation = d.generation;
    stats.depth_conversions++;
    return &conv;
}

void MetalBackend::Impl::upload_surface_from_guest(Surface& s)
{
    // The CPU (or the RSX's own copies) wrote the surface's memory after the GPU copy was made: reload it.
    s.cpu_newer = false;
    const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
    const bool argb = f == SurfaceColorFormat::A8R8G8B8 || f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 ||
        f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8;
    if (s.depth || s.swizzled || !argb) {
        unsupported(std::format("reloading a surface of format {}{} from guest memory is not implemented", s.gcm_format,
            s.depth ? " (depth)" : s.swizzled ? " (swizzled)" : ""));
        return;
    }
    TextureInfo t;
    t.format = tex::A8R8G8B8 | tex::kLinearFlag;
    t.width = static_cast<u16>(s.width);
    t.height = static_cast<u16>(s.height);
    t.pitch = s.pitch;
    t.dimension = 2;
    t.mipmaps = 1;
    t.border_type = true;
    std::string why;
    CachedTexture* c = decode_texture(t, s.address, false, &why);
    if (!c) {
        unsupported(why);
        return;
    }
    id<MTLRenderPipelineState> pso = blit_pipeline_for(s.pixel_format);
    if (!pso) return;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = s.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [render_buffer() renderCommandEncoderWithDescriptor:rp];
    enc.label = @"rsx surface reload";
    [enc setRenderPipelineState:pso];
    const float depth = 0.f;
    [enc setVertexBytes:&depth length:sizeof(depth) atIndex:0];
    const float params[4] = {f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8 ? 1.f : 0.f, 0.f, 0.f, 0.f};
    [enc setFragmentBytes:params length:sizeof(params) atIndex:0];
    [enc setFragmentTexture:c->texture atIndex:0];
    [enc setFragmentSamplerState:nearest_sampler atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    s.initialized = true;
    s.gpu_newer = false;
}

bool MetalBackend::Impl::read_back(Surface& s, std::vector<u8>& raw, bool stencil)
{
    u32 bpp = 0;
    MTLBlitOption options = MTLBlitOptionNone;
    if (stencil) {
        if (s.pixel_format != MTLPixelFormatDepth32Float_Stencil8) return false;
        bpp = 1;
        options = MTLBlitOptionStencilFromDepthStencil;
    } else switch (s.pixel_format) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatR32Float: bpp = 4; break;
    case MTLPixelFormatR8Unorm: bpp = 1; break;
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatDepth16Unorm: bpp = 2; break;
    case MTLPixelFormatRGBA16Float: bpp = 8; break;
    case MTLPixelFormatRGBA32Float: bpp = 16; break;
    case MTLPixelFormatDepth32Float_Stencil8:
        bpp = 4;
        options = MTLBlitOptionDepthFromDepthStencil;
        break;
    default: return false;
    }
    const u32 row = s.width * bpp;
    id<MTLBuffer> buf = [device newBufferWithLength:static_cast<NSUInteger>(row) * s.height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:s.texture
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(s.width, s.height, 1)
                    toBuffer:buf
           destinationOffset:0
      destinationBytesPerRow:row
    destinationBytesPerImage:static_cast<NSUInteger>(row) * s.height
                     options:options];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    raw.assign(static_cast<const u8*>(buf.contents), static_cast<const u8*>(buf.contents) + static_cast<usize>(row) * s.height);
    return true;
}

// A depth buffer to guest memory as the RSX lays it out: Z24S8 as big-endian words, depth in the high 24 bits and
// stencil in the low 8 (RPCS3 surface_store); Z16 as big-endian halfwords.
bool MetalBackend::Impl::write_back_depth(Surface& s)
{
    const bool z16 = s.pixel_format == MTLPixelFormatDepth16Unorm;
    if (s.swizzled || (!z16 && s.pixel_format != MTLPixelFormatDepth32Float_Stencil8)) {
        unsupported(std::format("GPU readback of depth format {}{} is not implemented", s.gcm_format,
            s.swizzled ? " (swizzled)" : ""));
        return false;
    }
    submit(true);
    std::vector<u8> depth, stencil;
    if (!read_back(s, depth) || (!z16 && !read_back(s, stencil, true))) return false;
    stats.readbacks++;
    PS_LOG_WARN(kChan, "expensive GPU readback: depth buffer {:#010x} {}x{} (#{})", s.address, s.width, s.height,
        stats.readbacks);
    u8* const guest = mem.base();
    for (u32 y = 0; y < s.height; y++) {
        u8* row = guest + s.address + y * s.pitch;
        for (u32 x = 0; x < s.width; x++) {
            const usize i = static_cast<usize>(y) * s.width + x;
            if (z16) {
                u16 v;
                std::memcpy(&v, &depth[i * 2], 2);
                ps_st16(row + x * 2, v);
            } else {
                float z;
                std::memcpy(&z, &depth[i * 4], 4);
                const u32 d24 = static_cast<u32>(std::lround(std::clamp(z, 0.f, 1.f) * 16777215.f));
                ps_st32(row + x * 4, (d24 << 8) | stencil[i]);
            }
        }
    }
    tracker.notify_write(s.address, s.bytes());
    s.gpu_newer = false;
    return true;
}

bool MetalBackend::Impl::write_back(Surface& s)
{
    // GPU -> guest memory in the surface's format (RPCS3 surface_store "flush"). A full pipeline sync on a
    // tile-based GPU: plan section 7 wants every occurrence logged and patched out.
    if (s.depth) return write_back_depth(s);
    const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
    const bool argb = !s.depth && (f == SurfaceColorFormat::A8R8G8B8 || f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 ||
                                      f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8);
    const bool abgr = !s.depth && (f == SurfaceColorFormat::A8B8G8R8 || f == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8 ||
                                      f == SurfaceColorFormat::X8B8G8R8_O8B8G8R8);
    // Floating-point targets: each component big-endian, red first in memory (as W16_Z16_Y16_X16_FLOAT,
    // W32_Z32_Y32_X32_FLOAT and X32_FLOAT textures read them).
    const u32 float_bytes = s.depth ? 0
                          : f == SurfaceColorFormat::F_W16Z16Y16X16 ? 2
                          : (f == SurfaceColorFormat::F_W32Z32Y32X32 || f == SurfaceColorFormat::F_X32) ? 4
                                                                                                         : 0;
    const bool b8 = !s.depth && f == SurfaceColorFormat::B8;
    if (s.swizzled || (!argb && !abgr && !float_bytes && !b8)) {
        unsupported(std::format("GPU readback of surface format {}{} is not implemented", s.gcm_format,
            s.depth ? " (depth)" : s.swizzled ? " (swizzled)" : ""));
        return false;
    }
    submit(true);
    std::vector<u8> raw;
    if (!read_back(s, raw)) return false;
    stats.readbacks++;
    PS_LOG_WARN(kChan, "expensive GPU readback: surface {:#010x} {}x{} (#{})", s.address, s.width, s.height, stats.readbacks);

    if (b8) {
        // One byte per pixel, as the Metal copy holds it (the shader's blue: surface_output_swizzle).
        for (u32 y = 0; y < s.height; y++) std::memcpy(mem.base() + s.address + y * s.pitch, &raw[y * s.width], s.width);
        tracker.notify_write(s.address, s.bytes());
        s.gpu_newer = false;
        return true;
    }
    if (float_bytes) {
        // The Metal copy holds the same components little-endian: swap each one into place, row by row.
        const u32 components = f == SurfaceColorFormat::F_X32 ? 1 : 4;
        const usize row = static_cast<usize>(s.width) * components * float_bytes;
        u8* const guest_base = mem.base();
        for (u32 y = 0; y < s.height; y++) {
            const u8* src = &raw[y * row];
            u8* dst = guest_base + s.address + y * s.pitch;
            if (float_bytes == 2) {
                for (usize i = 0; i < row; i += 2) ps_st16(dst + i, static_cast<u16>(src[i] | (src[i + 1] << 8)));
            } else {
                for (usize i = 0; i < row; i += 4) {
                    u32 v;
                    std::memcpy(&v, src + i, 4);
                    ps_st32(dst + i, v);
                }
            }
        }
        tracker.notify_write(s.address, s.bytes());
        s.gpu_newer = false;
        return true;
    }

    const bool x_zero = f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 || f == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8;
    const bool x_one = f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8 || f == SurfaceColorFormat::X8B8G8R8_O8B8G8R8;
    u8* const guest = mem.base();
    for (u32 y = 0; y < s.height; y++) {
        for (u32 x = 0; x < s.width; x++) {
            const u8* p = &raw[(static_cast<usize>(y) * s.width + x) * 4];
            // BGRA8Unorm bytes are b g r a; RGBA8Unorm bytes are r g b a. Guest A8R8G8B8 is a r g b in memory,
            // A8B8G8R8 is a b g r.
            u8 r, g, b, a;
            if (argb) {
                b = p[0], g = p[1], r = p[2], a = p[3];
            } else {
                r = p[0], g = p[1], b = p[2], a = p[3];
            }
            if (x_zero) a = 0;
            if (x_one) a = 0xff;
            u8* dst = guest + s.address + y * s.pitch + x * 4;
            dst[0] = a;
            if (argb) {
                dst[1] = r, dst[2] = g, dst[3] = b;
            } else {
                dst[1] = b, dst[2] = g, dst[3] = r;
            }
        }
    }
    tracker.notify_write(s.address, s.bytes());
    s.gpu_newer = false;
    return true;
}

} // namespace pspocket::rsx::metal
