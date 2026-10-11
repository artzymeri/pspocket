// cellAdec's cores of D-621 on FFmpeg: Dolby Digital Plus, MPEG-2 AAC, MP3 Surround and ATRAC3 multichannel. Streams
// FFmpeg's encoders write in the tests' own FFmpeg (E-AC-3, AC-3, AAC: PSPOCKET_FFMPEG_ENCODERS, D-577), or frames
// built here (MP3, ATRAC3): decoded through pspocket's cellAdec, the PCM against FFmpeg's own decoding of the same
// units, every item, information block, PTS and error. The tests need FFmpeg (D-606). No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

#include "adec_test_util.hpp"
#include "hle_test_util.hpp"
#include "pspocket/recomp/nid.hpp"

#if PSPOCKET_HAVE_FFMPEG
#include "adec_ffmpeg.hpp" // runtime/hle/src (internal): the cores' FfContext layout
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::adec_test;

#if PSPOCKET_HAVE_FFMPEG

namespace {

constexpr u32 kTsInvalid = 0xFFFFFFFF;
constexpr u32 kFatal = 0x80610001;

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

std::vector<u8> concat(std::initializer_list<std::vector<u8>> parts)
{
    std::vector<u8> v;
    for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
    return v;
}

// ---- a decoder's life through pspocket's cellAdec (test_adec_cores.cpp's run_core)

struct Out {
    u32 handle = 0, status = 0, size = 0, pts_upper = 0, pts_lower = 0;
    u64 user_data = 0;
    std::vector<u8> bsi;
    std::vector<u8> pcm;
};

struct CoreRun {
    std::vector<std::string> lines;
    u32 mem = 0, version_upper = 0, version_lower = 0;
    std::vector<Out> outs;
    std::vector<std::pair<u32, u32>> callbacks; // (type, data)
};

// The units' PTS: given for all but `missing` (0x1000 plus `step` each), user data 0x77000000 + index.
struct Feed {
    std::vector<std::vector<u8>> aus;
    std::vector<u32> missing;
    u32 step = 2160;
    u32 bsi_size = 0x40;
    u32 au_size = 0; // the size each unit is given with (0: its own)
};

constexpr u32 kPcmSpan = 0x30000; // the PCM a test reads back (the cores' largest units fit)

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
    REQUIRE(run.mem <= kDecoderMemSize);
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
            REQUIRE(feed.aus[k].size() <= 0x2000);
            std::memcpy(b + at, feed.aus[k].data(), feed.aus[k].size());
            const bool missing = std::find(feed.missing.begin(), feed.missing.end(), k) != feed.missing.end();
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, feed.au_size ? feed.au_size : static_cast<u32>(feed.aus[k].size()));
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
            REQUIRE(o.size <= kPcmSpan);
            std::memset(b + kPcm, 0xCC, kPcmSpan);
            if (r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}) == 0) o.pcm.assign(b + kPcm, b + kPcm + o.size);
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

