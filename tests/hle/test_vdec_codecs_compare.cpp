// cellVdec's newest codecs against Sony's libvdec (D-588 to D-590, D-597, D-912): DivX 3.11, MPEG-4 part 2 (as DivX
// and as MPEG-4), VC-1's advanced profile and MVC, the streams test_vdec_codecs.cpp decodes (vdec_streams.hpp,
// vc1_writer.hpp, h264_writer.hpp), each family in a process of its own per half. Sony's libvdec loads Sony's codec
// cores (libdivx311dec, libdivxdec, libsmvd4, libsvc1d, libmvcdec: firmware-first or Sony's only, D-575). And pspocket's
// codec cores under Sony's libvdec against Sony's (D-920), MPEG-2 among them (mpeg2_writer.hpp's streams).
//
// Each family: QueryAttrEx over levels and codec specific information (and their refusals); OpenEx with the codec's part
// of what was asked for and one byte less; decoders' lives as a game lives them (each access unit's AUDONE, then the
// pictures announced taken at once: decode()), by a game that does not take its pictures (how many a decoder holds
// before it holds an access unit back: hold()), and with items taken ahead of their pictures (items_ahead()). For each
// picture: its item (the frame buffer's address as an offset into the decoder's memory), its information block (the
// codec's size: DivX 0x20, DivX 3.11 0x18, MPEG-4 0x38, VC-1 0x170, MPEG-2 0x1C8), its frame buffer (each core's own layout) and its
// samples; the callbacks in order. The decoder's memory is filled with 0xA5 before each OpenEx, so the bytes a decoder
// leaves unwritten show.
//
// What counts as the same picture (D-597, D-912): VC-1's transforms are exact by the standard, so those pictures and
// frame buffers must be the same sample for sample. DivX and MPEG-4 part 2 use an IDCT the standard lets decoders round
// in their own way (IEEE 1180): their samples may be one step off Sony's (1 to 2% are), never more. A stream decoded at
// a size it was not written for (Life::damaged) is compared but for its pictures and which of them a decoder reports
// damaged (notes). Skipped without the firmware (compare_util.hpp), and the DivX and VC-1 parts where this FFmpeg lacks
// the encoders or the decoder (build-ffmpeg.sh). No game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "h264_writer.hpp"
#include "mpeg2_writer.hpp"
#include "vc1_writer.hpp"
#include "vdec_streams.hpp"
#include "vdec_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace pspocket::hle::test::vdec_test;
namespace v1 = pspocket::hle::test::vc1;
namespace h = pspocket::hle::test::h264;
namespace m2 = pspocket::hle::test::mpeg2;

TEST_CASE("cellVdec's comparison calls Sony's exports by their names; pspocket's library", "[hle][vdec]")
{
    // libvdec is pspocket's own and compared already (D-196): not firmware-first, only its new codecs are new here.
    check_names("libvdec", {"cellVdecQueryAttrEx", "cellVdecOpenEx", "cellVdecSetFrameRate", "cellVdecStartSeq",
                               "cellVdecDecodeAu", "cellVdecEndSeq", "cellVdecGetPicItem", "cellVdecGetPicture",
                               "cellVdecClose"},
        false);
    CHECK_FALSE(hle::registry().is_firmware_first("libvdec"));
}

#if PSPOCKET_HAVE_FFMPEG
using namespace pspocket::hle::test::vdec_streams;

