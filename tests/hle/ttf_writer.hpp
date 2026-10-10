#pragma once

// A TrueType font built in code for the font tests (no game data, no firmware): the tables FreeType needs for a face
// with outlines and a Unicode character map (head, hhea, maxp, cmap with a format 4 subtable, hmtx, loca, glyf, name,
// post), glyphs of one or two contours of on-curve points (squares, triangles, a ring) and a composite glyph made of
// two of them, the second scaled. The characters ' ' to '~' map to glyphs 1 to 95, U+3042 to the composite (96);
// glyph 0 is .notdef. Used by the parser fuzzing (test_fuzz_files.cpp, D-603), which damages it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pspocket::test::ttf {

inline void put16(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
inline void put32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    put16(v, x >> 16);
    put16(v, x & 0xFFFF);
}

struct Point {
    int x, y;
};

// A simple glyph: its contours, each a closed loop of on-curve points, every coordinate a 16-bit delta.
inline std::vector<std::uint8_t> simple_glyph(const std::vector<std::vector<Point>>& contours)
{
    std::vector<std::uint8_t> g;
    int x_min = 0x7FFF, y_min = 0x7FFF, x_max = -0x8000, y_max = -0x8000;
    for (const auto& c : contours)
        for (const Point& p : c) {
            x_min = std::min(x_min, p.x);
            y_min = std::min(y_min, p.y);
            x_max = std::max(x_max, p.x);
            y_max = std::max(y_max, p.y);
        }
    put16(g, static_cast<std::uint32_t>(contours.size()));
    for (const int v : {x_min, y_min, x_max, y_max}) put16(g, static_cast<std::uint32_t>(v) & 0xFFFF);
    std::uint32_t points = 0;
    for (const auto& c : contours) {
        points += static_cast<std::uint32_t>(c.size());
        put16(g, points - 1); // endPtsOfContours
    }
    put16(g, 0); // instructionLength
    for (std::uint32_t i = 0; i < points; i++) g.push_back(0x01); // ON_CURVE_POINT, 16-bit x and y deltas
    int last = 0;
    for (const auto& c : contours)
        for (const Point& p : c) {
            put16(g, static_cast<std::uint32_t>(p.x - last) & 0xFFFF);
            last = p.x;
        }
    last = 0;
    for (const auto& c : contours)
        for (const Point& p : c) {
            put16(g, static_cast<std::uint32_t>(p.y - last) & 0xFFFF);
            last = p.y;
        }
    while (g.size() % 4) g.push_back(0);
    return g;
}

// A composite glyph: `first` as it is, `second` moved by (dx, dy) and scaled by `scale` (2.14 fixed point).
inline std::vector<std::uint8_t> composite_glyph(std::uint16_t first, std::uint16_t second, int dx, int dy,
    std::uint16_t scale)
{
    std::vector<std::uint8_t> g;
    put16(g, 0xFFFF); // numberOfContours -1
    for (int i = 0; i < 4; i++) put16(g, 0);
    // ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES | MORE_COMPONENTS, then the scaled one without MORE_COMPONENTS.
    put16(g, 0x0001 | 0x0002 | 0x0020);
    put16(g, first);
    put16(g, 0);
    put16(g, 0);
    put16(g, 0x0001 | 0x0002 | 0x0008);
    put16(g, second);
    put16(g, static_cast<std::uint32_t>(dx) & 0xFFFF);
    put16(g, static_cast<std::uint32_t>(dy) & 0xFFFF);
    put16(g, scale);
    while (g.size() % 4) g.push_back(0);
    return g;
}