// Each unit's callbacks: AUDONE, then PCMOUT with CELL_OK; for a unit that failed, ERROR with its error first and
// PCMOUT with it. SEQDONE last.
bool flow_ok(const CoreRun& run, u32 aus, const std::vector<std::pair<u32, u32>>& errors)
{
    std::vector<std::pair<u32, u32>> want;
    for (u32 k = 0; k < aus; k++) {
        u32 e = 0;
        for (const auto& [at, code] : errors)
            if (at == k) e = code;
        if (e) want.emplace_back(2, e);
        want.emplace_back(0, 0);
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

// The PTS of every out: the one given, or for a unit without one the previous plus `step` (the core's own correction
// of a codec whose frames last `step` ticks).
bool pts_ok(const CoreRun& run, const Feed& feed)
{
    for (u32 k = 0; k < run.outs.size(); k++)
        if (run.outs[k].pts_upper != 0 || run.outs[k].pts_lower != 0x1000 + feed.step * k) return false;
    return true;
}

// ---- signals, FFmpeg's encoders and decoders

using Unit = std::vector<std::vector<float>>; // planar: a unit's channels

// In channel c a sine of freqs[c] Hz, `amp` of full scale, `samples` a channel at `rate`.
std::vector<std::vector<double>> tones(u32 rate, u32 samples, const std::vector<double>& freqs, double amp = 0.3)
{
    std::vector<std::vector<double>> v(freqs.size());
    for (usize c = 0; c < freqs.size(); c++)
        for (u32 i = 0; i < samples; i++)
            v[c].push_back(amp * std::sin(2 * std::numbers::pi * freqs[c] * static_cast<double>(i) / rate));
    return v;
}

// A 24-bit sample of x, as an encoder of 32-bit samples is given it.
s32 to24(double x) { return static_cast<s32>(std::lround(x * 8388607.0)); }

struct Encoded {
    std::vector<std::vector<u8>> packets;
    std::vector<u8> extradata;
    u32 frame_size = 0, block_align = 0;
    s64 bit_rate = 0;
};

// What FFmpeg's encoder `name` made of the planar `input` in the channel layout `layout` (FFmpeg's name: "stereo",
// "5.1(side)"...), its experimental encoders allowed: floats where it takes them, else 24-bit samples in 32 bits, else
// 16-bit samples.
Encoded encode(const char* name, u32 rate, const char* layout, s64 bit_rate, const std::vector<std::vector<double>>& input,
    const std::function<void(AVCodecContext*)>& setup = {})
{
    Encoded e;
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return e;
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->sample_rate = static_cast<int>(rate);
    REQUIRE(av_channel_layout_from_string(&av->ch_layout, layout) == 0);
    REQUIRE(av->ch_layout.nb_channels == static_cast<int>(input.size()));
    av->bit_rate = bit_rate;
    av->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    const void* formats = nullptr;
    int count = 0;
    REQUIRE(avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count) >= 0);
    const auto has = [&](AVSampleFormat f) {
        for (int i = 0; i < count; i++)
            if (static_cast<const AVSampleFormat*>(formats)[i] == f) return true;
        return false;
    };
    av->sample_fmt = AV_SAMPLE_FMT_S16;
    for (const AVSampleFormat f : {AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_FLTP})
        if (has(f)) av->sample_fmt = f;
    if (setup) setup(av);
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    e.extradata.assign(av->extradata, av->extradata + av->extradata_size);
    e.frame_size = av->frame_size ? static_cast<u32>(av->frame_size) : 1024;
    e.block_align = static_cast<u32>(av->block_align);
    e.bit_rate = av->bit_rate;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const u32 channels = static_cast<u32>(input.size()), total = static_cast<u32>(input[0].size());
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
                const double x = input[c][at + i];
                switch (av->sample_fmt) {
                case AV_SAMPLE_FMT_FLTP: reinterpret_cast<float*>(frame->data[c])[i] = static_cast<float>(x); break;
                case AV_SAMPLE_FMT_S32P: reinterpret_cast<s32*>(frame->data[c])[i] = to24(x) * 256; break;
                case AV_SAMPLE_FMT_S32: reinterpret_cast<s32*>(frame->data[0])[i * channels + c] = to24(x) * 256; break;
                case AV_SAMPLE_FMT_S16P:
                    reinterpret_cast<s16*>(frame->data[c])[i] = static_cast<s16>(std::lround(x * 32767));
                    break;
                default:
                    reinterpret_cast<s16*>(frame->data[0])[i * channels + c] = static_cast<s16>(std::lround(x * 32767));
                    break;
                }
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

// A frame's sample as the cores convert it (adec_ffmpeg.cpp, FfDecoder::sample_of).
float sample_of(const AVFrame* f, u32 c, u32 i)
{
    const u32 channels = static_cast<u32>(f->ch_layout.nb_channels);
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float*>(f->extended_data[c])[i];
    case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(f->extended_data[0])[i * channels + c];
    case AV_SAMPLE_FMT_S16P: return static_cast<float>(reinterpret_cast<const s16*>(f->extended_data[c])[i]) / 32768.0f;
    case AV_SAMPLE_FMT_S16:
        return static_cast<float>(reinterpret_cast<const s16*>(f->extended_data[0])[i * channels + c]) / 32768.0f;
    case AV_SAMPLE_FMT_S32P:
        return static_cast<float>(static_cast<double>(reinterpret_cast<const s32*>(f->extended_data[c])[i]) / 2147483648.0);
    default:
        return static_cast<float>(
            static_cast<double>(reinterpret_cast<const s32*>(f->extended_data[0])[i * channels + c]) / 2147483648.0);
    }
}

// FFmpeg's own decoding of the units (the reference): each unit's packets (`packet_bytes` each, or the whole unit) in
// order, every frame's samples as planar floats; the channels' ids from the first frame.
struct Ref {
    std::vector<Unit> units;
    std::vector<int> ids;
    u32 rate = 0;
};
Ref reference(const char* decoder, const std::vector<std::vector<u8>>& aus,
    const std::function<void(AVCodecContext*)>& setup = {}, u32 packet_bytes = 0)
{
    Ref ref;
    const AVCodec* codec = avcodec_find_decoder_by_name(decoder);
    REQUIRE(codec);
    AVCodecContext* av = avcodec_alloc_context3(codec);
    if (setup) setup(av);
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    for (const auto& au : aus) {
        Unit unit;
        const u32 step = packet_bytes ? packet_bytes : static_cast<u32>(au.size());
        for (u32 at = 0; at + step <= au.size(); at += step) {
            std::vector<u8> data(au.begin() + at, au.begin() + at + step);
            data.resize(step + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            pkt->data = data.data();
            pkt->size = static_cast<int>(step);
            if (avcodec_send_packet(av, pkt) != 0) continue;
            while (avcodec_receive_frame(av, frame) == 0) {
                const u32 channels = static_cast<u32>(frame->ch_layout.nb_channels);
                if (ref.ids.empty()) {
                    for (u32 c = 0; c < channels; c++)
                        ref.ids.push_back(av_channel_layout_channel_from_index(&frame->ch_layout, c));
                    ref.rate = static_cast<u32>(frame->sample_rate);
                }
                unit.resize(channels);
                for (u32 c = 0; c < channels; c++)
                    for (u32 i = 0; i < static_cast<u32>(frame->nb_samples); i++) unit[c].push_back(sample_of(frame, c, i));
                av_frame_unref(frame);
            }
        }
        ref.units.push_back(std::move(unit));
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return ref;
}

// A unit as the cores write it: interleaved big-endian floats held below 1, or 16-bit samples floored.
std::vector<u8> pcm_of(const Unit& unit, bool sixteen = false)
{
    std::vector<u8> v;
    const usize n = unit.empty() ? 0 : unit[0].size();
    for (usize i = 0; i < n; i++) {
        for (const auto& ch : unit) {
            const float s = ch[i];
            if (sixteen) {
                const s32 q = s >= 1.f ? 0x7FFF : s <= -1.f ? -0x8000 : static_cast<s32>(std::floor(s * 32768.f));
                v.push_back(static_cast<u8>(static_cast<u32>(q) >> 8));
                v.push_back(static_cast<u8>(q));
            } else {
                const float c = s <= -1.f ? -1.f : s;
                u32 bits = std::bit_cast<u32>(c);
                if (s >= std::bit_cast<float>(std::bit_cast<u32>(1.f) - 1)) bits = 0x3F7FFFFF;
                for (u32 k = 0; k < 4; k++) v.push_back(static_cast<u8>(bits >> (24 - 8 * k)));
            }
        }
    }
    return v;
}

// Channel c of `channels` from big-endian PCM.
std::vector<float> channel_of(const std::vector<u8>& pcm, u32 channels, u32 c, bool sixteen = false)
{
    std::vector<float> x;
    const u32 bytes = sixteen ? 2 : 4;
    for (usize i = c * bytes; i + bytes <= pcm.size(); i += channels * bytes) {
        if (sixteen) x.push_back(static_cast<float>(static_cast<s16>(static_cast<u16>(pcm[i] << 8 | pcm[i + 1]))) / 32768.f);
        else x.push_back(std::bit_cast<float>(be32(pcm, i)));
    }
    return x;
}

std::vector<float> joined(const CoreRun& run, u32 channels, u32 c, bool sixteen = false)
{
    std::vector<float> v;
    for (const Out& o : run.outs) {
        const auto x = channel_of(o.pcm, channels, c, sixteen);
        v.insert(v.end(), x.begin(), x.end());
    }
    return v;
}

std::vector<float> floats(const std::vector<double>& x)
{
    std::vector<float> v;
    for (const double d : x) v.push_back(static_cast<float>(d));
    return v;
}

// How close `out` comes to `in` once the codec's delay (found by correlation, up to `max_delay` samples) is taken out:
// the signal-to-noise ratio in dB, away from both ends.
double snr_db(const std::vector<float>& in, const std::vector<float>& out, u32 max_delay)
{
    const usize n = std::min(in.size(), out.size());
    if (n < 2 * static_cast<usize>(max_delay) + 4096) return -999;
    u32 best = 0;
    double best_corr = -1e300;
    for (u32 d = 0; d <= max_delay; d++) {
        double c = 0;
        for (usize i = max_delay; i < max_delay + 4096; i++) c += static_cast<double>(in[i]) * static_cast<double>(out[i + d]);
        if (c > best_corr) {
            best_corr = c;
            best = d;
        }
    }
    double signal = 0, noise = 0;
    for (usize i = max_delay; i + best + max_delay < n; i++) {
        const double a = static_cast<double>(in[i]), b = static_cast<double>(out[i + best]);
        signal += a * a;
        noise += (a - b) * (a - b);
    }
    return noise > 0 ? 10 * std::log10(signal / noise) : 999;
}

bool close_to(const std::vector<float>& a, const std::vector<float>& b, double tolerance)
{
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); i++)
        if (std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) > tolerance) return false;
    return true;
}

} // namespace

