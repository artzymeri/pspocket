// cellFont: Sony's libfont.sprx as pspocket's own (D-526, part 2). libfont is the PS3's font API: it keeps the system
// fonts' table (which font file each font ID is, which fonts a fontset is made of and which of them draws which code),
// the file cache, the fonts a game opens (CellFont: a fontset, a file, memory or an instance of another), renderers,
// metrics and layouts, glyph images and outlines ("vertexes glyphs"), the graphics draw contexts. The FreeType work is
// the font library's (cellFontFT, through the CellFontLibrary's table).
//
// libfont's code is pspocket's here; its data are the firmware's: the system font table, the fontsets' code maps (with
// the code coverage of each font) and the fonts' paths are tables in libfont.sprx's data segment, which pspocket does
// not copy into its source. At the first call the module is read from dev_flash (/dev_flash/sys/external/libfont.sprx,
// decrypted with the build's SELF keys as the loader's modules are) and its segments placed and relocated in guest
// memory; the tables are read there, and libfont's static records (the library's state, its locks, the font entries,
// the file cache's settings) live in that data segment at libfont's own offsets, as in Sony's. Only the firmware whose
// libfont is known (its identity) is accepted.
//
// libfont's module_start (a call to cellSysutil for games built with SDKs before 1.40) is not made: pspocket's modules
// have no start. libfont's dead code (a reserved-font list no export sets, file helpers nothing calls) is not ported.
//
// Reference: Sony's libfont.sprx (firmware 4.92) as the oracle (tests/hle/test_font.cpp: the same scripts over Sony's
// libfont, libfontFT and libfreetype and over pspocket's cellFont, cellFontFT and cell_FreeType2).

#include "pspocket/hle/hle.hpp"

#if PSPOCKET_HAVE_FREETYPE

#include "freetype_bridge.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

// Sony's code multiplies and adds in separate single-precision steps except where it uses fmadds/fmsubs (std::fma
// here): no contraction.
#pragma clang fp contract(off)

namespace pspocket::hle {

namespace cfont {

using ftb::call_guest_fn;
using ftb::current_ctx;
using ftb::gbase;
using ftb::hp;
using ftb::rd16;
using ftb::rd32;
using ftb::rd8;
using ftb::rds16;
using ftb::rds32;
using ftb::Scope;
using ftb::wr16;
using ftb::wr32;
using ftb::wr8;

constexpr const char* kChan = "cellFont";

constexpr u32 E(u32 n) { return 0x80540000u | n; }
constexpr s32 Es(u32 n) { return static_cast<s32>(E(n)); }

float rdf(u32 a) { return std::bit_cast<float>(rd32(a)); }
void wrf(u32 a, float v) { wr32(a, std::bit_cast<u32>(v)); }
u64 rd64(u32 a) { return ps_read64(gbase(), a); }
void wr64(u32 a, u64 v) { ps_write64(gbase(), a, v); }

s32 fctiwz(float v)
{
    if (std::isnan(v)) return std::numeric_limits<s32>::min();
    if (v >= 2147483647.0f) return std::numeric_limits<s32>::max();
    if (v <= -2147483648.0f) return std::numeric_limits<s32>::min();
    return static_cast<s32>(v);
}
float i2f(s64 v) { return static_cast<float>(v); }
float u2f(u32 v) { return static_cast<float>(v); } // fcfid of a zero-extended word, then frsp

u32 nid(const char* name)
{
    static std::mutex m;
    static std::unordered_map<const char*, u32> cache;
    std::lock_guard g(m);
    auto [it, inserted] = cache.try_emplace(name, 0);
    if (inserted) it->second = recomp::nid_of(name);
    return it->second;
}

// The descriptor of one of this module's functions (linked on first use: libfont hands some to the font library and
// to graphics libraries).
u32 desc(const char* name) { return HleProcess::current()->link_function("cellFont", nid(name), name); }
// The same for an export with no known name (its NID).
u32 desc(u32 nid_, const char* name) { return HleProcess::current()->link_function("cellFont", nid_, name); }

// A guest function (descriptor) called with integer arguments in their registers and floating-point ones in f1...
u64 callf(u32 opd, std::initializer_list<u64> args, std::initializer_list<float> fargs)
{
    PPUContext& ctx = *current_ctx();
    double saved[13];
    std::memcpy(saved, &ctx.fpr[1], sizeof saved);
    u32 i = 1;
    for (const float f : fargs) ctx.fpr[i++] = static_cast<double>(f);
    const u64 r = call_guest_fn(opd, args);
    std::memcpy(&ctx.fpr[1], saved, sizeof saved);
    return r;
}
s32 calls(u32 opd, std::initializer_list<u64> args, std::initializer_list<float> fargs = {})
{
    return static_cast<s32>(static_cast<u32>(callf(opd, args, fargs)));
}
// The function at `off` of a CellFontLibrary's table.
u32 tab(u32 lib, u32 off) { return rd32(lib + off); }
float fret() { return static_cast<float>(current_ctx()->fpr[1]); }

struct Local {
    explicit Local(u32 size) : v(*current_ctx(), gbase(), size) {}
    u32 addr() const { return v.addr(); }
    u32 get() const { return rd32(v.addr()); }
    StackVar v;
};

// ------------------------------------------------------------------------------------------------- libfont's image
// libfont.sprx's segments placed in guest memory: its data at its own virtual addresses (va).
constexpr u32 kSeg1 = 0xD860; // the data segment's virtual address
// The identity (recomp::prx_identity) of the libfont whose data layout this file knows (firmware 4.92).
constexpr std::string_view kIdentity = "b57b75e3e4ebb3806ebf821fd1c04dafba5b4232663503f0dcb258d36a0fa6dd";

struct Image {
    std::mutex m;
    bool tried = false;
    u32 seg0 = 0, seg1 = 0;
};

Image& image() { return HleProcess::current()->state<Image>(); }

bool place_image(Image& im)
{
    HleProcess* p = HleProcess::current();
    std::vector<u8> self;
    if (!p->kernel().vfs_read_file("/dev_flash/sys/external/libfont.sprx", self)) {
        PS_LOG_ERROR(kChan, "/dev_flash/sys/external/libfont.sprx can't be read: libfont's font tables are missing");
        return false;
    }
    auto elf_bytes = recomp::decrypt_self(self, recomp::builtin_self_keys());
    if (!elf_bytes) {
        PS_LOG_ERROR(kChan, "libfont.sprx doesn't open: {}", elf_bytes.error().message);
        return false;
    }
    auto elf = recomp::ElfFile::parse(*elf_bytes);
    if (!elf) {
        PS_LOG_ERROR(kChan, "libfont.sprx: {}", elf.error().message);
        return false;
    }
    const std::string id = recomp::prx_identity(*elf);
    if (!kIdentity.empty() && id != kIdentity) {
        PS_LOG_ERROR(kChan, "libfont.sprx {} is not the firmware 4.92 module whose tables pspocket reads", id);
        return false;
    }
    const std::vector<u32> sizes = recomp::prx_load_sizes(*elf);
    if (sizes.size() < 2) return false;
    MainMemory& mm = p->main_memory();
    const u32 a0 = mm.alloc((sizes[0] + 0x1FF) & ~0xFFu, 0x100), a1 = mm.alloc((sizes[1] + 0x1FF) & ~0xFFu, 0x100);
    if (!a0 || !a1) {
        PS_LOG_ERROR(kChan, "no main memory for libfont's tables");
        return false;
    }
    const u32 addrs[] = {a0, a1};
    auto placed = recomp::place_prx(*elf, *elf_bytes, std::span<const u32>(addrs, 2));
    if (!placed) {
        PS_LOG_ERROR(kChan, "libfont.sprx: {}", placed.error().message);
        return false;
    }
    for (const recomp::PlacedSegment& s : placed->segments)
        if (s.addr) std::memcpy(hp(s.addr), s.bytes.data(), s.bytes.size());
    im.seg0 = a0;
    im.seg1 = a1;
    PS_LOG_INFO(kChan, "libfont's tables (module {}) at {:#x} and {:#x}", id, a0, a1);
    return true;
}

bool has_image()
{
    Image& im = image();
    std::lock_guard g(im.m);
    if (!im.tried) {
        im.tried = true;
        place_image(im);
    }
    return im.seg0 != 0;
}

// The guest address of libfont's virtual address `v`.
u32 va(u32 v)
{
    Image& im = image();
    return v < kSeg1 ? im.seg0 + v : im.seg1 + (v - kSeg1);
}

} // namespace cfont

namespace cfont {

// ------------------------------------------------------------------------------------------------- statics
// libfont's TOC slots point at its static records (their values relocated with the image).
u32 toc(u32 slot) { return rd32(va(slot)); }
u32 st() { return toc(0x12A70); }       // the library's state: lock word, revision, cache, font entries...
u32 st_ptr() { return toc(0x12A6C); }   // holds st while libfont is initialized (cellFontStatic)
u32 cfg() { return toc(0x12A08); }      // settings: +8 disabled functions, +0x28 file-error handler, +0x38 ...
u32 tables() { return toc(0x12A74); }   // the system fonts' tables: +8 lock word, +0x10.. the tables
u32 usleep_slot() { return toc(0x12ADC); }
u32 handler_slot() { return toc(0x12AE0); }

// Disabled functions (cellFontPatchWorks): a bit of the settings' 64-bit word.
bool disabled(u64 bit) { return (rd64(cfg() + 8) & bit) != 0; }

// ------------------------------------------------------------------------------------------------- locks
// libfont's lock words: the top bit is a spin lock (taken with other flag bits), released by clearing it and bit 29
// (0x5FFFFFFF); a waiter sleeps a millisecond through the game's usleep (cellFontsetUSleep) or sys_timer_usleep.
void nap()
{
    if (const u32 fn = rd32(usleep_slot())) call_guest_fn(fn, {1000});
    else lv2_syscall(*current_ctx(), 141, {1000});
}

u32 lock_word(u32 a, u32 bits)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, a);
        if (!(v & 0x80000000u)) {
            if (atomic_cas_be32(base, a, v, v | bits) == v) return v;
            continue;
        }
        nap();
    }
}

void unlock_word(u32 a)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, a);
        if (atomic_cas_be32(base, a, v, v & 0x5FFFFFFFu) == v) return;
    }
}

void or_word(u32 a, u32 bits)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, a);
        if (atomic_cas_be32(base, a, v, v | bits) == v) return;
    }
}

void and_word(u32 a, u32 mask)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, a);
        if (atomic_cas_be32(base, a, v, v & mask) == v) return;
    }
}

void add_word(u32 a, s32 d)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, a);
        if (d < 0 && !v) return;
        if (atomic_cas_be32(base, a, v, v + static_cast<u32>(d)) == v) return;
    }
}

// The library's own lock (st+0): taken; the word's value (bit 0: initialized) returned. fn_ae70 / fn_adc8.
u32 lock_static()
{
    return lock_word(st(), 0x80000000u) & 0x7FFFFFFFu;
}

void unlock_static(u32 initialized)
{
    if (initialized) {
        wr32(st_ptr(), st());
        or_word(st(), 1);
    } else {
        and_word(st(), ~1u);
        wr32(st_ptr(), 0);
    }
    unlock_word(st());
}

// ------------------------------------------------------------------------------------------------- font entries
// A font table {?, count, entries}; an entry {state, unique id, ?, face handle} (16 bytes): its state word's top bit
// a lock (taken with bit 29), bit 30 "open", the low bits a count of the fonts using it.
u32 entry(u32 table, u32 index)
{
    if (index >= rd32(table + 4)) return 0;
    return rd32(table + 8) + index * 16;
}

// ------------------------------------------------------------------------------------------------- system fonts
// The fontset state of a font (CellFont + 0x18): {type, map, ?, font count, font list, ?, code map, adjustments}.
namespace fsx {
constexpr u32 count = 12, list = 16, codes = 24, adjust = 28;
}

// fn_b3c0: the fontset's number of fonts.
s32 fontset_count(u32 fs, u32 out)
{
    if (!out) return Es(0x102);
    wr32(out, fs ? rd32(fs + fsx::count) : 0);
    return 0;
}

// fn_b3f4: the fontset's i-th font ID.
s32 fontset_font(u32 fs, u32 i, u32 out)
{
    if (!fs || !out || i >= rd32(fs + fsx::count)) return Es(0x102);
    wr32(out, rd32(rd32(fs + fsx::list) + 4 * i));
    return 0;
}

// fn_b444: a system font's file entry (its path and the path's length).
s32 font_file(u32 id, u32 path_out, u32 len_out)
{
    const u32 t = tables();
    const u32 hdr = rd32(t + 0x10), files = rd32(t + 0x14);
    if (!hdr || !files) return Es(0x103);
    if (id >= rd32(hdr + 0x14)) return Es(0x102);
    const u32 f = files + rd32(files + id * 4);
    if (path_out) wr32(path_out, f + 4);
    if (len_out) wr32(len_out, rd16(f + 2) + 1u);
    return 0;
}

// fn_b4d0: a system font's cache size (KB units of its table entry, in bytes).
s32 font_cache_size(u32 id, u32 out)
{
    const u32 t = tables();
    const u32 hdr = rd32(t + 0x10), files = rd32(t + 0x14);
    if (!hdr || !files) {
        if (!out) return Es(0x103);
        wr32(out, 0);
        return Es(0x103);
    }
    if (!out) return Es(0x102);
    if (id >= rd32(hdr + 0x14)) {
        wr32(out, 0);
        return Es(0x102);
    }
    wr32(out, static_cast<u32>(rd16(files + rd32(files + id * 4))) << 10);
    return 0;
}

// fn_b584: a system font's info record (+4 x scale, +8 y shift, +0xC weight scale, +0x10 ...).
s32 font_info(u32 id, u32 out)
{
    const u32 t = tables();
    const u32 hdr = rd32(t + 0x10), info = rd32(t + 0x18);
    if (!hdr || !info) return Es(0x103);
    if (id >= rd32(hdr + 0x14)) return Es(0x102);
    if (out) wr32(out, info + rd32(info + id * 4));
    return 0;
}

// fn_b5f0: a system font's two 16-bit sizes (its info's +0x10 and +0x12), when the tables' version has them.
s32 font_sizes(u32 id, u32 a, u32 b)
{
    const u32 t = tables();
    const u32 hdr = rd32(t + 0x10), info = rd32(t + 0x18);
    if (b) wr32(b, 0);
    if (a) wr32(a, 0);
    if (!hdr || !info) return Es(0x103);
    if (id >= rd32(hdr + 0x14)) return Es(0x102);
    if (rd32(hdr + 4) > 1) {
        const u32 r = rd32(t + 0x18) + rd32(rd32(t + 0x18) + id * 4);
        const u32 x = rd16(r + 0x10), y = rd16(r + 0x12);
        if (b) wr32(b, y);
        if (a) wr32(a, x);
        if (y && x) return 0;
    }
    return Es(0x106);
}

// fn_b6c8: the number of system fonts.
s32 font_count(u32 out)
{
    if (!out) return Es(0x102);
    const u32 hdr = rd32(tables() + 0x10);
    wr32(out, hdr ? rd32(hdr + 0x14) : 0);
    return 0;
}

// fn_b704: a code's adjustment in the fontset (ranges of codes with {x scale, y scale, y shift, x shift} for a font).
s32 code_adjust(u32 fs, u32 code, u32 out, u32 font)
{
    const u32 r = rd32(fs + fsx::adjust);
    const s32 n = rds32(r);
    if (n > 0 && rd32(r + 4) <= code) {
        u32 p = r + 4;
        u32 k = 0;
        bool found = true;
        while (rd32(p + 4) < code) {
            k++;
            if (k == static_cast<u32>(n)) {
                found = false;
                break;
            }
            p = r + 4 + k * 28;
            if (code < rd32(p)) {
                found = false;
                break;
            }
        }
        if (found && (rd32(p + 24) == 0 || rd32(p + 24) == font)) {
            wr32(out, rd32(p + 8));
            wr32(out + 4, rd32(p + 12));
            wr32(out + 8, rd32(p + 16));
            wr32(out + 12, rd32(p + 20));
            return 0;
        }
    }
    const u32 one = rd32(va(0x12A94));
    wr32(out + 4, one);
    wr32(out, one);
    wr32(out + 12, 0);
    wr32(out + 8, 0);
    return Es(0x105);
}

// fn_b8b4: a fontset's state from the system tables (the fontset type and map, else the first of the type).
s32 fontset_lookup(u32 fs, u32 type)
{
    const u32 t = tables();
    if (!(rd32(t + 8) & 1)) return Es(0x103);
    if (!fs) return Es(0x102);
    wr32(fs + 12, 0);
    const u32 sets = rd32(t + 0x1C);
    if (!sets) return Es(0x102);
    const u32 map = rd32(type + 4);
    const s32 n = rds32(rd32(t + 0x10) + 0x20);
    u32 found = 0;
    for (s32 i = 0; i < n; i++) {
        const u32 s = sets + rd32(sets + 4 * static_cast<u32>(i));
        if (rd32(s) == rd32(type)) {
            if (rd32(s + 4) == map) {
                found = s;
                break;
            }
            if (!found) found = s;
        }
    }
    if (!found) return Es(0x101);
    wr32(fs + 0, rd32(type));
    wr32(fs + 4, map);
    wr32(fs + 8, 0);
    wr32(fs + 16, found + 20);
    wr32(fs + 12, rd32(found + 16));
    const u32 ranges = rd32(t + 0x24);
    wr32(fs + 24, ranges + rd32(ranges + rd32(found + 8) * 4));
    u32 adj = 0;
    if (const u32 a = rd16(found + 12)) {
        const u32 adjs = rd32(t + 0x20);
        adj = adjs + rd32(adjs + a * 4 - 4);
    }
    wr32(fs + 28, adj);
    return 0;
}

