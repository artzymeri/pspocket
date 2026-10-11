// cellJpgEnc (D-616) and pspocket's baseline JPEG encoder (D-615): pictures built here in every colour space and
// sampling, encoded, then decoded by pspocket's own JPEG decoder (jpeg_decoder.cpp, which decodes as Sony's libjpgdec
// does) and checked within a stated PSNR of the input; the streams' headers, tables and restart markers; EncodePicture2's
// planes; the result queue and its waits; the stream size limit mode; buffer and file output; every argument check.
// No Sony module to compare with (firmware-first, D-563). No game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_encoder.hpp"
#include "pspocket/recomp/nid.hpp"

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

// ------------------------------------------------------------------------------------------------- cellJpgEnc

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kPic = kData + 0x200, kParam = kData + 0x240, kOutParam = kData + 0x280, kInfo = kData + 0x2C0,
              kNum = kData + 0x300, kName = kData + 0x800, kPixels = kData + 0x1000, kStream = kData + 0x80000,
              kWork = kData + 0x100000;
constexpr u32 kStreamMax = 0x80000, kWorkSize = 0x100000;
static_assert(kPixels + 0x7F000 <= kStream && kStream + kStreamMax <= kWork && kWork + kWorkSize <= kStacks);
constexpr u32 kNowhere = 0xF0000000; // not guest memory
const u64 kArg = r3_of_code(0x80611191), kBusy = r3_of_code(0x80611193), kEmpty = r3_of_code(0x80611194),
          kReset = r3_of_code(0x80611195);
constexpr s32 kOverflow = static_cast<s32>(0x806111A3), kFileOpen = static_cast<s32>(0x806111A4);
enum : u32 { GRAY = 1, RGB = 2, YCBCR = 3, RGBA = 10, ARGB = 20 };
enum : u32 { S444 = 0, S422 = 1, S420 = 2, S411 = 3, FULL = 4 };

u32 pixel_bytes(u32 space) { return space == GRAY ? 1 : space == RGB || space == YCBCR ? 3 : 4; }

// The source as a picture of `space` at `at` (kPixels), rows `pitch` bytes apart (the padding 0xA5); RGBA's and ARGB's
// alpha some pattern, not encoded.
void put_picture(u8* b, const Source& s, u32 space, u32 pitch, u32 at = kPixels)
{
    std::memset(b + at, 0xA5, usize{pitch} * s.h);
    for (u32 y = 0; y < s.h; y++)
        for (u32 x = 0; x < s.w; x++) {
            const u8* c = s.rgb.data() + (usize{y} * s.w + x) * 3;
            u8* o = b + at + usize{y} * pitch + usize{x} * pixel_bytes(space);
            const u8 alpha = static_cast<u8>(x * 31 + y * 17);
            switch (space) {
            case GRAY: o[0] = jpeg::rgb_to_y(c[0], c[1], c[2]); break;
            case RGB: std::memcpy(o, c, 3); break;
            case YCBCR: jpeg::rgb_to_ycbcr(c[0], c[1], c[2], o[0], o[1], o[2]); break;
            case RGBA:
                std::memcpy(o, c, 3);
                o[3] = alpha;
                break;
            default:
                o[0] = alpha;
                std::memcpy(o + 1, c, 3);
                break;
            }
        }
}

// ---- the library's calls

void write_config(u8* b, u32 max_w, u32 max_h, bool spu = false, u32 add = 0)
{
    std::memset(b + kCfg, 0, 24);
    ps_write32(b, kCfg, max_w);
    ps_write32(b, kCfg + 4, max_h);
    ps_write8(b, kCfg + 8, spu ? 1 : 0);
    ps_write32(b, kCfg + 12, add);
}

u32 needed(u32 max_w, bool spu = false, u32 add = 0) { return add + (spu ? 0x40000 : 0x10000) + max_w * 0x40; }

u32 open(GuestThread& g, u32 max_w = 640, u32 max_h = 480)
{
    u8* b = g.f.base();
    write_config(b, max_w, max_h);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(max_w));
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    REQUIRE(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == 0);
    return ps_read32(b, kHandle);
}

// CellJpgEncPicture (or CellJpgEncPicture2 with a sampling format).
struct Pic {
    u32 width = 0, height = 0, pitch = 0, space = RGB, sampling = FULL, addr = kPixels;
    u64 user = 0;
};

// CellJpgEncEncodeParam.
struct Param {
    u32 space = YCBCR, sampling = S420, method = 0, mode = 0, quality = 90;
    bool spu = false;
};

u64 encode(GuestThread& g, u32 handle, const Pic& p, const Param& e, u32 limit = kStreamMax, u32 location = 1,
    u32 name = 0, bool two = false)
{
    u8* b = g.f.base();
    std::memset(b + kPic, 0, 32);
    ps_write32(b, kPic, p.width);
    ps_write32(b, kPic + 4, p.height);
    ps_write32(b, kPic + 8, p.pitch);
    ps_write32(b, kPic + 12, p.space);
    if (two) ps_write32(b, kPic + 16, p.sampling);
    ps_write32(b, kPic + (two ? 20 : 16), p.addr);
    ps_write64(b, kPic + 24, p.user);
    std::memset(b + kParam, 0, 24);
    ps_write8(b, kParam, e.spu ? 1 : 0);
    ps_write32(b, kParam + 4, e.space);
    ps_write32(b, kParam + 8, e.sampling);
    ps_write32(b, kParam + 12, e.method);
    ps_write32(b, kParam + 16, e.mode);
    ps_write32(b, kParam + 20, e.quality);
    ps_write32(b, kOutParam, location);
    ps_write32(b, kOutParam + 4, name);
    ps_write32(b, kOutParam + 8, location == 1 ? kStream : 0);
    ps_write32(b, kOutParam + 12, limit);
    return g.call(two ? "cellJpgEncEncodePicture2" : "cellJpgEncEncodePicture", handle, kPic, kParam, kOutParam);
}

