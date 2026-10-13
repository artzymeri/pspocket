// The video codec cores' own APIs against Sony's modules (D-920): each core library called directly, as Sony's libvdec
// (or a game) calls it, with the same synthetic streams through pspocket's core and through Sony's module (the user's
// installed system software), each core in a process of its own per half. Sony's cores decode on SPURS: each half
// makes a SPURS instance and a taskset (pspocket's cellSpurs) and gives the taskset to the core's open. Compared: every
// answer and output structure, the frame buffers' places (as offsets into the core's memory) and their contents (the
// IDCT may round one step apart, as D-912 accepts for DivX and MPEG-4; VC-1 exactly), the pictures' order and the
// errors. The cores' own memory (their state, SPU tasks' areas) is opaque: not compared. Skipped without the firmware
// (compare_util.hpp) and without FFmpeg's encoders. No game data. libdivx311dec, libdivxdec and libsmvd4 so far.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "compare_util.hpp"
#include "mpeg2_writer.hpp"
#include "vdec_streams.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavutil/opt.h>
}
#endif

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

// The first packet of FFmpeg's stream with VBV parameters in its video object layer (FFmpeg's encoder writes none):
// bit rate 0x12345 (halves 0x2 and 0x2345), buffer size 0x1A3 (0x34 and 3), occupancy 0x5678 (0x0 and 0x5678).
std::vector<u8> with_vbv(const std::vector<u8>& packet)
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
    copy(1 + 8);
    const u32 ident = r.bits(1);
    w.put(ident, 1);
    if (ident) copy(4 + 3);
    const u32 aspect = r.bits(4);
    w.put(aspect, 4);
    if (aspect == 15) copy(16);
    const u32 control = r.bits(1);
    REQUIRE(control == 1);
    w.put(1, 1);
    copy(3); // chroma_format, low_delay
    REQUIRE(r.bits(1) == 0);
    w.put(1, 1); // vbv_parameters
    w.put(0x2, 15);
    w.put(1, 1);
    w.put(0x2345, 15);
    w.put(1, 1);
    w.put(0x34, 15);
    w.put(1, 1);
    w.put(3, 3);
    w.put(0, 11);
    w.put(1, 1);
    w.put(0x5678, 15);
    w.put(1, 1);
    // The rest without its stuffing (a 0, then 1s to the byte), stuffed again.
    usize end = vol.size() * 8;
    while (end > 0 && ((vol[(end - 1) / 8] >> (7 - (end - 1) % 8)) & 1)) end--;
    end--;
    while (r.pos < end) w.put(r.bits(1), 1);
    w.put(0, 1);
    while (w.n % 8) w.put(1, 1);
    std::vector<u8> out(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(at + 4));
    out.insert(out.end(), w.out.begin(), w.out.end());
    out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(next), packet.end());
    return out;
}

// The first packet with its video object layer rewritten without its layer identifier and control parameters (as
// FFmpeg's FF_BUG_MS writes it), the bits after them as they were.
std::vector<u8> with_plain_vol(const std::vector<u8>& packet)
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
    copy(1 + 8);
    REQUIRE(r.bits(1) == 1);
    r.bits(4 + 3);
    w.put(0, 1); // is_object_layer_identifier
    const u32 aspect = r.bits(4);
    w.put(aspect, 4);
    if (aspect == 15) copy(16);
    REQUIRE(r.bits(1) == 1);
    r.bits(3);
    REQUIRE(r.bits(1) == 0);
    w.put(0, 1); // vol_control_parameters
    usize end = vol.size() * 8;
    while (end > 0 && ((vol[(end - 1) / 8] >> (7 - (end - 1) % 8)) & 1)) end--;
    end--;
    while (r.pos < end) w.put(r.bits(1), 1);
    w.put(0, 1);
    while (w.n % 8) w.put(1, 1);
    std::vector<u8> out(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(at + 4));
    out.insert(out.end(), w.out.begin(), w.out.end());
    out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(next), packet.end());
    return out;
}

// A video object layer's fields as make_vol writes them (FFmpeg's stream's coding fields: 25 ticks a second, 96x64,
// progressive, OBMC off, 8 bits, H.263 quantisation, no estimation, no resync markers, no partitions, no scalability).
struct VolFields {
    u32 random_access = 0, type = 1, ident = 1, verid = 1, priority = 1, aspect = 1, par_w = 0, par_h = 0;
    u32 control = 1, chroma = 1, low_delay = 1, vbv = 0;
    u32 width = 96, height = 64;
};

// The first packet with its visual object header and video object layer replaced: the visual object's identifier
// (`vo_ident`, verid 1 and priority 1 when present) and a layer written from `f`.
std::vector<u8> with_vol(const std::vector<u8>& packet, const VolFields& f, u32 vo_ident = 1)
{
    const usize vo = start_code_at(packet, 0xB5), vo_start = start_code_at(packet, 0x00, vo + 4);
    const usize at = start_code_at(packet, 0x20);
    REQUIRE((vo < packet.size() && vo_start < packet.size() && at < packet.size()));
    const usize next = start_code_at(packet, 0xB2, at + 4) < packet.size() ? start_code_at(packet, 0xB2, at + 4)
                                                                           : start_code_at(packet, 0xB6, at + 4);
    BitWriter o;
    o.put(vo_ident, 1);
    if (vo_ident) {
        o.put(1, 4);
        o.put(1, 3);
    }
    o.put(1, 4); // video object
    o.put(0, 1); // no video signal type
    o.put(0, 1);
    while (o.n % 8) o.put(1, 1);
    BitWriter w;
    w.put(f.random_access, 1);
    w.put(f.type, 8);
    w.put(f.ident, 1);
    if (f.ident) {
        w.put(f.verid, 4);
        w.put(f.priority, 3);
    }
    w.put(f.aspect, 4);
    if (f.aspect == 15) {
        w.put(f.par_w, 8);
        w.put(f.par_h, 8);
    }
    w.put(f.control, 1);
    if (f.control) {
        w.put(f.chroma, 2);
        w.put(f.low_delay, 1);
        w.put(f.vbv, 1);
        if (f.vbv) {
            for (const auto& [v, n] : {std::pair{1u, 15u}, {1u, 1u}, {2u, 15u}, {1u, 1u}, {3u, 15u}, {1u, 1u}, {4u, 3u},
                     {5u, 11u}, {1u, 1u}, {6u, 15u}, {1u, 1u}})
                w.put(v, n);
        }
    }
    const u32 verid = f.ident ? f.verid : 1;
    w.put(0, 2);  // rectangular
    w.put(1, 1);
    w.put(25, 16);
    w.put(1, 1);
    w.put(0, 1);  // no fixed VOP rate
    w.put(1, 1);
    w.put(f.width, 13);
    w.put(1, 1);
    w.put(f.height, 13);
    w.put(1, 1);
    w.put(0, 1);  // progressive
    w.put(1, 1);  // obmc_disable
    w.put(0, verid == 1 ? 1 : 2); // sprite_enable
    w.put(0, 1);  // not_8_bit
    w.put(0, 1);  // quant_type
    if (verid != 1) w.put(0, 1); // quarter_sample
    w.put(1, 1);  // complexity_estimation_disable
    w.put(1, 1);  // resync_marker_disable
    w.put(0, 1);  // data_partitioned
    if (verid != 1) {
        w.put(0, 1); // newpred_enable
        w.put(0, 1); // reduced_resolution_vop_enable
    }
    w.put(0, 1); // scalability
    w.put(0, 1);
    while (w.n % 8) w.put(1, 1);
    std::vector<u8> out(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(vo + 4));
    out.insert(out.end(), o.out.begin(), o.out.end());
    out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(vo_start), packet.begin() + static_cast<std::ptrdiff_t>(at + 4));
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

// ------------------------------------------------------------------------------------------------- MPEG-4

// The MPEG-4 calls' other arguments: SetAuxData's 16 bytes, GetSideInfoPic's and GetAuxData's two outputs and count,
// GetSideInfoSeq's output.
constexpr u32 kAux = kArg + 0x200, kSide = kArg + 0x300, kAuxOut = kArg + 0x380, kSeq = kArg + 0x400,
              kCount = kArg + 0x480;

// smvd4CreateInstance2's parameters: the type (4: the memory for a level, 5: for a size, words 8 and 9), word 1 and the
// taskset (word 7).
void smvd4_param(u8* b, u32 type, u32 taskset, u32 w = 0, u32 h = 0, u32 word1 = 0)
{
    std::memset(b + kParam, 0, 0x28);
    ps_write32(b, kParam + 0x00, type);
    ps_write32(b, kParam + 0x04, word1);
    ps_write32(b, kParam + 0x1C, taskset);
    ps_write32(b, kParam + 0x20, w);
    ps_write32(b, kParam + 0x24, h);
}