// A range list {count, then {upper code, node, value}} (8 bytes each): the index of the first range whose upper
// bound is at least `code`, or -1.
s32 range_find(u32 list, u32 code)
{
    const s32 n = rds32(list);
    if (n < 1) return -1;
    for (s32 i = 0; i < n; i++)
        if (rd32(list + 4 + 8 * static_cast<u32>(i)) >= code) return i;
    return -1;
}

// The node table's entry for `node` (its low byte): 64-bit addresses in the tables plus a base.
u32 node_at(u32 node)
{
    const u32 t = tables();
    return static_cast<u32>(rd64(rd32(t + 0x28) + (node & 0xFF) * 8) + rd32(t + 0x2C));
}

// A range list {count, then {upper code, node, value}} (8 bytes each) of the system tables.
u32 range_list(u32 index)
{
    const u32 r = rd32(tables() + 0x24);
    return r + rd32(r + index * 4);
}

// fn_bfa0: the code map's tree walked from `node`: which font (the value's low byte) draws `code`, and as which code
// (a node of pairs maps it). Node kinds: 0 a range list (the value's low byte), 1 a bitmap of codes, 2 a sorted list
// of codes, 3 sorted pairs {code, mapped}. On a miss the node's 0x2000 bit sends the walk on to the range list of the
// value's high byte; without it that byte is the font.
u32 code_walk(u32 node, u32 value, u32 code, u32 out)
{
    for (;;) {
        const u32 n = node_at(node);
        const u16 kind = rd16(n);
        if (kind == 0) {
            const u32 l = range_list(value & 0xFF);
            const s32 i = range_find(l, code);
            if (i >= 0) {
                const u16 sub = rd16(l + 8 + 8 * static_cast<u32>(i)), val = rd16(l + 10 + 8 * static_cast<u32>(i));
                if (sub) code = code_walk(sub, val, code, out);
                else if (out) wr32(out, val);
            }
            if (rd32(out)) return code;
        } else if (kind == 1) {
            if (!out) return code;
            const u32 lo = rd32(n + 4), hi = rd32(n + 8);
            if (code >= lo && hi >= code) {
                const u32 d = code - lo;
                if (rd8(n + 12 + (d >> 3)) & (0x80u >> (d & 7))) {
                    wr32(out, value & 0xFF);
                    return code;
                }
            }
        } else if (kind == 2 || kind == 3) {
            if (!out) return code;
            const u32 cnt = rd32(n + 4), stride = kind == 2 ? 4 : 8;
            if (cnt) {
                s32 hi = static_cast<s32>(cnt - 1), lo = 0;
                u32 mid = static_cast<u32>(hi) >> 1;
                for (u32 left = cnt;;) {
                    const u32 at = n + 8 + mid * stride;
                    const u32 c = rd32(at);
                    if (code == c) {
                        wr32(out, value & 0xFF);
                        if (kind == 3 && (value & 0xFF)) return rd32(at + 4);
                        return code;
                    }
                    if (code < c) {
                        hi = static_cast<s32>(mid) - 1;
                        if (lo == static_cast<s32>(mid)) break;
                    } else {
                        if (static_cast<s32>(mid) == hi) break;
                        lo = static_cast<s32>(mid) + 1;
                    }
                    if (--left == 0) break;
                    mid = static_cast<u32>(lo + hi) >> 1;
                }
            }
        } else {
            return code;
        }
        if (!(node & 0x2000)) {
            wr32(out, value >> 8);
            return code;
        }
        const u32 l = range_list(value >> 8);
        const s32 i = range_find(l, code);
        if (i < 0) return code;
        node = rd16(l + 8 + 8 * static_cast<u32>(i));
        value = rd16(l + 10 + 8 * static_cast<u32>(i));
        if (!node) {
            wr32(out, value);
            return code;
        }
    }
}

// fn_c4c0: which font of the fontset draws `code`, and as which code.
s32 fontset_code(u32 fs, u32 code, u32 font_out, u32 code_out)
{
    if (!fs) return Es(0x102);
    const u32 l = rd32(fs + fsx::codes);
    if (!l || !code) {
        if (font_out) wr32(font_out, 0);
    } else if (const s32 i = range_find(l, code); i >= 0) {
        const u16 node = rd16(l + 8 + 8 * static_cast<u32>(i)), value = rd16(l + 10 + 8 * static_cast<u32>(i));
        if (!node) {
            if (font_out) wr32(font_out, value);
        } else {
            code = code_walk(node, value, code, font_out);
            if (!rd32(font_out)) {
                wr32(font_out, value & 0xFF);
                if (!(value & 0xFF)) {
                    if (!(node & 0x2000)) {
                        wr32(font_out, value >> 8);
                    } else {
                        const u32 l2 = range_list((value >> 8) & 0xFF);
                        if (const s32 j = range_find(l2, code); j >= 0) {
                            const u16 n2 = rd16(l2 + 8 + 8 * static_cast<u32>(j)),
                                      v2 = rd16(l2 + 10 + 8 * static_cast<u32>(j));
                            if (!n2) wr32(font_out, v2);
                            else code = code_walk(n2, v2, code, font_out);
                        }
                    }
                }
            }
        }
    }
    if (code_out) wr32(code_out, code);
    return 0;
}

} // namespace cfont

namespace cfont {

// ------------------------------------------------------------------------------------------------- fonts
// A CellFont: +2 its kind (0 a fontset; else one font), +4 its entry, +8 its ID, +0xC its font table, +0x10 the
// library, +0x14 the bound renderer, +0x18 the fontset state, +0x38 its number of fonts, +0x40/+0x44 DPI, +0x48 scale
// in points, +0x50 in pixels, +0x58 weight - 1, +0x5C slant, +0x60 the last code, +0x64 its font, +0x68 its code in
// that font, +0x6C/+0x70/+0x74 that font's scale, shift and weight factors, +0x78 the layouts' validity, +0x7C the
// renderer's horizontal layout, +0x94 the y scale last used, +0x9A the open mode, +0xA0.. a font extension.
namespace cf {
constexpr u32 kind = 2, entry = 4, id = 8, table = 0xC, library = 0x10, renderer = 0x14, fs = 0x18, fonts = 0x38,
              dpi_x = 0x40, dpi_y = 0x44, point_x = 0x48, point_y = 0x4C, pixel_x = 0x50, pixel_y = 0x54,
              weight = 0x58, slant = 0x5C, last_code = 0x60, cur_font = 0x64, cur_code = 0x68, f_scale = 0x6C,
              f_shift = 0x70, f_weight = 0x74, layouts = 0x78, hlayout = 0x7C, last_y = 0x94,
              ext = 0xA0, ext_vlayout = 0xA4, ext_hlayout = 0xA8, ext_id = 0xAC, ext_metrics = 0xB0, ext_render = 0xB4;
}
// A CellFontRenderer: +0x08/+0x0C DPI, +0x10 scale in points, +0x18 in pixels, +0x20 weight - 1, +0x24 slant,
// +0x40 the library, +0x44 the font library's renderer.
namespace rn {
constexpr u32 point_x = 0x10, point_y = 0x14, pixel_x = 0x18, pixel_y = 0x1C, weight = 0x20,
              slant = 0x24, library = 0x40;
}

// The font of the fontset that draws `code` (cached in the CellFont with its factors): 0 when the fontset has none.
s32 select_font(u32 font, u32 code)
{
    const u32 before = rd32(font + cf::cur_font);
    const s32 r = fontset_code(font + cf::fs, code, font + cf::cur_font, font + cf::cur_code);
    if (r) return r;
    const u32 now = rd32(font + cf::cur_font);
    wr32(font + cf::last_code, code);
    if (before != now) {
        Local info(8);
        font_info(now, info.addr());
        const u32 i = info.get();
        wr32(font + cf::f_scale, rd32(i + 4));
        wr32(font + cf::f_shift, rd32(i + 8));
        wr32(font + cf::f_weight, rd32(i + 12));
    }
    return 0;
}

void forget_font(u32 font)
{
    wr32(font + cf::cur_font, 0);
    wr32(font + cf::cur_code, 0);
    wr32(font + cf::last_code, 0);
}

void zero(u32 a, u32 words)
{
    for (u32 i = 0; i < words; i++) wr32(a + 4 * i, 0);
}

// fn_35ac: the kerning of two codes at a scale (0 when they are in different fonts of a fontset).
s32 kerning(float sx, float sy, u32 font, u32 c1, u32 c2, u32 out)
{
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!table || !lib) return Es(2);
    if (!c1 || !c2) {
        zero(out, 2);
        return 0;
    }
    u32 fid = 0, code1 = c1, code2 = c2;
    float fx = sx, fy;
    if (!rd16(font + cf::kind)) {
        if (rd32(font + cf::last_code) != c1 && select_font(font, c1)) {
            forget_font(font);
            zero(out, 2);
            return 0;
        }
        const u32 f1 = rd32(font + cf::cur_font);
        code1 = rd32(font + cf::cur_code);
        if (rd32(font + cf::last_code) != c2 && select_font(font, c2)) {
            forget_font(font);
            zero(out, 2);
            return 0;
        }
        const u32 f2 = rd32(font + cf::cur_font);
        code2 = rd32(font + cf::cur_code);
        if (f1 != f2 || !f1) {
            zero(out, 2);
            return 0;
        }
        fid = f1;
        fy = sy * rdf(font + cf::f_scale);
        wrf(font + cf::last_y, sy);
        fx = sx * rdf(font + cf::f_scale);
        if (rd32(font + cf::fs + fsx::adjust)) {
            Local v(16);
            if (!code_adjust(font + cf::fs, code2, v.addr(), fid)) {
                fx = fx * rdf(v.addr());
                fy = fy * rdf(v.addr() + 4);
            }
        }
    } else {
        fy = sy * rdf(font + cf::f_scale);
        fid = rd32(font + cf::entry);
        wrf(font + cf::last_y, sy);
    }
    s32 r = Es(1);
    if (const u32 e = entry(table, fid)) {
        lock_word(e, 0xA0000000u);
        const u32 h = rd32(e + 12);
        r = calls(tab(lib, 44), {h}, {fx, fy});
        if (!r) r = calls(tab(lib, 128), {h, code1, code2, out});
        unlock_word(e);
        if (!r) return 0;
    }
    zero(out, 2);
    return r;
}

// fn_3abc: a code's metrics at a scale, weight and slant (vertical ones with shift type k).
s32 char_metrics(float sx, float sy, float weight, float slant, u32 font, u32 code, u32 out, u32 k)
{
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!out || !table || !lib) return Es(2);
    if (const u32 ext = rd32(font + cf::ext)) {
        const s32 r = calls(rd32(font + cf::ext_metrics), {ext, code, out});
        if (r != Es(0x11)) return r;
    }
    u32 fid;
    float fx = sx, fy, dy = 0.0f, dx = 0.0f;
    s32 r;
    if (!rd16(font + cf::kind)) {
        if (rd32(font + cf::last_code) != code && select_font(font, code)) {
            forget_font(font);
            zero(out, 8);
            return Es(0x12);
        }
        fid = rd32(font + cf::cur_font);
        code = rd32(font + cf::cur_code);
        if (!fid) {
            zero(out, 8);
            return Es(0x11);
        }
        fy = sy * rdf(font + cf::f_scale);
        wrf(font + cf::last_y, sy);
        fx = sx * rdf(font + cf::f_scale);
        const float fw = rdf(font + cf::f_weight);
        float ox = 0.0f, oy = 0.0f;
        Local v(16);
        if (rd32(font + cf::fs + fsx::adjust) && !code_adjust(font + cf::fs, code, v.addr(), fid)) {
            fx = fx * rdf(v.addr());
            fy = fy * rdf(v.addr() + 4);
            dx = std::fma(fx, rdf(v.addr() + 12), 0.0f);
            oy = std::fma(fy, rdf(v.addr() + 8), 0.0f);
        }
        (void)ox;
        dy = std::fma(fy, rdf(font + cf::f_shift), oy);
        weight = std::fma(weight, fw, fw) - 1.0f;
    } else {
        fy = sy * rdf(font + cf::f_scale);
        fid = rd32(font + cf::entry);
        wrf(font + cf::last_y, sy);
    }
    const u32 e = entry(table, fid);
    if (!e) {
        zero(out, 8);
        return Es(1);
    }
    lock_word(e, 0xA0000000u);
    const u32 h = rd32(e + 12);
    r = calls(tab(lib, 44), {h}, {fx, fy});
    if (!r) r = calls(tab(lib, 48), {h}, {weight});
    if (!r) r = calls(tab(lib, 52), {h}, {slant});
    if (!r) {
        calls(tab(lib, 140), {h}, {rdf(font + cf::last_y)});
        r = k ? calls(tab(lib, 68), {h, code, out, k}) : calls(tab(lib, 64), {h, code, out});
    }
    unlock_word(e);
    if (!r) {
        wrf(out + 12, rdf(out + 12) - dy);
        wrf(out + 20, rdf(out + 20) - dx);
        return 0;
    }
    zero(out, 8);
    return r;
}

// fn_407c: the vertical layout over the font's fonts (the widest line).
s32 vertical_layout(float sx, float sy, float weight, float slant, u32 font, u32 out)
{
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!table || !lib) return Es(2);
    float lo = 0.0f, hi = 0.0f, effect = 0.0f;
    s32 r = 0;
    const auto finish = [&](float line) {
        wrf(out + 8, effect);
        wrf(out, lo);
        wrf(out + 4, line);
        return r;
    };
    for (u32 i = 0; i < rd32(font + cf::fonts); i++) {
        u32 fid;
        float f = 1.0f, w = weight;
        if (!rd16(font + cf::kind)) {
            Local id(8), info(8);
            if (fontset_font(font + cf::fs, i, id.addr()) || font_info(id.get(), info.addr())) {
                r = Es(1);
                return finish(hi - lo);
            }
            fid = id.get();
            const u32 in = info.get();
            w = std::fma(weight, rdf(in + 12), rdf(in + 12)) - 1.0f;
            f = rdf(in + 4);
        } else {
            if (i) {
                r = Es(2);
                return finish(hi - lo);
            }
            fid = rd32(font + cf::entry);
        }
        const u32 e = entry(table, fid);
        if (!e) return Es(1);
        const u32 state = lock_word(e, 0xA0000000u);
        Local l(12);
        r = Es(0xb);
        if (state & 0x40000000u) {
            const u32 h = rd32(e + 12);
            r = calls(tab(lib, 44), {h}, {f * sx, f * sy});
            if (!r) r = calls(tab(lib, 48), {h}, {w});
            if (!r) r = calls(tab(lib, 52), {h}, {slant});
            if (!r) r = calls(tab(lib, 60), {h, l.addr()});
            else r = Es(1);
        }
        unlock_word(e);
        if (r) {
            lo = 0.0f;
            return finish(0.0f);
        }
        const float base = rdf(l.addr());
        if (base < lo) {
            lo = base;
            effect = rdf(l.addr() + 8);
        }
        const float wd = base + rdf(l.addr() + 4);
        if (wd > hi) hi = wd;
    }
    r = 0;
    if (const u32 ext = rd32(font + cf::ext)) {
        if (const u32 fn = rd32(font + cf::ext_hlayout)) {
            Local l(12);
            r = calls(fn, {ext, l.addr()});
            if (!r) {
                const float base = rdf(l.addr());
                if (lo > base) lo = base;
                const float wd = base + rdf(l.addr() + 4);
                if (hi < wd) return finish(wd - lo);
            }
        }
    }
    return finish(hi - lo);
}

// fn_455c: the horizontal layout over the font's fonts (the highest ascent and the deepest line).
s32 horizontal_layout(float sx, float sy, float weight, u32 font, u32 out)
{
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!table || !lib) return Es(2);
    float base = 0.0f, line = 0.0f, effect = 0.0f;
    s32 r = 0;
    const auto finish = [&](float h) {
        wrf(out + 8, effect);
        wrf(out, base);
        wrf(out + 4, h);
        return r;
    };
    for (u32 i = 0; i < rd32(font + cf::fonts); i++) {
        u32 fid;
        float f = 1.0f, shift = 0.0f, w = weight;
        if (!rd16(font + cf::kind)) {
            Local id(8), info(8);
            if (fontset_font(font + cf::fs, i, id.addr()) || font_info(id.get(), info.addr())) {
                r = Es(1);
                return finish(line + base);
            }
            fid = id.get();
            const u32 in = info.get();
            const float a = std::fma(weight, rdf(in + 12), rdf(in + 12));
            shift = sy * rdf(in + 8);
            f = rdf(in + 4);
            w = a - 1.0f;
        } else {
            if (i) {
                r = Es(2);
                return finish(line + base);
            }
            fid = rd32(font + cf::entry);
        }
        const u32 e = entry(table, fid);
        if (!e) return Es(1);
        const u32 state = lock_word(e, 0xA0000000u);
        Local l(12);
        r = Es(0xb);
        if (state & 0x40000000u) {
            const u32 h = rd32(e + 12);
            r = calls(tab(lib, 44), {h}, {f * sx, f * sy});
            if (!r) r = calls(tab(lib, 48), {h}, {w});
            if (!r) r = calls(tab(lib, 56), {h, l.addr()});
            else r = Es(1);
        }
        unlock_word(e);
        if (r) {
            base = 0.0f;
            return finish(0.0f);
        }
        const float b = rdf(l.addr()) - shift;
        if (b > base) {
            base = b;
            effect = rdf(l.addr() + 8);
        }
        const float h = rdf(l.addr() + 4) - b;
        if (h > line) line = h;
    }
    r = 0;
    if (const u32 ext = rd32(font + cf::ext)) {
        if (const u32 fn = rd32(font + cf::ext_vlayout)) {
            Local l(12);
            r = calls(fn, {ext, l.addr()});
            if (!r) {
                const float b = rdf(l.addr());
                if (base < b) base = b;
                const float h = rdf(l.addr() + 4) - b;
                if (line < h) return finish(base + h);
            }
        }
    }
    return finish(base + line);
}

} // namespace cfont

