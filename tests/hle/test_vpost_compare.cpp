// cellVpost against Sony's libvpost (D-564, D-597, D-913): the same configurations, resources and pictures go to
// pspocket's library and to Sony's module. Compared: QueryAttr's answers and attributes; Open's, OpenEx's and Close's
// answers and the handle (as an offset into the work memory); for each Exec its answer, the picture information and the
// whole output picture, exactly (D-913: libvpost's colour conversion and scaler are pspocket's), with the bytes past it.
// The pictures are what libvpost takes: sizes even, from 32x32, windows inside their pictures, buffers 128-byte aligned.
// No game data; skipped without the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <array>
#include <cstring>
#include <format>
#include <string>
#include <tuple>
#include <vector>

#include "compare_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kCtrl = kData + 0x200, kInfo = kData + 0x280, kIn = kData + 0x1000, kOut = kData + 0x40000;
constexpr u32 kWork = 0x01000000, kWorkSize = 0x01000000; // the work memory Open gets (16 MB at most)
constexpr u32 kMax = 192;                                 // the handles' largest pictures
constexpr s32 kRgba = 0, kYuv = 1, kFull = 0, kBroadcast = 1, kBt601 = 0, kBt709 = 1;
constexpr u8 kFill = 0xEE;

struct VpostFixture : TlsFixture {
    VpostFixture()
    {
        REQUIRE(hle->main_memory().reserve(kWork, kWorkSize));
        REQUIRE(mem.commit(kWork, kWorkSize));
    }
};

struct Picture {
    u32 w = 0, h = 0;
    std::vector<u8> bytes; // Y, then U, then V (each plane its width's stride)
};

template <class Y, class U, class V>
Picture make(u32 w, u32 h, Y y, U u, V v)
{
    Picture p{w, h, {}};
    for (u32 r = 0; r < h; r++)
        for (u32 x = 0; x < w; x++) p.bytes.push_back(static_cast<u8>(y(x, r)));
    for (u32 r = 0; r < h / 2; r++)
        for (u32 x = 0; x < w / 2; x++) p.bytes.push_back(static_cast<u8>(u(x, r)));
    for (u32 r = 0; r < h / 2; r++)
        for (u32 x = 0; x < w / 2; x++) p.bytes.push_back(static_cast<u8>(v(x, r)));
    return p;
}

Picture flat(u32 w, u32 h, u8 y, u8 u, u8 v)
{
    return make(w, h, [=](u32, u32) { return y; }, [=](u32, u32) { return u; }, [=](u32, u32) { return v; });
}

// Smooth ramps in all three planes (luma 16-235, chroma about 128).
Picture gradient(u32 w, u32 h)
{
    return make(
        w, h, [=](u32 x, u32 r) { return 16 + x * 200 / w + r * 19 / h; },
        [=](u32 x, u32 r) { return 128 + static_cast<int>(x * 2) - static_cast<int>(w / 2) + static_cast<int>(r) / 4; },
        [=](u32 x, u32 r) { return 128 + static_cast<int>(r * 2) - static_cast<int>(h / 2) - static_cast<int>(x) / 4; });
}

// One bright luma sample (and one chroma sample) on grey: the scaler's taps, read off the output.
Picture impulse(u32 w, u32 h)
{
    return make(
        w, h, [=](u32 x, u32 r) { return x == w / 2 && r == h / 2 ? 235 : 16; },
        [=](u32 x, u32 r) { return x == w / 4 && r == h / 4 ? 240 : 128; }, [=](u32, u32) { return 128; });
}

// Vertical and horizontal edges.
Picture edges(u32 w, u32 h)
{
    return make(
        w, h, [=](u32 x, u32 r) { return (x < w / 2) != (r < h / 2) ? 40 : 200; },
        [=](u32 x, u32) { return x < w / 4 ? 90 : 170; }, [=](u32, u32 r) { return r < h / 4 ? 200 : 60; });
}

