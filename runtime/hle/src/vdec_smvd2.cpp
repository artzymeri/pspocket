// libsmvd2, the MPEG-2 core (MPEG-1 and MPEG-2 video, main profile), pspocket's own (D-920 part 6): answering as
// Sony's module does (firmware 4.92), compared call for call in tests/hle/test_vdec_score_compare.cpp. The PPU
// framework it shares with the other s-cores is vdec_score.hpp's; this file is what libsmvd2's SPU programs do with
// the stream, done on the host: the headers parsed and stored as Sony's program stores them, the pictures decoded by
// pspocket's own decoder of Sony's slice program (vdec_mpeg2.hpp: its inverse DCT, its BNR data, whether a picture is
// damaged, which references it predicts from) into Sony's frame buffers, laid out as Sony's are, and given out in
// display order.
//
// The instance's memory (128-byte aligned; GetMemorySize's size: the frames' part, the SPU tasks' queues and context,
// 0x4600 bytes of the core's own; CreateInstance2 clears its first 0x3C00 bytes) as a caller sees it:
// - +0x80: the sequence information (GetSideInfoSeq copies 0x20 bytes but one): whether a sequence header is in force,
//   the header's fields, whether a sequence extension came (an MPEG-2 stream) and its fields (an MPEG-1 stream's
//   defaults otherwise), the sequence display extension's.
// - +0x150: the existence flag: which headers the last access unit carried (bits below); +0x154: the bytes of the
//   access unit the last DecodePicture used (to its first picture's end); +0x158: the caller's auxiliary data.
// - +0x400: each frame buffer slot's information block (0x280 bytes): the picture's addresses, size and pitches, its
//   access units (one, or two for a frame of two field pictures) with each one's side information (0xBC bytes),
//   auxiliary data, BNR data and status.
// - The four frame buffers after the SPU tasks' context, each sized for the instance's largest picture, macroblock by
//   macroblock: 16 lines of 16 luma samples, then 8 lines of 8 Cb and 8 Cr samples.
//
// The side information is the program's record of the headers: the last GOP's fields (whether one came, a parity
// that turns at each GOP, its time code and flags), the picture header's (and, set by every picture header for an
// MPEG-1 stream, the coding extension's fields as MPEG-1 has them, which an MPEG-2 picture's coding extension then
// overwrites), whether the picture had a coding extension and a display extension, the display extension's offsets,
// closed caption data. A field a picture's headers leave out keeps its value; the program keeps the record in its SPU's
// local store, which SPURS does not clear between tasks (unless the taskset asks it to), so an instance starts with
// what the last one on the same SPURS instance left (here the record is the SPURS instance's, copied into each new
// instance: LocalStore). Initialize, Discontinue, CancelDecode, and a GOP with a broken link that is not closed, clear
// the GOP's flag and parity and the per-picture flags; FlushPicture clears the picture type.
//
// What the program does with an access unit (the status bits, vdec_score.hpp's): its headers parsed (a sequence header
// before any picture is needed: until one comes, no picture, ST_NO_PICTURE; a header whose flags call for more fields
// than it has reads them from what follows, start codes too), its first picture decoded. A frame picture, or a field
// picture that is not the other field of the last one (same temporal reference, other parity), takes the first free
// slot (the two last anchor pictures' slots are not free); an I or P frame waits for the next anchor frame (or
// FlushPicture) to be given out, a B frame is given out once both its fields are decoded, or not decoded at all in the
// decode mode that skips them (status 0x80: GetPicture's 0x..28). A picture predicted from a reference that is gone
// (after Initialize, Discontinue, CancelDecode, FlushPicture, a broken link, or one that was itself predicted from
// nothing or skipped) is flagged 0x800, one predicted from a reference that had any warning 0x400; a damaged picture
// (slices the program cannot decode, missing or out of order, an MPEG-2 picture without a coding extension) 0x4. A
// picture larger than the instance's frame buffers kills the decoder (0x81000000, then 0x80000000).
//
// Nothing here is Sony's code or data (CLAUDE.md rule 1): the behaviour is written from what the module does.

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "spurs.hpp"
#include "vdec_core_common.hpp"
#include "vdec_mpeg2.hpp"
#include "vdec_score.hpp"

namespace pspocket::hle::vdec_core {
namespace {

using score::PictureQueue;

constexpr std::string_view kChan = "smvd2";
constexpr u32 kPrefix = 0x80615000;
s32 fail(u32 low) { return score::code(kPrefix, low); }

// The SPU program's status bits of its own.
enum : u32 {
    ST_DAMAGED = 0x4,          // slices the program cannot decode; an MPEG-2 picture without a coding extension
    ST_MARKER = 0x10,          // a marker bit of a header that is not 1 (a warning: 0x..01)
    ST_SKIPPED = 0x80,         // a B picture the decode mode skips
    ST_REFERENCE_WARNED = 0x400, // predicted from a picture that had a warning
    ST_NO_REFERENCE = 0x800,   // predicted from a picture that is gone
    ST_TOO_LARGE = score::ST_DEAD | score::ST_NO_MEMORY_BIT,
};

// The existence flag's bits: the headers an access unit carried.
enum : u32 {
    EX_SEQUENCE = 0x1,
    EX_SEQUENCE_EXT = 0x2,
    EX_DISPLAY_EXT = 0x4,
    EX_SEQUENCE_USER = 0x10,
    EX_GOP = 0x100,
    EX_GOP_USER = 0x1000,
    EX_PICTURE = 0x10000,
    EX_PICTURE_EXT = 0x20000,
    EX_PICTURE_DISPLAY = 0x40000,
    EX_PICTURE_USER = 0x100000,
    EX_SEQUENCE_END = 0x1000000,
};

// Places in the instance's memory.
constexpr u32 kSeq = 0x80, kAuCopy = 0x100, kAu2Copy = 0x10C, kFlag = 0x150, kUsed = 0x154, kAux = 0x158,
              kMode = 0x168, kPictures = 0x400, kSlotBytes = 0x280, kCleared = 0x3C00;
// A slot's information block.
constexpr u32 kInfoCount = 0x1C, kInfoStructure = 0x1E, kInfoProgressive = 0x1F, kInfoSide = 0x20, kSideBytes = 0xBC,
              kInfoAux = 0x198, kInfoBnr = 0x1B8, kInfoStatus = 0x1F8;
constexpr u32 kSlots = 4;

u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// Each level's largest picture (0 to 4, and 16).
bool level_size(u32 level, u32& w, u32& h)
{
    switch (level) {
    case 0:
    case 4: w = 1920, h = 1152; return true;
    case 1: w = 352, h = 288; return true;
    case 2: w = 720, h = 576; return true;
    case 3: w = 1440, h = 1152; return true;
    case 16: w = 1280, h = 720; return true;
    default: return false;
    }
}

// The memory for pictures of w x h and `spus` SPU tasks: the frames' part (a line store and four frames, the width at
// least 48 and to 16, the height at least 64 and to 32), the context and queues of the tasks, the rest.
u32 picture_part(u32 w, u32 h)
{
    const u32 width = w > 47 ? align(w, 16) : 48, lines = w > 47 ? align(w, 16) * 24 : 0x480;
    const u32 rows = h > 47 ? align(h, 32) * 3 : 0xC0;
    return lines + rows * width * 2 + 0x200;
}
u32 tasks_part(u32 spus) { return 0x500 * spus + 0x500 + 0x2B000; }
u32 memory_for(u32 w, u32 h, u32 spus) { return picture_part(w, h) + 0x600 + tasks_part(spus) + 0x4000; }

// The SPU tasks' queues: the reply and command queues after each task's two (0x500 bytes a task), their buffers, then
// the tasks' context; the frames after that.
score::QueuePlaces queue_places(u32 spus)
{
    const u32 reply = kCleared + 0x500 * spus;
    return {reply, reply + 0x100, reply + 0x280, reply + 0x380, 2};
}
u32 frames_offset(u32 spus) { return kCleared + 0x500 * spus + 0x500 + 0x600 + 0x2B000 + 0x1100; }

// ---- the SPU program's state

// The headers as the program records them for a picture (side information), then its auxiliary data.
struct SideInfo {
    u8 b[kSideBytes] = {};
    u8 aux[16] = {};
};

// What the program keeps in its SPU's local store across tasks: the side information record. The local stores are
// those of the SPURS instance's SPUs, which start with nothing (zeros) when the instance is made (libvdec makes one
// for each decoder): the record is kept per SPURS instance, known by its native scheduler (each cellSpursInitialize
// makes one anew; spurs.hpp), or process-wide for a taskset of no instance pspocket runs.
struct LocalStore {
    std::mutex m;
    struct Record {
        std::weak_ptr<void> spurs;
        SideInfo side;
    };
    std::vector<Record> records;
    SideInfo other;

