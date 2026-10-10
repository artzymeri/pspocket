// cellAdec's own cores on FFmpeg (D-577): the firmware's core module first, pspocket's where none is linked, and each
// of pspocket's cores through the front end on streams built here (no game data). A codec FFmpeg has an LGPL encoder
// for is written by it in the test (the tests' own FFmpeg, PSPOCKET_FFMPEG_ENCODERS) and its PCM checked against the
// signal put in; the others are hand-built frames whose PCM is checked against FFmpeg's own decoding of them and for
// the tone each frame codes. Every answer, callback, PCM item (its size, PTS and user data) and information block.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "adec_test_util.hpp"
#include "hle_test_util.hpp"

#if PSPOCKET_HAVE_FFMPEG
#include "adec_ffmpeg.hpp" // runtime/hle/src (internal): the cores' codecs
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::adec_test;

namespace {

constexpr u32 kTypeMp3 = 4;

// Only the rule's test below runs without FFmpeg: the other tests, and their helpers here and in each codec's section,
// are left out of a build without it (unused helpers would stop its -Werror build, D-606).
#if PSPOCKET_HAVE_FFMPEG

constexpr u32 kTsInvalid = 0xFFFFFFFF;

// Bits written most significant first.
struct BitWriter {
    std::vector<u8> bytes;
    u32 bit = 0;
    void put(u32 v, u32 n)
    {
        for (u32 i = 0; i < n; i++, bit++) {
            if (bit / 8 >= bytes.size()) bytes.push_back(0);
            if ((v >> (n - 1 - i)) & 1) bytes[bit / 8] |= static_cast<u8>(0x80 >> (bit % 8));
        }
    }
};

std::vector<u8> be32s(std::initializer_list<u32> words)
{
    std::vector<u8> v;
    for (const u32 w : words)
        for (u32 k = 0; k < 4; k++) v.push_back(static_cast<u8>(w >> (24 - 8 * k)));
    return v;
}

u32 be32(const std::vector<u8>& v, usize at)
{
    return u32{v[at]} << 24 | u32{v[at + 1]} << 16 | u32{v[at + 2]} << 8 | v[at + 3];
}

// One PCM out of a decoder: its item and information block, and the PCM the game took.
struct Out {
    u32 handle = 0, status = 0, size = 0, pts_upper = 0, pts_lower = 0;
    u64 user_data = 0;
    std::vector<u8> bsi;
    std::vector<u8> pcm;
};

// A decoder's life on a core, through pspocket's cellAdec: what it answers, its PCM outs, its callbacks.
struct CoreRun {
    std::vector<std::string> lines; // the calls and their answers
    u32 mem = 0, version_upper = 0, version_lower = 0;
    std::vector<Out> outs;
    std::vector<std::pair<u32, u32>> callbacks; // (type, data), as the game's callback got them
    u32 count(u32 type) const
    {
        u32 n = 0;
        for (const auto& [t, d] : callbacks) n += t == type;
        return n;
    }
};

// The access units' PTS: given for all but `missing` (0x1000 plus `step` each), user data 0x77000000 + index.
struct Feed {
    std::vector<std::vector<u8>> aus;
    std::vector<u32> missing; // indices without a PTS: the front end's (or the core's) correction
    u32 step = 2160;
    u32 bsi_size = 0x40;
};

CoreRun run_core(u8* b, const Call& call, u32 type, const std::vector<u8>& param, const Feed& feed)
{
    CoreRun run;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        run.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    std::memset(b + kArgs, 0, 0x200);
    write_callback(b);
    ps_write32(b, kType, type);
    if (r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr}) != 0) return run;
    run.mem = ps_read32(b, kAttr);
    run.version_upper = ps_read32(b, kAttr + 4);
    run.version_lower = ps_read32(b, kAttr + 8);
    const u32 res[5] = {run.mem, kDecoderMem, 1000, 200, 0x4000};
    for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    if (r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) != 0) return run;
    const u32 handle = ps_read32(b, kHandle);
    std::memcpy(b + kParam, param.data(), param.size());
    if (r("cellAdecStartSeq", 0x487B613E, {handle, kParam}) == 0) {
        for (u32 k = 0; k < feed.aus.size(); k++) {
            const u32 at = kStream + 0x2000 * (k % 16);
            std::memcpy(b + at, feed.aus[k].data(), feed.aus[k].size());
            const bool missing = std::find(feed.missing.begin(), feed.missing.end(), k) != feed.missing.end();
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(feed.aus[k].size()));
            ps_write32(b, kAu + 8, missing ? kTsInvalid : 0);
            ps_write32(b, kAu + 12, missing ? kTsInvalid : 0x1000 + feed.step * k);
            ps_write64(b, kAu + 16, 0x77000000 + k);
            if (r("cellAdecDecodeAu", 0x1529E506, {handle, kAu}) != 0) continue;
            if (!wait_until([&] { return count_of(b, 1) >= k + 1; })) run.lines.push_back("no PCMOUT");
            ps_write32(b, kItemOut, 0);
            if (r("cellAdecGetPcmItem", 0xBD75F78B, {handle, kItemOut}) != 0) continue;
            const u32 item = ps_read32(b, kItemOut);
            Out o;
            o.handle = ps_read32(b, item);
            o.status = ps_read32(b, item + 4);
            o.size = ps_read32(b, item + 0xC);
            const u32 bsi = ps_read32(b, item + 0x10);
            o.bsi.assign(b + bsi, b + bsi + feed.bsi_size);
            o.pts_upper = ps_read32(b, item + 0x18 + 8);
            o.pts_lower = ps_read32(b, item + 0x18 + 12);
            o.user_data = ps_read64(b, item + 0x18 + 16);
            std::memset(b + kPcm, 0xCC, 0x20000);
            if (r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}) == 0) o.pcm.assign(b + kPcm, b + kPcm + std::min<u32>(o.size, 0x20000));
            run.outs.push_back(std::move(o));
        }
        r("cellAdecEndSeq", 0xE2EA549B, {handle});
        if (!wait_until([&] { return count_of(b, 3) >= 1; })) run.lines.push_back("no SEQDONE");
        r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}); // nothing left: EMPTY
    }
    for (u32 n = 0; n < log_count(b); n++)
        run.callbacks.emplace_back(ps_read32(b, kLog + 16 + 16 * n), ps_read32(b, kLog + 20 + 16 * n));
    r("cellAdecClose", 0x847D2380, {handle});
    return run;
}

std::string all(const CoreRun& run)
{
    std::string s;
    for (const std::string& l : run.lines) s += l + "\n";
    for (const auto& [t, d] : run.callbacks) s += std::format("callback {} {:#x}\n", t, d);
    return s;
}

// The callbacks of one access unit each: AUDONE, then PCMOUT with CELL_OK; or for a unit that failed, ERROR with its
// error first and PCMOUT with it.
bool flow_ok(const CoreRun& run, u32 aus, const std::vector<std::pair<u32, u32>>& errors)
{
    std::vector<std::pair<u32, u32>> want;
    for (u32 k = 0; k < aus; k++) {
        u32 e = 0;
        for (const auto& [at, code] : errors)
            if (at == k) e = code;
        if (e) want.emplace_back(2, e);
        want.emplace_back(0, 0); // AUDONE's data is the frame's AU information: not compared
        want.emplace_back(1, e);
    }
    want.emplace_back(3, 0);
    if (want.size() != run.callbacks.size()) return false;
    for (usize i = 0; i < want.size(); i++) {
        if (want[i].first != run.callbacks[i].first) return false;
        if (want[i].first != 0 && want[i].second != run.callbacks[i].second) return false;
    }
    return true;
}

