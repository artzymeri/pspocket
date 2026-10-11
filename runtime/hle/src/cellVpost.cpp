// cellVpost: the video post-processor (libvpost), pspocket's own (D-564), made to answer and convert as Sony's libvpost
// does (D-913, compared call for call with libvpost.sprx, tests/hle/test_vpost_compare.cpp). RPCS3's
// (rpcs3/Emu/Cell/Modules/cellVpost.cpp/.h) gave the structures and the error codes.
//
// What libvpost is: a PPU wrapper over four SPURS task stages (input, interlace conversion, scaler, colour conversion),
// run one after another for each Exec through two frame buffers in the game's work memory. Here the same work is done on
// the calling thread, with libvpost's checks, answers and arithmetic (each found by running Sony's module on chosen
// pictures; nothing of it in the repository):
// - QueryAttr: inMaxWidth and outMaxWidth even, 32 to 1920; the heights even, 32 to 1088; depths 8 bits, the input
//   YUV 4:2:0 planar, the output RGBA (0) or YUV 4:2:0 planar (1), both reserved words 0. memSize: twice the larger
//   frame (a frame: the width to 16 times the height, plus two chroma planes of half the width to 16 times half the
//   height plus one) plus 0x9FE5; delay 0, versions 0x40000 and 0x05060606.
// - Open makes the handle at the work memory's first 128-byte boundary (its first word itself, which Exec and Close
//   check); a PPU priority past 0xBFF, an SPU priority outside 16 to 255, SPUs other than one or less memory than
//   QueryAttr's is O_ARG_RSRC_INVALID. OpenEx takes a SPURS instance, eight priorities of 0 to 15 (not all 0) and a
//   maximum contention of 1.
// - Exec: the input 128-byte aligned (the Y plane, then U, then V, each its width's stride), the control parameters
//   within the handle's sizes and libvpost's ranges (windows inside their pictures, even, at least 32 square), the output
//   128-byte aligned. The output is the whole picture: the input window scaled into the output window, black (Y 0 or 16,
//   chroma 128) around it.
// - The scaler (when a window or the size changes): see scale_plane.
// - The colour conversion: per output pixel the chroma of its 2x2 block (type A), or between two chroma columns averaged
//   as bytes (type B), then libvpost's fixed point (coefficients in 1/8192 of the chroma, the luma in 1/16384, a sum in
//   2^-20 rounded down); out of range clamped; the alpha outAlpha.

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

#include "firmware_data.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellVpost";
constexpr const char* kChan = "cellVpost";

enum : u32 {
    CELL_VPOST_ERROR_Q_ARG_CFG_NULL = 0x80610410,
    CELL_VPOST_ERROR_Q_ARG_CFG_INVALID = 0x80610411,
    CELL_VPOST_ERROR_Q_ARG_ATTR_NULL = 0x80610412,
    CELL_VPOST_ERROR_O_ARG_CFG_NULL = 0x80610440,
    CELL_VPOST_ERROR_O_ARG_CFG_INVALID = 0x80610441,
    CELL_VPOST_ERROR_O_ARG_RSRC_NULL = 0x80610442,
    CELL_VPOST_ERROR_O_ARG_RSRC_INVALID = 0x80610443,
    CELL_VPOST_ERROR_O_ARG_HDL_NULL = 0x80610444,
    CELL_VPOST_ERROR_C_ARG_HDL_NULL = 0x80610470,
    CELL_VPOST_ERROR_C_ARG_HDL_INVALID = 0x80610471,
    CELL_VPOST_ERROR_E_ARG_HDL_NULL = 0x806104A0,
    CELL_VPOST_ERROR_E_ARG_HDL_INVALID = 0x806104A1,
    CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL = 0x806104A2,
    CELL_VPOST_ERROR_E_ARG_INPICBUF_INVALID = 0x806104A3,
    CELL_VPOST_ERROR_E_ARG_CTRL_NULL = 0x806104A4,
    CELL_VPOST_ERROR_E_ARG_CTRL_INVALID = 0x806104A5,
    CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL = 0x806104A6,
    CELL_VPOST_ERROR_E_ARG_OUTPICBUF_INVALID = 0x806104A7,
    CELL_VPOST_ERROR_E_ARG_PICINFO_NULL = 0x806104A8,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kOutRgba = 0, kOutYuv = 1;
constexpr u32 kHandleWords = 0x4080; // libvpost's second handle word (the size of its control block)

// CellVpostCfgParam {inMaxWidth, inMaxHeight, inDepth, inPicFmt, outMaxWidth, outMaxHeight, outDepth, outPicFmt,
// reserved1, reserved2}.
struct Config {
    std::array<u32, 10> w{};
    u32 in_w() const { return w[0]; }
    u32 in_h() const { return w[1]; }
    u32 out_w() const { return w[4]; }
    u32 out_h() const { return w[5]; }
    u32 out_fmt() const { return w[7]; }
};

Config read_config(u8* base, u32 at)
{
    Config c;
    for (u32 i = 0; i < 10; i++) c.w[i] = ps_read32(base, at + 4 * i);
    return c;
}

bool width_ok(u32 v) { return v - 32 <= 1920 - 32 && !(v & 1); }
bool height_ok(u32 v) { return v - 32 <= 1088 - 32 && !(v & 1); }

bool config_ok(const Config& c)
{
    return width_ok(c.w[0]) && height_ok(c.w[1]) && c.w[2] == 0 && c.w[3] == 0 && width_ok(c.w[4]) &&
           height_ok(c.w[5]) && c.w[6] == 0 && c.w[7] <= 1 && c.w[8] == 0 && c.w[9] == 0;
}

// A YUV 4:2:0 frame as libvpost keeps one in its work memory.
u32 frame_size(u32 w, u32 h)
{
    const u32 y = ((w + 15) & ~15u) * h;
    const u32 c = ((((w + 1) >> 1) + 15) & ~15u) * ((h + 2) >> 1);
    return (y + 2 * c) & ~15u;
}

u32 mem_size(const Config& c)
{
    return 2 * std::max(frame_size(c.in_w(), c.in_h()), frame_size(c.out_w(), c.out_h())) + 0x9FE5;
}

bool in_guest(PPUContext& ctx, u32 addr, u64 size)
{
    if (!addr || u64{addr} + size > 0x100000000ull) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u64 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, static_cast<u32>(addr + at), static_cast<u32>(std::min<u64>(size - at, 0x1000))))
            return false;
    return true;
}