// ---- the codecs' parameters and frames built here

namespace {

constexpr u32 kTypeM2Aac = 7, kTypeEac3 = 8, kTypeMp3s = 23, kTypeAtrac3Multi = 31;

// CellAdecParamAc3 (RPCS3's layout, which Dolby Digital Plus takes, D-621). The pointers give each of Dolby's channels
// (L, C, R, Ls, Rs, LFE) its output slot: by default WAV's order (L, R, C, LFE, Ls, Rs), as FFmpeg's.
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
        const u32 bits = std::bit_cast<u32>(x);
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

// An ADTS frame (no CRC) around a raw AAC frame, MPEG-2's (`id` 1) or MPEG-4's (0), with the object type, rate index
// and channel configuration of the AudioSpecificConfig `asc`.
std::vector<u8> adts(const std::vector<u8>& asc, const std::vector<u8>& raw, u32 id)
{
    const u32 aot = asc[0] >> 3, sfi = (asc[0] & 7u) << 1 | asc[1] >> 7, channels = (asc[1] >> 3) & 15u;
    BitWriter w;
    w.put(0xFFF, 12);
    w.put(id, 1);
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

// An MPEG-1 layer III frame at 48 kHz, 128 kbit/s (test_adec_cores.cpp's builder): in each granule and channel one
// spectral line, lines[ch], quantised to 1, coded in the count1 region with table B; no bit reservoir. `layer2` writes
// a layer II header instead (a frame MP3 refuses).
std::vector<u8> mp3_frame(u32 channels, std::array<u32, 2> lines, bool layer2 = false)
{
    constexpr u32 kFrameBytes = 144 * 1000 * 128 / 48000, kGain = 200;
    BitWriter w;
    w.put(0x7FF, 11);
    w.put(3, 2);              // MPEG-1
    w.put(layer2 ? 2 : 1, 2); // layer III (or II)
    w.put(1, 1);              // no CRC
    w.put(9, 4);              // 128 kbit/s
    w.put(1, 2);              // 48 kHz
    w.put(0, 2);              // padding, private
    w.put(channels == 1 ? 3 : 0, 2);
    w.put(0, 2); // mode extension
    w.put(0, 1); // copyright
    w.put(1, 1); // original
    w.put(0, 2); // emphasis
    const auto part23 = [&](u32 ch) { return 4 * (lines[ch] / 4) + 5; };
    w.put(0, 9); // main_data_begin
    w.put(0, channels == 1 ? 5 : 3);
    w.put(0, 4 * channels); // scfsi
    for (u32 gr = 0; gr < 2; gr++) {
        for (u32 ch = 0; ch < channels; ch++) {
            w.put(part23(ch), 12);
            w.put(0, 9); // big_values
            w.put(kGain, 8);
            w.put(0, 4); // scalefac_compress
            w.put(0, 1);
            w.put(0, 15 + 4 + 3);
            w.put(0, 1); // preflag
            w.put(0, 1); // scalefac_scale
            w.put(1, 1); // count1table_select: table B
        }
    }
    for (u32 gr = 0; gr < 2; gr++) {
        for (u32 ch = 0; ch < channels; ch++) {
            const u32 line = lines[ch];
            for (u32 q = 0; q < line / 4; q++) w.put(15, 4);
            w.put(15 - (8u >> (line % 4)), 4);
            w.put(0, 1);
        }
    }
    std::vector<u8> f = w.bytes;
    f.resize(kFrameBytes, 0);
    return f;
}

// One channel's ATRAC3 sound unit of `unit` bytes (test_adec_cores.cpp's atrac3_frame for one channel): its lowest QMF
// band only, its spectrum one line, `line`, at 20 in selector 7. Its sound is a tone at (line + 0.5) * rate / 2048 Hz.
// Line 0: the silent unit libatrac3multi writes (0xA2, then zeros).
constexpr u32 kSubbands[33] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 288, 320,
    352, 384, 416, 448, 480, 512, 576, 640, 704, 768, 896, 1024};
std::vector<u8> atrac3_unit(u32 line, u32 unit = 192)
{
    std::vector<u8> u(unit, 0);
    if (!line) {
        u[0] = 0xA2;
        return u;
    }
    BitWriter w;
    w.put(0x28, 6); // the sound unit's ID
    w.put(0, 2);    // one QMF band
    w.put(0, 3);    // no gain control points
    w.put(0, 5);    // no tonal components
    u32 sb = 0;
    while (kSubbands[sb + 1] <= line) sb++;
    w.put(sb, 5);
    w.put(1, 1); // constant length coding
    for (u32 i = 0; i <= sb; i++) w.put(i == sb ? 7 : 0, 3);
    w.put(48, 6);
    for (u32 j = kSubbands[sb]; j < kSubbands[sb + 1]; j++) w.put(j == line ? 20 : 0, 6);
    std::copy(w.bytes.begin(), w.bytes.end(), u.begin());
    return u;
}

// FFmpeg's ATRAC3 decoder for one channel's units (as the multichannel core opens one for each track).
std::function<void(AVCodecContext*)> atrac3_mono(u32 unit = 192)
{
    return [=](AVCodecContext* av) {
        av_channel_layout_uninit(&av->ch_layout);
        av_channel_layout_default(&av->ch_layout, 1);
        av->sample_rate = 44100;
        av->block_align = static_cast<int>(unit);
        static constexpr u8 kExtra[14] = {1, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
        av->extradata = static_cast<u8*>(av_mallocz(sizeof(kExtra) + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(av->extradata, kExtra, sizeof(kExtra));
        av->extradata_size = sizeof(kExtra);
    };
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

} // namespace

// ---- Dolby Digital Plus (D-621)

TEST_CASE("cellAdec: Dolby Digital Plus on FFmpeg, 5.1 E-AC-3 that FFmpeg's encoder wrote: PCM, information, PTS "
          "(D-621)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("eac3")) SKIP("this FFmpeg has no eac3 encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const auto input = tones(48000, 96000, {500, 800, 1200, 60, 1700, 2300});
    const Encoded enc = encode("eac3", 48000, "5.1(side)", 640000, input);
    REQUIRE(enc.packets.size() > 50);
    Feed feed;
    feed.bsi_size = 0x44;
    feed.step = 2880;
    feed.aus = enc.packets;
    feed.missing = {2, 3};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeEac3, ac3_param({}), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == feed.aus.size());
    CHECK(flow_ok(run, static_cast<u32>(feed.aus.size()), {}));
    CHECK(pts_ok(run, feed)); // 1536 samples at 48 kHz: 2880 ticks
    CHECK(run.version_lower == 0x01000000);
    const Ref ref = reference("eac3", feed.aus);
    for (u32 k = 0; k < run.outs.size(); k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.size == 1536 * 6 * 4);
        CHECK(o.pcm == pcm_of(ref.units[k])); // 3/2 with the LFE: no downmix, FFmpeg's samples in WAV's order
        CHECK(o.user_data == 0x77000000 + k);
        CHECK(be32(o.bsi, 0x00) == 1);          // codecType: E-AC-3
        CHECK(be32(o.bsi, 0x04) == 0x01000000);
        CHECK(be32(o.bsi, 0x0C) == k + 1);      // totalCallNum
        CHECK(be32(o.bsi, 0x10) == 640);        // kbit/s
        CHECK(be32(o.bsi, 0x14) == o.size);     // pcmSize
        CHECK(be32(o.bsi, 0x18) == 8 * 2560);   // esSizeBit
        CHECK(be32(o.bsi, 0x1C) == 0);
        CHECK(o.bsi[0x21] == 1);                // validity
        CHECK(o.bsi[0x22] == 6);                // channels with the LFE
        CHECK(o.bsi[0x23] == 0);                // fscod: 48 kHz
        CHECK(o.bsi[0x24] == 1);                // float
        CHECK(o.bsi[0x25] == 6);
        CHECK(be32(o.bsi, 0x28) == be32(feed.aus[k], 2)); // bsi()'s first 64 bits, from strmtyp on
        CHECK(be32(o.bsi, 0x2C) == be32(feed.aus[k], 6));
        CHECK((u32{o.bsi[0x30]} << 8 | o.bsi[0x31]) == 2560 / 2 - 1); // frmsiz
        CHECK(o.bsi[0x32] == 7);                // acmod 3/2
        CHECK(o.bsi[0x33] == 1);                // LFE
        CHECK(o.bsi[0x3B] == 16);               // bsid
        CHECK(o.bsi[0x3C] == 0);                // no mixing metadata
        CHECK(o.bsi[0x3E] == 0);                // no informational metadata
        CHECK(o.bsi[0x42] == 0);                // the CRC matched
        CHECK(o.bsi[0x43] == 0);                // no downmix
    }
    for (u32 c = 0; c < 6; c++) {
        const double snr = snr_db(floats(input[c]), joined(run, 6, c), 3000);
        INFO(std::format("channel {}: SNR {:.1f} dB", c, snr));
        CHECK(snr > 20);
    }
}

TEST_CASE("cellAdec: Dolby Digital Plus on FFmpeg: the stream's Lo/Ro levels, units of several frames, dependent "
          "substreams and other programs left, AC-3 frames, refusals (D-621)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("eac3") || !avcodec_find_encoder_by_name("ac3"))
        SKIP("this FFmpeg has no eac3 or ac3 encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const auto input = tones(48000, 48000, {500, 800, 1200, 60, 1700, 2300});
    // Mixing metadata (Lo/Ro preferred, its centre and surround levels -6 dB) and informational metadata.
    const Encoded enc = encode("eac3", 48000, "5.1(side)", 640000, input, [](AVCodecContext* av) {
        av_opt_set(av->priv_data, "dmix_mode", "loro", 0);
        av_opt_set_double(av->priv_data, "loro_cmixlev", 0.5, 0);
        av_opt_set_double(av->priv_data, "loro_surmixlev", 0.5, 0);
        av_opt_set_int(av->priv_data, "copyright", 1, 0);
        av_opt_set_int(av->priv_data, "original", 1, 0);
    });
    REQUIRE(enc.packets.size() >= 12);
    Feed feed;
    feed.bsi_size = 0x44;
    feed.step = 2880;
    feed.aus.assign(enc.packets.begin(), enc.packets.begin() + 12);
    const CoreRun full = run_core(f.base(), hle_calls(f, t), kTypeEac3, ac3_param({}), feed);
    const CoreRun loro = run_core(f.base(), hle_calls(f, t), kTypeEac3,
        ac3_param({.mode = 2, .lfe = 0, .channels = 2, .pointers = {0, -1, 1, -1, -1, -1}}), feed);
    INFO(all(loro));
    REQUIRE(full.outs.size() == 12);
    REQUIRE(loro.outs.size() == 12);
    for (u32 k = 0; k < 12; k++) {
        INFO("access unit " << k);
        const Out& o = loro.outs[k];
        CHECK(o.bsi[0x3C] == 1); // mixing metadata
        CHECK(o.bsi[0x3D] == 2); // dmixmod: Lo/Ro preferred
        CHECK(o.bsi[0x3E] == 1); // informational metadata
        CHECK(o.bsi[0x38] == 1); // copyright
        CHECK(o.bsi[0x39] == 1); // original
        CHECK(o.bsi[0x43] == 2); // a downmix, not Lt/Rt
        // Lo = L + 0.5 C + 0.5 Ls, over the coefficients' sum 2 (and Ro likewise), from the full output's channels.
        const auto l = channel_of(full.outs[k].pcm, 6, 0), r = channel_of(full.outs[k].pcm, 6, 1),
                   c = channel_of(full.outs[k].pcm, 6, 2), ls = channel_of(full.outs[k].pcm, 6, 4),
                   rs = channel_of(full.outs[k].pcm, 6, 5);
        std::vector<float> lo, ro;
        for (usize i = 0; i < l.size(); i++) {
            lo.push_back(0.5f * l[i] + 0.25f * c[i] + 0.25f * ls[i]);
            ro.push_back(0.5f * r[i] + 0.25f * c[i] + 0.25f * rs[i]);
        }
        CHECK(close_to(channel_of(o.pcm, 2, 0), lo, 1e-5));
        CHECK(close_to(channel_of(o.pcm, 2, 1), ro, 1e-5));
    }

    // Units of two frames, a frame with a dependent substream after it, one with another program's: only program 1's
    // independent frames are decoded, in order; a unit of 3072 samples lasts 5760 ticks (the third unit's PTS).
    const auto& p = enc.packets;
    std::vector<u8> dependent = p[3], other = p[4];
    dependent[2] |= 0x40; // strmtyp 1
    other[2] |= 0x08;     // substreamid 1
    Feed mixed;
    mixed.bsi_size = 0x44;
    mixed.step = 2880;
    mixed.aus = {concat({p[0], p[1]}), p[2], concat({p[3], dependent}), concat({p[4], other}), p[5]};
    mixed.missing = {1};
    const CoreRun m = run_core(f.base(), hle_calls(f, t), kTypeEac3, ac3_param({}), mixed);
    INFO(all(m));
    REQUIRE(m.outs.size() == 5);
    CHECK(flow_ok(m, 5, {}));
    const Ref ref = reference("eac3", {p[0], p[1], p[2], p[3], p[4], p[5]});
    Unit first = ref.units[0];
    for (usize c = 0; c < first.size(); c++) first[c].insert(first[c].end(), ref.units[1][c].begin(), ref.units[1][c].end());
    CHECK(m.outs[0].pcm == pcm_of(first));
    CHECK(m.outs[0].size == 3072 * 6 * 4);
    CHECK(be32(m.outs[0].bsi, 0x18) == 8 * (p[0].size() + p[1].size()));
    for (u32 k = 1; k < 5; k++) CHECK(m.outs[k].pcm == pcm_of(ref.units[k + 1]));
    CHECK(m.outs[1].pts_lower == 0x1000 + 5760); // the first unit's 3072 samples
    CHECK(m.outs[3].bsi[0x42] == 0);              // the program's frame's CRC (the other program's is not read)

    // AC-3 frames through the Dolby Digital Plus core: codecType 0, FFmpeg's E-AC-3 decoder's samples.
    const Encoded ac3 = encode("ac3", 48000, "stereo", 192000, tones(48000, 9216, {700, 1100}));
    REQUIRE(ac3.packets.size() >= 4);
    Feed ac3_feed;
    ac3_feed.bsi_size = 0x44;
    ac3_feed.step = 2880;
    ac3_feed.aus.assign(ac3.packets.begin(), ac3.packets.begin() + 4);
    const CoreRun a = run_core(f.base(), hle_calls(f, t), kTypeEac3,
        ac3_param({.mode = 2, .lfe = 0, .channels = 2, .pointers = {0, -1, 1, -1, -1, -1}}), ac3_feed);
    REQUIRE(a.outs.size() == 4);
    const Ref ac3_ref = reference("eac3", ac3_feed.aus);
    for (u32 k = 0; k < 4; k++) {
        CHECK(a.outs[k].pcm == pcm_of(ac3_ref.units[k]));
        CHECK(be32(a.outs[k].bsi, 0) == 0); // codecType: AC-3
        CHECK(a.outs[k].bsi[0x3B] == 8);     // bsid
        CHECK(a.outs[k].bsi[0x32] == 2);     // acmod 2/0
    }

    // Refused: a unit with no syncframe and one cut short (libadec's FATAL; the next unit decodes), and parameters.
    Feed bad;
    bad.bsi_size = 0x44;
    bad.step = 2880;
    bad.aus = {p[0], std::vector<u8>(64, 0), std::vector<u8>(p[2].begin(), p[2].begin() + 1000), p[3]};
    const CoreRun e = run_core(f.base(), hle_calls(f, t), kTypeEac3, ac3_param({}), bad);
    INFO(all(e));
    CHECK(flow_ok(e, 4, {{1, kFatal}, {2, kFatal}}));
    REQUIRE(e.outs.size() == 4);
    CHECK(e.outs[1].size == 0);
    CHECK(be32(e.outs[1].bsi, 0x1C) == kFatal);
    CHECK(e.outs[3].size == 1536 * 6 * 4);
    for (const Ac3Param& refused : {Ac3Param{.word = 2}, Ac3Param{.mode = 0}, Ac3Param{.channels = 7},
             Ac3Param{.pointers = {0, 0, 1, 2, 3, 4}}}) {
        const CoreRun r = run_core(f.base(), hle_calls(f, t), kTypeEac3, ac3_param(refused), Feed{});
        CHECK(std::find(r.lines.begin(), r.lines.end(), "cellAdecStartSeq -> 0x80610003") != r.lines.end());
    }
}

// ---- MPEG-2 AAC (D-621)

TEST_CASE("cellAdec: MPEG-2 AAC on FFmpeg, ADTS frames of MPEG-2 around FFmpeg's AAC encoder's: PCM, information, "
          "PTS, refusals (D-621)",
    "[hle][adec]")
{
    if (!avcodec_find_encoder_by_name("aac")) SKIP("this FFmpeg has no aac encoder (the tests' build: build-ffmpeg.sh)");
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const auto input = tones(48000, 96000, {1000, 2500}, 0.4);
    const Encoded enc = encode("aac", 48000, "stereo", 128000, input);
    REQUIRE(enc.packets.size() > 60);
    Feed feed;
    feed.bsi_size = 0x80;
    feed.step = 1920;
    for (const auto& raw : enc.packets) feed.aus.push_back(adts(enc.extradata, raw, 1));
    feed.missing = {2, 9};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeM2Aac, be32s({1, 0, 0, 0}), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == feed.aus.size());
    CHECK(flow_ok(run, static_cast<u32>(feed.aus.size()), {}));
    CHECK(pts_ok(run, feed)); // 1024 samples at 48 kHz, the core's own correction (type 25)
    const Ref ref = reference("aac", feed.aus);
    for (u32 k = 0; k < run.outs.size(); k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.pcm == pcm_of(ref.units[k]));
        CHECK(o.size == 1024 * 2 * 4);
        CHECK(be32(o.bsi, 0x00) == 48000);
        CHECK(be32(o.bsi, 0x04) == 2);
        CHECK(be32(o.bsi, 0x08) == 2); // front channels
        CHECK(be32(o.bsi, 0x24) == 1); // isBsiValid
        CHECK(be32(o.bsi, 0x28) == 1); // ADTS
        CHECK(be32(o.bsi, 0x2C) == 1); // its ID: MPEG-2
        CHECK(be32(o.bsi, 0x38) == 1); // LC
        CHECK(be32(o.bsi, 0x3C) == 3); // 48 kHz
        CHECK(be32(o.bsi, 0x44) == 2); // channel configuration
        CHECK(be32(o.bsi, 0x58) == feed.aus[k].size());
    }
    for (u32 c = 0; c < 2; c++) {
        const double snr = snr_db(floats(input[c]), joined(run, 2, c), 3000);
        INFO(std::format("channel {}: SNR {:.1f} dB", c, snr));
        CHECK(snr > 20);
    }
    // A unit without the sync and an SSR frame: libadec's FATAL (the next decodes); parameters StartSeq refuses: ARG.
    std::vector<u8> ssr = feed.aus[1];
    ssr[2] = static_cast<u8>((ssr[2] & 0x3F) | 0x80); // profile 2
    Feed bad;
    bad.bsi_size = 0x80;
    bad.step = 1920;
    bad.aus = {feed.aus[0], std::vector<u8>(32, 0), ssr, feed.aus[1]};
    const CoreRun e = run_core(f.base(), hle_calls(f, t), kTypeM2Aac, be32s({1, 0, 0, 0}), bad);
    INFO(all(e));
    CHECK(flow_ok(e, 4, {{1, kFatal}, {2, kFatal}}));
    for (const auto& refused : {be32s({3, 0, 0, 0}), be32s({2, 3, 3, 0}), be32s({2, 3, 2, 0}), be32s({1, 0, 0, 2})}) {
        const CoreRun r = run_core(f.base(), hle_calls(f, t), kTypeM2Aac, refused, Feed{});
        CHECK(std::find(r.lines.begin(), r.lines.end(), "cellAdecStartSeq -> 0x80610003") != r.lines.end());
    }
    // Raw data blocks of MPEG-2's LC profile decode as ADTS's did.
    Feed raw;
    raw.bsi_size = 0x80;
    raw.step = 1920;
    raw.aus.assign(enc.packets.begin(), enc.packets.begin() + 6);
    const CoreRun r = run_core(f.base(), hle_calls(f, t), kTypeM2Aac, be32s({2, 3, 1, 0}), raw);
    REQUIRE(r.outs.size() == 6);
    for (u32 k = 0; k < 6; k++) CHECK(r.outs[k].pcm == run.outs[k].pcm);
}

// ---- MP3 Surround (D-621)

TEST_CASE("cellAdec: MP3 Surround on FFmpeg, its stereo MP3 frames built here: PCM, information, PTS, refusals (D-621)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    Feed feed;
    feed.bsi_size = 0x1C;
    feed.step = 2160;
    for (u32 k = 0; k < 8; k++) feed.aus.push_back(mp3_frame(2, {45, 100}));
    feed.missing = {2, 5};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeMp3s, be32s({4}), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == 8);
    CHECK(flow_ok(run, 8, {}));
    CHECK(pts_ok(run, feed)); // 1152 samples at 48 kHz, the core's own correction (type 33)
    const Ref ref = reference("mp3float", feed.aus);
    for (u32 k = 0; k < 8; k++) {
        INFO("access unit " << k);
        const Out& o = run.outs[k];
        CHECK(o.pcm == pcm_of(ref.units[k]));
        CHECK(be32(o.bsi, 0x00) == be32(feed.aus[k], 0)); // the header
        CHECK(o.bsi[0x10] == 1);                           // no CRC
        CHECK(o.bsi[0x11] == 0);                           // stereo
        CHECK(be32(o.bsi, 0x18) == 0);
    }
    CHECK(energy_near(joined(run, 2, 0), 48000, 45.5 * 48000 / 1152, 60) > 0.8);
    // 16-bit PCM; a layer II header (FATAL, also in i_error_code); a word size StartSeq refuses (ARG).
    Feed bad;
    bad.bsi_size = 0x1C;
    bad.aus = {feed.aus[0], mp3_frame(2, {45, 100}, true), feed.aus[1]};
    const CoreRun e = run_core(f.base(), hle_calls(f, t), kTypeMp3s, be32s({3}), bad);
    INFO(all(e));
    CHECK(flow_ok(e, 3, {{1, kFatal}}));
    REQUIRE(e.outs.size() == 3);
    CHECK(be32(e.outs[1].bsi, 0x18) == kFatal);
    CHECK(e.outs[0].pcm == pcm_of(ref.units[0], true));
    const CoreRun r = run_core(f.base(), hle_calls(f, t), kTypeMp3s, be32s({5}), Feed{});
    CHECK(std::find(r.lines.begin(), r.lines.end(), "cellAdecStartSeq -> 0x80610003") != r.lines.end());
}

