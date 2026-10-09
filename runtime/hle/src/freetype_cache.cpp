// cell_FreeType2 (D-525), part 3: the cache subsystem (src/cache, FTC_*) on the host's FreeType.
//
// The manager, its caches and their nodes are the host's; the guest holds handles of their 32-bit sizes (an
// FTC_ManagerRec 0xB4, a glyph cache 0x68, the charmap cache 0x40; nodes a word each). The game's face requester is
// called on the guest with the face ID, the library and its data, and the face it opens is the guest's own (made with
// FT_New_Face). What the caches hand out is shown in the game's memory: an image cache's FT_Glyph as a guest glyph, a
// small bitmap cache's FTC_SBit as a guest FTC_SBitRec with its pixels; each goes when the host's cache frees what it
// shows. Old-style queries (FreeType 2.1.7's FTC_OldImageDesc and FTC_OldCMapDesc, which 2.2.1 recognises under
// FT_CONFIG_OPTION_OLD_INTERNALS) are read in the guest's layout. The host weighs its nodes with 64-bit sizes, so a
// cache flushes a little earlier than Sony's for the same max_bytes.

#include "pspocket/hle/hle.hpp"

#if PSPOCKET_HAVE_FREETYPE

#include "freetype_bridge.hpp"

#include FT_INTERNAL_OBJECTS_H

#include <cstring>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::hle {

namespace g {

using namespace ftb;

constexpr const char* kCChan = "freetype";
constexpr u32 kManagerRec = 0xB4, kGCacheRec = 0x68, kCMapCacheRec = 0x40, kNodeHandle = 4;

struct ManagerMirror : Mirror {
    u32 requester = 0, req_data = 0;
};

struct CacheMirror : Mirror {
    FTC_Manager manager = nullptr;
};

FTC_FaceID face_id_of(u32 guest) { return reinterpret_cast<FTC_FaceID>(static_cast<uintptr_t>(guest)); }
u32 guest_face_id(FTC_FaceID id) { return static_cast<u32>(reinterpret_cast<uintptr_t>(id)); }

ManagerMirror* manager_of(u32 guest)
{
    auto* m = by_guest_as<ManagerMirror>(guest, Mirror::Kind::manager);
    if (!m && guest) PS_LOG_WARN(kCChan, "{:#x} is not an FTC_Manager", guest);
    return m;
}

FTC_Manager hmanager(ManagerMirror* m) { return m ? static_cast<FTC_Manager>(const_cast<void*>(m->host)) : nullptr; }

template <class T>
T cache_of(u32 guest)
{
    Mirror* m = by_guest(guest, Mirror::Kind::cache);
    if (!m && guest) PS_LOG_WARN(kCChan, "{:#x} is not an FTC cache", guest);
    return m ? static_cast<T>(const_cast<void*>(m->host)) : nullptr;
}

// The game's requester, called with a face ID: the face it gives is one of pspocket's mirrors.
FT_Error requester_bridge(FTC_FaceID face_id, FT_Library, FT_Pointer data, FT_Face* aface)
{
    auto* m = static_cast<ManagerMirror*>(data);
    PPUContext& ctx = *current_ctx();
    StackVar out(ctx, gbase(), 8);
    const FT_Error e = static_cast<FT_Error>(static_cast<s32>(
        call_guest_fn(m->requester, {guest_face_id(face_id), m->lib->guest, m->req_data, out.addr()})));
    if (e) return e;
    FaceMirror* f = face_of(rd32(out.addr()));
    if (!f) {
        PS_LOG_ERROR(kCChan, "the face requester gave {:#x}, not a face", rd32(out.addr()));
        return FT_Err_Invalid_Face_Handle;
    }
    *aface = static_cast<FT_Face>(const_cast<void*>(f->host));
    return FT_Err_Ok;
}

// The faces of a library brought up to date after the cache worked on them (sizes made and activated, charmaps).
void sync_faces(Library* l)
{
    for (Mirror* m : mirrors_of(l, Mirror::Kind::face)) sync_face(static_cast<FaceMirror*>(m), false);
}

// A handle for a host object the guest only names (a node, a cache): a guest block, gone with the object.
u32 handle_for(Library* l, const void* host, Mirror::Kind kind, u32 size, FT_Error& error)
{
    error = FT_Err_Ok;
    if (const Mirror* m = by_host(host)) return m->guest;
    auto m = std::make_unique<Mirror>();
    m->kind = kind;
    m->host = host;
    m->lib = l;
    m->memory = l->memory;
    m->guest = g_alloc(l->memory, size, error);
    if (error) return 0;
    const u32 g = m->guest;
    add_mirror(std::move(m));
    return g;
}

s32 FTC_Manager_New(PPUContext& ctx, u32 library, u32 max_faces, u32 max_sizes, u32 max_bytes, u32 requester,
    u32 req_data, u32 amanager)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    LibLock lk = lock(l);
    auto m = std::make_unique<ManagerMirror>();
    m->kind = Mirror::Kind::manager;
    m->lib = l;
    m->memory = l->memory;
    m->requester = requester;
    m->req_data = req_data;
    FT_Error error = FT_Err_Ok;
    m->guest = g_alloc(l->memory, kManagerRec, error);
    if (error) return error;
    FTC_Manager manager = nullptr;
    error = ::FTC_Manager_New(l->ft, max_faces, max_sizes, max_bytes, requester_bridge, m.get(), &manager);
    if (error) {
        g_free(l->memory, m->guest);
        return error;
    }
    m->host = manager;
    const u32 g = m->guest;
    add_mirror(std::move(m));
    wr32(amanager, g);
    return FT_Err_Ok;
}