    SideInfo& of(const std::weak_ptr<void>& spurs)
    {
        if (spurs.owner_before(std::weak_ptr<void>{}) || std::weak_ptr<void>{}.owner_before(spurs)) {
            std::erase_if(records, [](const Record& r) { return r.spurs.expired(); });
            for (Record& r : records)
                if (!r.spurs.owner_before(spurs) && !spurs.owner_before(r.spurs)) return r.side;
            return records.emplace_back(Record{spurs, {}}).side;
        }
        return other;
    }
};
LocalStore& local_store()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "libsmvd2 called without an HLE process");
    return p->state<LocalStore>();
}

// The SPURS instance a taskset belongs to (its native scheduler), or none.
std::weak_ptr<void> spurs_of(u8* base, u32 taskset)
{
    const u32 spurs = static_cast<u32>(ps_read64(base, taskset + spurs::ts_off::spurs));
    spurs::SpursHost& h = spurs::host();
    std::lock_guard lk(h.mutex);
    const auto it = h.instances.find(spurs);
    if (it == h.instances.end() || !it->second.scheduler) return {};
    return std::shared_ptr<void>(it->second.scheduler);
}


struct Smvd2 {
    std::mutex m;
    u32 handle = 0, spus = 1, max_w = 0, max_h = 0;
    bool dead = false;
    bool sequence = false; // a sequence header since Initialize
    u32 width = 0, height = 0;
    bool mpeg2 = false;    // a sequence or picture coding extension since the last sequence header
    bool progressive_sequence = true;
    u8 intra_w[64] = {}, non_intra_w[64] = {}; // the quantiser matrices (natural order)
    SideInfo side;
    // The field pairing: the last picture's temporal reference (-1: none) and structure.
    int last_tr = -1, last_structure = 0;
    // The frame being decoded: its slot, the access unit (0, or 1 for its second field), its references.
    int current = -1;
    u32 au_index = 0;
    // The frame's references (slots): a P frame predicts from the newest anchor frame, a B frame from the one before
    // it and the newest.
    int ref_forward = 0, ref_backward = 0;
    u32 fields[kSlots] = {}; // a slot's decoded fields (1 top, 2 bottom): 0 is no reference
    u32 warned[kSlots] = {}; // a slot's statuses: a reference with a warning
    PictureQueue queue;
    int pending = -1;      // the anchor frame decoded and not given out yet
    bool skip_b = false;   // the decode mode
    u32 warnings = 0;      // the access unit's header warnings (ST_MARKER)
    u32 bnr[8] = {};       // the program's BNR data buffer: the last decoded picture's (a picture not decoded keeps it)
    std::weak_ptr<void> spurs; // the SPURS instance whose local stores keep the record (LocalStore)
};

using Smvd2s = score::Instances<Smvd2>;
Smvd2s& smvd2s() { return Smvd2s::of_process(kChan); }

struct Bits {
    const u8* p;
    u32 size;
    u64 pos;
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos++) v = (v << 1) | (pos / 8 < size ? (p[pos / 8] >> (7 - pos % 8)) & 1 : 0);
        return v;
    }
};

