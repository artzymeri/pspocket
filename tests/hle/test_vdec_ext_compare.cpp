// libvdec's functions D-920 made pspocket's own, against Sony's libvdec 4.92 (the user's installed system software):
// cellVdecSetFrameRateExt and SetFrameRate's codes, cellVdecStartSeqExt, cellVdecGetPicItemExt and GetPicItemEx (with
// their third argument), cellVdecGetInputStatus, cellVdecSetPts, cellVdecDecodeAuEx2 and its twin export 0x652B7BC0, the
// MVC export 0x20DA0CFA's refusals, the variable _cell_vdec_prx_ver; and what they act on: libvdec's ring giving a
// picture whose access unit had no PTS the last PTS plus the frame durations since (the rate set, else the stream's),
// the information blocks' frame rate codes, and the input queue as GetInputStatus reports it. Over DivX 3.11, DivX,
// MPEG-4 part 2 and VC-1 (the streams of vdec_streams.hpp), each family in a process of its own per half. What the
// D-912 comparisons check (pictures, frame buffers, information blocks) is test_vdec_codecs_compare.cpp's; here each
// item is compared whole but for its two addresses (given as offsets into the decoder's memory), and each information
// block. Skipped without the firmware (compare_util.hpp) and without FFmpeg's encoders. No game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "h264_writer.hpp"
#include "vc1_writer.hpp"
#include "vdec_streams.hpp"
#include "vdec_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace pspocket::hle::test::vdec_test;
namespace v1 = pspocket::hle::test::vc1;
namespace h = pspocket::hle::test::h264;

TEST_CASE("libvdec's D-920 functions: the comparison calls Sony's exports by their names; pspocket's own",
    "[hle][vdec]")
{
    check_names("libvdec",
        {"cellVdecSetFrameRateExt", "cellVdecStartSeqExt", "cellVdecGetPicItemExt", "cellVdecGetPicItemEx",
            "cellVdecGetInputStatus", "cellVdecSetPts", "cellVdecDecodeAuEx2"},
        false);
    CHECK(hle::registry().find("libvdec", 0x652B7BC0) != nullptr);
    CHECK(hle::registry().find("libvdec", 0x20DA0CFA) != nullptr);
    CHECK_FALSE(hle::registry().is_firmware_first("libvdec"));
}

#if PSPOCKET_HAVE_FFMPEG
using namespace pspocket::hle::test::vdec_streams;