namespace {

using namespace cfont;

// ------------------------------------------------------------------------------------------------- simple calls

void cellFontGetRevisionFlags(PPUContext& ctx, u32 flags)
{
    Scope sc(ctx);
    if (flags) wr64(flags, 100);
}

s32 cellFontGetLibrary(PPUContext& ctx, u32 font, u32 lib, u32 type)
{
    Scope sc(ctx);
    if (!font || !lib) return Es(2);
    wr32(lib, rd32(font + cf::library));
    if (!rd32(font + cf::library)) return Es(3);
    if (type) wr32(type, rd16(rd32(font + cf::library) + 2));
    return 0;
}

s32 cellFontCreateRenderer(PPUContext& ctx, u32 lib, u32 cfg_, u32 r)
{
    Scope sc(ctx);
    if (!lib || !cfg_ || !r) return Es(2);
    const u32 fn = rd32(lib + 0x54);
    if (!fn) return Es(0x10);
    const s32 e = calls(fn, {cfg_, r});
    if (e) {
        wr32(r + 0x40, 0);
        return e;
    }
    wr32(r + 0x34, 0);
    wr32(r + 0x40, lib);
    wr32(r + 8, 72);
    wrf(r + 0x20, 1.0f);
    wr32(r + 0x24, 0);
    wr16(r + 2, 0);
    wr32(r + 4, 0);
    wr32(r + 0xC, 72);
    wr32(r + 0x14, 0);
    wr32(r + 0x10, 0);
    wr32(r + 0x1C, 0);
    wr32(r + 0x18, 0);
    wr32(r + 0x28, 0);
    wr32(r + 0x2C, 0);
    wr32(r + 0x30, 0);
    return 0;
}

s32 cellFontDestroyRenderer(PPUContext& ctx, u32 r)
{
    Scope sc(ctx);
    if (!r || !rd32(r + 0x40)) return Es(2);
    const u32 fn = rd32(rd32(r + 0x40) + 0x58);
    if (!fn) return Es(0x10);
    const s32 e = calls(fn, {r});
    if (!e) wr32(r + 0x40, 0);
    return e;
}

s32 cellFontAdjustFontScaling(PPUContext& ctx, u32 font, float scale)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (!rd16(font + cf::kind)) return Es(0x10);
    if (scale > 2.0f || scale < 0.5f) return Es(2);
    wrf(font + 0x6C, scale);
    return 0;
}

s32 cellFontSetResolutionDpi(PPUContext& ctx, u32 font, u32 h, u32 v)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    wr32(font + cf::dpi_x, h ? h : 72);
    wr32(font + cf::dpi_y, v ? v : 72);
    return 0;
}

s32 cellFontGetResolutionDpi(PPUContext& ctx, u32 font, u32 h, u32 v)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (h) wr32(h, rd32(font + cf::dpi_x));
    if (v) wr32(v, rd32(font + cf::dpi_y));
    return 0;
}

s32 cellFontSetScalePoint(PPUContext& ctx, u32 font, float w, float h)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    wrf(font + cf::point_x, w);
    wrf(font + cf::point_y, h);
    wrf(font + cf::pixel_x, (w * u2f(rd32(font + cf::dpi_x))) / 72.0f);
    wrf(font + cf::pixel_y, (h * u2f(rd32(font + cf::dpi_y))) / 72.0f);
    return 0;
}

s32 cellFontSetScalePixel(PPUContext& ctx, u32 font, float w, float h)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    const float dx = u2f(rd32(font + cf::dpi_x)), dy = u2f(rd32(font + cf::dpi_y));
    wrf(font + cf::pixel_y, h);
    wrf(font + cf::pixel_x, w);
    wrf(font + cf::point_y, (h * 72.0f) / dy);
    wrf(font + cf::point_x, (w * 72.0f) / dx);
    return 0;
}

s32 cellFontGetScalePoint(PPUContext& ctx, u32 font, u32 w, u32 h)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (w) wr32(w, rd32(font + cf::point_x));
    if (h) wr32(h, rd32(font + cf::point_y));
    return 0;
}

s32 cellFontGetScalePixel(PPUContext& ctx, u32 font, u32 w, u32 h)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (w) wr32(w, rd32(font + cf::pixel_x));
    if (h) wr32(h, rd32(font + cf::pixel_y));
    return 0;
}

// The weight is kept as weight - 1, within [-0.07, 0.04] (libfont's constants).
float clamp_weight(float w)
{
    const float d = w - 1.0f;
    const float top = std::bit_cast<float>(rd32(va(0x12A1C))), bottom = std::bit_cast<float>(rd32(va(0x12A20)));
    if (d > top) return top;
    if (d < bottom) return bottom;
    return d;
}

float clamp_slant(float s)
{
    if (s > 1.0f) return 1.0f;
    if (s < -1.0f) return -1.0f;
    return s;
}

s32 cellFontSetEffectWeight(PPUContext& ctx, u32 font, float w)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (!has_image()) return Es(1);
    wrf(font + cf::weight, clamp_weight(w));
    return 0;
}

s32 cellFontGetEffectWeight(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    if (!font || !out) return Es(2);
    wrf(out, rdf(font + cf::weight) + 1.0f);
    return 0;
}

s32 cellFontSetEffectSlant(PPUContext& ctx, u32 font, float s)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    wrf(font + cf::slant, clamp_slant(s));
    return 0;
}

s32 cellFontGetEffectSlant(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    if (!font || !out) return Es(2);
    wr32(out, rd32(font + cf::slant));
    return 0;
}

// ---- glyphs' environments (CellFontGlyph's +0x3C)

s32 cellFontGlyphGetHorizontalShift(PPUContext& ctx, u32 glyph, u32 x, u32 y)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    if (disabled(0x0100000000000000ull)) return Es(0x10);
    if (!glyph || !rd32(glyph + 0x3C)) return Es(2);
    const u32 env = rd32(glyph + 0x3C);
    if (x) wr32(x, 0);
    if (y) wr32(y, rd32(env + 0x48));
    return 0;
}

s32 cellFontGlyphGetVerticalShift(PPUContext& ctx, u32 glyph, u32 x, u32 y)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    if (disabled(0x0080000000000000ull)) return Es(0x10);
    if (!glyph || !rd32(glyph + 0x3C)) return Es(2);
    const u32 env = rd32(glyph + 0x3C);
    const float neg = -rdf(env + 0x48);
    float v;
    if (rdf(env + 0x4C) == 0.0f) v = std::fma(rdf(env + 4) / rdf(env + 0x14), rdf(env + 0x18), neg);
    else v = neg + rdf(env + 0x4C);
    if (x) wr32(x, rd32(env + 0x38));
    if (y) wrf(y, v + rdf(env + 0x3C));
    return 0;
}

s32 cellFontGlyphGetScalePixel(PPUContext& ctx, u32 glyph, u32 w, u32 h)
{
    Scope sc(ctx);
    if (!glyph) return Es(2);
    const u32 env = rd32(glyph + 0x3C);
    if (!env) {
        if (w) wr32(w, 0);
        if (h) wr32(h, 0);
        return Es(2);
    }
    if (w) wr32(w, rd32(env));
    if (h) wr32(h, rd32(env + 4));
    return 0;
}

s32 cellFontGlyphGetOutlineControlDistance(PPUContext& ctx, u32 glyph, float max_scale, float base, u32 out)
{
    Scope sc(ctx);
    if (!glyph || !out) return Es(2);
    if (!(max_scale > 0.0f)) return Es(2);
    const u32 env = rd32(glyph + 0x3C);
    float m = rdf(env + 4);
    if (rdf(env) >= m) m = rdf(env);
    wrf(out, (m / max_scale) * base);
    return 0;
}

s32 cellFontDeleteGlyph(PPUContext& ctx, u32 font, u32 glyph)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    if (disabled(0x2000000000000000ull)) return Es(0x10);
    if (!font || !rd32(font + cf::library) || !glyph) return Es(2);
    const u32 lib = rd32(font + cf::library);
    return calls(rd32(lib + 0x50), {lib, glyph});
}

// ---- renderers bound to fonts

s32 cellFontBindRenderer(PPUContext& ctx, u32 font, u32 r)
{
    Scope sc(ctx);
    if (!font || !r || !rd32(r + rn::library)) return Es(2);
    if (rd32(font + cf::renderer)) return Es(0x20);
    for (u32 i = 0; i < 8; i++) wr32(r + 8 + 4 * i, rd32(font + cf::dpi_x + 4 * i));
    wr32(font + cf::renderer, r);
    return 0;
}

s32 cellFontGetBindingRenderer(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    if (!font) {
        wr32(out, 0);
        return Es(2);
    }
    const u32 r = rd32(font + cf::renderer);
    wr32(out, r);
    return r ? 0 : Es(0x21);
}

s32 cellFontUnbindRenderer(PPUContext& ctx, u32 font)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (!rd32(font + cf::renderer)) return Es(0x21);
    wr32(font + cf::renderer, 0);
    return 0;
}

// The font's renderer, after libfont's checks (0 with the error in `e`).
u32 bound(u32 font, s32& e)
{
    if (!font || !rd32(font + cf::table) || !rd32(font + cf::library)) {
        e = Es(2);
        return 0;
    }
    const u32 r = rd32(font + cf::renderer);
    e = r ? 0 : Es(0x21);
    return r;
}

s32 cellFontSetupRenderScalePoint(PPUContext& ctx, u32 font, float w, float h)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    if (w == 0.0f) w = rdf(font + cf::point_x);
    if (h == 0.0f) h = rdf(font + cf::point_y);
    wrf(r + rn::point_x, w);
    wrf(r + rn::point_y, h);
    wrf(r + rn::pixel_x, (w * u2f(rd32(font + cf::dpi_x))) / 72.0f);
    wrf(r + rn::pixel_y, (h * u2f(rd32(font + cf::dpi_y))) / 72.0f);
    wr32(font + cf::layouts, 0);
    return 0;
}

s32 cellFontSetupRenderScalePixel(PPUContext& ctx, u32 font, float w, float h)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    if (w == 0.0f) w = rdf(font + cf::pixel_x);
    if (h == 0.0f) h = rdf(font + cf::pixel_y);
    const float px = w * 72.0f, py = h * 72.0f;
    wrf(r + rn::point_x, px / u2f(rd32(font + cf::dpi_x)));
    wrf(r + rn::pixel_y, h);
    wrf(r + rn::pixel_x, w);
    wrf(r + rn::point_y, py / u2f(rd32(font + cf::dpi_y)));
    wr32(font + cf::layouts, 0);
    return 0;
}

s32 cellFontGetRenderScalePoint(PPUContext& ctx, u32 font, u32 w, u32 h)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    if (w) wr32(w, rd32(r + rn::point_x));
    if (h) wr32(h, rd32(r + rn::point_y));
    return 0;
}

s32 cellFontGetRenderScalePixel(PPUContext& ctx, u32 font, u32 w, u32 h)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    if (w) wr32(w, rd32(r + rn::pixel_x));
    if (h) wr32(h, rd32(r + rn::pixel_y));
    return 0;
}

s32 cellFontSetupRenderEffectWeight(PPUContext& ctx, u32 font, float w)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    if (!has_image()) return Es(1);
    wrf(r + rn::weight, clamp_weight(w));
    wr32(font + cf::layouts, 0);
    return 0;
}

s32 cellFontSetupRenderEffectSlant(PPUContext& ctx, u32 font, float s)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    wrf(r + rn::slant, clamp_slant(s));
    wr32(font + cf::layouts, 0);
    return 0;
}

s32 cellFontGetRenderEffectWeight(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    wrf(out, rdf(r + rn::weight) + 1.0f);
    return 0;
}

s32 cellFontGetRenderEffectSlant(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    s32 e;
    const u32 r = bound(font, e);
    if (!r) return e;
    wr32(out, rd32(r + rn::slant));
    return 0;
}

void cellFontRenderSurfaceInit(PPUContext& ctx, u32 s, u32 buffer, s32 width_byte, s32 pixel_size, s32 w, s32 h)
{
    Scope sc(ctx);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (!s) return;
    wr32(s + 32, static_cast<u32>(h));
    wr32(s, buffer);
    wr32(s + 4, static_cast<u32>(width_byte));
    wr32(s + 8, static_cast<u32>(pixel_size));
    wr32(s + 24, 0);
    wr32(s + 28, static_cast<u32>(w));
    wr32(s + 12, static_cast<u32>(w));
    wr32(s + 16, static_cast<u32>(h));
    wr32(s + 20, 0);
}

// ---- graphics draw contexts ({0xCF50, ...}: DPI, scales, draw type, colours)

bool is_draw_context(u32 c) { return c && rd16(c) == 0xCF50; }

s32 cellFontGraphicsSetupDrawContext(PPUContext& ctx, u32 g, u32 c)
{
    Scope sc(ctx);
    if (!g || !c) return Es(2);
    for (u32 i = 0; i < 0x100; i += 4) wr32(c + i, 0);
    wr16(c, 0xCF50);
    wr16(c + 2, 0);
    wr32(c + 4, 0);
    wr32(c + 8, 72);
    wr32(c + 0xC, 72);
    wr16(c + 0x40, 0);
    wr16(c + 0x42, 1);
    for (const u32 o : {0x50u, 0x54u, 0x58u, 0x5Cu, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu, 0x80u, 0x94u, 0xA8u, 0xBCu})
        wrf(c + o, 1.0f);
    return 0;
}

s32 cellFont_8D5094C4(PPUContext& ctx, u32 c, u64 bits)
{
    Scope sc(ctx);
    if (!is_draw_context(c)) return Es(2);
    const u16 set = static_cast<u16>(bits), clear = static_cast<u16>(bits >> 16);
    wr16(c + 0x40, static_cast<u16>((set | rd16(c + 0x40)) & ~clear));
    return 0;
}

s32 cellFont_47B70477(PPUContext& ctx, u32 c, u32 out)
{
    Scope sc(ctx);
    if (!is_draw_context(c) || !out) return Es(2);
    const u32 f = rd16(c + 0x40);
    wr32(out, f | (~f << 16));
    return 0;
}

s32 cellFontGraphicsSetDrawType(PPUContext& ctx, u32 c, u32 type)
{
    Scope sc(ctx);
    if (!is_draw_context(c)) return Es(2);
    wr16(c + 0x42, type);
    return 0;
}

s32 cellFontGraphicsGetDrawType(PPUContext& ctx, u32 c, u32 out)
{
    Scope sc(ctx);
    if (!is_draw_context(c) || !out) return Es(2);
    wr32(out, rd16(c + 0x42));
    return 0;
}

s32 rgba_set(u32 c, u32 at, u32 in)
{
    if (!is_draw_context(c) || !in) return Es(2);
    for (u32 i = 0; i < 4; i++) wr32(c + at + 4 * i, rd32(in + 4 * i));
    return 0;
}

s32 rgba_get(u32 c, u32 at, u32 out)
{
    if (!is_draw_context(c) || !out) return Es(2);
    for (u32 i = 0; i < 4; i++) wr32(out + 4 * i, rd32(c + at + 4 * i));
    return 0;
}

s32 cellFontGraphicsSetFontRGBA(PPUContext& ctx, u32 c, u32 in)
{
    Scope sc(ctx);
    return rgba_set(c, 0x70, in);
}
s32 cellFontGraphicsGetFontRGBA(PPUContext& ctx, u32 c, u32 out)
{
    Scope sc(ctx);
    return rgba_get(c, 0x70, out);
}
s32 cellFontGraphicsSetLineRGBA(PPUContext& ctx, u32 c, u32 in)
{
    Scope sc(ctx);
    return rgba_set(c, 0x60, in);
}
s32 cellFontGraphicsGetLineRGBA(PPUContext& ctx, u32 c, u32 out)
{
    Scope sc(ctx);
    return rgba_get(c, 0x60, out);
}

s32 cellFontGraphicsSetScalePixel(PPUContext& ctx, u32 c, float w, float h)
{
    Scope sc(ctx);
    if (!is_draw_context(c)) return Es(2);
    wrf(c + 0x1C, h);
    wrf(c + 0x18, w);
    wrf(c + 0x10, (w * 72.0f) / u2f(rd32(c + 8)));
    wrf(c + 0x14, (h * 72.0f) / u2f(rd32(c + 0xC)));
    return 0;
}

