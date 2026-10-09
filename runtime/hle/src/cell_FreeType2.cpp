// cell_FreeType2: Sony's libfreetype.sprx (FreeType 2.2.1 for the PPU) as pspocket's own (D-525). FreeType 2.2.1 runs
// on the host with Sony's options and modules; freetype_bridge.hpp says how its objects are shown to the guest. This
// file: the library and its modules, faces, sizes, charmaps, glyph loading and the format-specific queries, the
// fixed-point and trigonometric functions (FreeType's code for a 32-bit FT_Long with 64-bit products, as Sony's
// build computes them), lists, the old memory calls, and Sony's own additions (cellFreeType2Ex, the file-error
// handler). Outlines, bitmaps, glyph images, the stroker and the compressed streams are in freetype_glyph.cpp, the
// cache subsystem in freetype_cache.cpp.
//
// References: FreeType 2.2.1 (external/freetype-2.2.1: src/base/ftobjs.c, ftcalc.c, fttrigon.c, ftutil.c, ftsnames.c,
// fttype1.c, ftwinfnt.c, ftxf86.c, ftbdf.c, ftpfr.c, ftmm.c, ftotval.c, ftsynth.c); Sony's libfreetype.sprx 4.92 as
// the oracle (tests/hle/test_freetype_compare.cpp).

#include "pspocket/hle/hle.hpp"

#if PSPOCKET_HAVE_FREETYPE

#include "freetype_bridge.hpp"
#include "freetype_calc.hpp"

#include FT_INTERNAL_OBJECTS_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_TYPE1_TABLES_H
#include FT_WINFONTS_H
#include FT_MULTIPLE_MASTERS_H
#include FT_BDF_H
#include FT_PFR_H
#include FT_XFREE86_H
#include FT_OPENTYPE_VALIDATE_H
#include FT_SYNTHESIS_H
#include FT_TRIGONOMETRY_H
#include FT_LIST_H
#include FT_INTERNAL_TRUETYPE_TYPES_H

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::hle {

namespace g {

using namespace ftb;
using namespace ftcalc;

constexpr const char* kChan = "freetype";

FT_Face hface(FaceMirror* f) { return f ? static_cast<FT_Face>(const_cast<void*>(f->host)) : nullptr; }
FT_GlyphSlot hslot(SlotMirror* s) { return s ? static_cast<FT_GlyphSlot>(const_cast<void*>(s->host)) : nullptr; }
FT_Size hsize(SizeMirror* s) { return s ? static_cast<FT_Size>(const_cast<void*>(s->host)) : nullptr; }
FT_CharMap hcmap(Mirror* c) { return c ? static_cast<FT_CharMap>(const_cast<void*>(c->host)) : nullptr; }

// The face mirror of a host face (a slot's or a size's).
FaceMirror* face_mirror(FT_Face face)
{
    Mirror* m = face ? by_host(face) : nullptr;
    return m && m->kind == Mirror::Kind::face ? static_cast<FaceMirror*>(m) : nullptr;
}

s32 FT_MulDiv(PPUContext&, s32 a, s32 b, s32 c) { return muldiv32(a, b, c); }
s32 FT_MulFix(PPUContext&, s32 a, s32 b) { return mulfix32(a, b); }
s32 FT_DivFix(PPUContext&, s32 a, s32 b) { return divfix32(a, b); }

s32 FT_RoundFix(PPUContext&, s32 a)
{
    return a >= 0 ? static_cast<s32>((static_cast<u32>(a) + 0x8000u) & ~0xFFFFu)
                  : neg32(static_cast<s32>((static_cast<u32>(neg32(a)) + 0x8000u) & ~0xFFFFu));
}

s32 FT_CeilFix(PPUContext&, s32 a)
{
    return a >= 0 ? static_cast<s32>((static_cast<u32>(a) + 0xFFFFu) & ~0xFFFFu)
                  : neg32(static_cast<s32>((static_cast<u32>(neg32(a)) + 0xFFFFu) & ~0xFFFFu));
}

s32 FT_FloorFix(PPUContext&, s32 a)
{
    return a >= 0 ? static_cast<s32>(static_cast<u32>(a) & ~0xFFFFu)
                  : neg32(static_cast<s32>(static_cast<u32>(neg32(a)) & ~0xFFFFu));
}

s32 FT_Cos(PPUContext&, s32 angle) { return cos32(angle); }
s32 FT_Sin(PPUContext&, s32 angle) { return cos32(sub32(kAnglePi2, angle)); }

s32 FT_Tan(PPUContext&, s32 angle)
{
    Vec v{kTrigCoscale >> 2, 0};
    trig_pseudo_rotate(v, angle);
    return divfix32(v.y, v.x);
}

s32 FT_Atan2(PPUContext&, s32 dx, s32 dy) { return atan2_32(dx, dy); }
s32 FT_Angle_Diff(PPUContext&, s32 angle1, s32 angle2) { return angle_diff32(angle1, angle2); }

Vec read_vec(u32 a) { return Vec{rds32(a), rds32(a + 4)}; }
void write_vec(u32 a, const Vec& v)
{
    wr32(a, static_cast<u32>(v.x));
    wr32(a + 4, static_cast<u32>(v.y));
}

void FT_Vector_Unit(PPUContext& ctx, u32 vec, s32 angle)
{
    Scope sc(ctx);
    Vec v{kTrigCoscale >> 2, 0};
    trig_pseudo_rotate(v, angle);
    v.x >>= 12;
    v.y >>= 12;
    write_vec(vec, v);
}

void FT_Vector_Rotate(PPUContext& ctx, u32 vec, s32 angle)
{
    Scope sc(ctx);
    Vec v = read_vec(vec);
    vector_rotate32(v, angle);
    write_vec(vec, v);
}

s32 FT_Vector_Length(PPUContext& ctx, u32 vec)
{
    Scope sc(ctx);
    Vec v = read_vec(vec);
    if (v.x == 0) return v.y >= 0 ? v.y : neg32(v.y);
    if (v.y == 0) return v.x >= 0 ? v.x : neg32(v.x);
    const s32 shift = trig_prenorm(v);
    trig_pseudo_polarize(v);
    v.x = trig_downscale(v.x);
    if (shift > 0) return add32(v.x, static_cast<s32>(1u << (shift - 1))) >> shift;
    return shl32(v.x, -shift);
}

void FT_Vector_Polarize(PPUContext& ctx, u32 vec, u32 length, u32 angle)
{
    Scope sc(ctx);
    Vec v = read_vec(vec);
    if (v.x == 0 && v.y == 0) return;
    const s32 shift = trig_prenorm(v);
    trig_pseudo_polarize(v);
    v.x = trig_downscale(v.x);
    wr32(length, static_cast<u32>(shift >= 0 ? v.x >> shift : shl32(v.x, -shift)));
    wr32(angle, static_cast<u32>(v.y));
}

void FT_Vector_From_Polar(PPUContext& ctx, u32 vec, s32 length, s32 angle)
{
    Scope sc(ctx);
    Vec v{length, 0};
    vector_rotate32(v, angle);
    write_vec(vec, v);
}

// src/base/ftoutln.c FT_Vector_Transform, ftcalc.c FT_Matrix_Multiply and FT_Matrix_Invert.
void FT_Vector_Transform(PPUContext& ctx, u32 vec, u32 matrix)
{
    if (!vec || !matrix) return;
    Scope sc(ctx);
    const Vec v = read_vec(vec);
    const s32 xx = rds32(matrix), xy = rds32(matrix + 4), yx = rds32(matrix + 8), yy = rds32(matrix + 12);
    write_vec(vec, Vec{add32(mulfix32(v.x, xx), mulfix32(v.y, xy)), add32(mulfix32(v.x, yx), mulfix32(v.y, yy))});
}

void FT_Matrix_Multiply(PPUContext& ctx, u32 a, u32 b)
{
    if (!a || !b) return;
    Scope sc(ctx);
    const s32 axx = rds32(a), axy = rds32(a + 4), ayx = rds32(a + 8), ayy = rds32(a + 12);
    const s32 bxx = rds32(b), bxy = rds32(b + 4), byx = rds32(b + 8), byy = rds32(b + 12);
    wr32(b, static_cast<u32>(add32(mulfix32(axx, bxx), mulfix32(axy, byx))));
    wr32(b + 4, static_cast<u32>(add32(mulfix32(axx, bxy), mulfix32(axy, byy))));
    wr32(b + 8, static_cast<u32>(add32(mulfix32(ayx, bxx), mulfix32(ayy, byx))));
    wr32(b + 12, static_cast<u32>(add32(mulfix32(ayx, bxy), mulfix32(ayy, byy))));
}

s32 FT_Matrix_Invert(PPUContext& ctx, u32 m)
{
    if (!m) return FT_Err_Invalid_Argument;
    Scope sc(ctx);
    const s32 xx = rds32(m), xy = rds32(m + 4), yx = rds32(m + 8), yy = rds32(m + 12);
    const s32 delta = sub32(mulfix32(xx, yy), mulfix32(xy, yx));
    if (!delta) return FT_Err_Invalid_Argument;
    wr32(m + 4, static_cast<u32>(neg32(divfix32(xy, delta))));
    wr32(m + 8, static_cast<u32>(neg32(divfix32(yx, delta))));
    wr32(m, static_cast<u32>(divfix32(yy, delta)));
    wr32(m + 12, static_cast<u32>(divfix32(xx, delta)));
    return FT_Err_Ok;
}

// ------------------------------------------------------------------------------------------------- lists
// src/base/ftutil.c, on the guest's FT_ListRec and FT_ListNodeRec.

u32 FT_List_Find(PPUContext& ctx, u32 list, u32 data)
{
    Scope sc(ctx);
    for (u32 cur = rd32(list + lay::list_head); cur; cur = rd32(cur + lay::node_next))
        if (rd32(cur + lay::node_data) == data) return cur;
    return 0;
}

void FT_List_Add(PPUContext& ctx, u32 list, u32 node)
{
    Scope sc(ctx);
    const u32 before = rd32(list + lay::list_tail);
    wr32(node + lay::node_next, 0);
    wr32(node + lay::node_prev, before);
    if (before) wr32(before + lay::node_next, node);
    else wr32(list + lay::list_head, node);
    wr32(list + lay::list_tail, node);
}

void FT_List_Insert(PPUContext& ctx, u32 list, u32 node)
{
    Scope sc(ctx);
    const u32 after = rd32(list + lay::list_head);
    wr32(node + lay::node_next, after);
    wr32(node + lay::node_prev, 0);
    if (!after) wr32(list + lay::list_tail, node);
    else wr32(after + lay::node_prev, node);
    wr32(list + lay::list_head, node);
}

void FT_List_Remove(PPUContext& ctx, u32 list, u32 node)
{
    Scope sc(ctx);
    const u32 before = rd32(node + lay::node_prev), after = rd32(node + lay::node_next);
    if (before) wr32(before + lay::node_next, after);
    else wr32(list + lay::list_head, after);
    if (after) wr32(after + lay::node_prev, before);
    else wr32(list + lay::list_tail, before);
}

void FT_List_Up(PPUContext& ctx, u32 list, u32 node)
{
    Scope sc(ctx);
    const u32 before = rd32(node + lay::node_prev), after = rd32(node + lay::node_next);
    if (!before) return;
    wr32(before + lay::node_next, after);
    if (after) wr32(after + lay::node_prev, before);
    else wr32(list + lay::list_tail, before);
    wr32(node + lay::node_prev, 0);
    wr32(node + lay::node_next, rd32(list + lay::list_head));
    wr32(rd32(list + lay::list_head) + lay::node_prev, node);
    wr32(list + lay::list_head, node);
}

s32 FT_List_Iterate(PPUContext& ctx, u32 list, u32 iterator, u32 user)
{
    Scope sc(ctx);
    u32 cur = rd32(list + lay::list_head);
    s32 error = FT_Err_Ok;
    while (cur) {
        const u32 next = rd32(cur + lay::node_next);
        error = static_cast<s32>(call_guest_fn(iterator, {cur, user}));
        if (error) break;
        cur = next;
    }
    return error;
}

void FT_List_Finalize(PPUContext& ctx, u32 list, u32 destroy, u32 memory, u32 user)
{
    Scope sc(ctx);
    u32 cur = rd32(list + lay::list_head);
    while (cur) {
        const u32 next = rd32(cur + lay::node_next);
        const u32 data = rd32(cur + lay::node_data);
        if (destroy) call_guest_fn(destroy, {memory, data, user});
        g_free(memory, cur);
        cur = next;
    }
    wr32(list + lay::list_head, 0);
    wr32(list + lay::list_tail, 0);
}

// ------------------------------------------------------------------------------------------------- old memory calls
// src/base/ftutil.c under FT_CONFIG_OPTION_OLD_INTERNALS: FT_ALLOC, FT_REALLOC and FT_FREE on the guest's pointer.

s32 FT_Alloc(PPUContext& ctx, u32 memory, s32 size, u32 p)
{
    Scope sc(ctx);
    FT_Error error = FT_Err_Ok;
    const u32 block = g_alloc(memory, size, error);
    wr32(p, block);
    return error;
}

s32 FT_Realloc(PPUContext& ctx, u32 memory, s32 current, s32 size, u32 p)
{
    Scope sc(ctx);
    FT_Error error = FT_Err_Ok;
    const u32 block = g_realloc(memory, 1, current, size, rd32(p), error);
    wr32(p, block);
    return error;
}

void FT_Free(PPUContext& ctx, u32 memory, u32 p)
{
    Scope sc(ctx);
    if (const u32 block = rd32(p)) {
        g_free(memory, block);
        wr32(p, 0);
    }
}

// ------------------------------------------------------------------------------------------------- libraries

// Sony's system layer has no allocator: its FT_New_Memory answers NULL, and so FT_Init_FreeType answers
// FT_Err_Unimplemented_Feature without a library.
u32 FT_New_Memory(PPUContext&) { return 0; }
s32 FT_Init_FreeType(PPUContext&, u32) { return FT_Err_Unimplemented_Feature; }

s32 FT_New_Library(PPUContext& ctx, u32 memory, u32 alibrary)
{
    Scope sc(ctx);
    u32 lib = 0;
    const FT_Error e = new_library(memory, &lib);
    if (!e) wr32(alibrary, lib);
    return e;
}

s32 FT_Done_Library(PPUContext& ctx, u32 library)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    return done_library(l);
}

s32 FT_Done_FreeType(PPUContext& ctx, u32 library)
{
    Scope sc(ctx);
    if (Library* l = library_of(library)) {
        const u32 memory = l->memory;
        done_library(l);
        call_guest_fn(rd32(memory + lay::mem_free), {memory, memory}); // FT_Done_Memory
    }
    return FT_Err_Ok;
}

void FT_Add_Default_Modules(PPUContext& ctx, u32 library)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return;
    LibLock lk = lock(l);
    ::FT_Add_Default_Modules(l->ft);
    patch_sfnt(l->ft);
    sync_library(l);
}