namespace {

constexpr u32 CODEC_MPEG2 = 0, CODEC_MPEG4 = 2, CODEC_VC1 = 3, CODEC_DIVX = 5, CODEC_DIVX3_11 = 9, CODEC_MVC = 11,
              CODEC_MVC2 = 13;

// Sony's information block of each codec (RPCS3's VDEC_SCE_DECODER attributes).
u32 info_size(u32 codec)
{
    switch (codec) {
    case CODEC_MPEG2: return 0x1C8;
    case CODEC_MPEG4: return 0x38;
    case CODEC_DIVX: return 0x20;
    case CODEC_DIVX3_11: return 0x18;
    case CODEC_VC1: return 0x170;
    case CODEC_MVC:
    case CODEC_MVC2: return 0x214;
    default: return 0x244;
    }
}

// The decoder's memory: 64 MB at 0x01000000, so every decoder of these streams gets what QueryAttrEx asks for (VC-1's
// AP L4 asks for the most, 0x3949A7B bytes; Sony's libvdec refuses less than its codec's part of it).
constexpr u32 kMem = 0x01000000, kMemSize = 0x04000000;

struct VdecFixture : Fixture {
    VdecFixture()
    {
        REQUIRE(hle->link_internal_functions({"libvdec"}));
        REQUIRE(hle->main_memory().reserve(kMem, kMemSize));
        REQUIRE(mem.commit(kMem, kMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

std::string in_decoder(u32 addr)
{
    if (addr >= kMem && addr < kMem + kMemSize) return std::format("the decoder's memory +{:#x}", addr - kMem);
    return std::format("{:#x}", addr);
}

using Run = Half<VdecFixture>;

// QueryAttrEx of a codec, level and specific information: its answer and attribute.
void query(Run& r, const std::string& what, u32 codec, u32 level, const std::vector<u8>& spec)
{
    if (!selected(what)) return;
    u8* b = r.b;
    std::memset(b + kArgs, 0, 0x100);
    ps_write32(b, kType, codec);
    ps_write32(b, kType + 4, level);
    if (!spec.empty()) {
        std::memcpy(b + kSpec, spec.data(), spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    std::memset(b + kAttr, 0xEE, 16);
    r.r("cellVdecQueryAttrEx", {kType, kAttr}, "QueryAttrEx " + what);
    r.o.line("QueryAttrEx " + what + ": the attribute", compare::hex(b + kAttr, 16));
}

// A specific information block {thisSize, maxDecodedFrameWidth, maxDecodedFrameHeight} (MPEG-4, VC-1, DivX's first
// form) or H.264's {12, width, height, deblocking, buffers}.
std::vector<u8> spec8(u32 size, u16 width, u16 height)
{
    std::vector<u8> v = {static_cast<u8>(size >> 24), static_cast<u8>(size >> 16), static_cast<u8>(size >> 8),
        static_cast<u8>(size), static_cast<u8>(width >> 8), static_cast<u8>(width), static_cast<u8>(height >> 8),
        static_cast<u8>(height)};
    v.resize(size == 12 ? 12 : 8, 0);
    return v;
}


// How long a half may take for a callback it is due (an access unit decoded, the end of a sequence): Sony's decoders
// run under the interpreters here. A decoder that holds an access unit back (no frame buffer free) shows as "none".
constexpr std::chrono::seconds kDue{10};

bool wait_for(const std::function<bool()>& done, std::chrono::seconds timeout = kDue)
{
    return wait_until(done, timeout);
}

// Until the callbacks stop coming (none for 0.3 s, at most 10 s): the PICOUTs a decoder sends after an AUDONE.
void settle(u8* b)
{
    u32 seen = log_count(b);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int quiet = 0; quiet < 15 && std::chrono::steady_clock::now() < give_up; quiet++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const u32 now = log_count(b);
        if (now != seen) quiet = 0;
        seen = now;
    }
}

// The bytes of a picture's frame buffer the comparison reads (Sony's frame buffer of each codec, D-912): where they
// begin before the item's start address, and how many.
struct Region {
    u32 before = 0, size = 0;
};
Region frame_region(u32 codec, u32 width, u32 height, bool lines16 = false)
{
    const u32 w16 = (width + 15) & ~15u, h16 = (height + 15) & ~15u, hc = ((height + 1) / 2 + 7) & ~7u;
    switch (codec) {
    case CODEC_MPEG2: // libsmvd2's: macroblock after macroblock (16 lines of luma, then 8 of Cb and Cr: 0x180 bytes)
        return {0, (w16 / 16) * (h16 / 16) * 0x180};
    case CODEC_MPEG4: // libsmvd4's strips: the start address is the first luma sample, after the left edge strip
        return {(h16 + 32) * 16 + 16 * 16, (w16 + 32) * (h16 + hc + 48)};
    case CODEC_DIVX:
    case CODEC_DIVX3_11: // libdivxdec's, libdivx311dec's: the start address is the frame's
        return {0, (w16 + 32) * (h16 + hc + 48)};
    case CODEC_VC1: { // libsvc1d's: 64 lines of edge above and below the luma, 32 the chroma, the height to 32 lines
                      // (to 16 for a simple or main profile sequence's frames set up so, `lines16`)
        const u32 hl = lines16 ? h16 : (height + 31) & ~31u;
        return {(hl + 128) * 16 + 64 * 16, (w16 + 32) * (hl + 128) * 3 / 2};
    }
    default: { // libavcdec's (D-196): two strips of edge each side, no lines of edge; the start is the frame's
        const u32 strips = w16 / 16 + 4;
        return {0, strips * 16 * height + 0x400 + strips * 16 * (height / 2) + 0x400};
    }
    }
}

// A decoder's state as the test sees it, through its life.
struct Life {
    Run& r;
    std::string what;
    u32 handle = 0, codec = 0, width = 0, height = 0, taken = 0;
    Rule rule;
    // A stream decoded at a size it was not written for: the pictures are what each decoder makes of a damaged
    // stream (its own concealment), and which of them a decoder reports damaged (the item's status, the information's
    // error word, the PICOUT's data) is its own. Those are notes; the rest (answers, places, sizes) is compared.
    bool damaged = false;
    bool lines16 = false; // VC-1's frames laid out to 16 lines (frame_region)

    // Takes the pictures announced and not yet taken (each item, its places, information, frame buffer, then the
    // samples by GetPicture), at most `limit`; `when` names the step for the answer that ends it (EMPTY).
    void take(const std::string& when, u32 limit = ~0u)
    {
        u8* b = r.b;
        for (u32 n = 0; n < limit; n++) {
            ps_write32(b, kItemOut, 0);
            const std::string key = std::format("{}: picture {}", what, taken);
            const u64 got = r.side.call("cellVdecGetPicItem", {handle, kItemOut}).value_or(~0ull);
            if (got != 0) {
                r.o.line(std::format("{}: {}: GetPicItem", what, when), answer(got));
                return;
            }
            taken++;
            const u32 item = ps_read32(b, kItemOut);
            r.o.line(key + ": the item at", in_decoder(item));
            if (!r.readable(item, 0x4C)) return;
            const u32 start = ps_read32(b, item + 4), info = ps_read32(b, item + 0x48);
            std::vector<u8> fields(b + item, b + item + 0x4C);
            std::memset(fields.data() + 4, 0, 4);    // the frame buffer's address: below
            std::memset(fields.data() + 0x48, 0, 4); // the information's: below
            if (damaged) {
                r.o.note(std::format("{}: the item's status {:#x}", key, ps_read32(b, item + 0x40)));
                std::memset(fields.data() + 0x40, 0, 4);
            }
            r.o.line(key + ": the item", compare::hex(fields));
            r.o.line(key + ": its frame buffer at", in_decoder(start));
            r.o.line(key + ": its information at the item", std::format("{:+#x}", static_cast<s64>(info) - item));
            const u32 info_bytes = info_size(codec);
            if (r.readable(info, info_bytes)) {
                std::vector<u8> block(b + info, b + info + info_bytes);
                const u32 error_word = codec == CODEC_DIVX3_11 ? 0x14 : codec == CODEC_DIVX ? 0x1C : 0;
                if (damaged && error_word) {
                    r.o.note(std::format("{}: the information's error word {:#x}", key, ps_read32(b, info + error_word)));
                    std::memset(block.data() + error_word, 0, 4);
                }
                r.o.line(key + ": the information", compare::hex(block));
            } else {
                r.o.line(key + ": the information", "not in guest memory");
            }
            const Region fb = frame_region(codec, width, height, lines16);
            if (!damaged && start >= kMem + fb.before && start - fb.before + u64{fb.size} <= kMem + kMemSize) {
                std::vector<u8> frame(b + start - fb.before, b + start - fb.before + fb.size);
                // A VC-1 field picture's (its information's two fields): its edge strips' lines above and below the
                // picture left out, which libsvc1d's program pads racing with itself (D-920).
                if (codec == CODEC_VC1 && r.readable(info, info_bytes) && ps_read8(b, info + 0x20) == 2) {
                    const u32 w16 = (width + 15) & ~15u, hl = lines16 ? (height + 15) & ~15u : (height + 31) & ~31u;
                    const u32 lines = hl + 128, strips = w16 / 16 + 2, clines = hl / 2 + 64;
                    for (const u32 strip : {0u, strips - 1}) {
                        for (u32 line = 0; line < lines; line++)
                            if (line < 64 || line >= 64 + hl)
                                std::memset(frame.data() + (strip * lines + line) * 16, 0, 16);
                        for (u32 line = 0; line < clines; line++)
                            if (line < 32 || line >= 32 + hl / 2)
                                std::memset(frame.data() + (strips * lines + strip * clines + line) * 16, 0, 16);
                    }
                }
                r.o.bytes(key + ": its frame buffer", frame.data(), frame.size(), rule);
            }
            ps_write32(b, kFormat, 3); // YUV 4:2:0 planar
            ps_write32(b, kFormat + 4, 0);
            ps_write8(b, kFormat + 8, 0);
            const u32 size = std::min(ps_read32(b, item + 8), 0x1F0000u); // the item's picture size (kPicture holds 2 MB)
            std::memset(b + kPicture, 0xCC, size + 0x100);
            if (r.r("cellVdecGetPicture", {handle, kFormat, kPicture}, key + ": GetPicture") != 0) return;
            if (!damaged) r.o.bytes(key + ": the samples", b + kPicture, size, rule);
            r.o.line(key + ": the 256 bytes past the samples", compare::hex(b + kPicture + size, 256));
        }
    }
};

// CellVdecAuInfoEx2 (D-920 part 3): the two pieces' addresses and sizes, PTS, DTS, user data, codec specific data.
constexpr u32 kAuEx2 = kArgs + 0x1A0;

// Puts access unit `i` of `s` in guest memory at `at` and calls DecodeAu: its answer. A stream sent by DecodeAuEx2
// (Stream::ex2) gives its odd access units in two pieces, as at a ring buffer's end: a first of whole 16-byte blocks
// (libsmvd2's slice program reads on to the next 16-byte boundary, D-920 part 6), the rest at the next 16-byte
// boundary 0x40 bytes on.
u64 send_au(Run& r, const std::string& key, u32 handle, const Stream& s, usize i, u32& at)
{
    u8* b = r.b;
    const std::vector<u8>& au = s.aus[i];
    if (at + au.size() + 0x80 > kPicture) at = kStream;
    REQUIRE(at + au.size() + 0x80 <= kPicture);
    if (s.ex2) {
        const u32 size = static_cast<u32>(au.size()), first = i % 2 ? (size / 2) & ~15u : size;
        const u32 second_at = first < size ? (at + first + 0x40) & ~15u : 0;
        std::memcpy(b + at, au.data(), first);
        if (second_at) std::memcpy(b + second_at, au.data() + first, size - first);
        ps_write32(b, kAuEx2 + 0, at);
        ps_write32(b, kAuEx2 + 4, second_at);
        ps_write32(b, kAuEx2 + 8, first);
        ps_write32(b, kAuEx2 + 12, size - first);
        ps_write32(b, kAuEx2 + 16, 0);
        ps_write32(b, kAuEx2 + 20, 0x1000 + 3003 * static_cast<u32>(i));
        ps_write32(b, kAuEx2 + 24, 0);
        ps_write32(b, kAuEx2 + 28, 0x800 + 3003 * static_cast<u32>(i));
        ps_write64(b, kAuEx2 + 32, 0xABCD0000 + static_cast<u64>(i));
        ps_write64(b, kAuEx2 + 40, 0);
        at = ((second_at ? second_at + size - first : at + size) + 15) & ~15u;
        return r.r("cellVdecDecodeAuEx2", {handle, static_cast<u64>(static_cast<u32>(s.mode)), kAuEx2}, key);
    }
    std::memcpy(b + at, au.data(), au.size());
    ps_write32(b, kAu + 0, at);
    ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
    ps_write32(b, kAu + 8, 0);
    ps_write32(b, kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
    ps_write32(b, kAu + 16, 0);
    ps_write32(b, kAu + 20, 0x800 + 3003 * static_cast<u32>(i));
    ps_write64(b, kAu + 24, 0xABCD0000 + static_cast<u64>(i));
    ps_write64(b, kAu + 32, 0);
    at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
    return r.r("cellVdecDecodeAu", {handle, static_cast<u64>(static_cast<u32>(s.mode)), kAu}, key);
}

// QueryAttrEx and OpenEx of a stream's decoder with the memory asked for; the handle, or 0 (the answers written).
u32 open_decoder(Run& r, const std::string& what, const Stream& s)
{
    u8* b = r.b;
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, s.codec);
    ps_write32(b, kType + 4, s.level);
    if (!s.spec.empty()) {
        std::memcpy(b + kSpec, s.spec.data(), s.spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    std::memset(b + kAttr, 0xEE, 16);
    if (r.r("cellVdecQueryAttrEx", {kType, kAttr}, what + ": QueryAttrEx") != 0) return 0;
    r.o.line(what + ": QueryAttrEx's attribute", compare::hex(b + kAttr, 16));
    const u32 res[6] = {kMem, std::min(ps_read32(b, kAttr), kMemSize), 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    ps_write32(b, kHandle, 0xEEEEEEEE);
    // The decoder's memory as a game may hand it over, not cleared: what a decoder leaves unwritten (a field of an
    // information block it does not set) shows as this pattern, whatever ran there before.
    std::memset(b + kMem, 0xA5, res[1]);
    if (r.r("cellVdecOpenEx", {kType, kRes, kCb, kHandle}, what + ": OpenEx") != 0) return 0;
    const u32 handle = ps_read32(b, kHandle);
    r.o.line(what + ": the handle", in_decoder(handle));
    return handle;
}

void callbacks(Run& r, const std::string& what, bool damaged = false)
{
    u8* b = r.b;
    for (u32 i = 0; i < log_count(b) && i < 255; i++) {
        const u32 type = ps_read32(b, kLog + 16 + 16 * i), data = ps_read32(b, kLog + 20 + 16 * i);
        const bool own = damaged && type == 1; // a damaged picture's PICOUT data: the decoder's own (Life::damaged)
        if (own) r.o.note(std::format("{}: callback {}: PICOUT data {:#x}", what, i, data));
        r.o.line(std::format("{}: callback {}", what, i),
            std::format("type {} data {} handle {}", type, own ? std::string("(a note)") : std::format("{:#x}", data),
                in_decoder(ps_read32(b, kLog + 24 + 16 * i))));
    }
}

// The bytes of libvdec's own ring for a codec (its count of elements and information block size), which QueryAttrEx's
// answer has with the codec's part (D-912): to open a decoder with a given part of what was asked for.
u32 ring_bytes(u32 codec)
{
    const u32 count = codec == CODEC_MVC || codec == CODEC_MVC2 ? 0x30 : 0x20, info = info_size(codec);
    return (info + 0x90) * count + (count * 0x14 + 0x14) * 2 + 0x40;
}

// OpenEx of a decoder with the codec's part of what QueryAttrEx asks for, `less` bytes less (Sony's libvdec refuses
// less than its codec's part, D-912), then Close.
void open_with_less(Run& r, const std::string& what, u32 codec, u32 level, const std::vector<u8>& spec, u32 less)
{
    if (!selected(what)) return;
    u8* b = r.b;
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, codec);
    ps_write32(b, kType + 4, level);
    if (!spec.empty()) {
        std::memcpy(b + kSpec, spec.data(), spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    if (r.side.call("cellVdecQueryAttrEx", {kType, kAttr}).value_or(1) != 0) return;
    const u32 part = ps_read32(b, kAttr) - 0x858 - ring_bytes(codec);
    const u32 res[6] = {kMem, std::min(part - less, kMemSize), 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    std::memset(b + kMem, 0xA5, 0x4000);
    if (r.r("cellVdecOpenEx", {kType, kRes, kCb, kHandle}, "OpenEx " + what) == 0)
        r.r("cellVdecClose", {ps_read32(b, kHandle)}, "Close " + what);
}

// A decoder's life over `s` as a game lives it: each access unit decoded (DecodeAu, its AUDONE), then the pictures
// announced taken at once (GetPicItem, GetPicture), the end of the sequence and the last pictures, the callbacks in
// order, Close.
void decode(Run& r, const std::string& what, const Stream& s, u32 width, u32 height, Rule rule, bool damaged = false,
    const std::vector<s32>& modes = {})
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, s);
    if (!handle) return;
    Life life{r, what, handle, s.codec, width, height, 0, rule, damaged};
    // (A VC-1 stream that begins with the simple and main profiles' sequence command: a new instance's frames, laid
    // out to 16 lines.)
    life.lines16 = s.codec == CODEC_VC1 && !modes.empty() && modes[0] == 3;
    if (s.frame_rate) r.r("cellVdecSetFrameRate", {handle, s.frame_rate}, what + ": SetFrameRate");
    if (r.r("cellVdecStartSeq", {handle}, what + ": StartSeq") == 0) {
        u32 at = kStream, accepted = 0;
        for (usize i = 0; i < s.aus.size(); i++) {
            const std::string key = std::format("{}: DecodeAu {}", what, i);
            Stream one = s;
            if (i < modes.size()) one.mode = modes[i];
            if (send_au(r, key, handle, one, i, at) != 0) continue;
            if (s.codec == CODEC_VC1 && one.mode == 3) {
                // The simple and main profiles' sequence command: libvdec sends no AUDONE for it.
                settle(b);
                r.o.line(key + ": AUDONEs by then", std::format("{}", count_of(b, 0)));
                continue;
            }
            accepted++;
            r.o.line(key + ": AUDONE", wait_for([&] { return count_of(b, 0) >= accepted; }) ? "yes" : "none");
            settle(b);
            life.take(std::format("after DecodeAu {}", i));
        }
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        r.o.line(what + ": SEQDONE", wait_for([&] { return count_of(b, 2) >= 1; }) ? "yes" : "none");
        settle(b);
        life.take("after EndSeq");
        callbacks(r, what, damaged);
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
}

// A decoder's life over `s` by a game that does not take its pictures: each access unit decoded until one is held back
// (no AUDONE: the decoder has no frame buffer free), then one picture taken at a time until it is done; how many
// pictures the decoder can hold for the game, with which callbacks.
void hold(Run& r, const std::string& what, const Stream& s, u32 width, u32 height, Rule rule)
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, s);
    if (!handle) return;
    Life life{r, what, handle, s.codec, width, height, 0, rule, false};
    if (r.r("cellVdecStartSeq", {handle}, what + ": StartSeq") == 0) {
        u32 at = kStream, accepted = 0;
        for (usize i = 0; i < s.aus.size(); i++) {
            const std::string key = std::format("{}: DecodeAu {}", what, i);
            if (send_au(r, key, handle, s, i, at) != 0) continue;
            accepted++;
            for (u32 tries = 0;; tries++) {
                const bool done = wait_for([&] { return count_of(b, 0) >= accepted; });
                settle(b);
                r.o.line(std::format("{}: AUDONE ({} taken)", key, life.taken), done ? "yes" : "none");
                r.o.line(std::format("{}: pictures announced ({} taken)", key, life.taken),
                    std::format("{}", count_of(b, 1)));
                if (done || tries > 8) break;
                const u32 before = life.taken;
                life.take(std::format("held at DecodeAu {}", i), 1);
                if (life.taken == before) break; // nothing to take: the decoder is stuck
            }
        }
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        for (u32 tries = 0;; tries++) {
            const bool done = wait_for([&] { return count_of(b, 2) >= 1; });
            settle(b);
            r.o.line(std::format("{}: SEQDONE ({} taken)", what, life.taken), done ? "yes" : "none");
            if (done || tries > 8) break;
            const u32 before = life.taken;
            life.take("held at EndSeq", 1);
            if (life.taken == before) break;
        }
        life.take("after EndSeq");
        callbacks(r, what);
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
}

// Items taken ahead of their pictures: two access units decoded and their pictures left, then GetPicItem three times
// (the items come in turn, then EMPTY) and GetPicture three times (the pictures in turn, then EMPTY); and SetFrameRate
// inside the sequence (the frame rate code in the information blocks after it).
void items_ahead(Run& r, const std::string& what, const Stream& s)
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, s);
    if (!handle) return;
    if (r.r("cellVdecStartSeq", {handle}, what + ": StartSeq") == 0) {
        r.r("cellVdecSetFrameRate", {handle, 0x84}, what + ": SetFrameRate 30000/1001 in the sequence");
        u32 at = kStream, accepted = 0;
        for (usize i = 0; i < 2 && i < s.aus.size(); i++) {
            const std::string key = std::format("{}: DecodeAu {}", what, i);
            if (send_au(r, key, handle, s, i, at) != 0) continue;
            accepted++;
            r.o.line(key + ": AUDONE", wait_for([&] { return count_of(b, 0) >= accepted; }) ? "yes" : "none");
        }
        settle(b);
        r.o.line(what + ": pictures announced", std::format("{}", count_of(b, 1)));
        for (u32 k = 0; k < 3; k++) {
            ps_write32(b, kItemOut, 0);
            const std::string key = std::format("{}: GetPicItem {}", what, k);
            if (r.r("cellVdecGetPicItem", {handle, kItemOut}, key) != 0) continue;
            const u32 item = ps_read32(b, kItemOut);
            r.o.line(key + ": the item at", in_decoder(item));
            r.o.line(key + ": its information", compare::hex(b + ps_read32(b, item + 0x48), info_size(s.codec)));
        }
        ps_write32(b, kFormat, 3);
        ps_write32(b, kFormat + 4, 0);
        ps_write8(b, kFormat + 8, 0);
        for (u32 k = 0; k < 3; k++) r.r("cellVdecGetPicture", {handle, kFormat, 0}, std::format("{}: GetPicture {}", what, k));
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        r.o.line(what + ": SEQDONE", wait_for([&] { return count_of(b, 2) >= 1; }) ? "yes" : "none");
        settle(b);
        for (u32 k = 0;; k++) {
            ps_write32(b, kItemOut, 0);
            if (r.r("cellVdecGetPicItem", {handle, kItemOut}, std::format("{}: the last, GetPicItem {}", what, k)) != 0)
                break;
            r.r("cellVdecGetPicture", {handle, kFormat, 0}, std::format("{}: the last, GetPicture {}", what, k));
        }
        callbacks(r, what);
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
}

Stream stream(u32 codec, u32 level, std::vector<std::vector<u8>> aus, std::vector<u8> spec = {}, u32 frame_rate = 0,
    s32 mode = 0)
{
    Stream s;
    s.codec = codec;
    s.level = level;
    s.aus = std::move(aus);
    s.spec = std::move(spec);
    s.frame_rate = frame_rate;
    s.mode = mode;
    return s;
}

constexpr const char* kNoDivx = "this FFmpeg has no msmpeg4v3 or mpeg4 encoder (the tests' build: build-ffmpeg.sh)";
constexpr const char* kNoVc1 = "this FFmpeg has no VC-1 decoder (build-ffmpeg.sh again)";

Observed divx311(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    // The sizes: without the codec specific information, with it, and its checks (RPCS3's query_attr_divx).
    query(r, "DivX 3.11 level 15", CODEC_DIVX3_11, 15, {});
    query(r, "DivX 3.11 level 14", CODEC_DIVX3_11, 14, {});
    query(r, "DivX 3.11 1920x1088, 2 buffers", CODEC_DIVX3_11, 15, divx_info2(12, 1920, 1088, 2));
    query(r, "DivX 3.11 96x64", CODEC_DIVX3_11, 15, divx_info2(12, 96, 64, 0));
    query(r, "DivX 3.11 with DivX's first information form", CODEC_DIVX3_11, 15, divx_info2(8, 96, 64, 0));
    query(r, "DivX 3.11 1921 wide", CODEC_DIVX3_11, 15, divx_info2(12, 1921, 64, 0));
    query(r, "DivX 3.11 1089 high", CODEC_DIVX3_11, 15, divx_info2(12, 96, 1089, 0));
    query(r, "DivX 3.11 1 buffer", CODEC_DIVX3_11, 15, divx_info2(12, 96, 64, 1));
    for (const auto& [w, h] : {std::pair{16, 16}, {15, 16}, {16, 15}, {720, 576}, {721, 64}, {1920, 64}, {64, 1088},
             {0, 64}, {64, 0}, {352, 288}})
        query(r, std::format("DivX 3.11 {}x{}", w, h), CODEC_DIVX3_11, 15, divx_info2(12, static_cast<u16>(w),
            static_cast<u16>(h), 2));
    for (const u32 level : {0u, 10u, 16u}) query(r, std::format("DivX 3.11 level {}", level), CODEC_DIVX3_11, level, {});
    for (const u32 less : {0u, 1u})
        open_with_less(r, std::format("DivX 3.11 with {} bytes less than its codec's part", less), CODEC_DIVX3_11, 15,
            divx_info2(12, 96, 64, 0), less);
    const std::vector<std::vector<u8>> packets = encode(AV_CODEC_ID_MSMPEG4V3, 96, 64, 6, 3);
    const Rule rule = Rule::within(1);
    decode(r, "DivX 3.11 96x64, six pictures", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 96, 64, 0)), 96, 64,
        rule);
    decode(r, "DivX 3.11 96x64, SetFrameRate 25", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 96, 64, 0), 0x82),
        96, 64, rule);
    // The frame size given smaller and larger than the stream's (where libdivx311dec takes it from).
    decode(r, "DivX 3.11 96x64 told 64x48", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 64, 48, 0)), 96, 64, rule);
    decode(r, "DivX 3.11 96x64 told 128x96", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 128, 96, 0)), 96, 64,
        rule, true);
    // A game that does not take its pictures: how many the decoder holds before it holds an access unit back.
    hold(r, "DivX 3.11 96x64, pictures not taken", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 96, 64, 0)), 96,
        64, rule);
    items_ahead(r, "DivX 3.11 96x64, items ahead of their pictures",
        stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 96, 64, 0)));
    // Not sent: OpenEx without the codec specific information. Sony's libvdec then reads the frame size from address
    // 4 (its pointer to the information is a register left at 0, D-912), which faults on a PS3 as here: a game always
    // gives it. pspocket's decoder assumes 720x576 then (test_vdec_codecs.cpp).
    return std::move(r.o);
}

