// cellVdec: the video decoder (libvdec), pspocket's own on FFmpeg (D-181, D-195). H.264 (and the JVT form of it),
// MPEG-2 and MPEG-4 part 2 (as in DivX) decode with FFmpeg as in RPCS3 (rpcs3/Emu/Cell/Modules/cellVdec.cpp: the
// sequence states, the commands, the checks of each call, the picture formats). DivX 3.11 (D-588), VC-1's advanced
// profile (D-589) and MVC's base view (D-590), which RPCS3 does not decode, go the same way. What a game sees follows
// Sony's libvdec 4.92 and its codec cores, compared call for call (tests/hle/test_vdec.cpp for H.264, D-195;
// tests/hle/test_vdec_codecs_compare.cpp for DivX 3.11, DivX, MPEG-4, VC-1 and MVC, D-912), where it differs from
// RPCS3's:
//
// - Sizes: cellVdecQueryAttr gives Sony's: each codec core's memory query (H.264's avc_memory below, libavcdec's;
//   the others in vdec_layout.hpp, with the codec specific information), plus libvdec's own parts and ring. A decoder
//   opens with at least the codec's part of it (vdec_layout.hpp). RPCS3's codec memory queries call Sony's cores.
// - The handle is the address of the decoder's memory, as Sony's (RPCS3: an ID).
// - The ring (vdec_layout.hpp): each access unit takes the lowest free element of libvdec's ring at DecodeAu; its
//   picture's item (CellVdecPicItem) is in the element and its information block in the ring of blocks after them,
//   written when the picture is announced, only the bytes Sony's libvdec writes. GetPicture frees the element.
// - Pictures are announced one access unit late (H.264, DivX, DivX 3.11, VC-1) or at once (MPEG-4): after an access
//   unit's AUDONE come the PICOUTs; the last ones when the sequence ends, before SEQDONE.
// - Frame buffers: DivX's, DivX 3.11's, MPEG-4's and VC-1's are the cores' own, at their places, used in turn, laid
//   out in strips with edges; a core holds at most so many pictures (decoded, not yet released by GetPicture) before
//   it holds the next access unit back (no AUDONE until the game takes one). H.264's frame buffer is laid out as
//   Sony's (16-byte strips of luma, then of chroma, two strips of repeated edge each side; RPCS3: 0x123); its place is
//   pspocket's own.
// - Pictures to RGBA and ARGB use the colour matrix asked for (BT.601 or BT.709, limited range; RPCS3: a TODO), with
//   the exact coefficients, truncated: within one step of Sony's values. UYVY repeats each chroma row, as Sony's.
//
// cellVdecOpenExt, cellVdecSetPts and cellVdecDecodeAuEx2 are RPCS3's (D-596) on this decoder, without Sony's module to
// compare them with: they are unverified (D-594), so a game importing one still gets Sony's whole libvdec where the
// system software is installed.
//
// Not yet (each fails loudly): VC-1's simple and main profiles (they open, as Sony's, but their access units are not
// decoded: how libsvc1d is given their sequence header is not known, logged), MVC's views past the base one (FFmpeg
// has no MVC decoder; and Sony's libvdec 4.92 decodes MVC neither through DecodeAu, refused, nor soundly through
// DecodeAuEx2, D-912), the codec specific information of MPEG-2 and JVT in the sizes (Sony's answer without it, logged).

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
#include <set>
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
#include "vdec_layout.hpp"

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
constexpr s32 MSG_AUDONE = 0, MSG_PICOUT = 1, MSG_SEQDONE = 2, MSG_ERROR = 3;
constexpr s32 DEC_MODE_NORMAL = 0, DEC_MODE_B_SKIP = 1, DEC_MODE_PB_SKIP = 2;
constexpr u32 PICFMT_RGBA32 = 1, PICFMT_UYVY422 = 2, PICFMT_YUV420_PLANAR = 3;
constexpr u32 AVC_LEVEL_UNK = 1042; // 4.2 with the deblocking filter off
constexpr u32 kVersionUpper = 0x4920000;
constexpr u32 VC1_AP_L0 = 5; // CELL_VDEC_VC1_AP_L0: the levels below are the simple and main profiles' (D-589)

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

// Sony's answers (libvdec 4.92, test_vdec.cpp's probe) for MPEG-2 and JVT, whose memory queries are not reproduced:
// the memory for each level without codec specific information.
struct LevelSize {
    u32 codec, level, mem_size;
};
constexpr LevelSize kLevelSizes[] = {
    {CODEC_MPEG2, 0, 0x11290B}, {CODEC_MPEG2, 1, 0x2DFB8B}, {CODEC_MPEG2, 2, 0xA0270B}, {CODEC_MPEG2, 3, 0xD2F40B},
    {CODEC_JVT, 10, 0x336DDB}, {CODEC_JVT, 11, 0x73675B}, {CODEC_JVT, 12, 0x73675B}, {CODEC_JVT, 13, 0x73675B},
    {CODEC_JVT, 20, 0x73675B}, {CODEC_JVT, 21, 0xC6CB5B}, {CODEC_JVT, 22, 0x175A75B}, {CODEC_JVT, 30, 0x175A75B},
    {CODEC_JVT, 31, 0x31FBCDB}, {CODEC_JVT, 32, 0x444FDDB}, {CODEC_JVT, 40, 0x6B4D7DB}, {CODEC_JVT, 41, 0x6B4D7DB},
    {CODEC_JVT, 42, 0x6B4D7DB},
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
    u32 mem_size = 0;   // what QueryAttrEx answers
    u32 codec_part = 0; // the codec's part of it: a decoder opens with at least this (Sony's libvdec checks it)
    u8 cmd_depth = 4;
    u32 version = 0;
    vdec::Sizing sizing; // what the codec's frame buffers are sized for (D-912)
};

