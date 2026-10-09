#pragma once

// cell_FreeType2 (D-525): FreeType 2.2.1 running on the host, shown to the guest in the PS3's layout.
//
// Sony's libfreetype.sprx is FreeType 2.2.1 built for the PPU (32-bit pointers and longs, big-endian) with FreeType's
// default options and module list less the PCF and BDF drivers, 64-bit integer arithmetic (FT_LONG64), a system layer
// on cellFs, and no memory manager of its own (FT_New_Memory answers NULL, so FT_Init_FreeType fails and games make
// their libraries with FT_New_Library and an FT_Memory of their own). pspocket builds the same FreeType with the same
// options and modules for the host and runs it natively; every object the guest can see through FreeType's public API
// gets a mirror in guest memory, allocated through the game's FT_Memory, in FreeType 2.2.1's public layout for a 32-bit
// target: the library, its modules, faces with their charmaps, sizes and glyph slot, glyph images, strokers and the
// cache's objects. The host objects stay in host memory. A mirror is written after each call that changes its object
// (the guest's own fields, the `generic` ones, are left alone), and the guest's changes to a slot's image are read back
// before a call uses it. A mirror goes when its host object is freed: the host memory manager tells the bridge, and the
// mirror's guest blocks go back to the game's FT_Memory. Objects the guest owns outright (FT_Glyph, FT_Bitmap,
// FT_Outline buffers, list nodes) are worked on in guest memory as FreeType 2.2.1 works on them, with the same
// allocations through the game's FT_Memory.

#include <ft2build.h>
#include <freetype/internal/internal.h> // FT_INTERNAL_*_H (FreeType defines them for its own build only)
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BITMAP_H
#include FT_STROKER_H
#include FT_CACHE_H
#include FT_MODULE_H
#include FT_RENDER_H
#include FT_SIZES_H
#include FT_INTERNAL_OBJECTS_H

#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle::ftb {

