// cellVpost: the video post-processor (libvpost), pspocket's own (D-564). It scales a decoded YUV 4:2:0 picture and
// converts it to RGBA for the game to draw, as games do with the pictures cellVdec gives them.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellVpost.cpp and cellVpost.h: the structures, the error codes, the handles
// (idm IDs from 1), QueryAttr's answers, the picture information Exec writes, the input layout (the Y plane, then U,
// then V, each with its width's stride) and RPCS3's output (RGBA bytes, the alpha outAlpha, scaled with libswscale's
// bilinear filter). No Sony module was at hand to compare with: the library is firmware-first (D-563).
// Beyond RPCS3, which leaves them out:
// - the input's colour matrix (BT.601 or BT.709) and quantisation range (full, or broadcast: Y 16-235, C 16-240)
//   convert it, where libswscale's default is BT.601 broadcast whatever the game asks;
// - the windows: inWindow is the part of the input scaled, outWindow where it goes in the output; a window with no
//   width or height is the whole picture, one past the picture's edge is cut to it; output pixels outside outWindow
//   are left as they were;
// - outPicFmt YUV420_PLANAR gives the scaled planes, without conversion;
// - the chroma position: type A has a chroma sample at every second luma column (MPEG-2's), type B between two
//   (MPEG-1's); vertically both are between two lines;
// - the scaler is a tent filter as wide as the scaling (bilinear interpolation when enlarging, an average of what each
//   output pixel covers when reducing), libswscale's bilinear; the other scaler types and the interlace conversions
//   are done the same way, the picture always taken as a whole frame (as RPCS3), and logged once;
// - the arguments are checked as the error codes name them: null pointers, a configuration with formats libvpost has
//   not, sizes past the configuration's maxima, buffers outside guest memory.
// Exec runs on the calling thread; nothing is written to the work memory the game gives Open.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "idm.hpp"
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
    CELL_VPOST_ERROR_O_ARG_HDL_NULL = 0x80610444,
    CELL_VPOST_ERROR_O_FATAL_CREATEMON_FAIL = 0x80610461,
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

// The enumerations' values.
constexpr s32 kDepth8 = 0, kInYuv420Planar = 0, kOutRgbaIlv = 0, kOutYuv420Planar = 1;
constexpr s32 kStructPfrm = 0, kStructIfrm = 1, kStructItop = 2, kStructIbtm = 3;
constexpr s32 kScanP = 0, kScanI = 1;
constexpr s32 kChromaTypeA = 0;
constexpr s32 kRangeFull = 0;
constexpr s32 kMatrixBt709 = 1;

// RPCS3's QueryAttr answers.
constexpr u32 kMemSize = 4 * 1024 * 1024, kVerUpper = 0x260000, kVerLower = 0x280000;

// CellVpostCfgParam {inMaxWidth, inMaxHeight, inDepth, inPicFmt, outMaxWidth, outMaxHeight, outDepth, outPicFmt,
// reserved1, reserved2}.
struct Config {
    u32 in_max_w = 0, in_max_h = 0, out_max_w = 0, out_max_h = 0;
    s32 out_fmt = 0;
};

struct Instance {
    Config cfg;
};

struct State {
    std::mutex mutex;
    IdTable<Instance, 1, 1023> instances;
    bool exec_type_logged = false, scaler_logged = false, window_logged = false;
};

State& state() { return HleProcess::current()->state<State>(); }

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// Whether [addr, addr + size) is committed guest memory, page by page.
bool in_guest(PPUContext& ctx, u32 addr, u64 size)
{
    if (!addr || u64{addr} + size > 0x100000000ull) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u64 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, static_cast<u32>(addr + at), static_cast<u32>(std::min<u64>(size - at, 0x1000))))
            return false;
    return true;
}

