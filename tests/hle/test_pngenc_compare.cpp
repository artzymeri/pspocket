// cellPngEnc against Sony's libpngenc (D-566, D-597): the same configurations, resources, pictures and chunks go to
// pspocket's library (on libpng) and to Sony's module. Compared: QueryAttr's attributes; Open's, OpenEx's and Close's
// answers and the handle (as an offset into the work memory when it lies there); every answer of the result queue and
// every field of each stream's information; and each PNG the encoder wrote, to the game's buffer or to a file.
//
// What counts as the same PNG (D-597): the same chunks in the same order, every chunk but IDAT byte for byte (zTXt's,
// iTXt's and iCCP's compressed parts as libpng's reader gives them back), good CRCs, and the same pixels as libpng
// decodes them. The IDAT bytes may differ: zlib's version and settings and libpng's filter choice are the encoder's own
// (Sony's libpngenc is libpng 1.2.8 on zlib 1.2.3 as its decoder is, D-199, with its own heuristics, on the SPUs or not),
// and any of them gives the same pixels. So the stream sizes may differ too: each half's streamSize must be its own
// PNG's length. The zlib header, the row filters and the IDAT size are notes, to read Sony's choices off. No game
// data; skipped without the firmware (compare_util.hpp), and without libpng in the build.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "compare_util.hpp"

#if PSPOCKET_HAVE_LIBPNG
#include <csetjmp>

#include <png.h>
#include <zlib.h>
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

TEST_CASE("cellPngEnc's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][pngenc]")
{
#if PSPOCKET_HAVE_LIBPNG
    check_names("cellPngEnc", {"cellPngEncQueryAttr", "cellPngEncOpen", "cellPngEncOpenEx", "cellPngEncClose",
                                  "cellPngEncWaitForInput", "cellPngEncEncodePicture", "cellPngEncWaitForOutput",
                                  "cellPngEncGetStreamInfo", "cellPngEncReset"});
#else
    SKIP("no libpng in this build: cellPngEnc is not pspocket's");
#endif
}

#if PSPOCKET_HAVE_LIBPNG

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kPic = kData + 0x200, kParam = kData + 0x240, kOutParam = kData + 0x280, kInfo = kData + 0x2C0,
              kNum = kData + 0x300, kChunks = kData + 0x340, kChunkData = kData + 0x400, kName = kData + 0x800,
              kPixels = kData + 0x1000, kStreams = kData + 0x40000;
constexpr u32 kStreamSize = 0x10000; // each stream buffer (eight of them)
constexpr u32 kWork = 0x01000000, kWorkSize = 0x01000000;
constexpr u8 kFill = 0xEE;
enum : u32 { GRAY = 1, RGB = 2, PALETTE = 4, GRAY_ALPHA = 9, RGBA = 10, ARGB = 20 };
// cellPng.h's chunk types.
enum : u32 {
    PLTE, TRNS, CHRM, GAMA, ICCP, SBIT, SRGB, TEXT, BKGD, HIST, PHYS, SPLT, TIME, OFFS, PCAL, SCAL, UNKNOWN,
};

struct PngFixture : TlsFixture {
    PngFixture()
    {
        REQUIRE(hle->main_memory().reserve(kWork, kWorkSize));
        REQUIRE(mem.commit(kWork, kWorkSize));
    }
};

u32 stream_at(u32 i) { return kStreams + kStreamSize * i; }

// ---- reading a PNG back

struct Chunk {
    std::string type;
    std::vector<u8> data;
    bool crc_ok = false;
};

// The chunks up to IEND (or the end of the bytes).
struct Parsed {
    bool signature = false, complete = false;
    usize length = 0; // through IEND's CRC
    std::vector<Chunk> chunks;
};

Parsed parse(const u8* p, usize n)
{
    Parsed out;
    static constexpr u8 kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || std::memcmp(p, kSig, 8) != 0) return out;
    out.signature = true;
    usize at = 8;
    while (at + 12 <= n) {
        const u32 len = ps_ld32(p + at);
        if (at + 12 + u64{len} > n) break;
        Chunk c;
        c.type.assign(reinterpret_cast<const char*>(p + at + 4), 4);
        c.data.assign(p + at + 8, p + at + 8 + len);
        const u32 crc = static_cast<u32>(crc32(0, p + at + 4, len + 4));
        c.crc_ok = crc == ps_ld32(p + at + 8 + len);
        at += 12 + len;
        out.chunks.push_back(std::move(c));
        if (out.chunks.back().type == "IEND") {
            out.complete = true;
            out.length = at;
            break;
        }
    }
    return out;
}

