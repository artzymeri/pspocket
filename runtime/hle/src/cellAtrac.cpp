// cellAtrac (D-532): ATRAC3 and ATRAC3plus playback over cellAdec, as Sony's libatrac3plus.sprx (fw 4.92) does it, read
// from its code (the reference, D-046) and compared with it running on pspocket (tests/hle/test_atrac.cpp). RPCS3's
// cellAtrac answers without decoding (rpcs3/Emu/Cell/Modules/cellAtrac.cpp; its header gives the error codes and the
// public structures).
//
// The handle (CellAtracHandle, 512 bytes, the game's) holds everything, at libatrac3plus's offsets: a mutex and a
// condition guarding cellAdec's callback, cellAdec's type, attributes, callback and resource, the codec parameters, the
// work memory, and from +0x70 the stream state (an AT3/AT3+ file read whole, half, or streamed through a ring buffer,
// with a second buffer for a loop's tail). SetDataAndGetMemSize reads the RIFF WAVE header (fmt for ATRAC3 0x270 or
// ATRAC3plus's WAVE_FORMAT_EXTENSIBLE with its GUID, fact, smpl loops, data); CreateDecoder opens cellAdec on the work
// memory (the PCM of one frame first, then cellAdec's) and decodes the frames before the first sample; Decode feeds one
// frame (cellAdecDecodeAu), waits for the callback's PCM-out, takes the PCM (cellAdecGetPcm) into the game's buffer, or
// into the work memory and copies the part the play range keeps, and moves the stream's positions.
//
// cellAtracMulti (D-535) is Sony's libatrac3multi.sprx: the same stream code on a handle laid out 0x14 bytes lower
// (Layout), ATRAC3 only (no joint stereo), with output tracks: the game names a file channel for each (the track
// array), and each frame's channels are shuffled into the tracks before cellAdec's ATRAC3 multichannel decoder (type
// 31) gets it. Its calls refuse NULL pointers first (0x80610B91), and its errors are libatrac3plus's at 0x80610Bxx.
//
// Deviations: none in behaviour. Memory that is not the guest's is refused with CELL_ATRAC_ERROR_API_FAIL and the call
// site logged, where libatrac3plus would fault.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellAtrac";

enum : u32 {
    CELL_ATRAC_ERROR_API_FAIL = 0x80610301,
    CELL_ATRAC_ERROR_READSIZE_OVER_BUFFER = 0x80610311,
    CELL_ATRAC_ERROR_UNKNOWN_FORMAT = 0x80610312,
    CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL = 0x80610313,
    CELL_ATRAC_ERROR_ILLEGAL_SAMPLING_RATE = 0x80610314,
    CELL_ATRAC_ERROR_ILLEGAL_DATA = 0x80610315,
    CELL_ATRAC_ERROR_NO_DECODER = 0x80610321,
    CELL_ATRAC_ERROR_UNSET_DATA = 0x80610322,
    CELL_ATRAC_ERROR_DECODER_WAS_CREATED = 0x80610323,
    CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED = 0x80610331,
    CELL_ATRAC_ERROR_NODATA_IN_BUFFER = 0x80610332,
    CELL_ATRAC_ERROR_NOT_ALIGNED_OUT_BUFFER = 0x80610333,
    CELL_ATRAC_ERROR_NEED_SECOND_BUFFER = 0x80610334,
    CELL_ATRAC_ERROR_ALLDATA_IS_ONMEMORY = 0x80610341,
    CELL_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG = 0x80610342,
    CELL_ATRAC_ERROR_NONEED_SECOND_BUFFER = 0x80610351,
    CELL_ATRAC_ERROR_UNSET_LOOP_NUM = 0x80610361,
    CELL_ATRAC_ERROR_ILLEGAL_SAMPLE = 0x80610371,
    CELL_ATRAC_ERROR_ILLEGAL_RESET_BYTE = 0x80610372,
    CELL_ATRAC_ERROR_ILLEGAL_PPU_THREAD_PRIORITY = 0x80610381,
    CELL_ATRAC_ERROR_ILLEGAL_SPU_THREAD_PRIORITY = 0x80610382,
};
s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kMagic = 0x01234567;
constexpr u16 kAt3Plus = 0x1000, kAt3 = 0x1001;

// The handle: +0 mutex, +4 condition, +8 the first error cellAdec reported, +0xC flags (bit 0 mutex made, bit 1
// condition made, 0x10000000 a decode waits for PCM, 0x20000000 PCM came before the wait) | +0x10 cellAdec type |
// +0x14 CellAdecAttr | +0x20 CellAdecCb | +0x28 CellAdecResource | +0x3C cellAdec handle | +0x40 CellAdecParamAtrac3 |
// +0x50 CellAdecParamAtracX | +0x6C work memory (0: no decoder) | +0x70 the stream state S.
namespace hd {
constexpr u32 kMutex = 0, kCond = 4, kError = 8, kFlags = 0xC, kType = 0x10, kAttr = 0x14, kCb = 0x20, kResource = 0x28,
              kAdec = 0x3C, kAt3Param = 0x40, kAtxParam = 0x50;
constexpr u32 kWaiting = 0x10000000, kEarly = 0x20000000;
}
// The stream state, from the handle's +0x70: +0 magic | +4 next sample | +8 last sample | +0xC loop start | +0x10 loop
// end (0: none) | +0x14 the first sample's offset (the encoder delay and the decoder's) | +0x18 s8 frames still to
// decode before PCM is kept | +0x19 how the data is held (2 all in the buffer, 3 the rest still to add, 4 streamed,
// 5 streamed with a loop at the end, 6 streamed with a second buffer for the loop's tail) | +0x1A which buffer a
// streamed frame comes from (0/1 the second, 2 back to the first) | +0x1B channels | +0x1C u16 frame bytes | +0x1E u16
// codec (0x1000 ATRAC3plus, 0x1001 ATRAC3) | +0x20 sample rate | +0x24 the data's offset in the file | +0x28 the next
// frame's offset | +0x2C the data's end | +0x30 loops left (-1 for ever) | +0x34 stream bytes in the buffer | +0x38 where
// in the ring the next frame is | +0x3C where in the second buffer | +0x40 buffer | +0x44 second buffer | +0x48 buffer
// size | +0x4C second buffer size.
namespace st {
constexpr u32 kSample = 4, kEnd = 8, kLoopStart = 0xC, kLoopEnd = 0x10, kOffset = 0x14, kSkip = 0x18, kHold = 0x19,
              kPhase = 0x1A, kChannels = 0x1B, kFrame = 0x1C, kCodec = 0x1E, kRate = 0x20, kDataStart = 0x24,
              kRead = 0x28, kDataEnd = 0x2C, kLoops = 0x30, kBytes = 0x34, kRing = 0x38, kSecondPos = 0x3C,
              kBuffer = 0x40, kSecond = 0x44, kBufferSize = 0x48, kSecondSize = 0x4C;
}

// Which module's handle: libatrac3plus's, or libatrac3multi's (cellAtracMulti, D-535), whose stream state is the same
// from +0x5C, its work memory at +0x58 (the frame's tracks shuffled into the buffer at +0x54), its codec parameters at
// +0x40, its errors 0x80610Bxx where libatrac3plus's are 0x806103xx.
struct Layout {
    u32 s, work;
    bool multi;
};
constexpr Layout kPlus{0x70, 0x6C, false}, kMulti{0x5C, 0x58, true};
constexpr u32 kMultiAuBuffer = 0x54, kMultiTracks = 0x40, kMultiFileChannels = 0x50;
// libatrac3multi's stream state past libatrac3plus's: +0x50 the file's channels, +0x54 the track array (the file
// channel each output track takes, -1 or past the file's for silence; eight at most).
namespace mst {
constexpr u32 kFileChannels = 0x50, kTracks = 0x54;
}

// A view of one handle in guest memory.
struct Atrac {
    u8* b;
    u32 h;
    const Layout* L = &kPlus;
    u32 s() const { return h + L->s; }
    u32 work() const { return ps_read32(b, h + L->work); }
    void set_work(u32 v) const { ps_write32(b, h + L->work, v); }
    s32 e(u32 code) const { return static_cast<s32>(L->multi ? code + 0x800 : code); }
    u32 r32(u32 off) const { return ps_read32(b, h + off); }
    void w32(u32 off, u32 v) const { ps_write32(b, h + off, v); }
    u32 sr(u32 off) const { return ps_read32(b, s() + off); }
    void sw(u32 off, u32 v) const { ps_write32(b, s() + off, v); }
    u8 s8(u32 off) const { return b[s() + off]; }
    void s8w(u32 off, u8 v) const { b[s() + off] = v; }
    u16 s16(u32 off) const { return ps_read16(b, s() + off); }
    u16 codec() const { return s16(st::kCodec); }
    u32 frame() const { return s16(st::kFrame); }
    u32 frame_shift() const { return 0x100B - codec(); } // 11 for ATRAC3plus (2048 samples), 10 for ATRAC3 (1024)
    u32 frame_samples() const { return 0x800u >> (codec() & 1); }
    u32 sample_mask() const { return 0x7FFu >> (codec() & 1); }
    u32 delay() const { return codec() == kAt3 ? 0x45 : 0xB8; } // samples of the first frame not kept
    bool ready() const { return sr(0) == kMagic; }
};

// libatrac3plus's common checks: the data set (the magic), then a decoder made.
s32 check(const Atrac& a)
{
    if (!a.ready()) return a.e(CELL_ATRAC_ERROR_UNSET_DATA);
    if (!a.work()) return a.e(CELL_ATRAC_ERROR_NO_DECODER);
    return lv2::CELL_OK;
}

s32 not_guest(PPUContext& ctx, const Layout& L, std::string_view fn, std::string_view what, u32 addr)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}: the {} at {:#x} is not guest memory", fn, what, addr));
    return Atrac{nullptr, 0, &L}.e(CELL_ATRAC_ERROR_API_FAIL);
}

// The handle (512 bytes) as guest memory.
bool handle_ok(PPUContext& ctx, u32 h) { return guest_readable(thread_of(ctx), h, 0x200); }

// ------------------------------------------------------------------------------------------------ positions

// The last byte of the frame holding `sample`.
u32 frame_last_byte(const Atrac& a, u32 sample)
{
    return ((sample >> a.frame_shift()) + 1) * a.frame() + a.sr(st::kDataStart) - 1;
}

// Where decoding starts for `sample`: the frame before its frame (two before when the sample lies in the decoder's
// delay), as a file offset.
u32 frame_before(const Atrac& a, u32 sample)
{
    u32 at = ((sample >> a.frame_shift()) - 1) * a.frame();
    if ((sample & a.sample_mask()) < a.delay() && at != 0) at -= a.frame();
    return at + a.sr(st::kDataStart);
}

// Frames to decode before `sample`'s PCM is kept: one, or two in the delay.
u8 frames_before(const Atrac& a, u32 sample) { return (sample & a.sample_mask()) >= a.delay() ? 1 : 2; }

// The samples the next Decode gives: the next frame's, less what lies before the next sample and after the last.
u32 next_samples(const Atrac& a)
{
    const u32 pos = a.sr(st::kSample), end = a.sr(st::kEnd), mask = a.sample_mask();
    const u32 frame_end = mask + (pos & ~mask);
    const u32 cut = (frame_end > end ? frame_end - end : 0) + (pos & mask);
    return a.frame_samples() > cut ? a.frame_samples() - cut : 0;
}

