// cellJpgEnc against Sony's libjpgenc (D-616, D-618, D-913): the same configurations, resources, pictures and
// parameters go to pspocket's library and to Sony's module. Compared: QueryAttr's attributes; Open's, OpenEx's and
// Close's answers and the handle (as an offset into the work memory when it lies there); every answer of the two queues
// and every field of each stream's information; each JPEG byte for byte (libjpgenc is IJG's libjpeg 6b: its markers,
// tables and entropy-coded data are deterministic), with its markers, headers and tables as lines to read a difference
// by.
//
// The structures (Sony's, D-913): CellJpgEncConfig {maxWidth, maxHeight, encodeColorSpace, samplingFormat, enableSpu
// (b8, +16), exParamList, exParamNum}; CellJpgEncPicture {width, height, colorSpace, pictureAddr, userData (u64,
// +16)}; CellJpgEncPicture2 {width, height, pitchWidth, colorSpace, pictureAddr, (4 bytes), userData (u64, +24)};
// CellJpgEncEncodeParam {enableSpu (b8), restartInterval, dctMethod, compressionMode, compressionModeParam,
// exParamList, exParamNum}, each extended parameter {marker, data, size} (a marker segment); CellJpgEncOutputParam
// {location, streamFileName, streamAddr, limitSize}; CellJpgEncStreamInfo {state, location, streamFileName,
// streamAddr, limitSize, streamSize, processedLine, quality, userData (u64, +32)}.
//
// libjpgenc encodes on a thread of its own: each comparison waits for a stream (WaitForOutput blocking) before it reads
// it. No game data; skipped without the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

TEST_CASE("cellJpgEnc's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][jpgenc]")
{
    check_names("cellJpgEnc", {"cellJpgEncQueryAttr", "cellJpgEncOpen", "cellJpgEncOpenEx", "cellJpgEncClose",
                                  "cellJpgEncWaitForInput", "cellJpgEncEncodePicture", "cellJpgEncEncodePicture2",
                                  "cellJpgEncWaitForOutput", "cellJpgEncGetStreamInfo", "cellJpgEncReset"});
}

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kPic = kData + 0x200, kParam = kData + 0x240, kOutParam = kData + 0x280, kInfo = kData + 0x2C0,
              kNum = kData + 0x300, kEx = kData + 0x400, kExData = kData + 0x500, kName = kData + 0x800,
              kPixels = kData + 0x1000, kStreams = kData + 0x40000;
constexpr u32 kStreamSize = 0x10000; // each stream buffer (eight of them)
constexpr u32 kWork = 0x01000000, kWorkSize = 0x01000000, kEncoderMem = 0x100000;
constexpr u8 kFill = 0xEE;
enum : u32 { GRAY = 1, RGB = 2, YCBCR = 3, RGBA = 10, ARGB = 20 };
enum : u32 { S444 = 0, S422 = 1, S420 = 2, S411 = 3, FULL = 4 };
enum : u32 { QUALITY = 0, FAST = 5 };
static_assert(kPixels + 0x3F000 <= kStreams && kStreams + 8 * kStreamSize <= kData + 0x200000);

struct JpgFixture : TlsFixture {
    JpgFixture()
    {
        REQUIRE(hle->main_memory().reserve(kWork, kWorkSize));
        REQUIRE(mem.commit(kWork, kWorkSize));
    }
};

u32 stream_at(u32 i) { return kStreams + kStreamSize * i; }
u32 pixel_bytes(u32 space) { return space == GRAY ? 1 : space == RGB || space == YCBCR ? 3 : 4; }

// A picture of smooth gradients with detail (edges, a checker, noise), as bytes of `space` at kPixels, rows `pitch`
// apart: the same function of (x, y, channel) whatever the colour space, so each space's encoder has varied input.
void put_picture(u8* b, u32 w, u32 h, u32 space, u32 pitch, u32 seed)
{
    std::memset(b + kPixels, 0xA5, usize{pitch} * h + 64);
    const u32 n = pixel_bytes(space);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            for (u32 c = 0; c < n; c++) {
                const u32 k = c + seed;
                u32 v = (x * (3 + k) + y * (5 + 2 * k)) * 3;
                if (((x / 4) + (y / 4)) % 5 == k % 5) v += 60;
                if (x > w / 2 && y < h / 3) v = 255 - v;
                v += ((x * 2654435761u) ^ (y * 40503u) ^ (k * 977u)) >> 29;
                b[kPixels + usize{y} * pitch + usize{x} * n + c] = static_cast<u8>(v);
            }
}

