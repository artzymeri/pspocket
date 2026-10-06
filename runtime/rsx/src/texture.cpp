#include "pspocket/rsx/texture.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

#include "pspocket/core/be.h"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx {
namespace {

// Format table: block sizes from RPCS3 TextureUtils.cpp get_subresources_layout_impl / get_format_block_size_in_bytes.
// decode == Unsupported marks formats the decoder does not handle yet (they fail loudly in the backend).
constexpr TextureFormatInfo kFormats[] = {
    {tex::B8, "B8", 1, 1, TexelDecode::B8, DecodedFormat::RGBA8},
    {tex::A1R5G5B5, "A1R5G5B5", 2, 1, TexelDecode::A1R5G5B5, DecodedFormat::RGBA8},
    {tex::A4R4G4B4, "A4R4G4B4", 2, 1, TexelDecode::A4R4G4B4, DecodedFormat::RGBA8},
    {tex::R5G6B5, "R5G6B5", 2, 1, TexelDecode::R5G6B5, DecodedFormat::RGBA8},
    {tex::A8R8G8B8, "A8R8G8B8", 4, 1, TexelDecode::ARGB8, DecodedFormat::RGBA8},
    {tex::COMPRESSED_DXT1, "DXT1", 8, 4, TexelDecode::DXT1, DecodedFormat::BC1},
    {tex::COMPRESSED_DXT23, "DXT23", 16, 4, TexelDecode::DXT23, DecodedFormat::BC2},
    {tex::COMPRESSED_DXT45, "DXT45", 16, 4, TexelDecode::DXT45, DecodedFormat::BC3},
    {tex::G8B8, "G8B8", 2, 1, TexelDecode::G8B8, DecodedFormat::RGBA8},
    {tex::COMPRESSED_B8R8_G8R8, "COMPRESSED_B8R8_G8R8", 4, 2, TexelDecode::Unsupported, DecodedFormat::RGBA8},
    {tex::COMPRESSED_R8B8_R8G8, "COMPRESSED_R8B8_R8G8", 4, 2, TexelDecode::Unsupported, DecodedFormat::RGBA8},
    {tex::R6G5B5, "R6G5B5", 2, 1, TexelDecode::R6G5B5, DecodedFormat::RGBA8},
    {tex::DEPTH24_D8, "DEPTH24_D8", 4, 1, TexelDecode::D24, DecodedFormat::R32F},
    {tex::DEPTH24_D8_FLOAT, "DEPTH24_D8_FLOAT", 4, 1, TexelDecode::D24F, DecodedFormat::R32F},
    {tex::DEPTH16, "DEPTH16", 2, 1, TexelDecode::D16, DecodedFormat::R16},
    {tex::DEPTH16_FLOAT, "DEPTH16_FLOAT", 2, 1, TexelDecode::Unsupported, DecodedFormat::RGBA8},
    {tex::X16, "X16", 2, 1, TexelDecode::X16, DecodedFormat::R16},
    {tex::Y16_X16, "Y16_X16", 4, 1, TexelDecode::Y16X16, DecodedFormat::RG16},
    {tex::R5G5B5A1, "R5G5B5A1", 2, 1, TexelDecode::R5G5B5A1, DecodedFormat::RGBA8},
    {tex::COMPRESSED_HILO8, "COMPRESSED_HILO8", 2, 1, TexelDecode::HILO8, DecodedFormat::RGBA8},
    {tex::COMPRESSED_HILO_S8, "COMPRESSED_HILO_S8", 2, 1, TexelDecode::HILOS8, DecodedFormat::RGBA16F},
    {tex::W16_Z16_Y16_X16_FLOAT, "W16_Z16_Y16_X16_FLOAT", 8, 1, TexelDecode::W16Z16Y16X16F, DecodedFormat::RGBA16F},
    {tex::W32_Z32_Y32_X32_FLOAT, "W32_Z32_Y32_X32_FLOAT", 16, 1, TexelDecode::W32Z32Y32X32F, DecodedFormat::RGBA32F},
    {tex::X32_FLOAT, "X32_FLOAT", 4, 1, TexelDecode::X32F, DecodedFormat::R32F},
    {tex::D1R5G5B5, "D1R5G5B5", 2, 1, TexelDecode::D1R5G5B5, DecodedFormat::RGBA8},
    {tex::D8R8G8B8, "D8R8G8B8", 4, 1, TexelDecode::DRGB8, DecodedFormat::RGBA8},
    {tex::Y16_X16_FLOAT, "Y16_X16_FLOAT", 4, 1, TexelDecode::Y16X16F, DecodedFormat::RG16F},
};

u32 floor_log2(u32 v) { return v ? 31u - static_cast<u32>(std::countl_zero(v)) : 0u; }
u32 next_pow2(u32 v) { return v <= 1 ? 1 : 1u << ceil_log2(v); }

// 5- and 6-bit expansion to 8 bits as RPCS3 TextureUtils.cpp does ((x * 527 + 23) >> 6, (x * 259 + 33) >> 6).
u8 expand5(u32 v) { return static_cast<u8>((v * 527 + 23) >> 6); }
u8 expand6(u32 v) { return static_cast<u8>((v * 259 + 33) >> 6); }
float unorm8(u32 v) { return static_cast<float>(v) / 255.f; }

std::array<float, 4> rgba8(u32 r, u32 g, u32 b, u32 a) { return {unorm8(r), unorm8(g), unorm8(b), unorm8(a)}; }

// BC1 color endpoints and palette (DXT1/3/5 color block). four_color forces the 4-color mode (BC2/BC3).
void bc_color_palette(const u8* block, bool four_color, std::array<std::array<u8, 4>, 4>& pal)
{
    const u32 c0 = block[0] | (block[1] << 8);
    const u32 c1 = block[2] | (block[3] << 8);
    auto expand = [](u32 c, std::array<u8, 4>& out) {
        const u32 r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
        out = {static_cast<u8>((r5 << 3) | (r5 >> 2)), static_cast<u8>((g6 << 2) | (g6 >> 4)),
            static_cast<u8>((b5 << 3) | (b5 >> 2)), 255};
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    if (four_color || c0 > c1) {
        for (u32 ch = 0; ch < 3; ch++) {
            pal[2][ch] = static_cast<u8>((2 * pal[0][ch] + pal[1][ch]) / 3);
            pal[3][ch] = static_cast<u8>((pal[0][ch] + 2 * pal[1][ch]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (u32 ch = 0; ch < 3; ch++) pal[2][ch] = static_cast<u8>((pal[0][ch] + pal[1][ch]) / 2);
        pal[2][3] = 255;
        pal[3] = {0, 0, 0, 0};
    }
}

void bc_color_block(const u8* block, bool four_color, std::array<std::array<u8, 4>, 16>& out)
{
    std::array<std::array<u8, 4>, 4> pal{};
    bc_color_palette(block, four_color, pal);
    const u32 indices = block[4] | (block[5] << 8) | (block[6] << 16) | (static_cast<u32>(block[7]) << 24);
    for (u32 i = 0; i < 16; i++) out[i] = pal[(indices >> (2 * i)) & 3];
}

} // namespace

const TextureFormatInfo* texture_format_info(u32 base_format)
{
    for (const auto& f : kFormats) {
        if (f.gcm_format == base_format) return &f;
    }
    return nullptr;
}

u32 texture_mip_count(const TextureInfo& t, const TextureFormatInfo& fmt)
{
    // RPCS3 RSXTexture.cpp get_exact_mipmap_count
    const u32 requested = std::max<u32>(t.mipmaps, 1);
    const u32 w = t.width, h = t.dimension == 1 ? 1 : t.height;
    u32 full;
    if (fmt.block_texels == 4) {
        full = floor_log2(std::max<u32>(std::min(w / 4, h / 4), 1)) + 1;
    } else {
        full = floor_log2(std::max<u32>(std::max(w, h), 1)) + 1;
    }
    return std::min(requested, full);
}

std::optional<TextureLayout> compute_texture_layout(const TextureInfo& t, std::string* why)
{
    auto fail = [&](std::string reason) -> std::optional<TextureLayout> {
        if (why) *why = std::move(reason);
        return std::nullopt;
    };
    const TextureFormatInfo* fmt = texture_format_info(t.base_format());
    if (!fmt) return fail(std::format("unknown texture format {:#x}", t.base_format()));
    if (fmt->decode == TexelDecode::Unsupported) return fail(std::format("texture format {} is not decoded yet", fmt->name));
    if (t.dimension == 3 && fmt->block_texels != 1) {
        return fail(std::format("compressed 3D textures ({}x{}x{} {}) are not implemented yet", t.width, t.height,
            t.depth, fmt->name));
    }
    if (t.dimension < 1 || t.dimension > 3) return fail(std::format("invalid texture dimension {}", t.dimension));

    u32 w = t.width;
    u32 h = t.dimension == 1 ? 1 : t.height;
    if (w == 0 || h == 0) return fail("texture with a zero size");

    TextureLayout layout;
    layout.format = fmt;
    layout.swizzled = !t.is_linear();
    layout.cubemap = t.cubemap && t.dimension == 2;
    layout.volume = t.dimension == 3;
    layout.faces = layout.cubemap ? 6 : 1;
    layout.mips = texture_mip_count(t, *fmt);

    const bool compressed = fmt->block_texels == 4;
    const bool has_border = !t.border_type && !compressed;
    const u32 border = has_border ? (layout.swizzled ? 4u : 1u) : 0u;
    u32 pitch = t.pitch;
    if (!layout.swizzled) {
        // RPCS3 TextureUtils.cpp: a pitch smaller than the packed row narrows the texture to the pitch (or, for 0,
        // makes it a single packed row).
        const u32 packed = ((w + fmt->block_texels - 1) / fmt->block_texels + 2 * border) * fmt->block_bytes;
        if (pitch < packed) {
            if (pitch) {
                w = std::max<u32>(pitch / fmt->block_bytes * fmt->block_texels, 1);
            } else {
                h = 1;
                pitch = packed;
            }
        }
    }

    u32 offset = 0;
    for (u32 face = 0; face < layout.faces; face++) {
        u32 mw = w, mh = h, md = layout.volume ? std::max<u32>(t.depth, 1) : 1;
        for (u32 level = 0; level < layout.mips; level++) {
            TextureLevel l;
            l.face = face;
            l.level = level;
            l.width = mw;
            l.height = mh;
            l.width_blocks = (mw + fmt->block_texels - 1) / fmt->block_texels;
            l.height_blocks = (mh + fmt->block_texels - 1) / fmt->block_texels;
            l.border = border;
            l.offset = offset;
            if (!layout.swizzled) {
                l.pitch = pitch;
                l.rows = l.height_blocks + 2 * border;
            } else if (!border) {
                l.pitch = l.width_blocks * fmt->block_bytes;
                l.rows = l.height_blocks;
            } else {
                l.pitch = next_pow2(l.width_blocks + 2 * border) * fmt->block_bytes;
                l.rows = next_pow2(l.height_blocks + 2 * border);
            }
            l.swizzle_width = l.pitch / fmt->block_bytes;
            l.swizzle_height = l.rows;
            // RPCS3 get_subresources_layout_impl: a 3D level is its slices, pitch * rows bytes each.
            l.depth = md;
            l.swizzle_depth = layout.swizzled ? next_pow2(md) : md;
            l.size = l.pitch * l.rows * md;
            offset += l.size;
            layout.levels.push_back(l);
            mw = std::max(mw / 2, 1u);
            mh = std::max(mh / 2, 1u);
            md = std::max(md / 2, 1u);
        }
        if (layout.swizzled) offset = (offset + 127) & ~127u; // faces of swizzled textures are 128-byte aligned
    }
    layout.total_size = offset;
    return layout;
}

TextureRemap decode_remap(u32 remap)
{
    // RPCS3 Utils/color_utils.hpp decode_remap_encoding: low byte = source channels, next byte = control.
    TextureRemap r;
    for (u32 c = 0; c < 4; c++) {
        r.source[c] = static_cast<u8>((remap >> (2 * c)) & 3);
        r.control[c] = static_cast<u8>((remap >> (8 + 2 * c)) & 3);
    }
    return r;
}

std::array<u8, 4> texture_channel_sources(const TextureFormatInfo& fmt, u32 remap)
{
    // Logical channels in A, R, G, B order -> decoded texture channel (r = 0, g = 1, b = 2, a = 3).
    std::array<u8, 4> base{3, 0, 1, 2};
    bool float_format = false;
    switch (fmt.decode) {
    case TexelDecode::X32F:
    case TexelDecode::D24:
    case TexelDecode::D24F:
    case TexelDecode::D16: // RPCS3 VKFormats.cpp get_component_mapping: the depth formats read R in every channel
        base = {0, 0, 0, 0};
        float_format = true;
        break;
    case TexelDecode::W16Z16Y16X16F:
    case TexelDecode::W32Z32Y32X32F: float_format = true; break;
    // RPCS3 VKFormats.cpp get_component_mapping (A, R, G, B): Y16_X16_FLOAT, HILO8 and HILO_S8 {R, G, R, G}.
    case TexelDecode::Y16X16F:
        base = {0, 1, 0, 1};
        float_format = true;
        break;
    case TexelDecode::HILO8:
    case TexelDecode::HILOS8: base = {0, 1, 0, 1}; break;
    // RPCS3 VKFormats.cpp get_component_mapping (A, R, G, B): X16 {R, 1, R, 1}, Y16_X16 and G8B8 {G, R, G, R}.
    case TexelDecode::X16: base = {0, kChannelOne, 0, kChannelOne}; break;
    case TexelDecode::Y16X16:
    case TexelDecode::G8B8: base = {1, 0, 1, 0}; break;
    default: break;
    }
    TextureRemap r = decode_remap(remap);
    if (float_format) r.source = {0, 1, 2, 3};
    // Metal red, green, blue, alpha are logical R, G, B, A (indices 1, 2, 3, 0 in ARGB order).
    static constexpr u8 kLogical[4] = {1, 2, 3, 0};
    std::array<u8, 4> out{};
    for (u32 m = 0; m < 4; m++) {
        const u8 c = kLogical[m];
        switch (r.control[c]) {
        case 0: out[m] = kChannelZero; break;
        case 1: out[m] = kChannelOne; break;
        default: out[m] = base[r.source[c]]; break;
        }
    }
    return out;
}

u32 texture_format_features(u32 base_format)
{
    // RPCS3 Common/TextureUtils.cpp get_format_features, with its notes from hardware tests.
    using namespace texfeature;
    switch (base_format) {
    case tex::B8:
    case tex::A1R5G5B5:
    case tex::A4R4G4B4:
    case tex::R5G6B5:
    case tex::A8R8G8B8:
    case tex::COMPRESSED_DXT1:
    case tex::COMPRESSED_DXT23:
    case tex::COMPRESSED_DXT45:
    case tex::G8B8:
    case tex::COMPRESSED_B8R8_G8R8:
    case tex::COMPRESSED_R8B8_R8G8:
    case tex::R6G5B5:
    case tex::R5G5B5A1:
    case tex::D1R5G5B5:
    case tex::D8R8G8B8: return kSigned | kGamma | kBiasedRenormalization; // the base formats: everything
    case tex::DEPTH24_D8:
    case tex::DEPTH24_D8_FLOAT:
    case tex::DEPTH16:
    case tex::DEPTH16_FLOAT: return kBiasedRenormalization; // gamma and _bx2 hang the hardware, signed does nothing
    case tex::X16: return kBiasedRenormalization | k16BitChannels;
    case tex::Y16_X16: return kSigned | kBiasedRenormalization | k16BitChannels;
    case tex::COMPRESSED_HILO8: return kBiasedRenormalization;
    default: return 0; // HILO_S8 and the float formats: nothing works
    }
}

u32 texture_hardware_remap(u32 base_format, u32 remap)
{
    // RPCS3 RSXTexture.cpp decoded_remap.
    const u32 override_bits = (remap >> 16) & 0xffff;
    switch (base_format) {
    case tex::X32_FLOAT:
    case tex::W32_Z32_Y32_X32_FLOAT:
    case tex::W16_Z16_Y16_X16_FLOAT: return (remap & ~0xffu) | 0xe4; // floating point textures cannot be remapped
    case tex::Y16_X16_FLOAT: return (remap & ~0xffu) | (override_bits ? 0x56u : 0x66u);
    case tex::X16:
    case tex::Y16_X16:
    case tex::COMPRESSED_HILO8:
    case tex::COMPRESSED_HILO_S8: {
        // Two 16-bit channels: 0xE4 or 0x4E, the low bit of the high word choosing R-R-R-G over R-G-R-G.
        u32 lo = remap & 0xff;
        switch (lo) {
        case 0xe4: lo = override_bits ? 0x56 : 0x66; break;
        case 0x4e: lo = override_bits ? 0xa9 : 0x99; break;
        case 0xee: lo = 0xaa; break;
        case 0x44: lo = 0x55; break;
        default: break;
        }
        return (remap & 0xff00) | lo;
    }
    default: return remap;
    }
}

u32 texture_remap_mask(u32 hardware_remap, u32 bits)
{
    // RPCS3 texture_channel_remap_t::shuffle_mask_bits: output channel c (A, R, G, B) is in the result when its control
    // is REMAP (2) and the input channel it reads is in `bits`.
    if (!bits || hardware_remap == tex::kRemapIdentity) return bits;
    u32 result = 0;
    for (u32 c = 0; c < 4; c++) {
        const u32 control = (hardware_remap >> (8 + 2 * c)) & 3, source = (hardware_remap >> (2 * c)) & 3;
        if (control == 2 && (bits & (1u << source))) result |= 1u << c;
    }
    return result;
}

u32 texture_control(const TextureInfo& t)
{
    u32 c = t.alpha_kill ? texctl::kAlphaKill : 0u;
    const u32 features = texture_format_features(t.base_format());
    if (!features) return c;
    // RPCS3 RSXTexture.cpp format_ex: signed over gamma over the unsigned remap, each on the channels the ones before
    // left. Signed channels are a property of the bytes read, so they follow the remap; gamma applies to what the
    // remap made (RPCS3's hardware notes), and its register bits are R, G, B, A where the others are A, R, G, B.
    const u32 remap = texture_hardware_remap(t.base_format(), t.remap);
    const u32 sign = (features & texfeature::kSigned) ? texture_remap_mask(remap, t.argb_signed & 0xf) : 0u;
    const u32 gamma = (features & texfeature::kGamma) ? ((((t.gamma << 1) & 0xf) | ((t.gamma >> 3) & 1)) & ~sign) : 0u;
    const u32 read = texture_remap_mask(remap, 0xf);
    const bool biased = t.unsigned_remap == 1; // CELL_GCM_TEXTURE_UNSIGNED_REMAP_BIASED
    const u32 expand = (features & texfeature::kBiasedRenormalization) && biased ? read & ~(sign | gamma) : 0u;
    c |= (gamma << texctl::kGammaShift) | (expand << texctl::kExpandShift) | (sign << texctl::kSignShift);
    c |= (read << texctl::kBx2Shift) | (features << texctl::kFeatureShift);
    return c;
}

float half_to_float(u16 h)
{
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 0x3ff;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            // subnormal: normalise
            u32 e = 113, m = mant;
            while (!(m & 0x400)) {
                m <<= 1;
                e--;
            }
            bits = (sign << 31) | (e << 23) | ((m & 0x3ff) << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | (0xffu << 23) | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(bits);
}

void decode_bc1_block(const u8* block, std::array<std::array<u8, 4>, 16>& out) { bc_color_block(block, false, out); }

void decode_bc2_block(const u8* block, std::array<std::array<u8, 4>, 16>& out)
{
    bc_color_block(block + 8, true, out);
    for (u32 i = 0; i < 16; i++) {
        const u32 a4 = (block[i / 2] >> (4 * (i & 1))) & 0xf;
        out[i][3] = static_cast<u8>(a4 * 17);
    }
}

void decode_bc3_block(const u8* block, std::array<std::array<u8, 4>, 16>& out)
{
    bc_color_block(block + 8, true, out);
    const u32 a0 = block[0], a1 = block[1];
    std::array<u32, 8> alpha{a0, a1};
    if (a0 > a1) {
        for (u32 i = 2; i < 8; i++) alpha[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    } else {
        for (u32 i = 2; i < 6; i++) alpha[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        alpha[6] = 0;
        alpha[7] = 255;
    }
    u64 bits = 0;
    for (u32 i = 0; i < 6; i++) bits |= u64{block[2 + i]} << (8 * i);
    for (u32 i = 0; i < 16; i++) out[i][3] = static_cast<u8>(alpha[(bits >> (3 * i)) & 7]);
}

bool decode_level_cpu(const TextureLayout& layout, const TextureLevel& level, const u8* src,
    std::vector<std::array<float, 4>>& out)
{
    const TextureFormatInfo& fmt = *layout.format;
    out.assign(static_cast<usize>(level.width) * level.height * level.depth, {0.f, 0.f, 0.f, 0.f});
    const bool morton = layout.swizzled && fmt.block_texels == 1;
    const u32 lw = ceil_log2(level.swizzle_width), lh = ceil_log2(level.swizzle_height);
    const u32 ld = ceil_log2(level.swizzle_depth);
    if (level.depth > 1 && fmt.block_texels != 1) return false;

    for (u32 z = 0; z < level.depth; z++)
    for (u32 y = 0; y < level.height; y++) {
        for (u32 x = 0; x < level.width; x++) {
            std::array<float, 4>& o = out[(static_cast<usize>(z) * level.height + y) * level.width + x];
            if (fmt.block_texels == 4) {
                const u8* block = src + level.offset + (y / 4) * level.pitch + (x / 4) * fmt.block_bytes;
                std::array<std::array<u8, 4>, 16> texels{};
                switch (fmt.decode) {
                case TexelDecode::DXT1: decode_bc1_block(block, texels); break;
                case TexelDecode::DXT23: decode_bc2_block(block, texels); break;
                case TexelDecode::DXT45: decode_bc3_block(block, texels); break;
                default: return false;
                }
                const auto& t = texels[(y % 4) * 4 + (x % 4)];
                o = rgba8(t[0], t[1], t[2], t[3]);
                continue;
            }
            const u32 ex = x + level.border, ey = y + level.border;
            const u32 element = morton ? morton_index_3d(ex, ey, z, lw, lh, ld) * fmt.block_bytes
                                       : z * level.slice_bytes() + ey * level.pitch + ex * fmt.block_bytes;
            const u8* p = src + level.offset + element;
            switch (fmt.decode) {
            case TexelDecode::ARGB8: o = rgba8(p[1], p[2], p[3], p[0]); break;
            case TexelDecode::DRGB8: o = rgba8(p[1], p[2], p[3], 255); break;
            case TexelDecode::G8B8: o = rgba8(p[1], p[0], 0, 255); break;
            case TexelDecode::R5G6B5: {
                const u32 v = ps_ld16(p);
                o = rgba8(expand5(v >> 11), expand6((v >> 5) & 63), expand5(v & 31), 255);
                break;
            }
            case TexelDecode::A1R5G5B5: {
                const u32 v = ps_ld16(p);
                o = rgba8(expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31), (v >> 15) ? 255 : 0);
                break;
            }
            case TexelDecode::D1R5G5B5: {
                const u32 v = ps_ld16(p);
                o = rgba8(expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31), 255);
                break;
            }
            case TexelDecode::A4R4G4B4: {
                const u32 v = ps_ld16(p);
                o = rgba8(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17);
                break;
            }
            case TexelDecode::R5G5B5A1: {
                const u32 v = ps_ld16(p);
                o = rgba8(expand5(v >> 11), expand5((v >> 6) & 31), expand5((v >> 1) & 31), (v & 1) ? 255 : 0);
                break;
            }
            case TexelDecode::R6G5B5: {
                const u32 v = ps_ld16(p);
                o = rgba8(expand6(v >> 10), expand5((v >> 5) & 31), expand5(v & 31), 255);
                break;
            }
            case TexelDecode::B8: o = rgba8(p[0], p[0], p[0], 255); break;
            case TexelDecode::X32F: o = {ps_ldf32(p), 0.f, 0.f, 1.f}; break;
            case TexelDecode::W16Z16Y16X16F:
                o = {half_to_float(ps_ld16(p)), half_to_float(ps_ld16(p + 2)), half_to_float(ps_ld16(p + 4)),
                    half_to_float(ps_ld16(p + 6))};
                break;
            case TexelDecode::W32Z32Y32X32F: o = {ps_ldf32(p), ps_ldf32(p + 4), ps_ldf32(p + 8), ps_ldf32(p + 12)}; break;
            case TexelDecode::Y16X16F: o = {half_to_float(ps_ld16(p)), half_to_float(ps_ld16(p + 2)), 0.f, 1.f}; break;
            case TexelDecode::X16:
            case TexelDecode::D16: o = {ps_ld16(p) / 65535.f, 0.f, 0.f, 1.f}; break;
            case TexelDecode::D24: o = {static_cast<float>(ps_ld32(p) >> 8) / 16777215.f, 0.f, 0.f, 1.f}; break;
            case TexelDecode::HILO8: o = {unorm8(p[1]), unorm8(p[0]), 0.f, 1.f}; break;
            case TexelDecode::HILOS8: {
                auto snorm = [](u8 v) { return std::max(static_cast<float>(static_cast<s8>(v)) / 127.f, -1.f); };
                o = {snorm(p[1]), snorm(p[0]), 0.f, 1.f};
                break;
            }
            case TexelDecode::D24F: {
                const u32 bits = (ps_ld32(p) >> 8) << 7;
                float f;
                std::memcpy(&f, &bits, 4);
                o = {f, 0.f, 0.f, 1.f};
                break;
            }
            case TexelDecode::Y16X16: o = {ps_ld16(p) / 65535.f, ps_ld16(p + 2) / 65535.f, 0.f, 1.f}; break;
            default: return false;
            }
        }
    }
    return true;
}

} // namespace pspocket::rsx
