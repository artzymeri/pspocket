#pragma once

// A minimal VC-1 writer for tests (synthetic video, no game data, D-589, D-920): SMPTE 421M's advanced profile in its
// start code framing (a sequence header, an entry point, frames), progressive frames, with these kinds of picture:
// - intra pictures (I and BI) whose blocks carry only their DC coefficient (every macroblock's CBPCY 0, no AC
//   prediction), so each 8x8 block decodes flat to a value known from its DC level (block_value);
// - skipped P pictures (PTYPE 1111), which repeat the picture before them;
// - P pictures whose every macroblock is skipped (the SKIPMB bitplane all ones): the anchor before them again, or
//   with intensity compensation its samples mapped;
// - B pictures whose every macroblock is skipped and direct: the two anchors' average (their vectors all 0).
// The headers' fields are all the tests' to set (Sequence, Entry, Header). The few variable length codes used are the
// standard's (the I picture CBPCY table, the DC differential tables of TRANSDCTAB 0, the bitplane IMODE table, PTYPE,
// MVMODE, BFRACTION), as FFmpeg's vc1.c, msmpeg4_vc1_data.c and vc1_vlc_data.h hold them.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "h264_writer.hpp" // BitWriter

namespace pspocket::hle::test::vc1 {

using h264::BitWriter;

// The start code suffixes of the bitstream data units (Annex E).
constexpr uint8_t kSequenceHeader = 0x0F, kEntryPoint = 0x0E, kFrame = 0x0D, kField = 0x0C, kSlice = 0x0B,
                  kEndOfSequence = 0x0A, kFrameUserData = 0x1D;

// A bitstream data unit: its start code, then its payload with the emulation prevention bytes (Annex E.5, as H.264's).
inline void put_bdu(std::vector<uint8_t>& out, uint8_t suffix, const std::vector<uint8_t>& payload)
{
    out.insert(out.end(), {0x00, 0x00, 0x01, suffix});
    int zeros = 0;
    for (const uint8_t b : payload) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

struct Sequence {
    int width = 64, height = 48; // MAX_CODED_WIDTH and MAX_CODED_HEIGHT, the pictures' size
    int level = 1;
    int colordiff = 1;                            // COLORDIFF_FORMAT (1: 4:2:0)
    int frmrtq_postproc = 0, bitrtq_postproc = 0; // the post-processing hints
    bool postprocflag = false;
    bool pulldown = false, interlace = false, tfcntrflag = false, finterpflag = false, psf = false;
    // DISPLAY_EXT with its display size, when display_width is not 0; then each of these when given:
    int display_width = 0, display_height = 0;
    int aspect_ratio = 0, aspect_width = 0, aspect_height = 0; // ASPECT_RATIO (15: ASPECT_HORIZ_SIZE, ..._VERT_SIZE)
    int framerate_nr = 0, framerate_dr = 0;                    // FRAMERATENR, FRAMERATEDR (FRAMERATEIND 0)
    int framerate_exp = -1;                                    // FRAMERATEEXP (FRAMERATEIND 1) when not negative
    int color_prim = 0, transfer_char = 0, matrix_coef = 0;    // COLOR_FORMAT_FLAG when color_prim is not 0
    // HRD_PARAM_FLAG when hrd_buckets is not 0: HRD_NUM_LEAKY_BUCKETS, the exponents, each bucket's rate and buffer.
    int hrd_buckets = 0, hrd_rate_exp = 0, hrd_buffer_exp = 0;
};

// The sequence header BDU (6.1).
inline std::vector<uint8_t> sequence_header(const Sequence& s)
{
    BitWriter w;
    w.bits(3, 2); // PROFILE: advanced
    w.bits(static_cast<uint32_t>(s.level), 3);
    w.bits(static_cast<uint32_t>(s.colordiff), 2);
    w.bits(static_cast<uint32_t>(s.frmrtq_postproc), 3);
    w.bits(static_cast<uint32_t>(s.bitrtq_postproc), 5);
    w.bit(s.postprocflag);
    w.bits(static_cast<uint32_t>(s.width / 2 - 1), 12);
    w.bits(static_cast<uint32_t>(s.height / 2 - 1), 12);
    w.bit(s.pulldown);
    w.bit(s.interlace);
    w.bit(s.tfcntrflag);
    w.bit(s.finterpflag);
    w.bit(1); // reserved
    w.bit(s.psf);
    w.bit(s.display_width != 0); // DISPLAY_EXT
    if (s.display_width) {
        w.bits(static_cast<uint32_t>(s.display_width - 1), 14);
        w.bits(static_cast<uint32_t>(s.display_height - 1), 14);
        w.bit(s.aspect_ratio != 0); // ASPECT_RATIO_FLAG
        if (s.aspect_ratio) {
            w.bits(static_cast<uint32_t>(s.aspect_ratio), 4);
            if (s.aspect_ratio == 15) {
                w.bits(static_cast<uint32_t>(s.aspect_width - 1), 8);
                w.bits(static_cast<uint32_t>(s.aspect_height - 1), 8);
            }
        }
        w.bit(s.framerate_nr != 0 || s.framerate_exp >= 0); // FRAMERATE_FLAG
        if (s.framerate_exp >= 0) {
            w.bit(1); // FRAMERATEIND
            w.bits(static_cast<uint32_t>(s.framerate_exp), 16);
        } else if (s.framerate_nr) {
            w.bit(0);
            w.bits(static_cast<uint32_t>(s.framerate_nr), 8);
            w.bits(static_cast<uint32_t>(s.framerate_dr), 4);
        }
        w.bit(s.color_prim != 0); // COLOR_FORMAT_FLAG
        if (s.color_prim) {
            w.bits(static_cast<uint32_t>(s.color_prim), 8);
            w.bits(static_cast<uint32_t>(s.transfer_char), 8);
            w.bits(static_cast<uint32_t>(s.matrix_coef), 8);
        }
    }
    w.bit(s.hrd_buckets != 0); // HRD_PARAM_FLAG
    if (s.hrd_buckets) {
        w.bits(static_cast<uint32_t>(s.hrd_buckets), 5);
        w.bits(static_cast<uint32_t>(s.hrd_rate_exp), 4);
        w.bits(static_cast<uint32_t>(s.hrd_buffer_exp), 4);
        for (int i = 0; i < s.hrd_buckets; i++) {
            w.bits(static_cast<uint32_t>(0x1000 + i), 16); // HRD_RATE
            w.bits(static_cast<uint32_t>(0x2000 + i), 16); // HRD_BUFFER
        }
    }
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kSequenceHeader, w.take());
    return out;
}

struct Entry {
    bool broken_link = false, closed_entry = true;
    bool panscan = false, refdist = false, loopfilter = false, fastuvmc = false, extended_mv = false;
    int dquant = 0;
    bool vstransform = false, overlap = false;
    int quantizer = 0;                     // QUANTIZER: 0 implicit, 1 explicit (PQUANTIZER each picture), 2/3 fixed
    int coded_width = 0, coded_height = 0; // CODED_SIZE_FLAG with these when not 0
    bool extended_dmv = false;             // with EXTENDED_MV
    int range_mapy = -1, range_mapuv = -1; // RANGE_MAPY_FLAG and RANGE_MAPUV_FLAG with their values when not negative
    int hrd_full = 0;                      // HRD_FULL's value for each of the sequence's buckets
};

// The entry point BDU (6.2); `s` the sequence header's (its HRD buckets).
inline std::vector<uint8_t> entry_point(const Entry& e, const Sequence& s = {})
{
    BitWriter w;
    w.bit(e.broken_link);
    w.bit(e.closed_entry);
    w.bit(e.panscan);
    w.bit(e.refdist);
    w.bit(e.loopfilter);
    w.bit(e.fastuvmc);
    w.bit(e.extended_mv);
    w.bits(static_cast<uint32_t>(e.dquant), 2);
    w.bit(e.vstransform);
    w.bit(e.overlap);
    w.bits(static_cast<uint32_t>(e.quantizer), 2);
    for (int i = 0; i < s.hrd_buckets; i++) w.bits(static_cast<uint32_t>(e.hrd_full + i), 8);
    w.bit(e.coded_width != 0); // CODED_SIZE_FLAG
    if (e.coded_width) {
        w.bits(static_cast<uint32_t>(e.coded_width / 2 - 1), 12);
        w.bits(static_cast<uint32_t>(e.coded_height / 2 - 1), 12);
    }
    if (e.extended_mv) w.bit(e.extended_dmv);
    w.bit(e.range_mapy >= 0);
    if (e.range_mapy >= 0) w.bits(static_cast<uint32_t>(e.range_mapy), 3);
    w.bit(e.range_mapuv >= 0);
    if (e.range_mapuv >= 0) w.bits(static_cast<uint32_t>(e.range_mapuv), 3);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kEntryPoint, w.take());
    return out;
}

