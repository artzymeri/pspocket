// The debug PNG writer: a known 2x2 image byte for byte (chunk layout, stored deflate block, Adler-32, CRC-32), and a
// large image split into several stored blocks.

#include <catch_amalgamated.hpp>

#include <vector>

#include "pspocket/core/png.hpp"

using namespace pspocket;

namespace {

u32 be32(const std::vector<u8>& v, usize at)
{
    return u32{v[at]} << 24 | u32{v[at + 1]} << 16 | u32{v[at + 2]} << 8 | v[at + 3];
}

} // namespace

TEST_CASE("PNG: a 2x2 BGRA image as an RGB PNG with one stored block", "[core][png]")
{
    // Red, green / blue, white, as B, G, R, X bytes.
    const u8 bgra[] = {0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0};
    const std::vector<u8> png = encode_png_rgb(bgra, 2, 2, 8, true);
    const std::vector<u8> expected = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
        0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 2, 0, 0, 0, 2, 8, 2, 0, 0, 0, 0xFD, 0xD4, 0x9A, 0x73,
        0, 0, 0, 25, 'I', 'D', 'A', 'T',
        0x78, 0x01, 0x01, 14, 0, 0xF1, 0xFF,             // zlib header, the final stored block of 14 bytes
        0, 255, 0, 0, 0, 255, 0,                         // filter 0, red, green
        0, 0, 0, 255, 255, 255, 255,                     // filter 0, blue, white
        0x1F, 0xEE, 0x05, 0xFB,                          // Adler-32
        0xDE, 0xDD, 0xEC, 0x2B,                          // CRC-32 of IDAT (values from Python's zlib)
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    CHECK(png == expected);
}

TEST_CASE("PNG: rows beyond one stored block", "[core][png]")
{
    constexpr u32 w = 300, h = 100; // 90,100 bytes of image data: two stored blocks
    std::vector<u8> rgba(w * h * 4);
    for (usize i = 0; i < rgba.size(); i++) rgba[i] = static_cast<u8>(i * 7);
    const std::vector<u8> png = encode_png_rgb(rgba.data(), w, h, w * 4, false);
    REQUIRE(be32(png, 8) == 13);
    CHECK(be32(png, 16) == w);
    CHECK(be32(png, 20) == h);
    const u32 idat = be32(png, 33);
    const usize data = 41; // after the IDAT length and type
    CHECK(png[data] == 0x78);
    CHECK(png[data + 2] == 0); // a first block that is not the last
    CHECK((png[data + 3] | png[data + 4] << 8) == 65535);
    const usize second = data + 2 + 5 + 65535;
    CHECK(png[second] == 1); // the last block
    const usize rest = w * h * 3 + h - 65535;
    CHECK((png[second + 1] | png[second + 2] << 8) == rest);
    CHECK(idat == 2 + 5 + 65535 + 5 + rest + 4);
    CHECK(png.size() == 33 + 12 + idat + 12);
}
