#pragma once

// A minimal MPEG-2 video writer for tests (synthetic video, no game data, D-920): ISO/IEC 13818-2 main profile
// streams, 4:2:0, written bit by bit (the tests' FFmpeg has no MPEG-2 encoder), and MPEG-1 (ISO/IEC 11172-2) streams.
// One slice per macroblock row, three kinds of picture:
// - I pictures: every macroblock intra, each block its DC level (the block's mean) and, when asked, one AC coefficient
//   (an escape code), so the IDCT has something to round;
// - P pictures: each macroblock either predicted with no motion and not coded (a copy of the anchor's) or coded with no
//   motion compensation (the anchor's samples plus a residual: one DC coefficient per luma block);
// - B pictures: each macroblock interpolated from both anchors with no motion, not coded.
// Frame pictures (frame or field prediction and DCT modes) or field pictures, and the headers and extensions the
// tests vary: quantiser matrices, sequence display, quant matrix, picture display and copyright extensions, user data.
// The variable length codes used are the standard's (Annex B: B.1 macroblock_address_increment, B.2 to B.4
// macroblock_type, B.9 coded_block_pattern, B.10 motion_code, B.12 and B.13 dct_dc_size, B.14 and B.15's end of block
// and escape).

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "h264_writer.hpp" // BitWriter

