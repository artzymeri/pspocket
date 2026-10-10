// pspocket's JPEG decoder, sample for sample Sony's libjpgdec (see jpeg_decoder.hpp).

#include "jpeg_decoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

// Every fused multiply-add here is written as std::fma, where libjpgdec's code fuses one; nothing else may be fused.
#pragma STDC FP_CONTRACT OFF

namespace pspocket::hle::jpeg {
namespace {

constexpr int kNatural[64 + 16] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31,
    39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63}; // extra entries for safety (libjpeg's)

u32 div_round_up(u32 a, u32 b) { return (a + b - 1) / b; }

// ---- 6b's range limit table (jdmaster.c prepare_range_limit_table): idct_limit[x & 0x3FF] for an IDCT output x.
struct RangeLimit {
    std::array<u8, 1024> idct{};
    RangeLimit()
    {
        // post-IDCT table: [0, 127] -> 128 + i; [128, 511] -> 255; [512, 895] -> 0; [896, 1023] -> i - 896.
        for (int i = 0; i < 1024; i++) {
            if (i < 128) idct[static_cast<usize>(i)] = static_cast<u8>(i + 128);
            else if (i < 512) idct[static_cast<usize>(i)] = 255;
            else if (i < 896) idct[static_cast<usize>(i)] = 0;
            else idct[static_cast<usize>(i)] = static_cast<u8>(i - 896);
        }
    }
};
const RangeLimit kRange;
inline u8 limit(s64 v) { return kRange.idct[static_cast<usize>(v & 0x3FF)]; }

constexpr s64 descale(s64 x, int n) { return (x + (s64{1} << (n - 1))) >> n; }

// ---- libjpgdec's fast method (method 5) on the PPU: libjpeg's dct_method 3, Sony's own, an AltiVec IDCT in the
// tangent form (the Motorola/Apple AltiVec IDCT's flow) in 16-bit fixed point (measured, run 107). The coefficients
// times the quantisation values (modulo 2^16, vmladduhm), times a prescale row (rounded Q15, vmhraddshs) that leaves
// them in sample units, then the same 1-D transform down the columns and along the rows, every multiply a rounded Q15
// one and every add saturating, then + 128 (modulo 2^16) and saturated to 0..255. No extra precision bits: the fast
// method is a few levels less exact than libjpeg's.
namespace altivec {
constexpr s32 kC4 = 23170, kA0 = 13573, kA1 = 6518, kA2 = 21895; // cos(pi/4), tan(pi/8), tan(pi/16), tan(3pi/16) in Q15
// The prescale for rows (and columns) 0/4, 1/7, 2/6, 3/5: (1/8) S[r] S[c] in Q15 as libjpgdec has it.
constexpr s16 kPrescale[4][8] = {{4095, 5681, 5351, 4816, 4095, 4816, 5351, 5681},
    {5681, 7880, 7422, 6680, 5681, 6680, 7422, 7880}, {5351, 7422, 6992, 6292, 5351, 6292, 6992, 7422},
    {4816, 6680, 6292, 5663, 4816, 5663, 6292, 6680}};
constexpr int kPrescaleRow[8] = {0, 1, 2, 3, 0, 3, 2, 1};

inline s16 sat(s32 v) { return static_cast<s16>(std::clamp(v, -32768, 32767)); }
inline s16 adds(s16 a, s16 b) { return sat(s32{a} + b); }
inline s16 subs(s16 a, s16 b) { return sat(s32{a} - b); }
inline s16 mhradd(s32 a, s16 b, s16 c) { return sat(((a * b + 0x4000) >> 15) + c); } // vmhraddshs

// One 1-D transform of x[k * step], k = 0..7, in place.
inline void pass(s16* x, int step)
{
    const s16 x0 = x[0], x1 = x[step], x2 = x[2 * step], x3 = x[3 * step], x4 = x[4 * step], x5 = x[5 * step],
              x6 = x[6 * step], x7 = x[7 * step];
    const s16 tm17 = subs(mhradd(kA1, x1, 0), x7), tp17 = mhradd(kA1, x7, x1);
    const s16 tp35 = mhradd(kA2, x5, x3), tm35 = mhradd(-kA2, x3, x5);
    const s16 s04 = adds(x0, x4), d04 = subs(x0, x4);
    const s16 m26 = subs(mhradd(kA0, x2, 0), x6), p26 = mhradd(kA0, x6, x2);
    const s16 m = adds(tm17, tm35), a = subs(tm17, tm35), b = subs(tp17, tp35), p = adds(tp17, tp35);
    const s16 e0 = adds(s04, p26), e3 = subs(s04, p26), e1 = adds(d04, m26), e2 = subs(d04, m26);
    const s16 s2 = subs(b, a), s1 = adds(b, a);
    x[0] = adds(e0, p);
    x[7 * step] = subs(e0, p);
    x[step] = mhradd(kC4, s1, e1);
    x[6 * step] = mhradd(-kC4, s1, e1);
    x[2 * step] = mhradd(kC4, s2, e2);
    x[5 * step] = mhradd(-kC4, s2, e2);
    x[3 * step] = adds(e3, m);
    x[4 * step] = subs(e3, m);
}

void idct(const s16* in, const u16* q, u8* out, usize stride)
{
    s16 ws[64];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            const int i = 8 * r + c;
            const s16 dq = static_cast<s16>(static_cast<u16>(static_cast<u16>(in[i]) * q[i])); // vmladduhm
            ws[i] = mhradd(kPrescale[kPrescaleRow[r]][c], dq, 0);
        }
    for (int c = 0; c < 8; c++) pass(ws + c, 8);
    for (int r = 0; r < 8; r++) pass(ws + 8 * r, 1);
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            const s16 v = static_cast<s16>(static_cast<u16>(static_cast<u16>(ws[8 * r + c]) + 128)); // vadduhm
            out[stride * static_cast<usize>(r) + static_cast<usize>(c)] = static_cast<u8>(std::clamp<int>(v, 0, 255));
        }
}
} // namespace altivec

