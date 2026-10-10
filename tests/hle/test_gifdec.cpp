// cellGifDec (D-202): pspocket's GIF decoder, alone and against Sony's libgifdec from the user's installed system
// software (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp). The GIFs are built here (gif_writer.hpp: GIF87a and 89a,
// global and local palettes of every size, frames anywhere on the screen, interlacing, LZW at every code size with
// early clears and short sub-blocks, extensions, several frames); the same calls go to both decoders and every answer,
// header, parameter set, record and output row is compared.

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
#include "gif_writer.hpp"
#include "imgdec_ext_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace gif = pspocket::test::gif;

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
constexpr u32 kFile = kData + 0x10000; // the GIF, up to 0x30000 bytes
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
        REQUIRE(hle->main_memory().reserve(kHeap, kHeapSize));
        REQUIRE(mem.commit(kHeap, kHeapSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cellGifDec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

struct Observed {
    std::vector<std::string> lines;
    std::vector<std::vector<u8>> images;
    std::vector<std::string> labels; // each image's line
};

using Rgb = std::array<std::uint8_t, 3>;

std::vector<gif::File> test_files()
{
    std::vector<gif::File> v;
    const auto frame = [](u32 left, u32 top, u32 w, u32 h, int bits, u32 seed) {
        gif::Frame fr;
        fr.left = left;
        fr.top = top;
        fr.width = w;
        fr.height = h;
        fr.indices = gif::indices(w, h, bits, seed);
        return fr;
    };
    const auto gce = [](int disposal, bool transparent, int index, int delay) {
        gif::Extension e;
        e.label = 0xF9;
        e.blocks.push_back({static_cast<u8>((disposal << 2) | (transparent ? 1 : 0)), static_cast<u8>(delay),
            static_cast<u8>(delay >> 8), static_cast<u8>(index)});
        return e;
    };
    {
        gif::File f; // 0: a plain 4-bit image
        f.width = 13;
        f.height = 9;
        f.global_bits = 4;
        f.global = gif::palette(4, 0);
        f.frames.push_back(frame(0, 0, 13, 9, 4, 0));
        v.push_back(f);
    }
    {
        gif::File f = v[0]; // 1: transparency
        f.frames[0].extensions.push_back(gce(0, true, 3, 10));
        v.push_back(f);
    }
    {
        gif::File f; // 2: interlaced, 256 colours
        f.width = 11;
        f.height = 17;
        f.global_bits = 8;
        f.global = gif::palette(8, 1);
        f.frames.push_back(frame(0, 0, 11, 17, 8, 1));
        f.frames[0].interlaced = true;
        v.push_back(f);
    }
    {
        gif::File f; // 3: a smaller frame with a local palette, a background colour
        f.width = 16;
        f.height = 12;
        f.global_bits = 2;
        f.global = gif::palette(2, 2);
        f.background = 2;
        f.aspect = 49;
        f.sorted = true;
        f.color_resolution = 3;
        gif::Frame fr = frame(2, 3, 9, 5, 3, 2);
        fr.local_bits = 3;
        fr.local = gif::palette(3, 3);
        f.frames.push_back(fr);
        v.push_back(f);
    }
    {
        gif::File f; // 4: two colours, the smallest code size
        f.width = 21;
        f.height = 7;
        f.global_bits = 1;
        f.global = {Rgb{0, 0, 0}, Rgb{255, 255, 255}};
        f.frames.push_back(frame(0, 0, 21, 7, 1, 4));
        v.push_back(f);
    }
    {
        gif::File f = v[0]; // 5: early clear codes, short sub-blocks
        f.frames[0].clear_every = 10;
        f.frames[0].block_size = 7;
        f.frames[0].min_code_size = 5;
        v.push_back(f);
    }
    {
        gif::File f = v[0]; // 6: GIF87a
        f.gif87 = true;
        v.push_back(f);
    }
    {
        gif::File f; // 7: an animation: application and comment extensions, two frames
        f.width = 12;
        f.height = 10;
        f.global_bits = 3;
        f.global = gif::palette(3, 7);
        gif::Frame a = frame(0, 0, 12, 10, 3, 7);
        gif::Extension app;
        app.label = 0xFF;
        app.blocks.push_back({'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'});
        app.blocks.push_back({1, 0, 0});
        gif::Extension com;
        com.label = 0xFE;
        com.blocks.push_back({'p', 's', 'p', 'o', 'c', 'k', 'e', 't'});
        a.extensions = {app, com, gce(1, false, 0, 5)};
        gif::Frame b = frame(3, 2, 6, 5, 3, 8);
        b.extensions = {gce(2, true, 1, 5)};
        f.frames = {a, b};
        v.push_back(f);
    }
    {
        gif::File f = v[0]; // 8: no trailer
        f.trailer = false;
        v.push_back(f);
    }
    {
        gif::File f; // 9: a frame running past the screen's right and bottom
        f.width = 10;
        f.height = 8;
        f.global_bits = 4;
        f.global = gif::palette(4, 9);
        f.background = 5;
        f.frames.push_back(frame(6, 5, 7, 6, 4, 9));
        v.push_back(f);
    }
    {
        gif::File f; // 10: no colour table at all
        f.width = 9;
        f.height = 6;
        f.frames.push_back(frame(0, 0, 9, 6, 3, 10));
        v.push_back(f);
    }
    {
        gif::File f = v[3]; // 11: a background index at the table's last entry
        f.background = 7;
        v.push_back(f);
    }
    {
        gif::File f; // 12: noise filling the LZW table (4096 codes)
        f.width = 64;
        f.height = 64;
        f.global_bits = 8;
        f.global = gif::palette(8, 12);
        gif::Frame fr = frame(0, 0, 64, 64, 8, 12);
        for (usize i = 0; i < fr.indices.size(); i++) fr.indices[i] = static_cast<u8>((i * 2654435761u >> 13) & 255);
        f.frames.push_back(fr);
        v.push_back(f);
    }
    {
        gif::File f = v[0]; // 13: a long comment (four sub-blocks, 620 bytes) and a plain text extension
        gif::Extension com;
        com.label = 0xFE;
        com.blocks.push_back(std::vector<u8>(255, 'a'));
        com.blocks.push_back(std::vector<u8>(45, 'b'));
        com.blocks.push_back(std::vector<u8>(255, 'c'));
        com.blocks.push_back(std::vector<u8>(60, 'd'));
        gif::Extension text;
        text.label = 0x01;
        text.blocks.push_back({0, 0, 0, 0, 13, 0, 9, 0, 8, 8, 1, 0});
        text.blocks.push_back({'h', 'i'});
        f.frames[0].extensions = {com, text};
        v.push_back(f);
    }
    {
        gif::File f; // 14: interlaced frames of 1, 2, 3 and 5 rows
        f.width = 7;
        f.height = 5;
        f.global_bits = 3;
        f.global = gif::palette(3, 14);
        for (const u32 h : {1u, 2u, 3u, 5u}) {
            gif::Frame fr = frame(0, 0, 7, h, 3, 14 + h);
            fr.interlaced = true;
            f.frames.push_back(fr);
        }
        v.push_back(f);
    }
    {
        gif::File f = v[3]; // 15: the background index is the transparent one
        f.frames[0].extensions.push_back(gce(0, true, 2, 0));
        v.push_back(f);
    }
    {
        gif::File f = v[7]; // 16: transparency on the first frame only (the second has no graphic control)
        f.frames[0].extensions = {gce(1, true, 3, 5)};
        f.frames[1].extensions.clear();
        v.push_back(f);
    }
    {
        gif::File f = v[12]; // 17: noise past the full LZW table with no clear code (the "deferred clear")
        f.frames[0].clear_every = -1;
        v.push_back(f);
    }
    {
        gif::File f = v[0]; // 18: a second image, after a graphic control extension
        f.frames.push_back(f.frames[0]);
        f.frames[1].extensions.push_back(gce(0, true, 2, 0));
        v.push_back(f);
    }
    return v;
}

// CellGifDecInParam: the command word (0: none, 1: a word asking to continue, 2: one asking to stop), colour space
// (RGBA 10, ARGB 20), outputColorAlpha1, outputColorAlpha2.
struct Setting {
    u32 space;
    u8 alpha1, alpha2;
    u32 command = 0;
};
const std::vector<Setting> kSettings = {{10, 0xFF, 0x00}, {20, 0x80, 0x40}, {10, 0x00, 0xFF}, {2, 0xFF, 0xFF}};
constexpr u32 kCommand = kArgs + 0x120;

bool g_trace = false;

// spu: the decoder created with SPU threads on (CellGifDecThreadInParam.spuThreadEnable).
Observed gif_run(u8* b, const Call& call, bool spu = false)
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
    if (r("cellGifDecCreate", 0xB60D42A5, {kHandleOut, kThreadIn, kThreadOut}) != 0) return o;
    const u32 handle = ps_read32(b, kHandleOut);
    note(std::format("codec version {:#x}", ps_read32(b, kThreadOut)));
    const u32 heap_after_create = ps_read32(b, kHeapCtl), mallocs = ps_read32(b, kHeapCtl + 4);
    if (g_trace) {
        std::string sizes;
        for (u32 k = 0; k < mallocs && k < 0xF0; k++) sizes += std::format(" {:#x}", ps_read32(b, kHeapCtl + 0x10 + 8 * k));
        note(std::format("create allocated {} block(s):{}", mallocs, sizes));
    }
    const std::vector<gif::File> files = test_files();
    const auto run_file = [&](const std::string& label, const std::vector<u8>& file, const Setting& s) {
        note(label);
        std::memcpy(b + kFile, file.data(), file.size());
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
        if (r("cellGifDecOpen", 0x75745079, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) return;
        const u32 stream = ps_read32(b, kStreamOut);
        note(std::format("open: initSpaceAllocated {:#x}", ps_read32(b, kOpenInfo)));
        std::memset(b + kInfo, 0xEE, 0x20);
        if (r("cellGifDecReadHeader", 0xF0DA95DE, {handle, stream, kInfo}) == 0) note(std::format("info: {}", hex(b + kInfo, 0x20)));
        std::memset(b + kInParam, 0, 0x10);
        if (s.command) {
            ps_write32(b, kCommand, s.command - 1);
            ps_write32(b, kInParam, kCommand);
        }
        ps_write32(b, kInParam + 4, s.space);
        ps_write8(b, kInParam + 8, s.alpha1);
        ps_write8(b, kInParam + 9, s.alpha2);
        std::memset(b + kOutParam, 0xEE, 0x28);
        const u32 sp = r("cellGifDecSetParameter", 0x41A90DC4, {handle, stream, kInParam, kOutParam});
        note(std::format("out: {}", hex(b + kOutParam, 0x28)));
        if (sp == 0) {
            const u32 width_bytes = static_cast<u32>(ps_read64(b, kOutParam));
            const u32 height = ps_read32(b, kOutParam + 12);
            const u32 pitch = (width_bytes + 16 + 15) & ~15u;
            ps_write64(b, kCtrl, pitch);
            for (int k = 0; k < 12 && pitch * height + 64 <= kImageMax; k++) {
                std::memset(b + kImage, 0xCC, pitch * height + 64);
                std::memset(b + kDataOut, 0xEE, 0x10);
                const u32 v = r("cellGifDecDecodeData", 0x44B1BC61, {handle, stream, kImage, kCtrl, kDataOut});
                note(std::format("data out: {}", hex(b + kDataOut, 0x10)));
                if (v != 0) { // what it wrote before failing
                    o.images.emplace_back(b + kImage, b + kImage + pitch * height + 64);
                    o.labels.push_back(std::format("{} record {} (failed)", label, k));
                    break;
                }
                const u32 type = ps_read32(b, kDataOut);
                if (type == 1) {
                    o.images.emplace_back(b + kImage, b + kImage + pitch * height + 64);
                    o.labels.push_back(std::format("{} record {}", label, k));
                } else if (type == 2) {
                    // The extension's bytes: its sub-blocks up to the terminator (the rest of libgifdec's buffer is its
                    // own state, not the record's).
                    const u32 p = ps_read32(b, kDataOut + 8);
                    u32 n = 0;
                    while (p && n < 0x400 && ps_read8(b, p + n) != 0) n += 1 + ps_read8(b, p + n);
                    note(std::format("extension {:#04x}: {}", ps_read8(b, kDataOut + 4),
                        p ? hex(b + p, std::getenv("PSPOCKET_PROBE_WIDE") ? 0x100 : n + 1) : std::string("(null)")));
                } else {
                    break;
                }
            }
        }
        r("cellGifDecClose", 0x116A7DA9, {handle, stream});
        std::string sizes;
        for (u32 k = mallocs; k < ps_read32(b, kHeapCtl + 4) && k < 0xF0; k++)
            sizes += std::format("{}{:#x}", k == mallocs ? "" : " ", ps_read32(b, kHeapCtl + 0x10 + 8 * k));
        if (g_trace) note(std::format("allocated: {}; freed {}", sizes, ps_read32(b, kHeapCtl + 8)));
        ps_write32(b, kHeapCtl, heap_after_create);
        ps_write32(b, kHeapCtl + 4, mallocs);
        ps_write32(b, kHeapCtl + 8, 0);
    };
    for (usize n = 0; n < files.size(); n++) {
        const std::vector<u8> file = gif::encode(files[n]);
        for (const Setting& s : kSettings)
            run_file(std::format("file {} ({}x{}), setting {} {:#x} {:#x}", n, files[n].width, files[n].height, s.space,
                         s.alpha1, s.alpha2),
                file, s);
    }
    // Damaged files (from file 0) and the stop command.
    const std::vector<u8> good = gif::encode(files[0]);
    const usize data_at = 13 + 48 + 10; // header, screen, 16-colour table, image descriptor
    std::vector<u8> bad_code = good;
    bad_code[data_at + 3] = 0xFF; // a code past the table
    bad_code[data_at + 4] = 0xFF;
    const std::vector<std::pair<std::string, std::vector<u8>>> damaged = {
        {"damaged: a code past the table", bad_code},
        {"damaged: cut in the image data", std::vector<u8>(good.begin(), good.begin() + static_cast<long>(data_at + 20))},
        {"damaged: cut after the image descriptor", std::vector<u8>(good.begin(), good.begin() + static_cast<long>(data_at))},
        {"damaged: cut in the colour table", std::vector<u8>(good.begin(), good.begin() + 30)},
        {"damaged: not a GIF", std::vector<u8>(64, 0x47)},
        {"damaged: GIF90a", [&] { std::vector<u8> x = good; x[4] = '0'; return x; }()},
    };
    for (const auto& [label, bytes] : damaged) run_file(label, bytes, kSettings[0]);
    // Calls out of order: H ReadHeader, P SetParameter (RGBA), D DecodeData, C Close.
    const auto run_steps = [&](const std::string& label, const std::vector<u8>& file, const char* steps) {
        note(label);
        std::memcpy(b + kFile, file.data(), file.size());
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, kFile);
        ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
        if (r("cellGifDecOpen", 0x75745079, {handle, kStreamOut, kSrc, kOpenInfo}) != 0) return;
        const u32 stream = ps_read32(b, kStreamOut);
        const u32 pitch = (files[0].width * 4 + 16 + 15) & ~15u;
        bool closed = false;
        for (const char* st = steps; *st; st++) {
            if (*st == 'H') {
                std::memset(b + kInfo, 0xEE, 0x20);
                if (r("cellGifDecReadHeader", 0xF0DA95DE, {handle, stream, kInfo}) == 0)
                    note(std::format("info: {}", hex(b + kInfo, 0x20)));
            } else if (*st == 'P') {
                std::memset(b + kInParam, 0, 0x10);
                ps_write32(b, kInParam + 4, 10);
                ps_write8(b, kInParam + 8, 0xFF);
                std::memset(b + kOutParam, 0xEE, 0x28);
                if (r("cellGifDecSetParameter", 0x41A90DC4, {handle, stream, kInParam, kOutParam}) == 0)
                    note(std::format("out: {}", hex(b + kOutParam, 0x28)));
            } else if (*st == 'D' || *st == 'N') { // N: a row pitch narrower than the image
                ps_write64(b, kCtrl, *st == 'N' ? files[0].width * 4 - 4 : pitch);
                std::memset(b + kImage, 0xCC, pitch * files[0].height + 64);
                std::memset(b + kDataOut, 0xEE, 0x10);
                const u32 v = r("cellGifDecDecodeData", 0x44B1BC61, {handle, stream, kImage, kCtrl, kDataOut});
                note(std::format("data out: {}", hex(b + kDataOut, 0x10)));
                if (v == 0 && ps_read32(b, kDataOut) == 1) {
                    o.images.emplace_back(b + kImage, b + kImage + pitch * files[0].height + 64);
                    o.labels.push_back(std::format("{} step {}", label, st - steps));
                }
            } else {
                r("cellGifDecClose", 0x116A7DA9, {handle, stream});
                closed = true;
            }
        }
        if (!closed) r("cellGifDecClose", 0x116A7DA9, {handle, stream});
        ps_write32(b, kHeapCtl, heap_after_create);
        ps_write32(b, kHeapCtl + 4, mallocs);
        ps_write32(b, kHeapCtl + 8, 0);
    };
    run_steps("order: decode before the header", good, "DHPDC");
    run_steps("order: decode before the parameters", good, "HDPDC");
    run_steps("order: header twice", good, "HHPDC");
    run_steps("order: parameters twice", good, "HPPDC");
    run_steps("order: past the end", good, "HPDDDDC");
    run_steps("order: parameters between records", gif::encode(files[1]), "HPDPDDC");
    run_steps("order: parameters before the header", good, "PHPDC");
    run_steps("order: a narrow row pitch", good, "HPNDC");
    run_file("stop asked", good, {10, 0xFF, 0x00, 2});
    run_file("continue asked", gif::encode(files[7]), {10, 0xFF, 0x00, 1});
    r("cellGifDecDestroy", 0xE74B2CB1, {handle});
    return o;
}

} // namespace

