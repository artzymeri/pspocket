// cellVdec: the video decoder (libvdec), pspocket's own on FFmpeg (D-181, D-195). H.264 (and the JVT form of it),
// MPEG-2 and MPEG-4 part 2 (as in DivX) decode with FFmpeg as in RPCS3 (rpcs3/Emu/Cell/Modules/cellVdec.cpp: the
// sequence states, the commands, the checks of each call, the picture formats). DivX 3.11 (D-588), which RPCS3 does not
// decode, goes the same way. What a game sees follows Sony's libvdec 4.92 as it ran over synthetic H.264 in
// tests/hle/test_vdec.cpp (D-195), where it differs from RPCS3's:
//
// - Sizes: cellVdecQueryAttr gives Sony's. For H.264 the decoder's memory comes from libavcdec's avcDecQueryMemory,
//   reconstructed (avc_memory below: Sony's answers in every case tried); for the other codecs, Sony's answers for each
//   level. RPCS3's codec memory queries are TODOs.
// - The handle is the address of the decoder's memory, as Sony's (RPCS3: an ID).
// - Pictures are announced one access unit late: after an access unit's AUDONE come the PICOUTs of the pictures the
//   earlier access units made; the last ones when the sequence ends, before SEQDONE.
// - Picture items (CellVdecPicItem) and the H.264 information (CellVdecAvcInfo, 0x244 bytes as Sony's: RPCS3's
//   structure stops at 0x138) are rings in the decoder's memory at Sony's places (items at +0xDC, 0x90 apart; the
//   information at +0x1B9C, 0x244 apart). The H.264 information comes from the stream (SPS, PPS, the NAL units of the
//   access unit), as Sony's; RPCS3 writes fixed guesses.
// - An item's start address is the picture in a frame buffer laid out as Sony's: 16-byte-wide strips of luma, then
//   of chroma (8 Cb then 8 Cr a row), two strips of repeated edge on each side (RPCS3: 0x123).
// - Pictures to RGBA and ARGB use the colour matrix asked for (BT.601 or BT.709, limited range; RPCS3: a TODO), with
//   the exact coefficients, truncated: within one step of Sony's values. UYVY repeats each chroma row, as Sony's.
//
// Not yet (each fails loudly): VC-1 and MVC decoding (open fails, logged), the codec specific information of codecs
// other than H.264 in the sizes (Sony's answer without it is given, and logged; DivX 3.11's is checked and gives the
// frame size).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#include "vdec_hw.hpp"
#endif

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellVdec";

enum : u32 {
    CELL_VDEC_ERROR_ARG = 0x80610101,
    CELL_VDEC_ERROR_SEQ = 0x80610102,
    CELL_VDEC_ERROR_BUSY = 0x80610103,
    CELL_VDEC_ERROR_EMPTY = 0x80610104,
    CELL_VDEC_ERROR_AU = 0x80610105,
    CELL_VDEC_ERROR_PIC = 0x80610106,
    CELL_VDEC_ERROR_UNK = 0x80610110,
    CELL_VDEC_ERROR_FATAL = 0x80610180,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

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
    CODEC_MAX = 14,
};
constexpr s32 MSG_AUDONE = 0, MSG_PICOUT = 1, MSG_SEQDONE = 2;
constexpr s32 DEC_MODE_NORMAL = 0, DEC_MODE_B_SKIP = 1, DEC_MODE_PB_SKIP = 2;
constexpr u32 PICFMT_RGBA32 = 1, PICFMT_UYVY422 = 2, PICFMT_YUV420_PLANAR = 3;
constexpr u32 AVC_LEVEL_UNK = 1042; // 4.2 with the deblocking filter off
constexpr u32 kVersionUpper = 0x4920000;
// DivX 3.11's frames carry no size (an AVI file's header does): RPCS3's default for its decoder (query_attr_divx,
// DivxDecParams), the largest it takes through CellVdecDivxSpecificInfo2, and that structure's size (D-588).
constexpr u32 kDivx311Width = 720, kDivx311Height = 576, kDivx311MaxWidth = 1920, kDivx311MaxHeight = 1088;
constexpr u32 kDivxSpecificInfo2Size = 12;

// ---- sizes

// libavcdec's level table (H.264 Table A-1's limits as libavcdec keeps them: macroblocks per second, the largest frame
// in macroblocks, the decoded picture buffer in 1024-byte units x10, the minimum compression ratio), the worst-case
// picture it sizes frame buffers for without the game's size, and the bytes it adds for the level.
struct AvcLevel {
    u32 level;
    u32 mbps, max_fs, dpb_x10, min_cr, width, height, extra;
};
constexpr std::array<AvcLevel, 13> kAvcLevels{{
    {10, 1485, 99, 1485, 2, 80, 304, 256},
    {11, 3000, 396, 3375, 2, 80, 960, 128},
    {12, 6000, 396, 8910, 2, 80, 1072, 256},
    {13, 11880, 396, 8910, 2, 80, 1072, 128},
    {20, 11880, 396, 8910, 2, 80, 1072, 128},
    {21, 19800, 792, 17820, 2, 80, 1072, 128},
    {22, 20250, 1620, 30375, 2, 128, 1072, 128},
    {30, 40500, 1620, 30375, 2, 128, 1072, 0},
    {31, 108000, 3600, 67500, 4, 928, 992, 256},
    {32, 216000, 5120, 76800, 4, 1280, 1024, 256},
    {40, 245760, 8192, 122880, 4, 1920, 1088, 256},
    {41, 245760, 8192, 122880, 2, 1920, 1088, 256},
    {42, 245760, 8192, 122880, 2, 1920, 1088, 256},
}};

// avcDecQueryMemory (libavcdec, fw 4.92, read as the reference): the decoder's memory for a level, the game's largest
// frame in macroblocks (0: the level's worst case) and its frame buffer count (0: as many as the level's buffer
// holds, at most 16), or none for a combination libavcdec refuses. Seventeen blocks, each a multiple of 128 bytes.
std::optional<u32> avc_memory(u32 level, u32 disable_deblocking, u32 buffers, u32 width_mb, u32 height_mb)
{
    if (disable_deblocking > 2) return std::nullopt;
    const auto it = std::find_if(kAvcLevels.begin(), kAvcLevels.end(), [&](const AvcLevel& l) { return l.level == level; });
    if (it == kAvcLevels.end()) return std::nullopt;
    u32 width = it->width, height = it->height, frame_mbs = it->max_fs;
    if (width_mb && height_mb) {
        if (width_mb > 120 || width_mb <= 3 || height_mb > 68 || height_mb <= 3) return std::nullopt;
        frame_mbs = width_mb * height_mb;
        if (it->max_fs < frame_mbs) return std::nullopt;
        width = width_mb * 16;
        height = height_mb * 16;
    }
    u32 dpb = std::min<u32>(it->dpb_x10 * 1024 / 10 * 2 / (width * height * 3), 16);
    if (buffers) {
        if (buffers > dpb) return std::nullopt;
        dpb = buffers;
    }
    dpb += 2;
    const u32 min_cr = level > 40 ? 4 : it->min_cr;
    u32 au = static_cast<u32>(u64{it->mbps} * 384384 / (u64{min_cr} * 24000));
    if (au & 127) au = (au & ~127u) + 128;
    au += 8192 + 15360;
    const u32 frame = (width + 64) * height;
    const u32 blocks[] = {0x9B800, frame_mbs * 0x600, frame_mbs * dpb * 256, au * 3 + 256, (frame + frame / 2 + 2048) * dpb,
        768, 6400, 0x1E0000, 6400, 0xAC800, 6528, 0x20000, 256, 512};
    u32 total = 0, any = 0;
    for (const u32 v : blocks) {
        total += v;
        any |= v;
    }
    if (any & 127) return std::nullopt;
    u32 size = total + it->extra + 0x299500;
    if (size % 128) size += 128 - size % 128;
    return size;
}

