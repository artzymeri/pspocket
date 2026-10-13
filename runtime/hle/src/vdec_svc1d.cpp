// libsvc1d, the VC-1 core (simple, main and advanced profiles), pspocket's own (D-920 part 7): answering as Sony's
// module does (firmware 4.92), compared call for call in tests/hle/test_vdec_score_compare.cpp. The PPU framework it
// shares with the other s-cores is vdec_score.hpp's; this file is what libsvc1d's four SPU programs (headers and
// pictures, blocks, prediction, deblocking) do with the stream, done on the host: the headers parsed and recorded as
// Sony's program records them, the pictures decoded with FFmpeg (VC-1's reconstruction is exact by the standard) into
// Sony's frame buffers, laid out as Sony's are, and given out in display order through Sony's picture queue.
//
// The instance's memory (GetMemorySize's size; laid out as Places says) as a caller sees it:
// - The first 0x480 bytes: the PPU side's own state. +0x2C: the auxiliary data SetAuxData gave (kept for the next
//   access unit), +0xF8: the last picture GetPicture gave, as its information block (0x220 bytes: two fields'
//   records, the field count at +0x2FB, their statuses at +0x2FC, the picture's auxiliary data at +0x304).
// - The SPU side's: the sequence record (0x38 bytes: GetSideInfoSeq's), four picture entries' information blocks
//   (0x2A0 bytes) and descriptors (addresses, size, lines, format), the SPU tasks' areas, then the frame buffers: six
//   (nine at the main profile's levels), each laid out in 16-sample strips with 64 lines of edge above and below the
//   luma (its lines to 32) and 16 samples each side, chroma half that, Cb and Cr in each strip's halves.
//
// The records are the program's: a field a header leaves out keeps the value an earlier one gave it, across Initialize
// (which clears the picture record's first-entry flag alone); a new instance's start with nothing (its program's own,
// zeroed: a simple profile's, laid out to 16 lines). The sequence record carries the sequence header's fields and,
// for the simple and main profiles' STRUCT_C as for the advanced profile's entry point, the coding tools' (the
// advanced profile's as the entry point before the sequence header gave them). A picture's record: the entry point's
// fields (and whether it was the first since Initialize), then the picture header's.
//
// Nothing here is Sony's code or data (CLAUDE.md rule 1): the behaviour is written from what the module does.

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
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

using vdec::align;

constexpr std::string_view kChan = "svc1d";
constexpr u32 kPrefix = 0x80615300;
s32 fail(u32 low) { return score::code(kPrefix, low); }

// The SPU program's status bits of its own (vdec_score.hpp has the shared ones).
enum : u32 {
    ST_TOO_LARGE = 0x81000000, // a picture size past the instance's (SetPictureSize's, a sequence header's): stopped
    ST_RANGE_REDUCED = 0x2,    // a simple or main profile picture of RANGEREDFRM 1: a warning
    ST_SYNC_MARKERS = 0x4,     // a simple or main profile sequence of SYNCMARKER 1: each picture's warning
    ST_NO_FRAME = 0x8,         // an access unit with no frame (headers alone, bytes that are no stream): a warning
    ST_END_ALONE = 0x20,       // an access unit of an end of sequence alone
    ST_SKIPPED = 0x80,         // a B or BI picture the decode mode passes over (given out, not decoded)
    ST_REFERENCE_WARNED = 0x400, // a picture predicted since an anchor had a warning (until an I picture)
    ST_NO_REFERENCE = 0x800,     // a P picture with no anchor before it, a B one with none after it (after
                                 // FlushPicture, an end of sequence)
};

// What the program waits for before it decodes a picture again.
enum class Wait : u8 {
    Nothing,
    Entry,    // CancelDecode's and Discontinue's: an entry point (the frames before it passed over)
    Sequence, // Initialize's: a sequence header (an access unit that starts with anything else passed over unread)
};

// The existence flag's bits: what the last access unit carried (each level's header, then its user data).
enum : u32 {
    EX_SEQUENCE = 0x1,
    EX_SEQUENCE_USER = 0x10,
    EX_ENTRY = 0x100,
    EX_ENTRY_USER = 0x1000,
    EX_PICTURE = 0x10000,
    EX_FRAME_USER = 0x100000,
    EX_FIELD_USER = 0x200000,
    EX_END = 0x1000000, // an end of sequence
};

// The user data BDUs' start code suffixes, and the flag of each (a slice's none).
constexpr bool is_user_data(u8 code) { return code >= 0x1B && code <= 0x1F; }
constexpr u32 user_flag(u8 code)
{
    return code == 0x1F ? EX_SEQUENCE_USER : code == 0x1E ? EX_ENTRY_USER : code == 0x1D ? EX_FRAME_USER
         : code == 0x1C ? EX_FIELD_USER : 0;
}

// Picture types as the records give them.
enum : u8 { PT_I = 1, PT_P = 2, PT_B = 3, PT_BI = 4, PT_SKIPPED = 5 };

constexpr u32 kEntries = 4;     // the picture queue's
constexpr u32 kRecord = 0x100;  // a field's picture record
constexpr u32 kInfo = 0x2A0;    // a picture entry's information block
constexpr u32 kGetCopy = 0xF8;  // GetPicture's copy of the information block in the PPU state
constexpr u32 kAuxNow = 0x2C;   // SetAuxData's in the PPU state

// Each level's largest picture (0 to 10), and its frame buffers: nine for the main profile's levels (3 to 5) and level
// 0, six for the others.
bool level_size(u32 level, u32& w, u32& h)
{
    constexpr u32 kW[] = {2048, 176, 352, 352, 720, 1920, 352, 720, 1280, 2048, 2048};
    constexpr u32 kH[] = {2048, 144, 288, 288, 576, 1080, 288, 576, 900, 1080, 2048};
    if (level > 10) return false;
    w = kW[level];
    h = kH[level];
    return true;
}
u32 frames_of(u32 level) { return level == 0 || (level >= 3 && level <= 5) ? 9 : 6; }

// The tables sized for the picture: a block per macroblock, and one per block with an edge.
u32 mb_table(u32 w, u32 h)
{
    const u32 n = ((w + 15) >> 4) * ((h + 15) >> 4);
    return (n * 0x380 + 0x80) & ~0x7Fu;
}
u32 block_table(u32 w, u32 h)
{
    const u32 mbw = (w + 15) >> 4, mbh = (h + 15) >> 4;
    return ((3 * (2 * mbw + 4) * (2 * mbh + 2)) << 4) + 0x80 & ~0xFu;
}
u32 frame_bytes(u32 w, u32 h) { return (((align(h, 32) + 128) * (align(w, 16) + 32) * 3) >> 1) + 0x100; }
u32 memory_for(u32 level, u32 w, u32 h)
{
    return 0x1147F0 + frames_of(level) * frame_bytes(w, h) + mb_table(w, h) + block_table(w, h);
}

// Where things are in the instance's memory (Sony's layout, each place aligned to 128 bytes as the running offset
// reaches it).
struct Places {
    u32 spurs = 0, taskset = 0;                  // the core's own (type 3)
    u32 queue_to_ppu = 0, queue_to_ppu_buffer = 0, queue_to_spu = 0, queue_to_spu_buffer = 0;
    u32 reply = 0;                               // the program's reply: the bytes used, the existence flag
    u32 seq = 0;                                 // the sequence record
    u32 info[kEntries] = {}, desc[kEntries] = {}; // the picture entries' information blocks and descriptors
    u32 frame0 = 0;
};
Places places(u32 mem, u32 w, u32 h)
{
    Places p;
    u32 at = mem + 0x480;
    const auto take = [&](u32 size) {
        const u32 here = align(at, 128);
        at += size;
        return here;
    };
    take(0x100);
    p.spurs = take(0x1080);
    p.taskset = take(0x1980);
    take(0x80);
    at += 8 * 0x100 + 8 * 0x2280; // the SPU tasks' own queues and their buffers
    p.queue_to_ppu = take(0x180);
    p.queue_to_ppu_buffer = take(0x2280);
    p.queue_to_spu = take(0x180);
    p.queue_to_spu_buffer = take(0x2280);
    at += 0x100 + 0xC0 + 0xB0 + 0xA0 + 0xA0 + 0x80 + 4 * 0x80; // a barrier, the tasks' arguments
    at += 0x300 + 0x600 + 0xB8 + 0x88 + 0x280 + 0x200 + 0x180 + 0x3F0 + 2 * 0xA0;
    p.reply = take(0x90);
    p.seq = take(0xC0);
    take(0x100);
    for (u32 k = 0; k < kEntries; k++) p.info[k] = take(kInfo);
    for (u32 k = 0; k < kEntries; k++) p.desc[k] = take(0xA0);
    at += mb_table(w, h) + block_table(w, h) + 6 * 0x880 + 4 * 0x3D480;
    p.frame0 = align(at, 128);
    return p;
}

// A frame's layout: its lines to 32 (an advanced profile sequence's), or to 16 (the simple and main profiles', as
// SetPictureSize sets it up then).
vdec::Plan plan(bool lines32)
{
    vdec::Plan p;
    p.edge_y = 64;
    p.edge_c = 32;
    p.lines32 = lines32;
    p.start_inside = true;
    return p;
}

// ---- the program's records

// The sequence header's fields as the record keeps them (and the coding tools'), in the record's order.
struct SeqFields {
    u32 width = 0, height = 0;
    u8 profile = 0, frmrtq = 0, bitrtq = 0, finterpflag = 0;
    u8 loopfilter = 0, multires = 0, fastuvmc = 0, extended_mv = 0, dquant = 0, vstransform = 0, overlap = 0,
       syncmarker = 0, rangered = 0, maxbframes = 0, quantizer = 0;
    u8 level = 0, colordiff = 0, postprocflag = 0, pulldown = 0, interlace = 0, tfcntrflag = 0, psf = 0,
       display_ext = 0;
    u16 display_w = 0, display_h = 0;
    u8 aspect_flag = 0, aspect = 0, aspect_w = 0, aspect_h = 0;
    u8 framerate_flag = 0, framerate_ind = 0, framerate_nr = 0, framerate_dr = 0;
    u16 framerate_exp = 0;
    u8 color_flag = 0, color_prim = 0, transfer = 0, matrix = 0;
};

// The entry point's fields, as the picture records keep them.
struct EntryFields {
    u8 broken_link = 0, closed_entry = 0, panscan = 0, refdist = 0, loopfilter = 0, fastuvmc = 0, extended_mv = 0,
       dquant = 0, vstransform = 0, overlap = 0, quantizer = 0, coded_size = 0;
    u16 coded_w = 0, coded_h = 0; // the size's codes (n / 2 - 1)
    u8 extended_dmv = 0, range_mapy_flag = 0, range_mapy = 0, range_mapuv_flag = 0, range_mapuv = 0;
    u8 hrd_buckets = 0; // the sequence header's HRD_NUM_LEAKY_BUCKETS (the entry point has a byte for each)
};

// The program's records (its task's own: a new instance's zeroed).
struct Records {
    SeqFields seq;
    EntryFields entry;
    u8 pic[kRecord] = {}; // the picture record's fields
};

// ---- the instance

struct Picture {
    bool used = false;   // holds a picture not released
    bool ready = false;  // given a sequence number: GetPicture's in turn
    u8 seq = 0;
    int frame = -1;
    u32 status = 0;      // its warnings (what a P picture predicted from it inherits)
    bool fields = false; // a field picture (its frame's edges each field's)
    u8 fcm = 0;          // its frame coding mode
    // A range reduced P or B picture (the simple and main profiles' RANGEREDFRM): predicted from its anchors' samples
    // range reduced, as FFmpeg's decoder does, then expanded as the program gives it out (FFmpeg's does not).
    bool expand = false;
};

