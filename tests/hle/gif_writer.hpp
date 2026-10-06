#pragma once

// GIF files built in code for the image decoder tests (no game data): GIF87a or GIF89a, a global colour table and/or
// local ones of 2 to 256 entries, frames anywhere on the logical screen, interlaced or not, the LZW data at any minimum
// code size with optional early clear codes and any sub-block length, and extensions (graphic control with
// transparency, comment, application, plain text) in front of a frame. The encoder need not match any other: the
// decoders under test read the same bytes.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pspocket::test::gif {

struct Extension {
    std::uint8_t label = 0xFE;           // 0xF9 graphic control, 0xFE comment, 0xFF application, 0x01 plain text
    std::vector<std::vector<std::uint8_t>> blocks; // the sub-blocks (each up to 255 bytes)
};

struct Frame {
    std::uint32_t left = 0, top = 0, width = 0, height = 0;
    bool interlaced = false;
    int local_bits = 0;                  // 0: the global table; 1..8: a local table of 2^bits entries
    std::vector<std::array<std::uint8_t, 3>> local;
    std::vector<std::uint8_t> indices;   // width * height palette indices, row by row
    int min_code_size = 0;               // 0: max(2, the table's bits)
    int clear_every = 0;                 // a clear code after this many codes (0: only when the table is full; -1:
                                         // never, the full table kept as it is, the "deferred clear")
    int block_size = 255;                // the data's sub-block length
    std::vector<Extension> extensions;   // written before the image descriptor
};

struct File {
    bool gif87 = false;
    std::uint32_t width = 0, height = 0;
    int global_bits = 0; // 0: no global table
    int color_resolution = 7, background = 0, aspect = 0;
    bool sorted = false;
    std::vector<std::array<std::uint8_t, 3>> global;
    std::vector<Frame> frames;
    bool trailer = true;
};

inline void put16le(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}

// GIF's LZW (variable code width up to 12 bits, codes packed from the low bit), then cut into sub-blocks.
inline std::vector<std::uint8_t> lzw(const std::vector<std::uint8_t>& indices, int min_code_size, int clear_every,
    int block_size)
{
    std::vector<std::uint8_t> bytes;
    std::uint32_t acc = 0;
    int nbits = 0;
    const auto emit = [&](int code, int width) {
        acc |= static_cast<std::uint32_t>(code) << nbits;
        nbits += width;
        while (nbits >= 8) {
            bytes.push_back(static_cast<std::uint8_t>(acc));
            acc >>= 8;
            nbits -= 8;
        }
    };
    const int clear = 1 << min_code_size, end = clear + 1;
    std::map<std::pair<int, int>, int> table;
    int next = end + 1, width = min_code_size + 1, emitted = 0;
    const auto reset = [&] {
        table.clear();
        next = end + 1;
        width = min_code_size + 1;
        emitted = 0;
    };
    emit(clear, width);
    int prefix = -1;
    for (const std::uint8_t k : indices) {
        if (prefix < 0) {
            prefix = k;
            continue;
        }
        const auto it = table.find({prefix, k});
        if (it != table.end()) {
            prefix = it->second;
            continue;
        }
        emit(prefix, width);
        emitted++;
        if (next < 4096) {
            table[{prefix, k}] = next++;
            if (next > (1 << width) && width < 12) width++;
        }
        if ((next >= 4096 && clear_every >= 0) || (clear_every > 0 && emitted >= clear_every)) {
            emit(clear, width);
            reset();
        }
        prefix = k;
    }
    if (prefix >= 0) {
        emit(prefix, width);
        // The decoder adds an entry for this last code too, and may widen before reading the end code.
        if (emitted && next >= (1 << width) && width < 12) width++;
    }
    emit(end, width);
    if (nbits) bytes.push_back(static_cast<std::uint8_t>(acc));
    std::vector<std::uint8_t> out = {static_cast<std::uint8_t>(min_code_size)};
    for (std::size_t i = 0; i < bytes.size(); i += static_cast<std::size_t>(block_size)) {
        const std::size_t n = std::min(bytes.size() - i, static_cast<std::size_t>(block_size));
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), bytes.begin() + static_cast<long>(i), bytes.begin() + static_cast<long>(i + n));
    }
    out.push_back(0);
    return out;
}

// The rows of an interlaced image in the order GIF stores them (every 8th from 0, every 8th from 4, every 4th from 2,
// every 2nd from 1).
inline std::vector<std::uint32_t> interlaced_rows(std::uint32_t height)
{
    std::vector<std::uint32_t> rows;
    const std::uint32_t start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
    for (int p = 0; p < 4; p++)
        for (std::uint32_t y = start[p]; y < height; y += step[p]) rows.push_back(y);
    return rows;
}