// Sony's answers (libvdec 4.92, test_vdec.cpp's probe) for the codecs whose memory query is not reproduced: the
// memory for each level without codec specific information.
struct LevelSize {
    u32 codec, level, mem_size;
};
constexpr LevelSize kLevelSizes[] = {
    {CODEC_MPEG2, 0, 0x11290B}, {CODEC_MPEG2, 1, 0x2DFB8B}, {CODEC_MPEG2, 2, 0xA0270B}, {CODEC_MPEG2, 3, 0xD2F40B},
    {CODEC_MPEG4, 0, 0x8B78B}, {CODEC_MPEG4, 1, 0xEFE0B}, {CODEC_MPEG4, 2, 0xEFE0B}, {CODEC_MPEG4, 3, 0x22DB0B},
    {CODEC_MPEG4, 4, 0x1FC00B}, {CODEC_MPEG4, 5, 0x28570B},
    {CODEC_VC1, 0, 0x1FDEFB}, {CODEC_VC1, 1, 0x3298FB}, {CODEC_VC1, 2, 0x3D93FB}, {CODEC_VC1, 3, 0x9E383B},
    {CODEC_VC1, 4, 0x287197B}, {CODEC_VC1, 5, 0x3298FB}, {CODEC_VC1, 6, 0x79DB3B}, {CODEC_VC1, 7, 0x12073FB},
    {CODEC_VC1, 8, 0x202887B}, {CODEC_VC1, 9, 0x3949A7B},
    {CODEC_DIVX, 10, 0x11B720}, {CODEC_DIVX, 11, 0x19A740}, {CODEC_DIVX, 12, 0x386A60}, {CODEC_DIVX, 13, 0x692070},
    {CODEC_DIVX, 14, 0xD78100},
    {CODEC_JVT, 10, 0x336DDB}, {CODEC_JVT, 11, 0x73675B}, {CODEC_JVT, 12, 0x73675B}, {CODEC_JVT, 13, 0x73675B},
    {CODEC_JVT, 20, 0x73675B}, {CODEC_JVT, 21, 0xC6CB5B}, {CODEC_JVT, 22, 0x175A75B}, {CODEC_JVT, 30, 0x175A75B},
    {CODEC_JVT, 31, 0x31FBCDB}, {CODEC_JVT, 32, 0x444FDDB}, {CODEC_JVT, 40, 0x6B4D7DB}, {CODEC_JVT, 41, 0x6B4D7DB},
    {CODEC_JVT, 42, 0x6B4D7DB},
    {CODEC_DIVX3_11, 15, 0x1E3578},
    {CODEC_MVC, 10, 0xA2E0FD}, {CODEC_MVC, 11, 0xC37A7D}, {CODEC_MVC, 12, 0xE9047D}, {CODEC_MVC, 13, 0xEBE47D},
    {CODEC_MVC, 20, 0xEBE47D}, {CODEC_MVC, 21, 0x143CAFD}, {CODEC_MVC, 22, 0x1B058FD}, {CODEC_MVC, 30, 0x1BA3EFD},
    {CODEC_MVC, 31, 0x2911E7D}, {CODEC_MVC, 32, 0x3253E7D}, {CODEC_MVC, 40, 0x48CA47D}, {CODEC_MVC, 41, 0x48CA47D},
    {CODEC_MVC, 42, 0x48CA47D}, {CODEC_MVC, AVC_LEVEL_UNK, 0x48CA47D},
};

u32 version_lower(u32 codec)
{
    switch (codec) {
    case CODEC_MPEG2: return 0x1030000;
    case CODEC_AVC: return 0x11300;
    case CODEC_MPEG4: return 0x1080000;
    case CODEC_VC1: return 0x1090000;
    case CODEC_DIVX: return 0x30806;
    case CODEC_JVT: return 0x1050000;
    case CODEC_DIVX3_11: return 0x103;
    default: return 0x10804; // MVC
    }
}

// RPCS3's old-SDK sizes (before SDK 2.1.0 libvdec gives the level's largest): H.264's, by level.
u32 avc_old_sdk_size(u32 level)
{
    switch (level) {
    case 10: return 0xA014FD;
    case 11: return 0xB6C9FD;
    case 12: return 0xCE2D7D;
    case 13:
    case 20: return 0xD054FD;
    case 21: return 0x1190AFD;
    case 22: return 0x17E46FD;
    case 30: return 0x185B17D;
    case 31: return 0x1FD2AFD;
    case 32: return 0x2696F7D;
    default: return 0x36A527D;
    }
}

struct Attr {
    u32 mem_size = 0;
    u8 cmd_depth = 4;
    u32 version = 0;
};

// cellVdecQueryAttrEx's answer for a codec, level and codec specific information (0: none), or an error.
s32 query_attr(PPUContext& ctx, u32 codec, u32 level, u32 spec, Attr& out)
{
    u8* base = guest_base(ctx);
    const bool valid_codec = codec <= CODEC_MPEG4 || (codec < CODEC_MAX && (codec & 1));
    if (!valid_codec) return error(CELL_VDEC_ERROR_ARG);
    out.version = version_lower(codec);
    const bool new_sdk = thread_of(ctx).kernel.config().sdk_version >= 0x210000;
    if (codec == CODEC_AVC) {
        u32 lv = level, deblock_off = 0, buffers = 0, w = 0, h = 0;
        if (level == AVC_LEVEL_UNK) {
            lv = 42;
            deblock_off = 1;
        }
        if (spec) {
            if (!guest_readable(thread_of(ctx), spec, 10) || ps_read32(base, spec) != 12) return error(CELL_VDEC_ERROR_ARG);
            const u32 sw = ps_read16(base, spec + 4), sh = ps_read16(base, spec + 6);
            deblock_off = ps_read8(base, spec + 8);
            buffers = ps_read8(base, spec + 9);
            if (sw && sh) {
                w = (sw + 15) / 16;
                h = (sh + 15) / 16;
            }
        }
        const std::optional<u32> mem = avc_memory(lv, deblock_off, buffers, w, h);
        if (!mem) return error(CELL_VDEC_ERROR_ARG);
        const u32 decoder = 0x41580 + *mem + 0x89FD;
        const u32 unk = (0x244 + 0x90) * 0x30 + (0x30 * 0x14 + 0x14) * 2 + 0x40; // libvdec's own (RPCS3 get_unk_size)
        out.mem_size = new_sdk ? decoder + 0x858 + unk : avc_old_sdk_size(lv);
        return lv2::CELL_OK;
    }
    const auto it = std::find_if(std::begin(kLevelSizes), std::end(kLevelSizes), [&](const LevelSize& s) {
        return (s.codec == codec || (codec == CODEC_MVC2 && s.codec == CODEC_MVC)) && s.level == level;
    });
    if (it == std::end(kLevelSizes)) return error(CELL_VDEC_ERROR_ARG);
    if (spec && codec == CODEC_DIVX3_11) {
        // CellVdecDivxSpecificInfo2 (RPCS3 cellVdec.h: thisSize, maxDecodedFrameWidth, maxDecodedFrameHeight,
        // numberOfDecodedFrameBuffer), the only form DivX 3.11 takes, checked as RPCS3's query_attr_divx checks it.
        if (!guest_readable(thread_of(ctx), spec, 10) || ps_read32(base, spec) != kDivxSpecificInfo2Size ||
            ps_read16(base, spec + 4) > kDivx311MaxWidth || ps_read16(base, spec + 6) > kDivx311MaxHeight ||
            (ps_read16(base, spec + 8) != 0 && ps_read16(base, spec + 8) != 2))
            return error(CELL_VDEC_ERROR_ARG);
    }
    if (spec)
        PS_LOG_WARN(kChan, "cellVdecQueryAttr: codec {} specific information ({:#x}) is not reproduced; Sony's size "
                           "without it is given", codec, spec);
    out.mem_size = it->mem_size;
    return lv2::CELL_OK;
}

// ---- the H.264 stream: what CellVdecAvcInfo reports, from the access unit's NAL units

class Bits {
public:
    explicit Bits(std::vector<u8> rbsp) : d_(std::move(rbsp)) {}
    u32 bit()
    {
        if (pos_ >= d_.size() * 8) {
            over_ = true;
            return 0;
        }
        const u32 b = (d_[pos_ / 8] >> (7 - pos_ % 8)) & 1;
        pos_++;
        return b;
    }
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++) v = v << 1 | bit();
        return v;
    }
    u32 ue()
    {
        u32 zeros = 0;
        while (!bit() && !over_ && zeros < 32) zeros++;
        return zeros >= 32 ? 0 : ((1u << zeros) - 1 + bits(zeros));
    }
    s32 se()
    {
        const u32 v = ue();
        return (v & 1) ? static_cast<s32>((v + 1) / 2) : -static_cast<s32>(v / 2);
    }
    [[nodiscard]] bool over() const { return over_; }