struct Ctrl {
    s32 exec_type = 0, scaler = 0, ipc = 0;
    u32 in_w = 0, in_h = 0;
    s32 chroma = 0, range = kFull, matrix = kBt601;
    std::array<u32, 4> in_window{}, out_window{};
    u32 out_w = 0, out_h = 0;
    u8 alpha = 0xFF;
    u64 user = 0x1122334455667788ull;
};

struct Run : Half<VpostFixture> {
    using Half::Half;

    void write_cfg(u32 in_w, u32 in_h, u32 out_w, u32 out_h, s32 out_fmt, s32 in_fmt = 0, s32 in_depth = 0,
        s32 out_depth = 0, u32 reserved = 0)
    {
        const u32 v[10] = {in_w, in_h, static_cast<u32>(in_depth), static_cast<u32>(in_fmt), out_w, out_h,
            static_cast<u32>(out_depth), static_cast<u32>(out_fmt), reserved, reserved};
        for (u32 i = 0; i < 10; i++) ps_write32(b, kCfg + 4 * i, v[i]);
    }

    // QueryAttr's answer and attribute (filled first); the memory size it asks for, 0 when it fails.
    u32 query(const std::string& what)
    {
        std::memset(b + kAttr, kFill, 16);
        const u64 r = Half::r("cellVpostQueryAttr", {kCfg, kAttr}, "QueryAttr " + what);
        o.line("QueryAttr " + what + ": the attribute", hex(b + kAttr, 16));
        return r == 0 ? ps_read32(b, kAttr) : 0;
    }

    // CellVpostResource {memAddr, memSize, ppuThreadPriority, ppuThreadStackSize, spuThreadPriority, numOfSpus}.
    void write_resource(u32 mem, u32 size, s32 ppu_prio = 1000, u32 stack = 0x4000, s32 spu_prio = 100, u32 spus = 1)
    {
        const u32 v[6] = {mem, size, static_cast<u32>(ppu_prio), stack, static_cast<u32>(spu_prio), spus};
        for (u32 i = 0; i < 6; i++) ps_write32(b, kRsrc + 4 * i, v[i]);
    }

    // The handle Open wrote, as an offset into the work memory when it lies there.
    std::string handle_text(u32 h) const
    {
        if (h >= kWork && h < kWork + kWorkSize) return std::format("the work memory +{:#x}", h - kWork);
        return std::format("{:#x}", h);
    }

    // Open with a good resource (the memory QueryAttr asks for); the handle, or 0.
    u32 open(const std::string& what, u32 in_w, u32 in_h, u32 out_w, u32 out_h, s32 fmt)
    {
        write_cfg(in_w, in_h, out_w, out_h, fmt);
        const u32 size = query(what);
        write_resource(kWork, std::min(size ? size : 0x400000u, kWorkSize));
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = Half::r("cellVpostOpen", {kCfg, kRsrc, kHandle}, "Open " + what);
        const u32 h = ps_read32(b, kHandle);
        o.line("Open " + what + ": the handle", handle_text(h));
        return r == 0 ? h : 0;
    }

    void write_ctrl(const Ctrl& c)
    {
        std::memset(b + kCtrl, 0, 96);
        ps_write32(b, kCtrl, static_cast<u32>(c.exec_type));
        ps_write32(b, kCtrl + 4, static_cast<u32>(c.scaler));
        ps_write32(b, kCtrl + 8, static_cast<u32>(c.ipc));
        ps_write32(b, kCtrl + 12, c.in_w);
        ps_write32(b, kCtrl + 16, c.in_h);
        ps_write32(b, kCtrl + 20, static_cast<u32>(c.chroma));
        ps_write32(b, kCtrl + 24, static_cast<u32>(c.range));
        ps_write32(b, kCtrl + 28, static_cast<u32>(c.matrix));
        for (u32 i = 0; i < 4; i++) ps_write32(b, kCtrl + 32 + 4 * i, c.in_window[i]);
        ps_write32(b, kCtrl + 48, c.out_w);
        ps_write32(b, kCtrl + 52, c.out_h);
        for (u32 i = 0; i < 4; i++) ps_write32(b, kCtrl + 56 + 4 * i, c.out_window[i]);
        ps_write8(b, kCtrl + 72, c.alpha);
        ps_write64(b, kCtrl + 80, c.user);
    }

