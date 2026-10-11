// cellAdec's MPEG-4 AAC cores on FFmpeg (D-578): pspocket's own, where no firmware module gives libm4aacdec's
// (cellM4AacDec, table 0xAB61278D), its two-channel form's (cellM4AacDec2ch, 0xE996C664) or its "2ch mod" form's
// (cellM4AacDec2chmod, 0xDBD26836). The common part is adec_ffmpeg.cpp; FFmpeg's float AAC decoder ("aac": LC, Main,
// LTP, and HE-AAC's SBR and PS) decodes.
//
// From RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.h: the parameters (CellAdecParamM4Aac: configNumber ADIF, ADTS or raw
// data blocks; for ADIF the program, for raw blocks the sampling frequency index and profile; enableDownmix), the
// information block (CellAdecM4AacInfo, 0x80 bytes: aligned to 16) and the error codes (CELL_ADEC_ERROR_M4AAC_*), and
// that the three cores correct PTS with functions of their own (_SceAdecCorrectPtsValue_M4Aac, _M4Aac2ch,
// _M4Aac2chmod: ADEC_CORRECT_PTS_VALUE_TYPE_M4AAC). MPEG-2 AAC (CELL_ADEC_TYPE_M2AAC, cellM2AACdec's table 0xAC2F0831,
// D-621) is the same core: RPCS3 has no structures for it, so its parameters and information block are taken as
// MPEG-4 AAC's (MPEG-2 AAC is MPEG-4's LC, Main and SSR profiles without SBR), its PTS type is its own
// (ADEC_CORRECT_PTS_VALUE_TYPE_M2AAC, _SceAdecCorrectPtsValue_Aac), and, its error codes not being known, it answers
// libadec's own: ARG for parameters StartSeq refuses, BUSY, and FATAL for any unit it cannot decode (the reason
// logged). Raw data blocks take Main or LC (MPEG-2 has no LTP). RPCS3 runs Sony's modules, so what follows is
// pspocket's reading, to compare on the Mac:
// - ADTS: one frame per access unit, its header read here (no sync ADTS_SYNCWORD_ERROR, a layer not 0
//   INVALID_LAYER_ID, an index past 8 kHz INVALID_SAMP_RATE_INDEX, SSR NOT_SUPPORTED_PROFILE, a frame longer than the
//   unit END_OF_BITSTREAM); MPEG-2's ID is decoded as MPEG-4's.
// - Raw data blocks: the sequence's sampling frequency index and profile (the object type less one, as ADTS codes it:
//   1 is LC) with the channel configuration of the first block's first element (a single channel element mono, a pair
//   stereo, a PCE its own): FFmpeg needs an AudioSpecificConfig, which these parameters do not complete. Mono with a
//   pair, and stereo with two single elements, decode as FFmpeg allows them; more channels need a PCE.
// - ADIF: the header at the first access unit's start (no "ADIF" INVALID_ADIF_HEADER_ID); its program config element
//   (the one whose element_instance_tag is programNumber, else the programNumber-th; none PROG_NUMBER_NOT_FOUND) is
//   FFmpeg's configuration; raw data blocks follow it and fill the later units.
// - A unit FFmpeg refuses: CELL_ADEC_ERROR_M4AAC_FATAL (FFmpeg does not say which syntax error).
// - The PCM: big-endian floats (the structures name no word size), interleaved in FFmpeg's channel order (front left,
//   front right, centre, LFE, then the back and side pairs: WAV's, as cellAudio's 8 channels), held below 1. Past two
//   channels with enableDownmix, or in the two-channel cores, a stereo downmix (each side: its front at 1, the centre and
//   its surrounds at 0.7071, the LFE left out, normalised so a full-scale input cannot clip). The "2ch mod" core is
//   taken as the two-channel one.
// - CellAdecM4AacInfo: samplingFreq the AAC rate coded (SBR doubles it: enableSBR 1, SBRUpsamplingFactor 2; else 0
//   and 1); the stream's channels (numberOfChannels, and its front, front centre, side, back and LFE counts, from the
//   layout FFmpeg decoded); isBsiValid 1 once a unit decoded; configNumber the sequence's; the ADTS header's fields or
//   the ADIF header's (copyright ID, original copy, home, bit stream type, bit rate, the program config elements less
//   one, the buffer fullness); a PCE's matrix mixdown index and pseudo surround flag.
// - PTS: this core's own correction (AdecCore::correct_pts), the previous frame's PTS plus its length.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <utility>

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