// A handle: libvpost's control block, its first word its own address.
bool is_handle(PPUContext& ctx, u32 h) { return in_guest(ctx, h, 0x80) && ps_read32(guest_base(ctx), h) == h; }

// ------------------------------------------------------------------------------------------------- the pictures

struct Plane {
    u32 w = 0, h = 0;
    std::vector<u8> px;
    u8& at(u32 x, u32 y) { return px[static_cast<usize>(y) * w + x]; }
    u8 at(u32 x, u32 y) const { return px[static_cast<usize>(y) * w + x]; }
};

struct Picture {
    Plane y, u, v;
};

struct Window {
    u32 x = 0, y = 0, w = 0, h = 0;
};

u8 clamp_byte(s64 v) { return static_cast<u8>(std::clamp<s64>(v, 0, 255)); }

// libvpost's filter tables (its scaler's data, read from the installed system software, firmware_data.hpp): seventeen
// horizontal 8-tap filters of 256 phases in 1/4096, chosen by the reduction, and five vertical 4-tap filters of 64
// phases in 1/256 (each tap written twice).
constexpr std::string_view kLibvpostIdentity = "80bd203cbf192a7f13d9317217fa3d5200b9466d3d21417656226ee7a702725f";
constexpr u64 kFiltersVaddr = 0x4EE0;

struct Filters {
    bool tried = false, ok = false;
    std::array<std::array<std::array<s16, 8>, 256>, 17> h{};
    std::array<std::array<std::array<s16, 4>, 64>, 5> v{};
};

// The horizontal table for a window `in` wide scaled to `out` (libvpost: out / in times 16 plus 0.6, as a float).
u32 h_table(u32 in, u32 out)
{
    const float r = static_cast<float>(out) / static_cast<float>(in);
    const u32 idx = static_cast<u32>(static_cast<double>(r) * 16.0 + 0.6);
    return idx <= 1 ? 0 : idx <= 16 ? idx - 1 : 16;
}

// The vertical table (times 4 plus 0.6): 0 and 1 the first, then one each, past 4 the last.
u32 v_table(u32 in, u32 out)
{
    const float r = static_cast<float>(out) / static_cast<float>(in);
    const u32 idx = static_cast<u32>(static_cast<double>(r) * 4.0 + 0.6);
    static constexpr u32 kOrder[5] = {0, 0, 1, 2, 3};
    return idx < 5 ? kOrder[idx] : 4;
}

