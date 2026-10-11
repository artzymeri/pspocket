// cellAdec's Windows Media Audio cores on FFmpeg (D-622): pspocket's own, where no firmware module gives Sony's: WMA
// (CELL_ADEC_TYPE_WMA, cellWMAdec's table 0x88320B10: WMA 1 and 2, "WMA Standard"), WMA Pro (cellWMAPROdec's
// 0xA8BAC670) and WMA Lossless (cellWMALSLdec's 0x602AAB16). The common part is adec_ffmpeg.cpp; FFmpeg's decoders
// ("wmav1", "wmav2", "wmapro", "wmalossless") decode.
//
// RPCS3 runs Sony's modules and has nothing of WMA's but the cellAdec type of WMA (18), the modules' names and NIDs,
// and that each core corrects PTS with a function of its own (ADEC_CORRECT_PTS_VALUE_TYPE_WMA 30, WMAPRO 35, WMALSL 36:
// _SceAdecCorrectPtsValue_wma, _WmaPro, _WmaLsl) (rpcs3/Emu/Cell/Modules/cellAdec.cpp, cellAdec.h). libadec has no type
// for WMA Pro and WMA Lossless (RPCS3's RESERVED22 and RESERVED23, "never released"): cellAdec refuses those, and their
// cores serve whatever module drives them through their own tables, their contexts carrying 28 and 29 (adec.hpp). The
// rest is pspocket's, to compare with Sony's cores on the Mac:
// - The parameters (pspocket's CellAdecParamWma, 0x24 bytes of big-endian words): what an ASF file's stream properties
//   give a WMA decoder, its WAVEFORMATEX and codec data, as Sony's structures carry a WAV header's fields (ATRAC3's,
//   D-579): +0x00 formatTag (0x160 WMA 1, 0x161 WMA 2, 0x162 WMA Pro, 0x163 WMA Lossless), +0x04 channels, +0x08
//   samplesPerSec, +0x0C avgBytesPerSec, +0x10 blockAlign, +0x14 bitsPerSample (WMA Pro's and Lossless's
//   wValidBitsPerSample), +0x18 channelMask (theirs; 0: the channels' default), +0x1C encodeOptions (the codec data's
//   wEncodeOptions), +0x20 channelNumber (the output, as apply_channel_number: MONO, STEREO, else the stream's own).
//   StartSeq refuses (ARG) a format tag of another core, WMA 1 and 2 past 2 channels or 50 kHz or without a bit rate,
//   more than 8 channels, a mask of another channel count, WMA Lossless at other than 16 or 24 bits, no block, or a
//   layout past the enum.
// - FFmpeg gets them as a WAV file's codec data: WMA 1's 4 bytes and WMA 2's 10 with the options, WMA Pro's and
//   Lossless's 18 (the valid bits, the mask, the options).
// - An access unit is one or more packets of blockAlign bytes (an ASF payload's), each decoded in turn; bytes past the
//   last whole packet are left (logged once). A packet may finish no frame (WMA Pro's and Lossless's first, frames that
//   span packets): no PCM, which is no error. At most 32768 samples a channel per unit.
// - The PCM: big-endian floats (WMA Lossless's 16- and 24-bit samples exactly), interleaved in FFmpeg's channel order
//   (WAV's, the channel mask's).
// - The errors: libadec's own (ARG, BUSY, FATAL for a unit FFmpeg refuses or shorter than a packet), WMA's not known.
// - The information block: ginfo's head (adec_ffmpeg.hpp; the stream's channels and mask the parameters', bitsPerSample
//   WMA Pro's and Lossless's, bitRate avgBytesPerSec in bits), then +0x28 formatTag, +0x2C blockAlign, +0x30
//   encodeOptions, +0x34 the packets decoded in this unit.
// - PTS: the core's own correction (AdecCore::correct_pts), the previous unit's PTS plus its length.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <vector>

#include "adec_ffmpeg.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

