#pragma once

// The libfont scripts the font comparisons share (D-526): set-up, metrics, images and glyphs of fonts through libfont's
// API, kept as text lines (tests/hle/freetype_compare.hpp). Run over Sony's libfont by test_fontft_compare.cpp (with
// pspocket's cellFontFT under it) and over pspocket's by test_font.cpp.

#include "freetype_compare.hpp"

#include <bit>
#include <tuple>

namespace pspocket::hle::test::ft::fonts {

inline std::string font(const std::string& name) { return "/dev_flash/data/font/" + name; }

inline u64 sx(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

inline std::string words(Run& r, u32 a, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{}{:#x}", i ? " " : "", r.r32(a + 4 * i));
    return s;
}

// FNV-1a over guest bytes (pixels), rows of `w` bytes `pitch` apart.
inline u64 fnv(Run& r, u32 a, s32 w, s32 rows, s32 pitch)
{
    u64 h = 0xcbf29ce484222325ull;
    if (!a || w <= 0 || rows <= 0 || w > 0x10000 || rows > 0x10000) return 0;
    for (s32 y = 0; y < rows; y++)
        for (s32 x = 0; x < w; x++) {
            h ^= r.b[a + static_cast<u32>(y * pitch + x)];
            h *= 0x100000001b3ull;
        }
    return h;
}

inline std::string fwords(Run& r, u32 a, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{}{}", i ? " " : "", std::bit_cast<float>(r.r32(a + 4 * i)));
    return s;
}

// libfont's set-up: the file cache, the library of FreeType fonts over the script's memory interface.
struct FontSetup {
    u32 lib = 0, cache = 0;
};

inline FontSetup font_setup(Run& r, u32 cache_slots = 8)
{
    FontSetup fs;
    const u32 cfg = r.alloc(32), entries = r.alloc(16 * 4);
    fs.cache = r.alloc(24 + cache_slots * 0x2008);
    r.w32(cfg, fs.cache);
    r.w32(cfg + 4, 24 + cache_slots * 0x2008);
    r.w32(cfg + 8, 4);
    r.w32(cfg + 12, entries);
    r.w32(cfg + 16, 0);
    r.rec(std::format("initialize {:#x}", r.call("cellFont", "cellFontInitializeWithRevision", {0x7F, cfg})));
    const u32 ftcfg = r.alloc(32), out = r.alloc(8), iface = r.font_memory();
    r.w32(ftcfg, 0);
    for (u32 i = 0; i < 5; i++) r.w32(ftcfg + 4 + 4 * i, r.r32(iface + 4 * i));
    r.w32(out, 0xEEEEEEEE);
    r.rec(std::format("init library {:#x} null cfg {:#x} null out {:#x}",
        r.call("cellFontFT", "cellFontInitLibraryFreeType", {ftcfg, out}),
        r.call("cellFontFT", "cellFontInitLibraryFreeType", {0, r.alloc(8)}),
        r.call("cellFontFT", "cellFontInitLibraryFreeType", {ftcfg, 0})));
    fs.lib = r.r32(out);
    r.rec(std::format("library {} type {:#x} version {:#x}", fs.lib != 0, r.r32(fs.lib), r.r32(fs.lib + 4)));
    const u32 flags = r.alloc(8);
    r.rec(std::format("revision {:#x} {:#x}", r.call("cellFontFT", "cellFontFTGetInitializedRevisionFlags", {flags}),
        ps_read64(r.b, flags)));
    r.call("cellFontFT", "cellFontFTGetRevisionFlags", {flags});
    r.rec(std::format("revision flags {:#x}", ps_read64(r.b, flags)));
    const u32 again = r.alloc(8);
    r.w32(again, 0xEEEEEEEE);
    r.rec(std::format("init again {:#x} out {:#x}", r.call("cellFontFT", "cellFontInitLibraryFreeTypeWithRevision", {3, ftcfg, again}),
        r.r32(again)));
    return fs;
}

inline std::string trans_text(Run& r, u32 T, u32 surf_buf)
{
    const u32 image = r.r32(T), iwb = r.r32(T + 4), iw = r.r32(T + 8), ih = r.r32(T + 12), surf = r.r32(T + 16);
    return std::format("trans wb {} {}x{} surface {:+} wb {} pixels {:#x}", iwb, iw, ih,
        static_cast<s64>(surf) - static_cast<s64>(surf_buf), r.r32(T + 20),
        image ? fnv(r, image, static_cast<s32>(iw), static_cast<s32>(ih), static_cast<s32>(iwb)) : 0);
}

// A font's metrics, layouts and kerning through libfont.
inline void font_metrics(Run& r, u32 font, const std::string& tag, const std::vector<u32>& codes)
{
    const u32 m = r.alloc(32), l = r.alloc(12), k = r.alloc(8);
    std::memset(r.b + l, 0xEE, 12);
    r.rec(std::format("{} hlayout {:#x} {}", tag, r.call("cellFont", "cellFontGetHorizontalLayout", {font, l}), fwords(r, l, 3)));
    std::memset(r.b + l, 0xEE, 12);
    r.rec(std::format("{} vlayout {:#x} {}", tag, r.call("cellFont", "cellFontGetVerticalLayout", {font, l}), fwords(r, l, 3)));
    for (const u32 c : codes) {
        std::memset(r.b + m, 0xEE, 32);
        r.rec(std::format("{} metrics {:#x}: {:#x} {}", tag, c, r.call("cellFont", "cellFontGetCharGlyphMetrics", {font, c, m}),
            fwords(r, m, 8)));
        std::memset(r.b + m, 0xEE, 32);
        r.rec(std::format("{} vmetrics {:#x}: {:#x} {}", tag, c,
            r.call("cellFont", "cellFontGetCharGlyphMetricsVertical", {font, c, m}), fwords(r, m, 8)));
    }
    for (usize i = 0; i + 1 < codes.size(); i++) {
        std::memset(r.b + k, 0xEE, 8);
        r.rec(std::format("{} kerning {:#x} {:#x}: {:#x} {}", tag, codes[i], codes[i + 1],
            r.call("cellFont", "cellFontGetKerning", {font, codes[i], codes[i + 1], k}), fwords(r, k, 2)));
    }
}

// Glyph images through a renderer bound to the font.
inline void font_images(Run& r, u32 font, const std::string& tag, const std::vector<u32>& codes, u32 surf, u32 surf_buf)
{
    const u32 m = r.alloc(32), T = r.alloc(24), k = r.alloc(8);
    for (const u32 c : codes) {
        for (const auto& [x, y, name] : {std::tuple{10.0, 40.0, "cellFontRenderCharGlyphImage"},
                 std::tuple{33.25, 71.5, "cellFontRenderCharGlyphImageHorizontal"},
                 std::tuple{120.75, 20.125, "cellFontRenderCharGlyphImageVertical"},
                 std::tuple{-7.0, 3.0, "cellFontRenderCharGlyphImage"}}) {
            std::memset(r.b + m, 0xEE, 32);
            std::memset(r.b + T, 0xEE, 24);
            const u32 e = r.callf("cellFont", name, {font, c, surf, 0, 0, m, T}, {x, y});
            r.rec(std::format("{} {} {:#x} at {} {}: {:#x} {} {}", tag, name, c, x, y, e, fwords(r, m, 8), trans_text(r, T, surf_buf)));
        }
        std::memset(r.b + m, 0xEE, 32);
        r.rec(std::format("{} render metrics {:#x}: {:#x} {}", tag, c,
            r.call("cellFont", "cellFontGetRenderCharGlyphMetrics", {font, c, m}), fwords(r, m, 8)));
        std::memset(r.b + m, 0xEE, 32);
        r.rec(std::format("{} render vmetrics {:#x}: {:#x} {}", tag, c,
            r.call("cellFont", "cellFontGetRenderCharGlyphMetricsVertical", {font, c, m}), fwords(r, m, 8)));
    }
    for (usize i = 0; i + 1 < codes.size(); i++) {
        std::memset(r.b + k, 0xEE, 8);
        r.rec(std::format("{} scaled kerning {:#x} {:#x}: {:#x} {}", tag, codes[i], codes[i + 1],
            r.call("cellFont", "cellFontGetRenderScaledKerning", {font, codes[i], codes[i + 1], k}), fwords(r, k, 2)));
    }
}

// Outlines generated by the font and drawn by the renderer with a style.
inline void font_glyphs(Run& r, u32 font, u32 renderer, const std::string& tag, const std::vector<u32>& codes, u32 surf, u32 surf_buf)
{
    const u32 gp = r.alloc(8), m = r.alloc(32), T = r.alloc(24), st = r.alloc(16), v = r.alloc(8);
    for (const u32 c : codes) {
        for (const char* gen : {"cellFontGenerateCharGlyph", "cellFontGenerateCharGlyphVertical"}) {
            r.w32(gp, 0);
            const u32 e = r.call("cellFont", gen, {font, c, gp});
            const u32 g = r.r32(gp);
            std::string s = std::format("{} {} {:#x}: {:#x}", tag, gen, c, e);
            if (!e && g) {
                const s16 nc = r.s16at(g + 40), np = r.s16at(g + 42);
                s += std::format(" type {:#x} {} size {} metrics {} outline {}c {}p flags {:#x}", r.r16(g), r.r16(g + 2), r.r32(g + 4),
                    fwords(r, g + 8, 8), nc, np, r.r32(g + 56));
                u64 h = 0xcbf29ce484222325ull;
                for (s32 i = 0; i < np; i++) {
                    for (u32 b = 0; b < 8; b++) h = (h ^ r.b[r.r32(g + 44) + 8 * i + b]) * 0x100000001b3ull;
                    h = (h ^ r.b[r.r32(g + 48) + i]) * 0x100000001b3ull;
                }
                for (s32 i = 0; i < nc; i++) h = (h ^ r.r16(r.r32(g + 52) + 2 * i)) * 0x100000001b3ull;
                const u32 env = r.r32(g + 60);
                s += std::format(" points {:#x} env {}", h, fwords(r, env, 20));
                s += std::format(" env words {:#x} {:#x}", r.r32(env + 52), r.r32(env + 68));
                for (const char* q : {"cellFontGlyphGetScalePixel", "cellFontGlyphGetHorizontalShift", "cellFontGlyphGetVerticalShift"}) {
                    std::memset(r.b + v, 0xEE, 8);
                    s += std::format(" {} {:#x} {}", q, r.call("cellFont", q, {g, v, v + 4}), fwords(r, v, 2));
                }
                r.rec(s);
                for (const auto& [sx2, sy2, w, sl] : {std::tuple{24.0f, 24.0f, 1.0f, 0.0f}, std::tuple{40.0f, 20.0f, 1.03f, 0.25f},
                         std::tuple{17.5f, 33.0f, 0.95f, -0.4f}, std::tuple{30.0f, 30.0f, 1.2f, 1.5f}}) {
                    r.w32(st, std::bit_cast<u32>(sx2));
                    r.w32(st + 4, std::bit_cast<u32>(sy2));
                    r.w32(st + 8, std::bit_cast<u32>(w));
                    r.w32(st + 12, std::bit_cast<u32>(sl));
                    for (const char* rn : {"cellFontGlyphRenderImage", "cellFontGlyphRenderImageHorizontal", "cellFontGlyphRenderImageVertical"}) {
                        std::memset(r.b + m, 0xEE, 32);
                        std::memset(r.b + T, 0xEE, 24);
                        const u32 re = r.callf("cellFont", rn, {g, st, renderer, surf, 0, 0, m, T}, {50.5, 60.25});
                        r.rec(std::format("{} {} style {} {} {} {}: {:#x} {} {}", tag, rn, sx2, sy2, w, sl, re, fwords(r, m, 8),
                            trans_text(r, T, surf_buf)));
                    }
                }
                r.rec(std::format("{} delete {:#x}", tag, r.call("cellFont", "cellFontDeleteGlyph", {font, g})));
            } else {
                r.rec(s);
            }
        }
    }
}


// The libfont script: fontsets, a file and memory fonts, their metrics; renderers and images; generated glyphs drawn
// with styles; the open mode; closing. `data` is a font file (SCE-PS3-MT-I-LATIN.TTF).
inline void libfont_script(Run& r, const std::vector<u8>& data)
{
    const FontSetup fs = font_setup(r);
    const u32 lib = fs.lib;
    const u32 type = r.alloc(8);
    const u32 f1 = r.alloc(256), f2 = r.alloc(256), f3 = r.alloc(256), f4 = r.alloc(256);
    r.w32(type, 0); // CELL_FONT_TYPE_RODIN_SANS_SERIF_LATIN
    r.w32(type + 4, 1); // CELL_FONT_MAP_UNICODE
    r.rec(std::format("open fontset {:#x}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f1})));
    r.rec(std::format("open file {:#x}", r.call("cellFont", "cellFontOpenFontFile", {lib, r.str(font("SCE-PS3-YG-R-KOR.TTF")), 0, 5, f2})));
    const u32 blob = r.blob(data);
    r.rec(std::format("open memory {:#x}", r.call("cellFont", "cellFontOpenFontMemory", {lib, blob, static_cast<u32>(data.size()), 0, 6, f3})));
    r.w32(type, 0x100); // CELL_FONT_TYPE_NEWRODIN_GOTHIC_JP_SET
    r.rec(std::format("open jp set {:#x}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f4})));
    r.rec(std::format("open missing {:#x}", r.call("cellFont", "cellFontOpenFontFile", {lib, r.str(font("NOTHING.TTF")), 0, 7, r.alloc(256)})));

    const std::vector<u32> latin = {'A', 'V', 'g', 'W', '.', 0x00E9, 0x3042};
    const std::vector<u32> jp = {0x3042, 0x30A2, 0x4E00, 'A', 0x3001, 0xAC00};
    for (const auto& [f, tag] : {std::pair{f1, "set"}, std::pair{f2, "kor"}, std::pair{f3, "mem"}, std::pair{f4, "jpset"}}) {
        r.rec(std::format("{} scale {:#x}", tag, r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {24.0, 26.0})));
        font_metrics(r, f, tag, f == f2 || f == f4 ? jp : latin);
    }
    r.callf("cellFont", "cellFontSetEffectWeight", {f1, 0}, {1.04});
    r.callf("cellFont", "cellFontSetEffectSlant", {f1, 0}, {0.3});
    font_metrics(r, f1, "set fx", latin);
    r.callf("cellFont", "cellFontSetScalePoint", {f3, 0, 0}, {11.0, 13.5});
    font_metrics(r, f3, "mem pt", latin);
    const u32 pn = r.alloc(8);
    r.rec(std::format("expand info {:#x} {}", r.call("cellFont", "cellFontGetGlyphExpandBufferInfo", {f1, pn, pn + 4}), words(r, pn, 2)));
    r.rec(std::format("adjust {:#x}", r.call("cellFont", "cellFontAdjustGlyphExpandBuffer", {f1, 300, 40})));
    r.rec(std::format("expand info {:#x} {}", r.call("cellFont", "cellFontGetGlyphExpandBufferInfo", {f1, pn, pn + 4}), words(r, pn, 2)));
    r.rec(std::format("adjust default {:#x}", r.call("cellFont", "cellFontAdjustGlyphExpandBuffer", {f2, sx(-1), sx(-1)})));
    r.rec(std::format("expand info {:#x} {}", r.call("cellFont", "cellFontGetGlyphExpandBufferInfo", {f2, pn, pn + 4}), words(r, pn, 2)));

    // renderers
    const u32 rcfg = r.alloc(20), rend = r.alloc(256), rend2 = r.alloc(256);
    r.w32(rcfg, 0);
    r.w32(rcfg + 4, 0x2000);
    r.w32(rcfg + 8, 0x100000);
    r.w32(rcfg + 12, 0);
    r.w32(rcfg + 16, 0);
    r.rec(std::format("create renderer {:#x}", r.call("cellFont", "cellFontCreateRenderer", {lib, rcfg, rend})));
    r.w32(rcfg + 4, 0);
    r.w32(rcfg + 8, 0x800);
    r.rec(std::format("create small renderer {:#x}", r.call("cellFont", "cellFontCreateRenderer", {lib, rcfg, rend2})));
    constexpr u32 SW = 256, SH = 128;
    const u32 sbuf = r.alloc(SW * SH), surf = r.alloc(48);
    r.call("cellFont", "cellFontRenderSurfaceInit", {surf, sbuf, SW, 1, SW, SH});
    r.rec("surface " + words(r, surf, 9));
    for (const auto& [f, tag] : {std::pair{f1, "set"}, std::pair{f2, "kor"}, std::pair{f3, "mem"}}) {
        r.rec(std::format("{} bind {:#x}", tag, r.call("cellFont", "cellFontBindRenderer", {f, rend})));
        r.rec(std::format("{} setup scale {:#x}", tag, r.callf("cellFont", "cellFontSetupRenderScalePixel", {f, 0, 0}, {32.0, 30.0})));
        font_images(r, f, tag, f == f2 ? jp : latin, surf, sbuf);
        r.rec(std::format("{} setup weight {:#x} slant {:#x}", tag,
            r.callf("cellFont", "cellFontSetupRenderEffectWeight", {f, 0}, {1.05}),
            r.callf("cellFont", "cellFontSetupRenderEffectSlant", {f, 0}, {-0.2})));
        font_images(r, f, tag + std::string(" fx"), {'A', 0x3042, 'g'}, surf, sbuf);
        r.rec(std::format("{} setup point {:#x}", tag, r.callf("cellFont", "cellFontSetupRenderScalePoint", {f, 0, 0}, {18.0, 18.0})));
        font_images(r, f, tag + std::string(" pt"), {'W', 0x4E00}, surf, sbuf);
        r.rec(std::format("{} unbind {:#x}", tag, r.call("cellFont", "cellFontUnbindRenderer", {f})));
    }
    r.call("cellFont", "cellFontRenderSurfaceSetScissor", {surf, 20, 10, 100, 60});
    r.rec("scissor " + words(r, surf, 9));
    r.call("cellFont", "cellFontBindRenderer", {f1, rend});
    r.callf("cellFont", "cellFontSetupRenderScalePixel", {f1, 0, 0}, {40.0, 40.0});
    font_images(r, f1, "scissor", {'A', 'g'}, surf, sbuf);
    r.call("cellFont", "cellFontUnbindRenderer", {f1});
    r.rec(std::format("small bind {:#x}", r.call("cellFont", "cellFontBindRenderer", {f1, rend2})));
    r.rec(std::format("small setup {:#x}", r.callf("cellFont", "cellFontSetupRenderScalePixel", {f1, 0, 0}, {64.0, 64.0})));
    font_images(r, f1, "small", {'A'}, surf, sbuf);
    r.call("cellFont", "cellFontUnbindRenderer", {f1});

    // generated glyphs
    r.call("cellFont", "cellFontRenderSurfaceInit", {surf, sbuf, SW, 1, SW, SH});
    r.callf("cellFont", "cellFontSetScalePixel", {f1, 0, 0}, {30.0, 30.0});
    r.callf("cellFont", "cellFontSetEffectWeight", {f1, 0}, {1.0});
    r.callf("cellFont", "cellFontSetEffectSlant", {f1, 0}, {0.0});
    font_glyphs(r, f1, rend, "set", {'A', 'g', 0x00E9}, surf, sbuf);
    r.callf("cellFont", "cellFontSetEffectSlant", {f1, 0}, {0.2});
    font_glyphs(r, f1, rend, "set slant", {'W'}, surf, sbuf);
    r.callf("cellFont", "cellFontSetScalePixel", {f2, 0, 0}, {28.0, 28.0});
    font_glyphs(r, f2, rend, "kor", {0xAC00, 0x3042}, surf, sbuf);

    // the open mode and closing
    r.rec(std::format("open mode {:#x}", r.call("cellFont", "cellFontSetFontOpenMode", {lib, 1})));
    const u32 f5 = r.alloc(256);
    r.rec(std::format("open file no vertical {:#x}", r.call("cellFont", "cellFontOpenFontFile", {lib, r.str(font("SCE-PS3-NR-R-JPN.TTF")), 0, 9, f5})));
    r.callf("cellFont", "cellFontSetScalePixel", {f5, 0, 0}, {20.0, 20.0});
    font_metrics(r, f5, "novert", {0x3042, 'A'});
    r.call("cellFont", "cellFontSetFontOpenMode", {lib, 0});
    for (const u32 f : {f1, f2, f3, f4, f5}) r.rec(std::format("close {:#x}", r.call("cellFont", "cellFontCloseFont", {f})));
    r.rec(std::format("destroy {:#x} {:#x} again {:#x}", r.call("cellFont", "cellFontDestroyRenderer", {rend}),
        r.call("cellFont", "cellFontDestroyRenderer", {rend2}), r.call("cellFont", "cellFontDestroyRenderer", {rend})));
    r.rec(std::format("end library {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontEndLibrary", {lib}),
        r.call("cellFont", "cellFontEndLibrary", {lib}), r.call("cellFont", "cellFontEndLibrary", {lib})));
    r.rec(std::format("end {:#x}", r.call("cellFont", "cellFontEnd", {})));
    r.rec(std::format("memory: {} mallocs, {} frees", r.font_mallocs() != 0, r.font_frees() != 0));
}

} // namespace pspocket::hle::test::ft::fonts
