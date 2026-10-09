// cell_FreeType2 against libfreetype.sprx (D-193's method, D-525): the same scripts in two fresh processes, one on
// Sony's module and one on pspocket's, the answers and what the objects hold (faces, sizes, glyph slots, bitmaps,
// outlines, glyph images, cache records) compared; for what the guest owns outright (outlines, bitmaps, glyph images)
// the game's FT_Memory calls too. The fonts are the firmware's own (dev_flash/data/font), read at test time. Skipped
// without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include "freetype_compare.hpp"

#include <random>

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::ft;

namespace {

const std::vector<std::string> kLibs = {"cell_FreeType2"};

// FreeType's constants used by the scripts.
constexpr u32 FT_LOAD_DEFAULT = 0, FT_LOAD_NO_SCALE = 1, FT_LOAD_NO_HINTING = 2, FT_LOAD_RENDER = 4,
              FT_LOAD_NO_BITMAP = 8, FT_LOAD_VERTICAL_LAYOUT = 0x10, FT_LOAD_FORCE_AUTOHINT = 0x20,
              FT_LOAD_PEDANTIC = 0x80, FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH = 0x200, FT_LOAD_NO_RECURSE = 0x400,
              FT_LOAD_IGNORE_TRANSFORM = 0x800, FT_LOAD_MONOCHROME = 0x1000, FT_LOAD_LINEAR_DESIGN = 0x2000,
              FT_LOAD_NO_AUTOHINT = 0x8000, FT_LOAD_TARGET_LIGHT = 1 << 16, FT_LOAD_TARGET_MONO = 2 << 16,
              FT_LOAD_TARGET_LCD = 3 << 16, FT_LOAD_TARGET_LCD_V = 4 << 16;
constexpr u32 kFormatOutline = 0x6F75746C, kFormatBitmap = 0x62697473, kFormatComposite = 0x636F6D70;

std::string font(const std::string& name) { return "/dev_flash/data/font/" + name; }

const std::vector<std::string> kLatinFonts = {"SCE-PS3-RD-R-LATIN.TTF", "SCE-PS3-RD-B-LATIN.TTF",
    "SCE-PS3-MT-I-LATIN.TTF", "SCE-PS3-SR-R-LATIN2.TTF", "SCE-PS3-VR-R-LATIN.TTF", "SCE-PS3-CP-R-KANA.TTF"};

// ---------------------------------------------------------------------------------------------------- descriptions

std::string ptr(u32 p) { return p ? "p" : "0"; }

std::string bitmap_text(Run& r, u32 bm, bool pixels = true)
{
    const s32 rows = r.s32at(bm), width = r.s32at(bm + 4), pitch = r.s32at(bm + 8);
    const u32 buffer = r.r32(bm + 12);
    std::string s = std::format("bitmap {}x{} pitch {} grays {} mode {} palmode {} pal {} buf {}", width, rows, pitch,
        r.r16(bm + 16), r.r8(bm + 18), r.r8(bm + 19), ptr(r.r32(bm + 20)), ptr(buffer));
    if (pixels && buffer && rows > 0 && rows < 4096) {
        const u32 n = static_cast<u32>(std::abs(pitch)) * static_cast<u32>(rows);
        if (n < 0x100000) s += " " + hex(r.b + buffer, n);
    }
    return s;
}

std::string outline_text(Run& r, u32 ol, bool owner_flag = true)
{
    const s16 nc = r.s16at(ol), np = r.s16at(ol + 2);
    const u32 pts = r.r32(ol + 4), tags = r.r32(ol + 8), cts = r.r32(ol + 12);
    u32 flags = r.r32(ol + 16);
    if (!owner_flag) flags &= ~1u;
    std::string s = std::format("outline {}c {}p flags {:#x} arrays {}{}{}", nc, np, flags, ptr(pts), ptr(tags), ptr(cts));
    if (np > 0 && pts && tags) {
        s += " pts";
        for (s32 i = 0; i < np && i < 4096; i++)
            s += std::format(" {},{}:{}", r.s32at(pts + 8 * static_cast<u32>(i)), r.s32at(pts + 8 * static_cast<u32>(i) + 4),
                r.r8(tags + static_cast<u32>(i)));
    }
    if (nc > 0 && cts) {
        s += " ends";
        for (s32 i = 0; i < nc && i < 4096; i++) s += std::format(" {}", r.s16at(cts + 2 * static_cast<u32>(i)));
    }
    return s;
}

// A module: its class's flags, size, name, version and requirement.
std::string module_text(Run& r, u32 m)
{
    if (!m) return "module 0";
    const u32 c = r.r32(m);
    return std::format("module {} flags {:#x} size {:#x} v{:#x} req {:#x}", r.cstr(r.r32(c + 8)), r.r32(c), r.r32(c + 4),
        r.r32(c + 12), r.r32(c + 16));
}

void dump_library(Run& r, u32 lib, const std::string& what)
{
    r.rec(std::format("{}: memory {} version {}.{}.{} modules {}", what, r.r32(lib) == kMemory ? "ours" : "?",
        r.r32(lib + 12), r.r32(lib + 16), r.r32(lib + 20), r.r32(lib + 24)));
    const u32 n = r.r32(lib + 24);
    for (u32 i = 0; i < n && i < 32; i++) {
        const u32 m = r.r32(lib + 28 + 4 * i);
        r.rec(std::format("  [{}] {} lib {} mem {}", i, module_text(r, m), r.r32(m + 4) == lib ? "this" : "?",
            r.r32(m + 8) == kMemory ? "ours" : "?"));
    }
    std::string ren = "  renderers:";
    for (u32 node = r.r32(lib + 156); node; node = r.r32(node + 4)) ren += " " + r.cstr(r.r32(r.r32(r.r32(node + 8)) + 8));
    r.rec(ren);
    const u32 cur = r.r32(lib + 164), hinter = r.r32(lib + 168);
    r.rec(std::format("  current renderer {} auto-hinter {}", cur ? r.cstr(r.r32(r.r32(cur) + 8)) : "0",
        hinter ? r.cstr(r.r32(r.r32(hinter) + 8)) : "0"));
}

std::string size_metrics_text(Run& r, u32 m)
{
    return std::format("ppem {}x{} scale {:#x},{:#x} asc {} desc {} height {} maxadv {}", r.r16(m), r.r16(m + 2),
        r.r32(m + 4), r.r32(m + 8), r.s32at(m + 12), r.s32at(m + 16), r.s32at(m + 20), r.s32at(m + 24));
}

void dump_face(Run& r, u32 face, const std::string& what)
{
    r.rec(std::format("{}: faces {} index {} flags {:#x} style {:#x} glyphs {} family \"{}\" style \"{}\"", what,
        r.s32at(face), r.s32at(face + 4), r.r32(face + 8), r.r32(face + 12), r.s32at(face + 16), r.cstr(r.r32(face + 20)),
        r.cstr(r.r32(face + 24))));
    const s32 nfixed = r.s32at(face + 28);
    std::string fixed = std::format("  fixed sizes {}:", nfixed);
    for (s32 i = 0; i < nfixed && i < 64; i++) {
        const u32 e = r.r32(face + 32) + 16 * static_cast<u32>(i);
        fixed += std::format(" {}x{}/{}/{},{}", r.s16at(e + 2), r.s16at(e), r.s32at(e + 4), r.s32at(e + 8), r.s32at(e + 12));
    }
    r.rec(fixed);
    const s32 ncm = r.s32at(face + 36);
    std::string cms = std::format("  charmaps {}:", ncm);
    s32 current = -1;
    for (s32 i = 0; i < ncm && i < 64; i++) {
        const u32 cm = r.r32(r.r32(face + 40) + 4 * static_cast<u32>(i));
        if (cm == r.r32(face + 92)) current = i;
        cms += std::format(" {:#x}/{}/{}{}", r.r32(cm + 4), r.r16(cm + 8), r.r16(cm + 10), r.r32(cm) == face ? "" : "(face?)");
    }
    r.rec(cms + std::format(" current {}{}", current, r.r32(face + 92) && current < 0 ? "?" : ""));
    r.rec(std::format("  bbox {},{},{},{} upem {} asc {} desc {} height {} maxadv {},{} underline {},{}", r.s32at(face + 52),
        r.s32at(face + 56), r.s32at(face + 60), r.s32at(face + 64), r.r16(face + 68), r.s16at(face + 70), r.s16at(face + 72),
        r.s16at(face + 74), r.s16at(face + 76), r.s16at(face + 78), r.s16at(face + 80), r.s16at(face + 82)));
    const u32 size = r.r32(face + 88);
    r.rec(std::format("  glyph {} size {} {} memory {} driver {}", ptr(r.r32(face + 84)), ptr(size),
        size ? size_metrics_text(r, size + 12) : "", r.r32(face + 100) == kMemory ? "ours" : "?",
        module_text(r, r.r32(face + 96))));
    std::string sizes = "  sizes:";
    for (u32 node = r.r32(face + 108); node; node = r.r32(node + 4)) {
        const u32 s = r.r32(node + 8);
        sizes += std::format(" [{}{}]", s == size ? "*" : "", r.r32(s) == face ? "" : "face?");
    }
    r.rec(sizes);
    const u32 stream = r.r32(face + 104);
    // a file's stream keeps the last frame it extracted as its base (a FreeType quirk; pspocket's shows 0)
    if (stream)
        r.rec(std::format("  stream size {} pos {:#x} {} descriptor {} pathname \"{}\" memory {}", r.r32(stream + 4),
            r.r32(stream + 8), r.r32(stream + 20) ? "read" : std::format("memory base {}", ptr(r.r32(stream))),
            // a file's descriptor is its fd; a compressed stream's points to its state
            r.r32(stream + 16) ? std::to_string(r.r32(stream + 12)) : ptr(r.r32(stream + 12)),
            r.r32(stream + 16) ? r.cstr(r.r32(stream + 16)) : "", r.r32(stream + 28) == kMemory ? "ours" : "?"));
}

void dump_slot(Run& r, u32 slot, const std::string& what, bool pixels = true)
{
    r.rec(std::format("{}: metrics {} {} {} {} {} {} {} {} linear {:#x},{:#x} advance {},{} format {:#x}", what,
        r.s32at(slot + 24), r.s32at(slot + 28), r.s32at(slot + 32), r.s32at(slot + 36), r.s32at(slot + 40),
        r.s32at(slot + 44), r.s32at(slot + 48), r.s32at(slot + 52), r.r32(slot + 56), r.r32(slot + 60), r.s32at(slot + 64),
        r.s32at(slot + 68), r.r32(slot + 72)));
    r.rec("  " + bitmap_text(r, slot + 76, pixels) + std::format(" left {} top {}", r.s32at(slot + 100), r.s32at(slot + 104)));
    r.rec("  " + outline_text(r, slot + 108));
    std::string sub = std::format("  subglyphs {}", r.r32(slot + 128));
    for (u32 i = 0; i < r.r32(slot + 128) && i < 64; i++) {
        const u32 e = r.r32(slot + 132) + 32 * i;
        sub += std::format(" [{} {:#x} {} {} {:#x} {:#x} {:#x} {:#x}]", r.s32at(e), r.r16(e + 4), r.s32at(e + 8),
            r.s32at(e + 12), r.r32(e + 16), r.r32(e + 20), r.r32(e + 24), r.r32(e + 28));
    }
    r.rec(sub + std::format(" control {} lsb {} rsb {}", r.r32(slot + 140), r.s32at(slot + 144), r.s32at(slot + 148)));
}

std::string glyph_text(Run& r, u32 g)
{
    if (!g) return "glyph 0";
    std::string s = std::format("glyph format {:#x} advance {:#x},{:#x} class {:#x}/{:#x}", r.r32(g + 8), r.r32(g + 12),
        r.r32(g + 16), r.r32(r.r32(g + 4)), r.r32(r.r32(g + 4) + 4));
    if (r.r32(g + 8) == kFormatBitmap) s += std::format(" left {} top {} ", r.s32at(g + 20), r.s32at(g + 24)) + bitmap_text(r, g + 28);
    else if (r.r32(g + 8) == kFormatOutline) s += " " + outline_text(r, g + 20);
    return s;
}

// A library with the default modules.
u32 new_library(Run& r)
{
    const u32 out = r.alloc(8);
    r.ft("FT_New_Library", {kMemory, out});
    const u32 lib = r.r32(out);
    r.ftq("FT_Add_Default_Modules", {lib});
    return lib;
}

u32 open_face(Run& r, u32 lib, const std::string& path, s32 index = 0)
{
    const u32 out = r.alloc(8);
    const u32 e = r.ft("FT_New_Face", {lib, r.str(path), static_cast<u64>(static_cast<s64>(index)), out});
    return e ? 0 : r.r32(out);
}

u64 sx(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

} // namespace

TEST_CASE("cell_FreeType2: fixed point and trigonometry, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        std::mt19937 rng(525);
        std::vector<s32> vals = {0, 1, -1, 2, -2, 0x8000, -0x8000, 0xFFFF, 0x10000, -0x10000, 0x10001, 0x7FFF, 0x12345,
            0x7FFFFFFF, static_cast<s32>(0x80000000), static_cast<s32>(0x80000001), 0x40000000, -0x40000000, 46340, 46341,
            0xB504, 1 << 20, -(1 << 20), 1 << 24, 360 << 16, 180 << 16, 90 << 16, -(90 << 16), 45 << 16, 0x3FFFFFFF};
        for (int i = 0; i < 60; i++) vals.push_back(static_cast<s32>(rng()));
        for (int i = 0; i < 30; i++) vals.push_back(static_cast<s32>(rng() % 0x200000) - 0x100000);
        for (const s32 a : vals) {
            for (const char* fn : {"FT_RoundFix", "FT_CeilFix", "FT_FloorFix", "FT_Sin", "FT_Cos", "FT_Tan"})
                r.ft(fn, {sx(a)});
        }
        for (usize i = 0; i < vals.size(); i++) {
            for (usize j = 0; j < vals.size(); j += 7) {
                const s32 a = vals[i], b = vals[(i * 13 + j) % vals.size()];
                r.ft("FT_MulFix", {sx(a), sx(b)});
                r.ft("FT_DivFix", {sx(a), sx(b)});
                r.ft("FT_Atan2", {sx(a), sx(b)});
                r.ft("FT_Angle_Diff", {sx(a), sx(b)});
                r.ft("FT_MulDiv", {sx(a), sx(b), sx(vals[(i + j) % vals.size()])});
            }
        }
        const u32 v = r.alloc(16), len = r.alloc(4), ang = r.alloc(4), m = r.alloc(16), m2 = r.alloc(16);
        for (usize i = 0; i < vals.size(); i++) {
            const s32 x = vals[i], y = vals[(i * 7 + 3) % vals.size()], an = vals[(i * 5 + 1) % vals.size()];
            auto vec = [&] {
                r.w32(v, static_cast<u32>(x));
                r.w32(v + 4, static_cast<u32>(y));
            };
            auto keep = [&](const char* what) { r.rec(std::format("{} {} {}", what, r.s32at(v), r.s32at(v + 4))); };
            r.ftq("FT_Vector_Unit", {v, sx(an)});
            keep("unit");
            vec();
            r.ftq("FT_Vector_Rotate", {v, sx(an)});
            keep("rotate");
            vec();
            r.ft("FT_Vector_Length", {v});
            vec();
            r.w32(len, 0x1111);
            r.w32(ang, 0x2222);
            r.ftq("FT_Vector_Polarize", {v, len, ang});
            r.rec(std::format("polarize {} {}", r.s32at(len), r.s32at(ang)));
            r.ftq("FT_Vector_From_Polar", {v, sx(x), sx(an)});
            keep("from polar");
            vec();
            for (u32 k = 0; k < 4; k++) r.w32(m + 4 * k, static_cast<u32>(vals[(i * 3 + k * 11) % vals.size()]));
            r.ftq("FT_Vector_Transform", {v, m});
            keep("transform");
            for (u32 k = 0; k < 4; k++) r.w32(m2 + 4 * k, static_cast<u32>(vals[(i * 17 + k * 5 + 2) % vals.size()]));
            r.ftq("FT_Matrix_Multiply", {m, m2});
            r.rec(std::format("multiply {} {} {} {}", r.s32at(m2), r.s32at(m2 + 4), r.s32at(m2 + 8), r.s32at(m2 + 12)));
            r.ft("FT_Matrix_Invert", {m2});
            r.rec(std::format("invert {} {} {} {}", r.s32at(m2), r.s32at(m2 + 4), r.s32at(m2 + 8), r.s32at(m2 + 12)));
        }
        r.ftq("FT_Vector_Transform", {0, m});
        r.ftq("FT_Vector_Transform", {v, 0});
        r.ftq("FT_Matrix_Multiply", {0, m});
        r.ft("FT_Matrix_Invert", {0});
        for (u32 k = 0; k < 4; k++) r.w32(m + 4 * k, 0);
        r.ft("FT_Matrix_Invert", {m});
    }, "math");
}