s32 FT_Add_Module(PPUContext& ctx, u32 library, u32 clazz)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    if (!clazz) return FT_Err_Invalid_Argument;
    LibLock lk = lock(l);
    if (const FT_Module_Class* c = class_of(clazz)) {
        // a class of FreeType's own (as FT_Get_Module(...)->clazz gave it)
        const FT_Error e = ::FT_Add_Module(l->ft, c);
        patch_sfnt(l->ft);
        const FT_Error s = sync_library(l);
        return e ? e : s;
    }
    // A module class of the game's own: FreeType's checks, then refused (a module's code is the game's; pspocket's
    // FreeType runs on the host).
    if (rd32(clazz + lay::cls_requires) > static_cast<u32>((FREETYPE_MAJOR << 16) | FREETYPE_MINOR))
        return FT_Err_Invalid_Version;
    const std::string name = gstr(rd32(clazz + lay::cls_name));
    for (FT_UInt i = 0; i < l->ft->num_modules; i++) {
        FT_Module m = l->ft->modules[i];
        if (name == m->clazz->module_name) {
            if (static_cast<FT_Fixed>(rds32(clazz + lay::cls_version)) <= m->clazz->module_version)
                return FT_Err_Lower_Module_Version;
            break;
        }
    }
    if (l->ft->num_modules >= FT_MAX_MODULES) return FT_Err_Too_Many_Drivers;
    PS_LOG_ERROR(kChan, "FT_Add_Module: the game's own module class {:#x} (\"{}\") is not supported", clazz, name);
    return FT_Err_Unimplemented_Feature;
}

s32 FT_Remove_Module(PPUContext& ctx, u32 library, u32 module)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    LibLock lk = lock(l);
    const FT_Error e = ::FT_Remove_Module(l->ft, module_of(module));
    sync_library(l);
    return e;
}

u32 FT_Get_Module(PPUContext& ctx, u32 library, u32 name)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l || !name) return 0;
    LibLock lk = lock(l);
    const std::string n = gstr(name);
    FT_Error error = FT_Err_Ok;
    return module_guest(l, ::FT_Get_Module(l->ft, n.c_str()), error);
}

u32 FT_Get_Renderer(PPUContext& ctx, u32 library, u32 format)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return 0;
    LibLock lk = lock(l);
    FT_Error error = FT_Err_Ok;
    FT_Renderer r = ::FT_Get_Renderer(l->ft, static_cast<FT_Glyph_Format>(format));
    return r ? module_guest(l, &r->root, error) : 0;
}

s32 FT_Set_Renderer(PPUContext& ctx, u32 library, u32 renderer, u32 num_params, u32 params)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    LibLock lk = lock(l);
    FT_Module m = module_of(renderer);
    std::vector<FT_Parameter> p(num_params);
    for (u32 i = 0; i < num_params && params; i++) {
        p[i].tag = rd32(params + 8 * i);
        const u32 data = rd32(params + 8 * i + 4);
        p[i].data = data ? hp(data) : nullptr;
    }
    // a module that is not a renderer is not in the renderers list: FreeType's answer for an unknown one
    FT_Renderer r = m && FT_MODULE_IS_RENDERER(m) ? reinterpret_cast<FT_Renderer>(m) : nullptr;
    if (renderer && !r) return FT_Err_Invalid_Argument;
    const FT_Error e = ::FT_Set_Renderer(l->ft, r, num_params, params ? p.data() : nullptr);
    sync_library(l);
    return e;
}

void FT_Set_Debug_Hook(PPUContext& ctx, u32 library, u32 index, u32 hook)
{
    Scope sc(ctx);
    if (library_of(library) && hook && index < 4) wr32(library + lay::lib_debug_hooks + 4 * index, hook);
}

void FT_Library_Version(PPUContext& ctx, u32 library, u32 amajor, u32 aminor, u32 apatch)
{
    Scope sc(ctx);
    u32 v[3] = {0, 0, 0};
    if (library_of(library))
        for (u32 i = 0; i < 3; i++) v[i] = rd32(library + lay::lib_version + 4 * i);
    if (amajor) wr32(amajor, v[0]);
    if (aminor) wr32(aminor, v[1]);
    if (apatch) wr32(apatch, v[2]);
}

u32 FT_Get_TrueType_Engine_Type(PPUContext& ctx, u32 library)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    LibLock lk = lock(l);
    return ::FT_Get_TrueType_Engine_Type(l ? l->ft : nullptr);
}

// ------------------------------------------------------------------------------------------------- opening faces

