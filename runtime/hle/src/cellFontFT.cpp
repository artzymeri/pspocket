// cellFontFT: Sony's libfontFT.sprx as pspocket's own (D-526). libfontFT is the font library of libfont's FreeType
// type: cellFontInitLibraryFreeType hands libfont a CellFontLibrary (libfontFT's static record) whose table of 34
// functions libfont calls for everything it does with a font (open, scale, measure, render, generate outlines), and
// libfontFT's exports (FTManager_*, FTFaceH_*, FTCacheStream_*) are the layer those functions sit on: a manager over
// one FT_Library, a "face handle" over each FT_Face, a stream reading files through libfont's file cache.
//
// pspocket's runs on pspocket's own cell_FreeType2 (D-525) through its guest API, as Sony's runs on libfreetype: its
// records are in guest memory in Sony's layout (the library record, face handles, renderers, cache streams), allocated
// through the same FT_Memory calls, and FreeType's objects are read where libfontFT reads them (the guest mirrors of
// faces, sizes and glyph slots). Where libfontFT reaches into FreeType's internals (a slot's glyph loader, a face's
// transform flags, TrueType's maximum profile), the host's FreeType objects are used. Sony's arithmetic is kept: 32-bit
// and 64-bit integer steps as the PPU makes them (truncating shifts, sign extensions), single-precision floats with
// fused multiply-adds only where Sony's code has them.
//
// Reference: Sony's libfontFT.sprx (firmware 4.92) as the oracle (tests/hle/test_fontft_compare.cpp, with Sony's
// libfont over it); FreeType 2.2.1 for the structures it reads.

#include "pspocket/hle/hle.hpp"

#if PSPOCKET_HAVE_FREETYPE

#include "freetype_bridge.hpp"

#include FT_INTERNAL_OBJECTS_H
#include FT_INTERNAL_GLYPH_LOADER_H
#include FT_INTERNAL_TRUETYPE_TYPES_H

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

// Sony's code multiplies and adds in separate single-precision steps except where it uses fmadds/fmsubs (std::fma
// here): no contraction.
#pragma clang fp contract(off)

namespace pspocket::hle {

namespace fontft {

using namespace ftb;

constexpr const char* kChan = "cellFontFT";

constexpr u32 E(u32 n) { return 0x80540000u | n; }
constexpr s32 kErrFace = 35;  // FT_Err_Invalid_Face_Handle, libfontFT's answer for a missing face or handle
constexpr s32 kErrArg = 6;    // FT_Err_Invalid_Argument
constexpr s32 kErrOom = 0x40; // FT_Err_Out_Of_Memory

// ------------------------------------------------------------------------------------------------- arithmetic

// sradi/srawi n then addze: a division by 2^n truncating toward zero.
constexpr s64 t0(s64 v, int n) { return v / (s64{1} << n); }
// (s64)a * b, then t0
constexpr s64 mulsh(s64 a, s64 b, int n) { return t0(a * b, n); }

// fctiwz and fctidz: toward zero, saturating, NaN as the most negative value.
s32 fctiwz(double v)
{
    if (std::isnan(v)) return std::numeric_limits<s32>::min();
    if (v >= 2147483647.0) return std::numeric_limits<s32>::max();
    if (v <= -2147483648.0) return std::numeric_limits<s32>::min();
    return static_cast<s32>(v);
}
s64 fctidz(double v)
{
    if (std::isnan(v)) return std::numeric_limits<s64>::min();
    if (v >= 9223372036854775807.0) return std::numeric_limits<s64>::max();
    if (v <= -9223372036854775808.0) return std::numeric_limits<s64>::min();
    return static_cast<s64>(v);
}
s32 fctiwz(float v) { return fctiwz(static_cast<double>(v)); }
s64 fctidz(float v) { return fctidz(static_cast<double>(v)); }
// (float)(int): fcfid then frsp (exact for 32-bit values); for 64-bit values Sony's code rounds once to single.
float i2f(s64 v) { return static_cast<float>(v); }

float rdf(u32 a) { return std::bit_cast<float>(rd32(a)); }
void wrf(u32 a, float v) { wr32(a, std::bit_cast<u32>(v)); }

// ------------------------------------------------------------------------------------------------- other modules

u32 nid(const char* name)
{
    static std::mutex m;
    static std::unordered_map<const char*, u32> cache;
    std::lock_guard g(m);
    auto [it, inserted] = cache.try_emplace(name, 0);
    if (inserted) it->second = recomp::nid_of(name);
    return it->second;
}

// pspocket's cell_FreeType2, called as libfontFT calls libfreetype.
u64 ft(const char* name, std::initializer_list<u64> args) { return call_hle("cell_FreeType2", nid(name), args); }
s32 fts(const char* name, std::initializer_list<u64> args) { return static_cast<s32>(static_cast<u32>(ft(name, args))); }

// libfont's functions libfontFT imports (cellFontStatic, cellFontsetUSleep, cellFontGetRevisionFlags,
// cellFontControl): the firmware's libfont when it runs as it is, else pspocket's cellFont.
u64 font(const char* name, std::initializer_list<u64> args)
{
    const u32 n = nid(name);
    if (const u32 opd = HleProcess::current()->imported("cellFont", n)) return call_guest_fn(opd, args);
    if (registry().find("cellFont", n)) return call_hle("cellFont", n, args);
    PS_LOG_ERROR(kChan, "cellFont::{} is neither the firmware's nor pspocket's here", name);
    return 0;
}

// The descriptor of one of this module's internal functions.
u32 desc(const char* name) { return HleProcess::current()->descriptor_of("cellFontFT", nid(name)); }

// A guest variable on the caller's stack (libfontFT's locals whose address it hands to FreeType).
struct Local {
    explicit Local(u32 size) : v(*current_ctx(), gbase(), size) {}
    u32 addr() const { return v.addr(); }
    u32 get() const { return rd32(v.addr()); }
    StackVar v;
};

// ------------------------------------------------------------------------------------------------- the library
// libfontFT's static CellFontLibrary (256 bytes) and its shared FreeType library record.
namespace lb {
constexpr u32 type = 0, version = 4, lock = 8, w152 = 152, w154 = 154, revision = 160, manager = 168,
              mem_object = 192, mem_malloc = 196, mem_free = 200, mem_realloc = 204, mem_calloc = 208,
              memory_rec = 212, old_sdk = 232, lock_if = 240, lock_opd = 244, size = 256; // +248: the unlock's
constexpr u32 kType = 0xCFF00002, kVersion = 0x02020100;
constexpr u32 kLocked = 0x80000000u, kInitialized = 0x40000000u, kCount = 0x3FFFFFFFu;
} // namespace lb

// The table libfont calls, in libfontFT's order (offset from the record).
constexpr std::pair<u32, const char*> kTable[] = {
    {16, "pspocket_fontft_end_library"}, {20, "pspocket_fontft_open_file"}, {24, "pspocket_fontft_open_memory"},
    {28, "pspocket_fontft_close_font"}, {32, "pspocket_fontft_set_composite_codes"},
    {36, "pspocket_fontft_get_composite_codes"}, {40, "pspocket_fontft_set_scale_point_dpi"},
    {44, "pspocket_fontft_set_scale_point"}, {48, "pspocket_fontft_set_effect_weight"},
    {52, "pspocket_fontft_set_effect_slant"}, {56, "pspocket_fontft_horizontal_layout"},
    {60, "pspocket_fontft_vertical_layout"}, {64, "pspocket_fontft_glyph_metrics"},
    {68, "pspocket_fontft_glyph_metrics_vertical"}, {72, "pspocket_fontft_generate_glyph"},
    {76, "pspocket_fontft_generate_glyph_vertical"}, {80, "pspocket_fontft_free_memory"},
    {84, "pspocket_fontft_create_renderer"}, {88, "pspocket_fontft_destroy_renderer"},
    {92, "pspocket_fontft_setup_render_scale_dpi"}, {96, "pspocket_fontft_setup_render_scale"},
    {100, "pspocket_fontft_render_glyph"}, {104, "pspocket_fontft_render_glyph_shift"},
    {108, "pspocket_fontft_render_vertexes_glyph"}, {112, "pspocket_fontft_read_file"},
    {116, "pspocket_fontft_alloc_memory"}, {120, "pspocket_fontft_free_pointer"}, {124, "pspocket_fontft_load_file"},
    {128, "pspocket_fontft_kerning"}, {132, "pspocket_fontft_expand_glyph_buffer"},
    {136, "pspocket_fontft_glyph_buffer_info"}, {140, "pspocket_fontft_set_em_scale"},
    {144, "pspocket_fontft_em_center"}, {148, "pspocket_fontft_set_open_mode"},
};

struct State {
    std::mutex m;
    u32 lib = 0;    // the CellFontLibrary
    u32 shared = 0; // the shared library record: +4 is a manager (library, 1, kind, references); byte 0 is ""
};

State& state() { return HleProcess::current()->state<State>(); }

void make_records(State& s)
{
    MainMemory& mm = HleProcess::current()->main_memory();
    s.lib = mm.alloc(lb::size, 0x10);
    s.shared = mm.alloc(0x20, 0x10);
    if (!s.lib || !s.shared) PS_FATAL(kChan, "no main memory for libfontFT's records");
    std::memset(hp(s.lib), 0, lb::size);
    std::memset(hp(s.shared), 0, 0x20);
    const u32 g = s.lib;
    wr32(g + lb::type, lb::kType);
    wr32(g + lb::version, lb::kVersion);
    for (const auto& [off, name] : kTable) wr32(g + off, desc(name));
    wr32(g + lb::memory_rec + lay::mem_alloc, desc("pspocket_fontft_alloc"));
    wr32(g + lb::memory_rec + lay::mem_free, desc("pspocket_fontft_free"));
    wr32(g + lb::memory_rec + lay::mem_realloc, desc("pspocket_fontft_realloc"));
}

u32 lib()
{
    State& s = state();
    std::lock_guard g(s.m);
    if (!s.lib) make_records(s);
    return s.lib;
}

u32 shared()
{
    lib();
    return state().shared;
}

u32 manager() { return lib() + lb::manager; }

bool old_sdk() { return (ps_read64(gbase(), lib() + lb::old_sdk) & 1) != 0; }

// The library's lock (its word's top bit; libfontFT spins on it, sleeping a millisecond between tries).
void lock_lib(u32 word)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, word);
        if (!(v & lb::kLocked)) {
            if (atomic_cas_be32(base, word, v, v | lb::kLocked) == v) return;
            continue;
        }
        if (const u32 sleep = static_cast<u32>(font("cellFontsetUSleep", {}))) call_guest_fn(sleep, {1000});
        else lv2_syscall(*current_ctx(), 141, {1000}); // sys_timer_usleep
    }
}

// Unlocking clears the lock bit and bit 29 (libfontFT's mask 0x5FFFFFFF).
void unlock_lib(u32 word)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, word);
        if (atomic_cas_be32(base, word, v, v & 0x5FFFFFFFu) == v) return;
    }
}

void count_up(u32 word)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, word);
        if (atomic_cas_be32(base, word, v, v + 1) == v) return;
    }
}

void count_down(u32 word)
{
    u8* base = gbase();
    for (;;) {
        const u32 v = atomic_load_be32(base, word);
        if (!v || atomic_cas_be32(base, word, v, v - 1) == v) return;
    }
}

// The FT_Memory of the manager's library (libfontFT's A5979570 on the manager: **manager).
u32 memory_of(u32 mgr)
{
    if (!mgr) return 0;
    const u32 l = rd32(mgr);
    return l ? rd32(l + lay::lib_memory) : 0;
}
u32 memory() { return memory_of(manager()); }

s32 ft_alloc(u32 mem, s64 size, u32 out) { return fts("FT_Alloc", {mem, static_cast<u64>(size), out}); }
void ft_free(u32 mem, u32 p) { ft("FT_Free", {mem, p}); }

// ------------------------------------------------------------------------------------------------- streams
// FT_Stream_Open on a guest FT_StreamRec, as Sony's libfreetype fills one (its size, file, path, position, read and
// close; the rest is left as it was), with reads and closes of this module's own over Sony's file layer.
s32 stream_open(u32 stream, u32 path)
{
    if (!stream) return 0x28; // FT_Err_Invalid_Stream_Handle
    if (!path) return FT_Err_Cannot_Open_Resource;
    u32 fd = 0, size = 0;
    const FT_Error e = file_open(path, &fd, &size);
    if (e) return e;
    wr32(stream + lay::st_size, size);
    wr32(stream + lay::st_descriptor, fd);
    wr32(stream + lay::st_pathname, path);
    wr32(stream + lay::st_pos, 0);
    wr32(stream + lay::st_read, desc("pspocket_fontft_file_io"));
    wr32(stream + lay::st_close, desc("pspocket_fontft_file_close"));
    return 0;
}

u32 stream_read(u32 stream, u32 offset, u32 buffer, u32 count)
{
    return static_cast<u32>(call_guest_fn(rd32(stream + lay::st_read), {stream, offset, buffer, count}));
}
void stream_close(u32 stream) { call_guest_fn(rd32(stream + lay::st_close), {stream}); }

} // namespace fontft

namespace {

using namespace fontft;

// ------------------------------------------------------------------------------------------------- internals
// The FT_Memory libfontFT gives its library: the CellFontMemoryInterface libfont was set up with.

u32 pspocket_fontft_alloc(PPUContext& ctx, u32, u32 size)
{
    Scope sc(ctx);
    const u32 g = lib();
    return static_cast<u32>(call_guest_fn(rd32(g + lb::mem_malloc), {rd32(g + lb::mem_object), size}));
}

void pspocket_fontft_free(PPUContext& ctx, u32, u32 block)
{
    Scope sc(ctx);
    const u32 g = lib();
    call_guest_fn(rd32(g + lb::mem_free), {rd32(g + lb::mem_object), block});
}

// Without the interface's Realloc: a new block, the smaller size copied, the old one freed.
u32 pspocket_fontft_realloc(PPUContext& ctx, u32, u32 cur, u32 size, u32 block)
{
    Scope sc(ctx);
    const u32 g = lib();
    const u32 object = rd32(g + lb::mem_object);
    if (const u32 re = rd32(g + lb::mem_realloc)) return static_cast<u32>(call_guest_fn(re, {object, block, size}));
    if (cur == size) return block;
    if (!size) {
        if (block) call_guest_fn(rd32(g + lb::mem_free), {object, block});
        return 0;
    }
    const u32 n = static_cast<u32>(call_guest_fn(rd32(g + lb::mem_malloc), {object, size}));
    if (!n) return 0;
    const u32 copy = static_cast<s32>(cur) <= static_cast<s32>(size) ? cur : size;
    if (copy) std::memmove(hp(n), hp(block), copy);
    if (block) call_guest_fn(rd32(g + lb::mem_free), {object, block});
    return n;
}

// libfont's lock interface {?, object, lock, unlock} at cellFontStatic() + 88: the library record's +240 points to it,
// and its +244 and +248 are these two (a cache stream's lock).
void pspocket_fontft_lock(PPUContext& ctx, u32 x)
{
    Scope sc(ctx);
    call_guest_fn(rd32(x + 8), {rd32(x + 4)});
}

void pspocket_fontft_unlock(PPUContext& ctx, u32 x)
{
    Scope sc(ctx);
    call_guest_fn(rd32(x + 12), {rd32(x + 4)});
}

// A file stream's read and close (Sony's ft_ansi_stream_io and ft_ansi_stream_close).
u32 pspocket_fontft_file_io(PPUContext& ctx, u32 stream, u32 offset, u32 buffer, u32 count)
{
    Scope sc(ctx);
    return static_cast<u32>(
        file_read(rd32(stream + lay::st_descriptor), offset, buffer ? hp(buffer) : nullptr, count));
}

void pspocket_fontft_file_close(PPUContext& ctx, u32 stream)
{
    Scope sc(ctx);
    file_close(rd32(stream + lay::st_descriptor));
    wr32(stream + lay::st_descriptor, 0);
    wr32(stream + lay::st_size, 0);
    wr32(stream + lay::st_base, 0);
}

} // namespace

