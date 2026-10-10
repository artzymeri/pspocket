// cellJpgDec (D-200): pspocket's JPEG decoder, alone and against Sony's libjpgdec (IJG libjpeg 6b) from the user's
// installed system software (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp). The JPEGs are built here
// (jpeg_writer.hpp: gray and colour, every chroma sampling, restart markers, JFIF, Adobe or no header); the same calls
// go to both decoders and every answer, header, parameter set and output row is compared. The Ext functions (D-617)
// as test_pngdec.cpp tests cellPngDec's: whole, in parts, from a stream.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "imgdec_ext_util.hpp"
#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_writer.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace jpeg = pspocket::test::jpeg;

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

// Guest malloc (a bump allocator logging sizes) and free callbacks, as in test_pngdec.cpp.
constexpr u32 kCode = kData, kMallocOpd = kData + 0x200, kFreeOpd = kData + 0x208, kHeapCtl = kData + 0x1000;
constexpr u32 kHeap = 0x01000000, kHeapSize = 0x00800000;
constexpr u32 kArgs = kData + 0x4000;
constexpr u32 kHandleOut = kArgs, kThreadIn = kArgs + 0x10, kThreadOut = kArgs + 0x30, kSrc = kArgs + 0x40,
              kStreamOut = kArgs + 0x60, kOpenInfo = kArgs + 0x68, kInfo = kArgs + 0x80, kInParam = kArgs + 0xA0,
              kOutParam = kArgs + 0xC0, kCtrl = kArgs + 0xF0, kDataOut = kArgs + 0x100;
constexpr u32 kFile = kData + 0x10000; // the JPEG, up to 0x30000 bytes
constexpr u32 kImage = kData + 0x40000; // the decoded image, up to 0x80000 bytes
constexpr u32 kImageMax = 0x80000;

