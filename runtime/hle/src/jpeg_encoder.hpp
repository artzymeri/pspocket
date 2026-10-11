#pragma once

// pspocket's baseline JPEG encoder for cellJpgEnc (D-615, D-913): the stream Sony's libjpgenc writes, which is IJG's
// libjpeg 6b's (27-Mar-1998). Written here from libjpeg 6b's algorithms (the Independent JPEG Group's software, whose
// licence lets its methods be used with this acknowledgment; no code of Sony's module) and ITU-T T.81 (annex K's tables):
// - jpeg_set_defaults, jpeg_set_colorspace and jpeg_set_quality: gray (component 1), RGB (components 'R', 'G', 'B',
//   all on table 0) or YCbCr (1, 2, 3; the chroma on table 1 and Huffman tables 1); annex K's quantisation tables
//   scaled by libjpeg's quality curve, baseline (8-bit entries); annex K's Huffman tables.
// - jccolor.c's conversions in 16-bit fixed point (RGB to YCbCr, RGB to gray), jcsample.c's downsampling (h2v1 and
//   h2v2 with their alternating rounding bias, the general one for 4:1:1), jcprepct.c's edge expansion (the last column
//   and row repeated), jccoefct.c's dummy blocks past the image (zero, the DC of the block before).
// - jcdctmgr.c with jfdctfst.c (the fast integer DCT and its scaled divisors) or jfdctflt.c (the float DCT), as
//   libjpgenc builds it: FAST_FLOAT double, the PPU's fused multiply-adds where its compiler made them (two in the
//   transform, the quantisation's).
// - libjpgenc's SPU encoder (Dct::Spu), read from its SPU program: RGB to YCbCr in single-precision floats, a DCT
//   factored by tangents, quantisation by reciprocals refined from the SPU's estimate, all in the SPU's single
//   precision (results truncated, pspocket/spu/fp.h); past the picture's right edge black (the SPU's row buffers hold
//   only the picture's columns: what lies past them is whatever its local store held, here as when it held zeros).
// - jchuff.c's encoding (24-bit bit buffer, 0xFF stuffed, restart markers, the last byte filled with ones) and
//   jcmarker.c's markers: SOI, (JFIF APP0 or Adobe APP14 when asked: libjpgenc asks neither), the application's markers
//   (jpeg_write_marker), at the first row DQT for each table used, SOF0, DHT for each table as a component of the scan
//   first uses it, DRI when there is a restart interval, SOS; EOI.
// The stream goes to a sink a write at a time as libjpeg's destination manager gets it: a sink that refuses stops the
// encoder there (libjpgenc's buffer full: the rows encoded before it count as done).

#include <array>
#include <functional>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::jpeg {

enum class Space : u8 { Gray = 1, RGB = 2, YCbCr = 3 };
// libjpgenc's QUALITY (JDCT_FLOAT) and FAST (JDCT_IFAST) on the PPU; Spu: FAST on the SPUs, libjpgenc's SPU encoder
// (its colour conversion, DCT and quantisation in the SPU's single precision; see jpeg_encoder.cpp).
enum class Dct : u8 { Float, Fast, Spu };

struct Settings {
    u32 width = 0, height = 0;
    Space input = Space::RGB;    // the rows given: gray (a byte a pixel), RGB or YCbCr (three)
    Space output = Space::YCbCr; // the JPEG's colour space
    int h_samp = 2, v_samp = 2;  // a YCbCr JPEG's luma sampling factors (the chroma's are 1x1)
    int quality = 75;            // jpeg_set_quality's (at most 0 is 1, past 100 is 100), forced to baseline
    Dct dct = Dct::Float;
    u32 restart_interval = 0; // MCUs between restart markers; 0: none
    bool jfif = false, adobe = false;
};

// Where the stream goes, a piece at a time; false: it takes no more (the encoder stops).
using Sink = std::function<bool(const u8*, usize)>;

class Encoder {
public:
    Encoder(const Settings& s, Sink sink);
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // False when the settings cannot make a JPEG (a size of 0 or past 65535, a sampling libjpeg has not).
    bool valid() const;
    // jpeg_start_compress: SOI and the headers asked for.
    bool start();
    // jpeg_write_marker: a marker segment (0xFF, `code`, the length, the data), before the first row.
    bool marker(u8 code, const u8* data, usize size);
    // jpeg_write_scanlines with one row (width pixels of the input colour space); the frame and scan headers come with
    // the first. Each row completing an iMCU row (or the image) is encoded with it.
    bool write_row(const u8* row);
    // jpeg_finish_compress: the last bits, EOI.
    bool finish();
    // The rows written (libjpeg's next_scanline).
    u32 rows() const;
    // A coefficient past baseline's range (libjpeg's JERR_BAD_DCT_COEF) stopped the encoder.
    bool bad_coefficient() const;

private:
    struct Impl;
    Impl* impl_;
};

// libjpeg 6b's table for `luma` (annex K table K.1, else K.2) at `quality` (jpeg_quality_scaling and
// jpeg_add_quant_table with force_baseline), in natural order.
std::array<u8, 64> quant_table(bool luma, int quality);

// jccolor.c's conversions (16-bit fixed point, rounded as libjpeg rounds).
void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr);
u8 rgb_to_y(u8 r, u8 g, u8 b);

// A whole picture (rows `pitch` bytes apart) into a vector: the stream, or nothing when it cannot be one.
std::vector<u8> encode(const Settings& s, const u8* pixels, usize pitch);

} // namespace pspocket::hle::jpeg
