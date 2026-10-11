// sys_libm, and the math half of sys_libc: the PS3 SDK's C math library as a game links it as a PRX (D-507).
//
// RPCS3 registers almost the same names under both libraries (rpcs3/Emu/Cell/PPUFunction.cpp, its ppu_get_variable_name
// lists the variables) and implements none of them; no firmware module exports either, so there is no Sony code to
// compare with. The library is Dinkumware's C99 library with Sony's SIMD math (the Cell SDK's simdmath interface) and
// fast float forms beside it. Each function here is the C99 one (Annex F for the special values, Annex G for complex),
// computed by the host's libm, with the PS3's conventions where they differ from the host's:
// - long double is double on the PPU: every `l` function is its double form; `long` is 32 bits (lrint, lround,
//   scalbln), `long long` 64.
// - FP_ILOGB0 is INT_MIN and FP_ILOGBNAN INT_MAX (Dinkumware's math.h; Apple's FP_ILOGBNAN is INT_MIN).
// - lrint/lround and their long long forms convert as the PPU's fctiw/fctid do: saturated, a NaN gives the minimum.
// - rint, nearbyint, lrint and llrint round as FPSCR[RN] says (the guest's current rounding mode). Everything else
//   rounds to nearest, as pspocket's PPU arithmetic does (pspocket/ppu/fpu.h): the fenv functions keep FPSCR as the
//   architecture has it, but no arithmetic sets exception flags and no trap is taken.
// - nan("...") gives Dinkumware's quiet NaN whatever the text says.
// - errno is not set (math_errhandling as MATH_ERREXCEPT): the flags _Feraise raises go to FPSCR.
// - The `_f_` and `f_` fast float forms and the `fast` SIMD forms give the precise results.
// - The SIMD functions (vector float in v2.., results in v2) work element by element; with VSCR[NJ] set (the default)
//   denormal inputs and results are flushed to signed zero, as VMX arithmetic does, except in fabsf4, negatef4 and
//   copysignf4, which only touch sign bits. A vector argument takes two GPR slots (GCC's 64-bit PowerPC ABI), so a
//   pointer after one is in r5 (frexpf4, modff4, sincosf4); divi4 returns its two vectors through a pointer in r3.
// - Complex values are passed and returned as two floating-point registers, the real part first (GCC splits complex
//   arguments on PowerPC and returns them in f1 and f2), float complex as two floats in double format.
// Dinkumware's internals (_Dtest, _Exp, _Cosh, _Sin, _Atan, ...) follow the semantics of its xmath.h/ymath.h as the
// C++ library and the macros of math.h use them; four whose signatures are not known (_Eadd, _Emul, _Esub, _Etentox)
// log the call and answer an error. Guest pointers are checked before use (D-042): a call with one that is not guest
// memory is logged with its call site and writes nothing.

#pragma STDC FP_CONTRACT OFF

#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#include <type_traits>

#include "guest.hpp"
#include "libc_guest.hpp"
#include "libc_scan.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/ppu/fpu.h"

// The host's C99 complex functions (libSystem's libm; glibc has them too). <complex.h> means <complex> in C++.
extern "C" {
double _Complex cacos(double _Complex);
double _Complex cacosh(double _Complex);
double _Complex casin(double _Complex);
double _Complex casinh(double _Complex);
double _Complex catan(double _Complex);
double _Complex catanh(double _Complex);
double _Complex ccos(double _Complex);
double _Complex ccosh(double _Complex);
double _Complex cexp(double _Complex);
double _Complex clog(double _Complex);
double _Complex cpow(double _Complex, double _Complex);
double _Complex cproj(double _Complex);
double _Complex csin(double _Complex);
double _Complex csinh(double _Complex);
double _Complex csqrt(double _Complex);
double _Complex ctan(double _Complex);
double _Complex ctanh(double _Complex);
float _Complex cacosf(float _Complex);
float _Complex cacoshf(float _Complex);
float _Complex casinf(float _Complex);
float _Complex casinhf(float _Complex);
float _Complex catanf(float _Complex);
float _Complex catanhf(float _Complex);
float _Complex ccosf(float _Complex);
float _Complex ccoshf(float _Complex);
float _Complex cexpf(float _Complex);
float _Complex clogf(float _Complex);
float _Complex cpowf(float _Complex, float _Complex);
float _Complex cprojf(float _Complex);
float _Complex csinf(float _Complex);
float _Complex csinhf(float _Complex);
float _Complex csqrtf(float _Complex);
float _Complex ctanf(float _Complex);
float _Complex ctanhf(float _Complex);
}

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_libm";
using detail::thunk;

// ------------------------------------------------------------------------------------------------- guest memory

// Whether [addr, addr + size) is committed guest memory, page by page.
bool guest_span(PPUContext& ctx, u32 addr, u64 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (size == 0) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 p = addr & ~(page - 1); p < u64{addr} + size; p += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(p))) return false;
    return true;
}

bool guest_ok(PPUContext& ctx, u32 addr, u32 size) { return guest_readable(thread_of(ctx), addr, size); }

// A pointer that does not lead to guest memory: the call is logged with its site (D-042) and writes nothing.
void refuse(PPUContext& ctx, std::string_view function, u32 addr)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}: {:#010x} is not guest memory; nothing read or written",
                                                    function, addr));
}

// A guest vector at `addr` (stvx/lvx ignore the low four address bits): guest element i is host lane 3 - i (D-004).
void store_vector(u8* base, u32 addr, const ps_v128& v)
{
    for (u32 i = 0; i < 4; i++) ps_write32(base, (addr & ~15u) + 4 * i, v.u32[3 - i]);
}

// ------------------------------------------------------------------------------------------------- C99 conventions

// Dinkumware's classification codes (ymath.h): _Dtest and friends return them; fpclassify's FP_* are the same values.
constexpr s32 kZero = 0, kFinite = -1, kDenorm = -2, kInfCode = 1, kNanCode = 2;

// math.h: FP_ILOGB0 (-0x7fffffff - 1) and FP_ILOGBNAN 0x7fffffff.
template <class T>
s32 ilogb_of(T x)
{
    if (std::isnan(x)) return INT_MAX;
    if (std::isinf(x)) return INT_MAX;
    if (x == 0) return INT_MIN;
    return std::ilogb(x);
}

// Conversions to long (32 bits) and long long, as the PPU's fctiw/fctid: saturated, NaN -> the minimum.
s32 to_long(double t) { return static_cast<s32>(static_cast<u32>(ps_f64_bits(ps_ppu_fctiw(t, PS_FPSCR_RN_ZERO)))); }
u64 to_llong(double t) { return ps_f64_bits(ps_ppu_fctid(t, PS_FPSCR_RN_ZERO)); }

// The guest's current rounding mode (FPSCR[RN]).
u32 rounding(const PPUContext& ctx) { return ctx.fpscr & PS_FPSCR_RN; }

// Dinkumware's quiet NaNs (xvalues.c, _Nan and _FNan): the default NaNs of the PPU.
constexpr u64 kNanBits = 0x7FF8000000000000ull;
constexpr u32 kFNanBits = 0x7FC00000u;

// ------------------------------------------------------------------------------------------------- math.h

#define PS_LIBM_UNARY(name)                                                                                          \
    double m_##name(PPUContext&, double x) { return ::name(x); }                                                     \
    float m_##name##f(PPUContext&, float x) { return ::name##f(x); }
#define PS_LIBM_BINARY(name)                                                                                         \
    double m_##name(PPUContext&, double x, double y) { return ::name(x, y); }                                        \
    float m_##name##f(PPUContext&, float x, float y) { return ::name##f(x, y); }

PS_LIBM_UNARY(acos)
PS_LIBM_UNARY(acosh)
PS_LIBM_UNARY(asin)
PS_LIBM_UNARY(asinh)
PS_LIBM_UNARY(atan)
PS_LIBM_UNARY(atanh)
PS_LIBM_UNARY(cbrt)
PS_LIBM_UNARY(ceil)
PS_LIBM_UNARY(cos)
PS_LIBM_UNARY(cosh)
PS_LIBM_UNARY(erf)
PS_LIBM_UNARY(erfc)
PS_LIBM_UNARY(exp)
PS_LIBM_UNARY(exp2)
PS_LIBM_UNARY(expm1)
PS_LIBM_UNARY(fabs)
PS_LIBM_UNARY(floor)
PS_LIBM_UNARY(lgamma)
PS_LIBM_UNARY(log)
PS_LIBM_UNARY(log10)
PS_LIBM_UNARY(log1p)
PS_LIBM_UNARY(log2)
PS_LIBM_UNARY(logb)
PS_LIBM_UNARY(round)
PS_LIBM_UNARY(sin)
PS_LIBM_UNARY(sinh)
PS_LIBM_UNARY(sqrt)
PS_LIBM_UNARY(tan)
PS_LIBM_UNARY(tanh)
PS_LIBM_UNARY(tgamma)
PS_LIBM_UNARY(trunc)
PS_LIBM_BINARY(atan2)
PS_LIBM_BINARY(copysign)
PS_LIBM_BINARY(fdim)
PS_LIBM_BINARY(fmax)
PS_LIBM_BINARY(fmin)
PS_LIBM_BINARY(fmod)
PS_LIBM_BINARY(hypot)
PS_LIBM_BINARY(nextafter)
PS_LIBM_BINARY(pow)
PS_LIBM_BINARY(remainder)

#undef PS_LIBM_UNARY
#undef PS_LIBM_BINARY

double m_fma(PPUContext&, double x, double y, double z) { return ::fma(x, y, z); }
float m_fmaf(PPUContext&, float x, float y, float z) { return ::fmaf(x, y, z); }

// nexttoward's long double is a double.
double m_nexttoward(PPUContext&, double x, double y) { return ::nextafter(x, y); }
float m_nexttowardf(PPUContext&, float x, double y) { return ::nexttowardf(x, static_cast<long double>(y)); }

double m_ldexp(PPUContext&, double x, s32 n) { return ::scalbn(x, n); }
float m_ldexpf(PPUContext&, float x, s32 n) { return ::scalbnf(x, n); }
double m_scalbn(PPUContext&, double x, s32 n) { return ::scalbn(x, n); }
float m_scalbnf(PPUContext&, float x, s32 n) { return ::scalbnf(x, n); }
double m_scalbln(PPUContext&, double x, s32 n) { return ::scalbn(x, n); }
float m_scalblnf(PPUContext&, float x, s32 n) { return ::scalbnf(x, n); }

s32 m_ilogb(PPUContext&, double x) { return ilogb_of(x); }
s32 m_ilogbf(PPUContext&, float x) { return ilogb_of(x); }

// frexp's exponent of an infinity or a NaN is 0 (Dinkumware's _Dunscale).
double m_frexp(PPUContext& ctx, double x, u32 exp_ptr)
{
    int e = 0;
    const double m = std::frexp(x, &e);
    if (!std::isfinite(x)) e = 0;
    if (!guest_ok(ctx, exp_ptr, 4)) refuse(ctx, "frexp", exp_ptr);
    else ps_write32(guest_base(ctx), exp_ptr, static_cast<u32>(e));
    return m;
}

float m_frexpf(PPUContext& ctx, float x, u32 exp_ptr)
{
    int e = 0;
    const float m = std::frexp(x, &e);
    if (!std::isfinite(x)) e = 0;
    if (!guest_ok(ctx, exp_ptr, 4)) refuse(ctx, "frexpf", exp_ptr);
    else ps_write32(guest_base(ctx), exp_ptr, static_cast<u32>(e));
    return m;
}

double m_modf(PPUContext& ctx, double x, u32 int_ptr)
{
    double i = 0;
    const double f = std::modf(x, &i);
    if (!guest_ok(ctx, int_ptr, 8)) refuse(ctx, "modf", int_ptr);
    else ps_writef64(guest_base(ctx), int_ptr, i);
    return f;
}

float m_modff(PPUContext& ctx, float x, u32 int_ptr)
{
    float i = 0;
    const float f = std::modf(x, &i);
    if (!guest_ok(ctx, int_ptr, 4)) refuse(ctx, "modff", int_ptr);
    else ps_writef32(guest_base(ctx), int_ptr, i);
    return f;
}

double m_remquo(PPUContext& ctx, double x, double y, u32 quo_ptr)
{
    int q = 0;
    const double r = ::remquo(x, y, &q);
    if (!guest_ok(ctx, quo_ptr, 4)) refuse(ctx, "remquo", quo_ptr);
    else ps_write32(guest_base(ctx), quo_ptr, static_cast<u32>(q));
    return r;
}

float m_remquof(PPUContext& ctx, float x, float y, u32 quo_ptr)
{
    int q = 0;
    const float r = ::remquof(x, y, &q);
    if (!guest_ok(ctx, quo_ptr, 4)) refuse(ctx, "remquof", quo_ptr);
    else ps_write32(guest_base(ctx), quo_ptr, static_cast<u32>(q));
    return r;
}

// The text is not read: Dinkumware's nan gives its one quiet NaN.
double m_nan(PPUContext&, u32) { return std::bit_cast<double>(kNanBits); }
float m_nanf(PPUContext&, u32) { return std::bit_cast<float>(kFNanBits); }

// Rounding by FPSCR[RN].
double m_rint(PPUContext& ctx, double x) { return ps_ppu_round_mode(x, rounding(ctx)); }
float m_rintf(PPUContext& ctx, float x)
{
    return static_cast<float>(ps_ppu_round_mode(static_cast<double>(x), rounding(ctx)));
}
double m_nearbyint(PPUContext& ctx, double x) { return ps_ppu_round_mode(x, rounding(ctx)); }
float m_nearbyintf(PPUContext& ctx, float x)
{
    return static_cast<float>(ps_ppu_round_mode(static_cast<double>(x), rounding(ctx)));
}
s32 m_lrint(PPUContext& ctx, double x) { return to_long(ps_ppu_round_mode(x, rounding(ctx))); }
s32 m_lrintf(PPUContext& ctx, float x) { return to_long(ps_ppu_round_mode(static_cast<double>(x), rounding(ctx))); }
u64 m_llrint(PPUContext& ctx, double x) { return to_llong(ps_ppu_round_mode(x, rounding(ctx))); }
u64 m_llrintf(PPUContext& ctx, float x) { return to_llong(ps_ppu_round_mode(static_cast<double>(x), rounding(ctx))); }
s32 m_lround(PPUContext&, double x) { return to_long(std::round(x)); }
s32 m_lroundf(PPUContext&, float x) { return to_long(static_cast<double>(std::round(x))); }
u64 m_llround(PPUContext&, double x) { return to_llong(std::round(x)); }
u64 m_llroundf(PPUContext&, float x) { return to_llong(static_cast<double>(std::round(x))); }

