// Texture formats, guest layouts (linear, swizzled, mips, cube faces, borders), the Morton index, the CPU reference
// decoders (16/32-bit formats, DXT1/3/5) and remap composition. Texture data is synthetic.

#include <catch_amalgamated.hpp>

#include "pspocket/core/be.h"
#include "pspocket/rsx/texture.hpp"

using namespace pspocket;
using namespace pspocket::rsx;

namespace {

TextureInfo make_texture(u32 format, u16 w, u16 h, u32 mips = 1, u32 pitch = 0, bool cube = false)
{
    TextureInfo t;
    t.format = format;
    t.width = w;
    t.height = h;
    t.mipmaps = mips;
    t.pitch = pitch;
    t.dimension = 2;
    t.cubemap = cube;
    t.border_type = true; // border colour: no border texels
    return t;
}

} // namespace

TEST_CASE("Morton index interleaves x and y and continues with the longer side")
{
    // 4x4: x bits at 0 and 2, y bits at 1 and 3
    CHECK(morton_index(0, 0, 2, 2) == 0);
    CHECK(morton_index(1, 0, 2, 2) == 1);
    CHECK(morton_index(0, 1, 2, 2) == 2);
    CHECK(morton_index(1, 1, 2, 2) == 3);
    CHECK(morton_index(2, 0, 2, 2) == 4);
    CHECK(morton_index(3, 3, 2, 2) == 15);
    // 8x2: one interleaved pair, then x's remaining bits
    CHECK(morton_index(1, 1, 3, 1) == 3);
    CHECK(morton_index(2, 0, 3, 1) == 4);
    CHECK(morton_index(7, 1, 3, 1) == 15);
    // 2x8: y continues
    CHECK(morton_index(0, 2, 1, 3) == 4);
    CHECK(morton_index(1, 7, 1, 3) == 15);
    // Every index of a 16x8 area is hit exactly once.
    std::vector<int> seen(128, 0);
    for (u32 y = 0; y < 8; y++) {
        for (u32 x = 0; x < 16; x++) seen[morton_index(x, y, 4, 3)]++;
    }
    for (int s : seen) CHECK(s == 1);
}

TEST_CASE("texture layouts: linear mips share the pitch, swizzled mips are packed, cube faces align to 128")
{
    std::string why;
    // Linear ARGB8 64x32 with pitch 512 (wider than the 256-byte row), 3 mips
    const auto lin = compute_texture_layout(make_texture(tex::A8R8G8B8 | tex::kLinearFlag, 64, 32, 3, 512), &why);
    REQUIRE(lin.has_value());
    REQUIRE(lin->levels.size() == 3);
    CHECK_FALSE(lin->swizzled);
    CHECK(lin->levels[0].pitch == 512);
    CHECK(lin->levels[1].pitch == 512);
    CHECK(lin->levels[1].offset == 512 * 32);
    CHECK(lin->levels[2].offset == 512 * 32 + 512 * 16);
    CHECK(lin->levels[2].width == 16);
    CHECK(lin->total_size == 512 * (32 + 16 + 8));

    // Swizzled R5G6B5 16x16, full chain (5 levels)
    const auto swz = compute_texture_layout(make_texture(tex::R5G6B5, 16, 16, 10), &why);
    REQUIRE(swz.has_value());
    REQUIRE(swz->mips == 5); // capped at log2(16) + 1
    CHECK(swz->swizzled);
    CHECK(swz->levels[0].pitch == 32);
    CHECK(swz->levels[1].offset == 512);
    CHECK(swz->levels[4].width == 1);
    CHECK(swz->levels[4].offset == 512 + 128 + 32 + 8);

    // Swizzled cube map ARGB8 8x8 with 2 mips: each face is 256 + 64 bytes, rounded up to 384
    const auto cube = compute_texture_layout(make_texture(tex::A8R8G8B8, 8, 8, 2, 0, true), &why);
    REQUIRE(cube.has_value());
    CHECK(cube->faces == 6);
    REQUIRE(cube->levels.size() == 12);
    CHECK(cube->levels[2].face == 1);
    CHECK(cube->levels[2].offset == 384);
    CHECK(cube->total_size == 6 * 384);

    // DXT5 32x16: 4x4 blocks of 16 bytes; mips stop where a side reaches one block
    const auto dxt = compute_texture_layout(make_texture(tex::COMPRESSED_DXT45 | tex::kLinearFlag, 32, 16, 8, 128), &why);
    REQUIRE(dxt.has_value());
    CHECK(dxt->mips == 3);
    CHECK(dxt->levels[0].width_blocks == 8);
    CHECK(dxt->levels[0].height_blocks == 4);
    CHECK(dxt->levels[0].size == 128 * 4);
    CHECK(dxt->levels[1].offset == 512);

    // Linear texture with border texels: one texel all around
    TextureInfo bordered = make_texture(tex::A8R8G8B8 | tex::kLinearFlag, 4, 4, 1, 32);
    bordered.border_type = false;
    const auto b = compute_texture_layout(bordered, &why);
    REQUIRE(b.has_value());
    CHECK(b->levels[0].border == 1);
    CHECK(b->levels[0].rows == 6);

    // Failures
    CHECK_FALSE(compute_texture_layout(make_texture(0x7f, 4, 4), &why).has_value());
    CHECK(why.find("unknown") != std::string::npos);
    CHECK_FALSE(compute_texture_layout(make_texture(tex::DEPTH16_FLOAT, 4, 4), &why).has_value());
    CHECK(why.find("DEPTH16_FLOAT") != std::string::npos);
    TextureInfo volume = make_texture(tex::COMPRESSED_DXT1, 4, 4);
    volume.dimension = 3;
    volume.depth = 4;
    CHECK_FALSE(compute_texture_layout(volume, &why).has_value()); // compressed 3D textures (VTC tiling)
    CHECK(why.find("compressed 3D") != std::string::npos);
}