// A guest FT_Open_Args as the host's: memory and path in guest memory, a guest stream bridged, the driver's module.
struct OpenArgs {
    FT_Open_Args a{};
    std::vector<FT_Parameter> params;
    bool bad_driver = false;
    bool bridged = false; // a.stream is a bridge of ours, freed if the call never reaches FreeType
    ~OpenArgs()
    {
        if (bridged && !used && a.stream) free_bridge_stream(a.stream);
    }
    bool used = false;
};

OpenArgs read_open_args(u32 g)
{
    OpenArgs o;
    o.a.flags = rd32(g + lay::oa_flags);
    if (o.a.flags & FT_OPEN_MEMORY) {
        const u32 base = rd32(g + lay::oa_memory_base);
        o.a.memory_base = base ? hp(base) : nullptr;
        o.a.memory_size = rds32(g + lay::oa_memory_size);
    }
    if (o.a.flags & FT_OPEN_PATHNAME) {
        const u32 path = rd32(g + lay::oa_pathname);
        o.a.pathname = path ? reinterpret_cast<char*>(hp(path)) : nullptr;
    }
    if (o.a.flags & FT_OPEN_STREAM) {
        const u32 s = rd32(g + lay::oa_stream);
        if (s) {
            o.a.stream = host_stream_for(s);
            if (!o.a.stream) {
                o.a.stream = bridge_stream(s);
                o.bridged = true;
            }
        }
    }
    if (o.a.flags & FT_OPEN_DRIVER) {
        const u32 d = rd32(g + lay::oa_driver);
        o.a.driver = module_of(d);
        o.bad_driver = d && !o.a.driver;
    }
    if (o.a.flags & FT_OPEN_PARAMS) {
        o.a.num_params = rds32(g + lay::oa_num_params);
        const u32 params = rd32(g + lay::oa_params);
        if (o.a.num_params > 0 && params) {
            o.params.resize(static_cast<usize>(o.a.num_params));
            for (usize i = 0; i < o.params.size(); i++) {
                o.params[i].tag = rd32(params + 8 * static_cast<u32>(i));
                const u32 data = rd32(params + 8 * static_cast<u32>(i) + 4);
                o.params[i].data = data ? hp(data) : nullptr;
            }
            o.a.params = o.params.data();
        }
    }
    return o;
}

s32 open_face(Library* l, const FT_Open_Args* args, s32 index, u32 aface)
{
    FT_Face face = nullptr;
    const FT_Error e = ::FT_Open_Face(l ? l->ft : nullptr, args, index, aface ? &face : nullptr);
    if (e || !face) return e;
    u32 g = 0;
    const FT_Error m = make_face(l, face, &g);
    if (m) return m;
    wr32(aface, g);
    return FT_Err_Ok;
}

s32 FT_Open_Face(PPUContext& ctx, u32 library, u32 args, s32 index, u32 aface)
{
    Scope sc(ctx);
    if ((!aface && index >= 0) || !args) return FT_Err_Invalid_Argument;
    Library* l = library_of(library);
    LibLock lk = lock(l);
    OpenArgs o = read_open_args(args);
    if (!l) return FT_Err_Invalid_Library_Handle;
    if (o.bad_driver) {
        PS_LOG_WARN(kChan, "FT_Open_Face: {:#x} is not a module", rd32(args + lay::oa_driver));
        return FT_Err_Invalid_Handle;
    }
    o.used = true; // FreeType closes the stream it was given (FT_Stream_Free)
    const s32 r = open_face(l, &o.a, index, aface);
    // FreeType gave a user's stream the library's memory (FT_Stream_New)
    if ((o.a.flags & FT_OPEN_STREAM) && rd32(args + lay::oa_stream))
        wr32(rd32(args + lay::oa_stream) + lay::st_memory, l->memory);
    return r;
}

s32 FT_New_Face(PPUContext& ctx, u32 library, u32 path, s32 index, u32 aface)
{
    Scope sc(ctx);
    if (!path) return FT_Err_Invalid_Argument;
    if (!aface && index >= 0) return FT_Err_Invalid_Argument;
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    LibLock lk = lock(l);
    FT_Open_Args a{};
    a.flags = FT_OPEN_PATHNAME;
    a.pathname = reinterpret_cast<char*>(hp(path));
    return open_face(l, &a, index, aface);
}

s32 FT_New_Memory_Face(PPUContext& ctx, u32 library, u32 base, s32 size, s32 index, u32 aface)
{
    Scope sc(ctx);
    if (!base) return FT_Err_Invalid_Argument;
    if (!aface && index >= 0) return FT_Err_Invalid_Argument;
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    LibLock lk = lock(l);
    FT_Open_Args a{};
    a.flags = FT_OPEN_MEMORY;
    a.memory_base = hp(base);
    a.memory_size = size;
    return open_face(l, &a, index, aface);
}

s32 FT_Done_Face(PPUContext& ctx, u32 face)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return FT_Err_Invalid_Face_Handle;
    LibLock lk = lock(f->lib);
    return ::FT_Done_Face(hface(f)); // the mirrors go as the host's objects do
}

s32 FT_Attach_File(PPUContext& ctx, u32 face, u32 path)
{
    Scope sc(ctx);
    if (!path) return FT_Err_Invalid_Argument;
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const FT_Error e = ::FT_Attach_File(hface(f), reinterpret_cast<const char*>(hp(path)));
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Attach_Stream(PPUContext& ctx, u32 face, u32 args)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return FT_Err_Invalid_Face_Handle;
    LibLock lk = lock(f->lib);
    OpenArgs o = read_open_args(args);
    o.used = true;
    const FT_Error e = ::FT_Attach_Stream(hface(f), &o.a);
    sync_face(f, false);
    return e;
}

// ------------------------------------------------------------------------------------------------- sizes

s32 FT_Select_Size(PPUContext& ctx, u32 face, s32 strike)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const FT_Error e = ::FT_Select_Size(hface(f), strike);
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Request_Size(PPUContext& ctx, u32 face, u32 req)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Size_RequestRec r{};
    if (req) {
        r.type = static_cast<FT_Size_Request_Type>(rd32(req + lay::sr_type));
        r.width = rds32(req + lay::sr_width);
        r.height = rds32(req + lay::sr_height);
        r.horiResolution = rd32(req + lay::sr_hori);
        r.vertResolution = rd32(req + lay::sr_vert);
    }
    const FT_Error e = ::FT_Request_Size(hface(f), req ? &r : nullptr);
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Set_Char_Size(PPUContext& ctx, u32 face, s32 width, s32 height, u32 hres, u32 vres)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const FT_Error e = ::FT_Set_Char_Size(hface(f), width, height, hres, vres);
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Set_Pixel_Sizes(PPUContext& ctx, u32 face, u32 width, u32 height)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const FT_Error e = ::FT_Set_Pixel_Sizes(hface(f), width, height);
    if (f) sync_face(f, false);
    return e;
}

s32 FT_New_Size(PPUContext& ctx, u32 face, u32 asize)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Size size = nullptr;
    FT_Error e = ::FT_New_Size(hface(f), asize ? &size : nullptr);
    if (f && asize) wr32(asize, 0);
    if (e || !size) return e;
    e = sync_face(f, false);
    if (e) {
        ::FT_Done_Size(size);
        return e;
    }
    wr32(asize, guest_of(size));
    return FT_Err_Ok;
}

s32 FT_Done_Size(PPUContext& ctx, u32 size)
{
    Scope sc(ctx);
    SizeMirror* s = size_of(size);
    if (!s) return FT_Err_Invalid_Size_Handle;
    LibLock lk = lock(s->lib);
    FaceMirror* f = face_mirror(hsize(s)->face);
    const FT_Error e = ::FT_Done_Size(hsize(s)); // its mirror goes with it
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Activate_Size(PPUContext& ctx, u32 size)
{
    Scope sc(ctx);
    SizeMirror* s = size_of(size);
    if (!s) return FT_Err_Bad_Argument;
    LibLock lk = lock(s->lib);
    const FT_Error e = ::FT_Activate_Size(hsize(s));
    if (FaceMirror* f = face_mirror(hsize(s)->face)) sync_face(f, false);
    return e;
}

// ------------------------------------------------------------------------------------------------- glyphs

s32 FT_Load_Glyph(PPUContext& ctx, u32 face, u32 index, s32 flags)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return FT_Err_Invalid_Face_Handle;
    LibLock lk = lock(f->lib);
    const FT_Error e = ::FT_Load_Glyph(hface(f), index, flags);
    const FT_Error s = sync_face(f, true);
    sync_library(f->lib); // a render may have changed the current renderer
    return e ? e : s;
}

s32 FT_Load_Char(PPUContext& ctx, u32 face, u32 code, s32 flags)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return FT_Err_Invalid_Face_Handle;
    LibLock lk = lock(f->lib);
    const FT_Error e = ::FT_Load_Char(hface(f), code, flags);
    const FT_Error s = sync_face(f, true);
    sync_library(f->lib);
    return e ? e : s;
}

void FT_Set_Transform(PPUContext& ctx, u32 face, u32 matrix, u32 delta)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return;
    LibLock lk = lock(f->lib);
    FT_Matrix m{};
    FT_Vector d{};
    if (matrix) m = read_matrix(matrix);
    if (delta) d = read_vector(delta);
    ::FT_Set_Transform(hface(f), matrix ? &m : nullptr, delta ? &d : nullptr);
}

s32 FT_Render_Glyph(PPUContext& ctx, u32 slot, u32 mode)
{
    Scope sc(ctx);
    SlotMirror* s = slot_of(slot);
    if (!s) return FT_Err_Invalid_Argument;
    LibLock lk = lock(s->lib);
    FT_GlyphSlot hs = hslot(s);
    read_back_slot(s);
    const bool was_bitmap = hs->format == FT_GLYPH_FORMAT_BITMAP;
    const FT_Error e = ::FT_Render_Glyph(hs, static_cast<FT_Render_Mode>(mode));
    FT_Error r = FT_Err_Ok;
    if (!was_bitmap) r = sync_slot(s);
    sync_library(s->lib);
    return e ? e : r;
}