TEST_CASE("cell_FreeType2: lists and the old memory calls, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 list = r.alloc(8);
        std::vector<u32> nodes;
        for (u32 i = 0; i < 6; i++) {
            const u32 n = r.alloc(12);
            r.w32(n + 8, 100 + i);
            nodes.push_back(n);
        }
        auto name = [&](u32 n) -> std::string {
            for (usize i = 0; i < nodes.size(); i++)
                if (nodes[i] == n) return std::to_string(i);
            return n ? "?" : "0";
        };
        auto show = [&](const char* what) {
            std::string s = std::format("{}: head {} tail {}:", what, name(r.r32(list)), name(r.r32(list + 4)));
            for (u32 n = r.r32(list); n; n = r.r32(n + 4))
                s += std::format(" [{} prev {} next {} data {}]", name(n), name(r.r32(n)), name(r.r32(n + 4)), r.r32(n + 8));
            r.rec(s);
        };
        r.ftq("FT_List_Add", {list, nodes[0]});
        r.ftq("FT_List_Add", {list, nodes[1]});
        r.ftq("FT_List_Insert", {list, nodes[2]});
        r.ftq("FT_List_Add", {list, nodes[3]});
        r.ftq("FT_List_Insert", {list, nodes[4]});
        show("built");
        r.ftq("FT_List_Up", {list, nodes[3]});
        show("up 3");
        r.ftq("FT_List_Up", {list, nodes[3]});
        show("up 3 again");
        r.ftq("FT_List_Up", {list, nodes[1]});
        show("up 1");
        r.ftq("FT_List_Remove", {list, nodes[0]});
        show("remove 0");
        r.ftq("FT_List_Remove", {list, nodes[3]});
        show("remove head");
        r.rec("find 102 -> " + name(r.ftq("FT_List_Find", {list, 102})));
        r.rec("find 999 -> " + name(r.ftq("FT_List_Find", {list, 999})));
        const u32 it = r.logger(1, 0, 2);
        r.set_answer(2, 77);
        r.ft("FT_List_Iterate", {list, it, 0x55});
        for (u32 i = 0; i < r.log_count(); i++) {
            const auto e = r.log_entry(i);
            r.rec(std::format("iterate {} node {} user {:#x}", e[0], name(e[1]), e[2]));
        }
        r.clear_log();
        // FT_List_Finalize frees the nodes through the memory: they come from it
        const u32 list2 = r.alloc(8);
        for (u32 i = 0; i < 3; i++) {
            const u32 p = r.alloc(4);
            r.ft("FT_Alloc", {kMemory, 12, p});
            nodes.push_back(r.r32(p));
            r.w32(r.r32(p) + 8, 200 + i);
            r.ftq("FT_List_Add", {list2, r.r32(p)});
        }
        const u32 mark = r.heap_mark();
        const u32 destroy = r.logger(2, 0, 3);
        r.ftq("FT_List_Finalize", {list2, destroy, kMemory, 0x66});
        for (u32 i = 0; i < r.log_count(); i++) {
            const auto e = r.log_entry(i);
            r.rec(std::format("destroy {} memory {} data {} user {:#x}", e[0], e[1] == kMemory ? "ours" : "?", e[2], e[3]));
        }
        r.rec("finalize heap:" + r.heap_ops(mark) + std::format(" list {} {}", r.r32(list2), r.r32(list2 + 4)));
        // FT_Alloc, FT_Realloc, FT_Free
        const u32 p = r.alloc(4);
        const u32 m2 = r.heap_mark();
        r.ft("FT_Alloc", {kMemory, 0, p});
        r.rec(std::format("alloc 0 -> {}", ptr(r.r32(p))));
        r.ft("FT_Alloc", {kMemory, sx(-5), p});
        r.ft("FT_Alloc", {kMemory, 40, p});
        r.rec("alloc 40 zeroed " + hex(r.b + r.r32(p), 40));
        std::memset(r.b + r.r32(p), 0x5A, 40);
        r.ft("FT_Realloc", {kMemory, 40, 100, p});
        r.rec("realloc 100 " + hex(r.b + r.r32(p), 100));
        r.ft("FT_Realloc", {kMemory, 100, 20, p});
        r.ft("FT_Realloc", {kMemory, 20, 0, p});
        r.rec(std::format("realloc 0 -> {}", ptr(r.r32(p))));
        r.ft("FT_Realloc", {kMemory, 0, 16, p});
        r.ft("FT_Realloc", {kMemory, sx(-1), 16, p});
        r.ftq("FT_Free", {kMemory, p});
        r.rec(std::format("free -> {}", ptr(r.r32(p))));
        r.ftq("FT_Free", {kMemory, p});
        r.rec("memory calls:" + r.heap_ops(m2));
    }, "lists");
}

TEST_CASE("cell_FreeType2: libraries and modules, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 out = r.alloc(16);
        r.ft("FT_New_Memory", {});
        r.ft("FT_Init_FreeType", {out});
        r.ft("FT_New_Library", {0, out});
        r.ft("FT_New_Library", {kMemory, out});
        const u32 lib = r.r32(out);
        const u32 v = r.alloc(12);
        r.ftq("FT_Library_Version", {lib, v, v + 4, v + 8});
        r.rec(std::format("version {}.{}.{}", r.r32(v), r.r32(v + 4), r.r32(v + 8)));
        r.ftq("FT_Library_Version", {0, v, 0, v + 8});
        r.rec(std::format("version of none {}.{}.{}", r.r32(v), r.r32(v + 4), r.r32(v + 8)));
        dump_library(r, lib, "new");
        r.ft("FT_Get_TrueType_Engine_Type", {lib});
        r.ft("FT_Get_TrueType_Engine_Type", {0});
        r.ftq("FT_Add_Default_Modules", {lib});
        dump_library(r, lib, "default modules");
        for (const char* n : {"truetype", "type1", "smooth", "raster1", "autofitter", "sfnt", "pcf", "bdf", "nothing"})
            r.rec(std::format("get {} -> {}", n, module_text(r, r.ftq("FT_Get_Module", {lib, r.str(n)}))));
        r.rec(std::format("get null name -> {}", module_text(r, r.ftq("FT_Get_Module", {lib, 0}))));
        r.rec(std::format("get in no library -> {}", module_text(r, r.ftq("FT_Get_Module", {0, r.str("truetype")}))));
        for (const u32 fmt : {kFormatOutline, kFormatBitmap, kFormatComposite, 0u})
            r.rec(std::format("renderer {:#x} -> {}", fmt, module_text(r, r.ftq("FT_Get_Renderer", {lib, fmt}))));
        r.ft("FT_Get_TrueType_Engine_Type", {lib});
        // a module removed and added again from its class
        const u32 t1 = r.ftq("FT_Get_Module", {lib, r.str("type1")});
        const u32 t1_class = r.r32(t1);
        r.ft("FT_Remove_Module", {lib, t1});
        dump_library(r, lib, "type1 removed");
        r.ft("FT_Remove_Module", {lib, 0});
        r.ft("FT_Remove_Module", {0, 0});
        r.ft("FT_Add_Module", {lib, t1_class});
        dump_library(r, lib, "type1 added");
        r.ft("FT_Add_Module", {lib, t1_class}); // the same version again
        r.ft("FT_Add_Module", {lib, 0});
        r.ft("FT_Add_Module", {0, t1_class});
        // a class of the game's own: requiring a newer FreeType, or older than the module of its name
        const u32 fake = r.alloc(64);
        r.w32(fake + 4, 0x40);
        r.w32(fake + 8, r.str("truetype"));
        r.w32(fake + 12, 0x10000);
        r.w32(fake + 16, 0x30000);
        r.ft("FT_Add_Module", {lib, fake});
        r.w32(fake + 16, 0x20000);
        r.ft("FT_Add_Module", {lib, fake});
        // renderers
        const u32 raster = r.ftq("FT_Get_Module", {lib, r.str("raster1")});
        r.ft("FT_Set_Renderer", {lib, raster, 0, 0});
        dump_library(r, lib, "raster1 set");
        r.ft("FT_Set_Renderer", {lib, 0, 0, 0});
        r.ft("FT_Set_Renderer", {0, raster, 0, 0});
        const u32 smooth = r.ftq("FT_Get_Module", {lib, r.str("smooth")});
        r.ft("FT_Set_Renderer", {lib, smooth, 0, 0});
        dump_library(r, lib, "smooth set");
        r.ftq("FT_Set_Debug_Hook", {lib, 1, 0x1234});
        r.ftq("FT_Set_Debug_Hook", {lib, 4, 0x1234});
        r.ftq("FT_Set_Debug_Hook", {lib, 2, 0});
        r.rec(std::format("hooks {:#x} {:#x} {:#x} {:#x}", r.r32(lib + 180), r.r32(lib + 184), r.r32(lib + 188), r.r32(lib + 192)));
        // the finalizers of the library's generic and of a module's
        const u32 fin = r.logger(9, 0, 1);
        r.w32(lib + 8, fin);
        const u32 tt = r.ftq("FT_Get_Module", {lib, r.str("truetype")});
        r.w32(tt + 16, fin);
        r.ft("FT_Done_Library", {lib});
        for (u32 i = 0; i < r.log_count(); i++)
            r.rec(std::format("finalizer {}", r.log_entry(i)[1] == lib ? "library" : r.log_entry(i)[1] == tt ? "truetype" : "?"));
        r.ft("FT_Done_Library", {0});
        // FT_Done_FreeType frees the memory record itself
        r.ft("FT_New_Library", {kMemory, out});
        const u32 mark = r.heap_mark();
        r.ft("FT_Done_FreeType", {r.r32(out)});
        const std::string ops = r.heap_ops(mark);
        r.rec(std::format("done freetype frees the memory: {}", ops.find("F?") != std::string::npos));
        r.ft("FT_Done_FreeType", {0});
    }, "library");
}

