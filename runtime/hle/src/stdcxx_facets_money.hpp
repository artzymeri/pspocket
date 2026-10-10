#pragma once

// sys_libstdcxx's facets (D-561): _Mpunct and moneypunct, money_get and money_put, Dinkumware's (xlocmon of the SDK's
// library, VC8's generation). Included by stdcxx_facets.cpp only.
// - _Mpunct holds what moneypunct<_Elem, _Intl> answers, from the locale's lconv (the "C" locale's: no symbol, no
//   decimal point or separator, signs "" and "-", 0 fraction digits, patterns "$+xv"): Dinkumware's money_base parts
//   are characters ('$' symbol, '+' sign, ' ' space, 'v' value, 'x' none).
// - money_get reads by neg_format(): the symbol (required with showbase, else matched only when the input has it where
//   it is not last), the sign (the first character of positive_sign or negative_sign, the rest required after the
//   value; a missing sign is positive unless only the negative sign is empty), white space (required for space,
//   optional for none, nothing at the end), the value (digits checked against the grouping, then frac_digits digits
//   after the decimal point, padded with zeros). The result is the digits with a '-' for a negative value, widened.
// - money_put puts the sign's first character, the symbol (with showbase), the digits with separators and the decimal
//   point by pos_format() or neg_format(), the sign's rest at the end, fill for the width (internal: at space or none).

#include "stdcxx_facets_num.hpp"