private:
    std::vector<u8> d_;
    usize pos_ = 0;
    bool over_ = false;
};

// A NAL unit's payload without its emulation prevention bytes.
std::vector<u8> rbsp(std::span<const u8> nal)
{
    std::vector<u8> out;
    out.reserve(nal.size());
    u32 zeros = 0;
    for (const u8 b : nal) {
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

struct AvcSps {
    bool valid = false;
    u32 width = 0, height = 0, poc_type = 0;
    bool frame_mbs_only = true, vui = false, video_signal_type = false, full_range = false, colour_description = false,
         timing = false, fixed_frame_rate = false, low_delay_hrd = false;
    u8 aspect_ratio_idc = 0, video_format = 0, primaries = 0, transfer = 0, matrix = 0;
    u16 sar_width = 0, sar_height = 0;
    u32 units_in_tick = 0, time_scale = 0;
};

void skip_scaling_list(Bits& b, u32 size)
{
    s32 last = 8, next = 8;
    for (u32 j = 0; j < size; j++) {
        if (next) next = (last + b.se() + 256) % 256;
        last = next ? next : last;
    }
}

// hrd_parameters(): the CPB specifications, then four lengths.
void skip_hrd(Bits& b)
{
    const u32 count = b.ue() + 1;
    b.bits(8); // bit_rate_scale, cpb_size_scale
    for (u32 i = 0; i < count && !b.over(); i++) {
        b.ue();
        b.ue();
        b.bit();
    }
    b.bits(5 + 5 + 5 + 5);
}

AvcSps parse_sps(std::span<const u8> nal)
{
    AvcSps s;
    Bits b(rbsp(nal.subspan(1)));
    const u32 profile = b.bits(8);
    b.bits(16); // constraint flags, level
    b.ue();     // seq_parameter_set_id
    u32 chroma_format = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128) {
        chroma_format = b.ue();
        if (chroma_format == 3) b.bit();
        b.ue();
        b.ue();
        b.bit();
        if (b.bit()) {
            for (u32 i = 0; i < (chroma_format != 3 ? 8u : 12u); i++)
                if (b.bit()) skip_scaling_list(b, i < 6 ? 16 : 64);
        }
    }
    b.ue(); // log2_max_frame_num_minus4
    s.poc_type = b.ue();
    if (s.poc_type == 0) {
        b.ue();
    } else if (s.poc_type == 1) {
        b.bit();
        b.se();
        b.se();
        const u32 n = b.ue();
        for (u32 i = 0; i < n && !b.over(); i++) b.se();
    }
    b.ue();  // max_num_ref_frames
    b.bit(); // gaps_in_frame_num_value_allowed_flag
    const u32 w_mbs = b.ue() + 1, h_units = b.ue() + 1;
    s.frame_mbs_only = b.bit();
    if (!s.frame_mbs_only) b.bit();
    b.bit(); // direct_8x8_inference_flag
    u32 crop[4] = {};
    if (b.bit())
        for (u32& c : crop) c = b.ue();
    const u32 cx = chroma_format == 0 || chroma_format == 3 ? 1 : 2, cy = (chroma_format == 1 ? 2 : 1) * (2 - s.frame_mbs_only);
    s.width = w_mbs * 16 - cx * (crop[0] + crop[1]);
    s.height = h_units * 16 * (2 - s.frame_mbs_only) - cy * (crop[2] + crop[3]);
    s.vui = b.bit();
    if (s.vui) {
        if (b.bit()) {
            s.aspect_ratio_idc = static_cast<u8>(b.bits(8));
            if (s.aspect_ratio_idc == 255) {
                s.sar_width = static_cast<u16>(b.bits(16));
                s.sar_height = static_cast<u16>(b.bits(16));
            }
        }
        if (b.bit()) b.bit(); // overscan
        s.video_signal_type = b.bit();
        if (s.video_signal_type) {
            s.video_format = static_cast<u8>(b.bits(3));
            s.full_range = b.bit();
            s.colour_description = b.bit();
            if (s.colour_description) {
                s.primaries = static_cast<u8>(b.bits(8));
                s.transfer = static_cast<u8>(b.bits(8));
                s.matrix = static_cast<u8>(b.bits(8));
            }
        }
        if (b.bit()) { // chroma_loc_info
            b.ue();
            b.ue();
        }
        s.timing = b.bit();
        if (s.timing) {
            s.units_in_tick = b.bits(32);
            s.time_scale = b.bits(32);
            s.fixed_frame_rate = b.bit();
        }
        const bool nal_hrd = b.bit();
        if (nal_hrd) skip_hrd(b);
        const bool vcl_hrd = b.bit();
        if (vcl_hrd) skip_hrd(b);
        if (nal_hrd || vcl_hrd) s.low_delay_hrd = b.bit();
    }
    s.valid = !b.over() || s.width;
    return s;
}

// What one access unit says: its NAL units (CELL_VDEC_AVC_FLG_*), IDR, entropy coding, and the latest SPS.
struct AvcAuInfo {
    u16 nal_flags = 0;
    bool idr = false, entropy_cabac = false;
};

void scan_avc_au(std::span<const u8> au, AvcAuInfo& info, AvcSps& sps)
{
    usize i = 0;
    const auto next_start = [&](usize from) {
        for (usize k = from; k + 3 <= au.size(); k++)
            if (au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) return k;
        return au.size();
    };
    i = next_start(0);
    while (i < au.size()) {
        const usize begin = i + 3;
        usize end = next_start(begin);
        const usize next = end;
        while (end > begin && au[end - 1] == 0) end--; // trailing zeros (and a 4-byte start code's first byte)
        if (begin < end) {
            const std::span<const u8> nal = au.subspan(begin, end - begin);
            switch (nal[0] & 0x1F) {
            case 5: info.idr = true; break;
            case 6: { // SEI: the payload types present
                usize p = 1;
                const std::vector<u8> r = rbsp(nal);
                while (p < r.size() && r[p] != 0x80) {
                    u32 type = 0, size = 0;
                    while (p < r.size() && r[p] == 0xFF) type += r[p++];
                    if (p < r.size()) type += r[p++];
                    while (p < r.size() && r[p] == 0xFF) size += r[p++];
                    if (p < r.size()) size += r[p++];
                    if (type == 0) info.nal_flags |= 0x0800;
                    if (type == 1) info.nal_flags |= 0x0400;
                    if (type == 5) info.nal_flags |= 0x1000;
                    p += size;
                }
                break;
            }
            case 7:
                info.nal_flags |= 0x0001;
                sps = parse_sps(nal);
                break;
            case 8: {
                info.nal_flags |= 0x0002;
                Bits b(rbsp(nal.subspan(1)));
                b.ue();
                b.ue();
                info.entropy_cabac = b.bit();
                break;
            }
            case 9: info.nal_flags |= 0x0004; break;
            case 10: info.nal_flags |= 0x0008; break;
            case 11: info.nal_flags |= 0x0100; break;
            case 12: info.nal_flags |= 0x0200; break;
            default: break;
            }
        }
        i = next;
    }
}

// The index of a frame rate among the eight the codecs name (CELL_VDEC_FRC_* less 0x80, CELL_VDEC_AVC_FRC_*):
// 24000/1001, 24, 25, 30000/1001, 30, 50, 60000/1001, 60; 0xFF for another.
u8 frame_rate_index(double fps)
{
    const struct {
        double fps;
        u8 code;
    } codes[] = {{23.976, 0}, {24, 1}, {25, 2}, {29.97, 3}, {30, 4}, {50, 5}, {59.94, 6}, {60, 7}};
    for (const auto& c : codes)
        if (std::abs(fps - c.fps) < 0.002) return c.code;
    return 0xFF;
}

// CELL_VDEC_AVC_FRC_* from the VUI timing (two ticks a frame), 0xFF when it names none.
u8 avc_frame_rate_code(const AvcSps& s)
{
    if (!s.timing || !s.units_in_tick) return 0xFF;
    return frame_rate_index(static_cast<double>(s.time_scale) / (2.0 * s.units_in_tick));
}

// ---- the decoder

// Sony's places in the decoder's memory (handle = its address): picture items, H.264 information, then pspocket's
// frame buffers (Sony's layout; their place is pspocket's own).
constexpr u32 kItems = 0xDC, kItemStride = 0x90, kItemSize = 0x4C, kInfos = 0x1B9C, kInfoStride = 0x244, kRing = 16;
constexpr u32 kFrames = (kInfos + kRing * kInfoStride + 0x7F) & ~0x7Fu;
constexpr u32 kOutMax = 60; // decoded pictures the game has not taken (RPCS3 out_max)

enum class Seq : u32 { closed, dormant, ready, ending, resetting };

struct AuMeta {
    u64 index = 0, seq = 0;
    u32 pts_upper = 0, pts_lower = 0, dts_upper = 0, dts_lower = 0;
    u64 user_data = 0;
    s32 attr = 0;
    AvcAuInfo avc;
    AvcSps sps;
};

struct Picture {
    u64 seq = 0;
    u64 au = 0; // the access unit it came from
    u32 number = 0; // pictures before it in the decoder's life: its ring slot
    u32 width = 0, height = 0;
    int pict_type = 0;
    std::vector<u8> y, u, v; // planar 4:2:0, no padding
    AuMeta meta;
    bool item_taken = false;
    // What FFmpeg reports of the stream with the picture (CellVdecDivxInfo): the pixel aspect ratio (0/0: none), the
    // colour description (ISO/IEC 23001-8 codes, as MPEG-4's; 2: unspecified), the field order and the frame rate.
    s32 sar_num = 0, sar_den = 0;
    u8 primaries = 2, transfer = 2, matrix = 2;
    bool interlaced = false, top_field_first = false;
    double fps = 0;
};

struct Command {
    enum Type { start, au, drain, end, close, framerate };
    explicit Command(Type t = start) : type(t) {}
    Type type;
    u64 seq = 0;
    s32 mode = 0;
    u32 au_addr = 0, au_size = 0;
    AuMeta meta;
    u32 frame_rate = 0;
};

struct Vdec {
    u32 handle = 0, mem_size = 0, codec = 0, level = 0, cb_func = 0, cb_arg = 0;
    std::mutex m;
    Seq state = Seq::closed;
    u64 seq = 0, next_au = 0;
    u32 pending_aus = 0; // DecodeAu accepted, AUDONE not yet sent (Sony's command depth)
    std::deque<Command> commands;
    lv2::WaitQueue waiters; // the decoder's thread, waiting for a command
    std::deque<Picture> held, out; // decoded but not announced; announced, for the game
    u32 pictures = 0, frames = 0, frame_size = 0, frame_width = 0, frame_height = 0;
    u32 frame_rate = 0; // cellVdecSetFrameRate
    u64 thread_id = 0;
    u32 tls = 0;
    AvcSps sps;
    u32 divx_width = kDivx311Width, divx_height = kDivx311Height; // DivX 3.11's frame size (the stream has none)
#if PSPOCKET_HAVE_FFMPEG
    AVCodecContext* av = nullptr;
#endif
    ~Vdec()
    {
#if PSPOCKET_HAVE_FFMPEG
        avcodec_free_context(&av);
#endif
    }
};

struct Vdecs {
    std::mutex m;
    std::map<u32, std::shared_ptr<Vdec>> by_handle;
};

Vdecs& vdecs()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellVdec called without an HLE process");
    return p->state<Vdecs>();
}

