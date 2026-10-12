#pragma once

// cellVdec's decoders as Sony's libvdec 4.92 lays them out (D-912), read in libvdec and its codec cores (libdivxdec,
// libdivx311dec, libsmvd4, libsvc1d, libmvcdec) and checked against them in tests/hle/test_vdec_codecs_compare.cpp:
//
// - The decoder's memory: libvdec's context (0x5C bytes), then a ring of 0x90-byte elements, one per access unit in
//   the decoder (the CellVdecPicItem of its picture at +0x40), then the codec's information blocks (one per element),
//   then two queues of the ring's indices; then the codec's own memory, where its frame buffers are.
// - The sizes: each codec's memory query (the codec core's, with the game's codec specific information), plus libvdec's
//   own parts. A decoder opens with at least the codec's part of what QueryAttrEx asks for.
// - The frame buffers: each core's, at their place in its memory, used in turn; a core holds at most so many pictures
//   (decoded and not yet released by GetPicture) before it holds an access unit back.
// - The frame buffer layout: 16-byte-wide strips of luma, then of chroma (8 Cb then 8 Cr a line), with one strip of
//   repeated edge on each side and lines of repeated edge above and below (DivX, MPEG-4, VC-1); H.264's has two strips
//   of edge on each side and no lines of edge (D-196).
//
// Nothing here is Sony's code or data: the arithmetic of the sizes and places is reproduced from what the modules do,
// written out here (CLAUDE.md rule 1).

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

