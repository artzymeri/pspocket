// cellPngEnc (D-566): pspocket's PNG encoder (made from RPCS3's checks; no Sony module to compare with). Synthetic
// pictures in every colour space and depth, encoded and read back with libpng's own decoder pixel by pixel, with their
// ancillary chunks; the result queue; buffer and file output; the checks. No game data.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pspocket/recomp/nid.hpp"

#if PSPOCKET_HAVE_LIBPNG
#include <csetjmp>

#include <png.h>
#endif

using namespace pspocket;
using namespace pspocket::hle::test;

TEST_CASE("cellPngEnc: the nine functions, firmware first", "[hle][pngenc]")
{
#if PSPOCKET_HAVE_LIBPNG
    for (const std::string_view name : {"cellPngEncQueryAttr", "cellPngEncOpen", "cellPngEncOpenEx", "cellPngEncClose",
             "cellPngEncWaitForInput", "cellPngEncEncodePicture", "cellPngEncWaitForOutput", "cellPngEncGetStreamInfo",
             "cellPngEncReset"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellPngEnc", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
    CHECK(hle::registry().is_firmware_first("cellPngEnc"));
#else
    SKIP("no libpng in this build: cellPngEnc is not pspocket's");
#endif
}

#if PSPOCKET_HAVE_LIBPNG

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kPic = kData + 0x200, kParam = kData + 0x240, kOutParam = kData + 0x280, kInfo = kData + 0x2C0,
              kNum = kData + 0x300, kChunks = kData + 0x340, kChunkData = kData + 0x400, kName = kData + 0x800,
              kPixels = kData + 0x1000, kStream = kData + 0x20000, kWork = kData + 0x200000;
const u64 kArg = r3_of_code(0x80611291), kBusy = r3_of_code(0x80611293), kEmpty = r3_of_code(0x80611294);
constexpr s32 kOverflow = static_cast<s32>(0x806112A3), kFileOpen = static_cast<s32>(0x806112A4);
enum : u32 { GRAY = 1, RGB = 2, PALETTE = 4, GRAY_ALPHA = 9, RGBA = 10, ARGB = 20 };

void write_config(u8* b, u32 max_w, u32 max_h, u32 depth, bool spu = false, u32 add = 0)
{
    std::memset(b + kCfg, 0, 28);
    ps_write32(b, kCfg, max_w);
    ps_write32(b, kCfg + 4, max_h);
    ps_write32(b, kCfg + 8, depth);
    ps_write8(b, kCfg + 12, spu ? 1 : 0);
    ps_write32(b, kCfg + 16, add);
}

u32 needed(u32 max_w, u32 depth, bool spu = false, u32 add = 0)
{
    return add + (spu ? 0x78200 : 0x47A00) + (depth >> 1) * max_w * 7 + 0x1780;
}

u32 open(GuestThread& g, u32 max_w = 64, u32 max_h = 64, u32 depth = 16)
{
    u8* b = g.f.base();
    write_config(b, max_w, max_h, depth);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(max_w, depth));
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    REQUIRE(g.call("cellPngEncOpen", kCfg, kRsrc, kHandle) == 0);
    return ps_read32(b, kHandle);
}

struct Pic {
    u32 width = 0, height = 0, pitch = 0, space = RGBA, depth = 8;
    bool packed = false;
    u64 user = 0;
};

struct Param {
    u32 space = RGBA, level = 6, filters = 0xF8, chunk_count = 0;
};

u64 encode(GuestThread& g, u32 handle, const Pic& p, const Param& e, u32 limit = 0x10000, u32 location = 1,
    u32 name = 0)
{
    u8* b = g.f.base();
    std::memset(b + kPic, 0, 40);
    ps_write32(b, kPic, p.width);
    ps_write32(b, kPic + 4, p.height);
    ps_write32(b, kPic + 8, p.pitch);
    ps_write32(b, kPic + 12, p.space);
    ps_write32(b, kPic + 16, p.depth);
    ps_write8(b, kPic + 20, p.packed ? 1 : 0);
    ps_write32(b, kPic + 24, kPixels);
    ps_write64(b, kPic + 32, p.user);
    std::memset(b + kParam, 0, 24);
    ps_write32(b, kParam + 4, e.space);
    ps_write32(b, kParam + 8, e.level);
    ps_write32(b, kParam + 12, e.filters);
    ps_write32(b, kParam + 16, kChunks);
    ps_write32(b, kParam + 20, e.chunk_count);
    ps_write32(b, kOutParam, location);
    ps_write32(b, kOutParam + 4, name);
    ps_write32(b, kOutParam + 8, location == 1 ? kStream : 0);
    ps_write32(b, kOutParam + 12, limit);
    return g.call("cellPngEncEncodePicture", handle, kPic, kParam, kOutParam);
}

struct Info {
    s32 state;
    u32 location, name, addr, limit, size, lines;
    u64 user;
};

Info stream_info(GuestThread& g, u32 handle)
{
    u8* b = g.f.base();
    REQUIRE(g.call("cellPngEncGetStreamInfo", handle, kInfo) == 0);
    return {static_cast<s32>(ps_read32(b, kInfo)), ps_read32(b, kInfo + 4), ps_read32(b, kInfo + 8),
        ps_read32(b, kInfo + 12), ps_read32(b, kInfo + 16), ps_read32(b, kInfo + 20), ps_read32(b, kInfo + 24),
        ps_read64(b, kInfo + 32)};
}

// ---- libpng's reader

struct Png {
    u32 width = 0, height = 0;
    int depth = 0, color_type = 0;
    std::vector<std::vector<u8>> rows; // one byte a sample below 8 bits (png_set_packing)
    std::vector<std::array<u8, 3>> palette;
    std::vector<u8> trans;
    std::vector<std::pair<std::string, std::string>> texts;
    bool has_time = false, has_gama = false, has_phys = false;
    png_time time{};
    double gamma = 0;
    png_uint_32 phys_x = 0, phys_y = 0;
    int phys_unit = 0;
};

struct Cursor {
    const u8* data;
    size_t size, at = 0;
};

void read_fn(png_structp png, png_bytep out, size_t n)
{
    auto* c = static_cast<Cursor*>(png_get_io_ptr(png));
    if (c->at + n > c->size) png_error(png, "short");
    std::memcpy(out, c->data + c->at, n);
    c->at += n;
}

void fail_fn(png_structp png, png_const_charp) { png_longjmp(png, 1); }
void warn_fn(png_structp, png_const_charp) {}

bool read_png(const u8* data, size_t size, Png& out)
{
    Cursor c{data, size};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, fail_fn, warn_fn);
    png_infop info = png_create_info_struct(png);
    volatile bool ok = false;
    if (!setjmp(png_jmpbuf(png))) {
        png_set_read_fn(png, &c, read_fn);
        png_read_info(png, info);
        out.width = png_get_image_width(png, info);
        out.height = png_get_image_height(png, info);
        out.depth = png_get_bit_depth(png, info);
        out.color_type = png_get_color_type(png, info);
        png_set_packing(png);
        png_read_update_info(png, info);
        const size_t rb = png_get_rowbytes(png, info);
        out.rows.assign(out.height, std::vector<u8>(rb));
        std::vector<png_bytep> ptrs;
        for (auto& r : out.rows) ptrs.push_back(r.data());
        png_read_image(png, ptrs.data());
        png_read_end(png, info);
        png_colorp pal = nullptr;
        int num = 0;
        if (png_get_PLTE(png, info, &pal, &num))
            for (int i = 0; i < num; i++) out.palette.push_back({pal[i].red, pal[i].green, pal[i].blue});
        png_bytep alpha = nullptr;
        png_color_16p colour = nullptr;
        if (png_get_tRNS(png, info, &alpha, &num, &colour) && alpha) out.trans.assign(alpha, alpha + num);
        png_textp text = nullptr;
        num = png_get_text(png, info, &text, nullptr);
        for (int i = 0; i < num; i++) out.texts.emplace_back(text[i].key, text[i].text);
        png_timep t = nullptr;
        if (png_get_tIME(png, info, &t)) {
            out.has_time = true;
            out.time = *t;
        }
        out.has_gama = png_get_gAMA(png, info, &out.gamma) != 0;
        out.has_phys = png_get_pHYs(png, info, &out.phys_x, &out.phys_y, &out.phys_unit) != 0;
        ok = true;
    }
    png_destroy_read_struct(&png, &info, nullptr);
    return ok;
}