u32 start_code(const u8* p, u32 size, u32 from)
{
    for (u32 i = from; i + 3 < size; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return size;
}

u32 frame_bytes(const Smvd2& x) { return align(x.max_w, 16) * align(x.max_h, 16) * 3 / 2 + 0x80; }
u32 frame_of(const Smvd2& x, int slot)
{
    return x.handle + frames_offset(x.spus) + static_cast<u32>(slot) * frame_bytes(x);
}
u32 info_of(const Smvd2& x, int slot) { return x.handle + kPictures + static_cast<u32>(slot) * kSlotBytes; }

void put16(SideInfo& si, u32 at, u32 v)
{
    si.b[at] = static_cast<u8>(v >> 8);
    si.b[at + 1] = static_cast<u8>(v);
}

// The record's per-picture flags cleared, and the GOP's flag and parity (Initialize, Discontinue, CancelDecode, a
// broken link): the references are gone too.
void discontinuity(Smvd2& x)
{
    SideInfo& si = x.side;
    si.b[0x00] = si.b[0x01] = 0;
    si.b[0x0A] = si.b[0x16] = si.b[0x2C] = si.b[0x3A] = si.b[0x3B] = 0;
    for (u32 k = 0; k < kSlots; k++) x.fields[k] = x.warned[k] = 0;
    x.last_tr = -1;
}

// The quantiser matrices a sequence header (which restores the default ones it does not load) or a quant matrix
// extension loads, in the bitstream's zigzag order.
void load_matrices(Smvd2& x, Bits& b, bool defaults)
{
    if (b.bits(1)) {
        for (u32 k = 0; k < 64; k++) x.intra_w[mpeg2dec::kZigzag[k]] = static_cast<u8>(b.bits(8));
    } else if (defaults) {
        std::memcpy(x.intra_w, mpeg2dec::kDefaultIntra, 64);
    }
    if (b.bits(1)) {
        for (u32 k = 0; k < 64; k++) x.non_intra_w[mpeg2dec::kZigzag[k]] = static_cast<u8>(b.bits(8));
    } else if (defaults) {
        std::memset(x.non_intra_w, 16, 64);
    }
}

// The sequence header and its extensions into the sequence information.
void parse_sequence(u8* base, Smvd2& x, Bits& b)
{
    const u32 s = x.handle + kSeq;
    x.width = b.bits(12);
    x.height = b.bits(12);
    const u32 aspect = b.bits(4), rate = b.bits(4), bit_rate = b.bits(18);
    if (!b.bits(1)) x.warnings |= ST_MARKER;
    const u32 vbv = b.bits(10), constrained = b.bits(1);
    load_matrices(x, b, true);
    ps_write8(base, s + 0x00, 1);
    ps_write16(base, s + 0x02, static_cast<u16>(x.width));
    ps_write16(base, s + 0x04, static_cast<u16>(x.height));
    ps_write8(base, s + 0x06, static_cast<u8>(aspect));
    ps_write8(base, s + 0x07, static_cast<u8>(rate));
    ps_write32(base, s + 0x08, bit_rate);
    ps_write16(base, s + 0x0C, static_cast<u16>(vbv));
    ps_write8(base, s + 0x0E, static_cast<u8>(constrained));
    // An MPEG-1 stream's (no sequence extension): no profile, progressive, 4:2:0; no display extension.
    const u8 defaults[] = {0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (u32 k = 0; k < sizeof(defaults); k++) ps_write8(base, s + 0x0F + k, defaults[k]);
    x.mpeg2 = false;
    x.progressive_sequence = true;
}

void parse_sequence_extension(u8* base, Smvd2& x, Bits& b)
{
    const u32 s = x.handle + kSeq;
    const u32 profile = b.bits(8), progressive = b.bits(1), chroma = b.bits(2), hext = b.bits(2), vext = b.bits(2);
    const u32 rate_ext = b.bits(12);
    if (!b.bits(1)) x.warnings |= ST_MARKER;
    const u32 vbv_ext = b.bits(8), low_delay = b.bits(1), rate_n = b.bits(2), rate_d = b.bits(5);
    x.width |= hext << 12;
    x.height |= vext << 12;
    ps_write16(base, s + 0x02, static_cast<u16>(x.width));
    ps_write16(base, s + 0x04, static_cast<u16>(x.height));
    ps_write32(base, s + 0x08, ps_read32(base, s + 0x08) | rate_ext << 18);
    ps_write16(base, s + 0x0C, static_cast<u16>(ps_read16(base, s + 0x0C) | vbv_ext << 10));
    ps_write8(base, s + 0x0F, 1);
    ps_write8(base, s + 0x10, static_cast<u8>(profile));
    ps_write8(base, s + 0x11, static_cast<u8>(progressive));
    ps_write8(base, s + 0x12, static_cast<u8>(chroma));
    ps_write8(base, s + 0x13, static_cast<u8>(low_delay));
    ps_write8(base, s + 0x14, static_cast<u8>(rate_n));
    ps_write8(base, s + 0x15, static_cast<u8>(rate_d));
    x.mpeg2 = true;
    x.progressive_sequence = progressive != 0;
}

void parse_display_extension(u8* base, Smvd2& x, Bits& b)
{
    const u32 s = x.handle + kSeq;
    ps_write8(base, s + 0x16, 1);
    ps_write8(base, s + 0x17, static_cast<u8>(b.bits(3)));
    const u32 colour = b.bits(1);
    ps_write8(base, s + 0x18, static_cast<u8>(colour));
    if (colour)
        for (u32 k = 0; k < 3; k++) ps_write8(base, s + 0x19 + k, static_cast<u8>(b.bits(8)));
    ps_write16(base, s + 0x1C, static_cast<u16>(b.bits(14)));
    if (!b.bits(1)) x.warnings |= ST_MARKER;
    ps_write16(base, s + 0x1E, static_cast<u16>(b.bits(14)));
}

// The quant matrix extension: the matrices it loads (4:2:0's chroma ones are not used).
void parse_quant_matrix(Smvd2& x, Bits& b) { load_matrices(x, b, false); }

// The GOP header. A broken link in a GOP that is not closed is a discontinuity.
void parse_gop(Smvd2& x, Bits& b)
{
    SideInfo& si = x.side;
    const u32 time_code = b.bits(25);
    si.b[0x00] = 1;
    si.b[0x01] ^= 1;
    si.b[0x04] = static_cast<u8>(time_code >> 24);
    si.b[0x05] = static_cast<u8>(time_code >> 16);
    si.b[0x06] = static_cast<u8>(time_code >> 8);
    si.b[0x07] = static_cast<u8>(time_code);
    si.b[0x08] = static_cast<u8>(b.bits(1));
    si.b[0x09] = static_cast<u8>(b.bits(1));
    if (!si.b[0x08] && si.b[0x09]) discontinuity(x);
}

// The picture header; its coding type. It records the coding extension's fields as an MPEG-1 picture has them.
u32 parse_picture(Smvd2& x, Bits& b)
{
    SideInfo& si = x.side;
    si.b[0x0A] = 1;
    si.b[0x16] = 0;
    si.b[0x2C] = 0;
    put16(si, 0x0C, b.bits(10));
    const u32 type = b.bits(3);
    si.b[0x0E] = static_cast<u8>(type);
    put16(si, 0x10, b.bits(16));
    if (type == 2 || type == 3) {
        si.b[0x12] = static_cast<u8>(b.bits(1));
        si.b[0x13] = static_cast<u8>(b.bits(3));
    }
    if (type == 3) {
        si.b[0x14] = static_cast<u8>(b.bits(1));
        si.b[0x15] = static_cast<u8>(b.bits(3));
    }
    const u8 mpeg1[] = {0, 3, 0, 1, 0, 0, 0, 0, 0, 1, 1}; // 0x1B to 0x25
    for (u32 k = 0; k < sizeof(mpeg1); k++) si.b[0x1B + k] = mpeg1[k];
    return type;
}

void parse_picture_coding(Smvd2& x, Bits& b)
{
    SideInfo& si = x.side;
    si.b[0x16] = 1;
    for (u32 k = 0; k < 4; k++) si.b[0x17 + k] = static_cast<u8>(b.bits(4));
    si.b[0x1B] = static_cast<u8>(b.bits(2));
    si.b[0x1C] = static_cast<u8>(b.bits(2));
    for (u32 k = 0x1D; k <= 0x25; k++) si.b[k] = static_cast<u8>(b.bits(1));
    si.b[0x26] = static_cast<u8>(b.bits(1));
    if (si.b[0x26]) {
        si.b[0x27] = static_cast<u8>(b.bits(1));
        si.b[0x28] = static_cast<u8>(b.bits(3));
        si.b[0x29] = static_cast<u8>(b.bits(1));
        si.b[0x2A] = static_cast<u8>(b.bits(7));
        si.b[0x2B] = static_cast<u8>(b.bits(8));
    }
    x.mpeg2 = true;
}

void parse_picture_display(Smvd2& x, Bits& b)
{
    SideInfo& si = x.side;
    // The frame centre offsets: one for field pictures and progressive frames shown once, else two or three.
    u32 n = 1;
    if (!x.progressive_sequence) {
        if (si.b[0x1C] == 3) n = si.b[0x23] ? 3 : 2;
    } else if (si.b[0x23]) {
        n = si.b[0x1D] ? 3 : 2;
    }
    si.b[0x2C] = 1;
    si.b[0x2D] = static_cast<u8>(n);
    for (u32 k = 0; k < n; k++) {
        put16(si, 0x2E + 2 * k, b.bits(16));
        if (!b.bits(1)) x.warnings |= ST_MARKER;
        put16(si, 0x34 + 2 * k, b.bits(16));
        if (!b.bits(1)) x.warnings |= ST_MARKER;
    }
}

// User data: closed captions into the record (0x3A: how many bytes, 0x3B: where the user data was, 0 the sequence's,
// 1 the GOP's, 2 the picture's; from 0x3C: the bytes). ATSC's ("GA94", type 3): cc_data()'s flags and em_data bytes,
// its cc_count triples, the marker byte and the additional data to the next start code (to 128 bytes). DVD's ("CC"):
// the user data as it came, start code first, 9 + 3 x (the fifth byte & 63) bytes (none when more than 128 or when a
// start code comes first).
void parse_user_data(Smvd2& x, const u8* p, u32 size, u32 at, u8 where)
{
    SideInfo& si = x.side;
    Bits b{p, size, u64{at + 4} * 8};
    const auto peek = [&](u32 n) {
        Bits c = b;
        return c.bits(n);
    };
    const auto start_next = [&] { return peek(24) == 1; };
    if (peek(32) == 0x47413934) { // "GA94"
        b.bits(32);
        if (b.bits(8) == 3) {
            si.b[0x3B] = where;
            const u32 flags = b.bits(8);
            si.b[0x3C] = static_cast<u8>(flags);
            si.b[0x3D] = static_cast<u8>(b.bits(8));
            u32 count = 2;
            si.b[0x3A] = 2;
            for (u32 i = 0; i < (flags & 31) && i < 42; i++)
                for (u32 k = 0; k < 3; k++) {
                    si.b[0x3C + count] = static_cast<u8>(b.bits(8));
                    si.b[0x3A] = static_cast<u8>(++count);
                }
            if (0x3C + count < kSideBytes) si.b[0x3C + count] = static_cast<u8>(b.bits(8));
            si.b[0x3A] = static_cast<u8>(++count);
            if (flags & 0x20)
                while (!start_next() && count < 128) {
                    si.b[0x3C + count] = static_cast<u8>(b.bits(8));
                    si.b[0x3A] = static_cast<u8>(++count);
                }
        }
    }
    if (peek(16) == 0x4343) { // "CC"
        si.b[0x3B] = where;
        const u8 head[] = {0, 0, 1, 0xB2, 0x43, 0x43};
        std::memcpy(si.b + 0x3C, head, sizeof(head));
        b.bits(16);
        for (u32 k = 6; k < 9; k++) si.b[0x3C + k] = static_cast<u8>(b.bits(8));
        const u32 total = 3 * (si.b[0x3C + 8] & 63u) + 9;
        si.b[0x3A] = static_cast<u8>(total);
        if (total > 128) {
            si.b[0x3A] = 0;
            return;
        }
        for (u32 k = 9; k < total; k++) {
            if (start_next()) {
                si.b[0x3A] = 0;
                return;
            }
            si.b[0x3C + k] = static_cast<u8>(b.bits(8));
        }
    }
}

// The access unit's place in its slot's information block: its addresses, size and pitches; the access unit count and
// the fields so far; whether the sequence is progressive; its side information (the picture header present, the
// MPEG-2 flag the sequence's), auxiliary data, BNR data and status (with its fields, 0x100 top, 0x200 bottom, 0x300 a
// frame, when it has any bit but 0x10). A frame's first access unit clears what its second's would show.
void write_info(u8* base, const Smvd2& x, int slot, u32 index, u32 status, const u32 bnr[8])
{
    const u32 info = info_of(x, slot), frame = frame_of(x, slot);
    const u8 structure = x.side.b[0x1C];
    if (index == 0) {
        ps_write8(base, info + kInfoSide + kSideBytes + 0x00, 0);
        ps_write8(base, info + kInfoSide + kSideBytes + 0x0A, 0);
        ps_write8(base, info + kInfoStructure, 0);
        std::memset(base + info + kInfoAux + 0x10, 0, 0x10);
    }
    ps_write32(base, info + 0x00, frame);
    ps_write32(base, info + 0x04, frame + 0x100);
    ps_write32(base, info + 0x08, frame + 0x108);
    ps_write32(base, info + 0x0C, x.width);
    ps_write32(base, info + 0x10, x.height);
    ps_write32(base, info + 0x14, x.width);
    ps_write32(base, info + 0x18, x.width / 2);
    ps_write16(base, info + kInfoCount, static_cast<u16>(index + 1));
    ps_write8(base, info + kInfoStructure, static_cast<u8>(ps_read8(base, info + kInfoStructure) | structure));
    ps_write8(base, info + kInfoProgressive, x.progressive_sequence ? 1 : 0);
    u8* side = base + info + kInfoSide + index * kSideBytes;
    std::memcpy(side, x.side.b, kSideBytes);
    side[0x0A] = 1;
    side[0x16] = x.mpeg2 ? 1 : 0;
    std::memcpy(base + info + kInfoAux + index * 0x10, x.side.aux, 0x10);
    for (u32 k = 0; k < 8; k++) ps_write32(base, info + kInfoBnr + index * 0x20 + 4 * k, bnr[k]);
    u32 kept = status & ~0x10u;
    if (kept) kept |= structure == 1 ? 0x100u : structure == 2 ? 0x200u : 0x300u;
    ps_write32(base, info + kInfoStatus + index * 4, kept);
}

// The anchor waiting to be given out, given out.
void give_pending(Smvd2& x)
{
    if (x.pending < 0) return;
    x.queue.ready(x.pending);
    x.pending = -1;
}

// A reference's slot as the program takes it from the picture queue: none (after Initialize, CancelDecode or
// FlushPicture) is the queue's 0xFF, which it takes for the last slot (whose frame buffer is then read as it is).
int reference_slot(int slot) { return slot >= 0 ? slot : static_cast<int>(kSlots - 1); }

// FlushPicture's (and a sequence end code's): the last anchor given out, the references gone, the record's picture
// type cleared.
void flush(Smvd2& x)
{
    give_pending(x);
    const int newest = reference_slot(x.queue.forward);
    x.fields[newest] = x.warned[newest] = 0;
    x.queue.flush();
    x.side.b[0x0E] = 0;
}

// The decoded fields of the slot a reference is in; 0 when it is gone.
u32 valid(const Smvd2& x, int slot) { return slot >= 0 ? x.fields[slot] : 0; }

// ---- the SPU program's commands

u32 spu_decode(u8* base, Smvd2& x, u32 addr, u32 size, u32& used, u32& flag)
{
    const u8* p = base + addr;
    PictureQueue& q = x.queue;
    bool in_picture = false;
    u32 type = 0, first_slice = size;
    bool any = false;
    u32 last = 0; // the start code before the slices: which extension or user data belongs where
    // A new access unit: the caller's auxiliary data taken, the record's picture flags cleared.
    std::memcpy(x.side.aux, base + x.handle + kAux, 16);
    x.side.b[0x0A] = x.side.b[0x16] = x.side.b[0x2C] = 0;
    x.warnings = 0;
    bool end_after = false; // a sequence end code after the picture: taken with it, then a flush
    for (u32 at = start_code(p, size, 0); at < size;) {
        const u8 code = at + 3 < size ? p[at + 3] : 0;
        Bits b{p, size, u64{at + 4} * 8};
        if (code == 0xB3) {
            if (in_picture) break;
            parse_sequence(base, x, b);
            x.sequence = true;
            flag |= EX_SEQUENCE;
            last = code;
        } else if (code == 0xB5 && x.sequence) {
            const u32 id = b.bits(4);
            if (id == 1) {
                parse_sequence_extension(base, x, b);
                flag |= EX_SEQUENCE_EXT;
            } else if (id == 2) {
                parse_display_extension(base, x, b);
                flag |= EX_DISPLAY_EXT;
            } else if (id == 3) {
                parse_quant_matrix(x, b);
            } else if (id == 8) {
                parse_picture_coding(x, b);
                flag |= EX_PICTURE_EXT;
            } else if (id == 7) {
                parse_picture_display(x, b);
                flag |= EX_PICTURE_DISPLAY;
            }
        } else if (code == 0xB8 && x.sequence) {
            parse_gop(x, b);
            flag |= EX_GOP;
            last = code;
        } else if (code == 0xB2 && x.sequence) {
            flag |= last == 0xB3 ? EX_SEQUENCE_USER : last == 0xB8 ? EX_GOP_USER : EX_PICTURE_USER;
            parse_user_data(x, p, size, at, last == 0xB3 ? 0 : last == 0xB8 ? 1 : 2);
        } else if (code == 0xB7) {
            // The sequence's end: as FlushPicture (after the picture before it, which it ends).
            flag |= EX_SEQUENCE_END;
            if (in_picture) {
                end_after = true;
                used = std::min(size, at + 4);
                break;
            }
            flush(x);
        } else if (code == 0x00) {
            if (in_picture) break; // a second picture: left
            if (!x.sequence) break;
            type = parse_picture(x, b);
            in_picture = true;
            flag |= EX_PICTURE;
            last = code;
        } else if (code >= 0x01 && code <= 0xAF && in_picture) {
            if (!any) first_slice = at;
            any = true;
        }
        // The next start code from the byte after what the header read: a header that reads on past its end (fields
        // its flags call for that are not there) takes the start codes it reads with it.
        at = start_code(p, size, static_cast<u32>(std::max<u64>(at + 4, (b.pos + 7) / 8)));
        used = at;
    }
    if (!x.sequence || !in_picture) {
        used = size;
        return score::ST_NO_PICTURE | x.warnings;
    }
    const u32 picture_end = end_after ? used - 4 : used;
    if (!any && !end_after) used = size;
    u32 st = x.warnings;
    if (x.mpeg2 && !(flag & EX_PICTURE_EXT)) st |= ST_DAMAGED;
    // The frame: a new one, or the other field of the last picture's.
    const SideInfo& si = x.side;
    const int tr = si.b[0x0C] << 8 | si.b[0x0D], structure = si.b[0x1C];
    bool second = false;
    if (structure != 3 && x.last_tr == tr && x.current >= 0) {
        if (structure == x.last_structure) st |= score::ST_NO_PICTURE; // a field of the parity already decoded
        else second = true;
    }
    x.last_tr = tr;
    x.last_structure = structure;
    int slot = x.current;
    if (!second) {
        slot = q.free_slot();
        if (slot < 0) return score::ST_DEAD; // DecodePicture checked a slot was free
        x.current = slot;
        x.au_index = 0;
        x.fields[slot] = x.warned[slot] = 0;
        x.ref_forward = reference_slot(type == 3 ? q.backward : q.forward);
        x.ref_backward = reference_slot(q.forward);
        q.flushed = false;
        if (type != 3) {
            give_pending(x);
            x.pending = slot;
            q.anchor(slot);
        }
    } else {
        x.au_index = 1;
    }
    if (x.width * x.height > x.max_w * x.max_h || x.width > align(x.max_w, 16) || x.height > align(x.max_h, 16)) {
        PS_LOG_ERROR(kChan, "instance {:#x}: a {}x{} picture does not fit its frame buffers ({}x{})", x.handle, x.width,
            x.height, x.max_w, x.max_h);
        x.dead = true;
        used = first_slice;
        // Only the picture's size reaches its slot's information block.
        ps_write32(base, info_of(x, slot) + 0x0C, x.width);
        ps_write32(base, info_of(x, slot) + 0x10, x.height);
        return ST_TOO_LARGE;
    }
    if (type == 3 && x.skip_b) {
        st |= ST_SKIPPED;
        std::memset(x.bnr, 0, sizeof(x.bnr));
    } else if (st & (u32{ST_DAMAGED} | u32{score::ST_NO_PICTURE})) {
        // An MPEG-2 picture without its coding extension, a field of the parity already decoded: not decoded (the
        // frame buffer keeps what it had, the BNR data the last picture's).
    } else {
        // The picture decoded into its slot's frame buffer, predicted from its references' (the program's look at the
        // slices: vdec_mpeg2.hpp).
        mpeg2dec::Picture pic;
        pic.mpeg1 = !x.mpeg2;
        pic.type = static_cast<int>(type);
        pic.structure = structure;
        pic.second_field = second;
        if (x.mpeg2) {
            for (u32 k = 0; k < 4; k++) pic.f_code[k / 2][k % 2] = si.b[0x17 + k];
        } else {
            pic.f_code[0][0] = pic.f_code[0][1] = si.b[0x13];
            pic.f_code[1][0] = pic.f_code[1][1] = si.b[0x15];
            pic.full_pel[0] = si.b[0x12];
            pic.full_pel[1] = si.b[0x14];
        }
        pic.intra_dc_precision = si.b[0x1B];
        pic.top_field_first = si.b[0x1D];
        pic.frame_pred_frame_dct = si.b[0x1E];
        pic.concealment_motion_vectors = si.b[0x1F];
        pic.q_scale_type = si.b[0x20];
        pic.intra_vlc_format = si.b[0x21];
        pic.alternate_scan = si.b[0x22];
        const int mbw = static_cast<int>((x.width + 15) / 16), mbh = static_cast<int>((x.height + 15) / 16);
        pic.mb_width = mbw;
        pic.mb_rows = structure == 3 ? mbh : static_cast<int>((x.height + 31) / 32);
        pic.vertical_size = static_cast<int>(x.height);
        std::memcpy(pic.intra_matrix, x.intra_w, 64);
        std::memcpy(pic.non_intra_matrix, x.non_intra_w, 64);
        const auto frame_at = [&](int s) { return mpeg2dec::Frame{base + frame_of(x, s), mbw, mbh}; };
        pic.out = frame_at(slot);
        pic.forward = frame_at(x.ref_forward);
        pic.backward = frame_at(x.ref_backward);
        const mpeg2dec::Result scan =
            mpeg2dec::decode(p + first_slice, picture_end - std::min(picture_end, first_slice), pic);
        std::memcpy(x.bnr, scan.bnr, sizeof(x.bnr));
        if (scan.damaged) st |= ST_DAMAGED;
        // The references the picture's macroblocks predict from.
        // (A P frame's second field: its frame, whose first field it is predicted from.)
        const int forward = second && type == 2 ? slot : x.ref_forward;
        if (scan.forward) {
            if (!valid(x, forward)) st |= ST_NO_REFERENCE;
            else if (x.warned[forward]) st |= ST_REFERENCE_WARNED;
        }
        if (scan.backward) {
            if (!valid(x, x.ref_backward)) st |= ST_NO_REFERENCE;
            else if (x.warned[x.ref_backward]) st |= ST_REFERENCE_WARNED;
        }
    }
    if (st & ~0x10u) x.warned[slot] |= st;
    write_info(base, x, slot, x.au_index, st, x.bnr);
    x.fields[slot] |= static_cast<u32>(structure);
    if (x.fields[slot] == 3) {
        if (type == 3) q.ready(slot);
        // A picture predicted from nothing, or skipped, is no reference.
        if (st & (ST_SKIPPED | ST_NO_REFERENCE)) x.fields[slot] = x.warned[slot] = 0;
    }
    std::memset(x.side.aux, 0, 16);
    if (end_after) flush(x);
    return st;
}

// ---- the calls

std::shared_ptr<Smvd2> instance(u32 handle, std::string_view call)
{
    std::shared_ptr<Smvd2> x = smvd2s().find(handle);
    if (!x) PS_LOG_ERROR(kChan, "{} on {:#x}, which is no instance", call, handle);
    return x;
}

// The instance's record back into the local store after a command (the next instance starts with it).
void keep(const Smvd2& x)
{
    LocalStore& ls = local_store();
    std::lock_guard lk(ls.m);
    ls.of(x.spurs) = x.side;
}

s32 smvd2GetMemorySize(PPUContext& ctx, u32 size, u32 level)
{
    if (!writable(ctx, size, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    u32 w = 0, h = 0;
    if (!level_size(level, w, h)) {
        ps_write32(base, size, 0);
        return fail(score::LOW_SEQ);
    }
    ps_write32(base, size, memory_for(w, h, 6));
    return lv2::CELL_OK;
}

// The picture size type 5's parameters ask for (words 3 and 4), within the level's largest.
bool size_of(u8* base, u32 param, u32 level, u32& w, u32& h)
{
    u32 lw = 0, lh = 0;
    const s32 pw = static_cast<s32>(ps_read32(base, param + 0x0C)), ph = static_cast<s32>(ps_read32(base, param + 0x10));
    if (!level_size(level, lw, lh) || pw < 1 || ph < 1 || pw > static_cast<s32>(lw) || ph > static_cast<s32>(lh))
        return false;
    w = static_cast<u32>(pw);
    h = static_cast<u32>(ph);
    return true;
}

s32 smvd2GetMemorySize2(PPUContext& ctx, u32 size, u32 level, u32 param)
{
    if (!writable(ctx, size, 4) || !writable(ctx, param, 0x14)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, param) != 5) return fail(score::LOW_SEQ);
    u32 w = 0, h = 0;
    if (!size_of(base, param, level, w, h)) {
        ps_write32(base, size, 0);
        return fail(score::LOW_SEQ);
    }
    ps_write32(base, size, memory_for(w, h, ps_read32(base, param + 4)));
    return lv2::CELL_OK;
}

s32 smvd2CreateInstance2(PPUContext& ctx, u32 mem, u32 size, u32 level, u32 param)
{
    if (!mem || (mem & 0x7F) || !writable(ctx, param, 0x14)) return fail(score::LOW_ARG);
    if (size < kCleared) return fail(score::LOW_NO_MEMORY);
    u8* base = guest_base(ctx);
    if (!guest_readable(thread_of(ctx), mem, kCleared)) return fail(score::LOW_ARG);
    std::memset(base + mem, 0, kCleared);
    const u32 type = ps_read32(base, param), spus = ps_read32(base, param + 4), taskset = ps_read32(base, param + 8);
    u32 w = 0, h = 0, need = 0;
    if (type == 4) {
        if (level_size(level, w, h)) need = memory_for(w, h, 6);
    } else if (type == 5) {
        if (size_of(base, param, level, w, h)) need = memory_for(w, h, spus);
    } else {
        return fail(score::LOW_SEQ);
    }
    if (!taskset || spus < 1 || spus > 6) return fail(score::LOW_SEQ);
    if (!need || size < need) return fail(score::LOW_NO_MEMORY);
    if (!guest_readable(thread_of(ctx), mem, need)) {
        PS_LOG_ERROR(kChan, "CreateInstance2: memory {:#x} ({:#x} bytes) is not guest memory", mem, need);
        return fail(score::LOW_ARG);
    }
    if (const s32 r = score::make_queues(ctx, mem, taskset, queue_places(spus)); r != lv2::CELL_OK) return r;
    auto x = std::make_shared<Smvd2>();
    x->handle = mem;
    x->spus = spus;
    x->max_w = w;
    x->max_h = h;
    x->queue.slots = kSlots;
    std::memcpy(x->intra_w, mpeg2dec::kDefaultIntra, 64);
    std::memset(x->non_intra_w, 16, 64);
    x->spurs = spurs_of(base, taskset);
    if (!ps_read8(base, taskset + spurs::ts_off::enable_clear_ls)) { // a taskset that clears them: none
        LocalStore& ls = local_store();
        std::lock_guard lk(ls.m);
        x->side = ls.of(x->spurs);
    }
    smvd2s().add(mem, std::move(x));
    return lv2::CELL_OK;
}

s32 smvd2ReleaseInstance(PPUContext& ctx, u32 handle)
{
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = smvd2s().find(handle);
    if (!x || !smvd2s().remove(handle)) {
        PS_LOG_ERROR(kChan, "ReleaseInstance of {:#x}, which is no instance", handle);
        return lv2::CELL_OK;
    }
    score::drop_queues(ctx, handle, queue_places(x->spus));
    return lv2::CELL_OK;
}

s32 smvd2Initialize(PPUContext& ctx, u32 handle)
{
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "Initialize");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    ps_write8(guest_base(ctx), handle + kSeq, 0);
    x->queue.reset();
    x->pending = -1;
    x->current = -1;
    x->sequence = false;
    // A dead program resets nothing of its own (and answers 0).
    if (!x->dead) {
        x->mpeg2 = false;
        std::memset(x->side.aux, 0, 16);
        discontinuity(*x);
        keep(*x);
    }
    return lv2::CELL_OK;
}

s32 decode(PPUContext& ctx, u32 handle, u32 au, u32 used_out, bool two)
{
    if (!handle || !writable(ctx, au, two ? 16 : 12) || !writable(ctx, used_out, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, au)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, two ? "DecodePicture2" : "DecodePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->queue.free_slot() < 0) {
        ps_write32(base, used_out, 0);
        return fail(score::LOW_BUSY);
    }
    const u32 words = two ? 4 : 3;
    for (u32 k = 0; k < words; k++)
        ps_write32(base, handle + (two ? kAu2Copy : kAuCopy) + 4 * k, ps_read32(base, au + 4 * k));
    // The access unit. Two pieces are joined as the program's DMA joins them: the second lands at the first place
    // after the first whose address is the second's modulo 16, the bytes after the first piece in memory between them.
    std::vector<u8> data;
    const u32 pieces = two ? 2 : 1;
    u32 gap = 0, first_size = 0;
    for (u32 k = 0; k < pieces; k++) {
        const u32 addr = ps_read32(base, au + 8 * k), size = ps_read32(base, au + 8 * k + 4);
        if (k == 1 && first_size) {
            const u32 first_addr = ps_read32(base, au);
            gap = (addr - first_addr - first_size) & 15;
            if (gap && size) {
                if (!guest_readable(thread_of(ctx), first_addr + first_size, gap)) gap = 0;
                else data.insert(data.end(), base + first_addr + first_size, base + first_addr + first_size + gap);
            }
        }
        if (k == 0) first_size = size;
        if (!size) continue;
        if (!guest_readable(thread_of(ctx), addr, size)) {
            PS_LOG_ERROR(kChan, "instance {:#x}: an access unit at {:#x} ({} bytes) that is not memory", handle, addr,
                size);
            return score::answer(kPrefix, score::ST_DEAD);
        }
        data.insert(data.end(), base + addr, base + addr + size);
    }
    u32 st = 0;
    if (x->dead) {
        // A dead program decodes nothing, but its picture queue still takes each picture (an anchor gives out the one
        // before it); nothing else of the access unit is read.
        st = score::ST_DEAD;
        ps_write32(base, handle + kUsed, 0);
        const u32 at = start_code(data.data(), static_cast<u32>(data.size()), 0);
        for (u32 k = at; k + 5 < data.size(); k = start_code(data.data(), static_cast<u32>(data.size()), k + 4)) {
            if (data[k + 3] != 0x00) continue;
            const u32 type = (data[k + 5] >> 3) & 7;
            const int slot = x->queue.free_slot();
            if (slot >= 0) {
                if (type == 3) {
                    x->queue.ready(slot);
                } else {
                    give_pending(*x);
                    x->pending = slot;
                    x->queue.anchor(slot);
                }
            }
            break;
        }
    } else {
        // Decoded from a host copy of the access unit at a guest-like place: the parse reads guest memory.
        u32 used = 0, flag = 0;
        const u32 at = ps_read32(base, au);
        if (!two) {
            st = spu_decode(base, *x, at, static_cast<u32>(data.size()), used, flag);
        } else {
            StackVar copy(ctx, base, static_cast<u32>(data.size()) + 16);
            std::memcpy(base + copy.addr(), data.data(), data.size());
            st = spu_decode(base, *x, copy.addr(), static_cast<u32>(data.size()), used, flag);
            // The bytes used, counted in the access unit's own (not the gap's).
            if (used > first_size) used = used < first_size + gap ? first_size : used - gap;
        }
        ps_write32(base, handle + kUsed, used);
        ps_write32(base, handle + kFlag, flag);
        keep(*x);
    }
    ps_write32(base, used_out, ps_read32(base, handle + kUsed));
    return score::answer(kPrefix, st);
}

s32 smvd2DecodePicture(PPUContext& ctx, u32 handle, u32 au, u32 used) { return decode(ctx, handle, au, used, false); }
s32 smvd2DecodePicture2(PPUContext& ctx, u32 handle, u32 au, u32 used) { return decode(ctx, handle, au, used, true); }

s32 smvd2FlushPicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "FlushPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return score::answer(kPrefix, score::ST_DEAD);
    flush(*x);
    keep(*x);
    return lv2::CELL_OK;
}

s32 smvd2Discontinue(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "Discontinue");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return score::answer(kPrefix, score::ST_DEAD);
    discontinuity(*x);
    keep(*x);
    return lv2::CELL_OK;
}

