// cellVdec (D-195): the video decoder. Synthetic H.264 written here (h264_writer.hpp: I_PCM pictures, so any decoder
// must give the samples back exactly), the game's callback as guest code, and the same calls to Sony's libvdec from
// the user's installed system software when PSPOCKET_TEST_DEV_FLASH names it (firmware_compare.hpp).

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "h264_writer.hpp"
#include "pspocket/recomp/nid.hpp"
#include "vdec_hw.hpp"
#include "vdec_test_util.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::vdec_test;
namespace h = pspocket::hle::test::h264;

namespace {

constexpr int kWidth = 64, kHeight = 64, kPictures = 3; // the access units are 0x10000 bytes apart from kStream

// Whether `out` holds picture `seed` as YUV 4:2:0 planar (Y, then U, then V, no padding).
bool is_yuv420(const u8* out, int seed)
{
    const h::Picture want = h::pattern(kWidth, kHeight, seed);
    return std::memcmp(out, want.y.data(), want.y.size()) == 0 &&
           std::memcmp(out + want.y.size(), want.cb.data(), want.cb.size()) == 0 &&
           std::memcmp(out + want.y.size() + want.cb.size(), want.cr.data(), want.cr.size()) == 0;
}

// What a decoder's life shows: every answer, item, information block and callback as lines (compared exactly), and
// each picture's frame buffer and output (compared byte by byte; RGB within one step).
struct Observed {
    std::vector<std::string> lines;
    std::vector<std::vector<u8>> frames, pictures;
    std::vector<u32> formats;
};

// YUV420 planar; RGBA BT.709 alpha 0x80; ARGB BT.601 alpha 0xFF; UYVY422; RGBA BT.601 alpha 0; ARGB BT.709 alpha 0x10.
constexpr u32 kFormats[kPictures * 2][3] = {{3, 0, 0}, {1, 1, 0x80}, {0, 0, 0xFF}, {2, 0, 0}, {1, 0, 0}, {0, 1, 0x10}};

Observed vdec_run(u8* b, const Call& call)
{
    Observed o;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        o.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    const auto note = [&](std::string s) { o.lines.push_back(std::move(s)); };
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);