TEST_CASE("cellGifDec: Sony's libgifdec over GIFs built here (probe)", "[hle][gifdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    if (!flash) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellGifDec");
    g_trace = true;
    const Observed o = gif_run(f.base(),
        [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); },
        std::getenv("PSPOCKET_PROBE_SPU") != nullptr);
    if (const char* dump = std::getenv("PSPOCKET_PROBE_DUMP")) {
        if (FILE* fp = std::fopen(dump, "w")) {
            for (const std::string& l : o.lines) std::fprintf(fp, "%s\n", l.c_str());
            for (usize i = 0; i < o.images.size(); i++) {
                std::fprintf(fp, "IMAGE %s ", o.labels[i].c_str());
                for (const u8 c : o.images[i]) std::fprintf(fp, "%02x", c);
                std::fprintf(fp, "\n");
            }
            std::fclose(fp);
        }
    }
    WARN(std::format("{} lines, {} images", o.lines.size(), o.images.size()));
}

TEST_CASE("cellGifDec: the same as Sony's libgifdec (the user's firmware): answers, headers, parameters, records, images",
    "[hle][gifdec][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    g_trace = false;
    const bool spu = GENERATE(false, true);
    INFO(std::format("SPU threads {}", spu ? "on" : "off"));
    Observed ours, theirs;
    {
        BigFixture f;
        REQUIRE(f.hle->link_internal_functions({"cellGifDec"}));
        GuestThread t(f, 0, "ppu");
        ours = gif_run(f.base(), hle_calls(f, t), spu);
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellGifDec");
        theirs = gif_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); }, spu);
    }
    usize differ = 0, image_differ = 0;
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
        usize off = 0, first = a.size();
        for (usize k = 0; k < std::min(a.size(), c.size()); k++)
            if (a[k] != c[k]) {
                off++;
                if (first == a.size()) first = k;
            }
        if (++image_differ <= 20) report += std::format("{} ({}): {} bytes off, first at {}\n", i, theirs.labels[i], off, first);
    }
    INFO(report);
    INFO(std::format("{} lines differ of {}; {} of {} images differ", differ, theirs.lines.size(), image_differ,
        theirs.images.size()));
    CHECK(differ == 0);
    CHECK(image_differ == 0);
    WARN(std::format("compared with Sony's libgifdec: {} lines, {} images", theirs.lines.size(), theirs.images.size()));
}