s32 smvd2CancelDecode(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "CancelDecode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    x->queue.reset();
    x->pending = -1;
    x->current = -1;
    if (x->dead) return score::answer(kPrefix, score::ST_DEAD);
    discontinuity(*x);
    keep(*x);
    return lv2::CELL_OK;
}

s32 smvd2SetDecodeMode(PPUContext& ctx, u32 handle, u32 mode)
{
    if (!handle || !writable(ctx, mode, 8)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "SetDecodeMode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    u8* base = guest_base(ctx);
    ps_write32(base, handle + kMode, ps_read32(base, mode));
    ps_write32(base, handle + kMode + 4, ps_read32(base, mode + 4));
    x->skip_b = ps_read32(base, mode) != 0;
    return lv2::CELL_OK;
}

// GetPicture's fourth word: how the picture is made (frames and fields, their order, repeated fields).
u32 picture_format(u8* base, u32 info)
{
    const u8 structure = ps_read8(base, info + kInfoStructure), progressive = ps_read8(base, info + kInfoProgressive);
    const u8 tff = ps_read8(base, info + kInfoSide + 0x1D), rff = ps_read8(base, info + kInfoSide + 0x23);
    const u32 count = ps_read16(base, info + kInfoCount);
    if (progressive) {
        u32 v = 4;
        if (rff) v = (tff ? 0x3000u : 0u) + 0x1004;
        return v | 0x20000000;
    }
    if (structure == 3) {
        if (count == 1) {
            u32 v = tff ? (rff ? 0x4010u : 0x0010u) : (rff ? 0x5010u : 0x1010u);
            v |= 0x20000000;
            if (ps_read8(base, info + kInfoSide + 0x25) == 1) v |= 4;
            return v;
        }
        return ps_read8(base, info + kInfoSide + 0x1C) == 1 ? 0x20000010 : 0x20001010;
    }
    return structure == 1 ? 0x20000410 : 0x20001410;
}

s32 smvd2GetPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x24)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "GetPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    const int slot = x->queue.take();
    if (slot < 0) return fail(score::LOW_EMPTY);
    u8* base = guest_base(ctx);
    const u32 info = info_of(*x, slot);
    for (u32 k = 0; k < 3; k++) ps_write32(base, out + 4 * k, ps_read32(base, info + 4 * k));
    ps_write32(base, out + 12, picture_format(base, info));
    for (u32 k = 0; k < 4; k++) ps_write32(base, out + 16 + 4 * k, ps_read32(base, info + 12 + 4 * k));
    ps_write32(base, out + 32, 0);
    const u32 count = ps_read16(base, info + kInfoCount);
    u32 st = count ? 0 : score::ST_EMPTY;
    for (u32 k = 0; k < count; k++) st |= ps_read32(base, info + kInfoStatus + 4 * k);
    return score::picture_answer(kPrefix, st);
}

