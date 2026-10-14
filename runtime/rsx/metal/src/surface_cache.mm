// Surface cache: render targets keyed by address, format, pitch and size (plan section 7, "Surfaces"). Surfaces live
// on the GPU; guest memory is only written when the RSX itself reads it (flush_memory: an expensive readback,
// logged) and only read back into the GPU copy when the CPU wrote it (invalidate_memory -> cpu_newer).

#include <chrono>

#include "metal_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

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
    if (pass_open && pass.contains(s)) end_pass_for(kEndSurfaceGone);
    if (closing.encoder && closing.targets.contains(s)) finish_closing(false); // D-945: it stores what it drew
    // A child leaves what it wrote in its parent; children of a parent that goes stand alone (D-122).
    if (Surface* p = s->parent) {
        if (s->newer_than_parent) copy_region(s->texture, 0, 0, p->texture, s->parent_x, s->parent_y, s->width, s->height);
        std::erase(p->children, s);
    }
    for (Surface* c : s->children) {
        c->parent = nullptr;
        c->older_than_parent = c->newer_than_parent = false;
    }
    // A clear that waits on it clears the rest of its targets only (a child's ran above, before its copy; D-945). The
    // open pass's own first-draw clears went with it (it had the surface: it ended above).
    for (ClearOp& op : pending_clears) drop_target(op, s);
    std::erase_if(pending_clears, [](const ClearOp& op) { return !op.targets.color_count && !op.targets.depth; });
    tracker.unwatch(s->watch);
    std::erase_if(surfaces, [s](const std::unique_ptr<Surface>& p) { return p.get() == s; });
}

// Two surfaces overlap where their pixels do. Surfaces that share a pitch can interleave in memory row by row without
// touching: GTA V renders its shadow cascades into 128x128 tiles of one depth atlas (zeta offsets 0x100 apart, pitch
// 0x400), and each tile replaced the one before (run 070: the cascades were read from stale memory). Otherwise, and
// for rows that wrap past the pitch, where their bytes do.
bool surfaces_overlap(u32 a, u32 a_pitch, u32 a_row, u32 a_rows, u32 b, u32 b_pitch, u32 b_row, u32 b_rows)
{
    if (a_pitch && a_pitch == b_pitch) {
        const u32 base = std::min(a, b);
        const u32 ax = (a - base) % a_pitch, ay = (a - base) / a_pitch, bx = (b - base) % b_pitch, by = (b - base) / b_pitch;
        if (ax + a_row <= a_pitch && bx + b_row <= b_pitch)
            return ay < by + b_rows && by < ay + a_rows && ax < bx + b_row && bx < ax + a_row;
    }
    return u64{a} < u64{b} + u64{b_pitch} * b_rows && u64{b} < u64{a} + u64{a_pitch} * a_rows;
}

void MetalBackend::Impl::evict_overlapping(u32 address, u32 pitch, u32 row_bytes, u32 rows, const Surface* keep,
    const Surface* keep_too)
{
    std::vector<Surface*> victims;
    for (const auto& s : surfaces) {
        if (s.get() == keep || s.get() == keep_too) continue;
        const u32 s_row = s->width * surface_bytes_per_pixel(s->gcm_format, s->depth);
        if (surfaces_overlap(address, pitch, row_bytes, rows, s->address, s->pitch, s_row, s->height)) victims.push_back(s.get());
    }
    for (Surface* s : victims) {
        if (draw_targets.contains(s)) continue; // another attachment of the same draw (MRT): not a redefinition
        PS_LOG_DEBUG(kChan, "surface at {:#010x} ({}x{} fmt {}) replaced by a new surface at {:#010x}", s->address,
            s->width, s->height, s->gcm_format, address);
        remove_surface(s);
    }
}

// ---- targets inside targets (D-122) -------------------------------------------------------------------------------

// GTA V draws its characters' damage into a 2560x512 target, then clears a 640x256 one 256 rows into it with the same
// pitch; the guard of the first mission samples the large one. A new surface for the small one evicted the large one,
// and the guard came out black. The small one is now a child: it keeps its own texture (a pass with it has the
// depth buffer at its own origin, which one texture with the large one could not) and the large one stays.
Surface* MetalBackend::Impl::find_parent(u32 address, u32 pitch, u32 width, u32 height, u32 format) const
{
    const u32 bpp = surface_color_bpp(format);
    if (bpp == 0 || pitch == 0) return nullptr;
    Surface* best = nullptr;
    for (const auto& p : surfaces) {
        if (p->depth || p->swizzled || p->gcm_format != format || p->pitch != pitch || p->parent) continue;
        if (address <= p->address || address - p->address >= p->bytes()) continue;
        const u32 offset = address - p->address;
        if ((offset % pitch) % bpp) continue;
        const u32 x = (offset % pitch) / bpp, y = offset / pitch;
        if (x + width > p->width || y + height > p->height) continue;
        if (!best || p->last_used > best->last_used) best = p.get();
    }
    return best;
}