namespace pspocket::hle::stdcxx::facets {

// _Mpunct's virtual functions after the destructors.
inline constexpr u32 kMpDp = 2, kMpSep = 3, kMpGrouping = 4, kMpCurr = 5, kMpPos = 6, kMpNeg = 7, kMpFrac = 8,
                     kMpPosFmt = 9, kMpNegFmt = 10;

using Pattern = std::array<u8, 4>;

// _Makpat: the pattern for a sign's sep_by_space, cs_precedes and sign_posn; "$+xv" for the international format or
// values out of range (the "C" locale's CHAR_MAX), else C99's arrangement (7.11.2.1) in Dinkumware's characters.
inline Pattern makpat(bool intl, u32 sepbyspace, u32 csprecedes, u32 signposn)
{
    static constexpr const char* kTable[3][2][5] = {
        {{"+v$x", "+v$x", "v$+x", "v+$x", "v$+x"}, {"+$vx", "+$vx", "$v+x", "+$vx", "$+vx"}},
        {{"+v $", "+v $", "v $+", "v +$", "v $+"}, {"+$ v", "+$ v", "$ v+", "+$ v", "$+ v"}},
        {{"+ v$", "+ v$", "v$ +", "v+ $", "v$ +"}, {"+ $v", "+ $v", "$v +", "+ $v", "$ +v"}},
    };
    const char* p = intl || 2 < sepbyspace || 1 < csprecedes || 4 < signposn ? "$+xv"
                                                                              : kTable[sepbyspace][csprecedes][signposn];
    return {static_cast<u8>(p[0]), static_cast<u8>(p[1]), static_cast<u8>(p[2]), static_cast<u8>(p[3])};
}

// _Mpunct::_Init(const _Locinfo&): the strings copied (new[]) and the patterns made from the lconv; _International
// set before it, by the constructor.
template <u32 E>
void mpunct_init(PPUContext& ctx, u32 self, u32)
{
    using L = MpL<E>;
    G g(ctx);
    const u32 lc = lconv_of(ctx);
    const bool intl = g.r8(self + L::kIntl) != 0;
    const auto ch = [&](u32 field) -> u32 { return lc && g.ok(lc + field, 1) ? g.r8(lc + field) : kCharMax; };
    for (const u32 f : {L::kGrouping, L::kCurr, L::kPlus, L::kMinus}) g.w32(self + f, 0);
    g.w32(self + L::kGrouping, maklocstr<1>(ctx, lconv_str(g, lc, kLcMonGrouping)));
    g.w32(self + L::kCurr, maklocstr<E>(ctx, lconv_str(g, lc, intl ? kLcIntCurr : kLcCurrency)));
    g.w32(self + L::kPlus, maklocstr<E>(ctx, 4 < ch(kLcPSignPosn) ? std::string() : lconv_str(g, lc, kLcPosSign)));
    g.w32(self + L::kMinus, maklocstr<E>(ctx, 4 < ch(kLcNSignPosn) ? std::string("-") : lconv_str(g, lc, kLcNegSign)));
    const std::string dp = lconv_str(g, lc, kLcMonDp), sep = lconv_str(g, lc, kLcMonSep);
    g.template wel<E>(self + L::kDp, c_widen(dp.empty() ? 0 : static_cast<u8>(dp[0])));
    g.template wel<E>(self + L::kKsep, c_widen(sep.empty() ? 0 : static_cast<u8>(sep[0])));
    s32 frac = static_cast<s32>(ch(intl ? kLcIntFrac : kLcFrac));
    if (frac < 0 || static_cast<s32>(kCharMax) <= frac) frac = 0;
    g.w32(self + L::kFrac, static_cast<u32>(frac));
    const Pattern pos = makpat(intl, ch(kLcPSepSpace), ch(kLcPCsPrec), ch(kLcPSignPosn));
    const Pattern neg = makpat(intl, ch(kLcNSepSpace), ch(kLcNCsPrec), ch(kLcNSignPosn));
    for (u32 i = 0; i < 4; i++) {
        g.w8(self + L::kPlusfmt + i, pos[i]);
        g.w8(self + L::kMinusfmt + i, neg[i]);
    }
}

template <u32 E>
void mpunct_init_(PPUContext& ctx, u32 self, u32 lobj)
{
    G g(ctx);
    if (g.need("_Mpunct::_Init", self, MpL<E>::kBytes)) mpunct_init<E>(ctx, self, lobj);
}

// The base-object constructors: _Mpunct(size_t _Refs, bool _Intl) (a default _Locinfo) and _Mpunct(const _Locinfo&,
// size_t, bool).
template <u32 E>
void mpunct_construct(PPUContext& ctx, u32 self, u32 refs, bool intl, u32 lobj)
{
    G g(ctx);
    std::memset(g.b + self, 0, MpL<E>::kBytes);
    g.w32(self, vtable(ctx, E == 1 ? kMpunctC : kMpunctW));
    g.w32(self + kRefs, refs);
    g.w8(self + MpL<E>::kIntl, intl ? 1 : 0);
    if (lobj) {
        mpunct_init<E>(ctx, self, lobj);
    } else {
        Locinfo info(ctx);
        mpunct_init<E>(ctx, self, info.addr());
    }
}

template <u32 E>
void mpunct_ctor_(PPUContext& ctx, u32 self, u32 refs, bool intl)
{
    G g(ctx);
    if (g.need("_Mpunct::_Mpunct", self, MpL<E>::kBytes)) mpunct_construct<E>(ctx, self, refs, intl, 0);
}

template <u32 E>
void mpunct_ctor_info_(PPUContext& ctx, u32 self, u32 lobj, u32 refs, bool intl)
{
    G g(ctx);
    if (g.need("_Mpunct::_Mpunct", self, MpL<E>::kBytes) && g.need("_Mpunct::_Mpunct", lobj, 144))
        mpunct_construct<E>(ctx, self, refs, intl, lobj);
}

template <u32 E>
u32 mpunct_el_(PPUContext& ctx, u32 self, u32 field)
{
    G g(ctx);
    return g.need("_Mpunct", self, MpL<E>::kBytes) ? el<E>(g, self + field) : 0;
}

template <u32 E>
u32 mpunct_dp_(PPUContext& ctx, u32 self)
{
    return mpunct_el_<E>(ctx, self, MpL<E>::kDp);
}

template <u32 E>
u32 mpunct_sep_(PPUContext& ctx, u32 self)
{
    return mpunct_el_<E>(ctx, self, MpL<E>::kKsep);
}

template <u32 E>
s32 mpunct_frac_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    return g.need("_Mpunct::do_frac_digits", self, MpL<E>::kBytes) ? static_cast<s32>(g.r32(self + MpL<E>::kFrac)) : 0;
}