// libsmvd4's frame buffer (strips with 16 lines and samples of edge; the luma's lines to 32 for the allocation).
u32 smvd4_frame_bytes(u32 w, u32 h) { return (((h + 31) & ~31u) + 32) * (((w + 15) & ~15u) + 32) * 3 / 2 + 0x80; }

// The VOP an access unit begins with, for the test's bookkeeping: its type (I 0, P 1, B 2, S 3; none 4) and whether it
// is coded (the test's streams have 25 ticks a second: 5 bits of time increment).
struct VopKind {
    u32 type = 4;
    bool coded = true;
};
VopKind vop_kind(const std::vector<u8>& au)
{
    const usize at = start_code_at(au, 0xB6);
    if (at + 4 >= au.size()) return {};
    BitReader br{au, (at + 4) * 8};
    VopKind k;
    k.type = br.bits(2);
    while (br.bits(1)) {
    }
    br.bits(1 + 5 + 1);
    k.coded = br.bits(1) != 0;
    return k;
}

struct Smvd4 {
    Run& r;
    u32 handle = 0, width = 0, height = 0;
    u32 taken = 0;
    u32 at = kStream;
    // Pictures pspocket does not reproduce (each is a note, its frame's sum): a VOP damaged on purpose (FFmpeg conceals
    // what is missing otherwise than Sony's program) and the P pictures after it, until the next I picture. A picture
    // whose frame Sony's program does not write (B, not coded, interlaced) is a note when its slot's frame was one.
    bool broken = false;
    bool interlaced = false; // the stream's VOPs are not decoded (an interlaced layer)
    std::set<u32> broken_users{};
    std::set<u32> stale_users{};
    std::map<u32, bool> broken_slot{}; // by the picture's luma address
    std::deque<u32> pending{};         // the users of the pictures decoded and not taken, in decoding order
    // A P picture with no anchor is predicted from the third slot's frame; decoded into that slot itself, Sony's
    // program overwrites its reference as it decodes, so a few samples come out up to 2 steps off pspocket's (which
    // predicts from the whole old frame), and so do the pictures predicted from it: those compare within 2 steps.
    bool no_anchor = true, chain_approx = false;
    std::set<u32> anchorless_users{};
    std::map<u32, bool> approx_slot{};
    u32 slot2_luma() const { return kMem + 0x14A80 + 2 * smvd4_frame_bytes(width, height) + (height + 48) * 16; }

    std::string w(u32 v) { return std::format("{:#x}", v); }

    // The core's own state after a call, as a note (opaque, not compared): its sequence information, its words, its
    // picture queue and the pictures' information.
    void note_state(const std::string& key)
    {
        if (!handle || !env_on("PSPOCKET_CORE_STATE")) return;
        u8* b = r.b;
        r.o.note(key + ": seq " + compare::hex(b + handle + 0x80, 0x34));
        r.o.note(key + ": words " + compare::hex(b + handle + 0x100, 0x6C));
        r.o.note(key + ": queue " + compare::hex(b + handle + 0x680, 4) + " " + compare::hex(b + handle + 0x700, 4) +
                 " " + compare::hex(b + handle + 0x780, 0x10));
        const u32 entries = ps_read32(b, handle + 0x600);
        if (entries >= kMem && entries < kMem + kMemSize) {
            std::string e;
            for (u32 k = 0; k < 8; k++) e += compare::hex(b + entries + k * 0x80, 8) + " ";
            r.o.note(key + ": entries " + e + compare::hex(b + entries + 0x400, 8));
        }
        for (u32 k = 0; k < 3; k++)
            r.o.note(std::format("{}: picture {} ", key, k) + compare::hex(b + handle + 0x280 + k * 0x100, 0x90));
    }

    // Each picture the core holds: GetNumPicture, then GetPicture (its nine words, the addresses as offsets), its side
    // information and auxiliary data, its frame buffer; ReleasePicture after each when `release`.
    void take(const std::string& when, bool release, u32 limit = ~0u)
    {
        u8* b = r.b;
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("smvd4GetNumPicture", {handle, kOut}, when + ": GetNumPicture");
        r.o.line(when + ": pictures", w(ps_read32(b, kOut)));
        for (u32 k = 0; k < limit; k++) {
            std::memset(b + kPic, 0xEE, 0x30);
            const std::string key = std::format("{}: picture {}", when, taken);
            r.r("smvd4GetPicture", {handle, kPic}, key + ": GetPicture");
            if (ps_read32(b, kPic + 12) == 0xEEEEEEEE) return; // none
            taken++;
            const u32 luma = ps_read32(b, kPic), cb = ps_read32(b, kPic + 4), cr = ps_read32(b, kPic + 8);
            r.o.line(key + ": luma", in_core(luma));
            r.o.line(key + ": Cb", in_core(cb));
            r.o.line(key + ": Cr", in_core(cr));
            std::vector<u8> words(b + kPic, b + kPic + 0x30);
            std::memset(words.data(), 0, 12);
            r.o.line(key + ": the picture", compare::hex(words));
            std::memset(b + kSide, 0xEE, 0x40);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("smvd4GetSideInfoPic", {handle, kCount, kSide, kSide + 0x20}, key + ": GetSideInfoPic");
            r.o.line(key + ": side information", w(ps_read32(b, kCount)) + " " + compare::hex(b + kSide, 0x40));
            std::memset(b + kAuxOut, 0xEE, 0x20);
            ps_write32(b, kCount, 0xEEEEEEEE);
            r.r("smvd4GetAuxData", {handle, kCount, kAuxOut, kAuxOut + 0x10}, key + ": GetAuxData");
            r.o.line(key + ": auxiliary data", w(ps_read32(b, kCount)) + " " + compare::hex(b + kAuxOut, 0x20));
            // The picture's frame, laid out for its own size in its slot.
            const u32 pw = ps_read32(b, kPic + 16), ph = ps_read32(b, kPic + 20);
            const u32 lines = ((ph + 15) & ~15u) + 32;
            const u32 frame = luma - (lines * 16 + 16 * 16), bytes = smvd4_frame_bytes(pw, ph) - 0x80;
            u32 user = ~0u;
            if (!pending.empty()) {
                user = pending.front();
                pending.pop_front();
            }
            bool note = broken_users.contains(user);
            bool approx = false;
            if (stale_users.contains(user)) {
                note = broken_slot[luma];
                approx = approx_slot[luma];
            } else {
                broken_slot[luma] = note;
                if (anchorless_users.contains(user)) approx = luma == slot2_luma() || approx_slot[slot2_luma()];
                else approx = chain_approx;
                approx_slot[luma] = approx;
                chain_approx = approx;
            }
            if (luma >= kMem && frame >= kMem && frame + bytes <= kMem + kMemSize && pw <= 1920 && ph <= 1088) {
                if (note) {
                    u32 sum = 0;
                    for (u32 i = 0; i < bytes; i++) sum += b[frame + i];
                    r.o.note(std::format("{}: its frame buffer's sum {:#x} (not reproduced)", key, sum));
                    if (env_on("PSPOCKET_CORE_STATE"))
                        r.o.bytes(key + ": its frame buffer (a probe's)", b + frame, bytes);
                } else {
                    r.o.bytes(key + ": its frame buffer", b + frame, bytes, Rule::within(approx ? 2 : 1));
                }
            }
            if (release) r.r("smvd4ReleasePicture", {handle}, key + ": ReleasePicture");
        }
    }

