// EDGE geometry on the GPU, the SPU side (D-988; pspocket/spu/edge_gpu.hpp): the stages of GTA V's EDGE build
// (spu_dad457863220b1cc) recorded into a GPU work item instead of computed.
//
// What each stage reads, as its function finds it at its entry (the build's EDGE context, EdgeGeomSpuContext as this
// build lays it out, at $3; D-987):
//   +0x00  EdgeGeomSpuConfigInfo: flags and uniform tables, hole size, input formats (primary, secondary), output
//          format, delta format, index flavour (high nibble) and skinning flavour (low), matrix format, vertices
//          (u16 at +8), indices (u16 at +10)
//   +0x10  the uniform tables (a word each); +0x50 the index buffer, +0x54/+0x58 the scratch's start and end,
//          +0x58 + 4 * attribute id the table of each attribute; +0x6C/+0x70 the input stream descriptions, +0x74 the
//          output one; +0x7C/+0x80 custom decompression callbacks
//   +0xA0  the local-to-world matrix (3 rows), +0xD0 the scissor rectangle (u16 x, y, width, height) and the depth
//          range, +0xE0 the view-projection matrix (4 rows), +0x120 the viewport's scales, +0x130 its offsets (byte 13:
//          add 0.5 to x and y; +0x13D: the "no pixel" test), +0x150 the attribute id of each table (a byte each),
//          +0x174 the vertex-output callback, +0x178 its argument, +0x17E the index count
// A stream description: attributes, stride, blocks, a pad byte, 4 bytes, then 8-byte blocks: an attribute block
// (offset, format, components, attribute id, size, slot, the offset of its fixed-point block from it, pad) and, after a
// fixed-point attribute, its block of (integer bits, fraction bits) by component. Formats seen: 8 fixed point, 9 unit
// vector (input); 2 F32, 6 X11Y11Z10N (output).

#include "pspocket/spu/edge_gpu.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

#include "pspocket/core/be.h"
#include "pspocket/core/edge_registry.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/lifted.h"

