#pragma once

// sys_libstdcxx's facets (D-561): ctype<char>, ctype<wchar_t>, the codecvts, collate, messages, numpunct, their bases'
// destructors, _Maklocstr/_Maklocchr/_Maklocbyte for wchar_t and _Getctyptab. Included by stdcxx_facets.cpp only.
// Each is Dinkumware's (the SDK's, VC8's generation) in the "C" locale, the only one there is: classification and
// case from the facet's own tables (the C library's, D-515), a byte the wide character of its value and back (D-512).

#include "stdcxx_facets.hpp"

namespace pspocket::hle::stdcxx::facets {

// The elements of [first, last) of a facet function's arguments: their count, 0 for an empty range; a reversed, ragged
// or unmapped range is logged and taken as empty.
template <u32 E>
u32 range_of(const G& g, std::string_view fn, u32 first, u32 last)
{
    if (first == last) return 0;
    if (last < first || (last - first) % E != 0) {
        log_call_site(g.ctx, g.b, std::format("{}: [{:#x}, {:#x}) is no range of elements", fn, first, last));
        return 0;
    }
    return g.need(fn, first, last - first) ? (last - first) / E : 0;
}

// ------------------------------------------------------------------------------------------------- bases

// ~ctype_base, ~codecvt_base, ~messages_base, ~money_base, ~time_base: their vtable, then facet's.
template <const char* Cls, bool Deleting>
void base_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {Cls}, Deleting);
}

// ------------------------------------------------------------------------------------------------- ctype<char>

// The facet's tables (_Ctypevec: _Table, _Tolotab, _Touptab, each a pointer to the entry for 0), ASCII's when a
// table is missing.
inline u32 table_entry(const G& g, u32 self, u32 field, u32 c, u32 ascii)
{
    const u32 tab = g.r32(self + field);
    if (tab && g.ok(tab + 2 * c, 2)) return g.r16(tab + 2 * c);
    return ascii;
}

inline u32 ascii_lower(u32 c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
inline u32 ascii_upper(u32 c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

inline u32 tolower_of(const G& g, u32 self, u32 c) { return table_entry(g, self, kCtTolo, c, ascii_lower(c)) & 0xFF; }
inline u32 toupper_of(const G& g, u32 self, u32 c) { return table_entry(g, self, kCtToup, c, ascii_upper(c)) & 0xFF; }

u32 ctc_tolower_(PPUContext& ctx, u32 self, u32 c)
{
    G g(ctx);
    return g.need("ctype<char>::do_tolower", self, kCtypeCBytes) ? tolower_of(g, self, c & 0xFF) : (c & 0xFF);
}

u32 ctc_toupper_(PPUContext& ctx, u32 self, u32 c)
{
    G g(ctx);
    return g.need("ctype<char>::do_toupper", self, kCtypeCBytes) ? toupper_of(g, self, c & 0xFF) : (c & 0xFF);
}

// do_tolower/do_toupper(char *_First, const char *_Last): in place; the end.
template <bool Upper>
u32 ctc_case_range_(PPUContext& ctx, u32 self, u32 first, u32 last)
{
    G g(ctx);
    if (!g.need("ctype<char>::do_tolower", self, kCtypeCBytes)) return last;
    const u32 n = range_of<1>(g, "ctype<char>::do_tolower", first, last);
    for (u32 i = 0; i < n; i++)
        g.w8(first + i, static_cast<u8>(Upper ? toupper_of(g, self, g.r8(first + i)) : tolower_of(g, self, g.r8(first + i))));
    return last;
}

// do_widen(char) and do_narrow(char, char): the element itself.
u32 ctc_widen_(PPUContext&, u32, u32 c) { return c & 0xFF; }
u32 ctc_narrow_(PPUContext&, u32, u32 c, u32) { return c & 0xFF; }

// do_widen(const char *, const char *, char *) and do_narrow(..., char, char *): copied; the end of the source.
u32 ctc_widen_range_(PPUContext& ctx, u32, u32 first, u32 last, u32 dest)
{
    G g(ctx);
    const u32 n = range_of<1>(g, "ctype<char>::do_widen", first, last);
    if (n && g.need("ctype<char>::do_widen", dest, n)) std::memmove(g.b + dest, g.b + first, n);
    return last;
}

u32 ctc_narrow_range_(PPUContext& ctx, u32, u32 first, u32 last, u32, u32 dest)
{
    G g(ctx);
    const u32 n = range_of<1>(g, "ctype<char>::do_narrow", first, last);
    if (n && g.need("ctype<char>::do_narrow", dest, n)) std::memmove(g.b + dest, g.b + first, n);
    return last;
}

// ~ctype<char>: _Tidy (the table freed when the facet owns it: _Delfl > 0 by free, < 0 by delete[]), then the bases.
inline void ctype_tidy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const s32 delfl = static_cast<s32>(g.r32(self + kCtDelfl));
    const u32 table = g.r32(self + kCtTable);
    if (delfl > 0) libc_fn::free(ctx, {table});
    else if (delfl < 0) op_delete(ctx, table);
}

template <u32 E, bool Deleting>
void ctype_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kCtypeC : kCtypeW, kCtypeBase}, Deleting, &ctype_tidy);
}

