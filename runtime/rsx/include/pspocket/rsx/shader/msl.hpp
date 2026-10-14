#pragma once

// RSX shader microcode -> Metal Shading Language.
//
// Programs are hashed with embedded constants masked out; the constants reach the shader in a buffer (plan section 7).
// Vertex programs: every vector op RPCS3 translates, the scalar ops but PSH and POP, condition codes, the address
// registers and flow control (D-086, D-113). Fragment programs: every op RPCS3 translates (D-174 added LIT, LIF, DST,
// REFL, DP2A, PKG, UPG, BEM, TEXBEM and TXPBEM), condition codes, IFE, LOOP, REP, BRK and RET, inputs indexed by the
// loop index. What is left (CAL, POW, BEMLUM, TIMESWTEX, the vertex stack ops) fails with an "unsupported" error
// naming the opcode and its position, so the backend skips the draw loudly. Semantics follow RPCS3
// VertexProgramDecompiler.cpp / FragmentProgramDecompiler.cpp, cited per operation; where pspocket differs, the
// comment says why.
//
// Vertex pulling (plan section 7): the vertex shader reads raw guest bytes (big-endian) from a buffer and decodes
// every attribute itself (RPCS3 GLSLSnippets/RSXProg/RSXVertexFetch.glsl), so the CPU never converts vertex data.
// Index buffers are pulled the same way. The structs below mirror the MSL ones byte for byte.

#include <array>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/rsx/shader/ir.hpp"