void MetalBackend::Impl::copy_region(id<MTLTexture> src, u32 sx, u32 sy, id<MTLTexture> dst, u32 dx, u32 dy, u32 w,
    u32 h)
{
    end_pass_for(kEndRegionCopy);
    id<MTLBlitCommandEncoder> blit = [render_buffer() blitCommandEncoder];
    [blit copyFromTexture:src
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(sx, sy, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                toTexture:dst
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(dx, dy, 0)];
    [blit endEncoding];
    stats.surface_region_copies++;
}

// Before s is drawn to, sampled or read back: what its children wrote goes into their places in it; a child whose
// parent was written since takes its place back.
void MetalBackend::Impl::sync_surface(Surface& s)
{
    for (Surface* c : s.children) {
        if (!c->newer_than_parent) continue;
        copy_region(c->texture, 0, 0, s.texture, c->parent_x, c->parent_y, c->width, c->height);
        c->newer_than_parent = false;
        s.initialized = true;
        s.gpu_newer = true;
        s.generation++;
    }
    if (Surface* p = s.parent; p && s.older_than_parent) {
        sync_surface(*p); // its other children first
        copy_region(p->texture, s.parent_x, s.parent_y, s.texture, 0, 0, s.width, s.height);
        s.older_than_parent = false;
        s.initialized = true;
        s.gpu_newer = p->gpu_newer;
        s.generation++;
    }
}

void MetalBackend::Impl::surface_written(Surface& s)
{
    if (s.parent) {
        s.newer_than_parent = true;
        s.older_than_parent = false;
    }
    for (Surface* c : s.children) c->older_than_parent = true;
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
    s->serial = ++impl.surface_serial;
    s->last_used = impl.tick;
    s->last_frame = impl.stats.flips;
    impl.stats.surfaces_created++;
    impl.note_guest(*s);
    impl.surfaces.push_back(std::move(s));
    return impl.surfaces.back().get();
}

Surface* MetalBackend::Impl::get_color_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool swizzled)
{
    for (const auto& s : surfaces) {
        if (s->address == address && !s->depth && s->gcm_format == format && s->width == width &&
            s->height == height && s->pitch == pitch && s->swizzled == swizzled) {
            s->last_used = tick;
            s->last_frame = stats.flips;
            check_cpu_writes(*s);
            sync_surface(*s);
            return s.get();
        }
    }
    // A target at the start of a larger one, with its pitch and format, is its top-left corner: the same memory, so
    // the same texture, drawn in that corner (the pass is the surface clip). A larger one over a smaller one takes
    // what the smaller one held. Made anew, the larger one lost what lay outside the smaller one: GTA V clears the
    // right half of a 2560x512 target for its characters, then draws into its left half as a 1280x512 one, and they
    // sampled the half that was never cleared (black, D-120).
    Surface* inner = nullptr;
    if (!swizzled) {
        for (const auto& s : surfaces) {
            if (s->address != address || s->depth || s->swizzled || s->gcm_format != format || s->pitch != pitch) continue;
            if (s->width >= width && s->height >= height) {
                s->last_used = tick;
                s->last_frame = stats.flips;
                check_cpu_writes(*s);
                sync_surface(*s);
                return s.get();
            }
            if (s->width <= width && s->height <= height) inner = s.get();
        }
    }
    Surface* parent = swizzled || inner ? nullptr : find_parent(address, pitch, width, height, format);
    const u32 row = width * surface_color_bpp(format);
    evict_overlapping(address, pitch ? pitch : row, row, height, inner, parent);
    Surface* made = make_surface(address, pitch, width, height, format, false, swizzled, color_surface_format(format));
    if (inner) {
        if (made && inner->initialized) grow_surface(*made, *inner);
        remove_surface(inner);
    }
    if (parent && made) {
        made->parent = parent;
        made->parent_x = ((address - parent->address) % pitch) / surface_color_bpp(format);
        made->parent_y = (address - parent->address) / pitch;
        parent->children.push_back(made);
        made->older_than_parent = parent->initialized; // starts as its place in the parent holds
        sync_surface(*made);
        stats.surface_children++;
    }
    return made;
}

