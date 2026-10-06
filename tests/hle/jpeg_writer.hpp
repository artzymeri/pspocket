#pragma once

// JPEG files built in code for the image decoder tests (no game data): gray or YCbCr, chroma sampled 4:4:4, 4:2:2 or
// 4:2:0, the standard quantisation tables scaled by a quality, the standard Huffman tables (ITU T.81 annex K), optional
// restart markers, a JFIF APP0 header (or none, or an Adobe APP14); baseline, or progressive (a DC scan with successive
// approximation, AC scans in spectral bands, the DC refinement). The encoder need not match any other: the decoders
// under test read the same bytes.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pspocket::test::jpeg {

struct Image {
    std::uint32_t width = 0, height = 0;
    bool gray = false;
    std::uint32_t h_sampling = 2, v_sampling = 2; // the luma's sampling factors (chroma 1x1)
    int quality = 75;
    std::uint32_t restart_interval = 0; // in MCUs; 0: none
    int app = 0;                        // 0: JFIF APP0, 1: none, 2: Adobe APP14 (transform 1)
    bool progressive = false;           // SOF2 (no restart markers)
    std::vector<std::uint8_t> pixels;   // gray, or R G B, row by row
};

constexpr std::array<std::uint8_t, 64> kZigzag = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26,
    33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr std::array<std::uint8_t, 64> kLumaQ = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13,
    16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104,
    113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<std::uint8_t, 64> kChromaQ = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24,
    26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Annex K.3: bits (counts per length 1..16) and values.
struct HuffSpec {
    std::array<std::uint8_t, 16> bits;
    std::vector<std::uint8_t> values;
};
inline const HuffSpec kDcLuma = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
inline const HuffSpec kDcChroma = {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
inline const HuffSpec kAcLuma = {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
        0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
        0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
        0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
        0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
        0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
        0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
        0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
        0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
inline const HuffSpec kAcChroma = {{0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
        0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
        0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
        0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
        0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
        0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
        0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};

struct Code {
    std::uint16_t code = 0;
    std::uint8_t length = 0;
};

inline std::array<Code, 256> codes_of(const HuffSpec& spec)
{
    std::array<Code, 256> table{};
    std::uint16_t code = 0;
    std::size_t k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < spec.bits[static_cast<std::size_t>(len - 1)]; i++) {
            table[spec.values[k++]] = {code, static_cast<std::uint8_t>(len)};
            code++;
        }
        code <<= 1;
    }
    return table;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t bits, int n)
    {
        for (int i = n - 1; i >= 0; i--) {
            acc_ = static_cast<std::uint8_t>(acc_ << 1 | ((bits >> i) & 1));
            if (++count_ == 8) flush_byte();
        }
    }
    void align() // pad with ones, as T.81 asks
    {
        while (count_) put(1, 1);
    }

private:
    void flush_byte()
    {
        out_.push_back(acc_);
        if (acc_ == 0xFF) out_.push_back(0); // byte stuffing
        acc_ = 0;
        count_ = 0;
    }
    std::vector<std::uint8_t>& out_;
    std::uint8_t acc_ = 0;
    int count_ = 0;
};

