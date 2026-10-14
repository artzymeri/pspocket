#pragma once

// RSX 3D state: the method register file plus the state that methods build up outside it (vertex program memory,
// transform constants, immediate vertex attribute values, the draw clause between BEGIN and END).
//
// Every method writes its argument into regs[method] first (RPCS3 rsx_state::decode); decoded views below read the
// registers on demand. Bit layouts follow RPCS3 rpcs3/Emu/RSX/rsx_decode.h, RSXTexture.cpp and rsx_vertex_data.h.

#include <array>
#include <bit>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx {

inline constexpr u32 kMaxVertexProgramInstructions = 544; // RPCS3 program_util.h
inline constexpr u32 kTransformConstantCount = 468;        // usable vertex constants c[0]..c[467]
inline constexpr u32 kFragmentTextureCount = 16;
inline constexpr u32 kVertexTextureCount = 4;
inline constexpr u32 kVertexAttributeCount = 16;
inline constexpr u32 kMaxColorTargets = 4;

using float4 = std::array<float, 4>;

inline float bits_to_float(u32 v) { return std::bit_cast<float>(v); }

// ---- decoded views ----------------------------------------------------------------------------------------------

struct ColorBuffer {
    u32 offset = 0;
    u32 pitch = 0;
    u32 dma = 0; // CONTEXT_DMA_COLOR_x: kDmaFrameBuffer (local) or kDmaHostBuffer (main)
};

struct SurfaceInfo {
    u32 color_format = 0; // SurfaceColorFormat
    u32 depth_format = 0; // SurfaceDepthFormat (1 = Z16, 2 = Z24S8)
    u32 type = 0;         // 1 = pitch (linear), 2 = swizzle
    u32 antialias = 0;    // 0 = 1 sample, 3 = diagonal 2x, 4/5 = 4x
    u32 log2_width = 0;   // for swizzled surfaces
    u32 log2_height = 0;
    u16 clip_x = 0, clip_width = 0, clip_y = 0, clip_height = 0;
    u32 target = 0; // SurfaceTarget
    std::array<ColorBuffer, kMaxColorTargets> color{};
    ColorBuffer depth{};
    bool depth_float = false; // NV4097_SET_CONTROL0 bit 12
    u16 window_offset_x = 0, window_offset_y = 0;

    // Indices into `color` of the active render targets, in output order (RPCS3 utility::get_rtt_indexes).
    u32 active_targets(std::array<u32, kMaxColorTargets>& out) const;
};

struct ViewportInfo {
    u16 x = 0, width = 0, y = 0, height = 0; // NV4097_SET_VIEWPORT_HORIZONTAL/VERTICAL (clip rectangle)
    float4 scale{};                          // NV4097_SET_VIEWPORT_SCALE
    float4 offset{};                         // NV4097_SET_VIEWPORT_OFFSET
    float clip_min = 0.f, clip_max = 1.f;    // NV4097_SET_CLIP_MIN/MAX
};

struct ScissorInfo {
    u16 x = 0, width = 0, y = 0, height = 0;
};

struct ShaderWindow {
    u16 height = 0;        // bits 0-11
    bool origin_bottom = false;  // bits 12-15: 0 top, 1 bottom
    bool pixel_center_integer = false; // bits 16-19: 0 half, 1 integer
};

struct StencilFace {
    u32 func = 0;  // CompareFunc
    u8 ref = 0;
    u8 func_mask = 0xff;
    u8 write_mask = 0xff;
    u32 op_fail = 0, op_zfail = 0, op_zpass = 0; // StencilOp
};

struct DepthStencilInfo {
    bool depth_test = false;
    u32 depth_func = 0; // CompareFunc
    bool depth_write = true;
    bool stencil_test = false;
    bool two_sided_stencil = false;
    StencilFace front{}, back{};
    bool depth_bounds_test = false;
    float depth_bounds_min = 0.f, depth_bounds_max = 1.f;
};

struct BlendInfo {
    bool enable = false;     // target 0
    u32 enable_mrt = 0;      // bit 1/2/3: targets B/C/D
    u32 sfactor_rgb = 1, sfactor_a = 1, dfactor_rgb = 0, dfactor_a = 0; // BlendFactor
    u32 equation_rgb = 0x8006, equation_a = 0x8006;                     // BlendEquation
    u32 color = 0;  // NV4097_SET_BLEND_COLOR (8-bit BGRA: b bits 0-7, g 8-15, r 16-23, a 24-31)
    u32 color2 = 0; // NV4097_SET_BLEND_COLOR2 (16-bit B and A for float targets)
    bool logic_op_enable = false;
    u32 logic_op = 0;

