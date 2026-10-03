// Texture cache: guest textures decoded on the GPU (plan section 7, "Textures"). The guest bytes are copied into a
// staging buffer and a compute kernel (kernels.hpp rsx_decode_texture) byte-swaps, unswizzles (Morton), strips
// borders and expands 16-bit formats into the decoded texture; DXT1/3/5 blocks are copied straight into BC1-3
// textures when the device supports them. The channel remap register becomes a texture view swizzle. Entries are
// invalidated through the WriteTracker.

#include "metal_internal.hpp"

#include <format>

#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx::metal {
namespace {

// Mirrors rsx_decode_params in kernels.hpp.
struct DecodeParams {
    u32 format;
    u32 width;
    u32 height;
    u32 src_offset;
    u32 pitch;
    u32 border;
    u32 morton;
    u32 log2_w;
    u32 log2_h;
    u32 block_bytes;
    u32 pad0;
    u32 pad1;
};

u64 mix(u64 h, u64 v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

u64 texture_key(const TextureInfo& t, u32 ea, bool force_compute)
{
    u64 h = ea;
    h = mix(h, t.format);
    h = mix(h, u64{t.width} | (u64{t.height} << 16) | (u64{t.depth} << 32));
    h = mix(h, t.mipmaps);
    h = mix(h, t.pitch);
    h = mix(h, u64{t.cubemap} | (u64{t.dimension} << 1) | (u64{t.border_type} << 8) | (u64{force_compute} << 9));
    return h;
}

MTLPixelFormat decoded_pixel_format(DecodedFormat f, bool bc)
{
    switch (f) {
    case DecodedFormat::RGBA8: return MTLPixelFormatRGBA8Unorm;
    case DecodedFormat::R32F: return MTLPixelFormatR32Float;
    case DecodedFormat::RG16F: return MTLPixelFormatRG16Float;
    case DecodedFormat::RGBA16F: return MTLPixelFormatRGBA16Float;
    case DecodedFormat::RGBA32F: return MTLPixelFormatRGBA32Float;
    case DecodedFormat::BC1: return bc ? MTLPixelFormatBC1_RGBA : MTLPixelFormatRGBA8Unorm;
    case DecodedFormat::BC2: return bc ? MTLPixelFormatBC2_RGBA : MTLPixelFormatRGBA8Unorm;
    case DecodedFormat::BC3: return bc ? MTLPixelFormatBC3_RGBA : MTLPixelFormatRGBA8Unorm;
    }
    return MTLPixelFormatRGBA8Unorm;
}

MTLTextureSwizzle metal_swizzle(u8 source)
{
    switch (source) {
    case 0: return MTLTextureSwizzleRed;
    case 1: return MTLTextureSwizzleGreen;
    case 2: return MTLTextureSwizzleBlue;
    case 3: return MTLTextureSwizzleAlpha;
    case kChannelZero: return MTLTextureSwizzleZero;
    default: return MTLTextureSwizzleOne;
    }
}

MTLSamplerAddressMode address_mode(u32 wrap)
{
    switch (static_cast<tex::Wrap>(wrap)) {
    case tex::Wrap::Wrap: return MTLSamplerAddressModeRepeat;
    case tex::Wrap::Mirror: return MTLSamplerAddressModeMirrorRepeat;
    case tex::Wrap::Border: return MTLSamplerAddressModeClampToBorderColor;
    case tex::Wrap::MirrorOnceClampToEdge:
    case tex::Wrap::MirrorOnceBorder:
    case tex::Wrap::MirrorOnceClamp: return MTLSamplerAddressModeMirrorClampToEdge;
    case tex::Wrap::ClampToEdge:
    case tex::Wrap::Clamp:
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

} // namespace

id<MTLTexture> MetalBackend::Impl::view_2d(id<MTLTexture> tex, u32 face, u32 level)
{
    if (tex.textureType == MTLTextureType2D && tex.mipmapLevelCount == 1) return tex;
    return [tex newTextureViewWithPixelFormat:tex.pixelFormat
                                  textureType:MTLTextureType2D
                                       levels:NSMakeRange(level, 1)
                                       slices:NSMakeRange(face, 1)];
}

CachedTexture* MetalBackend::Impl::decode_texture(const TextureInfo& t, u32 ea, bool force_compute_dxt, std::string* error)
{
    std::string why;
    auto layout = compute_texture_layout(t, &why);
    if (!layout) {
        if (error) *error = std::format("texture at {:#010x}: {}", ea, why);
        return nullptr;
    }
    const TextureFormatInfo& fmt = *layout->format;
    const u32 size = layout->total_size;
    if (!mem.is_committed(ea) || !mem.is_committed(ea + size - 1)) {
        if (error) *error = std::format("texture at {:#010x} (+{:#x}) is not in committed guest memory", ea, size);
        return nullptr;
    }
    const TextureLevel& base = layout->levels[0];
    const bool compressed = fmt.block_texels == 4;
    const bool use_bc = compressed && bc_supported && !force_compute_dxt && base.width % 4 == 0 && base.height % 4 == 0;

    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = layout->cubemap ? MTLTextureTypeCube : MTLTextureType2D;
    d.pixelFormat = decoded_pixel_format(fmt.decoded, use_bc);
    d.width = base.width;
    d.height = base.height;
    d.mipmapLevelCount = layout->mips;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | (use_bc ? 0 : MTLTextureUsageShaderWrite);
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    if (!texture) {
        if (error) *error = std::format("texture at {:#010x}: allocation failed", ea);
        return nullptr;
    }
    texture.label = [NSString stringWithFormat:@"rsx texture %08x %s %ux%u", ea, fmt.name, base.width, base.height];

    const u64 key = texture_key(t, ea, force_compute_dxt);
    if (auto it = textures.find(key); it != textures.end()) {
        tracker.unwatch(it->second->watch);
        textures.erase(it);
    }
    auto entry = std::make_unique<CachedTexture>();
    entry->address = ea;
    entry->info = t;
    entry->layout = *layout;
    entry->texture = texture;
    entry->last_used = tick;
    // Watch first, copy second: a store racing with the copy either lands in it or dirties the entry.
    entry->watch = tracker.watch(ea, size);
    auto staging = transient.alloc(size, 256);
    std::memcpy(staging.data, mem.host(ea), size);

    if (use_bc) {
        id<MTLBlitCommandEncoder> blit = [upload_buffer() blitCommandEncoder];
        for (const TextureLevel& l : layout->levels) {
            [blit copyFromBuffer:staging.buffer
                       sourceOffset:staging.offset + l.offset
                  sourceBytesPerRow:l.pitch
                sourceBytesPerImage:static_cast<NSUInteger>(l.pitch) * l.rows
                         sourceSize:MTLSizeMake(l.width, l.height, 1)
                          toTexture:texture
                   destinationSlice:l.face
                   destinationLevel:l.level
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding];
    } else {
        id<MTLComputeCommandEncoder> enc = [upload_buffer() computeCommandEncoder];
        [enc setComputePipelineState:decode_pipeline];
        [enc setBuffer:staging.buffer offset:staging.offset atIndex:0];
        for (const TextureLevel& l : layout->levels) {
            DecodeParams p{};
            p.format = static_cast<u32>(fmt.decode);
            p.width = l.width;
            p.height = l.height;
            p.src_offset = l.offset;
            p.pitch = l.pitch;
            p.border = l.border;
            p.morton = layout->swizzled && !compressed;
            p.log2_w = ceil_log2(l.swizzle_width);
            p.log2_h = ceil_log2(l.swizzle_height);
            p.block_bytes = fmt.block_bytes;
            [enc setBytes:&p length:sizeof(p) atIndex:1];
            [enc setTexture:view_2d(texture, l.face, l.level) atIndex:0];
            [enc dispatchThreads:MTLSizeMake(l.width, l.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }
        [enc endEncoding];
    }
    stats.textures_decoded++;
    CachedTexture* raw = entry.get();
    textures[key] = std::move(entry);
    return raw;
}

id<MTLTexture> MetalBackend::Impl::sample_view(id<MTLTexture> tex, std::unordered_map<u32, id<MTLTexture>>& views,
    const TextureFormatInfo& fmt, u32 remap)
{
    const std::array<u8, 4> src = texture_channel_sources(fmt, remap);
    if (src == std::array<u8, 4>{0, 1, 2, 3}) return tex;
    const u32 key = src[0] | (src[1] << 4) | (src[2] << 8) | (src[3] << 12);
    if (auto it = views.find(key); it != views.end()) return it->second;
    const MTLTextureSwizzleChannels sw =
        MTLTextureSwizzleChannelsMake(metal_swizzle(src[0]), metal_swizzle(src[1]), metal_swizzle(src[2]), metal_swizzle(src[3]));
    const NSUInteger slices = tex.textureType == MTLTextureTypeCube ? 6 : 1;
    id<MTLTexture> v = [tex newTextureViewWithPixelFormat:tex.pixelFormat
                                              textureType:tex.textureType
                                                   levels:NSMakeRange(0, tex.mipmapLevelCount)
                                                   slices:NSMakeRange(0, slices)
                                                  swizzle:sw];
    views[key] = v;
    return v;
}

id<MTLTexture> MetalBackend::Impl::bind_texture(const TextureInfo& t, u32 unit, bool& ok)
{
    ok = true;
    const auto ea = rsx->translate(t.offset, t.location);
    if (!ea) {
        unsupported(std::format("texture unit {} at offset {:#x} (location {}) is not mapped", unit, t.offset, t.location));
        ok = false;
        return nil;
    }
    const TextureFormatInfo* fmt = texture_format_info(t.base_format());

    // A render target at this address: sample the GPU copy (plan section 7: "bind the Metal texture directly").
    if (Surface* s = find_surface(*ea, false); s && !s->cpu_newer && fmt) {
        const bool compatible = (fmt->decode == TexelDecode::ARGB8 || fmt->decode == TexelDecode::DRGB8) &&
            surface_color_bpp(s->gcm_format) == 4;
        if (!compatible) {
            unsupported(std::format("texture format {} sampled from a surface of format {} is not converted",
                fmt->name, s->gcm_format));
        }
        stats.surface_samples++;
        id<MTLTexture> tex = s->texture;
        if (draw_targets.contains(s)) {
            // Reading a surface the draw also renders to: sample a copy taken before the draw.
            tex = copy_texture(s->texture);
            std::unordered_map<u32, id<MTLTexture>> tmp;
            return sample_view(tex, tmp, *fmt, t.remap);
        }
        return sample_view(tex, s->views, *fmt, t.remap);
    }

    const u64 key = texture_key(t, *ea, false);
    CachedTexture* entry = nullptr;
    if (auto it = textures.find(key); it != textures.end()) {
        if (tracker.is_dirty(it->second->watch)) {
            tracker.unwatch(it->second->watch);
            textures.erase(it);
        } else {
            entry = it->second.get();
            stats.texture_cache_hits++;
        }
    }
    if (!entry) {
        std::string why;
        entry = decode_texture(t, *ea, false, &why);
        if (!entry) {
            unsupported(why);
            ok = false;
            return nil;
        }
    }
    entry->last_used = tick;
    return sample_view(entry->texture, entry->views, *entry->layout.format, t.remap);
}

id<MTLSamplerState> MetalBackend::Impl::sampler_for(const TextureInfo& t)
{
    // RSX filters: min 1 nearest, 2 linear (no mips), 3-6 nearest/linear x nearest/linear mip, 7 convolution
    // (treated as linear-linear, RPCS3 texture_minify_filter); mag 1 nearest, 2 linear, 4 convolution.
    const u32 minf = t.min_filter, magf = t.mag_filter;
    const u32 aniso = t.max_aniso & 7;
    const u32 lod_min = static_cast<u32>(t.min_lod * 256.f) & 0xfff, lod_max = static_cast<u32>(t.max_lod * 256.f) & 0xfff;
    const u32 border = t.border_color;
    const u64 key = u64{minf} | (u64{magf} << 3) | (u64{t.wrap_s} << 6) | (u64{t.wrap_t} << 10) | (u64{t.wrap_r} << 14) |
        (u64{aniso} << 18) | (u64{lod_min} << 21) | (u64{lod_max} << 33) |
        (u64{border == 0 ? 0u : border == 0xff000000u ? 1u : 2u} << 45);
    if (auto it = samplers.find(key); it != samplers.end()) return it->second;

    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    switch (minf) {
    case 1: d.minFilter = MTLSamplerMinMagFilterNearest; d.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
    case 2: d.minFilter = MTLSamplerMinMagFilterLinear; d.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
    case 3: d.minFilter = MTLSamplerMinMagFilterNearest; d.mipFilter = MTLSamplerMipFilterNearest; break;
    case 4: d.minFilter = MTLSamplerMinMagFilterLinear; d.mipFilter = MTLSamplerMipFilterNearest; break;
    case 5: d.minFilter = MTLSamplerMinMagFilterNearest; d.mipFilter = MTLSamplerMipFilterLinear; break;
    default: d.minFilter = MTLSamplerMinMagFilterLinear; d.mipFilter = MTLSamplerMipFilterLinear; break;
    }
    d.magFilter = magf == 1 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.sAddressMode = address_mode(t.wrap_s);
    d.tAddressMode = address_mode(t.wrap_t);
    d.rAddressMode = address_mode(t.wrap_r);
    // Metal has three border colours; pick the closest (the RSX border is an arbitrary ARGB colour).
    d.borderColor = border == 0 ? MTLSamplerBorderColorTransparentBlack
        : (border >> 24) == 0xff && (border & 0xffffff) == 0 ? MTLSamplerBorderColorOpaqueBlack
                                                              : MTLSamplerBorderColorOpaqueWhite;
    static constexpr NSUInteger kAniso[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    d.maxAnisotropy = kAniso[aniso];
    d.lodMinClamp = t.min_lod;
    d.lodMaxClamp = t.max_lod > t.min_lod ? t.max_lod : FLT_MAX;
    id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:d];
    samplers[key] = s;
    return s;
}

bool MetalBackend::Impl::read_texture(id<MTLTexture> tex, u32 face, u32 level, u32 w, u32 h,
    std::vector<std::array<float, 4>>& out)
{
    id<MTLTexture> view = view_2d(tex, face, level);
    id<MTLBuffer> buf = [device newBufferWithLength:static_cast<NSUInteger>(w) * h * 16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:read_pipeline];
    [enc setTexture:view atIndex:0];
    [enc setSamplerState:nearest_sampler atIndex:0];
    [enc setBuffer:buf offset:0 atIndex:0];
    const u32 size[4] = {w, h, 0, 0};
    [enc setBytes:size length:sizeof(size) atIndex:1];
    [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return false;
    out.resize(static_cast<usize>(w) * h);
    std::memcpy(out.data(), buf.contents, out.size() * 16);
    return true;
}

} // namespace pspocket::rsx::metal
