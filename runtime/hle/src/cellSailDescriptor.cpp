// cellSail's descriptors (D-562, part 3a): what the game reads and sets on a descriptor without the player's thread, as
// libsail (fw 4.92) has it. A descriptor (made by the player) holds +0 the stream type (0 PAMF, 1 MP4, 2 AVI), +0x10 the
// URI, +0x14 the media info, +0x18 flags (bit 0 set by SetParameter), +0x1C the player, +0x24 its source, +0x28/+0x2C/
// +0x30 the audio, video and user elementary stream tables (0x90, 0x78 and 0x4C bytes each, a state byte first), +0x38
// its state byte (0 created, 1 open, 3 and 5 others open), +0x39 auto selection off, +0x40 the stream's reader (PAMF
// reader, or the MP4/AVI library's movie), +0xD0 the stream's size. The capabilities come from the source's
// GetCapabilities callback (PAMF) or are fixed (MP4 0xF0, AVI 0x70 or 0xF0).

#include <cstring>

#include "cellSail.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace sail;

namespace ds {
constexpr u32 kType = 0, kUri = 0x10, kMediaInfo = 0x14, kFlags = 0x18, kPlayer = 0x1C, kSource = 0x24, kAudio = 0x28,
              kVideo = 0x2C, kUser = 0x30, kState = 0x38, kNoAuto = 0x39, kSize = 0xD0;
constexpr u32 kAudioStride = 0x90, kVideoStride = 0x78, kUserStride = 0x4C;
} // namespace ds

u32 sdk_version_of(PPUContext& ctx) { return thread_of(ctx).kernel.config().sdk_version; }

// The source's GetCapabilities(arg, stream type, media info, URI, u64* capabilities) (CellSailSourceFuncs[12]).
s32 source_capabilities(PPUContext& ctx, u32 source, u32 type, u32 media_info, u32 uri, u32 out)
{
    return static_cast<s32>(call_guest(ctx, r32(ctx, source + 8 + 12 * 4), {r32(ctx, source + 4), type, media_info, uri, out}));
}

u32 cellSailDescriptorGetStreamType(PPUContext& ctx, u32 d) { return r32(ctx, d + ds::kType); }
u32 cellSailDescriptorGetUri(PPUContext& ctx, u32 d) { return r32(ctx, d + ds::kUri); }
u32 cellSailDescriptorGetMediaInfo(PPUContext& ctx, u32 d) { return r32(ctx, d + ds::kMediaInfo); }

