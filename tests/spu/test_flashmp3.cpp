// Sony's MP3 decoder module (flashMP3.pic) natively (D-974): runtime/spu/replace/flashmp3.c on synthetic streams
// (mp3_writer.hpp), called as GTA V's decode job calls the module (its buffers' layout, one frame a call).
//
// - Without the firmware: the native decoder against FFmpeg's (when the build has FFmpeg), the tags, the Xing frame,
//   and what it declines changing nothing.
// - With the firmware (PSPOCKET_TEST_DEV_FLASH: the user's installed /dev_flash, as the library comparisons, D-193): the
//   module itself, from the user's flashMP3.pic, run by the interpreter on the same calls; each call from the module's
//   own state must leave what it leaves (the regions of pspocket/spu/replace.hpp: exact outside them, the PCM within
//   a step, the floats within 1e-4 of their peak), and a whole stream decoded natively from its own state stays
//   within a few steps of the module's. Nothing of the firmware enters the repository (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include "mp3_writer.hpp"
#include "native.h"
#include "pspocket/core/be.h"
#include "pspocket/spu/interpreter.hpp"
#include "pspocket/spu/replace.hpp"
#include "spu_test_util.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

using namespace pspocket;
using namespace pspocket::spu::test;
namespace rp = pspocket::spu::replace;