// `base` moved on by whole `step`s as far as `limit` allows (`limit` itself when base is past it).
u32 whole_steps(u32 base, u32 step, u32 limit) { return base < limit ? base + step * ((limit - base) / step) : limit; }

// A ring position `n` bytes on from `base`, wrapped at the last whole frame below `limit`.
u32 ring_wrap(u32 base, u32 limit, u32 n, u32 frame)
{
    u32 end = base + n;
    const u32 top = whole_steps(base, frame, limit);
    if (top > end) return end;
    end -= top;
    if (top < n) return 0;
    return end;
}

u32 ring_next(u32 end, u32 base, u32 n, u32 limit) { return base + n < end && n < limit ? base + n : 0; }

// The bytes in the buffer of a stream with a second buffer, once the play position passed the loop end and the second
// buffer holds the tail.
u32 bytes_held(const Atrac& a)
{
    if (a.sr(st::kSample) > a.sr(st::kLoopEnd) && a.s8(st::kPhase) == 1) {
        const u32 pos = a.sr(st::kSecondPos);
        const u32 top = whole_steps(pos, a.frame(), a.sr(st::kSecondSize));
        if (pos > top) return 0;
        const u32 d = top - pos;
        return a.sr(st::kBytes) > d ? a.sr(st::kBytes) - d : 0;
    }
    return a.sr(st::kBytes);
}

// Where the game writes more stream data (cellAtracGetStreamDataInfo's write address), from the buffer.
u32 write_offset(const Atrac& a)
{
    const u8 hold = a.s8(st::kHold);
    if (hold == 6 && a.sr(st::kSample) > a.sr(st::kLoopEnd) && a.s8(st::kPhase) == 1)
        return ring_wrap(0, a.sr(st::kBufferSize), bytes_held(a), a.frame());
    if (hold == 5 || hold == 6 || (hold == 4 && a.sr(st::kBytes) + a.sr(st::kRead) < a.sr(st::kDataEnd)))
        return ring_wrap(a.sr(st::kRing), a.sr(st::kBufferSize), a.sr(st::kBytes), a.frame());
    return 0;
}

// The room for more stream data in the buffer (cellAtracGetVacantSize).
u32 vacant(const Atrac& a)
{
    const u8 hold = a.s8(st::kHold);
    if (hold == 2 || hold == 3) {
        const u32 used = a.sr(st::kDataStart) + a.sr(st::kBytes);
        return used <= a.sr(st::kBufferSize) ? a.sr(st::kBufferSize) - used : 0;
    }
    if (hold >= 4 && hold <= 6) {
        const u32 held = bytes_held(a);
        const u32 base = a.s8(st::kPhase) != 1 ? a.sr(st::kRing) : 0;
        const u32 top = whole_steps(base, a.frame(), a.sr(st::kBufferSize));
        return held <= top ? top - held : 0;
    }
    return 0;
}

// How much stream data the game may add now (cellAtracAddStreamData's limit, GetStreamDataInfo's size): the room in the
// ring, no more than the file (or the loop, while it repeats) has left.
u32 writable(const Atrac& a)
{
    const u32 held = bytes_held(a);
    const u32 base = a.s8(st::kPhase) != 1 ? a.sr(st::kRing) : 0;
    const u32 top = whole_steps(base, a.frame(), a.sr(st::kBufferSize));
    const u32 room = top > base + held ? top - (base + held) : held <= top ? top - held : 0;
    const u8 hold = a.s8(st::kHold);
    const u32 read_end = a.sr(st::kRead) + a.sr(st::kBytes);
    if (hold == 4 || (hold == 6 && a.sr(st::kSample) > a.sr(st::kLoopEnd))) {
        if (a.sr(st::kDataEnd) <= read_end) return 0;
        return std::min(room, a.sr(st::kDataEnd) - read_end);
    }
    if (hold != 5 && hold != 6) return 0;
    const u32 last = frame_last_byte(a, a.sr(st::kLoopEnd));
    u32 left;
    if (last + 1 > read_end) {
        left = last - read_end + 1;
    } else {
        const u32 loop = last - frame_before(a, a.sr(st::kLoopStart)) + 1;
        const u32 past = read_end - last - 1;
        left = loop - (past - (past / loop) * loop);
    }
    return std::min(room, left);
}

// The file offset of the data that would be written next (GetStreamDataInfo's read position).
u32 read_position(const Atrac& a)
{
    const u8 hold = a.s8(st::kHold);
    if (hold == 4) return ring_next(a.sr(st::kDataEnd), a.sr(st::kRead), a.sr(st::kBytes), a.sr(st::kBufferSize));
    if (hold == 6 && a.sr(st::kLoopEnd) < a.sr(st::kSample)) {
        return ring_next(a.sr(st::kDataEnd), a.sr(st::kBytes) - bytes_held(a) + a.sr(st::kRead), bytes_held(a),
            a.sr(st::kBufferSize));
    }
    if (hold != 5 && hold != 6) return 0;
    const u32 start = frame_before(a, a.sr(st::kLoopStart)), last = frame_last_byte(a, a.sr(st::kLoopEnd));
    if (a.sr(st::kBytes) >= a.sr(st::kBufferSize)) return 0;
    const u32 at = a.sr(st::kRead) + a.sr(st::kBytes);
    if (last + 1 > at) return at;
    const u32 past = at - last - 1, loop = last - start + 1;
    return start + (past - (past / loop) * loop);
}

// Frames a streamed buffer still holds, or the special counts: -2 the rest of a stream without a loop is in the
// buffer, -3 so is a looped one's.
s32 remain_streamed(const Atrac& a)
{
    if (a.sr(st::kDataEnd) - a.sr(st::kRead) <= a.sr(st::kBytes)) return -2;
    const s32 n = static_cast<s32>(a.sr(st::kBytes) / a.frame()) - static_cast<s8>(a.s8(st::kSkip));
    return n & ~(n >> 31);
}

s32 remain_looped(const Atrac& a)
{
    s32 n = 0;
    if (a.frame()) {
        const u32 last = frame_last_byte(a, a.sr(st::kLoopEnd));
        const u32 at = a.sr(st::kBytes) + a.sr(st::kRead);
        if (last + 1 < at) {
            const u32 skip = frames_before(a, a.sr(st::kLoopStart));
            const u32 past = at - last - 1, start = frame_before(a, a.sr(st::kLoopStart));
            const u32 loop = last - start + 1, laps = past / loop;
            u32 frames = laps * (loop / a.frame() - skip) + (last - a.sr(st::kRead) + 1) / a.frame();
            const u32 rest = past - laps * loop;
            if (skip * a.frame() < rest) frames += rest / a.frame() - skip;
            n = static_cast<s32>(frames);
        } else {
            n = static_cast<s32>(a.sr(st::kBytes) / a.frame());
        }
        n -= static_cast<s8>(a.s8(st::kSkip));
        n &= ~(n >> 31);
    }
    if (static_cast<s32>(a.sr(st::kLoops)) >= 0) {
        const u32 last = frame_last_byte(a, a.sr(st::kLoopEnd));
        const u32 at = a.sr(st::kRead) + a.sr(st::kBytes);
        if (last + 1 <= at) {
            const u32 loop = last - frame_before(a, a.sr(st::kLoopStart)) + 1;
            if (static_cast<s32>(a.sr(st::kLoops)) <= static_cast<s32>((at - last - 1) / loop)) n = -3;
        }
    }
    return n;
}

// cellAtracGetRemainFrame's count: -1 when the whole file is in the buffer.
s32 remain(const Atrac& a)
{
    switch (a.s8(st::kHold)) {
    case 2: return -1;
    case 3: {
        const u32 have = a.sr(st::kDataStart) + a.sr(st::kBytes);
        if (have <= a.sr(st::kRead)) return 0;
        const s32 n = static_cast<s32>((have - a.sr(st::kRead)) / a.frame()) - static_cast<s8>(a.s8(st::kSkip));
        return n >= 0 ? n : 0;
    }
    case 4: return remain_streamed(a);
    case 5: return remain_looped(a);
    case 6: return a.sr(st::kSample) <= a.sr(st::kLoopEnd) ? remain_looped(a) : remain_streamed(a);
    default: return 0;
    }
}

// What the game reads for a reset to `sample` (cellAtracGetBufferInfoForResetting's CellAtracBufferInfo: where to write,
// the most, the least, from which file offset).
void reset_info(const Atrac& a, u32 sample, u32 out[4])
{
    const u8 hold = a.s8(st::kHold);
    const u32 frame = a.frame();
    out[0] = a.sr(st::kBuffer);
    if (hold == 4 || hold == 5) {
        const u32 room = whole_steps(0, frame, a.sr(st::kBufferSize));
        const u32 from = frame_before(a, sample);
        out[3] = from;
        out[1] = std::min(a.sr(st::kDataEnd) - from, room);
        out[2] = (frames_before(a, sample) + 1u) * frame;
        return;
    }
    if (hold == 3) {
        const u32 have = a.sr(st::kDataStart) + a.sr(st::kBytes);
        out[0] = a.sr(st::kBuffer) + have;
        out[1] = a.sr(st::kDataEnd) - have;
        out[3] = have;
        const u32 need = a.sr(st::kDataStart) + (sample / a.frame_samples() + 1) * frame;
        out[2] = have <= need ? need - have : 0;
        return;
    }
    if (hold == 6) {
        const u32 from = frame_before(a, sample), last = frame_last_byte(a, a.sr(st::kLoopEnd));
        u32 room = whole_steps(0, frame, a.sr(st::kBufferSize));
        u32 least = (frames_before(a, sample) + 1u) * frame;
        if (from < last) {
            out[3] = from;
            out[1] = std::min(last - from + 1, room);
            out[2] = std::min(last - from + 1, least);
            return;
        }
        const u32 second = whole_steps(0, frame, a.sr(st::kSecondSize));
        if (last + second <= from) {
            out[3] = from;
            out[1] = std::min(a.sr(st::kDataEnd) - from, room);
            out[2] = least;
            return;
        }
        if (last + a.sr(st::kSecondSize) + 1 < a.sr(st::kDataEnd)) {
            const u32 after = last + second + 1;
            out[3] = after;
            out[1] = std::min(a.sr(st::kDataEnd) - after, room);
            out[2] = after <= from + least ? from + least - after : 0;
            return;
        }
    }
    out[0] = a.sr(st::kBuffer);
    out[1] = out[2] = out[3] = 0;
}

