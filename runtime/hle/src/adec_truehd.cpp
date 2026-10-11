// cellAdec's Dolby TrueHD core on FFmpeg (D-622): pspocket's own, where no firmware module gives the TrueHD core
// (cellTRHDdec, table 0xE88E381B). The common part is adec_ffmpeg.cpp; FFmpeg's TrueHD decoder ("truehd": Dolby's
// lossless coding on Meridian Lossless Packing) decodes.
//
// RPCS3 runs Sony's module and has nothing of TrueHD's but the cellAdec type (CELL_ADEC_TYPE_TRUEHD, 9), the module's
// names and NIDs, and that the core corrects PTS with a function of its own (ADEC_CORRECT_PTS_VALUE_TYPE_TRUEHD, 27:
// _SceAdecCorrectPtsValue_truehd) (rpcs3/Emu/Cell/Modules/cellAdec.cpp, cellAdec.h). The rest is pspocket's, from the
// format as FFmpeg reads it (libavcodec/mlpdec.c, mlp_parse.c/.h), to compare with Sony's core on the Mac:
// - The parameters: lprm (adec_ffmpeg.hpp: CellAdecParamMpmc's layout). channelNumber STEREO or MONO decodes the
//   stream's two-channel presentation (FFmpeg's "downmix" option: the lossless downmix the encoder coded; a stream
//   without one is decoded whole and downmixed as the AAC cores do), MONO then that pair averaged; any other layout the
//   whole stream (its largest presentation, up to eight channels).
// - An access unit holds one or more TrueHD access units (each starting with its length in 16-bit words), decoded in
//   order. Decoding starts at a major sync (0xF8726FBA): the units before one give no PCM, which is no error (logged
//   once). FFmpeg's decoder opens there, given the channel layout its own parser takes from the major sync (the
//   eight-channel presentation's, else the six-channel one's): it decodes one- and two-channel streams only so. At
//   most 8192 samples a channel per unit.
// - The PCM: big-endian floats (TrueHD's 24-bit samples exactly), interleaved in FFmpeg's channel order (WAV's).
// - The errors: libadec's own (ARG for parameters StartSeq refuses, BUSY, FATAL for a unit FFmpeg refuses, the reason
//   logged), TrueHD's not being known.
// - The information block: ginfo's head (adec_ffmpeg.hpp); the stream's channels those of the last major sync's
//   eight-channel presentation, else its six-channel one (FFmpeg's thd_chancount and thd_layout, mlp_parse.h);
//   bitsPerSample 0 (TrueHD codes none); bitRate the major sync's peak data rate. Then +0x28 the last major sync's
//   format_info word and +0x2C the TrueHD access units in this unit.
// - PTS: the core's own correction (AdecCore::correct_pts), the previous unit's PTS plus its length.

#include <algorithm>
#include <format>

#include "adec_ffmpeg.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