std::shared_ptr<Vdec> vdec_of(u32 handle)
{
    Vdecs& all = vdecs();
    std::lock_guard lock(all.m);
    const auto it = all.by_handle.find(handle);
    return it == all.by_handle.end() ? nullptr : it->second;
}

template <class Ready>
void park_until(lv2::PpuThread& t, Vdec& v, std::unique_lock<std::mutex>& lk, Ready ready)
{
    while (!ready()) {
        lv2::prepare_wait(t.waiter);
        v.waiters.push(&t.waiter);
        const lv2::WaitStatus status = lv2::park(t.waiter, lk, 0, {lv2::WaitKind::Sleep, 0}, [&](bool) {
            return v.waiters.remove(&t.waiter) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
        });
        if (status == lv2::WaitStatus::Aborted) throw lv2::ThreadExit{};
        lk.lock();
    }
}

// Under v.m.
void push_command(Vdec& v, Command c)
{
    v.commands.push_back(std::move(c));
    for (lv2::Waiter* w : v.waiters.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

void callback(PPUContext& ctx, const Vdec& v, s32 type)
{
    call_guest(ctx, v.cb_func, {v.handle, static_cast<u64>(static_cast<u32>(type)), 0, v.cb_arg});
}

// Sony's frame buffer layout for a picture (luma, then chroma): 16-byte strips, two of repeated edge on each side.
u32 frame_size(u32 width, u32 height)
{
    const u32 strips = (width + 15) / 16 + 4;
    return strips * 16 * height + 0x400 + strips * 16 * (height / 2) + 0x400;
}

void write_frame(u8* base, u32 at, const Picture& p)
{
    const u32 strips = (p.width + 15) / 16 + 4, cw = p.width / 2, ch = p.height / 2;
    for (u32 s = 0; s < strips; s++) {
        for (u32 row = 0; row < p.height; row++) {
            for (u32 k = 0; k < 16; k++) {
                const s32 x = std::clamp<s32>(static_cast<s32>(s * 16 + k) - 32, 0, static_cast<s32>(p.width) - 1);
                base[at + s * 16 * p.height + row * 16 + k] = p.y[row * p.width + static_cast<u32>(x)];
            }
        }
    }
    const u32 luma_end = strips * 16 * p.height;
    std::memset(base + at + luma_end, 0, 0x400);
    const u32 chroma = at + luma_end + 0x400;
    for (u32 s = 0; s < strips; s++) {
        for (u32 row = 0; row < ch; row++) {
            for (u32 k = 0; k < 8; k++) {
                const s32 x = std::clamp<s32>(static_cast<s32>(s * 8 + k) - 16, 0, static_cast<s32>(cw) - 1);
                base[chroma + s * 16 * ch + row * 16 + k] = p.u[row * cw + static_cast<u32>(x)];
                base[chroma + s * 16 * ch + row * 16 + 8 + k] = p.v[row * cw + static_cast<u32>(x)];
            }
        }
    }
    std::memset(base + chroma + strips * 16 * ch, 0, 0x400);
}

#if PSPOCKET_HAVE_FFMPEG
const AVCodec* decoder_for(u32 codec)
{
    switch (codec) {
    case CODEC_AVC:
    case CODEC_JVT: return avcodec_find_decoder(AV_CODEC_ID_H264);
    case CODEC_MPEG2: return avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    case CODEC_MPEG4:
    case CODEC_DIVX: return avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    case CODEC_DIVX3_11: return avcodec_find_decoder(AV_CODEC_ID_MSMPEG4V3); // Microsoft's MPEG-4 version 3 (D-588)
    default: return nullptr;
    }
}

// Opens FFmpeg's decoder for the decoder's codec, with the frame size DivX 3.11's frames do not carry. False when
// FFmpeg has no decoder for it or refuses it (logged).
bool open_ffmpeg(Vdec& v, std::string_view call)
{
    const AVCodec* dec = decoder_for(v.codec);
    if (!dec) {
        PS_LOG_ERROR(kChan, "{}: codec {} is not decoded by pspocket (no decoder in its FFmpeg)", call, v.codec);
        return false;
    }
    v.av = avcodec_alloc_context3(dec);
    v.av->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    v.av->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    v.av->thread_count = 1; // pictures out as soon as decoded: Sony's lag is added in announce()
    if (v.codec == CODEC_DIVX3_11) {
        v.av->width = v.av->coded_width = static_cast<int>(v.divx_width);
        v.av->height = v.av->coded_height = static_cast<int>(v.divx_height);
    }
    const bool hardware = vdec_hw::attach(v.av); // H.264 on the media engine (D-254)
    if (avcodec_open2(v.av, dec, nullptr) < 0) {
        PS_LOG_ERROR(kChan, "{}: FFmpeg's decoder for codec {} did not open", call, v.codec);
        avcodec_free_context(&v.av);
        return false;
    }
    PS_LOG_INFO(kChan, "{}: codec {} decodes {}", call, v.codec, hardware ? "on VideoToolbox" : "in software");
    return true;
}

// The pictures the decoder has ready, with the access units they came from.
void receive_pictures(Vdec& v, std::map<u64, AuMeta>& in_flight, u64 seq)
{
    AVFrame* f = av_frame_alloc();
    while (avcodec_receive_frame(v.av, f) == 0) {
        Picture p;
        p.seq = seq;
        p.au = static_cast<u64>(reinterpret_cast<uintptr_t>(f->opaque));
        if (const auto it = in_flight.find(p.au); it != in_flight.end()) p.meta = it->second;
        p.width = static_cast<u32>(f->width);
        p.height = static_cast<u32>(f->height);
        p.pict_type = f->pict_type;
        p.sar_num = f->sample_aspect_ratio.num;
        p.sar_den = f->sample_aspect_ratio.den;
        p.primaries = static_cast<u8>(f->color_primaries);
        p.transfer = static_cast<u8>(f->color_trc);
        p.matrix = static_cast<u8>(f->colorspace);
        p.interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        p.top_field_first = (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
        if (v.av->framerate.num > 0 && v.av->framerate.den > 0) p.fps = av_q2d(v.av->framerate);
        // 4:2:0 from FFmpeg's software decoder, or from the media engine's pixel buffer (D-254).
        if (!vdec_hw::planes(f, p.width, p.height, p.y, p.u, p.v)) {
            PS_LOG_ERROR(kChan, "decoder {:#x}: a picture in pixel format {} (not 8-bit 4:2:0) is not supported", v.handle,
                f->format);
            av_frame_unref(f);
            continue;
        }
        v.held.push_back(std::move(p));
        av_frame_unref(f);
    }
    av_frame_free(&f);
}
#endif

// Announces the held pictures (all, or those of access units before `before`): each goes to the game's queue when
// it has room, then PICOUT.
void announce(PPUContext& ctx, Vdec& v, std::optional<u64> before)
{
    for (;;) {
        Picture p;
        {
            std::unique_lock lk(v.m);
            if (v.held.empty() || (before && v.held.front().au >= *before)) return;
            if (v.out.size() >= kOutMax) {
                PS_LOG_WARN(kChan, "decoder {:#x}: {} pictures not taken; the oldest is dropped", v.handle, v.out.size());
                v.out.pop_front();
            }
            p = std::move(v.held.front());
            v.held.pop_front();
            if (p.seq != v.seq) continue; // a sequence restarted meanwhile
            p.number = v.pictures++;
            // The frame buffers: as many of the picture's size as fit after the rings, at most 16 (Sony's count and
            // place are its own).
            if (p.width != v.frame_width || p.height != v.frame_height) {
                v.frame_width = p.width;
                v.frame_height = p.height;
                v.frame_size = (frame_size(p.width, p.height) + 0x7F) & ~0x7Fu;
                v.frames = std::min<u32>(kRing, (v.mem_size - kFrames) / v.frame_size);
            }
            if (v.frames) write_frame(guest_base(ctx), v.handle + kFrames + (p.number % v.frames) * v.frame_size, p);
            v.out.push_back(std::move(p));
        }
        callback(ctx, v, MSG_PICOUT);
    }
}

// The decoder's thread (an lv2 PPU thread, so the game's callback runs on a guest thread at the priority it asked):
// the commands in order.
void run(PPUContext& ctx, Vdec& v)
{
    lv2::PpuThread& t = thread_of(ctx);
    [[maybe_unused]] std::map<u64, AuMeta> in_flight; // by access unit index, until its pictures are out
    for (;;) {
        Command c;
        {
            std::unique_lock lk(v.m);
            park_until(t, v, lk, [&] { return !v.commands.empty(); });
            c = std::move(v.commands.front());
            v.commands.pop_front();
        }
        switch (c.type) {
        case Command::start: {
#if PSPOCKET_HAVE_FFMPEG
            if (v.av) avcodec_flush_buffers(v.av);
#endif
            in_flight.clear();
            std::lock_guard lk(v.m);
            v.held.clear();
            v.out.clear();
            break;
        }
        case Command::au:
        case Command::drain: {
#if PSPOCKET_HAVE_FFMPEG
            u64 seq = 0;
            {
                std::lock_guard lk(v.m);
                seq = v.seq;
            }
            if (c.seq == seq && v.av) {
                if (c.type == Command::au) {
                    AVPacket* pkt = av_packet_alloc();
                    pkt->data = guest_base(ctx) + c.au_addr;
                    pkt->size = static_cast<int>(c.au_size);
                    pkt->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(c.meta.index));
                    v.av->skip_frame = c.mode == DEC_MODE_NORMAL ? AVDISCARD_DEFAULT
                                       : c.mode == DEC_MODE_B_SKIP ? AVDISCARD_NONREF
                                                                   : AVDISCARD_NONINTRA;
                    in_flight[c.meta.index] = c.meta;
                    if (const int r = avcodec_send_packet(v.av, pkt); r < 0)
                        PS_LOG_ERROR(kChan, "decoder {:#x}: access unit {} ({:#x}, {} bytes) refused by the decoder ({})",
                            v.handle, c.meta.index, c.au_addr, c.au_size, r);
                    av_packet_free(&pkt);
                } else {
                    avcodec_send_packet(v.av, nullptr);
                }
                receive_pictures(v, in_flight, seq);
            }
#endif
            if (c.type == Command::au) {
                {
                    std::lock_guard lk(v.m);
                    if (v.pending_aus) v.pending_aus--;
                }
                callback(ctx, v, MSG_AUDONE);
                announce(ctx, v, c.meta.index); // the pictures of earlier access units: Sony's one-unit lag
            } else {
                announce(ctx, v, std::nullopt);
#if PSPOCKET_HAVE_FFMPEG
                if (v.av) avcodec_flush_buffers(v.av);
#endif
            }
            break;
        }
        case Command::end: {
            {
                std::lock_guard lk(v.m);
                v.state = Seq::dormant;
            }
            callback(ctx, v, MSG_SEQDONE);
            break;
        }
        case Command::framerate: v.frame_rate = c.frame_rate; break;
        case Command::close: return;
        }
    }
}

} // namespace

constexpr u32 NID_vdec_thread = 0xE7BB0813; // nid_of("_pspocket_vdec_thread"): pspocket's own

void _pspocket_vdec_thread(PPUContext& ctx, u64 handle)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (const std::shared_ptr<Vdec> v = vdec_of(static_cast<u32>(handle))) run(ctx, *v);
    else PS_LOG_ERROR(kChan, "the decoder thread started for an unknown decoder {:#x}", handle);
    lv2::_sys_ppu_thread_exit(t, 0);
}

