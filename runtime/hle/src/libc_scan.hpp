#pragma once

// sys_libc's number parsing and formatted input as the SDK's C library has them (D-907). The SDK's library is
// Dinkumware's (P.J. Plauger's) C99 library; games link it into themselves, so a game's copy is the reference: the
// parsing below is Assassin's Creed's copy (an SDK of 2007), the scan engine Grand Theft Auto V's (2013: Assassin's
// Creed links no scanf). Both were read where they lie in the executables; this is pspocket's own code written from
// what they do, not a transcription. Nothing here touches guest memory: an `At` callable gives the i-th character of
// a string (-1 where memory ends), a ScanIo the characters a scanf reads, ScanArgs where it stores.
//
// The parsing (strtod, _Stodx and their kin):
// - _Stopfx: white space, a sign, then "nan" (an optional (n-char-sequence)), "inf"/"infinity", "0x" (hex) or a decimal
//   field. A NaN is never negative; a word that fails half way ("inx") gives 0 with the end pointer at the character
//   that failed, not rolled back; "0x" is hex whatever follows it.
// - _Stoflt and _Stoxflt: the digits as an array of longs, nine decimal or seven hex digits each, lo[0] the exponent.
//   Leading zeros are skipped; at most maxsig words of digits are kept: the first excess digit of the integer part that
//   is above 4 (hex: above 7) adds one to the last word, the excess digits of the fraction are dropped; the last word
//   is padded to full width and whole zero words are dropped. The exponent's digits stop counting at 100000000.
// - _Stodx: the words as a double (the last word plus the one before times 1e9, one fused multiply-add), then
//   _Dtentox: 10^n as a product of 10, 100, 1e4 ... 1e128 (8 bits of n), 1e256 for each 256 more, a negative n as one
//   over that product, applied to the double's fraction (frexp's) with one rounding and the exponent put back by
//   _Dscale, which truncates a denormal result. ERANGE (and *perr |= 1) only for a zero or infinite result. Hex: the
//   words times 2^28, _Dscale by the binary exponent, no range check. _Stofx: one word of digits, the same in float
//   (10 ... 1e16, 1e32 per 32 more).
// - _Stoulx: Dinkumware's: no errno for a bad base, "0x" taken whatever follows it (so "0x" alone converts nothing and
//   the end pointer is the string), ERANGE as well as *perr. _Stolx calls it after its own sign, so "-+5" is -5.
//
// The scan engine (_Scanf and _Getfld with its four conversions): the SDK's _Sft walks the format; GETN reads the next
// character while the field width (nget) lasts, else counts a pretended EOF; one character is pushed back through the
// caller's function. Notable choices: %c with fewer characters than its width stores what it got and succeeds; "0x"
// with no hex digit after it, or "1e" with no exponent digits, convert as much as they can ("1e" is 1.0); floats keep
// 36 significant digits and 8 exponent digits and go through _Stold (float targets get that double rounded); integers
// go through _Stoll/_Stoull and store their low bits (L stores 32 bits, ll and j 64); %C fails; a literal byte past 127
// in the format never matches (it is compared as a signed char); a matching failure at the end of input returns the
// count so far, an input failure before the first conversion EOF.

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::libc {

// ------------------------------------------------------------------------------------------------- classes

// The "C" locale's classes as the SDK's tables have them: nothing past 127 (nor -1, the end of readable memory).
inline bool sc_space(s64 c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool sc_digit(s64 c) { return c >= '0' && c <= '9'; }
inline bool sc_upper(s64 c) { return c >= 'A' && c <= 'Z'; }
inline bool sc_alnum(s64 c) { return sc_digit(c) || sc_upper(c) || (c >= 'a' && c <= 'z'); }
// A hex digit's value, or -1.
inline int sc_hex(s64 c)
{
    if (sc_digit(c)) return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}
inline s32 wrap_add(s32 a, s32 b) { return static_cast<s32>(static_cast<u32>(a) + static_cast<u32>(b)); }
inline s32 wrap_mul(s32 a, s32 b) { return static_cast<s32>(static_cast<u32>(a) * static_cast<u32>(b)); }

// ------------------------------------------------------------------------------------------------- scaling

// Dinkumware's classification codes (_Dtest's): what _Dscale and _Dunscale answer.
inline constexpr s16 kDkFinite = -1, kDkZero = 0, kDkInf = 1, kDkNan = 2;

template <class T>
struct DkFormat;
template <>
struct DkFormat<double> {
    using U = u64;
    static constexpr int kMant = 52;
    static constexpr s32 kMax = 0x7FF;  // the exponent field of an infinity
    static constexpr s32 kHalf = 0x3FE; // frexp's fraction's exponent field
};
template <>
struct DkFormat<float> {
    using U = u32;
    static constexpr int kMant = 23;
    static constexpr s32 kMax = 0xFF;
    static constexpr s32 kHalf = 0x7E;
};

// The value's bits split as _Dnorm leaves them: the 1 + kMant bit significand with its leading bit set and the
// exponent field it goes with (1 - the shift for a denormal). False for a zero.
template <class T>
bool dk_split(typename DkFormat<T>::U bits, typename DkFormat<T>::U& sig, s32& xchar)
{
    using F = DkFormat<T>;
    using U = typename F::U;
    constexpr U kFrac = (U{1} << F::kMant) - 1;
    xchar = static_cast<s32>((bits >> F::kMant) & static_cast<U>(F::kMax));
    sig = bits & kFrac;
    if (xchar != 0) {
        sig |= U{1} << F::kMant;
        return true;
    }
    if (sig == 0) return false;
    const int shift = std::countl_zero(sig) - (static_cast<int>(8 * sizeof(U)) - 1 - F::kMant);
    sig <<= shift;
    xchar = 1 - shift;
    return true;
}

// _Dscale (_FDscale, _LDscale): x * 2^xexp; an overflow gives the signed infinity, a denormal result is truncated (its
// bits shifted out are dropped), one below the smallest denormal a signed zero. NaNs and infinities stay.
template <class T>
s16 dk_scale(T& x, s32 xexp)
{
    using F = DkFormat<T>;
    using U = typename F::U;
    constexpr U kSign = U{1} << (8 * sizeof(U) - 1);
    constexpr U kFrac = (U{1} << F::kMant) - 1;
    const U bits = std::bit_cast<U>(x);
    const U sign = bits & kSign;
    if (((bits >> F::kMant) & static_cast<U>(F::kMax)) == static_cast<U>(F::kMax))
        return (bits & kFrac) ? kDkNan : kDkInf;
    U sig = 0;
    s32 xchar = 0;
    if (!dk_split<T>(bits, sig, xchar)) return kDkZero;
    const s32 lexp = wrap_add(xexp, xchar);
    if (xexp >= F::kMax || lexp >= F::kMax) {
        x = std::bit_cast<T>(static_cast<U>(sign | (static_cast<U>(F::kMax) << F::kMant)));
        return kDkInf;
    }
    if (lexp > 0) {
        x = std::bit_cast<T>(static_cast<U>(sign | (static_cast<U>(lexp) << F::kMant) | (sig & kFrac)));
        return kDkFinite;
    }
    const s32 shift = 1 - lexp; // lexp - 1 below -kMant: nothing is left
    if (shift > F::kMant) {
        x = std::bit_cast<T>(sign);
        return kDkZero;
    }
    x = std::bit_cast<T>(static_cast<U>(sign | (sig >> shift)));
    return kDkFinite;
}

// _Dunscale (_FDunscale, _LDunscale): x becomes its fraction in [1/2, 1), *e its exponent; a zero, infinity or NaN is
// left alone with *e = 0.
template <class T>
s16 dk_unscale(s16& e, T& x)
{
    using F = DkFormat<T>;
    using U = typename F::U;
    constexpr U kSign = U{1} << (8 * sizeof(U) - 1);
    constexpr U kFrac = (U{1} << F::kMant) - 1;
    const U bits = std::bit_cast<U>(x);
    e = 0;
    if (((bits >> F::kMant) & static_cast<U>(F::kMax)) == static_cast<U>(F::kMax))
        return (bits & kFrac) ? kDkNan : kDkInf;
    U sig = 0;
    s32 xchar = 0;
    if (!dk_split<T>(bits, sig, xchar)) return kDkZero;
    x = std::bit_cast<T>(static_cast<U>((bits & kSign) | (static_cast<U>(F::kHalf) << F::kMant) | (sig & kFrac)));
    e = static_cast<s16>(xchar - F::kHalf);
    return kDkFinite;
}

// One step of _Dtentox: x's fraction times `factor` (one rounding in T), its exponent put back by dk_scale.
template <class T>
s16 dk_tento_step(T factor, T& x)
{
    s16 e = 0;
    dk_unscale(e, x);
    x = x * factor;
    return dk_scale(x, e);
}

template <class T>
struct DkTens;
template <>
struct DkTens<double> {
    static constexpr int kBits = 8;
    static constexpr std::array<double, 8> kPow = {1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128};
    static constexpr double kBig = 1e256, kSmall = 1e-256;
};
template <>
struct DkTens<float> {
    static constexpr int kBits = 5;
    static constexpr std::array<float, 5> kPow = {1e1f, 1e2f, 1e4f, 1e8f, 1e16f};
    // The SDK's 1e-32 is one float below the nearest (0x0A4FB11E, not ...1F): its 1e-32 multiplications come out low.
    static constexpr float kBig = 1e32f, kSmall = std::bit_cast<float>(std::bit_cast<u32>(1e-32f) - 1);
};

// _Dtentox (_FDtentox, _LDtentox): x * 10^n as the SDK computes it; *range when the result is a zero or an infinity
// (the caller sets errno to ERANGE and *perr |= 1).
template <class T>
T dk_tento(T x, s32 n, bool* range)
{
    using P = DkTens<T>;
    if (range) *range = false;
    if (n == 0 || x == 0) return x;
    s16 r = 0;
    if (n > 0) {
        T factor = 1;
        s32 k = n;
        for (int i = 0; i < P::kBits; i++) {
            const bool bit = (k & 1) != 0;
            k >>= 1;
            if (bit) factor = factor * P::kPow[static_cast<usize>(i)];
            if (k <= 0) break;
        }
        r = dk_tento_step(factor, x);
        while (r < 0 && k > 0) {
            r = dk_tento_step(P::kBig, x);
            k--;
        }
    } else {
        u32 m = 0u - static_cast<u32>(n);
        T factor = 1;
        for (int i = 0; i < P::kBits; i++) {
            const bool bit = (m & 1) != 0;
            m >>= 1;
            if (bit) factor = factor * P::kPow[static_cast<usize>(i)];
            if (m == 0) break;
        }
        r = dk_tento_step(static_cast<T>(T{1} / factor), x);
        while (r < 0 && m != 0) {
            r = dk_tento_step(P::kSmall, x);
            m--;
        }
    }
    if (static_cast<u16>(r) <= 1 && range) *range = true;
    return x;
}

// ------------------------------------------------------------------------------------------------- the parsing steps

inline constexpr int kFlErr = 0, kFlDec = 1, kFlHex = 2, kFlInf = 3, kFlNan = 4, kFlNeg = 8;

// What _Stopfx leaves: the code (kFl*, kFlNeg for a minus), *ps (where the field's digits start) and whether it wrote
// *endptr itself (an infinity, a NaN or a failed word; for a decimal or hex field _Stoflt/_Stoxflt write it).
struct Prefix {
    int code = kFlErr;
    u64 pos = 0;
    bool sets_end = false;
};

template <class At>
Prefix sdk_stopfx(At& at, u64 s)
{
    u64 p = s;
    while (sc_space(at(p))) p++;
    int code = 0;
    s64 c = at(p);
    if (c == '-') {
        p++;
        code = kFlNeg;
    } else if (c == '+') {
        p++;
    }
    c = at(p);
    const auto is = [&](u64 i, char lower) { const s64 v = at(i); return v == lower || v == lower - 32; };
    if (c == 'n' || c == 'N') {
        if (!is(++p, 'a') || !is(++p, 'n')) return {kFlErr, p, true};
        u64 q = ++p;
        if (at(q) == '(') {
            do q++;
            while (sc_alnum(at(q)) || at(q) == '_');
        }
        if (at(q) == ')') p = q + 1; // ")" right after "nan" is taken too
        return {kFlNan, p, true};
    }
    if (c == 'i' || c == 'I') {
        if (!is(++p, 'n') || !is(++p, 'f')) return {kFlErr, p, true};
        ++p;
        if (is(p, 'i') && is(p + 1, 'n') && is(p + 2, 'i') && is(p + 3, 't') && is(p + 4, 'y')) p += 5;
        return {code | kFlInf, p, true};
    }
    if (c == '0' && (at(p + 1) == 'x' || at(p + 1) == 'X')) return {code | kFlHex, p + 2, false};
    return {code | kFlDec, p, false};
}

// _Stoflt's and _Stoxflt's array of longs (lo[0] the exponent), grown as written; `top` is the highest index written.
class DigitWords {
public:
    u32& operator[](u64 i)
    {
        if (i >= kInline && i - kInline >= more_.size()) more_.resize(static_cast<usize>(i - kInline + 1), 0);
        if (i > top_) top_ = i;
        return i < kInline ? small_[static_cast<usize>(i)] : more_[static_cast<usize>(i - kInline)];
    }
    u32 get(u64 i) const
    {
        if (i < kInline) return small_[static_cast<usize>(i)];
        return i - kInline < more_.size() ? more_[static_cast<usize>(i - kInline)] : 0;
    }
    u64 top() const { return top_; }

private:
    static constexpr u64 kInline = 8;
    std::array<u32, kInline> small_{};
    std::vector<u32> more_;
    u64 top_ = 0;
};

struct DigitsResult {
    s32 words = 0;
    u64 end = 0; // what *endptr is set to
};

// The words of the parsers' own calls (maxsig 1 or 2: indices up to maxsig).
struct FixedWords {
    std::array<u32, 4> w{};
    u32& operator[](u64 i) { return w[static_cast<usize>(i)]; }
    u32 get(u64 i) const { return w[static_cast<usize>(i)]; }
};

// _Stoflt (Hex false) and _Stoxflt (Hex true), from s (s0 the field's start, where *endptr rolls back to).
template <bool Hex, class At, class Words>
DigitsResult sdk_stoflt_t(At& at, u64 s0, u64 s, Words& lo, s32 maxsig)
{
    constexpr bool hex = Hex;
    constexpr s32 per = hex ? 7 : 9;          // digits in a word
    const s32 maxdig = wrap_mul(maxsig, per); // the digits kept
    const auto digit = [](s64 c) {
        if constexpr (Hex) return sc_hex(c);
        else return sc_digit(c) ? static_cast<int>(c - '0') : -1;
    };
    const auto push = [&](u64 pw, int d) {
        if constexpr (Hex) lo[pw] = (lo[pw] << 4) + static_cast<u32>(d);
        else lo[pw] = lo[pw] * 10 + static_cast<u32>(d);
    };
    bool seen = false;
    u64 p = s;
    if (at(p) == '0') {
        while (at(++p) == '0') {
        }
        seen = true;
    }
    lo[0] = 0;
    u64 pw = 0;
    s32 nd = 0;
    int d = digit(at(p));
    if (d >= 0) {
        for (;;) {
            if (maxdig > nd) {
                const s32 r0 = nd % per;
                nd++;
                if (r0 == 0) lo[++pw] = 0;
                push(pw, d);
            } else {
                lo[0] = lo[0] + 1;
                if (maxdig == nd && d > (hex ? 7 : 4)) { // the first excess digit above half rounds the last word up
                    lo[pw] = lo[pw] + 1;
                    nd = wrap_add(maxdig, 1);
                }
            }
            d = digit(at(++p));
            if (d < 0) break;
        }
        seen = true;
    }
    if (!(maxdig > nd)) nd = maxdig;
    if (at(p) == '.') p++;
    if (nd == 0 && at(p) == '0') {
        do {
            p++;
            lo[0] = lo[0] - 1;
        } while (at(p) == '0');
        seen = true;
    }
    d = digit(at(p));
    if (d >= 0) {
        do {
            if (maxdig > nd) {
                const s32 r0 = nd % per;
                nd++;
                if (r0 == 0) lo[++pw] = 0;
                push(pw, d);
                lo[0] = lo[0] - 1;
            }
            d = digit(at(++p));
        } while (d >= 0);
        seen = true;
    }
    if (const s32 r0 = nd % per; r0 != 0) { // pad the last word to its full width
        const s32 k = per - r0;
        if (hex) {
            lo[pw] = lo[pw] << (4 * k);
        } else if (lo.get(pw) != 0) {
            u32 scale = 1;
            for (s32 i = 0; i < k; i++) scale *= 10;
            lo[pw] = lo[pw] * scale;
        }
        nd = wrap_add(nd, k);
        lo[0] = lo[0] - static_cast<u32>(k);
    }
    if (nd > per - 1 && lo.get(pw) == 0) { // drop whole zero words
        u32 ctr = static_cast<u32>(nd - per) / static_cast<u32>(per) + 1;
        do {
            if (pw == 0) break;
            pw--;
            lo[0] = lo[0] + static_cast<u32>(per);
            nd -= per;
        } while (--ctr != 0 && lo.get(pw) == 0);
    }
    if (hex) lo[0] = lo[0] << 2;
    u64 back = s0;
    const s64 e = at(p);
    if (seen && (hex ? (e == 'p' || e == 'P') : (e == 'e' || e == 'E'))) {
        u64 q = p + 1;
        s64 sign = at(q);
        if (hex || sign != 0) {
            if (sign == '+' || sign == '-') q++;
            else sign = '+';
            bool eseen = false;
            s32 lexp = 0;
            if (sc_digit(at(q))) {
                for (;;) {
                    if (lexp <= 99999999) lexp = lexp * 10 + static_cast<s32>(at(q) - '0');
                    q++;
                    if (!sc_digit(at(q))) break;
                }
                eseen = true;
            }
            if (sign == '-') lexp = -lexp;
            lo[0] = lo[0] + static_cast<u32>(lexp);
            back = p;
            p = q;
            seen = eseen;
        }
    }
    return {nd / per, seen ? p : back};
}

template <class At, class Words>
DigitsResult sdk_stoflt(At& at, u64 s0, u64 s, Words& lo, s32 maxsig, bool hex)
{
    return hex ? sdk_stoflt_t<true>(at, s0, s, lo, maxsig) : sdk_stoflt_t<false>(at, s0, s, lo, maxsig);
}

template <class T>
struct ParseResult {
    T value = 0;
    u64 end = 0;        // what *endptr is set to
    bool range = false; // ERANGE (errno, and *perr |= 1)
};

// _Stodx (_Stoldx: long double is double) on the string at s: the value, the end and the range error.
template <class At>
ParseResult<double> sdk_stodx(At& at, u64 s, s32 pten)
{
    ParseResult<double> r;
    const Prefix pf = sdk_stopfx(at, s);
    if (pf.sets_end) r.end = pf.pos;
    double x = 0;
    const int kind = pf.code & ~kFlNeg;
    if (kind == kFlDec || kind == kFlHex) {
        FixedWords lo;
        const DigitsResult d = sdk_stoflt(at, s, pf.pos, lo, 2, kind == kFlHex);
        r.end = d.end;
        if (d.words != 0) {
            const double step = kind == kFlHex ? 268435456.0 : 1e9; // 2^28, 10^9: the next word up
            x = static_cast<double>(static_cast<s32>(lo.get(static_cast<u64>(d.words))));
            double weight = step;
            for (s32 k = d.words - 1; k >= 1; k--) {
                const s32 w = static_cast<s32>(lo.get(static_cast<u64>(k)));
                if (w != 0) x = std::fma(static_cast<double>(w), weight, x);
                weight = weight * step;
            }
        }
        if (kind == kFlDec) {
            x = dk_tento(x, wrap_add(static_cast<s32>(lo.get(0)), pten), &r.range);
        } else {
            dk_scale(x, static_cast<s32>(lo.get(0)));
            x = dk_tento(x, pten, &r.range);
        }
    } else if (kind == kFlInf) {
        x = std::numeric_limits<double>::infinity();
    } else if (kind == kFlNan) {
        x = std::bit_cast<double>(u64{0x7FF8000000000000});
    }
    r.value = (pf.code & kFlNeg) ? -x : x;
    return r;
}

// _Stofx: one word of digits, scaled in float.
template <class At>
ParseResult<float> sdk_stofx(At& at, u64 s, s32 pten)
{
    ParseResult<float> r;
    const Prefix pf = sdk_stopfx(at, s);
    if (pf.sets_end) r.end = pf.pos;
    float x = 0;
    const int kind = pf.code & ~kFlNeg;
    if (kind == kFlDec || kind == kFlHex) {
        FixedWords lo;
        const DigitsResult d = sdk_stoflt(at, s, pf.pos, lo, 1, kind == kFlHex);
        r.end = d.end;
        if (d.words != 0) x = static_cast<float>(static_cast<s32>(lo.get(1)));
        if (kind == kFlDec) {
            x = dk_tento(x, wrap_add(static_cast<s32>(lo.get(0)), pten), &r.range);
        } else {
            dk_scale(x, static_cast<s32>(lo.get(0)));
            x = dk_tento(x, pten, &r.range);
        }
    } else if (kind == kFlInf) {
        x = std::numeric_limits<float>::infinity();
    } else if (kind == kFlNan) {
        x = std::bit_cast<float>(u32{0x7FC00000});
    }
    r.value = (pf.code & kFlNeg) ? -x : x;
    return r;
}

// ------------------------------------------------------------------------------------------------- integers

struct IntResult {
    u64 value = 0;
    u64 end = 0;        // what *endptr is set to
    bool range = false; // ERANGE (errno, and *perr = 1)
};

// _Stoulx (bits 32) and _Stoullx (64): an unsigned long (long long) with Dinkumware's subject sequence.
template <class At>
IntResult sdk_stoulx(At& at, u64 s, s32 base, u32 bits)
{
    IntResult r;
    r.end = s;
    const u64 umax = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    u64 p = s;
    while (sc_space(at(p))) p++;
    const s64 sign = at(p);
    const bool minus = sign == '-';
    if (sign == '-' || sign == '+') p++;
    if (base < 0 || base == 1 || base > 36) return r;
    const auto x_at = [&](u64 i) { const s64 c = at(i); return c == 'x' || c == 'X'; };
    if (base == 0) {
        if (at(p) != '0') base = 10;
        else if (x_at(p + 1)) {
            base = 16;
            p += 2;
        } else {
            base = 8;
        }
    } else if (base == 16 && at(p) == '0' && x_at(p + 1)) {
        p += 2;
    }
    const u64 s1 = p; // no digit from here on: nothing converted
    while (at(p) == '0') p++;
    u64 x = 0;
    bool over = false;
    for (;;) {
        s64 c = at(p);
        if (sc_upper(c)) c += 32;
        int dig = -1;
        if (sc_digit(c)) dig = static_cast<int>(c - '0');
        else if (c >= 'a' && c <= 'z') dig = static_cast<int>(c - 'a' + 10);
        if (dig < 0 || dig >= base) break;
        using U128 = unsigned __int128;
        const U128 next = static_cast<U128>(x) * static_cast<u64>(base) + static_cast<u64>(dig);
        if (next > umax) over = true;
        x = static_cast<u64>(next) & umax;
        p++;
    }
    if (p == s1) return r;
    r.end = p;
    if (over) {
        r.range = true;
        r.value = umax;
    } else {
        r.value = minus ? (0 - x) & umax : x;
    }
    return r;
}

// _Stolx (32) and _Stollx (64): its own white space and sign, then _Stoulx on the rest.
template <class At>
IntResult sdk_stolx(At& at, u64 s, s32 base, u32 bits)
{
    const u64 umax = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u64 smin = (umax >> 1) + 1; // the magnitude of the most negative value
    u64 p = s;
    while (sc_space(at(p))) p++;
    const s64 c = at(p);
    const bool minus = c == '-';
    const u64 from = (c == '-' || c == '+') ? p + 1 : p;
    IntResult u = sdk_stoulx(at, from, base, bits);
    IntResult r;
    r.range = u.range;
    r.end = u.end;
    u64 x = u.value;
    if (u.end == from || u.end == s) { // nothing converted
        r.end = s;
        if (x != 0) {
            r.range = true;
            r.value = minus ? smin : smin - 1;
            return r;
        }
        x = 0;
    }
    if (!minus) {
        if (x >= smin) {
            r.range = true;
            r.value = smin - 1;
            return r;
        }
        r.value = x;
    } else {
        if (x > smin) {
            r.range = true;
            r.value = smin;
            return r;
        }
        r.value = (0 - x) & umax;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- the scan engine

// The caller's input function, int pfn(void *arg, int ch, int getfl): get() is pfn(arg, 0, 1) (a character or -1,
// EOF), unget(c) pfn(arg, c, 0). The engine passes back exactly what get() gave.
class ScanIo {
public:
    virtual ~ScanIo() = default;
    virtual s32 get() = 0;
    virtual void unget(s32 c) = 0;
};

// Where a conversion stores: va_arg(px->ap, T *) and the bytes through it, and errno's ERANGE from the conversions.
class ScanArgs {
public:
    virtual ~ScanArgs() = default;
    virtual u32 next_pointer() = 0;
    virtual bool put(u32 addr, const u8* data, usize size) = 0;
    virtual void range_error() {}
    std::vector<std::string> problems;

    void put_int(u32 addr, u64 v, u32 bytes, char conv)
    {
        u8 b[8];
        for (u32 k = 0; k < bytes; k++) b[k] = static_cast<u8>(v >> (8 * (bytes - 1 - k)));
        if (!put(addr, b, bytes)) problem(conv, addr);
    }
    void problem(char conv, u32 addr)
    {
        problems.push_back(std::format("the %{} argument {:#x} is not memory", conv, addr));
    }
};

// What the narrow engine (C = char) and the wide one (C = char32_t, derived from it: no copy exists in a game) take
// differently. Narrow: characters are what the input function returns, classes on their low byte as the SDK's table
// lookups have them, a format byte compared as a signed char. Wide: wint_t values, the classes of ASCII only.
template <class C>
struct ScanChars;
template <>
struct ScanChars<char> {
    static bool space(s32 ch) { return sc_space(ch & 0xFF); }
    static bool digit_class(s32 ch) { return sc_digit(ch & 0xFF); }
    static bool nan_char(s32 ch) { return (ch >= 0 && ch < 256 && sc_alnum(ch)) || ch == '_'; }
    // memchr("0123456789abcdefABCDEF", ch, dlen)
    static bool in_digits(s32 ch, int dlen) { return in_list(static_cast<u8>(ch), dlen); }
    static bool literal(char f, s32 ch) { return static_cast<s32>(static_cast<s8>(f)) == ch; }
    static bool fmt_space(char f) { return sc_space(static_cast<u8>(f)); }
    static bool fmt_digit(char f) { return sc_digit(static_cast<u8>(f)); }
    static u32 set_char(s32 ch) { return static_cast<u8>(ch); }
    static u32 fmt_char(char f) { return static_cast<u8>(f); }
    static bool in_list(u32 v, int dlen)
    {
        static constexpr char kDigits[] = "0123456789abcdefABCDEF";
        for (int i = 0; i < dlen; i++)
            if (static_cast<u8>(kDigits[i]) == v) return true;
        return false;
    }
};
template <>
struct ScanChars<char32_t> {
    static bool space(s32 ch) { return sc_space(ch); }
    static bool digit_class(s32 ch) { return sc_digit(ch); }
    static bool nan_char(s32 ch) { return sc_alnum(ch) || ch == '_'; }
    static bool in_digits(s32 ch, int dlen) { return ch >= 0 && ScanChars<char>::in_list(static_cast<u32>(ch), dlen); }
    static bool literal(char32_t f, s32 ch) { return static_cast<u32>(f) == static_cast<u32>(ch); }
    static bool fmt_space(char32_t f) { return sc_space(static_cast<s64>(f)); }
    static bool fmt_digit(char32_t f) { return sc_digit(static_cast<s64>(f)); }
    static u32 set_char(s32 ch) { return static_cast<u32>(ch); }
    static u32 fmt_char(char32_t f) { return static_cast<u32>(f); }
};

// The SDK's _Sft (_WSft for the wide engine): the format's place, the counts and the conversion's settings.
template <class C>
struct Sft {
    ScanIo* io = nullptr;
    ScanArgs* args = nullptr;
    const C* s = nullptr; // the format at the conversion character, NUL-terminated (a second NUL after it)
    s32 nchar = 0, nget = 0, width = 0;
    u8 noconv = 0, qual = 0, stored = 0;

    s32 get()
    {
        nchar = wrap_add(nchar, 1);
        return io->get();
    }
    s32 getn() // the next character while the field lasts, else a pretended EOF
    {
        nget = wrap_add(nget, -1);
        nchar = wrap_add(nchar, 1);
        return nget >= 0 ? io->get() : -1;
    }
    void unget(s32 c)
    {
        nchar = wrap_add(nchar, -1);
        io->unget(c);
    }
    void ungetn(s32 c)
    {
        if (c != -1) unget(c);
        else nchar = wrap_add(nchar, -1);
    }
    u32 pointer() { return args->next_pointer(); }
};

// _Getint: %d %i %o %u %x %X %p. The text goes into a buffer of 31 characters (sign and digits, leading zeros as one)
// and through _Stoll or _Stoull.
template <class C>
int sdk_getint(Sft<C>& px)
{
    using Ch = ScanChars<C>;
    char ac[32];
    usize p = 0;
    const u32 conv = static_cast<u32>(*px.s);
    int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
    px.nget = px.width > 0 ? px.width : std::numeric_limits<s32>::max();
    s32 ch = px.getn();
    if (ch == '+' || ch == '-') {
        ac[p++] = static_cast<char>(ch);
        ch = px.getn();
    }
    bool seen = false, zeros = false;
    int dlen = 10;
    const auto dlen_of = [](int b) { return b == 0 || b == 10 ? 10 : b == 8 ? 8 : 22; };
    if (ch == '0') {
        ch = px.getn();
        if ((ch == 'x' || ch == 'X') && (base == 0 || base == 16)) {
            base = 16;
            dlen = 22;
            ch = px.getn();
            zeros = ch == '0';
        } else {
            if (base == 0) base = 8;
            seen = true;
            dlen = dlen_of(base);
            zeros = ch == '0';
        }
    } else {
        dlen = dlen_of(base);
    }
    if (zeros) {
        do ch = px.getn();
        while (ch == '0');
        seen = true;
    }
    if (seen) ac[p++] = '0';
    while (ch != -1) {
        if (!Ch::in_digits(ch, dlen)) {
            px.unget(ch);
            goto done;
        }
        if (p < 31) ac[p++] = static_cast<char>(ch);
        seen = true;
        ch = px.getn();
    }
    px.nchar = wrap_add(px.nchar, -1);
done:
    if (!seen) return p == 0 && ch == -1 ? -1 : 0;
    ac[p] = 0;
    if (px.noconv) return 1;
    auto at = [&](u64 k) -> s64 { return k <= p ? static_cast<u8>(ac[k]) : -1; };
    const bool is_signed = conv == 'd' || conv == 'i';
    const IntResult v = is_signed ? sdk_stolx(at, 0, base, 64) : sdk_stoulx(at, 0, base, 64);
    if (v.range) px.args->range_error();
    px.stored = 1;
    const bool wide64 = px.qual == 'j' || px.qual == 'q';
    const u32 bytes = conv == 'p' ? 4 : px.qual == 'b' ? 1 : px.qual == 'h' ? 2 : wide64 ? 8 : 4;
    const u32 addr = px.pointer();
    px.args->put_int(addr, v.value, bytes, static_cast<char>(conv));
    return 1;
}

// _Getfloat: %a %e %f %g (and capitals). The text goes into a buffer (36 significant digits, 8 exponent digits, the
// rest counted in a power of ten or dropped) and through _Stold.
template <class C>
int sdk_getfloat(Sft<C>& px)
{
    using Ch = ScanChars<C>;
    char buf[80];
    usize p = 0;
    px.nget = px.width > 0 ? px.width : std::numeric_limits<s32>::max();
    s32 ch = px.getn();
    if (ch == '+' || ch == '-') {
        buf[p++] = static_cast<char>(ch);
        ch = px.getn();
    }
    bool seen = false;
    int dlen = 10;
    s32 pten = 0;
    const auto word = [&](const char* rest) { // after "nan" or "inf": true when the word matched
        for (; *rest; rest++) {
            ch = px.getn();
            if (ch != *rest && ch != *rest - 32) return false;
        }
        return true;
    };
    const auto fail_at = [&] { // the character that ended a word that failed: pushed back
        px.ungetn(ch);
        return p == 0 && ch == -1 ? -1 : 0;
    };
    if (ch == 'n' || ch == 'N') {
        buf[p++] = 'n';
        if (!word("a")) return fail_at();
        buf[p++] = 'a';
        if (!word("n")) return fail_at();
        ch = px.getn();
        if (ch == '(') {
            do ch = px.getn();
            while (Ch::nan_char(ch));
            if (ch != ')') return fail_at();
        } else {
            px.ungetn(ch);
        }
        buf[p++] = 'n';
        seen = true;
        dlen = 0;
        goto convert;
    }
    if (ch == 'i' || ch == 'I') {
        buf[p++] = 'i';
        if (!word("n")) return fail_at();
        buf[p++] = 'n';
        if (!word("f")) return fail_at();
        ch = px.getn();
        if (ch == 'i' || ch == 'I') {
            if (!word("nity")) return fail_at();
        } else {
            px.ungetn(ch);
        }
        buf[p++] = 'f';
        seen = true;
        dlen = 0;
        goto convert;
    }
    if (ch == '0') {
        ch = px.getn();
        if (ch == 'x' || ch == 'X') {
            buf[p++] = '0';
            buf[p++] = 'x';
            dlen = 22;
            ch = px.getn();
        } else {
            seen = true;
        }
    }
    if (ch == '0') {
        do ch = px.getn();
        while (ch == '0');
        seen = true;
    }
    if (seen) buf[p++] = '0';
    {
        s32 ndig = 0;
        while (ch != -1 && Ch::in_digits(ch, dlen)) {
            if (ndig < 36) {
                buf[p++] = static_cast<char>(ch);
                ndig++;
            } else {
                pten++;
            }
            seen = true;
            ch = px.getn();
        }
        if (ch == '.') {
            buf[p++] = '.';
            ch = px.getn();
        }
        if (ndig == 0) {
            if (ch == '0') {
                do {
                    pten--;
                    ch = px.getn();
                } while (ch == '0');
                seen = true;
            }
            if (pten < 0) {
                buf[p++] = '0';
                pten++;
            }
        }
        while (ch != -1 && Ch::in_digits(ch, dlen)) {
            if (ndig < 36) {
                buf[p++] = static_cast<char>(ch);
                ndig++;
            }
            seen = true;
            ch = px.getn();
        }
    }
    if (seen && (dlen == 10 ? (ch == 'e' || ch == 'E') : (ch == 'p' || ch == 'P'))) {
        buf[p++] = static_cast<char>(ch);
        ch = px.getn();
        if (ch == '+' || ch == '-') {
            buf[p++] = static_cast<char>(ch);
            ch = px.getn();
        }
        if (ch == '0') {
            do ch = px.getn();
            while (ch == '0');
            buf[p++] = '0';
        }
        for (s32 edig = 0; Ch::digit_class(ch);) {
            if (edig <= 7) {
                buf[p++] = static_cast<char>(ch);
                edig++;
            }
            ch = px.getn();
        }
    }
    px.ungetn(ch);
    if (!seen) return p == 0 && ch == -1 ? -1 : 0;
convert:
    buf[p] = 0;
    if (px.noconv) return 1;
    {
        auto at = [&](u64 k) -> s64 { return k <= p ? static_cast<u8>(buf[k]) : -1; };
        ParseResult<double> v;
        if (dlen > 10) {
            v = sdk_stodx(at, 0, 0);
            dk_scale(v.value, wrap_mul(pten, 4));
        } else {
            v = sdk_stodx(at, 0, pten);
        }
        if (v.range) px.args->range_error();
        px.stored = 1;
        const u32 addr = px.pointer();
        const char conv = static_cast<char>(*px.s);
        if (px.qual == 'l' || px.qual == 'L') px.args->put_int(addr, std::bit_cast<u64>(v.value), 8, conv);
        else px.args->put_int(addr, std::bit_cast<u32>(static_cast<float>(v.value)), 4, conv);
    }
    return 1;
}

// A %[ set's test, ranges "a-z" where a '-' is neither the set's first nor its last character.
template <class C>
bool scanset_has(const C* set, usize n, u32 ch, bool ranges)
{
    using Ch = ScanChars<C>;
    usize i = 0;
    if (ranges) {
        while (n - i > 2) {
            if (Ch::fmt_char(set[i + 1]) == '-') {
                if (Ch::fmt_char(set[i]) <= ch && ch <= Ch::fmt_char(set[i + 2])) return true;
                i += 3;
            } else {
                if (Ch::fmt_char(set[i]) == ch) return true;
                i++;
            }
        }
    }
    for (; i < n; i++)
        if (Ch::fmt_char(set[i]) == ch) return true;
    return false;
}

// _Getstr: %s (sfl 1), %c (0) and %[ (-1); with l the characters are stored as wide ones (a narrow input's byte as the
// wide character of its value, the "C" locale's; the SDK's narrow form also counts each down from the width), without l
// as bytes (the wide engine: a wide character with no byte form, past 255, is a matching failure).
template <class C>
int sdk_getstr(Sft<C>& px, int sfl)
{
    using Ch = ScanChars<C>;
    const bool isc = sfl == 0;
    bool invert = false, ranges = false;
    const C* set = nullptr;
    usize nset = 0;
    if (sfl < 0) {
        px.s++;
        if (*px.s == '^') {
            px.s++;
            invert = true;
        }
        const C* t = px.s + (*px.s == ']' ? 1 : 0);
        while (*t && *t != ']') t++;
        if (!*t) return 0;
        set = px.s;
        nset = static_cast<usize>(t - px.s);
        px.s = t;
        if (nset > 2)
            for (usize k = 1; k + 1 < nset; k++)
                if (Ch::fmt_char(set[k]) == '-') ranges = true;
    }
    px.nget = px.width > 0 ? px.width : isc ? 1 : std::numeric_limits<s32>::max();
    const bool wide_dest = px.qual == 'l';
    u32 dest = 0;
    if (!px.noconv) dest = px.pointer();
    int seen = 0; // 2: a character was taken
    std::vector<u8> bytes;
    s32 ch;
    for (;;) {
        ch = px.getn();
        if (ch == -1) break;
        if (sfl > 0 && Ch::space(ch)) {
            px.unget(ch);
            goto end;
        }
        if (sfl < 0 && scanset_has(set, nset, Ch::set_char(ch), ranges) == invert) {
            px.unget(ch);
            goto end;
        }
        if (wide_dest) {
            const u32 wc = sizeof(C) == 1 ? static_cast<u8>(ch) : static_cast<u32>(ch);
            if (!px.noconv) {
                for (int k = 3; k >= 0; k--) bytes.push_back(static_cast<u8>(wc >> (8 * k)));
                px.stored = 1;
            }
            if constexpr (sizeof(C) == 1) px.width = wrap_add(px.width, -1);
        } else {
            if constexpr (sizeof(C) == 4) {
                if (static_cast<u32>(ch) > 0xFF) return 0; // no multibyte form in the "C" locale
            }
            if (!px.noconv) {
                bytes.push_back(static_cast<u8>(ch));
                px.stored = 1;
            }
        }
        seen = 2;
    }
    px.nchar = wrap_add(px.nchar, -1);
end:
    if (seen == 2) {
        if (!isc && !px.noconv) {
            if (wide_dest) bytes.insert(bytes.end(), 4, 0);
            else bytes.push_back(0);
        }
        if (!px.noconv && !px.args->put(dest, bytes.data(), bytes.size()))
            px.args->problem(static_cast<char>(sfl > 0 ? 's' : isc ? 'c' : '['), dest);
        return 1;
    }
    return ch == -1 ? -1 : 0;
}

// _Getfld: the conversion at px.s (stored cleared first).
template <class C>
int sdk_getfld(Sft<C>& px)
{
    px.stored = 0;
    switch (static_cast<u32>(*px.s)) {
    case '%': {
        const s32 ch = px.get();
        if (ch == '%') return 1;
        if (ch == -1) {
            px.nchar = wrap_add(px.nchar, -1);
            return -1;
        }
        px.unget(ch);
        return 0;
    }
    case 'S': px.qual = 'l'; return sdk_getstr(px, 1);
    case 's': return sdk_getstr(px, 1);
    case 'c': return sdk_getstr(px, 0);
    case '[': return sdk_getstr(px, -1);
    case 'n': {
        if (px.noconv) return 1;
        const u32 bytes = px.qual == 'b' ? 1 : px.qual == 'h' ? 2 : (px.qual == 'j' || px.qual == 'q') ? 8 : 4;
        const u32 addr = px.pointer();
        px.args->put_int(addr, static_cast<u64>(static_cast<s64>(px.nchar)), bytes, 'n');
        return 1;
    }
    case 'A': case 'E': case 'F': case 'G': case 'a': case 'e': case 'f': case 'g': return sdk_getfloat(px);
    case 'X': case 'd': case 'i': case 'o': case 'p': case 'u': case 'x': return sdk_getint(px);
    default: return 0;
    }
}

// _Scanf (_WScanf): the format `fmt` (NUL-terminated, with a second NUL after it: a '%' the format ends in reads one
// character past its end, as the SDK's does) on the input; the number of items assigned, or EOF for an input failure
// before the first.
template <class C>
int sdk_scanf(ScanIo& io, ScanArgs& args, const C* fmt)
{
    using Ch = ScanChars<C>;
    Sft<C> x;
    x.io = &io;
    x.args = &args;
    x.s = fmt;
    int nconv = 0;
    for (;;) {
        const C wc = *x.s;
        const int n = wc != 0 ? 1 : 0; // the "C" locale's characters are single bytes
        if (Ch::fmt_space(wc)) {
            s32 c;
            do c = x.get();
            while (Ch::space(c));
            x.ungetn(c);
            x.s += n;
            continue;
        }
        const int literal = n - (wc == '%' ? 1 : 0);
        for (int i = 0; i < literal; i++) {
            const s32 c = x.get();
            if (!Ch::literal(x.s[i], c)) {
                if (c != -1) x.unget(c);
                return nconv;
            }
        }
        x.s += n;
        if (wc == 0) return nconv;
        if (wc != '%') continue;
        x.noconv = 0;
        if (*x.s == '*') {
            x.s++;
            x.noconv = '*';
        }
        x.width = 0;
        for (; Ch::fmt_digit(*x.s); x.s++)
            if (x.width != std::numeric_limits<s32>::max())
                x.width = wrap_add(wrap_mul(x.width, 10), static_cast<s32>(Ch::fmt_char(*x.s) - '0'));
        const u32 q = Ch::fmt_char(*x.s);
        if (q == 0 || q == 'h' || q == 'j' || q == 'l' || q == 't' || q == 'z' || q == 'L') { // strchr finds NUL too
            x.qual = static_cast<u8>(q);
            x.s++;
            if (q == 'h' && *x.s == 'h') {
                x.s++;
                x.qual = 'b';
            } else if (q == 'l' && *x.s == 'l') {
                x.s++;
                x.qual = 'q';
            }
        } else {
            x.qual = 0;
        }
        const u32 cv = Ch::fmt_char(*x.s);
        if (cv != 0 && cv != 'c' && cv != 'C' && cv != 'n' && cv != '[') {
            s32 c;
            do c = x.get();
            while (Ch::space(c));
            x.ungetn(c);
        }
        const int r = sdk_getfld(x);
        if (r <= 0) return nconv ? nconv : r;
        if (x.stored) nconv++;
        x.s++;
    }
}

} // namespace pspocket::hle::libc
