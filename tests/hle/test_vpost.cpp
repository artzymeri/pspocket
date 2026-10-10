// cellVpost (D-564): pspocket's video post-processor, made from RPCS3's (no Sony module to compare with). Synthetic
// pictures: flat colours checked against the standard YCbCr values of the primaries for each matrix and range, ramps
// scaled up and down against hand-worked values, windows, YUV output, the picture information and the checks. No game
// data.

#include "hle_test_util.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kCtrl = kData + 0x200, kInfo = kData + 0x280, kIn = kData + 0x1000, kOut = kData + 0x40000;
const u64 kQCfgNull = r3_of_code(0x80610410), kQCfgInvalid = r3_of_code(0x80610411),
          kQAttrNull = r3_of_code(0x80610412), kOCfgNull = r3_of_code(0x80610440),
          kOCfgInvalid = r3_of_code(0x80610441), kORsrcNull = r3_of_code(0x80610442),
          kOHdlNull = r3_of_code(0x80610444), kCHdlNull = r3_of_code(0x80610470),
          kCHdlInvalid = r3_of_code(0x80610471), kEHdlNull = r3_of_code(0x806104A0),
          kEHdlInvalid = r3_of_code(0x806104A1), kEInNull = r3_of_code(0x806104A2),
          kEInInvalid = r3_of_code(0x806104A3), kECtrlNull = r3_of_code(0x806104A4),
          kECtrlInvalid = r3_of_code(0x806104A5), kEOutNull = r3_of_code(0x806104A6),
          kEPicInfoNull = r3_of_code(0x806104A8);
constexpr s32 kRgba = 0, kYuv = 1;
constexpr s32 kFull = 0, kBroadcast = 1, kBt601 = 0, kBt709 = 1;

void write_cfg(u8* b, u32 in_w, u32 in_h, u32 out_w, u32 out_h, s32 out_fmt)
{
    const u32 v[10] = {in_w, in_h, 0, 0, out_w, out_h, 0, static_cast<u32>(out_fmt), 0, 0};
    for (u32 i = 0; i < 10; i++) ps_write32(b, kCfg + 4 * i, v[i]);
}

u32 open(GuestThread& g, u32 in_w, u32 in_h, u32 out_w, u32 out_h, s32 out_fmt)
{
    u8* b = g.f.base();
    write_cfg(b, in_w, in_h, out_w, out_h, out_fmt);
    std::memset(b + kRsrc, 0, 24);
    ps_write32(b, kRsrc, kData + 0x100000); // memAddr, memSize: not touched
    ps_write32(b, kRsrc + 4, 4 * 1024 * 1024);
    REQUIRE(g.call("cellVpostOpen", kCfg, kRsrc, kHandle) == 0);
    return ps_read32(b, kHandle);
}

struct Ctrl {
    s32 exec_type = 0, scaler = 0;
    u32 in_w = 0, in_h = 0;
    s32 chroma = 0, range = kFull, matrix = kBt601;
    std::array<u32, 4> in_window{}, out_window{};
    u32 out_w = 0, out_h = 0;
    u8 alpha = 0xFF;
    u64 user = 0;
};

void write_ctrl(u8* b, const Ctrl& c)
{
    std::memset(b + kCtrl, 0, 96);
    ps_write32(b, kCtrl, static_cast<u32>(c.exec_type));
    ps_write32(b, kCtrl + 4, static_cast<u32>(c.scaler));
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

u64 exec(GuestThread& g, u32 handle, const Ctrl& c)
{
    write_ctrl(g.f.base(), c);
    return g.call("cellVpostExec", handle, kIn, kCtrl, kOut, kInfo);
}

// A w x h picture: Y = luma(x, y), U and V flat.
template <class Luma>
void write_picture(u8* b, u32 w, u32 h, Luma luma, u8 u, u8 v)
{
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) ps_write8(b, kIn + y * w + x, luma(x, y));
    std::memset(b + kIn + w * h, u, (w / 2) * (h / 2));
    std::memset(b + kIn + w * h + (w / 2) * (h / 2), v, (w / 2) * (h / 2));
}

std::array<u8, 4> pixel(u8* b, u32 out_w, u32 x, u32 y)
{
    const u32 at = kOut + 4 * (y * out_w + x);
    return {ps_read8(b, at), ps_read8(b, at + 1), ps_read8(b, at + 2), ps_read8(b, at + 3)};
}

bool near(u8 a, int want, int tolerance) { return std::abs(static_cast<int>(a) - want) <= tolerance; }

} // namespace