TEST_CASE("cell_FreeType2: faces and their tables, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 lib = new_library(r);
        for (const std::string& name : kLatinFonts) {
            const u32 face = open_face(r, lib, font(name));
            if (!face) continue;
            dump_face(r, face, name);
            r.rec("ps name " + r.cstr(r.ftq("FT_Get_Postscript_Name", {face})));
            r.rec("x11 format " + r.cstr(r.ftq("FT_Get_X11_Font_Format", {face})));
            r.ft("FT_Has_PS_Glyph_Names", {face});
            for (u32 tag = 0; tag < 8; tag++) {
                const u32 t = r.ftq("FT_Get_Sfnt_Table", {face, tag});
                static constexpr u32 sizes[] = {56, 32, 100, 44, 44, 32, 56, 0};
                if (t && (tag == 3 || tag == 4)) {
                    // the metrics arrays by what they hold
                    const u32 nl = r.r16(t + 34), longs = r.r32(t + 36), shorts = r.r32(t + 40);
                    r.rec(std::format("sfnt table {} -> {} longs {} shorts {}", tag, hex(r.b + t, 36),
                        longs ? hex(r.b + longs, 4 * nl) : "0", ptr(shorts)));
                } else {
                    r.rec(std::format("sfnt table {} -> {}", tag, t ? hex(r.b + t, sizes[tag]) : "0"));
                }
            }
            const u32 count = r.ft("FT_Get_Sfnt_Name_Count", {face});
            const u32 nm = r.alloc(16);
            for (u32 i = 0; i <= count; i++) {
                const u32 e = r.ftq("FT_Get_Sfnt_Name", {face, i, nm});
                r.rec(std::format("name {} -> {:#x} {}/{}/{}/{} {}", i, e, r.r16(nm), r.r16(nm + 2), r.r16(nm + 4), r.r16(nm + 6),
                    e ? "" : hex(r.b + r.r32(nm + 8), r.r32(nm + 12))));
            }
            const u32 tag = r.alloc(4), length = r.alloc(4);
            for (u32 i = 0; i < 30; i++) {
                r.w32(tag, 0);
                r.w32(length, 0);
                const u32 e = r.ftq("FT_Sfnt_Table_Info", {face, i, tag, length});
                r.rec(std::format("table info {} -> {:#x} {:#x} {}", i, e, r.r32(tag), r.r32(length)));
            }
            for (const u32 t : {0x68656164u, 0x6F732F32u, 0x6E616D65u, 0x636D6170u, 0x6B65726Eu, 0x0u, 0x41424344u}) {
                r.w32(length, 0);
                const u32 e = r.ftq("FT_Load_Sfnt_Table", {face, t, 0, 0, length});
                const u32 n = r.r32(length);
                r.rec(std::format("load table {:#x} length -> {:#x} {}", t, e, n));
                if (!e && n && n < 0x40000) {
                    const u32 buf = r.alloc(n);
                    r.ft("FT_Load_Sfnt_Table", {face, t, 0, buf, length});
                    r.rec(std::format("  bytes {:016x}", std::hash<std::string>{}(std::string(reinterpret_cast<char*>(r.b + buf), n))));
                    r.w32(length, 8);
                    r.ft("FT_Load_Sfnt_Table", {face, t, 4, buf, length});
                    r.rec("  8 at 4: " + hex(r.b + buf, 8));
                }
            }
            // charmaps
            const s32 ncm = r.s32at(face + 36);
            for (s32 i = 0; i < ncm; i++) {
                const u32 cm = r.r32(r.r32(face + 40) + 4 * static_cast<u32>(i));
                r.ft("FT_Get_Charmap_Index", {cm});
                r.ft("FT_Get_CMap_Language_ID", {cm});
                r.ft("FT_Set_Charmap", {face, cm});
                r.rec(std::format("charmap now {}", r.r32(face + 92) == cm));
                std::string s = "indices";
                for (u32 c = 0; c < 0x180; c += 3) s += std::format(" {}", r.ftq("FT_Get_Char_Index", {face, c}));
                for (const u32 c : {0x3042u, 0x30A2u, 0x4E00u, 0xAC00u, 0xFFFFu, 0x10000u, 0xFFFFFFFFu})
                    s += std::format(" {}", r.ftq("FT_Get_Char_Index", {face, c}));
                r.rec(s);
                const u32 gi = r.alloc(4);
                std::string it = std::format("first {:#x}", r.ftq("FT_Get_First_Char", {face, gi}));
                it += std::format("/{}", r.r32(gi));
                u32 c = r.ftq("FT_Get_First_Char", {face, gi});
                for (u32 k = 0; k < 64 && r.r32(gi); k++) {
                    c = r.ftq("FT_Get_Next_Char", {face, c, gi});
                    it += std::format(" {:#x}/{}", c, r.r32(gi));
                }
                r.rec(it);
            }
            r.ft("FT_Set_Charmap", {face, 0});
            r.ft("FT_Set_Charmap", {face, face});
            for (const u32 enc : {0x756E6963u, 0x61726D6Eu, 0x73796D62u, 0x736A6973u, 0x0u, 0x41424344u}) {
                r.ft("FT_Select_Charmap", {face, enc});
                r.rec(std::format("charmap encoding {:#x}", r.r32(face + 92) ? r.r32(r.r32(face + 92) + 4) : 0));
            }
            r.ft("FT_Select_Charmap", {face, 0x756E6963u});
            // glyph names and indices
            const u32 buf = r.alloc(64);
            for (const u32 gi : {0u, 1u, 3u, 36u, 68u, 100u, 0xFFFFu}) {
                std::memset(r.b + buf, 0x77, 64);
                r.ft("FT_Get_Glyph_Name", {face, gi, buf, 32});
                r.rec("  name " + hex(r.b + buf, 32));
            }
            r.ft("FT_Get_Glyph_Name", {face, 36, buf, 0});
            for (const char* n : {"A", ".notdef", "space", "uni3042", "nothing"}) r.ft("FT_Get_Name_Index", {face, r.str(n)});
            // other formats' queries on a TrueType face
            const u32 big = r.alloc(256);
            r.ft("FT_Get_PS_Font_Info", {face, big});
            r.ft("FT_Get_PS_Font_Private", {face, big});
            r.ft("FT_Get_WinFNT_Header", {face, big});
            const u32 a = r.alloc(8);
            r.ft("FT_Get_BDF_Charset_ID", {face, a, a + 4});
            r.ft("FT_Get_BDF_Property", {face, r.str("FONT"), a});
            r.ft("FT_Get_PFR_Metrics", {face, big, big + 4, big + 8, big + 12});
            r.rec(std::format("pfr metrics {} {} {:#x} {:#x}", r.r32(big), r.r32(big + 4), r.r32(big + 8), r.r32(big + 12)));
            r.ft("FT_Get_PFR_Kerning", {face, 36, 37, big});
            r.rec(std::format("pfr kerning {} {}", r.s32at(big), r.s32at(big + 4)));
            r.ft("FT_Get_PFR_Advance", {face, 36, big});
            r.ft("FT_Get_Multi_Master", {face, big});
            r.ft("FT_Get_MM_Var", {face, a});
            r.ft("FT_Set_MM_Design_Coordinates", {face, 1, big});
            r.ft("FT_Set_MM_Blend_Coordinates", {face, 1, big});
            r.ft("FT_Set_Var_Design_Coordinates", {face, 1, big});
            r.ft("FT_Set_Var_Blend_Coordinates", {face, 1, big});
            r.ft("FT_OpenType_Validate", {face, 0xFF00, big, big + 4, big + 8, big + 12, big + 16});
            r.ft("FT_Get_Track_Kerning", {face, 0x100000, 1, big});
            r.ft("FT_Done_Face", {face});
        }
        // the null and unknown cases
        const u32 out = r.alloc(8);
        r.ft("FT_New_Face", {0, r.str(font(kLatinFonts[0])), 0, out});
        r.ft("FT_New_Face", {lib, 0, 0, out});
        r.ft("FT_New_Face", {lib, r.str(font(kLatinFonts[0])), 0, 0});
        r.ft("FT_New_Face", {lib, r.str("/dev_flash/data/font/NOTHING.TTF"), 0, out});
        r.ft("FT_New_Face", {lib, r.str("/dev_flash/data/font/SONY-CC"), 0, out});
        r.ft("FT_New_Face", {lib, r.str(font(kLatinFonts[0])), 1, out});
        r.ft("FT_New_Face", {lib, r.str(font(kLatinFonts[0])), sx(-1), out});
        dump_face(r, r.r32(out), "index -1");
        r.ft("FT_Done_Face", {r.r32(out)});
        r.ft("FT_New_Face", {lib, r.str(font(kLatinFonts[0])), sx(-1), 0});
        r.ft("FT_Done_Face", {0});
        r.ft("FT_Done_Library", {lib});
    }, "faces");
}

TEST_CASE("cell_FreeType2: sizes, glyph loading and rendering, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 lib = new_library(r);
        const u32 face = open_face(r, lib, font("SCE-PS3-RD-R-LATIN.TTF"));
        const u32 slot = r.r32(face + 84);
        // sizes
        r.ft("FT_Set_Char_Size", {face, 0, 16 * 64, 72, 72});
        dump_face(r, face, "16pt");
        r.ft("FT_Set_Char_Size", {face, 10 * 64 + 17, 0, 96, 0});
        dump_face(r, face, "10.27pt at 96dpi");
        r.ft("FT_Set_Char_Size", {face, 0, 0, 0, 0});
        dump_face(r, face, "0");
        r.ft("FT_Set_Pixel_Sizes", {face, 24, 0});
        dump_face(r, face, "24px");
        r.ft("FT_Set_Pixel_Sizes", {face, 13, 31});
        dump_face(r, face, "13x31px");
        const u32 req = r.alloc(20);
        for (u32 type = 0; type < 6; type++) {
            r.w32(req, type);
            r.w32(req + 4, 20 * 64 + 7);
            r.w32(req + 8, 18 * 64);
            r.w32(req + 12, 72);
            r.w32(req + 16, 100);
            r.ft("FT_Request_Size", {face, req});
            dump_face(r, face, std::format("request type {}", type));
        }
        r.w32(req + 4, static_cast<u32>(-5));
        r.ft("FT_Request_Size", {face, req});
        r.ft("FT_Request_Size", {face, 0});
        r.ft("FT_Request_Size", {0, req});
        r.ft("FT_Select_Size", {face, 0});
        r.ft("FT_Set_Char_Size", {0, 0, 16 * 64, 72, 72});
        // more sizes
        const u32 out = r.alloc(8);
        r.ft("FT_New_Size", {face, out});
        const u32 size2 = r.r32(out);
        dump_face(r, face, "second size");
        r.ft("FT_Activate_Size", {size2});
        r.ft("FT_Set_Pixel_Sizes", {face, 40, 40});
        dump_face(r, face, "second size active");
        r.ft("FT_New_Size", {face, 0});
        r.ft("FT_New_Size", {0, out});
        r.ft("FT_Activate_Size", {0});
        r.ft("FT_Done_Size", {size2});
        dump_face(r, face, "second size done");
        r.ft("FT_Done_Size", {0});
        // loading
        r.ft("FT_Set_Char_Size", {face, 0, 14 * 64, 72, 72});
        const std::vector<u32> flags = {FT_LOAD_DEFAULT, FT_LOAD_NO_HINTING, FT_LOAD_NO_SCALE, FT_LOAD_RENDER,
            FT_LOAD_RENDER | FT_LOAD_MONOCHROME | FT_LOAD_TARGET_MONO, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT,
            FT_LOAD_RENDER | FT_LOAD_TARGET_LCD, FT_LOAD_RENDER | FT_LOAD_TARGET_LCD_V, FT_LOAD_FORCE_AUTOHINT,
            FT_LOAD_NO_AUTOHINT, FT_LOAD_VERTICAL_LAYOUT, FT_LOAD_LINEAR_DESIGN, FT_LOAD_NO_BITMAP | FT_LOAD_PEDANTIC,
            FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH, FT_LOAD_NO_RECURSE, FT_LOAD_IGNORE_TRANSFORM | FT_LOAD_RENDER};
        const std::vector<u32> chars = {'A', 'g', '@', ' ', 0xE9, '1', 'W', 0xC5};
        for (const u32 f : flags) {
            for (const u32 c : chars) {
                r.ft("FT_Load_Char", {face, c, f});
                dump_slot(r, slot, std::format("char {:#x} flags {:#x}", c, f));
            }
        }
        for (const u32 gi : {0u, 1u, 2u, 50u, 200u, 0xFFFFu}) {
            r.ft("FT_Load_Glyph", {face, gi, FT_LOAD_DEFAULT});
            dump_slot(r, slot, std::format("glyph {}", gi));
        }
        r.ft("FT_Load_Glyph", {0, 1, 0});
        r.ft("FT_Load_Char", {0, 'A', 0});
        // rendering what was loaded, in each mode
        for (u32 mode = 0; mode < 6; mode++) {
            r.ft("FT_Load_Char", {face, 'R', FT_LOAD_DEFAULT});
            r.ft("FT_Render_Glyph", {slot, mode});
            dump_slot(r, slot, std::format("render mode {}", mode));
            dump_library(r, lib, "after the render");
        }
        r.ft("FT_Render_Glyph", {slot, 0}); // a bitmap already
        r.ft("FT_Render_Glyph", {0, 0});
        // the guest's own changes to the outline are rendered
        r.ft("FT_Load_Char", {face, 'H', FT_LOAD_DEFAULT});
        const u32 pts = r.r32(slot + 108 + 4);
        for (s32 i = 0; i < r.s16at(slot + 110); i++) r.w32(pts + 8 * static_cast<u32>(i), r.r32(pts + 8 * static_cast<u32>(i)) + 50);
        r.ft("FT_Render_Glyph", {slot, 0});
        dump_slot(r, slot, "shifted H");
        // transforms
        const u32 m = r.alloc(16), d = r.alloc(8);
        r.w32(m, 0x10000);
        r.w32(m + 4, 0x4000);
        r.w32(m + 8, 0);
        r.w32(m + 12, 0x10000);
        r.w32(d, 100);
        r.w32(d + 4, static_cast<u32>(-37));
        r.ftq("FT_Set_Transform", {face, m, d});
        for (const u32 c : {'k', 'Q'}) {
            r.ft("FT_Load_Char", {face, c, FT_LOAD_RENDER});
            dump_slot(r, slot, "italic");
            r.ft("FT_Load_Char", {face, c, FT_LOAD_RENDER | FT_LOAD_IGNORE_TRANSFORM});
            dump_slot(r, slot, "italic ignored");
        }
        r.ftq("FT_Set_Transform", {face, 0, d});
        r.ft("FT_Load_Char", {face, 'k', FT_LOAD_RENDER});
        dump_slot(r, slot, "delta only");
        r.ftq("FT_Set_Transform", {face, 0, 0});
        r.ftq("FT_Set_Transform", {0, 0, 0});
        // kerning
        const u32 k = r.alloc(8);
        for (const u32 pair : {0x4156u, 0x5641u, 0x546Fu, 0x4141u}) {
            const u32 lgi = r.ftq("FT_Get_Char_Index", {face, pair >> 8}), rgi = r.ftq("FT_Get_Char_Index", {face, pair & 0xFF});
            for (u32 mode = 0; mode < 3; mode++) {
                r.ft("FT_Get_Kerning", {face, lgi, rgi, mode, k});
                r.rec(std::format("kerning {:#x} mode {} -> {} {}", pair, mode, r.s32at(k), r.s32at(k + 4)));
            }
        }
        r.ft("FT_Get_Kerning", {face, 1, 2, 0, 0});
        r.ft("FT_Get_Kerning", {0, 1, 2, 0, k});
        // subglyphs
        const u32 sg = r.alloc(32);
        r.ft("FT_Load_Char", {face, 0xC5, FT_LOAD_NO_RECURSE});
        dump_slot(r, slot, "no recurse");
        r.ft("FT_Get_SubGlyph_Info", {slot, 0, sg, sg + 4, sg + 8, sg + 12, sg + 16});
        r.rec("subglyph 0 " + hex(r.b + sg, 32));
        r.ft("FT_Get_SubGlyph_Info", {slot, 5, sg, sg + 4, sg + 8, sg + 12, sg + 16});
        r.ft("FT_Get_SubGlyph_Info", {0, 0, sg, sg + 4, sg + 8, sg + 12, sg + 16});
        // other fonts, CJK ones too
        for (const auto& [name, cs] : std::vector<std::pair<std::string, std::vector<u32>>>{
                 {"SCE-PS3-NR-R-JPN.TTF", {0x3042, 0x6F22, 0x5B57, 'A', 0xFF21}},
                 {"SCE-PS3-YG-R-KOR.TTF", {0xAC00, 0xD55C, 'a'}},
                 {"SCE-PS3-MT-BI-LATIN.TTF", {'f', 'Q', 0xDF}},
                 {"SCE-PS3-CP-R-KANA.TTF", {0x30A2, 0x30F3}}}) {
            const u32 f2 = open_face(r, lib, font(name));
            if (!f2) continue;
            dump_face(r, f2, name);
            const u32 s2 = r.r32(f2 + 84);
            for (const u32 px : {12u, 22u}) {
                r.ft("FT_Set_Pixel_Sizes", {f2, 0, px});
                for (const u32 c : cs) {
                    for (const u32 f : {FT_LOAD_RENDER, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP, FT_LOAD_NO_HINTING}) {
                        r.ft("FT_Load_Char", {f2, c, f});
                        dump_slot(r, s2, std::format("{} {:#x} {}px {:#x}", name, c, px, f));
                    }
                }
            }
            r.ft("FT_Done_Face", {f2});
        }
        r.ft("FT_Done_Face", {face});
        r.ft("FT_Done_Library", {lib});
    }, "glyphs");
}