// ------------------------------------------------------------------------------------------------- ctype<wchar_t>

// The class of a wide character: its byte's entry in the facet's table, none past 255 (the "C" locale's wide classes
// are ASCII's, D-512).
inline u32 wclass_of(const G& g, u32 self, u32 c) { return c > 0xFF ? 0 : table_entry(g, self, kCtTable, c, 0) & 0xFFFF; }

u32 ctw_is_(PPUContext& ctx, u32 self, u32 mask, u32 c)
{
    G g(ctx);
    if (!g.need("ctype<wchar_t>::do_is", self, kCtypeWBytes)) return 0;
    return (wclass_of(g, self, c) & mask & 0xFFFF) != 0 ? 1 : 0;
}

// do_is(const wchar_t *, const wchar_t *, mask *): each element's class; the end.
u32 ctw_is_range_(PPUContext& ctx, u32 self, u32 first, u32 last, u32 dest)
{
    G g(ctx);
    if (!g.need("ctype<wchar_t>::do_is", self, kCtypeWBytes)) return last;
    const u32 n = range_of<4>(g, "ctype<wchar_t>::do_is", first, last);
    if (n && !g.need("ctype<wchar_t>::do_is", dest, 2 * n)) return last;
    for (u32 i = 0; i < n; i++) g.w16(dest + 2 * i, static_cast<u16>(wclass_of(g, self, g.r32(first + 4 * i))));
    return last;
}

// do_scan_is / do_scan_not(mask, first, last): the first element whose class has (lacks) the mask; else the end.
template <bool Is>
u32 ctw_scan_(PPUContext& ctx, u32 self, u32 mask, u32 first, u32 last)
{
    G g(ctx);
    if (!g.need("ctype<wchar_t>::do_scan_is", self, kCtypeWBytes)) return last;
    const u32 n = range_of<4>(g, "ctype<wchar_t>::do_scan_is", first, last);
    for (u32 i = 0; i < n; i++)
        if (((wclass_of(g, self, g.r32(first + 4 * i)) & mask & 0xFFFF) != 0) == Is) return first + 4 * i;
    return last;
}

// do_tolower/do_toupper(wchar_t): the table's case for a byte's character, none past 255.
inline u32 wcase_of(const G& g, u32 self, u32 c, bool upper)
{
    if (c > 0xFF) return c;
    return upper ? toupper_of(g, self, c) : tolower_of(g, self, c);
}

template <bool Upper>
u32 ctw_case_(PPUContext& ctx, u32 self, u32 c)
{
    G g(ctx);
    return g.need("ctype<wchar_t>::do_tolower", self, kCtypeWBytes) ? wcase_of(g, self, c, Upper) : c;
}

template <bool Upper>
u32 ctw_case_range_(PPUContext& ctx, u32 self, u32 first, u32 last)
{
    G g(ctx);
    if (!g.need("ctype<wchar_t>::do_tolower", self, kCtypeWBytes)) return last;
    const u32 n = range_of<4>(g, "ctype<wchar_t>::do_tolower", first, last);
    for (u32 i = 0; i < n; i++) g.w32(first + 4 * i, wcase_of(g, self, g.r32(first + 4 * i), Upper));
    return last;
}

