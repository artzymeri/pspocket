// sys_libm (runtime/hle/src/sys_libm.cpp, D-510): the C99 math library of the PS3 SDK, registered under sys_libm and
// sys_libc. No firmware module has it (games link it into themselves), so these check it against the C standard and the
// SDK's conventions: each function called as guest code calls it (arguments in f1.., r3..; results in f1, r3, v2), its
// results the host's C99 ones, and the PS3's differences (32-bit long, Dinkumware's FP_ILOGB values, the PPU's
// saturating conversions and current rounding mode, VMX's flush of denormals). No game data.

#include <catch_amalgamated.hpp>

#include <bit>
#include <climits>
#include <cmath>
#include <complex>
#include <cstring>
#include <format>
#include <limits>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/ppu/fpu.h"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

// An argument as the 64-bit PowerPC ABI passes it: a double in the next FPR (f1..), an integer in the GPR of its
// position (r3 + n); a double takes up its GPR position too, so the integers after it come from later GPRs.
struct Arg {
    bool fp;
    double d = 0;
    u64 i = 0;
};
Arg F(double v) { return Arg{true, v, 0}; }
Arg I(u64 v) { return Arg{false, 0, v}; }

double fcall(GuestThread& t, const char* name, std::initializer_list<Arg> args)
{
    const Function& fn = GuestThread::find(name);
    u32 fpr = 1, pos = 0;
    for (const Arg& a : args) {
        if (a.fp) t.ctx.fpr[fpr++] = a.d;
        else t.ctx.gpr[3 + pos] = a.i;
        pos++;
    }
    fn.fn(t.ctx, t.f.base());
    return t.ctx.fpr[1];
}

double fcall(GuestThread& t, const char* name, std::initializer_list<double> f)
{
    const Function& fn = GuestThread::find(name);
    u32 k = 1;
    for (const double v : f) t.ctx.fpr[k++] = v;
    fn.fn(t.ctx, t.f.base());
    return t.ctx.fpr[1];
}

// The same, r3 back.
u64 icall(GuestThread& t, const char* name, std::initializer_list<double> f)
{
    fcall(t, name, f);
    return t.ctx.gpr[3];
}

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }
bool same_f(float a, float b) { return (std::isnan(a) && std::isnan(b)) || std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

const std::vector<double> kPoints = {0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 2.0, 3.25, -7.75, 1e-300, 1e300, 0.1, 10.0, 100.5,
    -100.5, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 0.999999, 1.000001, 709.0, -745.0};

} // namespace

TEST_CASE("sys_libm: each double and float function gives the C99 result through the guest's registers", "[hle][libm]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    struct Unary {
        const char* name;
        double (*d)(double);
        float (*s)(float);
    };
    const Unary unary[] = {{"acos", ::acos, ::acosf}, {"acosh", ::acosh, ::acoshf}, {"asin", ::asin, ::asinf},
        {"asinh", ::asinh, ::asinhf}, {"atan", ::atan, ::atanf}, {"atanh", ::atanh, ::atanhf}, {"cbrt", ::cbrt, ::cbrtf},
        {"ceil", ::ceil, ::ceilf}, {"cos", ::cos, ::cosf}, {"cosh", ::cosh, ::coshf}, {"erf", ::erf, ::erff},
        {"erfc", ::erfc, ::erfcf}, {"exp", ::exp, ::expf}, {"exp2", ::exp2, ::exp2f}, {"expm1", ::expm1, ::expm1f},
        {"fabs", ::fabs, ::fabsf}, {"floor", ::floor, ::floorf}, {"lgamma", ::lgamma, ::lgammaf}, {"log", ::log, ::logf},
        {"log10", ::log10, ::log10f}, {"log1p", ::log1p, ::log1pf}, {"log2", ::log2, ::log2f}, {"logb", ::logb, ::logbf},
        {"round", ::round, ::roundf}, {"sin", ::sin, ::sinf}, {"sinh", ::sinh, ::sinhf}, {"sqrt", ::sqrt, ::sqrtf},
        {"tan", ::tan, ::tanf}, {"tanh", ::tanh, ::tanhf}, {"tgamma", ::tgamma, ::tgammaf}, {"trunc", ::trunc, ::truncf}};
    usize checked = 0;
    for (const Unary& u : unary) {
        for (const double x : kPoints) {
            INFO(std::format("{}({})", u.name, x));
            CHECK(same(fcall(t, u.name, {x}), u.d(x)));
            // The float form gets its argument as a double in f1 and returns a float in double format.
            const float xf = static_cast<float>(x);
            CHECK(same_f(static_cast<float>(fcall(t, std::format("{}f", u.name).c_str(), {static_cast<double>(xf)})), u.s(xf)));
            // The long double form is the double one.
            CHECK(same(fcall(t, std::format("{}l", u.name).c_str(), {x}), u.d(x)));
            checked += 3;
        }
    }
    struct Binary {
        const char* name;
        double (*d)(double, double);
    };
    const Binary binary[] = {{"atan2", ::atan2}, {"copysign", ::copysign}, {"fdim", ::fdim}, {"fmax", ::fmax},
        {"fmin", ::fmin}, {"fmod", ::fmod}, {"hypot", ::hypot}, {"nextafter", ::nextafter}, {"pow", ::pow},
        {"remainder", ::remainder}};
    for (const Binary& bi : binary)
        for (const double x : kPoints)
            for (const double y : {0.0, -0.0, 1.5, -2.0, 3.0, 1e-310, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
                INFO(std::format("{}({}, {})", bi.name, x, y));
                CHECK(same(fcall(t, bi.name, {x, y}), bi.d(x, y)));
                checked++;
            }
    CHECK(same(fcall(t, "fma", {2.0, 3.0, 4.0}), 10.0));
    CHECK(same(fcall(t, "fma", {0x1p-60, 0x1p-60, 1.0}), ::fma(0x1p-60, 0x1p-60, 1.0)));
    CHECK(checked > 2000);
}

