#pragma once

// Synthetic MPEG audio layer III streams for tests (D-974): frames of random content that decoders must read alike,
// built here bit by bit. Each granule and channel gets random side information (block types 0 to 3, mixed blocks,
// scale factors with their compressions and, MPEG-1, scfsi; preflag; scalefac_scale; subblock gains; region counts;
// table selections of every table; count1 tables A and B), random quantised lines within what its tables code
// (linbits up to 13) and count1 quadruples, all Huffman coded with the trees of mp3_huffman.h (the codes found by
// walking them). The main data use the bit reservoir (main_data_begin up to 511, or 255 for MPEG-2 and 2.5), and the
// bytes no granule reads are random (ancillary data: a decoder that reads past part2_3_length reads garbage).
// MPEG-1, 2 and 2.5 at every rate; mono, stereo, dual channel and joint stereo with M/S; CRC.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "mp3_huffman.h"
#include "spu_test_util.hpp"

namespace pspocket::spu::test::mp3 {

struct BitWriter {
    std::vector<uint8_t> bytes;
    uint64_t bit = 0;
    void put(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--) {
            if (bit / 8 >= bytes.size()) bytes.push_back(0);
            if ((v >> i) & 1) bytes[bit / 8] |= static_cast<uint8_t>(0x80u >> (bit % 8));
            bit++;
        }
    }
    void append(const BitWriter& o)
    {
        for (uint64_t i = 0; i < o.bit; i++) put((o.bytes[i / 8] >> (7 - i % 8)) & 1, 1);
    }
};

struct Code {
    uint32_t code = 0;
    int len = -1;
};

// The code of each leaf of a tree (mpglib's form: -n a node, bit 1 skips n entries).
inline std::array<Code, 256> tree_codes(const int16_t* tree)
{
    std::array<Code, 256> out{};
    struct Walk {
        const int16_t* t;
        std::array<Code, 256>* out;
        void go(int at, uint32_t code, int len)
        {
            const int y = t[at];
            if (y >= 0) {
                (*out)[static_cast<size_t>(y)] = {code, len};
                return;
            }
            go(at + 1, code << 1, len + 1);
            go(at + 1 - y, code << 1 | 1, len + 1);
        }
    } w{tree, &out};
    w.go(0, 0, 0);
    return out;
}

// The largest value a table codes for x or y (15 and more with linbits).
inline int table_max(int t)
{
    static constexpr int kMax[16] = {0, 1, 2, 2, 0, 3, 3, 5, 5, 5, 7, 7, 7, 15, 0, 15};
    if (t < 16) return kMax[t];
    return 15 + (1 << mp3_ht[t].linbits) - 1;
}