// ---- ATRAC3 multichannel (D-621)

namespace {

// Each track's channel unit decoded alone by FFmpeg's ATRAC3 decoder for one channel, a decoder per track, unit after
// unit: the reference of the multichannel core.
std::vector<Unit> atrac3_tracks(const std::vector<std::vector<std::vector<u8>>>& units_per_track)
{
    std::vector<Unit> out(units_per_track[0].size());
    for (const auto& track : units_per_track) {
        const Ref r = reference("atrac3", track, atrac3_mono());
        for (usize k = 0; k < r.units.size(); k++) {
            REQUIRE(r.units[k].size() == 1);
            out[k].push_back(r.units[k][0]);
        }
    }
    return out;
}

} // namespace

TEST_CASE("cellAdec: ATRAC3 multichannel on FFmpeg, tracks of channel units built here as libatrac3multi lays them "
          "out: PCM, information, PTS, refusals (D-621)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // A stream of four channels (their tones at lines 100, 60, 140 and 30), its frames 4 x 192 bytes; four tracks
    // playing channels 2 and 0, nothing (libatrac3multi's silent unit) and channel 3.
    const std::array<u32, 4> lines = {100, 60, 140, 30};
    const std::array<s32, 4> tracks = {2, 0, -1, 3};
    Feed feed;
    feed.bsi_size = 0x10;
    feed.step = 3840;
    std::vector<std::vector<std::vector<u8>>> per_track(4);
    for (u32 k = 0; k < 12; k++) {
        std::vector<u8> au;
        for (u32 i = 0; i < 4; i++) {
            const auto unit = atrac3_unit(tracks[i] < 0 ? 0 : lines[static_cast<usize>(tracks[i])]);
            au.insert(au.end(), unit.begin(), unit.end());
            per_track[i].push_back(unit);
        }
        feed.aus.push_back(au);
    }
    feed.missing = {2, 7};
    const CoreRun run = run_core(f.base(), hle_calls(f, t), kTypeAtrac3Multi, be32s({4, 0, 768, 0x84, 4}), feed);
    INFO(all(run));
    REQUIRE(run.outs.size() == 12);
    CHECK(flow_ok(run, 12, {}));
    CHECK(pts_ok(run, feed)); // the front end's step for ATRAC3 multichannel (type 39): 3840
    const std::vector<Unit> ref = atrac3_tracks(per_track);
    for (u32 k = 0; k < 12; k++) {
        INFO("access unit " << k);
        CHECK(run.outs[k].size == 1024 * 4 * 4);
        CHECK(run.outs[k].pcm == pcm_of(ref[k]));
        CHECK(run.outs[k].bsi == be32s({4, 0, 768, 4}));
    }
    CHECK(energy_near(joined(run, 4, 0), 44100, 140.5 * 44100 / 2048, 150) > 0.8);
    CHECK(energy_near(joined(run, 4, 3), 44100, 30.5 * 44100 / 2048, 150) > 0.8);
    for (const float s : joined(run, 4, 2)) REQUIRE(s == 0.0f); // the silent track

    // Eight tracks of a two-channel stream, its channels repeated: the unit given with the stream frame's 384 bytes,
    // its eight blocks read past them; 16-bit PCM.
    Feed eight;
    eight.bsi_size = 0x10;
    eight.step = 3840;
    eight.au_size = 384;
    std::vector<std::vector<std::vector<u8>>> per8(8);
    for (u32 k = 0; k < 6; k++) {
        std::vector<u8> au;
        for (u32 i = 0; i < 8; i++) {
            const auto unit = atrac3_unit(lines[i % 2]);
            au.insert(au.end(), unit.begin(), unit.end());
            per8[i].push_back(unit);
        }
        eight.aus.push_back(au);
    }
    const CoreRun r8 = run_core(f.base(), hle_calls(f, t), kTypeAtrac3Multi, be32s({8, 0, 384, 2, 2}), eight);
    INFO(all(r8));
    REQUIRE(r8.outs.size() == 6);
    CHECK(flow_ok(r8, 6, {}));
    const std::vector<Unit> ref8 = atrac3_tracks(per8);
    for (u32 k = 0; k < 6; k++) {
        CHECK(r8.outs[k].size == 1024 * 8 * 2);
        CHECK(r8.outs[k].pcm == pcm_of(ref8[k], true));
        CHECK(r8.outs[k].bsi == be32s({8, 0, 384, 2}));
    }
    // Refused by StartSeq: 9 tracks, joint stereo, frames of 100 bytes a channel, a word size, a stream of 9 channels.
    for (const auto& refused : {be32s({9, 0, 768, 0x84, 4}), be32s({2, 1, 384, 0x84, 2}), be32s({4, 0, 400, 0x84, 4}),
             be32s({4, 0, 768, 5, 4}), be32s({4, 0, 9 * 192, 0x84, 9})}) {
        const CoreRun r = run_core(f.base(), hle_calls(f, t), kTypeAtrac3Multi, refused, Feed{});
        CHECK(std::find(r.lines.begin(), r.lines.end(), "cellAdecStartSeq -> 0x80610003") != r.lines.end());
    }
    // A stream's channel count of 0 is the tracks'.
    Feed two = feed;
    two.aus.resize(3);
    for (auto& au : two.aus) au.resize(2 * 192);
    const CoreRun r0 = run_core(f.base(), hle_calls(f, t), kTypeAtrac3Multi, be32s({2, 0, 384, 0x84, 0}), two);
    REQUIRE(r0.outs.size() == 3);
    CHECK(r0.outs[0].bsi == be32s({2, 0, 384, 2}));
}