s32 smvd2GetNumPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "GetNumPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    ps_write32(guest_base(ctx), out, x->queue.count());
    return x->queue.flushed ? score::answer(kPrefix, score::ST_FLUSHED) : lv2::CELL_OK;
}

s32 smvd2ReleasePicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, "ReleasePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    return x->queue.release_one() ? lv2::CELL_OK : fail(score::LOW_EMPTY);
}

// GetSideInfoPic, GetAuxData and GetBnrData: the picture GetPicture gave last, its access units' blocks.
s32 blocks_of_picture(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second, u32 from, u32 bytes,
    std::string_view call)
{
    if (!handle || !writable(ctx, count, 4) || !writable(ctx, first, bytes) || !second) return fail(score::LOW_ARG);
    const std::shared_ptr<Smvd2> x = instance(handle, call);
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->queue.current < 0) return fail(score::LOW_EMPTY);
    u8* base = guest_base(ctx);
    const u32 info = info_of(*x, x->queue.current);
    const u32 n = ps_read16(base, info + kInfoCount);
    if (n > 1 && !writable(ctx, second, bytes)) return fail(score::LOW_ARG);
    score::copy_blocks(base, count, first, second, info + from, n, bytes);
    return lv2::CELL_OK;
}

s32 smvd2GetSideInfoPic(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    return blocks_of_picture(ctx, handle, count, first, second, kInfoSide, kSideBytes, "GetSideInfoPic");
}
s32 smvd2GetAuxData(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    return blocks_of_picture(ctx, handle, count, first, second, kInfoAux, 0x10, "GetAuxData");
}
s32 smvd2GetBnrData(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    return blocks_of_picture(ctx, handle, count, first, second, kInfoBnr, 0x20, "GetBnrData");
}