// _Dowiden(char): mbrtowc of the byte in the facet's conversion state (the "C" locale's: its value); _Donarrow
// (wchar_t, char): wcrtomb's single byte, else the default.
u32 ctw_dowiden_(PPUContext&, u32, u32 c) { return c_widen(c); }
u32 ctw_donarrow_(PPUContext&, u32, u32 c, u32 dflt)
{
    const auto b = c_narrow(c);
    return b ? *b : (dflt & 0xFF);
}

u32 ctw_widen_range_(PPUContext& ctx, u32, u32 first, u32 last, u32 dest)
{
    G g(ctx);
    const u32 n = range_of<1>(g, "ctype<wchar_t>::do_widen", first, last);
    if (n && !g.need("ctype<wchar_t>::do_widen", dest, 4 * n)) return last;
    for (u32 i = 0; i < n; i++) g.w32(dest + 4 * i, c_widen(g.r8(first + i)));
    return last;
}

u32 ctw_narrow_range_(PPUContext& ctx, u32, u32 first, u32 last, u32 dflt, u32 dest)
{
    G g(ctx);
    const u32 n = range_of<4>(g, "ctype<wchar_t>::do_narrow", first, last);
    if (n && !g.need("ctype<wchar_t>::do_narrow", dest, n)) return last;
    for (u32 i = 0; i < n; i++) {
        const auto b = c_narrow(g.r32(first + 4 * i));
        g.w8(dest + i, b ? *b : static_cast<u8>(dflt));
    }
    return last;
}

// _Getctyptab(): the "C" locale's classification table (the C library's _Ctype, the entry for 0), which
// ctype<char>::classic_table() answers.
u32 getctyptab_(PPUContext& ctx)
{
    G g(ctx);
    const u32 v = libc_variable("_Ctype");
    return v ? g.r32(v) : 0;
}

// ------------------------------------------------------------------------------------------------- codecvt

enum Result : u32 { kOk = 0, kPartial = 1, kError = 2, kNoconv = 3 };

// codecvt_base: always no conversion, one element for one byte.
u32 cvt_base_noconv_(PPUContext&, u32) { return 1; }
u32 cvt_base_max_length_(PPUContext&, u32) { return 1; }
u32 cvt_base_encoding_(PPUContext&, u32) { return 1; }

// codecvt<char, char, _Mbstatet>: nothing converted (noconv), the next pointers at the starts.
u32 cvtc_inout_(PPUContext& ctx, u32, u32, u32 first1, u32, u32 mid1, u32 first2, u32, u32 mid2)
{
    G g(ctx);
    if (g.need("codecvt<char>::do_in", mid1, 4)) g.w32(mid1, first1);
    if (g.need("codecvt<char>::do_in", mid2, 4)) g.w32(mid2, first2);
    return kNoconv;
}

u32 cvtc_unshift_(PPUContext& ctx, u32, u32, u32 first2, u32, u32 mid2)
{
    G g(ctx);
    if (g.need("codecvt<char>::do_unshift", mid2, 4)) g.w32(mid2, first2);
    return kNoconv;
}

// do_length: min(_Count, _Last1 - _First1), one element a byte.
u32 cvtc_length_(PPUContext&, u32, u32, u32 first1, u32 last1, u32 count)
{
    const u32 n = last1 >= first1 ? last1 - first1 : 0;
    return count < n ? count : n;
}

// codecvt<wchar_t, char, _Mbstatet> in the "C" locale: a byte for each wide character up to 255.
u32 cvtw_noconv_(PPUContext&, u32) { return 0; }
u32 cvtw_max_length_(PPUContext&, u32) { return kMbLenMax; }
u32 cvtw_encoding_(PPUContext&, u32) { return 0; }

