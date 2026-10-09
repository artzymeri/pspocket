// cell_FreeType2 (D-525), part 2: outlines, bitmaps, glyph images, the stroker and the compressed streams.
//
// What the guest owns outright is worked on in guest memory as FreeType 2.2.1 works on it, through the game's
// FT_Memory with FreeType's allocations: FT_Outline (src/base/ftoutln.c, ported for the 32-bit layout; FT_Outline_New
// allocates twice the points it is asked for, as 2.2.1 does), FT_Glyph (src/base/ftglyph.c, ported), FT_Bitmap
// (src/base/ftbitmap.c run on the host over the guest's buffer, its memory forwarded to the game's FT_Memory). Rendering,
// bounding boxes and stroking run in the host's FreeType over host copies of the guest's outlines.

#include "pspocket/hle/hle.hpp"

#if PSPOCKET_HAVE_FREETYPE

#include "freetype_bridge.hpp"
#include "freetype_calc.hpp"

#include FT_INTERNAL_OBJECTS_H
#include FT_INTERNAL_STREAM_H
#include FT_GZIP_H
#include FT_LZW_H
#include FT_BBOX_H

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {

namespace g {

using namespace ftb;
using namespace ftcalc;

constexpr const char* kGChan = "freetype";
constexpr u32 kOwner = FT_OUTLINE_OWNER;

// ------------------------------------------------------------------------------------------------- guest memory
// The memory of a guest FT_Library: its record's first word, read as Sony's code reads it.
u32 lib_memory(u32 library) { return library ? rd32(library + lay::lib_memory) : 0; }

// A host FT_Memory over the game's: blocks are guest memory, given to the host's FreeType as host pointers. For host
// code that allocates byte buffers only (ftbitmap.c), so its allocations are Sony's to the byte.
struct Forward : FT_MemoryRec_ {
    u32 guest = 0;
};

void* fwd_alloc(FT_Memory m, long size)
{
    const u32 g = static_cast<u32>(call_guest_fn(rd32(static_cast<Forward*>(m)->guest + lay::mem_alloc),
        {static_cast<Forward*>(m)->guest, static_cast<u32>(size)}));
    return g ? hp(g) : nullptr;
}

void fwd_free(FT_Memory m, void* block)
{
    call_guest_fn(rd32(static_cast<Forward*>(m)->guest + lay::mem_free), {static_cast<Forward*>(m)->guest, gp(block)});
}

void* fwd_realloc(FT_Memory m, long cur, long size, void* block)
{
    const u32 g = static_cast<u32>(call_guest_fn(rd32(static_cast<Forward*>(m)->guest + lay::mem_realloc),
        {static_cast<Forward*>(m)->guest, static_cast<u32>(cur), static_cast<u32>(size), gp(block)}));
    return g ? hp(g) : nullptr;
}

struct ForwardLibrary {
    Forward memory;
    FT_LibraryRec lib{};
    explicit ForwardLibrary(u32 guest_memory)
    {
        memory.user = nullptr;
        memory.alloc = fwd_alloc;
        memory.free = fwd_free;
        memory.realloc = fwd_realloc;
        memory.guest = guest_memory;
        lib.memory = &memory;
    }
};

// ------------------------------------------------------------------------------------------------- outlines
// src/base/ftoutln.c on the guest's FT_Outline.

s32 outline_done_internal(u32 memory, u32 outline)
{
    if (!memory || !outline) return FT_Err_Invalid_Argument;
    if (rd32(outline + lay::ol_flags) & kOwner) {
        for (const u32 off : {lay::ol_points, lay::ol_tags, lay::ol_contours}) {
            g_free(memory, rd32(outline + off));
            wr32(outline + off, 0);
        }
    }
    std::memset(hp(outline), 0, lay::ol_rec);
    return FT_Err_Ok;
}

s32 outline_new_internal(u32 memory, u32 num_points, s32 num_contours, u32 outline)
{
    if (!outline || !memory) return FT_Err_Invalid_Argument;
    std::memset(hp(outline), 0, lay::ol_rec);
    FT_Error error = FT_Err_Ok;
    // FT_NEW_ARRAY(points, numPoints * 2L): twice the points (FreeType 2.2.1), counts as the PPU's 32-bit FT_Long
    const s32 n2 = static_cast<s32>(num_points * 2u);
    const u32 points = g_realloc(memory, 8, 0, n2, 0, error);
    wr32(outline + lay::ol_points, points);
    if (!error) {
        const u32 tags = g_realloc(memory, 1, 0, static_cast<s32>(num_points), 0, error);
        wr32(outline + lay::ol_tags, tags);
        if (!error) {
            const u32 contours = g_realloc(memory, 2, 0, num_contours, 0, error);
            wr32(outline + lay::ol_contours, contours);
        }
    }
    if (error) {
        wr32(outline + lay::ol_flags, rd32(outline + lay::ol_flags) | kOwner);
        outline_done_internal(memory, outline);
        return error;
    }
    wr16(outline + lay::ol_n_points, static_cast<u16>(num_points));
    wr16(outline + lay::ol_n_contours, static_cast<u16>(num_contours));
    wr32(outline + lay::ol_flags, rd32(outline + lay::ol_flags) | kOwner);
    return FT_Err_Ok;
}

s32 outline_copy(u32 source, u32 target)
{
    if (!source || !target || rds16(source + lay::ol_n_points) != rds16(target + lay::ol_n_points) ||
        rds16(source + lay::ol_n_contours) != rds16(target + lay::ol_n_contours))
        return FT_Err_Invalid_Argument;
    if (source == target) return FT_Err_Ok;
    const s32 np = rds16(source + lay::ol_n_points), nc = rds16(source + lay::ol_n_contours);
    if (np > 0) {
        std::memmove(hp(rd32(target + lay::ol_points)), hp(rd32(source + lay::ol_points)), 8u * static_cast<u32>(np));
        std::memmove(hp(rd32(target + lay::ol_tags)), hp(rd32(source + lay::ol_tags)), static_cast<u32>(np));
    }
    if (nc > 0)
        std::memmove(hp(rd32(target + lay::ol_contours)), hp(rd32(source + lay::ol_contours)), 2u * static_cast<u32>(nc));
    const u32 is_owner = rd32(target + lay::ol_flags) & kOwner;
    wr32(target + lay::ol_flags, (rd32(source + lay::ol_flags) & ~kOwner) | is_owner);
    return FT_Err_Ok;
}

void outline_translate(u32 outline, s32 dx, s32 dy)
{
    const u32 pts = rd32(outline + lay::ol_points);
    const u16 n = rd16(outline + lay::ol_n_points); // FT_UShort n < outline->n_points
    for (u32 i = 0; static_cast<s32>(i) < static_cast<s16>(n); i++) {
        wr32(pts + 8 * i, static_cast<u32>(add32(rds32(pts + 8 * i), dx)));
        wr32(pts + 8 * i + 4, static_cast<u32>(add32(rds32(pts + 8 * i + 4), dy)));
    }
}

void vector_transform(u32 v, s32 xx, s32 xy, s32 yx, s32 yy)
{
    const s32 x = rds32(v), y = rds32(v + 4);
    wr32(v, static_cast<u32>(add32(mulfix32(x, xx), mulfix32(y, xy))));
    wr32(v + 4, static_cast<u32>(add32(mulfix32(x, yx), mulfix32(y, yy))));
}

void outline_transform(u32 outline, u32 matrix)
{
    if (!outline || !matrix) return;
    const s32 xx = rds32(matrix), xy = rds32(matrix + 4), yx = rds32(matrix + 8), yy = rds32(matrix + 12);
    const u32 pts = rd32(outline + lay::ol_points);
    const s32 n = rds16(outline + lay::ol_n_points);
    for (s32 i = 0; i < n; i++) vector_transform(pts + 8 * static_cast<u32>(i), xx, xy, yx, yy);
}

void outline_cbox(u32 outline, s32 box[4])
{
    s32 xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    const s32 n = rds16(outline + lay::ol_n_points);
    if (n != 0) {
        const u32 pts = rd32(outline + lay::ol_points);
        xmin = xmax = rds32(pts);
        ymin = ymax = rds32(pts + 4);
        for (s32 i = 1; i < n; i++) {
            const s32 x = rds32(pts + 8 * static_cast<u32>(i)), y = rds32(pts + 8 * static_cast<u32>(i) + 4);
            xmin = std::min(xmin, x);
            xmax = std::max(xmax, x);
            ymin = std::min(ymin, y);
            ymax = std::max(ymax, y);
        }
    }
    box[0] = xmin;
    box[1] = ymin;
    box[2] = xmax;
    box[3] = ymax;
}

void write_bbox(u32 at, const s32 box[4])
{
    for (u32 i = 0; i < 4; i++) wr32(at + 4 * i, static_cast<u32>(box[i]));
}

s32 outline_orientation(u32 outline)
{
    if (!outline || rds16(outline + lay::ol_n_points) <= 0) return FT_ORIENTATION_TRUETYPE;
    const u32 pts = rd32(outline + lay::ol_points), tags = rd32(outline + lay::ol_tags),
              cts = rd32(outline + lay::ol_contours);
    const s32 nc = rds16(outline + lay::ol_n_contours);
    s32 xmin = 32768, xmin_point = -1, xmin_first = -1, xmin_last = -1;
    s32 first = 0, last = 0;
    auto px = [&](s32 i) { return rds32(pts + 8 * static_cast<u32>(i)); };
    auto py = [&](s32 i) { return rds32(pts + 8 * static_cast<u32>(i) + 4); };
    auto on = [&](s32 i) { return rd8(tags + static_cast<u32>(i)) & 1; };
    for (s32 c = 0; c < nc; c++, first = last + 1) {
        last = rds16(cts + 2 * static_cast<u32>(c));
        if (last < first + 2) continue;
        s32 on_count = 0, tmp_xmin = 32768, tmp_point = -1;
        for (s32 p = first; p <= last; ++p) {
            const s32 o = on(p);
            on_count += o;
            if (px(p) < tmp_xmin && o) {
                tmp_xmin = px(p);
                tmp_point = p;
            }
        }
        if (on_count > 2 && tmp_xmin < xmin) {
            xmin = tmp_xmin;
            xmin_point = tmp_point;
            xmin_first = first;
            xmin_last = last;
        }
    }
    if (xmin_point < 0) return FT_ORIENTATION_TRUETYPE;
    s32 prev = xmin_point == xmin_first ? xmin_last : xmin_point - 1;
    s32 next = xmin_point == xmin_last ? xmin_first : xmin_point + 1;
    while (on(prev) == 0) prev = prev == xmin_first ? xmin_last : prev - 1;
    while (on(next) == 0) next = next == xmin_last ? xmin_first : next + 1;
    if (atan2_32(sub32(px(prev), px(xmin_point)), sub32(py(prev), py(xmin_point))) >
        atan2_32(sub32(px(next), px(xmin_point)), sub32(py(next), py(xmin_point))))
        return FT_ORIENTATION_POSTSCRIPT;
    return FT_ORIENTATION_TRUETYPE;
}

s32 FT_Outline_New(PPUContext& ctx, u32 library, u32 num_points, s32 num_contours, u32 outline)
{
    if (!library) return FT_Err_Invalid_Library_Handle;
    Scope sc(ctx);
    return outline_new_internal(lib_memory(library), num_points, num_contours, outline);
}

s32 FT_Outline_Done(PPUContext& ctx, u32 library, u32 outline)
{
    if (!library) return FT_Err_Invalid_Library_Handle;
    Scope sc(ctx);
    return outline_done_internal(lib_memory(library), outline);
}

s32 FT_Outline_Copy(PPUContext& ctx, u32 source, u32 target)
{
    Scope sc(ctx);
    return outline_copy(source, target);
}

void FT_Outline_Translate(PPUContext& ctx, u32 outline, s32 dx, s32 dy)
{
    if (!outline) return;
    Scope sc(ctx);
    outline_translate(outline, dx, dy);
}

void FT_Outline_Transform(PPUContext& ctx, u32 outline, u32 matrix)
{
    Scope sc(ctx);
    outline_transform(outline, matrix);
}

void FT_Outline_Reverse(PPUContext& ctx, u32 outline)
{
    if (!outline) return;
    Scope sc(ctx);
    const u32 pts = rd32(outline + lay::ol_points), tags = rd32(outline + lay::ol_tags),
              cts = rd32(outline + lay::ol_contours);
    s32 first = 0;
    const u16 nc = rd16(outline + lay::ol_n_contours); // FT_UShort n < outline->n_contours
    for (u32 n = 0; static_cast<s32>(n) < static_cast<s16>(nc); n++) {
        const s32 last = rds16(cts + 2 * n);
        for (s32 p = first, q = last; p < q; p++, q--) {
            const u64 a = ps_read64(gbase(), pts + 8 * static_cast<u32>(p));
            ps_write64(gbase(), pts + 8 * static_cast<u32>(p), ps_read64(gbase(), pts + 8 * static_cast<u32>(q)));
            ps_write64(gbase(), pts + 8 * static_cast<u32>(q), a);
            const u8 t = rd8(tags + static_cast<u32>(p));
            wr8(tags + static_cast<u32>(p), rd8(tags + static_cast<u32>(q)));
            wr8(tags + static_cast<u32>(q), t);
        }
        first = last + 1;
    }
    wr32(outline + lay::ol_flags, rd32(outline + lay::ol_flags) ^ FT_OUTLINE_REVERSE_FILL);
}

s32 FT_Outline_Check(PPUContext& ctx, u32 outline)
{
    if (!outline) return FT_Err_Invalid_Argument;
    Scope sc(ctx);
    const s32 np = rds16(outline + lay::ol_n_points), nc = rds16(outline + lay::ol_n_contours);
    if (np == 0 && nc == 0) return FT_Err_Ok;
    if (np <= 0 || nc <= 0) return FT_Err_Invalid_Argument;
    const u32 cts = rd32(outline + lay::ol_contours);
    s32 end0 = -1, end = -1;
    for (s32 n = 0; n < nc; n++) {
        end = rds16(cts + 2 * static_cast<u32>(n));
        if (end <= end0 || end >= np) return FT_Err_Invalid_Argument;
        end0 = end;
    }
    return end == np - 1 ? FT_Err_Ok : FT_Err_Invalid_Argument;
}

void FT_Outline_Get_CBox(PPUContext& ctx, u32 outline, u32 acbox)
{
    if (!outline || !acbox) return;
    Scope sc(ctx);
    s32 box[4];
    outline_cbox(outline, box);
    // FreeType writes xMin, xMax, yMin, yMax
    write_bbox(acbox, box);
}

s32 FT_Outline_Get_BBox(PPUContext& ctx, u32 outline, u32 abbox)
{
    Scope sc(ctx);
    FT_BBox none{};
    if (!outline) return ::FT_Outline_Get_BBox(nullptr, abbox ? &none : nullptr);
    HostOutline o;
    read_outline(outline, o);
    FT_BBox b{};
    const FT_Error e = ::FT_Outline_Get_BBox(&o.o, abbox ? &b : nullptr);
    if (!e && abbox) {
        const s32 box[4] = {static_cast<s32>(b.xMin), static_cast<s32>(b.yMin), static_cast<s32>(b.xMax),
            static_cast<s32>(b.yMax)};
        write_bbox(abbox, box);
    }
    return e;
}

s32 FT_Outline_Embolden(PPUContext& ctx, u32 outline, s32 strength)
{
    if (!outline) return FT_Err_Invalid_Argument;
    Scope sc(ctx);
    strength /= 2;
    if (strength == 0) return FT_Err_Ok;
    const s32 orientation = outline_orientation(outline);
    const s32 nc = rds16(outline + lay::ol_n_contours);
    if (orientation == FT_ORIENTATION_NONE) return nc ? FT_Err_Invalid_Argument : FT_Err_Ok;
    const s32 rotate = orientation == FT_ORIENTATION_TRUETYPE ? -kAnglePi2 : kAnglePi2;
    const u32 pts = rd32(outline + lay::ol_points), cts = rd32(outline + lay::ol_contours);
    auto read = [&](s32 i) { return Vec{rds32(pts + 8 * static_cast<u32>(i)), rds32(pts + 8 * static_cast<u32>(i) + 4)}; };
    s32 first = 0;
    for (s32 c = 0; c < nc; c++) {
        const s32 last = rds16(cts + 2 * static_cast<u32>(c));
        const Vec v_first = read(first);
        Vec v_prev = read(last), v_cur = v_first, v_next{};
        for (s32 n = first; n <= last; n++) {
            v_next = n < last ? read(n + 1) : v_first;
            Vec in{sub32(v_cur.x, v_prev.x), sub32(v_cur.y, v_prev.y)};
            const Vec out{sub32(v_next.x, v_cur.x), sub32(v_next.y, v_cur.y)};
            const s32 angle_in = atan2_32(in.x, in.y);
            const s32 angle_out = atan2_32(out.x, out.y);
            const s32 diff = angle_diff32(angle_in, angle_out);
            const s32 scale = cos32(diff / 2);
            if (scale < 0x4000 && scale > -0x4000) {
                in.x = in.y = 0;
            } else {
                const s32 d = divfix32(strength, scale);
                in = Vec{d, 0};
                vector_rotate32(in, sub32(add32(angle_in, diff / 2), rotate));
            }
            wr32(pts + 8 * static_cast<u32>(n), static_cast<u32>(add32(add32(v_cur.x, strength), in.x)));
            wr32(pts + 8 * static_cast<u32>(n) + 4, static_cast<u32>(add32(add32(v_cur.y, strength), in.y)));
            v_prev = v_cur;
            v_cur = v_next;
        }
        first = last + 1;
    }
    return FT_Err_Ok;
}

u32 FT_Outline_Get_Orientation(PPUContext& ctx, u32 outline)
{
    Scope sc(ctx);
    return static_cast<u32>(outline_orientation(outline));
}

u32 FT_Outline_GetInsideBorder(PPUContext& ctx, u32 outline)
{
    Scope sc(ctx);
    return outline_orientation(outline) == FT_ORIENTATION_TRUETYPE ? FT_STROKER_BORDER_RIGHT : FT_STROKER_BORDER_LEFT;
}

u32 FT_Outline_GetOutsideBorder(PPUContext& ctx, u32 outline)
{
    Scope sc(ctx);
    return outline_orientation(outline) == FT_ORIENTATION_TRUETYPE ? FT_STROKER_BORDER_LEFT : FT_STROKER_BORDER_RIGHT;
}

// FT_Outline_Decompose: the guest's callbacks get pointers to vectors on its stack, as Sony's get its locals.
s32 FT_Outline_Decompose(PPUContext& ctx, u32 outline, u32 funcs, u32 user)
{
    if (!outline || !funcs) return FT_Err_Invalid_Argument;
    Scope sc(ctx);
    const u32 move_to = rd32(funcs + lay::of_move_to), line_to = rd32(funcs + lay::of_line_to),
              conic_to = rd32(funcs + lay::of_conic_to), cubic_to = rd32(funcs + lay::of_cubic_to);
    const s32 shift = rds32(funcs + lay::of_shift), delta = rds32(funcs + lay::of_delta);
    StackVar vars(ctx, gbase(), 32);
    const u32 v0 = vars.addr(), v1 = v0 + 8, v2 = v0 + 16;
    auto put = [&](u32 at, const Vec& v) {
        wr32(at, static_cast<u32>(v.x));
        wr32(at + 4, static_cast<u32>(v.y));
    };
    auto call = [&](u32 fn, std::initializer_list<u64> args) -> s32 {
        if (!fn) {
            PS_LOG_ERROR(kGChan, "FT_Outline_Decompose: no callback");
            return FT_Err_Invalid_Argument;
        }
        return static_cast<s32>(call_guest_fn(fn, args));
    };
    const u32 pts = rd32(outline + lay::ol_points), tags = rd32(outline + lay::ol_tags),
              cts = rd32(outline + lay::ol_contours);
    auto scaled = [&](s32 i) {
        return Vec{sub32(shl32(rds32(pts + 8 * static_cast<u32>(i)), shift), delta),
            sub32(shl32(rds32(pts + 8 * static_cast<u32>(i) + 4), shift), delta)};
    };
    auto tag_of = [&](s32 i) { return rd8(tags + static_cast<u32>(i)) & 3; };
    const s32 nc = rds16(outline + lay::ol_n_contours);
    s32 first = 0;
    for (s32 n = 0; n < nc; n++) {
        const s32 last = rds16(cts + 2 * static_cast<u32>(n));
        s32 limit = last;
        Vec v_start = scaled(first), v_last = scaled(last), v_control = v_start;
        s32 point = first;
        s32 tag = tag_of(first);
        if (tag == FT_CURVE_TAG_CUBIC) return FT_Err_Invalid_Outline;
        if (tag == FT_CURVE_TAG_CONIC) {
            if (tag_of(last) == FT_CURVE_TAG_ON) {
                v_start = v_last;
                limit--;
            } else {
                v_start.x = add32(v_start.x, v_last.x) / 2;
                v_start.y = add32(v_start.y, v_last.y) / 2;
                v_last = v_start;
            }
            point--;
        }
        put(v0, v_start);
        s32 error = call(move_to, {v0, user});
        if (error) return error;
        bool closed = false;
        while (point < limit) {
            point++;
            tag = tag_of(point);
            if (tag == FT_CURVE_TAG_ON) {
                put(v0, scaled(point));
                error = call(line_to, {v0, user});
                if (error) return error;
                continue;
            }
            if (tag == FT_CURVE_TAG_CONIC) {
                v_control = scaled(point);
                for (;;) {
                    if (point < limit) {
                        point++;
                        tag = tag_of(point);
                        const Vec vec = scaled(point);
                        if (tag == FT_CURVE_TAG_ON) {
                            put(v1, v_control);
                            put(v0, vec);
                            error = call(conic_to, {v1, v0, user});
                            if (error) return error;
                            break;
                        }
                        if (tag != FT_CURVE_TAG_CONIC) return FT_Err_Invalid_Outline;
                        const Vec middle{add32(v_control.x, vec.x) / 2, add32(v_control.y, vec.y) / 2};
                        put(v1, v_control);
                        put(v0, middle);
                        error = call(conic_to, {v1, v0, user});
                        if (error) return error;
                        v_control = vec;
                        continue;
                    }
                    put(v1, v_control);
                    put(v0, v_start);
                    error = call(conic_to, {v1, v0, user});
                    closed = true;
                    break;
                }
                if (closed) break;
                continue;
            }
            // FT_CURVE_TAG_CUBIC
            if (point + 1 > limit || tag_of(point + 1) != FT_CURVE_TAG_CUBIC) return FT_Err_Invalid_Outline;
            point += 2;
            put(v1, scaled(point - 2));
            put(v2, scaled(point - 1));
            if (point <= limit) {
                put(v0, scaled(point));
                error = call(cubic_to, {v1, v2, v0, user});
                if (error) return error;
                continue;
            }
            put(v0, v_start);
            error = call(cubic_to, {v1, v2, v0, user});
            closed = true;
            break;
        }
        if (!closed) {
            put(v0, v_start);
            error = call(line_to, {v0, user});
        }
        if (error) return error;
        first = last + 1;
    }
    return FT_Err_Ok;
}

// Rendering a guest outline with the host's renderers.
struct SpanCall {
    u32 fn = 0, user = 0;
};

void span_bridge(int y, int count, const FT_Span* spans, void* user)
{
    auto* sc = static_cast<SpanCall*>(user);
    PPUContext& ctx = *current_ctx();
    StackVar buf(ctx, gbase(), 6 * static_cast<u32>(std::max(count, 1)) + 8);
    for (int i = 0; i < count; i++) {
        const u32 at = buf.addr() + 6 * static_cast<u32>(i);
        wr16(at, static_cast<u16>(spans[i].x));
        wr16(at + 2, spans[i].len);
        wr8(at + 4, spans[i].coverage);
    }
    call_guest_fn(sc->fn, {static_cast<u64>(static_cast<s64>(y)), static_cast<u64>(static_cast<s64>(count)),
        buf.addr(), sc->user});
}

s32 FT_Outline_Render(PPUContext& ctx, u32 library, u32 outline, u32 params)
{
    Scope sc(ctx);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    if (!outline || !params) return FT_Err_Invalid_Argument;
    LibLock lk = lock(l);
    wr32(params + lay::rp_source, outline); // params->source = outline, as FreeType does
    HostOutline o;
    read_outline(outline, o);
    FT_Raster_Params p{};
    const u32 target = rd32(params + lay::rp_target);
    FT_Bitmap bitmap{};
    if (target) {
        bitmap = read_bitmap(target);
        p.target = &bitmap;
    }
    p.source = &o.o;
    p.flags = rds32(params + lay::rp_flags);
    SpanCall spans{rd32(params + lay::rp_gray_spans), rd32(params + lay::rp_user)};
    if (spans.fn) {
        p.gray_spans = span_bridge;
        p.user = &spans;
    }
    p.clip_box.xMin = rds32(params + lay::rp_clip_box);
    p.clip_box.yMin = rds32(params + lay::rp_clip_box + 4);
    p.clip_box.xMax = rds32(params + lay::rp_clip_box + 8);
    p.clip_box.yMax = rds32(params + lay::rp_clip_box + 12);
    const FT_Error e = ::FT_Outline_Render(l->ft, &o.o, &p);
    sync_library(l);
    return e;
}

s32 FT_Outline_Get_Bitmap(PPUContext& ctx, u32 library, u32 outline, u32 abitmap)
{
    Scope sc(ctx);
    if (!abitmap) return FT_Err_Invalid_Argument;
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    if (!outline) return FT_Err_Invalid_Argument;
    LibLock lk = lock(l);
    HostOutline o;
    read_outline(outline, o);
    const FT_Bitmap b = read_bitmap(abitmap);
    const FT_Error e = ::FT_Outline_Get_Bitmap(l->ft, &o.o, &b);
    sync_library(l);
    return e;
}

// ------------------------------------------------------------------------------------------------- bitmaps

// A guest bitmap a slot shows: the slot keeps owning its (possibly new) buffer.
void bitmap_changed(u32 bitmap)
{
    if (SlotMirror* s = by_guest_as<SlotMirror>(bitmap - lay::slot_bitmap, Mirror::Kind::slot)) {
        s->buffer = rd32(bitmap + lay::bm_buffer);
        s->own_blocks();
    }
}

void FT_Bitmap_New(PPUContext& ctx, u32 abitmap)
{
    Scope sc(ctx);
    std::memset(hp(abitmap), 0, lay::bm_rec);
}

s32 FT_Bitmap_Copy(PPUContext& ctx, u32 library, u32 source, u32 target)
{
    if (!library) return FT_Err_Invalid_Library_Handle;
    if (source == target) return FT_Err_Ok;
    Scope sc(ctx);
    ForwardLibrary fl(lib_memory(library));
    const FT_Bitmap s = read_bitmap(source);
    FT_Bitmap t = read_bitmap(target);
    const FT_Error e = ::FT_Bitmap_Copy(&fl.lib, &s, &t);
    write_bitmap(target, t);
    bitmap_changed(target);
    return e;
}

s32 FT_Bitmap_Embolden(PPUContext& ctx, u32 library, u32 bitmap, s32 xs, s32 ys)
{
    if (!library) return FT_Err_Invalid_Library_Handle;
    if (!bitmap) return FT_Err_Invalid_Argument;
    Scope sc(ctx);
    ForwardLibrary fl(lib_memory(library));
    FT_Bitmap b = read_bitmap(bitmap);
    const FT_Error e = ::FT_Bitmap_Embolden(&fl.lib, &b, xs, ys);
    write_bitmap(bitmap, b);
    bitmap_changed(bitmap);
    return e;
}

s32 FT_Bitmap_Convert(PPUContext& ctx, u32 library, u32 source, u32 target, s32 alignment)
{
    if (!library) return FT_Err_Invalid_Library_Handle;
    Scope sc(ctx);
    ForwardLibrary fl(lib_memory(library));
    const FT_Bitmap s = read_bitmap(source);
    FT_Bitmap t = read_bitmap(target);
    const FT_Error e = ::FT_Bitmap_Convert(&fl.lib, &s, &t, alignment);
    write_bitmap(target, t);
    bitmap_changed(target);
    return e;
}

s32 FT_Bitmap_Done(PPUContext& ctx, u32 library, u32 bitmap)
{
    if (!library) return FT_Err_Invalid_Library_Handle;
    if (!bitmap) return FT_Err_Invalid_Argument;
    Scope sc(ctx);
    ForwardLibrary fl(lib_memory(library));
    FT_Bitmap b = read_bitmap(bitmap);
    const FT_Error e = ::FT_Bitmap_Done(&fl.lib, &b);
    write_bitmap(bitmap, b);
    bitmap_changed(bitmap);
    return e;
}

// ------------------------------------------------------------------------------------------------- glyph images
// src/base/ftglyph.c on the guest's FT_Glyph. FreeType's two glyph classes are process-wide guest records (their
// functions pspocket's own); a class of the game's own has its functions called on the guest.

struct GlyphClasses {
    u32 bitmap = 0, outline = 0;
};

// The process's glyph classes and compressed streams.
struct Compressed;
struct GlyphState {
    std::mutex mutex;
    GlyphClasses classes;
    std::map<u32, Compressed*> compressed; // by the guest's FT_StreamRec
};

GlyphState& glyph_state() { return HleProcess::current()->state<GlyphState>(); }

GlyphClasses& glyph_classes()
{
    GlyphState& st = glyph_state();
    std::lock_guard lk(st.mutex);
    GlyphClasses& c = st.classes;
    if (!c.bitmap) {
        const u32 a = HleProcess::current()->main_memory().alloc(2 * lay::gc_rec, 0x10);
        if (a) {
            std::memset(hp(a), 0, 2 * lay::gc_rec);
            c.bitmap = a;
            c.outline = a + lay::gc_rec;
            wr32(c.bitmap + lay::gc_size, lay::bgl_rec);
            wr32(c.bitmap + lay::gc_format, FT_GLYPH_FORMAT_BITMAP);
            wr32(c.outline + lay::gc_size, lay::ogl_rec);
            wr32(c.outline + lay::gc_format, FT_GLYPH_FORMAT_OUTLINE);
        }
    }
    return c;
}

u32 glyph_library(u32 glyph) { return rd32(glyph + lay::gl_library); }

s32 new_glyph(u32 library, u32 clazz, u32* out)
{
    *out = 0;
    FT_Error error = FT_Err_Ok;
    const u32 g = g_alloc(lib_memory(library), rds32(clazz + lay::gc_size), error);
    if (!error) {
        wr32(g + lay::gl_library, library);
        wr32(g + lay::gl_clazz, clazz);
        wr32(g + lay::gl_format, rd32(clazz + lay::gc_format));
        *out = g;
    }
    return error;
}

// FT_Bitmap_Copy on guest bitmaps through `library`'s memory.
s32 bitmap_copy(u32 library, u32 source, u32 target)
{
    if (source == target) return FT_Err_Ok;
    ForwardLibrary fl(lib_memory(library));
    const FT_Bitmap s = read_bitmap(source);
    FT_Bitmap t = read_bitmap(target);
    const FT_Error e = ::FT_Bitmap_Copy(&fl.lib, &s, &t);
    write_bitmap(target, t);
    return e;
}

void bitmap_done(u32 library, u32 bitmap)
{
    ForwardLibrary fl(lib_memory(library));
    FT_Bitmap b = read_bitmap(bitmap);
    ::FT_Bitmap_Done(&fl.lib, &b);
    write_bitmap(bitmap, b);
}

void done_glyph(u32 glyph)
{
    if (!glyph) return;
    const u32 library = glyph_library(glyph), clazz = rd32(glyph + lay::gl_clazz);
    const GlyphClasses& gc = glyph_classes();
    if (clazz == gc.bitmap) {
        bitmap_done(library, glyph + lay::bgl_bitmap);
    } else if (clazz == gc.outline) {
        outline_done_internal(lib_memory(library), glyph + lay::ogl_outline);
    } else if (const u32 fn = rd32(clazz + lay::gc_done)) {
        call_guest_fn(fn, {glyph});
    }
    g_free(lib_memory(library), glyph);
}

void glyph_transform_class(u32 glyph, u32 matrix, u32 delta)
{
    // ft_outline_glyph_transform
    if (matrix) outline_transform(glyph + lay::ogl_outline, matrix);
    if (delta) outline_translate(glyph + lay::ogl_outline, rds32(delta), rds32(delta + 4));
}

s32 glyph_transform(u32 glyph, u32 matrix, u32 delta)
{
    if (!glyph || !rd32(glyph + lay::gl_clazz)) return FT_Err_Invalid_Argument;
    const u32 clazz = rd32(glyph + lay::gl_clazz);
    const GlyphClasses& gc = glyph_classes();
    if (clazz == gc.bitmap) return FT_Err_Invalid_Glyph_Format;
    if (clazz == gc.outline) {
        glyph_transform_class(glyph, matrix, delta);
    } else if (const u32 fn = rd32(clazz + lay::gc_transform)) {
        call_guest_fn(fn, {glyph, matrix, delta});
    } else {
        return FT_Err_Invalid_Glyph_Format;
    }
    if (matrix) {
        vector_transform(glyph + lay::gl_advance, rds32(matrix), rds32(matrix + 4), rds32(matrix + 8),
            rds32(matrix + 12));
    }
    return FT_Err_Ok;
}

s32 glyph_copy(u32 source, u32 target)
{
    if (!target || !source || !rd32(source + lay::gl_clazz)) return FT_Err_Invalid_Argument;
    wr32(target, 0);
    const u32 clazz = rd32(source + lay::gl_clazz);
    const u32 library = glyph_library(source);
    u32 copy = 0;
    FT_Error error = new_glyph(library, clazz, &copy);
    if (error) return error;
    wr32(copy + lay::gl_advance, rd32(source + lay::gl_advance));
    wr32(copy + lay::gl_advance + 4, rd32(source + lay::gl_advance + 4));
    wr32(copy + lay::gl_format, rd32(source + lay::gl_format));
    const GlyphClasses& gc = glyph_classes();
    if (clazz == gc.bitmap) {
        wr32(copy + lay::bgl_left, rd32(source + lay::bgl_left));
        wr32(copy + lay::bgl_top, rd32(source + lay::bgl_top));
        error = bitmap_copy(library, source + lay::bgl_bitmap, copy + lay::bgl_bitmap);
    } else if (clazz == gc.outline) {
        const u32 so = source + lay::ogl_outline, to = copy + lay::ogl_outline;
        error = outline_new_internal(lib_memory(library), rd16(so + lay::ol_n_points), rds16(so + lay::ol_n_contours),
            to);
        if (!error) outline_copy(so, to);
    } else if (const u32 fn = rd32(clazz + lay::gc_copy)) {
        error = static_cast<s32>(call_guest_fn(fn, {source, copy}));
    }
    if (error) done_glyph(copy);
    else wr32(target, copy);
    return error;
}

void glyph_cbox(u32 glyph, u32 mode, s32 box[4])
{
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!glyph || !rd32(glyph + lay::gl_clazz)) return;
    const u32 clazz = rd32(glyph + lay::gl_clazz);
    const GlyphClasses& gc = glyph_classes();
    if (clazz == gc.bitmap) {
        box[0] = shl32(rds32(glyph + lay::bgl_left), 6);
        box[2] = add32(box[0], shl32(rds32(glyph + lay::bgl_bitmap + lay::bm_width), 6));
        box[3] = shl32(rds32(glyph + lay::bgl_top), 6);
        box[1] = sub32(box[3], shl32(rds32(glyph + lay::bgl_bitmap + lay::bm_rows), 6));
    } else if (clazz == gc.outline) {
        outline_cbox(glyph + lay::ogl_outline, box);
    } else if (const u32 fn = rd32(clazz + lay::gc_bbox)) {
        StackVar b(*current_ctx(), gbase(), 16);
        call_guest_fn(fn, {glyph, b.addr()});
        for (u32 i = 0; i < 4; i++) box[i] = rds32(b.addr() + 4 * i);
    } else {
        return;
    }
    if (mode == FT_GLYPH_BBOX_GRIDFIT || mode == FT_GLYPH_BBOX_PIXELS) {
        box[0] = box[0] & ~63;
        box[1] = box[1] & ~63;
        box[2] = add32(box[2], 63) & ~63;
        box[3] = add32(box[3], 63) & ~63;
    }
    if (mode == FT_GLYPH_BBOX_TRUNCATE || mode == FT_GLYPH_BBOX_PIXELS) {
        for (u32 i = 0; i < 4; i++) box[i] >>= 6;
    }
}

void FT_Done_Glyph(PPUContext& ctx, u32 glyph)
{
    if (!glyph) return;
    Scope sc(ctx);
    done_glyph(glyph);
}

s32 FT_Glyph_Copy(PPUContext& ctx, u32 source, u32 target)
{
    Scope sc(ctx);
    return glyph_copy(source, target);
}

s32 FT_Glyph_Transform(PPUContext& ctx, u32 glyph, u32 matrix, u32 delta)
{
    Scope sc(ctx);
    return glyph_transform(glyph, matrix, delta);
}

void FT_Glyph_Get_CBox(PPUContext& ctx, u32 glyph, u32 mode, u32 acbox)
{
    if (!acbox) return;
    Scope sc(ctx);
    s32 box[4];
    glyph_cbox(glyph, mode, box);
    write_bbox(acbox, box);
}

s32 FT_Get_Glyph(PPUContext& ctx, u32 slot, u32 aglyph)
{
    if (!slot) return FT_Err_Invalid_Slot_Handle;
    Scope sc(ctx);
    if (!aglyph) return FT_Err_Invalid_Argument;
    const u32 library = rd32(slot + lay::slot_library);
    const u32 format = rd32(slot + lay::slot_format);
    const GlyphClasses& gc = glyph_classes();
    u32 clazz = 0;
    if (format == FT_GLYPH_FORMAT_BITMAP) clazz = gc.bitmap;
    else if (format == FT_GLYPH_FORMAT_OUTLINE) clazz = gc.outline;
    if (!clazz) return FT_Err_Invalid_Glyph_Format; // no renderer of other formats
    u32 glyph = 0;
    FT_Error error = new_glyph(library, clazz, &glyph);
    if (error) return error;
    wr32(glyph + lay::gl_advance, static_cast<u32>(shl32(rds32(slot + lay::slot_advance), 10)));
    wr32(glyph + lay::gl_advance + 4, static_cast<u32>(shl32(rds32(slot + lay::slot_advance + 4), 10)));
    if (clazz == gc.bitmap) {
        // ft_bitmap_glyph_init: a bitmap the slot owns goes to the glyph, else it is copied
        wr32(glyph + lay::bgl_left, rd32(slot + lay::slot_bitmap_left));
        wr32(glyph + lay::bgl_top, rd32(slot + lay::slot_bitmap_top));
        SlotMirror* s = by_guest_as<SlotMirror>(slot, Mirror::Kind::slot);
        if (s && s->own_buffer) {
            std::memcpy(hp(glyph + lay::bgl_bitmap), hp(slot + lay::slot_bitmap), lay::bm_rec);
            s->own_buffer = false;
            s->buffer = 0;
            s->own_blocks();
        } else {
            std::memset(hp(glyph + lay::bgl_bitmap), 0, lay::bm_rec);
            error = bitmap_copy(library, slot + lay::slot_bitmap, glyph + lay::bgl_bitmap);
        }
    } else {
        // ft_outline_glyph_init
        const u32 so = slot + lay::slot_outline, to = glyph + lay::ogl_outline;
        error = outline_new_internal(lib_memory(library), rd16(so + lay::ol_n_points), rds16(so + lay::ol_n_contours),
            to);
        if (!error) outline_copy(so, to);
    }
    if (error) done_glyph(glyph);
    else wr32(aglyph, glyph);
    return error;
}

// The host's FreeType renders a guest outline in a slot of its own (FT_Glyph_To_Bitmap's dummy).
FT_Error render_guest_outline(Library* l, u32 outline, FT_Render_Mode mode, FT_GlyphSlotRec& dummy,
    FT_GlyphSlot_InternalRec& internal, HostOutline& o)
{
    read_outline(outline, o);
    std::memset(&dummy, 0, sizeof dummy);
    std::memset(&internal, 0, sizeof internal);
    dummy.internal = &internal;
    dummy.library = l->ft;
    dummy.format = FT_GLYPH_FORMAT_OUTLINE;
    dummy.outline = o.o;
    dummy.outline.flags &= ~FT_OUTLINE_OWNER;
    return ::FT_Render_Glyph_Internal(l->ft, &dummy, mode);
}

s32 FT_Glyph_To_Bitmap(PPUContext& ctx, u32 the_glyph, u32 mode, u32 origin, u32 destroy)
{
    Scope sc(ctx);
    if (!the_glyph || !rd32(the_glyph)) return FT_Err_Invalid_Argument;
    const u32 glyph = rd32(the_glyph);
    const u32 clazz = rd32(glyph + lay::gl_clazz);
    const GlyphClasses& gc = glyph_classes();
    if (clazz == gc.bitmap) return FT_Err_Ok;
    if (!clazz || (clazz != gc.outline && !rd32(clazz + lay::gc_prepare))) return FT_Err_Invalid_Argument;
    if (clazz != gc.outline) {
        PS_LOG_ERROR(kGChan, "FT_Glyph_To_Bitmap: the game's own glyph class {:#x} is not supported", clazz);
        return FT_Err_Unimplemented_Feature;
    }
    const u32 library = glyph_library(glyph);
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Library_Handle;
    LibLock lk = lock(l);
    u32 bitmap = 0;
    FT_Error error = new_glyph(library, gc.bitmap, &bitmap);
    if (error) return error;
    if (origin) glyph_transform(glyph, 0, origin);
    FT_GlyphSlotRec dummy;
    FT_GlyphSlot_InternalRec internal;
    HostOutline o;
    error = render_guest_outline(l, glyph + lay::ogl_outline, static_cast<FT_Render_Mode>(mode), dummy, internal, o);
    sync_library(l);
    if (!(destroy & 0xFF) && origin) {
        StackVar v(ctx, gbase(), 8);
        wr32(v.addr(), static_cast<u32>(neg32(rds32(origin))));
        wr32(v.addr() + 4, static_cast<u32>(neg32(rds32(origin + 4))));
        glyph_transform(glyph, 0, v.addr());
    }
    if (!error) {
        // ft_bitmap_glyph_init over the dummy slot, whose bitmap the host's renderer allocated: the glyph's in the
        // game's memory
        wr32(bitmap + lay::bgl_left, static_cast<u32>(dummy.bitmap_left));
        wr32(bitmap + lay::bgl_top, static_cast<u32>(dummy.bitmap_top));
        FT_Bitmap b = dummy.bitmap;
        u32 buffer = 0;
        if (b.buffer) {
            const u32 n = static_cast<u32>(b.pitch < 0 ? -b.pitch : b.pitch) * static_cast<u32>(b.rows);
            buffer = g_alloc(lib_memory(library), n, error);
            if (!error && n) std::memcpy(hp(buffer), b.buffer, n);
        }
        if (internal.flags & FT_GLYPH_OWN_BITMAP) l->ft->memory->free(l->ft->memory, dummy.bitmap.buffer);
        if (!error) {
            b.buffer = buffer ? hp(buffer) : nullptr;
            write_bitmap(bitmap + lay::bgl_bitmap, b);
            wr32(bitmap + lay::gl_advance, rd32(glyph + lay::gl_advance));
            wr32(bitmap + lay::gl_advance + 4, rd32(glyph + lay::gl_advance + 4));
            if (destroy & 0xFF) done_glyph(glyph);
            wr32(the_glyph, bitmap);
            return FT_Err_Ok;
        }
    } else if (internal.flags & FT_GLYPH_OWN_BITMAP) {
        l->ft->memory->free(l->ft->memory, dummy.bitmap.buffer);
    }
    done_glyph(bitmap);
    return error;
}

// A cache's glyph (FTC_ImageCache_Lookup) as a guest FT_Glyph, made once per host glyph and gone with it: what
// FT_Get_Glyph would have made of the slot the cache loaded it into.
u32 glyph_mirror(Library* l, FT_Glyph glyph, FT_Error& error)
{
    error = FT_Err_Ok;
    if (!glyph) return 0;
    if (const Mirror* m = by_host(glyph)) return m->guest;
    const GlyphClasses& gc = glyph_classes();
    const bool bitmap = glyph->format == FT_GLYPH_FORMAT_BITMAP;
    if (!bitmap && glyph->format != FT_GLYPH_FORMAT_OUTLINE) return 0;
    auto m = std::make_unique<Mirror>();
    m->kind = Mirror::Kind::glyph;
    m->host = glyph;
    m->lib = l;
    m->memory = l->memory;
    u32 g = 0;
    error = new_glyph(l->guest, bitmap ? gc.bitmap : gc.outline, &g);
    if (error) return 0;
    m->guest = g;
    wr32(g + lay::gl_advance, static_cast<u32>(glyph->advance.x));
    wr32(g + lay::gl_advance + 4, static_cast<u32>(glyph->advance.y));
    if (bitmap) {
        const auto* bg = reinterpret_cast<FT_BitmapGlyph>(glyph);
        wr32(g + lay::bgl_left, static_cast<u32>(bg->left));
        wr32(g + lay::bgl_top, static_cast<u32>(bg->top));
        FT_Bitmap b = bg->bitmap;
        u32 buffer = 0;
        if (b.buffer) {
            const u32 n = static_cast<u32>(b.pitch < 0 ? -b.pitch : b.pitch) * static_cast<u32>(b.rows);
            buffer = g_qalloc(l->memory, n, error);
            if (error) {
                g_free(l->memory, g);
                return 0;
            }
            if (n) std::memcpy(hp(buffer), b.buffer, n);
            m->blocks.push_back(buffer);
        }
        b.buffer = buffer ? hp(buffer) : nullptr;
        b.palette = nullptr;
        write_bitmap(g + lay::bgl_bitmap, b);
    } else {
        const FT_Outline& o = reinterpret_cast<FT_OutlineGlyph>(glyph)->outline;
        const u32 to = g + lay::ogl_outline;
        error = outline_new_internal(l->memory, static_cast<u16>(o.n_points), o.n_contours, to);
        if (error) {
            g_free(l->memory, g);
            return 0;
        }
        write_outline(to, o);
        wr32(to + lay::ol_flags, static_cast<u32>(o.flags) | kOwner);
        for (const u32 off : {lay::ol_points, lay::ol_tags, lay::ol_contours})
            if (const u32 a = rd32(to + off)) m->blocks.push_back(a);
    }
    add_mirror(std::move(m));
    return g;
}

// ------------------------------------------------------------------------------------------------- the stroker
// src/base/ftstroke.c on the host; the guest holds a handle (a block of FT_StrokerRec's size, 0x78 on the PPU).

constexpr u32 kStrokerRec = 0x78;

FT_Stroker stroker_of(u32 guest)
{
    Mirror* m = by_guest(guest, Mirror::Kind::stroker);
    if (!m && guest) PS_LOG_WARN(kGChan, "{:#x} is not an FT_Stroker", guest);
    return m ? static_cast<FT_Stroker>(const_cast<void*>(m->host)) : nullptr;
}

s32 FT_Stroker_New(PPUContext& ctx, u32 library, u32 astroker)
{
    Scope sc(ctx);
    if (!library) return FT_Err_Invalid_Argument;
    Library* l = library_of(library);
    if (!l) return FT_Err_Invalid_Argument;
    LibLock lk = lock(l);
    FT_Stroker s = nullptr;
    FT_Error error = ::FT_Stroker_New(l->ft, &s);
    u32 g = 0;
    if (!error) {
        auto m = std::make_unique<Mirror>();
        m->kind = Mirror::Kind::stroker;
        m->host = s;
        m->lib = l;
        m->memory = l->memory;
        m->guest = g_alloc(l->memory, kStrokerRec, error);
        if (error) {
            ::FT_Stroker_Done(s);
        } else {
            g = m->guest;
            add_mirror(std::move(m));
        }
    }
    if (astroker) wr32(astroker, g);
    return error;
}

void FT_Stroker_Done(PPUContext& ctx, u32 stroker)
{
    Scope sc(ctx);
    if (FT_Stroker s = stroker_of(stroker)) ::FT_Stroker_Done(s); // its handle goes with it
}

void FT_Stroker_Set(PPUContext& ctx, u32 stroker, s32 radius, u32 cap, u32 join, s32 miter)
{
    Scope sc(ctx);
    if (FT_Stroker s = stroker_of(stroker))
        ::FT_Stroker_Set(s, radius, static_cast<FT_Stroker_LineCap>(cap), static_cast<FT_Stroker_LineJoin>(join), miter);
}

void FT_Stroker_Rewind(PPUContext& ctx, u32 stroker)
{
    Scope sc(ctx);
    if (FT_Stroker s = stroker_of(stroker)) ::FT_Stroker_Rewind(s);
}

s32 FT_Stroker_ParseOutline(PPUContext& ctx, u32 stroker, u32 outline, u32 opened)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!outline || !s) return FT_Err_Invalid_Argument;
    HostOutline o;
    read_outline(outline, o);
    return ::FT_Stroker_ParseOutline(s, &o.o, static_cast<FT_Bool>(opened & 0xFF));
}

