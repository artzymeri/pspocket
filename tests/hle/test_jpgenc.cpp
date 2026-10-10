// pspocket's baseline JPEG encoder (D-615): pictures built here in every sampling and gray, at sizes off the MCU,
// encoded with both DCTs and with restart markers, then decoded by pspocket's own JPEG decoder (jpeg_decoder.cpp,
// which decodes as Sony's libjpgdec does) and checked within a stated PSNR of the input, with the streams' markers and
// tables. No game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_encoder.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace jpeg = pspocket::hle::jpeg;

namespace {

// ---- pictures built here

// A smooth picture (so that chroma subsampling loses little) with some detail: R, G, B rows of width x 3 bytes.
struct Source {
    u32 w = 0, h = 0;
    std::vector<u8> rgb;
};

Source smooth(u32 w, u32 h, u32 seed)
{
    Source s{w, h, std::vector<u8>(usize{w} * h * 3)};
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u8* p = s.rgb.data() + (usize{y} * w + x) * 3;
            const double fx = x + seed, fy = y;
            p[0] = static_cast<u8>(std::clamp(128 + 90 * std::sin(fx * 0.13 + fy * 0.05), 0.0, 255.0));
            p[1] = static_cast<u8>(std::clamp(110 + 70 * std::cos(fy * 0.11 - fx * 0.02), 0.0, 255.0));
            p[2] = static_cast<u8>(std::clamp(40 + 1.5 * (fx + fy) + 20 * std::sin(fx * fy * 0.004), 0.0, 255.0));
        }
    return s;
}

// The JPEG components the library makes of the source (Y, Cb, Cr at full size; gray: Y).
jpeg::Planes planes_of(const Source& s, bool gray)
{
    jpeg::Planes p;
    p.width = s.w;
    p.height = s.h;
    p.components = gray ? 1 : 3;
    const usize n = usize{s.w} * s.h;
    p.y.resize(n);
    p.cb.resize(gray ? 0 : n);
    p.cr.resize(gray ? 0 : n);
    for (usize i = 0; i < n; i++) {
        const u8* c = s.rgb.data() + 3 * i;
        if (gray) p.y[i] = jpeg::rgb_to_y(c[0], c[1], c[2]);
        else jpeg::rgb_to_ycbcr(c[0], c[1], c[2], p.y[i], p.cb[i], p.cr[i]);
    }
    return p;
}

double psnr(const std::vector<u8>& a, const std::vector<u8>& b)
{
    REQUIRE(a.size() == b.size());
    double se = 0;
    for (usize i = 0; i < a.size(); i++) se += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
    return se == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / (se / static_cast<double>(a.size())));
}

std::vector<u8> gray_of(const Source& s)
{
    std::vector<u8> g(usize{s.w} * s.h);
    for (usize i = 0; i < g.size(); i++) g[i] = jpeg::rgb_to_y(s.rgb[3 * i], s.rgb[3 * i + 1], s.rgb[3 * i + 2]);
    return g;
}

// ---- reading a JPEG back with pspocket's decoder

struct Decoded {
    bool ok = false;
    jpeg::Header header;
    std::vector<std::array<int, 3>> sampling; // each component's id, h, v
    std::array<u16, 64> q0{}, q1{};
    int warnings = 0;
    std::vector<u8> pixels; // `space`: RGB or gray, width x height
};

Decoded read_jpeg(const u8* data, usize size, jpeg::ColorSpace space)
{
    Decoded d;
    jpeg::Decoder dec;
    std::string why;
    if (!dec.read_header(data, size, why) || !dec.decode(why)) {
        INFO(why);
        return d;
    }
    d.header = dec.header();
    for (const jpeg::ComponentInfo& c : dec.components()) d.sampling.push_back({c.id, c.h, c.v});
    std::memcpy(d.q0.data(), dec.quant(0), sizeof(d.q0));
    std::memcpy(d.q1.data(), dec.quant(1), sizeof(d.q1));
    d.warnings = dec.warnings();
    jpeg::Decoder::Output o;
    if (!dec.output(1, false, space, o, why)) return d;
    d.pixels = std::move(o.pixels);
    d.ok = !dec.ran_out();
    return d;
}

std::array<u16, 64> table(bool luma, int quality)
{
    const std::array<u8, 64> t = jpeg::quant_table(luma, quality);
    std::array<u16, 64> out{};
    for (usize i = 0; i < 64; i++) out[i] = t[i];
    return out;
}