s32 cellFontGraphicsGetScalePixel(PPUContext& ctx, u32 c, u32 w, u32 h)
{
    Scope sc(ctx);
    if (!is_draw_context(c)) {
        if (w) wr32(w, 0);
        if (h) wr32(h, 0);
        return Es(2);
    }
    if (w) wr32(w, rd32(c + 0x18));
    if (h) wr32(h, rd32(c + 0x1C));
    return 0;
}

} // namespace

namespace cfont {

// ------------------------------------------------------------------------------------------------- set-up
// fn_bc98(0, 0): the system fonts' tables made from libfont's own data (the header in the tables' record, the
// node table's 64-bit addresses).
void load_tables()
{
    const u32 t = tables();
    const u32 hdr = t + 0x30;
    wr32(t + 0x10, hdr);
    wr32(t + 0x30, 0);
    wr32(t + 0x38, 0);
    wr32(t + 0x34, 7);
    const std::pair<u32, u32> fields[] = {{0xC, 0}, {0x10, 0}, {0x14, 0x28}, {0x20, 0x65}, {0x24, 0}, {0x28, 0x86},
        {0x2C, 0}, {0x30, 0x22}, {0x34, 0}, {0x38, 4}, {0x3C, 0}};
    for (const auto& [o, v] : fields) wr32(hdr + o, v);
    wr32(t + 0x14, toc(0x12A98));
    wr32(t + 0x18, toc(0x12A9C));
    wr32(t + 0x1C, toc(0x12AA0));
    wr32(t + 0x24, toc(0x12AD0));
    const u32 n = toc(0x12A7C);
    const u64 base = toc(0x12A80);
    const std::pair<u32, u64> nodes[] = {{0x00, 0}, {0x08, u64{t} + 4}, {0x10, base}, {0x18, base + 0x5C},
        {0x20, base + 0x88}, {0x28, toc(0x12AD4)}, {0x30, toc(0x12AD8)}, {0x38, base + 300}, {0x40, base + 0x188},
        {0x48, base + 0x1B8}, {0x50, base + 0x1E8}, {0x58, base + 0x2D4}, {0x60, base + 0x390}, {0x68, base + 0x3FC},
        {0x70, base + 0xE48}, {0x78, base + 0xF04}, {0x80, base + 0xF14}, {0x88, base + 0xF24}, {0x90, base + 0xF5C},
        {0x98, base + 0xF68}, {0xA0, base + 0xFB4}, {0xA8, base + 0x10A0}, {0xB0, base + 0x1104}, {0xB8, toc(0x12AB0)},
        {0xC0, toc(0x12AB4)}, {0xC8, toc(0x12AB8)}, {0xD0, toc(0x12ABC)}, {0xD8, toc(0x12AC4)}, {0xE0, toc(0x12AC0)},
        {0xE8, toc(0x12AC8)}, {0xF0, base + 0x1254}, {0xF8, base + 0x1280}, {0x100, base + 0x12B4},
        {0x108, base + 0x1300}};
    for (const auto& [o, v] : nodes) wr64(n + o, v);
    wr32(t + 0x28, n);
    wr32(t + 0x2C, 0);
    wr32(t + 0x20, toc(0x12ACC));
}

// fn_c890: the tables loaded once (their record's word: bit 0).
s32 ensure_tables()
{
    const u32 w = tables() + 8;
    if (rd32(w) & 1) return 0;
    if (!(lock_word(w, 0x80000000u) & 0x7FFFFFFFu)) {
        load_tables();
        or_word(w, 1);
    }
    unlock_word(w);
    return 0;
}

// fn_b028: libfont's file cache over the game's buffer: {first slot, slots, next, free, size, block size}, then the
// slots of {owner, block, block size bytes}.
s32 cache_init(u32 c, u32 size, u32 block)
{
    if (!c || size <= 0x17) return static_cast<s32>(0x805400C1u);
    const u32 slot = block ? block + 8 : 0x2008;
    wr32(c + 20, block);
    wr32(c + 8, 0);
    wr32(c, c + 24);
    wr32(c + 16, size);
    const u32 n = (size - 0x18) / slot;
    wr32(c + 12, n);
    wr32(c + 4, n);
    return 0;
}

// The game's SDK version (lv2 sys_process_get_sdk_version): false when lv2 does not give it.
bool sdk_version(u32& out)
{
    PPUContext& ctx = *current_ctx();
    const u32 pid = static_cast<u32>(lv2_syscall(ctx, 1, {}));
    Local ver(8);
    if (static_cast<u32>(lv2_syscall(ctx, 25, {pid, ver.addr()}))) return false;
    out = ver.get();
    return true;
}

// fn_8c54: every font of `lib` in a font table closed.
void close_library_fonts(u32 table, u32 lib)
{
    lock_word(table, 0x80000000u);
    u32 i = 0;
    for (;;) {
        const u32 count = rd32(table + 4), entries = rd32(table + 8);
        if (count <= i) break;
        u32 j = i;
        while (j < count && rd32(entries + j * 16 + 8) != lib) j++;
        if (j >= count) break;
        i = j;
        const u32 e = entries + i * 16;
        const u32 v = lock_word(e, 0x80000000u);
        if (!(v & 0x40000000u)) {
            wr32(e + 8, 0);
            wr32(e + 4, 0);
        } else if (!calls(rd32(lib + 0x1C), {e + 12, 0})) {
            wr32(e + 8, 0);
            wr32(e + 4, 0);
            and_word(e, 0x80000000u);
        }
        unlock_word(e);
    }
    unlock_word(table);
}

} // namespace cfont

namespace {

using namespace cfont;

// The file cache's lock interface libfontFT calls (libfont's fn_77b4 and fn_777c on st + 0x58).
void pspocket_font_cache_lock(PPUContext& ctx, u32 word)
{
    Scope sc(ctx);
    lock_word(word, 0x80000000u);
}

void pspocket_font_cache_unlock(PPUContext& ctx, u32 word)
{
    Scope sc(ctx);
    unlock_word(word);
}

u32 cellFontStatic(PPUContext& ctx)
{
    Scope sc(ctx);
    return has_image() ? rd32(st_ptr()) : 0;
}

u32 cellFontsetUSleep(PPUContext& ctx)
{
    Scope sc(ctx);
    return has_image() ? rd32(usleep_slot()) : 0;
}

s32 cellFontControl(PPUContext& ctx, u32 cmd, u32 arg)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    const u32 c = cfg();
    if (cmd == 0x540002) {
        if (arg) {
            const u32 a = rd32(arg), b = rd32(arg + 4);
            wr32(c + 0x2C, b);
            wr32(c + 0x28, a);
            wr32(handler_slot() + 4, b);
            wr32(handler_slot(), a);
        }
    } else if (cmd == 0x540000) {
        wr32(c, arg);
    } else if (cmd == 0x540003) {
        if (arg) {
            wr32(arg + 4, rd32(c + 0x2C));
            wr32(arg, rd32(c + 0x28));
        }
    } else if (cmd == 0x540100) {
        wr32(tables(), arg);
    }
    return 0;
}

// fn_2fe0: the library initialized with the game's settings {file cache, its size, user font entries, their count}.
s32 initialize(u64 revision, u32 config)
{
    if (!config) return Es(2);
    const u32 s = st();
    const u32 was = lock_static();
    if (was) {
        unlock_static(was);
        return Es(6);
    }
    const u32 cache = rd32(config);
    if (cache_init(cache, rd32(config + 4), 0x2000)) {
        unlock_static(0);
        return Es(5);
    }
    if (u32 ver = 0; sdk_version(ver)) {
        u64 flags = ver <= 0x15FFFF ? 1 : 0;
        if (ver <= 0x46FFFF) flags |= 2;
        wr64(s + 0x48, flags);
    }
    const u32 c = cfg();
    wr32(s + 0x14, cache);
    wr64(s + 8, revision);
    for (u32 i = 0; i < 128; i++) {
        const u32 e = c + 0x40 + 16 * i;
        wr32(e + 12, 0);
        wr32(e, 0);
        wr32(e + 8, 0);
    }
    wr32(s + 0x20, 0);
    wr32(s + 0x28, c + 0x40);
    wr32(s + 0x24, 128);
    u32 n = rd32(config + 8);
    u32 entries = rd32(config + 12);
    if (!n || !entries) {
        n = 0;
        entries = 0;
    } else {
        for (u32 i = 0; i < n; i++) {
            const u32 e = entries + 16 * i;
            wr32(e + 12, 0);
            wr32(e, 0);
            wr32(e + 8, 0);
        }
    }
    wr32(s + 0x2C + 8, entries);
    wr32(s + 0x2C, 0);
    wr32(s + 0x2C + 4, n);
    if (rd32(c + 0x38)) PS_LOG_ERROR(kChan, "libfont's reserved system fonts (cellFontPatchWorks) are not kept: skipped");
    wr32(s + 100, desc("pspocket_font_cache_unlock"));
    wr32(s + 92, s + 88);
    wr32(s + 96, desc("pspocket_font_cache_lock"));
    unlock_static(1);
    return 0;
}

s32 cellFontInitializeWithRevision(PPUContext& ctx, u64 revision, u32 config)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    if (ensure_tables()) return Es(1);
    return initialize(revision, config);
}

// The initialization without the system font tables' set-up (an export with no known name: libfont's fn_2fe0).
s32 cellFont_57E03823(PPUContext& ctx, u64 revision, u32 config)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    return initialize(revision, config);
}

s32 cellFontGetInitializedRevisionFlags(PPUContext& ctx, u32 out)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    const u32 was = lock_static();
    s32 r = Es(3);
    if (!was) {
        if (out) wr64(out, 0);
    } else {
        r = 0;
        if (out) wr64(out, rd64(st() + 8));
    }
    unlock_static(was);
    return r;
}

s32 cellFontEnd(PPUContext& ctx)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    const u32 was = lock_static();
    s32 r = Es(3);
    if (was) {
        const u32 s = st();
        const u32 lib = rd32(s + 0x40);
        if (lib && rd32(lib + 0x10)) {
            close_library_fonts(s + 0x2C, lib);
            close_library_fonts(s + 0x20, lib);
            calls(rd32(lib + 0x10), {});
        }
        const u32 cache = rd32(s + 0x14);
        r = 0;
        if (!cache || !rd32(cache)) r = Es(1);
        else wr32(cache, 0);
    }
    unlock_static(0);
    return r;
}

// fn_af44: one of the library slots (st + 0x38 .. 0x44) holding `lib`.
u32 library_slot(u32 lib)
{
    const u32 s = rd32(st_ptr());
    for (const u32 o : {0x38u, 0x3Cu, 0x40u})
        if (rd32(s + o) == lib) return lib;
    return rd32(s + 0x44) == lib ? lib : 0;
}

s32 end_library(u32 lib)
{
    if (!has_image()) return Es(1);
    const u32 was = lock_static();
    if (was) {
        if (const u32 l = library_slot(lib); l && rd32(l + 0x10)) {
            const u32 s = st();
            close_library_fonts(s + 0x2C, l);
            close_library_fonts(s + 0x20, l);
            calls(rd32(l + 0x10), {});
            unlock_static(was);
            return 0;
        }
    }
    unlock_static(was);
    return Es(3);
}

s32 cellFontEndLibrary(PPUContext& ctx, u32 lib)
{
    Scope sc(ctx);
    return end_library(lib);
}

s32 cellFontClearFileCache(PPUContext& ctx)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    const u32 was = lock_static();
    s32 r = Es(3);
    if (was) {
        const u32 s = st();
        calls(rd32(s + 0x60), {rd32(s + 0x5C)});
        const u32 cache = rd32(s + 0x14);
        s32 e = static_cast<s32>(0x805400C1u);
        if (cache) {
            wr32(cache + 12, rd32(cache + 4));
            wr32(cache + 8, 0);
            e = 0;
        }
        calls(rd32(s + 0x64), {rd32(s + 0x5C)});
        r = e ? Es(1) : 0;
    }
    unlock_static(was);
    return r;
}

s32 cellFontSetFontOpenMode(PPUContext& ctx, u32 lib, u32 mode)
{
    Scope sc(ctx);
    if (!has_image() || !rd32(st_ptr())) return Es(3);
    if (!lib) return Es(2);
    wr16(lib + 0x9A, mode);
    return 0;
}

s32 cellFontSetFontsetOpenMode(PPUContext& ctx, u32 lib, u32 mode)
{
    Scope sc(ctx);
    const u32 s = has_image() ? rd32(st_ptr()) : 0;
    if (!s) return Es(3);
    if (!lib) return Es(2);
    lock_word(s + 0x20, 0x80000000u);
    const u32 n = rd32(s + 0x24), entries = rd32(s + 0x28);
    bool used = false;
    for (u32 i = 0; i < n; i++)
        if (rd32(entries + 16 * i + 8) == lib) {
            used = true;
            break;
        }
    if (!used) wr16(lib + 0x98, mode);
    unlock_word(s + 0x20);
    return 0;
}

} // namespace

namespace {

using namespace cfont;

void cellFontRenderSurfaceSetScissor(PPUContext& ctx, u32 s, u32 x, u32 y, u32 w, u32 h)
{
    Scope sc(ctx);
    if (!s) return;
    const u32 stat = has_image() ? rd32(st_ptr()) : 0;
    u64 flags = 0;
    if (stat) {
        flags = rd64(stat + 0x48);
    } else if (u32 ver = 0; sdk_version(ver)) {
        flags = ver <= 0x15FFFF ? 1 : 0;
        if (ver <= 0x46FFFF) flags |= 2;
    }
    if (!(flags & 1)) {
        if (const u32 W = rd32(s + 12)) {
            u32 x0, x1;
            if (static_cast<s32>(x) < 0) {
                x0 = 0;
                x1 = 0;
                if (0u - x < w) {
                    const u32 e = x + w;
                    x1 = W > e ? e : W;
                }
            } else if (W < x) {
                x0 = W;
                x1 = W;
            } else {
                const u32 e = x + (W > w ? w : W);
                x0 = x;
                x1 = e > W ? W : e;
            }
            wr32(s + 20, x0);
            wr32(s + 28, x1);
        }
        if (const u32 H = rd32(s + 16)) {
            u32 y0, y1;
            if (static_cast<s32>(y) < 0) {
                y0 = 0;
                y1 = 0;
                if (0u - y < h) {
                    const u32 e = y + h;
                    y1 = H > e ? e : H;
                }
            } else if (H < y) {
                y0 = H;
                y1 = H;
            } else {
                const u32 e = y + (H > h ? h : H);
                y0 = y;
                y1 = e > H ? H : e;
            }
            wr32(s + 32, y1);
            wr32(s + 24, y0);
        }
    } else {
        if (rd32(s + 12)) {
            if (rds32(s + 20) < static_cast<s32>(x)) wr32(s + 20, x);
            if (rds32(s + 28) > static_cast<s32>(w + x)) wr32(s + 28, w + x);
        }
        if (rd32(s + 16)) {
            if (rds32(s + 24) < static_cast<s32>(y)) wr32(s + 24, y);
            if (rds32(s + 32) > static_cast<s32>(h + y)) wr32(s + 32, h + y);
        }
    }
}

s32 cellFontGetFontIdCode(PPUContext& ctx, u32 font, u32 code, u32 id_out, u32 code_out)
{
    Scope sc(ctx);
    const auto none = [&](s32 r) {
        if (id_out) wr32(id_out, 0);
        if (code_out) wr32(code_out, 0);
        return r;
    };
    if (!font || !rd32(font + cf::library)) return none(Es(2));
    if (const u32 ext = rd32(font + cf::ext)) {
        const s32 r = calls(rd32(font + cf::ext_id), {ext, code, id_out, code_out});
        if (r != Es(0x11)) return none(r);
    }
    if (rd16(font + cf::kind)) {
        if (id_out) wr32(id_out, rd32(font + cf::id) & 0x7FFFFFFF);
        if (code_out) wr32(code_out, code);
        return 0;
    }
    if (rd32(font + cf::last_code) == code) {
        if (id_out) wr32(id_out, rd32(font + cf::cur_font));
        if (code_out) wr32(code_out, rd32(font + cf::cur_code));
        return 0;
    }
    if (!fontset_code(font + cf::fs, code, id_out, code_out)) return 0;
    return none(Es(0x11));
}

// The face handle of a font that is one font (not a fontset), its entry locked (with 0xA0000000) for `call`.
template <class F>
s32 on_single_font(u32 font, F call)
{
    const u32 stat = has_image() ? rd32(st_ptr()) : 0;
    if (!font) return Es(2);
    const u32 lib = rd32(font + cf::library);
    if (!stat || !lib) return Es(3);
    const u32 table = rd32(font + cf::table);
    if (!table) return Es(2);
    s32 r = 0;
    for (u32 i = 0; i < rd32(font + cf::fonts); i++) {
        if (!rd16(font + cf::kind)) return Es(0x10);
        if (i) return Es(2);
        const u32 e = entry(table, rd32(font + cf::entry));
        if (!e) return Es(1);
        const u32 v = lock_word(e, 0xA0000000u);
        r = Es(0xb);
        if (v & 0x40000000u) r = call(lib, rd32(e + 12));
        unlock_word(e);
        call(0, 0); // after the unlock
        if (r) return r;
    }
    return r;
}

s32 cellFontGetGlyphExpandBufferInfo(PPUContext& ctx, u32 font, u32 points, u32 contours)
{
    Scope sc(ctx);
    if (points) wr32(points, 0);
    if (contours) wr32(contours, 0);
    Local l(8);
    return on_single_font(font, [&](u32 lib, u32 h) -> s32 {
        if (lib) return calls(rd32(lib + 0x88), {h, l.addr(), l.addr() + 4, 0});
        // the largest of the fonts' counts
        if (points && rds32(points) < rds32(l.addr())) wr32(points, rd32(l.addr()));
        if (contours && rds32(contours) < rds32(l.addr() + 4)) wr32(contours, rd32(l.addr() + 4));
        return 0;
    });
}

s32 cellFontAdjustGlyphExpandBuffer(PPUContext& ctx, u32 font, s32 points, s32 contours)
{
    Scope sc(ctx);
    return on_single_font(font, [&](u32 lib, u32 h) -> s32 {
        if (!lib) return 0;
        return calls(rd32(lib + 0x84), {h, static_cast<u64>(static_cast<s64>(points)),
                                           static_cast<u64>(static_cast<s64>(contours)), ~0ull});
    });
}

s32 cellFontGetKerning(PPUContext& ctx, u32 font, u32 c1, u32 c2, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    if (!font) {
        zero(out, 2);
        return Es(2);
    }
    return kerning(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), font, c1, c2, out);
}