    bool enabled_for(u32 target_index) const { return target_index == 0 ? enable : ((enable_mrt >> target_index) & 1); }
};

struct RasterInfo {
    bool cull_enable = false;
    u32 cull_face = 0x405;  // CullFace
    u32 front_face = 0x901; // FrontFace
    u32 front_polygon_mode = 0x1b02, back_polygon_mode = 0x1b02;
    bool poly_offset_fill = false, poly_offset_line = false, poly_offset_point = false;
    float poly_offset_scale = 0.f, poly_offset_bias = 0.f;
    float line_width = 1.f;
    float point_size = 1.f;
    u32 shade_mode = 0x1d01;
    bool alpha_test = false;
    u32 alpha_func = 0x207;
    u32 alpha_ref = 0;
    bool dither = true;
    bool restart_index_enable = false;
    u32 restart_index = 0xffffffff;
    bool two_side_light = false;
    u32 user_clip_control = 0;
    bool polygon_stipple = false;
    // NV4097_SET_ZMIN_MAX_CONTROL (RPCS3 rsx_decode.h zmin_max_control): bits 0-3 clip window z to the clip range,
    // bits 4-7 clamp it there instead.
    bool depth_clip = true, depth_clamp = false;
};

// Color write mask per target: bit 0 = R, 1 = G, 2 = B, 3 = A.
struct ColorMaskInfo {
    std::array<u8, kMaxColorTargets> mask{};
};

struct VertexArrayInfo {
    VertexType type = VertexType::F;
    u8 size = 0;       // components, 0 = disabled
    u8 stride = 0;     // bytes, 0 = one element for every vertex
    u16 frequency = 0; // instancing divisor; 0 and 1 mean per vertex
    u32 offset = 0;    // offset within the location
    u32 location = 0;  // 0 local, 1 main
    bool enabled() const { return size != 0; }
};

struct IndexArrayInfo {
    u32 offset = 0;
    u32 location = 0;
    bool u16_indices = false; // type 1 = 16-bit, 0 = 32-bit
};

// Fragment (or vertex) texture unit descriptor (RSXTexture.cpp).
struct TextureInfo {
    u32 offset = 0;
    u32 location = 0;   // 0 local, 1 main
    bool cubemap = false;
    bool border_type = false;
    u32 dimension = 2;  // 1, 2, 3
    u32 format = 0;     // full format byte incl. LN/UN flags
    u32 mipmaps = 1;
    u32 wrap_s = 1, wrap_t = 1, wrap_r = 1; // tex::Wrap
    u32 unsigned_remap = 0;
    u32 gamma = 0;
    u32 aniso_bias = 0;
    u32 signed_remap = 0;
    u32 zfunc = 0;
    bool enabled = false;
    float min_lod = 0.f, max_lod = 0.f;
    u32 max_aniso = 0;
    bool alpha_kill = false;
    u32 remap = tex::kRemapIdentity;
    float bias = 0.f;
    u32 min_filter = 1, mag_filter = 1;
    u32 convolution_filter = 0;
    u32 argb_signed = 0;
    u16 width = 0, height = 0, depth = 1;
    u32 pitch = 0;
    u32 border_color = 0;

    u32 base_format() const { return format & ~(tex::kLinearFlag | tex::kUnnormalized); }
    bool is_linear() const { return (format & tex::kLinearFlag) != 0; }
    bool is_unnormalized() const { return (format & tex::kUnnormalized) != 0; }
};

struct ShaderProgramInfo {
    u32 offset = 0;
    u32 location = 0; // 0 local, 1 main
    u32 control = 0;  // NV4097_SET_SHADER_CONTROL
};

// ---- draw clause ------------------------------------------------------------------------------------------------

enum class DrawCommand : u8 { None, Array, Indexed, InlineArray };

struct DrawRange {
    u32 first = 0;
    u32 count = 0;
};

// An attribute value set through NV4097_SET_VERTEX_DATA*_M, decoded to floats (rsx_vertex_data.h
// register_vertex_data_info). Used for every vertex when the attribute has no enabled array.
struct RegisterVertexData {
    VertexType type = VertexType::F;
    u8 size = 0; // 0 = never written
    std::array<u32, 4> raw{};
    float4 value{0.f, 0.f, 0.f, 1.f};
};