// SetParameter(descriptor, 1, 0 or 8, 0): bit 0 of the flags; only while the descriptor is new.
s32 cellSailDescriptorSetParameter(PPUContext& ctx, u32 d, s32 param, s64 value, s64 value2)
{
    if (r8(ctx, d + ds::kState) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (param == 1 && (value == 0 || value == 8) && value2 == 0) {
        const u32 f = r32(ctx, d + ds::kFlags);
        w32(ctx, d + ds::kFlags, value == 0 ? f & ~1u : f | 1u);
        return lv2::CELL_OK;
    }
    return error(CELL_SAIL_ERROR_INVALID_ARG);
}

// The argument back, or INVALID_STATE once the descriptor is past new.
s32 cellSailDescriptorSetAutoSelection(PPUContext& ctx, u32 d, u8 on)
{
    if (r8(ctx, d + ds::kState) != 0) return error(CELL_SAIL_ERROR_INVALID_STATE);
    w8(ctx, d + ds::kNoAuto, static_cast<u8>(on ^ 1));
    return on;
}

u32 cellSailDescriptorIsAutoSelection(PPUContext& ctx, u32 d) { return static_cast<u8>(r8(ctx, d + ds::kNoAuto) ^ 1); }

// ClearEs(descriptor, kind (0 audio, 1 video, 2 user), index): the stream's state 1 back to 0; the descriptor no further
// than open.
s32 cellSailDescriptorClearEs(PPUContext& ctx, u32 d, s32 kind, u32 index)
{
    if (r8(ctx, d + ds::kState) > 1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 p = r32(ctx, d + ds::kPlayer);
    u32 es;
    if (kind == 1) {
        if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        es = r32(ctx, d + ds::kVideo) + index * ds::kVideoStride;
    } else if (kind == 2) {
        if (index >= r32(ctx, p + pl::kUserCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        es = r32(ctx, d + ds::kUser) + index * ds::kUserStride;
    } else if (kind == 0) {
        if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        es = r32(ctx, d + ds::kAudio) + index * ds::kAudioStride;
    } else {
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    }
    if (r8(ctx, es) != 1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    w8(ctx, es, 0);
    return lv2::CELL_OK;
}

s32 cellSailDescriptorDestroyDatabase(PPUContext& ctx, u32 d)
{
    return r32(ctx, d + ds::kType) < 3 ? lv2::CELL_OK : error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
}

// GetCapabilities(descriptor, u32* kind, u32* start flags, u32* seek flags, u32* terminus): libsail's reading of the
// capability bits.
s32 cellSailDescriptorGetCapabilities(PPUContext& ctx, u32 d, u32 kind, u32 start, u32 seek, u32 terminus)
{
    if (!r32(ctx, d + ds::kSource) || !r32(ctx, d + ds::kUri)) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 flags = r32(ctx, d + ds::kFlags), type = r32(ctx, d + ds::kType);
    u64 c;
    if (type == 1) {
        c = 0xF0;
    } else if (type == 2) {
        c = flags ? 0x70 : 0xF0;
    } else {
        StackVar caps(ctx, gb(ctx), 0x18);
        source_capabilities(ctx, r32(ctx, d + ds::kSource), type, r32(ctx, d + ds::kMediaInfo), r32(ctx, d + ds::kUri),
            caps.addr());
        c = r64(ctx, caps.addr());
    }
    // `none`: flag bit 0 clear (libsail's (|flags| - 1) >> 31, true only for 0).
    const bool none = flags == 0;
    const bool seekable = c & 1;
    u32 sk = seekable ? 2 : 0, st = seekable ? 1 : 0;
    if (c & 0x40) sk |= 1;
    if (!none) {
        if (c & 0x80) {
            st |= 0x10;
            sk |= 0x20;
        }
    } else {
        sk |= 0x20;
        st |= 0x50;
    }
    w32(ctx, kind, none ? 0x75 : 1);
    w32(ctx, start, st);
    w32(ctx, seek, sk);
    w32(ctx, terminus, (c & 0x10) ? 0xE : 0x8);
    return lv2::CELL_OK;
}

// InquireCapability(descriptor, CellSailSourceStartCommand-like inquiry {start type, seek type, terminus type, flags,
// count, 0, u64 start position, u64 end position}): whether the source can do it, as libsail checks it against the
// capability bits (bit 0 seek by position, 0x10 by time, 0x40 to the end, 0x80 by index) and the stream's size.
s32 cellSailDescriptorInquireCapability(PPUContext& ctx, u32 d, u32 q)
{
    if (!q || r32(ctx, q + 0x14)) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u8 state = r8(ctx, d + ds::kState);
    if (!r32(ctx, d + ds::kSource) || !r32(ctx, d + ds::kUri) ||
        (sdk_version_of(ctx) > 0x21FFF && state != 1 && state != 3 && state != 5))
        return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 type = r32(ctx, d + ds::kType), flags = r32(ctx, d + ds::kFlags);
    u64 caps;
    if (type == 1) {
        caps = 0xF0;
    } else if (type == 2) {
        caps = flags ? 0x70 : 0xF0;
    } else {
        if (type != 0) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
        StackVar out(ctx, gb(ctx), 0x18);
        const s32 r = source_capabilities(ctx, r32(ctx, d + ds::kSource), 0, r32(ctx, d + ds::kMediaInfo),
            r32(ctx, d + ds::kUri), out.addr());
        if (r < 0) return r;
        caps = r64(ctx, out.addr());
        if (!flags) caps |= 0x80;
    }
    const s32 bad = error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 start = r32(ctx, q), how = r32(ctx, q + 12);
    u64 from = r64(ctx, q + 0x18), to = r64(ctx, q + 0x20);
    const bool flagged = flags != 0; // libsail's -|flags| < 0
    const bool no_time = !((caps >> 4) & 1);
    s32 count = static_cast<s32>(r32(ctx, q + 16));
    if (start == 4) {
        if (no_time || count < 1) return bad;
    } else if (start < 5) {
        if (start > 1 || count != 0) return bad;
        count = 1;
    } else if (start == 0x40) {
        if (no_time || count != 0 || from || to) return bad;
        return (how >> 5) == 0 && (how & 7) == 0 ? lv2::CELL_OK : bad;
    } else if (start == 0x10 || start == 0x20) {
        if (no_time || flagged || count < 1) return bad;
        if (start == 0x20) count = -count;
    } else {
        return bad;
    }
    switch (r32(ctx, q + 4)) {
    case 1:
        if (from && !(caps & 1)) return bad;
        break;
    case 0:
        if (from) return bad;
        break;
    case 0x10:
        if (from && (flagged || !(caps & 0x80))) return bad;
        break;
    case 0x40: break;
    default: return bad;
    }
    const u64 size = r64(ctx, d + ds::kSize);
    switch (r32(ctx, q + 8)) {
    case 1:
        if (to || !(caps & 0x40)) return bad;
        if (count > 0) to = size;
        break;
    case 0:
        if (to) return bad;
        if (count > 0) to = size;
        break;
    case 2:
        if (to && !(caps & 1)) return bad;
        break;
    case 0x20:
        if (to && !(caps & 0x80)) return bad;
        break;
    default: return bad;
    }
    if (size < from || size < to) return bad;
    if (count < 1) {
        if (count == 0 || from < to) return bad;
    } else if (to < from) {
        return bad;
    }
    if (how == 0) return lv2::CELL_OK;
    if ((how & 6) == 0) return (how & 0x18) ? lv2::CELL_OK : bad;
    return no_time ? bad : lv2::CELL_OK;
}

} // namespace

void register_cellSailDescriptor(Registry& r)
{
#define SAIL(f) ::pspocket::hle::Function{{}, #f, recomp::nid_of(#f), &detail::thunk<&f>, false}
    r.add("cellSail", {
        SAIL(cellSailDescriptorGetStreamType),
        SAIL(cellSailDescriptorGetUri),
        SAIL(cellSailDescriptorGetMediaInfo),
        SAIL(cellSailDescriptorSetParameter),
        SAIL(cellSailDescriptorSetAutoSelection),
        SAIL(cellSailDescriptorIsAutoSelection),
        SAIL(cellSailDescriptorClearEs),
        SAIL(cellSailDescriptorDestroyDatabase),
        SAIL(cellSailDescriptorGetCapabilities),
        SAIL(cellSailDescriptorInquireCapability),
    });
#undef SAIL
}

} // namespace pspocket::hle
