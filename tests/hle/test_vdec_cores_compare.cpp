// The video codec cores' own APIs against Sony's modules (D-920): each core library called directly, as Sony's libvdec
// (or a game) calls it, with the same synthetic streams through pspocket's core and through Sony's module (the user's
// installed system software), each core in a process of its own per half. Sony's cores decode on SPURS: each half
// makes a SPURS instance and a taskset (pspocket's cellSpurs) and gives the taskset to the core's open. Compared: every
// answer and output structure, the frame buffers' places (as offsets into the core's memory) and their contents (the
// IDCT may round one step apart, as D-912 accepts for DivX and MPEG-4; VC-1 exactly), the pictures' order and the
// errors. The cores' own memory (their state, SPU tasks' areas) is opaque: not compared. Skipped without the firmware
// (compare_util.hpp) and without FFmpeg's encoders. No game data. libdivx311dec and libdivxdec so far.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "compare_util.hpp"
#include "vdec_streams.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

#if PSPOCKET_HAVE_FFMPEG
using namespace pspocket::hle::test::vdec_streams;

namespace {

// Guest memory: the core's memory, SPURS and its taskset, the arguments and the streams.
constexpr u32 kMem = 0x01000000, kMemSize = 0x02000000;
constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kTsAttr = kData + 0x4000, kPrio = kData + 0x4400,
              kTs = kData + 0x6000; // the taskset: 6400 bytes
constexpr u32 kArg = kData + 0x8000, kParam = kArg, kOut = kArg + 0x40, kOut2 = kArg + 0x48, kAu = kArg + 0x80,
              kStatus = kArg + 0xC0, kPic = kArg + 0x100;
constexpr u32 kStream = kData + 0x10000;

struct CoreFixture : Fixture {
    CoreFixture()
    {
        REQUIRE(hle->main_memory().reserve(kMem, kMemSize));
        REQUIRE(mem.commit(kMem, kMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};
using Run = Half<CoreFixture>;

std::string in_core(u32 addr)
{
    if (addr >= kMem && addr < kMem + kMemSize) return std::format("the core's memory +{:#x}", addr - kMem);
    return std::format("{:#x}", addr);
}

// A SPURS instance with two SPUs and a taskset on them, as libvdec makes them (pspocket's cellSpurs in both halves).
void make_spurs(Run& r)
{
    REQUIRE(r.t.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 2, 200, 1000, 1) == 0);
    REQUIRE(r.t.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    const u8 prio[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    std::memcpy(r.b + kPrio, prio, 8);
    REQUIRE(r.t.call("_cellSpursTasksetAttributeInitialize", kTsAttr, 1, 0x330000, 0, kPrio, 2) == 0);
    REQUIRE(r.t.call("cellSpursCreateTasksetWithAttribute", kSpurs, kTs, kTsAttr) == 0);
}

void end_spurs(Run& r)
{
    r.t.call("cellSpursShutdownTaskset", kTs);
    r.t.call("cellSpursJoinTaskset", kTs);
    r.t.call("cellSpursFinalize", kSpurs);
}

// ------------------------------------------------------------------------------------------------- DivX 3.11

// divx311DecQueryAttr's parameters {type, width, height, frame buffers}.
void divx311_param(u8* b, u32 type, u32 w, u32 h, u32 n)
{
    ps_write32(b, kParam + 0, type);
    ps_write32(b, kParam + 4, w);
    ps_write32(b, kParam + 8, h);
    ps_write32(b, kParam + 12, n);
}

// The frame buffer's bytes of a DivX picture (libdivx311dec's strips: the start address is the frame's).
u32 divx_frame_bytes(u32 w, u32 h)
{
    return ((((h + 1) / 2 + 7) & ~7u) + ((h + 15) & ~15u) + 0x30) * (((w + 15) & ~15u) + 0x20);
}

struct Divx311 {
    Run& r;
    u32 handle = 0, width = 0, height = 0;
    u32 taken = 0;
    u32 at = kStream;

    std::string w(u32 v) { return std::format("{:#x}", v); }

    // The core's own state after a call, as a note (opaque, not compared): its first 0x80 bytes and its picture and
    // frame tables.
    void note_state(const std::string& key)
    {
        if (!handle || !env_on("PSPOCKET_CORE_STATE")) return; // a probe's: PSPOCKET_CORE_STATE=1
        r.o.note(key + ": state " + compare::hex(r.b + handle, 0x80));
        r.o.note(key + ": tables " + compare::hex(r.b + handle + 0x3D580, 0xD8));
    }

    u64 query(const std::string& what, u32 type, u32 wd, u32 ht, u32 n, bool null_size = false, bool null_ver = false)
    {
        u8* b = r.b;
        divx311_param(b, type, wd, ht, n);
        std::memset(b + kOut, 0xEE, 16);
        const u64 v = r.r("divx311DecQueryAttr", {kParam, null_size ? 0u : kOut, null_ver ? 0u : kOut2}, "QueryAttr " + what);
        r.o.line("QueryAttr " + what + ": the outputs", compare::hex(b + kOut, 16));
        return v;
    }

    // Pictures of a decoder opened for another size than its access units': libdivx311dec's SPU program decodes them
    // otherwise (its first picture is up to three steps off pspocket's, whose decoder takes the access units' size):
    // their frame buffers are notes.
    bool other_size = false;

    // Each picture the core holds: GetNumOfPictures, then GetPicture (its 11 words, the frame address as an offset)
    // and the frame buffer; ReleasePicture after each when `release`.
    void take(const std::string& when, bool release, u32 limit = ~0u)
    {
        u8* b = r.b;
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("divx311DecGetNumOfPictures", {handle, kOut}, when + ": GetNumOfPictures");
        r.o.line(when + ": pictures", w(ps_read32(b, kOut)));
        for (u32 k = 0; k < limit; k++) {
            std::memset(b + kPic, 0xEE, 0x40);
            const std::string key = std::format("{}: picture {}", when, taken);
            const u64 got = r.r("divx311DecGetPicture", {handle, kPic}, key + ": GetPicture");
            if (static_cast<u32>(got) != 0 && static_cast<u32>(got) != 0x80615540) return;
            taken++;
            std::vector<u8> words(b + kPic, b + kPic + 0x30);
            const u32 frame = ps_read32(b, kPic);
            std::memset(words.data(), 0, 4);
            r.o.line(key + ": the picture's frame", in_core(frame));
            r.o.line(key + ": the picture", compare::hex(words));
            if (frame >= kMem && frame + divx_frame_bytes(width, height) <= kMem + kMemSize) {
                if (other_size) {
                    u32 sum = 0;
                    for (u32 i = 0; i < divx_frame_bytes(width, height); i++) sum += b[frame + i];
                    r.o.note(std::format("{}: its frame buffer's sum {:#x}", key, sum));
                } else {
                    r.o.bytes(key + ": its frame buffer", b + frame, divx_frame_bytes(width, height), Rule::within(1));
                }
            }
            if (release) r.r("divx311DecReleasePicture", {handle}, key + ": ReleasePicture");
        }
    }

    u64 decode(const std::string& key, const std::vector<u8>& au, u32 mode, u32 aw, u32 ah, u32 user)
    {
        u8* b = r.b;
        if (at + au.size() > kStream + 0x80000) at = kStream;
        std::memcpy(b + at, au.data(), au.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, aw);
        ps_write32(b, kAu + 12, ah);
        ps_write32(b, kAu + 16, 0x1000 + user);
        ps_write32(b, kAu + 20, 0x2000 + user);
        ps_write32(b, kAu + 24, 0x3000 + user);
        ps_write32(b, kAu + 28, 0x4000 + user);
        at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
        ps_write32(b, kStatus, 0xEEEEEEEE);
        const u64 v = r.r("divx311DecDecodeAu", {handle, kAu, mode, kStatus}, key);
        r.o.line(key + ": status", w(ps_read32(b, kStatus)));
        note_state(key);
        return v;
    }
};

Observed divx311(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libdivx311dec");
    u8* b = r.b;
    make_spurs(r);
    Divx311 d{r};
    // QueryAttr: the type 15 (the largest frames), sizes, buffer counts and the refusals.
    d.query("type 15", 15, 0, 0, 2);
    d.query("type 15, 3 buffers", 15, 0, 0, 3);
    for (const auto& [wd, ht] : {std::pair{96u, 64u}, {16u, 16u}, {15u, 16u}, {16u, 15u}, {1920u, 1088u}, {1921u, 64u},
             {64u, 1089u}, {720u, 576u}, {0u, 0u}})
        d.query(std::format("{}x{}", wd, ht), 0, wd, ht, 2);
    d.query("96x64, 1 buffer", 0, 96, 64, 1);
    d.query("96x64, 4 buffers", 0, 96, 64, 4);
    d.query("96x64, type 14", 14, 96, 64, 2);
    d.query("96x64, no size", 0, 96, 64, 2, true);
    d.query("96x64, no version", 0, 96, 64, 2, false, true);
    r.r("divx311DecQueryAttr", {0, kOut, kOut2}, "QueryAttr with no parameters");

    const std::vector<std::vector<u8>> packets = encode(AV_CODEC_ID_MSMPEG4V3, 96, 64, 8, 4);
    divx311_param(b, 0, 96, 64, 2);
    ps_write32(b, kOut, 0);
    if (r.r("divx311DecQueryAttr", {kParam, kOut, kOut2}, "QueryAttr for the decoder") != 0) {
        end_spurs(r);
        return std::move(r.o);
    }
    const u32 need = ps_read32(b, kOut);
    // Open's refusals: the memory for the frame buffers and 0x3D77F bytes is needed (QueryAttr's size is more).
    const u32 frames = 2 * divx_frame_bytes(96, 64);
    std::memset(b + kMem, 0xA5, 0x80000);
    r.r("divx311DecOpen", {kParam, kMem, frames + 0x3D77E, kTs}, "Open with one byte less");
    if (ps_read32(b, kMem + 0x3D64C) == 0xBABEFACE) r.side.call("divx311DecClose", {kMem});
    std::memset(b + kData + 0x7000, 0, 0x1000);
    r.r("divx311DecOpen", {kParam, kMem, need, kData + 0x7000}, "Open with a taskset that is none");
    r.r("divx311DecOpen", {0, kMem, need + 0x3D77F, kTs}, "Open with no parameters");
    r.r("divx311DecOpen", {kParam, 0, need + 0x3D77F, kTs}, "Open with no memory");
    r.r("divx311DecOpen", {kParam, kMem, need + 0x3D77F, 0}, "Open with no taskset");
    divx311_param(b, 0, 96, 64, 3);
    r.r("divx311DecOpen", {kParam, kMem, need + 0x3D77F, kTs}, "Open with 3 buffers");
    divx311_param(b, 0, 96, 64, 2);
    // The decoder: its memory filled first.
    std::memset(b + kMem, 0xA5, need + 0x40000);
    d.width = 96;
    d.height = 64;
    if (r.r("divx311DecOpen", {kParam, kMem, need + 0x3D77F, kTs}, "Open") == 0) {
        d.handle = kMem;
        r.r("divx311DecOpen", {kParam, kMem, need + 0x3D77F, kTs}, "Open again on the same memory");
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("divx311DecGetNumOfPictures", {d.handle, kOut}, "GetNumOfPictures before a sequence");
        r.o.line("pictures before a sequence", std::format("{:#x}", ps_read32(b, kOut)));
        r.r("divx311DecGetPicture", {d.handle, kPic}, "GetPicture before a sequence");
        r.r("divx311DecReleasePicture", {d.handle}, "ReleasePicture before a sequence");
        r.r("divx311DecFlushPicture", {d.handle}, "FlushPicture before a sequence");
        d.decode("DecodeAu before a sequence", packets[0], 0x20, 96, 64, 0);
        r.r("divx311DecReset", {d.handle, 0}, "Reset");
        // A sequence as libvdec lives it: each access unit, its pictures taken and released at once.
        for (usize i = 0; i < packets.size(); i++) {
            d.decode(std::format("DecodeAu {}", i), packets[i], 0x20, 96, 64, static_cast<u32>(i));
            d.take(std::format("after DecodeAu {}", i), true);
        }
        r.r("divx311DecFlushPicture", {d.handle}, "FlushPicture");
        d.take("after FlushPicture", true);
        r.r("divx311DecFlushPicture", {d.handle}, "FlushPicture again");
        d.decode("DecodeAu after FlushPicture", packets[0], 0x20, 96, 64, 50);
        // Pictures held: how many access units the core takes before it has no frame buffer.
        r.r("divx311DecReset", {d.handle, 0}, "Reset for holding");
        for (usize i = 0; i < 5; i++)
            d.decode(std::format("held: DecodeAu {}", i), packets[i], 0x20, 96, 64, 100 + static_cast<u32>(i));
        d.take("held", false, 1);
        r.r("divx311DecReleasePicture", {d.handle}, "held: ReleasePicture");
        d.decode("held: DecodeAu again", packets[2], 0x20, 96, 64, 110);
        d.take("held, then", true);
        // The decode modes and the checks of DecodeAu.
        r.r("divx311DecReset", {d.handle, 1}, "Reset 1");
        d.decode("mode 0x22", packets[0], 0x22, 96, 64, 200);
        d.decode("mode 0x21", packets[1], 0x21, 96, 64, 201);
        d.decode("mode 0x23", packets[2], 0x23, 96, 64, 202);
        d.decode("mode 0x1f", packets[2], 0x1F, 96, 64, 203);
        d.take("modes", true);
        r.r("divx311DecReset", {d.handle, 0}, "Reset 2");
        d.decode("a P picture first after Reset", packets[1], 0x20, 96, 64, 204);
        d.take("P first", true);
        ps_write32(b, kAu + 4, 0);
        r.r("divx311DecDecodeAu", {d.handle, kAu, 0x20, kStatus}, "DecodeAu with size 0");
        r.r("divx311DecDecodeAu", {d.handle, 0, 0x20, kStatus}, "DecodeAu with no access unit");
        d.decode("DecodeAu with no status", packets[0], 0x20, 96, 64, 205);
        d.decode("DecodeAu told 64x48", packets[0], 0x20, 64, 48, 206);
        d.take("told 64x48", true);
        // GetPicture with none ready takes the head entry off: what the decoder does after it.
        r.r("divx311DecReset", {d.handle, 0}, "Reset 3");
        d.decode("quirk: DecodeAu 0", packets[0], 0x20, 96, 64, 300);
        r.r("divx311DecGetPicture", {d.handle, kPic}, "quirk: GetPicture with none ready");
        d.decode("quirk: DecodeAu 1", packets[1], 0x20, 96, 64, 301);
        d.take("quirk: after DecodeAu 1", true);
        d.decode("quirk: DecodeAu 2", packets[2], 0x20, 96, 64, 302);
        d.take("quirk: after DecodeAu 2", true);
        r.r("divx311DecGetPicture", {d.handle, kPic}, "quirk: GetPicture with none ready again");
        r.r("divx311DecFlushPicture", {d.handle}, "quirk: FlushPicture");
        d.take("quirk: after FlushPicture", true);
        // Pictures held two at a time, released together; one release too many.
        r.r("divx311DecReset", {d.handle, 0}, "Reset 4");
        for (usize i = 0; i < 6; i++) {
            d.decode(std::format("pairs: DecodeAu {}", i), packets[i], 0x20, 96, 64, 400 + static_cast<u32>(i));
            d.take(std::format("pairs: after DecodeAu {}", i), false);
            if (i % 2) {
                r.r("divx311DecReleasePicture", {d.handle}, std::format("pairs: ReleasePicture {} a", i));
                r.r("divx311DecReleasePicture", {d.handle}, std::format("pairs: ReleasePicture {} b", i));
                r.r("divx311DecReleasePicture", {d.handle}, std::format("pairs: ReleasePicture {} c", i));
            }
        }
        // Only I pictures (mode 0x21) through a whole stream, then the flush.
        r.r("divx311DecReset", {d.handle, 0}, "Reset 5");
        for (usize i = 0; i < packets.size(); i++) {
            d.decode(std::format("I only: DecodeAu {}", i), packets[i], 0x21, 96, 64, 500 + static_cast<u32>(i));
            d.take(std::format("I only: after DecodeAu {}", i), true);
        }
        r.r("divx311DecFlushPicture", {d.handle}, "I only: FlushPicture");
        d.take("I only: after FlushPicture", true);
        // A flush right after Reset (nothing decoded), then decoding again after Reset.
        r.r("divx311DecReset", {d.handle, 0}, "Reset 6");
        r.r("divx311DecFlushPicture", {d.handle}, "FlushPicture after Reset");
        d.take("after FlushPicture after Reset", true);
        r.r("divx311DecReset", {d.handle, 0}, "Reset 7");
        d.decode("after Reset 7: DecodeAu 0", packets[4], 0x20, 96, 64, 600);
        d.decode("after Reset 7: DecodeAu 1", packets[5], 0x20, 96, 64, 601);
        d.take("after Reset 7", true);
        r.r("divx311DecClose", {d.handle}, "Close");
        r.r("divx311DecClose", {d.handle}, "Close again");
        r.r("divx311DecGetNumOfPictures", {d.handle, kOut}, "GetNumOfPictures after Close");
        r.r("divx311DecClose", {0}, "Close with no handle");
        // Opened for 128x96, given 96x64 access units (the frame buffers are the size opened for).
        divx311_param(b, 0, 128, 96, 2);
        std::memset(b + kMem + 0x3D600, 0xA5, 0x100);
        if (r.r("divx311DecOpen", {kParam, kMem, 2 * divx_frame_bytes(128, 96) + 0x3D77F, kTs}, "Open 128x96") == 0) {
            Divx311 big{r};
            big.handle = kMem;
            big.width = 96;
            big.height = 64;
            big.other_size = true;
            for (usize i = 0; i < 4; i++)
                big.decode(std::format("128x96: DecodeAu {}", i), packets[i], 0x20, 96, 64, 800 + static_cast<u32>(i));
            big.take("128x96", true);
            r.r("divx311DecClose", {kMem}, "Close 128x96");
        }
        // Type 15: the frame buffers for 1920x1088.
        divx311_param(b, 15, 0, 0, 2);
        std::memset(b + kMem + 0x3D600, 0xA5, 0x100);
        r.r("divx311DecOpen", {kParam, kMem, 2 * 0x320A00 + 0x3D77E, kTs}, "Open type 15 with one byte less");
        if (r.r("divx311DecOpen", {kParam, kMem, 2 * 0x320A00 + 0x3D77F, kTs}, "Open type 15") == 0) {
            Divx311 big{r};
            big.handle = kMem;
            big.width = 96;
            big.height = 64;
            big.other_size = true;
            big.decode("type 15: DecodeAu 0", packets[0], 0x20, 96, 64, 700);
            big.decode("type 15: DecodeAu 1", packets[1], 0x20, 96, 64, 701);
            big.take("type 15", true);
            r.r("divx311DecClose", {kMem}, "Close type 15");
        }
    }
    end_spurs(r);
    return std::move(r.o);
}

// ------------------------------------------------------------------------------------------------- DivX

// A VOP that is not coded (vop_coded 0, an "N-VOP"): a P VOP's header at `time` in a layer whose time increments take
// `bits` bits, as DivX's packed bitstream gives a picture's display slot (D-920). The modulo time base is 0.
std::vector<u8> n_vop(u32 bits, u32 time)
{
    std::vector<u8> out = {0, 0, 1, 0xB6};
    u32 acc = 0, n = 0;
    const auto put = [&](u32 v, u32 count) {
        for (u32 i = count; i-- > 0;) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++n == 8) {
                out.push_back(static_cast<u8>(acc));
                acc = n = 0;
            }
        }
    };
    put(1, 2);        // vop_coding_type: P
    put(0, 1);        // modulo_time_base: no whole second
    put(1, 1);        // marker
    put(time, bits);  // vop_time_increment
    put(1, 1);        // marker
    put(0, 1);        // vop_coded: 0
    put(0, 1);        // next_start_code: a 0, then 1s to the byte
    while (n) put(1, 1);
    return out;
}

// Bits of an MPEG-4 part 2 header, read and written (the streams' VOL and VO headers rewritten below).
struct BitReader {
    const std::vector<u8>& d;
    usize pos = 0; // in bits
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos++) v = (v << 1) | ((pos / 8 < d.size() ? d[pos / 8] >> (7 - pos % 8) : 0) & 1);
        return v;
    }
};
struct BitWriter {
    std::vector<u8> out;
    u32 n = 0;
    void put(u32 v, u32 count)
    {
        for (u32 i = count; i-- > 0;) {
            if (n % 8 == 0) out.push_back(0);
            out.back() |= static_cast<u8>(((v >> i) & 1) << (7 - n % 8));
            n++;
        }
    }
};

// Where the start code `code` (00 00 01 code) begins in `p`, or the size.
usize start_code_at(const std::vector<u8>& p, u8 code, usize from = 0)
{
    for (usize i = from; i + 3 < p.size(); i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == code) return i;
    return p.size();
}

// The first packet of FFmpeg's stream with its video object layer rewritten to a fixed VOP rate of `increment` ticks
// (FFmpeg's encoder writes none), the bits after the field as they were.
std::vector<u8> with_fixed_vop_rate(const std::vector<u8>& packet, u32 increment)
{
    const usize at = start_code_at(packet, 0x20);
    REQUIRE(at < packet.size());
    const usize next = start_code_at(packet, 0xB2, at + 4) < packet.size() ? start_code_at(packet, 0xB2, at + 4)
                                                                           : start_code_at(packet, 0xB6, at + 4);
    const std::vector<u8> vol(packet.begin() + static_cast<std::ptrdiff_t>(at + 4),
        packet.begin() + static_cast<std::ptrdiff_t>(next));
    BitReader r{vol};
    BitWriter w;
    const auto copy = [&](u32 n) { w.put(r.bits(n), n); };
    copy(1 + 8); // random_accessible_vol, video_object_type_indication
    u32 verid = 1;
    const u32 ident = r.bits(1);
    w.put(ident, 1);
    if (ident) {
        verid = r.bits(4);
        w.put(verid, 4);
        copy(3);
    }
    const u32 aspect = r.bits(4);
    w.put(aspect, 4);
    if (aspect == 15) copy(16);
    const u32 control = r.bits(1);
    w.put(control, 1);
    if (control) {
        copy(3);
        const u32 vbv = r.bits(1);
        w.put(vbv, 1);
        if (vbv) copy(79);
    }
    const u32 shape = r.bits(2);
    w.put(shape, 2);
    if (shape == 3 && verid != 1) copy(4);
    copy(1);
    const u32 resolution = r.bits(16);
    w.put(resolution, 16);
    copy(1);
    const u32 fixed = r.bits(1);
    u32 bits = 1;
    while (bits < 16 && (1u << bits) < resolution) bits++;
    if (fixed) r.bits(bits);
    w.put(1, 1);
    w.put(increment, bits);
    while (r.pos < vol.size() * 8) w.put(r.bits(1), 1); // the rest, its stuffing included
    std::vector<u8> out(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(at + 4));
    out.insert(out.end(), w.out.begin(), w.out.end());
    out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(next), packet.end());
    return out;
}