namespace fontft {

// ------------------------------------------------------------------------------------------------- managers
// A manager: {FT_Library, 1, kind (0 the game's library, 1 FT_New_Library, 3 FT_Init_FreeType), references}. The
// shared record's +4 is one too (the library libfontFT makes is shared by every manager without one of its own).
namespace mg {
constexpr u32 library = 0, active = 4, kind = 8, refs = 12;
}

// Makes the shared library (FTManager_Init_FreeType's): false when FreeType failed (the record's library cleared).
bool make_shared_library(u32 d, u32 mem)
{
    if (mem) {
        if (fts("FT_New_Library", {mem, d + 4})) {
            wr32(d + 4, 0);
            return false;
        }
        const u32 l = rd32(d + 4);
        if (!rd32(l + lay::lib_version)) {
            wr32(l + lay::lib_version, 2);
            wr32(l + lay::lib_version + 4, 2);
            wr32(l + lay::lib_version + 8, 1);
        }
        ft("FT_Add_Default_Modules", {rd32(d + 4)});
        wr32(d + 12, 1);
    } else {
        if (fts("FT_Init_FreeType", {d + 4})) {
            wr32(d + 4, 0);
            return false;
        }
        wr32(d + 12, 3);
    }
    return true;
}

// A manager letting go of the shared library: the last reference does it (FT_Done_Library, then the FT_Memory record
// freed through itself). Returns what libfontFT's code returns there (the old library when the record had no kind).
u32 release_shared_library()
{
    const u32 d = shared();
    const u32 l = rd32(d + 4);
    if (!l || !rd32(d + 12)) {
        wr32(d + 4, 0);
        return l;
    }
    const u32 refs = rd32(d + 16);
    if (!refs) return 0;
    wr32(d + 16, refs - 1);
    if (refs - 1) return 0;
    if (rd32(d + 12) == 3) {
        ft("FT_Done_FreeType", {l});
    } else {
        const u32 mem = rd32(l + lay::lib_memory);
        ft("FT_Done_Library", {l});
        call_guest_fn(rd32(mem + lay::mem_free), {mem, mem});
    }
    wr32(d + 4, 0);
    return 0;
}

u32 init_freetype(u32 mgr, u32 library, u32 mem)
{
    const u32 d = shared();
    if (!mgr) {
        if (!rd32(d + 4)) {
            if (library) {
                wr32(d + 12, 0);
                wr32(d + 4, library);
            } else if (!make_shared_library(d, mem)) {
                return 0;
            }
        }
        wr32(d + 16, rd32(d + 16) + 1);
        mgr = d + 4;
    } else {
        if (!library) {
            if (!rd32(d + 4) && !make_shared_library(d, mem)) return 0;
            wr32(d + 16, rd32(d + 16) + 1);
            wr32(mgr + mg::kind, rd32(d + 12));
            library = rd32(d + 4);
        } else {
            wr32(mgr + mg::kind, 0);
        }
        wr32(mgr + mg::library, library);
        wr32(mgr + mg::refs, 1);
        wr32(mgr + mg::active, 1);
    }
    // a FreeType with the bytecode interpreter (FT_TRUETYPE_ENGINE_TYPE_PATENTED) is refused
    if (fts("FT_Get_TrueType_Engine_Type", {rd32(mgr + mg::library)}) == 2) {
        if (rd32(mgr + mg::kind)) release_shared_library();
        if (rd32(mgr + mg::active)) {
            wr32(mgr + mg::library, 0);
            wr32(mgr + mg::refs, 0);
        }
        return 0;
    }
    return mgr;
}

u32 done_freetype(u32 mgr)
{
    if (!mgr) return release_shared_library();
    u32 r = 0;
    if (rd32(mgr + mg::kind)) r = release_shared_library();
    if (rd32(mgr + mg::active)) {
        wr32(mgr + mg::library, 0);
        wr32(mgr + mg::refs, 0);
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- face handles
// A face handle (256 bytes, FT_Alloc'd by libfontFT's table functions).
namespace fh {
constexpr u32 face = 0, library = 4, args = 8, args_flags = 8, args_base = 12, args_size = 16, args_path = 20,
              args_stream = 24, mode = 40, em_scale = 44, dpi_x = 48, dpi_y = 52, weight = 56, slant = 60, code = 64,
              codes = 68, metrics = 84, composite_fn = 116, composite_arg = 120, size = 256;
}

void clear_handle(u32 h)
{
    for (const u32 o : {120u, 4u, 0u, 56u, 60u, 64u, 68u, 92u, 96u, 100u, 104u, 108u, 112u, 84u, 88u, 116u})
        wr32(h + o, 0);
}

// FT_Open_Face over the handle's FT_Open_Args, then libfontFT's defaults (the Unicode charmap, 18 pixels).
s32 open_face(u32 mgr, u32 h, s32 index, u32 mode)
{
    const u32 library = rd32(mgr + mg::library);
    Local out(8);
    const s32 e = fts("FT_Open_Face", {library, h + fh::args, static_cast<u64>(static_cast<s64>(index)), out.addr()});
    if (e) return e;
    const u32 face = out.get();
    wr32(h + fh::em_scale, 0);
    wr32(h + fh::face, face);
    wr32(h + fh::library, library);
    wr32(h + fh::mode, mode);
    wr32(mgr + mg::refs, rd32(mgr + mg::refs) + 1);
    ft("FT_Select_Charmap", {rd32(h + fh::face), FT_ENCODING_UNICODE});
    ft("FT_Set_Pixel_Sizes", {rd32(h + fh::face), 18, 18});
    return 0;
}

s32 open_mem_face(u32 mgr, u32 h, u32 data, s32 size, s32 index)
{
    if (!h) return kErrFace;
    clear_handle(h);
    if (size == -1) {
        wr32(h + fh::args_flags, FT_OPEN_STREAM);
        wr32(h + fh::args_stream, data);
    } else if (size == 0) {
        wr32(h + fh::args_flags, FT_OPEN_PATHNAME);
        wr32(h + fh::args_path, data);
    } else {
        wr32(h + fh::args_flags, FT_OPEN_MEMORY);
        wr32(h + fh::args_base, data);
        wr32(h + fh::args_size, static_cast<u32>(size));
    }
    return open_face(mgr, h, index, static_cast<u32>(size));
}

s32 open_stream_face(u32 mgr, u32 h, u32 stream, s32 index)
{
    if (!h) return kErrFace;
    clear_handle(h);
    wr32(h + fh::args_flags, FT_OPEN_STREAM);
    wr32(h + fh::args_stream, stream);
    return open_face(mgr, h, index, 0xFFFFFFFFu);
}

s32 open_file_face(u32 mgr, u32 h, u32 path, s32 index)
{
    if (!h) return kErrFace;
    clear_handle(h);
    wr32(h + fh::args_flags, FT_OPEN_PATHNAME);
    wr32(h + fh::args_path, path);
    return open_face(mgr, h, index, 0);
}

// FT_Done_Face, then what the face was opened from (its flags and the stream, path or memory).
s32 close_face(u32 h, u32 type, u32 what)
{
    if (!h) {
        PS_LOG_ERROR(kChan, "a null face handle closed (Sony's libfontFT reads through it): refused");
        return kErrFace;
    }
    const s32 r = fts("FT_Done_Face", {rd32(h + fh::face)});
    if (type) wr32(type, rd32(h + fh::args_flags));
    switch (rd32(h + fh::args_flags)) {
    case FT_OPEN_STREAM:
        if (what) wr32(what, rd32(h + fh::args_stream));
        break;
    case FT_OPEN_PATHNAME:
        if (what) wr32(what, rd32(h + fh::args_path));
        break;
    case FT_OPEN_MEMORY:
        if (what) wr32(what, rd32(h + fh::args_base));
        break;
    default: break;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- cache streams
// A file read through libfont's file cache: an FT_StreamRec, then {owner id, cache, block index array, the file's own
// read, the lock interface}. The cache: {?, slots, next slot, free slots, ?}, then slots of {owner, block, 8 KB}.
namespace cs {
constexpr u32 id = 40, cache = 44, index = 48, read = 52, lock = 56, size = 60;
constexpr u32 kBlock = 8192, kSlot = kBlock + 8, kSlots = 20;
} // namespace cs

u32 index_size(u32 stream_size)
{
    const u32 v = stream_size + 8191;
    return ((v >> 11) | (v << 21)) & 0x1FFFFCu;
}

s32 cache_stream_init(u32 s, u32 path, u32 id)
{
    if (!s) return kErrArg;
    const s32 e = stream_open(s, path);
    if (!e) wr32(s + cs::id, id);
    wr32(s + cs::cache, 0);
    wr32(s + cs::index, 0);
    return e;
}

s32 cache_stream_cache_init(u32 s, u32 index, u32 cache)
{
    if (!s || !index || !cache || rd32(s + cs::index) || rd32(s + cs::cache)) return kErrArg;
    wr32(s + cs::read, rd32(s + lay::st_read));
    wr32(s + lay::st_read, desc("pspocket_fontft_cached_io"));
    wr32(s + cs::lock, 0);
    wr32(s + cs::index, index);
    wr32(s + cs::cache, cache);
    const u32 n = index_size(rd32(s + lay::st_size));
    if (n > 3)
        for (u32 i = 0; i < ((n - 4) >> 2) + 1; i++) wr32(index + 4 * i, 0);
    return 0;
}

s32 cache_stream_cache_end(u32 s, u32 index, u32 cache)
{
    if (!s) return kErrArg;
    if (index) wr32(index, rd32(s + cs::index));
    if (cache) wr32(cache, rd32(s + cs::cache));
    if (rd32(s + cs::index) && rd32(s + cs::cache)) {
        wr32(s + cs::cache, 0);
        wr32(s + lay::st_read, 0);
        wr32(s + cs::index, 0);
    }
    return 0;
}

s32 cache_stream_end(u32 s)
{
    if (!s) return kErrArg;
    if (rd32(s + cs::index) && rd32(s + cs::cache)) {
        wr32(s + cs::cache, 0);
        wr32(s + lay::st_read, 0);
        wr32(s + cs::index, 0);
    }
    return 0;
}

} // namespace fontft

namespace {

using namespace fontft;

// A cache stream's read: the file's 8 KB blocks through the cache's slots (round robin, a slot taken from another
// file or from this one's blocks outside the read), a read of more than four blocks (or of more blocks than the
// cache has) straight from the file.
u32 pspocket_fontft_cached_io(PPUContext& ctx, u32 s, u32 off, u32 buf, u32 cnt)
{
    Scope sc(ctx);
    if (!cnt) return rd32(s + lay::st_size) < off && !buf ? 1 : 0;
    const u32 in = off & 0x1FFF;
    const u32 b0 = off >> 13;
    const u32 nb = (in + cnt + 8191) >> 13;
    const u32 saved_pos = rd32(s + lay::st_pos);
    const u32 lk = rd32(s + cs::lock);
    if (lk) call_guest_fn(rd32(lk + 4), {rd32(lk)});
    wr32(s + lay::st_pos, off + cnt);
    const u32 cache = rd32(s + cs::cache);
    const u32 slots = rd32(cache + 4);
    if (nb > slots || static_cast<s32>(nb) > 4) {
        u32 r = 0;
        if (const u32 orig = rd32(s + cs::read)) {
            if (lk) call_guest_fn(rd32(lk + 8), {rd32(lk)});
            r = static_cast<u32>(call_guest_fn(orig, {s, off, buf, cnt}));
        }
        wr32(s + lay::st_pos, saved_pos); // Sony's leaves the lock taken when the file has no read of its own
        return r;
    }
    const u32 id = rd32(s + cs::id);
    const u32 index = rd32(s + cs::index);
    const auto finish = [&](u32 total) {
        wr32(s + lay::st_pos, saved_pos);
        if (const u32 l = rd32(s + cs::lock)) call_guest_fn(rd32(l + 8), {rd32(l)});
        return total;
    };
    u32 remaining = cnt, chunk = std::min(cs::kBlock - in, cnt), total = 0, from = in;
    u32 file_off = b0 << 13;
    if (static_cast<s32>(nb) <= 0) return finish(0);
    for (u32 i = 0;; i++) {
        const u32 blk = b0 + i;
        const u32 entry = index + 4 * blk;
        u32 data = 0;
        if (const u32 e = rd32(entry)) {
            const u32 limit = (rd32(cache + 4) - rd32(cache + 12)) * cs::kSlot + cs::kSlots;
            if (limit > e) {
                const u32 slot = cache + e;
                if (rd32(slot) == id && rd32(slot + 4) == blk) data = slot + 8;
                else wr32(entry, 0);
            }
        }
        if (!data) {
            u32 next = rd32(cache + 8);
            u32 off_slot = next * cs::kSlot + cs::kSlots;
            u32 slot = cache + off_slot;
            if (const u32 free = rd32(cache + 12)) {
                wr32(cache + 12, free - 1);
            } else {
                for (;;) {
                    if (rd32(slot) != id) break;
                    const s32 owned = static_cast<s32>(rd32(slot + 4));
                    if (static_cast<s32>(b0) > owned || static_cast<s32>(b0 + nb) <= owned) {
                        if (owned >= 0 && static_cast<u32>(owned) < ((rd32(s + lay::st_size) + 8191) >> 13))
                            wr32(index + 4 * static_cast<u32>(owned), 0);
                        break;
                    }
                    if (!rd32(index + 4 * static_cast<u32>(owned))) break;
                    next = rd32(cache + 8) + 1;
                    wr32(cache + 8, next);
                    if (rd32(cache + 4) <= next) {
                        next = 0;
                        wr32(cache + 8, 0);
                    }
                    off_slot = next * cs::kSlot + cs::kSlots;
                    slot = cache + off_slot;
                }
            }
            wr32(slot, id);
            const u32 size = rd32(s + lay::st_size);
            const u32 want = file_off + cs::kBlock > size ? size - file_off : cs::kBlock;
            const u32 got = static_cast<u32>(call_guest_fn(rd32(s + cs::read), {s, file_off, slot + 8, want}));
            if (got != want) {
                wr32(slot + 4, 0xFFFFFFFFu);
                return finish(total);
            }
            wr32(slot + 4, blk);
            wr32(entry, off_slot);
            next = rd32(cache + 8) + 1;
            wr32(cache + 8, rd32(cache + 4) <= next ? 0 : next);
            data = slot + 8;
        }
        if (chunk) std::memmove(hp(buf + total), hp(data + from), chunk);
        total += chunk;
        file_off += cs::kBlock;
        if (i + 1 == nb) break;
        remaining -= chunk;
        chunk = std::min(remaining, cs::kBlock);
        from = 0;
    }
    return finish(total);
}

} // namespace

namespace fontft {

// ------------------------------------------------------------------------------------------------- face reads
// FreeType 2.2.1's FT_FaceRec and FT_SizeRec as libfontFT reads them (the guest mirrors).
namespace fc {
constexpr u32 xMin = 52, yMin = 56, xMax = 60, yMax = 64, upem = 68, asc = 70, desc = 72, max_adv_w = 76,
              max_adv_h = 78, glyph = 84, size = 88;
} // namespace fc
namespace sz {
constexpr u32 x_ppem = 12, y_ppem = 14, x_scale = 16, y_scale = 20;
}
// FT_GlyphSlotRec's metrics and advance.
namespace sl {
constexpr u32 width = 24, height = 28, hBX = 32, hBY = 36, hAdv = 40, vBX = 44, vBY = 48, vAdv = 52, adv_x = 64,
              adv_y = 68, outline = 108;
}

u32 face_of_h(u32 h) { return h ? rd32(h + fh::face) : 0; }
s32 upem(u32 face) { return rd16(face + fc::upem); }
s32 asc(u32 face) { return rds16(face + fc::asc); }
s32 dsc(u32 face) { return rds16(face + fc::desc); }

// value * the size's x or y scale, shifted right truncating (libfontFT's getters): 0 without a handle, face or size.
template <class V>
s32 scaled(u32 h, bool y, int shift, V value)
{
    const u32 face = face_of_h(h);
    if (!face) return 0;
    const u32 size = rd32(face + fc::size);
    if (!size) return 0;
    const s64 v = value(face);
    return static_cast<s32>(mulsh(v, rds32(size + (y ? sz::y_scale : sz::x_scale)), shift));
}
s32 bbox_w(u32 f) { return static_cast<s32>(rd32(f + fc::xMax) - rd32(f + fc::xMin)); }
s32 bbox_h(u32 f) { return static_cast<s32>(rd32(f + fc::yMax) - rd32(f + fc::yMin)); }

// The effect weight (16.16 of the em) as 26.6 along x or y.
s32 weight_scaled(u32 h, bool y)
{
    const u32 face = face_of_h(h);
    if (!face) return 0;
    const s32 w = rds32(h + fh::weight);
    if (!w) return 0;
    const u32 size = rd32(face + fc::size);
    if (!size) return 0;
    return static_cast<s32>(mulsh(t0(w, 16), rds32(size + (y ? sz::y_scale : sz::x_scale)), 6));
}

// The em box's vertical centre line in 26.6 from the handle's em scale (libfontFT's A52C0070): 35 without a face.
s32 em_center(u32 h)
{
    const u32 face = face_of_h(h);
    if (!face) return kErrFace;
    s32 t = dsc(face) + (upem(face) - asc(face));
    if (t < 0) t = 0;
    const s32 v = asc(face) + static_cast<s32>(t0(t, 1));
    return static_cast<s32>(mulsh(v, rd32(h + fh::em_scale), 16)); // the scale zero-extended, as Sony's lwz leaves it
}

// ------------------------------------------------------------------------------------------------- render sizes
// The bitmap a glyph of this face can need: its bounding box scaled, the weight's widening, the slant's lean (the
// width rounded up to 4 bytes, 4 columns and 5 rows of margin, 9 rows more in the total).
struct Box {
    s64 top, bot, right, left;
};

Box effect_box(s32 xMin, s32 yMin, s32 xMax, s32 yMax, s32 xs, s32 ys, s32 weight, s32 slant)
{
    const s64 wt = t0(weight, 16);
    const s64 dy = static_cast<s32>(t0(ys * wt, 7));
    const s64 dx = static_cast<s32>(t0(xs * wt, 7));
    Box b;
    b.top = (t0(s64{ys} * yMax, 6) + dy) << 6;
    b.bot = (t0(s64{ys} * static_cast<s32>(0u - static_cast<u32>(yMin)), 6) + dy) << 6;
    b.right = (t0(s64{xs} * xMax, 6) + dx) << 6;
    b.left = (t0(s64{xs} * static_cast<s32>(0u - static_cast<u32>(xMin)), 6) + dx) << 6;
    const s64 s = slant;
    if (s > 0) {
        b.left += t0(b.bot * s, 16);
        b.right += t0(b.top * s, 16);
    } else if (s < 0) {
        b.left -= t0(b.top * s, 16);
        b.right -= t0(b.bot * s, 16);
    }
    return b;
}

// libfontFT rounds a 16.16-of-26.6 extent up to pixels: -(floor(trunc(-v / 65536) / 64)).
s64 up(s64 v) { return -(t0(-v, 16) >> 6); }

s32 buffer_size(u32 h, u32 w_out, u32 h_out, u32 x_out, u32 y_out)
{
    const u32 face = face_of_h(h);
    const u32 size = face ? rd32(face + fc::size) : 0;
    if (!size) {
        for (const u32 p : {w_out, h_out, x_out, y_out})
            if (p) wr32(p, 0);
        return 0;
    }
    const Box b = effect_box(rds32(face + fc::xMin), rds32(face + fc::yMin), rds32(face + fc::xMax),
        rds32(face + fc::yMax), rds32(size + sz::x_scale), rds32(size + sz::y_scale), rds32(h + fh::weight),
        rds32(h + fh::slant));
    const s64 bot = up(b.bot), hh = bot + up(b.top), left = up(b.left);
    const s32 wd = static_cast<s32>(left + up(b.right)) + 12;
    const s32 wa = static_cast<s32>(t0(wd, 2)) * 4;
    if (x_out) wr32(x_out, static_cast<u32>(left + 4));
    if (y_out) wr32(y_out, static_cast<u32>(bot + 5));
    if (w_out) wr32(w_out, static_cast<u32>(wa));
    if (h_out) wr32(h_out, static_cast<u32>(hh + 5));
    return static_cast<s32>(static_cast<u32>(hh + 9) * static_cast<u32>(wa));
}

// ------------------------------------------------------------------------------------------------- glyph images
// A loaded glyph's metrics made libfontFT's: the vertical ones from the em box when the handle has an em scale, the
// weight's widening (and the outline emboldened when an image is drawn), the slant's lean.
s32 adjust_glyph(u32 h, u32 face, u32 slot, bool first, bool image)
{
    const auto m = [&](u32 o) { return rds32(slot + o); };
    const auto set = [&](u32 o, s64 v) { wr32(slot + o, static_cast<u32>(v)); };
    if (first) set(sl::vBX, -t0(m(sl::width), 1));
    if (const s64 sc = rd32(h + fh::em_scale)) { // zero-extended
        s32 t = upem(face) - asc(face) + dsc(face);
        if (t < 0) t = 0;
        const s32 em = static_cast<s32>(mulsh(upem(face), sc, 16));
        const s32 half = static_cast<s32>(t0(static_cast<s32>(mulsh(sc, t, 16)), 1));
        const s32 a = static_cast<s32>(mulsh(sc, asc(face) + static_cast<s32>(t0(t, 1)), 16));
        const s32 vby = a - m(sl::hBY);
        const s32 v = vby + half + m(sl::height);
        set(sl::vBY, vby);
        set(sl::vAdv, v > em ? v : em);
    }
    if (const s32 w = rds32(h + fh::weight)) {
        s32 ex = 0, ey = 0, hx = 0, shift = 0;
        const u32 f = rd32(h + fh::face);
        const u32 size = f ? rd32(f + fc::size) : 0;
        if (size) {
            const s64 wi = t0(w, 16);
            ex = static_cast<s32>(t0(static_cast<s32>(mulsh(wi, rds32(size + sz::x_scale), 6)), 10));
            ey = static_cast<s32>(t0(static_cast<s32>(mulsh(rds32(size + sz::y_scale), wi, 6)), 10));
            hx = static_cast<s32>(t0(ex, 1));
            set(sl::hAdv, m(sl::hAdv) + ex);
            set(sl::adv_x, m(sl::adv_x) + ex);
            shift = hx;
            if (ex < 0) {
                ex = static_cast<s32>(t0(ex, 2));
                shift = static_cast<s32>(t0(ex, 1));
            }
        }
        if (m(sl::width)) {
            const s32 nw = m(sl::width) + ex;
            set(sl::width, nw < 0 ? 0 : nw);
        }
        set(sl::hBX, m(sl::hBX) - shift);
        set(sl::vBX, m(sl::vBX) - shift);
        if (ey < 0) ey = static_cast<s32>(t0(ey, 3));
        set(sl::vAdv, m(sl::vAdv) + ey);
        set(sl::adv_y, m(sl::adv_y) + ey);
        if (m(sl::height)) {
            const s32 nh = m(sl::height) + ey;
            set(sl::height, nh < 0 ? 0 : nh);
        }
        const s32 hy = static_cast<s32>(t0(ey, 1));
        set(sl::hBY, m(sl::hBY) + hy);
        set(sl::vBY, m(sl::vBY) - hy);
        if (image) ft("FT_Outline_Embolden", {slot + sl::outline, static_cast<u64>(static_cast<s64>(hx))});
    }
    const s32 s = rds32(h + fh::slant);
    if (s && m(sl::width)) {
        const s32 ht = m(sl::height);
        const s32 d = static_cast<s32>(mulsh(ht, s, 16));
        set(sl::width, s < 0 ? m(sl::width) - d : m(sl::width) + d);
        const s32 v = s > 0 ? ht - m(sl::hBY) : -m(sl::hBY);
        set(sl::hBX, m(sl::hBX) - static_cast<s32>(mulsh(v, s, 16)));
        set(sl::vBX, m(sl::vBX) - static_cast<s32>(mulsh(s, m(sl::vAdv), 17)));
    }
    return 0;
}

// FTFaceH_GetGlyphImage: the glyph of `code` (or the codes the handle's composite function gives for it) loaded
// unhinted, adjusted, and drawn into S[0] when there is one; S: {buffer, metrics out, x0, top, x1, bottom}.
s32 glyph_image(u32 h, u32 code, u32 S, s32 W, s32 H, s32 dx, s32 dy)
{
    if (!h || !rd32(h + fh::face)) return kErrFace;
    if (!S) return 0;
    const u32 face = rd32(h + fh::face);
    Local v(64); // bw, bx, bh, by; then the delta, the matrix, the bitmap
    const u32 bw = v.addr(), bx = v.addr() + 4, bh = v.addr() + 8, by = v.addr() + 12;
    const u32 delta = v.addr() + 16, matrix = v.addr() + 24, bitmap = v.addr() + 40;
    buffer_size(h, bw, bh, bx, by);
    const s32 fx = dx & 63, fy = dy & 63;
    wr32(delta, static_cast<u32>((rds32(bx) << 6) + fx));
    wr32(delta + 4, static_cast<u32>((rds32(by) << 6) - fy));
    wr32(matrix, 0x10000);
    wr32(matrix + 4, rd32(h + fh::slant));
    wr32(matrix + 8, 0);
    wr32(matrix + 12, 0x10000);
    const u32 buf = rd32(S);
    if (buf) {
        const s32 pitch = rds32(bw);
        const s32 cw = pitch > W ? W : pitch;
        const s32 ch = rds32(bh) > H ? H : rds32(bh);
        for (s32 row = 0; row < ch; row++)
            if (cw) std::memset(hp(buf + static_cast<u32>(row * pitch)), 0, static_cast<u32>(cw));
        wr32(bitmap + lay::bm_rows, static_cast<u32>(ch));
        wr32(bitmap + lay::bm_width, static_cast<u32>(cw));
        wr32(bitmap + lay::bm_pitch, static_cast<u32>(pitch));
        wr32(bitmap + lay::bm_buffer, buf);
        wr16(bitmap + lay::bm_num_grays, 256);
        wr8(bitmap + lay::bm_pixel_mode, FT_PIXEL_MODE_GRAY);
        ft("FT_Set_Transform", {face, matrix, delta});
    } else {
        ft("FT_Set_Transform", {face, matrix, 0});
    }
    if (const u32 arg = rd32(h + fh::composite_arg)) {
        call_guest_fn(rd32(h + fh::composite_fn), {arg, code, h + fh::codes});
    } else {
        wr32(h + fh::codes + 4, 0);
        wr32(h + fh::codes, code);
    }
    wr32(h + fh::code, code);
    const u32 M = h + fh::metrics;
    const auto load = [&](u32 c) -> s32 {
        const u32 gi = static_cast<u32>(ft("FT_Get_Char_Index", {face, c}));
        if (!gi) return -1;
        return fts("FT_Load_Glyph", {face, gi, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP});
    };
    const auto draw = [&](u32 slot) -> s32 {
        if (!buf) return 0;
        return fts("FT_Outline_Get_Bitmap", {rd32(h + fh::library), slot + sl::outline, bitmap});
    };
    if (const s32 e = load(rd32(h + fh::codes))) return e;
    u32 slot = rd32(face + fc::glyph);
    adjust_glyph(h, face, slot, true, buf != 0);
    std::memcpy(hp(M), hp(slot + sl::width), 32);
    if (const s32 e = draw(slot)) return e;
    const auto Mr = [&](u32 i) { return rds32(M + 4 * i); };
    const auto Mw = [&](u32 i, s32 x) { wr32(M + 4 * i, static_cast<u32>(x)); };
    if (rd32(h + fh::codes + 4)) {
        Mw(0, Mr(0) + rds32(slot + sl::hBX));
        Mw(1, Mr(1) - rds32(slot + sl::hBY));
        for (u32 k = 1;; k++) {
            const u32 c = rd32(h + fh::codes + 4 * k);
            if (!c) break;
            if (const s32 e = load(c)) return e;
            slot = rd32(face + fc::glyph);
            adjust_glyph(h, face, slot, false, buf != 0);
            const auto m = [&](u32 i) { return rds32(slot + sl::width + 4 * i); };
            if (Mr(0) < m(0) + m(2)) Mw(0, m(0) + m(2));
            if (Mr(1) < m(1) - m(3)) Mw(1, m(1) - m(3));
            if (Mr(2) > m(2)) Mw(2, m(2));
            if (Mr(3) < m(3)) Mw(3, m(3));
            if (Mr(4) < m(4)) Mw(4, m(4));
            if (Mr(7) < m(7)) Mw(7, m(7));
            if (Mr(6) > m(6)) Mw(6, m(6));
            if (const s32 e = draw(slot)) return e;
        }
        const s32 w = Mr(0) - Mr(2);
        Mw(1, Mr(1) + Mr(3));
        Mw(5, Mr(2) - static_cast<s32>(t0(w, 1)));
        Mw(0, w);
    }
    if (const u32 out = rd32(S + 4)) std::memcpy(hp(out), hp(M), 32);
    const s32 x0 = rds32(delta) - fx + Mr(2);
    wr32(S + 8, static_cast<u32>(x0 >> 6));
    wr32(S + 16, Mr(0) ? (static_cast<u32>(fx + Mr(0)) + static_cast<u32>(x0 + 63)) >> 6 : static_cast<u32>(x0 >> 6));
    const s32 top = rds32(bh) - static_cast<s32>(t0(rds32(delta + 4) + Mr(3) + (fy + 63), 6));
    wr32(S + 12, static_cast<u32>(top));
    wr32(S + 20, Mr(1) ? static_cast<u32>(rds32(bh) - static_cast<s32>(t0(Mr(3) - Mr(1) + rds32(delta + 4) - 63, 6)))
                       : static_cast<u32>(top));
    if (buf) {
        // libfontFT clears the transform's flags in FreeType's face internals (not FT_Set_Transform)
        if (FaceMirror* f = face_of(face)) static_cast<FT_Face>(const_cast<void*>(f->host))->internal->transform_flags = 0;
    }
    return 0;
}

s32 glyph_metrics(u32 h, u32 code, u32 m)
{
    if (!h || !rd32(h + fh::face)) return kErrFace;
    Local S(24);
    wr32(S.addr(), 0);
    wr32(S.addr() + 4, m);
    return glyph_image(h, code, S.addr(), 0, 0, 0, 0);
}

// ------------------------------------------------------------------------------------------------- vertical shift
// Where a glyph goes in vertical writing (libfontFT's 24A63186): the high byte of `k` picks the base (an em-centred
// slant correction), the low nibble of the low byte the horizontal placement, its high nibble the vertical one. The
// metrics (26.6) get the shift: vBX = hBX + dx, vBY += dy.
s32 vertical_shift(u32 h, s32 k, u32 m, u32 a_out, u32 b_out)
{
    if (!h) return kErrFace;
    const u32 face = rd32(h + fh::face);
    if (!face) return kErrFace;
    const s32 hi = k >> 8;
    const s32 s = rds32(h + fh::slant);
    s32 dx = 0;
    s32 em = static_cast<s32>(face); // Sony's register holds the face's address on the paths that do not compute it
    if (hi) {
        s64 A = 0, D = 0; // D stays 64-bit on the em-scale path, as in Sony's register
        if (const s64 sc = rd32(h + fh::em_scale)) { // zero-extended
            D = mulsh(dsc(face), sc, 16);
            s32 t = dsc(face) + (upem(face) - asc(face));
            if (t < 0) t = 0;
            A = static_cast<s32>(mulsh(asc(face) + static_cast<s32>(t0(t, 1)), sc, 16));
        } else if (const u32 size = rd32(face + fc::size)) {
            const s32 ys = rds32(size + sz::y_scale);
            D = static_cast<s32>(t0(static_cast<s32>(mulsh(dsc(face), ys, 6)), 10));
            A = static_cast<s32>(t0(static_cast<s32>(mulsh(asc(face), ys, 6)), 10));
        }
        const s32 lh = hi & 15;
        if (lh) {
            em = 0;
            if (const u32 size = rd32(face + fc::size))
                em = static_cast<s32>(t0(static_cast<s32>(mulsh(upem(face), rds32(size + sz::x_scale), 6)), 10));
            const s64 r = ((hi & 0x10) || s < 0) ? A : D;
            dx = -static_cast<s32>(mulsh(r, s, 16));
            if (lh == 2) dx -= static_cast<s32>(t0(em, 1));
            else if (lh == 3) dx -= em;
        } else {
            dx = ((hi & 0x10) && s > 0) ? -static_cast<s32>(mulsh(A, s, 16)) : 0;
        }
    }
    s32 dy = 0;
    const s32 lo = k & 0xFF;
    if (lo && m) {
        const auto M = [&](u32 i) { return rds32(m + 4 * i); };
        s32 r10 = dx - M(2);
        if (s) r10 += static_cast<s32>(mulsh(s > 0 ? M(1) - M(3) : M(3), s, 16));
        const s32 ln = lo & 15;
        const auto lean = [&](s32 v) { return s ? static_cast<s32>(mulsh(s > 0 ? v : -v, s, 16)) : 0; };
        if (ln == 2) {
            dx = r10 + M(5) + (s ? static_cast<s32>(mulsh(s > 0 ? M(1) : -M(1), s, 17)) : 0);
        } else if (ln == 3) {
            const s32 d = em - (M(0) - lean(M(1)));
            dx = r10 + d - static_cast<s32>(t0(d, 3));
        } else if (ln == 4) {
            const s32 d = em - (M(0) - lean(M(1)));
            dx = r10 + d - d / 9;
        } else if (ln == 6) {
            dx = dx + static_cast<s32>(t0(-M(4), 1)) +
                (s ? static_cast<s32>(mulsh(s > 0 ? M(3) - M(1) : -M(3), s, 16)) : 0);
        } else {
            dx = r10;
        }
        if (const s32 hn = lo >> 4) {
            s32 e = -M(6);
            if (hn == 3) e += M(7) - M(1);
            else if (hn == 4) e += M(1) / 3;
            else if (hn == 2) e += static_cast<s32>(t0(M(7) - M(1), 1));
            dy = e;
            dx -= static_cast<s32>(mulsh(e, s, 16));
        }
    }
    if (a_out) wr32(a_out, static_cast<u32>(dx));
    if (b_out) wr32(b_out, static_cast<u32>(dy));
    if (m) {
        wr32(m + 24, static_cast<u32>(rds32(m + 24) + dy));
        wr32(m + 20, static_cast<u32>(rds32(m + 8) + dx));
    }
    return 0;
}

} // namespace fontft

namespace fontft {

// ------------------------------------------------------------------------------------------------- renderers
// libfontFT's renderer (128 bytes, FT_Alloc'd; a CellFontRenderer's +68): its face handle, memory, glyph buffer and
// the last image's place in it ({buffer, metrics, x0, top, x1, bottom}, GetGlyphImage's S), the buffer's width and
// rows, its initial and maximum sizes and its size.
namespace rd {
constexpr u32 handle = 0, memory = 4, image = 8, buffer = 8, metrics = 12, x0 = 16, top = 20, x1 = 24, bottom = 28,
              width = 32, height = 36, init = 40, max = 44, size = 48;
}
// The CellFontRenderer libfont passes: +64 the CellFontLibrary, +68 the renderer, +72 the outline buffer of the
// vertexes glyph renderer, +128 its sizing record (X), +180 the image's dimensions (O[6]).
namespace rr {
constexpr u32 library = 64, renderer = 68, outline = 72, x = 128, o = 180;
}

s32 renderer_init(u32 r, s32 init, s32 max, u32 mem)
{
    if (!r || !mem) return kErrArg;
    Local p(8);
    wr32(p.addr(), 0);
    if (init) {
        const s32 e = ft_alloc(mem, init, p.addr());
        if (!p.get()) return e;
    }
    wr32(r + rd::height, 0);
    wr32(r + rd::buffer, p.get());
    wr32(r + rd::memory, mem);
    wr32(r + rd::size, static_cast<u32>(init));
    wr32(r + rd::max, static_cast<u32>(max));
    wr32(r + rd::handle, 0);
    wr32(r + rd::init, static_cast<u32>(init));
    wr32(r + rd::width, 0);
    return 0;
}

s32 renderer_done(u32 r)
{
    if (!r || !rd32(r + rd::memory)) return kErrArg;
    if (rd32(r + rd::buffer)) ft_free(rd32(r + rd::memory), r + rd::buffer);
    for (const u32 o : {rd::height, rd::handle, rd::memory, rd::size, rd::max, rd::width}) wr32(r + o, 0);
    return 0;
}

// Grows the buffer to `n` bytes (a new block; the maximum, when set, answers 10 with the rows that fit).
s32 renderer_reserve(u32 r, s32 n, u32 width, u32 height)
{
    const u32 buf = rd32(r + rd::buffer);
    if (buf && rds32(r + rd::size) >= n) {
        wr32(r + rd::width, width);
        wr32(r + rd::height, height);
        return 0;
    }
    const s32 max = rds32(r + rd::max);
    if (max && n > max) {
        const s32 w = rds32(r + rd::width);
        wr32(r + rd::height, w ? static_cast<u32>(rds32(r + rd::size) / w) : 0);
        return 10;
    }
    const u32 mem = rd32(r + rd::memory);
    if (!mem) return kErrArg;
    if (buf) ft_free(mem, r + rd::buffer);
    if (const s32 e = ft_alloc(mem, n, r + rd::buffer)) {
        wr32(r + rd::size, 0);
        wr32(r + rd::width, 0);
        wr32(r + rd::height, 0);
        return e;
    }
    wr32(r + rd::size, static_cast<u32>(n));
    wr32(r + rd::width, width);
    wr32(r + rd::height, height);
    return 0;
}

// libfontFT's 9F0A5F5D: the face handle scaled (FTFaceH_SetRenderScalePoint, its answer ignored) and the buffer
// made big enough for its glyphs.
s32 renderer_scale(u32 r, u32 h, s32 cw, s32 ch, u32 dx, u32 dy);

// The image's place clipped to the surface's scissor: CellFontImageTransInfo {image, imageWidthByte, imageWidth,
// imageHeight, surface, surfWidthByte}. S: CellFontRenderSurface {buffer, widthByte, pixelSizeByte, width, height,
// sc_x0, sc_y0, sc_x1, sc_y1}.
void clip(u32 r, u32 S, s32 px, s32 py, u32 T)
{
    s32 gw = rds32(r + rd::x1) - rds32(r + rd::x0);
    s32 gh = rds32(r + rd::bottom) - rds32(r + rd::top);
    if (gw > rds32(r + rd::width)) gw = rds32(r + rd::width);
    if (gh > rds32(r + rd::height)) gh = rds32(r + rd::height);
    if (px + gw > rds32(S + 28)) gw = std::max(rds32(S + 28) - px, 0);
    s32 sx = 0;
    if (px < rds32(S + 20)) {
        sx = rds32(S + 20) - px;
        gw = std::max(gw - sx, 0);
        px = rds32(S + 20);
    }
    if (py + gh > rds32(S + 32)) gh = std::max(rds32(S + 32) - py, 0);
    s32 sy = 0;
    if (py < rds32(S + 24)) {
        sy = rds32(S + 24) - py;
        gh = std::max(gh - sy, 0);
        py = rds32(S + 24);
    }
    if (!T) {
        PS_LOG_ERROR(kChan, "a glyph image with no CellFontImageTransInfo (Sony's libfontFT writes through it): skipped");
        return;
    }
    wr32(T + 16, rd32(S) + static_cast<u32>(px * rds32(S + 8) + py * rds32(S + 4)));
    wr32(T + 8, static_cast<u32>(gw));
    wr32(T + 12, static_cast<u32>(gh));
    wr32(T + 0, static_cast<u32>((sy + rds32(r + rd::top)) * rds32(r + rd::width)) + rd32(r + rd::buffer) +
            rd32(r + rd::x0) + static_cast<u32>(sx));
    wr32(T + 4, rd32(r + rd::width));
    wr32(T + 20, rd32(S + 4));
}

// libfontFT's 869c: a character's glyph drawn into the renderer's buffer at the 26.6 position (its fraction kept), the
// image's place in the surface given back.
s32 render(u32 r, u32 code, u32 S, s32 x, s32 y, u32 m, u32 T)
{
    if (!r || !S || !m) return kErrArg;
    wr32(r + rd::metrics, m);
    const u32 h = rd32(r + rd::handle);
    const s32 a = scaled(h, true, 6, [](u32 f) { return s64{rds32(f + fc::yMax)}; });
    const s32 d = weight_scaled(h, true);
    const s32 fy = (y + static_cast<s32>(t0(s64{a} + t0(d, 1), 10))) & 63;
    if (const s32 e = glyph_image(h, code, r + rd::image, rds32(r + rd::width), rds32(r + rd::height), x & 63, fy))
        return e;
    const s32 px = (x >> 6) + (rds32(m + 8) >> 6);
    const s32 py = ((y - fy) >> 6) + rds32(r + rd::top);
    clip(r, S, px, py, T);
    return 0;
}

// libfontFT's A91EA22B: the same with a vertical-writing shift (k, as 24A63186 takes it).
s32 render_shifted(u32 r, u32 code, u32 S, s32 x, s32 y, u32 m, u32 T, s32 k)
{
    if (!r || !rd32(r + rd::handle)) return kErrArg;
    const u32 h = rd32(r + rd::handle);
    Local ab(8);
    bool post = false;
    if ((k & 0xFF) == 0) {
        vertical_shift(h, k, 0, ab.addr(), ab.addr() + 4);
        post = m != 0;
    } else {
        if (const s32 e = glyph_metrics(h, code, m)) return e;
        vertical_shift(h, k, m, ab.addr(), ab.addr() + 4);
    }
    const s32 a = rds32(ab.addr()), b = rds32(ab.addr() + 4);
    const s32 X = x + a, Y = y + b;
    const s32 asc_y = scaled(h, true, 6, [](u32 f) { return s64{rds32(f + fc::yMax)}; });
    const s32 d = weight_scaled(h, true);
    const s32 base = Y - static_cast<s32>(t0(s64{asc_y} + t0(d, 1), 10));
    const s32 y2 = base + (rd32(h + fh::em_scale) ? em_center(h)
                                                   : static_cast<s32>(t0(scaled(h, true, 6, [](u32 f) {
                                                         return s64{asc(f)};
                                                     }), 10)));
    const s32 e = render(r, code, S, X, y2, m, T);
    if (m) {
        if (post) wr32(m + 20, static_cast<u32>(rds32(m + 8) + a));
        wr32(m + 24, static_cast<u32>(rds32(m + 24) + b));
    }
    return e;
}

// libfontFT's 42A0789F: the sizing of a vertexes glyph's image from its record X {xMin, yMin, xMax, yMax, upem,
// ascender, descender, max advance, x scale, y scale, weight, slant, em scale}: O = {width (4-byte rounded), rows,
// left margin, bottom margin, bytes, top}.
s32 vertexes_size(u32 X, u32 O)
{
    if (!O) return -1;
    if (!X) {
        for (const u32 i : {5u, 1u, 0u, 3u, 2u}) wr32(O + 4 * i, 0);
        return 0;
    }
    const Box b = effect_box(rds32(X + 0), rds32(X + 4), rds32(X + 8), rds32(X + 12), rds32(X + 32), rds32(X + 36),
        rds32(X + 40), rds32(X + 44));
    const s64 top = up(b.top), bot = up(b.bot), left = up(b.left);
    const s32 wd = static_cast<s32>(left + up(b.right)) + 12;
    const s32 wa = static_cast<s32>(t0(wd, 2)) * 4;
    const s32 hh = static_cast<s32>(bot + top);
    wr32(O + 20, static_cast<u32>(top));
    wr32(O + 12, static_cast<u32>(bot + 5));
    wr32(O + 8, static_cast<u32>(left + 4));
    wr32(O + 4, static_cast<u32>(hh + 5));
    wr32(O + 0, static_cast<u32>(wa));
    const s32 n = static_cast<s32>(static_cast<u32>(hh + 9) * static_cast<u32>(wa));
    wr32(O + 16, static_cast<u32>(n));
    return n;
}

// libfontFT's 70B52CD6: X's em centre line.
s32 vertexes_center(u32 X)
{
    const s32 t = rds32(X + 16) - rds32(X + 20) + rds32(X + 24);
    const s32 v = t >= 0 ? rds32(X + 20) + static_cast<s32>(t0(t, 1)) : rds32(X + 20);
    return static_cast<s32>(mulsh(v, rds32(X + 48), 16));
}

// libfontFT's 40DCBC8D: an outline drawn into the renderer's image buffer (cleared first), its place computed from the
// 26.6 metrics A and the dimensions P.
s32 outline_image(u32 library, u32 outline, u32 A, u32 P, u32 S, s32 dx, s32 dy)
{
    if (!S) return 0;
    const u32 buf = rd32(S);
    const s32 W = rds32(P), R = rds32(P + 4), P2 = rds32(P + 8), P3 = rds32(P + 12);
    if (buf) {
        for (s32 row = 0; row < R; row++)
            if (W) std::memset(hp(buf + static_cast<u32>(row * W)), 0, static_cast<u32>(W));
        Local bm(lay::bm_rec);
        wr32(bm.addr() + lay::bm_rows, static_cast<u32>(R));
        wr32(bm.addr() + lay::bm_width, static_cast<u32>(W));
        wr32(bm.addr() + lay::bm_pitch, static_cast<u32>(W));
        wr32(bm.addr() + lay::bm_buffer, buf);
        wr16(bm.addr() + lay::bm_num_grays, 256);
        wr8(bm.addr() + lay::bm_pixel_mode, FT_PIXEL_MODE_GRAY);
        if (const s32 e = fts("FT_Outline_Get_Bitmap", {library, outline, bm.addr()})) return e;
    }
    const s32 x0 = (P2 << 6) + rds32(A + 8);
    wr32(S + 8, static_cast<u32>(x0 >> 6));
    wr32(S + 16, rds32(A) ? (static_cast<u32>((dx & 63) + rds32(A)) + static_cast<u32>(x0 + 63)) >> 6
                          : static_cast<u32>(x0 >> 6));
    const s32 base = (P3 << 6) - (dy & 63);
    const s32 top = R - static_cast<s32>(t0((dy & 63) + rds32(A + 12) + base + 63, 6));
    wr32(S + 12, static_cast<u32>(top));
    wr32(S + 20, rds32(A + 4) ? static_cast<u32>(R - static_cast<s32>(t0(rds32(A + 12) - rds32(A + 4) + base - 63, 6)))
                              : static_cast<u32>(top));
    return 0;
}

// libfontFT's BC7553F5: a vertexes glyph's outline drawn and clipped as `render` does a character's.
s32 render_outline(u32 r, u32 O, u32 library, u32 outline, u32 M, u32 S, s32 x, s32 y, u32 T)
{
    if (!r || !S || !M) return kErrArg;
    wr32(r + rd::metrics, M);
    const s32 fy = (rds32(O + 20) + y) & 63;
    if (const s32 e = outline_image(library, outline, M, O, r + rd::image, x & 63, fy)) return e;
    const s32 px = (x >> 6) + (rds32(M + 8) >> 6);
    const s32 py = ((y - fy) >> 6) + rds32(r + rd::top);
    clip(r, S, px, py, T);
    return 0;
}

s32 renderer_scale(u32 r, u32 h, s32 cw, s32 ch, u32 dx, u32 dy)
{
    if (!r || !h) return kErrArg;
    wr32(r + rd::handle, h);
    Local v(8);
    {
        // FTFaceH_SetRenderScalePoint
        const u32 face = rd32(h + fh::face);
        const s32 e = fts("FT_Set_Char_Size", {face, static_cast<u64>(static_cast<s64>(cw)),
                                                  static_cast<u64>(static_cast<s64>(ch)), dx, dy});
        if (e) {
            wr32(h + fh::dpi_y, 72);
            wr32(h + fh::dpi_x, 72);
        } else {
            wr32(h + fh::dpi_x, static_cast<s32>(dx) > 0 ? dx : 72);
            wr32(h + fh::dpi_y, static_cast<s32>(dy) > 0 ? dy : 72);
        }
    }
    const s32 n = buffer_size(h, r + rd::width, r + rd::height, 0, 0);
    const u32 buf = rd32(r + rd::buffer);
    if (buf && rds32(r + rd::size) >= n) return 0;
    const s32 max = rds32(r + rd::max);
    if (max && n > max) {
        const s32 w = rds32(r + rd::width);
        wr32(r + rd::height, w ? static_cast<u32>(rds32(r + rd::size) / w) : 0);
        return 10;
    }
    const u32 mem = rd32(r + rd::memory);
    if (!mem) return kErrArg;
    if (buf) ft_free(mem, r + rd::buffer);
    if (const s32 e = ft_alloc(mem, n, r + rd::buffer)) {
        wr32(r + rd::size, 0);
        wr32(r + rd::width, 0);
        wr32(r + rd::height, 0);
        return e;
    }
    wr32(r + rd::size, static_cast<u32>(n));
    return 0;
}

// ------------------------------------------------------------------------------------------------- vertexes glyphs
// libfontFT's 75c: a CellFontGlyph made from a loaded outline (26.6 to float pixels, moved by `off`) with its metrics,
// the face's em box in its generate environment.
u32 make_glyph(u32 h, u32 o, u32 m, u32 off, u32 k)
{
    const float ox = rdf(off), oy = rdf(off + 4);
    const u32 mem = memory();
    if (!mem) return 0;
    const s32 np = rds16(o + lay::ol_n_points), nc = rds16(o + lay::ol_n_contours);
    const s32 tags_size = (np + 1) & ~1;
    const s32 size = 192 + np * 8 + tags_size + nc * 2;
    Local out(8);
    if (ft_alloc(mem, size, out.addr())) return 0;
    const u32 p = out.get();
    wr32(p + 4, static_cast<u32>(size));
    const u32 q = p + 64;
    wr32(p + 60, q);
    const u32 face = rd32(h + fh::face);
    wrf(q + 20, i2f(upem(face)));
    wrf(q + 24, i2f(asc(face)));
    wrf(q + 28, i2f(dsc(face)));
    wrf(q + 32, i2f(rds16(face + fc::max_adv_w)));
    wrf(q + 44, i2f(rds32(face + fc::xMin)));
    wrf(q + 48, i2f(rds32(face + fc::yMin)));
    wrf(q + 36, i2f(rds32(face + fc::xMax)));
    wrf(q + 40, i2f(rds32(face + fc::yMax)));
    wr32(q + 52, k);
    wr32(q + 56, 0);
    wr32(q + 60, 0);
    wrf(q + 64, i2f(rd32(h + fh::em_scale)) * (1.0f / 65536.0f)); // Sony's converts the word unsigned
    wr32(q + 72, 0);
    wrf(q + 76, i2f(em_center(h)) * 0.015625f);
    const u32 pts = p + 192, tags = pts + static_cast<u32>(np) * 8, contours = tags + static_cast<u32>(tags_size);
    wr16(p + 42, static_cast<u32>(np));
    wr32(p + 44, pts);
    wr32(p + 48, tags);
    const u32 src_pts = rd32(o + lay::ol_points), src_tags = rd32(o + lay::ol_tags);
    for (s32 i = 0; i < np; i++) {
        wrf(pts + 8 * i, std::fma(i2f(rds32(src_pts + 8 * i)), 0.015625f, ox));
        wrf(pts + 8 * i + 4, std::fma(i2f(rds32(src_pts + 8 * i + 4)), 0.015625f, oy));
        wr8(tags + i, rd8(src_tags + i));
    }
    wr16(p + 40, static_cast<u32>(nc));
    wr32(p + 52, contours);
    const u32 src_contours = rd32(o + lay::ol_contours);
    for (s32 i = 0; i < nc; i++) wr16(contours + 2 * i, rd16(src_contours + 2 * i));
    wr32(p + 56, rd32(o + lay::ol_flags));
    const auto M = [&](u32 i) { return i2f(rds32(m + 4 * i)); };
    wrf(p + 8, M(0) * 0.015625f);
    wrf(p + 12, M(1) * 0.015625f);
    wrf(p + 16, std::fma(M(2), 0.015625f, ox));
    wrf(p + 20, std::fma(M(3), 0.015625f, oy));
    wrf(p + 24, M(4) * 0.015625f);
    wrf(p + 28, std::fma(M(5), 0.015625f, ox));
    wrf(p + 32, std::fma(M(6), 0.015625f, oy));
    wrf(p + 36, M(7) * 0.015625f);
    wr16(p, 0xCF20);
    wr16(p + 2, 1);
    return p;
}

// Table +108: a CellFontGlyph (vertexes glyph) drawn with a style {scale x, scale y, weight, slant} at (x, y), through
// the renderer's outline buffer (the outline scaled, slanted and emboldened as the style asks).
s32 render_vertexes(u32 RR, u32 G, u32 st, u32 S, float x, float y, u32 outM, u32 T, s32 k)
{
    const u32 r = rd32(RR + rr::renderer);
    if (!r) return static_cast<s32>(E(0x21));
    const u32 env = rd32(G + 60);
    const auto e = [&](u32 off) { return rdf(env + off); };
    const float st0 = rdf(st), st4 = rdf(st + 4), st8 = rdf(st + 8), st12 = rdf(st + 12);
    s32 vk = 0;
    if (k < 0 && rd32(env + 52)) {
        vk = rds32(env + 52);
        const float sy = st4 / e(4);
        float c = e(24);
        const float t = e(20) - e(24) + e(28);
        if (t > 0.0f) c = std::fma(t, 0.5f, e(24));
        const float d6072 = e(60) - e(72);
        const float f3 = sy * d6072;
        const float f30 = sy * e(72);
        const float sx = st0 / e(0);
        const s32 rc = fctiwz(c);
        const s32 a = fctiwz(d6072 * 64.0f);
        const s32 b = fctiwz(e(12) * 65536.0f);
        const float f8 = std::fma(e(12) * e(72), e(0) / e(4), 0.0f);
        y = (y + f3) + f30;
        const s64 t1 = t0(s64{a} * b, 16);
        const float u = std::fma(i2f(t1), 0.015625f, e(56));
        const s64 q1 = fctiwz((sx * u) * 64.0f);
        const s64 t2 = t0(s64{fctiwz(f3 * 64.0f)} * fctiwz(st12 * 65536.0f), 16);
        float f12 = std::fma(i2f(q1 - t2), 0.015625f, x);
        const float e64 = e(64) * 65536.0f;
        const s32 A = fctiwz(sx * e64), B = fctiwz(sy * e64);
        const s32 r0 = static_cast<s32>(t0(static_cast<s32>(static_cast<u32>(rc) * static_cast<u32>(B)), 16));
        const s32 r9 = static_cast<s32>(t0(static_cast<s32>(static_cast<u32>(rc) * static_cast<u32>(A)), 16));
        const float f0 = std::fma(sx, f8, -((st12 * f30) * (st0 / st4)));
        f12 = f12 + f0;
        x = std::fma(e(12), i2f(r9) * 0.015625f, f12) - st12 * (i2f(r0) * 0.015625f);
    } else if (k < 0) {
        vk = 2;
    } else {
        const float f7 = st0 / st4;
        const float f8 = e(0) / e(4);
        const float f10 = st0 / e(0);
        float f11 = e(72) * e(12);
        const float f12 = (st4 / e(4)) * e(72);
        f11 = std::fma(f11, f8, 0.0f);
        const float f0 = (f12 * st12) * f7;
        y = y + f12;
        x = x + std::fma(f10, f11, -f0);
    }
    float f31;
    s32 s16;
    if (st12 > 1.0f) {
        f31 = 1.0f;
        s16 = 0x10000;
    } else if (st12 < -1.0f) {
        f31 = -1.0f;
        s16 = -0x10000;
    } else {
        f31 = st12;
        s16 = fctiwz(st12 * 65536.0f);
    }
    // the weight clamped to [0.93, 1.04] (libfontFT's constants, bit for bit: (0.93f - 1) * 65536 and (1.04f - 1) * 65536)
    float W;
    if (st8 < std::bit_cast<float>(0x3F6E147Bu)) W = std::bit_cast<float>(0xC58F5C28u);
    else if (st8 > std::bit_cast<float>(0x3F851EB8u)) W = std::bit_cast<float>(0x4523D700u);
    else W = (st8 - 1.0f) * 65536.0f;
    const u32 X = RR + rr::x, O = RR + rr::o;
    wr32(X + 16, static_cast<u32>(fctiwz(e(20))));
    wr32(X + 20, static_cast<u32>(fctiwz(e(24))));
    wr32(X + 24, static_cast<u32>(fctiwz(e(28))));
    wr32(X + 28, static_cast<u32>(fctiwz(e(32))));
    wr32(X + 0, static_cast<u32>(fctiwz(e(44))));
    wr32(X + 8, static_cast<u32>(fctiwz(e(36))));
    wr32(X + 4, static_cast<u32>(fctiwz(e(48))));
    wr32(X + 12, static_cast<u32>(fctiwz(e(40))));
    wr32(X + 32, static_cast<u32>(fctidz(i2f(fctidz(((e(68) * st0) * 64.0f) * 65536.0f)) / e(20))));
    wr32(X + 36, static_cast<u32>(fctidz(i2f(fctidz(((e(68) * st4) * 64.0f) * 65536.0f)) / e(20))));
    wr32(X + 48, static_cast<u32>(fctiwz(((st4 * 65536.0f) / e(4)) * e(64))));
    wr32(X + 44, static_cast<u32>(s16));
    wr32(X + 40, static_cast<u32>(fctiwz(W * e(20))));
    vertexes_size(X, O);
    if (const s32 er = renderer_reserve(r, rds32(O + 16), rd32(O), rd32(O + 4)))
        return static_cast<s32>(er == 10 ? E(0x24) : E(0x23));
    // the top line (the y scale and the weight's widening, in 26.6), zero-extended scales as Sony's
    const s64 ysu = rd32(X + 36);
    const s32 d = static_cast<s32>(t0(t0(s64{rds32(X + 12)} * ysu, 6) + t0(ysu * t0(rds32(X + 40), 16), 7), 10));
    wr32(O + 20, static_cast<u32>(d));
    s32 base = fctiwz(y * 64.0f) - d;
    if (vk) base += rd32(X + 48) ? vertexes_center(X) : static_cast<s32>(t0(s64{rds32(X + 20)} * ysu, 16));
    const u32 mem = memory_of(rd32(RR + rr::library) + lb::manager);
    if (!mem) return static_cast<s32>(E(3));
    const s32 np = rds16(G + 42), nc = rds16(G + 40);
    const u32 tags_size = static_cast<u32>(np + 1) & ~1u;
    const s32 need = nc * 2 + static_cast<s32>(tags_size) + np * 8 + 128;
    u32 ob = rd32(RR + rr::outline);
    if (!ob || rds32(ob) < need) {
        if (ob) {
            ft_free(mem, RR + rr::outline);
            wr32(RR + rr::outline, 0);
        }
        if (ft_alloc(mem, need, RR + rr::outline)) return static_cast<s32>(E(0x23));
        ob = rd32(RR + rr::outline);
        wr32(ob, static_cast<u32>(need));
    }
    const u32 outline = ob + 4, pts = ob + 128, tags = pts + static_cast<u32>(np) * 8, contours = tags + tags_size;
    wr32(outline + lay::ol_points, pts);
    wr32(outline + lay::ol_contours, contours);
    wr32(outline + lay::ol_tags, tags);
    wr16(outline + lay::ol_n_points, static_cast<u32>(np));
    wr16(outline + lay::ol_n_contours, static_cast<u32>(nc));
    wr32(outline + lay::ol_flags, rd32(G + 56));
    const u32 M2 = ob + 24;
    const auto g = [&](u32 off) { return rdf(G + off); };
    const float gw = g(8), gh = g(12), ghbx = g(16), ghby = g(20), ghadv = g(24), gvbx = g(28), gvby = g(32),
                gvadv = g(36);
    const float f4 = (st4 / e(4)) * 64.0f, f3 = (st0 / e(0)) * 64.0f;
    const float hadv = f3 * ghadv;
    wr32(M2 + 24, static_cast<u32>(fctiwz(f4 * gvby)));
    wr32(M2 + 16, static_cast<u32>(fctiwz(hadv)));
    const float ratio = (st0 / e(0)) / (st4 / e(4));
    const float f29 = f31 - e(12) * ratio;
    const float e12 = e(12);
    float H, VA, HBY, Wd, HBX, VBX;
    if (f31 == e12 && f3 == f4) {
        H = f4 * gh;
        VA = f4 * gvadv;
        HBY = f4 * ghby;
        Wd = f3 * gw;
        HBX = f3 * ghbx;
        VBX = f3 * gvbx;
    } else {
        const float hmb = gh - ghby;
        const float w0 = e12 > 0.0f ? gw - e12 * gh : std::fma(e12, gh, gw);
        H = f4 * gh;
        const float w0s = w0 * f3;
        Wd = f31 > 0.0f ? std::fma(f31, H, w0s) : w0s - f31 * H;
        const float b0 = e12 > 0.0f ? std::fma(e12, hmb, ghbx) : ghbx - e12 * ghby;
        HBY = f4 * ghby;
        HBX = b0 * f3;
        HBX = f31 > 0.0f ? HBX - f31 * (f4 * hmb) : std::fma(f31, HBY, HBX);
        VA = f4 * gvadv;
        const float v0 = std::fma(e12, gvadv * 0.5f, gvbx);
        VBX = std::fma(f3, v0, f31 * (VA * -0.5f));
    }
    const auto M = [&](u32 i) { return rds32(M2 + 4 * i); };
    const auto Ms = [&](u32 i, s64 v) { wr32(M2 + 4 * i, static_cast<u32>(v)); };
    Ms(3, fctiwz(HBY));
    Ms(1, fctiwz(H));
    Ms(7, fctiwz(VA));
    Ms(0, fctiwz(Wd));
    Ms(2, fctiwz(HBX));
    Ms(5, fctiwz(VBX));
    s32 hx = 0;
    if (const s32 wt = rds32(X + 40)) {
        const s64 wi = t0(wt, 16);
        s32 ey = static_cast<s32>(t0(wi * s64{rd32(X + 36)}, 16));
        s32 ex = static_cast<s32>(t0(wi * s64{rd32(X + 32)}, 16));
        hx = static_cast<s32>(t0(ex, 1));
        Ms(4, M(4) + ex);
        s32 shift = hx;
        if (ex < 0) {
            ex = static_cast<s32>(t0(ex, 2));
            shift = static_cast<s32>(t0(ex, 1));
        }
        if (!M(0) || M(0) + ex <= 0) {
            Ms(0, 0);
        } else {
            Ms(0, M(0) + ex);
            if (f31 != e12) {
                // Sony's adds these whole, not as differences
                const float f0 = f31 * i2f(ey);
                const float half = f0 * 0.5f;
                Ms(4, fctiwz(hadv + f0));
                Ms(0, M(0) + fctiwz(Wd + f0));
                Ms(2, M(2) - fctiwz(HBX - half));
                Ms(5, M(5) - fctiwz(VBX - half));
            }
        }
        Ms(2, M(2) - shift);
        Ms(5, M(5) - shift);
        if (ey < 0) ey = static_cast<s32>(t0(ey, 3));
        Ms(7, M(7) + ey);
        if (M(1)) Ms(1, std::max(M(1) + ey, 0));
        const s32 hy = static_cast<s32>(t0(ey, 1));
        Ms(3, M(3) + hy);
        Ms(6, M(6) - hy);
    }
    const s32 x64 = fctiwz(x * 64.0f);
    const u32 gpts = rd32(G + 44), gtags = rd32(G + 48), gcont = rd32(G + 52);
    const s32 ox = rds32(O + 8) * 64, oy = rds32(O + 12) * 64;
    const s32 fy = (base + d) & 63;
    for (s32 i = 0; i < np; i++) {
        const float px = rdf(gpts + 8 * i), py = rdf(gpts + 8 * i + 4);
        wr8(tags + i, rd8(gtags + i));
        const float ys = f4 * py;
        const float xs = std::fma(f3, px, f29 * ys);
        wr32(pts + 8 * i, static_cast<u32>(ox + fctiwz(xs) + (x64 & 63)));
        wr32(pts + 8 * i + 4, static_cast<u32>(oy + fctiwz(ys) - fy));
    }
    for (s32 i = 0; i < nc; i++) wr16(contours + 2 * i, rd16(gcont + 2 * i));
    if (hx) ft("FT_Outline_Embolden", {outline, static_cast<u64>(static_cast<s64>(hx))});
    const u32 L = rd32(RR + rr::library);
    Local scopy(36), t(24);
    std::memcpy(hp(scopy.addr()), hp(S), 36);
    lock_lib(L + lb::lock);
    const u32 ftlib = rd32(L + lb::manager);
    const s32 er = render_outline(r, O, ftlib, outline, M2, scopy.addr(), x64, base, t.addr());
    unlock_lib(L + lb::lock);
    if (er) {
        wr32(T + 20, rd32(scopy.addr() + 4));
        for (const u32 i : {3u, 0u, 1u, 2u}) wr32(T + 4 * i, 0);
        wr32(T + 16, rd32(scopy.addr()) + static_cast<u32>((fctiwz(y) * rds32(scopy.addr() + 4) + fctiwz(x)) *
                                                           rds32(scopy.addr() + 8)));
        return static_cast<s32>(E(0x12));
    }
    std::memcpy(hp(T), hp(t.addr()), 24);
    if (outM)
        for (u32 i = 0; i < 8; i++) wrf(outM + 4 * i, i2f(M(i)) * 0.015625f);
    return 0;
}

} // namespace fontft

namespace {

using namespace fontft;

// ------------------------------------------------------------------------------------------------- the table
// What libfont calls through the CellFontLibrary (its +16 .. +148), in libfontFT's terms. Errors become CellFont's.

s32 e1(s32 r) { return r ? static_cast<s32>(E(1)) : 0; }
s32 e2(s32 r) { return r ? static_cast<s32>(E(2)) : 0; }
s32 eoom(s32 r) { return !r ? 0 : static_cast<s32>(r == kErrOom ? E(7) : E(1)); }
s32 egm(s32 r) { return !r ? 0 : static_cast<s32>(r == kErrOom ? E(7) : E(0x12)); }
s32 ers(s32 r) { return !r ? 0 : static_cast<s32>(r == 10 ? E(0x24) : r == kErrOom ? E(0x23) : E(2)); }

void floats_out(u32 out, u32 m, u32 n)
{
    for (u32 i = 0; i < n; i++) wrf(out + 4 * i, i2f(rds32(m + 4 * i)) * 0.015625f);
}

// +16: the library's last user gone, its FreeType library done.
s32 pspocket_fontft_end_library(PPUContext& ctx)
{
    Scope sc(ctx);
    const u32 st = static_cast<u32>(font("cellFontStatic", {}));
    if (!st) return static_cast<s32>(E(3));
    const u32 g = lib();
    lock_lib(g + lb::lock);
    const u32 v = atomic_load_be32(gbase(), g + lb::lock) & ~lb::kLocked;
    if (!(v & lb::kInitialized)) {
        unlock_lib(g + lb::lock);
        return static_cast<s32>(E(3));
    }
    if ((v & lb::kCount) != 1) {
        count_down(g + lb::lock);
        unlock_lib(g + lb::lock);
        return 0;
    }
    if (done_freetype(g + lb::manager)) {
        unlock_lib(g + lb::lock);
        return static_cast<s32>(E(1));
    }
    for (u8* base = gbase();;) {
        const u32 w = atomic_load_be32(base, g + lb::lock);
        if (atomic_cas_be32(base, g + lb::lock, w, w & ~lb::kInitialized) == w) break;
    }
    if (!old_sdk()) wr32(st + 64, 0);
    count_down(g + lb::lock);
    unlock_lib(g + lb::lock);
    return 0;
}

// +20: a font file opened: through the file cache (mode 2) and/or as a file (mode 4).
s32 pspocket_fontft_open_file(PPUContext& ctx, u32 path, u32 mode, s32 index, u32 id, u32 out)
{
    Scope sc(ctx);
    const u32 st = static_cast<u32>(font("cellFontStatic", {}));
    wr32(out, 0);
    if (!st || !rd32(st + 64)) return static_cast<s32>(E(3));
    const u32 l = rd32(st + 64);
    const u32 mgr = l + lb::manager;
    const u32 mem = memory_of(mgr);
    Local fh(8);
    s32 r = ft_alloc(mem, fh::size, fh.addr());
    if (r) return static_cast<s32>(E(7));
    if (mode & 2) {
        Local cs(cs::size), cs2(8);
        r = cache_stream_init(cs.addr(), path, id);
        if (!r) {
            u32 stream = 0;
            if (const u32 n = index_size(rd32(cs.addr() + lay::st_size))) {
                r = ft_alloc(memory_of(mgr), n + 128, cs2.addr());
                if (!r) {
                    stream = cs2.get();
                    std::memcpy(hp(stream), hp(cs.addr()), cs::size);
                    cache_stream_cache_init(stream, stream + 128, rd32(st + 20));
                    wr32(stream + cs::lock, l + lb::lock_if);
                }
            } else {
                PS_LOG_ERROR(kChan, "font file {} is empty: Sony's libfontFT opens an unset stream here", gstr(path));
            }
            if (!r) {
                r = open_stream_face(mgr, fh.get(), stream, index);
                if (r && stream) ft_free(mem, cs2.addr());
            }
        }
    }
    if (mode & 4) r = open_file_face(mgr, fh.get(), path, index);
    if (r) {
        ft_free(mem, fh.addr());
        return static_cast<s32>(r == kErrOom ? E(7) : E(0xb));
    }
    wr32(out, fh.get());
    return 0;
}

// +24: a font in memory.
s32 pspocket_fontft_open_memory(PPUContext& ctx, u32 data, s32 size, s32 index, u32, u32 out)
{
    Scope sc(ctx);
    const u32 st = static_cast<u32>(font("cellFontStatic", {}));
    wr32(out, 0);
    if (!st || !rd32(st + 64)) return static_cast<s32>(E(3));
    const u32 mgr = rd32(st + 64) + lb::manager;
    const u32 mem = memory_of(mgr);
    Local fh(8);
    if (ft_alloc(mem, fh::size, fh.addr())) return static_cast<s32>(E(7));
    const s32 r = open_mem_face(mgr, fh.get(), data, size, index);
    if (!r) {
        wr32(out, fh.get());
        return 0;
    }
    ft_free(mem, fh.addr());
    return static_cast<s32>(r == kErrOom ? E(7) : E(0xb));
}

// +28: a font closed (its handle freed through `hp`; a cached file's stream ended and freed; a file's path given back).
s32 pspocket_fontft_close_font(PPUContext& ctx, u32 hpp, u32 out)
{
    Scope sc(ctx);
    if (out) wr32(out, 0);
    const u32 mem = memory();
    Local v(8);
    if (close_face(rd32(hpp), v.addr(), v.addr() + 4)) return static_cast<s32>(E(0xe));
    const u32 type = rd32(rd32(hpp) + fh::args_flags);
    if (type == FT_OPEN_MEMORY) {
        if (out) wr32(out, rd32(v.addr() + 4));
    } else if (type == FT_OPEN_STREAM) {
        cache_stream_end(rd32(v.addr() + 4));
        ft_free(mem, v.addr() + 4);
    }
    ft_free(mem, hpp);
    return 0;
}

s32 set_composite(u32 h, u32 a, u32 b)
{
    if (!h) return kErrFace;
    if (a && b) {
        wr32(h + fh::composite_arg, b);
        wr32(h + fh::composite_fn, a);
    }
    return 0;
}

s32 get_composite(u32 h, u32 a, u32 b)
{
    if (!h) {
        if (a) wr32(a, 0);
        if (b) wr32(b, 0);
        return kErrFace;
    }
    if (a) wr32(a, rd32(h + fh::composite_fn));
    if (b) wr32(b, rd32(h + fh::composite_arg));
    return 0;
}

s32 pspocket_fontft_set_composite_codes(PPUContext& ctx, u32 h, u32 a, u32 b)
{
    Scope sc(ctx);
    return e2(set_composite(h, a, b));
}

s32 pspocket_fontft_get_composite_codes(PPUContext& ctx, u32 h, u32 a, u32 b)
{
    Scope sc(ctx);
    return e2(get_composite(h, a, b));
}

s32 set_scale_point(u32 h, s32 cw, s32 ch, u32 dx, u32 dy)
{
    if (!h) return kErrFace;
    const s32 r = fts("FT_Set_Char_Size", {rd32(h + fh::face), static_cast<u64>(static_cast<s64>(cw)),
                                              static_cast<u64>(static_cast<s64>(ch)), dx, dy});
    if (r) {
        wr32(h + fh::dpi_y, 72);
        wr32(h + fh::dpi_x, 72);
        return r;
    }
    wr32(h + fh::dpi_x, static_cast<s32>(dx) > 0 ? dx : 72);
    wr32(h + fh::dpi_y, static_cast<s32>(dy) > 0 ? dy : 72);
    return 0;
}

s32 pspocket_fontft_set_scale_point_dpi(PPUContext& ctx, u32 h, float x, float y, s32 dx, s32 dy)
{
    Scope sc(ctx);
    return e1(set_scale_point(h, static_cast<s32>(fctidz(x * 64.0f)), static_cast<s32>(fctidz(y * 64.0f)),
        static_cast<u32>(dx), static_cast<u32>(dy)));
}

s32 pspocket_fontft_set_scale_point(PPUContext& ctx, u32 h, float x, float y)
{
    Scope sc(ctx);
    return e1(set_scale_point(h, static_cast<s32>(fctidz(x * 64.0f)), static_cast<s32>(fctidz(y * 64.0f)), 72, 72));
}

s32 set_weight(u32 h, s32 w)
{
    if (!h) return kErrFace;
    if (!rd32(h + fh::face)) {
        PS_LOG_ERROR(kChan, "an effect weight set on a handle with no face (Sony's libfontFT reads through it): refused");
        return kErrFace;
    }
    wr32(h + fh::weight, static_cast<u32>(upem(rd32(h + fh::face))) * static_cast<u32>(w));
    return 0;
}

s32 set_slant(u32 h, s32 s)
{
    if (!h) return kErrFace;
    wr32(h + fh::slant, static_cast<u32>(std::clamp(s, -0x10000, 0x10000)));
    return 0;
}

s32 pspocket_fontft_set_effect_weight(PPUContext& ctx, u32 h, float w)
{
    Scope sc(ctx);
    return e1(set_weight(h, fctiwz(w * 65536.0f)));
}

s32 pspocket_fontft_set_effect_slant(PPUContext& ctx, u32 h, float s)
{
    Scope sc(ctx);
    return e1(set_slant(h, fctiwz(s * 65536.0f)));
}

s32 pspocket_fontft_horizontal_layout(PPUContext& ctx, u32 h, u32 out)
{
    Scope sc(ctx);
    s32 desc = scaled(h, true, 6, [](u32 f) { return s64{rds32(f + fc::yMin)}; });
    s32 ascy = scaled(h, true, 6, [](u32 f) { return s64{rds32(f + fc::yMax)}; });
    float effect = 0.0f;
    if (rd32(h + fh::weight)) {
        const s32 d = static_cast<s32>(t0(weight_scaled(h, true), 1));
        ascy += d;
        desc -= d;
        effect = i2f(t0(d, 10)) * 0.015625f;
    }
    wrf(out, i2f(t0(ascy, 10)) * 0.015625f);
    wrf(out + 4, i2f(t0(ascy - desc, 10)) * 0.015625f);
    wrf(out + 8, effect);
    return 0;
}

s32 pspocket_fontft_vertical_layout(PPUContext& ctx, u32 h, u32 out)
{
    Scope sc(ctx);
    s32 w = scaled(h, false, 6, [](u32 f) { return s64{rds16(f + fc::max_adv_w)}; });
    s32 effect = 0;
    if (rd32(h + fh::weight)) {
        effect = static_cast<s32>(t0(weight_scaled(h, false), 1));
        w += 2 * effect;
    }
    if (const s32 s = rds32(h + fh::slant)) {
        const s32 bh = scaled(h, true, 6, [](u32 f) { return s64{bbox_h(f)}; });
        const s32 v = static_cast<s32>(mulsh(bh, s, 16));
        const s32 a = v < 0 ? -v : v;
        effect += a;
        w += a;
    }
    const float lw = i2f(t0(w, 10)) * 0.015625f;
    wrf(out + 4, lw);
    wrf(out + 8, i2f(t0(effect, 10)) * 0.015625f);
    wrf(out, lw * -0.5f);
    return 0;
}

s32 pspocket_fontft_glyph_metrics(PPUContext& ctx, u32 h, u32 code, u32 out)
{
    Scope sc(ctx);
    Local m(32);
    const s32 r = glyph_metrics(h, code, m.addr());
    if (r) return egm(r);
    floats_out(out, m.addr(), 8);
    return 0;
}

s32 pspocket_fontft_glyph_metrics_vertical(PPUContext& ctx, u32 h, u32 code, u32 out, s32 k)
{
    Scope sc(ctx);
    Local m(32);
    s32 r = kErrFace;
    if (h && rd32(h + fh::face)) {
        r = glyph_metrics(h, code, m.addr());
        if (!r) vertical_shift(h, k, m.addr(), 0, 0);
    }
    if (r) return egm(r);
    floats_out(out, m.addr(), 8);
    return 0;
}

// Emboldening a generated glyph's outline by the weight (26.6 / 2048, truncated).
void embolden(u32 h, u32 o)
{
    if (const s32 w = weight_scaled(h, false))
        ft("FT_Outline_Embolden", {o, static_cast<u64>(static_cast<s64>(t0(w, 11)))});
}

u32 slot_outline(u32 h)
{
    const u32 face = face_of_h(h);
    const u32 slot = face ? rd32(face + fc::glyph) : 0;
    return slot ? slot + sl::outline : 0;
}

s32 pspocket_fontft_generate_glyph(PPUContext& ctx, u32 h, u32 code, u32 out, u32 off)
{
    Scope sc(ctx);
    Local m(32);
    if (glyph_metrics(h, code, m.addr())) return static_cast<s32>(E(0x12));
    const u32 o = slot_outline(h);
    if (!o) return static_cast<s32>(E(0x12));
    embolden(h, o);
    const u32 g = make_glyph(h, o, m.addr(), off, 0);
    wr32(out, g);
    if (!g) return static_cast<s32>(E(7));
    wr16(g + 2, 1);
    return 0;
}

s32 pspocket_fontft_generate_glyph_vertical(PPUContext& ctx, u32 h, u32 code, u32 out, u32 off, s32 k)
{
    Scope sc(ctx);
    Local m(32), ab(8);
    if (const s32 r = glyph_metrics(h, code, m.addr())) return r;
    const u32 o = slot_outline(h);
    if (!o) return static_cast<s32>(E(0x12));
    embolden(h, o);
    if (k) vertical_shift(h, k, m.addr(), ab.addr(), ab.addr() + 4); // Sony's reads these unset when k is 0
    const u32 g = make_glyph(h, o, m.addr(), off, static_cast<u32>(k));
    wr32(out, g);
    if (!g) return static_cast<s32>(E(7));
    const u32 env = rd32(g + 60);
    wr32(env + 52, static_cast<u32>(k));
    wrf(env + 56, i2f(rds32(ab.addr())) * 0.015625f);
    wrf(env + 60, i2f(rds32(ab.addr() + 4)) * 0.015625f);
    wr16(g + 2, 2);
    return 0;
}

s32 pspocket_fontft_free_memory(PPUContext& ctx, u32 l, u32 p)
{
    Scope sc(ctx);
    if (!l || !p) return static_cast<s32>(E(2));
    const u32 mem = memory_of(l + lb::manager);
    if (!mem) return static_cast<s32>(E(3));
    Local v(8);
    wr32(v.addr(), p);
    ft_free(mem, v.addr());
    return 0;
}

bool initialized() { return (atomic_load_be32(gbase(), lib() + lb::lock) & lb::kInitialized) != 0; }

s32 pspocket_fontft_create_renderer(PPUContext& ctx, u32 cfg, u32 R)
{
    Scope sc(ctx);
    if (!initialized()) return static_cast<s32>(E(3));
    const u32 mem = memory();
    if (!mem) return static_cast<s32>(E(3));
    Local p(8);
    if (!ft_alloc(mem, 128, p.addr())) {
        const s32 r = renderer_init(p.get(), rds32(cfg + 4), rds32(cfg + 8), mem);
        if (old_sdk()) wr32(p.get() + 48, 0);
        if (!r) {
            wr32(R + rr::outline, 0);
            wr32(R + rr::renderer, p.get());
            return 0;
        }
        ft_free(mem, p.addr());
    }
    wr32(R + rr::outline, 0);
    wr32(R + rr::renderer, 0);
    return static_cast<s32>(E(0x23));
}

s32 pspocket_fontft_destroy_renderer(PPUContext& ctx, u32 R)
{
    Scope sc(ctx);
    if (!initialized()) return static_cast<s32>(E(3));
    const u32 r = rd32(R + rr::renderer);
    if (!r) return static_cast<s32>(E(0x22));
    const u32 mem = memory();
    renderer_done(r);
    if (!mem) return static_cast<s32>(E(3));
    ft_free(mem, R + rr::renderer);
    if (rd32(R + rr::outline)) ft_free(mem, R + rr::outline);
    return 0;
}

s32 pspocket_fontft_setup_render_scale_dpi(PPUContext& ctx, u32 r, u32 h, float w, float hh, s32 dx, s32 dy)
{
    Scope sc(ctx);
    return ers(renderer_scale(r, h, static_cast<s32>(fctidz(w * 64.0f)), static_cast<s32>(fctidz(hh * 64.0f)),
        static_cast<u32>(dx), static_cast<u32>(dy)));
}

s32 pspocket_fontft_setup_render_scale(PPUContext& ctx, u32 r, u32 h, float w, float hh)
{
    Scope sc(ctx);
    return ers(renderer_scale(r, h, static_cast<s32>(fctidz(w * 64.0f)), static_cast<s32>(fctidz(hh * 64.0f)), 72, 72));
}

s32 render_char(u32 RR, u32 code, u32 S, float x, float y, u32 outM, u32 T, s32 k)
{
    const u32 r = rd32(RR + rr::renderer);
    if (!r) return static_cast<s32>(E(0x21));
    Local scopy(36), m(32), t(24);
    std::memcpy(hp(scopy.addr()), hp(S), 36);
    const s32 x64 = static_cast<s32>(fctidz(x * 64.0f)), y64 = static_cast<s32>(fctidz(y * 64.0f));
    const u32 g = lib();
    lock_lib(g + lb::lock);
    const s32 e = k ? render_shifted(r, code, scopy.addr(), x64, y64, m.addr(), t.addr(), k)
                    : render(r, code, scopy.addr(), x64, y64, m.addr(), t.addr());
    unlock_lib(g + lb::lock);
    if (e) {
        for (const u32 i : {3u, 0u, 1u, 2u}) wr32(T + 4 * i, 0);
        wr32(T + 20, rd32(scopy.addr() + 4));
        wr32(T + 16, rd32(scopy.addr()) + static_cast<u32>((fctiwz(y) * rds32(scopy.addr() + 4) + fctiwz(x)) *
                                                           rds32(scopy.addr() + 8)));
        return static_cast<s32>(e == kErrOom ? E(7) : E(0x12));
    }
    std::memcpy(hp(T), hp(t.addr()), 24);
    if (outM) floats_out(outM, m.addr(), 8);
    return 0;
}

s32 pspocket_fontft_render_glyph(PPUContext& ctx, u32 RR, u32 code, u32 S, float x, float y, u32 outM, u32 T, s32)
{
    Scope sc(ctx);
    return render_char(RR, code, S, x, y, outM, T, 0);
}

s32 pspocket_fontft_render_glyph_shift(PPUContext& ctx, u32 RR, u32 code, u32 S, float x, float y, u32 outM, u32 T,
    s32 k)
{
    Scope sc(ctx);
    return render_char(RR, code, S, x, y, outM, T, k);
}

s32 pspocket_fontft_render_vertexes_glyph(PPUContext& ctx, u32 RR, u32 G, u32 st, u32 S, float x, float y,
    u32 outM, u32 T, s32 k)
{
    Scope sc(ctx);
    return render_vertexes(RR, G, st, S, x, y, outM, T, k);
}

// +112: part of a file read (FT_Stream_Open, then its read).
s32 pspocket_fontft_read_file(PPUContext& ctx, u32 path, u32 off, u32 size, u32 buf, u32 got, u32 f)
{
    Scope sc(ctx);
    if (!got) return static_cast<s32>(E(2));
    wr32(got, 0);
    if (!buf) return static_cast<s32>(E(2));
    if (f) return static_cast<s32>(E(0x10));
    if (!size) return 0;
    Local s(lay::st_rec);
    if (stream_open(s.addr(), path)) return static_cast<s32>(E(9));
    const u32 total = rd32(s.addr() + lay::st_size);
    s32 r = 0;
    if (!total) {
        r = static_cast<s32>(E(7));
    } else {
        if (off + size > total) size = total - off;
        const u32 n = stream_read(s.addr(), off, buf, size);
        wr32(got, n);
        if (n != size) r = static_cast<s32>(E(0xa));
    }
    stream_close(s.addr());
    return r;
}

s32 pspocket_fontft_alloc_memory(PPUContext& ctx, s32 size, u32 out)
{
    Scope sc(ctx);
    return e2(ft_alloc(memory(), size, out));
}

s32 pspocket_fontft_free_pointer(PPUContext& ctx, u32 pp)
{
    Scope sc(ctx);
    ft_free(memory(), pp);
    return 0;
}

// +124: a whole file read into a block of the library's memory (at least *size bytes).
s32 pspocket_fontft_load_file(PPUContext& ctx, u32 path, u32 buf, u32 size)
{
    Scope sc(ctx);
    if (!buf) return static_cast<s32>(E(2));
    wr32(buf, 0);
    u32 want = 0;
    if (size) {
        want = rd32(size);
        wr32(size, 0);
    }
    Local s(lay::st_rec);
    if (stream_open(s.addr(), path)) return static_cast<s32>(E(9));
    const u32 mem = memory();
    const u32 total = rd32(s.addr() + lay::st_size);
    s32 r = static_cast<s32>(E(7));
    if (mem && total) {
        if (want < total) want = total;
        if (!ft_alloc(mem, static_cast<s32>(want), buf)) {
            if (stream_read(s.addr(), 0, rd32(buf), total) != total) {
                ft_free(mem, buf);
                r = static_cast<s32>(E(0xa));
            } else {
                if (size) wr32(size, total);
                r = 0;
            }
        }
    }
    stream_close(s.addr());
    return r;
}

s32 kerning(u32 h, u32 c1, u32 c2, u32 v)
{
    const u32 face = face_of_h(h);
    if (!face) return kErrFace;
    const u32 g1 = static_cast<u32>(ft("FT_Get_Char_Index", {face, c1}));
    const u32 g2 = static_cast<u32>(ft("FT_Get_Char_Index", {face, c2}));
    return fts("FT_Get_Kerning", {face, g1, g2, FT_KERNING_UNFITTED, v});
}

s32 pspocket_fontft_kerning(PPUContext& ctx, u32 h, u32 c1, u32 c2, u32 out)
{
    Scope sc(ctx);
    Local v(8);
    const s32 r = kerning(h, c1, c2, v.addr());
    floats_out(out, v.addr(), 2);
    return e1(r);
}

// libfontFT's 75960E0C: the face's glyph loader grown for P points, C contours and S subglyphs (TrueType's maximum
// profile when negative), on the host's loader.
s32 expand_loader(u32 h, s32 P, s32 C, s32 S)
{
    const u32 face = face_of_h(h);
    if (!face) return kErrFace;
    FaceMirror* fm = face_of(face);
    FT_Face hf = fm ? static_cast<FT_Face>(const_cast<void*>(fm->host)) : nullptr;
    if (!hf) return kErrFace;
    if (P < 0 || C < 0) {
        const char* name = hf->driver ? hf->driver->root.clazz->module_name : "";
        if (std::strcmp(name, "truetype") == 0) {
            const TT_MaxProfile& mp = reinterpret_cast<TT_Face>(hf)->max_profile;
            const u32 p = std::max<u32>(mp.maxPoints, mp.maxCompositePoints);
            const u32 c = std::max<u32>(mp.maxContours, mp.maxCompositeContours);
            P = p > 2047 ? 920 : static_cast<s32>(p);
            C = c > 1023 ? 228 : static_cast<s32>(c);
            S = 7;
        } else {
            P = 920;
            C = 228;
            S = 5;
        }
    } else if (S < 0) {
        S = 7;
    }
    if (!hf->glyph || !hf->glyph->internal || !hf->glyph->internal->loader) return kErrFace;
    FT_GlyphLoader loader = hf->glyph->internal->loader;
    FT_GlyphLoader_Reset(loader);
    FT_Error e = FT_GlyphLoader_CheckPoints(loader, static_cast<FT_UInt>(P + 4), static_cast<FT_UInt>(C));
    if (!e) e = FT_GlyphLoader_CheckSubGlyphs(loader, static_cast<FT_UInt>(S + 1));
    if (e) FT_GlyphLoader_Reset(loader);
    return e;
}

s32 pspocket_fontft_expand_glyph_buffer(PPUContext& ctx, u32 h, s32 P, s32 C, s32 S)
{
    Scope sc(ctx);
    return eoom(expand_loader(h, P, C, S));
}

s32 pspocket_fontft_glyph_buffer_info(PPUContext& ctx, u32 h, u32 a, u32 b, u32 c)
{
    Scope sc(ctx);
    const u32 face = face_of_h(h);
    if (!face) return e1(kErrFace);
    FaceMirror* fm = face_of(face);
    FT_Face hf = fm ? static_cast<FT_Face>(const_cast<void*>(fm->host)) : nullptr;
    if (!hf || !hf->glyph || !hf->glyph->internal || !hf->glyph->internal->loader) return 0;
    const FT_GlyphLoader l = hf->glyph->internal->loader;
    if (a) wr32(a, l->max_points > 3 ? l->max_points - 4 : 0);
    if (b) wr32(b, l->max_contours);
    if (c) wr32(c, l->max_subglyphs);
    return 0;
}

s32 pspocket_fontft_set_em_scale(PPUContext& ctx, u32 h, float v)
{
    Scope sc(ctx);
    if (!h) return static_cast<s32>(E(2));
    if (old_sdk()) return 0;
    const u32 face = rd32(h + fh::face);
    if (!face) return static_cast<s32>(E(2));
    const s32 x = static_cast<s32>(fctidz(v * 64.0f));
    wr32(h + fh::em_scale, static_cast<u32>(ft("FT_DivFix", {static_cast<u64>(static_cast<s64>(x)),
                                                    static_cast<u64>(upem(face))})));
    return 0;
}

s32 pspocket_fontft_em_center(PPUContext& ctx, u32 h, u32 out)
{
    Scope sc(ctx);
    const s32 r = em_center(h);
    if (!r) {
        wr32(out, 0);
        return static_cast<s32>(E(2));
    }
    wrf(out, i2f(r) * 0.015625f);
    return 0;
}

void set_open_mode(u32 mgr, u32 v) { wr32(rd32(mgr + mg::library) + lay::lib_debug_hooks + 12, v); }

s32 pspocket_fontft_set_open_mode(PPUContext& ctx, u32 mode)
{
    Scope sc(ctx);
    set_open_mode(manager(), (mode & 1) << 24);
    return 0;
}

} // namespace

namespace {

using namespace fontft;

// ------------------------------------------------------------------------------------------------- exports

s32 cellFontInitLibraryFreeTypeWithRevision(PPUContext& ctx, u64 revision, u32 cfg, u32 out)
{
    Scope sc(ctx);
    const u32 st = static_cast<u32>(font("cellFontStatic", {}));
    if (!out) return static_cast<s32>(E(2));
    wr32(out, 0);
    if (!cfg) return static_cast<s32>(E(2));
    if (!st) return static_cast<s32>(E(3));
    {
        Local f(8);
        font("cellFontGetRevisionFlags", {f.addr()});
        if (!ps_read64(gbase(), f.addr())) return static_cast<s32>(E(3));
    }
    const u32 g = lib();
    lock_lib(g + lb::lock);
    const u32 v = atomic_load_be32(gbase(), g + lb::lock);
    if (v & lb::kInitialized) {
        count_up(g + lb::lock);
        unlock_lib(g + lb::lock);
        return 0;
    }
    if (!rd32(cfg + 8) || !rd32(cfg + 12)) {
        unlock_lib(g + lb::lock);
        return static_cast<s32>(E(2));
    }
    wr32(g + lb::mem_object, rd32(cfg + 4));
    wr32(g + lb::mem_free, rd32(cfg + 12));
    wr32(g + lb::mem_realloc, rd32(cfg + 16));
    wr32(g + lb::mem_calloc, rd32(cfg + 20));
    wr32(g + lb::mem_malloc, rd32(cfg + 8));
    const u32 mem = static_cast<u32>(call_guest_fn(rd32(cfg + 8), {rd32(cfg + 4), 16}));
    if (!mem) {
        unlock_lib(g + lb::lock);
        return static_cast<s32>(E(2));
    }
    std::memcpy(hp(mem), hp(g + lb::memory_rec), 16);
    {
        // games built with SDK 2.1 and older keep libfontFT's old behaviour (lv2 sys_process_get_sdk_version)
        const u32 pid = static_cast<u32>(lv2_syscall(ctx, 1, {}));
        Local ver(8);
        if (!static_cast<u32>(lv2_syscall(ctx, 25, {pid, ver.addr()}))) {
            ps_write64(gbase(), g + lb::old_sdk, 0);
            if (rd32(ver.addr()) <= 0x15FFFF) ps_write64(gbase(), g + lb::old_sdk, 1);
        }
    }
    if (!init_freetype(g + lb::manager, rd32(cfg), mem)) {
        unlock_lib(g + lb::lock);
        return static_cast<s32>(E(4));
    }
    wr32(st + 64, g);
    for (u8* base = gbase();;) {
        const u32 w = atomic_load_be32(base, g + lb::lock);
        if (atomic_cas_be32(base, g + lb::lock, w, w | lb::kInitialized) == w) break;
    }
    ps_write64(gbase(), g + lb::revision, revision);
    wr32(out, g);
    {
        Local w(8);
        font("cellFontControl", {0x00540003, w.addr()});
        call_hle("cell_FreeType2", 0x904CA41F, {rd32(w.addr()), rd32(w.addr() + 4)});
        Local ver(8);
        call_hle("cell_FreeType2", 0x211248E7, {ver.addr()});
        const u64 fv = ps_read64(gbase(), ver.addr());
        wr32(g + lb::version, (static_cast<u32>(fv) & ~0xFFu) | static_cast<u32>((fv >> 32) & 0xFF));
    }
    wr32(g + lb::lock_opd + 4, desc("pspocket_fontft_unlock"));
    wr32(g + lb::lock_if, st + 88);
    wr32(g + lb::lock_opd, desc("pspocket_fontft_lock"));
    wr16(g + lb::w152, 0);
    wr16(g + lb::w154, 0);
    count_up(g + lb::lock);
    unlock_lib(g + lb::lock);
    return 0;
}

s32 cellFontInitLibraryFreeType(PPUContext& ctx, u32 cfg, u32 out)
{
    return cellFontInitLibraryFreeTypeWithRevision(ctx, 1, cfg, out);
}

void cellFontFTGetRevisionFlags(PPUContext& ctx, u32 flags)
{
    Scope sc(ctx);
    if (flags) ps_write64(gbase(), flags, 0x43);
}

s32 cellFontFTGetInitializedRevisionFlags(PPUContext& ctx, u32 flags)
{
    Scope sc(ctx);
    if (!flags) return static_cast<s32>(E(2));
    const u64 v = ps_read64(gbase(), lib() + lb::revision);
    ps_write64(gbase(), flags, v);
    return v ? 0 : static_cast<s32>(E(3));
}

// ---- managers

u32 FTManager_Init_FreeType(PPUContext& ctx, u32 mgr, u32 library, u32 mem)
{
    Scope sc(ctx);
    return init_freetype(mgr, library, mem);
}

u32 FTManager_Done_FreeType(PPUContext& ctx, u32 mgr)
{
    Scope sc(ctx);
    return done_freetype(mgr);
}

s32 FTManager_OpenFileFace(PPUContext& ctx, u32 mgr, u32 h, u32 path, s32 index)
{
    Scope sc(ctx);
    return open_file_face(mgr, h, path, index);
}

s32 FTManager_OpenMemFace(PPUContext& ctx, u32 mgr, u32 h, u32 data, s32 size, s32 index)
{
    Scope sc(ctx);
    return open_mem_face(mgr, h, data, size, index);
}

s32 FTManager_OpenStreamFace(PPUContext& ctx, u32 mgr, u32 h, u32 stream, s32 index)
{
    Scope sc(ctx);
    return open_stream_face(mgr, h, stream, index);
}

s32 FTManager_CloseFace(PPUContext& ctx, u32, u32 h, u32 type, u32 what)
{
    Scope sc(ctx);
    return close_face(h, type, what);
}

s32 FTManager_SetFontOpenMode(PPUContext& ctx, u32 mgr, u32 v)
{
    Scope sc(ctx);
    set_open_mode(mgr, v);
    return 0;
}

// ---- cache streams

s32 FTCacheStream_Init(PPUContext& ctx, u32 s, u32 path, u32 id)
{
    Scope sc(ctx);
    return cache_stream_init(s, path, id);
}

s32 FTCacheStream_End(PPUContext& ctx, u32 s)
{
    Scope sc(ctx);
    return cache_stream_end(s);
}

s32 FTCacheStream_CacheInit(PPUContext& ctx, u32 s, u32 index, u32 cache)
{
    Scope sc(ctx);
    return cache_stream_cache_init(s, index, cache);
}

s32 FTCacheStream_CacheEnd(PPUContext& ctx, u32 s, u32 index, u32 cache)
{
    Scope sc(ctx);
    return cache_stream_cache_end(s, index, cache);
}

s32 FTCacheStream_CalcCacheIndexSize(PPUContext& ctx, u32 s)
{
    Scope sc(ctx);
    return s ? static_cast<s32>(index_size(rd32(s + lay::st_size))) : 0;
}

// ---- face handles

s32 FTFaceH_Close(PPUContext& ctx, u32 h, u32 type, u32 what)
{
    Scope sc(ctx);
    return close_face(h, type, what);
}

u32 FTFaceH_FontFamilyName(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    const u32 face = face_of_h(h);
    return face ? rd32(face + lay::face_family_name) : shared(); // libfontFT's "" (its shared record's first byte)
}

u32 FTFaceH_FontStyleName(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    const u32 face = face_of_h(h);
    return face ? rd32(face + lay::face_style_name) : shared();
}

s32 FTFaceH_GetAscender(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, true, 16, [](u32 f) { return s64{asc(f)}; });
}

s32 FTFaceH_GetBoundingBoxHeight(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, true, 16, [](u32 f) { return s64{bbox_h(f)}; });
}

s32 FTFaceH_GetBoundingBoxMaxX(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, false, 16, [](u32 f) { return s64{rds32(f + fc::xMax)}; });
}

s32 FTFaceH_GetBoundingBoxMaxY(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, true, 16, [](u32 f) { return s64{rds32(f + fc::yMax)}; });
}

s32 FTFaceH_GetBoundingBoxMinX(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, false, 16, [](u32 f) { return s64{rds32(f + fc::xMin)}; });
}

