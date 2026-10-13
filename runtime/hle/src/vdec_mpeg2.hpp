#pragma once

// MPEG-1 and MPEG-2 pictures decoded as libsmvd2's SPU slice program decodes them (D-920 part 6): its own decoder, for
// the samples to be Sony's to the bit. Written from ISO/IEC 13818-2 and 11172-2 (the variable length codes are the
// standards' tables B.1 to B.4, B.9 to B.15, the default quantiser matrix theirs) and from what Sony's program was
// seen to compute; nothing of Sony's code or data (CLAUDE.md rule 1).
//
// - The inverse DCT is the program's: the AP-922 row transform (each row's constants 2^14 x sqrt2 cos(r pi/16) x
//   sqrt2 cos(u pi/16), r 4, 1, 2, 3 for the row pairs 0/4, 1/7, 2/6, 3/5), rounded at 2^11 and kept to 16 bits
//   after a shift of 12, then a column transform in 16-bit arithmetic with tangent constants (the products' high
//   halves) and its own rounding (see idct() in the .cpp), the result shifted by 5.
// - MPEG-1 blocks are reconstructed as MPEG-2's (saturation, then mismatch control), not with MPEG-1's odd values.
// - The pictures are predicted from, and decoded into, frame buffers laid out as Sony's are (Frame).
// - What the program reports of a picture besides its samples: the BNR data (smvd2GetBnrData's eight words), two
//   histograms over bands of slice rows (a slice's band: its vertical position divided by the picture's macroblock
//   rows / 4 + 1). Words 4 to 7, a P or B picture's residual: each slice adds (the sum over its macroblocks of 10240
//   for an intra macroblock, or 128 plus the magnitudes of its luma coefficients as dequantised before saturation,
//   ((2|level| + 1) x weight x quantiser scale) >> 5, for a non-intra one with a coded block pattern) >> 8. Words 0 to
//   3, the motion: each slice adds, over its non-intra macroblocks, the sum of the sizes of their vectors' differences
//   from their predictions times a weight (4 for one vector a direction, 2 for two, halved for both directions; 4 for
//   dual prime). Whether the picture is damaged (a code no table has, a coefficient past the block's end, a macroblock
//   past its row or the picture, a slice whose position is not in the picture, data that ends inside a macroblock: the
//   slice stops there; slices missing or out of order). Which references its macroblocks predict from.
// - Where a damaged picture's macroblocks go (Decoder::slice() and decode() in the .cpp): the slices from the first of
//   vertical position 1, each from where the slice before it ended when it does not start there itself, the reading
//   after a slice in error going on from where it stopped, the macroblock in error not written. Not reproduced: the
//   program leaves what a macroblock in error had parsed in its coefficient buffers, which the next macroblock coding
//   those blocks then has too, and counts it in the BNR data; how far it parses (into the bytes after the slice, where
//   it finds the error) is its own. Here every block starts from no coefficients, so the macroblock after one in error
//   may be off.

#include "pspocket/core/types.hpp"

namespace pspocket::hle::mpeg2dec {

// A frame buffer as libsmvd2 keeps it: macroblock after macroblock, each 16 lines of 16 luma samples, then 8 lines of
// 8 Cb and 8 Cr samples (0x180 bytes).
struct Frame {
    u8* p = nullptr;
    int mb_width = 0, mb_height = 0;

    u8& luma(int x, int y) const { return p[((y >> 4) * mb_width + (x >> 4)) * 0x180 + (y & 15) * 16 + (x & 15)]; }
    u8& chroma(int c, int x, int y) const
    {
        return p[((y >> 3) * mb_width + (x >> 3)) * 0x180 + 0x100 + (y & 7) * 16 + c * 8 + (x & 7)];
    }
};

// What the picture's headers give the decoder.
struct Picture {
    bool mpeg1 = false;
    int type = 1;      // picture_coding_type: 1 I, 2 P, 3 B, 4 D (MPEG-1)
    int structure = 3; // 1 top field, 2 bottom field, 3 frame
    bool second_field = false; // a field picture whose frame's other field is decoded already
    int f_code[2][2] = {{15, 15}, {15, 15}}; // [forward, backward][horizontal, vertical]; MPEG-1: the header's code
    bool full_pel[2] = {};                   // MPEG-1's full_pel_forward_vector and _backward_vector
    int intra_dc_precision = 0;
    bool frame_pred_frame_dct = true, concealment_motion_vectors = false, q_scale_type = false,
         intra_vlc_format = false, alternate_scan = false, top_field_first = false;
    int mb_width = 0, mb_rows = 0; // macroblock columns; rows of this picture (a field's rows for a field picture)
    int vertical_size = 0;         // the sequence's lines: slices carry a position extension above 2800
    u8 intra_matrix[64] = {};      // the quantiser matrices in natural order (row by row)
    u8 non_intra_matrix[64] = {};
    // The frames: the one decoded into, the forward and backward references (the frames their fields are in).
    Frame out, forward, backward;
};

struct Result {
    u32 bnr[8] = {};
    bool damaged = false;
    bool forward = false, backward = false; // some macroblock predicted from the forward, the backward reference
    u32 macroblocks = 0;                    // macroblocks the slices covered (skipped ones too)
    u32 slices = 0;
};

// The standards' default intra quantiser matrix, natural order.
extern const u8 kDefaultIntra[64];
// The default zigzag scan: a coefficient's place in the 8x8 block (natural order) by its position in the bitstream.
extern const u8 kZigzag[64];

// `data`: the picture's slices (from its first slice start code to its end).
Result decode(const u8* data, u32 size, const Picture& p);

// The program's inverse DCT of one block in natural order (in place).
void idct(s16 block[64]);

} // namespace pspocket::hle::mpeg2dec
