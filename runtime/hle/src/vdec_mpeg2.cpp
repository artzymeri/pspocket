// MPEG-1 and MPEG-2 pictures decoded as libsmvd2's SPU slice program decodes them (vdec_mpeg2.hpp, D-920 part 6).

#include "vdec_mpeg2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace pspocket::hle::mpeg2dec {

const u8 kDefaultIntra[64] = {8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34,
    38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34,
    38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
const u8 kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
    13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63};

namespace {

// ---- variable length codes: each table indexed by the next `bits` bits of the stream

struct Code {
    u16 code;
    u8 len;
    s16 value;
};

struct Vlc {
    u32 bits = 0;
    std::vector<u32> lookup; // (length << 16) | (value + 0x8000); 0: no code

    Vlc(u32 b, std::initializer_list<Code> codes) : bits(b), lookup(std::size_t{1} << b, 0)
    {
        for (const Code& c : codes) {
            const u32 first = static_cast<u32>(c.code) << (bits - c.len), count = 1u << (bits - c.len);
            for (u32 k = 0; k < count; k++)
                lookup[first + k] = (static_cast<u32>(c.len) << 16) | static_cast<u32>(c.value + 0x8000);
        }
    }
};

// B.1: macroblock_address_increment (1 to 33); 34 the escape (33 more), 35 MPEG-1's stuffing.
const Vlc& address_increment()
{
    static const Vlc v(11, {{0x1, 1, 1}, {0x3, 3, 2}, {0x2, 3, 3}, {0x3, 4, 4}, {0x2, 4, 5}, {0x3, 5, 6}, {0x2, 5, 7},
        {0x7, 7, 8}, {0x6, 7, 9}, {0xb, 8, 10}, {0xa, 8, 11}, {0x9, 8, 12}, {0x8, 8, 13}, {0x7, 8, 14}, {0x6, 8, 15},
        {0x17, 10, 16}, {0x16, 10, 17}, {0x15, 10, 18}, {0x14, 10, 19}, {0x13, 10, 20}, {0x12, 10, 21},
        {0x23, 11, 22}, {0x22, 11, 23}, {0x21, 11, 24}, {0x20, 11, 25}, {0x1f, 11, 26}, {0x1e, 11, 27},
        {0x1d, 11, 28}, {0x1c, 11, 29}, {0x1b, 11, 30}, {0x1a, 11, 31}, {0x19, 11, 32}, {0x18, 11, 33},
        {0x8, 11, 34}, {0xf, 11, 35}});
    return v;
}

// The macroblock_type flags.
enum : int { MB_INTRA = 1, MB_PATTERN = 2, MB_BACKWARD = 4, MB_FORWARD = 8, MB_QUANT = 16 };

// B.2 to B.4: macroblock_type in I, P and B pictures.
const Vlc& type_i()
{
    static const Vlc v(2, {{0x1, 1, MB_INTRA}, {0x1, 2, MB_QUANT | MB_INTRA}});
    return v;
}
const Vlc& type_p()
{
    static const Vlc v(6, {{0x1, 1, MB_FORWARD | MB_PATTERN}, {0x1, 2, MB_PATTERN}, {0x1, 3, MB_FORWARD},
        {0x3, 5, MB_INTRA}, {0x2, 5, MB_QUANT | MB_FORWARD | MB_PATTERN}, {0x1, 5, MB_QUANT | MB_PATTERN},
        {0x1, 6, MB_QUANT | MB_INTRA}});
    return v;
}
const Vlc& type_b()
{
    static const Vlc v(6, {{0x2, 2, MB_FORWARD | MB_BACKWARD}, {0x3, 2, MB_FORWARD | MB_BACKWARD | MB_PATTERN},
        {0x2, 3, MB_BACKWARD}, {0x3, 3, MB_BACKWARD | MB_PATTERN}, {0x2, 4, MB_FORWARD},
        {0x3, 4, MB_FORWARD | MB_PATTERN}, {0x3, 5, MB_INTRA}, {0x2, 5, MB_QUANT | MB_FORWARD | MB_BACKWARD | MB_PATTERN},
        {0x3, 6, MB_QUANT | MB_FORWARD | MB_PATTERN}, {0x2, 6, MB_QUANT | MB_BACKWARD | MB_PATTERN},
        {0x1, 6, MB_QUANT | MB_INTRA}});
    return v;
}

// B.9: coded_block_pattern (4:2:0), by value.
const Vlc& block_pattern()
{
    static const u8 code[64][2] = {{0x1, 9}, {0xb, 5}, {0x9, 5}, {0xd, 6}, {0xd, 4}, {0x17, 7}, {0x13, 7}, {0x1f, 8},
        {0xc, 4}, {0x16, 7}, {0x12, 7}, {0x1e, 8}, {0x13, 5}, {0x1b, 8}, {0x17, 8}, {0x13, 8}, {0xb, 4}, {0x15, 7},
        {0x11, 7}, {0x1d, 8}, {0x11, 5}, {0x19, 8}, {0x15, 8}, {0x11, 8}, {0xf, 6}, {0xf, 8}, {0xd, 8}, {0x3, 9},
        {0xf, 5}, {0xb, 8}, {0x7, 8}, {0x7, 9}, {0xa, 4}, {0x14, 7}, {0x10, 7}, {0x1c, 8}, {0xe, 6}, {0xe, 8},
        {0xc, 8}, {0x2, 9}, {0x10, 5}, {0x18, 8}, {0x14, 8}, {0x10, 8}, {0xe, 5}, {0xa, 8}, {0x6, 8}, {0x6, 9},
        {0x12, 5}, {0x1a, 8}, {0x16, 8}, {0x12, 8}, {0xd, 5}, {0x9, 8}, {0x5, 8}, {0x5, 9}, {0xc, 5}, {0x8, 8},
        {0x4, 8}, {0x4, 9}, {0x7, 3}, {0xa, 5}, {0x8, 5}, {0xc, 6}};
    static const Vlc v = [] {
        Vlc t(9, {});
        for (u32 value = 0; value < 64; value++) {
            const u32 len = code[value][1], first = static_cast<u32>(code[value][0]) << (9 - len);
            for (u32 k = 0; k < (1u << (9 - len)); k++) t.lookup[first + k] = (len << 16) | (value + 0x8000);
        }
        return t;
    }();
    return v;
}

// B.10: motion_code's magnitude (a sign bit follows a non-zero one).
const Vlc& motion_code()
{
    static const Vlc v(10, {{0x1, 1, 0}, {0x1, 2, 1}, {0x1, 3, 2}, {0x1, 4, 3}, {0x3, 6, 4}, {0x5, 7, 5}, {0x4, 7, 6},
        {0x3, 7, 7}, {0xb, 9, 8}, {0xa, 9, 9}, {0x9, 9, 10}, {0x11, 10, 11}, {0x10, 10, 12}, {0xf, 10, 13},
        {0xe, 10, 14}, {0xd, 10, 15}, {0xc, 10, 16}});
    return v;
}

// B.12, B.13: dct_dc_size_luminance and _chrominance.
const Vlc& dc_luma()
{
    static const Vlc v(9, {{0x4, 3, 0}, {0x0, 2, 1}, {0x1, 2, 2}, {0x5, 3, 3}, {0x6, 3, 4}, {0xe, 4, 5}, {0x1e, 5, 6},
        {0x3e, 6, 7}, {0x7e, 7, 8}, {0xfe, 8, 9}, {0x1fe, 9, 10}, {0x1ff, 9, 11}});
    return v;
}
const Vlc& dc_chroma()
{
    static const Vlc v(10, {{0x0, 2, 0}, {0x1, 2, 1}, {0x2, 2, 2}, {0x6, 3, 3}, {0xe, 4, 4}, {0x1e, 5, 5},
        {0x3e, 6, 6}, {0x7e, 7, 7}, {0xfe, 8, 8}, {0x1fe, 9, 9}, {0x3fe, 10, 10}, {0x3ff, 10, 11}});
    return v;
}

// B.14 and B.15: dct_coefficients (run, level); a sign bit follows. Values: run << 8 | level; -1 the end of block,
// -2 the escape.
struct Rl {
    u16 code;
    u8 len, run, level;
};
constexpr Rl kTableZero[] = {{0x3, 2, 0, 1}, {0x4, 4, 0, 2}, {0x5, 5, 0, 3}, {0x6, 7, 0, 4}, {0x26, 8, 0, 5},
    {0x21, 8, 0, 6}, {0xa, 10, 0, 7}, {0x1d, 12, 0, 8}, {0x18, 12, 0, 9}, {0x13, 12, 0, 10}, {0x10, 12, 0, 11},
    {0x1a, 13, 0, 12}, {0x19, 13, 0, 13}, {0x18, 13, 0, 14}, {0x17, 13, 0, 15}, {0x1f, 14, 0, 16}, {0x1e, 14, 0, 17},
    {0x1d, 14, 0, 18}, {0x1c, 14, 0, 19}, {0x1b, 14, 0, 20}, {0x1a, 14, 0, 21}, {0x19, 14, 0, 22}, {0x18, 14, 0, 23},
    {0x17, 14, 0, 24}, {0x16, 14, 0, 25}, {0x15, 14, 0, 26}, {0x14, 14, 0, 27}, {0x13, 14, 0, 28}, {0x12, 14, 0, 29},
    {0x11, 14, 0, 30}, {0x10, 14, 0, 31}, {0x18, 15, 0, 32}, {0x17, 15, 0, 33}, {0x16, 15, 0, 34}, {0x15, 15, 0, 35},
    {0x14, 15, 0, 36}, {0x13, 15, 0, 37}, {0x12, 15, 0, 38}, {0x11, 15, 0, 39}, {0x10, 15, 0, 40}, {0x3, 3, 1, 1},
    {0x6, 6, 1, 2}, {0x25, 8, 1, 3}, {0xc, 10, 1, 4}, {0x1b, 12, 1, 5}, {0x16, 13, 1, 6}, {0x15, 13, 1, 7},
    {0x1f, 15, 1, 8}, {0x1e, 15, 1, 9}, {0x1d, 15, 1, 10}, {0x1c, 15, 1, 11}, {0x1b, 15, 1, 12}, {0x1a, 15, 1, 13},
    {0x19, 15, 1, 14}, {0x13, 16, 1, 15}, {0x12, 16, 1, 16}, {0x11, 16, 1, 17}, {0x10, 16, 1, 18}, {0x5, 4, 2, 1},
    {0x4, 7, 2, 2}, {0xb, 10, 2, 3}, {0x14, 12, 2, 4}, {0x14, 13, 2, 5}, {0x7, 5, 3, 1}, {0x24, 8, 3, 2},
    {0x1c, 12, 3, 3}, {0x13, 13, 3, 4}, {0x6, 5, 4, 1}, {0xf, 10, 4, 2}, {0x12, 12, 4, 3}, {0x7, 6, 5, 1},
    {0x9, 10, 5, 2}, {0x12, 13, 5, 3}, {0x5, 6, 6, 1}, {0x1e, 12, 6, 2}, {0x14, 16, 6, 3}, {0x4, 6, 7, 1},
    {0x15, 12, 7, 2}, {0x7, 7, 8, 1}, {0x11, 12, 8, 2}, {0x5, 7, 9, 1}, {0x11, 13, 9, 2}, {0x27, 8, 10, 1},
    {0x10, 13, 10, 2}, {0x23, 8, 11, 1}, {0x1a, 16, 11, 2}, {0x22, 8, 12, 1}, {0x19, 16, 12, 2}, {0x20, 8, 13, 1},
    {0x18, 16, 13, 2}, {0xe, 10, 14, 1}, {0x17, 16, 14, 2}, {0xd, 10, 15, 1}, {0x16, 16, 15, 2}, {0x8, 10, 16, 1},
    {0x15, 16, 16, 2}, {0x1f, 12, 17, 1}, {0x1a, 12, 18, 1}, {0x19, 12, 19, 1}, {0x17, 12, 20, 1}, {0x16, 12, 21, 1},
    {0x1f, 13, 22, 1}, {0x1e, 13, 23, 1}, {0x1d, 13, 24, 1}, {0x1c, 13, 25, 1}, {0x1b, 13, 26, 1}, {0x1f, 16, 27, 1},
    {0x1e, 16, 28, 1}, {0x1d, 16, 29, 1}, {0x1c, 16, 30, 1}, {0x1b, 16, 31, 1}};
// Table one (B.15) where it differs from table zero.
constexpr Rl kTableOneChanges[] = {{0x2, 2, 0, 1}, {0x6, 3, 0, 2}, {0x7, 4, 0, 3}, {0x1c, 5, 0, 4}, {0x1d, 5, 0, 5},
    {0x5, 6, 0, 6}, {0x4, 6, 0, 7}, {0x7b, 7, 0, 8}, {0x7c, 7, 0, 9}, {0x23, 8, 0, 10}, {0x22, 8, 0, 11},
    {0xfa, 8, 0, 12}, {0xfb, 8, 0, 13}, {0xfe, 8, 0, 14}, {0xff, 8, 0, 15}, {0x2, 3, 1, 1}, {0x6, 5, 1, 2},
    {0x79, 7, 1, 3}, {0x27, 8, 1, 4}, {0x20, 8, 1, 5}, {0x5, 5, 2, 1}, {0x7, 7, 2, 2}, {0xfc, 8, 2, 3},
    {0xc, 10, 2, 4}, {0x26, 8, 3, 2}, {0x6, 6, 4, 1}, {0xfd, 8, 4, 2}, {0x4, 9, 5, 2}, {0x6, 7, 6, 1},
    {0x4, 7, 7, 1}, {0x5, 7, 8, 1}, {0x78, 7, 9, 1}, {0x7a, 7, 10, 1}, {0x21, 8, 11, 1}, {0x25, 8, 12, 1},
    {0x24, 8, 13, 1}, {0x5, 9, 14, 1}, {0x7, 9, 15, 1}, {0xd, 10, 16, 1}};

constexpr s16 kEndOfBlock = -1, kEscape = -2;

Vlc coefficients(bool one)
{
    Vlc t(16, {});
    const auto put = [&](u32 code, u32 len, s16 value) {
        const u32 first = code << (16 - len);
        for (u32 k = 0; k < (1u << (16 - len)); k++)
            t.lookup[first + k] = (len << 16) | static_cast<u32>(value + 0x8000);
    };
    for (const Rl& r : kTableZero) {
        Rl use = r;
        if (one)
            for (const Rl& c : kTableOneChanges)
                if (c.run == r.run && c.level == r.level) use = c;
        put(use.code, use.len, static_cast<s16>(use.run << 8 | use.level));
    }
    put(0x1, 6, kEscape);
    if (one) put(0x6, 4, kEndOfBlock);
    else put(0x2, 2, kEndOfBlock);
    return t;
}
const Vlc& table_zero()
{
    static const Vlc v = coefficients(false);
    return v;
}
const Vlc& table_one()
{
    static const Vlc v = coefficients(true);
    return v;
}

// The alternate scan.
constexpr u8 kAlternate[64] = {0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12,
    19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7,
    15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};

// The non-linear quantiser scale (q_scale_type 1).
constexpr u8 kNonLinear[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52,
    56, 64, 72, 80, 88, 96, 104, 112};

// ---- the bitstream

struct Reader {
    const u8* p;
    u32 size;
    u64 pos = 0; // bits
    bool over = false;

