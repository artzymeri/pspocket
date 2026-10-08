#pragma once

// sys_libc's printf and scanf engines and its number parsing (D-508), the C99 standard's (7.19.6, 7.20.1.3-4,
// 7.24.2), for narrow and wide formats, in the C locale. The PS3's C library is Dinkumware's (RPCS3 PPUFunction.cpp
// names its internals); its implementation-defined choices are written next to the code. No guest memory is touched
// here: a FormatSource gives the arguments and the strings they point at, a ScanInput the characters, a ScanTarget
// takes what is stored. The C library's sizes: int, long, size_t, ptrdiff_t and pointers 32 bits; long long and
// intmax_t 64; long double is double; wchar_t 32 bits.
//
// Floating point text is the host C library's (snprintf and strtod in the "C" locale, both correctly rounded), which
// gives the standard's results for every finite value; infinities and NaNs are written here ("inf", "nan", with the
// sign bit, padded with spaces).

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

// ------------------------------------------------------------------------------------------------- floating point

// strtod's subject sequence (7.20.1.3), found in `text`: its length (0: none) and the value as double and float, the
// host's strtod_l and strtof_l in the C locale (correctly rounded, hex, inf, nan and nan(...) as C99 has them).
// *range: ERANGE (overflow, or an underflow to a tiny value).
struct FloatParse {
    double d = 0;
    float f = 0;
    u64 end = 0;
    bool range_d = false, range_f = false;
};

inline FloatParse parse_float(const std::string& text)
{
    FloatParse r;
    const char* s = text.c_str();
    char* end = nullptr;
    errno = 0;
    r.d = strtod_l(s, &end, c_locale());
    r.range_d = errno == ERANGE;
    r.end = static_cast<u64>(end - s);
    errno = 0;
    char* end_f = nullptr;
    r.f = strtof_l(s, &end_f, c_locale());
    r.range_f = errno == ERANGE;
    errno = 0;
    return r;
}

