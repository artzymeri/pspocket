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
//   as libatxdec writes them (adec_atracx.cpp: floats held below 1, the 24-bit in a word's low three bytes). As
//   libat3dec (D-944): the sound is FFmpeg's with its sign turned over, and a sequence's first frame, and the first
//   after a refused unit, comes out empty (decoded: the frames after overlap it).
// - FFmpeg takes the configuration as a WAV file's 14 extra bytes (joint stereo or single channels) and frames of 96,
//   152 or 192 bytes a channel (ATRAC3's rates): StartSeq refuses (ARG) another size, as it does channels other than 1
//   or 2, joint stereo of one channel, or another word size.
// - A unit shorter than its frame, or one FFmpeg refuses: AT3_ERROR.
// - CellAdecAtrac3Info: the sequence's nch and isJoint, and a channel's bytes of a frame (nbytes / nch, libat3dec's).
// - PTS: the front end's step for ATRAC3 (correctPtsValueType 6, whatever the rate, as Sony's front end has it).
//
// ATRAC3 multichannel (CELL_ADEC_TYPE_ATRAC3MULTI, for cellAtrac3multidec's table 0xC20C6BD7, D-621), which
// cellAtracMulti decodes its files through (cellAtrac.cpp, D-535). RPCS3 has no structures for it; the parameters are
// what libatrac3multi writes (D-535, its handle compared byte for byte with Sony's module's): nch the output tracks (1
// to 8), isJoint 0 (multichannel ATRAC3 has no joint stereo), nbytes the stream's frame (all its channels), bw_pcm as
// ATRAC3's, and at +0x10 the stream's channels (pspocket's name for the 0x14 bytes: CellAdecParamAtrac3Multi). Before
// each frame libatrac3multi lays out, for each track, the block (nbytes / the stream's channels) of the channel the
// track plays, a silent block for a track that plays none, and gives that with the stream frame's size; so:
// - An access unit is nch blocks, each one channel's ATRAC3 frame (one sound unit), decoded by FFmpeg's ATRAC3 decoder
//   as a mono stream, a decoder for each track (each keeps its channel's overlap and gain control state). The blocks
//   may pass the size given (eight tracks of a two-channel stream): the core reads nch blocks from the unit's start.
// - The PCM: nch channels interleaved, 1024 samples each, in bw_pcm's format as ATRAC3's.
// - StartSeq refuses (ARG) a track count outside 1 to 8, joint stereo, a stream of more than 8 channels, a frame other
//   than 96, 152 or 192 bytes a channel, or another word size; a stream's channel count of 0 is taken as nch. Its
//   errors are libadec's own (ARG, BUSY, and FATAL for a unit it cannot decode), its own not being known.
// - The information block: nch, isJoint, nbytes and the stream's channels (pspocket's, 0x10 bytes).
// - PTS: the front end's step for it (correctPtsValueType 39).

#include <algorithm>
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

Format format_of(u32 bw)
{
    return bw == WORD_SZ_16BIT ? Format::S16 : bw == WORD_SZ_24BIT ? Format::S24 :
           bw == WORD_SZ_32BIT ? Format::S32 : Format::F32;
}

// FFmpeg's ATRAC3 decoder for frames of `nbytes` and `channels` channels, its configuration a WAV file's 14 extra
// bytes: 1, the samples per channel, the coding mode (joint stereo or single channels) twice, the frame factor 1, 0.
bool open_atrac3(FfDecoder& ff, std::string_view name, u32 channels, bool joint, u32 nbytes)
{
    std::array<u8, 14> extra{};
    const auto le16 = [&](u32 at, u32 v) {
        extra[at] = static_cast<u8>(v);
        extra[at + 1] = static_cast<u8>(v >> 8);
    };
    le16(0, 1);
    le16(2, 0x1000);
    le16(6, joint);
    le16(8, joint);
    le16(10, 1);
    return ff.open("atrac3", name, [&](AVCodecContext* av) {
        av_channel_layout_uninit(&av->ch_layout);
        av_channel_layout_default(&av->ch_layout, static_cast<int>(channels));
        av->sample_rate = 44100; // not in the parameters; FFmpeg's decoding does not depend on it
        av->block_align = static_cast<int>(nbytes);
        av->extradata = static_cast<u8*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(av->extradata, extra.data(), extra.size());
        av->extradata_size = static_cast<int>(extra.size());
    });
}