    u32 peek(u32 n) const
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++) {
            const u64 at = pos + i;
            v = (v << 1) | (at / 8 < size ? (p[at / 8] >> (7 - at % 8)) & 1 : 0);
        }
        return v;
    }
    void skip(u32 n)
    {
        pos += n;
        if (pos > u64{size} * 8) over = true;
    }
    u32 bits(u32 n)
    {
        const u32 v = peek(n);
        skip(n);
        return v;
    }
    // A code of the table, its value; false when none matches.
    bool code(const Vlc& t, int& value)
    {
        const u32 e = t.lookup[peek(t.bits)];
        if (!e) return false;
        skip(e >> 16);
        value = static_cast<int>(e & 0xFFFF) - 0x8000;
        return true;
    }
};

// ---- the inverse DCT

s16 h16(int v) { return static_cast<s16>(static_cast<u16>(v)); }
// A 16-bit sample times a 16-bit constant, the product's high half.
s16 high(int x, int c) { return h16((x * c) >> 16); }

// A row's constants: c[u] = 2^14 x sqrt2 cos(r pi/16) x sqrt2 cos(u pi/16), u 1 to 7 (rows 0 and 4 take r 4; 1 and 7,
// r 1; 2 and 6, r 2; 3 and 5, r 3), rounded.
struct RowConstants {
    int c[4][8];
    RowConstants()
    {
        const double pi = 3.14159265358979323846, s2 = 1.41421356237309504880;
        const int r_of[4] = {4, 1, 2, 3};
        for (int t = 0; t < 4; t++)
            for (int u = 1; u < 8; u++) {
                const double k = s2 * std::cos(r_of[t] * pi / 16) * s2 * std::cos(u * pi / 16) * 16384.0;
                c[t][u] = static_cast<int>(k < 0 ? k - 0.5 : k + 0.5);
            }
    }
};
const RowConstants& row_constants()
{
    static const RowConstants k;
    return k;
}

