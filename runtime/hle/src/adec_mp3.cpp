// cellAdec's MP3 core on FFmpeg (D-577): pspocket's own, in place of libmp3dec's (cellMP3dec, its operation table
// 0x84276A23). The common part is adec_ffmpeg.cpp; FFmpeg's float MP3 decoder in its ADU form (mp3adufloat) decodes,
// the core keeping the bit reservoir itself.
//
// From RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.h: the parameters (CellAdecParamMP3: bw_pcm, 16-bit or float PCM), the
// information block (CellAdecMP3Info, 0x1C bytes) and the error codes (CELL_ADEC_ERROR_MP3_*); the PTS steps the front
// end adds for an access unit without a PTS (ADEC_CORRECT_PTS_VALUE_TYPE_MP3_48000Hz and the others: 1152 samples at
// 48, 44.1 and 32 kHz). What the core does with them is libmp3dec's, compared with it on the Mac (D-944):
// - One frame per access unit: MPEG-1, MPEG-2 or MPEG-2.5 layer III, the version as libmp3dec reads it (a clear
//   MPEG-2.5 bit is MPEG-2.5 whatever the bit beside it: the reserved version too). A header without the 11-bit sync
//   is LOST_SYNC; layer I or II NOT_L3; free format or bit rate 15 BAD_BITRATE; the reserved rate BAD_SFREQ, emphasis
//   BAD_EMPHASIS; a granule switched to block type 0 BAD_BLKTYPE; a frame FFmpeg refuses MP3_ERROR. libmp3dec's own
//   checks of the main data (HUFFMAN_NUM and the rest) are not made: FFmpeg decodes data libmp3dec would refuse. A
//   unit shorter than its frame is decoded with zeros past its end (libmp3dec: with whatever its buffer held there,
//   the same while the main data are inside the unit), one shorter than a header is LOST_SYNC; a unit longer than its
//   frame: the frame is decoded, the rest left (logged once).
// - The bit reservoir: each frame's main data, main_data_begin bytes back from the end of what the frames before left,
//   go to FFmpeg right after the frame's side information. A frame whose main data begin before what the reservoir
//   holds (a stream entered mid-way) makes no PCM and no error, its main data area kept for the frames after; after a
//   frame FFmpeg refuses the reservoir is empty and the decoder silent (its overlap and filter bank), as libmp3dec is
//   after a frame whose main data it refuses.
// - The PCM: the stream's channels (1 or 2) interleaved, 16-bit or float as bw_pcm says, big-endian; floats as decoded
//   (past 1 where the stream goes there), 16-bit samples rounded half away from zero and held in range. Another
//   bw_pcm is refused by cellAdecStartSeq (ARG).
// - CellAdecMP3Info, which libmp3dec keeps from frame to frame: for a frame decoded (or taken in without PCM)
//   ui_header the header's low 20 bits; ui_main_data_begin the side information's; ui_main_data_remain_size what the
//   reservoir held before the frame (left after the frame before's main data); ui_main_data_now_size 0; uc_crc the
//   header's protection bit (MP3_CRC 0: a CRC follows; MP3_NO_CRC 1); uc_mode, uc_mode_extension, uc_copyright,
//   uc_original and uc_emphasis the header's fields; uc_crc_error_flag 1 when the CRC over the header's second half
//   and the side information does not match (the frame is decoded anyway); i_error_code 0. A unit refused leaves the
//   block as the last frame made it (zeros before any) with i_error_code libmp3dec's number for the error
//   (mp3dec_number). The padding byte before i_error_code, where libmp3dec leaves a byte of its own after an error,
//   stays 0.
// - PTS: correctPtsValueType 7, 8 or 9 by the rate's index (48, 44.1 and 32 kHz and their halves and quarters: the
//   front end steps every frame as 1152 samples at the MPEG-1 rate).
//
// MP3 Surround (CELL_ADEC_TYPE_MP3S, for cellMP3Sdec's table 0x292CDF0A, D-621) is the same core. MP3 Surround is an
// MP3 stream whose ancillary data carries spatial parameters for 5.1; FFmpeg has no decoder for those, so its stereo
// MP3 (the downmix the encoder made, which every MP3 decoder plays) is decoded and the spatial data left (logged once).
// RPCS3 has no structures for it: its parameters and information block are taken as MP3's (CellAdecParamMP3,
// CellAdecMP3Info), its errors, not being known, are libadec's own (ARG for a word size StartSeq refuses, BUSY, FATAL
// for a unit it cannot decode, the reason logged and i_error_code FATAL), and its PTS is the core's own correction
// (ADEC_CORRECT_PTS_VALUE_TYPE_MP3S, _SceAdecCorrectPtsValue_Mp3s: the previous frame's PTS plus its length).

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <vector>