s32 cellFontGetRenderScaledKerning(PPUContext& ctx, u32 font, u32 c1, u32 c2, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    if (!font) {
        zero(out, 2);
        return Es(2);
    }
    const u32 r = rd32(font + cf::renderer);
    if (!r) {
        zero(out, 2);
        return Es(0x21);
    }
    return kerning(rdf(r + rn::pixel_x), rdf(r + rn::pixel_y), font, c1, c2, out);
}

s32 cellFontGetCharGlyphMetrics(PPUContext& ctx, u32 font, u32 code, u32 out)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    return char_metrics(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), rdf(font + cf::weight),
        rdf(font + cf::slant), font, code, out, 0);
}

s32 cellFontGetRenderCharGlyphMetrics(PPUContext& ctx, u32 font, u32 code, u32 out)
{
    Scope sc(ctx);
    if (!font || !rd32(font + cf::table) || !rd32(font + cf::library)) return Es(2);
    const u32 r = rd32(font + cf::renderer);
    if (!r) return Es(0x21);
    return char_metrics(rdf(r + rn::pixel_x), rdf(r + rn::pixel_y), rdf(r + rn::weight), rdf(r + rn::slant), font,
        code, out, 0);
}

} // namespace

namespace cfont {

// libfont's vertical forms: 20 pairs of {code, vertical code} (its data), then the shift type of a code.
u32 vertical_code(u32 code)
{
    const u32 t = toc(0x12A48);
    for (u32 i = 0; i < 20; i++)
        if (rd32(t + 8 * i) == code) return rd32(t + 8 * i + 4);
    return code;
}

u32 shift_type(u32 code, bool old_sdk)
{
    u32 k;
    if (code <= 0x1FFF || code == 0x20AC) {
        k = 2;
    } else if (code > 0x30FF) {
        k = 0x200;
    } else if (code == 0x3001 || code == 0x3002) {
        k = 0x243;
    } else if (code == 0x3041) {
        k = 0x223;
    } else {
        k = 0x200;
        const u32 list = toc(0x12A48) + 172;
        for (u32 i = 0; i < 21; i++)
            if (rd32(list + 4 * i) == code) {
                k = 0x223;
                break;
            }
    }
    if (!old_sdk) k |= 0x1000;
    return k;
}

bool old_sdk() { return (rd64(rd32(st_ptr()) + 0x48) & 1) != 0; }

} // namespace cfont

namespace {

using namespace cfont;

s32 cellFontGetCharGlyphMetricsVertical(PPUContext& ctx, u32 font, u32 code, u32 out)
{
    Scope sc(ctx);
    if (!font) return Es(2);
    if (!has_image()) return Es(1);
    code = vertical_code(code);
    const u32 k = shift_type(code, old_sdk());
    return char_metrics(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), rdf(font + cf::weight),
        rdf(font + cf::slant), font, code, out, k);
}

s32 cellFontGetRenderCharGlyphMetricsVertical(PPUContext& ctx, u32 font, u32 code, u32 out)
{
    Scope sc(ctx);
    if (!font || !rd32(font + cf::table) || !rd32(font + cf::library)) return Es(2);
    const u32 r = rd32(font + cf::renderer);
    if (!r) return Es(0x21);
    if (!has_image()) return Es(1);
    code = vertical_code(code);
    const u32 k = shift_type(code, old_sdk());
    return char_metrics(rdf(r + rn::pixel_x), rdf(r + rn::pixel_y), rdf(r + rn::weight), rdf(r + rn::slant), font,
        code, out, k);
}

s32 cellFontGetHorizontalLayout(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    zero(out, 3);
    if (!font) return Es(2);
    return horizontal_layout(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), rdf(font + cf::weight), font, out);
}

s32 cellFontGetVerticalLayout(PPUContext& ctx, u32 font, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    zero(out, 3);
    if (!font) return Es(2);
    return vertical_layout(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), rdf(font + cf::weight),
        rdf(font + cf::slant), font, out);
}

} // namespace

namespace cfont {

// The adjustment of a code in the fontset (code_adjust's {x scale, y scale, y shift, x shift}) applied to the scales:
// the shifts (dx, dy0) as 26.6-scaled offsets, fused as libfont computes them.
struct Scaled {
    float fx, fy, dx = 0.0f, dy0 = 0.0f;
};

Scaled adjusted(u32 font, u32 code, u32 fid, float fx, float fy)
{
    Scaled s{fx, fy};
    Local v(16);
    if (rd32(font + cf::fs + fsx::adjust) && !code_adjust(font + cf::fs, code, v.addr(), fid)) {
        s.fx = s.fx * rdf(v.addr());
        s.fy = s.fy * rdf(v.addr() + 4);
        s.dx = std::fma(s.fx, rdf(v.addr() + 12), 0.0f);
        s.dy0 = std::fma(s.fy, rdf(v.addr() + 8), 0.0f);
    }
    return s;
}

// fn_49d0: a character drawn through the bound renderer (kind 1: on the line's centre, cellFontRenderCharGlyphImage;
// kind 0: cellFontRenderCharGlyphImageHorizontal).
s32 render_char(u32 font, u32 code, u32 S, float x, float y, u32 outM, u32 T, u32 kind)
{
    if (!font || !S || !T) return Es(2);
    if (const u32 ext = rd32(font + cf::ext)) {
        const s32 r = calls(rd32(font + cf::ext_render), {ext, code, S, 0, 0, outM, T}, {x, y});
        if (r != Es(0x11)) return r;
    }
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!table || !lib) return Es(2);
    const u32 R = rd32(font + cf::renderer);
    if (!R) return Es(0x21);
    const auto fail = [&](s32 r) {
        if (outM) zero(outM, 8);
        return r;
    };
    float fx = rdf(R + rn::pixel_x), sy = rdf(R + rn::pixel_y), w = rdf(R + rn::weight);
    const float sx = fx;
    float off = 0.0f, fy, dx = 0.0f, dy = 0.0f;
    u32 fid;
    const auto line_centre = [&] {
        if (!(rd32(font + cf::layouts) & 1)) {
            horizontal_layout(sx, sy, w, font, font + cf::hlayout);
            wr32(font + cf::layouts, rd32(font + cf::layouts) | 1);
        }
        off = std::fma(rdf(font + cf::hlayout + 8), -0.5f, rdf(font + cf::hlayout));
    };
    if (!rd16(font + cf::kind)) {
        if (rd32(font + cf::last_code) != code && select_font(font, code)) {
            forget_font(font);
            return fail(Es(0x12));
        }
        fid = rd32(font + cf::cur_font);
        code = rd32(font + cf::cur_code);
        if (!fid) return fail(Es(0x11));
        if (!(rd32(font + cf::layouts) & 1)) {
            horizontal_layout(sx, sy, w, font, font + cf::hlayout);
            wr32(font + cf::layouts, rd32(font + cf::layouts) | 1);
        }
        if (kind == 1) off = std::fma(rdf(font + cf::hlayout + 8), -0.5f, rdf(font + cf::hlayout));
        fy = sy * rdf(font + cf::f_scale);
        wrf(font + cf::last_y, sy);
        fx = fx * rdf(font + cf::f_scale);
        const float fw = rdf(font + cf::f_weight);
        const Scaled a = adjusted(font, code, fid, fx, fy);
        fx = a.fx;
        fy = a.fy;
        dx = a.dx;
        w = std::fma(w, fw, fw) - 1.0f;
        dy = std::fma(fy, rdf(font + cf::f_shift), a.dy0);
    } else {
        if (kind == 1) line_centre();
        fy = sy * rdf(font + cf::f_scale);
        fid = rd32(font + cf::entry);
        wrf(font + cf::last_y, sy);
    }
    const u32 e = entry(table, fid);
    if (!e) return fail(Es(1));
    lock_word(e, 0xA0000000u);
    const u32 h = rd32(e + 12);
    const float slant = rdf(R + rn::slant);
    calls(tab(lib, 48), {h}, {w});
    calls(tab(lib, 52), {h}, {slant});
    s32 r = calls(tab(lib, 96), {rd32(R + 0x44), h}, {fx, fy});
    if (!r) {
        Local l(12);
        r = calls(tab(lib, 56), {h, l.addr()});
        if (!r) r = calls(tab(lib, 100), {R, code, S, 0, 0, outM, T}, {x, ((dy + off) + y) - rdf(l.addr())});
    }
    unlock_word(e);
    if (r) return fail(r);
    if (outM) {
        wrf(outM + 12, rdf(outM + 12) - dy);
        wrf(outM + 20, rdf(outM + 20) - dx);
    }
    return 0;
}

// cellFontRenderCharGlyphImageVertical: the vertical form of the code drawn shifted for vertical writing.
s32 render_char_vertical(u32 font, u32 code, u32 S, float x, float y, u32 outM, u32 T)
{
    if (!font || !S || !T) return Es(2);
    if (const u32 ext = rd32(font + cf::ext)) {
        const s32 r = calls(rd32(font + cf::ext_render), {ext, code, S, 0, 0, outM, T}, {x, y});
        if (r != Es(0x11)) return r;
    }
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!table || !lib) return Es(2);
    const u32 R = rd32(font + cf::renderer);
    if (!R) return Es(0x21);
    const auto fail = [&](s32 r) {
        if (outM) zero(outM, 8);
        return r;
    };
    float fx = rdf(R + rn::pixel_x), sy = rdf(R + rn::pixel_y), w = rdf(R + rn::weight);
    const float sx = fx;
    code = vertical_code(code);
    const u32 k = shift_type(code, old_sdk());
    float fy, dx = 0.0f, dy = 0.0f;
    u32 fid;
    if (!rd16(font + cf::kind)) {
        if (rd32(font + cf::last_code) != code && select_font(font, code)) {
            forget_font(font);
            return fail(Es(0x12));
        }
        fid = rd32(font + cf::cur_font);
        code = rd32(font + cf::cur_code);
        if (!fid) return fail(Es(0x11));
        if (!(rd32(font + cf::layouts) & 2)) {
            vertical_layout(sx, sy, w, rdf(R + rn::slant), font, font + 0x88);
            wr32(font + cf::layouts, rd32(font + cf::layouts) | 2);
        }
        fy = sy * rdf(font + cf::f_scale);
        wrf(font + cf::last_y, sy);
        fx = fx * rdf(font + cf::f_scale);
        const float fw = rdf(font + cf::f_weight);
        const Scaled a = adjusted(font, code, fid, fx, fy);
        fx = a.fx;
        fy = a.fy;
        dx = a.dx;
        w = std::fma(w, fw, fw) - 1.0f;
        dy = std::fma(fy, rdf(font + cf::f_shift), a.dy0);
    } else {
        fy = sy * rdf(font + cf::f_scale);
        fid = rd32(font + cf::entry);
        wrf(font + cf::last_y, sy);
    }
    const u32 e = entry(table, fid);
    if (!e) return fail(Es(1));
    lock_word(e, 0xA0000000u);
    const u32 h = rd32(e + 12);
    const float slant = rdf(R + rn::slant);
    calls(tab(lib, 48), {h}, {w});
    calls(tab(lib, 52), {h}, {slant});
    calls(tab(lib, 140), {h}, {rdf(font + cf::last_y)});
    s32 r = calls(tab(lib, 96), {rd32(R + 0x44), h}, {fx, fy});
    if (!r) {
        Local l(12);
        r = calls(tab(lib, 60), {h, l.addr()});
        if (!r) r = calls(tab(lib, 104), {R, code, S, 0, 0, outM, T, k}, {dx + x, dy + y});
    }
    unlock_word(e);
    if (r) return fail(r);
    if (outM) {
        wrf(outM + 12, rdf(outM + 12) - dy);
        wrf(outM + 20, rdf(outM + 20) - dx);
    }
    return 0;
}

// fn_5028: a character's outline as a CellFontGlyph (libfontFT's), with its environment: the scales, weight, slant
// and shift type it was made with, the shifts, the font's layouts.
s32 generate_glyph(float sx, float sy, float weight, float slant, u32 font, u32 code, u32 out, u32 k)
{
    if (!out) return Es(2);
    wr32(out, 0);
    if (!font) return Es(2);
    const u32 table = rd32(font + cf::table), lib = rd32(font + cf::library);
    if (!table || !lib || sx == 0.0f || sy == 0.0f) return Es(2);
    float fx = sx, fy, dx = 0.0f, dy = 0.0f, w = 0.0f;
    u32 fid;
    if (!rd16(font + cf::kind)) {
        if (rd32(font + cf::last_code) != code && select_font(font, code)) {
            forget_font(font);
            return Es(0x12);
        }
        fid = rd32(font + cf::cur_font);
        code = rd32(font + cf::cur_code);
        if (!fid) return Es(0x11);
        fy = sy * rdf(font + cf::f_scale);
        wrf(font + cf::last_y, sy);
        fx = sx * rdf(font + cf::f_scale);
        const float fw = rdf(font + cf::f_weight);
        const Scaled a = adjusted(font, code, fid, fx, fy);
        fx = a.fx;
        fy = a.fy;
        dx = a.dx;
        w = std::fma(weight, fw, fw) - 1.0f;
        dy = std::fma(fy, rdf(font + cf::f_shift), a.dy0);
    } else {
        fy = sy * rdf(font + cf::f_scale);
        fid = rd32(font + cf::entry);
        wrf(font + cf::last_y, sy);
    }
    Local hl(12), vl(12);
    if (const s32 r = horizontal_layout(sx, sy, weight, font, hl.addr())) return r;
    if (const s32 r = vertical_layout(sx, sy, weight, slant, font, vl.addr())) return r;
    const u32 e = entry(table, fid);
    s32 r = Es(1);
    if (e) {
        lock_word(e, 0xA0000000u);
        const u32 h = rd32(e + 12);
        r = calls(tab(lib, 44), {h}, {fx, fy});
        if (!r) r = calls(tab(lib, 48), {h}, {w});
        if (!r) r = calls(tab(lib, 52), {h}, {slant});
        if (!r) {
            calls(tab(lib, 140), {h}, {rdf(font + cf::last_y)});
            Local off(8);
            r = k ? calls(tab(lib, 76), {h, code, out, off.addr(), k}) : calls(tab(lib, 72), {h, code, out, off.addr()});
            if (!r) {
                const u32 env = rd32(rd32(out) + 60);
                r = calls(tab(lib, 56), {h, env + 92});
                if (!r) r = calls(tab(lib, 60), {h, env + 116});
            }
        }
        unlock_word(e);
        if (!r) {
            const u32 env = rd32(rd32(out) + 60);
            wrf(env + 72, dy);
            wrf(env + 0, sx);
            wrf(env + 4, sy);
            wrf(env + 8, w);
            wrf(env + 12, slant);
            wr32(env + 52, k);
            wrf(env + 68, fy / sy);
            wrf(env + 56, dx + rdf(env + 56));
            wrf(env + 60, dy + rdf(env + 60));
            for (u32 i = 0; i < 3; i++) wr32(env + 80 + 4 * i, rd32(hl.addr() + 4 * i));
            for (u32 i = 0; i < 3; i++) wr32(env + 104 + 4 * i, rd32(vl.addr() + 4 * i));
            return 0;
        }
    }
    if (const u32 g = rd32(out)) calls(tab(lib, 80), {rd32(font + cf::library), g});
    wr32(out, 0);
    return r;
}

} // namespace cfont

namespace {

using namespace cfont;

s32 cellFontRenderCharGlyphImage(PPUContext& ctx, u32 font, u32 code, u32 S, float x, float y, u32 m, u32 T)
{
    Scope sc(ctx);
    return render_char(font, code, S, x, y, m, T, 1);
}

s32 cellFontRenderCharGlyphImageHorizontal(PPUContext& ctx, u32 font, u32 code, u32 S, float x, float y, u32 m, u32 T)
{
    Scope sc(ctx);
    return render_char(font, code, S, x, y, m, T, 0);
}

s32 cellFontRenderCharGlyphImageVertical(PPUContext& ctx, u32 font, u32 code, u32 S, float x, float y, u32 m, u32 T)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    return render_char_vertical(font, code, S, x, y, m, T);
}