// The first packet of FFmpeg's MPEG-4 stream with its visual object header replaced by one with a video signal type and
// colour description (FFmpeg's encoder writes none).
std::vector<u8> with_colour(const std::vector<u8>& packet, u8 primaries, u8 transfer, u8 matrix)
{
    const auto code_at = [&](u8 code, usize from) {
        for (usize i = from; i + 3 < packet.size(); i++)
            if (packet[i] == 0 && packet[i + 1] == 0 && packet[i + 2] == 1 && packet[i + 3] == code) return i;
        return packet.size();
    };
    const usize at = code_at(0xB5, 0), next = code_at(0x00, at + 4);
    REQUIRE(next < packet.size());
    // is_visual_object_identifier 1, verid 1, priority 1, type video, video_signal_type 1, format 5, range 0,
    // colour_description 1, the three codes, then a 0 and 1s to the byte.
    const u64 bits = (u64{1} << 47) | (u64{1} << 43) | (u64{1} << 40) | (u64{1} << 36) | (u64{1} << 35) |
                     (u64{5} << 32) | (u64{1} << 30) | (u64{primaries} << 22) | (u64{transfer} << 14) |
                     (u64{matrix} << 6) | 0x1F;
    std::vector<u8> out(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(at + 4));
    for (int k = 5; k >= 0; k--) out.push_back(static_cast<u8>(bits >> (8 * k)));
    out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(next), packet.end());
    return out;
}