void FTC_Manager_Done(PPUContext& ctx, u32 manager)
{
    Scope sc(ctx);
    ManagerMirror* m = manager_of(manager);
    if (!m) return;
    LibLock lk = lock(m->lib);
    ::FTC_Manager_Done(hmanager(m)); // its caches, nodes, faces and its own handle go with it
}

s32 FTC_Manager_LookupFace(PPUContext& ctx, u32 manager, u32 face_id, u32 aface)
{
    Scope sc(ctx);
    if (!aface) return FT_Err_Bad_Argument;
    wr32(aface, 0);
    ManagerMirror* m = manager_of(manager);
    if (!m) return FT_Err_Invalid_Cache_Handle;
    LibLock lk = lock(m->lib);
    FT_Face face = nullptr;
    const FT_Error e = ::FTC_Manager_LookupFace(hmanager(m), face_id_of(face_id), &face);
    sync_faces(m->lib); // a face the manager opened has its first size done (ftc_face_node_init)
    if (!e) wr32(aface, guest_of(face));
    return e;
}

s32 FTC_Manager_LookupSize(PPUContext& ctx, u32 manager, u32 scaler, u32 asize)
{
    Scope sc(ctx);
    if (!asize) return FT_Err_Bad_Argument;
    wr32(asize, 0);
    ManagerMirror* m = manager_of(manager);
    if (!m) return FT_Err_Invalid_Cache_Handle;
    LibLock lk = lock(m->lib);
    FTC_ScalerRec s{};
    s.face_id = face_id_of(rd32(scaler + lay::sc_face_id));
    s.width = rd32(scaler + lay::sc_width);
    s.height = rd32(scaler + lay::sc_height);
    s.pixel = rds32(scaler + lay::sc_pixel);
    s.x_res = rd32(scaler + lay::sc_x_res);
    s.y_res = rd32(scaler + lay::sc_y_res);
    FT_Size size = nullptr;
    FT_Error e = ::FTC_Manager_LookupSize(hmanager(m), &s, &size);
    sync_faces(m->lib);
    if (!e && size) wr32(asize, guest_of(size));
    return e;
}

void FTC_Manager_RemoveFaceID(PPUContext& ctx, u32 manager, u32 face_id)
{
    Scope sc(ctx);
    ManagerMirror* m = manager_of(manager);
    if (!m) return;
    LibLock lk = lock(m->lib);
    ::FTC_Manager_RemoveFaceID(hmanager(m), face_id_of(face_id));
    sync_faces(m->lib);
}

void FTC_Node_Unref(PPUContext& ctx, u32 node, u32 manager)
{
    Scope sc(ctx);
    Mirror* n = by_guest(node, Mirror::Kind::node);
    ManagerMirror* m = manager_of(manager);
    if (!n || !m) return;
    LibLock lk = lock(m->lib);
    ::FTC_Node_Unref(static_cast<FTC_Node>(const_cast<void*>(n->host)), hmanager(m));
}

template <class New, class T>
s32 new_cache(PPUContext& ctx, u32 manager, u32 acache, u32 size, New make)
{
    Scope sc(ctx);
    ManagerMirror* m = manager_of(manager);
    if (!m) return FT_Err_Invalid_Cache_Handle;
    LibLock lk = lock(m->lib);
    T cache = nullptr;
    FT_Error e = make(hmanager(m), &cache);
    if (e) return e;
    u32 g = 0;
    if (const Mirror* known = by_host(cache)) {
        g = known->guest;
    } else {
        auto c = std::make_unique<CacheMirror>();
        c->kind = Mirror::Kind::cache;
        c->host = cache;
        c->lib = m->lib;
        c->memory = m->lib->memory;
        c->manager = hmanager(m);
        c->guest = g_alloc(m->lib->memory, size, e);
        if (e) return e;
        g = c->guest;
        add_mirror(std::move(c));
    }
    if (acache) wr32(acache, g);
    return FT_Err_Ok;
}