// do_grouping (a string), do_curr_symbol, do_positive_sign, do_negative_sign (string_type): by value.
template <u32 E, u32 Field, u32 StrE>
void mpunct_str_(PPUContext& ctx, u32 out, u32 self)
{
    G g(ctx);
    if (!g.need("_Mpunct", out, StrLayout<StrE>::kBytes)) return;
    std::vector<u32> v;
    if (g.need("_Mpunct", self, MpL<E>::kBytes)) v = field_cstr<StrE>(g, self, Field);
    make_str<StrE>(g, out, v);
}

// do_pos_format, do_neg_format: the 4-byte pattern by value.
template <u32 E, u32 Field>
void mpunct_fmt_(PPUContext& ctx, u32 out, u32 self)
{
    G g(ctx);
    if (!g.need("_Mpunct", out, 4) || !g.need("_Mpunct", self, MpL<E>::kBytes)) return;
    for (u32 i = 0; i < 4; i++) g.w8(out + i, g.r8(self + Field + i));
}

template <u32 E>
void mpunct_tidy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    for (const u32 f : {MpL<E>::kGrouping, MpL<E>::kCurr, MpL<E>::kPlus, MpL<E>::kMinus}) op_delete(ctx, g.r32(self + f));
}

template <u32 E, bool Deleting>
void mpunct_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kMpunctC : kMpunctW, kMoneyBase}, Deleting, &mpunct_tidy<E>);
}

template <u32 E, bool Intl>
inline constexpr const char* moneypunct_type = E == 1 ? (Intl ? kMoneypunctC1 : kMoneypunctC0)
                                                      : (Intl ? kMoneypunctW1 : kMoneypunctW0);

template <u32 E, bool Intl, bool Deleting>
void moneypunct_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {moneypunct_type<E, Intl>, E == 1 ? kMpunctC : kMpunctW, kMoneyBase}, Deleting, &mpunct_tidy<E>);
}

// `new moneypunct<_Elem, _Intl>`: _Mpunct's construction with the default _Locinfo, the derived class's vtable.
template <u32 E, bool Intl>
u32 make_moneypunct(PPUContext& ctx)
{
    const u32 p = op_new(ctx, MpL<E>::kBytes);
    mpunct_construct<E>(ctx, p, 0, Intl, 0);
    G(ctx).w32(p, vtable(ctx, moneypunct_type<E, Intl>));
    return p;
}

// ------------------------------------------------------------------------------------------------- money_get

template <u32 E>
Pattern vpattern(PPUContext& ctx, u32 mp, u32 slot)
{
    G g(ctx);
    StackVar tmp(ctx, g.b, 8);
    vcall(ctx, mp, slot, {tmp.addr(), mp});
    return {g.r8(tmp.addr()), g.r8(tmp.addr() + 1), g.r8(tmp.addr() + 2), g.r8(tmp.addr() + 3)};
}

// The index of an element among the widened atoms "0123456789-" (11 when it is none of them).
inline usize atom_of(const std::vector<u32>& atoms, u32 c)
{
    for (usize i = 0; i < atoms.size(); i++)
        if (atoms[i] == c) return i;
    return atoms.size();
}

// The C locale's white space test the parse uses for space and none (ctype's is(space, c) on the stream's facet).
template <u32 E>
bool is_space(PPUContext& ctx, u32 ct, u32 c)
{
    if constexpr (E == 1) {
        G g(ctx);
        return (table_entry(g, ct, kCtTable, c & 0xFF, 0) & (0x40 | 0x04 | 0x100)) != 0; // _CN|_SP|_XS
    } else {
        return vint(ctx, ct, 2, {ct, 0x40 | 0x04 | 0x100, c}) & 0xFF;
    }
}