// The bitplanes' coding modes (Header::bitplane).
constexpr int kRowSkip = 0, kNorm2 = 1, kDiff2 = 2, kNorm6 = 3, kDiff6 = 4, kColSkip = 5;

// A progressive frame's header fields after its type, as the sequence header and entry point call for them.
struct Header {
    int tfcntr = 0;                      // with TFCNTRFLAG
    int rptfrm = 0;                      // with PULLDOWN (a progressive sequence, or PSF)
    bool tff = true, rff = false;        // with PULLDOWN in an interlaced sequence
    // With the entry point's PANSCAN_FLAG: PS_PRESENT and the windows (as many as the pulldown fields call for).
    bool ps_present = false;
    struct Window {
        int hoffset = 0, voffset = 0, width = 0, height = 0;
    };
    std::vector<Window> windows;
    bool rndctrl = false, uvsamp = false, interpfrm = false; // UVSAMP with INTERLACE, INTERPFRM with FINTERPFLAG
    int bfraction = 0;                   // B pictures: BFRACTION's index (0 1/2 ... 6 2/5, 7 to 20 the long codes)
    int pqindex = 4;
    bool halfqp = false;                 // with a PQINDEX below 9
    bool pquantizer = false;             // with QUANTIZER 1
    int postproc = 0;                    // with POSTPROCFLAG
    int condover = 0; // I pictures with OVERLAP and PQUANT up to 8: 0 none, 2 all (3, a bitplane, not)
    int mvrange = 0;                     // P and B pictures with EXTENDED_MV
    // P pictures: MVMODE (kOneMv, kMixedMv: an MVTYPEMB bitplane of 1MV macroblocks, kHalfPel, kHalfPelBilinear), or
    // intensity compensation (MVMODE's IC, then that mode as MVMODE2) with LUMSCALE and LUMSHIFT.
    int mvmode = 0;
    bool intensity = false;
    int lumscale = 32, lumshift = 0;
    bool b_hpel_bilinear = false;        // B pictures: MVMODE 0 (half sample bilinear) instead of 1MV
    int mvtab = 0, cbptab = 0;           // P and B pictures
    // VOPDQUANT (with DQUANT; I pictures after TRANSDCTAB): DQUANT 1's DQUANTFRM, then DQPROFILE (0 all four edges, 1
    // double edges, 2 single edge, 3 all macroblocks) and its DQDBEDGE or DQSBEDGE, or DQBILEVEL; PQDIFF (7: ABSPQ).
    bool dquantfrm = false;
    int dqprofile = 0, dqedge = 0;
    bool dqbilevel = true;
    int pqdiff = 0, abspq = 1;
    // TTMBF and TTFRM (P and B pictures with VSTRANSFORM: TTFRM 0 8x8, 1 8x4, 2 4x8, 3 4x4).
    bool ttmbf = true;
    int ttfrm = 0;
    int transacfrm = 0, transacfrm2 = 0; // the AC tables (TRANSACFRM2: intra pictures)
    bool transdctab = false;
    // Interlaced pictures: P frames' 4MVSWITCH and INTCOMP (with LUMSCALE and LUMSHIFT), their tables; P fields'
    // NUMREF and REFFIELD; field pictures' REFDIST (with the entry point's REFDIST_FLAG), a DMVRANGE code.
    bool fourmvswitch = false, intcomp = false;
    int mbmodetab = 0, imvtab = 0, icbptab = 0, twomvbptab = 0, fourmvbptab = 0;
    bool numref = false, reffield = false;
    int refdist = 0, dmvrange = 0;
    int bitplane = kRowSkip; // the flat bitplanes' coding mode
};

// MVMODE's values (Header::mvmode).
constexpr int kOneMv = 0, kMixedMv = 1, kHalfPel = 2, kHalfPelBilinear = 3;

