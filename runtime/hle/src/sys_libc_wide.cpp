// sys_libc part 2 (D-512, D-906): wide strings, multibyte conversions and wide character classes, as C99 (7.24, 7.25)
// and the SDK's C library have them. A guest wchar_t is a 32-bit big-endian word. The conversions are the SDK's
// (Dinkumware's state-table machine, libc_locale.hpp): in the "C" locale UTF-8 of up to three bytes (MB_CUR_MAX 3, the
// SDK's wchar_t being 16 bits: a wide character past 0xFFFF has no multibyte form, EILSEQ), no shift states. The
// classes and translations are the SDK's tables (_Wctype, _Wctrans): ASCII's, in its order. Registered under sys_libc
// and sys_libm with the rest of the C library.

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_locale.hpp"
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
    const u32 k = std::min<u32>(static_cast<u32>(t->size()) + 1, n); // as much as n holds, as the SDK's (D-906)
    if (k && range_ok(ctx, "wcsxfrm", d, 4 * u64{k}))
        write_wstring(thread_of(ctx), d, std::u32string_view(t->data(), std::min<usize>(k, t->size())), k > t->size());
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

// The wide forms of strtol and strtod (7.24.4.1): the same subject sequences over wide characters; one past 127
// ends them. *end set as their narrow forms.
u64 wide_int(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 base, u32 bits, bool is_signed)
{
    GuestWords in(thread_of(ctx), s);
    const auto at = [&](u64 i) -> s64 {
        const s64 c = in.at(i);
        return c > 0x7F ? 0x7F : c; // no wide character past 127 is a digit, sign or space
    };
    if (base < 0 || base == 1 || base > 36) {
        set_errno(ctx, kEINVAL);
        if (endp && range_ok(ctx, fn, endp, 4)) wr(ctx, endp, s);
        return 0;
    }
    const IntParse p = parse_int(at, base);
    bool range = false;
    const u64 v = int_result(p, bits, is_signed, &range);
    if (range) set_errno(ctx, kERANGE);
    if (endp && range_ok(ctx, fn, endp, 4)) wr(ctx, endp, p.end ? s + 4 * static_cast<u32>(p.end) : s);
    return v;
}

u32 wcstol_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(wide_int(ctx, "wcstol", s, e, b, 32, true)); }
u32 wcstoul_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(wide_int(ctx, "wcstoul", s, e, b, 32, false)); }
u64 wcstoll_(PPUContext& ctx, u32 s, u32 e, s32 b) { return wide_int(ctx, "wcstoll", s, e, b, 64, true); }
u64 wcstoull_(PPUContext& ctx, u32 s, u32 e, s32 b) { return wide_int(ctx, "wcstoull", s, e, b, 64, false); }
u64 wcstoimax_(PPUContext& ctx, u32 s, u32 e, s32 b) { return wide_int(ctx, "wcstoimax", s, e, b, 64, true); }
u64 wcstoumax_(PPUContext& ctx, u32 s, u32 e, s32 b) { return wide_int(ctx, "wcstoumax", s, e, b, 64, false); }

FloatParse wide_float(PPUContext& ctx, const char* fn, u32 s, u32 endp)
{
    const auto t = wstr(ctx, fn, s, 0x10000);
    FloatParse p{};
    if (t) {
        std::string narrow;
        for (const char32_t c : *t) {
            if (c > 0x7F) break;
            narrow += static_cast<char>(c);
        }
        p = parse_float(narrow);
    }
    if (endp && range_ok(ctx, fn, endp, 4)) wr(ctx, endp, s + 4 * static_cast<u32>(p.end));
    return p;
}

double wcstod_(PPUContext& ctx, u32 s, u32 e)
{
    const FloatParse p = wide_float(ctx, "wcstod", s, e);
    if (p.range_d) set_errno(ctx, kERANGE);
    return p.d;
}

double wcstold_(PPUContext& ctx, u32 s, u32 e)
{
    const FloatParse p = wide_float(ctx, "wcstold", s, e);
    if (p.range_d) set_errno(ctx, kERANGE);
    return p.d;
}

float wcstof_(PPUContext& ctx, u32 s, u32 e)
{
    const FloatParse p = wide_float(ctx, "wcstof", s, e);
    if (p.range_f) set_errno(ctx, kERANGE);
    return p.f;
}

