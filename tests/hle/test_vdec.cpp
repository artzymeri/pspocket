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

// The life of a decoder, every observation a line: sizes, open, three I_PCM pictures decoded, their items and
// pictures (YUV 4:2:0, then RGBA and ARGB with alpha), the end of the sequence, close.
std::vector<std::string> vdec_flow(u8* b, const Call& call)
{
    std::vector<std::string> out;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        out.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    const auto note = [&](std::string s) { out.push_back(std::move(s)); };
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);

    // Sizes: H.264 level 2.1, then with its specific information (64x64, the deblocking filter on, default buffers).
    ps_write32(b, kType, 1);
    ps_write32(b, kType + 4, 21);
    r("cellVdecQueryAttr", 0xFF6F6EBE, {kType, kAttr});
    note(std::format("attr {:#x} {} {:#x} {:#x}", ps_read32(b, kAttr), ps_read8(b, kAttr + 4), ps_read32(b, kAttr + 8),
        ps_read32(b, kAttr + 12)));
    ps_write32(b, kType + 8, kSpec);
    ps_write32(b, kSpec, 12);
    ps_write16(b, kSpec + 4, kWidth);
    ps_write16(b, kSpec + 6, kHeight);
    r("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr});
    const u32 mem_size = ps_read32(b, kAttr);
    note(std::format("attr ex {:#x} {}", mem_size, ps_read8(b, kAttr + 4)));
    r("cellVdecQueryAttr", 0xFF6F6EBE, {0, kAttr});

    // Open (CellVdecResource: memory, PPU priority and stack, SPU priority, SPUs), with too little memory first.
    const u32 res[6] = {kDecoderMem, mem_size, 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    if (r("cellVdecOpenEx", 0x0053E2D8, {kType, kRes, kCb, kHandle}) != 0) return out;
    const u32 handle = ps_read32(b, kHandle);
    note(std::format("handle {:#x} (memory {:+#x})", handle, static_cast<s64>(handle) - kDecoderMem));

    // Before a sequence: decoding is refused, no picture.
    r("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut});
    r("cellVdecEndSeq", 0x824433F0, {handle});
    if (r("cellVdecStartSeq", 0xC757C2AA, {handle}) != 0) return out;

    // Three access units (CellVdecAuInfo: address, size, PTS, DTS, user data, codec specific data).
    for (int i = 0; i < kPictures; i++) {
        const std::vector<u8> au = h::access_unit(h::pattern(kWidth, kHeight, i), static_cast<u32>(i & 1));
        const u32 at = kStream + 0x10000 * static_cast<u32>(i);
        std::memcpy(b + at, au.data(), au.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        ps_write32(b, kAu + 8, 0);
        ps_write32(b, kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
        ps_write32(b, kAu + 16, 0xFFFFFFFF);
        ps_write32(b, kAu + 20, 0xFFFFFFFF);
        ps_write64(b, kAu + 24, 0xABCD0000 + static_cast<u64>(i));
        ps_write64(b, kAu + 32, 0);
        r("cellVdecDecodeAu", 0x2BF4DDD2, {handle, 0, kAu});
    }
    // Every access unit decoded (AUDONE); then the pictures out so far (PICOUT stops changing), each item then its
    // picture; the end of the sequence (SEQDONE) and the pictures it let out.
    if (!wait_until([&] { return count_of(b, 0) >= kPictures; })) note("not all access units decoded");
    const auto settle = [&] {
        u32 seen = count_of(b, 1);
        for (int quiet = 0; quiet < 100; quiet++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const u32 now = count_of(b, 1);
            if (now != seen) quiet = 0;
            seen = now;
        }
    };
    settle();
    note(std::format("pictures out after {} access units: {}", kPictures, count_of(b, 1)));
    const u32 formats[3][3] = {{3, 0, 0}, {1, 1, 0x80}, {0, 0, 0xFF}}; // YUV420 planar; RGBA BT.709; ARGB BT.601
    int next = 0;
    const auto take = [&] {
        for (; next < kPictures; next++) {
            ps_write32(b, kItemOut, 0);
            if (r("cellVdecGetPicItem", 0x17C702B9, {handle, kItemOut}) != 0) return;
            const u32 item = ps_read32(b, kItemOut);
            note(std::format("item {} at {:+#x}", next, static_cast<s64>(item) - kDecoderMem));
            note(std::format("item {} raw {}", next, hex(b + item, 0x4C)));
            const u32 start = ps_read32(b, item + 4), info = ps_read32(b, item + 0x48);
            if (start >= kDecoderMem && start < kDecoderMem + kDecoderMemSize)
                note(std::format("item {} start {:+#x}: YUV420 of the picture there: {}", next,
                    static_cast<s64>(start) - kDecoderMem, is_yuv420(b + start, next)));
            if (info >= kDecoderMem && info < kDecoderMem + kDecoderMemSize)
                note(std::format("item {} info at item {:+#x}: {}", next, static_cast<s64>(info) - item, hex(b + info, 0x60)));
            const u32* f = formats[next];
            ps_write32(b, kFormat, f[0]);
            ps_write32(b, kFormat + 4, f[1]);
            ps_write8(b, kFormat + 8, static_cast<u8>(f[2]));
            std::memset(b + kPicture, 0xCC, kWidth * kHeight * 4 + 0x100);
            if (r("cellVdecGetPicture", 0x807C861A, {handle, kFormat, kPicture}) != 0) return;
            const u32 bytes = f[0] == 3 ? kWidth * kHeight * 3 / 2 : kWidth * kHeight * 4;
            if (f[0] == 3) note(std::format("picture {} is the I_PCM samples: {}", next, is_yuv420(b + kPicture, next)));
            note(std::format("picture {} first bytes {} ... last {} after {}", next, hex(b + kPicture, 16),
                hex(b + kPicture + bytes - 8, 8), hex(b + kPicture + bytes, 4)));
            u64 sum = 0;
            for (u32 k = 0; k < bytes; k++) sum = sum * 31 + b[kPicture + k];
            note(std::format("picture {} checksum {:#x}", next, sum));
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
    return out;
}

struct BigFixture : Fixture {
    BigFixture()
    {
        REQUIRE(hle->main_memory().reserve(kDecoderMem, kDecoderMemSize));
        REQUIRE(mem.commit(kDecoderMem, kDecoderMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

} // namespace

TEST_CASE("cellVdec: Sony's libvdec over the synthetic H.264 (probe)", "[hle][vdec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    if (!flash) return;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "libvdec");
    const std::vector<std::string> lines =
        vdec_flow(f.base(), [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    for (const std::string& l : lines) std::printf("%s\n", l.c_str());
}

#if PSPOCKET_HAVE_FFMPEG
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
