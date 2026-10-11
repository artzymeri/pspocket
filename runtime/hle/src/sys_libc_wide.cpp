// sys_libc part 2 (D-512): wide strings, multibyte conversions and wide character classes, in the "C" locale, as C99
// (7.24, 7.25) and Dinkumware's "C" locale have them. A guest wchar_t is a 32-bit big-endian word. The "C" locale's
// multibyte characters are single bytes, each the wide character of its value (MB_CUR_MAX 1, no shift states): a wide
// character past 255 has no multibyte form (EILSEQ). Its classes are ASCII's: no wide character past 127 is a letter,
// digit, space or punctuation. Registered under sys_libc and sys_libm with the rest of the C library.

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_scan_guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }


std::optional<std::u32string> wstr(PPUContext& ctx, const char* fn, u32 s, u64 max = kMaxString / 4)
{
    std::optional<std::u32string> r = read_wstring(thread_of(ctx), s, max);
    if (!r) refuse(ctx, std::format("{}: the wide string at {:#x} runs out of guest memory", fn, s));
    return r;
}

u32 rd(PPUContext& ctx, u32 a) { return ps_read32(gb(ctx), a); }
void wr(PPUContext& ctx, u32 a, u32 v) { ps_write32(gb(ctx), a, v); }

// Dinkumware's wide comparisons, like its narrow ones: -1 or +1 (wchar_t is unsigned here).
s32 order(u32 a, u32 b) { return a < b ? -1 : a > b ? 1 : 0; }

// ------------------------------------------------------------------------------------------------- wchar.h strings

u32 wcslen_(PPUContext& ctx, u32 s)
{
    const auto t = wstr(ctx, "wcslen", s);
    return t ? static_cast<u32>(t->size()) : 0;
}

u32 wcscpy_(PPUContext& ctx, u32 d, u32 s)
{
    const auto t = wstr(ctx, "wcscpy", s);
    if (t && range_ok(ctx, "wcscpy", d, 4 * (t->size() + 1))) write_wstring(thread_of(ctx), d, *t);
    return d;
}

u32 wcsncpy_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    const auto t = wstr(ctx, "wcsncpy", s, n);
    if (!t || !range_ok(ctx, "wcsncpy", d, u64{n} * 4)) return d;
    for (u32 i = 0; i < n; i++) wr(ctx, d + 4 * i, i < t->size() ? static_cast<u32>((*t)[i]) : 0);
    return d;
}

u32 wcscat_(PPUContext& ctx, u32 d, u32 s)
{
    const auto a = wstr(ctx, "wcscat", d), b = wstr(ctx, "wcscat", s);
    if (a && b && range_ok(ctx, "wcscat", d, 4 * (a->size() + b->size() + 1)))
        write_wstring(thread_of(ctx), d + 4 * static_cast<u32>(a->size()), *b);
    return d;
}

u32 wcsncat_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    const auto a = wstr(ctx, "wcsncat", d), b = wstr(ctx, "wcsncat", s, n);
    if (a && b && range_ok(ctx, "wcsncat", d, 4 * (a->size() + b->size() + 1)))
        write_wstring(thread_of(ctx), d + 4 * static_cast<u32>(a->size()), *b);
    return d;
}

s32 wcsncmp_impl(PPUContext& ctx, const char* fn, u32 a, u32 b, u64 n)
{
    GuestWords x(thread_of(ctx), a), y(thread_of(ctx), b);
    for (u64 i = 0; i < n; i++) {
        const s64 c = x.at(i), e = y.at(i);
        if (c < 0 || e < 0) {
            refuse(ctx, std::format("{}: a wide string ({:#x} or {:#x}) runs out of guest memory", fn, a, b));
            return 0;
        }
        if (c != e) return order(static_cast<u32>(c), static_cast<u32>(e));
        if (!c) return 0;
    }
    return 0;
}