    // Sizes: H.264 level 2.1; with its specific information (64x64; a wrong size field is refused); refusals.
    ps_write32(b, kType, 1);
    ps_write32(b, kType + 4, 21);
    r("cellVdecQueryAttr", 0xFF6F6EBE, {kType, kAttr});
    note(std::format("attr {:#x} {} {:#x} {:#x}", ps_read32(b, kAttr), ps_read8(b, kAttr + 4), ps_read32(b, kAttr + 8),
        ps_read32(b, kAttr + 12)));
    ps_write32(b, kType + 8, kSpec);
    ps_write32(b, kSpec, 16);
    r("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr});
    ps_write32(b, kSpec, 12);
    ps_write16(b, kSpec + 4, kWidth);
    ps_write16(b, kSpec + 6, kHeight);
    r("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr});
    const u32 mem_size = ps_read32(b, kAttr);
    note(std::format("attr ex {:#x} {}", mem_size, ps_read8(b, kAttr + 4)));
    r("cellVdecQueryAttr", 0xFF6F6EBE, {0, kAttr});
    ps_write32(b, kAttr + 0x40, 4); // codec 4: none
    ps_write32(b, kAttr + 0x44, 0);
    r("cellVdecQueryAttr", 0xFF6F6EBE, {kAttr + 0x40, kAttr});
    ps_write32(b, kAttr + 0x40, 1); // H.264 level 5: none
    ps_write32(b, kAttr + 0x44, 5);
    r("cellVdecQueryAttr", 0xFF6F6EBE, {kAttr + 0x40, kAttr});

    // Open (CellVdecResource: memory, PPU priority and stack, SPU priority, SPUs), a too small stack first.
    const u32 res[6] = {kDecoderMem, mem_size, 1000, 0x800, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    r("cellVdecOpenEx", 0x0053E2D8, {kType, kRes, kCb, kHandle});
    ps_write32(b, kRes + 12, 0x4000);
    if (r("cellVdecOpenEx", 0x0053E2D8, {kType, kRes, kCb, kHandle}) != 0) return o;
    const u32 handle = ps_read32(b, kHandle);
    note(std::format("handle {:#x} (memory {:+#x})", handle, static_cast<s64>(handle) - kDecoderMem));

    // Before a sequence: no picture, nothing to end.
    r("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut});
    r("cellVdecEndSeq", 0x824433F0, {handle});
    if (r("cellVdecStartSeq", 0xC757C2AA, {handle}) != 0) return o;

    // Six access units (CellVdecAuInfo: address, size, PTS, DTS, user data, codec specific data), each decoded
    // (AUDONE) before the next; the second without a DTS.
    for (int i = 0; i < kPictures * 2; i++) {
        const std::vector<u8> au = h::access_unit(h::pattern(kWidth, kHeight, i), static_cast<u32>(i & 1));
        const u32 at = kStream + 0x10000 * static_cast<u32>(i);
        std::memcpy(b + at, au.data(), au.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, 0);
        ps_write32(b, kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
        ps_write32(b, kAu + 16, i == 1 ? 0xFFFFFFFF : 0);
        ps_write32(b, kAu + 20, i == 1 ? 0xFFFFFFFF : 0x800 + 3003 * static_cast<u32>(i));
        ps_write64(b, kAu + 24, 0xABCD0000 + static_cast<u64>(i));
        ps_write64(b, kAu + 32, 0);
        r("cellVdecDecodeAu", 0x2BF4DDD2, {handle, 0, kAu});
        if (!wait_until([&] { return count_of(b, 0) >= static_cast<u32>(i + 1); })) note("no AUDONE");
    }
    const auto settle = [&] {
        u32 seen = count_of(b, 1);
        for (int quiet = 0; quiet < 50; quiet++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const u32 now = count_of(b, 1);
            if (now != seen) quiet = 0;
            seen = now;
        }
    };
    settle();
    note(std::format("pictures out after {} access units: {}", kPictures * 2, count_of(b, 1)));

    // The pictures: each item (its start address as the frame buffer's contents), its H.264 information, its picture.
    int next = 0;
    const auto take = [&] {
        for (; next < kPictures * 2; next++) {
            ps_write32(b, kItemOut, 0);
            if (r("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut}) != 0) return;
            const u32 item = ps_read32(b, kItemOut), start = ps_read32(b, item + 4), info = ps_read32(b, item + 0x48);
            std::vector<u8> fields(b + item, b + item + 0x4C);
            std::memset(fields.data() + 4, 0, 4); // the frame buffer's address: pspocket's own place
            note(std::format("item {} at {:+#x}: {}", next, static_cast<s64>(item) - kDecoderMem, hex(fields.data(), 0x48)));
            note(std::format("item {} information at item {:+#x}: {}", next, static_cast<s64>(info) - item,
                hex(b + info, 0x244)));
            o.frames.emplace_back(b + start, b + start + 0x3800);
            const u32* f = kFormats[next];
            ps_write32(b, kFormat, f[0]);
            ps_write32(b, kFormat + 4, f[1]);
            ps_write8(b, kFormat + 8, static_cast<u8>(f[2]));
            std::memset(b + kPicture, 0xCC, kWidth * kHeight * 4);
            if (r("cellVdecGetPicture", 0x807C861A, {handle, kFormat, kPicture}) != 0) return;
            const u32 bytes = f[0] == 3 ? kWidth * kHeight * 3 / 2 : f[0] == 2 ? kWidth * kHeight * 2 : kWidth * kHeight * 4;
            o.pictures.emplace_back(b + kPicture, b + kPicture + bytes);
            o.formats.push_back(f[0]);
            if (f[0] == 3) note(std::format("picture {} is the I_PCM samples: {}", next, is_yuv420(b + kPicture, next)));
        }
    };
    take();
    r("cellVdecGetPicture", 0x807C861A, {handle, kFormat, kPicture}); // none out now
    r("cellVdecEndSeq", 0x824433F0, {handle});
    if (!wait_until([&] { return count_of(b, 2) >= 1; })) note("no SEQDONE");
    take();
    r("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut}); // none left
    r("cellVdecDecodeAu", 0x2BF4DDD2, {handle, 0, kAu});      // no sequence
    for (u32 i = 0; i < log_count(b); i++)
        note(std::format("callback {}: type {} data {:#x} handle {:#x}", i, ps_read32(b, kLog + 16 + 16 * i),
            ps_read32(b, kLog + 20 + 16 * i), ps_read32(b, kLog + 24 + 16 * i)));
    r("cellVdecClose", 0x16698E83, {handle});
    r("cellVdecClose", 0x16698E83, {handle}); // closed
    return o;
}

} // namespace

#if PSPOCKET_HAVE_FFMPEG
TEST_CASE("cellVdec: H.264 decoded as Sony's libvdec does it (D-195)", "[hle][vdec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Observed o = vdec_run(b, hle_calls(f, t));
    INFO(all_lines(o.lines));
    const auto line = [&](std::string_view prefix) {
        const std::string* l = find_line(o.lines, prefix);
        return l ? *l : std::string("(missing)");
    };
    CHECK(line("attr ") == "attr 0xe90dfd 4 0x4920000 0x11300"); // Sony's, level 2.1
    CHECK(line("attr ex ") == "attr ex 0x7165fd 4");             // Sony's, 64x64
    CHECK(line("handle ") == std::format("handle {:#x} (memory +0x0)", kDecoderMem));
    CHECK(line("pictures out after") == "pictures out after 6 access units: 5"); // one access unit late
    CHECK(line("item 0 at ").starts_with("item 0 at +0xdc: 00000001"));
    CHECK(line("item 1 at ").starts_with("item 1 at +0x16c: "));
    for (int i = 0; i < kPictures * 2; i++)
        if (kFormats[i][0] == 3) CHECK(line(std::format("picture {} is", i)).ends_with("true"));
    REQUIRE(o.pictures.size() == kPictures * 2);
    CHECK(o.lines.back() == "cellVdecClose -> 0x80610102"); // closed: SEQ, as Sony's
}
#endif

TEST_CASE("cellVdec: the same as Sony's libvdec (the user's firmware): sizes, answers, items, information, pictures",
    "[hle][vdec][firmware]")
{
#if !PSPOCKET_HAVE_FFMPEG
    SKIP("no FFmpeg in this build (scripts/build-ffmpeg.sh): pspocket's cellVdec decodes no picture");
#endif
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    Observed ours, theirs;
    std::vector<std::string> our_sizes, their_sizes;
    const auto sizes = [](u8* b, const auto& query, std::vector<std::string>& out) {
        for (const u32 codec : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 9u, 11u, 13u}) {
            for (u32 level = 0; level <= 43; level++) {
                ps_write32(b, kType, codec);
                ps_write32(b, kType + 4, level == 43 ? 1042 : level);
                std::memset(b + kAttr, 0xEE, 0x10);
                const u32 r = query(kType, kAttr);
                if (r == 0)
                    out.push_back(std::format("codec {} level {}: {:#x} {} {:#x} {:#x}", codec, level, ps_read32(b, kAttr),
                        ps_read8(b, kAttr + 4), ps_read32(b, kAttr + 8), ps_read32(b, kAttr + 12)));
                else
                    out.push_back(std::format("codec {} level {}: {:#x}", codec, level, r));
            }
        }
    };
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        const Call c = hle_calls(f, t);
        ours = vdec_run(f.base(), c);
        sizes(f.base(), [&](u32 type, u32 attr) { return c("cellVdecQueryAttr", 0xFF6F6EBE, {type, attr}); }, our_sizes);
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "libvdec", true, kVideoCores);
        theirs = vdec_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
        sizes(f.base(), [&](u32 type, u32 attr) { return sony.call(t, 0xFF6F6EBE, {type, attr}); }, their_sizes);
    }
    usize differ = 0;
    std::string report;
    const auto compare_lines = [&](const std::vector<std::string>& a, const std::vector<std::string>& c) {
        for (usize i = 0; i < std::max(a.size(), c.size()); i++) {
            const std::string x = i < a.size() ? a[i] : "(none)", y = i < c.size() ? c[i] : "(none)";
            if (x == y) continue;
            if (++differ <= 30) report += std::format("line {}:\n  pspocket: {}\n  Sony:     {}\n", i, x, y);
        }
    };
    compare_lines(our_sizes, their_sizes);
    compare_lines(ours.lines, theirs.lines);
    REQUIRE(ours.frames.size() == theirs.frames.size());
    for (usize i = 0; i < ours.frames.size(); i++)
        if (ours.frames[i] != theirs.frames[i] && ++differ <= 40) report += std::format("frame buffer {} differs\n", i);
    REQUIRE(ours.pictures.size() == theirs.pictures.size());
    usize rgb_values = 0, rgb_off_by_one = 0;
    for (usize i = 0; i < ours.pictures.size(); i++) {
        const std::vector<u8>& a = ours.pictures[i];
        const std::vector<u8>& c = theirs.pictures[i];
        if (ours.formats[i] >= 2) { // YUV 4:2:0 and UYVY: exact
            if (a != c && ++differ <= 40) report += std::format("picture {} (format {}) differs\n", i, ours.formats[i]);
            continue;
        }
        for (usize k = 0; k < a.size(); k++) {
            const bool alpha = ours.formats[i] == 1 ? k % 4 == 3 : k % 4 == 0;
            if (alpha) {
                if (a[k] != c[k] && ++differ <= 40) report += std::format("picture {} alpha at {} differs\n", i, k);
                continue;
            }
            rgb_values++;
            const int d = std::abs(a[k] - c[k]);
            if (d == 1) rgb_off_by_one++;
            if (d > 1 && ++differ <= 40) report += std::format("picture {} byte {}: {} vs {}\n", i, k, a[k], c[k]);
        }
    }
    INFO(report);
    INFO(std::format("{} differences; RGB values one step off Sony's: {} of {}", differ, rgb_off_by_one, rgb_values));
    CHECK(differ == 0);
    WARN(std::format("RGB values one step off Sony's: {} of {} ({:.1f}%)", rgb_off_by_one, rgb_values,
        100.0 * static_cast<double>(rgb_off_by_one) / static_cast<double>(std::max<usize>(rgb_values, 1))));
}

#if PSPOCKET_HAVE_FFMPEG
TEST_CASE("cellVdec: H.264 on VideoToolbox gives the software decoder's pictures, in its order (D-254)", "[hle][vdec]")
{
    // The same stream through FFmpeg's software decoder and through its VideoToolbox hwaccel (cellVdec's helper):
    // the same pictures, sample for sample, in the same order, each with the access unit it came from.
    const auto decode = [](bool hardware, std::vector<std::vector<uint8_t>>& out, std::vector<int64_t>& from) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        ctx->thread_count = 1;
        const bool attached = hardware && pspocket::hle::vdec_hw::attach(ctx);
        REQUIRE(avcodec_open2(ctx, codec, nullptr) == 0);
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool on_hardware = false;
        const auto drain = [&] {
            while (avcodec_receive_frame(ctx, frame) == 0) {
                on_hardware |= pspocket::hle::vdec_hw::is_hardware(frame);
                std::vector<uint8_t> y, u, v;
                REQUIRE(pspocket::hle::vdec_hw::planes(frame, 320, 240, y, u, v));
                y.insert(y.end(), u.begin(), u.end());
                y.insert(y.end(), v.begin(), v.end());
                out.push_back(std::move(y));
                from.push_back(static_cast<int64_t>(reinterpret_cast<intptr_t>(frame->opaque)));
                av_frame_unref(frame);
            }
        };
        for (int i = 0; i < 6; i++) {
            const h::Picture pic = h::pattern(320, 240, i);
            std::vector<uint8_t> au = h::access_unit(pic, static_cast<uint32_t>(i & 1));
            pkt->data = au.data();
            pkt->size = static_cast<int>(au.size());
            pkt->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(100 + i));
            REQUIRE(avcodec_send_packet(ctx, pkt) == 0);
            drain();
        }
        REQUIRE(avcodec_send_packet(ctx, nullptr) == 0);
        drain();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return attached && on_hardware;
    };
    std::vector<std::vector<uint8_t>> software, hardware;
    std::vector<int64_t> software_from, hardware_from;
    decode(false, software, software_from);
    const bool on_hardware = decode(true, hardware, hardware_from);
    REQUIRE(software.size() == 6);
    if (!on_hardware) {
        WARN("VideoToolbox did not decode here: only the software path was checked");
        return;
    }
    REQUIRE(hardware.size() == software.size());
    for (usize i = 0; i < software.size(); i++) {
        INFO("picture " << i);
        CHECK(hardware[i] == software[i]);
        CHECK(hardware_from[i] == software_from[i]);
    }
}

TEST_CASE("cellVdec tests: the synthetic H.264 decodes to its own samples (FFmpeg)", "[hle][vdec]")
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    REQUIRE(codec);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    REQUIRE(avcodec_open2(ctx, codec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int got = 0;
    for (int i = 0; i < 3; i++) {
        const h::Picture pic = h::pattern(64, 48, i);
        std::vector<uint8_t> au = h::access_unit(pic, static_cast<uint32_t>(i & 1));
        pkt->data = au.data();
        pkt->size = static_cast<int>(au.size());
        REQUIRE(avcodec_send_packet(ctx, pkt) == 0);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            const h::Picture want = h::pattern(64, 48, got);
            REQUIRE(frame->width == 64);
            REQUIRE(frame->height == 48);
            bool same = true;
            for (int y = 0; y < 48; y++)
                for (int x = 0; x < 64; x++) same &= frame->data[0][y * frame->linesize[0] + x] == want.y[y * 64 + x];
            for (int y = 0; y < 24; y++)
                for (int x = 0; x < 32; x++) {
                    same &= frame->data[1][y * frame->linesize[1] + x] == want.cb[y * 32 + x];
                    same &= frame->data[2][y * frame->linesize[2] + x] == want.cr[y * 32 + x];
                }
            CHECK(same);
            CHECK(frame->pict_type == AV_PICTURE_TYPE_I);
            got++;
        }
    }
    REQUIRE(avcodec_send_packet(ctx, nullptr) == 0);
    while (avcodec_receive_frame(ctx, frame) == 0) got++;
    CHECK(got == 3);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
}

// ---- cellVdecOpenExt (D-596: RPCS3's, unverified, D-594), cellVdecSetPts and cellVdecDecodeAuEx2 (compared, D-920).

namespace {

// A decoder's life over four 64x64 H.264 access units, opened by Open and fed by DecodeAu, or opened by OpenExt on its
// CellVdecResourceExt and fed by DecodeAuEx2 (the odd access units in two pieces, D-920): what it shows, as
// vdec_test::decode_stream records it.
Decoded ext_life(u8* b, const Call& call, bool ext)
{
    Decoded d;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        d.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, 1);      // H.264
    ps_write32(b, kType + 4, 21); // level 2.1
    REQUIRE(call("cellVdecQueryAttr", 0xFF6F6EBE, {kType, kAttr}) == 0);
    const u32 mem_size = ps_read32(b, kAttr);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    if (ext) {
        // CellVdecResourceExt: memAddr, memSize, ppuThreadPriority, ppuThreadStackSize, 12 bytes, numOfSpus.
        const u32 res[4] = {kDecoderMem, mem_size, 1000, 0x4000};
        for (u32 i = 0; i < 4; i++) ps_write32(b, kRes + 4 * i, res[i]);
        std::memset(b + kRes + 16, 0xAB, 12);
        ps_write32(b, kRes + 0x1C, 1);
        d.open = r("cellVdecOpenExt", 0xEF4D8AD7, {kType, kRes, kCb, kHandle});
    } else {
        const u32 res[6] = {kDecoderMem, mem_size, 1000, 0x4000, 200, 1};
        for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
        d.open = r("cellVdecOpen", 0xB6BBCD5D, {kType, kRes, kCb, kHandle});
    }
    if (d.open) return d;
    const u32 handle = ps_read32(b, kHandle);
    CHECK(handle == kDecoderMem);
    REQUIRE(r("cellVdecStartSeq", 0xC757C2AA, {handle}) == 0);
    for (u32 i = 0; i < 4; i++) {
        const std::vector<u8> au = h::access_unit(h::pattern(kWidth, kHeight, static_cast<int>(i)), i & 1);
        const u32 at = kStream + 0x10000 * i, size = static_cast<u32>(au.size());
        std::memcpy(b + at, au.data(), au.size());
        const u32 pts = 0x1000 + 3003 * i, dts = 0x800 + 3003 * i;
        if (ext) {
            // CellVdecAuInfoEx2: the two pieces' addresses and sizes, pts, dts, userData, codecSpecificData; an odd
            // access unit's rest 0x8000 bytes on.
            const u32 first = (i & 1) ? (size / 2) & ~0x7Fu : size;
            if (first < size) std::memcpy(b + at + 0x8000, au.data() + first, size - first);
            const u32 words[8] = {at, first < size ? at + 0x8000 : 0, first, size - first, 0, pts, 0, dts};
            for (u32 k = 0; k < 8; k++) ps_write32(b, kAu + 4 * k, words[k]);
            ps_write64(b, kAu + 0x20, 0xABCD0000 + u64{i});
            ps_write64(b, kAu + 0x28, 0);
            r("cellVdecDecodeAuEx2", 0x1533C6A9, {handle, 0, kAu});
        } else {
            const u32 words[6] = {at, size, 0, pts, 0, dts};
            for (u32 k = 0; k < 6; k++) ps_write32(b, kAu + 4 * k, words[k]);
            ps_write64(b, kAu + 0x18, 0xABCD0000 + u64{i});
            ps_write64(b, kAu + 0x20, 0);
            r("cellVdecDecodeAu", 0x2BF4DDD2, {handle, 0, kAu});
        }
        if (!wait_until([&] { return count_of(b, 0) >= i + 1; })) d.lines.push_back("no AUDONE");
    }
    u32 seen = count_of(b, 1);
    for (int quiet = 0; quiet < 20; quiet++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const u32 now = count_of(b, 1);
        if (now != seen) quiet = 0;
        seen = now;
    }
    d.pictures_after_aus = seen;
    r("cellVdecEndSeq", 0x824433F0, {handle});
    if (!wait_until([&] { return count_of(b, 2) >= 1; })) d.lines.push_back("no SEQDONE");
    for (;;) {
        ps_write32(b, kItemOut, 0);
        if (call("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut}) != 0) break;
        const u32 item = ps_read32(b, kItemOut), info = ps_read32(b, item + 0x48), size = ps_read32(b, item + 8);
        d.items.emplace_back(b + item, b + item + 0x4C);
        d.infos.emplace_back(b + info, b + info + 0x244);
        ps_write32(b, kFormat, 3); // YUV 4:2:0 planar
        ps_write32(b, kFormat + 4, 0);
        ps_write8(b, kFormat + 8, 0);
        std::memset(b + kPicture, 0xCC, size + 0x100);
        if (r("cellVdecGetPicture", 0x807C861A, {handle, kFormat, kPicture}) != 0) break;
        d.pictures.emplace_back(b + kPicture, b + kPicture + size);
    }
    d.audone = count_of(b, 0);
    d.picout = count_of(b, 1);
    d.seqdone = count_of(b, 2);
    r("cellVdecClose", 0x16698E83, {handle});
    return d;
}

} // namespace

TEST_CASE("cellVdec: OpenExt opens on its resource, SetPts' answers, DecodeAuEx2 decodes as DecodeAu does (D-596, D-920)",
    "[hle][vdec]")
{
    const std::pair<const char*, u32> kExtFunctions[] = {{"cellVdecOpenExt", 0xEF4D8AD7}, {"cellVdecSetPts", 0x3CE2E4F8},
        {"cellVdecDecodeAuEx2", 0x1533C6A9}};
    for (const auto& [name, nid] : kExtFunctions) CHECK(recomp::nid_of(name) == nid);
    // OpenExt and MVC's two-view decode stay unverified (D-594); SetPts and DecodeAuEx2 with its twin are compared with
    // Sony's (D-920).
    for (const u32 nid : {0xEF4D8AD7u, 0x20DA0CFAu}) CHECK(hle::registry().is_unverified("libvdec", nid));
    for (const u32 nid : {0xB6BBCD5Du, 0x0053E2D8u, 0x2BF4DDD2u, 0x807C861Au, 0x17C702B9u, 0xE13EF6FCu, 0x3CE2E4F8u,
             0x1533C6A9u, 0x652B7BC0u})
        CHECK_FALSE(hle::registry().is_unverified("libvdec", nid));
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Call call = hle_calls(f, t);
    constexpr u32 kArg = 0x80610101, kSeq = 0x80610102;

    // The same decoder life both ways: every callback, item, information block and picture the same.
    const Decoded want = ext_life(b, call, false);
    const Decoded got = ext_life(b, call, true);
    INFO(all_lines(got.lines));
    REQUIRE(want.open == 0);
    REQUIRE(got.open == 0);
    CHECK(want.pictures.size() == 4);
    CHECK(got.audone == want.audone);
    CHECK(got.picout == want.picout);
    CHECK(got.seqdone == want.seqdone);
    CHECK(got.pictures_after_aus == want.pictures_after_aus);
    CHECK(got.items == want.items);
    CHECK(got.infos == want.infos);
    CHECK(got.pictures == want.pictures);
    REQUIRE(got.lines.size() == want.lines.size());
    for (usize i = 0; i < got.lines.size(); i++) // the answers, the calls' names apart
        CHECK(got.lines[i].substr(got.lines[i].find(" -> ")) == want.lines[i].substr(want.lines[i].find(" -> ")));

    // OpenExt's refusals: no resource, one that is not guest memory, then Open's own checks on what it holds.
    write_callback(b);
    ps_write32(b, kType, 1);
    ps_write32(b, kType + 4, 21);
    REQUIRE(call("cellVdecQueryAttr", 0xFF6F6EBE, {kType, kAttr}) == 0);
    const u32 res[8] = {kDecoderMem, ps_read32(b, kAttr), 1000, 0x4000, 0, 0, 0, 1};
    const auto open_ext = [&](u32 type, u32 at) { return call("cellVdecOpenExt", 0xEF4D8AD7, {type, at, kCb, kHandle}); };
    for (u32 i = 0; i < 8; i++) ps_write32(b, kRes + 4 * i, res[i]);
    CHECK(open_ext(kType, 0) == kArg);
    CHECK(open_ext(kType, 0xF0000000) == kArg);
    CHECK(open_ext(0, kRes) == kArg);
    ps_write32(b, kRes + 12, 0x800); // a stack under 4096 bytes
    CHECK(open_ext(kType, kRes) == kArg);
    ps_write32(b, kRes + 12, 0x4000);
    ps_write32(b, kRes + 8, 3072); // a PPU priority past 3071
    CHECK(open_ext(kType, kRes) == kArg);
    ps_write32(b, kRes + 8, 1000);
    REQUIRE(open_ext(kType, kRes) == 0);
    const u32 handle = ps_read32(b, kHandle);

    // SetPts: no decoder or no pointer ARG; an open decoder, in a sequence or not, CELL_OK; a closed one SEQ.
    const auto set_pts = [&](u32 h, u32 p) { return call("cellVdecSetPts", 0x3CE2E4F8, {h, p}); };
    CHECK(set_pts(handle + 0x100, kSpec) == kArg);
    CHECK(set_pts(handle, 0) == kArg);
    CHECK(set_pts(handle, kSpec) == 0);
    // DecodeAuEx2: DecodeAu's answers (no information, no size or start, the decode modes, before a sequence).
    const auto decode_ex2 = [&](u32 mode, u32 info) { return call("cellVdecDecodeAuEx2", 0x1533C6A9, {handle, mode, info}); };
    const std::vector<u8> au = h::access_unit(h::pattern(kWidth, kHeight, 0), 0);
    std::memcpy(b + kStream, au.data(), au.size());
    std::memset(b + kAu, 0, 0x30);
    ps_write32(b, kAu + 0, kStream);
    ps_write32(b, kAu + 8, static_cast<u32>(au.size()));
    CHECK(decode_ex2(0, kAu) == kSeq); // no sequence yet
    REQUIRE(call("cellVdecStartSeq", 0xC757C2AA, {handle}) == 0);
    CHECK(set_pts(handle, kSpec) == 0);
    CHECK(decode_ex2(0, 0) == kArg);
    CHECK(decode_ex2(0, 0xF0000000) == kArg);
    CHECK(decode_ex2(3, kAu) == kArg); // B and PB skipping together: VC-1's only
    CHECK(decode_ex2(5, kAu) == kArg);
    ps_write32(b, kAu + 8, 0);
    CHECK(decode_ex2(0, kAu) == kArg); // no size
    ps_write32(b, kAu + 8, static_cast<u32>(au.size()));
    ps_write32(b, kAu + 0, 0);
    CHECK(decode_ex2(0, kAu) == kArg); // no start
    ps_write32(b, kAu + 0, kStream);
    ps_write32(b, kAu + 12, 0x10);
    CHECK(decode_ex2(0, kAu) == 0x80610180); // a second piece with no address: libavcdec refuses it (D-920)
    ps_write32(b, kAu + 4, kStream + 0x8000);
    ps_write32(b, kAu + 12, 0x1FFFFF80);
    CHECK(decode_ex2(0, kAu) == 0x80610180); // the two pieces past 0x1FFFFF80 bytes
    ps_write32(b, kAu + 4, 0);
    ps_write32(b, kAu + 12, 0);
    REQUIRE(call("cellVdecClose", 0x16698E83, {handle}) == 0);
    CHECK(set_pts(handle, kSpec) == kSeq);
    ps_write32(b, kAu + 0, kStream);
    CHECK(decode_ex2(0, kAu) == kSeq);
}
#endif

// Sony's pictures in every output format, its picture items, H.264 information and frame buffers, written to the
// directory PSPOCKET_PROBE_DUMP names (for working out pspocket's to match; nothing from it is committed).
TEST_CASE("cellVdec: Sony's libvdec pictures, items and frame buffers dumped (probe)", "[hle][vdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    const char* dump = std::getenv("PSPOCKET_PROBE_DUMP");
    if (!flash || !dump) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "libvdec", true, kVideoCores);
    u8* b = f.base();
    const auto call = [&](u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); };
    const auto save = [&](const std::string& name, u32 addr, u32 size) {
        FILE* out = std::fopen((std::string(dump) + "/" + name).c_str(), "wb");
        REQUIRE(out);
        std::fwrite(b + addr, 1, size, out);
        std::fclose(out);
    };
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, 1);
    ps_write32(b, kType + 4, 21);
    ps_write32(b, kType + 8, kSpec);
    ps_write32(b, kSpec, 12);
    ps_write16(b, kSpec + 4, kWidth);
    ps_write16(b, kSpec + 6, kHeight);
    REQUIRE(call(0xC982A84A, {kType, kAttr}) == 0);
    const u32 res[6] = {kDecoderMem, ps_read32(b, kAttr), 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    REQUIRE(call(0x0053E2D8, {kType, kRes, kCb, kHandle}) == 0);
    const u32 handle = ps_read32(b, kHandle);
    REQUIRE(call(0xC757C2AA, {handle}) == 0);
    constexpr int kCount = 6;
    for (int i = 0; i < kCount; i++) {
        const std::vector<u8> au = h::access_unit(h::pattern(kWidth, kHeight, i), static_cast<u32>(i & 1));
        const u32 at = kStream + 0x10000 * static_cast<u32>(i % 8);
        std::memcpy(b + at, au.data(), au.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, 0);
        ps_write32(b, kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
        ps_write32(b, kAu + 16, 0);
        ps_write32(b, kAu + 20, 0x800 + 3003 * static_cast<u32>(i));
        ps_write64(b, kAu + 24, 0xABCD0000 + static_cast<u64>(i));
        ps_write64(b, kAu + 32, 0);
        REQUIRE(call(0x2BF4DDD2, {handle, 0, kAu}) == 0);
        REQUIRE(wait_until([&] { return count_of(b, 0) >= static_cast<u32>(i + 1); }));
    }
    REQUIRE(call(0x824433F0, {handle}) == 0);
    REQUIRE(wait_until([&] { return count_of(b, 2) >= 1; }));
    // YUV420 planar; RGBA BT.709 alpha 0x80; ARGB BT.601 alpha 0xFF; UYVY422; RGBA BT.601 alpha 0; ARGB BT.709 0x10.
    const u32 formats[kCount][3] = {{3, 0, 0}, {1, 1, 0x80}, {0, 0, 0xFF}, {2, 0, 0}, {1, 0, 0}, {0, 1, 0x10}};
    for (int i = 0; i < kCount; i++) {
        REQUIRE(call(0x17C702B9, {handle, kItemOut}) == 0);
        const u32 item = ps_read32(b, kItemOut);
        save(std::format("item{}.bin", i), item, 0x90);
        save(std::format("info{}.bin", i), ps_read32(b, item + 0x48), 0x244);
        save(std::format("frame{}.bin", i), ps_read32(b, item + 4), 0x3800);
        std::printf("item %d at %#x start %#x info %#x\n", i, item - kDecoderMem, ps_read32(b, item + 4) - kDecoderMem,
            ps_read32(b, item + 0x48) - kDecoderMem);
        ps_write32(b, kFormat, formats[i][0]);
        ps_write32(b, kFormat + 4, formats[i][1]);
        ps_write8(b, kFormat + 8, static_cast<u8>(formats[i][2]));
        std::memset(b + kPicture, 0xCC, kWidth * kHeight * 4 + 0x100);
        REQUIRE(call(0x807C861A, {handle, kFormat, kPicture}) == 0);
        save(std::format("pic{}.bin", i), kPicture, kWidth * kHeight * 4 + 0x100);
    }
    save("decoder_memory.bin", kDecoderMem, 0x4000);
    for (u32 i = 0; i < log_count(b); i++) std::printf("callback %u: type %u\n", i, ps_read32(b, kLog + 16 + 16 * i));
    REQUIRE(call(0x16698E83, {handle}) == 0);
}

// Sony's cellVdecQueryAttr over every codec and level (the sizes pspocket's must give).
TEST_CASE("cellVdec: Sony's sizes for every codec and level (probe)", "[hle][vdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    if (!flash) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "libvdec", true, kVideoCores);
    u8* b = f.base();
    for (const u32 codec : {0u, 1u, 2u, 3u, 5u, 7u, 9u, 11u, 13u}) {
        for (u32 level = 0; level < 1100; level++) {
            if (level > 50 && level != 1042) continue;
            ps_write32(b, kType, codec);
            ps_write32(b, kType + 4, level);
            std::memset(b + kAttr, 0xEE, 0x10);
            const u32 r = sony.call(t, 0xFF6F6EBE, {kType, kAttr});
            if (r == 0)
                std::printf("codec %u level %u: mem %#x depth %u lower %#x\n", codec, level, ps_read32(b, kAttr),
                    ps_read8(b, kAttr + 4), ps_read32(b, kAttr + 12));
        }
    }
}
