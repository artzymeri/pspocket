// cellPamf: the reader of PAMF headers, the container format of the PS3's movies (the stream list, each stream's
// codec parameters, the entry points a player seeks to). Pure parsing over the header in guest memory: no SPU, no
// decoder. The first of pspocket's own media libraries (D-179, D-193): with it a game's movies need no libpamf.sprx
// from the firmware.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellPamf.cpp/.h, which reproduces Sony's libpamf down to its bugs (the
// header verification, the stream type and channel mapping, the entry point formats, the functions "broken on LLE").
// Every check, answer and bug here is RPCS3's; a comment marks each bug kept.
//
// Differences from RPCS3:
// - cellPamfReaderGetEpWithTimeStamp gives the next entry point's offset in bytes, as Sony's libpamf does; RPCS3 gives
//   it in 2048-byte units. Found by comparing every call with Sony's libpamf from the user's firmware (D-193,
//   tests/hle/test_pamf.cpp): the rest answers and writes the same, byte for byte.
// - Where RPCS3 ensure()s a pointer LLE never checks (it would fault), pspocket logs the call site and answers
//   CELL_PAMF_ERROR_INVALID_ARG (D-042), and a header that runs off guest memory is refused the same way instead of
//   faulting.

#include <array>
#include <bitset>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellPamf";