TEST_CASE("sys_libm: the PS3's conventions", "[hle][libm]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
    // long is 32 bits; conversions saturate as fctiw/fctid do, a NaN giving the minimum.
    CHECK(static_cast<u32>(icall(t, "lround", {2.5})) == 3);
    CHECK(static_cast<u32>(icall(t, "lround", {-2.5})) == static_cast<u32>(-3));
    CHECK(static_cast<u32>(icall(t, "lround", {1e20})) == 0x7FFFFFFFu);
    CHECK(static_cast<u32>(icall(t, "lround", {-1e20})) == 0x80000000u);
    CHECK(static_cast<u32>(icall(t, "lround", {nan})) == 0x80000000u);
    CHECK(icall(t, "llround", {1e30}) == 0x7FFFFFFFFFFFFFFFull);
    CHECK(icall(t, "llround", {nan}) == 0x8000000000000000ull);
    // rint and lrint round as FPSCR[RN] says.
    t.ctx.fpscr = (t.ctx.fpscr & ~PS_FPSCR_RN) | PS_FPSCR_RN_NEAR;
    CHECK(same(fcall(t, "rint", {2.5}), 2.0));
    CHECK(static_cast<u32>(icall(t, "lrint", {3.5})) == 4);
    t.ctx.fpscr = (t.ctx.fpscr & ~PS_FPSCR_RN) | PS_FPSCR_RN_ZERO;
    CHECK(same(fcall(t, "rint", {2.7}), 2.0));
    CHECK(same(fcall(t, "nearbyint", {-2.7}), -2.0));
    t.ctx.fpscr = (t.ctx.fpscr & ~PS_FPSCR_RN) | PS_FPSCR_RN_PINF;
    CHECK(same(fcall(t, "rint", {2.1}), 3.0));
    CHECK(icall(t, "llrint", {-2.9}) == static_cast<u64>(-2));
    t.ctx.fpscr = (t.ctx.fpscr & ~PS_FPSCR_RN) | PS_FPSCR_RN_MINF;
    CHECK(same(fcall(t, "rint", {2.9}), 2.0));
    fcall(t, "fegetround", std::initializer_list<double>{});
    CHECK(static_cast<s32>(t.ctx.gpr[3]) == static_cast<s32>(PS_FPSCR_RN_MINF));
    t.ctx.fpscr &= ~PS_FPSCR_RN;
    // ilogb: Dinkumware's FP_ILOGB0 (INT_MIN) and FP_ILOGBNAN (INT_MAX); an infinity's is INT_MAX.
    CHECK(static_cast<s32>(icall(t, "ilogb", {0.0})) == INT_MIN);
    CHECK(static_cast<s32>(icall(t, "ilogb", {nan})) == INT_MAX);
    CHECK(static_cast<s32>(icall(t, "ilogb", {inf})) == INT_MAX);
    CHECK(static_cast<s32>(icall(t, "ilogb", {1024.0})) == 10);
    // frexp, modf and remquo write through their pointers; an infinity's exponent is 0.
    ps_write32(b, kData, 0xDEADBEEF);
    CHECK(same(fcall(t, "frexp", {F(48.0), I(kData)}), 0.75));
    CHECK(ps_read32(b, kData) == 6);
    CHECK(same(fcall(t, "frexp", {F(inf), I(kData)}), inf));
    CHECK(ps_read32(b, kData) == 0);
    CHECK(same(fcall(t, "modf", {F(-3.25), I(kData)}), -0.25));
    CHECK(ps_readf64(b, kData) == -3.0);
    // nan("...") is the default quiet NaN whatever the text.
    std::memcpy(b + kData + 0x100, "123\0", 4);
    CHECK(std::bit_cast<u64>(fcall(t, "nan", {I(kData + 0x100)})) == 0x7FF8000000000000ull);
    // ldexp and scalbln take a 32-bit exponent.
    CHECK(same(fcall(t, "ldexp", {F(1.5), I(4)}), 24.0));
    CHECK(same(fcall(t, "scalbln", {F(1.0), I(static_cast<u64>(static_cast<s64>(-3)))}), 0.125));
    // A pointer that is not guest memory: logged, nothing written, the value still returned.
    CHECK(same(fcall(t, "frexp", {F(48.0), I(0x0FFF0000)}), 0.75));
}