struct Info {
    s32 state;
    u32 location, name, addr, limit, size, lines;
    u64 user;
};

Info stream_info(GuestThread& g, u32 handle)
{
    u8* b = g.f.base();
    REQUIRE(g.call("cellJpgEncGetStreamInfo", handle, kInfo) == 0);
    return {static_cast<s32>(ps_read32(b, kInfo)), ps_read32(b, kInfo + 4), ps_read32(b, kInfo + 8),
        ps_read32(b, kInfo + 12), ps_read32(b, kInfo + 16), ps_read32(b, kInfo + 20), ps_read32(b, kInfo + 24),
        ps_read64(b, kInfo + 32)};
}

// Encodes to the buffer, takes the one result, gives the stream.
std::vector<u8> round_trip(GuestThread& g, u32 handle, const Pic& p, const Param& e, bool two = false)
{
    u8* b = g.f.base();
    REQUIRE(encode(g, handle, p, e, kStreamMax, 1, 0, two) == 0);
    REQUIRE(g.call("cellJpgEncWaitForOutput", handle, kNum, 1) == 0);
    REQUIRE(ps_read32(b, kNum) == 1);
    const Info i = stream_info(g, handle);
    REQUIRE(i.state == 0);
    CHECK(i.lines == p.height);
    CHECK(i.location == 1);
    CHECK(i.addr == kStream);
    REQUIRE(i.size > 0);
    REQUIRE(i.size <= kStreamMax);
    return std::vector<u8>(b + kStream, b + kStream + i.size);
}

} // namespace

TEST_CASE("cellJpgEnc: the ten functions, firmware first", "[hle][jpgenc]")
{
    for (const std::string_view name : {"cellJpgEncQueryAttr", "cellJpgEncOpen", "cellJpgEncOpenEx", "cellJpgEncClose",
             "cellJpgEncWaitForInput", "cellJpgEncEncodePicture", "cellJpgEncEncodePicture2", "cellJpgEncWaitForOutput",
             "cellJpgEncGetStreamInfo", "cellJpgEncReset"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellJpgEnc", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
        CHECK_FALSE(hle::registry().is_unverified("cellJpgEnc", recomp::nid_of(name)));
    }
    CHECK(hle::registry().is_firmware_first("cellJpgEnc"));
}

TEST_CASE("cellJpgEnc: QueryAttr's memory and the checks of Open and Close", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_config(b, 640, 480, false, 0x100);
    REQUIRE(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == needed(640, false, 0x100));
    CHECK(ps_read8(b, kAttr + 4) == 4);         // cmdQueueDepth
    CHECK(ps_read32(b, kAttr + 8) == 0x270000); // versionUpper
    CHECK(ps_read32(b, kAttr + 12) == 0);       // versionLower
    write_config(b, 1920, 1080, true);
    REQUIRE(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == needed(1920, true));
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, 0) == kArg);
    CHECK(g.call("cellJpgEncQueryAttr", 0, kAttr) == kArg);
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kNowhere) == kArg);
    write_config(b, 0, 480);
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 640, 0);
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 0x10000, 480); // past JPEG's 65535
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 640, 0xFFFF);
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == 0);
    write_config(b, 640, 480, false, 0x80000000); // addMemSize negative
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 640, 480);
    ps_write32(b, kCfg + 20, 1); // extended parameters
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);

    // Open: the handle is the work memory; too little of it, priorities out of range, or memory already open: ARG.
    write_config(b, 64, 64);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(64) - 1);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 4, needed(64));
    ps_write32(b, kRsrc + 8, 0xC00);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 8, 0xFFFFFFFF);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 0x100);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 12, 200);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, 0) == kArg);
    CHECK(g.call("cellJpgEncOpen", kCfg, 0, kHandle) == kArg);
    CHECK(g.call("cellJpgEncOpen", 0, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc, 0);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc, kWork);
    REQUIRE(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == kWork);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg); // already open
    CHECK(g.call("cellJpgEncClose", kWork) == 0);
    CHECK(g.call("cellJpgEncClose", kWork) == kArg);
    CHECK(g.call("cellJpgEncClose", 0) == kArg);
    // OpenEx: a SPURS instance and eight priorities of at most 15.
    std::memset(b + kRsrc, 0, 24);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(64));
    ps_write32(b, kRsrc + 8, 1000);
    ps_write8(b, kRsrc + 16 + 3, 16);
    CHECK(g.call("cellJpgEncOpenEx", kCfg, kRsrc, kHandle) == kArg);
    ps_write8(b, kRsrc + 16 + 3, 15);
    REQUIRE(g.call("cellJpgEncOpenEx", kCfg, kRsrc, kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == kWork);
    CHECK(g.call("cellJpgEncClose", kWork) == 0);
}