s32 cellFontGenerateCharGlyph(PPUContext& ctx, u32 font, u32 code, u32 out)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    if (static_cast<s64>(rd64(cfg() + 8)) < 0) return Es(0x10);
    if (!font) return Es(2);
    return generate_glyph(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), rdf(font + cf::weight),
        rdf(font + cf::slant), font, code, out, 0);
}

s32 cellFontGenerateCharGlyphVertical(PPUContext& ctx, u32 font, u32 code, u32 out)
{
    Scope sc(ctx);
    if (!has_image()) return Es(1);
    if (disabled(0x4000000000000000ull)) return Es(0x10);
    if (!font) return Es(2);
    const u32 v = vertical_code(code);
    const u32 k = shift_type(v, old_sdk());
    const s32 r = generate_glyph(rdf(font + cf::pixel_x), rdf(font + cf::pixel_y), rdf(font + cf::weight),
        rdf(font + cf::slant), font, v, out, k);
    if (!r && code != v) {
        const u32 g = rd32(out);
        wr16(g + 2, rd16(g + 2) | 0x80);
    }
    return r;
}

// ---- vertexes glyphs drawn by a renderer (the CellFontLibrary's table +108)

s32 glyph_render(u32 glyph, u32 style, u32 r, u32 S, float x, float y, u32 m, u32 T, u64 bit, s64 k, bool centre)
{
    if (!has_image()) return Es(1);
    if (disabled(bit)) return Es(0x10);
    if (!glyph || !r || !rd32(r + rn::library)) return Es(2);
    if (centre) {
        const u32 env = rd32(glyph + 60);
        const float ratio = rdf(style + 4) / rdf(env + 4);
        y = std::fma(ratio, std::fma(rdf(env + 88), -0.5f, rdf(env + 80)), y);
    }
    const s32 e = calls(rd32(rd32(r + rn::library) + 108), {r, glyph, style, S, 0, 0, m, T, static_cast<u64>(k)}, {x, y});
    if (!e) return 0;
    if (m) zero(m, 8);
    return e;
}

s32 cellFontGlyphRenderImage(PPUContext& ctx, u32 glyph, u32 style, u32 r, u32 S, float x, float y, u32 m, u32 T)
{
    Scope sc(ctx);
    return glyph_render(glyph, style, r, S, x, y, m, T, 0x0800000000000000ull, 1, true);
}

s32 cellFontGlyphRenderImageHorizontal(PPUContext& ctx, u32 glyph, u32 style, u32 r, u32 S, float x, float y, u32 m,
    u32 T)
{
    Scope sc(ctx);
    return glyph_render(glyph, style, r, S, x, y, m, T, 0x0400000000000000ull, 0, false);
}

s32 cellFontGlyphRenderImageVertical(PPUContext& ctx, u32 glyph, u32 style, u32 r, u32 S, float x, float y, u32 m,
    u32 T)
{
    Scope sc(ctx);
    return glyph_render(glyph, style, r, S, x, y, m, T, 0x0200000000000000ull, -1, false);
}

} // namespace

namespace cfont {

// ------------------------------------------------------------------------------------------------- opening fonts
u32 stat() { return has_image() ? rd32(st_ptr()) : 0; }

// libfont's quick count change: while other fonts use an open entry (its state over 0x40000001, or over 0xE0000001
// under a reader's lock) its count moves without taking the lock. False when the lock is needed.
bool quick_count(u32 e, s32 d)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, e);
        if (!(v > 0xE0000001u || static_cast<s32>(v) > 0x40000001)) return false;
        if (atomic_cas_be32(base, e, v, v + static_cast<u32>(d)) == v) return true;
    }
}

// A font table's entry of {unique ID, library}: its index, else a free entry's (one without a library) claimed for
// it; E(0xd) when the table is full. The table locked meanwhile.
s32 claim_entry(u32 table, u32 lib, u32 id, u32& index)
{
    lock_word(table, 0x80000000u);
    const u32 n = rd32(table + 4), entries = rd32(table + 8);
    s32 free = -1, r = 0;
    bool found = false;
    for (u32 i = 0; i < n; i++) {
        const u32 e = entries + 16 * i;
        if (rd32(e + 4) == id && rd32(e + 8) == lib) {
            index = i;
            found = true;
            break;
        }
        if (free == -1 && !rd32(e + 8)) free = static_cast<s32>(i);
    }
    if (!found) {
        if (free < 0) {
            r = Es(0xd);
        } else {
            const u32 e = entries + 16 * static_cast<u32>(free);
            wr32(e + 8, lib);
            wr32(e, 0);
            wr32(e + 4, id);
            index = static_cast<u32>(free);
        }
    }
    unlock_word(table);
    return r;
}

// The CellFont an opening starts from: no renderer, 72 DPI, no scale, weight or slant, the factors 1.
void font_init(u32 font, u32 lib, u32 table)
{
    wrf(font + cf::last_y, 1.0f);
    wrf(font + cf::f_scale, 1.0f);
    wrf(font + cf::f_weight, 1.0f);
    wr32(font + cf::table, table);
    wr32(font + cf::library, lib);
    wr32(font + cf::renderer, 0);
    wr32(font + cf::dpi_x, 72);
    wr32(font + cf::dpi_y, 72);
    for (u32 o = cf::point_x; o <= cf::cur_code; o += 4) wr32(font + o, 0);
    wr32(font + cf::f_shift, 0);
    for (u32 o = 0x98; o < 0xB8; o += 4) wr32(font + o, 0);
    wr32(font + cf::fonts, 0);
}

// cellFontOpenFontFile / cellFontOpenFontMemory: a font of the user table (st + 0x2C), its face opened once per
// {unique ID, library} by `open(face handle slot, entry ID)` and counted by the fonts using it.
template <class F>
s32 open_user_font(u32 lib, u32 source, s32 unique, u32 font, u16 kind, F open)
{
    const u32 s = stat();
    if (!font) return Es(2);
    wr16(font + cf::kind, 0);
    wr32(font + cf::library, 0);
    wr32(font + cf::table, 0);
    if (!lib) return Es(2);
    if (!s) return Es(3);
    if (!source || unique < 0) return Es(2);
    font_init(font, lib, s + 0x2C);
    wr16(font + cf::kind, kind);
    wr32(font + cf::layouts, 0);
    const u32 id = static_cast<u32>(unique) | 0x80000000u;
    const u32 table = rd32(font + cf::table);
    u32 index = 0;
    s32 r = claim_entry(table, lib, id, index);
    if (!r) {
        const u32 e = rd32(table + 8) + 16 * index;
        const u32 v = lock_word(e, 0x80000000u) & 0x7FFFFFFFu;
        s32 d = 0;
        if (v & 0x40000000u) {
            if (!(rd64(s + 0x48) & 2)) {
                wr32(font + cf::entry, index);
                wr32(font + cf::id, id);
            }
            d = 1;
        } else {
            calls(tab(lib, 0x94), {rd16(lib + 0x9A)});
            r = open(e + 12, id);
            if (r) {
                wr32(e + 8, 0);
                wr32(e + 4, 0);
            } else {
                wr32(e + 4, id);
                wr32(e + 8, lib);
                wr32(font + cf::entry, index);
                wr32(font + cf::id, id);
                if (!(rd64(s + 0x48) & 1)) r = calls(tab(lib, 0x84), {rd32(e + 12), ~0ull, ~0ull, ~0ull});
                if (r) {
                    calls(tab(lib, 0x1C), {e + 12, 0});
                    wr32(e + 8, 0);
                    wr32(e + 4, 0);
                } else {
                    or_word(e, 0x40000000u);
                    d = 1;
                }
            }
        }
        if (d) add_word(e, d);
        unlock_word(e);
    }
    if (r) {
        wr32(font + cf::table, 0);
        wr32(font + cf::library, 0);
        return r;
    }
    wr32(font + cf::fonts, 1);
    wr16(font, 0xCF00);
    return 0;
}

// fn_7dd4: the first `n` fonts of a fontset given back (their entries' counts down; the last user's face closed, its
// file buffer and composite codes freed). The answer is the last font's (the first of the set).
s32 close_fontset_fonts(u32 font, u32 n)
{
    s32 r = 0;
    while (n) {
        n--;
        Local id(8);
        r = fontset_font(font + cf::fs, n, id.addr());
        if (r) return r;
        const u32 table = rd32(font + cf::table), fid = id.get();
        if (fid >= rd32(table + 4)) continue;
        const u32 e = rd32(table + 8) + 16 * fid;
        if (quick_count(e, -1)) continue;
        const u32 v = lock_word(e, 0x80000000u) & 0x7FFFFFFFu;
        s32 d = -1;
        if (!(v & 0x1FFFFFFFu)) {
            r = Es(0xb);
            d = 0;
        } else if ((v & 0x1FFFFFFFu) == 1 && (v & 0x40000000u)) {
            const u32 lib = rd32(font + cf::library);
            Local codes(8), buffer(8);
            calls(tab(lib, 0x24), {rd32(e + 12), 0, codes.addr()});
            const s32 c = calls(tab(lib, 0x1C), {e + 12, buffer.addr()});
            if (c) {
                r = c;
                d = 0;
            } else {
                // (libfont skips freeing the buffer of a font its reserved list keeps: that list is never set)
                wr32(e + 8, 0);
                wr32(e + 4, 0);
                if (buffer.get()) calls(tab(lib, 0x78), {buffer.addr()});
                if (codes.get()) calls(tab(lib, 0x78), {codes.addr()});
                and_word(e, ~0x40000000u);
            }
        }
        if (d) add_word(e, d);
        unlock_word(e);
    }
    return r;
}

// cellFontCloseFont.
s32 close_font(u32 font)
{
    const u32 s = stat();
    if (!font) return Es(2);
    const u32 lib = rd32(font + cf::library);
    if (!s || !lib) return Es(3);
    const auto closed = [&] {
        wr32(font + cf::library, 0);
        wr32(font + cf::fonts, 0);
        return 0;
    };
    if (!rd16(font + cf::kind)) {
        if (rd32(font + cf::table) != s + 0x20) return Es(2);
        if (const s32 r = close_fontset_fonts(font, rd32(font + cf::fonts))) return r;
        return closed();
    }
    const u32 table = s + 0x2C;
    if (rd32(font + cf::table) != table) return Es(2);
    const u32 index = rd32(font + cf::entry);
    if (index < rd32(table + 4) && quick_count(rd32(table + 8) + 16 * index, -1)) return closed();
    lock_word(table, 0x80000000u);
    s32 r = Es(1);
    if (index < rd32(table + 4)) {
        const u32 e = rd32(table + 8) + 16 * index;
        const u32 v = lock_word(e, 0x80000000u) & 0x7FFFFFFFu;
        s32 d = -1;
        r = 0;
        if (!(v & 0x1FFFFFFFu)) {
            r = Es(0xb);
            d = 0;
        } else if ((v & 0x1FFFFFFFu) == 1 && (v & 0x40000000u)) {
            r = calls(tab(lib, 0x1C), {e + 12, 0});
            if (r) {
                d = 0;
            } else {
                wr32(e + 8, 0);
                wr32(e + 4, 0);
                and_word(e, ~0x40000000u);
            }
        }
        if (d) add_word(e, d);
        unlock_word(e);
    }
    unlock_word(table);
    if (r) return r;
    return closed();
}

// fn_9338's paths of a system font from its table path (`src`): the font file's and its composite codes' (the file's
// extension made ".ccd"). The tables keep a path as a name for the font server (XOR 0xDE) to an 0xDE, then the file
// name (XOR 0xFA) to an 0xFA. Without a font directory (cellFontControl 0x540000) the path is the font server's
// "/:dev_font:/" and that name, its NUL, 0xDE, then "/dev_flash/data/font/" (libfont's data) and the file name XOR
// 0xF2 to an 0xF2 (the hidden path the font library opens); with one it is the directory and the file name. (Paths
// past libfont's 1 KB buffers, which it would overrun, are cut.)
void font_paths(u32 src, std::vector<u8>& file, std::vector<u8>& codes)
{
    constexpr usize kMax = 1022;
    const u32 dir = rd32(cfg());
    const bool hidden = dir == 0;
    file.clear();
    u32 p = src;
    usize end;
    if (hidden) {
        for (const char c : std::string_view("/:dev_font:/")) file.push_back(static_cast<u8>(c));
        for (;;) {
            const u8 c = rd8(p++) ^ 0xDE;
            file.push_back(c);
            if (!c || file.size() >= 1023) break;
        }
        file.push_back(0xDE);
        const u32 d = toc(0x12A48) + 0x100;
        for (u32 k = 0; k < 21; k++) file.push_back(rd8(d + k) ^ 8);
        for (;;) {
            const u8 c = rd8(p);
            file.push_back(c ^ 8);
            if (c == 0xFA || file.size() > kMax) break;
            p++;
        }
    } else {
        for (u32 k = 0; k < 512 && rd8(dir + k); k++) file.push_back(rd8(dir + k));
        while ((rd8(p++) ^ 0xDE) != 0) {
        }
        for (;;) {
            const u8 c = rd8(p);
            file.push_back(c ^ 0xFA);
            if (c == 0xFA || file.size() > kMax) break;
            p++;
        }
    }
    end = file.size() - 1; // the terminator
    const u8 dot = hidden ? 0xDC : '.', slash = hidden ? 0xDD : '/';
    usize pos = 0;
    for (usize i = end; i-- > 1;) {
        if (file[i] == dot) {
            pos = i;
            break;
        }
        if (file[i] == slash) {
            pos = end;
            break;
        }
    }
    codes.assign(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(pos));
    if (pos + 4 > kMax) {
        codes.assign(1, 0);
        return;
    }
    if (hidden) codes.insert(codes.end(), {0xDC, 0x91, 0x91, 0x96, 0xF2});
    else codes.insert(codes.end(), {'.', 'c', 'c', 'd', 0});
}

// fn_9338: cellFontOpenFontset (the fonts' files opened) and cellFontOpenFontsetOnMemory (the files read into memory
// first): each font of the fontset an entry of the system table (st + 0x20), opened once and counted.
s32 open_fontset(u32 lib, u32 type, u32 font, bool memory)
{
    const u32 s = stat();
    if (!font) return Es(2);
    wr16(font + cf::kind, 0);
    wr32(font + cf::library, 0);
    wr32(font + cf::table, 0);
    if (!lib || !type) return Es(2);
    if (!s) return Es(3);
    const u16 mode = rd16(lib + 0x98);
    font_init(font, lib, s + 0x20);
    wr32(font + cf::entry, 0);
    wr32(font + cf::id, 0);
    const auto fail = [&](s32 r) {
        wr32(font + cf::table, 0);
        wr32(font + cf::library, 0);
        return r;
    };
    // the old and new names of the Japanese sets
    Local t(8);
    u32 ty = rd32(type);
    if (ty == 0x100) ty = 0x2000100;
    else if (ty == 0x101) ty = 0x2000101;
    else if (ty == 0x1000100) ty = 0x100;
    else if (ty == 0x1000101) ty = 0x101;
    wr32(t.addr(), ty);
    wr32(t.addr() + 4, rd32(type + 4));
    const u32 fs = font + cf::fs;
    Local count(8), all(8);
    if (fontset_lookup(fs, t.addr()) || fontset_count(fs, count.addr()) || font_count(all.addr())) return fail(Es(8));
    wr32(font + cf::layouts, 0);
    wr32(font + 0x3C, all.get());
    const u32 n = count.get(), table = s + 0x20;
    Local fid(8), path(8), len(8), ccd(8), size(8), data(8), sizes(8), file(1024), codes(1024);
    std::vector<u8> fpath, cpath;
    for (u32 i = 0; i < n; i++) {
        const auto undo = [&](u32 upto, s32 r) {
            close_fontset_fonts(font, upto);
            return fail(r);
        };
        if (fontset_font(fs, i, fid.addr())) return undo(i, Es(1));
        if (const s32 r = font_file(fid.get(), path.addr(), len.addr())) return undo(i, r == Es(0x103) ? Es(3) : Es(1));
        font_paths(path.get(), fpath, cpath);
        std::memcpy(hp(file.addr()), fpath.data(), std::min<usize>(fpath.size(), 1024));
        std::memcpy(hp(codes.addr()), cpath.data(), std::min<usize>(cpath.size(), 1024));
        const u32 id = fid.get();
        if (id >= rd32(table + 4)) return undo(i, Es(1));
        const u32 e = rd32(table + 8) + 16 * id;
        const u32 v = lock_word(e, 0x80000000u) & 0x7FFFFFFFu;
        s32 d = 1, r = 0;
        u32 upto = i;
        if (!(v & 0x40000000u)) {
            wr32(size.addr(), 0);
            if (calls(tab(lib, 0x7C), {codes.addr(), ccd.addr(), size.addr()})) wr32(ccd.addr(), 0);
            calls(tab(lib, 0x94), {mode});
            if (memory) {
                font_cache_size(id, size.addr());
                r = calls(tab(lib, 0x7C), {file.addr(), data.addr(), size.addr()});
                if (!r) r = calls(tab(lib, 0x18), {data.get(), size.get(), 0, id, e + 12});
            } else {
                wr32(data.addr(), 0);
                r = calls(tab(lib, 0x14), {file.addr(), 2, 0, id, e + 12});
            }
            if (r) {
                if (ccd.get()) calls(tab(lib, 0x78), {ccd.addr()});
                d = 0;
            } else {
                wr32(e + 8, lib);
                wr32(e + 4, id);
                if (ccd.get()) calls(tab(lib, 0x20), {rd32(e + 12), desc("pspocket_font_composite_codes"), ccd.get()});
                if (!(rd64(s + 0x48) & 1)) {
                    Local a(8), b(8);
                    if (!font_sizes(id, a.addr(), b.addr()))
                        r = calls(tab(lib, 0x84), {rd32(e + 12), static_cast<u64>(static_cast<s64>(static_cast<s32>(a.get()))),
                                                      static_cast<u64>(static_cast<s64>(static_cast<s32>(b.get()))), 7});
                    else
                        r = calls(tab(lib, 0x84), {rd32(e + 12), ~0ull, ~0ull, ~0ull});
                    // a failed expansion leaves the font open and counted: it is given back with the others
                    if (r) upto = i + 1;
                }
                or_word(e, 0x40000000u);
            }
        }
        if (d) add_word(e, d);
        unlock_word(e);
        if (r) return undo(upto, r);
    }
    wr16(font, 0xCF00);
    wr32(font + cf::fonts, n);
    return 0;
}