TEST_CASE("sys_libm: the SIMD functions work lane by lane, VMX's way with denormals", "[hle][libm]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    const Function& sinf4 = GuestThread::find("sinf4");
    const float in[4] = {0.0f, 0.5f, -1.25f, 3.0f};
    for (u32 i = 0; i < 4; i++) t.ctx.vr[2].f32[i] = in[i];
    sinf4.fn(t.ctx, f.base());
    for (u32 i = 0; i < 4; i++) CHECK(same_f(t.ctx.vr[2].f32[i], ::sinf(in[i])));
    // A denormal in and out is flushed to a signed zero with VSCR[NJ] set (the default).
    const Function& sqrtf4 = GuestThread::find("sqrtf4");
    const float tiny = std::numeric_limits<float>::denorm_min();
    for (u32 i = 0; i < 4; i++) t.ctx.vr[2].f32[i] = i == 1 ? -tiny : tiny * 4;
    sqrtf4.fn(t.ctx, f.base());
    CHECK(same_f(t.ctx.vr[2].f32[0], 0.0f));
    CHECK(same_f(t.ctx.vr[2].f32[1], -0.0f));
    // Two-argument forms take v2 and v3.
    const Function& powf4 = GuestThread::find("powf4");
    for (u32 i = 0; i < 4; i++) {
        t.ctx.vr[2].f32[i] = static_cast<float>(i + 1);
        t.ctx.vr[3].f32[i] = 2.0f;
    }
    powf4.fn(t.ctx, f.base());
    for (u32 i = 0; i < 4; i++) CHECK(same_f(t.ctx.vr[2].f32[i], ::powf(static_cast<float>(i + 1), 2.0f)));
}

TEST_CASE("sys_libm: complex values in f1 and f2", "[hle][libm]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    fcall(t, "csqrt", {-4.0, 0.0});
    CHECK(same(t.ctx.fpr[1], 0.0));
    CHECK(same(t.ctx.fpr[2], 2.0));
    fcall(t, "conj", {1.5, -2.5});
    CHECK(same(t.ctx.fpr[1], 1.5));
    CHECK(same(t.ctx.fpr[2], 2.5));
    fcall(t, "cexp", {0.0, M_PI});
    CHECK(std::abs(t.ctx.fpr[1] + 1.0) < 1e-15);
    CHECK(std::abs(t.ctx.fpr[2]) < 1e-15);
}

TEST_CASE("sys_libm: every function and variable of both libraries is registered", "[hle][libm]")
{
    usize libm = 0, libc = 0;
    for (const Function& fn : registry().functions()) {
        if (fn.module == "sys_libm") libm++;
        if (fn.module == "sys_libc") libc++;
    }
    CHECK(libm >= 522);
    CHECK(libc >= 522);
    const Variable* inf = registry().find_variable("sys_libc", 0x57DBCF27); // _Inf
    REQUIRE(inf != nullptr);
    std::vector<u8> mem(16);
    inf->init(mem.data() - 0, 0);
    CHECK(ps_read64(mem.data(), 0) == 0x7FF0000000000000ull);
}