// ---------------------------------------------------------------------------------------------------- guest layouts
// FreeType 2.2.1's public structures on a 32-bit big-endian target (clang -target powerpc over FreeType's headers with
// pspocket's options; Sony's allocation sizes agree: FT_LibraryRec 0xC4, FT_GlyphSlotRec 0xA0, TT_FaceRec 0x334).
namespace lay {
// FT_MemoryRec
constexpr u32 mem_user = 0, mem_alloc = 4, mem_free = 8, mem_realloc = 12;
// FT_LibraryRec
constexpr u32 lib_memory = 0, lib_generic = 4, lib_version = 12, lib_num_modules = 24, lib_modules = 28,
              lib_renderers = 156, lib_cur_renderer = 164, lib_auto_hinter = 168, lib_raster_pool = 172,
              lib_raster_pool_size = 176, lib_debug_hooks = 180, lib_size = 196;
constexpr u32 max_modules = 32;
// FT_ModuleRec, FT_DriverRec, FT_RendererRec
constexpr u32 mod_clazz = 0, mod_library = 4, mod_memory = 8, mod_generic = 12, mod_size = 20;
constexpr u32 drv_clazz = 20, drv_faces_list = 24, drv_extensions = 32, drv_glyph_loader = 36, drv_size = 40;
constexpr u32 ren_clazz = 20, ren_glyph_format = 24, ren_glyph_class = 28, ren_raster = 60, ren_raster_render = 64,
              ren_render = 68, ren_size = 72;
// FT_Module_Class (+ FT_Renderer_Class's glyph_format)
constexpr u32 cls_flags = 0, cls_size = 4, cls_name = 8, cls_version = 12, cls_requires = 16, cls_interface = 20,
              cls_init = 24, cls_done = 28, cls_get_interface = 32, cls_base_size = 36, cls_ren_glyph_format = 36,
              cls_ren_size = 60, cls_drv_size = 104;
// FT_FaceRec
constexpr u32 face_num_faces = 0, face_face_index = 4, face_face_flags = 8, face_style_flags = 12,
              face_num_glyphs = 16, face_family_name = 20, face_style_name = 24, face_num_fixed_sizes = 28,
              face_available_sizes = 32, face_num_charmaps = 36, face_charmaps = 40, face_generic = 44, face_bbox = 52,
              face_units_per_EM = 68, face_ascender = 70, face_descender = 72, face_height = 74,
              face_max_advance_width = 76, face_max_advance_height = 78, face_underline_position = 80,
              face_underline_thickness = 82, face_glyph = 84, face_size = 88, face_charmap = 92, face_driver = 96,
              face_memory = 100, face_stream = 104, face_sizes_list = 108, face_autohint = 116,
              face_extensions = 124, face_internal = 128, face_rec_size = 132;
// FT_Bitmap_Size, FT_CharMapRec
constexpr u32 bsize_height = 0, bsize_width = 2, bsize_size = 4, bsize_x_ppem = 8, bsize_y_ppem = 12, bsize_rec = 16;
constexpr u32 cmap_face = 0, cmap_encoding = 4, cmap_platform_id = 8, cmap_encoding_id = 10, cmap_rec = 12;
// FT_SizeRec (metrics: x_ppem, y_ppem, x_scale, y_scale, ascender, descender, height, max_advance)
constexpr u32 size_face = 0, size_generic = 4, size_metrics = 12, size_internal = 40, size_rec = 44;
// FT_GlyphSlotRec
constexpr u32 slot_library = 0, slot_face = 4, slot_next = 8, slot_reserved = 12, slot_generic = 16,
              slot_metrics = 24, slot_linearHoriAdvance = 56, slot_linearVertAdvance = 60, slot_advance = 64,
              slot_format = 72, slot_bitmap = 76, slot_bitmap_left = 100, slot_bitmap_top = 104, slot_outline = 108,
              slot_num_subglyphs = 128, slot_subglyphs = 132, slot_control_data = 136, slot_control_len = 140,
              slot_lsb_delta = 144, slot_rsb_delta = 148, slot_other = 152, slot_internal = 156, slot_rec = 160;
// FT_SubGlyphRec
constexpr u32 sub_index = 0, sub_flags = 4, sub_arg1 = 8, sub_arg2 = 12, sub_transform = 16, sub_rec = 32;
// FT_Bitmap
constexpr u32 bm_rows = 0, bm_width = 4, bm_pitch = 8, bm_buffer = 12, bm_num_grays = 16, bm_pixel_mode = 18,
              bm_palette_mode = 19, bm_palette = 20, bm_rec = 24;
// FT_Outline
constexpr u32 ol_n_contours = 0, ol_n_points = 2, ol_points = 4, ol_tags = 8, ol_contours = 12, ol_flags = 16,
              ol_rec = 20;
// FT_StreamRec
constexpr u32 st_base = 0, st_size = 4, st_pos = 8, st_descriptor = 12, st_pathname = 16, st_read = 20, st_close = 24,
              st_memory = 28, st_cursor = 32, st_limit = 36, st_rec = 40;
// FT_Open_Args, FT_Parameter
constexpr u32 oa_flags = 0, oa_memory_base = 4, oa_memory_size = 8, oa_pathname = 12, oa_stream = 16, oa_driver = 20,
              oa_num_params = 24, oa_params = 28;
// FT_ListRec, FT_ListNodeRec
constexpr u32 list_head = 0, list_tail = 4, node_prev = 0, node_next = 4, node_data = 8, node_rec = 12;
// FT_GlyphRec, FT_BitmapGlyphRec, FT_OutlineGlyphRec, FT_Glyph_Class
constexpr u32 gl_library = 0, gl_clazz = 4, gl_format = 8, gl_advance = 12, gl_rec = 20;
constexpr u32 bgl_left = 20, bgl_top = 24, bgl_bitmap = 28, bgl_rec = 52;
constexpr u32 ogl_outline = 20, ogl_rec = 40;
constexpr u32 gc_size = 0, gc_format = 4, gc_init = 8, gc_done = 12, gc_copy = 16, gc_transform = 20, gc_bbox = 24,
              gc_prepare = 28, gc_rec = 32;
// FT_Outline_Funcs, FT_Raster_Params, FT_Span
constexpr u32 of_move_to = 0, of_line_to = 4, of_conic_to = 8, of_cubic_to = 12, of_shift = 16, of_delta = 20;
constexpr u32 rp_target = 0, rp_source = 4, rp_flags = 8, rp_gray_spans = 12, rp_black_spans = 16, rp_bit_test = 20,
              rp_bit_set = 24, rp_user = 28, rp_clip_box = 32;
// FT_Size_RequestRec, FT_SfntName
constexpr u32 sr_type = 0, sr_width = 4, sr_height = 8, sr_hori = 12, sr_vert = 16;
constexpr u32 sn_platform_id = 0, sn_encoding_id = 2, sn_language_id = 4, sn_name_id = 6, sn_string = 8,
              sn_string_len = 12;
// FTC_ScalerRec, FTC_ImageTypeRec, FTC_SBitRec
constexpr u32 sc_face_id = 0, sc_width = 4, sc_height = 8, sc_pixel = 12, sc_x_res = 16, sc_y_res = 20;
constexpr u32 it_face_id = 0, it_width = 4, it_height = 8, it_flags = 12;
constexpr u32 sb_width = 0, sb_height = 1, sb_left = 2, sb_top = 3, sb_format = 4, sb_max_grays = 5, sb_pitch = 6,
              sb_xadvance = 8, sb_yadvance = 9, sb_buffer = 12, sb_rec = 16;
} // namespace lay

