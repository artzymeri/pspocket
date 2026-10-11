#pragma once

// pspocket's baseline JPEG encoder for cellJpgEnc (D-615), written here from ITU-T T.81 (the JPEG standard: the
// forward DCT of annex A.3.3, quantisation, the zigzag order of figure A.6, the Huffman coding of DC differences and
// AC run/size pairs of annex F.1.2 with the code tables built as annex C builds them, the marker segments of annex B,
// restart intervals) with its annex K tables (the example quantisation tables and the typical Huffman tables), and
// ITU-T T.871 (JFIF: the APP0 segment, and YCbCr from RGB). No other encoder's code is copied or linked.
// What it writes: SOI, a JFIF APP0 (version 1.01, no density, no thumbnail), DQT, SOF0 (8-bit samples), DHT, DRI when
// there are restart markers, one interleaved scan of every component, EOI. A gray JPEG has one component (id 1);
// a colour one Y, Cb and Cr (ids 1 to 3), the chroma at 1x1 and the luma at the sampling's factors. Edges are padded
// by repeating the last column and row, chroma is subsampled as the mean of the luma samples it covers.

#include <array>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::jpeg {

// A colour JPEG's chroma sampling: the luma's sampling factors (horizontal x vertical) against the chroma's 1x1.
enum class Sampling { YCbCr444, YCbCr422, YCbCr420, YCbCr411 }; // 1x1, 2x1, 2x2, 4x1

// What to encode: each component's samples at the picture's full size, rows `width` bytes apart.
struct Planes {
    u32 width = 0, height = 0;
    int components = 1;        // 1: a gray JPEG (y only); 3: YCbCr
    std::vector<u8> y, cb, cr; // width x height samples each (cb and cr for 3 components)
};

struct EncodeSettings {
    int quality = 75;                       // 1 to 100: annex K's tables scaled (as libjpeg scales them)
    Sampling sampling = Sampling::YCbCr420; // a colour JPEG's
    bool fast = false;                      // the DCT in single precision (cellJpgEnc's fast method), else double
    u32 restart_interval = 0;               // MCUs between restart markers (a DRI segment); 0: none
};

// The JFIF stream of `planes`, or nothing when they cannot be one: a width or height of 0 or past 65535 (SOF's
// 16-bit fields), a plane with fewer samples than width x height, a restart interval past 65535 (DRI's field).
std::vector<u8> encode(const Planes& planes, const EncodeSettings& settings);

// Annex K's example table for the luma (table K.1) or the chroma (K.2) scaled for `quality` (clamped to 1-100):
// 5000 / quality percent below 50, 200 - 2 x quality from 50, each entry rounded and kept within 1-255 (baseline's
// 8-bit tables). In natural order (row by row), not zigzag.
std::array<u8, 64> quant_table(bool luma, int quality);

// T.871 (section 7): Y, Cb and Cr from R, G and B, each rounded and kept within 0-255.
void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr);
u8 rgb_to_y(u8 r, u8 g, u8 b);

} // namespace pspocket::hle::jpeg
