// The video codec cores' own APIs against Sony's modules (D-920): each core library called directly, as Sony's libvdec
// (or a game) calls it, with the same synthetic streams through pspocket's core and through Sony's module (the user's
// installed system software), each core in a process of its own per half. Sony's cores decode on SPURS: each half
// makes a SPURS instance and a taskset (pspocket's cellSpurs) and gives the taskset to the core's open. Compared: every
// answer and output structure, the frame buffers' places (as offsets into the core's memory) and their contents (the
// IDCT may round one step apart, as D-912 accepts for DivX and MPEG-4; VC-1 exactly), the pictures' order and the
// errors. The cores' own memory (their state, SPU tasks' areas) is opaque: not compared. Skipped without the firmware
// (compare_util.hpp) and without FFmpeg's encoders. No game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <optional>
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
            if (got != 0 && got != 0x80615540) return;
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

#endif