// ---------------------------------------------------------------------------------------------------- the call
// The HLE call in progress on this thread: guest callbacks (memory, streams, finalizers, the cache's face requester,
// outline decomposition, spans) run on its context. Nested scopes (the game's requester calling FT_New_Face) stack.
class Scope {
public:
    explicit Scope(PPUContext& ctx);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    PPUContext* saved_;
};
PPUContext* current_ctx(); // null outside a call
u8* gbase();
// A guest function (its descriptor) called on the current context: r3.
u64 call_guest_fn(u32 opd, std::initializer_list<u64> args);

// ---------------------------------------------------------------------------------------------------- guest memory
// FreeType's ft_mem_* (src/base/ftutil.c) over a guest FT_Memory: the game's alloc, free and realloc called with
// FreeType's sizes and checks, the same calls Sony's libfreetype makes. Errors as FreeType's.
u32 g_qalloc(u32 memory, s64 size, FT_Error& error);
u32 g_alloc(u32 memory, s64 size, FT_Error& error); // zeroed
u32 g_qrealloc(u32 memory, s64 item, s64 cur, s64 count, u32 block, FT_Error& error);
u32 g_realloc(u32 memory, s64 item, s64 cur, s64 count, u32 block, FT_Error& error); // the new part zeroed
void g_free(u32 memory, u32 block);

// Guest memory accessors.
inline u32 rd32(u32 a) { return ps_read32(gbase(), a); }
inline s32 rds32(u32 a) { return static_cast<s32>(ps_read32(gbase(), a)); }
inline u16 rd16(u32 a) { return ps_read16(gbase(), a); }
inline s16 rds16(u32 a) { return static_cast<s16>(ps_read16(gbase(), a)); }
inline u8 rd8(u32 a) { return ps_read8(gbase(), a); }
inline void wr32(u32 a, u64 v) { ps_write32(gbase(), a, static_cast<u32>(v)); }
inline void wr16(u32 a, u64 v) { ps_write16(gbase(), a, static_cast<u16>(v)); }
inline void wr8(u32 a, u64 v) { ps_write8(gbase(), a, static_cast<u8>(v)); }
inline u8* hp(u32 a) { return gbase() + a; } // a guest address as a host pointer
inline u32 gp(const void* p) { return static_cast<u32>(static_cast<const u8*>(p) - gbase()); }
inline bool in_guest(const void* p)
{
    const u8* q = static_cast<const u8*>(p);
    return q >= gbase() && q < gbase() + 0x100000000ull;
}
// The guest string at `a` (empty for 0).
std::string gstr(u32 a);
// A guest string allocated through `memory` (0 when the allocation failed).
u32 g_strdup(u32 memory, std::string_view s, FT_Error& error);