enum : u32 {
    CELL_ADEC_ERROR_M4AAC_FATAL = 0x80612401,
    CELL_ADEC_ERROR_M4AAC_ARG = 0x80612403,
    CELL_ADEC_ERROR_M4AAC_BUSY = 0x80612404,
    CELL_ADEC_ERROR_M4AAC_END_OF_BITSTREAM = 0x80612407,
    CELL_ADEC_ERROR_M4AAC_INVALID_LAYER_ID = 0x80612414,
    CELL_ADEC_ERROR_M4AAC_ADTS_SYNCWORD_ERROR = 0x80612415,
    CELL_ADEC_ERROR_M4AAC_INVALID_ADIF_HEADER_ID = 0x80612425,
    CELL_ADEC_ERROR_M4AAC_NOT_SUPPORTED_PROFILE = 0x80612426,
    CELL_ADEC_ERROR_M4AAC_PROG_NUMBER_NOT_FOUND = 0x80612427,
    CELL_ADEC_ERROR_M4AAC_INVALID_SAMP_RATE_INDEX = 0x80612428,
};
enum : u32 { ADIF_HEADER = 0, ADTS_HEADER = 1, RAW_DATA_BLOCK_ONLY = 2 };
enum : u32 { ID_SCE = 0, ID_CPE = 1, ID_LFE = 3, ID_DSE = 4, ID_PCE = 5, ID_FIL = 6, ID_END = 7 };

constexpr u32 kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

// CellAdecM4AacInfo.
namespace info {
constexpr u32 sampling_freq = 0x00, number_of_channels = 0x04, front = 0x08, front_mono = 0x0C, side = 0x10, back = 0x14,
              lfe = 0x18, enable_sbr = 0x1C, sbr_factor = 0x20, is_bsi_valid = 0x24, config_number = 0x28, bsi = 0x2C,
              matrix_mixdown_present = 0x68, mixdown_index = 0x6C, pseudo_surround = 0x70, size = 0x80;
// The ADIF header's fields (bsi.adif) and the ADTS header's (bsi.adts).
constexpr u32 adif_copyright_present = 0x2C, adif_copyright_id = 0x30, adif_original = 0x3C, adif_home = 0x40,
              adif_bitstream_type = 0x44, adif_bitrate = 0x48, adif_pce_count = 0x4C, adif_buffer_fullness = 0x50;
constexpr u32 adts_id = 0x2C, adts_layer = 0x30, adts_protection_absent = 0x34, adts_profile = 0x38, adts_sfi = 0x3C,
              adts_private = 0x40, adts_channel_config = 0x44, adts_original = 0x48, adts_home = 0x4C,
              adts_copyright_bit = 0x50, adts_copyright_start = 0x54, adts_frame_length = 0x58,
              adts_buffer_fullness = 0x5C, adts_raw_blocks = 0x60, adts_crc = 0x64;
} // namespace info

// A program config element (ISO/IEC 14496-3 4.4.1.1), as read: its fields, its elements, its comment.
struct Pce {
    u32 tag = 0, object_type = 0, sfi = 0;
    struct Element {
        bool is_cpe = false;
        u32 tag = 0;
    };
    std::vector<Element> front, side, back;
    std::vector<u32> lfe, assoc;
    std::vector<std::pair<bool, u32>> cc;
    bool mono_mixdown = false, stereo_mixdown = false, matrix_mixdown = false, pseudo_surround = false;
    u32 mono_mixdown_element = 0, stereo_mixdown_element = 0, matrix_mixdown_index = 0;
    std::vector<u8> comment;
};