TEST_CASE("cellAtracMulti plays a four-channel ATRAC3 file through pspocket's ATRAC3 multichannel core where no "
          "firmware core is (D-621)",
    "[hle][adec][atrac]")
{
    InterpretMisses interpret;
    BigFixture f;
    REQUIRE(f.hle->link_internal_functions({"cellAtracMulti"}));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kAtHandle = kData + 0x8000, kAtOut = kData + 0x8400, kAtTracks = kData + 0x8500,
                  kAtFile = kData + 0x60000, kAtWork = kData + 0xB0000, kAtPcm = kData + 0x1A0000;
    // A RIFF WAVE ATRAC3 file (fmt 0x270, its 14 extra bytes, no joint stereo), 44.1 kHz, four channels of tones.
    const std::array<u32, 4> lines = {100, 60, 140, 30};
    constexpr u32 kFrames = 16, kFrame = 4 * 192;
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
    tag("RIFF");
    u32le(0);
    tag("WAVE");
    tag("fmt ");
    u32le(0x20);
    u16le(0x270);
    u16le(4);
    u32le(44100);
    u32le(44100 * kFrame / 1024);
    u16le(kFrame);
    u16le(0);
    u16le(14);
    u16le(1);
    u32le(0x1000);
    u16le(0);
    u16le(0);
    u16le(1);
    u16le(0);
    tag("data");
    u32le(kFrames * kFrame);
    for (u32 k = 0; k < kFrames; k++)
        for (const u32 line : lines) {
            const auto unit = atrac3_unit(line);
            file.insert(file.end(), unit.begin(), unit.end());
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
    ps_write32(b, kAtTracks, 3); // the first track plays channel 3, the second channel 0
    ps_write32(b, kAtTracks + 4, 0);
    const u32 size = static_cast<u32>(file.size());
    REQUIRE(call("cellAtracMultiSetDataAndGetMemSize", {kAtHandle, kAtFile, size, size, 2, kAtTracks, kAtOut}) == 0);
    REQUIRE(ps_read32(b, kAtOut) < 0xF0000);
    REQUIRE(call("cellAtracMultiCreateDecoder", {kAtHandle, kAtWork, 1000, 100}) == 0);
    std::vector<u8> pcm;
    u32 decoded = 0;
    for (u32 n = 0; n < kFrames + 4; n++) {
        std::memset(b + kAtPcm, 0xCC, 0x4000);
        if (call("cellAtracMultiDecode", {kAtHandle, kAtPcm, kAtOut, kAtOut + 4, kAtOut + 8}) != 0) break;
        const u32 samples = ps_read32(b, kAtOut);
        decoded += samples;
        pcm.insert(pcm.end(), b + kAtPcm, b + kAtPcm + samples * 2 * 4);
        if (ps_read32(b, kAtOut + 4)) break; // finished
    }
    INFO("samples decoded: " << decoded);
    CHECK(decoded >= (kFrames - 2) * 1024);
    CHECK(energy_near(channel_of(pcm, 2, 0), 44100, 30.5 * 44100 / 2048, 150) > 0.8);
    CHECK(energy_near(channel_of(pcm, 2, 1), 44100, 100.5 * 44100 / 2048, 150) > 0.8);
    CHECK(call("cellAtracMultiDeleteDecoder", {kAtHandle}) == 0);
}

#endif