void idct_row(s16* x, const int* c)
{
    int even[4], odd[4];
    for (int k = 0; k < 4; k++) {
        static const int s2[4] = {2, 6, -6, -2}, s4[4] = {4, -4, -4, 4}, s6[4] = {6, -2, 2, -6};
        static const int o1[4] = {1, 3, 5, 7}, o3[4] = {3, -7, -1, -5}, o5[4] = {5, -1, 7, 3}, o7[4] = {7, -5, 3, -1};
        const auto at = [&](int s) { return s < 0 ? -c[-s] : c[s]; };
        even[k] = x[0] * c[4] + x[2] * at(s2[k]) + x[4] * at(s4[k]) + x[6] * at(s6[k]) + 2048;
        odd[k] = x[1] * at(o1[k]) + x[3] * at(o3[k]) + x[5] * at(o5[k]) + x[7] * at(o7[k]);
    }
    for (int k = 0; k < 4; k++) {
        x[k] = h16((even[k] + odd[k]) >> 12);
        x[7 - k] = h16((even[k] - odd[k]) >> 12);
    }
}

// One column (eight rows' samples of it), 16-bit arithmetic with wrap-around as the program's.
void idct_column(s16* v)
{
    constexpr int tan1 = 13036, tan2 = 27146, tan3 = -21746, cos4 = -19195; // tan3 and cos4 less 1, in 16 bits
    const int r0 = v[0], r1 = v[8], r2 = v[16], r3 = v[24], r4 = v[32], r5 = v[40], r6 = v[48], r7 = v[56];
    const s16 u = h16(h16(r5 - r3) - high(r3, tan3)); // r5 - r3 tan(3pi/16)
    const s16 m1 = h16(high(r1, tan1) - r7);
    const s16 m0 = h16(high(r7, tan1) + r1);
    const s16 m2 = h16(h16(r3 + r5) + high(r5, tan3));
    const s16 v1 = h16(h16(m1 - u) + 1);
    const s16 w = h16(m0 - m2);
    const s16 p = h16(w - v1), q = h16(w + v1);
    const s16 m7 = h16(h16(m0 + m2) + 1);
    const s16 m4 = h16(m1 + u);
    const s16 pq = h16(h16(q + high(q, cos4)) | 1), pp = h16(h16(p + high(p, cos4)) | 1);
    const s16 e1 = h16(high(r2, tan2) - r6), e2 = h16(high(r6, tan2) + r2);
    const s16 s = h16(r0 + r4), d = h16(r0 - r4);
    const s16 g1 = h16(h16(s + e2) + 16), g2 = h16(h16(d + e1) + 16);
    const s16 f1 = h16(h16(s - e2) + 15), f2 = h16(h16(d - e1) + 15);
    v[0] = static_cast<s16>(h16(g1 + m7) >> 5);
    v[8] = static_cast<s16>(h16(g2 + pq) >> 5);
    v[16] = static_cast<s16>(h16(f2 + pp) >> 5);
    v[24] = static_cast<s16>(h16(f1 + m4) >> 5);
    v[32] = static_cast<s16>(h16(f1 - m4) >> 5);
    v[40] = static_cast<s16>(h16(f2 - pp) >> 5);
    v[48] = static_cast<s16>(h16(g2 - pq) >> 5);
    v[56] = static_cast<s16>(h16(g1 - m7) >> 5);
}

} // namespace