namespace {

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("cellVdec: {} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

s32 finish(s32 r, const std::string& call)
{
    if (r < 0) PS_LOG_WARN(kChan, "{}: {:#010x}", call, static_cast<u32>(r));
    else PS_LOG_DEBUG(kChan, "{}: CELL_OK", call);
    return r;
}

// RPCS3 vdecOpen, with Sony's handle and memory use (see the top).
s32 open(PPUContext& ctx, u32 type, u32 spec, u32 res, u32 cb, u32 handle_out, const std::string& call)
{
    u8* base = guest_base(ctx);
    if (!type || !res || !cb || !handle_out || !usable(ctx, type, 8, call) || !usable(ctx, res, 0x18, call) ||
        !usable(ctx, cb, 8, call) || !usable(ctx, handle_out, 4, call) || !ps_read32(base, cb))
        return error(CELL_VDEC_ERROR_ARG);
    const u32 mem = ps_read32(base, res), mem_size = ps_read32(base, res + 4);
    const u32 ppu_prio = ps_read32(base, res + 8), stack = ps_read32(base, res + 12), spu_prio = ps_read32(base, res + 16);
    const u32 codec = ps_read32(base, type), level = ps_read32(base, type + 4);
    if (!mem || ppu_prio >= 3072 || spu_prio >= 256 || stack < 4096 || codec >= CODEC_MAX) return error(CELL_VDEC_ERROR_ARG);
    Attr attr;
    if (const s32 r = query_attr(ctx, codec, level, spec, attr); r != lv2::CELL_OK) return r;
    if (mem_size < kFrames || !usable(ctx, mem, mem_size, "the decoder's memory")) return error(CELL_VDEC_ERROR_ARG);
    if (mem_size < attr.mem_size)
        PS_LOG_WARN(kChan, "{}: {:#x} bytes, less than the {:#x} asked for (Sony's libvdec opens anyway)", call, mem_size,
            attr.mem_size);
    auto v = std::make_shared<Vdec>();
    v->handle = mem;
    v->mem_size = mem_size;
    v->codec = codec;
    v->level = level;
    v->cb_func = ps_read32(base, cb);
    v->cb_arg = ps_read32(base, cb + 4);
    if (codec == CODEC_DIVX3_11) {
        // DivX 3.11's frame size (D-588): the stream has none, so the codec specific information's largest frame
        // (query_attr checked it) is the picture's, else RPCS3's default for the decoder.
        if (spec && ps_read16(base, spec + 4) && ps_read16(base, spec + 6)) {
            v->divx_width = ps_read16(base, spec + 4);
            v->divx_height = ps_read16(base, spec + 6);
        } else {
            PS_LOG_WARN(kChan, "{}: DivX 3.11 without its frame size (CellVdecDivxSpecificInfo2): {}x{} assumed", call,
                kDivx311Width, kDivx311Height);
        }
    }
#if PSPOCKET_HAVE_FFMPEG
    if (!open_ffmpeg(*v, call)) return error(CELL_VDEC_ERROR_FATAL);
#else
    PS_LOG_ERROR(kChan, "{}: pspocket was built without FFmpeg: no video decoding", call);
    return error(CELL_VDEC_ERROR_FATAL);
#endif
    v->state = Seq::dormant;
    {
        Vdecs& all = vdecs();
        std::lock_guard lock(all.m);
        if (const auto it = all.by_handle.find(mem); it != all.by_handle.end()) {
            std::lock_guard vl(it->second->m);
            if (it->second->state != Seq::closed) return error(CELL_VDEC_ERROR_FATAL); // a decoder there already (Sony's)
        }
        all.by_handle[mem] = v;
    }
    // The decoder's thread.
    lv2::PpuThread& t = thread_of(ctx);
    HleProcess& p = *HleProcess::current();
    const u32 entry = p.descriptor_of("libvdec", NID_vdec_thread);
    v->tls = p.alloc_tls();
    StackVar sv(ctx, base, 0x40);
    ps_write32(base, sv.addr() + 8, entry);
    ps_write32(base, sv.addr() + 12, v->tls ? v->tls + kTlsR13Offset : 0);
    std::memcpy(base + sv.addr() + 0x10, "HLE Video Decoder", 18);
    s32 r = entry ? lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, mem, 0, static_cast<s32>(ppu_prio), stack, 1,
                        sv.addr() + 0x10)
                  : error(CELL_VDEC_ERROR_FATAL);
    if (r == lv2::CELL_OK) r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
    if (r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "{}: the decoder's thread: {:#x}{}", call, static_cast<u32>(r), entry ? "" : " (not linked)");
        if (v->tls) p.free_tls(v->tls);
        Vdecs& all = vdecs();
        std::lock_guard lock(all.m);
        all.by_handle.erase(mem);
        return error(CELL_VDEC_ERROR_FATAL);
    }
    v->thread_id = ps_read64(base, sv.addr());
    ps_write32(base, handle_out, mem);
    return lv2::CELL_OK;
}