    // SetAuxData (16 bytes of the caller's), then DecodePicture(handle, au {address, size, a word}, out). `damaged`:
    // the access unit's VOP is damaged on purpose (its picture not reproduced).
    u64 decode(const std::string& key, const std::vector<u8>& au, u32 user, bool aux = true, bool damaged = false)
    {
        u8* b = r.b;
        if (at + au.size() > kStream + 0x80000) at = kStream;
        std::memcpy(b + at, au.data(), au.size());
        if (aux) {
            for (u32 k = 0; k < 4; k++) ps_write32(b, kAux + 4 * k, (0x1000 << k) + user);
            r.r("smvd4SetAuxData", {handle, kAux}, key + ": SetAuxData");
        }
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, 0x5000 + user);
        at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
        ps_write32(b, kStatus, 0xEEEEEEEE);
        u64 v = 0;
        if (interlaced) {
            // Bit 2 of an interlaced P VOP's warning comes from its macroblocks in Sony's program (not reproduced):
            // compared without it, the answer itself a note.
            const std::optional<u64> got = r.side.call("smvd4DecodePicture", {handle, kAu, kStatus});
            v = got ? *got : ~u64{0};
            r.o.line(key + " (bit 2 aside)", got ? answer(static_cast<u32>(v) & ~4u) : std::string("(not exported)"));
            r.o.note(key + ": " + answer(v));
        } else {
            v = r.r("smvd4DecodePicture", {handle, kAu, kStatus}, key);
        }
        r.o.line(key + ": out", w(ps_read32(b, kStatus)));
        flag(key);
        note_state(key);
        // The test's bookkeeping: whether a picture came out, and whether pspocket reproduces its frame.
        const u32 a = static_cast<u32>(v);
        if (a == 0 || (a >= 0x80615100 && a < 0x80615110)) {
            const VopKind k = vop_kind(au);
            pending.push_back(user);
            if (interlaced || !k.coded || k.type >= 2) {
                stale_users.insert(user);
            } else if (k.type == 0) {
                broken = damaged;
                if (damaged) broken_users.insert(user);
                no_anchor = false;
            } else if (k.type == 1) {
                if (broken || damaged) {
                    broken = true;
                    broken_users.insert(user);
                }
                if (no_anchor) anchorless_users.insert(user);
                no_anchor = false;
            }
        }
        return v;
    }

    void flag(const std::string& key)
    {
        ps_write32(r.b, kOut, 0xEEEEEEEE);
        r.r("smvd4GetExistenceFlag", {handle, kOut}, key + ": GetExistenceFlag");
        r.o.line(key + ": the flag", w(ps_read32(r.b, kOut)));
    }

    void seq(const std::string& key)
    {
        std::memset(r.b + kSeq, 0xEE, 0x40);
        r.r("smvd4GetSideInfoSeq", {handle, kSeq}, key + ": GetSideInfoSeq");
        r.o.line(key + ": the sequence", compare::hex(r.b + kSeq, 0x40));
    }

    // Initialize, CancelDecode and FlushPicture, with the test's bookkeeping.
    u64 init(const std::string& key)
    {
        broken = false;
        no_anchor = true;
        pending.clear();
        return r.r("smvd4Initialize", {handle}, key);
    }
    u64 cancel(const std::string& key)
    {
        broken = false;
        no_anchor = true;
        pending.clear();
        return r.r("smvd4CancelDecode", {handle}, key);
    }
    u64 flush(const std::string& key)
    {
        no_anchor = true;
        const u64 v = r.r("smvd4FlushPicture", {handle}, key);
        note_state(key);
        return v;
    }

    // A whole stream, each picture taken and released as soon as there is one; then the flush.
    void stream(const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0)
    {
        for (usize i = 0; i < aus.size(); i++) {
            decode(std::format("{}: DecodePicture {}", what, i), aus[i], user0 + static_cast<u32>(i));
            take(std::format("{}: after DecodePicture {}", what, i), true);
        }
        flush(what + ": FlushPicture");
        take(what + ": after FlushPicture", true);
    }
};

