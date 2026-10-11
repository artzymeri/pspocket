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

// The bilinear scaler (type 0): vertically first, then horizontally, each output a weighted sum of the two samples
// about its position (weights in 1/256, the sum rounded down; a row past the window's bottom is the picture's, past the
// picture's its last; a column past the window's right edge its last).
// Vertical positions are in 1/65536 of a row (the step rounded down), each row's its own. Horizontal ones are in 1/256,
// sixteen output columns at a time as libvpost writes them (from a multiple of 16 in the output picture): each lane of
// the first sixteen a step (in 1/256) from the window's first column, each later sixteen 16 steps on (in 1/65536,
// rounded down to 1/256). A chroma plane is sited between its luma samples (type A horizontally, both types
// vertically): its positions start (step - 1) / 4 on
// (rounded down); a negative start for a window at a multiple of 16 in the output is 0.
void scale_plane(const Plane& in_plane, const Window& iw_given, const Window& ow, bool chroma, u32 chroma_type, Plane& out)
{
    // Past a halving horizontally, every second column of the window is taken first.
    Plane halved;
    Window iw = iw_given;
    const Plane* src = &in_plane;
    if (iw.w > 2 * ow.w) {
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
    const u32 cols = iw.w;
    Plane tmp{cols, ow.h, std::vector<u8>(static_cast<usize>(cols) * ow.h)};
    for (u32 y = 0; y < ow.h; y++) {
        const u32 pos = static_cast<u32>(std::max<s64>(s64{y} * step_v + off_v, 0));
        const u32 i = pos >> 16, f = (pos >> 8) & 0xFF;
        const u32 r0 = std::min(iw.y + i, in.h - 1), r1 = std::min(iw.y + i + 1, in.h - 1);
        for (u32 x = 0; x < cols; x++)
            tmp.at(x, y) = static_cast<u8>((in.at(iw.x + x, r0) * (256 - f) + in.at(iw.x + x, r1) * f) >> 8);
    }
    out.w = ow.w;
    out.h = ow.h;
    out.px.assign(static_cast<usize>(ow.w) * ow.h, 0);
    const s32 block = static_cast<s32>((u64{step_h} * 16) >> 8), step8 = static_cast<s32>(step_h >> 8);
    const u32 first = ow.x & 15;
    std::array<s32, 16> lane{};
    for (u32 k = 0; k < 16; k++) lane[k] = off_h + (static_cast<s32>(k) - static_cast<s32>(first)) * step8;
    if (first == 0 && lane[0] < 0) lane[0] = 0;
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

u8 clamp_byte(s64 v) { return static_cast<u8>(std::clamp<s64>(v, 0, 255)); }

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
};

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
    const u64 out_size = fmt == kOutRgba ? u64{out_w} * out_h * 4 : u64{out_w} * out_h + 2 * (u64{out_w / 2} * (out_h / 2));
    if (!in_guest(ctx, in_pic, in_size) || !in_guest(ctx, out_pic, out_size) || !in_guest(ctx, info, 88)) {
        log_call_site(ctx, base, "cellVpostExec: a picture is not in guest memory");
        return error(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
    }
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        if (exec_type != 0 && !s.exec_type_logged) {
            s.exec_type_logged = true;
            PS_LOG_WARN(kChan, "cellVpostExec: exec type {} (fields): converted as a frame, not as libvpost's interlace "
                               "conversion (D-913)",
                exec_type);
        }
        if (scaler != 0 && !s.scaler_logged) {
            s.scaler_logged = true;
            PS_LOG_WARN(kChan, "cellVpostExec: scaler type {}: scaled with libvpost's bilinear one, not its own (D-913)",
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

    // The output's planes: black around the window, the input's window scaled into it.
    const bool same = iw.x == 0 && iw.y == 0 && ow.x == 0 && ow.y == 0 && iw.w == in_w && iw.h == in_h && ow.w == out_w &&
                      ow.h == out_h && iw.w == ow.w && iw.h == ow.h;
    Picture out;
    if (same) {
        out = in;
    } else {
        const u8 black = range == 1 ? 16 : 0;
        out.y = {out_w, out_h, std::vector<u8>(static_cast<usize>(out_w) * out_h, black)};
        out.u = {out_w / 2, out_h / 2, std::vector<u8>(static_cast<usize>(out_w / 2) * (out_h / 2), 128)};
        out.v = out.u;
        Plane scaled;
        scale_plane(in.y, iw, ow, false, chroma, scaled);
        for (u32 y = 0; y < ow.h; y++)
            std::memcpy(&out.y.at(ow.x, ow.y + y), &scaled.at(0, y), ow.w);
        const Window ciw{iw.x / 2, iw.y / 2, iw.w / 2, iw.h / 2}, cow{ow.x / 2, ow.y / 2, ow.w / 2, ow.h / 2};
        for (auto [src, dst] : {std::pair{&in.u, &out.u}, std::pair{&in.v, &out.v}}) {
            scale_plane(*src, ciw, cow, true, chroma, scaled);
            for (u32 y = 0; y < cow.h; y++)
                std::memcpy(&dst->at(cow.x, cow.y + y), &scaled.at(0, y), cow.w);
        }
    }

    u8* o = base + out_pic;
    if (fmt == kOutYuv) {
        std::memcpy(o, out.y.px.data(), out.y.px.size());
        std::memcpy(o + out.y.px.size(), out.u.px.data(), out.u.px.size());
        std::memcpy(o + out.y.px.size() + out.u.px.size(), out.v.px.data(), out.v.px.size());
    } else {
        const Matrix m = matrix_of(matrix, range);
        const u8 alpha = ps_read8(base, ctrl + 72);
        const u32 ocw = out_w / 2;
        for (u32 y = 0; y < out_h; y++)
            for (u32 x = 0; x < out_w; x++) {
                const u32 cx = x / 2, cy = y / 2;
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