TEST_CASE("cellJpgEnc: pictures in every colour space and sampling, decoded back within a PSNR", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const Source src = smooth(37, 23, 11);
    const int hv[5][2] = {{1, 1}, {2, 1}, {2, 2}, {4, 1}, {1, 1}}; // FULL: every component at full size
    for (const u32 sampling : {S444, S422, S420, S411, FULL}) {
        // The same JPEG whatever the picture's layout: RGB, RGBA and ARGB (their alpha not encoded), and YCbCr made of
        // the same colours as the library makes them; rows padded.
        std::vector<u8> first;
        for (const u32 space : {RGB, RGBA, ARGB, YCBCR}) {
            INFO(std::format("sampling {}, colour space {}", sampling, space));
            const u32 pitch = src.w * pixel_bytes(space) + 9;
            put_picture(b, src, space, pitch);
            const std::vector<u8> file = round_trip(g, h, {src.w, src.h, pitch, space}, {YCBCR, sampling, 0, 0, 85});
            if (first.empty()) {
                first = file;
                jpeg::EncodeSettings s;
                s.quality = 85;
                s.sampling = static_cast<jpeg::Sampling>(sampling == FULL ? S444 : sampling);
                CHECK(file == jpeg::encode(planes_of(src, false), s)); // the encoder's, from T.871's YCbCr
            }
            CHECK(file == first);
        }
        const Decoded d = read_jpeg(first.data(), first.size(), jpeg::ColorSpace::RGB);
        REQUIRE(d.ok);
        CHECK(d.header.color_space == jpeg::ColorSpace::YCbCr);
        REQUIRE(d.sampling.size() == 3);
        CHECK(d.sampling[0] == std::array<int, 3>{1, hv[sampling][0], hv[sampling][1]});
        CHECK(d.q0 == table(true, 85));
        // Measured: 4:4:4 44.2 dB, 4:2:2 41.9, 4:2:0 41.6, 4:1:1 33.3.
        const double p = psnr(d.pixels, src.rgb);
        INFO(std::format("sampling {}: PSNR {:.2f} dB", sampling, p));
        constexpr double kAtLeast[5] = {41.0, 38.0, 38.0, 30.0, 41.0};
        CHECK(p > kAtLeast[sampling]);
    }
    // A gray JPEG (encodeColorSpace GRAYSCALE), from a gray picture and from colour ones (T.871's luma): the same.
    {
        std::vector<u8> first;
        for (const u32 space : {GRAY, RGB, RGBA, ARGB, YCBCR})
            for (const u32 sampling : {S420, FULL}) {
                INFO(std::format("gray from colour space {}, sampling {}", space, sampling));
                const u32 pitch = src.w * pixel_bytes(space) + 3;
                put_picture(b, src, space, pitch);
                const std::vector<u8> file = round_trip(g, h, {src.w, src.h, pitch, space}, {GRAY, sampling, 0, 0, 85});
                if (first.empty()) first = file;
                CHECK(file == first); // the sampling format is the chroma's: none here
            }
        const Decoded d = read_jpeg(first.data(), first.size(), jpeg::ColorSpace::Gray);
        REQUIRE(d.ok);
        CHECK(d.header.components == 1);
        CHECK(d.header.color_space == jpeg::ColorSpace::Gray);
        const double p = psnr(d.pixels, gray_of(src));
        INFO(std::format("gray: PSNR {:.2f} dB", p));
        CHECK(p > 47.0); // measured 50.6
    }
    // A gray picture as a colour JPEG: its chroma neutral.
    {
        put_picture(b, src, GRAY, src.w);
        const std::vector<u8> file = round_trip(g, h, {src.w, src.h, src.w, GRAY}, {YCBCR, S444, 0, 0, 90});
        const Decoded d = read_jpeg(file.data(), file.size(), jpeg::ColorSpace::YCbCr);
        REQUIRE(d.ok);
        REQUIRE(d.header.components == 3);
        for (usize i = 0; i < d.pixels.size(); i += 3) {
            CHECK(std::abs(d.pixels[i + 1] - 128) <= 1);
            CHECK(std::abs(d.pixels[i + 2] - 128) <= 1);
        }
    }
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: EncodePicture2's planar YCbCr pictures", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const Source src = smooth(29, 19, 5);
    const jpeg::Planes full = planes_of(src, false);
    // Each planar layout as cellVdec's YUV420_PLANAR has it: Y (pitch), then Cb and Cr (pitch / hs rounded up, height /
    // vs rounded up), the chroma the mean of the samples it covers; encoded at that sampling, it is the JPEG of the
    // full-size planes with that chroma repeated.
    const u32 subsampling[4][2] = {{1, 1}, {2, 1}, {2, 2}, {4, 1}};
    for (const u32 sampling : {S444, S422, S420, S411}) {
        INFO("planes at sampling " << sampling);
        const u32 hs = subsampling[sampling][0], vs = subsampling[sampling][1];
        const u32 pitch = 32, cpitch = (pitch + hs - 1) / hs, cw = (src.w + hs - 1) / hs, ch = (src.h + vs - 1) / vs;
        std::memset(b + kPixels, 0x5A, 0x10000);
        jpeg::Planes repeated = full;
        for (u32 y = 0; y < src.h; y++)
            for (u32 x = 0; x < src.w; x++) ps_write8(b, kPixels + y * pitch + x, full.y[y * src.w + x]);
        const u32 cb_at = kPixels + pitch * src.h, cr_at = cb_at + cpitch * ch;
        for (u32 cy = 0; cy < ch; cy++)
            for (u32 cx = 0; cx < cw; cx++) {
                u32 sum_b = 0, sum_r = 0, n = 0;
                for (u32 y = cy * vs; y < std::min(src.h, cy * vs + vs); y++)
                    for (u32 x = cx * hs; x < std::min(src.w, cx * hs + hs); x++) {
                        sum_b += full.cb[y * src.w + x];
                        sum_r += full.cr[y * src.w + x];
                        n++;
                    }
                const u8 cb = static_cast<u8>((sum_b + n / 2) / n), cr = static_cast<u8>((sum_r + n / 2) / n);
                ps_write8(b, cb_at + cy * cpitch + cx, cb);
                ps_write8(b, cr_at + cy * cpitch + cx, cr);
                for (u32 y = cy * vs; y < std::min(src.h, cy * vs + vs); y++)
                    for (u32 x = cx * hs; x < std::min(src.w, cx * hs + hs); x++) {
                        repeated.cb[y * src.w + x] = cb;
                        repeated.cr[y * src.w + x] = cr;
                    }
            }
        Pic p{src.w, src.h, pitch, YCBCR, sampling};
        const std::vector<u8> file = round_trip(g, h, p, {YCBCR, sampling, 0, 0, 90}, true);
        jpeg::EncodeSettings s;
        s.quality = 90;
        s.sampling = static_cast<jpeg::Sampling>(sampling);
        CHECK(file == jpeg::encode(repeated, s));
        const Decoded d = read_jpeg(file.data(), file.size(), jpeg::ColorSpace::RGB);
        REQUIRE(d.ok);
        CHECK(psnr(d.pixels, src.rgb) > 31.0); // measured 34.3 (4:1:1) to 46.1 (4:4:4)
        // At another sampling it still decodes as the picture.
        const std::vector<u8> other = round_trip(g, h, p, {YCBCR, sampling == S444 ? S420 : S444, 5, 0, 90}, true);
        const Decoded o = read_jpeg(other.data(), other.size(), jpeg::ColorSpace::RGB);
        REQUIRE(o.ok);
        CHECK(psnr(o.pixels, src.rgb) > 28.0);
    }
    // FULL: the pixels interleaved, as EncodePicture's.
    put_picture(b, src, RGBA, src.w * 4);
    const std::vector<u8> two = round_trip(g, h, {src.w, src.h, src.w * 4, RGBA, FULL}, {YCBCR, S422, 0, 0, 70}, true);
    CHECK(two == round_trip(g, h, {src.w, src.h, src.w * 4, RGBA}, {YCBCR, S422, 0, 0, 70}));
    // Planes of a picture that is not YCbCr, an unknown sampling, planes past guest memory: ARG.
    CHECK(encode(g, h, {src.w, src.h, 32, RGB, S420}, {}, kStreamMax, 1, 0, true) == kArg);
    CHECK(encode(g, h, {src.w, src.h, 32, YCBCR, 5}, {}, kStreamMax, 1, 0, true) == kArg);
    // (the Y plane runs past the end of guest memory)
    CHECK(encode(g, h, {src.w, src.h, 32, YCBCR, S420, kData + 0x2FFF80}, {}, kStreamMax, 1, 0, true) == kArg);
    CHECK(encode(g, h, {src.w, src.h, 16, YCBCR, S420}, {}, kStreamMax, 1, 0, true) == kArg); // pitch < width
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: the quality, the DCT methods, SPUs asked for", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const Source src = smooth(48, 40, 2);
    put_picture(b, src, RGB, src.w * 3);
    const Pic pic{src.w, src.h, src.w * 3, RGB};
    usize last_size = 0;
    double last_psnr = 0;
    for (const u32 quality : {1u, 10u, 40u, 75u, 95u, 100u}) {
        INFO("quality " << quality);
        const std::vector<u8> file = round_trip(g, h, pic, {YCBCR, S444, 0, 0, quality});
        const Decoded d = read_jpeg(file.data(), file.size(), jpeg::ColorSpace::RGB);
        REQUIRE(d.ok);
        CHECK(d.q0 == table(true, static_cast<int>(quality)));
        CHECK(d.q1 == table(false, static_cast<int>(quality)));
        const double p = psnr(d.pixels, src.rgb);
        CHECK(file.size() > last_size);
        CHECK(p > last_psnr);
        last_size = file.size();
        last_psnr = p;
    }
    CHECK(last_psnr > 45.0); // quality 100: every quantiser 1
    // The fast method: as good a picture, nearly the same stream.
    const std::vector<u8> quality = round_trip(g, h, pic, {YCBCR, S420, 0, 0, 80});
    const std::vector<u8> fast = round_trip(g, h, pic, {YCBCR, S420, 5, 0, 80, true});
    const Decoded dq = read_jpeg(quality.data(), quality.size(), jpeg::ColorSpace::RGB);
    const Decoded df = read_jpeg(fast.data(), fast.size(), jpeg::ColorSpace::RGB);
    REQUIRE(dq.ok);
    REQUIRE(df.ok);
    CHECK(std::abs(psnr(dq.pixels, src.rgb) - psnr(df.pixels, src.rgb)) < 0.5);
    CHECK(psnr(dq.pixels, df.pixels) > 40.0);
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: the stream size limit mode lowers the quality until the stream fits", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const Source src = smooth(64, 48, 9);
    put_picture(b, src, RGB, src.w * 3);
    const Pic pic{src.w, src.h, src.w * 3, RGB};
    const jpeg::Planes planes = planes_of(src, false);
    const auto size_at = [&](int quality) {
        jpeg::EncodeSettings s;
        s.quality = quality;
        s.sampling = jpeg::Sampling::YCbCr420;
        return jpeg::encode(planes, s).size();
    };
    const auto quality_of = [](const Decoded& d) {
        for (int q = 1; q <= 100; q++)
            if (d.q0 == table(true, q) && d.q1 == table(false, q)) return q;
        return 0;
    };
    // A limit between quality 30's stream and 90's: the highest quality up to 90 whose stream fits.
    const u32 limit = static_cast<u32>((size_at(30) + size_at(90)) / 2);
    REQUIRE(encode(g, h, pic, {YCBCR, S420, 0, 1, 90}, limit) == 0);
    Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.limit == limit);
    CHECK(i.size <= limit);
    CHECK(i.lines == src.h);
    std::vector<u8> file(b + kStream, b + kStream + i.size);
    Decoded d = read_jpeg(file.data(), file.size(), jpeg::ColorSpace::RGB);
    REQUIRE(d.ok);
    const int chosen = quality_of(d);
    INFO("quality " << chosen);
    CHECK(chosen > 30);
    CHECK(chosen < 90);
    CHECK(size_at(chosen) == i.size);
    CHECK(size_at(chosen + 1) > limit);
    // A stream that fits at the quality asked: that quality.
    REQUIRE(encode(g, h, pic, {YCBCR, S420, 0, 1, 60}, kStreamMax) == 0);
    i = stream_info(g, h);
    CHECK(i.state == 0);
    file.assign(b + kStream, b + kStream + i.size);
    d = read_jpeg(file.data(), file.size(), jpeg::ColorSpace::RGB);
    CHECK(quality_of(d) == 60);
    // To a limit even quality 1 does not fit: what fits of quality 1's stream, STREAM_OVERFLOW.
    std::memset(b + kStream, 0xEE, 0x400);
    REQUIRE(encode(g, h, pic, {YCBCR, S420, 0, 1, 90}, 300) == 0);
    i = stream_info(g, h);
    CHECK(i.state == kOverflow);
    CHECK(i.size == 300);
    CHECK(i.lines == src.h);
    jpeg::EncodeSettings s1;
    s1.quality = 1;
    const std::vector<u8> q1 = jpeg::encode(planes, s1);
    CHECK(std::memcmp(b + kStream, q1.data(), 300) == 0);
    CHECK(ps_read8(b, kStream + 300) == 0xEE);
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: the result queue: four at most, BUSY, EMPTY, Reset; a buffer too small", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    std::memset(b + kPixels, 0x22, 64 * 4);
    CHECK(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == kEmpty);
    CHECK(ps_read32(b, kNum) == 0);
    CHECK(g.call("cellJpgEncGetStreamInfo", h, kInfo) == kEmpty);
    for (u32 i = 0; i < 4; i++) {
        REQUIRE(g.call("cellJpgEncWaitForInput", h, 1) == 0);
        Pic p{8, 8, 32, RGBA};
        p.user = 0x100 + i;
        REQUIRE(encode(g, h, p, {}) == 0);
    }
    CHECK(g.call("cellJpgEncWaitForInput", h, 0) == kBusy);
    CHECK(encode(g, h, {8, 8, 32, RGBA}, {}) == kBusy);
    REQUIRE(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == 0);
    CHECK(ps_read32(b, kNum) == 4);
    Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.location == 1);
    CHECK(i.name == 0);
    CHECK(i.addr == kStream);
    CHECK(i.limit == kStreamMax);
    CHECK(i.lines == 8);
    CHECK(i.user == 0x100); // oldest first
    CHECK(i.size > 600);
    CHECK(ps_read32(b, kStream) == 0xFFD8FFE0); // SOI, APP0
    CHECK(ps_read16(b, kStream + i.size - 2) == 0xFFD9);
    CHECK(stream_info(g, h).user == 0x101);
    CHECK(g.call("cellJpgEncWaitForInput", h, 0) == 0);
    REQUIRE(g.call("cellJpgEncWaitForOutput", h, kNum, 1) == 0);
    CHECK(ps_read32(b, kNum) == 2);
    REQUIRE(g.call("cellJpgEncReset", h) == 0);
    CHECK(g.call("cellJpgEncGetStreamInfo", h, kInfo) == kEmpty);
    CHECK(g.call("cellJpgEncReset", h + 4) == kArg);
    CHECK(g.call("cellJpgEncWaitForOutput", h, 0, 0) == kArg);
    CHECK(g.call("cellJpgEncGetStreamInfo", h, kNowhere) == kArg);

    // A buffer too small: what fits is written, the stream overflows.
    std::memset(b + kStream, 0xEE, 64);
    REQUIRE(encode(g, h, {8, 8, 32, RGBA}, {}, 20) == 0);
    i = stream_info(g, h);
    CHECK(i.state == kOverflow);
    CHECK(i.size == 20);
    CHECK(i.limit == 20);
    CHECK(ps_read32(b, kStream) == 0xFFD8FFE0);
    CHECK(ps_read8(b, kStream + 20) == 0xEE);
    CHECK(g.call("cellJpgEncClose", h) == 0);
    CHECK(g.call("cellJpgEncWaitForInput", h, 0) == kArg);
    CHECK(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == kArg);
    CHECK(g.call("cellJpgEncEncodePicture", h, kPic, kParam, kOutParam) == kArg);
}