Observed smvd4(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libsmvd4");
    u8* b = r.b;
    make_spurs(r);
    Smvd4 d{r};
    // The memory queries: each level's largest picture, and sizes within them.
    for (u32 level = 0; level <= 8; level++) {
        ps_write32(b, kOut, 0xEEEEEEEE);
        r.r("smvd4GetMemorySize", {kOut, level}, std::format("GetMemorySize level {}", level));
        r.o.line(std::format("GetMemorySize level {}: the size", level), d.w(ps_read32(b, kOut)));
    }
    r.r("smvd4GetMemorySize", {0, 1}, "GetMemorySize with no output");
    for (const auto& [level, wd, ht] : {std::tuple{1u, 96u, 64u}, {1u, 176u, 144u}, {1u, 177u, 144u}, {1u, 176u, 145u},
             {0u, 720u, 576u}, {6u, 16u, 16u}, {1u, 0u, 64u}, {1u, 96u, 0u}, {7u, 96u, 64u}, {5u, 640u, 480u},
             {5u, 0xFFFFFFFFu, 64u}}) {
        smvd4_param(b, 5, 0, wd, ht);
        ps_write32(b, kOut, 0xEEEEEEEE);
        const std::string key = std::format("GetMemorySize2 level {} {}x{}", level, wd, ht);
        r.r("smvd4GetMemorySize2", {kOut, level, kParam}, key);
        r.o.line(key + ": the size", d.w(ps_read32(b, kOut)));
    }
    smvd4_param(b, 4, 0, 96, 64);
    ps_write32(b, kOut, 0xEEEEEEEE);
    r.r("smvd4GetMemorySize2", {kOut, 1, kParam}, "GetMemorySize2 type 4");
    r.o.line("GetMemorySize2 type 4: the size", d.w(ps_read32(b, kOut)));
    r.r("smvd4GetMemorySize2", {kOut, 1, 0}, "GetMemorySize2 with no parameters");
    r.r("smvd4GetMemorySize2", {0, 1, kParam}, "GetMemorySize2 with no output");
    ps_write32(b, kOut, 0xEEEEEEEE);
    r.r("smvd4GetVersionNumber", {kOut}, "GetVersionNumber");
    r.o.line("GetVersionNumber: the version", d.w(ps_read32(b, kOut)));

    const std::vector<std::vector<u8>> plain = encode(AV_CODEC_ID_MPEG4, 96, 64, 10, 4);
    smvd4_param(b, 5, kTs, 96, 64);
    ps_write32(b, kOut, 0);
    r.r("smvd4GetMemorySize2", {kOut, 1, kParam}, "GetMemorySize2 for the decoder");
    const u32 need = ps_read32(b, kOut);
    // CreateInstance2's refusals.
    std::memset(b + kMem, 0xA5, need + 0x1000);
    r.r("smvd4CreateInstance2", {0, need, 1, kParam}, "CreateInstance2 with no memory");
    r.r("smvd4CreateInstance2", {kMem + 0x40, need, 1, kParam}, "CreateInstance2 with memory not aligned");
    r.r("smvd4CreateInstance2", {kMem, need, 1, 0}, "CreateInstance2 with no parameters");
    for (const u32 type : {0u, 1u, 2u, 3u, 6u}) {
        smvd4_param(b, type, kTs, 96, 64);
        r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, std::format("CreateInstance2 type {}", type));
    }
    smvd4_param(b, 5, 0, 96, 64);
    r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, "CreateInstance2 with no taskset");
    std::memset(b + kData + 0x7000, 0, 0x1000);
    smvd4_param(b, 5, kData + 0x7000, 96, 64);
    r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, "CreateInstance2 with a taskset that is none");
    smvd4_param(b, 5, kTs, 96, 64);
    r.r("smvd4CreateInstance2", {kMem, need - 1, 1, kParam}, "CreateInstance2 with one byte less");
    r.r("smvd4CreateInstance2", {kMem, need, 7, kParam}, "CreateInstance2 level 7");
    smvd4_param(b, 5, kTs, 200, 64);
    r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, "CreateInstance2 200x64 level 1");
    smvd4_param(b, 4, kTs, 96, 64);
    r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, "CreateInstance2 type 4 level 1 with too little");
    // The decoder.
    smvd4_param(b, 5, kTs, 96, 64);
    std::memset(b + kMem, 0xA5, need + 0x1000);
    d.width = 96;
    d.height = 64;
    if (r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, "CreateInstance2") == 0) {
        d.handle = kMem;
        d.note_state("CreateInstance2");
        d.flag("after CreateInstance2");
        d.seq("after CreateInstance2");
        r.r("smvd4GetPicture", {d.handle, kPic}, "GetPicture before Initialize");
        r.r("smvd4ReleasePicture", {d.handle}, "ReleasePicture before Initialize");
        d.init("Initialize");
        d.note_state("Initialize");
        d.flag("after Initialize");
        d.seq("after Initialize");
        d.stream("plain", plain, 10);
        d.flag("after the stream");
        d.seq("after the stream");
        // Pictures held: how many access units the core takes before it has no frame buffer.
        d.init("Initialize for holding");
        for (usize i = 0; i < 5; i++)
            d.decode(std::format("held: DecodePicture {}", i), plain[i], 100 + static_cast<u32>(i));
        d.take("held", false, 1);
        r.r("smvd4ReleasePicture", {d.handle}, "held: ReleasePicture");
        d.decode("held: DecodePicture again", plain[3], 110);
        d.take("held, then", true);
        r.r("smvd4ReleasePicture", {d.handle}, "held: ReleasePicture once too many");
        // Discontinue and CancelDecode in a stream.
        d.init("Initialize 2");
        d.decode("Discontinue: DecodePicture 0", plain[0], 200);
        d.decode("Discontinue: DecodePicture 1", plain[1], 201);
        r.r("smvd4Discontinue", {d.handle}, "Discontinue");
        d.note_state("Discontinue");
        d.take("after Discontinue", true);
        d.decode("Discontinue: DecodePicture 2", plain[2], 202);
        d.take("after Discontinue: DecodePicture 2", true);
        d.decode("CancelDecode: DecodePicture 3", plain[3], 203);
        d.cancel("CancelDecode");
        d.note_state("CancelDecode");
        d.take("after CancelDecode", true);
        d.decode("CancelDecode: DecodePicture 4", plain[4], 204);
        d.take("after CancelDecode: DecodePicture 4", true);
        d.decode("a P picture after CancelDecode", plain[5], 205);
        d.take("after a P picture after CancelDecode", true);
        // The checks.
        d.init("Initialize 3");
        d.decode("a P picture first", plain[1], 300);
        d.take("a P picture first", true);
        d.decode("without SetAuxData", plain[0], 301, false);
        d.take("without SetAuxData", true);
        r.r("smvd4SetAuxData", {d.handle, 0}, "SetAuxData with no data");
        r.r("smvd4DecodePicture", {d.handle, 0, kStatus}, "DecodePicture with no access unit");
        r.r("smvd4DecodePicture", {d.handle, kAu, 0}, "DecodePicture with no output");
        ps_write32(b, kAu + 4, 0);
        r.r("smvd4DecodePicture", {d.handle, kAu, kStatus}, "DecodePicture with size 0");
        d.take("after size 0", true);
        r.r("smvd4GetNumPicture", {d.handle, 0}, "GetNumPicture with no output");
        r.r("smvd4GetPicture", {d.handle, 0}, "GetPicture with no output");
        r.r("smvd4GetSideInfoPic", {d.handle, kCount, kSide, 0}, "GetSideInfoPic with no second output");
        r.r("smvd4GetAuxData", {d.handle, kCount, kAuxOut, 0}, "GetAuxData with no second output");
        r.r("smvd4GetSideInfoSeq", {d.handle, 0}, "GetSideInfoSeq with no output");
        r.r("smvd4GetExistenceFlag", {d.handle, 0}, "GetExistenceFlag with no output");
        // Streams of other kinds, each after Initialize.
        const auto variant = [&](const std::string& what, const std::vector<std::vector<u8>>& aus, u32 user0) {
            if (!selected(what)) return;
            d.init(what + ": Initialize");
            for (usize i = 0; i < aus.size(); i++)
                r.o.note(std::format("{}: access unit {}: {} bytes", what, i, aus[i].size()));
            d.stream(what, aus, user0);
            d.flag(what);
            d.seq(what);
        };
        const auto enc = [](int w, int h, int frames, int gop, std::function<void(AVCodecContext*)> setup = {}) {
            return encode(AV_CODEC_ID_MPEG4, w, h, frames, gop, setup);
        };
        const auto flags = [](int f) { return [f](AVCodecContext* av) { av->flags |= f; }; };
        const auto option = [](const char* name, const char* value) {
            return [name, value](AVCodecContext* av) { av_opt_set(av->priv_data, name, value, 0); };
        };
        const int kInterlaced = AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
        // The start codes' ids and random_accessible_vol.
        {
            std::vector<std::vector<u8>> aus = plain;
            for (auto& au : aus) {
                const usize vo = start_code_at(au, 0x00);
                if (vo < au.size()) au[vo + 3] = 0x01;
                const usize vol = start_code_at(au, 0x20);
                if (vol < au.size()) {
                    au[vol + 3] = 0x21;
                    au[vol + 4] |= 0x80;
                }
            }
            variant("ids", aus, 1000);
        }
        variant("SAR 64:45", enc(96, 64, 3, 4, [](AVCodecContext* av) { av->sample_aspect_ratio = {64, 45}; }), 1100);
        {
            std::vector<std::vector<u8>> aus = enc(96, 64, 3, 4);
            aus[0] = with_fixed_vop_rate(aus[0], 1);
            variant("a fixed VOP rate", aus, 1200);
        }
        {
            std::vector<std::vector<u8>> aus = enc(96, 64, 3, 4);
            aus[0] = with_colour(aus[0], 5, 6, 7);
            variant("colour", aus, 1300);
        }
        variant("30 pictures", enc(96, 64, 30, 12), 1400);
        variant("closed GOP", enc(96, 64, 6, 3, [](AVCodecContext* av) { av->flags |= AV_CODEC_FLAG_CLOSED_GOP;
            av_opt_set_int(av->priv_data, "sc_threshold", 1000000000, 0);
        }), 1500);
        variant("one VOL", enc(96, 64, 6, 3, [](AVCodecContext* av) {
            av->strict_std_compliance = FF_COMPLIANCE_VERY_STRICT;
        }), 1600);
        variant("MPEG quantisation", enc(96, 64, 4, 4, option("mpeg_quant", "1")), 1700);
        variant("resync markers", enc(96, 64, 4, 4, option("ps", "60")), 1800);
        variant("data partitioning", enc(96, 64, 4, 4, [](AVCodecContext* av) {
            av_opt_set(av->priv_data, "ps", "60", 0);
            av_opt_set(av->priv_data, "data_partitioning", "1", 0);
        }), 1900);
        // A layer refused (MPEG quantisation) of another size than the one before: what the sequence information
        // keeps.
        variant("80x48 with MPEG quantisation", enc(80, 48, 2, 4, option("mpeg_quant", "1")), 1750);
        variant("4MV", enc(96, 64, 4, 4, [](AVCodecContext* av) { av->flags |= AV_CODEC_FLAG_4MV; }), 2000);
        variant("AC prediction", enc(96, 64, 4, 4, flags(AV_CODEC_FLAG_AC_PRED)), 2100);
        d.interlaced = true;
        variant("interlaced", enc(96, 64, 4, 4, flags(kInterlaced)), 2200);
        variant("interlaced, longer", enc(96, 64, 12, 12, flags(kInterlaced)), 2250);
        d.interlaced = false;
        variant("quarter samples", enc(96, 64, 4, 4, flags(AV_CODEC_FLAG_QPEL)), 2300);
        variant("B pictures", enc(96, 64, 7, 6, [](AVCodecContext* av) { av->max_b_frames = 2; }), 2400);
        variant("80x48", enc(80, 48, 4, 4), 2500);
        {
            // N-VOPs, two VOPs in one access unit, headers alone, a VOP in two halves, bytes that are no stream.
            const std::vector<u8> nv = n_vop(5, 1);
            const usize vop = start_code_at(plain[0], 0xB6);
            const std::vector<u8> headers(plain[0].begin(), plain[0].begin() + static_cast<std::ptrdiff_t>(vop));
            const std::vector<u8> ivop(plain[0].begin() + static_cast<std::ptrdiff_t>(vop), plain[0].end());
            const auto half = plain[1].begin() + static_cast<std::ptrdiff_t>(plain[1].size() / 2);
            const std::vector<u8> first(plain[1].begin(), half);
            const std::vector<u8> second(half, plain[1].end());
            std::vector<u8> two = plain[1];
            two.insert(two.end(), plain[2].begin(), plain[2].end());
            std::vector<u8> junk(40);
            for (usize i = 0; i < junk.size(); i++) junk[i] = static_cast<u8>(i * 37 + 11);
            variant("an N-VOP", {plain[0], nv, plain[1]}, 2600);
            variant("two VOPs in one", {plain[0], two, plain[3]}, 2700);
            variant("headers alone", {headers, ivop, plain[1]}, 2800);
            if (selected("a VOP in halves")) {
                d.init("a VOP in halves: Initialize");
                d.decode("a VOP in halves: DecodePicture 0", plain[0], 2900);
                d.take("a VOP in halves: after DecodePicture 0", true);
                d.decode("a VOP in halves: DecodePicture 1", first, 2901, true, true);
                d.take("a VOP in halves: after DecodePicture 1", true);
                d.decode("a VOP in halves: DecodePicture 2", second, 2902);
                d.take("a VOP in halves: after DecodePicture 2", true);
                d.decode("a VOP in halves: DecodePicture 3", plain[2], 2903);
                d.take("a VOP in halves: after DecodePicture 3", true);
                d.flush("a VOP in halves: FlushPicture");
                d.take("a VOP in halves: after FlushPicture", true);
                d.flag("a VOP in halves");
                d.seq("a VOP in halves");
            }
            variant("bytes that are no stream", {junk, plain[0], junk, plain[1]}, 3000);
            // An I picture without its GOV header; a GOV a second in; a broken link; a picture larger than the
            // instance's; damaged bits.
            {
                std::vector<std::vector<u8>> aus = plain;
                for (const usize k : {usize{4}, usize{8}}) {
                    const usize at_gov = start_code_at(aus[k], 0xB3), at_vop = start_code_at(aus[k], 0xB6);
                    if (at_gov < at_vop && at_vop < aus[k].size())
                        aus[k].erase(aus[k].begin() + static_cast<std::ptrdiff_t>(at_gov),
                            aus[k].begin() + static_cast<std::ptrdiff_t>(at_vop));
                }
                variant("I without GOV", aus, 3200);
            }
            variant("a GOV a second in", enc(96, 64, 27, 25), 3300);
            {
                std::vector<std::vector<u8>> aus = plain;
                const usize gov = start_code_at(aus[4], 0xB3);
                if (gov + 6 < aus[4].size()) aus[4][gov + 6] |= 0x10; // broken_link (after closed_gov)
                variant("a broken link", aus, 3400);
            }
            {
                std::vector<std::vector<u8>> aus = plain;
                for (usize i = 12; i < 20 && i < aus[1].size(); i++) aus[1][i] ^= 0x5A;
                for (usize i = 30; i < 60 && i < aus[0].size(); i++) aus[0][i] ^= 0xA5;
                variant("damaged", {aus[0], aus[1], plain[2]}, 3600);
            }
            // How long Discontinue's mark lasts; a P picture right after CancelDecode; P pictures with no anchor
            // decoded into the second and the third slot.
            if (selected("discontinuity")) {
                d.init("discontinuity: Initialize");
                d.decode("discontinuity: DecodePicture 0", plain[0], 3110);
                d.decode("discontinuity: DecodePicture 1", plain[1], 3111);
                d.take("discontinuity: the first two", true);
                r.r("smvd4Discontinue", {d.handle}, "discontinuity: Discontinue");
                for (usize i = 2; i < 7; i++) {
                    d.decode(std::format("discontinuity: DecodePicture {}", i), plain[i], 3110 + static_cast<u32>(i));
                    d.take(std::format("discontinuity: after DecodePicture {}", i), true);
                }
            }
            if (selected("cancelled")) {
                d.init("cancelled: Initialize");
                d.decode("cancelled: DecodePicture 0", plain[0], 3120);
                d.decode("cancelled: DecodePicture 1", plain[1], 3121);
                d.cancel("cancelled: CancelDecode");
                d.decode("cancelled: a P picture", plain[2], 3122);
                d.take("cancelled: a P picture", true);
                d.decode("cancelled: an I picture", plain[4], 3123);
                d.take("cancelled: an I picture", true);
            }
            if (selected("no anchor")) {
                d.init("no anchor, slot 1: Initialize");
                d.decode("no anchor, slot 1: an I picture", plain[0], 3130);
                d.flush("no anchor, slot 1: FlushPicture");
                d.decode("no anchor, slot 1: a P picture", plain[1], 3131);
                d.take("no anchor, slot 1", true);
                d.init("no anchor, slot 2: Initialize");
                d.decode("no anchor, slot 2: an I picture", plain[0], 3140);
                d.decode("no anchor, slot 2: a P picture", plain[1], 3141);
                d.flush("no anchor, slot 2: FlushPicture");
                d.decode("no anchor, slot 2: another P picture", plain[2], 3142);
                d.take("no anchor, slot 2", true);
            }
            if (selected("after FlushPicture")) {
                d.init("after FlushPicture: Initialize");
                d.decode("after FlushPicture: DecodePicture 0", plain[0], 3100);
                d.decode("after FlushPicture: DecodePicture 1", plain[1], 3101);
                d.take("after FlushPicture: the first two", true);
                d.flush("after FlushPicture: FlushPicture");
                d.take("after FlushPicture: the flush", true);
                d.decode("after FlushPicture: a P picture", plain[2], 3102);
                d.take("after FlushPicture: a P picture", true);
                d.flush("after FlushPicture: FlushPicture 2");
                d.flush("after FlushPicture: FlushPicture 3");
                d.decode("after FlushPicture: an I picture", plain[4], 3103);
                d.take("after FlushPicture: an I picture", true);
                d.flag("after FlushPicture");
            }
        }
        // The GOV's time code in hours and minutes; VBV parameters; a layer without its identifier and control
        // parameters (FFmpeg's FF_BUG_MS).
        {
            std::vector<std::vector<u8>> aus = plain;
            const usize gov = start_code_at(aus[4], 0xB3);
            if (gov + 6 < aus[4].size()) {
                // hours 1, minutes 2, marker, seconds 3, closed_gov 0, broken_link 0, then stuffing.
                BitWriter tw;
                tw.put(1, 5);
                tw.put(2, 6);
                tw.put(1, 1);
                tw.put(3, 6);
                tw.put(0, 2);
                tw.put(0, 1);
                while (tw.n % 8) tw.put(1, 1);
                std::copy(tw.out.begin(), tw.out.end(), aus[4].begin() + static_cast<std::ptrdiff_t>(gov + 4));
            }
            variant("a GOV at 1:02:03", aus, 3700);
        }
        {
            std::vector<std::vector<u8>> aus = plain;
            aus[0] = with_vbv(aus[0]);
            variant("VBV parameters", aus, 3900);
        }
        {
            std::vector<std::vector<u8>> aus = plain;
            aus[0] = with_plain_vol(aus[0]);
            variant("no layer identifier or control parameters", aus, 3800);
        }
        // Layers written whole: identifiers and control parameters of other values (each stream after the one before:
        // the fields a layer leaves out keep what?).
        {
            const auto layer = [&](const std::string& what, const VolFields& f, u32 vo_ident, u32 user0) {
                std::vector<std::vector<u8>> aus(plain.begin(), plain.begin() + 2);
                aus[0] = with_vol(aus[0], f, vo_ident);
                variant(what, aus, user0);
            };
            VolFields a;
            a.verid = 2;
            a.priority = 3;
            layer("layer a: no VO identifier, verid 2, priority 3", a, 0, 3810);
            VolFields e0;
            e0.ident = 0;
            layer("layer e: no identifier after verid 2", e0, 1, 3815);
            VolFields bb;
            bb.random_access = 1;
            bb.type = 5;
            bb.low_delay = 0;
            bb.chroma = 1;
            bb.vbv = 1;
            layer("layer b: random access, type 5, low delay 0, VBV", bb, 1, 3820);
            VolFields c;
            c.ident = 0;
            c.control = 0;
            c.aspect = 2;
            layer("layer c: no identifier, no control parameters", c, 1, 3830);
            VolFields e4;
            e4.priority = 7;
            e4.verid = 4;
            e4.chroma = 2;
            layer("layer d: priority 7, verid 4, chroma format 2", e4, 1, 3840);
        }
        // A layer larger than the instance's: a fatal error; what the decoder answers after it.
        if (selected("larger than the instance")) {
            variant("larger than the instance", enc(176, 144, 3, 4), 3500);
            d.init("after the fatal error: Initialize");
            d.decode("after the fatal error: DecodePicture", plain[0], 3510);
            d.take("after the fatal error", true);
            r.r("smvd4Discontinue", {d.handle}, "after the fatal error: Discontinue");
            r.r("smvd4CancelDecode", {d.handle}, "after the fatal error: CancelDecode");
            d.flush("after the fatal error: FlushPicture");
            d.flag("after the fatal error");
            d.seq("after the fatal error");
        }
        r.r("smvd4ReleaseInstance", {d.handle}, "ReleaseInstance");
        d.flag("after ReleaseInstance");
    }
    // Fresh instances: the existence flag and the GOV's fields from the start; layers of other sizes than the
    // instance's (larger ones are fatal errors).
    if (selected("fresh")) {
        const std::vector<u8> nothing(16, 0x33);
        const usize vop = start_code_at(plain[0], 0xB6), gov = start_code_at(plain[0], 0xB3);
        const std::vector<u8> headers(plain[0].begin(), plain[0].begin() + static_cast<std::ptrdiff_t>(gov));
        std::vector<u8> no_gov = plain[0];
        no_gov.erase(no_gov.begin() + static_cast<std::ptrdiff_t>(gov),
            no_gov.begin() + static_cast<std::ptrdiff_t>(vop));
        smvd4_param(b, 5, kTs, 96, 64);
        std::memset(b + kMem, 0xA5, need + 0x1000);
        if (r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, "fresh: CreateInstance2") == 0) {
            Smvd4 f{r};
            f.handle = kMem;
            f.width = 96;
            f.height = 64;
            f.init("fresh: Initialize");
            const std::vector<std::vector<u8>> mq = encode(AV_CODEC_ID_MPEG4, 96, 64, 1, 4, [](AVCodecContext* av) {
                av_opt_set(av->priv_data, "mpeg_quant", "1", 0);
            });
            f.decode("fresh: a layer with MPEG quantisation", mq[0], 4999);
            f.take("fresh: a layer with MPEG quantisation", true);
            f.decode("fresh: a P picture", plain[1], 5000);
            f.take("fresh: a P picture", true);
            f.decode("fresh: headers", headers, 5001);
            f.take("fresh: headers", true);
            f.decode("fresh: an I picture without GOV", no_gov, 5002);
            f.take("fresh: an I picture without GOV", true);
            f.decode("fresh: an I picture", plain[0], 5003);
            f.take("fresh: an I picture", true);
            f.init("fresh: Initialize again");
            f.flag("fresh: after Initialize again");
            f.decode("fresh: again, an I picture without GOV", no_gov, 5004);
            f.take("fresh: again, an I picture without GOV", true);
            r.r("smvd4ReleaseInstance", {f.handle}, "fresh: ReleaseInstance");
        }
        for (const auto& [wd, ht] :
            {std::pair{112, 48}, {64, 80}, {96, 64}, {80, 64}, {96, 96}, {48, 128}, {32, 144}}) {
            const std::string key = std::format("fresh {}x{}", wd, ht);
            std::memset(b + kMem, 0xA5, need + 0x1000);
            if (r.r("smvd4CreateInstance2", {kMem, need, 1, kParam}, key + ": CreateInstance2") != 0) continue;
            Smvd4 f{r};
            f.handle = kMem;
            f.width = static_cast<u32>(wd);
            f.height = static_cast<u32>(ht);
            f.init(key + ": Initialize");
            const std::vector<std::vector<u8>> aus = encode(AV_CODEC_ID_MPEG4, wd, ht, 2, 4);
            f.decode(key + ": DecodePicture 0", aus[0], 5100);
            f.take(key + ": 0", true);
            f.decode(key + ": DecodePicture 1", aus[1], 5101);
            f.take(key + ": 1", true);
            f.seq(key);
            r.r("smvd4ReleaseInstance", {f.handle}, key + ": ReleaseInstance");
        }
    }
    // An instance for a level (type 4, level 1: 176x144), on fresh memory: no GOV yet, junk first.
    smvd4_param(b, 4, kTs);
    ps_write32(b, kOut, 0);
    r.r("smvd4GetMemorySize", {kOut, 1}, "GetMemorySize for the level 1 decoder");
    const u32 need4 = ps_read32(b, kOut);
    std::memset(b + kMem, 0xA5, need4 + 0x1000);
    if (selected("level 1") && r.r("smvd4CreateInstance2", {kMem, need4, 1, kParam}, "level 1: CreateInstance2") == 0) {
        Smvd4 e{r};
        e.handle = kMem;
        e.width = 176;
        e.height = 144;
        e.init("level 1: Initialize");
        std::vector<u8> junk(16, 0x33);
        e.decode("level 1: junk", junk, 4000);
        e.seq("level 1: after junk");
        std::vector<std::vector<u8>> aus = encode(AV_CODEC_ID_MPEG4, 176, 144, 4, 4);
        for (auto& au : aus) {
            const usize at_gov = start_code_at(au, 0xB3), at_vop = start_code_at(au, 0xB6);
            if (at_gov < at_vop && at_vop < au.size())
                au.erase(au.begin() + static_cast<std::ptrdiff_t>(at_gov),
                    au.begin() + static_cast<std::ptrdiff_t>(at_vop));
        }
        e.stream("level 1: no GOV", aus, 4100);
        e.seq("level 1");
        e.stream("level 1: 96x64", plain, 4200);
        r.r("smvd4ReleaseInstance", {e.handle}, "level 1: ReleaseInstance");
    }
    r.r("smvd4ReleaseInstance", {0}, "ReleaseInstance with no instance");
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

