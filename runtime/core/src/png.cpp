// PNG with stored deflate blocks (RFC 1950/1951, PNG spec chunks and CRC-32).

#include "pspocket/core/png.hpp"

#include <array>
#include <cstdio>

#include "pspocket/core/log.hpp"

namespace pspocket {
namespace {

const std::array<u32, 256>& crc_table()
{
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256; n++) {
            u32 c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

void put32(std::vector<u8>& out, u32 v)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<u8>(v >> shift));
}

// A chunk: length, type, data, CRC-32 of type and data.
void chunk(std::vector<u8>& out, const char (&type)[5], const std::vector<u8>& data)
{
    put32(out, static_cast<u32>(data.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    u32 crc = 0xFFFFFFFFu;
    for (usize i = start; i < out.size(); i++) crc = crc_table()[(crc ^ out[i]) & 0xFF] ^ (crc >> 8);
    put32(out, crc ^ 0xFFFFFFFFu);
}

} // namespace

std::vector<u8> encode_png_rgb(const u8* pixels, u32 width, u32 height, u32 stride, bool bgra)
{
    // The image data: each row is a filter byte (0: none) and its RGB triples.
    std::vector<u8> raw;
    raw.reserve(usize{height} * (1 + usize{width} * 3));
    for (u32 y = 0; y < height; y++) {
        raw.push_back(0);
        const u8* row = pixels + usize{y} * stride;
        for (u32 x = 0; x < width; x++) {
            const u8* p = row + usize{x} * 4;
            raw.push_back(bgra ? p[2] : p[0]);
            raw.push_back(p[1]);
            raw.push_back(bgra ? p[0] : p[2]);
        }
    }
    // zlib: header 0x78 0x01, stored blocks of at most 65535 bytes, Adler-32.
    std::vector<u8> z = {0x78, 0x01};
    usize at = 0;
    do {
        const usize n = std::min<usize>(raw.size() - at, 65535);
        z.push_back(at + n == raw.size() ? 1 : 0); // BFINAL, BTYPE 00
        z.push_back(static_cast<u8>(n));
        z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n));
        z.push_back(static_cast<u8>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < raw.size());
    u32 a = 1, b = 0;
    for (const u8 v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8 bits, RGB, deflate, no filter method, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

bool write_png_rgb(const std::string& path, const u8* pixels, u32 width, u32 height, u32 stride, bool bgra)
{
    const std::vector<u8> png = encode_png_rgb(pixels, width, height, stride, bgra);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const bool ok = f && std::fwrite(png.data(), 1, png.size(), f) == png.size();
    if (f && std::fclose(f) != 0) return false;
    if (!ok) PS_LOG_ERROR("png", "cannot write {}", path);
    return ok;
}

} // namespace pspocket
