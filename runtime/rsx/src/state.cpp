#include "pspocket/rsx/state.hpp"

namespace pspocket::rsx {
namespace {

// Signed/unsigned fixed point with 8 fraction bits (RPCS3 rsx_utils.h decode_fxp<4, 8>).
float fxp_unsigned_4_8(u32 v) { return static_cast<float>(v) / 256.f; }
float fxp_signed_4_8(u32 v13)
{
    const s32 s = static_cast<s32>(v13 << 19) >> 19; // sign-extend 13 bits
    return static_cast<float>(s) / 256.f;
}

u32 compare_func(u32 v) { return (v | 0x200) & 0x20f; }

} // namespace

void DrawClause::reset(Primitive prim)
{
    primitive = prim;
    command = DrawCommand::None;
    ranges.clear();
    inline_words.clear();
    array_elements.clear();
    for (auto& v : immediate) v.clear();
    immediate_vertex_count = 0;
}

bool DrawClause::empty() const
{
    return ranges.empty() && inline_words.empty() && array_elements.empty() && immediate_vertex_count == 0;
}

u32 DrawClause::vertex_count() const
{
    switch (command) {
    case DrawCommand::Array:
    case DrawCommand::Indexed: {
        u32 n = 0;
        for (const DrawRange& r : ranges) n += r.count;
        return n;
    }
    case DrawCommand::InlineArray: return 0; // depends on the vertex layout (stride), see the backend
    case DrawCommand::None:
        if (!array_elements.empty()) return static_cast<u32>(array_elements.size());
        return immediate_vertex_count;
    }
    return 0;
}

void RsxState::reset()
{
    regs.fill(0);
    transform_program.fill(0);
    for (auto& c : transform_constants) c = {0.f, 0.f, 0.f, 0.f};
    for (auto& v : vertex_registers) v = RegisterVertexData{};
    draw.reset(Primitive::Triangles);
    in_begin_end = false;
    dirty_bits = dirty::kAll;

    // RPCS3 rsx_methods.cpp rsx_state::init: power-on values and the commands cellGcmInit injects.
    auto& r = regs;
    r[m::NV4097_SET_SHADER_PROGRAM] = (0 << 2) | (loc::kLocal + 1);
    for (u32 i = 0; i < kFragmentTextureCount; i++) {
        r[m::NV4097_SET_TEXTURE_FORMAT + i * 8] = (1u << 16) | ((tex::R5G6B5) << 8) | (2u << 4) | (loc::kLocal + 1);
        r[m::NV4097_SET_TEXTURE_ADDRESS + i * 8] = 0x30101;
        r[m::NV4097_SET_TEXTURE_CONTROL0 + i * 8] = 0x60000;
        r[m::NV4097_SET_TEXTURE_FILTER + i * 8] = 0x2052000;
        r[m::NV4097_SET_TEXTURE_IMAGE_RECT + i * 8] = 0x80008;
        r[m::NV4097_SET_TEXTURE_CONTROL1 + i * 8] = tex::kRemapIdentity;
        r[m::NV4097_SET_TEXTURE_CONTROL3 + i] = 0x100008;
        r[m::NV4097_SET_TEXTURE_CONTROL2 + i] = 0x2dc8;
    }
    for (u32 i = 0; i < kVertexTextureCount; i++) {
        r[m::NV4097_SET_VERTEX_TEXTURE_FORMAT + i * 8] =
            (1u << 16) | ((tex::X32_FLOAT | tex::kLinearFlag) << 8) | (2u << 4) | (loc::kLocal + 1);
        r[m::NV4097_SET_VERTEX_TEXTURE_ADDRESS + i * 8] = 0x101;
        r[m::NV4097_SET_VERTEX_TEXTURE_CONTROL0 + i * 8] = 0x60000;
        r[m::NV4097_SET_VERTEX_TEXTURE_CONTROL3 + i * 8] = 0x8;
        r[m::NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT + i * 8] = 0x80008;
    }
    r[m::NV406E_SET_CONTEXT_DMA_SEMAPHORE] = loc::kDmaSemaphoreR;
    r[m::NV406E_SEMAPHORE_OFFSET] = 0x10;
    r[m::NV4097_SET_OBJECT] = 0x31337000;
    r[m::NV4097_SET_CONTEXT_DMA_NOTIFIES] = 0x66604200;
    r[m::NV4097_SET_CONTEXT_DMA_A] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_B] = loc::kDmaHostBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_COLOR_A] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_COLOR_B] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_COLOR_C] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_COLOR_D] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_ZETA] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_VERTEX_A] = loc::kDmaFrameBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_VERTEX_B] = loc::kDmaHostBuffer;
    r[m::NV4097_SET_CONTEXT_DMA_SEMAPHORE] = loc::kDmaSemaphoreRW;
    r[m::NV4097_SET_CONTEXT_DMA_REPORT] = loc::kDmaReportLocal;
    r[m::NV4097_SET_SURFACE_FORMAT] = 0x121;
    r[m::NV4097_SET_SURFACE_PITCH_A] = 0x40;
    r[m::NV4097_SET_SURFACE_PITCH_B] = 0x40;
    r[m::NV4097_SET_SURFACE_PITCH_C] = 0x40;
    r[m::NV4097_SET_SURFACE_PITCH_D] = 0x40;
    r[m::NV4097_SET_SURFACE_PITCH_Z] = 0x40;
    r[m::NV4097_SET_SURFACE_COLOR_TARGET] = static_cast<u32>(SurfaceTarget::A);
    r[0x224 >> 2] = 0x80;
    r[0x228 >> 2] = 0x100;
    r[m::NV4097_SET_SURFACE_COMPRESSION] = 0x3;
    for (u32 a = 0x2c0; a <= 0x2fc; a += 4) r[a >> 2] = 0xfff0000;
    r[m::NV4097_SET_CLEAR_RECT_HORIZONTAL] = 0xfff0000;
    r[m::NV4097_SET_CLEAR_RECT_VERTICAL] = 0xfff0000;
    r[m::NV4097_SET_CONTROL0] = 0x100000;
    r[m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK] = 0x3fffff;
    r[m::NV4097_SET_ATTRIB_COLOR] = 0x6144321;
    r[m::NV4097_SET_ATTRIB_TEX_COORD] = 0xedcba987;
    r[m::NV4097_SET_ATTRIB_TEX_COORD_EX] = 0x6f;
    r[m::NV4097_SET_ATTRIB_UCLIP0] = 0x171615;
    r[m::NV4097_SET_ATTRIB_UCLIP1] = 0x1b1a19;
    r[m::NV4097_SET_SPECULAR_ENABLE] = 0x1;
    r[m::NV4097_SET_SHADER_WINDOW] = 0x1000;
    r[m::NV4097_SET_TRANSFORM_EXECUTION_MODE] = 0x11;
    r[m::NV4097_SET_PERFORMANCE_PARAMS] = 0x80003;
    r[m::NV4097_SET_INDEXED_CONSTANT_READ_LIMITS] = 0x2000000;
    r[m::NV4097_SET_EDGE_FLAG] = 0x1;
    r[m::NV4097_SET_REDUCE_DST_COLOR] = 0x1;
    r[m::NV4097_SET_FOG_MODE] = 0x800;
    r[0x240 >> 2] = 0xffff;
    for (u32 i = 0; i < 16; i++) r[m::NV4097_SET_ANISO_SPREAD + i] = 0x10101;
    for (u32 i = 0; i < 16; i++) r[(0x400 >> 2) + i] = 0x7421;
    static constexpr u32 k440[32] = {0x9aabaa98, 0x66666789, 0x98766666, 0x89aabaa9, 0x99999999, 0x88888889,
        0x98888888, 0x99999999, 0x56676654, 0x33333345, 0x54333333, 0x45667665, 0xaabbba99, 0x66667899,
        0x99876666, 0x99abbbaa, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (u32 i = 0; i < 16; i++) r[(0x440 >> 2) + i] = k440[i];
    r[m::GCM_SET_DRIVER_OBJECT] = 0xcafebabe;
    r[m::NV4097_SET_ALPHA_FUNC] = 0x207;
    r[m::NV4097_SET_BACK_STENCIL_FUNC] = 0x207;
    r[m::NV4097_SET_BACK_STENCIL_FUNC_MASK] = 0xff;
    r[m::NV4097_SET_BACK_STENCIL_MASK] = 0xff;
    r[m::NV4097_SET_BACK_STENCIL_OP_FAIL] = 0x1e00;
    r[m::NV4097_SET_BACK_STENCIL_OP_ZFAIL] = 0x1e00;
    r[m::NV4097_SET_BACK_STENCIL_OP_ZPASS] = 0x1e00;
    r[m::NV4097_SET_BLEND_EQUATION] = 0x80068006;
    r[m::NV4097_SET_BLEND_FUNC_SFACTOR] = 0x10001;
    r[m::NV4097_SET_BLEND_FUNC_DFACTOR] = 0x0;
    r[m::NV4097_SET_ZSTENCIL_CLEAR_VALUE] = 0xffffff00;
    r[m::NV4097_SET_COLOR_MASK] = 0x1010101;
    r[m::NV4097_SET_CULL_FACE] = 0x405;
    r[m::NV4097_SET_DEPTH_BOUNDS_MAX] = 0x3f800000;
    r[m::NV4097_SET_DEPTH_FUNC] = 0x201;
    r[m::NV4097_SET_DEPTH_MASK] = 0x1;
    r[m::NV4097_SET_DITHER_ENABLE] = 0x1;
    r[m::NV4097_SET_FRONT_FACE] = 0x901;
    r[m::NV4097_SET_LINE_WIDTH] = 0x8;
    r[m::NV4097_SET_LOGIC_OP] = 0x1503;
    r[m::NV4097_SET_POINT_SIZE] = 0x3f800000;
    r[m::NV4097_SET_RESTART_INDEX] = 0xffffffff;
    r[m::NV4097_SET_SCISSOR_HORIZONTAL] = 0x10000000;
    r[m::NV4097_SET_SCISSOR_VERTICAL] = 0x10000000;
    r[m::NV4097_SET_SHADE_MODE] = 0x1d01;
    r[m::NV4097_SET_STENCIL_FUNC] = 0x207;
    r[m::NV4097_SET_STENCIL_FUNC_MASK] = 0xff;
    r[m::NV4097_SET_STENCIL_MASK] = 0xff;
    r[m::NV4097_SET_STENCIL_OP_FAIL] = 0x1e00;
    r[m::NV4097_SET_STENCIL_OP_ZFAIL] = 0x1e00;
    r[m::NV4097_SET_STENCIL_OP_ZPASS] = 0x1e00;
    for (u32 i = 0; i < kVertexAttributeCount; i++) r[m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + i] = 0x2;
    r[m::NV4097_SET_VIEWPORT_HORIZONTAL] = 0x10000000;
    r[m::NV4097_SET_VIEWPORT_VERTICAL] = 0x10000000;
    r[m::NV4097_SET_CLIP_MAX] = 0x3f800000;
    r[m::NV4097_SET_VIEWPORT_OFFSET + 0] = 0x45000000; // 2048.0
    r[m::NV4097_SET_VIEWPORT_OFFSET + 1] = 0x45000000;
    r[m::NV4097_SET_VIEWPORT_OFFSET + 2] = 0x3f000000; // 0.5
    r[m::NV4097_SET_VIEWPORT_SCALE + 0] = 0x45000000;
    r[m::NV4097_SET_VIEWPORT_SCALE + 1] = 0x45000000;
    r[m::NV4097_SET_VIEWPORT_SCALE + 2] = 0x3f000000;
    r[m::NV4097_SET_ANTI_ALIASING_CONTROL] = 0xffff0000;
    r[m::NV4097_SET_BACK_POLYGON_MODE] = 0x1b02;
    r[m::NV4097_SET_FRONT_POLYGON_MODE] = 0x1b02;
    r[m::NV4097_SET_RENDER_ENABLE] = 0x1000000;
    r[m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK] = 0xffff;
    r[m::NV4097_SET_ZMIN_MAX_CONTROL] = 0x1;
    r[m::NV4097_SET_ZCULL_EN] = 0x3;
    r[m::NV4097_SET_ZCULL_CONTROL0] = 0x10;
    r[m::NV4097_SET_ZCULL_CONTROL1] = 0x1000100;
    r[m::NV4097_SET_SCULL_CONTROL] = 0xff000002;
    r[m::NV0039_SET_OBJECT] = 0x31337303;
    r[m::NV0039_SET_CONTEXT_DMA_NOTIFIES] = 0x66604200;
    r[m::NV0039_SET_CONTEXT_DMA_BUFFER_IN] = loc::kDmaHostBuffer;
    r[m::NV0039_SET_CONTEXT_DMA_BUFFER_OUT] = loc::kDmaFrameBuffer;
    r[m::NV0039_FORMAT] = 0x101;
    r[m::NV3062_SET_OBJECT] = 0x313371c3;
    r[m::NV3062_SET_CONTEXT_DMA_NOTIFIES] = 0x66604200;
    r[m::NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE] = loc::kDmaFrameBuffer;
    r[m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN] = loc::kDmaFrameBuffer;
    r[m::NV3062_SET_COLOR_FORMAT] = 0xa;
    r[m::NV3062_SET_PITCH] = 0x400040;
    r[m::NV308A_SET_OBJECT] = 0x31337808;
    r[m::NV308A_SET_CONTEXT_DMA_NOTIFIES] = 0x66604200;
    r[m::NV308A_SET_CONTEXT_SURFACE] = 0x313371c3;
    r[m::NV308A_SET_OPERATION] = 0x3;
    r[m::NV308A_SET_COLOR_FORMAT] = 0x4;
    r[m::NV309E_SET_OBJECT] = 0x31337a73;
    r[m::NV309E_SET_CONTEXT_DMA_NOTIFIES] = 0x66604200;
    r[m::NV309E_SET_CONTEXT_DMA_IMAGE] = loc::kDmaFrameBuffer;
    r[m::NV309E_SET_FORMAT] = 0x1000a;
    r[m::NV3089_SET_OBJECT] = 0x3137af00;
    r[m::NV3089_SET_CONTEXT_DMA_NOTIFIES] = 0x66604200;
    r[m::NV3089_SET_CONTEXT_DMA_IMAGE] = loc::kDmaFrameBuffer;
    r[m::NV3089_SET_CONTEXT_SURFACE] = 0x313371c3;
    r[m::NV3089_SET_COLOR_CONVERSION] = 0x1;
    r[m::NV3089_SET_COLOR_FORMAT] = 0x3;
    r[m::NV3089_SET_OPERATION] = 0x3;
    r[m::NV3089_IMAGE_IN_SIZE] = 0x10002;
    r[m::NV3089_IMAGE_IN_FORMAT] = 0x10000;
    r[m::NV406E_SET_REFERENCE] = 0xffffffff;
}

