// cellAdec's DTS cores on FFmpeg (D-622): pspocket's own, where no firmware module gives Sony's: DTS
// (CELL_ADEC_TYPE_DTS, cellDTSdec's table 0x15248EC5), DTS-HD (CELL_ADEC_TYPE_DTSHD, cellDTSHDdec's 0x51AC2B6C), the
// core of DTS-HD (CELL_ADEC_TYPE_DTSHDCORE, cellDTSHDCOREdec's 0x6C8F4F1C) and DTS Express (CELL_ADEC_TYPE_DTSLBR,
// cellDTSLBRdec's 0x8C50AF52). The common part is adec_ffmpeg.cpp; FFmpeg's DTS decoder ("dca": Coherent Acoustics'
// core with its XCh, XXCH, X96 and XBR extensions, the extension substream's lossless XLL and low bit rate LBR)
// decodes.
//
// RPCS3 runs Sony's modules and has nothing of DTS's but the cellAdec types (DTS and DTS-HD core "removed in firmware
// 4.00, integrated into DTSHD"), the modules' names and NIDs, and that each core corrects PTS with a function of its
// own (ADEC_CORRECT_PTS_VALUE_TYPE_DTS 28, DTSHD 21, DTSHDCORE_UNK1 and UNK2 37 and 38, DTSLBR 31:
// _SceAdecCorrectPtsValue_DtsCore, _DtsHd, _DtsHd_Core, _DtsLbr) (rpcs3/Emu/Cell/Modules/cellAdec.cpp, cellAdec.h).
// The rest is pspocket's, from the format as ETSI TS 102 114 and FFmpeg (libavcodec/dca_core.c, dcadec.c) read it, to
// compare on the Mac:
// - What each decodes: DTS and the DTS-HD core the core substream alone (FFmpeg's core_only: a DTS-HD stream's
//   extension substream is passed over; a unit without a core is an error); DTS-HD and DTS Express all FFmpeg decodes
//   of a unit (the core with its extensions, XLL's lossless audio, LBR).
// - The parameters: lprm (adec_ffmpeg.hpp: CellAdecParamMpmc's layout). channelNumber STEREO or MONO asks FFmpeg for
//   stereo (the downmix coefficients a stream carries, where it has them; a stream without them decoded whole and
//   downmixed as the AAC cores do), MONO then that pair averaged; any other layout the whole stream, up to 8 channels.
// - An access unit is one DTS frame: a core frame (16-bit big-endian, or the 14-bit and little-endian forms FFmpeg
//   converts) with or without an extension substream, or an extension substream alone. At most 4096 samples a channel.
// - The PCM: big-endian floats (XLL's 16- and 24-bit samples exactly), interleaved in FFmpeg's channel order (WAV's).
// - The errors: libadec's own (ARG for parameters StartSeq refuses, BUSY, FATAL for a unit with no DTS sync or one
//   FFmpeg refuses, the reason logged), DTS's not being known.
// - The information block: ginfo's head (adec_ffmpeg.hpp); the stream's channels its core header's (AMODE and LFF)
//   where the unit has a 16-bit core, else those FFmpeg decoded; bitsPerSample FFmpeg's lossless resolution (XLL), else
//   the core's source resolution (PCMR); bitRate the core's (RATE) where it names one. Then the core header's fields
//   (each 0xFFFFFFFF without a 16-bit core): +0x28 AMODE, +0x2C the frame's bytes (FSIZE + 1), +0x30 SFREQ, +0x34 RATE,
//   +0x38 LFF; +0x3C 1 when the unit has an extension substream.
// - PTS: the core's own correction (AdecCore::correct_pts), the previous frame's PTS plus its length; the DTS-HD core's
//   type is 37 (RPCS3's DTSHDCORE_UNK1; 38 calls the same function).

#include <algorithm>
#include <array>
#include <format>

#include "adec_ffmpeg.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

#if PSPOCKET_HAVE_FFMPEG

extern "C" {
#include <libavutil/opt.h>
}