// cellFontOpenFontInstance: another CellFont on the same faces (their counts up).
s32 open_instance(u32 src, u32 dst)
{
    const u32 s = stat();
    if (!dst) return Es(2);
    wr32(dst + cf::fonts, 0);
    wr16(dst + cf::kind, 0);
    wr32(dst + cf::library, 0);
    wr32(dst + cf::table, 0);
    if (!s) return Es(3);
    if (!src || !rd32(src + cf::library)) return Es(2);
    const auto count_up = [](u32 e) -> s32 {
        if (quick_count(e, 1)) return 0;
        const u32 v = lock_word(e, 0x80000000u) & 0x7FFFFFFFu;
        s32 r = 0;
        if (v & 0x40000000u) add_word(e, 1);
        else r = Es(9);
        unlock_word(e);
        return r;
    };
    if (!rd16(src + cf::kind)) {
        const u32 n = rd32(src + cf::fonts), table = s + 0x20;
        for (u32 i = 0; i < n; i++) {
            Local id(8);
            if (fontset_font(src + cf::fs, i, id.addr()) || id.get() >= rd32(table + 4)) {
                close_fontset_fonts(src, i);
                return Es(1);
            }
            if (const s32 r = count_up(rd32(table + 8) + 16 * id.get())) {
                close_fontset_fonts(src, i);
                return r;
            }
        }
    } else {
        const u32 table = rd32(src + cf::table);
        if (!table) return Es(2);
        u32 index = 0;
        if (claim_entry(table, rd32(src + cf::library), rd32(src + cf::id), index)) return Es(2);
        const u32 e = rd32(table + 8) + 16 * index;
        if (!quick_count(e, 1)) {
            const u32 v = lock_word(e, 0x80000000u) & 0x7FFFFFFFu;
            s32 r = 0;
            if (v & 0x40000000u) {
                add_word(e, 1);
            } else {
                r = Es(9);
                wr32(e + 8, 0);
                wr32(e + 4, 0);
            }
            unlock_word(e);
            if (r) return r;
        }
    }
    for (u32 o = 4; o < 0xB8; o += 4) wr32(dst + o, rd32(src + o));
    wr16(dst + cf::kind, rd16(src + cf::kind));
    wr16(dst, 0xCF01);
    return 0;
}

// ------------------------------------------------------------------------------------------------- graphics
// fn_afb4: a graphics record libfont keeps (st + 0x68 .. 0x74), else 0.
u32 graphics_of(u32 g)
{
    const u32 s = rd32(st_ptr());
    for (const u32 o : {0x68u, 0x6Cu, 0x70u, 0x74u})
        if (rd32(s + o) == g) return g;
    return 0;
}

// cellFontEndGraphics: the graphics' own end (its +0x10) called, its record no longer open.
s32 end_graphics(u32 g)
{
    if (!has_image()) return Es(1);
    const u32 was = lock_static();
    s32 r = Es(3);
    if (was) {
        if (const u32 x = graphics_of(g); x && rd32(x + 0x10)) {
            calls(rd32(x + 0x10), {x});
            and_word(x + 8, ~0x40000000u);
            r = 0;
        }
    }
    unlock_static(was);
    return r;
}

// ------------------------------------------------------------------------------------------------- outlines' vertexes
// A CellFontGlyph's outline (+0x28 its contours, +0x2C its points as float pairs, +0x30 their tags, +0x34 the
// contours' last points) walked as cellFontGlyphGetOutlineVertexes walks it: a contour's start (kind 0), lines'
// points (16), conics' and cubics' points (36 and 52) halving the curves until they are within the control distance
// (32 halvings at most; a cubic whose controls are on both sides of its chord is halved at least once), the close
// back to the start (24). Each vertex goes to `put(contour, x, y, its index in the contour, kind)`.
using Put = std::function<void(s32, float, float, s32, u32)>;

struct Box {
    float x0, y0, x1, y1;
};

void extend_box(Box& b, float x, float y)
{
    if (!(x >= b.x0)) b.x0 = x;
    if (!(x <= b.x1)) b.x1 = x;
    if (!(y >= b.y0)) b.y0 = y;
    if (!(y <= b.y1)) b.y1 = y;
}

// A conic from (x0, y0) by (cx, cy) to (tx, ty): the points' count goes on from k.
s32 conic(float x0, float y0, float cx, float cy, float tx, float ty, float limit, s32 c, s32 k, Box& b, const Put& put)
{
    struct Half {
        float cx, cy, tx, ty;
    };
    Half stack[33];
    u32 depth = 0;
    for (;;) {
        for (;;) {
            const float ey = (cy + cy) - (y0 + ty), ex = (cx + cx) - (x0 + tx);
            if (std::fma(ex, ex, ey * ey) < limit || depth > 31) break;
            const float my = (cy + ty) * 0.5f, ncy = (y0 + cy) * 0.5f, mx = (cx + tx) * 0.5f, ncx = (x0 + cx) * 0.5f;
            stack[depth++] = {mx, my, tx, ty};
            tx = (mx + ncx) * 0.5f;
            ty = (my + ncy) * 0.5f;
            cx = ncx;
            cy = ncy;
        }
        put(c, tx, ty, k, 36);
        extend_box(b, tx, ty);
        k++;
        if (!depth) return k;
        x0 = tx;
        y0 = ty;
        const Half& h = stack[--depth];
        cx = h.cx;
        cy = h.cy;
        tx = h.tx;
        ty = h.ty;
    }
}

// fn_cac: a cubic from (x0, y0) by (c1x, c1y) and (c2x, c2y) to (tx, ty).
s32 cubic(float x0, float y0, float c1x, float c1y, float c2x, float c2y, float tx, float ty, float limit, s32 c, s32 k,
    Box& b, const Put& put)
{
    struct Third {
        float c1x, c1y, c2x, c2y, tx, ty;
    };
    Third stack[33];
    float sx = c1x + c2x, sy = c1y + c2y;
    const auto spread = [&] {
        const float ey = (y0 + ty) - sy, ex = (x0 + tx) - sx;
        return std::fma(ex, ex, ey * ey);
    };
    float d = spread();
    if (!(d >= limit)) {
        const float dx3 = tx - x0, dy1 = c1y - y0, dy3 = ty - y0, dx1 = c1x - x0, dy2 = c2y - y0, dx2 = c2x - x0;
        const float k1 = std::fma(dx1, dy3, -(dy1 * dx3)), k2 = std::fma(dy3, dx2, -(dx3 * dy2));
        if (!((k1 >= 0.0f && k2 >= 0.0f) || (k1 <= 0.0f && k2 <= 0.0f))) d = limit;
    }
    u32 depth = 0;
    for (;;) {
        while (d >= limit && depth <= 31) {
            const float rc2y = (c2y + ty) * 0.5f, rc2x = (c2x + tx) * 0.5f, lc1x = (x0 + c1x) * 0.5f,
                        lc1y = (y0 + c1y) * 0.5f, mx = sx * 0.5f, my = sy * 0.5f;
            const float rc1y = (rc2y + my) * 0.5f, rc1x = (rc2x + mx) * 0.5f, lc2x = (mx + lc1x) * 0.5f,
                        lc2y = (my + lc1y) * 0.5f;
            stack[depth++] = {rc1x, rc1y, rc2x, rc2y, tx, ty};
            tx = (rc1x + lc2x) * 0.5f;
            ty = (rc1y + lc2y) * 0.5f;
            c1x = lc1x;
            c1y = lc1y;
            c2x = lc2x;
            c2y = lc2y;
            sy = c1y + c2y;
            sx = c1x + c2x;
            d = spread();
        }
        put(c, tx, ty, k, 52);
        extend_box(b, tx, ty);
        k++;
        if (!depth) return k;
        x0 = tx;
        y0 = ty;
        const Third& t = stack[--depth];
        c1x = t.c1x;
        c1y = t.c1y;
        c2x = t.c2x;
        c2y = t.c2y;
        tx = t.tx;
        ty = t.ty;
        sy = c1y + c2y;
        sx = c1x + c2x;
        d = spread();
    }
}

// The walk over the contours: E(0x12) on a contour starting with a cubic's control or a conic followed by one.
s32 walk(u32 g, float distance, const Put& put, Box& box, u32& total)
{
    const s32 n = static_cast<s16>(rd16(g + 40));
    const u32 pts = rd32(g + 44), tags = rd32(g + 48), ends = rd32(g + 52);
    const auto px = [&](s32 i) { return rdf(pts + 8 * static_cast<u32>(i)); };
    const auto py = [&](s32 i) { return rdf(pts + 8 * static_cast<u32>(i) + 4); };
    const auto tag = [&](s32 i) { return rd8(tags + static_cast<u32>(i)) & 3u; };
    const float limit = distance * (distance * 16.0f);
    s32 start = 0;
    for (s32 c = 0; c < n; start = static_cast<s32>(rd16(ends + 2 * static_cast<u32>(c))) + 1, c++) {
        if (tag(start) == 2) return Es(0x12);
        const s32 end = rd16(ends + 2 * static_cast<u32>(c));
        float sx, sy;
        s32 i, last;
        if (tag(start) != 0) {
            sx = px(start);
            sy = py(start);
            i = start + 1;
            if (end <= i) continue;
            last = end;
        } else if (tag(end) != 0) {
            sx = px(end);
            sy = py(end);
            i = start;
            last = end - 1;
        } else {
            sx = (px(start) + px(end)) * 0.5f;
            sy = (py(start) + py(end)) * 0.5f;
            i = start;
            last = end;
        }
        put(c, sx, sy, 0, 0);
        extend_box(box, sx, sy);
        s32 k = 1;
        float x = sx, y = sy;
        bool close = false;
        for (;;) {
            const u32 t = tag(i);
            if (t == 1) {
                const float qx = px(i), qy = py(i);
                i++;
                put(c, qx, qy, k, 16);
                k++;
                extend_box(box, qx, qy);
                if (last < i) {
                    close = true;
                    break;
                }
                x = qx;
                y = qy;
                continue;
            }
            if (t == 0) {
                float cx = px(i), cy = py(i);
                s32 j = i + 1;
                if (last < j) {
                    k = conic(x, y, cx, cy, sx, sy, limit, c, k, box, put);
                    break;
                }
                bool next = false;
                for (;;) {
                    const u32 tj = tag(j);
                    if (tj == 1) {
                        const float qx = px(j), qy = py(j);
                        k = conic(x, y, cx, cy, qx, qy, limit, c, k, box, put);
                        if (j == last) {
                            close = true;
                        } else {
                            i = j + 1;
                            x = qx;
                            y = qy;
                            next = true;
                        }
                        break;
                    }
                    if (tj != 0) return Es(0x12);
                    const float dx = px(j), dy = py(j);
                    const float mx = (dx + cx) * 0.5f, my = (dy + cy) * 0.5f;
                    k = conic(x, y, cx, cy, mx, my, limit, c, k, box, put);
                    j++;
                    if (last < j) {
                        k = conic(mx, my, dx, dy, sx, sy, limit, c, k, box, put);
                        break;
                    }
                    cx = dx;
                    cy = dy;
                    x = mx;
                    y = my;
                }
                if (next) continue;
                break;
            }
            const float c1x = px(i), c1y = py(i);
            const s32 j = i + 1;
            if (last < j || tag(j) != 2) break;
            const float c2x = px(j), c2y = py(j);
            const s32 m = j + 1;
            if (last < m) {
                k = cubic(x, y, c1x, c1y, c2x, c2y, sx, sy, limit, c, k, box, put);
                break;
            }
            const float qx = px(m), qy = py(m);
            k = cubic(x, y, c1x, c1y, c2x, c2y, qx, qy, limit, c, k, box, put);
            if (m == last) {
                close = true;
                break;
            }
            i = m + 1;
            x = qx;
            y = qy;
        }
        if (close) {
            put(c, sx, sy, k, 24);
            k++;
        }
        total += static_cast<u32>(k);
    }
    return 0;
}

// cellFontGlyphGetOutlineVertexes' work: the bounding box {min x, min y, max x, max y} and the vertexes' count.
s32 vertexes(u32 glyph, float distance, const Put& put, u32 bbox, u32 count)
{
    if (!glyph) return Es(2);
    if (!rd16(glyph + 40)) {
        if (bbox) zero(bbox, 4);
        if (count) wr32(count, 0);
        return 0;
    }
    constexpr float kMax = std::numeric_limits<float>::max();
    Box box{kMax, kMax, -kMax, -kMax};
    u32 total = 0;
    if (const s32 r = walk(glyph, distance, put, box, total)) {
        if (bbox) zero(bbox, 4);
        if (count) wr32(count, 0);
        return r;
    }
    if (bbox) {
        wrf(bbox, box.x0);
        wrf(bbox + 4, box.y0);
        wrf(bbox + 8, box.x1);
        wrf(bbox + 12, box.y1);
    }
    if (count) wr32(count, total);
    return 0;
}

} // namespace cfont

