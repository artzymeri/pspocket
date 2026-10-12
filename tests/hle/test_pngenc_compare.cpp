// cellPngEnc against Sony's libpngenc (D-566, D-597, D-913): the same configurations, resources, pictures and chunks go
// to pspocket's library and to Sony's module. Compared: QueryAttr's attributes; Open's, OpenEx's and Close's answers and
// the handle (as an offset into the work memory when it lies there); every answer of the two queues (the commands
// waiting to be encoded and the streams waiting to be taken) and every field of each stream's information; and each PNG
// the encoder wrote, to the game's buffer or to a file.
//
// What counts as the same PNG (D-597): the same chunks in the same order, every chunk but IDAT byte for byte (zTXt's,
// iTXt's and iCCP's compressed parts as libpng's reader gives them back), good CRCs, and the same pixels as libpng
// decodes them. The IDAT bytes may differ: zlib's version is the encoder's own (Sony's libpngenc is libpng 1.2.8 on zlib
// 1.2.3, D-199, on the SPUs or not), and any of them gives the same pixels. So the stream sizes may differ too: each
// half's streamSize must be its own PNG's length. The zlib header, the row filters and the IDAT size are notes, to read
// Sony's choices off.
//
// libpngenc encodes on a thread of its own: each comparison waits for a stream (WaitForOutput blocking) before it reads
// it, and the queue's states are reached only through answers that do not depend on how far that thread has got. No
// game data; skipped without the firmware (compare_util.hpp), and without libpng in the build.

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

TEST_CASE("cellPngEnc's comparison calls Sony's exports by their names; pspocket's library", "[hle][pngenc]")
{
#if PSPOCKET_HAVE_LIBPNG
    check_names("cellPngEnc", {"cellPngEncQueryAttr", "cellPngEncOpen", "cellPngEncOpenEx", "cellPngEncClose",
                                  "cellPngEncWaitForInput", "cellPngEncEncodePicture", "cellPngEncWaitForOutput",
                                  "cellPngEncGetStreamInfo", "cellPngEncReset"},
        /*marked=*/false);
#else
    SKIP("no libpng in this build: cellPngEnc is not pspocket's");
#endif
}

#if PSPOCKET_HAVE_LIBPNG

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kPic = kData + 0x200, kParam = kData + 0x240, kOutParam = kData + 0x280, kInfo = kData + 0x2C0,
              kNum = kData + 0x300, kChunks = kData + 0x400, kName = kData + 0x800, kChunkData = kData + 0x1000,
              kPixels = kData + 0x8000, kStreams = kData + 0x40000;
constexpr u32 kStreamSize = 0x10000; // each stream buffer (eight of them)
constexpr u32 kWork = 0x01000000, kWorkSize = 0x01000000;
constexpr u32 kEncoderMem = 0x100000; // each encoder's work memory within kWork
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
    std::vector<u8> pixels;         // the rows one after another
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
    // Chunks libpng 1.6 would refuse to read (an unknown chunk, a profile of only a header) are kept as they come.
    png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_ALWAYS, nullptr, 0);
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