// Reads a PCE at the reader's position (its element ID read already); `start` is the bit its byte alignment counts from.
Pce read_pce(Bits& b, u32 start)
{
    Pce p;
    p.tag = b.get(4);
    p.object_type = b.get(2);
    p.sfi = b.get(4);
    const u32 nf = b.get(4), ns = b.get(4), nb = b.get(4), nl = b.get(2), na = b.get(3), nc = b.get(4);
    if ((p.mono_mixdown = b.flag())) p.mono_mixdown_element = b.get(4);
    if ((p.stereo_mixdown = b.flag())) p.stereo_mixdown_element = b.get(4);
    if ((p.matrix_mixdown = b.flag())) {
        p.matrix_mixdown_index = b.get(2);
        p.pseudo_surround = b.flag();
    }
    const auto elements = [&](u32 n, std::vector<Pce::Element>& v) {
        for (u32 i = 0; i < n; i++) {
            Pce::Element e;
            e.is_cpe = b.flag();
            e.tag = b.get(4);
            v.push_back(e);
        }
    };
    elements(nf, p.front);
    elements(ns, p.side);
    elements(nb, p.back);
    for (u32 i = 0; i < nl; i++) p.lfe.push_back(b.get(4));
    for (u32 i = 0; i < na; i++) p.assoc.push_back(b.get(4));
    for (u32 i = 0; i < nc; i++) {
        const bool ind = b.flag();
        p.cc.emplace_back(ind, b.get(4));
    }
    b.skip((8 - (b.pos() - start) % 8) % 8); // byte_alignment
    const u32 comment = b.get(8);
    for (u32 i = 0; i < comment; i++) p.comment.push_back(static_cast<u8>(b.get(8)));
    return p;
}

// Bits written most significant first.
struct BitOut {
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

// An AudioSpecificConfig for FFmpeg: the object type, the rate, the channel configuration (0: `pce` follows, written
// with its byte alignment counted from the config's start, as FFmpeg reads it).
std::vector<u8> audio_specific_config(u32 object_type, u32 sfi, u32 channel_config, const Pce* pce)
{
    BitOut o;
    o.put(object_type, 5);
    o.put(sfi, 4);
    o.put(channel_config, 4);
    o.put(0, 3); // frameLengthFlag, dependsOnCoreCoder, extensionFlag
    if (channel_config == 0 && pce) {
        o.put(pce->tag, 4);
        o.put(pce->object_type, 2);
        o.put(pce->sfi, 4);
        o.put(static_cast<u32>(pce->front.size()), 4);
        o.put(static_cast<u32>(pce->side.size()), 4);
        o.put(static_cast<u32>(pce->back.size()), 4);
        o.put(static_cast<u32>(pce->lfe.size()), 2);
        o.put(static_cast<u32>(pce->assoc.size()), 3);
        o.put(static_cast<u32>(pce->cc.size()), 4);
        o.put(pce->mono_mixdown, 1);
        if (pce->mono_mixdown) o.put(pce->mono_mixdown_element, 4);
        o.put(pce->stereo_mixdown, 1);
        if (pce->stereo_mixdown) o.put(pce->stereo_mixdown_element, 4);
        o.put(pce->matrix_mixdown, 1);
        if (pce->matrix_mixdown) {
            o.put(pce->matrix_mixdown_index, 2);
            o.put(pce->pseudo_surround, 1);
        }
        for (const auto* list : {&pce->front, &pce->side, &pce->back})
            for (const Pce::Element& e : *list) {
                o.put(e.is_cpe, 1);
                o.put(e.tag, 4);
            }
        for (const u32 t : pce->lfe) o.put(t, 4);
        for (const u32 t : pce->assoc) o.put(t, 4);
        for (const auto& [ind, t] : pce->cc) {
            o.put(ind, 1);
            o.put(t, 4);
        }
        o.put(0, (8 - o.bit % 8) % 8);
        o.put(static_cast<u32>(pce->comment.size()), 8);
        for (const u8 c : pce->comment) o.put(c, 8);
    }
    return o.bytes;
}

// The first element of a raw data block that carries channels (data and fill elements skipped): its ID, and a PCE when
// it is one.
std::pair<u32, std::optional<Pce>> first_element(const u8* p, u32 size)
{
    Bits b(p, size);
    for (u32 guard = 0; guard < 64 && !b.overrun; guard++) {
        const u32 id = b.get(3);
        if (id == ID_DSE) {
            b.skip(4);
            const bool align = b.flag();
            u32 count = b.get(8);
            if (count == 255) count += b.get(8);
            if (align) b.skip((8 - b.pos() % 8) % 8);
            b.skip(8 * count);
            continue;
        }
        if (id == ID_FIL) {
            u32 count = b.get(4);
            if (count == 15) count += b.get(8) - 1;
            b.skip(8 * count);
            continue;
        }
        if (id == ID_PCE) return {id, read_pce(b, 0)};
        return {id, std::nullopt};
    }
    return {ID_END, std::nullopt};
}

class Aac final : public Codec {
public:
    // `mpeg2`: cellM2AACdec's core (libadec's error codes, its own PTS type).
    Aac(u32 max_channels, bool mpeg2)
        : max_channels_(max_channels), mpeg2_(mpeg2), name_(mpeg2 ? "MPEG-2 AAC" : "MPEG-4 AAC")
    {
    }