// ------------------------------------------------------------------------------------------------- fenv.h

// fenv.h on the PPU: the exception flags are FPSCR's own bits (FE_INVALID is VX, set through VXSOFT), the rounding
// modes FPSCR[RN]; fenv_t and fexcept_t are one 32-bit word holding FPSCR (FE_DFL_ENV is &_Fenv0, all zero).
constexpr u32 kFeInexact = 0x02000000u, kFeDivByZero = 0x04000000u, kFeUnderflow = 0x08000000u;
constexpr u32 kFeOverflow = 0x10000000u, kFeInvalid = 0x20000000u;
constexpr u32 kFeAll = kFeInexact | kFeDivByZero | kFeUnderflow | kFeOverflow | kFeInvalid;
constexpr u32 kTrapShift = 22; // FE_x >> 22 is its enable bit (VE OE UE ZE XE)
constexpr u32 kTrapEnables = PS_FPSCR_VE | PS_FPSCR_OE | PS_FPSCR_UE | PS_FPSCR_ZE | PS_FPSCR_XE;

void set_fpscr(PPUContext& ctx, u32 value)
{
    static std::atomic<bool> warned_rn{false}, warned_traps{false};
    value = ps_fpscr_summarize(value);
    if ((value & PS_FPSCR_RN) != 0 && !warned_rn.exchange(true))
        PS_LOG_WARN(kChan, "rounding mode {} set: rint, nearbyint, lrint and llrint honour it, arithmetic still rounds "
                           "to nearest (pspocket/ppu/fpu.h)", value & PS_FPSCR_RN);
    if ((value & kTrapEnables) != 0 && !warned_traps.exchange(true))
        PS_LOG_WARN(kChan, "floating-point traps {:#x} enabled: kept in FPSCR, but no exception is ever raised "
                           "(pspocket/ppu/fpu.h)", (value & kTrapEnables) << kTrapShift);
    ctx.fpscr = value;
}

// Raising sets the flags (and FX for a flag that was clear), as an exception the hardware detects would.
u32 raise_flags(u32 fpscr, u32 excepts)
{
    excepts &= kFeAll;
    u32 v = fpscr | (excepts & ~kFeInvalid);
    if (excepts & kFeInvalid) v |= PS_FPSCR_VXSOFT;
    if (excepts & ~(fpscr & kFeAll)) v |= PS_FPSCR_FX;
    return ps_fpscr_summarize(v);
}

u32 clear_flags(u32 fpscr, u32 excepts)
{
    excepts &= kFeAll;
    u32 v = fpscr & ~(excepts & ~kFeInvalid);
    if (excepts & kFeInvalid) v &= ~PS_FPSCR_VX_ALL;
    return ps_fpscr_summarize(v);
}

s32 m_feclearexcept(PPUContext& ctx, s32 excepts)
{
    ctx.fpscr = clear_flags(ctx.fpscr, static_cast<u32>(excepts));
    return 0;
}

s32 m_feraiseexcept(PPUContext& ctx, s32 excepts)
{
    ctx.fpscr = raise_flags(ctx.fpscr, static_cast<u32>(excepts));
    return 0;
}

s32 m_fetestexcept(PPUContext& ctx, s32 excepts) { return static_cast<s32>(ctx.fpscr & static_cast<u32>(excepts) & kFeAll); }

s32 m_fegetexceptflag(PPUContext& ctx, u32 flag_ptr, s32 excepts)
{
    if (!guest_ok(ctx, flag_ptr, 4)) {
        refuse(ctx, "fegetexceptflag", flag_ptr);
        return -1;
    }
    ps_write32(guest_base(ctx), flag_ptr, ctx.fpscr & static_cast<u32>(excepts) & kFeAll);
    return 0;
}

// Sets the flags as the saved word has them, without raising (FX unchanged).
s32 m_fesetexceptflag(PPUContext& ctx, u32 flag_ptr, s32 excepts)
{
    if (!guest_ok(ctx, flag_ptr, 4)) {
        refuse(ctx, "fesetexceptflag", flag_ptr);
        return -1;
    }
    const u32 which = static_cast<u32>(excepts) & kFeAll;
    const u32 saved = ps_read32(guest_base(ctx), flag_ptr) & which;
    u32 v = clear_flags(ctx.fpscr, which) | (saved & ~kFeInvalid);
    if (saved & kFeInvalid) v |= PS_FPSCR_VXSOFT;
    ctx.fpscr = ps_fpscr_summarize(v);
    return 0;
}

s32 m_fegetround(PPUContext& ctx) { return static_cast<s32>(rounding(ctx)); }

s32 m_fesetround(PPUContext& ctx, s32 round)
{
    if (static_cast<u32>(round) & ~PS_FPSCR_RN) return -1;
    set_fpscr(ctx, (ctx.fpscr & ~PS_FPSCR_RN) | static_cast<u32>(round));
    return 0;
}

s32 m_fegetenv(PPUContext& ctx, u32 env_ptr)
{
    if (!guest_ok(ctx, env_ptr, 4)) {
        refuse(ctx, "fegetenv", env_ptr);
        return -1;
    }
    ps_write32(guest_base(ctx), env_ptr, ctx.fpscr);
    return 0;
}

s32 m_fesetenv(PPUContext& ctx, u32 env_ptr)
{
    if (!guest_ok(ctx, env_ptr, 4)) {
        refuse(ctx, "fesetenv", env_ptr);
        return -1;
    }
    set_fpscr(ctx, ps_read32(guest_base(ctx), env_ptr));
    return 0;
}

// Saves the environment, then clears the flags and disables every trap (non-stop mode).
s32 m_feholdexcept(PPUContext& ctx, u32 env_ptr)
{
    if (!guest_ok(ctx, env_ptr, 4)) {
        refuse(ctx, "feholdexcept", env_ptr);
        return -1;
    }
    ps_write32(guest_base(ctx), env_ptr, ctx.fpscr);
    ctx.fpscr = ps_fpscr_summarize(ctx.fpscr & ~(PS_FPSCR_FX | PS_FPSCR_EXCEPTIONS | kTrapEnables));
    return 0;
}

// Installs the environment, then raises the flags that were set before.
s32 m_feupdateenv(PPUContext& ctx, u32 env_ptr)
{
    if (!guest_ok(ctx, env_ptr, 4)) {
        refuse(ctx, "feupdateenv", env_ptr);
        return -1;
    }
    const u32 raised = ctx.fpscr & kFeAll;
    set_fpscr(ctx, ps_read32(guest_base(ctx), env_ptr));
    ctx.fpscr = raise_flags(ctx.fpscr, raised);
    return 0;
}

// Dinkumware's trap controls: the FE_* exceptions whose traps are enabled.
s32 m_fegettrapenable(PPUContext& ctx) { return static_cast<s32>((ctx.fpscr & kTrapEnables) << kTrapShift); }

s32 m_fesettrapenable(PPUContext& ctx, s32 enables)
{
    set_fpscr(ctx, (ctx.fpscr & ~kTrapEnables) | ((static_cast<u32>(enables) & kFeAll) >> kTrapShift));
    return 0;
}

// _Feraise takes ymath.h's _FE_* codes (1 invalid, 4 divide by zero, 8 overflow, 0x10 underflow, 0x20 inexact);
// FE_* bits are taken as they are.
u32 fe_of_code(u32 code)
{
    u32 e = code & kFeAll;
    if (code & 0x01) e |= kFeInvalid;
    if (code & 0x04) e |= kFeDivByZero;
    if (code & 0x08) e |= kFeOverflow;
    if (code & 0x10) e |= kFeUnderflow;
    if (code & 0x20) e |= kFeInexact;
    return e;
}

void dk_Feraise(PPUContext& ctx, s32 code) { ctx.fpscr = raise_flags(ctx.fpscr, fe_of_code(static_cast<u32>(code))); }
void dk_Force_raise(PPUContext& ctx, s32 code) { ctx.fpscr = raise_flags(ctx.fpscr, fe_of_code(static_cast<u32>(code))); }

// ------------------------------------------------------------------------------------------------- Dinkumware

template <class T>
struct Bits;
template <>
struct Bits<double> {
    using U = u64;
    static constexpr int kMant = 52;          // fraction bits
    static constexpr U kExpMax = 0x7FF;       // the exponent of infinities and NaNs
    static constexpr s64 kLsb = 1023 + 52;    // the biased exponent's offset from the weight of the last bit
};
template <>
struct Bits<float> {
    using U = u32;
    static constexpr int kMant = 23;
    static constexpr U kExpMax = 0xFF;
    static constexpr s64 kLsb = 127 + 23;
};

template <class T>
s32 classify(T x)
{
    switch (std::fpclassify(x)) {
    case FP_NAN: return kNanCode;
    case FP_INFINITE: return kInfCode;
    case FP_ZERO: return kZero;
    case FP_SUBNORMAL: return kDenorm;
    default: return kFinite;
    }
}

// _Dint: drops the bits of x worth less than 2^-xexp (truncation towards zero to xexp fraction bits). Returns 0 when
// none was set (or x is zero), _FINITE when some were, the codes of infinities and NaNs (left alone).
template <class T>
s32 dint(T& x, s32 xexp)
{
    using B = Bits<T>;
    using U = typename B::U;
    const U b = std::bit_cast<U>(x);
    const U sign = U{1} << (8 * sizeof(U) - 1);
    const U frac_mask = (U{1} << B::kMant) - 1;
    s64 e = static_cast<s64>((b >> B::kMant) & B::kExpMax);
    if (static_cast<U>(e) == B::kExpMax) return (b & frac_mask) ? kNanCode : kInfCode;
    if ((b & ~sign) == 0) return kZero;
    if (e == 0) e = 1; // a denormal's last bit weighs as the smallest normal exponent's
    const s64 drop = B::kLsb - e - xexp;
    if (drop <= 0) return kZero;
    if (drop > B::kMant) {
        x = std::bit_cast<T>(static_cast<U>(b & sign));
        return kFinite;
    }
    const U mask = (U{1} << drop) - 1;
    x = std::bit_cast<T>(static_cast<U>(b & ~mask));
    return (b & mask) ? kFinite : kZero;
}

// _Dscale: x * 2^n with checking (round to nearest for a denormal result): _INFCODE on overflow (an infinity),
// 0 for a zero result, _FINITE otherwise; infinities and NaNs are left alone with their codes.
template <class T>
s32 dscale(T& x, s64 n)
{
    if (std::isnan(x)) return kNanCode;
    if (std::isinf(x)) return kInfCode;
    if (x == 0) return kZero;
    n = std::clamp<s64>(n, -100000, 100000);
    x = std::scalbn(x, static_cast<int>(n));
    if (std::isinf(x)) return kInfCode;
    if (x == 0) return kZero;
    return kFinite;
}

// _Exp: x = y * e^x * 2^eoff (Dinkumware's helper for exp, cosh, sinh and complex exponentials), returning _Dscale's
// code. HUGE_EXP (0.9 * the largest exponent field) bounds the arguments worth computing.
template <class T>
s32 exp_scaled(T& x, T y, s64 eoff)
{
    constexpr double kHugeExp = std::is_same_v<T, double> ? 1842.0 : 229.0;
    if (y == 0) {
        x = y;
        return kZero;
    }
    const double xd = static_cast<double>(x);
    if (std::isnan(xd)) {
        x = x + y;
        return kNanCode;
    }
    if (xd < -kHugeExp) {
        x = std::copysign(T{0}, y);
        return kZero;
    }
    if (kHugeExp < xd) {
        x = std::copysign(std::numeric_limits<T>::infinity(), y);
        return kInfCode;
    }
    // e^x = 2^n * e^r, |r| <= ln2/2 (Cody and Waite's split of ln 2); y = m * 2^k.
    constexpr double kLog2e = 1.44269504088896340736, kLn2Hi = 6.93147180369123816490e-01;
    constexpr double kLn2Lo = 1.90821492927058770002e-10;
    const double n = std::nearbyint(xd * kLog2e);
    const double r = (xd - n * kLn2Hi) - n * kLn2Lo;
    int k = 0;
    const double m = std::frexp(static_cast<double>(y), &k);
    double v = m * std::exp(r);
    const s32 code = dscale(v, static_cast<s64>(n) + k + eoff);
    x = static_cast<T>(v);
    if constexpr (std::is_same_v<T, float>) return classify(x) == kDenorm ? kFinite : classify(x);
    else return code;
}

// _Cosh: y * cosh(x); _Sinh: y * sinh(x) (Dinkumware's helpers for the hyperbolic and complex functions), through
// _Exp past _Xbig, so a product that is finite comes out finite even where cosh(x) alone overflows.
template <class T>
T cosh_scaled(T x, T y)
{
    if (std::isnan(x)) return x;
    if (x == 0 || y == 0) return y;
    if (std::isinf(x)) return y * std::numeric_limits<T>::infinity();
    const T ax = std::fabs(x);
    if (static_cast<double>(ax) < 18.0) return static_cast<T>(static_cast<double>(y) * std::cosh(static_cast<double>(ax)));
    T r = ax;
    exp_scaled(r, y, -1);
    return r;
}