// A zlib stream inflated (empty when it does not inflate).
std::vector<u8> inflate_all(const u8* z, usize n)
{
    std::vector<u8> out;
    z_stream s{};
    if (inflateInit(&s) != Z_OK) return out;
    s.next_in = const_cast<u8*>(z);
    s.avail_in = static_cast<uInt>(n);
    u8 buf[4096];
    int r = Z_OK;
    while (r == Z_OK) {
        s.next_out = buf;
        s.avail_out = sizeof(buf);
        r = inflate(&s, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof(buf) - s.avail_out));
    }
    inflateEnd(&s);
    if (r != Z_STREAM_END) out.clear();
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
    const std::vector<u8> raw = z.empty() ? std::vector<u8>() : inflate_all(z.data(), z.size());
    std::string s;
    for (u32 y = 0; y < height && (y + 1) * row <= raw.size(); y++) s += std::format("{}{}", y ? " " : "", raw[y * row]);
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

    // An encoder in work memory `slot` (kEncoderMem apart, an odd offset into it so the handle is the next 128 bytes).
    u32 open(const std::string& what, u32 slot, u32 max_w, u32 max_h, u32 depth, bool spu, u32 add = 0)
    {
        write_config(max_w, max_h, depth, spu, add);
        const u32 size = query(what);
        write_resource(kWork + slot * kEncoderMem + 0x10, std::min(size ? size : 0x100000u, kEncoderMem - 0x10));
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
        const s32 state = static_cast<s32>(ps_read32(b, kInfo));
        const u32 size = ps_read32(b, kInfo + 20), addr = ps_read32(b, kInfo + 12);
        if (state == 0) std::memset(fields.data() + 20, 0, 4); // streamSize: below
        o.line(std::string("GetStreamInfo ") + what + (state == 0 ? ": the information but streamSize"
                                                                    : ": the information"),
            hex(fields));
        if (state != 0) return;
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

    // Waits for one stream and takes it.
    void take(const std::string& what, u32 handle)
    {
        wait_output(what, handle, true);
        stream_info(what, handle);
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
            // Compressed parts: as libpng reads them back, below; the rest of the chunk byte for byte.
            if (c.type == "zTXt" || c.type == "iCCP") {
                const auto nul = std::find(c.data.begin(), c.data.end(), u8{0});
                o.line(std::format("stream {}: {}'s keyword", what, c.type), std::string(c.data.begin(), nul));
                const usize head = static_cast<usize>(nul - c.data.begin()) + 2;
                if (head <= c.data.size()) {
                    o.line(std::format("stream {}: {}'s method", what, c.type), std::format("{}", c.data[head - 1]));
                    o.line(std::format("stream {}: {} inflated", what, c.type),
                        hex(inflate_all(c.data.data() + head, c.data.size() - head)));
                }
                continue;
            }
            if (c.type == "iTXt" && c.data.size() > 2) {
                const auto nul = std::find(c.data.begin(), c.data.end(), u8{0});
                const usize k = static_cast<usize>(nul - c.data.begin());
                if (k + 3 <= c.data.size() && c.data[k + 1] == 1) { // compressed: the header, then the text inflated
                    usize at = k + 3;
                    for (int fields = 0; fields < 2 && at < c.data.size(); at++)
                        if (c.data[at] == 0) fields++;
                    o.line(std::format("stream {}: iTXt's header", what), hex(c.data.data(), at));
                    o.line(std::format("stream {}: iTXt inflated", what),
                        hex(inflate_all(c.data.data() + at, c.data.size() - at)));
                    continue;
                }
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

    // One picture encoded to stream 0 and taken.
    void one(const std::string& what, u32 handle, const Pic& p, const Param& e)
    {
        if (encode(what, handle, p, e) == 0) take(what, handle);
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

// CellPngTextInfo {txtType, keyword, text, textLength, languageTag, translatedKeyword} at `at`, its strings after it.
void text_info(u8* b, u32 at, u32 type, const char* key, const char* text, u32 length = 0, const char* lang = nullptr,
    const char* translated = nullptr)
{
    u32 s = at + 0x20;
    const auto str = [&](const char* v) -> u32 {
        if (!v) return 0;
        const u32 here = s;
        put_string(b, here, v);
        s += static_cast<u32>(std::strlen(v) + 1 + 7) & ~7u;
        return here;
    };
    const u32 words[6] = {type, str(key), str(text), length, str(lang), str(translated)};
    for (u32 k = 0; k < 6; k++) ps_write32(b, at + 4 * k, words[k]);
}

Observed half(const Setup& s, bool sony_half)
{
    namespace fs = std::filesystem;
    Run h(s, sony_half, "cellPngEnc");
    u8* b = h.b;
    using Pic = Run::Pic;

    // QueryAttr: the memory for each configuration, and its checks.
    h.write_config(640, 480, 8);
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
             Query{1000000, 1000000, 16, true, 0x1234, 0, "1000000 square, 16 bits with SPUs and 0x1234 more"},
             Query{640, 480, 12, false, 0, 0, "12 bits"}, Query{640, 480, 4, false, 0, 0, "4 bits"},
             Query{0, 480, 8, false, 0, 0, "no width"}, Query{640, 0, 8, false, 0, 0, "no height"},
             Query{1000001, 480, 8, false, 0, 0, "1000001 wide"}, Query{640, 1000001, 8, false, 0, 0, "1000001 high"},
             Query{640, 480, 8, false, 0x80000000u, 0, "addMemSize 2 GB"},
             Query{640, 480, 8, false, 0x7FFFFFFFu, 0, "addMemSize 2 GB less a byte"},
             Query{640, 480, 8, false, 0, 1, "an extended parameter"}}) {
        h.write_config(q.w, q.h, q.depth, q.spu, q.add, q.ex);
        // CellPngEncExParam {index 0, value -> 0}, for the configuration that has one.
        std::memset(b + kChunkData, 0, 16);
        ps_write32(b, kChunkData + 4, kChunkData + 8);
        h.query(q.what);
    }

    // Open's and OpenEx's checks.
    h.write_config(64, 64, 16);
    const u32 size = h.query("for Open's checks");
    const u32 mem = std::min(size ? size : 0x100000u, kEncoderMem);
    // An Open that should be refused; one that is not is closed again.
    const auto open_refused = [&](const std::string& what, u32 cfg, u32 rsrc, u32 handle, bool ex = false) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = h.r(ex ? "cellPngEncOpenEx" : "cellPngEncOpen", {cfg, rsrc, handle},
            (ex ? "OpenEx " : "Open ") + what);
        h.o.line((ex ? "OpenEx " : "Open ") + what + ": the handle word", h.handle_text(ps_read32(b, kHandle)));
        if (r == 0) h.r("cellPngEncClose", {ps_read32(b, kHandle)}, "Close after Open " + what);
    };
    h.write_resource(kWork, mem > 0 ? mem - 1 : 0);
    open_refused("with a byte too little memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 0xC00);
    open_refused("with PPU priority 0xC00", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, -1);
    open_refused("with PPU priority -1", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0x100);
    open_refused("with SPU priority 0x100", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, -1);
    open_refused("with SPU priority -1", kCfg, kRsrc, kHandle);
    h.write_resource(0, mem);
    open_refused("without memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, 0);
    open_refused("with no memory size", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem);
    open_refused("without a handle pointer", kCfg, kRsrc, 0);
    open_refused("without a resource", kCfg, 0, kHandle);
    open_refused("without a configuration", 0, kRsrc, kHandle);
    h.write_config(64, 64, 12);
    open_refused("with a configuration of 12 bits", kCfg, kRsrc, kHandle);
    h.write_config(64, 64, 16);
    // CellPngEncResourceEx {memAddr, memSize, ppuThreadPriority, spurs, priority[8]}.
    std::memset(b + kRsrc, 0, 28);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, mem);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write8(b, kRsrc + 16 + 3, 16);
    open_refused("with a priority of 16", kCfg, kRsrc, kHandle, true);
    ps_write8(b, kRsrc + 16 + 3, 1);
    ps_write32(b, kRsrc + 8, 0xC00);
    open_refused("with PPU priority 0xC00", kCfg, kRsrc, kHandle, true);
    open_refused("without a resource", kCfg, 0, kHandle, true);

    // The encoder for the pictures, with memory for the ancillary chunks.
    const u32 hd = h.open("64x64 16 bits with 0x20000 more", 1, 64, 64, 16, false, 0x20000);
    if (!hd) return std::move(h.o);

    // Pictures in every colour space and depth (test_imgenc.cpp's), each taken from the queue and read back.
    fill(b, 24 * 3, 1);
    h.one("RGBA 5x3, rows of 24 bytes, level 9, every filter", hd, {5, 3, 24, RGBA, 8}, {RGBA, 9, 0xF8});
    fill(b, 32, 7);
    h.one("ARGB 4x2, level 0, no filter", hd, {4, 2, 16, ARGB, 8}, {ARGB, 0, 0x08});
    h.one("ARGB 4x2 to RGB, filter sub", hd, {4, 2, 16, ARGB, 8}, {RGB, 6, 0x10});
    h.one("RGBA 4x2 to RGB, rows of 12 bytes", hd, {4, 2, 12, RGBA, 8}, {RGB, 6, 0x08});
    fill(b, 18, 3);
    h.one("RGB 3x2, filter up", hd, {3, 2, 9, RGB, 8}, {RGB, 6, 0x20});
    fill(b, 32, 5);
    h.one("RGBA 2x2 16 bits, filter average", hd, {2, 2, 16, RGBA, 16}, {RGBA, 6, 0x40});
    h.one("RGBA 2x2 16 bits to RGB", hd, {2, 2, 16, RGBA, 16}, {RGB, 6, 0x40});
    h.one("ARGB 2x2 16 bits to RGB", hd, {2, 2, 16, ARGB, 16}, {RGB, 6, 0x40});
    fill(b, 4, 9);
    h.one("gray 4x1, filter Paeth", hd, {4, 1, 4, GRAY, 8}, {GRAY, 6, 0x80});
    fill(b, 8, 9);
    h.one("gray and alpha 4x1 to gray", hd, {4, 1, 8, GRAY_ALPHA, 8}, {GRAY, 6, 0xF8});
    h.one("gray and alpha 4x1", hd, {4, 1, 8, GRAY_ALPHA, 8}, {GRAY_ALPHA, 6, 0xF8});
    h.one("gray and alpha 2x1 16 bits to gray", hd, {2, 1, 8, GRAY_ALPHA, 16}, {GRAY, 6, 0xF8});
    for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(i % 16));
    h.one("gray 5x2 4 bits, a sample a byte", hd, {5, 2, 8, GRAY, 4}, {GRAY, 6, 0x08});
    const u8 packed[6] = {0x12, 0x34, 0x50, 0xAB, 0xCD, 0xE0};
    std::memcpy(b + kPixels, packed, 6);
    h.one("gray 5x2 4 bits packed", hd, {5, 2, 3, GRAY, 4, true}, {GRAY, 6, 0x08});
    ps_write8(b, kPixels, 0xA5);
    ps_write8(b, kPixels + 1, 0x80);
    h.one("gray 10x1 1 bit packed", hd, {10, 1, 2, GRAY, 1, true}, {GRAY, 6, 0x08});
    for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(i % 4));
    h.one("gray 8x2 2 bits, a sample a byte", hd, {8, 2, 8, GRAY, 2}, {GRAY, 6, 0xF8});
    for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(0xF0 | (i & 1)));
    h.one("gray 16x1 1 bit, a sample a byte (the high bits set)", hd, {16, 1, 16, GRAY, 1}, {GRAY, 6, 0x08});
    fill(b, 8 * 16, 11);
    h.one("gray 8x8 16 bits", hd, {8, 8, 16, GRAY, 16}, {GRAY, 6, 0xF8});
    h.one("gray and alpha 8x8 16 bits", hd, {8, 8, 32, GRAY_ALPHA, 16}, {GRAY_ALPHA, 6, 0xF8});
    h.one("RGB 4x4 16 bits", hd, {4, 4, 24, RGB, 16}, {RGB, 6, 0xF8});
    h.one("ARGB 4x4 16 bits", hd, {4, 4, 32, ARGB, 16}, {ARGB, 6, 0xF8});

    // A palette with its PLTE and tRNS; a 16-colour palette at 4 bits with its histogram and background.
    {
        const u8 idx[6] = {0, 1, 2, 2, 1, 0};
        std::memcpy(b + kPixels, idx, 6);
        std::memset(b + kChunkData, 0, 0x200);
        ps_write32(b, kChunkData, 3);
        ps_write32(b, kChunkData + 4, kChunkData + 0x40);
        const u8 rgb[9] = {255, 0, 0, 0, 255, 0, 0, 0, 255};
        std::memcpy(b + kChunkData + 0x40, rgb, 9);
        ps_write32(b, kChunkData + 0x20, kChunkData + 0x80);
        ps_write32(b, kChunkData + 0x24, 3);
        const u8 alpha[3] = {0, 128, 255};
        std::memcpy(b + kChunkData + 0x80, alpha, 3);
        chunks(b, {{PLTE, kChunkData}, {TRNS, kChunkData + 0x20}});
        h.one("a palette 3x2 with PLTE and tRNS", hd, {3, 2, 3, PALETTE, 8}, {PALETTE, 6, 0x08, 2});
        ps_write32(b, kChunkData + 0x24, 2);
        h.one("a palette 3x2 with PLTE and two of tRNS", hd, {3, 2, 3, PALETTE, 8}, {PALETTE, 6, 0x08, 2});
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
        h.one("a palette 8x8 of 16 colours at 4 bits with hIST and bKGD", hd, {8, 8, 8, PALETTE, 4},
            {PALETTE, 6, 0xF8, 3});
        // hIST naming fewer entries than the palette has; bKGD past the bit depth, then at its last value.
        ps_write32(b, kChunkData + 0x104, 5);
        ps_write8(b, kChunkData + 0x110, 16);
        h.one("a palette 8x8, hIST of 5 entries, bKGD index 16", hd, {8, 8, 8, PALETTE, 4}, {PALETTE, 6, 0xF8, 3});
        ps_write8(b, kChunkData + 0x110, 15);
        h.one("a palette 8x8, hIST of 5 entries, bKGD index 15", hd, {8, 8, 8, PALETTE, 4}, {PALETTE, 6, 0xF8, 3});
        ps_write32(b, kChunkData, 15);
        h.one("a palette of 15 at 4 bits, bKGD index 15", hd, {8, 8, 8, PALETTE, 4}, {PALETTE, 6, 0xF8, 3});
        ps_write32(b, kChunkData, 16);
        // A PLTE on an RGB picture (a suggested palette) and on a gray one.
        chunks(b, {{PLTE, kChunkData}});
        fill(b, 48, 2);
        h.one("RGB 4x4 with a PLTE", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0x08, 1});
        h.one("gray 4x4 with a PLTE", hd, {4, 4, 4, GRAY, 8}, {GRAY, 6, 0x08, 1});
    }

    // Ancillary chunks: text (tEXt, zTXt, iTXt), time, gamma, physical size.
    {
        std::memset(b + kChunkData, 0, 0x1000);
        text_info(b, kChunkData, 0, "Title", "pspocket");
        text_info(b, kChunkData + 0x100, 1, "Comment", "a longer text, compressed, a longer text, compressed");
        text_info(b, kChunkData + 0x200, 2, "Author", "nobody", 0, "en", "Auteur");
        text_info(b, kChunkData + 0x300, 0, "  Spaced   key  ", "text after a spaced keyword");
        text_info(b, kChunkData + 0x400, 0, "Length", "a text whose length says 4", 4);
        text_info(b, kChunkData + 0x500, 1, "Empty", "");
        text_info(b, kChunkData + 0x600, 2, "NoTranslation", "iTXt without a translated keyword", 0, "fr");
        ps_write16(b, kChunkData + 0x800, 2026);
        const u8 when[5] = {10, 9, 12, 34, 56};
        std::memcpy(b + kChunkData + 0x802, when, 5);
        ps_writef64(b, kChunkData + 0x810, 0.45455);
        ps_write32(b, kChunkData + 0x820, 2835);
        ps_write32(b, kChunkData + 0x824, 2835);
        ps_write32(b, kChunkData + 0x828, 1);
        std::memset(b + kPixels, 0x40, 64);
        const char* text_names[7] = {"a tEXt", "a zTXt", "an iTXt", "a tEXt with a spaced keyword",
            "a tEXt whose length says 4", "an empty zTXt", "an iTXt without a translated keyword"};
        for (u32 t = 0; t < 7; t++) {
            chunks(b, {{TEXT, kChunkData + 0x100 * t}});
            h.one(std::string("gray 4x4 with ") + text_names[t], hd, {4, 4, 4, GRAY, 8}, {GRAY, 6, 0x08, 1});
        }
        chunks(b, {{TEXT, kChunkData}, {TEXT, kChunkData + 0x100}, {TEXT, kChunkData + 0x200},
                      {TIME, kChunkData + 0x800}, {GAMA, kChunkData + 0x810}, {PHYS, kChunkData + 0x820}});
        h.one("gray 4x4 with texts, time, gamma and physical size", hd, {4, 4, 4, GRAY, 8}, {GRAY, 6, 0xF8, 6});

        // cHRM, sRGB, sBIT, bKGD (RGB), oFFs, sCAL, tRNS (a colour) on RGB.
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
        h.one("RGB 4x4 with cHRM, sRGB, sBIT, bKGD, oFFs, sCAL and tRNS", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0xF8, 7});
        // The same chunks given twice: the later one counts.
        ps_write32(b, kChunkData + 0x140, 3);
        ps_write32(b, kChunkData + 0x144, static_cast<u32>(-7));
        ps_write32(b, kChunkData + 0x148, 1);
        chunks(b, {{OFFS, kChunkData + 0x90}, {OFFS, kChunkData + 0x140}, {SRGB, kChunkData + 0x40},
                      {SRGB, kChunkData + 0x98}});
        h.one("RGB 4x4 with oFFs and sRGB twice", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0xF8, 4});
        // 16-bit tRNS and bKGD values on an 8-bit picture (libpng leaves them out); on a 16-bit one.
        ps_write16(b, kChunkData + 0xD8, 0x1234);
        ps_write32(b, kChunkData + 0x74, 0x1234);
        chunks(b, {{TRNS, kChunkData + 0xD0}, {BKGD, kChunkData + 0x70}});
        h.one("RGB 4x4 with 16-bit tRNS and bKGD", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0xF8, 2});
        h.one("RGB 2x2 16 bits with 16-bit tRNS and bKGD", hd, {2, 2, 12, RGB, 16}, {RGB, 6, 0xF8, 2});
        // Gray tRNS and bKGD past the bit depth, at 4 bits; sBIT of gray and alpha.
        ps_write16(b, kChunkData + 0xDE, 20);
        ps_write32(b, kChunkData + 0x80, 3);
        for (u32 i = 0; i < 16; i++) ps_write8(b, kPixels + i, static_cast<u8>(i));
        chunks(b, {{TRNS, kChunkData + 0xD0}});
        h.one("gray 4x4 4 bits with tRNS 20", hd, {4, 4, 4, GRAY, 4}, {GRAY, 6, 0x08, 1});
        ps_write16(b, kChunkData + 0xDE, 9);
        h.one("gray 4x4 4 bits with tRNS 9", hd, {4, 4, 4, GRAY, 4}, {GRAY, 6, 0x08, 1});
        chunks(b, {{BKGD, kChunkData + 0x70}});
        h.one("gray 4x4 4 bits with bKGD 3", hd, {4, 4, 4, GRAY, 4}, {GRAY, 6, 0x08, 1});
        const u32 sbit_ga[5] = {0, 0, 0, 7, 6};
        for (u32 i = 0; i < 5; i++) ps_write32(b, kChunkData + 0x50 + 4 * i, sbit_ga[i]);
        chunks(b, {{SBIT, kChunkData + 0x50}});
        fill(b, 32, 4);
        h.one("gray and alpha 4x4 with sBIT", hd, {4, 4, 8, GRAY_ALPHA, 8}, {GRAY_ALPHA, 6, 0x08, 1});
        chunks(b, {{TRNS, kChunkData + 0xD0}});
        h.one("gray and alpha 4x4 with a tRNS", hd, {4, 4, 8, GRAY_ALPHA, 8}, {GRAY_ALPHA, 6, 0x08, 1});

        // sCAL's numbers as libpngenc prints them, one picture each.
        fill(b, 3, 6);
        for (const double v : {0.0, 1.0, 3.0, 9.9999995, 9.999999, 0.99999995, 1234.5678, 123456789.0, 1e-10, 2.5e-7,
                 0.1, 7.0000005, 1.5e+20}) {
            ps_write32(b, kChunkData + 0x200, 2);
            ps_writef64(b, kChunkData + 0x208, v);
            ps_writef64(b, kChunkData + 0x210, v * 3);
            chunks(b, {{SCAL, kChunkData + 0x200}});
            h.one(std::format("RGB 1x1 with sCAL {:.10g}", v), hd, {1, 1, 3, RGB, 8}, {RGB, 6, 0x08, 1});
        }
        // gAMA alone up to its largest.
        for (const double v : {1.0, 10.0, 1000.0, 21474.0, 21474.8, 21474.82, 21474.829}) {
            ps_writef64(b, kChunkData + 0x220, v);
            chunks(b, {{GAMA, kChunkData + 0x220}});
            h.one(std::format("RGB 1x1 with gAMA {:.10g}", v), hd, {1, 1, 3, RGB, 8}, {RGB, 6, 0x08, 1});
        }
        // cHRM's white point at 0.8 (the float above it).
        for (u32 i = 0; i < 8; i++) ps_writef64(b, kChunkData + 0x240 + 8 * i, i == 0 ? 0.8 : i == 1 ? 0.2 : 0.3);
        chunks(b, {{CHRM, kChunkData + 0x240}});
        h.one("RGB 1x1 with a cHRM white point of 0.8, 0.2", hd, {1, 1, 3, RGB, 8}, {RGB, 6, 0x08, 1});
        // gAMA and cHRM's numbers (times 100000, rounded).
        for (const double v : {0.45455, 1.0 / 2.2, 0.000005, 0.000015, 0.123455, 2.0, 0.3, 0.7}) {
            ps_writef64(b, kChunkData + 0x220, v);
            for (u32 i = 0; i < 8; i++) ps_writef64(b, kChunkData + 0x240 + 8 * i, i % 2 ? v / 4 : v / 2);
            chunks(b, {{GAMA, kChunkData + 0x220}, {CHRM, kChunkData + 0x240}});
            h.one(std::format("RGB 1x1 with gAMA and cHRM of {:.10g}", v), hd, {1, 1, 3, RGB, 8}, {RGB, 6, 0x08, 2});
        }

        // iCCP alone: a profile of only its header (an RGB display profile, no tags).
        std::memset(b + kChunkData, 0, 0x400);
        put_string(b, kChunkData + 0x100, "a profile");
        const u32 icc = kChunkData + 0x140;
        ps_write32(b, icc, 132);             // its size
        ps_write32(b, icc + 8, 0x02100000);  // version 2.1
        ps_write32(b, icc + 12, 0x6D6E7472); // 'mntr'
        ps_write32(b, icc + 16, 0x52474220); // 'RGB '
        ps_write32(b, icc + 20, 0x58595A20); // 'XYZ '
        ps_write32(b, icc + 36, 0x61637370); // 'acsp'
        ps_write32(b, icc + 68, 0x0000F6D6); // the D50 illuminant
        ps_write32(b, icc + 72, 0x00010000);
        ps_write32(b, icc + 76, 0x0000D32D);
        ps_write32(b, kChunkData + 0xC0, kChunkData + 0x100); // iCCP {name, profile, size}
        ps_write32(b, kChunkData + 0xC4, icc);
        ps_write32(b, kChunkData + 0xC8, 132);
        chunks(b, {{ICCP, kChunkData + 0xC0}});
        fill(b, 48, 13);
        h.one("RGB 4x4 with iCCP", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0xF8, 1});

        // sPLT (8 and 16 bits), pCAL, unknown chunks before PLTE, after it, after IDAT and nowhere.
        std::memset(b + kChunkData, 0, 0x1000);
        put_string(b, kChunkData + 0x400, "eight");
        put_string(b, kChunkData + 0x410, "sixteen");
        for (u32 i = 0; i < 3; i++)
            for (u32 k = 0; k < 5; k++) {
                ps_write16(b, kChunkData + 0x500 + 10 * i + 2 * k, static_cast<u16>(i * 40 + k * 11));
                ps_write16(b, kChunkData + 0x540 + 10 * i + 2 * k, static_cast<u16>(i * 4000 + k * 1100 + 7));
            }
        // CellPngSPLT {paletteName, sampleDepth (u8), paletteEntries, paletteEntriesNumber} x 2, then {list, 2}.
        ps_write32(b, kChunkData + 0x300, kChunkData + 0x400);
        ps_write8(b, kChunkData + 0x304, 8);
        ps_write32(b, kChunkData + 0x308, kChunkData + 0x500);
        ps_write32(b, kChunkData + 0x30C, 3);
        ps_write32(b, kChunkData + 0x310, kChunkData + 0x410);
        ps_write8(b, kChunkData + 0x314, 16);
        ps_write32(b, kChunkData + 0x318, kChunkData + 0x540);
        ps_write32(b, kChunkData + 0x31C, 2);
        ps_write32(b, kChunkData + 0x380, kChunkData + 0x300);
        ps_write32(b, kChunkData + 0x384, 2);
        // CellPngPCAL {calibrationName, x0, x1, equationType, numberOfParameters, unitName, parameter}.
        put_string(b, kChunkData + 0x600, "calibration");
        put_string(b, kChunkData + 0x620, "metres");
        put_string(b, kChunkData + 0x640, "1.5");
        put_string(b, kChunkData + 0x650, "-2e3");
        ps_write32(b, kChunkData + 0x680, kChunkData + 0x640);
        ps_write32(b, kChunkData + 0x684, kChunkData + 0x650);
        const u32 pcal[7] = {kChunkData + 0x600, static_cast<u32>(-10), 1000, 1, 2, kChunkData + 0x620,
            kChunkData + 0x680};
        for (u32 i = 0; i < 7; i++) ps_write32(b, kChunkData + 0x6A0 + 4 * i, pcal[i]);
        // CellPngUnknownChunk {chunkType[5], chunkData (+8), length, location}.
        const auto unknown = [&](u32 at, const char* name, u32 location, u32 data_at, u32 length) {
            std::memcpy(b + at, name, 5);
            ps_write32(b, at + 8, data_at);
            ps_write32(b, at + 12, length);
            ps_write32(b, at + 16, location);
        };
        for (u32 i = 0; i < 32; i++) ps_write8(b, kChunkData + 0x800 + i, static_cast<u8>(0xA0 + i));
        unknown(kChunkData + 0x700, "prVa", 1, kChunkData + 0x800, 5);
        unknown(kChunkData + 0x720, "prVb", 2, kChunkData + 0x800, 6);
        unknown(kChunkData + 0x740, "prVc", 8, kChunkData + 0x800, 7);
        unknown(kChunkData + 0x760, "prVd", 0, kChunkData + 0x800, 8);
        unknown(kChunkData + 0x780, "prVE", 1, kChunkData + 0x800, 9);
        unknown(kChunkData + 0x7A0, "prVf", 3, kChunkData + 0x800, 10);
        fill(b, 48, 21);
        chunks(b, {{SPLT, kChunkData + 0x380}});
        h.one("RGB 4x4 with two sPLT", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0x08, 1});
        chunks(b, {{PCAL, kChunkData + 0x6A0}});
        h.one("RGB 4x4 with a pCAL", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0x08, 1});
        for (u32 u = 0; u < 6; u++) {
            chunks(b, {{UNKNOWN, kChunkData + 0x700 + 0x20 * u}});
            h.one(std::format("RGB 4x4 with unknown chunk {}", u), hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0x08, 1});
        }
        chunks(b, {{UNKNOWN, kChunkData + 0x700}, {UNKNOWN, kChunkData + 0x720}, {UNKNOWN, kChunkData + 0x740},
                      {UNKNOWN, kChunkData + 0x7A0}, {SPLT, kChunkData + 0x380}, {PCAL, kChunkData + 0x6A0}});
        h.one("RGB 4x4 with sPLT, pCAL and unknown chunks", hd, {4, 4, 12, RGB, 8}, {RGB, 6, 0x08, 6});
    }

    // The memory the chunks take, against addMemSize: a tEXt ("Title", "pspocket") takes 0x150 bytes.
    {
        std::memset(b + kChunkData, 0, 0x200);
        text_info(b, kChunkData, 0, "Title", "pspocket");
        chunks(b, {{TEXT, kChunkData}});
        std::memset(b + kPixels, 0x55, 16);
        for (const u32 add : {0x150u, 0x14Fu, 0u}) {
            const u32 e = h.open(std::format("8x8 8 bits with {:#x} more", add), 2, 8, 8, 8, false, add);
            if (!e) continue;
            h.one(std::format("gray 4x4 with a tEXt, {:#x} more", add), e, {4, 4, 4, GRAY, 8}, {GRAY, 6, 0x08, 1});
            h.r("cellPngEncClose", {e}, std::format("Close of the encoder with {:#x} more", add));
        }
    }

    // A larger picture under each filter set and several levels: Sony's filter choice and zlib's settings (notes).
    gradient_rgba(b, 64, 64);
    for (const u32 filters : {0x08u, 0x10u, 0x20u, 0x40u, 0x80u, 0x30u, 0xF8u})
        h.one(std::format("RGBA 64x64 ramps, filters {:#x}", filters), hd, {64, 64, 256, RGBA, 8}, {RGBA, 6, filters});
    for (const u32 level : {0u, 1u, 9u})
        h.one(std::format("RGBA 64x64 ramps, level {}", level), hd, {64, 64, 256, RGBA, 8}, {RGBA, level, 0xF8});
    h.one("RGBA 64x64 ramps to RGB", hd, {64, 64, 256, RGBA, 8}, {RGB, 6, 0xF8});

    // The queues: up to four streams waiting, then commands wait (four at most); Reset turns the commands waiting into
    // streams; the next picture after it is skipped.
    h.wait_output("with nothing waiting", hd, false);
    h.r("cellPngEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo with nothing waiting");
    h.r("cellPngEncWaitForInput", {hd, 0}, "WaitForInput with nothing waiting");
    std::memset(b + kPixels, 0x22, 64);
    // Until `n` streams wait (Sony's encodes on a thread of its own): how many do, after at most two seconds.
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
    for (u32 i = 0; i < 4; i++) {
        h.r("cellPngEncWaitForInput", {hd, 1}, std::format("WaitForInput before picture {}", i));
        Pic p{4, 4, 16, RGBA, 8};
        p.user = 0x100 + i;
        h.encode(std::format("queued picture {}", i), hd, p, {RGBA, 6, 0xF8}, i);
    }
    h.o.line("the four queued pictures encoded", std::format("{}", ready(4)));
    for (u32 i = 4; i < 9; i++) {
        h.r("cellPngEncWaitForInput", {hd, 0}, std::format("WaitForInput before command {}, not blocking", i));
        Pic p{4, 4, 16, RGBA, 8};
        p.user = 0x100 + i;
        h.encode(std::format("command {} with four streams waiting", i), hd, p, {RGBA, 6, 0xF8}, i == 8 ? 6 : i);
    }
    h.o.line("streams waiting after the commands", std::format("{}", ready(5)));
    h.wait_output("with four waiting and four commands", hd, false);
    h.r("cellPngEncReset", {hd}, "Reset with four streams and four commands waiting");
    h.o.line("streams waiting after Reset", std::format("{}", ready(8)));
    h.r("cellPngEncReset", {hd}, "Reset with eight streams waiting");
    for (u32 i = 0; i < 8; i++) h.stream_info(std::format("stream {} after Reset", i), hd);
    h.wait_output("with all taken", hd, false);
    h.one("the first picture after Reset", hd, {4, 4, 16, RGBA, 8}, {RGBA, 6, 0xF8});
    h.one("the second picture after Reset", hd, {4, 4, 16, RGBA, 8}, {RGBA, 6, 0xF8});
    h.r("cellPngEncReset", {hd + 4}, "Reset of no handle");
    h.r("cellPngEncReset", {0}, "Reset(NULL)");
    h.r("cellPngEncReset", {hd}, "Reset with nothing waiting");
    h.one("the first picture after an idle Reset", hd, {4, 4, 16, RGBA, 8}, {RGBA, 6, 0xF8});
    h.one("the second picture after an idle Reset", hd, {4, 4, 16, RGBA, 8}, {RGBA, 6, 0xF8});

    // A buffer too small: what the stream information says, what was written.
    gradient_rgba(b, 64, 64);
    for (const u32 limit : {0x31u, 0x40u, 0x200u}) {
        if (h.encode(std::format("to a buffer of {:#x} bytes", limit), hd, {64, 64, 256, RGBA, 8}, {RGBA, 6, 0x08}, 5,
                limit) != 0)
            continue;
        h.wait_output(std::format("to a buffer of {:#x} bytes", limit), hd, true);
        std::memset(b + kInfo, kFill, 48);
        h.r("cellPngEncGetStreamInfo", {hd, kInfo}, std::format("GetStreamInfo to a buffer of {:#x} bytes", limit));
        h.o.line(std::format("GetStreamInfo to a buffer of {:#x} bytes: the information", limit), hex(b + kInfo, 48));
        // The signature, IHDR and the IDAT chunk's type (its length and data are the compressor's), the bytes past.
        h.o.line(std::format("the buffer of {:#x} bytes: its first 37 bytes", limit), hex(b + stream_at(5), 37));
        h.o.line(std::format("the buffer of {:#x} bytes: the rest", limit), hex(b + stream_at(5) + 37, limit - 37));
        h.o.line(std::format("the buffer of {:#x} bytes: the 16 bytes past it", limit), hex(b + stream_at(5) + limit, 16));
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

    // EncodePicture's checks.
    {
        const Pic good{4, 4, 16, RGBA, 8};
        h.encode("of no handle", hd + 1, good, {RGBA});
        h.r("cellPngEncEncodePicture", {hd, 0, kParam, kOutParam}, "EncodePicture without a picture");
        h.encode("of no width", hd, {0, 4, 16, RGBA, 8}, {RGBA});
        h.encode("of no height", hd, {4, 0, 16, RGBA, 8}, {RGBA});
        h.encode("past maxWidth", hd, {65, 4, 260, RGBA, 8}, {RGBA});
        h.encode("past maxHeight", hd, {4, 65, 16, RGBA, 8}, {RGBA});
        h.encode("of colour space 3", hd, {4, 4, 16, 3, 8}, {3});
        h.encode("of colour space 0", hd, {4, 4, 16, 0, 8}, {0});
        h.encode("packed at 8 bits", hd, {4, 4, 16, RGBA, 8, true}, {RGBA});
        h.encode("of RGB at 4 bits", hd, {4, 4, 4, RGB, 4}, {RGB});
        h.encode("of gray at 3 bits", hd, {4, 4, 4, GRAY, 3}, {GRAY});
        h.encode("of gray and alpha at 4 bits", hd, {4, 4, 4, GRAY_ALPHA, 4}, {GRAY_ALPHA});
        h.encode("of a palette at 16 bits", hd, {4, 4, 8, PALETTE, 16}, {PALETTE});
        h.encode("with a pitch shorter than a row", hd, {4, 4, 15, RGBA, 8}, {RGBA});
        if (h.encode("RGBA to RGB with a pitch shorter than an RGBA row", hd, {4, 4, 13, RGBA, 8}, {RGB, 6, 0x08}) == 0)
            h.take("RGBA to RGB with a pitch shorter than an RGBA row", hd);
        h.encode("RGBA to RGB with a pitch shorter than an RGB row", hd, {4, 4, 11, RGBA, 8}, {RGB, 6, 0x08});
        h.encode("of colour to gray", hd, good, {GRAY});
        h.encode("of colour to a palette", hd, good, {PALETTE});
        h.encode("of RGB to RGBA", hd, {4, 4, 12, RGB, 8}, {RGBA});
        h.encode("of ARGB to RGBA", hd, {4, 4, 16, ARGB, 8}, {RGBA});
        h.wait_output("after the colour checks", hd, false);
        h.encode("of RGBA to ARGB", hd, good, {ARGB});
        h.encode("of gray to gray and alpha", hd, {4, 4, 4, GRAY, 8}, {GRAY_ALPHA});
        h.encode("of gray to RGB", hd, {4, 4, 4, GRAY, 8}, {RGB});
        h.encode("at level 10", hd, good, {RGBA, 10});
        h.encode("with no filter bit", hd, good, {RGBA, 6, 0});
        h.encode("with filter bits 0x01", hd, good, {RGBA, 6, 0x09});
        h.encode("with filter bits 0x100", hd, good, {RGBA, 6, 0x108});
        h.encode("on the SPUs, not opened for them", hd, good, {RGBA, 6, 0xF8, 0, true});
        h.encode("to a buffer of no size", hd, good, {RGBA}, 6, 0);
        h.encode("to a buffer of 0x30 bytes", hd, good, {RGBA}, 6, 0x30);
        ps_write32(b, kOutParam, 2); // the rest as for the buffer of 0x30 bytes
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to location 2");
        ps_write32(b, kOutParam, 1);
        ps_write32(b, kOutParam + 8, 0);
        ps_write32(b, kOutParam + 12, 0x1000);
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to a buffer at no address");
        ps_write32(b, kOutParam, 0);
        ps_write32(b, kOutParam + 4, 0);
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to a file without a name");
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, 0}, "EncodePicture without an output");
        ps_write32(b, kPic + 24, 0);
        ps_write32(b, kOutParam, 1);
        ps_write32(b, kOutParam + 8, stream_at(6));
        h.r("cellPngEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture of a picture at no address");
        // The chunks' checks, each on its own (a palette picture where they need one).
        std::memset(b + kChunkData, 0, 0x1000);
        ps_write32(b, kChunkData + 0xF00, 4); // a 4-colour PLTE
        ps_write32(b, kChunkData + 0xF04, kChunkData + 0xF40);
        const auto chunk_refused = [&](const std::string& what, std::initializer_list<std::pair<u32, u32>> list,
                                       u32 space = RGB, u32 depth = 8) {
            chunks(b, list);
            const u32 comps = space == RGB ? 3 : space == RGBA ? 4 : space == GRAY_ALPHA ? 2 : 1;
            if (h.encode("with " + what, hd, {4, 4, 4 * comps * (depth / 8 ? depth / 8 : 1), space, depth},
                    {space, 6, 0x08, static_cast<u32>(list.size())}) == 0)
                h.take("with " + what, hd);
        };
        const u32 c = kChunkData; // the chunk under test at c, its strings and arrays after it
        const u32 plte = kChunkData + 0xF00;
        chunk_refused("a chunk of type 17", {{17, c}});
        chunk_refused("a chunk without data", {{GAMA, 0}});
        ps_writef64(b, c, 0.0);
        chunk_refused("gAMA 0", {{GAMA, c}});
        ps_writef64(b, c, -0.5);
        chunk_refused("gAMA -0.5", {{GAMA, c}});
        ps_writef64(b, c, 1e10);
        chunk_refused("gAMA 1e10", {{GAMA, c}});
        ps_writef64(b, c, 21474.83);
        chunk_refused("gAMA 21474.83", {{GAMA, c}});
        ps_writef64(b, c, 21474.84);
        chunk_refused("gAMA 21474.84", {{GAMA, c}});
        for (u32 i = 0; i < 8; i++) ps_writef64(b, c + 8 * i, 0.3);
        ps_writef64(b, c, 0.81);
        chunk_refused("cHRM with a white point x of 0.81", {{CHRM, c}});
        ps_writef64(b, c, 0.8);
        chunk_refused("cHRM with a white point x of 0.8", {{CHRM, c}});
        ps_writef64(b, c + 16, 0.71);
        ps_writef64(b, c + 24, 0.3);
        chunk_refused("cHRM with red x + y above 1", {{CHRM, c}});
        ps_writef64(b, c + 16, -0.01);
        chunk_refused("cHRM with a red x below 0", {{CHRM, c}});
        std::memset(b + c, 0, 0x100);
        chunk_refused("iCCP without a name", {{ICCP, c}});
        ps_write32(b, c, c + 0x40);
        put_string(b, c + 0x40, "p");
        ps_write32(b, c + 8, 16);
        chunk_refused("iCCP without a profile", {{ICCP, c}});
        ps_write32(b, c + 4, c + 0x80);
        ps_write32(b, c + 8, 0);
        chunk_refused("iCCP of no length", {{ICCP, c}});
        std::memset(b + c, 0, 0x100);
        const u32 sbit_rgb[5] = {9, 8, 8, 0, 0};
        for (u32 i = 0; i < 5; i++) ps_write32(b, c + 4 * i, sbit_rgb[i]);
        chunk_refused("sBIT red 9 at 8 bits", {{SBIT, c}});
        ps_write32(b, c, 0);
        chunk_refused("sBIT red 0", {{SBIT, c}});
        ps_write32(b, c, 8);
        chunk_refused("sBIT of RGB", {{SBIT, c}});
        chunk_refused("sBIT of RGBA without alpha", {{SBIT, c}}, RGBA);
        ps_write32(b, c + 12, 9);
        chunk_refused("sBIT gray 9 on gray", {{SBIT, c}}, GRAY);
        ps_write32(b, c + 16, 3);
        chunk_refused("sBIT of a palette, red 8", {{PLTE, plte}, {SBIT, c}}, PALETTE);
        ps_write32(b, c, 9);
        chunk_refused("sBIT of a palette, red 9", {{PLTE, plte}, {SBIT, c}}, PALETTE);
        ps_write32(b, c, 4);
        chunk_refused("sRGB 4", {{SRGB, c}});
        ps_write32(b, c, 3);
        chunk_refused("sRGB 3", {{SRGB, c}});
        std::memset(b + c, 0, 0x100);
        put_string(b, c + 0x40, "key");
        put_string(b, c + 0x50, "");
        ps_write32(b, c, 3);
        ps_write32(b, c + 4, c + 0x40);
        chunk_refused("a text of type 3", {{TEXT, c}});
        ps_write32(b, c, 0);
        ps_write32(b, c + 4, 0);
        chunk_refused("a text without a keyword", {{TEXT, c}});
        ps_write32(b, c + 4, c + 0x50);
        chunk_refused("a text with an empty keyword", {{TEXT, c}});
        ps_write32(b, c + 4, c + 0x40);
        chunk_refused("a tEXt without a text", {{TEXT, c}});
        ps_write32(b, c, 2);
        chunk_refused("an iTXt without a language", {{TEXT, c}});
        ps_write32(b, c + 16, c + 0x50);
        chunk_refused("an iTXt with an empty language", {{TEXT, c}});
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c + 16, 256);
        chunk_refused("a gray bKGD of 256", {{BKGD, c}}, GRAY);
        ps_write32(b, c + 16, 255);
        chunk_refused("a gray bKGD of 255", {{BKGD, c}}, GRAY);
        ps_write32(b, c + 8, 256);
        chunk_refused("an RGB bKGD of green 256", {{BKGD, c}});
        ps_write8(b, c, 5);
        chunk_refused("a palette bKGD of index 5 of 4", {{PLTE, plte}, {BKGD, c}}, PALETTE);
        chunk_refused("a palette bKGD before the PLTE", {{BKGD, c}, {PLTE, plte}}, PALETTE);
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c, c + 0x40);
        ps_write32(b, c + 4, 4);
        chunk_refused("a tRNS on RGBA", {{TRNS, c}}, RGBA);
        chunk_refused("a palette tRNS before the PLTE", {{TRNS, c}, {PLTE, plte}}, PALETTE);
        ps_write32(b, c + 4, 5);
        chunk_refused("a palette tRNS of 5 of 4", {{PLTE, plte}, {TRNS, c}}, PALETTE);
        ps_write32(b, c + 4, 0);
        chunk_refused("a palette tRNS of none", {{PLTE, plte}, {TRNS, c}}, PALETTE);
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c, c + 0x40);
        ps_write32(b, c + 4, 5);
        chunk_refused("a hIST of 5 of 4", {{PLTE, plte}, {HIST, c}}, PALETTE);
        chunk_refused("a hIST without a PLTE", {{HIST, c}});
        ps_write32(b, c + 4, 0);
        chunk_refused("a hIST of none", {{PLTE, plte}, {HIST, c}}, PALETTE);
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c + 8, 2);
        chunk_refused("pHYs of unit 2", {{PHYS, c}});
        chunk_refused("oFFs of unit 2", {{OFFS, c}});
        std::memset(b + c, 0, 0x100);
        ps_write16(b, c, 2026);
        const u8 bad_times[5][5] = {{0, 1, 0, 0, 0}, {13, 1, 0, 0, 0}, {1, 0, 0, 0, 0}, {1, 32, 0, 0, 0},
            {1, 1, 24, 0, 0}};
        for (const auto& t : bad_times) {
            std::memcpy(b + c + 2, t, 5);
            chunk_refused(std::format("tIME {}-{} {}:00", t[0], t[1], t[2]), {{TIME, c}});
        }
        const u8 edge_time[5] = {12, 31, 23, 59, 60};
        std::memcpy(b + c + 2, edge_time, 5);
        chunk_refused("tIME 12-31 23:59:60", {{TIME, c}});
        const u8 bad_minute[5] = {12, 31, 23, 60, 0};
        std::memcpy(b + c + 2, bad_minute, 5);
        chunk_refused("tIME 23:60", {{TIME, c}});
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c, 3);
        chunk_refused("sCAL of unit 3", {{SCAL, c}});
        ps_write32(b, c, 1);
        ps_writef64(b, c + 8, -1.0);
        chunk_refused("sCAL of width -1", {{SCAL, c}});
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c + 4, 4);
        chunk_refused("a PLTE without entries", {{PLTE, c}}, PALETTE);
        ps_write32(b, c + 4, c + 0x40);
        ps_write32(b, c, 0);
        chunk_refused("a PLTE of none", {{PLTE, c}}, PALETTE);
        ps_write32(b, c, 257);
        chunk_refused("a PLTE of 257 at 8 bits", {{PLTE, c}}, PALETTE);
        ps_write32(b, c, 5);
        chunk_refused("a PLTE of 5 at 2 bits", {{PLTE, c}}, PALETTE, 2);
        chunk_refused("a palette without its PLTE", {}, PALETTE);
        std::memset(b + c, 0, 0x100);
        std::memcpy(b + c, "prVa", 5);
        ps_write32(b, c + 12, 4);
        ps_write32(b, c + 16, 1);
        chunk_refused("an unknown chunk without data", {{UNKNOWN, c}});
        ps_write32(b, c + 8, c + 0x40);
        ps_write32(b, c + 12, 0);
        chunk_refused("an unknown chunk of no length", {{UNKNOWN, c}});
        ps_write32(b, c + 12, 4);
        ps_write32(b, c + 16, 4);
        chunk_refused("an unknown chunk at location 4", {{UNKNOWN, c}});
        // sPLT: its list, a depth of 4, an entry past 255 at 8 bits.
        std::memset(b + c, 0, 0x100);
        chunk_refused("an sPLT without a list", {{SPLT, c}});
        ps_write32(b, c, c + 0x40);
        ps_write32(b, c + 4, 1);
        ps_write32(b, c + 0x40, c + 0x80);
        put_string(b, c + 0x80, "s");
        ps_write8(b, c + 0x44, 4);
        ps_write32(b, c + 0x48, c + 0xA0);
        ps_write32(b, c + 0x4C, 1);
        chunk_refused("an sPLT of depth 4", {{SPLT, c}});
        ps_write8(b, c + 0x44, 8);
        ps_write16(b, c + 0xA4, 256);
        chunk_refused("an 8-bit sPLT entry of 256", {{SPLT, c}});
        std::memset(b + c, 0, 0x100);
        ps_write32(b, c, c + 0x40);
        put_string(b, c + 0x40, "p");
        chunk_refused("a pCAL without parameters", {{PCAL, c}});
        ps_write32(b, c + 16, 1);
        ps_write32(b, c + 12, 4);
        chunk_refused("a pCAL of equation type 4", {{PCAL, c}});
        // What the refusals left queued (a refused picture must not be).
        h.wait_output("after the checks", hd, false);
    }
    h.r("cellPngEncClose", {hd}, "Close");
    h.r("cellPngEncClose", {hd}, "Close again");
    h.r("cellPngEncWaitForInput", {hd, 0}, "WaitForInput after Close");
    h.r("cellPngEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo after Close");

    // With the SPUs: an encoder opened for them, a picture encoded on them; the widths the SPUs take.
    const u32 spu = h.open("64x64 8 bits with SPUs", 3, 64, 64, 8, true);
    if (spu) {
        gradient_rgba(b, 64, 64);
        if (h.encode("RGBA 64x64 ramps on the SPUs", spu, {64, 64, 256, RGBA, 8}, {RGBA, 6, 0xF8, 0, true}) == 0) {
            h.wait_output("RGBA 64x64 ramps on the SPUs", spu, true);
            h.stream_info("RGBA 64x64 ramps on the SPUs", spu);
        }
        h.r("cellPngEncClose", {spu}, "Close of the SPU encoder");
    }
    for (const auto& [w, depth] : {std::pair{2560u, 8u}, std::pair{2561u, 8u}, std::pair{1280u, 16u},
             std::pair{1281u, 16u}}) {
        const u32 e = h.open(std::format("{} wide at {} bits with SPUs", w, depth), 4, w, 2, depth, true);
        if (!e) continue;
        if (h.encode(std::format("{} wide at {} bits on the SPUs", w, depth), e, {w, 1, w * 4 * depth / 8, RGBA, depth},
                {RGBA, 6, 0x08, 0, true}, 7) == 0) {
            h.wait_output(std::format("{} wide at {} bits on the SPUs", w, depth), e, true);
            std::memset(b + kInfo, kFill, 48);
            h.r("cellPngEncGetStreamInfo", {e, kInfo}, std::format("GetStreamInfo {} wide at {} bits", w, depth));
            h.o.line(std::format("GetStreamInfo {} wide at {} bits: its state and lines", w, depth),
                std::format("{:#x} {}", ps_read32(b, kInfo), ps_read32(b, kInfo + 24)));
        }
        h.r("cellPngEncClose", {e}, std::format("Close of the {}-wide SPU encoder", w));
    }

    // An encoder opened again over its own work memory: a new encoder there. Sony's Open builds it over the first one,
    // whose thread still runs, and creates its lwmutex where that thread's is: the waiter count starts again at 0.
    // Until the thread sleeps on its lwcond, a lock it is in the middle of counts a waiter on the old lwmutex and takes
    // it off the new one, which borrows from the owner (sys_lwmutex_lock finds 0xfffffffe or 0xfffffffc where it
    // expects its turn: a fatal one run in a few, D-934). Asleep there, nothing wakes it again (the next lwcond is a
    // new one). Its thread's ID is the handle's +0x10.
    const u32 again = h.open("an encoder to open twice", 5, 16, 16, 8, false);
    if (again) {
        if (h.side.sony()) {
            const u32 thread = static_cast<u32>(ps_read64(b, again + 0x10));
            bool asleep = false;
            for (int tries = 0; tries < 400 && !asleep; tries++) {
                const auto t = h.f.kernel->thread(thread);
                asleep = t && t->waiter.waiting_on().kind == lv2::WaitKind::LwCond;
                if (!asleep) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            REQUIRE(asleep);
        }
        h.write_resource(kWork + 5 * kEncoderMem + 0x10, kEncoderMem - 0x10);
        ps_write32(b, kHandle, 0xEEEEEEEE);
        h.r("cellPngEncOpen", {kCfg, kRsrc, kHandle}, "Open of the memory already open");
        h.o.line("Open of the memory already open: the handle", h.handle_text(ps_read32(b, kHandle)));
        h.wait_output("of the encoder opened twice", again, false);
    }
    fs::remove_all(root);
    return std::move(h.o);
}

} // namespace