namespace {

// An outline of the scripts' own: `np` points and `nc` contours from `seed`.
u32 make_outline(Run& r, u32 np, u32 nc, u32 seed, bool sane = true)
{
    std::mt19937 rng(seed);
    const u32 o = r.alloc(20), pts = r.alloc(8 * np), tags = r.alloc(np), cts = r.alloc(2 * nc);
    r.w16(o, static_cast<u16>(nc));
    r.w16(o + 2, static_cast<u16>(np));
    r.w32(o + 4, pts);
    r.w32(o + 8, tags);
    r.w32(o + 12, cts);
    for (u32 i = 0; i < np; i++) {
        r.w32(pts + 8 * i, static_cast<u32>(static_cast<s32>(rng() % 2000) - 500));
        r.w32(pts + 8 * i + 4, static_cast<u32>(static_cast<s32>(rng() % 2000) - 500));
        u32 tag = rng() % 3 == 0 ? 0 : 1;
        if (!sane && rng() % 5 == 0) tag = 2;
        r.b[tags + i] = static_cast<u8>(tag);
    }
    for (u32 c = 0; c < nc; c++) r.w16(cts + 2 * c, static_cast<u16>((c + 1) * np / nc - 1));
    if (sane) {
        for (u32 c = 0; c < nc; c++) r.b[tags + (c ? (c * np / nc) : 0)] = 1; // contours start on the curve
    }
    return o;
}

void outline_ops(Run& r, u32 lib, u32 src, const std::string& what)
{
    const u32 np = r.r16(src + 2);
    const s32 nc = r.s16at(src);
    const u32 o = r.alloc(20);
    u32 mark = r.heap_mark();
    r.ft("FT_Outline_New", {lib, np, sx(nc), o});
    r.rec(what + " new:" + r.heap_ops(mark) + " " + outline_text(r, o, true).substr(0, 60));
    r.ft("FT_Outline_Copy", {src, o});
    r.rec(what + " copy " + outline_text(r, o));
    r.ft("FT_Outline_Check", {o});
    const u32 box = r.alloc(16);
    r.ftq("FT_Outline_Get_CBox", {o, box});
    r.rec(std::format("cbox {} {} {} {}", r.s32at(box), r.s32at(box + 4), r.s32at(box + 8), r.s32at(box + 12)));
    r.ft("FT_Outline_Get_BBox", {o, box});
    r.rec(std::format("bbox {} {} {} {}", r.s32at(box), r.s32at(box + 4), r.s32at(box + 8), r.s32at(box + 12)));
    r.ft("FT_Outline_Get_Orientation", {o});
    r.ft("FT_Outline_GetInsideBorder", {o});
    r.ft("FT_Outline_GetOutsideBorder", {o});
    // decomposition, as is and scaled
    const u32 funcs = r.alloc(24);
    r.w32(funcs, r.logger(1, 1, 1));
    r.w32(funcs + 4, r.logger(2, 1, 1));
    r.w32(funcs + 8, r.logger(3, 2, 1));
    r.w32(funcs + 12, r.logger(4, 3, 1));
    for (const auto& [shift, delta] : std::vector<std::pair<s32, s32>>{{0, 0}, {2, 5}, {6, -3}}) {
        r.clear_log();
        r.w32(funcs + 16, static_cast<u32>(shift));
        r.w32(funcs + 20, static_cast<u32>(delta));
        r.ft("FT_Outline_Decompose", {o, funcs, 0x77});
        std::string s = "decompose";
        for (u32 i = 0; i < r.log_count(); i++) {
            const auto e = r.log_entry(i);
            s += std::format(" {}:", e[0]);
            for (u32 k = 1; k < 8; k++) s += std::format("{},", static_cast<s32>(e[k]));
        }
        r.rec(s);
    }
    r.clear_log();
    r.set_answer(3, 0x55);
    r.ft("FT_Outline_Decompose", {o, funcs, 0});
    r.rec(std::format("decompose stopped after {}", r.log_count()));
    r.clear_log();
    // rendered into bitmaps of the scripts' own
    const u32 bm = r.alloc(24), pixels = r.alloc(64 * 64);
    for (const u32 mode : {2u, 1u}) {
        std::memset(r.b + pixels, 0, 64 * 64);
        r.w32(bm, 40);
        r.w32(bm + 4, 48);
        r.w32(bm + 8, mode == 2 ? 48 : 8);
        r.w32(bm + 12, pixels);
        r.w16(bm + 16, mode == 2 ? 256 : 2);
        r.b[bm + 18] = static_cast<u8>(mode);
        r.ft("FT_Outline_Get_Bitmap", {lib, o, bm});
        r.rec("get bitmap " + bitmap_text(r, bm));
        std::memset(r.b + pixels, 0, 64 * 64);
        const u32 params = r.alloc(48);
        r.w32(params, bm);
        r.w32(params + 8, mode == 2 ? 1 : 0);
        r.ft("FT_Outline_Render", {lib, o, params});
        r.rec(std::format("render source {} {}", r.r32(params + 4) == o, bitmap_text(r, bm)));
    }
    // direct rendering through spans, clipped
    const u32 params = r.alloc(48);
    r.w32(params + 8, 1 | 2 | 4);
    r.w32(params + 12, r.span_logger(7));
    r.w32(params + 28, 0x99);
    r.w32(params + 32, 0);
    r.w32(params + 36, 0);
    r.w32(params + 40, 30);
    r.w32(params + 44, 20);
    r.ft("FT_Outline_Render", {lib, o, params});
    std::string spans = "spans";
    for (u32 i = 0; i < r.log_count(); i++) {
        const auto e = r.log_entry(i);
        spans += std::format(" [{} y {} n {} user {:#x} {}]", e[0], static_cast<s32>(e[1]), e[2], e[3],
            hex(r.b + kLogCtl + 0x40 + 64 * i + 16, 6 * std::min<u32>(e[2], 8)));
    }
    r.rec(spans);
    r.clear_log();
    // in place
    r.ftq("FT_Outline_Translate", {o, 100, sx(-50)});
    r.rec("translated " + outline_text(r, o));
    const u32 m = r.alloc(16);
    r.w32(m, 0x10000);
    r.w32(m + 4, 0x5000);
    r.w32(m + 8, static_cast<u32>(-0x2000));
    r.w32(m + 12, 0xC000);
    r.ftq("FT_Outline_Transform", {o, m});
    r.rec("transformed " + outline_text(r, o));
    r.ftq("FT_Outline_Reverse", {o});
    r.rec("reversed " + outline_text(r, o));
    for (const s32 strength : {64, 1, -40, 300}) {
        r.ft("FT_Outline_Embolden", {o, sx(strength)});
        r.rec(std::format("emboldened {} ", strength) + outline_text(r, o));
    }
    mark = r.heap_mark();
    r.ft("FT_Outline_Done", {lib, o});
    r.rec(what + " done:" + r.heap_ops(mark) + " " + outline_text(r, o));
}

} // namespace

TEST_CASE("cell_FreeType2: outlines, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 lib = new_library(r);
        const u32 face = open_face(r, lib, font("SCE-PS3-RD-R-LATIN.TTF"));
        const u32 slot = r.r32(face + 84);
        r.ft("FT_Set_Pixel_Sizes", {face, 0, 40});
        for (const u32 c : {u32{'A'}, u32{'g'}, u32{'O'}, u32{'8'}, u32{'&'}, 0xE9u, u32{'%'}}) {
            r.ft("FT_Load_Char", {face, c, FT_LOAD_NO_BITMAP});
            outline_ops(r, lib, slot + 108, std::format("glyph {:#x}", c));
        }
        r.ft("FT_Load_Char", {face, 'B', FT_LOAD_NO_SCALE});
        outline_ops(r, lib, slot + 108, "unscaled B");
        for (u32 i = 0; i < 6; i++) outline_ops(r, lib, make_outline(r, 12 + 5 * i, 1 + i % 3, 100 + i), std::format("made {}", i));
        // what FreeType refuses
        const u32 o = r.alloc(20);
        r.ft("FT_Outline_New", {0, 10, 1, o});
        r.ft("FT_Outline_New", {lib, 10, 1, 0});
        u32 mark = r.heap_mark();
        r.ft("FT_Outline_New", {lib, 0, 0, o});
        r.rec("empty:" + r.heap_ops(mark) + " " + outline_text(r, o));
        r.ft("FT_Outline_Done", {lib, o});
        mark = r.heap_mark();
        r.ft("FT_Outline_New", {lib, 5, sx(-1), o});
        r.rec("negative contours:" + r.heap_ops(mark) + " " + outline_text(r, o));
        r.ft("FT_Outline_New", {lib, 0x7FFFFFFF, 2, o});
        r.rec("huge:" + r.heap_ops(mark));
        r.ft("FT_Outline_Done", {0, o});
        r.ft("FT_Outline_Done", {lib, 0});
        r.ft("FT_Outline_Check", {0});
        const u32 bad = make_outline(r, 10, 2, 7);
        r.w16(r.r32(bad + 12), 11);
        r.ft("FT_Outline_Check", {bad});
        r.w16(r.r32(bad + 12), 3);
        r.w16(r.r32(bad + 12) + 2, 2);
        r.ft("FT_Outline_Check", {bad});
        r.ft("FT_Outline_Copy", {bad, make_outline(r, 9, 2, 8)});
        r.ft("FT_Outline_Copy", {0, bad});
        r.ft("FT_Outline_Copy", {bad, bad});
        r.ft("FT_Outline_Embolden", {0, 64});
        r.ft("FT_Outline_Get_Orientation", {0});
        r.ft("FT_Outline_Get_BBox", {0, o});
        r.ftq("FT_Outline_Transform", {0, 0});
        r.ftq("FT_Outline_Reverse", {0});
        r.ftq("FT_Outline_Get_CBox", {0, o});
        r.ft("FT_Outline_Decompose", {bad, 0, 0});
        r.ft("FT_Outline_Decompose", {0, bad, 0});
        r.ft("FT_Outline_Get_Bitmap", {lib, bad, 0});
        r.ft("FT_Outline_Get_Bitmap", {0, bad, o});
        r.ft("FT_Outline_Render", {lib, 0, o});
        r.ft("FT_Outline_Render", {lib, bad, 0});
        r.ft("FT_Outline_Render", {0, bad, o});
        // outlines FT_Outline_Decompose finds broken (a cubic first, a lone cubic)
        const u32 funcs = r.alloc(24);
        for (u32 k = 0; k < 4; k++) r.w32(funcs + 4 * k, r.logger(10 + k, 1 + (k >= 2 ? k - 1 : 0), 1));
        for (u32 seed = 0; seed < 8; seed++) {
            r.clear_log();
            const u32 w = make_outline(r, 9 + seed, 1 + seed % 2, 300 + seed, false);
            r.ft("FT_Outline_Decompose", {w, funcs, 0});
            r.rec(std::format("weird {} calls {}", seed, r.log_count()));
            r.ft("FT_Outline_Get_Orientation", {w});
            r.ft("FT_Outline_Embolden", {w, 30});
            r.rec(outline_text(r, w));
        }
        r.ft("FT_Done_Face", {face});
        r.ft("FT_Done_Library", {lib});
    }, "outlines");
}

