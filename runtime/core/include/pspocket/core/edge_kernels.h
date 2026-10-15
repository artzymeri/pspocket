#pragma once

// EDGE geometry's GPU kernels as C++ (D-988): pspocket/core/edge_kernels.inc with the C++ meanings of its words. The
// Metal backend compiles the same code (runtime/rsx/metal/src/edge_geometry.mm); here it runs on the CPU (the deferred
// path's CPU mode, the tests, edge-gpu-check). It emulates the SPU's rounding on hardware that rounds to nearest: run it
// in the default floating-point environment (not the SPU's, ps_spu_fpenv_enter).

#include <cmath>
#include <cstdint>
#include <cstring>

#include "pspocket/core/edge_job.h"

#define EK_DEV
#define EK_IN const
#define EK_THR
#define EK_PAL const
#define EK_TABLE static const
#define EK_INLINE static inline
typedef uint32_t ek_u32;
typedef int32_t ek_s32;
typedef uint8_t ek_u8;
static inline ek_u32 ek_as_u32(float f)
{
    ek_u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}
static inline float ek_as_f32(ek_u32 u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
#define ek_hfma(a, b, c) std::fma(a, b, c) // the hardware's fused product (rounded as the environment says)
#define ek_min(a, b) ((a) < (b) ? (a) : (b))

#define EK_CODE(...) __VA_ARGS__
#include "pspocket/core/edge_kernels.inc"
#undef EK_CODE

#include <vector>

// A job's bones' matrices as ek_vertex reads them.
inline std::vector<float> ek_palette(const uint8_t* blob)
{
    const auto* j = reinterpret_cast<const ps_edge_job*>(blob);
    std::vector<float> pal(12u * j->matrix_count + 12u, 0.0f);
    for (uint32_t i = 0; i < 12u * j->matrix_count; i++) pal[i] = ek_palette_word(blob, j, i);
    return pal;
}