// ---- libjpgdec's quality method (method 0): jidctflt.c with FAST_FLOAT double (measured, run 107), its quantisation
// table (jddctmgr.c) q * aanscale[row] * aanscale[col] in double, four of its multiply-adds fused as libjpgdec's
// compiler fused them; the result truncated to an integer, then descaled by 3 with rounding.
namespace flt {
constexpr double kAan[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};
constexpr double kC1414 = 1.414213562, kC1847 = 1.847759065, kC2613 = -2.613125930, kC1082 = 1.082392200;

void table(const u16* q, double* out)
{
    for (int i = 0; i < 64; i++) out[i] = static_cast<double>(q[i]) * kAan[i / 8] * kAan[i % 8];
}

inline s32 trunc_int(double v) // fctiwz
{
    if (!(v == v)) return INT32_MIN;
    return v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : static_cast<s32>(v);
}

void idct(const s16* in, const double* q, u8* out, usize stride)
{
    double ws[64];
    for (int c = 0; c < 8; c++) {
        const s16* ip = in + c;
        const double* qp = q + c;
        double* wp = ws + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[32] && !ip[40] && !ip[48] && !ip[56]) {
            const double dc = static_cast<double>(ip[0]) * qp[0];
            for (int k = 0; k < 8; k++) wp[8 * k] = dc;
            continue;
        }
        const double t0 = static_cast<double>(ip[0]) * qp[0], t1 = static_cast<double>(ip[16]) * qp[16];
        const double t2 = static_cast<double>(ip[32]) * qp[32], t3 = static_cast<double>(ip[48]) * qp[48];
        const double t4 = static_cast<double>(ip[8]) * qp[8], t5 = static_cast<double>(ip[24]) * qp[24];
        const double t6 = static_cast<double>(ip[40]) * qp[40], t7 = static_cast<double>(ip[56]) * qp[56];
        const double z10 = t6 - t5, z13 = t5 + t6, z12 = t4 - t7, z11 = t4 + t7;
        const double d13 = t1 - t3, tmp13 = t1 + t3;
        const double s1012 = z10 + z12;
        const double tmp7 = z13 + z11, d1113 = z11 - z13;
        const double tmp12 = std::fma(d13, kC1414, -tmp13);
        const double z5 = s1012 * kC1847;
        const double tmp11 = t0 - t2, tmp10 = t0 + t2;
        const double o12 = std::fma(z10, kC2613, z5);
        const double e2 = tmp11 - tmp12;
        const double o10 = std::fma(z12, kC1082, -z5);
        const double e1 = tmp11 + tmp12, e3 = tmp10 - tmp13, e0 = tmp10 + tmp13;
        const double tmp6 = o12 - tmp7;
        wp[56] = e0 - tmp7;
        wp[0] = e0 + tmp7;
        const double tmp5 = std::fma(d1113, kC1414, -tmp6);
        wp[48] = e1 - tmp6;
        wp[8] = e1 + tmp6;
        const double tmp4 = o10 + tmp5;
        wp[40] = e2 - tmp5;
        wp[16] = e2 + tmp5;
        wp[24] = e3 - tmp4;
        wp[32] = e3 + tmp4;
    }
    for (int r = 0; r < 8; r++) {
        const double* w = ws + 8 * r;
        u8* o = out + stride * static_cast<usize>(r);
        const double tmp10 = w[0] + w[4], tmp13 = w[2] + w[6], z13 = w[5] + w[3], z11 = w[1] + w[7];
        const double z12 = w[1] - w[7], z10 = w[5] - w[3], d26 = w[2] - w[6], tmp11 = w[0] - w[4];
        const double e0 = tmp10 + tmp13, e3 = tmp10 - tmp13;
        const double tmp7 = z13 + z11, d1113 = z11 - z13, s1012 = z10 + z12;
        const double tmp12 = std::fma(d26, kC1414, -tmp13);
        const auto put = [](double v) { return limit(descale(trunc_int(v), 3)); };
        o[0] = put(e0 + tmp7);
        o[7] = put(e0 - tmp7);
        const double z5 = s1012 * kC1847;
        const double e1 = tmp11 + tmp12, e2 = tmp11 - tmp12;
        const double o12 = std::fma(z10, kC2613, z5), o10 = std::fma(z12, kC1082, -z5);
        const double tmp6 = o12 - tmp7;
        o[1] = put(e1 + tmp6);
        o[6] = put(e1 - tmp6);
        const double tmp5 = std::fma(d1113, kC1414, -tmp6);
        o[2] = put(e2 + tmp5);
        o[5] = put(e2 - tmp5);
        const double tmp4 = o10 + tmp5;
        o[4] = put(e3 + tmp4);
        o[3] = put(e3 - tmp4);
    }
}
} // namespace flt

// ---- libjpgdec's reduced sizes (measured, run 107; not 6b's jidctred.c). 1/2: a four-lane float AltiVec IDCT of the
// block's low 4 x 4 coefficients (4-point, tangent form), the table q times (1/8) S[r] S[c] (S = 1, sqrt(2) cos(pi/8),
// to six significant digits) in single precision, multiply-adds fused, + 128.5 and truncated. 1/4: a 2-point float
// IDCT of the low 2 x 2 coefficients, + 0.5 and truncated. 1/8: 6b's (the DC alone, descaled).
namespace scaled {
constexpr float kT2 = 0.414214f, kHalf = 128.5f; // tan(pi/8)
constexpr float kTable4[4][4] = {{0.125f, 0.16332f, 0.125f, 0.16332f}, {0.16332f, 0.213388f, 0.16332f, 0.213388f},
    {0.125f, 0.16332f, 0.125f, 0.16332f}, {0.16332f, 0.213388f, 0.16332f, 0.213388f}};

void table4(const u16* q, float* out) // out[r * 4 + c]
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[4 * r + c] = static_cast<float>(q[8 * r + c]) * kTable4[r][c];
}

inline void pass4(float* x, int step) // in place, x[k * step]
{
    const float x0 = x[0], x1 = x[step], x2 = x[2 * step], x3 = x[3 * step];
    const float p = std::fma(x3, kT2, x1), m = std::fma(x1, kT2, -x3);
    const float s = x0 + x2, d = x0 - x2;
    x[0] = s + p;
    x[step] = d + m;
    x[2 * step] = d - m;
    x[3 * step] = s - p;
}

void idct4(const s16* in, const float* q, u8* out, usize stride)
{
    float ws[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) ws[4 * r + c] = static_cast<float>(in[8 * r + c]) * q[4 * r + c];
    for (int c = 0; c < 4; c++) pass4(ws + c, 4);
    for (int r = 0; r < 4; r++) pass4(ws + 4 * r, 1);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            const float v = ws[4 * r + c] + kHalf; // vctuxs, then packed with saturation
            out[stride * static_cast<usize>(r) + static_cast<usize>(c)] =
                !(v > 0) ? 0 : v >= 255 ? 255 : static_cast<u8>(static_cast<u32>(v));
        }
}

void idct2(const s16* in, const u16* q, u8* out, usize stride)
{
    const auto f = [&](int i) { return static_cast<float>(s32{in[i]} * s32{q[i]}) * 0.125f; };
    const float c0 = f(0), c1 = f(1), c8 = f(8), c9 = f(9);
    const float a = c1 + c9, b = c1 - c9, e = c0 + c8, g = c0 - c8;
    const auto put = [](float v) { return limit(static_cast<s32>(v + 0.5f)); };
    out[0] = put(e + a);
    out[1] = put(e - a);
    out[stride] = put(g + b);
    out[stride + 1] = put(g - b);
}

void idct1(const s16* in, const u16* q, u8* out, usize)
{
    out[0] = limit(descale(s64{in[0]} * q[0], 3));
}
} // namespace scaled