// Without the firmware: pspocket's answers and images, as the comparison above found them identical to Sony's
// (run 108), stay so (a digest of every line and image, both ways of creating the decoder).
TEST_CASE("cellGifDec: pspocket's answers and images stay those found identical to Sony's", "[hle][gifdec]")
{
    InterpretMisses interpret;
    g_trace = false;
    for (const bool spu : {false, true}) {
        BigFixture f;
        REQUIRE(f.hle->link_internal_functions({"cellGifDec"}));
        GuestThread t(f, 0, "ppu");
        const Observed o = gif_run(f.base(), hle_calls(f, t), spu);
        u64 h = 0xcbf29ce484222325ull;
        const auto mix = [&](const u8* p, usize n) {
            for (usize i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        };
        for (const std::string& l : o.lines) mix(reinterpret_cast<const u8*>(l.data()), l.size());
        for (const auto& img : o.images) mix(img.data(), img.size());
        INFO(std::format("SPU threads {}: {} lines, {} images, digest {:#018x}", spu ? "on" : "off", o.lines.size(),
            o.images.size(), h));
        CHECK(o.lines.size() == 1131);
        CHECK(o.images.size() == 85);
        CHECK(h == (spu ? 0xdaef63078b88c4e8ull : 0xb134c4b9baf4316bull));
    }
}

// ---- the Ext functions (D-595): RPCS3's checks on pspocket's decoder, with stream input and partial output as RPCS3's
// cellPngDec gives them; unverified (D-594). Every file through them gives the records DecodeData gives: whole, each
// image's screen in parts handed to the game's display callback, and from a stream the game's stream callback feeds.

namespace {

namespace ext = pspocket::hle::test::imgdec_ext;

constexpr u32 kExtCode = kData + 0x2000, kStrmCtl = kData + 0x2800, kDispCtl = kData + 0xC0000;
constexpr u32 kFrame = kData + 0xD0000, kPart = kData + 0x150000; // the parts' frame (0x80000), one part's buffer
constexpr u32 kExt = kArgs + 0x300, kExtThreadIn = kExt, kExtThreadOut = kExt + 0x10, kCbStrm = kExt + 0x20,
              kExtInfo = kExt + 0x30, kExtIn = kExt + 0x40, kExtOut = kExt + 0x50, kCbDisp = kExt + 0x60,
              kDispParam = kExt + 0x68, kCommandWord = kExt + 0x70;
constexpr u32 kNowhere = 0xF0000000; // not guest memory
constexpr u32 kArg = 0x80611303, kSeq = 0x80611302, kStreamFormat = 0x80611301, kCbParam = 0x80611307;

struct GifExtOptions {
    bool ext = true;        // the Ext functions (else Open, ReadHeader, SetParameter, DecodeData: the reference)
    u32 counts = 4;         // CellGifDecExtInParam's outputCounts
    u32 mode = 0;           // the display callback's (imgdec_ext::disp)
    bool display = true;    // a display callback
    bool game_param = true; // the game's own CellGifDecDispParam
    u32 first_image = 0;    // the first part's place (0: the frame, or the part's buffer in mode 1)
    u32 first = 0;          // stream input: the source buffer's bytes (0: the whole file, no stream callback)
    u32 chunk = 0;          // the stream callback's part
    u32 stream_source = 0;  // where it reads (0: the file)
    u32 stop_at = 0;        // the display callback asking CELL_GIFDEC_STOP at this call
};

struct GifExtRun {
    u32 open = 0, header = 0, parameter = 0, stream = 0, stream_calls_at_header = 0;
    u64 out_width_byte = 0, pitch = 0;
    u32 out_height = 0, height = 0;
    std::string info;
    std::vector<std::string> records;          // each DecodeData's answer and CellGifDecDataOutInfo
    std::vector<std::vector<u8>> screens;      // each image record's frame, pitch x height + 64 bytes at kFrame
    std::vector<std::vector<ext::Part>> parts; // each record's display callbacks
    std::vector<std::pair<u32, u32>> stream_calls;
};

// Open (or ExtOpen), the header, RGBA (alpha1 0xFF, alpha2 0x00), then every record to the trailer, on a stream whose
// allocations start where the heap was after Create (so that the extension buffers are where they were last time).
GifExtRun gif_ext_run(u8* b, const Call& call, const ext::Callbacks& cbs, u32 handle, const std::vector<u8>& file,
    const GifExtOptions& e, u32 heap_after_create, u32 mallocs)
{
    GifExtRun o;
    ps_write32(b, kHeapCtl, heap_after_create);
    ps_write32(b, kHeapCtl + 4, mallocs);
    ps_write32(b, kHeapCtl + 8, 0);
    std::memcpy(b + kFile, file.data(), file.size());
    std::memset(b + kSrc, 0, 0x20);
    ps_write32(b, kSrc + 0, 1);
    ps_write32(b, kSrc + 20, kFile);
    ps_write32(b, kSrc + 24, e.first ? e.first : static_cast<u32>(file.size()));
    std::memset(b + kStrmCtl, 0, 0x1000);
    ps_write32(b, kStrmCtl + ext::strm::source, e.stream_source ? e.stream_source : kFile);
    ps_write32(b, kStrmCtl + ext::strm::size, static_cast<u32>(file.size()));
    ps_write32(b, kStrmCtl + ext::strm::offset, e.first);
    ps_write32(b, kStrmCtl + ext::strm::chunk, e.chunk);
    ps_write32(b, kStrmCtl + ext::strm::sample, kDispCtl + ext::disp::count);
    std::memset(b + kDispCtl, 0, 0x40);
    ps_write32(b, kCbStrm, cbs.stream_opd);
    ps_write32(b, kCbStrm + 4, kStrmCtl);
    o.open = e.ext ? call("cellGifDecExtOpen", 0x17FB83C1, {handle, kStreamOut, kSrc, kOpenInfo, e.first ? kCbStrm : 0})
                   : call("cellGifDecOpen", 0x75745079, {handle, kStreamOut, kSrc, kOpenInfo});
    if (o.open) return o;
    o.stream = ps_read32(b, kStreamOut);
    std::memset(b + kInfo, 0xEE, 0x20);
    std::memset(b + kExtInfo, 0xEE, 8);
    o.header = e.ext ? call("cellGifDecExtReadHeader", 0xE53F91F2, {handle, o.stream, kInfo, kExtInfo})
                     : call("cellGifDecReadHeader", 0xF0DA95DE, {handle, o.stream, kInfo});
    o.info = hex(b + kInfo, 0x20);
    o.stream_calls_at_header = ps_read32(b, kStrmCtl + ext::strm::calls);
    if (o.header == 0) {
        std::memset(b + kInParam, 0, 0x10);
        ps_write32(b, kInParam, kCommandWord);
        ps_write32(b, kCommandWord, 0);
        ps_write32(b, kInParam + 4, 10);
        ps_write8(b, kInParam + 8, 0xFF);
        ps_write8(b, kInParam + 9, 0x00);
        ps_write32(b, kExtIn + 0, 1); // CELL_GIFDEC_LINE_MODE
        ps_write32(b, kExtIn + 4, e.counts);
        ps_write32(b, kExtIn + 8, 0);
        std::memset(b + kExtOut, 0xEE, 16);
        o.parameter = e.ext ? call("cellGifDecExtSetParameter", 0x95CAE771, {handle, o.stream, kInParam, kOutParam, kExtIn, kExtOut})
                            : call("cellGifDecSetParameter", 0x41A90DC4, {handle, o.stream, kInParam, kOutParam});
    }
    if (o.header == 0 && o.parameter == 0) {
        o.out_width_byte = ps_read64(b, kExtOut);
        o.out_height = ps_read32(b, kExtOut + 8);
        const u64 row = ps_read64(b, kOutParam);
        o.height = ps_read32(b, kOutParam + 12);
        o.pitch = (row + 16 + 15) & ~u64{15};
        ps_write64(b, kCtrl, o.pitch);
        ps_write32(b, kDispCtl + ext::disp::mode, e.mode);
        ps_write32(b, kDispCtl + ext::disp::frame, kFrame);
        ps_write32(b, kDispCtl + ext::disp::stop_at, e.stop_at);
        ps_write32(b, kDispCtl + ext::disp::command, kCommandWord);
        ps_write32(b, kCbDisp, cbs.display_opd);
        ps_write32(b, kCbDisp + 4, kDispCtl);
        const u32 first_image = e.first_image ? e.first_image : e.mode == 1 ? kPart : kFrame;
        const bool parts = e.ext && e.display && e.counts;
        for (int k = 0; k < 12; k++) {
            std::memset(b + kFrame, 0xCC, o.pitch * o.height + 64);
            std::memset(b + kPart, 0xCC, 0x10000);
            ps_write32(b, kDispParam, first_image);
            const u32 before = ps_read32(b, kDispCtl + ext::disp::count);
            const u32 data = !parts ? kFrame : e.game_param ? kImage : first_image;
            std::memset(b + kDataOut, 0xEE, 0x10);
            const u32 v = e.ext ? call("cellGifDecExtDecodeData", 0x02E7E03E, {handle, o.stream, data, kCtrl, kDataOut,
                                      e.display ? kCbDisp : 0, e.game_param ? kDispParam : 0})
                                : call("cellGifDecDecodeData", 0x44B1BC61, {handle, o.stream, data, kCtrl, kDataOut});
            o.records.push_back(std::format("{:#x} {}", v, hex(b + kDataOut, 0x10)));
            const std::vector<ext::Part> all = ext::parts(b, kDispCtl);
            o.parts.emplace_back(all.begin() + before, all.end());
            if (v != 0) break;
            const u32 type = ps_read32(b, kDataOut);
            if (type == 1) o.screens.emplace_back(b + kFrame, b + kFrame + o.pitch * o.height + 64);
            if (type == 3 || ps_read32(b, kDataOut + 12) == 1) break; // the trailer, or stopped
        }
    }
    o.stream_calls = ext::stream_calls(b, kStrmCtl);
    REQUIRE(call("cellGifDecClose", 0x116A7DA9, {handle, o.stream}) == 0);
    return o;
}

} // namespace

TEST_CASE("cellGifDec: the Ext functions: whole, in parts to a display callback, from a stream callback (D-595)",
    "[hle][gifdec]")
{
    InterpretMisses interpret;
    BigFixture f;
    REQUIRE(f.hle->link_internal_functions({"cellGifDec"}));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Call call = hle_calls(f, t);
    const std::pair<const char*, u32> kExtFunctions[] = {{"cellGifDecExtCreate", 0x4711CB7F},
        {"cellGifDecExtOpen", 0x17FB83C1}, {"cellGifDecExtReadHeader", 0xE53F91F2},
        {"cellGifDecExtSetParameter", 0x95CAE771}, {"cellGifDecExtDecodeData", 0x02E7E03E}};
    for (const auto& [name, nid] : kExtFunctions) {
        CHECK(recomp::nid_of(name) == nid);
        CHECK(hle::registry().is_unverified("cellGifDec", nid));
    }
    for (const u32 nid : {0xB60D42A5u, 0xE74B2CB1u, 0x75745079u, 0x116A7DA9u, 0xF0DA95DEu, 0x41A90DC4u, 0x44B1BC61u})
        CHECK_FALSE(hle::registry().is_unverified("cellGifDec", nid));

    write_callbacks(b);
    const ext::Callbacks cbs = ext::write_callbacks(b, kExtCode, 0x38);
    std::memset(b + kArgs, 0, 0x400);
    const u32 tin[7] = {0, 1000, 200, kMallocOpd, kHeapCtl, kFreeOpd, kHeapCtl};
    for (u32 i = 0; i < 7; i++) ps_write32(b, kThreadIn + 4 * i, tin[i]);
    // CellGifDecExtThreadInParam: SPURS, eight priorities, maxContention; RPCS3's checks.
    const auto ext_in = [&](u32 spurs, u32 priority, u32 contention) {
        ps_write32(b, kExtThreadIn, spurs);
        for (u32 i = 0; i < 8; i++) ps_write8(b, kExtThreadIn + 4 + i, static_cast<u8>(i == 5 ? priority : 3));
        ps_write32(b, kExtThreadIn + 12, contention);
    };
    const auto ext_create = [&](u32 in, u32 out) {
        return call("cellGifDecExtCreate", 0x4711CB7F, {kHandleOut, kThreadIn, kThreadOut, in, out});
    };
    ext_in(0, 15, 7);
    CHECK(ext_create(0, kExtThreadOut) == kArg);
    CHECK(ext_create(kExtThreadIn, 0) == kArg);
    ext_in(0, 15, 0);
    CHECK(ext_create(kExtThreadIn, kExtThreadOut) == kArg); // maxContention 1 to 7
    ext_in(0, 15, 8);
    CHECK(ext_create(kExtThreadIn, kExtThreadOut) == kArg);
    ext_in(0, 16, 7);
    CHECK(ext_create(kExtThreadIn, kExtThreadOut) == kArg); // priorities at most 15
    ext_in(0, 15, 7);
    ps_write32(b, kThreadIn, 2);
    CHECK(ext_create(kExtThreadIn, kExtThreadOut) == kArg); // spuThreadEnable 0 or 1
    ps_write32(b, kThreadIn, 1);
    CHECK(ext_create(kExtThreadIn, kExtThreadOut) == kArg); // SPU threads without SPURS
    ext_in(kData + 0x3000, 15, 7);
    REQUIRE(ext_create(kExtThreadIn, kExtThreadOut) == 0); // with SPURS: Create's memory with SPU threads
    CHECK(ps_read32(b, kHeapCtl + 0x10) == 0x380);
    CHECK(ps_read32(b, kHeapCtl + 0x18) == 0x33880);
    REQUIRE(call("cellGifDecDestroy", 0xE74B2CB1, {ps_read32(b, kHandleOut)}) == 0);
    ps_write32(b, kThreadIn, 0);
    write_callbacks(b);
    std::memset(b + kExtThreadOut, 0xEE, 8);
    REQUIRE(ext_create(kExtThreadIn, kExtThreadOut) == 0);
    CHECK(ps_read32(b, kThreadOut) == 0x240000);
    CHECK(ps_read64(b, kExtThreadOut) == 0xEEEEEEEEEEEEEEEEull); // reserved, left as it is (RPCS3)
    const u32 handle = ps_read32(b, kHandleOut);
    const u32 heap_after_create = ps_read32(b, kHeapCtl), mallocs = ps_read32(b, kHeapCtl + 4);
    CHECK(mallocs == 1);

    const std::vector<gif::File> files = test_files();
    const auto run = [&](usize n, const GifExtOptions& e) {
        return gif_ext_run(b, call, cbs, handle, gif::encode(files[n]), e, heap_after_create, mallocs);
    };
    GifExtOptions plain;
    plain.ext = false;

    SECTION("whole, without a display callback or with no lines per part: DecodeData's records")
    {
        for (const usize n : {usize{0}, usize{2}, usize{3}, usize{7}, usize{13}, usize{14}}) {
            const GifExtRun want = run(n, plain);
            REQUIRE(want.records.size() > 1);
            for (const bool display : {false, true}) {
                GifExtOptions e;
                e.display = display;
                e.counts = display ? 0 : 4;
                const GifExtRun o = run(n, e);
                INFO("file " << n << ", display callback " << display);
                CHECK(o.header == 0);
                CHECK(o.info == want.info);
                CHECK(o.parameter == 0);
                CHECK(o.out_width_byte == files[n].width * 4);
                CHECK(o.out_height == (display ? files[n].height : std::min(4u, files[n].height)));
                CHECK(o.records == want.records);
                CHECK(o.screens == want.screens);
                for (const auto& p : o.parts) CHECK(p.empty());
            }
        }
    }

    SECTION("in parts: each image's screen, outputCounts lines to each display callback; extensions have none")
    {
        // File 7: an application, a comment and a graphic control extension, an image, another extension and image.
        const GifExtRun want = run(7, plain);
        for (const bool game_param : {true, false}) {
            GifExtOptions e;
            e.game_param = game_param;
            const GifExtRun o = run(7, e);
            INFO("the game's dispParam " << game_param);
            CHECK(o.records == want.records);
            CHECK(o.screens == want.screens);
            REQUIRE(o.records.size() == 7);
            for (const usize k : {usize{0}, usize{1}, usize{2}, usize{4}, usize{6}}) CHECK(o.parts[k].empty());
            for (const usize k : {usize{3}, usize{5}}) {
                REQUIRE(o.parts[k].size() == 3); // 10 lines: 4, 4, 2
                const u32 starts[3] = {0, 4, 8}, heights[3] = {4, 4, 2};
                for (u32 i = 0; i < 3; i++) {
                    const ext::Part& p = o.parts[k][i];
                    CHECK(p.frame_width_byte == o.pitch);
                    CHECK(p.frame_height == 10);
                    CHECK(p.start_x_byte == 0);
                    CHECK(p.start_y == starts[i]);
                    CHECK(p.width_byte == 48);
                    CHECK(p.height == heights[i]);
                    CHECK(p.bit_depth == 8);
                    CHECK(p.components == 4);
                    CHECK(p.word_34 == 0);                            // scanPassCount
                    CHECK(p.word_38 == kFrame + starts[i] * o.pitch); // outputImage
                    CHECK(p.word_3c == 0);                            // interlaceFlag
                    CHECK(p.param == (game_param ? kDispParam : o.stream + 0x50));
                }
            }
        }
    }

    SECTION("in parts into one buffer the callback copies from: an interlaced image, its rows in order")
    {
        const GifExtRun want = run(2, plain);
        GifExtOptions e;
        e.mode = 1;
        e.counts = 5;
        const GifExtRun o = run(2, e);
        CHECK(o.records == want.records);
        CHECK(o.screens == want.screens);
        REQUIRE(o.parts.size() == 2);
        REQUIRE(o.parts[0].size() == 4); // 17 lines: 5, 5, 5, 2
        for (u32 i = 0; i < 4; i++) {
            CHECK(o.parts[0][i].start_y == 5 * i);
            CHECK(o.parts[0][i].height == (i < 3 ? 5u : 2u));
            CHECK(o.parts[0][i].word_38 == kPart);
        }
    }

    SECTION("from a stream: each record's bytes from the stream callback as DecodeData reads it")
    {
        const std::vector<u8> file = gif::encode(files[7]);
        const GifExtRun want = run(7, plain);
        GifExtOptions e;
        e.first = 10; // less than the screen descriptor
        e.chunk = 23;
        const GifExtRun o = run(7, e);
        CHECK(o.header == 0);
        CHECK(o.info == want.info);
        CHECK(o.stream_calls_at_header > 0);
        CHECK(o.records == want.records);
        CHECK(o.screens == want.screens);
        REQUIRE(o.stream_calls.size() == (file.size() - 10 + 22) / 23); // to the trailer, none past it
        for (usize i = 0; i < o.stream_calls.size(); i++) CHECK(o.stream_calls[i].first == 10 + 23 * i);
        // The second image's bytes were asked for after the first image's three parts went out.
        CHECK(o.stream_calls.front().second == 0);
        bool between = false;
        for (usize i = 0; i < o.stream_calls.size(); i++) {
            between |= o.stream_calls[i].second == 3;
            if (i) CHECK(o.stream_calls[i].second >= o.stream_calls[i - 1].second);
        }
        CHECK(between);
    }

    SECTION("CELL_GIFDEC_STOP at commandPtr from a display callback stops the image's parts")
    {
        GifExtOptions e;
        e.stop_at = 2;
        const GifExtRun o = run(0, e); // 13x9: parts of 4, 4, 1
        REQUIRE(o.records.size() == 1);
        CHECK(o.records[0].starts_with("0x0 00000001")); // the image record
        CHECK(o.records[0].ends_with("00000001"));      // CELL_GIFDEC_DEC_STATUS_STOP
        CHECK(o.parts[0].size() == 2);
    }

    SECTION("what the Ext functions refuse")
    {
        const std::vector<u8> file = gif::encode(files[0]);
        const auto open_ext = [&](u32 cb) {
            ps_write32(b, kHeapCtl, heap_after_create);
            ps_write32(b, kHeapCtl + 4, mallocs);
            std::memcpy(b + kFile, file.data(), file.size());
            std::memset(b + kSrc, 0, 0x20);
            ps_write32(b, kSrc + 0, 1);
            ps_write32(b, kSrc + 20, kFile);
            ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
            REQUIRE(call("cellGifDecExtOpen", 0x17FB83C1, {handle, kStreamOut, kSrc, kOpenInfo, cb}) == 0);
            return ps_read32(b, kStreamOut);
        };
        // No CellGifDecExtInfo: ARG, and the stream fails.
        u32 stream = open_ext(0);
        CHECK(call("cellGifDecExtReadHeader", 0xE53F91F2, {handle, stream, kInfo, 0}) == kArg);
        CHECK(call("cellGifDecReadHeader", 0xF0DA95DE, {handle, stream, kInfo}) == kSeq);
        REQUIRE(call("cellGifDecClose", 0x116A7DA9, {handle, stream}) == 0);
        // No extension parameters, or a buffer mode that is not CELL_GIFDEC_LINE_MODE: ARG, and the stream fails.
        for (const int c : {0, 1, 2}) {
            stream = open_ext(0);
            REQUIRE(call("cellGifDecExtReadHeader", 0xE53F91F2, {handle, stream, kInfo, kExtInfo}) == 0);
            std::memset(b + kInParam, 0, 0x10);
            ps_write32(b, kInParam + 4, 10);
            ps_write32(b, kExtIn + 0, c == 2 ? 0 : 1);
            ps_write32(b, kExtIn + 4, 4);
            CHECK(call("cellGifDecExtSetParameter", 0x95CAE771,
                      {handle, stream, kInParam, kOutParam, c == 0 ? 0 : kExtIn, c == 1 ? 0 : kExtOut}) == kArg);
            CHECK(call("cellGifDecSetParameter", 0x41A90DC4, {handle, stream, kInParam, kOutParam}) == kSeq);
            REQUIRE(call("cellGifDecClose", 0x116A7DA9, {handle, stream}) == 0);
        }
        // A stream callback with a file source: STREAM_FORMAT, the handle still free.
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 4, kSrc + 0x100);
        std::memcpy(b + kSrc + 0x100, "/app_home/image.gif", 20);
        ps_write32(b, kCbStrm, cbs.stream_opd);
        ps_write32(b, kCbStrm + 4, kStrmCtl);
        CHECK(call("cellGifDecExtOpen", 0x17FB83C1, {handle, kStreamOut, kSrc, kOpenInfo, kCbStrm}) == kStreamFormat);
        REQUIRE(call("cellGifDecClose", 0x116A7DA9, {handle, open_ext(0)}) == 0);
        {
            GifExtOptions e; // the stream callback's bytes are not guest memory
            e.first = 10;
            e.chunk = 23;
            e.stream_source = kNowhere;
            CHECK(run(0, e).header == kCbParam);
        }
        {
            GifExtOptions e; // the display callback gives no place for the next part
            e.mode = 2;
            const GifExtRun o = run(0, e);
            REQUIRE(o.records.size() == 1);
            CHECK(o.records[0].starts_with(std::format("{:#x}", kCbParam)));
            CHECK(o.parts[0].size() == 1);
        }
        {
            GifExtOptions e; // the first part's place is not guest memory
            e.first_image = kNowhere;
            const GifExtRun o = run(0, e);
            REQUIRE(o.records.size() == 1);
            CHECK(o.records[0].starts_with(std::format("{:#x}", kArg)));
            CHECK(o.parts[0].empty());
        }
    }
    REQUIRE(call("cellGifDecDestroy", 0xE74B2CB1, {handle}) == 0);
}