// ------------------------------------------------------------------------------------------------- multibyte

constexpr u32 kEOF = 0xFFFFFFFFu, kWEOF = 0xFFFFFFFFu;

// The thread's internal states (mbtowc's, wctomb's and the restartable functions' when given no state).
u32 own_state(PPUContext& ctx, u32 offset)
{
    const u32 b = thread_block(ctx);
    return b ? b + offset : 0;
}

// _Mbtowc(pwc, s, n, ps): the thread's multibyte state tables (UTF-8 in the "C" locale).
u32 mb_to_wc(PPUContext& ctx, std::string_view fn, u32 pwc, u32 s, u32 n, u32 ps)
{
    return mbtowcx(ctx, fn, pwc, s, n, ps, mb_tables(ctx));
}

// _Wctomb(s, wc, ps): the thread's tables (the wide ones only when s is given, as the SDK's).
u32 wc_to_mb(PPUContext& ctx, std::string_view fn, u32 s, u32 wc, u32 ps)
{
    return wctombx(ctx, fn, s, wc, ps, mb_tables(ctx), s ? wc_tables(ctx) : 0);
}

// The conversion of a NUL alone (mbrtowc with no string is mbrtowc(NULL, "", 1, ps), C99 7.24.6.3.2).
u32 mb_nul(PPUContext& ctx, std::string_view fn, u32 ps)
{
    if (!range_ok(ctx, fn, ps, 8)) return kEOF;
    MbState st = get_mbstate(gb(ctx), ps);
    bool unreadable = false;
    const MbStep r = mbtowc_step(ctx, [](u64 i) { return i == 0 ? 0 : -1; }, 1, st, mb_tables(ctx), &unreadable);
    if (r.write != StateWrite::None) ps_write16(gb(ctx), ps + 6, st.state);
    if (r.write == StateWrite::Both) wr(ctx, ps, st.wchar);
    return r.result;
}

// mbrtowc (7.24.6.3.2): the next multibyte character by the locale's encoding (UTF-8 in the "C" locale, as the SDK's:
// up to three bytes, (size_t)-2 for an incomplete one, -1 and EILSEQ for a bad one); its state the caller's or its
// own.
u32 mbrtowc_(PPUContext& ctx, u32 pwc, u32 s, u32 n, u32 ps)
{
    if (!ps) ps = own_state(ctx, tb::kMbrtowc);
    return s ? mb_to_wc(ctx, "mbrtowc", pwc, s, n, ps) : mb_nul(ctx, "mbrtowc", ps);
}

u32 mbrlen_(PPUContext& ctx, u32 s, u32 n, u32 ps)
{
    if (!ps) ps = own_state(ctx, tb::kMbrlen);
    return s ? mb_to_wc(ctx, "mbrlen", 0, s, n, ps) : mb_nul(ctx, "mbrlen", ps);
}

// mbtowc and mblen (7.20.7.1, 7.20.7.2): their own states; no string resets it and answers whether the encoding has
// shift states (none in the "C" locale); an incomplete or bad character is -1 (the state reset).
s32 mb_int(PPUContext& ctx, std::string_view fn, u32 pwc, u32 s, u32 n, u32 state)
{
    const u32 r = mb_to_wc(ctx, fn, pwc, s, n, state);
    if (s && (r == kEOF || r == kMbPartial)) {
        if (state && range_ok(ctx, fn, state, 8)) std::memset(gb(ctx) + state, 0, 8);
        return -1;
    }
    return static_cast<s32>(r);
}

s32 mbtowc_(PPUContext& ctx, u32 pwc, u32 s, u32 n) { return mb_int(ctx, "mbtowc", pwc, s, n, own_state(ctx, tb::kMbState)); }
s32 mblen_(PPUContext& ctx, u32 s, u32 n) { return mb_int(ctx, "mblen", 0, s, n, own_state(ctx, tb::kCoState)); }

// wcrtomb (7.24.6.3.3): the multibyte form of wc (UTF-8 in the "C" locale: a wide character past 0xFFFF has none,
// EILSEQ); no array is wcrtomb(buf, L'\0', ps).
u32 wcrtomb_(PPUContext& ctx, u32 s, u32 wc, u32 ps)
{
    if (!ps) ps = own_state(ctx, tb::kWcrtomb);
    if (s) return wc_to_mb(ctx, "wcrtomb", s, wc, ps);
    StackVar buf(ctx, gb(ctx), 16);
    return wc_to_mb(ctx, "wcrtomb", buf.addr(), 0, ps);
}