// ---- jdcolor.c's YCbCr to RGB tables (16-bit fixed point).
struct ColorTables {
    int cr_r[256], cb_b[256];
    s64 cr_g[256], cb_g[256];
    ColorTables()
    {
        constexpr int SCALEBITS = 16;
        constexpr s64 ONE_HALF = s64{1} << (SCALEBITS - 1);
        const auto fix = [](double x) { return static_cast<s64>(x * (1L << SCALEBITS) + 0.5); };
        for (int i = 0; i < 256; i++) {
            const s64 x = i - 128;
            cr_r[i] = static_cast<int>((fix(1.40200) * x + ONE_HALF) >> SCALEBITS);
            cb_b[i] = static_cast<int>((fix(1.77200) * x + ONE_HALF) >> SCALEBITS);
            cr_g[i] = -fix(0.71414) * x;
            cb_g[i] = -fix(0.34414) * x + ONE_HALF;
        }
    }
};
const ColorTables kColor;

inline u8 clamp255(int v) { return static_cast<u8>(std::clamp(v, 0, 255)); }

} // namespace

// ---- markers

bool Decoder::read_header(const u8* data, usize size, std::string& error)
{
    data_ = data;
    size_ = size;
    header_ = {};
    comps_.clear();
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        error = "no SOI";
        return false;
    }
    usize pos = 2;
    bool have_frame = false;
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) {
            pos++; // garbage between markers: skipped, as libjpeg's next_marker does
            continue;
        }
        const int marker = data[pos + 1];
        if (marker == 0xFF) {
            pos++;
            continue;
        }
        pos += 2;
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
        if (marker == 0xD9) {
            error = "EOI before the first scan";
            return false;
        }
        if (pos + 2 > size) break;
        const u32 len = u32{data[pos]} << 8 | data[pos + 1];
        if (len < 2 || pos + len > size) {
            error = std::format("marker {:#04x} runs past the data", marker);
            return false;
        }
        const u8* p = data + pos + 2;
        const u32 n = len - 2;
        if (marker == 0xDA) {
            if (!have_frame) {
                error = "a scan before the frame";
                return false;
            }
            pos_ = pos - 2;
            return true;
        }
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (!parse_sof(p, n, marker, error)) return false;
            have_frame = true;
        } else if (marker == 0xC4) {
            if (!parse_dht(p, n, error)) return false;
        } else if (marker == 0xDB) {
            if (!parse_dqt(p, n, error)) return false;
        } else if (marker == 0xDD) {
            if (n < 2) {
                error = "a short DRI";
                return false;
            }
            restart_interval_ = u32{p[0]} << 8 | p[1];
        } else if (marker == 0xE0 && n >= 5 && std::memcmp(p, "JFIF", 5) == 0) {
            header_.jfif = true;
        } else if (marker == 0xEE && n >= 12 && std::memcmp(p, "Adobe", 5) == 0) {
            header_.adobe = true;
            header_.adobe_transform = p[11];
        }
        pos += len;
    }
    error = "the data ends before the first scan";
    return false;
}

bool Decoder::parse_sof(const u8* p, u32 n, int marker, std::string& error)
{
    if (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) {
        error = std::format("SOF{} (not baseline, extended or progressive Huffman)", marker - 0xC0);
        return false;
    }
    if (n < 6 || p[0] != 8) {
        error = "a frame that is not 8 bits";
        return false;
    }
    header_.progressive = marker == 0xC2;
    header_.height = u32{p[1]} << 8 | p[2];
    header_.width = u32{p[3]} << 8 | p[4];
    header_.components = p[5];
    if (!header_.width || !header_.height || header_.components < 1 || header_.components > 4 ||
        n < 6u + 3u * static_cast<u32>(header_.components)) {
        error = "a bad frame header";
        return false;
    }
    comps_.assign(static_cast<usize>(header_.components), {});
    max_h_ = max_v_ = 1;
    for (int i = 0; i < header_.components; i++) {
        ComponentInfo& c = comps_[static_cast<usize>(i)];
        c.id = p[6 + 3 * i];
        c.h = p[7 + 3 * i] >> 4;
        c.v = p[7 + 3 * i] & 15;
        c.tq = p[8 + 3 * i];
        if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.tq > 3) {
            error = "bad sampling factors or quantisation table";
            return false;
        }
        max_h_ = std::max(max_h_, c.h);
        max_v_ = std::max(max_v_, c.v);
    }
    mcus_x_ = div_round_up(header_.width, 8u * static_cast<u32>(max_h_));
    mcus_y_ = div_round_up(header_.height, 8u * static_cast<u32>(max_v_));
    for (ComponentInfo& c : comps_) {
        c.width_in_blocks = div_round_up(div_round_up(header_.width * static_cast<u32>(c.h), static_cast<u32>(max_h_)), 8);
        c.height_in_blocks = div_round_up(div_round_up(header_.height * static_cast<u32>(c.v), static_cast<u32>(max_v_)), 8);
        c.blocks_per_row = mcus_x_ * static_cast<u32>(c.h);
        c.block_rows = mcus_y_ * static_cast<u32>(c.v);
        c.coefficients.assign(usize{c.blocks_per_row} * c.block_rows * 64, 0);
    }
    // libjpeg 6b's default_decompress_parms: the colour space from the component count and the markers.
    switch (header_.components) {
    case 1: header_.color_space = ColorSpace::Gray; break;
    case 3:
        if (header_.jfif) header_.color_space = ColorSpace::YCbCr;
        else if (header_.adobe) header_.color_space = header_.adobe_transform == 0 ? ColorSpace::RGB : ColorSpace::YCbCr;
        else if (comps_[0].id == 82 && comps_[1].id == 71 && comps_[2].id == 66) header_.color_space = ColorSpace::RGB;
        else header_.color_space = ColorSpace::YCbCr;
        break;
    case 4:
        header_.color_space = header_.adobe && header_.adobe_transform == 2 ? ColorSpace::YCCK : ColorSpace::CMYK;
        break;
    default: header_.color_space = ColorSpace::Unknown; break;
    }
    return true;
}

bool Decoder::parse_dqt(const u8* p, u32 n, std::string& error)
{
    u32 i = 0;
    while (i < n) {
        const int pq = p[i] >> 4, tq = p[i] & 15;
        i++;
        if (tq > 3 || i + (pq ? 128u : 64u) > n) {
            error = "a bad DQT";
            return false;
        }
        for (int k = 0; k < 64; k++) {
            quant_[tq][kNatural[k]] = pq ? static_cast<u16>(p[i] << 8 | p[i + 1]) : p[i];
            i += pq ? 2 : 1;
        }
        quant_present_[tq] = true;
    }
    return true;
}

