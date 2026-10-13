// cellAdec's MPEG audio layer I and II cores on FFmpeg (D-577): pspocket's own, where no firmware module gives
// libmpl1dec's (cellMPL1dec, table 0xBBC70551, layer I) or libm2bcdec's (cellM2BCdec, table 0x300AFE4D, layer II with
// MPEG-2's backward-compatible multichannel). The common part is adec_ffmpeg.cpp; FFmpeg's float decoders (mp1float,
// mp2float) decode.
//
// From RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.h: the parameters (CellAdecParamMpmc: channelNumber, downmix,
// lfeUpSample), the information block (CellAdecMpmcInfo, 16 words) with its enums (M2BC_*), the error codes
// (CELL_ADEC_ERROR_M2BC_*), and that both cores correct PTS with functions of their own (_SceAdecCorrectPtsValue_mpmcl1
// and _mpmc: ADEC_CORRECT_PTS_VALUE_TYPE_MPEG_L1 and _MPEG_L2). RPCS3 runs Sony's modules, so what follows is
// pspocket's reading of those names, to compare with Sony's cores on the Mac:
// - One frame per access unit, of the core's layer: MPEG-1, or MPEG-2 at its lower rates (ISO/IEC 13818-3). No 12-bit
//   sync (MPEG-2.5's 11 bits included) is SYNCF; the other layer LAYER; the reserved version VERSION; bit rate 15
//   BITRATE; free format UNSUPPORT (FFmpeg decodes it only from a stream); the reserved rate SAMPLEFREQ; a unit
//   shorter than its frame OPENBS; one FFmpeg refuses SAMPLE. A CRC mismatch is no error (FFmpeg logs it and decodes
//   the frame; CellAdecMpmcInfo has no field for it).
// - MPEG-2's multichannel extension (in a layer II frame's ancillary data) is not decoded: its stereo base is, which is
//   the multichannel's downmix by design, and the information's multichannel fields say 2 channels (logged once when a
//   layout past stereo is asked for).
// - The PCM: big-endian floats (the structures name no word size), interleaved, held below 1. channelNumber is read as
//   the CellAdecChannel layouts LPCM's parameters use (cellAdec's own naming): CELL_ADEC_CH_MONO gives one channel (a
//   stereo frame's two averaged), CELL_ADEC_CH_STEREO two (a mono frame's in both), anything else the stream's own.
//   lfeUpSample has nothing to upsample; downmix nothing past stereo.
// - CellAdecMpmcInfo: channelNumber the output's CellAdecChannel (MONO or STEREO); sampleFreq the header's index
//   (M2BC_SampleFrequency: 44.1, 48, 32 kHz, or their halves); errorProtection 1 when a CRC follows the header;
//   bitrateIndex, stereoMode, stereoModeExtention, emphasis, copyright and original the header's fields; surroundMode,
//   centerMode, audioMix, lfePresent 0 and multiCodecMode 2CH (no extension decoded); outputFrameSize the samples per
//   channel (384 or 1152); channelConfiguration M2BC's MONO, DUAL or L_R from the mode.
// - PTS: this core's own correction (AdecCore::correct_pts), the previous frame's PTS plus its length.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include "adec_ffmpeg.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

#if PSPOCKET_HAVE_FFMPEG