// ---------------------------------------------------------------------------------------------------- mirrors
struct Library;
struct State;

struct Mirror {
    enum class Kind : u8 { library, module, face, slot, size, charmap, manager, cache, node, sbit, glyph, stroker };
    Kind kind{};
    u32 guest = 0;            // the guest object
    const void* host = nullptr;
    u32 memory = 0;           // the guest FT_Memory its blocks came from
    std::vector<u32> blocks;  // guest blocks it owns besides `guest`, freed with it
    Library* lib = nullptr;   // its library (null for the library itself)
    virtual ~Mirror() = default;
};

// A library: the host's FT_Library over pspocket's host memory, the guest's FT_LibraryRec over the game's FT_Memory.
struct Library : Mirror {
    State* st = nullptr;
    FT_Library ft = nullptr;
    FT_MemoryRec_ hmem{};
    std::recursive_mutex mutex; // one library's calls at a time, as FreeType wants
    std::vector<u32> renderer_nodes; // the guest renderers list's nodes, by renderer
    std::map<const void*, u32> renderer_node_of;
};

struct SlotMirror : Mirror {
    // its guest blocks: the outline's arrays, the subglyphs and the bitmap it owns
    void own_blocks()
    {
        blocks.clear();
        for (const u32 a : {points, tags, contours, subglyphs})
            if (a) blocks.push_back(a);
        if (own_buffer && buffer) blocks.push_back(buffer);
    }
    u32 points = 0, tags = 0, contours = 0; // the outline's guest arrays
    u32 max_points = 0, max_contours = 0;
    u32 subglyphs = 0, max_subglyphs = 0;
    u32 buffer = 0;      // the bitmap's guest buffer when the slot owns it
    bool own_buffer = false;
};

struct SizeMirror : Mirror {
    u32 node = 0; // its node in the face's guest sizes_list
};

struct FaceMirror : Mirror {
    u32 stream = 0;      // the guest FT_StreamRec shown as face->stream
    u32 charmaps = 0;    // the guest array of FT_CharMap
    std::map<u32, u32> sfnt_tables;  // FT_Sfnt_Tag -> its guest copy
    std::map<u32, u32> sfnt_names;   // index -> the guest copy of its string
    u32 ps_name = 0;     // FT_Get_Postscript_Name's guest copy
    std::map<std::string, u32> strings; // other strings the face shows the guest (PS font info)
};

State& state();

// The mirror of host object `host` (of kind `k`), or null.
Mirror* by_host(const void* host);
// The mirror whose guest object is `guest`, of kind `k`, or null.
Mirror* by_guest(u32 guest, Mirror::Kind k);
template <class T>
T* by_guest_as(u32 guest, Mirror::Kind k)
{
    return static_cast<T*>(by_guest(guest, k));
}
// Registers a mirror made by the caller (its guest object and host key set). Takes ownership.
Mirror* add_mirror(std::unique_ptr<Mirror> m);
// Frees a mirror's guest blocks and forgets it (what a host free of its object does).
void release_mirror(Mirror* m);

// The face whose guest FT_StreamRec (pspocket's mirror of a file stream) is `guest_stream`, or null.
FaceMirror* face_of_stream(u32 guest_stream);
// Host memory of no library's, its blocks laid out as a library's (a block of either may be freed through the other:
// FreeType frees a compressed stream's state through the memory of the library whose face used it).
FT_Memory plain_memory();
// The mirrors of one kind a library has.
std::vector<Mirror*> mirrors_of(Library* l, Mirror::Kind k);

// The library a guest FT_Library names (null, logged, for one that is not a library).
Library* library_of(u32 guest);
// The face, slot, size and charmap mirrors of guest handles.
FaceMirror* face_of(u32 guest);
SlotMirror* slot_of(u32 guest);
SizeMirror* size_of(u32 guest);
Mirror* charmap_of(u32 guest);
// The guest object of a host one (0 when it has no mirror).
u32 guest_of(const void* host);

// Locks a library for a call (FreeType's objects are one library's at a time).
using LibLock = std::unique_lock<std::recursive_mutex>;
inline LibLock lock(Library* l) { return l ? LibLock(l->mutex) : LibLock(); }