// The longest prefix of `peek`ed characters (given one at a time, `width` at most) that may begin a C99 floating
// subject sequence, as fscanf takes it (7.19.6.2: it reads the longest such prefix, pushing back one character): the
// characters taken, and whether they form a whole sequence. `next(c)` takes a character (-1 at the end) and returns
// whether it was accepted; the scanner stops at the first it refuses.
class FloatScanner {
public:
    // Offers the next character; false when it is not part of the sequence (the caller pushes it back).
    bool offer(s64 c)
    {
        const char lc = static_cast<char>(c_tolower(c));
        switch (state_) {
        case Start:
            if (c == '+' || c == '-') return take(c, Signed);
            [[fallthrough]];
        case Signed:
            if (lc == 'i') return take(c, Word, "inf");
            if (lc == 'n') return take(c, Word, "nan");
            if (c == '0') return take(c, Zero, nullptr, true);
            if (c_isdigit(c)) return take(c, Int, nullptr, true);
            if (c == '.') return take(c, Dot);
            return false;
        case Zero:
            if (lc == 'x') return take(c, HexStart, nullptr, false);
            [[fallthrough]];
        case Int:
            if (c_isdigit(c)) return take(c, Int, nullptr, true);
            if (c == '.') return take(c, Frac, nullptr, true);
            if (lc == 'e') return take(c, Exp, nullptr, false);
            return false;
        case Dot:
            if (c_isdigit(c)) return take(c, Frac, nullptr, true);
            return false;
        case Frac:
            if (c_isdigit(c)) return take(c, Frac, nullptr, true);
            if (lc == 'e') return take(c, Exp, nullptr, false);
            return false;
        case Exp:
            if (c == '+' || c == '-') return take(c, ExpSign, nullptr, false);
            [[fallthrough]];
        case ExpSign:
            if (c_isdigit(c)) return take(c, ExpDigits, nullptr, true);
            return false;
        case ExpDigits:
            if (c_isdigit(c)) return take(c, ExpDigits, nullptr, true);
            return false;
        case HexStart:
            if (c_isxdigit(c)) return take(c, HexInt, nullptr, true);
            if (c == '.') return take(c, HexDot, nullptr, false);
            return false;
        case HexInt:
            if (c_isxdigit(c)) return take(c, HexInt, nullptr, true);
            if (c == '.') return take(c, HexFrac, nullptr, true);
            if (lc == 'p') return take(c, HexExp, nullptr, false);
            return false;
        case HexDot:
            if (c_isxdigit(c)) return take(c, HexFrac, nullptr, true);
            return false;
        case HexFrac:
            if (c_isxdigit(c)) return take(c, HexFrac, nullptr, true);
            if (lc == 'p') return take(c, HexExp, nullptr, false);
            return false;
        case HexExp:
            if (c == '+' || c == '-') return take(c, HexExpSign, nullptr, false);
            [[fallthrough]];
        case HexExpSign:
            if (c_isdigit(c)) return take(c, HexExpDigits, nullptr, true);
            return false;
        case HexExpDigits:
            if (c_isdigit(c)) return take(c, HexExpDigits, nullptr, true);
            return false;
        case Word: {
            // "inf" then optionally "inity"; "nan" then optionally "(n-char-sequence)".
            if (word_[0] == 'i') {
                static constexpr std::string_view kInfinity = "infinity";
                if (matched_ < kInfinity.size() && lc == kInfinity[matched_]) {
                    matched_++;
                    return take(c, Word, nullptr, matched_ == 3 || matched_ == 8);
                }
                return false;
            }
            if (matched_ < 3) {
                if (lc == "nan"[matched_]) {
                    matched_++;
                    return take(c, Word, nullptr, matched_ == 3);
                }
                return false;
            }
            if (matched_ == 3 && c == '(') {
                matched_ = 4;
                return take(c, Word, nullptr, false);
            }
            if (matched_ == 4) {
                if (c == ')') {
                    matched_ = 5;
                    return take(c, Word, nullptr, true);
                }
                if (c_isalpha(c) || c_isdigit(c) || c == '_') return take(c, Word, nullptr, false);
            }
            return false;
        }
        }
        return false;
    }
    const std::string& text() const { return text_; }
    bool complete() const { return complete_; }

private:
    enum State {
        Start, Signed, Zero, Int, Dot, Frac, Exp, ExpSign, ExpDigits, HexStart, HexInt, HexDot, HexFrac, HexExp,
        HexExpSign, HexExpDigits, Word
    };
    bool take(s64 c, State next, const char* word = nullptr, bool complete = false)
    {
        text_ += static_cast<char>(c);
        state_ = next;
        complete_ = complete;
        if (word) {
            word_ = word;
            matched_ = 1;
        }
        return true;
    }
    State state_ = Start;
    std::string text_;
    bool complete_ = false;
    const char* word_ = "";
    usize matched_ = 0;
};

// ------------------------------------------------------------------------------------------------- printf

// Where a formatter takes its arguments from (the guest's variadic arguments or va_list, or an SPU's local store) and
// what their pointers lead to. A string or %n target that is not memory is a problem (noted, the call goes on).
class FormatSource {
public:
    virtual ~FormatSource() = default;
    virtual u64 next() = 0; // the next argument's 64-bit slot (an int is its low word, a double its bits)
    virtual double next_double() { return std::bit_cast<double>(next()); }
    // The string at `addr`: up to its NUL, at most `max` bytes (wide: characters).
    virtual std::optional<std::string> string(u32 addr, u64 max) = 0;
    virtual std::optional<std::u32string> wstring(u32 addr, u64 max) = 0;
    virtual bool store(u32 addr, u64 value, u32 bytes) = 0;
    std::vector<std::string> problems;
};

enum class Len : u8 { None, hh, h, l, ll, j, z, t, L };

struct Spec {
    bool minus = false, plus = false, space = false, alt = false, zero = false;
    u64 width = 0;
    s64 prec = -1; // -1: none
    Len len = Len::None;
};

// Widths and numeric precisions are cut to these, so "%999999999d" cannot exhaust the host's memory.
inline constexpr u64 kMaxWidth = 0x1000000;
inline constexpr s64 kMaxNumericPrecision = 0x100000;