s32 FTFaceH_GetBoundingBoxMinY(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, true, 16, [](u32 f) { return s64{rds32(f + fc::yMin)}; });
}

s32 FTFaceH_GetBoundingBoxWidth(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, false, 16, [](u32 f) { return s64{bbox_w(f)}; });
}

s32 FTFaceH_GetMaxHorizontalAdvance(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, false, 16, [](u32 f) { return s64{rds16(f + fc::max_adv_w)}; });
}

s32 FTFaceH_GetMaxVerticalAdvance(PPUContext& ctx, u32 h)
{
    Scope sc(ctx);
    return scaled(h, true, 16, [](u32 f) { return s64{rds16(f + fc::max_adv_h)}; });
}

s32 FTFaceH_GetCompositeCodes(PPUContext& ctx, u32 h, u32 a, u32 b)
{
    Scope sc(ctx);
    return get_composite(h, a, b);
}

s32 FTFaceH_SetCompositeCodes(PPUContext& ctx, u32 h, u32 a, u32 b)
{
    Scope sc(ctx);
    return set_composite(h, a, b);
}

s32 FTFaceH_GetGlyphImage(PPUContext& ctx, u32 h, u32 code, u32 S, s32 W, s32 H, s32 dx, s32 dy)
{
    Scope sc(ctx);
    return glyph_image(h, code, S, W, H, dx, dy);
}