s32 FT_Stroker_BeginSubPath(PPUContext& ctx, u32 stroker, u32 to, u32 open)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s || !to) return FT_Err_Invalid_Argument;
    FT_Vector v = read_vector(to);
    return ::FT_Stroker_BeginSubPath(s, &v, static_cast<FT_Bool>(open & 0xFF));
}

s32 FT_Stroker_EndSubPath(PPUContext& ctx, u32 stroker)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s) return FT_Err_Invalid_Argument;
    return ::FT_Stroker_EndSubPath(s);
}

s32 FT_Stroker_LineTo(PPUContext& ctx, u32 stroker, u32 to)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s || !to) return FT_Err_Invalid_Argument;
    FT_Vector v = read_vector(to);
    return ::FT_Stroker_LineTo(s, &v);
}

s32 FT_Stroker_ConicTo(PPUContext& ctx, u32 stroker, u32 control, u32 to)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s || !control || !to) return FT_Err_Invalid_Argument;
    FT_Vector c = read_vector(control), v = read_vector(to);
    return ::FT_Stroker_ConicTo(s, &c, &v);
}

s32 FT_Stroker_CubicTo(PPUContext& ctx, u32 stroker, u32 control1, u32 control2, u32 to)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s || !control1 || !control2 || !to) return FT_Err_Invalid_Argument;
    FT_Vector c1 = read_vector(control1), c2 = read_vector(control2), v = read_vector(to);
    return ::FT_Stroker_CubicTo(s, &c1, &c2, &v);
}

