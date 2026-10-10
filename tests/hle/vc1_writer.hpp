#pragma once

// A minimal VC-1 writer for tests (synthetic video, no game data, D-589): SMPTE 421M's advanced profile in its start
// code framing (a sequence header, an entry point, frames), progressive, with two kinds of picture:
// - intra pictures whose blocks carry only their DC coefficient (every macroblock's CBPCY 0, no AC prediction), so
//   each 8x8 block decodes flat to a value known from its DC level (block_value);
// - skipped P pictures (PTYPE 1111), which repeat the picture before them.
// The few variable length codes used are the standard's (the I picture CBPCY table, the DC differential tables of
// TRANSDCTAB 0, the bitplane IMODE table), as FFmpeg's msmpeg4_vc1_data.c and vc1_vlc_data.h hold them.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "h264_writer.hpp" // BitWriter

namespace pspocket::hle::test::vc1 {

using h264::BitWriter;

// The start code suffixes of the bitstream data units (Annex E).
constexpr uint8_t kSequenceHeader = 0x0F, kEntryPoint = 0x0E, kFrame = 0x0D, kEndOfSequence = 0x0A;

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
    bool pulldown = false, tfcntrflag = false;
    // DISPLAY_EXT with its display size, when display_width is not 0; then each of these when given:
    int display_width = 0, display_height = 0;
    int aspect_ratio = 0, aspect_width = 0, aspect_height = 0; // ASPECT_RATIO (15: ASPECT_HORIZ_SIZE, ..._VERT_SIZE)
    int framerate_nr = 0, framerate_dr = 0;                    // FRAMERATENR, FRAMERATEDR (FRAMERATEIND 0)
    int framerate_exp = -1;                                    // FRAMERATEEXP (FRAMERATEIND 1) when not negative
    int color_prim = 0, transfer_char = 0, matrix_coef = 0;    // COLOR_FORMAT_FLAG when color_prim is not 0
};

// The sequence header BDU (6.1): 4:2:0, progressive, no post-processing hints, no HRD.
inline std::vector<uint8_t> sequence_header(const Sequence& s)
{
    BitWriter w;
    w.bits(3, 2); // PROFILE: advanced
    w.bits(static_cast<uint32_t>(s.level), 3);
    w.bits(1, 2); // COLORDIFF_FORMAT: 4:2:0
    w.bits(0, 3); // FRMRTQ_POSTPROC
    w.bits(0, 5); // BITRTQ_POSTPROC
    w.bit(0);     // POSTPROCFLAG
    w.bits(static_cast<uint32_t>(s.width / 2 - 1), 12);
    w.bits(static_cast<uint32_t>(s.height / 2 - 1), 12);
    w.bit(s.pulldown);
    w.bit(0); // INTERLACE
    w.bit(s.tfcntrflag);
    w.bit(0); // FINTERPFLAG
    w.bit(1); // reserved
    w.bit(0); // PSF
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
    w.bit(0); // HRD_PARAM_FLAG
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kSequenceHeader, w.take());
    return out;
}

struct Entry {
    bool broken_link = false, closed_entry = true;
    int range_mapy = -1, range_mapuv = -1; // RANGE_MAPY_FLAG and RANGE_MAPUV_FLAG with their values when not negative
};