namespace pspocket::spu::edge_gpu {
namespace {

inline uint32_t pref(const SPUContext& ctx, int r) { return ctx.gpr[r].u32[3]; }
inline uint32_t ld32(const uint8_t* ls, uint32_t a) { return ps_ld32(ls + (a & PS_SPU_LS_MASK)); }
inline uint32_t ld16(const uint8_t* ls, uint32_t a) { return ps_ld16(ls + (a & PS_SPU_LS_MASK)); }
inline uint32_t ld8(const uint8_t* ls, uint32_t a) { return ls[a & PS_SPU_LS_MASK]; }
inline void st32(uint8_t* ls, uint32_t a, uint32_t v) { ps_st32(ls + (a & PS_SPU_LS_MASK), v); }
inline uint32_t w_of(ps_spu_vec v, int i) { return v[3 - i]; }
inline float f32(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// A condition the GPU path goes on through (the job's stages already skipped): a warning, said once a kind. Run 331
// logged 134,527 lines of one kind, each looked up in a growing list under one lock, and the EDGE jobs waited on it:
// now a bit a kind, tested with one relaxed load (D-1005).
enum class Warn : uint8_t { cull_offset, cull_count, output_changed, vertices_put, allocation, no_hole, missing_table };
std::atomic<uint64_t> g_warned{0};

template <class F> void warn_once(Warn w, F&& text)
{
    const uint64_t bit = uint64_t{1} << static_cast<int>(w);
    if ((g_warned.load(std::memory_order_relaxed) & bit) || (g_warned.fetch_or(bit) & bit)) return;
    PS_LOG_WARN("edge", "EDGE on the GPU: {} (said once)", text());
}

// A route or a job left to the SPU, said once a reason (with the first job's detail) as information.
std::atomic<uint64_t> g_said{0};
void say_once(edge::Route route, edge::Reason why, uint32_t detail)
{
    const uint64_t bit = uint64_t{1} << (static_cast<int>(why) % 64);
    if (why == edge::Reason::none || (g_said.load(std::memory_order_relaxed) & bit) || (g_said.fetch_or(bit) & bit)) return;
    PS_LOG_INFO("edge", "EDGE on the GPU: jobs {} for the {} ({:#x} in the first) (said once)", edge::route_name(route),
        edge::reason_name(why), detail);
}

} // namespace

namespace {

bool fail(Recording& r, edge::Reason why, uint32_t detail = 0)
{
    if (r.reject == edge::Reason::none) {
        r.reject = why;
        r.detail = detail;
    }
    return false;
}

// The return of a stage's function: its link slot (the callee stores $lr at the caller's sp + 16), the pc.
void leave(uint8_t* ls, SPUContext& ctx)
{
    const uint32_t sp = pref(ctx, 1);
    psl_stq(ls, sp + 0x10, psl_ld(&ctx, 0));
    ctx.pc = pref(ctx, 0) & PS_SPU_PC_MASK;
}

struct Attr {
    uint32_t offset, format, components, id, size, fixed_at;
};

// The attribute blocks of a stream description at `d`.
bool parse_description(Recording& r, const uint8_t* ls, uint32_t d, uint32_t& stride, std::vector<Attr>& out)
{
    const uint32_t attrs = ld8(ls, d), blocks = ld8(ls, d + 2);
    stride = ld8(ls, d + 1);
    if (attrs == 0 || attrs > 8 || blocks > 16 || blocks < attrs) return fail(r, edge::Reason::stream_description, attrs);
    uint32_t b = d + 8;
    for (uint32_t k = 0; k < attrs; k++) {
        Attr a{ld8(ls, b), ld8(ls, b + 1), ld8(ls, b + 2), ld8(ls, b + 3), ld8(ls, b + 4), 0};
        if (a.format == PS_EDGE_FMT_FIXED) {
            a.fixed_at = b + ld8(ls, b + 6);
            b += 16; // the attribute block and its fixed-point block
        } else {
            b += 8;
        }
        out.push_back(a);
    }
    return true;
}

// The table of attribute `id` as the job's EDGE context lists it (0x07470 and 0x074C8): the tables are words at +0x10,
// their attribute ids a byte each at +0x150, as many as the low nibble of byte 0 and one more with its top bit (the
// vertex-output callback's table); 0: none.
uint32_t table_of_id(const uint8_t* ls, uint32_t c, uint32_t id)
{
    const uint32_t b0 = ld8(ls, c), n = std::min<uint32_t>((b0 & 15u) + (b0 >> 7), 16u);
    for (uint32_t k = 0; k < n; k++)
        if (ld8(ls, c + 0x150 + k) == id) return ld32(ls, c + 0x10 + 4 * k);
    return 0;
}

// The table the compression (0x08E20, at 0x09008) puts out for attribute `id`: the first of the 16 id bytes at +0x150
// that matches, whatever the count (stale bytes past it too), its table word at +0x10; none matching: index 16, the word
// at +0x50 (the index buffer's address: the SPU puts out what is there, and so does the GPU path).
uint32_t compress_table_of_id(const uint8_t* ls, uint32_t c, uint32_t id)
{
    uint32_t k = 0;
    while (k < 16 && ld8(ls, c + 0x150 + k) != id) k++;
    return ld32(ls, c + 0x10 + 4 * k);
}

// The output vertex as the configuration (byte 4 of the context: predefined 0 or 2, or 0xFF: the description at +0x74)
// lays it out, into j.out / out_count / out_stride; false (with r.reject) when the GPU path does not make it. What
// the GPU computes: from the decompression, the positions, normals and tangents it decodes; from the skinning, the same
// from the SPU's tables, skinned; from the culling, nothing: every output attribute is a table the SPU made (its
// attribute id in `table` until the compression records it, D-1006).
bool output_layout(Recording& r, const uint8_t* ls, uint32_t c, ps_edge_job& j, edge::Route route)
{
    const uint32_t format = ld8(ls, c + 4);
    const bool has_nrm = j.nrm_offset != ~0u, has_tan = j.tan_offset != ~0u;
    j.out_count = 0;
    const auto add = [&](uint32_t id, uint32_t fmt, uint32_t comps, uint32_t offset) {
        const bool computed = route != edge::Route::cull &&
                              (id == PS_EDGE_ATTR_POSITION || (id == PS_EDGE_ATTR_NORMAL && has_nrm) || (id == PS_EDGE_ATTR_TANGENT && has_tan));
        if ((!computed && route == edge::Route::decompress) || j.out_count == PS_EDGE_MAX_OUT)
            return fail(r, edge::Reason::output_attribute, id);
        j.out[j.out_count++] = ps_edge_out_attr{computed ? id : uint32_t{PS_EDGE_ATTR_TABLE}, fmt, comps, offset, id};
        return true;
    };
    if (format == 0) {
        j.out_stride = 12;
        return add(PS_EDGE_ATTR_POSITION, PS_EDGE_FMT_F32, 3, 0);
    }
    if (format == 2) {
        j.out_stride = 24;
        return add(PS_EDGE_ATTR_POSITION, PS_EDGE_FMT_F32, 3, 0) && add(PS_EDGE_ATTR_NORMAL, PS_EDGE_FMT_X11Y11Z10N, 3, 12) &&
               add(PS_EDGE_ATTR_TANGENT, PS_EDGE_FMT_I16N, 4, 16);
    }
    if (format != 0xFF) return fail(r, edge::Reason::output_format, format);
    uint32_t stride = 0;
    std::vector<Attr> a;
    if (!parse_description(r, ls, ld32(ls, c + 0x74), stride, a)) return false;
    for (const Attr& x : a) {
        uint32_t fmt = 0, comps = x.components;
        if (x.format == PS_EDGE_FMT_F32 && x.size == 4 * x.components && x.components >= 1 && x.components <= 4)
            fmt = PS_EDGE_FMT_F32;
        else if (x.format == PS_EDGE_FMT_X11Y11Z10N && x.size == 4)
            fmt = PS_EDGE_FMT_X11Y11Z10N, comps = 3;
        else if (x.format == PS_EDGE_FMT_U8N && x.size == x.components && x.components >= 1 && x.components <= 4)
            fmt = PS_EDGE_FMT_U8N;
        else if (x.format == PS_EDGE_FMT_I16N && x.size == 2 * x.components && x.components >= 1 && x.components <= 4)
            fmt = PS_EDGE_FMT_I16N;
        else
            return fail(r, edge::Reason::output_format, 0x100 | x.format << 8 | x.size);
        if (x.offset + x.size > stride) return fail(r, edge::Reason::output_stride, stride);
        if (!add(x.id, fmt, comps, x.offset)) return false;
    }
    j.out_stride = stride;
    return stride != 0 || fail(r, edge::Reason::output_stride, 0);
}

// GTA V's job driver's own steps around EDGE's (D-1006, from its code: its descriptor from + 0xF0 is in $85 from
// before the decompression on, the fields named by their descriptor offsets): those that read or change the stages'
// tables, by where they run.
struct DriverSteps {
    edge::Reason before_skin = edge::Reason::none; // after the decompression, before the skinning
    edge::Reason after_skin = edge::Reason::none;  // after the skinning, before the culling
    uint32_t detail = 0;
};

DriverSteps driver_steps(const uint8_t* ls, const SPUContext& ctx, uint32_t skin)
{
    DriverSteps d;
    const uint32_t p = pref(ctx, 85);
    const uint32_t b1a4 = ld8(ls, p + 0xB4), b1a6 = ld8(ls, p + 0xB6), b1a7 = ld8(ls, p + 0xB7);
    // 0x119D4: 0x06808 filters the triangles by words of the positions' table (and 0x198, 0x1D0 on) before the culling
    if (b1a6) {
        d.after_skin = edge::Reason::triangle_filter;
        d.detail = b1a6;
    } else if (skin && ld32(ls, p + 0xCC)) {
        // 0x11658 to 0x119C4 (word 0x1BC, the skinning on, more of the descriptor and the job): positions and normals
        // changed from extra data after the skinning
        d.after_skin = edge::Reason::normal_pass;
        d.detail = ld32(ls, p + 0xCC);
    }
    // 0x11338: $83 added to every position after the decompression
    for (int k = 0; k < 4; k++)
        if (ctx.gpr[83].u32[k] & 0x7FFFFFFFu) d.before_skin = edge::Reason::position_offset;
    if (d.before_skin != edge::Reason::none) return d;
    if (b1a4 & 4) {
        // 0x11398: extra data fetched (0x074C8) and applied by 0x04C30, or 0x0DBA8
        d.before_skin = edge::Reason::blend_path;
        d.detail = b1a4;
    } else if (b1a4 & 0x18) {
        // 0x115DC: 0x0E398's mode (bits 3 and 4; it runs with mode 0 in the walk's jobs too, its work read at the
        // skinning's entry)
        d.before_skin = edge::Reason::matrix_path;
        d.detail = b1a4;
    } else if (b1a7 == 2) {
        // 0x11488: the output format made 4 (word 0x194) or the output description changed; more after the hole
        d.before_skin = edge::Reason::output_mode;
        d.detail = b1a7;
    } else if (b1a7 == 0 && ld32(ls, p + 0xA4)) {
        // 0x08A38, after the culling: a palette from extra data looked up per vertex into a table the compression puts
        // out (word 0x194, 0x1A7 0); the tables it reads are the decompression's
        d.before_skin = edge::Reason::colour_pass;
        d.detail = ld32(ls, p + 0xA4);
    }
    // never read by the driver's code: words 0x190, 0x19C; word 0x198 goes to the context's +0x17C (read by nothing)
    // and to the filter; 0x1A5; 0x1B0 to 0x1BB and 0x1C0 on but in the paths above
    return d;
}

} // namespace

void Recording::clear()
{
    blob.assign(sizeof(ps_edge_job), 0);
    job = ps_edge_job{};
    route = edge::Route::spu;
    why = edge::Reason::none;
    reject = edge::Reason::none;
    detail = 0;
    stages = 0;
    ctx = 0;
    told = 0;
}

uint32_t Recording::append(const uint8_t* ls, uint32_t at, uint32_t n)
{
    const uint32_t offset = static_cast<uint32_t>(blob.size());
    n = std::min<uint32_t>(n, PS_SPU_LS_SIZE);
    at &= PS_SPU_LS_MASK;
    const uint32_t first = std::min(n, PS_SPU_LS_SIZE - at); // local store wraps
    blob.insert(blob.end(), ls + at, ls + at + first);
    if (first < n) blob.insert(blob.end(), ls, ls + (n - first));
    blob.resize(offset + ((n + 15) & ~15u) + 16, 0); // 16-aligned, and room for the kernels' 4-byte reads past the end
    return offset;
}

void Recording::finish()
{
    job.magic = PS_EDGE_JOB_MAGIC;
    job.size = static_cast<uint32_t>(blob.size());
    std::memcpy(blob.data(), &job, sizeof job);
}


bool reroutable(edge::Reason why)
{
    switch (why) {
    case edge::Reason::input_format:
    case edge::Reason::blend_shapes:
    case edge::Reason::custom_decompress:
    case edge::Reason::primary_stream:
    case edge::Reason::fixed_point_wide:
    case edge::Reason::secondary_attribute:
    case edge::Reason::stream_description:
    case edge::Reason::output_attribute: return true;
    default: return false;
    }
}

void choose_route(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3);
    r.ctx = c;
    r.route = edge::Route::spu;
    const auto spu = [&](edge::Reason why, uint32_t detail) {
        r.reject = why;
        r.detail = detail;
    };
    const uint32_t nv = ld16(ls, c + 8), ni = ld16(ls, c + 10);
    // what no route makes: another vertex-output callback (the GPU makes the screen transform), counts out of range
    if (((ld32(ls, c + 0x174) - ctx.reloc) & PS_SPU_PC_MASK) != 0x0ECF8) return spu(edge::Reason::callback, ld32(ls, c + 0x174));
    if (ld8(ls, ld32(ls, c + 0x178) + 183) == 3) return spu(edge::Reason::projecting, 3);
    if (nv == 0 || nv > 0x4000 || ni > 0x10000) return spu(edge::Reason::counts, nv);
    if (!(ld8(ls, c) & 0x80)) return spu(edge::Reason::context_flag, ld8(ls, c));
    const uint32_t skin = ld8(ls, c + 6) & 15u, matrix_format = ld8(ls, c + 7);
    const DriverSteps d = driver_steps(ls, ctx, skin);
    r.detail = d.detail;
    if (d.after_skin != edge::Reason::none) {
        r.route = edge::Route::cull;
        r.why = d.after_skin;
        return;
    }
    if (skin != 0 && skin != 1 && skin != 4) {
        r.route = edge::Route::cull;
        r.why = edge::Reason::skin_flavour;
        r.detail = skin;
        return;
    }
    if (matrix_format != 0) {
        r.route = edge::Route::cull;
        r.why = edge::Reason::matrix_format;
        r.detail = matrix_format;
        return;
    }
    r.route = d.before_skin != edge::Reason::none ? edge::Route::skin : edge::Route::decompress;
    r.why = d.before_skin;
    if (static_cast<int>(r.route) < static_cast<int>(edge::earliest_route())) r.route = edge::earliest_route();
}

// ---- the vertex decompression (0x0DAF0: $3 context, $4 and $5 the primary stream and its fixed offsets, $6 and $7
// the secondary's) ----

bool on_decompress(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3), v1 = pref(ctx, 4), fo1 = pref(ctx, 5), v2 = pref(ctx, 6), fo2 = pref(ctx, 7);
    r.ctx = c;
    const uint32_t in1 = ld8(ls, c + 2), in2 = ld8(ls, c + 3), out = ld8(ls, c + 4), delta = ld8(ls, c + 5);
    const uint32_t nv = ld16(ls, c + 8), ni = ld16(ls, c + 10);
    if (out != 0 && out != 2 && out != 0xFF) return fail(r, edge::Reason::output_format, out);
    if (in1 != 0xFF || (v2 && in2 != 0xFF)) return fail(r, edge::Reason::input_format, in1 << 8 | in2);
    if (delta != 0xFF) return fail(r, edge::Reason::blend_shapes, delta);
    if (ld32(ls, c + 0x7C) || ld32(ls, c + 0x80)) return fail(r, edge::Reason::custom_decompress);
    ps_edge_job& j = r.job;
    j.num_vertexes = nv;
    j.num_indexes = ni;