// A reset to `sample` with `bytes` of data written as reset_info asked.
s32 reset(const Atrac& a, u32 sample, u32 bytes)
{
    u32 info[4];
    reset_info(a, sample, info);
    if (bytes < info[2] || info[1] < bytes) return a.e(CELL_ATRAC_ERROR_ILLEGAL_RESET_BYTE);
    a.sw(st::kSample, sample);
    a.s8w(st::kSkip, frames_before(a, sample));
    a.sw(st::kLoops, 0);
    a.sw(st::kRead, frame_before(a, sample));
    const u8 hold = a.s8(st::kHold);
    if (hold == 4 || hold == 5 || (hold == 6 && frame_before(a, sample) < frame_last_byte(a, a.sr(st::kLoopEnd)))) {
        a.sw(st::kBytes, bytes);
        a.sw(st::kRing, 0);
        a.s8w(st::kPhase, 0);
    } else if (hold == 3) {
        a.s8w(st::kPhase, 0);
        a.sw(st::kBytes, bytes + a.sr(st::kBytes));
        if (a.sr(st::kDataEnd) <= a.sr(st::kBytes) + a.sr(st::kDataStart)) a.s8w(st::kHold, 2);
    } else if (hold == 6) {
        const u32 from = frame_before(a, sample), last = frame_last_byte(a, a.sr(st::kLoopEnd));
        const u32 second = whole_steps(0, a.frame(), a.sr(st::kSecondSize));
        if (from < last + second) {
            a.sw(st::kBytes, last + second - from + bytes + 1);
            a.s8w(st::kPhase, 1);
            a.sw(st::kSecondPos, from - last - 1);
        } else {
            a.sw(st::kBytes, bytes);
            a.s8w(st::kPhase, 2);
            a.sw(st::kRing, 0);
        }
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------ cellAdec

u64 adec(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    const u32 nid = recomp::nid_of(name);
    if (const u32 opd = HleProcess::current()->imported("cellAdec", nid)) return call_guest(ctx, opd, args);
    if (const Function* f = registry().find("cellAdec", nid)) {
        u32 i = 3;
        for (const u64 v : args) ctx.gpr[i++] = v;
        f->fn(ctx, guest_base(ctx));
        return ctx.gpr[3];
    }
    PS_LOG_ERROR(kChan, "{}: no cellAdec in this process", name);
    return error(CELL_ATRAC_ERROR_API_FAIL);
}

s32 lock(PPUContext& ctx, const Atrac& a) { return static_cast<s32>(lv2_syscall(ctx, 102, {a.r32(hd::kMutex), 0})); }
s32 unlock(PPUContext& ctx, const Atrac& a) { return static_cast<s32>(lv2_syscall(ctx, 104, {a.r32(hd::kMutex)})); }

// The handle's mutex (priority, not recursive, "_a3pm00"; libatrac3multi's "_a3mpm0") and condition ("_a3pc00",
// "_a3mpc0").
s32 make_sync(PPUContext& ctx, const Atrac& a)
{
    u8* b = a.b;
    a.w32(hd::kError, 0);
    StackVar v(ctx, b, 0x40);
    const u32 mattr = v.addr(), cattr = v.addr() + 0x28;
    ps_write32(b, mattr, 2);
    ps_write32(b, mattr + 4, 0x20);
    ps_write32(b, mattr + 8, 0x200);
    ps_write32(b, mattr + 12, 0x2000);
    ps_write64(b, mattr + 16, 0);
    ps_write32(b, mattr + 24, 0);
    std::memcpy(b + mattr + 32, a.L->multi ? "_a3mpm0" : "_a3pm00", 8);
    ps_write32(b, cattr, 0x200);
    ps_write32(b, cattr + 4, 0);
    ps_write64(b, cattr + 8, 0);
    std::memcpy(b + cattr + 16, a.L->multi ? "_a3mpc0" : "_a3pc00", 8);
    s32 r = static_cast<s32>(lv2_syscall(ctx, 100, {a.h + hd::kMutex, mattr}));
    if (r >= 0) {
        a.w32(hd::kFlags, a.r32(hd::kFlags) | 1);
        r = static_cast<s32>(lv2_syscall(ctx, 105, {a.h + hd::kCond, a.r32(hd::kMutex), cattr}));
        if (r >= 0) {
            a.w32(hd::kFlags, a.r32(hd::kFlags) | 2);
            return lv2::CELL_OK;
        }
    }
    s32 c = 0, m = 0;
    if (a.r32(hd::kFlags) & 2) c = static_cast<s32>(lv2_syscall(ctx, 106, {a.r32(hd::kCond)}));
    if (a.r32(hd::kFlags) & 1) m = static_cast<s32>(lv2_syscall(ctx, 101, {a.r32(hd::kMutex)}));
    (void)c;
    (void)m;
    return r;
}

s32 destroy_sync(PPUContext& ctx, const Atrac& a)
{
    s32 c = 0, m = 0;
    u32 flags = a.r32(hd::kFlags);
    if (flags & 2) {
        c = static_cast<s32>(lv2_syscall(ctx, 106, {a.r32(hd::kCond)}));
        flags = a.r32(hd::kFlags);
    }
    if (flags & 1) m = static_cast<s32>(lv2_syscall(ctx, 101, {a.r32(hd::kMutex)}));
    return c ? c : m;
}

// cellAdec's callback (libatrac3plus's): PCM out wakes the decode waiting for it (or is noted for it); an error is
// kept, the first one.
s32 _pspocket_cellAtracAdecCallback(PPUContext& ctx, u32, s32 type, s32 data, u32 handle)
{
    const Atrac a{guest_base(ctx), handle};
    if (type == 2) {
        if (a.r32(hd::kError) == 0) a.w32(hd::kError, static_cast<u32>(data));
        return lv2::CELL_OK;
    }
    if (type != 1) return lv2::CELL_OK;
    s32 r = lock(ctx, a);
    if (r >= 0) {
        const u32 flags = a.r32(hd::kFlags);
        if (flags & hd::kWaiting) {
            a.w32(hd::kFlags, flags & ~hd::kWaiting);
            r = static_cast<s32>(lv2_syscall(ctx, 108, {a.r32(hd::kCond)}));
        } else {
            a.w32(hd::kFlags, flags | hd::kEarly);
            r = 0;
        }
        const s32 u = unlock(ctx, a);
        if (r == 0 && u < 0) r = u;
    }
    const s32 e = static_cast<s32>(a.r32(hd::kError));
    return e < 0 ? e : r;
}

// The decode's wait for PCM (the mutex held).
void wait_pcm(PPUContext& ctx, const Atrac& a)
{
    const u32 flags = a.r32(hd::kFlags);
    if (flags & hd::kEarly) {
        a.w32(hd::kFlags, flags & ~hd::kEarly);
        return;
    }
    a.w32(hd::kFlags, flags | hd::kWaiting);
    lv2_syscall(ctx, 107, {a.r32(hd::kCond), 0});
}

// ------------------------------------------------------------------------------------------------ one frame

// libatrac3multi's frame for cellAdec: for each output track, the block of the file channel its track array entry names
// (the frame's bytes / the file's channels each), or a silent block (zeros, 0xA2 first) for an entry that names none,
// into the buffer at +0x54. cellAdec gets that buffer with the frame's size.
u32 shuffle_tracks(const Atrac& a, u32 src)
{
    const s32 tracks = static_cast<s32>(a.r32(kMultiTracks));
    if (tracks <= 0) return src;
    const s32 file_channels = static_cast<s32>(a.r32(kMultiFileChannels));
    const u32 block = file_channels ? static_cast<u32>(static_cast<s32>(a.r32(hd::kAt3Param + 8)) / file_channels) : 0;
    u32 dst = a.r32(kMultiAuBuffer);
    for (s32 i = 0; i < tracks; i++, dst += block) {
        const s32 t = static_cast<s32>(a.sr(mst::kTracks + 4 * static_cast<u32>(i)));
        if (t >= 0 && t < file_channels) {
            for (u32 k = 0; k < block; k++) a.b[dst + k] = a.b[src + block * static_cast<u32>(t) + k];
        } else {
            std::memset(a.b + dst, 0, block);
            a.b[dst] = 0xA2;
        }
    }
    return a.r32(kMultiAuBuffer);
}

// libatrac3plus's frame step: one frame decoded; its samples (*samples) and whether the play range ended (*finish);
// the PCM into `out` (when not 0 and frames to skip are done). Then the positions, loops and the ring move on.
s32 decode_frame(PPUContext& ctx, const Atrac& a, u32 out, u32* samples, u32* finish)
{
    u8* b = a.b;
    const u32 next = next_samples(a);
    const u32 frame = a.frame();
    if (a.sr(st::kDataEnd) < a.sr(st::kRead) + frame || a.sr(st::kSample) > a.sr(st::kEnd)) {
        *finish = 1;
        return a.e(CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED);
    }
    const u8 hold = a.s8(st::kHold);
    const bool streamed = hold & 4;
    if ((streamed && a.sr(st::kBytes) < frame) ||
        (hold == 3 && a.sr(st::kDataStart) + a.sr(st::kBytes) < a.sr(st::kRead) + frame)) {
        *finish = 0;
        return a.e(CELL_ATRAC_ERROR_NODATA_IN_BUFFER);
    }
    const bool whole = a.frame_samples() == next;
    u32 pcm = a.work();
    if (whole && out) pcm = out;
    u32 au_start;
    if (streamed) {
        const bool second = a.s8(st::kPhase) & 1;
        au_start = a.sr(second ? st::kSecond : st::kBuffer) + a.sr(second ? st::kSecondPos : st::kRing);
    } else {
        au_start = a.sr(st::kRead) + a.sr(st::kBuffer);
    }
    if (a.L->multi) au_start = shuffle_tracks(a, au_start);
    StackVar au(ctx, b, 0x18);
    ps_write32(b, au.addr(), au_start);
    ps_write32(b, au.addr() + 4, a.r32(a.codec() == kAt3 ? hd::kAt3Param + 8 : hd::kAtxParam + 0xC));
    ps_write32(b, au.addr() + 8, 0xFFFFFFFF);
    ps_write32(b, au.addr() + 12, 0xFFFFFFFF);
    ps_write64(b, au.addr() + 16, 0);
    if (lock(ctx, a) < 0) {
        *finish = 0;
        return a.e(CELL_ATRAC_ERROR_API_FAIL);
    }
    s32 r = static_cast<s32>(adec(ctx, "cellAdecDecodeAu", {a.r32(hd::kAdec), au.addr()}));
    if (r >= 0) {
        wait_pcm(ctx, a);
        if (a.r32(hd::kError) != 0) {
            r = a.e(CELL_ATRAC_ERROR_API_FAIL);
        } else {
            const s32 got = static_cast<s32>(adec(ctx, "cellAdecGetPcm", {a.r32(hd::kAdec), pcm}));
            const s32 u = unlock(ctx, a);
            if (got < 0 || u < 0) {
                *finish = 0;
                return a.e(CELL_ATRAC_ERROR_API_FAIL);
            }
            r = u;
            goto decoded;
        }
    }
    unlock(ctx, a);
    *finish = 0;
    return a.e(CELL_ATRAC_ERROR_API_FAIL);
decoded:
    *samples = next;
    *finish = a.sr(st::kSample) + next > a.sr(st::kEnd) ? (a.sr(st::kLoops) == 0 ? 1u : 0u) : 0u;
    a.sw(st::kRead, a.sr(st::kRead) + frame);
    if (const u8 skip = a.s8(st::kSkip)) {
        a.s8w(st::kSkip, static_cast<u8>(skip - 1));
    } else {
        if (!whole && next && out) { // the kept part of the frame, from the work memory
            const u32 ch = a.s8(st::kChannels), stride = ch * 4;
            const u32 from = pcm + (a.sample_mask() & a.sr(st::kSample)) * stride;
            std::memmove(b + out, b + from, next * stride);
        }
        const u32 pos = a.sr(st::kSample) + next;
        a.sw(st::kSample, pos);
        if (a.sr(st::kLoopEnd) && a.sr(st::kLoops) && pos > a.sr(st::kLoopEnd)) {
            a.sw(st::kRead, frame_before(a, a.sr(st::kLoopStart)));
            a.s8w(st::kSkip, frames_before(a, a.sr(st::kLoopStart)));
            a.sw(st::kSample, a.sr(st::kLoopStart));
            if (static_cast<s32>(a.sr(st::kLoops)) > 0) a.sw(st::kLoops, a.sr(st::kLoops) - 1);
        }
    }
    if (!streamed) return r;
    a.sw(st::kBytes, a.sr(st::kBytes) - frame);
    if (a.s8(st::kPhase) == 1) {
        u32 at = frame + a.sr(st::kSecondPos);
        if (frame + at > a.sr(st::kSecondSize)) {
            a.s8w(st::kPhase, 2);
            a.sw(st::kRing, 0);
            at = 0;
        }
        a.sw(st::kSecondPos, at);
        return r;
    }
    u32 ring = frame + a.sr(st::kRing);
    if (frame + ring > a.sr(st::kBufferSize)) ring = 0;
    a.sw(st::kRing, ring);
    const u32 loop_end = a.sr(st::kLoopEnd);
    if (loop_end && (a.sr(st::kLoops) || a.sr(st::kSample) <= loop_end)) return r;
    if (a.s8(st::kHold) != 6 || a.s8(st::kPhase) != 0) return r;
    if (a.sr(st::kRead) <= frame_last_byte(a, loop_end)) return r;
    // Past the loop's tail: the frames come from the second buffer; the part of its last frame that does not fill a
    // whole one goes to the front of the first.
    a.s8w(st::kPhase, 1);
    a.sw(st::kSecondPos, 0);
    const u32 size = a.sr(st::kSecondSize);
    a.sw(st::kBytes, size);
    const u32 rest = size % frame;
    if (rest) std::memmove(b + a.sr(st::kBuffer), b + a.sr(st::kSecond) + (size - rest), rest);
    return r;
}

// The frames before the first kept sample.
s32 preroll(PPUContext& ctx, const Atrac& a)
{
    for (s32 n = static_cast<s8>(a.s8(st::kSkip)); n > 0; n--) {
        u32 samples = 0, finish = 0;
        if (const s32 r = decode_frame(ctx, a, 0, &samples, &finish); r != lv2::CELL_OK) return r;
    }
    return lv2::CELL_OK;
}

// Bytes of PCM memory one frame needs ahead of cellAdec's: 4 KB per channel for ATRAC3, 8 KB per channel for
// ATRAC3plus's 1, 2, 6 or 8 channels, else 64 KB.
u32 pcm_bytes(u16 codec, u8 channels)
{
    if (codec == kAt3) return (channels & 0xFFFFFu) << 12;
    if (channels < 9 && ((0x146u >> channels) & 1)) return (channels & 0x7FFFFu) << 13;
    return 0x10000;
}

// ------------------------------------------------------------------------------------------------ the header

// Reads `n` bytes little-endian at *at when *at is inside `size`.
u32 le(const u8* p, u32& at, u32 size, u32 n)
{
    u32 v = 0;
    if (at < size) {
        for (u32 i = 0; i < n; i++) v |= u32{p[at + i]} << (8 * i);
        at += n;
    }
    return v;
}

bool rate_ok(u32 rate) { return rate == 44100 || rate == 48000; }

// The RIFF WAVE header (libatrac3plus's reader): what it found into `info` (as libatrac3plus lays it out: +0 codec,
// +2 channels, +4 rate, +8 bytes per second, +0xC block align, +0xE bits, +0x10 extra size, +0x12 valid bits, +0x14
// channel mask (ATRAC3: joint stereo), +0x18 the subformat GUID, +0x28 ATRAC3plus' version, +0x2A its two config bytes,
// +0x2C eight more; +0x34 the data's offset, +0x38 the samples (fact), +0x3C the data's size, +0x40 the first sample's
// offset, +0x44/+0x48 the loop (smpl)), and the codec (0x1000/0x1001) or an error. A chunk is read field by field from
// its start, the rest of it (its size rounded to even) skipped.
u32 parse_header(const u8* p, u32 size, u8* info, bool multi = false)
{
    const auto put16 = [&](u32 off, u32 v) { ps_st16(info + off, static_cast<u16>(v)); };
    const auto put32 = [&](u32 off, u32 v) { ps_st32(info + off, v); };
    const auto get32 = [&](u32 off) { return ps_ld32(info + off); };
    std::memset(info, 0, 0x50);
    put32(0x48, 0xFFFFFFFF);
    put32(0x44, 0xFFFFFFFF);
    if (size <= 0xB) return CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
    u32 at = 0;
    const auto rd = [&](u32 n) { return le(p, at, size, n); };
    for (u32 riff = 0;; riff = at) {
        if (rd(4) != 0x46464952) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT; // RIFF
        const u32 len = rd(4);
        if (rd(4) == 0x45564157) break; // WAVE
        at += ((len - 4) & 1) + (len - 4);
        // pspocket's (D-646): a length the 32-bit sum wraps back to this header, or before it, is refused;
        // libatrac3plus's reader read the same headers again for ever.
        if (at <= riff || size < at || size < at + 0xC) return CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
    }
    if (at + 8 >= size) return CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
    static constexpr u8 kGuid[16] = {0xE9, 0x23, 0xAA, 0xBF, 0xCB, 0x58, 0x44, 0x71,
                                     0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62}; // ATRAC3plus' subformat
    u32 codec = CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
    bool fact_offset = false; // the fact chunk's second word, the first sample's offset
    for (;;) {
        const u32 id = rd(4);
        const u32 len = rd(4);
        u32 left = len + (len & 1);
        // A chunk past the bytes read. Checked without the 32-bit sum (pspocket's, D-646): libatrac3plus's wraps for
        // a length near 4 GB, which then took the reader back to a chunk before (read again for ever) or let a fmt
        // chunk be read past the bytes.
        if (left > size - at && id != 0x61746164) return CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
        if (id == 0x61746164) { // data
            put32(0x3C, left);
            put32(0x34, at);
            if (get32(0x40) == 0) {
                const u32 x = codec ^ 0x1001, m = static_cast<u32>(static_cast<s32>(x) >> 31);
                put32(0x40, (std::rotl(m - (m ^ x), 11) & 0x400) + 0x400); // 0x400 for ATRAC3, else 0x800
            }
            if (fact_offset && codec == kAt3Plus) {
                put32(0x40, get32(0x40) + 0xB8);
                if (get32(0x48) != 0xFFFFFFFF) {
                    put32(0x48, get32(0x48) + 0xB8);
                    put32(0x44, get32(0x44) + 0xB8);
                }
            }
            return codec;
        }
        if (id == 0x20746D66) { // fmt
            if (codec != CELL_ATRAC_ERROR_UNKNOWN_FORMAT) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
            const u32 tag = rd(2);
            if (tag == 0x270) {
                if (left <= 31) return codec;
                put16(0, kAt3);
                put16(2, rd(2));
                const u32 rate = rd(4);
                put32(4, rate);
                if (!rate_ok(rate)) return CELL_ATRAC_ERROR_ILLEGAL_SAMPLING_RATE;
                put32(8, rd(4));
                put16(0xC, rd(2));
                put16(0xE, rd(2));
                put16(0x10, rd(2));
                if (rd(2) != 1) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
                rd(4);
                const u32 js = rd(2);
                put32(0x14, js);
                if (multi && js != 0) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT; // libatrac3multi: no joint stereo
                if (rd(2) != js) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
                if (rd(2) != 1) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
                if (rd(2) != 0) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
                codec = kAt3;
                left -= 0x20;
            } else if (tag == 0xFFFE && !multi) {
                if (left <= 0x33) return codec;
                put16(0, kAt3Plus);
                put16(2, rd(2));
                const u32 rate = rd(4);
                put32(4, rate);
                if (!rate_ok(rate)) return CELL_ATRAC_ERROR_ILLEGAL_SAMPLING_RATE;
                put32(8, rd(4));
                put16(0xC, rd(2));
                put16(0xE, rd(2));
                put16(0x10, rd(2));
                const u32 valid = rd(2);
                put32(0x14, rd(4));
                put32(0x18, rd(4));
                put16(0x1C, rd(2));
                put16(0x1E, rd(2));
                std::memcpy(info + 0x20, p + at, 8);
                at += 8;
                if (get32(0x18) != 0xE923AABF || std::memcmp(info + 0x1C, kGuid + 4, 4) != 0 ||
                    std::memcmp(info + 0x20, kGuid + 8, 8) != 0)
                    return codec;
                put16(0x12, valid);
                put16(0x28, rd(2));
                info[0x2A] = p[at];
                info[0x2B] = p[at + 1];
                at += 2;
                std::memcpy(info + 0x2C, p + at, 8);
                at += 8;
                left -= 0x34;
                codec = kAt3Plus;
            } else {
                return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
            }
        } else if (id == 0x6C706D73 && static_cast<s32>(get32(0x44)) < 0) { // smpl, the first loop
            if (left <= 31) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
            at += 28;
            const u32 loops = rd(4);
            left -= 32;
            if (loops) {
                if (left <= 19) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
                at += 12;
                put32(0x44, rd(4));
                put32(0x48, rd(4));
                if (static_cast<s32>(get32(0x44)) > static_cast<s32>(get32(0x48))) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
                left -= 20;
            }
        } else if (id == 0x74636166) { // fact
            if (left <= 3) return CELL_ATRAC_ERROR_UNKNOWN_FORMAT;
            left -= 4;
            put32(0x38, rd(4));
            if (left > 7) {
                left -= 8;
                rd(4);
                put32(0x40, rd(4));
            } else if (left > 3) {
                left -= 4;
                put32(0x40, rd(4));
                fact_offset = true;
            }
        }
        const u32 next = at + left;
        if (size < next || next + 8 >= size) return CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
        at = next;
    }
}

// The stream state from the header (libatrac3plus's): play range, loop, frames before the first sample, how the data
// is held.
u32 set_stream(const Atrac& a, u32 buffer, u32 read, u32 buffer_size, const u8* info, u32 tracks = 0,
    u32 track_array = 0)
{
    const u16 codec = a.codec();
    const u32 delay = codec == kAt3 ? 0x45 : 0xB8;
    a.sw(st::kOffset, ps_ld32(info + 0x40) + delay);
    if (a.L->multi) { // the output tracks: the game's array, or the file's channels in order
        for (u32 i = 0; i < tracks; i++)
            a.sw(mst::kTracks + 4 * i, track_array ? ps_read32(a.b, track_array + 4 * i) : i);
        a.s8w(st::kChannels, static_cast<u8>(tracks));
        a.sw(mst::kFileChannels, ps_ld16(info + 2));
    } else {
        a.s8w(st::kChannels, static_cast<u8>(ps_ld16(info + 2)));
    }
    ps_write16(a.b, a.s() + st::kFrame, ps_ld16(info + 0xC));
    a.sw(st::kRate, ps_ld32(info + 4));
    const u32 frame = a.frame();
    if (!frame) return CELL_ATRAC_ERROR_ILLEGAL_DATA;
    a.sw(0, kMagic);
    const u32 per_frame = a.frame_samples(), first = ps_ld32(info + 0x40);
    const s32 loop_start = static_cast<s32>(ps_ld32(info + 0x44));
    if (ps_ld32(info + 0x38) == 0) a.sw(st::kEnd, (ps_ld32(info + 0x3C) / frame) * per_frame - 1);
    else a.sw(st::kEnd, ps_ld32(info + 0x38) + first + delay - 1);
    a.sw(st::kSample, first + delay);
    a.sw(st::kLoops, 0);
    a.s8w(st::kSkip, static_cast<u8>((first + delay) / per_frame));
    if (loop_start < 0) {
        a.sw(st::kLoopEnd, 0);
        a.sw(st::kLoopStart, 0);
    } else {
        a.sw(st::kLoopEnd, ps_ld32(info + 0x48) + delay);
        a.sw(st::kLoopStart, static_cast<u32>(loop_start) + delay);
    }
    const u32 end = a.sr(st::kEnd), data = ps_ld32(info + 0x34), data_end = ps_ld32(info + 0x3C) + data;
    a.sw(st::kBytes, read - data);
    a.s8w(st::kPhase, 0);
    a.sw(st::kBuffer, buffer);
    a.sw(st::kRead, data);
    a.sw(st::kDataStart, data);
    a.sw(st::kDataEnd, data_end);
    a.sw(st::kBufferSize, buffer_size);
    a.sw(st::kRing, data);
    const u32 loop_end = a.sr(st::kLoopEnd);
    if (!(a.sr(st::kLoopStart) < end && loop_end <= end && (end / per_frame) * frame < data_end - data))
        return CELL_ATRAC_ERROR_ILLEGAL_DATA;
    if (data_end <= buffer_size) {
        a.s8w(st::kHold, data_end <= read ? 2 : 3);
        return 0;
    }
    if (a.sr(st::kBytes) < frame * 3) return CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL;
    if (loop_end == 0) {
        a.s8w(st::kHold, 4);
        return 0;
    }
    if (end != loop_end) {
        const u32 bytes = frame_last_byte(a, loop_end) - a.sr(st::kDataStart) + 1;
        a.s8w(st::kHold, 6);
        if (bytes < a.sr(st::kBytes)) a.sw(st::kBytes, bytes);
        a.sw(st::kSecondPos, 0);
        a.sw(st::kSecond, 0);
        a.sw(st::kSecondSize, 0);
        return 0;
    }
    a.s8w(st::kHold, 5);
    return 0;
}

// ------------------------------------------------------------------------------------------------ the calls

// ATRAC3plus's bit rates by format (codec, rate, frame bytes: kbps), libatrac3plus's table; others computed.
struct Bitrate {
    u16 codec;
    u32 rate;
    u16 frame, kbps;
};
constexpr std::array<Bitrate, 8> kBitrates = {{{kAt3, 44100, 0x130, 105}, {kAt3Plus, 44100, 0xC0, 32},
    {kAt3Plus, 44100, 0x3A8, 160}, {kAt3Plus, 44100, 0x748, 320}, {kAt3Plus, 48000, 0xB0, 32},
    {kAt3Plus, 48000, 0x2B0, 128}, {kAt3Plus, 48000, 0x6B0, 320}, {kAt3Plus, 48000, 0xAB0, 512}}};

// The handle and the pointers a call writes, as guest memory.
bool args_ok(PPUContext& ctx, const Layout& L, std::string_view fn, u32 h, std::initializer_list<u32> outs, s32& r)
{
    if (!handle_ok(ctx, h)) {
        r = not_guest(ctx, L, fn, "handle", h);
        return false;
    }
    for (const u32 o : outs) {
        if (!guest_readable(thread_of(ctx), o, 4)) {
            r = not_guest(ctx, L, fn, "result", o);
            return false;
        }
    }
    return true;
}

#define ATRAC_ARGS(h, ...)                                                                   \
    s32 bad_ = 0;                                                                            \
    if (!args_ok(ctx, L, __func__, h, {__VA_ARGS__}, bad_)) return bad_;                     \
    const Atrac a{guest_base(ctx), h, &L}

s32 cellAtracSetDataAndGetMemSize_impl(PPUContext& ctx, const Layout& L, u32 h, u32 buffer, u32 read, u32 buffer_size, u32 mem_size)
{
    ATRAC_ARGS(h, mem_size);
    if (read > buffer_size) return a.e(CELL_ATRAC_ERROR_READSIZE_OVER_BUFFER);
    if (!guest_readable(thread_of(ctx), buffer, std::max<u32>(read, 1))) return not_guest(ctx, L, __func__, "buffer", buffer);
    u8* b = a.b;
    std::memset(b + h, 0, 0x200);
    a.w32(hd::kResource + 4, 0);
    a.set_work(0);
    u8 info[0x50];
    const u32 codec = parse_header(b + buffer, read, info);
    if (static_cast<s32>(codec) < 0) return a.e(codec);
    ps_write16(b, a.s() + st::kCodec, static_cast<u16>(codec));
    if (codec == kAt3) {
        a.w32(hd::kType, 5); // CELL_ADEC_TYPE_ATRAC3
        a.w32(hd::kAt3Param, ps_ld16(info + 2));
        a.w32(hd::kAt3Param + 4, ps_ld32(info + 0x14));
        a.w32(hd::kAt3Param + 8, ps_ld16(info + 0xC));
        a.w32(hd::kAt3Param + 0xC, 0x84);
    } else {
        const u16 ch = ps_ld16(info + 2);
        a.w32(hd::kType, ch == 6 ? 0xE : ch == 8 ? 0xF : (ch == 1 || ch == 2) ? 0xD : 3);
        const u8 c0 = info[0x2A], c1 = info[0x2B];
        const u32 p = hd::kAtxParam;
        a.w32(p + 0xC, ((((c0 & 3u) << 8) | c1) + 1) * 8);
        const u32 cfg = (c0 >> 2) & 7;
        a.w32(p + 4, cfg);
        a.w32(p + 8, cfg > 4 ? cfg + 1 : cfg);
        static constexpr u32 kRates[5] = {32000, 44100, 48000, 88200, 96000};
        const u32 r = c0 >> 5;
        if (r < 5) a.w32(p, kRates[r]);
        for (u32 i = 0; i < 4; i++) b[h + p + 0x10 + i] = info[0x2C + i];
        b[h + p + 0x19] = 0;
        b[h + p + 0x18] = 0;
        a.w32(p + 0x14, 0x84);
        if (r >= 5) return a.e(CELL_ATRAC_ERROR_ILLEGAL_SAMPLING_RATE);
    }
    if (const u32 e = set_stream(a, buffer, read, buffer_size, info); e) return a.e(e);
    ps_write32(b, mem_size, 0);
    const s32 r = static_cast<s32>(adec(ctx, "cellAdecQueryAttr", {h + hd::kType, h + hd::kAttr}));
    if (r < 0) return r;
    ps_write32(b, mem_size, pcm_bytes(a.codec(), a.s8(st::kChannels)) + a.r32(hd::kAttr) + 0x400);
    return r;
}

// cellAtracDeleteDecoder's body: the sync objects destroyed (as flagged: a second time fails, and the decoder stays),
// cellAdec closed, the handle unset.
s32 delete_decoder(PPUContext& ctx, const Atrac& a)
{
    if (const s32 e = check(a); e) return e;
    s32 r = destroy_sync(ctx, a);
    if (r >= 0) {
        r = static_cast<s32>(adec(ctx, "cellAdecClose", {a.r32(hd::kAdec)}));
        a.s8w(st::kHold, 0);
        a.sw(0, 0);
        a.w32(hd::kResource + 4, 0);
        a.set_work(0);
    }
    return r;
}

// Create and CreateExt's common part: the sync objects, cellAdec opened (`open` does it), the sequence started, the
// frames before the first sample decoded. On an error the decoder is deleted again (but for DECODER_WAS_CREATED).
template <typename Open>
s32 create(PPUContext& ctx, const Atrac& a, u32 work, Open&& open)
{
    s32 r = make_sync(ctx, a);
    if (r >= 0) {
        a.set_work(work);
        r = open();
        if (r < 0) {
            destroy_sync(ctx, a);
        } else {
            const u32 param = a.codec() == kAt3 ? hd::kAt3Param : hd::kAtxParam;
            r = static_cast<s32>(adec(ctx, "cellAdecStartSeq", {a.r32(hd::kAdec), a.h + param}));
            if (r >= 0) r = preroll(ctx, a);
        }
    }
    if (a.s8(st::kHold) & 4) { // streamed: the bytes past the frames move to the buffer's front
        const u32 from = whole_steps(a.sr(st::kDataStart), a.frame(), a.sr(st::kBufferSize));
        const s32 n = static_cast<s32>(a.sr(st::kRing) + a.sr(st::kBytes) - from);
        if (n > 0) std::memmove(a.b + a.sr(st::kBuffer), a.b + a.sr(st::kBuffer) + from, static_cast<u32>(n));
    }
    if (r < 0 && r != a.e(CELL_ATRAC_ERROR_DECODER_WAS_CREATED)) delete_decoder(ctx, a);
    return r;
}

// Where cellAdec's memory starts in the work memory: after the PCM of a frame. libatrac3multi first places the frame's
// buffer for the shuffled tracks there (the tracks' 256 bytes each), then starts cellAdec's memory after the PCM's size
// twice and that buffer: past what SetDataAndGetMemSize asked for by the PCM's size less 0x400, as Sony's module does.
u32 cellAdec_memory(const Atrac& a, u32 work, u32 pcm)
{
    if (!a.L->multi) return work + pcm;
    a.w32(kMultiAuBuffer, work + pcm);
    return work + pcm + pcm + ((u32{a.s8(st::kChannels)} & 0xFFFFFFu) << 8);
}

u32 callback_descriptor(const Layout& L)
{
    if (L.multi) {
        return HleProcess::current()->link_function("cellAtracMulti",
            recomp::nid_of("_pspocket_cellAtracMultiAdecCallback"), "_pspocket_cellAtracMultiAdecCallback");
    }
    return HleProcess::current()->link_function("cellAtrac", recomp::nid_of("_pspocket_cellAtracAdecCallback"),
        "_pspocket_cellAtracAdecCallback");
}

s32 cellAtracCreateDecoder_impl(PPUContext& ctx, const Layout& L, u32 h, u32 work, u32 ppu_priority, u32 spu_priority)
{
    ATRAC_ARGS(h);
    if (!a.ready()) return a.e(CELL_ATRAC_ERROR_UNSET_DATA);
    if (a.work()) return a.e(CELL_ATRAC_ERROR_DECODER_WAS_CREATED);
    if (ppu_priority > 3071) return a.e(CELL_ATRAC_ERROR_ILLEGAL_PPU_THREAD_PRIORITY);
    if (spu_priority > 255) return a.e(CELL_ATRAC_ERROR_ILLEGAL_SPU_THREAD_PRIORITY);
    return create(ctx, a, work, [&] {
        const u32 pcm = pcm_bytes(a.codec(), a.s8(st::kChannels));
        a.w32(hd::kResource, a.r32(hd::kAttr));
        a.w32(hd::kResource + 4, cellAdec_memory(a, work, pcm));
        a.w32(hd::kResource + 8, ppu_priority);
        a.w32(hd::kResource + 0xC, spu_priority);
        a.w32(hd::kResource + 0x10, 0x4000);
        a.w32(hd::kCb, callback_descriptor(L));
        a.w32(hd::kCb + 4, h);
        return static_cast<s32>(
            adec(ctx, "cellAdecOpen", {h + hd::kType, h + hd::kResource, h + hd::kCb, h + hd::kAdec}));
    });
}

s32 cellAtracCreateDecoderExt_impl(PPUContext& ctx, const Layout& L, u32 h, u32 work, u32 ppu_priority, u32 ext)
{
    ATRAC_ARGS(h);
    if (!a.ready()) return a.e(CELL_ATRAC_ERROR_UNSET_DATA);
    if (a.work()) return a.e(CELL_ATRAC_ERROR_DECODER_WAS_CREATED);
    if (ppu_priority > 3071) return a.e(CELL_ATRAC_ERROR_ILLEGAL_PPU_THREAD_PRIORITY);
    if (!guest_readable(thread_of(ctx), ext, 12)) return not_guest(ctx, L, __func__, "resource", ext);
    return create(ctx, a, work, [&] {
        u8* b = a.b;
        StackVar res(ctx, b, 0x20);
        ps_write32(b, res.addr(), a.r32(hd::kAttr));
        ps_write32(b, res.addr() + 4, cellAdec_memory(a, work, pcm_bytes(a.codec(), a.s8(st::kChannels))));
        ps_write32(b, res.addr() + 8, ppu_priority);
        ps_write32(b, res.addr() + 0xC, 0x4000);
        ps_write32(b, res.addr() + 0x10, ps_read32(b, ext));
        std::memcpy(b + res.addr() + 0x14, b + ext + 4, 8);
        a.w32(hd::kCb, callback_descriptor(L));
        a.w32(hd::kCb + 4, h);
        ps_write32(b, res.addr() + 0x1C, 1);
        return static_cast<s32>(adec(ctx, "cellAdecOpenExt", {h + hd::kType, res.addr(), h + hd::kCb, h + hd::kAdec}));
    });
}

s32 cellAtracDeleteDecoder_impl(PPUContext& ctx, const Layout& L, u32 h)
{
    ATRAC_ARGS(h);
    return delete_decoder(ctx, a);
}

s32 cellAtracDecode_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out, u32 samples, u32 finish, u32 remain_out)
{
    ATRAC_ARGS(h, samples, finish, remain_out);
    if (const s32 e = check(a); e) return e;
    if (out & 3) return a.e(CELL_ATRAC_ERROR_NOT_ALIGNED_OUT_BUFFER);
    if (a.s8(st::kHold) == 6 && !a.sr(st::kSecondSize)) return a.e(CELL_ATRAC_ERROR_NEED_SECOND_BUFFER);
    s32 r = 0;
    for (s32 n = -static_cast<s8>(a.s8(st::kSkip)); n <= 0; n++) {
        u32 got = 0, done = 0;
        r = decode_frame(ctx, a, out, &got, &done);
        ps_write32(a.b, samples, got);
        ps_write32(a.b, finish, done);
        if (r != 0) {
            ps_write32(a.b, samples, 0);
            break;
        }
    }
    ps_write32(a.b, remain_out, static_cast<u32>(remain(a)));
    return r;
}

s32 cellAtracGetStreamDataInfo_impl(PPUContext& ctx, const Layout& L, u32 h, u32 write_at, u32 size, u32 read_at)
{
    ATRAC_ARGS(h, write_at, size, read_at);
    if (const s32 e = check(a); e) return e;
    u8* b = a.b;
    const u8 hold = a.s8(st::kHold);
    if (hold == 2) {
        ps_write32(b, size, 0);
        ps_write32(b, write_at, a.sr(st::kBuffer));
        ps_write32(b, read_at, 0);
    } else if (hold == 3) {
        const u32 have = a.sr(st::kDataStart) + a.sr(st::kBytes);
        ps_write32(b, size, a.sr(st::kDataEnd) - have);
        ps_write32(b, write_at, a.sr(st::kBuffer) + have);
        ps_write32(b, read_at, have);
    } else {
        ps_write32(b, read_at, read_position(a));
        ps_write32(b, write_at, write_offset(a) + a.sr(st::kBuffer));
        ps_write32(b, size, writable(a));
    }
    return lv2::CELL_OK;
}

s32 cellAtracAddStreamData_impl(PPUContext& ctx, const Layout& L, u32 h, u32 bytes)
{
    ATRAC_ARGS(h);
    if (!a.ready()) return a.e(CELL_ATRAC_ERROR_UNSET_DATA);
    if (!a.work()) return a.e(CELL_ATRAC_ERROR_NO_DECODER);
    const u8 hold = a.s8(st::kHold);
    if (hold == 2) return a.e(CELL_ATRAC_ERROR_ALLDATA_IS_ONMEMORY);
    u32 limit;
    if (hold == 3) {
        limit = a.sr(st::kDataEnd) - a.sr(st::kDataStart) - a.sr(st::kBytes);
        if (bytes == limit) {
            a.s8w(st::kHold, 2);
            a.sw(st::kBytes, bytes + a.sr(st::kBytes));
            return lv2::CELL_OK;
        }
    } else {
        limit = writable(a);
    }
    if (limit < bytes) return a.e(CELL_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG);
    a.sw(st::kBytes, bytes + a.sr(st::kBytes));
    return lv2::CELL_OK;
}

s32 cellAtracGetRemainFrame_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, static_cast<u32>(remain(a)));
    return lv2::CELL_OK;
}