TEST_CASE("3D textures: slices after one another, or swizzled with x, y and z interleaved")
{
    // GTA V's first mission samples a 64x64x32 R5G6B5 swizzled volume.
    TextureInfo t = make_texture(tex::R5G6B5, 8, 4);
    t.dimension = 3;
    t.depth = 2;
    const auto swizzled = compute_texture_layout(t);
    REQUIRE(swizzled.has_value());
    CHECK(swizzled->volume);
    REQUIRE(swizzled->levels.size() == 1);
    CHECK(swizzled->levels[0].depth == 2);
    CHECK(swizzled->levels[0].size == 8 * 4 * 2 * 2);
    // RPCS3 calculate_z_index: x0 y0 z0 x1 y1 x2 for an 8x4x2 volume.
    CHECK(morton_index_3d(1, 0, 0, 3, 2, 1) == 1);
    CHECK(morton_index_3d(0, 1, 0, 3, 2, 1) == 2);
    CHECK(morton_index_3d(0, 0, 1, 3, 2, 1) == 4);
    CHECK(morton_index_3d(2, 0, 0, 3, 2, 1) == 8);
    CHECK(morton_index_3d(0, 2, 0, 3, 2, 1) == 16);
    CHECK(morton_index_3d(4, 0, 0, 3, 2, 1) == 32);
    CHECK(morton_index_3d(3, 1, 0, 2, 2, 0) == morton_index(3, 1, 2, 2)); // no depth bits: the 2D order

    // Decoded: texel (x, y, z) holds the value it was written with, through the order above.
    std::vector<u8> bytes(8 * 4 * 2 * 2);
    for (u32 z = 0; z < 2; z++)
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 8; x++) ps_st16(&bytes[morton_index_3d(x, y, z, 3, 2, 1) * 2], static_cast<u16>((x << 11) | (y << 5) | z));
    std::vector<std::array<float, 4>> out;
    REQUIRE(decode_level_cpu(*swizzled, swizzled->levels[0], bytes.data(), out));
    REQUIRE(out.size() == 64);
    for (u32 z = 0; z < 2; z++)
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 8; x++) {
                const auto& o = out[(z * 4 + y) * 8 + x];
                CHECK(static_cast<int>(o[0] * 255.f + 0.5f) == ((x * 527 + 23) >> 6));
                CHECK(static_cast<int>(o[2] * 255.f + 0.5f) == ((z * 527 + 23) >> 6));
            }

    // Linear: slices of pitch * height bytes.
    t.format = tex::R5G6B5 | tex::kLinearFlag;
    t.pitch = 32;
    const auto linear = compute_texture_layout(t);
    REQUIRE(linear.has_value());
    CHECK(linear->levels[0].slice_bytes() == 32 * 4);
    CHECK(linear->levels[0].size == 32 * 4 * 2);
}