// The share of `x`'s energy within `width` Hz of `freq`, scanned at 5 Hz steps up to `rate` / 2 (Goertzel).
double energy_near(const std::vector<float>& x, double rate, double freq, double width)
{
    double near = 0, total = 0;
    for (double f = 5; f < rate / 2; f += 5) {
        const double coeff = 2 * std::cos(2 * std::numbers::pi * f / rate);
        double s1 = 0, s2 = 0;
        for (const float v : x) {
            const double s0 = static_cast<double>(v) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double e = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        total += e;
        if (std::abs(f - freq) <= width) near += e;
    }
    return total > 0 ? near / total : 0;
}

// Planar floats from FFmpeg's own decoder (the reference): each unit in order, every channel's samples per unit.
std::vector<std::vector<std::vector<float>>> ffmpeg_decode(const char* decoder, const std::vector<std::vector<u8>>& aus,
    const std::function<void(AVCodecContext*)>& setup = {})
{
    const AVCodec* codec = avcodec_find_decoder_by_name(decoder);
    REQUIRE(codec);
    AVCodecContext* av = avcodec_alloc_context3(codec);
    if (setup) setup(av);
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<std::vector<std::vector<float>>> out;
    for (const auto& au : aus) {
        std::vector<u8> data(au);
        data.resize(au.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        pkt->data = data.data();
        pkt->size = static_cast<int>(au.size());
        std::vector<std::vector<float>> unit;
        if (avcodec_send_packet(av, pkt) == 0) {
            while (avcodec_receive_frame(av, frame) == 0) {
                unit.resize(static_cast<usize>(frame->ch_layout.nb_channels));
                for (usize c = 0; c < unit.size(); c++) {
                    const float* s = reinterpret_cast<const float*>(frame->extended_data[c]);
                    unit[c].insert(unit[c].end(), s, s + frame->nb_samples);
                }
                av_frame_unref(frame);
            }
        }
        out.push_back(std::move(unit));
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return out;
}

// The core's PCM for planar floats: interleaved big-endian floats (held below 1) or 16-bit samples (floored), as
// adec_ffmpeg.cpp writes them.
std::vector<u8> expected_pcm(const std::vector<std::vector<float>>& unit, bool s16)
{
    std::vector<u8> v;
    const usize n = unit.empty() ? 0 : unit[0].size();
    for (usize i = 0; i < n; i++) {
        for (const auto& ch : unit) {
            const float s = ch[i];
            if (s16) {
                const s32 q = s >= 1.f ? 0x7FFF : s <= -1.f ? -0x8000 : static_cast<s32>(std::floor(s * 32768.f));
                v.push_back(static_cast<u8>(static_cast<u32>(q) >> 8));
                v.push_back(static_cast<u8>(q));
            } else {
                u32 bits;
                const float c = s <= -1.f ? -1.f : s;
                std::memcpy(&bits, &c, 4);
                if (s >= std::bit_cast<float>(std::bit_cast<u32>(1.f) - 1)) bits = 0x3F7FFFFF;
                for (u32 k = 0; k < 4; k++) v.push_back(static_cast<u8>(bits >> (24 - 8 * k)));
            }
        }
    }
    return v;
}

// ---- MP3

// An MPEG-1 (or, `lsf`, MPEG-2) layer III frame at 128 kbit/s (64 for MPEG-2): in each granule and channel one spectral
// line, lines[ch], quantised to 1 and scaled by `gain` (global_gain), coded in the count1 region with table B, every
// other line 0; no bit reservoir (main_data_begin 0). `crc` protects it; `bad_block` switches the first granule's
// window with block type 0 (forbidden). `rate_index` and the header fields are as the frame header codes them.
struct Mp3Spec {
    u32 channels = 2;
    u32 rate_index = 1; // 48 kHz (MPEG-1), 24 kHz (MPEG-2)
    bool lsf = false;
    std::array<u32, 2> lines = {45, 100};
    u32 gain = 200;
    bool crc = false;
    bool bad_block = false;
    bool negative = false;
};

std::vector<u8> mp3_frame(const Mp3Spec& s)
{
    static constexpr u32 kRates[2][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}};
    const u32 rate = kRates[s.lsf][s.rate_index];
    const u32 kbps = s.lsf ? 64 : 128;
    const u32 frame_bytes = (s.lsf ? 72 : 144) * 1000 * kbps / rate;
    BitWriter w;
    w.put(0x7FF, 11);
    w.put(s.lsf ? 2 : 3, 2); // version
    w.put(1, 2);             // layer III
    w.put(s.crc ? 0 : 1, 1); // protection_bit
    w.put(s.lsf ? 8 : 9, 4); // 64 or 128 kbit/s
    w.put(s.rate_index, 2);
    w.put(0, 1); // padding
    w.put(0, 1); // private
    w.put(s.channels == 1 ? 3 : 0, 2);
    w.put(0, 2); // mode extension
    w.put(0, 1); // copyright
    w.put(1, 1); // original
    w.put(0, 2); // emphasis
    if (s.crc) w.put(0, 16); // filled below
    const u32 granules = s.lsf ? 1 : 2;
    const auto part23 = [&](u32 ch) { return 4 * (s.lines[ch] / 4) + 5; }; // zero quadruples, the line's, its sign
    // Side information.
    w.put(0, s.lsf ? 8 : 9); // main_data_begin
    w.put(0, s.lsf ? (s.channels == 1 ? 1 : 2) : (s.channels == 1 ? 5 : 3));
    if (!s.lsf) w.put(0, 4 * s.channels); // scfsi
    for (u32 gr = 0; gr < granules; gr++) {
        for (u32 ch = 0; ch < s.channels; ch++) {
            w.put(part23(ch), 12);
            w.put(0, 9); // big_values
            w.put(s.gain, 8);
            w.put(0, s.lsf ? 9 : 4); // scalefac_compress: no scale factors
            if (s.bad_block && gr == 0 && ch == 0) {
                w.put(1, 1); // window_switching_flag
                w.put(0, 2); // block_type 0: forbidden with it
                w.put(0, 1 + 10 + 9);
            } else {
                w.put(0, 1);
                w.put(0, 15 + 4 + 3);
            }
            if (!s.lsf) w.put(0, 1); // preflag
            w.put(0, 1);             // scalefac_scale
            w.put(1, 1);             // count1table_select: table B
        }
    }
    const u32 side_end = w.bit;
    // Main data.
    for (u32 gr = 0; gr < granules; gr++) {
        for (u32 ch = 0; ch < s.channels; ch++) {
            const u32 line = s.lines[ch];
            for (u32 q = 0; q < line / 4; q++) w.put(15, 4); // (0, 0, 0, 0)
            w.put(15 - (8u >> (line % 4)), 4);
            w.put(s.negative ? 1 : 0, 1);
        }
    }
    std::vector<u8> f = w.bytes;
    f.resize(frame_bytes, 0);
    if (s.crc) {
        // CRC-16 (x^16 + x^15 + x^2 + 1, from all ones) over the header's second half and the side information.
        u16 crc = 0xFFFF;
        const auto feed = [&](u32 from_bit, u32 to_bit) {
            for (u32 i = from_bit; i < to_bit; i++) {
                const bool in = (f[i / 8] >> (7 - i % 8)) & 1;
                const bool top = crc & 0x8000;
                crc = static_cast<u16>(crc << 1);
                if (in != top) crc ^= 0x8005;
            }
        };
        feed(16, 32);
        feed(48, side_end);
        f[4] = static_cast<u8>(crc >> 8);
        f[5] = static_cast<u8>(crc);
    }
    return f;
}

std::vector<u8> mp3_param(u32 bw_pcm) { return be32s({bw_pcm}); }

// The interleaved channel `c` of `channels` from big-endian PCM.
std::vector<float> channel_of(const std::vector<u8>& pcm, u32 channels, u32 c, bool s16)
{
    std::vector<float> x;
    const u32 bytes = s16 ? 2 : 4;
    for (usize i = c * bytes; i + bytes <= pcm.size(); i += channels * bytes) {
        if (s16) {
            x.push_back(static_cast<float>(static_cast<int16_t>(static_cast<u16>(pcm[i] << 8 | pcm[i + 1]))) / 32768.f);
        } else {
            const u32 bits = be32(pcm, i);
            float f;
            std::memcpy(&f, &bits, 4);
            x.push_back(f);
        }
    }
    return x;
}

#endif

} // namespace

// ---- the rule: the firmware's core first, pspocket's where none is linked

namespace {

// A firmware core's operation table, as guest code: getMemSize writes 0x22222, getVersion 0x00ABCDEF,
// getPcmHandleNum is 2 and getBsiInfoSize 0x20; open, close, startSeq, endSeq, decodeAu, realign and releasePcm count
// their calls (a word each at kFakeCalls + 4 * op) and answer CELL_OK.
constexpr u32 kFakeCode = kData + 0x6000, kFakeOpds = kData + 0x7000, kFakeTable = kData + 0x7400,
              kFakeCalls = kData + 0x7800;

void write_fake_core(u8* b)
{
    std::vector<u32> code;
    std::array<u32, 12> entry{};
    const auto load = [&](u32 reg, u32 value) {
        code.push_back(as::lis(reg, static_cast<s32>(static_cast<s16>(value >> 16))));
        code.push_back(as::ori(reg, reg, value & 0xFFFF));
    };
    for (u32 op = 0; op < 12; op++) {
        entry[op] = kFakeCode + 4 * static_cast<u32>(code.size());
        load(11, kFakeCalls + 4 * op); // count the call
        code.push_back(as::lwz(12, 0, 11));
        code.push_back(as::addi(12, 12, 1));
        code.push_back(as::stw(12, 0, 11));
        switch (op) {
        case 0: // getMemSize(attr)
            load(12, 0x22222);
            code.push_back(as::stw(12, 0, 3));
            code.push_back(as::li(3, 0));
            break;
        case 6: // getVersion(version)
            load(12, 0x00ABCDEF);
            code.push_back(as::stw(12, 0, 3));
            break;
        case 9: code.push_back(as::li(3, 2)); break;    // getPcmHandleNum
        case 10: code.push_back(as::li(3, 0x20)); break; // getBsiInfoSize
        default: code.push_back(as::li(3, 0)); break;
        }
        code.push_back(as::blr());
    }
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kFakeCode + 4 * static_cast<u32>(i), code[i]);
    for (u32 op = 0; op < 12; op++) {
        ps_write32(b, kFakeOpds + 8 * op, entry[op]);
        ps_write32(b, kFakeOpds + 8 * op + 4, 0);
        ps_write32(b, kFakeTable + 4 * op, kFakeOpds + 8 * op);
        ps_write32(b, kFakeCalls + 4 * op, 0);
    }
}

} // namespace

