// cell_FreeType2's bridge between the host's FreeType 2.2.1 and the guest (D-525): see freetype_bridge.hpp.

#include "freetype_bridge.hpp"

#include FT_INTERNAL_OBJECTS_H
#include FT_INTERNAL_STREAM_H
#include FT_INTERNAL_GLYPH_LOADER_H

#include <algorithm>
#include <cstdlib>
#include <format>

#include "freetype/pspocket_ftsystem.h"
#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::ftb {

namespace {

constexpr const char* kChan = "freetype";

struct Current {
    PPUContext* ctx = nullptr;
    State* st = nullptr;
};
thread_local Current t_cur;

} // namespace

// ------------------------------------------------------------------------------------------------- state

struct State {
    std::mutex maps; // the maps only; never held across a guest call
    std::map<uintptr_t, std::unique_ptr<Mirror>> host;
    std::unordered_map<u32, Mirror*> guest;
    std::mutex classes_mutex;
    std::map<const void*, u32> classes; // host module class -> its guest mirror
    std::unordered_map<u32, const FT_Module_Class*> class_by_guest;
    u32 class_area = 0, class_used = 0;
    std::vector<Library*> libraries;
    bool closing = false;

    ~State()
    {
        // The process ends: the host's objects go, the guest's memory with the process.
        closing = true;
        for (Library* l : std::vector<Library*>(libraries))
            if (l->ft) ::FT_Done_Library(l->ft);
    }
};

State& state()
{
    HleProcess* p = HleProcess::current();
    PS_CHECK(p != nullptr);
    return p->state<State>();
}

Scope::Scope(PPUContext& ctx) : saved_(t_cur.ctx)
{
    State* st = &state();
    t_cur.ctx = &ctx;
    t_cur.st = st;
}

Scope::~Scope()
{
    t_cur.ctx = saved_;
    if (!saved_) t_cur.st = nullptr;
}

PPUContext* current_ctx() { return t_cur.ctx; }

u8* gbase()
{
    PS_CHECK(t_cur.ctx != nullptr);
    return guest_base(*t_cur.ctx);
}

u64 call_guest_fn(u32 opd, std::initializer_list<u64> args)
{
    PS_CHECK(t_cur.ctx != nullptr);
    return call_guest(*t_cur.ctx, opd, args);
}