Observed mpeg4(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    query(r, "MPEG-4 SP L1", CODEC_MPEG4, 0, {});
    query(r, "MPEG-4 SP L1 96x64", CODEC_MPEG4, 0, spec8(8, 96, 64));
    query(r, "DivX QMOBILE", CODEC_DIVX, 10, {});
    query(r, "DivX QMOBILE 96x64", CODEC_DIVX, 10, spec8(8, 96, 64));
    query(r, "DivX QMOBILE 96x64, 0 buffers", CODEC_DIVX, 10, divx_info2(12, 96, 64, 0));
    for (u32 level = 0; level <= 6; level++) {
        query(r, std::format("MPEG-4 level {}", level), CODEC_MPEG4, level, {});
        query(r, std::format("MPEG-4 level {} 176x144", level), CODEC_MPEG4, level, spec8(8, 176, 144));
        query(r, std::format("MPEG-4 level {} 720x576", level), CODEC_MPEG4, level, spec8(8, 720, 576));
        query(r, std::format("MPEG-4 level {} 80x48", level), CODEC_MPEG4, level, spec8(8, 80, 48));
    }
    query(r, "MPEG-4 SP L1 0x64", CODEC_MPEG4, 0, spec8(8, 0, 64));
    query(r, "MPEG-4 SP L1 177x144", CODEC_MPEG4, 0, spec8(8, 177, 144));
    query(r, "MPEG-4 SP L1, the information 12 bytes", CODEC_MPEG4, 0, spec8(12, 96, 64));
    for (u32 level = 9; level <= 15; level++) {
        query(r, std::format("DivX level {}", level), CODEC_DIVX, level, {});
        query(r, std::format("DivX level {} 176x144, 2 buffers", level), CODEC_DIVX, level, divx_info2(12, 176, 144, 2));
    }
    for (const u16 n : {1, 2, 3, 4, 5})
        query(r, std::format("DivX HD 720 1280x720, {} buffers", n), CODEC_DIVX, 13, divx_info2(12, 1280, 720, n));
    query(r, "DivX QMOBILE 177x144", CODEC_DIVX, 10, divx_info2(12, 177, 144, 0));
    query(r, "DivX QMOBILE 176x145", CODEC_DIVX, 10, divx_info2(12, 176, 145, 0));
    query(r, "DivX QMOBILE 0x64", CODEC_DIVX, 10, divx_info2(12, 0, 64, 0));
    query(r, "DivX QMOBILE, the information 10 bytes", CODEC_DIVX, 10, divx_info2(10, 96, 64, 0));
    for (const u32 less : {0u, 1u}) {
        open_with_less(r, std::format("DivX with {} bytes less than its codec's part", less), CODEC_DIVX, 10, {}, less);
        open_with_less(r, std::format("MPEG-4 with {} bytes less than its codec's part", less), CODEC_MPEG4, 0, {}, less);
    }
    for (const AVRational sar : {AVRational{12, 11}, AVRational{8, 9}}) {
        const std::vector<std::vector<u8>> packets =
            encode(AV_CODEC_ID_MPEG4, 96, 64, 4, 2, [&](AVCodecContext* av) { av->sample_aspect_ratio = sar; });
        for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4})
            decode(r, std::format("{} 96x64, pixels {}:{}", codec == CODEC_DIVX ? "DivX" : "MPEG-4", sar.num, sar.den),
                stream(codec, codec == CODEC_DIVX ? 10 : 0, packets), 96, 64, Rule::within(1));
        if (sar.num != 12) continue;
        // With the codec specific information: the frame buffers sized for the stream (where they are, how many).
        decode(r, "DivX 96x64 told 96x64, 3 buffers", stream(CODEC_DIVX, 10, packets, divx_info2(12, 96, 64, 3)), 96, 64,
            Rule::within(1));
        decode(r, "MPEG-4 96x64 told 96x64", stream(CODEC_MPEG4, 0, packets, spec8(8, 96, 64)), 96, 64, Rule::within(1));
        hold(r, "DivX 96x64 told 96x64, 3 buffers, pictures not taken",
            stream(CODEC_DIVX, 10, packets, divx_info2(12, 96, 64, 3)), 96, 64, Rule::within(1));
        items_ahead(r, "DivX 96x64, items ahead of their pictures", stream(CODEC_DIVX, 10, packets));
        items_ahead(r, "MPEG-4 96x64, items ahead of their pictures", stream(CODEC_MPEG4, 0, packets));
        for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4})
            hold(r, std::format("{} 96x64, pictures not taken", codec == CODEC_DIVX ? "DivX" : "MPEG-4"),
                stream(codec, codec == CODEC_DIVX ? 10 : 0, packets), 96, 64, Rule::within(1));
    }
    // A size that is not a multiple of 32 lines (libsmvd4 aligns its frames' height to 32), as DivX and as MPEG-4.
    const std::vector<std::vector<u8>> small = encode(AV_CODEC_ID_MPEG4, 80, 48, 3, 3);
    decode(r, "DivX 80x48", stream(CODEC_DIVX, 10, small), 80, 48, Rule::within(1));
    decode(r, "MPEG-4 80x48", stream(CODEC_MPEG4, 0, small), 80, 48, Rule::within(1));
    // D-920: an interlaced layer (DivX), a visual object with a colour description (libdivxdec reports none). Not yet
    // here: B pictures (cellVdec's own frame buffer and picture handling is not libvdec's with libdivxdec's and
    // libsmvd4's then), MPEG-4's interlaced pictures (libsmvd4 reports them in error, undecoded).
    const std::vector<std::vector<u8>> interlaced = encode(AV_CODEC_ID_MPEG4, 96, 64, 4, 4,
        [](AVCodecContext* av) { av->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME; });
    std::vector<std::vector<u8>> coloured = encode(AV_CODEC_ID_MPEG4, 96, 64, 4, 4);
    coloured[0] = with_colour(coloured[0], 1, 6, 5);
    decode(r, "DivX 96x64 interlaced", stream(CODEC_DIVX, 10, interlaced), 96, 64, Rule::within(1));
    for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4})
        decode(r, std::format("{} 96x64 with a colour description", codec == CODEC_DIVX ? "DivX" : "MPEG-4"),
            stream(codec, codec == CODEC_DIVX ? 10 : 0, coloured), 96, 64, Rule::within(1));
    if (s.ours_on_firmware) {
        // The cores under Sony's libvdec only (D-920): B pictures, which libsmvd4 does not decode (their frame buffers
        // as they were); libsmvd4's interlaced pictures (in error, undecoded) and quarter sample motion (taken as half
        // sample motion).
        const std::vector<std::vector<u8>> with_b =
            encode(AV_CODEC_ID_MPEG4, 96, 64, 7, 6, [](AVCodecContext* av) { av->max_b_frames = 2; });
        const std::vector<std::vector<u8>> qpel =
            encode(AV_CODEC_ID_MPEG4, 96, 64, 4, 4, [](AVCodecContext* av) { av->flags |= AV_CODEC_FLAG_QPEL; });
        for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4})
            decode(r, std::format("{} 96x64 with B pictures", codec == CODEC_DIVX ? "DivX" : "MPEG-4"),
                stream(codec, codec == CODEC_DIVX ? 10 : 0, with_b), 96, 64, Rule::within(1));
        decode(r, "MPEG-4 96x64 interlaced", stream(CODEC_MPEG4, 0, interlaced), 96, 64, Rule::within(1));
        decode(r, "MPEG-4 96x64 quarter samples", stream(CODEC_MPEG4, 0, qpel), 96, 64, Rule::within(1));
    }
    return std::move(r.o);
}