// The first packet with its visual object header replaced by one with a video signal type and colour description.
std::vector<u8> with_colour(const std::vector<u8>& packet, u8 primaries, u8 transfer, u8 matrix)
{
    const usize at = start_code_at(packet, 0xB5);
    REQUIRE(at < packet.size());
    const usize next = start_code_at(packet, 0x00, at + 4) < packet.size() ? start_code_at(packet, 0x00, at + 4)
                                                                           : start_code_at(packet, 0x20, at + 4);
    BitWriter w;
    w.put(1, 1);     // is_visual_object_identifier
    w.put(1, 4);     // visual_object_verid
    w.put(1, 3);     // visual_object_priority
    w.put(1, 4);     // visual_object_type: video
    w.put(1, 1);     // video_signal_type
    w.put(5, 3);     // video_format: unspecified
    w.put(0, 1);     // video_range
    w.put(1, 1);     // colour_description
    w.put(primaries, 8);
    w.put(transfer, 8);
    w.put(matrix, 8);
    w.put(0, 1); // next_start_code: a 0, then 1s to the byte
    while (w.n % 8) w.put(1, 1);
    std::vector<u8> out(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(at + 4));
    out.insert(out.end(), w.out.begin(), w.out.end());
    out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(next), packet.end());
    return out;
}