struct BandInfo {
    int longIdx[23];
};
// Long band starts (ISO 11172-3 / 13818-3 table B.8, mpglib's bandInfo), by the decoder's rate index.
inline const int* long_idx(int sfreq)
{
    static constexpr int kLong[9][23] = {
        {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
        {0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
        {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
        {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
        {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 330, 394, 464, 540, 576},
        {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
        {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
        {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
        {0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576},
    };
    return kLong[sfreq];
}

// What a stream may use.
struct Options {
    int version = 1;         // 1: MPEG-1, 2: MPEG-2, 25: MPEG-2.5
    int rate_index = 0;      // 0..2
    int mode = 0;            // 0 stereo, 1 joint (M/S as ms says), 2 dual channel, 3 mono
    bool ms = true;          // joint stereo: mode_ext 2 (else 0)
    bool crc = false;
    int block_types = 100;   // percent of granules not long (types 1, 2, 3)
    int mixed = 30;          // percent of short-block granules mixed
    int max_big = 288;       // big_values at most
    int gain_min = 100, gain_max = 150; // below full scale mostly (overdriven output differs between decoders more)
    int linbits_tables = 20; // percent of regions coded with tables 16 to 31
    int reservoir = 60;      // percent of frames that start their main data in the reservoir
    int count1_max = 40;     // count1 quadruples at most
    bool lsf_partitions = true; // MPEG-2 and 2.5: scale factor compressions of every partition (else the first only)
};

struct Stream {
    std::vector<std::vector<uint8_t>> frames;
};

namespace detail {

struct GrCh {
    int part2_3_length = 0, big_values = 0, global_gain = 0, scalefac_compress = 0;
    int window_switching = 0, block_type = 0, mixed = 0, table_select[3] = {}, subblock_gain[3] = {};
    int region0_count = 0, region1_count = 0, preflag = 0, scalefac_scale = 0, count1table = 0;
    BitWriter main; // scale factors then Huffman codes
};

inline int sfreq_of(const Options& o)
{
    return o.version == 1 ? o.rate_index : o.version == 2 ? 3 + o.rate_index : 6 + o.rate_index;
}

inline void huffman_part(Rng& r, const Options& o, GrCh& g, int region1start, int region2start)
{
    static const std::array<std::array<Code, 256>, 32> codes = [] {
        std::array<std::array<Code, 256>, 32> c{};
        for (int t = 0; t < 32; t++) c[static_cast<size_t>(t)] = tree_codes(mp3_ht[t].tree);
        return c;
    }();
    static const std::array<std::array<Code, 256>, 2> c1codes = {tree_codes(mp3_htc[0]), tree_codes(mp3_htc[1])};
    const int bv = g.big_values;
    for (int i = 0; i < bv; i++) {
        const int region = i < region1start ? 0 : i < region2start ? 1 : 2;
        const int t = g.table_select[region];
        const int mx = table_max(t);
        int v[2] = {0, 0};
        for (int& x : v) {
            if (!mx) break;
            const int k = static_cast<int>(r.below(100));
            if (k < 35) x = 0;
            else if (k < 85) x = static_cast<int>(r.below(static_cast<uint32_t>(std::min(mx, 15)) + 1));
            else x = static_cast<int>(r.below(static_cast<uint32_t>(mx) + 1));
        }
        const int cx = std::min(v[0], 15), cy = std::min(v[1], 15);
        const Code c = codes[static_cast<size_t>(t)][static_cast<size_t>(cx << 4 | cy)];
        g.main.put(c.code, c.len);
        const uint32_t linbits = mp3_ht[t].linbits;
        for (int j = 0; j < 2; j++) {
            if (v[j] >= 15 && linbits) g.main.put(static_cast<uint32_t>(v[j] - 15), static_cast<int>(linbits));
            if (v[j]) g.main.put(r.below(2), 1);
        }
    }
    // count1 quadruples in what is left of the 576 lines
    const int quads = std::min(static_cast<int>(r.below(static_cast<uint32_t>(o.count1_max) + 1)), (576 - 2 * bv) / 4);
    for (int q = 0; q < quads; q++) {
        const uint32_t vwxy = r.below(16);
        const Code c = c1codes[static_cast<size_t>(g.count1table)][vwxy];
        g.main.put(c.code, c.len);
        for (int b = 3; b >= 0; b--)
            if ((vwxy >> b) & 1) g.main.put(r.below(2), 1);
    }
}

inline int choose_table(Rng& r, const Options& o)
{
    if (r.below(100) < static_cast<uint32_t>(o.linbits_tables)) return 16 + static_cast<int>(r.below(16));
    static constexpr int kSmall[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15};
    return kSmall[r.below(sizeof kSmall / sizeof kSmall[0])];
}

// One granule and channel: side information and main data (scale factors, Huffman codes).
inline GrCh granule(Rng& r, const Options& o, int gr, int scfsi)
{
    GrCh g;
    const bool lsf = o.version != 1;
    const int sfreq = sfreq_of(o);
    g.global_gain = o.gain_min + static_cast<int>(r.below(static_cast<uint32_t>(o.gain_max - o.gain_min + 1)));
    g.scalefac_scale = static_cast<int>(r.below(2));
    g.count1table = static_cast<int>(r.below(2));
    g.window_switching = r.below(100) < static_cast<uint32_t>(o.block_types) ? 1 : 0;
    int region1start, region2start;
    if (g.window_switching) {
        static constexpr int kTypes[3] = {1, 2, 3};
        g.block_type = kTypes[r.below(3)];
        g.mixed = g.block_type == 2 && r.below(100) < static_cast<uint32_t>(o.mixed) ? 1 : 0;
        for (int& s : g.subblock_gain) s = static_cast<int>(r.below(8));
        g.table_select[0] = choose_table(r, o);
        g.table_select[1] = choose_table(r, o);
        if (!lsf) region1start = 18;
        else if (g.block_type == 2) region1start = g.mixed ? 24 : 18;
        else region1start = 27;
        if (lsf && sfreq == 8) region1start *= 2;
        region2start = 288;
    } else {
        for (int& t : g.table_select) t = choose_table(r, o);
        g.region0_count = static_cast<int>(r.below(16));
        g.region1_count = static_cast<int>(r.below(8));
        while (g.region0_count + g.region1_count + 2 > 22) g.region1_count--;
        if (g.region1_count < 0) {
            g.region1_count = 0;
            g.region0_count = 20;
        }
        region1start = long_idx(sfreq)[g.region0_count + 1] >> 1;
        region2start = long_idx(sfreq)[g.region0_count + g.region1_count + 2] >> 1;
    }
    // scale factors
    if (!lsf) {
        g.scalefac_compress = static_cast<int>(r.below(16));
        static constexpr int kSlen[2][16] = {{0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4},
            {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3}};
        const int num0 = kSlen[0][g.scalefac_compress], num1 = kSlen[1][g.scalefac_compress];
        const auto put_n = [&](int count, int bits) {
            for (int i = 0; i < count; i++) g.main.put(r.below(1u << bits), bits);
        };
        if (g.block_type == 2) {
            if (g.mixed) {
                put_n(8, num0);
                put_n(9, num0);
            } else {
                put_n(18, num0);
            }
            put_n(18, num1);
        } else if (gr == 0 || scfsi < 0) {
            put_n(11, num0);
            put_n(10, num1);
        } else {
            if (!(scfsi & 8)) put_n(6, num0);
            if (!(scfsi & 4)) put_n(5, num0);
            if (!(scfsi & 2)) put_n(5, num1);
            if (!(scfsi & 1)) put_n(5, num1);
        }
        g.preflag = static_cast<int>(r.below(2));
    } else {
        g.scalefac_compress = static_cast<int>(r.below(o.lsf_partitions ? 512 : 400));
        // n_slen2 (mpglib layer3.c init): the four bit counts and the partition
        unsigned slen = 0;
        const int sfc = g.scalefac_compress;
        if (sfc < 400) {
            const int i = sfc / 80, j = sfc / 16 % 5, k = sfc / 4 % 4, l = sfc % 4;
            slen = static_cast<unsigned>(i | j << 3 | k << 6 | l << 9);
        } else if (sfc < 500) {
            const int s = sfc - 400, i = s / 20, j = s / 4 % 5, k = s % 4;
            slen = static_cast<unsigned>(i | j << 3 | k << 6 | 1 << 12);
        } else {
            const int s = sfc - 500, i = s / 3, j = s % 3;
            slen = static_cast<unsigned>(i | j << 3 | 2 << 12 | 1 << 15);
        }
        static constexpr int kStab[3][6][4] = {
            {{6, 5, 5, 5}, {6, 5, 7, 3}, {11, 10, 0, 0}, {7, 7, 7, 0}, {6, 6, 6, 3}, {8, 8, 5, 0}},
            {{9, 9, 9, 9}, {9, 9, 12, 6}, {18, 18, 0, 0}, {12, 12, 12, 0}, {12, 9, 9, 6}, {15, 12, 9, 0}},
            {{6, 9, 9, 9}, {6, 9, 12, 6}, {15, 18, 0, 0}, {6, 15, 12, 0}, {6, 12, 9, 6}, {6, 18, 9, 0}}};
        const int n = g.block_type == 2 ? (g.mixed ? 2 : 1) : 0;
        const int* pnt = kStab[n][(slen >> 12) & 7];
        for (int i = 0; i < 4; i++) {
            const int num = static_cast<int>(slen & 7);
            slen >>= 3;
            for (int j = 0; j < pnt[i] && num; j++) g.main.put(r.below(1u << num), num);
        }
    }
    const int scale_bits = static_cast<int>(g.main.bit);
    g.big_values = static_cast<int>(r.below(static_cast<uint32_t>(o.max_big) + 1));
    huffman_part(r, o, g, region1start, region2start);
    g.part2_3_length = static_cast<int>(g.main.bit);
    (void)scale_bits;
    return g;
}

} // namespace detail

// A stream of `count` frames.
inline Stream stream(Rng& r, const Options& o, int count)
{
    using namespace detail;
    static constexpr int kRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    static constexpr int kKbps[2][15] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
    const bool lsf = o.version != 1;
    const int rate = kRates[o.version == 1 ? 0 : o.version == 2 ? 1 : 2][o.rate_index];
    const int channels = o.mode == 3 ? 1 : 2;
    const int granules = lsf ? 1 : 2;
    const int side = lsf ? (channels == 1 ? 9 : 17) : (channels == 1 ? 17 : 32);
    const int max_back = lsf ? 255 : 511;

    struct Built {
        int bitrate_index, padding, mode_ext, scfsi[2];
        std::vector<GrCh> g; // gr * channels + ch
        BitWriter main;
        int main_data_begin = 0;
        int frame_bytes = 0;
    };
    std::vector<Built> built;
    int reservoir = 0; // bytes the frames so far left unused at the end of their main data areas (up to max_back)
    for (int k = 0; k < count; k++) {
        Built b;
        b.mode_ext = o.mode == 1 ? (o.ms ? 2 : 0) : static_cast<int>(r.below(4)) & 0; // no intensity stereo
        for (int ch = 0; ch < 2; ch++) b.scfsi[ch] = lsf ? -1 : static_cast<int>(r.below(16));
        for (int gr = 0; gr < granules; gr++)
            for (int ch = 0; ch < channels; ch++) b.g.push_back(granule(r, o, gr, gr == 1 ? b.scfsi[ch] : -1));
        for (const GrCh& g : b.g) b.main.append(g.main);
        const int need = static_cast<int>((b.main.bit + 7) / 8);
        // the smallest bit rate whose main data area, with what the reservoir lends, holds this frame's main data
        const int lend = r.below(100) < static_cast<uint32_t>(o.reservoir) ? std::min(reservoir, max_back) : 0;
        b.main_data_begin = lend;
        b.padding = static_cast<int>(r.below(2));
        b.bitrate_index = 0;
        for (int i = 1; i < 15; i++) {
            const int bytes = (lsf ? 72 : 144) * 1000 * kKbps[lsf][i] / rate + b.padding;
            const int area = bytes - 4 - (o.crc ? 2 : 0) - side;
            if (area + lend >= need) {
                b.bitrate_index = i;
                b.frame_bytes = bytes;
                break;
            }
        }
        if (!b.bitrate_index) { // too much: drop this frame's lines and try again
            k--;
            continue;
        }
        const int area = b.frame_bytes - 4 - (o.crc ? 2 : 0) - side;
        reservoir = std::min(max_back, area + lend - need);
        built.push_back(std::move(b));
    }
    // Lay the main data out: frame k's begin main_data_begin bytes before its own area, in the areas' concatenation.
    std::vector<int> area_start;
    int total = 0;
    for (const Built& b : built) {
        area_start.push_back(total);
        total += b.frame_bytes - 4 - (o.crc ? 2 : 0) - side;
    }
    std::vector<uint8_t> areas(static_cast<size_t>(total));
    for (uint8_t& v : areas) v = static_cast<uint8_t>(r.below(256)); // ancillary bytes: garbage
    for (size_t k = 0; k < built.size(); k++) {
        const int at = area_start[k] - built[k].main_data_begin;
        for (size_t i = 0; i < built[k].main.bytes.size(); i++) {
            const uint8_t mask = i + 1 == built[k].main.bytes.size() && built[k].main.bit % 8
                ? static_cast<uint8_t>(0xFF00u >> (built[k].main.bit % 8))
                : 0xFF;
            uint8_t& dst = areas[static_cast<size_t>(at) + i];
            dst = static_cast<uint8_t>((dst & ~mask) | (built[k].main.bytes[i] & mask));
        }
    }
    Stream s;
    for (size_t k = 0; k < built.size(); k++) {
        const Built& b = built[k];
        BitWriter w;
        w.put(0x7FF, 11);
        w.put(o.version == 1 ? 3 : o.version == 2 ? 2 : 0, 2);
        w.put(1, 2); // layer III
        w.put(o.crc ? 0 : 1, 1);
        w.put(static_cast<uint32_t>(b.bitrate_index), 4);
        w.put(static_cast<uint32_t>(o.rate_index), 2);
        w.put(static_cast<uint32_t>(b.padding), 1);
        w.put(0, 1);
        w.put(static_cast<uint32_t>(o.mode), 2);
        w.put(static_cast<uint32_t>(b.mode_ext), 2);
        w.put(0, 1);
        w.put(1, 1);
        w.put(0, 2);
        if (o.crc) w.put(0, 16);
        const uint64_t side_from = w.bit;
        w.put(static_cast<uint32_t>(b.main_data_begin), lsf ? 8 : 9);
        w.put(0, lsf ? (channels == 1 ? 1 : 2) : (channels == 1 ? 5 : 3));
        if (!lsf)
            for (int ch = 0; ch < channels; ch++) w.put(static_cast<uint32_t>(b.scfsi[ch]), 4);
        for (int gr = 0; gr < granules; gr++)
            for (int ch = 0; ch < channels; ch++) {
                const GrCh& g = b.g[static_cast<size_t>(gr * channels + ch)];
                w.put(static_cast<uint32_t>(g.part2_3_length), 12);
                w.put(static_cast<uint32_t>(g.big_values), 9);
                w.put(static_cast<uint32_t>(g.global_gain), 8);
                w.put(static_cast<uint32_t>(g.scalefac_compress), lsf ? 9 : 4);
                w.put(static_cast<uint32_t>(g.window_switching), 1);
                if (g.window_switching) {
                    w.put(static_cast<uint32_t>(g.block_type), 2);
                    w.put(static_cast<uint32_t>(g.mixed), 1);
                    w.put(static_cast<uint32_t>(g.table_select[0]), 5);
                    w.put(static_cast<uint32_t>(g.table_select[1]), 5);
                    for (int s3 : g.subblock_gain) w.put(static_cast<uint32_t>(s3), 3);
                } else {
                    for (int t : g.table_select) w.put(static_cast<uint32_t>(t), 5);
                    w.put(static_cast<uint32_t>(g.region0_count), 4);
                    w.put(static_cast<uint32_t>(g.region1_count), 3);
                }
                if (!lsf) w.put(static_cast<uint32_t>(g.preflag), 1);
                w.put(static_cast<uint32_t>(g.scalefac_scale), 1);
                w.put(static_cast<uint32_t>(g.count1table), 1);
            }
        const uint64_t side_to = w.bit;
        std::vector<uint8_t> f = w.bytes;
        const auto from = areas.begin() + area_start[k];
        f.insert(f.end(), from, from + (b.frame_bytes - 4 - (o.crc ? 2 : 0) - side));
        if (o.crc) {
            uint16_t crc = 0xFFFF;
            const auto feed = [&](uint64_t a, uint64_t z) {
                for (uint64_t i = a; i < z; i++) {
                    const bool in = (f[i / 8] >> (7 - i % 8)) & 1;
                    const bool top = crc & 0x8000;
                    crc = static_cast<uint16_t>(crc << 1);
                    if (in != top) crc ^= 0x8005;
                }
            };
            feed(16, 32);
            feed(side_from, side_to);
            f[4] = static_cast<uint8_t>(crc >> 8);
            f[5] = static_cast<uint8_t>(crc);
        }
        s.frames.push_back(std::move(f));
    }
    return s;
}

} // namespace pspocket::spu::test::mp3