void idct(s16 block[64])
{
    const RowConstants& k = row_constants();
    static const int table_of_row[8] = {0, 1, 2, 3, 0, 3, 2, 1};
    for (int r = 0; r < 8; r++) idct_row(block + r * 8, k.c[table_of_row[r]]);
    for (int x = 0; x < 8; x++) idct_column(block + x);
}

namespace {

// ---- a picture's slices

int clip255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// A macroblock's samples in its own lines: 16 x 16 luma, 8 x 8 Cb, 8 x 8 Cr (a field macroblock's lines for a field
// picture, the frame's otherwise).
struct Block16 {
    u8 y[256];
    u8 c[2][64];
};

struct Decoder {
    const Picture& p;
    Reader r;
    Result out;
    int n = 1;      // slice rows a band
    int qscale = 0; // the current quantiser scale (MPEG-2's, twice MPEG-1's code)
    // What the program counts (vdec_mpeg2.hpp).
    u32 lanes = 0, motion = 0, weight = 0, slice_motion = 0;
    // Motion vector predictions [r][s][t], the DC predictions, the last macroblock (a skipped B one predicts as it did).
    int pmv[2][2][2] = {};
    int dc[3] = {};
    struct Prediction {
        int type = 0, motion_type = 2;
        int v[2][2][2] = {};     // [r][s][t] in half samples (field vectors' vertical in field lines)
        int select[2][2] = {};   // motion_vertical_field_select [r][s]
        int dmv[2] = {};
    } last;
    const bool frame;
    const int parity; // a field picture's: 0 top, 1 bottom
    s16 coef[6][64];
    // Where the slices go: the macroblock address after the last slice's own last macroblock, and how far from its
    // own place the current slice's macroblocks are written (slice()).
    int own_end = 0, shift = 0;

    Decoder(const Picture& pic, const u8* data, u32 size)
        : p(pic), r{data, size}, frame(pic.structure == 3), parity(pic.structure == 2 ? 1 : 0)
    {
    }

    void quantiser(u32 code) { qscale = p.q_scale_type && !p.mpeg1 ? kNonLinear[code] : static_cast<int>(code) * 2; }
    void reset_dc()
    {
        const int v = p.mpeg1 ? 128 : 128 << p.intra_dc_precision;
        dc[0] = dc[1] = dc[2] = v;
    }
    void reset_pmv() { std::memset(pmv, 0, sizeof(pmv)); }

    // ---- motion vectors (7.6.3)

    // One vector's two components into v (prediction from pmv[rr][s], the field vector's vertical halved in frame
    // pictures); false on an error.
    bool motion_vector(int rr, int s, bool field_vector, bool dual_prime, int v[2], int dmv[2])
    {
        for (int t = 0; t < 2; t++) {
            int magnitude = 0;
            if (!r.code(motion_code(), magnitude)) return false;
            int code = magnitude;
            if (magnitude && r.bits(1)) code = -magnitude;
            const int f_code = p.f_code[s][p.mpeg1 ? 0 : t];
            const int r_size = f_code - 1, f = 1 << std::max(0, r_size);
            int residual = 0;
            if (f > 1 && magnitude) residual = static_cast<int>(r.bits(static_cast<u32>(r_size)));
            if (dual_prime) {
                // B.11: '0' 0, '10' +1, '11' -1
                dmv[t] = r.bits(1) ? (r.bits(1) ? -1 : 1) : 0;
            }
            int delta = code;
            if (f > 1 && code) {
                delta = (magnitude - 1) * f + residual + 1;
                if (code < 0) delta = -delta;
            }
            motion += static_cast<u32>(std::abs(delta)); // the program's motion measure: the differences' sizes
            const bool halve = field_vector && t == 1 && frame;
            int prediction = pmv[rr][s][t];
            if (halve) prediction >>= 1;
            int vec = prediction + delta;
            const int low = -16 * f, high = 16 * f - 1, range = 32 * f;
            if (vec < low) vec += range;
            if (vec > high) vec -= range;
            pmv[rr][s][t] = halve ? vec * 2 : vec;
            v[t] = vec;
        }
        return true;
    }

