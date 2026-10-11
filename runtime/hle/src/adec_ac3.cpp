// cellAdec's AC-3 core on FFmpeg (D-579): pspocket's own, where no firmware module gives libac3dec's (cell_libac3dec,
// table 0xC58BB170). The common part is adec_ffmpeg.cpp; FFmpeg's float AC-3 decoder ("ac3") decodes every channel,
// and the downmix to the output the game asks for is done here.
//
// From RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.h: the parameters (CellAdecParamAc3, its byte-wide enums: word size,
// output mode, LFE, dynamic range compression, stereo, dual mono and karaoke modes, the PCM scale and six channel
// pointers), the information block (CellAdecBsiAc3, 0x44 bytes), the error codes (CELL_ADEC_ERROR_AC3_*), and the
// front end's fixed PTS step for AC-3 (ADEC_CORRECT_PTS_VALUE_TYPE_AC3, 1536 samples at 48 kHz). The parameters are
// those of Dolby's decoder: its channels are numbered L 0, C 1, R 2, Ls 3, Rs 4 and LFE 5 (RPCS3's AC3_InputChannel
// names them L, C, R, l, r and s: s is the LFE, the subwoofer). RPCS3 runs Sony's module, so what follows is
// pspocket's reading of the names, to compare with libac3dec on the Mac:
// - The output: numberOfChannels interleaved slots (1 to 6); decoded channel K goes to slot channelPointerK (-1: not
//   out), the slots no channel points at silent. The channels are those of outputMode (Dolby's 1/0 to 3/2, a mono
//   surround in Ls's place for 2/1 and 3/1), with the LFE when outLfeOn; 16-bit (wordSize 0) or float (1), big-endian,
//   times pcmScaleFactor. StartSeq refuses (AC3_PARAM) a field past its enum, a channel count past 6, a pointer past
//   the slots or two at one, a scale factor out of 0 to 1.
// - The downmix to outputMode, after ATSC A/52 7.8: the centre into the front pair at the centre mix level (cmixlev)
//   when the output has no centre; the surrounds into the front pair at the surround mix level (surmixlev; a mono
//   surround at 0.7071 of it to each side) when the output has none; a pair of surrounds to a mono surround at 0.7071
//   each, and a mono surround to a pair at 0.7071; the LFE never mixed. To 2/0 a Dolby Surround compatible Lt/Rt (the
//   centre at -3 dB to both, the surrounds at -3 dB, subtracted on the left and added on the right) or a stereo Lo/Ro as
//   stereoMode says, its auto taking the stream's preferred mode (A/52 annex D's dmixmod) and Lo/Ro when it names none
//   (FFmpeg's default); annex D's own mix levels for Lt/Rt and Lo/Ro when the stream has them. To 1/0 the Lo/Ro pair's
//   sum at 0.7071. Every output channel is then divided by its coefficients' sum when that passes 1, so a full-scale
//   input cannot clip (FFmpeg's normalisation of its stereo downmix). Dual mono (1+1): dualmonoMode stereo puts Ch1 left
//   and Ch2 right, left mono Ch1, right mono Ch2 and mixed mono their mean, a mono to the centre or, without one, at
//   0.7071 to each side.
// - Dynamic range: FFmpeg's drc_scale takes drcCutScaleFactor for the custom and line modes (one factor for cut and
//   boost: a boost factor of its own is logged once), heavy compression for RF remodulation. Dialogue normalisation is
//   not applied (as FFmpeg); karaoke streams' vocal modes are not reproduced (logged once).
// - One syncframe per access unit: no sync word, a reserved rate or frame size code, a bsid past 10 (E-AC-3 is
//   cellDDPdec's), a unit shorter than its frame, or one FFmpeg refuses: AC3_FRAME. A CRC mismatch is flagged, and the
//   frame concealed as FFmpeg conceals it by default (the previous frame's last block repeated), not an error.
// - CellAdecBsiAc3: codecType 0; versionInfo the core's version; totalCallNum the frames decoded in the sequence
//   (totalCallCarryRun 0); bitrateValue in kbit/s; pcmSize the PCM's bytes; esSizeBit the frame's bits; errorCode;
//   validity 1 for a decoded frame; channelValue the stream's channels with its LFE; fsIndex fscod;
//   outputQuantization wordSize; outputChannel numberOfChannels; monoFlag 1 for a 1/0 output; bsi the 64 bits from
//   bsid on; frmSizeCod and the BSI's fields (acmod, lfeon, cmixlev, surmixlev, dsurmod, copyright, original, bsmod,
//   bsid, and annex D's xbsi1e, dmixmod, xbsi2e, dsurexmod, dheadphonmod, adconvtyp); karaokeMode 1 for a karaoke
//   stream (bsmod 7 past 2/0); crcErrorFlag; execDmixType 0 without a downmix, 1 Lt/Rt, 2 any other.
// - PTS: the front end's step for AC-3 (correctPtsValueType 5).
//
// Dolby Digital Plus (CELL_ADEC_TYPE_EAC3, for cellDDPdec's table 0xF5E9C15C, D-621) is the same core on FFmpeg's
// E-AC-3 decoder ("eac3", which decodes AC-3 frames too, as a DD+ decoder must). RPCS3 has no structures for it, so its
// parameters and information block are taken as AC-3's (CellAdecParamAc3, CellAdecBsiAc3: Dolby's DD+ decoder has the
// AC-3 decoder's parameters), read and checked as above, and its errors, not being known, are libadec's own (ARG for
// parameters StartSeq refuses, BUSY, FATAL for a unit it cannot decode, the reason logged):
// - An access unit holds one or more syncframes (AC-3's, or E-AC-3's of 1, 2, 3 or 6 blocks, A/52 annex E). Program 1's
//   independent frames are decoded in order: an AC-3 frame, or E-AC-3's independent substream 0. Its dependent
//   substreams (7.1's extra channels) and the other programs' substreams are left out (logged once): the independent
//   substream carries the program's 5.1 as the encoder mixed it, as a 5.1 decoder plays it. At most 6144 samples a
//   channel (four 1536-sample frames) per unit. Bytes after the last syncframe are left (logged once).
// - The information block describes the unit's first independent frame: codecType 0 for an AC-3 frame and 1 for an
//   E-AC-3 one; fsIndex fscod, or 3 + fscod2 at E-AC-3's reduced rates (24, 22.05, 16 kHz); frmSizeCod AC-3's
//   frmsizecod or E-AC-3's frmsiz; bsi the 64 bits from bsi()'s start (AC-3's bsid, E-AC-3's strmtyp); bitrateValue
//   from the frame's size and length; esSizeBit the bits decoded; crcErrorFlag any frame's crc2 mismatch. E-AC-3's
//   mixing metadata (mixmdate) gives annex D's fields and levels (xbsi1e, dmixmod, the Lt/Rt and Lo/Ro mix levels,
//   which the downmix then uses), its informational metadata (infomdate: xbsi2e) bsmod, copyright, original, dsurmod,
//   dheadphonmod, dsurexmod and adconvtyp; without mixing metadata the centre and surround mix levels are FFmpeg's
//   E-AC-3 defaults (-4.5 and -6 dB: cmixlev and surmixlev 1).
// - PTS: the core's own correction (ADEC_CORRECT_PTS_VALUE_TYPE_EAC3, _SceAdecCorrectPtsValue_Ddp), the previous
//   frame's PTS plus its length (an E-AC-3 frame's length varies with its blocks and rate).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

