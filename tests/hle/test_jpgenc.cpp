// cellJpgEnc (D-616, D-913) and pspocket's baseline JPEG encoder (D-615, D-913: libjpeg 6b's, as Sony's libjpgenc).
// Pictures built here in every colour space and sampling, encoded, then decoded by pspocket's own JPEG decoder
// (jpeg_decoder.cpp, which decodes as Sony's libjpgdec does) and checked within a PSNR of the input; the streams'
// markers, frame headers and tables as libjpeg 6b writes them; the two queues and Reset's skipped streams; the stream
// size limit mode; buffer and file output; the checks. tests/hle/test_jpgenc_compare.cpp compares the library with Sony's
// libjpgenc byte for byte. No game data.

#include "firmware_compare.hpp" // test_dev_flash
#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_encoder.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace jpeg = pspocket::hle::jpeg;

namespace {

// ---- pictures built here

struct Source {
    u32 w = 0, h = 0;
    std::vector<u8> rgb;
};

// A smooth picture with some detail: R, G, B rows of width x 3 bytes.
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

// Noise: what JPEG cannot make small (a stream of 2 to 3 bytes a pixel at quality 100).
Source noisy(u32 w, u32 h, u32 seed)
{
    Source s{w, h, std::vector<u8>(usize{w} * h * 3)};
    u32 v = seed * 2654435761u + 1;
    for (u8& c : s.rgb) {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
        c = static_cast<u8>(v >> 24);
    }
    return s;
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
    std::vector<u8> pixels; // RGB or gray, width x height
};

Decoded read_jpeg(const std::vector<u8>& data, jpeg::ColorSpace space)
{
    Decoded d;
    jpeg::Decoder dec;
    std::string why;
    if (!dec.read_header(data.data(), data.size(), why) || !dec.decode(why)) {
        INFO(why);
        return d;
    }
    jpeg::Decoder::Output o;
    if (!dec.output(1, false, space, o, why)) return d;
    d.pixels = std::move(o.pixels);
    d.ok = !dec.ran_out();
    return d;
}

// The stream's markers in order (each 0xFF's code), to EOI, the scan's data skipped (restart markers listed).
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

// The data of the first segment with marker `m` before the scan.
std::vector<u8> segment(const std::vector<u8>& d, u8 m)
{
    usize at = 2;
    while (at + 4 <= d.size() && d[at] == 0xFF && d[at + 1] != 0xDA) {
        const usize len = usize{d[at + 2]} << 8 | d[at + 3];
        if (d[at + 1] == m)
            return std::vector<u8>(d.begin() + static_cast<long>(at) + 4, d.begin() + static_cast<long>(at + 2 + len));
        at += 2 + len;
    }
    return {};
}

// ---- the library

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kPic = kData + 0x200, kParam = kData + 0x240, kOutParam = kData + 0x280, kInfo = kData + 0x2C0,
              kNum = kData + 0x300, kEx = kData + 0x400, kExData = kData + 0x500, kName = kData + 0x800,
              kPixels = kData + 0x1000, kStream = kData + 0x40000, kWork = kData + 0x200000;
const u64 kArg = r3_of_code(0x80611191), kBusy = r3_of_code(0x80611193), kEmpty = r3_of_code(0x80611194);
constexpr s32 kSkip = static_cast<s32>(0x806111A2), kOverflow = static_cast<s32>(0x806111A3),
              kFileOpen = static_cast<s32>(0x806111A4);
enum : u32 { GRAY = 1, RGB = 2, YCBCR = 3, RGBA = 10, ARGB = 20 };
enum : u32 { S444 = 0, S422 = 1, S420 = 2, S411 = 3, NONE = 4 };
enum : u32 { QUALITY = 0, FAST = 5 };

void write_config(u8* b, u32 max_w, u32 max_h, u32 space = YCBCR, u32 sampling = S420, bool spu = false)
{
    std::memset(b + kCfg, 0, 28);
    ps_write32(b, kCfg, max_w);
    ps_write32(b, kCfg + 4, max_h);
    ps_write32(b, kCfg + 8, space);
    ps_write32(b, kCfg + 12, sampling);
    ps_write8(b, kCfg + 16, spu ? 1 : 0);
}

// libjpgenc's memory for a configuration (QueryAttr's memSize).
u32 needed(u32 max_w, u32 space, u32 sampling, bool spu = false)
{
    const u32 w = (max_w + 15) & ~15u;
    const u32 per = space == GRAY ? 12
        : space == RGB            ? 30
        : sampling == S444        ? 30
        : sampling == S422        ? 22
        : sampling == S411        ? 18
                                  : 33;
    return (spu ? 0x388A0 : 0x1B630) + w * per + 0x1780;
}

u32 open(GuestThread& g, u32 space = YCBCR, u32 sampling = S420, u32 max_w = 64, u32 max_h = 64)
{
    u8* b = g.f.base();
    write_config(b, max_w, max_h, space, sampling);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(max_w, space, sampling));
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    REQUIRE(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == 0);
    return ps_read32(b, kHandle);
}

struct Pic {
    u32 width = 0, height = 0, space = RGB, pitch = 0; // pitch 0: EncodePicture's rows of the picture's own width
    u64 user = 0;
};

struct Param {
    u32 quality = 90, method = QUALITY, restart = 0, mode = 0, ex = 0;
    bool spu = false;
};