Observed vc1_family(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    for (u32 level = 0; level <= 10; level++) query(r, std::format("VC-1 level {}", level), CODEC_VC1, level, {});
    query(r, "VC-1 AP L1 64x48", CODEC_VC1, 6, spec8(8, 64, 48));
    query(r, "VC-1 AP L4 1920x1088", CODEC_VC1, 9, spec8(8, 1920, 1088));
    for (u32 level = 0; level <= 9; level++) {
        query(r, std::format("VC-1 level {} 176x144", level), CODEC_VC1, level, spec8(8, 176, 144));
        query(r, std::format("VC-1 level {} 720x576", level), CODEC_VC1, level, spec8(8, 720, 576));
        query(r, std::format("VC-1 level {} 100x60", level), CODEC_VC1, level, spec8(8, 100, 60));
    }
    query(r, "VC-1 AP L1 0x48", CODEC_VC1, 6, spec8(8, 0, 48));
    query(r, "VC-1 AP L1 64x0", CODEC_VC1, 6, spec8(8, 64, 0));
    query(r, "VC-1 AP L1 721x576", CODEC_VC1, 6, spec8(8, 721, 576));
    query(r, "VC-1 AP L1, the information 12 bytes", CODEC_VC1, 6, spec8(12, 64, 48));
    for (const u32 less : {0u, 1u})
        open_with_less(r, std::format("VC-1 with {} bytes less than its codec's part", less), CODEC_VC1, 6,
            spec8(8, 64, 48), less);
    // The simple and main profiles: what OpenEx says (pspocket refuses them, FATAL).
    for (u32 level = 0; level < 5; level++) {
        u8* b = r.b;
        write_callback(b);
        std::memset(b + kArgs, 0, 0x200);
        ps_write32(b, kType, CODEC_VC1);
        ps_write32(b, kType + 4, level);
        r.side.call("cellVdecQueryAttrEx", {kType, kAttr});
        const u32 res[6] = {kMem, std::min(ps_read32(b, kAttr), kMemSize), 1000, 0x4000, 200, 1};
        for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
        ps_write32(b, kCb, kOpd);
        ps_write32(b, kCb + 4, kLog);
        if (r.r("cellVdecOpenEx", {kType, kRes, kCb, kHandle}, std::format("OpenEx VC-1 level {}", level)) == 0)
            r.r("cellVdecClose", {ps_read32(b, kHandle)}, std::format("Close VC-1 level {}", level));
    }
    // test_vdec_codecs.cpp's streams: I, skipped, I, skipped with a display extension; range mapping and pulldown;
    // access units before the headers; a new sequence header; the decode modes.
    v1::Sequence seq;
    seq.display_width = 64;
    seq.display_height = 48;
    seq.aspect_ratio = 15;
    seq.aspect_width = 4;
    seq.aspect_height = 3;
    seq.framerate_nr = 2;
    seq.framerate_dr = 1;
    seq.color_prim = 1;
    seq.transfer_char = 1;
    seq.matrix_coef = 6;
    const v1::Entry entry;
    const v1::DcPicture a = dc_levels(64, 48, 0), c = dc_levels(64, 48, 1);
    const std::vector<u8> headers = concat({v1::sequence_header(seq), v1::entry_point(entry)});
    decode(r, "VC-1 64x48 I, skipped, I, skipped",
        stream(CODEC_VC1, 6,
            {concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq), concat({headers, v1::intra_frame(c, seq)}),
                v1::skipped_frame(seq)}),
        64, 48, Rule::exact());
    {
        v1::Sequence pull;
        pull.pulldown = true;
        pull.tfcntrflag = true;
        v1::Entry mapped;
        mapped.range_mapy = 7;
        mapped.range_mapuv = 1;
        const std::vector<u8> hd = concat({v1::sequence_header(pull), v1::entry_point(mapped)});
        decode(r, "VC-1 64x48 with range mapping and pulldown",
            stream(CODEC_VC1, 9, {concat({hd, v1::intra_frame(dc_levels(64, 48, 2), pull, 2)})}), 64, 48, Rule::exact());
    }
    const v1::Sequence plain;
    const std::vector<u8> plain_headers = concat({v1::sequence_header(plain), v1::entry_point(entry)});
    const v1::DcPicture d = dc_levels(64, 48, 3);
    decode(r, "VC-1 64x48 with an access unit before the headers",
        stream(CODEC_VC1, 5,
            {v1::skipped_frame(plain), concat({plain_headers, v1::intra_frame(d, plain)}), v1::skipped_frame(plain)}),
        64, 48, Rule::exact());
    {
        v1::Sequence small;
        small.width = small.height = 32;
        // Each picture's size: the stream's first is 64x48, then 32x32 (the frame buffer compared at the first's).
        decode(r, "VC-1 a 64x48 sequence header, then a 32x32 one",
            stream(CODEC_VC1, 5,
                {concat({plain_headers, v1::intra_frame(d, plain)}),
                    concat({v1::sequence_header(small), v1::entry_point(entry),
                        v1::intra_frame(dc_levels(32, 32, 4), small)})}),
            64, 48, Rule::exact());
    }
    hold(r, "VC-1 64x48, pictures not taken",
        stream(CODEC_VC1, 6,
            {concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq), concat({headers, v1::intra_frame(c, seq)}),
                v1::skipped_frame(seq), concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq)}),
        64, 48, Rule::exact());
    decode(r, "VC-1 64x48 told 64x48",
        stream(CODEC_VC1, 6, {concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq)}, spec8(8, 64, 48)), 64,
        48, Rule::exact());
    decode(r, "VC-1 64x48 in mode 3 (the simple and main profiles' sequence command)",
        stream(CODEC_VC1, 5, {concat({plain_headers, v1::intra_frame(d, plain)})}, {}, 0, 3), 64, 48, Rule::exact());
    decode(r, "VC-1 64x48 in mode 2", stream(CODEC_VC1, 5, {concat({plain_headers, v1::intra_frame(d, plain)})}, {}, 0, 2),
        64, 48, Rule::exact());
    if (s.ours_on_firmware) {
        // libsvc1d under Sony's libvdec only (D-920 part 7): pictures of every kind (P, B, BI, skipped, intensity
        // compensation), interlaced frames and field pictures; and the simple and main profiles.
        const v1::Header h0;
        v1::Header ic;
        ic.intensity = true;
        ic.lumscale = 16;
        ic.lumshift = 10;
        decode(r, "VC-1 64x48 I, P, I, B, BI, P, skipped",
            stream(CODEC_VC1, 5,
                {concat({plain_headers, v1::intra_frame(dc_levels(64, 48, 5), plain, entry, h0)}),
                    v1::skipped_mbs_p_frame(plain, entry, h0, 4, 3),
                    v1::intra_frame(dc_levels(64, 48, 6), plain, entry, h0), v1::direct_b_frame(plain, entry, h0, 4, 3),
                    v1::intra_frame(dc_levels(64, 48, 7), plain, entry, h0, true),
                    v1::skipped_mbs_p_frame(plain, entry, ic, 4, 3), v1::skipped_frame(plain, entry, h0)}),
            64, 48, Rule::exact());
        v1::Sequence si;
        si.width = 64;
        si.height = 32;
        si.interlace = true;
        const std::vector<u8> hdi = concat({v1::sequence_header(si), v1::entry_point(entry, si)});
        const auto fl = [](int seed) { return dc_levels(64, 16, seed); };
        decode(r, "VC-1 64x32 interlaced frames",
            stream(CODEC_VC1, 5,
                {concat({hdi, v1::interlaced_intra_frame(dc_levels(64, 32, 8), si, entry, h0)}),
                    v1::interlaced_skipped_p_frame(si, entry, h0, 4, 2),
                    v1::interlaced_intra_frame(dc_levels(64, 32, 9), si, entry, h0),
                    v1::interlaced_direct_b_frame(si, entry, h0, 4, 2), v1::skipped_frame(si, entry, h0)}),
            64, 32, Rule::exact());
        decode(r, "VC-1 64x32 field pictures",
            stream(CODEC_VC1, 5,
                {concat({hdi, v1::field_picture(0, fl(10), fl(11), si, entry, h0)}),
                    v1::field_picture(1, fl(12), fl(13), si, entry, h0),
                    v1::field_picture(3, fl(0), fl(0), si, entry, h0), v1::field_picture(4, fl(0), fl(0), si, entry, h0)}),
            64, 32, Rule::exact());
        // The simple and main profiles: DecodeAu's mode 3 is their sequence's command. Sony's libvdec reads four words
        // at the access unit's address: the height, the width (libsvc1d's SetPictureSize takes them), the sequence
        // header's address and size (DecodeSeqHeader: STRUCT_C behind a sequence header's start code); the frames
        // follow as BDUs of their own (a frame's start code, escaped). The decoder's instance is new: its frames laid
        // out to 16 lines.
        constexpr u32 kSequenceBytes = kArgs + 0x800;
        const auto bdu = [](u8 code, const std::vector<u8>& payload) {
            std::vector<u8> out = {0x00, 0x00, 0x01, code};
            int zeros = 0;
            for (const u8 c : payload) {
                if (zeros >= 2 && c <= 3) {
                    out.push_back(3);
                    zeros = 0;
                }
                out.push_back(c);
                zeros = c == 0 ? zeros + 1 : 0;
            }
            return out;
        };
        const auto command = [&](u32 width, u32 height, const std::vector<u8>& sequence) {
            std::memcpy(r.b + kSequenceBytes, sequence.data(), sequence.size());
            std::vector<u8> au(16);
            const u32 words[4] = {height, width, kSequenceBytes, static_cast<u32>(sequence.size())};
            for (u32 k = 0; k < 4; k++) ps_write32(au.data(), 4 * k, words[k]);
            return au;
        };
        // (`damaged`: the pictures left out, as a sync markers sequence's are, D-920.)
        const auto simple = [&](const std::string& what, u32 level, const v1::SimpleSequence& ms,
                                const v1::SimpleHeader& mh, bool framed = true, u32 width = 64, u32 height = 48,
                                bool damaged = false) {
            if (!selected(what)) return;
            const int w = static_cast<int>(std::min(width, 64u)), h = static_cast<int>(std::min(height, 64u));
            const auto levels = [&](int seed) {
                v1::DcPicture p = dc_levels(w, h, seed);
                for (auto* plane : {&p.y, &p.cb, &p.cr})
                    for (int& v : *plane) v += 100;
                return p;
            };
            const std::vector<u8> sc = v1::struct_c(ms);
            std::vector<std::vector<u8>> aus = {command(width, height, framed ? bdu(0x0F, sc) : sc),
                bdu(0x0D, v1::simple_intra_frame(levels(20), ms, mh)),
                bdu(0x0D, v1::simple_skipped_mbs_p_frame(ms, mh, w / 16, h / 16)),
                bdu(0x0D, v1::simple_intra_frame(levels(21), ms, mh))};
            if (ms.maxbframes) {
                aus.push_back(bdu(0x0D, v1::simple_direct_b_frame(ms, mh, w / 16, h / 16)));
                aus.push_back(bdu(0x0D, v1::simple_intra_frame(levels(22), ms, mh, true)));
            }
            v1::SimpleHeader mi = mh;
            mi.intensity = true;
            mi.lumscale = 16;
            mi.lumshift = 10;
            aus.push_back(bdu(0x0D, v1::simple_skipped_mbs_p_frame(ms, mi, w / 16, h / 16)));
            decode(r, what, stream(CODEC_VC1, level, aus), static_cast<u32>(w), static_cast<u32>(h), Rule::exact(),
                damaged, {3});
        };
        v1::SimpleSequence main_profile, simple_profile, with_b, reduced;
        simple_profile.profile = 0;
        with_b.maxbframes = 1;
        reduced.rangered = true;
        v1::SimpleHeader reduced_frames;
        reduced_frames.rangeredfrm = true;
        simple("VC-1 main profile 64x48", 2, main_profile, {});
        simple("VC-1 main profile 64x64", 3, main_profile, {}, true, 64, 64);
        simple("VC-1 simple profile 64x48", 0, simple_profile, {});
        simple("VC-1 main profile 64x48 with B pictures", 3, with_b, {});
        // Range reduction: the main profile's tool (libsvc1d warns of it at the other profiles' levels, so libvdec
        // reports those pictures in error).
        simple("VC-1 main profile 64x48, range reduction", 2, reduced, reduced_frames);
        simple("VC-1 main profile 64x48, range reduction at the simple profile's level", 1, reduced, reduced_frames);
        simple("VC-1 main profile 64x48, range reduction at an advanced profile's level", 5, reduced, reduced_frames);
        // (No start code: libsvc1d's default sequence, of sync markers.)
        simple("VC-1 main profile 64x48, STRUCT_C without its start code", 2, main_profile, {}, false, 64, 48, true);
        simple("VC-1 main profile, a size past the level's", 2, main_profile, {}, true, 1920, 1088);
    }
    return std::move(r.o);
}