namespace pspocket::hle::vdec {

enum : u32 {
    CODEC_MPEG2 = 0,
    CODEC_AVC = 1,
    CODEC_MPEG4 = 2,
    CODEC_VC1 = 3,
    CODEC_DIVX = 5,
    CODEC_JVT = 7,
    CODEC_DIVX3_11 = 9,
    CODEC_MVC = 11,
    CODEC_MVC2 = 13,
};

inline constexpr u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// ---- the ring: elements of 0x90 bytes (the access unit's information at +0x18, its picture's item at +0x40) and the
// information blocks

struct Ring {
    u32 count = 0x20, info = 0x20;
};

inline Ring ring_of(u32 codec)
{
    switch (codec) {
    case CODEC_MPEG2: return {0x20, 0x1C8};
    case CODEC_AVC: return {0x30, 0x244};
    case CODEC_MPEG4: return {0x20, 0x38};
    case CODEC_VC1: return {0x20, 0x170};
    case CODEC_DIVX: return {0x20, 0x20};
    case CODEC_JVT: return {0x20, 0x160};
    case CODEC_DIVX3_11: return {0x20, 0x18};
    default: return {0x30, 0x214}; // MVC
    }
}

inline constexpr u32 kContext = 0x5C;   // libvdec's context at the start of the decoder's memory
inline constexpr u32 kElement = 0x90;   // a ring element
inline constexpr u32 kQuery = 0x858;    // what QueryAttrEx adds for libvdec itself
inline constexpr u32 kSpursPart = 0x417B8; // SPURS and its task contexts (0x41580) and five SPU tasks' (5 x 0x50 + 0xA8)

inline u32 ring_bytes(const Ring& r) { return (r.info + kElement) * r.count + (r.count * 0x14 + 0x14) * 2 + 0x40; }
inline u32 element_addr(u32 handle, u32 slot) { return handle + kContext + 0x40 + slot * kElement; }
inline u32 item_addr(u32 handle, u32 slot) { return element_addr(handle, slot) + 0x40; }
inline u32 info_addr(u32 handle, const Ring& r, u32 slot) { return handle + kContext + 0x40 + r.count * kElement + slot * r.info; }
inline u32 codec_memory(u32 handle, const Ring& r) { return handle + kContext + ring_bytes(r); }

// An element made free (at open, at StartSeq, when its picture is released or its access unit made none): the fields
// of the item it holds that libvdec sets then (the start address and size 0, the PTS and DTS of both access units all
// ones, their user data 0). The item's other bytes are left as they are.
inline void clear_item(u8* base, u32 item)
{
    ps_write32(base, item + 0x04, 0);
    ps_write32(base, item + 0x08, 0);
    for (const u32 at : {0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x28u, 0x2Cu}) ps_write32(base, item + at, 0xFFFFFFFF);
    ps_write64(base, item + 0x30, 0);
    ps_write64(base, item + 0x38, 0);
}

// ---- sizes

enum class Fail : u32 { none = 0, arg = 0x80610101, unk = 0x80610110, fatal = 0x80610180 };

// What a codec's memory query gives: libvdec's part plus the core's (the least memory a decoder opens with), and what
// the core sizes its frame buffers for.
struct Sizing {
    Fail fail = Fail::none;
    u32 codec_part = 0;
    u32 width = 0, height = 0, buffers = 0; // the frames' size and count (DivX, DivX 3.11)
    u32 level = 0;                          // the core's own level number (VC-1: 1 to 10)
};

// libdivxdec's and libdivx311dec's frame buffer: luma lines to 16, chroma lines to 8, 48 lines and 32 samples of edge.
inline u32 divx_frame_bytes(u32 w, u32 h) { return (align((h + 1) / 2, 8) + align(h, 16) + 0x30) * (align(w, 16) + 0x20); }

inline Sizing divx_sizing(u8* base, u32 level, u32 spec, bool readable)
{
    Sizing s;
    constexpr u32 kW[] = {176, 352, 720, 1280, 1920}, kH[] = {144, 288, 576, 720, 1088};
    constexpr u32 kMax[] = {0x1577BE, 0x1E675E, 0x4108DE, 0x77DF0E, 0xF420FE};
    if (level < 10 || level > 14) return {Fail::arg};
    u32 w = kW[level - 10], h = kH[level - 10], n = 4;
    if (spec) {
        if (!readable) return {Fail::arg};
        const u32 size = ps_read32(base, spec);
        if (size != 8 && size != 12) return {Fail::arg};
        const u32 sw = ps_read16(base, spec + 4), sh = ps_read16(base, spec + 6);
        if (sw && sh) {
            if (sw > w || sh > h) return {Fail::arg};
            w = sw;
            h = sh;
        }
        if (size == 12 && ps_read16(base, spec + 8)) n = ps_read16(base, spec + 8);
    }
    // divxDecQueryAttr with the size and count.
    if (n < 2 || n > 4) return {Fail::arg};
    u32 frame = 0;
    if (w - 0x10 < 0x771 && h >= 0x10 && h <= 0x440) frame = divx_frame_bytes(w, h);
    else if (w == 0x580 && h == 0x480) frame = 0x270600;
    else return {Fail::arg};
    const u32 frames = n * frame;
    const u32 mem = (static_cast<u32>(static_cast<s64>(static_cast<double>(frames) * 0.005)) + frames + 0x9E86B) & ~0xFu;
    if (mem > kMax[level - 10]) return {Fail::arg};
    s.codec_part = kSpursPart + mem + 0x34F8;
    s.width = w;
    s.height = h;
    s.buffers = n;
    return s;
}

inline Sizing divx311_sizing(u8* base, u32 level, u32 spec, bool readable)
{
    Sizing s;
    u32 w = 720, h = 576;
    if (spec) {
        if (!readable || ps_read32(base, spec) != 12) return {Fail::arg};
        const u32 sw = ps_read16(base, spec + 4), sh = ps_read16(base, spec + 6);
        if (sw && sh) {
            if (sw > 1920 || sh > 1088) return {Fail::arg};
            w = sw;
            h = sh;
        }
        const u32 n = ps_read16(base, spec + 8);
        if (n != 0 && n != 2) return {Fail::arg};
    }
    if (w - 0x10 > 0x770 || h > 0x440 || h < 0x10) return {Fail::arg};
    const u32 frame = divx_frame_bytes(w, h);
    const u32 mem = frame ? frame * 2 + 0x4D800 : 0;
    if (level != 15) return {Fail::arg};
    if (w <= 720 && h <= 576 && mem > 0x1CE600) return {Fail::arg};
    s.codec_part = kSpursPart + mem + 0x3500;
    s.width = w;
    s.height = h;
    s.buffers = 2;
    return s;
}

// libsmvd4's frame buffer: the luma's lines to 32 for the allocation, 16 lines and samples of edge, and 128 bytes.
inline u32 smvd4_frame_bytes(u32 w, u32 h) { return (align(h, 32) + 32) * (align(w, 16) + 32) * 3 / 2 + 0x80; }

inline Sizing mpeg4_sizing(u8* base, u32 level, u32 spec, bool readable)
{
    Sizing s;
    constexpr u32 kW[] = {176, 352, 352, 720, 640, 720}, kH[] = {144, 288, 288, 480, 480, 576};
    constexpr u32 kMax[] = {0x44480, 0xA8B00, 0xA8B00, 0x1E6800, 0x1B4D00, 0x23E400};
    if (level > 5) return {Fail::arg};
    u32 w = kW[level], h = kH[level];
    if (spec) {
        if (!readable || ps_read32(base, spec) != 8) return {Fail::arg};
        const u32 sw = ps_read16(base, spec + 4), sh = ps_read16(base, spec + 6);
        if (sw && sh) { // smvd4GetMemorySize2: within the level's size
            if (sw > w || sh > h) return {Fail::fatal};
            w = sw;
            h = sh;
        }
    }
    const u32 mem = ((w + 15) >> 4) * ((h + 15) >> 4) * 0x80 + 3 * smvd4_frame_bytes(w, h) + 0x15380;
    if (mem > kMax[level]) return {Fail::fatal};
    s.codec_part = kSpursPart + 0x3493 + mem;
    s.width = w;
    s.height = h;
    s.buffers = 3;
    return s;
}

// libsvc1d's frame buffer: the luma's lines to 32, 64 lines of edge above and below, 16 samples each side, to 256.
inline u32 svc1d_frame_bytes(u32 w, u32 h) { return ((align(h, 32) + 128) * (align(w, 16) + 32) * 3 / 2 + 256) & ~0xFFu; }
// The frames of a core level (1 to 10): nine for the main profile (B pictures), six for the others.
inline u32 svc1d_frames(u32 core_level) { return core_level >= 3 && core_level <= 5 ? 9 : 6; }
// The tables before the frame buffers, sized for the picture: per macroblock, and per block with an edge.
inline u32 svc1d_mb_table(u32 w, u32 h) { return (((w + 15) >> 4) * ((h + 15) >> 4) * 896 + 128) & ~0x7Fu; }
inline u32 svc1d_block_table(u32 w, u32 h)
{
    const u32 mbw = (w + 15) >> 4, mbh = (h + 15) >> 4;
    return ((2 * mbw + 4) * (2 * mbh + 2) * 3 * 16 + 128) & ~0xFu;
}

inline Sizing vc1_sizing(u8* base, u32 level, u32 spec, bool readable)
{
    Sizing s;
    constexpr u32 kW[] = {176, 352, 352, 720, 1920, 352, 720, 1280, 2048, 2048};
    constexpr u32 kH[] = {144, 288, 288, 576, 1080, 288, 576, 900, 1080, 2048};
    constexpr u32 kMax[] = {0x1B44F0, 0x2DFEF0, 0x38F9F0, 0x999E30, 0x2827F70, 0x2DFEF0, 0x754130, 0x11BD9F0, 0x1FDEE70,
        0x3900070};
    if (level > 9) return {Fail::arg};
    u32 w = kW[level], h = kH[level];
    if (spec) {
        if (!readable || ps_read32(base, spec) != 8) return {Fail::arg};
        const u32 sw = ps_read16(base, spec + 4), sh = ps_read16(base, spec + 6);
        if (sw) { // svc1dGetMemorySize2: a size given is within the level's, to 16
            if (sw > w) return {Fail::fatal};
            w = align(sw, 16);
        }
        if (sh) {
            if (sh > h) return {Fail::fatal};
            h = align(sh, 16);
        }
    }
    const u32 core = level + 1;
    // The frames and the tables sized for the picture, then the core's fixed parts.
    const u32 mem = svc1d_frames(core) * svc1d_frame_bytes(w, h) + svc1d_mb_table(w, h) + svc1d_block_table(w, h) +
                    22464 + 1108912 + 0x480;
    if (mem > kMax[level]) return {Fail::fatal};
    s.codec_part = kSpursPart + 0x3493 + mem;
    s.width = w;
    s.height = h;
    s.buffers = svc1d_frames(core);
    s.level = core;
    return s;
}

// libmvcdec's mvcDecQueryMemory: as libavcdec's (cellVdec.cpp avc_memory) for two views, its own blocks.
struct MvcLevel {
    u32 level, mbps, max_fs, dpb_x10, min_cr, width, height, max_mem;
};
inline constexpr std::array<MvcLevel, 13> kMvcLevels{{
    {10, 1485, 99, 1485, 2, 80, 304, 0x9E2980},
    {11, 3000, 396, 3375, 2, 80, 960, 0xBEC300},
    {12, 6000, 396, 8910, 2, 80, 1072, 0xE44D00},
    {13, 11880, 396, 8910, 2, 80, 1072, 0xE72D00},
    {20, 11880, 396, 8910, 2, 80, 1072, 0xE72D00},
    {21, 19800, 792, 17820, 2, 80, 1072, 0x13F1380},
    {22, 20250, 1620, 30375, 2, 128, 1072, 0x1ABA180},
    {30, 40500, 1620, 30375, 2, 128, 1072, 0x1B58780},
    {31, 108000, 3600, 67500, 4, 928, 992, 0x28C6700},
    {32, 216000, 5120, 76800, 4, 1280, 1024, 0x3208700},
    {40, 245760, 8192, 122880, 4, 1920, 1088, 0x487ED00},
    {41, 245760, 8192, 122880, 2, 1920, 1088, 0x487ED00},
    {42, 245760, 8192, 122880, 2, 1920, 1088, 0x487ED00},
}};

inline Sizing mvc_sizing(u8* base, u32 level, u32 spec, bool readable)
{
    Sizing s;
    u32 lv = level, deblock = 0, buffers = 0, wmb = 0, hmb = 0;
    if (level == 1042) {
        lv = 42;
        deblock = 1;
    }
    const auto it = std::find_if(kMvcLevels.begin(), kMvcLevels.end(), [&](const MvcLevel& l) { return l.level == lv; });
    if (it == kMvcLevels.end()) return {Fail::arg};
    if (spec) {
        if (!readable || ps_read32(base, spec) != 12) return {Fail::arg};
        const u32 sw = ps_read16(base, spec + 4), sh = ps_read16(base, spec + 6);
        if (sw && sh) {
            wmb = (sw + 15) / 16;
            hmb = (sh + 15) / 16;
        }
        deblock = ps_read8(base, spec + 8);
        buffers = ps_read8(base, spec + 9);
    }
    if (deblock > 2) return {Fail::fatal};
    u32 width = it->width, height = it->height, frame_mbs = it->max_fs;
    if (wmb && hmb) {
        if (wmb > 120 || wmb <= 3 || hmb > 68 || hmb <= 3 || wmb * hmb > it->max_fs) return {Fail::fatal};
        frame_mbs = wmb * hmb;
        width = wmb * 16;
        height = hmb * 16;
    }
    const u32 min_cr = lv > 40 ? 4 : it->min_cr;
    u32 au = static_cast<u32>(u64{it->mbps} * 384384 / (u64{min_cr} * 24000));
    au = align((au + 8192) * 2, 128) + 17408;
    u32 dpb = it->dpb_x10 * 1024 / 10 * 2 * 2 / (width * height * 3);
    if (dpb & 1) dpb++;
    dpb = std::min<u32>(dpb, 32);
    if (buffers) {
        if (buffers > dpb || (buffers & 1)) return {Fail::fatal};
        dpb = buffers;
    }
    dpb += 4;
    const u32 frame = (width + 64) * height + (width + 64) * height / 2 + 2048;
    const u32 blocks[] = {0xE9400, frame_mbs * 0x600, frame_mbs * 0x200, au * 2, frame * dpb, 1152, 17280, 0x1B7E00, 9984,
        0x9F600, 8064, 640, 0x40000, 256, 768, align((dpb - 1) * frame_mbs * 80, 128)};
    u32 total = 0, any = 0;
    for (const u32 v : blocks) {
        total += v;
        any |= v;
    }
    if (any & 127) return {Fail::fatal};
    const u32 mem = align(total + 0x33700 + 0x4FC100, 128);
    if (mem > it->max_mem) return {Fail::fatal};
    s.codec_part = 0x41580 + mem + 0x12FD;
    return s;
}

// ---- the frame buffers

// How a decoder's pictures are kept: its frame buffers (in turn), how many pictures it holds before an access unit
// waits, how many access units late a picture is announced, and the layout of a frame.
struct Plan {
    u32 frames = 0;      // frame buffers used in turn; 0: pspocket's own frames (H.264, MPEG-2, D-196)
    u32 limit = ~0u;     // pictures decoded and not released before an access unit waits
    u32 lag = 1;         // pictures announced after the next access unit's AUDONE (1) or after their own (0)
    u32 frame0 = 0, frame_bytes = 0;
    u32 edge_y = 16, edge_c = 8; // the strips' lines of edge above and below (luma, chroma)
    bool lines32 = false;        // the luma's lines to 32 (VC-1), else to 16
    bool start_inside = false;   // the start address is the first luma sample's, else the frame's
};

// The first luma sample's place in a frame laid out in strips (the left strip of edge, the lines of edge above).
inline u32 strip_start(const Plan& p, u32 height)
{
    const u32 lines = (p.lines32 ? align(height, 32) : align(height, 16)) + 2 * p.edge_y;
    return lines * 16 + p.edge_y * 16;
}

// The luma part of a frame laid out in strips, and the first Cb sample's place.
inline u32 strip_luma_bytes(const Plan& p, u32 width, u32 height)
{
    const u32 lines = (p.lines32 ? align(height, 32) : align(height, 16)) + 2 * p.edge_y;
    return (align(width, 16) / 16 + 2) * 16 * lines;
}
inline u32 strip_cb(const Plan& p, u32 width, u32 height)
{
    const u32 chroma_lines = (p.lines32 ? align(height, 32) : align(height, 16)) / 2 + 2 * p.edge_c;
    return strip_luma_bytes(p, width, height) + chroma_lines * 16 + p.edge_c * 16;
}

// The edges of a picture already in a frame laid out in strips (its samples where write_strips puts them): every
// sample past the picture repeats its nearest one, read back from the frame (a decoder pads a reference picture's edges
// so, D-920).
inline void pad_strips(u8* base, u32 frame, const Plan& p, u32 w, u32 h)
{
    const u32 hl = p.lines32 ? align(h, 32) : align(h, 16);
    const u32 strips = align(w, 16) / 16 + 2, lines = hl + 2 * p.edge_y, clines = hl / 2 + 2 * p.edge_c;
    const u32 cw = w / 2, ch = h / 2;
    u8* luma = base + frame;
    u8* chroma = luma + strips * lines * 16;
    const auto y_at = [&](u32 x, u32 row) -> u8& { return luma[((x / 16 + 1) * lines + row + p.edge_y) * 16 + x % 16]; };
    const auto c_at = [&](u32 x, u32 row, u32 plane) -> u8& {
        return chroma[((x / 8 + 1) * clines + row + p.edge_c) * 16 + plane * 8 + x % 8];
    };
    for (u32 s = 0; s < strips; s++)
        for (u32 r = 0; r < lines; r++)
            for (u32 k = 0; k < 16; k++) {
                const s32 x = static_cast<s32>(s * 16 + k) - 16, row = static_cast<s32>(r) - static_cast<s32>(p.edge_y);
                if (x >= 0 && x < static_cast<s32>(w) && row >= 0 && row < static_cast<s32>(h)) continue;
                luma[(s * lines + r) * 16 + k] = y_at(static_cast<u32>(std::clamp<s32>(x, 0, static_cast<s32>(w) - 1)),
                    static_cast<u32>(std::clamp<s32>(row, 0, static_cast<s32>(h) - 1)));
            }
    for (u32 s = 0; s < strips; s++)
        for (u32 r = 0; r < clines; r++)
            for (u32 k = 0; k < 8; k++) {
                const s32 x = static_cast<s32>(s * 8 + k) - 8, row = static_cast<s32>(r) - static_cast<s32>(p.edge_c);
                if (x >= 0 && x < static_cast<s32>(cw) && row >= 0 && row < static_cast<s32>(ch)) continue;
                const u32 sx = static_cast<u32>(std::clamp<s32>(x, 0, static_cast<s32>(cw) - 1));
                const u32 sr = static_cast<u32>(std::clamp<s32>(row, 0, static_cast<s32>(ch) - 1));
                chroma[(s * clines + r) * 16 + k] = c_at(sx, sr, 0);
                chroma[(s * clines + r) * 16 + 8 + k] = c_at(sx, sr, 1);
            }
}

// A picture (planar 4:2:0, no padding) in a frame laid out in strips with edges: every sample past the picture repeats
// its nearest one; with `edges` false the picture's samples alone (a picture no other is predicted from, whose edges a
// decoder leaves as they were, D-920).
inline void write_strips(u8* base, u32 frame, const Plan& p, u32 w, u32 h, const u8* y, const u8* u, const u8* v,
    bool edges = true)
{
    const u32 hl = p.lines32 ? align(h, 32) : align(h, 16);
    const u32 strips = align(w, 16) / 16 + 2, lines = hl + 2 * p.edge_y, clines = hl / 2 + 2 * p.edge_c;
    const u32 cw = w / 2, ch = h / 2;
    u8* out = base + frame;
    for (u32 s = 0; s < strips; s++)
        for (u32 r = 0; r < lines; r++) {
            const s32 rr = static_cast<s32>(r) - static_cast<s32>(p.edge_y);
            const u32 row = static_cast<u32>(std::clamp<s32>(rr, 0, static_cast<s32>(h) - 1));
            for (u32 k = 0; k < 16; k++, out++) {
                const s32 xx = static_cast<s32>(s * 16 + k) - 16;
                if (!edges && (xx < 0 || xx >= static_cast<s32>(w) || rr < 0 || rr >= static_cast<s32>(h))) continue;
                *out = y[row * w + static_cast<u32>(std::clamp<s32>(xx, 0, static_cast<s32>(w) - 1))];
            }
        }
    for (u32 s = 0; s < strips; s++)
        for (u32 r = 0; r < clines; r++) {
            const s32 rr = static_cast<s32>(r) - static_cast<s32>(p.edge_c);
            const u32 row = static_cast<u32>(std::clamp<s32>(rr, 0, static_cast<s32>(ch) - 1));
            for (u32 k = 0; k < 8; k++) {
                const s32 xx = static_cast<s32>(s * 8 + k) - 8;
                if (!edges && (xx < 0 || xx >= static_cast<s32>(cw) || rr < 0 || rr >= static_cast<s32>(ch))) continue;
                const u32 x = static_cast<u32>(std::clamp<s32>(xx, 0, static_cast<s32>(cw) - 1));
                out[k] = u[row * cw + x];
                out[8 + k] = v[row * cw + x];
            }
            out += 16;
        }
}

// libvdec's places before a DivX core's memory: its context aligned to 128 bytes, the SPU tasks' areas, SPURS, then the
// core's instance (128-byte aligned).
inline u32 divx_instance(u32 handle, const Ring& r, bool divx311)
{
    const u32 ctx = align(codec_memory(handle, r), 128);
    if (divx311) {
        const u32 tasks = (ctx + 0x238 + 0x2A7F) & ~0x7Fu;
        return align(tasks + 0x80 + 0x41580, 128);
    }
    const u32 queues = ctx + 0x100 + 0x1000 + 0x1900;
    const u32 tasks = (queues + 0x7F + 0x238) & ~0x7Fu;
    return align(tasks + 0x80 + 0x41580, 128);
}

// The frame buffers of a decoder of `codec` at `handle` (D-912), from its sizing; H.264, MPEG-2 and MVC keep
// pspocket's own (frames 0).
inline Plan plan_for(u32 codec, u32 handle, const Sizing& s)
{
    Plan p;
    const Ring r = ring_of(codec);
    switch (codec) {
    case CODEC_DIVX3_11:
        p.frames = 2;
        p.limit = 2;
        p.frame0 = align(divx_instance(handle, r, true) + 0x3D700, 128);
        p.frame_bytes = divx_frame_bytes(s.width, s.height);
        break;
    case CODEC_DIVX:
        p.frames = std::min<u32>(s.buffers, 3);
        p.limit = s.buffers > 2 ? s.buffers - 1 : s.buffers;
        p.frame0 = divx_instance(handle, r, false) + 0x76680;
        p.frame_bytes = divx_frame_bytes(s.width, s.height);
        break;
    case CODEC_MPEG4:
        p.frames = 3;
        p.limit = 3;
        p.lag = 0;
        p.frame0 = align(handle, 128) + 0x16980;
        p.frame_bytes = smvd4_frame_bytes(s.width, s.height);
        p.start_inside = true;
        break;
    case CODEC_VC1:
        p.frames = s.buffers;
        p.limit = 4;
        p.frame0 = align(handle, 128) + 0x118A00 + svc1d_mb_table(s.width, s.height) +
                   align(svc1d_block_table(s.width, s.height), 128);
        p.frame_bytes = svc1d_frame_bytes(s.width, s.height);
        p.edge_y = 64;
        p.edge_c = 32;
        p.lines32 = true;
        p.start_inside = true;
        break;
    default: break;
    }
    return p;
}

} // namespace pspocket::hle::vdec
