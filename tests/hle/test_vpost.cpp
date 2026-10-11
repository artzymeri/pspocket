// cellVpost (D-564, D-913): pspocket's video post-processor, made to answer and convert as Sony's libvpost does
// (tests/hle/test_vpost_compare.cpp compares it with libvpost.sprx). Synthetic pictures: libvpost's colour conversion
// in each matrix and range, its chroma sitings, its bilinear scaler (positions, rounding, sixteen columns at a time),
// windows on black, YUV output, the picture information, QueryAttr's memory, the handle in the work memory and every
// check. No game data.

#include "hle_test_util.hpp"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kCfg = kData + 0x100, kAttr = kData + 0x140, kRsrc = kData + 0x180, kHandle = kData + 0x1C0,
              kCtrl = kData + 0x200, kInfo = kData + 0x280, kIn = kData + 0x1000, kOut = kData + 0x40000,
              kWork = kData + 0x100000;
const u64 kQCfgNull = r3_of_code(0x80610410), kQCfgInvalid = r3_of_code(0x80610411),
          kQAttrNull = r3_of_code(0x80610412), kOCfgNull = r3_of_code(0x80610440),
          kORsrcNull = r3_of_code(0x80610442), kORsrcInvalid = r3_of_code(0x80610443),
          kOHdlNull = r3_of_code(0x80610444), kCHdlNull = r3_of_code(0x80610470),
          kCHdlInvalid = r3_of_code(0x80610471), kEHdlNull = r3_of_code(0x806104A0),
          kEHdlInvalid = r3_of_code(0x806104A1), kEInNull = r3_of_code(0x806104A2),
          kEInInvalid = r3_of_code(0x806104A3), kECtrlNull = r3_of_code(0x806104A4),
          kECtrlInvalid = r3_of_code(0x806104A5), kEOutNull = r3_of_code(0x806104A6),
          kEOutInvalid = r3_of_code(0x806104A7), kEPicInfoNull = r3_of_code(0x806104A8);
constexpr u32 kRgba = 0, kYuv = 1, kFull = 0, kBroadcast = 1, kBt601 = 0, kBt709 = 1;

void write_cfg(u8* b, u32 in_w, u32 in_h, u32 out_w, u32 out_h, u32 out_fmt)
{
    const u32 v[10] = {in_w, in_h, 0, 0, out_w, out_h, 0, out_fmt, 0, 0};
    for (u32 i = 0; i < 10; i++) ps_write32(b, kCfg + 4 * i, v[i]);
}

void write_resource(u8* b, u32 mem, u32 size, u32 ppu = 1000, u32 spu = 100, u32 spus = 1)
{
    const u32 v[6] = {mem, size, ppu, 0x4000, spu, spus};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRsrc + 4 * i, v[i]);
}

u32 open(GuestThread& g, u32 out_fmt, u32 max = 256)
{
    u8* b = g.f.base();
    write_cfg(b, max, max, max, max, out_fmt);
    REQUIRE(g.call("cellVpostQueryAttr", kCfg, kAttr) == 0);
    write_resource(b, kWork, ps_read32(b, kAttr));
    REQUIRE(g.call("cellVpostOpen", kCfg, kRsrc, kHandle) == 0);
    return ps_read32(b, kHandle);
}

struct Ctrl {
    u32 exec_type = 0, scaler = 0, ipc = 0, in_w = 0, in_h = 0, chroma = 0, range = kFull, matrix = kBt601;
    std::array<u32, 4> in_window{}, out_window{};
    u32 out_w = 0, out_h = 0;
    u8 alpha = 0xFF;
    u64 user = 0;
};

