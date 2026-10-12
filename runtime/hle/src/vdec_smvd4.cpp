// libsmvd4, the MPEG-4 part 2 core (Simple profile), pspocket's own (D-920 part 5): answering as Sony's module does
// (firmware 4.92), compared call for call in tests/hle/test_vdec_cores_compare.cpp. The PPU framework it shares with
// the other s-cores is vdec_score.hpp's; this file is what libsmvd4's SPU program does with the stream, done on the
// host: the headers parsed as Sony's program parses them (what it stores of them, what it refuses), the pictures
// decoded with FFmpeg into Sony's frame buffers, laid out as Sony's are.
//
// The instance's memory (128-byte aligned; GetMemorySize's size: a block per macroblock, three frame buffers and
// 0x15380 bytes of the core's own) as a caller sees it:
// - +0x80: the sequence information (GetSideInfoSeq copies 0x33 bytes but three): whether a video object layer is in
//   force, its start code, then its fields in syntax order as the program stores them (a field a layer leaves out
//   keeps the value an earlier layer gave it).
// - +0x13C: the bytes of its access unit the last DecodePicture used (its output): up to the end of its first VOP,
//   all of it when it has none, where the parse stopped when a layer is refused; +0x144: the existence flag (1 a
//   layer was parsed, 0x10000 a picture decoded, ever: neither Initialize nor anything else clears them); +0x148: the
//   caller's auxiliary data (SetAuxData), kept with each picture decoded after it.
// - +0x280: each frame buffer slot's information block (0x100 bytes): the picture's addresses, size and lines, one
//   access unit, its side information (+0x20: the last group of VOPs' fields and the VOP's own; again a field a VOP
//   leaves out keeps its value), the auxiliary data (+0x60) and the status (+0x80).
// - +0x14A80: the three frame buffers, each sized for the instance's largest picture, laid out in 16-sample strips
//   with 16 lines and samples of edge (8 for chroma), the edges padded as each picture is decoded.
//
// What the program does with an access unit (the status bits, vdec_score.hpp's):
// - Headers before the first VOP are parsed (VOS, VO, VOL, user data, GOV); a layer of a frame larger than the
//   instance's kills the decoder (0x81000000: DecodePicture answers 0x..81, then every command 0x..80); a layer with
//   MPEG quantisation is refused (no layer in force: its VOPs give no picture, ST_NO_PICTURE).
// - Its first VOP gives one picture in the first free slot, an anchor (the slots of the two last anchors are not
//   free). I and P VOPs are decoded. A P VOP is predicted from its anchor's frame buffer as it holds it (whatever
//   wrote it: a B VOP's slot keeps its old frame, and a P VOP after it predicts from that). A P VOP with no anchor
//   (after Initialize, FlushPicture or CancelDecode) is predicted from the third slot's frame and flagged 0x800
//   (GetPicture's 0x..1B) unless it is decoded into that slot; after Discontinue a P VOP is flagged 0x800 too, until
//   the next I VOP. A VOP not coded gives the last anchor's slot again, with its own information. B and S VOPs and
//   the VOPs of an interlaced layer are not decoded (0x2; an interlaced P VOP 0x402): a B VOP's slot keeps its frame
//   and its information block as they were; an interlaced VOP's block is written, its frame left. A quarter sample
//   layer's motion is taken as half sample motion. Nothing is concealed: a macroblock the program cannot decode (a
//   VOP cut short, damaged data) keeps what its slot held.
// - An access unit with no VOP, a VOP with no layer in force: no picture (0x1).
//
// FFmpeg does the decoding, made to do what Sony's program does: each P VOP's reference rewritten from the anchor's
// frame buffer (vdec_ffmpeg.hpp rewrite_last), each picture's buffer filled with its slot's old frame first and error
// concealment off (prefill), the quarter sample flag cleared from the layer it is given. Not reproduced, each logged
// where it happens: the layer features Sony's Simple profile program refuses besides MPEG quantisation (shapes,
// sprites, other sample depths, complexity estimation, NEWPRED, reduced resolution VOPs, scalability: refused as MPEG
// quantisation is, not compared); bit 2 of an interlaced P VOP's warning (Sony's program sets it for some P VOPs, from
// their macroblocks); a P VOP with no anchor decoded into the third slot itself, which Sony's program overwrites as
// it predicts from it (a few samples up to 2 steps off); a P VOP before FFmpeg has had a picture of the layer's size
// (predicted from grey); the IDCT's rounding (FFmpeg's FAAN, one step off Sony's in a few samples).
//
// Nothing here is Sony's code or data (CLAUDE.md rule 1): the behaviour is written from what the module does.

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vdec_core_common.hpp"
#include "vdec_ffmpeg.hpp"
#include "vdec_layout.hpp"
#include "vdec_score.hpp"