    // The primary stream: one attribute, the positions in fixed point.
    uint32_t stride1 = 0;
    std::vector<Attr> a1;
    if (!parse_description(r, ls, ld32(ls, c + 0x6C), stride1, a1)) return false;
    if (a1.size() != 1 || a1[0].id != PS_EDGE_ATTR_POSITION || a1[0].format != PS_EDGE_FMT_FIXED || a1[0].components != 3)
        return fail(r, edge::Reason::primary_stream, static_cast<uint32_t>(a1.size()) << 24 | a1[0].id << 16 | a1[0].format << 8 | a1[0].components);
    uint32_t bits = 0;
    for (uint32_t k = 0; k < 3; k++) {
        const uint32_t ib = ld8(ls, a1[0].fixed_at + 2 * k), fb = ld8(ls, a1[0].fixed_at + 2 * k + 1);
        // over 24 bits: the wide path's, with 8 to 24 fraction bits (its conversions exact, D-1007)
        if (ib + fb == 0 || ib + fb > 32 || fb > 24 || (ib + fb > 24 && fb < 8)) return fail(r, edge::Reason::fixed_point_wide, ib << 8 | fb);
        if (ib + fb > 24) r.why = edge::Reason::fixed_point_wide;
        j.pos_bits[k] = ib + fb;
        j.pos_frac[k] = fb;
        j.pos_bias[k] = static_cast<int32_t>(ld32(ls, fo1 + 4 * k));
        bits += ib + fb;
    }
    if (stride1 == 0 || a1[0].offset * 8 + bits > stride1 * 8) return fail(r, edge::Reason::primary_stream, stride1);
    j.pos_stride = stride1;
    j.pos_bit = a1[0].offset * 8;
    j.pos_data = r.append(ls, v1, nv * stride1);

