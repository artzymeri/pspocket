// cellFont against libfont.sprx (D-526): the same scripts in two fresh processes, one on Sony's libfont, libfontFT and
// libfreetype and one on pspocket's cellFont, cellFontFT and cell_FreeType2. libfont's API is called as games call it
// (initialization, settings, fontsets of every kind on files and in memory, file fonts and their instances, renderers,
// metrics, layouts, images, generated glyphs, outlines' vertexes and vertexes glyphs, draw contexts, extensions,
// deletion, the patch works); the answers, metrics, pixels and vertexes are compared. The fonts are the firmware's
// own (dev_flash/data/font), read at test time; some outlines are made here (cubic ones: the firmware's fonts are all
// TrueType). Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include "font_scripts.hpp"

#include <array>
#include <unistd.h>

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::ft;
using namespace pspocket::hle::test::ft::fonts;

namespace {

const std::vector<std::string> kAll = {"cellFont", "cellFontFT", "cell_FreeType2"};

u32 fbits(float f) { return std::bit_cast<u32>(f); }

// libfont initialized over a cache of `slots` blocks and `entries` user font entries: the answer.
u32 initialize(Run& r, u32 slots, u32 entries)
{
    const u32 cfg = r.alloc(32), e = r.alloc(16 * entries), cache = r.alloc(24 + slots * 0x2008);
    r.w32(cfg, cache);
    r.w32(cfg + 4, 24 + slots * 0x2008);
    r.w32(cfg + 8, entries);
    r.w32(cfg + 12, e);
    return r.call("cellFont", "cellFontInitializeWithRevision", {0x7F, cfg});
}

// A library of FreeType fonts over the script's memory interface.
u32 ft_library(Run& r)
{
    const u32 ftcfg = r.alloc(32), out = r.alloc(8), iface = r.font_memory();
    for (u32 i = 0; i < 5; i++) r.w32(ftcfg + 4 + 4 * i, r.r32(iface + 4 * i));
    r.rec(std::format("init library {:#x}", r.call("cellFontFT", "cellFontInitLibraryFreeType", {ftcfg, out})));
    return r.r32(out);
}

// A font's state: magic, kind, fonts, ID; a fontset's type, map and font IDs.
std::string font_text(Run& r, u32 f)
{
    std::string s = std::format("magic {:#x} kind {} fonts {} all {} id {:#x}", r.r16(f), r.r16(f + 2), r.r32(f + 0x38),
        r.r32(f + 0x3C), r.r32(f + 8));
    if (!r.r16(f + 2) && r.r32(f + 0x28)) {
        s += std::format(" set {:#x} {:#x} {} ids", r.r32(f + 0x18), r.r32(f + 0x1C), r.r32(f + 0x24));
        for (u32 i = 0; i < r.r32(f + 0x24) && i < 16; i++) s += std::format(" {}", r.r32(r.r32(f + 0x28) + 4 * i));
    }
    return s;
}

// The user font entries {state, unique ID, library, face handle}: the state, the ID, whose library, whether a face.
std::string entries_text(Run& r, u32 entries, u32 n, u32 lib)
{
    std::string s;
    for (u32 i = 0; i < n; i++) {
        const u32 e = entries + 16 * i;
        s += std::format(" [{:#x} {:#x} {} {}]", r.r32(e), r.r32(e + 4), r.r32(e + 8) == lib ? "lib" : r.r32(e + 8) ? "other" : "0",
            r.r32(e + 12) != 0);
    }
    return s;
}

// Which font draws each code, as which code, and its metrics.
void font_codes(Run& r, u32 f, const std::string& tag, const std::vector<u32>& codes)
{
    const u32 id = r.alloc(8), m = r.alloc(32);
    for (const u32 c : codes) {
        std::memset(r.b + id, 0xEE, 8);
        std::memset(r.b + m, 0xEE, 32);
        const u32 e = r.call("cellFont", "cellFontGetFontIdCode", {f, c, id, id + 4});
        r.rec(std::format("{} {:#x}: id {:#x} {} metrics {:#x} {}", tag, c, e, words(r, id, 2),
            r.call("cellFont", "cellFontGetCharGlyphMetrics", {f, c, m}), fwords(r, m, 8)));
    }
}

const std::vector<u32> kSetCodes = {'A', 'g', 0x00E9, 0x0416, 0x03A9, 0x3042, 0x30A2, 0x4E00, 0xAC00, 0x20AC, 0x2460,
    0xFF21, 0x3001, 0xE000, 0x1F600, 0};

// Guest code: a vertex callback appending {contour, index, kind, x, y} at its arg's {next, count}; a graphics start
// writing its config to *out.
struct Guest {
    u32 vertex = 0, graphics_init = 0;
};

Guest guest_code(Run& r)
{
    namespace as = pspocket::recomp::ppu::as;
    const u32 code = r.alloc(128, 4);
    as::Assembler a(code);
    const u32 vertex_at = a.pc();
    a.emit(as::lwz(7, 0, 3));
    a.emit(as::stw(4, 0, 7));
    a.emit(as::stw(5, 4, 7));
    a.emit(as::stw(6, 8, 7));
    a.emit(as::stfs(1, 12, 7));
    a.emit(as::stfs(2, 16, 7));
    a.emit(as::addi(7, 7, 20));
    a.emit(as::stw(7, 0, 3));
    a.emit(as::lwz(8, 4, 3));
    a.emit(as::addi(8, 8, 1));
    a.emit(as::stw(8, 4, 3));
    a.emit(as::blr());
    const u32 init_at = a.pc();
    a.emit(as::stw(3, 0, 4));
    a.emit(as::li(3, 0));
    a.emit(as::blr());
    const std::vector<u8> bytes = a.finish();
    std::memcpy(r.b + code, bytes.data(), bytes.size());
    const u32 opds = r.alloc(16);
    r.w32(opds, vertex_at);
    r.w32(opds + 8, init_at);
    return {opds, opds + 8};
}

} // namespace

TEST_CASE("cellFont: libfont's fonts, renderers and glyphs, as libfont", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    const std::vector<u8> data = read_file(*flash + "/data/font/SCE-PS3-MT-I-LATIN.TTF");
    REQUIRE(!data.empty());
    compare(*flash, {}, kAll, [&data](Run& r) { libfont_script(r, data); }, "libfont");
}