namespace {

using namespace cfont;

s32 cellFontOpenFontFile(PPUContext& ctx, u32 lib, u32 path, u32 sub, s32 unique, u32 font)
{
    Scope sc(ctx);
    return open_user_font(lib, path, unique, font, 1,
        [&](u32 slot, u32 id) { return calls(tab(lib, 0x14), {path, 2, sub, id, slot}); });
}

s32 cellFontOpenFontMemory(PPUContext& ctx, u32 lib, u32 data, u32 size, u32 sub, s32 unique, u32 font)
{
    Scope sc(ctx);
    return open_user_font(lib, data, unique, font, 2,
        [&](u32 slot, u32 id) { return calls(tab(lib, 0x18), {data, size, sub, id, slot}); });
}

s32 cellFontOpenFontset(PPUContext& ctx, u32 lib, u32 type, u32 font)
{
    Scope sc(ctx);
    return open_fontset(lib, type, font, false);
}

s32 cellFontOpenFontsetOnMemory(PPUContext& ctx, u32 lib, u32 type, u32 font)
{
    Scope sc(ctx);
    return open_fontset(lib, type, font, true);
}

s32 cellFontOpenFontInstance(PPUContext& ctx, u32 src, u32 dst)
{
    Scope sc(ctx);
    return open_instance(src, dst);
}

s32 cellFontCloseFont(PPUContext& ctx, u32 font)
{
    Scope sc(ctx);
    return close_font(font);
}

// fn_d058: the composite-code table (a ".ccd" file) libfont gives the font library for a fontset's font: ranges of
// codes {first, last, count, offset}, each code drawn as `count` glyph codes; others as themselves.
void pspocket_font_composite_codes(PPUContext& ctx, u32 data, u32 code, u32 out)
{
    Scope sc(ctx);
    if (code > 0xFFFE) return;
    const u32 n = rd16(data + 6), ranges = data + 8;
    for (u32 i = 0; i < n; i++) {
        const u32 r = ranges + 8 * i;
        if (code < rd16(r)) break;
        if (rd16(r + 2) < code) continue;
        const u32 cnt = rd16(r + 4);
        if (!cnt) {
            wr32(out, 0);
            return;
        }
        const u32 p = ranges + 8 * n + 2 * (rd16(r + 6) + cnt * (code - rd16(r)));
        for (u32 k = 0; k < cnt; k++) wr32(out + 4 * k, rd16(p + 2 * k));
        wr32(out + 4 * cnt, 0);
        return;
    }
    wr32(out, code);
    wr32(out + 4, 0);
}

s32 cellFontEndGraphics(PPUContext& ctx, u32 g)
{
    Scope sc(ctx);
    if (!g) return Es(2);
    return end_graphics(g);
}

// fn_7b1c (cellFontExtend 0xCFE00001): a graphics library's start, its record kept by libfont (st + 0x68).
s32 pspocket_font_graphics_init(PPUContext& ctx, u32 rec, u32 init, u32 config, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    wr32(out, 0);
    if (!config) return Es(2);
    if (!has_image()) return Es(1);
    const u32 was = lock_static();
    s32 r = Es(3);
    if (was) {
        const u32 w = rec + 8;
        const u32 v = lock_word(w, 0x80000000u) & 0x7FFFFFFFu;
        if (v & 0x40000000u) {
            r = Es(6);
            wr32(out, rec);
        } else {
            r = calls(init, {config, out});
            if (!r) {
                wr32(st() + 0x68, rd32(out));
                or_word(w, 0x40000000u);
                add_word(w, 1);
            }
        }
        unlock_word(w);
    }
    unlock_static(was);
    return r;
}

s32 cellFontDelete(PPUContext& ctx, u32 lib, u32 p)
{
    Scope sc(ctx);
    if (!lib || !p) return Es(2);
    switch (rd32(p) & 0xFFF00000u) {
    case 0xCF000000u: // a font
        return rd32(p + cf::library) == lib ? close_font(p) : Es(2);
    case 0xCF100000u: { // a renderer
        const u32 l = rd32(p + rn::library);
        if (!l) return Es(2);
        const u32 fn = rd32(l + 0x58);
        if (!fn) return Es(0x10);
        const s32 r = calls(fn, {p});
        if (!r) wr32(p + rn::library, 0);
        return r;
    }
    case 0xCF200000u: // a glyph
        return calls(tab(lib, 0x50), {lib, p});
    case 0xCFE00000u: // graphics
        return end_graphics(p);
    case 0xCFF00000u: // a library
        return end_library(p);
    default:
        return Es(2);
    }
}

// libfont's extension queries {count, then {query, out}}: the sizes of its records, the offsets of a draw context's
// fields, the graphics functions.
s32 cellFontExtend(PPUContext& ctx, u32 magic, u64 zero_, u32 list)
{
    Scope sc(ctx);
    if (magic != 0xCFE00000u || zero_ || !list) return Es(0x10);
    for (s32 i = 0; i < rds32(list); i++) {
        const u32 q = rd32(list + 4 + 8 * static_cast<u32>(i)), out = rd32(list + 8 + 8 * static_cast<u32>(i));
        if (!out) continue;
        u32 v = 0;
        switch (q) {
        case 0xCF300001u: case 0xCF300011u: v = 12; break;
        case 0xCF300003u: case 0xCF300014u: v = 24; break;
        case 0xCF300004u: v = 48; break;
        case 0xCF300005u: v = 36; break;
        case 0xCF300006u: v = 72; break;
        case 0xCF300007u: v = 84; break;
        case 0xCF300015u: v = 32; break;
        case 0xCF500001u: v = 0x42; break;
        case 0xCF500002u: v = 0x70; break;
        case 0xCF500003u: v = 0x60; break;
        case 0xCF500004u: v = 0x80; break;
        case 0xCFE00001u: v = desc("pspocket_font_graphics_init"); break;
        case 0xCFE00002u: v = desc(0x8D5094C4, "cellFont_8D5094C4"); break;
        case 0xCFE00003u: v = desc(0x47B70477, "cellFont_47B70477"); break;
        default:
            if ((q & 0xF00) != 0xF00) return Es(2);
            break;
        }
        wr32(out, v);
    }
    return 0;
}

// Functions turned off (0xDF, 1..9: a bit of the settings' word each, from the top); 0xCF does nothing.
s32 cellFontPatchWorks(PPUContext& ctx, s32 cmd, u64 arg)
{
    Scope sc(ctx);
    if (cmd == 0xCF) return 0;
    if (cmd != 0xDF || arg - 1 > 8) return Es(0x10);
    if (!has_image()) return Es(1);
    wr64(cfg() + 8, rd64(cfg() + 8) | (0x8000000000000000ull >> (arg - 1)));
    return 0;
}

s32 cellFontGlyphGetOutlineVertexes(PPUContext& ctx, u32 glyph, float distance, u32 iface, u32 bbox, u32 count)
{
    Scope sc(ctx);
    if (!glyph) return Es(2);
    const u32 fn = iface ? rd32(iface) : 0, arg = iface ? rd32(iface + 4) : 0;
    const Put put = [&](s32 c, float x, float y, s32 k, u32 kind) {
        if (fn)
            callf(fn, {arg, static_cast<u64>(static_cast<s64>(c)), static_cast<u64>(static_cast<s64>(k)), kind}, {x, y});
    };
    return vertexes(glyph, distance, put, bbox, count);
}

// A vertexes glyph: an 84-byte header (its size, contours, the fields' layout, the glyph's scale and weight, the
// bounding box's corners and centre) and the vertexes {x, y, index} after it.
s32 cellFontGlyphSetupVertexesGlyph(PPUContext& ctx, u32 glyph, float distance, u32 mem, u32 size, u32 vg, u32 bytes)
{
    Scope sc(ctx);
    if (bytes) wr32(bytes, 0);
    if (!vg) return Es(2);
    wr32(vg, 0);
    wr32(vg + 4, 0);
    if (!mem) return Es(2);
    u32 cur = mem + 84;
    const u32 end = mem + (size / 12) * 12 - 12;
    s32 err = 0;
    const Put put = [&](s32, float x, float y, s32 k, u32) {
        if (end < cur) {
            err = Es(0x16);
            return;
        }
        const u32 at = cur;
        cur += 12;
        wrf(at, x);
        wrf(at + 4, y);
        wrf(at + 8, i2f(k));
    };
    Local box(16), count(8);
    if (const s32 r = vertexes(glyph, distance, put, box.addr(), count.addr())) return r;
    const u32 n = count.get();
    if (!n) return 0;
    const u32 total = n * 12 + 84;
    if (bytes) wr32(bytes, total);
    if (err) return err;
    wr32(vg, mem);
    wr32(mem, total);
    wr16(mem + 4, rd16(glyph + 40));
    wr8(mem + 6, 12);
    wr8(mem + 7, 21);
    wr32(mem + 8, 0x0664B255);
    const u32 env = rd32(glyph + 60);
    wr32(mem + 12, rd32(env));
    wr32(mem + 16, rd32(env + 4));
    wr32(mem + 20, rd32(env + 12));
    const u32 b = box.addr();
    const u32 x0 = rd32(b), y0 = rd32(b + 4), x1 = rd32(b + 8), y1 = rd32(b + 12);
    const u32 corners[4][3] = {{x0, y1, std::bit_cast<u32>(-1.0f)}, {x1, y1, std::bit_cast<u32>(-2.0f)},
        {x0, y0, std::bit_cast<u32>(-3.0f)}, {x1, y0, std::bit_cast<u32>(-4.0f)}};
    for (u32 i = 0; i < 4; i++)
        for (u32 k = 0; k < 3; k++) wr32(mem + 24 + 12 * i + 4 * k, corners[i][k]);
    wrf(mem + 72, (rdf(b) + rdf(b + 8)) * 0.5f);
    wrf(mem + 76, (rdf(b + 4) + rdf(b + 12)) * 0.5f);
    wr32(mem + 80, 0);
    return 0;
}

s32 cellFontGlyphGetVertexesGlyphSize(PPUContext& ctx, u32 glyph, float distance, u32 out)
{
    Scope sc(ctx);
    if (!out) return Es(2);
    wr32(out, 0);
    if (!glyph) return Es(2);
    u32 cur = 84;
    const Put put = [&](s32, float, float, s32, u32) {
        if (!(0x40000000u < cur)) cur += 12;
    };
    Local box(16), count(8);
    if (const s32 r = vertexes(glyph, distance, put, box.addr(), count.addr())) return r;
    const u32 n = count.get();
    wr32(out, n ? n * 12 + 84 : 0);
    return 0;
}

// A vertexes glyph moved: its vertexes copied to `data` (when given their size, checked against the header's), a new
// header made of the old one's fields.
s32 cellFontVertexesGlyphRelocate(PPUContext& ctx, u32 src, u32 dst, u32 head, u32 data, u32 size)
{
    Scope sc(ctx);
    if (!dst) return Es(2);
    if (dst != src) {
        wr32(dst + 4, 0);
        wr32(dst, 0);
    }
    if (!src || !head) return Es(2);
    const u32 old = rd32(src);
    if (!data) return old ? Es(2) : 0;
    if (!old) return 0;
    if (size) {
        if (rd32(old) != size) return Es(2);
        if (data != old)
            for (u32 o = 0; o < size; o += 4) wr32(data + o, rd32(old + o));
    }
    zero(head, 12);
    wr32(head, rd32(old));
    wr16(head + 4, rd16(old + 4));
    wr8(head + 6, rd8(old + 6));
    wr8(head + 7, rd8(old + 7));
    wr32(head + 8, (rd32(old + 8) & 0xC1F003FFu) | 0x06032000u);
    wr32(head + 12, rd32(old + 12));
    wr32(head + 16, rd32(old + 16));
    wr32(head + 24, rd32(old + 36));
    wr32(head + 28, rd32(old + 40));
    wr32(head + 32, rd32(old + 48));
    wr32(head + 36, rd32(old + 52));
    wr32(dst + 4, head);
    wr32(dst, data);
    return 0;
}

} // namespace

#define FONT_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, false}
#define FONT_INTERNAL(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, true}

void register_cellFont(Registry& r)
{
    r.add("cellFont", {
        FONT_FUNC(0x47CA71EF, cellFontAdjustFontScaling),
        FONT_FUNC(0x9E3B1E16, cellFontAdjustGlyphExpandBuffer),
        FONT_FUNC(0x66A23100, cellFontBindRenderer),
        FONT_FUNC(0x3DA90559, cellFontClearFileCache),
        FONT_FUNC(0xB276F1F6, cellFontCloseFont),
        FONT_FUNC(0x3897F229, cellFontControl),
        FONT_FUNC(0x042E74E3, cellFontCreateRenderer),
        FONT_FUNC(0xA7B2103A, cellFontDelete),
        FONT_FUNC(0xD62F5D76, cellFontDeleteGlyph),
        FONT_FUNC(0x21EBB248, cellFontDestroyRenderer),
        FONT_FUNC(0x7AB47F7E, cellFontEnd),
        FONT_FUNC(0x8A35C887, cellFontEndGraphics),
        FONT_FUNC(0x40D40544, cellFontEndLibrary),
        FONT_FUNC(0xDEE0836C, cellFontExtend),
        FONT_FUNC(0xC17259DE, cellFontGenerateCharGlyph),
        FONT_FUNC(0xCAED32C1, cellFontGenerateCharGlyphVertical),
        FONT_FUNC(0xC91C8ECE, cellFontGetBindingRenderer),
        FONT_FUNC(0xD8EAEE9F, cellFontGetCharGlyphMetrics),
        FONT_FUNC(0xFE9A6DD7, cellFontGetCharGlyphMetricsVertical),
        FONT_FUNC(0xE16E679A, cellFontGetEffectSlant),
        FONT_FUNC(0x25DBEFF9, cellFontGetEffectWeight),
        FONT_FUNC(0x98AC5524, cellFontGetFontIdCode),
        FONT_FUNC(0x59EF0073, cellFontGetGlyphExpandBufferInfo),
        FONT_FUNC(0x1387C45C, cellFontGetHorizontalLayout),
        FONT_FUNC(0x7C5DF0D8, cellFontGetInitializedRevisionFlags),
        FONT_FUNC(0x06BE743D, cellFontGetKerning),
        FONT_FUNC(0x5ABD8B1E, cellFontGetLibrary),
        FONT_FUNC(0x2DA9FD9D, cellFontGetRenderCharGlyphMetrics),
        FONT_FUNC(0x700E6223, cellFontGetRenderCharGlyphMetricsVertical),
        FONT_FUNC(0xCED4DDA9, cellFontGetRenderEffectSlant),
        FONT_FUNC(0x0109F3D3, cellFontGetRenderEffectWeight),
        FONT_FUNC(0x0D106A11, cellFontGetRenderScalePixel),
        FONT_FUNC(0xA165DAAE, cellFontGetRenderScalePoint),
        FONT_FUNC(0x0BAF90FE, cellFontGetRenderScaledKerning),
        FONT_FUNC(0x8A632038, cellFontGetResolutionDpi),
        FONT_FUNC(0xB015A84E, cellFontGetRevisionFlags),
        FONT_FUNC(0x285D30D6, cellFontGetScalePixel),
        FONT_FUNC(0xF7A19060, cellFontGetScalePoint),
        FONT_FUNC(0x698897F8, cellFontGetVerticalLayout),
        FONT_FUNC(0x231D5941, cellFontGlyphGetHorizontalShift),
        FONT_FUNC(0xA8FAE920, cellFontGlyphGetOutlineControlDistance),
        FONT_FUNC(0x9C8D3FF7, cellFontGlyphGetOutlineVertexes),
        FONT_FUNC(0x22E24707, cellFontGlyphGetScalePixel),
        FONT_FUNC(0xB4D112AF, cellFontGlyphGetVertexesGlyphSize),
        FONT_FUNC(0x534E785F, cellFontGlyphGetVerticalShift),
        FONT_FUNC(0xE01B199E, cellFontGlyphRenderImage),
        FONT_FUNC(0x97B95244, cellFontGlyphRenderImageHorizontal),
        FONT_FUNC(0x8E3F2C40, cellFontGlyphRenderImageVertical),
        FONT_FUNC(0x53F529FE, cellFontGlyphSetupVertexesGlyph),
        FONT_FUNC(0x03A142B9, cellFontGraphicsGetDrawType),
        FONT_FUNC(0xA41342DC, cellFontGraphicsGetFontRGBA),
        FONT_FUNC(0xF7AAA8E2, cellFontGraphicsGetLineRGBA),
        FONT_FUNC(0x2388186C, cellFontGraphicsGetScalePixel),
        FONT_FUNC(0x87BD650F, cellFontGraphicsSetDrawType),
        FONT_FUNC(0x061049AD, cellFontGraphicsSetFontRGBA),
        FONT_FUNC(0x7C83BC15, cellFontGraphicsSetLineRGBA),
        FONT_FUNC(0x16322DF1, cellFontGraphicsSetScalePixel),
        FONT_FUNC(0x970D4C22, cellFontGraphicsSetupDrawContext),
        FONT_FUNC(0xF03DCC29, cellFontInitializeWithRevision),
        FONT_FUNC(0x0A7306A4, cellFontOpenFontFile),
        FONT_FUNC(0x29329541, cellFontOpenFontInstance),
        FONT_FUNC(0x9E19072B, cellFontOpenFontMemory),
        FONT_FUNC(0xA885CC9B, cellFontOpenFontset),
        FONT_FUNC(0x073FA321, cellFontOpenFontsetOnMemory),
        FONT_FUNC(0xB3D304B2, cellFontPatchWorks),
        FONT_FUNC(0x88BE4799, cellFontRenderCharGlyphImage),
        FONT_FUNC(0x1A218FE4, cellFontRenderCharGlyphImageHorizontal),
        FONT_FUNC(0xE857A0CA, cellFontRenderCharGlyphImageVertical),
        FONT_FUNC(0x90B9465E, cellFontRenderSurfaceInit),
        FONT_FUNC(0xB422B005, cellFontRenderSurfaceSetScissor),
        FONT_FUNC(0x8657C8F5, cellFontSetEffectSlant),
        FONT_FUNC(0x25253FE4, cellFontSetEffectWeight),
        FONT_FUNC(0x6CFADA83, cellFontSetFontOpenMode),
        FONT_FUNC(0x6BF6F832, cellFontSetFontsetOpenMode),
        FONT_FUNC(0xFB3341BA, cellFontSetResolutionDpi),
        FONT_FUNC(0x297F0E93, cellFontSetScalePixel),
        FONT_FUNC(0x70F3E728, cellFontSetScalePoint),
        FONT_FUNC(0x78D05E08, cellFontSetupRenderEffectSlant),
        FONT_FUNC(0xA6DC25D1, cellFontSetupRenderEffectWeight),
        FONT_FUNC(0x227E1E3C, cellFontSetupRenderScalePixel),
        FONT_FUNC(0x4D19C631, cellFontSetupRenderScalePoint),
        FONT_FUNC(0xE2F14DCF, cellFontStatic),
        FONT_FUNC(0xF16379FA, cellFontUnbindRenderer),
        FONT_FUNC(0x6BAD7A69, cellFontVertexesGlyphRelocate),
        FONT_FUNC(0x47B70477, cellFont_47B70477),
        FONT_FUNC(0x57E03823, cellFont_57E03823),
        FONT_FUNC(0x8D5094C4, cellFont_8D5094C4),
        FONT_FUNC(0x67E805C3, cellFontsetUSleep),
        FONT_INTERNAL(0xB60CEA19, pspocket_font_cache_lock),
        FONT_INTERNAL(0xD59C795C, pspocket_font_cache_unlock),
        FONT_INTERNAL(0xACF6C8B6, pspocket_font_composite_codes),
        FONT_INTERNAL(0x00CDE6AD, pspocket_font_graphics_init),
    });
}

} // namespace pspocket::hle

#else

namespace pspocket::hle {

// Without FreeType (PSPOCKET_FREETYPE_DIR) the firmware's libfont serves the games, over the firmware's libfontFT.
void register_cellFont(Registry&) {}

} // namespace pspocket::hle

#endif
