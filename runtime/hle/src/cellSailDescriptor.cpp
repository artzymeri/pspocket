// cellSail's descriptors (D-562, part 3a): what the game reads and sets on a descriptor without the player's thread, as
// libsail (fw 4.92) has it. A descriptor (made by the player) holds +0 the stream type (0 PAMF, 1 MP4, 2 AVI), +0x10 the
// URI, +0x14 the media info, +0x18 flags (bit 0 set by SetParameter), +0x1C the player, +0x24 its source, +0x28/+0x2C/
// +0x30 the audio, video and user elementary stream tables (0x90, 0x78 and 0x4C bytes each, a state byte first), +0x38
// its state byte (0 created, 1 open, 3 and 5 others open), +0x39 auto selection off, +0x40 the stream's reader (PAMF
// reader, or the MP4/AVI library's movie), +0xD0 the stream's size. The capabilities come from the source's
// GetCapabilities callback (PAMF) or are fixed (MP4 0xF0, AVI 0x70 or 0xF0).

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace sail {

s32 clear_es(PPUContext& ctx, u32 d, s32 kind, u32 index)
{
    const u32 p = r32(ctx, d + 0x1C);
    u32 es;
    if (kind == 1) {
        if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        es = r32(ctx, d + 0x2C) + index * 0x78;
    } else if (kind == 2) {
        if (index >= r32(ctx, p + pl::kUserCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        es = r32(ctx, d + 0x30) + index * 0x4C;
    } else if (kind == 0) {
        if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        es = r32(ctx, d + 0x28) + index * 0x90;
    } else {
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    }
    if (r8(ctx, es) != 1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    w8(ctx, es, 0);
    return lv2::CELL_OK;
}

} // namespace sail

namespace {

using namespace sail;

namespace ds {
constexpr u32 kType = 0, kUri = 0x10, kMediaInfo = 0x14, kFlags = 0x18, kPlayer = 0x1C, kSource = 0x24, kAudio = 0x28,
              kVideo = 0x2C, kUser = 0x30, kState = 0x38, kNoAuto = 0x39, kReader = 0x40, kSize = 0xD0;
constexpr u32 kAudioStride = 0x90, kVideoStride = 0x78, kUserStride = 0x4C;
} // namespace ds

u32 sdk_version_of(PPUContext& ctx) { return thread_of(ctx).kernel.config().sdk_version; }

u64 pamf(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return module_call(ctx, "cellPamf", name, args);
}

void unsupported_container(PPUContext& ctx, u32 d, std::string_view what)
{
    log_call_site(ctx, gb(ctx), std::format("cellSail: {} of descriptor {:#x}, stream type {} (MP4 or AVI): the firmware "
        "libraries' part is not pspocket's yet", what, d, r32(ctx, d + ds::kType)));
    PS_FATAL(kChan, "libsail's MP4 and AVI descriptors are a later part of D-562");
}

u16 r16(PPUContext& ctx, u32 a) { return ps_read16(gb(ctx), a); }

// ------------------------------------------------------------------------------------------------- elementary streams

// The stream's entries (selected byte first, +1 its index, +2 its PAMF type, +4 the reader's stream index, +0xC the
// elementary stream's filter ID, 16 bytes). Audio (0x90 bytes): +0x1C 48000 (LPCM) or 0, LPCM's +0x20 channels, +0x24
// sample bits (16, 24), +0x30 1, +0x34 1 or 3, +0x38 bytes per frame; +0x28 the codec (13-15 ATRAC3plus by channels, 1
// LPCM, 2 AC3), +0x60 1, +0x61 channels, +0x62 samples per frame, +0x64 48000, +0x68 the channel layout. Video (0x78):
// +0x1C the level (AVC) or the profile kind (M2V: 1 for 4:2:0 main, else 3), +0x20 1 AVC / 0 M2V, +0x24 the level or
// profile, +0x28 points at +0x2C {12 AVC / 8 M2V, u16 width, u16 height, AVC: deblocking off, 3}, +0x38 the picture
// {3, interlaced, 8, frame rate code, u16 width, u16 height, u32 width, 0, 0xFF, aspect code}. User (0x4C): the filter ID
// at +0x3C.

// The "changed" accumulator: libsail starts from the last libpamf answer and marks a difference with 1.
struct Picture {
    u8 interlaced;
    u16 width, height;
    u32 width32;
    u8 aspect;
};

s32 set_picture(PPUContext& ctx, u32 e, const Picture& pic, u8 rate, s32 changed)
{
    const u32 v = e + 0x38;
    const bool same = r8(ctx, v) == 3 && static_cast<s8>(r8(ctx, v + 1)) == static_cast<s8>(pic.interlaced) &&
        r8(ctx, v + 2) == 8 && static_cast<s16>(r16(ctx, v + 4)) == static_cast<s16>(pic.width) &&
        static_cast<s16>(r16(ctx, v + 6)) == static_cast<s16>(pic.height) && r32(ctx, v + 8) == pic.width32 &&
        r32(ctx, v + 12) == 0 && r8(ctx, v + 16) == 0xFF && static_cast<s8>(r8(ctx, v + 17)) == static_cast<s8>(pic.aspect);
    if (!same) changed = 1;
    w8(ctx, v, 3);
    w8(ctx, v + 1, pic.interlaced);
    w8(ctx, v + 2, 8);
    w16(ctx, v + 4, pic.width);
    w16(ctx, v + 6, pic.height);
    w32(ctx, v + 8, pic.width32);
    w32(ctx, v + 12, 0);
    w8(ctx, v + 16, 0xFF);
    w8(ctx, v + 17, pic.aspect);
    if (rate == 0xFF) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    if (static_cast<s8>(r8(ctx, v + 3)) != static_cast<s8>(rate)) changed = 1;
    w8(ctx, v + 3, rate);
    if (changed < 0) return changed;
    w8(ctx, e, 1);
    return changed;
}

// The aspect code of a progressive 720-wide picture with square-ish pixels: 5 for 480 lines, 4 for 576, else 1.
u8 aspect_of(u16 width, u8 interlaced, u8 aspect, u16 height)
{
    if (width != 720 || interlaced != 0 || aspect != 1) return aspect;
    return height == 480 ? 5 : height == 576 ? 4 : 1;
}

s32 select_audio(PPUContext& ctx, u32 d, u32 index, u32 reader, u32 tmp)
{
    const u32 p = r32(ctx, d + ds::kPlayer);
    if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    const u32 e = r32(ctx, d + ds::kAudio) + index * ds::kAudioStride;
    if (!e) return error(CELL_SAIL_ERROR_INVALID_ARG);
    s32 r = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamTypeAndChannel", {reader, tmp, tmp + 1}));
    if (r < 0) return r;
    const u8 type = r8(ctx, tmp);
    if (static_cast<u8>(type - 2) > 2) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    w8(ctx, e + 2, type);
    w32(ctx, e + 4, static_cast<u32>(pamf(ctx, "cellPamfReaderGetStreamIndex", {reader})));
    r = static_cast<s32>(pamf(ctx, "cellPamfStreamTypeToEsFilterId", {type, r8(ctx, tmp + 1), tmp + 0x10}));
    if (r < 0) return r;
    std::memmove(gb(ctx) + e + 0xC, gb(ctx) + tmp + 0x10, 16);
    w8(ctx, e + 0x60, 1);
    const u32 info = tmp + 0x20;
    s32 changed;
    u32 codec, layout;
    u16 frame;
    u8 ch;
    if (type == 3) {
        // LPCM: 48 kHz, 1 or 2 (16 or 24 bits) per sample, 1, 2, 6 or 8 channels.
        changed = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamInfo", {reader, info, 8}));
        if (changed < 0) return changed;
        if (r32(ctx, info) != 1) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
        ch = r8(ctx, info + 4);
        const u16 bits = r16(ctx, info + 6);
        u32 depth;
        if (bits == 1) depth = 16;
        else if (bits == 3) depth = 24;
        else return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
        if (!(r32(ctx, e + 0x1C) == 48000 && r32(ctx, e + 0x20) == ch && r32(ctx, e + 0x24) == depth)) changed = 1;
        w32(ctx, e + 0x24, depth);
        w32(ctx, e + 0x20, ch);
        w32(ctx, e + 0x1C, 48000);
        w32(ctx, e + 0x38, bits == 1 ? ch * 480u : ch * 720u);
        w32(ctx, e + 0x34, bits);
        w32(ctx, e + 0x30, 1);
        codec = 1;
        frame = 0xF0;
        if (ch == 2) layout = 2;
        else if (ch == 1) layout = 1;
        else if (ch == 6) layout = 8;
        else if (ch == 8) layout = 11;
        else return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    } else {
        // ATRAC3plus and AC3: 48 kHz only.
        changed = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamInfo", {reader, info, 8}));
        if (changed < 0) return changed;
        if (r32(ctx, info) != 1) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
        ch = r8(ctx, info + 4);
        w32(ctx, e + 0x1C, 0);
        if (type == 2) {
            frame = 0x800;
            if (ch <= 2) codec = 13, layout = 2;
            else if (ch <= 6) codec = 14, layout = 9;
            else codec = 15, layout = 11;
        } else {
            codec = 2;
            frame = 0x600;
            if (ch == 2) layout = 2;
            else if (ch == 6) layout = 8;
            else if (ch == 1) layout = 1;
            else return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
        }
    }
    if (static_cast<s32>(r32(ctx, e + 0x28)) != static_cast<s32>(codec)) changed = 1;
    w32(ctx, e + 0x28, codec);
    if (static_cast<s8>(r8(ctx, e + 0x61)) != static_cast<s32>(ch) ||
        static_cast<s32>(r32(ctx, e + 0x68)) != static_cast<s32>(layout) ||
        static_cast<s16>(r16(ctx, e + 0x62)) != static_cast<s16>(frame) || r32(ctx, e + 0x64) != 48000)
        changed = 1;
    w8(ctx, e + 0x61, ch);
    w32(ctx, e + 0x64, 48000);
    w32(ctx, e + 0x68, layout);
    w16(ctx, e + 0x62, frame);
    if (changed < 0) return changed;
    w8(ctx, e, 1);
    return changed;
}

s32 select_video(PPUContext& ctx, u32 d, u32 index, u32 reader, u32 tmp)
{
    const u32 p = r32(ctx, d + ds::kPlayer);
    if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    s32 r = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamTypeAndChannel", {reader, tmp + 1, tmp}));
    if (r < 0) return r;
    const u8 type = r8(ctx, tmp + 1);
    if (type > 1) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    const u32 e = r32(ctx, d + ds::kVideo) + index * ds::kVideoStride;
    w8(ctx, e + 2, type);
    w32(ctx, e + 4, static_cast<u32>(pamf(ctx, "cellPamfReaderGetStreamIndex", {reader})));
    r = static_cast<s32>(pamf(ctx, "cellPamfStreamTypeToEsFilterId", {type, r8(ctx, tmp), tmp + 0x10}));
    if (r < 0) return r;
    std::memmove(gb(ctx) + e + 0xC, gb(ctx) + tmp + 0x10, 16);
    const u32 info = tmp + 0x20;
    if (type == 0) {
        // AVC: the level, the picture less its bottom crop (twice over for fields), the frame rate's code.
        s32 changed = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamInfo", {reader, info, 0x20}));
        if (changed < 0) return changed;
        const u8 level = r8(ctx, info + 1);
        w32(ctx, e + 0x1C, level);
        if (!(r32(ctx, e + 0x20) == 1 && r32(ctx, e + 0x24) == level)) changed = 1;
        w32(ctx, e + 0x24, level);
        w16(ctx, e + 0x30, r16(ctx, info + 10));
        w32(ctx, e + 0x20, 1);
        w16(ctx, e + 0x32, r16(ctx, info + 12));
        w32(ctx, e + 0x2C, 12);
        w8(ctx, e + 0x35, 3);
        w8(ctx, e + 0x34, r8(ctx, info + 28) == 0 ? 1 : 0);
        w32(ctx, e + 0x28, e + 0x2C);
        const u8 mbs = r8(ctx, info + 2);
        Picture pic{};
        pic.width = r16(ctx, info + 10);
        pic.width32 = pic.width;
        pic.height = static_cast<u16>(r16(ctx, info + 12) - (2u - mbs) * (r16(ctx, info + 20) * 2u));
        pic.interlaced = mbs == 0 ? 1 : 0;
        pic.aspect = aspect_of(pic.width, pic.interlaced, r8(ctx, info + 5), pic.height);
        static constexpr u8 kRates[7] = {0, 0xFF, 2, 3, 0xFF, 5, 6};
        const u8 fr = r8(ctx, info + 4);
        return set_picture(ctx, e, pic, fr < 7 ? kRates[fr] : 0xFF, changed);
    }
    // MPEG-2: the profile, the display size, the frame rate's code (1280x720 at code 7: profile 2).
    s32 changed = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamInfo", {reader, info, 0x18}));
    if (changed < 0) return changed;
    const u8 profile = r8(ctx, info);
    w32(ctx, e + 0x1C, profile == 1 ? 1 : 3);
    u32 kind = profile > 3 ? 3 : profile;
    const u16 hsv = r16(ctx, info + 14), vsv = r16(ctx, info + 16);
    if (hsv == 1280 && vsv == 720 && r8(ctx, info + 3) == 7) kind = 2;
    if (r32(ctx, e + 0x20) != 0 || r32(ctx, e + 0x24) != kind) changed = 1;
    w32(ctx, e + 0x24, kind);
    w32(ctx, e + 0x28, e + 0x2C);
    w16(ctx, e + 0x30, r16(ctx, info + 10));
    w16(ctx, e + 0x32, r16(ctx, info + 12));
    w32(ctx, e + 0x20, 0);
    w32(ctx, e + 0x2C, 8);
    Picture pic{};
    pic.interlaced = static_cast<u8>(r8(ctx, info + 1) ^ 1);
    pic.width = hsv;
    pic.height = vsv;
    pic.width32 = r16(ctx, info + 10);
    pic.aspect = aspect_of(hsv, pic.interlaced, r8(ctx, info + 4), vsv);
    static constexpr u8 kRates[8] = {0xFF, 0, 0xFF, 2, 3, 0xFF, 5, 6};
    const u8 fr = r8(ctx, info + 3);
    return set_picture(ctx, e, pic, fr < 8 ? kRates[fr] : 0xFF, changed);
}

s32 select_user(PPUContext& ctx, u32 d, u32 index, u32 reader, u32 tmp)
{
    const u32 p = r32(ctx, d + ds::kPlayer);
    if (index >= r32(ctx, p + pl::kUserCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
    s32 r = static_cast<s32>(pamf(ctx, "cellPamfReaderGetStreamTypeAndChannel", {reader, tmp + 1, tmp}));
    if (r < 0) return r;
    const u32 e = r32(ctx, d + ds::kUser) + index * ds::kUserStride;
    w8(ctx, e + 2, r8(ctx, tmp + 1));
    r = static_cast<s32>(pamf(ctx, "cellPamfStreamTypeToEsFilterId", {r8(ctx, tmp + 1), r8(ctx, tmp), tmp + 0x10}));
    if (r < 0) return r;
    std::memmove(gb(ctx) + e + 0x3C, gb(ctx) + tmp + 0x10, 16);
    w8(ctx, e, 1);
    return r;
}

} // namespace

s32 sail::select_es(PPUContext& ctx, u32 d, s32 kind, u32 index, u32 es)
{
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) unsupported_container(ctx, d, "an elementary stream chosen");
    if (type != 0) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    u32 reader = es;
    if (!reader) {
        u32 stype;
        if (kind == 1) stype = 20;
        else if (kind == 2) stype = 5;
        else if (kind == 0) stype = 21;
        else return error(CELL_SAIL_ERROR_INVALID_ARG);
        reader = d + ds::kReader;
        const s32 r = static_cast<s32>(pamf(ctx, "cellPamfReaderSetStreamWithTypeAndIndex", {reader, stype, index & 0xFF}));
        if (r < 0) return r;
    }
    StackVar tmp(ctx, gb(ctx), 0x60); // type and channel, the filter ID (+0x10), the stream's information (+0x20)
    if (kind == 1) return select_video(ctx, d, index, reader, tmp.addr());
    if (kind == 2) return select_user(ctx, d, index, reader, tmp.addr());
    if (kind != 0) return error(CELL_SAIL_ERROR_INVALID_ARG);
    return select_audio(ctx, d, index, reader, tmp.addr());
}

namespace {

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

// SetEs(descriptor, kind, index, a pointer that must be given, the stream): an open descriptor's stream not chosen yet
// (USING when it is), chosen through `es`.
s32 cellSailDescriptorSetEs(PPUContext& ctx, u32 d, s32 kind, u32 index, u32 given, u32 es)
{
    if (r8(ctx, d + ds::kState) != 1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    if (!given) return error(CELL_SAIL_ERROR_INVALID_ARG);
    const u32 p = r32(ctx, d + ds::kPlayer);
    u32 entry;
    if (kind == 1) {
        if (index >= r32(ctx, p + pl::kVideoCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        entry = r32(ctx, d + ds::kVideo) + index * ds::kVideoStride;
    } else if (kind == 2) {
        if (index >= r32(ctx, p + pl::kUserCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        entry = r32(ctx, d + ds::kUser) + index * ds::kUserStride;
    } else if (kind == 0) {
        if (index >= r32(ctx, p + pl::kAudioCount)) return error(CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE);
        entry = r32(ctx, d + ds::kAudio) + index * ds::kAudioStride;
    } else {
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    }
    if (r8(ctx, entry) != 0) return error(CELL_SAIL_ERROR_USING);
    return select_es(ctx, d, kind, index, es);
}

// CreateDatabase(descriptor, database, size, arg): for PAMF a copy of the descriptor's reader (0x80 bytes, at least that
// much room) set to the first video stream; the descriptor open (or held open).
s32 cellSailDescriptorCreateDatabase(PPUContext& ctx, u32 d, u32 db, u32 size, u64)
{
    const u8 st = r8(ctx, d + ds::kState);
    if (st != 1 && st != 3 && st != 5) return error(CELL_SAIL_ERROR_INVALID_STATE);
    const u32 type = r32(ctx, d + ds::kType);
    if (type == 1 || type == 2) {
        if (size > 0x7F) unsupported_container(ctx, d, "a database made");
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    }
    if (type != 0) return error(CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    if (size <= 0x7F) return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + db, gb(ctx) + d + ds::kReader, 0x80);
    pamf(ctx, "cellPamfReaderSetStreamWithTypeAndIndex", {db, 0x14, 0});
    return lv2::CELL_OK;
}

// ClearEs(descriptor, kind (0 audio, 1 video, 2 user), index): the stream's state 1 back to 0; the descriptor no further
// than open (the player's own use, clear_es, does not look at the descriptor's state).
s32 cellSailDescriptorClearEs(PPUContext& ctx, u32 d, s32 kind, u32 index)
{
    if (r8(ctx, d + ds::kState) > 1) return error(CELL_SAIL_ERROR_INVALID_STATE);
    return clear_es(ctx, d, kind, index);
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
        SAIL(cellSailDescriptorSetEs),
        SAIL(cellSailDescriptorCreateDatabase),
        SAIL(cellSailDescriptorDestroyDatabase),
        SAIL(cellSailDescriptorGetCapabilities),
        SAIL(cellSailDescriptorInquireCapability),
    });
#undef SAIL
}

} // namespace pspocket::hle