// ---- reading a JPEG back

struct Segment {
    u8 marker = 0;
    std::vector<u8> data;
};

// The marker segments to EOI (or the end of the bytes): SOI and EOI as segments without data, each scan's
// entropy-coded data skipped (its restart markers counted).
struct Parsed {
    bool soi = false, eoi = false;
    usize length = 0; // through EOI
    u32 restarts = 0;
    std::vector<Segment> segments;
};

Parsed parse(const u8* p, usize n)
{
    Parsed out;
    if (n < 2 || p[0] != 0xFF || p[1] != 0xD8) return out;
    out.soi = true;
    usize at = 2;
    bool scan = false;
    while (at + 1 < n) {
        if (p[at] != 0xFF) {
            at++;
            continue;
        }
        const u8 m = p[at + 1];
        if (m == 0xFF || (scan && m == 0)) {
            at += m ? 1 : 2;
            continue;
        }
        if (m >= 0xD0 && m <= 0xD7) {
            out.restarts++;
            at += 2;
            continue;
        }
        if (m == 0xD9) {
            out.eoi = true;
            out.length = at + 2;
            break;
        }
        if (at + 4 > n) break;
        const usize len = usize{p[at + 2]} << 8 | p[at + 3];
        if (len < 2 || at + 2 + len > n) break;
        out.segments.push_back({m, std::vector<u8>(p + at + 4, p + at + 2 + len)});
        at += 2 + len;
        scan = scan || m == 0xDA;
    }
    return out;
}

std::string marker_name(u8 m)
{
    if (m >= 0xE0 && m <= 0xEF) return std::format("APP{}", m - 0xE0);
    switch (m) {
    case 0xC0: return "SOF0";
    case 0xC1: return "SOF1";
    case 0xC2: return "SOF2";
    case 0xC4: return "DHT";
    case 0xDA: return "SOS";
    case 0xDB: return "DQT";
    case 0xDD: return "DRI";
    case 0xFE: return "COM";
    default: return std::format("{:#04x}", m);
    }
}

struct Run : Half<JpgFixture> {
    using Half::Half;

    void write_config(u32 max_w, u32 max_h, u32 space, u32 sampling, bool spu = false, u32 ex_num = 0)
    {
        std::memset(b + kCfg, 0, 28);
        ps_write32(b, kCfg, max_w);
        ps_write32(b, kCfg + 4, max_h);
        ps_write32(b, kCfg + 8, space);
        ps_write32(b, kCfg + 12, sampling);
        ps_write8(b, kCfg + 16, spu ? 1 : 0);
        ps_write32(b, kCfg + 20, ex_num ? kEx : 0);
        ps_write32(b, kCfg + 24, ex_num);
    }

    u32 query(const std::string& what)
    {
        std::memset(b + kAttr, kFill, 16);
        const u64 r = Half::r("cellJpgEncQueryAttr", {kCfg, kAttr}, "QueryAttr " + what);
        o.line("QueryAttr " + what + ": the attribute", hex(b + kAttr, 16));
        return r == 0 ? ps_read32(b, kAttr) : 0;
    }

    std::string handle_text(u32 h) const
    {
        if (h >= kWork && h < kWork + kWorkSize) return std::format("the work memory +{:#x}", h - kWork);
        return std::format("{:#x}", h);
    }

    // CellJpgEncResource {memAddr, memSize, ppuThreadPriority, spuThreadPriority}.
    void write_resource(u32 mem, u32 size, s32 ppu = 1000, s32 spu = 200)
    {
        ps_write32(b, kRsrc, mem);
        ps_write32(b, kRsrc + 4, size);
        ps_write32(b, kRsrc + 8, static_cast<u32>(ppu));
        ps_write32(b, kRsrc + 12, static_cast<u32>(spu));
    }