Observed mvc(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    for (const u32 codec : {CODEC_MVC, CODEC_MVC2}) {
        query(r, std::format("codec {} level 21", codec), codec, 21, {});
        query(r, std::format("codec {} level 21 64x64", codec), codec, 21, spec8(12, 64, 64));
    }
    // MVC's sizes: libmvcdec's mvcDecQueryMemory over its levels, picture sizes, buffer counts and deblocking.
    for (const u32 level : {9u, 10u, 11u, 12u, 13u, 14u, 20u, 21u, 22u, 30u, 31u, 32u, 40u, 41u, 42u, 43u, 1042u}) {
        query(r, std::format("MVC level {}", level), CODEC_MVC, level, {});
        query(r, std::format("MVC level {} 176x144", level), CODEC_MVC, level, spec8(12, 176, 144));
        query(r, std::format("MVC level {} 1920x1088", level), CODEC_MVC, level, spec8(12, 1920, 1088));
    }
    const auto mvc_spec = [](u16 w, u16 h, u8 deblock, u8 buffers) {
        std::vector<u8> v = spec8(12, w, h);
        v[8] = deblock;
        v[9] = buffers;
        return v;
    };
    for (const u8 buffers : {1, 2, 4, 6, 40})
        query(r, std::format("MVC level 31 720x480, {} buffers", buffers), CODEC_MVC, 31, mvc_spec(720, 480, 0, buffers));
    for (const u8 deblock : {1, 2, 3})
        query(r, std::format("MVC level 31 720x480, deblocking {}", deblock), CODEC_MVC, 31, mvc_spec(720, 480, deblock, 0));
    query(r, "MVC level 21 48x48", CODEC_MVC, 21, spec8(12, 48, 48));
    query(r, "MVC level 21 2000x64", CODEC_MVC, 21, spec8(12, 2000, 64));
    query(r, "MVC level 21, the information 8 bytes", CODEC_MVC, 21, spec8(8, 64, 64));
    for (const u32 less : {0u, 1u})
        open_with_less(r, std::format("MVC with {} bytes less than its codec's part", less), CODEC_MVC, 21,
            spec8(12, 64, 64), less);
    std::vector<std::vector<u8>> two_views, apart;
    for (int i = 0; i < 4; i++)
        two_views.push_back(h::mvc_access_unit(h::pattern(64, 64, i), h::pattern(64, 64, 10 + i), static_cast<u32>(i & 1)));
    for (int i = 0; i < 2; i++) {
        apart.push_back(h::access_unit(h::pattern(64, 64, i), static_cast<u32>(i)));
        apart.push_back(h::second_view_only(h::pattern(64, 64, 10 + i), static_cast<u32>(i)));
    }
    for (const u32 codec : {CODEC_MVC, CODEC_MVC2})
        decode(r, std::format("codec {}: four access units of two views", codec), stream(codec, 21, two_views), 64, 64,
            Rule::exact());
    decode(r, "MVC: the views in access units apart", stream(CODEC_MVC, 21, apart), 64, 64, Rule::exact());
    hold(r, "MVC: pictures not taken", stream(CODEC_MVC, 21, two_views), 64, 64, Rule::exact());
    // Not sent: MVC through DecodeAuEx2. Sony's libvdec 4.92 hands libmvcdec the picture information's address from
    // 0x10 bytes past where it keeps it (the caller's r26, saved in its frame), and writes the picture's information
    // there when the picture comes: on a PS3 that is wherever the game's r26 points (here 0: a fault at 0x15c, D-912).
    // There is no behaviour to compare; pspocket's DecodeAuEx2 decodes MVC's base view (D-590), logged.
    return std::move(r.o);
}