bool Decoder::parse_dht(const u8* p, u32 n, std::string& error)
{
    u32 i = 0;
    while (i + 17 <= n) {
        const int tc = p[i] >> 4, th = p[i] & 15;
        if (tc > 1 || th > 3) {
            error = "a bad DHT";
            return false;
        }
        u32 count = 0;
        int bits[17] = {};
        for (int l = 1; l <= 16; l++) count += bits[l] = p[i + static_cast<u32>(l)];
        i += 17;
        if (count > 256 || i + count > n) {
            error = "a bad DHT";
            return false;
        }
        Huffman& h = tc ? ac_[th] : dc_[th];
        h = {};
        h.present = true;
        h.values.assign(p + i, p + i + count);
        i += count;
        // T.81 annex C/F: codes per length; maxcode, valoffset; an 8-bit lookahead table.
        int code = 0, k = 0;
        for (int l = 1; l <= 16; l++) {
            h.valoffset[l] = k - code;
            // A length's codes fit in it with the all-ones code left over, else the table is bad (jdhuff.c
            // jpeg_make_d_derived_tbl, when a scan uses it): its codes would index past the lookahead table (D-605).
            if (code + bits[l] >= (1 << l)) {
                h.bad = true;
                break;
            }
            for (int j = 0; j < bits[l]; j++) {
                if (l <= 8)
                    for (int fill = 0; fill < (1 << (8 - l)); fill++) {
                        h.lookup_length[(code << (8 - l)) | fill] = static_cast<u8>(l);
                        h.lookup_value[(code << (8 - l)) | fill] = h.values[static_cast<usize>(k)];
                    }
                code++;
                k++;
            }
            h.maxcode[l] = bits[l] ? code - 1 : -1;
            code <<= 1;
        }
        h.maxcode[17] = 0x7FFFFFFF;
    }
    return true;
}

// ---- entropy decoding: libjpeg 6b's, step for step where damaged data makes a difference (jdhuff.c and jdphuff.c:
// the bit buffer refilled to 57 bits, a marker inside the data padding with zeros once and leaving the rest of the
// restart interval zero; jdmarker.c's next_marker counting skipped bytes; jpeg_resync_to_restart). libjpgdec's source
// is the whole file in memory and running past its end stops the decode (measured, run 107).

namespace {
constexpr int kMinGetBits = 57; // libjpgdec's bit buffer is 64 bits (BIT_BUF_SIZE 64 - 7; measured, run 107)

int extend(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }
} // namespace

struct Decoder::Input {
    const u8* data;
    usize size, pos;
    int& warnings;
    int unread_marker = 0;
    int discarded = 0;
    bool ran_out = false;

    bool byte(int& c)
    {
        if (pos >= size) {
            ran_out = true;
            return false;
        }
        c = data[pos++];
        return true;
    }
    bool next_marker() // jdmarker.c
    {
        int c = 0;
        for (;;) {
            if (!byte(c)) return false;
            while (c != 0xFF) {
                discarded++;
                if (!byte(c)) return false;
            }
            do {
                if (!byte(c)) return false;
            } while (c == 0xFF);
            if (c != 0) break;
            discarded += 2; // a stuffed zero outside the data
        }
        if (discarded) { // JWRN_EXTRANEOUS_DATA
            warnings++;
            discarded = 0;
        }
        unread_marker = c;
        return true;
    }
    // jpeg_resync_to_restart: the marker found is not the restart expected.
    bool resync(int desired)
    {
        int marker = unread_marker;
        warnings++; // JWRN_MUST_RESYNC
        for (;;) {
            int action = 1;
            if (marker < 0xC0) action = 2;
            else if (marker < 0xD0 || marker > 0xD7) action = 3;
            else if (marker == 0xD0 + ((desired + 1) & 7) || marker == 0xD0 + ((desired + 2) & 7)) action = 3;
            else if (marker == 0xD0 + ((desired - 1) & 7) || marker == 0xD0 + ((desired - 2) & 7)) action = 2;
            if (action == 1) {
                unread_marker = 0;
                return true;
            }
            if (action == 3) return true;
            if (!next_marker()) return false;
            marker = unread_marker;
        }
    }
};

struct Decoder::Bits {
    Input& in;
    bool insufficient = false;
    u64 buf = 0;
    int left = 0;

    bool fill(int nbits) // jpeg_fill_bit_buffer
    {
        if (in.unread_marker == 0) {
            while (left < kMinGetBits) {
                int c = 0;
                if (!in.byte(c)) return false;
                if (c == 0xFF) {
                    do {
                        if (!in.byte(c)) return false;
                    } while (c == 0xFF);
                    if (c == 0) {
                        c = 0xFF;
                    } else {
                        in.unread_marker = c;
                        break;
                    }
                }
                buf = (buf << 8) | static_cast<u64>(c);
                left += 8;
            }
            if (in.unread_marker == 0) return true;
        }
        if (nbits > left) {
            if (!insufficient) { // JWRN_HIT_MARKER
                in.warnings++;
                insufficient = true;
            }
            buf <<= kMinGetBits - left;
            left = kMinGetBits;
        }
        return true;
    }
    bool check(int n) { return left >= n || fill(n); } // CHECK_BIT_BUFFER
    int get(int n)
    {
        left -= n;
        return static_cast<int>((buf >> left) & ((u64{1} << n) - 1));
    }
    // HUFF_DECODE and jpeg_huff_decode: the symbol, or -1 when the data ran out.
    int decode(const Huffman& h)
    {
        int nb = 1;
        if (left < 8) {
            if (!fill(0)) return -1;
        }
        if (left >= 8) {
            const int look = static_cast<int>((buf >> (left - 8)) & 0xFF);
            if ((nb = h.lookup_length[look]) != 0) {
                left -= nb;
                return h.lookup_value[look];
            }
            nb = 9;
        }
        if (!check(nb)) return -1;
        int code = get(nb), l = nb;
        while (code > h.maxcode[l]) {
            code <<= 1;
            if (!check(1)) return -1;
            code |= get(1);
            l++;
        }
        if (l > 16) { // JWRN_HUFF_BAD_CODE: a zero
            in.warnings++;
            return 0;
        }
        return h.values[static_cast<usize>(code + h.valoffset[l])];
    }
};

struct Decoder::Scan {
    std::vector<int> comps; // indices into comps_
    int ss = 0, se = 63, ah = 0, al = 0;
};