// The type of the first video object plane in an access unit (I 0, P 1, B 2, S 3; none 4).
u32 vop_type(const std::vector<u8>& au)
{
    const usize at = start_code_at(au, 0xB6);
    return at + 4 < au.size() ? (au[at + 4] >> 6) & 3 : 4;
}

struct Divx {
    Run& r;
    u32 handle = 0, width = 0, height = 0;
    u32 taken = 0;
    u32 at = kStream;
    bool other_size = false; // opened for another size than the access units' (frame buffers: notes)
    // An anchor picture's access unit refused (BUSY) leaves the pictures after it predicted from others: Sony's SPU
    // program decodes them anyway, FFmpeg leaves the B pictures whose times then make no sense. Their frame buffers are
    // notes, from the refused access unit to the next I picture decoded (the access units' words name them).
    bool broken = false;
    std::set<u32> broken_users{};
    bool flushed = false; // pictures taken now are the flush's

    u64 flush(const std::string& key)
    {
        flushed = true;
        return r.r("divxDecFlushPicture", {handle}, key);
    }

    std::string w(u32 v) { return std::format("{:#x}", v); }

    // The core's own state after a call, as a note (opaque, not compared): what its SPU side reported and its tables.
    void note_state(const std::string& key)
    {
        if (!handle || !env_on("PSPOCKET_CORE_STATE")) return;
        r.o.note(key + ": state " + compare::hex(r.b + handle, 0x90));
        r.o.note(key + ": entries " + compare::hex(r.b + handle + 0x380, 0x4C * 4));
        r.o.note(key + ": ring " + compare::hex(r.b + handle + 0x4B0, 0x1C));
        r.o.note(key + ": frames " + compare::hex(r.b + handle + 0x4CC, 0x64));
        r.o.note(key + ": flags " + compare::hex(r.b + handle + 0x520, 0x14));
    }