TEST_CASE("cellJpgEnc: the waits block until there is a result or room, and a Reset ends a wait", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    std::memset(b + kPixels, 0x40, 16 * 16 * 3);
    // WaitForOutput blocks until another thread's picture is encoded.
    {
        std::atomic<bool> encoded{false};
        std::thread other([&] {
            GuestThread o(f, 1, "encoder");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            encoded = true;
            THREAD_CHECK(encode(o, h, {16, 16, 48, RGB}, {}) == 0);
        });
        const u64 r = g.call("cellJpgEncWaitForOutput", h, kNum, 1);
        CHECK(encoded.load());
        other.join();
        CHECK(r == 0);
        CHECK(ps_read32(b, kNum) == 1);
        CHECK(stream_info(g, h).state == 0);
    }
    // WaitForOutput ends with RESET when the encoder is reset meanwhile.
    {
        std::thread other([&] {
            GuestThread o(f, 1, "resetter");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            THREAD_CHECK(o.call("cellJpgEncReset", h) == 0);
        });
        const u64 r = g.call("cellJpgEncWaitForOutput", h, kNum, 1);
        other.join();
        CHECK(r == kReset);
    }
    // WaitForInput blocks while four results wait, until one is taken.
    for (u32 i = 0; i < 4; i++) REQUIRE(encode(g, h, {16, 16, 48, RGB}, {}) == 0);
    {
        std::atomic<bool> taken{false};
        std::thread other([&] {
            GuestThread o(f, 1, "taker");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            taken = true;
            THREAD_CHECK(o.call("cellJpgEncGetStreamInfo", h, kData + 0x600) == 0);
        });
        const u64 r = g.call("cellJpgEncWaitForInput", h, 1);
        CHECK(taken.load());
        other.join();
        CHECK(r == 0);
    }
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: to a file on /dev_hdd0, and one that cannot be made", "[hle][jpgenc]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "pspocket-test-jpgenc";
    fs::remove_all(root);
    fs::create_directories(root / "hdd0" / "tmp"); // lv2 lets no file be made in /dev_hdd0 itself
    Fixture f;
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const Source src = smooth(40, 30, 4);
    put_picture(b, src, ARGB, src.w * 4);
    const Pic pic{src.w, src.h, src.w * 4, ARGB};
    std::memcpy(b + kName, "/dev_hdd0/tmp/shot.jpg", 23);
    const auto read_file = [&] {
        std::ifstream file(root / "hdd0" / "tmp" / "shot.jpg", std::ios::binary);
        return std::vector<u8>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    REQUIRE(encode(g, h, pic, {YCBCR, S422, 0, 0, 90}, 0, 0, kName) == 0);
    Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.location == 0);
    CHECK(i.name == kName);
    CHECK(i.addr == 0);
    CHECK(i.lines == src.h);
    std::vector<u8> bytes = read_file();
    CHECK(bytes.size() == i.size);
    Decoded d = read_jpeg(bytes.data(), bytes.size(), jpeg::ColorSpace::RGB);
    REQUIRE(d.ok);
    CHECK(d.sampling[0] == std::array<int, 3>{1, 2, 1});
    CHECK(psnr(d.pixels, src.rgb) > 40.0); // measured 43.7
    // In the stream size limit mode a file is limited too: to between quality 30's stream and 90's.
    jpeg::EncodeSettings s30;
    s30.quality = 30;
    s30.sampling = jpeg::Sampling::YCbCr422;
    const u32 limit = static_cast<u32>((jpeg::encode(planes_of(src, false), s30).size() + bytes.size()) / 2);
    REQUIRE(encode(g, h, pic, {YCBCR, S422, 0, 1, 90}, limit, 0, kName) == 0);
    i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.size <= limit);
    bytes = read_file();
    CHECK(bytes.size() == i.size);
    d = read_jpeg(bytes.data(), bytes.size(), jpeg::ColorSpace::RGB);
    CHECK(d.ok);
    std::memcpy(b + kName, "/dev_nowhere/x.jpg", 19);
    REQUIRE(encode(g, h, pic, {YCBCR, S422, 0, 0, 90}, 0, 0, kName) == 0);
    i = stream_info(g, h);
    CHECK(i.state == kFileOpen);
    CHECK(i.size == 0);
    CHECK(g.call("cellJpgEncClose", h) == 0);
    fs::remove_all(root);
}