// do_in: bytes to wide characters while both have room; ok once one is converted (partial when there was input and
// none was), as Dinkumware's loop answers.
u32 cvtw_in_(PPUContext& ctx, u32, u32, u32 first1, u32 last1, u32 mid1, u32 first2, u32 last2, u32 mid2)
{
    G g(ctx);
    if (!g.need("codecvt<wchar_t>::do_in", mid1, 4) || !g.need("codecvt<wchar_t>::do_in", mid2, 4)) return kError;
    const u32 nin = range_of<1>(g, "codecvt<wchar_t>::do_in", first1, last1);
    const u32 nout = range_of<4>(g, "codecvt<wchar_t>::do_in", first2, last2);
    u32 result = nin == 0 ? kOk : kPartial, i = 0;
    for (; i < nin && i < nout; i++) {
        g.w32(first2 + 4 * i, c_widen(g.r8(first1 + i)));
        result = kOk;
    }
    g.w32(mid1, first1 + i);
    g.w32(mid2, first2 + 4 * i);
    return result;
}

// do_out: wide characters to bytes; one past 255 is an error (the pointers at it).
u32 cvtw_out_(PPUContext& ctx, u32, u32, u32 first1, u32 last1, u32 mid1, u32 first2, u32 last2, u32 mid2)
{
    G g(ctx);
    if (!g.need("codecvt<wchar_t>::do_out", mid1, 4) || !g.need("codecvt<wchar_t>::do_out", mid2, 4)) return kError;
    const u32 nin = range_of<4>(g, "codecvt<wchar_t>::do_out", first1, last1);
    const u32 nout = range_of<1>(g, "codecvt<wchar_t>::do_out", first2, last2);
    u32 result = nin == 0 ? kOk : kPartial, i = 0;
    for (; i < nin && i < nout; i++) {
        const auto b = c_narrow(g.r32(first1 + 4 * i));
        if (!b) {
            result = kError;
            break;
        }
        g.w8(first2 + i, *b);
        result = kOk;
    }
    g.w32(mid1, first1 + 4 * i);
    g.w32(mid2, first2 + i);
    return result;
}

// do_unshift: the "C" locale has no shift state: nothing to write, ok.
u32 cvtw_unshift_(PPUContext& ctx, u32, u32, u32 first2, u32, u32 mid2)
{
    G g(ctx);
    if (g.need("codecvt<wchar_t>::do_unshift", mid2, 4)) g.w32(mid2, first2);
    return kOk;
}

// do_length: the bytes making at most _Count wide characters (each byte one).
u32 cvtw_length_(PPUContext& ctx, u32 self, u32 st, u32 first1, u32 last1, u32 count)
{
    return cvtc_length_(ctx, self, st, first1, last1, count);
}

template <u32 E, bool Deleting>
void codecvt_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kCodecvtC : kCodecvtW, kCvtBase}, Deleting);
}

// ------------------------------------------------------------------------------------------------- collate

// do_compare: the "C" locale's collation is the elements' order (unsigned); -1, 0 or +1.
template <u32 E>
s32 collate_compare_(PPUContext& ctx, u32, u32 first1, u32 last1, u32 first2, u32 last2)
{
    G g(ctx);
    const u32 n1 = range_of<E>(g, "collate::do_compare", first1, last1);
    const u32 n2 = range_of<E>(g, "collate::do_compare", first2, last2);
    for (u32 i = 0; i < n1 && i < n2; i++) {
        const u32 a = el<E>(g, first1 + E * i), b = el<E>(g, first2 + E * i);
        if (a != b) return a < b ? -1 : 1;
    }
    return n1 < n2 ? -1 : n1 == n2 ? 0 : 1;
}

// do_transform: the key of [first, last) (the "C" locale's strxfrm: the elements themselves), a string by value.
template <u32 E>
void collate_transform_(PPUContext& ctx, u32 out, u32, u32 first, u32 last)
{
    G g(ctx);
    if (!g.need("collate::do_transform", out, StrLayout<E>::kBytes)) return;
    const u32 n = range_of<E>(g, "collate::do_transform", first, last);
    std::vector<u32> v(n);
    for (u32 i = 0; i < n; i++) v[i] = el<E>(g, first + E * i);
    make_str<E>(g, out, v);
}