inline void put16(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

inline std::array<std::uint8_t, 64> scaled(const std::array<std::uint8_t, 64>& base, int quality)
{
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    std::array<std::uint8_t, 64> q{};
    for (std::size_t i = 0; i < 64; i++)
        q[i] = static_cast<std::uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
    return q;
}

inline std::vector<std::uint8_t> encode(const Image& im)
{
    const std::uint32_t nc = im.gray ? 1 : 3;
    const std::uint32_t hs = im.gray ? 1 : im.h_sampling, vs = im.gray ? 1 : im.v_sampling;
    const std::uint32_t mcu_w = 8 * hs, mcu_h = 8 * vs;
    const std::uint32_t mcus_x = (im.width + mcu_w - 1) / mcu_w, mcus_y = (im.height + mcu_h - 1) / mcu_h;
    // The planes, padded to whole MCUs by repeating the last row and column.
    const std::uint32_t pw = mcus_x * mcu_w, ph = mcus_y * mcu_h;
    std::vector<std::vector<double>> plane(nc, std::vector<double>(std::size_t{pw} * ph));
    for (std::uint32_t y = 0; y < ph; y++)
        for (std::uint32_t x = 0; x < pw; x++) {
            const std::uint32_t sx = std::min(x, im.width - 1), sy = std::min(y, im.height - 1);
            const std::size_t i = std::size_t{y} * pw + x;
            if (im.gray) {
                plane[0][i] = im.pixels[std::size_t{sy} * im.width + sx];
            } else {
                const std::uint8_t* p = &im.pixels[(std::size_t{sy} * im.width + sx) * 3];
                const double r = p[0], g = p[1], b = p[2];
                plane[0][i] = 0.299 * r + 0.587 * g + 0.114 * b;
                plane[1][i] = -0.168736 * r - 0.331264 * g + 0.5 * b + 128;
                plane[2][i] = 0.5 * r - 0.418688 * g - 0.081312 * b + 128;
            }
        }
    const std::array<std::uint8_t, 64> ql = scaled(kLumaQ, im.quality), qc = scaled(kChromaQ, im.quality);
    std::vector<std::uint8_t> out = {0xFF, 0xD8};
    if (im.app == 0) {
        out.insert(out.end(), {0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    } else if (im.app == 2) {
        out.insert(out.end(), {0xFF, 0xEE, 0, 14, 'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, 1});
    }
    for (int t = 0; t < (im.gray ? 1 : 2); t++) {
        out.insert(out.end(), {0xFF, 0xDB, 0, 67, static_cast<std::uint8_t>(t)});
        const auto& q = t ? qc : ql;
        for (std::size_t i = 0; i < 64; i++) out.push_back(q[kZigzag[i]]);
    }
    out.insert(out.end(), {0xFF, static_cast<std::uint8_t>(im.progressive ? 0xC2 : 0xC0)});
    put16(out, 8 + 3 * nc);
    out.push_back(8);
    put16(out, im.height);
    put16(out, im.width);
    out.push_back(static_cast<std::uint8_t>(nc));
    for (std::uint32_t c = 0; c < nc; c++) {
        out.push_back(static_cast<std::uint8_t>(c + 1));
        out.push_back(static_cast<std::uint8_t>(c == 0 ? (hs << 4 | vs) : 0x11));
        out.push_back(static_cast<std::uint8_t>(c == 0 ? 0 : 1));
    }
    const auto dht = [&](int cls, int id, const HuffSpec& spec) {
        out.insert(out.end(), {0xFF, 0xC4});
        put16(out, static_cast<std::uint32_t>(3 + 16 + spec.values.size()));
        out.push_back(static_cast<std::uint8_t>(cls << 4 | id));
        out.insert(out.end(), spec.bits.begin(), spec.bits.end());
        out.insert(out.end(), spec.values.begin(), spec.values.end());
    };
    dht(0, 0, kDcLuma);
    dht(1, 0, kAcLuma);
    if (!im.gray) {
        dht(0, 1, kDcChroma);
        dht(1, 1, kAcChroma);
    }
    if (im.restart_interval && !im.progressive) {
        out.insert(out.end(), {0xFF, 0xDD, 0, 4});
        put16(out, im.restart_interval);
    }
    // Every block's quantised coefficients (natural order), each component's blocks padded to whole MCUs.
    std::vector<std::uint32_t> bw_of(nc), bh_of(nc);
    std::vector<std::vector<std::array<int, 64>>> coefs(nc);
    for (std::uint32_t c = 0; c < nc; c++) {
        bw_of[c] = c == 0 ? mcus_x * hs : mcus_x;
        bh_of[c] = c == 0 ? mcus_y * vs : mcus_y;
        coefs[c].resize(std::size_t{bw_of[c]} * bh_of[c]);
        const std::uint32_t sh = c == 0 ? 1 : hs, sv = c == 0 ? 1 : vs;
        const auto& q = c == 0 ? ql : qc;
        for (std::uint32_t by = 0; by < bh_of[c]; by++)
            for (std::uint32_t bx = 0; bx < bw_of[c]; bx++) {
                double f[64];
                for (std::uint32_t y = 0; y < 8; y++)
                    for (std::uint32_t x = 0; x < 8; x++) {
                        double sum = 0;
                        for (std::uint32_t yy = 0; yy < sv; yy++)
                            for (std::uint32_t xx = 0; xx < sh; xx++)
                                sum += plane[c][std::size_t{(by * 8 + y) * sv + yy} * pw + (bx * 8 + x) * sh + xx];
                        f[y * 8 + x] = sum / (sh * sv) - 128;
                    }
                std::array<int, 64>& coef = coefs[c][std::size_t{by} * bw_of[c] + bx];
                for (int v = 0; v < 8; v++)
                    for (int u = 0; u < 8; u++) {
                        double t = 0;
                        for (int y = 0; y < 8; y++)
                            for (int x = 0; x < 8; x++)
                                t += f[y * 8 + x] * std::cos((2 * x + 1) * u * M_PI / 16) * std::cos((2 * y + 1) * v * M_PI / 16);
                        t *= 0.25 * (u ? 1 : M_SQRT1_2) * (v ? 1 : M_SQRT1_2);
                        coef[static_cast<std::size_t>(v * 8 + u)] =
                            static_cast<int>(std::lround(t / q[static_cast<std::size_t>(v * 8 + u)]));
                    }
            }
    }
    const std::array<Code, 256> dcl = codes_of(kDcLuma), acl = codes_of(kAcLuma), dcc = codes_of(kDcChroma),
                                acc = codes_of(kAcChroma);
    const auto magnitude = [](int v) {
        int n = 0;
        for (int a = v < 0 ? -v : v; a; a >>= 1) n++;
        return n;
    };
    // One scan: its components (0-based), spectral band, successive approximation.
    const auto scan = [&](std::vector<std::uint32_t> comps, int ss, int se, int ah, int al) {
        out.insert(out.end(), {0xFF, 0xDA});
        put16(out, static_cast<std::uint32_t>(6 + 2 * comps.size()));
        out.push_back(static_cast<std::uint8_t>(comps.size()));
        for (const std::uint32_t c : comps) {
            out.push_back(static_cast<std::uint8_t>(c + 1));
            out.push_back(static_cast<std::uint8_t>(c == 0 ? 0x00 : 0x11));
        }
        out.insert(out.end(), {static_cast<std::uint8_t>(ss), static_cast<std::uint8_t>(se),
                                  static_cast<std::uint8_t>(ah << 4 | al)});
        BitWriter bw(out);
        std::array<int, 3> pred{};
        const auto block = [&](std::uint32_t c, const std::array<int, 64>& coef) {
            const auto& dc = c == 0 ? dcl : dcc;
            const auto& ac = c == 0 ? acl : acc;
            if (ss == 0) {
                if (ah) { // DC refinement: the next bit
                    bw.put(static_cast<std::uint32_t>(coef[0] >> al) & 1, 1);
                    return;
                }
                const int v = coef[0] >> al; // arithmetic shift (T.81 G.1.2.1)
                const int diff = v - pred[c];
                pred[c] = v;
                const int dn = magnitude(diff);
                bw.put(dc[static_cast<std::size_t>(dn)].code, dc[static_cast<std::size_t>(dn)].length);
                if (dn) bw.put(static_cast<std::uint32_t>(diff < 0 ? diff + (1 << dn) - 1 : diff), dn);
                if (se == 0) return;
            }
            int run = 0;
            for (int k = ss == 0 ? 1 : ss; k <= se; k++) {
                const int v = coef[kZigzag[static_cast<std::size_t>(k)]];
                if (!v) {
                    run++;
                    continue;
                }
                while (run > 15) {
                    bw.put(ac[0xF0].code, ac[0xF0].length);
                    run -= 16;
                }
                const int n = magnitude(v);
                const std::size_t sym = static_cast<std::size_t>(run << 4 | n);
                bw.put(ac[sym].code, ac[sym].length);
                bw.put(static_cast<std::uint32_t>(v < 0 ? v + (1 << n) - 1 : v), n);
                run = 0;
            }
            if (run) bw.put(ac[0].code, ac[0].length); // EOB (in a progressive scan, a run of one block)
        };
        if (comps.size() == 1) {
            // A non-interleaved scan covers the component's own blocks only (T.81 A.2.2).
            const std::uint32_t c = comps[0];
            const std::uint32_t cw = (im.width * (c == 0 ? hs : 1) + hs - 1) / hs, ch = (im.height * (c == 0 ? vs : 1) + vs - 1) / vs;
            for (std::uint32_t by = 0; by < (ch + 7) / 8; by++)
                for (std::uint32_t bx = 0; bx < (cw + 7) / 8; bx++) block(c, coefs[c][std::size_t{by} * bw_of[c] + bx]);
        } else {
            std::uint32_t mcu = 0, rst = 0;
            for (std::uint32_t my = 0; my < mcus_y; my++)
                for (std::uint32_t mx = 0; mx < mcus_x; mx++, mcu++) {
                    if (im.restart_interval && !im.progressive && mcu && mcu % im.restart_interval == 0) {
                        bw.align();
                        out.insert(out.end(), {0xFF, static_cast<std::uint8_t>(0xD0 + (rst++ & 7))});
                        pred = {};
                    }
                    for (const std::uint32_t c : comps) {
                        const std::uint32_t h = c == 0 ? hs : 1, v = c == 0 ? vs : 1;
                        for (std::uint32_t by = 0; by < v; by++)
                            for (std::uint32_t bx = 0; bx < h; bx++)
                                block(c, coefs[c][std::size_t{my * v + by} * bw_of[c] + mx * h + bx]);
                    }
                }
        }
        bw.align();
    };
    std::vector<std::uint32_t> all;
    for (std::uint32_t c = 0; c < nc; c++) all.push_back(c);
    if (!im.progressive) {
        scan(all, 0, 63, 0, 0);
    } else {
        scan(all, 0, 0, 0, 1);            // DC, all but the last bit
        scan({0}, 1, 5, 0, 0);            // the luma's low AC band
        for (std::uint32_t c = 1; c < nc; c++) scan({c}, 1, 63, 0, 0);
        scan({0}, 6, 63, 0, 0);           // the rest of the luma
        scan(all, 0, 0, 1, 0);            // the DC's last bit
    }
    out.insert(out.end(), {0xFF, 0xD9});
    return out;
}

// A test picture: smooth gradients and an edge in each channel, with a pattern.
inline Image pattern(std::uint32_t w, std::uint32_t h, bool gray, std::uint32_t hs, std::uint32_t vs, std::uint32_t seed)
{
    Image im;
    im.width = w;
    im.height = h;
    im.gray = gray;
    im.h_sampling = hs;
    im.v_sampling = vs;
    const std::uint32_t nc = gray ? 1 : 3;
    for (std::uint32_t y = 0; y < h; y++)
        for (std::uint32_t x = 0; x < w; x++)
            for (std::uint32_t c = 0; c < nc; c++) {
                const double v = 128 + 90 * std::sin((x * (c + 1) + seed) * 0.21) * std::cos((y + c * 3) * 0.17) +
                    (x > w / 2 ? 30 : -30) * (c == 1 ? 1 : -1) + ((x * 7 + y * 13 + seed) % 11);
                im.pixels.push_back(static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0)));
            }
    return im;
}

} // namespace pspocket::test::jpeg