TEST_CASE("cellAdec: a codec's firmware core serves first, pspocket's own where no module gives one (D-577)", "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Call call = hle_calls(f, t);
    ps_write32(b, kType, kTypeMp3);
#if PSPOCKET_HAVE_FFMPEG
    // No firmware module: pspocket's MP3 core (its context and a 1152-sample stereo float frame, three PCM handles of
    // CellAdecMP3Info, version 0x01000000).
    REQUIRE(call("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr}) == 0);
    const u32 own_mem = ((0x200 + 1152 * 2 * 4 + 0x7F) & ~0x7Fu) + 0x80;
    CHECK(ps_read32(b, kAttr) == own_mem + (0x1C + 0x68) * 3 + 0x530 + 0x7F);
    CHECK(ps_read32(b, kAttr + 4) == 0x492000);
    CHECK(ps_read32(b, kAttr + 8) == 0x01000000);
#else
    CHECK(call("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr}) == 0x80610001); // none of its own: FATAL
#endif
    // The loader linked a firmware core (here a fake table): its sizes and version, and a decoder opened on it stays on
    // it: Open, StartSeq, DecodeAu, EndSeq and Close reach its operations.
    write_fake_core(b);
    f.hle->link_import("cellMP3dec", 0x84276A23, kFakeTable);
    REQUIRE(call("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr}) == 0);
    CHECK(ps_read32(b, kAttr) == 0x22222 + (0x20 + 0x68) * 2 + 0x530 + 0x7F);
    CHECK(ps_read32(b, kAttr + 8) == 0x00ABCDEF);
    const u32 res[5] = {ps_read32(b, kAttr), kDecoderMem, 1000, 200, 0x4000};
    for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
    write_callback(b);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    REQUIRE(call("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) == 0);
    const u32 handle = ps_read32(b, kHandle);
    ps_write32(b, kParam, 4);
    CHECK(call("cellAdecStartSeq", 0x487B613E, {handle, kParam}) == 0);
    ps_write32(b, kAu, kStream);
    ps_write32(b, kAu + 4, 0x180);
    ps_write32(b, kAu + 8, 0);
    ps_write32(b, kAu + 12, 0);
    CHECK(call("cellAdecDecodeAu", 0x1529E506, {handle, kAu}) == 0);
    CHECK(call("cellAdecEndSeq", 0xE2EA549B, {handle}) == 0);
    CHECK(call("cellAdecClose", 0x847D2380, {handle}) == 0);
    for (const u32 op : {1u, 2u, 3u, 4u, 5u}) CHECK(ps_read32(b, kFakeCalls + 4 * op) == 1); // open .. decodeAu
}

#if PSPOCKET_HAVE_FFMPEG

TEST_CASE("cellAdec: MP3 on FFmpeg, frames built here: PCM, items, information, PTS and errors (D-577)", "[hle][adec]")
{
    InterpretMisses interpret;
    for (const bool s16 : {false, true}) {
        DYNAMIC_SECTION((s16 ? "16-bit" : "float") << " PCM")
        {
            BigFixture f;
            GuestThread t(f, 0, "ppu");
            // Eight stereo frames at 48 kHz (lines 45 and 100: 1896 and 4187 Hz), the third without a PTS, the
            // sixth broken (a layer II header), the seventh with a CRC.
            Feed feed;
            feed.bsi_size = 0x1C;
            std::vector<std::vector<u8>> valid;
            for (u32 k = 0; k < 8; k++) {
                std::vector<u8> au = mp3_frame({.crc = k == 6});
                if (k == 5) au[1] = static_cast<u8>((au[1] & ~0x06) | 0x04); // layer II
                else valid.push_back(au);
                feed.aus.push_back(std::move(au));
            }
            feed.missing = {2};
            const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMp3, mp3_param(s16 ? 3 : 4), feed);
            INFO(all(run));
            REQUIRE(run.outs.size() == 8);
            CHECK(flow_ok(run, 8, {{5, 0x80612783}})); // NOT_L3
            const auto reference = ffmpeg_decode("mp3float", valid);
            u32 v = 0;
            for (u32 k = 0; k < 8; k++) {
                const Out& o = run.outs[k];
                INFO("access unit " << k);
                CHECK(o.user_data == 0x77000000 + k);
                CHECK(o.pts_upper == 0);
                CHECK(o.pts_lower == 0x1000 + 2160 * k); // the third's from the second's plus 1152 samples at 48 kHz
                if (k == 5) {
                    CHECK(o.size == 0);
                    CHECK(be32(o.bsi, 0x18) == 0x80612783); // i_error_code
                    continue;
                }
                CHECK(o.size == 1152 * 2 * (s16 ? 2 : 4));
                CHECK(o.pcm == expected_pcm(reference[v++], s16));
                CHECK(be32(o.bsi, 0) == (be32(feed.aus[k], 0)));               // ui_header
                CHECK(be32(o.bsi, 4) == 0);                                     // main_data_begin
                const u32 now = 384 - 4 - (k == 6 ? 2 : 0) - 32;                // past header, CRC, side information
                CHECK(be32(o.bsi, 0xC) == now);
                CHECK(be32(o.bsi, 8) == now - (2 * ((4 * 11 + 5) + (4 * 25 + 5)) + 7) / 8); // the reservoir left
                CHECK(o.bsi[0x10] == (k == 6 ? 0 : 1));                         // uc_crc: MP3_CRC 0, MP3_NO_CRC 1
                CHECK(o.bsi[0x11] == 0);                                        // stereo
                CHECK(o.bsi[0x14] == 1);                                        // original
                CHECK(o.bsi[0x16] == 0);                                        // the CRC matched
                CHECK(be32(o.bsi, 0x18) == 0);
            }
            // The sound: each channel's tone where its line puts it ((line + 0.5) * 48000 / 1152 Hz).
            std::vector<u8> pcm;
            for (u32 k = 3; k < 5; k++) pcm.insert(pcm.end(), run.outs[k].pcm.begin(), run.outs[k].pcm.end());
            const auto left = channel_of(pcm, 2, 0, s16), right = channel_of(pcm, 2, 1, s16);
            CHECK(energy_near(left, 48000, 45.5 * 48000 / 1152, 200) > 0.8);
            CHECK(energy_near(right, 48000, 100.5 * 48000 / 1152, 200) > 0.8);
            float peak = 0;
            for (const float x : left) peak = std::max(peak, std::abs(x));
            CHECK(peak > 0.01f);
        }
    }
}

TEST_CASE("cellAdec: MP3 on FFmpeg, mono and the lower rates, CRC errors, refused frames and parameters (D-577)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // Mono at 44.1 kHz: 1152 samples a frame, PTS steps of 2351; MPEG-2 at 22.05 kHz: 576 samples, the same step.
    {
        Feed feed;
        feed.bsi_size = 0x1C;
        feed.step = 2351;
        for (u32 k = 0; k < 4; k++) feed.aus.push_back(mp3_frame({.channels = 1, .rate_index = 0, .lines = {60, 0}}));
        for (u32 k = 0; k < 3; k++)
            feed.aus.push_back(mp3_frame({.channels = 1, .rate_index = 0, .lsf = true, .lines = {30, 0}}));
        feed.missing = {1, 5};
        const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMp3, mp3_param(4), feed);
        INFO(all(run));
        REQUIRE(run.outs.size() == 7);
        CHECK(flow_ok(run, 7, {}));
        for (u32 k = 0; k < 7; k++) {
            INFO("access unit " << k);
            CHECK(run.outs[k].size == (k < 4 ? 1152 : 576) * 4);
            CHECK(run.outs[k].pts_lower == 0x1000 + 2351 * k);
            CHECK(run.outs[k].bsi[0x11] == 3); // mono
        }
        const auto ref = ffmpeg_decode("mp3float", feed.aus);
        for (u32 k = 0; k < 7; k++) CHECK(run.outs[k].pcm == expected_pcm(ref[k], false));
        CHECK(energy_near(channel_of(run.outs[3].pcm, 1, 0, false), 44100, 60.5 * 44100 / 1152, 200) > 0.8);
    }
    // A protected frame whose CRC is wrong is decoded, flagged; each refused header gets its error, and the frame
    // after them decodes.
    {
        Feed feed;
        feed.bsi_size = 0x1C;
        std::vector<u8> bad_crc = mp3_frame({.crc = true});
        bad_crc[5] ^= 0x40;
        feed.aus.push_back(bad_crc);
        const auto with = [](std::vector<u8> au, u32 byte, u8 mask, u8 value) {
            au[byte] = static_cast<u8>((au[byte] & ~mask) | value);
            return au;
        };
        const std::vector<u8> good = mp3_frame({});
        feed.aus.push_back(with(good, 0, 0xFF, 0x7F));              // no sync: LOST_SYNC
        feed.aus.push_back(with(good, 1, 0x18, 0x00));              // MPEG-2.5: BAD_VERSION
        feed.aus.push_back(with(good, 1, 0x18, 0x08));              // the reserved version: BAD_VERSION
        feed.aus.push_back(with(good, 2, 0xF0, 0xF0));              // bit rate 15: BAD_BITRATE
        feed.aus.push_back(with(good, 2, 0xF0, 0x00));              // free format: BAD_BITRATE
        feed.aus.push_back(with(good, 2, 0x0C, 0x0C));              // the reserved rate: BAD_SFREQ
        feed.aus.push_back(with(good, 3, 0x03, 0x02));              // the reserved emphasis: BAD_EMPHASIS
        feed.aus.push_back(mp3_frame({.bad_block = true}));         // BAD_BLKTYPE
        feed.aus.push_back(std::vector<u8>(good.begin(), good.begin() + 200)); // cut short: MP3_ERROR
        feed.aus.push_back(good);
        const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMp3, mp3_param(3), feed);
        INFO(all(run));
        REQUIRE(run.outs.size() == 11);
        CHECK(flow_ok(run, 11, {{1, 0x80612782}, {2, 0x8061278C}, {3, 0x8061278C}, {4, 0x80612784}, {5, 0x80612784},
            {6, 0x80612785}, {7, 0x80612786}, {8, 0x80612787}, {9, 0x80612781}}));
        CHECK(run.outs[0].size == 1152 * 2 * 2);
        CHECK(run.outs[0].bsi[0x16] == 1); // uc_crc_error_flag
        CHECK(run.outs[0].bsi[0x10] == 0); // protected
        for (u32 k = 1; k < 10; k++) CHECK(run.outs[k].size == 0);
        CHECK(run.outs[10].size == 1152 * 2 * 2);
    }
    // StartSeq refuses a word size MP3 has not.
    {
        Feed feed;
        const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMp3, mp3_param(5), feed);
        CHECK(run.lines[2] == "cellAdecStartSeq -> 0x80610003");
        CHECK(run.lines.back() == "cellAdecClose -> 0x0");
    }
}

#endif

// ---- MPEG audio layers I and II

#if PSPOCKET_HAVE_FFMPEG

namespace {

constexpr u32 kTypeMpegL1 = 22, kTypeMpegL2 = 6;

// An MPEG-1 layer I frame at 48 kHz, 384 kbit/s (384 bytes), stereo or mono: in subband bands[ch] of each channel a
// sinusoid at a quarter of the subband rate (0.9, 0, -0.9, 0 over the frame's 12 samples, 15-bit samples, scale factor
// 6: 0.5), every other subband silent. Its sound is a tone at (band + 0.5) * 750 Hz (in an odd band too: the filter
// bank turns odd bands over, and a quarter is their middle).
std::vector<u8> layer1_tone(u32 channels, std::array<u32, 2> bands)
{
    BitWriter w;
    w.put(0x7FF, 11);                // sync
    w.put(3, 2);                     // MPEG-1
    w.put(3, 2);                     // layer I
    w.put(1, 1);                     // no CRC
    w.put(12, 4);                    // 384 kbit/s
    w.put(1, 2);                     // 48 kHz
    w.put(0, 2);                     // padding, private
    w.put(channels == 1 ? 3 : 0, 2); // mono or stereo
    w.put(0, 4);                     // mode extension, copyright, original
    w.put(0, 2);                     // emphasis
    const auto alloc = [&](u32 sb, u32 ch) { return sb == bands[ch] ? 14u : 0u; };
    for (u32 sb = 0; sb < 32; sb++)
        for (u32 ch = 0; ch < channels; ch++) w.put(alloc(sb, ch), 4);
    for (u32 sb = 0; sb < 32; sb++)
        for (u32 ch = 0; ch < channels; ch++)
            if (alloc(sb, ch)) w.put(6, 6);
    for (u32 j = 0; j < 12; j++) {
        const s32 x = j % 4 == 0 ? 14745 : j % 4 == 2 ? -14745 : 0; // 0.9 of 16383
        for (u32 sb = 0; sb < 32; sb++)
            for (u32 ch = 0; ch < channels; ch++)
                if (alloc(sb, ch)) w.put(static_cast<u32>(x + 16383), 15);
    }
    std::vector<u8> f = w.bytes;
    f.resize(384, 0);
    return f;
}

std::vector<u8> mpmc_param(u32 channel_number) { return be32s({channel_number, 0, 0}); }

// An MPEG audio header's fields the information block repeats.
struct MpegFields {
    u32 crc, bitrate_index, mode;
};
MpegFields mpeg_fields(u32 word) { return {((word >> 16) & 1) ? 0u : 1u, (word >> 12) & 15, (word >> 6) & 3}; }

// Interleaved 16-bit test signal at `rate`, `samples` per channel: in channel c a sine at freqs[c] Hz of amps[c].
std::vector<s16> sines(u32 rate, u32 samples, const std::vector<double>& freqs, const std::vector<double>& amps)
{
    std::vector<s16> v;
    for (u32 i = 0; i < samples; i++)
        for (usize c = 0; c < freqs.size(); c++)
            v.push_back(static_cast<s16>(std::lround(32767.0 * amps[c] *
                std::sin(2 * std::numbers::pi * freqs[c] * static_cast<double>(i) / rate))));
    return v;
}

// What FFmpeg's encoder `name` made of interleaved 16-bit input (given as planar floats to an encoder that takes only
// those): its packets and its configuration (extradata).
struct Encoded {
    std::vector<std::vector<u8>> packets;
    std::vector<u8> extradata;
    u32 frame_size = 0;
};

Encoded ffmpeg_encode(const char* name, u32 rate, u32 channels, s64 bit_rate, const std::vector<s16>& pcm,
    const std::function<void(AVCodecContext*)>& setup = {})
{
    Encoded e;
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return e;
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->sample_rate = static_cast<int>(rate);
    av_channel_layout_default(&av->ch_layout, static_cast<int>(channels));
    av->bit_rate = bit_rate;
    const void* formats = nullptr;
    int count = 0;
    REQUIRE(avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count) >= 0);
    bool planar_float = true;
    for (int i = 0; i < count; i++)
        if (static_cast<const AVSampleFormat*>(formats)[i] == AV_SAMPLE_FMT_S16) planar_float = false;
    av->sample_fmt = planar_float ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
    if (setup) setup(av);
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    e.extradata.assign(av->extradata, av->extradata + av->extradata_size);
    e.frame_size = static_cast<u32>(av->frame_size);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const u32 total = static_cast<u32>(pcm.size() / channels);
    const auto drain = [&] {
        while (avcodec_receive_packet(av, pkt) == 0) {
            e.packets.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    };
    for (u32 at = 0; at + e.frame_size <= total; at += e.frame_size) {
        frame->nb_samples = static_cast<int>(e.frame_size);
        frame->format = av->sample_fmt;
        frame->sample_rate = av->sample_rate;
        REQUIRE(av_channel_layout_copy(&frame->ch_layout, &av->ch_layout) == 0);
        REQUIRE(av_frame_get_buffer(frame, 0) == 0);
        for (u32 i = 0; i < e.frame_size; i++)
            for (u32 c = 0; c < channels; c++) {
                const s16 s = pcm[(at + i) * channels + c];
                if (planar_float) reinterpret_cast<float*>(frame->data[c])[i] = static_cast<float>(s) / 32768.f;
                else reinterpret_cast<s16*>(frame->data[0])[i * channels + c] = s;
            }
        REQUIRE(avcodec_send_frame(av, frame) == 0);
        av_frame_unref(frame);
        drain();
    }
    avcodec_send_frame(av, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return e;
}

// How close `out` (a decoded channel) comes to `in` (the channel put in) once the codec's delay (found by correlation,
// up to `max_delay` samples) is taken out: the signal-to-noise ratio in dB, away from both ends.
double snr_db(const std::vector<float>& in, const std::vector<float>& out, u32 max_delay, u32* delay_out = nullptr)
{
    const usize n = std::min(in.size(), out.size());
    if (n < 2 * static_cast<usize>(max_delay) + 4096) return -999;
    u32 best = 0;
    double best_corr = -1e300;
    for (u32 d = 0; d <= max_delay; d++) {
        double c = 0;
        for (usize i = max_delay; i < max_delay + 4096; i++)
            c += static_cast<double>(in[i]) * static_cast<double>(out[i + d]);
        if (c > best_corr) {
            best_corr = c;
            best = d;
        }
    }
    if (delay_out) *delay_out = best;
    double signal = 0, noise = 0;
    for (usize i = max_delay; i + best + max_delay < n; i++) {
        const double a = static_cast<double>(in[i]), b = static_cast<double>(out[i + best]);
        signal += a * a;
        noise += (a - b) * (a - b);
    }
    return noise > 0 ? 10 * std::log10(signal / noise) : 999;
}

std::vector<float> floats_of(const std::vector<s16>& pcm, u32 channels, u32 c)
{
    std::vector<float> v;
    for (usize i = c; i < pcm.size(); i += channels) v.push_back(static_cast<float>(pcm[i]) / 32768.f);
    return v;
}

// One channel of every PCM out of a run, in order.
std::vector<float> joined(const CoreRun& run, u32 channels, u32 c, bool sixteen = false)
{
    std::vector<float> v;
    for (const Out& o : run.outs) {
        const auto x = channel_of(o.pcm, channels, c, sixteen);
        v.insert(v.end(), x.begin(), x.end());
    }
    return v;
}

} // namespace

TEST_CASE("cellAdec: MPEG layer I on FFmpeg, frames built here: tones, information, PTS, channels (D-577)", "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // Six stereo frames, tones in subbands 4 and 9 (3375 and 7125 Hz), two without a PTS (the core's own correction:
    // 384 samples at 48 kHz, 720 ticks), the stream's channels kept (channelNumber 0).
    Feed feed;
    feed.bsi_size = 0x40;
    feed.step = 720;
    for (u32 k = 0; k < 6; k++) feed.aus.push_back(layer1_tone(2, {4, 9}));
    feed.missing = {0, 3};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMpegL1, mpmc_param(0), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == 6);
    CHECK(flow_ok(run, 6, {}));
    const auto ref = ffmpeg_decode("mp1float", feed.aus);
    for (u32 k = 0; k < 6; k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.size == 384 * 2 * 4);
        CHECK(o.pcm == expected_pcm(ref[k], false));
        // The first has no PTS and none before it: none; the fourth follows the third by 720.
        CHECK(o.pts_upper == (k == 0 ? kTsInvalid : 0));
        CHECK(o.pts_lower == (k == 0 ? kTsInvalid : 0x1000 + 720 * k));
        CHECK(be32(o.bsi, 0x00) == 3);   // channelNumber: CELL_ADEC_CH_STEREO
        CHECK(be32(o.bsi, 0x04) == 1);   // sampleFreq: 48 kHz
        CHECK(be32(o.bsi, 0x08) == 0);   // no CRC
        CHECK(be32(o.bsi, 0x0C) == 12);  // bit rate index
        CHECK(be32(o.bsi, 0x10) == 0);   // stereo
        CHECK(be32(o.bsi, 0x30) == 384); // outputFrameSize
        CHECK(be32(o.bsi, 0x3C) == 2);   // L_R
    }
    std::vector<u8> pcm;
    for (u32 k = 2; k < 6; k++) pcm.insert(pcm.end(), run.outs[k].pcm.begin(), run.outs[k].pcm.end());
    CHECK(energy_near(channel_of(pcm, 2, 0, false), 48000, 3375, 200) > 0.9);
    CHECK(energy_near(channel_of(pcm, 2, 1, false), 48000, 7125, 200) > 0.9);
    // channelNumber MONO: the two averaged; STEREO from a mono stream: the one in both.
    Feed mono;
    mono.bsi_size = 0x40;
    for (u32 k = 0; k < 3; k++) mono.aus.push_back(layer1_tone(2, {4, 9}));
    const CoreRun down = run_core(f.base(), hle_calls(f, t), kTypeMpegL1, mpmc_param(1), mono);
    REQUIRE(down.outs.size() == 3);
    const auto ref3 = ffmpeg_decode("mp1float", mono.aus);
    for (u32 k = 0; k < 3; k++) {
        CHECK(down.outs[k].size == 384 * 4);
        CHECK(be32(down.outs[k].bsi, 0) == 1);
        std::vector<std::vector<float>> avg(1);
        for (usize i = 0; i < ref3[k][0].size(); i++) avg[0].push_back((ref3[k][0][i] + ref3[k][1][i]) * 0.5f);
        CHECK(down.outs[k].pcm == expected_pcm(avg, false));
    }
    Feed one;
    one.bsi_size = 0x40;
    for (u32 k = 0; k < 3; k++) one.aus.push_back(layer1_tone(1, {6, 0}));
    const CoreRun up = run_core(f.base(), hle_calls(f, t), kTypeMpegL1, mpmc_param(3), one);
    REQUIRE(up.outs.size() == 3);
    const auto ref1 = ffmpeg_decode("mp1float", one.aus);
    for (u32 k = 0; k < 3; k++) {
        CHECK(up.outs[k].size == 384 * 2 * 4);
        CHECK(be32(up.outs[k].bsi, 0x3C) == 0); // MONO
        CHECK(up.outs[k].pcm == expected_pcm({ref1[k][0], ref1[k][0]}, false));
    }
}

TEST_CASE("cellAdec: MPEG layer II on FFmpeg, a stream FFmpeg's encoder wrote: the signal, information, errors (D-577)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("mp2")) SKIP("this FFmpeg has no mp2 encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // Two seconds of 1 kHz (left, 0.5) and 2.5 kHz (right, 0.3) at 48 kHz, 384 kbit/s (layer II's most).
    const std::vector<s16> input = sines(48000, 96000, {1000, 2500}, {0.5, 0.3});
    const Encoded enc = ffmpeg_encode("mp2", 48000, 2, 384000, input);
    REQUIRE(enc.packets.size() > 60);
    Feed feed;
    feed.bsi_size = 0x40;
    feed.aus = enc.packets;
    feed.missing = {5, 6};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMpegL2, mpmc_param(0), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == enc.packets.size());
    CHECK(flow_ok(run, static_cast<u32>(enc.packets.size()), {}));
    const MpegFields h = mpeg_fields(be32(enc.packets[0], 0));
    for (u32 k = 0; k < run.outs.size(); k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.size == 1152 * 2 * 4);
        CHECK(o.pts_lower == 0x1000 + 2160 * k); // the sixth and seventh corrected by 1152 samples at 48 kHz
        CHECK(be32(o.bsi, 0x04) == 1);
        CHECK(be32(o.bsi, 0x08) == h.crc);
        CHECK(be32(o.bsi, 0x0C) == h.bitrate_index);
        CHECK(be32(o.bsi, 0x10) == h.mode);
        CHECK(be32(o.bsi, 0x30) == 1152);
    }
    u32 delay = 0;
    const double left = snr_db(floats_of(input, 2, 0), joined(run, 2, 0), 2000, &delay);
    const double right = snr_db(floats_of(input, 2, 1), joined(run, 2, 1), 2000);
    INFO(std::format("SNR {:.1f} and {:.1f} dB, delay {} samples", left, right, delay));
    CHECK(left > 30);
    CHECK(right > 30);
    // Frames refused: a layer III frame (LAYER), one cut short (OPENBS), free format (UNSUPPORT), MPEG-2.5's sync
    // (SYNCF); the frame after them decodes.
    Feed bad;
    bad.bsi_size = 0x40;
    bad.aus.push_back(mp3_frame({}));
    bad.aus.emplace_back(enc.packets[10].begin(), enc.packets[10].begin() + 300);
    std::vector<u8> free_format = enc.packets[11];
    free_format[2] &= 0x0F;
    bad.aus.push_back(free_format);
    std::vector<u8> lsf25 = enc.packets[12];
    lsf25[1] = static_cast<u8>(lsf25[1] & ~0x18);
    bad.aus.push_back(lsf25);
    bad.aus.push_back(enc.packets[13]);
    const CoreRun refused = run_core(f.base(), hle_calls(f, t), kTypeMpegL2, mpmc_param(0), bad);
    INFO(all(refused));
    REQUIRE(refused.outs.size() == 5);
    CHECK(flow_ok(refused, 5, {{0, 0x80612B12}, {1, 0x80612B36}, {2, 0x80612B17}, {3, 0x80612B11}}));
    CHECK(refused.outs[4].size == 1152 * 2 * 4);
    // StartSeq refuses a layout past the CellAdecChannel values.
    const CoreRun arg = run_core(f.base(), hle_calls(f, t), kTypeMpegL2, mpmc_param(12), Feed{});
    CHECK(arg.lines[2] == "cellAdecStartSeq -> 0x80612b03");
}

#endif

// ---- MPEG-4 AAC

#if PSPOCKET_HAVE_FFMPEG

namespace {

constexpr u32 kTypeM4Aac = 16, kTypeM4Aac2ch = 20, kTypeM4Aac2chMod = 24;

// CellAdecParamM4Aac: configNumber (0 ADIF, 1 ADTS, 2 raw data blocks), the program or the rate index, the profile,
// enableDownmix.
std::vector<u8> m4aac_param(u32 config, u32 a = 0, u32 profile = 0, u32 downmix = 0)
{
    return be32s({config, a, profile, downmix});
}

// An ADTS frame (MPEG-4, no CRC) around a raw AAC frame, with the object type, rate index and channel configuration of
// the AudioSpecificConfig `asc`.
std::vector<u8> adts(const std::vector<u8>& asc, const std::vector<u8>& raw)
{
    const u32 aot = asc[0] >> 3, sfi = (asc[0] & 7u) << 1 | asc[1] >> 7, channels = (asc[1] >> 3) & 15u;
    BitWriter w;
    w.put(0xFFF, 12);
    w.put(0, 1); // MPEG-4
    w.put(0, 2); // layer
    w.put(1, 1); // no CRC
    w.put(aot - 1, 2);
    w.put(sfi, 4);
    w.put(0, 1);
    w.put(channels, 3);
    w.put(0, 4); // original, home, copyright bits
    w.put(static_cast<u32>(raw.size()) + 7, 13);
    w.put(0x7FF, 11);
    w.put(0, 2);
    std::vector<u8> f = w.bytes;
    f.insert(f.end(), raw.begin(), raw.end());
    return f;
}

// An ADIF header for one program of one front element (a pair, or a single channel): "ADIF", no copyright ID, an
// original, a variable rate of `bitrate` bit/s, one program config element (tag `tag`, LC, rate index `sfi`), aligned,
// no comment.
std::vector<u8> adif_header(u32 sfi, bool pair, u32 bitrate, u32 tag = 0)
{
    BitWriter w;
    w.put(0x41444946, 32);
    w.put(0, 1);         // copyright_id_present
    w.put(1, 1);         // original_copy
    w.put(0, 1);         // home
    w.put(1, 1);         // bitstream_type: variable (no buffer fullness)
    w.put(bitrate, 23);
    w.put(0, 4);         // one PCE
    w.put(tag, 4);       // element_instance_tag
    w.put(1, 2);         // LC
    w.put(sfi, 4);
    w.put(1, 4);         // one front element
    w.put(0, 4 + 4 + 2 + 3 + 4);
    w.put(0, 3);         // no mono, stereo or matrix mixdown
    w.put(pair, 1);
    w.put(0, 4);         // its tag
    w.put(0, (8 - w.bit % 8) % 8);
    w.put(0, 8);         // comment
    return w.bytes;
}

// AAC LC written by FFmpeg's encoder: `channels` channels (5.1 with the back pair for 6) of `freqs`.
Encoded aac_encode(const std::vector<s16>& input, u32 channels, u32 rate, s64 bit_rate)
{
    return ffmpeg_encode("aac", rate, channels, bit_rate, input, [&](AVCodecContext* av) {
        if (channels == 6) {
            av_channel_layout_uninit(&av->ch_layout);
            av_channel_layout_from_mask(&av->ch_layout, AV_CH_LAYOUT_5POINT1_BACK);
        }
    });
}

} // namespace

TEST_CASE("cellAdec: MPEG-4 AAC on FFmpeg, ADTS, raw data blocks and ADIF that FFmpeg's encoder wrote (D-578)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("aac")) SKIP("this FFmpeg has no aac encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // Two seconds of 1 kHz (left, 0.5) and 2.5 kHz (right, 0.3) at 48 kHz, LC at 128 kbit/s.
    const std::vector<s16> input = sines(48000, 96000, {1000, 2500}, {0.5, 0.3});
    const Encoded enc = aac_encode(input, 2, 48000, 128000);
    REQUIRE(enc.extradata.size() >= 2);
    REQUIRE(enc.packets.size() > 80);
    // ADTS: every unit a frame; two without a PTS (1024 samples at 48 kHz: 1920 ticks).
    Feed feed;
    feed.bsi_size = 0x80;
    feed.step = 1920;
    for (const auto& p : enc.packets) feed.aus.push_back(adts(enc.extradata, p));
    feed.missing = {3, 4};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(1), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == enc.packets.size());
    CHECK(flow_ok(run, static_cast<u32>(enc.packets.size()), {}));
    for (u32 k = 0; k < run.outs.size(); k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.size == 1024 * 2 * 4);
        CHECK(o.pts_lower == 0x1000 + 1920 * k);
        const std::vector<u32> head = {48000, 2, 2, 0, 0, 0, 0, 0, 1, 1, 1}; // rate, channels by place, no SBR, valid, ADTS
        for (u32 i = 0; i < head.size(); i++) CHECK(be32(o.bsi, 4 * i) == head[i]);
        // The ADTS header: MPEG-4, layer 0, no CRC, LC, 48 kHz, private 0, two channels, ..., its length.
        const std::vector<u32> adts_fields = {0, 0, 1, 1, 3, 0, 2, 0, 0, 0, 0, static_cast<u32>(feed.aus[k].size()), 0x7FF,
            0, 0};
        for (u32 i = 0; i < adts_fields.size(); i++) CHECK(be32(o.bsi, 0x2C + 4 * i) == adts_fields[i]);
    }
    const double left = snr_db(floats_of(input, 2, 0), joined(run, 2, 0), 3000);
    const double right = snr_db(floats_of(input, 2, 1), joined(run, 2, 1), 3000);
    INFO(std::format("SNR {:.1f} and {:.1f} dB", left, right));
    CHECK(left > 25);
    CHECK(right > 25);
    // Raw data blocks with the rate index and profile in the parameters: the same PCM.
    Feed raw;
    raw.bsi_size = 0x80;
    raw.aus = enc.packets;
    const CoreRun raw_run = run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(2, 3, 1), raw);
    REQUIRE(raw_run.outs.size() == run.outs.size());
    CHECK(flow_ok(raw_run, static_cast<u32>(raw.aus.size()), {}));
    for (u32 k = 0; k < raw_run.outs.size(); k++) {
        CHECK(raw_run.outs[k].pcm == run.outs[k].pcm);
        CHECK(be32(raw_run.outs[k].bsi, 0) == 48000);
        CHECK(be32(raw_run.outs[k].bsi, 0x28) == 2);
    }
    // ADIF: the header before the first block, its program 0 a stereo pair; the same PCM, the header's fields kept.
    Feed adif;
    adif.bsi_size = 0x80;
    adif.aus = enc.packets;
    const std::vector<u8> header = adif_header(3, true, 128000);
    adif.aus[0].insert(adif.aus[0].begin(), header.begin(), header.end());
    const CoreRun adif_run = run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(0, 0), adif);
    INFO(all(adif_run));
    REQUIRE(adif_run.outs.size() == run.outs.size());
    CHECK(flow_ok(adif_run, static_cast<u32>(adif.aus.size()), {}));
    for (u32 k = 0; k < adif_run.outs.size(); k++) {
        INFO("ADIF access unit " << k);
        const Out& o = adif_run.outs[k];
        CHECK(o.pcm == run.outs[k].pcm);
        CHECK(be32(o.bsi, 0x28) == 0);         // configNumber: ADIF
        CHECK(be32(o.bsi, 0x2C) == 0);         // no copyright ID
        CHECK(be32(o.bsi, 0x3C) == 1);         // original
        CHECK(be32(o.bsi, 0x44) == 1);         // variable rate
        CHECK(be32(o.bsi, 0x48) == 128000);    // bit rate
        CHECK(be32(o.bsi, 0x4C) == 0);         // one PCE
    }
}

