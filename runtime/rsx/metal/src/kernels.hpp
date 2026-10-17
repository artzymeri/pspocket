#pragma once

// MSL for the backend's own work: texture decode (compute), texture readback, the present blit and clear quads.
// Compiled at runtime with newLibraryWithSource (allowed on iOS; plan section 2). The decode formulas match the CPU
// reference decoders in runtime/rsx/src/texture.cpp exactly (the tests compare them).

namespace pspocket::rsx::metal {

// Mirrors DecodeParams in texture_cache.mm.
inline constexpr const char* kKernelSource = R"MSL(#include <metal_stdlib>
using namespace metal;

struct rsx_decode_params {
    uint format;      // TexelDecode
    uint width;       // level size in texels
    uint height;
    uint src_offset;  // byte offset of the level's image (incl. border) in the source buffer
    uint pitch;       // bytes per row of blocks (linear layouts)
    uint border;      // border texels on each side
    uint morton;      // 1 = Morton (swizzled) order
    uint log2_w;      // Morton area, in elements
    uint log2_h;
    uint block_bytes;
    uint depth;       // 3D textures: slices of the level
    uint log2_d;      // 3D textures: Morton depth, in slices
    uint slice_bytes; // 3D textures, linear: bytes per slice
    uint pad;
};

// TexelDecode values (runtime/rsx/include/pspocket/rsx/texture.hpp)
constant uint kARGB8 = 1, kDRGB8 = 2, kR5G6B5 = 3, kA1R5G5B5 = 4, kD1R5G5B5 = 5, kA4R4G4B4 = 6, kR5G5B5A1 = 7,
              kR6G5B5 = 8, kB8 = 9, kX32F = 10, kW16F = 11, kW32F = 12, kY16X16F = 13, kX16 = 14, kY16X16 = 15,
              kDXT1 = 16, kDXT23 = 17, kDXT45 = 18, kD16 = 19, kG8B8 = 20, kD24 = 21, kD24F = 22, kHILO8 = 23,
              kHILOS8 = 24;

static uint morton(uint x, uint y, uint lw, uint lh)
{
    uint offset = 0, shift = 0;
    while ((x | y) != 0u) {
        if (lw != 0u) { offset |= (x & 1u) << shift++; x >>= 1; lw--; }
        if (lh != 0u) { offset |= (y & 1u) << shift++; y >>= 1; lh--; }
        if (lw == 0u && lh == 0u) break;
    }
    return offset;
}

static uint morton3(uint x, uint y, uint z, uint lw, uint lh, uint ld)
{
    uint offset = 0, shift = 0;
    while ((x | y | z) != 0u) {
        if (lw != 0u) { offset |= (x & 1u) << shift++; x >>= 1; lw--; }
        if (lh != 0u) { offset |= (y & 1u) << shift++; y >>= 1; lh--; }
        if (ld != 0u) { offset |= (z & 1u) << shift++; z >>= 1; ld--; }
        if (lw == 0u && lh == 0u && ld == 0u) break;
    }
    return offset;
}

static uint ld16be(device const uchar* b, uint o) { return (uint(b[o]) << 8) | uint(b[o + 1]); }
static uint ld32be(device const uchar* b, uint o)
{
    return (uint(b[o]) << 24) | (uint(b[o + 1]) << 16) | (uint(b[o + 2]) << 8) | uint(b[o + 3]);
}
static float unorm8(uint v) { return float(v) / 255.0; }
static uint expand5(uint v) { return (v * 527u + 23u) >> 6; }
static uint expand6(uint v) { return (v * 259u + 33u) >> 6; }
static float4 rgba8(uint r, uint g, uint b, uint a) { return float4(unorm8(r), unorm8(g), unorm8(b), unorm8(a)); }

static uint4 bc_color(device const uchar* blk, uint texel, bool four_color)
{
    const uint c0 = uint(blk[0]) | (uint(blk[1]) << 8);
    const uint c1 = uint(blk[2]) | (uint(blk[3]) << 8);
    const uint3 p0 = uint3(((c0 >> 11) & 31u) << 3 | ((c0 >> 11) & 31u) >> 2,
                           ((c0 >> 5) & 63u) << 2 | ((c0 >> 5) & 63u) >> 4,
                           (c0 & 31u) << 3 | (c0 & 31u) >> 2);
    const uint3 p1 = uint3(((c1 >> 11) & 31u) << 3 | ((c1 >> 11) & 31u) >> 2,
                           ((c1 >> 5) & 63u) << 2 | ((c1 >> 5) & 63u) >> 4,
                           (c1 & 31u) << 3 | (c1 & 31u) >> 2);
    const uint idx = (uint(blk[4u + texel / 4u]) >> (2u * (texel % 4u))) & 3u;
    if (idx == 0u) return uint4(p0, 255u);
    if (idx == 1u) return uint4(p1, 255u);
    if (four_color || c0 > c1) {
        return idx == 2u ? uint4((2u * p0 + p1) / 3u, 255u) : uint4((p0 + 2u * p1) / 3u, 255u);
    }
    return idx == 2u ? uint4((p0 + p1) / 2u, 255u) : uint4(0u);
}

static float4 decode_dxt(device const uchar* src, constant rsx_decode_params& p, uint2 t)
{
    const uint block = p.src_offset + (t.y / 4u) * p.pitch + (t.x / 4u) * p.block_bytes;
    const uint texel = (t.y % 4u) * 4u + (t.x % 4u);
    if (p.format == kDXT1) {
        const uint4 c = bc_color(src + block, texel, false);
        return rgba8(c.r, c.g, c.b, c.a);
    }
    uint4 c = bc_color(src + block + 8u, texel, true);
    if (p.format == kDXT23) {
        c.a = ((uint(src[block + texel / 2u]) >> (4u * (texel & 1u))) & 15u) * 17u;
    } else {
        const uint a0 = src[block], a1 = src[block + 1u];
        const uint bit = 3u * texel;
        const uint idx = ((uint(src[block + 2u + bit / 8u]) | (uint(src[block + 3u + bit / 8u]) << 8)) >> (bit % 8u)) & 7u;
        uint a;
        if (idx == 0u) a = a0;
        else if (idx == 1u) a = a1;
        else if (a0 > a1) a = ((8u - idx) * a0 + (idx - 1u) * a1) / 7u;
        else if (idx < 6u) a = ((6u - idx) * a0 + (idx - 1u) * a1) / 5u;
        else a = idx == 6u ? 0u : 255u;
        c.a = a;
    }
    return rgba8(c.r, c.g, c.b, c.a);
}

// One texel of an uncompressed format at byte o of src (TexelDecode p.format).
static float4 decode_texel(device const uchar* src, uint o, constant rsx_decode_params& p)
{
    float4 c = float4(0.0);
    switch (p.format) {
    case kARGB8: c = rgba8(src[o + 1], src[o + 2], src[o + 3], src[o]); break;
    case kDRGB8: c = rgba8(src[o + 1], src[o + 2], src[o + 3], 255u); break;
    case kG8B8: c = rgba8(src[o + 1], src[o], 0u, 255u); break;
    case kR5G6B5: { const uint v = ld16be(src, o); c = rgba8(expand5(v >> 11), expand6((v >> 5) & 63u), expand5(v & 31u), 255u); break; }
    case kA1R5G5B5: { const uint v = ld16be(src, o); c = rgba8(expand5((v >> 10) & 31u), expand5((v >> 5) & 31u), expand5(v & 31u), (v >> 15) != 0u ? 255u : 0u); break; }
    case kD1R5G5B5: { const uint v = ld16be(src, o); c = rgba8(expand5((v >> 10) & 31u), expand5((v >> 5) & 31u), expand5(v & 31u), 255u); break; }
    case kA4R4G4B4: { const uint v = ld16be(src, o); c = rgba8(((v >> 8) & 15u) * 17u, ((v >> 4) & 15u) * 17u, (v & 15u) * 17u, (v >> 12) * 17u); break; }
    case kR5G5B5A1: { const uint v = ld16be(src, o); c = rgba8(expand5(v >> 11), expand5((v >> 6) & 31u), expand5((v >> 1) & 31u), (v & 1u) != 0u ? 255u : 0u); break; }
    case kR6G5B5: { const uint v = ld16be(src, o); c = rgba8(expand6(v >> 10), expand5((v >> 5) & 31u), expand5(v & 31u), 255u); break; }
    case kB8: c = rgba8(src[o], src[o], src[o], 255u); break;
    case kX32F: c = float4(as_type<float>(ld32be(src, o)), 0.0, 0.0, 1.0); break;
    case kW16F: c = float4(float(as_type<half>(ushort(ld16be(src, o)))), float(as_type<half>(ushort(ld16be(src, o + 2)))),
                           float(as_type<half>(ushort(ld16be(src, o + 4)))), float(as_type<half>(ushort(ld16be(src, o + 6))))); break;
    case kW32F: c = float4(as_type<float>(ld32be(src, o)), as_type<float>(ld32be(src, o + 4)),
                           as_type<float>(ld32be(src, o + 8)), as_type<float>(ld32be(src, o + 12))); break;
    case kY16X16F: c = float4(float(as_type<half>(ushort(ld16be(src, o)))), float(as_type<half>(ushort(ld16be(src, o + 2)))), 0.0, 1.0); break;
    case kX16:
    case kD16: c = float4(float(ld16be(src, o)) / 65535.0, 0.0, 0.0, 1.0); break;
    case kD24: c = float4(float(ld32be(src, o) >> 8u) / 16777215.0, 0.0, 0.0, 1.0); break;
    case kD24F: c = float4(as_type<float>((ld32be(src, o) >> 8u) << 7u), 0.0, 0.0, 1.0); break;
    case kHILO8: c = float4(float(src[o + 1]) / 255.0, float(src[o]) / 255.0, 0.0, 1.0); break;
    case kHILOS8: c = float4(max(float(as_type<char>(src[o + 1])) / 127.0, -1.0),
                             max(float(as_type<char>(src[o])) / 127.0, -1.0), 0.0, 1.0); break;
    case kY16X16: c = float4(float(ld16be(src, o)) / 65535.0, float(ld16be(src, o + 2)) / 65535.0, 0.0, 1.0); break;
    default: break;
    }
    return c;
}

kernel void rsx_decode_texture(device const uchar* src [[buffer(0)]], constant rsx_decode_params& p [[buffer(1)]],
                               texture2d<float, access::write> dst [[texture(0)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    float4 c = float4(0.0);
    if (p.format >= kDXT1 && p.format <= kDXT45) {
        c = decode_dxt(src, p, gid);
    } else {
        const uint ex = gid.x + p.border, ey = gid.y + p.border;
        const uint o = p.src_offset + (p.morton != 0u ? morton(ex, ey, p.log2_w, p.log2_h) * p.block_bytes
                                                       : ey * p.pitch + ex * p.block_bytes);
        c = decode_texel(src, o, p);
    }
    dst.write(c, gid);
}

// A level of a 3D texture (uncompressed formats): its slices one after another, or swizzled in the Morton order with x,
// y and z interleaved (RPCS3 convert_linear_swizzle_3d).
kernel void rsx_decode_texture3d(device const uchar* src [[buffer(0)]], constant rsx_decode_params& p [[buffer(1)]],
                                 texture3d<float, access::write> dst [[texture(0)]], uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height || gid.z >= p.depth) return;
    const uint ex = gid.x + p.border, ey = gid.y + p.border;
    const uint o = p.src_offset + (p.morton != 0u ? morton3(ex, ey, gid.z, p.log2_w, p.log2_h, p.log2_d) * p.block_bytes
                                                   : gid.z * p.slice_bytes + ey * p.pitch + ex * p.block_bytes);
    dst.write(decode_texel(src, o, p), gid);
}

// A depth buffer read as a texture, on the GPU (D-088): the texture sees the depth buffer's bits as the RSX lays them
// out, Z24S8 as big-endian words (depth in bits 31-8, stencil in 7-0) and Z16 as big-endian halfwords, and decodes them
// as its own format (p.x: kARGB8, kDRGB8, kX16, kY16X16 as rsx_decode_texture does; kDepthValue: the depth itself, for
// the depth texture formats). p.y: the buffer is Z16.
constant uint kDepthValue = 100;
kernel void rsx_depth_to_texture(depth2d<float, access::read> depth [[texture(0)]],
                                 texture2d<uint, access::read> stencil [[texture(1)]],
                                 texture2d<float, access::write> dst [[texture(2)]],
                                 constant uint4& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    const float z = depth.read(gid);
    const bool z16 = p.y != 0u;
    const uint d = z16 ? uint(rint(saturate(z) * 65535.0)) : uint(rint(saturate(z) * 16777215.0));
    const uint word = z16 ? (d << 16) : ((d << 8) | (stencil.read(gid).r & 0xffu));
    const uint b0 = word >> 24, b1 = (word >> 16) & 0xffu, b2 = (word >> 8) & 0xffu, b3 = word & 0xffu;
    float4 c;
    switch (p.x) {
    case kARGB8: c = rgba8(b1, b2, b3, b0); break;
    case kDRGB8: c = rgba8(b1, b2, b3, 255u); break;
    case kX16: c = float4(float(word >> 16) / 65535.0, 0.0, 0.0, 1.0); break;
    case kY16X16: c = float4(float(word >> 16) / 65535.0, float(word & 0xffffu) / 65535.0, 0.0, 1.0); break;
    default: c = float4(z); break;
    }
    dst.write(c, gid);
}

// A colour target read as a texture of another format whose texels are its pixels: the texture sees the target's bytes
// as the RSX lays them out (A8R8G8B8 as a r g b, A8B8G8R8 as a b g r in memory) and decodes them as its own format
// (p.x: kY16X16, two big-endian halfwords). p.y: the target is A8B8G8R8 (else A8R8G8B8); p.z: its alpha byte is the
// pixel's (0), zero (1) or one (2), for the X8 formats.
kernel void rsx_color_to_texture(texture2d<float, access::read> src [[texture(0)]],
                                 texture2d<float, access::write> dst [[texture(1)]],
                                 constant uint4& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    const float4 c = src.read(gid);
    const uint r = uint(rint(saturate(c.r) * 255.0)), g = uint(rint(saturate(c.g) * 255.0)),
               b = uint(rint(saturate(c.b) * 255.0));
    const uint a = p.z == 1u ? 0u : p.z == 2u ? 255u : uint(rint(saturate(c.a) * 255.0));
    const uint word = p.y != 0u ? (a << 24) | (b << 16) | (g << 8) | r : (a << 24) | (r << 16) | (g << 8) | b;
    float4 o = float4(0.0, 0.0, 0.0, 1.0);
    if (p.x == kY16X16) o = float4(float(word >> 16) / 65535.0, float(word & 0xffffu) / 65535.0, 0.0, 1.0);
    dst.write(o, gid);
}

// A render target's pixels as the RSX lays them out in memory (store_readback's bytes, surface_cache.mm), written into
// a buffer that holds a copy of the guest memory under a texture of another format, which then decodes it (D-137): the
// target's newer pixels reach the texture on the GPU, without a write-back the RSX thread waits for. Mirrors
// OverlayParams in texture_cache.mm.
struct rsx_overlay_params {
    uint mode;     // kOv*
    uint width;    // the target's size in pixels
    uint height;
    uint pitch;    // its bytes per row in memory (linear targets)
    int delta;     // its address minus the buffer's
    uint size;     // the buffer's bytes
    uint swizzled; // Morton order (power-of-two colour targets, D-111)
    uint log2_w;
    uint log2_h;
    uint bpp;      // its bytes per pixel in memory
    uint alpha;    // the X formats' top bits: the pixel's alpha (0), zero (1) or one (2)
    uint pad;
};
constant uint kOvARGB8 = 0, kOvABGR8 = 1, kOvB8 = 2, kOvG8B8 = 3, kOvR5G6B5 = 4, kOvX1R5G5B5 = 5, kOvF16x4 = 6,
              kOvF32x4 = 7, kOvF32 = 8, kOvZ24S8 = 9, kOvZ16 = 10;

static void overlay_store(device uchar* dst, constant rsx_overlay_params& p, uint2 gid, thread const uchar* b)
{
    const uint element = p.swizzled != 0u ? morton(gid.x, gid.y, p.log2_w, p.log2_h) * p.bpp : gid.y * p.pitch + gid.x * p.bpp;
    const int at = p.delta + int(element);
    for (uint i = 0; i < p.bpp; i++) {
        const int o = at + int(i);
        if (o >= 0 && uint(o) < p.size) dst[o] = b[i];
    }
}

static uint quantize(uint v, uint max) { return (v * max + 127u) / 255u; }

// The 8-bit formats, from the target's unorm channels (exact: n / 255 rounds back to n).
kernel void rsx_color_to_memory(texture2d<float, access::read> src [[texture(0)]], device uchar* dst [[buffer(0)]],
                                constant rsx_overlay_params& p [[buffer(1)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    const float4 c = src.read(gid);
    const uint r = uint(rint(saturate(c.r) * 255.0)), g = uint(rint(saturate(c.g) * 255.0)),
               bl = uint(rint(saturate(c.b) * 255.0));
    const uint a = p.alpha == 1u ? 0u : p.alpha == 2u ? 255u : uint(rint(saturate(c.a) * 255.0));
    uchar b[4] = {0, 0, 0, 0};
    switch (p.mode) {
    case kOvARGB8: b[0] = uchar(a); b[1] = uchar(r); b[2] = uchar(g); b[3] = uchar(bl); break;
    case kOvABGR8: b[0] = uchar(a); b[1] = uchar(bl); b[2] = uchar(g); b[3] = uchar(r); break;
    case kOvB8: b[0] = uchar(r); break;                   // the R8 copy holds the target's byte in red
    case kOvG8B8: b[0] = uchar(g); b[1] = uchar(r); break; // the RG8 copy: blue in red, green in green (D-112)
    case kOvR5G6B5: {
        const uint v = (quantize(r, 31u) << 11) | (quantize(g, 63u) << 5) | quantize(bl, 31u);
        b[0] = uchar(v >> 8); b[1] = uchar(v);
        break;
    }
    case kOvX1R5G5B5: {
        const uint v = (p.alpha == 2u ? 0x8000u : 0u) | (quantize(r, 31u) << 10) | (quantize(g, 31u) << 5) | quantize(bl, 31u);
        b[0] = uchar(v >> 8); b[1] = uchar(v);
        break;
    }
    default: break;
    }
    overlay_store(dst, p, gid, b);
}

// The float formats, from an integer view of the target (their bits as they are): each component big-endian, red first.
kernel void rsx_float_to_memory(texture2d<uint, access::read> src [[texture(0)]], device uchar* dst [[buffer(0)]],
                                constant rsx_overlay_params& p [[buffer(1)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    const uint4 v = src.read(gid);
    uchar b[16];
    if (p.mode == kOvF16x4) {
        for (uint i = 0; i < 4u; i++) { b[2u * i] = uchar(v[i] >> 8); b[2u * i + 1u] = uchar(v[i]); }
    } else {
        const uint n = p.mode == kOvF32x4 ? 4u : 1u;
        for (uint i = 0; i < n; i++) {
            b[4u * i] = uchar(v[i] >> 24); b[4u * i + 1u] = uchar(v[i] >> 16);
            b[4u * i + 2u] = uchar(v[i] >> 8); b[4u * i + 3u] = uchar(v[i]);
        }
    }
    overlay_store(dst, p, gid, b);
}

// A depth buffer: Z24S8 as big-endian words (depth in bits 31-8, stencil in 7-0), Z16 as big-endian halfwords.
kernel void rsx_depth_to_memory(depth2d<float, access::read> depth [[texture(0)]],
                                texture2d<uint, access::read> stencil [[texture(1)]], device uchar* dst [[buffer(0)]],
                                constant rsx_overlay_params& p [[buffer(1)]], uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    const float z = depth.read(gid);
    uchar b[4];
    if (p.mode == kOvZ16) {
        const uint d = uint(rint(saturate(z) * 65535.0));
        b[0] = uchar(d >> 8); b[1] = uchar(d);
    } else {
        const uint w = (uint(rint(saturate(z) * 16777215.0)) << 8) | (stencil.read(gid).r & 0xffu);
        b[0] = uchar(w >> 24); b[1] = uchar(w >> 16); b[2] = uchar(w >> 8); b[3] = uchar(w);
    }
    overlay_store(dst, p, gid, b);
}

// 5 and 6 bits to 8 as the CPU's upload_surface_bytes widens them (the BGRA8 copy R5G6B5 targets render to, D-112).
static uchar bits5(uint v) { return uchar((v << 3) | (v >> 2)); }
static uchar bits6(uint v) { return uchar((v << 2) | (v >> 4)); }

// The inverse (D-1025): guest bytes in a target's own format (a staging copy of its memory, other targets laid over it
// with the kernels above) to the texel bytes of its Metal texture, which a blit then copies in: what a target made over
// the memory of others of other formats starts with on the GPU (GTA V packs its depth into an A8B8G8R8 target, then
// tests against that memory as a Z24S8 buffer). Mirrors upload_surface_bytes (surface_cache.mm). Z24S8: the depth as
// a float into `texels`, the stencil into `stencil`.
kernel void rsx_memory_to_surface(device const uchar* src [[buffer(0)]], device uchar* texels [[buffer(1)]],
                                  device uchar* stencil [[buffer(2)]], constant rsx_overlay_params& p [[buffer(3)]],
                                  uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    const uint o = p.swizzled != 0u ? morton(gid.x, gid.y, p.log2_w, p.log2_h) * p.bpp : gid.y * p.pitch + gid.x * p.bpp;
    const uint i = gid.y * p.width + gid.x;
    switch (p.mode) {
    case kOvZ24S8: {
        const uint v = ld32be(src, o);
        const float z = precise::divide(float(v >> 8), 16777215.0f); // as the CPU's upload: the write-back's rint gets d back
        const uint bits = as_type<uint>(z);
        texels[i * 4u] = uchar(bits); texels[i * 4u + 1u] = uchar(bits >> 8);
        texels[i * 4u + 2u] = uchar(bits >> 16); texels[i * 4u + 3u] = uchar(bits >> 24);
        stencil[i] = uchar(v);
        break;
    }
    case kOvZ16: texels[i * 2u] = src[o + 1u]; texels[i * 2u + 1u] = src[o]; break;
    case kOvB8: texels[i] = src[o]; break;
    case kOvG8B8: texels[i * 2u] = src[o + 1u]; texels[i * 2u + 1u] = src[o]; break; // G B -> R(blue) G
    case kOvR5G6B5:
    case kOvX1R5G5B5: {
        const uint v = ld16be(src, o);
        device uchar* t = texels + i * 4u;
        if (p.mode == kOvR5G6B5) {
            t[0] = bits5(v & 31u); t[1] = bits6((v >> 5) & 63u); t[2] = bits5(v >> 11);
        } else {
            t[0] = bits5(v & 31u); t[1] = bits5((v >> 5) & 31u); t[2] = bits5((v >> 10) & 31u);
        }
        t[3] = p.alpha == 1u ? 0u : 255u;
        break;
    }
    case kOvF16x4:
        for (uint k = 0; k < 8u; k += 2u) { texels[i * 8u + k] = src[o + k + 1u]; texels[i * 8u + k + 1u] = src[o + k]; }
        break;
    case kOvF32x4:
    case kOvF32: {
        const uint n = p.mode == kOvF32x4 ? 16u : 4u;
        for (uint k = 0; k < n; k += 4u)
            for (uint b = 0; b < 4u; b++) texels[i * n + k + b] = src[o + k + 3u - b];
        break;
    }
    default: { // kOvARGB8, kOvABGR8: the bytes reverse; the X8 forms' X reads as zero or one
        device uchar* t = texels + i * 4u;
        t[0] = src[o + 3u]; t[1] = src[o + 2u]; t[2] = src[o + 1u];
        t[3] = p.alpha == 1u ? 0u : p.alpha == 2u ? 255u : src[o];
        break;
    }
    }
}

// The presented image as a 64x36 thumbnail (D-1027): each texel the mean of its box of the image, BGRA8, alpha 255,
// into a shared buffer the host reads once the frame has run (pspocket-boot's compare_frame computes the same on the CPU).
kernel void rsx_thumbnail(texture2d<float, access::read> src [[texture(0)]], device uchar* out [[buffer(0)]],
                          constant uint4& size [[buffer(1)]], uint2 gid [[thread_position_in_grid]])
{
    const uint tw = size.x, th = size.y, w = src.get_width(), h = src.get_height();
    if (gid.x >= tw || gid.y >= th) return;
    const uint y0 = gid.y * h / th, y1 = max(y0 + 1u, (gid.y + 1u) * h / th);
    const uint x0 = gid.x * w / tw, x1 = max(x0 + 1u, (gid.x + 1u) * w / tw);
    uint3 sum = uint3(0);
    for (uint y = y0; y < y1; y++)
        for (uint x = x0; x < x1; x++) {
            const float4 c = src.read(uint2(x, y));
            sum += uint3(uint(rint(saturate(c.b) * 255.0)), uint(rint(saturate(c.g) * 255.0)), uint(rint(saturate(c.r) * 255.0)));
        }
    const uint n = (y1 - y0) * (x1 - x0);
    device uchar* o = out + (gid.y * tw + gid.x) * 4u;
    o[0] = uchar(sum.x / n); o[1] = uchar(sum.y / n); o[2] = uchar(sum.z / n); o[3] = 255u;
}

// Conditional rendering on the GPU (D-1058): a ZPASS report's counters (copied together) sum to zero, so the draws under
// it run no instance: each one's arguments in `args` (32 bytes apart after a 16-byte header whose first word counts
// them; the instance count is the second word of both argument layouts) get an instance count of 0.
kernel void rsx_predicate(device const ulong* counts [[buffer(0)]], device uint* args [[buffer(1)]],
                          constant uint& n [[buffer(2)]], uint tid [[thread_position_in_grid]])
{
    if (tid != 0) return;
    ulong sum = 0;
    for (uint i = 0; i < n; i++) sum += counts[i];
    if (sum != 0) return;
    const uint draws = args[0];
    for (uint i = 0; i < draws; i++) args[4u + i * 8u + 1u] = 0u;
}

// Reads a texture back as float4 per texel (nearest sampling at texel centres: works for every format, BCn included).
kernel void rsx_read_texture(texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],
                             device float4* out [[buffer(0)]], constant uint4& size [[buffer(1)]],
                             uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= size.x || gid.y >= size.y) return;
    const float2 uv = (float2(gid) + 0.5) / float2(size.xy);
    out[gid.y * size.x + gid.x] = src.sample(s, uv);
}

struct rsx_blit_out {
    float4 position [[position]];
    float2 uv;
};

// One triangle covering the viewport.
vertex rsx_blit_out rsx_fullscreen_vs(uint vid [[vertex_id]], constant float& depth [[buffer(0)]])
{
    const float2 p = float2((vid << 1) & 2, vid & 2);
    rsx_blit_out out;
    out.position = float4(p * 2.0 - 1.0, depth, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

// x: force alpha to one (X8R8G8B8 display buffers)
fragment float4 rsx_blit_fs(rsx_blit_out in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],
                            constant float4& params [[buffer(0)]])
{
    float4 c = src.sample(s, in.uv);
    if (params.x > 0.5) c.a = 1.0;
    return c;
}

struct rsx_clear1 { float4 c0 [[color(0)]]; };
struct rsx_clear2 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };
struct rsx_clear3 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };
struct rsx_clear4 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };
fragment rsx_clear1 rsx_clear_fs1(constant float4& c [[buffer(0)]]) { return {c}; }
fragment rsx_clear2 rsx_clear_fs2(constant float4& c [[buffer(0)]]) { return {c, c}; }
fragment rsx_clear3 rsx_clear_fs3(constant float4& c [[buffer(0)]]) { return {c, c, c}; }
fragment rsx_clear4 rsx_clear_fs4(constant float4& c [[buffer(0)]]) { return {c, c, c, c}; }
fragment void rsx_clear_fs0() {}
)MSL";

} // namespace pspocket::rsx::metal