enum : u32 {
    CELL_PAMF_ERROR_STREAM_NOT_FOUND = 0x80610501,
    CELL_PAMF_ERROR_INVALID_PAMF = 0x80610502,
    CELL_PAMF_ERROR_INVALID_ARG = 0x80610503,
    CELL_PAMF_ERROR_UNKNOWN_TYPE = 0x80610504,
    CELL_PAMF_ERROR_UNSUPPORTED_VERSION = 0x80610505,
    CELL_PAMF_ERROR_UNKNOWN_STREAM = 0x80610506,
    CELL_PAMF_ERROR_EP_NOT_FOUND = 0x80610507,
    CELL_PAMF_ERROR_NOT_AVAILABLE = 0x80610508,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_PAMF_ATTRIBUTE_VERIFY_ON = 1, CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER = 2;

enum : u8 {
    CELL_PAMF_STREAM_TYPE_AVC = 0,
    CELL_PAMF_STREAM_TYPE_M2V = 1,
    CELL_PAMF_STREAM_TYPE_ATRAC3PLUS = 2,
    CELL_PAMF_STREAM_TYPE_PAMF_LPCM = 3,
    CELL_PAMF_STREAM_TYPE_AC3 = 4,
    CELL_PAMF_STREAM_TYPE_USER_DATA = 5,
    CELL_PAMF_STREAM_TYPE_PSMF_AVC = 6,
    CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS = 7,
    CELL_PAMF_STREAM_TYPE_PSMF_LPCM = 8,
    CELL_PAMF_STREAM_TYPE_PSMF_USER_DATA = 9,
    CELL_PAMF_STREAM_TYPE_VIDEO = 20,
    CELL_PAMF_STREAM_TYPE_AUDIO = 21,
    CELL_PAMF_STREAM_TYPE_UNK = 22,
};

enum : u8 {
    PAMF_STREAM_CODING_TYPE_M2V = 0x02,
    PAMF_STREAM_CODING_TYPE_AVC = 0x1b,
    PAMF_STREAM_CODING_TYPE_PAMF_LPCM = 0x80,
    PAMF_STREAM_CODING_TYPE_AC3 = 0x81,
    PAMF_STREAM_CODING_TYPE_ATRAC3PLUS = 0xdc,
    PAMF_STREAM_CODING_TYPE_USER_DATA = 0xdd,
    PAMF_STREAM_CODING_TYPE_PSMF = 0xff,
};

constexpr u32 CELL_PAMF_M2V_UNKNOWN = 255;
constexpr u64 CODEC_TS_INVALID = 0xFFFFFFFF;

// ---- The header in guest memory: packed big-endian structures (RPCS3 cellPamf.h), as offsets.
// PamfHeader: magic, version, header_size and data_size (both in 2048-byte units), psmf_marks offset and size, unk
// offset and size, 0x30 reserved, then the sequence info.
namespace pamf {
constexpr u32 kMagic = 0x00, kVersion = 0x04, kHeaderSize = 0x08, kDataSize = 0x0C, kMarksOffset = 0x10,
              kMarksSize = 0x14, kUnkOffset = 0x18, kUnkSize = 0x1C;
constexpr u32 kSeqInfo = 0x50; // PamfSequenceInfo: size, u16 reserved, start pts (u16 high, u32 low), end pts, mux rate,
                               // std delay, u32 total streams, reserved, grouping period count
constexpr u32 kSeqStartHigh = 0x06, kSeqStartLow = 0x08, kSeqEndHigh = 0x0C, kSeqEndLow = 0x0E, kSeqMuxRate = 0x12,
              kSeqTotalStreams = 0x1A, kSeqGroupingPeriodNum = 0x1F, kSeqGroupingPeriods = 0x20;
constexpr u32 kGroupingPeriod = kSeqInfo + kSeqGroupingPeriods; // PamfGroupingPeriod: size, start pts, end pts, group num
constexpr u32 kGpStartHigh = 0x04, kGpStartLow = 0x06, kGpEndLow = 0x0C, kGpGroupNum = 0x11, kGpGroups = 0x12;
constexpr u32 kGroup = kGroupingPeriod + kGpGroups; // PamfGroup: size, reserved, stream num, streams
constexpr u32 kGroupStreamNum = 0x05, kGroupStreams = 0x06;
constexpr u32 kStreams = kGroup + kGroupStreams; // 0x88
// PamfStreamHeader (48 bytes): coding type, 3 reserved, stream_id, private_stream_id, P-STD buffer, EP table offset
// and count, then 32 bytes of codec information.
constexpr u32 kStreamSize = 48, kStCodingType = 0, kStStreamId = 4, kStPrivateId = 5, kStEpOffset = 8, kStEpNum = 12,
              kStInfo = 16;
constexpr u32 kEpSize = 12; // PamfEpHeader: u16 value0, u16 pts high, u32 pts low, u32 rpnOffset
} // namespace pamf

// The PSMF layout (PSP movies), which libpamf reads too: the sequence info at 0x50 as well, no coding type byte,
// 16-byte stream headers.
namespace psmf {
constexpr u32 kSeqStartHigh = 0x04, kSeqStartLow = 0x06, kSeqEndHigh = 0x0A, kSeqEndLow = 0x0C, kSeqMuxRate = 0x10;
constexpr u32 kGroupStreams = 0x06; // in PsmfGroup (at 0x50 + 0x1A + 0x12); its stream count at 5, as PAMF's
constexpr u32 kStreamSize = 16, kStStreamId = 0, kStPrivateId = 1, kStEpOffset = 4, kStEpNum = 8, kStInfo = 12;
constexpr u32 kEpSize = 10; // PsmfEpHeader: u16 value0, u32 pts low, u32 rpnOffset
} // namespace psmf

// CellPamfReader (128 bytes, the game's).
namespace reader {
constexpr u32 kSize = 128, kHeaderSize = 0x00, kDataSize = 0x08, kAttribute = 0x10, kIsPsmf = 0x14, kVersion = 0x16,
              kGroupingPeriodIndex = 0x18, kGroupIndex = 0x1C, kStreamIndex = 0x20, kHeader = 0x24, kSequenceInfo = 0x28,
              kCurrentGroupingPeriod = 0x2C, kCurrentGroup = 0x30, kCurrentStream = 0x34;
} // namespace reader

// CellPamfEpIterator: b8 isPamf, u32 index (at 4), u32 num, u32 pCur.
namespace iter {
constexpr u32 kSize = 16, kIsPamf = 0, kIndex = 4, kNum = 8, kCur = 12;
} // namespace iter

constexpr u32 kEpSize = 0x18, kEpUnkSize = 0x20; // CellPamfEp, CellPamfEpUnk (the EP, then nextRpnOffset)

s32 finish(s32 r, const std::string& call)
{
    if (r < 0) PS_LOG_WARN(kChan, "{}: {:#010x}", call, static_cast<u32>(r));
    else PS_LOG_DEBUG(kChan, "{}: {}", call, r);
    return r;
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: LLE would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("cellPamf: {} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

struct Guest {
    u8* b;
    u8 r8(u32 a) const { return ps_read8(b, a); }
    u16 r16(u32 a) const { return ps_read16(b, a); }
    u32 r32(u32 a) const { return ps_read32(b, a); }
    u64 r64(u32 a) const { return ps_read64(b, a); }
    void w8(u32 a, u8 v) const { ps_write8(b, a, v); }
    void w16(u32 a, u16 v) const { ps_write16(b, a, v); }
    void w32(u32 a, u32 v) const { ps_write32(b, a, v); }
    void w64(u32 a, u64 v) const { ps_write64(b, a, v); }
};

// RPCS3 pamfVerifyMagicAndVersion: "PAMF", version "0040" or "0041" (written to the reader when there is one).
s32 verify_magic_and_version(const Guest& g, u32 header, u32 self)
{
    if (g.r32(header + pamf::kMagic) != 0x50414D46) return error(CELL_PAMF_ERROR_UNKNOWN_TYPE); // "PAMF"
    if (self) g.w16(self + reader::kIsPsmf, 0);
    u16 version = 0;
    if (g.r32(header + pamf::kVersion) == 0x30303430) version = 40; // "0040"
    else if (g.r32(header + pamf::kVersion) == 0x30303431) version = 41; // "0041"
    else return error(CELL_PAMF_ERROR_UNSUPPORTED_VERSION);
    if (self) g.w16(self + reader::kVersion, version);
    return lv2::CELL_OK;
}

// RPCS3 pamfTypeChannelToStream. LLE breaks on a channel over 15; that is refused here.
bool type_channel_to_stream(u8 type, u8 ch, u8* coding_type, u8* stream_id, u8* private_stream_id)
{
    if (ch >= 16) return false;
    u8 c = 0, id = 0, priv = 0;
    switch (type) {
    case CELL_PAMF_STREAM_TYPE_AVC: c = PAMF_STREAM_CODING_TYPE_AVC, id = 0xe0 | ch; break;
    case CELL_PAMF_STREAM_TYPE_M2V: c = PAMF_STREAM_CODING_TYPE_M2V, id = 0xe0 | ch; break;
    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS: c = PAMF_STREAM_CODING_TYPE_ATRAC3PLUS, id = 0xbd, priv = ch; break;
    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM: c = PAMF_STREAM_CODING_TYPE_PAMF_LPCM, id = 0xbd, priv = 0x40 | ch; break;
    case CELL_PAMF_STREAM_TYPE_AC3: c = PAMF_STREAM_CODING_TYPE_AC3, id = 0xbd, priv = 0x30 | ch; break;
    case CELL_PAMF_STREAM_TYPE_USER_DATA: c = PAMF_STREAM_CODING_TYPE_USER_DATA, id = 0xbd, priv = 0x20 | ch; break;
    case CELL_PAMF_STREAM_TYPE_PSMF_AVC: c = PAMF_STREAM_CODING_TYPE_PSMF, id = 0xe0 | ch; break;
    case CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS: c = PAMF_STREAM_CODING_TYPE_PSMF, id = 0xbd, priv = ch; break;
    case CELL_PAMF_STREAM_TYPE_PSMF_LPCM: c = PAMF_STREAM_CODING_TYPE_PSMF, id = 0xbd, priv = 0x10 | ch; break;
    case CELL_PAMF_STREAM_TYPE_PSMF_USER_DATA: c = PAMF_STREAM_CODING_TYPE_PSMF, id = 0xbd, priv = 0x20 | ch; break;
    default: PS_LOG_ERROR(kChan, "pamfTypeChannelToStream(): unknown type {}", type); return false;
    }
    if (coding_type) *coding_type = c;
    if (stream_id) *stream_id = id;
    if (private_stream_id) *private_stream_id = priv;
    return true;
}

// RPCS3 pamfStreamToTypeChannel: CELL_OK or CELL_PAMF_ERROR_STREAM_NOT_FOUND.
s32 stream_to_type_channel(u8 coding_type, u8 stream_id, u8 private_stream_id, u8* type, u8* ch)
{
    u8 t = 0, c = 0;
    switch (coding_type) {
    case PAMF_STREAM_CODING_TYPE_AVC: t = CELL_PAMF_STREAM_TYPE_AVC, c = stream_id & 0x0f; break;
    case PAMF_STREAM_CODING_TYPE_M2V: t = CELL_PAMF_STREAM_TYPE_M2V, c = stream_id & 0x0f; break;
    case PAMF_STREAM_CODING_TYPE_ATRAC3PLUS: t = CELL_PAMF_STREAM_TYPE_ATRAC3PLUS, c = private_stream_id & 0x0f; break;
    case PAMF_STREAM_CODING_TYPE_PAMF_LPCM: t = CELL_PAMF_STREAM_TYPE_PAMF_LPCM, c = private_stream_id & 0x0f; break;
    case PAMF_STREAM_CODING_TYPE_AC3: t = CELL_PAMF_STREAM_TYPE_AC3, c = private_stream_id & 0x0f; break;
    case PAMF_STREAM_CODING_TYPE_USER_DATA: t = CELL_PAMF_STREAM_TYPE_USER_DATA, c = private_stream_id & 0x0f; break;
    case PAMF_STREAM_CODING_TYPE_PSMF:
        if ((stream_id & 0xf0) == 0xe0) {
            t = CELL_PAMF_STREAM_TYPE_PSMF_AVC, c = stream_id & 0x0f;
            if (private_stream_id != 0) return error(CELL_PAMF_ERROR_STREAM_NOT_FOUND);
        } else if (stream_id == 0xbd) {
            c = private_stream_id & 0x0f;
            switch (private_stream_id & 0xf0) {
            case 0x00: t = CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS; break;
            case 0x10: t = CELL_PAMF_STREAM_TYPE_PSMF_LPCM; break;
            case 0x20: t = CELL_PAMF_STREAM_TYPE_PSMF_LPCM; break; // LLE's (not PSMF_USER_DATA)
            default: return error(CELL_PAMF_ERROR_STREAM_NOT_FOUND);
            }
        } else {
            return error(CELL_PAMF_ERROR_STREAM_NOT_FOUND);
        }
        break;
    default:
        PS_LOG_ERROR(kChan, "pamfStreamToTypeChannel(): unknown stream_coding_type {:#04x}", coding_type);
        return error(CELL_PAMF_ERROR_STREAM_NOT_FOUND);
    }
    if (type) *type = t;
    if (ch) *ch = c;
    return lv2::CELL_OK;
}

// RPCS3 pamfEpUnpack / psmfEpUnpack: a packed entry point into a CellPamfEp.
void ep_unpack(const Guest& g, bool is_pamf, u32 packed, u32 ep)
{
    const u16 value0 = g.r16(packed);
    g.w32(ep + 0, (value0 >> 14) + 1u); // indexN
    if (is_pamf) {
        g.w32(ep + 4, (value0 & 0x1fffu) * 0x800u + 0x800u); // nThRefPictureOffset
        g.w32(ep + 8, g.r16(packed + 2));                     // pts.upper
        g.w32(ep + 12, g.r32(packed + 4));                    // pts.lower
        g.w64(ep + 16, g.r32(packed + 8) * 0x800ull);         // rpnOffset
    } else {
        g.w32(ep + 4, (value0 & 0xffeu) * 0x400u + 0x800u);
        g.w32(ep + 8, value0 & 1u);
        g.w32(ep + 12, g.r32(packed + 2));
        g.w64(ep + 16, g.r32(packed + 6) * 0x800ull);
    }
}

// RPCS3 pamfIsSameStreamType.
bool same_stream_type(u8 type, u8 requested)
{
    switch (requested) {
    case CELL_PAMF_STREAM_TYPE_VIDEO: return type == CELL_PAMF_STREAM_TYPE_AVC || type == CELL_PAMF_STREAM_TYPE_M2V;
    case CELL_PAMF_STREAM_TYPE_AUDIO:
        return type == CELL_PAMF_STREAM_TYPE_ATRAC3PLUS || type == CELL_PAMF_STREAM_TYPE_AC3 ||
               type == CELL_PAMF_STREAM_TYPE_PAMF_LPCM;
    case CELL_PAMF_STREAM_TYPE_UNK: // RPCS3: "no idea what this is for"
        return type == CELL_PAMF_STREAM_TYPE_PAMF_LPCM || type == CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS;
    default: return requested == type;
    }
}

s32 invalid(const char* why)
{
    PS_LOG_WARN(kChan, "pamfVerify() failed: {}", why);
    return error(CELL_PAMF_ERROR_INVALID_PAMF);
}

// RPCS3 pamfVerify, check by check. `self` (0: none) gets the version, the header and data sizes.
s32 verify(PPUContext& ctx, const Guest& g, u32 header, u64 file_size, u32 self, u32 attribute)
{
    if (!usable(ctx, header, pamf::kStreams, "the PAMF header")) return error(CELL_PAMF_ERROR_INVALID_ARG);
    if (const s32 r = verify_magic_and_version(g, header, self); r != lv2::CELL_OK) return r;
    const u64 header_size = g.r32(header + pamf::kHeaderSize) * 0x800ull;
    const u64 data_size = g.r32(header + pamf::kDataSize) * 0x800ull;
    if (header_size == 0) return invalid("invalid header_size");
    if (self) {
        g.w64(self + reader::kHeaderSize, header_size);
        g.w64(self + reader::kDataSize, data_size);
    }
    if (file_size != 0 && header_size + data_size != file_size)
        return invalid("fileSize isn't equal header_size + data_size");
    // The rest of the header is read from here on: it must be guest memory (D-042).
    if (header_size > 0xFFFFFFFFull || !usable(ctx, header, static_cast<u32>(header_size), "the PAMF header"))
        return error(CELL_PAMF_ERROR_INVALID_ARG);

    const u32 marks_offset = g.r32(header + pamf::kMarksOffset), marks_size = g.r32(header + pamf::kMarksSize);
    const u32 unk_offset = g.r32(header + pamf::kUnkOffset), unk_size = g.r32(header + pamf::kUnkSize);
    if (marks_offset == 0) {
        if (marks_size != 0) return invalid("psmf_marks_offset is zero but psmf_marks_size is not zero");
    } else {
        if (marks_size == 0) return invalid("psmf_marks_offset is set but psmf_marks_size is zero");
        if (header_size < u64{marks_offset} + marks_size)
            return invalid("header_size is less than psmf_marks_offset + psmf_marks_size");
    }
    if (unk_offset == 0) {
        if (unk_size != 0) return invalid("unk_offset is zero but unk_size is not zero");
    } else {
        if (unk_size == 0) return invalid("unk_offset is set but unk_size is zero");
        if (header_size < u64{unk_offset} + unk_size) return invalid("header_size is less than unk_offset + unk_size");
    }
    if (unk_offset < u64{marks_offset} + marks_size)
        return invalid("unk_offset is less than psmf_marks_offset + psmf_marks_size");

    // Sequence info.
    const u32 seq = header + pamf::kSeqInfo;
    const u32 seq_info_size = g.r32(seq);
    if (pamf::kSeqInfo + 4ull + seq_info_size > header_size) return invalid("invalid seq_info_size");
    const u64 start_pts = u64{g.r16(seq + pamf::kSeqStartHigh)} << 32 | g.r32(seq + pamf::kSeqStartLow);
    const u64 end_pts = u64{g.r16(seq + pamf::kSeqEndHigh)} << 32 | g.r32(seq + pamf::kSeqEndLow);
    if (end_pts > CODEC_TS_INVALID) return invalid("invalid end_pts");
    if (start_pts >= end_pts) return invalid("invalid start_pts");
    if (g.r8(seq + pamf::kSeqGroupingPeriodNum) != 1) return invalid("invalid grouping_period_num");

    // Grouping period.
    const u32 gp = header + pamf::kGroupingPeriod;
    const u32 grouping_period_size = g.r32(gp);
    if (pamf::kGroupingPeriod + 4ull + grouping_period_size > header_size) return invalid("invalid grouping_period_size");
    const u64 gp_start_pts = u64{g.r16(gp + pamf::kGpStartHigh)} << 32 | g.r32(gp + pamf::kGpStartLow);
    const u64 gp_end_pts = u64{g.r16(gp + pamf::kGpStartHigh)} << 32 | g.r32(gp + pamf::kGpEndLow); // LLE: start's high
    if (gp_end_pts > CODEC_TS_INVALID) return invalid("invalid grp_period_end_pts");
    if (gp_start_pts >= gp_end_pts) return invalid("invalid grp_period_start_pts");
    if (gp_start_pts != start_pts) return invalid("grp_period_start_pts not equal start_pts");
    if (g.r8(gp + pamf::kGpGroupNum) != 1) return invalid("invalid group_num");

    // Group.
    const u32 group = header + pamf::kGroup;
    const u32 group_size = g.r32(group);
    if (pamf::kGroup + 4ull + group_size > header_size) return invalid("invalid group_size");
    const u8 stream_num = g.r8(group + pamf::kGroupStreamNum);
    if (stream_num == 0) return invalid("invalid stream_num");
    if (!usable(ctx, header + pamf::kStreams, stream_num * pamf::kStreamSize, "the PAMF stream headers"))
        return error(CELL_PAMF_ERROR_INVALID_ARG);

    // Streams.
    std::array<std::bitset<16>, 6> channels_used{};
    u32 end_of_streams = 0, next_ep_table = 0; // offsets from the header (0: none yet)
    for (u8 i = 0; i < stream_num; i++) {
        const u32 st = header + pamf::kStreams + i * pamf::kStreamSize;
        u8 type = 0, ch = 0;
        if (stream_to_type_channel(g.r8(st + pamf::kStCodingType), g.r8(st + pamf::kStStreamId),
                g.r8(st + pamf::kStPrivateId), &type, &ch) != lv2::CELL_OK)
            return error(CELL_PAMF_ERROR_UNKNOWN_STREAM);
        if (type >= channels_used.size()) return error(CELL_PAMF_ERROR_UNKNOWN_STREAM); // a PSMF type (RPCS3 at32)
        if (channels_used[type].test(ch)) return invalid("invalid channel"); // a channel once per type
        channels_used[type].set(ch);

        const u32 ep_offset = g.r32(st + pamf::kStEpOffset), ep_num = g.r32(st + pamf::kStEpNum);
        if (ep_num == 0) {
            if (ep_offset != 0) return invalid("ep_num is zero but ep_offset is not zero");
        } else {
            if (ep_offset == 0) return invalid("invalid ep_offset");
            if (ep_offset + u64{ep_num} * pamf::kEpSize > header_size) return invalid("invalid ep_num");
        }
        // No entry points, or the minimum header: nothing more to check for this stream.
        if (ep_offset == 0 || (attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)) continue;
        // The entry point tables follow the streams in stream order, with no gaps.
        if (end_of_streams == 0) {
            end_of_streams = ep_offset;
            next_ep_table = end_of_streams;
        } else if (next_ep_table != ep_offset) {
            return invalid("invalid ep table address");
        }
        u64 previous_rpn = 0;
        for (u32 e = 0; e < ep_num; e++) {
            const u32 ep = header + ep_offset + e * pamf::kEpSize;
            const u64 pts = u64{g.r16(ep + 2)} << 32 | g.r32(ep + 4);
            if (pts > CODEC_TS_INVALID) return invalid("invalid ep pts");
            const u64 rpn = g.r32(ep + 8) * 0x800ull;
            if (rpn > data_size || rpn < previous_rpn) return invalid("invalid rpn_offset");
            previous_rpn = rpn;
        }
        next_ep_table += ep_num * pamf::kEpSize;
    }
    // LLE computes these in u32 and can overflow (RPCS3 keeps the +4 on both sides).
    if (static_cast<u32>(group_size + 4) > static_cast<u32>(grouping_period_size - pamf::kGpGroups + 4) ||
        static_cast<u32>(grouping_period_size + 4) > static_cast<u32>(seq_info_size - pamf::kSeqGroupingPeriods + 4))
        return invalid("size mismatch");
    // One grouping period with one group: the sequence's stream count is the group's.
    if (g.r32(seq + pamf::kSeqTotalStreams) != stream_num) return invalid("number of streams mismatch");

    // PsmfMarks (RPCS3: "probably useless since the official PAMF tools don't support PsmfMarks").
    if (end_of_streams == 0) {
        if (marks_offset != 0) end_of_streams = marks_offset;
        else if (unk_offset != 0) end_of_streams = unk_offset;
    }
    if (end_of_streams != 0 && pamf::kGroupingPeriod + 4 + grouping_period_size != end_of_streams)
        return invalid("invalid offset of ep tables or psmf marks");
    if (next_ep_table != 0 && marks_offset == 0) {
        if (unk_offset != 0 && unk_offset != next_ep_table) return invalid("invalid unk_offset");
    } else if (next_ep_table != 0 && marks_offset != next_ep_table) {
        return invalid("invalid psmf_marks_offset");
    } else if (marks_offset != 0 && !(attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)) {
        const u32 size = g.r32(header + marks_offset);
        if (size + 4u != marks_size) return invalid("invalid psmf_marks_size");
        const u16 marks_num = g.r16(header + marks_offset + 6); // LLE reads it at 6 instead of 4
        if (2u + marks_num * 0x28u != size) return invalid("invalid marks_num"); // 0x28: sizeof PsmfMark
        // LLE has more checks here, never reached because of the bug above (RPCS3).
    }
    return lv2::CELL_OK;
}

// RPCS3 pamfGetHeaderAndDataSize.
s32 header_and_data_size(PPUContext& ctx, const Guest& g, u32 header, u64 file_size, u32 header_out, u32 data_out)
{
    if (!usable(ctx, header, 0x10, "the PAMF header")) return error(CELL_PAMF_ERROR_INVALID_ARG);
    if (const s32 r = verify_magic_and_version(g, header, 0); r != lv2::CELL_OK) return r;
    const u64 header_size = g.r32(header + pamf::kHeaderSize) * 0x800ull;
    const u64 data_size = g.r32(header + pamf::kDataSize) * 0x800ull;
    if (header_size == 0 || (file_size != 0 && header_size + data_size != file_size))
        return error(CELL_PAMF_ERROR_INVALID_PAMF);
    if (header_out) g.w64(header_out, header_size);
    if (data_out) g.w64(data_out, data_size);
    return lv2::CELL_OK;
}

// ---- The reader's state, in the game's CellPamfReader.
struct Reader {
    const Guest& g;
    u32 self;
    bool psmf() const { return g.r16(self + reader::kIsPsmf) != 0; }
    u32 header() const { return g.r32(self + reader::kHeader); }
    u32 sequence_info() const { return g.r32(self + reader::kSequenceInfo); }
    u32 group() const { return g.r32(self + reader::kCurrentGroup); }
    u32 stream() const { return g.r32(self + reader::kCurrentStream); }
    u8 stream_num() const { return g.r8(group() + pamf::kGroupStreamNum); } // the same offset in PSMF's group
    u32 streams() const { return group() + (psmf() ? psmf::kGroupStreams : pamf::kGroupStreams); }
    u32 stream_size() const { return psmf() ? psmf::kStreamSize : pamf::kStreamSize; }
    u32 ep_size() const { return psmf() ? psmf::kEpSize : pamf::kEpSize; }
    u32 ep_num() const { return g.r32(stream() + (psmf() ? psmf::kStEpNum : pamf::kStEpNum)); }
    u32 ep_table() const { return header() + g.r32(stream() + (psmf() ? psmf::kStEpOffset : pamf::kStEpOffset)); }
    // The type and channel of a stream header of the current group.
    s32 type_channel(u32 st, u8* type, u8* ch) const
    {
        if (psmf())
            return stream_to_type_channel(PAMF_STREAM_CODING_TYPE_PSMF, g.r8(st + psmf::kStStreamId),
                g.r8(st + psmf::kStPrivateId), type, ch);
        return stream_to_type_channel(g.r8(st + pamf::kStCodingType), g.r8(st + pamf::kStStreamId),
            g.r8(st + pamf::kStPrivateId), type, ch);
    }
    // An entry point's time stamp.
    u64 ep_pts(u32 ep) const
    {
        return psmf() ? (u64{g.r16(ep) & 1u} << 32 | g.r32(ep + 2)) : (u64{g.r16(ep + 2)} << 32 | g.r32(ep + 4));
    }
    u32 ep_rpn(u32 ep) const { return g.r32(ep + (psmf() ? 6 : 8)); }
};

// A reader the game passes: its 128 bytes, and the header it was initialised over.
bool reader_ok(PPUContext& ctx, u32 self, std::string_view call)
{
    if (!usable(ctx, self, reader::kSize, std::format("{}: the reader", call))) return false;
    const Guest g{guest_base(ctx)};
    const Reader r{g, self};
    return usable(ctx, r.group(), pamf::kGroupStreams, std::format("{}: the reader's group", call)) &&
           usable(ctx, r.stream(), r.stream_size(), std::format("{}: the reader's stream", call));
}

} // namespace

s32 cellPamfGetStreamOffsetAndSize(PPUContext& ctx, u32 pAddr, u64 fileSize, u32 pOffset, u32 pSize)
{
    const std::string call = std::format("cellPamfGetStreamOffsetAndSize({:#x}, {:#x}, {:#x}, {:#x})", pAddr, fileSize,
        pOffset, pSize);
    if ((pOffset && !usable(ctx, pOffset, 8, call)) || (pSize && !usable(ctx, pSize, 8, call)))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    return finish(header_and_data_size(ctx, Guest{guest_base(ctx)}, pAddr, fileSize, pOffset, pSize), call);
}

s32 cellPamfGetHeaderSize(PPUContext& ctx, u32 pAddr, u64 fileSize, u32 pSize)
{
    return cellPamfGetStreamOffsetAndSize(ctx, pAddr, fileSize, pSize, 0);
}

s32 cellPamfGetHeaderSize2(PPUContext& ctx, u32 pAddr, u64 fileSize, u32 attribute, u32 pSize)
{
    const std::string call = std::format("cellPamfGetHeaderSize2({:#x}, {:#x}, {:#x}, {:#x})", pAddr, fileSize, attribute, pSize);
    const Guest g{guest_base(ctx)};
    if (pSize && !usable(ctx, pSize, 8, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    u64 header_size = 0;
    {
        if (!usable(ctx, pAddr, pamf::kSeqInfo + 4, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        if (const s32 r = verify_magic_and_version(g, pAddr, 0); r != lv2::CELL_OK) return finish(r, call);
        header_size = g.r32(pAddr + pamf::kHeaderSize) * 0x800ull;
        const u64 data_size = g.r32(pAddr + pamf::kDataSize) * 0x800ull;
        if (header_size == 0 || (fileSize != 0 && header_size + data_size != fileSize))
            return finish(error(CELL_PAMF_ERROR_INVALID_PAMF), call);
    }
    if (attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER) {
        // The header without its entry point tables.
        const u64 min_header_size = pamf::kSeqInfo + 4ull + g.r32(pAddr + pamf::kSeqInfo);
        if (min_header_size > header_size) return finish(error(CELL_PAMF_ERROR_INVALID_PAMF), call);
        header_size = min_header_size;
    }
    if (pSize) g.w64(pSize, header_size);
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfVerify(PPUContext& ctx, u32 pAddr, u64 fileSize)
{
    const std::string call = std::format("cellPamfVerify({:#x}, {:#x})", pAddr, fileSize);
    return finish(verify(ctx, Guest{guest_base(ctx)}, pAddr, fileSize, 0, CELL_PAMF_ATTRIBUTE_VERIFY_ON), call);
}

s32 cellPamfReaderSetStreamWithIndex(PPUContext& ctx, u32 pSelf, u8 streamIndex);

s32 cellPamfReaderInitialize(PPUContext& ctx, u32 pSelf, u32 pAddr, u64 fileSize, u32 attribute)
{
    const std::string call = std::format("cellPamfReaderInitialize({:#x}, {:#x}, {:#x}, {:#x})", pSelf, pAddr, fileSize, attribute);
    if (!usable(ctx, pSelf, reader::kSize, call) || !usable(ctx, pAddr, pamf::kStreams, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    std::memset(g.b + pSelf, 0, reader::kSize);
    g.w32(pSelf + reader::kAttribute, attribute);
    if (attribute & CELL_PAMF_ATTRIBUTE_VERIFY_ON)
        if (const s32 r = verify(ctx, g, pAddr, fileSize, pSelf, attribute); r != lv2::CELL_OK) return finish(r, call);
    g.w32(pSelf + reader::kHeader, pAddr);
    g.w32(pSelf + reader::kSequenceInfo, pAddr + pamf::kSeqInfo);
    g.w32(pSelf + reader::kGroupingPeriodIndex, 0xFFFFFFFF);
    g.w32(pSelf + reader::kGroupIndex, 0xFFFFFFFF);
    g.w32(pSelf + reader::kStreamIndex, 0xFFFFFFFF);
    if (g.r8(pAddr + pamf::kSeqInfo + pamf::kSeqGroupingPeriodNum) == 0)
        return finish(error(CELL_PAMF_ERROR_INVALID_PAMF), call);
    g.w32(pSelf + reader::kGroupingPeriodIndex, 0);
    g.w32(pSelf + reader::kCurrentGroupingPeriod, pAddr + pamf::kGroupingPeriod);
    if (g.r8(pAddr + pamf::kGroupingPeriod + pamf::kGpGroupNum) != 0) {
        g.w32(pSelf + reader::kGroupIndex, 0);
        g.w32(pSelf + reader::kCurrentGroup, pAddr + pamf::kGroup);
        cellPamfReaderSetStreamWithIndex(ctx, pSelf, 0);
    }
    return finish(lv2::CELL_OK, call);
}

namespace {

s32 presentation_time(PPUContext& ctx, u32 pSelf, u32 pTimeStamp, bool end, const std::string& call)
{
    // Always CELL_OK on LLE.
    if (!usable(ctx, pSelf, reader::kSize, call) || !usable(ctx, pTimeStamp, 8, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    const u32 seq = r.sequence_info();
    if (!usable(ctx, seq, 0x20, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (r.psmf()) {
        g.w32(pTimeStamp, g.r16(seq + (end ? psmf::kSeqEndHigh : psmf::kSeqStartHigh)));
        g.w32(pTimeStamp + 4, g.r32(seq + (end ? psmf::kSeqEndLow : psmf::kSeqStartLow)));
    } else {
        g.w32(pTimeStamp, g.r16(seq + (end ? pamf::kSeqEndHigh : pamf::kSeqStartHigh)));
        g.w32(pTimeStamp + 4, g.r32(seq + (end ? pamf::kSeqEndLow : pamf::kSeqStartLow)));
    }
    return finish(lv2::CELL_OK, call);
}

} // namespace

s32 cellPamfReaderGetPresentationStartTime(PPUContext& ctx, u32 pSelf, u32 pTimeStamp)
{
    return presentation_time(ctx, pSelf, pTimeStamp, false,
        std::format("cellPamfReaderGetPresentationStartTime({:#x}, {:#x})", pSelf, pTimeStamp));
}

s32 cellPamfReaderGetPresentationEndTime(PPUContext& ctx, u32 pSelf, u32 pTimeStamp)
{
    return presentation_time(ctx, pSelf, pTimeStamp, true,
        std::format("cellPamfReaderGetPresentationEndTime({:#x}, {:#x})", pSelf, pTimeStamp));
}

u32 cellPamfReaderGetMuxRateBound(PPUContext& ctx, u32 pSelf)
{
    const std::string call = std::format("cellPamfReaderGetMuxRateBound({:#x})", pSelf);
    if (!usable(ctx, pSelf, reader::kSize, call)) return 0;
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    if (!usable(ctx, r.sequence_info(), 0x20, call)) return 0;
    return g.r32(r.sequence_info() + (r.psmf() ? psmf::kSeqMuxRate : pamf::kSeqMuxRate)) & 0x003fffff;
}

u32 cellPamfReaderGetNumberOfStreams(PPUContext& ctx, u32 pSelf)
{
    const std::string call = std::format("cellPamfReaderGetNumberOfStreams({:#x})", pSelf);
    if (!reader_ok(ctx, pSelf, call)) return 0;
    const Guest g{guest_base(ctx)};
    return Reader{g, pSelf}.stream_num();
}

u32 cellPamfReaderGetNumberOfSpecificStreams(PPUContext& ctx, u32 pSelf, u8 streamType)
{
    const std::string call = std::format("cellPamfReaderGetNumberOfSpecificStreams({:#x}, {})", pSelf, streamType);
    if (!reader_ok(ctx, pSelf, call)) return 0;
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    const u8 n = r.stream_num();
    if (!usable(ctx, r.streams(), n * r.stream_size(), call)) return 0;
    u8 found = 0;
    for (u8 i = 0; i < n; i++) {
        u8 type = 0;
        if (r.type_channel(r.streams() + i * r.stream_size(), &type, nullptr) == lv2::CELL_OK)
            found = static_cast<u8>(found + same_stream_type(type, streamType));
    }
    return found;
}

s32 cellPamfReaderSetStreamWithIndex(PPUContext& ctx, u32 pSelf, u8 streamIndex)
{
    const std::string call = std::format("cellPamfReaderSetStreamWithIndex({:#x}, {})", pSelf, streamIndex);
    if (!usable(ctx, pSelf, reader::kSize, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    if (!usable(ctx, r.group(), pamf::kGroupStreams, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (streamIndex >= r.stream_num()) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    g.w32(pSelf + reader::kStreamIndex, streamIndex);
    g.w32(pSelf + reader::kCurrentStream, r.streams() + streamIndex * r.stream_size());
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfReaderSetStreamWithTypeAndChannel(PPUContext& ctx, u32 pSelf, u8 streamType, u8 ch)
{
    // "This function is broken on LLE" (RPCS3): kept as it is, as games met it so.
    const std::string call = std::format("cellPamfReaderSetStreamWithTypeAndChannel({:#x}, {}, {})", pSelf, streamType, ch);
    if (!usable(ctx, pSelf, reader::kSize, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    u8 coding_type = 0, stream_id = 0, private_stream_id = 0;
    if (!type_channel_to_stream(streamType, ch, &coding_type, &stream_id, &private_stream_id))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (!usable(ctx, r.group(), pamf::kGroupStreams, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const u8 stream_num = r.stream_num();
    u32 i = 0;
    for (; i < stream_num; i++) {
        // LLE steps 12 stream headers at a time for PSMF, 0x10 for PAMF, instead of one.
        const u32 st = r.streams() + i * (r.psmf() ? 12 : 0x10) * r.stream_size();
        if (!guest_readable(thread_of(ctx), st, r.stream_size())) continue; // LLE would read whatever is there
        if (r.psmf()) {
            if (coding_type == PAMF_STREAM_CODING_TYPE_PSMF && g.r8(st + psmf::kStStreamId) == stream_id &&
                g.r8(st + psmf::kStPrivateId) == private_stream_id)
                break;
        } else if (g.r8(st + pamf::kStCodingType) == coding_type && g.r8(st + pamf::kStStreamId) == stream_id &&
                   g.r8(st + pamf::kStPrivateId) == private_stream_id) {
            break;
        }
    }
    if (i == stream_num) i = CELL_PAMF_ERROR_STREAM_NOT_FOUND; // LLE writes the error code as the index
    if (g.r32(pSelf + reader::kStreamIndex) != i) {
        g.w32(pSelf + reader::kCurrentStream, r.streams()); // LLE always sets the first stream
        g.w32(pSelf + reader::kStreamIndex, i);
    }
    if (i == CELL_PAMF_ERROR_STREAM_NOT_FOUND) return finish(error(CELL_PAMF_ERROR_STREAM_NOT_FOUND), call);
    return finish(static_cast<s32>(i), call);
}

s32 cellPamfReaderSetStreamWithTypeAndIndex(PPUContext& ctx, u32 pSelf, u8 streamType, u8 streamIndex)
{
    const std::string call = std::format("cellPamfReaderSetStreamWithTypeAndIndex({:#x}, {}, {})", pSelf, streamType, streamIndex);
    if (!usable(ctx, pSelf, reader::kSize, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    if (!usable(ctx, r.group(), pamf::kGroupStreams, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const u8 stream_num = r.stream_num();
    if (streamIndex >= stream_num) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (!usable(ctx, r.streams(), stream_num * r.stream_size(), call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    u32 found = 0;
    for (u8 i = 0; i < stream_num; i++) {
        u8 type = 0;
        if (r.type_channel(r.streams() + i * r.stream_size(), &type, nullptr) != lv2::CELL_OK) continue;
        if (r.psmf()) {
            found += type == streamType;
            if (found > streamIndex) {
                g.w32(pSelf + reader::kStreamIndex, streamIndex); // LLE: the index among streams of that type
                g.w32(pSelf + reader::kCurrentStream, r.streams()); // LLE always sets the first stream
                return finish(i, call);
            }
        } else {
            found += same_stream_type(type, streamType);
            if (found > streamIndex) {
                g.w32(pSelf + reader::kStreamIndex, i);
                g.w32(pSelf + reader::kCurrentStream, r.streams() + i * r.stream_size());
                return finish(i, call);
            }
        }
    }
    return finish(error(CELL_PAMF_ERROR_STREAM_NOT_FOUND), call);
}

s32 cellPamfStreamTypeToEsFilterId(PPUContext& ctx, u8 type, u8 ch, u32 pEsFilterId)
{
    const std::string call = std::format("cellPamfStreamTypeToEsFilterId({}, {}, {:#x})", type, ch, pEsFilterId);
    if (!pEsFilterId) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (!usable(ctx, pEsFilterId, 16, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    u8 stream_id = 0, private_stream_id = 0;
    if (!type_channel_to_stream(type, ch, nullptr, &stream_id, &private_stream_id))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    g.w32(pEsFilterId + 0, stream_id);
    g.w32(pEsFilterId + 4, private_stream_id);
    g.w32(pEsFilterId + 8, type == CELL_PAMF_STREAM_TYPE_AVC);
    g.w32(pEsFilterId + 12, 0);
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfReaderGetStreamIndex(PPUContext& ctx, u32 pSelf)
{
    const std::string call = std::format("cellPamfReaderGetStreamIndex({:#x})", pSelf);
    if (!usable(ctx, pSelf, reader::kSize, call)) return error(CELL_PAMF_ERROR_INVALID_ARG);
    return static_cast<s32>(ps_read32(guest_base(ctx), pSelf + reader::kStreamIndex));
}

s32 cellPamfReaderGetStreamTypeAndChannel(PPUContext& ctx, u32 pSelf, u32 pType, u32 pCh)
{
    const std::string call = std::format("cellPamfReaderGetStreamTypeAndChannel({:#x}, {:#x}, {:#x})", pSelf, pType, pCh);
    if (!reader_ok(ctx, pSelf, call) || (pType && !usable(ctx, pType, 1, call)) || (pCh && !usable(ctx, pCh, 1, call)))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    u8 type = 0, ch = 0;
    const s32 r = Reader{g, pSelf}.type_channel(Reader{g, pSelf}.stream(), &type, &ch);
    if (r == lv2::CELL_OK) {
        if (pType) g.w8(pType, type);
        if (pCh) g.w8(pCh, ch);
    }
    return finish(r, call);
}

s32 cellPamfReaderGetEsFilterId(PPUContext& ctx, u32 pSelf, u32 pEsFilterId)
{
    // Always CELL_OK on LLE.
    const std::string call = std::format("cellPamfReaderGetEsFilterId({:#x}, {:#x})", pSelf, pEsFilterId);
    if (!reader_ok(ctx, pSelf, call) || !usable(ctx, pEsFilterId, 16, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    const u32 st = r.stream();
    if (r.psmf()) {
        g.w32(pEsFilterId + 0, g.r8(st + psmf::kStStreamId));
        g.w32(pEsFilterId + 4, g.r8(st + psmf::kStPrivateId));
        g.w32(pEsFilterId + 8, 0);
    } else {
        g.w32(pEsFilterId + 0, g.r8(st + pamf::kStStreamId));
        g.w32(pEsFilterId + 4, g.r8(st + pamf::kStPrivateId));
        g.w32(pEsFilterId + 8, g.r8(st + pamf::kStCodingType) == PAMF_STREAM_CODING_TYPE_AVC);
    }
    g.w32(pEsFilterId + 12, 0);
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfReaderGetStreamInfo(PPUContext& ctx, u32 pSelf, u32 pInfo, u32 size)
{
    const std::string call = std::format("cellPamfReaderGetStreamInfo({:#x}, {:#x}, {})", pSelf, pInfo, size);
    if (!reader_ok(ctx, pSelf, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    const u32 st = r.stream();
    u8 type = 0;
    if (r.type_channel(st, &type, nullptr) != lv2::CELL_OK) return finish(error(CELL_PAMF_ERROR_INVALID_PAMF), call);
    const auto out = [&](u32 bytes) { return size >= bytes && usable(ctx, pInfo, bytes, call); };
    const u32 in = st + pamf::kStInfo; // PAMF's codec information
    switch (type) {
    case CELL_PAMF_STREAM_TYPE_AVC: {
        if (!out(0x20)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        const u8 x2 = g.r8(in + 2), aspect = g.r8(in + 3), x14 = g.r8(in + 20), x18 = g.r8(in + 24);
        const bool signal_info = (x2 & 0x40) != 0;
        g.w8(pInfo + 0x00, g.r8(in + 0));             // profileIdc
        g.w8(pInfo + 0x01, g.r8(in + 1));             // levelIdc
        g.w8(pInfo + 0x02, (x2 & 0x80) >> 7);         // frameMbsOnlyFlag
        g.w8(pInfo + 0x03, signal_info);              // videoSignalInfoFlag
        g.w8(pInfo + 0x04, static_cast<u8>((x2 & 0x0f) - 1)); // frameRateInfo
        g.w8(pInfo + 0x05, aspect);                   // aspectRatioIdc
        g.w16(pInfo + 0x06, aspect == 0xff ? g.r16(in + 4) : 0); // sarWidth
        g.w16(pInfo + 0x08, aspect == 0xff ? g.r16(in + 6) : 0); // sarHeight
        g.w16(pInfo + 0x0A, g.r8(in + 9) * 16u);      // horizontalSize
        g.w16(pInfo + 0x0C, g.r8(in + 11) * 16u);     // verticalSize
        g.w16(pInfo + 0x0E, g.r16(in + 12));          // frameCrop left, right, top, bottom
        g.w16(pInfo + 0x10, g.r16(in + 14));
        g.w16(pInfo + 0x12, g.r16(in + 16));
        g.w16(pInfo + 0x14, g.r16(in + 18));
        g.w8(pInfo + 0x16, signal_info ? x14 >> 5 : 0);          // videoFormat
        g.w8(pInfo + 0x17, signal_info ? (x14 & 0x10) >> 4 : 0); // videoFullRangeFlag
        g.w8(pInfo + 0x18, signal_info ? g.r8(in + 21) : 0);     // colourPrimaries
        g.w8(pInfo + 0x19, signal_info ? g.r8(in + 22) : 0);     // transferCharacteristics
        g.w8(pInfo + 0x1A, signal_info ? g.r8(in + 23) : 0);     // matrixCoefficients
        g.w8(pInfo + 0x1B, (x18 & 0x80) >> 7);        // entropyCodingModeFlag
        g.w8(pInfo + 0x1C, (x18 & 0x40) >> 6);        // deblockingFilterFlag
        g.w8(pInfo + 0x1D, (x18 & 0x30) >> 4);        // minNumSlicePerPictureIdc
        g.w8(pInfo + 0x1E, x18 & 0x03);               // nfwIdc
        g.w8(pInfo + 0x1F, g.r8(in + 25));            // maxMeanBitrate
        break;
    }
    case CELL_PAMF_STREAM_TYPE_M2V: {
        if (!out(0x18)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        const u8 x0 = g.r8(in + 0), x2 = g.r8(in + 2), aspect = g.r8(in + 3), x14 = g.r8(in + 20);
        const bool signal_info = (x2 & 0x40) != 0;
        g.w8(pInfo + 0x00, x0 == 0x44 ? 3 : x0 == 0x48 ? 1 : CELL_PAMF_M2V_UNKNOWN); // profileAndLevelIndication
        g.w8(pInfo + 0x01, (x2 & 0x80) >> 7);         // progressiveSequence
        g.w8(pInfo + 0x02, signal_info);              // videoSignalInfoFlag
        g.w8(pInfo + 0x03, x2 & 0x0f);                // frameRateInfo
        g.w8(pInfo + 0x04, aspect);                   // aspectRatioIdc
        g.w16(pInfo + 0x06, aspect == 0xff ? g.r16(in + 4) : 0);
        g.w16(pInfo + 0x08, aspect == 0xff ? g.r16(in + 6) : 0);
        g.w16(pInfo + 0x0A, g.r8(in + 9) * 16u);
        g.w16(pInfo + 0x0C, g.r8(in + 11) * 16u);
        g.w16(pInfo + 0x0E, g.r16(in + 12));          // horizontalSizeValue
        g.w16(pInfo + 0x10, g.r16(in + 14));          // verticalSizeValue
        g.w8(pInfo + 0x12, signal_info ? x14 >> 5 : 0);
        g.w8(pInfo + 0x13, signal_info ? (x14 & 0x10) >> 4 : 0);
        g.w8(pInfo + 0x14, signal_info ? g.r8(in + 21) : 0);
        g.w8(pInfo + 0x15, signal_info ? g.r8(in + 22) : 0);
        g.w8(pInfo + 0x16, signal_info ? g.r8(in + 23) : 0);
        break;
    }
    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS:
    case CELL_PAMF_STREAM_TYPE_AC3: // CellPamfAtrac3plusInfo, CellPamfAc3Info: frequency, channels
        if (!out(8)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        g.w32(pInfo + 0, g.r8(in + 3) & 0xfu);
        g.w8(pInfo + 4, g.r8(in + 2) & 0xf);
        break;
    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM: // CellPamfLpcmInfo: frequency, channels, bits per sample
        if (!out(8)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        g.w32(pInfo + 0, g.r8(in + 3) & 0xfu);
        g.w8(pInfo + 4, g.r8(in + 2) & 0xf);
        g.w16(pInfo + 6, g.r8(in + 4) >> 6);
        break;
    case CELL_PAMF_STREAM_TYPE_USER_DATA:
    case CELL_PAMF_STREAM_TYPE_PSMF_USER_DATA:
        PS_LOG_ERROR(kChan, "cellPamfReaderGetStreamInfo(): invalid type CELL_PAMF_STREAM_TYPE_USER_DATA");
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    case CELL_PAMF_STREAM_TYPE_PSMF_AVC: // two u16: the picture's size
        if (!out(4)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        g.w16(pInfo + 0, g.r8(st + psmf::kStInfo) * 0x10u);
        g.w16(pInfo + 2, g.r8(st + psmf::kStInfo + 1) * 0x10u);
        break;
    case CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS:
    case CELL_PAMF_STREAM_TYPE_PSMF_LPCM: // two u8: channel configuration, frequency
        if (!out(2)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        g.w8(pInfo + 0, g.r8(st + psmf::kStInfo + 2));
        g.w8(pInfo + 1, g.r8(st + psmf::kStInfo + 3) & 0x0f);
        break;
    default:
        PS_LOG_ERROR(kChan, "cellPamfReaderGetStreamInfo(): invalid type {}", type);
        return finish(error(CELL_PAMF_ERROR_INVALID_PAMF), call);
    }
    return finish(lv2::CELL_OK, call);
}

u32 cellPamfReaderGetNumberOfEp(PPUContext& ctx, u32 pSelf)
{
    const std::string call = std::format("cellPamfReaderGetNumberOfEp({:#x})", pSelf);
    if (!reader_ok(ctx, pSelf, call)) return 0;
    const Guest g{guest_base(ctx)};
    return Reader{g, pSelf}.ep_num();
}

namespace {

// The current stream's entry point table, when the reader may read it: its address, or 0 with *err set.
u32 ep_table(PPUContext& ctx, const Reader& r, u32 count, const std::string& call, s32* err)
{
    const u32 table = r.ep_table();
    if (!usable(ctx, table, count * r.ep_size(), call)) {
        *err = error(CELL_PAMF_ERROR_INVALID_ARG);
        return 0;
    }
    return table;
}

// The last entry point at or before `ts`, else the first (RPCS3: "always output eps[0] if no other suitable ep is
// found").
u32 ep_at_time(const Reader& r, u32 table, u32 ep_num, u64 ts)
{
    u32 i = ep_num - 1;
    for (; i >= 1; i--)
        if (r.ep_pts(table + i * r.ep_size()) <= ts) break;
    return i;
}

} // namespace

s32 cellPamfReaderGetEpIteratorWithIndex(PPUContext& ctx, u32 pSelf, u32 epIndex, u32 pIt)
{
    const std::string call = std::format("cellPamfReaderGetEpIteratorWithIndex({:#x}, {}, {:#x})", pSelf, epIndex, pIt);
    if (!reader_ok(ctx, pSelf, call) || !usable(ctx, pIt, iter::kSize, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    const u32 ep_num = r.ep_num();
    if (epIndex >= ep_num) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (g.r32(pSelf + reader::kAttribute) & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)
        return finish(error(CELL_PAMF_ERROR_NOT_AVAILABLE), call);
    g.w8(pIt + iter::kIsPamf, !r.psmf());
    g.w32(pIt + iter::kIndex, epIndex);
    g.w32(pIt + iter::kNum, ep_num);
    g.w32(pIt + iter::kCur, r.ep_table() + epIndex * r.ep_size());
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfReaderGetEpIteratorWithTimeStamp(PPUContext& ctx, u32 pSelf, u32 pTimeStamp, u32 pIt)
{
    const std::string call = std::format("cellPamfReaderGetEpIteratorWithTimeStamp({:#x}, {:#x}, {:#x})", pSelf, pTimeStamp, pIt);
    if (!reader_ok(ctx, pSelf, call) || !usable(ctx, pTimeStamp, 8, call) || !usable(ctx, pIt, iter::kSize, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    if (g.r32(pSelf + reader::kAttribute) & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)
        return finish(error(CELL_PAMF_ERROR_NOT_AVAILABLE), call);
    const u32 ep_num = r.ep_num();
    g.w32(pIt + iter::kNum, ep_num);
    g.w8(pIt + iter::kIsPamf, !r.psmf());
    if (ep_num == 0) return finish(error(CELL_PAMF_ERROR_EP_NOT_FOUND), call);
    s32 err = 0;
    const u32 table = ep_table(ctx, r, ep_num, call, &err);
    if (!table) return finish(err, call);
    const u32 i = ep_at_time(r, table, ep_num, g.r64(pTimeStamp));
    g.w32(pIt + iter::kCur, table + i * r.ep_size());
    g.w32(pIt + iter::kIndex, i);
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfEpIteratorGetEp(PPUContext& ctx, u32 pIt, u32 pEp)
{
    // Always CELL_OK on LLE.
    const std::string call = std::format("cellPamfEpIteratorGetEp({:#x}, {:#x})", pIt, pEp);
    if (!usable(ctx, pIt, iter::kSize, call) || !usable(ctx, pEp, kEpSize, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const bool is_pamf = g.r8(pIt + iter::kIsPamf) != 0;
    const u32 cur = g.r32(pIt + iter::kCur);
    if (!usable(ctx, cur, is_pamf ? pamf::kEpSize : psmf::kEpSize, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    ep_unpack(g, is_pamf, cur, pEp);
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfEpIteratorMove(PPUContext& ctx, u32 pIt, s32 steps, u32 pEp)
{
    const std::string call = std::format("cellPamfEpIteratorMove({:#x}, {}, {:#x})", pIt, steps, pEp);
    if (!usable(ctx, pIt, iter::kSize, call) || (pEp && !usable(ctx, pEp, kEpSize, call)))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const u32 index = g.r32(pIt + iter::kIndex), num = g.r32(pIt + iter::kNum);
    u32 new_index = index + static_cast<u32>(steps);
    if (static_cast<s32>(new_index) < 0) {
        steps = -static_cast<s32>(index);
        new_index = 0;
    } else if (new_index >= num) {
        steps = static_cast<s32>(num - index - 1);
        new_index = index + static_cast<u32>(steps);
    }
    g.w32(pIt + iter::kIndex, new_index);
    const bool is_pamf = g.r8(pIt + iter::kIsPamf) != 0;
    const u32 cur = g.r32(pIt + iter::kCur) + static_cast<u32>(steps) * (is_pamf ? pamf::kEpSize : psmf::kEpSize);
    g.w32(pIt + iter::kCur, cur);
    if (pEp) {
        if (!usable(ctx, cur, is_pamf ? pamf::kEpSize : psmf::kEpSize, call)) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
        ep_unpack(g, is_pamf, cur, pEp);
    }
    PS_LOG_DEBUG(kChan, "{}: {} steps", call, steps); // a count, negative going back: not an error
    return steps;
}

s32 cellPamfReaderGetEpWithTimeStamp(PPUContext& ctx, u32 pSelf, u32 pTimeStamp, u32 pEp, u32 unk)
{
    // "This function is broken on LLE" (RPCS3): it finds the entry point and writes only nextRpnOffset. In bytes, as
    // Sony's libpamf writes it (D-193: compared with the user's firmware); RPCS3 leaves it in 2048-byte units.
    const std::string call = std::format("cellPamfReaderGetEpWithTimeStamp({:#x}, {:#x}, {:#x}, {:#x})", pSelf, pTimeStamp, pEp, unk);
    if (!reader_ok(ctx, pSelf, call) || !usable(ctx, pTimeStamp, 8, call) || !usable(ctx, pEp, kEpUnkSize, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    if (g.r32(pSelf + reader::kAttribute) & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)
        return finish(error(CELL_PAMF_ERROR_NOT_AVAILABLE), call);
    const u32 ep_num = r.ep_num();
    u64 next_rpn = g.r64(pSelf + reader::kDataSize);
    if (ep_num == 0) return finish(error(CELL_PAMF_ERROR_EP_NOT_FOUND), call);
    s32 err = 0;
    const u32 table = ep_table(ctx, r, ep_num, call, &err);
    if (!table) return finish(err, call);
    const u32 i = ep_at_time(r, table, ep_num, g.r64(pTimeStamp));
    if (i < ep_num - 1) next_rpn = r.ep_rpn(table + (i + 1) * r.ep_size()) * 0x800ull;
    if (unk == kEpUnkSize) g.w64(pEp + kEpSize, next_rpn);
    return finish(lv2::CELL_OK, call);
}

s32 cellPamfReaderGetEpWithIndex(PPUContext& ctx, u32 pSelf, u32 epIndex, u32 pEp, u32 unk)
{
    const std::string call = std::format("cellPamfReaderGetEpWithIndex({:#x}, {}, {:#x}, {:#x})", pSelf, epIndex, pEp, unk);
    if (!reader_ok(ctx, pSelf, call) || !usable(ctx, pEp, kEpUnkSize, call))
        return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    const Guest g{guest_base(ctx)};
    const Reader r{g, pSelf};
    const u32 ep_num = r.ep_num();
    u64 next_rpn = g.r64(pSelf + reader::kDataSize);
    if (epIndex >= ep_num) return finish(error(CELL_PAMF_ERROR_INVALID_ARG), call);
    if (g.r32(pSelf + reader::kAttribute) & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)
        return finish(error(CELL_PAMF_ERROR_NOT_AVAILABLE), call);
    s32 err = 0;
    const u32 table = ep_table(ctx, r, ep_num, call, &err);
    if (!table) return finish(err, call);
    const u32 ep = table + epIndex * r.ep_size();
    ep_unpack(g, !r.psmf(), ep, pEp);
    if (epIndex < ep_num - 1) next_rpn = r.ep_rpn(ep + r.ep_size()) * 0x800ull;
    if (unk == kEpUnkSize) g.w64(pEp + kEpSize, next_rpn);
    return finish(lv2::CELL_OK, call);
}

void register_cellPamf(Registry& r)
{
    r.add("cellPamf", {
        PS_HLE_FUNC(0xCA8181C1, cellPamfGetHeaderSize),
        PS_HLE_FUNC(0x90FC9A59, cellPamfGetHeaderSize2),
        PS_HLE_FUNC(0x44F5C9E3, cellPamfGetStreamOffsetAndSize),
        PS_HLE_FUNC(0xD1A40EF4, cellPamfVerify),
        PS_HLE_FUNC(0xB8436EE5, cellPamfReaderInitialize),
        PS_HLE_FUNC(0x4DE501B1, cellPamfReaderGetPresentationStartTime),
        PS_HLE_FUNC(0xF61609D6, cellPamfReaderGetPresentationEndTime),
        PS_HLE_FUNC(0xDB70296C, cellPamfReaderGetMuxRateBound),
        PS_HLE_FUNC(0x37F723F7, cellPamfReaderGetNumberOfStreams),
        PS_HLE_FUNC(0xD0230671, cellPamfReaderGetNumberOfSpecificStreams),
        PS_HLE_FUNC(0x461534B4, cellPamfReaderSetStreamWithIndex),
        PS_HLE_FUNC(0x03FD2CAA, cellPamfReaderSetStreamWithTypeAndChannel),
        PS_HLE_FUNC(0x28B4E2C1, cellPamfReaderSetStreamWithTypeAndIndex),
        PS_HLE_FUNC(0x01067E22, cellPamfStreamTypeToEsFilterId),
        PS_HLE_FUNC(0x041CC708, cellPamfReaderGetStreamIndex),
        PS_HLE_FUNC(0x9AB20793, cellPamfReaderGetStreamTypeAndChannel),
        PS_HLE_FUNC(0x71DF326A, cellPamfReaderGetEsFilterId),
        PS_HLE_FUNC(0x67FD273B, cellPamfReaderGetStreamInfo),
        PS_HLE_FUNC(0xD9EA3457, cellPamfReaderGetNumberOfEp),
        PS_HLE_FUNC(0xE8586EC6, cellPamfReaderGetEpIteratorWithIndex),
        PS_HLE_FUNC(0x439FBA17, cellPamfReaderGetEpIteratorWithTimeStamp),
        PS_HLE_FUNC(0x1ABEB9D6, cellPamfEpIteratorGetEp),
        PS_HLE_FUNC(0x50B83205, cellPamfEpIteratorMove),
        PS_HLE_FUNC(0x7AF9E578, cellPamfReaderGetEpWithTimeStamp),
        PS_HLE_FUNC(0xFBE551D3, cellPamfReaderGetEpWithIndex),
    });
}

} // namespace pspocket::hle