s32 wcscmp_(PPUContext& ctx, u32 a, u32 b) { return wcsncmp_impl(ctx, "wcscmp", a, b, ~u64{0}); }
s32 wcsncmp_(PPUContext& ctx, u32 a, u32 b, u32 n) { return wcsncmp_impl(ctx, "wcsncmp", a, b, n); }
s32 wcscoll_(PPUContext& ctx, u32 a, u32 b) { return wcscmp_(ctx, a, b); }

u32 wcsxfrm_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    const auto t = wstr(ctx, "wcsxfrm", s);
    if (!t) return 0;
    if (t->size() < n && range_ok(ctx, "wcsxfrm", d, 4 * (t->size() + 1))) write_wstring(thread_of(ctx), d, *t);
    return static_cast<u32>(t->size());
}

u32 wcschr_(PPUContext& ctx, u32 s, u32 c)
{
    const auto t = wstr(ctx, "wcschr", s);
    if (!t) return 0;
    if (!c) return s + 4 * static_cast<u32>(t->size());
    const usize i = t->find(static_cast<char32_t>(c));
    return i == std::u32string::npos ? 0 : s + 4 * static_cast<u32>(i);
}

u32 wcsrchr_(PPUContext& ctx, u32 s, u32 c)
{
    const auto t = wstr(ctx, "wcsrchr", s);
    if (!t) return 0;
    if (!c) return s + 4 * static_cast<u32>(t->size());
    const usize i = t->rfind(static_cast<char32_t>(c));
    return i == std::u32string::npos ? 0 : s + 4 * static_cast<u32>(i);
}

u32 wcsspn_(PPUContext& ctx, u32 s, u32 set)
{
    const auto a = wstr(ctx, "wcsspn", s), b = wstr(ctx, "wcsspn", set);
    return a && b ? static_cast<u32>(std::min(a->find_first_not_of(*b), a->size())) : 0;
}

u32 wcscspn_(PPUContext& ctx, u32 s, u32 set)
{
    const auto a = wstr(ctx, "wcscspn", s), b = wstr(ctx, "wcscspn", set);
    return a && b ? static_cast<u32>(std::min(a->find_first_of(*b), a->size())) : 0;
}

u32 wcspbrk_(PPUContext& ctx, u32 s, u32 set)
{
    const auto a = wstr(ctx, "wcspbrk", s), b = wstr(ctx, "wcspbrk", set);
    if (!a || !b) return 0;
    const usize i = a->find_first_of(*b);
    return i == std::u32string::npos ? 0 : s + 4 * static_cast<u32>(i);
}

u32 wcsstr_(PPUContext& ctx, u32 s, u32 sub)
{
    const auto a = wstr(ctx, "wcsstr", s), b = wstr(ctx, "wcsstr", sub);
    if (!a || !b) return 0;
    const usize i = a->find(*b);
    return i == std::u32string::npos ? 0 : s + 4 * static_cast<u32>(i);
}

// wcstok(s, delim, ptr) (C99: its place in *ptr).
u32 wcstok_(PPUContext& ctx, u32 s, u32 delim, u32 ptr)
{
    if (!range_ok(ctx, "wcstok", ptr, 4)) return 0;
    if (!s) s = rd(ctx, ptr);
    if (!s) return 0;
    const auto t = wstr(ctx, "wcstok", s), d = wstr(ctx, "wcstok", delim);
    if (!t || !d) return 0;
    const usize start = t->find_first_not_of(*d);
    if (start == std::u32string::npos) {
        wr(ctx, ptr, 0);
        return 0;
    }
    const usize end = t->find_first_of(*d, start);
    if (end == std::u32string::npos) {
        wr(ctx, ptr, 0);
    } else {
        wr(ctx, s + 4 * static_cast<u32>(end), 0);
        wr(ctx, ptr, s + 4 * static_cast<u32>(end) + 4);
    }
    return s + 4 * static_cast<u32>(start);
}