u64 encode(GuestThread& g, u32 handle, const Pic& p, const Param& e, u32 limit = 0x10000, u32 location = 1)
{
    u8* b = g.f.base();
    std::memset(b + kPic, 0, 32);
    ps_write32(b, kPic, p.width);
    ps_write32(b, kPic + 4, p.height);
    if (p.pitch) {
        ps_write32(b, kPic + 8, p.pitch);
        ps_write32(b, kPic + 12, p.space);
        ps_write32(b, kPic + 16, kPixels);
        ps_write64(b, kPic + 24, p.user);
    } else {
        ps_write32(b, kPic + 8, p.space);
        ps_write32(b, kPic + 12, kPixels);
        ps_write64(b, kPic + 16, p.user);
    }
    std::memset(b + kParam, 0, 28);
    ps_write8(b, kParam, e.spu ? 1 : 0);
    ps_write32(b, kParam + 4, e.restart);
    ps_write32(b, kParam + 8, e.method);
    ps_write32(b, kParam + 12, e.mode);
    ps_write32(b, kParam + 16, e.quality);
    ps_write32(b, kParam + 20, e.ex ? kEx : 0);
    ps_write32(b, kParam + 24, e.ex);
    ps_write32(b, kOutParam, location);
    ps_write32(b, kOutParam + 4, location == 0 ? kName : 0);
    ps_write32(b, kOutParam + 8, location == 1 ? kStream : 0);
    ps_write32(b, kOutParam + 12, location == 1 ? limit : 0);
    return g.call(p.pitch ? "cellJpgEncEncodePicture2" : "cellJpgEncEncodePicture", handle, kPic, kParam, kOutParam);
}

struct Info {
    s32 state;
    u32 location, name, addr, limit, size, lines, quality;
    u64 user;
};

Info stream_info(GuestThread& g, u32 handle)
{
    u8* b = g.f.base();
    REQUIRE(g.call("cellJpgEncGetStreamInfo", handle, kInfo) == 0);
    return {static_cast<s32>(ps_read32(b, kInfo)), ps_read32(b, kInfo + 4), ps_read32(b, kInfo + 8),
        ps_read32(b, kInfo + 12), ps_read32(b, kInfo + 16), ps_read32(b, kInfo + 20), ps_read32(b, kInfo + 24),
        ps_read32(b, kInfo + 28), ps_read64(b, kInfo + 32)};
}

void put_rgb(u8* b, const Source& s, u32 space, u32 pitch)
{
    const u32 n = space == GRAY ? 1 : space == RGB || space == YCBCR ? 3 : 4;
    for (u32 y = 0; y < s.h; y++)
        for (u32 x = 0; x < s.w; x++) {
            const u8* c = s.rgb.data() + (usize{y} * s.w + x) * 3;
            u8* o = b + kPixels + usize{y} * pitch + usize{x} * n;
            switch (space) {
            case GRAY: o[0] = jpeg::rgb_to_y(c[0], c[1], c[2]); break;
            case RGB: std::memcpy(o, c, 3); break;
            case YCBCR: jpeg::rgb_to_ycbcr(c[0], c[1], c[2], o[0], o[1], o[2]); break;
            case RGBA:
                std::memcpy(o, c, 3);
                o[3] = 0x5A;
                break;
            default:
                o[0] = 0xA5;
                std::memcpy(o + 1, c, 3);
                break;
            }
        }
}

} // namespace

// ------------------------------------------------------------------------------------------------- the encoder

TEST_CASE("pspocket's JPEG encoder: libjpeg 6b's tables and colours", "[hle][jpgenc]")
{
    // jpeg_set_quality: 5000 / q percent below 50, 200 - 2q from 50, each entry rounded, 1 to 255 (baseline).
    CHECK(jpeg::quant_table(true, 50)[0] == 16);
    CHECK(jpeg::quant_table(true, 85)[0] == 5); // 16 x 30% = 4.8
    const std::array<u8, 64> q100 = jpeg::quant_table(true, 100);
    CHECK(std::all_of(q100.begin(), q100.end(), [](u8 v) { return v == 1; }));
    CHECK(jpeg::quant_table(true, 1)[0] == 255);
    CHECK(jpeg::quant_table(true, 0) == jpeg::quant_table(true, 1));
    CHECK(jpeg::quant_table(false, 10)[0] == 85); // 17 x 500% = 85
    // jccolor.c in 16-bit fixed point.
    u8 y = 0, cb = 0, cr = 0;
    jpeg::rgb_to_ycbcr(255, 255, 255, y, cb, cr);
    CHECK((y == 255 && cb == 128 && cr == 128));
    jpeg::rgb_to_ycbcr(255, 0, 0, y, cb, cr);
    CHECK((y == 76 && cb == 85 && cr == 255));
    jpeg::rgb_to_ycbcr(0, 0, 255, y, cb, cr);
    CHECK((y == 29 && cb == 255 && cr == 107));
    CHECK(jpeg::rgb_to_y(0, 255, 0) == 150);
}