s32 FTC_CMapCache_New(PPUContext& ctx, u32 manager, u32 acache)
{
    return new_cache<decltype(&::FTC_CMapCache_New), FTC_CMapCache>(ctx, manager, acache, kCMapCacheRec,
        &::FTC_CMapCache_New);
}

s32 FTC_ImageCache_New(PPUContext& ctx, u32 manager, u32 acache)
{
    return new_cache<decltype(&::FTC_ImageCache_New), FTC_ImageCache>(ctx, manager, acache, kGCacheRec,
        &::FTC_ImageCache_New);
}

s32 FTC_SBitCache_New(PPUContext& ctx, u32 manager, u32 acache)
{
    return new_cache<decltype(&::FTC_SBitCache_New), FTC_SBitCache>(ctx, manager, acache, kGCacheRec,
        &::FTC_SBitCache_New);
}

// The query a guest FTC_ImageType makes: a width of 0x10000 or more is FreeType 2.1.7's FTC_OldImageDesc
// ({face_id, u16 pix_width, u16 pix_height, flags}).
FTC_ImageTypeRec image_type(u32 type)
{
    FTC_ImageTypeRec t{};
    t.face_id = face_id_of(rd32(type + lay::it_face_id));
    if (rd32(type + lay::it_width) >= 0x10000) {
        t.width = rd16(type + 4);
        t.height = rd16(type + 6);
        t.flags = rds32(type + 8);
    } else {
        t.width = rds32(type + lay::it_width);
        t.height = rds32(type + lay::it_height);
        t.flags = rds32(type + lay::it_flags);
    }
    return t;
}

Mirror* cache_mirror(u32 guest) { return by_guest(guest, Mirror::Kind::cache); }

// A cache's glyph as a guest FT_Glyph (bitmap or outline), made once per host glyph.
u32 glyph_mirror(Library* l, FT_Glyph glyph, FT_Error& error);

u32 FTC_CMapCache_Lookup(PPUContext& ctx, u32 cache, u32 face_id, s32 cmap_index, u32 char_code)
{
    Scope sc(ctx);
    Mirror* c = cache_mirror(cache);
    if (!c) return 0;
    LibLock lk = lock(c->lib);
    auto* hc = static_cast<FTC_CMapCache>(const_cast<void*>(c->host));
    u32 r = 0;
    if (cmap_index >= 4) {
        // FreeType 2.1.7's call: face_id is an FTC_OldCMapDesc {face_id, type, u}; FreeType 2.2.1 reads it under
        // FT_CONFIG_OPTION_OLD_INTERNALS (src/cache/ftccmap.c, its bugs too), given the host's copy of it
        struct OldCMapDesc {
            FTC_FaceID face_id;
            int type;
            union {
                FT_UInt index;
                FT_Encoding encoding;
                struct {
                    FT_UInt platform, encoding;
                } id;
            } u;
        } desc{};
        desc.face_id = face_id_of(rd32(face_id));
        desc.type = rds32(face_id + 4);
        desc.u.id.platform = rd32(face_id + 8);
        desc.u.id.encoding = rd32(face_id + 12);
        r = ::FTC_CMapCache_Lookup(hc, reinterpret_cast<FTC_FaceID>(&desc), cmap_index, char_code);
    } else {
        r = ::FTC_CMapCache_Lookup(hc, face_id_of(face_id), cmap_index, char_code);
    }
    sync_faces(c->lib);
    return r;
}

s32 FTC_ImageCache_Lookup(PPUContext& ctx, u32 cache, u32 type, u32 gindex, u32 aglyph, u32 anode)
{
    Scope sc(ctx);
    if (!aglyph) return FT_Err_Invalid_Argument;
    wr32(aglyph, 0);
    if (anode) wr32(anode, 0);
    Mirror* c = cache_mirror(cache);
    if (!c) return FT_Err_Invalid_Cache_Handle;
    LibLock lk = lock(c->lib);
    FTC_ImageTypeRec t = image_type(type);
    FT_Glyph glyph = nullptr;
    FTC_Node node = nullptr;
    FT_Error e = ::FTC_ImageCache_Lookup(static_cast<FTC_ImageCache>(const_cast<void*>(c->host)), &t, gindex, &glyph,
        anode ? &node : nullptr);
    sync_faces(c->lib);
    if (e) return e;
    const u32 g = glyph_mirror(c->lib, glyph, e);
    if (e) return e;
    wr32(aglyph, g);
    if (anode && node) {
        const u32 n = handle_for(c->lib, node, Mirror::Kind::node, kNodeHandle, e);
        if (e) return e;
        wr32(anode, n);
    }
    return FT_Err_Ok;
}