    // The secondary stream: normals and tangents as unit vectors.
    j.nrm_offset = j.tan_offset = ~0u;
    std::vector<Attr> a2;
    if (v2) {
        uint32_t stride2 = 0;
        if (!parse_description(r, ls, ld32(ls, c + 0x70), stride2, a2)) return false;
        for (const Attr& a : a2) {
            if (a.format != PS_EDGE_FMT_UNIT_VECTOR || a.size != 3 || a.offset + 3 > stride2)
                return fail(r, edge::Reason::secondary_attribute, a.id << 16 | a.format << 8 | a.size);
            if (a.id == PS_EDGE_ATTR_NORMAL && a.components == 3) j.nrm_offset = a.offset;
            else if (a.id == PS_EDGE_ATTR_TANGENT && a.components == 4) j.tan_offset = a.offset;
            else return fail(r, edge::Reason::secondary_attribute, a.id << 16 | a.components);
        }
        j.uv_stride = stride2;
        j.uv_data = r.append(ls, v2, nv * stride2);
        (void)fo2;
    }
    // The output vertex now (the later stages cannot turn the job back to the SPU: this one skipped its work).
    ps_edge_job probe = j;
    if (!output_layout(r, ls, c, probe, edge::Route::decompress)) return false;
    r.stages |= 1;
    return true;
}

// ---- the later routes' tables (D-1006) ----

namespace {

// The positions, normals and tangents the SPU's stages made, as they stand at this stage's entry.
bool record_tables(Recording& r, const uint8_t* ls, uint32_t c, bool vectors)
{
    ps_edge_job& j = r.job;
    const uint32_t nv = ld16(ls, c + 8), tp = table_of_id(ls, c, PS_EDGE_ATTR_POSITION);
    if (!tp) return fail(r, edge::Reason::tables, PS_EDGE_ATTR_POSITION);
    j.num_vertexes = nv;
    j.num_indexes = ld16(ls, c + 10);
    j.tables = 1;
    j.pos_data = r.append(ls, tp, 16 * nv);
    j.nrm_offset = j.tan_offset = ~0u;
    if (!vectors) return true;
    if (const uint32_t t = table_of_id(ls, c, PS_EDGE_ATTR_NORMAL)) {
        j.nrm_offset = 0;
        j.nrm_table = r.append(ls, t, 16 * nv);
    }
    if (const uint32_t t = table_of_id(ls, c, PS_EDGE_ATTR_TANGENT)) {
        j.tan_offset = 0;
        j.tan_table = r.append(ls, t, 16 * nv);
    }
    return true;
}

} // namespace

namespace {

// Each output attribute the GPU does not compute has a table to put out by the compression (D-1006): without one the
// SPU's compression puts out what the index buffer's address holds then (the culling's compacted list): left to the
// SPU. The colour pass makes its table (attribute 19) after the culling.
bool output_tables(Recording& r, const uint8_t* ls, uint32_t c, const ps_edge_job& probe)
{
    for (uint32_t a = 0; a < probe.out_count; a++) {
        const ps_edge_out_attr& o = probe.out[a];
        if (o.source != PS_EDGE_ATTR_TABLE || r.why == edge::Reason::colour_pass) continue;
        uint32_t k = 0;
        while (k < 16 && ld8(ls, c + 0x150 + k) != o.table) k++;
        if (k == 16) return fail(r, edge::Reason::tables, o.table);
    }
    return true;
}

} // namespace