TEST_CASE("cellFont: initialization, settings, fontsets, file fonts and instances, as libfont", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, {}, kAll, [](Run& r) {
        const u32 flags = r.alloc(8), any = r.alloc(256);
        ps_write64(r.b, flags, 0xEEEEEEEEEEEEEEEEull);
        r.call("cellFont", "cellFontGetRevisionFlags", {flags});
        r.call("cellFont", "cellFontGetRevisionFlags", {0});
        r.rec(std::format("revision flags {:#x}", ps_read64(r.b, flags)));
        ps_write64(r.b, flags, 0xEEEEEEEEEEEEEEEEull);
        r.rec(std::format("uninitialized: flags {:#x} {:#x}", r.call("cellFont", "cellFontGetInitializedRevisionFlags", {flags}),
            ps_read64(r.b, flags)));
        r.rec(std::format("uninitialized: static {} usleep {:#x} end {:#x} clear {:#x} end library {:#x}",
            r.call("cellFont", "cellFontStatic", {}) != 0, r.call("cellFont", "cellFontsetUSleep", {}),
            r.call("cellFont", "cellFontEnd", {}), r.call("cellFont", "cellFontClearFileCache", {}),
            r.call("cellFont", "cellFontEndLibrary", {any})));
        r.rec(std::format("uninitialized: open {:#x} {:#x} close {:#x} open mode {:#x} {:#x}",
            r.call("cellFont", "cellFontOpenFontset", {any, any, r.alloc(256)}),
            r.call("cellFont", "cellFontOpenFontFile", {any, r.str("x"), 0, 0, r.alloc(256)}),
            r.call("cellFont", "cellFontCloseFont", {r.alloc(256)}), r.call("cellFont", "cellFontSetFontOpenMode", {any, 1}),
            r.call("cellFont", "cellFontSetFontsetOpenMode", {any, 1})));

        // the settings' checks
        const u32 cfg = r.alloc(32), entries = r.alloc(16 * 3), cache = r.alloc(24 + 4 * 0x2008);
        r.w32(cfg, cache);
        r.w32(cfg + 4, 0x10);
        r.w32(cfg + 8, 3);
        r.w32(cfg + 12, entries);
        r.rec(std::format("init null {:#x} small cache {:#x}", r.call("cellFont", "cellFontInitializeWithRevision", {0x7F, 0}),
            r.call("cellFont", "cellFontInitializeWithRevision", {0x7F, cfg})));
        r.w32(cfg, 0);
        r.w32(cfg + 4, 24 + 4 * 0x2008);
        r.rec(std::format("init no cache {:#x}", r.call("cellFont", "cellFontInitializeWithRevision", {0x7F, cfg})));
        r.w32(cfg, cache);
        std::memset(r.b + entries, 0xEE, 16 * 3);
        r.rec(std::format("init {:#x}", r.call("cellFont", "cellFontInitializeWithRevision", {0x1234567, cfg})));
        r.rec(std::format("cache {} {} entries {}", r.r32(cache) == cache + 24, words(r, cache + 4, 5), words(r, entries, 12)));
        r.rec(std::format("init again {:#x} {:#x}", r.call("cellFont", "cellFontInitializeWithRevision", {0x7F, cfg}),
            r.call("cellFont", "cellFont_57E03823", {0x7F, cfg})));
        ps_write64(r.b, flags, 0);
        r.rec(std::format("initialized: flags {:#x} {:#x} static {} usleep {:#x}",
            r.call("cellFont", "cellFontGetInitializedRevisionFlags", {flags}), ps_read64(r.b, flags),
            r.call("cellFont", "cellFontStatic", {}) != 0, r.call("cellFont", "cellFontsetUSleep", {})));
        const u32 h = r.alloc(8), h2 = r.alloc(8), zero = r.alloc(8);
        r.w32(h, 0x1234);
        r.w32(h + 4, 0x5678);
        r.rec(std::format("control {:#x} {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontControl", {0x540002, h}),
            r.call("cellFont", "cellFontControl", {0x540003, h2}), r.call("cellFont", "cellFontControl", {0x540003, 0}),
            r.call("cellFont", "cellFontControl", {0x540002, 0})));
        r.rec("handler " + words(r, h2, 2));
        r.rec(std::format("control {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontControl", {0x540002, zero}),
            r.call("cellFont", "cellFontControl", {0x540100, 0}), r.call("cellFont", "cellFontControl", {0x12345, 0})));

        const u32 lib = ft_library(r);
        r.rec(std::format("library {} {:#x}", lib != 0, r.r32(lib)));

        // fontsets of every kind, by Unicode and by the first of their type
        const u32 type = r.alloc(8), f = r.alloc(256);
        for (const auto& [t, map] : {std::pair{0x0u, 1u}, std::pair{0x18u, 1u}, std::pair{0x20u, 1u}, std::pair{0x8u, 1u},
                 std::pair{0xCu, 1u}, std::pair{0x40u, 1u}, std::pair{0x48u, 1u}, std::pair{0x100u, 1u},
                 std::pair{0x1000100u, 1u}, std::pair{0x101u, 1u}, std::pair{0x204u, 1u}, std::pair{0x10Au, 1u},
                 std::pair{0x300105u, 1u}, std::pair{0x300229u, 1u}, std::pair{0x40100u, 1u}, std::pair{0x0u, 0u},
                 std::pair{0x12345u, 1u}, std::pair{0x1u, 7u}}) {
            r.w32(type, t);
            r.w32(type + 4, map);
            std::memset(r.b + f, 0, 256);
            const u32 e = r.call("cellFont", "cellFontOpenFontset", {lib, type, f});
            const std::string tag = std::format("set {:#x}/{}", t, map);
            r.rec(std::format("{} open {:#x} {}", tag, e, font_text(r, f)));
            if (e) continue;
            r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {20.0, 22.0});
            font_codes(r, f, tag, kSetCodes);
            r.rec(std::format("{} close {:#x}", tag, r.call("cellFont", "cellFontCloseFont", {f})));
        }
        r.w32(type, 0);
        r.w32(type + 4, 1);
        r.rec(std::format("null args {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontOpenFontset", {0, type, f}),
            r.call("cellFont", "cellFontOpenFontset", {lib, 0, f}), r.call("cellFont", "cellFontOpenFontset", {lib, type, 0})));

        // the fontsets' open mode: kept while no fontset of the library is open
        const u32 f2 = r.alloc(256), m = r.alloc(32);
        r.w32(type, 0x8);
        r.rec(std::format("fontset open mode {:#x} open {:#x}", r.call("cellFont", "cellFontSetFontsetOpenMode", {lib, 1}),
            r.call("cellFont", "cellFontOpenFontset", {lib, type, f})));
        r.rec(std::format("fontset open mode while open {:#x} null {:#x}", r.call("cellFont", "cellFontSetFontsetOpenMode", {lib, 0}),
            r.call("cellFont", "cellFontSetFontsetOpenMode", {0, 0})));
        r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {20.0, 20.0});
        for (const u32 c : {0x3042u, 0x3001u, 0x41u}) {
            std::memset(r.b + m, 0xEE, 32);
            r.rec(std::format("ignoring vertical {:#x}: {:#x} {}", c, r.call("cellFont", "cellFontGetCharGlyphMetricsVertical", {f, c, m}),
                fwords(r, m, 8)));
        }
        r.w32(type, 0x0);
        r.rec(std::format("second fontset {:#x} {}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f2}), font_text(r, f2)));
        r.rec(std::format("close {:#x} {:#x} mode {:#x}", r.call("cellFont", "cellFontCloseFont", {f}),
            r.call("cellFont", "cellFontCloseFont", {f2}), r.call("cellFont", "cellFontSetFontsetOpenMode", {lib, 0})));

        // fontsets on memory, and instances
        const u32 f3 = r.alloc(256), f4 = r.alloc(256);
        r.rec(std::format("on memory {:#x} {}", r.call("cellFont", "cellFontOpenFontsetOnMemory", {lib, type, f3}), font_text(r, f3)));
        r.callf("cellFont", "cellFontSetScalePixel", {f3, 0, 0}, {18.0, 18.0});
        font_codes(r, f3, "memory", {'A', 'g', 0x3042});
        r.rec(std::format("instance {:#x} {}", r.call("cellFont", "cellFontOpenFontInstance", {f3, f4}), font_text(r, f4)));
        r.rec(std::format("instance null {:#x} {:#x}", r.call("cellFont", "cellFontOpenFontInstance", {0, r.alloc(256)}),
            r.call("cellFont", "cellFontOpenFontInstance", {f3, 0})));
        font_codes(r, f4, "instance", {'A', 0x00E9});
        r.rec(std::format("close memory {:#x}", r.call("cellFont", "cellFontCloseFont", {f3})));
        font_codes(r, f4, "instance alone", {'A'});
        r.rec(std::format("close instance {:#x} again {:#x}", r.call("cellFont", "cellFontCloseFont", {f4}),
            r.call("cellFont", "cellFontCloseFont", {f4})));
        r.w32(type, 0x300105);
        r.rec(std::format("on memory capie {:#x} {}", r.call("cellFont", "cellFontOpenFontsetOnMemory", {lib, type, f3}), font_text(r, f3)));
        r.callf("cellFont", "cellFontSetScalePixel", {f3, 0, 0}, {26.0, 24.0});
        font_codes(r, f3, "capie", kSetCodes);
        r.rec(std::format("close capie {:#x}", r.call("cellFont", "cellFontCloseFont", {f3})));

        // a font directory of the game's
        r.rec(std::format("directory {:#x}", r.call("cellFont", "cellFontControl", {0x540000, r.str("/dev_flash/data/font/")})));
        r.w32(type, 0x20);
        r.rec(std::format("set from directory {:#x} {}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f}), font_text(r, f)));
        r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {16.0, 16.0});
        font_codes(r, f, "directory", {'A', 'Q'});
        r.rec(std::format("close {:#x}", r.call("cellFont", "cellFontCloseFont", {f})));
        r.rec(std::format("empty directory {:#x}", r.call("cellFont", "cellFontControl", {0x540000, r.str("")})));
        r.rec(std::format("set from nowhere {:#x}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f})));
        r.rec(std::format("no directory {:#x}", r.call("cellFont", "cellFontControl", {0x540000, 0})));

        // file fonts: shared entries, the table's limit, instances
        const u32 g1 = r.alloc(256), g2 = r.alloc(256), g3 = r.alloc(256), g4 = r.alloc(256), g5 = r.alloc(256),
                  g6 = r.alloc(256);
        const u32 latin = r.str(font("SCE-PS3-RD-R-LATIN.TTF")), kor = r.str(font("SCE-PS3-YG-R-KOR.TTF"));
        r.rec(std::format("file {:#x} {}", r.call("cellFont", "cellFontOpenFontFile", {lib, latin, 0, 5, g1}), font_text(r, g1)));
        r.rec(std::format("same {:#x} {}", r.call("cellFont", "cellFontOpenFontFile", {lib, latin, 0, 5, g2}), font_text(r, g2)));
        r.rec(std::format("other {:#x} {}", r.call("cellFont", "cellFontOpenFontFile", {lib, kor, 0, 6, g3}), font_text(r, g3)));
        r.rec(std::format("file bad id {:#x} null path {:#x} null lib {:#x}",
            r.call("cellFont", "cellFontOpenFontFile", {lib, latin, 0, sx(-1), g4}), r.call("cellFont", "cellFontOpenFontFile", {lib, 0, 0, 1, g4}),
            r.call("cellFont", "cellFontOpenFontFile", {0, latin, 0, 1, g4})));
        r.rec(std::format("file missing {:#x}", r.call("cellFont", "cellFontOpenFontFile", {lib, r.str(font("NONE.TTF")), 0, 7, g4})));
        r.rec("entries" + entries_text(r, entries, 3, lib));
        r.rec(std::format("file third {:#x} table full {:#x}", r.call("cellFont", "cellFontOpenFontFile", {lib, kor, 0, 8, g4}),
            r.call("cellFont", "cellFontOpenFontFile", {lib, kor, 0, 9, g5})));
        r.rec(std::format("instance {:#x} {}", r.call("cellFont", "cellFontOpenFontInstance", {g1, g6}), font_text(r, g6)));
        const u32 lp = r.alloc(8);
        std::memset(r.b + lp, 0xEE, 8);
        r.rec(std::format("library of font {:#x} same {} type {:#x}", r.call("cellFont", "cellFontGetLibrary", {g6, lp, lp + 4}),
            r.r32(lp) == lib, r.r32(lp + 4)));
        r.rec(std::format("library null {:#x} {:#x}", r.call("cellFont", "cellFontGetLibrary", {0, lp, 0}),
            r.call("cellFont", "cellFontGetLibrary", {g6, 0, 0})));
        r.callf("cellFont", "cellFontSetScalePixel", {g1, 0, 0}, {20.0, 20.0});
        font_codes(r, g1, "file", {'A', 0x3042});
        r.rec(std::format("scaling {:#x} {:#x} {:#x} {:#x} set {:#x}", r.callf("cellFont", "cellFontAdjustFontScaling", {g1, 0}, {1.5}),
            r.callf("cellFont", "cellFontAdjustFontScaling", {g1, 0}, {3.0}), r.callf("cellFont", "cellFontAdjustFontScaling", {g1, 0}, {0.25}),
            r.callf("cellFont", "cellFontAdjustFontScaling", {0, 0}, {1.0}), r.callf("cellFont", "cellFontAdjustFontScaling", {f, 0}, {1.0})));
        font_codes(r, g1, "file scaled", {'A', 'g'});
        const u32 v = r.alloc(16);
        r.rec(std::format("dpi {:#x} {:#x}", r.call("cellFont", "cellFontSetResolutionDpi", {g1, 96, 0}),
                  r.call("cellFont", "cellFontGetResolutionDpi", {g1, v, v + 4})) + " " + words(r, v, 2));
        r.rec(std::format("scale point {:#x} {:#x} {} pixel {:#x} {}", r.callf("cellFont", "cellFontSetScalePoint", {g1, 0, 0}, {12.0, 14.5}),
            r.call("cellFont", "cellFontGetScalePoint", {g1, v, v + 4}), fwords(r, v, 2),
            r.call("cellFont", "cellFontGetScalePixel", {g1, v + 8, v + 12}), fwords(r, v + 8, 2)));
        font_codes(r, g1, "file points", {'W'});
        r.rec(std::format("scale pixel {:#x} {:#x} {} pixel {:#x} {}", r.callf("cellFont", "cellFontSetScalePixel", {g1, 0, 0}, {33.0, 0.0}),
            r.call("cellFont", "cellFontGetScalePoint", {g1, v, v + 4}), fwords(r, v, 2),
            r.call("cellFont", "cellFontGetScalePixel", {g1, v + 8, 0}), fwords(r, v + 8, 2)));
        for (const auto& [w, sl] : {std::pair{2.0, 3.0}, std::pair{0.5, -0.25}, std::pair{1.02, -7.0}, std::pair{0.94, 0.5}}) {
            std::memset(r.b + v, 0xEE, 16);
            r.rec(std::format("effects {} {}: {:#x} {:#x} {:#x} {:#x} {}", w, sl, r.callf("cellFont", "cellFontSetEffectWeight", {g1, 0}, {w}),
                r.callf("cellFont", "cellFontSetEffectSlant", {g1, 0}, {sl}), r.call("cellFont", "cellFontGetEffectWeight", {g1, v}),
                r.call("cellFont", "cellFontGetEffectSlant", {g1, v + 4}), fwords(r, v, 2)));
            font_codes(r, g1, "effects", {'A'});
        }
        r.rec(std::format("null getters {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontGetEffectWeight", {g1, 0}),
            r.call("cellFont", "cellFontGetEffectSlant", {0, v}), r.call("cellFont", "cellFontGetScalePoint", {0, v, v}),
            r.call("cellFont", "cellFontGetScalePixel", {0, v, v}), r.call("cellFont", "cellFontGetResolutionDpi", {0, v, v}),
            r.call("cellFont", "cellFontSetResolutionDpi", {0, 1, 1}), r.callf("cellFont", "cellFontSetScalePoint", {0, 0, 0}, {1.0, 1.0}),
            r.callf("cellFont", "cellFontSetEffectSlant", {0, 0}, {1.0})));
        std::memset(r.b + v, 0xEE, 16);
        r.rec(std::format("id code {:#x} {}", r.call("cellFont", "cellFontGetFontIdCode", {g3, 0xAC00, v, v + 4}), words(r, v, 2)));
        r.rec(std::format("id code null {:#x} {}", r.call("cellFont", "cellFontGetFontIdCode", {0, 0xAC00, v + 8, v + 12}), words(r, v + 8, 2)));
        {
            std::string c = "cache " + words(r, cache + 4, 4) + " slots";
            for (u32 i = 0; i < 4; i++) c += " " + words(r, cache + 20 + 0x2008 * i, 2);
            r.rec(c);
        }
        for (const u32 g : {g2, g1, g6, g1, g3, g4, g5})
            r.rec(std::format("close {:#x} entries{}", r.call("cellFont", "cellFontCloseFont", {g}), entries_text(r, entries, 3, lib)));
        r.rec(std::format("close null {:#x}", r.call("cellFont", "cellFontCloseFont", {0})));

        // the end
        r.rec(std::format("clear cache {:#x}", r.call("cellFont", "cellFontClearFileCache", {})));
        r.rec(std::format("end library {:#x}", r.call("cellFont", "cellFontEndLibrary", {lib})));
        r.rec(std::format("end library again {:#x}", r.call("cellFont", "cellFontEndLibrary", {lib})));
        r.rec(std::format("end {:#x} cache {}", r.call("cellFont", "cellFontEnd", {}), words(r, cache, 6)));
        r.rec(std::format("end again {:#x}", r.call("cellFont", "cellFontEnd", {})));
        ps_write64(r.b, flags, 0xEE);
        r.rec(std::format("after end {:#x} {:#x}", r.call("cellFont", "cellFontGetInitializedRevisionFlags", {flags}), ps_read64(r.b, flags)));
        r.rec(std::format("init without tables {:#x}", r.call("cellFont", "cellFont_57E03823", {0x55, cfg})));
        ps_write64(r.b, flags, 0xEE);
        r.rec(std::format("flags {:#x} {:#x}", r.call("cellFont", "cellFontGetInitializedRevisionFlags", {flags}), ps_read64(r.b, flags)));
        r.rec(std::format("end {:#x}", r.call("cellFont", "cellFontEnd", {})));
    }, "settings");
}

TEST_CASE("cellFont: draw contexts, extensions, graphics, deletion and the patch works, as libfont", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, {}, kAll, [](Run& r) {
        const Guest code = guest_code(r);
        r.rec(std::format("initialize {:#x}", initialize(r, 8, 4)));
        const u32 lib = ft_library(r);

        // draw contexts
        const u32 dc = r.alloc(0x100), v = r.alloc(32), rgba = r.alloc(16);
        std::memset(r.b + dc, 0xEE, 0x100);
        r.rec(std::format("draw context {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontGraphicsSetupDrawContext", {lib, dc}),
            r.call("cellFont", "cellFontGraphicsSetupDrawContext", {0, dc}), r.call("cellFont", "cellFontGraphicsSetupDrawContext", {lib, 0})));
        r.rec("context " + words(r, dc, 64));
        r.rec(std::format("draw type {:#x} {:#x} {} bad {:#x} {:#x}", r.call("cellFont", "cellFontGraphicsSetDrawType", {dc, 0x12345}),
            r.call("cellFont", "cellFontGraphicsGetDrawType", {dc, v}), words(r, v, 1),
            r.call("cellFont", "cellFontGraphicsSetDrawType", {v, 1}), r.call("cellFont", "cellFontGraphicsGetDrawType", {dc, 0})));
        for (u32 i = 0; i < 4; i++) r.w32(rgba + 4 * i, fbits(0.25f * static_cast<float>(i) + 0.1f));
        r.rec(std::format("font rgba {:#x} {:#x} {}", r.call("cellFont", "cellFontGraphicsSetFontRGBA", {dc, rgba}),
            r.call("cellFont", "cellFontGraphicsGetFontRGBA", {dc, v}), fwords(r, v, 4)));
        r.rec(std::format("line rgba {:#x} {:#x} {} bad {:#x} {:#x}", r.call("cellFont", "cellFontGraphicsSetLineRGBA", {dc, rgba + 4}),
            r.call("cellFont", "cellFontGraphicsGetLineRGBA", {dc, v + 16}), fwords(r, v + 16, 4),
            r.call("cellFont", "cellFontGraphicsSetFontRGBA", {dc, 0}), r.call("cellFont", "cellFontGraphicsGetLineRGBA", {rgba, v})));
        std::memset(r.b + v, 0xEE, 32);
        r.rec(std::format("scale {:#x} {:#x} {} bad {:#x} {:#x} {}", r.callf("cellFont", "cellFontGraphicsSetScalePixel", {dc, 0, 0}, {24.0, 13.5}),
            r.call("cellFont", "cellFontGraphicsGetScalePixel", {dc, v, v + 4}), fwords(r, v, 2),
            r.callf("cellFont", "cellFontGraphicsSetScalePixel", {v, 0, 0}, {1.0, 1.0}),
            r.call("cellFont", "cellFontGraphicsGetScalePixel", {v + 8, v + 12, v + 16}), words(r, v + 12, 2)));
        r.rec(std::format("flags {:#x} {:#x} {:#x}", r.call("cellFont", "cellFont_8D5094C4", {dc, 0x00030005}),
                  r.call("cellFont", "cellFont_47B70477", {dc, v}), r.call("cellFont", "cellFont_8D5094C4", {v, 1})) + " " + words(r, v, 1));
        r.rec("context " + words(r, dc, 64));

        // extensions
        const std::vector<u32> queries = {0xCF300001, 0xCF300003, 0xCF300004, 0xCF300005, 0xCF300006, 0xCF300007, 0xCF300011,
            0xCF300014, 0xCF300015, 0xCF500001, 0xCF500002, 0xCF500003, 0xCF500004, 0xCFE00F00, 0xCFE00F01, 0x12345F77};
        const u32 list = r.alloc(8 + 8 * 32), outs = r.alloc(4 * 32);
        r.w32(list, static_cast<u32>(queries.size()));
        for (u32 i = 0; i < queries.size(); i++) {
            r.w32(list + 4 + 8 * i, queries[i]);
            r.w32(list + 8 + 8 * i, outs + 4 * i);
        }
        std::memset(r.b + outs, 0xEE, 4 * 32);
        r.rec(std::format("extend {:#x} {}", r.call("cellFont", "cellFontExtend", {0xCFE00000, 0, list}),
            words(r, outs, static_cast<u32>(queries.size()))));
        r.w32(list, 4);
        r.w32(list + 4, 0xCF300002);
        r.w32(list + 12, 0xCF300001);
        r.w32(list + 16, 0);
        std::memset(r.b + outs, 0xEE, 4 * 32);
        r.rec(std::format("extend bad {:#x} {}", r.call("cellFont", "cellFontExtend", {0xCFE00000, 0, list}), words(r, outs, 4)));
        r.rec(std::format("extend magic {:#x} arg {:#x} null {:#x}", r.call("cellFont", "cellFontExtend", {0xCFE00001, 0, list}),
            r.call("cellFont", "cellFontExtend", {0xCFE00000, 1ull << 32, list}), r.call("cellFont", "cellFontExtend", {0xCFE00000, 0, 0})));
        // the graphics functions it gives
        r.w32(list, 3);
        for (u32 i = 0; i < 3; i++) {
            r.w32(list + 4 + 8 * i, 0xCFE00001 + i);
            r.w32(list + 8 + 8 * i, outs + 4 * i);
        }
        r.rec(std::format("extend graphics {:#x} given {} {} {}", r.call("cellFont", "cellFontExtend", {0xCFE00000, 0, list}),
            r.r32(outs) != 0, r.r32(outs + 4) != 0, r.r32(outs + 8) != 0));
        const u32 init = r.r32(outs), set_flags = r.r32(outs + 4), get_flags = r.r32(outs + 8);
        r.rec(std::format("given flags {:#x}", static_cast<u32>(call_guest(r.t.ctx, set_flags, {dc, 0x00010008}))));
        r.rec(std::format("given flags {:#x}", static_cast<u32>(call_guest(r.t.ctx, get_flags, {dc, v}))) + " " + words(r, v, 1));
        const u32 rec1 = r.alloc(32), rec2 = r.alloc(32), out = r.alloc(8);
        r.w32(rec2, 0xCFE00000);
        r.w32(rec2 + 0x10, r.logger(0x67, 0, 2));
        r.rec(std::format("graphics before {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontEndGraphics", {0}),
            r.call("cellFont", "cellFontEndGraphics", {rec2}),
            static_cast<u32>(call_guest(r.t.ctx, init, {rec1, code.graphics_init, rec2, 0}))));
        std::memset(r.b + out, 0xEE, 8);
        r.rec(std::format("graphics init {:#x}", static_cast<u32>(call_guest(r.t.ctx, init, {rec1, code.graphics_init, rec2, out}))));
        r.rec(std::format("graphics out {} rec {} {}", r.r32(out) == rec2, words(r, rec1 + 8, 1), words(r, rec2 + 8, 1)));
        std::memset(r.b + out, 0xEE, 8);
        r.rec(std::format("graphics again {:#x} out {}", static_cast<u32>(call_guest(r.t.ctx, init, {rec1, code.graphics_init, rec2, out})),
            r.r32(out) == rec1));
        r.rec(std::format("graphics null config {:#x}", static_cast<u32>(call_guest(r.t.ctx, init, {rec1, code.graphics_init, 0, out}))));
        r.clear_log();
        r.w32(rec2 + 8, 0x40000001);
        r.rec(std::format("end graphics {:#x}", r.call("cellFont", "cellFontEndGraphics", {rec2})));
        r.rec(std::format("log {} called {} rec {}", r.log_count(), r.log_count() && r.log_entry(0)[1] == rec2, words(r, rec2 + 8, 1)));
        r.w32(rec2 + 8, 0x40000001);
        r.rec(std::format("delete graphics {:#x}", r.call("cellFont", "cellFontDelete", {lib, rec2})));
        r.rec(std::format("log {} rec {}", r.log_count(), words(r, rec2 + 8, 1)));

        // deletion
        const u32 type = r.alloc(8), f = r.alloc(256), f2 = r.alloc(256), rend = r.alloc(256), rcfg = r.alloc(20), gp = r.alloc(8);
        r.w32(type, 0);
        r.w32(type + 4, 1);
        r.rec(std::format("open {:#x}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f})));
        r.rec(std::format("open file {:#x}", r.call("cellFont", "cellFontOpenFontFile", {lib, r.str(font("SCE-PS3-SR-R-LATIN.TTF")), 0, 3, f2})));
        r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {24.0, 24.0});
        r.w32(rcfg + 4, 0x1000);
        r.w32(rcfg + 8, 0x10000);
        r.rec(std::format("renderer {:#x} magic {:#x}", r.call("cellFont", "cellFontCreateRenderer", {lib, rcfg, rend}), r.r32(rend)));
        r.rec(std::format("glyph {:#x} magic {:#x}", r.call("cellFont", "cellFontGenerateCharGlyph", {f, 'A', gp}), r.r16(r.r32(gp))));
        const u32 other = r.alloc(16);
        r.rec(std::format("delete null {:#x} {:#x} unknown {:#x} other library's font {:#x}", r.call("cellFont", "cellFontDelete", {0, f}),
            r.call("cellFont", "cellFontDelete", {lib, 0}), r.call("cellFont", "cellFontDelete", {lib, v}),
            r.call("cellFont", "cellFontDelete", {other, f})));
        r.rec(std::format("delete glyph {:#x}", r.call("cellFont", "cellFontDelete", {lib, r.r32(gp)})));
        r.rec(std::format("delete renderer {:#x} {}", r.call("cellFont", "cellFontDelete", {lib, rend}), r.r32(rend + 0x40) != 0));
        r.rec(std::format("delete font {:#x} file {:#x} again {:#x}", r.call("cellFont", "cellFontDelete", {lib, f}),
            r.call("cellFont", "cellFontDelete", {lib, f2}), r.call("cellFont", "cellFontDelete", {lib, f2})));
        r.w32(rend, 0xCF100000);
        r.rec(std::format("delete renderer {:#x} {}", r.call("cellFont", "cellFontDelete", {lib, rend}), r.r32(rend + 0x40) != 0));
        r.rec(std::format("delete renderer again {:#x}", r.call("cellFont", "cellFontDelete", {lib, rend})));
        r.rec(std::format("delete library {:#x}", r.call("cellFont", "cellFontDelete", {lib, lib})));
        r.rec(std::format("delete library again {:#x}", r.call("cellFont", "cellFontDelete", {lib, lib})));

        // the patch works: functions turned off one by one
        const u32 lib2 = ft_library(r);
        r.rec(std::format("open {:#x}", r.call("cellFont", "cellFontOpenFontset", {lib2, type, f})));
        r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {24.0, 24.0});
        r.w32(gp, 0);
        r.rec(std::format("glyph {:#x}", r.call("cellFont", "cellFontGenerateCharGlyph", {f, 'B', gp})));
        const u32 g = r.r32(gp), st = r.alloc(16), surf = r.alloc(48), sbuf = r.alloc(64 * 64), m = r.alloc(32), T = r.alloc(24);
        r.call("cellFont", "cellFontRenderSurfaceInit", {surf, sbuf, 64, 1, 64, 64});
        r.rec(std::format("renderer {:#x}", r.call("cellFont", "cellFontCreateRenderer", {lib2, rcfg, rend})));
        for (u32 i = 0; i < 4; i++) r.w32(st + 4 * i, fbits(i < 2 ? 24.0f : 0.0f));
        r.w32(st + 8, fbits(1.0f));
        const auto glyph_calls = [&] {
            const u32 g2 = r.alloc(8);
            std::string s = std::format("{:#x}", r.call("cellFont", "cellFontGenerateCharGlyph", {f, 'C', g2}));
            s += std::format(" {:#x}", r.call("cellFont", "cellFontGenerateCharGlyphVertical", {f, 'C', g2}));
            s += std::format(" {:#x}", r.call("cellFont", "cellFontGlyphGetHorizontalShift", {g, v, v + 4}));
            s += std::format(" {:#x}", r.call("cellFont", "cellFontGlyphGetVerticalShift", {g, v, v + 4}));
            s += std::format(" {:#x}", r.callf("cellFont", "cellFontGlyphRenderImage", {g, st, rend, surf, 0, 0, m, T}, {10.0, 40.0}));
            s += std::format(" {:#x}", r.callf("cellFont", "cellFontGlyphRenderImageHorizontal", {g, st, rend, surf, 0, 0, m, T}, {10.0, 40.0}));
            s += std::format(" {:#x}", r.callf("cellFont", "cellFontGlyphRenderImageVertical", {g, st, rend, surf, 0, 0, m, T}, {10.0, 40.0}));
            return s;
        };
        // a renderer bound to the font: its settings
        {
            const u32 o = r.alloc(16);
            const auto getters = [&](const std::string& tag) {
                std::memset(r.b + o, 0xEE, 16);
                std::string t = tag + std::format(" binding {:#x} {}", r.call("cellFont", "cellFontGetBindingRenderer", {f, o}), r.r32(o) == rend);
                std::memset(r.b + o, 0xEE, 16);
                t += std::format(" point {:#x} {}", r.call("cellFont", "cellFontGetRenderScalePoint", {f, o, o + 4}), fwords(r, o, 2));
                std::memset(r.b + o, 0xEE, 16);
                t += std::format(" pixel {:#x} {}", r.call("cellFont", "cellFontGetRenderScalePixel", {f, o, o + 4}), fwords(r, o, 2));
                std::memset(r.b + o, 0xEE, 16);
                t += std::format(" weight {:#x} slant {:#x} {}", r.call("cellFont", "cellFontGetRenderEffectWeight", {f, o}),
                    r.call("cellFont", "cellFontGetRenderEffectSlant", {f, o + 4}), fwords(r, o, 2));
                r.rec(t);
            };
            getters("unbound");
            r.rec(std::format("bind {:#x} again {:#x} null {:#x} {:#x}", r.call("cellFont", "cellFontBindRenderer", {f, rend}),
                r.call("cellFont", "cellFontBindRenderer", {f, rend}), r.call("cellFont", "cellFontBindRenderer", {0, rend}),
                r.call("cellFont", "cellFontBindRenderer", {f, 0})));
            getters("bound");
            r.rec(std::format("setup point {:#x} pixel {:#x}", r.callf("cellFont", "cellFontSetupRenderScalePoint", {f, 0, 0}, {10.0, 0.0}),
                r.callf("cellFont", "cellFontSetupRenderScalePixel", {f, 0, 0}, {0.0, 17.25})));
            getters("scaled");
            r.rec(std::format("setup weight {:#x} slant {:#x}", r.callf("cellFont", "cellFontSetupRenderEffectWeight", {f, 0}, {1.5}),
                r.callf("cellFont", "cellFontSetupRenderEffectSlant", {f, 0}, {-2.0})));
            getters("effects");
            r.rec(std::format("null {:#x} {:#x} {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontGetBindingRenderer", {0, o}),
                r.call("cellFont", "cellFontGetBindingRenderer", {f, 0}), r.call("cellFont", "cellFontGetRenderScalePoint", {0, o, o}),
                r.callf("cellFont", "cellFontSetupRenderScalePoint", {0, 0, 0}, {1.0, 1.0}),
                r.callf("cellFont", "cellFontSetupRenderEffectSlant", {0, 0}, {1.0})));
            r.rec(std::format("unbind {:#x} again {:#x} null {:#x}", r.call("cellFont", "cellFontUnbindRenderer", {f}),
                r.call("cellFont", "cellFontUnbindRenderer", {f}), r.call("cellFont", "cellFontUnbindRenderer", {0})));
            getters("unbound again");
        }
        r.rec("before patches " + glyph_calls());
        r.rec(std::format("patch works {:#x} {:#x} {:#x}", r.call("cellFont", "cellFontPatchWorks", {0xCF, 0}),
            r.call("cellFont", "cellFontPatchWorks", {0xDF, 0}), r.call("cellFont", "cellFontPatchWorks", {0xDF, 10})));
        r.rec(std::format("patch works {:#x} {:#x}", r.call("cellFont", "cellFontPatchWorks", {0x12, 1}),
            r.call("cellFont", "cellFontPatchWorks", {0xDF, (1ull << 32) | 1})));
        for (u32 bit = 9; bit >= 1; bit--) {
            const u32 e = r.call("cellFont", "cellFontPatchWorks", {0xDF, bit});
            r.rec(std::format("patch {} {:#x}: {}", bit, e, glyph_calls()));
        }
        r.rec(std::format("delete glyph turned off {:#x}", r.call("cellFont", "cellFontDeleteGlyph", {f, g})));
        r.rec(std::format("close {:#x}", r.call("cellFont", "cellFontCloseFont", {f})));
        r.rec(std::format("end {:#x}", r.call("cellFont", "cellFontEnd", {})));
    }, "graphics");
}