// _Getmfld: the monetary field from [first, last): the digits (widened atoms) with a leading '-' atom when negative,
// empty when the input does not match.
template <u32 E>
std::vector<u32> getmfld(PPUContext& ctx, InIt<E>& first, InIt<E>& last, bool intl, const Ios& ios)
{
    const u32 mp = use_facet_of(ctx, ios.loc(), intl ? Of<E>::moneypunct1 : Of<E>::moneypunct0);
    const u32 ct = use_facet_of(ctx, ios.loc(), Of<E>::ctype);
    const std::vector<u32> atoms = widen_all<E>(ctx, ct, "0123456789-");
    bool bad = false, neg = false;
    std::vector<u32> sign, val;
    const Pattern pattern = vpattern<E>(ctx, mp, kMpNegFmt);
    for (usize off = 0; !bad && off < 4; ++off) {
        switch (pattern[off]) {
        case '$': {
            const std::vector<u32> symbol = vstring<E>(ctx, mp, kMpCurr);
            const u32 sym0 = symbol.empty() ? 0 : symbol[0];
            const bool showbase = (ios.flags() & kShowbase) != 0;
            if ((!showbase && !at_end(ctx, first, last) && first(ctx) != sym0) ||
                (off == 3 && sign.size() <= 1 && (at_end(ctx, first, last) || first(ctx) != sym0))) {
                // optional and absent, or optional at the end
            } else {
                usize i = 0;
                for (; !at_end(ctx, first, last) && i < symbol.size() && first(ctx) == symbol[i]; ++i, first.inc(ctx)) {
                }
                if (i != symbol.size()) bad = true;
            }
            break;
        }
        case '+':
            if (!at_end(ctx, first, last)) {
                const std::vector<u32> pos = vstring<E>(ctx, mp, kMpPos), negs = vstring<E>(ctx, mp, kMpNeg);
                if (!pos.empty() && pos[0] == first(ctx)) {
                    first.inc(ctx);
                    sign = pos;
                } else if (!negs.empty() && negs[0] == first(ctx)) {
                    first.inc(ctx);
                    sign = negs;
                    neg = true;
                } else if (!pos.empty() && negs.empty()) {
                    neg = true;
                }
            }
            break;
        case 'v': {
            u32 fracseen = 0;
            const s32 fracdigits = static_cast<s32>(vint(ctx, mp, kMpFrac, {mp}));
            const std::vector<u32> grouping = vstring<1>(ctx, mp, kMpGrouping);
            const u32 ksep = grouping.empty() ? 0 : elem_of<E>(vint(ctx, mp, kMpSep, {mp}));
            usize idx = 0;
            if (ksep == 0 || kCharMax <= group_at(grouping, 0)) {
                for (; !at_end(ctx, first, last) && (idx = atom_of(atoms, first(ctx))) < 10; first.inc(ctx))
                    val.push_back(atoms[idx]);
            } else {
                std::vector<u32> groups(1, 0);
                usize group = 0;
                for (; !at_end(ctx, first, last); first.inc(ctx)) {
                    if ((idx = atom_of(atoms, first(ctx))) < 10) {
                        val.push_back(atoms[idx]);
                        if (groups[group] != kCharMax) ++groups[group];
                    } else if (groups[group] == 0 || first(ctx) != ksep) {
                        break;
                    } else {
                        groups.push_back(0);
                        ++group;
                    }
                }
                if (group == 0) {
                } else if (0 < groups[group]) {
                    ++group;
                } else {
                    bad = true;
                }
                for (usize pg = 0; !bad && 0 < group;) {
                    const u32 g0 = group_at(grouping, pg);
                    if (g0 == kCharMax) break;
                    if ((0 < --group && g0 != groups[group]) || (0 == group && g0 < groups[group])) bad = true;
                    else if (0 < group_at(grouping, pg + 1)) ++pg;
                }
            }
            if (val.empty()) {
                bad = true;
            } else if (!at_end(ctx, first, last) && first(ctx) == elem_of<E>(vint(ctx, mp, kMpDp, {mp}))) {
                for (first.inc(ctx); !at_end(ctx, first, last) && static_cast<s32>(fracseen) < fracdigits &&
                     (idx = atom_of(atoms, first(ctx))) < 10;
                     first.inc(ctx)) {
                    val.push_back(atoms[idx]);
                    ++fracseen;
                }
                if (static_cast<s32>(fracseen) < fracdigits) bad = true;
            }
            if (!bad)
                for (; static_cast<s32>(fracseen) < fracdigits; ++fracseen) val.push_back(atoms[0]);
            break;
        }
        case ' ':
        case 'x': {
            if (off == 3) break; // space at the end is not read
            bool seen = false;
            for (; !at_end(ctx, first, last) && is_space<E>(ctx, ct, first(ctx)); first.inc(ctx)) seen = true;
            if (pattern[off] == ' ' && !seen) bad = true;
            break;
        }
        default: break;
        }
    }
    if (!bad && 1 < sign.size()) {
        usize i = 1;
        for (; i < sign.size() && !at_end(ctx, first, last) && first(ctx) == sign[i]; ++i, first.inc(ctx)) {
        }
        if (i != sign.size()) bad = true;
    }
    if (bad) return {};
    if (neg) val.insert(val.begin(), atoms[10]);
    return val;
}