bool on_skin_tables(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3);
    r.ctx = c;
    // a binormal table: the SPU's skinning would turn it too (the GPU skins positions, normals and tangents)
    if (table_of_id(ls, c, 4)) return fail(r, edge::Reason::tables, 4);
    if (!record_tables(r, ls, c, true)) return false;
    ps_edge_job probe = r.job;
    if (!output_layout(r, ls, c, probe, edge::Route::skin) || !output_tables(r, ls, c, probe)) return false;
    r.stages |= 1;
    return true;
}

bool on_cull_tables(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3);
    r.ctx = c;
    if (!record_tables(r, ls, c, false)) return false;
    r.job.skin = 0; // skinned by the SPU
    ps_edge_job probe = r.job;
    if (!output_layout(r, ls, c, probe, edge::Route::cull) || !output_tables(r, ls, c, probe)) return false;
    r.stages |= 3;
    return true;
}

void leave_decompress(const Recording& r, uint8_t* ls, SPUContext& ctx)
{
    const uint32_t c = r.ctx, v1 = pref(ctx, 4), v2 = pref(ctx, 6);
    st32(ls, c + 0x54, v1);
    st32(ls, c + 0x58, v2 && v2 < v1 ? v2 : v1); // the scratch from the lower stream on: both are consumed
    // Each attribute's table: the k-th attribute (primary first) in uniform table k.
    uint32_t ids = 0, k = 0;
    const auto table_of = [&](uint32_t id) {
        st32(ls, c + 0x58 + 4 * id, ld32(ls, c + 0x10 + 4 * k));
        ids |= id << (24 - 8 * k);
        k++;
    };
    table_of(PS_EDGE_ATTR_POSITION);
    if (pref(ctx, 6)) {
        // in the order of the secondary description
        const uint32_t d = ld32(ls, c + 0x70);
        const uint32_t attrs = ld8(ls, d);
        for (uint32_t i = 0; i < attrs; i++) table_of(ld8(ls, d + 8 + 8 * i + 3));
    }
    st32(ls, c + 0x150, ids);
    leave(ls, ctx);
}

// ---- skinning (0x07A10: $3 context, $4 the matrices, $5 how many, $6 the weights and bones) ----

bool on_skin(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3), mats = pref(ctx, 4), count = std::min<uint32_t>(pref(ctx, 5), 256), weights = pref(ctx, 6);
    const uint32_t skin = ld8(ls, c + 6) & 15u;
    ps_edge_job& j = r.job;
    j.skin = skin;
    j.matrix_count = count;
    j.matrices = r.append(ls, mats, 48 * count);
    j.weights = r.append(ls, weights, j.num_vertexes * (skin == 4 ? 1u : 8u));
    r.stages |= 2;
    return true;
}

void leave_skin(const Recording&, uint8_t* ls, SPUContext& ctx)
{
    st32(ls, pref(ctx, 3) + 0x58, pref(ctx, 4));
    leave(ls, ctx);
}

// ---- culling (0x0EFF8: $3 context, $4 the mode, $5 an offset in vertices into the callback's output) ----

