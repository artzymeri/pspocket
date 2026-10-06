// cellPngDec (D-199): pspocket's PNG decoder, alone and against Sony's libpngdec from the user's installed system
// software (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp). The images are built here (png_writer.hpp: every colour
// type and bit depth, interlaced or not, with and without ancillary chunks); the same calls go to both decoders and
// every answer, header, parameter, output row and chunk query is compared.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "png_writer.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace png = pspocket::test::png;

namespace {

struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    const ps_spu_miss_policy spu = ps_spu_get_miss_policy();
    InterpretMisses()
    {
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~InterpretMisses()
    {
        ps_ppu_set_miss_policy(ppu);
        ps_spu_set_miss_policy(spu);
    }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// Guest code: a malloc callback (size r3, arg r4) handing out 16-byte aligned blocks from a bump pointer at arg + 0,
// logging (size, address) at arg + 0x10 + 8 * n (n at arg + 4), and a free callback (ptr r3, arg r4) logging the
// pointer at arg + 0x800 + 4 * m (m at arg + 8), returning 0.
constexpr u32 kCode = kData, kMallocOpd = kData + 0x200, kFreeOpd = kData + 0x208, kHeapCtl = kData + 0x1000;
constexpr u32 kHeap = 0x01000000, kHeapSize = 0x00400000;
constexpr u32 kArgs = kData + 0x4000;
constexpr u32 kHandleOut = kArgs, kThreadIn = kArgs + 0x10, kThreadOut = kArgs + 0x30, kSrc = kArgs + 0x40,
              kStreamOut = kArgs + 0x60, kOpenInfo = kArgs + 0x68, kInfo = kArgs + 0x80, kInParam = kArgs + 0xA0,
              kOutParam = kArgs + 0xC0, kCtrl = kArgs + 0xF0, kDataOut = kArgs + 0x100, kQuery = kArgs + 0x200;
constexpr u32 kFile = kData + 0x10000; // the PNG, up to 0x30000 bytes
constexpr u32 kImage = kData + 0x40000; // the decoded image, up to 0x40000 bytes

void write_callbacks(u8* b)
{
    const std::vector<u32> code = {
        // malloc
        as::lwz(5, 0, 4), as::add(6, 5, 3), as::addi(6, 6, 15), as::rlwinm(6, 6, 0, 0, 27), as::stw(6, 0, 4),
        as::lwz(7, 4, 4), as::slwi(8, 7, 3), as::add(8, 8, 4), as::stw(3, 0x10, 8), as::stw(5, 0x14, 8),
        as::addi(7, 7, 1), as::stw(7, 4, 4), as::mr(3, 5), as::blr(),
        // free
        as::lwz(7, 8, 4), as::slwi(8, 7, 2), as::add(8, 8, 4), as::stw(3, 0x800, 8), as::addi(7, 7, 1), as::stw(7, 8, 4),
        as::li(3, 0), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kMallocOpd, kCode);
    ps_write32(b, kMallocOpd + 4, 0);
    ps_write32(b, kFreeOpd, kCode + 14 * 4);
    ps_write32(b, kFreeOpd + 4, 0);
    std::memset(b + kHeapCtl, 0, 0x1000);
    ps_write32(b, kHeapCtl, kHeap);
}

std::string hex(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

struct BigFixture : Fixture {
    BigFixture()
    {
        REQUIRE(hle->link_internal_functions({"cellPngDec"}));
        REQUIRE(hle->main_memory().reserve(kHeap, kHeapSize));
        REQUIRE(mem.commit(kHeap, kHeapSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cellPngDec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

struct Observed {
    std::vector<std::string> lines;
    std::vector<std::vector<u8>> images;
};

// One output setting: CellPngDecInParam's mode, colour space, bit depth, pack flag, alpha select, alpha.
struct Setting {
    u32 mode, space, depth, pack, alpha_select, alpha;
};

// The images and settings each test decodes.
std::vector<png::Image> test_images()
{
    std::vector<png::Image> v;
    const std::pair<u8, u8> kinds[] = {{0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1}, {3, 2}, {3, 4},
        {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16}};
    u32 seed = 0;
    for (const auto& [type, depth] : kinds) v.push_back(png::pattern(13, 7, type, depth, seed++));
    // Interlaced; with tRNS (palette, gray, RGB); with gAMA and text.
    png::Image il = png::pattern(19, 11, 6, 8, 40);
    il.interlaced = true;
    v.push_back(il);
    png::Image il2 = png::pattern(9, 9, 3, 4, 41);
    il2.interlaced = true;
    v.push_back(il2);
    png::Image pt = png::pattern(13, 7, 3, 8, 42);
    for (u32 i = 0; i < 40; i++) pt.trns.push_back(static_cast<u8>(i * 6));
    {
        std::vector<u8> hist;
        for (u32 i = 0; i < 256; i++) hist.insert(hist.end(), {static_cast<u8>(i >> 4), static_cast<u8>(i * 7)});
        pt.after_plte.push_back({"hIST", hist});
    }
    v.push_back(pt);
    png::Image gt = png::pattern(13, 7, 0, 8, 43);
    gt.trns = {0, static_cast<u8>(gt.samples[5] & 0xFF)};
    v.push_back(gt);
    png::Image rt = png::pattern(13, 7, 2, 8, 44);
    rt.trns = {0, static_cast<u8>(rt.samples[0]), 0, static_cast<u8>(rt.samples[1]), 0, static_cast<u8>(rt.samples[2])};
    {
        // A minimal ICC profile (a header and no tags): size, 'mntr' class, 'RGB ' on 'XYZ ', 'acsp', the D50 white.
        std::vector<u8> icc(132, 0);
        const auto put = [&](u32 at, u32 v) {
            for (u32 k = 0; k < 4; k++) icc[at + k] = static_cast<u8>(v >> (24 - 8 * k));
        };
        put(0, 132);
        put(8, 0x02100000);
        std::memcpy(&icc[12], "mntrRGB XYZ ", 12);
        std::memcpy(&icc[36], "acsp", 4);
        put(68, 0x0000F6D6);
        put(72, 0x00010000);
        put(76, 0x0000D32D);
        std::vector<u8> iccp = {'p', 's', 'p', 'o', 'c', 'k', 'e', 't', 0, 0};
        const std::vector<u8> z = png::zlib_stored(icc);
        iccp.insert(iccp.end(), z.begin(), z.end());
        rt.extra.push_back({"iCCP", iccp});
    }
    v.push_back(rt);
    png::Image ch = png::pattern(13, 7, 6, 8, 45);
    ch.extra.push_back({"gAMA", {0, 0, 0xB1, 0x8F}});
    ch.extra.push_back({"sRGB", {0}});
    ch.extra.push_back({"pHYs", {0, 0, 0x0B, 0x13, 0, 0, 0x0B, 0x13, 1}});
    ch.text.push_back({"Title", "pspocket"});
    ch.text.push_back({"Comment", "built in code"});
    ch.extra.push_back({"tEXt", {'B', 'e', 'f', 'o', 'r', 'e', 0, 'I', 'D', 'A', 'T'}});
    ch.extra.push_back({"bKGD", {0, 1, 0, 2, 0, 3}});
    ch.extra.push_back({"sBIT", {5, 6, 5, 8}});
    ch.extra.push_back({"oFFs", {0, 0, 0, 7, 0xFF, 0xFF, 0xFF, 0xF9, 0}});
    ch.extra.push_back({"cHRM", {0, 0, 0x7A, 0x26, 0, 0, 0x80, 0x84, 0, 0, 0xFA, 0, 0, 0, 0x80, 0xE8, 0, 0, 0x75, 0x30,
                                 0, 0, 0xEA, 0x60, 0, 0, 0x3A, 0x98, 0, 0, 0x17, 0x6F}});
    {
        const std::vector<u8> z = png::zlib_stored({'z', 'i', 'p', 'p', 'e', 'd'});
        std::vector<u8> ztxt = {'Z', 'k', 'e', 'y', 0, 0};
        ztxt.insert(ztxt.end(), z.begin(), z.end());
        ch.after.push_back({"zTXt", ztxt});
        ch.after.push_back({"iTXt", {'I', 'k', 'e', 'y', 0, 0, 0, 'e', 'n', 0, 'K', 'e', 'y', 0, 'i', 'n', 't', 'l'}});
    }
    ch.after.push_back({"tIME", {0x07, 0xEA, 10, 6, 21, 30, 59}});
    ch.extra.push_back({"sPLT", {'s', 'u', 'g', 0, 8, 10, 20, 30, 255, 0, 5, 40, 50, 60, 128, 0, 7}});
    ch.extra.push_back({"pCAL", {'c', 'a', 'l', 0, 0, 0, 0, 1, 0, 0, 0, 200, 0, 2, 'm', 0, '1', '.', '5', 0, '2', '.', '5'}});
    ch.extra.push_back({"sCAL", {1, '1', '.', '2', '5', 0, '2', '.', '5'}});
    ch.after.push_back({"psPk", {1, 2, 3, 4, 5}}); // a private, ancillary, safe-to-copy chunk
    v.push_back(ch);
    return v;
}

// /app_home/images.png for the file source: image 13 at 0, image 16 (interlaced) at 0x1000.
void mount_images(Fixture& f, const std::string& dir)
{
    namespace fs = std::filesystem;
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::vector<png::Image> images = test_images();
    std::vector<u8> file = png::encode(images[13]);
    file.resize(0x1000, 0);
    const std::vector<u8> second = png::encode(images[15]);
    file.insert(file.end(), second.begin(), second.end());
    std::ofstream(fs::path(dir) / "images.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    REQUIRE(f.kernel->vfs_mount("/app_home", dir, true));
}

const std::vector<Setting> kSettings = {
    {0, 10, 8, 1, 0, 0},    // RGBA, the stream's alpha
    {0, 20, 8, 1, 1, 0x80}, // ARGB, alpha 0x80
    {1, 10, 8, 1, 1, 0x40}, // RGBA bottom to top, alpha 0x40
    {0, 2, 8, 1, 0, 0},     // RGB
    {0, 1, 8, 1, 0, 0},     // grayscale
    {0, 9, 8, 1, 0, 0},     // grayscale with alpha
    {0, 4, 8, 1, 0, 0},     // palette
    {0, 10, 16, 1, 0, 0},   // RGBA, 16 bits
    {0, 10, 8, 0, 0, 0},    // RGBA, packed
};

bool g_trace = false; // the probe prints each line as it comes

Observed png_run(u8* b, const Call& call, usize max_images = 99)
{
    Observed o;
    const bool print = std::getenv("PSPOCKET_PROBE_TRACE") != nullptr;
    const auto note = [&](std::string s) {
        if (print) std::fprintf(stderr, "| %s\n", s.c_str());
        o.lines.push_back(std::move(s));
    };
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        if (print) std::fprintf(stderr, "| calling %s\n", name);
        const u32 v = call(name, nid, args);
        note(std::format("{} -> {:#x}", name, v));
        return v;
    };
    write_callbacks(b);
    std::memset(b + kArgs, 0, 0x400);
    // CellPngDecThreadInParam: spuThreadEnable, ppuThreadPriority, spuThreadPriority, malloc, its argument, free, its
    // argument.
    const u32 tin[7] = {0, 1000, 200, kMallocOpd, kHeapCtl, kFreeOpd, kHeapCtl};
    for (u32 i = 0; i < 7; i++) ps_write32(b, kThreadIn + 4 * i, tin[i]);
    if (r("cellPngDecCreate", 0x157D30C5, {kHandleOut, kThreadIn, kThreadOut}) != 0) return o;
    const u32 handle = ps_read32(b, kHandleOut);
    note(std::format("codec version {:#x}", ps_read32(b, kThreadOut)));
    // The bump allocator starts again here after each stream is closed (a stream's blocks are freed by then).
    const u32 heap_after_create = ps_read32(b, kHeapCtl);
    const u32 mallocs = ps_read32(b, kHeapCtl + 4);
    u32 frees = 0;
    note(std::format("create allocated {} block(s): {:#x} {:#x}", mallocs, ps_read32(b, kHeapCtl + 0x10),
        ps_read32(b, kHeapCtl + 0x18)));
    const std::vector<png::Image> images = test_images();
    for (usize n = 0; n < images.size() && n < max_images; n++) {
        const png::Image& im = images[n];
        const std::vector<u8> file = png::encode(im);
        std::memcpy(b + kFile, file.data(), file.size());
        for (const Setting& s : kSettings) {
            note(std::format("image {} (type {}, {} bits{}), setting {} {} {} {} {} {:#x}", n, im.color_type, im.bit_depth,
                im.interlaced ? ", interlaced" : "", s.mode, s.space, s.depth, s.pack, s.alpha_select, s.alpha));
            // CellPngDecSrc: buffer source.
            std::memset(b + kSrc, 0, 0x20);
            ps_write32(b, kSrc + 0, 1);
            ps_write32(b, kSrc + 20, kFile);
            ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
            if (r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) continue;
            const u32 stream = ps_read32(b, kStreamOut);
            note(std::format("open: initSpaceAllocated {:#x}", ps_read32(b, kOpenInfo)));
            std::memset(b + kInfo, 0xEE, 0x1C);
            if (r("cellPngDecReadHeader", 0x9CCDCC95, {handle, stream, kInfo}) == 0)
                note(std::format("info: {}", hex(b + kInfo, 0x1C)));
            const u32 in[7] = {0, s.mode, s.space, s.depth, s.pack, s.alpha_select, s.alpha};
            for (u32 i = 0; i < 7; i++) ps_write32(b, kInParam + 4 * i, in[i]);
            std::memset(b + kOutParam, 0xEE, 0x28);
            const u32 sp = r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam});
            note(std::format("out: {}", hex(b + kOutParam, 0x28)));
            if (sp == 0) {
                const u32 width_bytes = static_cast<u32>(ps_read64(b, kOutParam));
                const u32 height = ps_read32(b, kOutParam + 12);
                const u32 pitch = (width_bytes + 16 + 15) & ~15u; // a frame wider than the image: the padding shows
                ps_write64(b, kCtrl, pitch);
                std::memset(b + kImage, 0xCC, std::min<u32>(pitch * height + 64, 0x40000));
                std::memset(b + kDataOut, 0xEE, 0x10);
                if (pitch * height <= 0x40000 && r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut}) == 0) {
                    note(std::format("data out: {}", hex(b + kDataOut, 0x10)));
                    o.images.emplace_back(b + kImage, b + kImage + pitch * height + 64);
                }
            }
            r("cellPngDecClose", 0x5B3D1FF1, {handle, stream});
            // What this stream allocated and freed: the sizes in order, and how many frees.
            std::string sizes;
            for (u32 k = mallocs; k < ps_read32(b, kHeapCtl + 4) && k < 0xF0; k++)
                sizes += std::format("{}{:#x}", k == mallocs ? "" : " ", ps_read32(b, kHeapCtl + 0x10 + 8 * k));
            if (g_trace) note(std::format("allocated: {}; freed {}", sizes, ps_read32(b, kHeapCtl + 8) - frees)); // libpng's own
            ps_write32(b, kHeapCtl, heap_after_create);
            ps_write32(b, kHeapCtl + 4, mallocs);
            frees = ps_read32(b, kHeapCtl + 8);
            ps_write32(b, kHeapCtl + 8, frees = 0);
        }
    }
    // From a file (the test mounts /app_home): the whole file, and the second of two PNGs in one file by its offset.
    for (const auto& [offset, size] : {std::pair<u64, u32>{0, 0}, std::pair<u64, u32>{0x1000, 0}}) {
        note(std::format("file at offset {:#x}", offset));
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 0);
        ps_write32(b, kSrc + 4, kSrc + 0x100);
        std::memcpy(b + kSrc + 0x100, "/app_home/images.png", 21);
        ps_write64(b, kSrc + 8, offset);
        ps_write32(b, kSrc + 16, size);
        if (r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) continue;
        const u32 stream = ps_read32(b, kStreamOut);
        if (r("cellPngDecReadHeader", 0x9CCDCC95, {handle, stream, kInfo}) == 0) note(std::format("info: {}", hex(b + kInfo, 0x1C)));
        const u32 in[7] = {0, 0, 10, 8, 1, 1, 0x7F};
        for (u32 i = 0; i < 7; i++) ps_write32(b, kInParam + 4 * i, in[i]);
        if (r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam}) == 0) {
            const u32 pitch = static_cast<u32>(ps_read64(b, kOutParam)), height = ps_read32(b, kOutParam + 12);
            ps_write64(b, kCtrl, pitch);
            std::memset(b + kImage, 0xCC, pitch * height + 64);
            if (r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut}) == 0)
                o.images.emplace_back(b + kImage, b + kImage + pitch * height + 64);
        }
        r("cellPngDecClose", 0x5B3D1FF1, {handle, stream});
    }

    // Each chunk query on a stream of its own (a query that finds nothing puts the stream in an error state): the
    // image with every kind of chunk (20), the palette with tRNS (17), gray and RGB with tRNS (18, 19); before the
    // header (the wrong state), after the data, and with no place to put the answer.
    const auto str = [&](u32 p) { return p ? std::string(reinterpret_cast<const char*>(b + p)) : std::string("(null)"); };
    struct Query {
        const char* name;
        u32 nid;
    };
    const Query queries[] = {{"cellPngDecGetTextChunk", 0xB40CA175}, {"cellPngDecGetPLTE", 0xE163977F},
        {"cellPngDecGettRNS", 0xB96FB26E}, {"cellPngDecGetgAMA", 0xB153629C}, {"cellPngDecGetpHYs", 0x3D50016A},
        {"cellPngDecGetsBIT", 0x30CB334A}, {"cellPngDecGetbKGD", 0x7585A275}, {"cellPngDecGetoFFs", 0x27C921B5},
        {"cellPngDecGetcHRM", 0x7A062D26}, {"cellPngDecGetsRGB", 0xE4416E82}, {"cellPngDecGetiCCP", 0xF44B6C30},
        {"cellPngDecGethIST", 0xB905EBB7}, {"cellPngDecGetsPLT", 0xA5CDF57E}, {"cellPngDecGetpCAL", 0xB4FE75E1},
        {"cellPngDecGetsCAL", 0xC41E1198}, {"cellPngDecGettIME", 0x35A6846C}, {"cellPngDecGetUnknownChunks", 0x609EC7D5}};
    const auto open_decoded = [&](usize n, bool decode) -> u32 {
        const std::vector<u8> file = png::encode(images[n]);
        std::memcpy(b + kFile, file.data(), file.size());
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
        if (call("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) return 0;
        const u32 stream = ps_read32(b, kStreamOut);
        call("cellPngDecReadHeader", 0x9CCDCC95, {handle, stream, kInfo});
        if (decode) {
            const u32 in[7] = {0, 0, 10, 8, 1, 1, 0xFF};
            for (u32 i = 0; i < 7; i++) ps_write32(b, kInParam + 4 * i, in[i]);
            call("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam});
            ps_write64(b, kCtrl, 0x100);
            call("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut});
        }
        return stream;
    };
    const auto close = [&](u32 stream) {
        call("cellPngDecClose", 0x5B3D1FF1, {handle, stream});
        ps_write32(b, kHeapCtl, heap_after_create);
        ps_write32(b, kHeapCtl + 4, mallocs);
        ps_write32(b, kHeapCtl + 8, 0);
    };
    for (const usize n : {usize{20}, usize{17}, usize{18}, usize{19}}) {
        if (n >= images.size() || n >= max_images) continue;
        for (const Query& q : queries) {
            note(std::format("image {}: {}", n, q.name));
            const bool two = std::string_view(q.name) == "cellPngDecGetTextChunk" ||
                std::string_view(q.name) == "cellPngDecGetUnknownChunks";
            // Before the data: the wrong state.
            if (const u32 stream = open_decoded(n, false)) {
                r(q.name, q.nid, two ? std::initializer_list<u64>{handle, stream, kQuery, kQuery + 4}
                                     : std::initializer_list<u64>{handle, stream, kQuery});
                close(stream);
            }
            // With nowhere to put the answer.
            if (const u32 stream = open_decoded(n, true)) {
                r(q.name, q.nid, two ? std::initializer_list<u64>{handle, stream, 0, kQuery + 4}
                                     : std::initializer_list<u64>{handle, stream, 0});
                r("cellPngDecGetTextChunk", 0xB40CA175, {handle, stream, kQuery, kQuery + 4}); // after a failure
                close(stream);
            }
            const u32 stream = open_decoded(n, true);
            if (!stream) continue;
            std::memset(b + kQuery, 0xEE, 0x80);
            const u32 res = r(q.name, q.nid, two ? std::initializer_list<u64>{handle, stream, kQuery, kQuery + 4}
                                                 : std::initializer_list<u64>{handle, stream, kQuery});
            if (res == 0) {
                const std::string_view name = q.name;
                if (name == "cellPngDecGetTextChunk") {
                    const u32 num = ps_read32(b, kQuery), texts = ps_read32(b, kQuery + 4);
                    note(std::format("  {} texts", num));
                    for (u32 k = 0; k < num && k < 8 && texts; k++) {
                        const u32 at = texts + 24 * k;
                        note(std::format("  {} \"{}\" \"{}\" {} \"{}\" \"{}\"", ps_read32(b, at), str(ps_read32(b, at + 4)),
                            str(ps_read32(b, at + 8)), ps_read32(b, at + 12), str(ps_read32(b, at + 16)),
                            str(ps_read32(b, at + 20))));
                    }
                } else if (name == "cellPngDecGetUnknownChunks") {
                    const u32 chunks = ps_read32(b, kQuery), num = ps_read32(b, kQuery + 4);
                    note(std::format("  {} chunks", num));
                    for (u32 k = 0; k < num && k < 4 && chunks; k++) {
                        const u32 at = chunks + 20 * k, data = ps_read32(b, at + 8), length = ps_read32(b, at + 12);
                        note(std::format("  {} {} {} {}", hex(b + at, 5), data ? hex(b + data, std::min(length, 16u)) : "",
                            length, ps_read32(b, at + 16)));
                    }
                } else if (name == "cellPngDecGetPLTE") {
                    const u32 num = ps_read32(b, kQuery), entries = ps_read32(b, kQuery + 4);
                    note(std::format("  {} entries: {}", num, entries ? hex(b + entries, std::min(num, 8u) * 3) : ""));
                } else if (name == "cellPngDecGettRNS") {
                    const u32 alphas = ps_read32(b, kQuery), num = ps_read32(b, kQuery + 4);
                    note(std::format("  {} alphas: {}; {}", num, alphas ? hex(b + alphas, std::min(num, 8u)) : "",
                        hex(b + kQuery + 8, 8)));
                } else if (name == "cellPngDecGetiCCP") {
                    const u32 length = ps_read32(b, kQuery + 8), profile = ps_read32(b, kQuery + 4);
                    note(std::format("  \"{}\" {} bytes: {}", str(ps_read32(b, kQuery)), length,
                        profile ? hex(b + profile, std::min(length, 48u)) : ""));
                } else if (name == "cellPngDecGethIST") {
                    const u32 entries = ps_read32(b, kQuery), num = ps_read32(b, kQuery + 4);
                    note(std::format("  {} entries: {}", num, entries ? hex(b + entries, std::min(num, 8u) * 2) : ""));
                } else if (name == "cellPngDecGetsPLT") {
                    const u32 entries = ps_read32(b, kQuery), num = ps_read32(b, kQuery + 4);
                    note(std::format("  {} palettes", num));
                    for (u32 k = 0; k < num && k < 2 && entries; k++) {
                        const u32 at = entries + 16 * k, list = ps_read32(b, at + 8), count = ps_read32(b, at + 12);
                        note(std::format("  \"{}\" depth {} {} entries: {}", str(ps_read32(b, at)), b[at + 4], count,
                            list ? hex(b + list, std::min(count, 4u) * 10) : ""));
                    }
                } else if (name == "cellPngDecGetpCAL") {
                    const u32 n2 = ps_read32(b, kQuery + 16), params = ps_read32(b, kQuery + 24);
                    std::string ps;
                    for (u32 k = 0; k < n2 && k < 4 && params; k++) ps += " \"" + str(ps_read32(b, params + 4 * k)) + "\"";
                    note(std::format("  \"{}\" {} {} {} {} \"{}\"{}", str(ps_read32(b, kQuery)), static_cast<s32>(ps_read32(b, kQuery + 4)),
                        static_cast<s32>(ps_read32(b, kQuery + 8)), ps_read32(b, kQuery + 12), n2, str(ps_read32(b, kQuery + 20)), ps));
                } else {
                    note(std::format("  {}", hex(b + kQuery, 0x40)));
                }
            }
            r("cellPngDecGetTextChunk", 0xB40CA175, {handle, stream, kQuery, kQuery + 4}); // after it
            close(stream);
        }
    }

    // Calls out of order, and on things that are not handles or streams.
    {
        const std::vector<u8> file = png::encode(images[0]);
        std::memcpy(b + kFile, file.data(), file.size());
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
        note("out of order");
        const u32 in[7] = {0, 0, 1, 8, 1, 0, 0};
        for (u32 i = 0; i < 7; i++) ps_write32(b, kInParam + 4 * i, in[i]);
        ps_write64(b, kCtrl, 0x40);
        if (r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) == 0) {
            const u32 stream = ps_read32(b, kStreamOut);
            r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}); // a second stream
            r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam}); // before the header
            r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut});
            r("cellPngDecReadHeader", 0x9CCDCC95, {handle, stream, kInfo});
            r("cellPngDecReadHeader", 0x9CCDCC95, {handle, stream, kInfo}); // twice
            r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut}); // before the parameters
            ps_write32(b, kInParam + 8, 4); // palette from gray: refused
            r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam});
            ps_write32(b, kInParam + 8, 1);
            r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam});
            r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam}); // twice
            ps_write64(b, kCtrl, 4);
            std::memset(b + kDataOut, 0xEE, 0x10);
            r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut}); // lines too short
            note(std::format("data out: {}", hex(b + kDataOut, 0x10)));
            ps_write64(b, kCtrl, 0x40);
            r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut});
            r("cellPngDecDecodeData", 0x2310F155, {handle, stream, kImage, kCtrl, kDataOut}); // twice
            r("cellPngDecSetParameter", 0xE97C9BD4, {handle, stream, kInParam, kOutParam}); // after the data
            r("cellPngDecReadHeader", 0x9CCDCC95, {handle, stream, kInfo});
            r("cellPngDecClose", 0x5B3D1FF1, {handle, stream + 4});  // not a stream
            r("cellPngDecClose", 0x5B3D1FF1, {handle + 4, stream});  // not a handle
            r("cellPngDecClose", 0x5B3D1FF1, {handle, stream});
            r("cellPngDecClose", 0x5B3D1FF1, {handle, stream});      // closed
        }
        // Not a PNG; too short; a file that is not there.
        b[kFile + 1] = 'Q';
        if (r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) == 0) {
            r("cellPngDecReadHeader", 0x9CCDCC95, {handle, ps_read32(b, kStreamOut), kInfo});
            r("cellPngDecReadHeader", 0x9CCDCC95, {handle, ps_read32(b, kStreamOut), kInfo});
            r("cellPngDecClose", 0x5B3D1FF1, {handle, ps_read32(b, kStreamOut)});
        }
        b[kFile + 1] = 'P';
        ps_write32(b, kSrc + 24, 20);
        ps_write32(b, kStreamOut, 0);
        if (r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) == 0) {
            r("cellPngDecReadHeader", 0x9CCDCC95, {handle, ps_read32(b, kStreamOut), kInfo});
            r("cellPngDecClose", 0x5B3D1FF1, {handle, ps_read32(b, kStreamOut)});
        }
        r("cellPngDecOpen", 0xD2BC5BFD, {handle, 0, kSrc, kOpenInfo});
        r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, 0, kOpenInfo});
        // A stream left open: the handle cannot go.
        ps_write32(b, kSrc + 24, 0x1000);
        if (r("cellPngDecOpen", 0xD2BC5BFD, {handle, kStreamOut, kSrc, kOpenInfo}) == 0) {
            r("cellPngDecDestroy", 0x820DAE1A, {handle});
            r("cellPngDecClose", 0x5B3D1FF1, {handle, ps_read32(b, kStreamOut)});
        }
    }
    // Sources Open refuses, each on a handle of its own, then a good one on the same handle (some leave the handle
    // busy).
    for (int c = 0; c < 8; c++) {
        note(std::format("source case {}", c));
        if (r("cellPngDecCreate", 0x157D30C5, {kHandleOut, kThreadIn, kThreadOut}) != 0) break;
        const u32 h2 = ps_read32(b, kHandleOut);
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, 0x1000);
        switch (c) {
        case 0: ps_write32(b, kSrc + 0, 7); break;  // neither file nor buffer
        case 1: ps_write32(b, kSrc + 20, 0); break; // no buffer
        case 2: ps_write32(b, kSrc + 24, 0); break; // an empty buffer
        case 3:                                    // a file that is not there
            ps_write32(b, kSrc + 0, 0);
            ps_write32(b, kSrc + 4, kSrc + 0x100);
            std::memcpy(b + kSrc + 0x100, "/app_home/none.png", 19);
            break;
        default: ps_write32(b, kOpenInfo, 0); break; // a good one, or with an argument missing
        }
        const u32 open_args[3] = {c == 5 ? 0 : kStreamOut, c == 6 ? 0 : kSrc, c == 7 ? 0 : kOpenInfo};
        if (r("cellPngDecOpen", 0xD2BC5BFD, {h2, open_args[0], open_args[1], open_args[2]}) == 0) {
            r("cellPngDecReadHeader", 0x9CCDCC95, {h2, ps_read32(b, kStreamOut), kInfo});
            r("cellPngDecClose", 0x5B3D1FF1, {h2, ps_read32(b, kStreamOut)});
        }
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, 0x1000);
        if (r("cellPngDecOpen", 0xD2BC5BFD, {h2, kStreamOut, kSrc, kOpenInfo}) == 0)
            r("cellPngDecClose", 0x5B3D1FF1, {h2, ps_read32(b, kStreamOut)});
        r("cellPngDecDestroy", 0x820DAE1A, {h2});
    }
    r("cellPngDecDestroy", 0x820DAE1A, {handle + 4});
    r("cellPngDecDestroy", 0x820DAE1A, {handle});
    r("cellPngDecDestroy", 0x820DAE1A, {handle}); // destroyed
    ps_write32(b, kThreadIn + 12, 0);
    r("cellPngDecCreate", 0x157D30C5, {kHandleOut, kThreadIn, kThreadOut}); // no malloc
    ps_write32(b, kThreadIn + 12, kMallocOpd);
    ps_write32(b, kThreadIn + 0, 1);
    if (r("cellPngDecCreate", 0x157D30C5, {kHandleOut, kThreadIn, kThreadOut}) == 0) // with SPU threads
        r("cellPngDecDestroy", 0x820DAE1A, {ps_read32(b, kHandleOut)});
    ps_write32(b, kThreadIn + 0, 0);
    r("cellPngDecCreate", 0x157D30C5, {0, kThreadIn, kThreadOut});
    if (g_trace)
        note(std::format("after the streams: malloc calls {}, free calls {}", ps_read32(b, kHeapCtl + 4) - mallocs,
            ps_read32(b, kHeapCtl + 8)));
    return o;
}

} // namespace