struct DrawClause {
    Primitive primitive = Primitive::Triangles;
    DrawCommand command = DrawCommand::None;
    std::vector<DrawRange> ranges;     // DRAW_ARRAYS / DRAW_INDEX_ARRAY ranges since BEGIN
    std::vector<u32> inline_words;     // INLINE_ARRAY words, in FIFO (host) order
    std::vector<u32> array_elements;   // ARRAY_ELEMENT16 / ARRAY_ELEMENT32 indices
    // Immediate mode: SET_VERTEX_DATA*_M between BEGIN and END. Writing attribute 0 emits a vertex
    // (RPCS3 rsx_vertex_data.cpp push_buffer_vertex_info). One float4 per vertex per attribute.
    std::array<std::vector<float4>, kVertexAttributeCount> immediate{};
    u32 immediate_vertex_count = 0;

    void reset(Primitive prim);
    bool empty() const;
    u32 vertex_count() const; // sum of ranges, or inline/immediate/element counts
};

// ---- the state --------------------------------------------------------------------------------------------------

namespace dirty {
inline constexpr u32 kSurface = 1u << 0;
inline constexpr u32 kVertexProgram = 1u << 1;
inline constexpr u32 kVertexConstants = 1u << 2;
inline constexpr u32 kFragmentProgram = 1u << 3;
inline constexpr u32 kFragmentTextures = 1u << 4;
inline constexpr u32 kVertexTextures = 1u << 5;
inline constexpr u32 kPipeline = 1u << 6; // blend, depth, stencil, raster, masks
inline constexpr u32 kViewport = 1u << 7;
inline constexpr u32 kVertexArrays = 1u << 8;
inline constexpr u32 kAll = 0xffffffffu;
} // namespace dirty

class RsxState {
public:
    RsxState() { reset(); }

    // Context reset values: RPCS3 rsx_methods.cpp rsx_state::init (power-on and cellGcmInit-injected commands).
    void reset();

    // Raw register file.
    std::array<u32, m::kMethodCount> regs{};
    // Vertex program memory (544 instructions x 4 words, host order) and constants.
    std::array<u32, kMaxVertexProgramInstructions * 4> transform_program{};
    std::array<float4, 512> transform_constants{};
    std::array<RegisterVertexData, kVertexAttributeCount> vertex_registers{};
    DrawClause draw;
    bool in_begin_end = false;
    // Set by method handlers, cleared by the backend as it consumes them (both on the RSX thread; the backend sees
    // the state as const).
    mutable u32 dirty_bits = dirty::kAll;
    // NV4097_INVALIDATE_L2 commands so far (cellGcmSetInvalidateTextureCache): the RSX's texture cache sees what draws
    // wrote to a target only after one (D-947).
    u64 texture_cache_epoch = 0;

    u32 reg(u32 r) const { return regs[r]; }
    float reg_f(u32 r) const { return bits_to_float(regs[r]); }

    // Decoded views.
    SurfaceInfo surface() const;
    ViewportInfo viewport() const;
    ScissorInfo scissor() const;
    ShaderWindow shader_window() const;
    DepthStencilInfo depth_stencil() const;
    BlendInfo blend() const;
    RasterInfo raster() const;
    ColorMaskInfo color_mask() const;
    VertexArrayInfo vertex_array(u32 index) const;
    IndexArrayInfo index_array() const;
    TextureInfo fragment_texture(u32 index) const;
    TextureInfo vertex_texture(u32 index) const;
    ShaderProgramInfo shader_program() const;

    u32 vertex_attrib_input_mask() const { return regs[m::NV4097_SET_VERTEX_ATTRIB_INPUT_MASK]; }
    u32 vertex_attrib_output_mask() const { return regs[m::NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK]; }
    u32 transform_program_start() const { return regs[m::NV4097_SET_TRANSFORM_PROGRAM_START]; }
    u32 transform_program_load() const { return regs[m::NV4097_SET_TRANSFORM_PROGRAM_LOAD]; }
    u32 transform_constant_load() const { return regs[m::NV4097_SET_TRANSFORM_CONSTANT_LOAD]; }
    u32 vertex_data_base_offset() const { return regs[m::NV4097_SET_VERTEX_DATA_BASE_OFFSET]; }
    u32 vertex_data_base_index() const { return regs[m::NV4097_SET_VERTEX_DATA_BASE_INDEX]; }
    u32 frequency_divider_mask() const { return regs[m::NV4097_SET_FREQUENCY_DIVIDER_OPERATION]; }
    u32 color_clear_value() const { return regs[m::NV4097_SET_COLOR_CLEAR_VALUE]; } // ARGB8
    u32 zstencil_clear_value() const { return regs[m::NV4097_SET_ZSTENCIL_CLEAR_VALUE]; }
    u32 texcoord_control(u32 i) const { return regs[m::NV4097_SET_TEX_COORD_CONTROL + i]; }
    u32 transform_branch_bits() const { return regs[m::NV4097_SET_TRANSFORM_BRANCH_BITS]; }
};

} // namespace pspocket::rsx