// libpng's reading of the pixels (one byte a sample below 8 bits) and of the compressed chunks.
struct Decoded {
    bool ok = false;
    std::vector<u8> pixels; // the rows one after another
    std::vector<std::string> texts; // "compression keyword=text [lang|translated]"
    std::string iccp;               // "name: hex of the profile"
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

Decoded decode(const u8* data, usize size)
{
    Decoded out;
    Cursor c{data, size};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, fail_fn, warn_fn);
    png_infop info = png_create_info_struct(png);
    volatile bool ok = false;
    if (!setjmp(png_jmpbuf(png))) {
        png_set_read_fn(png, &c, read_fn);
        png_read_info(png, info);
        png_set_packing(png);
        png_read_update_info(png, info);
        const u32 height = png_get_image_height(png, info);
        const size_t rb = png_get_rowbytes(png, info);
        std::vector<std::vector<u8>> rows(height, std::vector<u8>(rb));
        std::vector<png_bytep> ptrs;
        for (auto& r : rows) ptrs.push_back(r.data());
        png_read_image(png, ptrs.data());
        png_read_end(png, info);
        for (const auto& r : rows) out.pixels.insert(out.pixels.end(), r.begin(), r.end());
        png_textp text = nullptr;
        const int n = png_get_text(png, info, &text, nullptr);
        for (int i = 0; i < n; i++)
            out.texts.push_back(std::format("{} {}={} [{}|{}]", text[i].compression, text[i].key,
                std::string(text[i].text, text[i].text_length ? text[i].text_length : text[i].itxt_length),
                text[i].lang ? text[i].lang : "", text[i].lang_key ? text[i].lang_key : ""));
        png_charp name = nullptr;
        int method = 0;
        png_bytep profile = nullptr;
        png_uint_32 length = 0;
        if (png_get_iCCP(png, info, &name, &method, &profile, &length))
            out.iccp = std::format("{} (method {}): {}", name, method, hex(profile, length));
        ok = true;
    }
    png_destroy_read_struct(&png, &info, nullptr);
    out.ok = ok;
    return out;
}

// The IDAT stream inflated: each row's filter byte, as "4 4 1 ..." (empty when it does not inflate).
std::string row_filters(const Parsed& p)
{
    std::vector<u8> z;
    u32 width = 0, height = 0, depth = 0, channels = 1;
    for (const Chunk& c : p.chunks) {
        if (c.type == "IHDR" && c.data.size() == 13) {
            width = ps_ld32(c.data.data());
            height = ps_ld32(c.data.data() + 4);
            depth = c.data[8];
            channels = c.data[9] == 2 ? 3 : c.data[9] == 4 ? 2 : c.data[9] == 6 ? 4 : 1;
        }
        if (c.type == "IDAT") z.insert(z.end(), c.data.begin(), c.data.end());
    }
    const usize row = (u64{width} * channels * depth + 7) / 8 + 1;
    std::vector<u8> raw(row * height + 1);
    uLongf got = static_cast<uLongf>(raw.size());
    if (z.empty() || uncompress(raw.data(), &got, z.data(), static_cast<uLong>(z.size())) != Z_OK) return {};
    std::string s;
    for (u32 y = 0; y < height && (y + 1) * row <= got; y++) s += std::format("{}{}", y ? " " : "", raw[y * row]);
    return s;
}

struct Run : Half<PngFixture> {
    using Half::Half;

    void write_config(u32 max_w, u32 max_h, u32 depth, bool spu = false, u32 add = 0, u32 ex_num = 0)
    {
        std::memset(b + kCfg, 0, 28);
        ps_write32(b, kCfg, max_w);
        ps_write32(b, kCfg + 4, max_h);
        ps_write32(b, kCfg + 8, depth);
        ps_write8(b, kCfg + 12, spu ? 1 : 0);
        ps_write32(b, kCfg + 16, add);
        ps_write32(b, kCfg + 20, ex_num ? kChunkData : 0);
        ps_write32(b, kCfg + 24, ex_num);
    }

    u32 query(const std::string& what)
    {
        std::memset(b + kAttr, kFill, 16);
        const u64 r = Half::r("cellPngEncQueryAttr", {kCfg, kAttr}, "QueryAttr " + what);
        o.line("QueryAttr " + what + ": the attribute", hex(b + kAttr, 16));
        return r == 0 ? ps_read32(b, kAttr) : 0;
    }

    std::string handle_text(u32 h) const
    {
        if (h >= kWork && h < kWork + kWorkSize) return std::format("the work memory +{:#x}", h - kWork);
        return std::format("{:#x}", h);
    }

    // CellPngEncResource {memAddr, memSize, ppuThreadPriority, spuThreadPriority}.
    void write_resource(u32 mem, u32 size, s32 ppu = 1000, s32 spu = 200)
    {
        ps_write32(b, kRsrc, mem);
        ps_write32(b, kRsrc + 4, size);
        ps_write32(b, kRsrc + 8, static_cast<u32>(ppu));
        ps_write32(b, kRsrc + 12, static_cast<u32>(spu));
    }

    u32 open(const std::string& what, u32 max_w, u32 max_h, u32 depth, bool spu)
    {
        write_config(max_w, max_h, depth, spu);
        const u32 size = query(what);
        write_resource(kWork, std::min(size ? size : 0x100000u, kWorkSize));
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = Half::r("cellPngEncOpen", {kCfg, kRsrc, kHandle}, "Open " + what);
        o.line("Open " + what + ": the handle", handle_text(ps_read32(b, kHandle)));
        return r == 0 ? ps_read32(b, kHandle) : 0;
    }

    struct Pic {
        u32 width = 0, height = 0, pitch = 0, space = RGBA, depth = 8;
        bool packed = false;
        u64 user = 0;
    };
    struct Param {
        u32 space = RGBA, level = 6, filters = 0xF8, chunks = 0;
        bool spu = false;
    };