u32 SurfaceInfo::active_targets(std::array<u32, kMaxColorTargets>& out) const
{
    switch (static_cast<SurfaceTarget>(target)) {
    case SurfaceTarget::None: return 0;
    case SurfaceTarget::A: out[0] = 0; return 1;
    case SurfaceTarget::B: out[0] = 1; return 1;
    case SurfaceTarget::AB: out = {0, 1, 0, 0}; return 2;
    case SurfaceTarget::ABC: out = {0, 1, 2, 0}; return 3;
    case SurfaceTarget::ABCD: out = {0, 1, 2, 3}; return 4;
    }
    return 0;
}

SurfaceInfo RsxState::surface() const
{
    SurfaceInfo s;
    const u32 fmt = regs[m::NV4097_SET_SURFACE_FORMAT];
    s.color_format = fmt & 0x1f;
    s.depth_format = (fmt >> 5) & 0x7;
    s.type = (fmt >> 8) & 0xf;
    s.antialias = (fmt >> 12) & 0xf;
    s.log2_width = (fmt >> 16) & 0xff;
    s.log2_height = (fmt >> 24) & 0xff;
    const u32 ch = regs[m::NV4097_SET_SURFACE_CLIP_HORIZONTAL];
    const u32 cv = regs[m::NV4097_SET_SURFACE_CLIP_VERTICAL];
    s.clip_x = static_cast<u16>(ch & 0xffff);
    s.clip_width = static_cast<u16>(ch >> 16);
    s.clip_y = static_cast<u16>(cv & 0xffff);
    s.clip_height = static_cast<u16>(cv >> 16);
    s.target = regs[m::NV4097_SET_SURFACE_COLOR_TARGET];
    s.color[0] = {regs[m::NV4097_SET_SURFACE_COLOR_AOFFSET], regs[m::NV4097_SET_SURFACE_PITCH_A],
        regs[m::NV4097_SET_CONTEXT_DMA_COLOR_A]};
    s.color[1] = {regs[m::NV4097_SET_SURFACE_COLOR_BOFFSET], regs[m::NV4097_SET_SURFACE_PITCH_B],
        regs[m::NV4097_SET_CONTEXT_DMA_COLOR_B]};
    s.color[2] = {regs[m::NV4097_SET_SURFACE_COLOR_COFFSET], regs[m::NV4097_SET_SURFACE_PITCH_C],
        regs[m::NV4097_SET_CONTEXT_DMA_COLOR_C]};
    s.color[3] = {regs[m::NV4097_SET_SURFACE_COLOR_DOFFSET], regs[m::NV4097_SET_SURFACE_PITCH_D],
        regs[m::NV4097_SET_CONTEXT_DMA_COLOR_D]};
    s.depth = {regs[m::NV4097_SET_SURFACE_ZETA_OFFSET], regs[m::NV4097_SET_SURFACE_PITCH_Z],
        regs[m::NV4097_SET_CONTEXT_DMA_ZETA]};
    s.depth_float = (regs[m::NV4097_SET_CONTROL0] >> 12) & 1;
    const u32 wo = regs[m::NV4097_SET_WINDOW_OFFSET];
    s.window_offset_x = static_cast<u16>(wo & 0xffff);
    s.window_offset_y = static_cast<u16>(wo >> 16);
    return s;
}