// ---------------------------------------------------------------------------------------------------- libraries
// FT_New_Library's work: the guest record, the host library. Error as FreeType's.
FT_Error new_library(u32 memory, u32* out);
FT_Error done_library(Library* l);
// The library's guest record rewritten from the host's: modules, renderers, the current renderer, the auto-hinter.
FT_Error sync_library(Library* l);
// The module mirror of a host module (made when missing); 0 when memory failed.
u32 module_guest(Library* l, FT_Module m, FT_Error& error);
// The host module a guest FT_Module names, or null.
FT_Module module_of(u32 guest);
// The guest class mirror of a host module class (process-wide).
u32 class_guest(const FT_Module_Class* clazz);
// The host module class a guest one stands for, or null.
const FT_Module_Class* class_of(u32 guest);

// ---------------------------------------------------------------------------------------------------- faces
// The face's mirror made after the host's FT_Open_Face: Out_Of_Memory (the host face done) when the game's memory
// failed.
FT_Error make_face(Library* l, FT_Face face, u32* out);
// The face's guest record rewritten from the host's (its charmap, size, sizes and slot); with `slot_image` the slot's
// image too (after a load or a render).
FT_Error sync_face(FaceMirror* f, bool slot_image);
// The slot's guest record from the host's.
FT_Error sync_slot(SlotMirror* s);
// The guest's changes to a slot's outline and format read back into the host's slot (before a render).
void read_back_slot(SlotMirror* s);
FT_Error sync_size(SizeMirror* s);
// A size's mirror (made when missing); 0 when memory failed.
u32 size_guest(FaceMirror* f, FT_Size size, FT_Error& error);
u32 slot_guest(FaceMirror* f, FT_GlyphSlot slot, FT_Error& error);

// ---------------------------------------------------------------------------------------------------- outlines
// A guest FT_Outline as a host one (arrays owned by the holder) and back.
struct HostOutline {
    FT_Outline o{};
    std::vector<FT_Vector> points;
    std::vector<char> tags;
    std::vector<short> contours;
};
void read_outline(u32 guest, HostOutline& out, u32 extra_points = 0, u32 extra_contours = 0);
// Writes the host outline's header and arrays into the guest outline's arrays (which must be large enough).
void write_outline(u32 guest, const FT_Outline& o);
// A guest FT_Bitmap as a host one: its buffer is the guest's own memory.
FT_Bitmap read_bitmap(u32 guest);
void write_bitmap(u32 guest, const FT_Bitmap& b);
// The guest's FT_Vector, FT_Matrix
FT_Vector read_vector(u32 guest);
void write_vector(u32 guest, const FT_Vector& v);
FT_Matrix read_matrix(u32 guest);

// ---------------------------------------------------------------------------------------------------- streams
// A host stream reading a guest FT_StreamRec (its memory, or its read function). Its close (FreeType's, when the face
// it was opened with is done) calls the guest's and frees it.
FT_Stream bridge_stream(u32 guest_stream);
void free_bridge_stream(FT_Stream s);
// The guest FT_StreamRec behind a host stream: a bridge's, or a gzip or LZW stream's (freetype_glyph.cpp); 0 if none.
u32 guest_stream_of(FT_Stream s);
u32 compressed_guest_stream(FT_Stream s);
// A compressed stream of pspocket's that a guest FT_StreamRec stands for (FT_Stream_OpenGzip, FT_Stream_OpenLZW), or
// null: FT_Open_Face uses it as it is.
FT_Stream host_stream_for(u32 guest_stream);
// The handler Sony's file layer calls when a read fails with CELL_EIO or CELL_EFSSPECIFIC (cell_FreeType2_904CA41F).
void set_file_handler(u32 fn, u32 arg);

// ---------------------------------------------------------------------------------------------------- calls
// An HLE function of another module (sys_fs's cellFs functions), called as the guest would: r3.
u64 call_hle(std::string_view module, u32 nid, std::initializer_list<u64> args);

} // namespace pspocket::hle::ftb