TEST_CASE("cell_FreeType2: bitmaps and glyph images, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 lib = new_library(r);
        const u32 face = open_face(r, lib, font("SCE-PS3-RD-B-LATIN.TTF"));
        const u32 slot = r.r32(face + 84);
        r.ft("FT_Set_Pixel_Sizes", {face, 0, 18});
        const u32 gp = r.alloc(8), gp2 = r.alloc(8);
        // glyph images from outlines
        for (const u32 c : {u32{'K'}, u32{'q'}, 0xC6u}) {
            r.ft("FT_Load_Char", {face, c, FT_LOAD_DEFAULT});
            u32 mark = r.heap_mark();
            r.ft("FT_Get_Glyph", {slot, gp});
            const u32 g = r.r32(gp);
            r.rec("get:" + r.heap_ops(mark) + " " + glyph_text(r, g));
            mark = r.heap_mark();
            r.ft("FT_Glyph_Copy", {g, gp2});
            r.rec("copy:" + r.heap_ops(mark) + " " + glyph_text(r, r.r32(gp2)));
            const u32 box = r.alloc(16);
            for (u32 mode = 0; mode < 4; mode++) {
                r.ftq("FT_Glyph_Get_CBox", {g, mode, box});
                r.rec(std::format("cbox {} {} {} {} {}", mode, r.s32at(box), r.s32at(box + 4), r.s32at(box + 8), r.s32at(box + 12)));
            }
            const u32 m = r.alloc(16), d = r.alloc(8);
            r.w32(m, 0xE000);
            r.w32(m + 4, 0x3000);
            r.w32(m + 8, 0);
            r.w32(m + 12, 0x11000);
            r.w32(d, 37);
            r.w32(d + 4, static_cast<u32>(-21));
            r.ft("FT_Glyph_Transform", {r.r32(gp2), m, d});
            r.rec("transformed " + glyph_text(r, r.r32(gp2)));
            r.ft("FT_Glyph_Transform", {r.r32(gp2), 0, 0});
            for (u32 mode = 0; mode < 5; mode++) {
                for (const bool destroy : {false, true}) {
                    for (const bool origin : {false, true}) {
                        mark = r.heap_mark();
                        r.ft("FT_Glyph_Copy", {g, gp2});
                        const u32 before = r.r32(gp2);
                        r.ft("FT_Glyph_To_Bitmap", {gp2, mode, origin ? d : 0, destroy ? 1u : 0u});
                        r.rec(std::format("to bitmap {} {} {}:", mode, destroy, origin) + r.heap_ops(mark) + " " +
                            glyph_text(r, r.r32(gp2)) + (destroy ? "" : " source " + glyph_text(r, before)));
                        dump_library(r, lib, "renderers");
                        mark = r.heap_mark();
                        r.ftq("FT_Done_Glyph", {r.r32(gp2)});
                        if (!destroy && r.r32(gp2) != before) r.ftq("FT_Done_Glyph", {before});
                        r.rec("done:" + r.heap_ops(mark));
                    }
                }
            }
            r.ft("FT_Glyph_To_Bitmap", {gp, 2, 0, 0}); // to mono
            r.ft("FT_Glyph_To_Bitmap", {gp, 0, 0, 1}); // a bitmap already
            r.rec(glyph_text(r, r.r32(gp)));
            mark = r.heap_mark();
            r.ftq("FT_Done_Glyph", {r.r32(gp)});
            r.rec("done:" + r.heap_ops(mark));
        }
        // a rendered slot's bitmap goes to the glyph; FT_GlyphSlot_Own_Bitmap copies it back
        r.ft("FT_Load_Char", {face, 'W', FT_LOAD_RENDER});
        u32 mark = r.heap_mark();
        r.ft("FT_Get_Glyph", {slot, gp});
        const u32 g = r.r32(gp);
        r.rec(std::format("bitmap glyph:{} same buffer {} ", r.heap_ops(mark), r.r32(g + 28 + 12) == r.r32(slot + 76 + 12)) +
            glyph_text(r, g));
        mark = r.heap_mark();
        r.ft("FT_GlyphSlot_Own_Bitmap", {slot});
        r.rec(std::format("own:{} same buffer {}", r.heap_ops(mark), r.r32(g + 28 + 12) == r.r32(slot + 76 + 12)));
        r.ft("FT_GlyphSlot_Own_Bitmap", {slot});
        dump_slot(r, slot, "owned");
        r.ft("FT_Get_Glyph", {slot, gp2});
        mark = r.heap_mark();
        r.ft("FT_Load_Char", {face, 'X', FT_LOAD_RENDER});
        dump_slot(r, slot, "next");
        r.ft("FT_Get_Glyph", {slot, gp2});
        r.rec("glyph of a bitmap " + glyph_text(r, r.r32(gp2)));
        mark = r.heap_mark();
        r.ft("FT_Glyph_Copy", {r.r32(gp2), gp});
        r.rec("copy of a bitmap:" + r.heap_ops(mark) + " " + glyph_text(r, r.r32(gp)));
        const u32 box = r.alloc(16);
        for (u32 mode = 0; mode < 4; mode++) {
            r.ftq("FT_Glyph_Get_CBox", {r.r32(gp), mode, box});
            r.rec(std::format("bitmap cbox {} {} {} {} {}", mode, r.s32at(box), r.s32at(box + 4), r.s32at(box + 8), r.s32at(box + 12)));
        }
        r.ft("FT_Glyph_Transform", {r.r32(gp), 0, 0});
        mark = r.heap_mark();
        r.ftq("FT_Done_Glyph", {r.r32(gp)});
        r.ftq("FT_Done_Glyph", {r.r32(gp2)});
        r.ftq("FT_Done_Glyph", {g});
        r.rec("done:" + r.heap_ops(mark));
        r.ft("FT_Get_Glyph", {0, gp});
        r.ft("FT_Get_Glyph", {slot, 0});
        r.ft("FT_Glyph_Copy", {0, gp});
        r.ft("FT_Glyph_Copy", {g, 0});
        r.ft("FT_Glyph_Transform", {0, 0, 0});
        r.ftq("FT_Glyph_Get_CBox", {0, 0, box});
        r.rec(std::format("cbox of none {} {} {} {}", r.s32at(box), r.s32at(box + 4), r.s32at(box + 8), r.s32at(box + 12)));
        r.ft("FT_Glyph_To_Bitmap", {0, 0, 0, 0});
        r.ftq("FT_Done_Glyph", {0});
        // bitmaps of the scripts' own, through the library's memory
        const u32 b1 = r.alloc(24), b2 = r.alloc(24), b3 = r.alloc(24);
        for (const u32 flags : {FT_LOAD_RENDER, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO, FT_LOAD_RENDER | FT_LOAD_TARGET_LCD,
                 FT_LOAD_RENDER | FT_LOAD_TARGET_LCD_V}) {
            r.ft("FT_Load_Char", {face, 'M', flags});
            r.ftq("FT_Bitmap_New", {b1});
            r.rec("new " + bitmap_text(r, b1));
            mark = r.heap_mark();
            r.ft("FT_Bitmap_Copy", {lib, slot + 76, b1});
            r.rec("copy:" + r.heap_ops(mark) + " " + bitmap_text(r, b1));
            r.ft("FT_Bitmap_Copy", {lib, slot + 76, b1});
            r.ft("FT_Bitmap_Copy", {lib, b1, b1});
            for (const auto& [x, y] : std::vector<std::pair<s32, s32>>{{64, 64}, {64, 0}, {0, 128}, {200, 30}, {0, 0}, {-64, 64}}) {
                r.ftq("FT_Bitmap_New", {b2});
                r.ft("FT_Bitmap_Copy", {lib, b1, b2});
                mark = r.heap_mark();
                r.ft("FT_Bitmap_Embolden", {lib, b2, sx(x), sx(y)});
                r.rec(std::format("embolden {} {}:", x, y) + r.heap_ops(mark) + " " + bitmap_text(r, b2));
                r.ft("FT_Bitmap_Done", {lib, b2});
            }
            for (const s32 align : {0, 1, 4, 7, -2}) {
                r.ftq("FT_Bitmap_New", {b3});
                mark = r.heap_mark();
                r.ft("FT_Bitmap_Convert", {lib, b1, b3, sx(align)});
                r.rec(std::format("convert {}:", align) + r.heap_ops(mark) + " " + bitmap_text(r, b3));
                r.ft("FT_Bitmap_Convert", {lib, b1, b3, sx(align)});
                r.ft("FT_Bitmap_Done", {lib, b3});
            }
            mark = r.heap_mark();
            r.ft("FT_Bitmap_Done", {lib, b1});
            r.rec("done:" + r.heap_ops(mark) + " " + bitmap_text(r, b1));
        }
        // 2- and 4-bit gray bitmaps
        for (const u8 mode : {3, 4}) {
            const u32 px = r.alloc(64);
            for (u32 i = 0; i < 64; i++) r.b[px + i] = static_cast<u8>(i * 37 + mode);
            r.w32(b1, 8);
            r.w32(b1 + 4, 13);
            r.w32(b1 + 8, mode == 3 ? 4 : 7);
            r.w32(b1 + 12, px);
            r.w16(b1 + 16, mode == 3 ? 4 : 16);
            r.b[b1 + 18] = mode;
            r.ftq("FT_Bitmap_New", {b3});
            r.ft("FT_Bitmap_Convert", {lib, b1, b3, 3});
            r.rec("converted " + bitmap_text(r, b3));
            r.ftq("FT_Bitmap_New", {b2});
            r.ft("FT_Bitmap_Copy", {lib, b1, b2});
            mark = r.heap_mark();
            r.ft("FT_Bitmap_Embolden", {lib, b2, 128, 64});
            r.rec("emboldened:" + r.heap_ops(mark) + " " + bitmap_text(r, b2));
        }
        r.b[b1 + 18] = 7;
        r.ft("FT_Bitmap_Convert", {lib, b1, b3, 1});
        r.ft("FT_Bitmap_Embolden", {lib, b1, 64, 64});
        r.ft("FT_Bitmap_Embolden", {0, b1, 64, 64});
        r.ft("FT_Bitmap_Embolden", {lib, 0, 64, 64});
        r.ft("FT_Bitmap_Convert", {0, b1, b3, 1});
        r.ft("FT_Bitmap_Done", {0, b1});
        r.ft("FT_Bitmap_Done", {lib, 0});
        // the slot's own bitmap emboldened in place
        r.ft("FT_Load_Char", {face, 'e', FT_LOAD_RENDER});
        mark = r.heap_mark();
        r.ft("FT_Bitmap_Embolden", {lib, slot + 76, 64, 64});
        r.rec("slot emboldened:" + r.heap_ops(mark));
        dump_slot(r, slot, "emboldened");
        // (a load's own allocations are FreeType's internals: Sony's go through the game's memory, pspocket's do not)
        r.ft("FT_Load_Char", {face, 'f', FT_LOAD_RENDER});
        dump_slot(r, slot, "next load");
        r.ft("FT_Done_Face", {face});
        r.ft("FT_Done_Library", {lib});
    }, "bitmaps and glyphs");
}

TEST_CASE("cell_FreeType2: the stroker, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 lib = new_library(r);
        const u32 face = open_face(r, lib, font("SCE-PS3-RD-R-LATIN.TTF"));
        const u32 slot = r.r32(face + 84);
        r.ft("FT_Set_Pixel_Sizes", {face, 0, 32});
        const u32 sp = r.alloc(8), gp = r.alloc(8), gp2 = r.alloc(8), counts = r.alloc(8);
        u32 mark = r.heap_mark();
        r.ft("FT_Stroker_New", {lib, sp});
        r.rec("new stroker allocates " + std::to_string(r.heap_count() - mark));
        r.ft("FT_Stroker_New", {0, sp});
        const u32 s = r.r32(sp);
        r.ft("FT_Stroker_New", {lib, sp});
        r.w32(sp, s);
        for (const u32 c : {u32{'S'}, u32{'b'}, u32{'4'}}) {
            r.ft("FT_Load_Char", {face, c, FT_LOAD_DEFAULT});
            r.ft("FT_Get_Glyph", {slot, gp});
            for (u32 cap = 0; cap < 3; cap++) {
                for (u32 join = 0; join < 3; join++) {
                    r.ftq("FT_Stroker_Set", {s, 48 + 16 * cap, cap, join, 0x20000 + 0x8000 * join});
                    // (the stroker's own arrays are in the game's memory in Sony's, the host's in pspocket's: the
                    // allocations are not compared here)
                    r.ft("FT_Glyph_Copy", {r.r32(gp), gp2});
                    r.ft("FT_Glyph_Stroke", {gp2, s, 1});
                    r.rec(std::format("stroke {} {}: ", cap, join) + glyph_text(r, r.r32(gp2)));
                    r.ftq("FT_Done_Glyph", {r.r32(gp2)});
                    for (const u32 inside : {0u, 1u}) {
                        r.ft("FT_Glyph_Copy", {r.r32(gp), gp2});
                        const u32 before = r.r32(gp2);
                        r.ft("FT_Glyph_StrokeBorder", {gp2, s, inside, 0});
                        r.rec(std::format("border {}: ", inside) + glyph_text(r, r.r32(gp2)));
                        r.ftq("FT_Done_Glyph", {r.r32(gp2)});
                        r.ftq("FT_Done_Glyph", {before});
                    }
                }
            }
            r.ft("FT_Stroker_ParseOutline", {s, slot + 108, 0});
            r.ft("FT_Stroker_GetCounts", {s, counts, counts + 4});
            r.rec(std::format("counts {} {}", r.r32(counts), r.r32(counts + 4)));
            for (u32 border = 0; border < 3; border++) {
                r.ft("FT_Stroker_GetBorderCounts", {s, border, counts, counts + 4});
                r.rec(std::format("border {} counts {} {}", border, r.r32(counts), r.r32(counts + 4)));
            }
            const u32 o = r.alloc(20);
            r.ft("FT_Stroker_GetCounts", {s, counts, counts + 4});
            r.ft("FT_Outline_New", {lib, r.r32(counts) + 4, r.r32(counts + 4) + 1, o});
            r.w16(o, 0);
            r.w16(o + 2, 0);
            r.ftq("FT_Stroker_Export", {s, o});
            r.rec("exported " + outline_text(r, o));
            r.w16(o, 0);
            r.w16(o + 2, 0);
            r.ftq("FT_Stroker_ExportBorder", {s, 1, o});
            r.rec("exported right " + outline_text(r, o));
            r.ftq("FT_Stroker_ExportBorder", {s, 0, o});
            r.rec("and left " + outline_text(r, o));
            r.ftq("FT_Stroker_ExportBorder", {s, 2, o});
            r.ft("FT_Outline_Done", {lib, o});
            r.ft("FT_Stroker_ParseOutline", {s, slot + 108, 1});
            r.ft("FT_Stroker_GetCounts", {s, counts, counts + 4});
            r.rec(std::format("opened counts {} {}", r.r32(counts), r.r32(counts + 4)));
            r.ftq("FT_Done_Glyph", {r.r32(gp)});
        }
        // a path drawn by hand
        const u32 v = r.alloc(32);
        auto put = [&](u32 at, s32 x, s32 y) {
            r.w32(at, static_cast<u32>(x));
            r.w32(at + 4, static_cast<u32>(y));
        };
        for (const u32 open : {0u, 1u}) {
            r.ftq("FT_Stroker_Rewind", {s});
            r.ftq("FT_Stroker_Set", {s, 64, 1, 0, 0x10000});
            put(v, 0, 0);
            r.ft("FT_Stroker_BeginSubPath", {s, v, open});
            put(v, 640, 0);
            r.ft("FT_Stroker_LineTo", {s, v});
            put(v, 900, 300);
            put(v + 8, 640, 640);
            r.ft("FT_Stroker_ConicTo", {s, v, v + 8});
            put(v, 300, 900);
            put(v + 8, -200, 700);
            put(v + 16, 0, 320);
            r.ft("FT_Stroker_CubicTo", {s, v, v + 8, v + 16});
            put(v, 0, 320);
            r.ft("FT_Stroker_LineTo", {s, v});
            r.ft("FT_Stroker_EndSubPath", {s});
            r.ft("FT_Stroker_GetCounts", {s, counts, counts + 4});
            r.rec(std::format("drawn {} counts {} {}", open, r.r32(counts), r.r32(counts + 4)));
            const u32 o = r.alloc(20);
            r.ft("FT_Outline_New", {lib, r.r32(counts), r.r32(counts + 4), o});
            r.w16(o, 0);
            r.w16(o + 2, 0);
            r.ftq("FT_Stroker_Export", {s, o});
            r.rec("drawn " + outline_text(r, o));
            r.ft("FT_Outline_Done", {lib, o});
        }
        r.ft("FT_Stroker_GetBorderCounts", {0, 0, counts, counts + 4});
        r.ft("FT_Stroker_ParseOutline", {s, 0, 0});
        r.ft("FT_Stroker_ParseOutline", {0, slot + 108, 0});
        r.ft("FT_Glyph_Stroke", {0, s, 0});
        r.ft("FT_Load_Char", {face, 'x', FT_LOAD_RENDER});
        r.ft("FT_Get_Glyph", {slot, gp});
        r.ft("FT_Glyph_Stroke", {gp, s, 0});
        r.ft("FT_Glyph_StrokeBorder", {gp, s, 0, 0});
        r.ftq("FT_Done_Glyph", {r.r32(gp)});
        mark = r.heap_mark();
        r.ftq("FT_Stroker_Done", {s});
        r.rec(std::format("stroker done frees its record {}", r.heap_count() > mark)); // (and in Sony's its arrays)
        r.ftq("FT_Stroker_Done", {0});
        r.ft("FT_Done_Face", {face});
        r.ft("FT_Done_Library", {lib});
    }, "stroker");
}