    // Exec of `p` with `c` on handle `h` (an RGBA handle when `rgba`): its answer, the picture information (and 8 bytes
    // past it), the output under the rule for its size (the file's header), the bytes past it, and with an output
    // window the pixels inside it under that rule and those outside it exactly.
    void exec(const std::string& what, u32 h, bool rgba, const Picture& p, Ctrl c)
    {
        c.in_w = p.w;
        c.in_h = p.h;
        if (!c.in_window[2]) c.in_window = {0, 0, p.w, p.h};
        if (!c.out_window[2]) c.out_window = {0, 0, c.out_w, c.out_h};
        std::memcpy(b + kIn, p.bytes.data(), p.bytes.size());
        write_ctrl(c);
        const u32 out_bytes = rgba ? c.out_w * c.out_h * 4 : c.out_w * c.out_h + 2 * ((c.out_w / 2) * (c.out_h / 2));
        std::memset(b + kOut, kFill, out_bytes + 256);
        std::memset(b + kInfo, kFill, 96);
        const u64 r = Half::r("cellVpostExec", {h, kIn, kCtrl, kOut, kInfo}, "Exec " + what);
        o.line("Exec " + what + ": the picture information", hex(b + kInfo, 96));
        o.line("Exec " + what + ": the 256 bytes past the output", hex(b + kOut + out_bytes, 256));
        if (r != 0) return;
        o.bytes("Exec " + what + ": the output", b + kOut, out_bytes);
    }
};

// Random bytes, the same in both halves.
Picture noise(u32 w, u32 h, u32 seed)
{
    u32 v = seed * 2654435761u + 7;
    const auto next = [&v](u32, u32) {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
        return v >> 24;
    };
    return make(w, h, next, next, next);
}