ViewportInfo RsxState::viewport() const
{
    ViewportInfo v;
    const u32 h = regs[m::NV4097_SET_VIEWPORT_HORIZONTAL];
    const u32 vv = regs[m::NV4097_SET_VIEWPORT_VERTICAL];
    v.x = static_cast<u16>(h & 0xffff);
    v.width = static_cast<u16>(h >> 16);
    v.y = static_cast<u16>(vv & 0xffff);
    v.height = static_cast<u16>(vv >> 16);
    for (u32 i = 0; i < 4; i++) {
        v.scale[i] = reg_f(m::NV4097_SET_VIEWPORT_SCALE + i);
        v.offset[i] = reg_f(m::NV4097_SET_VIEWPORT_OFFSET + i);
    }
    v.clip_min = reg_f(m::NV4097_SET_CLIP_MIN);
    v.clip_max = reg_f(m::NV4097_SET_CLIP_MAX);
    return v;
}

ScissorInfo RsxState::scissor() const
{
    const u32 h = regs[m::NV4097_SET_SCISSOR_HORIZONTAL];
    const u32 v = regs[m::NV4097_SET_SCISSOR_VERTICAL];
    return {static_cast<u16>(h & 0xffff), static_cast<u16>(h >> 16), static_cast<u16>(v & 0xffff),
        static_cast<u16>(v >> 16)};
}