namespace detail {

// PQUANT of a PQINDEX (the implicit quantizer's table, 7.1.1.6; the explicit and fixed ones: the index itself).
inline int pquant(int pqindex, int quantizer)
{
    constexpr int kImplicit[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 27, 29, 31};
    return quantizer == 0 ? kImplicit[pqindex] : pqindex;
}

// A bitplane of all ones or all zeros (8.7: INVERT, then a plane of zeros) in coding mode `mode` (Header::bitplane:
// kRowSkip, each row's ROWSKIP bit 0; kNorm2 and kDiff2, each pair's '0'; kNorm6 and kDiff6, each tile's '1' and its
// remainder's column and row skips; kColSkip, each column's bit 0).
inline void flat_bitplane(BitWriter& w, bool ones, int mbw, int mbh, int mode = kRowSkip)
{
    w.bit(ones);
    switch (mode) {
    case kNorm2:
    case kDiff2: {
        if (mode == kNorm2) w.bits(0x2, 2); // '10'
        else w.bits(0x1, 3);                // '001'
        const int n = mbw * mbh;
        for (int i = 0; i < (n & 1) + n / 2; i++) w.bit(0);
        break;
    }
    case kNorm6:
    case kDiff6:
        if (mode == kNorm6) w.bits(0x3, 2); // '11'
        else w.bits(0x1, 4);                // '0001'
        if (mbh % 3 == 0 && mbw % 3 != 0) { // 2x3 tiles, a column left
            for (int i = 0; i < mbh / 3 * (mbw / 2); i++) w.bit(1);
            if (mbw & 1) w.bit(0);
        } else { // 3x2 tiles, columns and a row left
            for (int i = 0; i < mbh / 2 * (mbw / 3); i++) w.bit(1);
            for (int i = 0; i < mbw % 3; i++) w.bit(0);
            if (mbh & 1) w.bit(0);
        }
        break;
    case kColSkip:
        w.bits(0x3, 3); // '011'
        for (int c = 0; c < mbw; c++) w.bit(0);
        break;
    default:
        w.bits(0x2, 3); // '010'
        for (int r = 0; r < mbh; r++) w.bit(0);
        break;
    }
}

// decode012's code of v (0 '0', 1 '10', 2 '11').
inline void code012(BitWriter& w, int v)
{
    if (v == 0) w.bit(0);
    else w.bits(static_cast<uint32_t>(v == 1 ? 2 : 3), 2);
}

// The frame header's fields from TFCNTR to POSTPROC (after PTYPE, and BFRACTION's place).
inline void header_start(BitWriter& w, const Sequence& s, const Entry& e, const Header& h, bool skipped)
{
    if (s.tfcntrflag) w.bits(static_cast<uint32_t>(h.tfcntr), 8);
    if (s.pulldown) {
        if (!s.interlace || s.psf) {
            w.bits(static_cast<uint32_t>(h.rptfrm), 2);
        } else {
            w.bit(h.tff);
            w.bit(h.rff);
        }
    }
    if (e.panscan) {
        w.bit(h.ps_present);
        if (h.ps_present)
            for (const Header::Window& win : h.windows) {
                w.bits(static_cast<uint32_t>(win.hoffset) & 0x3FFFF, 18);
                w.bits(static_cast<uint32_t>(win.voffset) & 0x3FFFF, 18);
                w.bits(static_cast<uint32_t>(win.width), 14);
                w.bits(static_cast<uint32_t>(win.height), 14);
            }
    }
    if (skipped) return;
    w.bit(h.rndctrl);
    if (s.interlace) w.bit(h.uvsamp);
}

inline void quantizer_fields(BitWriter& w, const Sequence& s, const Entry& e, const Header& h)
{
    w.bits(static_cast<uint32_t>(h.pqindex), 5);
    if (h.pqindex <= 8) w.bit(h.halfqp);
    if (e.quantizer == 1) w.bit(h.pquantizer);
    if (s.postprocflag) w.bits(static_cast<uint32_t>(h.postproc), 2);
}

// VOPDQUANT (7.1.1.31), with the entry point's DQUANT.
inline void vopdquant(BitWriter& w, const Entry& e, const Header& h)
{
    if (!e.dquant) return;
    if (e.dquant != 2) {
        w.bit(h.dquantfrm);
        if (!h.dquantfrm) return;
        w.bits(static_cast<uint32_t>(h.dqprofile), 2);
        if (h.dqprofile == 1 || h.dqprofile == 2) w.bits(static_cast<uint32_t>(h.dqedge), 2);
        if (h.dqprofile == 3) {
            w.bit(h.dqbilevel);
            if (!h.dqbilevel) return;
        }
    }
    w.bits(static_cast<uint32_t>(h.pqdiff), 3);
    if (h.pqdiff == 7) w.bits(static_cast<uint32_t>(h.abspq), 5);
}

// TTMBF and TTFRM, with VSTRANSFORM.
inline void transform_type(BitWriter& w, const Entry& e, const Header& h)
{
    if (!e.vstransform) return;
    w.bit(h.ttmbf);
    if (h.ttmbf) w.bits(static_cast<uint32_t>(h.ttfrm), 2);
}

// MVMODE's code (7.1.1.32): its table by PQUANT (up to 12, or above), `mode` a Header::mvmode; MVMODE2's (IC's)
// leaves the IC code out.
inline void mvmode_code(BitWriter& w, int mode, bool low, bool second)
{
    // The unary codes' order ('1', '01', '001', '0001', '0000'): low rate 1MV, mixed, half, IC, bilinear; high rate
    // bilinear, 1MV, half, IC, mixed (MVMODE2: the same without IC, '000' last).
    constexpr int kLow[4] = {0, 1, 2, 4}, kHigh[4] = {1, 4, 2, 0}; // by mode: the code's index
    int index = low ? kLow[mode] : kHigh[mode];
    if (second && index == 4) index = 3;
    const int ones_at = index; // zeros before the 1 (none for the last code)
    const int last = second ? 3 : 4;
    for (int i = 0; i < ones_at; i++) w.bit(0);
    if (index < last) w.bit(1);
}

} // namespace detail

// An intra picture's DC levels (quantized, as the stream codes them): the luma's per 8x8 block (width / 8 by
// height / 8, row by row), each chroma plane's per macroblock.
struct DcPicture {
    int width = 64, height = 48; // multiples of 16
    std::vector<int> y, cb, cr;
    DcPicture(int w, int h)
        : width(w), height(h), y(static_cast<size_t>(w / 8 * (h / 8))), cb(static_cast<size_t>(w / 16 * (h / 16))),
          cr(static_cast<size_t>(w / 16 * (h / 16)))
    {
    }
};

// PQINDEX 4 with the implicit quantizer: PQUANT 4, whose DC step is 8 (the standard's DC step table).
constexpr int kPqIndex = 4, kDcStep = 8;

// What a block of DC level `dc` and no AC coefficient decodes to: the inverse transform's DC gain (12 in each of its
// two passes, with each pass's rounding), then the 128 an intra block is offset by.
inline int block_value(int dc)
{
    const int d = dc * kDcStep;
    return std::clamp(128 + ((12 * ((12 * d + 4) >> 3) + 64) >> 7), 0, 255);
}