// do_get(first, last, intl, ios_base&, iostate&, long double&) and (..., string_type&).
template <u32 E, bool Str>
InIt<E> moneyget(PPUContext& ctx, InIt<E> first, InIt<E> last, bool intl, u32 ios_at, u32 state, u32 val)
{
    G g(ctx);
    const Ios ios{g, ios_at};
    const std::vector<u32> str = getmfld<E>(ctx, first, last, intl, ios);
    u32 bits = 0;
    if (at_end(ctx, first, last)) bits |= kEofbit;
    if (str.empty()) {
        bits |= kFailbit;
    } else if constexpr (Str) {
        if (g.need("money_get::do_get", val, StrLayout<E>::kBytes)) make_str<E>(g, val, str, true);
    } else {
        // the digits as chars, converted by the C library
        const u32 ct = use_facet_of(ctx, ios.loc(), Of<E>::ctype);
        const std::vector<u32> atoms = widen_all<E>(ctx, ct, "0123456789-");
        std::string text;
        for (const u32 c : str) {
            const usize i = atom_of(atoms, c);
            text += i < 10 ? static_cast<char>('0' + i) : '-';
        }
        const Converted c = convert(ctx, cfn::stoldx, text, 0, true);
        if (c.fail) bits |= kFailbit;
        else if (g.need("money_get::do_get", val, 8)) g.w64(val, c.bits);
    }
    g.w32(state, g.r32(state) | bits);
    return first;
}

template <bool Str>
void moneyget_c_(PPUContext& ctx, u32 out, u32, u64 first, u64 last, bool intl, u32 ios, u32 state, u32 val)
{
    G g(ctx);
    if (!g.need("money_get::do_get", out, 8) || !g.need("money_get::do_get", ios, kIosBaseBytes) ||
        !g.need("money_get::do_get", state, 4))
        return;
    moneyget<1, Str>(ctx, InIt<1>::regs(first), InIt<1>::regs(last), intl, ios, state, val).store(g, out);
}

template <bool Str>
void moneyget_w_(PPUContext& ctx, u32 out, u32, u64 f0, u64 f1, u64 l0, u64 l1, bool intl, u32 ios, u32 state,
    u32 val)
{
    G g(ctx);
    if (!g.need("money_get::do_get", out, 12) || !g.need("money_get::do_get", ios, kIosBaseBytes) ||
        !g.need("money_get::do_get", state, 4))
        return;
    moneyget<4, Str>(ctx, InIt<4>::regs(f0, f1), InIt<4>::regs(l0, l1), intl, ios, state, val).store(g, out);
}