TEST_CASE("The MPEG-4 core (libsmvd4): pspocket's own, answering as Sony's did (D-920)", "[hle][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP("this FFmpeg has no mpeg4 encoder (build-ffmpeg.sh)");
    check_names("libsmvd4", {"smvd4GetMemorySize", "smvd4GetMemorySize2", "smvd4CreateInstance2",
                                "smvd4ReleaseInstance", "smvd4Initialize", "smvd4DecodePicture", "smvd4FlushPicture",
                                "smvd4Discontinue", "smvd4CancelDecode", "smvd4GetPicture", "smvd4GetNumPicture",
                                "smvd4ReleasePicture", "smvd4GetSideInfoPic", "smvd4GetAuxData", "smvd4GetSideInfoSeq",
                                "smvd4GetExistenceFlag", "smvd4SetAuxData", "smvd4GetVersionNumber"},
        false);
    Setup s;
    s.mode = Mode::Self;
    const Observed o = smvd4(s, false);
    // Sony's answers (test_vdec_cores_compare.cpp's comparison, firmware 4.92).
    CHECK(o.value("GetMemorySize level 1: the size") == "0x44480");
    CHECK(o.value("GetMemorySize2 level 1 96x64: the size") == "0x23900");
    CHECK(o.value("GetMemorySize2 level 1 177x144") == "0x80615142");
    CHECK(o.value("CreateInstance2 type 0") == "0x80615142");
    CHECK(o.value("CreateInstance2 with one byte less") == "0x80615181");
    CHECK(o.value("CreateInstance2 level 7") == "0x80615180");
    CHECK(o.value("CreateInstance2 with a taskset that is none") == "0x80410902");
    CHECK(o.value("after CreateInstance2: the flag") == "0xa5a5a5a5"); // never written before an access unit
    CHECK(o.value("plain: DecodePicture 0: out") == "0x17a");
    CHECK(o.value("plain: after DecodePicture 0: picture 0: luma") == "the core's memory +0x15180");
    CHECK(o.value("plain: after DecodePicture 1: picture 1: luma") == "the core's memory +0x19a00");
    CHECK(o.value("plain: after DecodePicture 0: picture 0: the picture") ==
        "hex:000000000000000000000000400000040000006000000040000000600000003000000000eeeeeeeeeeeeeeeeeeeeeeee");
    CHECK(o.value("plain: after DecodePicture 1: picture 1: side information")
              .starts_with("0x1 hex:0101a5a50000000000000101000000000000000000010101000000a500020100ee"));
    CHECK(o.value("plain: after FlushPicture: GetNumPicture") == "0x80615122");
    CHECK(o.value("after the stream: the sequence")
              .starts_with("hex:010020000101010101000001010100ee00000000000000000000000000ee001900ee00000060004000"));
    CHECK(o.value("held: DecodePicture 3") == "0x80615121"); // three frame buffers, none free
    CHECK(o.value("a P picture first") == "0x80615120");     // no video object layer since Initialize
    CHECK(o.value("after Discontinue: DecodePicture 2: picture 16: GetPicture") == "0x8061511b");
    CHECK(o.value("larger than the instance: DecodePicture 0") == "0x80615181");
    CHECK(o.value("after the fatal error: Initialize") == "0x80615180");
    const Data* first = o.find_data("plain: after DecodePicture 0: picture 0: its frame buffer");
    REQUIRE(first);
    CHECK(first->bytes[0x700] == 30); // the picture's first sample, after a strip and 16 lines of edge
}