    s32 start(const u8* param) override
    {
        config_ = ps_ld32(param);
        program_ = ps_ld32(param + 4);
        sfi_ = ps_ld32(param + 4);
        profile_ = ps_ld32(param + 8);
        downmix_ = ps_ld32(param + 12) != 0;
        ff_.close();
        adif_read_ = false;
        configured_ = true;
        return lv2::CELL_OK;
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = mpeg2_ ? CORRECT_PTS_M2AAC : CORRECT_PTS_M4AAC;
        const auto fail = [&](u32 code) { // MPEG-2 AAC: libadec's FATAL for every unit refused (D-621)
            d.error = error(mpeg2_ ? CELL_ADEC_ERROR_FATAL : code);
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name_);
            return fail(CELL_ADEC_ERROR_M4AAC_FATAL);
        }
        ps_write32(base, bsi + info::config_number, config_);
        const u8* data = base + au;
        u32 bytes = size;
        u32 core_rate = 0;
        std::optional<Pce> pce;
        if (config_ == ADTS_HEADER) {
            if (size < 7) return fail(CELL_ADEC_ERROR_M4AAC_ADTS_SYNCWORD_ERROR);
            Bits b(data, size);
            if (b.get(12) != 0xFFF) return fail(CELL_ADEC_ERROR_M4AAC_ADTS_SYNCWORD_ERROR);
            const u32 id = b.get(1), layer = b.get(2), protection_absent = b.get(1), profile = b.get(2), sfi = b.get(4);
            const u32 private_bit = b.get(1), channel_config = b.get(3), original = b.get(1), home = b.get(1);
            const u32 copyright_bit = b.get(1), copyright_start = b.get(1), frame_length = b.get(13);
            const u32 fullness = b.get(11), raw_blocks = b.get(2), crc = protection_absent ? 0 : b.get(16);
            const std::pair<u32, u32> fields[] = {{info::adts_id, id}, {info::adts_layer, layer},
                {info::adts_protection_absent, protection_absent}, {info::adts_profile, profile}, {info::adts_sfi, sfi},
                {info::adts_private, private_bit}, {info::adts_channel_config, channel_config},
                {info::adts_original, original}, {info::adts_home, home}, {info::adts_copyright_bit, copyright_bit},
                {info::adts_copyright_start, copyright_start}, {info::adts_frame_length, frame_length},
                {info::adts_buffer_fullness, fullness}, {info::adts_raw_blocks, raw_blocks}, {info::adts_crc, crc}};
            for (const auto& [at, v] : fields) ps_write32(base, bsi + at, v);
            if (layer != 0) return fail(CELL_ADEC_ERROR_M4AAC_INVALID_LAYER_ID);
            if (sfi >= 12) return fail(CELL_ADEC_ERROR_M4AAC_INVALID_SAMP_RATE_INDEX);
            if (profile == 2) return fail(CELL_ADEC_ERROR_M4AAC_NOT_SUPPORTED_PROFILE);
            core_rate = kRates[sfi];
            d.duration = ticks(1024, core_rate); // a frame refused past here keeps its length
            if (frame_length < 7 || frame_length > size) {
                PS_LOG_ERROR("cellAdec", "{}: an ADTS frame of {} bytes in an access unit of {}", name_, frame_length, size);
                return fail(CELL_ADEC_ERROR_M4AAC_END_OF_BITSTREAM);
            }
            if (frame_length < size)
                log_once("AAC longer", std::format("{}: an access unit of {} bytes holds more than its {}-byte ADTS "
                    "frame: the frame is decoded, the rest left", name_, size, frame_length));
            if (raw_blocks)
                log_once("AAC raw blocks", std::format("{}: ADTS frames of several raw data blocks: FFmpeg decodes the "
                    "first", name_));
            bytes = frame_length;
            if (!ff_.is_open() && !ff_.open("aac", name_)) return fail(CELL_ADEC_ERROR_M4AAC_FATAL);
        } else if (config_ == ADIF_HEADER) {
            if (!adif_read_) {
                const s32 r = read_adif(base, au, size, bsi, bytes);
                if (r != lv2::CELL_OK) return fail(static_cast<u32>(r));
                data = base + au + (size - bytes);
            } else {
                std::memcpy(base + bsi + info::bsi, adif_info_.data(), adif_info_.size());
            }
            pce = adif_pce_;
            core_rate = adif_pce_ && adif_pce_->sfi < 13 ? kRates[adif_pce_->sfi] : 0;
        } else {
            core_rate = kRates[sfi_];
            if (!ff_.is_open()) {
                const auto [id, inband] = first_element(data, size);
                u32 channel_config = id == ID_CPE ? 2 : 1;
                if (id == ID_PCE && inband) {
                    channel_config = 0;
                    pce = inband;
                } else if (id != ID_SCE && id != ID_CPE) {
                    log_once("AAC raw first element", std::format("{}: a raw data block whose first element is {}: "
                        "decoded as mono", name_, id));
                }
                const std::vector<u8> asc = audio_specific_config(profile_ + 1, sfi_, channel_config, pce ? &*pce : nullptr);
                if (!open_with(asc)) return fail(CELL_ADEC_ERROR_M4AAC_FATAL);
            }
        }
        if (pce && pce->matrix_mixdown) {
            ps_write32(base, bsi + info::matrix_mixdown_present, 1);
            ps_write32(base, bsi + info::mixdown_index, pce->matrix_mixdown_index);
            ps_write32(base, bsi + info::pseudo_surround, pce->pseudo_surround);
        }
        if (!bytes) return fail(CELL_ADEC_ERROR_M4AAC_END_OF_BITSTREAM);
        pcm_.clear();
        if (const int e = ff_.decode(data, bytes, pcm_, 2048); e < 0 || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "{}: FFmpeg refused a frame ({}): the game sent invalid data", name_, e);
            return fail(CELL_ADEC_ERROR_M4AAC_FATAL);
        }
        // The stream's channels by position (FFmpeg's layout).
        u32 front = 0, front_mono = 0, side = 0, back = 0, lfe = 0;
        for (const int id : pcm_.ids) {
            switch (id) {
            case AV_CHAN_FRONT_CENTER: front++, front_mono++; break;
            case AV_CHAN_FRONT_LEFT:
            case AV_CHAN_FRONT_RIGHT:
            case AV_CHAN_FRONT_LEFT_OF_CENTER:
            case AV_CHAN_FRONT_RIGHT_OF_CENTER:
            case AV_CHAN_WIDE_LEFT:
            case AV_CHAN_WIDE_RIGHT: front++; break;
            case AV_CHAN_SIDE_LEFT:
            case AV_CHAN_SIDE_RIGHT: side++; break;
            case AV_CHAN_LOW_FREQUENCY:
            case AV_CHAN_LOW_FREQUENCY_2: lfe++; break;
            default: back++; break;
            }
        }
        if (!core_rate) core_rate = pcm_.rate;
        const bool sbr = pcm_.rate == 2 * core_rate;
        const std::pair<u32, u32> fields[] = {{info::sampling_freq, core_rate}, {info::number_of_channels, pcm_.channels()},
            {info::front, front}, {info::front_mono, front_mono}, {info::side, side}, {info::back, back},
            {info::lfe, lfe}, {info::enable_sbr, sbr ? 1u : 0u}, {info::sbr_factor, sbr ? 2u : 1u},
            {info::is_bsi_valid, 1}};
        for (const auto& [at, v] : fields) ps_write32(base, bsi + at, v);
        if (pcm_.channels() > 2 && (downmix_ || max_channels_ == 2)) stereo_downmix(pcm_);
        d.size = write_pcm(base, out, pcm_, straight(pcm_.channels()), Format::F32);
        d.duration = ticks(pcm_.samples, pcm_.rate);
        return d;
    }