struct Svc1d {
    std::mutex m;
    u32 handle = 0, level = 0, max_w = 0, max_h = 0, frames = 0;
    Places at;
    Records rec; // the program's records
    bool first_entry = true; // the next entry point is the first since Initialize
    bool sequence = false;   // a sequence header (or DecodeSeqHeader's) since the instance began
    u32 width = 0, height = 0; // the coded size in force
    bool simple = false;     // the sequence is the simple or main profile's (STRUCT_C)
    bool lines32 = false;    // the frames' layout (plan): an advanced profile sequence's, or SetPictureSize's
    bool no_anchor = false;  // the next picture has no anchor to predict from (a simple profile's after Discontinue)
    u8 rnd = 0;              // the simple and main profiles' rounding control (toggled by each P picture)
    u32 flag = 0;             // the existence flag
    // The picture queue: its entries, the frames' holders, the anchors (entries), the sequence numbers.
    Picture entry[kEntries];
    int newest = -1, older = -1;
    u8 next_seq = 0, get = 0, release = 0;
    int current = -1; // GetPicture's last
    bool flushed = false;
    // SetDecodeMode's: the byte's bit 0 passes every access unit over, the word (any value) the B and BI pictures.
    u8 mode_byte = 0;
    u32 mode_word = 0;
    Wait wait = Wait::Sequence;
    bool warned = false; // an anchor had a warning since the last I picture
    bool dead = false; // the program stopped (ST_TOO_LARGE): the calls that ask it answer 0x..80 and do nothing
    // The frame buffers the program predicts from, as it keeps them: an anchor's (I, P, skipped; a B picture's when
    // there was no anchor before it) becomes the backward one, the backward one the forward one. A P picture with no
    // anchor before it (after FlushPicture, an end of sequence) is predicted from the forward one.
    int pred_fwd = 0, pred_bwd = 0;
    // FFmpeg, and the headers in force as the stream gave them (its configuration; the sequence header it was opened
    // with); the access units of the anchors FFmpeg predicts from (0: none) and the last one's packet.
    vdec::FfDecoder ff;
    bool ff_open = false;
    std::vector<u8> seq_bdu, entry_bdu, ff_seq;
    u64 ff_fwd = 0, ff_bwd = 0;
    std::vector<u8> ff_anchor_packet;
    std::vector<u8> ff_config; // the configuration it was opened with (a simple profile sequence's, its size's)
    u64 ps_from = 0, ps_to = 0; // the last picture header's pan-scan bits (FFmpeg's decoder does not read them)
    u64 dq_at = 0;              // its VOPDQUANT's (0: none)
    u64 dmv_at = 0;             // where FFmpeg reads a DMVRANGE the picture has not (0: none)
    u64 header_end = 0;         // a simple or main profile picture's end when it has no macroblock layer (0: it has)
    u8 fcm = 0;                 // its frame coding mode (0 progressive, 1 an interlaced frame, 2 fields)
    u8 field_types[2] = {};     // a field picture's fields' types
    u8 records = 1;             // the last picture's records: 1, or a field picture's 2 (the first's in first_record)
    u8 first_record[kRecord] = {};
    u64 next_au = 1;
    std::map<u64, int> waiting; // an access unit FFmpeg has not given back yet, and its entry
};

using Svc1ds = score::Instances<Svc1d>;
Svc1ds& svc1ds() { return Svc1ds::of_process(kChan); }

std::shared_ptr<Svc1d> instance(u32 handle, std::string_view call)
{
    std::shared_ptr<Svc1d> x = svc1ds().find(handle);
    if (!x) PS_LOG_ERROR(kChan, "{} on {:#x}, which is no instance", call, handle);
    return x;
}

// ---- the stream

// The bitstream data units' payload without emulation prevention bytes (Annex E.5).
std::vector<u8> unescape(const u8* p, u32 size)
{
    std::vector<u8> out;
    out.reserve(size);
    u32 zeros = 0;
    for (u32 i = 0; i < size; i++) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(p[i]);
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

struct Bits {
    const std::vector<u8>& d;
    u64 pos = 0;
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos++) v = (v << 1) | (pos / 8 < d.size() ? (d[pos / 8] >> (7 - pos % 8)) & 1 : 0);
        return v;
    }
    u8 bit() { return static_cast<u8>(bits(1)); }
    // decode012: 0 '0', 1 '10', 2 '11' as the standard's codes; `codes` gives the codes' own values (0, 2, 3).
    u8 code012(bool codes = false)
    {
        if (!bit()) return 0;
        const u8 b = bit();
        return codes ? static_cast<u8>(2 + b) : static_cast<u8>(1 + b);
    }
};

// NORM-6's and DIFF-6's tile codes (SMPTE 421M 8.7.3.5: 64 codes of 1 to 13 bits, by the tile's six bits; FFmpeg's
// vc1_vlc_data.h holds the same table): one read, its six bits (-1 when no code matches).
int skip_tile(Bits& b)
{
    constexpr u16 kCodes[64] = {0x001, 0x002, 0x003, 0x000, 0x004, 0x001, 0x002, 0x047, 0x005, 0x003, 0x004, 0x04B,
        0x005, 0x04D, 0x04E, 0x30E, 0x006, 0x006, 0x007, 0x053, 0x008, 0x055, 0x056, 0x30D, 0x009, 0x059, 0x05A, 0x30C,
        0x05C, 0x30B, 0x30A, 0x037, 0x007, 0x00A, 0x00B, 0x043, 0x00C, 0x045, 0x046, 0x309, 0x00D, 0x049, 0x04A, 0x308,
        0x04C, 0x307, 0x306, 0x036, 0x00E, 0x051, 0x052, 0x305, 0x054, 0x304, 0x303, 0x035, 0x058, 0x302, 0x301, 0x034,
        0x300, 0x033, 0x032, 0x007};
    constexpr u8 kBits[64] = {1, 4, 4, 8, 4, 8, 8, 10, 4, 8, 8, 10, 8, 10, 10, 13, 4, 8, 8, 10, 8, 10, 10, 13, 8, 10,
        10, 13, 10, 13, 13, 9, 4, 8, 8, 10, 8, 10, 10, 13, 8, 10, 10, 13, 10, 13, 13, 9, 8, 10, 10, 13, 10, 13, 13, 9,
        10, 13, 13, 9, 13, 9, 9, 6};
    u32 code = 0;
    for (u32 n = 1; n <= 13; n++) {
        code = code << 1 | b.bit();
        for (u32 i = 0; i < 64; i++)
            if (kBits[i] == n && kCodes[i] == code) return static_cast<int>(i);
    }
    return -1;
}

// A bitplane (8.7) passed over: INVERT, IMODE, then the plane's bits where this mode codes them here (raw coding has
// them in the macroblock layer). False when its codes do not match. `ones`: whether it is every macroblock's (INVERT,
// and nothing coded but zeros).
bool skip_bitplane(Bits& b, u32 mbw, u32 mbh, bool* ones = nullptr)
{
    const u8 invert = b.bit();
    bool zeros = true; // nothing but zeros coded
    if (ones) *ones = false;
    // IMODE: raw '0000', NORM-2 '10', DIFF-2 '001', NORM-6 '11', DIFF-6 '0001', row skip '010', column skip '011'.
    enum { RAW, NORM2, DIFF2, NORM6, DIFF6, ROWSKIP, COLSKIP } mode;
    if (b.bit()) mode = b.bit() ? NORM6 : NORM2;
    else if (b.bit()) mode = b.bit() ? COLSKIP : ROWSKIP;
    else if (b.bit()) mode = DIFF2;
    else mode = b.bit() ? DIFF6 : RAW;
    // Rows (or columns) each skipped ('0') or given bit by bit.
    const auto rowskip = [&](u32 columns, u32 rows) {
        for (u32 r = 0; r < rows; r++)
            if (b.bit()) {
                zeros = false;
                b.bits(columns);
            }
    };
    const auto colskip = [&](u32 columns, u32 rows) {
        for (u32 c = 0; c < columns; c++)
            if (b.bit()) {
                zeros = false;
                b.bits(rows);
            }
    };
    const auto done = [&] {
        if (ones) *ones = invert && zeros;
        return true;
    };
    switch (mode) {
    case RAW: return true;
    case NORM2:
    case DIFF2: {
        const u32 n = mbw * mbh;
        if ((n & 1) && b.bit()) zeros = false;
        for (u32 i = n & 1; i < n; i += 2) // '0', '100', '101', '11'
            if (b.bit()) {
                zeros = false;
                if (!b.bit()) b.bit();
            }
        return done();
    }
    case NORM6:
    case DIFF6:
        if (mbh % 3 == 0 && mbw % 3 != 0) { // 2x3 tiles, a column left
            for (u32 y = 0; y < mbh; y += 3)
                for (u32 x = mbw & 1; x < mbw; x += 2) {
                    const int tile = skip_tile(b);
                    if (tile < 0) return false;
                    if (tile) zeros = false;
                }
            if (mbw & 1) colskip(1, mbh);
        } else { // 3x2 tiles, columns and a row left
            for (u32 y = mbh & 1; y < mbh; y += 2)
                for (u32 x = mbw % 3; x < mbw; x += 3) {
                    const int tile = skip_tile(b);
                    if (tile < 0) return false;
                    if (tile) zeros = false;
                }
            if (mbw % 3) colskip(mbw % 3, mbh);
            if (mbh & 1) rowskip(mbw - mbw % 3, 1);
        }
        return done();
    case ROWSKIP: rowskip(mbw, mbh); return done();
    case COLSKIP: colskip(mbw, mbh); return done();
    }
    return false;
}