    // An encoder in work memory `slot` (an odd offset into it: the handle is the next 128-byte boundary).
    u32 open(const std::string& what, u32 slot, u32 max_w, u32 max_h, u32 space, u32 sampling, bool spu = false)
    {
        write_config(max_w, max_h, space, sampling, spu);
        const u32 size = query(what);
        write_resource(kWork + slot * kEncoderMem + 0x10, std::min(size ? size : 0x100000u, kEncoderMem - 0x10));
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = Half::r("cellJpgEncOpen", {kCfg, kRsrc, kHandle}, "Open " + what);
        o.line("Open " + what + ": the handle", handle_text(ps_read32(b, kHandle)));
        return r == 0 ? ps_read32(b, kHandle) : 0;
    }

    struct Pic {
        u32 width = 0, height = 0, space = RGB, pitch = 0; // pitch 0: EncodePicture (the picture's own rows)
        u64 user = 0;
    };
    struct Param {
        u32 quality = 90, method = QUALITY, restart = 0, mode = 0, ex = 0;
        bool spu = false;
    };

    // EncodePicture (EncodePicture2 when the picture has a pitch) to stream buffer `slot`, or to the file kName when
    // `slot` is ~0u.
    u64 encode(const std::string& what, u32 handle, const Pic& p, const Param& e, u32 slot = 0, u32 limit = kStreamSize)
    {
        const bool two = p.pitch != 0;
        std::memset(b + kPic, 0, 32);
        ps_write32(b, kPic, p.width);
        ps_write32(b, kPic + 4, p.height);
        if (two) {
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
        const bool file = slot == ~0u;
        ps_write32(b, kOutParam, file ? 0 : 1);
        ps_write32(b, kOutParam + 4, file ? kName : 0);
        ps_write32(b, kOutParam + 8, file ? 0 : stream_at(slot));
        ps_write32(b, kOutParam + 12, file ? 0 : limit);
        if (!file) std::memset(b + stream_at(slot), kFill, kStreamSize);
        return Half::r(two ? "cellJpgEncEncodePicture2" : "cellJpgEncEncodePicture", {handle, kPic, kParam, kOutParam},
            (two ? "EncodePicture2 " : "EncodePicture ") + what);
    }

    // WaitForOutput, blocking or not: its answer and count.
    u64 wait_output(const std::string& what, u32 handle, bool block)
    {
        ps_write32(b, kNum, 0xEEEEEEEE);
        const u64 r = Half::r("cellJpgEncWaitForOutput", {handle, kNum, block ? 1u : 0u}, "WaitForOutput " + what);
        o.line("WaitForOutput " + what + ": the count", std::format("{:#x}", ps_read32(b, kNum)));
        return r;
    }

    // GetStreamInfo: its answer and every field, then the JPEG it describes.
    void stream_info(const std::string& what, u32 handle, const std::string& file = {})
    {
        std::memset(b + kInfo, kFill, 48);
        const u64 r = Half::r("cellJpgEncGetStreamInfo", {handle, kInfo}, "GetStreamInfo " + what);
        o.line("GetStreamInfo " + what + ": the information", hex(b + kInfo, 48));
        if (r != 0 || ps_read32(b, kInfo) != 0) return;
        const u32 size = ps_read32(b, kInfo + 20), addr = ps_read32(b, kInfo + 12);
        std::vector<u8> bytes;
        if (!file.empty()) {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            o.line("GetStreamInfo " + what + ": the file's size is streamSize",
                bytes.size() == size ? "yes" : std::format("no ({} bytes)", bytes.size()));
        } else if (addr >= kStreams && addr + u64{size} <= kStreams + 8 * kStreamSize) {
            bytes.assign(b + addr, b + addr + std::min(size + 64u, kStreamSize));
            bool untouched = bytes.size() >= u64{size} + 64;
            for (u32 i = 0; untouched && i < 64; i++) untouched = bytes[size + i] == kFill;
            o.line("stream " + what + ": the 64 bytes past it untouched", untouched ? "yes" : "no");
        }
        analyse(what, std::vector<u8>(bytes.begin(), bytes.begin() + std::min<usize>(bytes.size(), size)));
    }

    // A JPEG: its markers, headers and tables as lines (a difference names them), the whole stream exactly.
    void analyse(const std::string& what, const std::vector<u8>& bytes)
    {
        const Parsed p = parse(bytes.data(), bytes.size());
        o.line("stream " + what + ": a whole JPEG", !p.soi ? "no SOI" : p.eoi ? "yes" : "cut short");
        o.line("stream " + what + ": streamSize is the JPEG's length",
            p.eoi && p.length == bytes.size() ? "yes" : std::format("no (the JPEG {} bytes)", p.length));
        std::string order;
        u32 dqt = 0, dht = 0;
        for (const Segment& s : p.segments) {
            order += " " + marker_name(s.marker);
            if (s.marker == 0xDB) o.line(std::format("stream {}: DQT {}", what, dqt++), hex(s.data));
            else if (s.marker == 0xC4) o.line(std::format("stream {}: DHT {}", what, dht++), hex(s.data));
            else o.line(std::format("stream {}: {}", what, marker_name(s.marker)), hex(s.data));
        }
        o.line("stream " + what + ": the markers", std::format("SOI{} EOI, {} restart markers", order, p.restarts));
        o.bytes("stream " + what + ": the JPEG", bytes);
    }

    // One picture encoded to stream 0 and taken.
    void one(const std::string& what, u32 handle, const Pic& p, const Param& e)
    {
        if (encode(what, handle, p, e) != 0) return;
        wait_output(what, handle, true);
        stream_info(what, handle);
    }
};

const char* space_name(u32 s)
{
    switch (s) {
    case GRAY: return "gray";
    case RGB: return "RGB";
    case YCBCR: return "YCbCr";
    case RGBA: return "RGBA";
    case ARGB: return "ARGB";
    default: return "?";
    }
}

Observed half(const Setup& s, bool sony_half)
{
    namespace fs = std::filesystem;
    Run h(s, sony_half, "cellJpgEnc");
    u8* b = h.b;
    using Pic = Run::Pic;

    // QueryAttr: the memory for each configuration, and its checks.
    h.write_config(640, 480, YCBCR, S420);
    h.r("cellJpgEncQueryAttr", {kCfg, 0}, "QueryAttr(cfg, NULL)");
    h.r("cellJpgEncQueryAttr", {0, kAttr}, "QueryAttr(NULL, attr)");
    struct Query {
        u32 w, h, space, sampling;
        bool spu;
        u32 ex;
    };
    for (const Query& q : {Query{640, 480, YCBCR, S420, false, 0}, Query{640, 480, YCBCR, S444, false, 0},
             Query{640, 480, YCBCR, S422, false, 0}, Query{640, 480, YCBCR, S411, false, 0},
             Query{640, 480, GRAY, FULL, false, 0}, Query{640, 480, RGB, FULL, false, 0},
             Query{640, 480, YCBCR, S420, true, 0}, Query{1920, 1080, YCBCR, S422, true, 0},
             Query{1, 1, GRAY, FULL, false, 0}, Query{65535, 65535, YCBCR, S420, false, 0},
             Query{65536, 480, YCBCR, S420, false, 0}, Query{640, 65536, YCBCR, S420, false, 0},
             Query{0, 480, YCBCR, S420, false, 0}, Query{640, 0, YCBCR, S420, false, 0},
             Query{640, 480, 0, FULL, false, 0}, Query{640, 480, 4, FULL, false, 0},
             Query{640, 480, YCBCR, 5, false, 0}, Query{640, 480, YCBCR, FULL, false, 0},
             Query{640, 480, GRAY, S420, false, 0}, Query{640, 480, RGB, S444, false, 0},
             Query{640, 480, YCBCR, S420, false, 1}}) {
        h.write_config(q.w, q.h, q.space, q.sampling, q.spu, q.ex);
        h.query(std::format("{}x{} colour space {} sampling {}{}{}", q.w, q.h, q.space, q.sampling,
            q.spu ? " with SPUs" : "", q.ex ? " and an extended parameter" : ""));
    }

    // Open's and OpenEx's checks.
    h.write_config(64, 64, YCBCR, S420);
    const u32 size = h.query("for Open's checks");
    const u32 mem = std::min(size ? size : 0x100000u, kEncoderMem);
    const auto open_refused = [&](const std::string& what, u32 cfg, u32 rsrc, u32 handle, bool ex = false) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = h.r(ex ? "cellJpgEncOpenEx" : "cellJpgEncOpen", {cfg, rsrc, handle},
            (ex ? "OpenEx " : "Open ") + what);
        h.o.line((ex ? "OpenEx " : "Open ") + what + ": the handle word", h.handle_text(ps_read32(b, kHandle)));
        if (r == 0) h.r("cellJpgEncClose", {ps_read32(b, kHandle)}, "Close after Open " + what);
    };
    h.write_resource(kWork, mem > 0 ? mem - 1 : 0);
    open_refused("with a byte too little memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 0xC00);
    open_refused("with PPU priority 0xC00", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, -1);
    open_refused("with PPU priority -1", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0x100);
    open_refused("with SPU priority 0x100", kCfg, kRsrc, kHandle);
    h.write_resource(0, mem);
    open_refused("without memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, 0);
    open_refused("with no memory size", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem);
    open_refused("without a handle pointer", kCfg, kRsrc, 0);
    open_refused("without a resource", kCfg, 0, kHandle);
    open_refused("without a configuration", 0, kRsrc, kHandle);
    h.write_config(64, 64, GRAY, S420);
    open_refused("with gray at 4:2:0", kCfg, kRsrc, kHandle);
    h.write_config(64, 64, YCBCR, S420);
    // CellJpgEncResourceEx {memAddr, memSize, ppuThreadPriority, spurs, priority[8]}.
    std::memset(b + kRsrc, 0, 28);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, mem);
    ps_write32(b, kRsrc + 8, 1000);
    ps_write8(b, kRsrc + 16 + 3, 16);
    open_refused("with a priority of 16", kCfg, kRsrc, kHandle, true);
    open_refused("without a resource", kCfg, 0, kHandle, true);

    // An encoder for each encoded colour space and sampling, and the pictures each takes.
    struct Encoder {
        u32 space, sampling;
        const char* what;
    };
    const Encoder encoders[] = {{YCBCR, S420, "4:2:0"}, {YCBCR, S422, "4:2:2"}, {YCBCR, S444, "4:4:4"},
        {YCBCR, S411, "4:1:1"}, {GRAY, FULL, "gray"}, {RGB, FULL, "RGB"}};
    u32 slot = 1;
    for (const Encoder& enc : encoders) {
        const u32 e = h.open(std::string("an encoder of ") + enc.what, slot++, 64, 64, enc.space, enc.sampling);
        if (!e) continue;
        const std::string name = enc.what;
        // Every colour space of picture (the ones this encoder takes and the ones it refuses), 33x17.
        for (const u32 space : {GRAY, RGB, YCBCR, RGBA, ARGB}) {
            put_picture(b, 33, 17, space, 33 * pixel_bytes(space), space);
            h.one(std::format("{}: {} 33x17 at quality 85", name, space_name(space)), e, {33, 17, space},
                {85, QUALITY});
        }
        // Sizes on and off the MCU, both DCTs; qualities; restart intervals.
        const u32 in = enc.space == GRAY ? GRAY : RGB;
        for (const auto& [w, hh] : {std::pair{1u, 1u}, std::pair{8u, 8u}, std::pair{16u, 16u}, std::pair{7u, 5u},
                 std::pair{64u, 48u}, std::pair{50u, 61u}}) {
            put_picture(b, w, hh, in, w * pixel_bytes(in), w + hh);
            for (const u32 method : {QUALITY, FAST})
                h.one(std::format("{}: {} {}x{}, {} DCT", name, space_name(in), w, hh, method ? "fast" : "accurate"),
                    e, {w, hh, in}, {75, method});
        }
        put_picture(b, 40, 24, in, 40 * pixel_bytes(in), 3);
        for (const u32 q : {0u, 1u, 5u, 10u, 25u, 50u, 51u, 90u, 99u, 100u})
            h.one(std::format("{}: quality {}", name, q), e, {40, 24, in}, {q, FAST});
        for (const u32 restart : {1u, 2u, 5u, 1000u})
            h.one(std::format("{}: restart interval {}", name, restart), e, {40, 24, in}, {60, QUALITY, restart});
        // EncodePicture2: rows with a pitch longer than the picture's.
        put_picture(b, 20, 10, RGBA, 100, 9);
        h.one(std::format("{}: RGBA 20x10 in rows of 100 bytes", name), e, {20, 10, RGBA, 100, 0x1234}, {70, FAST});
        h.r("cellJpgEncClose", {e}, std::string("Close of the encoder of ") + enc.what);
    }

    // Extended parameters: marker segments written after the encoder's own header.
    const u32 e = h.open("an encoder for the rest", slot++, 64, 64, YCBCR, S420);
    if (!e) return std::move(h.o);
    put_picture(b, 16, 16, RGB, 48, 4);
    {
        for (u32 i = 0; i < 64; i++) ps_write8(b, kExData + i, static_cast<u8>('A' + i % 26));
        const u32 ex[3][3] = {{0xFFE1, kExData, 10}, {0xFFEC, kExData + 16, 1}, {0xFFFE, kExData + 32, 17}};
        for (u32 i = 0; i < 3; i++)
            for (u32 k = 0; k < 3; k++) ps_write32(b, kEx + 12 * i + 4 * k, ex[i][k]);
        h.one("with an APP1 segment", e, {16, 16, RGB}, {80, QUALITY, 0, 0, 1});
        h.one("with APP1, APP12 and COM segments", e, {16, 16, RGB}, {80, QUALITY, 0, 0, 3});
        ps_write32(b, kEx, 0xE1);
        h.one("with a marker of 0xE1", e, {16, 16, RGB}, {80, QUALITY, 0, 0, 1});
        ps_write32(b, kEx, 0x12345678);
        h.one("with a marker of 0x12345678", e, {16, 16, RGB}, {80, QUALITY, 0, 0, 1});
        ps_write32(b, kEx, 0xFFE1);
    }

    // The stream size limit mode: the highest quality whose stream fits (to the quality given), in a buffer.
    put_picture(b, 64, 48, RGB, 192, 11);
    for (const u32 limit : {0x1000u, 0x1800u, 0x3000u}) {
        const std::string what = std::format("64x48 in at most {:#x} bytes, up to quality 95", limit);
        if (h.encode(what, e, {64, 48, RGB}, {95, FAST, 0, 1}, 0, limit) != 0) continue;
        h.wait_output(what, e, true);
        h.stream_info(what, e);
    }

    // The queues: four streams wait, then the commands (four at most); Reset; the picture after it.
    h.wait_output("with nothing waiting", e, false);
    h.r("cellJpgEncGetStreamInfo", {e, kInfo}, "GetStreamInfo with nothing waiting");
    h.r("cellJpgEncWaitForInput", {e, 0}, "WaitForInput with nothing waiting");
    put_picture(b, 8, 8, RGB, 24, 2);
    const auto ready = [&](u32 n) {
        u32 got = 0;
        for (int tries = 0; tries < 400 && got < n; tries++) {
            ps_write32(b, kNum, 0);
            h.side.call("cellJpgEncWaitForOutput", {e, kNum, 0});
            got = ps_read32(b, kNum);
            if (got < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return got;
    };
    for (u32 i = 0; i < 4; i++) {
        Pic p{8, 8, RGB};
        p.user = 0x100 + i;
        h.encode(std::format("queued picture {}", i), e, p, {50, FAST}, i);
    }
    h.o.line("the four queued pictures encoded", std::format("{}", ready(4)));
    for (u32 i = 4; i < 9; i++) {
        h.r("cellJpgEncWaitForInput", {e, 0}, std::format("WaitForInput before command {}, not blocking", i));
        Pic p{8, 8, RGB};
        p.user = 0x100 + i;
        h.encode(std::format("command {} with four streams waiting", i), e, p, {50, FAST}, i == 8 ? 6 : i);
    }
    h.o.line("streams waiting after the commands", std::format("{}", ready(5)));
    h.r("cellJpgEncReset", {e}, "Reset with four streams and four commands waiting");
    h.o.line("streams waiting after Reset", std::format("{}", ready(8)));
    h.r("cellJpgEncReset", {e}, "Reset with eight streams waiting");
    for (u32 i = 0; i < 8; i++) h.stream_info(std::format("stream {} after Reset", i), e);
    h.one("the first picture after Reset", e, {8, 8, RGB}, {50, FAST});
    h.one("the second picture after Reset", e, {8, 8, RGB}, {50, FAST});
    h.r("cellJpgEncReset", {e + 4}, "Reset of no handle");
    h.r("cellJpgEncReset", {0}, "Reset(NULL)");

    // A buffer too small for the stream.
    put_picture(b, 64, 48, RGB, 192, 12);
    for (const u32 limit : {0x1000u, 0x1400u}) {
        const std::string what = std::format("to a buffer of {:#x} bytes", limit);
        if (h.encode(what, e, {64, 48, RGB}, {100, QUALITY}, 5, limit) != 0) continue;
        h.wait_output(what, e, true);
        std::memset(b + kInfo, kFill, 48);
        h.r("cellJpgEncGetStreamInfo", {e, kInfo}, "GetStreamInfo " + what);
        h.o.line("GetStreamInfo " + what + ": the information", hex(b + kInfo, 48));
        h.o.line("the buffer of " + what + ": its first 0x400 bytes", hex(b + stream_at(5), 0x400));
        h.o.line("the buffer of " + what + ": the 16 bytes past it", hex(b + stream_at(5) + limit, 16));
    }

    // To a file on /dev_hdd0, and one that cannot be made.
    const fs::path root = fs::temp_directory_path() /
        std::format("pspocket-jpgenc-compare-{}", h.side.sony_half() ? "theirs" : "ours");
    fs::remove_all(root);
    fs::create_directories(root / "hdd0" / "tmp");
    REQUIRE(h.f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    put_picture(b, 16, 16, RGB, 48, 13);
    std::memcpy(b + kName, "/dev_hdd0/tmp/shot.jpg", 23);
    if (h.encode("to a file", e, {16, 16, RGB}, {80, QUALITY}, ~0u) == 0) {
        h.wait_output("to a file", e, true);
        h.stream_info("to a file", e, (root / "hdd0" / "tmp" / "shot.jpg").string());
    }
    std::memcpy(b + kName, "/dev_nowhere/x.jpg", 19);
    if (h.encode("to a file that cannot be made", e, {16, 16, RGB}, {80, QUALITY}, ~0u) == 0) {
        h.wait_output("to a file that cannot be made", e, true);
        h.stream_info("to a file that cannot be made", e);
    }

    // EncodePicture's checks; what passes them is taken.
    {
        const Pic good{8, 8, RGB};
        const auto check = [&](const std::string& what, const Pic& p, const Run::Param& pr, u32 slot_ = 6,
                               u32 limit = kStreamSize) {
            if (h.encode(what, e, p, pr, slot_, limit) != 0) return;
            h.wait_output(what, e, true);
            h.stream_info(what, e);
        };
        h.encode("of no handle", e + 1, good, {});
        h.r("cellJpgEncEncodePicture", {e, 0, kParam, kOutParam}, "EncodePicture without a picture");
        h.r("cellJpgEncEncodePicture2", {e, 0, kParam, kOutParam}, "EncodePicture2 without a picture");
        check("of no width", {0, 8, RGB}, {});
        check("of no height", {8, 0, RGB}, {});
        check("past maxWidth", {65, 8, RGB}, {});
        check("past maxHeight", {8, 65, RGB}, {});
        for (const u32 space : {0u, 4u, 9u, 11u, 21u}) check(std::format("of colour space {}", space), {8, 8, space}, {});
        check("of gray to YCbCr", {8, 8, GRAY}, {});
        check("with a pitch shorter than a row", {8, 8, RGB, 23}, {});
        check("with a pitch of a row", {8, 8, RGB, 24}, {});
        check("at quality 101", good, {101});
        check("with DCT method 1", good, {75, 1});
        check("with DCT method 6", good, {75, 6});
        check("in compression mode 2", good, {75, FAST, 0, 2});
        check("with restart interval 0xFFFF", good, {75, FAST, 0xFFFF});
        check("with restart interval 0x10000", good, {75, FAST, 0x10000});
        check("on the SPUs, not opened for them", good, {75, FAST, 0, 0, 0, true});
        ps_write32(b, kEx + 8, 0);
        check("with a segment of no bytes", good, {75, FAST, 0, 0, 1});
        ps_write32(b, kEx + 8, 0xFFFE);
        check("with a segment of 0xFFFE bytes", good, {75, FAST, 0, 0, 1});
        ps_write32(b, kEx + 8, 10);
        ps_write32(b, kEx + 4, 0);
        check("with a segment of no data", good, {75, FAST, 0, 0, 1});
        ps_write32(b, kEx + 4, kExData);
        check("to a buffer of 0xFFF bytes", good, {}, 6, 0xFFF);
        check("to a buffer of 0x100D bytes with a segment of 10", good, {75, FAST, 0, 0, 1}, 6, 0x100D);
        check("to a buffer of 0x100E bytes with a segment of 10", good, {75, FAST, 0, 0, 1}, 6, 0x100E);
        check("in the size limit mode, to a file", good, {75, FAST, 0, 1}, ~0u);
        check("for the output checks", good, {});
        ps_write32(b, kOutParam, 2);
        h.r("cellJpgEncEncodePicture", {e, kPic, kParam, kOutParam}, "EncodePicture to location 2");
        ps_write32(b, kOutParam, 1);
        ps_write32(b, kOutParam + 8, 0);
        h.r("cellJpgEncEncodePicture", {e, kPic, kParam, kOutParam}, "EncodePicture to a buffer at no address");
        ps_write32(b, kOutParam, 0);
        ps_write32(b, kOutParam + 4, 0);
        h.r("cellJpgEncEncodePicture", {e, kPic, kParam, kOutParam}, "EncodePicture to a file without a name");
        h.r("cellJpgEncEncodePicture", {e, kPic, kParam, 0}, "EncodePicture without an output");
        h.wait_output("after the checks", e, false);
    }
    h.r("cellJpgEncClose", {e}, "Close");
    h.r("cellJpgEncClose", {e}, "Close again");
    h.r("cellJpgEncWaitForInput", {e, 0}, "WaitForInput after Close");

    // With the SPUs: an encoder opened for them, pictures encoded on them (the fast DCT only).
    const u32 spu = h.open("an SPU encoder", slot++, 64, 64, YCBCR, S420, true);
    if (spu) {
        put_picture(b, 33, 17, RGB, 99, 5);
        h.one("RGB 33x17 on the SPUs", spu, {33, 17, RGB}, {85, FAST, 0, 0, 0, true});
        h.one("RGB 33x17 on the SPUs, restart interval 2", spu, {33, 17, RGB}, {85, FAST, 2, 0, 0, true});
        h.one("RGB 33x17 with the SPUs' encoder, on the PPU", spu, {33, 17, RGB}, {85, FAST});
        h.one("RGB 33x17 on the SPUs, the accurate DCT", spu, {33, 17, RGB}, {85, QUALITY, 0, 0, 0, true});
        h.r("cellJpgEncClose", {spu}, "Close of the SPU encoder");
    }
    fs::remove_all(root);
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellJpgEnc against Sony's libjpgenc: sizes, handles, the queue, the streams, every JPEG's tables and picture",
    "[.][hle][lle][compare][jpgenc]")
{
    const Setup s = setup();
    run(s, "cellJpgEnc against Sony's libjpgenc", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("QueryAttr 640x480 colour space 3 sampling 2") == "0x0");
            CHECK(o.value("Open an encoder of 4:2:0: the handle") == "the work memory +0x100080");
            CHECK(o.value("stream 4:2:0: RGB 8x8, accurate DCT: a whole JPEG") == "yes");
        });
}
