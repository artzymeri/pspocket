// cellJpgEnc against Sony's libjpgenc (D-618; the library D-616, the suite D-597): the same configurations, resources
// and pictures go to pspocket's library (on its own encoder, D-615) and to Sony's module. pspocket's structures are
// inferred (D-616), so this comparison is first of all where the Mac finds out whether they are Sony's: a guess that
// is not shows as an answer (ARG where pspocket encodes), a frame header of another size or sampling, or a stream
// information laid out otherwise.
// Compared: QueryAttr's attributes; Open's, OpenEx's and Close's answers and the handle (as an offset into the work
// memory when it lies there); every answer of the result queue and every field of each stream's information but its
// size; each JPEG's frame header (size, components, their sampling and tables), quantisation tables (Sony's scaling of
// the quality), Huffman tables and scan header, whole or not, its length as streamSize; and its picture as pspocket's
// decoder reads it back (it decodes as Sony's libjpgdec does, D-200): within a few steps of the other half's at high
// qualities, and its PSNR against the picture within 10% at every quality. Notes, not compared: the markers in order
// (Sony may write other APPn or COM segments), JFIF's version and density, the stream's size, the quality the size
// limit mode chose. No game data; skipped without the firmware (compare_util.hpp).

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
#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_encoder.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
namespace jpeg = pspocket::hle::jpeg;

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
              kNum = kData + 0x300, kName = kData + 0x800, kPixels = kData + 0x1000, kStreams = kData + 0x40000;
constexpr u32 kStreamSize = 0x10000; // each stream buffer (eight of them)
constexpr u32 kWork = 0x01000000, kWorkSize = 0x01000000;
constexpr u8 kFill = 0xEE;
enum : u32 { GRAY = 1, RGB = 2, YCBCR = 3, RGBA = 10, ARGB = 20 };
enum : u32 { S444 = 0, S422 = 1, S420 = 2, S411 = 3, FULL = 4 };
static_assert(kPixels + 0x3F000 <= kStreams && kStreams + 8 * kStreamSize <= kData + 0x200000);

struct JpgFixture : TlsFixture {
    JpgFixture()
    {
        REQUIRE(hle->main_memory().reserve(kWork, kWorkSize));
        REQUIRE(mem.commit(kWork, kWorkSize));
    }
};

u32 stream_at(u32 i) { return kStreams + kStreamSize * i; }

// ---- the pictures (test_jpgenc.cpp's kind: smooth, with some detail), as R, G, B and as each colour space

struct Picture {
    u32 w = 0, h = 0;
    std::vector<u8> rgb;
};

Picture smooth(u32 w, u32 h, u32 seed)
{
    Picture p{w, h, std::vector<u8>(usize{w} * h * 3)};
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u8* c = p.rgb.data() + (usize{y} * w + x) * 3;
            const double fx = x + seed, fy = y;
            c[0] = static_cast<u8>(std::clamp(128 + 90 * std::sin(fx * 0.13 + fy * 0.05), 0.0, 255.0));
            c[1] = static_cast<u8>(std::clamp(110 + 70 * std::cos(fy * 0.11 - fx * 0.02), 0.0, 255.0));
            c[2] = static_cast<u8>(std::clamp(40 + 1.5 * (fx + fy) + 20 * std::sin(fx * fy * 0.004), 0.0, 255.0));
        }
    return p;
}

u32 pixel_bytes(u32 space) { return space == GRAY ? 1 : space == RGB || space == YCBCR ? 3 : 4; }

// The picture at kPixels in `space`, rows `pitch` bytes apart.
void put_picture(u8* b, const Picture& p, u32 space, u32 pitch)
{
    std::memset(b + kPixels, 0xA5, usize{pitch} * p.h);
    for (u32 y = 0; y < p.h; y++)
        for (u32 x = 0; x < p.w; x++) {
            const u8* c = p.rgb.data() + (usize{y} * p.w + x) * 3;
            u8* o = b + kPixels + usize{y} * pitch + usize{x} * pixel_bytes(space);
            switch (space) {
            case GRAY: o[0] = jpeg::rgb_to_y(c[0], c[1], c[2]); break;
            case RGB: std::memcpy(o, c, 3); break;
            case YCBCR: jpeg::rgb_to_ycbcr(c[0], c[1], c[2], o[0], o[1], o[2]); break;
            case RGBA:
                std::memcpy(o, c, 3);
                o[3] = static_cast<u8>(x * 9);
                break;
            default:
                o[0] = static_cast<u8>(y * 9);
                std::memcpy(o + 1, c, 3);
                break;
            }
        }
}