// cellVdecQueryAttrEx's answer for a codec, level and codec specific information (0: none), or an error. Sony's
// libvdec asks the codec's core for its memory (with the codec specific information where given), adds its own parts
// (kSpursPart and each codec's), then QueryAttrEx adds kQuery and the ring (D-912). Every refusal is ARG, as libvdec
// answers it whatever the core said.
s32 query_attr(PPUContext& ctx, u32 codec, u32 level, u32 spec, Attr& out)
{
    u8* base = guest_base(ctx);
    const bool valid_codec = codec <= CODEC_MPEG4 || (codec < CODEC_MAX && (codec & 1));
    if (!valid_codec) return error(CELL_VDEC_ERROR_ARG);
    out.version = version_lower(codec);
    const bool new_sdk = thread_of(ctx).kernel.config().sdk_version >= 0x210000;
    const vdec::Ring ring = vdec::ring_of(codec);
    const bool readable = spec && guest_readable(thread_of(ctx), spec, 12);
    if (codec == CODEC_AVC) {
        u32 lv = level, deblock_off = 0, buffers = 0, w = 0, h = 0;
        if (level == AVC_LEVEL_UNK) {
            lv = 42;
            deblock_off = 1;
        }
        if (spec) {
            if (!readable || ps_read32(base, spec) != 12) return error(CELL_VDEC_ERROR_ARG);
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
        out.codec_part = 0x41580 + *mem + 0x89FD;
        out.mem_size = new_sdk ? out.codec_part + vdec::kQuery + vdec::ring_bytes(ring) : avc_old_sdk_size(lv);
        return lv2::CELL_OK;
    }
    if (codec == CODEC_MPEG2 || codec == CODEC_JVT) {
        // Sony's answers for each level (the codec specific information is not reproduced for these, logged).
        const auto it = std::find_if(std::begin(kLevelSizes), std::end(kLevelSizes),
            [&](const LevelSize& s) { return s.codec == codec && s.level == level; });
        if (it == std::end(kLevelSizes)) return error(CELL_VDEC_ERROR_ARG);
        if (spec)
            PS_LOG_WARN(kChan, "cellVdecQueryAttr: codec {} specific information ({:#x}) is not reproduced; Sony's size "
                               "without it is given", codec, spec);
        out.mem_size = it->mem_size;
        out.codec_part = it->mem_size - vdec::kQuery - vdec::ring_bytes(ring);
        return lv2::CELL_OK;
    }
    vdec::Sizing s;
    switch (codec) {
    case CODEC_MPEG4: s = vdec::mpeg4_sizing(base, level, spec, readable); break;
    case CODEC_VC1: s = vdec::vc1_sizing(base, level, spec, readable); break;
    case CODEC_DIVX: s = vdec::divx_sizing(base, level, spec, readable); break;
    case CODEC_DIVX3_11: s = vdec::divx311_sizing(base, level, spec, readable); break;
    default: s = vdec::mvc_sizing(base, level, spec, readable); break; // MVC, MVC2
    }
    if (s.fail != vdec::Fail::none) return error(CELL_VDEC_ERROR_ARG);
    out.sizing = s;
    out.codec_part = s.codec_part;
    out.mem_size = s.codec_part + vdec::kQuery + vdec::ring_bytes(ring);
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

// Where the next start code (00 00 01) begins at or after `from`; the size when none does.
usize next_start_code(std::span<const u8> d, usize from)
{
    for (usize k = from; k + 3 <= d.size(); k++)
        if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) return k;
    return d.size();
}

// ---- MVC (H.264 Annex H, D-590): FFmpeg has no MVC decoder, so its H.264 decoder gets the base view alone

// An MVC access unit without the NAL units of the views past the base one: prefix NAL units (14), subset sequence
// parameter sets (15) and coded slice extensions (20). Each NAL unit runs from its start code (with the zero byte of a
// four-byte one) to the next one's, and the rest stays as it was; whether any was left out.
bool mvc_base_view(std::span<const u8> au, std::vector<u8>& out)
{
    std::vector<usize> codes; // where each start code begins
    for (usize i = next_start_code(au, 0); i < au.size(); i = next_start_code(au, i + 3)) codes.push_back(i);
    const auto begin_of = [&](usize k) {
        const usize floor = k ? codes[k - 1] + 4 : 0; // not into the start code and header before
        return codes[k] > floor && au[codes[k] - 1] == 0 ? codes[k] - 1 : codes[k];
    };
    const auto keep = [&](usize begin, usize end) {
        const std::span<const u8> part = au.subspan(begin, end - begin);
        out.insert(out.end(), part.begin(), part.end());
    };
    out.clear();
    keep(0, codes.empty() ? au.size() : begin_of(0));
    bool left_out = false;
    for (usize k = 0; k < codes.size(); k++) {
        const u8 type = codes[k] + 3 < au.size() ? au[codes[k] + 3] & 0x1F : 0;
        if (type == 14 || type == 15 || type == 20) left_out = true;
        else keep(begin_of(k), k + 1 < codes.size() ? begin_of(k + 1) : au.size());
    }
    return left_out;
}

// ---- VC-1 (SMPTE 421M), the advanced profile in its start code framing (D-589): FFmpeg's configuration and what
// CellVdecVc1Info reports, from the access unit's bitstream data units (BDUs)

// The sequence header's fields: sizes as values (a coded size's (n + 1) * 2, a display or aspect size's n + 1), the
// frame rate as coded.
struct Vc1Sequence {
    u8 level = 0;
    u16 max_coded_width = 0, max_coded_height = 0;
    bool pulldown = false, interlace = false, tfcntrflag = false, psf = false, display_ext = false;
    bool postprocflag = false, finterpflag = false, aspect_ratio_flag = false;
    u16 display_width = 0, display_height = 0, aspect_width = 0, aspect_height = 0, framerate_exp = 0;
    u8 aspect_ratio = 0, framerate_nr = 0, framerate_dr = 0;
    bool framerate_flag = false, framerate_ind = false, color_format_flag = false;
    u8 color_prim = 0, transfer_char = 0, matrix_coef = 0;
    u8 hrd_buckets = 0; // HRD_NUM_LEAKY_BUCKETS when HRD_PARAM_FLAG (the entry point has a byte for each)
};

// The entry point's fields CellVdecVc1Info reports, and its range mapping.
struct Vc1Entry {
    bool broken_link = false, closed_entry = false, range_mapy_flag = false, range_mapuv_flag = false;
    u8 range_mapy = 0, range_mapuv = 0;
};

// CellVdecVc1Info's picture types (pspocket's numbering: D-589): the standard's I, P, B, BI and skipped pictures.
constexpr u8 VC1_PCT_I = 0, VC1_PCT_P = 1, VC1_PCT_B = 2, VC1_PCT_BI = 3, VC1_PCT_SKIPPED = 4;

// The picture header's first fields: the frame coding mode (0 progressive, 1 an interlaced frame, 2 a field pair),
// the picture's type (a field pair's two), its field order and repetition.
struct Vc1Picture {
    u8 fcm = 0, rptfrm = 0;
    u8 type[2] = {VC1_PCT_I, VC1_PCT_I};
    bool tff = true, rff = false;
};

// What an access unit is decoded with: the sequence header and entry point in force, with their BDUs as the stream has
// them (FFmpeg's configuration), and its picture's header.
struct Vc1Headers {
    Vc1Sequence seq;
    Vc1Entry entry;
    Vc1Picture pic;
    std::vector<u8> seq_bdu, entry_bdu;
    u32 units = 0; // the access unit's own: a sequence header 1, an entry point 2, a frame 4
};

// An advanced profile sequence header's payload; false for another profile's (the simple and main profiles have no
// sequence header in their streams) or a cut one.
bool parse_vc1_sequence(std::span<const u8> payload, Vc1Sequence& s)
{
    Bits b(rbsp(payload)); // the escaping is H.264's
    s = {};
    if (b.bits(2) != 3) return false; // PROFILE: advanced
    s.level = static_cast<u8>(b.bits(3));
    b.bits(2);         // COLORDIFF_FORMAT
    b.bits(3 + 5); // FRMRTQ_POSTPROC, BITRTQ_POSTPROC
    s.postprocflag = b.bit();
    s.max_coded_width = static_cast<u16>((b.bits(12) + 1) * 2);
    s.max_coded_height = static_cast<u16>((b.bits(12) + 1) * 2);
    s.pulldown = b.bit();
    s.interlace = b.bit();
    s.tfcntrflag = b.bit();
    s.finterpflag = b.bit();
    b.bit(); // reserved
    s.psf = b.bit();
    s.display_ext = b.bit();
    if (s.display_ext) {
        s.display_width = static_cast<u16>(b.bits(14) + 1);
        s.display_height = static_cast<u16>(b.bits(14) + 1);
        s.aspect_ratio_flag = b.bit();
        if (s.aspect_ratio_flag) {
            s.aspect_ratio = static_cast<u8>(b.bits(4));
            if (s.aspect_ratio == 15) {
                s.aspect_width = static_cast<u16>(b.bits(8) + 1);
                s.aspect_height = static_cast<u16>(b.bits(8) + 1);
            }
        }
        s.framerate_flag = b.bit();
        if (s.framerate_flag) {
            s.framerate_ind = b.bit();
            if (s.framerate_ind) {
                s.framerate_exp = static_cast<u16>(b.bits(16));
            } else {
                s.framerate_nr = static_cast<u8>(b.bits(8));
                s.framerate_dr = static_cast<u8>(b.bits(4));
            }
        }
        s.color_format_flag = b.bit();
        if (s.color_format_flag) {
            s.color_prim = static_cast<u8>(b.bits(8));
            s.transfer_char = static_cast<u8>(b.bits(8));
            s.matrix_coef = static_cast<u8>(b.bits(8));
        }
    }
    if (b.bit()) s.hrd_buckets = static_cast<u8>(b.bits(5)); // HRD_PARAM_FLAG
    return !b.over();
}

bool parse_vc1_entry(std::span<const u8> payload, const Vc1Sequence& s, Vc1Entry& e)
{
    Bits b(rbsp(payload));
    e = {};
    e.broken_link = b.bit();
    e.closed_entry = b.bit();
    b.bits(4);                        // PANSCAN_FLAG, REFDIST_FLAG, LOOPFILTER, FASTUVMC
    const bool extended_mv = b.bit(); // EXTENDED_MV
    b.bits(2 + 1 + 1 + 2);            // DQUANT, VSTRANSFORM, OVERLAP, QUANTIZER
    for (u32 i = 0; i < s.hrd_buckets; i++) b.bits(8); // HRD_FULL
    if (b.bit()) b.bits(12 + 12);     // CODED_SIZE_FLAG: CODED_WIDTH, CODED_HEIGHT
    if (extended_mv) b.bit();         // EXTENDED_DMV
    e.range_mapy_flag = b.bit();
    if (e.range_mapy_flag) e.range_mapy = static_cast<u8>(b.bits(3));
    e.range_mapuv_flag = b.bit();
    if (e.range_mapuv_flag) e.range_mapuv = static_cast<u8>(b.bits(3));
    return !b.over();
}

void parse_vc1_picture(std::span<const u8> payload, const Vc1Sequence& s, Vc1Picture& p)
{
    Bits b(rbsp(payload));
    p = {};
    if (s.interlace && b.bit()) p.fcm = static_cast<u8>(1 + b.bit()); // FCM: 0, 10, 11
    if (p.fcm == 2) {
        // FPTYPE: the two fields' types.
        constexpr u8 kPairs[8][2] = {{VC1_PCT_I, VC1_PCT_I}, {VC1_PCT_I, VC1_PCT_P}, {VC1_PCT_P, VC1_PCT_I},
            {VC1_PCT_P, VC1_PCT_P}, {VC1_PCT_B, VC1_PCT_B}, {VC1_PCT_B, VC1_PCT_BI}, {VC1_PCT_BI, VC1_PCT_B},
            {VC1_PCT_BI, VC1_PCT_BI}};
        const u32 fptype = b.bits(3);
        p.type[0] = kPairs[fptype][0];
        p.type[1] = kPairs[fptype][1];
    } else {
        // PTYPE: 0 P, 10 B, 110 I, 1110 BI, 1111 skipped.
        constexpr u8 kTypes[5] = {VC1_PCT_P, VC1_PCT_B, VC1_PCT_I, VC1_PCT_BI, VC1_PCT_SKIPPED};
        u32 ones = 0;
        while (ones < 4 && b.bit()) ones++;
        p.type[0] = p.type[1] = kTypes[ones];
    }
    if (s.tfcntrflag) b.bits(8); // TFCNTR
    if (s.pulldown) {
        if (!s.interlace || s.psf) {
            p.rptfrm = static_cast<u8>(b.bits(2));
        } else {
            p.tff = b.bit();
            p.rff = b.bit();
        }
    }
}

// An access unit's BDUs in order: a sequence header or entry point becomes the decoder's (with its bytes, less the
// zeros before the next start code), a frame's header the access unit's picture. A sequence header of another
// profile than the advanced one is not taken (`other_profile`).
void scan_vc1_au(std::span<const u8> au, Vc1Headers& state, Vc1Picture& pic, bool& other_profile, u32& units)
{
    units = 0;
    usize i = next_start_code(au, 0);
    while (i + 4 <= au.size()) {
        const usize next = next_start_code(au, i + 4); // after the start code's suffix
        usize end = next;
        while (end > i + 4 && au[end - 1] == 0) end--;
        const std::span<const u8> bdu = au.subspan(i, end - i), payload = bdu.subspan(4);
        switch (au[i + 3]) {
        case 0x0F: { // sequence header
            units |= 1;
            Vc1Sequence s;
            if (parse_vc1_sequence(payload, s)) {
                state.seq = s;
                state.seq_bdu.assign(bdu.begin(), bdu.end());
                state.entry_bdu.clear(); // an entry point belongs to its sequence header
            } else {
                other_profile = true;
            }
            break;
        }
        case 0x0E: // entry point
            units |= 2;
            if (Vc1Entry e; !state.seq_bdu.empty() && parse_vc1_entry(payload, state.seq, e)) {
                state.entry = e;
                state.entry_bdu.assign(bdu.begin(), bdu.end());
            }
            break;
        case 0x0D: // frame
            units |= 4;
            if (!state.seq_bdu.empty()) parse_vc1_picture(payload, state.seq, pic);
            break;
        default: break; // fields, slices, user data, the end of a sequence
        }
        i = next;
    }
}

// ---- MPEG-4 part 2 (ISO/IEC 14496-2): what CellVdecMpeg4Info and CellVdecDivxInfo report of the stream's headers

// The visual object's video signal type and the video object layer's fields Sony's information blocks report (D-912).
struct Mpeg4Vol {
    bool valid = false;
    u8 aspect_ratio_info = 1, par_width = 0, par_height = 0;
    u16 time_increment_resolution = 0, fixed_vop_time_increment = 0, width = 0, height = 0;
    bool fixed_vop_rate = false, interlaced = false;
    bool colour_description = false;
    u8 colour_primaries = 0, transfer_characteristics = 0, matrix_coefficients = 0;
};

// The visual object header (start code 0xB5): its video signal type, when it is a video object.
void parse_mpeg4_visual_object(std::span<const u8> payload, Mpeg4Vol& v)
{
    Bits b(std::vector<u8>(payload.begin(), payload.end()));
    if (b.bit()) b.bits(4 + 3); // is_visual_object_identifier: verid, priority
    if (b.bits(4) != 1) return; // visual_object_type: video ID
    v.colour_description = false;
    if (b.bit()) {                     // video_signal_type
        b.bits(3 + 1);                 // video_format, video_range
        v.colour_description = b.bit(); // colour_description
        if (v.colour_description) {
            v.colour_primaries = static_cast<u8>(b.bits(8));
            v.transfer_characteristics = static_cast<u8>(b.bits(8));
            v.matrix_coefficients = static_cast<u8>(b.bits(8));
        }
    }
}

// A video object layer header (start codes 0x20 to 0x2F) of a rectangular layer.
void parse_mpeg4_vol(std::span<const u8> payload, Mpeg4Vol& v)
{
    Bits b(std::vector<u8>(payload.begin(), payload.end()));
    b.bit();   // random_accessible_vol
    b.bits(8); // video_object_type_indication
    u32 verid = 1;
    if (b.bit()) { // is_object_layer_identifier
        verid = b.bits(4);
        b.bits(3);
    }
    v.aspect_ratio_info = static_cast<u8>(b.bits(4));
    v.par_width = v.par_height = 0;
    if (v.aspect_ratio_info == 15) { // extended PAR
        v.par_width = static_cast<u8>(b.bits(8));
        v.par_height = static_cast<u8>(b.bits(8));
    }
    if (b.bit()) {                                                        // vol_control_parameters
        b.bits(2 + 1);                                                    // chroma_format, low_delay
        if (b.bit()) b.bits(15 + 1 + 15 + 1 + 15 + 1 + 3 + 11 + 1 + 15 + 1); // vbv_parameters
    }
    const u32 shape = b.bits(2);
    if (shape == 3 && verid != 1) b.bits(4);
    b.bit(); // marker
    v.time_increment_resolution = static_cast<u16>(b.bits(16));
    b.bit(); // marker
    v.fixed_vop_rate = b.bit();
    v.fixed_vop_time_increment = 0;
    if (v.fixed_vop_rate) {
        u32 bits = 1;
        while (bits < 16 && (1u << bits) < v.time_increment_resolution) bits++;
        v.fixed_vop_time_increment = static_cast<u16>(b.bits(bits));
    }
    if (shape != 0) return; // not rectangular: no size
    b.bit();
    v.width = static_cast<u16>(b.bits(13));
    b.bit();
    v.height = static_cast<u16>(b.bits(13));
    b.bit();
    v.interlaced = b.bit();
    v.valid = !b.over();
}

// An access unit's headers: the visual object and video object layer in force afterwards.
void scan_mpeg4_au(std::span<const u8> au, Mpeg4Vol& v)
{
    for (usize i = next_start_code(au, 0); i + 4 <= au.size(); i = next_start_code(au, i + 4)) {
        const u8 code = au[i + 3];
        const usize end = next_start_code(au, i + 4);
        const std::span<const u8> payload = au.subspan(i + 4, end - (i + 4));
        if (code == 0xB5) parse_mpeg4_visual_object(payload, v);
        else if (code >= 0x20 && code <= 0x2F) parse_mpeg4_vol(payload, v);
    }
}

// ---- the decoder

constexpr u32 kOutMax = 60;    // decoded pictures the game has not taken (RPCS3 out_max)
constexpr u32 kOwnFrames = 16; // H.264's and MPEG-2's frame buffers: pspocket's own count and place (D-196)

enum class Seq : u32 { closed, dormant, ready, ending, resetting };

struct AuMeta {
    u64 index = 0, seq = 0;
    u32 slot = 0; // the ring element it holds (Sony's: the lowest free one at DecodeAu, D-912)
    u32 pts_upper = 0, pts_lower = 0, dts_upper = 0, dts_lower = 0;
    u64 user_data = 0;
    s32 attr = 0;
    AvcAuInfo avc;
    AvcSps sps;
    Vc1Headers vc1;
    Mpeg4Vol vol;
};

struct Picture {
    u64 seq = 0;
    u64 au = 0;     // the access unit it came from
    u32 number = 0; // pictures before it in the decoder's life
    u32 width = 0, height = 0;
    int pict_type = 0;
    std::vector<u8> y, u, v; // planar 4:2:0, no padding
    AuMeta meta;
    bool item_taken = false;
    u32 frame = 0, start = 0; // its frame buffer and the item's start address in it
    bool damaged = false;     // the decoder concealed errors in it (the PICOUT's error, CELL_VDEC_ERROR_PIC)
    // The size libvdec gives the game (the item's size, GetPicture's picture): the picture's own, but VC-1's is the
    // sequence header's as the decoder has read it when the picture is announced (the picture cut to it, D-912).
    u32 out_width = 0, out_height = 0;
    // What FFmpeg reports of the stream with the picture: the pixel aspect ratio (0/0: none), the colour description
    // (ISO/IEC 23001-8 codes; 2: unspecified), the field order and the frame rate.
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
    // What the decoder is given when it is not the game's access unit as it is (MVC's base view, D-590): au_size
    // bytes, then FFmpeg's zero padding. Empty with own_data: nothing to decode.
    std::vector<u8> data;
    bool own_data = false;
};

// The zero bytes FFmpeg reads past a packet's end (AV_INPUT_BUFFER_PADDING_SIZE).
constexpr usize kPacketPadding = 64;
#if PSPOCKET_HAVE_FFMPEG
static_assert(kPacketPadding >= AV_INPUT_BUFFER_PADDING_SIZE);
#endif

struct Vdec {
    u32 handle = 0, mem_size = 0, codec = 0, level = 0, cb_func = 0, cb_arg = 0;
    std::mutex m;
    Seq state = Seq::closed;
    u64 seq = 0, next_au = 0;
    u32 pending_aus = 0; // DecodeAu accepted, AUDONE not yet sent (Sony's command depth)
    std::deque<Command> commands;
    lv2::WaitQueue waiters;        // the decoder's thread, waiting for a command or a picture released
    std::deque<Picture> held, out; // decoded but not announced; announced, not yet released by GetPicture
    u32 pictures = 0;
    vdec::Ring ring;
    vdec::Plan plan;
    vdec::Sizing sizing;
    std::vector<u8> slots; // the ring's elements in use (under m)
    u32 next_frame = 0;    // the frame buffer the next picture takes (Plan::frames; the decoder's thread)
    u32 last_frame = 0, last_start = 0; // the last picture's (a VC-1 skipped picture shows it again)
    bool have_last = false;
    u32 own_frame_size = 0, own_frames = 0, own_width = 0, own_height = 0; // pspocket's own frames (Plan::frames 0)
    u32 frame_rate = 0; // cellVdecSetFrameRate's code (H.264's 0 to 7), set on the decoder's thread, under m
    u32 seq_frame_rate = 0; // DivX's and DivX 3.11's: SetFrameRate's code during the sequence (StartSeq clears it)
    bool dead = false;      // a fatal error reported (ERROR): the decoder does nothing more but Close (D-912)
    u64 thread_id = 0;
    u32 tls = 0;
    AvcSps sps;
    Mpeg4Vol vol;                  // MPEG-4's headers in force (DecodeAu's, under m)
    u32 divx_width = 720, divx_height = 576; // DivX 3.11's frame size (the stream has none)
    Vc1Headers vc1;                // VC-1's sequence header and entry point in force (DecodeAu's, under m)
    Vc1Headers vc1_current;        // the headers as the decoder has read them (the decoder's thread)
    std::set<u64> produced;        // the access units that have given a picture (the decoder's thread)
    std::set<u64> awaiting;        // the access units FFmpeg has and has not given a picture of yet (under m)
    std::vector<u8> vc1_opened;    // the sequence header FFmpeg's decoder was opened with (the decoder's thread)
    bool vc1_skip_logged = false;  // the decoder's thread
    bool vc1_other_logged = false; // under m
    bool mvc_logged = false;       // under m
    bool set_pts_logged = false;   // under m (D-596)
    bool au_ex2_logged = false;    // under m (D-596)
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

void callback(PPUContext& ctx, const Vdec& v, s32 type, u32 data = 0)
{
    call_guest(ctx, v.cb_func, {v.handle, static_cast<u64>(static_cast<u32>(type)), data, v.cb_arg});
}

// Wakes the decoder's thread (a command queued, a picture released: an access unit held back may go on). Under v.m.
void wake_decoder(Vdec& v)
{
    for (lv2::Waiter* w : v.waiters.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
}

// H.264's frame buffer layout (D-196): 16-byte strips, two of repeated edge on each side, luma, a gap of 0x400 bytes,
// chroma (8 Cb then 8 Cr a line), a gap. Sony's libavcdec leaves the gaps as they were (D-912).
u32 avc_frame_size(u32 width, u32 height)
{
    const u32 strips = (width + 15) / 16 + 4;
    return strips * 16 * height + 0x400 + strips * 16 * (height / 2) + 0x400;
}

void write_avc_frame(u8* base, u32 at, const Picture& p)
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
    const u32 chroma = at + strips * 16 * p.height + 0x400;
    for (u32 s = 0; s < strips; s++) {
        for (u32 row = 0; row < ch; row++) {
            for (u32 k = 0; k < 8; k++) {
                const s32 x = std::clamp<s32>(static_cast<s32>(s * 8 + k) - 16, 0, static_cast<s32>(cw) - 1);
                base[chroma + s * 16 * ch + row * 16 + k] = p.u[row * cw + static_cast<u32>(x)];
                base[chroma + s * 16 * ch + row * 16 + 8 + k] = p.v[row * cw + static_cast<u32>(x)];
            }
        }
    }
}

// A picture decoded: the frame buffer it takes and its contents there (the decoder's thread). A core with its own
// frame buffers (Plan::frames) takes them in turn; a VC-1 skipped picture shows the frame before it again. H.264's and
// MPEG-2's are pspocket's own (D-196): as many of the picture's size as fit after the ring, at most 16.
void place_picture(u8* base, Vdec& v, Picture& p)
{
    const vdec::Plan& plan = v.plan;
    if (plan.frames) {
        if (v.codec == CODEC_VC1 && v.have_last && p.meta.vc1.pic.type[0] == VC1_PCT_SKIPPED) {
            p.frame = v.last_frame;
            p.start = v.last_start;
            return;
        }
        p.frame = plan.frame0 + v.next_frame * plan.frame_bytes;
        v.next_frame = (v.next_frame + 1) % plan.frames;
        p.start = plan.start_inside ? p.frame + vdec::strip_start(plan, p.height) : p.frame;
        vdec::write_strips(base, p.frame, plan, p.width, p.height, p.y.data(), p.u.data(), p.v.data());
    } else {
        const u32 first = vdec::align(vdec::codec_memory(v.handle, v.ring), 128);
        if (p.width != v.own_width || p.height != v.own_height) {
            v.own_width = p.width;
            v.own_height = p.height;
            v.own_frame_size = vdec::align(avc_frame_size(p.width, p.height), 128);
            v.own_frames = v.mem_size > first - v.handle
                               ? std::min<u32>(kOwnFrames, (v.mem_size - (first - v.handle)) / v.own_frame_size)
                               : 0;
        }
        if (!v.own_frames) return;
        p.frame = p.start = first + (p.number % v.own_frames) * v.own_frame_size;
        write_avc_frame(base, p.frame, p);
    }
    v.last_frame = p.frame;
    v.last_start = p.start;
    v.have_last = true;
}

#if PSPOCKET_HAVE_FFMPEG
const AVCodec* decoder_for(u32 codec)
{
    switch (codec) {
    case CODEC_AVC:
    case CODEC_JVT:
    case CODEC_MVC:
    case CODEC_MVC2: return avcodec_find_decoder(AV_CODEC_ID_H264); // MVC: its base view (D-590)
    case CODEC_MPEG2: return avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    case CODEC_MPEG4:
    case CODEC_DIVX: return avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    case CODEC_DIVX3_11: return avcodec_find_decoder(AV_CODEC_ID_MSMPEG4V3); // Microsoft's MPEG-4 version 3 (D-588)
    case CODEC_VC1: return avcodec_find_decoder(AV_CODEC_ID_VC1);            // the advanced profile (D-589)
    default: return nullptr;
    }
}

// Opens FFmpeg's decoder for the decoder's codec, with the frame size DivX 3.11's frames do not carry and the
// configuration VC-1's decoder needs (its sequence header and entry point). False when FFmpeg has no decoder for it or
// refuses it (logged).
bool open_ffmpeg(Vdec& v, std::string_view call, std::span<const u8> extradata = {})
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
    if (!extradata.empty()) { // the context owns it (avcodec_free_context frees it)
        v.av->extradata = static_cast<u8*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(v.av->extradata, extradata.data(), extradata.size());
        v.av->extradata_size = static_cast<int>(extradata.size());
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

// The pictures the decoder has ready, with the access units they came from: each placed in its frame buffer (the
// decoder's thread) and held until it is announced.
void receive_pictures(u8* base, Vdec& v, std::map<u64, AuMeta>& in_flight, u64 seq, std::optional<u64> current = {})
{
    AVFrame* f = av_frame_alloc();
    while (avcodec_receive_frame(v.av, f) == 0) {
        Picture p;
        p.seq = seq;
        p.au = static_cast<u64>(reinterpret_cast<uintptr_t>(f->opaque));
        // FFmpeg's VC-1 decoder shows a skipped picture as a new reference to the picture before it, which keeps that
        // one's access unit: a picture of an access unit that has given one already is the next one's still owed (the
        // pictures come in decoding order), else the current access unit's.
        if (current && v.produced.contains(p.au)) {
            p.au = *current;
            for (const u64 au : v.awaiting)
                if (!v.produced.contains(au)) {
                    p.au = au;
                    break;
                }
        }
        v.produced.insert(p.au);
        {
            // The earlier access units still owed give no picture now (the pictures come in decoding order).
            std::lock_guard lk(v.m);
            v.awaiting.erase(v.awaiting.begin(), v.awaiting.upper_bound(p.au));
        }
        if (v.produced.size() > 128) v.produced.erase(v.produced.begin());
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
        p.damaged = f->decode_error_flags != 0 || (f->flags & AV_FRAME_FLAG_CORRUPT) != 0;
        if (v.av->framerate.num > 0 && v.av->framerate.den > 0) p.fps = av_q2d(v.av->framerate);
        // 4:2:0 from FFmpeg's software decoder, or from the media engine's pixel buffer (D-254).
        if (!vdec_hw::planes(f, p.width, p.height, p.y, p.u, p.v)) {
            PS_LOG_ERROR(kChan, "decoder {:#x}: a picture in pixel format {} (not 8-bit 4:2:0) is not supported", v.handle,
                f->format);
            av_frame_unref(f);
            continue;
        }
        if (p.damaged)
            PS_LOG_WARN(kChan, "decoder {:#x}: picture of access unit {} decoded with errors concealed (reported as "
                               "CELL_VDEC_ERROR_PIC)", v.handle, p.au);
        p.number = v.pictures++;
        place_picture(base, v, p);
        {
            std::lock_guard lk(v.m);
            v.held.push_back(std::move(p));
        }
        av_frame_unref(f);
    }
    av_frame_free(&f);
}

// VC-1 (D-589): FFmpeg's decoder takes the sequence header and entry point as its configuration, so it opens at the
// first access unit they are known for. A different sequence header later (which FFmpeg's decoder would not read in
// the stream) opens it again, once the pictures the old one holds are out. Until the first, access units are skipped,
// logged once, as RPCS3 skips MPEG-2 and MPEG-4 access units before a sequence header (au_has_sequence_header).
void vc1_open(u8* base, Vdec& v, const Vc1Headers& h, std::map<u64, AuMeta>& in_flight, u64 seq)
{
    if (h.seq_bdu.empty() || h.entry_bdu.empty()) {
        if (!v.vc1_skip_logged)
            PS_LOG_WARN(kChan, "decoder {:#x}: VC-1 access units before a sequence header and entry point are skipped",
                v.handle);
        v.vc1_skip_logged = true;
        return;
    }
    if (h.seq_bdu == v.vc1_opened) return; // open with it (or refused, logged)
    if (v.av) {
        avcodec_send_packet(v.av, nullptr);
        receive_pictures(base, v, in_flight, seq);
        avcodec_free_context(&v.av);
        PS_LOG_INFO(kChan, "decoder {:#x}: a new VC-1 sequence header: FFmpeg's decoder opens again", v.handle);
    }
    v.vc1_opened = h.seq_bdu;
    std::vector<u8> extradata = h.seq_bdu;
    extradata.insert(extradata.end(), h.entry_bdu.begin(), h.entry_bdu.end());
    open_ffmpeg(v, std::format("decoder {:#x}", v.handle), extradata); // refused: its access units are skipped
}
#endif

// ---- what a picture's item and information block hold (written when the picture is announced, as Sony's libvdec
// does; only the bytes Sony's writes, the others left as they were: D-912)

// The error word libdivx311dec (and libdivxdec) report for a picture decoded with errors (0x11 as seen), else 0.
constexpr u32 kDivxDamaged = 0x11;

// CellVdecDivxInfo of DivX 3.11 (0x18 bytes): the picture type (I 0, P 1), the size, the frame rate code
// (CELL_VDEC_DIVX_FRC_*: SetFrameRate's during the sequence, else 0), the frame buffer's address and the size again,
// then two bytes and a word libdivx311dec reports (0, 0x30, and 0 or kDivxDamaged).
void write_divx311_info(u8* base, u32 info, const Vdec& v, const Picture& p)
{
#if PSPOCKET_HAVE_FFMPEG
    const u8 type = p.pict_type == AV_PICTURE_TYPE_I ? 0 : 1;
#else
    const u8 type = 0;
#endif
    ps_write8(base, info + 0x00, type);
    ps_write16(base, info + 0x02, static_cast<u16>(p.width));
    ps_write16(base, info + 0x04, static_cast<u16>(p.height));
    ps_write16(base, info + 0x06, static_cast<u16>(v.seq_frame_rate));
    ps_write32(base, info + 0x08, p.frame);
    ps_write16(base, info + 0x0C, static_cast<u16>(p.width));
    ps_write16(base, info + 0x0E, static_cast<u16>(p.height));
    ps_write8(base, info + 0x10, 0);
    ps_write8(base, info + 0x11, 0x30);
    ps_write32(base, info + 0x14, p.damaged ? kDivxDamaged : 0);
}

// CellVdecDivxInfo of DivX (0x20 bytes): RPCS3's fields (rpcs3/Emu/Cell/Modules/cellVdec.h) from the stream's headers
// as Sony's libdivxdec reports them (the video object layer's aspect ratio code and pixel sizes; the visual object's
// colour description, 0s without one; the picture structure), the frame rate code (SetFrameRate's during the sequence,
// else 0), then libdivxdec's own: the frame buffer's address, the size again, two bytes (0, 0x30) and a word (0 or
// kDivxDamaged).
void write_divx_info(u8* base, u32 info, const Vdec& v, const Picture& p)
{
#if PSPOCKET_HAVE_FFMPEG
    const u8 type = p.pict_type == AV_PICTURE_TYPE_B ? 2 : p.pict_type == AV_PICTURE_TYPE_I ? 0 : 1; // P, S (sprite)
#else
    const u8 type = 0;
#endif
    const Mpeg4Vol& vol = p.meta.vol;
    ps_write8(base, info + 0x00, type);
    ps_write16(base, info + 0x02, static_cast<u16>(p.width));
    ps_write16(base, info + 0x04, static_cast<u16>(p.height));
    ps_write8(base, info + 0x06, vol.aspect_ratio_info);
    ps_write8(base, info + 0x07, vol.par_width);
    ps_write8(base, info + 0x08, vol.par_height);
    ps_write8(base, info + 0x09, vol.colour_description);
    ps_write8(base, info + 0x0A, vol.colour_description ? vol.colour_primaries : 0);
    ps_write8(base, info + 0x0B, vol.colour_description ? vol.transfer_characteristics : 0);
    ps_write8(base, info + 0x0C, vol.colour_description ? vol.matrix_coefficients : 0);
    ps_write8(base, info + 0x0D, !p.interlaced ? 0 : p.top_field_first ? 1 : 2); // FRAME, TOP_BTM, BTM_TOP
    ps_write16(base, info + 0x0E, static_cast<u16>(v.seq_frame_rate));
    ps_write32(base, info + 0x10, p.frame);
    ps_write16(base, info + 0x14, static_cast<u16>(p.width));
    ps_write16(base, info + 0x16, static_cast<u16>(p.height));
    ps_write8(base, info + 0x18, 0);
    ps_write8(base, info + 0x19, 0x30);
    ps_write32(base, info + 0x1C, p.damaged ? kDivxDamaged : 0);
}

// CellVdecMpeg4Info (0x38 bytes), as Sony's libvdec writes it from libsmvd4's side information: the video object
// layer's aspect ratio code and pixel sizes, time increment resolution, fixed VOP rate and increment, width and height
// (when the stream has given a layer), the VOP coding type, the picture's luma, Cb and Cr addresses in its frame buffer
// (Cr 8 bytes past Cb: the strips hold 8 of each a line), the width and height as words (read back from the block), the
// frame's luma lines (with the edge's), then 3, 0, 0 and 0 at 0x30, 0x31, 0x32 and 0x35.
void write_mpeg4_info(u8* base, u32 info, const Vdec& v, const Picture& p)
{
    const Mpeg4Vol& vol = p.meta.vol;
    if (vol.valid) {
        ps_write8(base, info + 0x00, vol.aspect_ratio_info);
        ps_write8(base, info + 0x01, vol.par_width);
        ps_write8(base, info + 0x02, vol.par_height);
        ps_write16(base, info + 0x04, vol.time_increment_resolution);
        ps_write8(base, info + 0x06, vol.fixed_vop_rate);
        ps_write16(base, info + 0x08, vol.fixed_vop_time_increment);
        ps_write16(base, info + 0x0A, vol.width);
        ps_write16(base, info + 0x0C, vol.height);
    }
#if PSPOCKET_HAVE_FFMPEG
    const u8 type = p.pict_type == AV_PICTURE_TYPE_I   ? 0
                    : p.pict_type == AV_PICTURE_TYPE_P ? 1
                    : p.pict_type == AV_PICTURE_TYPE_B ? 2
                                                       : 3;
#else
    const u8 type = 0;
#endif
    ps_write8(base, info + 0x0E, type);
    const u32 cb = p.frame + vdec::strip_cb(v.plan, p.width, p.height);
    ps_write32(base, info + 0x10, p.start);
    ps_write32(base, info + 0x14, cb);
    ps_write32(base, info + 0x18, cb + 8);
    ps_write32(base, info + 0x20, ps_read16(base, info + 0x0A));
    ps_write32(base, info + 0x24, ps_read16(base, info + 0x0C));
    ps_write32(base, info + 0x28, vdec::align(p.height, 16) + 32); // the frame's luma lines
    ps_write8(base, info + 0x30, 3);
    ps_write8(base, info + 0x31, 0);
    ps_write8(base, info + 0x32, 0);
    ps_write8(base, info + 0x35, 0);
}

// CellVdecVc1Info (0x170 bytes) as Sony's libvdec writes it from libsvc1d's side information (D-912): the sequence
// header as the decoder has read it when the picture is announced (its size, flags, display extension, aspect ratio,
// frame rate code, colour description), then the picture's own entry point and picture header fields, the headers its
// access unit carried, what the decoder holds valid, the picture's addresses in its frame buffer, the size again, the
// frame's lines and its range mapping. The bytes libvdec leaves are left.
void write_vc1_info(u8* base, u32 info, const Vdec& v, const Picture& p)
{
    const Vc1Sequence& s = v.vc1_current.seq;
    const Vc1Headers& h = p.meta.vc1;
    ps_write32(base, info + 0x00, s.max_coded_width);
    ps_write32(base, info + 0x04, s.max_coded_height);
    ps_write8(base, info + 0x08, s.pulldown);
    ps_write8(base, info + 0x09, s.interlace);
    ps_write8(base, info + 0x0A, s.tfcntrflag);
    ps_write8(base, info + 0x0B, s.finterpflag);
    ps_write8(base, info + 0x0C, s.postprocflag);
    ps_write8(base, info + 0x0D, s.psf);
    ps_write8(base, info + 0x0E, s.display_ext);
    ps_write16(base, info + 0x10, static_cast<u16>(s.display_ext ? s.display_width - 1 : 0));
    ps_write16(base, info + 0x12, static_cast<u16>(s.display_ext ? s.display_height - 1 : 0));
    ps_write8(base, info + 0x14, s.aspect_ratio_flag);
    ps_write8(base, info + 0x15, s.aspect_ratio);
    ps_write8(base, info + 0x16, static_cast<u8>(s.aspect_width ? s.aspect_width - 1 : 0));
    ps_write8(base, info + 0x17, static_cast<u8>(s.aspect_height ? s.aspect_height - 1 : 0));
    if (s.display_ext && s.framerate_flag && !s.framerate_ind) {
        // The frame rate code (CELL_VDEC_FRC_* less 0x80: 24000/1001 1, 24 2, 25 3, 30000/1001 4, 30 5, 50 6,
        // 60000/1001 7, 60 8; 0 for another) from FRAMERATENR and FRAMERATEDR.
        u8 code = 0;
        if (s.framerate_dr == 1) {
            constexpr u8 kRates[8] = {0, 2, 3, 5, 6, 8, 0, 0};
            code = kRates[s.framerate_nr & 7];
        } else if (s.framerate_dr == 2) {
            constexpr u8 kRates[8] = {0, 1, 0, 4, 0, 7, 0, 0};
            code = kRates[s.framerate_nr & 7];
        }
        ps_write8(base, info + 0x18, code);
    }
    ps_write8(base, info + 0x19, s.color_format_flag);
    ps_write8(base, info + 0x1A, s.color_prim);
    ps_write8(base, info + 0x1B, s.transfer_char);
    ps_write8(base, info + 0x1C, s.matrix_coef);
    u32 valid = 1;
    if (!h.entry_bdu.empty()) {
        valid |= 2;
        ps_write8(base, info + 0x1D, h.entry.broken_link);
        ps_write8(base, info + 0x1E, h.entry.closed_entry);
    }
    if (h.units & 4) {
        valid |= 4;
        // libsvc1d's picture type codes: I 1 and skipped 5 (seen); P 2, B 3 and BI 4 assumed in the standard's order.
        const u8 type = h.pic.type[0] == VC1_PCT_I ? 1 : h.pic.type[0] == VC1_PCT_P ? 2 : h.pic.type[0] == VC1_PCT_B ? 3
                        : h.pic.type[0] == VC1_PCT_BI ? 4 : 5;
        ps_write8(base, info + 0x28, 0);
        ps_write8(base, info + 0x1F, type);
        ps_write8(base, info + 0x22, h.pic.rptfrm);
        ps_write8(base, info + 0x23, h.pic.tff);
        ps_write8(base, info + 0x24, h.pic.rff);
        ps_write8(base, info + 0x25, 0);
        ps_write8(base, info + 0x20, 0); // a frame: one field's entries
        ps_write8(base, info + 0x26, 0);
        ps_write8(base, info + 0x34, 0); // its pan-scan windows: none
    }
    ps_write32(base, info + 0x2C, h.units);
    ps_write32(base, info + 0x30, valid);
    const u32 cb = p.frame + vdec::strip_cb(v.plan, p.width, p.height);
    ps_write32(base, info + 0x148, p.start);
    ps_write32(base, info + 0x14C, cb);
    ps_write32(base, info + 0x150, cb + 8);
    ps_write32(base, info + 0x158, s.max_coded_width);
    ps_write32(base, info + 0x15C, s.max_coded_height);
    ps_write32(base, info + 0x160, vdec::align(p.height, 32) + 128);
    ps_write8(base, info + 0x168, 3);
    ps_write8(base, info + 0x169, h.entry.range_mapy_flag);
    ps_write8(base, info + 0x16A, h.entry.range_mapuv_flag);
    ps_write8(base, info + 0x16B, h.entry.range_mapy);
    ps_write8(base, info + 0x16C, h.entry.range_mapuv);
    ps_write8(base, info + 0x16D, 0);
}

// H.264's information (CellVdecAvcInfo, 0x244 bytes), and MVC's base view's as H.264's; MPEG-2's as RPCS3 fills it.
void write_avc_info(u8* base, u32 info, const Vdec& v, const Picture& p)
{
    std::memset(base + info, 0, v.ring.info);
    if (v.codec == CODEC_MPEG2) { // CellVdecMpeg2Info as RPCS3 fills it (not compared with Sony's yet)
        ps_write16(base, info + 0x00, static_cast<u16>(p.width));
        ps_write16(base, info + 0x02, static_cast<u16>(p.height));
        return;
    }
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
    // Past RPCS3's structure, as Sony's for the streams tried (meanings inferred): the picture order count type, the
    // VUI timing, the picture's number and its size.
    ps_write8(base, info + 0x138, static_cast<u8>(s.poc_type));
    ps_write32(base, info + 0x154, s.time_scale);
    ps_write32(base, info + 0x158, s.units_in_tick);
    ps_write8(base, info + 0x15C, s.fixed_frame_rate);
    if (v.ring.info < 0x230) return; // MVC's block is 0x214 bytes (D-590): its own layout past that is not known
    ps_write32(base, info + 0x224, p.number);
    ps_write32(base, info + 0x228, p.width);
    ps_write32(base, info + 0x22C, p.height);
}

// CellVdecPicItem as Sony's libvdec fills it when it announces the picture (D-912): the codec, the start address, the
// size of the picture in 4:2:0, one access unit, its PTS, DTS and user data (the second access unit's left as the free
// element has them), the status (the PICOUT's error) and the attributes. The information block's address was set when
// the decoder opened.
void write_item(u8* base, const Vdec& v, const Picture& p, u32 status)
{
    const u32 item = vdec::item_addr(v.handle, p.meta.slot), info = vdec::info_addr(v.handle, v.ring, p.meta.slot);
    ps_write32(base, item + 0x00, v.codec);
    ps_write32(base, item + 0x04, p.start);
    ps_write32(base, item + 0x08, p.out_width * p.out_height + p.out_width * p.out_height / 2);
    ps_write8(base, item + 0x0C, 1); // auNum
    ps_write32(base, item + 0x10, p.meta.pts_upper);
    ps_write32(base, item + 0x14, p.meta.pts_lower);
    ps_write32(base, item + 0x20, p.meta.dts_upper);
    ps_write32(base, item + 0x24, p.meta.dts_lower);
    ps_write64(base, item + 0x30, p.meta.user_data);
    ps_write32(base, item + 0x40, status);
    ps_write32(base, item + 0x44, static_cast<u32>(p.meta.attr));
    switch (v.codec) {
    case CODEC_DIVX3_11: write_divx311_info(base, info, v, p); break;
    case CODEC_DIVX: write_divx_info(base, info, v, p); break;
    case CODEC_MPEG4: write_mpeg4_info(base, info, v, p); break;
    case CODEC_VC1: write_vc1_info(base, info, v, p); break;
    default: write_avc_info(base, info, v, p); break; // H.264, JVT, MVC's base view, MPEG-2
    }
}

// A ring element made free (under v.m): its item's fields as libvdec sets them then.
void release_slot(u8* base, Vdec& v, u32 slot)
{
    if (slot >= v.slots.size()) return;
    v.slots[slot] = 0;
    vdec::clear_item(base, vdec::item_addr(v.handle, slot));
}

// The ring's elements whose access units hold no picture any more (nothing held or out came of them): free (under
// v.m). Sony's libvdec frees an element when its access unit is done without a picture.
void release_orphans(u8* base, Vdec& v)
{
    std::vector<u8> used(v.slots.size(), 0);
    for (const Picture& p : v.held) used[p.meta.slot] = 1;
    for (const Picture& p : v.out) used[p.meta.slot] = 1;
    for (u32 i = 0; i < v.slots.size(); i++)
        if (v.slots[i] && !used[i]) release_slot(base, v, i);
}

// Announces the held pictures (all, or those of access units before `before`): each goes to the game's queue, its item
// and information written, then PICOUT.
void announce(PPUContext& ctx, Vdec& v, std::optional<u64> before)
{
    u8* base = guest_base(ctx);
    for (;;) {
        u32 status = 0;
        {
            std::unique_lock lk(v.m);
            if (v.held.empty() || (before && v.held.front().au >= *before)) return;
            Picture p = std::move(v.held.front());
            v.held.pop_front();
            if (p.seq != v.seq) continue; // a sequence restarted meanwhile
            if (v.out.size() >= kOutMax) {
                PS_LOG_WARN(kChan, "decoder {:#x}: {} pictures not taken; the oldest is dropped", v.handle, v.out.size());
                release_slot(base, v, v.out.front().meta.slot);
                v.out.pop_front();
            }
            status = p.damaged ? CELL_VDEC_ERROR_PIC : 0;
            p.out_width = p.width;
            p.out_height = p.height;
            if (v.codec == CODEC_VC1 && v.vc1_current.seq.max_coded_width && v.vc1_current.seq.max_coded_height) {
                p.out_width = v.vc1_current.seq.max_coded_width;
                p.out_height = v.vc1_current.seq.max_coded_height;
            }
            write_item(base, v, p, status);
            v.out.push_back(std::move(p));
        }
        callback(ctx, v, MSG_PICOUT, status);
    }
}

// The decoder's thread (an lv2 PPU thread, so the game's callback runs on a guest thread at the priority it asked):
// the commands in order.
void run(PPUContext& ctx, Vdec& v)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = guest_base(ctx);
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
            v.produced.clear();
            std::lock_guard lk(v.m);
            v.awaiting.clear();
            v.held.clear();
            v.out.clear();
            v.next_frame = 0;
            v.have_last = false;
            v.seq_frame_rate = 0; // libdivxdec's and libdivx311dec's StartSeq forget a frame rate set before it
            break;
        }
        case Command::au: {
            // Sony's cores hold an access unit back while they hold their most pictures (decoded and not yet released
            // by GetPicture): it waits for the game to take one (D-912).
            {
                std::unique_lock lk(v.m);
                park_until(t, v, lk, [&] {
                    return c.seq != v.seq || v.held.size() + v.out.size() + v.awaiting.size() < v.plan.limit;
                });
                if (c.seq != v.seq || v.dead) {
                    if (v.pending_aus) v.pending_aus--;
                    break;
                }
            }
            u32 au_error = 0;
            bool decoded = false;
            if (v.codec == CODEC_VC1 && c.mode == (DEC_MODE_B_SKIP | DEC_MODE_PB_SKIP)) {
                // libsvc1d refuses the mode libvdec takes (B and PB pictures skipped): libvdec reports it as a fatal
                // error (ERROR, CELL_VDEC_ERROR_FATAL) and its decoder does nothing more (no AUDONE, no SEQDONE).
                PS_LOG_ERROR(kChan, "decoder {:#x}: VC-1 access unit {} in decode mode 3: refused by the codec, as "
                                    "Sony's (a fatal error; the decoder stops)", v.handle, c.meta.index);
                {
                    std::lock_guard lk(v.m);
                    v.dead = true;
                    if (v.pending_aus) v.pending_aus--;
                }
                callback(ctx, v, MSG_ERROR, CELL_VDEC_ERROR_FATAL);
                break;
            }
            if (v.codec == CODEC_VC1 && v.level < VC1_AP_L0) {
                PS_LOG_ERROR(kChan, "decoder {:#x}: VC-1's simple and main profiles are not decoded (how libsvc1d is "
                                    "given their sequence header is not known, D-589): access unit {} skipped",
                    v.handle, c.meta.index);
                au_error = CELL_VDEC_ERROR_AU;
            }
#if PSPOCKET_HAVE_FFMPEG
            if (!au_error && v.codec == CODEC_VC1) vc1_open(base, v, c.meta.vc1, in_flight, c.seq);
            if (!au_error && v.codec == CODEC_VC1 && !v.av) au_error = CELL_VDEC_ERROR_AU; // before the headers
            // An access unit of the views past MVC's base one alone has nothing to decode (an empty packet would end
            // the stream).
            const bool nothing = c.own_data && c.au_size == 0;
            if (!au_error && v.av && !nothing) {
                AVPacket* pkt = av_packet_alloc();
                pkt->data = c.own_data ? c.data.data() : base + c.au_addr;
                pkt->size = static_cast<int>(c.au_size);
                pkt->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(c.meta.index));
                v.av->skip_frame = c.mode == DEC_MODE_NORMAL ? AVDISCARD_DEFAULT
                                   : c.mode == DEC_MODE_B_SKIP ? AVDISCARD_NONREF
                                                               : AVDISCARD_NONINTRA;
                // An access unit's entry stays until no picture can come of it: FFmpeg holds pictures for reordering,
                // at most 16 access units (H.264's deepest), so 64 back are gone.
                const u64 oldest = c.meta.index > 64 ? c.meta.index - 64 : 0;
                in_flight.erase(in_flight.begin(), in_flight.lower_bound(oldest));
                in_flight[c.meta.index] = c.meta;
                {
                    std::lock_guard lk(v.m);
                    v.awaiting.insert(c.meta.index);
                }
                if (const int r = avcodec_send_packet(v.av, pkt); r < 0)
                    PS_LOG_ERROR(kChan, "decoder {:#x}: access unit {} ({:#x}, {} bytes) refused by the decoder ({})",
                        v.handle, c.meta.index, c.au_addr, c.au_size, r);
                av_packet_free(&pkt);
                receive_pictures(base, v, in_flight, c.seq, c.meta.index);
                decoded = true;
            }
#endif
            if (v.codec == CODEC_VC1 && !c.meta.vc1.seq_bdu.empty()) v.vc1_current = c.meta.vc1;
            {
                std::lock_guard lk(v.m);
                if (v.pending_aus) v.pending_aus--;
                if (!decoded) release_slot(base, v, c.meta.slot); // no picture comes of it
            }
            callback(ctx, v, MSG_AUDONE, au_error);
            // The pictures of earlier access units (Sony's one-unit lag), or of this one too for a core without it.
            announce(ctx, v, v.plan.lag ? c.meta.index : c.meta.index + 1);
            break;
        }
        case Command::drain: {
            {
                std::lock_guard lk(v.m);
                if (c.seq != v.seq || v.dead) break;
            }
#if PSPOCKET_HAVE_FFMPEG
            if (v.av) {
                avcodec_send_packet(v.av, nullptr);
                receive_pictures(base, v, in_flight, c.seq);
            }
#endif
            announce(ctx, v, std::nullopt);
#if PSPOCKET_HAVE_FFMPEG
            if (v.av) avcodec_flush_buffers(v.av);
#endif
            std::lock_guard lk(v.m);
            v.awaiting.clear(); // what the decoder had not given by now gives nothing
            release_orphans(base, v);
            break;
        }
        case Command::end: {
            {
                std::lock_guard lk(v.m);
                if (v.dead) break; // a decoder stopped by a fatal error ends no sequence
                v.state = Seq::dormant;
            }
            callback(ctx, v, MSG_SEQDONE);
            break;
        }
        case Command::framerate: {
            std::lock_guard lk(v.m); // GetPicItem reads it on the game's thread (D-601)
            v.frame_rate = c.frame_rate;
            // DivX's and DivX 3.11's codes (CELL_VDEC_DIVX_FRC_*, from 1): libvdec gives the core the code plus one.
            v.seq_frame_rate = (c.frame_rate & 7) + 1;
            break;
        }
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
    // Sony's libvdec opens with the codec's part of what QueryAttrEx asks for (one byte less than all of it opens, D-196),
    // and refuses less (D-912).
    if (mem_size < attr.codec_part) {
        PS_LOG_WARN(kChan, "{}: {:#x} bytes, less than the codec's {:#x} (of the {:#x} asked for): refused, as Sony's",
            call, mem_size, attr.codec_part, attr.mem_size);
        return error(CELL_VDEC_ERROR_ARG);
    }
    if (!usable(ctx, mem, mem_size, "the decoder's memory")) return error(CELL_VDEC_ERROR_ARG);
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
    v->ring = vdec::ring_of(codec);
    v->sizing = attr.sizing;
    v->plan = vdec::plan_for(codec, mem, attr.sizing);
    v->slots.assign(v->ring.count, 0);
    if (codec == CODEC_DIVX3_11) {
        // DivX 3.11's frame size (D-588): the stream has none, so it is the codec specific information's largest frame
        // (query_attr checked it). Without the information Sony's libvdec reads the size from address 4 (its pointer
        // to the information is a register left at 0, D-912), which faults on a PS3: a game always gives it.
        // pspocket's decoder assumes the query's 720x576 then, logged.
        v->divx_width = attr.sizing.width;
        v->divx_height = attr.sizing.height;
        if (!spec)
            PS_LOG_WARN(kChan, "{}: DivX 3.11 without its frame size (CellVdecDivxSpecificInfo2): Sony's libvdec faults "
                               "here (it reads address 4); {}x{} assumed", call, v->divx_width, v->divx_height);
    }
    if (codec == CODEC_VC1 && level < VC1_AP_L0)
        PS_LOG_ERROR(kChan, "{}: VC-1's simple and main profiles (level {}) open as Sony's, but are not decoded: their "
                            "streams carry no sequence header, and how libsvc1d is given it is not known (D-589)",
            call, level);
    // The ring as libvdec sets it up in the decoder's memory: each element's item free, and its information block's
    // address (the item's picInfo).
    for (u32 i = 0; i < v->ring.count; i++) {
        const u32 item = vdec::item_addr(mem, i);
        vdec::clear_item(base, item);
        ps_write32(base, item + 0x48, vdec::info_addr(mem, v->ring, i));
    }
#if PSPOCKET_HAVE_FFMPEG
    if (codec == CODEC_VC1) { // FFmpeg's decoder opens with the stream's headers (vc1_open)
        if (!decoder_for(codec)) {
            PS_LOG_ERROR(kChan, "{}: codec {} is not decoded by pspocket (no decoder in its FFmpeg)", call, codec);
            return error(CELL_VDEC_ERROR_FATAL);
        }
    } else if (!open_ffmpeg(*v, call)) {
        return error(CELL_VDEC_ERROR_FATAL);
    }
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

// The picture at the size libvdec gives the game (Picture::out_width): cut, or the samples past its edge repeating it.
void fit_to_output(Picture& p)
{
    if (!p.out_width || !p.out_height || (p.out_width == p.width && p.out_height == p.height)) return;
    const u32 w = p.out_width, h = p.out_height;
    std::vector<u8> y(w * h), u((w / 2) * (h / 2)), v((w / 2) * (h / 2));
    for (u32 r = 0; r < h; r++)
        for (u32 x = 0; x < w; x++) y[r * w + x] = p.y[std::min(r, p.height - 1) * p.width + std::min(x, p.width - 1)];
    for (u32 r = 0; r < h / 2; r++)
        for (u32 x = 0; x < w / 2; x++) {
            const u32 at = std::min(r, p.height / 2 - 1) * (p.width / 2) + std::min(x, p.width / 2 - 1);
            u[r * (w / 2) + x] = p.u[at];
            v[r * (w / 2) + x] = p.v[at];
        }
    p.y = std::move(y);
    p.u = std::move(u);
    p.v = std::move(v);
    p.width = w;
    p.height = h;
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
    // libvdec sets its ring up again: every element free (D-912).
    for (u32 i = 0; i < v->slots.size(); i++) release_slot(guest_base(ctx), *v, i);
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

// DecodeAu, and DecodeAuEx2 with its access unit information as a CellVdecAuInfo (D-596).
s32 decode_au(PPUContext& ctx, u32 handle, s32 mode, u32 au_info, const std::string& call, bool ex2 = false)
{
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
    // The access unit's ring element: the lowest free one (none free: FATAL, as libvdec answers it).
    const auto free_slot = std::find(v->slots.begin(), v->slots.end(), 0);
    if (free_slot == v->slots.end()) {
        PS_LOG_ERROR(kChan, "{}: all {} of the decoder's ring elements hold access units or pictures", call,
            v->slots.size());
        return finish(error(CELL_VDEC_ERROR_FATAL), call);
    }
    if ((v->codec == CODEC_MVC || v->codec == CODEC_MVC2) && !ex2) {
        // Sony's libvdec gives libmvcdec an access unit whose size is the word cellVdecDecodeAu leaves 0 (its MVC
        // decoder reads the information as DecodeAuEx2's), and mvcDecAccessUnit refuses it ("bad parameter"): MVC never
        // decodes through cellVdecDecodeAu, FATAL (D-912).
        PS_LOG_ERROR(kChan, "{}: MVC through cellVdecDecodeAu is refused, as Sony's libvdec refuses it (its MVC core "
                            "gets a size of 0)", call);
        return finish(error(CELL_VDEC_ERROR_FATAL), call);
    }
    if ((v->codec == CODEC_MVC || v->codec == CODEC_MVC2) && !v->mvc_logged)
        // Through DecodeAuEx2 Sony's libvdec gives libmvcdec the picture information's address from the caller's saved
        // r26 (0x10 bytes past its own) and writes the information there: nothing to match (D-912). pspocket decodes
        // the base view (D-590).
        PS_LOG_WARN(kChan, "{}: MVC through DecodeAuEx2: Sony's libvdec writes the picture information through the "
                           "caller's r26 here; pspocket decodes the base view", call);
    if (v->pending_aus >= 4) return finish(error(CELL_VDEC_ERROR_BUSY), call);
    // CellVdecAuInfo: startAddr, size, pts, dts, userData, codecSpecificData.
    Command c(Command::au);
    c.meta.slot = static_cast<u32>(free_slot - v->slots.begin());
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
    const bool mvc = v->codec == CODEC_MVC || v->codec == CODEC_MVC2;
    if (v->codec == CODEC_AVC || v->codec == CODEC_JVT || mvc)
        scan_avc_au({base + c.au_addr, c.au_size}, c.meta.avc, v->sps); // MVC: the base view's (D-590)
    c.meta.sps = v->sps;
    if (mvc) {
        // MVC (D-590): the decoder is given the base view, a copy made now (the game may reuse its buffer).
        if (mvc_base_view({base + c.au_addr, c.au_size}, c.data) && !v->mvc_logged) {
            PS_LOG_WARN(kChan, "decoder {:#x}: MVC's views past the base one (NAL units 14, 15, 20) are not decoded: "
                               "the base view's pictures only (2D)", v->handle);
            v->mvc_logged = true;
        }
        c.own_data = true;
        c.au_size = static_cast<u32>(c.data.size());
        c.data.resize(c.data.size() + kPacketPadding, 0);
    }
    if (v->codec == CODEC_MPEG4 || v->codec == CODEC_DIVX) {
        scan_mpeg4_au({base + c.au_addr, c.au_size}, v->vol);
        c.meta.vol = v->vol;
    }
    if (v->codec == CODEC_VC1) {
        Vc1Picture pic;
        bool other_profile = false;
        u32 units = 0;
        scan_vc1_au({base + c.au_addr, c.au_size}, v->vc1, pic, other_profile, units);
        if (other_profile && !v->vc1_other_logged)
            PS_LOG_ERROR(kChan, "decoder {:#x}: a VC-1 sequence header of the simple or main profile is not decoded "
                                "(D-589)", v->handle);
        v->vc1_other_logged |= other_profile;
        c.meta.vc1 = v->vc1;
        c.meta.vc1.pic = pic;
        c.meta.vc1.units = units;
    }
    v->pending_aus++;
    v->slots[c.meta.slot] = 1;
    push_command(*v, std::move(c));
    return lv2::CELL_OK;
}

s32 cellVdecDecodeAu(PPUContext& ctx, u32 handle, s32 mode, u32 au_info)
{
    const std::string call = std::format("cellVdecDecodeAu({:#x}, {}, {:#x})", handle, mode, au_info);
    return decode_au(ctx, handle, mode, au_info, call);
}

// ---- RPCS3's, unverified (D-594, D-596): rpcs3/Emu/Cell/Modules/cellVdec.cpp cellVdecOpenExt, cellVdecSetPts,
// cellVdecDecodeAuEx2 and the structures RPCS3 calls speculative (cellVdec.h CellVdecResourceExt, CellVdecAuInfoEx2).

// Open on CellVdecResourceExt: memAddr, memSize, ppuThreadPriority, ppuThreadStackSize, 12 bytes RPCS3 does not know,
// numOfSpus (at 0x1C). RPCS3 opens with a CellVdecResource of those and an SPU priority of 0 (pspocket's decoder runs
// on its PPU thread whatever the SPUs); no resource is ARG. The codec type is CellVdecType's, with no specific
// information (Open's).
s32 cellVdecOpenExt(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle)
{
    const std::string call = std::format("cellVdecOpenExt({:#x}, {:#x}, {:#x}, {:#x})", type, res, cb, handle);
    if (!res || !usable(ctx, res, 0x20, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    StackVar resource(ctx, base, 0x18);
    for (u32 i = 0; i < 4; i++) ps_write32(base, resource.addr() + 4 * i, ps_read32(base, res + 4 * i));
    ps_write32(base, resource.addr() + 16, 0);
    ps_write32(base, resource.addr() + 20, ps_read32(base, res + 0x1C));
    return finish(open(ctx, type, 0, resource.addr(), cb, handle, call), call);
}

// RPCS3's checks and answers: no decoder or no pointer ARG, a closed decoder SEQ, else CELL_OK. What the PTS it is
// given sets is not known (RPCS3 reads nothing of it and logs the call as an error): nothing is done with it, logged
// once a decoder.
s32 cellVdecSetPts(PPUContext& ctx, u32 handle, u32 pts)
{
    const std::string call = std::format("cellVdecSetPts({:#x}, {:#x})", handle, pts);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v || !pts) return finish(error(CELL_VDEC_ERROR_ARG), call);
    std::lock_guard lk(v->m);
    if (v->state == Seq::closed) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    if (!v->set_pts_logged)
        PS_LOG_WARN(kChan, "{}: what it sets is not known (RPCS3 answers it and does nothing): ignored (logged once a "
                           "decoder)", call);
    v->set_pts_logged = true;
    return finish(lv2::CELL_OK, call);
}

// DecodeAu on CellVdecAuInfoEx2: startAddr, a word RPCS3 does not know, size, another, pts, dts, userData,
// codecSpecificData (0x30 bytes): RPCS3 decodes it as the CellVdecAuInfo of those (and so does pspocket, with
// DecodeAu's checks and answers); the two unknown words are logged once a decoder when they are not 0.
s32 cellVdecDecodeAuEx2(PPUContext& ctx, u32 handle, s32 mode, u32 au_info)
{
    const std::string call = std::format("cellVdecDecodeAuEx2({:#x}, {}, {:#x})", handle, mode, au_info);
    if (!au_info || !usable(ctx, au_info, 0x30, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    const u32 unknown[2] = {ps_read32(base, au_info + 4), ps_read32(base, au_info + 12)};
    if (const std::shared_ptr<Vdec> v = live(ctx, handle); v && (unknown[0] || unknown[1])) {
        std::lock_guard lk(v->m);
        if (!v->au_ex2_logged)
            PS_LOG_WARN(kChan, "{}: the words RPCS3 does not know are {:#x} and {:#x}: not used (logged once a decoder)",
                call, unknown[0], unknown[1]);
        v->au_ex2_logged = true;
    }
    StackVar info(ctx, base, 0x28);
    ps_write32(base, info.addr() + 0, ps_read32(base, au_info + 0));
    ps_write32(base, info.addr() + 4, ps_read32(base, au_info + 8));
    for (u32 at = 0; at < 0x20; at += 4) ps_write32(base, info.addr() + 8 + at, ps_read32(base, au_info + 0x10 + at));
    return decode_au(ctx, handle, mode, info.addr(), call, true);
}

s32 cellVdecGetPicItem(PPUContext& ctx, u32 handle, u32 item_out)
{
    const std::string call = std::format("cellVdecGetPicItem({:#x}, {:#x})", handle, item_out);
    const std::shared_ptr<Vdec> v = live(ctx, handle);
    if (!v || !item_out || !usable(ctx, item_out, 4, call)) return finish(error(CELL_VDEC_ERROR_ARG), call);
    u8* base = guest_base(ctx);
    std::lock_guard lk(v->m);
    if (v->state == Seq::closed) return finish(error(CELL_VDEC_ERROR_SEQ), call);
    // The item libvdec filled when it announced the picture, in the picture's ring element (D-912).
    for (Picture& p : v->out) {
        if (p.item_taken) continue;
        p.item_taken = true;
        ps_write32(base, item_out, vdec::item_addr(v->handle, p.meta.slot));
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
        // The picture released: its ring element free, its frame buffer the core's again (an access unit held back
        // for it goes on).
        release_slot(base, *v, p.meta.slot);
        wake_decoder(*v);
    }
    fit_to_output(p);
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
        // Made from RPCS3, not compared with Sony's libvdec (D-594, D-596).
        PS_HLE_UNVERIFIED(0xEF4D8AD7, cellVdecOpenExt),
        PS_HLE_UNVERIFIED(0x3CE2E4F8, cellVdecSetPts),
        PS_HLE_UNVERIFIED(0x1533C6A9, cellVdecDecodeAuEx2),
        PS_HLE_INTERNAL(NID_vdec_thread, _pspocket_vdec_thread),
    });
}

} // namespace pspocket::hle