    u64 query(const std::string& what, u32 type, u32 wd, u32 ht, u32 n, bool null_size = false, bool null_ver = false)
    {
        u8* b = r.b;
        divx311_param(b, type, wd, ht, n);
        std::memset(b + kOut, 0xEE, 16);
        const u64 v = r.r("divxDecQueryAttr", {kParam, null_size ? 0u : kOut, null_ver ? 0u : kOut2}, "QueryAttr " + what);
        r.o.line("QueryAttr " + what + ": the outputs", compare::hex(b + kOut, 16));
        return v;
    }

    // Each picture the core holds: GetNumOfPictures, then GetPicture (its 0x40 bytes, the frame's address as an offset)
    // and the frame buffer; ReleasePicture after each when `release`.
    void take(const std::string& when, bool release, u32 limit = ~0u)
    {
        u8* b = r.b;
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("divxDecGetNumOfPictures", {handle, kOut}, when + ": GetNumOfPictures");
        r.o.line(when + ": pictures", w(ps_read32(b, kOut)));
        for (u32 k = 0; k < limit; k++) {
            std::memset(b + kPic, 0xEE, 0x48);
            const std::string key = std::format("{}: picture {}", when, taken);
            const u64 got = r.r("divxDecGetPicture", {handle, kPic}, key + ": GetPicture");
            if (static_cast<u32>(got) != 0 && static_cast<u32>(got) != 0x80615540) return;
            taken++;
            std::vector<u8> words(b + kPic, b + kPic + 0x48);
            const u32 frame = ps_read32(b, kPic);
            std::memset(words.data(), 0, 4);
            r.o.line(key + ": the picture's frame", in_core(frame));
            r.o.line(key + ": the picture", compare::hex(words));
            if (frame >= kMem && frame + divx_frame_bytes(width, height) <= kMem + kMemSize) {
                if (other_size || broken_users.contains(ps_read32(b, kPic + 8))) {
                    u32 sum = 0;
                    for (u32 i = 0; i < divx_frame_bytes(width, height); i++) sum += b[frame + i];
                    r.o.note(std::format("{}: its frame buffer's sum {:#x}", key, sum));
                } else if (flushed) {
                    // The picture the flush made ready: Sony's SPU program pads its chroma edges as it answers the
                    // flush, so how much of them the game sees padded is the threads' timing (a note); the rest whole.
                    const u32 strips = (width + 15) / 16 + 2, lines = ((height + 15) & ~15u) + 32;
                    const u32 clines = ((height + 15) & ~15u) / 2 + 16, luma = strips * lines * 16;
                    r.o.bytes(key + ": its frame buffer's luma", b + frame, luma, Rule::within(1));
                    std::vector<u8> chroma;
                    u32 sum = 0;
                    for (u32 st = 0; st < strips; st++)
                        for (u32 line = 0; line < clines; line++)
                            for (u32 c = 0; c < 16; c++) {
                                const u8 v = b[frame + luma + (st * clines + line) * 16 + c];
                                const u32 x = st * 8 + c % 8;
                                if (st > 0 && x - 8 < width / 2 && line >= 8 && line - 8 < height / 2) chroma.push_back(v);
                                else sum += v;
                            }
                    r.o.bytes(key + ": its frame buffer's chroma", chroma, Rule::within(1));
                    r.o.note(std::format("{}: its chroma edges' sum {:#x}", key, sum));
                } else {
                    r.o.bytes(key + ": its frame buffer", b + frame, divx_frame_bytes(width, height), Rule::within(1));
                }
            }
            if (release) r.r("divxDecReleasePicture", {handle}, key + ": ReleasePicture");
        }
    }

