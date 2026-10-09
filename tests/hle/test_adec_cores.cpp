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
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "adec_test_util.hpp"
#include "hle_test_util.hpp"

#if PSPOCKET_HAVE_FFMPEG
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

#if PSPOCKET_HAVE_FFMPEG

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

#endif

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

#if PSPOCKET_HAVE_FFMPEG

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

#endif

} // namespace

#if PSPOCKET_HAVE_FFMPEG

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