s32 smvd2GetSideInfoSeq(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x20) || !guest_readable(thread_of(ctx), handle + kSeq, 0x20))
        return fail(score::LOW_ARG);
    score::copy_skipping(guest_base(ctx), out, handle + kSeq, 0x20, {0x01});
    return lv2::CELL_OK;
}

s32 smvd2GetExistenceFlag(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4) || !guest_readable(thread_of(ctx), handle + kFlag, 4))
        return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    ps_write32(base, out, ps_read32(base, handle + kFlag));
    return lv2::CELL_OK;
}

s32 smvd2SetAuxData(PPUContext& ctx, u32 handle, u32 data)
{
    if (!handle || !writable(ctx, data, 16) || !guest_readable(thread_of(ctx), handle + kAux, 16))
        return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    std::memmove(base + handle + kAux, base + data, 16);
    return lv2::CELL_OK;
}

} // namespace

void register_smvd2(Registry& r)
{
    r.add("libsmvd2", {
        PS_HLE_FUNC(0x3B95270D, smvd2GetMemorySize),
        PS_HLE_FUNC(0xD09740F6, smvd2GetMemorySize2),
        PS_HLE_FUNC(0xDC6F4FF1, smvd2CreateInstance2),
        PS_HLE_FUNC(0x540B43AE, smvd2ReleaseInstance),
        PS_HLE_FUNC(0xE3072DF9, smvd2Initialize),
        PS_HLE_FUNC(0x60D0F4FA, smvd2DecodePicture),
        PS_HLE_FUNC(0x1A5B72DD, smvd2DecodePicture2),
        PS_HLE_FUNC(0xF007F937, smvd2FlushPicture),
        PS_HLE_FUNC(0xD8D930BC, smvd2Discontinue),
        PS_HLE_FUNC(0xFF05DD1F, smvd2CancelDecode),
        PS_HLE_FUNC(0x5A0355CF, smvd2SetDecodeMode),
        PS_HLE_FUNC(0xA0D447EB, smvd2GetPicture),
        PS_HLE_FUNC(0xE9FFF717, smvd2GetNumPicture),
        PS_HLE_FUNC(0x6B49A7C7, smvd2ReleasePicture),
        PS_HLE_FUNC(0x4A1964A6, smvd2GetSideInfoPic),
        PS_HLE_FUNC(0x32DA1344, smvd2GetAuxData),
        PS_HLE_FUNC(0xF4E8D559, smvd2GetBnrData),
        PS_HLE_FUNC(0xE36F2A6C, smvd2GetSideInfoSeq),
        PS_HLE_FUNC(0x6D1F7D3F, smvd2GetExistenceFlag),
        PS_HLE_FUNC(0x96C2A2D9, smvd2SetAuxData),
    });
}

} // namespace pspocket::hle::vdec_core