// The decoder at `handle`, open or closed: a closed one stays known (Sony's context stays in its memory), so a call on
// it is SEQ, not ARG.
std::shared_ptr<Vdec> live(PPUContext&, u32 handle) { return vdec_of(handle); }

// CellVdecDivxInfo, RPCS3's structure (rpcs3/Emu/Cell/Modules/cellVdec.h), which RPCS3 writes for MPEG-4 and DivX
// (D-588): pictureType (u8), horizontalSize and verticalSize (u16 at 2 and 4), pixelAspectRatio, parWidth, parHeight,
// colourDescription, colourPrimaries, transferCharacteristics, matrixCoefficients and pictureStruct (u8 at 6 to 13),
// frameRateCode (u16 at 14). From the stream where it says (MPEG-4's VOL through FFmpeg), else RPCS3's values: a
// square pixel, no colour description with BT.709's codes, a frame. The frame rate is SetFrameRate's, else the
// stream's, else CELL_VDEC_DIVX_FRC_UNDEFINED (RPCS3 gives 30, a guess; DivX 3.11's stream names none).
void write_divx_info(u8* base, u32 info, const Vdec& v, const Picture& p)
{
#if PSPOCKET_HAVE_FFMPEG
    const u8 type = p.pict_type == AV_PICTURE_TYPE_B ? 2 : p.pict_type == AV_PICTURE_TYPE_I ? 0 : 1; // P, S (sprite)
#else
    const u8 type = 0;
#endif
    ps_write8(base, info + 0x00, type);
    ps_write16(base, info + 0x02, static_cast<u16>(p.width));
    ps_write16(base, info + 0x04, static_cast<u16>(p.height));
    // CELL_VDEC_DIVX_ARI_PAR_*: MPEG-4's aspect_ratio_info, 15 with the ratio itself for another.
    u8 par = 1, par_width = 0, par_height = 0;
    if (p.sar_num > 0 && p.sar_den > 0) {
        const struct {
            s32 num, den;
            u8 code;
        } pars[] = {{1, 1, 1}, {12, 11, 2}, {10, 11, 3}, {16, 11, 4}, {40, 33, 5}};
        par = 15;
        par_width = static_cast<u8>(std::min(p.sar_num, 255));
        par_height = static_cast<u8>(std::min(p.sar_den, 255));
        for (const auto& k : pars)
            if (s64{p.sar_num} * k.den == s64{p.sar_den} * k.num) {
                par = k.code;
                par_width = par_height = 0;
            }
    }
    ps_write8(base, info + 0x06, par);
    ps_write8(base, info + 0x07, par_width);
    ps_write8(base, info + 0x08, par_height);
    const bool colour = p.primaries != 2 || p.transfer != 2 || p.matrix != 2;
    ps_write8(base, info + 0x09, colour);
    ps_write8(base, info + 0x0A, colour ? p.primaries : 1);
    ps_write8(base, info + 0x0B, colour ? p.transfer : 1);
    ps_write8(base, info + 0x0C, colour ? p.matrix : 1);
    ps_write8(base, info + 0x0D, !p.interlaced ? 0 : p.top_field_first ? 1 : 2); // FRAME, TOP_BTM, BTM_TOP
    // CELL_VDEC_DIVX_FRC_*: 0 undefined, then the eight rates from 24000/1001.
    const u8 index = v.frame_rate ? static_cast<u8>(v.frame_rate & 7) : frame_rate_index(p.fps);
    ps_write16(base, info + 0x0E, index == 0xFF ? 0 : static_cast<u16>(index + 1));
}