TEST_CASE("The MPEG-4 core (libsmvd4) against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP("this FFmpeg has no mpeg4 encoder (build-ffmpeg.sh)");
    const Setup s = setup();
    run(s, "libsmvd4 against Sony's", [&](bool sony) { return smvd4(s, sony); }, [](const Observed&) {});
}

// The MPEG-2 writer's streams (mpeg2_writer.hpp) as FFmpeg's decoder reads them: every picture, in display order, the
// I pictures' blocks flat at their levels, no errors; and its other kinds (MPEG-1, interlaced frames, field pictures,
// the coding options and extensions) without errors.
std::vector<std::vector<u8>> ffmpeg_mpeg2(const std::vector<std::vector<u8>>& aus, int w, int h, std::vector<int>* types,
    bool explode = true)
{
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    REQUIRE(dec);
    AVCodecContext* av = avcodec_alloc_context3(dec);
    av->err_recognition = explode ? AV_EF_EXPLODE : 0; // field pictures: FFmpeg's EXPLODE refuses a second field
    REQUIRE(avcodec_open2(av, dec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    std::vector<std::vector<u8>> lumas;
    const auto drain = [&] {
        while (avcodec_receive_frame(av, f) == 0) {
            CHECK(f->decode_error_flags == 0);
            CHECK(f->width == w);
            CHECK(f->height == h);
            std::vector<u8> y(static_cast<usize>(w * h));
            for (int r = 0; r < h; r++) std::memcpy(y.data() + r * w, f->data[0] + r * f->linesize[0], static_cast<usize>(w));
            lumas.push_back(std::move(y));
            if (types) types->push_back(f->pict_type);
            av_frame_unref(f);
        }
    };
    for (const auto& au : aus) {
        std::vector<u8> padded(au);
        padded.resize(au.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        pkt->data = padded.data();
        pkt->size = static_cast<int>(au.size());
        CHECK(avcodec_send_packet(av, pkt) == 0);
        drain();
    }
    avcodec_send_packet(av, nullptr);
    drain();
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return lumas;
}

TEST_CASE("The tests' MPEG-2 streams decode with FFmpeg", "[hle][vdec-cores]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO)) SKIP("this FFmpeg has no MPEG-2 decoder");
    mpeg2::Sequence seq;
    for (const bool ac : {false, true}) {
        INFO(ac);
        std::vector<int> types;
        const std::vector<std::vector<u8>> lumas = ffmpeg_mpeg2(mpeg2::stream(seq, "IPPBBPIBBP", ac), 96, 64, &types);
        REQUIRE(lumas.size() == 10);
        CHECK(types[0] == AV_PICTURE_TYPE_I);
        CHECK(types[3] == AV_PICTURE_TYPE_B);
        CHECK(types[6] == AV_PICTURE_TYPE_I);
        if (!ac) {
            mpeg2::Picture first;
            for (int mby = 0; mby < 4; mby++)
                for (int mbx = 0; mbx < 6; mbx++)
                    for (int b = 0; b < 4; b++) {
                        const int x = mbx * 16 + (b & 1) * 8 + 3, y = mby * 16 + (b >> 1) * 8 + 5;
                        CHECK(lumas[0][static_cast<usize>(y * 96 + x)] == mpeg2::block_level(first, mbx, mby, b));
                    }
            // A P picture's macroblocks that are not coded are the I picture's (the second picture's first is not),
            // the coded ones not.
            CHECK(lumas[1][2] == lumas[0][2]);
            CHECK(lumas[1][16 + 2] != lumas[0][16 + 2]);
        }
    }
    SECTION("MPEG-1")
    {
        mpeg2::Sequence m1;
        m1.mpeg1 = true;
        CHECK(ffmpeg_mpeg2(mpeg2::stream(m1, "IPBPB", true), 96, 64, nullptr).size() == 5);
    }
    SECTION("macroblocks of every kind")
    {
        mpeg2::Sequence plain, m1, il, big;
        m1.mpeg1 = true;
        il.progressive = false;
        big.width = 176;
        big.height = 144;
        CHECK(ffmpeg_mpeg2(mpeg2::stream(plain, "IPBBPBBP", true, 0, [](mpeg2::Picture& p, int) { p.motion = true; }),
                  96, 64, nullptr).size() == 8);
        using Edit = void (*)(mpeg2::Picture&, int);
        for (const Edit edit : {Edit{[](mpeg2::Picture& p, int) { p.motion = true; p.f_code = 2; }},
                 Edit{[](mpeg2::Picture& p, int) { p.motion = true; p.f_code = 3; }},
                 Edit{[](mpeg2::Picture& p, int i) { p.motion = true; p.q_scale_type = true; p.intra_vlc = i % 2; }}}) {
            CHECK(ffmpeg_mpeg2(mpeg2::stream(plain, "IPBPBP", true, 3, edit), 96, 64, nullptr).size() == 6);
            CHECK(ffmpeg_mpeg2(mpeg2::stream(big, "IPBPBP", true, 5, edit), 176, 144, nullptr).size() == 6);
        }
        CHECK(ffmpeg_mpeg2(mpeg2::stream(m1, "IPBPB", true, 0, [](mpeg2::Picture& p, int) { p.motion = true; p.f_code = 2; }),
                  96, 64, nullptr).size() == 5);
        CHECK(ffmpeg_mpeg2(mpeg2::stream(il, "IPBPB", true, 0, [](mpeg2::Picture& p, int) { p.motion = true; }), 96, 64,
                  nullptr).size() == 5);
    }
    SECTION("interlaced frames")
    {
        mpeg2::Sequence il;
        il.progressive = false;
        CHECK(ffmpeg_mpeg2(mpeg2::stream(il, "IPBPB", true), 96, 64, nullptr).size() == 5);
    }
    SECTION("coding options and extensions")
    {
        mpeg2::Sequence ex;
        ex.display_width = 90;
        ex.display_height = 60;
        ex.colour_primaries = 5;
        ex.transfer = 6;
        ex.matrix = 7;
        ex.user_data = {'s', 'e', 'q'};
        ex.intra_matrix.assign(64, 16);
        ex.non_intra_matrix.assign(64, 20);
        const auto edit = [](mpeg2::Picture& p, int i) {
            p.dc_precision = i % 3;
            p.q_scale_type = i % 2;
            p.intra_vlc = i % 2;
            p.alternate_scan = i % 3 == 1;
            p.display_offsets = {{i * 16, -i * 8}};
            p.copyright = i == 1;
            p.composite = i == 2;
            p.burst_amplitude = 5;
            if (i == 3) p.quant_intra_matrix.assign(64, 24);
            p.user_data = {'p', static_cast<u8>('0' + i)};
        };
        CHECK(ffmpeg_mpeg2(mpeg2::stream(ex, "IPBPBIP", true, 0, edit), 96, 64, nullptr).size() == 7);
    }
    SECTION("field pictures")
    {
        mpeg2::Sequence il;
        il.progressive = false;
        std::vector<std::vector<u8>> aus;
        for (int frame = 0; frame < 3; frame++) {
            std::vector<u8> au;
            if (frame == 0) au = mpeg2::join({mpeg2::sequence_header(il), mpeg2::gop_header(0)});
            for (const int structure : {1, 2}) {
                mpeg2::Picture p;
                p.type = frame == 0 ? mpeg2::I : mpeg2::P;
                p.temporal_reference = frame;
                p.structure = structure;
                p.top_field_first = true;
                p.progressive_frame = false;
                p.frame_pred_frame_dct = false;
                p.seed = frame;
                p.ac = true;
                const std::vector<u8> pic = mpeg2::picture(il, p);
                au.insert(au.end(), pic.begin(), pic.end());
            }
            aus.push_back(std::move(au));
        }
        const std::vector<std::vector<u8>> lumas = ffmpeg_mpeg2(aus, 96, 64, nullptr, false);
        REQUIRE(lumas.size() == 3);
        // The I frame's fields: the top field's lines its blocks' levels, the bottom field's its own.
        mpeg2::Picture first;
        CHECK(lumas[0][2 * 96 + 3] == mpeg2::block_level(first, 0, 0, 0));
        CHECK(lumas[0][3 * 96 + 3] == mpeg2::block_level(first, 0, 7, 0));
    }
}


// The VC-1 writer's streams (vc1_writer.hpp) as FFmpeg's decoder reads them (the sequence header and entry point its
// configuration, as cellVdec opens it): every picture, in display order, without errors, at the samples each kind
// stands for. Pan-scan windows FFmpeg does not read: not here.
std::vector<std::vector<u8>> ffmpeg_vc1(const std::vector<u8>& config, const std::vector<std::vector<u8>>& aus, int w,
    int h, std::vector<int>* types, AVCodecID id = AV_CODEC_ID_VC1)
{
    const AVCodec* dec = avcodec_find_decoder(id);
    REQUIRE(dec);
    AVCodecContext* av = avcodec_alloc_context3(dec);
    if (id == AV_CODEC_ID_WMV3) { // the simple and main profiles: the picture size from outside the stream
        av->width = av->coded_width = w;
        av->height = av->coded_height = h;
    }
    av->extradata = static_cast<u8*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(av->extradata, config.data(), config.size());
    av->extradata_size = static_cast<int>(config.size());
    av->err_recognition = AV_EF_EXPLODE;
    REQUIRE(avcodec_open2(av, dec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    std::vector<std::vector<u8>> pictures;
    const auto drain = [&] {
        while (avcodec_receive_frame(av, f) == 0) {
            CHECK(f->decode_error_flags == 0);
            CHECK(f->width == w);
            CHECK(f->height == h);
            std::vector<u8> yuv(static_cast<usize>(w * h * 3 / 2));
            for (int r = 0; r < h; r++)
                std::memcpy(yuv.data() + r * w, f->data[0] + r * f->linesize[0], static_cast<usize>(w));
            for (int c = 0; c < 2; c++)
                for (int r = 0; r < h / 2; r++)
                    std::memcpy(yuv.data() + w * h + c * (w * h / 4) + r * (w / 2), f->data[1 + c] + r * f->linesize[1 + c],
                        static_cast<usize>(w / 2));
            pictures.push_back(std::move(yuv));
            if (types) types->push_back(f->pict_type);
            av_frame_unref(f);
        }
    };
    for (const auto& au : aus) {
        std::vector<u8> padded(au);
        padded.resize(au.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        pkt->data = padded.data();
        pkt->size = static_cast<int>(au.size());
        CHECK(avcodec_send_packet(av, pkt) == 0);
        drain();
    }
    avcodec_send_packet(av, nullptr);
    drain();
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return pictures;
}

TEST_CASE("The tests' VC-1 streams decode with FFmpeg", "[hle][vdec-cores]")
{
    namespace v1 = pspocket::hle::test::vc1;
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP("this FFmpeg has no VC-1 decoder");
    const auto join = [](std::initializer_list<std::vector<u8>> parts) {
        std::vector<u8> out;
        for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
        return out;
    };
    v1::Sequence seq;
    v1::Entry entry;
    const std::vector<u8> config = join({v1::sequence_header(seq), v1::entry_point(entry)});
    const v1::DcPicture a = dc_levels(64, 48, 0), c = dc_levels(64, 48, 1), d = dc_levels(64, 48, 2);
    v1::Header plain, ic;
    ic.intensity = true;
    ic.lumscale = 16;
    ic.lumshift = 10;
    // Coded order: I a, P (every macroblock skipped), I c, B (direct) between them, BI d, P with intensity
    // compensation, a skipped P.
    const std::vector<std::vector<u8>> aus = {join({config, v1::intra_frame(a, seq)}),
        v1::skipped_mbs_p_frame(seq, entry, plain, 4, 3), v1::intra_frame(c, seq, entry, plain),
        v1::direct_b_frame(seq, entry, plain, 4, 3), v1::intra_frame(d, seq, entry, plain, true),
        v1::skipped_mbs_p_frame(seq, entry, ic, 4, 3), v1::skipped_frame(seq)};
    std::vector<int> types;
    const std::vector<std::vector<u8>> out = ffmpeg_vc1(config, aus, 64, 48, &types);
    // Display order: I a, P (a), B, BI d (a B picture too: before the anchor coded before it), I c, P (c, mapped),
    // skipped (that again).
    REQUIRE(out.size() == 7);
    const std::vector<u8> sa = v1::samples(a), sc = v1::samples(c), sd = v1::samples(d);
    CHECK(out[0] == sa);
    CHECK(out[1] == sa);
    CHECK(out[3] == sd);
    CHECK(out[4] == sc);
    // The B picture: its anchors' average (the P picture, which is a's samples, and c), rounded up.
    for (usize i = 0; i < sa.size(); i++)
        if (out[2][i] != ((sa[i] + sc[i] + 1) >> 1)) {
            FAIL(std::format("B picture sample {}: {} for ({} + {} + 1) / 2", i, out[2][i], sa[i], sc[i]));
            break;
        }
    // Intensity compensation: the reference's samples (c's: the last anchor) through the standard's tables.
    const int scale = ic.lumscale + 32, shift = ic.lumshift * 64;
    for (usize i = 0; i < sc.size(); i++) {
        const bool luma = i < 64 * 48;
        const int v = luma ? std::clamp((scale * sc[i] + shift + 32) >> 6, 0, 255)
                           : std::clamp((scale * (sc[i] - 128) + 128 * 64 + 32) >> 6, 0, 255);
        if (out[5][i] != v) {
            FAIL(std::format("intensity compensated sample {}: {} for {}", i, out[5][i], v));
            break;
        }
    }
    CHECK(out[6] == out[5]);
    CHECK(types[2] == AV_PICTURE_TYPE_B);
    // The other DC differential table (TRANSDCTAB 1), and the AC tables' choice with no AC coefficient.
    v1::Header other;
    other.transdctab = true;
    other.transacfrm = 2;
    other.transacfrm2 = 1;
    other.rndctrl = true;
    other.halfqp = true;
    const std::vector<std::vector<u8>> second = ffmpeg_vc1(config, {join({config, v1::intra_frame(c, seq, entry, other)})},
        64, 48, nullptr);
    REQUIRE(second.size() == 1);
    CHECK(second[0] == sc);
    // Interlaced pictures (64x32): an interlaced frame's I picture (frame transforms) and a P picture of skipped
    // macroblocks after it; a field picture of two I fields (each of its own DC levels, its lines interleaved).
    v1::Sequence il;
    il.width = 64;
    il.height = 32;
    il.interlace = true;
    const std::vector<u8> il_config = join({v1::sequence_header(il), v1::entry_point(entry)});
    const v1::DcPicture fa = dc_levels(64, 32, 3), top = dc_levels(64, 16, 4), bottom = dc_levels(64, 16, 5);
    const std::vector<std::vector<u8>> frames = ffmpeg_vc1(il_config, {join({il_config, v1::interlaced_intra_frame(fa,
        il, entry, plain)}), v1::interlaced_skipped_p_frame(il, entry, plain, 4, 2)}, 64, 32, nullptr);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == v1::samples(fa));
    CHECK(frames[1] == v1::samples(fa));
    // An interlaced frame's P picture with intensity compensation (its INTCOMP), and with the 4MV switch.
    for (const bool intcomp : {true, false}) {
        v1::Header hp;
        hp.intcomp = intcomp;
        hp.lumscale = 20;
        hp.lumshift = 7;
        hp.fourmvswitch = !intcomp;
        hp.fourmvbptab = 2;
        const std::vector<std::vector<u8>> ps = ffmpeg_vc1(il_config, {join({il_config, v1::interlaced_intra_frame(fa,
            il, entry, plain)}), v1::interlaced_skipped_p_frame(il, entry, hp, 4, 2)}, 64, 32, nullptr);
        REQUIRE(ps.size() == 2);
        for (usize i = 0; i < 64 * 32; i++) {
            const int want = intcomp ? std::clamp((52 * ps[0][i] + 7 * 64 + 32) >> 6, 0, 255) : ps[0][i];
            if (ps[1][i] != want) {
                FAIL(std::format("interlaced P sample {} ({}): {} for {}", i, intcomp ? "intensity compensated" : "4MV",
                    ps[1][i], want));
                break;
            }
        }
    }
    // The simple and main profiles (WMV3 to FFmpeg): an I picture, a P picture of skipped macroblocks, a B picture
    // (direct) after another I picture.
    {
        v1::SimpleSequence mp;
        mp.maxbframes = 1;
        v1::DcPicture ma = dc_levels(64, 48, 6), mc = dc_levels(64, 48, 7);
        for (auto* pic : {&ma, &mc})
            for (auto* plane : {&pic->y, &pic->cb, &pic->cr})
                for (int& v : *plane) v += 100;
        const v1::SimpleHeader sh;
        std::vector<int> mtypes;
        // (This FFmpeg's VC-1 decoder, given STRUCT_C behind a sequence header's start code and an entry point of
        // the same coding tools with the coded size, and the frames escaped as BDUs' payloads, as the core does.)
        std::vector<u8> mconfig = {0x00, 0x00, 0x01, 0x0F};
        const std::vector<u8> mstruct = v1::struct_c(mp);
        mconfig.insert(mconfig.end(), mstruct.begin(), mstruct.end());
        v1::Entry me;
        me.coded_width = 64;
        me.coded_height = 48;
        me.fastuvmc = mp.fastuvmc;
        const std::vector<u8> mentry = v1::entry_point(me);
        mconfig.insert(mconfig.end(), mentry.begin(), mentry.end());
        const auto escaped = [](const std::vector<u8>& raw) {
            std::vector<u8> out;
            int zeros = 0;
            for (const u8 c : raw) {
                if (zeros >= 2 && c <= 3) {
                    out.push_back(3);
                    zeros = 0;
                }
                out.push_back(c);
                zeros = c == 0 ? zeros + 1 : 0;
            }
            return out;
        };
        const std::vector<std::vector<u8>> mo = ffmpeg_vc1(mconfig, {escaped(v1::simple_intra_frame(ma, mp, sh)),
            escaped(v1::simple_skipped_mbs_p_frame(mp, sh, 4, 3)), escaped(v1::simple_intra_frame(mc, mp, sh)),
            escaped(v1::simple_direct_b_frame(mp, sh, 4, 3))}, 64, 48, &mtypes, AV_CODEC_ID_VC1);
        REQUIRE(mo.size() == 4);
        CHECK(mo[0] == v1::simple_samples(ma));
        CHECK(mo[1] == v1::simple_samples(ma));
        const std::vector<u8> mb = v1::simple_samples(mc);
        CHECK(mo[3] == mb);
        CHECK(mtypes[2] == AV_PICTURE_TYPE_B);
    }
    const std::vector<std::vector<u8>> fields = ffmpeg_vc1(il_config, {join({il_config, v1::field_picture(0, top,
        bottom, il, entry, plain)})}, 64, 32, nullptr);
    REQUIRE(fields.size() == 1);
    const std::vector<u8> st = v1::samples(top), sb = v1::samples(bottom);
    for (int r = 0; r < 32; r++)
        for (int x = 0; x < 64; x++) {
            const u8 want = (r & 1 ? sb : st)[static_cast<usize>((r / 2) * 64 + x)];
            if (fields[0][static_cast<usize>(r * 64 + x)] != want) {
                FAIL(std::format("field picture sample {}, {}: {} for {}", x, r, fields[0][static_cast<usize>(r * 64 + x)],
                    want));
                r = 32;
                break;
            }
        }
}

#endif