bool Decoder::decode(std::string& error)
{
    std::fill(std::begin(dc_sum_), std::end(dc_sum_), 0);
    last_mcus_ = 0;
    warnings_ = 0;
    ran_out_ = false;
    rows_done_ = 0;
    multiple_scans_ = header_.progressive;
    scans_ = 0;
    for (ComponentInfo& c : comps_) std::fill(c.coefficients.begin(), c.coefficients.end(), s16{0});
    Input in{data_, size_, pos_, warnings_};
    for (;;) {
        if (in.unread_marker == 0 && !in.next_marker()) break;
        const int marker = in.unread_marker;
        in.unread_marker = 0;
        if (marker == 0xD9) return true; // EOI
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
        int hi = 0, lo = 0;
        if (!in.byte(hi) || !in.byte(lo)) break;
        const u32 len = static_cast<u32>(hi << 8 | lo);
        if (len < 2 || in.pos + len - 2 > size_) {
            in.pos = size_;
            in.ran_out = true;
            break;
        }
        const u8* p = data_ + in.pos;
        const u32 n = len - 2;
        in.pos += n;
        if (marker == 0xDA) {
            if (!decode_scan(p, n, in, error)) return false;
            if (in.ran_out) break;
        } else if (marker == 0xC4) {
            if (!parse_dht(p, n, error)) return false;
        } else if (marker == 0xDB) {
            if (!parse_dqt(p, n, error)) return false;
        } else if (marker == 0xDD && n >= 2) {
            restart_interval_ = u32{p[0]} << 8 | p[1];
        }
    }
    ran_out_ = true; // past the end of the file before EOI
    return true;
}

bool Decoder::decode_scan(const u8* p, u32 n, Input& in, std::string& error)
{
    if (n < 1) {
        error = "a short SOS";
        return false;
    }
    Scan scan;
    const int ns = p[0];
    if (ns < 1 || ns > 4 || n < 1u + 2u * static_cast<u32>(ns) + 3u) {
        error = "a bad SOS";
        return false;
    }
    for (int i = 0; i < ns; i++) {
        const int id = p[1 + 2 * i];
        const auto it = std::find_if(comps_.begin(), comps_.end(), [&](const ComponentInfo& c) { return c.id == id; });
        if (it == comps_.end()) {
            error = "a scan of an unknown component";
            return false;
        }
        it->td = p[2 + 2 * i] >> 4;
        it->ta = p[2 + 2 * i] & 15;
        scan.comps.push_back(static_cast<int>(it - comps_.begin()));
    }
    const u8* q = p + 1 + 2 * ns;
    scan.ss = q[0];
    scan.se = q[1];
    scan.ah = q[2] >> 4;
    scan.al = q[2] & 15;
    if (header_.progressive) {
        // jdphuff.c start_pass_phuff_decoder (JERR_BAD_PROGRESSION): a DC band ends at 0; an AC band lies in 1..63 and
        // has one component; a refinement's Al is Ah - 1; Al is at most 13. The band's end indexes the zigzag order
        // and the block (D-605).
        const bool dc_band = scan.ss == 0;
        if ((dc_band ? scan.se != 0 : scan.ss > scan.se || scan.se > 63 || ns != 1) ||
            (scan.ah != 0 && scan.al != scan.ah - 1) || scan.al > 13) {
            error = std::format("a bad progression (Ss {}, Se {}, Ah {}, Al {})", scan.ss, scan.se, scan.ah, scan.al);
            return false;
        }
    } else {
        scan.ss = 0;
        scan.se = 63;
        scan.ah = scan.al = 0;
    }
    if (scans_++ == 0 && scan.comps.size() < comps_.size()) multiple_scans_ = true; // jdinput.c
    for (const int ci : scan.comps) {
        const ComponentInfo& c = comps_[static_cast<usize>(ci)];
        const bool needs_dc = scan.ss == 0 && scan.ah == 0, needs_ac = scan.se > 0;
        // A table number past the four is a missing table too (jdhuff.c jpeg_make_d_derived_tbl: JERR_NO_HUFF_TABLE);
        // the scan header's four bits can name up to 15 (D-605).
        if ((needs_dc && (c.td > 3 || !dc_[c.td].present)) || (needs_ac && (c.ta > 3 || !ac_[c.ta].present))) {
            error = "a scan without its Huffman table";
            return false;
        }
        if ((needs_dc && dc_[c.td].bad) || (needs_ac && ac_[c.ta].bad)) {
            error = "a scan with a bad Huffman table";
            return false;
        }
    }
    Bits bits{in};
    std::array<int, 4> last_dc{};
    int eobrun = 0;
    u32 restarts_to_go = restart_interval_;
    int next_restart = 0;
    const auto process_restart = [&] {
        in.discarded += bits.left / 8;
        bits.left = 0;
        if (in.unread_marker == 0 && !in.next_marker()) return false;
        if (in.unread_marker == 0xD0 + next_restart) in.unread_marker = 0;
        else if (!in.resync(next_restart)) return false;
        next_restart = (next_restart + 1) & 7;
        last_dc = {};
        eobrun = 0;
        restarts_to_go = restart_interval_;
        if (in.unread_marker == 0) bits.insufficient = false;
        return true;
    };
    const int p1 = 1 << scan.al, m1 = -1 << scan.al;
    // One block of the MCU; false when the data ran out.
    const auto block = [&](ComponentInfo& c, int ci, s16* coef, int blkn) -> bool {
        const usize cu = static_cast<usize>(ci);
        if (!header_.progressive || (scan.ss == 0 && scan.ah == 0)) { // a sequential block, or the first DC scan
            int s = bits.decode(dc_[c.td]);
            if (s < 0) return false;
            s = std::min(s, 16);
            if (s) {
                if (!bits.check(s)) return false;
                s = extend(bits.get(s), s);
            }
            s += last_dc[cu];
            last_dc[cu] = s;
            if (!header_.progressive) {
                coef[0] = static_cast<s16>(s);
                if (blkn < 10) dc_sum_[blkn] += s; // libjpgdec's sums for its mean
                for (int k = 1; k < 64; k++) {
                    s = bits.decode(ac_[c.ta]);
                    if (s < 0) return false;
                    int r = s >> 4;
                    s &= 15;
                    if (s) {
                        k += r;
                        if (!bits.check(s)) return false;
                        r = bits.get(s);
                        coef[kNatural[k]] = static_cast<s16>(extend(r, s));
                    } else {
                        if (r != 15) break;
                        k += 15;
                    }
                }
            } else {
                coef[0] = static_cast<s16>(s * p1);
            }
            return true;
        }
        if (scan.ss == 0) { // DC refinement
            if (!bits.check(1)) return false;
            if (bits.get(1)) coef[0] = static_cast<s16>(coef[0] | p1);
            return true;
        }
        if (scan.ah == 0) { // AC first
            if (eobrun > 0) {
                eobrun--;
                return true;
            }
            for (int k = scan.ss; k <= scan.se; k++) {
                int s = bits.decode(ac_[c.ta]);
                if (s < 0) return false;
                int r = s >> 4;
                s &= 15;
                if (s) {
                    k += r;
                    if (!bits.check(s)) return false;
                    r = bits.get(s);
                    coef[kNatural[k]] = static_cast<s16>(extend(r, s) * p1);
                } else if (r == 15) {
                    k += 15;
                } else {
                    eobrun = 1 << r;
                    if (r) {
                        if (!bits.check(r)) return false;
                        eobrun += bits.get(r);
                    }
                    eobrun--;
                    break;
                }
            }
            return true;
        }
        // AC refinement (jdphuff.c decode_mcu_AC_refine)
        int k = scan.ss;
        if (eobrun == 0) {
            for (; k <= scan.se; k++) {
                int s = bits.decode(ac_[c.ta]);
                if (s < 0) return false;
                int r = s >> 4;
                s &= 15;
                if (s) {
                    if (s != 1) in.warnings++; // JWRN_HUFF_BAD_CODE
                    if (!bits.check(1)) return false;
                    s = bits.get(1) ? p1 : m1;
                } else if (r != 15) {
                    eobrun = 1 << r;
                    if (r) {
                        if (!bits.check(r)) return false;
                        eobrun += bits.get(r);
                    }
                    break;
                }
                do {
                    s16* thiscoef = coef + kNatural[k];
                    if (*thiscoef != 0) {
                        if (!bits.check(1)) return false;
                        if (bits.get(1) && (*thiscoef & p1) == 0)
                            *thiscoef = static_cast<s16>(*thiscoef >= 0 ? *thiscoef + p1 : *thiscoef + m1);
                    } else if (--r < 0) {
                        break;
                    }
                    k++;
                } while (k <= scan.se);
                if (s) coef[kNatural[k]] = static_cast<s16>(s);
            }
        }
        if (eobrun > 0) {
            for (; k <= scan.se; k++) {
                s16* thiscoef = coef + kNatural[k];
                if (*thiscoef != 0) {
                    if (!bits.check(1)) return false;
                    if (bits.get(1) && (*thiscoef & p1) == 0)
                        *thiscoef = static_cast<s16>(*thiscoef >= 0 ? *thiscoef + p1 : *thiscoef + m1);
                }
            }
            eobrun--;
        }
        return true;
    };
    const auto block_at = [&](ComponentInfo& c, u32 bx, u32 by) {
        return c.coefficients.data() + (usize{by} * c.blocks_per_row + bx) * 64;
    };
    // One MCU: the restart first, then its blocks unless the data is known to be short (they stay zero).
    const auto mcu = [&](const auto& blocks) -> bool {
        if (restart_interval_) {
            if (restarts_to_go == 0 && !process_restart()) return false;
        }
        // The DC refinement reads its bits even then (jdphuff.c: "not worth the cycles").
        if (!bits.insufficient || (header_.progressive && scan.ss == 0 && scan.ah != 0))
            if (!blocks()) return false;
        restarts_to_go--;
        return true;
    };
    const bool single = !multiple_scans_;
    if (scan.comps.size() == 1) {
        const int ci = scan.comps[0];
        ComponentInfo& c = comps_[static_cast<usize>(ci)];
        last_mcus_ = c.width_in_blocks * c.height_in_blocks;
        for (u32 by = 0; by < c.height_in_blocks; by++) {
            for (u32 bx = 0; bx < c.width_in_blocks; bx++)
                if (!mcu([&] { return block(c, ci, block_at(c, bx, by), 0); })) return true;
            if (single && (by + 1) % static_cast<u32>(c.v) == 0) rows_done_ = (by + 1) / static_cast<u32>(c.v);
        }
    } else {
        last_mcus_ = mcus_x_ * mcus_y_;
        for (u32 my = 0; my < mcus_y_; my++) {
            for (u32 mx = 0; mx < mcus_x_; mx++) {
                const auto blocks = [&] {
                    int blkn = 0;
                    for (const int ci : scan.comps) {
                        ComponentInfo& c = comps_[static_cast<usize>(ci)];
                        for (int v = 0; v < c.v; v++)
                            for (int h = 0; h < c.h; h++, blkn++)
                                if (!block(c, ci, block_at(c, mx * static_cast<u32>(c.h) + static_cast<u32>(h),
                                                    my * static_cast<u32>(c.v) + static_cast<u32>(v)), blkn))
                                    return false;
                    }
                    return true;
                };
                if (!mcu(blocks)) return true;
            }
            if (single) rows_done_ = my + 1;
        }
    }
    return true;
}