enum : u32 {
    CELL_ADEC_ERROR_AC3_BUSY = 0x80612500,
    CELL_ADEC_ERROR_AC3_PARAM = 0x80612502,
    CELL_ADEC_ERROR_AC3_FRAME = 0x80612503,
};

// CellAdecParamAc3.
namespace prm {
constexpr u32 word_size = 0x00, output_mode = 0x01, out_lfe_on = 0x02, drc_cut = 0x04, drc_boost = 0x08,
              compression_mode = 0x0C, number_of_channels = 0x0D, stereo_mode = 0x0E, dualmono_mode = 0x0F,
              karaoke_mode = 0x10, pcm_scale = 0x14, channel_pointer = 0x18, size = 0x30;
}
// CellAdecBsiAc3.
namespace info {
constexpr u32 codec_type = 0x00, version_info = 0x04, total_call_carry_run = 0x08, total_call_num = 0x0C,
              bitrate_value = 0x10, pcm_size = 0x14, es_size_bit = 0x18, error_code = 0x1C, lib_info_flag = 0x20,
              validity = 0x21, channel_value = 0x22, fs_index = 0x23, output_quantization = 0x24,
              output_channel = 0x25, mono_flag = 0x26, bsi = 0x28, frm_size_cod = 0x30, acmod = 0x32, lfe_on = 0x33,
              karaoke_mode = 0x34, cmixlev = 0x35, surmixlev = 0x36, dsurmod = 0x37, copyright = 0x38, original = 0x39,
              bsmod = 0x3A, bsid = 0x3B, xbsi1e = 0x3C, dmixmod = 0x3D, xbsi2e = 0x3E, dsurexmod = 0x3F,
              dheadphonmod = 0x40, adconvtyp = 0x41, crc_error_flag = 0x42, exec_dmix_type = 0x43, size = 0x44;
}

enum : u32 { STEREO_AUTO = 0, STEREO_LTRT = 1, STEREO_LORO = 2 };
enum : u32 { DUALMONO_STEREO = 0, DUALMONO_LEFT = 1, DUALMONO_RIGHT = 2, DUALMONO_MIXED = 3 };
enum : u32 { RF_REMOD = 3 };
// Dolby's channel numbers.
enum : u32 { L = 0, C = 1, R = 2, LS = 3, RS = 4, LFE = 5, S = LS };