Observed half(const Setup& s, bool sony_half)
{
    Run h(s, sony_half, "cellVpost");
    u8* b = h.b;

    // QueryAttr: its checks, then the sizes and formats it is asked about.
    h.write_cfg(1280, 720, 1280, 720, kRgba);
    h.r("cellVpostQueryAttr", {0, kAttr}, "QueryAttr(NULL, attr)");
    h.r("cellVpostQueryAttr", {kCfg, 0}, "QueryAttr(cfg, NULL)");
    struct Query {
        u32 in_w, in_h, out_w, out_h;
        s32 out_fmt, in_fmt = 0, in_depth = 0, out_depth = 0;
        u32 reserved = 0;
        const char* what;
    };
    const Query queries[] = {
        {1920, 1080, 1920, 1080, kRgba, 0, 0, 0, 0, "1920x1080 RGBA"},
        {1920, 1088, 1920, 1088, kYuv, 0, 0, 0, 0, "1920x1088 YUV"},
        {1280, 720, 1280, 720, kRgba, 0, 0, 0, 0, "1280x720 RGBA"},
        {720, 480, 1280, 720, kYuv, 0, 0, 0, 0, "720x480 to 1280x720 YUV"},
        {1280, 720, 640, 360, kRgba, 0, 0, 0, 0, "1280x720 to 640x360 RGBA"},
        {128, 128, 128, 128, kRgba, 0, 0, 0, 0, "128x128 RGBA"},
        {32, 32, 32, 32, kRgba, 0, 0, 0, 0, "32x32 RGBA"},
        {34, 32, 1922 - 2, 32, kRgba, 0, 0, 0, 0, "34x32 to 1920x32 RGBA"},
        {30, 32, 64, 64, kRgba, 0, 0, 0, 0, "input 30 wide"},
        {64, 30, 64, 64, kRgba, 0, 0, 0, 0, "input 30 high"},
        {33, 32, 64, 64, kRgba, 0, 0, 0, 0, "input 33 wide"},
        {64, 33, 64, 64, kRgba, 0, 0, 0, 0, "input 33 high"},
        {1922, 32, 64, 64, kRgba, 0, 0, 0, 0, "input 1922 wide"},
        {64, 1090, 64, 64, kRgba, 0, 0, 0, 0, "input 1090 high"},
        {64, 64, 1922, 64, kRgba, 0, 0, 0, 0, "output 1922 wide"},
        {64, 64, 64, 1090, kRgba, 0, 0, 0, 0, "output 1090 high"},
        {64, 64, 63, 64, kRgba, 0, 0, 0, 0, "output 63 wide"},
        {16, 16, 16, 16, kRgba, 0, 0, 0, 0, "16x16 RGBA"},
        {1280, 720, 1280, 720, 2, 0, 0, 0, 0, "output format 2"},
        {1280, 720, 1280, 720, kRgba, 1, 0, 0, 0, "input format 1"},
        {1280, 720, 1280, 720, kRgba, 0, 1, 0, 0, "input depth 1"},
        {1280, 720, 1280, 720, kRgba, 0, 0, 1, 0, "output depth 1"},
        {0, 720, 1280, 720, kRgba, 0, 0, 0, 0, "input width 0"},
        {1280, 720, 1280, 0, kRgba, 0, 0, 0, 0, "output height 0"},
        {1280, 720, 1280, 720, kRgba, 0, 0, 0, 7, "reserved words set"},
    };
    for (const Query& q : queries) {
        h.write_cfg(q.in_w, q.in_h, q.out_w, q.out_h, q.out_fmt, q.in_fmt, q.in_depth, q.out_depth, q.reserved);
        h.query(q.what);
    }

    // Open's checks, each resource field at its limits, then an RGBA handle and a YUV one.
    h.write_cfg(kMax, kMax, kMax, kMax, kRgba);
    const u32 size = h.query("for Open's checks");
    const u32 mem = std::min(size ? size : 0x400000u, kWorkSize / 4);
    const auto open_try = [&](const std::string& what, u32 cfg, u32 rsrc, u32 handle) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        const u64 r = h.r("cellVpostOpen", {cfg, rsrc, handle}, "Open " + what);
        h.o.line("Open " + what + ": the handle word", h.handle_text(ps_read32(b, kHandle)));
        if (r == 0) h.r("cellVpostClose", {ps_read32(b, kHandle)}, "Close after Open " + what);
    };
    h.write_resource(kWork, mem);
    open_try("without a configuration", 0, kRsrc, kHandle);
    open_try("without a resource", kCfg, 0, kHandle);
    open_try("without a handle pointer", kCfg, kRsrc, 0);
    h.write_cfg(kMax, kMax, kMax, kMax, 7);
    open_try("with output format 7", kCfg, kRsrc, kHandle);
    h.write_cfg(kMax, kMax, kMax, kMax, kRgba);
    h.write_resource(0, mem);
    open_try("without work memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem - 1);
    open_try("with a byte less memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork + 0x10, mem);
    open_try("at work memory +0x10", kCfg, kRsrc, kHandle);
    h.write_resource(kWork + 0x80, mem);
    open_try("at work memory +0x80", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 0xBFF);
    open_try("with PPU priority 0xBFF", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 0xC00);
    open_try("with PPU priority 0xC00", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, -1);
    open_try("with PPU priority -1", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0);
    open_try("with no PPU stack", kCfg, kRsrc, kHandle);
    for (const s32 prio : {15, 16, 255, 256}) {
        h.write_resource(kWork, mem, 1000, 0x4000, prio);
        open_try(std::format("with SPU priority {}", prio), kCfg, kRsrc, kHandle);
    }
    for (const u32 spus : {0u, 2u, 7u}) {
        h.write_resource(kWork, mem, 1000, 0x4000, 100, spus);
        open_try(std::format("with {} SPUs", spus), kCfg, kRsrc, kHandle);
    }
    // OpenEx's checks: CellVpostResourceEx {memAddr, memSize, spurs, priority[8], maxContention}. A good one needs a
    // SPURS instance (pspocket's converts on the caller's thread): the refusals only.
    const auto open_ex = [&](const std::string& what, u32 spurs, std::array<u8, 8> prio, u32 contention, u32 rsrc = kRsrc,
                             u32 handle = kHandle) {
        std::memset(b + kRsrc, 0, 24);
        ps_write32(b, kRsrc, kWork);
        ps_write32(b, kRsrc + 4, mem);
        ps_write32(b, kRsrc + 8, spurs);
        for (u32 i = 0; i < 8; i++) ps_write8(b, kRsrc + 12 + i, prio[i]);
        ps_write32(b, kRsrc + 20, contention);
        ps_write32(b, kHandle, 0xEEEEEEEE);
        h.r("cellVpostOpenEx", {kCfg, rsrc, handle}, "OpenEx " + what);
        h.o.line("OpenEx " + what + ": the handle word", h.handle_text(ps_read32(b, kHandle)));
    };
    h.r("cellVpostOpenEx", {0, kRsrc, kHandle}, "OpenEx without a configuration");
    open_ex("without a resource", 0x100, {1, 1, 1, 1, 1, 1, 1, 1}, 1, 0);
    open_ex("without a SPURS instance", 0, {1, 1, 1, 1, 1, 1, 1, 1}, 1);
    open_ex("with every priority 0", 0x100, {0, 0, 0, 0, 0, 0, 0, 0}, 1);
    open_ex("with a priority of 16", 0x100, {1, 16, 1, 1, 1, 1, 1, 1}, 1);
    open_ex("with a maximum contention of 2", 0x100, {1, 1, 1, 1, 1, 1, 1, 1}, 2);
    open_ex("with a maximum contention of 0", 0x100, {1, 0, 0, 0, 0, 0, 0, 0}, 0);

    const u32 rgba = h.open("RGBA up to 192x192", kMax, kMax, kMax, kMax, kRgba);
    if (!rgba) return std::move(h.o);
    h.r("cellVpostClose", {0}, "Close(NULL)");
    h.r("cellVpostClose", {rgba + 0x80}, "Close of a pointer into the work memory that is no handle");

    // Same size: flat colours in both matrices and ranges, then noise with each chroma siting and the alpha.
    struct Colour {
        s32 matrix, range;
        u8 y, u, v;
    };
    const Colour colours[] = {{kBt601, kBroadcast, 16, 128, 128}, {kBt601, kBroadcast, 235, 128, 128},
        {kBt601, kBroadcast, 81, 90, 240}, {kBt601, kBroadcast, 145, 54, 34}, {kBt601, kBroadcast, 41, 240, 110},
        {kBt709, kBroadcast, 63, 102, 240}, {kBt709, kBroadcast, 173, 42, 26}, {kBt709, kBroadcast, 32, 240, 118},
        {kBt601, kFull, 0, 128, 128}, {kBt601, kFull, 255, 128, 128}, {kBt601, kFull, 76, 85, 255},
        {kBt601, kFull, 150, 44, 21}, {kBt601, kFull, 29, 255, 107}, {kBt709, kFull, 54, 99, 255},
        {kBt709, kFull, 182, 30, 12}, {kBt709, kFull, 18, 255, 116}, {kBt709, kFull, 0, 0, 0},
        {kBt709, kBroadcast, 255, 255, 255}};
    for (const Colour& c : colours) {
        Ctrl ctrl;
        ctrl.out_w = ctrl.out_h = 32;
        ctrl.matrix = c.matrix;
        ctrl.range = c.range;
        ctrl.alpha = 0x5A;
        h.exec(std::format("flat YCbCr {} {} {} (matrix {}, range {})", c.y, c.u, c.v, c.matrix, c.range), rgba, true,
            flat(32, 32, c.y, c.u, c.v), ctrl);
    }
    u32 seed = 1;
    for (const s32 matrix : {kBt601, kBt709})
        for (const s32 range : {kFull, kBroadcast})
            for (const s32 chroma : {0, 1}) {
                Ctrl ctrl;
                ctrl.out_w = 64;
                ctrl.out_h = 48;
                ctrl.matrix = matrix;
                ctrl.range = range;
                ctrl.chroma = chroma;
                ctrl.alpha = static_cast<u8>(0x10 * (1 + matrix + 2 * range + 4 * chroma));
                h.exec(std::format("64x48 noise (matrix {}, range {}, chroma type {})", matrix, range, chroma), rgba,
                    true, noise(64, 48, seed++), ctrl);
                h.exec(std::format("64x48 ramps (matrix {}, range {}, chroma type {})", matrix, range, chroma), rgba,
                    true, gradient(64, 48), ctrl);
            }

    // Scaled: ramps, edges, impulses and noise enlarged and reduced, each scaler type.
    struct Scale {
        u32 in_w, in_h, out_w, out_h;
        int kind; // 0 ramps, 1 edges, 2 impulse, 3 noise
    };
    const Scale scales[] = {{64, 48, 96, 72, 0}, {64, 48, 32, 32, 0}, {64, 48, 128, 96, 3}, {64, 48, 48, 48, 3},
        {64, 48, 96, 72, 1}, {64, 64, 40, 34, 1}, {32, 32, 64, 64, 2}, {32, 32, 48, 48, 2}, {64, 64, 32, 32, 2},
        {32, 32, 96, 34, 2}, {96, 64, 40, 64, 3}, {64, 48, 72, 40, 3}, {34, 34, 190, 186, 3}};
    for (const Scale& sc : scales)
        for (const s32 scaler : {0, 1, 2, 3}) {
            Ctrl ctrl;
            ctrl.out_w = sc.out_w;
            ctrl.out_h = sc.out_h;
            ctrl.scaler = scaler;
            ctrl.chroma = (sc.in_w + scaler) & 1;
            const Picture p = sc.kind == 0 ? gradient(sc.in_w, sc.in_h)
                : sc.kind == 1             ? edges(sc.in_w, sc.in_h)
                : sc.kind == 2             ? impulse(sc.in_w, sc.in_h)
                                           : noise(sc.in_w, sc.in_h, seed++);
            const char* kind = sc.kind == 0 ? "ramps" : sc.kind == 1 ? "edges" : sc.kind == 2 ? "impulse" : "noise";
            h.exec(std::format("{} {}x{} to {}x{}, scaler type {}", kind, sc.in_w, sc.in_h, sc.out_w, sc.out_h, scaler),
                rgba, true, p, ctrl);
        }

    // Windows: part of the input to part of the output, black around it.
    struct Win {
        std::array<u32, 4> in, out;
        u32 out_w, out_h;
    };
    const Win wins[] = {{{8, 8, 32, 32}, {16, 8, 32, 32}, 64, 48}, {{8, 8, 32, 32}, {0, 0, 64, 48}, 64, 48},
        {{0, 0, 64, 48}, {32, 16, 96, 64}, 160, 96}, {{16, 2, 48, 40}, {64, 4, 64, 56}, 160, 96},
        {{30, 14, 34, 34}, {2, 6, 98, 34}, 128, 64}, {{0, 0, 64, 48}, {0, 0, 32, 32}, 64, 48}};
    for (const Win& w : wins)
        for (const s32 chroma : {0, 1}) {
            Ctrl ctrl;
            ctrl.out_w = w.out_w;
            ctrl.out_h = w.out_h;
            ctrl.in_window = w.in;
            ctrl.out_window = w.out;
            ctrl.chroma = chroma;
            ctrl.range = chroma;
            h.exec(std::format("64x48 noise, window ({}, {}) {}x{} to ({}, {}) {}x{} of {}x{}, chroma type {}", w.in[0],
                       w.in[1], w.in[2], w.in[3], w.out[0], w.out[1], w.out[2], w.out[3], w.out_w, w.out_h, chroma),
                rgba, true, noise(64, 48, seed++), ctrl);
        }

    // The exec types (fields and frames) and interpolation types, at the same size and scaled.
    for (s32 type = 0; type < 8; type++)
        for (const s32 ipc : {0, 1, 2}) {
            Ctrl ctrl;
            ctrl.exec_type = type;
            ctrl.ipc = ipc;
            ctrl.out_w = 64;
            ctrl.out_h = 48;
            h.exec(std::format("64x48 noise, exec type {}, interpolation type {}", type, ipc), rgba, true,
                noise(64, 48, seed++), ctrl);
            ctrl.out_w = 96;
            ctrl.out_h = 64;
            h.exec(std::format("64x48 noise to 96x64, exec type {}, interpolation type {}", type, ipc), rgba, true,
                noise(64, 48, seed++), ctrl);
        }

    // Exec's checks.
    {
        Ctrl ctrl;
        ctrl.in_w = ctrl.in_h = ctrl.out_w = ctrl.out_h = 32;
        const Picture p = gradient(32, 32);
        std::memcpy(b + kIn, p.bytes.data(), p.bytes.size());
        const auto put = [&](const Ctrl& c) {
            h.write_ctrl(c);
            for (u32 i = 0; i < 4; i++) {
                if (!c.in_window[2]) ps_write32(b, kCtrl + 32 + 4 * i, i < 2 ? 0 : i == 2 ? c.in_w : c.in_h);
                if (!c.out_window[2]) ps_write32(b, kCtrl + 56 + 4 * i, i < 2 ? 0 : i == 2 ? c.out_w : c.out_h);
            }
        };
        put(ctrl);
        h.r("cellVpostExec", {0, kIn, kCtrl, kOut, kInfo}, "Exec(NULL handle)");
        h.r("cellVpostExec", {rgba + 0x80, kIn, kCtrl, kOut, kInfo}, "Exec of a pointer that is no handle");
        h.r("cellVpostExec", {rgba, 0, kCtrl, kOut, kInfo}, "Exec without a picture");
        h.r("cellVpostExec", {rgba, kIn + 0x40, kCtrl, kOut, kInfo}, "Exec of a picture off 128 bytes");
        h.r("cellVpostExec", {rgba, kIn, 0, kOut, kInfo}, "Exec without control parameters");
        h.r("cellVpostExec", {rgba, kIn, kCtrl, 0, kInfo}, "Exec without an output");
        h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut + 0x10, kInfo}, "Exec to an output off 128 bytes");
        h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, 0}, "Exec without picture information");
        const auto refused = [&](const std::string& what, const Ctrl& c) {
            put(c);
            h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, kInfo}, "Exec " + what);
        };
        refused("of a good one", ctrl);
        Ctrl c = ctrl;
        c.in_w = 256;
        refused("with an input wider than the handle's", c);
        c = ctrl;
        c.out_h = 256;
        refused("with an output taller than the handle's", c);
        c = ctrl;
        c.in_w = 30;
        refused("with an input 30 wide", c);
        c = ctrl;
        c.out_w = 34;
        c.out_h = 33;
        refused("with an output 33 high", c);
        c = ctrl;
        c.exec_type = 8;
        refused("with exec type 8", c);
        c = ctrl;
        c.scaler = 4;
        refused("with scaler type 4", c);
        c = ctrl;
        c.ipc = 3;
        refused("with interpolation type 3", c);
        c = ctrl;
        c.matrix = 2;
        refused("with colour matrix 2", c);
        c = ctrl;
        c.range = 2;
        refused("with quantisation range 2", c);
        c = ctrl;
        c.chroma = 2;
        refused("with chroma position type 2", c);
        c = ctrl;
        c.in_window = {0, 0, 30, 32};
        refused("with an input window 30 wide", c);
        c = ctrl;
        c.in_window = {2, 0, 32, 32};
        refused("with an input window past the picture", c);
        c = ctrl;
        c.in_w = c.in_h = 64;
        c.in_window = {1, 0, 32, 32};
        refused("with an input window at an odd column", c);
        c = ctrl;
        c.out_w = c.out_h = 64;
        c.out_window = {32, 32, 32, 32};
        refused("with an output window in the corner", c);
        c = ctrl;
        c.out_w = c.out_h = 64;
        c.out_window = {34, 0, 32, 32};
        refused("with an output window past the picture", c);
        put(ctrl);
        ps_write32(b, kCtrl + 88, 1);
        h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, kInfo}, "Exec with reserved1 set");
        put(ctrl);
        ps_write32(b, kCtrl + 92, 1);
        h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, kInfo}, "Exec with reserved2 set");
    }
    h.r("cellVpostClose", {rgba}, "Close of the RGBA handle");
    h.r("cellVpostClose", {rgba}, "Close of it again");
    h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, kInfo}, "Exec on the closed handle");

    // YUV out: copies, scaling and windows in both chroma sitings.
    const u32 yuv = h.open("YUV up to 192x192", kMax, kMax, kMax, kMax, kYuv);
    if (!yuv) return std::move(h.o);
    for (const s32 chroma : {0, 1}) {
        Ctrl ctrl;
        ctrl.chroma = chroma;
        ctrl.range = chroma;
        ctrl.out_w = 48;
        ctrl.out_h = 32;
        h.exec(std::format("YUV 48x32 copied, chroma type {}", chroma), yuv, false, noise(48, 32, seed++), ctrl);
        for (const auto& [w, hh, ow, oh] : {std::array<u32, 4>{64, 48, 96, 72}, std::array<u32, 4>{64, 48, 32, 32},
                 std::array<u32, 4>{32, 32, 64, 64}, std::array<u32, 4>{96, 64, 40, 34}, std::array<u32, 4>{48, 48, 190, 50}}) {
            ctrl.out_w = ow;
            ctrl.out_h = oh;
            h.exec(std::format("YUV noise {}x{} to {}x{}, chroma type {}", w, hh, ow, oh, chroma), yuv, false,
                noise(w, hh, seed++), ctrl);
        }
        ctrl.out_w = 128;
        ctrl.out_h = 96;
        ctrl.in_window = {2, 4, 40, 36};
        ctrl.out_window = {32, 16, 64, 64};
        h.exec(std::format("YUV noise window (2, 4) 40x36 to (32, 16) 64x64 of 128x96, chroma type {}", chroma), yuv,
            false, noise(64, 48, seed++), ctrl);
    }
    h.r("cellVpostClose", {yuv}, "Close of the YUV handle");
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellVpost's comparison calls Sony's exports by their names; pspocket's library, firmware first", "[hle][vpost]")
{
    check_names("cellVpost", {"cellVpostQueryAttr", "cellVpostOpen", "cellVpostOpenEx", "cellVpostClose", "cellVpostExec"});
}