    // EncodePicture to stream buffer `slot` (or to the file kName when `slot` is ~0u).
    u64 encode(const std::string& what, u32 handle, const Pic& p, const Param& e, u32 slot = 0, u32 limit = kStreamSize)
    {
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
        ps_write8(b, kParam, e.spu ? 1 : 0);
        ps_write32(b, kParam + 4, e.space);
        ps_write32(b, kParam + 8, e.level);
        ps_write32(b, kParam + 12, e.filters);
        ps_write32(b, kParam + 16, e.chunks ? kChunks : 0);
        ps_write32(b, kParam + 20, e.chunks);
        const bool file = slot == ~0u;
        ps_write32(b, kOutParam, file ? 0 : 1);
        ps_write32(b, kOutParam + 4, file ? kName : 0);
        ps_write32(b, kOutParam + 8, file ? 0 : stream_at(slot));
        ps_write32(b, kOutParam + 12, file ? 0 : limit);
        if (!file) std::memset(b + stream_at(slot), kFill, kStreamSize);
        return Half::r("cellPngEncEncodePicture", {handle, kPic, kParam, kOutParam}, "EncodePicture " + what);
    }

    // WaitForOutput, blocking or not: its answer and count.
    u64 wait_output(const std::string& what, u32 handle, bool block)
    {
        ps_write32(b, kNum, 0xEEEEEEEE);
        const u64 r = Half::r("cellPngEncWaitForOutput", {handle, kNum, block ? 1u : 0u}, "WaitForOutput " + what);
        o.line("WaitForOutput " + what + ": the count", std::format("{:#x}", ps_read32(b, kNum)));
        return r;
    }

    // GetStreamInfo: its answer and fields (the size checked against the PNG it describes), then the PNG.
    void stream_info(const std::string& what, u32 handle, const std::string& file = {})
    {
        std::memset(b + kInfo, kFill, 48);
        const u64 r = Half::r("cellPngEncGetStreamInfo", {handle, kInfo}, "GetStreamInfo " + what);
        if (r != 0) {
            o.line("GetStreamInfo " + what + ": the information", hex(b + kInfo, 48));
            return;
        }
        std::vector<u8> fields(b + kInfo, b + kInfo + 48);
        const u32 size = ps_read32(b, kInfo + 20), addr = ps_read32(b, kInfo + 12);
        std::memset(fields.data() + 20, 0, 4); // streamSize: below
        o.line("GetStreamInfo " + what + ": the information but streamSize", hex(fields));
        std::vector<u8> bytes;
        if (!file.empty()) {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            o.line("GetStreamInfo " + what + ": the file's size is streamSize",
                bytes.size() == size ? "yes" : std::format("no ({} bytes)", bytes.size()));
        } else if (addr >= kStreams && addr + u64{size} <= kStreams + 8 * kStreamSize) {
            bytes.assign(b + addr, b + addr + std::min(size + 64u, kStreamSize));
        }
        analyse(what, bytes, size, file.empty());
    }

    // A PNG of `size` bytes (`bytes` may hold 64 more: what follows it in the buffer).
    void analyse(const std::string& what, const std::vector<u8>& bytes, u32 size, bool buffer)
    {
        const Parsed p = parse(bytes.data(), std::min<usize>(bytes.size(), size));
        o.line("stream " + what + ": a whole PNG", !p.signature ? "no signature" : p.complete ? "yes" : "cut short");
        o.line("stream " + what + ": streamSize is the PNG's length",
            p.complete && p.length == size ? "yes" : std::format("no (the PNG {} bytes)", p.length));
        if (buffer && bytes.size() >= u64{size} + 64) {
            bool untouched = true;
            for (u32 i = 0; i < 64; i++) untouched = untouched && bytes[size + i] == kFill;
            o.line("stream " + what + ": the 64 bytes past it untouched", untouched ? "yes" : "no");
        }
        std::string order, bad;
        usize idat = 0;
        for (const Chunk& c : p.chunks) {
            if (!c.crc_ok) bad += " " + c.type;
            if (c.type == "IDAT") {
                if (!order.ends_with("IDAT")) order += " IDAT";
                idat += c.data.size();
                continue;
            }
            order += " " + c.type;
            // Compressed parts: as libpng reads them back, below.
            if (c.type == "zTXt" || c.type == "iCCP" || (c.type == "iTXt" && c.data.size() > 0)) {
                const auto nul = std::find(c.data.begin(), c.data.end(), u8{0});
                o.line(std::format("stream {}: {}'s keyword", what, c.type), std::string(c.data.begin(), nul));
                continue;
            }
            o.line(std::format("stream {}: {}", what, c.type), hex(c.data));
        }
        o.line("stream " + what + ": the chunks", order.empty() ? "(none)" : order.substr(1));
        o.line("stream " + what + ": the chunks with a bad CRC", bad.empty() ? "none" : bad.substr(1));
        const Decoded d = decode(bytes.data(), std::min<usize>(bytes.size(), size));
        o.line("stream " + what + ": libpng reads it", d.ok ? "yes" : "no");
        if (!d.ok) return;
        o.bytes("stream " + what + ": the pixels", d.pixels);
        for (usize i = 0; i < d.texts.size(); i++) o.line(std::format("stream {}: text {}", what, i), d.texts[i]);
        if (!d.iccp.empty()) o.line("stream " + what + ": the ICC profile", d.iccp);
        const u8* z = nullptr;
        for (const Chunk& c : p.chunks)
            if (c.type == "IDAT" && c.data.size() >= 2) {
                z = c.data.data();
                break;
            }
        o.note(std::format("stream {}: {} bytes, IDAT {} bytes, zlib header {}, row filters {}", what, size, idat,
            z ? std::format("{:02x}{:02x}", z[0], z[1]) : "none", row_filters(p)));
    }
};