ShaderWindow RsxState::shader_window() const
{
    const u32 v = regs[m::NV4097_SET_SHADER_WINDOW];
    return {static_cast<u16>(v & 0xfff), ((v >> 12) & 0xf) != 0, ((v >> 16) & 0xf) != 0};
}

DepthStencilInfo RsxState::depth_stencil() const
{
    DepthStencilInfo d;
    d.depth_test = regs[m::NV4097_SET_DEPTH_TEST_ENABLE] != 0;
    d.depth_func = compare_func(regs[m::NV4097_SET_DEPTH_FUNC]);
    d.depth_write = regs[m::NV4097_SET_DEPTH_MASK] != 0;
    d.stencil_test = regs[m::NV4097_SET_STENCIL_TEST_ENABLE] != 0;
    d.two_sided_stencil = regs[m::NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE] != 0;
    d.front.func = compare_func(regs[m::NV4097_SET_STENCIL_FUNC]);
    d.front.ref = static_cast<u8>(regs[m::NV4097_SET_STENCIL_FUNC_REF]);
    d.front.func_mask = static_cast<u8>(regs[m::NV4097_SET_STENCIL_FUNC_MASK]);
    d.front.write_mask = static_cast<u8>(regs[m::NV4097_SET_STENCIL_MASK]);
    d.front.op_fail = regs[m::NV4097_SET_STENCIL_OP_FAIL];
    d.front.op_zfail = regs[m::NV4097_SET_STENCIL_OP_ZFAIL];
    d.front.op_zpass = regs[m::NV4097_SET_STENCIL_OP_ZPASS];
    if (d.two_sided_stencil) {
        d.back.func = compare_func(regs[m::NV4097_SET_BACK_STENCIL_FUNC]);
        d.back.ref = static_cast<u8>(regs[m::NV4097_SET_BACK_STENCIL_FUNC_REF]);
        d.back.func_mask = static_cast<u8>(regs[m::NV4097_SET_BACK_STENCIL_FUNC_MASK]);
        d.back.write_mask = static_cast<u8>(regs[m::NV4097_SET_BACK_STENCIL_MASK]);
        d.back.op_fail = regs[m::NV4097_SET_BACK_STENCIL_OP_FAIL];
        d.back.op_zfail = regs[m::NV4097_SET_BACK_STENCIL_OP_ZFAIL];
        d.back.op_zpass = regs[m::NV4097_SET_BACK_STENCIL_OP_ZPASS];
    } else {
        d.back = d.front;
    }
    d.depth_bounds_test = regs[m::NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE] != 0;
    d.depth_bounds_min = reg_f(m::NV4097_SET_DEPTH_BOUNDS_MIN);
    d.depth_bounds_max = reg_f(m::NV4097_SET_DEPTH_BOUNDS_MAX);
    return d;
}