namespace pspocket::hle::vdec_core {
namespace {

using score::PictureQueue;

constexpr std::string_view kChan = "smvd4";
constexpr u32 kPrefix = 0x80615100;
s32 fail(u32 low) { return score::code(kPrefix, low); }

// The SPU program's status bits of its own (vdec_score.hpp has the shared ones).
enum : u32 {
    ST_NOT_DECODED = 0x2,
    ST_INTERLACED_P = 0x400,
    ST_DISCONTINUITY = 0x800,
    ST_TOO_LARGE = score::ST_DEAD | score::ST_NO_MEMORY_BIT,
};

// Places in the instance's memory.
constexpr u32 kSeq = 0x80, kAuCopy = 0x100, kUsed = 0x13C, kFlag = 0x144, kAux = 0x148, kPictures = 0x280,
              kFrames = 0x14A80;
constexpr score::QueuePlaces kQueues{0x3280, 0x3380, 0x3480, 0x3580};
// The core's own memory besides the macroblocks' blocks and the frame buffers (the SPU task's context among it).
constexpr u32 kFixed = 0x15380;

// Each level's largest picture (levels 0 to 6).
constexpr u32 kLevelW[] = {720, 176, 352, 352, 720, 640, 720}, kLevelH[] = {576, 144, 288, 288, 480, 480, 576};

u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// A frame buffer for a w x h picture: the luma's lines to 32, 16 lines and samples of edge each side, chroma half that,
// and 128 bytes.
u32 frame_bytes(u32 w, u32 h) { return (align(h, 32) + 32) * (align(w, 16) + 32) * 3 / 2 + 0x80; }
u32 memory_for(u32 w, u32 h) { return ((w + 15) >> 4) * ((h + 15) >> 4) * 0x80 + 3 * frame_bytes(w, h) + kFixed; }

vdec::Plan plan()
{
    vdec::Plan p;
    p.edge_y = 16;
    p.edge_c = 8;
    p.start_inside = true;
    return p;
}

// ---- the SPU program's state

// The video object layer as the program stores it (the sequence information's fields).
struct Vol {
    u8 code = 0, random_access = 0, type = 0, ident = 0, verid = 0, priority = 0, aspect = 0, par_w = 0, par_h = 0;
    u8 control = 0, chroma = 0, low_delay = 0, vbv = 0;
    u32 bit_rate = 0, vbv_size = 0, vbv_occupancy = 0;
    u8 shape = 0, fixed = 0;
    u16 resolution = 0, fixed_increment = 0, width = 0, height = 0;
    u8 interlaced = 0, obmc_disable = 0, sprite = 0, not_8_bit = 0, quant_type = 0, quarter_sample = 0;
    u8 complexity_disable = 0, resync_disable = 0, data_partitioned = 0, reversible_vlc = 0;
    u32 increment_bits = 1;
};

// The group of VOPs header's fields (the last one's) and the VOP header's, as the program keeps them.
struct Gov {
    u8 seen = 0, parity = 0, closed = 0, broken = 0;
    u32 seconds = 0;
};
struct Vop {
    u8 type = 0, coded = 0, rounding = 0, dc_threshold = 0, top_field_first = 0, alternate_scan = 0;
    u8 fcode_forward = 0, fcode_backward = 0;
    u16 increment = 0, quant = 0;
    u32 modulo = 0;
};

struct Smvd4 {
    std::mutex m;
    u32 handle = 0, max_w = 0, max_h = 0;
    bool dead = false;
    Vol vol;
    bool vol_valid = false; // a layer in force (since the last Initialize)
    Gov gov;
    Vop vop;
    u32 seconds = 0; // the time base: the last GOV's time code and the I and P VOPs' modulo time bases since
    bool ever_vol = false, ever_picture = false;
    PictureQueue queue;
    bool discontinuity = false;
    vdec::FfDecoder ff;
    bool ff_open = false;
    std::vector<u8> headers; // the headers parsed since FFmpeg's last VOP (given to it with the next)
    u64 qpel_bit = 0;        // where the last layer's quarter_sample is in its access unit (bits), 0 none
};

using Smvd4s = score::Instances<Smvd4>;
Smvd4s& smvd4s() { return Smvd4s::of_process(kChan); }

// ---- the stream

struct Bits {
    const u8* p;
    u32 size; // bytes
    u64 pos;  // bits from p
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos++) v = (v << 1) | (pos / 8 < size ? (p[pos / 8] >> (7 - pos % 8)) & 1 : 0);
        return v;
    }
    u8 bit() { return static_cast<u8>(bits(1)); }
};

