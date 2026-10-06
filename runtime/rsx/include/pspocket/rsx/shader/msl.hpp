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
// translation under a key the new one also uses. 2: D-174.
inline constexpr u32 kTranslationVersion = 2;

// Argument table slots shared by the translator and the Metal backend.
namespace slot {
inline constexpr u32 kVsVertexData = 0; // device const uchar*: attribute data for the draw (guest byte order)
inline constexpr u32 kVsParams = 1;     // constant MslVsParams&
inline constexpr u32 kVsConstants = 2;  // constant float4*: transform constants c[0..467]
inline constexpr u32 kVsIndexData = 3;  // device const uchar*: index data (guest byte order)
inline constexpr u32 kFsConstants = 0;  // constant float4*: embedded fragment constants, in program order
inline constexpr u32 kFsParams = 1;     // constant MslFsParams&
} // namespace slot

inline constexpr u32 kAttribModulo = 1;   // vertex index % divisor instead of / divisor
inline constexpr u32 kAttribSwap = 2;     // big-endian elements
inline constexpr u32 kAttribRegister = 4; // use `value` for every vertex (no array)

inline constexpr u32 kIndexNone = 0;     // vertex = first_vertex + vertex_id
inline constexpr u32 kIndexU16BE = 1;    // index buffer of big-endian u16
inline constexpr u32 kIndexU32BE = 2;    // big-endian u32
inline constexpr u32 kIndexU32Host = 3;  // host-order u32 (immediate ARRAY_ELEMENT indices)

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
    u32 pad1 = 0;
    MslAttrib attribs[16];
};
static_assert(sizeof(MslVsParams) == 80 + 16 * 48);

struct alignas(16) MslFsParams {
    float wpos[4] = {1.f, 0.f, 0.f, 0.f}; // WPOS: y' = y * wpos[0] + wpos[1]; x', y' += wpos[2]
    float tex_scale[16][4] = {};          // xy: coordinate multipliers (1/size for unnormalised textures); for
                                          // Shadow2D, z: the compare function, w: 1 linear filtering + 2 clamp
                                          // the reference to 1 (integer depth)
    u32 alpha_func = 0x207;               // CompareFunc, only read when FpOptions::alpha_test
    float alpha_ref = 0.f;
    u32 pad0 = 0, pad1 = 0;
    u32 tex_control[16] = {};             // per unit: the texel conversions (texture.hpp texture_control), read by the
                                          // units FpOptions::texture_convert_mask names and by _bx2 samples
    float tex_bias[16] = {};              // per unit: the LOD bias (NV4097_SET_TEXTURE_FILTER), 0 without mipmaps
};
static_assert(sizeof(MslFsParams) == 16 + 256 + 16 + 64 + 64);

struct VpOptions {
    bool point_size = false; // emit [[point_size]] (point primitives)
};

// Shadow2D: a depth texture sampled with a depth comparison (RPCS3 shadow_textures: a depth format and a compare
// function other than never and always); the lookup returns the comparison's result in every channel.
// Depth2D and DepthShadow2D: a depth buffer sampled directly (depth2d<float>, D-106), for its value in every channel
// or a depth comparison as Shadow2D.
enum class TextureKind : u8 { Tex2D = 0, Cube = 1, Tex1D = 2, Shadow2D = 3, Tex3D = 4, Depth2D = 5, DepthShadow2D = 6 };

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
TranslateResult translate_fragment_program(const FragmentProgram& fp, const FpOptions& options = {});

// The embedded constants of a fragment program as stored in guest memory now (games patch them between draws).
void read_fragment_constants(const FragmentProgram& fp, const u8* program_data, std::vector<std::array<float, 4>>& out);

// The shared MSL prologue (types, varyings, vertex fetch). Exposed for tests and the backend's own kernels.
const std::string& msl_prologue();

} // namespace pspocket::rsx::shader