TEST_CASE("cellVpost against Sony's libvpost: sizes, handles, conversions, scaling, windows, picture information",
    "[.][hle][lle][compare][vpost]")
{
    const Setup s = setup();
    run(s, "cellVpost against Sony's libvpost", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            // libvpost's attributes, the handle at the work memory's first 128-byte boundary, the pictures.
            CHECK(o.value("QueryAttr 1920x1080 RGBA: the attribute") == "hex:005f9ae500eeeeee0004000005060606");
            CHECK(o.value("QueryAttr 16x16 RGBA") == "0x80610411");
            CHECK(o.value("Open at work memory +0x10: the handle word") == "the work memory +0x80");
            CHECK(o.value("Open RGBA up to 192x192: the handle") == "the work memory +0x0");
            CHECK(o.value("Exec flat YCbCr 235 128 128 (matrix 0, range 1)") == "0x0");
            const Data* white = o.find_data("Exec flat YCbCr 235 128 128 (matrix 0, range 1): the output");
            REQUIRE(white);
            CHECK(std::vector<u8>(white->bytes.begin(), white->bytes.begin() + 4) == std::vector<u8>{255, 255, 255, 0x5A});
            CHECK(o.value("Exec with exec type 8") == "0x806104a5");
            CHECK(o.value("Exec of a pointer that is no handle") == "0x806104a1");
        });
}