// do_hash: Dinkumware's rotate-and-add over the elements, as a long.
template <u32 E>
u32 collate_hash_(PPUContext& ctx, u32, u32 first, u32 last)
{
    G g(ctx);
    const u32 n = range_of<E>(g, "collate::do_hash", first, last);
    u32 v = 0;
    for (u32 i = 0; i < n; i++) v = ((v << 8) | (v >> 24)) + el<E>(g, first + E * i);
    return v;
}

template <u32 E, bool Deleting>
void collate_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kCollateC : kCollateW}, Deleting);
}

// ------------------------------------------------------------------------------------------------- messages

// The SDK's messages have no catalogs: do_open answers -1 (none opened), do_get the default, do_close nothing.
s32 messages_open_(PPUContext&, u32, u32, u32) { return -1; }

template <u32 E>
void messages_get_(PPUContext& ctx, u32 out, u32, s32, s32, s32, u32 dflt)
{
    G g(ctx);
    if (!g.need("messages::do_get", out, StrLayout<E>::kBytes) ||
        !g.need("messages::do_get", dflt, StrLayout<E>::kBytes))
        return;
    make_str<E>(g, out, read_str<E>(g, dflt));
}

void messages_close_(PPUContext&, u32, s32) {}

template <u32 E, bool Deleting>
void messages_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kMessagesC : kMessagesW, kMessagesBase}, Deleting);
}

// ------------------------------------------------------------------------------------------------- numpunct

// The C string of an lconv member (a char * at `field`), "" when missing.
inline std::string lconv_str(const G& g, u32 lc, u32 field)
{
    if (!lc || !g.ok(lc + field, 4)) return {};
    return g.str(g.r32(lc + field)).value_or(std::string());
}

inline constexpr u32 kLcCurrency = 0, kLcIntCurr = 4, kLcMonDp = 8, kLcMonGrouping = 12, kLcMonSep = 16,
                     kLcNegSign = 20, kLcPosSign = 24, kLcFrac = 28, kLcNCsPrec = 29, kLcNSepSpace = 30,
                     kLcNSignPosn = 31, kLcPCsPrec = 32, kLcPSepSpace = 33, kLcPSignPosn = 34, kLcIntFrac = 35,
                     kLcDp = 44, kLcGrouping = 48, kLcSep = 52;

// _Init(const _Locinfo&): the grouping, false and true names copied (new[]), the decimal point and thousands
// separator converted from the locale's lconv (the "C" locale's: "", "false", "true", '.', none).
template <u32 E>
void numpunct_init(PPUContext& ctx, u32 self, u32)
{
    using L = NpL<E>;
    G g(ctx);
    const u32 lc = lconv_of(ctx);
    g.w32(self + L::kGrouping, 0);
    g.w32(self + L::kFalse, 0);
    g.w32(self + L::kTrue, 0);
    g.w32(self + L::kGrouping, maklocstr<1>(ctx, lconv_str(g, lc, kLcGrouping)));
    g.w32(self + L::kFalse, maklocstr<E>(ctx, "false"));
    g.w32(self + L::kTrue, maklocstr<E>(ctx, "true"));
    const std::string dp = lconv_str(g, lc, kLcDp), sep = lconv_str(g, lc, kLcSep);
    g.template wel<E>(self + L::kDp, c_widen(dp.empty() ? 0 : static_cast<u8>(dp[0])));
    g.template wel<E>(self + L::kKsep, c_widen(sep.empty() ? 0 : static_cast<u8>(sep[0])));
}

template <u32 E>
void numpunct_init_(PPUContext& ctx, u32 self, u32 lobj)
{
    G g(ctx);
    if (g.need("numpunct::_Init", self, NpL<E>::kBytes)) numpunct_init<E>(ctx, self, lobj);
}

template <u32 E>
u32 numpunct_dp_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    return g.need("numpunct::do_decimal_point", self, NpL<E>::kBytes) ? el<E>(g, self + NpL<E>::kDp) : '.';
}

template <u32 E>
u32 numpunct_sep_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    return g.need("numpunct::do_thousands_sep", self, NpL<E>::kBytes) ? el<E>(g, self + NpL<E>::kKsep) : 0;
}