// CellVdecPicItem and the codec's information for the picture at the front of the game's queue whose item is not
// taken yet, in the rings (see the top).
void write_item(u8* base, const Vdec& v, const Picture& p, u32 item, u32 info)
{
    std::memset(base + item, 0, kItemSize);
    ps_write32(base, item + 0x00, v.codec);
    ps_write32(base, item + 0x04, v.frames ? v.handle + kFrames + (p.number % v.frames) * v.frame_size : 0);
    ps_write32(base, item + 0x08, p.width * p.height * 3 / 2);
    ps_write8(base, item + 0x0C, 1); // auNum
    ps_write32(base, item + 0x10, p.meta.pts_upper);
    ps_write32(base, item + 0x14, p.meta.pts_lower);
    ps_write64(base, item + 0x18, ~0ull);
    ps_write32(base, item + 0x20, p.meta.dts_upper);
    ps_write32(base, item + 0x24, p.meta.dts_lower);
    ps_write64(base, item + 0x28, ~0ull);
    ps_write64(base, item + 0x30, p.meta.user_data);
    ps_write64(base, item + 0x38, 0);
    ps_write32(base, item + 0x40, 0); // status
    ps_write32(base, item + 0x44, static_cast<u32>(p.meta.attr));
    ps_write32(base, item + 0x48, info);
    std::memset(base + info, 0, kInfoStride);
    if (v.codec == CODEC_AVC || v.codec == CODEC_JVT) {
        const AvcSps& s = p.meta.sps;
        ps_write16(base, info + 0x00, static_cast<u16>(p.width));
        ps_write16(base, info + 0x02, static_cast<u16>(p.height));
#if PSPOCKET_HAVE_FFMPEG
        ps_write8(base, info + 0x04, p.pict_type == AV_PICTURE_TYPE_I ? 0 : p.pict_type == AV_PICTURE_TYPE_P ? 1
                                       : p.pict_type == AV_PICTURE_TYPE_B ? 2 : 3);
#endif
        ps_write8(base, info + 0x06, p.meta.avc.idr);
        ps_write8(base, info + 0x07, s.aspect_ratio_idc);
        ps_write16(base, info + 0x08, s.sar_height);
        ps_write16(base, info + 0x0A, s.sar_width);
        ps_write8(base, info + 0x12, s.vui);
        ps_write8(base, info + 0x13, s.frame_mbs_only);
        ps_write8(base, info + 0x14, s.video_signal_type);
        ps_write8(base, info + 0x15, s.video_format);
        ps_write8(base, info + 0x16, s.full_range);
        ps_write8(base, info + 0x17, s.colour_description);
        ps_write8(base, info + 0x18, s.primaries);
        ps_write8(base, info + 0x19, s.transfer);
        ps_write8(base, info + 0x1A, s.matrix);
        ps_write8(base, info + 0x1B, s.timing);
        const u8 frc = v.frame_rate ? static_cast<u8>(v.frame_rate & 7) : avc_frame_rate_code(s);
        ps_write8(base, info + 0x1C, frc == 0xFF ? 0 : frc);
        ps_write8(base, info + 0x1D, s.fixed_frame_rate);
        ps_write8(base, info + 0x1E, s.low_delay_hrd);
        ps_write8(base, info + 0x1F, p.meta.avc.entropy_cabac);
        ps_write16(base, info + 0x20, p.meta.avc.nal_flags);
        // Past RPCS3's structure, as Sony's for the streams tried (meanings inferred): the picture order count type,
        // the VUI timing, the picture's number and its size.
        ps_write8(base, info + 0x138, static_cast<u8>(s.poc_type));
        ps_write32(base, info + 0x154, s.time_scale);
        ps_write32(base, info + 0x158, s.units_in_tick);
        ps_write8(base, info + 0x15C, s.fixed_frame_rate);
        ps_write32(base, info + 0x224, p.number);
        ps_write32(base, info + 0x228, p.width);
        ps_write32(base, info + 0x22C, p.height);
    } else if (v.codec == CODEC_MPEG2) {
        // CellVdecMpeg2Info as RPCS3 fills it (not compared with Sony's yet).
        ps_write16(base, info + 0x00, static_cast<u16>(p.width));
        ps_write16(base, info + 0x02, static_cast<u16>(p.height));
    } else {
        write_divx_info(base, info, v, p); // MPEG-4, DivX, DivX 3.11
    }
}

// The picture in the format asked (CellVdecPicFormat: type, colour matrix, alpha).
void convert(u8* out, const Picture& p, u32 format, u32 matrix, u8 alpha)
{
    const u32 w = p.width, h = p.height, cw = w / 2;
    if (format == PICFMT_YUV420_PLANAR) {
        std::memcpy(out, p.y.data(), p.y.size());
        std::memcpy(out + p.y.size(), p.u.data(), p.u.size());
        std::memcpy(out + p.y.size() + p.u.size(), p.v.data(), p.v.size());
        return;
    }
    if (format == PICFMT_UYVY422) {
        for (u32 r = 0; r < h; r++)
            for (u32 x = 0; x < w; x += 2) {
                u8* o = out + (r * w + x) * 2;
                o[0] = p.u[(r / 2) * cw + x / 2];
                o[1] = p.y[r * w + x];
                o[2] = p.v[(r / 2) * cw + x / 2];
                o[3] = p.y[r * w + x + 1];
            }
        return;
    }
    // Limited range BT.601 or BT.709 with the exact coefficients, truncated.
    const bool bt709 = matrix == 1;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    const double ky = 255.0 / 219.0, rv = 255.0 / 224.0 * 2 * (1 - kr), bu = 255.0 / 224.0 * 2 * (1 - kb);
    const double gu = 255.0 / 224.0 * 2 * (1 - kb) * kb / kg, gv = 255.0 / 224.0 * 2 * (1 - kr) * kr / kg;
    const auto clamp = [](double c) { return static_cast<u8>(std::clamp(std::floor(c), 0.0, 255.0)); };
    for (u32 r = 0; r < h; r++)
        for (u32 x = 0; x < w; x++) {
            const double Y = p.y[r * w + x] - 16.0, U = p.u[(r / 2) * cw + x / 2] - 128.0,
                         V = p.v[(r / 2) * cw + x / 2] - 128.0;
            const u8 R = clamp(ky * Y + rv * V), G = clamp(ky * Y - gu * U - gv * V), B = clamp(ky * Y + bu * U);
            u8* o = out + (r * w + x) * 4;
            if (format == PICFMT_RGBA32) {
                o[0] = R, o[1] = G, o[2] = B, o[3] = alpha;
            } else {
                o[0] = alpha, o[1] = R, o[2] = G, o[3] = B;
            }
        }
}

u32 picture_bytes(const Picture& p, u32 format)
{
    return format == PICFMT_YUV420_PLANAR ? p.width * p.height * 3 / 2
           : format == PICFMT_UYVY422     ? p.width * p.height * 2
                                          : p.width * p.height * 4;
}

} // namespace

// ---- the API

s32 cellVdecQueryAttrEx(PPUContext& ctx, u32 type, u32 attr)
{
    const std::string call = std::format("cellVdecQueryAttrEx({:#x}, {:#x})", type, attr);
    if (!type || !attr || !usable(ctx, type, 12, call) || !usable(ctx, attr, 16, call))
        return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    ps_write32(base, attr, 0);
    Attr a;
    if (const s32 r = query_attr(ctx, ps_read32(base, type), ps_read32(base, type + 4), ps_read32(base, type + 8), a);
        r != lv2::CELL_OK)
        return finish(error(CELL_VDEC_ERROR_ARG), call);
    // CellVdecAttr: memSize, cmdDepth (u8), decoderVerUpper, decoderVerLower.
    ps_write32(base, attr + 0, a.mem_size);
    ps_write8(base, attr + 4, a.cmd_depth);
    ps_write32(base, attr + 8, kVersionUpper);
    ps_write32(base, attr + 12, a.version);
    return finish(lv2::CELL_OK, call);
}

s32 cellVdecQueryAttr(PPUContext& ctx, u32 type, u32 attr)
{
    const std::string call = std::format("cellVdecQueryAttr({:#x}, {:#x})", type, attr);
    if (!type || !usable(ctx, type, 8, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    StackVar ex(ctx, base, 12);
    ps_write32(base, ex.addr(), ps_read32(base, type));
    ps_write32(base, ex.addr() + 4, ps_read32(base, type + 4));
    ps_write32(base, ex.addr() + 8, 0);
    return cellVdecQueryAttrEx(ctx, ex.addr(), attr);
}

s32 cellVdecOpen(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle)
{
    const std::string call = std::format("cellVdecOpen({:#x}, {:#x}, {:#x}, {:#x})", type, res, cb, handle);
    return finish(open(ctx, type, 0, res, cb, handle, call), call);
}

s32 cellVdecOpenEx(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle)
{
    const std::string call = std::format("cellVdecOpenEx({:#x}, {:#x}, {:#x}, {:#x})", type, res, cb, handle);
    const u32 spec = type && guest_readable(thread_of(ctx), type, 12) ? ps_read32(guest_base(ctx), type + 8) : 0;
    return finish(open(ctx, type, spec, res, cb, handle, call), call);
}

s32 cellVdecClose(PPUContext& ctx, u32 handle)
{
    const std::string call = std::format("cellVdecClose({:#x})", handle);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v) return finish(error(CELL_VDEC_ERROR_ARG), call);
    {
        std::lock_guard lk(v->m);
        if (v->state == Seq::closed) return finish(error(CELL_VDEC_ERROR_SEQ), call);
        v->state = Seq::closed;
        v->seq++; // what is still queued is dropped
        push_command(*v, Command(Command::close));
    }
    lv2::PpuThread& t = thread_of(ctx);
    StackVar status(ctx, guest_base(ctx), 8);
    lv2::sys_ppu_thread_join(t, static_cast<u32>(v->thread_id), status.addr());
    if (v->tls) HleProcess::current()->free_tls(v->tls);
    std::lock_guard lk(v->m);
    v->out.clear();
    v->held.clear();
#if PSPOCKET_HAVE_FFMPEG
    avcodec_free_context(&v->av);
#endif
    return finish(lv2::CELL_OK, call);
}

s32 cellVdecStartSeq(PPUContext& ctx, u32 handle)
{
    const std::string call = std::format("cellVdecStartSeq({:#x})", handle);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v) return finish(error(CELL_VDEC_ERROR_ARG), call);
    std::lock_guard lk(v->m);
    if (v->state != Seq::dormant && v->state != Seq::ready) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    v->seq++;
    v->pending_aus = 0;
    Command c(Command::start);
    c.seq = v->seq;
    push_command(*v, c);
    v->state = Seq::ready;
    return finish(lv2::CELL_OK, call);
}