// The scaler (libvpost's third stage): vertically first, then horizontally.
// - Type 0 (bilinear), and the horizontal pass of type 2: each output a weighted sum of the two samples about its
//   position (weights in 1/256, the sum rounded down; a row past the window's bottom is the picture's, past the
//   picture's its last; a column past the window's right edge its last). Vertical positions are in 1/65536 of a row (the
//   step rounded down), each row's its own. Horizontal ones are in 1/256, sixteen output columns at a time as libvpost
//   writes them (from a multiple of 16 in the output picture): each lane of the first sixteen a step (in 1/256) from the
//   window's first column, each later sixteen 16 steps on (in 1/65536, rounded down to 1/256). A chroma plane is sited
//   between its luma samples (type A horizontally, both types vertically): its positions start (step - 1) / 4 on
//   (rounded down); for windows at multiples of 16 in both pictures, a negative position in the first sixteen is 0.
//   Past a halving horizontally, every second column of the window is taken first.
// - Types 2 and 3 vertically: libvpost's 4-tap filter of the row's phase (its position's 6 bits below the row) over the
//   rows above it to two below, the sum in 1/256 rounded down and clamped; rows above the picture are its first, below
//   it mirrored about its last; an output row whose position reaches the last row is filtered there (its phase 0), the
//   rows below it the last.
// - Type 3 horizontally: libvpost's 8-tap filter of the column's phase (positions in 1/256, each its own from the
//   output's 16-column block, less the window's first lane's whole steps) over the
//   columns three before it to four after, mirrored before the 16-byte block holding the window's first and after its last; the products summed in pairs, each pair's
//   sum in 1/16 (rounded down), the four rounded to the nearest and clamped; a copy where the width is kept.
void scale_plane(const Plane& in_plane, const Window& iw_given, const Window& ow, bool chroma, u32 chroma_type,
    u32 scaler, const Filters* filters, Plane& out)
{
    // The horizontal 8-tap pass is a copy where the width is kept (the vertical filter is applied at any height).
    const bool four_tap = filters && (scaler == 2 || scaler == 3);
    const bool eight_tap = filters && scaler == 3 && iw_given.w != ow.w;
    // Past a halving horizontally, every second column of the window is taken first.
    Plane halved;
    Window iw = iw_given;
    const Plane* src = &in_plane;
    if (!eight_tap && iw.w > 2 * ow.w) {
        halved = {iw.w / 2, in_plane.h, std::vector<u8>(static_cast<usize>(iw.w / 2) * in_plane.h)};
        for (u32 y = 0; y < in_plane.h; y++)
            for (u32 x = 0; x < iw.w / 2; x++) halved.at(x, y) = in_plane.at(iw.x + 2 * x, y);
        iw = {0, iw.y, iw.w / 2, iw.h};
        src = &halved;
    }
    const Plane& in = *src;
    const u32 step_v = static_cast<u32>((u64{iw.h} << 16) / ow.h), step_h = static_cast<u32>((u64{iw.w} << 16) / ow.w);
    const s32 off_v = chroma ? (static_cast<s32>(step_v) - 0x10000) >> 2 : 0;
    const s32 off_h = chroma && chroma_type == 0 ? (static_cast<s32>(step_h) - 0x10000) >> 10 : 0;
    // The vertical pass over the window's columns; for the 8-tap filter from the 16-byte block holding its first, as
    // libvpost reads them (the filter reads before the window into it, mirrored before it; past the window mirrored).
    const u32 col0 = eight_tap ? iw.x & ~15u : iw.x;
    const u32 cols = iw.x + iw.w - col0;
    Plane tmp{cols, ow.h, std::vector<u8>(static_cast<usize>(cols) * ow.h)};
    if (four_tap) {
        const auto& table = filters->v[v_table(iw.h, ow.h)];
        const s32 last = static_cast<s32>(in.h) - 1;
        for (u32 y = 0; y < ow.h; y++) {
            u32 pos = static_cast<u32>(std::max<s64>(s64{y} * step_v + off_v, 0));
            const bool clamped = static_cast<s32>(iw.y + (pos >> 16)) >= last;
            if (clamped) pos = (static_cast<u32>(last) - iw.y) << 16;
            const s32 i = static_cast<s32>(iw.y + (pos >> 16));
            const auto& taps = table[(pos >> 10) & 63];
            s32 rows[4];
            for (s32 k = 0; k < 4; k++) {
                s32 r = i - 1 + k;
                if (r < static_cast<s32>(iw.y)) r = static_cast<s32>(iw.y);
                if (r > last) r = clamped ? last : 2 * last - r;
                rows[k] = r;
            }
            for (u32 x = 0; x < cols; x++) {
                s32 sum = 0;
                for (s32 k = 0; k < 4; k++) sum += taps[k] * in.at(col0 + x, static_cast<u32>(rows[k]));
                tmp.at(x, y) = clamp_byte(sum >> 8);
            }
        }
    } else {
        for (u32 y = 0; y < ow.h; y++) {
            const u32 pos = static_cast<u32>(std::max<s64>(s64{y} * step_v + off_v, 0));
            const u32 i = pos >> 16, f = (pos >> 8) & 0xFF;
            const u32 r0 = std::min(iw.y + i, in.h - 1), r1 = std::min(iw.y + i + 1, in.h - 1);
            for (u32 x = 0; x < cols; x++)
                tmp.at(x, y) = static_cast<u8>((in.at(col0 + x, r0) * (256 - f) + in.at(col0 + x, r1) * f) >> 8);
        }
    }
    out.w = ow.w;
    out.h = ow.h;
    out.px.assign(static_cast<usize>(ow.w) * ow.h, 0);
    if (eight_tap) {
        const auto& table = filters->h[h_table(iw.w, ow.w)];
        const s32 n = static_cast<s32>(cols);
        for (u32 x = 0; x < ow.w; x++) {
            // Positions as libvpost makes them for the sixteen columns from a multiple of 16 in the output picture,
            // moved back by the window's first lane's step (rounded down).
            const u32 f = ow.x & 15;
            const s64 p = (s64{x + f} * step_h >> 8) - s64{f} * (step_h >> 8) + off_h;
            const s32 i = static_cast<s32>(iw.x - col0) + static_cast<s32>(p >> 8);
            const auto& taps = table[static_cast<u64>(p) & 0xFF];
            u32 c[8];
            for (s32 k = 0; k < 8; k++) {
                s32 col = i - 3 + k;
                if (col < 0) col = -col - 1;
                if (col > n - 1) col = 2 * n - 1 - col;
                c[k] = static_cast<u32>(std::clamp(col, 0, n - 1));
            }
            for (u32 y = 0; y < ow.h; y++) {
                s32 sum = 0;
                for (u32 k = 0; k < 8; k += 2)
                    sum += (taps[k] * tmp.at(c[k], y) + taps[k + 1] * tmp.at(c[k + 1], y)) >> 8;
                out.at(x, y) = clamp_byte((sum + 8) >> 4);
            }
        }
        return;
    }
    const s32 block = static_cast<s32>((u64{step_h} * 16) >> 8), step8 = static_cast<s32>(step_h >> 8);
    const u32 first = ow.x & 15;
    std::array<s32, 16> lane{};
    for (u32 k = 0; k < 16; k++) lane[k] = off_h + (static_cast<s32>(k) - static_cast<s32>(first)) * step8;
    if (first == 0 && (iw_given.x & 15) == 0)
        for (s32& l : lane) l = std::max(l, 0);
    for (u32 x = 0; x < ow.w; x++) {
        const u32 a = ow.x + x;
        const s32 p = static_cast<s32>((a >> 4) - (ow.x >> 4)) * block + lane[a & 15];
        const u32 pos = static_cast<u32>(std::max(p, 0));
        const u32 i = pos >> 8, f = pos & 0xFF;
        const u32 c0 = std::min(i, cols - 1), c1 = std::min(i + 1, cols - 1);
        for (u32 y = 0; y < ow.h; y++)
            out.at(x, y) = static_cast<u8>((tmp.at(c0, y) * (256 - f) + tmp.at(c1, y) * f) >> 8);
    }
}