s32 FT_GlyphSlot_Own_Bitmap(PPUContext& ctx, u32 slot)
{
    // src/base/ftsynth.c: a bitmap the slot no longer owns (FT_Get_Glyph took it) is copied for it.
    Scope sc(ctx);
    SlotMirror* s = slot_of(slot);
    if (!s) return FT_Err_Ok;
    LibLock lk = lock(s->lib);
    if (rd32(slot + lay::slot_format) != FT_GLYPH_FORMAT_BITMAP || s->own_buffer) return FT_Err_Ok;
    const u32 gb = slot + lay::slot_bitmap;
    const u32 src = rd32(gb + lay::bm_buffer);
    if (src) {
        const s32 pitch = rds32(gb + lay::bm_pitch);
        const u32 n = static_cast<u32>((pitch < 0 ? -pitch : pitch) * rds32(gb + lay::bm_rows));
        FT_Error error = FT_Err_Ok;
        const u32 copy = g_qalloc(s->memory, n, error);
        if (error) return error;
        if (n) std::memmove(hp(copy), hp(src), n);
        wr32(gb + lay::bm_buffer, copy);
        s->buffer = copy;
    }
    s->own_buffer = true;
    s->own_blocks();
    return FT_Err_Ok;
}

s32 FT_Get_SubGlyph_Info(PPUContext& ctx, u32 glyph, u32 sub_index, u32 p_index, u32 p_flags, u32 p_arg1,
    u32 p_arg2, u32 p_transform)
{
    Scope sc(ctx);
    SlotMirror* s = slot_of(glyph);
    LibLock lk = lock(s ? s->lib : nullptr);
    FT_Int index = 0, arg1 = 0, arg2 = 0;
    FT_UInt flags = 0;
    FT_Matrix m{};
    FT_GlyphSlot hs = hslot(s);
    const bool fills = hs && hs->format == FT_GLYPH_FORMAT_COMPOSITE && sub_index < hs->num_subglyphs;
    const FT_Error e = ::FT_Get_SubGlyph_Info(hs, sub_index, &index, &flags, &arg1, &arg2, &m);
    if (fills) {
        if (p_index) wr32(p_index, static_cast<u32>(index));
        if (p_flags) wr32(p_flags, flags);
        if (p_arg1) wr32(p_arg1, static_cast<u32>(arg1));
        if (p_arg2) wr32(p_arg2, static_cast<u32>(arg2));
        if (p_transform) {
            wr32(p_transform, m.xx);
            wr32(p_transform + 4, m.xy);
            wr32(p_transform + 8, m.yx);
            wr32(p_transform + 12, m.yy);
        }
    }
    return e;
}

// ------------------------------------------------------------------------------------------------- charmaps, kerning

s32 FT_Select_Charmap(PPUContext& ctx, u32 face, u32 encoding)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const FT_Error e = ::FT_Select_Charmap(hface(f), static_cast<FT_Encoding>(encoding));
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Set_Charmap(PPUContext& ctx, u32 face, u32 charmap)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    Mirror* c = by_guest(charmap, Mirror::Kind::charmap);
    // a pointer that is no charmap of the face's is not found among them (FT_Err_Invalid_Argument)
    static FT_CharMapRec none{};
    const FT_Error e = ::FT_Set_Charmap(hface(f), c ? hcmap(c) : &none);
    if (f) sync_face(f, false);
    return e;
}

s32 FT_Get_Charmap_Index(PPUContext& ctx, u32 charmap)
{
    Scope sc(ctx);
    Mirror* c = charmap_of(charmap);
    if (!c) return -1;
    LibLock lk = lock(c->lib);
    return ::FT_Get_Charmap_Index(hcmap(c));
}

u32 FT_Get_CMap_Language_ID(PPUContext& ctx, u32 charmap)
{
    Scope sc(ctx);
    Mirror* c = charmap_of(charmap);
    LibLock lk = lock(c ? c->lib : nullptr);
    return static_cast<u32>(::FT_Get_CMap_Language_ID(hcmap(c)));
}

u32 FT_Get_Char_Index(PPUContext& ctx, u32 face, u32 code)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    return ::FT_Get_Char_Index(hface(f), code);
}

u32 FT_Get_First_Char(PPUContext& ctx, u32 face, u32 agindex)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_UInt gi = 0;
    const FT_ULong r = ::FT_Get_First_Char(hface(f), &gi);
    if (agindex) wr32(agindex, gi);
    return static_cast<u32>(r);
}

u32 FT_Get_Next_Char(PPUContext& ctx, u32 face, u32 code, u32 agindex)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_UInt gi = 0;
    const FT_ULong r = ::FT_Get_Next_Char(hface(f), code, &gi);
    if (agindex) wr32(agindex, gi);
    return static_cast<u32>(r);
}

u32 FT_Get_Name_Index(PPUContext& ctx, u32 face, u32 name)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f || !name) return 0;
    LibLock lk = lock(f->lib);
    std::string n = gstr(name);
    return ::FT_Get_Name_Index(hface(f), n.data());
}

s32 FT_Get_Kerning(PPUContext& ctx, u32 face, u32 left, u32 right, u32 mode, u32 akerning)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Vector k{};
    const FT_Error e = ::FT_Get_Kerning(hface(f), left, right, mode, akerning ? &k : nullptr);
    if (f && akerning) write_vector(akerning, k);
    return e;
}

s32 FT_Get_Track_Kerning(PPUContext& ctx, u32 face, s32 point_size, s32 degree, u32 akerning)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Fixed k = 0;
    const FT_Error e = ::FT_Get_Track_Kerning(hface(f), point_size, degree, akerning ? &k : nullptr);
    if (f && akerning && !e) wr32(akerning, static_cast<u32>(k));
    return e;
}

s32 FT_Get_Glyph_Name(PPUContext& ctx, u32 face, u32 index, u32 buffer, u32 max)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Face hf = hface(f);
    if (hf && index >= static_cast<u32>(hf->num_glyphs) && !FT_IS_SFNT(hf)) {
        // FreeType 2.2.1 lets index == num_glyphs through to the PostScript drivers, which read past their name
        // table (Sony's reads whatever follows it in the game's memory): refused here
        PS_LOG_WARN(kChan, "FT_Get_Glyph_Name: glyph {} of {} has no name", index, hf->num_glyphs);
        if (buffer && max > 0) wr8(buffer, 0);
        return FT_Err_Invalid_Argument;
    }
    return ::FT_Get_Glyph_Name(hf, index, buffer ? hp(buffer) : nullptr, max);
}

u32 FT_Get_Postscript_Name(PPUContext& ctx, u32 face)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return 0;
    LibLock lk = lock(f->lib);
    const char* name = ::FT_Get_Postscript_Name(hface(f));
    if (!name) return 0;
    if (!f->ps_name) {
        FT_Error error = FT_Err_Ok;
        f->ps_name = g_strdup(f->memory, name, error);
        if (f->ps_name) f->blocks.push_back(f->ps_name);
    }
    return f->ps_name;
}

// ------------------------------------------------------------------------------------------------- SFNT

// A string the face shows the guest (from its tables), copied once into the face's memory.
u32 face_string(FaceMirror* f, const std::string& key, const char* s, usize len)
{
    if (!s) return 0;
    if (auto it = f->strings.find(key); it != f->strings.end()) return it->second;
    FT_Error error = FT_Err_Ok;
    const u32 g = g_qalloc(f->memory, static_cast<s64>(len) + 1, error);
    if (error) return 0;
    std::memcpy(hp(g), s, len);
    wr8(g + static_cast<u32>(len), 0);
    f->blocks.push_back(g);
    f->strings[key] = g;
    return g;
}

