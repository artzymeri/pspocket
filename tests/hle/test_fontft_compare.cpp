// cellFontFT against libfontFT.sprx (D-526): the same scripts in two fresh processes, one on Sony's libfontFT and
// libfreetype and one on pspocket's cellFontFT and cell_FreeType2, both under Sony's libfont (libfontFT imports its
// static record and settings; libfont reaches libfontFT through the CellFontLibrary's table). The FT layer is called
// directly (managers, face handles, cache streams) and through libfont's API (libraries, fonts, renderers, glyph
// images and outlines); the answers, metrics, pixels and outlines are compared. The fonts are the firmware's own
// (dev_flash/data/font), read at test time. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include "freetype_compare.hpp"

#include <bit>

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::ft;

namespace {

const std::vector<std::string> kUnder = {"cellFont"};
const std::vector<std::string> kHost = {"cellFontFT", "cell_FreeType2"};

std::string font(const std::string& name) { return "/dev_flash/data/font/" + name; }

u64 sx(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

std::string words(Run& r, u32 a, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{}{:#x}", i ? " " : "", r.r32(a + 4 * i));
    return s;
}

// FNV-1a over guest bytes (pixels), rows of `w` bytes `pitch` apart.
u64 fnv(Run& r, u32 a, s32 w, s32 rows, s32 pitch)
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

const std::vector<u32> kCodes = {'A', 'g', 'W', 'j', '1', '.', 0x00C9, 0x3042, 0x4E00, ' ', 0x10FFFF};

// The face of a handle: what libfontFT's getters read.
std::string face_text(Run& r, u32 face)
{
    if (!face) return "face 0";
    return std::format("face glyphs {} flags {:#x} style {:#x} upem {} asc {} desc {} bbox {} {} {} {} adv {} {} name {}/{}",
        r.s32at(face + 16), r.r32(face + 8), r.r32(face + 12), r.r16(face + 68), r.s16at(face + 70), r.s16at(face + 72),
        r.s32at(face + 52), r.s32at(face + 56), r.s32at(face + 60), r.s32at(face + 64), r.s16at(face + 76),
        r.s16at(face + 78), r.cstr(r.r32(face + 20)), r.cstr(r.r32(face + 24)));
}

// A face handle's own fields (not its pointers).
std::string handle_text(Run& r, u32 h)
{
    return std::format("handle flags {} size {:#x} mode {:#x} em {:#x} dpi {} {} weight {:#x} slant {:#x} code {:#x} codes {}",
        r.r32(h + 8), r.r32(h + 16), r.r32(h + 40), r.r32(h + 44), r.r32(h + 48), r.r32(h + 52), r.r32(h + 56),
        r.r32(h + 60), r.r32(h + 64), words(r, h + 68, 4));
}

// Every getter of a face handle.
void getters(Run& r, u32 h, const std::string& tag)
{
    const u32 v = r.alloc(32);
    std::string s = tag;
    for (const char* fn : {"FTFaceH_GetAscender", "FTFaceH_GetBoundingBoxHeight", "FTFaceH_GetBoundingBoxMaxX",
             "FTFaceH_GetBoundingBoxMaxY", "FTFaceH_GetBoundingBoxMinX", "FTFaceH_GetBoundingBoxMinY",
             "FTFaceH_GetBoundingBoxWidth", "FTFaceH_GetMaxHorizontalAdvance", "FTFaceH_GetMaxVerticalAdvance"})
        s += std::format(" {:#x}", r.call("cellFontFT", fn, {h}));
    std::memset(r.b + v, 0xEE, 32);
    s += std::format(" | scale {:#x} {}", r.call("cellFontFT", "FTFaceH_GetRenderScale", {h, v, v + 4}), words(r, v, 2));
    std::memset(r.b + v, 0xEE, 32);
    s += std::format(" pixel {:#x} {}", r.call("cellFontFT", "FTFaceH_GetRenderScalePixel", {h, v, v + 4}), words(r, v, 2));
    std::memset(r.b + v, 0xEE, 32);
    s += std::format(" point {:#x} {}", r.call("cellFontFT", "FTFaceH_GetRenderScalePoint", {h, v, v + 4, v + 8, v + 12}),
        words(r, v, 4));
    std::memset(r.b + v, 0xEE, 32);
    s += std::format(" buffer {:#x} {}", r.call("cellFontFT", "FTFaceH_GetRenderBufferSize", {h, v, v + 4}), words(r, v, 2));
    std::memset(r.b + v, 0xEE, 32);
    s += std::format(" weight {:#x} {}", r.call("cellFontFT", "FTFaceH_GetRenderEffectWeight", {h, v}), words(r, v, 1));
    s += std::format(" slant {:#x} {}", r.call("cellFontFT", "FTFaceH_GetRenderEffectSlant", {h, v}), words(r, v, 1));
    std::memset(r.b + v, 0xEE, 32);
    s += std::format(" comp {:#x} {}", r.call("cellFontFT", "FTFaceH_GetCompositeCodes", {h, v, v + 4}), words(r, v, 2));
    s += " names " + r.cstr(r.call("cellFontFT", "FTFaceH_FontFamilyName", {h})) + "/" +
        r.cstr(r.call("cellFontFT", "FTFaceH_FontStyleName", {h}));
    r.rec(s);
}

// Metrics and an image of each code: the image's place and its pixels.
void glyphs(Run& r, u32 h, const std::string& tag, const std::vector<u32>& codes, bool images)
{
    const u32 m = r.alloc(32), S = r.alloc(32), kerning = r.alloc(8);
    constexpr s32 W = 160, H = 160;
    const u32 buf = r.alloc(W * H);
    for (const u32 c : codes) {
        std::memset(r.b + m, 0xEE, 32);
        const u32 e = r.call("cellFontFT", "FTFaceH_GetGlyphMetrics", {h, c, m});
        r.rec(std::format("{} metrics {:#x} {:#x}: {}", tag, c, e, words(r, m, 8)));
        if (!images) continue;
        for (const auto& [dx, dy] : {std::pair{0, 0}, std::pair{21, 43}, std::pair{-5, 70}}) {
            std::memset(r.b + m, 0xEE, 32);
            std::memset(r.b + buf, 0x55, W * H);
            r.w32(S, buf);
            r.w32(S + 4, m);
            for (u32 i = 2; i < 6; i++) r.w32(S + 4 * i, 0xEEEEEEEE);
            const u32 ie = r.call("cellFontFT", "FTFaceH_GetGlyphImage", {h, c, S, W, H, sx(dx), sx(dy)});
            r.rec(std::format("{} image {:#x} at {} {}: {:#x} place {} metrics {} pixels {:#x}", tag, c, dx, dy, ie,
                words(r, S + 8, 4), words(r, m, 8), fnv(r, buf, W, H, W)));
        }
    }
    for (usize i = 0; i + 1 < codes.size(); i++) {
        std::memset(r.b + kerning, 0xEE, 8);
        const u32 e = r.call("cellFontFT", "FTFaceH_GetKerning", {h, codes[i], codes[i + 1], kerning});
        r.rec(std::format("{} kerning {:#x} {:#x}: {:#x} {}", tag, codes[i], codes[i + 1], e, words(r, kerning, 2)));
    }
    const u32 ab = r.alloc(8);
    for (const char a : {'A', 'V', 'T', 'o'})
        for (const char b2 : {'V', 'A', 'o', '.'}) {
            std::memset(r.b + ab, 0xEE, 8);
            const u32 e = r.call("cellFontFT", "FTFaceH_GetKerning", {h, static_cast<u32>(a), static_cast<u32>(b2), ab});
            r.rec(std::format("{} kerning {}{}: {:#x} {}", tag, a, b2, e, words(r, ab, 2)));
        }
}

// The guest's composite-code function: code -> {code, code + 1, 0}, and the call counted at its arg.
u32 composite_fn(Run& r)
{
    namespace as = pspocket::recomp::ppu::as;
    const u32 code_at = r.alloc(64, 4);
    as::Assembler a(code_at);
    a.emit(as::lwz(6, 0, 3));
    a.emit(as::addi(6, 6, 1));
    a.emit(as::stw(6, 0, 3));
    a.emit(as::stw(4, 0, 5));
    a.emit(as::addi(6, 4, 1));
    a.emit(as::stw(6, 4, 5));
    a.emit(as::li(6, 0));
    a.emit(as::stw(6, 8, 5));
    a.emit(as::blr());
    const std::vector<u8> bytes = a.finish();
    std::memcpy(r.b + code_at, bytes.data(), bytes.size());
    const u32 opd = r.alloc(8);
    r.w32(opd, code_at);
    r.w32(opd + 4, 0);
    return opd;
}

} // namespace

TEST_CASE("cellFontFT: managers, face handles and their getters, as libfontFT", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    const std::vector<u8> data = read_file(*flash + "/data/font/SCE-PS3-RD-R-LATIN.TTF");
    REQUIRE(!data.empty());
    compare(*flash, kUnder, kHost, [&data](Run& r) {
        const u32 mgr = r.alloc(16), mgr2 = r.alloc(16);
        const u32 m1 = r.call("cellFontFT", "FTManager_Init_FreeType", {mgr, 0, kMemory});
        r.rec(std::format("init {} lib {} mgr {}", m1 == mgr, r.r32(mgr) != 0, words(r, mgr + 4, 3)));
        const u32 m2 = r.call("cellFontFT", "FTManager_Init_FreeType", {mgr2, 0, kMemory});
        r.rec(std::format("init2 {} same lib {} mgr {}", m2 == mgr2, r.r32(mgr2) == r.r32(mgr), words(r, mgr2 + 4, 3)));
        const u32 lib = r.r32(mgr);
        r.rec(std::format("library version {}", words(r, lib + 12, 3)));

        const u32 blob = r.blob(data);
        const u32 h1 = r.alloc(256), h2 = r.alloc(256), h3 = r.alloc(256), h4 = r.alloc(256);
        r.rec(std::format("open mem {:#x}", r.call("cellFontFT", "FTManager_OpenMemFace",
                                                {mgr, h1, blob, static_cast<u32>(data.size()), 0})));
        r.rec(handle_text(r, h1) + " " + face_text(r, r.r32(h1)) + " mgr " + words(r, mgr + 4, 3));
        r.rec(std::format("open file {:#x}", r.call("cellFontFT", "FTManager_OpenFileFace",
                                                 {mgr, h2, r.str(font("SCE-PS3-NR-R-JPN.TTF")), 0})));
        r.rec(handle_text(r, h2) + " " + face_text(r, r.r32(h2)));
        r.rec(std::format("open missing {:#x}", r.call("cellFontFT", "FTManager_OpenFileFace",
                                                    {mgr, h3, r.str(font("NOTHING.TTF")), 0})));
        r.rec(std::format("open null handle {:#x}", r.call("cellFontFT", "FTManager_OpenFileFace", {mgr, 0, 0, 0})));
        r.rec(std::format("open bad index {:#x}", r.call("cellFontFT", "FTManager_OpenMemFace",
                                                     {mgr, h3, blob, static_cast<u32>(data.size()), 3})));

        getters(r, h1, "mem");
        getters(r, h2, "jpn");
        getters(r, 0, "null");
        for (const auto& [w, hh] : {std::pair{32u, 32u}, std::pair{13u, 27u}, std::pair{100u, 50u}}) {
            r.rec(std::format("pixel {} {}: {:#x}", w, hh, r.call("cellFontFT", "FTFaceH_SetRenderScalePixel", {h1, w, hh})));
            getters(r, h1, "mem px");
        }
        for (const auto& [cw, ch, dx, dy] : {std::tuple{12 * 64, 12 * 64, 72u, 72u}, std::tuple{20 * 64, 15 * 64, 96u, 0u},
                 std::tuple{9 * 64 + 17, 0, 0u, 300u}}) {
            r.rec(std::format("point: {:#x}",
                r.call("cellFontFT", "FTFaceH_SetRenderScalePoint", {h2, sx(cw), sx(ch), dx, dy})));
            getters(r, h2, "jpn pt");
        }
        for (const s32 w : {0, 0x800, -0x400, 0x2000}) {
            r.rec(std::format("weight {:#x}: {:#x}", w, r.call("cellFontFT", "FTFaceH_SetRenderEffectWeight", {h1, sx(w)})));
            for (const s32 s : {0, 0x4000, -0x3000, 0x20000, -0x20000}) {
                r.rec(std::format("slant {:#x}: {:#x}", s, r.call("cellFontFT", "FTFaceH_SetRenderEffectSlant", {h1, sx(s)})));
                getters(r, h1, "mem fx");
            }
        }
        r.rec(std::format("null slant {:#x} weight {:#x}", r.call("cellFontFT", "FTFaceH_SetRenderEffectSlant", {0, 5}),
            r.call("cellFontFT", "FTFaceH_SetRenderEffectWeight", {0, 5})));

        // streams through a file cache of 6 slots
        const u32 cache = r.alloc(24 + 6 * 8200), cs = r.alloc(64);
        r.w32(cache, cache + 24);
        r.w32(cache + 4, 6);
        r.w32(cache + 8, 0);
        r.w32(cache + 12, 6);
        r.w32(cache + 16, 24 + 6 * 8200);
        r.w32(cache + 20, 0x2000);
        r.rec(std::format("cs init {:#x}", r.call("cellFontFT", "FTCacheStream_Init", {cs, r.str(font("SCE-PS3-SR-R-LATIN2.TTF")), 7})));
        r.rec(std::format("cs size {:#x} id {:#x}", r.r32(cs + 4), r.r32(cs + 40)));
        const u32 n = r.call("cellFontFT", "FTCacheStream_CalcCacheIndexSize", {cs});
        r.rec(std::format("cs index size {:#x} null {:#x}", n, r.call("cellFontFT", "FTCacheStream_CalcCacheIndexSize", {0})));
        const u32 index = r.alloc(n + 16);
        r.rec(std::format("cs cache init {:#x} again {:#x} null {:#x}",
            r.call("cellFontFT", "FTCacheStream_CacheInit", {cs, index, cache}),
            r.call("cellFontFT", "FTCacheStream_CacheInit", {cs, index, cache}),
            r.call("cellFontFT", "FTCacheStream_CacheInit", {cs, 0, cache})));
        r.rec(std::format("open stream {:#x}", r.call("cellFontFT", "FTManager_OpenStreamFace", {mgr, h4, cs, 0})));
        r.rec(handle_text(r, h4) + " " + face_text(r, r.r32(h4)));
        r.rec(std::format("cache {} slots {} {} {} {} {} {}", words(r, cache + 4, 3), words(r, cache + 20, 2),
            words(r, cache + 20 + 8200, 2), words(r, cache + 20 + 2 * 8200, 2), words(r, cache + 20 + 3 * 8200, 2),
            words(r, cache + 20 + 4 * 8200, 2), words(r, cache + 20 + 5 * 8200, 2)));
        r.rec("index " + words(r, index, n / 4));
        getters(r, h4, "stream");
        r.call("cellFontFT", "FTFaceH_SetRenderScalePixel", {h4, 40, 40});
        glyphs(r, h4, "stream", {'A', 'b', 0x0141, 0x0179, '@'}, true);
        r.rec(std::format("cache after {} slots {} {} {} {} {} {}", words(r, cache + 4, 3), words(r, cache + 20, 2),
            words(r, cache + 20 + 8200, 2), words(r, cache + 20 + 2 * 8200, 2), words(r, cache + 20 + 3 * 8200, 2),
            words(r, cache + 20 + 4 * 8200, 2), words(r, cache + 20 + 5 * 8200, 2)));
        const u32 ab = r.alloc(8);
        std::memset(r.b + ab, 0xEE, 8);
        r.rec(std::format("cs cache end {:#x} {} same {} {}", r.call("cellFontFT", "FTCacheStream_CacheEnd", {cs, ab, ab + 4}),
            r.r32(ab) == index, r.r32(ab + 4) == cache, words(r, cs + 44, 2)));
        r.rec(std::format("cs end {:#x} null {:#x}", r.call("cellFontFT", "FTCacheStream_End", {cs}),
            r.call("cellFontFT", "FTCacheStream_End", {0})));

        // glyphs, images and kerning of the handles, with effects and composite codes
        r.call("cellFontFT", "FTFaceH_SetRenderEffectWeight", {h1, 0});
        r.call("cellFontFT", "FTFaceH_SetRenderEffectSlant", {h1, 0});
        r.call("cellFontFT", "FTFaceH_SetRenderScalePixel", {h1, 48, 48});
        glyphs(r, h1, "plain", kCodes, true);
        r.call("cellFontFT", "FTFaceH_SetRenderEffectWeight", {h1, 0x1000});
        glyphs(r, h1, "bold", {'A', 'g', 'W', '.'}, true);
        r.call("cellFontFT", "FTFaceH_SetRenderEffectSlant", {h1, 0x5000});
        glyphs(r, h1, "bold slant", {'A', 'g', 'W', '.'}, true);
        r.call("cellFontFT", "FTFaceH_SetRenderEffectWeight", {h1, sx(-0x800)});
        r.call("cellFontFT", "FTFaceH_SetRenderEffectSlant", {h1, sx(-0x6000)});
        glyphs(r, h1, "thin back", {'A', 'g', 'W', '.'}, true);
        r.call("cellFontFT", "FTFaceH_SetRenderEffectWeight", {h1, 0});
        r.call("cellFontFT", "FTFaceH_SetRenderEffectSlant", {h1, 0});
        const u32 comp = composite_fn(r), counter = r.alloc(4);
        r.rec(std::format("composite set {:#x} {:#x} {:#x}", r.call("cellFontFT", "FTFaceH_SetCompositeCodes", {h1, comp, counter}),
            r.call("cellFontFT", "FTFaceH_SetCompositeCodes", {h1, 0, counter}),
            r.call("cellFontFT", "FTFaceH_SetCompositeCodes", {0, comp, counter})));
        glyphs(r, h1, "composite", {'A', 'f', 'l'}, true);
        r.rec(std::format("composite calls {} handle {}", r.r32(counter), handle_text(r, h1)));
        r.call("cellFontFT", "FTFaceH_SetRenderScalePoint", {h2, 30 * 64, 30 * 64, 72, 72});
        glyphs(r, h2, "jpn", {0x3042, 0x30A2, 0x4E00, 0x6F22, 'A', 0xFF21, 0x3001}, true);
        r.rec(std::format("null image {:#x} {:#x}", r.call("cellFontFT", "FTFaceH_GetGlyphImage", {0, 'A', 0, 0, 0, 0, 0}),
            r.call("cellFontFT", "FTFaceH_GetGlyphImage", {h1, 'A', 0, 0, 0, 0, 0})));

        // the open mode: vertical metrics of the faces opened after it
        r.rec(std::format("open mode {:#x}", r.call("cellFontFT", "FTManager_SetFontOpenMode", {mgr, 0x01000000})));
        const u32 h5 = r.alloc(256);
        r.rec(std::format("open file ignoring vertical {:#x}", r.call("cellFontFT", "FTManager_OpenFileFace",
                                                                {mgr, h5, r.str(font("SCE-PS3-NR-R-JPN.TTF")), 0})));
        r.rec(handle_text(r, h5) + " " + face_text(r, r.r32(h5)));
        r.call("cellFontFT", "FTManager_SetFontOpenMode", {mgr, 0});

        // closing
        const u32 tw = r.alloc(8);
        for (const u32 h : {h1, h2, h4, h5}) {
            std::memset(r.b + tw, 0xEE, 8);
            const u32 e = h == h4 ? r.call("cellFontFT", "FTFaceH_Close", {h, tw, tw + 4})
                                  : r.call("cellFontFT", "FTManager_CloseFace", {mgr, h, tw, tw + 4});
            r.rec(std::format("close {:#x} type {} what {}", e, r.r32(tw),
                r.r32(tw + 4) == blob ? "blob" : r.r32(tw + 4) == cs ? "stream" : r.r32(tw + 4) ? r.cstr(r.r32(tw + 4)) : "0"));
        }
        r.rec("mgr " + words(r, mgr + 4, 3));
        r.rec(std::format("done2 {:#x} mgr {}", r.call("cellFontFT", "FTManager_Done_FreeType", {mgr2}), words(r, mgr2, 4)));
        r.rec(std::format("done {:#x} mgr {}", r.call("cellFontFT", "FTManager_Done_FreeType", {mgr}), words(r, mgr, 4)));
        r.rec(std::format("memory {}", r.heap_ops(0).empty() ? "none" : "used"));
    }, "FT layer");
}

namespace {

std::string fwords(Run& r, u32 a, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{}{}", i ? " " : "", std::bit_cast<float>(r.r32(a + 4 * i)));
    return s;
}

// libfont's set-up: the file cache, the library of FreeType fonts over the script's memory interface.
struct FontSetup {
    u32 lib = 0, cache = 0;
};

FontSetup font_setup(Run& r, u32 cache_slots = 8)
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

std::string trans_text(Run& r, u32 T, u32 surf_buf)
{
    const u32 image = r.r32(T), iwb = r.r32(T + 4), iw = r.r32(T + 8), ih = r.r32(T + 12), surf = r.r32(T + 16);
    return std::format("trans wb {} {}x{} surface {:+} wb {} pixels {:#x}", iwb, iw, ih,
        static_cast<s64>(surf) - static_cast<s64>(surf_buf), r.r32(T + 20),
        image ? fnv(r, image, static_cast<s32>(iw), static_cast<s32>(ih), static_cast<s32>(iwb)) : 0);
}

// A font's metrics, layouts and kerning through libfont.
void font_metrics(Run& r, u32 font, const std::string& tag, const std::vector<u32>& codes)
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
void font_images(Run& r, u32 font, const std::string& tag, const std::vector<u32>& codes, u32 surf, u32 surf_buf)
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
void font_glyphs(Run& r, u32 font, u32 renderer, const std::string& tag, const std::vector<u32>& codes, u32 surf, u32 surf_buf)
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

} // namespace

TEST_CASE("cellFontFT: libfont's fonts, renderers and glyphs over libfontFT", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    const std::vector<u8> data = read_file(*flash + "/data/font/SCE-PS3-MT-I-LATIN.TTF");
    REQUIRE(!data.empty());
    compare(*flash, kUnder, kHost, [&data](Run& r) {
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
    }, "libfont over libfontFT");
}