u32 wmemchr_(PPUContext& ctx, u32 s, u32 c, u32 n)
{
    if (!range_ok(ctx, "wmemchr", s, u64{n} * 4)) return 0;
    for (u32 i = 0; i < n; i++)
        if (rd(ctx, s + 4 * i) == c) return s + 4 * i;
    return 0;
}

s32 wmemcmp_(PPUContext& ctx, u32 a, u32 b, u32 n)
{
    if (!range_ok(ctx, "wmemcmp", a, u64{n} * 4) || !range_ok(ctx, "wmemcmp", b, u64{n} * 4)) return 0;
    for (u32 i = 0; i < n; i++)
        if (const u32 x = rd(ctx, a + 4 * i), y = rd(ctx, b + 4 * i); x != y) return order(x, y);
    return 0;
}

u32 wmemmove_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    if (range_ok(ctx, "wmemmove", d, u64{n} * 4) && range_ok(ctx, "wmemmove", s, u64{n} * 4))
        std::memmove(gb(ctx) + d, gb(ctx) + s, u64{n} * 4);
    return d;
}

u32 wmemcpy_(PPUContext& ctx, u32 d, u32 s, u32 n) { return wmemmove_(ctx, d, s, n); }

u32 wmemset_(PPUContext& ctx, u32 d, u32 c, u32 n)
{
    if (range_ok(ctx, "wmemset", d, u64{n} * 4))
        for (u32 i = 0; i < n; i++) wr(ctx, d + 4 * i, c);
    return d;
}

// ------------------------------------------------------------------------------------------------- wide numbers

// The wide forms of strtol and strtod (7.24.4.1): the narrow ones' SDK parsing (libc_scan.hpp, D-907) over 32-bit
// wide characters; no wide character past 127 is a digit, sign, space or letter in the "C" locale.
u32 wcstol_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(guest_strtoint(ctx, "wcstol", s, e, b, 32, true, 0, true)); }
u32 wcstoul_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(guest_strtoint(ctx, "wcstoul", s, e, b, 32, false, 0, true)); }
u64 wcstoll_(PPUContext& ctx, u32 s, u32 e, s32 b) { return guest_strtoint(ctx, "wcstoll", s, e, b, 64, true, 0, true); }
u64 wcstoull_(PPUContext& ctx, u32 s, u32 e, s32 b) { return guest_strtoint(ctx, "wcstoull", s, e, b, 64, false, 0, true); }
u64 wcstoimax_(PPUContext& ctx, u32 s, u32 e, s32 b) { return guest_strtoint(ctx, "wcstoimax", s, e, b, 64, true, 0, true); }
u64 wcstoumax_(PPUContext& ctx, u32 s, u32 e, s32 b) { return guest_strtoint(ctx, "wcstoumax", s, e, b, 64, false, 0, true); }
double wcstod_(PPUContext& ctx, u32 s, u32 e) { return guest_strtod(ctx, "wcstod", s, e, 0, 0, true, false); }
double wcstold_(PPUContext& ctx, u32 s, u32 e) { return guest_strtod(ctx, "wcstold", s, e, 0, 0, true, false); }
float wcstof_(PPUContext& ctx, u32 s, u32 e) { return static_cast<float>(guest_strtod(ctx, "wcstof", s, e, 0, 0, true, true)); }

// ------------------------------------------------------------------------------------------------- multibyte

constexpr u32 kEOF = 0xFFFFFFFFu, kWEOF = 0xFFFFFFFFu;

// mbrtowc (7.24.6.3.2) in the "C" locale: one byte, the wide character of its value; s NULL resets (0). The state
// is untouched (no shift states).
u32 mbrtowc_impl(PPUContext& ctx, u32 pwc, u32 s, u32 n)
{
    if (!s) return 0;
    if (!n) return 0xFFFFFFFEu; // (size_t)-2: incomplete
    if (!range_ok(ctx, "mbrtowc", s, 1)) return kEOF;
    const u8 c = gb(ctx)[s];
    if (pwc) {
        if (!range_ok(ctx, "mbrtowc", pwc, 4)) return kEOF;
        wr(ctx, pwc, c);
    }
    return c ? 1 : 0;
}