TEST_CASE("cellAdec: MPEG-4 AAC on FFmpeg, 5.1 out, downmixed by the two-channel cores, and refused units (D-578)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("aac")) SKIP("this FFmpeg has no aac encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // 5.1 (FFmpeg's order: front left, right, centre, LFE, back left, right), a tone in each, at 48 kHz.
    const std::vector<double> freqs = {500, 800, 1200, 60, 1700, 2300};
    const std::vector<s16> input = sines(48000, 96000, freqs, {0.3, 0.3, 0.3, 0.3, 0.3, 0.3});
    const Encoded enc = aac_encode(input, 6, 48000, 384000);
    REQUIRE(enc.packets.size() > 80);
    Feed feed;
    feed.bsi_size = 0x80;
    for (const auto& p : enc.packets) feed.aus.push_back(adts(enc.extradata, p));
    const CoreRun full = run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(1), feed);
    INFO(all(full));
    REQUIRE(full.outs.size() == feed.aus.size());
    for (const Out& o : full.outs) {
        CHECK(o.size == 1024 * 6 * 4);
        const std::vector<u32> head = {48000, 6, 3, 1, 0, 2, 1}; // rate; channels, front, front centre, side, back, LFE
        for (u32 i = 0; i < head.size(); i++) CHECK(be32(o.bsi, 4 * i) == head[i]);
    }
    for (u32 c = 0; c < 6; c++) {
        const double snr = snr_db(floats_of(input, 6, c), joined(full, 6, c), 3000);
        INFO(std::format("channel {}: SNR {:.1f} dB", c, snr));
        CHECK(snr > 20);
    }
    // The two-channel cores, and enableDownmix: each side its front, the centre and its back at 0.7071, the LFE out,
    // over the sum of those (2.414).
    const auto expected_stereo = [&](u32 k) {
        std::vector<std::vector<float>> s(2);
        const auto ch = [&](u32 c) { return channel_of(full.outs[k].pcm, 6, c, false); };
        const auto fl = ch(0), fr = ch(1), fc = ch(2), bl = ch(4), br = ch(5);
        const float k7 = 0.70710678f, norm = 1.0f + 2 * k7;
        for (usize i = 0; i < fl.size(); i++) {
            s[0].push_back(fl[i] / norm + fc[i] * (k7 / norm) + bl[i] * (k7 / norm));
            s[1].push_back(fr[i] / norm + fc[i] * (k7 / norm) + br[i] * (k7 / norm));
        }
        return s;
    };
    for (const auto& [type, downmix] : {std::pair{kTypeM4Aac2ch, 0u}, {kTypeM4Aac2chMod, 0u}, {kTypeM4Aac, 1u}}) {
        INFO("type " << type << " downmix " << downmix);
        const CoreRun two = run_core(f.base(), hle_calls(f, t), type, m4aac_param(1, 0, 0, downmix), feed);
        REQUIRE(two.outs.size() == feed.aus.size());
        double worst = 0;
        for (u32 k = 0; k < two.outs.size(); k++) {
            CHECK(two.outs[k].size == 1024 * 2 * 4);
            CHECK(be32(two.outs[k].bsi, 4) == 6); // the stream's channels
            const auto want = expected_stereo(k);
            for (u32 c = 0; c < 2; c++) {
                const auto got = channel_of(two.outs[k].pcm, 2, c, false);
                for (usize i = 0; i < got.size(); i++)
                    worst = std::max(worst, static_cast<double>(std::abs(got[i] - want[c][i])));
            }
        }
        CHECK(worst < 1e-6);
    }
    // Units refused: ADTS without its sync, of layer 1, of rate index 13, of the SSR profile, one longer than its unit;
    // the frame after them decodes.
    std::vector<u8> good = feed.aus[10];
    const auto with = [&](u32 byte, u8 mask, u8 value) {
        std::vector<u8> au = good;
        au[byte] = static_cast<u8>((au[byte] & ~mask) | value);
        return au;
    };
    Feed bad;
    bad.bsi_size = 0x80;
    bad.aus = {with(1, 0xF0, 0x70), with(1, 0x06, 0x02), with(2, 0x3C, 0x34), with(2, 0xC0, 0x80),
        std::vector<u8>(good.begin(), good.begin() + 100), good};
    const CoreRun refused = run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(1), bad);
    INFO(all(refused));
    REQUIRE(refused.outs.size() == 6);
    CHECK(flow_ok(refused, 6, {{0, 0x80612415}, {1, 0x80612414}, {2, 0x80612428}, {3, 0x80612426}, {4, 0x80612407}}));
    CHECK(refused.outs[5].size == 1024 * 6 * 4);
    // ADIF: a header that is not one, and a program the header has not.
    Feed no_adif;
    no_adif.bsi_size = 0x80;
    no_adif.aus = {enc.packets[0]};
    CHECK(flow_ok(run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(0), no_adif), 1, {{0, 0x80612425}}));
    Feed other;
    other.bsi_size = 0x80;
    other.aus = {adif_header(3, true, 1000)};
    CHECK(flow_ok(run_core(f.base(), hle_calls(f, t), kTypeM4Aac, m4aac_param(0, 5), other), 1, {{0, 0x80612427}}));
    // StartSeq's checks: an unknown configuration, a raw rate index past 8 kHz, the SSR profile.
    for (const auto& p : {m4aac_param(3), m4aac_param(2, 12, 1), m4aac_param(2, 3, 2), m4aac_param(1, 0, 0, 2)})
        CHECK(run_core(f.base(), hle_calls(f, t), kTypeM4Aac, p, Feed{}).lines[2] == "cellAdecStartSeq -> 0x80612403");
}