#include "adec_ffmpeg.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"

#if PSPOCKET_HAVE_FFMPEG

namespace pspocket::hle::adec::ffcore {
namespace {

enum : u32 {
    CELL_ADEC_ERROR_MP3_BUSY = 0x80612764,
    CELL_ADEC_ERROR_MP3_ERROR = 0x80612781,
    CELL_ADEC_ERROR_MP3_LOST_SYNC = 0x80612782,
    CELL_ADEC_ERROR_MP3_NOT_L3 = 0x80612783,
    CELL_ADEC_ERROR_MP3_BAD_BITRATE = 0x80612784,
    CELL_ADEC_ERROR_MP3_BAD_SFREQ = 0x80612785,
    CELL_ADEC_ERROR_MP3_BAD_EMPHASIS = 0x80612786,
    CELL_ADEC_ERROR_MP3_BAD_BLKTYPE = 0x80612787,
};
constexpr u32 MP3_WORD_SZ_16BIT = 3, MP3_WORD_SZ_FLOAT = 4;

// libmp3dec's own number for an error, which its information block holds: its table (libmp3dec.sprx, 0x4998) pairs
// -1 to -7 with CELL_ADEC_ERROR_MP3_ERROR to BAD_BLKTYPE, -10 to -12 with BAD_VERSION to BAD_MODE_EXT and -15 to -23
// with HUFFMAN_NUM to PCMOUT, of which pspocket's core gives the first seven (D-944). MP3 Surround's libadec codes are
// written as they are.
u32 mp3dec_number(u32 code)
{
    if (code >= CELL_ADEC_ERROR_MP3_ERROR && code <= CELL_ADEC_ERROR_MP3_BAD_BLKTYPE)
        return static_cast<u32>(-static_cast<s32>(code - CELL_ADEC_ERROR_MP3_ERROR + 1));
    return code;
}

// CellAdecMP3Info.
namespace info {
constexpr u32 header = 0x00, main_data_begin = 0x04, main_data_remain_size = 0x08, main_data_now_size = 0x0C,
              crc = 0x10, mode = 0x11, mode_extension = 0x12, copyright = 0x13, original = 0x14, emphasis = 0x15,
              crc_error_flag = 0x16, error_code = 0x18, size = 0x1C;
}

// Layer III's side information (ISO/IEC 11172-3 2.4.1.7; 13818-3 2.4.1.7 for the lower rates).
struct SideInfo {
    u32 main_data_begin = 0;
    u32 bytes = 0;       // its size
    u32 part23_bits = 0; // the frame's main data, every granule and channel
    bool bad_block = false;
};

SideInfo side_info(const u8* p, u32 size, const MpegHeader& h)
{
    const bool lsf = h.version != 3;
    const u32 nch = h.channels();
    SideInfo s;
    s.bytes = lsf ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
    Bits b(p, size);
    s.main_data_begin = b.get(lsf ? 8 : 9);
    b.skip(lsf ? (nch == 1 ? 1 : 2) : (nch == 1 ? 5 : 3)); // private bits
    if (!lsf) b.skip(4 * nch);                             // scfsi
    for (u32 gr = 0; gr < (lsf ? 1u : 2u); gr++) {
        for (u32 ch = 0; ch < nch; ch++) {
            s.part23_bits += b.get(12);
            b.skip(9 + 8 + (lsf ? 9 : 4)); // big_values, global_gain, scalefac_compress
            if (b.flag()) {                // window_switching_flag
                if (b.get(2) == 0) s.bad_block = true;
                b.skip(1 + 2 * 5 + 3 * 3); // mixed_block_flag, table_select, subblock_gain
            } else {
                b.skip(3 * 5 + 4 + 3); // table_select, region0_count, region1_count
            }
            b.skip(lsf ? 2 : 3); // preflag (MPEG-1), scalefac_scale, count1table_select
        }
    }
    return s;
}

class Mp3 final : public Codec {
public:
    // `surround`: cellMP3Sdec's core (D-621): MP3 Surround's stereo, libadec's error codes, its own PTS correction.
    explicit Mp3(bool surround) : surround_(surround), name_(surround ? "MP3 Surround" : "MP3") {}