    // DecodeAu(handle, au {address, size, four words of the caller's}, mode, status).
    u64 decode(const std::string& key, const std::vector<u8>& au, u32 mode, u32 user, bool with_status = true)
    {
        u8* b = r.b;
        if (at + au.size() > kStream + 0x80000) at = kStream;
        std::memcpy(b + at, au.data(), au.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, 0x1000 + user);
        ps_write32(b, kAu + 12, 0x2000 + user);
        ps_write32(b, kAu + 16, 0x3000 + user);
        ps_write32(b, kAu + 20, 0x4000 + user);
        ps_write32(b, kAu + 24, 0x5000 + user); // past the four words: not the core's
        at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
        ps_write32(b, kStatus, 0xEEEEEEEE);
        flushed = false;
        const u64 v = r.r("divxDecDecodeAu", {handle, kAu, mode, with_status ? kStatus : 0u}, key);
        r.o.line(key + ": status", w(ps_read32(b, kStatus)));
        note_state(key);
        // (r3's upper word is the module's own: Sony's core zero-extends its answers.)
        const u32 answer32 = static_cast<u32>(v), type = vop_type(au);
        if (answer32 == 0x80615510 && type <= 1) broken = true;
        else if (answer32 == 0 && type == 0) broken = false;
        if (broken && answer32 == 0) broken_users.insert(0x1000 + user);
        return v;
    }

    // A whole stream, each picture taken and released as soon as there is one; then the flush. `later`: the mode from
    // the stream's middle on.
    void stream(const std::string& what, const std::vector<std::vector<u8>>& aus, u32 mode, u32 user0, u32 later = 0)
    {
        for (usize i = 0; i < aus.size(); i++) {
            const u32 m = later && i >= aus.size() / 2 ? later : mode;
            decode(std::format("{}: DecodeAu {}", what, i), aus[i], m, user0 + static_cast<u32>(i));
            take(std::format("{}: after DecodeAu {}", what, i), true);
        }
        flush(what + ": FlushPicture");
        take(what + ": after FlushPicture", true);
    }
};

// DivX's packed bitstream from FFmpeg's coded order (I P B B P B B ...): each P VOP with the first B VOP after it in one
// access unit, the other B VOPs alone, and after each such run an N-VOP giving the P picture's display slot.
std::vector<std::vector<u8>> packed(const std::vector<std::vector<u8>>& coded, u32 bits)
{
    std::vector<std::vector<u8>> out;
    const auto type_of = [](const std::vector<u8>& p) {
        for (usize i = 0; i + 4 < p.size(); i++)
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0xB6) return (p[i + 4] >> 6) & 3;
        return 0;
    };
    for (usize i = 0; i < coded.size();) {
        const int t = type_of(coded[i]);
        if (t == 1 && i + 1 < coded.size() && type_of(coded[i + 1]) == 2) {
            std::vector<u8> pair = coded[i];
            pair.insert(pair.end(), coded[i + 1].begin(), coded[i + 1].end());
            out.push_back(pair);
            usize j = i + 2;
            while (j < coded.size() && type_of(coded[j]) == 2) out.push_back(coded[j++]);
            out.push_back(n_vop(bits, static_cast<u32>(i)));
            i = j;
        } else {
            out.push_back(coded[i++]);
        }
    }
    return out;
}