u32 copy_sfnt_table(FaceMirror* f, u32 tag, const void* t)
{
    FT_Error error = FT_Err_Ok;
    static constexpr u32 sizes[] = {56, 32, 100, 44, 44, 32, 56};
    const u32 g = g_alloc(f->memory, sizes[tag], error);
    if (error) return 0;
    switch (tag) {
    case ft_sfnt_head: {
        const auto* h = static_cast<const TT_Header*>(t);
        wr32(g + 0, h->Table_Version);
        wr32(g + 4, h->Font_Revision);
        wr32(g + 8, h->CheckSum_Adjust);
        wr32(g + 12, h->Magic_Number);
        wr16(g + 16, h->Flags);
        wr16(g + 18, h->Units_Per_EM);
        wr32(g + 20, h->Created[0]);
        wr32(g + 24, h->Created[1]);
        wr32(g + 28, h->Modified[0]);
        wr32(g + 32, h->Modified[1]);
        wr16(g + 36, static_cast<u16>(h->xMin));
        wr16(g + 38, static_cast<u16>(h->yMin));
        wr16(g + 40, static_cast<u16>(h->xMax));
        wr16(g + 42, static_cast<u16>(h->yMax));
        wr16(g + 44, h->Mac_Style);
        wr16(g + 46, h->Lowest_Rec_PPEM);
        wr16(g + 48, static_cast<u16>(h->Font_Direction));
        wr16(g + 50, static_cast<u16>(h->Index_To_Loc_Format));
        wr16(g + 52, static_cast<u16>(h->Glyph_Data_Format));
        break;
    }
    case ft_sfnt_maxp: {
        const auto* m = static_cast<const TT_MaxProfile*>(t);
        wr32(g + 0, m->version);
        const FT_UShort v[] = {m->numGlyphs, m->maxPoints, m->maxContours, m->maxCompositePoints,
            m->maxCompositeContours, m->maxZones, m->maxTwilightPoints, m->maxStorage, m->maxFunctionDefs,
            m->maxInstructionDefs, m->maxStackElements, m->maxSizeOfInstructions, m->maxComponentElements,
            m->maxComponentDepth};
        for (u32 i = 0; i < 14; i++) wr16(g + 4 + 2 * i, v[i]);
        break;
    }
    case ft_sfnt_os2: {
        const auto* o = static_cast<const TT_OS2*>(t);
        const FT_UShort a[] = {o->version, static_cast<FT_UShort>(o->xAvgCharWidth), o->usWeightClass,
            o->usWidthClass, static_cast<FT_UShort>(o->fsType), static_cast<FT_UShort>(o->ySubscriptXSize),
            static_cast<FT_UShort>(o->ySubscriptYSize), static_cast<FT_UShort>(o->ySubscriptXOffset),
            static_cast<FT_UShort>(o->ySubscriptYOffset), static_cast<FT_UShort>(o->ySuperscriptXSize),
            static_cast<FT_UShort>(o->ySuperscriptYSize), static_cast<FT_UShort>(o->ySuperscriptXOffset),
            static_cast<FT_UShort>(o->ySuperscriptYOffset), static_cast<FT_UShort>(o->yStrikeoutSize),
            static_cast<FT_UShort>(o->yStrikeoutPosition), static_cast<FT_UShort>(o->sFamilyClass)};
        for (u32 i = 0; i < 16; i++) wr16(g + 2 * i, a[i]);
        for (u32 i = 0; i < 10; i++) wr8(g + 32 + i, o->panose[i]);
        wr32(g + 44, o->ulUnicodeRange1);
        wr32(g + 48, o->ulUnicodeRange2);
        wr32(g + 52, o->ulUnicodeRange3);
        wr32(g + 56, o->ulUnicodeRange4);
        for (u32 i = 0; i < 4; i++) wr8(g + 60 + i, static_cast<u8>(o->achVendID[i]));
        const FT_UShort b[] = {o->fsSelection, o->usFirstCharIndex, o->usLastCharIndex,
            static_cast<FT_UShort>(o->sTypoAscender), static_cast<FT_UShort>(o->sTypoDescender),
            static_cast<FT_UShort>(o->sTypoLineGap), o->usWinAscent, o->usWinDescent};
        for (u32 i = 0; i < 8; i++) wr16(g + 64 + 2 * i, b[i]);
        wr32(g + 80, o->ulCodePageRange1);
        wr32(g + 84, o->ulCodePageRange2);
        const FT_UShort c[] = {static_cast<FT_UShort>(o->sxHeight), static_cast<FT_UShort>(o->sCapHeight),
            o->usDefaultChar, o->usBreakChar, o->usMaxContext};
        for (u32 i = 0; i < 5; i++) wr16(g + 88 + 2 * i, c[i]);
        break;
    }
    case ft_sfnt_hhea:
    case ft_sfnt_vhea: {
        // TT_HoriHeader and TT_VertHeader share their layout
        const auto* h = static_cast<const TT_HoriHeader*>(t);
        wr32(g + 0, h->Version);
        const FT_UShort a[] = {static_cast<FT_UShort>(h->Ascender), static_cast<FT_UShort>(h->Descender),
            static_cast<FT_UShort>(h->Line_Gap), h->advance_Width_Max,
            static_cast<FT_UShort>(h->min_Left_Side_Bearing), static_cast<FT_UShort>(h->min_Right_Side_Bearing),
            static_cast<FT_UShort>(h->xMax_Extent), static_cast<FT_UShort>(h->caret_Slope_Rise),
            static_cast<FT_UShort>(h->caret_Slope_Run), static_cast<FT_UShort>(h->caret_Offset)};
        for (u32 i = 0; i < 10; i++) wr16(g + 4 + 2 * i, a[i]);
        for (u32 i = 0; i < 4; i++) wr16(g + 24 + 2 * i, static_cast<u16>(h->Reserved[i]));
        wr16(g + 32, static_cast<u16>(h->metric_Data_Format));
        wr16(g + 34, h->number_Of_HMetrics);
        // the metrics FreeType 2.2.1 loads with FT_CONFIG_OPTION_OLD_INTERNALS ({u16 advance, s16 bearing} each, then
        // the bearings of the glyphs past them), in the face's memory
        const auto* maxp = static_cast<const TT_MaxProfile*>(::FT_Get_Sfnt_Table(hface(f), ft_sfnt_maxp));
        const u32 nl = h->number_Of_HMetrics;
        const s32 ns = maxp ? std::max<s32>(static_cast<s32>(maxp->numGlyphs) - static_cast<s32>(nl), 0) : 0;
        u32 longs = 0, shorts = 0;
        if (h->long_metrics && nl) {
            longs = g_qalloc(f->memory, 4 * nl, error);
            if (error) return 0;
            f->blocks.push_back(longs);
            const auto* lm = static_cast<const TT_LongMetricsRec*>(h->long_metrics);
            for (u32 i = 0; i < nl; i++) {
                wr16(longs + 4 * i, lm[i].advance);
                wr16(longs + 4 * i + 2, static_cast<u16>(lm[i].bearing));
            }
        }
        if (h->short_metrics && ns) {
            shorts = g_qalloc(f->memory, 2 * ns, error);
            if (error) return 0;
            f->blocks.push_back(shorts);
            const auto* sm = static_cast<const TT_ShortMetrics*>(h->short_metrics);
            for (s32 i = 0; i < ns; i++) wr16(shorts + 2 * static_cast<u32>(i), static_cast<u16>(sm[i]));
        }
        wr32(g + 36, longs);
        wr32(g + 40, shorts);
        break;
    }
    case ft_sfnt_post: {
        const auto* p = static_cast<const TT_Postscript*>(t);
        wr32(g + 0, p->FormatType);
        wr32(g + 4, p->italicAngle);
        wr16(g + 8, static_cast<u16>(p->underlinePosition));
        wr16(g + 10, static_cast<u16>(p->underlineThickness));
        wr32(g + 12, p->isFixedPitch);
        wr32(g + 16, p->minMemType42);
        wr32(g + 20, p->maxMemType42);
        wr32(g + 24, p->minMemType1);
        wr32(g + 28, p->maxMemType1);
        break;
    }
    case ft_sfnt_pclt: {
        const auto* p = static_cast<const TT_PCLT*>(t);
        wr32(g + 0, p->Version);
        wr32(g + 4, p->FontNumber);
        wr16(g + 8, p->Pitch);
        wr16(g + 10, p->xHeight);
        wr16(g + 12, p->Style);
        wr16(g + 14, p->TypeFamily);
        wr16(g + 16, p->CapHeight);
        wr16(g + 18, p->SymbolSet);
        for (u32 i = 0; i < 16; i++) wr8(g + 20 + i, static_cast<u8>(p->TypeFace[i]));
        for (u32 i = 0; i < 8; i++) wr8(g + 36 + i, static_cast<u8>(p->CharacterComplement[i]));
        for (u32 i = 0; i < 6; i++) wr8(g + 44 + i, static_cast<u8>(p->FileName[i]));
        wr8(g + 50, static_cast<u8>(p->StrokeWeight));
        wr8(g + 51, static_cast<u8>(p->WidthType));
        wr8(g + 52, p->SerifStyle);
        wr8(g + 53, p->Reserved);
        break;
    }
    default: break;
    }
    f->blocks.push_back(g);
    return g;
}

u32 FT_Get_Sfnt_Table(PPUContext& ctx, u32 face, u32 tag)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return 0;
    LibLock lk = lock(f->lib);
    const void* t = ::FT_Get_Sfnt_Table(hface(f), static_cast<FT_Sfnt_Tag>(tag));
    if (!t || tag > ft_sfnt_pclt) return 0;
    if (auto it = f->sfnt_tables.find(tag); it != f->sfnt_tables.end()) return it->second;
    const u32 g = copy_sfnt_table(f, tag, t);
    if (g) f->sfnt_tables[tag] = g;
    return g;
}

s32 FT_Load_Sfnt_Table(PPUContext& ctx, u32 face, u32 tag, s32 offset, u32 buffer, u32 length)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_ULong len = length ? rd32(length) : 0;
    if (f && !buffer && !(length && len == 0) && FT_IS_SFNT(hface(f))) {
        PS_LOG_WARN(kChan, "FT_Load_Sfnt_Table: no buffer to read into");
        return FT_Err_Invalid_Argument;
    }
    const FT_Error e = ::FT_Load_Sfnt_Table(hface(f), tag, offset, buffer ? hp(buffer) : nullptr, length ? &len : nullptr);
    if (length) wr32(length, static_cast<u32>(len));
    return e;
}

s32 FT_Sfnt_Table_Info(PPUContext& ctx, u32 face, u32 index, u32 tag, u32 length)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_ULong t = tag ? rd32(tag) : 0, l = length ? rd32(length) : 0;
    const FT_Error e = ::FT_Sfnt_Table_Info(hface(f), index, tag ? &t : nullptr, length ? &l : nullptr);
    if (tag) wr32(tag, static_cast<u32>(t));
    if (length) wr32(length, static_cast<u32>(l));
    return e;
}

u32 FT_Get_Sfnt_Name_Count(PPUContext& ctx, u32 face)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    return ::FT_Get_Sfnt_Name_Count(hface(f));
}

s32 FT_Get_Sfnt_Name(PPUContext& ctx, u32 face, u32 idx, u32 aname)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_SfntName n{};
    const FT_Error e = ::FT_Get_Sfnt_Name(hface(f), idx, aname ? &n : nullptr);
    if (e || !aname) return e;
    wr16(aname + lay::sn_platform_id, n.platform_id);
    wr16(aname + lay::sn_encoding_id, n.encoding_id);
    wr16(aname + lay::sn_language_id, n.language_id);
    wr16(aname + lay::sn_name_id, n.name_id);
    u32 s = 0;
    if (n.string) {
        if (auto it = f->sfnt_names.find(idx); it != f->sfnt_names.end()) {
            s = it->second;
        } else {
            FT_Error error = FT_Err_Ok;
            s = g_qalloc(f->memory, n.string_len ? n.string_len : 1, error);
            if (error) return error;
            std::memcpy(hp(s), n.string, n.string_len);
            f->blocks.push_back(s);
            f->sfnt_names[idx] = s;
        }
    }
    wr32(aname + lay::sn_string, s);
    wr32(aname + lay::sn_string_len, n.string_len);
    return e;
}