std::vector<u8> gray_of(const Picture& p)
{
    std::vector<u8> g(usize{p.w} * p.h);
    for (usize i = 0; i < g.size(); i++) g[i] = jpeg::rgb_to_y(p.rgb[3 * i], p.rgb[3 * i + 1], p.rgb[3 * i + 2]);
    return g;
}

double psnr(const std::vector<u8>& a, const std::vector<u8>& b)
{
    if (a.size() != b.size() || a.empty()) return 0;
    double se = 0;
    for (usize i = 0; i < a.size(); i++) se += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
    return se == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / (se / static_cast<double>(a.size())));
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

    void write_config(u32 max_w, u32 max_h, bool spu = false, u32 add = 0, u32 ex_num = 0)
    {
        // CellJpgEncConfig {maxWidth, maxHeight, enableSpu (b8), addMemSize, exParamList, exParamNum} (D-616).
        std::memset(b + kCfg, 0, 24);
        ps_write32(b, kCfg, max_w);
        ps_write32(b, kCfg + 4, max_h);
        ps_write8(b, kCfg + 8, spu ? 1 : 0);
        ps_write32(b, kCfg + 12, add);
        ps_write32(b, kCfg + 16, ex_num ? kName + 0x100 : 0);
        ps_write32(b, kCfg + 20, ex_num);
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

    u32 open(const std::string& what, u32 max_w, u32 max_h, bool spu)
    {
        write_config(max_w, max_h, spu);
        const u32 size = query(what);
        write_resource(kWork, std::min(size ? size : 0x100000u, kWorkSize));
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = Half::r("cellJpgEncOpen", {kCfg, kRsrc, kHandle}, "Open " + what);
        o.line("Open " + what + ": the handle", handle_text(ps_read32(b, kHandle)));
        return r == 0 ? ps_read32(b, kHandle) : 0;
    }

    struct Pic {
        u32 width = 0, height = 0, pitch = 0, space = RGB, sampling = FULL;
        u64 user = 0;
    };
    struct Param {
        u32 space = YCBCR, sampling = S420, method = 0, mode = 0, quality = 90;
        bool spu = false;
    };

    // EncodePicture (EncodePicture2 with `two`) to stream buffer `slot`, or to the file kName when `slot` is ~0u.
    u64 encode(const std::string& what, u32 handle, const Pic& p, const Param& e, u32 slot = 0, u32 limit = kStreamSize,
        bool two = false)
    {
        // CellJpgEncPicture {width, height, pitchWidth, colorSpace, pictureAddr, userData (u64, +24)}; Picture2 with
        // samplingFormat at +16 and pictureAddr at +20. CellJpgEncEncodeParam {enableSpu (b8), encodeColorSpace,
        // samplingFormat, dctMethod, compressionMode, compressionModeParam}. CellJpgEncOutputParam {location,
        // streamFileName, streamAddr, limitSize} (D-616).
        std::memset(b + kPic, 0, 32);
        ps_write32(b, kPic, p.width);
        ps_write32(b, kPic + 4, p.height);
        ps_write32(b, kPic + 8, p.pitch);
        ps_write32(b, kPic + 12, p.space);
        if (two) ps_write32(b, kPic + 16, p.sampling);
        ps_write32(b, kPic + (two ? 20 : 16), kPixels);
        ps_write64(b, kPic + 24, p.user);
        std::memset(b + kParam, 0, 24);
        ps_write8(b, kParam, e.spu ? 1 : 0);
        ps_write32(b, kParam + 4, e.space);
        ps_write32(b, kParam + 8, e.sampling);
        ps_write32(b, kParam + 12, e.method);
        ps_write32(b, kParam + 16, e.mode);
        ps_write32(b, kParam + 20, e.quality);
        const bool file = slot == ~0u;
        ps_write32(b, kOutParam, file ? 0 : 1);
        ps_write32(b, kOutParam + 4, file ? kName : 0);
        ps_write32(b, kOutParam + 8, file ? 0 : stream_at(slot));
        ps_write32(b, kOutParam + 12, limit);
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

    // GetStreamInfo: its answer and fields (the size checked against the JPEG it describes), then the JPEG, against
    // `want` (the picture as RGB, or gray) at `quality`.
    void stream_info(const std::string& what, u32 handle, const std::vector<u8>& want = {}, u32 quality = 0,
        const std::string& file = {})
    {
        std::memset(b + kInfo, kFill, 48);
        const u64 r = Half::r("cellJpgEncGetStreamInfo", {handle, kInfo}, "GetStreamInfo " + what);
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
        analyse(what, bytes, size, file.empty(), want, quality);
    }

    // A JPEG of `size` bytes (`bytes` may hold 64 more: what follows it in the buffer).
    void analyse(const std::string& what, const std::vector<u8>& bytes, u32 size, bool buffer,
        const std::vector<u8>& want, u32 quality)
    {
        const usize n = std::min<usize>(bytes.size(), size);
        const Parsed p = parse(bytes.data(), n);
        const std::string s = "stream " + what + ": ";
        o.line(s + "a whole JPEG", !p.soi ? "no SOI" : p.eoi ? "yes" : "cut short");
        o.line(s + "streamSize is the JPEG's length", p.eoi && p.length == size ? "yes" : std::format("no ({})", p.length));
        if (buffer && bytes.size() >= u64{size} + 64) {
            bool untouched = true;
            for (u32 i = 0; i < 64; i++) untouched = untouched && bytes[size + i] == kFill;
            o.line(s + "the 64 bytes past it untouched", untouched ? "yes" : "no");
        }
        std::string order, app0;
        std::vector<u8> tables, huffman;
        for (const Segment& g : p.segments) {
            order += " " + marker_name(g.marker);
            if (g.marker == 0xC0 || g.marker == 0xC1 || g.marker == 0xC2)
                o.line(s + "the frame (" + marker_name(g.marker) + ")", hex(g.data));
            if (g.marker == 0xDB) tables.insert(tables.end(), g.data.begin(), g.data.end());
            if (g.marker == 0xC4) huffman.insert(huffman.end(), g.data.begin(), g.data.end());
            if (g.marker == 0xDA) o.line(s + "the scan", hex(g.data));
            if (g.marker == 0xDD) o.line(s + "the restart interval", hex(g.data));
            if (g.marker == 0xE0) app0 = hex(g.data);
        }
        o.line(s + "the quantisation tables", hex(tables));
        o.line(s + "the Huffman tables", hex(huffman));
        o.note(std::format("stream {}: {} bytes, markers{} EOI, {} restart markers, APP0 {}", what, size, order,
            p.restarts, app0.empty() ? "none" : app0));
        jpeg::Decoder d;
        std::string why;
        const bool read = p.soi && d.read_header(bytes.data(), n, why) && d.decode(why);
        o.line(s + "pspocket's decoder reads it", read ? "yes" : "no: " + why);
        if (!read) return;
        const bool gray = d.header().components == 1;
        jpeg::Decoder::Output img;
        if (!d.output(1, false, gray ? jpeg::ColorSpace::Gray : jpeg::ColorSpace::RGB, img, why)) return;
        o.line(s + "decoded", std::format("{}x{}, {} components{}", img.width, img.height, img.components,
            d.warnings() ? std::format(", {} warnings", d.warnings()) : ""));
        // The picture: within a few steps of the other half's at high qualities (where the two encoders' rounding is
        // all that differs), and its PSNR against the picture within 10% at any.
        if (quality >= 85) o.bytes(s + "the picture as decoded", img.pixels, Rule::within(10));
        if (!want.empty()) o.numbers(s + "its PSNR against the picture (dB)", {psnr(img.pixels, want)}, 0.1);
    }
};

Observed half(const Setup& s, bool sony_half)
{
    namespace fs = std::filesystem;
    Run h(s, sony_half, "cellJpgEnc");
    u8* b = h.b;
    using Pic = Run::Pic;
    using Param = Run::Param;

    // QueryAttr: the memory for each configuration, and its checks.
    h.r("cellJpgEncQueryAttr", {kCfg, 0}, "QueryAttr(cfg, NULL)");
    h.r("cellJpgEncQueryAttr", {0, kAttr}, "QueryAttr(NULL, attr)");
    struct Query {
        u32 w, h;
        bool spu;
        u32 add, ex;
        const char* what;
    };
    for (const Query& q : {Query{640, 480, false, 0x100, 0, "640x480, 0x100 more"}, Query{640, 480, true, 0, 0, "640x480 with SPUs"},
             Query{64, 64, false, 0, 0, "64x64"}, Query{1920, 1080, false, 0, 0, "1920x1080"}, Query{1, 1, false, 0, 0, "1x1"},
             Query{4096, 4096, false, 0, 0, "4096x4096"}, Query{65535, 16, false, 0, 0, "65535 wide"},
             Query{65536, 16, false, 0, 0, "65536 wide"}, Query{0, 480, false, 0, 0, "no width"},
             Query{640, 0, false, 0, 0, "no height"}, Query{640, 480, false, 0x80000000u, 0, "addMemSize 2 GB"},
             Query{640, 480, false, 0, 1, "an extended parameter"}}) {
        h.write_config(q.w, q.h, q.spu, q.add, q.ex);
        std::memset(b + kName + 0x100, 0, 16); // CellJpgEncExParam {index 0, value -> 0}, for the one that has it
        ps_write32(b, kName + 0x104, kName + 0x108);
        h.query(q.what);
    }

    // Open's and OpenEx's checks, then the handle.
    h.write_config(128, 96);
    const u32 size = h.query("for Open's checks");
    const u32 mem = std::min(size ? size : 0x100000u, kWorkSize);
    // An Open that should be refused; one that is not is closed again (unless its handle is `keep`'s).
    const auto open_refused = [&](const std::string& what, u32 cfg, u32 rsrc, u32 handle, bool ex = false, u32 keep = 0) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = h.r(ex ? "cellJpgEncOpenEx" : "cellJpgEncOpen", {cfg, rsrc, handle}, (ex ? "OpenEx " : "Open ") + what);
        h.o.line((ex ? "OpenEx " : "Open ") + what + ": the handle word", h.handle_text(ps_read32(b, kHandle)));
        if (r == 0 && ps_read32(b, kHandle) != keep) h.r("cellJpgEncClose", {ps_read32(b, kHandle)}, "Close after Open " + what);
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
    ps_write8(b, kRsrc + 16 + 3, 15);
    open_refused("with eight priorities of at most 15", kCfg, kRsrc, kHandle, true);

    const u32 hd = h.open("128x96", 128, 96, false);
    if (!hd) return std::move(h.o);
    h.write_resource(kWork, mem);
    open_refused("of the memory already open", kCfg, kRsrc, kHandle, false, hd);

    // Pictures in every colour space and sampling, each taken from the queue and read back.
    const Picture pic = smooth(61, 37, 3);
    const auto take = [&](const std::string& what, const std::vector<u8>& want, u32 quality) {
        h.wait_output(what, hd, true);
        h.stream_info(what, hd, want, quality);
    };
    const auto one = [&](const std::string& what, const Pic& p, const Param& e, bool two = false) {
        if (h.encode(what, hd, p, e, 0, kStreamSize, two) == 0) take(what, e.space == GRAY ? gray_of(pic) : pic.rgb, e.quality);
    };
    for (const u32 space : {GRAY, RGB, YCBCR, RGBA, ARGB}) {
        const u32 pitch = pic.w * pixel_bytes(space) + 3;
        put_picture(b, pic, space, pitch);
        for (const u32 sampling : {S444, S422, S420, S411, FULL})
            one(std::format("colour space {} at sampling {}", space, sampling), {pic.w, pic.h, pitch, space},
                {YCBCR, sampling, 0, 0, 90});
        one(std::format("colour space {} as gray", space), {pic.w, pic.h, pitch, space}, {GRAY, S420, 0, 0, 90});
    }
    // Qualities, the fast method, SPUs asked for.
    put_picture(b, pic, RGB, pic.w * 3);
    for (const u32 quality : {1u, 10u, 50u, 75u, 95u, 100u})
        one(std::format("RGB at quality {}", quality), {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S420, 0, 0, quality});
    one("RGB by the fast method", {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S420, 5, 0, 90});
    one("RGB with enableSpu", {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S420, 0, 0, 90, true});
    // EncodePicture2: FULL, then a YCbCr picture's 4:2:0 planes (D-616's guess).
    one("RGB (Picture2, FULL)", {pic.w, pic.h, pic.w * 3, RGB, FULL}, {YCBCR, S420, 0, 0, 90}, true);
    {
        const u32 pitch = 64, cw = (pic.w + 1) / 2, ch = (pic.h + 1) / 2, cb = kPixels + pitch * pic.h,
                  cr = cb + (pitch / 2) * ch;
        std::memset(b + kPixels, 0x80, pitch * pic.h + pitch * ch);
        for (u32 y = 0; y < pic.h; y++)
            for (u32 x = 0; x < pic.w; x++) {
                const u8* c = pic.rgb.data() + (usize{y} * pic.w + x) * 3;
                u8 yy = 0, u = 0, v = 0;
                jpeg::rgb_to_ycbcr(c[0], c[1], c[2], yy, u, v);
                ps_write8(b, kPixels + y * pitch + x, yy);
                if (x % 2 == 0 && y % 2 == 0 && x / 2 < cw && y / 2 < ch) {
                    ps_write8(b, cb + (y / 2) * (pitch / 2) + x / 2, u);
                    ps_write8(b, cr + (y / 2) * (pitch / 2) + x / 2, v);
                }
            }
        one("YCbCr 4:2:0 planes (Picture2)", {pic.w, pic.h, pitch, YCBCR, S420}, {YCBCR, S420, 0, 0, 90}, true);
    }

    // The stream size limit mode: a limit between quality 30's stream and 90's (pspocket's), then one nothing fits.
    {
        jpeg::Planes planes;
        planes.width = pic.w;
        planes.height = pic.h;
        planes.components = 3;
        planes.y.resize(usize{pic.w} * pic.h);
        planes.cb.resize(planes.y.size());
        planes.cr.resize(planes.y.size());
        for (usize i = 0; i < planes.y.size(); i++)
            jpeg::rgb_to_ycbcr(pic.rgb[3 * i], pic.rgb[3 * i + 1], pic.rgb[3 * i + 2], planes.y[i], planes.cb[i], planes.cr[i]);
        jpeg::EncodeSettings at;
        at.quality = 30;
        const usize low = jpeg::encode(planes, at).size();
        at.quality = 90;
        const u32 limit = static_cast<u32>((low + jpeg::encode(planes, at).size()) / 2);
        put_picture(b, pic, RGB, pic.w * 3);
        if (h.encode("in the size limit mode", hd, {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S420, 0, 1, 90}, 1, limit) == 0) {
            h.wait_output("in the size limit mode", hd, true);
            std::memset(b + kInfo, kFill, 48);
            if (h.r("cellJpgEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo in the size limit mode") == 0) {
                const u32 got = ps_read32(b, kInfo + 20);
                h.o.line("in the size limit mode: the stream fits the limit", got <= limit ? "yes" : "no");
                h.o.line("in the size limit mode: the state", std::format("{:#x}", ps_read32(b, kInfo)));
                const Parsed p = parse(b + stream_at(1), std::min(got, kStreamSize));
                for (const Segment& g : p.segments)
                    if (g.marker == 0xDB && g.data.size() >= 65)
                        h.o.note(std::format("in the size limit mode: {} of {} bytes, the luma table's first entry {}",
                            got, limit, g.data[1]));
            }
        }
        if (h.encode("in the size limit mode to 300 bytes", hd, {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S420, 0, 1, 90}, 2,
                300) == 0) {
            h.wait_output("in the size limit mode to 300 bytes", hd, true);
            std::memset(b + kInfo, kFill, 48);
            h.r("cellJpgEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo in the size limit mode to 300 bytes");
            h.o.line("in the size limit mode to 300 bytes: the information", hex(b + kInfo, 48));
            h.o.line("in the size limit mode to 300 bytes: the byte past the limit", hex(b + stream_at(2) + 300, 1));
        }
    }

    // The queue: four waiting (each in its own buffer), then room and a fifth; oldest first; Reset; EMPTY.
    h.wait_output("with nothing waiting", hd, false);
    h.r("cellJpgEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo with nothing waiting");
    std::memset(b + kPixels, 0x22, 16 * 16 * 4);
    for (u32 i = 0; i < 4; i++) {
        h.r("cellJpgEncWaitForInput", {hd, 1}, std::format("WaitForInput before picture {}", i));
        Pic p{16, 16, 64, RGBA};
        p.user = 0x100 + i;
        h.encode(std::format("queued picture {}", i), hd, p, {}, i);
    }
    // Until `n` are encoded (Sony's may encode on threads of its own; a non-blocking answer before then would only say
    // how far it had got): how many were, after at most two seconds.
    const auto ready = [&](u32 n) {
        u32 got = 0;
        for (int tries = 0; tries < 400 && got < n; tries++) {
            ps_write32(b, kNum, 0);
            h.side.call("cellJpgEncWaitForOutput", {hd, kNum, 0});
            got = ps_read32(b, kNum);
            if (got < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return got;
    };
    h.o.line("the four queued pictures encoded", std::format("{}", ready(4)));
    h.r("cellJpgEncWaitForInput", {hd, 0}, "WaitForInput with four waiting, not blocking");
    if (h.encode("a fifth with four waiting", hd, {16, 16, 64, RGBA}, {}, 4) == 0)
        h.o.line("the fifth encoded too", std::format("{}", ready(5)));
    h.wait_output("with four waiting", hd, false);
    h.stream_info("the oldest of the four", hd);
    h.stream_info("the next of the four", hd);
    h.r("cellJpgEncWaitForInput", {hd, 0}, "WaitForInput with two taken");
    h.r("cellJpgEncReset", {hd}, "Reset");
    h.r("cellJpgEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo after Reset");
    h.wait_output("after Reset", hd, false);
    h.r("cellJpgEncReset", {hd + 4}, "Reset of no handle");
    h.r("cellJpgEncReset", {0}, "Reset(NULL)");

    // A buffer too small: what fits, the stream overflowing.
    if (h.encode("to a buffer of 20 bytes", hd, {16, 16, 64, RGBA}, {}, 5, 20) == 0) {
        h.wait_output("to a buffer of 20 bytes", hd, true);
        std::memset(b + kInfo, kFill, 48);
        h.r("cellJpgEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo to a buffer of 20 bytes");
        h.o.line("GetStreamInfo to a buffer of 20 bytes: the information", hex(b + kInfo, 48));
        h.o.line("the buffer of 20 bytes and the 16 after it", hex(b + stream_at(5), 36));
    }

    // To a file on /dev_hdd0 (lv2 makes no file in /dev_hdd0 itself), and one that cannot be made.
    const fs::path root = fs::temp_directory_path() /
        std::format("pspocket-jpgenc-compare-{}", h.side.sony_half() ? "theirs" : "ours");
    fs::remove_all(root);
    fs::create_directories(root / "hdd0" / "tmp");
    REQUIRE(h.f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
    put_picture(b, pic, RGB, pic.w * 3);
    std::memcpy(b + kName, "/dev_hdd0/tmp/shot.jpg", 23);
    if (h.encode("to a file", hd, {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S422, 0, 0, 90}, ~0u, 0) == 0) {
        h.wait_output("to a file", hd, true);
        h.stream_info("to a file", hd, pic.rgb, 90, (root / "hdd0" / "tmp" / "shot.jpg").string());
    }
    std::memcpy(b + kName, "/dev_nowhere/x.jpg", 19);
    if (h.encode("to a file that cannot be made", hd, {pic.w, pic.h, pic.w * 3, RGB}, {}, ~0u, 0) == 0) {
        h.wait_output("to a file that cannot be made", hd, true);
        std::memset(b + kInfo, kFill, 48);
        h.r("cellJpgEncGetStreamInfo", {hd, kInfo}, "GetStreamInfo to a file that cannot be made");
        h.o.line("GetStreamInfo to a file that cannot be made: the information", hex(b + kInfo, 48));
    }

    // EncodePicture's checks (test_jpgenc.cpp's).
    {
        std::memset(b + kPixels, 0x33, 128 * 96 * 4);
        const Pic good{16, 16, 64, RGBA};
        h.encode("of no handle", hd + 1, good, {});
        h.r("cellJpgEncEncodePicture", {hd, 0, kParam, kOutParam}, "EncodePicture without a picture");
        h.encode("of no width", hd, {0, 16, 64, RGBA}, {});
        h.encode("past maxWidth", hd, {129, 16, 520, RGBA}, {});
        h.encode("past maxHeight", hd, {16, 97, 64, RGBA}, {});
        h.encode("of colour space 4", hd, {16, 16, 64, 4}, {});
        h.encode("with a pitch shorter than a row", hd, {16, 16, 63, RGBA}, {});
        h.encode("to encode colour space 9", hd, good, {9});
        h.encode("at sampling format 5", hd, good, {YCBCR, 5});
        h.encode("by DCT method 1", hd, good, {YCBCR, S420, 1});
        h.encode("in compression mode 2", hd, good, {YCBCR, S420, 0, 2});
        h.encode("at quality 0", hd, good, {YCBCR, S420, 0, 0, 0});
        h.encode("at quality 101", hd, good, {YCBCR, S420, 0, 0, 101});
        h.encode("to a buffer of no size", hd, good, {}, 6, 0);
        ps_write32(b, kOutParam, 2); // the rest as for the buffer of no size
        h.r("cellJpgEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to location 2");
        ps_write32(b, kOutParam, 0);
        ps_write32(b, kOutParam + 4, 0);
        h.r("cellJpgEncEncodePicture", {hd, kPic, kParam, kOutParam}, "EncodePicture to a file without a name");
        h.encode("as planes of an RGB picture", hd, {16, 16, 64, RGB, S420}, {}, 6, kStreamSize, true);
        // What the refusals left queued (a refused picture must not be).
        for (int i = 0; i < 4; i++) {
            if (h.wait_output(std::format("after the checks, {}", i), hd, false) != 0) break;
            h.stream_info(std::format("after the checks, {}", i), hd);
        }
    }
    h.r("cellJpgEncClose", {hd}, "Close");
    h.r("cellJpgEncClose", {hd}, "Close again");
    h.r("cellJpgEncWaitForInput", {hd, 0}, "WaitForInput after Close");

    // With the SPUs: an encoder opened for them, a picture encoded on them.
    const u32 spu = h.open("128x96 with SPUs", 128, 96, true);
    if (spu) {
        put_picture(b, pic, RGB, pic.w * 3);
        if (h.encode("RGB on the SPUs", spu, {pic.w, pic.h, pic.w * 3, RGB}, {YCBCR, S420, 0, 0, 90, true}) == 0) {
            h.wait_output("RGB on the SPUs", spu, true);
            h.stream_info("RGB on the SPUs", spu, pic.rgb, 90);
        }
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
            // pspocket's own half (D-616): its memory measure, the handle at the work memory, the streams.
            CHECK(o.value("QueryAttr 64x64: the attribute") ==
                std::format("hex:{:08x}04eeeeee0027000000000000", 0x10000 + 64 * 0x40));
            CHECK(o.value("Open 128x96: the handle") == "the work memory +0x0");
            CHECK(o.value("Open of the memory already open") == "0x80611191");
            CHECK(o.value("QueryAttr 65536 wide") == "0x80611191");
            const std::string first = "stream colour space 2 at sampling 2: ";
            CHECK(o.value(first + "a whole JPEG") == "yes");
            CHECK(o.value(first + "streamSize is the JPEG's length") == "yes");
            CHECK(o.value(first + "the 64 bytes past it untouched") == "yes");
            // SOF0: 8 bits, 37 high, 61 wide, three components: Y 2x2 table 0, Cb and Cr 1x1 table 1.
            CHECK(o.value(first + "the frame (SOF0)") == "hex:080025003d03012200021101031101");
            CHECK(o.value(first + "pspocket's decoder reads it") == "yes");
            CHECK(o.value(first + "decoded") == "61x37, 3 components");
            CHECK(o.value("stream colour space 1 as gray: the frame (SOF0)") == "hex:080025003d01011100");
            CHECK(o.value("stream colour space 2 at sampling 3: the frame (SOF0)") == "hex:080025003d03014100021101031101");
            CHECK(o.value("stream YCbCr 4:2:0 planes (Picture2): pspocket's decoder reads it") == "yes");
            CHECK(o.value("in the size limit mode: the stream fits the limit") == "yes");
            CHECK(o.value("in the size limit mode to 300 bytes: the byte past the limit") == "hex:ee");
            CHECK(o.value("the four queued pictures encoded") == "4");
            CHECK(o.value("WaitForInput with four waiting, not blocking") == "0x80611193");
            CHECK(o.value("GetStreamInfo the next of the four: the information but streamSize")
                      .ends_with("0000000000000101eeeeeeeeeeeeeeee"));
            CHECK(o.value("stream to a file: pspocket's decoder reads it") == "yes");
            CHECK(o.value("GetStreamInfo to a file that cannot be made: the information").starts_with("hex:806111a4"));
            CHECK(o.value("EncodePicture at quality 101") == "0x80611191");
            CHECK(o.value("EncodePicture2 as planes of an RGB picture") == "0x80611191");
        });
}
