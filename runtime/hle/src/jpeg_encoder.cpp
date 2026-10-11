// pspocket's baseline JPEG encoder (D-615, D-913): libjpeg 6b's compression as Sony's libjpgenc builds it; see
// jpeg_encoder.hpp. The arithmetic follows IJG libjpeg 6b (jccolor.c, jcsample.c, jcprepct.c, jccoefct.c,
// jcdctmgr.c, jfdctfst.c, jfdctflt.c, jchuff.c, jcmarker.c, jcparam.c) and ITU-T T.81 annex K's tables.

#include "jpeg_encoder.hpp"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "pspocket/spu/fp.h"

// The float DCT must keep the PPU's operations as they are: fused multiply-adds only where they are written below.
#pragma clang fp contract(off)

namespace pspocket::hle::jpeg {
namespace {

// T.81 figure A.6: the zigzag order, as each zigzag position's natural index (libjpeg's jpeg_natural_order).
constexpr std::array<u8, 64> make_zigzag()
{
    std::array<u8, 64> z{};
    usize k = 0;
    for (int s = 0; s <= 14; s++) {
        const int lo = s > 7 ? s - 7 : 0, hi = s < 7 ? s : 7;
        if (s & 1) {
            for (int row = lo; row <= hi; row++) z[k++] = static_cast<u8>(row * 8 + s - row);
        } else {
            for (int row = hi; row >= lo; row--) z[k++] = static_cast<u8>(row * 8 + s - row);
        }
    }
    return z;
}
constexpr std::array<u8, 64> kZigzag = make_zigzag();
static_assert(kZigzag[2] == 8 && kZigzag[3] == 16 && kZigzag[5] == 2 && kZigzag[63] == 63 && kZigzag[62] == 62);

// T.81 annex K.1, tables K.1 (luminance) and K.2 (chrominance), in natural order (libjpeg's std_*_quant_tbl).
constexpr std::array<u8, 64> kLumaQ = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24,
    40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<u8, 64> kChromaQ = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56,
    99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// T.81 annex K.3, tables K.3 to K.6 (libjpeg's std_huff_tables): BITS (codes of each length 1-16) and HUFFVAL.
struct HuffSpec {
    std::array<u8, 16> bits;
    std::vector<u8> values;
};
const HuffSpec kDcLuma = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffSpec kDcChroma = {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffSpec kAcLuma = {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
        0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
        0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
        0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65,
        0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
        0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9,
        0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
        0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
        0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}};
const HuffSpec kAcChroma = {{0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
        0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16,
        0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64,
        0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86,
        0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
        0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8,
        0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
        0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}};

struct Code {
    u16 code = 0;
    u8 length = 0;
};
using CodeTable = std::array<Code, 256>;

// Annex C (libjpeg's jpeg_make_c_derived_tbl): each symbol's code and length.
CodeTable codes_of(const HuffSpec& spec)
{
    CodeTable t{};
    u32 code = 0;
    usize k = 0;
    for (u32 len = 1; len <= 16; len++) {
        for (u32 i = 0; i < spec.bits[len - 1]; i++) {
            t[spec.values[k++]] = {static_cast<u16>(code), static_cast<u8>(len)};
            code++;
        }
        code <<= 1;
    }
    return t;
}

// jcdctmgr.c: the fast DCT's divisors are each table entry times its AA&N scale factor (2^14), in 2^3ths; the float
// DCT's, 1 / (entry x scale factor (row) x scale factor (column) x 8). The scale factors as libjpeg writes them.
constexpr std::array<s16, 64> kAanScales = {16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520, 22725, 31521, 29692,
    26722, 22725, 17855, 12299, 6270, 21407, 29692, 27969, 25172, 21407, 16819, 11585, 5906, 19266, 26722, 25172, 22654,
    19266, 15137, 10426, 5315, 16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520, 12873, 17855, 16819, 15137, 12873,
    10114, 6967, 3552, 8867, 12299, 11585, 10426, 8867, 6967, 4799, 2446, 4520, 6270, 5906, 5315, 4520, 3552, 2446, 1247};
constexpr std::array<double, 8> kAanScaleFactor = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958,
    0.541196100, 0.275899379};

// jfdctfst.c: the AA&N DCT in 8-bit fixed point, products shifted right without rounding.
void fdct_fast(int* d)
{
    constexpr int k0382 = 98, k0541 = 139, k0707 = 181, k1306 = 334;
    const auto mul = [](int v, int c) { return (v * c) >> 8; };
    for (int pass = 0; pass < 2; pass++) {
        const int step = pass ? 8 : 1, next = pass ? 1 : 8;
        for (int i = 0; i < 8; i++) {
            int* p = d + i * next;
            const int tmp0 = p[0] + p[7 * step], tmp7 = p[0] - p[7 * step];
            const int tmp1 = p[step] + p[6 * step], tmp6 = p[step] - p[6 * step];
            const int tmp2 = p[2 * step] + p[5 * step], tmp5 = p[2 * step] - p[5 * step];
            const int tmp3 = p[3 * step] + p[4 * step], tmp4 = p[3 * step] - p[4 * step];
            int tmp10 = tmp0 + tmp3;
            const int tmp13 = tmp0 - tmp3;
            int tmp11 = tmp1 + tmp2;
            int tmp12 = tmp1 - tmp2;
            p[0] = tmp10 + tmp11;
            p[4 * step] = tmp10 - tmp11;
            const int z1 = mul(tmp12 + tmp13, k0707);
            p[2 * step] = tmp13 + z1;
            p[6 * step] = tmp13 - z1;
            tmp10 = tmp4 + tmp5;
            tmp11 = tmp5 + tmp6;
            tmp12 = tmp6 + tmp7;
            const int z5 = mul(tmp10 - tmp12, k0382);
            const int z2 = mul(tmp10, k0541) + z5;
            const int z4 = mul(tmp12, k1306) + z5;
            const int z3 = mul(tmp11, k0707);
            const int z11 = tmp7 + z3, z13 = tmp7 - z3;
            p[5 * step] = z13 + z2;
            p[3 * step] = z13 - z2;
            p[step] = z11 + z4;
            p[7 * step] = z11 - z4;
        }
    }
}

// jfdctflt.c in double precision, z2 and z4 by fused multiply-adds (libjpgenc's build).
void fdct_float(double* d)
{
    const double c0707 = 0.707106781, c0382 = 0.382683433, c0541 = 0.541196100, c1306 = 1.306562965;
    for (int pass = 0; pass < 2; pass++) {
        const int step = pass ? 8 : 1, next = pass ? 1 : 8;
        for (int i = 0; i < 8; i++) {
            double* p = d + i * next;
            const double tmp0 = p[0] + p[7 * step], tmp7 = p[0] - p[7 * step];
            const double tmp1 = p[step] + p[6 * step], tmp6 = p[step] - p[6 * step];
            const double tmp2 = p[2 * step] + p[5 * step], tmp5 = p[2 * step] - p[5 * step];
            const double tmp3 = p[3 * step] + p[4 * step], tmp4 = p[3 * step] - p[4 * step];
            double tmp10 = tmp0 + tmp3;
            const double tmp13 = tmp0 - tmp3;
            double tmp11 = tmp1 + tmp2;
            double tmp12 = tmp1 - tmp2;
            p[0] = tmp10 + tmp11;
            p[4 * step] = tmp10 - tmp11;
            const double z1 = (tmp12 + tmp13) * c0707;
            p[2 * step] = tmp13 + z1;
            p[6 * step] = tmp13 - z1;
            tmp10 = tmp4 + tmp5;
            tmp11 = tmp5 + tmp6;
            tmp12 = tmp6 + tmp7;
            const double z5 = (tmp10 - tmp12) * c0382;
            const double z2 = std::fma(tmp10, c0541, z5);
            const double z4 = std::fma(tmp12, c1306, z5);
            const double z3 = tmp11 * c0707;
            const double z11 = tmp7 + z3, z13 = tmp7 - z3;
            p[5 * step] = z13 + z2;
            p[3 * step] = z13 - z2;
            p[step] = z11 + z4;
            p[7 * step] = z11 - z4;
        }
    }
}

// ---- libjpgenc's SPU encoder (Dct::Spu), read from its SPU program ----
// Single precision as the SPU computes it, in pspocket's model of it (pspocket/spu/fp.h, the one the SPU programs run
// on): each result the exact one truncated toward zero to 24 bits, results below 2^-126 zero; a product with a zero
// operand +0 and a multiply-add with one its addend. The exact sum comes from TwoSum: s + err.
float spu_round(double s, double err)
{
    float f = static_cast<float>(s);
    const double fd = static_cast<double>(f);
    if (std::fabs(fd) > std::fabs(s) || (fd == s && err != 0 && (err < 0) != (s < 0))) f = std::nextafter(f, 0.0f);
    return std::fabs(f) < FLT_MIN ? 0.0f : f;
}
float spu_sum(double a, double b)
{
    const double s = a + b, bb = s - a, err = (a - (s - bb)) + (b - bb);
    return spu_round(s, err);
}
float spu_fa(float a, float b) { return spu_sum(static_cast<double>(a), static_cast<double>(b)); }
float spu_fs(float a, float b) { return spu_sum(static_cast<double>(a), -static_cast<double>(b)); }
float spu_fm(float a, float b)
{
    return a == 0 || b == 0 ? 0.0f : spu_round(static_cast<double>(a) * static_cast<double>(b), 0);
}
float spu_fma(float a, float b, float c)
{
    return a == 0 || b == 0 ? c : spu_sum(static_cast<double>(a) * static_cast<double>(b), static_cast<double>(c));
}
float spu_fms(float a, float b, float c) { return spu_fma(a, b, -c); }
float spu_fnms(float a, float b, float c) { return spu_fma(-a, b, c); }
// A double truncated to single precision (the SPU program's colour weights are the decimal ones truncated).
float spu_float(double v) { return spu_round(v, 0); }

// frest and fi: the SPU's reciprocal estimate and its interpolation (RPCS3's table and fixed-point model,
// rpcs3/Emu/Cell/SPUThread.cpp and SPULLVMRecompiler.cpp "spu_fi", as pspocket/spu/fp.h has them).
float spu_frest(float x)
{
    const u32 u = std::bit_cast<u32>(x);
    return std::bit_cast<float>(ps_spu_frest_fraction_lut[(u >> 18) & 0x1F] | ps_spu_frest_exponent_lut[(u >> 23) & 0xFF] |
        (u & 0x80000000u));
}
float spu_fi(float a, float b)
{
    const u32 ua = std::bit_cast<u32>(a), ub = std::bit_cast<u32>(b);
    const u32 base = (ub & 0x007FFC00u) << 9, ymul = (ub & 0x3FFu) * (ua & 0x7FFFFu);
    const bool adjust = ymul > base;
    const u32 bnew = (base - ymul) >> (adjust ? 8 : 9);
    const u32 expo = (ub & 0xFF800000u) - (adjust ? 0x00800000u : 0);
    return std::bit_cast<float>(expo | (bnew & 0x007FFFFFu));
}

// The quantisation's multipliers: each entry's DCT scale over the table's entry, the reciprocal from the estimate and
// one Newton step. The scales are the AA&N factors' products over 8 to six decimals, as libjpgenc's program has them.
std::array<float, 64> spu_multipliers(const std::array<u8, 64>& q)
{
    static constexpr double kS[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 1.175875602, 1.306562965,
        1.387039845};
    std::array<float, 64> m{};
    for (int i = 0; i < 64; i++) {
        const float c = static_cast<float>(std::round(kS[i / 8] * kS[i % 8] / 8 * 1e6) / 1e6);
        const float qf = static_cast<float>(q[static_cast<usize>(i)]);
        const float r = spu_fi(qf, spu_frest(qf));
        const float t = spu_fm(c, r);
        m[static_cast<usize>(i)] = spu_fma(spu_fnms(t, qf, c), r, t);
    }
    return m;
}

// The SPU program's 8-point DCT, factored by tangents (outputs in natural order, each scaled as the multipliers expect).
void spu_dct8(float* p, int step)
{
    constexpr float t1 = 0.414214f, t2 = 0.198912f, t3 = 0.668179f, c4 = 0.707107f;
    const float x0 = p[0], x1 = p[step], x2 = p[2 * step], x3 = p[3 * step], x4 = p[4 * step], x5 = p[5 * step],
                x6 = p[6 * step], x7 = p[7 * step];
    const float s07 = spu_fa(x0, x7), s34 = spu_fa(x3, x4), s16 = spu_fa(x1, x6), s25 = spu_fa(x2, x5);
    const float e0 = spu_fs(s07, s34), e1 = spu_fs(s16, s25);
    const float d16 = spu_fs(x1, x6), d25 = spu_fs(x2, x5), d07 = spu_fs(x0, x7), d34 = spu_fs(x3, x4);
    const float sum = spu_fa(d16, d25), diff = spu_fs(d16, d25);
    const float a = spu_fma(sum, c4, d07), b = spu_fma(diff, c4, d34);
    const float c = spu_fma(diff, -c4, d34), d = spu_fma(sum, -c4, d07);
    const float f0 = spu_fa(s07, s34), f1 = spu_fa(s16, s25);
    p[0] = spu_fa(f0, f1);
    p[step] = spu_fma(b, t2, a);
    p[2 * step] = spu_fma(e1, t1, e0);
    p[3 * step] = spu_fma(c, -t3, d);
    p[4 * step] = spu_fs(f0, f1);
    p[5 * step] = spu_fma(d, t3, c);
    p[6 * step] = spu_fms(e0, t1, e1);
    p[7 * step] = spu_fms(a, t2, b);
}

// libjpgenc's SPU colour conversion: each channel the sum of two truncated products-and-sums, truncated to a byte (at
// most 255, at least 0), with a half and 128 and a half as the program has them (a little over and under).
void spu_rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr)
{
    static const float kR[3] = {spu_float(0.299), spu_float(-0.16874), 0.5f};
    static const float kG[3] = {spu_float(0.587), spu_float(-0.33126), spu_float(-0.41869)};
    static const float kB[3] = {spu_float(0.114), 0.5f, spu_float(-0.08131)};
    static const float kOff[3] = {static_cast<float>(0.5 + 150.0 / 16777216.0), static_cast<float>(128.5 - 7.0 / 65536.0),
        static_cast<float>(128.5 - 7.0 / 65536.0)};
    u8* out[3] = {&y, &cb, &cr};
    for (int k = 0; k < 3; k++) {
        const float v = spu_fa(spu_fma(r, kR[k], spu_fm(g, kG[k])), spu_fma(b, kB[k], kOff[k]));
        *out[k] = static_cast<u8>(v <= 0 ? 0 : v >= 255 ? 255 : static_cast<int>(v));
    }
}