// libvpost's colour conversion: (A * Y + Cu * (U - 128) + Cv * (V - 128) + B) >> 20 for each of R, G, B.
struct Matrix {
    s64 a, b;          // the luma's weight and the sum's constant (the luma's offset and the rounding in it)
    s64 rv, gu, gv, bu; // the chroma's weights
};

Matrix matrix_of(u32 matrix, u32 range)
{
    // In 1/2^20: the luma 2^20 (full range) or 64 * 19076 (broadcast, from 16); the chroma 128 * libvpost's 1/8192.
    const bool broadcast = range == 1;
    const s64 a = broadcast ? 64 * 19076 : 1 << 20;
    const s64 b = broadcast ? 523264 - 16 * a : 1 << 19;
    if (matrix == 1) {
        if (broadcast) return {a, b, 128 * 14690, 128 * -1746, 128 * -4378, 128 * 17322};
        return {a, b, 128 * 12900, 128 * -1534, 128 * -3834, 128 * 15201};
    }
    if (broadcast) return {a, b, 128 * 13078, 128 * -3204, 128 * -6658, 128 * 16520};
    return {a, b, 128 * 11485, 128 * -2818, 128 * -5850, 128 * 14516};
}


// The interlace conversion (libvpost's second stage): the field of `parity` kept, the other's rows made from it. Type 0
// repeats the field's row (above for the top field, below for the bottom), type 1 averages the rows above and below as
// bytes (rounded up), type 2 averages them along the direction of three (left, down, right) whose three pixels about it
// differ least (a diagonal taken only when two or more less than straight down, the left one on a tie); the chroma's
// rows the field's alike, averaged straight for types 1 and 2. Rows past the picture's edge are the field's nearest.
void deinterlace_plane(const Plane& in, u32 parity, u32 type, bool luma, Plane& out)
{
    out = in;
    const u32 w = in.w, h = in.h;
    const auto at = [&](u32 row, s32 x) { return static_cast<s32>(in.at(static_cast<u32>(std::clamp<s32>(x, 0, static_cast<s32>(w) - 1)), row)); };
    for (u32 r = 0; r < h; r++) {
        if ((r & 1) == parity) continue;
        const bool has_up = r >= 1, has_down = r + 1 < h;
        if (type == 0 || !has_up || !has_down) {
            const u32 from = type == 0 ? (parity == 0 ? (has_up ? r - 1 : r + 1) : (has_down ? r + 1 : r - 1))
                                       : (has_up ? r - 1 : r + 1);
            for (u32 x = 0; x < w; x++) out.at(x, r) = in.at(x, from);
            continue;
        }
        const u32 up = r - 1, down = r + 1;
        for (u32 x = 0; x < w; x++) {
            s32 d = 0;
            if (type == 2 && luma) {
                const auto cost = [&](s32 dir) {
                    s32 c = 0;
                    for (s32 i = -1; i <= 1; i++)
                        c += std::abs(at(up, static_cast<s32>(x) + dir + i) - at(down, static_cast<s32>(x) - dir + i));
                    return c;
                };
                const s32 c0 = cost(0), cl = cost(-1), cr = cost(1);
                const s32 dd = cl <= cr ? -1 : 1, cd = cl <= cr ? cl : cr;
                if (cd + 2 < c0) d = dd;
            }
            out.at(x, r) = static_cast<u8>((at(up, static_cast<s32>(x) + d) + at(down, static_cast<s32>(x) - d) + 1) >> 1);
        }
    }
}