private:
    bool open_with(const std::vector<u8>& asc)
    {
        return ff_.open("aac", name_, [&](AVCodecContext* av) {
            av->extradata = static_cast<u8*>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(av->extradata, asc.data(), asc.size());
            av->extradata_size = static_cast<int>(asc.size());
        });
    }

    // The ADIF header at the sequence's first unit: its fields into the information (kept for the later units), the
    // program's PCE as FFmpeg's configuration. `rest` gets the bytes past the header.
    s32 read_adif(u8* base, u32 au, u32 size, u32 bsi, u32& rest)
    {
        Bits b(base + au, size);
        if (b.get(32) != 0x41444946) return error(CELL_ADEC_ERROR_M4AAC_INVALID_ADIF_HEADER_ID); // "ADIF"
        const bool copyright = b.flag();
        ps_write32(base, bsi + info::adif_copyright_present, copyright);
        if (copyright)
            for (u32 i = 0; i < 9; i++) ps_write8(base, bsi + info::adif_copyright_id + i, static_cast<u8>(b.get(8)));
        ps_write32(base, bsi + info::adif_original, b.get(1));
        ps_write32(base, bsi + info::adif_home, b.get(1));
        const u32 type = b.get(1);
        ps_write32(base, bsi + info::adif_bitstream_type, type);
        ps_write32(base, bsi + info::adif_bitrate, b.get(23));
        const u32 count = b.get(4);
        ps_write32(base, bsi + info::adif_pce_count, count);
        std::vector<Pce> pces;
        for (u32 i = 0; i <= count && !b.overrun; i++) {
            if (type == 0) ps_write32(base, bsi + info::adif_buffer_fullness, b.get(20));
            pces.push_back(read_pce(b, 0));
        }
        if (b.overrun || b.pos() % 8) {
            PS_LOG_ERROR("cellAdec", "{}: an ADIF header past the access unit ({} bytes)", name_, size);
            return error(CELL_ADEC_ERROR_M4AAC_END_OF_BITSTREAM);
        }
        const Pce* chosen = nullptr;
        for (const Pce& p : pces)
            if (p.tag == program_) chosen = &p;
        if (!chosen && program_ < pces.size()) chosen = &pces[program_];
        if (!chosen) {
            PS_LOG_ERROR("cellAdec", "{}: program {} is not in the ADIF header's {}", name_, program_, pces.size());
            return error(CELL_ADEC_ERROR_M4AAC_PROG_NUMBER_NOT_FOUND);
        }
        if (chosen->object_type == 2) return error(CELL_ADEC_ERROR_M4AAC_NOT_SUPPORTED_PROFILE);
        if (chosen->sfi >= 12) return error(CELL_ADEC_ERROR_M4AAC_INVALID_SAMP_RATE_INDEX);
        if (!open_with(audio_specific_config(chosen->object_type + 1, chosen->sfi, 0, chosen)))
            return error(CELL_ADEC_ERROR_M4AAC_FATAL);
        adif_pce_ = *chosen;
        adif_info_.assign(base + bsi + info::bsi, base + bsi + info::matrix_mixdown_present);
        adif_read_ = true;
        rest = size - b.pos() / 8;
        return lv2::CELL_OK;
    }

    u32 max_channels_;
    bool mpeg2_;
    std::string_view name_;
    FfDecoder ff_;
    Planar pcm_;
    u32 config_ = ADTS_HEADER, program_ = 0, sfi_ = 3, profile_ = 1;
    bool downmix_ = false, configured_ = false, adif_read_ = false;
    std::optional<Pce> adif_pce_;
    std::vector<u8> adif_info_;
};