// MPEG-2 and MPEG-1 (mpeg2_writer.hpp's streams), under Sony's libvdec only (D-920 part 6): pspocket's own cellVdec
// decodes them with FFmpeg and fills their information as RPCS3 does, not compared with Sony's libvdec yet.
Observed mpeg2_family(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    for (u32 level = 0; level <= 4; level++) {
        query(r, std::format("MPEG-2 level {}", level), CODEC_MPEG2, level, {});
        query(r, std::format("MPEG-2 level {} 96x64", level), CODEC_MPEG2, level, spec8(8, 96, 64));
        query(r, std::format("MPEG-2 level {} 1920x1088", level), CODEC_MPEG2, level, spec8(8, 1920, 1088));
    }
    query(r, "MPEG-2 level 1, the information 12 bytes", CODEC_MPEG2, 1, spec8(12, 96, 64));
    query(r, "MPEG-2 level 1 0x64", CODEC_MPEG2, 1, spec8(8, 0, 64));
    query(r, "MPEG-2 level 1 97x64", CODEC_MPEG2, 1, spec8(8, 97, 64));
    using Edit = void (*)(m2::Picture&, int);
    const Rule exact = Rule::exact();
    const m2::Sequence seq;
    m2::Sequence il, m1, small, shown;
    il.progressive = false;
    m1.mpeg1 = true;
    small.width = 80;
    small.height = 48;
    shown.aspect = 3;
    shown.frame_rate = 4;
    shown.display_width = 90;
    shown.display_height = 60;
    shown.colour_primaries = 5;
    shown.transfer = 6;
    shown.matrix = 7;
    const std::vector<std::vector<u8>> ippp = m2::stream(seq, "IPPPP", true),
                                       ibbp = m2::stream(seq, "IBBPBBP", true, 1),
                                       motion = m2::stream(seq, "IPBBPBBP", true, 2,
                                           Edit{[](m2::Picture& p, int) { p.motion = true; }});
    decode(r, "MPEG-2 96x64 IPPPP", stream(CODEC_MPEG2, 0, ippp), 96, 64, exact);
    decode(r, "MPEG-2 96x64 IBBPBBP", stream(CODEC_MPEG2, 0, ibbp), 96, 64, exact);
    decode(r, "MPEG-2 96x64, macroblocks of every kind", stream(CODEC_MPEG2, 0, motion), 96, 64, exact);
    decode(r, "MPEG-2 96x64 interlaced",
        stream(CODEC_MPEG2, 0, m2::stream(il, "IPBPB", true, 3, Edit{[](m2::Picture& p, int) { p.motion = true; }})),
        96, 64, exact);
    decode(r, "MPEG-1 96x64", stream(CODEC_MPEG2, 0, m2::stream(m1, "IPBPB", true, 4, Edit{[](m2::Picture& p, int) {
        p.motion = true;
        p.f_code = 2;
    }})), 96, 64, exact);
    decode(r, "MPEG-2 80x48", stream(CODEC_MPEG2, 0, m2::stream(small, "IPBP", true, 5)), 80, 48, exact);
    // Pictures shown for two and three frames, each with its frame centre offsets (one, two or three); then with one
    // where two or three are called for (the program reads the first slice's start code as the others: damaged
    // pictures, not decoded).
    decode(r, "MPEG-2 96x64, 16:9 at 30000/1001 with a display extension",
        stream(CODEC_MPEG2, 0, m2::stream(shown, "IPBP", true, 6, Edit{[](m2::Picture& p, int i) {
            p.repeat_first_field = i % 2;
            p.top_field_first = i % 3 == 0;
            p.display_offsets = {{i * 16, -i * 8}};
            if (p.repeat_first_field) p.display_offsets.push_back({i * 8, 4});
            if (p.repeat_first_field && p.top_field_first) p.display_offsets.push_back({-16, -i});
            p.vbv_delay = 0x1234 + i;
        }})), 96, 64, exact);
    decode(r, "MPEG-2 96x64 with too few frame centre offsets",
        stream(CODEC_MPEG2, 0, m2::stream(shown, "IPBP", true, 6, Edit{[](m2::Picture& p, int i) {
            p.repeat_first_field = i % 2;
            p.top_field_first = i % 3 == 0;
            p.display_offsets = {{i * 16, -i * 8}};
        }})), 96, 64, exact);
    // Field pictures: both fields in one access unit, then one field an access unit.
    {
        const auto field_pic = [&](m2::Type type, int frame, int structure) {
            m2::Picture p;
            p.type = type;
            p.temporal_reference = frame;
            p.structure = structure;
            p.top_field_first = true;
            p.progressive_frame = false;
            p.frame_pred_frame_dct = false;
            p.seed = frame;
            p.ac = true;
            return m2::picture(il, p);
        };
        std::vector<std::vector<u8>> both, one;
        const m2::Type types[] = {m2::I, m2::P, m2::P};
        for (int frame = 0; frame < 3; frame++) {
            std::vector<u8> head;
            if (frame == 0) head = m2::join({m2::sequence_header(il), m2::gop_header(0)});
            both.push_back(m2::join({head, field_pic(types[frame], frame, 1), field_pic(types[frame], frame, 2)}));
            one.push_back(m2::join({head, field_pic(types[frame], frame, 1)}));
            one.push_back(field_pic(types[frame], frame, 2));
        }
        decode(r, "MPEG-2 field pictures, a frame an access unit", stream(CODEC_MPEG2, 0, both), 96, 64, exact);
        decode(r, "MPEG-2 field pictures, a field an access unit", stream(CODEC_MPEG2, 0, one), 96, 64, exact);
    }
    // Closed captions: DVD's in the sequence's user data, ATSC's in the pictures'.
    {
        m2::Sequence cc;
        cc.user_data = {'C', 'C', 0x01, 0xF8, 0x84, 0xFF, 0x94, 0x20, 0xFE, 0x80, 0x80, 0xFF, 0x94, 0x2C, 0xFE, 0x80,
            0x80};
        decode(r, "MPEG-2 96x64 with closed captions",
            stream(CODEC_MPEG2, 0, m2::stream(cc, "IPBP", true, 7, Edit{[](m2::Picture& p, int i) {
                if (i % 2)
                    p.user_data = {'G', 'A', '9', '4', 3, 0x42, 0xFF, 0xFC, static_cast<u8>(0x94 + i), 0x2C, 0xFD, 0x10,
                        0x20, 0xFF};
            }})), 96, 64, exact);
    }
    // The end of the sequence in the last access unit.
    {
        std::vector<std::vector<u8>> ended = ibbp;
        ended.back() = m2::join({ended.back(), m2::sequence_end()});
        decode(r, "MPEG-2 96x64 with a sequence end code", stream(CODEC_MPEG2, 0, ended), 96, 64, exact);
    }
    decode(r, "MPEG-2 96x64, SetFrameRate 25", stream(CODEC_MPEG2, 0, ippp, {}, 0x82), 96, 64, exact);
    decode(r, "MPEG-2 96x64 told 96x64", stream(CODEC_MPEG2, 0, ibbp, spec8(8, 96, 64)), 96, 64, exact);
    for (const s32 mode : {1, 2})
        decode(r, std::format("MPEG-2 96x64 in mode {}", mode), stream(CODEC_MPEG2, 0, ibbp, {}, 0, mode), 96, 64,
            exact);
    {
        Stream pieces = stream(CODEC_MPEG2, 0, ibbp);
        pieces.ex2 = true;
        decode(r, "MPEG-2 96x64 by DecodeAuEx2, odd access units in two pieces", pieces, 96, 64, exact);
    }
    hold(r, "MPEG-2 96x64, pictures not taken", stream(CODEC_MPEG2, 0, ibbp), 96, 64, exact);
    items_ahead(r, "MPEG-2 96x64, items ahead of their pictures", stream(CODEC_MPEG2, 0, ibbp));
    return std::move(r.o);
}