void write_ctrl(u8* b, const Ctrl& c)
{
    std::memset(b + kCtrl, 0, 96);
    const u32 w[8] = {c.exec_type, c.scaler, c.ipc, c.in_w, c.in_h, c.chroma, c.range, c.matrix};
    for (u32 i = 0; i < 8; i++) ps_write32(b, kCtrl + 4 * i, w[i]);
    const std::array<u32, 4> iw = c.in_window[2] ? c.in_window : std::array<u32, 4>{0, 0, c.in_w, c.in_h};
    const std::array<u32, 4> ow = c.out_window[2] ? c.out_window : std::array<u32, 4>{0, 0, c.out_w, c.out_h};
    for (u32 i = 0; i < 4; i++) ps_write32(b, kCtrl + 32 + 4 * i, iw[i]);
    ps_write32(b, kCtrl + 48, c.out_w);
    ps_write32(b, kCtrl + 52, c.out_h);
    for (u32 i = 0; i < 4; i++) ps_write32(b, kCtrl + 56 + 4 * i, ow[i]);
    ps_write8(b, kCtrl + 72, c.alpha);
    ps_write64(b, kCtrl + 80, c.user);
}

// A picture: Y, then U, then V, each its width's stride.
struct Pic {
    u32 w, h;
    std::vector<u8> y, u, v;
};

Pic flat(u32 w, u32 h, u8 y, u8 u, u8 v)
{
    return {w, h, std::vector<u8>(usize{w} * h, y), std::vector<u8>(usize{w / 2} * (h / 2), u),
        std::vector<u8>(usize{w / 2} * (h / 2), v)};
}

u64 exec(GuestThread& g, u32 handle, const Pic& p, Ctrl c)
{
    u8* b = g.f.base();
    c.in_w = p.w;
    c.in_h = p.h;
    std::memcpy(b + kIn, p.y.data(), p.y.size());
    std::memcpy(b + kIn + p.y.size(), p.u.data(), p.u.size());
    std::memcpy(b + kIn + p.y.size() + p.u.size(), p.v.data(), p.v.size());
    write_ctrl(b, c);
    std::memset(b + kOut, 0xEE, 0x40000);
    return g.call("cellVpostExec", handle, kIn, kCtrl, kOut, kInfo);
}