template <class T>
T sinh_scaled(T x, T y)
{
    if (std::isnan(x)) return x;
    if (x == 0) return x * y;
    if (y == 0) return y;
    if (std::isinf(x)) return y * x;
    const T ax = std::fabs(x);
    T r;
    if (static_cast<double>(ax) < 18.0) {
        r = static_cast<T>(static_cast<double>(y) * std::sinh(static_cast<double>(ax)));
    } else {
        r = ax;
        exp_scaled(r, y, -1);
    }
    return std::signbit(x) ? -r : r;
}

// _Atan: atan(x) (0 <= x <= 1) placed by `hex`: bit 3 negates, bits 1-2 select pi/2 - t, pi/2 + t or pi - t (atan of
// a reciprocal, of a negative x in atan2), bit 0 adds pi/6 (the reduction _Atan itself makes).
template <class T>
T atan_placed(T x, s32 hex)
{
    constexpr double kPi = 3.14159265358979323846, kPi2 = 1.57079632679489661923, kPi6 = 0.52359877559829887308;
    constexpr double kBase[4] = {0.0, kPi2, kPi2, kPi};
    double t = std::atan(static_cast<double>(x));
    if (hex & 1) t += kPi6;
    if (hex & 2) t = -t;
    t += kBase[(hex >> 1) & 3];
    return static_cast<T>((hex & 8) ? -t : t);
}

// _Log: ln(x) (0), log10(x) (positive) or log2(x) (negative).
template <class T>
T log_of(T x, s32 base)
{
    if constexpr (std::is_same_v<T, float>) return base == 0 ? ::logf(x) : base > 0 ? ::log10f(x) : ::log2f(x);
    else return base == 0 ? ::log(x) : base > 0 ? ::log10(x) : ::log2(x);
}

// _Sin: sin(x + qoff * pi/2): 0 sin, 1 cos, 2 -sin, 3 -cos.
template <class T>
T sin_quadrant(T x, u32 qoff)
{
    T r;
    if constexpr (std::is_same_v<T, float>) r = (qoff & 1) ? ::cosf(x) : ::sinf(x);
    else r = (qoff & 1) ? ::cos(x) : ::sin(x);
    return (qoff & 2) ? -r : r;
}

// Reads and writes of the guest value a Dinkumware helper points at.
double load(u8* base, u32 addr, double) { return ps_readf64(base, addr); }
float load(u8* base, u32 addr, float) { return ps_readf32(base, addr); }
void store(u8* base, u32 addr, double v) { ps_writef64(base, addr, v); }
void store(u8* base, u32 addr, float v) { ps_writef32(base, addr, v); }

template <class T>
s32 dk_test(PPUContext& ctx, u32 px, std::string_view name)
{
    if (!guest_ok(ctx, px, sizeof(T))) {
        refuse(ctx, name, px);
        return kZero;
    }
    return classify(load(guest_base(ctx), px, T{}));
}

s32 dk_Dtest(PPUContext& ctx, u32 px) { return dk_test<double>(ctx, px, "_Dtest"); }
s32 dk_FDtest(PPUContext& ctx, u32 px) { return dk_test<float>(ctx, px, "_FDtest"); }
s32 dk_Dclass(PPUContext&, double x) { return classify(x); }
s32 dk_FDclass(PPUContext&, float x) { return classify(x); }

// _Dsign: the sign bit as Dinkumware's DSIGN macro has it (0x8000 of the top 16-bit word).
s32 dk_Dsign(PPUContext&, double x) { return std::signbit(x) ? 0x8000 : 0; }
s32 dk_FDsign(PPUContext&, float x) { return std::signbit(x) ? 0x8000 : 0; }

// _Fpcomp: _FP_LT 1, _FP_EQ 2, _FP_GT 4, 0 when unordered (isgreater and friends).
template <class T>
s32 fpcomp(T x, T y)
{
    return x < y ? 1 : x == y ? 2 : x > y ? 4 : 0;
}
s32 dk_Fpcomp(PPUContext&, double x, double y) { return fpcomp(x, y); }
s32 dk_FFpcomp(PPUContext&, float x, float y) { return fpcomp(x, y); }

template <class T>
s32 dk_int(PPUContext& ctx, u32 px, s32 xexp, std::string_view name)
{
    if (!guest_ok(ctx, px, sizeof(T))) {
        refuse(ctx, name, px);
        return kZero;
    }
    u8* base = guest_base(ctx);
    T x = load(base, px, T{});
    const s32 code = dint(x, xexp);
    store(base, px, x);
    return code;
}

s32 dk_Dint(PPUContext& ctx, u32 px, s32 xexp) { return dk_int<double>(ctx, px, xexp, "_Dint"); }
s32 dk_FDint(PPUContext& ctx, u32 px, s32 xexp) { return dk_int<float>(ctx, px, xexp, "_FDint"); }

// _Dnorm: normalises the fraction of a denormal (as 16-bit words, most significant first) so that its leading bit is
// the hidden one, which it then clears; the exponent field is left 0 and the sign kept. Returns the exponent field
// the value would have (1 for a zero fraction, less by the bits shifted).
template <class T>
s32 dk_norm(PPUContext& ctx, u32 ps, std::string_view name)
{
    using B = Bits<T>;
    using U = typename B::U;
    constexpr u32 kWords = sizeof(U) / 2;
    if (!guest_ok(ctx, ps, sizeof(U))) {
        refuse(ctx, name, ps);
        return kZero;
    }
    u8* base = guest_base(ctx);
    U b = 0;
    for (u32 i = 0; i < kWords; i++) b = static_cast<U>((b << 16) | ps_read16(base, ps + 2 * i));
    const U sign = b & (U{1} << (8 * sizeof(U) - 1));
    const U frac_mask = (U{1} << B::kMant) - 1;
    U frac = b & frac_mask;
    s32 xchar = 1;
    if (frac != 0) {
        const int shift = B::kMant - (8 * static_cast<int>(sizeof(U)) - 1 - std::countl_zero(frac));
        frac = static_cast<U>(frac << shift) & frac_mask;
        xchar -= shift;
    }
    b = sign | frac;
    for (u32 i = 0; i < kWords; i++)
        ps_write16(base, ps + 2 * i, static_cast<u16>(b >> (16 * (kWords - 1 - i))));
    return xchar;
}

s32 dk_Dnorm(PPUContext& ctx, u32 ps) { return dk_norm<double>(ctx, ps, "_Dnorm"); }
s32 dk_FDnorm(PPUContext& ctx, u32 ps) { return dk_norm<float>(ctx, ps, "_FDnorm"); }

template <class T>
s32 dk_scale(PPUContext& ctx, u32 px, s32 n, std::string_view name)
{
    if (!guest_ok(ctx, px, sizeof(T))) {
        refuse(ctx, name, px);
        return kZero;
    }
    u8* base = guest_base(ctx);
    T x = load(base, px, T{});
    const s32 code = libc::dk_scale(x, n); // the SDK's: a denormal result truncated (D-907)
    store(base, px, x);
    return code;
}

s32 dk_Dscale(PPUContext& ctx, u32 px, s32 n) { return dk_scale<double>(ctx, px, n, "_Dscale"); }
s32 dk_FDscale(PPUContext& ctx, u32 px, s32 n) { return dk_scale<float>(ctx, px, n, "_FDscale"); }

// _Dunscale: *px = its fraction in [1/2, 1) and *pex (a short) its exponent: frexp, with the codes; a zero, infinity
// or NaN is left alone with *pex = 0.
template <class T>
s32 dk_unscale(PPUContext& ctx, u32 pex, u32 px, std::string_view name)
{
    if (!guest_ok(ctx, pex, 2) || !guest_ok(ctx, px, sizeof(T))) {
        refuse(ctx, name, guest_ok(ctx, pex, 2) ? px : pex);
        return kZero;
    }
    u8* base = guest_base(ctx);
    const T x = load(base, px, T{});
    const s32 code = classify(x);
    if (code == kNanCode || code == kInfCode || code == kZero) {
        ps_write16(base, pex, 0);
        return code;
    }
    int e = 0;
    store(base, px, std::frexp(x, &e));
    ps_write16(base, pex, static_cast<u16>(static_cast<s16>(e)));
    return kFinite;
}

s32 dk_Dunscale(PPUContext& ctx, u32 pex, u32 px) { return dk_unscale<double>(ctx, pex, px, "_Dunscale"); }
s32 dk_FDunscale(PPUContext& ctx, u32 pex, u32 px) { return dk_unscale<float>(ctx, pex, px, "_FDunscale"); }

template <class T>
s32 dk_exp(PPUContext& ctx, u32 px, T y, s32 eoff, std::string_view name)
{
    if (!guest_ok(ctx, px, sizeof(T))) {
        refuse(ctx, name, px);
        return kZero;
    }
    u8* base = guest_base(ctx);
    T x = load(base, px, T{});
    const s32 code = exp_scaled(x, y, eoff);
    store(base, px, x);
    return code;
}

s32 dk_Exp(PPUContext& ctx, u32 px, double y, s32 eoff) { return dk_exp<double>(ctx, px, y, eoff, "_Exp"); }
s32 dk_FExp(PPUContext& ctx, u32 px, float y, s32 eoff) { return dk_exp<float>(ctx, px, y, eoff, "_FExp"); }

double dk_Cosh(PPUContext&, double x, double y) { return cosh_scaled(x, y); }
float dk_FCosh(PPUContext&, float x, float y) { return cosh_scaled(x, y); }
double dk_Sinh(PPUContext&, double x, double y) { return sinh_scaled(x, y); }
float dk_FSinh(PPUContext&, float x, float y) { return sinh_scaled(x, y); }
double dk_Atan(PPUContext&, double x, s32 hex) { return atan_placed(x, hex); }
float dk_FAtan(PPUContext&, float x, s32 hex) { return atan_placed(x, hex); }
double dk_Log(PPUContext&, double x, s32 base) { return log_of(x, base); }
float dk_FLog(PPUContext&, float x, s32 base) { return log_of(x, base); }
double dk_Sin(PPUContext&, double x, u32 qoff) { return sin_quadrant(x, qoff); }
float dk_FSin(PPUContext&, float x, u32 qoff) { return sin_quadrant(x, qoff); }
double dk_Erfc(PPUContext&, double x) { return ::erfc(x); }
float dk_FErfc(PPUContext&, float x) { return ::erfcf(x); }
double dk_Lgamma(PPUContext&, double x) { return ::lgamma(x); }
float dk_FLgamma(PPUContext&, float x) { return ::lgammaf(x); }
double dk_Tgamma(PPUContext&, double x) { return ::tgamma(x); }
float dk_FTgamma(PPUContext&, float x) { return ::tgammaf(x); }
// _Dtentox(x, n, perr) as the SDK's (libc_scan.hpp, D-907): errno ERANGE and *perr |= 1 for a zero or infinite result.
template <class T>
T dk_tentox(PPUContext& ctx, T x, s32 n, u32 perr)
{
    bool range = false;
    const T r = libc::dk_tento(x, n, &range);
    if (range) {
        libc::set_errno(ctx, libc::kERANGE);
        if (perr && guest_ok(ctx, perr, 4)) ps_write32(guest_base(ctx), perr, ps_read32(guest_base(ctx), perr) | 1);
    }
    return r;
}
double dk_Dtentox(PPUContext& ctx, double x, s32 n, u32 perr) { return dk_tentox(ctx, x, n, perr); }
float dk_FDtentox(PPUContext& ctx, float x, s32 n, u32 perr) { return dk_tentox(ctx, x, n, perr); }

// _Hypot: hypot(x, y) = result * 2^*pexp, so that the result stays finite (*pexp is 1 where hypot overflows).
template <class T>
T dk_hypot(PPUContext& ctx, T x, T y, u32 pexp, std::string_view name)
{
    s32 e = 0;
    T z = std::hypot(x, y);
    if (std::isinf(z) && std::isfinite(x) && std::isfinite(y)) {
        z = std::hypot(x * T{0.5}, y * T{0.5});
        e = 1;
    }
    if (!guest_ok(ctx, pexp, 4)) refuse(ctx, name, pexp);
    else ps_write32(guest_base(ctx), pexp, static_cast<u32>(e));
    return z;
}

double dk_Hypot(PPUContext& ctx, double x, double y, u32 pexp) { return dk_hypot(ctx, x, y, pexp, "_Hypot"); }
float dk_FHypot(PPUContext& ctx, float x, float y, u32 pexp) { return dk_hypot(ctx, x, y, pexp, "_FHypot"); }

// _Poly: tab[0] * x^n + tab[1] * x^(n-1) + ... + tab[n] (Horner, in the type's precision, no fused steps).
template <class T>
T dk_poly(PPUContext& ctx, T x, u32 tab, s32 n, std::string_view name)
{
    const u64 count = n < 0 ? 1 : u64(n) + 1;
    if (!guest_span(ctx, tab, count * sizeof(T))) {
        refuse(ctx, name, tab);
        return std::numeric_limits<T>::quiet_NaN();
    }
    u8* base = guest_base(ctx);
    T y = load(base, tab, T{});
    for (u64 i = 1; i < count; i++) y = y * x + load(base, tab + static_cast<u32>(i * sizeof(T)), T{});
    return y;
}

double dk_Poly(PPUContext& ctx, double x, u32 tab, s32 n) { return dk_poly(ctx, x, tab, n, "_Poly"); }
float dk_FPoly(PPUContext& ctx, float x, u32 tab, s32 n) { return dk_poly(ctx, x, tab, n, "_FPoly"); }

// Dinkumware's extended-precision helpers, whose signatures no reference available here shows: logged, not computed.
void unknown_signature(PPUContext& ctx, u8* base, std::string_view name)
{
    log_call_site(ctx, base, std::format("{}: Dinkumware's internal with no known signature; not computed (D-507)", name));
    ctx.gpr[3] = ~u64{0};
    ctx.fpr[1] = std::bit_cast<double>(kNanBits);
}

