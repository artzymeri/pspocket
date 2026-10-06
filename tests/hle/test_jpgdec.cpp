// cellJpgDec (D-200): pspocket's JPEG decoder, alone and against Sony's libjpgdec (IJG libjpeg 6b) from the user's
// installed system software (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp). The JPEGs are built here
// (jpeg_writer.hpp: gray and colour, every chroma sampling, restart markers, JFIF, Adobe or no header); the same calls
// go to both decoders and every answer, header, parameter set and output row is compared.

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
#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_writer.hpp"
#include "pspocket/ppu/runtime.h"
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