// The exported _Getmfld(_InIt&, _InIt&, bool, ios_base&): the field as a string_type by value.
template <u32 E>
void getmfld_(PPUContext& ctx, u32 out, u32, u32 first_at, u32 last_at, bool intl, u32 ios)
{
    G g(ctx);
    if (!g.need("money_get::_Getmfld", out, StrLayout<E>::kBytes) ||
        !g.need("money_get::_Getmfld", first_at, InIt<E>::kBytes) ||
        !g.need("money_get::_Getmfld", last_at, InIt<E>::kBytes) || !g.need("money_get::_Getmfld", ios, kIosBaseBytes))
        return;
    InIt<E> first = InIt<E>::load(g, first_at), last = InIt<E>::load(g, last_at);
    const std::vector<u32> v = getmfld<E>(ctx, first, last, intl, Ios{g, ios});
    first.store(g, first_at);
    last.store(g, last_at);
    make_str<E>(g, out, v);
}

template <u32 E, bool Deleting>
void moneyget_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kMoneyGetC : kMoneyGetW}, Deleting);
}

// ------------------------------------------------------------------------------------------------- money_put

// _Putmfld: the digits `val` (widened) put as the locale's pattern for their sign says.
template <u32 E>
void putmfld(PPUContext& ctx, OutIt<E>& dest, bool intl, const Ios& ios, u32 fill, bool neg, std::vector<u32> val)
{
    const u32 ct = use_facet_of(ctx, ios.loc(), Of<E>::ctype);
    const std::vector<u32> atoms = widen_all<E>(ctx, ct, "0123456789");
    const u32 mp = use_facet_of(ctx, ios.loc(), intl ? Of<E>::moneypunct1 : Of<E>::moneypunct0);
    const std::vector<u32> grouping = vstring<1>(ctx, mp, kMpGrouping);
    const std::vector<u32> sign = vstring<E>(ctx, mp, neg ? kMpNeg : kMpPos);
    const std::vector<u32> symbol = vstring<E>(ctx, mp, kMpCurr);
    const Pattern pattern = vpattern<E>(ctx, mp, neg ? kMpNegFmt : kMpPosFmt);
    const s32 ifrac = static_cast<s32>(vint(ctx, mp, kMpFrac, {mp}));
    const usize frac = static_cast<usize>(ifrac < 0 ? -ifrac : ifrac);
    if (val.size() <= frac) {
        val.insert(val.begin(), frac - val.size() + 1, atoms[0]);
    } else if (group_at(grouping, 0) != kCharMax && 0 < group_at(grouping, 0)) {
        const u32 ksep = elem_of<E>(vint(ctx, mp, kMpSep, {mp}));
        usize off = val.size() - frac;
        for (usize pg = 0; group_at(grouping, pg) != kCharMax && 0 < group_at(grouping, pg) &&
             group_at(grouping, pg) < off;) {
            off -= group_at(grouping, pg);
            val.insert(val.begin() + static_cast<std::ptrdiff_t>(off), ksep);
            if (0 < group_at(grouping, pg + 1)) ++pg;
        }
    }
    if (frac)
        val.insert(val.end() - static_cast<std::ptrdiff_t>(frac), elem_of<E>(vint(ctx, mp, kMpDp, {mp})));
    const bool showbase = (ios.flags() & kShowbase) != 0;
    usize fillcount = 0;
    for (const u8 part : pattern) {
        switch (part) {
        case '$': fillcount += showbase ? symbol.size() : 0; break;
        case '+': fillcount += sign.size(); break;
        case 'v': fillcount += val.size(); break;
        case ' ': ++fillcount; break;
        default: break;
        }
    }
    fillcount = ios.width() <= 0 || static_cast<usize>(ios.width()) <= fillcount ? 0 : static_cast<usize>(ios.width()) - fillcount;
    const u32 adjust = ios.flags() & kAdjustfield;
    if (adjust != kLeft && adjust != kInternal) {
        dest.rep(ctx, fill, fillcount);
        fillcount = 0;
    }
    for (const u8 part : pattern) {
        switch (part) {
        case '$':
            if (showbase) dest.put(ctx, symbol);
            break;
        case '+':
            if (!sign.empty()) dest.put(ctx, sign[0]);
            break;
        case 'v': dest.put(ctx, val); break;
        case ' ': dest.rep(ctx, fill, 1); [[fallthrough]];
        case 'x':
            if (adjust == kInternal) {
                dest.rep(ctx, fill, fillcount);
                fillcount = 0;
            }
            break;
        default: break;
        }
    }
    if (1 < sign.size()) dest.put(ctx, sign, 1);
    ios.clear_width();
    dest.rep(ctx, fill, fillcount);
}

