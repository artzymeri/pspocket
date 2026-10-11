#pragma once

// sys_libc's printf as the SDK's C library has it (D-905): Dinkumware's _Printf with its print-formatting structure
// _Pft and its steps _Putfld, _Puttxt, _Putstr, _Litob, _Ldtob and _Genld, Sony's AltiVec qualifier among them, and
// the wide forms (_WPrintf, _WPutfld, _WPuttxt, _WPutstr, _WLitob, _WLdtob, _WGenld). A game's statically linked copy
// of the library (Assassin's Creed's) was read and compared with this code call for call; its choices, which C99
// leaves to the library or which differ from the host's printf:
// - Floating point: _Ldtob scales the value by powers of ten in double arithmetic to eight digits a time, generates at
//   most 19 significant digits ("%.20f" of 0.1 is 0.10000000000000000000), and rounds half up on the first dropped
//   digit ("%.0f" of 0.5 is 1, "%.1f" of 0.25 is 0.3); the digits are those of that arithmetic, not the exact
//   decimal expansion. "%a" is exact, with no more digits than the value needs.
// - %p is the hex digits alone ("%#p" too); %s of a null pointer prints "(null)" whatever the precision; an unknown
//   conversion character prints itself, padded to the width, and takes no argument ("%5k" is "    k"); "%5%" pads.
// - L with an integer conversion is 32 bits (only ll, q and j are 64); "% Ld" has no space.
// - The "C" locale's multibyte characters are UTF-8 (see c_wctomb). %ls and %lc convert through a buffer of 64 bytes,
//   or of width + 3 when the field is right-justified and wider; a buffer that fills is put out with no padding (so
//   "%-100ls" of a long string is not padded).
// - Sony's vectors (AltiVec): v, vl or lv for four 32-bit elements, vh or hv for eight 16-bit ones, vhh or hhv for
//   sixteen bytes, vll or llv for two 64-bit ones, vL or Lv (with a floating conversion) for two doubles; v with a
//   floating conversion is four floats, vc sixteen characters; "%xc" and "%cx" (o, u, x, X) sixteen bytes. Each element
//   is a field of its own, separated by a space or the flag , ; : or _ (a space unless one is given; %vc has none).
// - The wide printf has no vectors and no %S; its %n stores nothing, and its fields are padded even for %n.
// The output goes through a sink a piece at a time as the library's does (_Puttxt: padding, the prefix, zeros, the
// text): the caller's pfn for _Printf, a string for sprintf and the rest. Arguments come from a FormatSource; the text
// lives where the memory policy says (the host's buffers, or guest memory for the internals a game calls itself).

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::libc {

// ------------------------------------------------------------------------------------------------- arguments

// Where a formatter takes its arguments from (the guest's variadic arguments or va_list, or an SPU's local store) and
// what their pointers lead to. A string or %n target that is not memory is a problem (noted, the call goes on).
class FormatSource {
public:
    virtual ~FormatSource() = default;
    virtual u64 next() = 0; // the next argument's 64-bit slot (an int is its low word, a double its bits)
    // The next integer or pointer argument of `bits` (32 or 64), in the low bits: a source that keeps a 32-bit value
    // elsewhere than a 64-bit one (an SPU's quadwords) needs to know which; the PPU's slots do not.
    virtual u64 next_int(u32 bits)
    {
        (void)bits;
        return next();
    }
    virtual double next_double() { return std::bit_cast<double>(next()); }
    // A vector argument (Sony's AltiVec qualifier): 16 bytes at the next 16-byte boundary of the arguments, in memory
    // order. The default takes two slots.
    virtual std::array<u8, 16> next_vector()
    {
        std::array<u8, 16> v{};
        const u64 hi = next(), lo = next();
        for (int k = 0; k < 8; k++) {
            v[static_cast<usize>(k)] = static_cast<u8>(hi >> (56 - 8 * k));
            v[static_cast<usize>(8 + k)] = static_cast<u8>(lo >> (56 - 8 * k));
        }
        return v;
    }
    // The string at `addr`: up to its NUL, at most `max` bytes (wide: characters).
    virtual std::optional<std::string> string(u32 addr, u64 max) = 0;
    virtual std::optional<std::u32string> wstring(u32 addr, u64 max) = 0;
    virtual bool store(u32 addr, u64 value, u32 bytes) = 0;
    std::vector<std::string> problems;
};

// ------------------------------------------------------------------------------------------------- _Pft

// _Pft's flags: the format's ' ', '+', '-', '#', '0' and Sony's separators ',', ';', ':', '_'; 0x20 marks text in an
// SPU's local store (the SPU printf's).
inline constexpr u16 kFsp = 0x1, kFpl = 0x2, kFmi = 0x4, kFno = 0x8, kFze = 0x10, kFlsText = 0x20, kFcomma = 0x80,
                     kFsemi = 0x100, kFcolon = 0x200, kFunder = 0x400;

// The print-formatting structure, its fields as the library's (the guest's layout is in sys_libc_format.cpp). P is
// where text lives: a host pointer, or a guest one for the internals called by a game. The field is put out as
// width - (the rest) spaces, n0 characters of the caller's buffer (sign, prefix), nz0 zeros, n1 characters at s, nz1
// zeros, n2 characters at s + n1, nz2 zeros (spaces after instead with '-').
template <class C, class P>
struct Pft {
    u64 v = 0;                // the integer (li) or the double's bits (ld)
    std::array<u8, 16> vec{}; // a vector argument
    P s{};
    s32 n0 = 0, nz0 = 0, n1 = 0, nz1 = 0, n2 = 0, nz2 = 0;
    s32 prec = -1, nchar = 0, width = 0;
    u16 flags = 0;
    C qual = 0;
};

// The library's int arithmetic (it wraps).
inline s32 add32(s32 a, s32 b) { return static_cast<s32>(static_cast<u32>(a) + static_cast<u32>(b)); }
inline s32 sub32(s32 a, s32 b) { return static_cast<s32>(static_cast<u32>(a) - static_cast<u32>(b)); }
inline s32 mul10add(s32 a, s32 d) { return static_cast<s32>(static_cast<u32>(a) * 10u + static_cast<u32>(d)); }

// fctiwz: toward zero, saturated, a NaN the minimum.
inline s32 fctiwz(double x)
{
    if (std::isnan(x)) return INT_MIN;
    if (x >= 2147483647.0) return INT_MAX;
    if (x <= -2147483648.0) return INT_MIN;
    return static_cast<s32>(x);
}

// ------------------------------------------------------------------------------------------------- _Litob

// An integer's digits at s (n1 of them) for d, i, o, u, x, X (p prints as x): v as a signed value for d and i,
// unsigned otherwise; no digit for 0 at precision 0; '#' with o puts a leading 0; the precision's zeros, or the '0'
// flag's when there is no precision and no '-', in nz0.
template <class C, class P>
void litob(Pft<C, P>& px, char code)
{
    C ac[24];
    const char* digs = code == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    const u64 base = code == 'o' ? 8 : (code != 'x' && code != 'X') ? 10 : 16;
    int i = 24;
    u64 ul = px.v;
    if ((code == 'd' || code == 'i') && static_cast<s64>(px.v) < 0) ul = 0 - ul;
    if (ul != 0 || px.prec != 0) ac[--i] = static_cast<C>(digs[ul % base]);
    s64 q = static_cast<s64>(ul / base);
    while (0 < q && 0 < i) {
        ac[--i] = static_cast<C>(digs[q % static_cast<s64>(base)]);
        q /= static_cast<s64>(base);
    }
    px.v = static_cast<u64>(q);
    if (base == 8 && (px.flags & kFno) && (i > 23 || ac[i] != static_cast<C>('0'))) ac[--i] = static_cast<C>('0');
    px.n1 = 24 - i;
    for (s32 k = 0; k < px.n1; k++) px.s[k] = ac[i + k];
    if (px.n1 < px.prec) {
        px.nz0 = px.prec - px.n1;
        px.flags = static_cast<u16>(px.flags & ~kFze);
    } else if (px.prec < 0 && (px.flags & (kFmi | kFze)) == kFze) {
        const s32 n = sub32(sub32(sub32(px.width, px.n0), px.nz0), px.n1);
        if (0 < n) px.nz0 = n;
    }
}

// ------------------------------------------------------------------------------------------------- _Genld

// The text of a floating conversion from nsig significant digits at p (characters; "0" when nsig <= 0) whose first
// is at the decimal exponent xexp (for a and A, the binary exponent): f-style for f and F, and for g and G when
// -4 <= xexp < the precision, e-style otherwise (p and P for a and A). The precision is the digits after the point
// (significant digits for g; '#' keeps trailing zeros and the point). Digits at s (n1), zeros (nz1), more at s + n1
// (n2), zeros (nz2); nz0 for the '0' flag.
template <class C, class P, class D>
void genld(Pft<C, P>& px, char code, D p, s16 nsig, s16 xexp)
{
    const C point = static_cast<C>('.'); // the "C" locale's decimal point
    const bool zero_digits = nsig <= 0;  // the library's "0" then
    if (zero_digits) nsig = 1;
    const auto digit = [&](s32 k) -> C { return zero_digits ? static_cast<C>('0') : static_cast<C>(p[k]); };
    const auto copy = [&](s32 at, s32 from, s32 n) {
        for (s32 k = 0; k < n; k++) px.s[at + k] = digit(from + k);
    };
    if (code == 'f' || code == 'F' || ((code == 'g' || code == 'G') && -4 <= xexp && xexp < px.prec)) {
        if (code == 'g' || code == 'G') {
            if (!(px.flags & kFno) && nsig < px.prec) px.prec = nsig;
            px.prec = sub32(px.prec, xexp + 1);
            if (px.prec < 0) px.prec = 0;
        }
        xexp = static_cast<s16>(xexp + 1); // the digits before the point
        if (xexp <= 0) {
            px.s[px.n1++] = static_cast<C>('0');
            if (0 < px.prec || (px.flags & kFno)) px.s[px.n1++] = point;
            if (px.prec < -xexp) xexp = static_cast<s16>(-px.prec);
            px.nz1 = -xexp;
            px.prec = add32(px.prec, xexp);
            if (px.prec < nsig) nsig = static_cast<s16>(px.prec);
            copy(px.n1, 0, nsig);
            px.n2 = nsig;
            px.nz2 = sub32(px.prec, nsig);
        } else if (nsig < xexp) {
            copy(px.n1, 0, nsig);
            px.n1 += nsig;
            px.nz1 = xexp - nsig;
            if (0 < px.prec || (px.flags & kFno)) {
                px.s[px.n1] = point;
                ++px.n2;
            }
            px.nz2 = px.prec;
        } else {
            copy(px.n1, 0, xexp);
            px.n1 += xexp;
            nsig = static_cast<s16>(nsig - xexp);
            if (0 < px.prec || (px.flags & kFno)) px.s[px.n1++] = point;
            if (px.prec < nsig) nsig = static_cast<s16>(px.prec);
            copy(px.n1, xexp, nsig);
            px.n1 += nsig;
            px.nz1 = sub32(px.prec, nsig);
        }
    } else {
        if (code == 'g' || code == 'G') {
            if (nsig < px.prec && !(px.flags & kFno)) px.prec = nsig;
            if (--px.prec < 0) px.prec = 0;
            code = code == 'g' ? 'e' : 'E';
        } else if (code == 'a') {
            code = 'p';
        } else if (code == 'A') {
            code = 'P';
        }
        px.s[px.n1++] = digit(0);
        if (0 < px.prec || (px.flags & kFno)) {
            px.s[px.n1++] = point;
            if (0 < px.prec) {
                s32 k = nsig - 1;
                if (px.prec < k) k = px.prec;
                copy(px.n1, 1, k);
                px.n1 += k;
                px.nz1 = sub32(px.prec, k);
            }
        }
        s32 e = px.n1;
        px.s[e++] = static_cast<C>(code);
        s32 x = xexp;
        if (0 <= x) {
            px.s[e++] = static_cast<C>('+');
        } else {
            px.s[e++] = static_cast<C>('-');
            x = static_cast<s16>(-x);
        }
        char buf[10];
        int n = 0;
        for (; 0 < x; ++n) {
            buf[n] = static_cast<char>(x % 10);
            x /= 10;
        }
        if (n < 2 && (code == 'e' || code == 'E')) px.s[e++] = static_cast<C>('0');
        if (n == 0) px.s[e++] = static_cast<C>('0');
        while (0 < n) px.s[e++] = static_cast<C>('0' + buf[--n]);
        px.n2 = e - px.n1;
    }
    if ((px.flags & (kFmi | kFze)) == kFze) {
        const s32 n = add32(add32(add32(add32(px.n0, px.n1), px.nz1), px.n2), px.nz2);
        if (n < px.width) px.nz0 = sub32(px.width, n);
    }
}

// ------------------------------------------------------------------------------------------------- _Ldtob

// Dinkumware's _Dunscale: *bits = f * 2^e with 0.5 <= |f| < 1, f replacing it; the class: 0 zero, -1 finite, 1
// infinite, 2 NaN (e 0 for those).
inline int dunscale(s16& e, u64& bits)
{
    const u32 ex = static_cast<u32>(bits >> 52) & 0x7FF;
    if (ex == 0x7FF) {
        e = 0;
        return (bits & 0xFFFFFFFFFFFFFull) ? 2 : 1;
    }
    if ((bits << 1) == 0) {
        e = 0;
        return 0;
    }
    int k = 0;
    const double f = std::frexp(std::bit_cast<double>(bits), &k);
    bits = std::bit_cast<u64>(f);
    e = static_cast<s16>(k);
    return -1;
}

// The powers of ten _Ldtob scales by: 10^(2^k).
inline constexpr double kTenPowers[9] = {1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256};

// A double's text for e, E, f, F, g, G, a, A (the sign is the caller's): "inf"/"nan" (upper case for E, F, G, A);
// "0x" for a and A after the caller's prefix; then _Genld on the digits generated as the library does (see the top).
// px.v is left holding the fraction _Dunscale made of a finite value, as the library's leaves it.
template <class C, class P>
void ldtob(Pft<C, P>& px, char code)
{
#pragma clang fp contract(off)
    double ldval = std::bit_cast<double>(px.v);
    if (code != 'a' && code != 'A') {
        if (px.prec < 0) px.prec = 6;
        else if (px.prec == 0 && (code == 'g' || code == 'G')) px.prec = 1;
    }
    s16 xexp = 0;
    const int errx = dunscale(xexp, px.v);
    if (errx == 2 || errx == 1) {
        const bool lower = code == 'a' || code == 'e' || code == 'f' || code == 'g';
        const char* t = errx == 2 ? (lower ? "nan" : "NAN") : (lower ? "inf" : "INF");
        px.n1 = 3;
        for (s32 k = 0; k < 3; k++) px.s[k] = static_cast<C>(t[k]);
        return;
    }
    if (code == 'a' || code == 'A') {
        px.s[0] = static_cast<C>('0');
        px.s[1] = static_cast<C>(code == 'a' ? 'x' : 'X');
        px.s = px.s + 2;
        px.n0 += 2;
    }
    // b[1] is the library's leading "0" before the digits, b[0] the byte before it (never '0' or '9': the library's
    // stack holds a remainder below 10 there); digits from b[2].
    char b[48] = {};
    char* q = b + 2;
    s32 nsig = 0;
    if (errx == 0) {
        xexp = 0;
    } else if (code == 'a' || code == 'A') {
        const char* digs = code == 'a' ? "0123456789abcdef" : "0123456789ABCDEF";
        s32 limit = 33, left = 34;
        nsig = 33;
        if (0 <= px.prec) {
            nsig = add32(px.prec, 1);
            limit = static_cast<s16>(nsig);
            left = limit + 1;
        }
        const double frac = std::bit_cast<double>(px.v);
        double x = std::ldexp(ldval < 0 ? -frac : frac, -3);
        xexp = static_cast<s16>(xexp - 1);
        b[1] = 0;
        char* d = b + 2;
        if (0 < left && 0 < x) {
            do {
                x = std::ldexp(x, 28);
                left -= 7;
                s32 lo = fctiwz(x);
                if (0 < left) x -= static_cast<double>(lo);
                if (lo <= 0) {
                    std::memset(d, 0, 7);
                } else {
                    for (int j = 6;; --j) {
                        d[j] = static_cast<char>(lo & 15);
                        lo >>= 4;
                        if (lo <= 0) {
                            std::memset(d, 0, static_cast<usize>(j));
                            break;
                        }
                        if (j == 0) break;
                    }
                }
                d += 7;
            } while (0 < left && 0 < x && d + 7 <= b + sizeof b);
        }
        const s32 gen = static_cast<s32>(d - (b + 2));
        if (limit > gen) nsig = gen;
        s16 ns = static_cast<s16>(nsig);
        if (0 <= ns) {
            const char drop = (ns < gen && q[ns] > 7) ? 15 : 0;
            s32 k = ns - 1;
            if (q[k] == drop) {
                s32 cnt = 0;
                do ++cnt;
                while (q[ns - 1 - cnt] == drop);
                k = ns - cnt - 1;
                nsig = ns - cnt;
            }
            if (drop == 15) ++q[k];
            if (k < 0) {
                q = b + 1;
                ++nsig;
                xexp = static_cast<s16>(xexp + 4);
            }
            ns = static_cast<s16>(nsig);
            for (s32 i = ns - 1; 0 <= i; --i) q[i] = digs[static_cast<u8>(q[i]) & 15];
        }
        if (px.prec < 0) px.prec = ns - 1;
    } else {
        if (ldval < 0) ldval = -ldval;
        xexp = static_cast<s16>(static_cast<s32>(xexp) * 30103 / 100000 - 4);
        if (xexp < 0) {
            s32 n = (3 - static_cast<s32>(xexp)) & ~3;
            xexp = static_cast<s16>(-n);
            for (int k = 0; 0 < n && k < 9; n >>= 1, ++k)
                if (n & 1) ldval *= kTenPowers[k];
        } else if (xexp != 0) {
            xexp = static_cast<s16>(static_cast<u16>(xexp) & 0xFFFC);
            double factor = 1.0;
            for (s32 n = xexp, k = 0; 0 < n && k < 9; n >>= 1, ++k)
                if (n & 1) factor *= kTenPowers[k];
            ldval /= factor;
        }
        s32 gen = add32((code == 'f' || code == 'F') ? xexp + 10 : 6, px.prec);
        if (gen > 19) gen = 19;
        b[1] = '0';
        char* d = b + 2;
        if (0 < gen && 0 < ldval) {
            do {
                s32 lo = fctiwz(ldval);
                gen -= 8;
                if (0 < gen) ldval = (ldval - static_cast<double>(lo)) * 1e8;
                if (lo <= 0) {
                    std::memset(d, '0', 8);
                } else {
                    for (int j = 7;; --j) {
                        d[j] = static_cast<char>('0' + lo % 10);
                        lo /= 10;
                        if (lo <= 0) {
                            std::memset(d, '0', static_cast<usize>(j));
                            break;
                        }
                        if (j == 0) break;
                    }
                }
                d += 8;
            } while (0 < gen && 0 < ldval && d + 8 <= b + sizeof b);
        }
        xexp = static_cast<s16>(xexp + 7);
        s32 g = static_cast<s32>(d - (b + 2));
        if (b[2] == '0') {
            do ++q;
            while (*q == '0');
            xexp = static_cast<s16>(xexp + static_cast<s32>((b + 2) - q));
            g = static_cast<s32>(d - q);
        }
        const s32 extra = (code == 'f' || code == 'F') ? xexp + 1 : (code == 'e' || code == 'E') ? 1 : 0;
        nsig = add32(px.prec, extra);
        s16 ns = static_cast<s16>(nsig);
        if (ns > g) {
            nsig = g;
            ns = static_cast<s16>(g);
        }
        if (0 <= ns) {
            const char drop = (ns < g && q[ns] > '4') ? '9' : '0';
            s32 k = ns - 1;
            if (q[k] == drop) {
                s32 cnt = 0;
                do ++cnt;
                while (q[ns - 1 - cnt] == drop);
                k = ns - cnt - 1;
                nsig -= cnt;
            }
            if (drop == '9') ++q[k];
            if (k < 0) {
                --q;
                ++nsig;
                xexp = static_cast<s16>(xexp + 1);
            }
        }
    }
    genld(px, code, static_cast<const char*>(q), static_cast<s16>(nsig), xexp);
}

// ------------------------------------------------------------------------------------------------- _Puttxt

// The field through the sink: padding, n0 characters of ac, zeros, the text at s; false when the sink stops (the
// library's pfn returned NULL). The sink adds what it puts out to px.nchar.
template <class C, class P, class Sink>
bool puttxt(Pft<C, P>& px, P ac, Sink& out)
{
    const s32 width = sub32(sub32(sub32(sub32(sub32(sub32(px.width, px.n0), px.nz0), px.n1), px.nz1), px.n2), px.nz2);
    if (!(px.flags & kFmi) && 0 < width && !out.pad(px, static_cast<C>(' '), width)) return false;
    if (0 < px.n0 && !out.put(px, ac, px.n0)) return false;
    if (0 < px.nz0 && !out.pad(px, static_cast<C>('0'), px.nz0)) return false;
    if (0 < px.n1 && !out.put(px, px.s, px.n1)) return false;
    if (0 < px.nz1 && !out.pad(px, static_cast<C>('0'), px.nz1)) return false;
    if (0 < px.n2 && !out.put(px, px.s + px.n1, px.n2)) return false;
    if (0 < px.nz2 && !out.pad(px, static_cast<C>('0'), px.nz2)) return false;
    if ((px.flags & kFmi) && 0 < width && !out.pad(px, static_cast<C>(' '), width)) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------- _Putstr, _WPutstr

// A malloc the library would get for _Putstr's buffer (the host's engine: one bigger than the PS3's memory fails).
inline constexpr u32 kMallocLimit = 0x10000000;

// The SDK's "C" locale converts wide characters and multibyte ones as UTF-8 (the game's copy encodes U+00E9 as C3 A9),
// by the bits alone: no check for surrogates or overlong forms. Its library was built with a 16-bit wchar_t (three
// bytes at most); for sys_libc's 32-bit one the same rule goes on to six bytes (31 bits), as UTF-8 was first defined.
// wctomb: the bytes of wc in out; their count, or -1 for a character past 31 bits.
inline s32 c_wctomb(char32_t wc, char* out)
{
    const u32 c = static_cast<u32>(wc);
    if (c < 0x80) {
        out[0] = static_cast<char>(c);
        return 1;
    }
    int n = c < 0x800 ? 2 : c < 0x10000 ? 3 : c < 0x200000 ? 4 : c < 0x4000000 ? 5 : c < 0x80000000u ? 6 : 0;
    if (!n) return -1;
    for (int k = n - 1; k > 0; --k) out[k] = static_cast<char>(0x80 | ((c >> (6 * (n - 1 - k))) & 0x3F));
    static constexpr u8 kLead[7] = {0, 0, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};
    out[0] = static_cast<char>(kLead[n] | (c >> (6 * (n - 1))));
    return n;
}

// mbtowc on the bytes of s from `at` (a NUL after them when `terminated`): the bytes taken (0 for a NUL character,
// whatever its length), or -1 for a byte that begins no character or a sequence cut short.
inline s32 c_mbtowc(const std::string& s, usize at, bool terminated, char32_t& wc)
{
    const auto byte = [&](usize k) -> s32 {
        if (k < s.size()) return static_cast<u8>(s[k]);
        return (k == s.size() && terminated) ? 0 : -1;
    };
    const s32 b0 = byte(at);
    if (b0 < 0) return -1;
    if (b0 < 0x80) {
        wc = static_cast<char32_t>(b0);
        return b0 ? 1 : 0;
    }
    const int n = b0 < 0xC0 ? 0 : b0 < 0xE0 ? 2 : b0 < 0xF0 ? 3 : b0 < 0xF8 ? 4 : b0 < 0xFC ? 5 : b0 < 0xFE ? 6 : 0;
    if (!n) return -1;
    u32 c = static_cast<u32>(b0) & (0x7Fu >> n);
    for (int k = 1; k < n; k++) {
        const s32 b = byte(at + static_cast<usize>(k));
        if (b < 0x80 || b >= 0xC0) return -1;
        c = (c << 6) | (static_cast<u32>(b) & 0x3F);
    }
    wc = static_cast<char32_t>(c);
    return c ? n : 0;
}

// _Putstr (%ls, %lc): wide characters (`w`, then a NUL when `terminated`; at most the precision of bytes are put out)
// as multibyte characters through a buffer from m.scratch (64 bytes; width + 3 when not '-' and wider), put out with
// _Puttxt when full (without padding) and at the end. -1 when a character has no multibyte form (after putting out
// what came before) or the buffer cannot be had; 0 otherwise, even when the sink stopped (the library ignores
// _Puttxt's answer here).
template <class P, class Sink, class M>
s32 putstr(Pft<char, P>& px, const std::u32string& w, bool terminated, Sink& out, M& m)
{
    s32 max = px.prec < 0 ? INT_MAX : px.prec;
    u32 bufsize = 64;
    if (!(px.flags & kFmi)) {
        const u32 want = static_cast<u32>(px.width) + 3;
        if (want > 64) bufsize = want;
    }
    const u32 need = std::min<u32>(bufsize, static_cast<u32>(std::min<usize>(w.size() + 1, 0x7FFFFFFF)));
    const std::optional<P> buf = m.scratch(bufsize, need);
    if (!buf) return -1;
    const P ac = *buf;
    s32 result = 0;
    if (0 < max) {
        for (usize i = 0;; ++i) {
            if (i >= w.size() && !terminated) break; // (the precision ends it first: at most max characters are read)
            const char32_t wc = i < w.size() ? w[i] : 0;
            char mb[8];
            const s32 n = c_wctomb(wc, mb);
            if (n < 0) {
                result = -1;
                break;
            }
            const s32 nn = wc == 0 ? n - 1 : n;
            if (nn > max) break;
            max -= nn;
            if (static_cast<u32>(px.n0 + nn) > bufsize) {
                px.width = 0;
                puttxt(px, ac, out);
                px.n0 = 0;
            }
            for (s32 k = 0; k < nn; k++) ac[px.n0++] = mb[k];
            if (wc == 0 || max <= 0) break;
        }
    }
    puttxt(px, ac, out);
    px.n0 = 0;
    px.width = 0;
    return result;
}

// _WPutstr (%s of the wide printf): multibyte characters (UTF-8) as wide ones, at most the precision of them, through
// a buffer of 64 characters (width when not '-' and wider), put out with _WPuttxt when full (without padding) and at
// the end; -1 for a byte sequence that is no character (after putting out what came before) or a buffer that cannot
// be had.
template <class P, class Sink, class M>
s32 wputstr(Pft<char32_t, P>& px, const std::string& s, bool terminated, Sink& out, M& m)
{
    s32 max = px.prec < 0 ? INT_MAX : px.prec;
    u32 bufsize = 64;
    if (!(px.flags & kFmi) && static_cast<u32>(px.width) > 64) bufsize = static_cast<u32>(px.width);
    const u32 need = std::min<u32>(bufsize, static_cast<u32>(std::min<usize>(s.size() + 1, 0x7FFFFFFF)));
    const std::optional<P> buf = m.scratch(bufsize, need);
    if (!buf) return -1;
    const P ac = *buf;
    s32 result = 0;
    if (max != 0) {
        for (usize at = 0;;) {
            if (static_cast<u32>(px.n0) >= bufsize) {
                px.width = 0;
                puttxt(px, ac, out);
                px.n0 = 0;
            }
            --max;
            char32_t wc = 0;
            const s32 n = c_mbtowc(s, at, terminated, wc);
            if (n < 0) {
                result = -1;
                break;
            }
            at += static_cast<usize>(n);
            ac[px.n0] = wc;
            if (n == 0) break;
            ++px.n0;
            if (max == 0) break;
        }
    }
    puttxt(px, ac, out);
    px.n0 = 0;
    px.width = 0;
    return result;
}

// ------------------------------------------------------------------------------------------------- _Putfld

namespace fmtdetail {

inline u64 be_at(const std::array<u8, 16>& v, usize at, usize n)
{
    u64 x = 0;
    for (usize k = 0; k < n; k++) x = (x << 8) | v[at + k];
    return x;
}

// After a vector element but the last: the separator (a space for the ' ' flag, else the first of , ; : _).
template <class C, class P>
void separator(Pft<C, P>& px, P ac)
{
    px.n0 = 0;
    const u16 f = px.flags;
    const char c = (f & kFsp) ? ' ' : (f & kFcomma) ? ',' : (f & kFsemi) ? ';' : (f & kFcolon) ? ':' : (f & kFunder) ? '_' : 0;
    if (c) ac[px.n0++] = static_cast<C>(c);
}

template <class C, class P>
void clear_text(Pft<C, P>& px)
{
    px.nz2 = px.n2 = px.nz1 = px.n1 = px.nz0 = 0;
}

inline bool is_vector_int(char q) { return q == 'v' || q == 'a' || q == 'c' || q == 'd' || q == 'g'; }

} // namespace fmtdetail

// The vector conversions of _Putfld (narrow only).
template <class P, class Sink, class M>
s32 put_vector(Pft<char, P>& px, M& m, char code, P ac, Sink& out)
{
    using namespace fmtdetail;
    px.vec = m.src().next_vector();
    const char q = static_cast<char>(px.qual);
    if (code == 'e' || code == 'E' || code == 'f' || code == 'F' || code == 'g' || code == 'G' || code == 'a' ||
        code == 'A') {
        const int count = q == 'v' ? 4 : 2;
        for (int k = 0;; ++k) {
            clear_text(px);
            if (q == 'v')
                px.v = std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(static_cast<u32>(be_at(px.vec, 4 * static_cast<usize>(k), 4)))));
            else
                px.v = be_at(px.vec, 8 * static_cast<usize>(k), 8);
            if (px.v >> 63) ac[px.n0++] = '-';
            else if (px.flags & kFpl) ac[px.n0++] = '+';
            else if (px.flags & kFsp) ac[px.n0++] = ' ';
            px.s = ac + px.n0;
            ldtob(px, code);
            if (!puttxt(px, ac, out)) return -1;
            if (k + 1 == count) return 0;
            separator(px, ac);
        }
    }
    if (code == 'c' && q == 'f') {
        for (int k = 0;; ++k) {
            clear_text(px);
            ac[px.n0++] = static_cast<char>(px.vec[static_cast<usize>(k)]);
            if (!puttxt(px, ac, out)) return -1;
            if (k == 15) return 0;
            separator(px, ac);
        }
    }
    const bool is_signed = code == 'd' || code == 'i';
    int count = 1, size = 0;
    if (is_signed) {
        if (q == 'g') count = 16, size = 1;
        else if (q == 'c') count = 8, size = 2;
        else if (q == 'v') count = 4, size = 4;
        else if (q == 'a') count = 2, size = 8;
    } else {
        if (q == 'd' || q == 'g') count = 16, size = 1;
        else if (q == 'c') count = 8, size = 2;
        else if (q == 'v') count = 4, size = 4;
        else if (q == 'a') count = 2, size = 8;
    }
    for (int k = 0;; ++k) {
        clear_text(px);
        if (size) {
            const u64 e = be_at(px.vec, static_cast<usize>(size * k), static_cast<usize>(size));
            if (is_signed && size < 8) {
                const int sh = 64 - 8 * size;
                px.v = static_cast<u64>(static_cast<s64>(e << sh) >> sh);
            } else {
                px.v = e;
            }
        }
        if (is_signed) {
            if (static_cast<s64>(px.v) < 0) ac[px.n0++] = '-';
            else if (px.flags & kFpl) ac[px.n0++] = '+';
        } else if ((px.flags & kFno) && px.v != 0 && (code == 'x' || code == 'X')) {
            ac[px.n0++] = '0';
            ac[px.n0++] = code;
        }
        px.s = ac + px.n0;
        litob(px, code);
        if (!puttxt(px, ac, out)) return -1;
        if (k + 1 == count) return 0;
        if (is_signed) {
            // a space for ' ', and then the first of , ; : _ besides
            px.n0 = 0;
            if (px.flags & kFsp) ac[px.n0++] = ' ';
            const u16 f = px.flags;
            const char c = (f & kFcomma) ? ',' : (f & kFsemi) ? ';' : (f & kFcolon) ? ':' : (f & kFunder) ? '_' : 0;
            if (c) ac[px.n0++] = c;
        } else {
            separator(px, ac);
        }
    }
}