namespace {

u32 crc32_of(const std::vector<u8>& d)
{
    u32 c = 0xFFFFFFFF;
    for (const u8 b : d) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// A gzip file of `d` in stored deflate blocks.
std::vector<u8> gzip_stored(const std::vector<u8>& d)
{
    std::vector<u8> o = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};
    usize at = 0;
    do {
        const usize n = std::min<usize>(d.size() - at, 0xFFFF);
        const bool last = at + n == d.size();
        o.push_back(last ? 1 : 0);
        o.push_back(static_cast<u8>(n));
        o.push_back(static_cast<u8>(n >> 8));
        o.push_back(static_cast<u8>(~n));
        o.push_back(static_cast<u8>(~n >> 8));
        o.insert(o.end(), d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < d.size());
    const u32 crc = crc32_of(d), size = static_cast<u32>(d.size());
    for (int k = 0; k < 4; k++) o.push_back(static_cast<u8>(crc >> (8 * k)));
    for (int k = 0; k < 4; k++) o.push_back(static_cast<u8>(size >> (8 * k)));
    return o;
}

// A .Z file (compress(1), 16 bits, block mode) of `d`, its codes packed least significant bit first, as FreeType's
// decoder reads them.
std::vector<u8> lzw_compress(const std::vector<u8>& d)
{
    std::vector<u8> o = {0x1F, 0x9D, 16 | 0x80};
    u64 pad = 0;
    u32 pad_bits = 0;
    auto put = [&](u32 code, u32 bits) {
        pad |= static_cast<u64>(code) << pad_bits;
        pad_bits += bits;
        while (pad_bits >= 8) {
            o.push_back(static_cast<u8>(pad));
            pad >>= 8;
            pad_bits -= 8;
        }
    };
    std::map<std::pair<u32, u8>, u32> dict;
    u32 next = 257, bits = 9, free_ent = 1, free_bits = 256, codes = 0;
    const u32 max_free = (1u << 16) - 256;
    auto width_step = [&] {
        // the decoder makes an entry after each code but the first
        if (codes >= 1 && free_ent < max_free) {
            if (++free_ent == free_bits) {
                bits++;
                free_bits = bits < 16 ? (1u << bits) - 256 : max_free + 1;
            }
        }
    };
    if (d.empty()) return o;
    u32 w = d[0];
    for (usize i = 1; i < d.size(); i++) {
        const u8 c = d[i];
        auto it = dict.find({w, c});
        if (it != dict.end()) {
            w = it->second;
            continue;
        }
        if (codes >= 2) width_step();
        put(w, bits);
        codes++;
        if (next < 65536) dict[{w, c}] = next++;
        w = c;
    }
    if (codes >= 2) width_step();
    put(w, bits);
    if (pad_bits) o.push_back(static_cast<u8>(pad));
    return o;
}

// A guest FT_StreamRec over data in guest memory: with `read` its read function is the scripts' (over the descriptor),
// else a memory stream.
u32 guest_stream(Run& r, u32 data, u32 size, bool read)
{
    const u32 s = r.alloc(40);
    r.w32(s + 4, size);
    r.w32(s + 28, kMemory);
    if (read) {
        r.w32(s + 12, data);
        r.w32(s + 20, r.stream_read());
    } else {
        r.w32(s, data);
    }
    return s;
}

void face_glyphs(Run& r, u32 face, const std::string& what)
{
    if (!face) return;
    dump_face(r, face, what);
    r.ft("FT_Set_Pixel_Sizes", {face, 0, 17});
    for (const u32 c : {u32{'a'}, u32{'Z'}, u32{'3'}}) {
        r.ft("FT_Load_Char", {face, c, FT_LOAD_RENDER});
        dump_slot(r, r.r32(face + 84), std::format("{} {:#x}", what, c));
    }
}

} // namespace

TEST_CASE("cell_FreeType2: opening from memory, streams and compressed streams, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    const std::vector<u8> ttf = read_file(*flash + "/data/font/SCE-PS3-VR-R-LATIN.TTF");
    REQUIRE(ttf.size() > 1000);
    const std::vector<u8> gz = gzip_stored(ttf), z = lzw_compress(ttf);
    compare(*flash, kLibs, [&](Run& r) {
        const u32 lib = new_library(r);
        const u32 data = r.blob(ttf), gzd = r.blob(gz), zd = r.blob(z);
        const u32 out = r.alloc(8);
        // FT_New_Memory_Face
        r.ft("FT_New_Memory_Face", {lib, data, static_cast<u32>(ttf.size()), 0, out});
        face_glyphs(r, r.r32(out), "memory");
        r.ft("FT_Done_Face", {r.r32(out)});
        r.ft("FT_New_Memory_Face", {lib, data, 100, 0, out});
        r.ft("FT_New_Memory_Face", {lib, 0, 100, 0, out});
        r.ft("FT_New_Memory_Face", {lib, data + 4, static_cast<u32>(ttf.size() - 4), 0, out});
        r.ft("FT_New_Memory_Face", {lib, data, static_cast<u32>(ttf.size()), sx(-1), 0});
        r.ft("FT_New_Memory_Face", {0, data, static_cast<u32>(ttf.size()), 0, out});
        // FT_Open_Face with each kind of argument
        const u32 args = r.alloc(32), params = r.alloc(16);
        auto open = [&](u32 flags, u32 base, u32 size, u32 path, u32 stream, u32 driver, u32 np, u32 pp, s32 index,
                        const std::string& what) {
            r.w32(args, flags);
            r.w32(args + 4, base);
            r.w32(args + 8, size);
            r.w32(args + 12, path);
            r.w32(args + 16, stream);
            r.w32(args + 20, driver);
            r.w32(args + 24, np);
            r.w32(args + 28, pp);
            r.w32(out, 0);
            const u32 e = r.ft("FT_Open_Face", {lib, args, sx(index), out});
            r.rec(std::format("{} -> {:#x}", what, e));
            const u32 face = e ? 0 : r.r32(out);
            if (index >= 0) face_glyphs(r, face, what); // (a face opened with index -1 has no size to set)
            else if (face) dump_face(r, face, what);
            return face;
        };
        const u32 tt = r.ftq("FT_Get_Module", {lib, r.str("truetype")}), t1 = r.ftq("FT_Get_Module", {lib, r.str("type1")}),
                  sfnt = r.ftq("FT_Get_Module", {lib, r.str("sfnt")});
        r.w32(params, 0x756E7061); // FT_PARAM_TAG_UNPATENTED_HINTING
        r.w32(params + 4, 0);
        r.w32(params + 8, 0x41424344);
        r.w32(params + 12, data);
        for (const u32 face : {open(1, data, static_cast<u32>(ttf.size()), 0, 0, 0, 0, 0, 0, "args memory"),
                 open(4, 0, 0, r.str(font("SCE-PS3-VR-R-LATIN.TTF")), 0, 0, 0, 0, 0, "args path"),
                 open(1 | 8, data, static_cast<u32>(ttf.size()), 0, 0, tt, 0, 0, 0, "args truetype"),
                 open(1 | 8, data, static_cast<u32>(ttf.size()), 0, 0, t1, 0, 0, 0, "args type1"),
                 open(1 | 8, data, static_cast<u32>(ttf.size()), 0, 0, sfnt, 0, 0, 0, "args sfnt"),
                 open(1 | 0x10, data, static_cast<u32>(ttf.size()), 0, 0, 0, 2, params, 0, "args params"),
                 open(0, data, static_cast<u32>(ttf.size()), 0, 0, 0, 0, 0, 0, "args none"),
                 open(1, data, static_cast<u32>(ttf.size()), 0, 0, 0, 0, 0, sx(-1), "args index -1")})
            if (face) r.ft("FT_Done_Face", {face});
        r.ft("FT_Open_Face", {lib, 0, 0, out});
        r.ft("FT_Open_Face", {lib, args, 0, 0});
        r.ft("FT_Open_Face", {0, args, 0, out});
        // guest streams: memory, and read through the scripts' function
        for (const bool read : {false, true}) {
            const u32 s = guest_stream(r, data, static_cast<u32>(ttf.size()), read);
            const u32 face = open(2, 0, 0, 0, s, 0, 0, 0, 0, std::format("stream read {}", read));
            r.rec(std::format("stream after open: memory {} size {}", r.r32(s + 28) == kMemory, r.r32(s + 4)));
            if (face) r.ft("FT_Done_Face", {face});
        }
        // compressed streams
        for (const bool read : {false, true}) {
            for (const bool lzw : {false, true}) {
                const u32 src = guest_stream(r, lzw ? zd : gzd, static_cast<u32>(lzw ? z.size() : gz.size()), read);
                const u32 s = r.alloc(40);
                std::memset(r.b + s, 0x5A, 40);
                const u32 e = r.ft(lzw ? "FT_Stream_OpenLZW" : "FT_Stream_OpenGzip", {s, src});
                r.rec(std::format("compressed {} {}: {:#x} size {:#x} pos {} base {} memory {} read {} close {}", lzw, read, e,
                    r.r32(s + 4), r.r32(s + 8), r.r32(s), r.r32(s + 28) == kMemory, ptr(r.r32(s + 20)), ptr(r.r32(s + 24))));
                if (e) continue;
                const u32 face = open(2, 0, 0, 0, s, 0, 0, 0, 0, std::format("compressed {} {}", lzw, read));
                if (face) r.ft("FT_Done_Face", {face});
                r.rec(std::format("closed: descriptor {}", ptr(r.r32(s + 12))));
            }
        }
        const u32 bad = guest_stream(r, data, static_cast<u32>(ttf.size()), false), s = r.alloc(40);
        r.ft("FT_Stream_OpenGzip", {s, bad});
        r.ft("FT_Stream_OpenLZW", {s, bad});
        // attaching to a TrueType face (whose driver attaches nothing)
        const u32 face = open_face(r, lib, font("SCE-PS3-VR-R-LATIN.TTF"));
        r.ft("FT_Attach_File", {face, r.str(font("SCE-PS3-VR-R-LATIN.TTF"))});
        r.ft("FT_Attach_File", {face, r.str("/dev_flash/data/font/NOTHING.AFM")});
        r.ft("FT_Attach_File", {face, 0});
        r.ft("FT_Attach_File", {0, r.str(font("SCE-PS3-VR-R-LATIN.TTF"))});
        r.w32(args, 1);
        r.w32(args + 4, data);
        r.w32(args + 8, static_cast<u32>(ttf.size()));
        r.ft("FT_Attach_Stream", {face, args});
        r.ft("FT_Attach_Stream", {0, args});
        r.w32(args, 2);
        r.w32(args + 16, guest_stream(r, data, static_cast<u32>(ttf.size()), true));
        r.ft("FT_Attach_Stream", {face, args});
        dump_face(r, face, "attached");
        // a file stream's mirror reads as the face's
        const u32 stream = r.r32(face + 104), buf = r.alloc(16);
        if (r.r32(stream + 20)) {
            const u32 got = static_cast<u32>(call_guest(r.t.ctx, r.r32(stream + 20), {stream, 0, buf, 12}));
            r.rec(std::format("stream read {} {}", got, hex(r.b + buf, 12)));
        }
        r.ft("FT_Done_Face", {face});
        // an OpenType table freed through the face's memory
        const u32 f2 = open_face(r, lib, font("SCE-PS3-VR-R-LATIN.TTF")), p = r.alloc(4);
        r.ft("FT_Alloc", {kMemory, 24, p});
        const u32 mark = r.heap_mark();
        r.ftq("FT_OpenType_Free", {f2, r.r32(p)});
        r.ftq("FT_OpenType_Free", {f2, 0});
        r.rec("opentype free:" + r.heap_ops(mark));
        r.ft("FT_Done_Face", {f2});
        r.ft("FT_Done_Library", {lib});
    }, "streams");
}

TEST_CASE("cell_FreeType2: the cache subsystem, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 lib = new_library(r);
        const u32 req = r.alloc(256);
        r.w32(req + 4, r.str(font("SCE-PS3-RD-R-LATIN.TTF")));
        r.w32(req + 8, r.str(font("SCE-PS3-MT-I-LATIN.TTF")));
        r.w32(req + 12, r.str(font("SCE-PS3-SR-R-LATIN.TTF")));
        r.w32(req + 16, 0);
        const u32 out = r.alloc(8), out2 = r.alloc(8), out3 = r.alloc(8);
        r.ft("FTC_Manager_New", {0, 2, 4, 0, r.requester(), req, out});
        r.ft("FTC_Manager_New", {lib, 2, 4, 0, r.requester(), req, out});
        const u32 m = r.r32(out);
        auto calls = [&] { return r.r32(req + 252); };
        r.ft("FTC_Manager_LookupFace", {m, 1, out2});
        const u32 f1 = r.r32(out2);
        dump_face(r, f1, std::format("face 1 ({} requests)", calls()));
        r.ft("FTC_Manager_LookupFace", {m, 1, out2});
        r.rec(std::format("again: same {} requests {}", r.r32(out2) == f1, calls()));
        r.ft("FTC_Manager_LookupFace", {m, 2, out2});
        const u32 f2 = r.r32(out2);
        dump_face(r, f2, "face 2");
        r.ft("FTC_Manager_LookupFace", {m, 4, out2});
        r.rec(std::format("face 4 (no path) {} requests {}", ptr(r.r32(out2)), calls()));
        r.ft("FTC_Manager_LookupFace", {m, 3, out2});
        r.rec(std::format("face 3: requests {}", calls()));
        r.ft("FTC_Manager_LookupFace", {m, 1, out2});
        r.rec(std::format("face 1 after 3 (max 2 faces): same {} requests {}", r.r32(out2) == f1, calls()));
        const u32 face1 = r.r32(out2);
        r.ft("FTC_Manager_LookupFace", {m, 1, 0});
        r.ft("FTC_Manager_LookupFace", {0, 1, out2});
        // sizes
        const u32 sc = r.alloc(24);
        for (const auto& [w, h, pixel, xr, yr] : std::vector<std::tuple<u32, u32, s32, u32, u32>>{
                 {0, 16, 1, 0, 0}, {20, 18, 1, 0, 0}, {0, 12 * 64, 0, 72, 72}, {10 * 64, 0, 0, 96, 96}, {0, 16, 1, 0, 0}}) {
            r.w32(sc, 1);
            r.w32(sc + 4, w);
            r.w32(sc + 8, h);
            r.w32(sc + 12, static_cast<u32>(pixel));
            r.w32(sc + 16, xr);
            r.w32(sc + 20, yr);
            r.ft("FTC_Manager_LookupSize", {m, sc, out3});
            const u32 size = r.r32(out3);
            r.rec(std::format("size {}x{} {} {}x{}: {} active {}", w, h, pixel, xr, yr, size ? size_metrics_text(r, size + 12) : "0",
                r.r32(face1 + 88) == size));
            dump_face(r, face1, "with sizes");
        }
        r.ft("FTC_Manager_LookupSize", {m, sc, 0});
        r.ft("FTC_Manager_LookupSize", {0, sc, out3});
        // charmaps
        r.ft("FTC_CMapCache_New", {m, out3});
        const u32 cmc = r.r32(out3);
        std::string ci = "cmap";
        for (const u32 c : {u32{'A'}, u32{'z'}, 0xE9u, 0x3042u, 0x20ACu, 0u})
            for (const s32 index : {0, 1, -1}) ci += std::format(" {}", r.ftq("FTC_CMapCache_Lookup", {cmc, 1, sx(index), c}));
        r.rec(ci);
        r.ft("FTC_CMapCache_Lookup", {0, 1, 0, 'A'});
        // an old-style query: index >= 4 is the character, face_id an FTC_OldCMapDesc {face_id, type, index or encoding}
        const u32 desc = r.alloc(12);
        r.w32(desc, 1);
        r.w32(desc + 4, 0);
        r.w32(desc + 8, 0);
        r.ft("FTC_CMapCache_Lookup", {cmc, desc, 'B', 0});
        r.w32(desc + 4, 1);
        r.w32(desc + 8, 0x756E6963);
        r.ft("FTC_CMapCache_Lookup", {cmc, desc, 'C', 0});
        r.w32(desc + 4, 7);
        r.ft("FTC_CMapCache_Lookup", {cmc, desc, 'D', 0});
        // images
        r.ft("FTC_ImageCache_New", {m, out3});
        const u32 ic = r.r32(out3);
        const u32 type = r.alloc(16);
        const u32 gp = r.alloc(4), np = r.alloc(4);
        for (const u32 flags : {FT_LOAD_DEFAULT, FT_LOAD_RENDER, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO, FT_LOAD_NO_HINTING}) {
            r.w32(type, 1);
            r.w32(type + 4, 0);
            r.w32(type + 8, 21);
            r.w32(type + 12, flags);
            for (const u32 gi : {36u, 68u, 3u}) {
                r.ft("FTC_ImageCache_Lookup", {ic, type, gi, gp, np});
                const u32 g = r.r32(gp), node = r.r32(np);
                r.rec(std::format("image {:#x} {}: ", flags, gi) + glyph_text(r, g));
                r.ft("FTC_ImageCache_Lookup", {ic, type, gi, gp, np});
                r.rec(std::format("again: same glyph {} same node {}", r.r32(gp) == g, r.r32(np) == node));
                r.ftq("FTC_Node_Unref", {node, m});
                r.ftq("FTC_Node_Unref", {node, m});
                r.ft("FTC_ImageCache_Lookup", {ic, type, gi, gp, 0});
                r.rec(std::format("without a node: same glyph {}", r.r32(gp) == g));
            }
        }
        // an old-style image type: {face_id, u16 pix_width, u16 pix_height, flags}
        r.w32(type, 2);
        r.w16(type + 4, 13);
        r.w16(type + 6, 15);
        r.w32(type + 8, FT_LOAD_RENDER);
        r.ft("FTC_ImageCache_Lookup", {ic, type, 40, gp, np});
        r.rec("old-style image " + glyph_text(r, r.r32(gp)));
        r.ft("FTC_ImageCache_Lookup", {ic, type, 40, 0, np});
        // small bitmaps
        r.ft("FTC_SBitCache_New", {m, out3});
        const u32 sbc = r.r32(out3);
        for (const u32 flags : {FT_LOAD_RENDER, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO, FT_LOAD_DEFAULT}) {
            r.w32(type, 1);
            r.w32(type + 4, 0);
            r.w32(type + 8, 14);
            r.w32(type + 12, flags);
            for (const u32 gi : {36u, 37u, 50u, 3u, 0u}) {
                r.ft("FTC_SBitCache_Lookup", {sbc, type, gi, gp, np});
                const u32 sb = r.r32(gp);
                const u32 bytes = sb ? static_cast<u32>(std::abs(r.s16at(sb + 6))) * r.r8(sb + 1) : 0;
                r.rec(std::format("sbit {:#x} {}: {} buffer {} {}", flags, gi, sb ? hex(r.b + sb, 12) : "0",
                    sb ? ptr(r.r32(sb + 12)) : "-", sb && r.r32(sb + 12) ? hex(r.b + r.r32(sb + 12), bytes) : ""));
                r.ftq("FTC_Node_Unref", {r.r32(np), m});
            }
        }
        r.ft("FTC_SBitCache_Lookup", {sbc, type, 36, 0, np});
        // a face removed: asked for again
        r.ftq("FTC_Manager_RemoveFaceID", {m, 1});
        r.ft("FTC_Manager_LookupFace", {m, 1, out2});
        r.rec(std::format("after removal: requests {}", calls()));
        dump_face(r, r.r32(out2), "face 1 again");
        r.ftq("FTC_Manager_Done", {m});
        r.ftq("FTC_Manager_Done", {0});
        r.ft("FT_Done_Library", {lib});
    }, "cache");
}