s32 cellAtracGetVacantSize_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, vacant(a));
    return lv2::CELL_OK;
}

s32 cellAtracIsSecondBufferNeeded_impl(PPUContext& ctx, const Layout& L, u32 h)
{
    ATRAC_ARGS(h);
    if (const s32 e = check(a); e) return e;
    return a.s8(st::kHold) == 6 ? 1 : 0;
}

s32 cellAtracGetSecondBufferInfo_impl(PPUContext& ctx, const Layout& L, u32 h, u32 at, u32 size)
{
    ATRAC_ARGS(h, at, size);
    if (const s32 e = check(a); e) return e;
    if (a.s8(st::kHold) != 6) {
        ps_write32(a.b, at, 0);
        ps_write32(a.b, size, 0);
        return a.e(CELL_ATRAC_ERROR_NONEED_SECOND_BUFFER);
    }
    const u32 from = frame_last_byte(a, a.sr(st::kLoopEnd)) + 1;
    ps_write32(a.b, at, from);
    ps_write32(a.b, size, a.sr(st::kDataEnd) - from);
    return lv2::CELL_OK;
}

s32 cellAtracSetSecondBuffer_impl(PPUContext& ctx, const Layout& L, u32 h, u32 buffer, u32 size)
{
    ATRAC_ARGS(h);
    if (const s32 e = check(a); e) return e;
    const u32 last = frame_last_byte(a, a.sr(st::kLoopEnd));
    if (!(a.frame() * 3 <= size || a.sr(st::kDataEnd) - last - 1 <= size)) return a.e(CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL);
    if (a.s8(st::kHold) != 6) return a.e(CELL_ATRAC_ERROR_NONEED_SECOND_BUFFER);
    a.sw(st::kSecondPos, 0);
    a.sw(st::kSecond, buffer);
    a.sw(st::kSecondSize, size);
    return lv2::CELL_OK;
}