TEST_CASE("pspocket's JPEG encoder: libjpeg 6b's markers and headers, every sampling, both DCTs, read back",
    "[hle][jpgenc]")
{
    const Source src = smooth(37, 23, 5);
    const int hv[4][2] = {{1, 1}, {2, 1}, {2, 2}, {4, 1}};
    const double floor_db[4] = {38, 36, 35, 31};
    for (int k = 0; k < 4; k++)
        for (const jpeg::Dct dct : {jpeg::Dct::Float, jpeg::Dct::Fast}) {
            INFO(std::format("sampling {}x{}, {}", hv[k][0], hv[k][1], dct == jpeg::Dct::Fast ? "fast" : "float"));
            jpeg::Settings s;
            s.width = src.w;
            s.height = src.h;
            s.h_samp = hv[k][0];
            s.v_samp = hv[k][1];
            s.quality = 92;
            s.dct = dct;
            const std::vector<u8> j = jpeg::encode(s, src.rgb.data(), usize{src.w} * 3);
            REQUIRE(!j.empty());
            // SOI, two DQT, SOF0, four DHT, SOS, EOI: no JFIF (libjpgenc writes none).
            CHECK(markers(j) == std::vector<u8>{0xD8, 0xDB, 0xDB, 0xC0, 0xC4, 0xC4, 0xC4, 0xC4, 0xDA, 0xD9});
            const std::vector<u8> sof = segment(j, 0xC0);
            REQUIRE(sof.size() == 15);
            CHECK(sof[0] == 8);
            CHECK(((sof[1] << 8 | sof[2]) == 23 && (sof[3] << 8 | sof[4]) == 37 && sof[5] == 3));
            CHECK(sof[7] == ((hv[k][0] << 4) | hv[k][1]));
            CHECK((sof[10] == 0x11 && sof[11] == 1 && sof[13] == 0x11 && sof[14] == 1));
            const Decoded d = read_jpeg(j, jpeg::ColorSpace::RGB);
            REQUIRE(d.ok);
            CHECK(psnr(d.pixels, src.rgb) > floor_db[k]);
        }
    // Gray and RGB: one quantisation table, two Huffman tables; RGB's components 'R', 'G', 'B' on table 0.
    jpeg::Settings s;
    s.width = src.w;
    s.height = src.h;
    s.output = jpeg::Space::Gray;
    s.quality = 90;
    std::vector<u8> j = jpeg::encode(s, src.rgb.data(), usize{src.w} * 3);
    CHECK(markers(j) == std::vector<u8>{0xD8, 0xDB, 0xC0, 0xC4, 0xC4, 0xDA, 0xD9});
    Decoded d = read_jpeg(j, jpeg::ColorSpace::Gray);
    REQUIRE(d.ok);
    CHECK(psnr(d.pixels, gray_of(src)) > 40);
    s.output = jpeg::Space::RGB;
    j = jpeg::encode(s, src.rgb.data(), usize{src.w} * 3);
    CHECK(markers(j) == std::vector<u8>{0xD8, 0xDB, 0xC0, 0xC4, 0xC4, 0xDA, 0xD9});
    CHECK(segment(j, 0xC0) == std::vector<u8>{8, 0, 23, 0, 37, 3, 'R', 0x11, 0, 'G', 0x11, 0, 'B', 0x11, 0});
    // Restart markers every 2 MCUs, a DRI before SOS; and what cannot be a JPEG.
    s.output = jpeg::Space::YCbCr;
    s.restart_interval = 2;
    j = jpeg::encode(s, src.rgb.data(), usize{src.w} * 3);
    const std::vector<u8> m = markers(j);
    CHECK(std::count_if(m.begin(), m.end(), [](u8 c) { return c >= 0xD0 && c <= 0xD7; }) == 2); // 6 MCUs
    CHECK(segment(j, 0xDD) == std::vector<u8>{0, 2});
    s.width = 0;
    CHECK(jpeg::encode(s, src.rgb.data(), 0).empty());
    s.width = 70000;
    CHECK(jpeg::encode(s, src.rgb.data(), 0).empty());
}

TEST_CASE("pspocket's JPEG encoder: a sink that fills stops it, the rows before counted", "[hle][jpgenc]")
{
    const Source src = noisy(64, 48, 2);
    jpeg::Settings s;
    s.width = 64;
    s.height = 48;
    s.quality = 100;
    std::vector<u8> out;
    const usize limit = 0x1000;
    jpeg::Encoder e(s, [&](const u8* p, usize n) {
        const usize take = std::min(n, limit - out.size());
        out.insert(out.end(), p, p + take);
        return out.size() < limit; // full: no more (libjpeg's empty_output_buffer)
    });
    REQUIRE(e.start());
    u32 y = 0;
    while (y < 48 && e.write_row(src.rgb.data() + usize{y} * 64 * 3)) y++;
    CHECK(y < 48);
    CHECK(e.rows() == y);
    CHECK(y % 16 == 15); // the row completing an iMCU row (16 rows at 4:2:0) is the one whose data does not fit
    CHECK(out.size() == limit);
}

// ------------------------------------------------------------------------------------------------- the library