namespace {

constexpr u64 kHash = 0x884AE88F68A9E778ull;
constexpr u32 kModule = 0x17400, kModuleSize = 0x10380, kEntry = 0x1E820, kPrintf = 0x20730;
// The job's layout (as GTA V's: one 4992-byte block for channel 0 with the reservoir, 4480 for channel 1).
constexpr u32 kHeap = 0x2A000, kState0 = 0x30000, kState1 = 0x31400, kFrame = 0x33000, kConsumed = 0x34000,
              kPcmBytes = 0x34010, kBo = 0x34020, kStack = 0x3FF00, kReturn = 0x00000;

struct Call {
    std::vector<u8> ls;
    SPUContext ctx{};
};

// A local store as the job leaves it before a call: the module (when there is one) and the frame in place.
void set_up(std::vector<u8>& ls, SPUContext& ctx, const std::vector<u8>& frame)
{
    std::memcpy(ls.data() + kFrame, frame.data(), frame.size());
    std::memset(ls.data() + kFrame + frame.size(), 0, 4096 + 16 - std::min<size_t>(frame.size(), 4096));
    ps_st32(ls.data() + kReturn, 0x00003FFF); // stop 0x3fff: back from the module
    ps_spu_context_init(&ctx, ls.data(), nullptr);
    ctx.xfloat = PS_SPU_XFLOAT_APPROXIMATE;
    ctx.ch.dec_frozen = 1;
    ctx.ch.dec_value = 0x12345678;
    const auto set = [&](int r, u32 v) { ctx.gpr[r] = splat(v); };
    set(0, kReturn);
    set(1, kStack);
    set(3, kFrame);
    set(4, kConsumed);
    set(5, kPcmBytes);
    set(6, kHeap);
    set(7, kState0 + 2176);
    set(8, kState1 + 2176);
    set(9, kState0);
    set(10, kState1);
    set(11, kState0 + 4480);
    set(12, kBo);
    ctx.pc = kEntry;
}

int call_native(std::vector<u8>& ls, const std::vector<u8>& frame, SPUContext* out = nullptr)
{
    SPUContext ctx{};
    set_up(ls, ctx, frame);
    FpEnv fp;
    const int r = ps_flashmp3_884a_1e820(&ctx, ls.data());
    if (out) *out = ctx;
    ps_spu_host_detach(&ctx);
    return r;
}

std::optional<std::vector<u8>> firmware_module()
{
    const char* flash = std::getenv("PSPOCKET_TEST_DEV_FLASH");
    if (!flash || !*flash) return std::nullopt;
    std::ifstream f(std::filesystem::path(flash) / "sys/external/flashMP3.pic", std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 0x100 + kModuleSize) return std::nullopt;
    return std::vector<u8>(file.begin() + 0x100, file.begin() + 0x100 + kModuleSize);
}

// The module on the interpreter: 1, or 0 when it reached its printf to the PPU (a frame it refuses).
int call_module(std::vector<u8>& ls, const std::vector<u8>& module, const std::vector<u8>& frame, SPUContext* out = nullptr)
{
    std::memcpy(ls.data() + kModule, module.data(), module.size());
    ps_st32(ls.data() + kPrintf, 0x00002222); // stop 0x2222 where the module would print
    SPUContext ctx{};
    set_up(ls, ctx, frame);
    FpEnv fp;
    spu::RunOptions o;
    o.max_steps = 50'000'000;
    spu::run(ctx, o);
    REQUIRE(ctx.stop_reason == PS_SPU_STOP_SIGNAL);
    const int printed = ctx.stop_code == 0x2222;
    if (!printed) REQUIRE(ctx.stop_code == 0x3FFF);
    ctx.pc = kReturn; // as the module's bi $lr leaves it
    ctx.stop_reason = PS_SPU_STOP_NONE;
    ctx.stop_code = 0;
    if (out) *out = ctx;
    ps_spu_host_detach(&ctx);
    std::memcpy(ls.data() + kPrintf, module.data() + (kPrintf - kModule), 4);
    return printed ? 0 : 1;
}

struct Compared {
    std::string differs; // outside the regions, or past their tolerance
    int pcm = 0;         // the largest 16-bit difference
    double f32 = 0;      // the largest float difference against its region's peak
};

Compared compare(const std::vector<u8>& want, const std::vector<u8>& got, const SPUContext& entry, const u8* ls_entry)
{
    Compared c;
    std::vector<u8> covered(PS_SPU_LS_SIZE, 0);
    for (const rp::Region& r : rp::regions(kHash, kModule, kModuleSize, kEntry, entry, ls_entry)) {
        std::memset(covered.data() + r.start, 1, r.size);
        if (r.kind == rp::Region::Kind::s16) {
            for (u32 i = 0; i < r.size; i += 2)
                c.pcm = std::max(c.pcm, std::abs(static_cast<int16_t>(ps_ld16(&want[r.start + i])) -
                                                 static_cast<int16_t>(ps_ld16(&got[r.start + i]))));
            if (c.pcm > r.tolerance) c.differs += std::format(" PCM at {:05x}: {} steps;", r.start, c.pcm);
        } else if (r.kind == rp::Region::Kind::f32) {
            double peak = 0, worst = 0;
            for (u32 i = 0; i < r.size; i += 4) {
                const double x = static_cast<double>(ps_ldf32(&want[r.start + i]));
                const double y = static_cast<double>(ps_ldf32(&got[r.start + i]));
                peak = std::max(peak, std::fabs(x));
                worst = std::max(worst, std::fabs(x - y));
            }
            const double rel = peak > 0 ? worst / peak : worst;
            c.f32 = std::max(c.f32, rel);
            if (rel > r.tolerance) c.differs += std::format(" floats at {:05x}: {:.3g} of {:.3g};", r.start, rel, peak);
        }
    }
    for (u32 i = 0; i < PS_SPU_LS_SIZE; i++)
        if (!covered[i] && want[i] != got[i]) {
            c.differs += std::format(" local store at {:05x}: {:02x} vs {:02x};", i, want[i], got[i]);
            break;
        }
    return c;
}

std::vector<u8> fresh_ls()
{
    std::vector<u8> ls(PS_SPU_LS_SIZE, 0);
    Rng r(7);
    for (u8& b : ls) b = static_cast<u8>(r.below(256)); // the job's buffers are not clear
    // the state starts silent, the filterbank at 0
    std::memset(ls.data() + kState0, 0, 4992);
    std::memset(ls.data() + kState1, 0, 4480);
    ps_st32(ls.data() + kBo, 0);
    return ls;
}

std::vector<int16_t> pcm_of(const std::vector<u8>& ls, int ch)
{
    const u32 bytes = ps_ld32(ls.data() + kPcmBytes);
    std::vector<int16_t> v;
    if (bytes == 0xFFFFFFFFu) return v;
    for (u32 i = 0; i < bytes; i += 2) v.push_back(static_cast<int16_t>(ps_ld16(ls.data() + kHeap + 2304 + 4096 * static_cast<u32>(ch) + i)));
    return v;
}

struct Case {
    const char* name;
    mp3::Options o;
};

std::vector<Case> cases()
{
    std::vector<Case> c;
    const auto add = [&](const char* name, auto f) {
        mp3::Options o;
        f(o);
        c.push_back({name, o});
    };
    add("MPEG-1 44.1 kHz joint stereo (M/S)", [](mp3::Options& o) { o.mode = 1; });
    add("MPEG-1 48 kHz mono", [](mp3::Options& o) { o.rate_index = 1; o.mode = 3; });
    add("MPEG-1 32 kHz stereo, CRC", [](mp3::Options& o) { o.rate_index = 2; o.crc = true; });
    add("MPEG-1 44.1 kHz dual channel, long blocks", [](mp3::Options& o) { o.mode = 2; o.block_types = 0; });
    add("MPEG-1 48 kHz joint stereo without M/S, short blocks", [](mp3::Options& o) {
        o.rate_index = 1; o.mode = 1; o.ms = false; o.block_types = 100; o.mixed = 0; });
    add("MPEG-1 44.1 kHz mono, mixed blocks, large values", [](mp3::Options& o) {
        o.mode = 3; o.block_types = 100; o.mixed = 100; o.linbits_tables = 60; });
    add("MPEG-2 22.05 kHz stereo", [](mp3::Options& o) { o.version = 2; });
    add("MPEG-2 24 kHz mono, CRC", [](mp3::Options& o) { o.version = 2; o.rate_index = 1; o.mode = 3; o.crc = true; });
    add("MPEG-2 16 kHz joint stereo", [](mp3::Options& o) { o.version = 2; o.rate_index = 2; o.mode = 1; });
    add("MPEG-2.5 11.025 kHz mono", [](mp3::Options& o) { o.version = 25; o.mode = 3; });
    add("MPEG-2.5 8 kHz stereo, short blocks", [](mp3::Options& o) { o.version = 25; o.rate_index = 2; o.block_types = 80; });
    add("MPEG-2.5 12 kHz joint stereo", [](mp3::Options& o) { o.version = 25; o.rate_index = 1; o.mode = 1; });
    return c;
}

} // namespace