    s32 start(const u8* param) override
    {
        bw_pcm_ = ps_ld32(param);
        info_.fill(0);
        reservoir_.clear();
        if (surround_)
            log_once("MP3 Surround", "MP3 Surround: the spatial data in its ancillary bytes is not decoded: its stereo "
                "MP3, the encoder's downmix, is");
        configured_ = ff_.open("mp3adufloat", name_);
        return configured_ ? lv2::CELL_OK : error(surround_ ? u32{CELL_ADEC_ERROR_ARG} : u32{CELL_ADEC_ERROR_MP3_ERROR});
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        if (surround_) d.pts_type = CORRECT_PTS_MP3S;
        // A unit refused leaves the information block as the last frame made it, with the error (D-944).
        const auto fail = [&](u32 code) { // MP3 Surround: libadec's FATAL for every unit refused (D-621)
            const u32 c = surround_ ? CELL_ADEC_ERROR_FATAL : code;
            d.error = error(c);
            ps_st32(info_.data() + info::error_code, mp3dec_number(c));
            std::memcpy(base + bsi, info_.data(), info_.size());
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name_);
            return fail(CELL_ADEC_ERROR_MP3_ERROR);
        }
        if (size < 4) return fail(CELL_ADEC_ERROR_MP3_LOST_SYNC);
        const u32 word = ps_read32(base, au);
        // libmp3dec takes a header whose MPEG-2.5 bit is clear as MPEG-2.5 whatever the version bit beside it (the
        // reserved version too, which FFmpeg refuses: it gets MPEG-2.5's).
        const u32 header = word & (1u << 20) ? word : word & ~(1u << 19);
        const MpegHeader h = parse_mpeg_header(header);
        if (!h.sync) return fail(CELL_ADEC_ERROR_MP3_LOST_SYNC);
        if (h.layer != 3) return fail(CELL_ADEC_ERROR_MP3_NOT_L3);
        if (h.rate) {
            // The front end's steps for 1152 samples at 48, 44.1 and 32 kHz, by the rate's index whatever the version
            // (MPEG-2.5's 576 samples at 12 kHz step as 24 ms too); a frame refused past its header keeps its length.
            // MP3 Surround corrects its own.
            if (!surround_)
                d.pts_type = h.rate_index == 1 ? CORRECT_PTS_MP3_48000 : h.rate_index == 0 ? CORRECT_PTS_MP3_44100 :
                                                                                            CORRECT_PTS_MP3_32000;
            d.duration = ticks(h.samples, h.rate);
        }
        if (!h.bitrate) return fail(CELL_ADEC_ERROR_MP3_BAD_BITRATE);
        if (!h.rate) return fail(CELL_ADEC_ERROR_MP3_BAD_SFREQ);
        if (h.emphasis == 2) return fail(CELL_ADEC_ERROR_MP3_BAD_EMPHASIS);
        // The frame's bytes, zeros past the end of a unit shorter than its frame.
        frame_.assign(h.frame_bytes, 0);
        std::memcpy(frame_.data(), base + au, std::min(size, h.frame_bytes));
        if (size > h.frame_bytes)
            log_once(std::format("{} longer", name_), std::format("{}: an access unit of {} bytes holds more than its "
                "{}-byte frame: the frame is decoded, the rest left", name_, size, h.frame_bytes));
        const u32 crc_bytes = h.crc ? 2 : 0;
        const SideInfo si = side_info(frame_.data() + 4 + crc_bytes, h.frame_bytes - 4 - crc_bytes, h);
        const u32 side_end = 4 + crc_bytes + si.bytes;
        if (h.frame_bytes < side_end) {
            PS_LOG_ERROR("cellAdec", "{}: a {}-byte frame without room for its side information", name_, h.frame_bytes);
            return fail(CELL_ADEC_ERROR_MP3_ERROR);
        }
        if (si.bad_block) return fail(CELL_ADEC_ERROR_MP3_BAD_BLKTYPE);
        bool crc_error = false;
        if (h.crc) {
            u16 crc = mpeg_crc(frame_.data() + 2, 16);
            crc = mpeg_crc(frame_.data() + 6, 8 * si.bytes, crc);
            crc_error = crc != ps_ld16(frame_.data() + 4);
        }
        // The frame as libmp3dec reports one it decoded or took in: the reservoir it found (what was left after the
        // frame before's main data), now_size 0.
        const auto report = [&] {
            info_.fill(0);
            u8* const i = info_.data();
            ps_st32(i + info::header, word & 0xFFFFF);
            ps_st32(i + info::main_data_begin, si.main_data_begin);
            ps_st32(i + info::main_data_remain_size, static_cast<u32>(reservoir_.size()));
            ps_st32(i + info::main_data_now_size, 0);
            i[info::crc] = h.crc ? 0 : 1;
            i[info::mode] = static_cast<u8>(h.mode);
            i[info::mode_extension] = static_cast<u8>(h.mode_extension);
            i[info::copyright] = h.copyright;
            i[info::original] = h.original;
            i[info::emphasis] = static_cast<u8>(h.emphasis);
            i[info::crc_error_flag] = crc_error;
            std::memcpy(base + bsi, info_.data(), info_.size());
        };
        if (si.main_data_begin > reservoir_.size()) {
            // The main data begin before what the reservoir holds (a stream entered mid-way): libmp3dec makes no PCM
            // and reports no error, and keeps the frame's main data area for the frames after.
            report();
            reservoir_.insert(reservoir_.end(), frame_.begin() + side_end, frame_.end());
            return d;
        }
        // The main data: the reservoir's last main_data_begin bytes, then the frame's; past the frame's end, zeros.
        main_.assign(reservoir_.end() - si.main_data_begin, reservoir_.end());
        main_.insert(main_.end(), frame_.begin() + side_end, frame_.end());
        const usize used = (si.part23_bits + 7) / 8;
        // FFmpeg's ADU decoder takes them right after the side information, as one unit.
        adu_.assign(frame_.begin(), frame_.begin() + side_end);
        ps_st32(adu_.data(), header);
        adu_.insert(adu_.end(), main_.begin(), main_.begin() + static_cast<std::ptrdiff_t>(std::min(used, main_.size())));
        adu_.resize(side_end + used, 0);
        pcm_.clear();
        if (const int e = ff_.decode(adu_.data(), static_cast<u32>(adu_.size()), pcm_, h.samples);
            e < 0 || !pcm_.samples) {
            // libmp3dec starts afresh after a frame whose main data it could not decode: the reservoir empty, the
            // overlap and filter bank silent.
            PS_LOG_ERROR("cellAdec", "{}: FFmpeg refused a frame ({}): the game sent invalid data", name_, e);
            reservoir_.clear();
            ff_.flush();
            return fail(CELL_ADEC_ERROR_MP3_ERROR);
        }
        report();
        // What follows the frame's main data stays for the frames after.
        reservoir_.assign(main_.begin() + static_cast<std::ptrdiff_t>(std::min(used, main_.size())), main_.end());
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()),
            bw_pcm_ == MP3_WORD_SZ_16BIT ? Format::S16 : Format::F32, 1.0f, Rounding::Nearest);
        return d;
    }