std::array<u8, 4> pixel(GuestThread& g, u32 w, u32 x, u32 y)
{
    const u8* p = g.f.base() + kOut + (usize{y} * w + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

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

TEST_CASE("cellVpost: QueryAttr's memory and checks; Open's, OpenEx's and Close's", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    write_cfg(b, 1920, 1080, 1920, 1080, kRgba);
    REQUIRE(g.call("cellVpostQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == 0x5F9AE5); // twice a 1920x1080 frame (3,112,320 bytes) + 0x9FE5
    CHECK(ps_read8(b, kAttr + 4) == 0);
    CHECK(ps_read32(b, kAttr + 8) == 0x40000);
    CHECK(ps_read32(b, kAttr + 12) == 0x05060606);
    write_cfg(b, 128, 128, 128, 128, kYuv);
    REQUIRE(g.call("cellVpostQueryAttr", kCfg, kAttr) == 0);
    CHECK(ps_read32(b, kAttr) == 0x160E5);
    CHECK(g.call("cellVpostQueryAttr", 0, kAttr) == kQCfgNull);
    CHECK(g.call("cellVpostQueryAttr", kCfg, 0) == kQAttrNull);
    for (const auto& [iw, ih, ow, oh, fmt] : {std::array<u32, 5>{30, 32, 64, 64, 0}, {33, 32, 64, 64, 0},
             {1922, 32, 64, 64, 0}, {64, 1090, 64, 64, 0}, {64, 64, 63, 64, 0}, {64, 64, 64, 64, 2}}) {
        write_cfg(b, iw, ih, ow, oh, fmt);
        CHECK(g.call("cellVpostQueryAttr", kCfg, kAttr) == kQCfgInvalid);
    }
    write_cfg(b, 64, 64, 64, 64, kRgba);
    ps_write32(b, kCfg + 36, 1);
    CHECK(g.call("cellVpostQueryAttr", kCfg, kAttr) == kQCfgInvalid);

    write_cfg(b, 64, 64, 64, 64, kRgba);
    REQUIRE(g.call("cellVpostQueryAttr", kCfg, kAttr) == 0);
    const u32 mem = ps_read32(b, kAttr);
    CHECK(g.call("cellVpostOpen", 0, kRsrc, kHandle) == kOCfgNull);
    CHECK(g.call("cellVpostOpen", kCfg, 0, kHandle) == kORsrcNull);
    write_resource(b, kWork, mem - 1);
    CHECK(g.call("cellVpostOpen", kCfg, kRsrc, kHandle) == kORsrcInvalid);
    for (const auto& [ppu, spu, spus] :
        {std::array<u32, 3>{0xC00, 100, 1}, {1000, 15, 1}, {1000, 256, 1}, {1000, 100, 0}, {1000, 100, 2}}) {
        write_resource(b, kWork, mem, ppu, spu, spus);
        CHECK(g.call("cellVpostOpen", kCfg, kRsrc, kHandle) == kORsrcInvalid);
    }
    write_resource(b, kWork + 0x10, mem + 0x80, 0xBFF, 255);
    CHECK(g.call("cellVpostOpen", kCfg, kRsrc, 0) == kOHdlNull);
    REQUIRE(g.call("cellVpostOpen", kCfg, kRsrc, kHandle) == 0);
    const u32 h = ps_read32(b, kHandle);
    CHECK(h == kWork + 0x80); // the work memory's first 128-byte boundary
    CHECK(ps_read32(b, h) == h);
    CHECK(g.call("cellVpostClose", h + 0x80) == kCHdlInvalid);
    CHECK(g.call("cellVpostClose", 0) == kCHdlNull);
    REQUIRE(g.call("cellVpostClose", h) == 0);
    CHECK(g.call("cellVpostClose", h) == kCHdlInvalid);

    // OpenEx: CellVpostResourceEx {memAddr, memSize, spurs, priority[8], maxContention}.
    const auto ex = [&](u32 spurs, u8 p0, u8 p1, u32 contention) {
        std::memset(b + kRsrc, 0, 24);
        ps_write32(b, kRsrc, kWork);
        ps_write32(b, kRsrc + 4, mem);
        ps_write32(b, kRsrc + 8, spurs);
        ps_write8(b, kRsrc + 12, p0);
        ps_write8(b, kRsrc + 13, p1);
        ps_write32(b, kRsrc + 20, contention);
        return g.call("cellVpostOpenEx", kCfg, kRsrc, kHandle);
    };
    CHECK(ex(0, 1, 0, 1) == kORsrcInvalid);
    CHECK(ex(0x100, 0, 0, 1) == kORsrcInvalid);
    CHECK(ex(0x100, 1, 16, 1) == kORsrcInvalid);
    CHECK(ex(0x100, 1, 0, 2) == kORsrcInvalid);
    REQUIRE(ex(0x100, 1, 15, 1) == 0);
    CHECK(ps_read32(b, kHandle) == kWork);
    CHECK(g.call("cellVpostClose", kWork) == 0);
}

TEST_CASE("cellVpost: libvpost's colour conversion and chroma sitings", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    const u32 h = open(g, kRgba);
    Ctrl c;
    c.out_w = c.out_h = 32;
    c.alpha = 0x5A;
    // Full range: Y as it is, the chroma's weights in 1/8192 (R 11485 V, G -2818 U -5850 V, B 14516 U), rounded down
    // from a sum in 2^-20 with a half added: R 100 + 45.36, G 100 - 9.28, B 100 - 66.8.
    REQUIRE(exec(g, h, flat(32, 32, 100, 90, 160), c) == 0);
    CHECK(pixel(g, 32, 5, 5) == std::array<u8, 4>{145, 90, 33, 0x5A});
    // Broadcast: (Y - 16) in 19076/16384, the chroma in 1/8192 (R 13078, B 16520): black 16 and white 235.
    c.range = kBroadcast;
    REQUIRE(exec(g, h, flat(32, 32, 16, 128, 128), c) == 0);
    CHECK(pixel(g, 32, 0, 0) == std::array<u8, 4>{0, 0, 0, 0x5A});
    REQUIRE(exec(g, h, flat(32, 32, 235, 128, 128), c) == 0);
    CHECK(pixel(g, 32, 31, 31) == std::array<u8, 4>{255, 255, 255, 0x5A});
    REQUIRE(exec(g, h, flat(32, 32, 128, 128, 255), c) == 0);
    CHECK(pixel(g, 32, 1, 1)[0] == 255); // 130.4 + 202.7: clamped
    // BT.709 full range: R 12900, G -1534 and -3834, B 15201.
    c.range = kFull;
    c.matrix = kBt709;
    REQUIRE(exec(g, h, flat(32, 32, 128, 64, 192), c) == 0);
    CHECK(pixel(g, 32, 3, 3) == std::array<u8, 4>{229, 110, 9, 0x5A});

    // Chroma type A: a chroma sample for its 2x2 pixels; type B: the odd columns its average with the next (the last
    // column's its own), rounded up.
    Pic p = flat(32, 32, 128, 128, 128);
    for (u32 x = 0; x < 16; x++) p.v[x] = static_cast<u8>(100 + 3 * x);
    c.matrix = kBt601;
    c.chroma = 0;
    REQUIRE(exec(g, h, p, c) == 0);
    CHECK(pixel(g, 32, 2, 0)[0] == pixel(g, 32, 3, 0)[0]);
    CHECK(pixel(g, 32, 2, 1)[0] == pixel(g, 32, 2, 0)[0]);
    c.chroma = 1;
    REQUIRE(exec(g, h, p, c) == 0);
    const auto r_of = [](s64 v) { return static_cast<u8>(128 + ((11485 * 128 * (v - 128) + (1 << 19)) >> 20)); };
    CHECK(pixel(g, 32, 2, 0)[0] == r_of(103));
    CHECK(pixel(g, 32, 3, 0)[0] == r_of((103 + 106 + 1) / 2));
    CHECK(pixel(g, 32, 31, 0)[0] == r_of(145));
    CHECK(g.call("cellVpostClose", h) == 0);
}

TEST_CASE("cellVpost: the bilinear scaler, windows on black, YUV out, the picture information", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, kYuv);
    const auto y_at = [&](u32 w, u32 x, u32 y) { return ps_read8(b, kOut + y * w + x); };
    // An impulse enlarged twice: the samples between take the mean of their two, rounded down (vertically first).
    Pic p = flat(64, 32, 50, 128, 128);
    p.y[10 * 64 + 20] = 200;
    Ctrl c;
    c.out_w = 128;
    c.out_h = 64;
    REQUIRE(exec(g, h, p, c) == 0);
    CHECK(y_at(128, 40, 20) == 200);
    CHECK(y_at(128, 41, 20) == 125);
    CHECK(y_at(128, 41, 21) == 87);
    // Enlarged by half: the positions step 0.6640625 (170/256), rounded down.
    Pic r = flat(64, 32, 0, 128, 128);
    for (u32 x = 0; x < 64; x++) r.y[x] = static_cast<u8>(x * 4);
    c.out_w = 96;
    c.out_h = 32;
    REQUIRE(exec(g, h, r, c) == 0);
    CHECK(y_at(96, 1, 0) == 2); // 4 * 170 / 256
    CHECK(y_at(96, 3, 0) == 7); // (4 * 2 + 8 * 254) / 256
    // 64 to 40: steps of 409/256; the second sixteen columns start 16 steps of 1/65536 on, rounded down: 6553.
    c.out_w = 40;
    REQUIRE(exec(g, h, r, c) == 0);
    CHECK(y_at(40, 15, 0) == 95);  // 6135: 92 and 96 at 247/256
    CHECK(y_at(40, 16, 0) == 102); // 6553: 100 and 104 at 153/256 (409 * 16 would be 6544)
    // A window on black: Y 0 (16 in broadcast range), chroma 128 around it.
    c.out_w = 128;
    c.out_h = 64;
    c.range = kBroadcast;
    c.out_window = {34, 18, 64, 32};
    REQUIRE(exec(g, h, flat(64, 32, 200, 100, 150), c) == 0);
    CHECK(y_at(128, 33, 18) == 16);
    CHECK(y_at(128, 34, 18) == 200);
    CHECK(y_at(128, 97, 49) == 200);
    CHECK(y_at(128, 98, 49) == 16);
    CHECK(ps_read8(b, kOut + 128 * 64 + 8 * 64 + 16) == 128);
    CHECK(ps_read8(b, kOut + 128 * 64 + 9 * 64 + 17) == 100);
    // The same size: a copy.
    c = Ctrl{};
    c.out_w = 64;
    c.out_h = 32;
    c.chroma = 1;
    c.range = kBroadcast;
    c.user = 0x1122334455667788ull;
    p.u[5] = 7;
    REQUIRE(exec(g, h, p, c) == 0);
    CHECK(std::memcmp(b + kOut, p.y.data(), p.y.size()) == 0);
    CHECK(ps_read8(b, kOut + 64 * 32 + 5) == 7);
    // The picture information: what was asked, the exec type's scans and structures, the output's chroma and range.
    const u32 expect[22] = {64, 32, 0, 0, 0, 1, 0, 1, 0, 64, 32, 0, 0, kYuv, 1, 0, 1, 0, 0x11223344, 0x55667788, 0, 0};
    for (u32 i = 0; i < 22; i++) {
        INFO(i);
        CHECK(ps_read32(b, kInfo + 4 * i) == expect[i]);
    }
    c.exec_type = 4; // IBTM_PFRM
    REQUIRE(exec(g, h, p, c) == 0);
    CHECK(ps_read32(b, kInfo + 12) == 1);
    CHECK(ps_read32(b, kInfo + 24) == 3);
    CHECK(ps_read32(b, kInfo + 48) == 0);
    CHECK(ps_read32(b, kInfo + 60) == 0);
    CHECK(g.call("cellVpostClose", h) == 0);
}

TEST_CASE("cellVpost: Exec's checks", "[hle][vpost]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const u32 h = open(g, kRgba, 64);
    Ctrl c;
    c.in_w = c.in_h = c.out_w = c.out_h = 32;
    write_ctrl(b, c);
    CHECK(g.call("cellVpostExec", 0, kIn, kCtrl, kOut, kInfo) == kEHdlNull);
    CHECK(g.call("cellVpostExec", h + 0x80, kIn, kCtrl, kOut, kInfo) == kEHdlInvalid);
    CHECK(g.call("cellVpostExec", h, 0, kCtrl, kOut, kInfo) == kEInNull);
    CHECK(g.call("cellVpostExec", h, kIn + 0x40, kCtrl, kOut, kInfo) == kEInInvalid);
    CHECK(g.call("cellVpostExec", h, kIn, 0, kOut, kInfo) == kECtrlNull);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, 0, kInfo) == kEOutNull);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut + 0x10, kInfo) == kEOutInvalid);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut, 0) == kEPicInfoNull);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut, kInfo) == 0);
    const auto refused = [&](const Ctrl& x) {
        write_ctrl(b, x);
        return g.call("cellVpostExec", h, kIn, kCtrl, kOut, kInfo) == kECtrlInvalid;
    };
    Ctrl x = c;
    x.in_w = 128; // past the handle's 64
    CHECK(refused(x));
    x = c;
    x.out_w = 30;
    CHECK(refused(x));
    x = c;
    x.exec_type = 8;
    CHECK(refused(x));
    x = c;
    x.scaler = 4;
    CHECK(refused(x));
    x = c;
    x.ipc = 3;
    CHECK(refused(x));
    x = c;
    x.chroma = 2;
    CHECK(refused(x));
    x = c;
    x.in_w = x.in_h = 64;
    x.in_window = {1, 0, 32, 32}; // an odd column
    CHECK(refused(x));
    x.in_window = {34, 0, 32, 32}; // past the picture
    CHECK(refused(x));
    x.in_window = {0, 0, 30, 32}; // narrower than 32
    CHECK(refused(x));
    write_ctrl(b, c);
    ps_write32(b, kCtrl + 92, 1); // reserved2
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut, kInfo) == kECtrlInvalid);
    CHECK(g.call("cellVpostClose", h) == 0);
    write_ctrl(b, c);
    CHECK(g.call("cellVpostExec", h, kIn, kCtrl, kOut, kInfo) == kEHdlInvalid);
}