namespace pspocket::hle::adec::ffcore {
namespace {

constexpr u32 kSyncCore = 0x7FFE8001, kSyncCore14 = 0x1FFFE800, kSyncCoreLe = 0xFE7F0180, kSyncCore14Le = 0xFF1F00E8,
              kSyncExss = 0x64582025;
constexpr u32 kMaxSamples = 4096, kMaxChannels = 8, kNone = 0xFFFFFFFF;
constexpr u32 info_amode = ginfo::own + 0x0, info_frame_bytes = ginfo::own + 0x4, info_sfreq = ginfo::own + 0x8,
              info_rate = ginfo::own + 0xC, info_lff = ginfo::own + 0x10, info_exss = ginfo::own + 0x14;

enum class Kind : u8 { Core, Hd, HdCore, Lbr };

// The core frame header (ETSI TS 102 114 5.3.1, FFmpeg's dca_core.c parse_frame_header): what the information gives.
struct CoreHeader {
    u32 nblks = 0, fsize = 0, amode = 0, sfreq = 0, rate = 0, lff = 0, pcmr = 0;
};
bool read_core(const u8* p, u32 size, CoreHeader& h)
{
    if (size < 12 || ps_ld32(p) != kSyncCore) return false;
    Bits b(p + 4, size - 4);
    b.skip(1 + 5);         // FTYPE, SHORT
    const bool cpf = b.flag();
    h.nblks = b.get(7);
    h.fsize = b.get(14);
    h.amode = b.get(6);
    h.sfreq = b.get(4);
    h.rate = b.get(5);
    b.skip(1 + 1 + 1 + 1 + 1 + 3 + 1 + 1); // FixedBit, DYNF, TIMEF, AUXF, HDCD, EXT_AUDIO_ID, EXT_AUDIO, ASPF
    h.lff = b.get(2);
    b.skip(1); // HFLAG
    if (cpf) b.skip(16);
    b.skip(1 + 4 + 2); // FILTS, VERNUM, CHIST
    h.pcmr = b.get(3);
    return !b.overrun;
}

// AMODE's channels and, for the arrangements FFmpeg gives a fixed layout, their mask (WAVE_FORMAT_EXTENSIBLE's): A;
// A + B; L + R; L + R as sum and difference; Lt + Rt; C + L + R; L + R + S; C + L + R + S; L + R + Ls + Rs;
// C + L + R + Ls + Rs; then the six- to eight-channel arrangements (count only).
constexpr u8 kAmodeChannels[16] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
constexpr u32 kAmodeMask[16] = {0x4, 0x3, 0x3, 0x3, 0x3, 0x7, 0x103, 0x107, 0x603, 0x607};
// RATE in bits per second (TS 102 114 table 5-7); 29 to 31 are open, variable and lossless (none).
constexpr u32 kRates[29] = {32000, 56000, 64000, 96000, 112000, 128000, 192000, 224000, 256000, 320000, 384000, 448000,
    512000, 576000, 640000, 768000, 960000, 1024000, 1152000, 1280000, 1344000, 1408000, 1411200, 1472000, 1536000,
    1920000, 2048000, 3072000, 3840000};
constexpr u8 kPcmrBits[8] = {16, 16, 20, 20, 0, 24, 24, 0};

class Dts final : public Codec {
public:
    Dts(Kind kind, std::string_view name) : kind_(kind), name_(name) {}