namespace detail {

// The advanced profile's DC predictor: the block to the left (C) or the one above (A), whichever the top-left one (B)
// is further from; 0 for the picture's first block (rows above `top`, a slice's first, unavailable).
inline int dc_predictor(const std::vector<int>& plane, int columns, int bx, int by, int top = 0)
{
    const bool a_ok = by > top, c_ok = bx > 0;
    const int a = a_ok ? plane[static_cast<size_t>((by - 1) * columns + bx)] : 0;
    const int c = c_ok ? plane[static_cast<size_t>(by * columns + bx - 1)] : 0;
    const int b = a_ok && c_ok ? plane[static_cast<size_t>((by - 1) * columns + bx - 1)] : 0;
    if (c_ok && (!a_ok || std::abs(a - b) <= std::abs(b - c))) return c;
    return a_ok ? a : 0;
}

// DCDIFF (PQUANT above 2: no extra bits) with TRANSDCTAB's table: 0's code, else the escape code, 8 bits and the sign.
inline void put_dc_diff(BitWriter& w, int diff, bool chroma, bool table1 = false)
{
    if (diff == 0) {
        if (table1) w.bits(chroma ? 0x0 : 0x2, 2);
        else if (chroma) w.bits(0x0, 2);
        else w.bits(0x1, 1);
        return;
    }
    if (table1) {
        if (chroma) w.bits(0x18F648F, 25);
        else w.bits(0x1E6967, 26);
    } else if (chroma) {
        w.bits(0x60888F, 23);
    } else {
        w.bits(0x6078F, 24);
    }
    w.bits(static_cast<uint32_t>(std::abs(diff)), 8);
    w.bit(diff < 0);
}

} // namespace detail