TEST_CASE("flashMP3 native: tags, a Xing frame, and what it declines (D-974)", "[spu][mp3]")
{
    Rng r(1);
    mp3::Options o;
    const mp3::Stream s = mp3::stream(r, o, 2);
    // ID3v2: skipped by its size, no PCM, $3 0
    {
        std::vector<u8> ls = fresh_ls();
        std::vector<u8> tag = {'I', 'D', '3', 3, 0, 0, 0, 0, 2, 1};
        tag.resize(300, 0);
        SPUContext ctx{};
        REQUIRE(call_native(ls, tag, &ctx) == 1);
        CHECK(ps_ld32(ls.data() + kConsumed) == 10 + 0x101);
        CHECK(ps_ld32(ls.data() + kPcmBytes) == 0);
        CHECK(ctx.gpr[3].u32[3] == 0);
        CHECK(ctx.pc == kReturn);
    }
    // ID3v1
    {
        std::vector<u8> ls = fresh_ls();
        std::vector<u8> tag = {'T', 'A', 'G'};
        tag.resize(128, 'x');
        REQUIRE(call_native(ls, tag) == 1);
        CHECK(ps_ld32(ls.data() + kConsumed) == 128);
    }
    // A frame: consumed its size, PCM of 1152 samples a channel at the heap + 2304
    {
        std::vector<u8> ls = fresh_ls();
        SPUContext ctx{};
        REQUIRE(call_native(ls, s.frames[0], &ctx) == 1);
        CHECK(ps_ld32(ls.data() + kConsumed) == s.frames[0].size());
        CHECK(ps_ld32(ls.data() + kPcmBytes) == 2304);
        CHECK(ctx.gpr[3].u32[3] == kHeap + 2304);
    }
    // A Xing frame: decoded, -1 PCM bytes
    {
        std::vector<u8> f = s.frames[0];
        std::memcpy(f.data() + 4 + 32, "Xing", 4);
        std::vector<u8> ls = fresh_ls();
        REQUIRE(call_native(ls, f) == 1);
        CHECK(ps_ld32(ls.data() + kPcmBytes) == 0xFFFFFFFFu);
    }
    // Declined, local store untouched: a broken header (the module prints), intensity stereo (the module refuses it),
    // a reserved rate, a layer II header
    const auto declined = [&](const std::vector<u8>& f) {
        std::vector<u8> before = fresh_ls();
        SPUContext tmp{};
        set_up(before, tmp, f);
        ps_spu_host_detach(&tmp);
        std::vector<u8> after = before;
        CHECK(call_native(after, f) == 0);
        CHECK(after == before);
    };
    {
        std::vector<u8> f = s.frames[0];
        f[0] = 0x12;
        declined(f);
    }
    {
        mp3::Options js;
        js.mode = 1;
        Rng r2(2);
        std::vector<u8> f = mp3::stream(r2, js, 1).frames[0];
        f[3] = static_cast<u8>((f[3] & ~0x30) | 0x10); // mode extension: intensity stereo
        declined(f);
    }
    {
        std::vector<u8> f = s.frames[0];
        f[2] |= 0x0C; // rate index 3
        declined(f);
    }
    {
        std::vector<u8> f = s.frames[0];
        f[1] = static_cast<u8>((f[1] & ~0x06) | 0x04); // layer II
        declined(f);
    }
}