class Atrac3 final : public Codec {
public:
    s32 start(const u8* param) override
    {
        nch_ = ps_ld32(param + prm::nch);
        joint_ = ps_ld32(param + prm::is_joint) != 0;
        nbytes_ = ps_ld32(param + prm::nbytes);
        format_ = format_of(ps_ld32(param + prm::bw_pcm));
        configured_ = open_atrac3(ff_, "ATRAC3", nch_, joint_, nbytes_);
        held_ = false;
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_AT3_ERROR);
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = CORRECT_PTS_ATRAC3;
        ps_write32(base, bsi + prm::nch, nch_);
        ps_write32(base, bsi + prm::is_joint, joint_);
        ps_write32(base, bsi + prm::nbytes, nch_ ? nbytes_ / nch_ : nbytes_); // a channel's bytes, as libat3dec's
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "ATRAC3: a decode without a sequence started with valid parameters");
            d.error = error(CELL_ADEC_ERROR_AT3_ERROR);
            return d;
        }
        if (size < nbytes_) {
            PS_LOG_ERROR("cellAdec", "ATRAC3: an access unit of {} bytes for {}-byte frames", size, nbytes_);
            d.error = error(CELL_ADEC_ERROR_AT3_ERROR);
            held_ = false; // the frame after a refused one comes out empty, as a sequence's first (D-944)
            return d;
        }
        if (size > nbytes_)
            log_once("ATRAC3 longer", std::format("ATRAC3: an access unit of {} bytes holds more than its {}-byte frame: "
                "the frame is decoded, the rest left", size, nbytes_));
        pcm_.clear();
        if (const int e = ff_.decode(base + au, nbytes_, pcm_, 1024); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "ATRAC3: FFmpeg refused a frame ({}): the game sent invalid data", e);
            d.error = error(CELL_ADEC_ERROR_AT3_ERROR);
            held_ = false;
            return d;
        }
        // libat3dec's first frame of a sequence comes out empty (decoded: the frames after overlap it), and its sound
        // is FFmpeg's with the sign turned over.
        const bool first = !held_;
        held_ = true;
        if (first) return d;
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()), format_, -1.0f);
        return d;
    }

private:
    FfDecoder ff_;
    Planar pcm_;
    u32 nch_ = 2, nbytes_ = 0;
    bool joint_ = false, configured_ = false, held_ = false;
    Format format_ = Format::F32;
};

// ATRAC3 multichannel (D-621): CellAdecParamAtrac3's four words and the stream's channels; its information block the
// same five words but bw_pcm.
namespace mprm {
constexpr u32 nch = 0x0, is_joint = 0x4, nbytes = 0x8, bw_pcm = 0xC, channels = 0x10, size = 0x14;
}
constexpr u32 kMultiInfoSize = 0xC, kMaxTracks = 8; // libat3multidec's block: nch, isJoint, a channel's bytes

