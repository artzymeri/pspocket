#pragma once

// EDGE geometry's GPU kernels as C++ (D-988): pspocket/core/edge_kernels.inc with the C++ meanings of its words. The
// Metal backend compiles the same code (runtime/rsx/metal/src/edge_kernels.mm); here it runs on the CPU (the deferred
// path's CPU mode, the tests, edge-gpu-check). The floats round as the calling thread's environment says: to nearest
// as the GPU does, or toward zero in the SPU's (ps_spu_fpenv_enter), where every operation but the estimates gives
// the SPU's bits.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "pspocket/core/edge_job.h"

#define EK_DEV
#define EK_IN const
#define EK_THR
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
#define ek_fma(a, b, c) std::fma(a, b, c)
#define ek_min(a, b) ((a) < (b) ? (a) : (b))
/* A product rounded once and never fused into a following add by the compiler (the SPU's fm): fma with -0 adds
 * nothing (a product of +0 stays +0, of -0 stays -0). Every product below that the SPU makes with fm goes through it,
 * so neither compiler (C++ with fp-contract, Metal) changes the arithmetic. */
#define ek_mul(a, b) ek_fma(a, b, -0.0f)

#define EK_CODE(...) __VA_ARGS__
#include "pspocket/core/edge_kernels.inc"
#undef EK_CODE