TEST_CASE("cellPngDec: Sony's libpngdec over images built here (probe)", "[hle][pngdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    if (!flash) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellPngDec");
    mount_images(f, (std::filesystem::temp_directory_path() / "pspocket-test-pngdec").string());
    g_trace = true;
    const Observed o = png_run(f.base(),
        [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    if (const char* dump = std::getenv("PSPOCKET_PROBE_DUMP")) {
        if (FILE* fp = std::fopen(dump, "w")) {
            for (const std::string& l : o.lines) std::fprintf(fp, "%s\n", l.c_str());
            std::fclose(fp);
        }
    }
    WARN(std::format("{} lines, {} images", o.lines.size(), o.images.size()));
}

TEST_CASE("cellPngDec: the same as Sony's libpngdec (the user's firmware): answers, headers, parameters, images, chunks",
    "[hle][pngdec][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    g_trace = false;
    Observed ours, theirs;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        mount_images(f, (std::filesystem::temp_directory_path() / "pspocket-test-pngdec").string());
        ours = png_run(f.base(), hle_calls(f, t));
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellPngDec");
        mount_images(f, (std::filesystem::temp_directory_path() / "pspocket-test-pngdec").string());
        theirs = png_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    usize differ = 0;
    std::string report;
    for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++) {
        const std::string x = i < ours.lines.size() ? ours.lines[i] : "(none)";
        const std::string y = i < theirs.lines.size() ? theirs.lines[i] : "(none)";
        if (x != y && ++differ <= 40) report += std::format("line {}:\n  pspocket: {}\n  Sony:     {}\n", i, x, y);
    }
    REQUIRE(ours.images.size() == theirs.images.size());
    usize bytes = 0;
    for (usize i = 0; i < ours.images.size(); i++) {
        bytes += theirs.images[i].size();
        if (ours.images[i] == theirs.images[i]) continue;
        usize first = 0;
        while (first < std::min(ours.images[i].size(), theirs.images[i].size()) && ours.images[i][first] == theirs.images[i][first])
            first++;
        if (++differ <= 70)
            report += std::format("image {} differs from byte {} ({:02x} against Sony's {:02x})\n", i, first,
                first < ours.images[i].size() ? ours.images[i][first] : 0, first < theirs.images[i].size() ? theirs.images[i][first] : 0);
    }
    INFO(report);
    INFO(std::format("{} differences in {} lines and {} images", differ, theirs.lines.size(), theirs.images.size()));
    CHECK(differ == 0);
    WARN(std::format("compared with Sony's libpngdec: {} lines, {} images ({} bytes)", theirs.lines.size(),
        theirs.images.size(), bytes));
}