void dk_Eadd(PPUContext& ctx, u8* base) { unknown_signature(ctx, base, "_Eadd"); }
void dk_Emul(PPUContext& ctx, u8* base) { unknown_signature(ctx, base, "_Emul"); }
void dk_Esub(PPUContext& ctx, u8* base) { unknown_signature(ctx, base, "_Esub"); }
void dk_Etentox(PPUContext& ctx, u8* base) { unknown_signature(ctx, base, "_Etentox"); }

// ------------------------------------------------------------------------------------------------- complex.h

using CD = double _Complex;
using CF = float _Complex;
static_assert(sizeof(CD) == 2 * sizeof(double) && sizeof(CF) == 2 * sizeof(float));

struct Cd {
    double re, im;
};
struct Cf {
    float re, im;
};

Cd arg_cd(const PPUContext& ctx, int f) { return {ctx.fpr[f], ctx.fpr[f + 1]}; }
Cf arg_cf(const PPUContext& ctx, int f) { return {static_cast<float>(ctx.fpr[f]), static_cast<float>(ctx.fpr[f + 1])}; }

void ret(PPUContext& ctx, Cd z)
{
    ctx.fpr[1] = z.re;
    ctx.fpr[2] = z.im;
}
void ret(PPUContext& ctx, Cf z)
{
    ctx.fpr[1] = static_cast<double>(z.re);
    ctx.fpr[2] = static_cast<double>(z.im);
}

CD to_c(Cd z)
{
    CD c;
    std::memcpy(&c, &z, sizeof c);
    return c;
}
CF to_c(Cf z)
{
    CF c;
    std::memcpy(&c, &z, sizeof c);
    return c;
}
Cd from_c(CD c)
{
    Cd z;
    std::memcpy(&z, &c, sizeof z);
    return z;
}
Cf from_c(CF c)
{
    Cf z;
    std::memcpy(&z, &c, sizeof z);
    return z;
}

template <CD (*F)(CD)>
void cd_unary(PPUContext& ctx, u8*)
{
    ret(ctx, from_c(F(to_c(arg_cd(ctx, 1)))));
}
template <CF (*F)(CF)>
void cf_unary(PPUContext& ctx, u8*)
{
    ret(ctx, from_c(F(to_c(arg_cf(ctx, 1)))));
}

void c_cacos(PPUContext& ctx, u8* b) { cd_unary<&::cacos>(ctx, b); }
void c_cacosh(PPUContext& ctx, u8* b) { cd_unary<&::cacosh>(ctx, b); }
void c_casin(PPUContext& ctx, u8* b) { cd_unary<&::casin>(ctx, b); }
void c_casinh(PPUContext& ctx, u8* b) { cd_unary<&::casinh>(ctx, b); }
void c_catan(PPUContext& ctx, u8* b) { cd_unary<&::catan>(ctx, b); }
void c_catanh(PPUContext& ctx, u8* b) { cd_unary<&::catanh>(ctx, b); }
void c_ccos(PPUContext& ctx, u8* b) { cd_unary<&::ccos>(ctx, b); }
void c_ccosh(PPUContext& ctx, u8* b) { cd_unary<&::ccosh>(ctx, b); }
void c_cexp(PPUContext& ctx, u8* b) { cd_unary<&::cexp>(ctx, b); }
void c_clog(PPUContext& ctx, u8* b) { cd_unary<&::clog>(ctx, b); }
void c_cproj(PPUContext& ctx, u8* b) { cd_unary<&::cproj>(ctx, b); }
void c_csin(PPUContext& ctx, u8* b) { cd_unary<&::csin>(ctx, b); }
void c_csinh(PPUContext& ctx, u8* b) { cd_unary<&::csinh>(ctx, b); }
void c_csqrt(PPUContext& ctx, u8* b) { cd_unary<&::csqrt>(ctx, b); }
void c_ctan(PPUContext& ctx, u8* b) { cd_unary<&::ctan>(ctx, b); }
void c_ctanh(PPUContext& ctx, u8* b) { cd_unary<&::ctanh>(ctx, b); }
void c_cacosf(PPUContext& ctx, u8* b) { cf_unary<&::cacosf>(ctx, b); }
void c_cacoshf(PPUContext& ctx, u8* b) { cf_unary<&::cacoshf>(ctx, b); }
void c_casinf(PPUContext& ctx, u8* b) { cf_unary<&::casinf>(ctx, b); }
void c_casinhf(PPUContext& ctx, u8* b) { cf_unary<&::casinhf>(ctx, b); }
void c_catanf(PPUContext& ctx, u8* b) { cf_unary<&::catanf>(ctx, b); }
void c_catanhf(PPUContext& ctx, u8* b) { cf_unary<&::catanhf>(ctx, b); }
void c_ccosf(PPUContext& ctx, u8* b) { cf_unary<&::ccosf>(ctx, b); }
void c_ccoshf(PPUContext& ctx, u8* b) { cf_unary<&::ccoshf>(ctx, b); }
void c_cexpf(PPUContext& ctx, u8* b) { cf_unary<&::cexpf>(ctx, b); }
void c_clogf(PPUContext& ctx, u8* b) { cf_unary<&::clogf>(ctx, b); }
void c_cprojf(PPUContext& ctx, u8* b) { cf_unary<&::cprojf>(ctx, b); }
void c_csinf(PPUContext& ctx, u8* b) { cf_unary<&::csinf>(ctx, b); }
void c_csinhf(PPUContext& ctx, u8* b) { cf_unary<&::csinhf>(ctx, b); }
void c_csqrtf(PPUContext& ctx, u8* b) { cf_unary<&::csqrtf>(ctx, b); }
void c_ctanf(PPUContext& ctx, u8* b) { cf_unary<&::ctanf>(ctx, b); }
void c_ctanhf(PPUContext& ctx, u8* b) { cf_unary<&::ctanhf>(ctx, b); }

void c_cpow(PPUContext& ctx, u8*) { ret(ctx, from_c(::cpow(to_c(arg_cd(ctx, 1)), to_c(arg_cd(ctx, 3))))); }
void c_cpowf(PPUContext& ctx, u8*) { ret(ctx, from_c(::cpowf(to_c(arg_cf(ctx, 1)), to_c(arg_cf(ctx, 3))))); }

void c_conj(PPUContext& ctx, u8*) { ret(ctx, Cd{ctx.fpr[1], -ctx.fpr[2]}); }
void c_conjf(PPUContext& ctx, u8*)
{
    const Cf z = arg_cf(ctx, 1);
    ret(ctx, Cf{z.re, -z.im});
}

// clog10 (Dinkumware's): clog(z) / ln 10.
void c_clog10(PPUContext& ctx, u8*)
{
    constexpr double kLog10e = 0.43429448190325182765;
    const Cd l = from_c(::clog(to_c(arg_cd(ctx, 1))));
    ret(ctx, Cd{l.re * kLog10e, l.im * kLog10e});
}
void c_clog10f(PPUContext& ctx, u8*)
{
    constexpr float kLog10e = 0.43429448190325182765f;
    const Cf l = from_c(::clogf(to_c(arg_cf(ctx, 1))));
    ret(ctx, Cf{l.re * kLog10e, l.im * kLog10e});
}

double m_cabs(PPUContext&, double re, double im) { return ::hypot(re, im); }
float m_cabsf(PPUContext&, float re, float im) { return ::hypotf(re, im); }
double m_carg(PPUContext&, double re, double im) { return ::atan2(im, re); }
float m_cargf(PPUContext&, float re, float im) { return ::atan2f(im, re); }
double m_creal(PPUContext&, double re, double) { return re; }
float m_crealf(PPUContext&, float re, float) { return re; }
double m_cimag(PPUContext&, double, double im) { return im; }
float m_cimagf(PPUContext&, float, float im) { return im; }

// Multiplication and division with C99 Annex G's recovery of infinities (its G.5.1 examples _Cmultd and _Cdivd), as
// the compiler's own complex * and / give them.
template <class T>
void annex_g_mul(T a, T b, T c, T d, T& x, T& y)
{
    const T inf = std::numeric_limits<T>::infinity();
    const T ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    x = ac - bd;
    y = ad + bc;
    if (!(std::isnan(x) && std::isnan(y))) return;
    bool recalc = false;
    auto box = [](T v) { return std::copysign(std::isinf(v) ? T{1} : T{0}, v); };
    auto unnan = [](T v) { return std::isnan(v) ? std::copysign(T{0}, v) : v; };
    if (std::isinf(a) || std::isinf(b)) {
        a = box(a);
        b = box(b);
        c = unnan(c);
        d = unnan(d);
        recalc = true;
    }
    if (std::isinf(c) || std::isinf(d)) {
        c = box(c);
        d = box(d);
        a = unnan(a);
        b = unnan(b);
        recalc = true;
    }
    if (!recalc && (std::isinf(ac) || std::isinf(bd) || std::isinf(ad) || std::isinf(bc))) {
        a = unnan(a);
        b = unnan(b);
        c = unnan(c);
        d = unnan(d);
        recalc = true;
    }
    if (recalc) {
        x = inf * (a * c - b * d);
        y = inf * (a * d + b * c);
    }
}

template <class T>
void annex_g_div(T a, T b, T c, T d, T& x, T& y)
{
    const T inf = std::numeric_limits<T>::infinity();
    const T logbw = std::logb(std::fmax(std::fabs(c), std::fabs(d)));
    int ilogbw = 0;
    if (std::isfinite(logbw)) {
        ilogbw = static_cast<int>(logbw);
        c = std::scalbn(c, -ilogbw);
        d = std::scalbn(d, -ilogbw);
    }
    const T denom = c * c + d * d;
    x = std::scalbn((a * c + b * d) / denom, -ilogbw);
    y = std::scalbn((b * c - a * d) / denom, -ilogbw);
    if (!(std::isnan(x) && std::isnan(y))) return;
    if (denom == T{0} && (!std::isnan(a) || !std::isnan(b))) {
        x = std::copysign(inf, c) * a;
        y = std::copysign(inf, c) * b;
    } else if ((std::isinf(a) || std::isinf(b)) && std::isfinite(c) && std::isfinite(d)) {
        a = std::copysign(std::isinf(a) ? T{1} : T{0}, a);
        b = std::copysign(std::isinf(b) ? T{1} : T{0}, b);
        x = inf * (a * c + b * d);
        y = inf * (b * c - a * d);
    } else if (std::isinf(logbw) && std::isfinite(a) && std::isfinite(b)) {
        c = std::copysign(std::isinf(c) ? T{1} : T{0}, c);
        d = std::copysign(std::isinf(d) ? T{1} : T{0}, d);
        x = T{0} * (a * c + b * d);
        y = T{0} * (b * c - a * d);
    }
}

// Dinkumware's complex arithmetic for C without a complex type (_Cbuild, _Caddcc, ...): what the operators give.
template <class T>
using CT = std::conditional_t<std::is_same_v<T, double>, Cd, Cf>;

template <class T>
CT<T> arg_c(const PPUContext& ctx, int f)
{
    if constexpr (std::is_same_v<T, double>) return arg_cd(ctx, f);
    else return arg_cf(ctx, f);
}
template <class T>
T arg_r(const PPUContext& ctx, int f)
{
    return static_cast<T>(ctx.fpr[f]);
}

template <class T>
void cbuild(PPUContext& ctx, u8*)
{
    ret(ctx, CT<T>{arg_r<T>(ctx, 1), arg_r<T>(ctx, 2)});
}
template <class T>
void caddcc(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1), y = arg_c<T>(ctx, 3);
    ret(ctx, CT<T>{x.re + y.re, x.im + y.im});
}
template <class T>
void caddcr(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1);
    ret(ctx, CT<T>{x.re + arg_r<T>(ctx, 3), x.im});
}
template <class T>
void csubcc(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1), y = arg_c<T>(ctx, 3);
    ret(ctx, CT<T>{x.re - y.re, x.im - y.im});
}
template <class T>
void csubcr(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1);
    ret(ctx, CT<T>{x.re - arg_r<T>(ctx, 3), x.im});
}
template <class T>
void cmulcc(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1), y = arg_c<T>(ctx, 3);
    CT<T> z;
    annex_g_mul(x.re, x.im, y.re, y.im, z.re, z.im);
    ret(ctx, z);
}
template <class T>
void cmulcr(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1);
    const T r = arg_r<T>(ctx, 3);
    ret(ctx, CT<T>{x.re * r, x.im * r});
}
template <class T>
void cdivcc(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1), y = arg_c<T>(ctx, 3);
    CT<T> z;
    annex_g_div(x.re, x.im, y.re, y.im, z.re, z.im);
    ret(ctx, z);
}
template <class T>
void cdivcr(PPUContext& ctx, u8*)
{
    const auto x = arg_c<T>(ctx, 1);
    const T r = arg_r<T>(ctx, 3);
    ret(ctx, CT<T>{x.re / r, x.im / r});
}

void dk_Cbuild(PPUContext& ctx, u8* b) { cbuild<double>(ctx, b); }
void dk_Caddcc(PPUContext& ctx, u8* b) { caddcc<double>(ctx, b); }
void dk_Caddcr(PPUContext& ctx, u8* b) { caddcr<double>(ctx, b); }
void dk_Csubcc(PPUContext& ctx, u8* b) { csubcc<double>(ctx, b); }
void dk_Csubcr(PPUContext& ctx, u8* b) { csubcr<double>(ctx, b); }
void dk_Cmulcc(PPUContext& ctx, u8* b) { cmulcc<double>(ctx, b); }
void dk_Cmulcr(PPUContext& ctx, u8* b) { cmulcr<double>(ctx, b); }
void dk_Cdivcc(PPUContext& ctx, u8* b) { cdivcc<double>(ctx, b); }
void dk_Cdivcr(PPUContext& ctx, u8* b) { cdivcr<double>(ctx, b); }
void dk_FCbuild(PPUContext& ctx, u8* b) { cbuild<float>(ctx, b); }
void dk_FCaddcc(PPUContext& ctx, u8* b) { caddcc<float>(ctx, b); }
void dk_FCaddcr(PPUContext& ctx, u8* b) { caddcr<float>(ctx, b); }
void dk_FCsubcc(PPUContext& ctx, u8* b) { csubcc<float>(ctx, b); }
void dk_FCsubcr(PPUContext& ctx, u8* b) { csubcr<float>(ctx, b); }
void dk_FCmulcc(PPUContext& ctx, u8* b) { cmulcc<float>(ctx, b); }
void dk_FCmulcr(PPUContext& ctx, u8* b) { cmulcr<float>(ctx, b); }
void dk_FCdivcc(PPUContext& ctx, u8* b) { cdivcc<float>(ctx, b); }
void dk_FCdivcr(PPUContext& ctx, u8* b) { cdivcr<float>(ctx, b); }