namespace pspocket::hle::adec::ffcore {
namespace {

// The errors: libm2bcdec's CELL_ADEC_ERROR_M2BC_* (0x80612Bxx), and libmpl1dec's, the same numbers from 0x80613100
// (D-944: its own range, which RPCS3 does not list).
constexpr u32 kM2bcBase = 0x80612B00, kMpl1Base = 0x80613100;
enum : u32 {
    ERR_FATAL = 0x01,
    ERR_ARG = 0x03,
    ERR_BUSY = 0x04,
    ERR_SYNCF = 0x11,      // no 12-bit sync (MPEG-2.5's and the reserved version's 11 bits too), a unit under 4 bytes
    ERR_CHANNELS_L1 = 0x12, // libmpl1dec: channelNumber past 2
    ERR_BITRATE = 0x13,    // bit rate 15, or free format
    ERR_SAMPLEFREQ = 0x14, // the reserved rate
    ERR_OPENBS_EX = 0x21,  // libm2bcdec: channelNumber past 2 (a multichannel extension asked for)
    ERR_SYNCF_EX = 0x22,   // the other layer's header, a unit shorter than its frame, a frame the decoder refuses
};
enum : u32 { M2BC_CH_CONFIG_MONO = 0, M2BC_CH_CONFIG_DUAL = 1, M2BC_CH_CONFIG_L_R = 2 };

// CellAdecMpmcInfo.
namespace info {
constexpr u32 channel_number = 0x00, sample_freq = 0x04, error_protection = 0x08, bitrate_index = 0x0C,
              stereo_mode = 0x10, stereo_mode_extension = 0x14, emphasis = 0x18, copyright = 0x1C, original = 0x20,
              surround_mode = 0x24, center_mode = 0x28, audio_mix = 0x2C, output_frame_size = 0x30,
              multi_codec_mode = 0x34, lfe_present = 0x38, channel_configuration = 0x3C, size = 0x40;
}

class Mpeg final : public Codec {
public:
    explicit Mpeg(u32 layer) : layer_(layer) {}

    s32 start(const u8* param) override
    {
        channel_number_ = ps_ld32(param);
        reset_info();
        configured_ = ff_.open(layer_ == 1 ? "mp1float" : "mp2float", name());
        return configured_ ? lv2::CELL_OK : error(code(ERR_FATAL));
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = layer_ == 1 ? CORRECT_PTS_MPEG_L1 : CORRECT_PTS_MPEG_L2;
        // A unit refused: an item of 1152 samples of silence in the channels of the information block, which stays as
        // the last frame decoded left it (D-944); libmpl1dec's gives out a layer I frame's 384 of them.
        const auto fail = [&](u32 number) {
            d.error = error(code(number));
            const u32 nch = std::clamp<u32>(ps_ld32(info_.data() + info::channel_number), 1, 2);
            pcm_.clear();
            pcm_.ch.assign(nch, std::vector<float>(1152, 0.0f));
            pcm_.samples = 1152;
            d.size = write_pcm(base, out, pcm_, straight(nch), Format::F32, 1.0f, Rounding::Nearest);
            if (layer_ == 1) d.copied = 384 * nch * 4;
            std::memcpy(base + bsi, info_.data(), info_.size());
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name());
            return fail(ERR_FATAL);
        }
        if (size < 4) return fail(ERR_SYNCF);
        const MpegHeader h = parse_mpeg_header(ps_read32(base, au));
        if (!h.sync || h.version == 0 || h.version == 1) return fail(ERR_SYNCF);
        if (h.layer != layer_) return fail(ERR_SYNCF_EX);
        if (h.bitrate_index == 15 || h.bitrate_index == 0) return fail(ERR_BITRATE);
        if (!h.rate) return fail(ERR_SAMPLEFREQ);
        d.duration = ticks(h.samples, h.rate); // a frame refused past here keeps its length
        // channelNumber 0 to 2 leave the stream's channels as they are; past 2 asks for a multichannel extension
        // pspocket does not decode, refused frame by frame as Sony's cores refuse it without one.
        if (channel_number_ > 2) {
            log_once("MPEG multichannel", std::format("{}: channelNumber {} (a multichannel output) asked for: refused "
                "as Sony's core refuses a stream without MPEG-2's multichannel extension", name(), channel_number_));
            return fail(layer_ == 1 ? ERR_CHANNELS_L1 : ERR_OPENBS_EX);
        }
        if (size < h.frame_bytes) {
            PS_LOG_ERROR("cellAdec", "{}: an access unit of {} bytes for a {}-byte frame", name(), size, h.frame_bytes);
            return fail(ERR_SYNCF_EX);
        }
        if (size > h.frame_bytes)
            log_once(std::format("{} longer", name()), std::format("{}: an access unit of {} bytes holds more than its "
                "{}-byte frame: the frame is decoded, the rest left", name(), size, h.frame_bytes));
        pcm_.clear();
        if (const int e = ff_.decode(base + au, h.frame_bytes, pcm_, h.samples); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "{}: FFmpeg refused a frame ({}): the game sent invalid data", name(), e);
            return fail(ERR_SYNCF_EX);
        }
        const u32 nch = pcm_.channels();
        const auto field = [&](u32 at, u32 v) { ps_st32(info_.data() + at, v); };
        field(info::channel_number, nch);
        field(info::sample_freq, h.rate_index);
        field(info::error_protection, h.crc);
        field(info::bitrate_index, h.bitrate_index);
        field(info::stereo_mode, h.mode);
        field(info::stereo_mode_extension, h.mode_extension);
        field(info::emphasis, h.emphasis);
        field(info::copyright, h.copyright);
        field(info::original, h.original);
        // No multichannel extension decoded: no surround, centre, mix or LFE; MCExtension 2CH.
        for (const u32 at : {info::surround_mode, info::center_mode, info::audio_mix, info::multi_codec_mode,
                 info::lfe_present})
            field(at, 0);
        field(info::output_frame_size, 1152 * nch * 4);
        field(info::channel_configuration,
            h.mode == 3 ? M2BC_CH_CONFIG_MONO : h.mode == 2 ? M2BC_CH_CONFIG_DUAL : M2BC_CH_CONFIG_L_R);
        std::memcpy(base + bsi, info_.data(), info_.size());
        d.size = write_pcm(base, out, pcm_, straight(nch), Format::F32, 1.0f, Rounding::Nearest);
        return d;
    }

private:
    std::string_view name() const { return layer_ == 1 ? "MPEG layer I" : "MPEG layer II"; }
    u32 code(u32 number) const { return (layer_ == 1 ? kMpl1Base : kM2bcBase) + number; }