    // motion_vectors(s) for the macroblock's motion type into `pr`.
    bool motion_vectors(int s, int motion_type, Prediction& pr)
    {
        if (p.mpeg1) {
            int v[2], dmv[2];
            if (!motion_vector(0, s, false, false, v, dmv)) return false;
            if (p.full_pel[s]) v[0] *= 2, v[1] *= 2;
            pmv[1][s][0] = pmv[0][s][0];
            pmv[1][s][1] = pmv[0][s][1];
            pr.v[0][s][0] = pr.v[1][s][0] = v[0];
            pr.v[0][s][1] = pr.v[1][s][1] = v[1];
            return true;
        }
        int count = 1;
        bool field_format = !frame, dual_prime = false;
        if (frame) {
            if (motion_type == 1) count = 2, field_format = true;
            else if (motion_type == 3) field_format = true, dual_prime = true;
        } else {
            if (motion_type == 2) count = 2;
            else if (motion_type == 3) dual_prime = true;
        }
        if (count == 1) {
            if (field_format && !dual_prime) pr.select[0][s] = static_cast<int>(r.bits(1));
            int v[2], dmv[2] = {};
            if (!motion_vector(0, s, field_format, dual_prime, v, dmv)) return false;
            // One vector: both predictions take it.
            pmv[1][s][0] = pmv[0][s][0];
            pmv[1][s][1] = pmv[0][s][1];
            pr.v[0][s][0] = pr.v[1][s][0] = v[0];
            pr.v[0][s][1] = pr.v[1][s][1] = v[1];
            pr.select[1][s] = pr.select[0][s];
            if (dual_prime) pr.dmv[0] = dmv[0], pr.dmv[1] = dmv[1];
            return true;
        }
        for (int k = 0; k < 2; k++) {
            pr.select[k][s] = static_cast<int>(r.bits(1));
            int v[2], dmv[2];
            if (!motion_vector(k, s, true, false, v, dmv)) return false;
            pr.v[k][s][0] = v[0];
            pr.v[k][s][1] = v[1];
        }
        return true;
    }

    // ---- blocks (7.2 to 7.4)

    // block(i): its coefficients dequantised into coef[i] (natural order); the non-intra luma ones' magnitudes as
    // dequantised before saturation into `sum`.
    bool block(int i, bool intra, u32& sum)
    {
        s16* f = coef[i];
        std::memset(f, 0, sizeof(coef[i]));
        const u8* scan = p.alternate_scan && !p.mpeg1 ? kAlternate : kZigzag;
        const u8* w = intra ? p.intra_matrix : p.non_intra_matrix;
        const Vlc& table = intra && p.intra_vlc_format && !p.mpeg1 ? table_one() : table_zero();
        int index = -1, total = 0;
        const auto put = [&](int at, int level) {
            const int pos = scan[at];
            int value;
            if (intra) {
                value = (level * w[pos] * qscale * 2) / 32;
            } else {
                const int k = level > 0 ? 1 : -1;
                value = ((2 * level + k) * w[pos] * qscale) / 32;
                if (i < 4) sum += static_cast<u32>(((2 * std::abs(level) + 1) * w[pos] * qscale) >> 5);
            }
            value = std::clamp(value, -2048, 2047);
            f[pos] = static_cast<s16>(value);
            total += value;
        };
        if (intra) {
            int size = 0;
            if (!r.code(i < 4 ? dc_luma() : dc_chroma(), size)) return false;
            int diff = 0;
            if (size) {
                diff = static_cast<int>(r.bits(static_cast<u32>(size)));
                if (diff < (1 << (size - 1))) diff = diff - (1 << size) + 1;
            }
            const int c = i < 4 ? 0 : i - 3;
            dc[c] += diff;
            const int mult = p.mpeg1 ? 8 : 8 >> p.intra_dc_precision;
            f[0] = static_cast<s16>(dc[c] * mult);
            total += f[0];
            index = 0;
            if (p.type == 4) return !r.over; // D pictures: the DC coefficient alone
        } else if (r.peek(1)) {
            // The first coefficient's '1s': run 0, level +-1.
            r.skip(1);
            put(0, r.bits(1) ? -1 : 1);
            index = 0;
        }
        for (;;) {
            int value = 0;
            if (!r.code(table, value)) return false;
            if (value == kEndOfBlock) break;
            int run = 0, level = 0;
            if (value == kEscape) {
                run = static_cast<int>(r.bits(6));
                if (p.mpeg1) {
                    level = static_cast<int>(r.bits(8));
                    if (level == 0) level = static_cast<int>(r.bits(8));
                    else if (level == 128) level = static_cast<int>(r.bits(8)) - 256;
                    else if (level > 128) level -= 256;
                } else {
                    level = static_cast<int>(r.bits(12));
                    if (level >= 2048) level -= 4096;
                }
                if (level == 0) return false;
            } else {
                run = value >> 8;
                level = value & 0xFF;
                if (r.bits(1)) level = -level;
            }
            index += run + 1;
            if (index > 63) return false;
            put(index, level);
        }
        // MPEG-2's mismatch control, MPEG-1's blocks too (the program reconstructs them as MPEG-2's, not with
        // MPEG-1's odd values): an even sum makes the last coefficient odd.
        if (!(total & 1)) f[63] = static_cast<s16>(f[63] ^ 1);
        return !r.over;
    }