private:
    bool surround_;
    std::string_view name_;
    FfDecoder ff_;
    Planar pcm_;
    u32 bw_pcm_ = MP3_WORD_SZ_FLOAT;
    bool configured_ = false;
    std::array<u8, info::size> info_{}; // the information block as libmp3dec keeps it from frame to frame
    std::vector<u8> reservoir_;         // the bit reservoir: the bytes after the last frame's main data
    std::vector<u8> frame_, main_, adu_;
};

// cellAdecStartSeq: bw_pcm 16-bit or float.
s32 check(const u8* param)
{
    const u32 bw = ps_ld32(param);
    if (bw == MP3_WORD_SZ_16BIT || bw == MP3_WORD_SZ_FLOAT) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "MP3: PCM word size {} (3: 16-bit, 4: float)", bw);
    return error(CELL_ADEC_ERROR_ARG);
}

std::unique_ptr<Codec> make() { return std::make_unique<Mp3>(false); }
std::unique_ptr<Codec> make_surround() { return std::make_unique<Mp3>(true); }

} // namespace

// libmp3dec's version is not known (pspocket's word); its information block is CellAdecMP3Info's size.
// libmp3dec's answers (D-944): its version, getMemSize 0x27300, three PCM handles.
const Spec kMp3 = {CELL_ADEC_TYPE_MP3, "MP3", 4, info::size, 1152 * 2 * 4, 0x01030000, CELL_ADEC_ERROR_MP3_BUSY, check,
    make, 0x27300, 3, false};
// MP3 Surround (D-621): MP3's parameters and information block, libadec's error codes.
const Spec kMp3s = {CELL_ADEC_TYPE_MP3S, "MP3 Surround", 4, info::size, 1152 * 2 * 4, 0x01000000, CELL_ADEC_ERROR_BUSY,
    check, make_surround};

} // namespace pspocket::hle::adec::ffcore

#endif