u32 mbrtowc_(PPUContext& ctx, u32 pwc, u32 s, u32 n, u32) { return mbrtowc_impl(ctx, pwc, s, n); }
u32 mbrlen_(PPUContext& ctx, u32 s, u32 n, u32) { return mbrtowc_impl(ctx, 0, s, n); }
s32 mbtowc_(PPUContext& ctx, u32 pwc, u32 s, u32 n) { return s ? static_cast<s32>(mbrtowc_impl(ctx, pwc, s, n)) : 0; }
s32 mblen_(PPUContext& ctx, u32 s, u32 n) { return s ? static_cast<s32>(mbrtowc_impl(ctx, 0, s, n)) : 0; }

// wcrtomb (7.24.6.3.3): the byte of a wide character up to 255, else EILSEQ ((size_t)-1).
u32 wcrtomb_impl(PPUContext& ctx, u32 s, u32 wc)
{
    if (!s) return 1;
    if (wc > 0xFF) {
        set_errno(ctx, kEILSEQ);
        return kEOF;
    }
    if (!range_ok(ctx, "wcrtomb", s, 1)) return kEOF;
    gb(ctx)[s] = static_cast<u8>(wc);
    return 1;
}

u32 wcrtomb_(PPUContext& ctx, u32 s, u32 wc, u32) { return wcrtomb_impl(ctx, s, wc); }
s32 wctomb_(PPUContext& ctx, u32 s, u32 wc) { return s ? static_cast<s32>(wcrtomb_impl(ctx, s, wc)) : 0; }

// mbsrtowcs (7.24.6.4.1): bytes to wide characters up to the NUL or `len` of them; *src set past the last converted
// (NULL after the NUL). mbstowcs is it without the pointer to update.
u32 mbs_to_wcs(PPUContext& ctx, const char* fn, u32 dst, u32 src, u32 len, u32* src_out)
{
    const auto t = read_string(thread_of(ctx), src);
    if (!t) {
        refuse(ctx, std::format("{}: the string at {:#x} runs out of guest memory", fn, src));
        return kEOF;
    }
    if (!dst) return static_cast<u32>(t->size());
    u32 n = 0;
    for (; n < len; n++) {
        const u8 c = n < t->size() ? static_cast<u8>((*t)[n]) : 0;
        if (!range_ok(ctx, fn, dst + 4 * n, 4)) return kEOF;
        wr(ctx, dst + 4 * n, c);
        if (!c) {
            if (src_out) *src_out = 0;
            return n;
        }
    }
    if (src_out) *src_out = src + n;
    return n;
}

u32 mbstowcs_(PPUContext& ctx, u32 dst, u32 src, u32 len) { return mbs_to_wcs(ctx, "mbstowcs", dst, src, len, nullptr); }

u32 mbsrtowcs_(PPUContext& ctx, u32 dst, u32 srcp, u32 len, u32)
{
    if (!range_ok(ctx, "mbsrtowcs", srcp, 4)) return kEOF;
    u32 src = rd(ctx, srcp);
    const u32 r = mbs_to_wcs(ctx, "mbsrtowcs", dst, src, len, &src);
    if (dst && r != kEOF) wr(ctx, srcp, src);
    return r;
}

// wcsrtombs (7.24.6.4.2): wide characters to bytes, EILSEQ for one past 255.
u32 wcs_to_mbs(PPUContext& ctx, const char* fn, u32 dst, u32 src, u32 len, u32* src_out)
{
    const auto t = wstr(ctx, fn, src);
    if (!t) return kEOF;
    u32 n = 0;
    for (u32 i = 0;; i++) {
        const char32_t c = i < t->size() ? (*t)[i] : 0;
        if (c > 0xFF) {
            set_errno(ctx, kEILSEQ);
            if (src_out && dst) *src_out = src + 4 * i;
            return kEOF;
        }
        if (dst) {
            if (n >= len) {
                if (src_out) *src_out = src + 4 * i;
                return n;
            }
            if (!range_ok(ctx, fn, dst + n, 1)) return kEOF;
            gb(ctx)[dst + n] = static_cast<u8>(c);
        }
        if (!c) {
            if (src_out && dst) *src_out = 0;
            return n;
        }
        n++;
    }
}