u64 call_hle(std::string_view module, u32 nid, std::initializer_list<u64> args)
{
    PPUContext& ctx = *t_cur.ctx;
    const Function* f = registry().find(module, nid);
    PS_CHECK(f != nullptr);
    u64 saved[10];
    for (u32 k = 0; k < 10; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 10; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// ------------------------------------------------------------------------------------------------- guest memory
// src/base/ftutil.c: ft_mem_qalloc, ft_mem_alloc, ft_mem_qrealloc, ft_mem_realloc, ft_mem_free, with the PPU's 32-bit
// FT_Long.

u32 g_qalloc(u32 memory, s64 size, FT_Error& error)
{
    error = FT_Err_Ok;
    const s32 n = static_cast<s32>(size);
    if (n > 0) {
        const u32 block = static_cast<u32>(call_guest_fn(rd32(memory + lay::mem_alloc), {memory, static_cast<u64>(n)}));
        if (!block) error = FT_Err_Out_Of_Memory;
        return block;
    }
    if (n < 0) error = FT_Err_Invalid_Argument;
    return 0;
}

u32 g_alloc(u32 memory, s64 size, FT_Error& error)
{
    const u32 block = g_qalloc(memory, size, error);
    if (!error && static_cast<s32>(size) > 0) std::memset(hp(block), 0, static_cast<u32>(size));
    return block;
}

u32 g_qrealloc(u32 memory, s64 item, s64 cur, s64 count, u32 block, FT_Error& error)
{
    error = FT_Err_Ok;
    const s32 is = static_cast<s32>(item), cc = static_cast<s32>(cur), nc = static_cast<s32>(count);
    if (cc < 0 || nc < 0 || is <= 0) {
        error = FT_Err_Invalid_Argument;
    } else if (nc == 0) {
        g_free(memory, block);
        block = 0;
    } else if (nc > 0x7FFFFFFF / is) {
        error = FT_Err_Array_Too_Large;
    } else if (cc == 0) {
        block = g_alloc(memory, static_cast<s64>(nc) * is, error);
    } else {
        const u32 b2 = static_cast<u32>(call_guest_fn(rd32(memory + lay::mem_realloc),
            {memory, static_cast<u64>(static_cast<u32>(cc * is)), static_cast<u64>(static_cast<u32>(nc * is)), block}));
        if (!b2) error = FT_Err_Out_Of_Memory;
        else block = b2;
    }
    return block;
}

u32 g_realloc(u32 memory, s64 item, s64 cur, s64 count, u32 block, FT_Error& error)
{
    block = g_qrealloc(memory, item, cur, count, block, error);
    const s32 is = static_cast<s32>(item), cc = static_cast<s32>(cur), nc = static_cast<s32>(count);
    if (!error && nc > cc) std::memset(hp(block + static_cast<u32>(cc * is)), 0, static_cast<u32>((nc - cc) * is));
    return block;
}

void g_free(u32 memory, u32 block)
{
    if (block) call_guest_fn(rd32(memory + lay::mem_free), {memory, block});
}

std::string gstr(u32 a)
{
    std::string s;
    if (!a) return s;
    const u8* b = gbase();
    while (b[a + s.size()] && s.size() < 0x10000) s += static_cast<char>(b[a + s.size()]);
    return s;
}

u32 g_strdup(u32 memory, std::string_view s, FT_Error& error)
{
    const u32 g = g_qalloc(memory, static_cast<s64>(s.size()) + 1, error);
    if (error) return 0;
    std::memcpy(hp(g), s.data(), s.size());
    wr8(g + static_cast<u32>(s.size()), 0);
    return g;
}

// ------------------------------------------------------------------------------------------------- mirrors

namespace {

void free_guest_blocks(State& st, Mirror& m)
{
    if (st.closing || !t_cur.ctx || !m.memory) return;
    for (auto it = m.blocks.rbegin(); it != m.blocks.rend(); ++it) g_free(m.memory, *it);
    if (m.guest) g_free(m.memory, m.guest);
}

// The mirrors of the host objects in [lo, hi) go (their memory is being freed): the library's own is released by
// done_library.
void release_range(State& st, uintptr_t lo, uintptr_t hi)
{
    std::vector<std::unique_ptr<Mirror>> gone;
    {
        std::lock_guard g(st.maps);
        auto it = st.host.lower_bound(lo);
        while (it != st.host.end() && it->first < hi) {
            if (it->second->kind == Mirror::Kind::library) {
                ++it;
                continue;
            }
            st.guest.erase(it->second->guest);
            gone.push_back(std::move(it->second));
            it = st.host.erase(it);
        }
    }
    for (auto& m : gone) free_guest_blocks(st, *m);
}

// pspocket's host memory for a library: blocks with their size in front, so a free names the range whose mirrors go.
constexpr usize kHeader = 16;

void* hm_alloc(FT_Memory, long size)
{
    auto* p = static_cast<u8*>(std::malloc(static_cast<usize>(size) + kHeader));
    if (!p) return nullptr;
    const u64 n = static_cast<u64>(size);
    std::memcpy(p, &n, sizeof n);
    return p + kHeader;
}

void hm_free(FT_Memory memory, void* block)
{
    if (!block) return;
    u8* p = static_cast<u8*>(block) - kHeader;
    u64 n = 0;
    std::memcpy(&n, p, sizeof n);
    // (a block of a library's memory may hold mirrored objects; plain_memory()'s hold none)
    if (auto* l = static_cast<Library*>(memory->user)) {
        const auto lo = reinterpret_cast<uintptr_t>(block);
        release_range(*l->st, lo, lo + n);
    }
    std::free(p);
}

void* hm_realloc(FT_Memory memory, long, long size, void* block)
{
    if (!block) return hm_alloc(memory, size);
    u8* p = static_cast<u8*>(block) - kHeader;
    u64 old = 0;
    std::memcpy(&old, p, sizeof old);
    auto* q = static_cast<u8*>(std::realloc(p, static_cast<usize>(size) + kHeader));
    if (!q) return nullptr;
    const u64 n = static_cast<u64>(size);
    std::memcpy(q, &n, sizeof n);
    if (q != p && memory->user) {
        // Mirrors of objects inside a moved block move with it (none are expected: FreeType reallocates arrays).
        auto* l = static_cast<Library*>(memory->user);
        State& st = *l->st;
        std::lock_guard g(st.maps);
        const uintptr_t lo = reinterpret_cast<uintptr_t>(block), hi = lo + static_cast<uintptr_t>(std::min<u64>(old, n));
        const auto delta = reinterpret_cast<uintptr_t>(q + kHeader) - lo;
        std::vector<std::unique_ptr<Mirror>> moved;
        for (auto it = st.host.lower_bound(lo); it != st.host.end() && it->first < hi;) {
            moved.push_back(std::move(it->second));
            it = st.host.erase(it);
        }
        for (auto& m : moved) {
            m->host = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(m->host) + delta);
            const auto key = reinterpret_cast<uintptr_t>(m->host);
            st.host[key] = std::move(m);
        }
    }
    return q + kHeader;
}

// Generic finalizers: FreeType calls the host object's while destroying it; the guest's (in its mirror's `generic`) is
// called with the guest object, as Sony's libfreetype calls it.
void call_guest_finalizer(void* object, u32 generic)
{
    if (!t_cur.ctx || !t_cur.st || t_cur.st->closing) return;
    const Mirror* m = by_host(object);
    if (!m) return;
    const u32 fin = rd32(m->guest + generic + 4);
    if (fin) call_guest_fn(fin, {m->guest});
}
void face_finalizer(void* object) { call_guest_finalizer(object, lay::face_generic); }
void size_finalizer(void* object) { call_guest_finalizer(object, lay::size_generic); }
void module_finalizer(void* object) { call_guest_finalizer(object, lay::mod_generic); }
void library_finalizer(void* object) { call_guest_finalizer(object, lay::lib_generic); }

} // namespace

FT_Memory plain_memory()
{
    static FT_MemoryRec_ m = {nullptr, hm_alloc, hm_free, hm_realloc};
    return &m;
}

Mirror* by_host(const void* host)
{
    State& st = t_cur.st ? *t_cur.st : state();
    std::lock_guard g(st.maps);
    auto it = st.host.find(reinterpret_cast<uintptr_t>(host));
    return it == st.host.end() ? nullptr : it->second.get();
}

Mirror* by_guest(u32 guest, Mirror::Kind k)
{
    if (!guest) return nullptr;
    State& st = t_cur.st ? *t_cur.st : state();
    std::lock_guard g(st.maps);
    auto it = st.guest.find(guest);
    if (it == st.guest.end() || it->second->kind != k) return nullptr;
    return it->second;
}

u32 guest_of(const void* host)
{
    if (!host) return 0;
    const Mirror* m = by_host(host);
    return m ? m->guest : 0;
}

Mirror* add_mirror(std::unique_ptr<Mirror> m)
{
    State& st = t_cur.st ? *t_cur.st : state();
    std::lock_guard g(st.maps);
    Mirror* raw = m.get();
    st.guest[m->guest] = raw;
    st.host[reinterpret_cast<uintptr_t>(m->host)] = std::move(m);
    return raw;
}

void release_mirror(Mirror* m)
{
    State& st = t_cur.st ? *t_cur.st : state();
    std::unique_ptr<Mirror> owned;
    {
        std::lock_guard g(st.maps);
        auto it = st.host.find(reinterpret_cast<uintptr_t>(m->host));
        if (it == st.host.end() || it->second.get() != m) return;
        st.guest.erase(m->guest);
        owned = std::move(it->second);
        st.host.erase(it);
    }
    free_guest_blocks(st, *owned);
}

FaceMirror* face_of_stream(u32 guest_stream)
{
    State& st = t_cur.st ? *t_cur.st : state();
    std::lock_guard g(st.maps);
    for (auto& [key, m] : st.host)
        if (m->kind == Mirror::Kind::face && static_cast<FaceMirror*>(m.get())->stream == guest_stream)
            return static_cast<FaceMirror*>(m.get());
    return nullptr;
}

std::vector<Mirror*> mirrors_of(Library* l, Mirror::Kind k)
{
    State& st = t_cur.st ? *t_cur.st : state();
    std::lock_guard g(st.maps);
    std::vector<Mirror*> out;
    for (auto& [key, m] : st.host)
        if (m->kind == k && m->lib == l) out.push_back(m.get());
    return out;
}

Library* library_of(u32 guest)
{
    auto* l = by_guest_as<Library>(guest, Mirror::Kind::library);
    if (!l && guest) PS_LOG_WARN(kChan, "{:#x} is not an FT_Library", guest);
    return l;
}

FaceMirror* face_of(u32 guest)
{
    auto* f = by_guest_as<FaceMirror>(guest, Mirror::Kind::face);
    if (!f && guest) PS_LOG_WARN(kChan, "{:#x} is not an FT_Face", guest);
    return f;
}

SlotMirror* slot_of(u32 guest)
{
    auto* s = by_guest_as<SlotMirror>(guest, Mirror::Kind::slot);
    if (!s && guest) PS_LOG_WARN(kChan, "{:#x} is not an FT_GlyphSlot", guest);
    return s;
}

SizeMirror* size_of(u32 guest)
{
    auto* s = by_guest_as<SizeMirror>(guest, Mirror::Kind::size);
    if (!s && guest) PS_LOG_WARN(kChan, "{:#x} is not an FT_Size", guest);
    return s;
}

Mirror* charmap_of(u32 guest)
{
    Mirror* c = by_guest(guest, Mirror::Kind::charmap);
    if (!c && guest) PS_LOG_WARN(kChan, "{:#x} is not an FT_CharMap", guest);
    return c;
}

// ------------------------------------------------------------------------------------------------- module classes

namespace {

// FreeType 2.2.1's module records on a 32-bit target, by module name: the size Sony's libfreetype allocates for each
// (FT_Module_Class.module_size as its classes give it; the host's are 64-bit sizes).
u32 module_size_32(std::string_view name, u32 host_size)
{
    static const std::pair<std::string_view, u32> sizes[] = {
        {"autofitter", 0xC4}, {"truetype", 0x4C}, {"type1", 0x28}, {"cff", 0x2C}, {"t1cid", 0x28}, {"pfr", 0x28},
        {"type42", 0x30}, {"winfonts", 0x28}, {"psaux", 0x14}, {"psnames", 0x14}, {"pshinter", 0xB0},
        {"raster1", 0x48}, {"sfnt", 0x14}, {"smooth", 0x48}, {"smooth-lcd", 0x48}, {"smooth-lcdv", 0x48},
    };
    for (const auto& [n, s] : sizes)
        if (n == name) return s;
    return host_size;
}

constexpr u32 kClassArea = 0x1000;

} // namespace

u32 class_guest(const FT_Module_Class* clazz)
{
    if (!clazz) return 0;
    State& st = *t_cur.st;
    std::lock_guard g(st.classes_mutex);
    if (auto it = st.classes.find(clazz); it != st.classes.end()) return it->second;
    // Process-wide guest data, as Sony's classes are its module's data: the class record (a renderer's with its glyph
    // format, a driver's at its full size) and the name.
    const bool renderer = (clazz->module_flags & FT_MODULE_RENDERER) != 0;
    const bool driver = (clazz->module_flags & FT_MODULE_FONT_DRIVER) != 0;
    const u32 rec = renderer ? lay::cls_ren_size : driver ? lay::cls_drv_size : lay::cls_base_size;
    const std::string_view name = clazz->module_name ? clazz->module_name : "";
    const u32 need = (rec + static_cast<u32>(name.size()) + 1 + 15) & ~15u;
    if (!st.class_area || st.class_used + need > kClassArea) {
        st.class_area = HleProcess::current()->main_memory().alloc(kClassArea, 0x10);
        st.class_used = 0;
        if (!st.class_area) {
            PS_LOG_ERROR(kChan, "no memory for FreeType's module classes");
            return 0;
        }
    }
    const u32 c = st.class_area + st.class_used;
    st.class_used += need;
    std::memset(hp(c), 0, need);
    wr32(c + lay::cls_flags, clazz->module_flags);
    wr32(c + lay::cls_size, module_size_32(name, static_cast<u32>(clazz->module_size)));
    wr32(c + lay::cls_name, c + rec);
    wr32(c + lay::cls_version, static_cast<u32>(clazz->module_version));
    wr32(c + lay::cls_requires, static_cast<u32>(clazz->module_requires));
    if (renderer) wr32(c + lay::cls_ren_glyph_format, reinterpret_cast<const FT_Renderer_Class*>(clazz)->glyph_format);
    std::memcpy(hp(c + rec), name.data(), name.size());
    st.classes[clazz] = c;
    st.class_by_guest[c] = clazz;
    return c;
}

const FT_Module_Class* class_of(u32 guest)
{
    State& st = *t_cur.st;
    std::lock_guard g(st.classes_mutex);
    auto it = st.class_by_guest.find(guest);
    return it == st.class_by_guest.end() ? nullptr : it->second;
}

// ------------------------------------------------------------------------------------------------- libraries

u32 module_guest(Library* l, FT_Module m, FT_Error& error)
{
    error = FT_Err_Ok;
    if (!m) return 0;
    if (const Mirror* mm = by_host(m)) return mm->guest;
    const FT_Module_Class* clazz = m->clazz;
    const u32 gclazz = class_guest(clazz);
    const std::string_view name = clazz->module_name ? clazz->module_name : "";
    const u32 size = module_size_32(name, static_cast<u32>(clazz->module_size));
    auto mm = std::make_unique<Mirror>();
    mm->kind = Mirror::Kind::module;
    mm->host = m;
    mm->lib = l;
    mm->memory = l->memory;
    mm->guest = g_alloc(l->memory, size, error);
    if (error) return 0;
    const u32 g = mm->guest;
    wr32(g + lay::mod_clazz, gclazz);
    wr32(g + lay::mod_library, l->guest);
    wr32(g + lay::mod_memory, l->memory);
    if (FT_MODULE_IS_DRIVER(m) && size >= lay::drv_size) wr32(g + lay::drv_clazz, gclazz);
    if (FT_MODULE_IS_RENDERER(m) && size >= lay::ren_size) {
        wr32(g + lay::ren_clazz, gclazz);
        wr32(g + lay::ren_glyph_format, reinterpret_cast<FT_Renderer>(m)->glyph_format);
        // its node in the library's renderers list
        const u32 node = g_alloc(l->memory, lay::node_rec, error);
        if (error) {
            g_free(l->memory, g);
            return 0;
        }
        mm->blocks.push_back(node);
        wr32(node + lay::node_data, g);
    }
    m->generic.finalizer = module_finalizer;
    add_mirror(std::move(mm));
    return g;
}

FT_Module module_of(u32 guest)
{
    Mirror* m = by_guest(guest, Mirror::Kind::module);
    return m ? static_cast<FT_Module>(const_cast<void*>(m->host)) : nullptr;
}

FT_Error sync_library(Library* l)
{
    FT_Library lib = l->ft;
    FT_Error error = FT_Err_Ok;
    const u32 g = l->guest;
    wr32(g + lay::lib_num_modules, lib->num_modules);
    for (u32 i = 0; i < lay::max_modules; i++) {
        u32 gm = 0;
        if (i < lib->num_modules) {
            gm = module_guest(l, lib->modules[i], error);
            if (error) return error;
        }
        wr32(g + lay::lib_modules + 4 * i, gm);
    }
    // the renderers list, in the host's order, over the renderers' own guest nodes
    u32 prev = 0, head = 0;
    for (FT_ListNode n = lib->renderers.head; n; n = n->next) {
        const Mirror* rm = by_host(n->data);
        if (!rm || rm->blocks.empty()) continue;
        const u32 node = rm->blocks.front();
        wr32(node + lay::node_prev, prev);
        wr32(node + lay::node_next, 0);
        wr32(node + lay::node_data, rm->guest);
        if (prev) wr32(prev + lay::node_next, node);
        else head = node;
        prev = node;
    }
    wr32(g + lay::lib_renderers + lay::list_head, head);
    wr32(g + lay::lib_renderers + lay::list_tail, prev);
    wr32(g + lay::lib_cur_renderer, guest_of(lib->cur_renderer));
    wr32(g + lay::lib_auto_hinter, guest_of(lib->auto_hinter));
    wr32(g + lay::lib_raster_pool_size, lib->raster_pool_size);
    return error;
}

FT_Error new_library(u32 memory, u32* out)
{
    if (!memory) return FT_Err_Invalid_Argument;
    FT_Error error = FT_Err_Ok;
    const u32 g = g_alloc(memory, lay::lib_size, error);
    if (error) return error;
    auto l = std::make_unique<Library>();
    l->kind = Mirror::Kind::library;
    l->guest = g;
    l->memory = memory;
    l->st = t_cur.st;
    l->hmem.user = l.get();
    l->hmem.alloc = hm_alloc;
    l->hmem.free = hm_free;
    l->hmem.realloc = hm_realloc;
    error = ::FT_New_Library(&l->hmem, &l->ft);
    if (error) {
        g_free(memory, g);
        return error;
    }
    l->ft->generic.finalizer = library_finalizer;
    l->host = l->ft;
    wr32(g + lay::lib_memory, memory);
    wr32(g + lay::lib_raster_pool_size, l->ft->raster_pool_size);
    Library* raw = l.get();
    raw->st->libraries.push_back(raw);
    add_mirror(std::move(l));
    *out = g;
    return FT_Err_Ok;
}

FT_Error done_library(Library* l)
{
    {
        LibLock lk(l->mutex);
        ::FT_Done_Library(l->ft);
        l->ft = nullptr;
    }
    State& st = *l->st;
    std::erase(st.libraries, l);
    release_mirror(l);
    return FT_Err_Ok;
}

// ------------------------------------------------------------------------------------------------- faces

namespace {

// The stream a face reads, as the guest sees it: an FT_StreamRec of the face's own unless the guest gave its stream.
FT_Error make_stream_mirror(FaceMirror* f, FT_Stream s)
{
    if (s->close == nullptr && s->read == nullptr && !s->base) return FT_Err_Ok;
    FT_Error error = FT_Err_Ok;
    const u32 g = g_alloc(f->memory, lay::st_rec, error);
    if (error) return error;
    f->blocks.push_back(g);
    f->stream = g;
    wr32(g + lay::st_base, s->base && in_guest(s->base) ? gp(s->base) : 0);
    wr32(g + lay::st_size, s->size);
    wr32(g + lay::st_descriptor, static_cast<u32>(s->descriptor.value));
    wr32(g + lay::st_pathname, s->pathname.pointer && in_guest(s->pathname.pointer) ? gp(s->pathname.pointer) : 0);
    wr32(g + lay::st_memory, f->memory);
    if (s->read) {
        // a file's: its read and close are pspocket's (cell_FreeType2.cpp)
        HleProcess* p = HleProcess::current();
        wr32(g + lay::st_read, p->descriptor_of("cell_FreeType2", recomp::nid_of("pspocket_ft_stream_io")));
        wr32(g + lay::st_close, p->descriptor_of("cell_FreeType2", recomp::nid_of("pspocket_ft_stream_close")));
    }
    return FT_Err_Ok;
}

} // namespace

u32 slot_guest(FaceMirror* f, FT_GlyphSlot slot, FT_Error& error)
{
    error = FT_Err_Ok;
    if (!slot) return 0;
    if (const Mirror* m = by_host(slot)) return m->guest;
    auto s = std::make_unique<SlotMirror>();
    s->kind = Mirror::Kind::slot;
    s->host = slot;
    s->lib = f->lib;
    s->memory = f->memory;
    s->guest = g_alloc(f->memory, lay::slot_rec, error);
    if (error) return 0;
    const u32 g = s->guest;
    wr32(g + lay::slot_library, f->lib->guest);
    wr32(g + lay::slot_face, f->guest);
    add_mirror(std::move(s));
    return g;
}

u32 size_guest(FaceMirror* f, FT_Size size, FT_Error& error)
{
    error = FT_Err_Ok;
    if (!size) return 0;
    if (const Mirror* m = by_host(size)) return m->guest;
    auto s = std::make_unique<SizeMirror>();
    s->kind = Mirror::Kind::size;
    s->host = size;
    s->lib = f->lib;
    s->memory = f->memory;
    s->guest = g_alloc(f->memory, lay::size_rec, error);
    if (error) return 0;
    s->node = g_alloc(f->memory, lay::node_rec, error);
    if (error) {
        g_free(f->memory, s->guest);
        return 0;
    }
    s->blocks.push_back(s->node);
    wr32(s->node + lay::node_data, s->guest);
    wr32(s->guest + lay::size_face, f->guest);
    size->generic.finalizer = size_finalizer;
    SizeMirror* raw = s.get();
    add_mirror(std::move(s));
    sync_size(raw);
    return raw->guest;
}

FT_Error sync_size(SizeMirror* s)
{
    auto* size = static_cast<FT_Size>(const_cast<void*>(s->host));
    const u32 g = s->guest + lay::size_metrics;
    const FT_Size_Metrics& m = size->metrics;
    wr16(g + 0, m.x_ppem);
    wr16(g + 2, m.y_ppem);
    wr32(g + 4, m.x_scale);
    wr32(g + 8, m.y_scale);
    wr32(g + 12, m.ascender);
    wr32(g + 16, m.descender);
    wr32(g + 20, m.height);
    wr32(g + 24, m.max_advance);
    return FT_Err_Ok;
}

FT_Error make_face(Library* l, FT_Face face, u32* out)
{
    FT_Error error = FT_Err_Ok;
    auto fm = std::make_unique<FaceMirror>();
    fm->kind = Mirror::Kind::face;
    fm->host = face;
    fm->lib = l;
    fm->memory = l->memory;
    fm->guest = g_alloc(l->memory, lay::face_rec_size, error);
    if (error) {
        ::FT_Done_Face(face);
        return error;
    }
    FaceMirror* f = static_cast<FaceMirror*>(add_mirror(std::move(fm)));
    face->generic.finalizer = face_finalizer;
    auto fail = [&](FT_Error e) {
        ::FT_Done_Face(face); // its mirrors go with it
        return e;
    };
    const u32 g = f->guest;
    // what does not change after the face is open: names, fixed sizes, charmaps, the stream
    if (face->family_name) {
        const u32 s = g_strdup(f->memory, face->family_name, error);
        if (error) return fail(error);
        f->blocks.push_back(s);
        wr32(g + lay::face_family_name, s);
    }
    if (face->style_name) {
        const u32 s = g_strdup(f->memory, face->style_name, error);
        if (error) return fail(error);
        f->blocks.push_back(s);
        wr32(g + lay::face_style_name, s);
    }
    if (face->num_fixed_sizes > 0 && face->available_sizes) {
        const u32 a = g_alloc(f->memory, static_cast<s64>(face->num_fixed_sizes) * lay::bsize_rec, error);
        if (error) return fail(error);
        f->blocks.push_back(a);
        for (FT_Int i = 0; i < face->num_fixed_sizes; i++) {
            const FT_Bitmap_Size& b = face->available_sizes[i];
            const u32 e = a + static_cast<u32>(i) * lay::bsize_rec;
            wr16(e + lay::bsize_height, static_cast<u16>(b.height));
            wr16(e + lay::bsize_width, static_cast<u16>(b.width));
            wr32(e + lay::bsize_size, b.size);
            wr32(e + lay::bsize_x_ppem, b.x_ppem);
            wr32(e + lay::bsize_y_ppem, b.y_ppem);
        }
        wr32(g + lay::face_available_sizes, a);
    }
    if (face->num_charmaps > 0 && face->charmaps) {
        const u32 a = g_alloc(f->memory, static_cast<s64>(face->num_charmaps) * 4, error);
        if (error) return fail(error);
        f->blocks.push_back(a);
        f->charmaps = a;
        for (FT_Int i = 0; i < face->num_charmaps; i++) {
            FT_CharMap cm = face->charmaps[i];
            auto cmm = std::make_unique<Mirror>();
            cmm->kind = Mirror::Kind::charmap;
            cmm->host = cm;
            cmm->lib = l;
            cmm->memory = f->memory;
            cmm->guest = g_alloc(f->memory, lay::cmap_rec, error);
            if (error) return fail(error);
            const u32 c = cmm->guest;
            wr32(c + lay::cmap_face, g);
            wr32(c + lay::cmap_encoding, cm->encoding);
            wr16(c + lay::cmap_platform_id, cm->platform_id);
            wr16(c + lay::cmap_encoding_id, cm->encoding_id);
            wr32(a + 4 * static_cast<u32>(i), c);
            add_mirror(std::move(cmm));
        }
        wr32(g + lay::face_charmaps, a);
    }
    if (face->stream) {
        if (face->face_flags & FT_FACE_FLAG_EXTERNAL_STREAM) {
            // the guest's own FT_StreamRec (FT_OPEN_STREAM), or the host stream behind it
            wr32(g + lay::face_stream, guest_stream_of(face->stream));
        } else {
            error = make_stream_mirror(f, face->stream);
            if (error) return fail(error);
            wr32(g + lay::face_stream, f->stream);
        }
    }
    wr32(g + lay::face_memory, f->memory);
    error = sync_face(f, true);
    if (error) return fail(error);
    *out = g;
    return FT_Err_Ok;
}

FT_Error sync_face(FaceMirror* f, bool slot_image)
{
    auto* face = static_cast<FT_Face>(const_cast<void*>(f->host));
    const u32 g = f->guest;
    FT_Error error = FT_Err_Ok;
    wr32(g + lay::face_num_faces, face->num_faces);
    wr32(g + lay::face_face_index, face->face_index);
    wr32(g + lay::face_face_flags, face->face_flags);
    wr32(g + lay::face_style_flags, face->style_flags);
    wr32(g + lay::face_num_glyphs, face->num_glyphs);
    wr32(g + lay::face_num_fixed_sizes, face->num_fixed_sizes);
    wr32(g + lay::face_num_charmaps, face->num_charmaps);
    wr32(g + lay::face_bbox + 0, face->bbox.xMin);
    wr32(g + lay::face_bbox + 4, face->bbox.yMin);
    wr32(g + lay::face_bbox + 8, face->bbox.xMax);
    wr32(g + lay::face_bbox + 12, face->bbox.yMax);
    wr16(g + lay::face_units_per_EM, face->units_per_EM);
    wr16(g + lay::face_ascender, static_cast<u16>(face->ascender));
    wr16(g + lay::face_descender, static_cast<u16>(face->descender));
    wr16(g + lay::face_height, static_cast<u16>(face->height));
    wr16(g + lay::face_max_advance_width, static_cast<u16>(face->max_advance_width));
    wr16(g + lay::face_max_advance_height, static_cast<u16>(face->max_advance_height));
    wr16(g + lay::face_underline_position, static_cast<u16>(face->underline_position));
    wr16(g + lay::face_underline_thickness, static_cast<u16>(face->underline_thickness));
    // the glyph slot
    u32 gs = 0;
    if (face->glyph) {
        gs = slot_guest(f, face->glyph, error);
        if (error) return error;
    }
    wr32(g + lay::face_glyph, gs);
    // sizes, in the face's list
    u32 prev = 0, head = 0;
    for (FT_ListNode n = face->sizes_list.head; n; n = n->next) {
        const u32 gsz = size_guest(f, static_cast<FT_Size>(n->data), error);
        if (error) return error;
        auto* sm = by_guest_as<SizeMirror>(gsz, Mirror::Kind::size);
        sync_size(sm);
        wr32(sm->node + lay::node_prev, prev);
        wr32(sm->node + lay::node_next, 0);
        if (prev) wr32(prev + lay::node_next, sm->node);
        else head = sm->node;
        prev = sm->node;
    }
    wr32(g + lay::face_sizes_list + lay::list_head, head);
    wr32(g + lay::face_sizes_list + lay::list_tail, prev);
    u32 gsize = 0;
    if (face->size) {
        gsize = size_guest(f, face->size, error);
        if (error) return error;
    }
    wr32(g + lay::face_size, gsize);
    wr32(g + lay::face_charmap, guest_of(face->charmap));
    if (f->stream && face->stream) {
        // where the stream was last read (FreeType's own bookkeeping)
        wr32(f->stream + lay::st_pos, face->stream->pos);
        wr32(f->stream + lay::st_size, face->stream->size);
    } else if (face->stream && (face->face_flags & FT_FACE_FLAG_EXTERNAL_STREAM)) {
        // the guest's own stream, which FreeType reads through
        if (const u32 gst = guest_stream_of(face->stream)) wr32(gst + lay::st_pos, face->stream->pos);
    }
    if (face->driver) {
        const u32 d = module_guest(f->lib, &face->driver->root, error);
        if (error) return error;
        wr32(g + lay::face_driver, d);
    }
    if (slot_image && face->glyph) {
        auto* s = by_guest_as<SlotMirror>(gs, Mirror::Kind::slot);
        error = sync_slot(s);
    }
    return error;
}

FT_Error sync_slot(SlotMirror* s)
{
    auto* slot = static_cast<FT_GlyphSlot>(const_cast<void*>(s->host));
    const u32 g = s->guest;
    FT_Error error = FT_Err_Ok;
    wr32(g + lay::slot_next, guest_of(slot->next));
    wr32(g + lay::slot_reserved, slot->reserved);
    const FT_Glyph_Metrics& m = slot->metrics;
    const FT_Pos mv[8] = {m.width, m.height, m.horiBearingX, m.horiBearingY, m.horiAdvance, m.vertBearingX,
        m.vertBearingY, m.vertAdvance};
    for (u32 i = 0; i < 8; i++) wr32(g + lay::slot_metrics + 4 * i, mv[i]);
    wr32(g + lay::slot_linearHoriAdvance, slot->linearHoriAdvance);
    wr32(g + lay::slot_linearVertAdvance, slot->linearVertAdvance);
    wr32(g + lay::slot_advance, slot->advance.x);
    wr32(g + lay::slot_advance + 4, slot->advance.y);
    wr32(g + lay::slot_format, slot->format);
    // the bitmap: the slot's own guest copy of the host's image
    const FT_Bitmap& b = slot->bitmap;
    const u32 gb = g + lay::slot_bitmap;
    if (s->own_buffer && s->buffer) g_free(s->memory, s->buffer);
    s->buffer = 0;
    s->own_buffer = false;
    u32 buffer = 0;
    if (b.buffer) {
        const u32 n = static_cast<u32>(b.pitch < 0 ? -b.pitch : b.pitch) * static_cast<u32>(b.rows);
        buffer = g_alloc(s->memory, n, error);
        if (error) {
            s->own_blocks();
            return error;
        }
        if (n) std::memcpy(hp(buffer), b.buffer, n);
        s->buffer = buffer;
        s->own_buffer = buffer != 0;
    }
    wr32(gb + lay::bm_rows, b.rows);
    wr32(gb + lay::bm_width, b.width);
    wr32(gb + lay::bm_pitch, b.pitch);
    wr32(gb + lay::bm_buffer, buffer);
    wr16(gb + lay::bm_num_grays, static_cast<u16>(b.num_grays));
    wr8(gb + lay::bm_pixel_mode, static_cast<u8>(b.pixel_mode));
    wr8(gb + lay::bm_palette_mode, static_cast<u8>(b.palette_mode));
    wr32(gb + lay::bm_palette, 0);
    wr32(g + lay::slot_bitmap_left, slot->bitmap_left);
    wr32(g + lay::slot_bitmap_top, slot->bitmap_top);
    // the outline, into the slot's guest arrays
    const FT_Outline& o = slot->outline;
    const u32 np = o.n_points > 0 ? static_cast<u32>(o.n_points) : 0;
    const u32 nc = o.n_contours > 0 ? static_cast<u32>(o.n_contours) : 0;
    if (np > s->max_points) {
        const u32 cap = (np + 7) & ~7u;
        s->points = g_realloc(s->memory, 8, s->max_points, cap, s->points, error);
        if (error) return error;
        s->tags = g_realloc(s->memory, 1, s->max_points, cap, s->tags, error);
        if (error) return error;
        s->max_points = cap;
    }
    if (nc > s->max_contours) {
        const u32 cap = (nc + 3) & ~3u;
        s->contours = g_realloc(s->memory, 2, s->max_contours, cap, s->contours, error);
        if (error) return error;
        s->max_contours = cap;
    }
    for (u32 i = 0; i < np; i++) {
        wr32(s->points + 8 * i, o.points[i].x);
        wr32(s->points + 8 * i + 4, o.points[i].y);
        wr8(s->tags + i, static_cast<u8>(o.tags[i]));
    }
    for (u32 i = 0; i < nc; i++) wr16(s->contours + 2 * i, static_cast<u16>(o.contours[i]));
    const u32 go = g + lay::slot_outline;
    wr16(go + lay::ol_n_contours, static_cast<u16>(o.n_contours));
    wr16(go + lay::ol_n_points, static_cast<u16>(o.n_points));
    wr32(go + lay::ol_points, s->points);
    wr32(go + lay::ol_tags, s->tags);
    wr32(go + lay::ol_contours, s->contours);
    wr32(go + lay::ol_flags, o.flags);
    // subglyphs (a composite loaded with FT_LOAD_NO_RECURSE)
    const u32 ns = slot->num_subglyphs;
    if (ns > s->max_subglyphs) {
        s->subglyphs = g_realloc(s->memory, lay::sub_rec, s->max_subglyphs, ns, s->subglyphs, error);
        if (error) return error;
        s->max_subglyphs = ns;
    }
    for (u32 i = 0; i < ns && slot->subglyphs; i++) {
        const FT_SubGlyph sg = slot->subglyphs + i;
        const u32 e = s->subglyphs + i * lay::sub_rec;
        wr32(e + lay::sub_index, sg->index);
        wr16(e + lay::sub_flags, sg->flags);
        wr32(e + lay::sub_arg1, sg->arg1);
        wr32(e + lay::sub_arg2, sg->arg2);
        wr32(e + lay::sub_transform + 0, sg->transform.xx);
        wr32(e + lay::sub_transform + 4, sg->transform.xy);
        wr32(e + lay::sub_transform + 8, sg->transform.yx);
        wr32(e + lay::sub_transform + 12, sg->transform.yy);
    }
    wr32(g + lay::slot_num_subglyphs, ns);
    wr32(g + lay::slot_subglyphs, s->subglyphs);
    wr32(g + lay::slot_control_data, 0);
    wr32(g + lay::slot_control_len, slot->control_len);
    wr32(g + lay::slot_lsb_delta, slot->lsb_delta);
    wr32(g + lay::slot_rsb_delta, slot->rsb_delta);
    s->own_blocks();
    return FT_Err_Ok;
}

void read_back_slot(SlotMirror* s)
{
    auto* slot = static_cast<FT_GlyphSlot>(const_cast<void*>(s->host));
    const u32 g = s->guest;
    slot->format = static_cast<FT_Glyph_Format>(rd32(g + lay::slot_format));
    const u32 go = g + lay::slot_outline;
    const s16 np = rds16(go + lay::ol_n_points), nc = rds16(go + lay::ol_n_contours);
    FT_Outline& o = slot->outline;
    FT_GlyphLoader loader = slot->internal ? slot->internal->loader : nullptr;
    if (loader && (np > 0 || nc > 0)) {
        if (::FT_GlyphLoader_CheckPoints(loader, std::max<s16>(np, 0), std::max<s16>(nc, 0)) == FT_Err_Ok) {
            loader->base.outline.n_points = 0;
            loader->base.outline.n_contours = 0;
            o.points = loader->base.outline.points;
            o.tags = loader->base.outline.tags;
            o.contours = loader->base.outline.contours;
        }
    }
    const u32 pts = rd32(go + lay::ol_points), tags = rd32(go + lay::ol_tags), cts = rd32(go + lay::ol_contours);
    if (np > 0 && pts && tags && o.points && o.tags) {
        for (s32 i = 0; i < np; i++) {
            o.points[i].x = rds32(pts + 8 * static_cast<u32>(i));
            o.points[i].y = rds32(pts + 8 * static_cast<u32>(i) + 4);
            o.tags[i] = static_cast<char>(rd8(tags + static_cast<u32>(i)));
        }
    }
    if (nc > 0 && cts && o.contours)
        for (s32 i = 0; i < nc; i++) o.contours[i] = rds16(cts + 2 * static_cast<u32>(i));
    o.n_points = np;
    o.n_contours = nc;
    o.flags = rds32(go + lay::ol_flags);
}

// ------------------------------------------------------------------------------------------------- outlines

void read_outline(u32 guest, HostOutline& out, u32 extra_points, u32 extra_contours)
{
    const s16 np = rds16(guest + lay::ol_n_points), nc = rds16(guest + lay::ol_n_contours);
    const u32 n = np > 0 ? static_cast<u32>(np) : 0, c = nc > 0 ? static_cast<u32>(nc) : 0;
    out.points.assign(n + extra_points, FT_Vector{});
    out.tags.assign(n + extra_points, 0);
    out.contours.assign(c + extra_contours, 0);
    const u32 pts = rd32(guest + lay::ol_points), tags = rd32(guest + lay::ol_tags),
              cts = rd32(guest + lay::ol_contours);
    for (u32 i = 0; i < n && pts; i++) {
        out.points[i].x = rds32(pts + 8 * i);
        out.points[i].y = rds32(pts + 8 * i + 4);
    }
    for (u32 i = 0; i < n && tags; i++) out.tags[i] = static_cast<char>(rd8(tags + i));
    for (u32 i = 0; i < c && cts; i++) out.contours[i] = rds16(cts + 2 * i);
    out.o.n_points = np;
    out.o.n_contours = nc;
    out.o.points = pts || extra_points ? out.points.data() : nullptr;
    out.o.tags = tags || extra_points ? out.tags.data() : nullptr;
    out.o.contours = cts || extra_contours ? out.contours.data() : nullptr;
    out.o.flags = rds32(guest + lay::ol_flags);
}

void write_outline(u32 guest, const FT_Outline& o)
{
    wr16(guest + lay::ol_n_points, static_cast<u16>(o.n_points));
    wr16(guest + lay::ol_n_contours, static_cast<u16>(o.n_contours));
    wr32(guest + lay::ol_flags, o.flags);
    const u32 pts = rd32(guest + lay::ol_points), tags = rd32(guest + lay::ol_tags),
              cts = rd32(guest + lay::ol_contours);
    const u32 n = o.n_points > 0 ? static_cast<u32>(o.n_points) : 0;
    const u32 c = o.n_contours > 0 ? static_cast<u32>(o.n_contours) : 0;
    for (u32 i = 0; i < n && pts && o.points; i++) {
        wr32(pts + 8 * i, o.points[i].x);
        wr32(pts + 8 * i + 4, o.points[i].y);
    }
    for (u32 i = 0; i < n && tags && o.tags; i++) wr8(tags + i, static_cast<u8>(o.tags[i]));
    for (u32 i = 0; i < c && cts && o.contours; i++) wr16(cts + 2 * i, static_cast<u16>(o.contours[i]));
}

FT_Bitmap read_bitmap(u32 guest)
{
    FT_Bitmap b{};
    b.rows = rds32(guest + lay::bm_rows);
    b.width = rds32(guest + lay::bm_width);
    b.pitch = rds32(guest + lay::bm_pitch);
    const u32 buf = rd32(guest + lay::bm_buffer);
    b.buffer = buf ? hp(buf) : nullptr;
    b.num_grays = rds16(guest + lay::bm_num_grays);
    b.pixel_mode = static_cast<char>(rd8(guest + lay::bm_pixel_mode));
    b.palette_mode = static_cast<char>(rd8(guest + lay::bm_palette_mode));
    const u32 pal = rd32(guest + lay::bm_palette);
    b.palette = pal ? hp(pal) : nullptr;
    return b;
}

void write_bitmap(u32 guest, const FT_Bitmap& b)
{
    wr32(guest + lay::bm_rows, b.rows);
    wr32(guest + lay::bm_width, b.width);
    wr32(guest + lay::bm_pitch, b.pitch);
    wr32(guest + lay::bm_buffer, b.buffer ? gp(b.buffer) : 0);
    wr16(guest + lay::bm_num_grays, static_cast<u16>(b.num_grays));
    wr8(guest + lay::bm_pixel_mode, static_cast<u8>(b.pixel_mode));
    wr8(guest + lay::bm_palette_mode, static_cast<u8>(b.palette_mode));
    wr32(guest + lay::bm_palette, b.palette ? gp(b.palette) : 0);
}

FT_Vector read_vector(u32 guest) { return FT_Vector{rds32(guest), rds32(guest + 4)}; }

void write_vector(u32 guest, const FT_Vector& v)
{
    wr32(guest, v.x);
    wr32(guest + 4, v.y);
}

FT_Matrix read_matrix(u32 guest) { return FT_Matrix{rds32(guest), rds32(guest + 4), rds32(guest + 8), rds32(guest + 12)}; }

// ------------------------------------------------------------------------------------------------- streams

namespace {

// cellFs as Sony's libfreetype calls it (its fopen, fseek, ftell, fread and fclose over cellFsOpen, cellFsLseek,
// cellFsRead and cellFsClose): a read or seek failing with CELL_EIO or CELL_EFSSPECIFIC (the disc gone) calls the
// handler the system registered (cell_FreeType2_904CA41F).
struct FileHandler {
    std::mutex mutex;
    u32 fn = 0, arg = 0;
};
FileHandler& file_handler() { return HleProcess::current()->state<FileHandler>(); }

u32 nid(const char* name) { return recomp::nid_of(name); }

s32 fs_call(const char* name, std::initializer_list<u64> args)
{
    return static_cast<s32>(static_cast<u32>(call_hle("sys_fs", nid(name), args)));
}

void fs_error(s32 r)
{
    if (r != static_cast<s32>(0x8001002B) && r != static_cast<s32>(0x80010038)) return;
    u32 fn = 0, arg = 0;
    {
        FileHandler& h = file_handler();
        std::lock_guard g(h.mutex);
        fn = h.fn;
        arg = h.arg;
    }
    if (fn) call_guest_fn(fn, {static_cast<u64>(static_cast<s64>(r)), arg});
}

s32 fs_seek(u32 fd, s32 offset, u32 whence)
{
    if (whence > 2) return -1;
    PPUContext& ctx = *t_cur.ctx;
    StackVar pos(ctx, guest_base(ctx), 8);
    const s32 r = fs_call("cellFsLseek", {fd, static_cast<u64>(static_cast<s64>(offset)), whence, pos.addr()});
    if (r == 0) return 0;
    fs_error(r);
    return -1;
}

s32 fs_tell(u32 fd)
{
    PPUContext& ctx = *t_cur.ctx;
    StackVar pos(ctx, guest_base(ctx), 8);
    const s32 r = fs_call("cellFsLseek", {fd, 0, 1, pos.addr()});
    if (r == 0) return static_cast<s32>(ps_read32(guest_base(ctx), pos.addr() + 4));
    fs_error(r);
    return -1;
}

// fread(buffer, 1, count, fd) into host memory, through the guest's stack in 4 KB pieces.
unsigned long fs_read(u32 fd, unsigned char* buffer, unsigned long count)
{
    if (!buffer || !count) return 0;
    PPUContext& ctx = *t_cur.ctx;
    u8* base = guest_base(ctx);
    constexpr u32 kChunk = 0x1000;
    StackVar chunk(ctx, base, kChunk);
    StackVar nread(ctx, base, 8);
    unsigned long done = 0;
    while (done < count) {
        const u32 want = static_cast<u32>(std::min<unsigned long>(count - done, kChunk));
        ps_write64(base, nread.addr(), 0);
        const s32 r = fs_call("cellFsRead", {fd, chunk.addr(), want, nread.addr()});
        if (r != 0) {
            fs_error(r);
            return done;
        }
        const u32 got = static_cast<u32>(ps_read64(base, nread.addr()));
        std::memcpy(buffer + done, base + chunk.addr(), std::min(got, want));
        done += std::min(got, want);
        if (got < want) break;
    }
    return done;
}

unsigned long file_stream_io(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count)
{
    if (!t_cur.ctx) return 0;
    const u32 fd = static_cast<u32>(stream->descriptor.value);
    fs_seek(fd, static_cast<s32>(offset), 0);
    return fs_read(fd, buffer, count);
}

void file_stream_close(FT_Stream stream)
{
    if (t_cur.ctx) {
        const s32 r = fs_call("cellFsClose", {static_cast<u32>(stream->descriptor.value)});
        if (r != 0) fs_error(r);
    }
    stream->base = nullptr;
    stream->descriptor.value = 0;
    stream->size = 0;
}

// Sony's libfreetype opens the paths cellFreeType2Ex hands out ("/:dev_font:/NAME", then a NUL, 0xDE and the real
// path XORed with 0xF2) by the real path: the prefix is "/:dev_font", one character other than ':' or none, then
// ":/". Any other path opens as it is (at most 1087 characters).
FT_Error stream_open(FT_Stream stream, const char* path)
{
    if (!t_cur.ctx) return FT_Err_Cannot_Open_Resource;
    PPUContext& ctx = *t_cur.ctx;
    u8* base = guest_base(ctx);
    constexpr u32 kMax = 1087;
    StackVar buf(ctx, base, kMax + 1);
    StackVar fd(ctx, base, 8);
    u32 len = 0;
    while (len < kMax && path[len]) {
        base[buf.addr() + len] = static_cast<u8>(path[len]);
        len++;
    }
    base[buf.addr() + len] = 0;
    // the pattern, as Sony's matcher walks it
    static constexpr char kPattern[] = "/:dev_font\x01:/"; // \x01: one character other than ':', or none
    u32 want = 13, i = 0, pi = 0;
    bool match = true;
    while (i < want) {
        const char c = static_cast<char>(base[buf.addr() + i]);
        if (!c) {
            match = false;
            break;
        }
        if (kPattern[pi] == '\x01') {
            if (c == ':') {
                want--; // the wildcard matched nothing: this ':' is the pattern's
                i++;
                pi += 2;
            } else {
                i++;
                pi++;
            }
            continue;
        }
        if (c != kPattern[pi]) {
            match = false;
            break;
        }
        i++;
        pi++;
    }
    match = match && i == want;
    u32 open_at = buf.addr();
    if (match) {
        const u32 hidden = len + 1;
        open_at = buf.addr() + hidden;
        // the real path follows the visible one's NUL (only a guest string can carry one)
        if (in_guest(path) && static_cast<u8>(path[hidden]) == 0xDE && hidden <= kMax - 1) {
            u32 k = 0;
            for (; hidden + k < kMax; k++) {
                const u8 c = static_cast<u8>(static_cast<u8>(path[hidden + 1 + k]) ^ 0xF2);
                base[buf.addr() + hidden + k] = c;
                if (!c) break;
            }
            base[buf.addr() + std::min(hidden + k, kMax)] = 0;
        }
    }
    const s32 r = fs_call("cellFsOpen", {open_at, 0, fd.addr(), 0, 0});
    if (match) std::memset(base + buf.addr() + len + 1, 0, kMax - len); // Sony wipes the real path from its stack
    if (r != 0) {
        fs_error(r);
        return FT_Err_Cannot_Open_Resource;
    }
    const u32 file = ps_read32(base, fd.addr());
    if (!file) return FT_Err_Cannot_Open_Resource;
    fs_seek(file, 0, 2);
    stream->size = static_cast<unsigned long>(static_cast<long>(fs_tell(file)) & 0xFFFFFFFF);
    fs_seek(file, 0, 0);
    stream->descriptor.value = static_cast<long>(file);
    stream->pathname.pointer = const_cast<char*>(path);
    stream->pos = 0;
    stream->read = file_stream_io;
    stream->close = file_stream_close;
    return FT_Err_Ok;
}

struct Installer {
    Installer() { pspocket_ft_stream_open = stream_open; }
} g_installer;

// A host stream over a guest FT_StreamRec: its memory, or its read and close functions called on the guest.
struct GuestStream : FT_StreamRec {
    u32 guest = 0;
    u32 g_read = 0, g_close = 0; // the guest's functions
};

unsigned long guest_stream_io(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count)
{
    auto* s = static_cast<GuestStream*>(stream);
    if (!t_cur.ctx) return 0;
    PPUContext& ctx = *t_cur.ctx;
    u8* base = guest_base(ctx);
    if (!count) return static_cast<unsigned long>(call_guest_fn(s->g_read, {s->guest, offset, 0, 0}) & 0xFFFFFFFF);
    constexpr u32 kChunk = 0x1000;
    StackVar chunk(ctx, base, kChunk);
    unsigned long done = 0;
    while (done < count) {
        const u32 want = static_cast<u32>(std::min<unsigned long>(count - done, kChunk));
        const u32 got = static_cast<u32>(call_guest_fn(s->g_read, {s->guest, offset + done, chunk.addr(), want}));
        std::memcpy(buffer + done, base + chunk.addr(), std::min(got, want));
        done += std::min(got, want);
        if (got < want) break;
    }
    return done;
}

void guest_stream_close(FT_Stream stream)
{
    auto* s = static_cast<GuestStream*>(stream);
    if (s->g_close && t_cur.ctx) call_guest_fn(s->g_close, {s->guest});
    delete s;
}

} // namespace

void set_file_handler(u32 fn, u32 arg)
{
    FileHandler& h = file_handler();
    std::lock_guard g(h.mutex);
    h.fn = fn;
    h.arg = arg;
}

FT_Stream bridge_stream(u32 g)
{
    auto* s = new GuestStream{};
    s->guest = g;
    const u32 base = rd32(g + lay::st_base);
    s->g_read = rd32(g + lay::st_read);
    s->g_close = rd32(g + lay::st_close);
    s->size = rd32(g + lay::st_size);
    s->pos = rd32(g + lay::st_pos);
    s->descriptor.value = rds32(g + lay::st_descriptor);
    s->base = s->g_read ? nullptr : (base ? hp(base) : nullptr);
    s->read = s->g_read ? guest_stream_io : nullptr;
    s->close = guest_stream_close;
    return s;
}

void free_bridge_stream(FT_Stream s) { delete static_cast<GuestStream*>(s); }

u32 guest_stream_of(FT_Stream s)
{
    if (s && s->close == guest_stream_close) return static_cast<GuestStream*>(s)->guest;
    return compressed_guest_stream(s);
}

} // namespace pspocket::hle::ftb
