// cellFontFT against libfontFT.sprx (D-526): the same scripts in two fresh processes, one on Sony's libfontFT and
// libfreetype and one on pspocket's cellFontFT and cell_FreeType2, both under Sony's libfont (libfontFT imports its
// static record and settings; libfont reaches libfontFT through the CellFontLibrary's table). The FT layer is called
// directly (managers, face handles, cache streams) and through libfont's API (libraries, fonts, renderers, glyph
// images and outlines); the answers, metrics, pixels and outlines are compared. The fonts are the firmware's own
// (dev_flash/data/font), read at test time. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include "font_scripts.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::ft;
using namespace pspocket::hle::test::ft::fonts;

namespace {

const std::vector<std::string> kUnder = {"cellFont"};
const std::vector<std::string> kHost = {"cellFontFT", "cell_FreeType2"};

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

TEST_CASE("cellFontFT: libfont's fonts, renderers and glyphs over libfontFT", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    const std::vector<u8> data = read_file(*flash + "/data/font/SCE-PS3-MT-I-LATIN.TTF");
    REQUIRE(!data.empty());
    compare(*flash, kUnder, kHost, [&data](Run& r) { libfont_script(r, data); }, "libfont over libfontFT");
}
