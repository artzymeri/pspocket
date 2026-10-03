// SPU floating-point model support (pspocket/spu/fp.h): the FREST/FRSQEST tables, the host FP environment and the
// double-precision fallbacks for hosts without the AArch64 FPCR window.

#include "pspocket/spu/fp.h"

#include <atomic>

#if !defined(__aarch64__)
#include <cfenv>
#include <cmath>
#endif

#include "pspocket/core/types.hpp"

using pspocket::u32;
using pspocket::u64;

// Tables: RPCS3 rpcs3/Emu/Cell/SPUThread.cpp (spu_frest_fraction_lut, spu_frest_exponent_lut,
// spu_frsqest_fraction_lut, spu_frsqest_exponent_lut). The exponent tables are generated from their closed forms
// below and checked against RPCS3's values in tests/spu.
extern "C" const uint32_t ps_spu_frest_fraction_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07,
};

extern "C" const uint32_t ps_spu_frsqest_fraction_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA,
};

namespace {

// RPCS3's exponent tables in closed form (identical to SPULLVMRecompiler.cpp's FREST/FRSQEST expressions):
// frest: e == 0 -> 0x7F800000, else max(253 - e, 0) << 23; frsqest: e == 0 -> 0x7F800000, else
// (190 - (e + 1) / 2) << 23.
struct ExponentTables {
    uint32_t frest[256];
    uint32_t frsqest[256];
    constexpr ExponentTables() : frest(), frsqest()
    {
        for (u32 e = 0; e < 256; e++) {
            frest[e] = e == 0 ? 0x7F800000u : (e < 253 ? (253u - e) << 23 : 0u);
            frsqest[e] = e == 0 ? 0x7F800000u : (190u - (e + 1) / 2) << 23;
        }
    }
};
constexpr ExponentTables kExponentTables;

std::atomic<uint8_t> g_default_xfloat{PS_SPU_XFLOAT_APPROXIMATE};

} // namespace

extern "C" const uint32_t ps_spu_frest_exponent_lut[256] = {
#define E(n) kExponentTables.frest[n]
#define ROW(b) E(b), E(b + 1), E(b + 2), E(b + 3), E(b + 4), E(b + 5), E(b + 6), E(b + 7)
    ROW(0), ROW(8), ROW(16), ROW(24), ROW(32), ROW(40), ROW(48), ROW(56),
    ROW(64), ROW(72), ROW(80), ROW(88), ROW(96), ROW(104), ROW(112), ROW(120),
    ROW(128), ROW(136), ROW(144), ROW(152), ROW(160), ROW(168), ROW(176), ROW(184),
    ROW(192), ROW(200), ROW(208), ROW(216), ROW(224), ROW(232), ROW(240), ROW(248),
#undef E
#undef ROW
};

extern "C" const uint32_t ps_spu_frsqest_exponent_lut[256] = {
#define E(n) kExponentTables.frsqest[n]
#define ROW(b) E(b), E(b + 1), E(b + 2), E(b + 3), E(b + 4), E(b + 5), E(b + 6), E(b + 7)
    ROW(0), ROW(8), ROW(16), ROW(24), ROW(32), ROW(40), ROW(48), ROW(56),
    ROW(64), ROW(72), ROW(80), ROW(88), ROW(96), ROW(104), ROW(112), ROW(120),
    ROW(128), ROW(136), ROW(144), ROW(152), ROW(160), ROW(168), ROW(176), ROW(184),
    ROW(192), ROW(200), ROW(208), ROW(216), ROW(224), ROW(232), ROW(240), ROW(248),
#undef E
#undef ROW
};

extern "C" {

void ps_spu_set_default_xfloat(uint8_t mode) { g_default_xfloat.store(mode ? PS_SPU_XFLOAT_ACCURATE : PS_SPU_XFLOAT_APPROXIMATE); }
uint8_t ps_spu_default_xfloat(void) { return g_default_xfloat.load(); }

#if defined(__aarch64__)

uint64_t ps_spu_fpenv_enter(void)
{
    const u64 current = ps_spu_fpcr_read();
    const u64 env = ps_spu_fpcr_env(current);
    if (env == current) return 0;
    ps_spu_fpcr_write(env);
    return current | (1ull << 63); // bit 63 marks "restore" (FPCR's top half is RES0)
}

void ps_spu_fpenv_leave(uint64_t token)
{
    if (token >> 63) ps_spu_fpcr_write(token & ~(1ull << 63));
}

int ps_spu_fpenv_active(void)
{
    const u64 current = ps_spu_fpcr_read();
    return ps_spu_fpcr_env(current) == current;
}

#else

// Other hosts: rounding mode only (flush-to-zero and default-NaN behaviour is the host's). Toolchain builds don't
// execute SPU code in production; tests on such hosts are best effort.
uint64_t ps_spu_fpenv_enter(void)
{
    const int old = std::fegetround();
    if (old == FE_TOWARDZERO) return 0;
    std::fesetround(FE_TOWARDZERO);
    return (1ull << 63) | static_cast<u32>(old);
}

void ps_spu_fpenv_leave(uint64_t token)
{
    if (token >> 63) std::fesetround(static_cast<int>(token & 0xFFFFFFFFu));
}

int ps_spu_fpenv_active(void) { return std::fegetround() == FE_TOWARDZERO; }

#pragma STDC FENV_ACCESS ON

namespace {
struct NearestScope {
    int old = std::fegetround();
    NearestScope() { std::fesetround(FE_TONEAREST); }
    ~NearestScope() { std::fesetround(old); }
};
} // namespace

double ps_spu_dp_add(double a, double b)
{
    NearestScope s;
    volatile double r = a + b;
    return r;
}
double ps_spu_dp_sub(double a, double b)
{
    NearestScope s;
    volatile double r = a - b;
    return r;
}
double ps_spu_dp_mul(double a, double b)
{
    NearestScope s;
    volatile double r = a * b;
    return r;
}
double ps_spu_dp_fma(double a, double b, double c)
{
    NearestScope s;
    volatile double r = std::fma(a, b, c);
    return r;
}
float ps_spu_dp_to_sp(double a)
{
    NearestScope s;
    volatile float r = static_cast<float>(a);
    return r;
}

#endif

} // extern "C"