// The font file.
inline std::vector<std::uint8_t> font()
{
    constexpr std::uint32_t kGlyphs = 97, kUnits = 1000;
    // glyf and loca (long offsets): .notdef a box, ' ' empty, then shapes in turn, the composite last.
    std::vector<std::uint8_t> glyf, loca;
    const auto add_glyph = [&](const std::vector<std::uint8_t>& g) {
        put32(loca, static_cast<std::uint32_t>(glyf.size()));
        glyf.insert(glyf.end(), g.begin(), g.end());
    };
    const std::vector<Point> box = {{50, 0}, {50, 700}, {450, 700}, {450, 0}};
    add_glyph(simple_glyph({box, {{100, 50}, {400, 50}, {400, 650}, {100, 650}}}));
    add_glyph({}); // ' '
    for (std::uint32_t i = 2; i < kGlyphs - 1; i++) {
        const int w = 200 + static_cast<int>(i % 7) * 50, h = 300 + static_cast<int>(i % 5) * 80;
        switch (i % 3) {
        case 0: add_glyph(simple_glyph({{{40, 0}, {40, h}, {40 + w, h}, {40 + w, 0}}})); break;
        case 1: add_glyph(simple_glyph({{{30, 0}, {30 + w / 2, h}, {30 + w, 0}}})); break;
        default:
            add_glyph(simple_glyph({{{20, -100}, {20, h}, {20 + w, h}, {20 + w, -100}},
                {{60, -60}, {20 + w - 40, -60}, {20 + w - 40, h - 40}, {60, h - 40}}}));
            break;
        }
    }
    add_glyph(composite_glyph(3, 4, 300, 100, 0x2000)); // 0.5
    put32(loca, static_cast<std::uint32_t>(glyf.size()));

    std::vector<std::uint8_t> head;
    put32(head, 0x00010000);
    put32(head, 0x00010000); // fontRevision
    put32(head, 0);          // checkSumAdjustment
    put32(head, 0x5F0F3CF5);
    put16(head, 0x000B);     // flags: baseline at 0, lsb at 0, integer scaling
    put16(head, kUnits);
    for (int i = 0; i < 4; i++) put32(head, 0); // created, modified
    put16(head, 0);
    put16(head, static_cast<std::uint32_t>(-100) & 0xFFFF);
    put16(head, 1000);
    put16(head, 900);
    put16(head, 0);  // macStyle
    put16(head, 8);  // lowestRecPPEM
    put16(head, 2);  // fontDirectionHint
    put16(head, 1);  // indexToLocFormat: long
    put16(head, 0);  // glyphDataFormat

    std::vector<std::uint8_t> hhea;
    put32(hhea, 0x00010000);
    put16(hhea, 800);
    put16(hhea, static_cast<std::uint32_t>(-200) & 0xFFFF);
    put16(hhea, 90); // lineGap
    put16(hhea, 1000);
    put16(hhea, 0);
    put16(hhea, 0);
    put16(hhea, 1000);
    put16(hhea, 1); // caretSlopeRise
    put16(hhea, 0);
    put16(hhea, 0);
    for (int i = 0; i < 4; i++) put16(hhea, 0);
    put16(hhea, 0);       // metricDataFormat
    put16(hhea, kGlyphs); // numberOfHMetrics

    std::vector<std::uint8_t> maxp;
    put32(maxp, 0x00010000);
    put16(maxp, kGlyphs);
    put16(maxp, 16); // maxPoints
    put16(maxp, 2);  // maxContours
    put16(maxp, 8);  // maxCompositePoints
    put16(maxp, 3);  // maxCompositeContours
    put16(maxp, 2);  // maxZones
    for (int i = 0; i < 7; i++) put16(maxp, 0);
    put16(maxp, 2); // maxComponentElements
    put16(maxp, 1); // maxComponentDepth

    std::vector<std::uint8_t> hmtx;
    for (std::uint32_t i = 0; i < kGlyphs; i++) {
        put16(hmtx, 500 + (i % 7) * 40);
        put16(hmtx, 30);
    }

    // cmap: one Windows Unicode (3, 1) subtable of format 4: ' '..'~' to glyphs 1..95, U+3042 to 96, the end segment.
    std::vector<std::uint8_t> sub;
    const std::vector<std::array<std::uint32_t, 3>> segments = {{0x20, 0x7E, 1}, {0x3042, 0x3042, 96}, {0xFFFF, 0xFFFF, 0}};
    const std::uint32_t seg_x2 = static_cast<std::uint32_t>(segments.size()) * 2;
    put16(sub, 4);
    put16(sub, 16 + seg_x2 * 4); // length
    put16(sub, 0);               // language
    put16(sub, seg_x2);
    put16(sub, 4); // searchRange: 2 * 2^floor(log2(3))
    put16(sub, 1); // entrySelector
    put16(sub, seg_x2 - 4);
    for (const auto& s : segments) put16(sub, s[1]); // endCode
    put16(sub, 0);                                   // reservedPad
    for (const auto& s : segments) put16(sub, s[0]); // startCode
    for (const auto& s : segments) put16(sub, s[1] == 0xFFFF ? 1 : (s[2] - s[0]) & 0xFFFF); // idDelta
    for (std::size_t i = 0; i < segments.size(); i++) put16(sub, 0); // idRangeOffset
    std::vector<std::uint8_t> cmap;
    put16(cmap, 0);
    put16(cmap, 1);
    put16(cmap, 3);
    put16(cmap, 1);
    put32(cmap, 12);
    cmap.insert(cmap.end(), sub.begin(), sub.end());

    // name: the family and full names, Windows Unicode.
    std::vector<std::uint8_t> name, strings;
    const std::vector<std::pair<std::uint32_t, std::string>> names = {{1, "Synthetic"}, {2, "Regular"}, {4, "Synthetic Regular"}};
    put16(name, 0);
    put16(name, static_cast<std::uint32_t>(names.size()));
    put16(name, 6 + 12 * static_cast<std::uint32_t>(names.size()));
    for (const auto& [id, text] : names) {
        put16(name, 3);
        put16(name, 1);
        put16(name, 0x409);
        put16(name, id);
        put16(name, static_cast<std::uint32_t>(text.size() * 2));
        put16(name, static_cast<std::uint32_t>(strings.size()));
        for (const char c : text) put16(strings, static_cast<std::uint8_t>(c));
    }
    name.insert(name.end(), strings.begin(), strings.end());

    std::vector<std::uint8_t> post;
    put32(post, 0x00030000);
    for (int i = 0; i < 7; i++) put32(post, 0);

    // The table directory, its records sorted by tag, each table 4-byte aligned with its checksum.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>*>> tables = {{"cmap", &cmap}, {"glyf", &glyf},
        {"head", &head}, {"hhea", &hhea}, {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}, {"name", &name},
        {"post", &post}};
    std::vector<std::uint8_t> out;
    const std::uint32_t n = static_cast<std::uint32_t>(tables.size());
    put32(out, 0x00010000);
    put16(out, n);
    put16(out, 128); // searchRange: 16 * 2^floor(log2(9))
    put16(out, 3);
    put16(out, n * 16 - 128);
    std::uint32_t offset = 12 + 16 * n;
    std::vector<std::uint8_t> body;
    for (auto& [tag, data] : tables) {
        while (data->size() % 4) data->push_back(0);
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < data->size(); i += 4)
            sum += static_cast<std::uint32_t>((*data)[i]) << 24 | static_cast<std::uint32_t>((*data)[i + 1]) << 16 |
                   static_cast<std::uint32_t>((*data)[i + 2]) << 8 | (*data)[i + 3];
        for (const char c : tag) out.push_back(static_cast<std::uint8_t>(c));
        put32(out, sum);
        put32(out, offset);
        put32(out, static_cast<std::uint32_t>(data->size()));
        offset += static_cast<std::uint32_t>(data->size());
        body.insert(body.end(), data->begin(), data->end());
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace pspocket::test::ttf