// _Putfld: one conversion of the narrow printf with its argument from m.src(), into ac (n0 characters there already
// count as the field's prefix) and the text at px.s, put out through _Puttxt (or _Putstr). 0, or -1 when the sink
// stopped or a wide character has no multibyte form.
template <class P, class Sink, class M>
s32 putfld(Pft<char, P>& px, M& m, char code, P ac, Sink& out)
{
    using namespace fmtdetail;
    FormatSource& src = m.src();
    const auto field = [&]() -> s32 { return puttxt(px, ac, out) ? 0 : -1; };
    const char q = static_cast<char>(px.qual);
    switch (code) {
    case 'c':
        if (q == 'f' || q == 'd' || q == 'g') return put_vector(px, m, code, ac, out);
        if (q == 'l') {
            const char32_t wc = static_cast<char32_t>(static_cast<u32>(src.next_int(32)));
            px.prec = -1;
            return putstr(px, std::u32string(1, wc), true, out, m) < 0 ? -1 : 0;
        }
        ac[px.n0++] = static_cast<char>(src.next_int(32));
        return field();
    case 'S':
        px.qual = 'l';
        [[fallthrough]];
    case 's': {
        const u32 a = static_cast<u32>(src.next_int(32));
        if (px.qual == 'l') {
            const u64 limit = px.prec < 0 ? ~u64{0} : static_cast<u64>(px.prec);
            std::u32string w;
            bool terminated = true;
            if (!a) {
                w = U"(null)";
            } else if (const auto r = m.wide_chars(a, limit)) {
                w = *r;
                terminated = w.size() < limit;
            }
            return putstr(px, w, terminated, out, m) < 0 ? -1 : 0;
        }
        if (!a) {
            px.s = m.null_text();
            px.n1 = 6;
        } else if (!m.text_arg(a, px.prec, px.s, px.n1)) {
            px.n1 = 0;
        }
        return field();
    }
    case 'p':
        px.v = static_cast<u32>(src.next_int(32));
        px.s = ac + px.n0;
        litob(px, 'x');
        return field();
    case 'n': {
        const u32 a = static_cast<u32>(src.next_int(32));
        const u32 bytes = q == 'b' ? 1 : q == 'h' ? 2 : (q == 'j' || q == 'q') ? 8 : 4;
        if (!m.store(a, static_cast<u64>(static_cast<s64>(px.nchar)), bytes))
            src.problems.push_back("the %n argument is not memory (nothing stored)");
        return 0;
    }
    case 'd':
    case 'i':
        if (is_vector_int(q)) return put_vector(px, m, code, ac, out);
        px.v = (q == 'q' || q == 'j') ? src.next_int(64) : static_cast<u64>(static_cast<s64>(static_cast<s32>(src.next_int(32))));
        if (q == 'h') px.v = static_cast<u64>(static_cast<s64>(static_cast<s16>(px.v)));
        else if (q == 'b') px.v = static_cast<u64>(static_cast<s64>(static_cast<s8>(px.v)));
        else if (q == 't' || q == 'z') px.v = static_cast<u64>(static_cast<s64>(static_cast<s32>(px.v)));
        clear_text(px);
        if (static_cast<s64>(px.v) < 0) ac[px.n0++] = '-';
        else if (px.flags & kFpl) ac[px.n0++] = '+';
        else if ((px.flags & kFsp) &&
                 (q == 'b' || q == 'q' || q == 'j' || q == 't' || q == 'z' || q == 'h' || q == 'l' || q == 0))
            ac[px.n0++] = ' ';
        px.s = ac + px.n0;
        litob(px, code);
        return field();
    case 'o':
    case 'u':
    case 'x':
    case 'X':
        if (is_vector_int(q)) return put_vector(px, m, code, ac, out);
        px.v = (q == 'q' || q == 'j') ? src.next_int(64) : static_cast<u32>(src.next_int(32));
        if (q == 'h') px.v = static_cast<u16>(px.v);
        else if (q == 'b') px.v = static_cast<u8>(px.v);
        else if (q == 't' || q == 'z') px.v = static_cast<u32>(px.v);
        clear_text(px);
        if ((px.flags & kFno) && px.v != 0 && (code == 'x' || code == 'X')) {
            ac[px.n0++] = '0';
            ac[px.n0++] = code;
        }
        px.s = ac + px.n0;
        litob(px, code);
        return field();
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
        if (q == 'v' || q == 'e') return put_vector(px, m, code, ac, out);
        px.v = std::bit_cast<u64>(src.next_double());
        clear_text(px);
        if (px.v >> 63) ac[px.n0++] = '-';
        else if (px.flags & kFpl) ac[px.n0++] = '+';
        else if (px.flags & kFsp) ac[px.n0++] = ' ';
        px.s = ac + px.n0;
        ldtob(px, code);
        return field();
    default: // '%', and any character it does not convert: itself (a NUL as '%')
        ac[px.n0++] = code ? code : '%';
        return field();
    }
}