// do_grouping (a string), do_falsename and do_truename (string_type): the facet's strings, by value.
template <u32 E, u32 Field, u32 StrE>
void numpunct_str_(PPUContext& ctx, u32 out, u32 self)
{
    G g(ctx);
    if (!g.need("numpunct::do_grouping", out, StrLayout<StrE>::kBytes)) return;
    std::vector<u32> v;
    if (g.need("numpunct::do_grouping", self, NpL<E>::kBytes)) v = field_cstr<StrE>(g, self, Field);
    make_str<StrE>(g, out, v);
}

template <u32 E>
void numpunct_tidy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    for (const u32 f : {NpL<E>::kGrouping, NpL<E>::kFalse, NpL<E>::kTrue}) op_delete(ctx, g.r32(self + f));
}

template <u32 E, bool Deleting>
void numpunct_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kNumpunctC : kNumpunctW}, Deleting, &numpunct_tidy<E>);
}

// ------------------------------------------------------------------------------------------------- _Makloc*<wchar_t>

// _Maklocstr<wchar_t>(const char *, wchar_t *, const _Cvtvec&): new wchar_t[] of the string converted.
u32 maklocstr_w_(PPUContext& ctx, u32 ptr, u32, u32)
{
    G g(ctx);
    const auto s = g.str(ptr);
    if (!s) {
        log_call_site(ctx, g.b, std::format("_Maklocstr: {:#x} is no string", ptr));
        return maklocstr<4>(ctx, "");
    }
    return maklocstr<4>(ctx, *s);
}

// _Maklocchr<wchar_t>(char, wchar_t *, const _Cvtvec&) and _Maklocbyte<wchar_t>(wchar_t, const _Cvtvec&): one element
// converted (a wide character with no byte is 0).
u32 maklocchr_w_(PPUContext&, u32 byte, u32, u32) { return c_widen(byte); }
u32 maklocbyte_w_(PPUContext&, u32 wc, u32) { return c_narrow(wc).value_or(0); }

// ------------------------------------------------------------------------------------------------- made by _Getcat

inline u32 make_ctype_c(PPUContext& ctx)
{
    Locinfo lobj(ctx);
    const u32 p = new_facet(ctx, kCtypeC, kCtypeCBytes);
    info_vec(ctx, fn::getctype, lobj.addr(), p + kCtTable, 16);
    return p;
}

inline u32 make_ctype_w(PPUContext& ctx)
{
    Locinfo lobj(ctx);
    const u32 p = new_facet(ctx, kCtypeW, kCtypeWBytes);
    info_vec(ctx, fn::getctype, lobj.addr(), p + kCtTable, 16);
    info_vec(ctx, fn::getcvt, lobj.addr(), p + kCtCvt, 8);
    return p;
}

template <u32 E>
u32 make_codecvt(PPUContext& ctx)
{
    Locinfo lobj(ctx);
    if constexpr (E == 1) {
        return new_facet(ctx, kCodecvtC, kPlainBytes);
    } else {
        const u32 p = new_facet(ctx, kCodecvtW, kCodecvtWBytes);
        info_vec(ctx, fn::getcvt, lobj.addr(), p + kCvtCvt, 8);
        return p;
    }
}

template <u32 E>
u32 make_collate(PPUContext& ctx)
{
    Locinfo lobj(ctx);
    const u32 p = new_facet(ctx, E == 1 ? kCollateC : kCollateW, kCollateBytes);
    info_vec(ctx, fn::getcoll, lobj.addr(), p + kCollColl, 8);
    return p;
}

// A facet whose constructor keeps nothing of the _Locinfo it makes (messages, num_get, num_put, money_get, money_put,
// time_put).
inline u32 make_plain(PPUContext& ctx, std::string_view type)
{
    Locinfo lobj(ctx);
    return new_facet(ctx, type, kPlainBytes);
}

template <u32 E>
u32 make_numpunct(PPUContext& ctx)
{
    Locinfo lobj(ctx);
    const u32 p = new_facet(ctx, E == 1 ? kNumpunctC : kNumpunctW, NpL<E>::kBytes);
    numpunct_init<E>(ctx, p, lobj.addr());
    return p;
}

} // namespace pspocket::hle::stdcxx::facets
