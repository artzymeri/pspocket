#pragma once

// PNG files built in code for the image decoder tests (no game data): any colour type and bit depth, optionally Adam7
// interlaced, each row with a chosen filter, the image data in uncompressed deflate blocks (zlib "stored"), with the
// ancillary chunks a test asks for. Only what the PNG specification requires: signature, IHDR, PLTE for palettes,
// IDAT, IEND, each chunk with its CRC-32 and the zlib stream with its Adler-32.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pspocket::test::png {

inline std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0)
{
    crc = ~crc;
    for (std::size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

inline void put32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}

inline void chunk(std::vector<std::uint8_t>& out, std::string_view type, const std::vector<std::uint8_t>& data)
{
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type.begin(), type.end());
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

// A zlib stream of stored deflate blocks (at most 65535 bytes each) holding `raw`.
inline std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& raw)
{
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t n = std::min<std::size_t>(raw.size() - at, 65535);
        z.push_back(at + n == raw.size() ? 1 : 0); // BFINAL, BTYPE 00
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < raw.size());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);
    return z;
}

struct Image {
    std::uint32_t width = 0, height = 0;
    std::uint8_t color_type = 6; // 0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA
    std::uint8_t bit_depth = 8;
    bool interlaced = false;
    // Samples, row by row, each sample in a 16-bit slot (palette indices, gray, R G B, alpha as the colour type has).
    std::vector<std::uint16_t> samples;
    std::vector<std::array<std::uint8_t, 3>> palette;
    std::vector<std::uint8_t> trns;                                // tRNS chunk contents, as written
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> extra; // more chunks before IDAT
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> after_plte; // chunks between PLTE and IDAT
    std::vector<std::pair<std::string, std::string>> text;        // tEXt keyword, text (after IDAT)
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> after; // more chunks after IDAT (after the text)
    std::uint8_t filter_seed = 0; // row r gets filter (r + filter_seed) % 5

    std::uint32_t channels() const
    {
        switch (color_type) {
        case 2: return 3;
        case 4: return 2;
        case 6: return 4;
        default: return 1;
        }
    }
};

inline std::uint8_t paeth(int a, int b, int c)
{
    const int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// The filtered scanlines of a (sub)image of `w` x `h` pixels whose pixel (x, y) is image pixel at(x, y).
template <class At>
void scanlines(const Image& im, std::uint32_t w, std::uint32_t h, At at, std::vector<std::uint8_t>& raw, std::uint32_t& row)
{
    if (!w || !h) return;
    const std::uint32_t ch = im.channels(), bits = im.bit_depth * ch;
    const std::size_t line = (std::size_t{w} * bits + 7) / 8, bpp = std::max<std::size_t>(1, bits / 8);
    std::vector<std::uint8_t> prev(line, 0), cur(line);
    for (std::uint32_t y = 0; y < h; y++, row++) {
        std::fill(cur.begin(), cur.end(), 0);
        std::size_t bit = 0;
        for (std::uint32_t x = 0; x < w; x++) {
            const std::size_t px = at(x, y);
            for (std::uint32_t c = 0; c < ch; c++) {
                const std::uint16_t s = im.samples[px * ch + c];
                if (im.bit_depth == 16) {
                    cur[bit / 8] = static_cast<std::uint8_t>(s >> 8);
                    cur[bit / 8 + 1] = static_cast<std::uint8_t>(s);
                } else if (im.bit_depth == 8) {
                    cur[bit / 8] = static_cast<std::uint8_t>(s);
                } else {
                    const std::size_t shift = 8 - im.bit_depth - bit % 8;
                    cur[bit / 8] |= static_cast<std::uint8_t>((s & ((1u << im.bit_depth) - 1)) << shift);
                }
                bit += im.bit_depth;
            }
        }
        const std::uint8_t f = static_cast<std::uint8_t>((row + im.filter_seed) % 5);
        raw.push_back(f);
        for (std::size_t i = 0; i < line; i++) {
            const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int pred = 0;
            switch (f) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: break;
            }
            raw.push_back(static_cast<std::uint8_t>(cur[i] - pred));
        }
        prev = cur;
    }
}

inline std::vector<std::uint8_t> encode(const Image& im)
{
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, im.width);
    put32(ihdr, im.height);
    ihdr.push_back(im.bit_depth);
    ihdr.push_back(im.color_type);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(im.interlaced ? 1 : 0);
    chunk(out, "IHDR", ihdr);
    for (const auto& [type, data] : im.extra) chunk(out, type, data);
    if (!im.palette.empty()) {
        std::vector<std::uint8_t> plte;
        for (const auto& c : im.palette) plte.insert(plte.end(), c.begin(), c.end());
        chunk(out, "PLTE", plte);
    }
    if (!im.trns.empty()) chunk(out, "tRNS", im.trns);
    for (const auto& [type, data] : im.after_plte) chunk(out, type, data);
    std::vector<std::uint8_t> raw;
    std::uint32_t row = 0;
    if (!im.interlaced) {
        scanlines(im, im.width, im.height, [&](std::uint32_t x, std::uint32_t y) { return std::size_t{y} * im.width + x; },
            raw, row);
    } else {
        constexpr std::uint32_t xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
        constexpr std::uint32_t dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; p++) {
            const std::uint32_t w = im.width > xs[p] ? (im.width - xs[p] + dx[p] - 1) / dx[p] : 0;
            const std::uint32_t h = im.height > ys[p] ? (im.height - ys[p] + dy[p] - 1) / dy[p] : 0;
            scanlines(im, w, h, [&](std::uint32_t x, std::uint32_t y) {
                return std::size_t{ys[p] + y * dy[p]} * im.width + xs[p] + x * dx[p];
            }, raw, row);
        }
    }
    chunk(out, "IDAT", zlib_stored(raw));
    for (const auto& [key, text] : im.text) {
        std::vector<std::uint8_t> t(key.begin(), key.end());
        t.push_back(0);
        t.insert(t.end(), text.begin(), text.end());
        chunk(out, "tEXt", t);
    }
    for (const auto& [type, data] : im.after) chunk(out, type, data);
    chunk(out, "IEND", {});
    return out;
}

// A test image: every sample a different pattern of x, y and the channel, within the bit depth.
inline Image pattern(std::uint32_t w, std::uint32_t h, std::uint8_t color_type, std::uint8_t bit_depth, std::uint32_t seed)
{
    Image im;
    im.width = w;
    im.height = h;
    im.color_type = color_type;
    im.bit_depth = bit_depth;
    const std::uint32_t ch = im.channels(), max = (1u << bit_depth) - 1;
    for (std::uint32_t y = 0; y < h; y++)
        for (std::uint32_t x = 0; x < w; x++)
            for (std::uint32_t c = 0; c < ch; c++) {
                const std::uint32_t v = (x * 37 + y * 101 + c * 59 + seed * 13 + (x * y) % 7) * 2654435761u;
                im.samples.push_back(static_cast<std::uint16_t>(color_type == 3 ? (v >> 7) % (std::min(max, 255u) + 1)
                                                                                : (v >> 7) & max));
            }
    if (color_type == 3)
        for (std::uint32_t i = 0; i <= std::min(max, 255u); i++)
            im.palette.push_back({static_cast<std::uint8_t>(i * 7 + seed), static_cast<std::uint8_t>(255 - i * 3),
                static_cast<std::uint8_t>(i * i)});
    return im;
}

} // namespace pspocket::test::png