    s32 start(const u8* param) override
    {
        channel_number_ = ps_ld32(param + lprm::channel_number);
        const bool two = channel_number_ == CELL_ADEC_CH_MONO || channel_number_ == CELL_ADEC_CH_STEREO;
        const bool core_only = kind_ == Kind::Core || kind_ == Kind::HdCore;
        configured_ = ff_.open("dca", name_, [&](AVCodecContext* av) {
            av_opt_set_int(av, "core_only", core_only, AV_OPT_SEARCH_CHILDREN);
            if (two) av_opt_set(av, "downmix", "stereo", AV_OPT_SEARCH_CHILDREN);
        });
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_ARG);
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = kind_ == Kind::Core ? CORRECT_PTS_DTS : kind_ == Kind::Hd ? CORRECT_PTS_DTSHD :
                     kind_ == Kind::HdCore ? CORRECT_PTS_DTSHDCORE : CORRECT_PTS_DTSLBR;
        const auto fail = [&] {
            d.error = error(CELL_ADEC_ERROR_FATAL);
            ps_write32(base, bsi + ginfo::error_code, CELL_ADEC_ERROR_FATAL);
            return d;
        };
        for (const u32 at : {info_amode, info_frame_bytes, info_sfreq, info_rate, info_lff}) ps_write32(base, bsi + at, kNone);
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name_);
            return fail();
        }
        const u8* p = base + au;
        const u32 sync = size >= 4 ? ps_ld32(p) : 0;
        if (sync != kSyncCore && sync != kSyncCore14 && sync != kSyncCoreLe && sync != kSyncCore14Le && sync != kSyncExss) {
            PS_LOG_ERROR("cellAdec", "{}: an access unit of {} bytes with no DTS sync ({:#010x})", name_, size, sync);
            return fail();
        }
        CoreHeader h;
        const bool core = read_core(p, size, h);
        bool exss = sync == kSyncExss;
        if (core) {
            ps_write32(base, bsi + info_amode, h.amode);
            ps_write32(base, bsi + info_frame_bytes, h.fsize + 1);
            ps_write32(base, bsi + info_sfreq, h.sfreq);
            ps_write32(base, bsi + info_rate, h.rate);
            ps_write32(base, bsi + info_lff, h.lff);
            const u32 next = (h.fsize + 1 + 3) & ~3u; // an extension substream follows aligned to 4 bytes
            exss = next + 4 <= size && ps_ld32(p + next) == kSyncExss;
        }
        ps_write32(base, bsi + info_exss, exss);
        pcm_.clear();
        if (const int e = ff_.decode(p, size, pcm_, kMaxSamples); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "{}: FFmpeg refused a frame ({}): the game sent invalid data", name_, e);
            return fail();
        }
        limit_channels(pcm_, kMaxChannels, name_);
        u32 stream_channels = pcm_.channels(), stream_mask = channel_mask(pcm_);
        if (core && h.amode < 16) {
            stream_channels = kAmodeChannels[h.amode] + (h.lff ? 1 : 0);
            stream_mask = h.amode < 10 ? kAmodeMask[h.amode] | (h.lff ? 0x8u : 0u) : 0;
        }
        const u32 bits = pcm_.bits ? pcm_.bits : core ? kPcmrBits[h.pcmr] : 0;
        apply_channel_number(pcm_, channel_number_);
        write_ginfo(base, bsi, pcm_, stream_channels, stream_mask, bits, core && h.rate < 29 ? kRates[h.rate] : 0);
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()), Format::F32);
        d.duration = ticks(pcm_.samples, pcm_.rate);
        return d;
    }

private:
    Kind kind_;
    std::string_view name_;
    FfDecoder ff_;
    Planar pcm_;
    u32 channel_number_ = 0;
    bool configured_ = false;
};

s32 check_core(const u8* p) { return check_layout_params(p, "DTS"); }
s32 check_hd(const u8* p) { return check_layout_params(p, "DTS-HD"); }
s32 check_hd_core(const u8* p) { return check_layout_params(p, "DTS-HD core"); }
s32 check_lbr(const u8* p) { return check_layout_params(p, "DTS Express"); }

} // namespace

// The cores' versions are not known (pspocket's word); the information block is ginfo's. Up to 8 channels of 4096
// samples.
const Spec kDts = {CELL_ADEC_TYPE_DTS, "DTS", lprm::size, ginfo::size, kMaxSamples * kMaxChannels * 4, 0x01000000,
    CELL_ADEC_ERROR_BUSY, check_core, [] { return std::unique_ptr<Codec>(std::make_unique<Dts>(Kind::Core, "DTS")); }};
const Spec kDtsHd = {CELL_ADEC_TYPE_DTSHD, "DTS-HD", lprm::size, ginfo::size, kMaxSamples * kMaxChannels * 4,
    0x01000000, CELL_ADEC_ERROR_BUSY, check_hd,
    [] { return std::unique_ptr<Codec>(std::make_unique<Dts>(Kind::Hd, "DTS-HD")); }};
const Spec kDtsHdCore = {CELL_ADEC_TYPE_DTSHDCORE, "DTS-HD core", lprm::size, ginfo::size,
    kMaxSamples * kMaxChannels * 4, 0x01000000, CELL_ADEC_ERROR_BUSY, check_hd_core,
    [] { return std::unique_ptr<Codec>(std::make_unique<Dts>(Kind::HdCore, "DTS-HD core")); }};
const Spec kDtsLbr = {CELL_ADEC_TYPE_DTSLBR, "DTS Express", lprm::size, ginfo::size, kMaxSamples * kMaxChannels * 4,
    0x01000000, CELL_ADEC_ERROR_BUSY, check_lbr,
    [] { return std::unique_ptr<Codec>(std::make_unique<Dts>(Kind::Lbr, "DTS Express")); }};

} // namespace pspocket::hle::adec::ffcore

#endif