bool on_cull(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t s = pref(ctx, 3), mode = pref(ctx, 4), voff = pref(ctx, 5);
    ps_edge_job& j = r.job;
    // Not seen in the walk (one batch from vertex 0, the configured count): said once if a job does.
    if (voff != 0) warn_once(Warn::cull_offset, [&] { return std::format("culling from vertex {} (taken as 0)", voff); });
    const uint32_t ni = ld16(ls, s + 0x17E);
    if (ni != j.num_indexes && r.route != edge::Route::cull) warn_once(Warn::cull_count, [&] { return std::format("{} indices to cull, {} configured", ni, j.num_indexes); });
    j.num_indexes = ni - ni % 3;
    j.indices = r.append(ls, ld32(ls, s + 0x50), 2 * ni);

    // The screen transform's constants, as 0x0ECF8 makes them (in the SPU's arithmetic: they are the same bits).
    const ps_spu_vec m0r0 = psl_lq(ls, s + 0xA0), m0r1 = psl_lq(ls, s + 0xB0), m0r2 = psl_lq(ls, s + 0xC0);
    const ps_spu_vec unit_w = psl_const(0, 0, 0, 0x3F800000u);
    for (int i = 0; i < 4; i++) {
        const ps_spu_vec m1 = psl_lq(ls, s + 0xE0 + 16u * static_cast<uint32_t>(i));
        const ps_spu_vec row = psl_fma(psl_splat32(w_of(m1, 3)), unit_w,
            psl_fma(psl_splat32(w_of(m1, 2)), m0r2, psl_fma(psl_splat32(w_of(m1, 1)), m0r1, psl_fm(psl_splat32(w_of(m1, 0)), m0r0))));
        for (int k = 0; k < 4; k++) j.combined[4 * i + k] = f32(w_of(row, k));
    }
    const ps_spu_vec vs = psl_lq(ls, s + 0x120), vo = psl_lq(ls, s + 0x130);
    const bool half = (psl_rotqby(vo, 10)[3] & 0xFFu) != 0;
    const ps_spu_vec half_v = psl_splat32(half ? 0x3F000000u : 0u);
    for (int k = 0; k < 3; k++) j.scale[k] = f32(w_of(vs, k));
    j.offset[0] = f32(w_of(psl_fa(psl_splat32(w_of(vo, 0)), half_v), 0));
    j.offset[1] = f32(w_of(psl_fa(psl_splat32(w_of(vo, 1)), half_v), 0));
    j.offset[2] = f32(w_of(vo, 2));
    const ps_spu_vec rr = psl_lq(ls, s + 0xD0);
    const uint32_t rx = w_of(rr, 0) >> 16, ry = w_of(rr, 0) & 0xFFFFu, rw = w_of(rr, 1) >> 16, rh = w_of(rr, 1) & 0xFFFFu;
    const ps_spu_vec rect = psl_cuflt(psl_const(rx, ry, (rx + rw) & 0xFFFFu, (ry + rh) & 0xFFFFu), 155, 0);
    const ps_spu_vec z0 = psl_splat32(w_of(rr, 2)), z1 = psl_splat32(w_of(rr, 3));
    const bool z0_gt_z1 = w_of(psl_fcgt(z0, z1), 0) != 0;
    j.bound_min[0] = f32(w_of(rect, 0));
    j.bound_min[1] = f32(w_of(rect, 1));
    j.bound_max[0] = f32(w_of(rect, 2));
    j.bound_max[1] = f32(w_of(rect, 3));
    j.bound_min[2] = f32(z0_gt_z1 ? w_of(rr, 3) : w_of(rr, 2));
    j.bound_max[2] = f32(z0_gt_z1 ? w_of(rr, 2) : w_of(rr, 3));

    // The rules (0x0F008): mode 1 culls one facing, 2 and 3 the other, anything else nothing.
    if (mode >= 1 && mode <= 3) {
        const uint32_t b6 = ld8(ls, s + 6) >> 4;
        j.cull_mode = mode;
        j.flip = ((b6 == 2 || b6 == 0) ? 0x80000000u : 0u) ^ (mode == 2 ? 0u : 0x80000000u);
        // The mode's arithmetic on the tests' masks (0 or all ones: as floats, NaNs), in the SPU's environment as the
        // job's code does it at run time: the compiler must not fold it in its own rules (a folded NaN keeps its
        // payload; the run-time one is the default NaN, D-988).
        ps_spu_vec m = psl_splat32(mode == 1 ? 0u : 0xFFFFFFFFu);
        for (uint32_t keep = 0; keep < 2; keep++)
            for (uint32_t outside = 0; outside < 2; outside++) {
                ps_spu_vec k = psl_splat32(keep ? ~0u : 0u), o = psl_splat32(outside ? ~0u : 0u);
                __asm__ volatile("" : "+w"(k), "+w"(o), "+w"(m));
                const ps_spu_vec g = psl_fnms(k, m, o);
                j.culled[keep * 2 + outside] = w_of(g, 0) & 1u;
            }
        j.nopix = ld8(ls, s + 0x13D) != 0;
    }
    r.stages |= 4;
    return true;
}

void leave_cull(const Recording& r, uint8_t* ls, SPUContext& ctx)
{
    // The count told (every index, or one triangle: Recording::told) to the halfword at +0x17E and as the result, the
    // buffer's aligned end after it.
    const uint32_t s = pref(ctx, 3), n = r.told ? r.told : r.job.num_indexes;
    st32(ls, s + 0x58, (ld32(ls, s + 0x50) + 2 * n + 15) & ~15u);
    ps_st16(ls + ((s + 0x17E) & PS_SPU_LS_MASK), n);
    ctx.gpr[3].u32[3] = n;
    leave(ls, ctx);
}

// ---- compression (0x0F5B8: $3 context) ----

bool on_compress(Recording& r, const uint8_t* ls, const SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3);
    ps_edge_job& j = r.job;
    if (!output_layout(r, ls, c, j, r.route))
        warn_once(Warn::output_changed, [&] { return std::format("the output vertex changed after the decompression ({}, {:#x})", edge::reason_name(r.reject), r.detail); });
    // The later routes' output attributes the GPU does not compute: the SPU's tables as they stand now (after the
    // driver's steps since the route's stage, such as the colour pass, D-1006).
    for (uint32_t a = 0; a < j.out_count; a++) {
        ps_edge_out_attr& o = j.out[a];
        if (o.source != PS_EDGE_ATTR_TABLE) continue;
        const uint32_t id = o.table;
        if (!table_of_id(ls, c, id))
            warn_once(Warn::missing_table, [&] { return std::format("no table of attribute {} at the compression: what the SPU's compression reads instead", id); });
        o.table = r.append(ls, compress_table_of_id(ls, c, id), 16 * j.num_vertexes);
    }
    r.stages |= 8;
    return true;
}

void leave_compress(const Recording& r, uint8_t* ls, SPUContext& ctx)
{
    const uint32_t c = pref(ctx, 3);
    const uint32_t start = ld32(ls, c + 0x58);
    st32(ls, c + 0x54, start);
    st32(ls, c + 0x58, (start + r.job.num_vertexes * r.job.out_stride + 15) & ~15u);
    ctx.gpr[3].u32[3] = start;
    leave(ls, ctx);
}

} // namespace pspocket::spu::edge_gpu

// ---- the job: the stages' stand-ins, the dropped PUTs, the work item ----

#include "pspocket/core/edge_job.h"
#include "pspocket/core/edge_kernels.h"
#include "pspocket/core/edge_registry.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/timebase.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"
#include "native.h"