// The "C" locale's btowc (UTF-8): a byte below 0x80 is its wide character; EOF and the others have none (WEOF).
inline char32_t c_btowc(s32 c)
{
    const u8 b = static_cast<u8>(c);
    return (c == -1 || b >= 0x80) ? static_cast<char32_t>(0xFFFFFFFF) : static_cast<char32_t>(b);
}

// _WPutfld: one conversion of the wide printf into ac and px.s (the caller puts it out with _WPuttxt, except %s,
// which _WPutstr puts out). 0, or -1 for a byte with no wide character or a buffer _WPutstr cannot have.
template <class P, class Sink, class M>
s32 wputfld(Pft<char32_t, P>& px, M& m, char32_t code, P ac, Sink& out)
{
    FormatSource& src = m.src();
    const char32_t q = px.qual;
    switch (code) {
    case 'c':
        if (q == 'l') {
            ac[px.n0++] = static_cast<char32_t>(static_cast<u32>(src.next_int(32)));
        } else {
            const char32_t wc = c_btowc(static_cast<s32>(src.next_int(32)));
            if (wc == static_cast<char32_t>(0xFFFFFFFF)) return -1;
            ac[px.n0++] = wc;
        }
        return 0;
    case 's': {
        const u32 a = static_cast<u32>(src.next_int(32));
        if (q == 'l') {
            if (!a) {
                px.s = m.null_text();
                px.n1 = 6;
            } else if (!m.text_arg(a, px.prec, px.s, px.n1)) {
                px.n1 = 0;
            }
            return 0;
        }
        const u64 limit = px.prec < 0 ? ~u64{0} : 6 * static_cast<u64>(px.prec); // a character is six bytes at most
        std::string s;
        bool terminated = true;
        if (!a) {
            s = "(null)";
        } else if (const auto r = m.narrow_chars(a, limit)) {
            s = *r;
            terminated = s.size() < limit;
        }
        return wputstr(px, s, terminated, out, m) == -1 ? -1 : 0;
    }
    case 'p':
        px.v = static_cast<u32>(src.next_int(32));
        px.s = ac + px.n0;
        litob(px, 'x');
        return 0;
    case 'n': // the wide printf takes the pointer and stores nothing
        src.next_int(32);
        return 0;
    case 'd':
    case 'i':
        px.v = (q == 'q' || q == 'j') ? src.next_int(64) : static_cast<u64>(static_cast<s64>(static_cast<s32>(src.next_int(32))));
        if (q == 'h') px.v = static_cast<u64>(static_cast<s64>(static_cast<s16>(px.v)));
        else if (q == 'b') px.v = static_cast<u64>(static_cast<s64>(static_cast<s8>(px.v)));
        else if (q == 't' || q == 'z') px.v = static_cast<u64>(static_cast<s64>(static_cast<s32>(px.v)));
        if (static_cast<s64>(px.v) < 0) ac[px.n0++] = '-';
        else if (px.flags & kFpl) ac[px.n0++] = '+';
        else if (px.flags & kFsp) ac[px.n0++] = ' ';
        px.s = ac + px.n0;
        litob(px, static_cast<char>(code));
        return 0;
    case 'o':
    case 'u':
    case 'x':
    case 'X':
        px.v = (q == 'q' || q == 'j') ? src.next_int(64) : static_cast<u32>(src.next_int(32));
        if (q == 'h') px.v = static_cast<u16>(px.v);
        else if (q == 'b') px.v = static_cast<u8>(px.v);
        else if (q == 't' || q == 'z') px.v = static_cast<u32>(px.v);
        if ((px.flags & kFno) && px.v != 0 && (code == 'x' || code == 'X')) {
            ac[px.n0++] = '0';
            ac[px.n0++] = code;
        }
        px.s = ac + px.n0;
        litob(px, static_cast<char>(code));
        return 0;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
        px.v = std::bit_cast<u64>(src.next_double());
        if (px.v >> 63) ac[px.n0++] = '-';
        else if (px.flags & kFpl) ac[px.n0++] = '+';
        else if (px.flags & kFsp) ac[px.n0++] = ' ';
        px.s = ac + px.n0;
        ldtob(px, static_cast<char>(code));
        return 0;
    default:
        ac[px.n0++] = code ? code : static_cast<char32_t>('%');
        return 0;
    }
}