s32 FTC_SBitCache_Lookup(PPUContext& ctx, u32 cache, u32 type, u32 gindex, u32 ansbit, u32 anode)
{
    Scope sc(ctx);
    if (anode) wr32(anode, 0);
    if (!ansbit) return FT_Err_Invalid_Argument;
    wr32(ansbit, 0);
    Mirror* c = cache_mirror(cache);
    if (!c) return FT_Err_Invalid_Cache_Handle;
    LibLock lk = lock(c->lib);
    FTC_ImageTypeRec t = image_type(type);
    FTC_SBit sbit = nullptr;
    FTC_Node node = nullptr;
    FT_Error e = ::FTC_SBitCache_Lookup(static_cast<FTC_SBitCache>(const_cast<void*>(c->host)), &t, gindex, &sbit,
        anode ? &node : nullptr);
    sync_faces(c->lib);
    if (e) return e;
    // the small bitmap, once per host record
    u32 g = 0;
    if (const Mirror* m = by_host(sbit)) {
        g = m->guest;
    } else {
        auto m2 = std::make_unique<Mirror>();
        m2->kind = Mirror::Kind::sbit;
        m2->host = sbit;
        m2->lib = c->lib;
        m2->memory = c->lib->memory;
        m2->guest = g_alloc(c->lib->memory, lay::sb_rec, e);
        if (e) return e;
        g = m2->guest;
        wr8(g + lay::sb_width, sbit->width);
        wr8(g + lay::sb_height, sbit->height);
        wr8(g + lay::sb_left, static_cast<u8>(sbit->left));
        wr8(g + lay::sb_top, static_cast<u8>(sbit->top));
        wr8(g + lay::sb_format, sbit->format);
        wr8(g + lay::sb_max_grays, sbit->max_grays);
        wr16(g + lay::sb_pitch, static_cast<u16>(sbit->pitch));
        wr8(g + lay::sb_xadvance, static_cast<u8>(sbit->xadvance));
        wr8(g + lay::sb_yadvance, static_cast<u8>(sbit->yadvance));
        u32 buffer = 0;
        if (sbit->buffer) {
            const u32 n = static_cast<u32>(sbit->pitch < 0 ? -sbit->pitch : sbit->pitch) * sbit->height;
            buffer = g_qalloc(c->lib->memory, n, e);
            if (e) {
                g_free(c->lib->memory, g);
                return e;
            }
            if (n) std::memcpy(hp(buffer), sbit->buffer, n);
            m2->blocks.push_back(buffer);
        }
        wr32(g + lay::sb_buffer, buffer);
        add_mirror(std::move(m2));
    }
    wr32(ansbit, g);
    if (anode && node) {
        const u32 n = handle_for(c->lib, node, Mirror::Kind::node, kNodeHandle, e);
        if (e) return e;
        wr32(anode, n);
    }
    return FT_Err_Ok;
}

} // namespace g

#define FT_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&g::fn>, false}

void register_ft_cache(Registry& r)
{
    r.add("cell_FreeType2", {
        FT_FUNC(0x0C08DD3A, FTC_Manager_New),
        FT_FUNC(0xF4CF5435, FTC_Manager_Done),
        FT_FUNC(0x0B411261, FTC_Manager_LookupFace),
        FT_FUNC(0xF638C45E, FTC_Manager_LookupSize),
        FT_FUNC(0x61405530, FTC_Manager_RemoveFaceID),
        FT_FUNC(0x6525EF63, FTC_Node_Unref),
        FT_FUNC(0x6377137F, FTC_CMapCache_New),
        FT_FUNC(0xBC8C081D, FTC_ImageCache_New),
        FT_FUNC(0x15A11D97, FTC_SBitCache_New),
        FT_FUNC(0x8B14BE9C, FTC_CMapCache_Lookup),
        FT_FUNC(0xF926D22F, FTC_ImageCache_Lookup),
        FT_FUNC(0x5F7F83AA, FTC_SBitCache_Lookup),
    });
}

} // namespace pspocket::hle

#endif