namespace pspocket::spu::edge_gpu {
namespace {

constexpr uint64_t kDad4 = 0xDAD457863220B1CCull; // GTA V's EDGE build, the one whose stages are known here

struct JobState {
    Recording rec;
    bool active = false;    // the job takes the deferred path (until its decompression declines)
    bool recorded = false;  // its decompression was recorded: the later stages are skipped too
    bool finished = false;  // its work item went out
    uint32_t lo = ~0u, hi = 0, dropped = 0; // the PUTs into its ring allocation, dropped
    uint32_t first_put = 0;                 // verify: the first one's size (the kept indices)
    uint32_t label_gets = 0;                // GETs of an RSX label: the ring full, its allocator waiting for the RSX
    uint32_t second_ea = 0;                 // the second PUT into the allocation: its vertices
    bool verify = false;                    // the stages run too; nothing dropped
    uint64_t start_ns = 0;
    uint64_t last_end_ns = 0; // this thread's previous EDGE job's end
};
thread_local JobState t_job;

// mode cpu: the work item's kernels here, their output written into the allocation as the job would have put it: the
// kept indices, then (to keep the hole's full count) degenerate triangles, then the vertices.
void run_here(const edge::WorkItem& item, uint8_t* membase)
{
    // The kernels emulate the SPU's rounding on hardware that rounds to nearest: out of the SPU's environment for them.
#if defined(__aarch64__)
    const uint64_t saved = ps_spu_fpcr_read();
    ps_spu_fpcr_write(saved & ~(PS_SPU_FPCR_RMODE | PS_SPU_FPCR_FZ | PS_SPU_FPCR_DN));
    struct Restore {
        uint64_t v;
        ~Restore() { ps_spu_fpcr_write(v); }
    } restore{saved};
#endif
    const ps_edge_job& j = *reinterpret_cast<const ps_edge_job*>(item.blob.data());
    uint8_t* alloc = membase + item.alloc_ea;
    std::vector<ek_screen> scr(j.num_vertexes);
    const std::vector<float> pal = ek_palette(item.blob.data());
    for (uint32_t v = 0; v < j.num_vertexes; v++)
        scr[v] = ek_vertex(item.blob.data(), &j, pal.data(), v, alloc + item.index_bytes + v * j.out_stride);
    const uint8_t* idx = item.blob.data() + j.indices;
    uint32_t out = 0, fill = 0;
    for (uint32_t i = 0; i + 2 < j.num_indexes; i += 3) {
        const uint32_t a = ps_ld16(idx + 2 * i), b = ps_ld16(idx + 2 * i + 2), c = ps_ld16(idx + 2 * i + 4);
        if (a >= j.num_vertexes || b >= j.num_vertexes || c >= j.num_vertexes) continue;
        if (ek_culled(&j, scr[a], scr[b], scr[c])) continue;
        ps_st16(alloc + 2 * out, a);
        ps_st16(alloc + 2 * out + 2, b);
        ps_st16(alloc + 2 * out + 4, c);
        fill = a;
        out += 3;
    }
    for (; out < j.num_indexes; out++) ps_st16(alloc + 2 * out, fill);
}

void finalize(SPUContext* ctx)
{
    JobState& s = t_job;
    s.finished = true;
    Recording& r = s.rec;
    r.finish();
    const ps_edge_job& j = r.job;
    std::unique_ptr<edge::WorkItem> item = edge::acquire(); // a used one, its blob's room kept
    item->alloc_ea = s.lo;
    item->alloc_size = s.hi - s.lo;
    item->index_bytes = (2 * (r.told ? r.told : j.num_indexes) + 15) & ~15u; // where the job put its vertices
    item->out_index_bytes = (2 * j.num_indexes + 15) & ~15u;
    if (!s.verify && s.second_ea && s.second_ea != s.lo + item->index_bytes)
        warn_once(Warn::vertices_put, [&] { return std::format("the vertices put at {:#x} in the allocation, not {:#x}", s.second_ea - s.lo, item->index_bytes); });
    item->guest_index_bytes = s.verify ? s.first_put : 0;
    item->kinds = (uint64_t{1} << static_cast<int>(r.why)) | (uint64_t{1} << edge::kind_route(r.route));
    if (!s.verify && item->index_bytes + j.num_vertexes * j.out_stride > item->alloc_size)
        warn_once(Warn::allocation, [&] { return std::format("an allocation of {:#x} bytes for {:#x} of indices and {} vertices of {} bytes", item->alloc_size,
            item->index_bytes, j.num_vertexes, j.out_stride); });
    std::swap(item->blob, r.blob); // the recording goes on with the old blob's room
    if (edge::mode() == edge::Mode::cpu) {
        run_here(*item, ctx->membase);
        // what the RSX reads now (the job's own PUTs make their stores visible as these do: mfc.cpp)
        std::atomic_thread_fence(std::memory_order_release);
        edge::recycle(std::move(item));
        return;
    }
    edge::publish(std::move(item));
}

// The job's PUTs into RSX local memory once its culling is recorded (its indices, then its vertices): dropped (the GPU makes what they would put). The
// first PUT elsewhere after them is its command buffer hole: the work item goes out before it (the RSX may read the hole
// from then on).
int filter(void*, SPUContext* ctx, uint32_t, uint32_t cmd, uint32_t, uint32_t ea, uint32_t size)
{
    JobState& s = t_job;
    if ((cmd & 0xF0) == 0x40 && ea - 0x40300000u < 0x100000u) s.label_gets++; // the RSX's labels (rsx/context.hpp)
    if (!s.active || !s.recorded || s.finished || (s.rec.stages & 4) == 0) return 0; // from the culling on: the indices
    if ((cmd & 0xF0) != 0x20) return 0; // PUTs only
    if (ea - PS_MEM_RSX_LOCAL_BASE < PS_MEM_RSX_LOCAL_SIZE) {
        if (!s.dropped) s.first_put = size;
        else if (s.dropped == 1) s.second_ea = ea;
        s.lo = std::min(s.lo, ea);
        s.hi = std::max(s.hi, ea + size);
        s.dropped++;
        return s.verify ? 0 : 1; // verify: the job's own output goes out, to be compared
    }
    if (s.dropped) finalize(ctx);
    return 0;
}

bool skipping() { return t_job.active && t_job.recorded; }
// verify: the stages record, then run as ever (the stand-ins decline)
bool verifying() { return t_job.verify; }

} // namespace

bool job_begin(uint64_t key, uint64_t start_ns)
{
    if (key != kDad4 || edge::mode() == edge::Mode::off) return false;
    JobState& s = t_job;
    s.active = edge::deferred_now();
    s.verify = false; // decided as the job's route starts (verify_this)
    s.recorded = s.finished = false;
    s.first_put = s.label_gets = 0;
    s.lo = ~0u;
    s.hi = s.dropped = 0;
    // a batch's jobs share their grab's time: the later of it and this thread's previous job's end (one clock read a
    // job, at its end)
    s.start_ns = std::max(start_ns, s.last_end_ns);
    ps_spu_set_mfc_filter(filter, nullptr); // every EDGE job: its ring waits counted
    return true;
}

void job_end()
{
    JobState& s = t_job;
    ps_spu_set_mfc_filter(nullptr, nullptr);
    const bool deferred = s.active && s.recorded && !s.verify;
    if (deferred && !s.finished && s.dropped) warn_once(Warn::no_hole, [] { return std::string("a job's PUTs dropped but no command buffer hole after them"); });
    const uint64_t now = ps_host_now_ns();
    edge::count_job(deferred, now - s.start_ns, s.label_gets);
    s.last_end_ns = now;
    s.active = false;
}

} // namespace pspocket::spu::edge_gpu