// A larger target over a smaller one at its start: guest memory under it, then the smaller one's pixels in its corner.
void MetalBackend::Impl::grow_surface(Surface& outer, Surface& inner)
{
    if (readback_supported(outer)) {
        outer.cpu_newer = true;
        end_pass_for(kEndGrow);
        upload_surface_from_guest(outer); // what lies outside the smaller one, as guest memory holds it
    }
    if (!inner.cpu_newer) {
        end_pass_for(kEndGrow);
        id<MTLBlitCommandEncoder> blit = [render_buffer() blitCommandEncoder];
        [blit copyFromTexture:inner.texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(inner.width, inner.height, 1)
                    toTexture:outer.texture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    }
    outer.initialized = true;
    outer.gpu_newer = inner.gpu_newer || outer.gpu_newer;
    outer.generation++;
    surface_written(outer);
    stats.surfaces_grown++;
}

Surface* MetalBackend::Impl::get_depth_surface(u32 address, u32 pitch, u32 width, u32 height, u32 format, bool depth_float)
{
    (void)depth_float; // Depth32Float holds both the fixed and the float 24-bit encodings
    for (const auto& s : surfaces) {
        if (s->address == address && s->depth && s->gcm_format == format && s->width == width && s->height == height &&
            s->pitch == pitch) {
            s->last_used = tick;
            s->last_frame = stats.flips;
            return s.get();
        }
    }
    // A smaller clip over a depth buffer with its pitch: its top-left corner (D-120). GTA V's character pass uses its
    // 1280x720 depth buffer at 1280x512, which made a new one each frame and lost what the frame had drawn.
    for (const auto& s : surfaces) {
        if (s->address == address && s->depth && s->gcm_format == format && s->pitch == pitch && s->width >= width &&
            s->height >= height) {
            s->last_used = tick;
            s->last_frame = stats.flips;
            return s.get();
        }
    }
    const u32 row = width * surface_bytes_per_pixel(format, true);
    evict_overlapping(address, pitch ? pitch : row, row, height, nullptr);
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

// A draw samples a target it also draws to: it reads a copy, taken before it (D-148). The copy is the surface's own,
// made again only after a write to the surface (its generations), so draws that do not write the target they sample
// (GTA V's, colour mask 0 there) share one copy and keep their pass. A new copy is a new texture: copying into the
// one earlier draws read made the GPU wait for them each time (GPU time per frame 8.5 ms to 22 ms on the Mac walk);
// the old one goes when the GPU is done with it, so a frame holds as many as it has writes between such draws.
//
// The RSX reads such a target through its texture cache, which sees what draws wrote to it only after the game
// invalidates the cache (cellGcmSetInvalidateTextureCache, NV4097_INVALIDATE_L2): until then a draw may read what the
// cache took in before. The copy plays the cache (D-947): kept, though the target was written, while the draws go on to
// the same targets and no invalidation came, so a run of such draws keeps its pass (GTA V's: up to 400 copies and
// passes a frame on the iPhone, 80 to 155 ms of GPU time, run 327; RPCS3 samples the target in place unless in its
// strict rendering mode, texture_cache_helpers.h force_strict_fbo_sampling). The depth bounds test's copy is not one.
id<MTLTexture> MetalBackend::Impl::feedback_copy(Surface& s, bool depth_only)
{
    Surface::Conversion& c = s.feedback;
    const bool written = c.generation != s.generation || (!depth_only && c.stencil_generation != s.stencil_generation);
    bool fresh = !c.texture || written;
    const bool cached = feedback_cached && !depth_only && ab_on();
    if (fresh && c.texture && cached && c.bind_epoch == bind_epoch && c.cache_epoch == cache_epoch) {
        fresh = false;
        stats.feedback_reused++;
        feedback_sources[feedback_source(s)].reused++;
    }
    if (fresh && !depth_only) {
        if (!c.texture) stats.feedback_first++;
        else if (!cached) stats.feedback_strict++;
        else if (c.bind_epoch != bind_epoch) stats.feedback_rebound++;
        else stats.feedback_invalidated++;
        feedback_sources[feedback_source(s)].made++;
    }
    if (fresh) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s.texture.pixelFormat
                                                                                     width:s.texture.width
                                                                                    height:s.texture.height
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        d.storageMode = MTLStorageModePrivate;
        c.texture = [device newTextureWithDescriptor:d];
        c.views.clear();
        end_pass_for(kEndFeedbackCopy);
        id<MTLBlitCommandEncoder> blit = [render_buffer() blitCommandEncoder];
        [blit copyFromTexture:s.texture toTexture:c.texture];
        [blit endEncoding];
        c.generation = s.generation;
        c.stencil_generation = s.stencil_generation;
        c.bind_epoch = bind_epoch;
        c.cache_epoch = cache_epoch;
        stats.feedback_copies++;
    }
    return c.texture;
}

std::string MetalBackend::Impl::feedback_source(const Surface& s) const
{
    return std::format("fragment program {:#x} unit {} reading target {:#010x} {}x{} format {}", feedback_fp,
        feedback_unit, s.address, s.width, s.height, s.gcm_format);
}

Surface::Conversion* MetalBackend::Impl::depth_as_texture(Surface& d, u32 mode)
{
    const bool z16 = d.pixel_format == MTLPixelFormatDepth16Unorm;
    if (!d.depth || d.swizzled || (!z16 && d.pixel_format != MTLPixelFormatDepth32Float_Stencil8)) return nullptr;
    Surface::Conversion& conv = d.conversions[mode];
    __strong id<MTLTexture>& tex = conv.texture;
    // Up to date unless the depth changed, or the stencil for a format that sees its bits (Z24S8 read as A8R8G8B8,
    // D8R8G8B8 or Y16_X16). The depth buffer of the pass that reads it (GTA V's lighting tests against it and marks
    // light volumes in its stencil) is read as it was at the conversion: such feedback is undefined on the RSX, and
    // converting again at every draw split the pass each time (D-105).
    const bool reads_stencil = !z16 && mode != kDepthValueMode;
    const bool feedback = draw_targets.depth == &d;
    if (tex && conv.generation == d.generation &&
        (!reads_stencil || feedback || conv.stencil_generation == d.stencil_generation)) {
        return &conv;
    }
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
    end_pass_for(kEndDepthConversion);
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
    conv.stencil_generation = d.stencil_generation;
    stats.depth_conversions++;
    return &conv;
}

// A colour target read as a texture of another format whose texels are its pixels (D-098): GTA V's first mission reads
// a 640x360 A8B8G8R8 target as Y16_X16 every frame. Converted on the GPU from the target's bytes, as RPCS3 does, instead
// of a write-back and a decode from memory (run 066: a GPU sync each time).
Surface::Conversion* MetalBackend::Impl::color_as_texture(Surface& s, u32 mode)
{
    const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
    const bool argb = f == SurfaceColorFormat::A8R8G8B8 || f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 ||
                      f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8;
    const bool abgr = f == SurfaceColorFormat::A8B8G8R8 || f == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8 ||
                      f == SurfaceColorFormat::X8B8G8R8_O8B8G8R8;
    if (s.depth || s.swizzled || (!argb && !abgr) || mode != static_cast<u32>(TexelDecode::Y16X16)) return nullptr;
    Surface::Conversion& conv = s.conversions[mode];
    __strong id<MTLTexture>& tex = conv.texture;
    if (tex && conv.generation == s.generation) return &conv;
    if (!tex) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Unorm
                                                                                         width:s.width
                                                                                        height:s.height
                                                                                     mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
        desc.storageMode = MTLStorageModePrivate;
        tex = [device newTextureWithDescriptor:desc];
        if (!tex) return nullptr;
    }
    const bool x_zero = f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 || f == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8;
    const bool x_one = f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8 || f == SurfaceColorFormat::X8B8G8R8_O8B8G8R8;
    end_pass_for(kEndColorConversion);
    id<MTLComputeCommandEncoder> enc = [render_buffer() computeCommandEncoder];
    [enc setComputePipelineState:color_pipeline];
    [enc setTexture:s.texture atIndex:0];
    [enc setTexture:tex atIndex:1];
    const u32 params[4] = {mode, abgr ? 1u : 0u, x_zero ? 1u : x_one ? 2u : 0u, 0};
    [enc setBytes:params length:sizeof(params) atIndex:0];
    [enc dispatchThreads:MTLSizeMake(s.width, s.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    conv.generation = s.generation;
    stats.color_conversions++;
    return &conv;
}

void MetalBackend::Impl::upload_surface_from_guest(Surface& s)
{
    // The CPU (or the RSX's own copies) wrote the surface's memory after the GPU copy was made: reload it.
    s.cpu_newer = false;
    stats.surface_reloads++;
    const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
    const bool argb = !s.depth && (f == SurfaceColorFormat::A8R8G8B8 || f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 ||
                                      f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8);
    if (!argb || s.swizzled) {
        if (!readback_supported(s)) {
            unsupported(std::format("reloading a surface of format {}{} from guest memory is not implemented",
                s.gcm_format, s.depth ? " (depth)" : s.swizzled ? " (swizzled)" : ""));
            return;
        }
        upload_surface_bytes(s);
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
// ---- write-backs (RPCS3 surface_store "flush"): GPU texels to guest memory in the surface's format ---------------

bool MetalBackend::Impl::readback_supported(const Surface& s)
{
    // Swizzled targets (Morton order, D-111): colour ones of power-of-two sizes, as RSX swizzled surfaces are.
    if (s.swizzled && (s.depth || !std::has_single_bit(s.width) || !std::has_single_bit(s.height))) return false;
    if (s.depth) return s.pixel_format == MTLPixelFormatDepth16Unorm || s.pixel_format == MTLPixelFormatDepth32Float_Stencil8;
    const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
    switch (f) {
    case SurfaceColorFormat::A8R8G8B8: case SurfaceColorFormat::X8R8G8B8_Z8R8G8B8: case SurfaceColorFormat::X8R8G8B8_O8R8G8B8:
    case SurfaceColorFormat::A8B8G8R8: case SurfaceColorFormat::X8B8G8R8_Z8B8G8R8: case SurfaceColorFormat::X8B8G8R8_O8B8G8R8:
    case SurfaceColorFormat::F_W16Z16Y16X16: case SurfaceColorFormat::F_W32Z32Y32X32: case SurfaceColorFormat::F_X32:
    case SurfaceColorFormat::B8: case SurfaceColorFormat::G8B8: case SurfaceColorFormat::R5G6B5:
    case SurfaceColorFormat::X1R5G5B5_Z1R5G5B5: case SurfaceColorFormat::X1R5G5B5_O1R5G5B5: return true;
    default: return false;
    }
}

// The surface's texels (and a Z24S8 buffer's stencil) blitted into shared buffers at the end of the render buffer.
MetalBackend::Impl::Readback MetalBackend::Impl::encode_readback(Surface& s)
{
    end_pass(); // a pass with only a load-action clear so far runs first
    sync_surface(s); // what its children wrote, or its parent (D-122)
    Readback r;
    r.image = {s.address, s.pitch, s.width, s.height, s.gcm_format, s.depth, s.swizzled, s.pixel_format};
    u32 bpp = 4;
    MTLBlitOption options = MTLBlitOptionNone;
    switch (s.pixel_format) {
    case MTLPixelFormatR8Unorm: bpp = 1; break;
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatDepth16Unorm: bpp = 2; break;
    case MTLPixelFormatRGBA16Float: bpp = 8; break;
    case MTLPixelFormatRGBA32Float: bpp = 16; break;
    case MTLPixelFormatDepth32Float_Stencil8: options = MTLBlitOptionDepthFromDepthStencil; break;
    default: break;
    }
    id<MTLBlitCommandEncoder> blit = [render_buffer() blitCommandEncoder];
    const auto copy = [&](u32 b, MTLBlitOption o) {
        const u32 row = s.width * b;
        id<MTLBuffer> buf = [device newBufferWithLength:static_cast<NSUInteger>(row) * s.height
                                                options:MTLResourceStorageModeShared];
        [blit copyFromTexture:s.texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(s.width, s.height, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:static_cast<NSUInteger>(row) * s.height
                         options:o];
        return buf;
    };
    r.data = copy(bpp, options);
    if (s.pixel_format == MTLPixelFormatDepth32Float_Stencil8) r.stencil = copy(1, MTLBlitOptionStencilFromDepthStencil);
    [blit endEncoding];
    return r;
}

void MetalBackend::Impl::store_readback(const Readback& r)
{
    const SurfaceImage& s = r.image;
    const u8* raw = static_cast<const u8*>(r.data.contents);
    u8* const guest = mem.base();
    if (s.depth) {
        // Z24S8 as big-endian words, depth in the high 24 bits and stencil in the low 8 (RPCS3 surface_store); Z16 as
        // big-endian halfwords.
        const bool z16 = s.pixel_format == MTLPixelFormatDepth16Unorm;
        const u8* stencil = r.stencil ? static_cast<const u8*>(r.stencil.contents) : nullptr;
        for (u32 y = 0; y < s.height; y++) {
            u8* row = guest + s.address + y * s.pitch;
            for (u32 x = 0; x < s.width; x++) {
                const usize i = static_cast<usize>(y) * s.width + x;
                if (z16) {
                    u16 v;
                    std::memcpy(&v, &raw[i * 2], 2);
                    ps_st16(row + x * 2, v);
                } else {
                    float z;
                    std::memcpy(&z, &raw[i * 4], 4);
                    const u32 d24 = static_cast<u32>(std::lround(std::clamp(z, 0.f, 1.f) * 16777215.f));
                    ps_st32(row + x * 4, (d24 << 8) | (stencil ? stencil[i] : 0));
                }
            }
        }
    } else {
        // Colour: a linear image at `out` (guest memory, or for a swizzled target a temporary scattered in Morton
        // order below, D-111).
        const u32 gbpp = surface_color_bpp(s.gcm_format);
        std::vector<u8> linear;
        u8* out = guest + s.address;
        u32 out_pitch = s.pitch;
        if (s.swizzled) {
            linear.resize(static_cast<usize>(s.width) * gbpp * s.height);
            out = linear.data();
            out_pitch = s.width * gbpp;
        }
        const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
        const bool argb = f == SurfaceColorFormat::A8R8G8B8 || f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 ||
                          f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8;
        // Floating-point targets: each component big-endian, red first in memory (as W16_Z16_Y16_X16_FLOAT,
        // W32_Z32_Y32_X32_FLOAT and X32_FLOAT textures read them).
        const u32 float_bytes = f == SurfaceColorFormat::F_W16Z16Y16X16                                         ? 2
                              : (f == SurfaceColorFormat::F_W32Z32Y32X32 || f == SurfaceColorFormat::F_X32) ? 4
                                                                                                           : 0;
        const bool r5g6b5 = f == SurfaceColorFormat::R5G6B5;
        const bool x1r5g5b5 = f == SurfaceColorFormat::X1R5G5B5_Z1R5G5B5 || f == SurfaceColorFormat::X1R5G5B5_O1R5G5B5;
        if (f == SurfaceColorFormat::B8) {
            // One byte per pixel, as the Metal copy holds it (the shader's blue: surface_output_swizzle).
            for (u32 y = 0; y < s.height; y++) std::memcpy(out + y * out_pitch, &raw[y * s.width], s.width);
        } else if (f == SurfaceColorFormat::G8B8) {
            // The RG8 copy holds blue in R and green in G (surface_output_swizzle); memory holds G then B (D-112).
            for (u32 y = 0; y < s.height; y++) {
                for (u32 x = 0; x < s.width; x++) {
                    const u8* p = &raw[(static_cast<usize>(y) * s.width + x) * 2];
                    u8* dst = out + y * out_pitch + x * 2;
                    dst[0] = p[1], dst[1] = p[0];
                }
            }
        } else if (r5g6b5 || x1r5g5b5) {
            // 16-bit big-endian, from the BGRA8 copy they render to, rounded to 5 and 6 bits (D-112).
            const u16 x = f == SurfaceColorFormat::X1R5G5B5_O1R5G5B5 ? 0x8000 : 0;
            const auto q = [](u32 v, u32 max) { return (v * max + 127) / 255; };
            for (u32 y = 0; y < s.height; y++) {
                for (u32 x2 = 0; x2 < s.width; x2++) {
                    const u8* p = &raw[(static_cast<usize>(y) * s.width + x2) * 4]; // b g r a
                    const u16 v = r5g6b5 ? static_cast<u16>((q(p[2], 31) << 11) | (q(p[1], 63) << 5) | q(p[0], 31))
                                         : static_cast<u16>(x | (q(p[2], 31) << 10) | (q(p[1], 31) << 5) | q(p[0], 31));
                    ps_st16(out + y * out_pitch + x2 * 2, v);
                }
            }
        } else if (float_bytes) {
            // The Metal copy holds the same components little-endian: swap each one into place, row by row.
            const u32 components = f == SurfaceColorFormat::F_X32 ? 1 : 4;
            const usize row = static_cast<usize>(s.width) * components * float_bytes;
            for (u32 y = 0; y < s.height; y++) {
                const u8* src = &raw[y * row];
                u8* dst = out + y * out_pitch;
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
        } else {
            const bool x_zero = f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8 || f == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8;
            const bool x_one = f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8 || f == SurfaceColorFormat::X8B8G8R8_O8B8G8R8;
            for (u32 y = 0; y < s.height; y++) {
                for (u32 x = 0; x < s.width; x++) {
                    const u8* p = &raw[(static_cast<usize>(y) * s.width + x) * 4];
                    // BGRA8Unorm bytes are b g r a; RGBA8Unorm bytes are r g b a. Guest A8R8G8B8 is a r g b in memory,
                    // A8B8G8R8 is a b g r.
                    u8 rr, g, b, a;
                    if (argb) {
                        b = p[0], g = p[1], rr = p[2], a = p[3];
                    } else {
                        rr = p[0], g = p[1], b = p[2], a = p[3];
                    }
                    if (x_zero) a = 0;
                    if (x_one) a = 0xff;
                    u8* dst = out + y * out_pitch + x * 4;
                    dst[0] = a;
                    if (argb) {
                        dst[1] = rr, dst[2] = g, dst[3] = b;
                    } else {
                        dst[1] = b, dst[2] = g, dst[3] = rr;
                    }
                }
            }
        }
        if (s.swizzled) {
            const u32 lw = static_cast<u32>(std::countr_zero(s.width)), lh = static_cast<u32>(std::countr_zero(s.height));
            for (u32 y = 0; y < s.height; y++)
                for (u32 x = 0; x < s.width; x++)
                    std::memcpy(guest + s.address + morton_index(x, y, lw, lh) * gbpp, out + y * out_pitch + x * gbpp, gbpp);
        }
    }
    own_write();
    tracker.notify_write(s.address, s.pitch * s.height);
    if (check_surfaces) {
        for (auto& c : surfaces) {
            if (c->address == s.address && c->pitch == s.pitch && c->height == s.height) note_guest(*c);
        }
    }
    if (r.mirror) {
        for (auto& c : surfaces) {
            if (c->texture != r.mirror) continue;
            if (c->generation == r.generation) {
                c->gpu_newer = false; // guest memory holds what the GPU does
                tracker.unwatch(c->watch);
                c->watch = tracker.watch(c->address, c->bytes());
            }
            break;
        }
    }
}

void MetalBackend::Impl::note_guest(Surface& s)
{
    if (!check_surfaces || !mem.is_committed(s.address) || !mem.is_committed(s.address + s.bytes() - 1)) return;
    s.guest_hash = WriteTracker::content_hash(mem.host(s.address), s.bytes());
}

void MetalBackend::Impl::check_guest(const Surface& s, const TextureInfo& t, u32 unit)
{
    if (!check_surfaces || !mem.is_committed(s.address) || !mem.is_committed(s.address + s.bytes() - 1)) return;
    if (WriteTracker::content_hash(mem.host(s.address), s.bytes()) == s.guest_hash) return;
    stats.surface_guest_changes++;
    static std::unordered_set<u64> seen;
    const u64 key = (u64{s.address} << 16) ^ t.base_format() ^ (u64{s.width} << 48);
    if (seen.size() < 64 && seen.insert(key).second) {
        PS_LOG_WARN(kChan, "texture unit {} ({:#04x} {}x{}) samples the surface at {:#010x} ({}x{} fmt {}{}), whose guest "
            "memory changed since it was made or written back: a write the backend was not told of", unit,
            t.base_format(), t.width, t.height, s.address, s.width, s.height, s.gcm_format, s.depth ? " depth" : "");
    }
}

void MetalBackend::Impl::check_cpu_writes(Surface& s)
{
    if (s.watch.active && tracker.is_dirty(s.watch)) {
        tracker.unwatch(s.watch);
        s.cpu_newer = true;
    }
}

// Guest memory to the surface's texels, the inverse of store_readback (D-110): its bytes in the Metal format, through a
// staging buffer and a blit (a Z24S8 buffer's depth and stencil apart).
void MetalBackend::Impl::upload_surface_bytes(Surface& s)
{
    const u32 w = s.width, h = s.height;
    if (!mem.is_committed(s.address) || !mem.is_committed(s.address + s.bytes() - 1)) {
        unsupported(std::format("surface at {:#010x} is not in committed guest memory: not reloaded", s.address));
        return;
    }
    const u8* guest = mem.base() + s.address;
    u32 guest_pitch = s.pitch;
    std::vector<u8> linear;
    if (s.swizzled) {
        // Morton order to a linear image first (D-111).
        const u32 gbpp = surface_color_bpp(s.gcm_format);
        const u32 lw = static_cast<u32>(std::countr_zero(w)), lh = static_cast<u32>(std::countr_zero(h));
        linear.resize(static_cast<usize>(w) * gbpp * h);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                std::memcpy(&linear[(static_cast<usize>(y) * w + x) * gbpp], guest + morton_index(x, y, lw, lh) * gbpp, gbpp);
        guest = linear.data();
        guest_pitch = w * gbpp;
    }
    const bool z24s8 = s.pixel_format == MTLPixelFormatDepth32Float_Stencil8;
    u32 bpp = 4;
    switch (s.pixel_format) {
    case MTLPixelFormatR8Unorm: bpp = 1; break;
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatDepth16Unorm: bpp = 2; break;
    case MTLPixelFormatRGBA16Float: bpp = 8; break;
    case MTLPixelFormatRGBA32Float: bpp = 16; break;
    default: break;
    }
    const u32 row = w * bpp;
    TransientAllocator::Allocation texels = transient.alloc(row * h, 256);
    TransientAllocator::Allocation stencil{};
    if (z24s8) stencil = transient.alloc(w * h, 256);
    const auto f = static_cast<SurfaceColorFormat>(s.gcm_format);
    for (u32 y = 0; y < h; y++) {
        const u8* src = guest + y * guest_pitch;
        u8* dst = texels.data + static_cast<usize>(y) * row;
        if (s.depth) {
            for (u32 x = 0; x < w; x++) {
                if (!z24s8) {
                    const u16 v = ps_ld16(src + x * 2);
                    std::memcpy(dst + x * 2, &v, 2);
                } else {
                    const u32 v = ps_ld32(src + x * 4);
                    const float z = static_cast<float>(v >> 8) / 16777215.f;
                    std::memcpy(dst + x * 4, &z, 4);
                    stencil.data[static_cast<usize>(y) * w + x] = static_cast<u8>(v);
                }
            }
        } else if (f == SurfaceColorFormat::B8) {
            std::memcpy(dst, src, w);
        } else if (f == SurfaceColorFormat::G8B8) {
            for (u32 x = 0; x < w; x++) dst[x * 2] = src[x * 2 + 1], dst[x * 2 + 1] = src[x * 2]; // G B -> R(blue) G
        } else if (f == SurfaceColorFormat::R5G6B5 || f == SurfaceColorFormat::X1R5G5B5_Z1R5G5B5 ||
                   f == SurfaceColorFormat::X1R5G5B5_O1R5G5B5) {
            // Expanded to the BGRA8 copy they render to (D-112).
            const bool r5g6b5 = f == SurfaceColorFormat::R5G6B5;
            const u8 alpha = f == SurfaceColorFormat::X1R5G5B5_Z1R5G5B5 ? 0 : 0xff;
            const auto e5 = [](u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); };
            const auto e6 = [](u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); };
            for (u32 x = 0; x < w; x++) {
                const u32 v = ps_ld16(src + x * 2);
                u8* t = dst + x * 4;
                if (r5g6b5) {
                    t[0] = e5(v & 31), t[1] = e6((v >> 5) & 63), t[2] = e5(v >> 11);
                } else {
                    t[0] = e5(v & 31), t[1] = e5((v >> 5) & 31), t[2] = e5((v >> 10) & 31);
                }
                t[3] = alpha;
            }
        } else if (f == SurfaceColorFormat::F_W16Z16Y16X16) {
            for (u32 i = 0; i < row; i += 2) {
                const u16 v = ps_ld16(src + i);
                std::memcpy(dst + i, &v, 2);
            }
        } else if (f == SurfaceColorFormat::F_W32Z32Y32X32 || f == SurfaceColorFormat::F_X32) {
            for (u32 i = 0; i < row; i += 4) {
                const u32 v = ps_ld32(src + i);
                std::memcpy(dst + i, &v, 4);
            }
        } else {
            // A8B8G8R8: a b g r in memory, r g b a in the RGBA8Unorm copy; A8R8G8B8 (swizzled ones come here): a r g b,
            // b g r a in the BGRA8Unorm copy. Either way the bytes reverse; the X8 forms' X reads as zero or one.
            const bool x_zero = f == SurfaceColorFormat::X8B8G8R8_Z8B8G8R8 || f == SurfaceColorFormat::X8R8G8B8_Z8R8G8B8;
            const bool x_one = f == SurfaceColorFormat::X8B8G8R8_O8B8G8R8 || f == SurfaceColorFormat::X8R8G8B8_O8R8G8B8;
            for (u32 x = 0; x < w; x++) {
                const u8* p = src + x * 4;
                u8* t = dst + x * 4;
                t[0] = p[3], t[1] = p[2], t[2] = p[1];
                t[3] = x_zero ? 0 : x_one ? 0xff : p[0];
            }
        }
    }
    // Called while a pass opens, before its encoder starts (as the A8R8G8B8 reload): no encoder is open.
    PS_CHECK(encoder == nil);
    id<MTLBlitCommandEncoder> blit = [render_buffer() blitCommandEncoder];
    blit.label = @"rsx surface reload";
    const auto copy = [&](const TransientAllocator::Allocation& a, u32 bytes_per_row, MTLBlitOption o) {
        [blit copyFromBuffer:a.buffer
                   sourceOffset:a.offset
              sourceBytesPerRow:bytes_per_row
            sourceBytesPerImage:static_cast<NSUInteger>(bytes_per_row) * h
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:s.texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)
                        options:o];
    };
    copy(texels, row, z24s8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone);
    if (z24s8) copy(stencil, w, MTLBlitOptionStencilFromDepthStencil);
    [blit endEncoding];
    s.initialized = true;
    s.gpu_newer = false;
    s.generation++;
    if (s.depth) s.stencil_generation++;
}

bool MetalBackend::Impl::write_back_depth(Surface& s, const char* why) { return write_back(s, why); }

bool MetalBackend::Impl::write_back(Surface& s, const char* why)
{
    // A full pipeline sync on a tile-based GPU: plan section 7 wants every occurrence logged and patched out.
    if (!readback_supported(s)) {
        unsupported(std::format("GPU readback of surface format {}{} is not implemented", s.gcm_format,
            s.depth ? " (depth)" : s.swizzled ? " (swizzled)" : ""));
        return false;
    }
    before_access(s.address, s.bytes(), true); // an older deferred copy of this surface lands first (D-107)
    const Readback r = encode_readback(s);
    {
        const auto t0 = std::chrono::steady_clock::now();
        submit(true);
        stats.readback_wait_ns += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    stats.readbacks++;
    PS_LOG_WARN(kChan, "expensive GPU readback: {} {:#010x} {}x{} for {} (#{})", s.depth ? "depth buffer" : "surface",
        s.address, s.width, s.height, why, stats.readbacks);
    store_readback(r);
    s.gpu_newer = false;
    return true;
}

usize MetalBackend::Impl::deferred_conflict(u32 addr, u32 size, bool write) const
{
    const auto overlaps = [&](u32 a, u32 n) { return n && size && u64{a} < u64{addr} + size && addr < u64{a} + n; };
    usize count = 0;
    for (usize i = 0; i < deferred.size(); i++) {
        const Deferred& d = deferred[i];
        bool hit = overlaps(d.write_addr, d.write_size);
        if (write && !hit) {
            hit = overlaps(d.read_addr, d.read_size);
            for (const Readback& r : d.readbacks) hit = hit || overlaps(r.image.address, r.image.pitch * r.image.height);
        }
        if (hit) count = i + 1;
    }
    return count;
}

void MetalBackend::Impl::before_access(u32 addr, u32 size, bool write)
{
    if (deferred.empty()) return;
    if (const usize n = deferred_conflict(addr, size, write)) {
        stats.deferred_conflicts++;
        if (stats.deferred_conflicts <= 16) {
            const Deferred& d = deferred[n - 1];
            PS_LOG_INFO(kChan, "{} of {:#010x} (+{:#x}) waits for a deferred copy {:#010x} (+{:#x}) -> {:#010x} (+{:#x})",
                write ? "a write" : "a read", addr, size, d.read_addr, d.read_size, d.write_addr, d.write_size);
        }
        run_deferred(true, n);
    }
}

void MetalBackend::Impl::run_deferred(bool wait, usize count)
{
    for (; count && !deferred.empty(); count--) {
        Deferred& d = deferred.front();
        if (d.buffer && d.buffer.status < MTLCommandBufferStatusCompleted) {
            PS_CHECK(d.buffer.status >= MTLCommandBufferStatusCommitted); // flush_memory_later commits it
            if (!wait) return;
            const auto t0 = std::chrono::steady_clock::now();
            [d.buffer waitUntilCompleted];
            stats.readback_wait_ns += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }
        Deferred done = std::move(d);
        deferred.pop_front();
        for (const Readback& r : done.readbacks) store_readback(r);
        if (done.then) done.then();
    }
}

} // namespace pspocket::rsx::metal