TEST_CASE("cellJpgEnc: the ten functions, pspocket's own (no longer firmware-first, D-913)", "[hle][jpgenc]")
{
    for (const std::string_view name : {"cellJpgEncQueryAttr", "cellJpgEncOpen", "cellJpgEncOpenEx", "cellJpgEncClose",
             "cellJpgEncWaitForInput", "cellJpgEncEncodePicture", "cellJpgEncEncodePicture2", "cellJpgEncWaitForOutput",
             "cellJpgEncGetStreamInfo", "cellJpgEncReset"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellJpgEnc", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
    CHECK_FALSE(hle::registry().is_firmware_first("cellJpgEnc"));
}

TEST_CASE("cellJpgEnc: QueryAttr's memory and the checks of Open and Close", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_config(b, 640, 480);
    REQUIRE(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == 0x22030); // libjpgenc's at 4:2:0
    CHECK(ps_read8(b, kAttr + 4) == 4);
    CHECK(ps_read32(b, kAttr + 8) == 0x270000);
    CHECK(ps_read32(b, kAttr + 12) == 4);
    for (const auto& [space, sampling] : {std::pair{YCBCR, S444}, std::pair{YCBCR, S422}, std::pair{YCBCR, S411},
             std::pair{GRAY, NONE}, std::pair{RGB, NONE}}) {
        write_config(b, 640, 480, space, sampling, true);
        REQUIRE(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == 0);
        CHECK(ps_read32(b, kAttr) == needed(640, space, sampling, true));
    }
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, 0) == kArg);
    CHECK(g.call("cellJpgEncQueryAttr", 0, kAttr) == kArg);
    for (const auto& [w, space, sampling] : {std::tuple{0u, u32{YCBCR}, u32{S420}}, std::tuple{65536u, u32{YCBCR}, u32{S420}},
             std::tuple{640u, u32{YCBCR}, u32{NONE}}, std::tuple{640u, u32{GRAY}, u32{S420}},
             std::tuple{640u, u32{RGB}, u32{S444}}, std::tuple{640u, 4u, u32{NONE}}, std::tuple{640u, u32{YCBCR}, 5u}}) {
        write_config(b, w, 480, space, sampling);
        CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);
    }
    write_config(b, 640, 480);
    ps_write32(b, kCfg + 24, 1); // extended parameters
    CHECK(g.call("cellJpgEncQueryAttr", kCfg, kAttr) == kArg);

    // Open: the handle is the memory's first 128-byte boundary; too little memory or priorities out of range: ARG;
    // opened again over its memory, a new encoder there.
    write_config(b, 64, 64);
    ps_write32(b, kRsrc, kWork + 0x10);
    ps_write32(b, kRsrc + 4, needed(64, YCBCR, S420) - 1);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 4, needed(64, YCBCR, S420));
    ps_write32(b, kRsrc + 8, 0xC00);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 0x100);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 12, 200);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, 0) == kArg);
    REQUIRE(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == kWork + 0x80);
    CHECK(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == 0);
    CHECK(g.call("cellJpgEncClose", kWork + 0x80) == 0);
    CHECK(g.call("cellJpgEncClose", kWork + 0x80) == kArg);
    CHECK(g.call("cellJpgEncClose", 0) == kArg);
}

TEST_CASE("cellJpgEnc: pictures in every colour space, the streams libjpeg 6b writes", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const Source src = smooth(33, 17, 7);
    // A 4:2:0 encoder: RGB, RGBA and ARGB give the same stream (the alpha left out); so does YCbCr made by libjpeg's
    // own conversion; gray is refused.
    const u32 h = open(g);
    std::vector<std::vector<u8>> streams;
    for (const u32 space : {RGB, YCBCR, RGBA, ARGB}) {
        INFO(space);
        put_rgb(b, src, space, 0x200);
        REQUIRE(encode(g, h, {33, 17, space, 0x200, space}, {85, FAST}) == 0);
        const Info i = stream_info(g, h);
        CHECK(i.state == 0);
        CHECK(i.lines == 17);
        CHECK(i.quality == 85);
        CHECK(i.user == space);
        streams.emplace_back(b + kStream, b + kStream + i.size);
    }
    CHECK(streams[1] == streams[0]);
    CHECK(streams[2] == streams[0]);
    CHECK(streams[3] == streams[0]);
    jpeg::Settings s;
    s.width = 33;
    s.height = 17;
    s.quality = 85;
    s.dct = jpeg::Dct::Fast;
    CHECK(streams[0] == jpeg::encode(s, src.rgb.data(), 33 * 3));
    put_rgb(b, src, GRAY, 33);
    CHECK(encode(g, h, {33, 17, GRAY}, {85, FAST}) == kArg);
    CHECK(g.call("cellJpgEncClose", h) == 0);
    // A gray encoder takes gray, RGB and YCbCr pictures; an RGB one neither gray nor YCbCr.
    const u32 gh = open(g, GRAY, NONE);
    put_rgb(b, src, GRAY, 33);
    REQUIRE(encode(g, gh, {33, 17, GRAY}, {90}) == 0);
    Info i = stream_info(g, gh);
    REQUIRE(i.state == 0);
    const Decoded d = read_jpeg(std::vector<u8>(b + kStream, b + kStream + i.size), jpeg::ColorSpace::Gray);
    REQUIRE(d.ok);
    CHECK(psnr(d.pixels, gray_of(src)) > 40);
    CHECK(g.call("cellJpgEncClose", gh) == 0);
    const u32 rh = open(g, RGB, NONE);
    CHECK(encode(g, rh, {33, 17, YCBCR}, {90}) == kArg);
    CHECK(encode(g, rh, {33, 17, GRAY}, {90}) == kArg);
    put_rgb(b, src, RGB, 99);
    REQUIRE(encode(g, rh, {33, 17, RGB}, {90}) == 0);
    i = stream_info(g, rh);
    REQUIRE(i.state == 0);
    CHECK(segment(std::vector<u8>(b + kStream, b + kStream + i.size), 0xC0)[6] == 'R');
    // Extended parameters: segments right after SOI, the marker code their low byte.
    for (u32 k = 0; k < 16; k++) ps_write8(b, kExData + k, static_cast<u8>('a' + k));
    const u32 ex[2][3] = {{0xFFE1, kExData, 10}, {0x12345678, kExData + 4, 3}};
    for (u32 n = 0; n < 2; n++)
        for (u32 k = 0; k < 3; k++) ps_write32(b, kEx + 12 * n + 4 * k, ex[n][k]);
    REQUIRE(encode(g, rh, {33, 17, RGB}, {90, QUALITY, 0, 0, 2}) == 0);
    i = stream_info(g, rh);
    REQUIRE(i.state == 0);
    CHECK(std::vector<u8>(b + kStream, b + kStream + 8) == std::vector<u8>{0xFF, 0xD8, 0xFF, 0xE1, 0, 12, 'a', 'b'});
    CHECK(std::vector<u8>(b + kStream + 16, b + kStream + 24) ==
        std::vector<u8>{0xFF, 0x78, 0, 5, 'e', 'f', 'g', 0xFF});
    CHECK(g.call("cellJpgEncClose", rh) == 0);
}