u32 start_code(const u8* p, u32 size, u32 from)
{
    for (u32 i = from; i + 3 < size; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return size;
}

// The advanced profile's sequence header (6.1) into the record's fields; the coding tools' fields are the entry
// point's as it stands. The bits read.
u64 parse_sequence(Svc1d& x, const std::vector<u8>& d)
{
    Bits b{d};
    SeqFields& s = x.rec.seq;
    s.profile = static_cast<u8>(b.bits(2));
    s.level = static_cast<u8>(b.bits(3));
    s.colordiff = static_cast<u8>(b.bits(2));
    s.frmrtq = static_cast<u8>(b.bits(3));
    s.bitrtq = static_cast<u8>(b.bits(5));
    s.postprocflag = b.bit();
    s.width = (b.bits(12) + 1) * 2;
    s.height = (b.bits(12) + 1) * 2;
    s.pulldown = b.bit();
    s.interlace = b.bit();
    s.tfcntrflag = b.bit();
    s.finterpflag = b.bit();
    b.bit();
    s.psf = b.bit();
    s.display_ext = b.bit();
    if (s.display_ext) {
        s.display_w = static_cast<u16>(b.bits(14));
        s.display_h = static_cast<u16>(b.bits(14));
        s.aspect_flag = b.bit();
        if (s.aspect_flag) {
            s.aspect = static_cast<u8>(b.bits(4));
            if (s.aspect == 15) {
                s.aspect_w = static_cast<u8>(b.bits(8));
                s.aspect_h = static_cast<u8>(b.bits(8));
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
        s.color_flag = b.bit();
        if (s.color_flag) {
            s.color_prim = static_cast<u8>(b.bits(8));
            s.transfer = static_cast<u8>(b.bits(8));
            s.matrix = static_cast<u8>(b.bits(8));
        }
    }
    x.rec.entry.hrd_buckets = 0;
    if (b.bit()) x.rec.entry.hrd_buckets = static_cast<u8>(b.bits(5));
    const EntryFields& e = x.rec.entry;
    s.loopfilter = e.loopfilter;
    s.fastuvmc = e.fastuvmc;
    s.extended_mv = e.extended_mv;
    s.dquant = e.dquant;
    s.vstransform = e.vstransform;
    s.overlap = e.overlap;
    s.quantizer = e.quantizer;
    s.multires = s.syncmarker = s.rangered = s.maxbframes = 0;
    x.width = s.width;
    x.height = s.height;
    x.sequence = true;
    x.simple = false;
    x.lines32 = true;
    return b.pos;
}

// A simple or main profile sequence in force (STRUCT_C's fields in the record): its coded size the one in force
// (SetPictureSize's), its coding tools the pictures' as an entry point gives them in the advanced profile.
void simple_sequence(Svc1d& x)
{
    SeqFields& s = x.rec.seq;
    // (Its size the one in force: in a new instance before SetPictureSize, the program's 2x2, from size codes of 0.)
    s.width = x.width ? x.width : 2;
    s.height = x.height ? x.height : 2;
    EntryFields& e = x.rec.entry;
    e.loopfilter = s.loopfilter;
    e.fastuvmc = s.fastuvmc;
    e.extended_mv = s.extended_mv;
    e.dquant = s.dquant;
    e.vstransform = s.vstransform;
    e.overlap = s.overlap;
    e.quantizer = s.quantizer;
    e.extended_dmv = e.panscan = e.refdist = 0;
    x.sequence = true;
    x.simple = true;
}

// STRUCT_C (Annex J: the simple and main profiles' sequence layer, as DecodeSeqHeader takes it behind a sequence
// header's start code) into the record's fields: the profile and its two reserved bits, the post-processing hints,
// the coding tools (RES_X8, RES_FASTTX, RES_TRANSTAB passed over), MAXBFRAMES, QUANTIZER, FINTERPFLAG.
void parse_struct_c(Svc1d& x, const std::vector<u8>& d)
{
    Bits b{d};
    SeqFields& s = x.rec.seq;
    s.profile = static_cast<u8>(b.bits(2));
    b.bits(2);
    s.frmrtq = static_cast<u8>(b.bits(3));
    s.bitrtq = static_cast<u8>(b.bits(5));
    s.loopfilter = b.bit();
    b.bit();
    s.multires = b.bit();
    b.bit();
    s.fastuvmc = b.bit();
    s.extended_mv = b.bit();
    s.dquant = static_cast<u8>(b.bits(2));
    s.vstransform = b.bit();
    b.bit();
    s.overlap = b.bit();
    s.syncmarker = b.bit();
    s.rangered = b.bit();
    s.maxbframes = static_cast<u8>(b.bits(3));
    s.quantizer = static_cast<u8>(b.bits(2));
    s.finterpflag = b.bit();
    simple_sequence(x);
}

// DecodeSeqHeader's with no sequence header in its bytes: a simple profile sequence of no coding tool but SYNCMARKER
// (what Sony's program records then).
void default_simple_sequence(Svc1d& x)
{
    SeqFields& s = x.rec.seq;
    s.profile = s.frmrtq = s.bitrtq = s.finterpflag = 0;
    s.loopfilter = s.multires = s.fastuvmc = s.extended_mv = s.dquant = s.vstransform = s.overlap = 0;
    s.rangered = s.maxbframes = s.quantizer = 0;
    s.syncmarker = 1;
    simple_sequence(x);
}

void write_sequence(u8* base, const Svc1d& x)
{
    const SeqFields& s = x.rec.seq;
    const u32 r = x.at.seq;
    ps_write8(base, r + 0x00, 1);
    ps_write32(base, r + 0x04, s.width);
    ps_write32(base, r + 0x08, s.height);
    const u8 tools[] = {s.profile, s.frmrtq, s.bitrtq, s.finterpflag, s.loopfilter, s.multires, s.fastuvmc,
        s.extended_mv, s.dquant, s.vstransform, s.overlap, s.syncmarker, s.rangered, s.maxbframes, s.quantizer,
        s.level, s.colordiff, s.postprocflag, s.pulldown, s.interlace, s.tfcntrflag, s.psf, s.display_ext};
    for (u32 k = 0; k < sizeof(tools); k++) ps_write8(base, r + 0x0C + k, tools[k]);
    ps_write16(base, r + 0x24, s.display_w);
    ps_write16(base, r + 0x26, s.display_h);
    const u8 tail[] = {s.aspect_flag, s.aspect, s.aspect_w, s.aspect_h, s.framerate_flag, s.framerate_ind,
        s.framerate_nr, s.framerate_dr};
    for (u32 k = 0; k < sizeof(tail); k++) ps_write8(base, r + 0x28 + k, tail[k]);
    ps_write16(base, r + 0x30, s.framerate_exp & 0xFF); // the program keeps FRAMERATEEXP's low byte
    ps_write8(base, r + 0x32, s.color_flag);
    ps_write8(base, r + 0x33, s.color_prim);
    ps_write8(base, r + 0x34, s.transfer);
    ps_write8(base, r + 0x35, s.matrix);
}

// The picture record's fields.
void set(Svc1d& x, u32 at, u8 v) { x.rec.pic[at] = v; }
void set16(Svc1d& x, u32 at, u16 v)
{
    set(x, at, static_cast<u8>(v >> 8));
    set(x, at + 1, static_cast<u8>(v));
}
void set32(Svc1d& x, u32 at, u32 v)
{
    set16(x, at, static_cast<u16>(v >> 16));
    set16(x, at + 2, static_cast<u16>(v));
}

// The entry point (6.2) into the record.
void parse_entry(Svc1d& x, const std::vector<u8>& d)
{
    Bits b{d};
    EntryFields& e = x.rec.entry;
    e.broken_link = b.bit();
    e.closed_entry = b.bit();
    e.panscan = b.bit();
    e.refdist = b.bit();
    e.loopfilter = b.bit();
    e.fastuvmc = b.bit();
    e.extended_mv = b.bit();
    e.dquant = static_cast<u8>(b.bits(2));
    e.vstransform = b.bit();
    e.overlap = b.bit();
    e.quantizer = static_cast<u8>(b.bits(2));
    for (u32 i = 0; i < e.hrd_buckets; i++) b.bits(8);
    e.coded_size = b.bit();
    if (e.coded_size) {
        e.coded_w = static_cast<u16>(b.bits(12));
        e.coded_h = static_cast<u16>(b.bits(12));
    } else {
        e.coded_w = static_cast<u16>(x.rec.seq.width / 2 - 1);
        e.coded_h = static_cast<u16>(x.rec.seq.height / 2 - 1);
    }
    e.extended_dmv = e.extended_mv ? b.bit() : 0;
    e.range_mapy_flag = b.bit();
    if (e.range_mapy_flag) e.range_mapy = static_cast<u8>(b.bits(3));
    e.range_mapuv_flag = b.bit();
    if (e.range_mapuv_flag) e.range_mapuv = static_cast<u8>(b.bits(3));
    set(x, 0x00, 1);
    set(x, 0x01, x.first_entry ? 1 : 0);
    x.first_entry = false;
    const u8 bytes[] = {e.broken_link, e.closed_entry, e.panscan, e.refdist, e.loopfilter, e.fastuvmc, e.extended_mv,
        e.dquant, e.vstransform, e.overlap, e.quantizer, e.coded_size};
    for (u32 k = 0; k < sizeof(bytes); k++) set(x, 0x02 + k, bytes[k]);
    set16(x, 0x0E, e.coded_w);
    set16(x, 0x10, e.coded_h);
    set(x, 0x12, e.extended_dmv);
    set(x, 0x13, e.range_mapy_flag);
    if (e.range_mapy_flag) set(x, 0x14, e.range_mapy);
    set(x, 0x15, e.range_mapuv_flag);
    if (e.range_mapuv_flag) set(x, 0x16, e.range_mapuv);
}

// BFRACTION's value in 256ths, by its index (the standard's table; 22, a simple or main profile's BI picture, 0xFF).
u8 bfraction(u32 index)
{
    constexpr u8 kTable[23] = {128, 85, 170, 64, 192, 51, 102, 153, 204, 43, 213, 37, 73, 110, 146, 183, 219, 32, 96,
        160, 224, 0, 0xFF};
    return index < 23 ? kTable[index] : 0;
}

// The PQUANT of a PQINDEX with the implicit quantizer.
u32 pquant(u32 pqindex, u32 quantizer)
{
    constexpr u8 kImplicit[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 27, 29, 31};
    return quantizer == 0 ? kImplicit[pqindex & 31] : pqindex;
}

// VOPDQUANT (7.1.1.31) into the record: DQUANTFRM (0 without DQUANT), DQPROFILE and its edge (single, double) or
// DQBILEVEL (0 under another profile), PQDIFF and ABSPQ, each kept when the header has none (DQUANT 2's: PQDIFF and
// ABSPQ alone, its frame quantizer at all four edges). Where it began, for FFmpeg (vop_dquant_decoding reads DQUANT
// 2's as no quantizer).
void parse_vopdquant(Svc1d& x, Bits& b)
{
    const EntryFields& e = x.rec.entry;
    if (!e.dquant) {
        set(x, 0x77, 0);
        return;
    }
    x.dq_at = b.pos;
    if (e.dquant != 2) {
        const u8 frm = b.bit();
        set(x, 0x77, frm);
        if (!frm) return;
        const u8 profile = static_cast<u8>(b.bits(2));
        set(x, 0x78, profile);
        set(x, 0x7B, 0); // (DQBILEVEL: the all-macroblocks profile's alone)
        if (profile == 2) {
            set(x, 0x79, static_cast<u8>(b.bits(2))); // DQSBEDGE
        } else if (profile == 1) {
            set(x, 0x7A, static_cast<u8>(b.bits(2))); // DQDBEDGE
        } else if (profile == 3) {
            const u8 bilevel = b.bit();
            set(x, 0x7B, bilevel);
            if (!bilevel) return;
        }
    }
    const u8 pqdiff = static_cast<u8>(b.bits(3));
    set(x, 0x75, pqdiff);
    if (pqdiff == 7) set(x, 0x76, static_cast<u8>(b.bits(5)));
}

// A code of up to three bits ('0', '10', '110', '111': MVRANGE, DMVRANGE) as the record keeps it, its bits' value.
u8 code3(Bits& b)
{
    u8 code = 0;
    for (u32 n = 0; n < 3; n++) {
        const u8 bit = b.bit();
        code = static_cast<u8>(code << 1 | bit);
        if (!bit) break;
    }
    return code;
}

// The motion vector modes as the record codes them: 1MV 0x01, half sample 0x03, half sample bilinear 0x07, mixed
// 0x08, intensity compensation 0x10 with its second mode.
enum : u8 { MV_1MV = 0x01, MV_HALF = 0x03, MV_BILINEAR = 0x07, MV_MIXED = 0x08, MV_IC = 0x10 };

// MVMODE (with MVMODE2 after intensity compensation's code) by its table (PQUANT up to 12, or above): its value.
u8 mvmode(Bits& b, bool low, bool second)
{
    const u32 last = second ? 3 : 4;
    u32 n = 0;
    while (n < last && !b.bit()) n++;
    constexpr u8 kLow[5] = {MV_1MV, MV_MIXED, MV_HALF, MV_IC, MV_BILINEAR},
                 kHigh[5] = {MV_BILINEAR, MV_1MV, MV_HALF, MV_IC, MV_MIXED};
    constexpr u8 kLow2[4] = {MV_1MV, MV_MIXED, MV_HALF, MV_BILINEAR};
    constexpr u8 kHigh2[4] = {MV_BILINEAR, MV_1MV, MV_HALF, MV_MIXED};
    if (second) return low ? kLow2[n] : kHigh2[n];
    return low ? kLow[n] : kHigh[n];
}

// PQINDEX to POSTPROC: PQUANT.
u32 parse_quantizer(Svc1d& x, Bits& b)
{
    const u32 pqindex = b.bits(5);
    set(x, 0x1C, static_cast<u8>(pqindex));
    set(x, 0x1D, pqindex <= 8 ? b.bit() : 0);
    if (x.rec.entry.quantizer == 1) set(x, 0x1E, b.bit());
    if (x.rec.seq.postprocflag && !x.simple) set(x, 0x69, static_cast<u8>(b.bits(2)));
    return pquant(pqindex, x.rec.entry.quantizer);
}

// An intra picture's or field's fields after the quantizer: FIELDTX (an interlaced frame's), ACPRED and OVERFLAGS
// (bitplanes, passed over), CONDOVER, the AC and DC tables, VOPDQUANT. An interlaced picture's records its 2MV and 4MV
// block pattern tables as none. False when a bitplane is not read here.
bool parse_intra(Svc1d& x, Bits& b, u32 pq, u32 mbw, u32 mbh, u8 fcm)
{
    const EntryFields& e = x.rec.entry;
    if (fcm == 1 && !skip_bitplane(b, mbw, mbh)) return false; // FIELDTX
    if (!skip_bitplane(b, mbw, mbh)) return false;              // ACPRED
    u8 condover = 0;
    if (e.overlap && pq <= 8) {
        condover = b.code012(true); // CONDOVER's code (3: OVERFLAGS, a bitplane)
        if (condover == 3 && !skip_bitplane(b, mbw, mbh)) return false;
    }
    set(x, 0x6A, condover);
    // The AC tables (chroma's TRANSACFRM, luma's TRANSACFRM2) and the DC table.
    set(x, 0x20, b.code012());
    set(x, 0x22, b.code012());
    set(x, 0x21, b.bit());
    parse_vopdquant(x, b);
    if (fcm) {
        set(x, 0x6F, 0);
        set(x, 0x70, 0);
    }
    return true;
}

// A P or B picture's last fields: VOPDQUANT, TTMBF and TTFRM (as the record codes it: 8x8 0x04, 8x4 0x0C, 4x8 0x14,
// 4x4 0x1C; 8x8's when per macroblock), the AC and DC tables. (A simple or main profile picture's VOPDQUANT is its
// caller's.)
void parse_inter_tail(Svc1d& x, Bits& b)
{
    parse_vopdquant(x, b);
    u8 ttmbf = 1, ttfrm = 0;
    if (x.rec.entry.vstransform) {
        ttmbf = b.bit();
        if (ttmbf) ttfrm = static_cast<u8>(b.bits(2));
    }
    set(x, 0x28, ttmbf);
    set(x, 0x29, static_cast<u8>(ttfrm << 3 | 4));
    // The AC table (chroma's and luma's both) and the DC table.
    const u8 ac = b.code012();
    set(x, 0x20, ac);
    set(x, 0x22, ac);
    set(x, 0x21, b.bit());
}

// A field's header from PQINDEX (a field picture's first in the frame BDU, its second in the FIELD BDU) into the
// record. False when a bitplane is not read here.
bool parse_field(Svc1d& x, Bits& b, u8 type)
{
    const EntryFields& e = x.rec.entry;
    const u32 pq = parse_quantizer(x, b);
    const u32 mbw = (x.width + 15) / 16, mbh = (x.height / 2 + 15) / 16;
    if (type == PT_I || type == PT_BI) {
        set(x, 0x72, 0); // (NUMREF none)
        return parse_intra(x, b, pq, mbw, mbh, 2);
    }
    const bool low = pq <= 12;
    if (type == PT_P) {
        const u8 numref = b.bit();
        set(x, 0x72, numref);
        if (!numref) set(x, 0x73, b.bit()); // REFFIELD
        if (e.extended_mv) set(x, 0x1F, code3(b));
        if (e.extended_dmv) set(x, 0x6B, code3(b));
        u8 mode = mvmode(b, low, false);
        if (mode == MV_IC) {
            mode = static_cast<u8>(MV_IC | mvmode(b, low, true));
            // INTCOMPFIELD ('1' both reference fields, '01' the second, '00' the first), each one's LUMSCALE and
            // LUMSHIFT.
            const u8 both = b.bit();
            const u8 fields = both ? 3 : (b.bit() ? 2 : 1);
            if (fields & 1) {
                set(x, 0x2A, static_cast<u8>(b.bits(6)));
                set(x, 0x2C, static_cast<u8>(b.bits(6)));
            }
            if (fields & 2) {
                set(x, 0x2B, static_cast<u8>(b.bits(6)));
                set(x, 0x2D, static_cast<u8>(b.bits(6)));
            }
        }
        set(x, 0x25, mode);
        set(x, 0x6E, static_cast<u8>(b.bits(3)));          // MBMODETAB
        set(x, 0x26, static_cast<u8>(b.bits(2 + numref))); // IMVTAB
        set(x, 0x27, static_cast<u8>(b.bits(3)));          // ICBPTAB
        set(x, 0x6F, 0);                                    // (no 2MVBPTAB)
        set(x, 0x70, (mode & 0xF) == MV_MIXED ? static_cast<u8>(b.bits(2)) : 0); // 4MVBPTAB
    } else {
        set(x, 0x72, 1); // (a B field's two references)
        if (e.extended_mv) set(x, 0x1F, code3(b));
        if (e.extended_dmv) set(x, 0x6B, code3(b));
        const u8 mode = mvmode(b, low, true);
        set(x, 0x25, mode);
        if (!skip_bitplane(b, mbw, mbh)) return false; // FORWARDMB
        set(x, 0x6E, static_cast<u8>(b.bits(3)));     // MBMODETAB
        set(x, 0x26, static_cast<u8>(b.bits(3)));     // IMVTAB
        set(x, 0x27, static_cast<u8>(b.bits(3)));     // ICBPTAB
        set(x, 0x6F, 0);
        set(x, 0x70, mode == MV_MIXED ? static_cast<u8>(b.bits(2)) : 0);
    }
    parse_inter_tail(x, b);
    return true;
}

// A picture's header (the advanced profile's, 7.1: a progressive frame, an interlaced frame, a field picture and its
// first field) into the record: its type (0 when it has none). x.fcm: the frame coding mode (0 progressive, 1 an
// interlaced frame, 2 fields); for fields, x.field_types: the two fields' types (the second's header is
// parse_second_field()'s).
u8 parse_picture(Svc1d& x, const std::vector<u8>& d)
{
    Bits b{d};
    const SeqFields& s = x.rec.seq;
    const EntryFields& e = x.rec.entry;
    x.dq_at = 0;
    x.dmv_at = 0;
    x.fcm = s.interlace ? b.code012() : 0;
    u8 type = 0;
    if (x.fcm == 2) {
        // FPTYPE: I/I, I/P, P/I, P/P, B/B, B/BI, BI/B, BI/BI; the record's type 6 on.
        const u8 fptype = static_cast<u8>(b.bits(3));
        const auto field_type = [&](bool second) -> u8 {
            const bool other = second ? (fptype & 1) : (fptype & 2);
            if (fptype & 4) return other ? PT_BI : PT_B;
            return other ? PT_P : PT_I;
        };
        x.field_types[0] = field_type(false);
        x.field_types[1] = field_type(true);
        type = x.field_types[0];
        set(x, 0x1B, static_cast<u8>(6 + fptype));
    } else {
        u32 ones = 0;
        while (ones < 4 && b.bit()) ones++;
        constexpr u8 kTypes[5] = {PT_P, PT_B, PT_I, PT_BI, PT_SKIPPED};
        type = kTypes[ones];
        set(x, 0x1B, type);
    }
    set(x, 0x17, 1);
    if (s.tfcntrflag) set(x, 0x66, static_cast<u8>(b.bits(8)));
    // RPTFRM, or TFF and RFF; without them, one frame shown once, top field first.
    set(x, 0x62, 0);
    set(x, 0x63, 1);
    set(x, 0x64, 0);
    if (s.pulldown) {
        if (!s.interlace || s.psf) {
            set(x, 0x62, static_cast<u8>(b.bits(2)));
        } else {
            set(x, 0x63, b.bit());
            set(x, 0x64, b.bit());
        }
    }
    // The frame coding mode: an interlaced frame 1, a field 2 (the top) or 3 (the bottom).
    set(x, 0x61, x.fcm == 2 ? (x.rec.pic[0x63] ? 2 : 3) : x.fcm);
    x.ps_from = x.ps_to = 0;
    if (e.panscan) {
        x.ps_from = b.pos;
        // PS_PRESENT, and its windows: as many as the frame's fields shown (their count kept when there are none).
        const u8 present = b.bit();
        set(x, 0x65, present);
        if (present) {
            u32 n = 1;
            if (s.interlace && !s.psf) n = s.pulldown ? 2 + x.rec.pic[0x64] : 2;
            else if (s.pulldown) n = 1 + x.rec.pic[0x62];
            set(x, 0x60, static_cast<u8>(n));
            for (u32 k = 0; k < n && k < 4; k++) {
                const u32 at = 0x30 + 12 * k;
                set32(x, at, b.bits(18));
                set32(x, at + 4, b.bits(18));
                set16(x, at + 8, static_cast<u16>(b.bits(14)));
                set16(x, at + 10, static_cast<u16>(b.bits(14)));
            }
        }
        x.ps_to = b.pos;
    }
    if (type == PT_SKIPPED) return type;
    set(x, 0x67, b.bit()); // RNDCTRL
    if (s.interlace) set(x, 0x68, b.bit());
    if (x.fcm == 2) {
        // REFDIST (an I or P pair's, with REFDIST_FLAG: two bits, 3 on in a unary code), a B pair's BFRACTION; the
        // first field.
        if (e.refdist && type != PT_B && type != PT_BI) {
            u32 refdist = b.bits(2);
            if (refdist == 3)
                while (refdist < 16 && b.bit()) refdist++;
            set(x, 0x71, static_cast<u8>(refdist));
        }
        if (type == PT_B || type == PT_BI) {
            u32 index = b.bits(3);
            if (index == 7) index = 7 + b.bits(4);
            set(x, 0x24, bfraction(index));
        }
        parse_field(x, b, type);
        return type;
    }
    if (x.fcm == 0) {
        if (s.finterpflag) set(x, 0x18, b.bit());
        if (type == PT_B) {
            u32 index = b.bits(3);
            if (index == 7) index = 7 + b.bits(4);
            set(x, 0x24, bfraction(index));
        }
    }
    const u32 pq = parse_quantizer(x, b);
    const u32 mbw = (x.width + 15) / 16, mbh = (x.height + 15) / 16;
    set(x, 0x72, 0); // (NUMREF: fields' alone)
    if (type == PT_I || type == PT_BI) {
        parse_intra(x, b, pq, mbw, mbh, x.fcm);
        return type;
    }
    if (x.fcm == 1) {
        // An interlaced frame's P or B picture: BFRACTION (a B picture's, here), MVRANGE, DMVRANGE, 4MVSWITCH (P),
        // INTCOMP (with LUMSCALE and LUMSHIFT; the record's mode 0x10 or 0), the DIRECTMB (B) and SKIPMB bitplanes,
        // MBMODETAB, IMVTAB, ICBPTAB, 2MVBPTAB, 4MVBPTAB (B, or P with 4MVSWITCH).
        if (type == PT_B) {
            u32 index = b.bits(3);
            if (index == 7) index = 7 + b.bits(4);
            set(x, 0x24, bfraction(index));
        }
        if (e.extended_mv) set(x, 0x1F, code3(b));
        if (e.extended_dmv) set(x, 0x6B, code3(b));
        u8 fourmv = 1;
        if (type == PT_P) fourmv = b.bit();
        set(x, 0x6C, type == PT_P ? fourmv : 0);
        const u8 intcomp = b.bit();
        set(x, 0x6D, intcomp);
        if (intcomp) {
            set(x, 0x2A, static_cast<u8>(b.bits(6)));
            set(x, 0x2C, static_cast<u8>(b.bits(6)));
        }
        set(x, 0x25, intcomp ? MV_IC : 0);
        if (type == PT_B && !skip_bitplane(b, mbw, mbh)) return type; // DIRECTMB
        if (!skip_bitplane(b, mbw, mbh)) return type;                // SKIPMB
        set(x, 0x6E, static_cast<u8>(b.bits(2))); // MBMODETAB
        set(x, 0x26, static_cast<u8>(b.bits(2))); // IMVTAB
        set(x, 0x27, static_cast<u8>(b.bits(3))); // ICBPTAB
        set(x, 0x6F, static_cast<u8>(b.bits(2))); // 2MVBPTAB
        set(x, 0x70, fourmv ? static_cast<u8>(b.bits(2)) : 0); // 4MVBPTAB
        parse_inter_tail(x, b);
        return type;
    }
    if (e.extended_mv) set(x, 0x1F, code3(b));
    // (FFmpeg reads a DMVRANGE here in an interlaced sequence's progressive P pictures, which have none.)
    if (s.interlace && e.extended_dmv && type == PT_P) x.dmv_at = b.pos;
    if (type == PT_P) {
        const bool low = pq <= 12;
        u8 mode = mvmode(b, low, false);
        if (mode == MV_IC) {
            mode = static_cast<u8>(MV_IC | mvmode(b, low, true));
            const u8 scale = static_cast<u8>(b.bits(6)), shift = static_cast<u8>(b.bits(6));
            set(x, 0x2A, scale);
            set(x, 0x2B, scale);
            set(x, 0x2C, shift);
            set(x, 0x2D, shift);
        }
        set(x, 0x25, mode);
        if ((mode & 0xF) == MV_MIXED && !skip_bitplane(b, mbw, mbh)) return type; // MVTYPEMB
    } else {
        set(x, 0x25, b.bit() ? MV_1MV : MV_BILINEAR);
        if (!skip_bitplane(b, mbw, mbh)) return type; // DIRECTMB
    }
    if (!skip_bitplane(b, mbw, mbh)) return type; // SKIPMB
    set(x, 0x26, static_cast<u8>(b.bits(2)));       // MVTAB
    set(x, 0x27, static_cast<u8>(b.bits(2)));       // CBPTAB
    parse_inter_tail(x, b);
    return type;
}

// A simple or main profile frame (7.1, behind a frame's start code as the program takes it) into the record: the
// sequence's coding tools where an entry point's are (no entry point: its flags 0, a closed one, the coded size
// SetPictureSize's), then its header: INTERPFRM, FRMCNT, RANGEREDFRM, PTYPE, BFRACTION (22: a BI picture), BF (passed
// over), the quantizer, MVRANGE, RESPIC, and as the advanced profile's progressive pictures; RNDCTRL 1 at each I and
// BI picture, toggled at each P picture. Its type.
u8 parse_simple_picture(Svc1d& x, const std::vector<u8>& d)
{
    Bits b{d};
    const SeqFields& s = x.rec.seq;
    const EntryFields& e = x.rec.entry;
    x.dq_at = x.dmv_at = x.ps_from = x.ps_to = 0;
    x.header_end = 0;
    x.fcm = 0;
    // The entry point's part of the record: not an entry point's, the sequence's coding tools and the coded size; the
    // rest (the first-entry flag, broken link, closed entry, pan-scan, reference distance, the coded size flag,
    // extended differential vectors, range mapping) as an earlier entry point left it.
    set(x, 0x00, 0);
    const u8 tools[] = {e.loopfilter, e.fastuvmc, e.extended_mv, e.dquant, e.vstransform, e.overlap, e.quantizer};
    for (u32 k = 0; k < sizeof(tools); k++) set(x, 6 + k, tools[k]);
    set16(x, 0x0E, static_cast<u16>(x.width / 2 - 1));
    set16(x, 0x10, static_cast<u16>(x.height / 2 - 1));
    // A frame of no byte or one: a skipped P picture (the anchor before it again).
    if (d.size() <= 1) {
        set(x, 0x17, 1);
        set(x, 0x1B, PT_SKIPPED);
        x.header_end = d.size() * 8ull;
        return PT_SKIPPED;
    }
    if (s.finterpflag) set(x, 0x18, b.bit());
    set(x, 0x19, static_cast<u8>(b.bits(2))); // FRMCNT
    if (s.rangered) set(x, 0x1A, b.bit()); // RANGEREDFRM
    u8 type = PT_I;
    if (b.bit()) type = PT_P;
    else if (s.maxbframes && !b.bit()) type = PT_B;
    if (type == PT_B) {
        u32 index = b.bits(3);
        if (index == 7) index = 7 + b.bits(4);
        set(x, 0x24, bfraction(index));
        if (index == 22) type = PT_BI;
    }
    set(x, 0x17, 1);
    set(x, 0x1B, type);
    set(x, 0x61, 0);
    set(x, 0x62, 0);
    set(x, 0x63, 1);
    set(x, 0x64, 0);
    if (type == PT_I || type == PT_BI) {
        b.bits(7); // BF
        x.rnd = 1;
    } else if (type == PT_P) {
        x.rnd ^= 1;
    }
    set(x, 0x67, x.rnd);
    set(x, 0x72, 0);
    const u32 pq = parse_quantizer(x, b);
    if (e.extended_mv) set(x, 0x1F, code3(b));
    if (s.multires && type != PT_B && b.bits(2))
        PS_LOG_WARN(kChan, "instance {:#x}: a picture at a smaller resolution (RESPIC), not reproduced", x.handle);
    // The macroblock rows the program reads: the height its frames are laid out for (to 32 when SetPictureSize set
    // them up under an advanced profile sequence, a row more than a height of 16's odd rows).
    const u32 mbw = (x.width + 15) / 16, mbh = align(x.height, x.lines32 ? 32 : 16) / 16;
    if (type == PT_I || type == PT_BI) {
        // The AC tables (chroma's TRANSACFRM, luma's TRANSACFRM2) and the DC table.
        set(x, 0x20, b.code012());
        set(x, 0x22, b.code012());
        set(x, 0x21, b.bit());
        return type;
    }
    if (type == PT_P) {
        const bool low = pq <= 12;
        u8 mode = mvmode(b, low, false);
        if (mode == MV_IC) {
            mode = static_cast<u8>(MV_IC | mvmode(b, low, true));
            const u8 scale = static_cast<u8>(b.bits(6)), shift = static_cast<u8>(b.bits(6));
            set(x, 0x2A, scale);
            set(x, 0x2B, scale);
            set(x, 0x2C, shift);
            set(x, 0x2D, shift);
        }
        set(x, 0x25, mode);
        if ((mode & 0xF) == MV_MIXED && !skip_bitplane(b, mbw, mbh)) return type; // MVTYPEMB
    } else {
        set(x, 0x25, b.bit() ? MV_1MV : MV_BILINEAR);
        if (!skip_bitplane(b, mbw, mbh)) return type; // DIRECTMB
    }
    bool skipped_mbs = false;
    if (!skip_bitplane(b, mbw, mbh, &skipped_mbs)) return type; // SKIPMB
    set(x, 0x26, static_cast<u8>(b.bits(2)));                    // MVTAB
    set(x, 0x27, static_cast<u8>(b.bits(2)));                    // CBPTAB
    parse_inter_tail(x, b);
    if (skipped_mbs) x.header_end = b.pos; // (no macroblock layer: the frame ends here)
    return type;
}

// A field picture's second field (its FIELD BDU's payload) into the record.
void parse_second_field(Svc1d& x, const std::vector<u8>& d)
{
    Bits b{d};
    x.dq_at = x.dmv_at = x.ps_from = x.ps_to = 0;
    set(x, 0x61, x.rec.pic[0x63] ? 3 : 2);
    parse_field(x, b, x.field_types[1]);
}

// A BDU for FFmpeg's decoder where it reads the stream otherwise than the standard (its pan-scan fields it does not
// read, DQUANT 2 it reads as no frame quantizer): payload `d` with bits [from, to) of each edit (in order) replaced
// by its own (0 or 1 each), escaped again.
struct BitEdit {
    u64 from = 0, to = 0;
    std::vector<u8> insert;
};
std::vector<u8> edited_bdu(u8 code, const std::vector<u8>& d, const std::vector<BitEdit>& edits)
{
    std::vector<u8> out;
    u32 acc = 0, n = 0;
    const auto put = [&](u32 bit) {
        acc = acc << 1 | bit;
        if (++n == 8) {
            out.push_back(static_cast<u8>(acc));
            acc = n = 0;
        }
    };
    const u64 total = d.size() * 8ull;
    const auto bit = [&](u64 i) { return static_cast<u32>((d[i / 8] >> (7 - i % 8)) & 1); };
    u64 i = 0;
    for (const BitEdit& e : edits) {
        for (; i < e.from && i < total; i++) put(bit(i));
        for (const u8 v : e.insert) put(v);
        i = std::max(i, e.to);
    }
    for (; i < total; i++) put(bit(i));
    if (n) out.push_back(static_cast<u8>(acc << (8 - n)));
    std::vector<u8> bdu = {0x00, 0x00, 0x01, code};
    u32 zeros = 0;
    for (const u8 c : out) {
        if (zeros >= 2 && c <= 3) {
            bdu.push_back(0x03);
            zeros = 0;
        }
        bdu.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return bdu;
}

// The entry point FFmpeg's VC-1 decoder is given after a simple or main profile's STRUCT_C (it takes no sequence
// without one): the sequence's coding tools (DQUANT 2 as 1, the pictures' VOPDQUANT rewritten to match), closed, the
// coded size in force.
std::vector<u8> simple_entry_for_ffmpeg(const Svc1d& x)
{
    const SeqFields& s = x.rec.seq;
    std::vector<u8> bits;
    const auto put = [&](u32 v, u32 n) {
        for (u32 i = n; i-- > 0;) bits.push_back(static_cast<u8>((v >> i) & 1));
    };
    put(0, 1); // BROKEN_LINK
    put(1, 1); // CLOSED_ENTRY
    put(0, 2); // PANSCAN_FLAG, REFDIST_FLAG
    put(s.loopfilter, 1);
    put(s.fastuvmc, 1);
    put(s.extended_mv, 1);
    put(s.dquant == 2 ? 1 : s.dquant, 2);
    put(s.vstransform, 1);
    put(s.overlap, 1);
    put(s.quantizer, 2);
    put(1, 1); // CODED_SIZE_FLAG
    put(x.width / 2 - 1, 12);
    put(x.height / 2 - 1, 12);
    if (s.extended_mv) put(0, 1); // EXTENDED_DMV
    put(0, 2);                     // RANGE_MAPY_FLAG, RANGE_MAPUV_FLAG
    put(1, 1);                     // the stop bit
    while (bits.size() % 8) bits.push_back(0);
    std::vector<u8> payload;
    for (usize i = 0; i < bits.size(); i += 8) {
        u8 c = 0;
        for (usize k = 0; k < 8; k++) c = static_cast<u8>(c << 1 | bits[i + k]);
        payload.push_back(c);
    }
    return edited_bdu(0x0E, payload, {});
}

// What FFmpeg is given of the last picture (or field) parsed: its BDU without the pan-scan fields (which FFmpeg does
// not read), with DQUANT 2's VOPDQUANT as DQUANT 1's with all four edges (which FFmpeg reads as no quantizer), with
// a DMVRANGE 0 where FFmpeg reads one an interlaced sequence's progressive P picture has not.
std::vector<BitEdit> ffmpeg_edits(const Svc1d& x)
{
    std::vector<BitEdit> edits;
    if (x.ps_to > x.ps_from) edits.push_back({x.ps_from, x.ps_to, {}});
    if (x.dmv_at) edits.push_back({x.dmv_at, x.dmv_at, {0}});
    if (x.dq_at && x.rec.entry.dquant == 2) edits.push_back({x.dq_at, x.dq_at, {1, 0, 0}});
    std::sort(edits.begin(), edits.end(), [](const BitEdit& a, const BitEdit& b) { return a.from < b.from; });
    return edits;
}

// ---- the picture queue

// The first entry neither holding a picture nor an anchor, -1 when none.
int free_entry(const Svc1d& x)
{
    for (int k = 0; k < static_cast<int>(kEntries); k++)
        if (!x.entry[k].used && k != x.newest && k != x.older) return k;
    return -1;
}
// The first frame buffer no entry holds and no anchor uses.
int free_frame(const Svc1d& x)
{
    for (int f = 0; f < static_cast<int>(kEntries); f++) {
        bool busy = false;
        for (u32 k = 0; k < kEntries; k++)
            if ((x.entry[k].used || static_cast<int>(k) == x.newest || static_cast<int>(k) == x.older) &&
                x.entry[k].frame == f)
                busy = true;
        if (!busy) return f;
    }
    return -1;
}
void make_ready(Svc1d& x, int k)
{
    x.entry[k].ready = true;
    x.entry[k].seq = x.next_seq;
    x.next_seq = (x.next_seq + 1) & 0x7F;
}
void reset_queue(Svc1d& x)
{
    for (Picture& p : x.entry) p = {};
    x.newest = x.older = x.current = -1;
    x.next_seq = x.get = x.release = 0;
    x.flushed = false;
}

// FlushPicture's, and an end of sequence's: the newest anchor given out, none to predict from.
void flush_anchor(Svc1d& x)
{
    if (x.newest >= 0) {
        make_ready(x, x.newest);
        x.older = x.newest;
        x.newest = -1;
    }
    x.flushed = true;
}

u32 frame_of(const Svc1d& x, int f) { return x.at.frame0 + static_cast<u32>(f) * frame_bytes(x.max_w, x.max_h); }

// GetPicture's fourth word: how the picture is shown (a progressive sequence's frame 4 and its repeats; an interlaced
// sequence's 0x10, a field picture 0x400, the bottom field first 0x1000, the first field repeated 0x4000).
u32 picture_format(const Svc1d& x)
{
    if (!x.rec.seq.interlace) return 0x40000004 | static_cast<u32>(x.rec.pic[0x62]) << 14;
    return 0x40000010 | (x.records == 2 ? 0x400u : 0u) | (x.rec.pic[0x63] ? 0u : 0x1000u) |
           (x.rec.pic[0x64] ? 0x4000u : 0u);
}

// The entry's descriptor and information block: the frame's addresses, the size and lines, the record, the status
// and the auxiliary data the access unit was given.
void write_entry(u8* base, const Svc1d& x, int k, u32 status)
{
    const vdec::Plan p = plan(x.lines32);
    const u32 frame = frame_of(x, x.entry[k].frame), w = x.width, h = x.height;
    const u32 d = x.at.desc[k];
    const u32 cb = frame + vdec::strip_cb(p, w, h);
    ps_write32(base, d + 0x00, frame + vdec::strip_start(p, h));
    ps_write32(base, d + 0x04, cb);
    ps_write32(base, d + 0x08, cb + 8);
    ps_write32(base, d + 0x0C, w);
    ps_write32(base, d + 0x10, h);
    const u32 hl = align(h, x.lines32 ? 32 : 16);
    ps_write32(base, d + 0x14, hl + 128);
    ps_write32(base, d + 0x18, hl / 2 + 64);
    ps_write32(base, d + 0x1C, picture_format(x));
    // The records, a field picture's two (their two gaps, at 0x2E and 0xFE, never written).
    const u32 info = x.at.info[k];
    for (u32 f = 0; f < x.records; f++) {
        const u8* rec = x.records == 2 && f == 0 ? x.first_record : x.rec.pic;
        for (u32 i = 0; i < 0xFE; i++)
            if (i != 0x2E && i != 0x2F) ps_write8(base, info + f * kRecord + i, rec[i]);
        ps_write32(base, info + 0x204 + 4 * f, status);
    }
    ps_write8(base, info + 0x203, x.records);
    // The auxiliary data: the caller's for this access unit (SetAuxData's, then forgotten), and whether it gave any.
    std::memmove(base + info + 0x20C, base + x.handle + kAuxNow, 16);
    ps_write8(base, info + 0x21C, ps_read8(base, x.handle + 0x4C));
}

bool open_ffmpeg(Svc1d& x, const std::vector<u8>& config)
{
#if PSPOCKET_HAVE_FFMPEG
    if (!x.ff_open) {
        // Each picture out as soon as it is decoded: Sony's program has it in its frame buffer then (and FFmpeg's
        // references are the pictures it gave, rewritten as the program's frame buffers hold them).
        x.ff_open = x.ff.open(AV_CODEC_ID_VC1, 0, 0, config, 0, AV_CODEC_FLAG_LOW_DELAY);
        if (!x.ff_open) PS_LOG_ERROR(kChan, "FFmpeg's VC-1 decoder did not open");
        x.ff.keep_pictures(true);
        x.ff_fwd = x.ff_bwd = 0;
        x.ff_anchor_packet.clear();
    }
    return x.ff_open;
#else
    (void)x;
    (void)config;
    return false;
#endif
}

// FFmpeg's reference (the picture of access unit `au`) made to hold frame buffer `f` as it is, when it does not.
void match_reference(const u8* base, Svc1d& x, u64 au, int f)
{
    std::vector<u8> y, u, v;
    vdec::read_strips(base, frame_of(x, f), plan(x.lines32), x.width, x.height, y, u, v);
    if (x.ff.holds(au, x.width, x.height, y.data(), u.data(), v.data())) return;
    if (!x.ff.rewrite(au, x.width, x.height, y.data(), u.data(), v.data()))
        PS_LOG_WARN(kChan, "instance {:#x}: FFmpeg's reference (access unit {}) not rewritten", x.handle, au);
}

// FFmpeg given an anchor's packet to have one more reference (its picture not placed).
void prime(Svc1d& x, const std::vector<u8>& packet, std::deque<vdec::DecodedPicture>& out)
{
    const u64 au = x.next_au++;
    x.ff.decode(packet.data(), static_cast<u32>(packet.size()), au, out);
    x.ff_fwd = x.ff_bwd;
    x.ff_bwd = au;
}

// The pictures FFmpeg gave: into their entries' frame buffers.
void place(u8* base, Svc1d& x, std::deque<vdec::DecodedPicture>& out)
{
    while (!out.empty()) {
        vdec::DecodedPicture pic = std::move(out.front());
        out.pop_front();
        const auto it = x.waiting.find(pic.au);
        if (it == x.waiting.end()) continue;
        const int k = it->second;
        x.waiting.erase(it);
        if (k < 0 || x.entry[k].frame < 0) continue;
        if (x.entry[k].expand)
            for (auto* plane : {&pic.y, &pic.u, &pic.v})
                for (u8& v : *plane) v = static_cast<u8>(std::clamp((v - 128) * 2 + 128, 0, 255));
        vdec::write_strips(base, frame_of(x, x.entry[k].frame), plan(x.lines32), pic.width, pic.height, pic.y.data(),
            pic.u.data(), pic.v.data(), true, x.entry[k].fields);
    }
}

// Where the program stops when it passes the rest of an access unit over: at the next start code from `from`, or
// where its search for one ends, three bytes before the end.
u32 passed_over(const u8* p, u32 size, u32 from)
{
    const u32 at = start_code(p, size, from);
    if (at < size) return at;
    return std::max(from, size >= 3 ? size - 3 : 0u);
}

// The command to decode an access unit (address and size, after joining its pieces): what the program does with it.
u32 spu_decode(u8* base, Svc1d& x, const u8* p, u32 size, u32& used)
{
    used = 0;
    x.flag = 0;
    const int k = free_entry(x);
    if (k < 0) return score::ST_BUSY;
    if (x.mode_byte & 1) return score::ST_NO_PICTURE; // every access unit passed over, unread
    u32 picture_at = size;
    u8 type = 0;
    u32 from = 0; // where the program's search for the next start code goes on
    bool end_after = false, end_alone = false;
    bool dropped = false; // a picture with user data after it the program does not take there
    // What FFmpeg is given: the headers before the frame, the frame (each as FFmpeg reads it, D-920).
    std::vector<u8> headers_for_ffmpeg, frame_for_ffmpeg;
    for (u32 at = start_code(p, size, 0); at < size;) {
        const u32 next = start_code(p, size, at + 4);
        const u8 code = p[at + 3];
        from = at + 4;
        // After Initialize: an access unit that does not begin with a sequence header passed over.
        if (x.wait == Wait::Sequence && code != 0x0F) {
            used = passed_over(p, size, at + 4);
            return score::ST_NO_PICTURE;
        }
        // User data before the frame: a sequence's or entry point's read to the next start code, and the BDU
        // there passed over with it (the program reads on after that start code); a frame's, a field's or a
        // slice's, the access unit's end (no picture).
        if (is_user_data(code)) {
            x.flag |= user_flag(code);
            if (code <= 0x1D) {
                used = passed_over(p, size, at + 4);
                return score::ST_NO_PICTURE;
            }
            if (next >= size) break;
            from = next + 4;
            at = start_code(p, size, next + 4);
            continue;
        }
        const std::vector<u8> d = unescape(p + at + 4, next - at - 4);
        // A header's bytes as FFmpeg's configuration takes them (the zeros before the next start code left); the
        // program's reading ends at the byte with its stop bit.
        u32 end = next;
        while (end > at + 4 && p[end - 1] == 0) end--;
        from = end > at + 4 ? end - 1 : at + 4;
        if (code == 0x0A) {
            x.flag |= EX_END;
            end_alone = true;
            flush_anchor(x);
        } else if (code == 0x0F) {
            // The advanced profile's sequence header, or a simple or main profile's STRUCT_C.
            if (!d.empty() && (d[0] >> 6) != 3) parse_struct_c(x, d);
            else parse_sequence(x, d);
            x.flag |= EX_SEQUENCE;
            write_sequence(base, x);
            x.seq_bdu.assign(p + at, p + end);
            headers_for_ffmpeg.insert(headers_for_ffmpeg.end(), x.seq_bdu.begin(), x.seq_bdu.end());
            if (x.wait == Wait::Sequence) x.wait = Wait::Nothing;
        } else if (code == 0x0E) {
            parse_entry(x, d);
            x.flag |= EX_ENTRY;
            // DQUANT 2 as DQUANT 1 for FFmpeg (its pictures' VOPDQUANT rewritten to match).
            if (x.rec.entry.dquant == 2) x.entry_bdu = edited_bdu(0x0E, d, {{7, 9, {0, 1}}});
            else x.entry_bdu.assign(p + at, p + end);
            headers_for_ffmpeg.insert(headers_for_ffmpeg.end(), x.entry_bdu.begin(), x.entry_bdu.end());
            if (x.wait == Wait::Entry) x.wait = Wait::Nothing;
        } else if (code == 0x0D) {
            x.flag |= EX_PICTURE;
            // (The simple and main profiles have no entry point to wait for.)
            if (x.wait == Wait::Entry && x.simple) x.wait = Wait::Nothing;
            if (x.wait != Wait::Nothing) {
                used = passed_over(p, size, at + 4);
                return score::ST_NO_PICTURE;
            }
            if (x.width > x.max_w || x.height > x.max_h) {
                PS_LOG_WARN(kChan, "instance {:#x}: a picture of {}x{}, past the instance's {}x{}: the program stops",
                    x.handle, x.width, x.height, x.max_w, x.max_h);
                used = at;
                return ST_TOO_LARGE;
            }
            type = x.simple ? parse_simple_picture(x, d) : parse_picture(x, d);
            picture_at = at;
            x.records = 1;
            const auto for_ffmpeg = [&](u8 bdu, u32 from, u32 to, const std::vector<u8>& payload) {
                std::vector<BitEdit> edits = ffmpeg_edits(x);
                if (edits.empty()) frame_for_ffmpeg.insert(frame_for_ffmpeg.end(), p + from, p + to);
                else {
                    const std::vector<u8> e = edited_bdu(bdu, payload, edits);
                    frame_for_ffmpeg.insert(frame_for_ffmpeg.end(), e.begin(), e.end());
                }
            };
            for_ffmpeg(0x0D, at, next, d);
            // The bytes used: from the one holding the frame's last bit (before its stop bit) to the next start
            // code, an end of sequence taken (the anchor given out after this picture), or to where the search ends.
            u32 last = end > at + 4 ? end - 1 : at + 4;
            // A simple or main profile frame has no stop bit: its end its header's when no macroblock is coded, else
            // taken as within its last byte (the program's own count, from the end of its macroblocks, is one more
            // when they end on a byte's boundary: an eighth of frames, not reproduced, pspocket not reading the
            // macroblock layer).
            if (x.simple)
                last = x.header_end ? at + 4 + static_cast<u32>(x.header_end / 8) : (next > at + 4 ? next - 1 : at + 4);
            // A field picture's second field: the FIELD BDU after the frame's.
            if (x.fcm == 2 && type) {
                if (next < size && p[next + 3] == 0x0C) {
                    const u32 fnext = start_code(p, size, next + 4);
                    const std::vector<u8> fd = unescape(p + next + 4, fnext - next - 4);
                    u32 fend = fnext;
                    while (fend > next + 4 && p[fend - 1] == 0) fend--;
                    std::memcpy(x.first_record, x.rec.pic, kRecord);
                    parse_second_field(x, fd);
                    x.records = 2;
                    for_ffmpeg(0x0C, next, fnext, fd);
                    last = fend > next + 4 ? fend - 1 : next + 4;
                } else {
                    PS_LOG_WARN(kChan, "instance {:#x}: a field picture without its second field", x.handle);
                }
            }
            // An interlaced frame's or progressive frame's slices.
            u32 sc = start_code(p, size, last);
            if (x.fcm != 2)
                while (sc < size && p[sc + 3] == 0x0B) {
                    const u32 snext = start_code(p, size, sc + 4);
                    frame_for_ffmpeg.insert(frame_for_ffmpeg.end(), p + sc, p + snext);
                    u32 send = snext;
                    while (send > sc + 4 && p[send - 1] == 0) send--;
                    last = send > sc + 4 ? send - 1 : sc + 4;
                    sc = start_code(p, size, last);
                }
            // After the picture: an end of sequence taken (the anchor given out after this picture); a frame's or
            // field's user data last in the access unit, right after the picture, taken to the end; any other user
            // data there, the picture not decoded.
            bool after_end = false;
            used = sc;
            while (sc < size) {
                const u8 c = p[sc + 3];
                if (c == 0x0A) {
                    end_after = after_end = true;
                    x.flag |= EX_END;
                    last = sc + 4;
                    sc = start_code(p, size, last);
                    used = sc;
                    continue;
                }
                if (c == 0x1D || c == 0x1C) {
                    if (!after_end && start_code(p, size, sc + 4) >= size) {
                        x.flag |= user_flag(c);
                        used = size;
                    } else {
                        used = sc;
                        dropped = true;
                    }
                }
                break;
            }
            if (sc >= size && used >= size && !dropped && (x.flag & (EX_FRAME_USER | EX_FIELD_USER)) == 0)
                used = std::max(last, size >= 3 ? size - 3 : 0u);
            break;
        }
        at = next;
    }
    if (!type) {
        // (A warning of the access unit's own: what is predicted after it is flagged as after a picture's.)
        used = passed_over(p, size, from);
        x.warned = true;
        return end_alone ? ST_END_ALONE : ST_NO_FRAME;
    }
    if (dropped) {
        if (end_after) flush_anchor(x);
        return score::ST_NO_PICTURE;
    }
    // The decode mode's B and BI pictures: given out as decoded ones are, their frame buffers left as they were (a BI
    // picture passed over from its header on).
    const bool skipped = x.mode_word && (type == PT_B || type == PT_BI);
    if (skipped && type == PT_BI) used = passed_over(p, size, picture_at + 4);
    // Its entry and frame buffer: a skipped picture's the newest anchor's frame again (with none, a free one, left as
    // it was; after an interlaced frame or field picture, a free one, the samples it is predicted from copied there).
    const bool copied = type == PT_SKIPPED && x.newest >= 0 && x.entry[x.newest].fcm;
    int frame = -1;
    if (type == PT_SKIPPED && x.newest >= 0 && !copied) {
        frame = x.entry[x.newest].frame;
    } else {
        frame = free_frame(x);
    }
    if (frame < 0) return score::ST_BUSY;
    if (copied) {
        std::vector<u8> y, u, v;
        vdec::read_strips(base, frame_of(x, x.pred_bwd), plan(x.lines32), x.width, x.height, y, u, v);
        vdec::write_strips(base, frame_of(x, frame), plan(x.lines32), x.width, x.height, y.data(), u.data(), v.data());
    }
    x.flushed = false;
    u32 status = skipped ? ST_SKIPPED : 0;
    // The simple and main profiles' warnings: SYNCMARKER in the sequence; a range reduced picture, but in an instance
    // for the main profile's levels (3 to 5) or level 0 (range reduction is the main profile's tool); the record's
    // picture then not valid.
    const bool main_level = x.level == 0 || (x.level >= 3 && x.level <= 5);
    if (x.simple && x.rec.seq.syncmarker) status |= ST_SYNC_MARKERS;
    if (x.simple && x.rec.seq.rangered && x.rec.pic[0x1A] && !main_level) status |= ST_RANGE_REDUCED;
    if (status & (ST_SYNC_MARKERS | ST_RANGE_REDUCED)) set(x, 0x17, 0);
    // The frame buffers it is predicted from, and the program's references after it.
    const bool no_anchor = x.newest < 0 || x.no_anchor;
    if (type == PT_I || type == PT_P || type == PT_SKIPPED) x.no_anchor = false;
    const int ref_p = no_anchor ? x.pred_fwd : x.pred_bwd, ref_fwd = x.pred_fwd, ref_bwd = x.pred_bwd;
    if (type == PT_I || type == PT_P || type == PT_SKIPPED || (type == PT_B && no_anchor)) {
        x.pred_fwd = x.pred_bwd;
        x.pred_bwd = frame;
    }
    // A picture predicted from no anchor (a P picture's, a B picture's after it), or since an anchor had a warning.
    if (type == PT_I) {
        x.warned = status != 0; // (its own warning: what is predicted from it is flagged)
    } else if (type != PT_BI) {
        if (no_anchor) status |= ST_NO_REFERENCE;
        if (x.warned) status |= ST_REFERENCE_WARNED;
        if (type != PT_B && status) x.warned = true;
    }
    Picture& e = x.entry[k];
    e = {};
    e.used = true;
    e.frame = frame;
    e.status = status;
    e.fields = x.records == 2;
    e.fcm = type == PT_SKIPPED ? 0 : x.fcm;
    e.expand = x.simple && x.rec.seq.rangered && x.rec.pic[0x1A] && (type == PT_P || type == PT_B);
    const bool anchor = type == PT_I || type == PT_P || type == PT_SKIPPED;
    if (anchor) {
        if (x.newest >= 0) make_ready(x, x.newest);
        x.older = x.newest;
        x.newest = k;
    } else {
        make_ready(x, k);
    }
    write_entry(base, x, k, status);
    if (end_after) flush_anchor(x); // the end of the sequence
    // FFmpeg decodes the access unit's picture into the frame buffer (a skipped picture's is its anchor's again), the
    // access unit to the end of the frame's BDU.
    if (type != PT_SKIPPED && !skipped) {
        // FFmpeg's decoder takes a sequence header as its configuration only (it does not read one in its packets):
        // opened again at a new one, with the entry point in force (a simple or main profile's made for it: its
        // coding tools and size, opened again when they change).
        std::vector<u8> config = x.seq_bdu;
        const std::vector<u8> entry = x.simple ? simple_entry_for_ffmpeg(x) : x.entry_bdu;
        config.insert(config.end(), entry.begin(), entry.end());
        if (x.ff_open && (x.simple ? x.ff_config != config : x.ff_seq != x.seq_bdu)) {
            x.ff.close();
            x.ff_open = false;
        }
        const bool opening = !x.ff_open;
        if (opening) {
            x.ff_seq = x.seq_bdu;
            x.ff_config = config;
        }
        if (open_ffmpeg(x, config)) {
            std::deque<vdec::DecodedPicture> out;
            // The headers first, alone (a decoder just opened, its configuration's): FFmpeg's decoder (re)initialises
            // at a sequence header, and holds the first picture it decodes then unless it was already initialised (its
            // low delay is set then).
            const std::vector<u8>& prime_headers = headers_for_ffmpeg.empty() && opening ? config : headers_for_ffmpeg;
            if (!prime_headers.empty())
                x.ff.decode(prime_headers.data(), static_cast<u32>(prime_headers.size()), 0, out);
            std::vector<u8> packet = headers_for_ffmpeg;
            packet.insert(packet.end(), frame_for_ffmpeg.begin(), frame_for_ffmpeg.end());
            // FFmpeg's references made the program's, as its frame buffers hold them (after FlushPicture, a skipped
            // picture, an anchor with no picture before it, they are not FFmpeg's own): a P picture's, by FFmpeg
            // decoding it once first when it has none; a B picture's two, by its decoding the last anchor again.
            // (A field picture with a P field: predicted from the anchor before it as a P picture is.)
            const bool p_field = x.records == 2 && (x.field_types[0] == PT_P || x.field_types[1] == PT_P);
            if (type == PT_P || p_field) {
                if (!x.ff_bwd) prime(x, packet, out);
                match_reference(base, x, x.ff_bwd, ref_p);
            } else if (type == PT_B) {
                while ((!x.ff_fwd || !x.ff_bwd) && !x.ff_anchor_packet.empty()) prime(x, x.ff_anchor_packet, out);
                if (x.ff_fwd) match_reference(base, x, x.ff_fwd, ref_fwd);
                if (x.ff_bwd) match_reference(base, x, x.ff_bwd, ref_bwd);
            }
            const u64 au = x.next_au++;
            x.waiting[au] = k;
            x.ff.decode(packet.data(), static_cast<u32>(packet.size()), au, out);
            if (type == PT_I || type == PT_P) {
                x.ff_fwd = x.ff_bwd;
                x.ff_bwd = au;
                x.ff_anchor_packet = packet;
            }
            x.ff.forget_all_but(x.ff_fwd, x.ff_bwd);
            place(base, x, out);
        }
    }
    return status;
}

// ---- the calls

s32 svc1dGetMemorySize(PPUContext& ctx, u32 size, u32 level)
{
    if (!writable(ctx, size, 4)) return fail(score::LOW_ARG);
    u32 w = 0, h = 0;
    u8* base = guest_base(ctx);
    if (!level_size(level, w, h)) {
        ps_write32(base, size, 0);
        return fail(score::LOW_SEQ);
    }
    ps_write32(base, size, memory_for(level, w, h));
    return lv2::CELL_OK;
}

// The picture size type 5's parameters ask for (words 8 and 9: all ones the level's, else 1 to the level's), to 16.
bool size_of(u8* base, u32 param, u32 type, u32& w, u32& h)
{
    if (type != 5) return true;
    const s32 pw = static_cast<s32>(ps_read32(base, param + 0x20));
    const s32 ph = static_cast<s32>(ps_read32(base, param + 0x24));
    if (pw != -1) {
        if (pw < 1 || pw > static_cast<s32>(w)) return false;
        w = align(static_cast<u32>(pw), 16);
    }
    if (ph != -1) {
        if (ph < 1 || ph > static_cast<s32>(h)) return false;
        h = align(static_cast<u32>(ph), 16);
    }
    return true;
}

s32 svc1dGetMemorySize2(PPUContext& ctx, u32 size, u32 level, u32 param)
{
    if (!writable(ctx, size, 4) || !writable(ctx, param, 0x28)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 type = ps_read32(base, param);
    if (type < 3 || type > 5) return fail(score::LOW_SEQ);
    u32 w = 0, h = 0;
    if (!level_size(level, w, h)) {
        ps_write32(base, size, 0);
        return fail(score::LOW_SEQ);
    }
    if (!size_of(base, param, type, w, h)) return fail(score::LOW_SEQ);
    ps_write32(base, size, memory_for(level, w, h));
    return lv2::CELL_OK;
}

s32 svc1dCreateInstance2(PPUContext& ctx, u32 mem, u32 size, u32 level, u32 param)
{
    if (!mem || !writable(ctx, param, 0x28)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 type = ps_read32(base, param);
    u32 w = 0, h = 0;
    if (type < 3 || type > 5 || !level_size(level, w, h) || !size_of(base, param, type, w, h))
        return fail(score::LOW_SEQ);
    const u32 need = memory_for(level, w, h) - 0x480;
    if (size < need) return fail(score::LOW_SEQ);
    if (type == 3) {
        PS_LOG_ERROR(kChan, "CreateInstance2 type 3 (the core's own SPURS) is not reproduced yet");
        return fail(score::LOW_SEQ);
    }
    const u32 taskset = ps_read32(base, param + 0x1C);
    if (!taskset) return fail(score::LOW_SEQ);
    if (!guest_readable(thread_of(ctx), mem, memory_for(level, w, h))) {
        PS_LOG_ERROR(kChan, "CreateInstance2: memory {:#x} is not guest memory", mem);
        return fail(score::LOW_ARG);
    }
    auto x = std::make_shared<Svc1d>();
    x->handle = mem;
    x->level = level;
    x->max_w = w;
    x->max_h = h;
    x->frames = frames_of(level);
    x->at = places(mem, w, h);
    const score::QueuePlaces q{x->at.queue_to_ppu - mem, x->at.queue_to_ppu_buffer - mem, x->at.queue_to_spu - mem,
        x->at.queue_to_spu_buffer - mem, 0x44};
    if (const s32 r = score::make_queues(ctx, mem, taskset, q); r != lv2::CELL_OK) return r;
    reset_queue(*x);
    // The PPU side's state: the auxiliary data for the next access unit (and the second field's) none.
    std::memset(base + mem + kAuxNow, 0, 0x21);
    svc1ds().add(mem, std::move(x));
    return lv2::CELL_OK;
}

s32 svc1dReleaseInstance(PPUContext& ctx, u32 handle)
{
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = svc1ds().find(handle);
    if (!x || !svc1ds().remove(handle)) {
        PS_LOG_ERROR(kChan, "ReleaseInstance of {:#x}, which is no instance", handle);
        return fail(score::LOW_ARG);
    }
    std::lock_guard lk(x->m);
    const score::QueuePlaces q{x->at.queue_to_ppu - handle, x->at.queue_to_ppu_buffer - handle,
        x->at.queue_to_spu - handle, x->at.queue_to_spu_buffer - handle, 0x44};
    score::drop_queues(ctx, handle, q);
    return lv2::CELL_OK;
}

s32 svc1dInitialize(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "Initialize");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    reset_queue(*x);
    x->first_entry = true;
    x->rec.pic[0x01] = 0; // (the record's first-entry flag: no entry point since)
    x->wait = Wait::Sequence;
    x->warned = false;
    x->no_anchor = false;
    x->rnd = 0;
    x->pred_fwd = x->pred_bwd = 0;
    // FFmpeg's decoder anew (its flush leaves state of its own, such as intensity compensation's tables).
    x->ff.close();
    x->ff_open = false;
    x->ff_fwd = x->ff_bwd = 0;
    x->ff_anchor_packet.clear();
    x->waiting.clear();
    return lv2::CELL_OK;
}

s32 svc1dSetAuxData(PPUContext& ctx, u32 handle, u32 aux)
{
    if (!handle || !writable(ctx, aux, 16)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "SetAuxData");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    u8* base = guest_base(ctx);
    std::memmove(base + handle + kAuxNow, base + aux, 16);
    ps_write8(base, handle + 0x4C, 1);
    return lv2::CELL_OK;
}

// DecodePicture2 and DecodePicture: the access unit {address, size, address, size}.
s32 decode(PPUContext& ctx, u32 handle, u32 a1, u32 s1, u32 a2, u32 s2, u32 used_out, std::string_view call)
{
    const std::shared_ptr<Svc1d> x = instance(handle, call);
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    u8* base = guest_base(ctx);
    std::vector<u8> au(base + a1, base + a1 + s1);
    if (s2) {
        // The program reads the first piece on to its next 16-byte boundary, then the second.
        const u32 gap = (a2 - a1 - s1) & 15;
        au.insert(au.end(), base + a1 + s1, base + a1 + s1 + gap);
        au.insert(au.end(), base + a2, base + a2 + s2);
    }
    u32 used = 0;
    const u32 st = spu_decode(base, *x, au.data(), static_cast<u32>(au.size()), used);
    if (st & score::ST_DEAD) x->dead = true;
    if (st & score::ST_BUSY) {
        ps_write32(base, used_out, 0);
        return score::answer(kPrefix, st);
    }
    // The program's reply: the bytes used, the existence flag.
    ps_write32(base, x->at.reply + 0, used);
    ps_write32(base, x->at.reply + 8, x->flag);
    ps_write32(base, used_out, used);
    // The auxiliary data went with the access unit.
    std::memset(base + handle + kAuxNow, 0, 16);
    ps_write8(base, handle + 0x4C, 0);
    return score::answer(kPrefix, st);
}

s32 svc1dDecodePicture2(PPUContext& ctx, u32 handle, u32 au, u32 used)
{
    if (!handle || !writable(ctx, au, 16) || !writable(ctx, used, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 a1 = ps_read32(base, au), s1 = ps_read32(base, au + 4), a2 = ps_read32(base, au + 8),
              s2 = ps_read32(base, au + 12);
    if (s2 && (!a2 || (a2 & 0x7F) || (s1 & 15))) return fail(score::LOW_ARG);
    return decode(ctx, handle, a1, s1, a2, s2, used, "DecodePicture2");
}

s32 svc1dDecodePicture(PPUContext& ctx, u32 handle, u32 au, u32 used)
{
    if (!handle || !writable(ctx, au, 8) || !writable(ctx, used, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    return decode(ctx, handle, ps_read32(base, au), ps_read32(base, au + 4), 0, 0, used, "DecodePicture");
}

s32 svc1dFlushPicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "FlushPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    flush_anchor(*x);
    return lv2::CELL_OK;
}

s32 svc1dDiscontinue(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "Discontinue");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    // The pictures stay; the stream goes on at its next entry point (the simple and main profiles have none: at its
    // next picture, with no anchor to predict from).
    if (x->simple) {
        x->no_anchor = true;
    } else {
        x->wait = Wait::Entry;
        x->first_entry = true;
    }
    return lv2::CELL_OK;
}

s32 svc1dCancelDecode(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "CancelDecode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    // The pictures dropped; the stream goes on at its next entry point.
    reset_queue(*x);
    x->wait = Wait::Entry;
    x->first_entry = true;
    x->warned = false;
    x->no_anchor = false;
    return lv2::CELL_OK;
}

s32 svc1dSetDecodeMode(PPUContext& ctx, u32 handle, u32 mode)
{
    if (!handle || !writable(ctx, mode, 8)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "SetDecodeMode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    x->mode_byte = ps_read8(guest_base(ctx), mode);
    x->mode_word = ps_read32(guest_base(ctx), mode + 4);
    return lv2::CELL_OK;
}

s32 svc1dSetPictureSize(PPUContext& ctx, u32 handle, u32 w, u32 h)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "SetPictureSize");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    if (w > x->max_w || h > x->max_h) {
        // The PPU side's own check: the instance stops as its program would.
        x->dead = true;
        return score::answer(kPrefix, ST_TOO_LARGE);
    }
    x->width = w;
    x->height = h;
    // The frames laid out for the profile the sequence record holds: the advanced profile's to 32 lines, the others'
    // (a new instance's record a simple profile's) to 16.
    x->lines32 = x->rec.seq.profile == 3;
    return lv2::CELL_OK;
}

// DecodeSeqHeader's command: the sequence header in the bytes given (the advanced profile's, or the simple and main
// profiles' STRUCT_C behind a sequence header's start code: libvdec's for those profiles, with SetPictureSize's size;
// the first BDU read so, whatever its start code: another's with the warnings 0xC) into the record, read to its end;
// with no start code, the default sequence (default_simple_sequence) and those warnings, every byte used.
u32 spu_sequence(u8* base, Svc1d& x, const u8* p, u32 size, u32& used)
{
    x.flag = EX_SEQUENCE;
    const u32 at = start_code(p, size, 0);
    if (at >= size) {
        default_simple_sequence(x);
        write_sequence(base, x);
        if (x.wait == Wait::Sequence) x.wait = Wait::Nothing;
        used = size;
        return ST_NO_FRAME | ST_SYNC_MARKERS;
    }
    const u32 next = start_code(p, size, at + 4);
    const std::vector<u8> d = unescape(p + at + 4, next - at - 4);
    u32 end = next;
    while (end > at + 4 && p[end - 1] == 0) end--;
    if (!d.empty() && (d[0] >> 6) != 3) {
        parse_struct_c(x, d);
        used = at + 8;
    } else {
        used = at + 4 + static_cast<u32>(parse_sequence(x, d) / 8);
    }
    x.seq_bdu.assign(p + at, p + end);
    write_sequence(base, x);
    if (x.wait == Wait::Sequence) x.wait = Wait::Nothing;
    // (Another BDU read as one: the same warnings.)
    return p[at + 3] == 0x0F ? 0 : ST_NO_FRAME | ST_SYNC_MARKERS;
}

s32 svc1dDecodeSeqHeader(PPUContext& ctx, u32 handle, u32 au, u32 used_out)
{
    if (!handle || !writable(ctx, au, 8) || !writable(ctx, used_out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "DecodeSeqHeader");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    u8* base = guest_base(ctx);
    const u32 addr = ps_read32(base, au), size = ps_read32(base, au + 4);
    if (size && !guest_readable(thread_of(ctx), addr, size)) {
        PS_LOG_ERROR(kChan, "DecodeSeqHeader: {:#x} bytes at {:#x} are not guest memory", size, addr);
        return fail(score::LOW_ARG);
    }
    const std::vector<u8> bytes(base + addr, base + addr + size);
    u32 used = 0;
    const u32 st = spu_sequence(base, *x, bytes.data(), size, used);
    // The program's reply, as DecodePicture's.
    ps_write32(base, x->at.reply + 0, used);
    ps_write32(base, x->at.reply + 8, x->flag);
    ps_write32(base, used_out, used);
    return score::answer(kPrefix, st);
}

s32 svc1dGetNumPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "GetNumPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    // The pictures given out to be taken and not released yet (taken or not).
    u32 n = 0;
    for (const Picture& p : x->entry) n += p.used && p.ready ? 1 : 0;
    ps_write32(guest_base(ctx), out, n);
    return x->flushed ? score::answer(kPrefix, score::ST_FLUSHED) : lv2::CELL_OK;
}

s32 svc1dGetPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x24)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "GetPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    int k = -1;
    for (int i = 0; i < static_cast<int>(kEntries); i++)
        if (x->entry[i].used && x->entry[i].ready && x->entry[i].seq == x->get) k = i;
    x->current = k;
    if (k < 0) return fail(score::LOW_EMPTY);
    x->get = (x->get + 1) & 0x7F;
    u8* base = guest_base(ctx);
    const u32 d = x->at.desc[k], info = x->at.info[k];
    const u32 order[9] = {0, 1, 2, 7, 3, 4, 5, 6};
    for (u32 i = 0; i < 8; i++) ps_write32(base, out + 4 * i, ps_read32(base, d + 4 * order[i]));
    ps_write32(base, out + 0x20, 0);
    std::memmove(base + handle + kGetCopy, base + info, 0x220);
    const u32 fields = ps_read8(base, info + 0x203);
    u32 st = 0;
    for (u32 f = 0; f < fields; f++) {
        const u32 s = ps_read32(base, info + 0x204 + 4 * f);
        st |= s | ((s & 0xE) ? 0x300u : 0u);
    }
    return score::picture_answer(kPrefix, st);
}

s32 svc1dReleasePicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "ReleasePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    for (Picture& p : x->entry)
        if (p.used && p.ready && p.seq == x->release) {
            p.used = false;
            p.ready = false;
            x->release = (x->release + 1) & 0x7F;
            return lv2::CELL_OK;
        }
    return fail(score::LOW_EMPTY);
}

s32 svc1dGetSideInfoPic(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    if (!handle || !writable(ctx, count, 4) || !writable(ctx, first, kRecord) || !writable(ctx, second, kRecord))
        return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 n = ps_read8(base, handle + kGetCopy + 0x203);
    for (u32 f = 0; f < n && f < 2; f++)
        std::memmove(base + (f == 0 ? first : second), base + handle + kGetCopy + f * kRecord, kRecord);
    ps_write32(base, count, n);
    return lv2::CELL_OK;
}

s32 svc1dGetAuxData(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    if (!handle || !writable(ctx, count, 4) || !writable(ctx, first, 16) || !writable(ctx, second, 16))
        return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    std::memmove(base + first, base + handle + kGetCopy + 0x20C, 16);
    std::memmove(base + second, base + handle + 0x3C, 16);
    ps_write32(base, count, ps_read8(base, handle + kGetCopy + 0x21C) ? 1 : 0);
    return lv2::CELL_OK;
}

s32 svc1dGetSideInfoSeq(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x38)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "GetSideInfoSeq");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    u8* base = guest_base(ctx);
    std::memmove(base + out, base + x->at.seq, 0x38);
    return lv2::CELL_OK;
}

s32 svc1dGetExistenceFlag(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Svc1d> x = instance(handle, "GetExistenceFlag");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    u8* base = guest_base(ctx);
    ps_write32(base, out, ps_read32(base, x->at.reply + 8));
    return lv2::CELL_OK;
}

} // namespace