TEST_CASE("cellVpost: the five functions, firmware first", "[hle][vpost]")
{
    for (const std::string_view name :
        {"cellVpostQueryAttr", "cellVpostOpen", "cellVpostOpenEx", "cellVpostClose", "cellVpostExec"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellVpost", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
    CHECK(hle::registry().is_firmware_first("cellVpost"));
}

TEST_CASE("cellVpost: QueryAttr's answers and checks; handles", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_cfg(b, 1280, 720, 1280, 720, kRgba);
    CHECK(g.call("cellVpostQueryAttr", 0, kAttr) == kQCfgNull);
    CHECK(g.call("cellVpostQueryAttr", kCfg, 0) == kQAttrNull);
    ps_write32(b, kCfg + 28, 2); // no such output format
    CHECK(g.call("cellVpostQueryAttr", kCfg, kAttr) == kQCfgInvalid);
    ps_write32(b, kCfg + 28, kYuv);
    ps_write32(b, kCfg + 12, 1); // no such input format
    CHECK(g.call("cellVpostQueryAttr", kCfg, kAttr) == kQCfgInvalid);
    ps_write32(b, kCfg + 12, 0);
    ps_write32(b, kCfg, 0); // no input width
    CHECK(g.call("cellVpostQueryAttr", kCfg, kAttr) == kQCfgInvalid);
    ps_write32(b, kCfg, 1280);
    std::memset(b + kAttr, 0xAA, 16);
    REQUIRE(g.call("cellVpostQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == 4 * 1024 * 1024); // memSize
    CHECK(ps_read8(b, kAttr + 4) == 0);            // delay
    CHECK(ps_read32(b, kAttr + 8) == 0x260000);    // vpostVerUpper
    CHECK(ps_read32(b, kAttr + 12) == 0x280000);   // vpostVerLower

    // Handles: idm IDs from 1, not reused at once; the checks in the error codes' order.
    CHECK(g.call("cellVpostOpen", 0, kRsrc, kHandle) == kOCfgNull);
    write_cfg(b, 64, 64, 64, 64, 7);
    CHECK(g.call("cellVpostOpen", kCfg, kRsrc, kHandle) == kOCfgInvalid);
    write_cfg(b, 64, 64, 64, 64, kRgba);
    CHECK(g.call("cellVpostOpen", kCfg, 0, kHandle) == kORsrcNull);
    CHECK(g.call("cellVpostOpen", kCfg, kRsrc, 0) == kOHdlNull);
    const u32 h1 = open(g, 64, 64, 64, 64, kRgba), h2 = open(g, 64, 64, 64, 64, kYuv);
    CHECK(h1 == 1);
    CHECK(h2 == 2);
    REQUIRE(g.call("cellVpostClose", h1) == 0);
    CHECK(g.call("cellVpostClose", h1) == kCHdlInvalid);
    CHECK(g.call("cellVpostClose", 0) == kCHdlNull);
    REQUIRE(g.call("cellVpostOpenEx", kCfg, kRsrc, kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == 3);
    CHECK(g.call("cellVpostClose", 3) == 0);
    CHECK(g.call("cellVpostClose", h2) == 0);
}

TEST_CASE("cellVpost: colours: BT.601 and BT.709, full and broadcast range, to RGBA with the alpha asked",
    "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, 16, 16, 16, 16, kRgba);
    // The standard YCbCr of black, white and the primaries (rounded, hence the tolerance).
    struct Colour {
        s32 matrix, range;
        u8 y, u, v;
        std::array<int, 3> rgb;
    };
    const Colour colours[] = {
        {kBt601, kBroadcast, 16, 128, 128, {0, 0, 0}},
        {kBt601, kBroadcast, 235, 128, 128, {255, 255, 255}},
        {kBt601, kBroadcast, 81, 90, 240, {255, 0, 0}},
        {kBt601, kBroadcast, 145, 54, 34, {0, 255, 0}},
        {kBt601, kBroadcast, 41, 240, 110, {0, 0, 255}},
        {kBt709, kBroadcast, 63, 102, 240, {255, 0, 0}},
        {kBt709, kBroadcast, 173, 42, 26, {0, 255, 0}},
        {kBt709, kBroadcast, 32, 240, 118, {0, 0, 255}},
        {kBt601, kFull, 0, 128, 128, {0, 0, 0}},
        {kBt601, kFull, 255, 128, 128, {255, 255, 255}},
        {kBt601, kFull, 76, 85, 255, {255, 0, 0}},
        {kBt601, kFull, 150, 44, 21, {0, 255, 0}},
        {kBt601, kFull, 29, 255, 107, {0, 0, 255}},
        {kBt709, kFull, 54, 99, 255, {255, 0, 0}},
        {kBt709, kFull, 182, 30, 12, {0, 255, 0}},
        {kBt709, kFull, 18, 255, 116, {0, 0, 255}},
        {kBt709, kFull, 128, 128, 128, {128, 128, 128}},
    };
    for (const Colour& c : colours) {
        INFO("matrix " << c.matrix << " range " << c.range << " YCbCr " << int{c.y} << " " << int{c.u} << " "
                       << int{c.v});
        write_picture(b, 16, 16, [&](u32, u32) { return c.y; }, c.u, c.v);
        Ctrl ctrl;
        ctrl.in_w = ctrl.in_h = ctrl.out_w = ctrl.out_h = 16;
        ctrl.matrix = c.matrix;
        ctrl.range = c.range;
        ctrl.alpha = 0x5A;
        REQUIRE(exec(g, h, ctrl) == 0);
        bool all = true;
        for (u32 y = 0; y < 16; y++)
            for (u32 x = 0; x < 16; x++) {
                const auto p = pixel(b, 16, x, y);
                all = all && near(p[0], c.rgb[0], 3) && near(p[1], c.rgb[1], 3) && near(p[2], c.rgb[2], 3) &&
                      p[3] == 0x5A;
            }
        const auto p = pixel(b, 16, 5, 5);
        INFO("RGBA " << int{p[0]} << " " << int{p[1]} << " " << int{p[2]} << " " << int{p[3]});
        CHECK(all);
    }
    CHECK(g.call("cellVpostClose", h) == 0);
}

TEST_CASE("cellVpost: scaling: enlarging interpolates, reducing averages; windows; the picture information",
    "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, 64, 64, 64, 64, kRgba);
    Ctrl ctrl;
    ctrl.range = kFull; // with grey chroma, R = G = B = Y
    const auto row = [&](u32 out_w, u32 y) {
        std::array<u8, 8> r{};
        for (u32 x = 0; x < out_w && x < 8; x++) r[x] = pixel(b, out_w, x, y)[0];
        return r;
    };

    // 2 x 2 to 4 x 2: each output pixel's centre between two input pixels, edges held.
    write_picture(b, 2, 2, [](u32 x, u32) { return static_cast<u8>(x ? 255 : 0); }, 128, 128);
    ctrl.in_w = 2;
    ctrl.in_h = 2;
    ctrl.out_w = 4;
    ctrl.out_h = 2;
    REQUIRE(exec(g, h, ctrl) == 0);
    CHECK(row(4, 0) == std::array<u8, 8>{0, 64, 191, 255});
    CHECK(row(4, 1) == std::array<u8, 8>{0, 64, 191, 255});
    CHECK(pixel(b, 4, 1, 0)[1] == 64);
    CHECK(pixel(b, 4, 1, 0)[2] == 64);

    // 4 x 2 to 2 x 2: a tent two input pixels wide each side: (0 + 3 x 0 + 3 x 100 + 200) / 8 = 62.5, and
    // (100 + 3 x 200 + 3 x 255 + 255) / 8 = 215.
    const u8 ramp[4] = {0, 100, 200, 255};
    write_picture(b, 4, 2, [&](u32 x, u32) { return ramp[x]; }, 128, 128);
    ctrl.in_w = 4;
    ctrl.out_w = 2;
    REQUIRE(exec(g, h, ctrl) == 0);
    CHECK(row(2, 0) == std::array<u8, 8>{63, 215});

    // Windows: the input's 2 x 2 from (1, 1) to the output's 2 x 2 at (1, 1), the rest of the output untouched; the
    // picture information as asked.
    write_picture(b, 4, 4, [](u32 x, u32 y) { return static_cast<u8>(10 * x + 50 * y); }, 128, 128);
    std::memset(b + kOut, 0xEE, 4 * 4 * 4);
    ctrl.in_w = ctrl.in_h = ctrl.out_w = ctrl.out_h = 4;
    ctrl.in_window = {1, 1, 2, 2};
    ctrl.out_window = {1, 1, 2, 2};
    ctrl.chroma = 1;
    ctrl.matrix = kBt709;
    ctrl.user = 0x1122334455667788ull;
    REQUIRE(exec(g, h, ctrl) == 0);
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 4; x++) {
            INFO(x << "," << y);
            const auto p = pixel(b, 4, x, y);
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) CHECK(p[0] == 10 * x + 50 * y);
            else CHECK(p == std::array<u8, 4>{0xEE, 0xEE, 0xEE, 0xEE});
        }
    }
    const u32 want[] = {4, 4, 0, 0, 0, 1, 0, kFull, kBt709, 4, 4, 0, 0, kRgba, 1, 0, kFull, kBt709};
    for (u32 i = 0; i < std::size(want); i++) {
        INFO("field " << i);
        CHECK(ps_read32(b, kInfo + 4 * i) == want[i]);
    }
    CHECK(ps_read64(b, kInfo + 72) == 0x1122334455667788ull);
    // A field-to-frame exec type: reported in the information, the picture taken as a frame.
    ctrl.exec_type = 3; // ITOP_PFRM
    REQUIRE(exec(g, h, ctrl) == 0);
    CHECK(ps_read32(b, kInfo + 12) == 1); // inScanType I
    CHECK(ps_read32(b, kInfo + 24) == 2); // inPicStruct ITOP
    CHECK(ps_read32(b, kInfo + 48) == 0); // outScanType P
    CHECK(ps_read32(b, kInfo + 60) == 0); // outPicStruct PFRM
    CHECK(g.call("cellVpostClose", h) == 0);
}

TEST_CASE("cellVpost: YUV 4:2:0 out: a copy at the same size, a flat picture enlarged", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, 64, 64, 64, 64, kYuv);
    for (u32 i = 0; i < 16 * 16; i++) ps_write8(b, kIn + i, static_cast<u8>(i * 7));
    for (u32 i = 0; i < 2 * 8 * 8; i++) ps_write8(b, kIn + 256 + i, static_cast<u8>(i * 13 + 5));
    Ctrl ctrl;
    ctrl.in_w = ctrl.in_h = ctrl.out_w = ctrl.out_h = 16;
    for (const s32 chroma : {0, 1}) {
        ctrl.chroma = chroma;
        std::memset(b + kOut, 0, 384);
        REQUIRE(exec(g, h, ctrl) == 0);
        CHECK(std::memcmp(b + kIn, b + kOut, 384) == 0);
    }
    write_picture(b, 8, 8, [](u32, u32) { return u8{50}; }, 60, 70);
    ctrl.in_w = ctrl.in_h = 8;
    ctrl.out_w = 16;
    ctrl.out_h = 12;
    REQUIRE(exec(g, h, ctrl) == 0);
    bool flat = true;
    for (u32 i = 0; i < 16 * 12; i++) flat = flat && ps_read8(b, kOut + i) == 50;
    for (u32 i = 0; i < 8 * 6; i++)
        flat = flat && ps_read8(b, kOut + 192 + i) == 60 && ps_read8(b, kOut + 192 + 48 + i) == 70;
    CHECK(flat);
    CHECK(ps_read32(b, kInfo + 52) == kYuv); // outPicFmt
    CHECK(g.call("cellVpostClose", h) == 0);
}

TEST_CASE("cellVpost: Exec's checks", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, 32, 32, 32, 32, kRgba);
    Ctrl ctrl;
    ctrl.in_w = ctrl.in_h = ctrl.out_w = ctrl.out_h = 32;
    write_ctrl(b, ctrl);
    CHECK(g.call("cellVpostExec", 0, kIn, kCtrl, kOut, kInfo) == kEHdlNull);
    CHECK(g.call("cellVpostExec", h + 7, kIn, kCtrl, kOut, kInfo) == kEHdlInvalid);
    CHECK(g.call("cellVpostExec", h, 0, kCtrl, kOut, kInfo) == kEInNull);
    CHECK(g.call("cellVpostExec", h, kIn, 0, kOut, kInfo) == kECtrlNull);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, 0, kInfo) == kEOutNull);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut, 0) == kEPicInfoNull);
    CHECK(g.call("cellVpostExec", h, 0xE0000000, kCtrl, kOut, kInfo) == kEInInvalid);
    ctrl.in_w = 64; // past the handle's maximum
    CHECK(exec(g, h, ctrl) == kECtrlInvalid);
    ctrl.in_w = 32;
    ctrl.out_h = 0;
    CHECK(exec(g, h, ctrl) == kECtrlInvalid);
    ctrl.out_h = 32;
    ctrl.exec_type = 8;
    CHECK(exec(g, h, ctrl) == kECtrlInvalid);
    ctrl.exec_type = 0;
    CHECK(exec(g, h, ctrl) == 0);
    CHECK(g.call("cellVpostClose", h) == 0);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut, kInfo) == kEHdlInvalid);
}