s32 cellAtracGetChannel_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, a.s8(st::kChannels));
    return lv2::CELL_OK;
}

s32 cellAtracGetMaxSample_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, a.frame_samples());
    return lv2::CELL_OK;
}

s32 cellAtracGetNextSample_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, next_samples(a));
    return lv2::CELL_OK;
}

s32 cellAtracGetSoundInfo_impl(PPUContext& ctx, const Layout& L, u32 h, u32 end, u32 loop_start, u32 loop_end)
{
    ATRAC_ARGS(h, end, loop_start, loop_end);
    if (const s32 e = check(a); e) return e;
    const u32 offset = a.sr(st::kOffset);
    ps_write32(a.b, end, a.sr(st::kEnd) - offset);
    if (!a.sr(st::kLoopEnd)) {
        ps_write32(a.b, loop_start, 0xFFFFFFFF);
        ps_write32(a.b, loop_end, 0xFFFFFFFF);
    } else {
        ps_write32(a.b, loop_start, a.sr(st::kLoopStart) - offset);
        ps_write32(a.b, loop_end, a.sr(st::kLoopEnd) - offset);
    }
    return lv2::CELL_OK;
}

s32 cellAtracGetNextDecodePosition_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    if (a.frame() > a.sr(st::kDataEnd) - a.sr(st::kRead) || a.sr(st::kSample) > a.sr(st::kEnd))
        return a.e(CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED);
    ps_write32(a.b, out, a.sr(st::kSample) - a.sr(st::kOffset));
    return lv2::CELL_OK;
}