// wctomb (7.20.7.3): its own state; no array resets it (no shift states in the "C" locale: 0).
s32 wctomb_(PPUContext& ctx, u32 s, u32 wc) { return static_cast<s32>(wc_to_mb(ctx, "wctomb", s, wc, own_state(ctx, tb::kWcState))); }

// mbsrtowcs (7.24.6.4.1): multibyte characters to wide ones up to the NUL or `len` of them; *src set past the last
// converted (NULL after the NUL), at the bad one on EILSEQ. With no array it counts them all. mbstowcs is it from the
// initial state with no pointer to update.
u32 mbs_to_wcs(PPUContext& ctx, std::string_view fn, u32 dst, u32 src, u32 len, u32 ps, u32* src_out)
{
    StackVar wcv(ctx, gb(ctx), 8);
    u32 s = src, n = 0;
    while (!dst || n < len) {
        const u32 r = mb_to_wc(ctx, fn, wcv.addr(), s, 0x7FFFFFFF, ps);
        if (r == kEOF || r == kMbPartial) {
            if (src_out && dst) *src_out = s;
            return kEOF;
        }
        const u32 wc = r == 0 ? 0 : rd(ctx, wcv.addr());
        if (dst) {
            if (!range_ok(ctx, fn, dst + 4 * n, 4)) return kEOF;
            wr(ctx, dst + 4 * n, wc);
        }
        if (r == 0) {
            if (src_out && dst) *src_out = 0;
            return n;
        }
        s += r;
        n++;
    }
    if (src_out) *src_out = s;
    return n;
}

u32 mbstowcs_(PPUContext& ctx, u32 dst, u32 src, u32 len)
{
    StackVar st(ctx, gb(ctx), 8);
    return mbs_to_wcs(ctx, "mbstowcs", dst, src, len, st.addr(), nullptr);
}

u32 mbsrtowcs_(PPUContext& ctx, u32 dst, u32 srcp, u32 len, u32 ps)
{
    if (!range_ok(ctx, "mbsrtowcs", srcp, 4)) return kEOF;
    if (!ps) ps = own_state(ctx, tb::kMbsrtowcs);
    u32 src = rd(ctx, srcp);
    const u32 r = mbs_to_wcs(ctx, "mbsrtowcs", dst, src, len, ps, &src);
    if (dst) wr(ctx, srcp, src);
    return r;
}

// wcsrtombs (7.24.6.4.2): wide characters to multibyte ones, while they fit in `len` bytes (the NUL stored but not
// counted); *src as mbsrtowcs. wcstombs from the initial state.
u32 wcs_to_mbs(PPUContext& ctx, std::string_view fn, u32 dst, u32 src, u32 len, u32 ps, u32* src_out)
{
    StackVar buf(ctx, gb(ctx), 16);
    u32 n = 0;
    for (u32 i = 0;; i++) {
        const u32 at = src + 4 * i;
        if (!range_ok(ctx, fn, at, 4)) return kEOF;
        const u32 wc = rd(ctx, at);
        const u32 r = wc_to_mb(ctx, fn, buf.addr(), wc, ps);
        if (r == kEOF) {
            if (src_out && dst) *src_out = at;
            return kEOF;
        }
        if (dst) {
            if (n + r > len) {
                if (src_out) *src_out = at;
                return n;
            }
            if (!range_ok(ctx, fn, dst + n, r)) return kEOF;
            std::memcpy(gb(ctx) + dst + n, gb(ctx) + buf.addr(), r);
        }
        if (wc == 0) {
            if (src_out && dst) *src_out = 0;
            return n;
        }
        n += r;
    }
}

u32 wcstombs_(PPUContext& ctx, u32 dst, u32 src, u32 len)
{
    StackVar st(ctx, gb(ctx), 8);
    return wcs_to_mbs(ctx, "wcstombs", dst, src, len, st.addr(), nullptr);
}