s32 FT_Stroker_GetBorderCounts(PPUContext& ctx, u32 stroker, u32 border, u32 anum_points, u32 anum_contours)
{
    Scope sc(ctx);
    FT_UInt np = 0, nc = 0;
    const FT_Error e = ::FT_Stroker_GetBorderCounts(stroker_of(stroker), static_cast<FT_StrokerBorder>(border), &np, &nc);
    if (anum_points) wr32(anum_points, np);
    if (anum_contours) wr32(anum_contours, nc);
    return e;
}

s32 FT_Stroker_GetCounts(PPUContext& ctx, u32 stroker, u32 anum_points, u32 anum_contours)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    FT_UInt np = 0, nc = 0;
    const FT_Error e = s ? ::FT_Stroker_GetCounts(s, &np, &nc) : FT_Err_Invalid_Argument;
    if (anum_points) wr32(anum_points, np);
    if (anum_contours) wr32(anum_contours, nc);
    return e;
}

// FT_Stroker_ExportBorder appends a border to the guest's outline (whose arrays the caller sized).
void export_border(FT_Stroker s, FT_StrokerBorder border, u32 outline)
{
    FT_UInt np = 0, nc = 0;
    if (::FT_Stroker_GetBorderCounts(s, border, &np, &nc) != FT_Err_Ok) return;
    HostOutline o;
    read_outline(outline, o, np, nc);
    const s32 n0 = rds16(outline + lay::ol_n_points), c0 = rds16(outline + lay::ol_n_contours);
    o.o.points = o.points.data();
    o.o.tags = o.tags.data();
    o.o.contours = o.contours.data();
    ::FT_Stroker_ExportBorder(s, border, &o.o);
    const u32 pts = rd32(outline + lay::ol_points), tags = rd32(outline + lay::ol_tags),
              cts = rd32(outline + lay::ol_contours);
    for (s32 i = std::max(n0, 0); i < o.o.n_points; i++) {
        wr32(pts + 8 * static_cast<u32>(i), o.o.points[i].x);
        wr32(pts + 8 * static_cast<u32>(i) + 4, o.o.points[i].y);
        wr8(tags + static_cast<u32>(i), static_cast<u8>(o.o.tags[i]));
    }
    for (s32 i = std::max(c0, 0); i < o.o.n_contours; i++) wr16(cts + 2 * static_cast<u32>(i), static_cast<u16>(o.o.contours[i]));
    wr16(outline + lay::ol_n_points, static_cast<u16>(o.o.n_points));
    wr16(outline + lay::ol_n_contours, static_cast<u16>(o.o.n_contours));
}