s32 cellAtracGetSamplingRate_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, a.sr(st::kRate));
    return lv2::CELL_OK;
}

s32 cellAtracGetBitrate_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    u32 kbps = 0;
    bool found = false;
    for (u32 i = kBitrates.size(); i-- > 0;) {
        const Bitrate& r = kBitrates[i];
        if (r.codec == a.codec() && r.rate == a.sr(st::kRate) && r.frame == a.frame()) {
            kbps = r.kbps;
            found = true;
            break;
        }
    }
    if (!found) kbps = static_cast<u32>(static_cast<u32>(a.frame() * (a.sr(st::kRate) << 3)) / 1000) >> (a.frame_shift() & 0x3F);
    ps_write32(a.b, out, kbps);
    return lv2::CELL_OK;
}

s32 cellAtracGetLoopInfo_impl(PPUContext& ctx, const Layout& L, u32 h, u32 loops, u32 status)
{
    ATRAC_ARGS(h, loops, status);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, loops, a.sr(st::kLoops));
    u32 s = 0;
    if (a.sr(st::kLoopEnd) && (s = 1, a.sr(st::kLoops) == 0)) s = a.sr(st::kSample) <= a.sr(st::kLoopEnd) ? 1 : 0;
    ps_write32(a.b, status, s);
    return lv2::CELL_OK;
}

s32 cellAtracSetLoopNum_impl(PPUContext& ctx, const Layout& L, u32 h, s32 loops)
{
    ATRAC_ARGS(h);
    if (const s32 e = check(a); e) return e;
    if (!a.sr(st::kLoopEnd) || (a.sr(st::kLoops) == 0 && a.sr(st::kSample) > a.sr(st::kLoopEnd)))
        return a.e(CELL_ATRAC_ERROR_UNSET_LOOP_NUM);
    a.sw(st::kLoops, static_cast<u32>(loops));
    return lv2::CELL_OK;
}

s32 cellAtracGetInternalErrorInfo_impl(PPUContext& ctx, const Layout& L, u32 h, u32 out)
{
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    ps_write32(a.b, out, a.r32(hd::kError));
    return lv2::CELL_OK;
}

