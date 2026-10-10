// cellAdec's MP3 core on FFmpeg (D-577): pspocket's own, where no firmware module gives libmp3dec's (cellMP3dec, its
// operation table 0x84276A23). The common part is adec_ffmpeg.cpp; FFmpeg's float MP3 decoder (mp3float) decodes.
//
// From RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.h: the parameters (CellAdecParamMP3: bw_pcm, 16-bit or float PCM), the
// information block (CellAdecMP3Info, 0x1C bytes) and the error codes (CELL_ADEC_ERROR_MP3_*); the PTS steps the front
// end adds for an access unit without a PTS (ADEC_CORRECT_PTS_VALUE_TYPE_MP3_48000Hz and the others: 1152 samples at
// 48, 44.1 and 32 kHz). RPCS3 runs Sony's module, so what follows is pspocket's reading of those names, to compare
// with libmp3dec on the Mac:
// - One frame per access unit: MPEG-1 or MPEG-2 (the lower rates) layer III. A header without the sync is LOST_SYNC;
//   layer I or II NOT_L3; MPEG-2.5 or the reserved version BAD_VERSION (the PTS steps name only MPEG-1's rates and
//   their halves); free format or bit rate 15 BAD_BITRATE; the reserved rate BAD_SFREQ, emphasis BAD_EMPHASIS; a
//   granule switched to block type 0 BAD_BLKTYPE; a unit shorter than its frame, or one FFmpeg refuses, MP3_ERROR. A
//   unit longer than its frame: the frame is decoded, the rest left (logged once).
// - The PCM: the stream's channels (1 or 2) interleaved, 16-bit or float as bw_pcm says, big-endian; floats held below
//   1, integers floored as libatxdec's (adec_atracx.cpp). Another bw_pcm is refused by cellAdecStartSeq (ARG).
// - CellAdecMP3Info: ui_header the frame's header word; ui_main_data_begin its side information's main_data_begin;
//   ui_main_data_now_size the bytes of main data in this frame (past the header, CRC and side information);
//   ui_main_data_remain_size what is left of the bit reservoir after it (main_data_begin + now_size, less the frame's
//   part2_3_length bits in whole bytes; 0 when that is negative); uc_crc the header's protection bit (MP3_CRC 0: a CRC
//   follows; MP3_NO_CRC 1); uc_mode, uc_mode_extension, uc_copyright, uc_original and uc_emphasis the header's fields;
//   uc_crc_error_flag 1 when the CRC over the header's second half and the side information does not match (the frame
//   is decoded anyway: the error codes have no CRC error); i_error_code the error the unit got, else 0.
// - PTS: correctPtsValueType 7, 8 or 9 for 48, 44.1 and 32 kHz (and 24, 22.05 and 16 kHz, whose 576-sample frames last
//   as long).

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
    CELL_ADEC_ERROR_MP3_BUSY = 0x80612764,
    CELL_ADEC_ERROR_MP3_ERROR = 0x80612781,
    CELL_ADEC_ERROR_MP3_LOST_SYNC = 0x80612782,
    CELL_ADEC_ERROR_MP3_NOT_L3 = 0x80612783,
    CELL_ADEC_ERROR_MP3_BAD_BITRATE = 0x80612784,
    CELL_ADEC_ERROR_MP3_BAD_SFREQ = 0x80612785,
    CELL_ADEC_ERROR_MP3_BAD_EMPHASIS = 0x80612786,
    CELL_ADEC_ERROR_MP3_BAD_BLKTYPE = 0x80612787,
    CELL_ADEC_ERROR_MP3_BAD_VERSION = 0x8061278C,
};
constexpr u32 MP3_WORD_SZ_16BIT = 3, MP3_WORD_SZ_FLOAT = 4;

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
    s32 start(const u8* param) override
    {
        bw_pcm_ = ps_ld32(param);
        configured_ = ff_.open("mp3float", "MP3");
        return configured_ ? lv2::CELL_OK : error(CELL_ADEC_ERROR_MP3_ERROR);
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        const auto fail = [&](u32 code) {
            d.error = error(code);
            ps_write32(base, bsi + info::error_code, code);
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "MP3: a decode without a sequence started with valid parameters");
            return fail(CELL_ADEC_ERROR_MP3_ERROR);
        }
        if (size < 4) return fail(CELL_ADEC_ERROR_MP3_LOST_SYNC);
        const MpegHeader h = parse_mpeg_header(ps_read32(base, au));
        if (!h.sync) return fail(CELL_ADEC_ERROR_MP3_LOST_SYNC);
        ps_write32(base, bsi + info::header, h.word);
        ps_write8(base, bsi + info::crc, h.crc ? 0 : 1);
        ps_write8(base, bsi + info::mode, static_cast<u8>(h.mode));
        ps_write8(base, bsi + info::mode_extension, static_cast<u8>(h.mode_extension));
        ps_write8(base, bsi + info::copyright, h.copyright);
        ps_write8(base, bsi + info::original, h.original);
        ps_write8(base, bsi + info::emphasis, static_cast<u8>(h.emphasis));
        if (h.version != 3 && h.version != 2) return fail(CELL_ADEC_ERROR_MP3_BAD_VERSION);
        if (h.layer != 3) return fail(CELL_ADEC_ERROR_MP3_NOT_L3);
        if (h.rate) {
            // The front end's steps for 1152 samples at 48, 44.1 and 32 kHz (576 at half those rates last as long);
            // a frame refused past its header keeps its length.
            const u32 rate = h.version == 3 ? h.rate : 2 * h.rate;
            d.pts_type = rate == 48000 ? CORRECT_PTS_MP3_48000 : rate == 44100 ? CORRECT_PTS_MP3_44100 : CORRECT_PTS_MP3_32000;
            d.duration = ticks(h.samples, h.rate);
        }
        if (!h.bitrate) return fail(CELL_ADEC_ERROR_MP3_BAD_BITRATE);
        if (!h.rate) return fail(CELL_ADEC_ERROR_MP3_BAD_SFREQ);
        if (h.emphasis == 2) return fail(CELL_ADEC_ERROR_MP3_BAD_EMPHASIS);
        const u32 crc_bytes = h.crc ? 2 : 0;
        const SideInfo si = side_info(base + au + 4 + crc_bytes, size - std::min(size, 4 + crc_bytes), h);
        if (size < h.frame_bytes || h.frame_bytes < 4 + crc_bytes + si.bytes) {
            PS_LOG_ERROR("cellAdec", "MP3: an access unit of {} bytes for a {}-byte frame", size, h.frame_bytes);
            return fail(CELL_ADEC_ERROR_MP3_ERROR);
        }
        if (size > h.frame_bytes)
            log_once("MP3 longer", std::format("MP3: an access unit of {} bytes holds more than its {}-byte frame: the "
                "frame is decoded, the rest left", size, h.frame_bytes));
        const u32 now = h.frame_bytes - 4 - crc_bytes - si.bytes;
        const s64 remain = s64{si.main_data_begin} + now - (si.part23_bits + 7) / 8;
        ps_write32(base, bsi + info::main_data_begin, si.main_data_begin);
        ps_write32(base, bsi + info::main_data_now_size, now);
        ps_write32(base, bsi + info::main_data_remain_size, remain > 0 ? static_cast<u32>(remain) : 0);
        if (h.crc) {
            u16 crc = mpeg_crc(base + au + 2, 16);
            crc = mpeg_crc(base + au + 6, 8 * si.bytes, crc);
            ps_write8(base, bsi + info::crc_error_flag, crc != ps_read16(base, au + 4));
        }
        if (si.bad_block) return fail(CELL_ADEC_ERROR_MP3_BAD_BLKTYPE);
        pcm_.clear();
        if (const int e = ff_.decode(base + au, h.frame_bytes, pcm_, h.samples); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "MP3: FFmpeg refused a frame ({}): the game sent invalid data", e);
            return fail(CELL_ADEC_ERROR_MP3_ERROR);
        }
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()),
            bw_pcm_ == MP3_WORD_SZ_16BIT ? Format::S16 : Format::F32);
        return d;
    }

private:
    FfDecoder ff_;
    Planar pcm_;
    u32 bw_pcm_ = MP3_WORD_SZ_FLOAT;
    bool configured_ = false;
};

// cellAdecStartSeq: bw_pcm 16-bit or float.
s32 check(const u8* param)
{
    const u32 bw = ps_ld32(param);
    if (bw == MP3_WORD_SZ_16BIT || bw == MP3_WORD_SZ_FLOAT) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "MP3: PCM word size {} (3: 16-bit, 4: float)", bw);
    return error(CELL_ADEC_ERROR_ARG);
}

std::unique_ptr<Codec> make() { return std::make_unique<Mp3>(); }

} // namespace

// libmp3dec's version is not known (pspocket's word); its information block is CellAdecMP3Info's size.
const Spec kMp3 = {CELL_ADEC_TYPE_MP3, "MP3", 4, info::size, 1152 * 2 * 4, 0x01000000, CELL_ADEC_ERROR_MP3_BUSY, check,
    make};

} // namespace pspocket::hle::adec::ffcore

#endif