void FT_Stroker_ExportBorder(PPUContext& ctx, u32 stroker, u32 border, u32 outline)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s || !outline || border > 1) return;
    export_border(s, static_cast<FT_StrokerBorder>(border), outline);
}

void FT_Stroker_Export(PPUContext& ctx, u32 stroker, u32 outline)
{
    Scope sc(ctx);
    FT_Stroker s = stroker_of(stroker);
    if (!s || !outline) return;
    export_border(s, FT_STROKER_BORDER_LEFT, outline);
    export_border(s, FT_STROKER_BORDER_RIGHT, outline);
}

// FT_Glyph_Stroke and FT_Glyph_StrokeBorder (src/base/ftstroke.c) over the guest's glyphs.
s32 glyph_stroke(u32 pglyph, u32 stroker, bool border_only, bool inside, bool destroy)
{
    if (!pglyph) return FT_Err_Invalid_Argument;
    u32 glyph = rd32(pglyph);
    if (!glyph || rd32(glyph + lay::gl_clazz) != glyph_classes().outline) return FT_Err_Invalid_Argument;
    StackVar copy_at(*current_ctx(), gbase(), 8);
    FT_Error error = glyph_copy(glyph, copy_at.addr());
    if (error) return error;
    glyph = rd32(copy_at.addr());
    const u32 outline = glyph + lay::ogl_outline;
    const u32 library = glyph_library(glyph);
    FT_Stroker s = stroker_of(stroker);
    FT_StrokerBorder border = FT_STROKER_BORDER_LEFT;
    if (border_only) {
        border = outline_orientation(outline) == FT_ORIENTATION_TRUETYPE ? FT_STROKER_BORDER_LEFT
                                                                          : FT_STROKER_BORDER_RIGHT;
        if (inside) border = border == FT_STROKER_BORDER_LEFT ? FT_STROKER_BORDER_RIGHT : FT_STROKER_BORDER_LEFT;
    }
    HostOutline o;
    read_outline(outline, o);
    error = s ? ::FT_Stroker_ParseOutline(s, &o.o, 0) : FT_Err_Invalid_Argument;
    if (!error) {
        FT_UInt np = 0, nc = 0;
        if (border_only) ::FT_Stroker_GetBorderCounts(s, border, &np, &nc);
        else ::FT_Stroker_GetCounts(s, &np, &nc);
        outline_done_internal(lib_memory(library), outline);
        error = outline_new_internal(lib_memory(library), np, static_cast<s32>(nc), outline);
        if (!error) {
            wr16(outline + lay::ol_n_points, 0);
            wr16(outline + lay::ol_n_contours, 0);
            if (border_only) {
                export_border(s, border, outline);
            } else {
                export_border(s, FT_STROKER_BORDER_LEFT, outline);
                export_border(s, FT_STROKER_BORDER_RIGHT, outline);
            }
        }
    }
    if (error) {
        done_glyph(glyph);
        if (!destroy) wr32(pglyph, 0);
        return error;
    }
    if (destroy) done_glyph(rd32(pglyph));
    wr32(pglyph, glyph);
    return FT_Err_Ok;
}