s32 cellAtracResetPlayPosition_impl(PPUContext& ctx, const Layout& L, u32 h, u32 sample, u32 bytes, u32 tracks = 0)
{
    ATRAC_ARGS(h);
    if (const s32 e = check(a); e) return e;
    if (a.s8(st::kHold) == 6 && !a.sr(st::kSecondSize)) return a.e(CELL_ATRAC_ERROR_NEED_SECOND_BUFFER);
    const u32 s = sample + a.sr(st::kOffset);
    if (s > a.sr(st::kEnd)) return a.e(CELL_ATRAC_ERROR_ILLEGAL_SAMPLE);
    if (tracks && a.s8(st::kChannels)) { // libatrac3multi: a new track array
        const u32 n = a.s8(st::kChannels);
        if (!guest_readable(thread_of(ctx), tracks, 4 * n)) return not_guest(ctx, L, __func__, "track array", tracks);
        for (u32 i = 0; i < n; i++) a.sw(mst::kTracks + 4 * i, ps_read32(a.b, tracks + 4 * i));
    }
    if (const s32 r = reset(a, s, bytes); r < 0) return r;
    return preroll(ctx, a);
}

s32 cellAtracGetBufferInfoForResetting_impl(PPUContext& ctx, const Layout& L, u32 h, u32 sample, u32 out)
{
    ATRAC_ARGS(h);
    if (const s32 e = check(a); e) return e;
    if (a.s8(st::kHold) == 6 && !a.sr(st::kSecondSize)) return a.e(CELL_ATRAC_ERROR_NEED_SECOND_BUFFER);
    const u32 s = sample + a.sr(st::kOffset);
    if (s > a.sr(st::kEnd)) return a.e(CELL_ATRAC_ERROR_ILLEGAL_SAMPLE);
    if (!guest_readable(thread_of(ctx), out, 16)) return not_guest(ctx, L, __func__, "buffer information", out);
    u32 info[4];
    reset_info(a, s, info);
    for (u32 i = 0; i < 4; i++) ps_write32(a.b, out + 4 * i, info[i]);
    return preroll(ctx, a);
}