// The comparison's setup: Sony's libvdec over Sony's codec cores, which pspocket now has its own of (D-920).
Setup libvdec_setup()
{
    Setup s = setup();
    s.forced = kVideoCores;
    return s;
}

// pspocket's codec cores driven by Sony's libvdec, as a game gets them when it imports one of libvdec's unverified
// functions and so Sony's libvdec (D-594): the same lives with Sony's libvdec in both halves, over pspocket's cores in
// the first and Sony's in the second (D-920).
Setup cores_setup()
{
    Setup s = libvdec_setup();
    s.ours_on_firmware = true;
    return s;
}

} // namespace

TEST_CASE("cellVdec's new codecs against Sony's libvdec: DivX 3.11", "[.][hle][lle][compare][vdec]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3)) SKIP(kNoDivx);
    const Setup s = libvdec_setup();
    run(s, "cellVdec's DivX 3.11 against Sony's libvdec", [&](bool sony) { return divx311(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: the values Sony's gave (D-912).
            CHECK(o.value("QueryAttrEx DivX 3.11 level 15: the attribute").starts_with("hex:001e3578"));
            CHECK(o.value("QueryAttrEx DivX 3.11 level 14") == "0x80610101");
            CHECK(o.value("QueryAttrEx DivX 3.11 1 buffer") == "0x80610101");
            CHECK(o.value("QueryAttrEx DivX 3.11 96x64: the attribute").starts_with("hex:0009d778"));
            CHECK(o.value("DivX 3.11 96x64, six pictures: the handle") == "the decoder's memory +0x0");
            CHECK(o.value("DivX 3.11 96x64, six pictures: picture 0: its frame buffer at") ==
                "the decoder's memory +0x83480");
            CHECK(o.value("DivX 3.11 96x64, six pictures: picture 0: the information") ==
                "hex:00a500600040000001083480006000400030a5a500000000");
            CHECK(o.value("DivX 3.11 96x64, six pictures: picture 2: the item at") == "the decoder's memory +0xdc");
            CHECK(o.value("DivX 3.11 96x64 told 64x48: picture 1: its frame buffer at") ==
                "the decoder's memory +0x86180");
            // Two frame buffers: a third access unit waits while the game holds the first picture.
            CHECK(o.value("DivX 3.11 96x64, pictures not taken: DecodeAu 2: AUDONE (0 taken)") == "none");
            CHECK(o.value("DivX 3.11 96x64, pictures not taken: DecodeAu 2: AUDONE (1 taken)") == "yes");
            CHECK(o.value("OpenEx DivX 3.11 with 1 bytes less than its codec's part") == "0x80610101");
            CHECK(o.value("OpenEx DivX 3.11 with 0 bytes less than its codec's part") == "0x0");
        });
}

TEST_CASE("The DivX 3.11 core under Sony's libvdec: pspocket's against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3)) SKIP(kNoDivx);
    const Setup s = cores_setup();
    run(s, "libdivx311dec under Sony's libvdec", [&](bool sony) { return divx311(s, sony); }, [](const Observed&) {});
}

TEST_CASE("The DivX and MPEG-4 cores under Sony's libvdec: pspocket's against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP(kNoDivx);
    const Setup s = cores_setup();
    run(s, "libdivxdec and libsmvd4 under Sony's libvdec", [&](bool sony) { return mpeg4(s, sony); },
        [](const Observed&) {});
}

TEST_CASE("The MPEG-2 core under Sony's libvdec: pspocket's against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    const Setup s = cores_setup();
    run(s, "libsmvd2 under Sony's libvdec", [&](bool sony) { return mpeg2_family(s, sony); }, [](const Observed&) {});
}

TEST_CASE("cellVdec's new codecs against Sony's libvdec: MPEG-4 part 2 as DivX and as MPEG-4",
    "[.][hle][lle][compare][vdec]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP(kNoDivx);
    const Setup s = libvdec_setup();
    run(s, "cellVdec's MPEG-4 part 2 against Sony's libvdec", [&](bool sony) { return mpeg4(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("QueryAttrEx MPEG-4 SP L1 96x64: the attribute").starts_with("hex:0006ac0b"));
            CHECK(o.value("QueryAttrEx DivX QMOBILE 96x64: the attribute").starts_with("hex:000f7a40"));
            CHECK(o.value("QueryAttrEx DivX HD 720 1280x720, 3 buffers: the attribute").starts_with("hex:00526e90"));
            // CellVdecDivxInfo: an I picture 96x64, pixel aspect ratio code 2, its frame buffer's address.
            CHECK(o.value("DivX 96x64, pixels 12:11: picture 0: the information") ==
                "hex:00a50060004002000000000000000000010bc500006000400030a5a500000000");
            CHECK(o.value("MPEG-4 96x64, pixels 12:11: picture 0: its frame buffer at") ==
                "the decoder's memory +0x17080");
            CHECK(o.value("MPEG-4 96x64, pixels 12:11: picture 0: the information").starts_with(
                "hex:020000a5001900a500000060004000a50101708001019d0001019d08"));
            CHECK(o.value("MPEG-4 96x64, pixels 12:11: callback 1").starts_with("type 1")); // no lag: PICOUT
            CHECK(o.value("MPEG-4 80x48: picture 0: its frame buffer at") == "the decoder's memory +0x16f80");
            CHECK(o.value("MPEG-4 96x64, pictures not taken: DecodeAu 3: AUDONE (0 taken)") == "none");
        });
}

TEST_CASE("cellVdec's new codecs against Sony's libvdec: VC-1's advanced profile", "[.][hle][lle][compare][vdec]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    const Setup s = libvdec_setup();
    run(s, "cellVdec's VC-1 against Sony's libvdec", [&](bool sony) { return vc1_family(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("OpenEx VC-1 level 0") == "0x0"); // the simple and main profiles open, as Sony's
            CHECK(o.value("QueryAttrEx VC-1 level 10") == "0x80610101");
            CHECK(o.value("QueryAttrEx VC-1 AP L1 64x48: the attribute").starts_with("hex:0018acfb"));
            const Data* pic = o.find_data("VC-1 64x48 I, skipped, I, skipped: picture 0: the samples");
            REQUIRE(pic);
            CHECK(pic->bytes == v1::samples(dc_levels(64, 48, 0)));
            CHECK(o.value("VC-1 64x48 I, skipped, I, skipped: picture 1: its frame buffer at") ==
                o.value("VC-1 64x48 I, skipped, I, skipped: picture 0: its frame buffer at")); // a skipped picture
            CHECK(o.value("VC-1 64x48 I, skipped, I, skipped: picture 0: its frame buffer at") ==
                "the decoder's memory +0x2cd980");
            CHECK(o.value("VC-1 64x48 with an access unit before the headers: callback 0") ==
                "type 0 data 0x80610105 handle the decoder's memory +0x0");
            CHECK(o.value("VC-1 64x48 in mode 2: DecodeAu 0") == "0x80610101");
            CHECK(o.value("VC-1 64x48 in mode 3 (the simple and main profiles' sequence command): callback 0") ==
                "type 3 data 0x80610180 handle the decoder's memory +0x0");
            CHECK(o.value("VC-1 64x48, pictures not taken: DecodeAu 4: AUDONE (0 taken)") == "none");
        });
}

TEST_CASE("The VC-1 core under Sony's libvdec: pspocket's against Sony's", "[.][hle][lle][compare][vdec-cores]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    const Setup s = cores_setup();
    run(s, "libsvc1d under Sony's libvdec", [&](bool sony) { return vc1_family(s, sony); }, [](const Observed&) {});
}

TEST_CASE("cellVdec's new codecs against Sony's libvdec: MVC", "[.][hle][lle][compare][vdec]")
{
    const Setup s = libvdec_setup();
    run(s, "cellVdec's MVC against Sony's libvdec", [&](bool sony) { return mvc(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("QueryAttrEx codec 11 level 21 64x64: the attribute").starts_with("hex:00a4117d"));
            CHECK(o.value("QueryAttrEx codec 11 level 21: the attribute").starts_with("hex:0143cafd"));
            // Sony's libvdec gives its MVC core a size of 0 through DecodeAu: every access unit FATAL.
            CHECK(o.value("codec 11: four access units of two views: DecodeAu 0") == "0x80610180");
            CHECK(o.value("MVC: the views in access units apart: DecodeAu 0") == "0x80610180");
        });
}

#else

TEST_CASE("cellVdec's new codecs against Sony's libvdec", "[.][hle][lle][compare][vdec]")
{
    SKIP("no FFmpeg in this build: cellVdec decodes nothing of its own");
}

#endif