// Lines libjpgdec has written when the data ran out: those of the iMCU rows decoded before (single-scan files only),
// less the last row group while h2v2 fancy upsampling waits for the next row (jdmainct.c context rows).
u32 Decoder::lines_written(int scale, bool fast, ColorSpace out_space, u32 height) const
{
    if (!ran_out_) return height;
    if (multiple_scans_) return 0;
    const int dct = 8 / scale;
    const u32 imcu = static_cast<u32>(max_v_ * dct);
    bool context = false;
    if (!fast && dct > 1 && out_space != ColorSpace::Gray)
        for (const ComponentInfo& c : comps_) {
            int ssize = dct;
            while (ssize < 8 && c.h * ssize * 2 <= max_h_ * dct && c.v * ssize * 2 <= max_v_ * dct) ssize *= 2;
            const u32 cw = div_round_up(header_.width * static_cast<u32>(c.h) * static_cast<u32>(ssize), static_cast<u32>(max_h_) * 8);
            if (max_h_ * dct == 2 * c.h * ssize && max_v_ * dct == 2 * c.v * ssize && cw > 2) context = true;
        }
    u32 lines = rows_done_ * imcu;
    if (context && rows_done_ > 0 && rows_done_ < (header_.height + imcu * static_cast<u32>(scale) - 1) / (imcu * static_cast<u32>(scale)))
        lines -= static_cast<u32>(max_v_);
    return std::min(lines, height);
}

// ---- output (libjpeg 6b's jdmaster, jdcoefct, jddctmgr, jdsample, jdcolor)

// libjpgdec's mean (Sony's addition to libjpeg, measured, run 107): its Huffman decoder adds every block's DC value to
// a sum for the block's place in the MCU; the mean of component 0 is then, for each of the component's h x v places,
// q0 * sum / 8 + 128 * MCUs added up in single precision, divided by MCUs * h * v. Progressive images: libjpgdec raises
// an error and answers -1.
float Decoder::mean() const
{
    if (comps_.empty() || header_.progressive) return -1.0f;
    const ComponentInfo& c = comps_[0];
    const int places = c.h * c.v;
    const s32 mcus = static_cast<s32>(last_mcus_);
    const double base = static_cast<double>(static_cast<float>(static_cast<s32>(static_cast<u32>(mcus) << 7)));
    float sum = 0.0f;
    for (int j = 0; j < places && j < 10; j++) {
        const float term = static_cast<float>(s64{quant_[c.tq][0]} * dc_sum_[j]);
        sum = static_cast<float>(static_cast<double>(sum) + (static_cast<double>(term) * 0.125 + base));
    }
    return sum / static_cast<float>(static_cast<s32>(static_cast<u32>(mcus) * static_cast<u32>(places)));
}