    // ---- prediction (7.6)

    // A reference sample: plane 0 luma, 1 Cb, 2 Cr, of `ref` (field -1: the frame; 0 top, 1 bottom), clamped into it.
    int sample(const Frame& ref, int plane, int field, int x, int y) const
    {
        const int w = ref.mb_width * (plane ? 8 : 16), hgt = ref.mb_height * (plane ? 8 : 16);
        const int lines = field < 0 ? hgt : hgt / 2;
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, lines - 1);
        if (field >= 0) y = y * 2 + field;
        return plane ? ref.chroma(plane - 1, x, y) : ref.luma(x, y);
    }

    // A w x h prediction from `ref`'s field (or frame) at (x, y) plus the half samples (hx, hy), into `out` rows
    // [row, row + h) a line apart by `step`.
    void predict(const Frame& ref, int plane, int field, int x, int y, int hx, int hy, int w, int h, u8* to,
        int stride, int row, int step) const
    {
        for (int i = 0; i < h; i++)
            for (int j = 0; j < w; j++) {
                int v = sample(ref, plane, field, x + j, y + i);
                if (hx && hy)
                    v = (v + sample(ref, plane, field, x + j + 1, y + i) + sample(ref, plane, field, x + j, y + i + 1) +
                            sample(ref, plane, field, x + j + 1, y + i + 1) + 2) >> 2;
                else if (hx)
                    v = (v + sample(ref, plane, field, x + j + 1, y + i) + 1) >> 1;
                else if (hy)
                    v = (v + sample(ref, plane, field, x + j, y + i + 1) + 1) >> 1;
                to[(row + i * step) * stride + j] = static_cast<u8>(v);
            }
    }

    // A whole prediction (luma vector v in half samples, the chroma one v / 2) of a part of the macroblock.
    void predict_part(const Frame& ref, int field, const int v[2], int mbx, int y0, int w, int h, Block16& b,
        int row, int step) const
    {
        predict(ref, 0, field, mbx * 16 + (v[0] >> 1), y0 + (v[1] >> 1), v[0] & 1, v[1] & 1, w, h, b.y, 16, row,
            step);
        // Chroma: the vector halved toward zero; a field's lines interleave in the macroblock as luma's do.
        const int cx = v[0] / 2, cy = v[1] / 2;
        for (int c = 0; c < 2; c++)
            predict(ref, c + 1, field, mbx * 8 + (cx >> 1), y0 / 2 + (cy >> 1), cx & 1, cy & 1, w / 2, h / 2, b.c[c], 8,
                step == 2 ? row : row / 2, step);
    }

    // The reference frame a field prediction reads (field pictures: the second field of a P frame predicts its
    // other field from its own frame).
    const Frame& reference(int s, int select) const
    {
        if (s == 0 && !frame && p.second_field && p.type == 2 && select != parity) return p.out;
        return s == 0 ? p.forward : p.backward;
    }

    // The macroblock's prediction from direction s into b.
    void predict_direction(int s, const Prediction& pr, int mbx, int mby, Block16& b) const
    {
        if (frame) {
            if (p.mpeg1 || pr.motion_type == 2) {
                predict_part(s == 0 ? p.forward : p.backward, -1, pr.v[0][s], mbx, mby * 16, 16, 16, b, 0, 1);
            } else if (pr.motion_type == 1) {
                for (int k = 0; k < 2; k++)
                    predict_part(s == 0 ? p.forward : p.backward, pr.select[k][s], pr.v[k][s], mbx, mby * 8, 16, 8, b,
                        k, 2);
            } else {
                dual_prime_frame(pr, mbx, mby, b);
            }
            return;
        }
        if (pr.motion_type == 1) {
            predict_part(reference(s, pr.select[0][s]), pr.select[0][s], pr.v[0][s], mbx, mby * 16, 16, 16, b, 0, 1);
        } else if (pr.motion_type == 2) {
            for (int k = 0; k < 2; k++)
                predict_part(reference(s, pr.select[k][s]), pr.select[k][s], pr.v[k][s], mbx, mby * 16 + 8 * k, 16, 8,
                    b, 8 * k, 1);
        } else {
            dual_prime_field(pr, mbx, mby, b);
        }
    }