u32 wcsrtombs_(PPUContext& ctx, u32 dst, u32 srcp, u32 len, u32 ps)
{
    if (!range_ok(ctx, "wcsrtombs", srcp, 4)) return kEOF;
    if (!ps) ps = own_state(ctx, tb::kWcsrtombs);
    u32 src = rd(ctx, srcp);
    const u32 r = wcs_to_mbs(ctx, "wcsrtombs", dst, src, len, ps, &src);
    if (dst) wr(ctx, srcp, src);
    return r;
}

// mbsinit: the initial state (no state, or none pending).
s32 mbsinit_(PPUContext& ctx, u32 ps)
{
    if (!ps) return 1;
    if (!range_ok(ctx, "mbsinit", ps, 8)) return 0;
    return ps_read16(gb(ctx), ps + 6) == 0 ? 1 : 0;
}

// btowc: a byte as a one-byte multibyte character (WEOF when it is none: in the "C" locale, any byte past 0x7F);
// wctob: a wide character whose multibyte form is one byte, that byte, else EOF.
u32 btowc_(PPUContext& ctx, s32 c)
{
    if (c == -1) return kWEOF;
    const u8 byte = static_cast<u8>(c);
    MbState st;
    bool unreadable = false;
    const MbStep r = mbtowc_step(ctx, [&](u64 i) { return i == 0 ? static_cast<int>(byte) : -1; }, 1, st,
        mb_tables(ctx), &unreadable);
    if (r.result == kEOF || r.result == kMbPartial) return kWEOF;
    return r.wc;
}

s32 wctob_(PPUContext& ctx, u32 wc)
{
    if (wc == kWEOF) return -1;
    StackVar buf(ctx, gb(ctx), 16);
    StackVar st(ctx, gb(ctx), 8);
    const u32 r = wctombx(ctx, "wctob", buf.addr(), wc, st.addr(), mb_tables(ctx), wc_tables(ctx));
    return r == 1 ? gb(ctx)[buf.addr()] : -1;
}

// ------------------------------------------------------------------------------------------------- wctype.h

// The wide class and translation tables, the SDK's layout: the variable holds the table's address; the table is
// {ranges, 0}, then {name, first range} for each class (wctype's descriptor its index, from 1), then {NULL, end};
// a range is three halfwords {lo, hi, value}, a character in it having the value (1 for a class, the delta added for
// a translation). The SDK's "C" classes are ASCII's in its order: alnum, alpha, cntrl (0..0x1F: not DEL), digit,
// graph, lower, print, punct, space, upper, xdigit, blank. Its library has no translation table; tolower and toupper
// are laid out the same way.
struct Range {
    u16 lo, hi, value;
};

struct ClassDef {
    std::string_view name;
    std::vector<Range> ranges;
};

const std::vector<ClassDef>& c_classes()
{
    static const std::vector<ClassDef> k = {
        {"alnum", {{'A', 'Z', 1}, {'a', 'z', 1}, {'0', '9', 1}}},
        {"alpha", {{'A', 'Z', 1}, {'a', 'z', 1}}},
        {"cntrl", {{0x00, 0x1F, 1}}},
        {"digit", {{'0', '9', 1}}},
        {"graph", {{0x21, 0x7E, 1}}},
        {"lower", {{'a', 'z', 1}}},
        {"print", {{0x20, 0x7E, 1}}},
        {"punct", {{0x21, 0x2F, 1}, {0x3A, 0x40, 1}, {0x5B, 0x60, 1}, {0x7B, 0x7E, 1}}},
        {"space", {{0x20, 0x20, 1}, {0x09, 0x0D, 1}}},
        {"upper", {{'A', 'Z', 1}}},
        {"xdigit", {{'0', '9', 1}, {'a', 'f', 1}, {'A', 'F', 1}}},
        {"blank", {{0x20, 0x20, 1}, {0x09, 0x09, 1}}},
    };
    return k;
}

const std::vector<ClassDef>& c_translations()
{
    static const std::vector<ClassDef> k = {
        {"tolower", {{'A', 'Z', 32}}},
        {"toupper", {{'a', 'z', static_cast<u16>(-32)}}},
    };
    return k;
}

u32 wide_table_bytes(const std::vector<ClassDef>& defs)
{
    u32 n = 8 + 8 * static_cast<u32>(defs.size() + 2);
    for (const ClassDef& d : defs) n += 6 * static_cast<u32>(d.ranges.size()) + static_cast<u32>(d.name.size()) + 1;
    return (n + 7) & ~7u;
}