TEST_CASE("CPU decoders: ARGB8 linear and swizzled agree")
{
    // A 4x4 image where texel (x, y) = A=0x80+i R=x*40 G=y*40 B=i, stored linear and swizzled.
    std::vector<u8> linear(64), swizzled(64);
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 4; x++) {
            const u8 px[4] = {static_cast<u8>(0x80 + y * 4 + x), static_cast<u8>(x * 40), static_cast<u8>(y * 40),
                static_cast<u8>(y * 4 + x)};
            std::memcpy(&linear[(y * 4 + x) * 4], px, 4);
            std::memcpy(&swizzled[morton_index(x, y, 2, 2) * 4], px, 4);
        }
    }
    const auto ll = compute_texture_layout(make_texture(tex::A8R8G8B8 | tex::kLinearFlag, 4, 4, 1, 16));
    const auto sl = compute_texture_layout(make_texture(tex::A8R8G8B8, 4, 4));
    REQUIRE(ll.has_value());
    REQUIRE(sl.has_value());
    std::vector<std::array<float, 4>> a, b;
    REQUIRE(decode_level_cpu(*ll, ll->levels[0], linear.data(), a));
    REQUIRE(decode_level_cpu(*sl, sl->levels[0], swizzled.data(), b));
    CHECK(a == b);
    CHECK(a[1 * 4 + 2][0] == 80.f / 255.f); // R = x * 40
    CHECK(a[1 * 4 + 2][1] == 40.f / 255.f); // G = y * 40
    CHECK(a[1 * 4 + 2][3] == static_cast<float>(0x80 + 6) / 255.f);
}

TEST_CASE("CPU decoders: 16-bit formats are big-endian with RPCS3's bit expansion")
{
    auto decode_one = [](u32 format, u16 value) {
        u8 bytes[2];
        ps_st16(bytes, value);
        const auto l = compute_texture_layout(make_texture(format | tex::kLinearFlag, 1, 1, 1, 2));
        REQUIRE(l.has_value());
        std::vector<std::array<float, 4>> out;
        REQUIRE(decode_level_cpu(*l, l->levels[0], bytes, out));
        std::array<int, 4> u8s{};
        for (u32 i = 0; i < 4; i++) u8s[i] = static_cast<int>(out[0][i] * 255.f + 0.5f);
        return u8s;
    };
    CHECK(decode_one(tex::R5G6B5, 0xf800) == std::array<int, 4>{255, 0, 0, 255});
    CHECK(decode_one(tex::R5G6B5, 0x07e0) == std::array<int, 4>{0, 255, 0, 255});
    CHECK(decode_one(tex::R5G6B5, 0x0010) == std::array<int, 4>{0, 0, 132, 255});
    CHECK(decode_one(tex::A1R5G5B5, 0x801f) == std::array<int, 4>{0, 0, 255, 255});
    CHECK(decode_one(tex::A1R5G5B5, 0x7c00) == std::array<int, 4>{255, 0, 0, 0});
    CHECK(decode_one(tex::D1R5G5B5, 0x7c00) == std::array<int, 4>{255, 0, 0, 255});
    CHECK(decode_one(tex::A4R4G4B4, 0x8f04) == std::array<int, 4>{255, 0, 68, 136});
    CHECK(decode_one(tex::R5G5B5A1, 0xf801) == std::array<int, 4>{255, 0, 0, 255});
    CHECK(decode_one(tex::R6G5B5, 0xfc00) == std::array<int, 4>{255, 0, 0, 255});
    CHECK(decode_one(tex::DEPTH16, 0xffff) == std::array<int, 4>{255, 0, 0, 255}); // the depth in red, unorm
    CHECK(decode_one(tex::DEPTH16, 0x8080) == std::array<int, 4>{128, 0, 0, 255});
}

TEST_CASE("CPU decoders: 24-bit depth textures in memory, fixed and float (Assassin's Creed, D-163)")
{
    auto decode = [](u32 format, u32 word) {
        TextureInfo t;
        t.format = format | tex::kLinearFlag;
        t.width = 1;
        t.height = 1;
        t.pitch = 4;
        t.dimension = 2;
        t.mipmaps = 1;
        t.border_type = true;
        std::string why;
        const auto l = compute_texture_layout(t, &why);
        INFO(why);
        REQUIRE(l);
        const u8 bytes[4] = {static_cast<u8>(word >> 24), static_cast<u8>(word >> 16), static_cast<u8>(word >> 8),
            static_cast<u8>(word)};
        std::vector<std::array<float, 4>> out;
        REQUIRE(decode_level_cpu(*l, l->levels[0], bytes, out));
        return out[0][0];
    };
    CHECK(decode(tex::DEPTH24_D8, 0xFFFFFF00) == 1.f);           // the depth in the top 24 bits, unorm
    CHECK(decode(tex::DEPTH24_D8, 0x000000FF) == 0.f);           // the stencil byte is not depth
    CHECK(decode(tex::DEPTH24_D8, 0x80000012) == Catch::Approx(0.5).epsilon(1e-6));
    CHECK(decode(tex::DEPTH24_D8_FLOAT, 0x7F000034) == 1.f);     // f24 0x7F0000 << 7 = 1.0f (RPCS3 d24f_to_f32)
    CHECK(decode(tex::DEPTH24_D8_FLOAT, 0x7E000000) == 0.5f);
    CHECK(decode(tex::DEPTH24_D8_FLOAT, 0x00000000) == 0.f);
    const TextureFormatInfo& d24 = *texture_format_info(tex::DEPTH24_D8);
    CHECK(d24.decode == TexelDecode::D24);
    CHECK(texture_channel_sources(d24, tex::kRemapIdentity) == std::array<u8, 4>{0, 0, 0, 0}); // in every channel
}