constexpr u16 kKbps[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
constexpr float kClev[4] = {0.70710678f, 0.59460356f, 0.5f, 0.59460356f}; // cmixlev (A/52 table 5.9)
constexpr float kSlev[4] = {0.70710678f, 0.5f, 0.0f, 0.5f};                // surmixlev (table 5.10)
// Annex D's mix levels (ltrtcmixlev and the others): +3 dB down to -6 dB in 1.5 dB steps, then off.
constexpr float kXlev[8] = {1.41421356f, 1.18920712f, 1.0f, 0.84089642f, 0.70710678f, 0.59460356f, 0.5f, 0.0f};
constexpr float k3dB = 0.70710678f;

// What a syncframe's header says (A/52 5.3, 5.4.2 and annexes D and E).
struct Bsi {
    u32 fscod = 0, frmsizecod = 0, bsid = 0, bsmod = 0, acmod = 0, cmixlev = 0, surmixlev = 0, dsurmod = 0;
    bool lfeon = false, copyright = false, original = false;
    bool xbsi1e = false, xbsi2e = false;
    u32 dmixmod = 0, ltrtcmixlev = 4, ltrtsurmixlev = 4, lorocmixlev = 4, lorosurmixlev = 4;
    u32 dsurexmod = 0, dheadphonmod = 0, adconvtyp = 0;
    u32 frame_bytes = 0, kbps = 0;
    // The samples a channel and the rate; where bsi() starts (AC-3: bsid at byte 5; E-AC-3: strmtyp at byte 2).
    u32 samples = 1536, rate = 48000, raw_from = 5;
    // E-AC-3's own (annex E).
    bool eac3 = false;
    u32 strmtyp = 0, substreamid = 0, frmsiz = 0, fscod2 = 0;
    u32 channels() const
    {
        static constexpr u8 kFront[8] = {2, 1, 2, 3, 2, 3, 2, 3}, kRear[8] = {0, 0, 0, 0, 1, 1, 2, 2};
        return kFront[acmod] + kRear[acmod] + lfeon;
    }
};

// The header of the frame at `p` (`size` bytes); false when it is no AC-3 frame.
bool read_bsi(const u8* p, u32 size, Bsi& h)
{
    if (size < 8) return false;
    Bits b(p, size);
    if (b.get(16) != 0x0B77) return false;
    b.skip(16); // crc1
    h.fscod = b.get(2);
    h.frmsizecod = b.get(6);
    h.bsid = b.get(5);
    h.bsmod = b.get(3);
    h.acmod = b.get(3);
    if ((h.acmod & 1) && h.acmod != 1) h.cmixlev = b.get(2);
    if (h.acmod & 4) h.surmixlev = b.get(2);
    if (h.acmod == 2) h.dsurmod = b.get(2);
    h.lfeon = b.flag();
    for (u32 n = 0; n < (h.acmod == 0 ? 2u : 1u); n++) {
        b.skip(5);                   // dialnorm
        if (b.flag()) b.skip(8);     // compre, compr
        if (b.flag()) b.skip(8);     // langcode, langcod
        if (b.flag()) b.skip(5 + 2); // audprodie, mixlevel, roomtyp
    }
    h.copyright = b.flag();
    h.original = b.flag();
    if (h.bsid == 6) {
        if ((h.xbsi1e = b.flag())) {
            h.dmixmod = b.get(2);
            h.ltrtcmixlev = b.get(3);
            h.ltrtsurmixlev = b.get(3);
            h.lorocmixlev = b.get(3);
            h.lorosurmixlev = b.get(3);
        }
        if ((h.xbsi2e = b.flag())) {
            h.dsurexmod = b.get(2);
            h.dheadphonmod = b.get(2);
            h.adconvtyp = b.get(1);
        }
    }
    if (h.fscod == 3 || h.frmsizecod >= 38) return false;
    h.kbps = kKbps[h.frmsizecod / 2];
    const u32 words = h.fscod == 0 ? h.kbps * 2 : h.fscod == 2 ? h.kbps * 3 : h.kbps * 96000 / 44100 + (h.frmsizecod & 1);
    h.frame_bytes = 2 * words;
    static constexpr u32 kRates[3] = {48000, 44100, 32000};
    h.rate = kRates[h.fscod] >> (h.bsid > 8 ? std::min<u32>(h.bsid - 8, 2) : 0); // bsid 9 and 10: half and quarter rates
    return !b.overrun;
}

// The header of the E-AC-3 syncframe at `p` (`size` bytes): A/52 annex E, E.1.2's syncinfo and bsi() as far as its
// informational metadata. False when it is none, names a reserved rate, or runs past `size`.
bool read_eac3_bsi(const u8* p, u32 size, Bsi& h)
{
    if (size < 6) return false;
    Bits b(p, size);
    if (b.get(16) != 0x0B77) return false;
    h.eac3 = true;
    h.raw_from = 2;
    h.cmixlev = h.surmixlev = 1; // FFmpeg's E-AC-3 defaults without mixing metadata: -4.5 and -6 dB
    h.strmtyp = b.get(2);
    h.substreamid = b.get(3);
    h.frmsiz = b.get(11);
    h.fscod = b.get(2);
    u32 numblkscod = 3;
    if (h.fscod == 3) {
        h.fscod2 = b.get(2);
        if (h.fscod2 == 3) return false;
    } else {
        numblkscod = b.get(2);
    }
    static constexpr u32 kBlocks[4] = {1, 2, 3, 6}, kRates[3] = {48000, 44100, 32000};
    const u32 blocks = kBlocks[numblkscod];
    h.samples = 256 * blocks;
    h.rate = h.fscod == 3 ? kRates[h.fscod2] / 2 : kRates[h.fscod];
    h.acmod = b.get(3);
    h.lfeon = b.flag();
    h.bsid = b.get(5);
    b.skip(5);                   // dialnorm
    if (b.flag()) b.skip(8);     // compre, compr
    if (h.acmod == 0) {
        b.skip(5);               // dialnorm2
        if (b.flag()) b.skip(8); // compr2e, compr2
    }
    if (h.strmtyp == 1 && b.flag()) b.skip(16); // chanmape, chanmap
    if ((h.xbsi1e = b.flag())) {                // mixmdate: the mixing metadata
        if (h.acmod > 2) h.dmixmod = b.get(2);
        if ((h.acmod & 1) && h.acmod > 2) {
            h.ltrtcmixlev = b.get(3);
            h.lorocmixlev = b.get(3);
        }
        if (h.acmod & 4) {
            h.ltrtsurmixlev = b.get(3);
            h.lorosurmixlev = b.get(3);
        }
        if (h.lfeon && b.flag()) b.skip(5); // lfemixlevcode, lfemixlevcod
        if (h.strmtyp == 0) {
            if (b.flag()) b.skip(6);                 // pgmscle, pgmscl
            if (h.acmod == 0 && b.flag()) b.skip(6); // pgmscl2e, pgmscl2
            if (b.flag()) b.skip(6);                 // extpgmscle, extpgmscl
            const u32 mixdef = b.get(2);
            if (mixdef == 1) b.skip(1 + 1 + 3); // premixcmpsel, drcsrc, premixcmpscl
            else if (mixdef == 2) b.skip(12);   // mixdata
            else if (mixdef == 3) b.skip(8 * (b.get(5) + 2)); // mixdeflen, mixdata
            if (h.acmod < 2) {
                if (b.flag()) b.skip(8 + 6);                 // paninfoe: panmean, paninfo
                if (h.acmod == 0 && b.flag()) b.skip(8 + 6); // paninfo2e
            }
            if (b.flag()) { // frmmixcfginfoe: blkmixcfginfo
                if (numblkscod == 0) {
                    b.skip(5);
                } else {
                    for (u32 k = 0; k < blocks; k++)
                        if (b.flag()) b.skip(5);
                }
            }
        }
    }
    if ((h.xbsi2e = b.flag())) { // infomdate: the informational metadata
        h.bsmod = b.get(3);
        h.copyright = b.flag();
        h.original = b.flag();
        if (h.acmod == 2) {
            h.dsurmod = b.get(2);
            h.dheadphonmod = b.get(2);
        }
        if (h.acmod >= 6) h.dsurexmod = b.get(2);
        if (b.flag()) { // audprodie: mixlevel, roomtyp, adconvtyp
            b.skip(5 + 2);
            h.adconvtyp = b.get(1);
        }
        if (h.acmod == 0 && b.flag()) b.skip(5 + 2 + 1); // audprodi2e
        if (h.fscod < 3) b.skip(1);                      // sourcefscod
    }
    h.frame_bytes = 2 * (h.frmsiz + 1);
    h.kbps = static_cast<u32>((u64{h.frame_bytes} * 8 * h.rate / h.samples + 500) / 1000);
    return !b.overrun;
}

// A downmix: output channel o (Dolby's numbers) as a sum of the decoded ones.
struct Matrix {
    std::array<std::array<float, 6>, 6> m{};
    std::array<bool, 6> out{};
    bool folded = false;
};

// The channels of an output mode (Dolby's acmod numbering, 1/0 to 3/2): which of L, C, R, Ls/S, Rs.
std::array<bool, 6> channels_of(u32 mode)
{
    std::array<bool, 6> c{};
    c[C] = mode & 1;
    c[L] = c[R] = mode != 1;
    c[LS] = mode >= 4;
    c[RS] = mode >= 6;
    return c;
}

class Ac3 final : public Codec {
public:
    // `ddp`: cellDDPdec's core (D-621): FFmpeg's E-AC-3 decoder, the syncframes of program 1, libadec's error codes.
    explicit Ac3(bool ddp) : ddp_(ddp), name_(ddp ? "E-AC-3" : "AC-3") {}

    s32 start(const u8* param) override
    {
        std::memcpy(p_, param, prm::size);
        frames_ = 0;
        const float cut = ps_ldf32(p_ + prm::drc_cut), boost = ps_ldf32(p_ + prm::drc_boost);
        const bool rf = p_[prm::compression_mode] == RF_REMOD;
        if (!rf && boost != cut)
            log_once(std::format("{} boost", name_), std::format("{}: dynamic range boost scale {} differs from the "
                "cut's {}: FFmpeg scales both by the cut's", name_, boost, cut));
        configured_ = ff_.open(ddp_ ? "eac3" : "ac3", name_, [&](AVCodecContext* av) {
            av_opt_set_double(av, "drc_scale", rf ? 1.0 : static_cast<double>(cut), AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(av, "heavy_compr", rf, AV_OPT_SEARCH_CHILDREN);
        });
        return configured_ ? lv2::CELL_OK : error(ddp_ ? u32{CELL_ADEC_ERROR_ARG} : u32{CELL_ADEC_ERROR_AC3_PARAM});
    }

    Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) override
    {
        Decoded d;
        d.pts_type = ddp_ ? CORRECT_PTS_EAC3 : CORRECT_PTS_AC3;
        if (!ddp_) d.duration = ticks(1536, 48000);
        const auto fail = [&](u32 code) { // E-AC-3: libadec's FATAL for every unit refused (D-621)
            const u32 c = ddp_ ? CELL_ADEC_ERROR_FATAL : code;
            d.error = error(c);
            ps_write32(base, bsi + info::error_code, c);
            return d;
        };
        if (!configured_) {
            PS_LOG_ERROR("cellAdec", "{}: a decode without a sequence started with valid parameters", name_);
            return fail(CELL_ADEC_ERROR_AC3_FRAME);
        }
        Bsi h;
        pcm_.clear();
        if (ddp_) {
            if (!decode_program(base, au, size, bsi, h)) return fail(CELL_ADEC_ERROR_AC3_FRAME);
            d.duration = ticks(pcm_.samples, pcm_.rate);
        } else {
            if (!read_bsi(base + au, size, h)) return fail(CELL_ADEC_ERROR_AC3_FRAME);
            write_bsi(base, bsi, base + au, size, h);
            if (h.bsid > 10) {
                log_once("AC-3 E-AC-3", std::format("AC-3: a frame of bsid {} (E-AC-3: cellDDPdec's) to the AC-3 core",
                    h.bsid));
                return fail(CELL_ADEC_ERROR_AC3_FRAME);
            }
            if (size < h.frame_bytes) {
                PS_LOG_ERROR("cellAdec", "AC-3: an access unit of {} bytes for a {}-byte frame", size, h.frame_bytes);
                return fail(CELL_ADEC_ERROR_AC3_FRAME);
            }
            if (size > h.frame_bytes)
                log_once("AC-3 longer", std::format("AC-3: an access unit of {} bytes holds more than its {}-byte frame: "
                    "the frame is decoded, the rest left", size, h.frame_bytes));
            ps_write8(base, bsi + info::crc_error_flag, mpeg_crc(base + au + 2, 8 * (h.frame_bytes - 2), 0) != 0);
            if (h.bsmod == 7 && h.acmod >= 2 && p_[prm::karaoke_mode] != 3)
                log_once("AC-3 karaoke", "AC-3: a karaoke stream's vocal modes are not reproduced: every channel is decoded");
            if (const int e = ff_.decode(base + au, h.frame_bytes, pcm_, 1536); e < 0 || !pcm_.samples) {
                PS_LOG_ERROR("cellAdec", "AC-3: FFmpeg refused a frame ({}): the game sent invalid data", e);
                return fail(CELL_ADEC_ERROR_AC3_FRAME);
            }
        }
        // FFmpeg's channels as Dolby's.
        Planar in;
        in.samples = pcm_.samples;
        in.rate = pcm_.rate;
        in.ch.assign(6, {});
        std::array<bool, 6> have{};
        for (u32 c = 0; c < pcm_.channels(); c++) {
            u32 k = 6;
            switch (pcm_.ids[c]) {
            case AV_CHAN_FRONT_LEFT: k = L; break;
            case AV_CHAN_FRONT_CENTER: k = C; break;
            case AV_CHAN_FRONT_RIGHT: k = R; break;
            case AV_CHAN_SIDE_LEFT:
            case AV_CHAN_BACK_LEFT:
            case AV_CHAN_BACK_CENTER: k = LS; break;
            case AV_CHAN_SIDE_RIGHT:
            case AV_CHAN_BACK_RIGHT: k = RS; break;
            case AV_CHAN_LOW_FREQUENCY: k = LFE; break;
            default: break;
            }
            if (k < 6) {
                in.ch[k] = std::move(pcm_.ch[c]);
                have[k] = true;
            }
        }
        const Matrix mx = matrix(h, have);
        // The output channels in Dolby's numbering, then routed to the game's slots.
        Planar dolby;
        dolby.samples = in.samples;
        dolby.rate = in.rate;
        dolby.ch.assign(6, std::vector<float>(in.samples, 0.0f));
        for (u32 o = 0; o < 6; o++) {
            if (!mx.out[o]) continue;
            for (u32 k = 0; k < 6; k++) {
                const float g = mx.m[o][k];
                if (g == 0.0f || !have[k]) continue;
                for (u32 i = 0; i < in.samples; i++) dolby.ch[o][i] += g * in.ch[k][i];
            }
        }
        const u32 slots = p_[prm::number_of_channels];
        std::vector<int> map(slots, -1);
        for (u32 k = 0; k < 6; k++) {
            const s32 at = static_cast<s32>(ps_ld32(p_ + prm::channel_pointer + 4 * k));
            if (at >= 0 && static_cast<u32>(at) < slots && mx.out[k]) map[static_cast<usize>(at)] = static_cast<int>(k);
        }
        d.size = write_pcm(base, out, dolby, map, p_[prm::word_size] ? Format::F32 : Format::S16,
            ps_ldf32(p_ + prm::pcm_scale));
        ps_write32(base, bsi + info::total_call_num, ++frames_);
        ps_write32(base, bsi + info::pcm_size, d.size);
        ps_write8(base, bsi + info::validity, 1);
        ps_write8(base, bsi + info::exec_dmix_type, mx.folded ? (lt_rt(h) && p_[prm::output_mode] == 2 ? 1 : 2) : 0);
        return d;
    }

private:
    // `frame` has `size` bytes: a unit too short for its header's raw bytes gives zeros for those it lacks (D-605).
    void write_bsi(u8* base, u32 bsi, const u8* frame, u32 size, const Bsi& h)
    {
        u8 raw[8]{}; // the bytes from bsi()'s start on (AC-3: 5, E-AC-3: 2)
        if (size > h.raw_from) std::memcpy(raw, frame + h.raw_from, std::min<u32>(sizeof raw, size - h.raw_from));
        ps_write32(base, bsi + info::codec_type, h.eac3 ? 1 : 0);
        ps_write32(base, bsi + info::version_info, 0x01000000);
        ps_write32(base, bsi + info::total_call_carry_run, 0);
        ps_write32(base, bsi + info::bitrate_value, h.kbps);
        ps_write32(base, bsi + info::es_size_bit, 8 * h.frame_bytes);
        ps_write8(base, bsi + info::lib_info_flag, 0);
        ps_write8(base, bsi + info::channel_value, static_cast<u8>(h.channels()));
        ps_write8(base, bsi + info::fs_index, static_cast<u8>(h.eac3 && h.fscod == 3 ? 3 + h.fscod2 : h.fscod));
        ps_write8(base, bsi + info::output_quantization, p_[prm::word_size]);
        ps_write8(base, bsi + info::output_channel, p_[prm::number_of_channels]);
        ps_write8(base, bsi + info::mono_flag, p_[prm::output_mode] == 1);
        ps_write32(base, bsi + info::bsi, ps_ld32(raw));
        ps_write32(base, bsi + info::bsi + 4, ps_ld32(raw + 4));
        ps_write16(base, bsi + info::frm_size_cod, static_cast<u16>(h.eac3 ? h.frmsiz : h.frmsizecod));
        const std::pair<u32, u32> fields[] = {{info::acmod, h.acmod}, {info::lfe_on, h.lfeon},
            {info::karaoke_mode, h.bsmod == 7 && h.acmod >= 2}, {info::cmixlev, h.cmixlev}, {info::surmixlev, h.surmixlev},
            {info::dsurmod, h.dsurmod}, {info::copyright, h.copyright}, {info::original, h.original},
            {info::bsmod, h.bsmod}, {info::bsid, h.bsid}, {info::xbsi1e, h.xbsi1e}, {info::dmixmod, h.dmixmod},
            {info::xbsi2e, h.xbsi2e}, {info::dsurexmod, h.dsurexmod}, {info::dheadphonmod, h.dheadphonmod},
            {info::adconvtyp, h.adconvtyp}};
        for (const auto& [at, v] : fields) ps_write8(base, bsi + at, static_cast<u8>(v));
    }

    // E-AC-3 (D-621): program 1's independent syncframes of the unit decoded in order into pcm_, `h` the first one's
    // header (in the information block); its dependent substreams and the other programs' left. False when the unit
    // starts with no syncframe or one is cut, or FFmpeg refuses one.
    bool decode_program(u8* base, u32 au, u32 size, u32 bsi, Bsi& h)
    {
        constexpr u32 kMaxSamples = 4 * 1536;
        u32 at = 0, frames = 0, bits = 0;
        bool crc_error = false;
        while (at < size) {
            const u8* p = base + au + at;
            const u32 left = size - at;
            const u32 bsid = left >= 6 ? p[5] >> 3 : 0;
            Bsi f;
            const bool ok = left >= 6 && (bsid <= 10 ? read_bsi(p, left, f) : bsid <= 16 && read_eac3_bsi(p, left, f));
            if (!ok) {
                if (frames) {
                    log_once("E-AC-3 trailing", std::format("E-AC-3: an access unit's {} bytes past its last syncframe "
                        "are left", left));
                    break;
                }
                PS_LOG_ERROR("cellAdec", "E-AC-3: an access unit of {} bytes that starts with no syncframe (bsid {})",
                    size, bsid);
                return false;
            }
            if (f.frame_bytes < 8 || f.frame_bytes > left) {
                PS_LOG_ERROR("cellAdec", "E-AC-3: a {}-byte syncframe with {} bytes of its unit left", f.frame_bytes,
                    left);
                return false;
            }
            if (!f.eac3 || (f.strmtyp != 1 && f.substreamid == 0)) { // an AC-3 frame, or independent substream 0
                if (!frames) {
                    h = f;
                    write_bsi(base, bsi, p, left, h);
                    if (h.bsmod == 7 && h.acmod >= 2 && p_[prm::karaoke_mode] != 3)
                        log_once("E-AC-3 karaoke", "E-AC-3: a karaoke stream's vocal modes are not reproduced: every "
                            "channel is decoded");
                }
                crc_error |= mpeg_crc(p + 2, 8 * (f.frame_bytes - 2), 0) != 0;
                if (const int e = ff_.decode(p, f.frame_bytes, pcm_, kMaxSamples); e < 0) {
                    PS_LOG_ERROR("cellAdec", "E-AC-3: FFmpeg refused a syncframe ({}): the game sent invalid data", e);
                    return false;
                }
                frames++;
                bits += 8 * f.frame_bytes;
            } else {
                log_once("E-AC-3 substreams", std::format("E-AC-3: a {} substream {}: left out, program 1's "
                    "independent substream is decoded", f.strmtyp == 1 ? "dependent" : "independent", f.substreamid));
            }
            at += f.frame_bytes;
        }
        if (!frames || !pcm_.samples) {
            PS_LOG_ERROR("cellAdec", "E-AC-3: an access unit of {} bytes with no frame of program 1 decoded", size);
            return false;
        }
        ps_write8(base, bsi + info::crc_error_flag, crc_error);
        ps_write32(base, bsi + info::es_size_bit, bits);
        return true;
    }

    // Whether a 2/0 output of this stream is Lt/Rt (stereoMode, or its auto: annex D's preferred mode, else Lo/Ro).
    bool lt_rt(const Bsi& h) const
    {
        const u32 mode = p_[prm::stereo_mode];
        if (mode == STEREO_LTRT) return true;
        if (mode == STEREO_LORO) return false;
        return h.xbsi1e && h.dmixmod == 1;
    }

    // The downmix from the stream's channels to outputMode (see the file's head).
    Matrix matrix(const Bsi& h, const std::array<bool, 6>& have) const
    {
        Matrix x;
        const u32 mode = p_[prm::output_mode];
        const std::array<bool, 6> out = channels_of(mode);
        for (u32 k = 0; k < 5; k++) x.out[k] = out[k];
        x.out[LFE] = p_[prm::out_lfe_on] != 0;
        if (x.out[LFE]) x.m[LFE][LFE] = 1.0f;
        if (h.acmod == 0) { // dual mono: Ch1 in L, Ch2 in R
            const u32 dm = p_[prm::dualmono_mode];
            if (dm == DUALMONO_STEREO && out[L]) {
                x.m[L][L] = x.m[R][R] = 1.0f;
            } else {
                std::array<float, 6> mono{};
                if (dm == DUALMONO_LEFT) mono[L] = 1.0f;
                else if (dm == DUALMONO_RIGHT) mono[R] = 1.0f;
                else mono[L] = mono[R] = 0.5f;
                if (out[C]) x.m[C] = mono;
                else
                    for (u32 k = 0; k < 6; k++) x.m[L][k] = x.m[R][k] = k3dB * mono[k];
            }
            x.folded = dm != DUALMONO_STEREO || !out[L];
            return normalised(x);
        }
        const bool stereo_surrounds = have[LS] && have[RS], mono_surround = have[LS] && !have[RS];
        const bool two = mode == 2 || mode == 1; // 2/0, and 1/0 made from the 2/0 downmix
        const bool ltrt = mode == 2 && lt_rt(h);
        float clev = h.acmod & 1 && h.acmod != 1 ? kClev[h.cmixlev] : k3dB;
        float slev = h.acmod & 4 ? kSlev[h.surmixlev] : k3dB;
        if (h.xbsi1e && two) { // annex D's own levels for Lt/Rt and Lo/Ro
            clev = kXlev[ltrt ? h.ltrtcmixlev : h.lorocmixlev];
            slev = kXlev[std::max<u32>(3, ltrt ? h.ltrtsurmixlev : h.lorosurmixlev)];
        }
        if (ltrt && !h.xbsi1e) clev = slev = k3dB;
        // Front.
        if (have[L]) x.m[L][L] = 1.0f;
        if (have[R]) x.m[R][R] = 1.0f;
        if (have[C]) {
            if (out[C] && !two) {
                x.m[C][C] = 1.0f;
            } else {
                x.m[L][C] = x.m[R][C] = have[L] ? clev : 1.0f; // a mono stream's centre is both sides
                x.folded |= have[L];
            }
        }
        // Surround.
        if (stereo_surrounds || mono_surround) {
            // A mono surround goes to each side at 0.7071 of slev for Lo/Ro; Lt/Rt takes it whole, at its -3 dB.
            const float s_mono = mono_surround && !ltrt ? k3dB : 1.0f;
            if (two || !out[LS]) {
                const float sign = ltrt ? -1.0f : 1.0f;
                x.m[L][LS] = sign * slev * s_mono;
                if (stereo_surrounds) {
                    x.m[R][RS] = slev;
                    if (ltrt) { // Lt takes both surrounds, subtracted; Rt both, added
                        x.m[L][RS] = -slev;
                        x.m[R][LS] = slev;
                    }
                } else {
                    x.m[R][LS] = slev * s_mono;
                }
                x.folded = true;
            } else if (out[RS]) { // a pair out
                if (stereo_surrounds) {
                    x.m[LS][LS] = x.m[RS][RS] = 1.0f;
                } else {
                    x.m[LS][LS] = x.m[RS][LS] = k3dB;
                    x.folded = true;
                }
            } else { // one surround out
                if (stereo_surrounds) {
                    x.m[LS][LS] = x.m[LS][RS] = k3dB;
                    x.folded = true;
                } else {
                    x.m[LS][LS] = 1.0f;
                }
            }
        }
        if (mode == 1) { // 1/0: the 2/0 Lo/Ro pair's sum
            for (u32 k = 0; k < 6; k++) x.m[C][k] = k3dB * (x.m[L][k] + x.m[R][k]);
            x.m[L] = {};
            x.m[R] = {};
            x.folded = true;
        }
        return normalised(x);
    }

    // Each output channel over its coefficients' sum, when that passes 1.
    static Matrix normalised(Matrix x)
    {
        for (u32 o = 0; o < 5; o++) {
            float sum = 0;
            for (u32 k = 0; k < 5; k++) sum += std::abs(x.m[o][k]);
            if (sum > 1.0f)
                for (u32 k = 0; k < 5; k++) x.m[o][k] /= sum;
        }
        return x;
    }

    bool ddp_;
    std::string_view name_;
    FfDecoder ff_;
    Planar pcm_;
    u8 p_[prm::size] = {};
    u32 frames_ = 0;
    bool configured_ = false;
};

// cellAdecStartSeq: every field within its enum, 1 to 6 slots, each pointer a slot or -1 and none twice, the scale
// factors between 0 and 1. `ddp`: E-AC-3's, refused with libadec's ARG.
s32 check_params(const u8* p, bool ddp)
{
    bool ok = p[prm::word_size] <= 1 && p[prm::output_mode] >= 1 && p[prm::output_mode] <= 7 && p[prm::out_lfe_on] <= 1 &&
              p[prm::compression_mode] <= 3 && p[prm::stereo_mode] <= 2 && p[prm::dualmono_mode] <= 3 &&
              p[prm::karaoke_mode] <= 3 && p[prm::number_of_channels] >= 1 && p[prm::number_of_channels] <= 6;
    for (const u32 at : {prm::drc_cut, prm::drc_boost, prm::pcm_scale}) {
        const float v = ps_ldf32(p + at);
        ok = ok && v >= 0.0f && v <= 1.0f;
    }
    u32 taken = 0;
    for (u32 k = 0; k < 6 && ok; k++) {
        const s32 at = static_cast<s32>(ps_ld32(p + prm::channel_pointer + 4 * k));
        if (at == -1) continue;
        ok = at >= 0 && at < p[prm::number_of_channels] && !(taken & (1u << at));
        taken |= 1u << (at & 31);
    }
    if (ok) return lv2::CELL_OK;
    PS_LOG_ERROR("cellAdec", "{}: parameters refused (word {}, output mode {}, LFE {}, {} channels)",
        ddp ? "E-AC-3" : "AC-3", p[prm::word_size], p[prm::output_mode], p[prm::out_lfe_on], p[prm::number_of_channels]);
    return error(ddp ? u32{CELL_ADEC_ERROR_ARG} : u32{CELL_ADEC_ERROR_AC3_PARAM});
}
s32 check(const u8* p) { return check_params(p, false); }
s32 check_ddp(const u8* p) { return check_params(p, true); }

} // namespace

// libac3dec's version is not known (pspocket's word); the information block is CellAdecBsiAc3's size. Up to six
// channels of a frame's 1536 samples.
const Spec kAc3 = {CELL_ADEC_TYPE_AC3, "AC-3", prm::size, info::size, 1536 * 6 * 4, 0x01000000, CELL_ADEC_ERROR_AC3_BUSY,
    check, [] { return std::unique_ptr<Codec>(std::make_unique<Ac3>(false)); }};
// Dolby Digital Plus (D-621): AC-3's parameters and information block, libadec's error codes; up to four 1536-sample
// frames of six channels per unit.
const Spec kDdp = {CELL_ADEC_TYPE_EAC3, "E-AC-3", prm::size, info::size, 4 * 1536 * 6 * 4, 0x01000000,
    CELL_ADEC_ERROR_BUSY, check_ddp, [] { return std::unique_ptr<Codec>(std::make_unique<Ac3>(true)); }};

} // namespace pspocket::hle::adec::ffcore

#endif