// ------------------------------------------------------------------------------------------------- other formats

s32 FT_Get_PS_Font_Info(PPUContext& ctx, u32 face, u32 info)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    PS_FontInfoRec r{};
    const FT_Error e = ::FT_Get_PS_Font_Info(hface(f), info ? &r : nullptr);
    if (e || !info) return e;
    std::memset(hp(info), 0, 32); // a structure copy in Sony's: its padding too
    auto str = [&](const char* key, const FT_String* s) { return s ? face_string(f, key, s, std::strlen(s)) : 0; };
    wr32(info + 0, str("version", r.version));
    wr32(info + 4, str("notice", r.notice));
    wr32(info + 8, str("full_name", r.full_name));
    wr32(info + 12, str("family_name", r.family_name));
    wr32(info + 16, str("weight", r.weight));
    wr32(info + 20, r.italic_angle);
    wr8(info + 24, r.is_fixed_pitch);
    wr16(info + 26, static_cast<u16>(r.underline_position));
    wr16(info + 28, r.underline_thickness);
    return e;
}

s32 FT_Get_PS_Font_Private(PPUContext& ctx, u32 face, u32 priv)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    PS_PrivateRec r{};
    const FT_Error e = ::FT_Get_PS_Font_Private(hface(f), priv ? &r : nullptr);
    if (e || !priv) return e;
    std::memset(hp(priv), 0, 196);
    // (FreeType 2.2.1 stores the count of StdHW, StdVW and MinFeature in the record's first byte, a bug: on the PPU
    // the high byte of unique_id, on the host its low byte; unique_id is shown as the host has it)
    wr32(priv + 0, static_cast<u32>(r.unique_id));
    wr32(priv + 4, static_cast<u32>(r.lenIV));
    wr8(priv + 8, r.num_blue_values);
    wr8(priv + 9, r.num_other_blues);
    wr8(priv + 10, r.num_family_blues);
    wr8(priv + 11, r.num_family_other_blues);
    for (u32 i = 0; i < 14; i++) wr16(priv + 12 + 2 * i, static_cast<u16>(r.blue_values[i]));
    for (u32 i = 0; i < 10; i++) wr16(priv + 40 + 2 * i, static_cast<u16>(r.other_blues[i]));
    for (u32 i = 0; i < 14; i++) wr16(priv + 60 + 2 * i, static_cast<u16>(r.family_blues[i]));
    for (u32 i = 0; i < 10; i++) wr16(priv + 88 + 2 * i, static_cast<u16>(r.family_other_blues[i]));
    wr32(priv + 108, r.blue_scale);
    wr32(priv + 112, static_cast<u32>(r.blue_shift));
    wr32(priv + 116, static_cast<u32>(r.blue_fuzz));
    wr16(priv + 120, r.standard_width[0]);
    wr16(priv + 122, r.standard_height[0]);
    wr8(priv + 124, r.num_snap_widths);
    wr8(priv + 125, r.num_snap_heights);
    wr8(priv + 126, r.force_bold);
    wr8(priv + 127, r.round_stem_up);
    for (u32 i = 0; i < 13; i++) wr16(priv + 128 + 2 * i, static_cast<u16>(r.snap_widths[i]));
    for (u32 i = 0; i < 13; i++) wr16(priv + 154 + 2 * i, static_cast<u16>(r.snap_heights[i]));
    wr32(priv + 180, r.expansion_factor);
    wr32(priv + 184, r.language_group);
    wr32(priv + 188, r.password);
    wr16(priv + 192, static_cast<u16>(r.min_feature[0]));
    wr16(priv + 194, static_cast<u16>(r.min_feature[1]));
    return e;
}

s32 FT_Has_PS_Glyph_Names(PPUContext& ctx, u32 face)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    return ::FT_Has_PS_Glyph_Names(hface(f));
}

s32 FT_Get_WinFNT_Header(PPUContext& ctx, u32 face, u32 header)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_WinFNT_HeaderRec h{};
    const FT_Error e = ::FT_Get_WinFNT_Header(hface(f), header ? &h : nullptr);
    if (e || !header) return e;
    std::memset(hp(header), 0, 156);
    wr16(header + 0, h.version);
    wr32(header + 4, h.file_size);
    for (u32 i = 0; i < 60; i++) wr8(header + 8 + i, h.copyright[i]);
    const FT_UShort a[] = {h.file_type, h.nominal_point_size, h.vertical_resolution, h.horizontal_resolution,
        h.ascent, h.internal_leading, h.external_leading};
    for (u32 i = 0; i < 7; i++) wr16(header + 68 + 2 * i, a[i]);
    wr8(header + 82, h.italic);
    wr8(header + 83, h.underline);
    wr8(header + 84, h.strike_out);
    wr16(header + 86, h.weight);
    wr8(header + 88, h.charset);
    wr16(header + 90, h.pixel_width);
    wr16(header + 92, h.pixel_height);
    wr8(header + 94, h.pitch_and_family);
    wr16(header + 96, h.avg_width);
    wr16(header + 98, h.max_width);
    wr8(header + 100, h.first_char);
    wr8(header + 101, h.last_char);
    wr8(header + 102, h.default_char);
    wr8(header + 103, h.break_char);
    wr16(header + 104, h.bytes_per_row);
    wr32(header + 108, h.device_offset);
    wr32(header + 112, h.face_name_offset);
    wr32(header + 116, h.bits_pointer);
    wr32(header + 120, h.bits_offset);
    wr8(header + 124, h.reserved);
    wr32(header + 128, h.flags);
    wr16(header + 132, h.A_space);
    wr16(header + 134, h.B_space);
    wr16(header + 136, h.C_space);
    wr16(header + 138, h.color_table_offset);
    // reserved1: FreeType copies the file's 16 bytes into it as they are (FT_FRAME_BYTES)
    std::memcpy(hp(header + 140), reinterpret_cast<const u8*>(h.reserved1), 16);
    return e;
}

// The format names FreeType 2.2.1's drivers give (src/base/ftxf86.c), as process-wide guest strings.
struct StringsState {
    std::mutex mutex;
    std::map<std::string, u32> strings; // format names
    u32 ex_buffers = 0;                 // cellFreeType2Ex's
};

u32 format_string(const char* s)
{
    StringsState& st = HleProcess::current()->state<StringsState>();
    std::lock_guard lk(st.mutex);
    auto& strings = st.strings;
    const std::string key(s);
    if (auto it = strings.find(key); it != strings.end()) return it->second;
    const u32 n = static_cast<u32>(std::strlen(s)) + 1;
    const u32 a = HleProcess::current()->main_memory().alloc(n, 0x10);
    if (!a) return 0;
    std::memcpy(hp(a), s, n);
    strings[key] = a;
    return a;
}

u32 FT_Get_X11_Font_Format(PPUContext& ctx, u32 face)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const char* s = ::FT_Get_X11_Font_Format(hface(f));
    return s ? format_string(s) : 0;
}

s32 FT_Get_BDF_Charset_ID(PPUContext& ctx, u32 face, u32 aencoding, u32 aregistry)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    const char* enc = nullptr;
    const char* reg = nullptr;
    const FT_Error e = ::FT_Get_BDF_Charset_ID(hface(f), &enc, &reg);
    if (aencoding) wr32(aencoding, f && enc ? face_string(f, "bdf_encoding", enc, std::strlen(enc)) : 0);
    if (aregistry) wr32(aregistry, f && reg ? face_string(f, "bdf_registry", reg, std::strlen(reg)) : 0);
    return e;
}

s32 FT_Get_BDF_Property(PPUContext& ctx, u32 face, u32 name, u32 aproperty)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    BDF_PropertyRec p{};
    const std::string n = gstr(name);
    const FT_Error e = ::FT_Get_BDF_Property(hface(f), name ? n.c_str() : nullptr, &p);
    if (aproperty) {
        wr32(aproperty, p.type);
        u32 v = 0;
        if (p.type == BDF_PROPERTY_TYPE_ATOM)
            v = f && p.u.atom ? face_string(f, "bdf:" + n, p.u.atom, std::strlen(p.u.atom)) : 0;
        else if (p.type == BDF_PROPERTY_TYPE_INTEGER) v = static_cast<u32>(p.u.integer);
        else if (p.type == BDF_PROPERTY_TYPE_CARDINAL) v = p.u.cardinal;
        wr32(aproperty + 4, v);
    }
    return e;
}

s32 FT_Get_PFR_Metrics(PPUContext& ctx, u32 face, u32 aoutline, u32 ametrics, u32 axscale, u32 ayscale)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_UInt o = 0, m = 0;
    FT_Fixed x = 0, y = 0;
    const FT_Error e = ::FT_Get_PFR_Metrics(hface(f), &o, &m, &x, &y);
    if (f) {
        if (aoutline) wr32(aoutline, o);
        if (ametrics) wr32(ametrics, m);
        if (axscale) wr32(axscale, x);
        if (ayscale) wr32(ayscale, y);
    }
    return e;
}

s32 FT_Get_PFR_Kerning(PPUContext& ctx, u32 face, u32 left, u32 right, u32 avector)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Vector v{};
    const FT_Error e = ::FT_Get_PFR_Kerning(hface(f), left, right, avector ? &v : nullptr);
    if (f && avector) write_vector(avector, v);
    return e;
}

s32 FT_Get_PFR_Advance(PPUContext& ctx, u32 face, u32 gindex, u32 aadvance)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Pos a = 0;
    const FT_Error e = ::FT_Get_PFR_Advance(hface(f), gindex, &a);
    if (!e && aadvance) wr32(aadvance, a);
    return e;
}