void write_callbacks(u8* b)
{
    const std::vector<u32> code = {
        as::lwz(5, 0, 4), as::add(6, 5, 3), as::addi(6, 6, 15), as::rlwinm(6, 6, 0, 0, 27), as::stw(6, 0, 4),
        as::lwz(7, 4, 4), as::slwi(8, 7, 3), as::add(8, 8, 4), as::stw(3, 0x10, 8), as::stw(5, 0x14, 8),
        as::addi(7, 7, 1), as::stw(7, 4, 4), as::mr(3, 5), as::blr(),
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
        REQUIRE(hle->link_internal_functions({"cellJpgDec"}));
        REQUIRE(hle->main_memory().reserve(kHeap, kHeapSize));
        REQUIRE(mem.commit(kHeap, kHeapSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cellJpgDec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

struct Observed {
    std::vector<std::string> lines;
    std::vector<std::vector<u8>> images;
    std::vector<std::string> labels; // each image's line
};

// CellJpgDecInParam: downScale, method, outputMode, outputColorSpace, outputColorAlpha.
struct Setting {
    u32 scale, method, mode, space, alpha;
};

std::vector<jpeg::Image> test_images()
{
    std::vector<jpeg::Image> v;
    v.push_back(jpeg::pattern(37, 21, false, 2, 2, 0)); // 4:2:0
    v.push_back(jpeg::pattern(37, 21, false, 2, 1, 1)); // 4:2:2
    v.push_back(jpeg::pattern(37, 21, false, 1, 1, 2)); // 4:4:4
    v.push_back(jpeg::pattern(37, 21, true, 1, 1, 3));  // gray
    jpeg::Image r = jpeg::pattern(29, 18, false, 2, 2, 4);
    r.restart_interval = 2;
    r.quality = 90;
    v.push_back(r);
    jpeg::Image a = jpeg::pattern(16, 16, false, 1, 1, 5);
    a.app = 2; // Adobe APP14
    v.push_back(a);
    jpeg::Image n = jpeg::pattern(23, 9, false, 2, 2, 6);
    n.app = 1; // no APP0
    n.quality = 30;
    v.push_back(n);
    jpeg::Image pg = jpeg::pattern(37, 21, true, 1, 1, 7);
    pg.progressive = true;
    v.push_back(pg);
    jpeg::Image pc = jpeg::pattern(29, 18, false, 2, 2, 8);
    pc.progressive = true;
    v.push_back(pc);
    return v;
}

const std::vector<Setting> kSettings = {
    {1, 0, 0, 2, 0},     // RGB, quality (method 0)
    {1, 5, 0, 2, 0},     // RGB, fast (method 5)
    {1, 0, 1, 10, 0x80}, // RGBA bottom to top
    {1, 0, 0, 20, 0x40}, // ARGB
    {1, 0, 0, 1, 0},     // gray
    {1, 0, 0, 3, 0},     // YCbCr
    {1, 0, 0, 11, 0},    // upsampled only
    {1, 0, 0, 30, 0x20}, // 30 (libjpgdec accepts it)
    {1, 0, 0, 31, 0x20}, // 31
    {1, 0, 0, 40, 0x10}, // gray to alpha (RGBA)
    {1, 0, 0, 41, 0x10}, // gray to alpha (ARGB)
    {2, 0, 0, 2, 0},     // 1/2
    {4, 0, 0, 2, 0},     // 1/4
    {8, 0, 0, 2, 0},     // 1/8
    {3, 0, 0, 2, 0},     // 3: libjpgdec rounds
    {16, 0, 0, 2, 0},    // 16
    {2, 5, 0, 2, 0},     // 1/2, fast
    {8, 5, 0, 10, 0xFF}, // 1/8, fast, RGBA
    {1, 1, 0, 2, 0},     // method 1: refused
    {0, 0, 0, 2, 0},     // scale 0
};

bool g_trace = false;

// spu: the decoder created with SPU threads on (CellJpgDecThreadInParam.spuThreadEnable).
Observed jpg_run(u8* b, const Call& call, bool spu = false)
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
    const u32 tin[7] = {spu ? 1u : 0u, 1000, 200, kMallocOpd, kHeapCtl, kFreeOpd, kHeapCtl};
    for (u32 i = 0; i < 7; i++) ps_write32(b, kThreadIn + 4 * i, tin[i]);
    if (r("cellJpgDecCreate", 0xA7978F59, {kHandleOut, kThreadIn, kThreadOut}) != 0) return o;
    const u32 handle = ps_read32(b, kHandleOut);
    note(std::format("codec version {:#x}", ps_read32(b, kThreadOut)));
    const u32 heap_after_create = ps_read32(b, kHeapCtl), mallocs = ps_read32(b, kHeapCtl + 4);
    if (g_trace) {
        std::string sizes;
        for (u32 k = 0; k < mallocs && k < 0xF0; k++) sizes += std::format(" {:#x}", ps_read32(b, kHeapCtl + 0x10 + 8 * k));
        note(std::format("create allocated {} block(s):{}", mallocs, sizes));
    }
    const std::vector<jpeg::Image> images = test_images();
    for (usize n = 0; n < images.size(); n++) {
        const std::vector<u8> file = jpeg::encode(images[n]);
        std::memcpy(b + kFile, file.data(), file.size());
        for (const Setting& s : kSettings) {
            note(std::format("image {} ({}x{}, {}, {}x{} sampling), setting {} {} {} {} {:#x}", n, images[n].width,
                images[n].height, images[n].gray ? "gray" : "colour", images[n].h_sampling, images[n].v_sampling, s.scale,
                s.method, s.mode, s.space, s.alpha));
            std::memset(b + kSrc, 0, 0x20);
            ps_write32(b, kSrc + 0, 1);
            ps_write32(b, kSrc + 20, kFile);
            ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
            if (r("cellJpgDecOpen", 0x976CA5C2, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) continue;
            const u32 stream = ps_read32(b, kStreamOut);
            note(std::format("open: initSpaceAllocated {:#x}", ps_read32(b, kOpenInfo)));
            std::memset(b + kInfo, 0xEE, 0x10);
            if (r("cellJpgDecReadHeader", 0x6D9EBCCF, {handle, stream, kInfo}) == 0) note(std::format("info: {}", hex(b + kInfo, 0x10)));
            std::memset(b + kInParam, 0, 0x18);
            ps_write32(b, kInParam + 4, s.scale);
            ps_write32(b, kInParam + 8, s.method);
            ps_write32(b, kInParam + 12, s.mode);
            ps_write32(b, kInParam + 16, s.space);
            ps_write8(b, kInParam + 20, static_cast<u8>(s.alpha));
            std::memset(b + kOutParam, 0xEE, 0x28);
            const u32 sp = r("cellJpgDecSetParameter", 0xE08F3910, {handle, stream, kInParam, kOutParam});
            note(std::format("out: {}", hex(b + kOutParam, 0x28)));
            if (sp == 0) {
                const u32 width_bytes = static_cast<u32>(ps_read64(b, kOutParam));
                const u32 height = ps_read32(b, kOutParam + 12);
                const u32 pitch = (width_bytes + 16 + 15) & ~15u;
                ps_write64(b, kCtrl, pitch);
                if (pitch * height + 64 <= kImageMax) {
                    std::memset(b + kImage, 0xCC, pitch * height + 64);
                    std::memset(b + kDataOut, 0xEE, 0x10);
                    if (r("cellJpgDecDecodeData", 0xAF8BB012, {handle, stream, kImage, kCtrl, kDataOut}) == 0 ||
                        ps_read32(b, kDataOut + 4) != 0xEEEEEEEE) {
                        note(std::format("data out: {}", hex(b + kDataOut, 0x0C)));
                        o.images.emplace_back(b + kImage, b + kImage + pitch * height + 64);
                        o.labels.push_back(std::format("image {} setting {} {} {} {}", n, s.scale, s.method, s.mode, s.space));
                    }
                }
            }
            r("cellJpgDecClose", 0x9338A07A, {handle, stream});
            std::string sizes;
            for (u32 k = mallocs; k < ps_read32(b, kHeapCtl + 4) && k < 0xF0; k++)
                sizes += std::format("{}{:#x}", k == mallocs ? "" : " ", ps_read32(b, kHeapCtl + 0x10 + 8 * k));
            if (g_trace) note(std::format("allocated: {}; freed {}", sizes, ps_read32(b, kHeapCtl + 8)));
            ps_write32(b, kHeapCtl, heap_after_create);
            ps_write32(b, kHeapCtl + 4, mallocs);
            ps_write32(b, kHeapCtl + 8, 0);
        }
    }
    // Damaged files and calls out of order: H ReadHeader, P SetParameter (RGB), D DecodeData, C Close.
    const std::vector<u8> good = jpeg::encode(images[0]);
    const auto sos = [&] {
        for (usize i = 0; i + 1 < good.size(); i++)
            if (good[i] == 0xFF && good[i + 1] == 0xDA) return i + 2 + (usize{good[i + 2]} << 8 | good[i + 3]);
        return good.size();
    }();
    std::vector<u8> corrupt = good;
    for (usize i = sos + 40; i < sos + 60 && i < corrupt.size() - 2; i++) corrupt[i] = static_cast<u8>(corrupt[i] * 7 + 1);
    for (usize i = sos; i < corrupt.size() - 2; i++)
        if (corrupt[i] == 0xFF) corrupt[i] = 0xFE; // no markers made by accident
    struct Case {
        const char* label;
        std::vector<u8> bytes;
        const char* steps;
    };
    const std::vector<Case> cases = {
        {"truncated in the data", std::vector<u8>(good.begin(), good.begin() + static_cast<long>(sos + (good.size() - sos) / 2)), "HPDC"},
        {"cut after the scan header", std::vector<u8>(good.begin(), good.begin() + static_cast<long>(sos)), "HPDC"},
        {"without its EOI", std::vector<u8>(good.begin(), good.end() - 2), "HPDC"},
        {"damaged data", corrupt, "HPDC"},
        {"damaged data, YCbCr", corrupt, "HYDC"},
        {"damaged data, fast", corrupt, "HFDC"},
        {"not a JPEG", std::vector<u8>(64, 0x55), "HPDC"},
        {"SOI and EOI only", std::vector<u8>{0xFF, 0xD8, 0xFF, 0xD9}, "HPDC"},
        {"decode before the header", good, "DHPDC"},
        {"decode before the parameters", good, "HDPDC"},
        {"header twice", good, "HHPDC"},
        {"parameters twice", good, "HPPDC"},
        {"decode twice", good, "HPDDC"},
        {"parameters before the header", good, "PHPDC"},
    };
    for (const Case& c : cases) {
        note(std::format("case: {}", c.label));
        std::memcpy(b + kFile, c.bytes.data(), c.bytes.size());
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, static_cast<u32>(c.bytes.size()));
        if (r("cellJpgDecOpen", 0x976CA5C2, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) continue;
        const u32 stream = ps_read32(b, kStreamOut);
        bool closed = false;
        for (const char* st = c.steps; *st; st++) {
            if (*st == 'H') {
                std::memset(b + kInfo, 0xEE, 0x10);
                if (r("cellJpgDecReadHeader", 0x6D9EBCCF, {handle, stream, kInfo}) == 0)
                    note(std::format("info: {}", hex(b + kInfo, 0x10)));
            } else if (*st == 'P' || *st == 'Y' || *st == 'F') { // RGB, YCbCr, RGB by the fast method
                std::memset(b + kInParam, 0, 0x18);
                ps_write32(b, kInParam + 4, 1);
                ps_write32(b, kInParam + 8, *st == 'F' ? 5 : 0);
                ps_write32(b, kInParam + 16, *st == 'Y' ? 3 : 2);
                std::memset(b + kOutParam, 0xEE, 0x28);
                if (r("cellJpgDecSetParameter", 0xE08F3910, {handle, stream, kInParam, kOutParam}) == 0)
                    note(std::format("out: {}", hex(b + kOutParam, 0x28)));
            } else if (*st == 'D') {
                const u32 pitch = (37 * 3 + 16 + 15) & ~15u;
                ps_write64(b, kCtrl, pitch);
                std::memset(b + kImage, 0xCC, pitch * 21 + 64);
                std::memset(b + kDataOut, 0xEE, 0x10);
                const u32 v = r("cellJpgDecDecodeData", 0xAF8BB012, {handle, stream, kImage, kCtrl, kDataOut});
                note(std::format("data out: {}", hex(b + kDataOut, 0x0C)));
                if (v == 0 || v == 0x80611102) { // what it wrote, damaged data included
                    o.images.emplace_back(b + kImage, b + kImage + pitch * 21 + 64);
                    o.labels.push_back(std::format("case {}", c.label));
                }
            } else {
                r("cellJpgDecClose", 0x9338A07A, {handle, stream});
                closed = true;
            }
        }
        if (!closed) r("cellJpgDecClose", 0x9338A07A, {handle, stream});
        ps_write32(b, kHeapCtl, heap_after_create);
        ps_write32(b, kHeapCtl + 4, mallocs);
        ps_write32(b, kHeapCtl + 8, 0);
    }
    r("cellJpgDecDestroy", 0xD8EA91F8, {handle});
    return o;
}

} // namespace

TEST_CASE("cellJpgDec: Sony's libjpgdec over JPEGs built here (probe)", "[hle][jpgdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    if (!flash) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellJpgDec");
    g_trace = true;
    const Observed o = jpg_run(f.base(),
        [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    if (const char* dump = std::getenv("PSPOCKET_PROBE_DUMP")) {
        if (FILE* fp = std::fopen(dump, "w")) {
            usize img = 0;
            for (const std::string& l : o.lines) {
                std::fprintf(fp, "%s\n", l.c_str());
                if (l.starts_with("data out:") && img < o.images.size()) {
                    std::fprintf(fp, "IMAGE %zu ", img);
                    for (const u8 c : o.images[img]) std::fprintf(fp, "%02x", c);
                    std::fprintf(fp, "\n");
                    img++;
                }
            }
            std::fclose(fp);
        }
    }
    WARN(std::format("{} lines, {} images", o.lines.size(), o.images.size()));
}

TEST_CASE("cellJpgDec: the same as Sony's libjpgdec (the user's firmware): answers, headers, parameters, images",
    "[hle][jpgdec][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    g_trace = false;
    // Both ways a game can create the decoder: libjpgdec decodes on the PPU, or with SPU threads (its SPURS task; the
    // images are the same, the memory it takes is not).
    const bool spu = GENERATE(false, true);
    INFO(std::format("SPU threads {}", spu ? "on" : "off"));
    Observed ours, theirs;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        ours = jpg_run(f.base(), hle_calls(f, t), spu);
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellJpgDec");
        theirs = jpg_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); }, spu);
    }
    usize differ = 0, image_differ = 0, samples = 0, off_samples = 0;
    int worst = 0;
    std::string report;
    for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++) {
        const std::string x = i < ours.lines.size() ? ours.lines[i] : "(none)";
        const std::string y = i < theirs.lines.size() ? theirs.lines[i] : "(none)";
        if (x != y && ++differ <= 40) report += std::format("line {}:\n  pspocket: {}\n  Sony:     {}\n", i, x, y);
    }
    CHECK(ours.images.size() == theirs.images.size());
    for (usize i = 0; i < std::min(ours.images.size(), theirs.images.size()); i++) {
        const auto& a = ours.images[i];
        const auto& c = theirs.images[i];
        if (a == c) continue;
        image_differ++;
        usize first = a.size();
        for (usize k = 0; k < std::min(a.size(), c.size()); k++) {
            samples++;
            if (a[k] == c[k]) continue;
            off_samples++;
            worst = std::max(worst, std::abs(a[k] - c[k]));
            if (first == a.size()) first = k;
        }
        int worst_here = 0;
        usize off_here = 0;
        for (usize k = 0; k < std::min(a.size(), c.size()); k++) {
            worst_here = std::max(worst_here, std::abs(a[k] - c[k]));
            off_here += a[k] != c[k];
        }
        if (image_differ <= 60)
            report += std::format("{} ({}): {} bytes off, worst {}, first at {}\n", i, theirs.labels[i], off_here, worst_here, first);
        if (image_differ <= 3) {
            std::string at;
            for (usize k = 0, shown = 0; k < std::min(a.size(), c.size()) && shown < 24; k++)
                if (a[k] != c[k]) {
                    at += std::format(" {}:{}/{}", k, a[k], c[k]);
                    shown++;
                }
            report += "  (offset:pspocket/Sony)" + at + "\n";
        }
    }
    INFO(report);
    INFO(std::format("{} lines differ of {}; {} of {} images differ ({} of {} bytes off, worst by {})", differ,
        theirs.lines.size(), image_differ, theirs.images.size(), off_samples, samples, worst));
    CHECK(differ == 0);
    CHECK(image_differ == 0);
    WARN(std::format("compared with Sony's libjpgdec: {} lines, {} images", theirs.lines.size(), theirs.images.size()));
}