TEST_CASE("cellJpgEnc: EncodePicture's checks", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, 64, 48);
    std::memset(b + kPixels, 0x33, 64 * 48 * 4);
    const Pic good{16, 16, 64, RGBA};
    const Param ok{};
    CHECK(encode(g, h + 1, good, ok) == kArg);
    CHECK(encode(g, 0, good, ok) == kArg);
    CHECK(g.call("cellJpgEncEncodePicture", h, 0, kParam, kOutParam) == kArg);
    CHECK(g.call("cellJpgEncEncodePicture", h, kPic, 0, kOutParam) == kArg);
    CHECK(g.call("cellJpgEncEncodePicture", h, kPic, kParam, 0) == kArg);
    CHECK(g.call("cellJpgEncEncodePicture", h, kNowhere, kParam, kOutParam) == kArg);
    CHECK(encode(g, h, {0, 16, 64, RGBA}, ok) == kArg);         // no width
    CHECK(encode(g, h, {16, 0, 64, RGBA}, ok) == kArg);         // no height
    CHECK(encode(g, h, {65, 16, 260, RGBA}, ok) == kArg);       // past maxWidth
    CHECK(encode(g, h, {16, 49, 64, RGBA}, ok) == kArg);        // past maxHeight
    CHECK(encode(g, h, {16, 16, 64, 4}, ok) == kArg);           // no such colour space (a palette)
    CHECK(encode(g, h, {16, 16, 63, RGBA}, ok) == kArg);        // a pitch shorter than a row
    CHECK(encode(g, h, {16, 16, 47, RGB}, ok) == kArg);
    CHECK(encode(g, h, {16, 16, 64, RGBA, FULL, 0}, ok) == kArg);       // no pixels
    CHECK(encode(g, h, {16, 16, 64, RGBA, FULL, kNowhere}, ok) == kArg); // pixels not guest memory
    CHECK(encode(g, h, {16, 16, 64, RGBA, FULL, kData + 0x2FFF00}, ok) == kArg); // pixels running past it
    CHECK(encode(g, h, good, {9}) == kArg);                     // no such encode colour space
    CHECK(encode(g, h, good, {YCBCR, 5}) == kArg);              // no such sampling format
    CHECK(encode(g, h, good, {YCBCR, S420, 1}) == kArg);        // no such DCT method
    CHECK(encode(g, h, good, {YCBCR, S420, 0, 2}) == kArg);     // no such compression mode
    CHECK(encode(g, h, good, {YCBCR, S420, 0, 0, 0}) == kArg);  // quality 1 to 100
    CHECK(encode(g, h, good, {YCBCR, S420, 0, 0, 101}) == kArg);
    CHECK(encode(g, h, good, {YCBCR, S420, 0, 1, 0}) == kArg);
    CHECK(encode(g, h, good, ok, 0x1000, 2) == kArg);           // no such location
    CHECK(encode(g, h, good, ok, 0, 1) == kArg);                // a buffer of no size
    CHECK(encode(g, h, good, ok, 0x1000, 0, 0) == kArg);        // a file without a name
    std::memcpy(b + kName, "/dev_hdd0/tmp/x.jpg", 20);
    CHECK(encode(g, h, good, {YCBCR, S420, 0, 1, 90}, 0, 0, kName) == kArg); // the limit mode without a limit
    {
        // A buffer without an address.
        CHECK(encode(g, h, good, ok) == 0);
        ps_write32(b, kOutParam + 8, 0);
        CHECK(g.call("cellJpgEncEncodePicture", h, kPic, kParam, kOutParam) == kArg);
    }
    CHECK(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == 0);
    CHECK(ps_read32(b, kNum) == 1); // only the good one queued
    CHECK(encode(g, h, good, ok) == 0);
    CHECK(encode(g, h, {64, 48, 256, RGBA}, ok) == 0); // the largest the encoder was opened for
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