TEST_CASE("cell_FreeType2: Sony's additions, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    compare(*flash, kLibs, [](Run& r) {
        const u32 arg = r.alloc(16), list = r.alloc(8), entries = r.alloc(8 * 90), outs = r.alloc(4 * 90);
        r.w32(arg, 3247);
        r.w32(arg + 8, 0xCF200000);
        r.w32(arg + 12, list);
        r.w32(list, 90);
        r.w32(list + 4, entries);
        for (u32 k = 0; k < 90; k++) {
            r.w32(entries + 8 * k, k < 80 ? k : 0x10000 + k);
            r.w32(entries + 8 * k + 4, k == 85 ? 0 : outs + 4 * k);
            r.w32(outs + 4 * k, 0x11111111);
        }
        r.ft("cellFreeType2Ex", {arg});
        std::vector<u32> paths(90);
        for (u32 k = 0; k < 90; k++) {
            paths[k] = r.r32(outs + 4 * k);
            r.rec(std::format("key {}: {}", k, paths[k] && paths[k] != 0x11111111 ? hex(r.b + paths[k], 128) : std::format("{:#x}", paths[k])));
        }
        r.w32(arg, 4043);
        r.w32(list, 3);
        r.ft("cellFreeType2Ex", {arg});
        r.w32(arg, 4044);
        r.ft("cellFreeType2Ex", {arg});
        r.w32(arg, 3247);
        r.w32(arg + 8, 0xCF200001);
        r.ft("cellFreeType2Ex", {arg});
        r.w32(arg + 8, 0xCF200000);
        r.w32(list, 0);
        r.ft("cellFreeType2Ex", {arg});
        r.ft("cellFreeType2Ex", {0});
        // the paths open the fonts behind them
        const u32 lib = new_library(r);
        for (const u32 k : {0u, 8u, 20u, 65u, 72u}) {
            const u32 face = open_face(r, lib, r.cstr(paths[k]) + std::string(1, '\0'));
            (void)face;
            const u32 out = r.alloc(8);
            const u32 e = r.ft("FT_New_Face", {lib, paths[k], 0, out});
            if (!e) {
                dump_face(r, r.r32(out), std::format("font of key {}", k));
                r.ft("FT_Done_Face", {r.r32(out)});
            }
        }
        // the pattern: "/:dev_font", one character other than ':' or none, ":/"; then the hidden path or nothing
        std::string hidden;
        for (const char c : std::string("/dev_flash/data/font/SCE-PS3-MT-R-LATIN.TTF")) hidden += static_cast<char>(c ^ 0xF2);
        for (const std::string visible : {"/:dev_font:/X", "/:dev_fontQ:/X", "/:dev_fontQQ:/X", "/:dev_font::/X", "/:dev_fon:/X",
                 "/:dev_font:", "/:dev_font"}) {
            const std::string p = visible + std::string(1, '\0') + "\xDE" + hidden + "\xF2";
            const u32 at = r.alloc(static_cast<u32>(p.size()) + 1);
            std::memcpy(r.b + at, p.data(), p.size());
            const u32 out = r.alloc(8);
            const u32 e = r.ft("FT_New_Face", {lib, at, 0, out});
            r.rec(std::format("{} -> {:#x} {}", visible, e, e ? "" : r.cstr(r.r32(r.r32(out) + 20))));
            if (!e) r.ft("FT_Done_Face", {r.r32(out)});
        }
        const u32 plain = r.str("/:dev_font:/RD-R-LATIN.ttf"), out = r.alloc(8);
        r.ft("FT_New_Face", {lib, plain, 0, out});
        r.ft("FT_Done_Library", {lib});
        // the unnamed exports
        const u32 v = r.alloc(16);
        std::memset(r.b + v, 0x77, 16);
        r.call("cell_FreeType2", "cell_FreeType2_211248E7", {v});
        r.rec("version word " + hex(r.b + v, 16));
        r.call("cell_FreeType2", "cell_FreeType2_211248E7", {0});
        r.call("cell_FreeType2", "cell_FreeType2_904CA41F", {0, 0});
    }, "sony");
}