void register_svc1d(Registry& r)
{
    r.add("libsvc1d", {
        PS_HLE_FUNC(0xF106AAD3, svc1dGetMemorySize),
        PS_HLE_FUNC(0x593E2315, svc1dGetMemorySize2),
        PS_HLE_FUNC(0x5E8AAE6A, svc1dCreateInstance2),
        PS_HLE_FUNC(0xD9CD56BA, svc1dReleaseInstance),
        PS_HLE_FUNC(0x3607BBF8, svc1dInitialize),
        PS_HLE_FUNC(0x51C73000, svc1dDecodePicture),
        PS_HLE_FUNC(0x4F4EEE83, svc1dDecodePicture2),
        PS_HLE_FUNC(0x9867CDC2, svc1dDecodeSeqHeader),
        PS_HLE_FUNC(0xA8E94A19, svc1dSetPictureSize),
        PS_HLE_FUNC(0x19610523, svc1dFlushPicture),
        PS_HLE_FUNC(0x0306A00D, svc1dDiscontinue),
        PS_HLE_FUNC(0x3666354F, svc1dCancelDecode),
        PS_HLE_FUNC(0xFE0E34B8, svc1dSetDecodeMode),
        PS_HLE_FUNC(0xD8465650, svc1dGetPicture),
        PS_HLE_FUNC(0x149555D0, svc1dGetNumPicture),
        PS_HLE_FUNC(0x95665052, svc1dReleasePicture),
        PS_HLE_FUNC(0x7441419B, svc1dGetSideInfoPic),
        PS_HLE_FUNC(0xA1468D7B, svc1dGetAuxData),
        PS_HLE_FUNC(0xEBBFEAC3, svc1dGetSideInfoSeq),
        PS_HLE_FUNC(0x76D867F4, svc1dGetExistenceFlag),
        PS_HLE_FUNC(0x383CBE2F, svc1dSetAuxData),
    });
}

} // namespace pspocket::hle::vdec_core