inline std::vector<std::uint8_t> encode(const File& f)
{
    std::vector<std::uint8_t> out;
    const std::string sig = f.gif87 ? "GIF87a" : "GIF89a";
    out.insert(out.end(), sig.begin(), sig.end());
    put16le(out, f.width);
    put16le(out, f.height);
    out.push_back(static_cast<std::uint8_t>((f.global_bits ? 0x80 : 0) | (f.color_resolution << 4) | (f.sorted ? 8 : 0) |
        (f.global_bits ? f.global_bits - 1 : 0)));
    out.push_back(static_cast<std::uint8_t>(f.background));
    out.push_back(static_cast<std::uint8_t>(f.aspect));
    if (f.global_bits)
        for (int i = 0; i < (1 << f.global_bits); i++) {
            const auto c = static_cast<std::size_t>(i) < f.global.size() ? f.global[static_cast<std::size_t>(i)]
                                                                           : std::array<std::uint8_t, 3>{0, 0, 0};
            out.insert(out.end(), c.begin(), c.end());
        }
    for (const Frame& fr : f.frames) {
        for (const Extension& e : fr.extensions) {
            out.push_back(0x21);
            out.push_back(e.label);
            for (const auto& b : e.blocks) {
                out.push_back(static_cast<std::uint8_t>(b.size()));
                out.insert(out.end(), b.begin(), b.end());
            }
            out.push_back(0);
        }
        out.push_back(0x2C);
        put16le(out, fr.left);
        put16le(out, fr.top);
        put16le(out, fr.width);
        put16le(out, fr.height);
        out.push_back(static_cast<std::uint8_t>((fr.local_bits ? 0x80 : 0) | (fr.interlaced ? 0x40 : 0) |
            (fr.local_bits ? fr.local_bits - 1 : 0)));
        if (fr.local_bits)
            for (int i = 0; i < (1 << fr.local_bits); i++) {
                const auto c = static_cast<std::size_t>(i) < fr.local.size() ? fr.local[static_cast<std::size_t>(i)]
                                                                               : std::array<std::uint8_t, 3>{0, 0, 0};
                out.insert(out.end(), c.begin(), c.end());
            }
        std::vector<std::uint8_t> stored;
        if (fr.interlaced) {
            for (const std::uint32_t y : interlaced_rows(fr.height))
                stored.insert(stored.end(), fr.indices.begin() + static_cast<long>(y * fr.width),
                    fr.indices.begin() + static_cast<long>((y + 1) * fr.width));
        } else {
            stored = fr.indices;
        }
        const int bits = fr.local_bits ? fr.local_bits : (f.global_bits ? f.global_bits : 8);
        const int mcs = fr.min_code_size ? fr.min_code_size : std::max(2, bits);
        const std::vector<std::uint8_t> data = lzw(stored, mcs, fr.clear_every, fr.block_size);
        out.insert(out.end(), data.begin(), data.end());
    }
    if (f.trailer) out.push_back(0x3B);
    return out;
}

// A palette of 2^bits colours spread over the RGB cube, and a frame's indices: bands, a diagonal and noise.
inline std::vector<std::array<std::uint8_t, 3>> palette(int bits, std::uint32_t seed)
{
    std::vector<std::array<std::uint8_t, 3>> p;
    for (int i = 0; i < (1 << bits); i++)
        p.push_back({static_cast<std::uint8_t>((i * 37 + seed * 11) & 255), static_cast<std::uint8_t>((i * 91 + 64) & 255),
            static_cast<std::uint8_t>((255 - i * 53 + seed) & 255)});
    return p;
}

inline std::vector<std::uint8_t> indices(std::uint32_t w, std::uint32_t h, int bits, std::uint32_t seed)
{
    std::vector<std::uint8_t> v;
    const std::uint32_t n = 1u << bits;
    for (std::uint32_t y = 0; y < h; y++)
        for (std::uint32_t x = 0; x < w; x++) {
            std::uint32_t i = (x / 3 + y / 2 + seed) % n;
            if (x == y) i = n - 1;
            if ((x * 7 + y * 5 + seed) % 13 == 0) i = (i * 5 + 1) % n;
            v.push_back(static_cast<std::uint8_t>(i));
        }
    return v;
}

} // namespace pspocket::test::gif