void fill(u8* b, u32 bytes, u32 seed)
{
    for (u32 i = 0; i < bytes; i++) ps_write8(b, kPixels + i, static_cast<u8>(i * 37 + seed));
}

// A 64x64 picture of smooth ramps (filters help it) with some noise (they cannot remove all of it), RGBA.
void gradient_rgba(u8* b, u32 w, u32 h)
{
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u32 at = kPixels + 4 * (y * w + x);
            const u8 noise = static_cast<u8>(((x * 7 + y * 13) ^ (x * y)) & 7);
            ps_write8(b, at, static_cast<u8>(x * 4 + noise));
            ps_write8(b, at + 1, static_cast<u8>(y * 4));
            ps_write8(b, at + 2, static_cast<u8>((x + y) * 2 + noise));
            ps_write8(b, at + 3, static_cast<u8>(255 - y));
        }
}

// The ancillary chunk list at kChunks: {type, data} pairs.
void chunks(u8* b, std::initializer_list<std::pair<u32, u32>> list)
{
    u32 i = 0;
    for (const auto& [type, at] : list) {
        ps_write32(b, kChunks + 8 * i, type);
        ps_write32(b, kChunks + 8 * i + 4, at);
        i++;
    }
}

void put_string(u8* b, u32 at, const char* s) { std::memcpy(b + at, s, std::strlen(s) + 1); }