#if PSPOCKET_HAVE_FFMPEG

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace pspocket::hle::adec::ffcore {
namespace {

constexpr u32 kMajorSync = 0xF8726FBA, kMaxSamples = 8192, kMaxChannels = 8;
constexpr u32 info_format_info = ginfo::own + 0x0, info_access_units = ginfo::own + 0x4;

// A TrueHD channel assignment's channels and their mask (FFmpeg's thd_chancount and thd_layout, mlp_parse.h; the
// mask's bits past 31 dropped): L/R, C, LFE, Ls/Rs, Lvh/Rvh, Lc/Rc, Lrs/Rrs, Cs, Ts, Lsd/Rsd, Lw/Rw, Cvh, LFE2.
constexpr u8 kChanCount[13] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};
constexpr u64 kChanMask[13] = {0x3, 0x4, 0x8, 0x600, 0x5000, 0xC0, 0x30, 0x100, 0x800, 0x600000000ull, 0x180000000ull,
    0x2000, 0x800000000ull};

struct Assignment {
    u32 channels = 0, mask = 0; // the mask's 32 bits (WAVE_FORMAT_EXTENSIBLE's)
    u64 layout = 0;             // FFmpeg's whole mask
};
Assignment assignment(u32 map)
{
    Assignment a;
    for (u32 i = 0; i < 13; i++)
        if ((map >> i) & 1) {
            a.channels += kChanCount[i];
            a.layout |= kChanMask[i];
        }
    a.mask = static_cast<u32>(a.layout);
    return a;
}

class TrueHd final : public Codec {
public:
    // FFmpeg's decoder opens at the sequence's first major sync (open_at), with the layout FFmpeg's own parser gives it
    // from there (mlp_parser.c): one- and two-channel streams decode only so.
    s32 start(const u8* param) override
    {
        channel_number_ = ps_ld32(param + lprm::channel_number);
        ff_.close();
        stream_ = {};
        format_info_ = peak_rate_ = 0;
        configured_ = true;
        return lv2::CELL_OK;
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = CORRECT_PTS_TRUEHD;
        const auto fail = [&] {
            d.error = error(CELL_ADEC_ERROR_FATAL);
            ps_write32(base, bsi + ginfo::error_code, CELL_ADEC_ERROR_FATAL);
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "TrueHD: a decode without a sequence started with valid parameters");
            return fail();
        }
        if (size < 4) {
            PS_LOG_ERROR("cellAdec", "TrueHD: an access unit of {} bytes", size);
            return fail();
        }
        // The TrueHD access units: their count, and each major sync's stream description.
        u32 units = 0;
        for (u32 at = 0; at + 4 <= size; units++) {
            const u32 bytes = 2 * (ps_ld16(base + au + at) & 0xFFF);
            if (bytes < 4 || at + bytes > size) {
                log_once("TrueHD units", std::format("TrueHD: an access unit's TrueHD units do not end at its end ({} "
                    "bytes at {} of {}): FFmpeg decodes what it can", bytes, at, size));
                break;
            }
            if (bytes >= 4 + 28 && ps_ld32(base + au + at + 4) == kMajorSync) major_sync(base + au + at + 4);
            at += bytes;
        }
        ps_write32(base, bsi + info_format_info, format_info_);
        ps_write32(base, bsi + info_access_units, units);
        if (!ff_.is_open()) {
            if (!stream_.channels) {
                log_once("TrueHD sync", "TrueHD: access units before the stream's first major sync give no PCM");
                return d;
            }
            const bool two = channel_number_ == CELL_ADEC_CH_MONO || channel_number_ == CELL_ADEC_CH_STEREO;
            if (!ff_.open("truehd", "TrueHD", [&](AVCodecContext* av) {
                    av_channel_layout_uninit(&av->ch_layout);
                    av_channel_layout_from_mask(&av->ch_layout, stream_.layout);
                    if (two) av_opt_set(av, "downmix", "stereo", AV_OPT_SEARCH_CHILDREN);
                }))
                return fail();
        }
        pcm_.clear();
        if (const int e = ff_.decode(base + au, size, pcm_, kMaxSamples); e < 0) {
            PS_LOG_ERROR("cellAdec", "TrueHD: FFmpeg refused an access unit ({}): the game sent invalid data", e);
            return fail();
        }
        if (!pcm_.samples) return d; // the units FFmpeg passed over before a major sync
        limit_channels(pcm_, kMaxChannels, "TrueHD");
        apply_channel_number(pcm_, channel_number_);
        write_ginfo(base, bsi, pcm_, stream_.channels, stream_.mask, 0, peak_rate_);
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()), Format::F32);
        d.duration = ticks(pcm_.samples, pcm_.rate);
        return d;
    }

private:
    // A major sync at `p` (FFmpeg's ff_mlp_read_major_sync): format_info (the rate code in bits 31 to 28, the
    // six-channel presentation's assignment in bits 19 to 15, the eight-channel one's in bits 12 to 0), then 48 bits,
    // the variable rate flag and the 15-bit peak data rate (in sample rate / 16 bits per second).
    void major_sync(const u8* p)
    {
        format_info_ = ps_ld32(p + 4);
        const u32 code = format_info_ >> 28; // FFmpeg's mlp_samplerate: 48 or 44.1 kHz times 2^(code & 7), 15 none
        const u32 rate = code == 0xF ? 0 : (code & 8 ? 44100u : 48000u) << (code & 7);
        const Assignment six = assignment((format_info_ >> 15) & 0x1F), eight = assignment(format_info_ & 0x1FFF);
        stream_ = eight.channels ? eight : six; // as FFmpeg's parser takes it
        peak_rate_ = static_cast<u32>((u64{ps_ld16(p + 14) & 0x7FFFu} * rate + 8) >> 4);
    }

    FfDecoder ff_;
    Planar pcm_;
    u32 channel_number_ = 0, format_info_ = 0, peak_rate_ = 0;
    Assignment stream_;
    bool configured_ = false;
};

s32 check(const u8* p) { return check_layout_params(p, "TrueHD"); }

} // namespace

// The core's version is not known (pspocket's word); the information block is ginfo's.
const Spec kTrueHd = {CELL_ADEC_TYPE_TRUEHD, "TrueHD", lprm::size, ginfo::size, kMaxSamples * kMaxChannels * 4,
    0x01000000, CELL_ADEC_ERROR_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<TrueHd>()); }};

} // namespace pspocket::hle::adec::ffcore

#endif