// Without the firmware: pspocket's answers and images, as the comparison above found them identical to Sony's
// (run 107), stay so (a digest of every line and image, both ways of creating the decoder).
TEST_CASE("cellJpgDec: pspocket's answers and images stay those found identical to Sony's", "[hle][jpgdec]")
{
    InterpretMisses interpret;
    g_trace = false;
    for (const bool spu : {false, true}) {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        const Observed o = jpg_run(f.base(), hle_calls(f, t), spu);
        u64 h = 0xcbf29ce484222325ull;
        const auto mix = [&](const u8* p, usize n) {
            for (usize i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        };
        for (const std::string& l : o.lines) mix(reinterpret_cast<const u8*>(l.data()), l.size());
        for (const auto& img : o.images) mix(img.data(), img.size());
        INFO(std::format("SPU threads {}: {} lines, {} images, digest {:#018x}", spu ? "on" : "off", o.lines.size(),
            o.images.size(), h));
        CHECK(o.lines.size() == 1848);
        CHECK(o.images.size() == 149);
        CHECK(h == (spu ? 0x98e6136eacfae59cull : 0xa8e9ab51027e06ceull));
    }
}


namespace {

// Where each marker segment of a JPEG starts (its 0xFF), from the first after SOI to the first scan's, then every
// later scan's (found in the entropy-coded data, where 0xFF is stuffed and so never starts a false SOS).
std::vector<std::pair<u8, usize>> segments(const std::vector<u8>& d)
{
    std::vector<std::pair<u8, usize>> out;
    usize at = 2;
    while (at + 4 <= d.size() && d[at] == 0xFF) {
        out.push_back({d[at + 1], at});
        if (d[at + 1] == 0xDA) break;
        at += 2 + (usize{d[at + 2]} << 8 | d[at + 3]);
    }
    for (at += 2; at + 1 < d.size(); at++)
        if (d[at] == 0xFF && d[at + 1] == 0xDA) out.push_back({0xDA, at});
    return out;
}

// What pspocket's decoder makes of `file`: its header read, then the whole decode's answer and error.
struct DecodeOutcome {
    bool header = false, decoded = false;
    std::string error;
};
DecodeOutcome decode_file(const std::vector<u8>& file)
{
    DecodeOutcome o;
    pspocket::hle::jpeg::Decoder d;
    o.header = d.read_header(file.data(), file.size(), o.error);
    if (o.header) o.decoded = d.decode(o.error);
    return o;
}

} // namespace

TEST_CASE("pspocket's JPEG decoder: a scan's tables and band, and a table's counts, are checked before use (D-605)",
    "[hle][jpgdec]")
{
    // Damaged headers the parser fuzzing found (D-603): each read past one of the decoder's tables before, where
    // libjpeg 6b (and libjpgdec) end the decode with an error.
    jpeg::Image gray = jpeg::pattern(29, 18, true, 1, 1, 3);
    const std::vector<u8> baseline = jpeg::encode(gray);
    jpeg::Image colour = jpeg::pattern(29, 18, false, 2, 2, 8);
    colour.progressive = true;
    const std::vector<u8> progressive = jpeg::encode(colour);
    REQUIRE(decode_file(baseline).decoded);
    REQUIRE(decode_file(progressive).decoded);
    const auto first = [](const std::vector<u8>& d, u8 marker, usize nth = 0) {
        for (const auto& [m, at] : segments(d))
            if (m == marker && nth-- == 0) return at;
        FAIL("no such marker");
        return usize{0};
    };

    SECTION("a scan naming Huffman tables past the four (jdhuff.c: JERR_NO_HUFF_TABLE)")
    {
        for (const u8 tables : {u8{0x50}, u8{0x0C}, u8{0xFF}}) {
            std::vector<u8> d = baseline;
            d[first(d, 0xDA) + 6] = tables; // the first component's Td and Ta
            const DecodeOutcome o = decode_file(d);
            CHECK(o.header);
            CHECK_FALSE(o.decoded);
            CHECK(o.error == "a scan without its Huffman table");
        }
    }
    SECTION("a progressive scan's band past the block, or its successive approximation out of range "
            "(jdphuff.c: JERR_BAD_PROGRESSION)")
    {
        const usize ac = first(progressive, 0xDA, 1); // the luma's AC band 1..5: one component
        REQUIRE(progressive[ac + 4] == 1);
        const struct {
            usize at;
            u8 value;
        } cases[] = {
            {ac + 8, 200},                          // Se past 63: the zigzag order and the block
            {ac + 7, 9},                            // Ss after Se
            {ac + 9, 0x0E},                         // Al 14
            {ac + 9, 0x31},                         // a refinement with Al not Ah - 1
            {first(progressive, 0xDA) + 4 + 1 + 2 * 3 + 1, 5}, // the DC scan's band not ending at 0
        };
        for (const auto& c : cases) {
            std::vector<u8> d = progressive;
            d[c.at] = c.value;
            const DecodeOutcome o = decode_file(d);
            INFO(c.at << " " << int{c.value});
            CHECK_FALSE(o.decoded);
            CHECK(o.error.starts_with("a bad progression"));
        }
    }
    SECTION("a DHT whose codes overflow their lengths: read, then refused by the scan that uses it "
            "(jdhuff.c jpeg_make_d_derived_tbl: JERR_BAD_HUFF_TABLE)")
    {
        std::vector<u8> d = baseline;
        const usize dht = first(d, 0xC4); // the luma's DC table: counts at +5, one per length
        REQUIRE((d[dht + 4] & 0xF0) == 0);
        u8* bits = d.data() + dht + 5;
        REQUIRE(bits[2] >= 2);
        bits[0] += 2; // two more codes of 1 bit and two fewer of 3: as many values, an impossible tree
        bits[2] -= 2;
        const DecodeOutcome o = decode_file(d);
        CHECK(o.header);
        CHECK_FALSE(o.decoded);
        CHECK(o.error == "a scan with a bad Huffman table");
    }
}

// ---- the Ext functions (D-617): made on pspocket's decoder as cellPngDec's are (D-595), unverified (D-594). Every
// JPEG through them gives what DecodeData gives: whole, in parts handed to the game's display callback, and from a
// stream the game's stream callback feeds.

namespace {

namespace ext = pspocket::hle::test::imgdec_ext;

constexpr u32 kExtCode = kData + 0x2000, kStrmCtl = kData + 0x2800, kDispCtl = kData + 0xC0000;
constexpr u32 kFrame = kData + 0xD0000, kPart = kData + 0x110000; // the parts' frame (0x40000), one part's buffer
constexpr u32 kExt = kArgs + 0x300, kExtThreadIn = kExt, kExtThreadOut = kExt + 0x10, kCbStrm = kExt + 0x20,
              kExtInfo = kExt + 0x30, kExtIn = kExt + 0x40, kExtOut = kExt + 0x50, kCbDisp = kExt + 0x60,
              kDispParam = kExt + 0x68, kCommandWord = kExt + 0x70;
constexpr u32 kNowhere = 0xF0000000; // not guest memory
constexpr u32 kArg = 0x80611103, kSeq = 0x80611104, kStreamFormat = 0x80611102, kCbParam = 0x80611109;
static_assert(kStrmCtl + 0x1800 <= kArgs && kImage + kImageMax <= kDispCtl && kPart + 0x10000 <= kData + 0x200000);

constexpr u32 kExtCreate = 0x8B300F66, kExtOpen = 0xA9F703E3, kExtReadHeader = 0xB91EB3D2,
              kExtSetParameter = 0x65CBBB16, kExtDecodeData = 0x716F8792;

// One Ext decode.
struct ExtOptions {
    u32 counts = 3;         // CellJpgDecExtInParam's outputCounts
    u32 mode = 0;           // the display callback's (imgdec_ext::disp)
    bool display = true;    // a display callback
    bool game_param = true; // the game's own CellJpgDecDispParam
    u32 first_image = 0;    // the first part's place (0: the frame, or the part's buffer in mode 1)
    u32 first = 0;          // stream input: the source buffer's bytes (0: the whole file, no stream callback)
    u32 chunk = 0;          // the stream callback's part
    u32 stream_size = 0;    // what the stream callback gives in all (0: the file)
    u32 stream_source = 0;  // where it reads (0: the file)
    u32 stop_at = 0;        // the display callback asking CELL_JPGDEC_STOP at this call
    u32 buffer_mode = 1;    // CELL_JPGDEC_LINE_MODE
};

struct ExtRun {
    u32 open = 0, header = 0, parameter = 0, decode = 0, decode_again = 0, stream = 0;
    u64 ext_info = 0, out_width_byte = 0, pitch = 0, row = 0;
    u32 out_height = 0, height = 0, stream_calls_at_header = 0;
    std::string info, out_param, data_out;
    std::vector<u8> frame; // pitch x height + 64 bytes at kFrame
    bool data_untouched = true;
    std::vector<ext::Part> parts;
    std::vector<std::pair<u32, u32>> stream_calls;
};

// jpg_run's frame: 16 bytes past the row, 16-byte aligned.
u64 frame_pitch(u64 row) { return (row + 16 + 15) & ~u64{15}; }

void buffer_source(u8* b, const std::vector<u8>& file, u32 bytes)
{
    std::memcpy(b + kFile, file.data(), file.size());
    std::memset(b + kSrc, 0, 0x20);
    ps_write32(b, kSrc + 0, 1);
    ps_write32(b, kSrc + 20, kFile);
    ps_write32(b, kSrc + 24, bytes);
}

// CellJpgDecInParam: commandPtr, downScale, method, outputMode, outputColorSpace, outputColorAlpha.
void in_param(u8* b, const Setting& s, u32 command)
{
    std::memset(b + kInParam, 0, 0x18);
    ps_write32(b, kInParam + 0, command);
    ps_write32(b, kInParam + 4, s.scale);
    ps_write32(b, kInParam + 8, s.method);
    ps_write32(b, kInParam + 12, s.mode);
    ps_write32(b, kInParam + 16, s.space);
    ps_write8(b, kInParam + 20, static_cast<u8>(s.alpha));
}

// DecodeData's frame for `file` in `s` (the reference), with its CellJpgDecInfo, CellJpgDecOutParam and
// CellJpgDecDataOutInfo.
std::vector<u8> plain_decode(u8* b, const Call& call, u32 handle, const std::vector<u8>& file, const Setting& s,
    std::string* data_out = nullptr, std::string* info = nullptr, std::string* out_param = nullptr)
{
    buffer_source(b, file, static_cast<u32>(file.size()));
    REQUIRE(call("cellJpgDecOpen", 0x976CA5C2, {handle, kStreamOut, kSrc, kOpenInfo}) == 0);
    const u32 stream = ps_read32(b, kStreamOut);
    std::memset(b + kInfo, 0xEE, 0x10);
    REQUIRE(call("cellJpgDecReadHeader", 0x6D9EBCCF, {handle, stream, kInfo}) == 0);
    if (info) *info = hex(b + kInfo, 0x10);
    in_param(b, s, 0);
    std::memset(b + kOutParam, 0xEE, 0x28);
    REQUIRE(call("cellJpgDecSetParameter", 0xE08F3910, {handle, stream, kInParam, kOutParam}) == 0);
    if (out_param) *out_param = hex(b + kOutParam, 0x24);
    const u64 pitch = frame_pitch(ps_read64(b, kOutParam));
    const u32 height = ps_read32(b, kOutParam + 12);
    ps_write64(b, kCtrl, pitch);
    std::memset(b + kFrame, 0xCC, pitch * height + 64);
    std::memset(b + kDataOut, 0xEE, 0x0C);
    const u32 r = call("cellJpgDecDecodeData", 0xAF8BB012, {handle, stream, kFrame, kCtrl, kDataOut});
    REQUIRE((r == 0 || r == kStreamFormat));
    if (data_out) *data_out = std::format("{:#x} {}", r, hex(b + kDataOut, 0x0C));
    std::vector<u8> frame(b + kFrame, b + kFrame + pitch * height + 64);
    REQUIRE(call("cellJpgDecClose", 0x9338A07A, {handle, stream}) == 0);
    return frame;
}

ExtRun ext_decode(u8* b, const Call& call, const ext::Callbacks& cbs, u32 handle, const std::vector<u8>& file,
    const Setting& s, const ExtOptions& e)
{
    ExtRun o;
    buffer_source(b, file, e.first ? e.first : static_cast<u32>(file.size()));
    std::memset(b + kStrmCtl, 0, 0x1800);
    ps_write32(b, kStrmCtl + ext::strm::source, e.stream_source ? e.stream_source : kFile);
    ps_write32(b, kStrmCtl + ext::strm::size, e.stream_size ? e.stream_size : static_cast<u32>(file.size()));
    ps_write32(b, kStrmCtl + ext::strm::offset, e.first);
    ps_write32(b, kStrmCtl + ext::strm::chunk, e.chunk);
    ps_write32(b, kStrmCtl + ext::strm::sample, kDispCtl + ext::disp::count);
    std::memset(b + kDispCtl, 0, 0x40);
    ps_write32(b, kCbStrm, cbs.stream_opd);
    ps_write32(b, kCbStrm + 4, kStrmCtl);
    o.open = call("cellJpgDecExtOpen", kExtOpen, {handle, kStreamOut, kSrc, kOpenInfo, e.first ? kCbStrm : 0});
    if (o.open) return o;
    o.stream = ps_read32(b, kStreamOut);
    std::memset(b + kExtInfo, 0xEE, 8);
    std::memset(b + kInfo, 0xEE, 0x10);
    o.header = call("cellJpgDecExtReadHeader", kExtReadHeader, {handle, o.stream, kInfo, kExtInfo});
    o.ext_info = ps_read64(b, kExtInfo);
    o.info = hex(b + kInfo, 0x10);
    o.stream_calls_at_header = ps_read32(b, kStrmCtl + ext::strm::calls);
    if (o.header == 0) {
        in_param(b, s, kCommandWord);
        ps_write32(b, kCommandWord, 0);
        ps_write32(b, kExtIn + 0, e.buffer_mode);
        ps_write32(b, kExtIn + 4, e.counts);
        ps_write32(b, kExtIn + 8, 0);
        std::memset(b + kExtOut, 0xEE, 16);
        std::memset(b + kOutParam, 0xEE, 0x28);
        o.parameter = call("cellJpgDecExtSetParameter", kExtSetParameter, {handle, o.stream, kInParam, kOutParam, kExtIn, kExtOut});
        o.out_param = hex(b + kOutParam, 0x24);
    }
    if (o.header == 0 && o.parameter == 0) {
        o.out_width_byte = ps_read64(b, kExtOut);
        o.out_height = ps_read32(b, kExtOut + 8);
        o.row = ps_read64(b, kOutParam);
        o.height = ps_read32(b, kOutParam + 12);
        o.pitch = frame_pitch(o.row);
        ps_write64(b, kCtrl, o.pitch);
        std::memset(b + kFrame, 0xCC, o.pitch * o.height + 64);
        std::memset(b + kPart, 0xCC, 0x10000);
        std::memset(b + kImage, 0xCC, 0x100);
        ps_write32(b, kDispCtl + ext::disp::mode, e.mode);
        ps_write32(b, kDispCtl + ext::disp::frame, kFrame);
        ps_write32(b, kDispCtl + ext::disp::stop_at, e.stop_at);
        ps_write32(b, kDispCtl + ext::disp::command, kCommandWord);
        ps_write32(b, kCbDisp, cbs.display_opd);
        ps_write32(b, kCbDisp + 4, kDispCtl);
        const u32 first_image = e.first_image ? e.first_image : e.mode == 1 ? kPart : kFrame;
        ps_write32(b, kDispParam, first_image);
        const bool parts = e.display && e.counts;
        const u32 data = !parts ? kFrame : e.game_param ? kImage : first_image;
        std::memset(b + kDataOut, 0xEE, 0x0C);
        o.decode = call("cellJpgDecExtDecodeData", kExtDecodeData,
            {handle, o.stream, data, kCtrl, kDataOut, e.display ? kCbDisp : 0, e.game_param ? kDispParam : 0});
        o.data_out = std::format("{:#x} {}", o.decode, hex(b + kDataOut, 0x0C));
        o.frame.assign(b + kFrame, b + kFrame + o.pitch * o.height + 64);
        for (u32 i = 0; i < 0x100; i++) o.data_untouched &= b[kImage + i] == 0xCC;
        o.parts = ext::parts(b, kDispCtl);
        o.decode_again = call("cellJpgDecExtDecodeData", kExtDecodeData, {handle, o.stream, data, kCtrl, kDataOut, kCbDisp, 0});
    }
    o.stream_calls = ext::stream_calls(b, kStrmCtl);
    REQUIRE(call("cellJpgDecClose", 0x9338A07A, {handle, o.stream}) == 0);
    return o;
}

} // namespace

TEST_CASE("cellJpgDec: the Ext functions: whole, in parts to a display callback, from a stream callback (D-617)",
    "[hle][jpgdec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Call call = hle_calls(f, t);
    // The five, unverified (D-594); the functions compared with Sony's are not.
    const std::pair<const char*, u32> kExtFunctions[] = {{"cellJpgDecExtCreate", kExtCreate},
        {"cellJpgDecExtOpen", kExtOpen}, {"cellJpgDecExtReadHeader", kExtReadHeader},
        {"cellJpgDecExtSetParameter", kExtSetParameter}, {"cellJpgDecExtDecodeData", kExtDecodeData}};
    for (const auto& [name, nid] : kExtFunctions) {
        CHECK(recomp::nid_of(name) == nid);
        CHECK(hle::registry().is_unverified("cellJpgDec", nid));
    }
    for (const u32 nid : {0xA7978F59u, 0xD8EA91F8u, 0x976CA5C2u, 0x9338A07Au, 0x6D9EBCCFu, 0xE08F3910u, 0xAF8BB012u})
        CHECK_FALSE(hle::registry().is_unverified("cellJpgDec", nid));
    CHECK_FALSE(hle::registry().is_firmware_first("cellJpgDec"));

    write_callbacks(b);
    const ext::Callbacks cbs = ext::write_callbacks(b, kExtCode, 0x3C); // outputImage at CellPngDecDispInfo's place
    std::memset(b + kArgs, 0, 0x400);
    const u32 tin[7] = {0, 1000, 200, kMallocOpd, kHeapCtl, kFreeOpd, kHeapCtl};
    for (u32 i = 0; i < 7; i++) ps_write32(b, kThreadIn + 4 * i, tin[i]);
    // CellJpgDecExtThreadInParam: no SPURS, eight priorities, maxContention.
    for (u32 i = 0; i < 8; i++) ps_write8(b, kExtThreadIn + 4 + i, 8);
    ps_write32(b, kExtThreadIn + 12, 1);
    CHECK(call("cellJpgDecExtCreate", kExtCreate, {kHandleOut, kThreadIn, kThreadOut, kExtThreadIn, kNowhere}) == kArg);
    CHECK(call("cellJpgDecExtCreate", kExtCreate, {kHandleOut, kThreadIn, kThreadOut, kNowhere, kExtThreadOut}) == kArg);
    CHECK(ps_read32(b, kHeapCtl + 4) == 0); // nothing taken for those
    REQUIRE(call("cellJpgDecExtCreate", kExtCreate, {kHandleOut, kThreadIn, kThreadOut, kExtThreadIn, kExtThreadOut}) == 0);
    CHECK(ps_read32(b, kThreadOut) == 0x430000);
    const u32 handle = ps_read32(b, kHandleOut);
    // Create's memory, the same.
    CHECK(ps_read32(b, kHeapCtl + 4) == 1);
    CHECK(ps_read32(b, kHeapCtl + 0x10) == 0x428);

    const std::vector<jpeg::Image> images = test_images();
    const Setting rgb{1, 0, 0, 2, 0}, rgba{1, 0, 0, 10, 0x80}, gray{1, 0, 0, 1, 0}, half{2, 5, 0, 20, 0x40};

    SECTION("whole, without a display callback or with no lines per part: DecodeData's image")
    {
        for (const usize n : {usize{0}, usize{3}, usize{4}, usize{7}, usize{8}}) {
            for (const Setting* s : {&rgb, &gray, &half}) {
                std::string data_out, info, out_param;
                const std::vector<u8> file = jpeg::encode(images[n]);
                const std::vector<u8> want = plain_decode(b, call, handle, file, *s, &data_out, &info, &out_param);
                for (const bool display : {false, true}) {
                    ExtOptions e;
                    e.display = display;
                    e.counts = display ? 0 : 3;
                    const ExtRun o = ext_decode(b, call, cbs, handle, file, *s, e);
                    INFO("image " << n << ", colour space " << s->space << ", display callback " << display);
                    REQUIRE(o.open == 0);
                    CHECK(o.header == 0);
                    CHECK(o.ext_info == 0); // CellJpgDecExtInfo's reserved word
                    CHECK(o.info == info);
                    CHECK(o.parameter == 0);
                    CHECK(o.out_param == out_param);
                    CHECK(o.out_width_byte == o.row);
                    CHECK(o.out_height == (display ? o.height : std::min(3u, o.height)));
                    CHECK(o.decode == 0);
                    CHECK(o.data_out == data_out);
                    CHECK(o.frame == want);
                    CHECK(o.parts.empty());
                    CHECK(o.decode_again == kSeq);
                }
            }
        }
    }

    SECTION("in parts: outputCounts lines to each display callback, the last part too")
    {
        const std::vector<u8> file = jpeg::encode(images[0]); // 37x21, 4:2:0
        std::string data_out;
        const std::vector<u8> want = plain_decode(b, call, handle, file, rgba, &data_out);
        for (const bool game_param : {true, false}) {
            ExtOptions e;
            e.game_param = game_param;
            e.counts = 8;
            const ExtRun o = ext_decode(b, call, cbs, handle, file, rgba, e);
            INFO("the game's dispParam " << game_param);
            REQUIRE(o.decode == 0);
            CHECK(o.out_width_byte == 37 * 4);
            CHECK(o.out_height == 8);
            CHECK(o.data_out == data_out); // the mean, the lines, the status
            CHECK(o.frame == want);
            CHECK(o.data_untouched); // the parts go to nextOutputImage (DecodeData's `data` when the game gives none)
            REQUIRE(o.parts.size() == 3);
            const u32 starts[3] = {0, 8, 16}, heights[3] = {8, 8, 5};
            for (u32 i = 0; i < 3; i++) {
                const ext::Part& p = o.parts[i];
                CHECK(p.frame_width_byte == o.pitch);
                CHECK(p.frame_height == 21);
                CHECK(p.start_x_byte == 0);
                CHECK(p.start_y == starts[i]);
                CHECK(p.width_byte == 37 * 4);
                CHECK(p.height == heights[i]);
                CHECK(p.bit_depth == 8);
                CHECK(p.components == 4);
                CHECK(p.word_34 == starts[i] + heights[i]);       // nextOutputStartY
                CHECK(p.word_38 == 0);                            // scanPassCount
                CHECK(p.word_3c == kFrame + starts[i] * o.pitch); // outputImage
                CHECK(p.param == (game_param ? kDispParam : o.stream + 0x50));
                CHECK(p.next_image == p.word_3c);
            }
            CHECK(o.decode_again == kSeq);
        }
    }

    SECTION("in parts into one buffer the callback copies from; gray; downscaled; progressive")
    {
        struct Case {
            usize image;
            Setting setting;
            u32 counts, lines, width_byte, components;
        };
        const Case cases[] = {{3, gray, 2, 21, 37, 1}, {0, half, 4, 11, 19 * 4, 4}, {8, rgb, 5, 18, 29 * 3, 3},
            {4, rgb, 1, 18, 29 * 3, 3}};
        for (const Case& c : cases) {
            INFO("image " << c.image << ", " << c.counts << " lines a part");
            const std::vector<u8> file = jpeg::encode(images[c.image]);
            const std::vector<u8> want = plain_decode(b, call, handle, file, c.setting);
            ExtOptions e;
            e.mode = 1;
            e.counts = c.counts;
            const ExtRun o = ext_decode(b, call, cbs, handle, file, c.setting, e);
            REQUIRE(o.decode == 0);
            CHECK(o.out_height == c.counts);
            CHECK(o.frame == want);
            REQUIRE(o.parts.size() == (c.lines + c.counts - 1) / c.counts);
            for (u32 i = 0; i < o.parts.size(); i++) {
                CHECK(o.parts[i].word_3c == kPart);
                CHECK(o.parts[i].start_y == i * c.counts);
                CHECK(o.parts[i].height == std::min(c.counts, c.lines - i * c.counts));
                CHECK(o.parts[i].components == c.components);
                CHECK(o.parts[i].width_byte == c.width_byte);
                CHECK(o.parts[i].frame_height == c.lines);
            }
        }
    }

    SECTION("from a stream: the source's first bytes, the rest from the stream callback as the decoder needs them")
    {
        for (const usize n : {usize{0}, usize{4}, usize{7}, usize{8}}) { // baseline; restart markers; progressive
            INFO("image " << n);
            const std::vector<u8> file = jpeg::encode(images[n]);
            std::string data_out, info;
            const std::vector<u8> want = plain_decode(b, call, handle, file, rgb, &data_out, &info);
            ExtOptions e;
            e.first = 20;
            e.chunk = 37;
            e.counts = 4;
            const ExtRun o = ext_decode(b, call, cbs, handle, file, rgb, e);
            REQUIRE(o.header == 0);
            CHECK(o.info == info);
            CHECK(o.stream_calls_at_header > 0); // the header's segments asked for at ReadHeader
            CHECK(o.stream_calls_at_header < o.stream_calls.size());
            REQUIRE(o.decode == 0);
            CHECK(o.frame == want);
            CHECK(o.data_out == data_out);
            // Each call told the bytes given so far; every byte given, none asked for past EOI.
            REQUIRE(o.stream_calls.size() == (file.size() - 20 + 36) / 37);
            for (usize i = 0; i < o.stream_calls.size(); i++) CHECK(o.stream_calls[i].first == 20 + 37 * i);
            // The rest asked for before any part went out: the image is decoded whole first.
            CHECK(o.stream_calls.back().second == 0);
        }
        {
            // The plain ReadHeader, SetParameter and DecodeData on a stream opened with a stream callback do the same.
            const std::vector<u8> file = jpeg::encode(images[2]);
            std::string data_out;
            const std::vector<u8> want = plain_decode(b, call, handle, file, rgb, &data_out);
            buffer_source(b, file, 50);
            std::memset(b + kStrmCtl, 0, 0x1800);
            ps_write32(b, kStrmCtl + ext::strm::source, kFile);
            ps_write32(b, kStrmCtl + ext::strm::size, static_cast<u32>(file.size()));
            ps_write32(b, kStrmCtl + ext::strm::offset, 50);
            ps_write32(b, kStrmCtl + ext::strm::chunk, 100);
            ps_write32(b, kCbStrm, cbs.stream_opd);
            ps_write32(b, kCbStrm + 4, kStrmCtl);
            REQUIRE(call("cellJpgDecExtOpen", kExtOpen, {handle, kStreamOut, kSrc, kOpenInfo, kCbStrm}) == 0);
            const u32 stream = ps_read32(b, kStreamOut);
            REQUIRE(call("cellJpgDecReadHeader", 0x6D9EBCCF, {handle, stream, kInfo}) == 0);
            in_param(b, rgb, 0);
            REQUIRE(call("cellJpgDecSetParameter", 0xE08F3910, {handle, stream, kInParam, kOutParam}) == 0);
            const u64 pitch = frame_pitch(ps_read64(b, kOutParam));
            ps_write64(b, kCtrl, pitch);
            std::memset(b + kFrame, 0xCC, pitch * 21 + 64);
            std::memset(b + kDataOut, 0xEE, 0x0C);
            const u32 r = call("cellJpgDecDecodeData", 0xAF8BB012, {handle, stream, kFrame, kCtrl, kDataOut});
            CHECK(std::format("{:#x} {}", r, hex(b + kDataOut, 0x0C)) == data_out);
            CHECK(std::vector<u8>(b + kFrame, b + kFrame + pitch * 21 + 64) == want);
            CHECK(ps_read32(b, kStrmCtl + ext::strm::calls) == (file.size() - 50 + 99) / 100);
            REQUIRE(call("cellJpgDecClose", 0x9338A07A, {handle, stream}) == 0);
        }
    }

    SECTION("CELL_JPGDEC_STOP at commandPtr from a display callback stops the decoding")
    {
        const std::vector<u8> file = jpeg::encode(images[0]);
        const std::vector<u8> want = plain_decode(b, call, handle, file, rgba);
        ExtOptions e;
        e.counts = 4;
        e.stop_at = 2;
        const ExtRun o = ext_decode(b, call, cbs, handle, file, rgba, e);
        CHECK(o.decode == 0);
        CHECK(o.data_out.ends_with("0000000800000001")); // outputLines 8, CELL_JPGDEC_DEC_STATUS_STOP
        CHECK(o.parts.size() == 2);
        CHECK(std::equal(o.frame.begin(), o.frame.begin() + static_cast<std::ptrdiff_t>(8 * o.pitch), want.begin()));
        CHECK(o.frame[8 * o.pitch] == 0xCC); // nothing past the second part
        CHECK(o.decode_again == kSeq);
    }

    SECTION("what the Ext functions refuse")
    {
        const std::vector<u8> file = jpeg::encode(images[0]);
        {
            // A buffer mode that is not CELL_JPGDEC_LINE_MODE: ARG, and the stream fails.
            ExtOptions e;
            e.buffer_mode = 2;
            const ExtRun o = ext_decode(b, call, cbs, handle, file, rgb, e);
            CHECK(o.header == 0);
            CHECK(o.parameter == kArg);
        }
        buffer_source(b, file, static_cast<u32>(file.size()));
        REQUIRE(call("cellJpgDecExtOpen", kExtOpen, {handle, kStreamOut, kSrc, kOpenInfo, 0}) == 0);
        u32 stream = ps_read32(b, kStreamOut);
        CHECK(call("cellJpgDecExtReadHeader", kExtReadHeader, {handle, stream, kInfo, kNowhere}) == kArg);
        REQUIRE(call("cellJpgDecExtReadHeader", kExtReadHeader, {handle, stream, kInfo, 0}) == 0);
        in_param(b, rgb, 0);
        ps_write32(b, kExtIn + 0, 2);
        ps_write32(b, kExtIn + 4, 3);
        CHECK(call("cellJpgDecExtSetParameter", kExtSetParameter, {handle, stream, kInParam, kOutParam, kExtIn, kExtOut}) == kArg);
        ps_write64(b, kCtrl, 0x100);
        CHECK(call("cellJpgDecExtDecodeData", kExtDecodeData, {handle, stream, kFrame, kCtrl, kDataOut, kCbDisp, 0}) == kSeq);
        REQUIRE(call("cellJpgDecClose", 0x9338A07A, {handle, stream}) == 0);
        // A stream callback with a file source: STREAM_FORMAT, the handle still free.
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 4, kSrc + 0x100);
        std::memcpy(b + kSrc + 0x100, "/app_home/images.jpg", 21);
        ps_write32(b, kCbStrm, cbs.stream_opd);
        ps_write32(b, kCbStrm + 4, kStrmCtl);
        CHECK(call("cellJpgDecExtOpen", kExtOpen, {handle, kStreamOut, kSrc, kOpenInfo, kCbStrm}) == kStreamFormat);
        CHECK(call("cellJpgDecExtOpen", kExtOpen, {handle, kStreamOut, kSrc, kOpenInfo, kNowhere}) == kArg);
        buffer_source(b, file, static_cast<u32>(file.size()));
        REQUIRE(call("cellJpgDecOpen", 0x976CA5C2, {handle, kStreamOut, kSrc, kOpenInfo}) == 0);
        stream = ps_read32(b, kStreamOut);
        REQUIRE(call("cellJpgDecClose", 0x9338A07A, {handle, stream}) == 0);
        {
            ExtOptions e; // the stream callback's bytes are not guest memory
            e.first = 20;
            e.chunk = 37;
            e.stream_source = kNowhere;
            CHECK(ext_decode(b, call, cbs, handle, file, rgb, e).header == kCbParam);
        }
        {
            ExtOptions e; // the stream callback's bytes stop short of the first byte: no SOI
            e.first = 1;
            e.chunk = 37;
            e.stream_source = kNowhere;
            CHECK(ext_decode(b, call, cbs, handle, file, rgb, e).header == kCbParam);
        }
        {
            ExtOptions e; // the stream ends in the header
            e.first = 20;
            e.chunk = 37;
            e.stream_size = 100;
            CHECK(ext_decode(b, call, cbs, handle, file, rgb, e).header == kStreamFormat);
        }
        {
            // The stream ends in the scan: the lines decoded go out in parts, then STREAM_FORMAT (as DecodeData).
            ExtOptions e;
            e.first = 20;
            e.chunk = 37;
            e.counts = 4;
            e.stream_size = static_cast<u32>(file.size()) - 60;
            std::string data_out;
            const std::vector<u8> cut(file.begin(), file.end() - 60);
            const std::vector<u8> want = plain_decode(b, call, handle, cut, rgb, &data_out);
            const ExtRun o = ext_decode(b, call, cbs, handle, file, rgb, e);
            CHECK(o.decode == kStreamFormat);
            CHECK(o.data_out.substr(0, 10) == data_out.substr(0, 10)); // the answer
            CHECK(o.data_out.substr(19, 8) == data_out.substr(19, 8)); // outputLines
            CHECK(o.frame == want);
            CHECK_FALSE(o.parts.empty());
            CHECK(o.decode_again == kSeq);
        }
        {
            ExtOptions e; // the display callback gives no place for the next part
            e.mode = 2;
            const ExtRun o = ext_decode(b, call, cbs, handle, file, rgb, e);
            CHECK(o.decode == kCbParam);
            CHECK(o.parts.size() == 1);
        }
        {
            ExtOptions e; // the first part's place is not guest memory
            e.first_image = kNowhere;
            const ExtRun o = ext_decode(b, call, cbs, handle, file, rgb, e);
            CHECK(o.decode == kArg);
            CHECK(o.parts.empty());
        }
    }
    REQUIRE(call("cellJpgDecDestroy", 0xD8EA91F8, {handle}) == 0);
}