BlendInfo RsxState::blend() const
{
    BlendInfo b;
    b.enable = regs[m::NV4097_SET_BLEND_ENABLE] != 0;
    b.enable_mrt = regs[m::NV4097_SET_BLEND_ENABLE_MRT] & 0xe;
    const u32 sf = regs[m::NV4097_SET_BLEND_FUNC_SFACTOR];
    const u32 df = regs[m::NV4097_SET_BLEND_FUNC_DFACTOR];
    const u32 eq = regs[m::NV4097_SET_BLEND_EQUATION];
    b.sfactor_rgb = sf & 0xffff;
    b.sfactor_a = sf >> 16;
    b.dfactor_rgb = df & 0xffff;
    b.dfactor_a = df >> 16;
    b.equation_rgb = eq & 0xffff;
    b.equation_a = eq >> 16;
    b.color = regs[m::NV4097_SET_BLEND_COLOR];
    b.color2 = regs[m::NV4097_SET_BLEND_COLOR2];
    b.logic_op_enable = regs[m::NV4097_SET_LOGIC_OP_ENABLE] != 0;
    b.logic_op = regs[m::NV4097_SET_LOGIC_OP];
    return b;
}

RasterInfo RsxState::raster() const
{
    RasterInfo r;
    r.cull_enable = regs[m::NV4097_SET_CULL_FACE_ENABLE] != 0;
    r.cull_face = regs[m::NV4097_SET_CULL_FACE];
    r.front_face = regs[m::NV4097_SET_FRONT_FACE];
    r.front_polygon_mode = regs[m::NV4097_SET_FRONT_POLYGON_MODE];
    r.back_polygon_mode = regs[m::NV4097_SET_BACK_POLYGON_MODE];
    r.poly_offset_fill = regs[m::NV4097_SET_POLY_OFFSET_FILL_ENABLE] != 0;
    r.poly_offset_line = regs[m::NV4097_SET_POLY_OFFSET_LINE_ENABLE] != 0;
    r.poly_offset_point = regs[m::NV4097_SET_POLY_OFFSET_POINT_ENABLE] != 0;
    r.poly_offset_scale = reg_f(m::NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR);
    r.poly_offset_bias = reg_f(m::NV4097_SET_POLYGON_OFFSET_BIAS);
    const u32 lw = regs[m::NV4097_SET_LINE_WIDTH]; // 6.3 fixed point
    r.line_width = static_cast<float>(lw >> 3) + static_cast<float>(lw & 7) / 8.f;
    r.point_size = reg_f(m::NV4097_SET_POINT_SIZE);
    r.shade_mode = regs[m::NV4097_SET_SHADE_MODE];
    r.alpha_test = regs[m::NV4097_SET_ALPHA_TEST_ENABLE] != 0;
    r.alpha_func = compare_func(regs[m::NV4097_SET_ALPHA_FUNC]);
    r.alpha_ref = regs[m::NV4097_SET_ALPHA_REF];
    r.dither = regs[m::NV4097_SET_DITHER_ENABLE] != 0;
    r.restart_index_enable = regs[m::NV4097_SET_RESTART_INDEX_ENABLE] != 0;
    r.restart_index = regs[m::NV4097_SET_RESTART_INDEX];
    r.two_side_light = regs[m::NV4097_SET_TWO_SIDE_LIGHT_EN] != 0;
    r.user_clip_control = regs[m::NV4097_SET_USER_CLIP_PLANE_CONTROL];
    r.polygon_stipple = regs[m::NV4097_SET_POLYGON_STIPPLE] != 0;
    const u32 zc = regs[m::NV4097_SET_ZMIN_MAX_CONTROL];
    r.depth_clip = (zc & 0xf) != 0;
    r.depth_clamp = ((zc >> 4) & 0xf) != 0;
    return r;
}