// The next start code prefix (00 00 01) at or after byte `from`, or the size.
u32 start_code(const u8* p, u32 size, u32 from)
{
    for (u32 i = from; i + 3 < size; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return size;
}

// The sequence information at +0x80 (its gaps, 0x0F, 0x1D and 0x21, left).
void write_seq(u8* base, u32 handle, const Vol& v)
{
    const u32 s = handle + kSeq;
    const u8 bytes[] = {1, 0, v.code, v.random_access, v.type, v.ident, v.verid, v.priority, v.aspect, v.par_w, v.par_h,
        v.control, v.chroma, v.low_delay, v.vbv};
    for (u32 k = 0; k < sizeof(bytes); k++) ps_write8(base, s + k, bytes[k]);
    ps_write32(base, s + 0x10, v.bit_rate);
    ps_write32(base, s + 0x14, v.vbv_size);
    ps_write32(base, s + 0x18, v.vbv_occupancy);
    ps_write8(base, s + 0x1C, v.shape);
    ps_write16(base, s + 0x1E, v.resolution);
    ps_write8(base, s + 0x20, v.fixed);
    ps_write16(base, s + 0x22, v.fixed_increment);
    ps_write16(base, s + 0x24, v.width);
    ps_write16(base, s + 0x26, v.height);
    const u8 tail[] = {v.interlaced, v.obmc_disable, v.sprite, v.not_8_bit, v.quant_type, v.quarter_sample,
        v.complexity_disable, v.resync_disable, v.data_partitioned, v.reversible_vlc, 0};
    for (u32 k = 0; k < sizeof(tail); k++) ps_write8(base, s + 0x28 + k, tail[k]);
}

enum class VolResult { ok, refused, too_large };

// A video object layer header (after its start code, whose last byte is `code`), as Sony's program parses it into its
// state. A field the header leaves out keeps the value it had.
VolResult parse_vol(Smvd4& x, Bits& b, u8 code)
{
    Vol& v = x.vol;
    v.code = code;
    v.random_access = b.bit();
    v.type = static_cast<u8>(b.bits(8));
    v.ident = b.bit();
    if (v.ident) {
        v.verid = static_cast<u8>(b.bits(4));
        v.priority = static_cast<u8>(b.bits(3));
    } else {
        v.verid = 1;
        v.priority = 1;
    }
    const u32 verid = v.ident ? v.verid : 1;
    v.aspect = static_cast<u8>(b.bits(4));
    if (v.aspect == 15) {
        v.par_w = static_cast<u8>(b.bits(8));
        v.par_h = static_cast<u8>(b.bits(8));
    }
    v.control = b.bit();
    if (v.control) {
        v.chroma = static_cast<u8>(b.bits(2));
        v.low_delay = b.bit();
        v.vbv = b.bit();
        if (v.vbv) {
            const u32 rate_hi = b.bits(15);
            b.bit();
            const u32 rate_lo = b.bits(15);
            b.bit();
            const u32 size_hi = b.bits(15);
            b.bit();
            const u32 size_lo = b.bits(3);
            const u32 occupancy_hi = b.bits(11);
            b.bit();
            const u32 occupancy_lo = b.bits(15);
            b.bit();
            v.bit_rate = rate_hi << 15 | rate_lo;
            v.vbv_size = size_hi << 3 | size_lo;
            v.vbv_occupancy = occupancy_hi << 15 | occupancy_lo;
        }
    }
    const auto unverified = [&](std::string_view what) {
        PS_LOG_WARN(kChan, "instance {:#x}: a video object layer with {} refused (not compared with Sony's)", x.handle,
            what);
        return VolResult::refused;
    };
    v.shape = static_cast<u8>(b.bits(2));
    if (v.shape) return unverified("a shape other than rectangular");
    b.bit();
    v.resolution = static_cast<u16>(b.bits(16));
    b.bit();
    v.increment_bits = 1;
    while (v.increment_bits < 16 && (1u << v.increment_bits) < v.resolution) v.increment_bits++;
    v.fixed = b.bit();
    v.fixed_increment = v.fixed ? static_cast<u16>(b.bits(v.increment_bits)) : 0;
    b.bit();
    const u16 w = static_cast<u16>(b.bits(13));
    b.bit();
    const u16 h = static_cast<u16>(b.bits(13));
    b.bit();
    if (frame_bytes(w, h) > frame_bytes(x.max_w, x.max_h)) {
        PS_LOG_ERROR(kChan, "instance {:#x}: a video object layer of {}x{} does not fit its frame buffers ({}x{})",
            x.handle, w, h, x.max_w, x.max_h);
        return VolResult::too_large;
    }
    v.width = w;
    v.height = h;
    v.interlaced = b.bit();
    v.obmc_disable = b.bit();
    v.sprite = static_cast<u8>(b.bits(verid == 1 ? 1 : 2));
    if (v.sprite) return unverified("sprites");
    v.not_8_bit = b.bit();
    if (v.not_8_bit) return unverified("another sample depth");
    v.quant_type = b.bit();
    if (v.quant_type) return VolResult::refused; // MPEG quantisation, which Sony's program refuses
    v.quarter_sample = 0;
    x.qpel_bit = 0;
    if (verid != 1) {
        x.qpel_bit = b.pos;
        v.quarter_sample = b.bit();
    }
    v.complexity_disable = b.bit();
    if (!v.complexity_disable) return unverified("complexity estimation");
    v.resync_disable = b.bit();
    v.data_partitioned = b.bit();
    if (v.data_partitioned) v.reversible_vlc = b.bit();
    if (verid != 1) {
        if (b.bit()) return unverified("NEWPRED");
        if (b.bit()) return unverified("reduced resolution VOPs");
    }
    if (b.bit()) return unverified("scalability");
    return VolResult::ok;
}

// A group of VOPs header: its time code, closed_gov and broken_link; the time base starts again there.
void parse_gov(Smvd4& x, Bits& b)
{
    const u32 hours = b.bits(5), minutes = b.bits(6);
    b.bit();
    const u32 seconds = b.bits(6);
    x.gov.closed = b.bit();
    x.gov.broken = b.bit();
    x.gov.seconds = hours * 3600 + minutes * 60 + seconds;
    x.gov.seen = 1;
    x.gov.parity ^= 1;
    x.seconds = x.gov.seconds;
}

void parse_vop(Smvd4& x, Bits& b)
{
    Vop& p = x.vop;
    p.type = static_cast<u8>(b.bits(2));
    p.modulo = 0;
    while (p.modulo < 64 && b.bit()) p.modulo++;
    b.bit();
    p.increment = static_cast<u16>(b.bits(x.vol.increment_bits));
    b.bit();
    p.coded = b.bit();
    if (!p.coded) return;
    if (p.type == 1 || p.type == 3) p.rounding = b.bit();
    else if (p.type == 0) p.rounding = 0;
    p.dc_threshold = static_cast<u8>(b.bits(3));
    if (x.vol.interlaced) {
        p.top_field_first = b.bit();
        p.alternate_scan = b.bit();
    }
    p.quant = static_cast<u16>(b.bits(5));
    if (p.type != 0) p.fcode_forward = static_cast<u8>(b.bits(3));
    if (p.type == 2) p.fcode_backward = static_cast<u8>(b.bits(3));
}

// ---- the pictures

u32 info_of(u32 handle, int slot) { return handle + kPictures + static_cast<u32>(slot) * 0x100; }
u32 frame_of(const Smvd4& x, int slot)
{
    return x.handle + kFrames + static_cast<u32>(slot) * frame_bytes(x.max_w, x.max_h);
}

// A picture's information block: its addresses, size and lines, one access unit, its side information, the caller's
// auxiliary data and the status (0x300 added to a status that is not 0, 0x10 taken out).
void write_info(u8* base, const Smvd4& x, int slot, u32 status)
{
    const u32 info = info_of(x.handle, slot), frame = frame_of(x, slot);
    const u32 w = x.vol.width, h = x.vol.height;
    const vdec::Plan p = plan();
    const u32 cb = frame + vdec::strip_cb(p, w, h);
    ps_write32(base, info + 0x00, frame + vdec::strip_start(p, h));
    ps_write32(base, info + 0x04, cb);
    ps_write32(base, info + 0x08, cb + 8);
    ps_write32(base, info + 0x0C, w);
    ps_write32(base, info + 0x10, h);
    ps_write32(base, info + 0x14, align(h, 16) + 32);
    ps_write32(base, info + 0x18, align(h, 16) / 2 + 16);
    ps_write16(base, info + 0x1C, 1);
    ps_write8(base, info + 0x1F, 1);
    const u32 s = info + 0x20;
    ps_write8(base, s + 0x00, x.gov.seen);
    ps_write8(base, s + 0x01, x.gov.parity);
    ps_write32(base, s + 0x04, x.gov.seconds);
    ps_write8(base, s + 0x08, x.gov.closed);
    ps_write8(base, s + 0x09, x.gov.broken);
    ps_write8(base, s + 0x0A, 1);
    ps_write8(base, s + 0x0B, x.vop.type);
    ps_write32(base, s + 0x0C, x.vop.modulo);
    ps_write32(base, s + 0x10, x.seconds);
    ps_write16(base, s + 0x14, x.vop.increment);
    const u8 bytes[] = {x.vop.coded, x.vop.rounding, x.vop.dc_threshold, x.vop.top_field_first, x.vop.alternate_scan};
    for (u32 k = 0; k < sizeof(bytes); k++) ps_write8(base, s + 0x16 + k, bytes[k]);
    ps_write16(base, s + 0x1C, x.vop.quant);
    ps_write8(base, s + 0x1E, x.vop.fcode_forward);
    ps_write8(base, s + 0x1F, x.vop.fcode_backward);
    std::memmove(base + info + 0x60, base + x.handle + kAux, 16);
    const u32 kept = status & ~0x10u;
    ps_write32(base, info + 0x80, kept ? kept | 0x300 : 0);
}

bool open_ffmpeg(Smvd4& x)
{
#if PSPOCKET_HAVE_FFMPEG
    if (!x.ff_open) {
        // Each picture out as it is decoded (B VOPs are never given), with the IDCT nearest Sony's program's.
        x.ff_open = x.ff.open(AV_CODEC_ID_MPEG4, 0, 0, {}, FF_IDCT_FAAN, AV_CODEC_FLAG_LOW_DELAY);
        if (!x.ff_open) PS_LOG_ERROR(kChan, "FFmpeg's MPEG-4 decoder did not open");
        // A P VOP is predicted from its anchor slot's frame as the slot holds it (vdec_ffmpeg.hpp rewrite_last).
        x.ff.keep_last(true);
        // Sony's program conceals nothing: a macroblock it cannot decode keeps what its frame buffer held.
        x.ff.concealment(0);
    }
    return x.ff_open;
#else
    (void)x;
    return false;
#endif
}

// An I or P VOP (with the headers before it) decoded into `slot`'s frame buffer, its edges padded; a P VOP predicted
// from `ref`'s frame as that slot holds it (Sony's program reads its anchor from the frame buffer).
void decode_into(u8* base, Smvd4& x, const u8* vop, u32 size, int slot, int ref)
{
    std::vector<u8> packet = std::move(x.headers);
    x.headers.clear();
    packet.insert(packet.end(), vop, vop + size);
    if (!open_ffmpeg(x)) return;
    {
        std::vector<u8> y, u, v;
        vdec::read_strips(base, frame_of(x, slot), plan(), x.vol.width, x.vol.height, y, u, v);
        x.ff.prefill(x.vol.width, x.vol.height, std::move(y), std::move(u), std::move(v));
    }
    if (ref >= 0) {
        std::vector<u8> y, u, v;
        vdec::read_strips(base, frame_of(x, ref), plan(), x.vol.width, x.vol.height, y, u, v);
        if (!x.ff.rewrite_last(x.vol.width, x.vol.height, y.data(), u.data(), v.data()))
            PS_LOG_WARN(kChan, "instance {:#x}: a P VOP before FFmpeg has a {}x{} picture: predicted from grey (Sony's "
                               "program predicts from slot {}'s frame)", x.handle, x.vol.width, x.vol.height, ref);
    }
    std::deque<vdec::DecodedPicture> out;
    x.ff.decode(packet.data(), static_cast<u32>(packet.size()), 0, out);
    if (out.empty()) {
        PS_LOG_WARN(kChan, "instance {:#x}: FFmpeg gave no picture for a VOP; its frame buffer is left", x.handle);
        return;
    }
    const vdec::DecodedPicture& pic = out.back();
    if (pic.width != x.vol.width || pic.height != x.vol.height) {
        PS_LOG_WARN(kChan, "instance {:#x}: FFmpeg's picture is {}x{}, the layer's {}x{}; its frame buffer is left",
            x.handle, pic.width, pic.height, x.vol.width, x.vol.height);
        return;
    }
    vdec::write_strips(base, frame_of(x, slot), plan(), pic.width, pic.height, pic.y.data(), pic.u.data(),
        pic.v.data());
}

// DecodePicture's work: the access unit's headers, then its first VOP. The status; `used` the bytes it took.
u32 spu_decode(u8* base, Smvd4& x, u32 addr, u32 size, u32& used)
{
    if (x.dead) return score::ST_DEAD;
    const u8* p = base + addr;
    PictureQueue& q = x.queue;
    for (u32 at = start_code(p, size, 0); at < size;) {
        const u8 code = at + 3 < size ? p[at + 3] : 0;
        const u32 next = start_code(p, size, at + 4);
        Bits b{p, size, u64{at + 4} * 8};
        if (code >= 0x20 && code <= 0x2F) {
            const VolResult r = parse_vol(x, b, code);
            if (r == VolResult::too_large) {
                x.dead = true;
                used = 2;
                return ST_TOO_LARGE;
            }
            if (r == VolResult::refused) {
                x.vol_valid = false;
                x.headers.clear();
                ps_write8(base, x.handle + kSeq, 0);
                used = std::min<u32>(static_cast<u32>(b.pos / 8), size);
                return score::ST_NO_PICTURE;
            }
            x.vol_valid = true;
            x.ever_vol = true;
            write_seq(base, x.handle, x.vol);
            const usize from = x.headers.size();
            x.headers.insert(x.headers.end(), p + at, p + next);
            if (x.vol.quarter_sample) {
                // Sony's Simple profile program takes a layer's quarter sample motion as half sample motion: so does
                // FFmpeg, given the layer without the flag.
                const u64 bit = x.qpel_bit - u64{at} * 8;
                x.headers[from + bit / 8] &= static_cast<u8>(~(0x80u >> (bit % 8)));
            }
        } else if (code == 0xB3) {
            parse_gov(x, b);
            x.headers.insert(x.headers.end(), p + at, p + next);
        } else if (code == 0xB6) {
            used = next;
            if (!x.vol_valid) return score::ST_NO_PICTURE;
            parse_vop(x, b);
            const Vop& v = x.vop;
            if (v.type != 2) x.seconds += v.modulo;
            x.ever_picture = true;
            if (!v.coded) {
                // Not coded: the last anchor's picture again, with this VOP's information.
                if (q.forward < 0) {
                    PS_LOG_WARN(kChan, "instance {:#x}: a VOP not coded with no picture before it: none", x.handle);
                    return 0;
                }
                write_info(base, x, q.forward, 0);
                q.ready(q.forward);
                return 0;
            }
            const int slot = q.free_slot();
            q.flushed = false;
            if (v.type == 2 || v.type == 3) {
                // B and S VOPs: not decoded, the slot's frame and information as they were; an anchor all the same.
                PS_LOG_WARN(kChan, "instance {:#x}: a {} VOP is not decoded (Sony's Simple profile program)", x.handle,
                    v.type == 2 ? "B" : "S");
                q.anchor(slot);
                q.ready(slot);
                return ST_NOT_DECODED;
            }
            u32 st = 0;
            int ref = -1;
            if (v.type == 0) {
                x.discontinuity = false;
            } else {
                // A P VOP with no anchor (after Initialize, FlushPicture, CancelDecode) is predicted from the third
                // slot's frame, whatever it holds; flagged as after a discontinuity unless decoded into that slot.
                ref = q.forward >= 0 ? q.forward : 2;
                if (q.forward < 0 && slot != 2) x.discontinuity = true;
                if (x.discontinuity) st |= ST_DISCONTINUITY;
            }
            if (x.vol.interlaced) {
                st |= v.type == 0 ? ST_NOT_DECODED : ST_NOT_DECODED | ST_INTERLACED_P;
                PS_LOG_WARN(kChan, "instance {:#x}: a VOP of an interlaced layer is not decoded (Sony's program)",
                    x.handle);
                x.headers.clear();
            } else {
                decode_into(base, x, p + at, next - at, slot, ref);
            }
            write_info(base, x, slot, st);
            q.anchor(slot);
            q.ready(slot);
            return st;
        } else if (code <= 0x1F || code == 0xB0 || code == 0xB2 || code == 0xB5) {
            x.headers.insert(x.headers.end(), p + at, p + next);
        }
        at = next;
    }
    used = size;
    return score::ST_NO_PICTURE;
}

// ---- the calls

// The instance at `handle` for a call (a handle that is none: logged).
std::shared_ptr<Smvd4> instance(u32 handle, std::string_view call)
{
    std::shared_ptr<Smvd4> x = smvd4s().find(handle);
    if (!x) PS_LOG_ERROR(kChan, "{} on {:#x}, which is no instance", call, handle);
    return x;
}

// smvd4GetMemorySize(size*, level): the memory for the level's largest picture.
s32 smvd4GetMemorySize(PPUContext& ctx, u32 size, u32 level)
{
    if (!writable(ctx, size, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    if (level > 6) {
        ps_write32(base, size, 0);
        return fail(score::LOW_SEQ);
    }
    ps_write32(base, size, memory_for(kLevelW[level], kLevelH[level]));
    return lv2::CELL_OK;
}

// The picture size type 5's parameters ask for (words 8 and 9), within the level's largest; 0 when they do not.
bool size_of(u8* base, u32 param, u32 level, u32& w, u32& h)
{
    if (level > 6) return false;
    const s32 pw = static_cast<s32>(ps_read32(base, param + 0x20));
    const s32 ph = static_cast<s32>(ps_read32(base, param + 0x24));
    if (pw <= 0 || ph <= 0 || pw > static_cast<s32>(kLevelW[level]) || ph > static_cast<s32>(kLevelH[level]))
        return false;
    w = static_cast<u32>(pw);
    h = static_cast<u32>(ph);
    return true;
}

// smvd4GetMemorySize2(size*, level, param*): for the size in type 5's parameters.
s32 smvd4GetMemorySize2(PPUContext& ctx, u32 size, u32 level, u32 param)
{
    if (!writable(ctx, size, 4) || !writable(ctx, param, 0x28)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    u32 w = 0, h = 0;
    if (ps_read32(base, param) == 5 && size_of(base, param, level, w, h)) {
        ps_write32(base, size, memory_for(w, h));
        return lv2::CELL_OK;
    }
    ps_write32(base, size, 0);
    return fail(score::LOW_SEQ);
}

// smvd4CreateInstance2(memory, size, level, param*): an instance in `memory` (128-byte aligned) for type 4 (the
// level's largest picture) or type 5 (the parameters' size), its SPU task's home the taskset in word 7.
s32 smvd4CreateInstance2(PPUContext& ctx, u32 mem, u32 size, u32 level, u32 param)
{
    if (!mem || (mem & 0x7F) || !writable(ctx, param, 0x28)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 type = ps_read32(base, param);
    if (type != 4 && type != 5) return fail(score::LOW_SEQ);
    u32 w = 0, h = 0;
    bool sized = level <= 6;
    if (sized && type == 4) {
        w = kLevelW[level];
        h = kLevelH[level];
    } else if (sized) {
        sized = size_of(base, param, level, w, h);
    }
    if (!sized) return fail(score::LOW_FATAL); // the memory query refuses: its error read as a status
    const u32 need = memory_for(w, h);
    if (size < need) return fail(score::LOW_NO_MEMORY);
    const u32 taskset = ps_read32(base, param + 0x1C);
    if (!taskset) return fail(score::LOW_SEQ);
    if (!guest_readable(thread_of(ctx), mem, need)) {
        PS_LOG_ERROR(kChan, "CreateInstance2: memory {:#x} ({:#x} bytes) is not guest memory", mem, need);
        return fail(score::LOW_ARG);
    }
    if (const s32 r = score::make_queues(ctx, mem, taskset, kQueues); r != lv2::CELL_OK) return r;
    auto x = std::make_shared<Smvd4>();
    x->handle = mem;
    x->max_w = w;
    x->max_h = h;
    x->queue.slots = 3;
    ps_write8(base, mem + kSeq, 0);
    smvd4s().add(mem, std::move(x));
    return lv2::CELL_OK;
}

s32 smvd4ReleaseInstance(PPUContext& ctx, u32 handle)
{
    if (!handle) return fail(score::LOW_ARG);
    if (!smvd4s().remove(handle)) {
        PS_LOG_ERROR(kChan, "ReleaseInstance of {:#x}, which is no instance", handle);
        return lv2::CELL_OK;
    }
    score::drop_queues(ctx, handle, kQueues);
    return lv2::CELL_OK;
}

s32 smvd4Initialize(PPUContext& ctx, u32 handle)
{
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "Initialize");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    ps_write8(guest_base(ctx), handle + kSeq, 0);
    x->queue.reset();
    if (x->dead) return score::strict_answer(kPrefix, score::ST_DEAD);
    x->vol_valid = false;
    x->discontinuity = false;
    x->headers.clear();
    return lv2::CELL_OK;
}

// smvd4DecodePicture(handle, au* {address, size, a word}, used*): one access unit; *used the bytes it took (0 when
// no slot was free).
s32 smvd4DecodePicture(PPUContext& ctx, u32 handle, u32 au, u32 used_out)
{
    if (!handle || !writable(ctx, au, 12) || !writable(ctx, used_out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "DecodePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    u8* base = guest_base(ctx);
    if (x->queue.free_slot() < 0) {
        ps_write32(base, used_out, 0);
        return fail(score::LOW_BUSY);
    }
    for (u32 k = 0; k < 12; k += 4) ps_write32(base, handle + kAuCopy + k, ps_read32(base, au + k));
    const u32 addr = ps_read32(base, au), size = ps_read32(base, au + 4);
    u32 used = ps_read32(base, handle + kUsed);
    u32 st = 0;
    if (size && !guest_readable(thread_of(ctx), addr, size)) {
        PS_LOG_ERROR(kChan, "instance {:#x}: an access unit at {:#x} ({} bytes) that is not guest memory", handle, addr,
            size);
        x->dead = true;
        st = score::ST_DEAD;
    } else {
        const bool was_dead = x->dead;
        st = spu_decode(base, *x, addr, size, used);
        if (!was_dead) {
            ps_write32(base, handle + kUsed, used);
            ps_write32(base, handle + kFlag, (x->ever_picture ? 0x10000u : 0) | (x->ever_vol ? 1u : 0));
        }
    }
    ps_write32(base, used_out, ps_read32(base, handle + kUsed));
    return score::answer(kPrefix, st);
}

s32 smvd4FlushPicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "FlushPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return score::answer(kPrefix, score::ST_DEAD);
    x->queue.flush();
    return lv2::CELL_OK;
}

s32 smvd4Discontinue(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "Discontinue");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return score::answer(kPrefix, score::ST_DEAD);
    x->discontinuity = true;
    return lv2::CELL_OK;
}

s32 smvd4CancelDecode(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "CancelDecode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    x->queue.reset();
    if (x->dead) return score::answer(kPrefix, score::ST_DEAD);
    x->headers.clear();
    return lv2::CELL_OK;
}

// smvd4GetPicture(handle, picture*): the next picture (nine words: its luma, Cb and Cr addresses, 0x40000004, its
// width and height, its luma's and chroma's lines with their edges, 0) and its status's answer.
s32 smvd4GetPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x24)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "GetPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    const int slot = x->queue.take();
    if (slot < 0) return fail(score::LOW_EMPTY);
    u8* base = guest_base(ctx);
    const u32 info = info_of(handle, slot);
    for (u32 k = 0; k < 3; k++) ps_write32(base, out + 4 * k, ps_read32(base, info + 4 * k));
    ps_write32(base, out + 12, 0x40000004);
    for (u32 k = 0; k < 4; k++) ps_write32(base, out + 16 + 4 * k, ps_read32(base, info + 12 + 4 * k));
    ps_write32(base, out + 32, 0);
    const u32 count = ps_read16(base, info + 0x1C);
    u32 st = count ? 0 : score::ST_EMPTY;
    for (u32 k = 0; k < count; k++) st |= ps_read32(base, info + 0x80 + 4 * k);
    return score::picture_answer(kPrefix, st);
}

s32 smvd4GetNumPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "GetNumPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    ps_write32(guest_base(ctx), out, x->queue.count());
    return x->queue.flushed ? score::answer(kPrefix, score::ST_FLUSHED) : lv2::CELL_OK;
}

s32 smvd4ReleasePicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, "ReleasePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    return x->queue.release_one() ? lv2::CELL_OK : fail(score::LOW_EMPTY);
}