// The configuration at cfg, or false when libvpost has nothing of what it asks for.
bool read_config(u8* base, u32 cfg, Config& out)
{
    const s32 in_depth = static_cast<s32>(ps_read32(base, cfg + 8));
    const s32 in_fmt = static_cast<s32>(ps_read32(base, cfg + 12));
    const s32 out_depth = static_cast<s32>(ps_read32(base, cfg + 24));
    out.in_max_w = ps_read32(base, cfg);
    out.in_max_h = ps_read32(base, cfg + 4);
    out.out_max_w = ps_read32(base, cfg + 16);
    out.out_max_h = ps_read32(base, cfg + 20);
    out.out_fmt = static_cast<s32>(ps_read32(base, cfg + 28));
    return in_depth == kDepth8 && in_fmt == kInYuv420Planar && out_depth == kDepth8 &&
           (out.out_fmt == kOutRgbaIlv || out.out_fmt == kOutYuv420Planar) && out.in_max_w && out.in_max_h &&
           out.out_max_w && out.out_max_h;
}

// ------------------------------------------------------------------------------------------------- scaling

// For each output sample, the input samples it is made of and their weights: a tent filter centred where the output
// sample's centre falls in the input, as wide as an output sample (at least one input sample). `first` and `count`
// are the input samples the window holds; `centre(o)` maps output sample o to input coordinates.
struct Taps {
    std::vector<u32> start, count;
    std::vector<f32> weight; // max_taps a sample
    u32 max_taps = 0;
};

template <class Centre>
void make_taps(Taps& t, u32 out_count, f32 step, u32 first, u32 count, Centre centre)
{
    const f32 radius = std::max(1.0f, step);
    t.max_taps = static_cast<u32>(std::ceil(2.0f * radius)) + 1;
    t.start.assign(out_count, 0);
    t.count.assign(out_count, 0);
    t.weight.assign(static_cast<usize>(out_count) * t.max_taps, 0.0f);
    const s64 lo = first, hi = s64{first} + count - 1;
    std::vector<std::pair<s64, f32>> w;
    for (u32 o = 0; o < out_count; o++) {
        const f32 c = centre(o);
        const s64 k0 = static_cast<s64>(std::floor(c - radius)) + 1, k1 = static_cast<s64>(std::ceil(c + radius)) - 1;
        // Taps past the window's edge fold onto its edge sample.
        f32 sum = 0.0f;
        w.clear();
        for (s64 k = k0; k <= k1; k++) {
            const f32 wk = 1.0f - std::fabs(static_cast<f32>(k) - c) / radius;
            if (wk <= 0.0f) continue;
            w.emplace_back(std::clamp(k, lo, hi), wk);
            sum += wk;
        }
        if (w.empty()) w.emplace_back(std::clamp(static_cast<s64>(std::lround(c)), lo, hi), sum = 1.0f);
        std::sort(w.begin(), w.end());
        const s64 s0 = w.front().first;
        const u32 n = static_cast<u32>(w.back().first - s0 + 1);
        t.start[o] = static_cast<u32>(s0);
        t.count[o] = std::min(n, t.max_taps);
        f32* dst = &t.weight[static_cast<usize>(o) * t.max_taps];
        for (const auto& [k, wk] : w) {
            const s64 i = k - s0;
            if (i < t.max_taps) dst[i] += wk / sum;
        }
    }
}

// One plane's window scaled: in (stride `stride`) through the horizontal taps, then the vertical ones, into out
// (out_w x out_h floats, 0-255).
void scale_plane(const u8* in, u32 stride, const Taps& h, const Taps& v, u32 out_w, u32 out_h, std::vector<f32>& tmp,
    std::vector<f32>& out)
{
    u32 row_lo = ~0u, row_hi = 0;
    for (u32 y = 0; y < out_h; y++) {
        row_lo = std::min(row_lo, v.start[y]);
        row_hi = std::max(row_hi, v.start[y] + v.count[y]);
    }
    const u32 rows = row_hi - row_lo;
    tmp.resize(static_cast<usize>(rows) * out_w);
    for (u32 r = 0; r < rows; r++) {
        const u8* src = in + static_cast<usize>(row_lo + r) * stride;
        f32* dst = &tmp[static_cast<usize>(r) * out_w];
        for (u32 x = 0; x < out_w; x++) {
            const f32* w = &h.weight[static_cast<usize>(x) * h.max_taps];
            const u8* s = src + h.start[x];
            f32 acc = 0.0f;
            for (u32 k = 0; k < h.count[x]; k++) acc += w[k] * static_cast<f32>(s[k]);
            dst[x] = acc;
        }
    }
    out.resize(static_cast<usize>(out_w) * out_h);
    for (u32 y = 0; y < out_h; y++) {
        const f32* w = &v.weight[static_cast<usize>(y) * v.max_taps];
        f32* dst = &out[static_cast<usize>(y) * out_w];
        std::fill(dst, dst + out_w, 0.0f);
        for (u32 k = 0; k < v.count[y]; k++) {
            const f32* src = &tmp[static_cast<usize>(v.start[y] + k - row_lo) * out_w];
            for (u32 x = 0; x < out_w; x++) dst[x] += w[k] * src[x];
        }
    }
}