// The JFIF stream's markers in order (each 0xFF and its code), to EOI, the scan's data skipped (restart markers
// listed).
std::vector<u8> markers(const std::vector<u8>& d)
{
    std::vector<u8> out;
    usize at = 0;
    bool scan = false;
    while (at + 1 < d.size()) {
        if (d[at] != 0xFF) {
            at++;
            continue;
        }
        const u8 m = d[at + 1];
        if (scan && (m == 0 || m == 0xFF)) {
            at += m ? 1 : 2;
            continue;
        }
        out.push_back(m);
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) {
            at += 2;
            continue;
        }
        if (m == 0xD9) break;
        at += 2 + (usize{d[at + 2]} << 8 | d[at + 3]);
        scan = scan || m == 0xDA;
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------------------------------------- the encoder

TEST_CASE("pspocket's JPEG encoder: annex K's tables scaled, T.871's colours", "[hle][jpgenc]")
{
    // At 50 the tables are annex K's (K.1, K.2); at 100 all ones; at 1 kept within 8 bits.
    const std::array<u8, 64> k1 = jpeg::quant_table(true, 50), k2 = jpeg::quant_table(false, 50);
    CHECK(k1[0] == 16);
    CHECK(k1[1] == 11);
    CHECK(k1[8] == 12);
    CHECK(k1[63] == 99);
    CHECK(k2[0] == 17);
    CHECK(k2[3] == 47);
    CHECK(k2[63] == 99);
    for (const u8 v : jpeg::quant_table(true, 100)) CHECK(v == 1);
    for (const u8 v : jpeg::quant_table(false, 1)) CHECK(v == 255);
    CHECK(jpeg::quant_table(true, 75)[0] == 8);  // (16 x 50 + 50) / 100
    CHECK(jpeg::quant_table(true, 10)[0] == 80); // 16 x 500 / 100
    CHECK(jpeg::quant_table(true, 0) == jpeg::quant_table(true, 1));
    CHECK(jpeg::quant_table(true, 200) == jpeg::quant_table(true, 100));

    // T.871 section 7, rounded and kept within 0-255.
    const auto ycc = [](u8 r, u8 g, u8 b) {
        std::array<u8, 3> o{};
        jpeg::rgb_to_ycbcr(r, g, b, o[0], o[1], o[2]);
        CHECK(jpeg::rgb_to_y(r, g, b) == o[0]);
        return o;
    };
    CHECK(ycc(255, 255, 255) == std::array<u8, 3>{255, 128, 128});
    CHECK(ycc(0, 0, 0) == std::array<u8, 3>{0, 128, 128});
    CHECK(ycc(255, 0, 0) == std::array<u8, 3>{76, 85, 255});
    CHECK(ycc(0, 255, 0) == std::array<u8, 3>{150, 44, 21});
    CHECK(ycc(0, 0, 255) == std::array<u8, 3>{29, 255, 107});
    CHECK(ycc(128, 128, 128) == std::array<u8, 3>{128, 128, 128});
}

TEST_CASE("pspocket's JPEG encoder: every sampling and gray, sizes off the MCU, both DCTs, read back by pspocket's "
          "decoder",
    "[hle][jpgenc]")
{
    // The luma's sampling factors of 4:4:4, 4:2:2, 4:2:0 and 4:1:1 (the chroma 1x1).
    const int hv[4][2] = {{1, 1}, {2, 1}, {2, 2}, {4, 1}};
    for (const auto& [w, h] : {std::pair<u32, u32>{1, 1}, {7, 5}, {8, 8}, {16, 16}, {33, 17}, {61, 37}, {100, 3}})
        for (int sampling = -1; sampling < 4; sampling++)
            for (const bool fast : {false, true}) {
                const bool gray = sampling < 0;
                INFO(std::format("{}x{}, {}, {} DCT", w, h, gray ? "gray" : std::format("sampling {}", sampling),
                    fast ? "fast" : "quality"));
                const Source src = smooth(w, h, 3);
                jpeg::EncodeSettings s;
                s.quality = 90;
                s.fast = fast;
                if (!gray) s.sampling = static_cast<jpeg::Sampling>(sampling);
                const std::vector<u8> file = jpeg::encode(planes_of(src, gray), s);
                REQUIRE(file.size() > (gray ? 300u : 600u)); // the tables: one of each kind for gray, two for colour
                // SOI, JFIF, DQT, SOF0, DHT, SOS, EOI.
                CHECK(markers(file) == std::vector<u8>{0xD8, 0xE0, 0xDB, 0xC0, 0xC4, 0xDA, 0xD9});
                CHECK(std::memcmp(file.data() + 6, "JFIF\0\1\1", 7) == 0);
                const Decoded d = read_jpeg(file.data(), file.size(), gray ? jpeg::ColorSpace::Gray : jpeg::ColorSpace::RGB);
                REQUIRE(d.ok);
                CHECK(d.warnings == 0);
                CHECK(d.header.width == w);
                CHECK(d.header.height == h);
                CHECK_FALSE(d.header.progressive);
                CHECK(d.header.jfif);
                CHECK(d.header.color_space == (gray ? jpeg::ColorSpace::Gray : jpeg::ColorSpace::YCbCr));
                REQUIRE(d.sampling.size() == (gray ? 1u : 3u));
                CHECK(d.sampling[0] == std::array<int, 3>{1, gray ? 1 : hv[sampling][0], gray ? 1 : hv[sampling][1]});
                if (!gray) {
                    CHECK(d.sampling[1] == std::array<int, 3>{2, 1, 1});
                    CHECK(d.sampling[2] == std::array<int, 3>{3, 1, 1});
                    CHECK(d.q1 == table(false, 90));
                }
                CHECK(d.q0 == table(true, 90)); // in natural order: the DQT's zigzag order read back
                // Within a PSNR of the picture (the luma alone for gray), about 3 dB under the worst measured over
                // these sizes: gray 52, 4:4:4 45.5, 4:2:2 42.3, 4:2:0 40.0, 4:1:1 34.2 (its chroma a quarter as wide).
                const double p = gray ? psnr(d.pixels, gray_of(src)) : psnr(d.pixels, src.rgb);
                INFO(std::format("PSNR {:.2f} dB", p));
                constexpr double kAtLeast[4] = {42.0, 39.0, 37.0, 31.0};
                CHECK(p > (gray ? 48.0 : kAtLeast[sampling]));
            }
}

TEST_CASE("pspocket's JPEG encoder: restart markers every n MCUs, the same image; what it refuses", "[hle][jpgenc]")
{
    const Source src = smooth(61, 37, 7);
    const jpeg::Planes planes = planes_of(src, false);
    jpeg::EncodeSettings s;
    s.quality = 80;
    const std::vector<u8> plain = jpeg::encode(planes, s);
    const Decoded want = read_jpeg(plain.data(), plain.size(), jpeg::ColorSpace::RGB);
    REQUIRE(want.ok);
    // 4:2:0: 16x16 MCUs, 4 x 3 of them.
    for (const u32 interval : {1u, 2u, 5u, 11u, 12u, 40u}) {
        INFO("restart interval " << interval);
        s.restart_interval = interval;
        const std::vector<u8> file = jpeg::encode(planes, s);
        std::vector<u8> m = markers(file);
        const u32 restarts = (12 + interval - 1) / interval - 1;
        REQUIRE(m.size() == 8 + restarts); // SOI APP0 DQT SOF0 DHT DRI SOS, the restarts, EOI
        CHECK(m[5] == 0xDD);
        for (u32 i = 0; i < restarts; i++) CHECK(m[7 + i] == 0xD0 + i % 8); // RST0 to RST7, again
        const Decoded d = read_jpeg(file.data(), file.size(), jpeg::ColorSpace::RGB);
        REQUIRE(d.ok);
        CHECK(d.warnings == 0);
        CHECK(d.pixels == want.pixels); // the same coefficients, the DC predictions reset at each marker
    }
    // A gray picture: one block an MCU.
    s.restart_interval = 3;
    const std::vector<u8> gray = jpeg::encode(planes_of(src, true), s);
    CHECK(markers(gray).size() == 8 + (8 * 5 + 2) / 3 - 1); // 8 x 5 blocks
    s.restart_interval = 0;
    const Decoded g0 = read_jpeg(gray.data(), gray.size(), jpeg::ColorSpace::Gray);
    const std::vector<u8> gray_plain = jpeg::encode(planes_of(src, true), s);
    const Decoded g1 = read_jpeg(gray_plain.data(), gray_plain.size(), jpeg::ColorSpace::Gray);
    REQUIRE(g0.ok);
    CHECK(g0.pixels == g1.pixels);

    // What cannot be a JPEG.
    jpeg::Planes p = planes;
    p.width = 0;
    CHECK(jpeg::encode(p, s).empty());
    p = planes;
    p.y.resize(10);
    CHECK(jpeg::encode(p, s).empty());
    p = planes;
    p.width = 0x10000;
    CHECK(jpeg::encode(p, s).empty());
    p = planes;
    p.components = 2;
    CHECK(jpeg::encode(p, s).empty());
    s.restart_interval = 0x10000;
    CHECK(jpeg::encode(planes, s).empty());
}
