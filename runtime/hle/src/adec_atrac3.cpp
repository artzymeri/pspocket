// cellAdec's ATRAC3 core on FFmpeg (D-579): pspocket's own, where no firmware module gives libat3dec's (cellAtrac3dec,
// table 0x60487D65); cellAtrac plays ATRAC3 files through it (cellAtrac.cpp). The common part is adec_ffmpeg.cpp;
// FFmpeg's ATRAC3 decoder ("atrac3") decodes.
//
// From RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.h: the parameters (CellAdecParamAtrac3: nch, isJoint, nbytes, bw_pcm in
// ATRAC3_WordSize's 16, 24 and 32 bits or float), the information block (CellAdecAtrac3Info: nch, isJoint, nbytes),
// the error codes (CELL_ADEC_ERROR_AT3_*) and the front end's fixed PTS step (ADEC_CORRECT_PTS_VALUE_TYPE_ATRAC3).
// libatrac3plus fills the parameters from the file (cellAtrac.cpp: its channels, its joint stereo flag, its block size,
// float PCM). RPCS3 runs Sony's module, so what follows is pspocket's reading, to compare with libat3dec on the Mac:
// - One frame of nbytes (all channels) per access unit, 1024 samples per channel out, interleaved, in the four formats
//   as libatxdec writes them (adec_atracx.cpp: floats held below 1, the 24-bit in a word's low three bytes).
// - FFmpeg takes the configuration as a WAV file's 14 extra bytes (joint stereo or single channels) and frames of 96,
//   152 or 192 bytes a channel (ATRAC3's rates): StartSeq refuses (ARG) another size, as it does channels other than 1
//   or 2, joint stereo of one channel, or another word size.
// - A unit shorter than its frame, or one FFmpeg refuses: AT3_ERROR.
// - CellAdecAtrac3Info: the sequence's nch, isJoint and nbytes.
// - PTS: the front end's step for ATRAC3 (correctPtsValueType 6, whatever the rate, as Sony's front end has it).

#include <array>
#include <cstring>
#include <format>

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

enum : u32 { CELL_ADEC_ERROR_AT3_BUSY = 0x80612164, CELL_ADEC_ERROR_AT3_ERROR = 0x80612180 };
enum : u32 { WORD_SZ_16BIT = 2, WORD_SZ_24BIT = 3, WORD_SZ_32BIT = 4, WORD_SZ_FLOAT = 0x84 };

// CellAdecParamAtrac3, and CellAdecAtrac3Info (its first three words).
namespace prm {
constexpr u32 nch = 0x0, is_joint = 0x4, nbytes = 0x8, bw_pcm = 0xC, size = 0x10;
}
constexpr u32 kInfoSize = 0xC;

class Atrac3 final : public Codec {
public:
    s32 start(const u8* param) override
    {
        nch_ = ps_ld32(param + prm::nch);
        joint_ = ps_ld32(param + prm::is_joint) != 0;
        nbytes_ = ps_ld32(param + prm::nbytes);
        const u32 bw = ps_ld32(param + prm::bw_pcm);
        format_ = bw == WORD_SZ_16BIT ? Format::S16 : bw == WORD_SZ_24BIT ? Format::S24 :
                  bw == WORD_SZ_32BIT ? Format::S32 : Format::F32;
        // A WAV file's extra bytes: 1, the samples per channel, the coding mode twice, the frame factor 1, 0.
        std::array<u8, 14> extra{};
        const auto le16 = [&](u32 at, u32 v) {
            extra[at] = static_cast<u8>(v);
            extra[at + 1] = static_cast<u8>(v >> 8);
        };
        le16(0, 1);
        le16(2, 0x1000);
        le16(6, joint_);
        le16(8, joint_);
        le16(10, 1);
        configured_ = ff_.open("atrac3", "ATRAC3", [&](AVCodecContext* av) {
            av_channel_layout_uninit(&av->ch_layout);
            av_channel_layout_default(&av->ch_layout, static_cast<int>(nch_));
            av->sample_rate = 44100; // not in the parameters; FFmpeg's decoding does not depend on it
            av->block_align = static_cast<int>(nbytes_);
            av->extradata = static_cast<u8*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(av->extradata, extra.data(), extra.size());
            av->extradata_size = static_cast<int>(extra.size());
        });
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_AT3_ERROR);
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = CORRECT_PTS_ATRAC3;
        ps_write32(base, bsi + prm::nch, nch_);
        ps_write32(base, bsi + prm::is_joint, joint_);
        ps_write32(base, bsi + prm::nbytes, nbytes_);
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "ATRAC3: a decode without a sequence started with valid parameters");
            d.error = error(CELL_ADEC_ERROR_AT3_ERROR);
            return d;
        }
        if (size < nbytes_) {
            PS_LOG_ERROR("cellAdec", "ATRAC3: an access unit of {} bytes for {}-byte frames", size, nbytes_);
            d.error = error(CELL_ADEC_ERROR_AT3_ERROR);
            return d;
        }
        if (size > nbytes_)
            log_once("ATRAC3 longer", std::format("ATRAC3: an access unit of {} bytes holds more than its {}-byte frame: "
                "the frame is decoded, the rest left", size, nbytes_));
        pcm_.clear();
        if (const int e = ff_.decode(base + au, nbytes_, pcm_, 1024); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "ATRAC3: FFmpeg refused a frame ({}): the game sent invalid data", e);
            d.error = error(CELL_ADEC_ERROR_AT3_ERROR);
            return d;
        }
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()), format_);
        return d;
    }

private:
    FfDecoder ff_;
    Planar pcm_;
    u32 nch_ = 2, nbytes_ = 0;
    bool joint_ = false, configured_ = false;
    Format format_ = Format::F32;
};

// cellAdecStartSeq: one or two channels (joint stereo only with two), frames of 96, 152 or 192 bytes a channel, a word
// size ATRAC3_WordSize names.
s32 check(const u8* p)
{
    const u32 nch = ps_ld32(p + prm::nch), joint = ps_ld32(p + prm::is_joint), nbytes = ps_ld32(p + prm::nbytes),
              bw = ps_ld32(p + prm::bw_pcm);
    bool ok = (nch == 1 || nch == 2) && joint <= 1 && (!joint || nch == 2);
    ok = ok && (nbytes == 96 * nch || nbytes == 152 * nch || nbytes == 192 * nch);
    ok = ok && (bw == WORD_SZ_16BIT || bw == WORD_SZ_24BIT || bw == WORD_SZ_32BIT || bw == WORD_SZ_FLOAT);
    if (ok) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "ATRAC3: parameters nch {}, isJoint {}, nbytes {}, bw_pcm {:#x}", nch, joint, nbytes, bw);
    return error(CELL_ADEC_ERROR_ARG);
}

} // namespace

// libat3dec's version is not known (pspocket's word); the information block is CellAdecAtrac3Info's size. Two channels
// of a frame's 1024 samples, 32 bits each at most.
const Spec kAtrac3 = {CELL_ADEC_TYPE_ATRAC3, "ATRAC3", prm::size, kInfoSize, 1024 * 2 * 4, 0x01000000,
    CELL_ADEC_ERROR_AT3_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Atrac3>()); }};

} // namespace pspocket::hle::adec::ffcore

#endif