// ------------------------------------------------------------------------------------------------- SIMD (vector float)

// VMX's NJ mode: a denormal becomes a zero of its sign.
float nj(float x, bool flush)
{
    const u32 b = std::bit_cast<u32>(x);
    return flush && (b & 0x7F800000u) == 0 ? std::bit_cast<float>(b & 0x80000000u) : x;
}

template <float (*F)(float)>
void vec_unary(PPUContext& ctx, u8*)
{
    const bool flush = ctx.vscr_nj != 0;
    ps_v128 r;
    for (int i = 0; i < 4; i++) r.f32[i] = nj(F(nj(ctx.vr[2].f32[i], flush)), flush);
    ctx.vr[2] = r;
}

template <float (*F)(float, float)>
void vec_binary(PPUContext& ctx, u8*)
{
    const bool flush = ctx.vscr_nj != 0;
    ps_v128 r;
    for (int i = 0; i < 4; i++) r.f32[i] = nj(F(nj(ctx.vr[2].f32[i], flush), nj(ctx.vr[3].f32[i], flush)), flush);
    ctx.vr[2] = r;
}

float l_acos(float x) { return ::acosf(x); }
float l_asin(float x) { return ::asinf(x); }
float l_atan(float x) { return ::atanf(x); }
float l_cbrt(float x) { return ::cbrtf(x); }
float l_ceil(float x) { return ::ceilf(x); }
float l_cos(float x) { return ::cosf(x); }
float l_exp2(float x) { return ::exp2f(x); }
float l_exp(float x) { return ::expf(x); }
float l_expm1(float x) { return ::expm1f(x); }
float l_floor(float x) { return ::floorf(x); }
float l_log10(float x) { return ::log10f(x); }
float l_log1p(float x) { return ::log1pf(x); }
float l_log2(float x) { return ::log2f(x); }
float l_logb(float x) { return ::logbf(x); }
float l_log(float x) { return ::logf(x); }
float l_recip(float x) { return 1.0f / x; }
float l_rsqrt(float x) { return 1.0f / ::sqrtf(x); }
float l_sin(float x) { return ::sinf(x); }
float l_sqrt(float x) { return ::sqrtf(x); }
float l_tan(float x) { return ::tanf(x); }
float l_trunc(float x) { return ::truncf(x); }
float l_atan2(float y, float x) { return ::atan2f(y, x); }
float l_div(float x, float y) { return x / y; }
float l_fdim(float x, float y) { return ::fdimf(x, y); }
float l_fmax(float x, float y) { return ::fmaxf(x, y); }
float l_fmin(float x, float y) { return ::fminf(x, y); }
float l_fmod(float x, float y) { return ::fmodf(x, y); }
float l_hypot(float x, float y) { return ::hypotf(x, y); }
float l_pow(float x, float y) { return ::powf(x, y); }
float l_remainder(float x, float y) { return ::remainderf(x, y); }

void v_acosf4(PPUContext& ctx, u8* b) { vec_unary<&l_acos>(ctx, b); }
void v_asinf4(PPUContext& ctx, u8* b) { vec_unary<&l_asin>(ctx, b); }
void v_atanf4(PPUContext& ctx, u8* b) { vec_unary<&l_atan>(ctx, b); }
void v_cbrtf4(PPUContext& ctx, u8* b) { vec_unary<&l_cbrt>(ctx, b); }
void v_ceilf4(PPUContext& ctx, u8* b) { vec_unary<&l_ceil>(ctx, b); }
void v_cosf4(PPUContext& ctx, u8* b) { vec_unary<&l_cos>(ctx, b); }
void v_exp2f4(PPUContext& ctx, u8* b) { vec_unary<&l_exp2>(ctx, b); }
void v_expf4(PPUContext& ctx, u8* b) { vec_unary<&l_exp>(ctx, b); }
void v_expm1f4(PPUContext& ctx, u8* b) { vec_unary<&l_expm1>(ctx, b); }
void v_floorf4(PPUContext& ctx, u8* b) { vec_unary<&l_floor>(ctx, b); }
void v_log10f4(PPUContext& ctx, u8* b) { vec_unary<&l_log10>(ctx, b); }
void v_log1pf4(PPUContext& ctx, u8* b) { vec_unary<&l_log1p>(ctx, b); }
void v_log2f4(PPUContext& ctx, u8* b) { vec_unary<&l_log2>(ctx, b); }
void v_logbf4(PPUContext& ctx, u8* b) { vec_unary<&l_logb>(ctx, b); }
void v_logf4(PPUContext& ctx, u8* b) { vec_unary<&l_log>(ctx, b); }
void v_recipf4(PPUContext& ctx, u8* b) { vec_unary<&l_recip>(ctx, b); }
void v_rsqrtf4(PPUContext& ctx, u8* b) { vec_unary<&l_rsqrt>(ctx, b); }
void v_sinf4(PPUContext& ctx, u8* b) { vec_unary<&l_sin>(ctx, b); }
void v_sqrtf4(PPUContext& ctx, u8* b) { vec_unary<&l_sqrt>(ctx, b); }
void v_tanf4(PPUContext& ctx, u8* b) { vec_unary<&l_tan>(ctx, b); }
void v_truncf4(PPUContext& ctx, u8* b) { vec_unary<&l_trunc>(ctx, b); }
void v_atan2f4(PPUContext& ctx, u8* b) { vec_binary<&l_atan2>(ctx, b); }
void v_divf4(PPUContext& ctx, u8* b) { vec_binary<&l_div>(ctx, b); }
void v_fdimf4(PPUContext& ctx, u8* b) { vec_binary<&l_fdim>(ctx, b); }
void v_fmaxf4(PPUContext& ctx, u8* b) { vec_binary<&l_fmax>(ctx, b); }
void v_fminf4(PPUContext& ctx, u8* b) { vec_binary<&l_fmin>(ctx, b); }
void v_fmodf4(PPUContext& ctx, u8* b) { vec_binary<&l_fmod>(ctx, b); }
void v_hypotf4(PPUContext& ctx, u8* b) { vec_binary<&l_hypot>(ctx, b); }
void v_powf4(PPUContext& ctx, u8* b) { vec_binary<&l_pow>(ctx, b); }
void v_remainderf4(PPUContext& ctx, u8* b) { vec_binary<&l_remainder>(ctx, b); }

void v_fmaf4(PPUContext& ctx, u8*)
{
    const bool flush = ctx.vscr_nj != 0;
    ps_v128 r;
    for (int i = 0; i < 4; i++)
        r.f32[i] = nj(::fmaf(nj(ctx.vr[2].f32[i], flush), nj(ctx.vr[3].f32[i], flush), nj(ctx.vr[4].f32[i], flush)),
                      flush);
    ctx.vr[2] = r;
}

// The sign-bit functions: bit operations, no flush.
void v_fabsf4(PPUContext& ctx, u8*)
{
    for (u32& w : ctx.vr[2].u32) w &= 0x7FFFFFFFu;
}
void v_negatef4(PPUContext& ctx, u8*)
{
    for (u32& w : ctx.vr[2].u32) w ^= 0x80000000u;
}
void v_copysignf4(PPUContext& ctx, u8*)
{
    for (int i = 0; i < 4; i++) ctx.vr[2].u32[i] = (ctx.vr[2].u32[i] & 0x7FFFFFFFu) | (ctx.vr[3].u32[i] & 0x80000000u);
}

void v_ilogbf4(PPUContext& ctx, u8*)
{
    const bool flush = ctx.vscr_nj != 0;
    for (u32& w : ctx.vr[2].u32) w = static_cast<u32>(ilogb_of(nj(std::bit_cast<float>(w), flush)));
}

void v_ldexpf4(PPUContext& ctx, u8*)
{
    const bool flush = ctx.vscr_nj != 0;
    ps_v128 r;
    for (int i = 0; i < 4; i++)
        r.f32[i] = nj(::scalbnf(nj(ctx.vr[2].f32[i], flush), static_cast<s32>(ctx.vr[3].u32[i])), flush);
    ctx.vr[2] = r;
}

// frexpf4(x, vector signed int* e): the pointer follows the vector's two GPR slots, in r5.
void v_frexpf4(PPUContext& ctx, u8* base)
{
    const bool flush = ctx.vscr_nj != 0;
    const u32 e_ptr = static_cast<u32>(ctx.gpr[5]);
    ps_v128 m, e;
    for (int i = 0; i < 4; i++) {
        const float x = nj(ctx.vr[2].f32[i], flush);
        int k = 0;
        m.f32[i] = std::frexp(x, &k);
        e.u32[i] = std::isfinite(x) ? static_cast<u32>(k) : 0;
    }
    if (!guest_ok(ctx, e_ptr & ~15u, 16)) refuse(ctx, "frexpf4", e_ptr);
    else store_vector(base, e_ptr, e);
    ctx.vr[2] = m;
}

// modff4(x, vector float* i): the fraction, the integral parts through r5.
void v_modff4(PPUContext& ctx, u8* base)
{
    const bool flush = ctx.vscr_nj != 0;
    const u32 i_ptr = static_cast<u32>(ctx.gpr[5]);
    ps_v128 f, ip;
    for (int i = 0; i < 4; i++) {
        float whole = 0;
        f.f32[i] = std::modf(nj(ctx.vr[2].f32[i], flush), &whole);
        ip.f32[i] = whole;
    }
    if (!guest_ok(ctx, i_ptr & ~15u, 16)) refuse(ctx, "modff4", i_ptr);
    else store_vector(base, i_ptr, ip);
    ctx.vr[2] = f;
}

// sincosf4(x, vector float* s, vector float* c): r5 and r6.
void v_sincosf4(PPUContext& ctx, u8* base)
{
    const bool flush = ctx.vscr_nj != 0;
    const u32 s_ptr = static_cast<u32>(ctx.gpr[5]), c_ptr = static_cast<u32>(ctx.gpr[6]);
    ps_v128 s, c;
    for (int i = 0; i < 4; i++) {
        const float x = nj(ctx.vr[2].f32[i], flush);
        s.f32[i] = nj(::sinf(x), flush);
        c.f32[i] = nj(::cosf(x), flush);
    }
    if (!guest_ok(ctx, s_ptr & ~15u, 16)) refuse(ctx, "sincosf4", s_ptr);
    else if (!guest_ok(ctx, c_ptr & ~15u, 16)) refuse(ctx, "sincosf4", c_ptr);
    else {
        store_vector(base, s_ptr, s);
        store_vector(base, c_ptr, c);
    }
}

void v_absi4(PPUContext& ctx, u8*)
{
    for (u32& w : ctx.vr[2].u32) w = static_cast<s32>(w) < 0 ? 0u - w : w;
}

// divi4(numer, denom) -> divi4_t {quot, rem}, returned through the pointer in r3 (an aggregate). Truncating division;
// a zero divisor gives quotient 0 and remainder the numerator (the SIMD math library's), INT_MIN / -1 wraps.
void v_divi4(PPUContext& ctx, u8* base)
{
    const u32 out = static_cast<u32>(ctx.gpr[3]) & ~15u;
    ps_v128 q, r;
    for (int i = 0; i < 4; i++) {
        const s32 n = static_cast<s32>(ctx.vr[2].u32[i]), d = static_cast<s32>(ctx.vr[3].u32[i]);
        if (d == 0) {
            q.u32[i] = 0;
            r.u32[i] = static_cast<u32>(n);
        } else if (d == -1) {
            q.u32[i] = 0u - static_cast<u32>(n);
            r.u32[i] = 0;
        } else {
            q.u32[i] = static_cast<u32>(n / d);
            r.u32[i] = static_cast<u32>(n % d);
        }
    }
    if (!guest_ok(ctx, out, 32)) {
        refuse(ctx, "divi4", static_cast<u32>(ctx.gpr[3]));
        return;
    }
    store_vector(base, out, q);
    store_vector(base, out + 16, r);
}

// ------------------------------------------------------------------------------------------------- variables

// Dinkumware's constants as its headers declare them, in the PPU's big-endian layout (long double = double):
// - _Dconst (_Inf, _Nan, _Snan, _Denorm, _Hugeval, _Eps, _Rteps and the F/L forms): a union of unsigned short[8],
//   float, double and long double, 16 bytes; the value is at offset 0 in its type (float for the F forms).
// - _Xbig, _Zero: double (float for the F forms). _Xbig is (bits + 1) * 347 / 1000 in integers, where cosh and
//   sinh stop adding e^-x: 18, and 8 for float. _Eps is 2^-54 (2^-25), _Rteps its square root, 2^-27 (2^-12).
// - _Dbl, _Flt, _Ldbl (float.h's _Dvals): int _Ddig, _Dmdig, _Dmax10e, _Dmaxe, _Dmin10e, _Dmine, then the epsilon, the
//   largest and the smallest normal value as _Dconst unions at offsets 24, 40 and 56 (72 bytes).
// - sys_libm's: the bounds of erf's and tgamma's ranges (pspocket's values, in a _Dconst's 16 bytes): _Erf_one is the
//   smallest x whose erf rounds to 1, _Erf_small 2^-27 (2^-13), below which erf(x) is 2x/sqrt(pi) to the last bit,
//   _Gamma_big the smallest x whose tgamma overflows; _Fenv0, the default environment FE_DFL_ENV points at: FPSCR 0.
void put_d(u8* base, u32 addr, u64 bits) { ps_write64(base, addr, bits); }
void put_f(u8* base, u32 addr, u32 bits) { ps_write32(base, addr, bits); }