// libjpgdec's useMemorySpace (Sony's own function over libjpeg's state after jpeg_calc_output_dimensions, measured,
// run 107): a fixed part by the scaled DCT size plus the row buffers of the upsampling and colour conversion it will
// use, chosen from the colour space and the sampling factors.
u32 Decoder::memory_estimate(int scale, bool fast, u32 ow, u32 oc) const
{
    const u32 nc = static_cast<u32>(comps_.size());
    const u32 min_dct = static_cast<u32>(8 / scale);
    u32 blocks = 1;
    if (nc != 1) {
        blocks = 0;
        if (nc >= 2 && nc <= 4)
            for (const ComponentInfo& c : comps_) blocks += static_cast<u32>(c.h * c.v);
        if (blocks < 1 || blocks > 10) blocks = 10;
    }
    int kind = 0;
    if (header_.color_space != ColorSpace::Gray) {
        kind = 6;
        if (nc == 3) {
            const int key = (max_h_ << 4) | max_v_;
            kind = 5;
            if (key == 0x11) kind = 1;
            else if (key == 0x14 || key == 0x24 || key == 0x44) kind = 4;
            else if (key == 0x21 && (blocks == 4 || blocks == 1) && comps_[0].h == 2) kind = 2;
            else if (key == 0x22 && (blocks == 6 || blocks == 1) && comps_[0].h == 2 && comps_[0].v == 2) kind = 3;
        }
    }
    const bool fancy = !fast;
    const u32 iw = header_.width, ih = header_.height;
    u32 rows = 0;
    if (!header_.progressive) {
        switch (kind) {
        case 0: rows = ow * 8 * static_cast<u32>(max_v_); break;
        case 1: rows = ow * 24 + ow * oc; break;
        case 2: rows = ow * 16 + ow * oc + (fancy ? 2048 + ow * 2 : 0); break;
        case 3: rows = ow * 24 + 3 * ow * oc + (fancy ? 2048 + ((ow * 2) & ~3u) : 0); break;
        case 4: rows = nc * 32 * ow + 6 * ow * oc; break;
        default: rows = nc * 16 * ow + 4 * ow * oc; break;
        }
    } else { // the whole image's coefficients are kept (libjpgdec's other branch, taken for every progressive file seen)
        switch (kind) {
        case 0: rows = iw * ih + 3 * iw * oc; break;
        case 1: rows = 3 * iw * ih + 3 * ow * oc; break;
        case 2: rows = 2 * iw * ih + iw * oc + (fancy ? 2 * ow : 0); break;
        case 3: rows = (ih >> 1) * 3 * iw + 3 * ow * oc + (fancy ? ((ow * 2) & ~3u) : 0); break;
        case 4: rows = 4 * iw * ih + 3 * ow * oc; break;
        default: rows = (ih >> 1) * iw * nc * 2 + 3 * ow * oc; break;
        }
        rows = rows * 2 + 33480;
    }
    return kind == 0 ? (min_dct << 7) + 2500 + rows : (min_dct << 9) + 15200 + rows;
}