int clamp_quality(int q) { return std::clamp(q, 1, 100); }

} // namespace

std::array<u8, 64> quant_table(bool luma, int quality)
{
    const int q = clamp_quality(quality);
    const long scale = q < 50 ? 5000 / q : 200 - 2 * q;
    const std::array<u8, 64>& basic = luma ? kLumaQ : kChromaQ;
    std::array<u8, 64> t{};
    for (usize i = 0; i < 64; i++) t[i] = static_cast<u8>(std::clamp((basic[i] * scale + 50) / 100, 1L, 255L));
    return t;
}

void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr)
{
    // jccolor.c: FIX(x) = x * 2^16 rounded; ONE_HALF for Y, ONE_HALF - 1 and 128 << 16 for Cb and Cr.
    y = static_cast<u8>((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
    cb = static_cast<u8>((-11059 * r - 21709 * g + 32768 * b + (128 << 16) + 32767) >> 16);
    cr = static_cast<u8>((32768 * r - 27439 * g - 5329 * b + (128 << 16) + 32767) >> 16);
}

u8 rgb_to_y(u8 r, u8 g, u8 b) { return static_cast<u8>((19595 * r + 38470 * g + 7471 * b + 32768) >> 16); }

struct Encoder::Impl {
    struct Component {
        u8 id = 0;
        int h = 1, v = 1, quant = 0, table = 0; // table: the Huffman tables (0, or 1 for YCbCr's chroma)
        u32 width_in_blocks = 0, height_in_blocks = 0, last_col_width = 1, last_row_height = 1;
        std::vector<u8> rows; // an iMCU row: v x 8 rows of width_in_blocks x 8 samples
        int last_dc = 0;
    };
    static Component component(u8 id, int h, int v, int quant, int table)
    {
        Component c;
        c.id = id;
        c.h = h;
        c.v = v;
        c.quant = quant;
        c.table = table;
        return c;
    }

    Settings s;
    Sink sink;
    bool ok = false, failed = false, bad = false, headers = false;
    int max_h = 1, max_v = 1;
    std::vector<Component> comps;
    std::array<std::array<u8, 64>, 2> quant{};
    std::array<int, 64> fast_div[2]{};
    std::array<double, 64> float_div[2]{};
    std::array<float, 64> spu_mul[2]{};
    std::array<u8, 3> spu_blank{}; // a zero pixel of the input in each component (Dct::Spu's right edge)
    CodeTable dc_codes[2], ac_codes[2];
    u32 mcus_per_row = 0, total_imcu_rows = 0, imcu_row = 0;
    u32 rows_done = 0, group_rows = 0, groups = 0;
    std::vector<std::vector<u8>> group; // per component: max_v rows of the image's width (colour converted)
    std::vector<u8> out;                // bytes emitted, not yet given to the sink
    u32 put_buffer = 0;
    int put_bits = 0;
    u32 restarts_to_go = 0, next_restart = 0;

    Impl(const Settings& settings, Sink k) : s(settings), sink(std::move(k))
    {
        if (!s.width || !s.height || s.width > 65535 || s.height > 65535) return;
        if (s.output == Space::YCbCr) {
            if (s.h_samp < 1 || s.h_samp > 4 || s.v_samp < 1 || s.v_samp > 4 || s.h_samp * s.v_samp + 2 > 10) return;
            comps = {component(1, s.h_samp, s.v_samp, 0, 0), component(2, 1, 1, 1, 1), component(3, 1, 1, 1, 1)};
        } else if (s.output == Space::RGB) {
            comps = {component('R', 1, 1, 0, 0), component('G', 1, 1, 0, 0), component('B', 1, 1, 0, 0)};
        } else {
            comps = {component(1, 1, 1, 0, 0)};
        }
        // jpeg_set_colorspace takes only a converter libjpeg has (jinit_color_converter).
        if ((s.output == Space::RGB && s.input != Space::RGB) || (s.output == Space::YCbCr && s.input == Space::Gray))
            return;
        for (const Component& c : comps) {
            max_h = std::max(max_h, c.h);
            max_v = std::max(max_v, c.v);
        }
        for (Component& c : comps) {
            c.width_in_blocks = (s.width * c.h + max_h * 8 - 1) / (max_h * 8);
            c.height_in_blocks = (s.height * c.v + max_v * 8 - 1) / (max_v * 8);
            c.last_col_width = c.width_in_blocks % c.h ? c.width_in_blocks % c.h : c.h;
            c.last_row_height = c.height_in_blocks % c.v ? c.height_in_blocks % c.v : c.v;
            c.rows.assign(static_cast<usize>(c.v) * 8 * c.width_in_blocks * 8, 0);
        }
        if (comps.size() == 1) {
            mcus_per_row = comps[0].width_in_blocks; // non-interleaved: an MCU a block
            comps[0].last_col_width = comps[0].last_row_height = 1;
        } else {
            mcus_per_row = (s.width + max_h * 8 - 1) / (max_h * 8);
        }
        total_imcu_rows = (s.height + max_v * 8 - 1) / (max_v * 8);
        for (int t = 0; t < 2; t++) {
            quant[t] = quant_table(t == 0, s.quality);
            for (int i = 0; i < 64; i++) {
                fast_div[t][i] = (quant[t][i] * kAanScales[i] + (1 << 10)) >> 11;
                float_div[t][i] =
                    1.0 / (static_cast<double>(quant[t][i]) * kAanScaleFactor[i / 8] * kAanScaleFactor[i % 8] * 8.0);
            }
            if (s.dct == Dct::Spu) spu_mul[t] = spu_multipliers(quant[t]);
        }
        dc_codes[0] = codes_of(kDcLuma);
        ac_codes[0] = codes_of(kAcLuma);
        dc_codes[1] = codes_of(kDcChroma);
        ac_codes[1] = codes_of(kAcChroma);
        group.assign(comps.size(), std::vector<u8>(static_cast<usize>(max_v) * s.width));
        if (s.output == Space::YCbCr && s.input == Space::RGB)
            spu_rgb_to_ycbcr(0, 0, 0, spu_blank[0], spu_blank[1], spu_blank[2]);
        restarts_to_go = s.restart_interval;
        ok = true;
    }

    bool flush()
    {
        if (failed) return false;
        if (!out.empty() && !sink(out.data(), out.size())) failed = true;
        out.clear();
        return !failed;
    }

    void byte(u8 b) { out.push_back(b); }
    void two(u32 v)
    {
        byte(static_cast<u8>(v >> 8));
        byte(static_cast<u8>(v));
    }
    void marker_head(u8 m, u32 length)
    {
        byte(0xFF);
        byte(m);
        two(length);
    }

    // jchuff.c's emit_bits: a 24-bit buffer, whole bytes out, 0xFF followed by a stuffed 0.
    void bits(u32 code, int size)
    {
        u32 buffer = code & ((1u << size) - 1);
        put_bits += size;
        buffer <<= 24 - put_bits;
        buffer |= put_buffer;
        while (put_bits >= 8) {
            const u8 c = static_cast<u8>(buffer >> 16);
            byte(c);
            if (c == 0xFF) byte(0);
            buffer <<= 8;
            put_bits -= 8;
        }
        put_buffer = buffer & 0xFFFFFF;
    }
    void flush_bits()
    {
        bits(0x7F, 7);
        put_buffer = 0;
        put_bits = 0;
    }

    // jcmarker.c's write_frame_header and write_scan_header.
    void write_headers()
    {
        bool sent_q[2] = {}, sent_dc[2] = {}, sent_ac[2] = {};
        for (const Component& c : comps) {
            if (sent_q[c.quant]) continue;
            sent_q[c.quant] = true;
            marker_head(0xDB, 64 + 1 + 2);
            byte(static_cast<u8>(c.quant));
            for (int i = 0; i < 64; i++) byte(quant[c.quant][kZigzag[i]]);
        }
        marker_head(0xC0, static_cast<u32>(3 * comps.size() + 8));
        byte(8);
        two(s.height);
        two(s.width);
        byte(static_cast<u8>(comps.size()));
        for (const Component& c : comps) {
            byte(c.id);
            byte(static_cast<u8>((c.h << 4) + c.v));
            byte(static_cast<u8>(c.quant));
        }
        const auto dht = [&](const HuffSpec& spec, u8 index) {
            u32 count = 0;
            for (const u8 n : spec.bits) count += n;
            marker_head(0xC4, count + 2 + 1 + 16);
            byte(index);
            for (const u8 n : spec.bits) byte(n);
            for (u32 i = 0; i < count; i++) byte(spec.values[i]);
        };
        for (const Component& c : comps) {
            if (!sent_dc[c.table]) {
                sent_dc[c.table] = true;
                dht(c.table ? kDcChroma : kDcLuma, static_cast<u8>(c.table));
            }
            if (!sent_ac[c.table]) {
                sent_ac[c.table] = true;
                dht(c.table ? kAcChroma : kAcLuma, static_cast<u8>(0x10 + c.table));
            }
        }
        if (s.restart_interval) {
            marker_head(0xDD, 4);
            two(s.restart_interval);
        }
        marker_head(0xDA, static_cast<u32>(2 * comps.size() + 6));
        byte(static_cast<u8>(comps.size()));
        for (const Component& c : comps) {
            byte(c.id);
            byte(static_cast<u8>((c.table << 4) + c.table));
        }
        byte(0);
        byte(63);
        byte(0);
    }

    // jcsample.c's expand_right_edge: a row's last sample repeated to `to` columns. libjpgenc's SPU encoder converts its
    // row buffers whole: past the picture they are black (a zero pixel converted: `blank`).
    void expand(u8* row, u32 from, u32 to, u8 blank) const
    {
        if (to > from) std::memset(row + from, s.dct == Dct::Spu ? blank : row[from - 1], to - from);
    }

    // One row group (max_v rows, the image's last repeated past its bottom) into each component's iMCU rows: jcsample.c.
    void downsample_group()
    {
        for (usize ci = 0; ci < comps.size(); ci++) {
            Component& c = comps[ci];
            const u32 out_cols = c.width_in_blocks * 8;
            const int hx = max_h / c.h, vx = max_v / c.v;
            std::vector<u8> in(static_cast<usize>(max_v) * out_cols * hx);
            for (int r = 0; r < max_v; r++) {
                u8* dst = in.data() + static_cast<usize>(r) * out_cols * hx;
                std::memcpy(dst, group[ci].data() + static_cast<usize>(r) * s.width, s.width);
                expand(dst, s.width, out_cols * hx, spu_blank[ci]);
            }
            const usize stride = static_cast<usize>(out_cols) * hx;
            for (int orow = 0; orow < c.v; orow++) {
                u8* dst = c.rows.data() + (static_cast<usize>(groups) * c.v + orow) * out_cols;
                if (hx == 1 && vx == 1) {
                    std::memcpy(dst, in.data() + orow * stride, out_cols);
                } else if (hx == 2 && vx == 1) {
                    const u8* p = in.data() + orow * stride;
                    int bias = 0;
                    for (u32 x = 0; x < out_cols; x++) {
                        dst[x] = static_cast<u8>((p[2 * x] + p[2 * x + 1] + bias) >> 1);
                        bias ^= 1;
                    }
                } else if (hx == 2 && vx == 2) {
                    const u8* p0 = in.data() + (2 * orow) * stride;
                    const u8* p1 = p0 + stride;
                    int bias = 1;
                    for (u32 x = 0; x < out_cols; x++) {
                        dst[x] = static_cast<u8>((p0[2 * x] + p0[2 * x + 1] + p1[2 * x] + p1[2 * x + 1] + bias) >> 2);
                        bias ^= 3;
                    }
                } else {
                    const int n = hx * vx;
                    for (u32 x = 0; x < out_cols; x++) {
                        int sum = 0;
                        for (int vv = 0; vv < vx; vv++)
                            for (int hh = 0; hh < hx; hh++) sum += in[(orow * vx + vv) * stride + x * hx + hh];
                        dst[x] = static_cast<u8>((sum + n / 2) / n);
                    }
                }
            }
        }
        groups++;
    }

    // The forward DCT of one block (jcdctmgr.c's forward_DCT), its coefficients quantised, in natural order.
    void dct_block(const Component& c, u32 row0, u32 col0, int* coef)
    {
        const u32 cols = c.width_in_blocks * 8;
        const u8* p = c.rows.data() + static_cast<usize>(row0) * cols + col0;
        if (s.dct == Dct::Spu) {
            // Columns, then rows; each coefficient times its multiplier plus 16384.5, truncated, less 16384.
            float w[64];
            for (int r = 0; r < 8; r++)
                for (int x = 0; x < 8; x++) w[r * 8 + x] = static_cast<float>(p[r * cols + x] - 128);
            for (int x = 0; x < 8; x++) spu_dct8(w + x, 8);
            for (int r = 0; r < 8; r++) spu_dct8(w + r * 8, 1);
            for (int i = 0; i < 64; i++)
                coef[i] = static_cast<int>(spu_fma(w[i], spu_mul[c.quant][static_cast<usize>(i)], 16384.5f)) - 16384;
        } else if (s.dct == Dct::Fast) {
            int w[64];
            for (int r = 0; r < 8; r++)
                for (int x = 0; x < 8; x++) w[r * 8 + x] = p[r * cols + x] - 128;
            fdct_fast(w);
            for (int i = 0; i < 64; i++) {
                const int q = fast_div[c.quant][i];
                int t = w[i];
                const bool neg = t < 0;
                if (neg) t = -t;
                t += q >> 1;
                t = t >= q ? t / q : 0;
                coef[i] = neg ? -t : t;
            }
        } else {
            double w[64];
            for (int r = 0; r < 8; r++)
                for (int x = 0; x < 8; x++) w[r * 8 + x] = static_cast<double>(p[r * cols + x] - 128);
            fdct_float(w);
            for (int i = 0; i < 64; i++)
                coef[i] = static_cast<int>(std::fma(w[i], float_div[c.quant][i], 16384.5)) - 16384;
        }
    }

    // jchuff.c's encode_one_block.
    bool encode_block(const int* coef, int& last_dc, int table)
    {
        int t = coef[0] - last_dc, t2 = t;
        if (t < 0) {
            t = -t;
            t2--;
        }
        int nbits = 0;
        while (t) {
            nbits++;
            t >>= 1;
        }
        if (nbits > 11) return false;
        bits(dc_codes[table][nbits].code, dc_codes[table][nbits].length);
        if (nbits) bits(static_cast<u32>(t2), nbits);
        int run = 0;
        for (int k = 1; k < 64; k++) {
            t = coef[kZigzag[k]];
            if (!t) {
                run++;
                continue;
            }
            while (run > 15) {
                bits(ac_codes[table][0xF0].code, ac_codes[table][0xF0].length);
                run -= 16;
            }
            t2 = t;
            if (t < 0) {
                t = -t;
                t2--;
            }
            nbits = 1;
            while ((t >>= 1)) nbits++;
            if (nbits > 10) return false;
            const int sym = (run << 4) + nbits;
            bits(ac_codes[table][sym].code, ac_codes[table][sym].length);
            bits(static_cast<u32>(t2), nbits);
            run = 0;
        }
        if (run > 0) bits(ac_codes[table][0].code, ac_codes[table][0].length);
        last_dc = coef[0];
        return true;
    }

    // jccoefct.c's compress_data for one iMCU row, each MCU encoded (jchuff.c's encode_mcu_huff with its restarts).
    bool compress_imcu_row()
    {
        const bool last_row = imcu_row + 1 == total_imcu_rows;
        const u32 mcu_rows = comps.size() == 1 ? static_cast<u32>(comps[0].v) : 1;
        std::vector<std::array<int, 64>> blocks;
        for (u32 yoff = 0; yoff < mcu_rows; yoff++) {
            if (comps.size() == 1 && last_row && yoff >= 1) break; // non-interleaved: the image's last block row
            for (u32 col = 0; col < mcus_per_row; col++) {
                blocks.clear();
                std::vector<int> owner;
                for (usize ci = 0; ci < comps.size(); ci++) {
                    const Component& c = comps[ci];
                    const u32 mw = comps.size() == 1 ? 1 : static_cast<u32>(c.h);
                    const u32 mh = comps.size() == 1 ? 1 : static_cast<u32>(c.v);
                    const u32 count = col + 1 < mcus_per_row ? mw : c.last_col_width;
                    for (u32 yi = 0; yi < mh; yi++) {
                        const usize first = blocks.size();
                        blocks.resize(first + mw);
                        owner.insert(owner.end(), mw, static_cast<int>(ci));
                        if (!last_row || yoff + yi < c.last_row_height) {
                            for (u32 bi = 0; bi < count; bi++)
                                dct_block(c, (yoff + yi) * 8, (col * mw + bi) * 8, blocks[first + bi].data());
                            for (u32 bi = count; bi < mw; bi++) {
                                blocks[first + bi].fill(0);
                                blocks[first + bi][0] = blocks[first + bi - 1][0];
                            }
                        } else {
                            for (u32 bi = 0; bi < mw; bi++) {
                                blocks[first + bi].fill(0);
                                blocks[first + bi][0] = blocks[first - 1][0];
                            }
                        }
                    }
                }
                if (s.restart_interval && restarts_to_go == 0) { // emit_restart
                    flush_bits();
                    byte(0xFF);
                    byte(static_cast<u8>(0xD0 + next_restart));
                    for (Component& c : comps) c.last_dc = 0;
                }
                for (usize b = 0; b < blocks.size(); b++) {
                    Component& c = comps[static_cast<usize>(owner[b])];
                    if (!encode_block(blocks[b].data(), c.last_dc, c.table)) {
                        bad = true;
                        return false;
                    }
                }
                if (s.restart_interval) {
                    if (restarts_to_go == 0) {
                        restarts_to_go = s.restart_interval;
                        next_restart = (next_restart + 1) & 7;
                    }
                    restarts_to_go--;
                }
            }
        }
        imcu_row++;
        return true;
    }
};

Encoder::Encoder(const Settings& s, Sink sink) : impl_(new Impl(s, std::move(sink))) {}
Encoder::~Encoder() { delete impl_; }

bool Encoder::valid() const { return impl_->ok; }
u32 Encoder::rows() const { return impl_->rows_done; }
bool Encoder::bad_coefficient() const { return impl_->bad; }

bool Encoder::start()
{
    Impl& m = *impl_;
    if (!m.ok) return false;
    m.byte(0xFF);
    m.byte(0xD8);
    if (m.s.jfif) { // jcmarker.c's emit_jfif_app0: JFIF 1.01, no units, 1:1, no thumbnail
        m.marker_head(0xE0, 16);
        for (const u8 c : {u8{'J'}, u8{'F'}, u8{'I'}, u8{'F'}, u8{0}, u8{1}, u8{1}, u8{0}}) m.byte(c);
        m.two(1);
        m.two(1);
        m.byte(0);
        m.byte(0);
    }
    if (m.s.adobe) { // emit_adobe_app14: version 100, the transform (1 for YCbCr)
        m.marker_head(0xEE, 14);
        for (const u8 c : {u8{'A'}, u8{'d'}, u8{'o'}, u8{'b'}, u8{'e'}}) m.byte(c);
        m.two(100);
        m.two(0);
        m.two(0);
        m.byte(m.s.output == Space::YCbCr ? 1 : 0);
    }
    return m.flush();
}

bool Encoder::marker(u8 code, const u8* data, usize size)
{
    Impl& m = *impl_;
    if (!m.ok || m.failed || size > 65533) return false;
    m.marker_head(code, static_cast<u32>(size + 2));
    m.out.insert(m.out.end(), data, data + size);
    return m.flush();
}

bool Encoder::write_row(const u8* row)
{
    Impl& m = *impl_;
    if (!m.ok || m.failed || m.rows_done >= m.s.height) return false;
    if (!m.headers) {
        m.headers = true;
        m.write_headers();
    }
    // jccolor.c: the row into each component.
    const u32 w = m.s.width;
    const usize at = static_cast<usize>(m.group_rows) * w;
    for (u32 x = 0; x < w; x++) {
        if (m.s.input == Space::Gray) {
            m.group[0][at + x] = row[x];
            continue;
        }
        const u8* p = row + 3 * x;
        if (m.s.output == Space::Gray) {
            m.group[0][at + x] = m.s.input == Space::RGB ? rgb_to_y(p[0], p[1], p[2]) : p[0];
        } else if (m.s.output == Space::YCbCr && m.s.input == Space::RGB) {
            (m.s.dct == Dct::Spu ? spu_rgb_to_ycbcr : rgb_to_ycbcr)(p[0], p[1], p[2], m.group[0][at + x],
                m.group[1][at + x], m.group[2][at + x]);
        } else {
            for (int c = 0; c < 3; c++) m.group[static_cast<usize>(c)][at + x] = p[c];
        }
    }
    m.group_rows++;
    const bool last = m.rows_done + 1 == m.s.height;
    if (m.group_rows == static_cast<u32>(m.max_v) || last) {
        for (usize ci = 0; ci < m.comps.size(); ci++) // jcprepct.c: the last row group's missing rows repeat its last
            for (u32 r = m.group_rows; r < static_cast<u32>(m.max_v); r++)
                std::memcpy(m.group[ci].data() + static_cast<usize>(r) * w,
                    m.group[ci].data() + static_cast<usize>(m.group_rows - 1) * w, w);
        m.group_rows = 0;
        m.downsample_group();
        if (m.groups == 8 || last) {
            for (Impl::Component& c : m.comps) { // the iMCU row's missing rows repeat its last
                const usize cols = static_cast<usize>(c.width_in_blocks) * 8;
                for (u32 r = m.groups * c.v; r < static_cast<u32>(c.v) * 8; r++)
                    std::memcpy(c.rows.data() + r * cols, c.rows.data() + (m.groups * c.v - 1) * cols, cols);
            }
            m.groups = 0;
            if (!m.compress_imcu_row()) {
                m.failed = true;
                return false;
            }
        }
    }
    if (!m.flush()) return false;
    m.rows_done++;
    return true;
}

bool Encoder::finish()
{
    Impl& m = *impl_;
    if (!m.ok || m.failed || m.rows_done != m.s.height) return false;
    m.flush_bits();
    m.byte(0xFF);
    m.byte(0xD9);
    return m.flush();
}

std::vector<u8> encode(const Settings& s, const u8* pixels, usize pitch)
{
    std::vector<u8> out;
    Encoder e(s, [&](const u8* p, usize n) {
        out.insert(out.end(), p, p + n);
        return true;
    });
    if (!e.valid() || !e.start()) return {};
    for (u32 y = 0; y < s.height; y++)
        if (!e.write_row(pixels + y * pitch)) return {};
    if (!e.finish()) return {};
    return out;
}

} // namespace pspocket::hle::jpeg