TEST_CASE("cellJpgEnc: the queues, Reset's skipped streams, a buffer too small, the size limit", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    put_rgb(b, smooth(8, 8, 1), RGB, 24);
    CHECK(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == kEmpty);
    for (u32 i = 0; i < 8; i++) {
        Pic p{8, 8, RGB};
        p.user = 0x100 + i;
        REQUIRE(encode(g, h, p, {50, FAST}) == 0);
    }
    CHECK(g.call("cellJpgEncWaitForInput", h, 0) == kBusy);
    CHECK(encode(g, h, {8, 8, RGB}, {50, FAST}) == kBusy);
    REQUIRE(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == 0);
    CHECK(ps_read32(b, kNum) == 4);
    REQUIRE(g.call("cellJpgEncReset", h) == 0);
    REQUIRE(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == 0);
    CHECK(ps_read32(b, kNum) == 8);
    CHECK(g.call("cellJpgEncReset", h) == kBusy);
    for (u32 i = 0; i < 8; i++) {
        const Info info = stream_info(g, h);
        CHECK(info.user == 0x100 + i);
        CHECK(info.state == (i < 4 ? 0 : kSkip));
        CHECK(info.quality == 50);
    }
    REQUIRE(encode(g, h, {8, 8, RGB}, {50, FAST}) == 0);
    CHECK(stream_info(g, h).state == kSkip);
    REQUIRE(encode(g, h, {8, 8, RGB}, {50, FAST}) == 0);
    CHECK(stream_info(g, h).state == 0);

    // A buffer too small: STREAM_OVERFLOW, streamSize 0, the rows encoded before the write that filled it; the buffer
    // filled to its end.
    put_rgb(b, noisy(64, 48, 3), RGB, 192);
    CHECK(encode(g, h, {64, 48, RGB}, {100}, 0xFFF) == kArg);
    std::memset(b + kStream, 0xEE, 0x1100);
    REQUIRE(encode(g, h, {64, 48, RGB}, {100}, 0x1000) == 0);
    Info info = stream_info(g, h);
    CHECK(info.state == kOverflow);
    CHECK(info.size == 0);
    CHECK(info.lines % 16 == 15);
    CHECK(ps_read8(b, kStream + 0x1000) == 0xEE);
    // The stream size limit mode: a quality up to the one given whose stream fits.
    REQUIRE(encode(g, h, {64, 48, RGB}, {100, QUALITY, 0, 1}, 0x1000) == 0);
    info = stream_info(g, h);
    CHECK(info.state == 0);
    CHECK(info.size <= 0x1000);
    CHECK(info.quality < 100);
    CHECK(info.quality > 5);
    CHECK(encode(g, h, {64, 48, RGB}, {95, FAST, 0, 1}, 0, 0) == kArg); // to a file: refused
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: on the SPUs, libjpgenc's SPU encoder's arithmetic and its output buffer (D-913)",
    "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_config(b, 64, 64, YCBCR, S420, true);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(64, YCBCR, S420, true));
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    REQUIRE(g.call("cellJpgEncOpen", kCfg, kRsrc, kHandle) == 0);
    const u32 h = ps_read32(b, kHandle);
    const auto stream = [&](u32 size) { return std::vector<u8>(b + kStream, b + kStream + size); };
    // RGB (0, 0, 250): Y 28 in libjpeg's fixed point, 29 in the SPU program's single precision.
    for (u32 i = 0; i < 16 * 16; i++) {
        b[kPixels + 3 * i] = 0;
        b[kPixels + 3 * i + 1] = 0;
        b[kPixels + 3 * i + 2] = 250;
    }
    REQUIRE(encode(g, h, {16, 16, RGB}, {100, FAST, 0, 0, 0, true}) == 0);
    Info i = stream_info(g, h);
    REQUIRE(i.state == 0);
    // The stream counts two bytes past EOI, what the SPU's buffer held there: zeros at first.
    std::vector<u8> first = stream(i.size);
    REQUIRE(first.size() > 4);
    CHECK(first[first.size() - 4] == 0xFF);
    CHECK(first[first.size() - 3] == 0xD9);
    CHECK(first[first.size() - 2] == 0);
    CHECK(first[first.size() - 1] == 0);
    Decoded d = read_jpeg(std::vector<u8>(first.begin(), first.end() - 2), jpeg::ColorSpace::Gray);
    REQUIRE(d.ok);
    CHECK(d.pixels[0] == 29);
    REQUIRE(encode(g, h, {16, 16, RGB}, {100, FAST}) == 0); // the same encoder, on the PPU
    i = stream_info(g, h);
    d = read_jpeg(stream(i.size), jpeg::ColorSpace::Gray);
    REQUIRE(d.ok);
    CHECK(d.pixels[0] == 28);
    // A longer picture (noise past the SPU buffer's first 4094 bytes) ends at EOI; a shorter one after it ends with the
    // bytes the longer one left in the buffer.
    u32 seed = 1;
    for (u32 k = 0; k < 64 * 48 * 3; k++) {
        seed = seed * 1103515245u + 12345u;
        b[kPixels + k] = static_cast<u8>(seed >> 16);
    }
    REQUIRE(encode(g, h, {64, 48, RGB}, {100, FAST, 0, 0, 0, true}) == 0);
    i = stream_info(g, h);
    REQUIRE(i.state == 0);
    REQUIRE(i.size > 4096);
    const std::vector<u8> longer = stream(i.size);
    CHECK(longer[longer.size() - 2] == 0xFF);
    CHECK(longer[longer.size() - 1] == 0xD9);
    for (u32 k = 0; k < 16 * 16 * 3; k++) b[kPixels + k] = static_cast<u8>(k % 3 == 2 ? 250 : 0);
    REQUIRE(encode(g, h, {16, 16, RGB}, {90, FAST, 0, 0, 0, true}) == 0);
    i = stream_info(g, h);
    REQUIRE(i.state == 0);
    const std::vector<u8> after = stream(i.size);
    const usize n = after.size() - 2; // past the first 4094 bytes the buffer starts again: the longer one's 4094 + n
    CHECK(after[n] == longer[4094 + n]);
    CHECK(after[n + 1] == longer[4095 + n]);
    // The SPU sends 4096 bytes at a time only while the buffer has room past them: in 0x1000 bytes nothing goes, and
    // the stream overflows with every row encoded.
    REQUIRE(encode(g, h, {16, 16, RGB}, {90, FAST, 0, 0, 0, true}, 0x1000) == 0);
    i = stream_info(g, h);
    CHECK(i.state == kOverflow);
    CHECK(i.size == 0);
    CHECK(i.lines == 16);
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: to a file on /dev_hdd0, and one that cannot be made", "[hle][jpgenc]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "pspocket-test-jpgenc";
    fs::remove_all(root);
    fs::create_directories(root / "hdd0" / "tmp");
    Fixture f;
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    put_rgb(b, smooth(16, 16, 4), RGB, 48);
    std::memcpy(b + kName, "/dev_hdd0/tmp/shot.jpg", 23);
    REQUIRE(encode(g, h, {16, 16, RGB}, {80}, 0, 0) == 0);
    Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.location == 0);
    CHECK(i.name == kName);
    std::ifstream file(root / "hdd0" / "tmp" / "shot.jpg", std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() == i.size);
    CHECK(read_jpeg(bytes, jpeg::ColorSpace::RGB).ok);
    std::memcpy(b + kName, "/dev_nowhere/x.jpg", 19);
    REQUIRE(encode(g, h, {16, 16, RGB}, {80}, 0, 0) == 0);
    i = stream_info(g, h);
    CHECK(i.state == kFileOpen);
    CHECK(i.quality == 80);
    CHECK(g.call("cellJpgEncClose", h) == 0);
    fs::remove_all(root);
}