void v_inf(u8* b, u32 a) { put_d(b, a, 0x7FF0000000000000ull); }
void v_nan(u8* b, u32 a) { put_d(b, a, kNanBits); }
void v_snan(u8* b, u32 a) { put_d(b, a, 0x7FF0000000000001ull); }
void v_denorm(u8* b, u32 a) { put_d(b, a, 0x0000000000000001ull); }
void v_eps(u8* b, u32 a) { put_d(b, a, std::bit_cast<u64>(0x1p-54)); }
void v_rteps(u8* b, u32 a) { put_d(b, a, std::bit_cast<u64>(0x1p-27)); }
void v_xbig(u8* b, u32 a) { put_d(b, a, std::bit_cast<u64>(18.0)); }
void v_finf(u8* b, u32 a) { put_f(b, a, 0x7F800000u); }
void v_fnan(u8* b, u32 a) { put_f(b, a, kFNanBits); }
void v_fsnan(u8* b, u32 a) { put_f(b, a, 0x7F800001u); }
void v_fdenorm(u8* b, u32 a) { put_f(b, a, 0x00000001u); }
void v_feps(u8* b, u32 a) { put_f(b, a, std::bit_cast<u32>(0x1p-25f)); }
void v_frteps(u8* b, u32 a) { put_f(b, a, std::bit_cast<u32>(0x1p-12f)); }
void v_fxbig(u8* b, u32 a) { put_f(b, a, std::bit_cast<u32>(8.0f)); }
void v_erf_one(u8* b, u32 a) { put_d(b, a, std::bit_cast<u64>(0x1.7afb48dc96627p+2)); }
void v_erf_small(u8* b, u32 a) { put_d(b, a, std::bit_cast<u64>(0x1p-27)); }
void v_gamma_big(u8* b, u32 a) { put_d(b, a, std::bit_cast<u64>(0x1.573fae561f648p+7)); }
void v_ferf_one(u8* b, u32 a) { put_f(b, a, std::bit_cast<u32>(0x1.f5a88ap+1f)); }
void v_ferf_small(u8* b, u32 a) { put_f(b, a, std::bit_cast<u32>(0x1p-13f)); }
void v_fgamma_big(u8* b, u32 a) { put_f(b, a, std::bit_cast<u32>(0x1.18522p+5f)); }

void dvals(u8* b, u32 a, const s32 (&ints)[6])
{
    for (u32 i = 0; i < 6; i++) ps_write32(b, a + 4 * i, static_cast<u32>(ints[i]));
}
void v_dbl(u8* b, u32 a)
{
    dvals(b, a, {15, 53, 308, 1024, -307, -1021});
    put_d(b, a + 24, std::bit_cast<u64>(std::numeric_limits<double>::epsilon()));
    put_d(b, a + 40, std::bit_cast<u64>(std::numeric_limits<double>::max()));
    put_d(b, a + 56, std::bit_cast<u64>(std::numeric_limits<double>::min()));
}
void v_flt(u8* b, u32 a)
{
    dvals(b, a, {6, 24, 38, 128, -37, -125});
    put_f(b, a + 24, std::bit_cast<u32>(std::numeric_limits<float>::epsilon()));
    put_f(b, a + 40, std::bit_cast<u32>(std::numeric_limits<float>::max()));
    put_f(b, a + 56, std::bit_cast<u32>(std::numeric_limits<float>::min()));
}