s32 FT_Glyph_Stroke(PPUContext& ctx, u32 pglyph, u32 stroker, u32 destroy)
{
    Scope sc(ctx);
    return glyph_stroke(pglyph, stroker, false, false, (destroy & 0xFF) != 0);
}

s32 FT_Glyph_StrokeBorder(PPUContext& ctx, u32 pglyph, u32 stroker, u32 inside, u32 destroy)
{
    Scope sc(ctx);
    return glyph_stroke(pglyph, stroker, true, (inside & 0xFF) != 0, (destroy & 0xFF) != 0);
}

// ------------------------------------------------------------------------------------------------- compressed streams
// FT_Stream_OpenGzip and FT_Stream_OpenLZW (src/gzip/ftgzip.c, src/lzw/ftlzw.c) on the host over a bridge of the
// guest's source stream; the guest's FT_StreamRec is filled as FreeType fills it, its read and close pspocket's.

struct Compressed {
    FT_StreamRec host{};
    FT_Stream source = nullptr; // the bridge over the guest's source
    u32 guest = 0;
    FT_Stream_CloseFunc close = nullptr; // FreeType's
};


void compressed_close(FT_Stream stream)
{
    auto* c = reinterpret_cast<Compressed*>(stream);
    if (c->close) c->close(stream);
    free_bridge_stream(c->source);
    if (current_ctx()) wr32(c->guest + lay::st_descriptor, 0);
    {
        GlyphState& st = glyph_state();
        std::lock_guard lk(st.mutex);
        st.compressed.erase(c->guest);
    }
    delete c;
}

