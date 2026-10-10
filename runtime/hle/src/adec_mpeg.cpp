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
#include <format>

#include "adec_ffmpeg.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

#if PSPOCKET_HAVE_FFMPEG

namespace pspocket::hle::adec::ffcore {
namespace {

enum : u32 {
    CELL_ADEC_ERROR_M2BC_FATAL = 0x80612B01,
    CELL_ADEC_ERROR_M2BC_ARG = 0x80612B03,
    CELL_ADEC_ERROR_M2BC_BUSY = 0x80612B04,
    CELL_ADEC_ERROR_M2BC_SYNCF = 0x80612B11,
    CELL_ADEC_ERROR_M2BC_LAYER = 0x80612B12,
    CELL_ADEC_ERROR_M2BC_BITRATE = 0x80612B13,
    CELL_ADEC_ERROR_M2BC_SAMPLEFREQ = 0x80612B14,
    CELL_ADEC_ERROR_M2BC_VERSION = 0x80612B15,
    CELL_ADEC_ERROR_M2BC_UNSUPPORT = 0x80612B17,
    CELL_ADEC_ERROR_M2BC_SAMPLE = 0x80612B35,
    CELL_ADEC_ERROR_M2BC_OPENBS = 0x80612B36,
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
        if (channel_number_ > CELL_ADEC_CH_STEREO)
            log_once("MPEG multichannel", std::format("MPEG audio: channel layout {} asked for: MPEG-2's multichannel "
                "extension is not decoded, its stereo base is", channel_number_));
        configured_ = ff_.open(layer_ == 1 ? "mp1float" : "mp2float", name());
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_M2BC_FATAL);
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = layer_ == 1 ? CORRECT_PTS_MPEG_L1 : CORRECT_PTS_MPEG_L2;
        const auto fail = [&](u32 code) {
            d.error = error(code);
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name());
            return fail(CELL_ADEC_ERROR_M2BC_FATAL);
        }
        if (size < 4) return fail(CELL_ADEC_ERROR_M2BC_SYNCF);
        const MpegHeader h = parse_mpeg_header(ps_read32(base, au));
        if (!h.sync || h.version == 0) return fail(CELL_ADEC_ERROR_M2BC_SYNCF);
        if (h.version == 1) return fail(CELL_ADEC_ERROR_M2BC_VERSION);
        if (h.layer != layer_) return fail(CELL_ADEC_ERROR_M2BC_LAYER);
        if (h.bitrate_index == 15) return fail(CELL_ADEC_ERROR_M2BC_BITRATE);
        if (h.bitrate_index == 0) return fail(CELL_ADEC_ERROR_M2BC_UNSUPPORT);
        if (!h.rate) return fail(CELL_ADEC_ERROR_M2BC_SAMPLEFREQ);
        d.duration = ticks(h.samples, h.rate); // a frame refused past here keeps its length
        const u32 out_channels = channel_number_ == CELL_ADEC_CH_MONO ? 1 : channel_number_ == CELL_ADEC_CH_STEREO ? 2 :
                                                                                                                    h.channels();
        ps_write32(base, bsi + info::channel_number, out_channels == 1 ? CELL_ADEC_CH_MONO : CELL_ADEC_CH_STEREO);
        ps_write32(base, bsi + info::sample_freq, h.rate_index);
        ps_write32(base, bsi + info::error_protection, h.crc);
        ps_write32(base, bsi + info::bitrate_index, h.bitrate_index);
        ps_write32(base, bsi + info::stereo_mode, h.mode);
        ps_write32(base, bsi + info::stereo_mode_extension, h.mode_extension);
        ps_write32(base, bsi + info::emphasis, h.emphasis);
        ps_write32(base, bsi + info::copyright, h.copyright);
        ps_write32(base, bsi + info::original, h.original);
        // No multichannel extension decoded: no surround, centre, mix or LFE; MCExtension 2CH.
        for (const u32 field : {info::surround_mode, info::center_mode, info::audio_mix, info::multi_codec_mode,
                 info::lfe_present})
            ps_write32(base, bsi + field, 0);
        ps_write32(base, bsi + info::output_frame_size, h.samples);
        ps_write32(base, bsi + info::channel_configuration,
            h.mode == 3 ? M2BC_CH_CONFIG_MONO : h.mode == 2 ? M2BC_CH_CONFIG_DUAL : M2BC_CH_CONFIG_L_R);
        if (size < h.frame_bytes) {
            PS_LOG_ERROR("cellAdec", "{}: an access unit of {} bytes for a {}-byte frame", name(), size, h.frame_bytes);
            return fail(CELL_ADEC_ERROR_M2BC_OPENBS);
        }
        if (size > h.frame_bytes)
            log_once(std::format("{} longer", name()), std::format("{}: an access unit of {} bytes holds more than its "
                "{}-byte frame: the frame is decoded, the rest left", name(), size, h.frame_bytes));
        pcm_.clear();
        if (const int e = ff_.decode(base + au, h.frame_bytes, pcm_, h.samples); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "{}: FFmpeg refused a frame ({}): the game sent invalid data", name(), e);
            return fail(CELL_ADEC_ERROR_M2BC_SAMPLE);
        }
        if (out_channels == 1 && pcm_.channels() == 2) {
            for (u32 i = 0; i < pcm_.samples; i++) pcm_.ch[0][i] = (pcm_.ch[0][i] + pcm_.ch[1][i]) * 0.5f;
            pcm_.ch.resize(1);
        }
        const std::vector<int> map = out_channels == 2 && pcm_.channels() == 1 ? std::vector<int>{0, 0} :
                                                                                 straight(pcm_.channels());
        d.size = write_pcm(base, out, pcm_, map, Format::F32);
        return d;
    }

private:
    std::string_view name() const { return layer_ == 1 ? "MPEG layer I" : "MPEG layer II"; }

    u32 layer_;
    FfDecoder ff_;
    Planar pcm_;
    u32 channel_number_ = 0;
    bool configured_ = false;
};

// cellAdecStartSeq: channelNumber a CellAdecChannel layout (or 0), downmix and lfeUpSample flags.
s32 check(const u8* param)
{
    const u32 channels = ps_ld32(param), downmix = ps_ld32(param + 4), lfe = ps_ld32(param + 8);
    if (channels <= CELL_ADEC_CH_3_4_LFE && downmix <= 1 && lfe <= 1) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "MPEG audio: parameters channelNumber {}, downmix {}, lfeUpSample {}", channels, downmix, lfe);
    return error(CELL_ADEC_ERROR_M2BC_ARG);
}

} // namespace

// The cores' versions are not known (pspocket's word); the information block is CellAdecMpmcInfo's size.
const Spec kMpegL1 = {CELL_ADEC_TYPE_MPEG_L1, "MPEG layer I", 12, info::size, 384 * 2 * 4, 0x01000000,
    CELL_ADEC_ERROR_M2BC_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Mpeg>(1)); }};
const Spec kMpegL2 = {CELL_ADEC_TYPE_MPEG_L2, "MPEG layer II", 12, info::size, 1152 * 2 * 4, 0x01000000,
    CELL_ADEC_ERROR_M2BC_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Mpeg>(2)); }};

} // namespace pspocket::hle::adec::ffcore

#endif