void lay_wide_table(u8* b, u32 a, const std::vector<ClassDef>& defs)
{
    const u32 table = a + 8;
    const u32 ranges = table + 8 * static_cast<u32>(defs.size() + 2);
    u32 count = 0;
    for (const ClassDef& d : defs) count += static_cast<u32>(d.ranges.size());
    u32 text = ranges + 6 * count;
    ps_write32(b, a, table);
    ps_write32(b, table, ranges);
    ps_write32(b, table + 4, 0);
    u32 r = 0;
    for (u32 k = 0; k < defs.size(); k++) {
        ps_write32(b, table + 8 * (k + 1), text);
        ps_write32(b, table + 8 * (k + 1) + 4, 3 * r);
        std::memcpy(b + text, defs[k].name.data(), defs[k].name.size());
        b[text + defs[k].name.size()] = 0;
        text += static_cast<u32>(defs[k].name.size()) + 1;
        for (const Range& g : defs[k].ranges) {
            ps_write16(b, ranges + 6 * r, g.lo);
            ps_write16(b, ranges + 6 * r + 2, g.hi);
            ps_write16(b, ranges + 6 * r + 4, g.value);
            r++;
        }
    }
    ps_write32(b, table + 8 * static_cast<u32>(defs.size() + 1), 0);
    ps_write32(b, table + 8 * static_cast<u32>(defs.size() + 1) + 4, 3 * r);
}

void wctype_table(u8* b, u32 a) { lay_wide_table(b, a, c_classes()); }
void wctrans_table(u8* b, u32 a) { lay_wide_table(b, a, c_translations()); }

u32 table_of(PPUContext& ctx, std::string_view var_name) { return rd(ctx, variable(recomp::nid_of(var_name))); }

// The value of the first range of entry `desc` of the table holding wc (0 when none, or no such entry), as the SDK's
// _Iswctype searches (the character compared as an int).
std::optional<u32> lookup(PPUContext& ctx, u32 table, u32 desc, u32 wc)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!table || desc == 0 || !guest_readable(t, table, 8)) return std::nullopt;
    for (u32 k = 1; k <= desc; k++)
        if (!guest_readable(t, table + 8 * k, 8) || !rd(ctx, table + 8 * k)) return std::nullopt;
    const u32 ranges = rd(ctx, table);
    const u32 from = rd(ctx, table + 8 * desc + 4), to = rd(ctx, table + 8 * desc + 12);
    const s32 c = static_cast<s32>(wc);
    for (u32 r = from; r + 3 <= to; r += 3) {
        const u32 at = ranges + 2 * r;
        if (!guest_readable(t, at, 6)) return std::nullopt;
        if (static_cast<s32>(ps_read16(gb(ctx), at)) <= c && static_cast<s32>(ps_read16(gb(ctx), at + 2)) >= c)
            return ps_read16(gb(ctx), at + 4);
    }
    return std::nullopt;
}

s32 iswctype_(PPUContext& ctx, u32 wc, u32 desc) { return static_cast<s32>(lookup(ctx, table_of(ctx, "_Wctype"), desc, wc).value_or(0)); }

// The SDK's descriptors, in its table's order.
enum Class : u32 { kNone, kAlnum, kAlpha, kCntrl, kDigit, kGraph, kLower, kPrint, kPunct, kSpace, kUpper, kXdigit, kBlank };

#define PS_WCTYPE(name, cls) s32 name##_(PPUContext& ctx, u32 wc) { return iswctype_(ctx, wc, cls) ? 1 : 0; }
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

// The descriptor of a named entry of a table (0 for none).
u32 named(PPUContext& ctx, std::string_view fn, u32 table, u32 name)
{
    const auto n = read_string(thread_of(ctx), name, 256);
    if (!n) {
        refuse(ctx, std::format("{}: the name at {:#x} runs out of guest memory", fn, name));
        return 0;
    }
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 k = 1; table && guest_readable(t, table + 8 * k, 4) && rd(ctx, table + 8 * k); k++)
        if (read_string(t, rd(ctx, table + 8 * k), n->size() + 1).value_or("") == *n) return k;
    return 0;
}