// The stages' stand-ins, listed in GTA V's EDGE build (replace.cpp): each declines (the lifted function runs) unless the
// job takes the deferred path.
using namespace pspocket::spu::edge_gpu;

namespace {

// The job goes to the SPU from here (its earlier stages ran there as ever): counted, said once.
int to_spu(JobState& s, pspocket::edge::Reason why, uint32_t detail)
{
    say_once(pspocket::edge::Route::spu, why, detail);
    pspocket::edge::count_route(pspocket::edge::Route::spu, why);
    s.active = false;
    return 0;
}

// The job's route starts at this stage: counted; the stages from here on record instead of computing (verify: they
// record, then run).
void start(JobState& s)
{
    s.recorded = true;
    s.verify = pspocket::edge::mode() == pspocket::edge::Mode::verify && pspocket::edge::verify_this(s.rec.route, s.rec.why);
    say_once(s.rec.route, s.rec.why, s.rec.detail);
    pspocket::edge::count_route(s.rec.route, s.rec.why);
}

} // namespace

extern "C" int ps_edge_dad4_gpu_0daf0(SPUContext* ctx, uint8_t* ls)
{
    using pspocket::edge::Route;
    JobState& s = t_job;
    if (!s.active || s.recorded) return 0;
    s.rec.clear();
    s.second_ea = 0;
    choose_route(s.rec, ls, *ctx);
    if (s.rec.route == Route::spu) return to_spu(s, s.rec.reject, s.rec.detail);
    if (s.rec.route != Route::decompress) return 0; // the decompression (and what follows it) on the SPU
    if (!on_decompress(s.rec, ls, *ctx)) {
        if (!reroutable(s.rec.reject)) return to_spu(s, s.rec.reject, s.rec.detail);
        // the SPU decompresses; the GPU starts from its tables at the skinning
        const pspocket::edge::Reason why = s.rec.reject;
        const uint32_t detail = s.rec.detail;
        s.rec.clear();
        s.rec.route = Route::skin;
        s.rec.why = why;
        s.rec.detail = detail;
        return 0;
    }
    start(s);
    if (verifying()) return 0;
    leave_decompress(s.rec, ls, *ctx);
    return 1;
}

extern "C" int ps_edge_dad4_gpu_07a10(SPUContext* ctx, uint8_t* ls)
{
    using pspocket::edge::Route;
    JobState& s = t_job;
    if (!s.active) return 0;
    const bool skinned = (ls[(ctx->gpr[3].u32[3] + 6) & PS_SPU_LS_MASK] & 15u) != 0;
    if (!s.recorded && s.rec.route == Route::skin) {
        if (!on_skin_tables(s.rec, ls, *ctx)) {
            if (s.rec.reject != pspocket::edge::Reason::tables) return to_spu(s, s.rec.reject, s.rec.detail);
            // the SPU skins too; the GPU starts from its tables at the culling
            const pspocket::edge::Reason why = s.rec.reject;
            const uint32_t detail = s.rec.detail;
            s.rec.clear();
            s.rec.route = Route::cull;
            s.rec.why = why;
            s.rec.detail = detail;
            return 0;
        }
        start(s);
    }
    if (!s.recorded || s.rec.route == Route::cull) return 0;
    if (!skinned) return 0; // no skinning: the function does nothing
    on_skin(s.rec, ls, *ctx);
    if (verifying()) return 0;
    leave_skin(s.rec, ls, *ctx);
    return 1;
}

extern "C" int ps_edge_dad4_gpu_0eff8(SPUContext* ctx, uint8_t* ls)
{
    using pspocket::edge::Route;
    JobState& s = t_job;
    if (!s.active) return 0;
    if (!s.recorded && s.rec.route == Route::cull) {
        if (!on_cull_tables(s.rec, ls, *ctx)) return to_spu(s, s.rec.reject, s.rec.detail);
        start(s);
    }
    if (!s.recorded) return 0;
    on_cull(s.rec, ls, *ctx);
    if (verifying()) return 0;
    // One triangle told: the GPU's count draws (the CPU mode writes every index into the allocation: all of them).
    s.rec.told = pspocket::edge::mode() == pspocket::edge::Mode::cpu || s.rec.job.num_indexes < 3 ? 0u : 3u;
    leave_cull(s.rec, ls, *ctx);
    return 1;
}

extern "C" int ps_edge_dad4_gpu_0f5b8(SPUContext* ctx, uint8_t* ls)
{
    if (!skipping()) return 0;
    on_compress(t_job.rec, ls, *ctx);
    if (verifying()) return 0;
    leave_compress(t_job.rec, ls, *ctx);
    return 1;
}