s32 FT_Get_Multi_Master(PPUContext& ctx, u32 face, u32 amaster)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Multi_Master mm{};
    const FT_Error e = ::FT_Get_Multi_Master(hface(f), &mm);
    if (e || !amaster) return e;
    wr32(amaster + 0, mm.num_axis);
    wr32(amaster + 4, mm.num_designs);
    for (u32 i = 0; i < 4; i++) {
        const FT_MM_Axis& a = mm.axis[i];
        const u32 at = amaster + 8 + 12 * i;
        wr32(at + 0, a.name ? face_string(f, std::format("mm_axis{}", i), a.name, std::strlen(a.name)) : 0);
        wr32(at + 4, a.minimum);
        wr32(at + 8, a.maximum);
    }
    return e;
}

// FT_MM_Var as one guest block through the face's memory (the caller frees it), laid out as FreeType's drivers lay
// theirs: the record, the axes, the named styles, their coordinates.
s32 FT_Get_MM_Var(PPUContext& ctx, u32 face, u32 amaster)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_MM_Var* mm = nullptr;
    const FT_Error e = ::FT_Get_MM_Var(hface(f), &mm);
    if (e || !mm) return e;
    const u32 na = mm->num_axis, ns = mm->num_namedstyles;
    const u32 size = 20 + 24 * na + 8 * ns + 4 * na * ns;
    FT_Error error = FT_Err_Ok;
    const u32 g = g_alloc(f->memory, size, error);
    if (!error) {
        const u32 axes = g + 20, styles = axes + 24 * na, coords = styles + 8 * ns;
        wr32(g + 0, na);
        wr32(g + 4, mm->num_designs);
        wr32(g + 8, ns);
        wr32(g + 12, na ? axes : 0);
        wr32(g + 16, ns ? styles : 0);
        for (u32 i = 0; i < na; i++) {
            const FT_Var_Axis& a = mm->axis[i];
            const u32 at = axes + 24 * i;
            wr32(at + 0, a.name ? face_string(f, std::format("var_axis{}", i), a.name, std::strlen(a.name)) : 0);
            wr32(at + 4, a.minimum);
            wr32(at + 8, a.def);
            wr32(at + 12, a.maximum);
            wr32(at + 16, a.tag);
            wr32(at + 20, a.strid);
        }
        for (u32 i = 0; i < ns; i++) {
            wr32(styles + 8 * i, coords + 4 * na * i);
            wr32(styles + 8 * i + 4, mm->namedstyle[i].strid);
            for (u32 k = 0; k < na; k++) wr32(coords + 4 * (na * i + k), mm->namedstyle[i].coords[k]);
        }
        if (amaster) wr32(amaster, g);
    }
    FT_Memory memory = hface(f)->memory;
    memory->free(memory, mm);
    return error;
}

template <class T>
std::vector<T> read_array(u32 a, u32 n)
{
    std::vector<T> v(n);
    for (u32 i = 0; i < n && a; i++) v[i] = static_cast<T>(rds32(a + 4 * i));
    return v;
}

s32 FT_Set_MM_Design_Coordinates(PPUContext& ctx, u32 face, u32 num, u32 coords)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    auto c = read_array<FT_Long>(coords, num);
    return ::FT_Set_MM_Design_Coordinates(hface(f), num, coords ? c.data() : nullptr);
}

s32 FT_Set_MM_Blend_Coordinates(PPUContext& ctx, u32 face, u32 num, u32 coords)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    auto c = read_array<FT_Fixed>(coords, num);
    return ::FT_Set_MM_Blend_Coordinates(hface(f), num, coords ? c.data() : nullptr);
}

s32 FT_Set_Var_Design_Coordinates(PPUContext& ctx, u32 face, u32 num, u32 coords)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    auto c = read_array<FT_Fixed>(coords, num);
    return ::FT_Set_Var_Design_Coordinates(hface(f), num, coords ? c.data() : nullptr);
}

s32 FT_Set_Var_Blend_Coordinates(PPUContext& ctx, u32 face, u32 num, u32 coords)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    auto c = read_array<FT_Fixed>(coords, num);
    return ::FT_Set_Var_Blend_Coordinates(hface(f), num, coords ? c.data() : nullptr);
}

// Sony's libfreetype has no OpenType validator module (as FreeType's default list): FT_OpenType_Validate finds no
// service.
s32 FT_OpenType_Validate(PPUContext& ctx, u32 face, u32 flags, u32 base, u32 gdef, u32 gpos, u32 gsub, u32 jstf)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    LibLock lk = lock(f ? f->lib : nullptr);
    FT_Bytes t[5] = {};
    const FT_Error e = ::FT_OpenType_Validate(hface(f), flags, base ? &t[0] : nullptr, gdef ? &t[1] : nullptr,
        gpos ? &t[2] : nullptr, gsub ? &t[3] : nullptr, jstf ? &t[4] : nullptr);
    for (const FT_Bytes b : t)
        if (b) PS_LOG_ERROR(kChan, "FT_OpenType_Validate: a validated table cannot be shown");
    return e;
}

void FT_OpenType_Free(PPUContext& ctx, u32 face, u32 table)
{
    Scope sc(ctx);
    FaceMirror* f = face_of(face);
    if (!f) return;
    g_free(f->memory, table);
}

// ------------------------------------------------------------------------------------------------- Sony's additions

// cellFreeType2Ex(arg): the system's way to its own fonts. `arg` is {3247 or 4043, -, 0xCF200000, list}, the list
// {count, entries}, each entry {font type, char** out}: for a system font's type (CELL_FONT_TYPE_*) *out becomes a
// path FT_New_Face opens ("/:dev_font:/<name>", its NUL, 0xDE, then the font's real path XORed with 0xF2 and an
// XORed NUL) in a static buffer of the library's, one per font; any other type gets NULL. A bad argument is
// 0x805400F2. (Read from Sony's libfreetype; the paths are the firmware's font files, as RPCS3's cellFont.cpp names
// them.)
struct SystemFont {
    u32 type;
    const char* name; // the visible part
    const char* file; // in /dev_flash/data/font, after "SCE-PS3-"
};
constexpr SystemFont kSystemFonts[] = {
    {32, "MT-R-LATIN", "MT-R-LATIN"}, {34, "MT-B-LATIN", "MT-B-LATIN"}, {36, "MT-I-LATIN", "MT-I-LATIN"},
    {38, "MT-BI-LATIN", "MT-BI-LATIN"}, {0, "RD-R-LATIN", "RD-R-LATIN"}, {1, "RD-L-LATIN", "RD-L-LATIN"},
    {2, "RD-B-LATIN", "RD-B-LATIN"}, {4, "RD-I-LATIN", "RD-I-LATIN"}, {5, "RD-LI-LATIN", "RD-LI-LATIN"},
    {6, "RD-BI-LATIN", "RD-BI-LATIN"}, {24, "RD-L-LATIN2", "RD-L-LATIN2"}, {25, "RD-R-LATIN2", "RD-R-LATIN2"},
    {26, "RD-B-LATIN2", "RD-B-LATIN2"}, {8, "NR-R-JPN", "NR-R-JPN"}, {9, "NR-L-JPN", "NR-L-JPN"},
    {10, "NR-B-JPN", "NR-B-JPN"}, {72, "SR-R-JPN", "SR-R-JPN"}, {12, "YG-R-KOR", "YG-R-KOR"},
    {13, "YG-L-KOR", "YG-L-KOR"}, {14, "YG-B-KOR", "YG-B-KOR"}, {20, "DH-R-CGB", "DH-R-CGB"},
    {64, "SR-R-LATIN", "SR-R-LATIN"}, {65, "SR-R-LATIN", "SR-R-LATIN2"}, {67, "VR-R-LATIN", "VR-R-LATIN"},
    {68, "VR-R-LATIN", "VR-R-LATIN2"},
};
constexpr u32 kExSlot = 0x80;

s32 cellFreeType2Ex(PPUContext& ctx, u32 arg)
{
    constexpr s32 kBad = static_cast<s32>(0x805400F2);
    if (!arg) return kBad;
    Scope sc(ctx);
    const u32 magic = rd32(arg);
    if ((magic != 3247 && magic != 4043) || rd32(arg + 8) != 0xCF200000) return kBad;
    u32 area = 0;
    {
        StringsState& st = HleProcess::current()->state<StringsState>();
        std::lock_guard lk(st.mutex);
        u32& b = st.ex_buffers;
        if (!b) {
            b = HleProcess::current()->main_memory().alloc(kExSlot * static_cast<u32>(std::size(kSystemFonts)), 0x10);
            if (b) std::memset(hp(b), 0, kExSlot * std::size(kSystemFonts));
        }
        area = b;
    }
    const u32 list = rd32(arg + 12);
    const s32 count = rds32(list);
    const u32 entries = rd32(list + 4);
    for (s32 i = 0; i < count; i++) {
        const u32 e = entries + 8 * static_cast<u32>(i);
        const u32 type = rd32(e), out = rd32(e + 4);
        if (!out) continue;
        u32 slot = 0;
        bool found = false;
        for (; slot < std::size(kSystemFonts); slot++) {
            if (kSystemFonts[slot].type == type) {
                found = true;
                break;
            }
        }
        if (!found || !area) {
            wr32(out, 0);
            continue;
        }
        const u32 at = area + kExSlot * slot;
        std::string s = std::string("/:dev_font:/") + kSystemFonts[slot].name + ".ttf";
        s += '\0';
        s += '\xDE';
        for (const char c : std::string("/dev_flash/data/font/SCE-PS3-") + kSystemFonts[slot].file + ".TTF")
            s += static_cast<char>(c ^ 0xF2);
        s += '\xF2';
        std::memcpy(hp(at), s.data(), std::min<usize>(s.size(), kExSlot));
        wr32(out, at);
    }
    return 0;
}