#if PSPOCKET_HAVE_FFMPEG
TEST_CASE("flashMP3 native: the PCM of synthetic streams against FFmpeg's decoder (D-974)", "[spu][mp3]")
{
    for (const Case& k : cases()) {
        DYNAMIC_SECTION(k.name)
        {
            Rng r(11);
            mp3::Options o = k.o;
            // What the module (and so the native decoder) does otherwise than the standard, and FFmpeg as the standard,
            // left out: its silent long band 21, its 24 kHz band 18 at 330, its antialiasing of short blocks and its
            // MPEG-2 region and partition tables (flashmp3.c): long blocks, the lines below 280, the first partition.
            o.max_big = 60;
            o.count1_max = 40;
            o.block_types = 0;
            o.lsf_partitions = false;
            o.gain_min = 100;
            o.gain_max = 140; // below full scale: the clipping of either is not what is compared
            const mp3::Stream s = mp3::stream(r, o, 24);
            const AVCodec* codec = avcodec_find_decoder_by_name("mp3float");
            REQUIRE(codec);
            AVCodecContext* av = avcodec_alloc_context3(codec);
            REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
            AVPacket* pkt = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            std::vector<u8> ls = fresh_ls();
            int worst = 0;
            size_t compared = 0;
            for (const auto& f : s.frames) {
                REQUIRE(call_native(ls, f) == 1);
                std::vector<u8> data(f);
                data.resize(f.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
                pkt->data = data.data();
                pkt->size = static_cast<int>(f.size());
                REQUIRE(avcodec_send_packet(av, pkt) == 0);
                REQUIRE(avcodec_receive_frame(av, frame) == 0);
                const int channels = frame->ch_layout.nb_channels;
                for (int ch = 0; ch < channels; ch++) {
                    const std::vector<int16_t> mine = pcm_of(ls, ch);
                    REQUIRE(mine.size() == static_cast<size_t>(frame->nb_samples));
                    const float* ref = reinterpret_cast<const float*>(frame->extended_data[ch]);
                    for (int i = 0; i < frame->nb_samples; i++) {
                        const double want = std::clamp(static_cast<double>(ref[i]) * 32768.0, -32767.0, 32767.0);
                        worst = std::max(worst, static_cast<int>(std::ceil(std::fabs(want - mine[static_cast<size_t>(i)]))));
                        compared++;
                    }
                }
                av_frame_unref(frame);
            }
            av_frame_free(&frame);
            av_packet_free(&pkt);
            avcodec_free_context(&av);
            INFO(compared << " samples, the largest difference " << worst << " steps");
            CHECK(compared > 0);
            CHECK(worst <= 2);
        }
    }
}
#endif

TEST_CASE("flashMP3 native against the module itself, from the user's firmware (D-974)", "[spu][mp3]")
{
    const std::optional<std::vector<u8>> module = firmware_module();
    if (!module) SKIP("PSPOCKET_TEST_DEV_FLASH is not set (or has no sys/external/flashMP3.pic): no module to compare with");
    for (const Case& k : cases()) {
        DYNAMIC_SECTION(k.name)
        {
            Rng r(23);
            const mp3::Stream s = mp3::stream(r, k.o, 16);
            std::vector<u8> sony = fresh_ls(), mine_alone = fresh_ls();
            int worst_call = 0, worst_stream = 0;
            double worst_f32 = 0;
            for (size_t n = 0; n < s.frames.size(); n++) {
                // the call from the module's own state: each of the native's against the module's
                std::vector<u8> entry = sony;
                std::memcpy(entry.data() + kModule, module->data(), module->size());
                std::vector<u8> mine = entry;
                SPUContext ctx_sony{}, ctx_mine{};
                REQUIRE(call_module(sony, *module, s.frames[n], &ctx_sony) == 1);
                REQUIRE(call_native(mine, s.frames[n], &ctx_mine) == 1);
                // the entry state as both saw it (the frame and the return in place)
                SPUContext entry_ctx{};
                set_up(entry, entry_ctx, s.frames[n]);
                const Compared c = compare(sony, mine, entry_ctx, entry.data());
                ps_spu_host_detach(&entry_ctx);
                INFO("frame " << n << ":" << c.differs);
                CHECK(c.differs.empty());
                // PSPOCKET_MP3_DUMP=DIR: a call that differs, its local store at entry, the module's and the native's
                if (const char* dir = std::getenv("PSPOCKET_MP3_DUMP"); dir && !c.differs.empty()) {
                    const std::string base = std::format("{}/{}-{}", dir, k.name, n);
                    std::ofstream(base + ".in", std::ios::binary).write(reinterpret_cast<const char*>(entry.data()), PS_SPU_LS_SIZE);
                    std::ofstream(base + ".want", std::ios::binary).write(reinterpret_cast<const char*>(sony.data()), PS_SPU_LS_SIZE);
                    std::ofstream(base + ".got", std::ios::binary).write(reinterpret_cast<const char*>(mine.data()), PS_SPU_LS_SIZE);
                    std::ofstream(base + ".frame", std::ios::binary)
                        .write(reinterpret_cast<const char*>(s.frames[n].data()), static_cast<std::streamsize>(s.frames[n].size()));
                }
                CHECK(std::memcmp(&ctx_sony.gpr[3], &ctx_mine.gpr[3], 16) == 0);
                CHECK(ctx_mine.pc == ctx_sony.pc);
                worst_call = std::max(worst_call, c.pcm);
                worst_f32 = std::max(worst_f32, c.f32);
                // the native decoder on its own state through the stream
                REQUIRE(call_native(mine_alone, s.frames[n]) == 1);
                for (int ch = 0; ch < 2; ch++) {
                    const std::vector<int16_t> a = pcm_of(sony, ch), b = pcm_of(mine_alone, ch);
                    REQUIRE(a.size() == b.size());
                    for (size_t i = 0; i < a.size(); i++) worst_stream = std::max(worst_stream, std::abs(a[i] - b[i]));
                }
            }
            INFO("the largest 16-bit difference: " << worst_call << " a call, " << worst_stream << " through the stream; floats "
                                                     << worst_f32 << " of their peak");
            CHECK(worst_call <= 1);
            CHECK(worst_stream <= 2);
        }
    }
}