// cellAdecStartSeq: the configuration's type; for raw blocks a sampling frequency index and a profile FFmpeg decodes
// (Main, LC, LTP: not SSR; MPEG-2 AAC Main or LC); enableDownmix a flag.
s32 check_params(const u8* param, bool mpeg2)
{
    const u32 config = ps_ld32(param), a = ps_ld32(param + 4), profile = ps_ld32(param + 8), downmix = ps_ld32(param + 12);
    bool ok = config <= RAW_DATA_BLOCK_ONLY && downmix <= 1;
    if (ok && config == RAW_DATA_BLOCK_ONLY) ok = a < 12 && (mpeg2 ? profile <= 1 : profile <= 3 && profile != 2);
    if (ok) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "{}: parameters config {}, {:#x}, profile {}, downmix {}", mpeg2 ? "MPEG-2 AAC" : "MPEG-4 AAC",
        config, a, profile, downmix);
    return error(mpeg2 ? u32{CELL_ADEC_ERROR_ARG} : u32{CELL_ADEC_ERROR_M4AAC_ARG});
}
s32 check(const u8* param) { return check_params(param, false); }
s32 check_m2(const u8* param) { return check_params(param, true); }

} // namespace

// The cores' versions are not known (pspocket's word); the information block is CellAdecM4AacInfo's size. Up to 8
// channels of 2048 samples (SBR's) for the full core, 2 for the others.
const Spec kM4Aac = {CELL_ADEC_TYPE_M4AAC, "MPEG-4 AAC", 16, info::size, 2048 * 8 * 4, 0x01000000,
    CELL_ADEC_ERROR_M4AAC_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Aac>(8, false)); }};
const Spec kM4Aac2ch = {CELL_ADEC_TYPE_M4AAC_2CH, "MPEG-4 AAC 2ch", 16, info::size, 2048 * 2 * 4, 0x01000000,
    CELL_ADEC_ERROR_M4AAC_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Aac>(2, false)); }};
const Spec kM4Aac2chMod = {CELL_ADEC_TYPE_M4AAC_2CH_MOD, "MPEG-4 AAC 2ch mod", 16, info::size, 2048 * 2 * 4, 0x01000000,
    CELL_ADEC_ERROR_M4AAC_BUSY, check, [] { return std::unique_ptr<Codec>(std::make_unique<Aac>(2, false)); }};
// MPEG-2 AAC (D-621): MPEG-4 AAC's parameters and information block, libadec's error codes.
const Spec kM2Aac = {CELL_ADEC_TYPE_M2AAC, "MPEG-2 AAC", 16, info::size, 2048 * 8 * 4, 0x01000000, CELL_ADEC_ERROR_BUSY,
    check_m2, [] { return std::unique_ptr<Codec>(std::make_unique<Aac>(8, true)); }};

} // namespace pspocket::hle::adec::ffcore

#endif
