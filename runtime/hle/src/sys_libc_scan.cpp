// sys_libc's formatted input internals and parsing steps (D-907), under sys_libc and sys_libm: the SDK's scan structure
// and the conversions on it (_Getfld, _Getint, _Getfloat, _Getstr and the wide forms), and Dinkumware's digit-array
// steps under strtod (_Stopfx, _Stoflt, _Stoxflt and the wide forms). The engine is libc_scan.hpp's.
//
// The SDK's _Sft (32 bytes): +0 int (*pfn)(void *arg, int ch, int getfl), +4 arg, +8 va_list ap (the address of the
// next argument's 64-bit slot), +12 const char *s (the format at the conversion), +16 int nchar (characters read so
// far), +20 int nget (what the field may still read), +24 int width, +28 char noconv ('*' when suppressed), +29 char
// qual (h l L j t z, 'b' for hh, 'q' for ll), +30 char stored (set when a conversion assigned). _WSft is the same with
// a wide format (32-bit wchar_t, D-512) and wint_t pfn. Grand Theft Auto V's copy has the narrow ones (its wchar_t is
// 16 bits: %ls stores 16-bit characters there, 32-bit ones here); no game's copy has the wide ones, which follow the
// narrow ones' design on wide characters.

#include <format>
#include <string>

#include "libc_guest.hpp"
#include "libc_scan.hpp"
#include "libc_scan_guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- the scan structure

// One of _Getfld's family on the guest _Sft at px: its fields read into the engine's, the conversion run, its fields
// written back (the format pointer where the conversion left it, ap after the arguments taken).
template <class C, class F>
s32 on_sft(PPUContext& ctx, const char* fn, u32 px, F&& body)
{
    if (!range_ok(ctx, fn, px, 32)) return 0;
    u8* b = gb(ctx);
    const u32 s = ps_read32(b, px + 12);
    const auto fmt = scan_format<C>(ctx, s);
    if (!fmt) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, s));
        return 0;
    }
    GuestScanIo io(ctx, ps_read32(b, px), ps_read32(b, px + 4));
    GuestScanArgs args(ctx, ps_read32(b, px + 8));
    Sft<C> x;
    x.io = &io;
    x.args = &args;
    x.s = fmt->c_str();
    x.nchar = static_cast<s32>(ps_read32(b, px + 16));
    x.nget = static_cast<s32>(ps_read32(b, px + 20));
    x.width = static_cast<s32>(ps_read32(b, px + 24));
    x.noconv = b[px + 28];
    x.qual = b[px + 29];
    x.stored = b[px + 30];
    const s32 r = body(x);
    b = gb(ctx);
    ps_write32(b, px + 8, args.ap());
    ps_write32(b, px + 12, s + static_cast<u32>(sizeof(C) == 1 ? 1 : 4) * static_cast<u32>(x.s - fmt->c_str()));
    ps_write32(b, px + 16, static_cast<u32>(x.nchar));
    ps_write32(b, px + 20, static_cast<u32>(x.nget));
    ps_write32(b, px + 24, static_cast<u32>(x.width));
    b[px + 28] = x.noconv;
    b[px + 29] = x.qual;
    b[px + 30] = x.stored;
    for (const std::string& p : args.problems) refuse(ctx, std::format("{}: {}", fn, p));
    return r;
}

// int _Getfld(_Sft *px): the conversion at px->s; 1 when it matched (stored set when it assigned), 0 for a matching
// failure, EOF for an input failure.
template <class C>
s32 getfld(PPUContext& ctx, const char* fn, u32 px)
{
    return on_sft<C>(ctx, fn, px, [](Sft<C>& x) { return sdk_getfld(x); });
}
template <class C>
s32 getint(PPUContext& ctx, const char* fn, u32 px)
{
    return on_sft<C>(ctx, fn, px, [](Sft<C>& x) { return sdk_getint(x); });
}
template <class C>
s32 getfloat(PPUContext& ctx, const char* fn, u32 px)
{
    return on_sft<C>(ctx, fn, px, [](Sft<C>& x) { return sdk_getfloat(x); });
}
template <class C>
s32 getstr(PPUContext& ctx, const char* fn, u32 px, s32 sfl)
{
    return on_sft<C>(ctx, fn, px, [sfl](Sft<C>& x) { return sdk_getstr(x, sfl); });
}