#endif

// ---- AC-3

#if PSPOCKET_HAVE_FFMPEG

namespace {

constexpr u32 kTypeAc3 = 2;

// CellAdecParamAc3 (RPCS3's layout: byte-wide enums). The pointers give each of Dolby's channels (L, C, R, Ls, Rs,
// LFE) its output slot: by default WAV's order (L, R, C, LFE, Ls, Rs), as FFmpeg's.
struct Ac3Param {
    u8 word = 1, mode = 7, lfe = 1, compression = 2, channels = 6, stereo = 0, dualmono = 0, karaoke = 3;
    float cut = 1, boost = 1, scale = 1;
    std::array<s32, 6> pointers = {0, 2, 1, 4, 5, 3};
};

std::vector<u8> ac3_param(const Ac3Param& a)
{
    std::vector<u8> v(0x30, 0);
    v[0] = a.word;
    v[1] = a.mode;
    v[2] = a.lfe;
    const auto f32 = [&](usize at, float x) {
        u32 bits;
        std::memcpy(&bits, &x, 4);
        for (u32 k = 0; k < 4; k++) v[at + k] = static_cast<u8>(bits >> (24 - 8 * k));
    };
    f32(4, a.cut);
    f32(8, a.boost);
    v[0xC] = a.compression;
    v[0xD] = a.channels;
    v[0xE] = a.stereo;
    v[0xF] = a.dualmono;
    v[0x10] = a.karaoke;
    f32(0x14, a.scale);
    for (u32 k = 0; k < 6; k++)
        for (u32 b = 0; b < 4; b++) v[0x18 + 4 * k + b] = static_cast<u8>(static_cast<u32>(a.pointers[k]) >> (24 - 8 * b));
    return v;
}

} // namespace

TEST_CASE("cellAdec: AC-3's core reads a unit too short for its header's raw bytes only as far as it goes (D-605)",
    "[hle][adec]")
{
    // A stereo syncframe's header with no options ends in 8 bytes, but the core copied 8 raw bytes from byte 5 into the
    // information block: 13 bytes of an 8-byte unit, past the game's buffer. The unit ends here where guest memory
    // does (the next page reserved and never committed), as the parser fuzzing found it (D-603).
    Fixture f;
    u8* b = f.base();
    constexpr u32 kEnd = 0x00600000, kOut = kData + 0x100000, kBsi = kData + 0x40000;
    REQUIRE(f.hle->main_memory().reserve(kEnd - 0x10000, 0x20000));
    REQUIRE(f.mem.commit(kEnd - 0x10000, 0x10000));
    const hle::adec::ffcore::Spec* spec = hle::adec::ffcore::spec_of(kTypeAc3);
    REQUIRE(spec);
    REQUIRE(spec->output_bytes <= 0x100000);
    std::vector<u8> param = ac3_param({.word = 1, .mode = 2, .lfe = 0, .compression = 2, .channels = 2, .stereo = 0,
        .dualmono = 0, .karaoke = 3, .cut = 1, .boost = 1, .scale = 1, .pointers = {0, -1, 1, -1, -1, -1}});
    param.resize(std::max<usize>(param.size(), spec->param_size), 0);
    REQUIRE(spec->check(param.data()) == lv2::CELL_OK);
    const std::unique_ptr<hle::adec::ffcore::Codec> codec = spec->make();
    if (codec->start(param.data()) != lv2::CELL_OK) SKIP("this FFmpeg has no AC-3 decoder");
    // 0x0B77, CRC, 48 kHz at 64 kbit/s (a 256-byte frame), bsid 8, 2/0, no LFE, dialnorm 27, original.
    const u8 unit[8] = {0x0B, 0x77, 0x00, 0x00, 0x08, 0x40, 0x43, 0x61};
    std::memcpy(b + kEnd - 8, unit, 8);
    std::memset(b + kBsi, 0xEE, spec->bsi_size);
    const hle::adec::ffcore::Decoded d = codec->decode(b, kEnd - 8, 8, kOut, kBsi);
    CHECK(d.error != 0); // the frame is not all there
    CHECK(d.size == 0);
    CHECK(ps_read32(b, kBsi + 0x28) == 0x40436100u); // the raw bytes from 5 on: the three there are, then zeros
    CHECK(ps_read32(b, kBsi + 0x2C) == 0u);
    CHECK(ps_read8(b, kBsi + 0x3B) == 8); // bsid
}