// A file stream's read and close as the guest sees them (face->stream->read, ->close): the face's host stream. The
// face owns its stream; a close from the guest is refused.
u32 pspocket_ft_stream_io(PPUContext& ctx, u32 stream, u32 offset, u32 buffer, u32 count)
{
    Scope sc(ctx);
    FaceMirror* f = face_of_stream(stream);
    if (!f) return 0;
    LibLock lk = lock(f->lib);
    FT_Stream s = hface(f)->stream;
    std::vector<unsigned char> b(count);
    const unsigned long n = s->read ? s->read(s, offset, count ? b.data() : nullptr, count) : 0;
    if (buffer && n) std::memcpy(hp(buffer), b.data(), std::min<unsigned long>(n, count));
    return static_cast<u32>(n);
}

void pspocket_ft_stream_close(PPUContext&, u32 stream)
{
    PS_LOG_ERROR(kChan, "the guest closed the stream {:#x} of an open face: not done", stream);
}

// Unnamed exports libfontFT imports: the library's version as a 64-bit word (2.2.1, 0x0128), and the handler Sony's
// file layer calls when cellFs fails with CELL_EIO or CELL_EFSSPECIFIC.
void cell_FreeType2_211248E7(PPUContext& ctx, u32 out)
{
    if (!out) return;
    Scope sc(ctx);
    ps_write64(gbase(), out, 0x02020128);
}

void cell_FreeType2_904CA41F(PPUContext&, u32 fn, u32 arg) { set_file_handler(fn, arg); }

} // namespace g

#define FT_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&g::fn>, false}
#define FT_INTERNAL(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&g::fn>, true}

void register_ft_glyph(Registry& r);
void register_ft_cache(Registry& r);

void register_cell_FreeType2(Registry& r)
{
    r.add("cell_FreeType2", {
        FT_FUNC(0x84CE959C, FT_MulDiv),
        FT_FUNC(0x38E8695C, FT_MulFix),
        FT_FUNC(0xE68E4744, FT_DivFix),
        FT_FUNC(0x85C80B7F, FT_RoundFix),
        FT_FUNC(0xAFE5EBC4, FT_CeilFix),
        FT_FUNC(0x2803ABD1, FT_FloorFix),
        FT_FUNC(0xE46F3662, FT_Cos),
        FT_FUNC(0x3701F938, FT_Sin),
        FT_FUNC(0x7E5F23A4, FT_Tan),
        FT_FUNC(0xF78A4A76, FT_Atan2),
        FT_FUNC(0x1171AA63, FT_Angle_Diff),
        FT_FUNC(0xE4969D9D, FT_Vector_Unit),
        FT_FUNC(0x0AA1B8DE, FT_Vector_Rotate),
        FT_FUNC(0x615369C3, FT_Vector_Length),
        FT_FUNC(0xFA34723F, FT_Vector_Polarize),
        FT_FUNC(0xD7F0AFAA, FT_Vector_From_Polar),
        FT_FUNC(0x4BC591D7, FT_Vector_Transform),
        FT_FUNC(0x406D1E1A, FT_Matrix_Multiply),
        FT_FUNC(0xFF2D2F98, FT_Matrix_Invert),
        FT_FUNC(0x4DB200EF, FT_List_Find),
        FT_FUNC(0xC24E39FC, FT_List_Add),
        FT_FUNC(0x5923488B, FT_List_Insert),
        FT_FUNC(0xB6AAB704, FT_List_Remove),
        FT_FUNC(0x1827E724, FT_List_Up),
        FT_FUNC(0xA58C1554, FT_List_Iterate),
        FT_FUNC(0x4EE1B202, FT_List_Finalize),
        FT_FUNC(0xA26A2E9C, FT_Alloc),
        FT_FUNC(0x1A789B68, FT_Realloc),
        FT_FUNC(0x98DB2F48, FT_Free),
        FT_FUNC(0xBF97B657, FT_New_Memory),
        FT_FUNC(0x85A969B6, FT_Init_FreeType),
        FT_FUNC(0xA11BA28B, FT_New_Library),
        FT_FUNC(0x0124A94B, FT_Done_Library),
        FT_FUNC(0xE956BF64, FT_Done_FreeType),
        FT_FUNC(0x68C25868, FT_Add_Default_Modules),
        FT_FUNC(0xA6EC00F4, FT_Add_Module),
        FT_FUNC(0x99040A1D, FT_Remove_Module),
        FT_FUNC(0x4654E796, FT_Get_Module),
        FT_FUNC(0x492D8224, FT_Get_Renderer),
        FT_FUNC(0x917D6E9B, FT_Set_Renderer),
        FT_FUNC(0x72CF95FC, FT_Set_Debug_Hook),
        FT_FUNC(0x9EBA183C, FT_Library_Version),
        FT_FUNC(0xDD91F304, FT_Get_TrueType_Engine_Type),
        FT_FUNC(0x51B09E1D, FT_Open_Face),
        FT_FUNC(0xCC20195F, FT_New_Face),
        FT_FUNC(0x1BFBE848, FT_New_Memory_Face),
        FT_FUNC(0x1CEDF8D0, FT_Done_Face),
        FT_FUNC(0x6E560D59, FT_Attach_File),
        FT_FUNC(0xF9D31F75, FT_Attach_Stream),
        FT_FUNC(0x576E31A5, FT_Select_Size),
        FT_FUNC(0x92342AA4, FT_Request_Size),
        FT_FUNC(0x46D8826F, FT_Set_Char_Size),
        FT_FUNC(0xD12673A0, FT_Set_Pixel_Sizes),
        FT_FUNC(0x055C1970, FT_New_Size),
        FT_FUNC(0x9295B0FD, FT_Done_Size),
        FT_FUNC(0x06A553DF, FT_Activate_Size),
        FT_FUNC(0xB229E6BF, FT_Load_Glyph),
        FT_FUNC(0x9DA74BB6, FT_Load_Char),
        FT_FUNC(0x7EA2C657, FT_Set_Transform),
        FT_FUNC(0x5C36E44F, FT_Render_Glyph),
        FT_FUNC(0x67618BA2, FT_GlyphSlot_Own_Bitmap),
        FT_FUNC(0x67D56767, FT_Get_SubGlyph_Info),
        FT_FUNC(0xA4E6C423, FT_Select_Charmap),
        FT_FUNC(0xB9B430C2, FT_Set_Charmap),
        FT_FUNC(0x5F8370D7, FT_Get_Charmap_Index),
        FT_FUNC(0x81EED68C, FT_Get_CMap_Language_ID),
        FT_FUNC(0x4F36CF3F, FT_Get_Char_Index),
        FT_FUNC(0x39D44176, FT_Get_First_Char),
        FT_FUNC(0x37F42E67, FT_Get_Next_Char),
        FT_FUNC(0x4BE810BC, FT_Get_Name_Index),
        FT_FUNC(0xA6EB0B62, FT_Get_Kerning),
        FT_FUNC(0x36F59828, FT_Get_Track_Kerning),
        FT_FUNC(0x1E287E88, FT_Get_Glyph_Name),
        FT_FUNC(0x9E2B935A, FT_Get_Postscript_Name),
        FT_FUNC(0x89FB856E, FT_Get_Sfnt_Table),
        FT_FUNC(0x3545BDC1, FT_Load_Sfnt_Table),
        FT_FUNC(0x6A48D894, FT_Sfnt_Table_Info),
        FT_FUNC(0x838FE0A5, FT_Get_Sfnt_Name_Count),
        FT_FUNC(0x9D74740F, FT_Get_Sfnt_Name),
        FT_FUNC(0x8D79A5F9, FT_Get_PS_Font_Info),
        FT_FUNC(0xC3B65D96, FT_Get_PS_Font_Private),
        FT_FUNC(0xAED08DB1, FT_Has_PS_Glyph_Names),
        FT_FUNC(0x9A7A71B5, FT_Get_WinFNT_Header),
        FT_FUNC(0xA29CA82E, FT_Get_X11_Font_Format),
        FT_FUNC(0x55104762, FT_Get_BDF_Charset_ID),
        FT_FUNC(0xC8B94EE5, FT_Get_BDF_Property),
        FT_FUNC(0x50EC3696, FT_Get_PFR_Metrics),
        FT_FUNC(0xC141037D, FT_Get_PFR_Kerning),
        FT_FUNC(0x184A21DA, FT_Get_PFR_Advance),
        FT_FUNC(0x4CE3FC00, FT_Get_Multi_Master),
        FT_FUNC(0x8835C3C2, FT_Get_MM_Var),
        FT_FUNC(0x0AF56D94, FT_Set_MM_Design_Coordinates),
        FT_FUNC(0xCEE3C5A0, FT_Set_MM_Blend_Coordinates),
        FT_FUNC(0xFE7D5E4F, FT_Set_Var_Design_Coordinates),
        FT_FUNC(0x9164EFD1, FT_Set_Var_Blend_Coordinates),
        FT_FUNC(0xA742C04E, FT_OpenType_Validate),
        FT_FUNC(0xB5BBFBEA, FT_OpenType_Free),
        FT_FUNC(0xF8D95C49, cellFreeType2Ex),
        FT_FUNC(0x211248E7, cell_FreeType2_211248E7),
        FT_FUNC(0x904CA41F, cell_FreeType2_904CA41F),
        FT_INTERNAL(0x0F52B590, pspocket_ft_stream_io),
        FT_INTERNAL(0x89D2AF93, pspocket_ft_stream_close),
    });
    register_ft_glyph(r);
    register_ft_cache(r);
}

} // namespace pspocket::hle

#else

namespace pspocket::hle {

// Without FreeType (PSPOCKET_FREETYPE_DIR) the firmware's libfreetype serves the games.
void register_cell_FreeType2(Registry&) {}

} // namespace pspocket::hle

#endif