namespace pspocket::rsx::shader {

// The translation's version: raised whenever the MSL made for a program and its options changes. The shader cache
// (metal/src/shader_cache.mm) loads only the records of this version: a record of another one holds MSL of the old
// translation under a key the new one also uses. 2: D-174. 4: D-256 to D-263 (FOGC's fog factor, two-sided
// lighting, user clip planes, the depth bounds test, point sizes and sprites, polygon stipple); 3 was a step on the
// way that never left the Mac. 5: D-269 (a vertex program puts out only what its fragment program reads). 6: D-271
// (indexed draws run by Metal over the game's indices; a cached vertex program of 5 reads such an index as a vertex).
// 7: D-274 (vertex textures of every kind). 8: D-275 (point sizes on multisampled targets). 9: D-276 (WPOS in pixels on
// multisampled targets).
inline constexpr u32 kTranslationVersion = 9;

// Argument table slots shared by the translator and the Metal backend.
namespace slot {
inline constexpr u32 kVsVertexData = 0; // device const uchar*: attribute data for the draw (guest byte order)
inline constexpr u32 kVsParams = 1;     // constant MslVsParams&
inline constexpr u32 kVsConstants = 2;  // constant float4*: transform constants c[0..467]
inline constexpr u32 kVsIndexData = 3;  // device const uchar*: index data (guest byte order)
inline constexpr u32 kFsConstants = 0;  // constant float4*: embedded fragment constants, in program order
inline constexpr u32 kFsParams = 1;     // constant MslFsParams&
inline constexpr u32 kFsStipple = 2;    // constant uint*: NV4097_SET_POLYGON_STIPPLE_PATTERN's 32 rows (D-263)
inline constexpr u32 kFsDepthBounds = 16; // depth2d<float>: the depth buffer as it was before the draw (D-261)
} // namespace slot

inline constexpr u32 kAttribModulo = 1;   // vertex index % divisor instead of / divisor
inline constexpr u32 kAttribSwap = 2;     // big-endian elements
inline constexpr u32 kAttribRegister = 4; // use `value` for every vertex (no array)

inline constexpr u32 kIndexNone = 0;     // vertex = first_vertex + vertex_id
inline constexpr u32 kIndexU16BE = 1;    // index buffer of big-endian u16
inline constexpr u32 kIndexU32BE = 2;    // big-endian u32
inline constexpr u32 kIndexU32Host = 3;  // host-order u32 (immediate ARRAY_ELEMENT indices)
// An indexed draw Metal runs over the game's own big-endian indices (D-271): vertex_id is the index as Metal read it,
// byte-reversed, so shared vertices are run once; the shader reverses it back.
inline constexpr u32 kIndexDrawnU16BE = 4;
inline constexpr u32 kIndexDrawnU32BE = 5;

struct alignas(16) MslAttrib {
    u32 offset = 0;  // byte offset in the vertex data buffer of element `first`
    u32 stride = 0;  // bytes between elements; 0 = one element for every vertex
    u32 type = 0;    // VertexType (1-7)
    u32 size = 0;    // components 1-4
    u32 divisor = 0; // frequency: 0/1 per vertex, else divide or (kAttribModulo) modulo
    u32 flags = 0;
    u32 first = 0;   // element index stored at `offset`
    u32 pad = 0;
    float value[4] = {0.f, 0.f, 0.f, 1.f};
};
static_assert(sizeof(MslAttrib) == 48);

struct alignas(16) MslVsParams {
    // Viewport transform rows: clip' = (dot(pos_x, p), dot(pos_y, p), dot(pos_z, p), p.w). Maps RSX window
    // coordinates (viewport scale/offset) to Metal NDC for the bound surface (RPCS3 RSXDrawCommands.cpp
    // fill_scale_offset_data, with Metal's y-up NDC).
    float pos_x[4] = {1.f, 0.f, 0.f, 0.f};
    float pos_y[4] = {0.f, 1.f, 0.f, 0.f};
    float pos_z[4] = {0.f, 0.f, 1.f, 0.f};
    u32 index_mode = kIndexNone;
    u32 index_offset = 0;
    u32 base_index = 0;   // NV4097_SET_VERTEX_DATA_BASE_INDEX (indexed draws)
    u32 first_vertex = 0; // non-indexed draws
    u32 restart_enable = 0;
    u32 restart_index = 0;
    u32 branch_bits = 0; // NV4097_SET_TRANSFORM_BRANCH_BITS: what BRB and CLB test (D-113)
    // Points: NV4097_SET_POINT_SIZE, negated when NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK bit 5 makes the program's o6.x
    // the size where it writes o6 (D-262).
    float point_size = 1.f;
    MslAttrib attribs[16];
    // Points on a multisampled target, drawn one texel per sample (D-250): the size in pixels times the samples across
    // a pixel (D-275).
    float point_scale = 1.f;
    u32 pad2[3] = {};
};
static_assert(sizeof(MslVsParams) == 80 + 16 * 48 + 16);

struct alignas(16) MslFsParams {
    float wpos[4] = {1.f, 0.f, 0.f, 0.f}; // WPOS: y' = y * wpos[0] + wpos[1]; x', y' += wpos[2]
    float tex_scale[16][4] = {};          // xy: coordinate multipliers (1/size for unnormalised textures); for
                                          // Shadow2D, z: the compare function, w: 1 linear filtering + 2 clamp
                                          // the reference to 1 (integer depth)
    u32 alpha_func = 0x207;               // CompareFunc, only read when FpOptions::alpha_test
    float alpha_ref = 0.f;
    float depth_bounds[2] = {0.f, 1.f};   // NV4097_SET_DEPTH_BOUNDS_MIN/MAX, read when FpOptions::depth_bounds (D-261)
    u32 tex_control[16] = {};             // per unit: the texel conversions (texture.hpp texture_control), read by the
                                          // units FpOptions::texture_convert_mask names and by _bx2 samples
    float tex_bias[16] = {};              // per unit: the LOD bias (NV4097_SET_TEXTURE_FILTER), 0 without mipmaps
    u32 fog_mode = 0x800;                 // NV4097_SET_FOG_MODE and _PARAMS: FOGC's fog factor (D-256)
    float fog_param[2] = {};
    u32 aa = 0x0101;                      // samples across (bits 0-7) and down (8-15) a pixel: WPOS in pixels (D-276)
    // The blending FpOptions::blend_in_shader's outputs do in the shader (D-948): NV4097_SET_BLEND_EQUATION, _FUNC_SFACTOR
    // and _FUNC_DFACTOR as the registers hold them (RGB in the low 16 bits, alpha in the high), and the blend colour.
    u32 blend_equation = 0x80068006;
    u32 blend_sfactor = 0x00010001;
    u32 blend_dfactor = 0;
    u32 pad0 = 0;
    float blend_color[4] = {};
};
static_assert(sizeof(MslFsParams) == 16 + 256 + 16 + 64 + 64 + 16 + 32);

// Shadow2D: a depth texture sampled with a depth comparison (RPCS3 shadow_textures: a depth format and a compare
// function other than never and always); the lookup returns the comparison's result in every channel.
// Depth2D and DepthShadow2D: a depth buffer sampled directly (depth2d<float>, D-106), for its value in every channel
// or a depth comparison as Shadow2D.
enum class TextureKind : u8 { Tex2D = 0, Cube = 1, Tex1D = 2, Shadow2D = 3, Tex3D = 4, Depth2D = 5, DepthShadow2D = 6 };

struct VpOptions {
    bool point_size = false; // emit [[point_size]] (point primitives)
    // User clip planes (D-259): NV4097_SET_USER_CLIP_PLANE_CONTROL's layout, 4 bits per plane: 1 keeps what the
    // plane's distance (o5.y, o5.z, o5.w, o6.y, o6.z, o6.w) has below zero, 2 at or above it; 0 (and anything else)
    // leaves the plane off (RPCS3 RSXDrawCommands.cpp fill_user_clip_data, VKVertexProgram.cpp reg_table).
    u32 user_clip = 0;
    // o[] registers NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK leaves out (bit n: o[n]): they reach the fragment program as
    // (0, 0, 0, 1) whatever the program wrote (RPCS3 VKVertexProgram.cpp reg_table, D-267).
    u16 masked_outputs = 0;
    // o[] registers (bit n: o[n], 1-15) the fragment program reads: the only outputs put out besides the position, so
    // a vertex writes no more than the draw needs (D-269). 0xfffe: all of them.
    u16 read_outputs = 0xfffe;
    // The four vertex texture units' kinds (D-274): 2D, 1D (a 2D texture one texel high, at y 0.5), 3D and cube
    // sampled with xyz (RPCS3 GLSLCommon.cpp VERTEX_TEXTURE_FETCH1D/2D/3D/CUBE: textureLod at level 0).
    std::array<TextureKind, 4> texture_kind{};
};


struct FpOptions {
    u32 shader_control = 0; // NV4097_SET_SHADER_CONTROL (0x40: 32-bit exports, 0xe: depth export)
    u32 mrt_count = 1;      // color outputs to emit
    // Per output: the result channel feeding Metal r, g, b, a (0-3 = r, g, b, a; 4 = zero; 5 = one). B8 and G8B8
    // surfaces store specific channels (RPCS3 color_utils.hpp get_g8b8_*/ROP remaps).
    std::array<std::array<u8, 4>, 4> output_swizzle{{{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}}};
    std::array<TextureKind, 16> texture_kind{};
    u32 texcoord_2d_mask = 0; // NV4097_SET_TEX_COORD_CONTROL bit 0 per unit: z = 0, w = 1/w
    bool alpha_test = false;
    bool flat_colors = false;
    // D-174. Per unit: texels converted (texture_control() & texctl::kConvertMask: signed channels, gamma, the biased
    // unsigned remap; RPCS3 RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT), and the alpha kill (RPCS3
    // RSX_SHADER_CONTROL_TEXTURE_ALPHA_KILL). Shadow lookups have neither (RPCS3: no _process_texel).
    u16 texture_convert_mask = 0;
    u16 texture_alpha_kill_mask = 0;
    // D-174, the ROP as RPCS3's RSXROPEpilogue.glsl orders it: the colour outputs to sRGB (NV4097_SET_SHADER_PACKER on
    // an 8-bit target with 16-bit exports; RSXThread.cpp), then the alpha test and alpha to coverage
    // (NV4097_SET_ANTI_ALIASING_CONTROL bit 4, emulated as RPCS3 does without hardware support) on colour 0's alpha.
    bool srgb_output = false;
    bool alpha_to_coverage = false;
    // NV4097_SET_TWO_SIDE_LIGHT_EN: COL0 and COL1 are the back colours (the vertex program's o3, o4) on back faces
    // (RPCS3 VKFragmentProgram.cpp two_sided_lighting, D-257).
    bool two_sided_lighting = false;
    // NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE: a fragment whose pixel's stored depth (slot::kFsDepthBounds) is outside
    // MslFsParams::depth_bounds is discarded (RPCS3 VKDraw.cpp depth bounds, D-261).
    bool depth_bounds = false;
    // Point sprites (NV4097_SET_POINT_SPRITE_CONTROL on a point draw): the texture coordinates whose bit is set are the
    // fragment's place in its point, (x, y, 1, 0), or (x, y, 0, 1/w) when 2D (RPCS3 FragmentProgramDecompiler.cpp
    // texcoord_is_point_coord, D-262).
    u16 point_coord_mask = 0;
    // NV4097_SET_POLYGON_STIPPLE on a polygon draw: a fragment whose bit in the 32x32 pattern (row y % 32, bit x % 32)
    // is clear is discarded (RPCS3 RSXROPPrologue.glsl, D-263).
    bool polygon_stipple = false;
    // The draw's depth is clamped to the viewport's range (NV4097_SET_ZMIN_MAX_CONTROL, D-258). Apple GPUs leave the
    // depth of a fragment function that can discard unclamped: such a program exports its rasterized depth, which Metal
    // clamps as it clamps every exported depth (D-266, D-941).
    bool clamp_depth = false;
    // Outputs (bit n: colour n) whose blending the shader does, reading the pixel from the target (programmable
    // blending, [[color(n)]]), with MslFsParams' equations and factors: the signed equations, which Metal's blending
    // has not (RPCS3 RSXThread.cpp evaluate_programmable_blending_state, RSXProgrammableBlendPrologue.glsl; D-948).
    // blend_unorm: those whose target holds bytes, where a signed equation reads one as two's complement.
    u8 blend_in_shader = 0;
    u8 blend_unorm = 0;
    bool operator==(const FpOptions&) const = default;
};

struct MslShader {
    std::string source;
    std::string entry;
    u32 input_mask = 0;     // VP: attributes fetched; FP: FpInput bits read
    u32 texture_mask = 0;   // FP: texture units sampled; VP: vertex texture units sampled (TXL)
    u32 constant_count = 0; // FP: number of fc[] entries (FragmentProgram::constant_offsets.size())
};

struct TranslateResult {
    bool ok = false;
    std::string error;
    MslShader shader;
};

TranslateResult translate_vertex_program(const VertexProgram& vp, const VpOptions& options = {});
// The opcodes the translation supports (the shader interpreter, D-940, supports the same).
bool vp_vec_op_supported(u32 op);
bool vp_sca_op_supported(u32 op);
bool fp_op_supported(u32 op);
TranslateResult translate_fragment_program(const FragmentProgram& fp, const FpOptions& options = {});

// The embedded constants of a fragment program as stored in guest memory now (games patch them between draws).
void read_fragment_constants(const FragmentProgram& fp, const u8* program_data, std::vector<std::array<float, 4>>& out);

// The shared MSL prologue (types, varyings, vertex fetch). Exposed for tests and the backend's own kernels.
const std::string& msl_prologue();
// The hand-written packs a program that packs gets (D-942), for tests.
const std::string& msl_pack_helpers();

// A fingerprint of the translation itself (D-272): the version, and the MSL made of two built-in programs with no
// option and with every option. A change to what the translator writes changes it, so the shader cache drops records
// of another translation even when kTranslationVersion was not raised.
u64 translation_fingerprint();

} // namespace pspocket::rsx::shader