// libatrac3plus's exports.
s32 cellAtracSetDataAndGetMemSize(PPUContext& ctx, u32 h, u32 buffer, u32 read, u32 buffer_size, u32 mem_size)
{
    return cellAtracSetDataAndGetMemSize_impl(ctx, kPlus, h, buffer, read, buffer_size, mem_size);
}
s32 cellAtracCreateDecoder(PPUContext& ctx, u32 h, u32 work, u32 ppu_priority, u32 spu_priority)
{
    return cellAtracCreateDecoder_impl(ctx, kPlus, h, work, ppu_priority, spu_priority);
}
s32 cellAtracCreateDecoderExt(PPUContext& ctx, u32 h, u32 work, u32 ppu_priority, u32 ext)
{
    return cellAtracCreateDecoderExt_impl(ctx, kPlus, h, work, ppu_priority, ext);
}
s32 cellAtracDeleteDecoder(PPUContext& ctx, u32 h)
{
    return cellAtracDeleteDecoder_impl(ctx, kPlus, h);
}
s32 cellAtracDecode(PPUContext& ctx, u32 h, u32 out, u32 samples, u32 finish, u32 remain_out)
{
    return cellAtracDecode_impl(ctx, kPlus, h, out, samples, finish, remain_out);
}
s32 cellAtracGetStreamDataInfo(PPUContext& ctx, u32 h, u32 write_at, u32 size, u32 read_at)
{
    return cellAtracGetStreamDataInfo_impl(ctx, kPlus, h, write_at, size, read_at);
}
s32 cellAtracAddStreamData(PPUContext& ctx, u32 h, u32 bytes)
{
    return cellAtracAddStreamData_impl(ctx, kPlus, h, bytes);
}
s32 cellAtracGetRemainFrame(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetRemainFrame_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetVacantSize(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetVacantSize_impl(ctx, kPlus, h, out);
}
s32 cellAtracIsSecondBufferNeeded(PPUContext& ctx, u32 h)
{
    return cellAtracIsSecondBufferNeeded_impl(ctx, kPlus, h);
}
s32 cellAtracGetSecondBufferInfo(PPUContext& ctx, u32 h, u32 at, u32 size)
{
    return cellAtracGetSecondBufferInfo_impl(ctx, kPlus, h, at, size);
}
s32 cellAtracSetSecondBuffer(PPUContext& ctx, u32 h, u32 buffer, u32 size)
{
    return cellAtracSetSecondBuffer_impl(ctx, kPlus, h, buffer, size);
}
s32 cellAtracGetChannel(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetChannel_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetMaxSample(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetMaxSample_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetNextSample(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetNextSample_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetSoundInfo(PPUContext& ctx, u32 h, u32 end, u32 loop_start, u32 loop_end)
{
    return cellAtracGetSoundInfo_impl(ctx, kPlus, h, end, loop_start, loop_end);
}
s32 cellAtracGetNextDecodePosition(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetNextDecodePosition_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetSamplingRate(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetSamplingRate_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetBitrate(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetBitrate_impl(ctx, kPlus, h, out);
}
s32 cellAtracGetLoopInfo(PPUContext& ctx, u32 h, u32 loops, u32 status)
{
    return cellAtracGetLoopInfo_impl(ctx, kPlus, h, loops, status);
}
s32 cellAtracSetLoopNum(PPUContext& ctx, u32 h, s32 loops)
{
    return cellAtracSetLoopNum_impl(ctx, kPlus, h, loops);
}
s32 cellAtracGetInternalErrorInfo(PPUContext& ctx, u32 h, u32 out)
{
    return cellAtracGetInternalErrorInfo_impl(ctx, kPlus, h, out);
}
s32 cellAtracResetPlayPosition(PPUContext& ctx, u32 h, u32 sample, u32 bytes)
{
    return cellAtracResetPlayPosition_impl(ctx, kPlus, h, sample, bytes);
}
s32 cellAtracGetBufferInfoForResetting(PPUContext& ctx, u32 h, u32 sample, u32 out)
{
    return cellAtracGetBufferInfoForResetting_impl(ctx, kPlus, h, sample, out);
}

// libatrac3multi's exports (cellAtracMulti, D-535): a NULL handle or pointer the call takes is 0x80610B91 before
// anything else; then libatrac3plus's bodies on libatrac3multi's layout, ATRAC3 only, with the output tracks.
constexpr u32 kMultiNull = 0x80610B91;
#define MULTI_NULL(...)                                                                    \
    for (const u32 p_ : {__VA_ARGS__})                                                     \
        if (!p_) return error(kMultiNull)

s32 cellAtracMultiSetDataAndGetMemSize(PPUContext& ctx, u32 h, u32 buffer, u32 read, u32 buffer_size, u32 tracks,
    u32 track_array, u32 mem_size)
{
    MULTI_NULL(h, buffer, mem_size);
    if (tracks - 1 > 7) return error(kMultiNull);
    const Layout& L = kMulti;
    ATRAC_ARGS(h, mem_size);
    if (read > buffer_size) return a.e(CELL_ATRAC_ERROR_READSIZE_OVER_BUFFER);
    if (!guest_readable(thread_of(ctx), buffer, std::max<u32>(read, 1))) return not_guest(ctx, L, __func__, "buffer", buffer);
    if (track_array && !guest_readable(thread_of(ctx), track_array, 4 * tracks))
        return not_guest(ctx, L, __func__, "track array", track_array);
    u8* b = a.b;
    std::memset(b + h, 0, 0x200);
    a.set_work(0);
    a.w32(hd::kResource + 4, 0);
    u8 info[0x50];
    const u32 codec = parse_header(b + buffer, read, info, true);
    if (static_cast<s32>(codec) < 0) return a.e(codec);
    a.w32(hd::kType, 0x1F); // CELL_ADEC_TYPE_ATRAC3MULTI
    ps_write16(b, a.s() + st::kCodec, static_cast<u16>(codec));
    a.w32(hd::kAt3Param + 4, ps_ld32(info + 0x14));
    a.w32(hd::kAt3Param, tracks);
    a.w32(hd::kAt3Param + 8, ps_ld16(info + 0xC));
    a.w32(hd::kAt3Param + 0xC, 0x84);
    a.w32(hd::kAt3Param + 0x10, ps_ld16(info + 2));
    if (const u32 e = set_stream(a, buffer, read, buffer_size, info, tracks, track_array); e) return a.e(e);
    ps_write32(b, mem_size, 0);
    const s32 r = static_cast<s32>(adec(ctx, "cellAdecQueryAttr", {h + hd::kType, h + hd::kAttr}));
    if (r < 0) return r;
    const u32 ch = a.s8(st::kChannels);
    ps_write32(b, mem_size, ((ch & 0xFFFFFu) << 12) + ((ch & 0xFFFFFFu) << 8) + a.r32(hd::kAttr) + 0x400);
    return r;
}

s32 cellAtracMultiCreateDecoder(PPUContext& ctx, u32 h, u32 work, u32 ppu_priority, u32 spu_priority)
{
    MULTI_NULL(h, work);
    return cellAtracCreateDecoder_impl(ctx, kMulti, h, work, ppu_priority, spu_priority);
}

s32 cellAtracMultiCreateDecoderExt(PPUContext& ctx, u32 h, u32 work, u32 ppu_priority, u32 ext)
{
    MULTI_NULL(h, work, ext);
    return cellAtracCreateDecoderExt_impl(ctx, kMulti, h, work, ppu_priority, ext);
}

s32 cellAtracMultiDeleteDecoder(PPUContext& ctx, u32 h)
{
    MULTI_NULL(h);
    return cellAtracDeleteDecoder_impl(ctx, kMulti, h);
}

s32 cellAtracMultiDecode(PPUContext& ctx, u32 h, u32 out, u32 samples, u32 finish, u32 remain_out)
{
    MULTI_NULL(h, out, samples, finish, remain_out);
    return cellAtracDecode_impl(ctx, kMulti, h, out, samples, finish, remain_out);
}

s32 cellAtracMultiGetStreamDataInfo(PPUContext& ctx, u32 h, u32 write_at, u32 size, u32 read_at)
{
    MULTI_NULL(h, write_at, size, read_at);
    return cellAtracGetStreamDataInfo_impl(ctx, kMulti, h, write_at, size, read_at);
}

s32 cellAtracMultiAddStreamData(PPUContext& ctx, u32 h, u32 bytes)
{
    MULTI_NULL(h);
    return cellAtracAddStreamData_impl(ctx, kMulti, h, bytes);
}

s32 cellAtracMultiGetRemainFrame(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetRemainFrame_impl(ctx, kMulti, h, out);
}

s32 cellAtracMultiGetVacantSize(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetVacantSize_impl(ctx, kMulti, h, out);
}

s32 cellAtracMultiIsSecondBufferNeeded(PPUContext& ctx, u32 h)
{
    MULTI_NULL(h);
    return cellAtracIsSecondBufferNeeded_impl(ctx, kMulti, h);
}

s32 cellAtracMultiGetSecondBufferInfo(PPUContext& ctx, u32 h, u32 at, u32 size)
{
    MULTI_NULL(h, at, size);
    return cellAtracGetSecondBufferInfo_impl(ctx, kMulti, h, at, size);
}

s32 cellAtracMultiSetSecondBuffer(PPUContext& ctx, u32 h, u32 buffer, u32 size)
{
    MULTI_NULL(h, buffer);
    return cellAtracSetSecondBuffer_impl(ctx, kMulti, h, buffer, size);
}

s32 cellAtracMultiGetChannel(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetChannel_impl(ctx, kMulti, h, out);
}

s32 cellAtracMultiGetMaxSample(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetMaxSample_impl(ctx, kMulti, h, out);
}

s32 cellAtracMultiGetNextSample(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetNextSample_impl(ctx, kMulti, h, out);
}

s32 cellAtracMultiGetSoundInfo(PPUContext& ctx, u32 h, u32 end, u32 loop_start, u32 loop_end)
{
    MULTI_NULL(h, end, loop_start, loop_end);
    return cellAtracGetSoundInfo_impl(ctx, kMulti, h, end, loop_start, loop_end);
}

s32 cellAtracMultiGetNextDecodePosition(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetNextDecodePosition_impl(ctx, kMulti, h, out);
}

s32 cellAtracMultiGetSamplingRate(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetSamplingRate_impl(ctx, kMulti, h, out);
}

// The bit rate: libatrac3plus's formula (no table) for the file, per file channel, times the output tracks.
s32 cellAtracMultiGetBitrate(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    const Layout& L = kMulti;
    ATRAC_ARGS(h, out);
    if (const s32 e = check(a); e) return e;
    const u32 file = static_cast<u32>(static_cast<u32>(a.frame() * (a.sr(st::kRate) << 3)) / 1000) >>
        (a.frame_shift() & 0x3F);
    const u32 per = a.sr(mst::kFileChannels) ? file / a.sr(mst::kFileChannels) : 0;
    ps_write32(a.b, out, per * a.s8(st::kChannels));
    return lv2::CELL_OK;
}

s32 cellAtracMultiGetLoopInfo(PPUContext& ctx, u32 h, u32 loops, u32 status)
{
    MULTI_NULL(h, loops, status);
    return cellAtracGetLoopInfo_impl(ctx, kMulti, h, loops, status);
}

s32 cellAtracMultiSetLoopNum(PPUContext& ctx, u32 h, s32 loops)
{
    MULTI_NULL(h);
    return cellAtracSetLoopNum_impl(ctx, kMulti, h, loops);
}

s32 cellAtracMultiGetInternalErrorInfo(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetInternalErrorInfo_impl(ctx, kMulti, h, out);
}

// cellAtracMultiResetPlayPosition(handle, sample, bytes, tracks): a new track array when `tracks` is not NULL.
s32 cellAtracMultiResetPlayPosition(PPUContext& ctx, u32 h, u32 sample, u32 bytes, u32 tracks)
{
    MULTI_NULL(h);
    return cellAtracResetPlayPosition_impl(ctx, kMulti, h, sample, bytes, tracks);
}

s32 cellAtracMultiGetBufferInfoForResetting(PPUContext& ctx, u32 h, u32 sample, u32 out)
{
    MULTI_NULL(h, out);
    return cellAtracGetBufferInfoForResetting_impl(ctx, kMulti, h, sample, out);
}

// The track array: one file channel per output track (the data set is all it needs, no decoder).
s32 cellAtracMultiGetTrackArray(PPUContext& ctx, u32 h, u32 out)
{
    MULTI_NULL(h, out);
    const Layout& L = kMulti;
    ATRAC_ARGS(h);
    if (!a.ready()) return a.e(CELL_ATRAC_ERROR_UNSET_DATA);
    const u32 n = a.s8(st::kChannels);
    if (n && !guest_readable(thread_of(ctx), out, 4 * n)) return not_guest(ctx, L, __func__, "track array", out);
    for (u32 i = 0; i < n; i++) ps_write32(a.b, out + 4 * i, a.sr(mst::kTracks + 4 * i));
    return lv2::CELL_OK;
}

s32 _pspocket_cellAtracMultiAdecCallback(PPUContext& ctx, u32 adec_handle, s32 type, s32 data, u32 handle)
{
    return _pspocket_cellAtracAdecCallback(ctx, adec_handle, type, data, handle);
}

#undef MULTI_NULL
#undef ATRAC_ARGS

constexpr HostImport kImports[] = {
    {"cellAtrac", "cellAdec", 0x7E4A4A49, false}, // cellAdecQueryAttr
    {"cellAtrac", "cellAdec", 0xD00A6988, false}, // cellAdecOpen
    {"cellAtrac", "cellAdec", 0x8B5551A4, false}, // cellAdecOpenExt
    {"cellAtrac", "cellAdec", 0x847D2380, false}, // cellAdecClose
    {"cellAtrac", "cellAdec", 0x487B613E, false}, // cellAdecStartSeq
    {"cellAtrac", "cellAdec", 0x1529E506, false}, // cellAdecDecodeAu
    {"cellAtrac", "cellAdec", 0x97FF2AF1, false}, // cellAdecGetPcm
};

constexpr HostImport kMultiImports[] = {
    {"cellAtracMulti", "cellAdec", 0x7E4A4A49, false}, // cellAdecQueryAttr
    {"cellAtracMulti", "cellAdec", 0xD00A6988, false}, // cellAdecOpen
    {"cellAtracMulti", "cellAdec", 0x8B5551A4, false}, // cellAdecOpenExt
    {"cellAtracMulti", "cellAdec", 0x847D2380, false}, // cellAdecClose
    {"cellAtracMulti", "cellAdec", 0x487B613E, false}, // cellAdecStartSeq
    {"cellAtracMulti", "cellAdec", 0x1529E506, false}, // cellAdecDecodeAu
    {"cellAtracMulti", "cellAdec", 0x97FF2AF1, false}, // cellAdecGetPcm
};

} // namespace

void register_cellAtrac(Registry& r)
{
    r.add_imports("cellAtrac", kImports);
    r.add("cellAtrac", {
        PS_HLE_FUNC(0x006016DA, cellAtracGetBitrate),
        PS_HLE_FUNC(0x06DDB53E, cellAtracSetSecondBuffer),
        PS_HLE_FUNC(0x0F9667B6, cellAtracGetChannel),
        PS_HLE_FUNC(0x2642D4CC, cellAtracCreateDecoderExt),
        PS_HLE_FUNC(0x2BFFF084, cellAtracGetStreamDataInfo),
        PS_HLE_FUNC(0x46CFC013, cellAtracAddStreamData),
        PS_HLE_FUNC(0x4797D1FF, cellAtracGetNextSample),
        PS_HLE_FUNC(0x5F62D546, cellAtracGetMaxSample),
        PS_HLE_FUNC(0x66AFC68E, cellAtracSetDataAndGetMemSize),
        PS_HLE_FUNC(0x761CB9BE, cellAtracDeleteDecoder),
        PS_HLE_FUNC(0x7772EB2B, cellAtracResetPlayPosition),
        PS_HLE_FUNC(0x77E24BB0, cellAtracGetSamplingRate),
        PS_HLE_FUNC(0x78BA5C41, cellAtracSetLoopNum),
        PS_HLE_FUNC(0x7B22E672, cellAtracGetNextDecodePosition),
        PS_HLE_FUNC(0x8EB0E65F, cellAtracDecode),
        PS_HLE_FUNC(0x99EFE171, cellAtracIsSecondBufferNeeded),
        PS_HLE_FUNC(0x99FB73D1, cellAtracGetBufferInfoForResetting),
        PS_HLE_FUNC(0xAB6B6DBF, cellAtracGetLoopInfo),
        PS_HLE_FUNC(0xB5C11938, cellAtracGetInternalErrorInfo),
        PS_HLE_FUNC(0xBE07F05E, cellAtracGetSecondBufferInfo),
        PS_HLE_FUNC(0xC9A95FCB, cellAtracGetVacantSize),
        PS_HLE_FUNC(0xCF01D5D4, cellAtracGetSoundInfo),
        PS_HLE_FUNC(0xDFAB73AA, cellAtracGetRemainFrame),
        PS_HLE_FUNC(0xFA293E88, cellAtracCreateDecoder),
        ::pspocket::hle::Function{{}, "_pspocket_cellAtracAdecCallback",
            recomp::nid_of("_pspocket_cellAtracAdecCallback"),
            &detail::thunk<&_pspocket_cellAtracAdecCallback>, true},
    });
    r.add_imports("cellAtracMulti", kMultiImports);
    r.add("cellAtracMulti", {
        PS_HLE_FUNC(0x0FFB4665, cellAtracMultiCreateDecoder),
        PS_HLE_FUNC(0x1AE28192, cellAtracMultiAddStreamData),
        PS_HLE_FUNC(0x489E22F1, cellAtracMultiGetNextDecodePosition),
        PS_HLE_FUNC(0x59431D1B, cellAtracMultiGetRemainFrame),
        PS_HLE_FUNC(0x5B6CCF5A, cellAtracMultiGetTrackArray),
        PS_HLE_FUNC(0x60B2101D, cellAtracMultiGetVacantSize),
        PS_HLE_FUNC(0x659775FB, cellAtracMultiSetSecondBuffer),
        PS_HLE_FUNC(0x670B2721, cellAtracMultiGetStreamDataInfo),
        PS_HLE_FUNC(0x6CFF1D7F, cellAtracMultiGetBitrate),
        PS_HLE_FUNC(0x77BD901A, cellAtracMultiGetNextSample),
        PS_HLE_FUNC(0x780A18BB, cellAtracMultiDeleteDecoder),
        PS_HLE_FUNC(0x81B22CA8, cellAtracMultiSetDataAndGetMemSize),
        PS_HLE_FUNC(0x9119CFD1, cellAtracMultiResetPlayPosition),
        PS_HLE_FUNC(0x92D48677, cellAtracMultiGetInternalErrorInfo),
        PS_HLE_FUNC(0xB23F070A, cellAtracMultiCreateDecoderExt),
        PS_HLE_FUNC(0xB91967BC, cellAtracMultiGetLoopInfo),
        PS_HLE_FUNC(0xC5C5EED3, cellAtracMultiGetSoundInfo),
        PS_HLE_FUNC(0xD85F2D10, cellAtracMultiGetMaxSample),
        PS_HLE_FUNC(0xD86ACB12, cellAtracMultiDecode),
        PS_HLE_FUNC(0xDA43F526, cellAtracMultiSetLoopNum),
        PS_HLE_FUNC(0xDE9B7905, cellAtracMultiGetBufferInfoForResetting),
        PS_HLE_FUNC(0xED30F1FA, cellAtracMultiGetSecondBufferInfo),
        PS_HLE_FUNC(0xF11B8DD5, cellAtracMultiGetChannel),
        PS_HLE_FUNC(0xF197CCAA, cellAtracMultiGetSamplingRate),
        PS_HLE_FUNC(0xF8B1AB0C, cellAtracMultiIsSecondBufferNeeded),
        ::pspocket::hle::Function{{}, "_pspocket_cellAtracMultiAdecCallback",
            recomp::nid_of("_pspocket_cellAtracMultiAdecCallback"),
            &detail::thunk<&_pspocket_cellAtracMultiAdecCallback>, true},
    });
}

} // namespace pspocket::hle