// GetSideInfoPic(handle, count*, first*, second*) and GetAuxData: the picture GetPicture gave last, its access units'
// side information (0x20 bytes each, from +0x20 of its block) or auxiliary data (0x10 bytes each, from +0x60).
s32 side_of_picture(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second, u32 from, u32 bytes,
    std::string_view call)
{
    if (!handle || !writable(ctx, count, 4) || !writable(ctx, first, bytes) || !second) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd4> x = instance(handle, call);
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->queue.current < 0) return fail(score::LOW_EMPTY);
    u8* base = guest_base(ctx);
    const u32 info = info_of(handle, x->queue.current);
    const u32 n = ps_read16(base, info + 0x1C);
    if (n > 1 && !writable(ctx, second, bytes)) return fail(score::LOW_ARG);
    score::copy_blocks(base, count, first, second, info + from, n, bytes);
    return lv2::CELL_OK;
}

s32 smvd4GetSideInfoPic(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    return side_of_picture(ctx, handle, count, first, second, 0x20, 0x20, "GetSideInfoPic");
}

s32 smvd4GetAuxData(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    return side_of_picture(ctx, handle, count, first, second, 0x60, 0x10, "GetAuxData");
}

s32 smvd4GetSideInfoSeq(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x33) || !guest_readable(thread_of(ctx), handle + kSeq, 0x33))
        return fail(score::LOW_ARG);
    score::copy_skipping(guest_base(ctx), out, handle + kSeq, 0x33, {0x0F, 0x1D, 0x21});
    return lv2::CELL_OK;
}