u32 wcstombs_(PPUContext& ctx, u32 dst, u32 src, u32 len) { return wcs_to_mbs(ctx, "wcstombs", dst, src, len, nullptr); }

u32 wcsrtombs_(PPUContext& ctx, u32 dst, u32 srcp, u32 len, u32)
{
    if (!range_ok(ctx, "wcsrtombs", srcp, 4)) return kEOF;
    u32 src = rd(ctx, srcp);
    const u32 r = wcs_to_mbs(ctx, "wcsrtombs", dst, src, len, &src);
    if (dst) wr(ctx, srcp, src);
    return r;
}

// mbsinit: no shift states, so every state (and NULL) is the initial one.
s32 mbsinit_(PPUContext&, u32) { return 1; }
u32 btowc_(PPUContext&, s32 c) { return c == -1 ? kWEOF : static_cast<u32>(static_cast<u8>(c)); }
s32 wctob_(PPUContext&, u32 wc) { return wc <= 0xFF ? static_cast<s32>(wc) : -1; }

// ------------------------------------------------------------------------------------------------- wctype.h

// wctype's descriptors (Dinkumware numbers its classes from 1, in this order; 0 is none).
enum Class : u32 { kNone, kAlnum, kAlpha, kBlank, kCntrl, kDigit, kGraph, kLower, kPrint, kPunct, kSpace, kUpper, kXdigit };

bool is_class(u32 wc, u32 cls)
{
    if (wc > 0x7F) return false;
    const s64 c = wc;
    switch (cls) {
    case kAlnum: return c_isalpha(c) || c_isdigit(c);
    case kAlpha: return c_isalpha(c);
    case kBlank: return c == ' ' || c == '\t';
    case kCntrl: return c < 0x20 || c == 0x7F;
    case kDigit: return c_isdigit(c);
    case kGraph: return c > 0x20 && c < 0x7F;
    case kLower: return c >= 'a' && c <= 'z';
    case kPrint: return c >= 0x20 && c < 0x7F;
    case kPunct: return c > 0x20 && c < 0x7F && !c_isalpha(c) && !c_isdigit(c);
    case kSpace: return c_isspace(c);
    case kUpper: return c >= 'A' && c <= 'Z';
    case kXdigit: return c_isxdigit(c);
    default: return false;
    }
}

#define PS_WCTYPE(name, cls) s32 name##_(PPUContext&, u32 wc) { return is_class(wc, cls) ? 1 : 0; }
PS_WCTYPE(iswalnum, kAlnum)
PS_WCTYPE(iswalpha, kAlpha)
PS_WCTYPE(iswblank, kBlank)
PS_WCTYPE(iswcntrl, kCntrl)
PS_WCTYPE(iswdigit, kDigit)
PS_WCTYPE(iswgraph, kGraph)
PS_WCTYPE(iswlower, kLower)
PS_WCTYPE(iswprint, kPrint)
PS_WCTYPE(iswpunct, kPunct)
PS_WCTYPE(iswspace, kSpace)
PS_WCTYPE(iswupper, kUpper)
PS_WCTYPE(iswxdigit, kXdigit)
#undef PS_WCTYPE

s32 iswctype_(PPUContext&, u32 wc, u32 desc) { return is_class(wc, desc) ? 1 : 0; }