u8 to_byte(f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 255.0f))); }

struct Window {
    u32 x = 0, y = 0, w = 0, h = 0;
};

// A window as Exec uses it: the whole picture when it has no size, cut to the picture.
Window fit(Window w, u32 width, u32 height, bool& adjusted)
{
    if (!w.w || !w.h) return {0, 0, width, height};
    Window f = w;
    f.x = std::min(f.x, width - 1);
    f.y = std::min(f.y, height - 1);
    f.w = std::min(f.w, width - f.x);
    f.h = std::min(f.h, height - f.y);
    adjusted = adjusted || f.x != w.x || f.y != w.y || f.w != w.w || f.h != w.h;
    return f;
}

// ------------------------------------------------------------------------------------------------- the calls

// cellVpostQueryAttr(cfgParam, attr): CellVpostAttr {memSize, delay (u8), vpostVerUpper (+8), vpostVerLower (+12)}.
s32 cellVpostQueryAttr(PPUContext& ctx, u32 cfg, u32 attr)
{
    u8* base = gb(ctx);
    if (!cfg) return error(CELL_VPOST_ERROR_Q_ARG_CFG_NULL);
    if (!attr) return error(CELL_VPOST_ERROR_Q_ARG_ATTR_NULL);
    if (!in_guest(ctx, cfg, 40) || !in_guest(ctx, attr, 16)) {
        log_call_site(ctx, base, "cellVpostQueryAttr: a parameter is not in guest memory");
        return error(cfg && !in_guest(ctx, cfg, 40) ? CELL_VPOST_ERROR_Q_ARG_CFG_INVALID
                                                     : CELL_VPOST_ERROR_Q_ARG_ATTR_NULL);
    }
    Config c;
    if (!read_config(base, cfg, c)) return error(CELL_VPOST_ERROR_Q_ARG_CFG_INVALID);
    ps_write32(base, attr, kMemSize);
    ps_write8(base, attr + 4, 0);
    ps_write32(base, attr + 8, kVerUpper);
    ps_write32(base, attr + 12, kVerLower);
    return lv2::CELL_OK;
}

// cellVpostOpen and OpenEx (cfgParam, resource, handle *): the resource (memory, threads, SPUs or a SPURS instance)
// is only checked for being there.
s32 open(PPUContext& ctx, u32 cfg, u32 resource, u32 handle, u32 resource_size, const char* name)
{
    u8* base = gb(ctx);
    if (!cfg) return error(CELL_VPOST_ERROR_O_ARG_CFG_NULL);
    Config c;
    if (!in_guest(ctx, cfg, 40) || !read_config(base, cfg, c)) return error(CELL_VPOST_ERROR_O_ARG_CFG_INVALID);
    if (!resource || !in_guest(ctx, resource, resource_size)) return error(CELL_VPOST_ERROR_O_ARG_RSRC_NULL);
    if (!handle || !in_guest(ctx, handle, 4)) return error(CELL_VPOST_ERROR_O_ARG_HDL_NULL);
    State& s = state();
    std::lock_guard lock(s.mutex);
    const u32 id = s.instances.make(Instance{c});
    if (!id) return error(CELL_VPOST_ERROR_O_FATAL_CREATEMON_FAIL);
    ps_write32(base, handle, id);
    PS_LOG_INFO(kChan, "{}: handle {}, in up to {}x{}, out up to {}x{} {}", name, id, c.in_max_w, c.in_max_h,
        c.out_max_w, c.out_max_h, c.out_fmt == kOutRgbaIlv ? "RGBA" : "YUV420 planar");
    return lv2::CELL_OK;
}

s32 cellVpostOpen(PPUContext& ctx, u32 cfg, u32 resource, u32 handle)
{
    return open(ctx, cfg, resource, handle, 24, "cellVpostOpen");
}

