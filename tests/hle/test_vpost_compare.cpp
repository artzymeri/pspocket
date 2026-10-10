// cellVpost against Sony's libvpost (D-564, D-597): the same configurations, resources and pictures go to pspocket's
// library and to Sony's module. Compared: QueryAttr's answers and attributes; Open's, OpenEx's and Close's answers and
// the handle (as an offset into the work memory when it lies there); for each Exec its answer, the picture information
// and the output picture, with the bytes past it and, with an output window, the pixels outside it.
//
// What counts as the same picture (D-597): libvpost converts on its SPUs with its own arithmetic, so a sample may be
// one step off pspocket's where the picture keeps its size (RGB within 1, the alpha exactly), and two where it is
// scaled (its filter's taps and rounding are not known: a filter of another shape shows larger errors at the edges and
// in the impulse pictures, which are there to show its taps). A YUV picture of the same size is a copy: exactly. The
// pixels outside an output window and the bytes past the output must stay as they were: exactly. The counts within the
// tolerance are reported. No game data; skipped without the firmware (compare_util.hpp).

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
constexpr u32 kMax = 128;                                 // the handles' largest pictures
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
        std::memcpy(b + kIn, p.bytes.data(), p.bytes.size());
        write_ctrl(c);
        const u32 out_bytes = rgba ? c.out_w * c.out_h * 4 : c.out_w * c.out_h + 2 * ((c.out_w / 2) * (c.out_h / 2));
        std::memset(b + kOut, kFill, out_bytes + 256);
        std::memset(b + kInfo, kFill, 96);
        const u64 r = Half::r("cellVpostExec", {h, kIn, kCtrl, kOut, kInfo}, "Exec " + what);
        o.line("Exec " + what + ": the picture information", hex(b + kInfo, 96));
        o.line("Exec " + what + ": the 256 bytes past the output", hex(b + kOut + out_bytes, 256));
        if (r != 0) return;
        const bool same_size = c.out_w == p.w && c.out_h == p.h && !c.in_window[2] && !c.out_window[2];
        const Rule rule = !rgba && same_size ? Rule::exact()
            : rgba                           ? Rule::within(same_size ? 1 : 2, 4, 3)
                                             : Rule::within(2);
        const std::array<u32, 4>& win = c.out_window;
        if (!rgba || !win[2] || !win[3]) {
            o.bytes("Exec " + what + ": the output", b + kOut, out_bytes, rule);
            return;
        }
        // An RGBA output window: inside it under the rule, outside it exactly as it was.
        std::vector<u8> inside, outside;
        for (u32 y = 0; y < c.out_h; y++)
            for (u32 x = 0; x < c.out_w; x++) {
                const bool in = x >= win[0] && x < win[0] + win[2] && y >= win[1] && y < win[1] + win[3];
                const u8* px = b + kOut + 4 * (y * c.out_w + x);
                std::vector<u8>& to = in ? inside : outside;
                to.insert(to.end(), px, px + 4);
            }
        o.bytes("Exec " + what + ": the output inside the window", std::move(inside), rule);
        o.bytes("Exec " + what + ": the output outside the window", std::move(outside), Rule::exact());
    }
};

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
        {1280, 720, 1280, 720, kRgba, 0, 0, 0, 0, "1280x720 RGBA"},
        {720, 480, 1280, 720, kYuv, 0, 0, 0, 0, "720x480 to 1280x720 YUV"},
        {128, 128, 128, 128, kRgba, 0, 0, 0, 0, "128x128 RGBA"},
        {128, 128, 128, 128, kYuv, 0, 0, 0, 0, "128x128 YUV"},
        {16, 16, 16, 16, kRgba, 0, 0, 0, 0, "16x16 RGBA"},
        {2, 2, 4, 4, kRgba, 0, 0, 0, 0, "2x2 to 4x4 RGBA"},
        {4096, 4096, 4096, 4096, kRgba, 0, 0, 0, 0, "4096x4096 RGBA"},
        {1280, 720, 1280, 720, 2, 0, 0, 0, 0, "output format 2"},
        {1280, 720, 1280, 720, kRgba, 1, 0, 0, 0, "input format 1"},
        {1280, 720, 1280, 720, kRgba, 0, 1, 0, 0, "input depth 1"},
        {1280, 720, 1280, 720, kRgba, 0, 0, 1, 0, "output depth 1"},
        {0, 720, 1280, 720, kRgba, 0, 0, 0, 0, "input width 0"},
        {1280, 0, 1280, 720, kRgba, 0, 0, 0, 0, "input height 0"},
        {1280, 720, 0, 720, kRgba, 0, 0, 0, 0, "output width 0"},
        {1280, 720, 1280, 0, kRgba, 0, 0, 0, 0, "output height 0"},
        {1280, 720, 1280, 720, kRgba, 0, 0, 0, 7, "reserved words set"},
    };
    for (const Query& q : queries) {
        h.write_cfg(q.in_w, q.in_h, q.out_w, q.out_h, q.out_fmt, q.in_fmt, q.in_depth, q.out_depth, q.reserved);
        h.query(q.what);
    }

    // Open's checks, then an RGBA handle and a YUV one, each for pictures up to 128x128.
    h.write_cfg(kMax, kMax, kMax, kMax, kRgba);
    const u32 size = h.query("for Open's checks");
    const u32 mem = std::min(size ? size : 0x400000u, kWorkSize);
    const auto open_refused = [&](const std::string& what, u32 cfg, u32 rsrc, u32 handle) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        h.r("cellVpostOpen", {cfg, rsrc, handle}, "Open " + what);
        h.o.line("Open " + what + ": the handle word", std::format("{:#x}", ps_read32(b, kHandle)));
        if (ps_read32(b, kHandle) != 0xEEEEEEEE) h.r("cellVpostClose", {ps_read32(b, kHandle)}, "Close after Open " + what);
    };
    h.write_resource(kWork, mem);
    open_refused("without a configuration", 0, kRsrc, kHandle);
    open_refused("without a resource", kCfg, 0, kHandle);
    open_refused("without a handle pointer", kCfg, kRsrc, 0);
    h.write_cfg(kMax, kMax, kMax, kMax, 7);
    open_refused("with output format 7", kCfg, kRsrc, kHandle);
    h.write_cfg(kMax, kMax, kMax, kMax, kRgba);
    h.write_resource(0, mem);
    open_refused("without work memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, 0);
    open_refused("with no memory size", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem > 0x100 ? mem - 0x100 : 0);
    open_refused("with 0x100 bytes less memory", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 4000);
    open_refused("with PPU priority 4000", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0);
    open_refused("with no PPU stack", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0x4000, 300);
    open_refused("with SPU priority 300", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0x4000, 100, 0);
    open_refused("with no SPUs", kCfg, kRsrc, kHandle);
    h.write_resource(kWork, mem, 1000, 0x4000, 100, 7);
    open_refused("with seven SPUs", kCfg, kRsrc, kHandle);
    // OpenEx's checks (a good one needs a SPURS instance: by hand on the Mac).
    std::memset(b + kRsrc, 0, 24);
    ps_write32(b, kRsrc, kWork);
    ps_write32(b, kRsrc + 4, mem);
    for (const auto& [cfg, rsrc, handle, what] :
        {std::tuple{0u, kRsrc, kHandle, "without a configuration"}, std::tuple{kCfg, 0u, kHandle, "without a resource"},
            std::tuple{kCfg, kRsrc, 0u, "without a handle pointer"}}) {
        ps_write32(b, kHandle, 0xEEEEEEEE);
        h.r("cellVpostOpenEx", {cfg, rsrc, handle}, std::string("OpenEx ") + what);
    }

    const u32 rgba = h.open("RGBA up to 128x128", kMax, kMax, kMax, kMax, kRgba);
    if (!rgba) return std::move(h.o);
    h.r("cellVpostClose", {0}, "Close(NULL)");
    h.r("cellVpostClose", {kWork + 0x40}, "Close of a pointer into the work memory that is no handle");

    // Same size: flat colours in both matrices and ranges (test_vpost.cpp's), then ramps with each chroma siting.
    struct Colour {
        s32 matrix, range;
        u8 y, u, v;
    };
    const Colour colours[] = {{kBt601, kBroadcast, 16, 128, 128}, {kBt601, kBroadcast, 235, 128, 128},
        {kBt601, kBroadcast, 81, 90, 240}, {kBt601, kBroadcast, 145, 54, 34}, {kBt601, kBroadcast, 41, 240, 110},
        {kBt709, kBroadcast, 63, 102, 240}, {kBt709, kBroadcast, 173, 42, 26}, {kBt709, kBroadcast, 32, 240, 118},
        {kBt601, kFull, 0, 128, 128}, {kBt601, kFull, 255, 128, 128}, {kBt601, kFull, 76, 85, 255},
        {kBt601, kFull, 150, 44, 21}, {kBt601, kFull, 29, 255, 107}, {kBt709, kFull, 54, 99, 255},
        {kBt709, kFull, 182, 30, 12}, {kBt709, kFull, 18, 255, 116}, {kBt709, kFull, 128, 128, 128}};
    for (const Colour& c : colours) {
        Ctrl ctrl;
        ctrl.out_w = ctrl.out_h = 16;
        ctrl.matrix = c.matrix;
        ctrl.range = c.range;
        ctrl.alpha = 0x5A;
        h.exec(std::format("flat YCbCr {} {} {} (matrix {}, range {})", c.y, c.u, c.v, c.matrix, c.range), rgba, true,
            flat(16, 16, c.y, c.u, c.v), ctrl);
    }
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
                h.exec(std::format("64x48 ramps (matrix {}, range {}, chroma type {})", matrix, range, chroma), rgba,
                    true, gradient(64, 48), ctrl);
            }

    // Scaled: ramps, edges and impulses enlarged and reduced, the tiny cases of test_vpost.cpp, every scaler type.
    struct Scale {
        u32 in_w, in_h, out_w, out_h;
        int kind; // 0 ramps, 1 edges, 2 impulse
    };
    const Scale scales[] = {{64, 48, 96, 72, 0}, {64, 48, 32, 24, 0}, {64, 48, 128, 96, 0}, {64, 48, 48, 48, 0},
        {64, 48, 96, 72, 1}, {64, 48, 32, 24, 1}, {32, 32, 64, 64, 2}, {32, 32, 48, 48, 2}, {32, 32, 16, 16, 2},
        {32, 32, 96, 24, 2}};
    for (const Scale& sc : scales)
        for (const s32 scaler : {0, 1, 2, 3}) {
            Ctrl ctrl;
            ctrl.out_w = sc.out_w;
            ctrl.out_h = sc.out_h;
            ctrl.scaler = scaler;
            const Picture p = sc.kind == 0 ? gradient(sc.in_w, sc.in_h)
                : sc.kind == 1             ? edges(sc.in_w, sc.in_h)
                                           : impulse(sc.in_w, sc.in_h);
            const char* kind = sc.kind == 0 ? "ramps" : sc.kind == 1 ? "edges" : "impulse";
            h.exec(std::format("{} {}x{} to {}x{}, scaler type {}", kind, sc.in_w, sc.in_h, sc.out_w, sc.out_h, scaler),
                rgba, true, p, ctrl);
        }
    {
        Ctrl ctrl;
        ctrl.out_w = 4;
        ctrl.out_h = 2;
        h.exec("2x2 to 4x2 (black and white)", rgba, true,
            make(2, 2, [](u32 x, u32) { return x ? 255 : 0; }, [](u32, u32) { return 128; }, [](u32, u32) { return 128; }),
            ctrl);
        ctrl.out_w = 2;
        const u8 ramp[4] = {0, 100, 200, 255};
        h.exec("4x2 to 2x2 (0, 100, 200, 255)", rgba, true,
            make(4, 2, [&](u32 x, u32) { return ramp[x]; }, [](u32, u32) { return 128; }, [](u32, u32) { return 128; }),
            ctrl);
    }

    // Windows: part of the input to part of the output, the rest left alone; one past the picture's edge.
    {
        Ctrl ctrl;
        ctrl.out_w = 64;
        ctrl.out_h = 48;
        ctrl.in_window = {8, 8, 32, 24};
        ctrl.out_window = {16, 8, 32, 24};
        h.exec("64x48 ramps, window (8, 8) 32x24 to (16, 8) 32x24", rgba, true, gradient(64, 48), ctrl);
        ctrl.out_window = {8, 4, 48, 36};
        h.exec("64x48 ramps, window (8, 8) 32x24 to (8, 4) 48x36", rgba, true, gradient(64, 48), ctrl);
        ctrl.in_window = {48, 32, 32, 32};
        ctrl.out_window = {40, 30, 40, 40};
        h.exec("64x48 ramps, windows past the edges", rgba, true, gradient(64, 48), ctrl);
        ctrl.in_window = {1, 1, 2, 2};
        ctrl.out_window = {1, 1, 2, 2};
        ctrl.out_w = ctrl.out_h = 4;
        ctrl.chroma = 1;
        ctrl.matrix = kBt709;
        h.exec("4x4, window (1, 1) 2x2 to (1, 1) 2x2 (test_vpost.cpp's)", rgba, true,
            make(4, 4, [](u32 x, u32 y) { return 10 * x + 50 * y; }, [](u32, u32) { return 128; },
                [](u32, u32) { return 128; }),
            ctrl);
    }

    // The exec types (fields and frames) and interpolation types on one picture: the information they report.
    for (s32 type = 0; type < 8; type++)
        for (const s32 ipc : {0, 1, 2}) {
            Ctrl ctrl;
            ctrl.exec_type = type;
            ctrl.ipc = ipc;
            ctrl.out_w = 64;
            ctrl.out_h = 48;
            h.exec(std::format("64x48 edges, exec type {}, interpolation type {}", type, ipc), rgba, true,
                edges(64, 48), ctrl);
        }
    // The alpha and the user data as given.
    for (const u8 alpha : {u8{0}, u8{0x80}, u8{0xFF}}) {
        Ctrl ctrl;
        ctrl.out_w = ctrl.out_h = 16;
        ctrl.alpha = alpha;
        ctrl.user = 0xCAFE0000ull + alpha;
        h.exec(std::format("16x16 ramps, alpha {:#x}", alpha), rgba, true, gradient(16, 16), ctrl);
    }

    // Exec's checks (a buffer outside guest memory is not sent: Sony's SPUs would read it).
    {
        Ctrl ctrl;
        ctrl.in_w = ctrl.in_h = ctrl.out_w = ctrl.out_h = 32;
        const Picture p = gradient(32, 32);
        std::memcpy(b + kIn, p.bytes.data(), p.bytes.size());
        h.write_ctrl(ctrl);
        h.r("cellVpostExec", {0, kIn, kCtrl, kOut, kInfo}, "Exec(NULL handle)");
        h.r("cellVpostExec", {kWork + 0x40, kIn, kCtrl, kOut, kInfo}, "Exec of a pointer that is no handle");
        h.r("cellVpostExec", {rgba, 0, kCtrl, kOut, kInfo}, "Exec without a picture");
        h.r("cellVpostExec", {rgba, kIn, 0, kOut, kInfo}, "Exec without control parameters");
        h.r("cellVpostExec", {rgba, kIn, kCtrl, 0, kInfo}, "Exec without an output");
        h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, 0}, "Exec without picture information");
        const auto refused = [&](const std::string& what, Ctrl c) {
            h.write_ctrl(c);
            h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, kInfo}, "Exec " + what);
        };
        Ctrl c = ctrl;
        c.in_w = 256;
        refused("with an input wider than the handle's", c);
        c = ctrl;
        c.out_h = 256;
        refused("with an output taller than the handle's", c);
        c = ctrl;
        c.out_h = 0;
        refused("with an output of no height", c);
        c = ctrl;
        c.in_w = 0;
        refused("with an input of no width", c);
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
        c.in_w = c.in_h = 31;
        refused("with an odd size", c);
    }
    h.r("cellVpostClose", {rgba}, "Close of the RGBA handle");
    h.r("cellVpostClose", {rgba}, "Close of it again");
    h.r("cellVpostExec", {rgba, kIn, kCtrl, kOut, kInfo}, "Exec on the closed handle");

    // YUV out: a copy at the same size with each chroma siting, a flat picture enlarged, ramps and impulses scaled.
    const u32 yuv = h.open("YUV up to 128x128", kMax, kMax, kMax, kMax, kYuv);
    if (!yuv) return std::move(h.o);
    {
        const Picture counted = make(
            16, 16, [](u32 x, u32 y) { return (y * 16 + x) * 7; }, [](u32 x, u32 y) { return (y * 8 + x) * 13 + 5; },
            [](u32 x, u32 y) { return (64 + y * 8 + x) * 13 + 5; });
        for (const s32 chroma : {0, 1}) {
            Ctrl ctrl;
            ctrl.out_w = ctrl.out_h = 16;
            ctrl.chroma = chroma;
            h.exec(std::format("YUV 16x16 copied, chroma type {}", chroma), yuv, false, counted, ctrl);
        }
        Ctrl ctrl;
        ctrl.out_w = 16;
        ctrl.out_h = 12;
        h.exec("YUV flat 8x8 to 16x12", yuv, false, flat(8, 8, 50, 60, 70), ctrl);
        for (const auto& [w, hh, ow, oh] : {std::array<u32, 4>{64, 48, 96, 72}, std::array<u32, 4>{64, 48, 32, 24},
                 std::array<u32, 4>{32, 32, 64, 64}}) {
            ctrl.out_w = ow;
            ctrl.out_h = oh;
            h.exec(std::format("YUV ramps {}x{} to {}x{}", w, hh, ow, oh), yuv, false, gradient(w, hh), ctrl);
            h.exec(std::format("YUV impulse {}x{} to {}x{}", w, hh, ow, oh), yuv, false, impulse(w, hh), ctrl);
        }
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
            // pspocket's own half: RPCS3's attributes, the idm handles, test_vpost.cpp's hand-worked pictures.
            CHECK(o.value("QueryAttr 1280x720 RGBA") == "0x0");
            CHECK(o.value("QueryAttr 1280x720 RGBA: the attribute") == "hex:0040000000eeeeee0026000000280000");
            CHECK(o.value("QueryAttr output format 2") == "0x80610411");
            // idm IDs from 1, not reused at once: the eight resources of Open's checks that pspocket takes (it only
            // checks a resource is there) had 1 to 8.
            CHECK(o.value("Open with no SPUs") == "0x0");
            CHECK(o.value("Open RGBA up to 128x128: the handle") == "0x9");
            CHECK(o.value("Open YUV up to 128x128: the handle") == "0xa");
            CHECK(o.value("Exec flat YCbCr 235 128 128 (matrix 0, range 1)") == "0x0");
            const Data* white = o.find_data("Exec flat YCbCr 235 128 128 (matrix 0, range 1): the output");
            REQUIRE(white);
            CHECK(white->bytes.size() == 16 * 16 * 4);
            CHECK(std::vector<u8>(white->bytes.begin(), white->bytes.begin() + 4) == std::vector<u8>{255, 255, 255, 0x5A});
            const Data* up = o.find_data("Exec 2x2 to 4x2 (black and white): the output");
            REQUIRE(up);
            CHECK(up->bytes[0] == 0);
            CHECK(up->bytes[4] == 64);
            CHECK(up->bytes[8] == 191);
            CHECK(up->bytes[12] == 255);
            const Data* copy = o.find_data("Exec YUV 16x16 copied, chroma type 1: the output");
            REQUIRE(copy);
            CHECK(copy->bytes[17] == 17 * 7);
            CHECK(o.value("Exec with exec type 8") == "0x806104a5");
            CHECK(o.value("Exec of a pointer that is no handle") == "0x806104a1");
        });
}