u32 wctype_(PPUContext& ctx, u32 name)
{
    const auto t = read_string(thread_of(ctx), name, 64);
    if (!t) {
        refuse(ctx, std::format("wctype: the name at {:#x} runs out of guest memory", name));
        return 0;
    }
    static constexpr std::string_view kNames[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
        "print", "punct", "space", "upper", "xdigit"};
    for (u32 i = 1; i < std::size(kNames); i++)
        if (*t == kNames[i]) return i;
    return 0;
}

u32 towlower_(PPUContext&, u32 wc) { return (wc >= 'A' && wc <= 'Z') ? wc + 32 : wc; }
u32 towupper_(PPUContext&, u32 wc) { return (wc >= 'a' && wc <= 'z') ? wc - 32 : wc; }

// wctrans's descriptors: 1 tolower, 2 toupper (0 none); towctrans with another leaves the character as it is.
u32 wctrans_(PPUContext& ctx, u32 name)
{
    const auto t = read_string(thread_of(ctx), name, 64);
    if (!t) {
        refuse(ctx, std::format("wctrans: the name at {:#x} runs out of guest memory", name));
        return 0;
    }
    return *t == "tolower" ? 1 : *t == "toupper" ? 2 : 0;
}

u32 towctrans_(PPUContext& ctx, u32 wc, u32 desc)
{
    return desc == 1 ? towlower_(ctx, wc) : desc == 2 ? towupper_(ctx, wc) : wc;
}

// ------------------------------------------------------------------------------------------------- Dinkumware's

// Dinkumware's internal forms the macros of its wchar.h and wctype.h call: the same work with its state and table
// arguments, which the "C" locale does not need.
u32 _Mbtowc_(PPUContext& ctx, u32 pwc, u32 s, u32 n, u32) { return mbrtowc_impl(ctx, pwc, s, n); }
u32 _Mbtowcx_(PPUContext& ctx, u32 pwc, u32 s, u32 n, u32, u32) { return mbrtowc_impl(ctx, pwc, s, n); }
u32 _Wctomb_(PPUContext& ctx, u32 s, u32 wc, u32) { return wcrtomb_impl(ctx, s, wc); }
u32 _Wctombx_(PPUContext& ctx, u32 s, u32 wc, u32, u32, u32) { return wcrtomb_impl(ctx, s, wc); }
u32 _Btowc_(PPUContext& ctx, s32 c) { return btowc_(ctx, c); }
s32 _Wctob_(PPUContext& ctx, u32 wc) { return wctob_(ctx, wc); }
s32 _Iswctype_(PPUContext&, u32 wc, u32 desc) { return is_class(wc, desc) ? 1 : 0; }

// The thread's errno (Dinkumware's errno.h: #define errno (*_Geterrno())), and its conversion and collation states,
// in its block.
u32 in_block(PPUContext& ctx, u32 offset)
{
    const u32 b = thread_block(ctx);
    return b ? b + offset : 0;
}

u32 _Geterrno_(PPUContext& ctx) { return in_block(ctx, tb::kErrno); }
u32 _Getpmbstate_(PPUContext& ctx) { return in_block(ctx, tb::kMbState); }
u32 _Getpwcstate_(PPUContext& ctx) { return in_block(ctx, tb::kWcState); }
u32 _Getpcostate_(PPUContext& ctx) { return in_block(ctx, tb::kCoState); }
u32 _Getpwcostate_(PPUContext& ctx) { return in_block(ctx, tb::kWcoState); }

// MB_CUR_MAX: 1 in the "C" locale, from the function (stdlib.h's MB_CUR_MAX is _Getmbcurmax()) or the char
// variable _Mbcurmax of Dinkumware's single-threaded headers.
s32 _Getmbcurmax_(PPUContext&) { return 1; }
void mbcurmax_init(u8* b, u32 a) { b[a] = 1; }

// The wide class and translation tables: the variables _Wctype and _Wctrans (each 13 entries of {name, value}).
u32 _Getpwctytab_(PPUContext&) { return variable(recomp::nid_of("_Wctype")); }
u32 _Getpwctrtab_(PPUContext&) { return variable(recomp::nid_of("_Wctrans")); }