TEST_CASE("cellAdec: AC-3 on FFmpeg, 5.1 that FFmpeg's encoder wrote: every channel routed, the information (D-579)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("ac3")) SKIP("this FFmpeg has no ac3 encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // 5.1 at 48 kHz, 448 kbit/s (FFmpeg's order: front left, right, centre, LFE, side left, right), a tone in each.
    const std::vector<double> freqs = {500, 800, 1200, 60, 1700, 2300};
    const std::vector<s16> input = sines(48000, 96000, freqs, {0.3, 0.3, 0.3, 0.3, 0.3, 0.3});
    const Encoded enc = ffmpeg_encode("ac3", 48000, 6, 448000, input);
    REQUIRE(enc.packets.size() > 50);
    Feed feed;
    feed.bsi_size = 0x44;
    feed.step = 2880;
    feed.aus = enc.packets;
    feed.missing = {2, 3};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeAc3, ac3_param({}), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == feed.aus.size());
    CHECK(flow_ok(run, static_cast<u32>(feed.aus.size()), {}));
    const auto ref = ffmpeg_decode("ac3", feed.aus);
    for (u32 k = 0; k < run.outs.size(); k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.size == 1536 * 6 * 4);
        CHECK(o.pcm == expected_pcm(ref[k], false)); // 3/2 with the LFE: no downmix, FFmpeg's samples in WAV's order
        CHECK(o.pts_lower == 0x1000 + 2880 * k);
        CHECK(be32(o.bsi, 0x04) == 0x01000000);
        CHECK(be32(o.bsi, 0x0C) == k + 1);          // totalCallNum
        CHECK(be32(o.bsi, 0x10) == 448);            // kbit/s
        CHECK(be32(o.bsi, 0x14) == o.size);         // pcmSize
        CHECK(be32(o.bsi, 0x18) == 8 * 1792);       // esSizeBit
        CHECK(be32(o.bsi, 0x1C) == 0);
        CHECK(o.bsi[0x21] == 1);                    // validity
        CHECK(o.bsi[0x22] == 6);                    // channels with the LFE
        CHECK(o.bsi[0x23] == 0);                    // 48 kHz
        CHECK(o.bsi[0x24] == 1);                    // float
        CHECK(o.bsi[0x25] == 6);
        CHECK(o.bsi[0x32] == 7);                    // acmod 3/2
        CHECK(o.bsi[0x33] == 1);                    // LFE
        CHECK(o.bsi[0x3B] == feed.aus[k][5] >> 3);  // bsid
        CHECK(o.bsi[0x42] == 0);                    // the CRC matched
        CHECK(o.bsi[0x43] == 0);                    // no downmix
    }
    for (u32 c = 0; c < 6; c++) {
        const double snr = snr_db(floats_of(input, 6, c), joined(run, 6, c), 3000);
        INFO(std::format("channel {}: SNR {:.1f} dB", c, snr));
        CHECK(snr > 20);
    }
    // The pointers turned round (L to the last slot ... LFE to the first) and the PCM scaled by 0.5 in 16 bits.
    const CoreRun turned = run_core(f.base(), hle_calls(f, t), kTypeAc3,
        ac3_param({.word = 0, .scale = 0.5f, .pointers = {5, 3, 4, 1, 0, 2}}), feed);
    REQUIRE(turned.outs.size() == run.outs.size());
    // Slot s of the turned output against WAV's channel w of the first: L 5 <- 0, C 3 <- 2, R 4 <- 1, Ls 1 <- 4,
    // Rs 0 <- 5, LFE 2 <- 3.
    const std::array<u32, 6> from = {5, 4, 3, 2, 1, 0};
    double worst = 0;
    for (u32 k = 0; k < run.outs.size(); k++) {
        CHECK(turned.outs[k].size == 1536 * 6 * 2);
        for (u32 s = 0; s < 6; s++) {
            const auto got = channel_of(turned.outs[k].pcm, 6, s, true), want = channel_of(run.outs[k].pcm, 6, from[s], false);
            for (usize i = 0; i < got.size(); i++)
                worst = std::max(worst, static_cast<double>(std::abs(got[i] - 0.5f * want[i])));
        }
    }
    CHECK(worst < 1.0 / 32768 + 1e-9); // the 16-bit floor
}

TEST_CASE("cellAdec: AC-3 on FFmpeg, downmixed: Lo/Ro, Lt/Rt, mono, 3/0, 2/1; stereo out wide; refusals (D-579)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("ac3")) SKIP("this FFmpeg has no ac3 encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const std::vector<s16> input = sines(48000, 48000, {500, 800, 1200, 60, 1700, 2300}, {0.3, 0.3, 0.3, 0.3, 0.3, 0.3});
    const Encoded enc = ffmpeg_encode("ac3", 48000, 6, 448000, input);
    REQUIRE(enc.packets.size() > 20);
    Feed feed;
    feed.bsi_size = 0x44;
    feed.aus = enc.packets;
    const CoreRun full = run_core(f.base(), hle_calls(f, t), kTypeAc3, ac3_param({}), feed);
    REQUIRE(full.outs.size() == feed.aus.size());
    const u8 cmix = full.outs[0].bsi[0x35], smix = full.outs[0].bsi[0x36];
    const float clev = std::array{0.70710678f, 0.59460356f, 0.5f, 0.59460356f}[cmix];
    const float slev = std::array{0.70710678f, 0.5f, 0.0f, 0.5f}[smix];
    const float k3 = 0.70710678f;
    // A downmix of the full output (WAV's order: L, R, C, LFE, Ls, Rs), each row over its sum when that passes 1.
    using Row = std::array<float, 6>;
    const auto check_downmix = [&](const Ac3Param& p, const std::vector<Row>& rows, u8 dmix_type) {
        const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeAc3, ac3_param(p), feed);
        REQUIRE(run.outs.size() == full.outs.size());
        double worst = 0;
        for (u32 k = 0; k < run.outs.size(); k++) {
            CHECK(run.outs[k].size == 1536 * rows.size() * 4);
            CHECK(run.outs[k].bsi[0x43] == dmix_type);
            for (u32 o = 0; o < rows.size(); o++) {
                float sum = 0;
                for (const float c : rows[o]) sum += std::abs(c);
                const float norm = std::max(sum, 1.0f);
                const auto got = channel_of(run.outs[k].pcm, static_cast<u32>(rows.size()), o, false);
                for (usize i = 0; i < got.size(); i++) {
                    double want = 0;
                    for (u32 w = 0; w < 6; w++) {
                        if (rows[o][w] == 0) continue;
                        want += static_cast<double>(rows[o][w] / norm) *
                            static_cast<double>(channel_of(full.outs[k].pcm, 6, w, false)[i]);
                    }
                    worst = std::max(worst, std::abs(static_cast<double>(got[i]) - want));
                }
            }
        }
        CHECK(worst < 1e-5);
    };
    const std::array<s32, 6> lr = {0, -1, 1, -1, -1, -1};
    // 2/0 Lo/Ro: L + clev C + slev Ls; Lt/Rt: L + 0.7071 C - 0.7071 (Ls + Rs), R + 0.7071 C + 0.7071 (Ls + Rs).
    check_downmix({.mode = 2, .lfe = 0, .channels = 2, .stereo = 2, .pointers = lr},
        {Row{1, 0, clev, 0, slev, 0}, Row{0, 1, clev, 0, 0, slev}}, 2);
    check_downmix({.mode = 2, .lfe = 0, .channels = 2, .stereo = 1, .pointers = lr},
        {Row{1, 0, k3, 0, -k3, -k3}, Row{0, 1, k3, 0, k3, k3}}, 1);
    // Auto: the stream names no preferred mode, so Lo/Ro.
    check_downmix({.mode = 2, .lfe = 0, .channels = 2, .stereo = 0, .pointers = lr},
        {Row{1, 0, clev, 0, slev, 0}, Row{0, 1, clev, 0, 0, slev}}, 2);
    // 1/0: the Lo/Ro pair's sum at 0.7071, to the one slot the centre points at.
    check_downmix({.mode = 1, .lfe = 0, .channels = 1, .pointers = {-1, 0, -1, -1, -1, -1}},
        {Row{k3, k3, 2 * k3 * clev, 0, k3 * slev, k3 * slev}}, 2);
    // 3/0 with the LFE: the surrounds into the fronts; 2/1: the centre into the fronts, the surrounds to one.
    check_downmix({.mode = 3, .lfe = 1, .channels = 4, .pointers = {0, 2, 1, -1, -1, 3}},
        {Row{1, 0, 0, 0, slev, 0}, Row{0, 1, 0, 0, 0, slev}, Row{0, 0, 1, 0, 0, 0}, Row{0, 0, 0, 1, 0, 0}}, 2);
    check_downmix({.mode = 4, .lfe = 0, .channels = 3, .pointers = {0, -1, 1, 2, -1, -1}},
        {Row{1, 0, clev, 0, 0, 0}, Row{0, 1, clev, 0, 0, 0}, Row{0, 0, 0, 0, k3, k3}}, 2);
    // A stereo stream into 3/2 with the LFE: L and R, the other slots silent.
    const std::vector<s16> two = sines(48000, 24000, {700, 1300}, {0.4, 0.4});
    const Encoded stereo = ffmpeg_encode("ac3", 48000, 2, 192000, two);
    Feed st;
    st.bsi_size = 0x44;
    st.aus = stereo.packets;
    const CoreRun wide = run_core(f.base(), hle_calls(f, t), kTypeAc3, ac3_param({}), st);
    const auto ref = ffmpeg_decode("ac3", st.aus);
    REQUIRE(wide.outs.size() == st.aus.size());
    for (u32 k = 0; k < wide.outs.size(); k++) {
        CHECK(wide.outs[k].size == 1536 * 6 * 4);
        CHECK(wide.outs[k].bsi[0x32] == 2); // acmod 2/0
        std::vector<std::vector<float>> want(6, std::vector<float>(1536, 0.0f));
        want[0] = ref[k][0];
        want[1] = ref[k][1];
        CHECK(wide.outs[k].pcm == expected_pcm(want, false));
    }
    // After a good frame, refused ones: no sync, the reserved rate, E-AC-3's bsid, one cut short; then a CRC mismatch,
    // flagged and concealed (FFmpeg repeats the last block), and a good frame.
    const std::vector<u8> good = enc.packets[5];
    const auto with = [&](u32 byte, u8 mask, u8 value) {
        std::vector<u8> au = good;
        au[byte] = static_cast<u8>((au[byte] & ~mask) | value);
        return au;
    };
    Feed bad;
    bad.bsi_size = 0x44;
    std::vector<u8> crc = good;
    crc[100] ^= 0x10;
    bad.aus = {enc.packets[4], with(0, 0xFF, 0x0A), with(4, 0xC0, 0xC0), with(5, 0xF8, 0x80),
        std::vector<u8>(good.begin(), good.begin() + 900), crc, good};
    const CoreRun refused = run_core(f.base(), hle_calls(f, t), kTypeAc3, ac3_param({}), bad);
    INFO(all(refused));
    REQUIRE(refused.outs.size() == 7);
    CHECK(flow_ok(refused, 7, {{1, 0x80612503}, {2, 0x80612503}, {3, 0x80612503}, {4, 0x80612503}}));
    CHECK(refused.outs[5].size == 1536 * 6 * 4);
    CHECK(refused.outs[5].bsi[0x42] == 1); // crcErrorFlag
    CHECK(refused.outs[6].bsi[0x42] == 0);
    CHECK(refused.outs[6].size == 1536 * 6 * 4);
    // StartSeq's checks: a word size, an output mode, a channel count, a pointer past the slots, two at one slot, a
    // scale factor past 1.
    for (const Ac3Param& p : {Ac3Param{.word = 2}, Ac3Param{.mode = 0}, Ac3Param{.mode = 8}, Ac3Param{.channels = 0},
             Ac3Param{.channels = 7}, Ac3Param{.channels = 5}, Ac3Param{.pointers = {0, 0, 1, 2, 3, 4}},
             Ac3Param{.scale = 1.5f}})
        CHECK(run_core(f.base(), hle_calls(f, t), kTypeAc3, ac3_param(p), Feed{}).lines[2] ==
            "cellAdecStartSeq -> 0x80612502");
}