TEST_CASE("CPU decoders: HILO8, HILO_S8 and Y16_X16_FLOAT, read as RPCS3 maps them (D-163)")
{
    auto decode = [](u32 format, std::vector<u8> bytes, u16 pitch) {
        TextureInfo t;
        t.format = format | tex::kLinearFlag;
        t.width = 1;
        t.height = 1;
        t.pitch = pitch;
        t.dimension = 2;
        t.mipmaps = 1;
        t.border_type = true;
        const auto l = compute_texture_layout(t, nullptr);
        REQUIRE(l);
        std::vector<std::array<float, 4>> out;
        REQUIRE(decode_level_cpu(*l, l->levels[0], bytes.data(), out));
        return out[0];
    };
    // HILO: HI first in memory; the texture holds LO in red, HI in green; (A, R, G, B) read {R, G, R, G}.
    CHECK(decode(tex::COMPRESSED_HILO8, {0xFF, 0x00}, 2) == std::array<float, 4>{0.f, 1.f, 0.f, 1.f});
    CHECK(decode(tex::COMPRESSED_HILO_S8, {0x81, 0x7F}, 2) == std::array<float, 4>{1.f, -1.f, 0.f, 1.f});
    CHECK(decode(tex::COMPRESSED_HILO_S8, {0x80, 0x00}, 2)[1] == -1.f); // -128 clamps to -1
    CHECK(texture_channel_sources(*texture_format_info(tex::COMPRESSED_HILO8), tex::kRemapIdentity) ==
        std::array<u8, 4>{1, 0, 1, 0}); // Metal r, g, b, a = logical R (green), G (red), B (green), A (red)
    // Y16_X16_FLOAT: two BE halves, the first in red.
    CHECK(decode(tex::Y16_X16_FLOAT, {0x3C, 0x00, 0xC0, 0x00}, 4) == std::array<float, 4>{1.f, -2.f, 0.f, 1.f});
}