// Every function, under sys_libm and sys_libc alike (nids.txt lists each of them in both).
void add_functions(Registry& r, std::string_view module)
{
#define FN(nid, name, impl) Function{{}, #name, nid, &thunk<&impl>, false},
#define RAW(nid, name, impl) Function{{}, #name, nid, &impl, false},
    r.add(module, {
        FN(0xC7369FCE, _Atan, dk_Atan)
        RAW(0x4AB22A63, _Caddcc, dk_Caddcc)
        RAW(0x266D2473, _Caddcr, dk_Caddcr)
        RAW(0xA8D180E8, _Cbuild, dk_Cbuild)
        RAW(0xEAC62795, _Cdivcc, dk_Cdivcc)
        RAW(0xFA765D42, _Cdivcr, dk_Cdivcr)
        RAW(0xCAC167A5, _Cmulcc, dk_Cmulcc)
        RAW(0x1498A072, _Cmulcr, dk_Cmulcr)
        FN(0x7D6191D0, _Cosh, dk_Cosh)
        RAW(0x315673F6, _Csubcc, dk_Csubcc)
        RAW(0xEE0DB701, _Csubcr, dk_Csubcr)
        FN(0x3EEEDB0E, _Dclass, dk_Dclass)
        FN(0xAD17E787, _Dint, dk_Dint)
        FN(0x18F7B77D, _Dnorm, dk_Dnorm)
        FN(0x5F922A30, _Dscale, dk_Dscale)
        FN(0xEE303936, _Dsign, dk_Dsign)
        FN(0xACE90BE4, _Dtentox, dk_Dtentox)
        FN(0xB94B9D13, _Dtest, dk_Dtest)
        FN(0x889D5804, _Dunscale, dk_Dunscale)
        RAW(0x23D3BCA7, _Eadd, dk_Eadd)
        RAW(0x8E2484F1, _Emul, dk_Emul)
        FN(0x0CF9B8BD, _Erfc, dk_Erfc)
        RAW(0x2EEA9F25, _Esub, dk_Esub)
        RAW(0x89B62F56, _Etentox, dk_Etentox)
        FN(0xDFD41734, _Exp, dk_Exp)
        FN(0xF16568AF, _FAtan, dk_FAtan)
        RAW(0x5BFD37BE, _FCaddcc, dk_FCaddcc)
        RAW(0xD70DF92A, _FCaddcr, dk_FCaddcr)
        RAW(0x01ECEF7D, _FCbuild, dk_FCbuild)
        RAW(0x70F71871, _FCdivcc, dk_FCdivcc)
        RAW(0x33F27F25, _FCdivcr, dk_FCdivcr)
        RAW(0x63BBDFA6, _FCmulcc, dk_FCmulcc)
        RAW(0x241F9337, _FCmulcr, dk_FCmulcr)
        FN(0x2E69BB2A, _FCosh, dk_FCosh)
        RAW(0x95DFECB1, _FCsubcc, dk_FCsubcc)
        RAW(0x6EF6B083, _FCsubcr, dk_FCsubcr)
        FN(0x007854F4, _FDclass, dk_FDclass)
        FN(0x48D462A9, _FDint, dk_FDint)
        FN(0x9886810C, _FDnorm, dk_FDnorm)
        FN(0xD4912EE3, _FDscale, dk_FDscale)
        FN(0xA8C16038, _FDsign, dk_FDsign)
        FN(0x7994C28D, _FDtentox, dk_FDtentox)
        FN(0x9232BAEA, _FDtest, dk_FDtest)
        FN(0x296BC72F, _FDunscale, dk_FDunscale)
        FN(0xFBE88922, _FErfc, dk_FErfc)
        FN(0x05F1DC9E, _FExp, dk_FExp)
        FN(0xDA31FC5D, _FFpcomp, dk_FFpcomp)
        FN(0x9616E336, _FHypot, dk_FHypot)
        FN(0xAD5D3E57, _FLgamma, dk_FLgamma)
        FN(0x71293B71, _FLog, dk_FLog)
        FN(0x6FCC1E27, _FPoly, dk_FPoly)
        FN(0xB5E28191, _FSin, dk_FSin)
        FN(0xCCC66F11, _FSinh, dk_FSinh)
        FN(0x418036E3, _FTgamma, dk_FTgamma)
        FN(0x003395D9, _Feraise, dk_Feraise)
        FN(0xE5D2293F, _Force_raise, dk_Force_raise)
        FN(0x790C53BD, _Fpcomp, dk_Fpcomp)
        FN(0xD28EF6DD, _Hypot, dk_Hypot)
        FN(0x89D1D168, _LAtan, dk_Atan)
        RAW(0xD5D38552, _LCaddcc, dk_Caddcc)
        RAW(0x0D86295D, _LCaddcr, dk_Caddcr)
        RAW(0x07F400E3, _LCbuild, dk_Cbuild)
        RAW(0x1BE996CC, _LCdivcc, dk_Cdivcc)
        RAW(0xAAA270DC, _LCdivcr, dk_Cdivcr)
        RAW(0xFFFE79BF, _LCmulcc, dk_Cmulcc)
        RAW(0xE9AC8223, _LCmulcr, dk_Cmulcr)
        FN(0xAD3A093D, _LCosh, dk_Cosh)
        RAW(0x0F721A9D, _LCsubcc, dk_Csubcc)
        RAW(0x81DAF880, _LCsubcr, dk_Csubcr)
        FN(0x6B6AB2A9, _LDclass, dk_Dclass)
        FN(0xB412A8DC, _LDint, dk_Dint)
        FN(0x5751ACF9, _LDscale, dk_Dscale)
        FN(0x33E5929B, _LDsign, dk_Dsign)
        FN(0xB5D353E8, _LDtentox, dk_Dtentox)
        FN(0x3C616743, _LDtest, dk_Dtest)
        FN(0x60627FB3, _LDunscale, dk_Dunscale)
        FN(0x5EE37927, _LErfc, dk_Erfc)
        FN(0x4111B546, _LExp, dk_Exp)
        FN(0xC7B45A19, _LFpcomp, dk_Fpcomp)
        FN(0x947AE18E, _LHypot, dk_Hypot)
        FN(0xB348C5C2, _LLgamma, dk_Lgamma)
        FN(0x1D35BFE4, _LLog, dk_Log)
        FN(0xE584836C, _LPoly, dk_Poly)
        FN(0xA2C81938, _LSin, dk_Sin)
        FN(0x1BCDEB47, _LSinh, dk_Sinh)
        FN(0x8BD1DEB2, _LTgamma, dk_Tgamma)
        FN(0xAFCFDAD7, _Lgamma, dk_Lgamma)
        FN(0xCA463458, _Log, dk_Log)
        FN(0x3F701E78, _Poly, dk_Poly)
        FN(0x742F12B4, _Sin, dk_Sin)
        FN(0xD612FA16, _Sinh, dk_Sinh)
        FN(0xB4F4513E, _Tgamma, dk_Tgamma)
        RAW(0xA0AB76D5, _absi4, v_absi4)
        RAW(0x9FDED78A, _acosf4, v_acosf4)
        RAW(0x6B660894, _acosf4fast, v_acosf4)
        RAW(0x0E53319F, _asinf4, v_asinf4)
        RAW(0x14208B00, _asinf4fast, v_asinf4)
        RAW(0xC90F4BBC, _atan2f4, v_atan2f4)
        RAW(0xEC7DA0C8, _atan2f4fast, v_atan2f4)
        RAW(0x76E639EC, _atanf4, v_atanf4)
        RAW(0x70357B12, _atanf4fast, v_atanf4)
        RAW(0x3EC9DE23, _cbrtf4, v_cbrtf4)
        RAW(0xF3BD7D08, _cbrtf4fast, v_cbrtf4)
        RAW(0x3D901A10, _ceilf4, v_ceilf4)
        RAW(0xA0160C30, _copysignf4, v_copysignf4)
        RAW(0x0BB036A6, _cosf4, v_cosf4)
        RAW(0xCB6A147E, _cosf4fast, v_cosf4)
        RAW(0x9245E01B, _divf4, v_divf4)
        RAW(0x71F2BC56, _divf4fast, v_divf4)
        RAW(0xCC5E0C72, _divi4, v_divi4)
        RAW(0x22C3E308, _exp2f4, v_exp2f4)
        RAW(0x4875601D, _exp2f4fast, v_exp2f4)
        RAW(0xB8AA984E, _expf4, v_expf4)
        RAW(0xE3E379B8, _expf4fast, v_expf4)
        RAW(0x60E9FF3C, _expm1f4, v_expm1f4)
        RAW(0x398483AA, _expm1f4fast, v_expm1f4)
        FN(0x9E289062, _f_ceilf, m_ceilf)
        FN(0xFAE9E727, _f_copysignf, m_copysignf)
        FN(0x734CA589, _f_cosf, m_cosf)
        FN(0x8D5858DB, _f_exp2f, m_exp2f)
        FN(0x03593D2C, _f_expf, m_expf)
        FN(0x3CB818FA, _f_fdimf, m_fdimf)
        FN(0x2DF339BC, _f_floorf, m_floorf)
        FN(0xE92F3FB8, _f_fmaf, m_fmaf)
        FN(0xF7844153, _f_fmaxf, m_fmaxf)
        FN(0x8C85369B, _f_fminf, m_fminf)
        FN(0x02E68D44, _f_fmodf, m_fmodf)
        FN(0x414C5ECC, _f_hypotf, m_hypotf)
        FN(0x48157605, _f_llrintf, m_llrintf)
        FN(0xBFDA6837, _f_log10f, m_log10f)
        FN(0xDC151707, _f_log2f, m_log2f)
        FN(0xEED82401, _f_logf, m_logf)
        FN(0x358D7F93, _f_lrintf, m_lrintf)
        FN(0x7919F414, _f_nearbyintf, m_nearbyintf)
        FN(0x41D1B236, _f_rintf, m_rintf)
        FN(0x90F0242F, _f_sinf, m_sinf)
        FN(0xEC43B983, _f_sqrtf, m_sqrtf)
        RAW(0x40A2E212, _fabsf4, v_fabsf4)
        RAW(0x054AAE63, _fdimf4, v_fdimf4)
        RAW(0x1C11885D, _floorf4, v_floorf4)
        RAW(0xD76A16DA, _fmaf4, v_fmaf4)
        RAW(0x08139BD2, _fmaxf4, v_fmaxf4)
        RAW(0x012D0A91, _fminf4, v_fminf4)
        RAW(0xFCEDABC3, _fmodf4, v_fmodf4)
        RAW(0x642E3D18, _frexpf4, v_frexpf4)
        RAW(0xD0FD3CA8, _hypotf4, v_hypotf4)
        RAW(0xE31CC0D3, _ilogbf4, v_ilogbf4)
        RAW(0xC9C536CE, _ldexpf4, v_ldexpf4)
        RAW(0x6261C0B5, _log10f4, v_log10f4)
        RAW(0xCBDF9AFB, _log10f4fast, v_log10f4)
        RAW(0x842CB14D, _log1pf4, v_log1pf4)
        RAW(0x405F9727, _log1pf4fast, v_log1pf4)
        RAW(0xFE23DBE9, _log2f4, v_log2f4)
        RAW(0xDE7833F2, _log2f4fast, v_log2f4)
        RAW(0x72F1F64B, _logbf4, v_logbf4)
        RAW(0x8F2BCDB5, _logf4, v_logf4)
        RAW(0xC0BCF25E, _logf4fast, v_logf4)
        RAW(0x1D5BF5D0, _modff4, v_modff4)
        RAW(0x29685118, _negatef4, v_negatef4)
        RAW(0x12DE4E46, _powf4, v_powf4)
        RAW(0xE5A0BE9F, _powf4fast, v_powf4)
        RAW(0x0522D1AF, _recipf4, v_recipf4)
        RAW(0x17CD5D87, _recipf4fast, v_recipf4)
        RAW(0xCFEE82D8, _remainderf4, v_remainderf4)
        RAW(0x7A893AF1, _rsqrtf4, v_rsqrtf4)
        RAW(0xB89863BC, _rsqrtf4fast, v_rsqrtf4)
        RAW(0x6AD1C42B, _sincosf4, v_sincosf4)
        RAW(0xB5961D4E, _sincosf4fast, v_sincosf4)
        RAW(0x016556DF, _sinf4, v_sinf4)
        RAW(0x1E9FD6BA, _sinf4fast, v_sinf4)
        RAW(0x76AFAF04, _sqrtf4, v_sqrtf4)
        RAW(0x8FB7BAC7, _sqrtf4fast, v_sqrtf4)
        RAW(0xC9481758, _tanf4, v_tanf4)
        RAW(0x938FB946, _tanf4fast, v_tanf4)
        RAW(0xF537D837, _truncf4, v_truncf4)
        RAW(0x14052AE0, absi4, v_absi4)
        FN(0x1313A420, acos, m_acos)
        FN(0xDECE76A6, acosf, m_acosf)
        RAW(0x1ACB2B16, acosf4, v_acosf4)
        RAW(0xE8FCF1F8, acosf4fast, v_acosf4)
        FN(0x78E4590A, acosh, m_acosh)
        FN(0x6C6285C6, acoshf, m_acoshf)
        FN(0xA5D0B260, acoshl, m_acosh)
        FN(0x5516D621, acosl, m_acos)
        FN(0x05EFC660, asin, m_asin)
        FN(0x411434BB, asinf, m_asinf)
        RAW(0x010818FC, asinf4, v_asinf4)
        RAW(0x91CDFDB0, asinf4fast, v_asinf4)
        FN(0x26EF50ED, asinh, m_asinh)
        FN(0xBD8BB75C, asinhf, m_asinhf)
        FN(0x0829A21D, asinhl, m_asinh)
        FN(0x575E9B6E, asinl, m_asin)
        FN(0x7F579E03, atan, m_atan)
        FN(0x31DB8C89, atan2, m_atan2)
        FN(0xFB932A56, atan2f, m_atan2f)
        RAW(0xD8C4096D, atan2f4, v_atan2f4)
        RAW(0xDD8660D2, atan2f4fast, v_atan2f4)
        FN(0x3D4EFAFB, atan2l, m_atan2)
        FN(0x3C057FBD, atanf, m_atanf)
        RAW(0x347C1EE1, atanf4, v_atanf4)
        RAW(0x178D98DD, atanf4fast, v_atanf4)
        FN(0x596AB55C, atanh, m_atanh)
        FN(0xE1288C47, atanhf, m_atanhf)
        FN(0xA46A70A1, atanhl, m_atanh)
        FN(0xDA217D1F, atanl, m_atan)
        FN(0xA92BCC85, cabs, m_cabs)
        FN(0x812ED488, cabsf, m_cabsf)
        FN(0x43D522F4, cabsl, m_cabs)
        RAW(0x8E258FA0, cacos, c_cacos)
        RAW(0x434881A0, cacosf, c_cacosf)
        RAW(0x970A3432, cacosh, c_cacosh)
        RAW(0x8E01379E, cacoshf, c_cacoshf)
        RAW(0xFDEC16E1, cacoshl, c_cacosh)
        RAW(0x2A4DCBAD, cacosl, c_cacos)
        FN(0x7048396E, carg, m_carg)
        FN(0x501C412F, cargf, m_cargf)
        FN(0x904E646B, cargl, m_carg)
        RAW(0x9AF30EAF, casin, c_casin)
        RAW(0x1225DD31, casinf, c_casinf)
        RAW(0x6CC4BD13, casinh, c_casinh)
        RAW(0x68A8957F, casinhf, c_casinhf)
        RAW(0x5B474C22, casinhl, c_casinh)
        RAW(0xDFFB4E3C, casinl, c_casin)
        RAW(0xA56557B6, catan, c_catan)
        RAW(0x99A6C261, catanf, c_catanf)
        RAW(0x5EE10A95, catanh, c_catanh)
        RAW(0x0B3F4E90, catanhf, c_catanhf)
        RAW(0x89B507B3, catanhl, c_catanh)
        RAW(0x61250988, catanl, c_catan)
        FN(0x4020F5EF, cbrt, m_cbrt)
        FN(0x453F9E91, cbrtf, m_cbrtf)
        RAW(0xC406DD09, cbrtf4, v_cbrtf4)
        RAW(0xBB208B20, cbrtf4fast, v_cbrtf4)
        FN(0xC94FCC63, cbrtl, m_cbrt)
        RAW(0x9E8130B6, ccos, c_ccos)
        RAW(0xE2B596EC, ccosf, c_ccosf)
        RAW(0x834F5917, ccosh, c_ccosh)
        RAW(0xF0AB77C1, ccoshf, c_ccoshf)
        RAW(0xE1C71B05, ccoshl, c_ccosh)
        RAW(0x38BA5590, ccosl, c_ccos)
        FN(0x21E6D304, ceil, m_ceil)
        FN(0xBAF11866, ceilf, m_ceilf)
        RAW(0xEAC7CA2C, ceilf4, v_ceilf4)
        FN(0xDD92118E, ceill, m_ceil)
        RAW(0x488DF791, cexp, c_cexp)
        RAW(0x6F5DD7D2, cexpf, c_cexpf)
        RAW(0x2118FE46, cexpl, c_cexp)
        FN(0x833E6B0E, cimag, m_cimag)
        FN(0xAFD9A625, cimagf, m_cimagf)
        FN(0x12E04CD7, cimagl, m_cimag)
        RAW(0x3593A445, clog, c_clog)
        RAW(0x1988732D, clog10, c_clog10)
        RAW(0xD1A3574C, clog10f, c_clog10f)
        RAW(0xB0FA1592, clog10l, c_clog10)
        RAW(0x9700D9CD, clogf, c_clogf)
        RAW(0x5B18EDED, clogl, c_clog)
        RAW(0xF1AAA2F8, conj, c_conj)
        RAW(0xD125B89E, conjf, c_conjf)
        RAW(0x080414BD, conjl, c_conj)
        FN(0x4E010403, copysign, m_copysign)
        FN(0xACCA2F83, copysignf, m_copysignf)
        RAW(0xEDA86C48, copysignf4, v_copysignf4)
        FN(0x645557BD, copysignl, m_copysign)
        FN(0x9F78F052, cos, m_cos)
        FN(0x24497C52, cosf, m_cosf)
        RAW(0xF5CD1E19, cosf4, v_cosf4)
        RAW(0x32F994A1, cosf4fast, v_cosf4)
        FN(0x8217E783, cosh, m_cosh)
        FN(0x36778D1B, coshf, m_coshf)
        FN(0xBA84EAB5, coshl, m_cosh)
        FN(0x26DEED0B, cosl, m_cos)
        RAW(0x373054D1, cpow, c_cpow)
        RAW(0xFF036800, cpowf, c_cpowf)
        RAW(0xD5ADC4B2, cpowl, c_cpow)
        RAW(0x729B7269, cproj, c_cproj)
        RAW(0xCAAF7AE7, cprojf, c_cprojf)
        RAW(0xBF23F2E7, cprojl, c_cproj)
        FN(0x35D3F688, creal, m_creal)
        FN(0xE9F501DF, crealf, m_crealf)
        FN(0x964AC044, creall, m_creal)
        RAW(0x032CC709, csin, c_csin)
        RAW(0xBA136594, csinf, c_csinf)
        RAW(0x07274304, csinh, c_csinh)
        RAW(0x3436F008, csinhf, c_csinhf)
        RAW(0x64ABDB4D, csinhl, c_csinh)
        RAW(0x2C601F3B, csinl, c_csin)
        RAW(0x2033EEB7, csqrt, c_csqrt)
        RAW(0xE2DE89E6, csqrtf, c_csqrtf)
        RAW(0x46B66F76, csqrtl, c_csqrt)
        RAW(0xE93ABFCA, ctan, c_ctan)
        RAW(0x7793A86B, ctanf, c_ctanf)
        RAW(0xD2A666C9, ctanh, c_ctanh)
        RAW(0xF0947035, ctanhf, c_ctanhf)
        RAW(0x3D549F2A, ctanhl, c_ctanh)
        RAW(0xA20827A8, ctanl, c_ctan)
        RAW(0x03AEA906, divf4, v_divf4)
        RAW(0x657D0E83, divf4fast, v_divf4)
        RAW(0x14348B57, divi4, v_divi4)
        FN(0x8BD67EFC, erf, m_erf)
        FN(0x2627D6B2, erfc, m_erfc)
        FN(0xA9E039C4, erfcf, m_erfcf)
        FN(0xADDCE673, erfcl, m_erfc)
        FN(0xD40F3F2C, erff, m_erff)
        FN(0xE58FC9B5, erfl, m_erf)
        FN(0x18668CE3, exp, m_exp)
        FN(0x4AE52DD3, exp2, m_exp2)
        FN(0xCB6599C0, exp2f, m_exp2f)
        RAW(0x5E48DEDE, exp2f4, v_exp2f4)
        RAW(0x2EC867B4, exp2f4fast, v_exp2f4)
        FN(0x9F0EFC6E, exp2l, m_exp2)
        FN(0xFCF08193, expf, m_expf)
        RAW(0x137F7E77, expf4, v_expf4)
        RAW(0x74902D4B, expf4fast, v_expf4)
        FN(0x79BA9B5C, expl, m_exp)
        FN(0xC8DD9279, expm1, m_expm1)
        FN(0xBB165807, expm1f, m_expm1f)
        RAW(0x1BBDCD9F, expm1f4, v_expm1f4)
        RAW(0x47433144, expm1f4fast, v_expm1f4)
        FN(0x0E8573DC, expm1l, m_expm1)
        FN(0x705D9E24, f_acosf, m_acosf)
        FN(0x4B584841, f_asinf, m_asinf)
        FN(0xABDCCC7A, f_atan2f, m_atan2f)
        FN(0x1E85EF02, f_atanf, m_atanf)
        FN(0xEE204AC6, f_ceilf, m_ceilf)
        FN(0x642F7D6B, f_copysignf, m_copysignf)
        FN(0x7AB679DA, f_cosf, m_cosf)
        FN(0x4A5AE27D, f_exp2f, m_exp2f)
        FN(0xD8D157F5, f_expf, m_expf)
        FN(0x05CB1718, f_fdimf, m_fdimf)
        FN(0xB4EF29D5, f_floorf, m_floorf)
        FN(0xF83A372F, f_fmaf, m_fmaf)
        FN(0x39EF81C9, f_fmaxf, m_fmaxf)
        FN(0x3F6262B3, f_fminf, m_fminf)
        FN(0x10627248, f_fmodf, m_fmodf)
        FN(0x3ADC01D7, f_frexpf, m_frexpf)
        FN(0x2CBB6F53, f_hypotf, m_hypotf)
        FN(0x728149E5, f_ldexpf, m_ldexpf)
        FN(0xAFA13040, f_llrintf, m_llrintf)
        FN(0x6F639AFB, f_llroundf, m_llroundf)
        FN(0x6C009C56, f_log10f, m_log10f)
        FN(0xBBAA300B, f_log1pf, m_log1pf)
        FN(0x6764C707, f_log2f, m_log2f)
        FN(0xEA1E83E3, f_logf, m_logf)
        FN(0xC7FB73D6, f_lrintf, m_lrintf)
        FN(0xC477C0F6, f_lroundf, m_lroundf)
        FN(0x961688D1, f_nearbyintf, m_nearbyintf)
        FN(0x00FDE072, f_powf, m_powf)
        FN(0x4B03D5B2, f_rintf, m_rintf)
        FN(0xB54CC9A1, f_sinf, m_sinf)
        FN(0x246EA8D0, f_sqrtf, m_sqrtf)
        FN(0xED9D1AC5, f_tanf, m_tanf)
        FN(0x58EB9E57, fabs, m_fabs)
        FN(0x3DA55602, fabsf, m_fabsf)
        RAW(0xF99DA2FC, fabsf4, v_fabsf4)
        FN(0xD42904B7, fabsl, m_fabs)
        FN(0xD8270894, fdim, m_fdim)
        FN(0x46CF72D9, fdimf, m_fdimf)
        RAW(0x9F65BD34, fdimf4, v_fdimf4)
        FN(0x8B168769, fdiml, m_fdim)
        FN(0x4ADD664C, feclearexcept, m_feclearexcept)
        FN(0x238AF59B, fegetenv, m_fegetenv)
        FN(0xBBF7354E, fegetexceptflag, m_fegetexceptflag)
        FN(0x892F2590, fegetround, m_fegetround)
        FN(0xD54039CB, fegettrapenable, m_fegettrapenable)
        FN(0x417851CE, feholdexcept, m_feholdexcept)
        FN(0xE5EA65E8, feraiseexcept, m_feraiseexcept)
        FN(0x0C14CFCC, fesetenv, m_fesetenv)
        FN(0x3261DE11, fesetexceptflag, m_fesetexceptflag)
        FN(0xD97CE5D4, fesetround, m_fesetround)
        FN(0x72A3ED28, fesettrapenable, m_fesettrapenable)
        FN(0xC977E1EA, fetestexcept, m_fetestexcept)
        FN(0xFA97AFBF, feupdateenv, m_feupdateenv)
        FN(0x519EBB77, floor, m_floor)
        FN(0x23B985F7, floorf, m_floorf)
        RAW(0x452AC4BB, floorf4, v_floorf4)
        FN(0x6DF35518, floorl, m_floor)
        FN(0x4826DB61, fma, m_fma)
        FN(0x476B5591, fmaf, m_fmaf)
        RAW(0xDC14974C, fmaf4, v_fmaf4)
        FN(0xC7F1D407, fmal, m_fma)
        FN(0x2AF4B73B, fmax, m_fmax)
        FN(0x895CDB49, fmaxf, m_fmaxf)
        RAW(0x752FA85E, fmaxf4, v_fmaxf4)
        FN(0x2C45FE6A, fmaxl, m_fmax)
        FN(0xA4578433, fmin, m_fmin)
        FN(0x7C2EAEB5, fminf, m_fminf)
        RAW(0xC41F01DB, fminf4, v_fminf4)
        FN(0x00367BE0, fminl, m_fmin)
        FN(0xE769E5CF, fmod, m_fmod)
        FN(0x9A81E583, fmodf, m_fmodf)
        RAW(0xCA239640, fmodf4, v_fmodf4)
        FN(0xB598A495, fmodl, m_fmod)
        FN(0x7F381837, frexp, m_frexp)
        FN(0x6636C4A5, frexpf, m_frexpf)
        RAW(0x3E7EB58F, frexpf4, v_frexpf4)
        FN(0xC357B33A, frexpl, m_frexp)
        FN(0x6DDD31B2, hypot, m_hypot)
        FN(0xEBB4E08A, hypotf, m_hypotf)
        RAW(0x0C9B8305, hypotf4, v_hypotf4)
        FN(0x7831A2E0, hypotl, m_hypot)
        FN(0xA823836B, ilogb, m_ilogb)
        FN(0x65935877, ilogbf, m_ilogbf)
        RAW(0x28FAAA5A, ilogbf4, v_ilogbf4)
        FN(0xC8910002, ilogbl, m_ilogb)
        FN(0xF4AD6EA8, ldexp, m_ldexp)
        FN(0x430309A1, ldexpf, m_ldexpf)
        RAW(0x3B802524, ldexpf4, v_ldexpf4)
        FN(0xD231E30A, ldexpl, m_ldexp)
        FN(0x69725DCE, lgamma, m_lgamma)
        FN(0x9F03DD3E, lgammaf, m_lgammaf)
        FN(0x749440F9, lgammal, m_lgamma)
        FN(0xE6C1FF41, llrint, m_llrint)
        FN(0x2A89CE33, llrintf, m_llrintf)
        FN(0x0F02F882, llrintl, m_llrint)
        FN(0x01B84B27, llround, m_llround)
        FN(0xA4CA5CF2, llroundf, m_llroundf)
        FN(0x889CCCB0, llroundl, m_llround)
        FN(0x714ADCE1, log, m_log)
        FN(0xD8F79F4C, log10, m_log10)
        FN(0x16BF208A, log10f, m_log10f)
        RAW(0x3459748B, log10f4, v_log10f4)
        RAW(0x05E27A13, log10f4fast, v_log10f4)
        FN(0x8F96319E, log10l, m_log10)
        FN(0x56C573A8, log1p, m_log1p)
        FN(0x21A37B3E, log1pf, m_log1pf)
        RAW(0xA7658186, log1pf4, v_log1pf4)
        RAW(0xFB6E6213, log1pf4fast, v_log1pf4)
        FN(0x37345541, log1pl, m_log1p)
        FN(0x17316BEE, log2, m_log2)
        FN(0xAB377381, log2f, m_log2f)
        RAW(0x07DAED62, log2f4, v_log2f4)
        RAW(0x96D1B95E, log2f4fast, v_log2f4)
        FN(0xFA28434B, log2l, m_log2)
        FN(0x2BB0F2C9, logb, m_logb)
        FN(0xB24BD2F8, logbf, m_logbf)
        RAW(0x69040B9B, logbf4, v_logbf4)
        FN(0x4930AC11, logbl, m_logb)
        FN(0xD477852D, logf, m_logf)
        RAW(0x30BC7A53, logf4, v_logf4)
        RAW(0xE913A166, logf4fast, v_logf4)
        FN(0x9E3ADA21, logl, m_log)
        FN(0x3AD203FA, lrint, m_lrint)
        FN(0x9558ED08, lrintf, m_lrintf)
        FN(0x35B6E70A, lrintl, m_lrint)
        FN(0x772F1E4D, lround, m_lround)
        FN(0xFBB4047A, lroundf, m_lroundf)
        FN(0x3792B12D, lroundl, m_lround)
        FN(0xA713F8CF, modf, m_modf)
        FN(0xB79012BA, modff, m_modff)
        RAW(0xC4CCCD1F, modff4, v_modff4)
        FN(0x9110708A, modfl, m_modf)
        FN(0x45034943, nan, m_nan)
        FN(0xCE91FF18, nanf, m_nanf)
        FN(0x2DCAB6A4, nanl, m_nan)
        FN(0x758F33DC, nearbyint, m_nearbyint)
        FN(0xC0609820, nearbyintf, m_nearbyintf)
        FN(0x75E3E2E9, nearbyintl, m_nearbyint)
        RAW(0x664E04B9, negatef4, v_negatef4)
        FN(0x8ECAE294, nextafter, m_nextafter)
        FN(0xB7696143, nextafterf, m_nextafterf)
        FN(0x68F72416, nextafterl, m_nextafter)
        FN(0x4CB5FA99, nexttoward, m_nexttoward)
        FN(0x4FA4F5EC, nexttowardf, m_nexttowardf)
        FN(0x321C55DE, nexttowardl, m_nexttoward)
        FN(0x38E69F09, pow, m_pow)
        FN(0x4DDB926B, powf, m_powf)
        RAW(0x4A6CA9A6, powf4, v_powf4)
        RAW(0xF95B7769, powf4fast, v_powf4)
        FN(0x34C0371E, powl, m_pow)
        RAW(0xBD7410D9, recipf4, v_recipf4)
        RAW(0x88FB4A66, recipf4fast, v_recipf4)
        FN(0x44CD6308, remainder, m_remainder)
        FN(0xDDDABB32, remainderf, m_remainderf)
        RAW(0x4D878773, remainderf4, v_remainderf4)
        FN(0x18B26998, remainderl, m_remainder)
        FN(0x4189A367, remquo, m_remquo)
        FN(0xBB761C89, remquof, m_remquof)
        FN(0xB9D2AD22, remquol, m_remquo)
        FN(0x0F428F0F, rint, m_rint)
        FN(0x54D2FB8C, rintf, m_rintf)
        FN(0x18EC6099, rintl, m_rint)
        FN(0xF3EC0258, round, m_round)
        FN(0xC984BF53, roundf, m_roundf)
        FN(0x99C228FC, roundl, m_round)
        RAW(0x329EC019, rsqrtf4, v_rsqrtf4)
        RAW(0x1A1ADEDE, rsqrtf4fast, v_rsqrtf4)
        FN(0x4152669C, scalbln, m_scalbln)
        FN(0x31BE25C3, scalblnf, m_scalblnf)
        FN(0xD48EAAE1, scalblnl, m_scalbln)
        FN(0xC78AC9D0, scalbn, m_scalbn)
        FN(0x3E919CBA, scalbnf, m_scalbnf)
        FN(0x3DFA060F, scalbnl, m_scalbn)
        FN(0x4EB5EB51, sin, m_sin)
        RAW(0xF19C5E94, sincosf4, v_sincosf4)
        RAW(0x6E9EB0DC, sincosf4fast, v_sincosf4)
        FN(0x0CBDAE68, sinf, m_sinf)
        RAW(0x0A242ED5, sinf4, v_sinf4)
        RAW(0x547FB4A7, sinf4fast, v_sinf4)
        FN(0x683CACB3, sinh, m_sinh)
        FN(0xD7653782, sinhf, m_sinhf)
        FN(0x376FB27F, sinhl, m_sinh)
        FN(0xD97852B7, sinl, m_sin)
        FN(0x659E011E, sqrt, m_sqrt)
        FN(0x8451EDF0, sqrtf, m_sqrtf)
        RAW(0xED05C265, sqrtf4, v_sqrtf4)
        RAW(0x7D02A5CA, sqrtf4fast, v_sqrtf4)
        FN(0x2B282EBB, sqrtl, m_sqrt)
        FN(0xD50277AD, tan, m_tan)
        FN(0x9379E36E, tanf, m_tanf)
        RAW(0x487BBD1C, tanf4, v_tanf4)
        RAW(0x7F91CD41, tanf4fast, v_tanf4)
        FN(0xE2F1D4B2, tanh, m_tanh)
        FN(0xD4F37B9D, tanhf, m_tanhf)
        FN(0x44CF744B, tanhl, m_tanh)
        FN(0xD3A346A8, tanl, m_tan)
        FN(0x11C51388, tgamma, m_tgamma)
        FN(0x9F46F5A4, tgammaf, m_tgammaf)
        FN(0x182CD542, tgammal, m_tgamma)
        FN(0x475D855B, trunc, m_trunc)
        FN(0x2A138D2B, truncf, m_truncf)
        RAW(0x1E623F95, truncf4, v_truncf4)
        FN(0x55C8A549, truncl, m_trunc)
    });
#undef FN
#undef RAW
}

} // namespace