#if PSPOCKET_HAVE_FFMPEG

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace pspocket::hle::adec::ffcore {
namespace {

enum : u32 { TAG_WMA1 = 0x160, TAG_WMA2 = 0x161, TAG_WMAPRO = 0x162, TAG_WMALSL = 0x163 };
enum class Kind : u8 { Standard, Pro, Lossless };

// CellAdecParamWma (pspocket's).
namespace prm {
constexpr u32 format_tag = 0x00, channels = 0x04, rate = 0x08, avg_bytes = 0x0C, block_align = 0x10, bits = 0x14,
              channel_mask = 0x18, encode_options = 0x1C, channel_number = 0x20, size = 0x24;
}
constexpr u32 info_format_tag = ginfo::own + 0x0, info_block_align = ginfo::own + 0x4, info_options = ginfo::own + 0x8,
              info_packets = ginfo::own + 0xC;
constexpr u32 kMaxSamples = 32768;

class Wma final : public Codec {
public:
    Wma(Kind kind, std::string_view name) : kind_(kind), name_(name) {}

    s32 start(const u8* param) override
    {
        tag_ = ps_ld32(param + prm::format_tag);
        channels_ = ps_ld32(param + prm::channels);
        block_ = ps_ld32(param + prm::block_align);
        bits_ = ps_ld32(param + prm::bits);
        mask_ = ps_ld32(param + prm::channel_mask);
        options_ = ps_ld32(param + prm::encode_options);
        channel_number_ = ps_ld32(param + prm::channel_number);
        bit_rate_ = 8 * ps_ld32(param + prm::avg_bytes);
        const u32 rate = ps_ld32(param + prm::rate);
        // The codec data as an ASF stream's WAVEFORMATEX carries it, little-endian.
        std::vector<u8> extra;
        const auto le = [&](u32 v, u32 bytes) {
            for (u32 i = 0; i < bytes; i++) extra.push_back(static_cast<u8>(v >> (8 * i)));
        };
        if (tag_ == TAG_WMA1) {
            le(0, 2);
            le(options_, 2);
        } else if (tag_ == TAG_WMA2) {
            le(0, 4); // dwSamplesPerBlock
            le(options_, 2);
            le(0, 4);
        } else {
            le(bits_, 2);
            le(mask_, 4);
            le(0, 8);
            le(options_, 2);
            le(0, 2);
        }
        const char* decoder = tag_ == TAG_WMA1 ? "wmav1" : tag_ == TAG_WMA2 ? "wmav2" : tag_ == TAG_WMAPRO ? "wmapro" :
                                                                                                              "wmalossless";
        configured_ = ff_.open(decoder, name_, [&](AVCodecContext* av) {
            av_channel_layout_uninit(&av->ch_layout);
            if (mask_) av_channel_layout_from_mask(&av->ch_layout, mask_);
            else av_channel_layout_default(&av->ch_layout, static_cast<int>(channels_));
            av->sample_rate = static_cast<int>(rate);
            av->bit_rate = bit_rate_;
            av->block_align = static_cast<int>(block_);
            av->extradata = static_cast<u8*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(av->extradata, extra.data(), extra.size());
            av->extradata_size = static_cast<int>(extra.size());
        });
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_ARG);
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = kind_ == Kind::Standard ? CORRECT_PTS_WMA : kind_ == Kind::Pro ? CORRECT_PTS_WMAPRO : CORRECT_PTS_WMALSL;
        const auto fail = [&] {
            d.error = error(CELL_ADEC_ERROR_FATAL);
            ps_write32(base, bsi + ginfo::error_code, CELL_ADEC_ERROR_FATAL);
            return d;
        };
        ps_write32(base, bsi + info_format_tag, tag_);
        ps_write32(base, bsi + info_block_align, block_);
        ps_write32(base, bsi + info_options, options_);
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name_);
            return fail();
        }
        if (size < block_) {
            PS_LOG_ERROR("cellAdec", "{}: an access unit of {} bytes for {}-byte packets", name_, size, block_);
            return fail();
        }
        if (size % block_)
            log_once(std::format("{} longer", name_), std::format("{}: an access unit of {} bytes is no whole number of "
                "{}-byte packets: the rest is left", name_, size, block_));
        pcm_.clear();
        const u32 packets = size / block_;
        for (u32 k = 0; k < packets; k++) {
            if (const int e = ff_.decode(base + au + k * block_, block_, pcm_, kMaxSamples); e < 0) {
                PS_LOG_ERROR("cellAdec", "{}: FFmpeg refused packet {} of an access unit ({}): the game sent invalid data",
                    name_, k, e);
                return fail();
            }
        }
        ps_write32(base, bsi + info_packets, packets);
        if (!pcm_.samples) return d; // no frame finished in these packets
        limit_channels(pcm_, 8, name_);
        const u32 stream_mask = mask_ ? mask_ : channel_mask(pcm_);
        apply_channel_number(pcm_, channel_number_);
        write_ginfo(base, bsi, pcm_, channels_, stream_mask, kind_ == Kind::Standard ? 0 : bits_,
            static_cast<u32>(bit_rate_));
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()), Format::F32);
        d.duration = ticks(pcm_.samples, pcm_.rate);
        return d;
    }

