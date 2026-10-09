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
#include "vdec_hw.hpp"
#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace h = pspocket::hle::test::h264;

namespace {

struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    const ps_spu_miss_policy spu = ps_spu_get_miss_policy();
    InterpretMisses()
    {
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~InterpretMisses()
    {
        ps_ppu_set_miss_policy(ppu);
        ps_spu_set_miss_policy(spu);
    }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// Guest memory: the scratch area for the callback, its log and the arguments; a larger area for the decoder.
constexpr u32 kCode = kData, kOpd = kData + 0x400, kLog = kData + 0x1000;
constexpr u32 kArgs = kData + 0x4000, kType = kArgs, kAttr = kArgs + 0x20, kRes = kArgs + 0x40, kCb = kArgs + 0x60,
              kHandle = kArgs + 0x70, kSpec = kArgs + 0x80, kAu = kArgs + 0xA0, kFormat = kArgs + 0xD0,
              kItemOut = kArgs + 0xF0;
constexpr u32 kStream = kData + 0x10000;   // the access units, 0x10000 bytes each
constexpr u32 kPicture = kData + 0x100000; // GetPicture's output
constexpr u32 kDecoderMem = 0x01000000, kDecoderMemSize = 0x01000000;
constexpr int kWidth = 64, kHeight = 64, kPictures = 3;

// The callback (handle r3, msgType r4, msgData r5, cbArg r6) appends (type, data, handle) to the log at cbArg: a
// count, then 16-byte entries from +16.
void write_callback(u8* b)
{
    const std::vector<u32> code = {
        as::lwz(9, 0, 6), as::slwi(10, 9, 4), as::add(10, 10, 6), as::stw(4, 16, 10), as::stw(5, 20, 10),
        as::stw(3, 24, 10), as::lwsync(), as::addi(9, 9, 1), as::stw(9, 0, 6), as::li(3, 0), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    std::memset(b + kLog, 0, 0x1000);
}

u32 log_count(u8* b)
{
    const u32 n = ps_read32(b, kLog);
    std::atomic_thread_fence(std::memory_order_acquire);
    return n;
}

u32 count_of(u8* b, u32 type)
{
    u32 n = 0;
    for (u32 i = 0; i < log_count(b); i++) n += ps_read32(b, kLog + 16 + 16 * i) == type;
    return n;
}

bool wait_until(const std::function<bool()>& done, std::chrono::seconds timeout = std::chrono::seconds(60))
{
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

std::string hex(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

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

struct BigFixture : Fixture {
    BigFixture()
    {
        REQUIRE(hle->link_internal_functions({"libvdec"}));
        REQUIRE(hle->main_memory().reserve(kDecoderMem, kDecoderMemSize));
        REQUIRE(mem.commit(kDecoderMem, kDecoderMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

// pspocket's libvdec, linked as a game's imports would be.
Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("libvdec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

[[maybe_unused]] std::string all_lines(const std::vector<std::string>& lines)
{
    std::string s;
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

[[maybe_unused]] const std::string* find_line(const std::vector<std::string>& lines, std::string_view prefix)
{
    for (const std::string& l : lines)
        if (l.starts_with(prefix)) return &l;
    return nullptr;
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
        FirmwareLibrary sony(f, t, *flash, "libvdec");
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
    FirmwareLibrary sony(f, t, *flash, "libvdec");
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
    FirmwareLibrary sony(f, t, *flash, "libvdec");
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