TEST_CASE("cellFont: outline vertexes and vertexes glyphs, as libfont", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, {}, kAll, [](Run& r) {
        const Guest code = guest_code(r);
        r.rec(std::format("initialize {:#x}", initialize(r, 8, 4)));
        const u32 lib = ft_library(r);
        constexpr u32 kVertexMax = 0x4000;
        const u32 vlog = r.alloc(20 * kVertexMax), ctl = r.alloc(16), iface = r.alloc(8), box = r.alloc(16), count = r.alloc(8);
        r.w32(iface, code.vertex);
        r.w32(iface + 4, ctl);
        // the vertexes of a glyph, through the script's callback and without one
        const auto vertexes = [&](u32 g, float distance, const std::string& tag) {
            r.w32(ctl, vlog);
            r.w32(ctl + 4, 0);
            std::memset(r.b + box, 0xEE, 16);
            std::memset(r.b + count, 0xEE, 8);
            const u32 e = r.callf("cellFont", "cellFontGlyphGetOutlineVertexes", {g, 0, iface, box, count}, {static_cast<double>(distance)});
            const u32 n = r.r32(ctl + 4);
            REQUIRE(n < kVertexMax);
            std::string s = std::format("{} vertexes {}: {:#x} count {} calls {} box {}", tag, distance, e, r.r32(count), n, fwords(r, box, 4));
            u64 hsh = 0xcbf29ce484222325ull;
            for (u32 i = 0; i < 20 * n; i++) hsh = (hsh ^ r.b[vlog + i]) * 0x100000001b3ull;
            s += std::format(" log {:#x}", hsh);
            for (u32 i = 0; i < std::min<u32>(n, 6); i++) {
                const u32 a = vlog + 20 * i;
                s += std::format(" [{} {} {} {} {}]", r.s32at(a), r.s32at(a + 4), r.r32(a + 8), std::bit_cast<float>(r.r32(a + 12)),
                    std::bit_cast<float>(r.r32(a + 16)));
            }
            r.rec(s);
            std::memset(r.b + box, 0xEE, 16);
            const u32 e2 = r.callf("cellFont", "cellFontGlyphGetOutlineVertexes", {g, 0, 0, box, count}, {static_cast<double>(distance)});
            r.rec(std::format("{} no callback {:#x} count {} box {}", tag, e2, r.r32(count), fwords(r, box, 4)));
            r.rec(std::format("{} no outputs {:#x}", tag, r.callf("cellFont", "cellFontGlyphGetOutlineVertexes", {g, 0, 0, 0, 0}, {static_cast<double>(distance)})));
        };
        // vertexes glyphs: the size, the glyph made in enough memory and in too little, relocated
        const auto vertexes_glyph = [&](u32 g, float distance, const std::string& tag) {
            const u32 size = r.alloc(8), vg = r.alloc(8), bytes = r.alloc(8);
            r.w32(size, 0xEEEEEEEE);
            const u32 e = r.callf("cellFont", "cellFontGlyphGetVertexesGlyphSize", {g, 0, size}, {static_cast<double>(distance)});
            const u32 need = r.r32(size);
            r.rec(std::format("{} size {}: {:#x} {}", tag, distance, e, need));
            if (e || !need) return;
            const u32 mem = r.alloc(need + 64);
            std::memset(r.b + mem, 0xEE, need + 64);
            r.w32(bytes, 0xEEEEEEEE);
            const u32 se = r.callf("cellFont", "cellFontGlyphSetupVertexesGlyph", {g, 0, mem, need, vg, bytes}, {static_cast<double>(distance)});
            r.rec(std::format("{} setup {:#x} bytes {} vg {} {} head {} {} data {:#x} past {}", tag, se, r.r32(bytes), r.r32(vg) == mem,
                r.r32(vg + 4), words(r, mem, 3), fwords(r, mem + 12, 18), fnv(r, mem + 84, static_cast<s32>(need - 84), 1, 0),
                words(r, mem + need, 1)));
            const u32 small = r.alloc(need);
            r.w32(bytes, 0xEEEEEEEE);
            const u32 se2 = r.callf("cellFont", "cellFontGlyphSetupVertexesGlyph", {g, 0, small, need - 12, vg, bytes}, {static_cast<double>(distance)});
            r.rec(std::format("{} setup small {:#x} bytes {} vg {} {}", tag, se2, r.r32(bytes), r.r32(vg), r.r32(vg + 4)));
            r.rec(std::format("{} setup null {:#x} {:#x}", tag,
                r.callf("cellFont", "cellFontGlyphSetupVertexesGlyph", {g, 0, 0, need, vg, 0}, {static_cast<double>(distance)}),
                r.callf("cellFont", "cellFontGlyphSetupVertexesGlyph", {g, 0, mem, need, 0, bytes}, {static_cast<double>(distance)})));
            r.callf("cellFont", "cellFontGlyphSetupVertexesGlyph", {g, 0, mem, need, vg, bytes}, {static_cast<double>(distance)});
            const u32 vg2 = r.alloc(8), head = r.alloc(48), data = r.alloc(need);
            std::memset(r.b + head, 0xEE, 48);
            const u32 re = r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, head, data, need});
            r.rec(std::format("{} relocate {:#x} vg {} {} head {} same data {}", tag, re, r.r32(vg2) == data, r.r32(vg2 + 4) == head,
                words(r, head, 12), std::memcmp(r.b + data, r.b + mem, need) == 0));
            std::memset(r.b + head, 0xEE, 48);
            const u32 re2 = r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg, head, mem, need});
            r.rec(std::format("{} relocate in place {:#x} {}", tag, re2, words(r, head, 12)));
            std::string s = tag + " relocate";
            s += std::format(" no size {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, head, data, 0}));
            s += std::format(" wrong size {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, head, data, need + 4}));
            s += std::format(" no data {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, head, 0, need}));
            s += std::format(" no head {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, 0, data, need}));
            s += std::format(" no source {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {0, vg2, head, data, need}));
            s += std::format(" no target {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, 0, head, data, need}));
            r.w32(vg, 0);
            s += std::format(" empty {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, head, data, need}));
            s += std::format(" empty no data {:#x}", r.call("cellFont", "cellFontVertexesGlyphRelocate", {vg, vg2, head, 0, need}));
            r.rec(s + " vg2 " + words(r, vg2, 2));
        };

        // made outlines: lines, conics (on-curve ends, implied midpoints, ending on the start), cubics (one bending both
        // ways), degenerate contours, and malformed ones
        struct Pt {
            float x, y;
            u8 tag;
        };
        const auto make = [&](const std::vector<std::vector<Pt>>& contours, s16 n) {
            const u32 g = r.alloc(64), env = r.alloc(128);
            u32 total = 0;
            for (const auto& c : contours) total += static_cast<u32>(c.size());
            const u32 pts = r.alloc(8 * total + 8), tags = r.alloc(total + 8), ends = r.alloc(2 * static_cast<u32>(contours.size()) + 8);
            u32 i = 0;
            for (u32 k = 0; k < contours.size(); k++) {
                for (const Pt& p : contours[k]) {
                    r.w32(pts + 8 * i, fbits(p.x));
                    r.w32(pts + 8 * i + 4, fbits(p.y));
                    r.b[tags + i] = p.tag;
                    i++;
                }
                r.w16(ends + 2 * k, static_cast<u16>(i - 1));
            }
            r.w16(g + 40, static_cast<u16>(n));
            r.w16(g + 42, static_cast<u16>(total));
            r.w32(g + 44, pts);
            r.w32(g + 48, tags);
            r.w32(g + 52, ends);
            r.w32(g + 60, env);
            for (u32 k = 0; k < 32; k++) r.w32(env + 4 * k, fbits(1.0f + 0.5f * static_cast<float>(k)));
            return g;
        };
        const std::vector<std::vector<Pt>> shapes = {
            {{0, 0, 1}, {10, 0, 1}, {10, 10, 1}, {0, 10, 1}},
            {{20, 0, 1}, {30, 0, 0}, {30, 10, 1}, {30, 20, 0}, {20, 20, 0}},
            {{40, 0, 1}, {50, 0, 2}, {60, 10, 2}, {60, 20, 1}, {60, 35, 2}, {40, 30, 2}},
            {{0, 40, 1}, {6, 47, 2}, {9, 33, 2}, {15, 40, 1}},
            {{70, 0, 0}, {80, 0, 1}, {80, 10, 0}, {70, 10, 1}},
            {{90, 0, 0}, {100, 5, 0}, {95, 15, 0}, {85, 12, 0}},
            {{0, 60, 1}, {5, 65, 1}},
            {{20, 60, 1}, {25, 70, 2}, {30, 60, 1}, {35, 65, 1}},
            {{40, 60, 3}, {45, 70, 0}, {50, 60, 1}},
            {{60, 60, 1}},
            {{70, 60, 0}},
            {{80, 60, 1}, {81, 60.5f, 0}, {82, 61, 1}, {83, 61.5f, 2}, {84, 62, 2}, {85, 62.5f, 1}},
        };
        const u32 made = make(shapes, static_cast<s16>(shapes.size()));
        for (const float d : {0.05f, 0.5f, 4.0f}) {
            vertexes(made, d, "made");
            vertexes_glyph(made, d, "made");
        }
        const u32 bad1 = make({{{0, 0, 2}, {1, 1, 1}, {2, 0, 1}}}, 1);
        const u32 bad2 = make({{{0, 0, 1}, {1, 1, 0}, {2, 0, 2}, {3, 1, 2}}}, 1);
        const u32 bad3 = make({{{0, 0, 1}, {1, 1, 0}, {2, 0, 0}, {3, 1, 3}}}, 1);
        const u32 none = make({}, 0), negative = make({{{0, 0, 1}}}, -1);
        for (const auto& [g, tag] : {std::pair{bad1, "cubic start"}, std::pair{bad2, "conic then cubic"},
                 std::pair{bad3, "conics then cubic"}, std::pair{none, "none"}, std::pair{negative, "negative"}}) {
            vertexes(g, 0.5f, tag);
            vertexes_glyph(g, 0.5f, tag);
        }
        const u32 cd = r.alloc(8);
        r.rec(std::format("null glyph {:#x} {:#x} {:#x} {:#x}",
            r.callf("cellFont", "cellFontGlyphGetOutlineVertexes", {0, 0, iface, box, count}, {1.0}),
            r.callf("cellFont", "cellFontGlyphGetVertexesGlyphSize", {0, 0, cd}, {1.0}),
            r.callf("cellFont", "cellFontGlyphGetVertexesGlyphSize", {made, 0, 0}, {1.0}),
            r.callf("cellFont", "cellFontGlyphSetupVertexesGlyph", {0, 0, cd, 64, r.alloc(8), 0}, {1.0})));
        r.rec(std::format("control distance {:#x} {:#x} {:#x}",
            r.callf("cellFont", "cellFontGlyphGetOutlineControlDistance", {made, 0, 0, cd}, {2.0, 0.25}),
            r.callf("cellFont", "cellFontGlyphGetOutlineControlDistance", {made, 0, 0, cd}, {0.0, 0.25}),
            r.callf("cellFont", "cellFontGlyphGetOutlineControlDistance", {made, 0, 0, 0}, {2.0, 0.25})) + " " + fwords(r, cd, 1));

        // the fonts' own glyphs
        const u32 type = r.alloc(8), f = r.alloc(256), gp = r.alloc(8);
        r.w32(type, 0x100);
        r.w32(type + 4, 1);
        r.rec(std::format("open {:#x}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f})));
        r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {30.0, 30.0});
        for (const u32 c : {0x41u, 0x67u, 0x40u, 0x3042u, 0x4E00u, 0x30FCu}) {
            for (const char* gen : {"cellFontGenerateCharGlyph", "cellFontGenerateCharGlyphVertical"}) {
                r.w32(gp, 0);
                const u32 e = r.call("cellFont", gen, {f, c, gp});
                const std::string tag = std::format("{} {:#x}", gen, c);
                r.rec(std::format("{}: {:#x}", tag, e));
                if (e) continue;
                const u32 g = r.r32(gp);
                for (const float d : {0.1f, 1.0f}) {
                    vertexes(g, d, tag);
                    vertexes_glyph(g, d, tag);
                }
                std::memset(r.b + cd, 0xEE, 8);
                r.rec(std::format("{} control distance {:#x} {}", tag,
                    r.callf("cellFont", "cellFontGlyphGetOutlineControlDistance", {g, 0, 0, cd}, {48.0, 0.5}), fwords(r, cd, 1)));
                r.rec(std::format("{} delete {:#x}", tag, r.call("cellFont", "cellFontDeleteGlyph", {f, g})));
            }
        }
        r.rec(std::format("close {:#x}", r.call("cellFont", "cellFontCloseFont", {f})));
        r.rec(std::format("end library {:#x}", r.call("cellFont", "cellFontEndLibrary", {lib})));
        r.rec(std::format("end {:#x}", r.call("cellFont", "cellFontEnd", {})));
    }, "vertexes");
}

TEST_CASE("cellFont: composite codes of a font in the game's font directory, as libfont", "[font][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    // A font directory of the game's: the firmware's Latin font (copied for the test, removed after) and a composite
    // code table beside it, made here: 'A'..'C' drawn as 'X'..'Z', 'a' and 'b' as two codes, 'c' as none, U+2000 and
    // U+2001 as 'W' and U+3042.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / std::format("pspocket-font-ccd-{}", static_cast<unsigned>(::getpid()));
    fs::create_directories(dir);
    fs::copy_file(*flash + "/data/font/SCE-PS3-RD-R-LATIN.TTF", dir / "SCE-PS3-RD-R-LATIN.TTF", fs::copy_options::overwrite_existing);
    {
        const std::vector<std::array<u16, 4>> ranges = {{0x41, 0x43, 1, 0}, {0x61, 0x62, 2, 3}, {0x63, 0x63, 0, 0}, {0x2000, 0x2001, 1, 7}};
        const std::vector<u16> table = {'X', 'Y', 'Z', 'o', 'i', 'd', 'l', 'W', 0x3042};
        std::vector<u8> ccd(8 + 8 * ranges.size() + 2 * table.size());
        const auto put = [&](usize at, u16 v) {
            ccd[at] = static_cast<u8>(v >> 8);
            ccd[at + 1] = static_cast<u8>(v);
        };
        put(6, static_cast<u16>(ranges.size()));
        for (usize i = 0; i < ranges.size(); i++)
            for (usize k = 0; k < 4; k++) put(8 + 8 * i + 2 * k, ranges[i][k]);
        for (usize i = 0; i < table.size(); i++) put(8 + 8 * ranges.size() + 2 * i, table[i]);
        std::ofstream(dir / "SCE-PS3-RD-R-LATIN.ccd", std::ios::binary).write(reinterpret_cast<const char*>(ccd.data()),
            static_cast<std::streamsize>(ccd.size()));
    }
    compare(*flash, {}, kAll, [&dir](Run& r) {
        REQUIRE(r.f.kernel->vfs_mount("/dev_hdd0", dir.string(), true));
        r.rec(std::format("initialize {:#x}", initialize(r, 8, 4)));
        const u32 lib = ft_library(r);
        r.rec(std::format("directory {:#x}", r.call("cellFont", "cellFontControl", {0x540000, r.str("/dev_hdd0/")})));
        const u32 type = r.alloc(8), f = r.alloc(256), f2 = r.alloc(256);
        r.w32(type, 0);
        r.w32(type + 4, 1);
        r.rec(std::format("open {:#x} {}", r.call("cellFont", "cellFontOpenFontset", {lib, type, f}), font_text(r, f)));
        r.callf("cellFont", "cellFontSetScalePixel", {f, 0, 0}, {20.0, 20.0});
        const std::vector<u32> codes = {'A', 'B', 'C', 'D', 'a', 'b', 'c', 'd', 0x2000, 0x2001, 0x2002, 0xFFFF};
        font_codes(r, f, "composite", codes);
        r.rec(std::format("on memory {:#x}", r.call("cellFont", "cellFontOpenFontsetOnMemory", {lib, type, f2})));
        r.callf("cellFont", "cellFontSetScalePixel", {f2, 0, 0}, {13.0, 13.0});
        font_codes(r, f2, "composite on memory", codes);
        const u32 gp = r.alloc(8);
        for (const u32 c : {'A', 'a', 'c'}) {
            r.w32(gp, 0);
            const u32 e = r.call("cellFont", "cellFontGenerateCharGlyph", {f, c, gp});
            r.rec(std::format("glyph {:#x}: {:#x} {}", c, e, e ? std::string() : fwords(r, r.r32(gp) + 8, 8)));
        }
        r.rec(std::format("close {:#x} {:#x}", r.call("cellFont", "cellFontCloseFont", {f}), r.call("cellFont", "cellFontCloseFont", {f2})));
        r.rec(std::format("no directory {:#x} end {:#x}", r.call("cellFont", "cellFontControl", {0x540000, 0}),
            r.call("cellFont", "cellFontEnd", {})));
        // (FreeType's own objects are the host's, D-525: the game's allocator is used, not as often as by Sony's)
        r.rec(std::format("memory: {} mallocs, {} frees", r.font_mallocs() != 0, r.font_frees() != 0));
    }, "composite codes");
    fs::remove_all(dir);
}