Compressed* compressed_of(u32 guest)
{
    GlyphState& st = glyph_state();
    std::lock_guard lk(st.mutex);
    auto it = st.compressed.find(guest);
    return it == st.compressed.end() ? nullptr : it->second;
}

u32 compressed_descriptor(const char* name);

s32 open_compressed(u32 stream, u32 source, bool gzip)
{
    if (!stream || !source) return FT_Err_Invalid_Stream_Handle;
    auto* c = new Compressed{};
    c->guest = stream;
    c->source = bridge_stream(source);
    c->source->memory = plain_memory();
    const FT_Error error = gzip ? ::FT_Stream_OpenGzip(&c->host, c->source) : ::FT_Stream_OpenLZW(&c->host, c->source);
    if (error) {
        free_bridge_stream(c->source);
        delete c;
        return error;
    }
    c->close = c->host.close;
    c->host.close = compressed_close;
    std::memset(hp(stream), 0, lay::st_rec);
    wr32(stream + lay::st_memory, rd32(source + lay::st_memory));
    wr32(stream + lay::st_size, static_cast<u32>(c->host.size));
    wr32(stream + lay::st_descriptor, stream); // a handle of pspocket's
    wr32(stream + lay::st_read, compressed_descriptor("pspocket_ft_compressed_io"));
    wr32(stream + lay::st_close, compressed_descriptor("pspocket_ft_compressed_close"));
    GlyphState& st = glyph_state();
    std::lock_guard lk(st.mutex);
    st.compressed[stream] = c;
    return FT_Err_Ok;
}