// Encodes, takes the one result, reads it back.
Png round_trip(GuestThread& g, u32 handle, const Pic& p, const Param& e)
{
    u8* b = g.f.base();
    REQUIRE(encode(g, handle, p, e) == 0);
    REQUIRE(g.call("cellPngEncWaitForOutput", handle, kNum, 1) == 0);
    REQUIRE(ps_read32(b, kNum) == 1);
    const Info i = stream_info(g, handle);
    REQUIRE(i.state == 0);
    CHECK(i.lines == p.height);
    Png png;
    REQUIRE(read_png(b + kStream, i.size, png));
    CHECK(png.width == p.width);
    CHECK(png.height == p.height);
    return png;
}

} // namespace

TEST_CASE("cellPngEnc: QueryAttr's memory and the checks of Open and Close", "[hle][pngenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_config(b, 640, 480, 8, false, 0x100);
    REQUIRE(g.call("cellPngEncQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == needed(640, 8, false, 0x100));
    CHECK(ps_read8(b, kAttr + 4) == 4);           // cmdQueueDepth
    CHECK(ps_read32(b, kAttr + 8) == 0x270000);   // versionUpper
    CHECK(ps_read32(b, kAttr + 12) == 0);         // versionLower
    write_config(b, 640, 480, 16, true);
    REQUIRE(g.call("cellPngEncQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == needed(640, 16, true));
    CHECK(g.call("cellPngEncQueryAttr", kCfg, 0) == kArg);
    CHECK(g.call("cellPngEncQueryAttr", 0, kAttr) == kArg);
    write_config(b, 640, 480, 12); // 8 or 16 bits only
    CHECK(g.call("cellPngEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 0, 480, 8);
    CHECK(g.call("cellPngEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 1000001, 480, 8);
    CHECK(g.call("cellPngEncQueryAttr", kCfg, kAttr) == kArg);
    write_config(b, 640, 480, 8);
    ps_write32(b, kCfg + 24, 1); // extended parameters
    CHECK(g.call("cellPngEncQueryAttr", kCfg, kAttr) == kArg);

    // Open: the handle is the work memory; too little of it, priorities out of range, or memory already open: ARG.
    write_config(b, 64, 64, 8);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(64, 8) - 1);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 200);
    CHECK(g.call("cellPngEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 4, needed(64, 8));
    ps_write32(b, kRsrc + 8, 0xC00);
    CHECK(g.call("cellPngEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write32(b, kRsrc + 12, 0x100);
    CHECK(g.call("cellPngEncOpen", kCfg, kRsrc, kHandle) == kArg);
    ps_write32(b, kRsrc + 12, 200);
    CHECK(g.call("cellPngEncOpen", kCfg, kRsrc, 0) == kArg);
    REQUIRE(g.call("cellPngEncOpen", kCfg, kRsrc, kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == kWork);
    CHECK(g.call("cellPngEncOpen", kCfg, kRsrc, kHandle) == kArg);
    CHECK(g.call("cellPngEncClose", kWork) == 0);
    CHECK(g.call("cellPngEncClose", kWork) == kArg);
    CHECK(g.call("cellPngEncClose", 0) == kArg);
    // OpenEx: a SPURS instance and eight priorities of at most 15.
    std::memset(b + kRsrc, 0, 24);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, needed(64, 8));
    ps_write32(b, kRsrc + 8, 1000);
    ps_write8(b, kRsrc + 16 + 3, 16);
    CHECK(g.call("cellPngEncOpenEx", kCfg, kRsrc, kHandle) == kArg);
    ps_write8(b, kRsrc + 16 + 3, 15);
    REQUIRE(g.call("cellPngEncOpenEx", kCfg, kRsrc, kHandle) == 0);
    CHECK(g.call("cellPngEncClose", kWork) == 0);
}

TEST_CASE("cellPngEnc: pictures in every colour space and depth, read back with libpng", "[hle][pngenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    const auto fill = [&](u32 bytes, u32 seed) {
        for (u32 i = 0; i < bytes; i++) ps_write8(b, kPixels + i, static_cast<u8>(i * 37 + seed));
    };
    const auto px = [&](u32 at) { return ps_read8(b, kPixels + at); };

    SECTION("RGBA to RGBA, rows padded, every filter and zlib's strongest level")
    {
        Pic p{5, 3, 24, RGBA, 8};
        fill(24 * 3, 1);
        const Png png = round_trip(g, h, p, {RGBA, 9, 0xF8});
        CHECK(png.color_type == PNG_COLOR_TYPE_RGB_ALPHA);
        CHECK(png.depth == 8);
        for (u32 y = 0; y < 3; y++)
            for (u32 x = 0; x < 20; x++) CHECK(png.rows[y][x] == px(y * 24 + x));
    }
    SECTION("ARGB to RGBA (reordered) and to RGB (alpha dropped)")
    {
        Pic p{4, 2, 16, ARGB, 8};
        fill(32, 7);
        Png png = round_trip(g, h, p, {ARGB, 0, 0});
        CHECK(png.color_type == PNG_COLOR_TYPE_RGB_ALPHA);
        for (u32 y = 0; y < 2; y++)
            for (u32 x = 0; x < 4; x++) {
                const u32 at = y * 16 + 4 * x;
                CHECK(png.rows[y][4 * x] == px(at + 1));
                CHECK(png.rows[y][4 * x + 1] == px(at + 2));
                CHECK(png.rows[y][4 * x + 2] == px(at + 3));
                CHECK(png.rows[y][4 * x + 3] == px(at));
            }
        png = round_trip(g, h, p, {RGB, 6, 0x10});
        CHECK(png.color_type == PNG_COLOR_TYPE_RGB);
        for (u32 x = 0; x < 4; x++) CHECK(png.rows[1][3 * x + 2] == px(16 + 4 * x + 3));
    }
    SECTION("RGB to RGBA (opaque), 16-bit RGBA, gray to gray and alpha and back")
    {
        Pic p{3, 2, 9, RGB, 8};
        fill(18, 3);
        Png png = round_trip(g, h, p, {RGBA, 6, 0x20});
        for (u32 x = 0; x < 3; x++) {
            CHECK(png.rows[1][4 * x] == px(9 + 3 * x));
            CHECK(png.rows[1][4 * x + 3] == 0xFF);
        }
        p = {2, 2, 16, RGBA, 16};
        fill(32, 5);
        png = round_trip(g, h, p, {RGBA, 6, 0x40});
        CHECK(png.depth == 16);
        for (u32 i = 0; i < 16; i++) CHECK(png.rows[1][i] == px(16 + i)); // big-endian samples as they are
        p = {4, 1, 4, GRAY, 8};
        fill(4, 9);
        png = round_trip(g, h, p, {GRAY_ALPHA, 6, 0x80});
        CHECK(png.color_type == PNG_COLOR_TYPE_GRAY_ALPHA);
        for (u32 x = 0; x < 4; x++) {
            CHECK(png.rows[0][2 * x] == px(x));
            CHECK(png.rows[0][2 * x + 1] == 0xFF);
        }
        p = {4, 1, 8, GRAY_ALPHA, 8};
        png = round_trip(g, h, p, {GRAY, 6, 0xF8});
        CHECK(png.color_type == PNG_COLOR_TYPE_GRAY);
        for (u32 x = 0; x < 4; x++) CHECK(png.rows[0][x] == px(2 * x));
    }
    SECTION("4-bit gray, one sample a byte and packed; 1-bit gray")
    {
        Pic p{5, 2, 8, GRAY, 4};
        for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(i % 16));
        Png png = round_trip(g, h, p, {GRAY, 6, 0x08});
        CHECK(png.depth == 4);
        for (u32 y = 0; y < 2; y++)
            for (u32 x = 0; x < 5; x++) CHECK(png.rows[y][x] == (y * 8 + x) % 16);
        p.packed = true;
        p.pitch = 3;
        const u8 packed[6] = {0x12, 0x34, 0x50, 0xAB, 0xCD, 0xE0};
        std::memcpy(b + kPixels, packed, 6);
        png = round_trip(g, h, p, {GRAY, 6, 0x08});
        const u8 want[2][5] = {{1, 2, 3, 4, 5}, {0xA, 0xB, 0xC, 0xD, 0xE}};
        for (u32 y = 0; y < 2; y++)
            for (u32 x = 0; x < 5; x++) CHECK(png.rows[y][x] == want[y][x]);
        p = {10, 1, 2, GRAY, 1, true};
        ps_write8(b, kPixels, 0xA5);
        ps_write8(b, kPixels + 1, 0x80);
        png = round_trip(g, h, p, {GRAY, 6, 0x08});
        const u8 bits[10] = {1, 0, 1, 0, 0, 1, 0, 1, 1, 0};
        for (u32 x = 0; x < 10; x++) CHECK(png.rows[0][x] == bits[x]);
    }
    SECTION("a palette picture with its PLTE and tRNS")
    {
        Pic p{3, 2, 3, PALETTE, 8};
        const u8 idx[6] = {0, 1, 2, 2, 1, 0};
        std::memcpy(b + kPixels, idx, 6);
        // CellPngPLTE {3 entries, at kChunkData + 0x40}; CellPngTRNS {alphas at kChunkData + 0x80, 3, ...}.
        ps_write32(b, kChunkData, 3);
        ps_write32(b, kChunkData + 4, kChunkData + 0x40);
        const u8 rgb[9] = {255, 0, 0, 0, 255, 0, 0, 0, 255};
        std::memcpy(b + kChunkData + 0x40, rgb, 9);
        std::memset(b + kChunkData + 0x20, 0, 16);
        ps_write32(b, kChunkData + 0x20, kChunkData + 0x80);
        ps_write32(b, kChunkData + 0x24, 3);
        const u8 alpha[3] = {0, 128, 255};
        std::memcpy(b + kChunkData + 0x80, alpha, 3);
        ps_write32(b, kChunks, 0); // PLTE
        ps_write32(b, kChunks + 4, kChunkData);
        ps_write32(b, kChunks + 8, 1); // tRNS
        ps_write32(b, kChunks + 12, kChunkData + 0x20);
        const Png png = round_trip(g, h, p, {PALETTE, 6, 0x08, 2});
        CHECK(png.color_type == PNG_COLOR_TYPE_PALETTE);
        REQUIRE(png.palette.size() == 3);
        CHECK(png.palette[2] == std::array<u8, 3>{0, 0, 255});
        CHECK(png.trans == std::vector<u8>{0, 128, 255});
        for (u32 i = 0; i < 6; i++) CHECK(png.rows[i / 3][i % 3] == idx[i]);
        // Without its PLTE, a palette picture's stream aborts.
        REQUIRE(encode(g, h, p, {PALETTE, 6, 0x08, 0}) == 0);
        CHECK(stream_info(g, h).state == static_cast<s32>(0x806112A1));
    }
    CHECK(g.call("cellPngEncClose", h) == 0);
}

TEST_CASE("cellPngEnc: ancillary chunks: text, time, gamma, physical size", "[hle][pngenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    std::memset(b + kPixels, 0x40, 64);
    std::memset(b + kChunkData, 0, 0x200);
    // CellPngTextInfo {tEXt, keyword, text, textLength 0 (NUL-terminated)}.
    std::memcpy(b + kChunkData + 0x100, "Title", 6);
    std::memcpy(b + kChunkData + 0x120, "pspocket", 9);
    ps_write32(b, kChunkData, 0);
    ps_write32(b, kChunkData + 4, kChunkData + 0x100);
    ps_write32(b, kChunkData + 8, kChunkData + 0x120);
    // CellPngTIME {2026, 10, 9, 12, 34, 56}; CellPngGAMA {0.45455}; CellPngPHYS {2835, 2835, metre}.
    ps_write16(b, kChunkData + 0x40, 2026);
    const u8 when[5] = {10, 9, 12, 34, 56};
    std::memcpy(b + kChunkData + 0x42, when, 5);
    ps_writef64(b, kChunkData + 0x50, 0.45455);
    ps_write32(b, kChunkData + 0x60, 2835);
    ps_write32(b, kChunkData + 0x64, 2835);
    ps_write32(b, kChunkData + 0x68, 1);
    const u32 chunks[][2] = {{7, kChunkData}, {12, kChunkData + 0x40}, {3, kChunkData + 0x50}, {10, kChunkData + 0x60},
        {11, kChunkData + 0x80}}; // and an sPLT, left out
    for (u32 i = 0; i < 5; i++) {
        ps_write32(b, kChunks + 8 * i, chunks[i][0]);
        ps_write32(b, kChunks + 8 * i + 4, chunks[i][1]);
    }
    const Png png = round_trip(g, h, {4, 4, 4, GRAY, 8}, {GRAY, 6, 0xF8, 5});
    REQUIRE(png.texts.size() == 1);
    CHECK(png.texts[0].first == "Title");
    CHECK(png.texts[0].second == "pspocket");
    REQUIRE(png.has_time);
    CHECK(png.time.year == 2026);
    CHECK(png.time.month == 10);
    CHECK(png.time.second == 56);
    REQUIRE(png.has_gama);
    CHECK(png.gamma == Catch::Approx(0.45455).epsilon(1e-5));
    REQUIRE(png.has_phys);
    CHECK(png.phys_x == 2835);
    CHECK(png.phys_unit == 1);
    CHECK(g.call("cellPngEncClose", h) == 0);
}

TEST_CASE("cellPngEnc: three text chunks in one picture, short keywords and texts (D-598)", "[hle][pngenc]")
{
    // A later text's strings used to move an earlier one's (short strings keep their characters inside them), and
    // libpng then refused the keyword it was pointed at: the stream aborted.
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    std::memset(b + kPixels, 0x40, 64);
    std::memset(b + kChunkData, 0, 0x400);
    const char* strings[] = {"Title", "pspocket", "Comment", "a compressed text", "Author", "en", "Auteur", "nobody"};
    for (u32 i = 0; i < 8; i++) std::memcpy(b + kChunkData + 0x200 + 0x20 * i, strings[i], std::strlen(strings[i]) + 1);
    const auto s = [](u32 i) { return kChunkData + 0x200 + 0x20 * i; };
    // CellPngTextInfo {txtType (tEXt 0, zTXt 1, iTXt 2), keyword, text, textLength, languageTag, translatedKeyword}.
    const u32 texts[3][6] = {{0, s(0), s(1), 0, 0, 0}, {1, s(2), s(3), 0, 0, 0}, {2, s(4), s(7), 0, s(5), s(6)}};
    for (u32 t = 0; t < 3; t++) {
        for (u32 k = 0; k < 6; k++) ps_write32(b, kChunkData + 0x18 * t + 4 * k, texts[t][k]);
        ps_write32(b, kChunks + 8 * t, 7);
        ps_write32(b, kChunks + 8 * t + 4, kChunkData + 0x18 * t);
    }
    const Png png = round_trip(g, h, {4, 4, 4, GRAY, 8}, {GRAY, 6, 0xF8, 3});
    REQUIRE(png.texts.size() == 3);
    CHECK(png.texts[0] == std::pair<std::string, std::string>{"Title", "pspocket"});
    CHECK(png.texts[1] == std::pair<std::string, std::string>{"Comment", "a compressed text"});
    CHECK(png.texts[2] == std::pair<std::string, std::string>{"Author", "nobody"});
    CHECK(g.call("cellPngEncClose", h) == 0);
}

TEST_CASE("cellPngEnc: the result queue: four at most, BUSY, EMPTY, Reset; a buffer too small", "[hle][pngenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    std::memset(b + kPixels, 0x22, 64);
    CHECK(g.call("cellPngEncWaitForOutput", h, kNum, 0) == kEmpty);
    CHECK(ps_read32(b, kNum) == 0);
    CHECK(g.call("cellPngEncGetStreamInfo", h, kInfo) == kEmpty);
    for (u32 i = 0; i < 4; i++) {
        REQUIRE(g.call("cellPngEncWaitForInput", h, 1) == 0);
        Pic p{4, 4, 16, RGBA, 8};
        p.user = 0x100 + i;
        REQUIRE(encode(g, h, p, {RGBA, 6, 0xF8}) == 0);
    }
    CHECK(g.call("cellPngEncWaitForInput", h, 0) == kBusy);
    CHECK(encode(g, h, {4, 4, 16, RGBA, 8}, {RGBA, 6, 0xF8}) == kBusy);
    REQUIRE(g.call("cellPngEncWaitForOutput", h, kNum, 0) == 0);
    CHECK(ps_read32(b, kNum) == 4);
    Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.location == 1);
    CHECK(i.addr == kStream);
    CHECK(i.limit == 0x10000);
    CHECK(i.lines == 4);
    CHECK(i.user == 0x100); // oldest first
    CHECK(i.size > 8);
    CHECK(std::memcmp(b + kStream, "\x89PNG\r\n\x1A\n", 8) == 0);
    CHECK(stream_info(g, h).user == 0x101);
    CHECK(g.call("cellPngEncWaitForInput", h, 0) == 0);
    REQUIRE(g.call("cellPngEncReset", h) == 0);
    CHECK(g.call("cellPngEncGetStreamInfo", h, kInfo) == kEmpty);
    CHECK(g.call("cellPngEncReset", h + 4) == kArg);

    // A buffer too small: what fits is written, the stream overflows.
    REQUIRE(encode(g, h, {4, 4, 16, RGBA, 8}, {RGBA, 0, 0x08}, 20) == 0);
    i = stream_info(g, h);
    CHECK(i.state == kOverflow);
    CHECK(i.size == 20);
    CHECK(g.call("cellPngEncClose", h) == 0);
    CHECK(g.call("cellPngEncWaitForInput", h, 0) == kArg);
}

TEST_CASE("cellPngEnc: to a file on /dev_hdd0, and one that cannot be made", "[hle][pngenc]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "pspocket-test-pngenc";
    fs::remove_all(root);
    fs::create_directories(root / "hdd0" / "tmp"); // lv2 lets no file be made in /dev_hdd0 itself
    Fixture f;
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g);
    std::memset(b + kPixels, 0x77, 64);
    std::memcpy(b + kName, "/dev_hdd0/tmp/shot.png", 23);
    REQUIRE(encode(g, h, {4, 4, 16, RGBA, 8}, {RGB, 9, 0xF8}, 0, 0, kName) == 0);
    Info i = stream_info(g, h);
    CHECK(i.state == 0);
    CHECK(i.location == 0);
    CHECK(i.name == kName);
    CHECK(i.addr == 0);
    std::ifstream file(root / "hdd0" / "tmp" / "shot.png", std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() == i.size);
    Png png;
    REQUIRE(read_png(bytes.data(), bytes.size(), png));
    CHECK(png.color_type == PNG_COLOR_TYPE_RGB);
    CHECK(png.rows[3][11] == 0x77);
    std::memcpy(b + kName, "/dev_nowhere/x.png", 19);
    REQUIRE(encode(g, h, {4, 4, 16, RGBA, 8}, {RGB, 9, 0xF8}, 0, 0, kName) == 0);
    CHECK(stream_info(g, h).state == kFileOpen);
    CHECK(g.call("cellPngEncClose", h) == 0);
    fs::remove_all(root);
}

TEST_CASE("cellPngEnc: EncodePicture's checks", "[hle][pngenc]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    const u32 h = open(g, 64, 64, 8);
    const Pic good{4, 4, 16, RGBA, 8};
    CHECK(encode(g, h + 1, good, {RGBA}) == kArg);
    CHECK(g.call("cellPngEncEncodePicture", h, 0, kParam, kOutParam) == kArg);
    CHECK(encode(g, h, {0, 4, 16, RGBA, 8}, {RGBA}) == kArg);          // no width
    CHECK(encode(g, h, {65, 4, 260, RGBA, 8}, {RGBA}) == kArg);        // past maxWidth
    CHECK(encode(g, h, {4, 4, 32, RGBA, 16}, {RGBA}) == kArg);         // past maxBitDepth
    CHECK(encode(g, h, {4, 4, 16, 3, 8}, {RGBA}) == kArg);             // no such colour space
    CHECK(encode(g, h, {4, 4, 16, RGBA, 8, true}, {RGBA}) == kArg);    // packed at 8 bits
    CHECK(encode(g, h, {4, 4, 4, RGB, 4}, {RGB}) == kArg);             // RGB below 8 bits
    CHECK(encode(g, h, {4, 4, 15, RGBA, 8}, {RGBA}) == kArg);          // a pitch shorter than a row
    CHECK(encode(g, h, good, {GRAY}) == kArg);                         // colour to gray
    CHECK(encode(g, h, good, {PALETTE}) == kArg);                      // colour to a palette
    CHECK(encode(g, h, good, {RGBA, 10}) == kArg);                     // zlib's levels are 0-9
    CHECK(encode(g, h, good, {RGBA, 6, 0x01}) == kArg);                // not a filter
    CHECK(encode(g, h, good, {RGBA}, 0x1000, 2) == kArg);              // no such location
    CHECK(encode(g, h, good, {RGBA}, 0, 1) == kArg);                   // a buffer of no size
    CHECK(encode(g, h, good, {RGBA}, 0x1000, 0, 0) == kArg);           // a file without a name
    CHECK(encode(g, h, good, {RGBA}) == 0);
    CHECK(g.call("cellPngEncClose", h) == 0);
}

namespace {

// A thread's own structures for EncodePicture, 0x100 bytes from kThreadCalls + 0x100 * n: the picture, the parameters,
// the output parameters.
constexpr u32 kThreadCalls = kData + 0x18000;
constexpr u32 kAtPic = 0x00, kAtParam = 0x30, kAtOut = 0x50;

u64 encode_at(GuestThread& g, u32 handle, u32 at, const Pic& p, const Param& e, u32 stream, u32 limit)
{
    u8* b = g.f.base();
    const u32 pic = at + kAtPic, param = at + kAtParam, out = at + kAtOut;
    std::memset(b + pic, 0, 40);
    ps_write32(b, pic, p.width);
    ps_write32(b, pic + 4, p.height);
    ps_write32(b, pic + 8, p.pitch);
    ps_write32(b, pic + 12, p.space);
    ps_write32(b, pic + 16, p.depth);
    ps_write32(b, pic + 24, kPixels);
    ps_write64(b, pic + 32, p.user);
    std::memset(b + param, 0, 24);
    ps_write32(b, param + 4, e.space);
    ps_write32(b, param + 8, e.level);
    ps_write32(b, param + 12, e.filters);
    ps_write32(b, out, 1); // a buffer
    ps_write32(b, out + 4, 0);
    ps_write32(b, out + 8, stream);
    ps_write32(b, out + 12, limit);
    return g.call("cellPngEncEncodePicture", handle, pic, param, out);
}

bool eventually(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("cellPngEnc: eight threads encoding with one encoder at once: four pictures queued, the others BUSY; a Reset "
          "drops those still being encoded (D-651)", "[hle][pngenc]")
{
    constexpr u32 kThreads = 8, kW = 128, kH = 96, kLimit = 0x20000;
    static_assert(kPixels + kW * kH * 4 <= kThreadCalls && kThreadCalls + 9 * 0x100 <= kStream);
    static_assert(kStream + kThreads * kLimit <= kWork);
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    for (u32 y = 0; y < kH; y++) // RGBA with some detail, so that the filters and zlib have work to do
        for (u32 x = 0; x < kW * 4; x++) b[kPixels + y * kW * 4 + x] = static_cast<u8>((x * x + 7 * y * x + y) >> 3);
    const u32 h = open(g, kW, kH, 8);
    const Pic pic{kW, kH, kW * 4, RGBA, 8};
    const Param param{RGBA, 9, 0xF8};
    // The stream the encoder makes of the picture alone.
    REQUIRE(encode_at(g, h, kThreadCalls, pic, param, kStream, kLimit) == 0);
    const Info alone = stream_info(g, h);
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
                const u64 r = encode_at(o, h, kThreadCalls + 0x100 * (t + 1), p, param, kStream + kLimit * t, kLimit);
                answers[t] = r;
                if (r == kBusy) busy++;
            });
        }
        while (ready.load() < kThreads) std::this_thread::yield();
        go = true;
        if (reset) {
            // Every thread has made its call once four were refused: the four taken still being encoded, or done.
            CHECK(eventually([&] { return busy.load() == kThreads - 4; }));
            REQUIRE(g.call("cellPngEncReset", h) == 0);
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
            REQUIRE(g.call("cellPngEncWaitForOutput", h, kNum, 0) == 0);
            CHECK(ps_read32(b, kNum) == 4);
            std::vector<u64> users;
            for (u32 n = 0; n < 4; n++) {
                const Info s = stream_info(g, h);
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
        CHECK(g.call("cellPngEncGetStreamInfo", h, kInfo) == kEmpty);
        CHECK(g.call("cellPngEncWaitForInput", h, 0) == 0);
    }
    CHECK(g.call("cellPngEncClose", h) == 0);
}

#endif