// do_put(dest, intl, ios_base&, fill, long double): "%.0Lf" of the magnitude (scaled by 1e10 steps from 1e35 up, the
// zeros appended), widened.
template <u32 E>
void moneyput_ld_(PPUContext& ctx, u32 out, u32, u64 dest, bool intl, u32 ios, u32 fill, double v)
{
    G g(ctx);
    if (!g.need("money_put::do_put", out, 8) || !g.need("money_put::do_put", ios, kIosBaseBytes)) return;
    const Ios is{g, ios};
    const bool neg = v < 0;
    if (neg) v = -v;
    usize exp = 0;
    for (; 1e35 <= v && exp < 5000; exp += 10) v /= 1e10;
    libc::Spec s;
    s.prec = 0;
    std::string text;
    libc::put_double<char>(text, s, 'f', v);
    const u32 ct = use_facet_of(ctx, is.loc(), Of<E>::ctype);
    std::vector<u32> digits = widen_all<E>(ctx, ct, text);
    const u32 e0 = widen<E>(ctx, ct, '0');
    digits.insert(digits.end(), exp, e0);
    OutIt<E> d = OutIt<E>::reg(dest);
    putmfld<E>(ctx, d, intl, is, elem_of<E>(fill), neg, digits);
    d.store(g, out);
}

// do_put(dest, intl, ios_base&, fill, const string_type& digits): a leading '-' makes it negative; the digits up to the
// first other element ("0" when there are none).
template <u32 E>
void moneyput_str_(PPUContext& ctx, u32 out, u32, u64 dest, bool intl, u32 ios, u32 fill, u32 digits_at)
{
    G g(ctx);
    if (!g.need("money_put::do_put", out, 8) || !g.need("money_put::do_put", ios, kIosBaseBytes) ||
        !g.need("money_put::do_put", digits_at, StrLayout<E>::kBytes))
        return;
    const Ios is{g, ios};
    const u32 ct = use_facet_of(ctx, is.loc(), Of<E>::ctype);
    const std::vector<u32> atoms = widen_all<E>(ctx, ct, "0123456789-");
    const std::vector<u32> in = read_str<E>(g, digits_at);
    bool neg = false;
    usize i = 0;
    if (!in.empty() && in[0] == atoms[10]) {
        neg = true;
        ++i;
    }
    std::vector<u32> val;
    for (; i < in.size() && atom_of(atoms, in[i]) < 10; ++i) val.push_back(in[i]);
    if (val.empty()) val.push_back(atoms[0]);
    OutIt<E> d = OutIt<E>::reg(dest);
    putmfld<E>(ctx, d, intl, is, elem_of<E>(fill), neg, val);
    d.store(g, out);
}

// The exported _Putmfld(dest, intl, ios_base&, fill, bool neg, string_type val): the string by reference to the
// caller's copy.
template <u32 E>
void putmfld_(PPUContext& ctx, u32 out, u32, u64 dest, bool intl, u32 ios, u32 fill, bool neg, u32 val_at)
{
    G g(ctx);
    if (!g.need("money_put::_Putmfld", out, 8) || !g.need("money_put::_Putmfld", ios, kIosBaseBytes) ||
        !g.need("money_put::_Putmfld", val_at, StrLayout<E>::kBytes))
        return;
    OutIt<E> d = OutIt<E>::reg(dest);
    putmfld<E>(ctx, d, intl, Ios{g, ios}, elem_of<E>(fill), neg, read_str<E>(g, val_at));
    d.store(g, out);
}

template <u32 E, bool Deleting>
void moneyput_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kMoneyPutC : kMoneyPutW}, Deleting);
}

} // namespace pspocket::hle::stdcxx::facets