// It matches (D-913): no longer hidden, it runs wherever the firmware is (the Mac's gate) and skips elsewhere.
TEST_CASE("cellPngEnc against Sony's libpngenc: sizes, handles, the queue, the streams, every PNG's chunks and pixels",
    "[hle][lle][compare][pngenc]")
{
    const Setup s = setup();
    run(s, "cellPngEnc against Sony's libpngenc", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: libpngenc's sizes, the handle in the work memory, test_imgenc.cpp's pictures.
            CHECK(o.value("QueryAttr 64x64 16 bits: the attribute") ==
                std::format("hex:{:08x}04eeeeee0027000000000000", 0x47A00 + 8 * 64 * 7 + 0x1780));
            CHECK(o.value("Open 64x64 16 bits with 0x20000 more: the handle") == "the work memory +0x100080");
            CHECK(o.value("stream RGBA 5x3, rows of 24 bytes, level 9, every filter: a whole PNG") == "yes");
            CHECK(o.value("stream RGBA 5x3, rows of 24 bytes, level 9, every filter: streamSize is the PNG's length") ==
                "yes");
            CHECK(o.value("stream RGBA 5x3, rows of 24 bytes, level 9, every filter: the chunks") == "IHDR IDAT IEND");
            CHECK(o.value("stream ARGB 4x2, level 0, no filter: IHDR") == "hex:00000004000000020806000000");
            const Data* argb = o.find_data("stream ARGB 4x2, level 0, no filter: the pixels");
            REQUIRE(argb);
            REQUIRE(argb->bytes.size() == 32);
            // ARGB's first pixel (bytes 7, 44, 81, 118 of the fill) written as RGBA.
            CHECK(argb->bytes[0] == static_cast<u8>(1 * 37 + 7));
            CHECK(argb->bytes[3] == static_cast<u8>(7));
            CHECK(o.value("stream gray 4x4 with texts, time, gamma and physical size: gAMA") == "hex:0000b18f");
            CHECK(o.value("stream a palette 3x2 with PLTE and tRNS: PLTE") == "hex:ff000000ff000000ff");
            CHECK(o.value("the four queued pictures encoded") == "4");
            CHECK(o.value("EncodePicture with no filter bit") == "0x80611291");
            CHECK(o.value("EncodePicture of RGB to RGBA") == "0x80611291");
            CHECK(o.value("stream to a file: libpng reads it") == "yes");
            CHECK(o.value("EncodePicture at level 10") == "0x80611291");
        });
}

#endif