#endif

// ---- ATRAC3

#if PSPOCKET_HAVE_FFMPEG

namespace {

constexpr u32 kTypeAtrac3 = 5;

// The spectrum's subbands (FFmpeg's subband_tab, from the ATRAC3 format).
constexpr u32 kSubbands[33] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 288, 320,
    352, 384, 416, 448, 480, 512, 576, 640, 704, 768, 896, 1024};

// An ATRAC3 frame of single channels (no joint stereo), `unit` bytes a channel: each channel's sound unit codes its
// lowest QMF band only, no gain control and no tonal components, its spectrum constant-length coded with one line,
// lines[ch], at 20 in selector 7 (6-bit values), scale factor index `sf`. Its sound is a tone at (line + 0.5) * rate /
// 2048 Hz.
std::vector<u8> atrac3_frame(u32 channels, std::array<u32, 2> lines, u32 unit = 192, u32 sf = 48)
{
    std::vector<u8> f;
    for (u32 ch = 0; ch < channels; ch++) {
        BitWriter w;
        w.put(0x28, 6); // the sound unit's ID
        w.put(0, 2);    // one QMF band
        w.put(0, 3);    // no gain control points
        w.put(0, 5);    // no tonal components
        const u32 line = lines[ch];
        u32 sb = 0;
        while (kSubbands[sb + 1] <= line) sb++;
        w.put(sb, 5); // subbands coded: 0 to sb
        w.put(1, 1);  // constant length coding
        for (u32 i = 0; i <= sb; i++) w.put(i == sb ? 7 : 0, 3);
        w.put(sf, 6);
        for (u32 j = kSubbands[sb]; j < kSubbands[sb + 1]; j++) w.put(j == line ? 20 : 0, 6);
        std::vector<u8> u = w.bytes;
        u.resize(unit, 0);
        f.insert(f.end(), u.begin(), u.end());
    }
    return f;
}

std::vector<u8> atrac3_param(u32 nch, u32 joint, u32 nbytes, u32 bw) { return be32s({nch, joint, nbytes, bw}); }

// FFmpeg's ATRAC3 decoder set up as the core sets it up: a WAV file's 14 extra bytes, the frame size.
std::function<void(AVCodecContext*)> atrac3_setup(u32 nch, u32 nbytes)
{
    return [=](AVCodecContext* av) {
        av_channel_layout_uninit(&av->ch_layout);
        av_channel_layout_default(&av->ch_layout, static_cast<int>(nch));
        av->sample_rate = 44100;
        av->block_align = static_cast<int>(nbytes);
        static constexpr u8 kExtra[14] = {1, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
        av->extradata = static_cast<u8*>(av_mallocz(sizeof(kExtra) + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(av->extradata, kExtra, sizeof(kExtra));
        av->extradata_size = sizeof(kExtra);
    };
}

// The core's 24- and 32-bit words for planar floats (as libatxdec converts: floored, held in range; 24 bits in a
// word's low three bytes).
std::vector<u8> expected_words(const std::vector<std::vector<float>>& unit, u32 bits)
{
    std::vector<u8> v;
    for (usize i = 0; i < unit[0].size(); i++) {
        for (const auto& ch : unit) {
            const float s = ch[i];
            u32 w;
            if (bits == 24)
                w = s >= 1.f ? 0x007FFFFFu : s <= -1.f ? 0x00800000u :
                    static_cast<u32>(static_cast<s32>(std::floor(s * 8388608.f))) & 0x00FFFFFFu;
            else
                w = s >= 1.f ? 0x7FFFFFFFu : s <= -1.f ? 0x80000000u :
                    static_cast<u32>(static_cast<s32>(std::floor(static_cast<double>(s) * 2147483648.0)));
            for (u32 k = 0; k < 4; k++) v.push_back(static_cast<u8>(w >> (24 - 8 * k)));
        }
    }
    return v;
}

} // namespace

TEST_CASE("cellAdec: ATRAC3 on FFmpeg, frames built here: the four PCM formats, information, PTS, refusals (D-579)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // Six stereo frames of 384 bytes (132 kbit/s at 44.1 kHz), lines 100 and 60 (2164 and 1303 Hz), the third
    // without a PTS (the front end's ATRAC3 step, 4180).
    Feed feed;
    feed.bsi_size = 0xC;
    feed.step = 4180;
    for (u32 k = 0; k < 6; k++) feed.aus.push_back(atrac3_frame(2, {100, 60}));
    feed.missing = {2};
    const auto ref = ffmpeg_decode("atrac3", feed.aus, atrac3_setup(2, 384));
    for (const u32 bw : {0x84u, 2u, 3u, 4u}) {
        INFO("bw_pcm " << bw);
        const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeAtrac3, atrac3_param(2, 0, 384, bw), feed);
        INFO(all(run));
        REQUIRE(run.outs.size() == 6);
        CHECK(flow_ok(run, 6, {}));
        for (u32 k = 0; k < 6; k++) {
            const Out& o = run.outs[k];
            CHECK(o.size == 1024 * 2 * (bw == 2 ? 2 : 4));
            CHECK(o.pts_lower == 0x1000 + 4180 * k);
            CHECK(be32(o.bsi, 0) == 2);   // nch
            CHECK(be32(o.bsi, 4) == 0);   // isJoint
            CHECK(be32(o.bsi, 8) == 384); // nbytes
            if (bw == 0x84 || bw == 2) CHECK(o.pcm == expected_pcm(ref[k], bw == 2));
            else CHECK(o.pcm == expected_words(ref[k], bw == 3 ? 24 : 32));
        }
        if (bw == 0x84) {
            std::vector<u8> pcm;
            for (u32 k = 2; k < 6; k++) pcm.insert(pcm.end(), run.outs[k].pcm.begin(), run.outs[k].pcm.end());
            const auto left = channel_of(pcm, 2, 0, false), right = channel_of(pcm, 2, 1, false);
            CHECK(energy_near(left, 44100, 100.5 * 44100 / 2048, 150) > 0.8);
            CHECK(energy_near(right, 44100, 60.5 * 44100 / 2048, 150) > 0.8);
            float peak = 0;
            for (const float x : left) peak = std::max(peak, std::abs(x));
            CHECK(peak > 0.001f);
        }
    }
    // Mono, 152-byte frames (105 kbit/s's size for one channel).
    Feed mono;
    mono.bsi_size = 0xC;
    for (u32 k = 0; k < 3; k++) mono.aus.push_back(atrac3_frame(1, {80, 0}, 152));
    const CoreRun one = run_core(f.base(), hle_calls(f, t), kTypeAtrac3, atrac3_param(1, 0, 152, 0x84), mono);
    REQUIRE(one.outs.size() == 3);
    const auto ref1 = ffmpeg_decode("atrac3", mono.aus, atrac3_setup(1, 152));
    for (u32 k = 0; k < 3; k++) {
        CHECK(one.outs[k].size == 1024 * 4);
        CHECK(one.outs[k].pcm == expected_pcm(ref1[k], false));
    }
    // Refused units: a sound unit whose ID is wrong (FFmpeg's error), one cut short; the frame after them decodes.
    Feed bad;
    bad.bsi_size = 0xC;
    std::vector<u8> wrong_id = atrac3_frame(2, {100, 60});
    wrong_id[0] = 0;
    bad.aus = {atrac3_frame(2, {100, 60}), wrong_id, std::vector<u8>(300, 0xA0), atrac3_frame(2, {100, 60})};
    const CoreRun refused = run_core(f.base(), hle_calls(f, t), kTypeAtrac3, atrac3_param(2, 0, 384, 0x84), bad);
    INFO(all(refused));
    REQUIRE(refused.outs.size() == 4);
    CHECK(flow_ok(refused, 4, {{1, 0x80612180}, {2, 0x80612180}}));
    CHECK(refused.outs[3].size == 1024 * 2 * 4);
    // StartSeq's checks: three channels, joint stereo of one, a frame size FFmpeg has not, a word size; joint stereo of
    // two is taken.
    for (const auto& p : {atrac3_param(3, 0, 576, 0x84), atrac3_param(1, 1, 192, 0x84), atrac3_param(2, 0, 400, 0x84),
             atrac3_param(2, 0, 384, 5)})
        CHECK(run_core(f.base(), hle_calls(f, t), kTypeAtrac3, p, Feed{}).lines[2] == "cellAdecStartSeq -> 0x80610003");
    CHECK(run_core(f.base(), hle_calls(f, t), kTypeAtrac3, atrac3_param(2, 1, 192, 0x84), Feed{}).lines[2] ==
        "cellAdecStartSeq -> 0x0");
}

TEST_CASE("cellAtrac plays an ATRAC3 file through pspocket's ATRAC3 core where no firmware core is (D-579)",
    "[hle][adec][atrac]")
{
    InterpretMisses interpret;
    BigFixture f;
    REQUIRE(f.hle->link_internal_functions({"cellAtrac"}));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    // A RIFF WAVE ATRAC3 file (fmt 0x270 with its 14 extra bytes), 44.1 kHz stereo, 384-byte frames of the tones.
    constexpr u32 kAtHandle = kData + 0x8000, kAtOut = kData + 0x8400, kAtFile = kData + 0x60000, kAtWork = kData + 0xB0000,
                  kAtPcm = kData + 0xE0000;
    std::vector<u8> file;
    const auto u16le = [&](u32 v) {
        file.push_back(static_cast<u8>(v));
        file.push_back(static_cast<u8>(v >> 8));
    };
    const auto u32le = [&](u32 v) {
        u16le(v & 0xFFFF);
        u16le(v >> 16);
    };
    const auto tag = [&](const char* s) { file.insert(file.end(), s, s + 4); };
    constexpr u32 kFrames = 16;
    tag("RIFF");
    u32le(0);
    tag("WAVE");
    tag("fmt ");
    u32le(0x20);
    u16le(0x270);
    u16le(2);
    u32le(44100);
    u32le(44100 * 384 / 1024);
    u16le(384);
    u16le(0);
    u16le(14);
    u16le(1);
    u32le(0x1000);
    u16le(0);
    u16le(0);
    u16le(1);
    u16le(0);
    tag("data");
    u32le(kFrames * 384);
    for (u32 k = 0; k < kFrames; k++) {
        const auto fr = atrac3_frame(2, {100, 60});
        file.insert(file.end(), fr.begin(), fr.end());
    }
    const u32 riff = static_cast<u32>(file.size() - 8);
    std::memcpy(file.data() + 4, &riff, 4);
    std::memcpy(b + kAtFile, file.data(), file.size());
    const auto call = [&](const char* name, std::initializer_list<u64> args) {
        const hle::Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) t.ctx.gpr[r++] = a;
        fn.fn(t.ctx, b);
        return static_cast<u32>(t.ctx.gpr[3]);
    };
    std::memset(b + kAtHandle, 0, 0x200);
    const u32 size = static_cast<u32>(file.size());
    REQUIRE(call("cellAtracSetDataAndGetMemSize", {kAtHandle, kAtFile, size, size, kAtOut}) == 0);
    REQUIRE(ps_read32(b, kAtOut) < 0x30000); // the work memory below the PCM
    REQUIRE(call("cellAtracCreateDecoder", {kAtHandle, kAtWork, 1000, 100}) == 0);
    std::vector<u8> pcm;
    u32 decoded = 0;
    for (u32 n = 0; n < kFrames + 4; n++) {
        std::memset(b + kAtPcm, 0xCC, 0x4000);
        const u32 r = call("cellAtracDecode", {kAtHandle, kAtPcm, kAtOut, kAtOut + 4, kAtOut + 8});
        if (r != 0) break;
        const u32 samples = ps_read32(b, kAtOut);
        decoded += samples;
        pcm.insert(pcm.end(), b + kAtPcm, b + kAtPcm + samples * 2 * 4);
        if (ps_read32(b, kAtOut + 4)) break; // finished
    }
    INFO("samples decoded: " << decoded);
    CHECK(decoded >= (kFrames - 2) * 1024);
    CHECK(energy_near(channel_of(pcm, 2, 0, false), 44100, 100.5 * 44100 / 2048, 150) > 0.8);
    CHECK(energy_near(channel_of(pcm, 2, 1, false), 44100, 60.5 * 44100 / 2048, 150) > 0.8);
    CHECK(call("cellAtracDeleteDecoder", {kAtHandle}) == 0);
}