// The bits of an integer argument of this length: hh 8, h 16, l/z/t/none 32 (long, size_t and ptrdiff_t are 32
// bits), ll/j/L 64.
inline u32 int_bits(Len len)
{
    switch (len) {
    case Len::hh: return 8;
    case Len::h: return 16;
    case Len::ll: case Len::j: case Len::L: return 64;
    default: return 32;
    }
}

template <class C>
void append_ascii(std::basic_string<C>& out, std::string_view s)
{
    for (const char c : s) out += static_cast<C>(static_cast<u8>(c));
}

// `prefix`, `zeros` zeros and `body`, padded to the width: spaces before, after with '-', or zeros after the prefix
// with '0' when `zero_pad`.
template <class C>
void put_field(std::basic_string<C>& out, const Spec& s, std::basic_string_view<C> prefix, u64 zeros,
    std::basic_string_view<C> body, bool zero_pad)
{
    const u64 n = prefix.size() + zeros + body.size();
    const u64 fill = s.width > n ? s.width - n : 0;
    if (s.minus) {
        out += prefix;
        out.append(zeros, static_cast<C>('0'));
        out += body;
        out.append(fill, static_cast<C>(' '));
    } else if (zero_pad && s.zero) {
        out += prefix;
        out.append(zeros + fill, static_cast<C>('0'));
        out += body;
    } else {
        out.append(fill, static_cast<C>(' '));
        out += prefix;
        out.append(zeros, static_cast<C>('0'));
        out += body;
    }
}

template <class C>
void put_integer(std::basic_string<C>& out, const Spec& s, char conv, u64 raw)
{
    const u32 bits = int_bits(s.len);
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    bool negative = false;
    u64 magnitude = raw & mask;
    if (conv == 'd' || conv == 'i') {
        const s64 v = bits == 64 ? static_cast<s64>(raw) : static_cast<s64>(raw << (64 - bits)) >> (64 - bits);
        negative = v < 0;
        magnitude = negative ? 0 - static_cast<u64>(v) : static_cast<u64>(v);
    }
    const u32 base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
    const char* set = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int k = sizeof buf;
    for (u64 v = magnitude; v; v /= base) buf[--k] = set[v % base];
    std::basic_string<C> digits;
    for (int i = k; i < static_cast<int>(sizeof buf); i++) digits += static_cast<C>(buf[i]);
    if (digits.empty() && s.prec != 0) digits = static_cast<C>('0'); // 0 has one digit, none at precision 0
    const s64 prec = std::min(s.prec, kMaxNumericPrecision);
    u64 zeros = prec > static_cast<s64>(digits.size()) ? static_cast<u64>(prec) - digits.size() : 0;
    // '#' with o: the precision grows until the first digit is 0.
    if (conv == 'o' && s.alt && zeros == 0 && (digits.empty() || digits[0] != '0')) zeros = 1;
    std::basic_string<C> prefix;
    if (conv == 'd' || conv == 'i') {
        if (negative) prefix = static_cast<C>('-');
        else if (s.plus) prefix = static_cast<C>('+');
        else if (s.space) prefix = static_cast<C>(' ');
    } else if ((conv == 'x' || conv == 'X') && s.alt && magnitude) {
        append_ascii(prefix, conv == 'x' ? "0x" : "0X");
    } else if (conv == 'p') {
        append_ascii(prefix, "0x");
    }
    put_field<C>(out, s, prefix, zeros, digits, s.prec < 0);
}