private:
    Kind kind_;
    std::string_view name_;
    FfDecoder ff_;
    Planar pcm_;
    u32 tag_ = 0, channels_ = 0, block_ = 0, bits_ = 0, mask_ = 0, options_ = 0, channel_number_ = 0;
    s64 bit_rate_ = 0;
    bool configured_ = false;
};

// cellAdecStartSeq: see the file's head.
s32 check_params(const u8* p, Kind kind, std::string_view name)
{
    const u32 tag = ps_ld32(p + prm::format_tag), channels = ps_ld32(p + prm::channels), rate = ps_ld32(p + prm::rate),
              avg = ps_ld32(p + prm::avg_bytes), block = ps_ld32(p + prm::block_align), bits = ps_ld32(p + prm::bits),
              mask = ps_ld32(p + prm::channel_mask), layout = ps_ld32(p + prm::channel_number);
    bool ok = channels >= 1 && channels <= 8 && rate >= 1 && rate <= 192000 && block >= 1 && block <= (1u << 21) &&
              layout <= CELL_ADEC_CH_3_4_LFE && (!mask || static_cast<u32>(std::popcount(mask)) == channels);
    switch (kind) {
    case Kind::Standard: ok = ok && (tag == TAG_WMA1 || tag == TAG_WMA2) && channels <= 2 && rate <= 50000 && avg; break;
    case Kind::Pro: ok = ok && tag == TAG_WMAPRO && bits >= 1 && bits <= 32; break;
    case Kind::Lossless: ok = ok && tag == TAG_WMALSL && (bits == 16 || bits == 24); break;
    }
    if (ok) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "{}: parameters format {:#x}, {} channels, {} Hz, {} bytes/s, block {}, {} bits, mask {:#x}, "
        "layout {}", name, tag, channels, rate, avg, block, bits, mask, layout);
    return error(CELL_ADEC_ERROR_ARG);
}

s32 check_standard(const u8* p) { return check_params(p, Kind::Standard, "WMA"); }
s32 check_pro(const u8* p) { return check_params(p, Kind::Pro, "WMA Pro"); }
s32 check_lossless(const u8* p) { return check_params(p, Kind::Lossless, "WMA Lossless"); }

} // namespace

// The cores' versions are not known (pspocket's word); the information block is ginfo's. WMA 1 and 2 have up to two
// channels, WMA Pro and Lossless up to eight; 32768 samples each a unit.
const Spec kWma = {CELL_ADEC_TYPE_WMA, "WMA", prm::size, ginfo::size, kMaxSamples * 2 * 4, 0x01000000,
    CELL_ADEC_ERROR_BUSY, check_standard,
    [] { return std::unique_ptr<Codec>(std::make_unique<Wma>(Kind::Standard, "WMA")); }};
const Spec kWmaPro = {CELL_ADEC_TYPE_WMAPRO_CORE, "WMA Pro", prm::size, ginfo::size, kMaxSamples * 8 * 4, 0x01000000,
    CELL_ADEC_ERROR_BUSY, check_pro, [] { return std::unique_ptr<Codec>(std::make_unique<Wma>(Kind::Pro, "WMA Pro")); }};
const Spec kWmaLsl = {CELL_ADEC_TYPE_WMALSL_CORE, "WMA Lossless", prm::size, ginfo::size, kMaxSamples * 8 * 4,
    0x01000000, CELL_ADEC_ERROR_BUSY, check_lossless,
    [] { return std::unique_ptr<Codec>(std::make_unique<Wma>(Kind::Lossless, "WMA Lossless")); }};

} // namespace pspocket::hle::adec::ffcore

#endif