s32 FT_Stream_OpenGzip(PPUContext& ctx, u32 stream, u32 source)
{
    Scope sc(ctx);
    return open_compressed(stream, source, true);
}

s32 FT_Stream_OpenLZW(PPUContext& ctx, u32 stream, u32 source)
{
    Scope sc(ctx);
    return open_compressed(stream, source, false);
}

// The guest's read and close of a compressed stream (FreeType's ft_gzip_stream_io and ft_gzip_stream_close).
u32 pspocket_ft_compressed_io(PPUContext& ctx, u32 stream, u32 offset, u32 buffer, u32 count)
{
    Scope sc(ctx);
    Compressed* c = compressed_of(stream);
    if (!c) return 0;
    std::vector<unsigned char> b(count);
    const unsigned long n = c->host.read(&c->host, offset, b.data(), count);
    if (buffer && n) std::memcpy(hp(buffer), b.data(), n);
    return static_cast<u32>(n);
}

void pspocket_ft_compressed_close(PPUContext& ctx, u32 stream)
{
    Scope sc(ctx);
    if (Compressed* c = compressed_of(stream)) compressed_close(&c->host);
}

u32 compressed_descriptor(const char* name)
{
    HleProcess* p = HleProcess::current();
    return p ? p->descriptor_of("cell_FreeType2", recomp::nid_of(name)) : 0;
}

} // namespace g