// ---- D-591: the cores on FFmpeg through pspocket's own exported operation tables, and their PTS functions

namespace {

// Whether two runs of a decoder showed the same: answers, sizes, version, callbacks and every PCM out.
bool same_runs(const CoreRun& a, const CoreRun& b)
{
    if (a.lines != b.lines || a.mem != b.mem || a.version_upper != b.version_upper ||
        a.version_lower != b.version_lower || a.callbacks != b.callbacks || a.outs.size() != b.outs.size())
        return false;
    for (usize i = 0; i < a.outs.size(); i++) {
        const Out& x = a.outs[i];
        const Out& y = b.outs[i];
        if (x.handle != y.handle || x.status != y.status || x.size != y.size || x.pts_upper != y.pts_upper ||
            x.pts_lower != y.pts_lower || x.user_data != y.user_data || x.bsi != y.bsi || x.pcm != y.pcm)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("cellAdec: each core on FFmpeg through pspocket's own operation table, as a firmware module drives it "
          "(D-591)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    // A table, the stream through it (some units without a PTS: the front end's step for MP3, AC-3 and ATRAC3, the
    // core's PTS function for the others) and its tag (the operations' names end with it).
    struct Case {
        const char* library;
        u32 type, nid;
        const char* pts_function; // _SceAdecCorrectPtsValue_*, or none
        u32 pts_nid;
        std::vector<u8> param;
        Feed feed;
        const char* tag;
    };
    std::vector<Case> cases;
    {
        Feed feed;
        feed.bsi_size = 0x1C;
        for (u32 k = 0; k < 4; k++) feed.aus.push_back(mp3_frame({}));
        feed.missing = {2};
        cases.push_back({"cellMP3dec", kTypeMp3, 0x84276A23, nullptr, 0, mp3_param(4), feed, "Mp3"});
    }
    {
        Feed feed;
        feed.bsi_size = 0x40;
        feed.step = 720;
        for (u32 k = 0; k < 5; k++) feed.aus.push_back(layer1_tone(2, {4, 9}));
        feed.missing = {0, 3};
        cases.push_back({"cellMPL1dec", kTypeMpegL1, 0xBBC70551, "_SceAdecCorrectPtsValue_mpmcl1", 0xA4A9A309,
            mpmc_param(0), feed, "mpmcl1"});
    }
    {
        Feed feed;
        feed.bsi_size = 0xC;
        feed.step = 4180;
        for (u32 k = 0; k < 4; k++) feed.aus.push_back(atrac3_frame(2, {100, 60}));
        feed.missing = {2};
        cases.push_back(
            {"cellAtrac3dec", kTypeAtrac3, 0x60487D65, nullptr, 0, atrac3_param(2, 0, 384, 0x84), feed, "atrac3"});
    }
    const std::vector<s16> input = sines(48000, 24000, {1000, 2500}, {0.5, 0.3});
    if (avcodec_find_encoder_by_name("mp2")) {
        const Encoded enc = ffmpeg_encode("mp2", 48000, 2, 384000, input);
        REQUIRE(enc.packets.size() >= 6);
        Feed feed;
        feed.bsi_size = 0x40;
        feed.aus.assign(enc.packets.begin(), enc.packets.begin() + 6);
        feed.missing = {2};
        cases.push_back({"cellM2BCdec", kTypeMpegL2, 0x300AFE4D, "_SceAdecCorrectPtsValue_mpmc", 0xEC7DE355,
            mpmc_param(0), feed, "mpmc"});
    }
    if (avcodec_find_encoder_by_name("aac")) {
        const Encoded enc = aac_encode(input, 2, 48000, 128000);
        REQUIRE(enc.packets.size() >= 6);
        Feed feed;
        feed.bsi_size = 0x80;
        feed.step = 1920;
        for (u32 k = 0; k < 6; k++) feed.aus.push_back(adts(enc.extradata, enc.packets[k]));
        feed.missing = {3};
        cases.push_back({"cellM4AacDec", kTypeM4Aac, 0xAB61278D, "_SceAdecCorrectPtsValue_M4Aac", 0x431A1DCA,
            m4aac_param(1), feed, "M4Aac"});
        cases.push_back({"cellM4AacDec2ch", kTypeM4Aac2ch, 0xE996C664, "_SceAdecCorrectPtsValue_M4Aac2ch", 0x869E328D,
            m4aac_param(1), feed, "M4Aac2ch"});
        cases.push_back({"cellM4AacDec2chmod", kTypeM4Aac2chMod, 0xDBD26836, "_SceAdecCorrectPtsValue_M4Aac2chmod",
            0x156FE2F0, m4aac_param(1), feed, "M4Aac2chmod"});
    }
    if (avcodec_find_encoder_by_name("ac3")) {
        const Encoded enc = ffmpeg_encode("ac3", 48000, 2, 192000, input);
        REQUIRE(enc.packets.size() >= 5);
        Feed feed;
        feed.bsi_size = 0x44;
        feed.step = 2880;
        feed.aus.assign(enc.packets.begin(), enc.packets.begin() + 5);
        feed.missing = {2};
        cases.push_back({"cell_libac3dec", kTypeAc3, 0xC58BB170, nullptr, 0, ac3_param({}), feed, "ac3"});
    }
    CHECK(cases.size() >= 3);
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.library)
        {
            CoreRun direct, tabled;
            {
                BigFixture f;
                GuestThread t(f, 0, "ppu");
                direct = run_core(f.base(), hle_calls(f, t), c.type, c.param, c.feed);
                CHECK(ps_read32(f.base(), kDecoderMem + 0x34) == 0); // pspocket's own core, called directly
                CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_pcm_out") == 0);
            }
            {
                BigFixture f;
                GuestThread t(f, 0, "ppu");
                const u32 table = link_own_table(f, c.library, c.nid);
                if (c.pts_function) link_own_function(f, c.library, c.pts_nid);
                tabled = run_core(f.base(), hle_calls(f, t), c.type, c.param, c.feed);
                CHECK(ps_read32(f.base(), kDecoderMem + 0x34) == table);
                for (const char* op : {"GetMemSize", "Open", "Close", "StartSeq", "EndSeq", "DecodeAu", "GetVersion",
                         "Realign", "ReleasePcm", "GetPcmHandleNum", "GetBsiInfoSize"}) {
                    INFO(op);
                    CHECK(guest_calls(f, "cellAdec", std::format("_CellAdecCoreOp{}_{}", op, c.tag)) > 0);
                }
                CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_pcm_out") == c.feed.aus.size());
                CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_seq_done") == 1);
                if (c.pts_function) CHECK(guest_calls(f, c.library, c.pts_function) == c.feed.aus.size());
            }
            INFO(all(direct));
            INFO(all(tabled));
            CHECK(direct.outs.size() == c.feed.aus.size());
            CHECK(flow_ok(direct, static_cast<u32>(c.feed.aus.size()), {}));
            CHECK(same_runs(direct, tabled));
        }
    }
}

// D-591: _SceAdecCorrectPtsValue_* on a core context as its open leaves it: a missing PTS follows the core's last one
// by that frame's length (the front end's rule for its own cores, D-577), a given one is kept; half a timestamp, a core
// of another codec or a timestamp outside guest memory is FATAL.
TEST_CASE("cellAdec: the cores' own PTS functions (D-591)", "[hle][adec]")
{
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kCore = kDecoderMem, kPts = kArgs + 0x100;
    // FfContext (adec_ffmpeg.hpp): the type at +0x154, the last PTS out at +0x160, its frame's length at +0x168, the
    // length of the frame being given out at +0x16C.
    const struct {
        const char* library;
        const char* name;
        u32 nid, type;
    } fns[] = {{"cellMPL1dec", "_SceAdecCorrectPtsValue_mpmcl1", 0xA4A9A309, kTypeMpegL1},
        {"cellM2BCdec", "_SceAdecCorrectPtsValue_mpmc", 0xEC7DE355, kTypeMpegL2},
        {"cellM4AacDec", "_SceAdecCorrectPtsValue_M4Aac", 0x431A1DCA, kTypeM4Aac},
        {"cellM4AacDec2ch", "_SceAdecCorrectPtsValue_M4Aac2ch", 0x869E328D, kTypeM4Aac2ch},
        {"cellM4AacDec2chmod", "_SceAdecCorrectPtsValue_M4Aac2chmod", 0x156FE2F0, kTypeM4Aac2chMod}};
    for (const auto& fn : fns) {
        INFO(fn.name);
        REQUIRE(fn.nid == recomp::nid_of(fn.name));
        const u32 d = link_own_function(f, fn.library, fn.nid);
        const auto correct = [&](u32 upper, u32 lower) {
            ps_write32(b, kPts, upper);
            ps_write32(b, kPts + 4, lower);
            return static_cast<u32>(hle::call_guest(t.ctx, d, {kCore, kPts}));
        };
        std::memset(b + kCore, 0, 0x200);
        ps_write32(b, kCore + 0x154, fn.type);
        ps_write32(b, kCore + 0x160, kTsInvalid);
        ps_write32(b, kCore + 0x164, kTsInvalid);
        ps_write32(b, kCore + 0x16C, 720);
        CHECK(correct(kTsInvalid, kTsInvalid) == 0); // none before: none
        CHECK(ps_read32(b, kPts) == kTsInvalid);
        CHECK(ps_read32(b, kPts + 4) == kTsInvalid);
        CHECK(correct(0, 0x1000) == 0); // kept, and the last
        CHECK(ps_read32(b, kPts + 4) == 0x1000);
        ps_write32(b, kCore + 0x16C, 1152);
        CHECK(correct(kTsInvalid, kTsInvalid) == 0); // the last plus its frame's 720
        CHECK(ps_read32(b, kPts) == 0);
        CHECK(ps_read32(b, kPts + 4) == 0x1000 + 720);
        CHECK(correct(kTsInvalid, kTsInvalid) == 0); // that one plus its 1152
        CHECK(ps_read32(b, kPts + 4) == 0x1000 + 720 + 1152);
        ps_write32(b, kCore + 0x160, 0);
        ps_write32(b, kCore + 0x164, 0xFFFFFF00); // the lower word wraps: bit 32 goes on
        CHECK(correct(kTsInvalid, kTsInvalid) == 0);
        CHECK(ps_read32(b, kPts) == 1);
        CHECK(ps_read32(b, kPts + 4) == 0xFFFFFF00u + 1152);
        CHECK(correct(kTsInvalid, 5) == 0x80610001);         // half a timestamp
        ps_write32(b, kCore + 0x154, kTypeMp3);
        CHECK(correct(0, 0x2000) == 0x80610001);             // a core of another codec
        ps_write32(b, kCore + 0x154, fn.type);
        CHECK(static_cast<u32>(hle::call_guest(t.ctx, d, {kCore, 0x30000000})) == 0x80610001); // not guest memory
    }
}

#endif