// ------------------------------------------------------------------------------------- several threads at once (D-650)

namespace {

// A thread's own guest structures for the calls, 0x100 bytes from `at` (kCalls + 0x100 * n, nine of them): Open's
// configuration and resource, the handle, the picture, the parameters, the output parameters, the stream information.
constexpr u32 kCalls = kData + 0x400;
constexpr u32 kAtCfg = 0x00, kAtRsrc = 0x20, kAtHandle = 0x40, kAtPic = 0x50, kAtParam = 0x70, kAtOut = 0x90,
              kAtInfo = 0xA0, kAtNum = 0xD0;
static_assert(kCalls + 9 * 0x100 <= kPixels);

bool eventually(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// cellJpgEncOpen of an encoder whose work memory is `work`, from the structures at `at`: its handle, or 0.
u32 open_at(GuestThread& g, u32 at, u32 work, u32 max_w = 640, u32 max_h = 480)
{
    u8* b = g.f.base();
    std::memset(b + at + kAtCfg, 0, 24);
    ps_write32(b, at + kAtCfg, max_w);
    ps_write32(b, at + kAtCfg + 4, max_h);
    ps_write32(b, at + kAtRsrc, work);
    ps_write32(b, at + kAtRsrc + 4, needed(max_w));
    ps_write32(b, at + kAtRsrc + 8, 1000);
    ps_write32(b, at + kAtRsrc + 12, 200);
    if (g.call("cellJpgEncOpen", at + kAtCfg, at + kAtRsrc, at + kAtHandle) != 0) return 0;
    return ps_read32(b, at + kAtHandle);
}

// cellJpgEncEncodePicture of `p` (its pixels at p.addr) into the buffer `stream` of `limit` bytes, from the structures
// at `at`.
u64 encode_at(GuestThread& g, u32 handle, u32 at, const Pic& p, const Param& e, u32 stream, u32 limit)
{
    u8* b = g.f.base();
    const u32 pic = at + kAtPic, param = at + kAtParam, out = at + kAtOut;
    std::memset(b + pic, 0, 32);
    ps_write32(b, pic, p.width);
    ps_write32(b, pic + 4, p.height);
    ps_write32(b, pic + 8, p.pitch);
    ps_write32(b, pic + 12, p.space);
    ps_write32(b, pic + 16, p.addr);
    ps_write64(b, pic + 24, p.user);
    std::memset(b + param, 0, 24);
    ps_write32(b, param + 4, e.space);
    ps_write32(b, param + 8, e.sampling);
    ps_write32(b, param + 12, e.method);
    ps_write32(b, param + 16, e.mode);
    ps_write32(b, param + 20, e.quality);
    ps_write32(b, out, 1); // a buffer
    ps_write32(b, out + 4, 0);
    ps_write32(b, out + 8, stream);
    ps_write32(b, out + 12, limit);
    return g.call("cellJpgEncEncodePicture", handle, pic, param, out);
}

// cellJpgEncGetStreamInfo into the structures at `at`: its answer, and the information in `i`.
u64 info_at(GuestThread& g, u32 handle, u32 at, Info& i)
{
    u8* b = g.f.base();
    const u32 info = at + kAtInfo;
    const u64 r = g.call("cellJpgEncGetStreamInfo", handle, info);
    i = {static_cast<s32>(ps_read32(b, info)), ps_read32(b, info + 4), ps_read32(b, info + 8), ps_read32(b, info + 12),
        ps_read32(b, info + 16), ps_read32(b, info + 20), ps_read32(b, info + 24), ps_read64(b, info + 32)};
    return r;
}

} // namespace

TEST_CASE("cellJpgEnc: two encoders on two threads at once, each stream the one an encoder alone makes (D-650)",
    "[hle][jpgenc]")
{
    constexpr u32 kRounds = 8, kW = 96, kH = 64, kLimit = 0x40000;
    constexpr u32 kPictures[2] = {kPixels, kPixels + 0x20000}, kWorks[2] = {kWork, kWork + 0x80000},
                  kStreams[2] = {kStream, kStream + kLimit};
    Fixture f;
    u8* b = f.base();
    put_picture(b, smooth(kW, kH, 5), RGB, kW * 3, kPictures[0]);
    put_picture(b, smooth(kW, kH, 41), RGBA, kW * 4, kPictures[1]);
    const auto picture = [&](u32 t, u32 round) {
        Pic p{kW, kH, kW * (t ? 4 : 3), t ? RGBA : RGB, FULL, kPictures[t]};
        p.user = 0x1000 * (t + 1) + round;
        return p;
    };
    // Gray and colour, every sampling, both DCTs, qualities 40 to 89.
    const auto settings = [](u32 round) {
        return Param{round % 3 ? YCBCR : GRAY, round % 4, round % 2 ? 5u : 0u, 0, 40 + 7 * round};
    };

    // What an encoder alone makes of each picture in each round.
    std::vector<u8> want[2][kRounds];
    {
        GuestThread g(f, 0, "alone");
        const u32 h = open_at(g, kCalls, kWorks[0]);
        REQUIRE(h != 0);
        for (u32 t = 0; t < 2; t++)
            for (u32 r = 0; r < kRounds; r++) {
                REQUIRE(encode_at(g, h, kCalls, picture(t, r), settings(r), kStreams[0], kLimit) == 0);
                Info i{};
                REQUIRE(info_at(g, h, kCalls, i) == 0);
                REQUIRE(i.state == 0);
                REQUIRE(i.size > 0);
                want[t][r].assign(b + kStreams[0], b + kStreams[0] + i.size);
            }
        REQUIRE(g.call("cellJpgEncClose", h) == 0);
    }

    // Two encoders, each on its own thread with its own pictures, at once.
    std::atomic<u32> rounds{0}, wrong{0};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < 2; t++) {
        threads.emplace_back([&, t] {
            GuestThread g(f, 1 + t, "encoder");
            const u32 at = kCalls + 0x100 * (t + 1);
            const u32 h = open_at(g, at, kWorks[t]);
            THREAD_CHECK(h == kWorks[t]);
            if (h != kWorks[t]) return;
            for (u32 r = 0; r < kRounds; r++) {
                const u64 encoded = encode_at(g, h, at, picture(t, r), settings(r), kStreams[t], kLimit);
                const u64 waited = g.call("cellJpgEncWaitForOutput", h, at + kAtNum, 1);
                Info i{};
                const u64 taken = info_at(g, h, at, i);
                const bool same = encoded == 0 && waited == 0 && taken == 0 && i.state == 0 && i.addr == kStreams[t] &&
                    i.user == picture(t, r).user && i.size == want[t][r].size() &&
                    std::memcmp(b + kStreams[t], want[t][r].data(), i.size) == 0;
                if (!same) wrong++;
                rounds++;
            }
            THREAD_CHECK(g.call("cellJpgEncClose", h) == 0);
        });
    }
    for (std::thread& th : threads) th.join();
    CHECK(rounds == 2 * kRounds);
    CHECK(wrong == 0);
}

