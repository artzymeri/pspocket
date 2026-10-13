#pragma once

// The RSX shader interpreter (D-940): one MSL vertex function and one fragment function that run any decoded vertex
// or fragment program from a buffer, so a draw whose translated programs or pipeline are still compiling is drawn
// instead of left out (D-158; RPCS3's "async with shader interpreter"). The programs are decoded on the CPU as for
// the translator (ir.hpp) and encoded here into fixed-width words; the MSL (interpreter_source) runs them operation by
// operation as msl.cpp translates them, and reads at run time what the translator's VpOptions and FpOptions decide at
// compile time. What the translator refuses (msl.hpp: the unsupported opcodes, an indexed input past SSA outside a
// loop) is refused here too, so the two always draw the same.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/rsx/shader/ir.hpp"
#include "pspocket/rsx/shader/msl.hpp"

namespace pspocket::rsx::shader {

// Raised whenever the encoding or the interpreter's MSL changes.
inline constexpr u32 kInterpreterVersion = 1;

// Argument table slots of the interpreter, after the translator's (msl.hpp slot::).
namespace islot {
inline constexpr u32 kVsProgram = 4;  // constant uint*: the encoded vertex program
inline constexpr u32 kVsOptions = 5;  // constant InterpVsOptions&
inline constexpr u32 kFsProgram = 3;  // constant uint*: the encoded fragment program
inline constexpr u32 kFsOptions = 4;  // constant InterpFsOptions&
// Textures of each kind (TextureKind) a unit can be: the unit's index plus the kind's base. Unused slots hold a 1x1
// texture of their type.
inline constexpr u32 kVsTex2D = 0, kVsTexCube = 4, kVsTex3D = 8;                    // 4 vertex texture units
inline constexpr u32 kFsTex2D = 0, kFsTexCube = 16, kFsTex3D = 32, kFsTexDepth = 48; // 16 fragment texture units
inline constexpr u32 kFsDepthBounds = 64;
// Function constants of the fragment function (pipeline variants): the colour outputs it has and depth export.
inline constexpr u32 kFcMrt = 0, kFcDepthExport = 1;
} // namespace islot

inline constexpr u32 kVpWords = 8;  // words per encoded vertex program instruction
inline constexpr u32 kFpWords = 12; // words per encoded fragment program instruction
inline constexpr u32 kFpHeaderWords = 4;

// What the translator's VpOptions decide, for the interpreter at run time (MSL rsx_vi_options).
struct alignas(16) InterpVsOptions {
    u32 start = 0;          // the program's first slot (branch targets are slots)
    u32 count = 0;          // encoded instructions
    u32 input_mask = 0;     // v[] registers fetched
    u32 output_mask = 0;    // o[] registers the program writes (the point size rule)
    u32 user_clip = 0;      // VpOptions::user_clip
    u32 masked_outputs = 0; // VpOptions::masked_outputs
    u32 point_size = 0;     // VpOptions::point_size
    u32 texture_kind = 0;   // 4 x 8 bits: VpOptions::texture_kind
};
static_assert(sizeof(InterpVsOptions) == 32);

// What the translator's FpOptions decide (MSL rsx_fi_options). The output count and depth export are function
// constants instead (they change the fragment function's outputs).
struct alignas(16) InterpFsOptions {
    u32 shader_control = 0;
    u32 texcoord_2d_mask = 0;
    u32 flags = 0; // kFi*
    u32 texture_convert_mask = 0;
    u32 texture_alpha_kill_mask = 0;
    u32 point_coord_mask = 0;
    u32 mrt_count = 1;
    u32 pad = 0;
    u32 texture_kind[16] = {}; // TextureKind per unit
    u32 output_swizzle[4] = {0x03020100, 0x03020100, 0x03020100, 0x03020100}; // 4 x 8 bits per output
};
static_assert(sizeof(InterpFsOptions) == 32 + 64 + 16);

inline constexpr u32 kFiAlphaTest = 1, kFiFlatColors = 2, kFiSrgbOutput = 4, kFiAlphaToCoverage = 8,
                     kFiTwoSidedLighting = 16, kFiDepthBounds = 32, kFiPolygonStipple = 64;

InterpVsOptions interp_vs_options(const VertexProgram& vp, const VpOptions& options);
InterpFsOptions interp_fs_options(const FpOptions& options);

struct EncodeResult {
    bool ok = false;
    std::string error; // as the translator words it for the same program
    std::vector<u32> words;
};

// The vertex program as the interpreter runs it: kVpWords words per instruction up to the first one with an invalid
// source type (the translator ends the program there). Refused where translate_vertex_program fails.
EncodeResult encode_vertex_program(const VertexProgram& vp);
// The fragment program: a header (instruction count, which register file each colour output is read from) and
// kFpWords words per instruction, the flow-control blocks' ends and elses as instruction indices. Refused where
// translate_fragment_program fails.
EncodeResult encode_fragment_program(const FragmentProgram& fp);

// The interpreter's MSL: msl_prologue() and the vertex function rsx_vs_interp and fragment function rsx_fs_interp.
const std::string& interpreter_source();
inline constexpr const char* kInterpVsEntry = "rsx_vs_interp";
inline constexpr const char* kInterpFsEntry = "rsx_fs_interp";

} // namespace pspocket::rsx::shader