// ------------------------------------------------------------------------------------------------- _Printf, _WPrintf

namespace fmtdetail {

inline int flag_index(s64 c, bool narrow)
{
    switch (c) {
    case ' ': return 0;
    case '+': return 1;
    case '-': return 2;
    case '#': return 3;
    case '0': return 4;
    case ',': return narrow ? 5 : -1;
    case ';': return narrow ? 6 : -1;
    case ':': return narrow ? 7 : -1;
    case '_': return narrow ? 8 : -1;
    default: return -1;
    }
}
inline constexpr u16 kFlagBits[9] = {kFsp, kFpl, kFmi, kFno, kFze, kFcomma, kFsemi, kFcolon, kFunder};

} // namespace fmtdetail

// _Printf (C = char) or _WPrintf (C = char32_t): `fmt` formatted with the arguments of m.src() through the sink;
// `fp` is the format as P (literal text is put out from there) and `ac` a buffer of at least 64 characters. The
// characters put out (px.nchar), or -1 (EOF) when the sink stopped or a conversion failed. A format that ends inside
// a conversion ends there (the library reads past the format's NUL: undefined).
template <class C, class P, class Sink, class M>
s32 print_format(std::basic_string_view<C> fmt, P fp, M& m, Sink& out, P ac)
{
    using namespace fmtdetail;
    constexpr bool narrow = sizeof(C) == 1;
    FormatSource& src = m.src();
    Pft<C, P> px;
    px.nchar = 0;
    const usize size = fmt.size();
    const auto ch = [&](usize k) -> s64 { return k < size ? static_cast<s64>(static_cast<std::make_unsigned_t<C>>(fmt[k])) : 0; };
    usize i = 0;
    for (;;) {
        const usize start = i;
        while (i < size && fmt[i] != static_cast<C>('%')) ++i;
        if (i > start && !out.put(px, fp + static_cast<s32>(start), static_cast<s32>(i - start))) return -1;
        if (i >= size) return px.nchar;
        ++i;
        px.n0 = px.nz0 = px.n1 = px.nz1 = px.n2 = px.nz2 = 0;
        px.flags = 0;
        for (int f; i < size && (f = flag_index(ch(i), narrow)) >= 0; ++i) px.flags = static_cast<u16>(px.flags | kFlagBits[f]);
        if (ch(i) == '*') {
            s32 w = static_cast<s32>(src.next_int(32));
            if (w < 0) {
                w = sub32(0, w);
                px.flags = static_cast<u16>(px.flags | kFmi);
            }
            px.width = w;
            ++i;
        } else {
            s32 w = 0;
            for (; ch(i) >= '0' && ch(i) <= '9'; ++i)
                if (w != INT_MAX) w = mul10add(w, static_cast<s32>(ch(i) - '0'));
            px.width = w;
        }
        if (ch(i) == '.') {
            ++i;
            if (ch(i) == '*') {
                px.prec = static_cast<s32>(src.next_int(32));
                ++i;
            } else {
                s32 p = 0;
                for (; ch(i) >= '0' && ch(i) <= '9'; ++i)
                    if (p != INT_MAX) p = mul10add(p, static_cast<s32>(ch(i) - '0'));
                px.prec = p;
            }
        } else {
            px.prec = -1;
        }
        s64 qual = 0;
        usize conv_at = i;
        const s64 c0 = ch(i);
        if constexpr (narrow) {
            if (c0 == 'h' || c0 == 'j' || c0 == 'l' || c0 == 't' || c0 == 'z' || c0 == 'L' || c0 == 'v') {
                const s64 c1 = ch(i + 1), c2 = ch(i + 2);
                qual = c0;
                conv_at = i + 1;
                if (c0 == 'h') {
                    if (c1 == 'h') {
                        if (c2 == 'v') qual = 'g', conv_at = i + 3;
                        else qual = 'b', conv_at = i + 2;
                    } else if (c1 == 'v') {
                        qual = 'c', conv_at = i + 2;
                    }
                } else if (c0 == 'v') {
                    if (c1 == 'c') {
                        qual = 'f'; // the 'c' is the conversion
                    } else if (c1 == 'h') {
                        if (c2 == 'h') qual = 'g', conv_at = i + 3;
                        else qual = 'c', conv_at = i + 2;
                    } else if (c1 == 'l') {
                        if (c2 == 'l') qual = 'a', conv_at = i + 3;
                        else conv_at = i + 2;
                    } else if (c1 == 'L') {
                        qual = 'e', conv_at = i + 2;
                    }
                } else if (c0 == 'l') {
                    if (c1 == 'l') {
                        if (c2 == 'v') qual = 'a', conv_at = i + 3;
                        else qual = 'q', conv_at = i + 2;
                    } else if (c1 == 'v') {
                        qual = 'v', conv_at = i + 2;
                    }
                } else if (c0 == 'L') {
                    if (c1 == 'v') qual = 'e', conv_at = i + 2;
                }
            }
        } else {
            if (c0 == 'h' || c0 == 'j' || c0 == 'l' || c0 == 't' || c0 == 'z' || c0 == 'L') {
                qual = c0;
                conv_at = i + 1;
                if (c0 == 'h' && ch(i + 1) == 'h') qual = 'b', conv_at = i + 2;
                else if (c0 == 'l' && ch(i + 1) == 'l') qual = 'q', conv_at = i + 2;
            }
        }
        if (conv_at >= size) return px.nchar;
        s64 conv = ch(conv_at);
        usize next = conv_at + 1;
        if constexpr (narrow) {
            const auto uoxX = [](s64 c) { return c == 'u' || c == 'o' || c == 'x' || c == 'X'; };
            bool vector_default = qual == 'v' || qual == 'a' || qual == 'c' || qual == 'g' || qual == 'd' || qual == 'e';
            if (uoxX(conv)) {
                if (ch(next) == 'c') qual = 'd', vector_default = true;
            } else if (conv == 'c' && uoxX(ch(next))) {
                qual = 'd';
                conv_at = next;
                conv = ch(next);
                next = conv_at + 1;
                vector_default = true;
            }
            if (vector_default && !(px.flags & (kFsp | kFcomma | kFsemi | kFcolon | kFunder)))
                px.flags = static_cast<u16>(px.flags | kFsp);
            px.qual = static_cast<char>(qual);
            if (putfld(px, m, static_cast<char>(conv), ac, out) < 0) return -1;
            if (ch(next) == 'c' && px.qual == 'd') ++next;
        } else {
            px.qual = static_cast<char32_t>(qual);
            if (wputfld(px, m, static_cast<char32_t>(conv), ac, out) < 0) return -1;
            if (!puttxt(px, ac, out)) return -1;
        }
        i = next;
    }
}