Observed divx(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libdivxdec");
    u8* b = r.b;
    make_spurs(r);
    Divx d{r};
    // QueryAttr: the types (levels), sizes, buffer counts and the refusals.
    for (u32 type = 9; type <= 15; type++)
        for (u32 n = 1; n <= 5; n++) d.query(std::format("type {}, {} buffers", type, n), type, 0, 0, n);
    for (const auto& [wd, ht] : {std::pair{96u, 64u}, {16u, 16u}, {15u, 16u}, {16u, 15u}, {1920u, 1088u}, {1921u, 64u},
             {64u, 1089u}, {720u, 576u}, {0x580u, 0x480u}, {0x580u, 0x47Fu}, {0u, 0u}, {80u, 48u}})
        d.query(std::format("{}x{}", wd, ht), 0, wd, ht, 3);
    d.query("96x64, no size", 0, 96, 64, 3, true);
    d.query("96x64, no version", 0, 96, 64, 3, false, true);
    r.r("divxDecQueryAttr", {0, kOut, kOut2}, "QueryAttr with no parameters");

    const auto stream = [](int w, int h, int frames, int gop, std::function<void(AVCodecContext*)> setup = {}) {
        return encode(AV_CODEC_ID_MPEG4, w, h, frames, gop, setup);
    };
    const std::vector<std::vector<u8>> plain = stream(96, 64, 10, 4);
    const std::vector<std::vector<u8>> with_b = stream(96, 64, 13, 6, [](AVCodecContext* av) { av->max_b_frames = 2; });
    // Open's memory: its own, its SPU tasks' and the frame buffers; one byte less refused.
    for (const auto& [type, n] : {std::pair{0u, 2u}, {0u, 3u}, {0u, 4u}, {10u, 2u}, {14u, 3u}}) {
        divx311_param(b, type, 96, 64, n);
        const u32 frame = type ? (type == 10 ? 0xD680u : 0x320A00u) : divx_frame_bytes(96, 64);
        const u32 need = 0x7684C + n * frame;
        std::memset(b + kMem, 0xA5, 0x1000);
        r.r("divxDecOpen", {kParam, kMem, need - 1, kTs}, std::format("Open type {}, {} buffers, one byte less", type, n));
        if (r.r("divxDecOpen", {kParam, kMem, need, kTs}, std::format("Open type {}, {} buffers", type, n)) == 0)
            r.r("divxDecClose", {kMem}, std::format("Close type {}, {} buffers", type, n));
    }
    divx311_param(b, 0, 96, 64, 3);
    ps_write32(b, kOut, 0);
    r.r("divxDecQueryAttr", {kParam, kOut, kOut2}, "QueryAttr for the decoder");
    const u32 need = ps_read32(b, kOut);
    std::memset(b + kData + 0x7000, 0, 0x1000);
    r.r("divxDecOpen", {kParam, kMem, need, kData + 0x7000}, "Open with a taskset that is none");
    r.r("divxDecOpen", {0, kMem, need, kTs}, "Open with no parameters");
    r.r("divxDecOpen", {kParam, 0, need, kTs}, "Open with no memory");
    r.r("divxDecOpen", {kParam, kMem, need, 0}, "Open with no taskset");
    divx311_param(b, 0, 96, 64, 5);
    r.r("divxDecOpen", {kParam, kMem, need, kTs}, "Open with 5 buffers");
    divx311_param(b, 0, 96, 64, 1);
    r.r("divxDecOpen", {kParam, kMem, need, kTs}, "Open with 1 buffer");
    divx311_param(b, 0, 0x580, 0x47F, 3);
    r.r("divxDecOpen", {kParam, kMem, 0x2000000, kTs}, "Open 1408x1151");
    divx311_param(b, 0, 96, 64, 3);
    std::memset(b + kMem, 0xA5, need + 0x10000);
    d.width = 96;
    d.height = 64;
    if (r.r("divxDecOpen", {kParam, kMem, need, kTs}, "Open") == 0) {
        d.handle = kMem;
        r.r("divxDecOpen", {kParam, kMem, need, kTs}, "Open again on the same memory");
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("divxDecGetNumOfPictures", {d.handle, kOut}, "GetNumOfPictures before a sequence");
        r.o.line("pictures before a sequence", std::format("{:#x}", ps_read32(b, kOut)));
        r.r("divxDecGetPicture", {d.handle, kPic}, "GetPicture before a sequence");
        r.r("divxDecReleasePicture", {d.handle}, "ReleasePicture before a sequence");
        d.flush("FlushPicture before a sequence");
        // (libvdec resets its core before decoding: Sony's SPU program reports what it holds from before otherwise.)
        r.r("divxDecReset", {d.handle, 0}, "Reset");
        d.stream("I and P", plain, 0x20, 0);
        d.flush("FlushPicture again");
        d.decode("DecodeAu after FlushPicture", plain[0], 0x20, 50);
        d.take("after DecodeAu after FlushPicture", true);
        r.r("divxDecReset", {d.handle, 0}, "Reset for B pictures");
        d.stream("I, P and B", with_b, 0x20, 100);
        // Pictures held: how many access units the core takes before it has no frame buffer.
        r.r("divxDecReset", {d.handle, 0}, "Reset for holding");
        for (usize i = 0; i < 6; i++)
            d.decode(std::format("held: DecodeAu {}", i), plain[i], 0x20, 200 + static_cast<u32>(i));
        d.take("held", false, 1);
        r.r("divxDecReleasePicture", {d.handle}, "held: ReleasePicture");
        d.decode("held: DecodeAu again", plain[3], 0x20, 210);
        d.take("held, then", true);
        r.r("divxDecReset", {d.handle, 0}, "Reset for holding B");
        for (usize i = 0; i < 7; i++)
            d.decode(std::format("held B: DecodeAu {}", i), with_b[i], 0x20, 220 + static_cast<u32>(i));
        d.take("held B", true);
        // The decode modes over a stream with B pictures, then the checks of DecodeAu.
        r.r("divxDecReset", {d.handle, 1}, "Reset 1");
        d.stream("mode 0x21", with_b, 0x21, 300);
        r.r("divxDecReset", {d.handle, 2}, "Reset 2");
        d.stream("mode 0x22", with_b, 0x22, 320);
        r.r("divxDecReset", {d.handle, 0}, "Reset 2b");
        d.stream("modes mixed", with_b, 0x20, 340, 0x22);
        d.decode("mode 0x23", plain[0], 0x23, 360);
        d.decode("mode 0x1f", plain[0], 0x1F, 361);
        r.r("divxDecReset", {d.handle, 0}, "Reset 3");
        d.decode("a P picture first after Reset", plain[1], 0x20, 370);
        d.take("P first", true);
        d.decode("a B picture first after Reset", with_b[2], 0x20, 371);
        d.take("B first", true);
        ps_write32(b, kAu + 4, 0);
        r.r("divxDecDecodeAu", {d.handle, kAu, 0x20, kStatus}, "DecodeAu with size 0");
        ps_write32(b, kAu + 0, 0);
        ps_write32(b, kAu + 4, 0x100);
        r.r("divxDecDecodeAu", {d.handle, kAu, 0x20, kStatus}, "DecodeAu with address 0");
        r.r("divxDecDecodeAu", {d.handle, 0, 0x20, kStatus}, "DecodeAu with no access unit");
        d.decode("DecodeAu with no status", plain[0], 0x20, 372, false);
        d.take("no status", true);
        // GetPicture with none ready takes the head entry off: what the decoder does after it.
        r.r("divxDecReset", {d.handle, 0}, "Reset 4");
        d.decode("quirk: DecodeAu 0", plain[0], 0x20, 400);
        r.r("divxDecGetPicture", {d.handle, kPic}, "quirk: GetPicture with none ready");
        d.decode("quirk: DecodeAu 1", plain[1], 0x20, 401);
        d.take("quirk: after DecodeAu 1", true);
        d.decode("quirk: DecodeAu 2", plain[2], 0x20, 402);
        d.take("quirk: after DecodeAu 2", true);
        r.r("divxDecGetPicture", {d.handle, kPic}, "quirk: GetPicture with none ready again");
        d.flush("quirk: FlushPicture");
        d.take("quirk: after FlushPicture", true);
        // Pictures held two at a time, released together; one release too many.
        r.r("divxDecReset", {d.handle, 0}, "Reset 5");
        for (usize i = 0; i < 8; i++) {
            d.decode(std::format("pairs: DecodeAu {}", i), with_b[i], 0x20, 500 + static_cast<u32>(i));
            d.take(std::format("pairs: after DecodeAu {}", i), false);
            if (i % 2) {
                r.r("divxDecReleasePicture", {d.handle}, std::format("pairs: ReleasePicture {} a", i));
                r.r("divxDecReleasePicture", {d.handle}, std::format("pairs: ReleasePicture {} b", i));
                r.r("divxDecReleasePicture", {d.handle}, std::format("pairs: ReleasePicture {} c", i));
            }
        }
        // DivX's packed bitstream: P and B VOPs in one access unit, an N-VOP after; N-VOPs alone.
        r.r("divxDecReset", {d.handle, 0}, "Reset 6");
        d.stream("packed", packed(with_b, 5), 0x20, 600);
        r.r("divxDecReset", {d.handle, 0}, "Reset 7");
        d.decode("an N-VOP first", n_vop(5, 3), 0x20, 700);
        d.take("N-VOP first", true);
        d.decode("N-VOPs: DecodeAu 0", plain[0], 0x20, 701);
        d.decode("N-VOPs: DecodeAu 1", n_vop(5, 1), 0x20, 702);
        d.take("N-VOPs: after DecodeAu 1", true);
        d.decode("N-VOPs: DecodeAu 2", plain[1], 0x20, 703);
        d.take("N-VOPs: after DecodeAu 2", true);
        d.decode("N-VOPs: DecodeAu 3", n_vop(5, 3), 0x20, 704);
        d.take("N-VOPs: after DecodeAu 3", true);
        d.flush("N-VOPs: FlushPicture");
        d.take("N-VOPs: after FlushPicture", true);
        // Streams of other kinds: what the core reports of their layer and pictures.
        struct Kind {
            const char* name;
            std::function<void(AVCodecContext*)> setup;
            int edit = 0; // 1: a fixed VOP rate, 2: a colour description, 3: 80x48
        };
        const Kind kinds[] = {
            {"SAR 12:11", [](AVCodecContext* av) { av->sample_aspect_ratio = {12, 11}; }},
            {"SAR 64:45", [](AVCodecContext* av) { av->sample_aspect_ratio = {64, 45}; }},
            {"qpel", [](AVCodecContext* av) { av->flags |= AV_CODEC_FLAG_QPEL; }},
            {"interlaced", [](AVCodecContext* av) { av->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME; }},
            {"30000/1001", [](AVCodecContext* av) {
                 av->time_base = {1001, 30000};
                 av->framerate = {30000, 1001};
             }},
            {"5 a second", [](AVCodecContext* av) {
                 av->time_base = {1, 5};
                 av->framerate = {5, 1};
             }},
            {"no VOL control", [](AVCodecContext* av) { av->workaround_bugs |= FF_BUG_MS; }},
            {"a fixed VOP rate", {}, 1},
            {"a colour description", {}, 2},
            {"80x48", {}, 3},
        };
        u32 k = 0;
        for (const Kind& kind : kinds) {
            std::vector<std::vector<u8>> aus = stream(kind.edit == 3 ? 80 : 96, kind.edit == 3 ? 48 : 64, 7, 3, kind.setup);
            if (kind.edit == 1) aus[0] = with_fixed_vop_rate(aus[0], 1);
            if (kind.edit == 2) aus[0] = with_colour(aus[0], 1, 6, 5);
            r.r("divxDecReset", {d.handle, 0}, std::format("{}: Reset", kind.name));
            if (kind.edit == 3) {
                Divx small{r};
                small.handle = d.handle;
                small.width = 80;
                small.height = 48;
                small.taken = d.taken;
                small.stream(kind.name, aus, 0x20, 800 + 10 * k);
                d.taken = small.taken;
            } else {
                d.stream(kind.name, aus, 0x20, 800 + 10 * k);
            }
            k++;
        }
        // A flush right after Reset (nothing decoded).
        r.r("divxDecReset", {d.handle, 0}, "Reset 8");
        d.flush("FlushPicture after Reset");
        d.take("after FlushPicture after Reset", true);
        r.r("divxDecClose", {d.handle}, "Close");
        r.r("divxDecClose", {d.handle}, "Close again");
        r.r("divxDecGetNumOfPictures", {d.handle, kOut}, "GetNumOfPictures after Close");
        r.r("divxDecClose", {0}, "Close with no handle");
    }
    // Two frame buffers: B pictures need three.
    divx311_param(b, 0, 96, 64, 2);
    ps_write32(b, kOut, 0);
    r.r("divxDecQueryAttr", {kParam, kOut, kOut2}, "QueryAttr for two buffers");
    if (r.r("divxDecOpen", {kParam, kMem, ps_read32(b, kOut), kTs}, "Open two buffers") == 0) {
        Divx two{r};
        two.handle = kMem;
        two.width = 96;
        two.height = 64;
        r.r("divxDecReset", {kMem, 0}, "two buffers: Reset");
        two.stream("two buffers", plain, 0x20, 900);
        r.r("divxDecReset", {kMem, 0}, "two buffers: Reset again");
        for (usize i = 0; i < 4; i++)
            two.decode(std::format("two buffers, B: DecodeAu {}", i), with_b[i], 0x20, 920 + static_cast<u32>(i));
        two.take("two buffers, B", true);
        r.r("divxDecClose", {kMem}, "Close two buffers");
    }
    // Four frame buffers, opened by type (the largest frames of its level), the access units smaller.
    divx311_param(b, 10, 0, 0, 4);
    ps_write32(b, kOut, 0);
    r.r("divxDecQueryAttr", {kParam, kOut, kOut2}, "QueryAttr type 10, 4 buffers");
    if (r.r("divxDecOpen", {kParam, kMem, ps_read32(b, kOut), kTs}, "Open type 10, 4 buffers") == 0) {
        Divx big{r};
        big.handle = kMem;
        big.width = 96;
        big.height = 64;
        r.r("divxDecReset", {kMem, 0}, "type 10: Reset");
        big.stream("type 10", with_b, 0x20, 950);
        r.r("divxDecClose", {kMem}, "Close type 10");
    }
    end_spurs(r);
    return std::move(r.o);
}

} // namespace