Picture deinterlace(const Picture& in, u32 parity, u32 type)
{
    Picture out;
    deinterlace_plane(in.y, parity, type, true, out.y);
    deinterlace_plane(in.u, parity, type, false, out.u);
    deinterlace_plane(in.v, parity, type, false, out.v);
    return out;
}

// The picture the output is made from: the input as it is (the same size, no window), else black (Y 0, or 16 in
// broadcast range; chroma 128) with the input's window scaled into the output's.
Picture frame_of(const Picture& in, bool same, const Window& iw, const Window& ow, u32 out_w, u32 out_h, u32 range,
    u32 chroma, u32 scaler, const Filters* filters)
{
    if (same) return in;
    Picture out;
    const u8 black = range == 1 ? 16 : 0;
    out.y = {out_w, out_h, std::vector<u8>(static_cast<usize>(out_w) * out_h, black)};
    out.u = {out_w / 2, out_h / 2, std::vector<u8>(static_cast<usize>(out_w / 2) * (out_h / 2), 128)};
    out.v = out.u;
    Plane scaled;
    scale_plane(in.y, iw, ow, false, chroma, scaler, filters, scaled);
    for (u32 y = 0; y < ow.h; y++) std::memcpy(&out.y.at(ow.x, ow.y + y), &scaled.at(0, y), ow.w);
    const Window ciw{iw.x / 2, iw.y / 2, iw.w / 2, iw.h / 2}, cow{ow.x / 2, ow.y / 2, ow.w / 2, ow.h / 2};
    for (auto [src, dst] : {std::pair{&in.u, &out.u}, std::pair{&in.v, &out.v}}) {
        scale_plane(*src, ciw, cow, true, chroma, scaler, filters, scaled);
        for (u32 y = 0; y < cow.h; y++) std::memcpy(&dst->at(cow.x, cow.y + y), &scaled.at(0, y), cow.w);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- the calls

// cellVpostQueryAttr(cfgParam, attr): CellVpostAttr {memSize, delay (u8), vpostVerUpper (+8), vpostVerLower (+12)}.
s32 cellVpostQueryAttr(PPUContext& ctx, u32 cfg, u32 attr)
{
    u8* base = guest_base(ctx);
    if (!cfg) return error(CELL_VPOST_ERROR_Q_ARG_CFG_NULL);
    if (!in_guest(ctx, cfg, 40)) return error(CELL_VPOST_ERROR_Q_ARG_CFG_INVALID);
    const Config c = read_config(base, cfg);
    if (!config_ok(c)) return error(CELL_VPOST_ERROR_Q_ARG_CFG_INVALID);
    if (!attr || !in_guest(ctx, attr, 16)) return error(CELL_VPOST_ERROR_Q_ARG_ATTR_NULL);
    ps_write32(base, attr, mem_size(c));
    ps_write8(base, attr + 4, 0);
    ps_write32(base, attr + 8, 0x40000);
    ps_write32(base, attr + 12, 0x05060606);
    return lv2::CELL_OK;
}

// The handle made in the work memory: its own address, the block's size, the configuration (+0x24) and the
// resource's memory (+0x4C, +0x50), as libvpost lays them out.
u32 make_handle(u8* base, u32 cfg, u32 mem, u32 size)
{
    const u32 h = (mem + 0x7F) & ~0x7Fu;
    ps_write32(base, h, h);
    ps_write32(base, h + 4, kHandleWords);
    for (u32 i = 0; i < 10; i++) ps_write32(base, h + 0x24 + 4 * i, ps_read32(base, cfg + 4 * i));
    ps_write32(base, h + 0x4C, mem);
    ps_write32(base, h + 0x50, size);
    return h;
}

// cellVpostOpen(cfgParam, CellVpostResource {memAddr, memSize, ppuThreadPriority, ppuThreadStackSize,
// spuThreadPriority, numOfSpus}, handle *).
s32 cellVpostOpen(PPUContext& ctx, u32 cfg, u32 resource, u32 handle)
{
    u8* base = guest_base(ctx);
    if (!cfg) return error(CELL_VPOST_ERROR_O_ARG_CFG_NULL);
    if (!in_guest(ctx, cfg, 40) || !config_ok(read_config(base, cfg))) return error(CELL_VPOST_ERROR_O_ARG_CFG_INVALID);
    if (!resource || !in_guest(ctx, resource, 24)) return error(CELL_VPOST_ERROR_O_ARG_RSRC_NULL);
    const u32 mem = ps_read32(base, resource), size = ps_read32(base, resource + 4);
    const u32 ppu = ps_read32(base, resource + 8), spu = ps_read32(base, resource + 16);
    const u32 spus = ps_read32(base, resource + 20);
    const Config c = read_config(base, cfg);
    if (!mem || ppu >= 0xC00 || spu - 16 >= 0xF0 || spus != 1 || size < mem_size(c) || !in_guest(ctx, mem, size))
        return error(CELL_VPOST_ERROR_O_ARG_RSRC_INVALID);
    if (!handle || !in_guest(ctx, handle, 4)) return error(CELL_VPOST_ERROR_O_ARG_HDL_NULL);
    const u32 h = make_handle(base, cfg, mem, size);
    ps_write32(base, handle, h);
    PS_LOG_INFO(kChan, "cellVpostOpen: handle {:#x}, in up to {}x{}, out up to {}x{} {}", h, c.in_w(), c.in_h(),
        c.out_w(), c.out_h(), c.out_fmt() == kOutRgba ? "RGBA" : "YUV420 planar");
    return lv2::CELL_OK;
}

// cellVpostOpenEx(cfgParam, CellVpostResourceEx {memAddr, memSize, spurs, priority[8] (u8), maxContention}, handle *).
s32 cellVpostOpenEx(PPUContext& ctx, u32 cfg, u32 resource, u32 handle)
{
    u8* base = guest_base(ctx);
    if (!cfg) return error(CELL_VPOST_ERROR_O_ARG_CFG_NULL);
    if (!in_guest(ctx, cfg, 40) || !config_ok(read_config(base, cfg))) return error(CELL_VPOST_ERROR_O_ARG_CFG_INVALID);
    if (!resource || !in_guest(ctx, resource, 24)) return error(CELL_VPOST_ERROR_O_ARG_RSRC_NULL);
    const u32 mem = ps_read32(base, resource), size = ps_read32(base, resource + 4);
    const u32 spurs = ps_read32(base, resource + 8), contention = ps_read32(base, resource + 20);
    bool any = false, bad = false;
    for (u32 i = 0; i < 8; i++) {
        const u8 p = ps_read8(base, resource + 12 + i);
        any = any || p != 0;
        bad = bad || p > 15;
    }
    const Config c = read_config(base, cfg);
    if (!mem || !spurs || bad || !any || contention != 1 || size < mem_size(c) || !in_guest(ctx, mem, size))
        return error(CELL_VPOST_ERROR_O_ARG_RSRC_INVALID);
    if (!handle || !in_guest(ctx, handle, 4)) return error(CELL_VPOST_ERROR_O_ARG_HDL_NULL);
    const u32 h = make_handle(base, cfg, mem, size);
    ps_write32(base, handle, h);
    PS_LOG_INFO(kChan, "cellVpostOpenEx: handle {:#x} (SPURS {:#x}: pspocket converts on the caller's thread)", h, spurs);
    return lv2::CELL_OK;
}

s32 cellVpostClose(PPUContext& ctx, u32 handle)
{
    if (!handle) return error(CELL_VPOST_ERROR_C_ARG_HDL_NULL);
    if (!is_handle(ctx, handle)) return error(CELL_VPOST_ERROR_C_ARG_HDL_INVALID);
    u8* base = guest_base(ctx);
    ps_write32(base, handle, 0);
    ps_write32(base, handle + 4, 0);
    ps_write32(base, handle + 0x20, 0x7FFFFFFF);
    return lv2::CELL_OK;
}

// A window inside its picture: even, at least 32 square.
bool window_ok(const Window& w, u32 width, u32 height)
{
    return w.x < width && w.y < height && w.w >= 32 && w.h >= 32 && !(w.x & 1) && !(w.y & 1) && !(w.w & 1) &&
           !(w.h & 1) && w.x + w.w <= width && w.y + w.h <= height;
}

struct State {
    std::mutex mutex;
    bool exec_type_logged = false, scaler_logged = false;
    Filters filters;
};

// libvpost's filters, read at the first picture that needs them; null without the firmware's module.
const Filters* filters_of(State& s)
{
    std::lock_guard lock(s.mutex);
    Filters& f = s.filters;
    if (!f.tried) {
        f.tried = true;
        const auto bytes = firmware_module_data(*HleProcess::current(), "libvpost.sprx", kLibvpostIdentity,
            kFiltersVaddr, 0x12400);
        if (bytes) {
            const auto be16 = [&](usize at) { return static_cast<s16>(((*bytes)[at] << 8) | (*bytes)[at + 1]); };
            for (usize t = 0; t < 17; t++)
                for (usize ph = 0; ph < 256; ph++)
                    for (usize k = 0; k < 8; k++) f.h[t][ph][k] = be16(t * 0x1000 + ph * 16 + k * 2);
            for (usize t = 0; t < 5; t++)
                for (usize ph = 0; ph < 64; ph++)
                    for (usize k = 0; k < 4; k++) f.v[t][ph][k] = be16(0x11000 + t * 0x400 + ph * 16 + k * 4);
            f.ok = true;
        }
    }
    return f.ok ? &f : nullptr;
}

State& state() { return HleProcess::current()->state<State>(); }

// cellVpostExec(handle, inPicBuff, ctrlParam, outPicBuff, picInfo). CellVpostCtrlParam: execType, scalerType,
// ipcType, inWidth, inHeight, inChromaPosType, inQuantRange, inColorMatrix, inWindow {x, y, width, height}, outWidth,
// outHeight, outWindow, outAlpha (u8, +72), userData (u64, +80), reserved1, reserved2 (+88, +92).
s32 cellVpostExec(PPUContext& ctx, u32 handle, u32 in_pic, u32 ctrl, u32 out_pic, u32 info)
{
    u8* base = guest_base(ctx);
    if (!handle) return error(CELL_VPOST_ERROR_E_ARG_HDL_NULL);
    if (!is_handle(ctx, handle)) return error(CELL_VPOST_ERROR_E_ARG_HDL_INVALID);
    if (!in_pic) return error(CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL);
    if (in_pic & 0x7F) return error(CELL_VPOST_ERROR_E_ARG_INPICBUF_INVALID);
    if (!ctrl) return error(CELL_VPOST_ERROR_E_ARG_CTRL_NULL);
    if (!in_guest(ctx, ctrl, 96)) return error(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
    const Config c = read_config(base, handle + 0x24);
    std::array<u32, 24> p{};
    for (u32 i = 0; i < 24; i++) p[i] = ps_read32(base, ctrl + 4 * i);
    const u32 exec_type = p[0], scaler = p[1], ipc = p[2], in_w = p[3], in_h = p[4], chroma = p[5], range = p[6],
              matrix = p[7], out_w = p[12], out_h = p[13];
    const Window iw{p[8], p[9], p[10], p[11]}, ow{p[14], p[15], p[16], p[17]};
    if (exec_type >= 8 || scaler >= 4 || ipc >= 3 || !width_ok(in_w) || !height_ok(in_h) || chroma >= 2 ||
        range >= 2 || matrix >= 2 || !window_ok(iw, in_w, in_h) || !width_ok(out_w) || !height_ok(out_h) ||
        !window_ok(ow, out_w, out_h) || in_w > c.in_w() || in_h > c.in_h() || out_w > c.out_w() || out_h > c.out_h() ||
        p[22] != 0 || p[23] != 0)
        return error(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
    if (!out_pic) return error(CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL);
    if (out_pic & 0x7F) return error(CELL_VPOST_ERROR_E_ARG_OUTPICBUF_INVALID);
    if (!info) return error(CELL_VPOST_ERROR_E_ARG_PICINFO_NULL);
    const u32 fmt = c.out_fmt();
    const u64 in_size = u64{in_w} * in_h + 2 * (u64{in_w / 2} * (in_h / 2));
    const u64 out_size = fmt == kOutRgba ? u64{out_w} * out_h * 4
                                         : (u64{out_w} * out_h + 2 * (u64{out_w / 2} * (out_h / 2)) + 15) & ~u64{15};
    if (!in_guest(ctx, in_pic, in_size) || !in_guest(ctx, out_pic, out_size) || !in_guest(ctx, info, 88)) {
        log_call_site(ctx, base, "cellVpostExec: a picture is not in guest memory");
        return error(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
    }
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        if (ipc == 2 && (exec_type >= 3) && !s.exec_type_logged) {
            s.exec_type_logged = true;
            PS_LOG_INFO(kChan, "cellVpostExec: interlace conversion type 2: libvpost's edge-adaptive average, a few "
                               "pixels of a picture may differ (D-913)");
        }
    }
    State& st = state();
    const Filters* filters = scaler >= 2 ? filters_of(st) : nullptr;
    if (scaler >= 2 && !filters) {
        std::lock_guard lock(st.mutex);
        if (!st.scaler_logged) {
            st.scaler_logged = true;
            PS_LOG_WARN(kChan, "cellVpostExec: scaler type {}: no libvpost.sprx of firmware 4.92 installed for its "
                               "filters: scaled with the bilinear one (D-913)",
                scaler);
        }
    }

    // The input planes.
    Picture in;
    in.y = {in_w, in_h, std::vector<u8>(base + in_pic, base + in_pic + static_cast<usize>(in_w) * in_h)};
    const u32 cw = in_w / 2, ch = in_h / 2;
    const u8* u_at = base + in_pic + static_cast<usize>(in_w) * in_h;
    in.u = {cw, ch, std::vector<u8>(u_at, u_at + static_cast<usize>(cw) * ch)};
    in.v = {cw, ch, std::vector<u8>(u_at + static_cast<usize>(cw) * ch, u_at + 2 * static_cast<usize>(cw) * ch)};

    const bool same = iw.x == 0 && iw.y == 0 && ow.x == 0 && ow.y == 0 && iw.w == in_w && iw.h == in_h && ow.w == out_w &&
                      ow.h == out_h && iw.w == ow.w && iw.h == ow.h;
    // libvpost's stages per exec type: a field picture in (3 to 7) is made a frame by the interlace conversion when it
    // becomes one (3, 4) or is scaled (5 to 7); a field picture out (1, 2, 5 to 7) is the output's rows of its parity
    // only (the others left as they are); IFRM_IFRM is ITOP_ITOP, then IBTM_IBTM.
    struct Pass {
        u32 parity;     // the field's (0 top, 1 bottom)
        bool field_in;  // the input is a field of an interlaced picture
        bool field_out; // only the output's rows of the parity are written
    };
    std::vector<Pass> passes;
    switch (exec_type) {
    case 0: passes = {{0, false, false}}; break;
    case 1: passes = {{0, false, true}}; break;
    case 2: passes = {{1, false, true}}; break;
    case 3: passes = {{0, true, false}}; break;
    case 4: passes = {{1, true, false}}; break;
    case 5: passes = {{0, true, true}, {1, true, true}}; break;
    case 6: passes = {{0, true, true}}; break;
    default: passes = {{1, true, true}}; break;
    }
    u8* o = base + out_pic;
    const Matrix m = matrix_of(matrix, range);
    const u8 alpha = ps_read8(base, ctrl + 72);
    for (const Pass& pass : passes) {
        const bool interlace = pass.field_in && (!pass.field_out || !same);
        const Picture src = interlace ? deinterlace(in, pass.parity, ipc) : in;
        const Picture out = frame_of(src, same, iw, ow, out_w, out_h, range, chroma, scaler, filters);
        // The chroma rows a pixel row takes: the frame's, or (a field kept as one) its field's.
        const bool field_chroma = pass.field_in && !interlace;
        const auto wanted = [&](u32 row) { return !pass.field_out || (row & 1) == pass.parity; };
        if (fmt == kOutYuv) {
            for (u32 y = 0; y < out_h; y++)
                if (wanted(y)) std::memcpy(o + static_cast<usize>(y) * out_w, out.y.px.data() + static_cast<usize>(y) * out_w, out_w);
            u8* ou = o + static_cast<usize>(out_w) * out_h;
            u8* ov = ou + static_cast<usize>(out_w / 2) * (out_h / 2);
            for (u32 y = 0; y < out_h / 2; y++)
                if (wanted(y)) {
                    std::memcpy(ou + static_cast<usize>(y) * (out_w / 2), out.u.px.data() + static_cast<usize>(y) * (out_w / 2), out_w / 2);
                    std::memcpy(ov + static_cast<usize>(y) * (out_w / 2), out.v.px.data() + static_cast<usize>(y) * (out_w / 2), out_w / 2);
                }
            // libvpost writes its chroma sixteen rows at a time from a buffer of sixteen, in 16-byte transfers: past a
            // picture whose size is no multiple of 16 go the rest of the last transfer, the first bytes of the row
            // sixteen above the last row's successor (still in the buffer).
            const usize size = static_cast<usize>(out_w) * out_h + 2 * static_cast<usize>(out_w / 2) * (out_h / 2);
            const usize pad = ((size + 15) & ~usize{15}) - size;
            if (!pass.field_out && pad && out_h / 2 >= 16) {
                const usize row = out_h / 2 - 16;
                std::memcpy(ov + static_cast<usize>(out_w / 2) * (out_h / 2), out.v.px.data() + row * (out_w / 2), pad);
            }
            continue;
        }
        const u32 ocw = out_w / 2, och = out_h / 2;
        for (u32 y = 0; y < out_h; y++) {
            if (!wanted(y)) continue;
            const u32 cy = std::min(field_chroma ? 2 * (y >> 2) + pass.parity : y >> 1, och - 1);
            for (u32 x = 0; x < out_w; x++) {
                const u32 cx = x / 2;
                s64 u = out.u.at(cx, cy), v = out.v.at(cx, cy);
                if (chroma == 1 && (x & 1)) {
                    const u32 nx = std::min(cx + 1, ocw - 1);
                    u = (u + out.u.at(nx, cy) + 1) >> 1;
                    v = (v + out.v.at(nx, cy) + 1) >> 1;
                }
                const s64 yy = m.a * out.y.at(x, y) + m.b;
                u8* px = o + (static_cast<usize>(y) * out_w + x) * 4;
                px[0] = clamp_byte((yy + m.rv * (v - 128)) >> 20);
                px[1] = clamp_byte((yy + m.gu * (u - 128) + m.gv * (v - 128)) >> 20);
                px[2] = clamp_byte((yy + m.bu * (u - 128)) >> 20);
                px[3] = alpha;
            }
        }
    }

    // The picture information: what was asked and what the exec type makes of it.
    static constexpr u32 kInScan[8] = {0, 0, 0, 1, 1, 1, 1, 1}, kInStruct[8] = {0, 0, 0, 2, 3, 1, 2, 3};
    static constexpr u32 kOutScan[8] = {0, 1, 1, 0, 0, 1, 1, 1}, kOutStruct[8] = {0, 2, 3, 0, 0, 1, 2, 3};
    const u32 words[18] = {in_w, in_h, c.w[2], kInScan[exec_type], c.w[3], chroma, kInStruct[exec_type], range, matrix,
        out_w, out_h, c.w[6], kOutScan[exec_type], fmt, chroma, kOutStruct[exec_type], fmt == kOutRgba ? 0 : range,
        matrix};
    for (u32 i = 0; i < 18; i++) ps_write32(base, info + 4 * i, words[i]);
    ps_write64(base, info + 72, ps_read64(base, ctrl + 80));
    ps_write32(base, info + 80, 0);
    ps_write32(base, info + 84, 0);
    return lv2::CELL_OK;
}

} // namespace

void register_cellVpost(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x95E788C3, cellVpostQueryAttr),
        PS_HLE_FUNC(0xCD33F3E2, cellVpostOpen),
        PS_HLE_FUNC(0x40524325, cellVpostOpenEx),
        PS_HLE_FUNC(0x10EF39F6, cellVpostClose),
        PS_HLE_FUNC(0xABB8CC3D, cellVpostExec),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