// ------------------------------------------------------------------------------------------------- host text

// The engine on the host's memory: strings and text in host buffers, the arguments' strings read through the source.
template <class C>
class HostText {
public:
    explicit HostText(FormatSource& src) : src_(src) {}
    FormatSource& src() { return src_; }
    // %s (narrow) or %ls (wide): the argument's text, at most `prec` characters (all with prec < 0); false when it is
    // not memory (noted).
    bool text_arg(u32 addr, s32 prec, C*& s, s32& n)
    {
        const u64 max = prec < 0 ? ~u64{0} : static_cast<u64>(prec);
        if constexpr (sizeof(C) == 1) {
            auto t = src_.string(addr, max);
            if (!t) {
                src_.problems.push_back("a %s argument is not memory (printed as nothing)");
                return false;
            }
            held_ = std::move(*t);
        } else {
            auto t = src_.wstring(addr, max);
            if (!t) {
                src_.problems.push_back("a %ls argument is not memory (printed as nothing)");
                return false;
            }
            held_ = std::move(*t);
        }
        s = held_.data();
        n = static_cast<s32>(std::min<usize>(held_.size(), 0x7FFFFFFF));
        return true;
    }
    C* null_text()
    {
        static C text[7] = {'(', 'n', 'u', 'l', 'l', ')', 0};
        return text;
    }
    std::optional<std::u32string> wide_chars(u32 addr, u64 max)
    {
        auto r = src_.wstring(addr, max);
        if (!r) src_.problems.push_back("a %ls argument is not memory (printed as nothing)");
        return r;
    }
    std::optional<std::string> narrow_chars(u32 addr, u64 max)
    {
        auto r = src_.string(addr, max);
        if (!r) src_.problems.push_back("a %s argument is not memory (printed as nothing)");
        return r;
    }
    // _Putstr's buffer: `size` characters the library would allocate (more than the PS3 has fails), of which `need`
    // are used.
    std::optional<C*> scratch(u32 size, u32 need)
    {
        if (size > kMallocLimit) return std::nullopt;
        scratch_.assign(std::max<u32>(need, 1), C{});
        return scratch_.data();
    }
    bool store(u32 addr, u64 value, u32 bytes) { return src_.store(addr, value, bytes); }

private:
    FormatSource& src_;
    std::basic_string<C> held_;
    std::vector<C> scratch_;
};