TEST_CASE("The DivX 3.11 core (libdivx311dec): pspocket's own, answering as Sony's did (D-920)", "[hle][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3)) SKIP("this FFmpeg has no msmpeg4v3 encoder (build-ffmpeg.sh)");
    check_names("libdivx311dec", {"divx311DecQueryAttr", "divx311DecOpen", "divx311DecClose", "divx311DecDecodeAu",
                                     "divx311DecGetNumOfPictures", "divx311DecGetPicture", "divx311DecReleasePicture",
                                     "divx311DecFlushPicture", "divx311DecReset"},
        false);
    Setup s;
    s.mode = Mode::Self;
    const Observed o = divx311(s, false);
    // Sony's answers (test_vdec_cores_compare.cpp's comparison, firmware 4.92).
    CHECK(o.value("QueryAttr 96x64: the outputs") == "hex:00056800eeeeeeee00000103eeeeeeee");
    CHECK(o.value("QueryAttr type 15: the outputs") == "hex:0068ec00eeeeeeee00000103eeeeeeee");
    CHECK(o.value("QueryAttr 96x64, 1 buffer") == "0x80615502");
    CHECK(o.value("Open again on the same memory") == "0x80615502");
    CHECK(o.value("GetPicture before a sequence") == "0x80615503");
    CHECK(o.value("after DecodeAu 0: pictures") == "0x0"); // a picture is shown when the next is decoded
    CHECK(o.value("after DecodeAu 1: picture 0: the picture's frame") == "the core's memory +0x3d700");
    CHECK(o.value("after DecodeAu 2: picture 1: the picture's frame") == "the core's memory +0x41f00");
    CHECK(o.value("after DecodeAu 1: picture 0: the picture") ==
        "hex:0000000000004800000010000000200000003000000040000060004000000000000000300000000000eeeeeeeeeeeeee");
    CHECK(o.value("held: DecodeAu 2") == "0x80615510"); // two frame buffers: one held, one the reference
    CHECK(o.value("a P picture first after Reset") == "0x80615520");
    CHECK(o.value("a P picture first after Reset: status") == "0x40020000");
    CHECK(o.value("modes: picture 10: the picture's frame") == "0x0"); // a skipped picture shows no frame
    CHECK(o.value("mode 0x23") == "0x80615502");
    CHECK(o.value("Close again") == "0x80615502");
    const Data* first = o.find_data("after DecodeAu 1: picture 0: its frame buffer");
    REQUIRE(first);
    CHECK(first->bytes[16 * 16] == 30); // the picture's first sample, under 16 lines of edge
}