    // The block before a sequence's first frame, as Sony's cores have it: stereo, 48 kHz, bit rate index 14, 1152
    // stereo samples, L_R.
    void reset_info()
    {
        info_.fill(0);
        ps_st32(info_.data() + info::channel_number, 2);
        ps_st32(info_.data() + info::sample_freq, 1);
        ps_st32(info_.data() + info::bitrate_index, 14);
        ps_st32(info_.data() + info::output_frame_size, 1152 * 2 * 4);
        ps_st32(info_.data() + info::channel_configuration, M2BC_CH_CONFIG_L_R);
    }

    u32 layer_;
    FfDecoder ff_;
    Planar pcm_;
    u32 channel_number_ = 0;
    std::array<u8, info::size> info_{};
    bool configured_ = false;
};

// cellAdecStartSeq, as Sony's cores check it (D-944): the downmix flag only (channelNumber any: past 2 the frames are
// refused; lfeUpSample any).
template <u32 Base>
s32 check(const u8* param)
{
    const u32 downmix = ps_ld32(param + 4);
    if (downmix <= 1) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "MPEG audio: downmix {}", downmix);
    return error(Base + ERR_ARG);
}

} // namespace

// The cores' versions are not known (pspocket's word); the information block is CellAdecMpmcInfo's size.
// The answers of Sony's cores (D-944): libmpl1dec's and libm2bcdec's versions, getMemSize 0x64000 and 0x3e400 (with the
// version written into the attributes too), four PCM handles.
const Spec kMpegL1 = {CELL_ADEC_TYPE_MPEG_L1, "MPEG layer I", 12, info::size, 1152 * 2 * 4, 0x01080000,
    kMpl1Base + ERR_BUSY, check<kMpl1Base>, [] { return std::unique_ptr<Codec>(std::make_unique<Mpeg>(1)); }, 0x64000, 4,
    true};
const Spec kMpegL2 = {CELL_ADEC_TYPE_MPEG_L2, "MPEG layer II", 12, info::size, 1152 * 2 * 4, 0x02040000,
    kM2bcBase + ERR_BUSY, check<kM2bcBase>, [] { return std::unique_ptr<Codec>(std::make_unique<Mpeg>(2)); }, 0x3e400, 4,
    true};

} // namespace pspocket::hle::adec::ffcore

#endif
