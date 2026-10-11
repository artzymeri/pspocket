// pspocket's baseline JPEG encoder (D-615); see jpeg_encoder.hpp. Written from ITU-T T.81 and T.871.

#include "jpeg_encoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace pspocket::hle::jpeg {
namespace {

// T.81 figure A.6: the zigzag order, as each zigzag position's natural index (row x 8 + column). The diagonals of
// constant row + column, the odd ones walked down (row rising), the even ones up.
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

// T.81 annex K.1, tables K.1 (luminance) and K.2 (chrominance), in natural order.
constexpr std::array<u8, 64> kLumaQ = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24,
    40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<u8, 64> kChromaQ = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56,
    99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// T.81 annex K.3, tables K.3 to K.6: the typical Huffman tables as BITS (the number of codes of each length, 1 to 16)
// and HUFFVAL (the symbols in code order).
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

// Annex C: each symbol's code and length (C.1's sizes, C.2's codes, C.3's ordering by HUFFVAL).
CodeTable codes_of(const HuffSpec& spec)
{
    CodeTable t{};
    u32 code = 0;
    usize k = 0;
    for (int length = 1; length <= 16; length++) {
        for (u32 i = 0; i < spec.bits[static_cast<usize>(length - 1)]; i++)
            t[spec.values[k++]] = {static_cast<u16>(code++), static_cast<u8>(length)};
        code <<= 1;
    }
    return t;
}

struct Tables {
    CodeTable dc[2], ac[2];
    Tables() : dc{codes_of(kDcLuma), codes_of(kDcChroma)}, ac{codes_of(kAcLuma), codes_of(kAcChroma)} {}
};

// The entropy-coded data, most significant bit first, a 0 stuffed after each 0xFF byte (F.1.2.3).
class BitWriter {
public:
    explicit BitWriter(std::vector<u8>& out) : out_(out) {}
    void put(u32 value, int length)
    {
        if (!length) return;
        acc_ = (acc_ << length) | (value & ((1u << length) - 1));
        n_ += length;
        while (n_ >= 8) {
            n_ -= 8;
            const u8 b = static_cast<u8>(acc_ >> n_);
            out_.push_back(b);
            if (b == 0xFF) out_.push_back(0);
        }
        acc_ &= (u64{1} << n_) - 1;
    }
    void put(const Code& c) { put(c.code, c.length); }
    // The last byte completed with 1 bits, before a marker (F.1.2.3).
    void align()
    {
        if (n_) put((1u << (8 - n_)) - 1, 8 - n_);
    }

private:
    std::vector<u8>& out_;
    u64 acc_ = 0;
    int n_ = 0;
};

// T.81 A.3.3's forward DCT in one dimension, X(u) = C(u)/2 sum over x of s(x) cos((2x+1)u pi/16) with C(0) = 1/sqrt(2):
// the even outputs from the sums of mirrored samples (a 4-point DCT of them), the odd ones from their differences.
// Applied to the rows and then the columns, it gives A.3.3's two-dimensional 1/4 C(u) C(v) sum.
template <class T>
struct DctConstants {
    T half_c4, c2, c6;
    T odd[4][4]; // X(1), X(3), X(5), X(7) from the four differences
    DctConstants()
    {
        constexpr double pi = std::numbers::pi;
        half_c4 = static_cast<T>(std::cos(pi / 4) / 2);
        c2 = static_cast<T>(std::cos(pi / 8) / 2);
        c6 = static_cast<T>(std::cos(3 * pi / 8) / 2);
        for (int u = 0; u < 4; u++)
            for (int k = 0; k < 4; k++) odd[u][k] = static_cast<T>(std::cos((2 * k + 1) * (2 * u + 1) * pi / 16) / 2);
    }
};

template <class T>
void dct8(const T* s, usize step, T* x, usize xstep)
{
    static const DctConstants<T> c;
    T a[4], d[4];
    for (usize i = 0; i < 4; i++) {
        a[i] = s[i * step] + s[(7 - i) * step];
        d[i] = s[i * step] - s[(7 - i) * step];
    }
    const T p0 = a[0] + a[3], p1 = a[1] + a[2], q0 = a[0] - a[3], q1 = a[1] - a[2];
    x[0] = (p0 + p1) * c.half_c4;
    x[4 * xstep] = (p0 - p1) * c.half_c4;
    x[2 * xstep] = q0 * c.c2 + q1 * c.c6;
    x[6 * xstep] = q0 * c.c6 - q1 * c.c2;
    for (usize u = 0; u < 4; u++)
        x[(2 * u + 1) * xstep] = d[0] * c.odd[u][0] + d[1] * c.odd[u][1] + d[2] * c.odd[u][2] + d[3] * c.odd[u][3];
}

// One block's samples (level shifted) to its quantised coefficients in zigzag order (A.3.4's rounding to the nearest
// integer), kept within baseline's ranges: AC within +-1023 (size 10), DC within -1024..1023 so that a difference
// takes at most 11 bits.
template <class T>
void transform(const int* samples, const std::array<u8, 64>& q, int* zz)
{
    T in[64], rows[64], out[64];
    for (usize i = 0; i < 64; i++) in[i] = static_cast<T>(samples[i] - 128);
    for (usize y = 0; y < 8; y++) dct8(in + 8 * y, 1, rows + 8 * y, 1);
    for (usize u = 0; u < 8; u++) dct8(rows + u, 8, out + u, 8);
    for (usize k = 0; k < 64; k++) {
        const usize i = kZigzag[k];
        const long v = std::lround(out[i] / static_cast<T>(q[i]));
        zz[k] = static_cast<int>(k ? std::clamp(v, -1023L, 1023L) : std::clamp(v, -1024L, 1023L));
    }
}

int magnitude_bits(int v) { return std::bit_width(static_cast<u32>(std::abs(v))); }

// F.1.2.1 and F.1.2.2: the DC difference's size and bits, then each AC coefficient's run of zeros and size (ZRL for
// sixteen zeros, EOB when the rest are zero) and bits; a negative value as its low bits less one.
void encode_block(BitWriter& bits, const int* zz, int& pred, const CodeTable& dc, const CodeTable& ac)
{
    const int diff = zz[0] - pred;
    pred = zz[0];
    const int size = magnitude_bits(diff);
    bits.put(dc[static_cast<usize>(size)]);
    bits.put(static_cast<u32>(diff < 0 ? diff - 1 : diff), size);
    int run = 0;
    for (usize k = 1; k < 64; k++) {
        const int v = zz[k];
        if (!v) {
            run++;
            continue;
        }
        for (; run > 15; run -= 16) bits.put(ac[0xF0]);
        const int n = magnitude_bits(v);
        bits.put(ac[static_cast<usize>(run << 4 | n)]);
        bits.put(static_cast<u32>(v < 0 ? v - 1 : v), n);
        run = 0;
    }
    if (run) bits.put(ac[0x00]);
}

void put16(std::vector<u8>& out, u32 v)
{
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

void marker(std::vector<u8>& out, u8 m, u32 length)
{
    out.push_back(0xFF);
    out.push_back(m);
    put16(out, length);
}

struct Component {
    const u8* plane;
    int h, v;  // sampling factors
    int table; // 0: the luma's quantisation and Huffman tables, 1: the chroma's
    int pred = 0;
};

template <class T>
std::vector<u8> encode_with(const Planes& p, const EncodeSettings& s)
{
    static const Tables tables;
    const int quality = std::clamp(s.quality, 1, 100);
    const std::array<u8, 64> q[2] = {quant_table(true, quality), quant_table(false, quality)};
    int hmax = 1, vmax = 1;
    if (p.components == 3) {
        switch (s.sampling) {
        case Sampling::YCbCr444: break;
        case Sampling::YCbCr422: hmax = 2; break;
        case Sampling::YCbCr420: hmax = vmax = 2; break;
        case Sampling::YCbCr411: hmax = 4; break;
        }
    }
    std::vector<Component> comps = {{p.y.data(), hmax, vmax, 0, 0}};
    if (p.components == 3) {
        comps.push_back({p.cb.data(), 1, 1, 1, 0});
        comps.push_back({p.cr.data(), 1, 1, 1, 0});
    }
    const int ntables = p.components == 3 ? 2 : 1;

    std::vector<u8> out;
    out.reserve(1024 + std::min<usize>(usize{p.width} * p.height / 2, usize{1} << 24));
    out.insert(out.end(), {0xFF, 0xD8});
    // T.871 section 10.1: JFIF APP0, version 1.01, no units (a 1:1 aspect ratio), no thumbnail.
    marker(out, 0xE0, 16);
    out.insert(out.end(), {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    // B.2.4.1: the quantisation tables, 8-bit, in zigzag order.
    marker(out, 0xDB, 2 + 65 * static_cast<u32>(ntables));
    for (int t = 0; t < ntables; t++) {
        out.push_back(static_cast<u8>(t));
        for (usize k = 0; k < 64; k++) out.push_back(q[t][kZigzag[k]]);
    }
    // B.2.2: the baseline frame.
    marker(out, 0xC0, 8 + 3 * static_cast<u32>(comps.size()));
    out.push_back(8);
    put16(out, p.height);
    put16(out, p.width);
    out.push_back(static_cast<u8>(comps.size()));
    for (usize i = 0; i < comps.size(); i++) {
        out.push_back(static_cast<u8>(i + 1));
        out.push_back(static_cast<u8>(comps[i].h << 4 | comps[i].v));
        out.push_back(static_cast<u8>(comps[i].table));
    }
    // B.2.4.2: the Huffman tables, DC then AC for each table class.
    u32 dht_length = 2;
    const HuffSpec* specs[2][2] = {{&kDcLuma, &kAcLuma}, {&kDcChroma, &kAcChroma}};
    for (int t = 0; t < ntables; t++)
        for (const HuffSpec* spec : specs[t]) dht_length += 17 + static_cast<u32>(spec->values.size());
    marker(out, 0xC4, dht_length);
    for (int t = 0; t < ntables; t++)
        for (int cls = 0; cls < 2; cls++) {
            const HuffSpec& spec = *specs[t][cls];
            out.push_back(static_cast<u8>(cls << 4 | t));
            out.insert(out.end(), spec.bits.begin(), spec.bits.end());
            out.insert(out.end(), spec.values.begin(), spec.values.end());
        }
    // B.2.4.4: the restart interval.
    if (s.restart_interval) {
        marker(out, 0xDD, 4);
        put16(out, s.restart_interval);
    }
    // B.2.3: one scan of every component, the whole spectrum, no successive approximation.
    marker(out, 0xDA, 6 + 2 * static_cast<u32>(comps.size()));
    out.push_back(static_cast<u8>(comps.size()));
    for (usize i = 0; i < comps.size(); i++) {
        out.push_back(static_cast<u8>(i + 1));
        out.push_back(static_cast<u8>(comps[i].table << 4 | comps[i].table));
    }
    out.insert(out.end(), {0, 63, 0});

    // A.2.3: the MCUs left to right, top to bottom, each component's blocks in it row by row. A.2.4: the picture is
    // padded to whole MCUs by repeating its last column and row (as the samples are read here); a chroma sample is the
    // mean of the fx x fy samples it covers.
    const u32 mcus_x = (p.width + 8 * static_cast<u32>(hmax) - 1) / (8 * static_cast<u32>(hmax));
    const u32 mcus_y = (p.height + 8 * static_cast<u32>(vmax) - 1) / (8 * static_cast<u32>(vmax));
    BitWriter bits(out);
    u32 mcu = 0;
    int next_restart = 0;
    int samples[64], zz[64];
    for (u32 my = 0; my < mcus_y; my++)
        for (u32 mx = 0; mx < mcus_x; mx++, mcu++) {
            if (s.restart_interval && mcu && mcu % s.restart_interval == 0) {
                bits.align();
                out.push_back(0xFF);
                out.push_back(static_cast<u8>(0xD0 + next_restart));
                next_restart = (next_restart + 1) & 7;
                for (Component& c : comps) c.pred = 0;
            }
            for (Component& c : comps) {
                const u32 fx = static_cast<u32>(hmax / c.h), fy = static_cast<u32>(vmax / c.v);
                for (int by = 0; by < c.v; by++)
                    for (int bx = 0; bx < c.h; bx++) {
                        const u32 x0 = ((mx * static_cast<u32>(c.h) + static_cast<u32>(bx)) * 8) * fx;
                        const u32 y0 = ((my * static_cast<u32>(c.v) + static_cast<u32>(by)) * 8) * fy;
                        for (u32 y = 0; y < 8; y++)
                            for (u32 x = 0; x < 8; x++) {
                                u32 sum = 0;
                                for (u32 j = 0; j < fy; j++) {
                                    const u8* row = c.plane + usize{std::min(y0 + y * fy + j, p.height - 1)} * p.width;
                                    for (u32 i = 0; i < fx; i++) sum += row[std::min(x0 + x * fx + i, p.width - 1)];
                                }
                                const u32 n = fx * fy;
                                samples[y * 8 + x] = static_cast<int>((sum + n / 2) / n);
                            }
                        transform<T>(samples, q[c.table], zz);
                        encode_block(bits, zz, c.pred, tables.dc[c.table], tables.ac[c.table]);
                    }
            }
        }
    bits.align();
    out.insert(out.end(), {0xFF, 0xD9});
    return out;
}

} // namespace

std::array<u8, 64> quant_table(bool luma, int quality)
{
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    std::array<u8, 64> t{};
    const std::array<u8, 64>& base = luma ? kLumaQ : kChromaQ;
    for (usize i = 0; i < 64; i++) t[i] = static_cast<u8>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
    return t;
}

void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr)
{
    // T.871's coefficients in 16-bit fixed point (each sum's coefficients add up to 65536, or to 0 for Cb and Cr).
    constexpr int kHalf = 1 << 15, kOffset = 128 << 16;
    y = rgb_to_y(r, g, b);
    cb = static_cast<u8>(std::clamp((-11059 * r - 21709 * g + 32768 * b + kOffset + kHalf) >> 16, 0, 255));
    cr = static_cast<u8>(std::clamp((32768 * r - 27439 * g - 5329 * b + kOffset + kHalf) >> 16, 0, 255));
}

u8 rgb_to_y(u8 r, u8 g, u8 b) { return static_cast<u8>(std::min((19595 * r + 38470 * g + 7471 * b + (1 << 15)) >> 16, 255)); }

std::vector<u8> encode(const Planes& p, const EncodeSettings& s)
{
    const usize n = usize{p.width} * p.height;
    if (!p.width || !p.height || p.width > 0xFFFF || p.height > 0xFFFF || s.restart_interval > 0xFFFF ||
        (p.components != 1 && p.components != 3) || p.y.size() < n ||
        (p.components == 3 && (p.cb.size() < n || p.cr.size() < n)))
        return {};
    return s.fast ? encode_with<float>(p, s) : encode_with<double>(p, s);
}

} // namespace pspocket::hle::jpeg