// A progressive intra picture's frame BDU (7.1): I, or BI when `bi`; every macroblock CBPCY 0 and its six DC
// differentials.
inline std::vector<uint8_t> intra_frame(const DcPicture& p, const Sequence& s, const Entry& e, const Header& h,
    bool bi = false)
{
    BitWriter w;
    if (s.interlace) w.bit(0); // FCM: a progressive frame
    if (bi) w.bits(0xE, 4);    // PTYPE: BI
    else w.bits(0x6, 3);       // PTYPE: I
    detail::header_start(w, s, e, h, false);
    if (s.finterpflag) w.bit(h.interpfrm);
    detail::quantizer_fields(w, s, e, h);
    w.bit(0);       // ACPRED bitplane: INVERT 0,
    w.bits(0x0, 4); // IMODE raw: each macroblock's bit in its own layer
    if (e.overlap && detail::pquant(h.pqindex, e.quantizer) <= 8) detail::code012(w, h.condover);
    detail::code012(w, h.transacfrm);
    detail::code012(w, h.transacfrm2);
    w.bit(h.transdctab);
    detail::vopdquant(w, e, h);
    const int mbw = p.width / 16, mbh = p.height / 16, columns = p.width / 8;
    for (int my = 0; my < mbh; my++)
        for (int mx = 0; mx < mbw; mx++) {
            w.bits(0x1, 1); // CBPCY 0
            w.bit(0);       // ACPRED
            for (int k = 0; k < 4; k++) {
                const int bx = 2 * mx + (k & 1), by = 2 * my + (k >> 1);
                const int dc = p.y[static_cast<size_t>(by * columns + bx)];
                detail::put_dc_diff(w, dc - detail::dc_predictor(p.y, columns, bx, by), false, h.transdctab);
            }
            for (const std::vector<int>* plane : {&p.cb, &p.cr}) {
                const int dc = (*plane)[static_cast<size_t>(my * mbw + mx)];
                detail::put_dc_diff(w, dc - detail::dc_predictor(*plane, mbw, mx, my), true, h.transdctab);
            }
        }
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// The I picture as test_vdec_codecs.cpp's streams have it (the default entry point and header, a repeat count).
inline std::vector<uint8_t> intra_frame(const DcPicture& p, const Sequence& s, int rptfrm = 0)
{
    Header h;
    h.rptfrm = rptfrm;
    return intra_frame(p, s, Entry{}, h);
}

// A skipped P picture's frame BDU: PTYPE 1111, the picture before it again.
inline std::vector<uint8_t> skipped_frame(const Sequence& s, const Entry& e = {}, const Header& h = {})
{
    BitWriter w;
    if (s.interlace) w.bit(0);
    w.bits(0xF, 4);
    detail::header_start(w, s, e, h, true);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// A P picture of `mbw` x `mbh` macroblocks, every one skipped (1MV, or intensity compensation then 1MV).
inline std::vector<uint8_t> skipped_mbs_p_frame(const Sequence& s, const Entry& e, const Header& h, int mbw, int mbh)
{
    BitWriter w;
    if (s.interlace) w.bit(0);
    w.bit(0); // PTYPE: P
    detail::header_start(w, s, e, h, false);
    if (s.finterpflag) w.bit(h.interpfrm);
    detail::quantizer_fields(w, s, e, h);
    if (e.extended_mv) { // MVRANGE
        for (int i = 0; i < h.mvrange; i++) w.bit(1);
        if (h.mvrange < 3) w.bit(0);
    }
    const bool low = detail::pquant(h.pqindex, e.quantizer) <= 12;
    if (h.intensity) {
        w.bits(0x1, 4); // MVMODE: intensity compensation
        detail::mvmode_code(w, h.mvmode, low, true);
        w.bits(static_cast<uint32_t>(h.lumscale), 6);
        w.bits(static_cast<uint32_t>(h.lumshift), 6);
    } else {
        detail::mvmode_code(w, h.mvmode, low, false);
    }
    if (h.mvmode == kMixedMv) detail::flat_bitplane(w, false, mbw, mbh, h.bitplane); // MVTYPEMB: every one 1MV
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane);                              // SKIPMB: every macroblock
    w.bits(static_cast<uint32_t>(h.mvtab), 2);
    w.bits(static_cast<uint32_t>(h.cbptab), 2);
    detail::vopdquant(w, e, h);
    detail::transform_type(w, e, h);
    detail::code012(w, h.transacfrm);
    w.bit(h.transdctab);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// A B picture of `mbw` x `mbh` macroblocks, every one skipped and direct: its anchors' average.
inline std::vector<uint8_t> direct_b_frame(const Sequence& s, const Entry& e, const Header& h, int mbw, int mbh)
{
    BitWriter w;
    if (s.interlace) w.bit(0);
    w.bits(0x2, 2); // PTYPE: B
    detail::header_start(w, s, e, h, false);
    if (s.finterpflag) w.bit(h.interpfrm);
    if (h.bfraction < 7) {
        w.bits(static_cast<uint32_t>(h.bfraction), 3);
    } else {
        w.bits(0x7, 3);
        w.bits(static_cast<uint32_t>(h.bfraction - 7), 4);
    }
    detail::quantizer_fields(w, s, e, h);
    if (e.extended_mv) {
        for (int i = 0; i < h.mvrange; i++) w.bit(1);
        if (h.mvrange < 3) w.bit(0);
    }
    w.bit(!h.b_hpel_bilinear);           // MVMODE
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // DIRECTMB: every macroblock
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // SKIPMB: every macroblock
    w.bits(static_cast<uint32_t>(h.mvtab), 2);
    w.bits(static_cast<uint32_t>(h.cbptab), 2);
    detail::vopdquant(w, e, h);
    detail::transform_type(w, e, h);
    detail::code012(w, h.transacfrm);
    w.bit(h.transdctab);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

namespace detail {

// MVRANGE's and DMVRANGE's codes ('0', '10', '110', '111').
inline void unary3(BitWriter& w, int v)
{
    for (int i = 0; i < v; i++) w.bit(1);
    if (v < 3) w.bit(0);
}

// An intra picture's (or field's) macroblock layer: every macroblock CBPCY 0, AC prediction raw 0, its DC
// differentials.
inline void intra_macroblocks(BitWriter& w, const DcPicture& p, bool table1)
{
    const int mbw = p.width / 16, mbh = p.height / 16, columns = p.width / 8;
    for (int my = 0; my < mbh; my++)
        for (int mx = 0; mx < mbw; mx++) {
            w.bits(0x1, 1); // CBPCY 0
            w.bit(0);       // ACPRED
            for (int k = 0; k < 4; k++) {
                const int bx = 2 * mx + (k & 1), by = 2 * my + (k >> 1);
                const int dc = p.y[static_cast<size_t>(by * columns + bx)];
                put_dc_diff(w, dc - dc_predictor(p.y, columns, bx, by), false, table1);
            }
            for (const std::vector<int>* plane : {&p.cb, &p.cr}) {
                const int dc = (*plane)[static_cast<size_t>(my * mbw + mx)];
                put_dc_diff(w, dc - dc_predictor(*plane, mbw, mx, my), true, table1);
            }
        }
}

// An I field (or an I picture's fields after its FIELDTX): from PQINDEX to its macroblocks.
inline void intra_body(BitWriter& w, const DcPicture& p, const Sequence& s, const Entry& e, const Header& h,
    bool fieldtx_plane)
{
    quantizer_fields(w, s, e, h);
    if (fieldtx_plane) flat_bitplane(w, false, p.width / 16, p.height / 16, h.bitplane); // FIELDTX: frame transforms
    w.bit(0);       // ACPRED bitplane: INVERT 0,
    w.bits(0x0, 4); // IMODE raw
    if (e.overlap && pquant(h.pqindex, e.quantizer) <= 8) code012(w, h.condover);
    code012(w, h.transacfrm);
    code012(w, h.transacfrm2);
    w.bit(h.transdctab);
    vopdquant(w, e, h);
    intra_macroblocks(w, p, h.transdctab);
}

// A P field: from PQINDEX to its macroblocks, each 1MV with no vector data and no coefficients (MBMODETAB 0's code
// '1': the reference field's samples, its vector predicted from its neighbours', all 0).
inline void p_field_body(BitWriter& w, int mbw, int mbh, const Sequence& s, const Entry& e, const Header& h)
{
    quantizer_fields(w, s, e, h);
    w.bit(h.numref);
    if (!h.numref) w.bit(h.reffield);
    if (e.extended_mv) unary3(w, h.mvrange);
    if (e.extended_dmv) unary3(w, h.dmvrange);
    const bool low = pquant(h.pqindex, e.quantizer) <= 12;
    mvmode_code(w, kOneMv, low, false);
    w.bits(0, 3);                                         // MBMODETAB 0
    w.bits(static_cast<uint32_t>(h.imvtab), 2 + h.numref); // IMVTAB
    w.bits(static_cast<uint32_t>(h.icbptab), 3);           // ICBPTAB
    vopdquant(w, e, h);
    transform_type(w, e, h);
    code012(w, h.transacfrm);
    w.bit(h.transdctab);
    for (int i = 0; i < mbw * mbh; i++) w.bit(1); // MBMODE: 1MV, no MVDATA, no CBPCY
}

// A B field: from PQINDEX to its macroblocks, each forward (the FORWARDMB plane all ones), 1MV with no vector data
// and no coefficients (MBMODETAB 0's '1').
inline void b_field_body(BitWriter& w, int mbw, int mbh, const Sequence& s, const Entry& e, const Header& h)
{
    quantizer_fields(w, s, e, h);
    if (e.extended_mv) unary3(w, h.mvrange);
    if (e.extended_dmv) unary3(w, h.dmvrange);
    const bool low = pquant(h.pqindex, e.quantizer) <= 12;
    mvmode_code(w, kOneMv, low, true);               // MVMODE (MVMODE2's table): 1MV
    flat_bitplane(w, true, mbw, mbh, h.bitplane);    // FORWARDMB: every macroblock
    w.bits(0, 3);                                    // MBMODETAB 0
    w.bits(static_cast<uint32_t>(h.imvtab & 7), 3); // IMVTAB
    w.bits(static_cast<uint32_t>(h.icbptab), 3);    // ICBPTAB
    vopdquant(w, e, h);
    transform_type(w, e, h);
    code012(w, h.transacfrm);
    w.bit(h.transdctab);
    for (int i = 0; i < mbw * mbh; i++) w.bit(1); // MBMODE: 1MV, no MVDATA, no CBPCY (forward)
}

} // namespace detail

// An interlaced frame's intra picture (FCM '10', 7.1): I, or BI when `bi`; its FIELDTX plane all frame transforms, its
// macroblocks as the progressive one's.
inline std::vector<uint8_t> interlaced_intra_frame(const DcPicture& p, const Sequence& s, const Entry& e,
    const Header& h, bool bi = false)
{
    BitWriter w;
    w.bits(0x2, 2); // FCM: interlaced frame
    if (bi) w.bits(0xE, 4);
    else w.bits(0x6, 3);
    detail::header_start(w, s, e, h, false);
    detail::intra_body(w, p, s, e, h, true);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// An interlaced frame's P picture of `mbw` x `mbh` macroblocks, every one skipped.
inline std::vector<uint8_t> interlaced_skipped_p_frame(const Sequence& s, const Entry& e, const Header& h, int mbw,
    int mbh)
{
    BitWriter w;
    w.bits(0x2, 2); // FCM
    w.bit(0);       // PTYPE: P
    detail::header_start(w, s, e, h, false);
    detail::quantizer_fields(w, s, e, h);
    if (e.extended_mv) detail::unary3(w, h.mvrange);
    if (e.extended_dmv) detail::unary3(w, h.dmvrange);
    w.bit(h.fourmvswitch);
    w.bit(h.intcomp);
    if (h.intcomp) {
        w.bits(static_cast<uint32_t>(h.lumscale), 6);
        w.bits(static_cast<uint32_t>(h.lumshift), 6);
    }
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // SKIPMB: every macroblock
    w.bits(static_cast<uint32_t>(h.mbmodetab), 2);
    w.bits(static_cast<uint32_t>(h.imvtab), 2);
    w.bits(static_cast<uint32_t>(h.icbptab), 3);
    w.bits(static_cast<uint32_t>(h.twomvbptab), 2);
    if (h.fourmvswitch) w.bits(static_cast<uint32_t>(h.fourmvbptab), 2);
    detail::vopdquant(w, e, h);
    detail::transform_type(w, e, h);
    detail::code012(w, h.transacfrm);
    w.bit(h.transdctab);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// An interlaced frame's B picture of `mbw` x `mbh` macroblocks, every one skipped and direct (BFRACTION after the
// quantizer's fields, as FFmpeg's vc1.c reads it).
inline std::vector<uint8_t> interlaced_direct_b_frame(const Sequence& s, const Entry& e, const Header& h, int mbw,
    int mbh)
{
    BitWriter w;
    w.bits(0x2, 2); // FCM
    w.bits(0x2, 2); // PTYPE: B
    detail::header_start(w, s, e, h, false);
    detail::quantizer_fields(w, s, e, h);
    if (h.bfraction < 7) {
        w.bits(static_cast<uint32_t>(h.bfraction), 3);
    } else {
        w.bits(0x7, 3);
        w.bits(static_cast<uint32_t>(h.bfraction - 7), 4);
    }
    if (e.extended_mv) detail::unary3(w, h.mvrange);
    if (e.extended_dmv) detail::unary3(w, h.dmvrange);
    w.bit(0);                            // INTCOMP
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // DIRECTMB: every macroblock
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // SKIPMB: every macroblock
    w.bits(static_cast<uint32_t>(h.mbmodetab), 2);
    w.bits(static_cast<uint32_t>(h.imvtab), 2);
    w.bits(static_cast<uint32_t>(h.icbptab), 3);
    w.bits(static_cast<uint32_t>(h.twomvbptab), 2);
    w.bits(static_cast<uint32_t>(h.fourmvbptab), 2);
    detail::vopdquant(w, e, h);
    detail::transform_type(w, e, h);
    detail::code012(w, h.transacfrm);
    w.bit(h.transdctab);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// A field picture (FCM '11'): FPTYPE 0 I/I, 1 I/P, 2 P/I, 3 P/P, 4 B/B, 5 B/BI, 6 BI/B, 7 BI/BI; the frame BDU (its
// header and the first field), then the second field's FIELD BDU. An I or BI field is `first`'s or `second`'s DC
// levels (the frame's width, half its height); a P field, every macroblock the reference field's samples
// (detail::p_field_body); a B field, every macroblock the forward reference field's (detail::b_field_body).
inline std::vector<uint8_t> field_picture(int fptype, const DcPicture& first, const DcPicture& second,
    const Sequence& s, const Entry& e, const Header& h)
{
    const bool b_pair = fptype & 4, other1 = fptype & 2, other2 = fptype & 1;
    BitWriter w;
    w.bits(0x3, 2); // FCM: field picture
    w.bits(static_cast<uint32_t>(fptype), 3);
    detail::header_start(w, s, e, h, false);
    if (e.refdist && !b_pair) { // REFDIST: 0 to 2 in two bits, 3 on in a unary code after '11'
        if (h.refdist < 3) {
            w.bits(static_cast<uint32_t>(h.refdist), 2);
        } else {
            w.bits(0x3, 2);
            for (int i = 3; i < h.refdist; i++) w.bit(1);
            w.bit(0);
        }
    }
    if (b_pair) { // BFRACTION
        if (h.bfraction < 7) {
            w.bits(static_cast<uint32_t>(h.bfraction), 3);
        } else {
            w.bits(0x7, 3);
            w.bits(static_cast<uint32_t>(h.bfraction - 7), 4);
        }
    }
    const int mbw = s.width / 16, mbh = (s.height / 2 + 15) / 16;
    // A field's body: P (an I/P pair's other), B (a B/BI pair's not other), or intra.
    const auto body = [&](BitWriter& bw, bool other, const DcPicture& p) {
        if (b_pair && !other) detail::b_field_body(bw, mbw, mbh, s, e, h);
        else if (!b_pair && other) detail::p_field_body(bw, mbw, mbh, s, e, h);
        else detail::intra_body(bw, p, s, e, h, false);
    };
    body(w, other1, first);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    BitWriter w2;
    body(w2, other2, second);
    w2.trailing();
    put_bdu(out, kField, w2.take());
    return out;
}

// A progressive I picture in slices: the frame BDU with its header and the macroblock rows before slice_rows[0], then
// a SLICE BDU at each of those rows (SLICE_ADDR, PIC_HEADER_FLAG 0) with its rows; the DC predictors of a slice's
// first row have no row above.
inline std::vector<uint8_t> intra_frame_slices(const DcPicture& p, const Sequence& s, const Entry& e, const Header& h,
    const std::vector<int>& slice_rows)
{
    const int mbw = p.width / 16, mbh = p.height / 16, columns = p.width / 8;
    const auto rows = [&](BitWriter& w, int from, int to) {
        for (int my = from; my < to; my++)
            for (int mx = 0; mx < mbw; mx++) {
                w.bits(0x1, 1); // CBPCY 0
                w.bit(0);       // ACPRED
                for (int k = 0; k < 4; k++) {
                    const int bx = 2 * mx + (k & 1), by = 2 * my + (k >> 1);
                    const int dc = p.y[static_cast<size_t>(by * columns + bx)];
                    detail::put_dc_diff(w, dc - detail::dc_predictor(p.y, columns, bx, by, 2 * from), false,
                        h.transdctab);
                }
                for (const std::vector<int>* plane : {&p.cb, &p.cr}) {
                    const int dc = (*plane)[static_cast<size_t>(my * mbw + mx)];
                    detail::put_dc_diff(w, dc - detail::dc_predictor(*plane, mbw, mx, my, from), true, h.transdctab);
                }
            }
    };
    BitWriter w;
    if (s.interlace) w.bit(0);
    w.bits(0x6, 3); // PTYPE: I
    detail::header_start(w, s, e, h, false);
    if (s.finterpflag) w.bit(h.interpfrm);
    detail::quantizer_fields(w, s, e, h);
    w.bit(0);       // ACPRED bitplane: INVERT 0,
    w.bits(0x0, 4); // IMODE raw
    if (e.overlap && detail::pquant(h.pqindex, e.quantizer) <= 8) detail::code012(w, h.condover);
    detail::code012(w, h.transacfrm);
    detail::code012(w, h.transacfrm2);
    w.bit(h.transdctab);
    detail::vopdquant(w, e, h);
    std::vector<int> starts = slice_rows;
    starts.push_back(mbh);
    rows(w, 0, starts[0]);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    for (size_t i = 0; i + 1 < starts.size(); i++) {
        BitWriter ws;
        ws.bits(static_cast<uint32_t>(starts[i]), 9); // SLICE_ADDR
        ws.bit(0);                                     // PIC_HEADER_FLAG
        rows(ws, starts[i], starts[i + 1]);
        ws.trailing();
        put_bdu(out, kSlice, ws.take());
    }
    return out;
}

// A user data BDU (frame level, 0x1D, or another suffix): its bytes as they are, then the stop bit's byte.
inline std::vector<uint8_t> user_data(const std::vector<uint8_t>& bytes, uint8_t suffix = kFrameUserData)
{
    std::vector<uint8_t> payload = bytes;
    payload.push_back(0x80);
    std::vector<uint8_t> out;
    put_bdu(out, suffix, payload);
    return out;
}

// ---- The simple and main profiles: no start codes; their sequence layer's STRUCT_C (Annex J), then each frame's
// bytes (7.1, its header and macroblocks), zeros to the byte.

struct SimpleSequence {
    int profile = 1; // PROFILE: 0 simple, 1 main
    int frmrtq = 0, bitrtq = 0;
    bool loopfilter = false, multires = false, fastuvmc = true, extended_mv = false;
    int dquant = 0;
    bool vstransform = false, overlap = false, syncmarker = false, rangered = false;
    int maxbframes = 0, quantizer = 0;
    bool finterpflag = false;
    int width = 64, height = 48; // not STRUCT_C's: SetPictureSize's
};

// STRUCT_C (32 bits): PROFILE and its two reserved bits, the post-processing hints, the coding tools (RES_X8 0,
// RES_FASTTX 1, RES_TRANSTAB 0), MAXBFRAMES, QUANTIZER, FINTERPFLAG, RES_RTM_FLAG 1 (as FFmpeg's vc1.c reads it).
inline std::vector<uint8_t> struct_c(const SimpleSequence& s)
{
    BitWriter w;
    w.bits(static_cast<uint32_t>(s.profile), 2);
    w.bits(0, 2);
    w.bits(static_cast<uint32_t>(s.frmrtq), 3);
    w.bits(static_cast<uint32_t>(s.bitrtq), 5);
    w.bit(s.loopfilter);
    w.bit(0);
    w.bit(s.multires);
    w.bit(1);
    w.bit(s.fastuvmc);
    w.bit(s.extended_mv);
    w.bits(static_cast<uint32_t>(s.dquant), 2);
    w.bit(s.vstransform);
    w.bit(0);
    w.bit(s.overlap);
    w.bit(s.syncmarker);
    w.bit(s.rangered);
    w.bits(static_cast<uint32_t>(s.maxbframes), 3);
    w.bits(static_cast<uint32_t>(s.quantizer), 2);
    w.bit(s.finterpflag);
    w.bit(1);
    return w.take();
}

// A simple or main profile frame's header fields.
struct SimpleHeader {
    bool interpfrm = false; // with FINTERPFLAG
    int frmcnt = 0;
    bool rangeredfrm = false; // with RANGERED
    int bf = 0;               // BF: I and BI pictures
    int pqindex = 4;
    bool halfqp = false, pquantizer = false;
    int mvrange = 0, respic = 0; // with EXTENDED_MV; with MULTIRES (not B)
    int bfraction = 0;           // B pictures (22: a BI picture)
    int mvmode = kOneMv;         // P pictures (Header's), intensity compensation with LUMSCALE and LUMSHIFT
    bool intensity = false;
    int lumscale = 32, lumshift = 0;
    bool b_hpel_bilinear = false;
    int mvtab = 0, cbptab = 0;
    bool dquantfrm = false; // VOPDQUANT (with DQUANT): Header's fields
    int dqprofile = 0, dqedge = 0, pqdiff = 0, abspq = 1;
    bool dqbilevel = true;
    bool ttmbf = true;
    int ttfrm = 0;
    int transacfrm = 0, transacfrm2 = 0;
    bool transdctab = false;
    int bitplane = kRowSkip;
};

namespace detail {

// The fields up to PQINDEX's: INTERPFRM, FRMCNT, RANGEREDFRM, PTYPE ('1' P, then '0' I without B pictures, '01' I and
// '00' B with them), BFRACTION, BF; then the quantizer's, MVRANGE and RESPIC.
inline void simple_header(BitWriter& w, const SimpleSequence& s, const SimpleHeader& h, int type)
{
    if (s.finterpflag) w.bit(h.interpfrm);
    w.bits(static_cast<uint32_t>(h.frmcnt), 2);
    if (s.rangered) w.bit(h.rangeredfrm);
    if (type == 2) {
        w.bit(1);
    } else if (!s.maxbframes) {
        w.bit(0);
    } else {
        w.bit(0);
        w.bit(type == 1); // '01' I, '00' B
    }
    if (type == 3 || type == 4) { // B, or BI (BFRACTION 22)
        const int index = type == 4 ? 22 : h.bfraction;
        if (index < 7) {
            w.bits(static_cast<uint32_t>(index), 3);
        } else {
            w.bits(0x7, 3);
            w.bits(static_cast<uint32_t>(index - 7), 4);
        }
    }
    if (type == 1 || type == 4) w.bits(static_cast<uint32_t>(h.bf), 7);
    w.bits(static_cast<uint32_t>(h.pqindex), 5);
    if (h.pqindex <= 8) w.bit(h.halfqp);
    if (s.quantizer == 1) w.bit(h.pquantizer);
    if (s.extended_mv) unary3(w, h.mvrange);
    if (s.multires && type != 3) w.bits(static_cast<uint32_t>(h.respic), 2);
}

inline Header as_header(const SimpleHeader& h)
{
    Header g;
    g.dquantfrm = h.dquantfrm;
    g.dqprofile = h.dqprofile;
    g.dqedge = h.dqedge;
    g.dqbilevel = h.dqbilevel;
    g.pqdiff = h.pqdiff;
    g.abspq = h.abspq;
    g.ttmbf = h.ttmbf;
    g.ttfrm = h.ttfrm;
    return g;
}

// The simple and main profiles' DC predictor: as the advanced profile's, the blocks outside the picture of DC level
// `outer` (1024 over the DC step, rounded: 128 at PQUANT 4).
inline int simple_dc_predictor(const std::vector<int>& plane, int columns, int bx, int by, int outer)
{
    const bool a_ok = by > 0, c_ok = bx > 0;
    const int a = a_ok ? plane[static_cast<size_t>((by - 1) * columns + bx)] : outer;
    const int c = c_ok ? plane[static_cast<size_t>(by * columns + bx - 1)] : outer;
    const int b = a_ok && c_ok ? plane[static_cast<size_t>((by - 1) * columns + bx - 1)] : outer;
    return std::abs(a - b) <= std::abs(b - c) ? c : a;
}

} // namespace detail

// A simple or main profile intra frame (I, or BI when `bi`): every macroblock CBPCY 0, ACPRED 0, its six DC
// differentials; `p`'s levels the DC levels themselves (no 128 added: 114 about mid-grey at PQUANT 4).
inline std::vector<uint8_t> simple_intra_frame(const DcPicture& p, const SimpleSequence& s, const SimpleHeader& h,
    bool bi = false)
{
    BitWriter w;
    detail::simple_header(w, s, h, bi ? 4 : 1);
    detail::code012(w, h.transacfrm);
    detail::code012(w, h.transacfrm2);
    w.bit(h.transdctab);
    const int mbw = p.width / 16, mbh = p.height / 16, columns = p.width / 8, outer = 128;
    for (int my = 0; my < mbh; my++)
        for (int mx = 0; mx < mbw; mx++) {
            w.bits(0x1, 1); // CBPCY 0
            w.bit(0);       // ACPRED
            for (int k = 0; k < 4; k++) {
                const int bx = 2 * mx + (k & 1), by = 2 * my + (k >> 1);
                const int dc = p.y[static_cast<size_t>(by * columns + bx)];
                detail::put_dc_diff(w, dc - detail::simple_dc_predictor(p.y, columns, bx, by, outer), false,
                    h.transdctab);
            }
            for (const std::vector<int>* plane : {&p.cb, &p.cr}) {
                const int dc = (*plane)[static_cast<size_t>(my * mbw + mx)];
                detail::put_dc_diff(w, dc - detail::simple_dc_predictor(*plane, mbw, mx, my, outer), true,
                    h.transdctab);
            }
        }
    w.align_zero();
    return w.take();
}

// A simple or main profile P frame of `mbw` x `mbh` macroblocks, every one skipped.
inline std::vector<uint8_t> simple_skipped_mbs_p_frame(const SimpleSequence& s, const SimpleHeader& h, int mbw,
    int mbh)
{
    BitWriter w;
    detail::simple_header(w, s, h, 2);
    const bool low = (s.quantizer == 0 ? detail::pquant(h.pqindex, 0) : h.pqindex) <= 12;
    if (h.intensity) {
        w.bits(0x1, 4);
        detail::mvmode_code(w, h.mvmode, low, true);
        w.bits(static_cast<uint32_t>(h.lumscale), 6);
        w.bits(static_cast<uint32_t>(h.lumshift), 6);
    } else {
        detail::mvmode_code(w, h.mvmode, low, false);
    }
    if (h.mvmode == kMixedMv) detail::flat_bitplane(w, false, mbw, mbh, h.bitplane);
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane);
    w.bits(static_cast<uint32_t>(h.mvtab), 2);
    w.bits(static_cast<uint32_t>(h.cbptab), 2);
    Entry e;
    e.dquant = s.dquant;
    e.vstransform = s.vstransform;
    const Header g = detail::as_header(h);
    detail::vopdquant(w, e, g);
    detail::transform_type(w, e, g);
    detail::code012(w, h.transacfrm);
    w.bit(h.transdctab);
    w.align_zero();
    return w.take();
}

// A main profile B frame (MAXBFRAMES above 0) of `mbw` x `mbh` macroblocks, every one skipped and direct.
inline std::vector<uint8_t> simple_direct_b_frame(const SimpleSequence& s, const SimpleHeader& h, int mbw, int mbh)
{
    BitWriter w;
    detail::simple_header(w, s, h, 3);
    w.bit(!h.b_hpel_bilinear);
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // DIRECTMB
    detail::flat_bitplane(w, true, mbw, mbh, h.bitplane); // SKIPMB
    w.bits(static_cast<uint32_t>(h.mvtab), 2);
    w.bits(static_cast<uint32_t>(h.cbptab), 2);
    Entry e;
    e.dquant = s.dquant;
    e.vstransform = s.vstransform;
    const Header g = detail::as_header(h);
    detail::vopdquant(w, e, g);
    detail::transform_type(w, e, g);
    detail::code012(w, h.transacfrm);
    w.bit(h.transdctab);
    w.align_zero();
    return w.take();
}

// The samples a simple or main profile intra frame of DC levels decodes to (no 128 added).
inline std::vector<uint8_t> simple_samples(const DcPicture& p)
{
    const auto value = [](int dc) { return std::clamp((12 * ((12 * dc * kDcStep + 4) >> 3) + 64) >> 7, 0, 255); };
    std::vector<uint8_t> out(static_cast<size_t>(p.width * p.height * 3 / 2));
    const int columns = p.width / 8, mbw = p.width / 16;
    for (int r = 0; r < p.height; r++)
        for (int x = 0; x < p.width; x++)
            out[static_cast<size_t>(r * p.width + x)] =
                static_cast<uint8_t>(value(p.y[static_cast<size_t>(r / 8 * columns + x / 8)]));
    const size_t u = static_cast<size_t>(p.width * p.height), v = u + u / 4;
    for (int r = 0; r < p.height / 2; r++)
        for (int x = 0; x < p.width / 2; x++) {
            const size_t at = static_cast<size_t>(r * (p.width / 2) + x), mb = static_cast<size_t>(r / 8 * mbw + x / 8);
            out[u + at] = static_cast<uint8_t>(value(p.cb[mb]));
            out[v + at] = static_cast<uint8_t>(value(p.cr[mb]));
        }
    return out;
}

// The end of a sequence (an end-of-sequence BDU, no payload).
inline std::vector<uint8_t> end_of_sequence()
{
    return {0x00, 0x00, 0x01, kEndOfSequence};
}

// The samples an intra picture of DC levels decodes to (YUV 4:2:0 planes without padding: Y, then U, then V).
inline std::vector<uint8_t> samples(const DcPicture& p)
{
    std::vector<uint8_t> out(static_cast<size_t>(p.width * p.height * 3 / 2));
    const int columns = p.width / 8, mbw = p.width / 16;
    for (int r = 0; r < p.height; r++)
        for (int x = 0; x < p.width; x++) {
            const int dc = p.y[static_cast<size_t>(r / 8 * columns + x / 8)];
            out[static_cast<size_t>(r * p.width + x)] = static_cast<uint8_t>(block_value(dc));
        }
    const size_t u = static_cast<size_t>(p.width * p.height), v = u + u / 4;
    for (int r = 0; r < p.height / 2; r++)
        for (int x = 0; x < p.width / 2; x++) {
            const size_t at = static_cast<size_t>(r * (p.width / 2) + x), mb = static_cast<size_t>(r / 8 * mbw + x / 8);
            out[u + at] = static_cast<uint8_t>(block_value(p.cb[mb]));
            out[v + at] = static_cast<uint8_t>(block_value(p.cr[mb]));
        }
    return out;
}

} // namespace pspocket::hle::test::vc1