bool Decoder::output(int scale, bool fast, ColorSpace out_space, Output& out, std::string& error) const
{
    if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
        error = std::format("scale 1/{}", scale);
        return false;
    }
    const int dct = 8 / scale; // DCT_scaled_size
    out.width = div_round_up(header_.width, static_cast<u32>(scale));
    out.height = div_round_up(header_.height, static_cast<u32>(scale));
    const bool gray_in = header_.components == 1;
    const bool convert = out_space == ColorSpace::RGB && header_.color_space == ColorSpace::YCbCr;
    const bool rgb_in = out_space == ColorSpace::RGB && header_.color_space == ColorSpace::RGB;
    if (out_space == ColorSpace::Gray) {
        if (header_.color_space != ColorSpace::Gray && header_.color_space != ColorSpace::YCbCr) {
            error = "gray from this colour space";
            return false;
        }
    } else if (out_space == ColorSpace::YCbCr) {
        if (gray_in) {
            error = "YCbCr from gray";
            return false;
        }
    } else if (!(convert || rgb_in)) {
        error = "RGB from this colour space";
        return false;
    }
    const usize ncomp = out_space == ColorSpace::Gray ? 1 : comps_.size();
    if (out_space != ColorSpace::Gray && ncomp != 3) {
        error = "not three components";
        return false;
    }
    out.components = out_space == ColorSpace::Gray ? 1 : 3;
    // Each component's samples (IDCT of the real blocks only), and its size as libjpeg computes it. jdmaster.c's
    // jpeg_calc_output_dimensions scales a subsampled component up through a larger IDCT before upsampling.
    std::vector<std::vector<u8>> planes(ncomp);
    std::vector<u32> dw(ncomp), dh(ncomp), pw(ncomp);
    std::vector<int> cdct(ncomp);
    for (usize ci = 0; ci < ncomp; ci++) {
        const ComponentInfo& c = comps_[ci];
        if (!quant_present_[c.tq]) {
            error = "a component without its quantisation table";
            return false;
        }
        int ssize = dct;
        while (ssize < 8 && c.h * ssize * 2 <= max_h_ * dct && c.v * ssize * 2 <= max_v_ * dct) ssize *= 2;
        if (out_space == ColorSpace::Gray) ssize = dct;
        cdct[ci] = ssize;
        dw[ci] = div_round_up(header_.width * static_cast<u32>(c.h) * static_cast<u32>(ssize), static_cast<u32>(max_h_) * 8);
        dh[ci] = div_round_up(header_.height * static_cast<u32>(c.v) * static_cast<u32>(ssize), static_cast<u32>(max_v_) * 8);
        pw[ci] = c.width_in_blocks * static_cast<u32>(ssize);
        planes[ci].assign(usize{pw[ci]} * c.height_in_blocks * static_cast<u32>(ssize), 0);
        // jddctmgr.c's choice: by the scaled DCT size, then for 8 by the method (libjpgdec's dct_method 2 or 3).
        const int dct_c = ssize;
        std::array<double, 64> flt_q{};
        std::array<float, 16> q4{};
        if (dct_c == 8 && !fast) flt::table(quant_[c.tq], flt_q.data());
        if (dct_c == 4) scaled::table4(quant_[c.tq], q4.data());
        for (u32 by = 0; by < c.height_in_blocks; by++)
            for (u32 bx = 0; bx < c.width_in_blocks; bx++) {
                const s16* coef = c.coefficients.data() + (usize{by} * c.blocks_per_row + bx) * 64;
                u8* o = planes[ci].data() + usize{by} * static_cast<u32>(dct_c) * pw[ci] + usize{bx} * static_cast<u32>(dct_c);
                if (dct_c == 8 && fast) altivec::idct(coef, quant_[c.tq], o, pw[ci]);
                else if (dct_c == 8) flt::idct(coef, flt_q.data(), o, pw[ci]);
                else if (dct_c == 4) scaled::idct4(coef, q4.data(), o, pw[ci]);
                else if (dct_c == 2) scaled::idct2(coef, quant_[c.tq], o, pw[ci]);
                else scaled::idct1(coef, quant_[c.tq], o, pw[ci]);
            }
    }
    // Upsampled components at the output size (jdsample.c). Fancy for the quality method when the component's scaled
    // block is larger than one sample and it is more than two samples wide; replication otherwise.
    std::vector<std::vector<u8>> full(ncomp, std::vector<u8>(usize{out.width + 2} * (out.height + 2)));
    for (usize ci = 0; ci < ncomp; ci++) {
        const ComponentInfo& c = comps_[ci];
        const int hgroup = c.h * cdct[ci] / dct, vgroup = c.v * cdct[ci] / dct; // samples per output group
        const int hf = ci == 0 && out_space == ColorSpace::Gray ? 1 : max_h_ / hgroup;
        const int vf = ci == 0 && out_space == ColorSpace::Gray ? 1 : max_v_ / vgroup;
        if ((max_h_ % hgroup) || (max_v_ % vgroup)) {
            error = "fractional sampling factors";
            return false;
        }
        const auto in = [&](u32 x, u32 y) {
            return planes[ci][usize{std::min(y, dh[ci] - 1)} * pw[ci] + std::min(x, pw[ci] - 1)];
        };
        std::vector<u8>& f = full[ci];
        const usize fw = out.width + 2;
        const bool fancy = !fast && cdct[ci] > 1 && dw[ci] > 2;
        if (hf == 1 && vf == 1) {
            for (u32 y = 0; y < out.height; y++)
                for (u32 x = 0; x < out.width; x++) f[y * fw + x] = in(x, y);
        } else if (hf == 2 && vf == 1 && fancy) {
            for (u32 y = 0; y < out.height; y++) {
                const u8* row = planes[ci].data() + usize{y} * pw[ci];
                u8* o = f.data() + y * fw;
                const u32 w = dw[ci];
                int v = row[0];
                *o++ = static_cast<u8>(v);
                *o++ = static_cast<u8>((v * 3 + row[1] + 2) >> 2);
                for (u32 x = 1; x + 1 < w; x++) {
                    v = row[x] * 3;
                    *o++ = static_cast<u8>((v + row[x - 1] + 1) >> 2);
                    *o++ = static_cast<u8>((v + row[x + 1] + 2) >> 2);
                }
                v = row[w - 1];
                *o++ = static_cast<u8>((v * 3 + row[w - 2] + 1) >> 2);
                *o++ = static_cast<u8>(v);
            }
        } else if (hf == 2 && vf == 2 && fancy) {
            const u32 w = dw[ci];
            for (u32 y = 0; y < dh[ci]; y++)
                for (int half = 0; half < 2; half++) {
                    const u32 oy = 2 * y + static_cast<u32>(half);
                    if (oy >= out.height) break;
                    const u8* r0 = planes[ci].data() + usize{y} * pw[ci];
                    const u32 ny = half ? std::min(y + 1, dh[ci] - 1) : (y ? y - 1 : 0);
                    const u8* r1 = planes[ci].data() + usize{ny} * pw[ci];
                    u8* o = f.data() + oy * fw;
                    int thiscol = r0[0] * 3 + r1[0], nextcol = r0[1] * 3 + r1[1];
                    *o++ = static_cast<u8>((thiscol * 4 + 8) >> 4);
                    *o++ = static_cast<u8>((thiscol * 3 + nextcol + 7) >> 4);
                    int lastcol = thiscol;
                    thiscol = nextcol;
                    for (u32 x = 2; x < w; x++) {
                        nextcol = r0[x] * 3 + r1[x];
                        *o++ = static_cast<u8>((thiscol * 3 + lastcol + 8) >> 4);
                        *o++ = static_cast<u8>((thiscol * 3 + nextcol + 7) >> 4);
                        lastcol = thiscol;
                        thiscol = nextcol;
                    }
                    *o++ = static_cast<u8>((thiscol * 3 + lastcol + 8) >> 4);
                    *o++ = static_cast<u8>((thiscol * 4 + 7) >> 4);
                }
        } else {
            for (u32 y = 0; y < out.height; y++)
                for (u32 x = 0; x < out.width; x++)
                    f[y * fw + x] = in(x / static_cast<u32>(hf), y / static_cast<u32>(vf));
        }
    }
    out.pixels.assign(usize{out.width} * out.height * out.components, 0);
    const usize fw = out.width + 2;
    // libjpeg's merged upsampler (jdmaster.c use_merged_upsample: box upsampling of 2x1 or 2x2 YCbCr to RGB, every
    // component at the same DCT size) runs libjpgdec's AltiVec loop over groups of 32 pixels, the plain colour
    // converter over groups of 16 (measured, run 107); the rest of each row takes 6b's tables.
    const bool merged = fast && convert && ncomp == 3 && comps_[0].h == 2 && comps_[1].h == 1 && comps_[2].h == 1 &&
        comps_[0].v <= 2 && comps_[1].v == 1 && comps_[2].v == 1 && cdct[0] == cdct[1] && cdct[1] == cdct[2];
    const u32 vector_width = out.width & (merged ? ~31u : ~15u);
    for (u32 y = 0; y < out.height; y++)
        for (u32 x = 0; x < out.width; x++) {
            u8* o = out.pixels.data() + (usize{y} * out.width + x) * out.components;
            const usize i = y * fw + x;
            if (out.components == 1) {
                o[0] = full[0][i];
            } else if (convert && x < vector_width) {
                // libjpgdec converts each row's first multiple of 16 pixels with AltiVec (measured, run 107): the
                // sample times 32 plus 16, plus vmhraddshs of the chroma times 64 with 14-bit constants, then >> 5.
                const auto mhr = [](int c, int d) { return (c * (d << 6) + 0x4000) >> 15; };
                const auto pack = [](int v) { return static_cast<u8>(std::clamp(v >> 5, 0, 255)); };
                const int base = (full[0][i] << 5) + 16, cb = full[1][i] - 128, cr = full[2][i] - 128;
                o[0] = pack(base + mhr(22970, cr));
                o[1] = pack(mhr(-11700, cr) + base + mhr(-5638, cb));
                o[2] = pack(base + mhr(29032, cb));
            } else if (convert) {
                const int yy = full[0][i], cb = full[1][i], cr = full[2][i];
                o[0] = clamp255(yy + kColor.cr_r[cr]);
                o[1] = clamp255(yy + static_cast<int>((kColor.cb_g[cb] + kColor.cr_g[cr]) >> 16));
                o[2] = clamp255(yy + kColor.cb_b[cb]);
            } else {
                o[0] = full[0][i];
                o[1] = full[1][i];
                o[2] = full[2][i];
            }
        }
    return true;
}

} // namespace pspocket::hle::jpeg