s32 cellVpostOpenEx(PPUContext& ctx, u32 cfg, u32 resource, u32 handle)
{
    return open(ctx, cfg, resource, handle, 24, "cellVpostOpenEx");
}

s32 cellVpostClose(PPUContext&, u32 handle)
{
    if (!handle) return error(CELL_VPOST_ERROR_C_ARG_HDL_NULL);
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.instances.remove(handle)) return error(CELL_VPOST_ERROR_C_ARG_HDL_INVALID);
    return lv2::CELL_OK;
}

// cellVpostExec(handle, inPicBuff, ctrlParam, outPicBuff, picInfo). CellVpostCtrlParam: execType, scalerType,
// ipcType, inWidth (+12), inHeight, inChromaPosType (+20), inQuantRange, inColorMatrix, inWindow {x, y, width, height}
// (+32), outWidth (+48), outHeight, outWindow (+56), outAlpha (u8, +72), userData (u64, +80).
s32 cellVpostExec(PPUContext& ctx, u32 handle, u32 in_pic, u32 ctrl, u32 out_pic, u32 info)
{
    u8* base = gb(ctx);
    if (!handle) return error(CELL_VPOST_ERROR_E_ARG_HDL_NULL);
    State& s = state();
    Config c;
    {
        std::lock_guard lock(s.mutex);
        const Instance* inst = s.instances.find(handle);
        if (!inst) return error(CELL_VPOST_ERROR_E_ARG_HDL_INVALID);
        c = inst->cfg;
    }
    if (!in_pic) return error(CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL);
    if (!ctrl) return error(CELL_VPOST_ERROR_E_ARG_CTRL_NULL);
    if (!out_pic) return error(CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL);
    if (!info) return error(CELL_VPOST_ERROR_E_ARG_PICINFO_NULL);
    if (!in_guest(ctx, ctrl, 96)) return error(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
    if (!in_guest(ctx, info, 88)) return error(CELL_VPOST_ERROR_E_ARG_PICINFO_NULL);

    const s32 exec_type = static_cast<s32>(ps_read32(base, ctrl)), scaler = static_cast<s32>(ps_read32(base, ctrl + 4));
    const u32 w = ps_read32(base, ctrl + 12), h = ps_read32(base, ctrl + 16);
    const s32 chroma_pos = static_cast<s32>(ps_read32(base, ctrl + 20));
    const s32 range = static_cast<s32>(ps_read32(base, ctrl + 24));
    const s32 matrix = static_cast<s32>(ps_read32(base, ctrl + 28));
    const Window in_win_asked{ps_read32(base, ctrl + 32), ps_read32(base, ctrl + 36), ps_read32(base, ctrl + 40),
        ps_read32(base, ctrl + 44)};
    const u32 ow = ps_read32(base, ctrl + 48), oh = ps_read32(base, ctrl + 52);
    const Window out_win_asked{ps_read32(base, ctrl + 56), ps_read32(base, ctrl + 60), ps_read32(base, ctrl + 64),
        ps_read32(base, ctrl + 68)};
    const u8 alpha = ps_read8(base, ctrl + 72);
    const u64 user_data = ps_read64(base, ctrl + 80);
    if (!w || !h || w > c.in_max_w || h > c.in_max_h || !ow || !oh || ow > c.out_max_w || oh > c.out_max_h ||
        exec_type < 0 || exec_type > 7) {
        PS_LOG_WARN(kChan, "cellVpostExec: in {}x{}, out {}x{}, exec type {}: not within the handle's {}x{} and {}x{}",
            w, h, ow, oh, exec_type, c.in_max_w, c.in_max_h, c.out_max_w, c.out_max_h);
        return error(CELL_VPOST_ERROR_E_ARG_CTRL_INVALID);
    }
    const u64 in_size = u64{w} * h + 2 * (u64{w / 2} * (h / 2));
    const u64 out_size = c.out_fmt == kOutRgbaIlv ? u64{ow} * oh * 4 : u64{ow} * oh + 2 * (u64{ow / 2} * (oh / 2));
    if (!in_guest(ctx, in_pic, in_size)) return error(CELL_VPOST_ERROR_E_ARG_INPICBUF_INVALID);
    if (!in_guest(ctx, out_pic, out_size)) return error(CELL_VPOST_ERROR_E_ARG_OUTPICBUF_INVALID);
    {
        std::lock_guard lock(s.mutex);
        if (exec_type != 0 && !s.exec_type_logged) {
            s.exec_type_logged = true;
            PS_LOG_INFO(kChan, "cellVpostExec: exec type {} (fields): the picture is taken as a whole frame",
                exec_type);
        }
        if (scaler != 0 && !s.scaler_logged) {
            s.scaler_logged = true;
            PS_LOG_INFO(kChan, "cellVpostExec: scaler type {}: scaled with the bilinear one", scaler);
        }
    }

    // The picture information: what was asked, the structures from the exec type.
    static constexpr s32 kInStruct[8] = {kStructPfrm, kStructPfrm, kStructPfrm, kStructItop, kStructIbtm, kStructIfrm,
        kStructItop, kStructIbtm};
    static constexpr s32 kOutStruct[8] = {kStructPfrm, kStructItop, kStructIbtm, kStructPfrm, kStructPfrm, kStructIfrm,
        kStructItop, kStructIbtm};
    const s32 in_struct = kInStruct[exec_type], out_struct = kOutStruct[exec_type];
    const s32 fields[] = {static_cast<s32>(w), static_cast<s32>(h), kDepth8, in_struct == kStructPfrm ? kScanP : kScanI,
        kInYuv420Planar, chroma_pos, in_struct, range, matrix, static_cast<s32>(ow), static_cast<s32>(oh), kDepth8,
        out_struct == kStructPfrm ? kScanP : kScanI, c.out_fmt, chroma_pos, out_struct, range, matrix};
    for (u32 i = 0; i < std::size(fields); i++) ps_write32(base, info + 4 * i, static_cast<u32>(fields[i]));
    ps_write64(base, info + 72, user_data);
    ps_write32(base, info + 80, 0);
    ps_write32(base, info + 84, 0);

    bool adjusted = false;
    const Window iw = fit(in_win_asked, w, h, adjusted), owin = fit(out_win_asked, ow, oh, adjusted);
    if (adjusted) {
        std::lock_guard lock(s.mutex);
        if (!s.window_logged) {
            s.window_logged = true;
            PS_LOG_WARN(kChan, "cellVpostExec: a window past its picture ({}x{} at {},{} of {}x{}; {}x{} at {},{} of "
                               "{}x{}) is cut to it",
                in_win_asked.w, in_win_asked.h, in_win_asked.x, in_win_asked.y, w, h, out_win_asked.w,
                out_win_asked.h, out_win_asked.x, out_win_asked.y, ow, oh);
        }
    }

    // Luma: output pixel o's centre in the input window; chroma: the same point in chroma samples (type A: a chroma
    // sample at every second luma column; B, and vertically: between two).
    const f32 sx = static_cast<f32>(iw.w) / static_cast<f32>(owin.w);
    const f32 sy = static_cast<f32>(iw.h) / static_cast<f32>(owin.h);
    const f32 hsite = chroma_pos == kChromaTypeA ? 0.0f : 0.5f;
    const u8* y_plane = base + in_pic;
    const u8* u_plane = y_plane + static_cast<usize>(w) * h;
    const u8* v_plane = u_plane + static_cast<usize>(w / 2) * (h / 2);
    const u32 cw = std::max(1u, w / 2), ch = std::max(1u, h / 2);
    const u32 cx0 = std::min(iw.x / 2, cw - 1), cy0 = std::min(iw.y / 2, ch - 1);
    const u32 cx1 = std::clamp((iw.x + iw.w + 1) / 2, cx0 + 1, cw);
    const u32 cy1 = std::clamp((iw.y + iw.h + 1) / 2, cy0 + 1, ch);

    thread_local Taps lh, lv, chh, chv;
    thread_local std::vector<f32> tmp, py, pu, pv;
    const bool yuv_out = c.out_fmt == kOutYuv420Planar;
    const auto luma_x = [&](f32 out_x) { return static_cast<f32>(iw.x) + (out_x + 0.5f) * sx - 0.5f; };
    const auto luma_y = [&](f32 out_y) { return static_cast<f32>(iw.y) + (out_y + 0.5f) * sy - 0.5f; };
    make_taps(lh, owin.w, sx, iw.x, iw.w, [&](u32 o) { return luma_x(static_cast<f32>(o)); });
    make_taps(lv, owin.h, sy, iw.y, iw.h, [&](u32 o) { return luma_y(static_cast<f32>(o)); });
    // The chroma wanted: at every output pixel for RGBA; for YUV, the output's own chroma samples, sited as the
    // input's (output luma column 2p, or 2p + 0.5 for type B; row 2p + 0.5).
    const u32 cow = yuv_out ? std::max(1u, owin.w / 2) : owin.w, coh = yuv_out ? std::max(1u, owin.h / 2) : owin.h;
    make_taps(chh, cow, yuv_out ? sx : sx / 2.0f, cx0, cx1 - cx0, [&](u32 o) {
        const f32 out_x = yuv_out ? 2.0f * static_cast<f32>(o) + hsite : static_cast<f32>(o);
        return (luma_x(out_x) - hsite) / 2.0f;
    });
    make_taps(chv, coh, yuv_out ? sy : sy / 2.0f, cy0, cy1 - cy0, [&](u32 o) {
        const f32 out_y = yuv_out ? 2.0f * static_cast<f32>(o) + 0.5f : static_cast<f32>(o);
        return (luma_y(out_y) - 0.5f) / 2.0f;
    });
    scale_plane(y_plane, w, lh, lv, owin.w, owin.h, tmp, py);
    scale_plane(u_plane, w / 2, chh, chv, cow, coh, tmp, pu);
    scale_plane(v_plane, w / 2, chh, chv, cow, coh, tmp, pv);

    u8* out = base + out_pic;
    if (yuv_out) {
        u8* oy = out;
        u8* ou = oy + static_cast<usize>(ow) * oh;
        u8* ov = ou + static_cast<usize>(ow / 2) * (oh / 2);
        for (u32 y = 0; y < owin.h; y++)
            for (u32 x = 0; x < owin.w; x++)
                oy[static_cast<usize>(owin.y + y) * ow + owin.x + x] = to_byte(py[static_cast<usize>(y) * owin.w + x]);
        const u32 ox = owin.x / 2, oy0 = owin.y / 2;
        for (u32 y = 0; y < coh && oy0 + y < oh / 2; y++) {
            for (u32 x = 0; x < cow && ox + x < ow / 2; x++) {
                const usize at = static_cast<usize>(oy0 + y) * (ow / 2) + ox + x;
                ou[at] = to_byte(pu[static_cast<usize>(y) * cow + x]);
                ov[at] = to_byte(pv[static_cast<usize>(y) * cow + x]);
            }
        }
        return lv2::CELL_OK;
    }

    // YCbCr to RGB: Kr and Kb of the matrix; broadcast range expands Y from 16-235 and C from 16-240.
    const f32 kr = matrix == kMatrixBt709 ? 0.2126f : 0.299f, kb = matrix == kMatrixBt709 ? 0.0722f : 0.114f;
    const f32 kg = 1.0f - kr - kb;
    const bool full = range == kRangeFull;
    const f32 ys = full ? 1.0f : 255.0f / 219.0f, yo = full ? 0.0f : 16.0f, cs = full ? 1.0f : 255.0f / 224.0f;
    const f32 rv = 2.0f * (1.0f - kr) * cs, bu = 2.0f * (1.0f - kb) * cs;
    const f32 gu = 2.0f * kb * (1.0f - kb) / kg * cs, gv = 2.0f * kr * (1.0f - kr) / kg * cs;
    for (u32 y = 0; y < owin.h; y++) {
        u8* row = out + (static_cast<usize>(owin.y + y) * ow + owin.x) * 4;
        const usize at = static_cast<usize>(y) * owin.w;
        for (u32 x = 0; x < owin.w; x++) {
            const f32 lum = (py[at + x] - yo) * ys, cb = pu[at + x] - 128.0f, cr = pv[at + x] - 128.0f;
            row[4 * x + 0] = to_byte(lum + rv * cr);
            row[4 * x + 1] = to_byte(lum - gu * cb - gv * cr);
            row[4 * x + 2] = to_byte(lum + bu * cb);
            row[4 * x + 3] = alpha;
        }
    }
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