namespace pspocket::hle::test::mpeg2 {

using h264::BitWriter;

// A start code: zero bits to the byte, then 00 00 01 `code`.
inline void start_code(BitWriter& w, uint8_t code)
{
    w.align_zero();
    w.bits(0x000001, 24);
    w.bits(code, 8);
}

inline void user_data(BitWriter& w, const std::vector<uint8_t>& data)
{
    if (data.empty()) return;
    start_code(w, 0xB2);
    for (const uint8_t c : data) w.bits(c, 8);
}

struct Sequence {
    int width = 96, height = 64;
    int aspect = 1;     // aspect_ratio_information: 1 square samples
    int frame_rate = 3; // frame_rate_code: 3 is 25 Hz
    uint32_t bit_rate = 0x3FFFF;
    int vbv_size = 112;
    bool constrained = false;
    std::vector<uint8_t> intra_matrix, non_intra_matrix; // 64 values in zigzag order, loaded when not empty
    bool mpeg1 = false;                                    // no sequence extension
    int profile_level = 0x48;                              // main profile, main level
    bool progressive = true;
    bool low_delay = false;
    int frame_rate_n = 0, frame_rate_d = 0;
    // sequence_display_extension, when display_width is not 0.
    int display_width = 0, display_height = 0;
    int video_format = 5;
    int colour_primaries = 0, transfer = 0, matrix = 0; // a colour description when colour_primaries is not 0
    std::vector<uint8_t> user_data;
};

inline std::vector<uint8_t> sequence_header(const Sequence& s)
{
    BitWriter w;
    start_code(w, 0xB3);
    w.bits(static_cast<uint32_t>(s.width) & 0xFFF, 12);
    w.bits(static_cast<uint32_t>(s.height) & 0xFFF, 12);
    w.bits(static_cast<uint32_t>(s.aspect), 4);
    w.bits(static_cast<uint32_t>(s.frame_rate), 4);
    w.bits(s.bit_rate & 0x3FFFF, 18);
    w.bit(1);
    w.bits(static_cast<uint32_t>(s.vbv_size) & 0x3FF, 10);
    w.bit(s.constrained ? 1 : 0);
    w.bit(s.intra_matrix.empty() ? 0 : 1);
    for (const uint8_t v : s.intra_matrix) w.bits(v, 8);
    w.bit(s.non_intra_matrix.empty() ? 0 : 1);
    for (const uint8_t v : s.non_intra_matrix) w.bits(v, 8);
    if (!s.mpeg1) {
        start_code(w, 0xB5);
        w.bits(1, 4);
        w.bits(static_cast<uint32_t>(s.profile_level), 8);
        w.bit(s.progressive ? 1 : 0);
        w.bits(1, 2); // chroma_format: 4:2:0
        w.bits(static_cast<uint32_t>(s.width) >> 12, 2);
        w.bits(static_cast<uint32_t>(s.height) >> 12, 2);
        w.bits(s.bit_rate >> 18, 12);
        w.bit(1);
        w.bits(static_cast<uint32_t>(s.vbv_size) >> 10, 8);
        w.bit(s.low_delay ? 1 : 0);
        w.bits(static_cast<uint32_t>(s.frame_rate_n), 2);
        w.bits(static_cast<uint32_t>(s.frame_rate_d), 5);
        if (s.display_width) {
            start_code(w, 0xB5);
            w.bits(2, 4);
            w.bits(static_cast<uint32_t>(s.video_format), 3);
            w.bit(s.colour_primaries ? 1 : 0);
            if (s.colour_primaries) {
                w.bits(static_cast<uint32_t>(s.colour_primaries), 8);
                w.bits(static_cast<uint32_t>(s.transfer), 8);
                w.bits(static_cast<uint32_t>(s.matrix), 8);
            }
            w.bits(static_cast<uint32_t>(s.display_width), 14);
            w.bit(1);
            w.bits(static_cast<uint32_t>(s.display_height), 14);
        }
    }
    user_data(w, s.user_data);
    w.align_zero();
    return w.take();
}

struct Gop {
    int seconds = 0, pictures = 0; // the time code: hours, minutes and seconds from `seconds`
    bool drop = false, closed = false, broken = false;
    std::vector<uint8_t> user_data;
};

inline std::vector<uint8_t> gop_header(const Gop& g)
{
    BitWriter w;
    start_code(w, 0xB8);
    w.bit(g.drop ? 1 : 0);
    w.bits(static_cast<uint32_t>(g.seconds / 3600) & 31, 5);
    w.bits(static_cast<uint32_t>(g.seconds / 60 % 60), 6);
    w.bit(1);
    w.bits(static_cast<uint32_t>(g.seconds % 60), 6);
    w.bits(static_cast<uint32_t>(g.pictures), 6);
    w.bit(g.closed ? 1 : 0);
    w.bit(g.broken ? 1 : 0);
    user_data(w, g.user_data);
    w.align_zero();
    return w.take();
}

inline std::vector<uint8_t> gop_header(int seconds, int pictures = 0, bool closed = false, bool broken = false)
{
    Gop g;
    g.seconds = seconds;
    g.pictures = pictures;
    g.closed = closed;
    g.broken = broken;
    return gop_header(g);
}

enum Type { I = 1, P = 2, B = 3 };

struct Picture {
    Type type = I;
    int temporal_reference = 0;
    int vbv_delay = 0xFFFF;
    int quant = 2;        // quantiser_scale_code
    int dc_precision = 0; // intra_dc_precision: 8 + this bits
    int structure = 3;    // picture_structure: 1 top field, 2 bottom field, 3 frame
    bool top_field_first = false, repeat_first_field = false;
    bool progressive_frame = true;
    bool frame_pred_frame_dct = true;
    bool q_scale_type = false, intra_vlc = false, alternate_scan = false, chroma_420_type = true;
    // composite_display_flag and its fields when composite.
    bool composite = false;
    int v_axis = 0, field_sequence = 0, sub_carrier = 0, burst_amplitude = 0, sub_carrier_phase = 0;
    std::vector<std::pair<int, int>> display_offsets; // picture_display_extension when not empty
    std::vector<uint8_t> quant_intra_matrix;          // quant_matrix_extension's intra matrix when not empty
    bool copyright = false;                           // copyright_extension
    std::vector<uint8_t> user_data;
    int seed = 0;         // what the samples are
    bool ac = false;      // I pictures: one AC coefficient per block too
    bool residual = true; // P pictures: the coded macroblocks' residual (else every macroblock a copy)
    // P and B frame pictures: macroblocks of every kind instead (motion compensated with vectors, coded or not,
    // intra, skipped, with a new quantiser), the coefficients with the tables' codes where they have one; `f_code`
    // the vectors' (MPEG-2's extension, MPEG-1's header).
    bool motion = false;
    int f_code = 1;
    // Field pictures' P macroblocks: the field they predict from (-1 the same parity's; 0 top, 1 bottom: then every
    // macroblock motion compensated, none without, as the second field of a frame whose first is its reference).
    int field_select = -1;
};

// An I picture's block level (its mean) for a macroblock and block (0 to 3 luma, 4 Cb, 5 Cr).
inline int block_level(const Picture& p, int mbx, int mby, int block)
{
    if (block >= 4) return 96 + (mbx * 7 + mby * 5 + p.seed * 3 + block * 11) % 64;
    const int bx = mbx * 2 + (block & 1), by = mby * 2 + (block >> 1);
    return 40 + (bx * 13 + by * 9 + p.seed * 17) % 170;
}

namespace detail {

// B.1: macroblock_address_increment (1 to 33).
inline void address_increment(BitWriter& w, int n)
{
    static const struct {
        uint32_t code;
        int len;
    } k[] = {{0, 0}, {1, 1}, {3, 3}, {2, 3}, {3, 4}, {2, 4}, {3, 5}, {2, 5}, {7, 7}, {6, 7}, {11, 8}, {10, 8},
        {9, 8}, {8, 8}, {7, 8}, {6, 8}, {23, 10}, {22, 10}, {21, 10}, {20, 10}, {19, 10}, {18, 10}, {35, 11},
        {34, 11}, {33, 11}, {32, 11}, {31, 11}, {30, 11}, {29, 11}, {28, 11}, {27, 11}, {26, 11}, {25, 11}, {24, 11}};
    while (n > 33) {
        w.bits(8, 11); // macroblock_escape
        n -= 33;
    }
    w.bits(k[n].code, k[n].len);
}

// B.12 and B.13: dct_dc_size, then the differential.
inline void dc(BitWriter& w, int diff, bool chroma)
{
    int size = 0;
    for (int a = std::abs(diff); a; a >>= 1) size++;
    static const struct {
        uint32_t code;
        int len;
    } luma[] = {{4, 3}, {0, 2}, {1, 2}, {5, 3}, {6, 3}, {14, 4}, {30, 5}, {62, 6}, {126, 7}, {254, 8}, {510, 9},
        {511, 9}},
      chroma_[] = {{0, 2}, {1, 2}, {2, 2}, {6, 3}, {14, 4}, {30, 5}, {62, 6}, {126, 7}, {254, 8}, {510, 9}, {1022, 10},
          {1023, 10}};
    const auto& c = chroma ? chroma_[size] : luma[size];
    w.bits(c.code, c.len);
    if (size) w.bits(static_cast<uint32_t>(diff > 0 ? diff : diff + (1 << size) - 1), size);
}

// B.14 and B.15: a coefficient as an escape (run, then the level: 12 bits in MPEG-2, 8 in MPEG-1), and the end of
// block ('10', or '0110' in an intra block of B.15).
inline void escape(BitWriter& w, int run, int level, bool mpeg1)
{
    w.bits(1, 6);
    w.bits(static_cast<uint32_t>(run), 6);
    if (mpeg1) w.bits(static_cast<uint32_t>(level) & 0xFF, 8);
    else w.bits(static_cast<uint32_t>(level) & 0xFFF, 12);
}
inline void eob(BitWriter& w, bool b15)
{
    if (b15) w.bits(6, 4);
    else w.bits(2, 2);
}

// A motion vector of 0 (motion_code 0 for both components).
inline void zero_mv(BitWriter& w) { w.bits(3, 2); }

// B.10: a motion vector component's difference from its prediction (half samples), with the f_code's residual.
inline void motion_delta(BitWriter& w, int delta, int f_code)
{
    static const struct {
        uint32_t code;
        int len;
    } k[17] = {{0x1, 1}, {0x1, 2}, {0x1, 3}, {0x1, 4}, {0x3, 6}, {0x5, 7}, {0x4, 7}, {0x3, 7}, {0xb, 9}, {0xa, 9},
        {0x9, 9}, {0x11, 10}, {0x10, 10}, {0xf, 10}, {0xe, 10}, {0xd, 10}, {0xc, 10}};
    const int f = 1 << (f_code - 1);
    if (delta < -16 * f) delta += 32 * f;
    if (delta > 16 * f - 1) delta -= 32 * f;
    if (!delta) {
        w.bit(1);
        return;
    }
    const int a = std::abs(delta) - 1, code = a / f + 1;
    w.bits(k[code].code, k[code].len);
    w.bit(delta < 0 ? 1 : 0);
    if (f > 1) w.bits(static_cast<uint32_t>(a % f), f_code - 1);
}

// B.14's codes for small (run, level) pairs (the sign follows); 0 when the table has none (an escape then).
inline int table_zero(int run, int level, uint32_t& code)
{
    static const struct {
        int run, level;
        uint32_t code;
        int len;
    } k[] = {{0, 1, 0x3, 2}, {0, 2, 0x4, 4}, {0, 3, 0x5, 5}, {0, 4, 0x6, 7}, {0, 5, 0x26, 8}, {1, 1, 0x3, 3},
        {1, 2, 0x6, 6}, {2, 1, 0x5, 4}, {2, 2, 0x4, 7}, {3, 1, 0x7, 5}, {4, 1, 0x6, 5}, {5, 1, 0x7, 6}, {6, 1, 0x5, 6},
        {7, 1, 0x4, 6}, {8, 1, 0x7, 7}, {9, 1, 0x5, 7}};
    for (const auto& e : k)
        if (e.run == run && e.level == level) {
            code = e.code;
            return e.len;
        }
    return 0;
}

// A coefficient: the first of a non-intra block's run 0, level +-1 as '1s', else the table's code or an escape.
inline void coefficient(BitWriter& w, int run, int level, bool first_non_intra, bool mpeg1)
{
    if (first_non_intra && run == 0 && std::abs(level) == 1) {
        w.bit(1);
        w.bit(level < 0 ? 1 : 0);
        return;
    }
    uint32_t code = 0;
    if (const int len = table_zero(run, std::abs(level), code)) {
        w.bits(code, len);
        w.bit(level < 0 ? 1 : 0);
        return;
    }
    escape(w, run, level, mpeg1);
}

inline void extension(BitWriter& w, uint32_t id)
{
    start_code(w, 0xB5);
    w.bits(id, 4);
}

// A frame picture's slices with macroblocks of every kind (Picture::motion): P pictures' no motion compensation
// coded, motion compensated (coded or not, with a new quantiser), intra and skipped macroblocks; B pictures' forward,
// backward and interpolated ones (coded or not), intra and skipped. Each vector keeps its prediction inside the
// picture; the coefficients are a few per block, with the tables' codes where they have them.
inline void motion_slices(BitWriter& w, const Sequence& s, const Picture& p, int mbw, int mbh)
{
    const bool m1 = s.mpeg1;
    const bool modes = !m1 && !p.frame_pred_frame_dct;
    const bool b15 = !m1 && p.intra_vlc;
    const auto inside = [&](int mbx, int mby, int vx, int vy) {
        const int x0 = mbx * 16 + (vx >> 1), y0 = mby * 16 + (vy >> 1);
        if (x0 < 0 || y0 < 0 || x0 + 16 + (vx & 1) > s.width || y0 + 16 + (vy & 1) > s.height) return false;
        const int cvx = vx / 2, cvy = vy / 2;
        const int cx0 = mbx * 8 + (cvx >> 1), cy0 = mby * 8 + (cvy >> 1);
        return cx0 >= 0 && cy0 >= 0 && cx0 + 8 + (cvx & 1) <= s.width / 2 && cy0 + 8 + (cvy & 1) <= s.height / 2;
    };
    const auto choose = [&](int mbx, int mby, int salt, int v[2]) {
        static const int cand[10][2] = {{3, 2}, {-2, 1}, {5, -3}, {-7, -4}, {1, 6}, {-3, -1}, {8, 0}, {0, -6},
            {2, 2}, {-1, 3}};
        for (int k = 0; k < 10; k++) {
            const int* c = cand[(mbx * 3 + mby * 7 + salt + p.seed + k) % 10];
            if (inside(mbx, mby, c[0] * p.f_code, c[1] * p.f_code)) {
                v[0] = c[0] * p.f_code;
                v[1] = c[1] * p.f_code;
                return;
            }
        }
        v[0] = v[1] = 0;
    };
    const auto blocks = [&](int mbx, int mby, int cbp, bool intra, int pred[3]) {
        for (int b = 0; b < 6; b++) {
            if (!(cbp & (32 >> b))) continue;
            if (intra) {
                const int c = b < 4 ? 0 : b - 3;
                const int level = block_level(p, mbx, mby, b) << p.dc_precision;
                dc(w, level - pred[c], c != 0);
                pred[c] = level;
                const int ac = ((mbx + mby * 2 + b + p.seed) % 5) - 2;
                if (b15) escape(w, 1, ac ? ac : 2, m1);
                else coefficient(w, 1, ac ? ac : 2, false, m1);
                eob(w, b15);
                continue;
            }
            static const int pairs[8][2] = {{0, 1}, {1, -1}, {0, 2}, {2, 1}, {0, -3}, {5, 1}, {3, -2}, {12, 3}};
            const int n = 1 + (mbx + mby + b + p.seed) % 3;
            for (int k = 0; k < n; k++) {
                const int* e = pairs[(mbx * 2 + mby + b * 3 + k * 5 + p.seed) % 8];
                coefficient(w, e[0], e[1], k == 0, m1);
            }
            eob(w, false);
        }
    };
    enum { NOMC_CODED, MC_NOT_CODED, MC_CODED, MC_CODED_QUANT, INTRA, SKIP, INTERP_NOT, INTERP_CODED,
        INTERP_CODED_QUANT, FWD_NOT, FWD_CODED, BWD_NOT, BWD_CODED };
    static const int p_kinds[10] = {NOMC_CODED, MC_NOT_CODED, MC_CODED, SKIP, MC_CODED, INTRA, MC_CODED_QUANT,
        MC_NOT_CODED, NOMC_CODED, SKIP};
    static const int b_kinds[10] = {INTERP_NOT, INTERP_CODED, FWD_NOT, FWD_CODED, BWD_NOT, BWD_CODED, INTRA, SKIP,
        INTERP_CODED_QUANT, SKIP};
    for (int mby = 0; mby < mbh; mby++) {
        start_code(w, static_cast<uint8_t>(mby + 1));
        w.bits(static_cast<uint32_t>(p.quant), 5);
        w.bit(0); // extra_bit_slice
        const int reset = 128 << p.dc_precision;
        int pred[3] = {reset, reset, reset};
        int pmv[2][2] = {{0, 0}, {0, 0}};
        int skipped = 0;
        bool after_intra = true; // a B macroblock is not skipped at a slice's start or after an intra one
        for (int mbx = 0; mbx < mbw; mbx++) {
            const int h = (mbx * 5 + mby * 3 + p.seed * 7) % 10;
            int kind = p.type == P ? p_kinds[h] : b_kinds[h];
            if (kind == SKIP && (mbx == 0 || mbx == mbw - 1 || (p.type == B && after_intra)))
                kind = p.type == P ? MC_CODED : INTERP_CODED;
            if (kind == SKIP) {
                skipped++;
                if (p.type == P) pmv[0][0] = pmv[0][1] = 0;
                pred[0] = pred[1] = pred[2] = reset;
                continue;
            }
            address_increment(w, 1 + skipped);
            skipped = 0;
            const bool intra = kind == INTRA;
            const bool quant = kind == MC_CODED_QUANT || kind == INTERP_CODED_QUANT;
            const bool fwd = kind == MC_NOT_CODED || kind == MC_CODED || kind == MC_CODED_QUANT || kind == INTERP_NOT ||
                             kind == INTERP_CODED || kind == INTERP_CODED_QUANT || kind == FWD_NOT || kind == FWD_CODED;
            const bool bwd = kind == INTERP_NOT || kind == INTERP_CODED || kind == INTERP_CODED_QUANT ||
                             kind == BWD_NOT || kind == BWD_CODED;
            const bool coded = intra || kind == NOMC_CODED || kind == MC_CODED || kind == MC_CODED_QUANT ||
                               kind == INTERP_CODED || kind == INTERP_CODED_QUANT || kind == FWD_CODED ||
                               kind == BWD_CODED;
            // macroblock_type (B.3, B.4)
            switch (kind) {
            case NOMC_CODED: w.bits(1, 2); break;
            case MC_NOT_CODED: w.bits(1, 3); break;
            case MC_CODED: w.bit(1); break;
            case MC_CODED_QUANT: w.bits(2, 5); break;
            case INTRA: w.bits(3, 5); break;
            case INTERP_NOT: w.bits(2, 2); break;
            case INTERP_CODED: w.bits(3, 2); break;
            case INTERP_CODED_QUANT: w.bits(2, 5); break;
            case FWD_NOT: w.bits(2, 4); break;
            case FWD_CODED: w.bits(3, 4); break;
            case BWD_NOT: w.bits(2, 3); break;
            case BWD_CODED: w.bits(3, 3); break;
            default: break;
            }
            if (modes && (fwd || bwd)) w.bits(2, 2); // frame_motion_type: frame
            if (modes && coded) w.bit(static_cast<uint32_t>((mbx + mby) & 1)); // dct_type
            if (quant) w.bits(static_cast<uint32_t>(1 + (mbx * 3 + mby + p.seed) % 31), 5);
            int v[2];
            if (fwd) {
                choose(mbx, mby, 0, v);
                for (int t = 0; t < 2; t++) {
                    motion_delta(w, v[t] - pmv[0][t], p.f_code);
                    pmv[0][t] = v[t];
                }
            }
            if (bwd) {
                choose(mbx, mby, 4, v);
                for (int t = 0; t < 2; t++) {
                    motion_delta(w, v[t] - pmv[1][t], p.f_code);
                    pmv[1][t] = v[t];
                }
            }
            if (p.type == P && !fwd) pmv[0][0] = pmv[0][1] = 0;
            if (intra) pmv[0][0] = pmv[0][1] = pmv[1][0] = pmv[1][1] = 0;
            if (!intra) pred[0] = pred[1] = pred[2] = reset;
            int cbp = intra ? 63 : 0;
            if (coded && !intra) {
                cbp = 1 + (mbx * 11 + mby * 5 + p.seed) % 63;
                static const uint8_t code[64][2] = {{0x1, 9}, {0xb, 5}, {0x9, 5}, {0xd, 6}, {0xd, 4}, {0x17, 7},
                    {0x13, 7}, {0x1f, 8}, {0xc, 4}, {0x16, 7}, {0x12, 7}, {0x1e, 8}, {0x13, 5}, {0x1b, 8}, {0x17, 8},
                    {0x13, 8}, {0xb, 4}, {0x15, 7}, {0x11, 7}, {0x1d, 8}, {0x11, 5}, {0x19, 8}, {0x15, 8}, {0x11, 8},
                    {0xf, 6}, {0xf, 8}, {0xd, 8}, {0x3, 9}, {0xf, 5}, {0xb, 8}, {0x7, 8}, {0x7, 9}, {0xa, 4},
                    {0x14, 7}, {0x10, 7}, {0x1c, 8}, {0xe, 6}, {0xe, 8}, {0xc, 8}, {0x2, 9}, {0x10, 5}, {0x18, 8},
                    {0x14, 8}, {0x10, 8}, {0xe, 5}, {0xa, 8}, {0x6, 8}, {0x6, 9}, {0x12, 5}, {0x1a, 8}, {0x16, 8},
                    {0x12, 8}, {0xd, 5}, {0x9, 8}, {0x5, 8}, {0x5, 9}, {0xc, 5}, {0x8, 8}, {0x4, 8}, {0x4, 9},
                    {0x7, 3}, {0xa, 5}, {0x8, 5}, {0xc, 6}};
                w.bits(code[cbp][0], code[cbp][1]); // B.9
            }
            blocks(mbx, mby, cbp, intra, pred);
            after_intra = intra;
        }
    }
}

} // namespace detail

// A picture: its header, its extensions and user data, and its slices.
inline std::vector<uint8_t> picture(const Sequence& s, const Picture& p)
{
    BitWriter w;
    const bool m1 = s.mpeg1;
    start_code(w, 0x00);
    w.bits(static_cast<uint32_t>(p.temporal_reference) & 0x3FF, 10);
    w.bits(static_cast<uint32_t>(p.type), 3);
    w.bits(static_cast<uint32_t>(p.vbv_delay) & 0xFFFF, 16);
    // full_pel_*_vector 0 and *_f_code: 7 in MPEG-2 (the extension's f_codes count), MPEG-1's own.
    if (p.type != I) w.bits(m1 ? static_cast<uint32_t>(p.f_code) : 7, 4);
    if (p.type == B) w.bits(m1 ? static_cast<uint32_t>(p.f_code) : 7, 4);
    w.bit(0); // extra_bit_picture
    const bool field = p.structure != 3;
    if (!m1) {
        detail::extension(w, 8); // picture_coding_extension
        const uint32_t f = static_cast<uint32_t>(p.f_code);
        w.bits(p.type != I ? f : 15, 4);
        w.bits(p.type != I ? f : 15, 4);
        w.bits(p.type == B ? f : 15, 4);
        w.bits(p.type == B ? f : 15, 4);
        w.bits(static_cast<uint32_t>(p.dc_precision), 2);
        w.bits(static_cast<uint32_t>(p.structure), 2);
        w.bit(p.top_field_first ? 1 : 0);
        w.bit(p.frame_pred_frame_dct ? 1 : 0);
        w.bit(0); // concealment_motion_vectors
        w.bit(p.q_scale_type ? 1 : 0);
        w.bit(p.intra_vlc ? 1 : 0);
        w.bit(p.alternate_scan ? 1 : 0);
        w.bit(p.repeat_first_field ? 1 : 0);
        w.bit(p.chroma_420_type ? 1 : 0);
        w.bit(p.progressive_frame ? 1 : 0);
        w.bit(p.composite ? 1 : 0);
        if (p.composite) {
            w.bit(static_cast<uint32_t>(p.v_axis));
            w.bits(static_cast<uint32_t>(p.field_sequence), 3);
            w.bit(static_cast<uint32_t>(p.sub_carrier));
            w.bits(static_cast<uint32_t>(p.burst_amplitude), 7);
            w.bits(static_cast<uint32_t>(p.sub_carrier_phase), 8);
        }
        if (!p.quant_intra_matrix.empty()) {
            detail::extension(w, 3);
            w.bit(1);
            for (const uint8_t v : p.quant_intra_matrix) w.bits(v, 8);
            w.bits(0, 3); // no non-intra, chroma intra or chroma non-intra matrix
        }
        if (!p.display_offsets.empty()) {
            detail::extension(w, 7);
            for (const auto& [h, v] : p.display_offsets) {
                w.bits(static_cast<uint32_t>(h) & 0xFFFF, 16);
                w.bit(1);
                w.bits(static_cast<uint32_t>(v) & 0xFFFF, 16);
                w.bit(1);
            }
        }
        if (p.copyright) {
            detail::extension(w, 4);
            w.bit(1);        // copyright_flag
            w.bits(0x2A, 8); // copyright_identifier
            w.bit(1);        // original_or_copy
            w.bits(0, 7);    // reserved
            w.bit(1);
            w.bits(0x12345, 20);
            w.bit(1);
            w.bits(0x6789A, 22);
            w.bit(1);
            w.bits(0x0BCDE, 22);
        }
    }
    user_data(w, p.user_data);
    const int mbw = (s.width + 15) / 16;
    const int mbh = field ? (s.height / 2 + 15) / 16 : (s.height + 15) / 16;
    if (p.motion && p.type != I && !field) {
        detail::motion_slices(w, s, p, mbw, mbh);
        w.align_zero();
        return w.take();
    }
    // Frame pictures without frame_pred_frame_dct carry their motion and DCT types.
    const bool modes = !m1 && !field && !p.frame_pred_frame_dct;
    const bool b15 = !m1 && p.intra_vlc;
    for (int mby = 0; mby < mbh; mby++) {
        start_code(w, static_cast<uint8_t>(mby + 1));
        w.bits(static_cast<uint32_t>(p.quant), 5);
        w.bit(0); // extra_bit_slice
        int pred[3] = {128 << p.dc_precision, 128 << p.dc_precision, 128 << p.dc_precision};
        for (int mbx = 0; mbx < mbw; mbx++) {
            detail::address_increment(w, 1);
            if (p.type == I) {
                w.bit(1); // macroblock_type: intra
                if (modes) w.bit(static_cast<uint32_t>((mbx + mby) & 1)); // dct_type
                for (int b = 0; b < 6; b++) {
                    const int c = b < 4 ? 0 : b - 3;
                    const int level = block_level(p, mbx, mby + (p.structure == 2 ? 7 : 0), b) << p.dc_precision;
                    detail::dc(w, level - pred[c], c != 0);
                    pred[c] = level;
                    if (p.ac) {
                        const int ac = ((mbx + mby + b + p.seed) % 7) - 3 + 4 * ((b + mbx) % 2);
                        detail::escape(w, 0, ac ? ac : 1, m1);
                    }
                    detail::eob(w, b15);
                }
            } else if (p.type == P) {
                const bool coded = p.residual && p.field_select < 0 && (mbx + mby + p.seed) % 2 == 0;
                if (coded) {
                    w.bits(1, 2);        // macroblock_type: pattern, no motion compensation
                    if (modes) w.bit(0); // dct_type: frame
                    w.bits(7, 3);        // coded_block_pattern 60: the four luma blocks
                    for (int b = 0; b < 4; b++) {
                        const int level = ((mbx * 3 + mby + b + p.seed) % 5) - 2;
                        detail::escape(w, 0, level ? level : 1, m1);
                        detail::eob(w, false);
                    }
                } else {
                    w.bits(1, 3);            // macroblock_type: motion forward, not coded
                    if (modes) w.bits(2, 2); // frame_motion_type: frame
                    if (field) {
                        w.bits(1, 2);                    // field_motion_type: field
                        // motion_vertical_field_select: the same parity, or the one asked for
                        w.bit(p.field_select >= 0 ? static_cast<uint32_t>(p.field_select) : p.structure == 2 ? 1 : 0);
                    }
                    detail::zero_mv(w);
                }
            } else {
                w.bits(2, 2); // macroblock_type: interpolated, not coded
                if (modes) w.bits(2, 2);
                if (field) w.bits(1, 2);
                for (int dir = 0; dir < 2; dir++) {
                    if (field) w.bit(p.structure == 2 ? 1 : 0);
                    detail::zero_mv(w);
                }
            }
        }
    }
    w.align_zero();
    return w.take();
}

inline std::vector<uint8_t> sequence_end() { return {0x00, 0x00, 0x01, 0xB7}; }

inline std::vector<uint8_t> join(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// A stream in coded order, one access unit per picture (the sequence header and a GOP header before each I picture):
// `pattern` in display order ("IPPP", "IBBPBBP"...), the B pictures after the anchor that follows them. `edit` changes
// each picture before it is written (its display index given).
inline std::vector<std::vector<uint8_t>> stream(const Sequence& s, const char* pattern, bool ac = false, int seed = 0,
    void (*edit)(Picture&, int) = nullptr)
{
    std::vector<std::vector<uint8_t>> out;
    std::vector<int> order;
    const int n = static_cast<int>(std::char_traits<char>::length(pattern));
    std::vector<int> waiting;
    for (int i = 0; i < n; i++) {
        if (pattern[i] == 'B') {
            waiting.push_back(i);
            continue;
        }
        order.push_back(i);
        for (const int b : waiting) order.push_back(b);
        waiting.clear();
    }
    for (const int b : waiting) order.push_back(b);
    int gop_start = 0;
    for (const int i : order) {
        Picture p;
        p.type = pattern[i] == 'I' ? I : pattern[i] == 'P' ? P : B;
        if (p.type == I) {
            // The group of pictures starts with the B pictures displayed before its I picture (an open GOP).
            gop_start = i;
            while (gop_start > 0 && pattern[gop_start - 1] == 'B') gop_start--;
        }
        p.temporal_reference = i - gop_start;
        p.seed = seed + i;
        p.ac = ac;
        p.progressive_frame = s.progressive;
        p.top_field_first = !s.progressive;
        p.frame_pred_frame_dct = s.progressive;
        if (edit) edit(p, i);
        std::vector<uint8_t> au;
        if (p.type == I) au = join({sequence_header(s), gop_header(i / 25, i % 25)});
        const std::vector<uint8_t> pic = picture(s, p);
        au.insert(au.end(), pic.begin(), pic.end());
        out.push_back(std::move(au));
    }
    return out;
}

} // namespace pspocket::hle::test::mpeg2