void register_sys_libm(Registry& r)
{
    add_functions(r, "sys_libm");
    add_functions(r, "sys_libc");

    r.add_variables("sys_libc", {
        PS_HLE_VAR(0x985FC057, _Dbl, 72, 8, v_dbl),
        PS_HLE_VAR(0x529D4301, _Denorm, 16, 8, v_denorm),
        PS_HLE_VAR(0x92C43F6D, _Eps, 16, 8, v_eps),
        PS_HLE_VAR(0xEACE53D6, _FDenorm, 16, 8, v_fdenorm),
        PS_HLE_VAR(0xFF2F0CC7, _FEps, 16, 8, v_feps),
        PS_HLE_VAR(0x6524499E, _FInf, 16, 8, v_finf),
        PS_HLE_VAR(0x210B2F6E, _FNan, 16, 8, v_fnan),
        PS_HLE_VAR(0xB6F5F98C, _FRteps, 16, 8, v_frteps),
        PS_HLE_VAR(0x76628EFB, _FSnan, 16, 8, v_fsnan),
        PS_HLE_VAR(0xEF25075B, _FXbig, 4, 4, v_fxbig),
        PS_HLE_VAR(0x5FF11EB4, _FZero, 4, 4, nullptr),
        PS_HLE_VAR(0x26A34F81, _Flt, 72, 8, v_flt),
        PS_HLE_VAR(0x2470D3BC, _Hugeval, 16, 8, v_inf),
        PS_HLE_VAR(0x57DBCF27, _Inf, 16, 8, v_inf),
        PS_HLE_VAR(0xB5B84F80, _LDenorm, 16, 8, v_denorm),
        PS_HLE_VAR(0x45EC2DF6, _LEps, 16, 8, v_eps),
        PS_HLE_VAR(0x17667744, _LInf, 16, 8, v_inf),
        PS_HLE_VAR(0x071928B0, _LNan, 16, 8, v_nan),
        PS_HLE_VAR(0x6A09DF41, _LRteps, 16, 8, v_rteps),
        PS_HLE_VAR(0x9C8454C9, _LSnan, 16, 8, v_snan),
        PS_HLE_VAR(0x29E76A6D, _LXbig, 8, 8, v_xbig),
        PS_HLE_VAR(0x81ACF7C1, _LZero, 8, 8, nullptr),
        PS_HLE_VAR(0xD698385D, _Ldbl, 72, 8, v_dbl),
        PS_HLE_VAR(0xD59C193C, _Nan, 16, 8, v_nan),
        PS_HLE_VAR(0x7F456AF2, _Rteps, 16, 8, v_rteps),
        PS_HLE_VAR(0x7AFF3242, _Snan, 16, 8, v_snan),
        PS_HLE_VAR(0x790B0082, _Xbig, 8, 8, v_xbig),
        PS_HLE_VAR(0x0FBC732D, _Zero, 8, 8, nullptr),
    });
    r.add_variables("sys_libm", {
        PS_HLE_VAR(0xAD443E79, _Erf_one, 16, 8, v_erf_one),
        PS_HLE_VAR(0x3ACAD7F1, _Erf_small, 16, 8, v_erf_small),
        PS_HLE_VAR(0x3FB8629D, _FErf_one, 16, 8, v_ferf_one),
        PS_HLE_VAR(0x4AF28F31, _FErf_small, 16, 8, v_ferf_small),
        PS_HLE_VAR(0xE9892674, _FGamma_big, 16, 8, v_fgamma_big),
        PS_HLE_VAR(0x42EB9508, _Fenv0, 8, 8, nullptr),
        PS_HLE_VAR(0xF39005FC, _Gamma_big, 16, 8, v_gamma_big),
        PS_HLE_VAR(0x1CF745BC, _LErf_one, 16, 8, v_erf_one),
        PS_HLE_VAR(0xA8D907FF, _LErf_small, 16, 8, v_erf_small),
        PS_HLE_VAR(0x2259EF96, _LGamma_big, 16, 8, v_gamma_big),
    });
}

} // namespace pspocket::hle