// int _Getfld(_Sft *px): the conversion at px->s; 1 when it matched (stored set when it assigned), 0 for a matching
// failure, EOF for an input failure. _Getstr(px, sfl): %s (sfl > 0), %c (0) or %[ (< 0).
s32 _Getfld_(PPUContext& ctx, u32 px) { return getfld<char>(ctx, "_Getfld", px); }
s32 _Getint_(PPUContext& ctx, u32 px) { return getint<char>(ctx, "_Getint", px); }
s32 _Getfloat_(PPUContext& ctx, u32 px) { return getfloat<char>(ctx, "_Getfloat", px); }
s32 _Getstr_(PPUContext& ctx, u32 px, s32 sfl) { return getstr<char>(ctx, "_Getstr", px, sfl); }
s32 _WGetfld_(PPUContext& ctx, u32 px) { return getfld<char32_t>(ctx, "_WGetfld", px); }
s32 _WGetint_(PPUContext& ctx, u32 px) { return getint<char32_t>(ctx, "_WGetint", px); }
s32 _WGetfloat_(PPUContext& ctx, u32 px) { return getfloat<char32_t>(ctx, "_WGetfloat", px); }
s32 _WGetstr_(PPUContext& ctx, u32 px, s32 sfl) { return getstr<char32_t>(ctx, "_WGetstr", px, sfl); }

// ------------------------------------------------------------------------------------------------- the parsing steps

// int _Stopfx(const char **ps, char **endptr): the prefix of a floating field (white space, sign, nan, inf, 0x); *ps
// moved to its digits; the code (1 decimal, 2 hex, 3 infinity, 4 NaN, 0 an error; 8 added for a minus).
s32 stopfx(PPUContext& ctx, const char* fn, u32 ps, u32 endptr, bool wide)
{
    if (!range_ok(ctx, fn, ps, 4)) return 0;
    const u32 s = ps_read32(gb(ctx), ps);
    GuestChars at(ctx, s, wide);
    const Prefix pf = sdk_stopfx(at, 0);
    const u32 pos = s + at.unit() * static_cast<u32>(pf.pos);
    if (pf.sets_end && endptr && range_ok(ctx, fn, endptr, 4)) ps_write32(gb(ctx), endptr, pos);
    ps_write32(gb(ctx), ps, pos);
    return pf.code;
}

s32 _Stopfx_(PPUContext& ctx, u32 ps, u32 endptr) { return stopfx(ctx, "_Stopfx", ps, endptr, false); }
s32 _WStopfx_(PPUContext& ctx, u32 ps, u32 endptr) { return stopfx(ctx, "_WStopfx", ps, endptr, true); }

// int _Stoflt(const char *s0, const char *s, char **endptr, long lo[], int maxsig) and _Stoxflt: the digits from s as
// maxsig words at most (nine decimal or seven hex digits each) in lo[1..], lo[0] the power of ten (two) they are
// scaled by; the number of words. *endptr rolls back to s0 when there is no number.
s32 stoflt(PPUContext& ctx, const char* fn, u32 s0, u32 s, u32 endptr, u32 lo, s32 maxsig, bool hex, bool wide)
{
    if (maxsig < -0x0E38E38E || maxsig > 0x0E38E38E) { // maxsig * 9 must be an int
        refuse(ctx, std::format("{}: maxsig {} is out of range", fn, maxsig));
        return 0;
    }
    const u32 base = s0 < s ? s0 : s;
    const u32 unit = wide ? 4 : 1;
    GuestChars at(ctx, base, wide);
    DigitWords w;
    const DigitsResult d = sdk_stoflt(at, (s0 - base) / unit, (s - base) / unit, w, maxsig, hex);
    if (!range_ok(ctx, fn, lo, 4 * (w.top() + 1))) return 0;
    for (u64 i = 0; i <= w.top(); i++) ps_write32(gb(ctx), lo + 4 * static_cast<u32>(i), w.get(i));
    if (endptr && range_ok(ctx, fn, endptr, 4)) ps_write32(gb(ctx), endptr, base + unit * static_cast<u32>(d.end));
    return d.words;
}

s32 _Stoflt_(PPUContext& ctx, u32 s0, u32 s, u32 e, u32 lo, s32 n) { return stoflt(ctx, "_Stoflt", s0, s, e, lo, n, false, false); }
s32 _Stoxflt_(PPUContext& ctx, u32 s0, u32 s, u32 e, u32 lo, s32 n) { return stoflt(ctx, "_Stoxflt", s0, s, e, lo, n, true, false); }
s32 _WStoflt_(PPUContext& ctx, u32 s0, u32 s, u32 e, u32 lo, s32 n) { return stoflt(ctx, "_WStoflt", s0, s, e, lo, n, false, true); }
s32 _WStoxflt_(PPUContext& ctx, u32 s0, u32 s, u32 e, u32 lo, s32 n) { return stoflt(ctx, "_WStoxflt", s0, s, e, lo, n, true, true); }

} // namespace

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_scan(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(_Getfld), LIBC(_Getint), LIBC(_Getfloat), LIBC(_Getstr), LIBC(_WGetfld), LIBC(_WGetint),
            LIBC(_WGetfloat), LIBC(_WGetstr), LIBC(_Stopfx), LIBC(_Stoflt), LIBC(_Stoxflt), LIBC(_WStopfx),
            LIBC(_WStoflt), LIBC(_WStoxflt),
        });
    }
}
#undef LIBC

} // namespace pspocket::hle