s32 cellVdecEndSeq(PPUContext& ctx, u32 handle)
{
    const std::string call = std::format("cellVdecEndSeq({:#x})", handle);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v) return finish(error(CELL_VDEC_ERROR_ARG), call);
    std::lock_guard lk(v->m);
    if (v->state != Seq::ready) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    v->state = Seq::ending;
    Command drain(Command::drain), end(Command::end);
    drain.seq = end.seq = v->seq;
    push_command(*v, drain);
    push_command(*v, end);
    return finish(lv2::CELL_OK, call);
}

s32 cellVdecDecodeAu(PPUContext& ctx, u32 handle, s32 mode, u32 au_info)
{
    const std::string call = std::format("cellVdecDecodeAu({:#x}, {}, {:#x})", handle, mode, au_info);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    u8* base = guest_base(ctx);
    if (!v || !au_info || !usable(ctx, au_info, 0x28, call) || !ps_read32(base, au_info + 4) || !ps_read32(base, au_info))
        return finish(error(CELL_VDEC_ERROR_ARG), call);
    std::lock_guard lk(v->m);
    if (v->state != Seq::ready) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    if (mode < 0 || mode > (DEC_MODE_B_SKIP | DEC_MODE_PB_SKIP) ||
        (mode == (DEC_MODE_B_SKIP | DEC_MODE_PB_SKIP) && v->codec != CODEC_VC1) ||
        (mode == DEC_MODE_PB_SKIP && v->codec != CODEC_AVC))
        return finish(error(CELL_VDEC_ERROR_ARG), call);
    if (v->pending_aus >= 4) return finish(error(CELL_VDEC_ERROR_BUSY), call);
    // CellVdecAuInfo: startAddr, size, pts, dts, userData, codecSpecificData.
    Command c(Command::au);
    c.seq = v->seq;
    c.mode = mode;
    c.au_addr = ps_read32(base, au_info);
    c.au_size = ps_read32(base, au_info + 4);
    if (!usable(ctx, c.au_addr, c.au_size, "the access unit")) return finish(error(CELL_VDEC_ERROR_ARG), call);
    c.meta.index = v->next_au++;
    c.meta.seq = v->seq;
    c.meta.pts_upper = ps_read32(base, au_info + 8);
    c.meta.pts_lower = ps_read32(base, au_info + 12);
    c.meta.dts_upper = ps_read32(base, au_info + 16);
    c.meta.dts_lower = ps_read32(base, au_info + 20);
    c.meta.user_data = ps_read64(base, au_info + 24);
    c.meta.attr = mode == DEC_MODE_NORMAL ? 0 : 1;
    if (v->codec == CODEC_AVC || v->codec == CODEC_JVT) scan_avc_au({base + c.au_addr, c.au_size}, c.meta.avc, v->sps);
    c.meta.sps = v->sps;
    v->pending_aus++;
    push_command(*v, std::move(c));
    return lv2::CELL_OK;
}

s32 cellVdecGetPicItem(PPUContext& ctx, u32 handle, u32 item_out)
{
    const std::string call = std::format("cellVdecGetPicItem({:#x}, {:#x})", handle, item_out);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v || !item_out || !usable(ctx, item_out, 4, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    std::lock_guard lk(v->m);
    if (v->state == Seq::closed) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    for (Picture& p : v->out) {
        if (p.item_taken) continue;
        p.item_taken = true;
        const u32 item = v->handle + kItems + (p.number % kRing) * kItemStride;
        const u32 info = v->handle + kInfos + (p.number % kRing) * kInfoStride;
        write_item(base, *v, p, item, info);
        ps_write32(base, item_out, item);
        return lv2::CELL_OK;
    }
    return error(CELL_VDEC_ERROR_EMPTY);
}

s32 cellVdecGetPicture(PPUContext& ctx, u32 handle, u32 format, u32 out_buff)
{
    const std::string call = std::format("cellVdecGetPicture({:#x}, {:#x}, {:#x})", handle, format, out_buff);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v || !format || !usable(ctx, format, 9, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    // CellVdecPicFormat: formatType, colorMatrixType, alpha.
    const u32 type = ps_read32(base, format), matrix = ps_read32(base, format + 4);
    const u8 alpha = ps_read8(base, format + 8);
    if (type > PICFMT_YUV420_PLANAR || (type <= PICFMT_RGBA32 && matrix > 1)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    Picture p;
    {
        std::lock_guard lk(v->m);
        if (v->state == Seq::closed) return finish(error(CELL_VDEC_ERROR_SEQ), call);
        if (v->out.empty()) return error(CELL_VDEC_ERROR_EMPTY);
        p = std::move(v->out.front());
        v->out.pop_front();
    }
    if (out_buff) {
        if (!usable(ctx, out_buff, picture_bytes(p, type), "the picture's buffer")) return finish(error(CELL_VDEC_ERROR_ARG), call);
        convert(base + out_buff, p, type, matrix, alpha);
    }
    return lv2::CELL_OK;
}

s32 cellVdecGetPictureExt(PPUContext& ctx, u32 handle, u32 format2, u32 out_buff, u32 arg4)
{
    const std::string call = std::format("cellVdecGetPictureExt({:#x}, {:#x}, {:#x}, {:#x})", handle, format2, out_buff, arg4);
    if (!format2 || !usable(ctx, format2, 0x14, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    if (arg4 && arg4 != 8 && arg4 != 0xC) return finish(error(CELL_VDEC_ERROR_ARG), call);
    // CellVdecPicFormat2 (RPCS3): formatType, colorMatrixType, unk0, alpha (at 0xC), unk1.
    u8* base = guest_base(ctx);
    StackVar f(ctx, base, 12);
    ps_write32(base, f.addr(), ps_read32(base, format2));
    ps_write32(base, f.addr() + 4, ps_read32(base, format2 + 4));
    ps_write8(base, f.addr() + 8, ps_read8(base, format2 + 0xC));
    return cellVdecGetPicture(ctx, handle, f.addr(), out_buff);
}

s32 cellVdecSetFrameRate(PPUContext& ctx, u32 handle, s32 frame_rate)
{
    const std::string call = std::format("cellVdecSetFrameRate({:#x}, {:#x})", handle, frame_rate);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v || (frame_rate & 0xF8) != 0x80) return finish(error(CELL_VDEC_ERROR_ARG), call);
    std::lock_guard lk(v->m);
    if (v->state == Seq::closed) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    Command c(Command::framerate);
    c.frame_rate = static_cast<u32>(frame_rate & 0x87);
    push_command(*v, c);
    return finish(lv2::CELL_OK, call);
}

void register_libvdec(Registry& r)
{
    r.add("libvdec", {
        PS_HLE_FUNC(0xFF6F6EBE, cellVdecQueryAttr),
        PS_HLE_FUNC(0xC982A84A, cellVdecQueryAttrEx),
        PS_HLE_FUNC(0xB6BBCD5D, cellVdecOpen),
        PS_HLE_FUNC(0x0053E2D8, cellVdecOpenEx),
        PS_HLE_FUNC(0x16698E83, cellVdecClose),
        PS_HLE_FUNC(0xC757C2AA, cellVdecStartSeq),
        PS_HLE_FUNC(0x824433F0, cellVdecEndSeq),
        PS_HLE_FUNC(0x2BF4DDD2, cellVdecDecodeAu),
        PS_HLE_FUNC(0x807C861A, cellVdecGetPicture),
        PS_HLE_FUNC(0xA21AA896, cellVdecGetPictureExt),
        PS_HLE_FUNC(0x17C702B9, cellVdecGetPicItem),
        PS_HLE_FUNC(0xE13EF6FC, cellVdecSetFrameRate),
        PS_HLE_INTERNAL(NID_vdec_thread, _pspocket_vdec_thread),
    });
}

} // namespace pspocket::hle