namespace ftb {

u32 compressed_guest_stream(FT_Stream s)
{
    if (!s || s->close != g::compressed_close) return 0;
    return reinterpret_cast<g::Compressed*>(s)->guest;
}

// The host stream for a guest FT_StreamRec given to FT_Open_Face: a compressed stream of pspocket's is used as it is.
FT_Stream host_stream_for(u32 guest)
{
    if (g::Compressed* c = g::compressed_of(guest)) return &c->host;
    return nullptr;
}

} // namespace ftb

#define FT_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&g::fn>, false}
#define FT_INTERNAL(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&g::fn>, true}

void register_ft_glyph(Registry& r)
{
    r.add("cell_FreeType2", {
        FT_FUNC(0xA60023EF, FT_Outline_New),
        FT_FUNC(0x3F2B82DF, FT_Outline_Done),
        FT_FUNC(0x3C863B04, FT_Outline_Copy),
        FT_FUNC(0x6EF5D792, FT_Outline_Translate),
        FT_FUNC(0xEEB810EC, FT_Outline_Transform),
        FT_FUNC(0xEB3B1212, FT_Outline_Reverse),
        FT_FUNC(0x9C8BE729, FT_Outline_Check),
        FT_FUNC(0x1B6F28A5, FT_Outline_Get_CBox),
        FT_FUNC(0x889C4159, FT_Outline_Get_BBox),
        FT_FUNC(0x4B4CBF7A, FT_Outline_Embolden),
        FT_FUNC(0x04DD4855, FT_Outline_Get_Orientation),
        FT_FUNC(0x52A1F1EC, FT_Outline_GetInsideBorder),
        FT_FUNC(0x04AF4D22, FT_Outline_GetOutsideBorder),
        FT_FUNC(0x1FCAEEBB, FT_Outline_Decompose),
        FT_FUNC(0xF67C1C4B, FT_Outline_Render),
        FT_FUNC(0x18C23BDF, FT_Outline_Get_Bitmap),
        FT_FUNC(0x13268E21, FT_Bitmap_New),
        FT_FUNC(0x7EC07F08, FT_Bitmap_Copy),
        FT_FUNC(0x1AEB33A5, FT_Bitmap_Embolden),
        FT_FUNC(0xFD5B7C04, FT_Bitmap_Convert),
        FT_FUNC(0xC2261290, FT_Bitmap_Done),
        FT_FUNC(0xDB615095, FT_Done_Glyph),
        FT_FUNC(0x420DE176, FT_Glyph_Copy),
        FT_FUNC(0xD912B0F4, FT_Glyph_Transform),
        FT_FUNC(0x44951709, FT_Glyph_Get_CBox),
        FT_FUNC(0x4B573039, FT_Get_Glyph),
        FT_FUNC(0xB499D895, FT_Glyph_To_Bitmap),
        FT_FUNC(0xD9C9F093, FT_Stroker_New),
        FT_FUNC(0x0C8CAE53, FT_Stroker_Done),
        FT_FUNC(0xE7EBF1EF, FT_Stroker_Set),
        FT_FUNC(0xCFD82265, FT_Stroker_Rewind),
        FT_FUNC(0x684766E9, FT_Stroker_ParseOutline),
        FT_FUNC(0xA4AFEB98, FT_Stroker_BeginSubPath),
        FT_FUNC(0x6DE0EFB4, FT_Stroker_EndSubPath),
        FT_FUNC(0xD78B41CB, FT_Stroker_LineTo),
        FT_FUNC(0xF885420C, FT_Stroker_ConicTo),
        FT_FUNC(0x4BBC14E6, FT_Stroker_CubicTo),
        FT_FUNC(0x773FF791, FT_Stroker_GetBorderCounts),
        FT_FUNC(0xC20A2975, FT_Stroker_GetCounts),
        FT_FUNC(0xC3A7E8F2, FT_Stroker_ExportBorder),
        FT_FUNC(0x18484DC6, FT_Stroker_Export),
        FT_FUNC(0x66D88C8C, FT_Glyph_Stroke),
        FT_FUNC(0xF7261881, FT_Glyph_StrokeBorder),
        FT_FUNC(0x459907C4, FT_Stream_OpenGzip),
        FT_FUNC(0x084EB81A, FT_Stream_OpenLZW),
        FT_INTERNAL(0x1E8D9600, pspocket_ft_compressed_io),
        FT_INTERNAL(0x95CE74B3, pspocket_ft_compressed_close),
    });
}

} // namespace pspocket::hle

#endif