// A sink collecting the text in a string (up to kMaxText characters; the count goes on past it).
template <class C>
class StringSink {
public:
    static constexpr usize kMaxText = 0x4000000;
    template <class PX>
    bool put(PX& px, const C* s, s32 n)
    {
        if (text.size() < kMaxText) text.append(s, std::min<usize>(static_cast<usize>(n), kMaxText - text.size()));
        px.nchar = add32(px.nchar, n);
        return true;
    }
    template <class PX>
    bool pad(PX& px, C c, s32 n)
    {
        if (text.size() < kMaxText) text.append(std::min<usize>(static_cast<usize>(n), kMaxText - text.size()), c);
        px.nchar = add32(px.nchar, n);
        return true;
    }
    std::basic_string<C> text;
};

// A sink with room for `cap` characters, as swprintf's output function: a piece that does not fit is written as far as
// it fits and stops it (EOF), and every piece after (the library's next call goes out with the NULL it got); padding
// goes 32 characters a piece (31 for the wide printf), each piece checked.
template <class C>
class BoundedSink {
public:
    explicit BoundedSink(usize cap) : room_(cap) {}
    template <class PX>
    bool put(PX& px, const C* s, s32 n)
    {
        if (failed) return false;
        if (static_cast<usize>(n) > room_) { // what fits goes in, then NULL
            text.append(s, room_);
            room_ = 0;
            failed = true;
            return false;
        }
        text.append(s, static_cast<usize>(n));
        room_ -= static_cast<usize>(n);
        px.nchar = add32(px.nchar, n);
        return true;
    }
    template <class PX>
    bool pad(PX& px, C c, s32 n)
    {
        constexpr s32 chunk = sizeof(C) == 1 ? 32 : 31;
        const std::basic_string<C> run(static_cast<usize>(chunk), c);
        for (; 0 < n; n -= chunk)
            if (!put(px, run.data(), std::min(n, chunk))) return false;
        return true;
    }
    std::basic_string<C> text;
    bool failed = false;

private:
    usize room_;
};

// printf's text on the host: the characters put out (truncated past StringSink::kMaxText) and the count, -1 for an
// encoding error (EILSEQ; the text then holds what came before it).
template <class C>
struct PrintResult {
    std::basic_string<C> text;
    s32 count = 0;
    bool failed() const { return count < 0; }
};

template <class C, class Sink = StringSink<C>>
PrintResult<C> print_host(std::basic_string_view<C> fmt, FormatSource& src, Sink out = Sink{})
{
    HostText<C> mem(src);
    C ac[128] = {};
    PrintResult<C> r;
    r.count = print_format<C>(fmt, const_cast<C*>(fmt.data()), mem, out, static_cast<C*>(ac));
    r.text = std::move(out.text);
    return r;
}

} // namespace pspocket::hle::libc