namespace {

constexpr u32 CODEC_AVC = 1, CODEC_MPEG4 = 2, CODEC_VC1 = 3, CODEC_DIVX = 5, CODEC_DIVX3_11 = 9;
constexpr u32 kMem = 0x01000000, kMemSize = 0x04000000;
constexpr u32 kStatus = kArgs + 0x180, kExtra = kArgs + 0x188, kPts = kArgs + 0x190, kAuEx = kArgs + 0x1A0;
constexpr u32 kNone = 0xFFFFFFFF;

u32 info_size(u32 codec)
{
    switch (codec) {
    case CODEC_MPEG4: return 0x38;
    case CODEC_DIVX: return 0x20;
    case CODEC_DIVX3_11: return 0x18;
    case CODEC_AVC: return 0x244;
    default: return 0x170; // VC-1
    }
}

struct VdecFixture : Fixture {
    VdecFixture()
    {
        REQUIRE(hle->link_internal_functions({"libvdec"}));
        REQUIRE(hle->main_memory().reserve(kMem, kMemSize));
        REQUIRE(mem.commit(kMem, kMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};
using Run = Half<VdecFixture>;

std::string in_decoder(u32 addr)
{
    if (addr >= kMem && addr < kMem + kMemSize) return std::format("the decoder's memory +{:#x}", addr - kMem);
    return std::format("{:#x}", addr);
}

// Until the callbacks stop coming (none for 0.3 s, at most 10 s).
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

struct Codec {
    u32 codec = 0, level = 0;
    std::vector<u8> spec;
    std::vector<std::vector<u8>> aus;
    std::string name;
};

// QueryAttrEx and OpenEx with the memory asked for (filled with 0xA5 first); the handle, or 0.
u32 open_decoder(Run& r, const std::string& what, const Codec& c)
{
    u8* b = r.b;
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, c.codec);
    ps_write32(b, kType + 4, c.level);
    if (!c.spec.empty()) {
        std::memcpy(b + kSpec, c.spec.data(), c.spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    if (r.side.call("cellVdecQueryAttrEx", {kType, kAttr}).value_or(1) != 0) {
        r.o.line(what + ": QueryAttrEx", "refused");
        return 0;
    }
    const u32 res[6] = {kMem, std::min(ps_read32(b, kAttr), kMemSize), 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    std::memset(b + kMem, 0xA5, res[1]);
    if (r.r("cellVdecOpenEx", {kType, kRes, kCb, kHandle}, what + ": OpenEx") != 0) return 0;
    return ps_read32(b, kHandle);
}

enum class Send { au, ex2, twin };

// How DecodeAuEx2 gives an access unit (CellVdecAuInfoEx2's two pieces, D-920): whole; odd ones in two pieces (H.264
// and VC-1 cut one of 0x100 bytes or more after whole 128-byte blocks, the second piece at a 16-byte boundary; the
// others, whose codec parts give their core the first piece alone, get it whole and a second piece of zeros); or a
// second piece the codec part refuses.
enum class Pieces { whole, odd_split, no_address, unblocked_first, unaligned_second };

// Access unit `i` of the codec's stream at `at`, with a PTS (none: both words all ones) and DTS: its answer.
u64 send(Run& r, const std::string& key, u32 handle, const Codec& c, usize i, u32& at, std::optional<u64> pts,
    Send how = Send::au, u32 mode = 0, Pieces pieces = Pieces::odd_split)
{
    u8* b = r.b;
    const std::vector<u8>& au = c.aus[i % c.aus.size()];
    if (at + 2 * au.size() + 0x200 > kPicture) at = kStream;
    const u32 size = static_cast<u32>(au.size());
    const bool cuts = c.codec == CODEC_AVC || c.codec == CODEC_VC1;
    u32 first = size, second_at = 0, second = 0;
    if (how != Send::au && pieces == Pieces::odd_split && (i % 2) && (!cuts || size >= 0x100)) {
        if (cuts) first = (size / 2) & ~0x7Fu;
        second_at = (at + size + 0x110) & ~0xFu;
        second = first < size ? size - first : 0x40;
    } else if (how != Send::au && pieces == Pieces::no_address) {
        second = 0x40;
    } else if (how != Send::au && pieces == Pieces::unblocked_first && size >= 0x100) {
        first = ((size / 2) & ~0x7Fu) - 0x10;
        second_at = (at + size + 0x110) & ~0xFu;
        second = size - first;
    } else if (how != Send::au && pieces == Pieces::unaligned_second && size >= 0x100) {
        first = (size / 2) & ~0x7Fu;
        second_at = ((at + size + 0x110) & ~0xFu) + 8;
        second = size - first;
    }
    const u32 at0 = at;
    std::memcpy(b + at, au.data(), first);
    if (second_at) {
        if (first < size) std::memcpy(b + second_at, au.data() + first, size - first);
        else std::memset(b + second_at, 0, second);
    }
    if (first < size || second_at) at = second_at + second;
    const u32 upper = pts ? static_cast<u32>(*pts >> 32) : kNone, lower = pts ? static_cast<u32>(*pts) : kNone;
    if (how == Send::au) {
        ps_write32(b, kAu + 0, at0);
        ps_write32(b, kAu + 4, size);
        ps_write32(b, kAu + 8, upper);
        ps_write32(b, kAu + 12, lower);
        ps_write32(b, kAu + 16, kNone);
        ps_write32(b, kAu + 20, kNone);
        ps_write64(b, kAu + 24, 0x5500 + i);
        ps_write64(b, kAu + 32, 0);
    } else { // CellVdecAuInfoEx2: the two pieces' addresses and sizes, pts, dts, userData, codecSpecificData
        ps_write32(b, kAuEx + 0, at0);
        ps_write32(b, kAuEx + 4, second_at);
        ps_write32(b, kAuEx + 8, first);
        ps_write32(b, kAuEx + 12, second);
        ps_write32(b, kAuEx + 16, upper);
        ps_write32(b, kAuEx + 20, lower);
        ps_write32(b, kAuEx + 24, kNone);
        ps_write32(b, kAuEx + 28, kNone);
        ps_write64(b, kAuEx + 32, 0x5500 + i);
        ps_write64(b, kAuEx + 40, 0);
    }
    at = (at + size + 15) & ~15u;
    if (how == Send::au) return r.r("cellVdecDecodeAu", {handle, mode, kAu}, key);
    if (how == Send::ex2) return r.r("cellVdecDecodeAuEx2", {handle, mode, kAuEx}, key);
    const std::optional<u64> v = r.side.call_nid(0x652B7BC0, {handle, mode, kAuEx});
    r.o.line(key, v ? answer(*v) : std::string("(not exported)"));
    return v.value_or(~0ull);
}

// The pictures announced: each by GetPicItemExt (every other one by GetPicItemEx) with its third argument, the item
// (its addresses as offsets), the word at the third argument, the information block; then GetPicture without output.
void take(Run& r, const std::string& what, u32 handle, const Codec& c, u32& taken)
{
    u8* b = r.b;
    for (;;) {
        ps_write32(b, kItemOut, 0);
        ps_write32(b, kExtra, 0xEEEEEEEE);
        const std::string key = std::format("{}: picture {}", what, taken);
        const char* fn = taken % 2 ? "cellVdecGetPicItemEx" : "cellVdecGetPicItemExt";
        const u64 got = r.side.call(fn, {handle, kItemOut, kExtra}).value_or(~0ull);
        if (got != 0) {
            r.o.line(std::format("{}: {} then", key, fn), answer(got));
            return;
        }
        taken++;
        const u32 item = ps_read32(b, kItemOut);
        r.o.line(key + ": the item at", in_decoder(item));
        if (!r.readable(item, 0x4C)) return;
        std::vector<u8> fields(b + item, b + item + 0x4C);
        const u32 start = ps_read32(b, item + 4), info = ps_read32(b, item + 0x48);
        std::memset(fields.data() + 4, 0, 4);
        std::memset(fields.data() + 0x48, 0, 4);
        r.o.line(key + ": the item", compare::hex(fields));
        r.o.line(key + ": the item's PTS", std::format("{:08x} {:08x}", ps_read32(b, item + 0x10), ps_read32(b, item + 0x14)));
        const u32 extra = ps_read32(b, kExtra);
        if (c.codec == CODEC_AVC) {
            // H.264's frame buffers are pspocket's own place and its information block is written whole (D-196,
            // D-912): the frame rate code and where the third argument points are compared.
            r.o.note(key + ": its frame buffer at " + in_decoder(start));
            r.o.line(key + ": the third argument's word",
                extra == 0xEEEEEEEE ? std::string("untouched") : std::format("the information {:+#x}", static_cast<s64>(extra) - info));
            if (r.readable(info, 0x20)) r.o.line(key + ": the frame rate code", std::format("{:#x}", ps_read8(b, info + 0x1C)));
        } else {
            r.o.line(key + ": its frame buffer at", in_decoder(start));
            r.o.line(key + ": the third argument's word", std::format("{:#x}", extra));
            if (r.readable(info, info_size(c.codec)))
                r.o.line(key + ": the information", compare::hex(b + info, info_size(c.codec)));
        }
        ps_write32(b, kFormat, 3);
        ps_write32(b, kFormat + 4, 0);
        ps_write8(b, kFormat + 8, 0);
        r.r("cellVdecGetPicture", {handle, kFormat, 0}, key + ": GetPicture");
    }
}

void callbacks(Run& r, const std::string& what)
{
    u8* b = r.b;
    for (u32 i = 0; i < log_count(b) && i < 255; i++)
        r.o.line(std::format("{}: callback {}", what, i),
            std::format("type {} data {:#x}", ps_read32(b, kLog + 16 + 16 * i), ps_read32(b, kLog + 20 + 16 * i)));
}

// A step of a life: what to do before an access unit (a frame rate, a PTS), and the access unit's PTS.
struct Step {
    std::optional<u64> pts;                  // the access unit's (none: no PTS)
    std::optional<std::pair<u32, u32>> rate; // SetFrameRateExt(rate, scale) before it
    std::optional<u32> code;                 // SetFrameRate(code) before it
    std::optional<u64> set_pts;              // SetPts before it
    Send how = Send::au;
    Step(std::optional<u64> p = std::nullopt, std::optional<std::pair<u32, u32>> r = std::nullopt,
        std::optional<u32> c = std::nullopt, std::optional<u64> sp = std::nullopt, Send h = Send::au)
        : pts(p), rate(r), code(c), set_pts(sp), how(h)
    {
    }
};

// A sequence: StartSeqExt(`start_arg`), then each step (its calls, its access unit, the AUDONE, the pictures announced
// taken), EndSeq, the last pictures, the callbacks; Close. The input status along the way.
void life(Run& r, const std::string& what, const Codec& c, const std::vector<Step>& steps, u32 start_arg = 0)
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, c);
    if (!handle) return;
    const auto status = [&](const std::string& when) {
        std::memset(b + kStatus, 0xEE, 8);
        r.r("cellVdecGetInputStatus", {handle, kStatus}, what + ": GetInputStatus " + when);
        r.o.line(what + ": the input status " + when, compare::hex(b + kStatus, 8));
    };
    status("before the sequence");
    if (r.r("cellVdecStartSeqExt", {handle, start_arg}, std::format("{}: StartSeqExt {:#x}", what, start_arg)) == 0) {
        status("after StartSeqExt");
        u32 at = kStream, accepted = 0, taken = 0;
        for (usize i = 0; i < steps.size(); i++) {
            const Step& s = steps[i];
            const std::string key = std::format("{}: access unit {}", what, i);
            if (s.rate)
                r.r("cellVdecSetFrameRateExt", {handle, s.rate->first, s.rate->second},
                    std::format("{}: SetFrameRateExt({}, {})", key, s.rate->first, s.rate->second));
            if (s.code) r.r("cellVdecSetFrameRate", {handle, *s.code}, std::format("{}: SetFrameRate({:#x})", key, *s.code));
            if (s.set_pts) {
                ps_write32(b, kPts, static_cast<u32>(*s.set_pts >> 32));
                ps_write32(b, kPts + 4, static_cast<u32>(*s.set_pts));
                r.r("cellVdecSetPts", {handle, kPts}, std::format("{}: SetPts({:#x})", key, *s.set_pts));
            }
            if (send(r, key, handle, c, i, at, s.pts, s.how) != 0) continue;
            accepted++;
            r.o.line(key + ": AUDONE", wait_until([&] { return count_of(b, 0) >= accepted; }, std::chrono::seconds(10))
                                           ? "yes"
                                           : "none");
            settle(b);
            take(r, what, handle, c, taken);
        }
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        r.o.line(what + ": SEQDONE",
            wait_until([&] { return count_of(b, 2) >= 1; }, std::chrono::seconds(10)) ? "yes" : "none");
        settle(b);
        take(r, what, handle, c, taken);
        status("after the sequence");
        callbacks(r, what);
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
    // A closed decoder: what each call says.
    r.r("cellVdecSetFrameRateExt", {handle, 3, 0}, what + ": SetFrameRateExt after Close");
    ps_write32(b, kPts, 0);
    ps_write32(b, kPts + 4, 0x100);
    r.r("cellVdecSetPts", {handle, kPts}, what + ": SetPts after Close");
    r.r("cellVdecGetPicItemExt", {handle, kItemOut, kExtra}, what + ": GetPicItemExt after Close");
    r.r("cellVdecStartSeqExt", {handle, 0}, what + ": StartSeqExt after Close");
    std::memset(b + kStatus, 0xEE, 8);
    r.r("cellVdecGetInputStatus", {handle, kStatus}, what + ": GetInputStatus after Close");
    r.o.line(what + ": the input status after Close", compare::hex(b + kStatus, 8));
}

// The calls' refusals on an open decoder outside a sequence and in one: no pointers, codes past the codec's, modes.
void refusals(Run& r, const std::string& what, const Codec& c)
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, c);
    if (!handle) return;
    r.r("cellVdecGetInputStatus", {0, kStatus}, what + ": GetInputStatus with no handle");
    r.r("cellVdecGetInputStatus", {handle, 0}, what + ": GetInputStatus with no status");
    for (u32 code = 0; code <= 9; code++)
        r.r("cellVdecSetFrameRateExt", {handle, code, 0}, std::format("{}: SetFrameRateExt({}, 0)", what, code));
    r.r("cellVdecSetFrameRateExt", {0, 1, 0}, what + ": SetFrameRateExt with no handle");
    for (const u32 code : {0x7Fu, 0x80u, 0x83u, 0x87u, 0x88u, 0x180u})
        r.r("cellVdecSetFrameRate", {handle, code}, std::format("{}: SetFrameRate({:#x})", what, code));
    r.r("cellVdecSetFrameRate", {0, 0x81}, what + ": SetFrameRate with no handle");
    r.r("cellVdecSetPts", {handle, 0}, what + ": SetPts with no timestamp");
    r.r("cellVdecSetPts", {0, kPts}, what + ": SetPts with no handle");
    r.r("cellVdecStartSeqExt", {0, 0}, what + ": StartSeqExt with no handle");
    r.r("cellVdecGetPicItemExt", {handle, 0, kExtra}, what + ": GetPicItemExt with no item");
    r.r("cellVdecGetPicItemExt", {0, kItemOut, kExtra}, what + ": GetPicItemExt with no handle");
    r.r("cellVdecGetPicItemEx", {handle, kItemOut, kExtra}, what + ": GetPicItemEx outside a sequence");
    u32 at = kStream;
    send(r, what + ": DecodeAuEx2 outside a sequence", handle, c, 0, at, 0x1000, Send::ex2);
    send(r, what + ": 0x652B7BC0 outside a sequence", handle, c, 0, at, 0x1000, Send::twin);
    // 0x20DA0CFA, MVC's: refused for other codecs, and without its information.
    std::memset(b + kAuEx, 0, 0x40);
    ps_write32(b, kAuEx, kStream);
    ps_write32(b, kAuEx + 4, 0x100);
    for (const auto& [h, info] : {std::pair{handle, kAuEx}, {handle, 0u}, {0u, kAuEx}}) {
        const std::optional<u64> v = r.side.call_nid(0x20DA0CFA, {h, 0, info});
        r.o.line(std::format("{}: 0x20DA0CFA({}, 0, {})", what, h ? "the decoder" : "0", info ? "the information" : "0"),
            v ? answer(*v) : std::string("(not exported)"));
    }
    if (r.r("cellVdecStartSeqExt", {handle, 0xFF}, what + ": StartSeqExt 0xff") == 0) {
        r.r("cellVdecStartSeqExt", {handle, 0x1}, what + ": StartSeqExt again, in the sequence");
        r.r("cellVdecGetPicItemExt", {handle, kItemOut, 0}, what + ": GetPicItemExt with nothing announced");
        for (const u32 mode : {1u, 2u, 3u, 4u}) {
            std::memset(b + kAuEx, 0, 0x30);
            ps_write32(b, kAuEx, kStream);
            ps_write32(b, kAuEx + 8, 0x10);
            r.r("cellVdecDecodeAuEx2", {handle, mode, kAuEx}, std::format("{}: DecodeAuEx2 in mode {}", what, mode));
        }
        std::memset(b + kAuEx, 0, 0x30);
        ps_write32(b, kAuEx + 8, 0x10);
        r.r("cellVdecDecodeAuEx2", {handle, 0, kAuEx}, what + ": DecodeAuEx2 with no address");
        ps_write32(b, kAuEx, kStream);
        ps_write32(b, kAuEx + 8, 0);
        r.r("cellVdecDecodeAuEx2", {handle, 0, kAuEx}, what + ": DecodeAuEx2 with no size");
        r.r("cellVdecDecodeAuEx2", {handle, 0, 0}, what + ": DecodeAuEx2 with no information");
        r.r("cellVdecDecodeAuEx2", {0, 0, kAuEx}, what + ": DecodeAuEx2 with no handle");
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        r.o.line(what + ": SEQDONE",
            wait_until([&] { return count_of(b, 2) >= 1; }, std::chrono::seconds(10)) ? "yes" : "none");
        settle(b);
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
}

// A game that does not take its pictures: access units sent until the decoder holds one back (no AUDONE), then more,
// the input status after each, until DecodeAu is BUSY; then the pictures taken one at a time, the input status again.
void input_queue(Run& r, const std::string& what, const Codec& c)
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, c);
    if (!handle) return;
    const auto status = [&](const std::string& when) {
        std::memset(b + kStatus, 0xEE, 8);
        r.r("cellVdecGetInputStatus", {handle, kStatus}, what + ": GetInputStatus " + when);
        r.o.line(what + ": the input status " + when, compare::hex(b + kStatus, 8));
    };
    if (r.r("cellVdecStartSeq", {handle}, what + ": StartSeq") == 0) {
        u32 at = kStream, accepted = 0, taken = 0;
        bool held = false;
        for (usize i = 0; i < 12; i++) {
            const std::string key = std::format("{}: access unit {}", what, i);
            const u64 got = send(r, key, handle, c, i, at, 0x1000 + 3003 * i);
            if (got != 0) {
                status(std::format("after access unit {} was refused", i));
                break;
            }
            accepted++;
            if (!held) {
                held = !wait_until([&] { return count_of(b, 0) >= accepted; }, std::chrono::seconds(3));
                settle(b);
                r.o.line(key + ": AUDONE", held ? "none (held back)" : "yes");
            } else {
                settle(b);
            }
            status(std::format("after access unit {}", i));
        }
        // The pictures taken one at a time: the queue drains.
        for (u32 k = 0; k < 12; k++) {
            ps_write32(b, kItemOut, 0);
            if (r.r("cellVdecGetPicItem", {handle, kItemOut}, std::format("{}: GetPicItem {}", what, k)) != 0) break;
            ps_write32(b, kFormat, 3);
            ps_write32(b, kFormat + 4, 0);
            ps_write8(b, kFormat + 8, 0);
            r.r("cellVdecGetPicture", {handle, kFormat, 0}, std::format("{}: GetPicture {}", what, k));
            wait_until([&] { return count_of(b, 0) >= accepted; }, std::chrono::seconds(2));
            settle(b);
            status(std::format("after picture {} was taken", k));
        }
        r.o.line(what + ": AUDONEs", std::format("{} of {}", count_of(b, 0), accepted));
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        // Right after EndSeq the flush is in the input queue until the decoder's thread takes it: which of the two
        // the status shows is the threads' timing (a note).
        std::memset(b + kStatus, 0xEE, 8);
        r.side.call("cellVdecGetInputStatus", {handle, kStatus});
        r.o.note(what + ": the input status right after EndSeq " + compare::hex(b + kStatus, 8));
        r.o.line(what + ": SEQDONE",
            wait_until([&] { return count_of(b, 2) >= 1; }, std::chrono::seconds(10)) ? "yes" : "none");
        settle(b);
        take(r, what, handle, c, taken);
        status("after the sequence");
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
}

// DecodeAuEx2 with a second piece its codec part refuses (D-920): an access unit whole, then the refused one, then
// another whole; the sequence's end and the callbacks. H.264's part refuses at once (FATAL); VC-1's core when its
// thread decodes it (ERROR, and its decoder does nothing more); the DivX and MPEG-4 parts give their core the first
// piece alone.
void pieces_life(Run& r, const std::string& what, const Codec& c, Pieces pieces)
{
    if (!selected(what)) return;
    u8* b = r.b;
    const u32 handle = open_decoder(r, what, c);
    if (!handle) return;
    const auto status = [&](const std::string& when) {
        std::memset(b + kStatus, 0xEE, 8);
        r.r("cellVdecGetInputStatus", {handle, kStatus}, what + ": GetInputStatus " + when);
        r.o.line(what + ": the input status " + when, compare::hex(b + kStatus, 8));
    };
    if (r.r("cellVdecStartSeq", {handle}, what + ": StartSeq") == 0) {
        u32 at = kStream, taken = 0;
        // The first access unit of the stream's size past 0x100 bytes goes second, cut where its pieces are refused.
        usize big = 0;
        while (big + 1 < c.aus.size() && c.aus[big].size() < 0x100) big++;
        const usize order[3] = {0, big, 2};
        for (u32 k = 0; k < 3; k++) {
            const std::string key = std::format("{}: access unit {}", what, k);
            const u64 got = send(r, key, handle, c, order[k], at, 0x1000 + 0x100 * k, Send::ex2, 0,
                k == 1 ? pieces : Pieces::whole);
            if (got == 0)
                r.o.line(key + ": AUDONE",
                    wait_until([&] { return count_of(b, 0) + count_of(b, 3) >= k + 1; }, std::chrono::seconds(10))
                        ? "yes"
                        : "none");
            settle(b);
            take(r, what, handle, c, taken);
            status(std::format("after access unit {}", k));
        }
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        r.o.line(what + ": SEQDONE",
            wait_until([&] { return count_of(b, 2) >= 1; }, std::chrono::seconds(10)) ? "yes" : "none");
        settle(b);
        take(r, what, handle, c, taken);
        status("after the sequence");
        callbacks(r, what);
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
    status("after Close");
}

// The lives every codec family goes through.
void lives(Run& r, const Codec& c)
{
    const std::string n = c.name;
    const auto none = std::nullopt;
    // No rate set: the stream's (or none), PTSs missing before and after given ones.
    life(r, n + ", PTSs missing, no rate",
        c, {{none}, {0x1000}, {none}, {none}, {0x1'0000'9000}, {none}, {none}});
    // SetPts gives the base before any access unit has a PTS; a PTS given replaces it.
    life(r, n + ", SetPts first", c,
        {{none, {}, {}, 0x5000}, {none}, {none}, {0x8000}, {none}, {none, {}, {}, 0x1'2345'6789}, {none}});
    // Each code SetFrameRate names, inside the sequence, then a missing PTS.
    std::vector<Step> codes;
    for (u32 code = 0x80; code <= 0x87; code++) {
        codes.push_back({0x2000 + 0x100 * (code - 0x80), {}, code});
        codes.push_back({none});
    }
    life(r, n + ", SetFrameRate's codes", c, codes);
    // SetFrameRateExt's codes and its rate and scale form (whole frames a second named, and not).
    std::vector<Step> exts;
    const std::pair<u32, u32> forms[] = {{0, 0}, {1, 0}, {4, 0}, {8, 0}, {30000, 1001}, {25, 1}, {24000, 1001}, {60, 1},
        {1000, 7}, {48, 1}, {0x10000, 3}};
    u64 pts = 0x3000;
    for (const auto& f : forms) {
        exts.push_back({pts, f});
        exts.push_back({none});
        pts += 0x100;
    }
    life(r, n + ", SetFrameRateExt", c, exts);
    // A rate set before the sequence is forgotten by it; StartSeqExt's argument.
    {
        const std::string what = n + ", a rate before the sequence";
        if (selected(what)) {
            const u32 handle = open_decoder(r, what, c);
            if (handle) {
                r.r("cellVdecSetFrameRateExt", {handle, 25, 1}, what + ": SetFrameRateExt before");
                r.r("cellVdecClose", {handle}, what + ": Close");
            }
        }
        life(r, n + ", StartSeqExt 0x40000001", c, {{0x1000}, {none}, {none}}, 0x40000001);
    }
    // DecodeAuEx2 and its twin export: the same decoding.
    life(r, n + ", DecodeAuEx2 and 0x652B7BC0", c,
        {{0x1000, {}, {}, {}, Send::ex2}, {none, {}, {}, {}, Send::twin}, {0x2000, {}, {}, {}, Send::twin},
            {none, {}, {}, {}, Send::ex2}});
    pieces_life(r, n + ", a second piece with no address", c, Pieces::no_address);
    if (c.codec == CODEC_AVC || c.codec == CODEC_VC1) { // the others decode the first piece alone: a cut access unit
        pieces_life(r, n + ", a first piece not of 128-byte blocks", c, Pieces::unblocked_first);
        pieces_life(r, n + ", a second piece off a 16-byte boundary", c, Pieces::unaligned_second);
    }
    refusals(r, n + ", refusals", c);
    input_queue(r, n + ", the input queue", c);
}

constexpr const char* kNoDivx = "this FFmpeg has no msmpeg4v3 or mpeg4 encoder (the tests' build: build-ffmpeg.sh)";
constexpr const char* kNoVc1 = "this FFmpeg has no VC-1 decoder (build-ffmpeg.sh again)";

Observed divx311(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    // The variable's value.
    const u32 ver = r.side.firmware()
                        ? [&] {
                              const auto it = r.side.firmware()->module->variables.find({"libvdec", 0x085A7ECB});
                              return it == r.side.firmware()->module->variables.end() ? 0u : it->second;
                          }()
                        : r.f.hle->link_variable("libvdec", 0x085A7ECB);
    r.o.line("_cell_vdec_prx_ver", ver && r.readable(ver, 4) ? std::format("{:#x}", ps_read32(r.b, ver)) : "none");
    Codec c{CODEC_DIVX3_11, 15, divx_info2(12, 96, 64, 0), encode(AV_CODEC_ID_MSMPEG4V3, 96, 64, 24, 6), "DivX 3.11"};
    lives(r, c);
    return std::move(r.o);
}

Observed mpeg4(const Setup& s, bool sony_half, u32 codec)
{
    Run r(s, sony_half, "libvdec");
    // 5 a second: the VOPs' modulo time base counts seconds every five pictures (no fixed VOP rate: FFmpeg's).
    Codec c{codec, codec == CODEC_DIVX ? 10u : 0u, {},
        encode(AV_CODEC_ID_MPEG4, 96, 64, 24, 6, [](AVCodecContext* av) {
            av->time_base = {1, 5};
            av->framerate = {5, 1};
        }),
        codec == CODEC_DIVX ? "DivX" : "MPEG-4"};
    lives(r, c);
    return std::move(r.o);
}

Observed avc(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    for (const bool timing : {true, false}) {
        h::Options o;
        o.timing = timing;
        Codec c{CODEC_AVC, 42, {}, {}, timing ? "H.264" : "H.264 without VUI timing"};
        for (int i = 0; i < 24; i++) c.aus.push_back(h::access_unit(h::pattern(64, 64, i), static_cast<u32>(i & 1), o));
        if (timing) lives(r, c);
        else life(r, c.name + ", PTSs missing, no rate", c, {{std::nullopt}, {0x1000}, {std::nullopt}, {std::nullopt}});
    }
    return std::move(r.o);
}

Observed vc1_codec(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    v1::Sequence seq;
    seq.display_width = 64;
    seq.display_height = 48;
    seq.framerate_nr = 2; // 25 a second
    seq.framerate_dr = 1;
    const v1::Entry entry;
    const std::vector<u8> headers = concat({v1::sequence_header(seq), v1::entry_point(entry)});
    Codec c{CODEC_VC1, 6, {}, {}, "VC-1"};
    for (int i = 0; i < 24; i++)
        c.aus.push_back(i % 2 ? v1::skipped_frame(seq) : concat({headers, v1::intra_frame(dc_levels(64, 48, i), seq)}));
    lives(r, c);
    // No frame rate in the sequence header.
    v1::Sequence plain;
    const std::vector<u8> plain_headers = concat({v1::sequence_header(plain), v1::entry_point(entry)});
    Codec p{CODEC_VC1, 6, {}, {}, "VC-1 without a frame rate"};
    for (int i = 0; i < 8; i++)
        p.aus.push_back(i % 2 ? v1::skipped_frame(plain) : concat({plain_headers, v1::intra_frame(dc_levels(64, 48, i), plain)}));
    life(r, p.name + ", PTSs missing, no rate", p, {{std::nullopt}, {0x1000}, {std::nullopt}, {std::nullopt}});
    return std::move(r.o);
}

// The comparison's setup: Sony's libvdec over Sony's codec cores, which pspocket now has its own of (D-920).
Setup libvdec_setup()
{
    Setup s = setup();
    s.forced = kVideoCores;
    return s;
}

} // namespace

TEST_CASE("libvdec's D-920 functions against Sony's libvdec: DivX 3.11", "[.][hle][lle][compare][vdec][vdec-ext]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3)) SKIP(kNoDivx);
    const Setup s = libvdec_setup();
    run(s, "libvdec's D-920 functions on DivX 3.11 against Sony's", [&](bool sony) { return divx311(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("_cell_vdec_prx_ver") == "0x49200");
            CHECK(o.value("DivX 3.11, refusals: SetFrameRateExt(0, 0)") == "0x80610101");
            CHECK(o.value("DivX 3.11, refusals: SetFrameRateExt(8, 0)") == "0x0");
            CHECK(o.value("DivX 3.11, PTSs missing, no rate: the input status before the sequence") ==
                "hex:00010000eeeeeeee");
        });
}

TEST_CASE("libvdec's D-920 functions against Sony's libvdec: DivX and MPEG-4 part 2",
    "[.][hle][lle][compare][vdec][vdec-ext]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP(kNoDivx);
    const Setup s = libvdec_setup();
    for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4})
        run(s, std::format("libvdec's D-920 functions on {} against Sony's", codec == CODEC_DIVX ? "DivX" : "MPEG-4"),
            [&](bool sony) { return mpeg4(s, sony, codec); }, [](const Observed&) {});
}

TEST_CASE("libvdec's D-920 functions against Sony's libvdec: H.264", "[.][hle][lle][compare][vdec][vdec-ext]")
{
    const Setup s = libvdec_setup();
    run(s, "libvdec's D-920 functions on H.264 against Sony's", [&](bool sony) { return avc(s, sony); },
        [](const Observed&) {});
}

TEST_CASE("libvdec's D-920 functions against Sony's libvdec: VC-1", "[.][hle][lle][compare][vdec][vdec-ext]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    const Setup s = libvdec_setup();
    run(s, "libvdec's D-920 functions on VC-1 against Sony's", [&](bool sony) { return vc1_codec(s, sony); },
        [](const Observed&) {});
}

#endif