ColorMaskInfo RsxState::color_mask() const
{
    ColorMaskInfo c;
    const u32 v = regs[m::NV4097_SET_COLOR_MASK];
    // NV4097_SET_COLOR_MASK: B bits 0-7, G 8-15, R 16-23, A 24-31 (nonzero = enabled).
    c.mask[0] = static_cast<u8>(((v >> 16) & 0xff ? 1 : 0) | ((v >> 8) & 0xff ? 2 : 0) | (v & 0xff ? 4 : 0) |
        ((v >> 24) & 0xff ? 8 : 0));
    // NV4097_SET_COLOR_MASK_MRT: 4 bits per target 1-3: A bit 0, R bit 1, G bit 2, B bit 3.
    const u32 mrt = regs[m::NV4097_SET_COLOR_MASK_MRT];
    for (u32 i = 1; i < kMaxColorTargets; i++) {
        const u32 bits = (mrt >> (i * 4)) & 0xf;
        c.mask[i] = static_cast<u8>(((bits >> 1) & 1) | (((bits >> 2) & 1) << 1) | (((bits >> 3) & 1) << 2) |
            ((bits & 1) << 3));
    }
    return c;
}

VertexArrayInfo RsxState::vertex_array(u32 index) const
{
    VertexArrayInfo a;
    const u32 fmt = regs[m::NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + index];
    const u32 type = fmt & 0x7;
    a.type = type == 0 ? VertexType::UB256 : static_cast<VertexType>(type);
    a.size = static_cast<u8>((fmt >> 4) & 0xf);
    a.stride = static_cast<u8>((fmt >> 8) & 0xff);
    a.frequency = static_cast<u16>(fmt >> 16);
    const u32 off = regs[m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + index];
    a.offset = off & 0x7fffffff;
    a.location = off >> 31;
    return a;
}

