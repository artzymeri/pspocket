#pragma once

// RSX texture formats, guest memory layouts and CPU reference decoders.
//
// Layout rules come from RPCS3 rpcs3/Emu/RSX/Common/TextureUtils.cpp (get_subresources_layout_impl):
//  - Linear ("LN") textures: every mip level uses the row pitch of level 0 (CONTROL3 pitch); levels follow each
//    other with no padding.
//  - Swizzled textures: rows are packed (pitch = width x block size), texels are in Morton (Z) order over the
//    power-of-two image; each cube face's mip chain starts 128-byte aligned.
//  - DXT1/3/5 blocks are stored exactly as BC1/2/3 (little-endian, never swizzled or byte-swapped); the swizzle flag
//    only changes the row pitch.
//  - Border texels (format bit 3 clear: CELL_GCM_TEXTURE_BORDER_TEXTURE) add 1 texel (linear) or 4 texels
//    (swizzled, padded to powers of two) around each level.
// Every other format is big-endian per element (16-bit or 32-bit words), decoded on the GPU (metal/) with these
// CPU decoders as the reference the tests compare against.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/rsx/state.hpp"

namespace pspocket::rsx {

// How a decoded texel is produced; selects the GPU decode kernel path.
enum class TexelDecode : u8 {
    Unsupported,
    ARGB8,     // bytes A R G B
    DRGB8,     // bytes X R G B, alpha = 1
    R5G6B5,    // BE u16
    A1R5G5B5,  // BE u16
    D1R5G5B5,  // BE u16, alpha = 1
    A4R4G4B4,  // BE u16
    R5G5B5A1,  // BE u16
    R6G5B5,    // BE u16, alpha = 1
    B8,        // one byte, replicated to every channel
    X32F,      // BE f32 -> red
    W16Z16Y16X16F, // 4 x BE f16, first in memory = red
    W32Z32Y32X32F, // 4 x BE f32, first in memory = red
    Y16X16F,   // 2 x BE f16
    X16,       // BE u16, unsigned normalized -> red
    Y16X16,    // 2 x BE u16, unsigned normalized -> red, green (first in memory = red)
    DXT1,
    DXT23,
    DXT45,
    D16,       // BE u16, unsigned normalized depth -> red, read in every channel (DEPTH16 in memory)
    G8B8,      // bytes G B -> red B, green G (RPCS3 uploads the big-endian u16 into R8G8), read (A, R, G, B) = (G, B, G, B)
    D24,       // BE u32, depth in the top 24 bits (unsigned normalized), stencil below -> red, read in every channel
    D24F,      // BE u32, depth as a 24-bit float in the top 24 bits (RPCS3 ShuffleBytes.glsl d24f_to_f32: bits << 7)
    HILO8,     // bytes HI LO, unsigned normalized -> red LO, green HI (RPCS3 uploads the BE u16 into R8G8)
    HILOS8,    // the same, signed normalized
};

// Decoded storage the GPU texture uses (the backend maps it to a Metal pixel format).
enum class DecodedFormat : u8 { RGBA8, R32F, RG16F, RGBA16F, RGBA32F, BC1, BC2, BC3, R16, RG16 };

struct TextureFormatInfo {
    u32 gcm_format = 0;       // base format (LN/UN masked off)
    const char* name = "";
    u32 block_bytes = 0;      // bytes per block (per texel when block_texels == 1)
    u32 block_texels = 1;     // block edge in texels: 4 for DXT
    TexelDecode decode = TexelDecode::Unsupported;
    DecodedFormat decoded = DecodedFormat::RGBA8;
};

// nullptr for unknown formats. Known-but-not-yet-decodable formats return an entry with decode == Unsupported.
const TextureFormatInfo* texture_format_info(u32 base_format);

struct TextureLevel {
    u32 face = 0;
    u32 level = 0;
    u32 width = 0, height = 0;   // texels
    u32 width_blocks = 0, height_blocks = 0;
    u32 offset = 0;              // bytes from the texture's base address to the level's (padded) image
    u32 pitch = 0;               // bytes per row of blocks in memory
    u32 rows = 0;                // rows of blocks in memory (incl. border rows)
    u32 border = 0;              // border texels on each side (0, 1 or 4)
    u32 size = 0;                // bytes occupied by the level (all its slices)
    // Swizzled images: the Morton-ordered power-of-two area, in blocks (incl. border).
    u32 swizzle_width = 0, swizzle_height = 0;
    // 3D textures: the level's slices, one after another (linear), or in the Morton order with x, y and z
    // interleaved over swizzle_depth (swizzled; RPCS3 convert_linear_swizzle_3d).
    u32 depth = 1;
    u32 swizzle_depth = 1;
    u32 slice_bytes() const { return pitch * rows; }
};

struct TextureLayout {
    const TextureFormatInfo* format = nullptr;
    bool swizzled = false;
    bool cubemap = false;
    bool volume = false; // a 3D texture
    u32 faces = 1;
    u32 mips = 1;
    u32 total_size = 0;
    std::vector<TextureLevel> levels; // face-major: levels[face * mips + level]
};

// Mip levels the hardware samples: min(requested, full chain) (RSXTexture.cpp get_exact_mipmap_count).
u32 texture_mip_count(const TextureInfo& t, const TextureFormatInfo& fmt);

// Guest memory layout of a texture. Fails (nullopt + reason) for unknown formats, compressed 3D textures and zero sizes.
std::optional<TextureLayout> compute_texture_layout(const TextureInfo& t, std::string* why = nullptr);

// Morton (Z-order) index used by RSX swizzled textures and surfaces: x and y bits interleaved from bit 0 (x first)
// while both dimensions have bits left, then the remaining bits of the larger dimension
// (RPCS3 Utils/image_utils.hpp calculate_z_index). log2 sizes of the power-of-two area.
constexpr u32 morton_index(u32 x, u32 y, u32 log2_w, u32 log2_h)
{
    u32 offset = 0, shift = 0;
    while (x | y) {
        if (log2_w) {
            offset |= (x & 1) << shift++;
            x >>= 1;
            log2_w--;
        }
        if (log2_h) {
            offset |= (y & 1) << shift++;
            y >>= 1;
            log2_h--;
        }
        if (!log2_w && !log2_h) break;
    }
    return offset;
}

// The 3D form: x, y and z bits interleaved in that order while each has bits left (RPCS3 image_utils.hpp
// calculate_z_index for 3D textures).
constexpr u32 morton_index_3d(u32 x, u32 y, u32 z, u32 log2_w, u32 log2_h, u32 log2_d)
{
    u32 offset = 0, shift = 0;
    while (x | y | z) {
        if (log2_w) {
            offset |= (x & 1) << shift++;
            x >>= 1;
            log2_w--;
        }
        if (log2_h) {
            offset |= (y & 1) << shift++;
            y >>= 1;
            log2_h--;
        }
        if (log2_d) {
            offset |= (z & 1) << shift++;
            z >>= 1;
            log2_d--;
        }
        if (!log2_w && !log2_h && !log2_d) break;
    }
    return offset;
}

constexpr u32 ceil_log2(u32 v)
{
    u32 r = 0;
    while ((1u << r) < v) r++;
    return r;
}

// Texture channel remap (NV4097_SET_TEXTURE_CONTROL1, RPCS3 Utils/color_utils.hpp decode_remap_encoding). Channels
// are in A, R, G, B order: for output channel c, control[c] is 0 = zero, 1 = one, 2 = take input channel source[c]
// (0 = A, 1 = R, 2 = G, 3 = B).
struct TextureRemap {
    std::array<u8, 4> source{0, 1, 2, 3};
    std::array<u8, 4> control{2, 2, 2, 2};
    bool identity() const { return source == std::array<u8, 4>{0, 1, 2, 3} && control == std::array<u8, 4>{2, 2, 2, 2}; }
};
TextureRemap decode_remap(u32 remap);

// Where a sampled texel's red, green, blue and alpha come from: 0-3 = channel r, g, b, a of the decoded texture,
// 4 = zero, 5 = one. Combines the format's channel mapping (RPCS3 VK/VKFormats.cpp get_component_mapping, e.g.
// X32_FLOAT replicates red) with the remap register. Floating point formats keep their channel order whatever the
// remap's source bits say ("cannot be remapped on realhw", RSXTexture.cpp decoded_remap); the zero/one controls
// still apply.
inline constexpr u8 kChannelZero = 4;
inline constexpr u8 kChannelOne = 5;
std::array<u8, 4> texture_channel_sources(const TextureFormatInfo& fmt, u32 remap);

// ---- texel conversions in the fragment shader (D-174) -------------------------------------------------------------

// What a texture format allows of the per-channel controls (RPCS3 Common/TextureUtils.cpp get_format_features): signed
// channels (NV4097_SET_TEXTURE_FILTER bits 28-31), gamma (NV4097_SET_TEXTURE_ADDRESS bits 20-23) and the biased
// unsigned remap (ADDRESS bits 12-15, and the fragment program's _bx2); 16-bit channels sign and expand by 16-bit steps.
namespace texfeature {
inline constexpr u32 kSigned = 1u << 0;
inline constexpr u32 kBiasedRenormalization = 1u << 1;
inline constexpr u32 kGamma = 1u << 2;
inline constexpr u32 k16BitChannels = 1u << 3;
} // namespace texfeature
u32 texture_format_features(u32 base_format);

// The remap as the hardware applies it, for the formats that cannot remap freely (RPCS3 RSXTexture.cpp decoded_remap:
// float formats keep their order, X16/Y16_X16/HILO remap two 16-bit channels).
u32 texture_hardware_remap(u32 base_format, u32 remap);
// The channels (A, R, G, B order, bit 0 alpha) after the remap that read an input channel in `bits` (RPCS3
// color_utils.hpp texture_channel_remap_t::shuffle_mask_bits); channels the remap forces to 0 or 1 are left out.
u32 texture_remap_mask(u32 hardware_remap, u32 bits);

// What the fragment shader does to a texel a unit returns (MslFsParams::tex_control, msl.cpp rsx_texel): RPCS3's
// texture_control_bits (Utils/color_utils.hpp) as RSXTexture.cpp format_ex and RSXThread.cpp make them. Channel masks
// are in A, R, G, B order (bit 0 alpha), over the channels after the remap.
namespace texctl {
inline constexpr u32 kGammaShift = 0;    // bits 0-3: sRGB to linear (not remapped, RPCS3)
inline constexpr u32 kAlphaKill = 1u << 4; // NV4097_SET_TEXTURE_CONTROL0 bit 2: a texel with alpha 0 discards the fragment
inline constexpr u32 kExpandShift = 6;   // bits 6-9: the unsigned remap BIASED: (x * 255 - 128) / 127
inline constexpr u32 kSignShift = 10;    // bits 10-13: the channel's byte is signed: (s8)(x * 255) / 127
inline constexpr u32 kBx2Shift = 16;     // bits 16-19: what _bx2 expands, the channels the remap reads (RPCS3 keeps
                                         // them in the remap's high word; its control word has other bits here)
inline constexpr u32 kFeatureShift = 25; // bits 25-28: texture_format_features (RPCS3 FF_SIGNED_BIT and on)
inline constexpr u32 kConvertMask = (0xfu << kGammaShift) | (0xfu << kExpandShift) | (0xfu << kSignShift);
} // namespace texctl
u32 texture_control(const TextureInfo& t);

// ---- CPU reference decoders -------------------------------------------------------------------------------------

// Decodes one level to RGBA float (r, g, b, a) per texel, row-major, width x height. src points at the texture's
// base address in host memory (the guest bytes). Returns false for formats without a decoder.
bool decode_level_cpu(const TextureLayout& layout, const TextureLevel& level, const u8* src,
    std::vector<std::array<float, 4>>& out);

// One 4x4 BCn block to RGBA8 texels (row-major, 16 entries of r, g, b, a). DXT1 = BC1, DXT23 = BC2, DXT45 = BC3.
void decode_bc1_block(const u8* block, std::array<std::array<u8, 4>, 16>& out);
void decode_bc2_block(const u8* block, std::array<std::array<u8, 4>, 16>& out);
void decode_bc3_block(const u8* block, std::array<std::array<u8, 4>, 16>& out);

float half_to_float(u16 h);

} // namespace pspocket::rsx