TEST_CASE("The DivX 3.11 core (libdivx311dec) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3)) SKIP("this FFmpeg has no msmpeg4v3 encoder (build-ffmpeg.sh)");
    const Setup s = setup();
    run(s, "libdivx311dec against Sony's", [&](bool sony) { return divx311(s, sony); }, [](const Observed&) {});
}

TEST_CASE("The DivX core (libdivxdec): pspocket's own, answering as Sony's did (D-920)", "[hle][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP("this FFmpeg has no mpeg4 encoder (build-ffmpeg.sh)");
    check_names("libdivxdec", {"divxDecQueryAttr", "divxDecOpen", "divxDecClose", "divxDecDecodeAu",
                                  "divxDecGetNumOfPictures", "divxDecGetPicture", "divxDecReleasePicture",
                                  "divxDecFlushPicture", "divxDecReset"},
        false);
    Setup s;
    s.mode = Mode::Self;
    const Observed o = divx(s, false);
    // Sony's answers (test_vdec_cores_compare.cpp's comparison, firmware 4.92).
    CHECK(o.value("QueryAttr type 10, 2 buffers: the outputs") == "hex:000b9790eeeeeeee00030806eeeeeeee");
    CHECK(o.value("Open type 0, 2 buffers, one byte less") == "0x80615502");
    CHECK(o.value("Open type 0, 2 buffers") == "0x0");
    CHECK(o.value("Open with 5 buffers") == "0x80615502");
    CHECK(o.value("I and P: after DecodeAu 1: picture 0: the picture's frame") == "the core's memory +0x76680");
    CHECK(o.value("I and P: after DecodeAu 1: picture 0: the picture") ==
        "hex:000000000000480000001000000020000000300000004000010000ee001901ee000000600040eeee000000000000003000000000"
        "00ee00000000000000000000eeeeeeeeeeeeeeee");
    // B pictures: the second of two is refused while the first, shown, is not taken (GetPicture took the waiting P
    // picture's entry off instead).
    CHECK(o.value("I, P and B: DecodeAu 3") == "0x80615510");
    CHECK(o.value("a P picture first after Reset") == "0x80615520");
    CHECK(o.value("a P picture first after Reset: status") == "0x20");
    CHECK(o.value("packed: after DecodeAu 1: pictures") == "0x2"); // the I picture, and the packed B picture
    // An N-VOP's picture: the frame before it again, marked 0x32.
    CHECK(o.value("N-VOPs: after DecodeAu 2: picture 64: the picture") ==
        "hex:0000000000004800000012be000022be000032be000042be010000ee001901ee000000600040eeee000000010000003200000000"
        "00ee00010000000000000000eeeeeeeeeeeeeeee");
    CHECK(o.value("SAR 64:45: after DecodeAu 1: picture 74: the picture").starts_with(
        "hex:00000000000048000000132a0000232a0000332a0000432a0f402dee"));
    CHECK(o.value("a fixed VOP rate: after DecodeAu 1: picture 116: the picture").starts_with(
        "hex:000000000000480000001366000023660000336600004366010000ee001901ee0001"));
    CHECK(o.value("80x48: after DecodeAu 1: picture 130: the picture").starts_with("hex:000000000000348000"));
}

TEST_CASE("The DivX core (libdivxdec) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP("this FFmpeg has no mpeg4 encoder (build-ffmpeg.sh)");
    const Setup s = setup();
    run(s, "libdivxdec against Sony's", [&](bool sony) { return divx(s, sony); }, [](const Observed&) {});
}



#endif