IndexArrayInfo RsxState::index_array() const
{
    IndexArrayInfo i;
    i.offset = regs[m::NV4097_SET_INDEX_ARRAY_ADDRESS] & 0x1fffffff;
    const u32 dma = regs[m::NV4097_SET_INDEX_ARRAY_DMA];
    i.location = dma & 0xf;
    i.u16_indices = ((dma >> 4) & 0xff) == 1;
    return i;
}

TextureInfo RsxState::fragment_texture(u32 index) const
{
    TextureInfo t;
    const u32 base = index * 8;
    t.offset = regs[m::NV4097_SET_TEXTURE_OFFSET + base] & 0x7fffffff;
    const u32 fmt = regs[m::NV4097_SET_TEXTURE_FORMAT + base];
    t.location = (fmt & 0x3) - 1;
    t.cubemap = (fmt >> 2) & 1;
    t.border_type = (fmt >> 3) & 1;
    t.dimension = (fmt >> 4) & 0xf;
    t.format = (fmt >> 8) & 0xff;
    t.mipmaps = fmt >> 16;
    const u32 addr = regs[m::NV4097_SET_TEXTURE_ADDRESS + base];
    t.wrap_s = addr & 0xf;
    t.aniso_bias = (addr >> 4) & 0xf;
    t.wrap_t = (addr >> 8) & 0xf;
    t.unsigned_remap = (addr >> 12) & 0xf;
    t.wrap_r = (addr >> 16) & 0xf;
    t.gamma = (addr >> 20) & 0xf;
    t.signed_remap = (addr >> 24) & 0xf;
    t.zfunc = (addr >> 28) & 0xf;
    const u32 c0 = regs[m::NV4097_SET_TEXTURE_CONTROL0 + base];
    t.enabled = (c0 >> 31) & 1;
    t.min_lod = fxp_unsigned_4_8((c0 >> 19) & 0xfff);
    t.max_lod = fxp_unsigned_4_8((c0 >> 7) & 0xfff);
    t.max_aniso = (c0 >> 4) & 0x7;
    t.alpha_kill = (c0 >> 2) & 1;
    t.remap = regs[m::NV4097_SET_TEXTURE_CONTROL1 + base];
    const u32 filter = regs[m::NV4097_SET_TEXTURE_FILTER + base];
    t.bias = fxp_signed_4_8(filter & 0x1fff);
    t.convolution_filter = (filter >> 13) & 0x7;
    t.min_filter = (filter >> 16) & 0x7;
    t.mag_filter = (filter >> 24) & 0x7;
    t.argb_signed = (filter >> 28) & 0xf;
    const u32 rect = regs[m::NV4097_SET_TEXTURE_IMAGE_RECT + base];
    t.width = static_cast<u16>(rect >> 16);
    t.height = t.dimension == 1 ? 1 : static_cast<u16>(rect & 0xffff);
    const u32 c3 = regs[m::NV4097_SET_TEXTURE_CONTROL3 + index];
    t.pitch = c3 & 0xfffff;
    t.depth = t.dimension == 3 ? static_cast<u16>(c3 >> 20) : 1;
    t.border_color = regs[m::NV4097_SET_TEXTURE_BORDER_COLOR + base];
    return t;
}