    static void average(Block16& a, const Block16& b)
    {
        for (int k = 0; k < 256; k++) a.y[k] = static_cast<u8>((a.y[k] + b.y[k] + 1) >> 1);
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < 64; k++) a.c[c][k] = static_cast<u8>((a.c[c][k] + b.c[c][k] + 1) >> 1);
    }

    // Dual prime (7.6.3.6): each field's prediction the average of the same parity's (the vector) and the other
    // parity's (the vector scaled to its distance, the dmvector and the field offset added).
    void dual_prime_frame(const Prediction& pr, int mbx, int mby, Block16& b) const
    {
        const int vx = pr.v[0][0][0], vy = pr.v[0][0][1];
        for (int k = 0; k < 2; k++) {
            Block16 same{}, other{};
            const int same_v[2] = {vx, vy};
            predict_part(p.forward, k, same_v, mbx, mby * 8, 16, 8, same, k, 2);
            const int m = (k == 0) == p.top_field_first ? 1 : 3;
            const int ov[2] = {((vx * m + (vx > 0)) >> 1) + pr.dmv[0],
                ((vy * m + (vy > 0)) >> 1) + pr.dmv[1] + (k ? 1 : -1)};
            predict_part(p.forward, 1 - k, ov, mbx, mby * 8, 16, 8, other, k, 2);
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 16; j++) {
                    const int at = (k + 2 * i) * 16 + j;
                    b.y[at] = static_cast<u8>((same.y[at] + other.y[at] + 1) >> 1);
                }
                if (i < 4)
                    for (int c = 0; c < 2; c++)
                        for (int j = 0; j < 8; j++) {
                            const int at = (k + 2 * i) * 8 + j;
                            b.c[c][at] = static_cast<u8>((same.c[c][at] + other.c[c][at] + 1) >> 1);
                        }
            }
        }
    }
    void dual_prime_field(const Prediction& pr, int mbx, int mby, Block16& b) const
    {
        const int vx = pr.v[0][0][0], vy = pr.v[0][0][1];
        Block16 other{};
        const int same_v[2] = {vx, vy};
        predict_part(reference(0, parity), parity, same_v, mbx, mby * 16, 16, 16, b, 0, 1);
        const int ov[2] = {((vx + (vx > 0)) >> 1) + pr.dmv[0], ((vy + (vy > 0)) >> 1) + pr.dmv[1] + (parity ? 1 : -1)};
        predict_part(reference(0, 1 - parity), 1 - parity, ov, mbx, mby * 16, 16, 16, other, 0, 1);
        average(b, other);
    }

    // ---- the macroblock into the frame

    // The macroblock's line i (of 16 luma or 8 chroma) as a frame line.
    int frame_line(int mby, int i, int lines) const { return frame ? mby * lines + i : (mby * lines + i) * 2 + parity; }

    void store(int mbx, int mby, const Block16& b) const
    {
        if (shift) { // a slice out of place: written where the slice before it ended (slice()), at most just past the
                     // picture's last macroblock
            const int at = mby * p.mb_width + mbx + shift;
            mbx = at % p.mb_width;
            mby = at / p.mb_width;
        }
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++) p.out.luma(mbx * 16 + j, frame_line(mby, i, 16)) = b.y[i * 16 + j];
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 8; j++) p.out.chroma(c, mbx * 8 + j, frame_line(mby, i, 8)) = b.c[c][i * 8 + j];
    }

    // The residual (or an intra macroblock's samples) of the coded blocks onto b (dct_type 1: the luma blocks are a
    // field's lines each).
    void add_blocks(int cbp, bool intra, bool field_dct, Block16& b)
    {
        for (int i = 0; i < 6; i++) {
            if (!(cbp & (32 >> i))) continue;
            idct(coef[i]);
            const s16* f = coef[i];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    u8* at;
                    if (i < 4) {
                        const int line = field_dct ? (i >> 1) + 2 * y : (i >> 1) * 8 + y;
                        at = &b.y[line * 16 + (i & 1) * 8 + x];
                    } else {
                        at = &b.c[i - 4][y * 8 + x];
                    }
                    *at = static_cast<u8>(clip255((intra ? 0 : *at) + f[y * 8 + x]));
                }
        }
    }

    // A skipped macroblock: P ones from the forward reference with no motion, B ones as the macroblock before them.
    void skipped(int mbx, int mby)
    {
        Block16 b{};
        if (p.type == 2) {
            Prediction pr;
            pr.type = MB_FORWARD;
            pr.motion_type = frame ? 2 : 1;
            pr.select[0][0] = pr.select[1][0] = parity;
            predict_direction(0, pr, mbx, mby, b);
            out.forward = true;
        } else {
            if (last.type & MB_FORWARD) predict_direction(0, last, mbx, mby, b);
            if (last.type & MB_BACKWARD) {
                Block16 back{};
                predict_direction(1, last, mbx, mby, back);
                if (last.type & MB_FORWARD) average(b, back);
                else b = back;
            }
            if (last.type & MB_FORWARD) out.forward = true;
            if (last.type & MB_BACKWARD) out.backward = true;
        }
        store(mbx, mby, b);
    }

    // A macroblock (after its address increment); false on an error.
    bool macroblock(int mbx, int mby)
    {
        int type = 0;
        const Vlc& types = p.type == 1 ? type_i() : p.type == 2 ? type_p() : type_b();
        if (p.type == 4) {
            if (!r.bits(1)) return false;
            type = MB_INTRA;
        } else if (!r.code(types, type)) {
            return false;
        }
        Prediction pr;
        pr.type = type;
        if (!p.mpeg1 && (type & (MB_FORWARD | MB_BACKWARD))) {
            if (frame) pr.motion_type = p.frame_pred_frame_dct ? 2 : static_cast<int>(r.bits(2));
            else pr.motion_type = static_cast<int>(r.bits(2));
            if (!pr.motion_type) return false;
        } else if (!frame) {
            pr.motion_type = 1;
        }
        bool field_dct = false;
        if (!p.mpeg1 && frame && !p.frame_pred_frame_dct && (type & (MB_INTRA | MB_PATTERN)))
            field_dct = r.bits(1) != 0;
        if (type & MB_QUANT) {
            const u32 code = r.bits(5);
            if (!code) return false;
            quantiser(code);
        }
        const bool intra = type & MB_INTRA;
        const bool concealment = intra && p.concealment_motion_vectors && !p.mpeg1;
        if ((type & MB_FORWARD) || concealment) {
            if (!motion_vectors(0, concealment ? (frame ? 2 : 1) : pr.motion_type, pr)) return false;
        }
        if (type & MB_BACKWARD) {
            if (!motion_vectors(1, pr.motion_type, pr)) return false;
        }
        if (concealment) r.skip(1);
        int cbp = intra ? 63 : 0;
        if (type & MB_PATTERN) {
            if (!r.code(block_pattern(), cbp)) return false;
        }
        if (!intra) reset_dc();
        u32 sum = 0;
        for (int i = 0; i < 6; i++)
            if (cbp & (32 >> i))
                if (!block(i, intra, sum)) return false;
        if (p.type == 4 && !r.bits(1)) return false;
        if (r.over) return false;
        // The samples.
        Block16 b{};
        if (!intra) {
            if (type & MB_FORWARD) predict_direction(0, pr, mbx, mby, b);
            if (type & MB_BACKWARD) {
                Block16 back{};
                predict_direction(1, pr, mbx, mby, back);
                if (type & MB_FORWARD) average(b, back);
                else b = back;
            }
            if (!(type & (MB_FORWARD | MB_BACKWARD))) {
                // P pictures' no motion compensation: the forward reference, no motion (the same parity's field).
                Prediction zero;
                zero.motion_type = frame ? 2 : 1;
                zero.select[0][0] = zero.select[1][0] = parity;
                predict_direction(0, zero, mbx, mby, b);
            }
        }
        add_blocks(cbp, intra, field_dct, b);
        store(mbx, mby, b);
        // The motion vector predictions (7.6.3.4): reset by intra macroblocks (without concealment vectors) and P
        // pictures' macroblocks with no forward motion.
        if (intra && !concealment) reset_pmv();
        if (p.type == 2 && !intra && !(type & MB_FORWARD)) reset_pmv();
        // What the program counts of it.
        if (intra) {
            lanes += 4 * 2560;
        } else {
            if (type & MB_PATTERN) lanes += sum + 4 * 32;
            if (type & (MB_FORWARD | MB_BACKWARD)) {
                const bool both = (type & MB_FORWARD) && (type & MB_BACKWARD);
                const bool two = !p.mpeg1 && ((frame && pr.motion_type == 1) || (!frame && pr.motion_type == 2));
                if (!p.mpeg1 && pr.motion_type == 3) weight = (type & MB_FORWARD) && !(type & MB_BACKWARD) ? 4 : 0;
                else weight = (two ? 2u : 4u) / (both ? 2u : 1u);
            }
            slice_motion += motion * weight;
            motion = 0;
            if (p.type == 2 || (type & MB_FORWARD)) out.forward = true;
            if (type & MB_BACKWARD) out.backward = true;
        }
        last = pr;
        return true;
    }

    // The slice whose start code is at the reader's place. Where its macroblocks go (what the program was seen to do):
    // a slice that starts where the slice before it ended (or in the row that one ended in: another slice of a row)
    // is written in its own place; one that does not (a slice missing, or out of order) damages the picture and is
    // written from where the slice before it ended (its own place still what it predicts from), until a macroblock
    // is written at or past the picture's end (the memory after the frame's last macroblock: the next frame buffer's
    // first, as on a PS3); then the next slice is expected after its own last macroblock. A slice whose position is
    // not in the picture is not decoded.
    void slice()
    {
        out.slices++;
        int position = static_cast<int>(r.bits(32) & 0xFF);
        if (!p.mpeg1 && p.vertical_size > 2800) position += static_cast<int>(r.bits(3)) << 7;
        quantiser(r.bits(5));
        if (p.mpeg1) {
            while (r.bits(1)) r.skip(8);
        } else if (r.peek(1)) {
            r.skip(9);
            while (r.bits(1)) r.skip(8);
        } else {
            r.skip(1);
        }
        reset_pmv();
        reset_dc();
        last = {};
        bool ok = position >= 1 && position <= p.mb_rows && !r.over;
        if (ok) {
            const int total = p.mb_width * p.mb_rows;
            int address = (position - 1) * p.mb_width - 1;
            const int row_end = position * p.mb_width;
            bool first = true;
            for (;;) {
                int increment = 0, step = 0;
                for (;;) {
                    if (!r.code(address_increment(), step)) {
                        ok = false;
                        break;
                    }
                    if (step == 35 && p.mpeg1) continue;
                    if (step == 34) {
                        increment += 33;
                        continue;
                    }
                    if (step == 35) ok = false;
                    break;
                }
                if (!ok) break;
                increment += step;
                if (address + increment >= total || (!p.mpeg1 && address + increment >= row_end)) {
                    ok = false;
                    break;
                }
                // Skipped macroblocks (not before the slice's first).
                if (!first)
                    for (int k = 1; k < increment; k++) {
                        const int at = address + k;
                        skipped(at % p.mb_width, at / p.mb_width);
                        out.macroblocks++;
                        reset_dc();
                        if (p.type == 2) reset_pmv();
                    }
                address += increment;
                if (first) {
                    shift = 0;
                    if (address != own_end && (own_end == 0 || address / p.mb_width != (own_end - 1) / p.mb_width)) {
                        out.damaged = true;
                        shift = own_end - address;
                    }
                }
                first = false;
                own_end = address + 1; // a macroblock in error too (not written)
                if (!macroblock(address % p.mb_width, address / p.mb_width)) {
                    ok = false;
                    break;
                }
                out.macroblocks++;
                if (shift && address + shift + 1 >= total) break; // written where the picture ends: the slice stops
                if (!r.peek(23) || r.over) break;
            }
        }
        if (!ok) out.damaged = true;
        // The slice's counts into its band.
        const int band = position / n;
        if (band >= 0 && band < 4) {
            if (p.type != 1) {
                out.bnr[4 + band] += lanes >> 8;
                lanes = 0;
            }
            out.bnr[band] += slice_motion;
        }
        slice_motion = 0;
    }
};