s32 smvd4GetExistenceFlag(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4) || !guest_readable(thread_of(ctx), handle + kFlag, 4))
        return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    ps_write32(base, out, ps_read32(base, handle + kFlag));
    return lv2::CELL_OK;
}

// SetAuxData(handle, data*): 16 bytes of the caller's, kept with each picture decoded after.
s32 smvd4SetAuxData(PPUContext& ctx, u32 handle, u32 data)
{
    if (!handle || !writable(ctx, data, 16) || !guest_readable(thread_of(ctx), handle + kAux, 16))
        return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    std::memmove(base + handle + kAux, base + data, 16);
    return lv2::CELL_OK;
}

} // namespace

void register_smvd4(Registry& r)
{
    r.add("libsmvd4", {
        PS_HLE_FUNC(0xF93027E0, smvd4GetMemorySize),
        PS_HLE_FUNC(0xD85B4BED, smvd4GetMemorySize2),
        PS_HLE_FUNC(0x75ECB783, smvd4CreateInstance2),
        PS_HLE_FUNC(0xBEF20E62, smvd4ReleaseInstance),
        PS_HLE_FUNC(0x4066EB75, smvd4Initialize),
        PS_HLE_FUNC(0xE6F4B68C, smvd4DecodePicture),
        PS_HLE_FUNC(0x9A2EBFB5, smvd4FlushPicture),
        PS_HLE_FUNC(0x06C9F85B, smvd4Discontinue),
        PS_HLE_FUNC(0xF6ECD42C, smvd4CancelDecode),
        PS_HLE_FUNC(0x8A292735, smvd4GetPicture),
        PS_HLE_FUNC(0x53BEF958, smvd4GetNumPicture),
        PS_HLE_FUNC(0xE200B0CE, smvd4ReleasePicture),
        PS_HLE_FUNC(0xA4E5F5E2, smvd4GetSideInfoPic),
        PS_HLE_FUNC(0x17A51600, smvd4GetAuxData),
        PS_HLE_FUNC(0x87AE5A06, smvd4GetSideInfoSeq),
        PS_HLE_FUNC(0xE3769B4A, smvd4GetExistenceFlag),
        PS_HLE_FUNC(0x1560605B, smvd4SetAuxData),
    });
}

} // namespace pspocket::hle::vdec_core
