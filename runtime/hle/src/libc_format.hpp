#pragma once

// sys_libc's formatting basics (D-508): the "C" locale's classes, a C99 integer subject sequence (the locale and
// time code's own reading of numbers), printf's argument sources and fields, for narrow and wide formats. The PS3's C
// library is Dinkumware's (RPCS3 PPUFunction.cpp names its internals); its implementation-defined choices are written
// next to the code. No guest memory is touched here: a FormatSource gives the arguments and the strings they point
// at. The C library's sizes: int, long, size_t, ptrdiff_t and pointers 32 bits; long long and intmax_t 64; long double
// is double; wchar_t 32 bits.
//
// printf is the SDK's own (libc_printf.hpp, D-905); scanf and strtod's family are the SDK's too (libc_scan.hpp, D-907).

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <locale.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

#include "libc_printf.hpp"
#include "pspocket/core/types.hpp"

namespace pspocket::hle::libc {

// ------------------------------------------------------------------------------------------------- the C locale

// The host's "C" locale, for strtod and snprintf whatever the host process's locale is.
inline locale_t c_locale()
{
    static const locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return loc;
}

// Uses the "C" locale on this host thread while it lives.
class CLocaleScope {
public:
    CLocaleScope() : saved_(uselocale(c_locale())) {}
    ~CLocaleScope() { uselocale(saved_); }
    CLocaleScope(const CLocaleScope&) = delete;
    CLocaleScope& operator=(const CLocaleScope&) = delete;

private:
    locale_t saved_;
};

// isspace and friends in the C locale (bytes past 127 have no class).
inline bool c_isspace(s64 c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool c_isdigit(s64 c) { return c >= '0' && c <= '9'; }
inline bool c_isxdigit(s64 c) { return c_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline bool c_isalpha(s64 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline s64 c_tolower(s64 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
// The value of a digit in bases up to 36, or 99.
inline int digit_value(s64 c)
{
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A' + 10);
    return 99;
}

// The C locale's multibyte characters are single bytes, each the wide character of its value (Dinkumware's "C"
// locale): a wide character past 255 has no multibyte form (EILSEQ).
inline std::optional<char> wide_to_byte(char32_t wc)
{
    if (static_cast<u32>(wc) > 0xFF) return std::nullopt;
    return static_cast<char>(static_cast<u8>(wc));
}

// ------------------------------------------------------------------------------------------------- integers

// A C99 integer subject sequence (strtol's, 7.20.1.4): white space, a sign, "0x" for base 16 (or 0) when a hex digit
// follows, the digits. `at(i)` is the i-th character (or -1 past the end); `limit` caps the characters looked at.
struct IntParse {
    u64 magnitude = 0;
    bool negative = false;
    bool overflow = false; // the magnitude passed 2^64 - 1
    bool any = false;      // digits were found
    u64 end = 0;           // the characters taken (0 without digits)
};

template <class At>
IntParse parse_int(At&& at, int base, u64 limit = ~u64{0})
{
    IntParse r;
    u64 i = 0;
    const auto ch = [&](u64 k) -> s64 { return k < limit ? at(k) : -1; };
    while (c_isspace(ch(i))) i++;
    if (ch(i) == '+' || ch(i) == '-') r.negative = ch(i++) == '-';
    if ((base == 0 || base == 16) && ch(i) == '0' && (ch(i + 1) == 'x' || ch(i + 1) == 'X') &&
        digit_value(ch(i + 2)) < 16) {
        i += 2;
        base = 16;
    } else if (base == 0) {
        base = ch(i) == '0' ? 8 : 10;
    }
    for (int d; (d = digit_value(ch(i))) < base; i++) {
        r.any = true;
        const u64 next = r.magnitude * static_cast<u64>(base) + static_cast<u64>(d);
        if (r.magnitude > (~u64{0} - static_cast<u64>(d)) / static_cast<u64>(base)) r.overflow = true;
        r.magnitude = r.overflow ? ~u64{0} : next;
    }
    r.end = r.any ? i : 0;
    return r;
}

// strtol's result for an integer of `bits` (32 or 64), signed or not; *range set when it was out of range (ERANGE).
inline u64 int_result(const IntParse& p, u32 bits, bool is_signed, bool* range)
{
    *range = false;
    const u64 umax = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    if (is_signed) {
        const u64 smax = umax >> 1; // 2^(bits-1) - 1
        if (p.negative) {
            if (p.overflow || p.magnitude > smax + 1) {
                *range = true;
                return (0 - (smax + 1)) & umax;
            }
            return (0 - p.magnitude) & umax;
        }
        if (p.overflow || p.magnitude > smax) {
            *range = true;
            return smax;
        }
        return p.magnitude;
    }
    if (p.overflow || p.magnitude > umax) {
        *range = true;
        return umax;
    }
    return p.negative ? (0 - p.magnitude) & umax : p.magnitude;
}

// ------------------------------------------------------------------------------------------------- printf

// printf is the SDK's (libc_printf.hpp, D-905); FormatSource is there. What follows: the lengths scanf's directives
// name, and the facets' way into printf's conversions.

enum class Len : u8 { None, hh, h, l, ll, j, z, t, L };

struct Spec {
    bool minus = false, plus = false, space = false, alt = false, zero = false;
    u64 width = 0;
    s64 prec = -1; // -1: none
    Len len = Len::None;
};

// Widths and precisions scanf and the facets take are cut to these.
inline constexpr u64 kMaxWidth = 0x1000000;
inline constexpr s64 kMaxNumericPrecision = 0x100000;

// The bits of an integer argument of this length for scanf: hh 8, h 16, l/z/t/none 32 (long, size_t and ptrdiff_t
// are 32 bits), ll/j/L 64.
inline u32 int_bits(Len len)
{
    switch (len) {
    case Len::hh: return 8;
    case Len::h: return 16;
    case Len::ll: case Len::j: case Len::L: return 64;
    default: return 32;
    }
}

namespace fmtdetail {

// A field of the SDK's printf for one value, from a Spec: the flags, width and precision as _Printf parses them.
template <class C>
Pft<C, C*> pft_of(const Spec& s)
{
    Pft<C, C*> px;
    px.flags = static_cast<u16>((s.space ? kFsp : 0) | (s.plus ? kFpl : 0) | (s.minus ? kFmi : 0) | (s.alt ? kFno : 0) |
                                (s.zero ? kFze : 0));
    px.width = static_cast<s32>(std::min<u64>(s.width, 0x7FFFFFFF));
    px.prec = s.prec < 0 ? -1 : static_cast<s32>(std::min<s64>(s.prec, 0x7FFFFFFF));
    return px;
}

} // namespace fmtdetail

// printf's text of one integer conversion (d, i, u, o, x, X, p) of the raw argument under `s`: the SDK's (_Putfld,
// _Litob), its length ll (64 bits), l or none (32).
template <class C>
void put_integer(std::basic_string<C>& out, const Spec& s, char conv, u64 raw)
{
    Pft<C, C*> px = fmtdetail::pft_of<C>(s);
    C ac[64] = {};
    const bool wide64 = s.len == Len::ll || s.len == Len::j;
    if (conv == 'p') {
        px.v = static_cast<u32>(raw);
        px.s = ac + px.n0;
        litob(px, 'x');
    } else if (conv == 'd' || conv == 'i') {
        px.v = wide64 ? raw : static_cast<u64>(static_cast<s64>(static_cast<s32>(raw)));
        if (static_cast<s64>(px.v) < 0) ac[px.n0++] = static_cast<C>('-');
        else if (px.flags & kFpl) ac[px.n0++] = static_cast<C>('+');
        else if (px.flags & kFsp) ac[px.n0++] = static_cast<C>(' ');
        px.s = ac + px.n0;
        litob(px, conv);
    } else {
        px.v = wide64 ? raw : static_cast<u32>(raw);
        if ((px.flags & kFno) && px.v != 0 && (conv == 'x' || conv == 'X')) {
            ac[px.n0++] = static_cast<C>('0');
            ac[px.n0++] = static_cast<C>(conv);
        }
        px.s = ac + px.n0;
        litob(px, conv);
    }
    StringSink<C> sink;
    puttxt(px, static_cast<C*>(ac), sink);
    out += sink.text;
}

// printf's text of one floating conversion (e, E, f, F, g, G, a, A) of v under `s`: the SDK's (_Putfld, _Ldtob).
template <class C>
void put_double(std::basic_string<C>& out, const Spec& s, char conv, double v)
{
    Pft<C, C*> px = fmtdetail::pft_of<C>(s);
    C ac[64] = {};
    px.v = std::bit_cast<u64>(v);
    if (px.v >> 63) ac[px.n0++] = static_cast<C>('-');
    else if (px.flags & kFpl) ac[px.n0++] = static_cast<C>('+');
    else if (px.flags & kFsp) ac[px.n0++] = static_cast<C>(' ');
    px.s = ac + px.n0;
    ldtob(px, conv);
    StringSink<C> sink;
    puttxt(px, static_cast<C*>(ac), sink);
    out += sink.text;
}

template <class C>
struct FormatResult {
    std::basic_string<C> text;
    bool failed = false; // an encoding error (EILSEQ): the function returns a negative value
};

// The text of `fmt` with the arguments of `src`, as the SDK's printf makes it (libc_printf.hpp).
template <class C>
FormatResult<C> format(std::basic_string_view<C> fmt, FormatSource& src)
{
    PrintResult<C> r = print_host<C>(fmt, src);
    return FormatResult<C>{std::move(r.text), r.failed()};
}

} // namespace pspocket::hle::libc