// The next start code (00 00 01) at or after byte `from`; `size` when none.
u32 next_start(const u8* d, u32 size, u32 from)
{
    for (u32 i = from; i + 3 <= size; i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) return i;
    return size;
}

} // namespace

Result decode(const u8* data, u32 size, const Picture& p)
{
    Decoder d(p, data, size);
    d.n = p.mb_rows / 4 + 1;
    // Each slice from its own start code, as the program finds them all before it decodes them: from the picture's
    // first (vertical position 1; those before it are passed over, and with none the picture is damaged and nothing of
    // it decoded).
    // After a slice, the next start code from where its reading stopped (a macroblock in error may have read on
    // through start codes: those slices are lost).
    bool started = false;
    for (u32 at = next_start(data, size, 0); at + 3 < size;) {
        const u8 code = data[at + 3];
        if (code < 0x01 || code > 0xAF) break;
        if (!started) {
            const bool extended = !p.mpeg1 && p.vertical_size > 2800;
            if (code != 1 || (extended && at + 4 < size && (data[at + 4] >> 5) != 0)) {
                at = next_start(data, size, at + 4);
                continue;
            }
            started = true;
        }
        d.r.pos = u64{at} * 8;
        d.r.over = false;
        d.slice();
        at = next_start(data, size, static_cast<u32>(std::max<u64>(at + 4, (d.r.pos + 7) / 8)));
    }
    if (!started) d.out.damaged = true;
    return d.out;
}

} // namespace pspocket::hle::mpeg2dec