class Atrac3Multi final : public Codec {
public:
    s32 start(const u8* param) override
    {
        nch_ = ps_ld32(param + mprm::nch);
        nbytes_ = ps_ld32(param + mprm::nbytes);
        channels_ = ps_ld32(param + mprm::channels);
        if (!channels_) channels_ = nch_;
        configured_ = channels_ && nch_ <= kMaxTracks; // as check_multi passed them
        block_ = configured_ ? nbytes_ / channels_ : 0;
        format_ = format_of(ps_ld32(param + mprm::bw_pcm));
        for (u32 t = 0; t < nch_ && configured_; t++)
            configured_ = open_atrac3(tracks_[t], "ATRAC3 multichannel", 1, false, block_);
        held_ = false;
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_ARG);
    }

    // Each track's block, from the unit's start (D-535: they may pass the stream frame's size the unit is given with).
    u32 bytes_needed(u32 size) const override { return configured_ ? std::max(size, nch_ * block_) : size; }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = CORRECT_PTS_ATRAC3MULTI;
        ps_write32(base, bsi + mprm::nch, nch_);
        ps_write32(base, bsi + mprm::is_joint, 0);
        ps_write32(base, bsi + mprm::nbytes, block_); // a channel's bytes, as libat3multidec's (D-944)
        const auto fail = [&] {
            d.error = error(CELL_ADEC_ERROR_FATAL);
            held_ = false;
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "ATRAC3 multichannel: a decode without a sequence started with valid parameters");
            return fail();
        }
        if (!size) return fail(); // not guest memory (logged)
        Planar pcm;
        pcm.ch.resize(nch_);
        pcm.samples = 1024;
        for (u32 t = 0; t < nch_; t++) {
            Planar one;
            if (const int e = tracks_[t].decode(base + au + t * block_, block_, one, 1024); e < 0 || one.samples != 1024) {
                PS_LOG_ERROR("cellAdec", "ATRAC3 multichannel: FFmpeg refused track {}'s block ({}): the game sent "
                    "invalid data", t, e);
                return fail();
            }
            pcm.ch[t] = std::move(one.ch[0]);
        }
        // As libat3dec (D-944): the sequence's first frame, and the first after a refused unit, comes out empty, and the
        // sound has FFmpeg's sign turned over.
        const bool first = !held_;
        held_ = true;
        if (first) return d;
        d.size = write_pcm(base, out, pcm, straight(nch_), format_, -1.0f);
        return d;
    }

private:
    std::array<FfDecoder, kMaxTracks> tracks_;
    u32 nch_ = 0, nbytes_ = 0, channels_ = 0, block_ = 0;
    bool configured_ = false, held_ = false;
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

// cellAdecStartSeq for ATRAC3 multichannel (D-621): 1 to 8 tracks, no joint stereo, a stream of up to 8 channels (0:
// the tracks'), frames of 96, 152 or 192 bytes a channel, a word size ATRAC3_WordSize names.
s32 check_multi(const u8* p)
{
    const u32 nch = ps_ld32(p + mprm::nch), joint = ps_ld32(p + mprm::is_joint), nbytes = ps_ld32(p + mprm::nbytes),
              bw = ps_ld32(p + mprm::bw_pcm), channels = ps_ld32(p + mprm::channels) ? ps_ld32(p + mprm::channels) : nch;
    bool ok = nch >= 1 && nch <= kMaxTracks && joint == 0 && channels >= 1 && channels <= kMaxTracks;
    ok = ok && (nbytes == 96 * channels || nbytes == 152 * channels || nbytes == 192 * channels);
    ok = ok && (bw == WORD_SZ_16BIT || bw == WORD_SZ_24BIT || bw == WORD_SZ_32BIT || bw == WORD_SZ_FLOAT);
    if (ok) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "ATRAC3 multichannel: parameters nch {}, isJoint {}, nbytes {}, bw_pcm {:#x}, {} channels",
        nch, joint, nbytes, bw, channels);
    return error(CELL_ADEC_ERROR_ARG);
}

} // namespace

// libat3dec's version is not known (pspocket's word); the information block is CellAdecAtrac3Info's size. Two channels
// of a frame's 1024 samples, 32 bits each at most.
// libatrac3dec's answers (D-944): its version, getMemSize 0x3cb80, three PCM handles.
const Spec kAtrac3 = {CELL_ADEC_TYPE_ATRAC3, "ATRAC3", prm::size, kInfoSize, 1024 * 2 * 4, 0x01020000,
    CELL_ADEC_ERROR_AT3_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Atrac3>()); }, 0x3cb80, 3, false};
// ATRAC3 multichannel (D-621): eight tracks of 1024 samples, 32 bits each at most; libadec's error codes.
// libat3multidec's answers (D-944): its version, getMemSize 0x42400, three PCM handles, a 0xC-byte block.
const Spec kAtrac3Multi = {CELL_ADEC_TYPE_ATRAC3MULTI, "ATRAC3 multichannel", mprm::size, kMultiInfoSize,
    1024 * kMaxTracks * 4, 0x00010000, CELL_ADEC_ERROR_BUSY, check_multi,
    [] { return std::unique_ptr<Codec>(std::make_unique<Atrac3Multi>()); }, 0x42400, 3, false};

} // namespace pspocket::hle::adec::ffcore

#endif