// _Wctype: Dinkumware's table of {name pointer, class} (the class names live after it); _Wctrans likewise.
void wctype_table(u8* b, u32 a)
{
    static constexpr std::string_view kNames[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
        "print", "punct", "space", "upper", "xdigit"};
    u32 text = a + 8 * 13;
    for (u32 i = 0; i < std::size(kNames); i++) {
        ps_write32(b, a + 8 * i, text);
        ps_write32(b, a + 8 * i + 4, i + 1);
        std::memcpy(b + text, kNames[i].data(), kNames[i].size());
        b[text + kNames[i].size()] = 0;
        text += 8;
    }
    // A last {0, 0} entry ends it (zeroed).
}

void wctrans_table(u8* b, u32 a)
{
    ps_write32(b, a, a + 24);
    ps_write32(b, a + 4, 1);
    ps_write32(b, a + 8, a + 32);
    ps_write32(b, a + 12, 2);
    std::memcpy(b + a + 24, "tolower", 8);
    std::memcpy(b + a + 32, "toupper", 8);
}

} // namespace

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_wide(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(wcslen), LIBC(wcscpy), LIBC(wcsncpy), LIBC(wcscat), LIBC(wcsncat), LIBC(wcscmp), LIBC(wcsncmp),
            LIBC(wcscoll), LIBC(wcsxfrm), LIBC(wcschr), LIBC(wcsrchr), LIBC(wcsspn), LIBC(wcscspn), LIBC(wcspbrk),
            LIBC(wcsstr), LIBC(wcstok), LIBC(wmemchr), LIBC(wmemcmp), LIBC(wmemcpy), LIBC(wmemmove), LIBC(wmemset),
            LIBC(wcstol), LIBC(wcstoul), LIBC(wcstoll), LIBC(wcstoull), LIBC(wcstoimax), LIBC(wcstoumax),
            LIBC(wcstod), LIBC(wcstold), LIBC(wcstof), LIBC(mbrtowc), LIBC(mbrlen), LIBC(mbtowc), LIBC(mblen),
            LIBC(wcrtomb), LIBC(wctomb), LIBC(mbstowcs), LIBC(mbsrtowcs), LIBC(wcstombs), LIBC(wcsrtombs),
            LIBC(mbsinit), LIBC(btowc), LIBC(wctob), LIBC(iswalnum), LIBC(iswalpha), LIBC(iswblank), LIBC(iswcntrl),
            LIBC(iswdigit), LIBC(iswgraph), LIBC(iswlower), LIBC(iswprint), LIBC(iswpunct), LIBC(iswspace),
            LIBC(iswupper), LIBC(iswxdigit), LIBC(iswctype), LIBC(wctype), LIBC(towlower), LIBC(towupper),
            LIBC(wctrans), LIBC(towctrans), LIBC(_Mbtowc), LIBC(_Mbtowcx), LIBC(_Wctomb), LIBC(_Wctombx),
            LIBC(_Btowc), LIBC(_Wctob), LIBC(_Iswctype), LIBC(_Geterrno), LIBC(_Getpmbstate), LIBC(_Getpwcstate),
            LIBC(_Getpcostate), LIBC(_Getpwcostate), LIBC(_Getmbcurmax), LIBC(_Getpwctytab), LIBC(_Getpwctrtab),
        });
    }
    r.add_variables("sys_libc", {
        Variable{{}, "_Wctype", recomp::nid_of("_Wctype"), 8 * 13 + 8 * 12, 8, &wctype_table},
        Variable{{}, "_Wctrans", recomp::nid_of("_Wctrans"), 48, 8, &wctrans_table},
        Variable{{}, "_Mbcurmax", recomp::nid_of("_Mbcurmax"), 4, 4, &mbcurmax_init},
    });
}
#undef LIBC

} // namespace pspocket::hle