template <class C>
void put_double(std::basic_string<C>& out, const Spec& s, char conv, double v)
{
    const bool upper = conv >= 'A' && conv <= 'Z';
    if (!std::isfinite(v)) {
        std::basic_string<C> body;
        if (std::signbit(v)) body = static_cast<C>('-');
        else if (s.plus) body = static_cast<C>('+');
        else if (s.space) body = static_cast<C>(' ');
        append_ascii(body, std::isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"));
        put_field<C>(out, s, {}, 0, body, false);
        return;
    }
    std::string f = "%";
    if (s.minus) f += '-';
    if (s.plus) f += '+';
    if (s.space) f += ' ';
    if (s.alt) f += '#';
    if (s.zero) f += '0';
    f += "*.*";
    f += conv;
    const int width = static_cast<int>(std::min<u64>(s.width, kMaxWidth));
    const int prec = s.prec < 0 ? -1 : static_cast<int>(std::min(s.prec, kMaxNumericPrecision));
    CLocaleScope c_locale_here;
    char small[128];
    // The format is one of the floating conversions with its flags, built above.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    const int n = std::snprintf(small, sizeof small, f.c_str(), width, prec, v);
    if (n < 0) return;
    if (static_cast<usize>(n) < sizeof small) {
        append_ascii(out, std::string_view(small, static_cast<usize>(n)));
        return;
    }
    std::string big(static_cast<usize>(n) + 1, '\0');
    std::snprintf(big.data(), big.size(), f.c_str(), width, prec, v);
#pragma clang diagnostic pop
    big.resize(static_cast<usize>(n));
    append_ascii(out, big);
}

template <class C>
struct FormatResult {
    std::basic_string<C> text;
    bool failed = false; // an encoding error (EILSEQ): the function returns a negative value
};

// The text of `fmt` with the arguments of `src` (7.19.6.1; wide: 7.24.2.1). Dinkumware's choices where the standard
// leaves them: %p is "0x" and the hex digits; %s or %ls of a null pointer prints "(null)"; a conversion character the
// standard does not have prints itself and takes no argument; a % the format ends in prints nothing; h and hh apply
// to %n's store as well; L, q and j are 64-bit for integers.
template <class C>
FormatResult<C> format(std::basic_string_view<C> fmt, FormatSource& src)
{
    FormatResult<C> r;
    std::basic_string<C>& out = r.text;
    usize i = 0;
    const auto at = [&](usize k) -> s64 { return k < fmt.size() ? static_cast<s64>(static_cast<u32>(fmt[k])) : -1; };
    const auto decimal = [&] {
        u64 v = 0;
        while (c_isdigit(at(i))) v = std::min<u64>(v * 10 + static_cast<u64>(fmt[i++] - '0'), kMaxWidth);
        return v;
    };
    while (i < fmt.size()) {
        if (fmt[i] != static_cast<C>('%')) {
            out += fmt[i++];
            continue;
        }
        i++;
        Spec s;
        for (bool flag = true; flag;) {
            switch (at(i)) {
            case '-': s.minus = true; break;
            case '+': s.plus = true; break;
            case ' ': s.space = true; break;
            case '#': s.alt = true; break;
            case '0': s.zero = true; break;
            default: flag = false; continue;
            }
            i++;
        }
        if (at(i) == '*') {
            i++;
            const s32 w = static_cast<s32>(src.next());
            if (w < 0) s.minus = true;
            s.width = std::min<u64>(w < 0 ? 0 - static_cast<u64>(static_cast<s64>(w)) : static_cast<u64>(w), kMaxWidth);
        } else {
            s.width = decimal();
        }
        if (at(i) == '.') {
            i++;
            if (at(i) == '*') {
                i++;
                const s32 p = static_cast<s32>(src.next());
                s.prec = p < 0 ? -1 : p;
            } else {
                u64 p = 0;
                while (c_isdigit(at(i))) p = std::min<u64>(p * 10 + static_cast<u64>(fmt[i++] - '0'), 0x7FFFFFFF);
                s.prec = static_cast<s64>(p);
            }
        }
        switch (at(i)) {
        case 'h':
            s.len = at(i + 1) == 'h' ? Len::hh : Len::h;
            i += s.len == Len::hh ? 2 : 1;
            break;
        case 'l':
            s.len = at(i + 1) == 'l' ? Len::ll : Len::l;
            i += s.len == Len::ll ? 2 : 1;
            break;
        case 'q': s.len = Len::ll; i++; break;
        case 'j': s.len = Len::j; i++; break;
        case 'z': s.len = Len::z; i++; break;
        case 't': s.len = Len::t; i++; break;
        case 'L': s.len = Len::L; i++; break;
        default: break;
        }
        if (i >= fmt.size()) break; // a conversion the format ends in prints nothing
        const s64 conv = at(i++);
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
            put_integer(out, s, static_cast<char>(conv), src.next());
            break;
        case 'p': {
            Spec p = s;
            p.len = Len::None;
            put_integer(out, p, 'p', src.next());
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            put_double(out, s, static_cast<char>(conv), src.next_double());
            break;
        case 'c': {
            const u64 v = src.next();
            std::basic_string<C> body;
            if constexpr (sizeof(C) == 1) {
                if (s.len == Len::l) { // a wint_t converted as by wcrtomb
                    const std::optional<char> b = wide_to_byte(static_cast<char32_t>(static_cast<u32>(v)));
                    if (!b) {
                        r.failed = true;
                        return r;
                    }
                    body = *b;
                } else {
                    body = static_cast<C>(static_cast<u8>(v));
                }
            } else {
                // wide: an int converted as by btowc, a wint_t as it is
                body = s.len == Len::l ? static_cast<C>(static_cast<u32>(v)) : static_cast<C>(static_cast<u8>(v));
            }
            put_field<C>(out, s, {}, 0, body, false);
            break;
        }
        case 's': {
            const u32 addr = static_cast<u32>(src.next());
            std::basic_string<C> body;
            const u64 max = s.prec >= 0 ? static_cast<u64>(s.prec) : ~u64{0};
            if (!addr) {
                append_ascii(body, std::string_view("(null)").substr(0, std::min<u64>(6, max)));
            } else if (s.len == Len::l) {
                const std::optional<std::u32string> w = src.wstring(addr, max);
                if (!w) {
                    src.problems.push_back(std::format("the %ls argument {:#x} is not memory (printed as nothing)", addr));
                } else if constexpr (sizeof(C) == 1) {
                    for (const char32_t wc : *w) { // as by wcrtomb, no more bytes than the precision
                        const std::optional<char> b = wide_to_byte(wc);
                        if (!b) {
                            r.failed = true;
                            return r;
                        }
                        body += *b;
                    }
                } else {
                    body = *w;
                }
            } else {
                const std::optional<std::string> n = src.string(addr, max);
                if (!n) src.problems.push_back(std::format("the %s argument {:#x} is not memory (printed as nothing)", addr));
                else for (const char c : *n) body += static_cast<C>(static_cast<u8>(c)); // as by mbrtowc for wide
            }
            put_field<C>(out, s, {}, 0, body, false);
            break;
        }
        case 'n': {
            const u32 addr = static_cast<u32>(src.next());
            const u32 bytes = s.len == Len::hh ? 1 : s.len == Len::h ? 2 : int_bits(s.len) == 64 ? 8 : 4;
            if (!src.store(addr, out.size(), bytes))
                src.problems.push_back(std::format("the %n argument {:#x} is not memory (nothing stored)", addr));
            break;
        }
        default: // '%' and any character the standard does not convert
            out += static_cast<C>(conv);
            break;
        }
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- scanf

// The characters a scanf reads: get() the next (-1 at the end of input or an input error), unget() pushes back the
// last one got (once).
class ScanInput {
public:
    virtual ~ScanInput() = default;
    virtual s64 get() = 0;
    virtual void unget(s64 c) = 0;
};

// Where a scanf stores: the next pointer argument, and bytes at a pointer.
class ScanTarget {
public:
    virtual ~ScanTarget() = default;
    virtual u32 next_pointer() = 0;
    virtual bool put(u32 addr, const u8* data, usize size) = 0;
    std::vector<std::string> problems;

    bool put_int(u32 addr, u64 v, u32 bytes)
    {
        u8 b[8];
        for (u32 k = 0; k < bytes; k++) b[k] = static_cast<u8>(v >> (8 * (bytes - 1 - k)));
        return put(addr, b, bytes);
    }
};

// fscanf (7.19.6.2; wide: 7.24.2.2) on `in`, the results through `out`: the number of input items assigned, or -1
// (EOF) when an input failure comes before the first conversion. Dinkumware's choices: a '-' inside a scanset that is
// neither its first nor its last character makes a range; %p reads as %x, a 32-bit pointer; a "0x" with no hex digit
// after it (or "1e" with no exponent digits) is a matching failure, as fscanf can push back one character only.
template <class C>
int scan(std::basic_string_view<C> fmt, ScanInput& in, ScanTarget& out)
{
    int assigned = 0;
    bool converted = false, input_failure = false;
    u64 count = 0;
    const auto get = [&]() -> s64 {
        const s64 c = in.get();
        if (c >= 0) count++;
        return c;
    };
    const auto unget = [&](s64 c) {
        if (c >= 0) {
            in.unget(c);
            count--;
        }
    };
    const auto skip_space = [&]() -> bool { // false at the end of input
        s64 c;
        while (c_isspace(c = get())) {
        }
        unget(c);
        return c >= 0;
    };
    usize i = 0;
    const auto at = [&](usize k) -> s64 { return k < fmt.size() ? static_cast<s64>(static_cast<u32>(fmt[k])) : -1; };
    // Stores the characters of a %c, %s or %[ field: bytes, or wide characters with l (narrow from wide input as by
    // wcrtomb; false on an encoding error).
    const auto store_chars = [&](u32 addr, const std::u32string& chars, bool wide_dest, bool nul) -> bool {
        if (wide_dest) {
            std::vector<u8> b;
            for (const char32_t c : chars)
                for (int k = 3; k >= 0; k--) b.push_back(static_cast<u8>(static_cast<u32>(c) >> (8 * k)));
            if (nul) b.insert(b.end(), 4, 0);
            return out.put(addr, b.data(), b.size());
        }
        std::vector<u8> b;
        for (const char32_t c : chars) {
            const std::optional<char> byte = wide_to_byte(c);
            if (!byte) return false;
            b.push_back(static_cast<u8>(*byte));
        }
        if (nul) b.push_back(0);
        return out.put(addr, b.data(), b.size());
    };
    while (i < fmt.size()) {
        const s64 f = at(i);
        if (c_isspace(f)) {
            while (c_isspace(at(i))) i++;
            skip_space();
            continue;
        }
        if (f != '%') {
            i++;
            const s64 c = get();
            if (c < 0) {
                input_failure = true;
                break;
            }
            if (c != f) {
                unget(c);
                break;
            }
            continue;
        }
        i++;
        if (at(i) == '%') {
            i++;
            if (!skip_space()) {
                input_failure = true;
                break;
            }
            const s64 c = get();
            if (c != '%') {
                unget(c);
                break;
            }
            continue;
        }
        const bool suppress = at(i) == '*';
        if (suppress) i++;
        u64 width = 0;
        while (c_isdigit(at(i))) width = std::min<u64>(width * 10 + static_cast<u64>(at(i++) - '0'), 0x7FFFFFFF);
        Len len = Len::None;
        switch (at(i)) {
        case 'h': len = at(i + 1) == 'h' ? Len::hh : Len::h; i += len == Len::hh ? 2 : 1; break;
        case 'l': len = at(i + 1) == 'l' ? Len::ll : Len::l; i += len == Len::ll ? 2 : 1; break;
        case 'q': len = Len::ll; i++; break;
        case 'j': len = Len::j; i++; break;
        case 'z': len = Len::z; i++; break;
        case 't': len = Len::t; i++; break;
        case 'L': len = Len::L; i++; break;
        default: break;
        }
        const s64 conv = at(i++);
        if (conv < 0) break;
        // %[: the set, with ^ and a leading ] taken literally.
        bool invert = false;
        std::vector<std::pair<u32, u32>> set;
        if (conv == '[') {
            if (at(i) == '^') {
                invert = true;
                i++;
            }
            const usize first = i;
            for (;;) {
                const s64 c = at(i);
                if (c < 0) break;
                if (c == ']' && i != first) {
                    i++;
                    break;
                }
                if (at(i + 1) == '-' && at(i + 2) >= 0 && at(i + 2) != ']' && i + 1 != first) {
                    set.emplace_back(static_cast<u32>(c), static_cast<u32>(at(i + 2)));
                    i += 3;
                    continue;
                }
                set.emplace_back(static_cast<u32>(c), static_cast<u32>(c));
                i++;
            }
        }
        const auto in_set = [&](s64 c) {
            bool hit = false;
            for (const auto& [lo, hi] : set)
                if (static_cast<u32>(c) >= lo && static_cast<u32>(c) <= hi) hit = true;
            return hit != invert;
        };
        if (conv == 'n') {
            if (!suppress) {
                const u32 addr = out.next_pointer();
                const u32 bytes = len == Len::hh ? 1 : len == Len::h ? 2 : int_bits(len) == 64 ? 8 : 4;
                if (!out.put_int(addr, count, bytes))
                    out.problems.push_back(std::format("the %n argument {:#x} is not memory", addr));
            }
            continue;
        }
        if (conv != '[' && conv != 'c' && !skip_space()) {
            input_failure = true;
            break;
        }
        const u64 limit = width ? width : ~u64{0};
        bool ok = true;
        switch (conv) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'p': {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            std::string text;
            bool digits = false;
            s64 c = get();
            const auto take = [&] {
                text += static_cast<char>(c);
                c = get();
            };
            if ((c == '+' || c == '-') && text.size() < limit) take();
            if ((base == 0 || base == 16) && c == '0' && text.size() < limit) {
                take();
                digits = true;
                if ((c == 'x' || c == 'X') && text.size() < limit) {
                    take();
                    base = 16;
                    digits = false;
                } else if (base == 0) {
                    base = 8;
                }
            }
            if (base == 0) base = 10;
            while (text.size() < limit && digit_value(c) < base) {
                take();
                digits = true;
            }
            unget(c);
            if (!digits) {
                if (text.empty() && c < 0) input_failure = true;
                ok = false;
                break;
            }
            const IntParse p = parse_int([&](u64 k) -> s64 { return k < text.size() ? static_cast<u8>(text[k]) : -1; },
                base);
            converted = true;
            if (suppress) break;
            const bool is_signed = conv == 'd' || conv == 'i';
            const u32 bits = conv == 'p' ? 32 : int_bits(len);
            bool range = false;
            const u64 v = int_result(p, 64, is_signed, &range);
            const u32 addr = out.next_pointer();
            if (!out.put_int(addr, v, bits / 8)) out.problems.push_back(std::format("the %{} argument {:#x} is not memory",
                static_cast<char>(conv), addr));
            assigned++;
            break;
        }
        case 'a': case 'A': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            FloatScanner fs;
            s64 c = get();
            while (fs.text().size() < limit && c >= 0 && fs.offer(c)) c = get();
            unget(c);
            if (!fs.complete()) {
                if (fs.text().empty() && c < 0) input_failure = true;
                ok = false;
                break;
            }
            converted = true;
            if (suppress) break;
            const FloatParse p = parse_float(fs.text());
            const u32 addr = out.next_pointer();
            bool stored;
            if (len == Len::l || len == Len::L) stored = out.put_int(addr, std::bit_cast<u64>(p.d), 8);
            else stored = out.put_int(addr, std::bit_cast<u32>(p.f), 4);
            if (!stored) out.problems.push_back(std::format("the %{} argument {:#x} is not memory", static_cast<char>(conv), addr));
            assigned++;
            break;
        }
        case 's': case 'c': case '[': {
            const u64 n = conv == 'c' ? (width ? width : 1) : limit;
            std::u32string chars;
            s64 c = 0;
            while (chars.size() < n) {
                c = get();
                if (c < 0) break;
                if ((conv == 's' && c_isspace(c)) || (conv == '[' && !in_set(c))) {
                    unget(c);
                    break;
                }
                chars += static_cast<char32_t>(c);
            }
            if (chars.empty() || (conv == 'c' && chars.size() < n)) {
                if (c < 0) input_failure = true;
                ok = false;
                break;
            }
            converted = true;
            if (suppress) break;
            const u32 addr = out.next_pointer();
            if (!store_chars(addr, chars, len == Len::l, conv != 'c')) {
                out.problems.push_back(std::format("the %{} argument {:#x} is not memory, or a character has no "
                                                   "multibyte form", static_cast<char>(conv), addr));
                ok = false;
                break;
            }
            assigned++;
            break;
        }
        default: // not a conversion: the directive fails
            ok = false;
            break;
        }
        if (!ok) break;
    }
    return input_failure && !converted ? -1 : assigned;
}

} // namespace pspocket::hle::libc