// Sony's RGB and gray outputs of the same images, for working out its colour to gray arithmetic (PSPOCKET_PROBE_DUMP).
TEST_CASE("cellPngDec: Sony's colour to gray (probe)", "[hle][pngdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    const char* dump = std::getenv("PSPOCKET_PROBE_DUMP");
    if (!flash || !dump) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellPngDec");
    mount_images(f, (std::filesystem::temp_directory_path() / "pspocket-test-pngdec").string());
    g_trace = false;
    const Observed o = png_run(f.base(),
        [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    FILE* fp = std::fopen(dump, "w");
    REQUIRE(fp);
    usize img = 0;
    for (const std::string& l : o.lines) {
        if (l.starts_with("data out:")) {
            std::fprintf(fp, "IMAGE %zu ", img);
            for (const u8 c : o.images[img]) std::fprintf(fp, "%02x", c);
            std::fprintf(fp, "\n");
            img++;
        } else {
            std::fprintf(fp, "%s\n", l.c_str());
        }
    }
    std::fclose(fp);
}

TEST_CASE("cellPngDec: PNGs built here decoded as Sony's libpngdec decodes them (D-199)", "[hle][pngdec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    mount_images(f, (std::filesystem::temp_directory_path() / "pspocket-test-pngdec-hle").string());
    g_trace = false;
    const Observed o = png_run(f.base(), hle_calls(f, t));
    std::string all;
    for (const std::string& l : o.lines) all += l + "\n";
    INFO(all);
    const auto has = [&](std::string_view text) { return all.find(text) != std::string::npos; };
    // Sony's answers (test above, run 106).
    CHECK(o.lines[1] == "codec version 0x420000");
    CHECK(has("open: initSpaceAllocated 0x4604"));
    CHECK(has("image 13 (type 6, 8 bits), setting 0 10 8 1 0 0x0\ncellPngDecOpen -> 0x0"));
    CHECK(has("  5 texts\n  0 \"Before\" \"IDAT\" 4 \"(null)\" \"(null)\""));
    CHECK(has("  2 \"Ikey\" \"intl\" 4 \"en\" \"Key\""));
    // Image 13 (RGBA, 8 bits) as RGBA with its own alpha: the samples, rows 16-byte aligned past 16 bytes of padding.
    const std::vector<png::Image> images = test_images();
    usize index = 0;
    for (usize n = 0; n < 13; n++)
        for (const Setting& setting : kSettings) {
            const bool alpha_ok = setting.alpha_select == 1 || images[n].color_type == 4 || images[n].color_type == 6;
            const bool depth_ok = setting.depth == 8 || images[n].bit_depth == 16;
            const bool space_ok = setting.space != 4 || images[n].color_type == 3;
            const bool stream_alpha_space = setting.space == 9 || setting.space == 10 || setting.space == 20;
            if ((!stream_alpha_space || alpha_ok) && depth_ok && space_ok) index++;
        }
    REQUIRE(index < o.images.size());
    const std::vector<u8>& rgba = o.images[index]; // image 13, setting 0
    const png::Image& im = images[13];
    for (u32 y = 0; y < 7; y++)
        for (u32 x = 0; x < 13; x++)
            for (u32 c = 0; c < 4; c++)
                if (rgba[y * 0x50 + 4 * x + c] != im.samples[(y * 13 + x) * 4 + c]) FAIL("pixel " << x << "," << y);
    CHECK(rgba[0x34] == 0xCC); // the padding untouched
}