TextureInfo RsxState::vertex_texture(u32 index) const
{
    TextureInfo t;
    const u32 base = index * 8;
    t.offset = regs[m::NV4097_SET_VERTEX_TEXTURE_OFFSET + base] & 0x7fffffff;
    const u32 fmt = regs[m::NV4097_SET_VERTEX_TEXTURE_FORMAT + base];
    t.location = (fmt & 0x3) - 1;
    t.cubemap = (fmt >> 2) & 1;
    t.border_type = true;
    t.dimension = (fmt >> 4) & 0xf;
    t.format = (fmt >> 8) & 0xff;
    t.mipmaps = fmt >> 16;
    const u32 addr = regs[m::NV4097_SET_VERTEX_TEXTURE_ADDRESS + base];
    t.wrap_s = addr & 0xf;
    t.wrap_t = (addr >> 8) & 0xf;
    t.wrap_r = static_cast<u32>(tex::Wrap::Wrap);
    const u32 c0 = regs[m::NV4097_SET_VERTEX_TEXTURE_CONTROL0 + base];
    t.enabled = (c0 >> 31) & 1;
    t.min_lod = fxp_unsigned_4_8((c0 >> 19) & 0xfff);
    t.max_lod = fxp_unsigned_4_8((c0 >> 7) & 0xfff);
    t.remap = tex::kRemapIdentity;
    t.bias = fxp_signed_4_8(regs[m::NV4097_SET_VERTEX_TEXTURE_FILTER + base] & 0x1fff);
    t.min_filter = static_cast<u32>(tex::MinFilter::Nearest);
    t.mag_filter = static_cast<u32>(tex::MagFilter::Nearest);
    const u32 rect = regs[m::NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT + base];
    t.width = static_cast<u16>(rect >> 16);
    t.height = t.dimension == 1 ? 1 : static_cast<u16>(rect & 0xffff);
    const u32 c3 = regs[m::NV4097_SET_VERTEX_TEXTURE_CONTROL3 + base];
    t.pitch = c3 & 0xfffff;
    t.depth = t.dimension == 3 ? static_cast<u16>(c3 >> 20) : 1;
    t.border_color = regs[m::NV4097_SET_VERTEX_TEXTURE_BORDER_COLOR + base];
    return t;
}

ShaderProgramInfo RsxState::shader_program() const
{
    const u32 v = regs[m::NV4097_SET_SHADER_PROGRAM] & 0x7fffffff;
    return {v & ~3u, (v & 3) - 1, regs[m::NV4097_SET_SHADER_CONTROL]};
}

} // namespace pspocket::rsx