TEST_CASE("cellJpgEnc: EncodePicture's checks", "[hle][jpgenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const Pic good{8, 8, RGB};
    CHECK(encode(g, h + 1, good, {}) == kArg);
    CHECK(g.call("cellJpgEncEncodePicture", h, 0, kParam, kOutParam) == kArg);
    CHECK(encode(g, h, {0, 8, RGB}, {}) == kArg);
    CHECK(encode(g, h, {65, 8, RGB}, {}) == kArg);
    CHECK(encode(g, h, {8, 8, 4}, {}) == kArg);
    CHECK(encode(g, h, {8, 8, GRAY}, {}) == kArg);
    CHECK(encode(g, h, {8, 8, RGB, 23}, {}) == kArg); // a pitch shorter than a row
    CHECK(encode(g, h, good, {101}) == kArg);
    CHECK(encode(g, h, good, {75, 1}) == kArg); // DCT methods 0 and 5 only
    CHECK(encode(g, h, good, {75, FAST, 0, 2}) == kArg);
    CHECK(encode(g, h, good, {75, FAST, 0x10000}) == kArg);
    CHECK(encode(g, h, good, {75, FAST, 0, 0, 0, true}) == kArg); // SPUs: not opened for them
    ps_write32(b, kEx, 0xFFE1);
    ps_write32(b, kEx + 4, kExData);
    ps_write32(b, kEx + 8, 0xFFFE);
    CHECK(encode(g, h, good, {75, FAST, 0, 0, 1}) == kArg);
    ps_write32(b, kEx + 8, 10);
    CHECK(encode(g, h, good, {75, FAST, 0, 0, 1}, 0x100D) == kArg); // 0x1000 + the segment's 14
    REQUIRE(encode(g, h, good, {75, FAST, 0, 0, 1}, 0x100E) == 0);
    CHECK(stream_info(g, h).state == 0);
    CHECK(encode(g, h, good, {}, 0x1000, 2) == kArg);
    CHECK(encode(g, h, good, {}, 0) == kArg);
    REQUIRE(encode(g, h, good, {}) == 0);
    CHECK(stream_info(g, h).state == 0);
    CHECK(g.call("cellJpgEncWaitForOutput", h, kNum, 0) == kEmpty); // nothing refused was queued
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

// ------------------------------------------------------------------------------------------------- threads (D-650, D-651)

namespace {

constexpr u32 kCalls = kData + 0x1F0000; // per-thread parameter blocks, 0x100 apart

// EncodePicture2 with its structures at `at` (a thread's own), the pixels at `pixels`, the stream to `stream`.
u64 encode_at(GuestThread& g, u32 handle, u32 at, const Pic& p, const Param& e, u32 pixels, u32 stream, u32 limit)
{
    u8* b = g.f.base();
    const u32 pic = at, param = at + 0x40, out = at + 0x80;
    std::memset(b + pic, 0, 32);
    ps_write32(b, pic, p.width);
    ps_write32(b, pic + 4, p.height);
    ps_write32(b, pic + 8, p.pitch);
    ps_write32(b, pic + 12, p.space);
    ps_write32(b, pic + 16, pixels);
    ps_write64(b, pic + 24, p.user);
    std::memset(b + param, 0, 28);
    ps_write32(b, param + 4, e.restart);
    ps_write32(b, param + 8, e.method);
    ps_write32(b, param + 16, e.quality);
    ps_write32(b, out, 1);
    ps_write32(b, out + 4, 0);
    ps_write32(b, out + 8, stream);
    ps_write32(b, out + 12, limit);
    return g.call("cellJpgEncEncodePicture2", handle, pic, param, out);
}

bool info_at(GuestThread& g, u32 handle, u32 at, Info& i)
{
    u8* b = g.f.base();
    const u32 info = at + 0xC0;
    if (g.call("cellJpgEncGetStreamInfo", handle, info) != 0) return false;
    i = {static_cast<s32>(ps_read32(b, info)), ps_read32(b, info + 4), ps_read32(b, info + 8), ps_read32(b, info + 12),
        ps_read32(b, info + 16), ps_read32(b, info + 20), ps_read32(b, info + 24), ps_read32(b, info + 28),
        ps_read64(b, info + 32)};
    return true;
}

u32 open_at(GuestThread& g, u32 at, u32 work, u32 max_w, u32 max_h)
{
    u8* b = g.f.base();
    const u32 cfg = at, rsrc = at + 0x40, handle = at + 0x80;
    std::memset(b + cfg, 0, 28);
    ps_write32(b, cfg, max_w);
    ps_write32(b, cfg + 4, max_h);
    ps_write32(b, cfg + 8, YCBCR);
    ps_write32(b, cfg + 12, S420);
    ps_write32(b, rsrc, work);
    ps_write32(b, rsrc + 4, needed(max_w, YCBCR, S420));
    ps_write32(b, rsrc + 8, 1000);
    ps_write32(b, rsrc + 12, 200);
    if (g.call("cellJpgEncOpen", cfg, rsrc, handle) != 0) return 0;
    return ps_read32(b, handle);
}

} // namespace

TEST_CASE("cellJpgEnc: two encoders on two threads at once, each stream the one an encoder alone makes (D-650)",
    "[hle][jpgenc]")
{
    constexpr u32 kRounds = 8, kW = 96, kH = 64, kLimit = 0x10000;
    constexpr u32 kPictures[2] = {kPixels, kPixels + 0x20000}, kWorks[2] = {kWork, kWork + 0x40000},
                  kStreams[2] = {kStream, kStream + kLimit};
    Fixture f;
    u8* b = f.base();
    const Source s0 = smooth(kW, kH, 5), s1 = smooth(kW, kH, 41);
    for (u32 y = 0; y < kH; y++) {
        std::memcpy(b + kPictures[0] + y * kW * 3, s0.rgb.data() + usize{y} * kW * 3, kW * 3);
        for (u32 x = 0; x < kW; x++) {
            std::memcpy(b + kPictures[1] + (y * kW + x) * 4, s1.rgb.data() + (usize{y} * kW + x) * 3, 3);
            b[kPictures[1] + (y * kW + x) * 4 + 3] = static_cast<u8>(x * 31 + y);
        }
    }
    const auto picture = [&](u32 t, u32 round) {
        Pic p{kW, kH, t ? RGBA : RGB, kW * (t ? 4 : 3)};
        p.user = 0x1000 * (t + 1) + round;
        return p;
    };
    const auto settings = [](u32 round) {
        Param e;
        e.quality = 40 + 7 * round;
        e.method = round % 2 ? FAST : QUALITY;
        e.restart = round % 3;
        return e;
    };
    // What an encoder alone makes of each picture in each round.
    std::vector<u8> want[2][kRounds];
    {
        GuestThread g(f, 0, "alone");
        const u32 h = open_at(g, kCalls, kWorks[0], kW, kH);
        REQUIRE(h != 0);
        for (u32 t = 0; t < 2; t++)
            for (u32 r = 0; r < kRounds; r++) {
                REQUIRE(encode_at(g, h, kCalls, picture(t, r), settings(r), kPictures[t], kStreams[0], kLimit) == 0);
                Info i{};
                REQUIRE(info_at(g, h, kCalls, i));
                REQUIRE(i.state == 0);
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
            const u32 h = open_at(g, at, kWorks[t], kW, kH);
            if (h == 0) {
                wrong++;
                return;
            }
            for (u32 r = 0; r < kRounds; r++) {
                const u64 encoded = encode_at(g, h, at, picture(t, r), settings(r), kPictures[t], kStreams[t], kLimit);
                const u64 waited = g.call("cellJpgEncWaitForOutput", h, at + 0xF0, 1);
                Info i{};
                const bool taken = info_at(g, h, at, i);
                const bool same = encoded == 0 && waited == 0 && taken && i.state == 0 && i.addr == kStreams[t] &&
                    i.user == picture(t, r).user && i.size == want[t][r].size() &&
                    std::memcmp(b + kStreams[t], want[t][r].data(), i.size) == 0;
                if (!same) wrong++;
                rounds++;
            }
            if (g.call("cellJpgEncClose", h) != 0) wrong++;
        });
    }
    for (std::thread& th : threads) th.join();
    CHECK(rounds == 2 * kRounds);
    CHECK(wrong == 0);
}

