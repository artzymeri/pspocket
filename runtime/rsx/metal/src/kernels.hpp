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
    uint pad0;
    uint pad1;
};

// TexelDecode values (runtime/rsx/include/pspocket/rsx/texture.hpp)
constant uint kARGB8 = 1, kDRGB8 = 2, kR5G6B5 = 3, kA1R5G5B5 = 4, kD1R5G5B5 = 5, kA4R4G4B4 = 6, kR5G5B5A1 = 7,
              kR6G5B5 = 8, kB8 = 9, kX32F = 10, kW16F = 11, kW32F = 12, kY16X16F = 13, kX16 = 14, kY16X16 = 15,
              kDXT1 = 16, kDXT23 = 17, kDXT45 = 18, kD16 = 19;

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
        switch (p.format) {
        case kARGB8: c = rgba8(src[o + 1], src[o + 2], src[o + 3], src[o]); break;
        case kDRGB8: c = rgba8(src[o + 1], src[o + 2], src[o + 3], 255u); break;
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
        case kY16X16: c = float4(float(ld16be(src, o)) / 65535.0, float(ld16be(src, o + 2)) / 65535.0, 0.0, 1.0); break;
        default: break;
        }
    }
    dst.write(c, gid);
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