// The entry point BDU (6.2): no loop filter, overlap, dquant or variable-size transform, the implicit quantizer, the
// sequence's size.
inline std::vector<uint8_t> entry_point(const Entry& e)
{
    BitWriter w;
    w.bit(e.broken_link);
    w.bit(e.closed_entry);
    w.bit(0);     // PANSCAN_FLAG
    w.bit(0);     // REFDIST_FLAG
    w.bit(0);     // LOOPFILTER
    w.bit(0);     // FASTUVMC
    w.bit(0);     // EXTENDED_MV
    w.bits(0, 2); // DQUANT
    w.bit(0);     // VSTRANSFORM
    w.bit(0);     // OVERLAP
    w.bits(0, 2); // QUANTIZER: implicit
    w.bit(0);     // CODED_SIZE_FLAG
    w.bit(e.range_mapy >= 0);
    if (e.range_mapy >= 0) w.bits(static_cast<uint32_t>(e.range_mapy), 3);
    w.bit(e.range_mapuv >= 0);
    if (e.range_mapuv >= 0) w.bits(static_cast<uint32_t>(e.range_mapuv), 3);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kEntryPoint, w.take());
    return out;
}

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
// is further from; 0 for the picture's first block.
inline int dc_predictor(const std::vector<int>& plane, int columns, int bx, int by)
{
    const bool a_ok = by > 0, c_ok = bx > 0;
    const int a = a_ok ? plane[static_cast<size_t>((by - 1) * columns + bx)] : 0;
    const int c = c_ok ? plane[static_cast<size_t>(by * columns + bx - 1)] : 0;
    const int b = a_ok && c_ok ? plane[static_cast<size_t>((by - 1) * columns + bx - 1)] : 0;
    if (c_ok && (!a_ok || std::abs(a - b) <= std::abs(b - c))) return c;
    return a_ok ? a : 0;
}

// DCDIFF with TRANSDCTAB 0 (PQUANT above 2: no extra bits): 0's code, else the escape code, 8 bits and the sign.
inline void put_dc_diff(BitWriter& w, int diff, bool chroma)
{
    if (diff == 0) {
        if (chroma) w.bits(0x0, 2);
        else w.bits(0x1, 1);
        return;
    }
    if (chroma) w.bits(0x60888F, 23);
    else w.bits(0x6078F, 24);
    w.bits(static_cast<uint32_t>(std::abs(diff)), 8);
    w.bit(diff < 0);
}

} // namespace detail

// A progressive I picture's frame BDU (7.1): every macroblock CBPCY 0 and its six DC differentials.
inline std::vector<uint8_t> intra_frame(const DcPicture& p, const Sequence& s, int rptfrm = 0)
{
    BitWriter w;
    w.bits(0x6, 3); // PTYPE: I
    if (s.tfcntrflag) w.bits(0, 8);
    if (s.pulldown) w.bits(static_cast<uint32_t>(rptfrm), 2); // RPTFRM (progressive)
    w.bit(0);       // RNDCTRL
    w.bits(static_cast<uint32_t>(kPqIndex), 5);
    w.bit(0);       // HALFQP
    w.bit(0);       // ACPRED bitplane: INVERT 0,
    w.bits(0x0, 4); // IMODE raw: each macroblock's bit in its own layer
    w.bit(0);       // TRANSACFRM
    w.bit(0);       // TRANSACFRM2
    w.bit(0);       // TRANSDCTAB
    const int mbw = p.width / 16, mbh = p.height / 16, columns = p.width / 8;
    for (int my = 0; my < mbh; my++)
        for (int mx = 0; mx < mbw; mx++) {
            w.bits(0x1, 1); // CBPCY 0
            w.bit(0);       // ACPRED
            for (int k = 0; k < 4; k++) {
                const int bx = 2 * mx + (k & 1), by = 2 * my + (k >> 1);
                const int dc = p.y[static_cast<size_t>(by * columns + bx)];
                detail::put_dc_diff(w, dc - detail::dc_predictor(p.y, columns, bx, by), false);
            }
            for (const std::vector<int>* plane : {&p.cb, &p.cr}) {
                const int dc = (*plane)[static_cast<size_t>(my * mbw + mx)];
                detail::put_dc_diff(w, dc - detail::dc_predictor(*plane, mbw, mx, my), true);
            }
        }
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
}

// A skipped P picture's frame BDU: PTYPE 1111, the picture before it again.
inline std::vector<uint8_t> skipped_frame(const Sequence& s)
{
    BitWriter w;
    w.bits(0xF, 4);
    if (s.tfcntrflag) w.bits(0, 8);
    if (s.pulldown) w.bits(0, 2);
    w.trailing();
    std::vector<uint8_t> out;
    put_bdu(out, kFrame, w.take());
    return out;
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