TEST_CASE("CPU decoders: DXT1, DXT3 and DXT5 blocks")
{
    // DXT1, 4-colour mode: c0 = red (0xf800) > c1 = blue (0x001f); indices 0,1,2,3 on the first row, then 0s.
    const u8 bc1[8] = {0x00, 0xf8, 0x1f, 0x00, 0xe4, 0x00, 0x00, 0x00};
    std::array<std::array<u8, 4>, 16> t{};
    decode_bc1_block(bc1, t);
    CHECK(t[0] == std::array<u8, 4>{255, 0, 0, 255});
    CHECK(t[1] == std::array<u8, 4>{0, 0, 255, 255});
    CHECK(t[2] == std::array<u8, 4>{170, 0, 85, 255}); // (2 * c0 + c1) / 3
    CHECK(t[3] == std::array<u8, 4>{85, 0, 170, 255});
    CHECK(t[4] == std::array<u8, 4>{255, 0, 0, 255});
    // 3-colour mode (c0 <= c1): index 3 is transparent black
    const u8 bc1b[8] = {0x1f, 0x00, 0x00, 0xf8, 0xe4, 0x00, 0x00, 0x00};
    decode_bc1_block(bc1b, t);
    CHECK(t[2] == std::array<u8, 4>{127, 0, 127, 255});
    CHECK(t[3] == std::array<u8, 4>{0, 0, 0, 0});

    // DXT3: explicit 4-bit alpha, first texel alpha 0xf, second 0x8
    u8 bc2[16] = {0x8f, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(bc2 + 8, bc1b, 8); // colour block forced to 4-colour mode
    decode_bc2_block(bc2, t);
    CHECK(t[0][3] == 255);
    CHECK(t[1][3] == 136);
    CHECK(t[3][3] == 0);
    // 4-colour mode even though c0 <= c1: index 3 is (c0 + 2 * c1) / 3 of blue and red, not transparent
    CHECK(t[3][0] == 170);
    CHECK(t[3][2] == 85);

    // DXT5: a0 = 255, a1 = 0 (8-alpha mode); indices 0, 1, 2, 7 for the first texels
    u8 bc3[16] = {255, 0};
    const u64 alpha_bits = 0 | (1ull << 3) | (2ull << 6) | (7ull << 9);
    for (u32 i = 0; i < 6; i++) bc3[2 + i] = static_cast<u8>(alpha_bits >> (8 * i));
    std::memcpy(bc3 + 8, bc1, 8);
    decode_bc3_block(bc3, t);
    CHECK(t[0][3] == 255);
    CHECK(t[1][3] == 0);
    CHECK(t[2][3] == (6 * 255) / 7);
    CHECK(t[3][3] == 255 / 7);
}

TEST_CASE("remap decoding and channel sources")
{
    const TextureRemap id = decode_remap(tex::kRemapIdentity);
    CHECK(id.identity());
    const TextureFormatInfo& argb = *texture_format_info(tex::A8R8G8B8);
    CHECK(texture_channel_sources(argb, tex::kRemapIdentity) == std::array<u8, 4>{0, 1, 2, 3});
    // R <- B, B <- R: sources A=A, R=B, G=G, B=R -> 0 | 3 << 2 | 2 << 4 | 1 << 6 = 0x6c
    CHECK(texture_channel_sources(argb, 0xaa6c) == std::array<u8, 4>{2, 1, 0, 3});
    // Alpha forced to one (control A = 1), others remapped
    CHECK(texture_channel_sources(argb, 0xa9e4) == std::array<u8, 4>{0, 1, 2, kChannelOne});
    // Green forced to zero (control G = 0 at bits 12-13)
    CHECK(texture_channel_sources(argb, 0x8ae4) == std::array<u8, 4>{0, kChannelZero, 2, 3});
    // X32_FLOAT replicates its single channel and ignores remap sources
    const TextureFormatInfo& x32 = *texture_format_info(tex::X32_FLOAT);
    CHECK(texture_channel_sources(x32, 0xaa1b) == std::array<u8, 4>{0, 0, 0, 0});
    // RPCS3's base mappings (A, R, G, B): X16 {R, 1, R, 1}, Y16_X16 {G, R, G, R}; Metal order is r, g, b, a.
    const TextureFormatInfo& x16 = *texture_format_info(tex::X16);
    CHECK(texture_channel_sources(x16, tex::kRemapIdentity) == std::array<u8, 4>{kChannelOne, 0, kChannelOne, 0});
    const TextureFormatInfo& y16x16 = *texture_format_info(tex::Y16_X16);
    CHECK(texture_channel_sources(y16x16, tex::kRemapIdentity) == std::array<u8, 4>{0, 1, 0, 1});
    // G8B8 too (D-137): its bytes G B decode to red B, green G, read (A, R, G, B) = (G, B, G, B).
    const TextureFormatInfo& g8b8 = *texture_format_info(tex::G8B8);
    CHECK(g8b8.decode == TexelDecode::G8B8);
    CHECK(texture_channel_sources(g8b8, tex::kRemapIdentity) == std::array<u8, 4>{0, 1, 0, 1});
    const auto layout = compute_texture_layout(make_texture(tex::G8B8 | tex::kLinearFlag, 1, 1, 1, 2));
    REQUIRE(layout.has_value());
    const u8 texel[2] = {0x20, 0x80};
    std::vector<std::array<float, 4>> out;
    REQUIRE(decode_level_cpu(*layout, layout->levels[0], texel, out));
    REQUIRE(out.size() == 1);
    CHECK(static_cast<int>(out[0][0] * 255.f + 0.5f) == 0x80);
    CHECK(static_cast<int>(out[0][1] * 255.f + 0.5f) == 0x20);
    // The depth formats read their value in every channel, like X32_FLOAT.
    const TextureFormatInfo& d16 = *texture_format_info(tex::DEPTH16);
    CHECK(d16.decode == TexelDecode::D16);
    CHECK(texture_channel_sources(d16, tex::kRemapIdentity) == std::array<u8, 4>{0, 0, 0, 0});
}

TEST_CASE("half floats")
{
    CHECK(half_to_float(0x3c00) == 1.f);
    CHECK(half_to_float(0xc000) == -2.f);
    CHECK(half_to_float(0x0000) == 0.f);
    CHECK(half_to_float(0x7bff) == 65504.f);
    CHECK(half_to_float(0x0001) == Catch::Approx(5.96046448e-8f));
    CHECK(std::isinf(half_to_float(0x7c00)));
}