u32 wctype_(PPUContext& ctx, u32 name) { return named(ctx, "wctype", table_of(ctx, "_Wctype"), name); }
u32 wctrans_(PPUContext& ctx, u32 name) { return named(ctx, "wctrans", table_of(ctx, "_Wctrans"), name); }

// towctrans: the delta of the translation's range holding wc added (in 16 bits, as the SDK's wide characters are);
// a character in no range, or another descriptor, is itself.
u32 towctrans_(PPUContext& ctx, u32 wc, u32 desc)
{
    const auto v = lookup(ctx, table_of(ctx, "_Wctrans"), desc, wc);
    return v ? (wc + *v) & 0xFFFF : wc;
}

u32 towlower_(PPUContext& ctx, u32 wc) { return towctrans_(ctx, wc, 1); }
u32 towupper_(PPUContext& ctx, u32 wc) { return towctrans_(ctx, wc, 2); }

// ------------------------------------------------------------------------------------------------- Dinkumware's

// Dinkumware's internal forms the macros of its wchar.h and wctype.h call.
u32 _Mbtowc_(PPUContext& ctx, u32 pwc, u32 s, u32 n, u32 ps) { return mb_to_wc(ctx, "_Mbtowc", pwc, s, n, ps); }
u32 _Mbtowcx_(PPUContext& ctx, u32 pwc, u32 s, u32 n, u32 ps, u32 tables) { return mbtowcx(ctx, "_Mbtowcx", pwc, s, n, ps, tables); }
u32 _Wctomb_(PPUContext& ctx, u32 s, u32 wc, u32 ps) { return wc_to_mb(ctx, "_Wctomb", s, wc, ps); }
u32 _Wctombx_(PPUContext& ctx, u32 s, u32 wc, u32 ps, u32 mbtab, u32 wctab) { return wctombx(ctx, "_Wctombx", s, wc, ps, mbtab, wctab); }
u32 _Btowc_(PPUContext& ctx, s32 c) { return btowc_(ctx, c); }
s32 _Wctob_(PPUContext& ctx, u32 wc) { return wctob_(ctx, wc); }
s32 _Iswctype_(PPUContext& ctx, u32 wc, u32 desc) { return iswctype_(ctx, wc, desc); }

// The thread's errno (Dinkumware's errno.h: #define errno (*_Geterrno())), and its multibyte, wide, collation and wide
// collation state tables (_Statab, thread-local in the SDK's; all NULL: the "C" locale's UTF-8 and code order).
u32 _Geterrno_(PPUContext& ctx) { return own_state(ctx, tb::kErrno); }
u32 _Getpmbstate_(PPUContext& ctx) { return mb_tables(ctx); }
u32 _Getpwcstate_(PPUContext& ctx) { return wc_tables(ctx); }
u32 _Getpcostate_(PPUContext& ctx) { return co_tables(ctx); }
u32 _Getpwcostate_(PPUContext& ctx) { return wco_tables(ctx); }

// MB_CUR_MAX (stdlib.h's _Getmbcurmax(), or the char variable _Mbcurmax): 3 in the SDK's "C" locale (UTF-8 of up to
// three bytes).
s32 _Getmbcurmax_(PPUContext& ctx)
{
    const u32 v = variable(recomp::nid_of("_Mbcurmax"));
    return v ? static_cast<s8>(gb(ctx)[v]) : 3;
}
void mbcurmax_init(u8* b, u32 a) { b[a] = 3; }

// The wide class and translation tables (the tables the variables point at, as the SDK's _Getpwctytab answers).
u32 _Getpwctytab_(PPUContext& ctx) { return table_of(ctx, "_Wctype"); }
u32 _Getpwctrtab_(PPUContext& ctx) { return table_of(ctx, "_Wctrans"); }

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
        Variable{{}, "_Wctype", recomp::nid_of("_Wctype"), wide_table_bytes(c_classes()), 8, &wctype_table},
        Variable{{}, "_Wctrans", recomp::nid_of("_Wctrans"), wide_table_bytes(c_translations()), 8, &wctrans_table},
        Variable{{}, "_Mbcurmax", recomp::nid_of("_Mbcurmax"), 4, 4, &mbcurmax_init},
    });
}
#undef LIBC

} // namespace pspocket::hle