s32 FTFaceH_GetGlyphMetrics(PPUContext& ctx, u32 h, u32 code, u32 m)
{
    Scope sc(ctx);
    return glyph_metrics(h, code, m);
}

s32 FTFaceH_GetKerning(PPUContext& ctx, u32 h, u32 c1, u32 c2, u32 v)
{
    Scope sc(ctx);
    return kerning(h, c1, c2, v);
}

s32 FTFaceH_GetRenderBufferSize(PPUContext& ctx, u32 h, u32 w, u32 hh)
{
    Scope sc(ctx);
    return buffer_size(h, w, hh, 0, 0);
}

s32 FTFaceH_GetRenderEffectSlant(PPUContext& ctx, u32 h, u32 out)
{
    Scope sc(ctx);
    if (!h) return kErrFace;
    wr32(out, rd32(h + fh::slant));
    return 0;
}

s32 FTFaceH_GetRenderEffectWeight(PPUContext& ctx, u32 h, u32 out)
{
    Scope sc(ctx);
    if (!h) return kErrFace;
    const u32 face = rd32(h + fh::face);
    const s32 u = face ? upem(face) : 0;
    wr32(out, u ? static_cast<u32>(rds32(h + fh::weight) / u) : 0);
    return 0;
}

s32 FTFaceH_GetRenderScale(PPUContext& ctx, u32 h, u32 x, u32 y)
{
    Scope sc(ctx);
    const u32 face = face_of_h(h);
    const u32 size = face ? rd32(face + fc::size) : 0;
    if (!size) return kErrFace;
    if (x) wr32(x, static_cast<u32>(t0(rds32(size + sz::x_scale), 16)));
    if (y) wr32(y, static_cast<u32>(t0(rds32(size + sz::y_scale), 16)));
    return 0;
}