TEST_CASE("cellJpgEnc: eight threads encoding with one encoder at once: each picture queued once or refused; a Reset "
          "meanwhile skips the waiting ones, never one being encoded (D-651, D-913)", "[hle][jpgenc]")
{
    constexpr u32 kThreads = 8, kW = 128, kH = 96, kLimit = 0x8000;
    static_assert(kStream + kThreads * kLimit <= kWork);
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const Source src = smooth(kW, kH, 17);
    std::memcpy(b + kPixels, src.rgb.data(), src.rgb.size());
    const u32 h = open_at(g, kCalls, kWork, kW, kH);
    REQUIRE(h != 0);
    const Pic pic{kW, kH, RGB, kW * 3};
    REQUIRE(encode_at(g, h, kCalls, pic, {}, kPixels, kStream, kLimit) == 0);
    Info alone{};
    REQUIRE(info_at(g, h, kCalls, alone));
    REQUIRE(alone.state == 0);
    const std::vector<u8> want(b + kStream, b + kStream + alone.size);

    for (const bool reset : {false, true}) {
        INFO("reset " << reset);
        std::atomic<u32> ready{0};
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
                answers[t] = encode_at(o, h, kCalls + 0x100 * (t + 1), p, {}, kPixels, kStream + kLimit * t, kLimit);
            });
        }
        while (ready.load() < kThreads) std::this_thread::yield();
        go = true;
        if (reset) {
            while (std::none_of(answers.begin(), answers.end(), [](const std::atomic<u64>& a) { return a == 0; }))
                std::this_thread::yield();
            REQUIRE(g.call("cellJpgEncReset", h) == 0);
        }
        for (std::thread& th : threads) th.join();
        std::vector<u64> accepted;
        u32 refused = 0;
        for (u32 t = 0; t < kThreads; t++) {
            if (answers[t] == 0) accepted.push_back(0x700 + t);
            else if (answers[t] == kBusy) refused++;
        }
        CHECK(accepted.size() >= 4);
        CHECK(accepted.size() + refused == kThreads);
        std::vector<u64> users;
        for (usize n = 0; n < accepted.size(); n++) {
            REQUIRE(g.call("cellJpgEncWaitForOutput", h, kCalls + 0xF0, 1) == 0);
            Info s{};
            REQUIRE(info_at(g, h, kCalls, s));
            users.push_back(s.user);
            const u64 t = s.user - 0x700;
            REQUIRE(t < kThreads);
            if (s.state != 0) {
                CHECK(reset);
                CHECK(s.state == kSkip);
                continue;
            }
            CHECK(s.addr == kStream + kLimit * t);
            REQUIRE(s.size == want.size());
            CHECK(std::memcmp(b + s.addr, want.data(), want.size()) == 0);
        }
        std::sort(users.begin(), users.end());
        CHECK(users == accepted);
        CHECK(g.call("cellJpgEncGetStreamInfo", h, kInfo) == kEmpty);
        CHECK(g.call("cellJpgEncWaitForInput", h, 0) == 0);
        if (reset) {
            REQUIRE(encode_at(g, h, kCalls, pic, {}, kPixels, kStream, kLimit) == 0);
            Info s{};
            REQUIRE(info_at(g, h, kCalls, s));
            CHECK((s.state == 0 || s.state == kSkip));
        }
    }
    CHECK(g.call("cellJpgEncClose", h) == 0);
}

TEST_CASE("cellJpgEnc: the size limit mode with libjpgenc's tables from the installed system software (D-913)",
    "[hle][jpgenc]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH: the size limit mode falls back to a search without libjpgenc's tables");
    Fixture f;
    REQUIRE(f.kernel->vfs_mount("/dev_flash", *flash, true));
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    put_rgb(b, noisy(64, 48, 3), RGB, 192);
    // libjpgenc aims at a share of the limit from its predictions: a quality under the one the stream alone would fit.
    REQUIRE(encode(g, h, {64, 48, RGB}, {95, FAST, 0, 1}, 0x1800) == 0);
    const Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.size < 0x1800);
    CHECK(i.quality < 95);
    CHECK(i.lines == 48);
    CHECK(g.call("cellJpgEncClose", h) == 0);
}