Observed half(const Setup& s, bool sony_half)
{
    namespace fs = std::filesystem;
    Run h(s, sony_half, "cellPngEnc");
    u8* b = h.b;
    using Pic = Run::Pic;
    using Param = Run::Param;

    // QueryAttr: the memory for each configuration, and its checks.
    h.r("cellPngEncQueryAttr", {kCfg, 0}, "QueryAttr(cfg, NULL)");
    h.r("cellPngEncQueryAttr", {0, kAttr}, "QueryAttr(NULL, attr)");
    struct Query {
        u32 w, h, depth;
        bool spu;
        u32 add, ex;
        const char* what;
    };
    for (const Query& q : {Query{640, 480, 8, false, 0x100, 0, "640x480 8 bits, 0x100 more"},
             Query{640, 480, 16, true, 0, 0, "640x480 16 bits with SPUs"}, Query{64, 64, 16, false, 0, 0, "64x64 16 bits"},
             Query{1920, 1080, 8, false, 0, 0, "1920x1080 8 bits"}, Query{1, 1, 8, false, 0, 0, "1x1"},
             Query{640, 480, 12, false, 0, 0, "12 bits"}, Query{640, 480, 4, false, 0, 0, "4 bits"},
             Query{0, 480, 8, false, 0, 0, "no width"}, Query{640, 0, 8, false, 0, 0, "no height"},
             Query{1000001, 480, 8, false, 0, 0, "1000001 wide"}, Query{640, 480, 8, false, 0x80000000u, 0, "addMemSize 2 GB"},
             Query{640, 480, 8, false, 0, 1, "an extended parameter"}}) {
        h.write_config(q.w, q.h, q.depth, q.spu, q.add, q.ex);
        // CellPngEncExParam {index 0, value -> 0}, for the configuration that has one.
        std::memset(b + kChunkData, 0, 16);
        ps_write32(b, kChunkData + 4, kChunkData + 8);
        h.query(q.what);
    }

    // Open's and OpenEx's checks, then the handle.
    h.write_config(64, 64, 16);
    const u32 size = h.query("for Open's checks");
    const u32 mem = std::min(size ? size : 0x100000u, kWorkSize);
    // An Open that should be refused; one that is not is closed again (unless its handle is `keep`'s).
    const auto open_refused = [&](const std::string& what, u32 cfg, u32 rsrc, u32 handle, bool ex = false,
                                  u32 keep = 0) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = h.r(ex ? "cellPngEncOpenEx" : "cellPngEncOpen", {cfg, rsrc, handle},
            (ex ? "OpenEx " : "Open ") + what);
        h.o.line((ex ? "OpenEx " : "Open ") + what + ": the handle word", h.handle_text(ps_read32(b, kHandle)));
        if (r == 0 && ps_read32(b, kHandle) != keep)
            h.r("cellPngEncClose", {ps_read32(b, kHandle)}, "Close after Open " + what);
    };
    h.write_resource(kWork, mem > 0 ? mem - 1 : 0);
    open_refused("with a byte too little memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 0xC00);
    open_refused("with PPU priority 0xC00", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0x100);
    open_refused("with SPU priority 0x100", kCfg, kRsrc, kHandle);
    h.write_resource(0, mem);
    open_refused("without memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem);
    open_refused("without a handle pointer", kCfg, kRsrc, 0);
    open_refused("without a resource", kCfg, 0, kHandle);
    open_refused("without a configuration", 0, kRsrc, kHandle);
    std::memset(b + kRsrc, 0, 24);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, mem);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write8(b, kRsrc + 16 + 3, 16);
    open_refused("with a priority of 16", kCfg, kRsrc, kHandle, true);
    open_refused("without a resource", kCfg, 0, kHandle, true);

    const u32 hd = h.open("64x64 16 bits", 64, 64, 16, false);
    if (!hd) return std::move(h.o);
    h.write_resource(kWork, mem);
    open_refused("of the memory already open", kCfg, kRsrc, kHandle, false, hd);

    // Pictures in every colour space and depth (test_imgenc.cpp's), each taken from the queue and read back.
    const auto take = [&](const std::string& what) {
        h.wait_output(what, hd, true);
        h.stream_info(what, hd);
    };
    const auto one = [&](const std::string& what, const Pic& p, const Param& e) {
        if (h.encode(what, hd, p, e) == 0) take(what);
    };
    fill(b, 24 * 3, 1);
    one("RGBA 5x3, rows of 24 bytes, level 9, every filter", {5, 3, 24, RGBA, 8}, {RGBA, 9, 0xF8});
    fill(b, 32, 7);
    one("ARGB 4x2 to RGBA, level 0, no filter", {4, 2, 16, ARGB, 8}, {ARGB, 0, 0});
    one("ARGB 4x2 to RGB, filter sub", {4, 2, 16, ARGB, 8}, {RGB, 6, 0x10});
    fill(b, 18, 3);
    one("RGB 3x2 to RGBA, filter up", {3, 2, 9, RGB, 8}, {RGBA, 6, 0x20});
    fill(b, 32, 5);
    one("RGBA 2x2 16 bits, filter average", {2, 2, 16, RGBA, 16}, {RGBA, 6, 0x40});
    fill(b, 4, 9);
    one("gray 4x1 to gray and alpha, filter Paeth", {4, 1, 4, GRAY, 8}, {GRAY_ALPHA, 6, 0x80});
    fill(b, 8, 9);
    one("gray and alpha 4x1 to gray", {4, 1, 8, GRAY_ALPHA, 8}, {GRAY, 6, 0xF8});
    for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(i % 16));
    one("gray 5x2 4 bits, a sample a byte", {5, 2, 8, GRAY, 4}, {GRAY, 6, 0x08});
    const u8 packed[6] = {0x12, 0x34, 0x50, 0xAB, 0xCD, 0xE0};
    std::memcpy(b + kPixels, packed, 6);
    one("gray 5x2 4 bits packed", {5, 2, 3, GRAY, 4, true}, {GRAY, 6, 0x08});
    ps_write8(b, kPixels, 0xA5);
    ps_write8(b, kPixels + 1, 0x80);
    one("gray 10x1 1 bit packed", {10, 1, 2, GRAY, 1, true}, {GRAY, 6, 0x08});
    for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(i % 4));
    one("gray 8x2 2 bits, a sample a byte", {8, 2, 8, GRAY, 2}, {GRAY, 6, 0xF8});
    fill(b, 8 * 16, 11);
    one("gray 8x8 16 bits", {8, 8, 16, GRAY, 16}, {GRAY, 6, 0xF8});
    one("gray and alpha 8x8 16 bits", {8, 8, 32, GRAY_ALPHA, 16}, {GRAY_ALPHA, 6, 0xF8});
    one("RGB 4x4 16 bits", {4, 4, 24, RGB, 16}, {RGB, 6, 0xF8});

    // A palette with its PLTE and tRNS, and without its PLTE.
    {
        const u8 idx[6] = {0, 1, 2, 2, 1, 0};
        std::memcpy(b + kPixels, idx, 6);
        std::memset(b + kChunkData, 0, 0x100);
        ps_write32(b, kChunkData, 3);
        ps_write32(b, kChunkData + 4, kChunkData + 0x40);
        const u8 rgb[9] = {255, 0, 0, 0, 255, 0, 0, 0, 255};
        std::memcpy(b + kChunkData + 0x40, rgb, 9);
        ps_write32(b, kChunkData + 0x20, kChunkData + 0x80);
        ps_write32(b, kChunkData + 0x24, 3);
        const u8 alpha[3] = {0, 128, 255};
        std::memcpy(b + kChunkData + 0x80, alpha, 3);
        chunks(b, {{PLTE, kChunkData}, {TRNS, kChunkData + 0x20}});
        one("a palette 3x2 with PLTE and tRNS", {3, 2, 3, PALETTE, 8}, {PALETTE, 6, 0x08, 2});
        one("a palette 3x2 without its PLTE", {3, 2, 3, PALETTE, 8}, {PALETTE, 6, 0x08, 0});
        // A 16-colour palette at 4 bits, its histogram and background.
        for (u32 i = 0; i < 16; i++) {
            ps_write8(b, kChunkData + 0x40 + 3 * i, static_cast<u8>(i * 16));
            ps_write8(b, kChunkData + 0x41 + 3 * i, static_cast<u8>(255 - i * 16));
            ps_write8(b, kChunkData + 0x42 + 3 * i, static_cast<u8>(i * 5));
            ps_write16(b, kChunkData + 0xC0 + 2 * i, static_cast<u16>(i + 1));
        }
        ps_write32(b, kChunkData, 16);
        ps_write32(b, kChunkData + 0x100, kChunkData + 0xC0); // CellPngHIST {entries, 16}
        ps_write32(b, kChunkData + 0x104, 16);
        std::memset(b + kChunkData + 0x110, 0, 20); // CellPngBKGD {paletteIndex 5}
        ps_write8(b, kChunkData + 0x110, 5);
        chunks(b, {{PLTE, kChunkData}, {HIST, kChunkData + 0x100}, {BKGD, kChunkData + 0x110}});
        for (u32 i = 0; i < 64; i++) ps_write8(b, kPixels + i, static_cast<u8>(i % 16));
        one("a palette 8x8 of 16 colours at 4 bits with hIST and bKGD", {8, 8, 8, PALETTE, 4}, {PALETTE, 6, 0xF8, 3});
    }

    // Ancillary chunks: text (tEXt, zTXt, iTXt), time, gamma, physical size, an sPLT; then the colour chunks.
    {
        std::memset(b + kChunkData, 0, 0x400);
        put_string(b, kChunkData + 0x200, "Title");
        put_string(b, kChunkData + 0x220, "pspocket");
        put_string(b, kChunkData + 0x240, "Comment");
        put_string(b, kChunkData + 0x260, "a longer text, compressed, a longer text, compressed");
        put_string(b, kChunkData + 0x2A0, "Author");
        put_string(b, kChunkData + 0x2C0, "en");
        put_string(b, kChunkData + 0x2C8, "Auteur");
        put_string(b, kChunkData + 0x2E0, "nobody");
        const u32 texts[3][6] = {{0, kChunkData + 0x200, kChunkData + 0x220, 0, 0, 0},
            {1, kChunkData + 0x240, kChunkData + 0x260, 0, 0, 0},
            {2, kChunkData + 0x2A0, kChunkData + 0x2E0, 0, kChunkData + 0x2C0, kChunkData + 0x2C8}};
        for (u32 t = 0; t < 3; t++)
            for (u32 k = 0; k < 6; k++) ps_write32(b, kChunkData + 0x18 * t + 4 * k, texts[t][k]);
        ps_write16(b, kChunkData + 0x60, 2026);
        const u8 when[5] = {10, 9, 12, 34, 56};
        std::memcpy(b + kChunkData + 0x62, when, 5);
        ps_writef64(b, kChunkData + 0x70, 0.45455);
        ps_write32(b, kChunkData + 0x80, 2835);
        ps_write32(b, kChunkData + 0x84, 2835);
        ps_write32(b, kChunkData + 0x88, 1);
        chunks(b, {{TEXT, kChunkData}, {TEXT, kChunkData + 0x18}, {TEXT, kChunkData + 0x30}, {TIME, kChunkData + 0x60},
                      {GAMA, kChunkData + 0x70}, {PHYS, kChunkData + 0x80}, {SPLT, kChunkData + 0x90}});
        std::memset(b + kPixels, 0x40, 64);
        one("gray 4x4 with texts, time, gamma, physical size and an sPLT", {4, 4, 4, GRAY, 8}, {GRAY, 6, 0xF8, 7});

        // cHRM, sRGB, sBIT, bKGD (RGB), oFFs, sCAL, iCCP, tRNS (a colour) on RGB.
        std::memset(b + kChunkData, 0, 0x400);
        const double chrm[8] = {0.3127, 0.329, 0.64, 0.33, 0.3, 0.6, 0.15, 0.06};
        for (u32 i = 0; i < 8; i++) ps_writef64(b, kChunkData + 8 * i, chrm[i]);
        ps_write32(b, kChunkData + 0x40, 1); // sRGB: relative colorimetric
        const u32 sbit[5] = {5, 6, 5, 0, 0};
        for (u32 i = 0; i < 5; i++) ps_write32(b, kChunkData + 0x50 + 4 * i, sbit[i]);
        const u32 bkgd[5] = {0, 10, 20, 30, 0};
        for (u32 i = 0; i < 5; i++) ps_write32(b, kChunkData + 0x70 + 4 * i, bkgd[i]);
        ps_write32(b, kChunkData + 0x90, 100); // oFFs {100, -50, pixel}
        ps_write32(b, kChunkData + 0x94, static_cast<u32>(-50));
        ps_write32(b, kChunkData + 0x98, 0);
        ps_write32(b, kChunkData + 0xA0, 1); // sCAL {metre, 0.001, 0.002}
        ps_writef64(b, kChunkData + 0xA8, 0.001);
        ps_writef64(b, kChunkData + 0xB0, 0.002);
        ps_write32(b, kChunkData + 0xD0, 0); // tRNS {no alphas, 0, red 1, green 2, blue 3}
        ps_write32(b, kChunkData + 0xD4, 0);
        ps_write16(b, kChunkData + 0xD8, 1);
        ps_write16(b, kChunkData + 0xDA, 2);
        ps_write16(b, kChunkData + 0xDC, 3);
        chunks(b, {{CHRM, kChunkData}, {SRGB, kChunkData + 0x40}, {SBIT, kChunkData + 0x50}, {BKGD, kChunkData + 0x70},
                      {OFFS, kChunkData + 0x90}, {SCAL, kChunkData + 0xA0}, {TRNS, kChunkData + 0xD0}});
        fill(b, 4 * 4 * 3, 13);
        one("RGB 4x4 with cHRM, sRGB, sBIT, bKGD, oFFs, sCAL and tRNS", {4, 4, 12, RGB, 8}, {RGB, 6, 0xF8, 7});

        // iCCP alone: a profile of only its header (an RGB display profile, no tags), which libpng 1.6 takes.
        std::memset(b + kChunkData, 0, 0x400);
        put_string(b, kChunkData + 0x100, "a profile");
        const u32 icc = kChunkData + 0x140;
        ps_write32(b, icc, 132);              // its size
        ps_write32(b, icc + 8, 0x02100000);   // version 2.1
        ps_write32(b, icc + 12, 0x6D6E7472);  // 'mntr'
        ps_write32(b, icc + 16, 0x52474220);  // 'RGB '
        ps_write32(b, icc + 20, 0x58595A20);  // 'XYZ '
        ps_write32(b, icc + 36, 0x61637370);  // 'acsp'
        ps_write32(b, icc + 68, 0x0000F6D6);  // the D50 illuminant
        ps_write32(b, icc + 72, 0x00010000);
        ps_write32(b, icc + 76, 0x0000D32D);
        ps_write32(b, kChunkData + 0xC0, kChunkData + 0x100); // iCCP {name, profile, size}
        ps_write32(b, kChunkData + 0xC4, icc);
        ps_write32(b, kChunkData + 0xC8, 132);
        chunks(b, {{ICCP, kChunkData + 0xC0}});
        one("RGB 4x4 with iCCP", {4, 4, 12, RGB, 8}, {RGB, 6, 0xF8, 1});
    }

    // A larger picture under each filter set and several levels: Sony's filter choice and zlib's settings (notes).
    gradient_rgba(b, 64, 64);
    for (const u32 filters : {0u, 0x08u, 0x10u, 0x20u, 0x40u, 0x80u, 0xF8u})
        one(std::format("RGBA 64x64 ramps, filters {:#x}", filters), {64, 64, 256, RGBA, 8}, {RGBA, 6, filters});
    for (const u32 level : {0u, 1u, 9u})
        one(std::format("RGBA 64x64 ramps, level {}", level), {64, 64, 256, RGBA, 8}, {RGBA, level, 0xF8});
    one("RGBA 64x64 ramps to RGB", {64, 64, 256, RGBA, 8}, {RGB, 6, 0xF8});

    // The queue: four waiting (each in its own buffer), then room and a fifth; oldest first; Reset; EMPTY.
    h.wait_output("with nothing waiting", hd, false);
    h.r("cellPngEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo with nothing waiting");
    std::memset(b + kPixels, 0x22, 64);
    for (u32 i = 0; i < 4; i++) {
        h.r("cellPngEncWaitForInput", {hd, 1}, std::format("WaitForInput before picture {}", i));
        Pic p{4, 4, 16, RGBA, 8};
        p.user = 0x100 + i;
        h.encode(std::format("queued picture {}", i), hd, p, {RGBA, 6, 0xF8}, i);
    }
    // Until `n` are encoded (Sony's encodes on threads of its own; a non-blocking answer before then would only say
    // how far it had got): how many were, after at most two seconds.
    const auto ready = [&](u32 n) {
        u32 got = 0;
        for (int tries = 0; tries < 400 && got < n; tries++) {
            ps_write32(b, kNum, 0);
            h.side.call("cellPngEncWaitForOutput", {hd, kNum, 0});
            got = ps_read32(b, kNum);
            if (got < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return got;
    };
    h.o.line("the four queued pictures encoded", std::format("{}", ready(4)));
    h.r("cellPngEncWaitForInput", {hd, 0}, "WaitForInput with four waiting, not blocking");
    if (h.encode("a fifth with four waiting", hd, {4, 4, 16, RGBA, 8}, {RGBA, 6, 0xF8}, 4) == 0)
        h.o.line("the fifth encoded too", std::format("{}", ready(5)));
    h.wait_output("with four waiting", hd, false);
    h.stream_info("the oldest of the four", hd);
    h.stream_info("the next of the four", hd);
    h.r("cellPngEncWaitForInput", {hd, 0}, "WaitForInput with two taken");
    h.r("cellPngEncReset", {hd}, "Reset");
    h.r("cellPngEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo after Reset");
    h.wait_output("after Reset", hd, false);
    h.r("cellPngEncReset", {hd + 4}, "Reset of no handle");
    h.r("cellPngEncReset", {0}, "Reset(NULL)");

    // A buffer too small: what fits, the stream overflowing.
    if (h.encode("to a buffer of 20 bytes", hd, {4, 4, 16, RGBA, 8}, {RGBA, 0, 0x08}, 5, 20) == 0) {
        h.wait_output("to a buffer of 20 bytes", hd, true);
        std::memset(b + kInfo, kFill, 48);
        h.r("cellPngEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo to a buffer of 20 bytes");
        h.o.line("GetStreamInfo to a buffer of 20 bytes: the information", hex(b + kInfo, 48));
        h.o.line("the buffer of 20 bytes and the 16 after it", hex(b + stream_at(5), 36));
    }

    // To a file on /dev_hdd0 (lv2 makes no file in /dev_hdd0 itself), and one that cannot be made.
    const fs::path root = fs::temp_directory_path() /
        std::format("pspocket-pngenc-compare-{}", h.side.sony_half() ? "theirs" : "ours");
    fs::remove_all(root);
    fs::create_directories(root / "hdd0" / "tmp");
    REQUIRE(h.f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    std::memset(b + kPixels, 0x77, 64);
    put_string(b, kName, "/dev_hdd0/tmp/shot.png");
    if (h.encode("to a file", hd, {4, 4, 16, RGBA, 8}, {RGB, 9, 0xF8}, ~0u) == 0) {
        h.wait_output("to a file", hd, true);
        h.stream_info("to a file", hd, (root / "hdd0" / "tmp" / "shot.png").string());
    }
    put_string(b, kName, "/dev_nowhere/x.png");
    if (h.encode("to a file that cannot be made", hd, {4, 4, 16, RGBA, 8}, {RGB, 9, 0xF8}, ~0u) == 0) {
        h.wait_output("to a file that cannot be made", hd, true);
        std::memset(b + kInfo, kFill, 48);
        h.r("cellPngEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo to a file that cannot be made");
        h.o.line("GetStreamInfo to a file that cannot be made: the information", hex(b + kInfo, 48));
    }

    // EncodePicture's checks (test_imgenc.cpp's).
    {
        const Pic good{4, 4, 16, RGBA, 8};
        h.encode("of no handle", hd + 1, good, {RGBA});
        h.r("cellPngEncEncodePicture", {hd, 0, kParam, kOutParam}, "EncodePicture without a picture");
        h.encode("of no width", hd, {0, 4, 16, RGBA, 8}, {RGBA});
        h.encode("past maxWidth", hd, {65, 4, 260, RGBA, 8}, {RGBA});
        h.encode("of colour space 3", hd, {4, 4, 16, 3, 8}, {RGBA});
        h.encode("packed at 8 bits", hd, {4, 4, 16, RGBA, 8, true}, {RGBA});
        h.encode("of RGB at 4 bits", hd, {4, 4, 4, RGB, 4}, {RGB});
        h.encode("with a pitch shorter than a row", hd, {4, 4, 15, RGBA, 8}, {RGBA});
        h.encode("of colour to gray", hd, good, {GRAY});
        h.encode("of colour to a palette", hd, good, {PALETTE});
        h.encode("at level 10", hd, good, {RGBA, 10});
        h.encode("with filter bits 0x01", hd, good, {RGBA, 6, 0x01});
        h.encode("to a buffer of no size", hd, good, {RGBA}, 6, 0);
        ps_write32(b, kOutParam, 2); // the rest as for the buffer of no size
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to location 2");
        ps_write32(b, kOutParam, 0);
        ps_write32(b, kOutParam + 4, 0);
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to a file without a name");
        // What the refusals left queued (a refused picture must not be).
        for (int i = 0; i < 4; i++) {
            if (h.wait_output(std::format("after the checks, {}", i), hd, false) != 0) break;
            h.stream_info(std::format("after the checks, {}", i), hd);
        }
    }
    h.r("cellPngEncClose", {hd}, "Close");
    h.r("cellPngEncClose", {hd}, "Close again");
    h.r("cellPngEncWaitForInput", {hd, 0}, "WaitForInput after Close");

    // With the SPUs: an encoder opened for them, a picture encoded on them.
    const u32 spu = h.open("64x64 8 bits with SPUs", 64, 64, 8, true);
    if (spu) {
        gradient_rgba(b, 64, 64);
        if (h.encode("RGBA 64x64 ramps on the SPUs", spu, {64, 64, 256, RGBA, 8}, {RGBA, 6, 0xF8, 0, true}) == 0) {
            h.wait_output("RGBA 64x64 ramps on the SPUs", spu, true);
            h.stream_info("RGBA 64x64 ramps on the SPUs", spu);
        }
        h.r("cellPngEncClose", {spu}, "Close of the SPU encoder");
    }
    fs::remove_all(root);
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellPngEnc against Sony's libpngenc: sizes, handles, the queue, the streams, every PNG's chunks and pixels",
    "[.][hle][lle][compare][pngenc]")
{
    const Setup s = setup();
    run(s, "cellPngEnc against Sony's libpngenc", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: RPCS3's sizes, the handle at the work memory, test_imgenc.cpp's pictures.
            CHECK(o.value("QueryAttr 64x64 16 bits: the attribute") ==
                std::format("hex:{:08x}04eeeeee0027000000000000", 0x47A00 + 8 * 64 * 7 + 0x1780));
            CHECK(o.value("Open 64x64 16 bits: the handle") == "the work memory +0x0");
            CHECK(o.value("Open of the memory already open") == "0x80611291");
            CHECK(o.value("stream RGBA 5x3, rows of 24 bytes, level 9, every filter: a whole PNG") == "yes");
            CHECK(o.value("stream RGBA 5x3, rows of 24 bytes, level 9, every filter: streamSize is the PNG's length") ==
                "yes");
            CHECK(o.value("stream RGBA 5x3, rows of 24 bytes, level 9, every filter: the chunks") == "IHDR IDAT IEND");
            CHECK(o.value("stream ARGB 4x2 to RGBA, level 0, no filter: IHDR") == "hex:00000004000000020806000000");
            const Data* argb = o.find_data("stream ARGB 4x2 to RGBA, level 0, no filter: the pixels");
            REQUIRE(argb);
            REQUIRE(argb->bytes.size() == 32);
            // ARGB's first pixel (bytes 7, 44, 81, 118 of the fill) written as RGBA.
            CHECK(argb->bytes[0] == static_cast<u8>(1 * 37 + 7));
            CHECK(argb->bytes[3] == static_cast<u8>(7));
            CHECK(o.value("stream gray 4x4 with texts, time, gamma, physical size and an sPLT: the chunks") ==
                "IHDR gAMA pHYs tIME tEXt zTXt iTXt IDAT IEND");
            CHECK(o.value("stream a palette 3x2 with PLTE and tRNS: PLTE") == "hex:ff000000ff000000ff");
            CHECK(o.value("GetStreamInfo a palette 3x2 without its PLTE: the information but streamSize")
                      .starts_with("hex:806112a1"));
            CHECK(o.value("the four queued pictures encoded") == "4");
            CHECK(o.value("WaitForInput with four waiting, not blocking") == "0x80611293");
            CHECK(o.value("GetStreamInfo the next of the four: the information but streamSize").ends_with(
                "0000000000000101eeeeeeeeeeeeeeee"));
            CHECK(o.value("stream to a file: libpng reads it") == "yes");
            CHECK(o.value("GetStreamInfo to a file that cannot be made: the information").starts_with("hex:806112a4"));
            CHECK(o.value("EncodePicture at level 10") == "0x80611291");
        });
}

#endif