s32 FTFaceH_GetRenderScalePixel(PPUContext& ctx, u32 h, u32 x, u32 y)
{
    Scope sc(ctx);
    const u32 face = face_of_h(h);
    const u32 size = face ? rd32(face + fc::size) : 0;
    if (!size) return kErrFace;
    if (x) wr32(x, rd16(size + sz::x_ppem));
    if (y) wr32(y, rd16(size + sz::y_ppem));
    return 0;
}

s32 FTFaceH_GetRenderScalePoint(PPUContext& ctx, u32 h, u32 x, u32 y, u32 dx, u32 dy)
{
    Scope sc(ctx);
    const u32 face = face_of_h(h);
    const u32 size = face ? rd32(face + fc::size) : 0;
    if (!size) return kErrFace;
    const auto pt = [](u32 ppem, u32 dpi) { return dpi ? ppem * 72 / dpi : 0; };
    if (x) wr32(x, pt(rd16(size + sz::x_ppem), rd32(h + fh::dpi_x)));
    if (y) wr32(y, pt(rd16(size + sz::y_ppem), rd32(h + fh::dpi_y)));
    if (dx) wr32(dx, rd32(h + fh::dpi_x));
    if (dy) wr32(dy, rd32(h + fh::dpi_y));
    return 0;
}