TEST_CASE("cellJpgEnc: eight threads encoding with one encoder at once: four pictures queued, the others BUSY; a Reset "
          "drops those still being encoded (D-651)", "[hle][jpgenc]")
{
    constexpr u32 kThreads = 8, kW = 256, kH = 192, kLimit = 0x10000;
    static_assert(kStream + kThreads * kLimit <= kWork);
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    put_picture(b, smooth(kW, kH, 17), RGB, kW * 3);
    const u32 h = open_at(g, kCalls, kWork);
    REQUIRE(h != 0);
    const Pic pic{kW, kH, kW * 3, RGB};
    // The stream the encoder makes of the picture alone.
    REQUIRE(encode_at(g, h, kCalls, pic, {}, kStream, kLimit) == 0);
    Info alone{};
    REQUIRE(info_at(g, h, kCalls, alone) == 0);
    REQUIRE(alone.state == 0);
    const std::vector<u8> want(b + kStream, b + kStream + alone.size);

    for (const bool reset : {false, true}) {
        INFO("reset " << reset);
        std::atomic<u32> ready{0}, busy{0};
        std::atomic<bool> go{false};
        std::array<std::atomic<u64>, kThreads> answers;
        for (auto& a : answers) a = ~0ull;
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kThreads; t++) {
            threads.emplace_back([&, t] {
                GuestThread o(f, 1 + t, "encoder");
                Pic p = pic;
                p.user = 0x700 + t;
                ready++;
                while (!go.load()) std::this_thread::yield();
                const u64 r = encode_at(o, h, kCalls + 0x100 * (t + 1), p, {}, kStream + kLimit * t, kLimit);
                answers[t] = r;
                if (r == kBusy) busy++;
            });
        }
        while (ready.load() < kThreads) std::this_thread::yield();
        go = true;
        if (reset) {
            // Every thread has made its call once four were refused: the four taken still being encoded, or done.
            CHECK(eventually([&] { return busy.load() == kThreads - 4; }));
            REQUIRE(g.call("cellJpgEncReset", h) == 0);
        }
        for (std::thread& th : threads) th.join();
        u32 refused = 0;
        std::vector<u64> accepted;
        for (u32 t = 0; t < kThreads; t++) {
            if (answers[t] == 0) accepted.push_back(0x700 + t);
            else if (answers[t] == kBusy) refused++;
        }
        CHECK(accepted.size() == 4);
        CHECK(refused == 4);
        if (!reset) {
            // The four pictures taken, each the stream the encoder makes alone, in its own thread's buffer.
            REQUIRE(g.call("cellJpgEncWaitForOutput", h, kCalls + kAtNum, 0) == 0);
            CHECK(ps_read32(b, kCalls + kAtNum) == 4);
            std::vector<u64> users;
            for (u32 n = 0; n < 4; n++) {
                Info s{};
                REQUIRE(info_at(g, h, kCalls, s) == 0);
                CHECK(s.state == 0);
                users.push_back(s.user);
                const u64 t = s.user - 0x700;
                REQUIRE(t < kThreads);
                CHECK(s.addr == kStream + kLimit * t);
                REQUIRE(s.size == want.size());
                CHECK(std::memcmp(b + s.addr, want.data(), want.size()) == 0);
            }
            std::sort(users.begin(), users.end());
            CHECK(users == accepted);
        }
        // Nothing more: the four taken, or dropped by the Reset with what was still being encoded then; room again.
        CHECK(g.call("cellJpgEncGetStreamInfo", h, kCalls + kAtInfo) == kEmpty);
        CHECK(g.call("cellJpgEncWaitForInput", h, 0) == 0);
    }
    CHECK(g.call("cellJpgEncClose", h) == 0);
}