namespace {

// ---- fonts made by the scripts: a Windows FNT (version 3) and a Type 1 font (PFA) with an AFM

std::vector<u8> make_fnt(bool v3)
{
    const u32 first = 32, last = 37, rows = 10, header = v3 ? 148 : 118, entry = v3 ? 6 : 4;
    const u32 count = last - first + 2; // with the sentinel
    std::vector<u32> widths;
    for (u32 i = 0; i < count; i++) widths.push_back(3 + (i * 5) % 11);
    std::vector<u8> bits;
    std::vector<u32> offsets;
    const u32 table_end = header + entry * count;
    for (u32 i = 0; i < count; i++) {
        offsets.push_back(table_end + static_cast<u32>(bits.size()));
        const u32 cols = (widths[i] + 7) / 8;
        for (u32 c = 0; c < cols; c++)
            for (u32 r = 0; r < rows; r++) bits.push_back(static_cast<u8>((i * 29 + c * 7 + r * 13) * 37));
    }
    std::vector<u8> f(header, 0);
    auto le16 = [&](u32 at, u32 v) {
        f[at] = static_cast<u8>(v);
        f[at + 1] = static_cast<u8>(v >> 8);
    };
    auto le32 = [&](u32 at, u32 v) {
        for (u32 k = 0; k < 4; k++) f[at + k] = static_cast<u8>(v >> (8 * k));
    };
    const u32 size = table_end + static_cast<u32>(bits.size()) + 8;
    le16(0, v3 ? 0x300 : 0x200);
    le32(2, size);
    const char* copyright = "pspocket test font";
    std::memcpy(f.data() + 6, copyright, std::strlen(copyright));
    le16(66, 0);    // file_type: raster
    le16(68, 9);    // nominal point size
    le16(70, 96);   // vertical resolution
    le16(72, 96);   // horizontal
    le16(74, 8);    // ascent
    le16(76, 1);    // internal leading
    le16(78, 2);    // external leading
    f[80] = 1;      // italic
    f[81] = 0;
    f[82] = 0;
    le16(83, 700);  // weight
    f[85] = 0;      // charset
    le16(86, 0);    // pixel width
    le16(88, rows); // pixel height
    f[90] = 0x31;
    le16(91, 6);    // avg width
    le16(93, 13);   // max width
    f[95] = static_cast<u8>(first);
    f[96] = static_cast<u8>(last);
    f[97] = 2;      // default char (relative)
    f[98] = 0;
    le16(99, 2);    // bytes per row
    le32(101, 0);
    le32(105, size - 8); // face name offset
    le32(109, 0);
    le32(113, table_end);
    f[117] = 0;
    if (v3) {
        le32(118, 0x11);
        le16(122, 1);
        le16(124, 2);
        le16(126, 3);
        le32(128, 0);
    }
    for (u32 i = 0; i < count; i++) {
        const u32 at = header + entry * i;
        f.resize(std::max<usize>(f.size(), at + entry));
        f[at] = static_cast<u8>(widths[i]);
        f[at + 1] = static_cast<u8>(widths[i] >> 8);
        if (v3) {
            for (u32 k = 0; k < 4; k++) f[at + 2 + k] = static_cast<u8>(offsets[i] >> (8 * k));
        } else {
            f[at + 2] = static_cast<u8>(offsets[i]);
            f[at + 3] = static_cast<u8>(offsets[i] >> 8);
        }
    }
    f.insert(f.end(), bits.begin(), bits.end());
    const char* name = "Fnt\0\0\0\0";
    f.insert(f.end(), name, name + 8);
    return f;
}

// Type 1 charstring and eexec encryption (Adobe's Type 1 Font Format, chapter 7).
std::vector<u8> t1_encrypt(const std::vector<u8>& plain, u16 r, u32 skip)
{
    std::vector<u8> in(skip, 0);
    for (u32 i = 0; i < skip; i++) in[i] = static_cast<u8>(i * 41 + 7);
    in.insert(in.end(), plain.begin(), plain.end());
    std::vector<u8> out;
    for (const u8 p : in) {
        const u8 c = static_cast<u8>(p ^ (r >> 8));
        r = static_cast<u16>((c + r) * 52845u + 22719u);
        out.push_back(c);
    }
    return out;
}

void t1_num(std::vector<u8>& o, s32 v)
{
    if (v >= -107 && v <= 107) {
        o.push_back(static_cast<u8>(v + 139));
    } else if (v >= 108 && v <= 1131) {
        v -= 108;
        o.push_back(static_cast<u8>(247 + v / 256));
        o.push_back(static_cast<u8>(v % 256));
    } else if (v <= -108 && v >= -1131) {
        v = -v - 108;
        o.push_back(static_cast<u8>(251 + v / 256));
        o.push_back(static_cast<u8>(v % 256));
    } else {
        o.push_back(255);
        for (int k = 3; k >= 0; k--) o.push_back(static_cast<u8>(static_cast<u32>(v) >> (8 * k)));
    }
}

std::vector<u8> t1_charstring(std::initializer_list<std::pair<std::vector<s32>, u8>> ops)
{
    std::vector<u8> o;
    for (const auto& [args, op] : ops) {
        for (const s32 a : args) t1_num(o, a);
        o.push_back(op);
    }
    return o;
}

std::vector<u8> make_type1()
{
    enum : u8 { hstem = 1, vstem = 3, rlineto = 5, rrcurveto = 8, closepath = 9, hsbw = 13, endchar = 14, rmoveto = 21 };
    const std::vector<std::pair<std::string, std::vector<u8>>> glyphs = {
        {".notdef", t1_charstring({{{0, 500}, hsbw}, {{}, endchar}})},
        {"A", t1_charstring({{{20, 600}, hsbw}, {{0, 50}, hstem}, {{30, 0}, rmoveto}, {{250, 700}, rlineto},
                  {{250, -700}, rlineto}, {{-90, 0}, rlineto}, {{-160, 500}, rlineto}, {{-160, -500}, rlineto},
                  {{}, closepath}, {{}, endchar}})},
        {"B", t1_charstring({{{40, 560}, hsbw}, {{0, 90}, vstem}, {{40, 0}, rmoveto}, {{0, 700}, rlineto},
                  {{200, 0}, rlineto}, {{150, 0, 150, -300, 0, -300}, rrcurveto}, {{180, 0, 180, -400, 0, -400}, rrcurveto},
                  {{}, closepath}, {{}, endchar}})},
        {"space", t1_charstring({{{0, 250}, hsbw}, {{}, endchar}})},
    };
    std::string priv = "dup /Private 16 dict dup begin\n"
                       "/RD{string currentfile exch readstring pop}executeonly def\n"
                       "/ND{noaccess def}executeonly def\n"
                       "/NP{noaccess put}executeonly def\n"
                       "/BlueValues [-20 0 700 720] def\n/OtherBlues [-210 -200] def\n/FamilyBlues [-21 0 701 721] def\n"
                       "/BlueScale 0.039625 def\n/BlueShift 7 def\n/BlueFuzz 1 def\n/StdHW [50] def\n/StdVW [80] def\n"
                       "/StemSnapH [50 60] def\n/StemSnapV [80 90] def\n/ForceBold true def\n/LanguageGroup 0 def\n"
                       "/password 5839 def\n/lenIV 4 def\n/MinFeature {16 16} def\n/Subrs 0 array\nND\n"
                       "2 index /CharStrings 4 dict dup begin\n";
    std::vector<u8> plain(priv.begin(), priv.end());
    for (const auto& [name, cs] : glyphs) {
        const std::vector<u8> enc = t1_encrypt(cs, 4330, 4);
        const std::string head = "/" + name + " " + std::to_string(enc.size()) + " RD ";
        plain.insert(plain.end(), head.begin(), head.end());
        plain.insert(plain.end(), enc.begin(), enc.end());
        const std::string tail = " ND\n";
        plain.insert(plain.end(), tail.begin(), tail.end());
    }
    const std::string end = "end\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\n"
                            "mark currentfile closefile\n";
    plain.insert(plain.end(), end.begin(), end.end());
    const std::vector<u8> enc = t1_encrypt(plain, 55665, 4);
    std::string font = "%!PS-AdobeFont-1.0: TestOne 001.000\n"
                       "12 dict begin\n"
                       "/FontInfo 9 dict dup begin\n"
                       "/version (001.000) readonly def\n/Notice (pspocket test font) readonly def\n"
                       "/FullName (Test One Bold) readonly def\n/FamilyName (Test One) readonly def\n"
                       "/Weight (Bold) readonly def\n/ItalicAngle -12 def\n/isFixedPitch false def\n"
                       "/UnderlinePosition -100 def\n/UnderlineThickness 50 def\n"
                       "end readonly def\n"
                       "/FontName /TestOne def\n"
                       "/Encoding 256 array\n0 1 255 {1 index exch /.notdef put} for\n"
                       "dup 32 /space put\ndup 65 /A put\ndup 66 /B put\nreadonly def\n"
                       "/PaintType 0 def\n/FontType 1 def\n/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n"
                       "/FontBBox {0 -200 800 800} readonly def\n/UniqueID 4000001 def\n"
                       "currentdict end\ncurrentfile eexec\n";
    for (usize i = 0; i < enc.size(); i++) {
        font += std::format("{:02x}", enc[i]);
        if (i % 32 == 31) font += "\n";
    }
    font += "\n";
    for (int i = 0; i < 8; i++) font += std::string(64, '0') + "\n";
    font += "cleartomark\n";
    return std::vector<u8>(font.begin(), font.end());
}

std::vector<u8> make_afm()
{
    const std::string afm = "StartFontMetrics 2.0\nFontName TestOne\nStartCharMetrics 3\n"
                            "C 65 ; WX 600 ; N A ; B 30 0 530 700 ;\nC 66 ; WX 560 ; N B ; B 40 0 400 700 ;\n"
                            "C 32 ; WX 250 ; N space ; B 0 0 0 0 ;\nEndCharMetrics\nStartKernData\nStartKernPairs 2\n"
                            "KPX A B -40\nKPX B A -25\nEndKernPairs\nEndKernData\nEndFontMetrics\n";
    return std::vector<u8>(afm.begin(), afm.end());
}

} // namespace

TEST_CASE("cell_FreeType2: Windows FNT and Type 1 faces, as libfreetype", "[freetype][hle]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("no PSPOCKET_TEST_DEV_FLASH");
    const std::vector<u8> fnt3 = make_fnt(true), fnt2 = make_fnt(false), t1 = make_type1(), afm = make_afm();
    compare(*flash, kLibs, [&](Run& r) {
        const u32 lib = new_library(r);
        const u32 out = r.alloc(8), big = r.alloc(256), a = r.alloc(16);
        for (const auto* data : {&fnt3, &fnt2}) {
            const u32 at = r.blob(*data);
            r.ft("FT_New_Memory_Face", {lib, at, static_cast<u32>(data->size()), 0, out});
            const u32 face = r.r32(out);
            if (!face) continue;
            dump_face(r, face, "fnt");
            std::memset(r.b + big, 0x33, 256);
            r.ft("FT_Get_WinFNT_Header", {face, big});
            r.rec("winfnt header " + hex(r.b + big, 156));
            r.rec("x11 format " + r.cstr(r.ftq("FT_Get_X11_Font_Format", {face})));
            r.ft("FT_Select_Size", {face, 0});
            dump_face(r, face, "selected");
            r.ft("FT_Select_Size", {face, 1});
            r.ft("FT_Set_Pixel_Sizes", {face, 0, 10});
            r.ft("FT_Set_Char_Size", {face, 0, 9 * 64, 96, 96});
            dump_face(r, face, "sized");
            for (const u32 c : {32u, 33u, 36u, 37u, 38u, 0u}) {
                r.ft("FT_Load_Char", {face, c, FT_LOAD_DEFAULT});
                dump_slot(r, r.r32(face + 84), std::format("fnt char {}", c));
                r.ft("FT_Load_Char", {face, c, FT_LOAD_RENDER});
                dump_slot(r, r.r32(face + 84), std::format("fnt rendered {}", c));
            }
            const u32 gi = r.alloc(4);
            std::string it = std::format("first {:#x}", r.ftq("FT_Get_First_Char", {face, gi}));
            for (u32 c = 30; c < 40; c++) it += std::format(" {}", r.ftq("FT_Get_Char_Index", {face, c}));
            r.rec(it);
            r.ft("FT_Get_PS_Font_Info", {face, big});
            r.ft("FT_Done_Face", {face});
        }
        // Type 1
        const u32 t1at = r.blob(t1);
        r.ft("FT_New_Memory_Face", {lib, t1at, static_cast<u32>(t1.size()), 0, out});
        const u32 face = r.r32(out);
        if (face) {
            dump_face(r, face, "type1");
            r.rec("ps name " + r.cstr(r.ftq("FT_Get_Postscript_Name", {face})));
            r.rec("x11 format " + r.cstr(r.ftq("FT_Get_X11_Font_Format", {face})));
            r.ft("FT_Has_PS_Glyph_Names", {face});
            std::memset(r.b + big, 0x33, 256);
            r.ft("FT_Get_PS_Font_Info", {face, big});
            r.rec(std::format("font info \"{}\" \"{}\" \"{}\" \"{}\" \"{}\" {}", r.cstr(r.r32(big)), r.cstr(r.r32(big + 4)),
                r.cstr(r.r32(big + 8)), r.cstr(r.r32(big + 12)), r.cstr(r.r32(big + 16)), hex(r.b + big + 20, 12)));
            std::memset(r.b + big, 0x33, 256);
            r.ft("FT_Get_PS_Font_Private", {face, big});
            // (unique_id, the first word, differs by construction: FreeType 2.2.1 writes a count over the record's
            // first byte, which is unique_id's high byte on the PPU and its low byte on the host)
            r.rec("private " + hex(r.b + big + 4, 192));
            r.ft("FT_Get_Sfnt_Table", {face, 0});
            r.ft("FT_Get_Sfnt_Name_Count", {face});
            const u32 nm = r.alloc(16);
            r.ft("FT_Get_Sfnt_Name", {face, 0, nm});
            for (const char* n : {"A", "B", "space", ".notdef", "C"}) r.ft("FT_Get_Name_Index", {face, r.str(n)});
            const u32 buf = r.alloc(64);
            for (u32 g = 0; g < 4; g++) { // (index 4 reads past the names in FreeType 2.2.1)
                std::memset(r.b + buf, 0, 64);
                r.ft("FT_Get_Glyph_Name", {face, g, buf, 32});
                r.rec("glyph name " + r.cstr(buf));
            }
            r.ft("FT_Get_Multi_Master", {face, big});
            r.ft("FT_Get_MM_Var", {face, a});
            r.ft("FT_Get_BDF_Charset_ID", {face, a, a + 4});
            r.ft("FT_Set_Char_Size", {face, 0, 30 * 64, 72, 72});
            for (const u32 c : {65u, 66u, 32u, 67u}) {
                for (const u32 f : {FT_LOAD_DEFAULT, FT_LOAD_RENDER, FT_LOAD_NO_HINTING, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO,
                         FT_LOAD_NO_SCALE}) {
                    r.ft("FT_Load_Char", {face, c, f});
                    dump_slot(r, r.r32(face + 84), std::format("type1 {} {:#x}", c, f));
                }
            }
            const u32 k = r.alloc(8);
            const u32 ga = r.ftq("FT_Get_Char_Index", {face, 65}), gb = r.ftq("FT_Get_Char_Index", {face, 66});
            r.ft("FT_Get_Kerning", {face, ga, gb, 0, k});
            r.rec(std::format("kerning before the AFM {} {}", r.s32at(k), r.s32at(k + 4)));
            // the metrics, attached from memory
            const u32 afmat = r.blob(afm), args = r.alloc(32);
            r.w32(args, 1);
            r.w32(args + 4, afmat);
            r.w32(args + 8, static_cast<u32>(afm.size()));
            r.ft("FT_Attach_Stream", {face, args});
            dump_face(r, face, "with its AFM");
            for (u32 mode = 0; mode < 3; mode++) {
                r.ft("FT_Get_Kerning", {face, ga, gb, mode, k});
                r.rec(std::format("kerning A B {} {} {}", mode, r.s32at(k), r.s32at(k + 4)));
                r.ft("FT_Get_Kerning", {face, gb, ga, mode, k});
                r.rec(std::format("kerning B A {} {} {}", mode, r.s32at(k), r.s32at(k + 4)));
            }
            r.ft("FT_Get_Track_Kerning", {face, 0x100000, 1, k});
            r.ft("FT_Get_PFR_Kerning", {face, ga, gb, k});
            r.rec(std::format("pfr kerning {} {}", r.s32at(k), r.s32at(k + 4)));
            r.ft("FT_Done_Face", {face});
        }
        r.ft("FT_Done_Library", {lib});
    }, "fnt and type1");
}