s32 FTFaceH_SetRenderEffectSlant(PPUContext& ctx, u32 h, s32 s)
{
    Scope sc(ctx);
    return set_slant(h, s);
}

s32 FTFaceH_SetRenderEffectWeight(PPUContext& ctx, u32 h, s32 w)
{
    Scope sc(ctx);
    return set_weight(h, w);
}

s32 FTFaceH_SetRenderScalePixel(PPUContext& ctx, u32 h, u32 w, u32 hh)
{
    Scope sc(ctx);
    if (!h) return kErrFace;
    const s32 r = fts("FT_Set_Pixel_Sizes", {rd32(h + fh::face), w, hh});
    if (!r) {
        wr32(h + fh::dpi_x, 72);
        wr32(h + fh::dpi_y, 72);
    }
    return r;
}

s32 FTFaceH_SetRenderScalePoint(PPUContext& ctx, u32 h, s32 cw, s32 ch, u32 dx, u32 dy)
{
    Scope sc(ctx);
    return set_scale_point(h, cw, ch, dx, dy);
}

} // namespace

#define FF_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, false}
#define FF_INTERNAL(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, true}

void register_cellFontFT(Registry& r)
{
    r.add("cellFontFT", {
        FF_FUNC(0xD08B68DA, FTCacheStream_CacheEnd),
        FF_FUNC(0x8DFABC0A, FTCacheStream_CacheInit),
        FF_FUNC(0x3EAAAD1C, FTCacheStream_CalcCacheIndexSize),
        FF_FUNC(0x1B67409D, FTCacheStream_End),
        FF_FUNC(0x38750386, FTCacheStream_Init),
        FF_FUNC(0x9A556A5F, FTFaceH_Close),
        FF_FUNC(0x2E267B28, FTFaceH_FontFamilyName),
        FF_FUNC(0xDA0FB37E, FTFaceH_FontStyleName),
        FF_FUNC(0x4B478CDD, FTFaceH_GetAscender),
        FF_FUNC(0xC25D60F0, FTFaceH_GetBoundingBoxHeight),
        FF_FUNC(0x280E6BA5, FTFaceH_GetBoundingBoxMaxX),
        FF_FUNC(0xB6A6AD5A, FTFaceH_GetBoundingBoxMaxY),
        FF_FUNC(0x71F86685, FTFaceH_GetBoundingBoxMinX),
        FF_FUNC(0xA4F73351, FTFaceH_GetBoundingBoxMinY),
        FF_FUNC(0xF5376485, FTFaceH_GetBoundingBoxWidth),
        FF_FUNC(0x3F9A35FD, FTFaceH_GetCompositeCodes),
        FF_FUNC(0x65E27CB3, FTFaceH_GetGlyphImage),
        FF_FUNC(0x400B7E88, FTFaceH_GetGlyphMetrics),
        FF_FUNC(0xFDB828D0, FTFaceH_GetKerning),
        FF_FUNC(0xE5282470, FTFaceH_GetMaxHorizontalAdvance),
        FF_FUNC(0x05F214E0, FTFaceH_GetMaxVerticalAdvance),
        FF_FUNC(0x8F7AEA15, FTFaceH_GetRenderBufferSize),
        FF_FUNC(0x53E38FF2, FTFaceH_GetRenderEffectSlant),
        FF_FUNC(0x29F51030, FTFaceH_GetRenderEffectWeight),
        FF_FUNC(0x8BB8A16A, FTFaceH_GetRenderScale),
        FF_FUNC(0x9306D15B, FTFaceH_GetRenderScalePixel),
        FF_FUNC(0x73209D59, FTFaceH_GetRenderScalePoint),
        FF_FUNC(0x9D792718, FTFaceH_SetCompositeCodes),
        FF_FUNC(0x1130EBE7, FTFaceH_SetRenderEffectSlant),
        FF_FUNC(0x77A94FA7, FTFaceH_SetRenderEffectWeight),
        FF_FUNC(0xF7715A5B, FTFaceH_SetRenderScalePixel),
        FF_FUNC(0x529AFC47, FTFaceH_SetRenderScalePoint),
        FF_FUNC(0x947C7B8A, FTManager_CloseFace),
        FF_FUNC(0xD632BCBF, FTManager_Done_FreeType),
        FF_FUNC(0xF8244C5E, FTManager_Init_FreeType),
        FF_FUNC(0xFCC91A83, FTManager_OpenFileFace),
        FF_FUNC(0x5E89C269, FTManager_OpenMemFace),
        FF_FUNC(0x329DAD93, FTManager_OpenStreamFace),
        FF_FUNC(0x5CF9C3F0, FTManager_SetFontOpenMode),
        FF_FUNC(0xFA0C2DE0, cellFontFTGetInitializedRevisionFlags),
        FF_FUNC(0xEC89A187, cellFontFTGetRevisionFlags),
        FF_FUNC(0xF9677A46, cellFontInitLibraryFreeType),
        FF_FUNC(0x7A0A83C4, cellFontInitLibraryFreeTypeWithRevision),
        FF_INTERNAL(0x2F04A3AD, pspocket_fontft_alloc),
        FF_INTERNAL(0x7257DADD, pspocket_fontft_free),
        FF_INTERNAL(0xD2F3916B, pspocket_fontft_realloc),
        FF_INTERNAL(0x3FB0D40B, pspocket_fontft_lock),
        FF_INTERNAL(0xF761252B, pspocket_fontft_unlock),
        FF_INTERNAL(0x347752D9, pspocket_fontft_file_io),
        FF_INTERNAL(0x2CFE05CC, pspocket_fontft_file_close),
        FF_INTERNAL(0x35559472, pspocket_fontft_cached_io),
        FF_INTERNAL(0xC8916323, pspocket_fontft_end_library),
        FF_INTERNAL(0x1BB6633B, pspocket_fontft_open_file),
        FF_INTERNAL(0xC93B51F7, pspocket_fontft_open_memory),
        FF_INTERNAL(0xF11CBAEB, pspocket_fontft_close_font),
        FF_INTERNAL(0xF50C2AE2, pspocket_fontft_set_composite_codes),
        FF_INTERNAL(0x2EC6D228, pspocket_fontft_get_composite_codes),
        FF_INTERNAL(0x2EF241CD, pspocket_fontft_set_scale_point_dpi),
        FF_INTERNAL(0xE2CA94DB, pspocket_fontft_set_scale_point),
        FF_INTERNAL(0xC4407354, pspocket_fontft_set_effect_weight),
        FF_INTERNAL(0xF56BC3C9, pspocket_fontft_set_effect_slant),
        FF_INTERNAL(0xEA794AA6, pspocket_fontft_horizontal_layout),
        FF_INTERNAL(0xFD726CD5, pspocket_fontft_vertical_layout),
        FF_INTERNAL(0x6888C6D0, pspocket_fontft_glyph_metrics),
        FF_INTERNAL(0x78702DD2, pspocket_fontft_glyph_metrics_vertical),
        FF_INTERNAL(0x1B7FF1E6, pspocket_fontft_generate_glyph),
        FF_INTERNAL(0x3C015B1A, pspocket_fontft_generate_glyph_vertical),
        FF_INTERNAL(0x9CE60FE8, pspocket_fontft_free_memory),
        FF_INTERNAL(0xB6E17971, pspocket_fontft_create_renderer),
        FF_INTERNAL(0x84C87F8F, pspocket_fontft_destroy_renderer),
        FF_INTERNAL(0xC42B10AD, pspocket_fontft_setup_render_scale_dpi),
        FF_INTERNAL(0xADE2C608, pspocket_fontft_setup_render_scale),
        FF_INTERNAL(0x49558883, pspocket_fontft_render_glyph),
        FF_INTERNAL(0xCD532FA3, pspocket_fontft_render_glyph_shift),
        FF_INTERNAL(0x800E3A86, pspocket_fontft_render_vertexes_glyph),
        FF_INTERNAL(0xB54C51F9, pspocket_fontft_read_file),
        FF_INTERNAL(0xB5E9C8FE, pspocket_fontft_alloc_memory),
        FF_INTERNAL(0x9EB7704D, pspocket_fontft_free_pointer),
        FF_INTERNAL(0x82975933, pspocket_fontft_load_file),
        FF_INTERNAL(0x3E532181, pspocket_fontft_kerning),
        FF_INTERNAL(0xFCA0697A, pspocket_fontft_expand_glyph_buffer),
        FF_INTERNAL(0x7D94BE52, pspocket_fontft_glyph_buffer_info),
        FF_INTERNAL(0xDA447E38, pspocket_fontft_set_em_scale),
        FF_INTERNAL(0x10590AE8, pspocket_fontft_em_center),
        FF_INTERNAL(0xDD7A19E7, pspocket_fontft_set_open_mode),
    });
    // libfont's functions libfontFT imports: the firmware's libfont's when it runs as it is (fontft::font)
    static constexpr HostImport kImports[] = {
        {{}, "cellFont", 0xE2F14DCF, false}, // cellFontStatic
        {{}, "cellFont", 0x67E805C3, false}, // cellFontsetUSleep
        {{}, "cellFont", 0xB015A84E, false}, // cellFontGetRevisionFlags
        {{}, "cellFont", 0x3897F229, false}, // cellFontControl
    };
    r.add_imports("cellFontFT", kImports);
}

} // namespace pspocket::hle

#else

namespace pspocket::hle {

// Without FreeType (PSPOCKET_FREETYPE_DIR) the firmware's libfontFT serves the games.
void register_cellFontFT(Registry&) {}

} // namespace pspocket::hle

#endif
