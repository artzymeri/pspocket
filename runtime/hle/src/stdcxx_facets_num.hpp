#pragma once

// sys_libstdcxx's facets (D-561): num_get and num_put, Dinkumware's (xlocnum of the SDK's library, VC8's generation).
// Included by stdcxx_facets.cpp only.
// - num_get gathers a field from the stream (_Getifld, _Getffld: sign, digits with the locale's grouping checked,
//   leading zeros dropped, at most _MAX_INT_DIG or _MAX_SIG_DIG significant digits, the rest counted as a power of
//   ten), then converts it with the C library's _Stolx, _Stoulx, _Stollx, _Stoullx, _Stofx, _Stodx or _Stoldx
//   (sys_libc, through the registry, as the SDK's calls its C library); a field the conversion refuses or finds out of
//   range sets failbit and leaves the value alone, the end of the input sets eofbit.
// - num_put formats with the C library's printf engine (libc_format.hpp, the same text as sys_libc's sprintf) under
//   the format _Ifmt or _Ffmt makes of the flags, then _Iput or _Fput widen it through the stream's ctype, put the
//   numpunct's decimal point and thousands separators per its grouping, pad to the width with the fill (left, right or
//   internal) and set the width to 0. A float's precision is capped at _MAX_SIG_DIG (36) significant digits, the rest
//   put as trailing zeros, and a fixed value of 1e35 or more (or 1e-35 or less) is scaled by 1e10 steps, the zeros put
//   before (after) the point, as Dinkumware's do_put does.

#include "stdcxx_facets_ctype.hpp"

namespace pspocket::hle::stdcxx::facets {

// numpunct's virtual functions: [~D1, ~D0, do_decimal_point, do_thousands_sep, do_grouping, do_falsename,
// do_truename]; ctype's do_widen(char) (6 for char, 10 for wchar_t) and its range form (7, 11).
inline constexpr u32 kNpDp = 2, kNpSep = 3, kNpGrouping = 4, kNpFalse = 5, kNpTrue = 6;

template <u32 E>
u32 elem_of(u32 v)
{
    return E == 1 ? (v & 0xFF) : v;
}

template <u32 E>
u32 widen(PPUContext& ctx, u32 ct, u8 c)
{
    return elem_of<E>(vint(ctx, ct, E == 1 ? 6 : 10, {ct, c}, c));
}

// ctype<E>::widen(first, last, dest) of a host string through the facet (the range form).
template <u32 E>
std::vector<u32> widen_all(PPUContext& ctx, u32 ct, std::string_view s)
{
    std::vector<u32> out;
    if (s.empty()) return out;
    G g(ctx);
    StackVar src(ctx, g.b, static_cast<u32>(s.size()) + 1);
    StackVar dst(ctx, g.b, E * static_cast<u32>(s.size()) + 4);
    std::memcpy(g.b + src.addr(), s.data(), s.size());
    vcall(ctx, ct, E == 1 ? 7 : 11, {ct, src.addr(), src.addr() + s.size(), dst.addr()});
    for (u32 i = 0; i < s.size(); i++) out.push_back(el<E>(g, dst.addr() + E * i));
    return out;
}

// A grouping string's i-th size (0 past its end, as the string's NUL).
inline u32 group_at(const std::vector<u32>& grouping, usize i) { return i < grouping.size() ? grouping[i] & 0xFF : 0; }

// The C library's number conversions, through the registry: the field written to the guest stack, its end and errno
// read back. `fail`: no conversion (the end at the start) or a range error.
namespace cfn {
inline const Callee stolx{"sys_libc", "_Stolx"}, stoulx{"sys_libc", "_Stoulx"}, stollx{"sys_libc", "_Stollx"},
    stoullx{"sys_libc", "_Stoullx"}, stofx{"sys_libc", "_Stofx"}, stodx{"sys_libc", "_Stodx"},
    stoldx{"sys_libc", "_Stoldx"};
} // namespace cfn

struct Converted {
    u64 bits = 0;   // an integer's value, or a double's bits (a float's as a double)
    bool fail = true;
    bool neg_field = false; // the field began with '-'
};

inline Converted convert(PPUContext& ctx, const Callee& f, const std::string& field, s32 base_or_pten, bool floating,
    usize skip = 0)
{
    G g(ctx);
    StackVar buf(ctx, g.b, 128);
    const u32 text = buf.addr(), endp = buf.addr() + 112, perr = buf.addr() + 116;
    std::memcpy(g.b + text, field.data(), std::min<usize>(field.size(), 110));
    g.w8(text + static_cast<u32>(std::min<usize>(field.size(), 110)), 0);
    g.w32(endp, 0);
    g.w32(perr, 0);
    const u32 start = text + static_cast<u32>(skip);
    Converted c;
    const u64 r = f(ctx, {start, endp, static_cast<u64>(static_cast<s64>(base_or_pten)), perr});
    c.bits = floating ? std::bit_cast<u64>(ctx.fpr[1]) : r;
    c.fail = g.r32(endp) == start || g.r32(perr) != 0;
    c.neg_field = !field.empty() && field[0] == '-';
    return c;
}

// ------------------------------------------------------------------------------------------------- num_get

// _Getifld: the integer field from [first, last) into `ac`, its base answered (`basefield` the stream's flags).
template <u32 E>
s32 getifld(PPUContext& ctx, std::string& ac, InIt<E>& first, InIt<E>& last, u32 basefield, u32 loc)
{
    const u32 np = use_facet_of(ctx, loc, Of<E>::numpunct);
    const std::vector<u32> grouping = vstring<1>(ctx, np, kNpGrouping);
    const u32 ksep = grouping.empty() ? 0 : elem_of<E>(vint(ctx, np, kNpSep, {np}));
    ac.clear();
    if (at_end(ctx, first, last)) {
    } else if (first(ctx) == '+') {
        ac += '+';
        first.inc(ctx);
    } else if (first(ctx) == '-') {
        ac += '-';
        first.inc(ctx);
    }
    basefield &= kBasefield;
    s32 base = basefield == kOct ? 8 : basefield == kHex ? 16 : basefield == 0 ? 0 : 10;
    bool seendigit = false, nonzero = false;
    if (!at_end(ctx, first, last) && first(ctx) == '0') {
        seendigit = true;
        first.inc(ctx);
        if (!at_end(ctx, first, last) && (first(ctx) == 'x' || first(ctx) == 'X') && (base == 0 || base == 16)) {
            base = 16;
            seendigit = false;
            first.inc(ctx);
        } else if (base == 0) {
            base = 8;
        }
    }
    const usize dlen = base == 0 || base == 10 ? 10 : base == 8 ? 8 : 22;
    std::vector<u32> groups(1, seendigit ? 1 : 0);
    usize group = 0;
    for (; !at_end(ctx, first, last); first.inc(ctx)) {
        const u32 c = first(ctx);
        const u8 byte = c_narrow(c).value_or(0);
        if (byte != 0 && std::memchr("0123456789abcdefABCDEF", byte, dlen)) {
            if ((nonzero || byte != '0') && ac.size() < kMaxIntDig - 1) {
                ac += static_cast<char>(byte);
                nonzero = true;
            }
            seendigit = true;
            if (groups[group] != kCharMax) ++groups[group];
        } else if (groups[group] == 0 || ksep == 0 || c != ksep) {
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
        seendigit = false; // a trailing separator
    }
    for (usize pg = 0; seendigit && 0 < group;) {
        const u32 g0 = group_at(grouping, pg);
        if (g0 == kCharMax) break;
        if ((0 < --group && g0 != groups[group]) || (0 == group && g0 < groups[group])) seendigit = false;
        else if (0 < group_at(grouping, pg + 1)) ++pg;
    }
    if (seendigit && !nonzero) ac += '0';
    else if (!seendigit) ac.clear();
    return base;
}

// _Getffld: the floating field into `ac` (at most _MAX_SIG_DIG significant digits, the C locale's '.', an exponent of
// at most _MAX_EXP_DIG digits); answers the power of ten the field is to be scaled by.
template <u32 E>
s32 getffld(PPUContext& ctx, std::string& ac, InIt<E>& first, InIt<E>& last, u32 loc)
{
    const u32 np = use_facet_of(ctx, loc, Of<E>::numpunct);
    const std::vector<u32> grouping = vstring<1>(ctx, np, kNpGrouping);
    const auto digit = [](u32 c) { return c >= '0' && c <= '9'; };
    bool bad = false;
    ac.clear();
    if (at_end(ctx, first, last)) {
    } else if (first(ctx) == '+') {
        ac += '+';
        first.inc(ctx);
    } else if (first(ctx) == '-') {
        ac += '-';
        first.inc(ctx);
    }
    bool seendigit = false;
    u32 significant = 0;
    s32 pten = 0;
    const auto take = [&](u32 c) {
        if (kMaxSigDig <= significant) ++pten;
        else if (c == '0' && significant == 0) {
        } else {
            ac += static_cast<char>(c);
            ++significant;
        }
    };
    if (group_at(grouping, 0) == kCharMax || group_at(grouping, 0) == 0) {
        for (; !at_end(ctx, first, last) && digit(first(ctx)); seendigit = true, first.inc(ctx)) take(first(ctx));
    } else {
        const u32 ksep = grouping.empty() ? 0 : elem_of<E>(vint(ctx, np, kNpSep, {np}));
        std::vector<u32> groups(1, 0);
        usize group = 0;
        for (; !at_end(ctx, first, last); first.inc(ctx)) {
            const u32 c = first(ctx);
            if (digit(c)) {
                seendigit = true;
                take(c);
                if (groups[group] != kCharMax) ++groups[group];
            } else if (groups[group] == 0 || ksep == 0 || c != ksep) {
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
    if (seendigit && significant == 0) ac += '0';
    if (!at_end(ctx, first, last) && first(ctx) == elem_of<E>(vint(ctx, np, kNpDp, {np}))) {
        ac += '.';
        first.inc(ctx);
    }
    if (significant == 0) {
        for (; !at_end(ctx, first, last) && first(ctx) == '0'; seendigit = true, first.inc(ctx)) --pten;
        if (pten < 0) {
            ac += '0';
            ++pten;
        }
    }
    for (; !at_end(ctx, first, last) && digit(first(ctx)); seendigit = true, first.inc(ctx))
        if (significant < kMaxSigDig) {
            ac += static_cast<char>(first(ctx));
            ++significant;
        }
    if (seendigit && !at_end(ctx, first, last) && (first(ctx) == 'e' || first(ctx) == 'E')) {
        ac += 'e';
        first.inc(ctx);
        seendigit = false;
        significant = 0;
        if (at_end(ctx, first, last)) {
        } else if (first(ctx) == '+') {
            ac += '+';
            first.inc(ctx);
        } else if (first(ctx) == '-') {
            ac += '-';
            first.inc(ctx);
        }
        for (; !at_end(ctx, first, last) && first(ctx) == '0';) {
            seendigit = true;
            first.inc(ctx);
        }
        if (seendigit) ac += '0';
        for (; !at_end(ctx, first, last) && digit(first(ctx)); seendigit = true, first.inc(ctx))
            if (significant < kMaxExpDig) {
                ac += static_cast<char>(first(ctx));
                ++significant;
            }
    }
    if (bad || !seendigit) ac.clear();
    return pten;
}

// What do_get delivers: the iostate bits it adds; the value stored only without failbit.
enum class NumKind : u8 { Bool, UShort, UInt, Long, ULong, LLong, ULLong, Float, Double, LDouble, Ptr };

template <u32 E>
void numget_store_state(const G& g, u32 state, u32 bits)
{
    g.w32(state, g.r32(state) | bits);
}

template <u32 E, NumKind K>
InIt<E> numget(PPUContext& ctx, u32 self, InIt<E> first, InIt<E> last, u32 ios_at, u32 state, u32 val)
{
    G g(ctx);
    const Ios ios{g, ios_at};
    std::string ac;
    u32 bits = 0;
    bool fail = true;
    u64 value = 0;
    if constexpr (K == NumKind::Bool) {
        s32 ans = -1;
        if (ios.flags() & kBoolalpha) {
            const u32 np = use_facet_of(ctx, ios.loc(), Of<E>::numpunct);
            std::vector<u32> names(1, 0);
            for (const u32 c : vstring<E>(ctx, np, kNpFalse)) names.push_back(c);
            names.push_back(0);
            for (const u32 c : vstring<E>(ctx, np, kNpTrue)) names.push_back(c);
            names.push_back(0);
            ans = getloctxt<E>(ctx, first, last, 2, [&](usize i) -> u32 { return i < names.size() ? names[i] : 0; });
        } else {
            const s32 base = getifld<E>(ctx, ac, first, last, ios.flags(), ios.loc());
            const Converted c = convert(ctx, cfn::stoulx, ac, base, false);
            if (!c.fail && static_cast<u32>(c.bits) <= 1) ans = static_cast<s32>(c.bits);
        }
        fail = ans < 0;
        value = ans > 0 ? 1 : 0;
    } else if constexpr (K == NumKind::UShort || K == NumKind::UInt) {
        const s32 base = getifld<E>(ctx, ac, first, last, ios.flags(), ios.loc());
        const bool neg = !ac.empty() && ac[0] == '-';
        const Converted c = convert(ctx, cfn::stoulx, ac, base, false, neg ? 1 : 0);
        const u32 ans = static_cast<u32>(c.bits);
        fail = c.fail || (K == NumKind::UShort && ans > 0xFFFF);
        value = neg ? 0u - ans : ans;
    } else if constexpr (K == NumKind::Long || K == NumKind::ULong || K == NumKind::LLong || K == NumKind::ULLong) {
        const s32 base = getifld<E>(ctx, ac, first, last, ios.flags(), ios.loc());
        const Callee& f = K == NumKind::Long ? cfn::stolx : K == NumKind::ULong ? cfn::stoulx
            : K == NumKind::LLong                           ? cfn::stollx
                                                            : cfn::stoullx;
        const Converted c = convert(ctx, f, ac, base, false);
        fail = c.fail;
        value = c.bits;
    } else if constexpr (K == NumKind::Ptr) {
        const s32 base = getifld<E>(ctx, ac, first, last, kHex, ios.loc());
        const Converted c = convert(ctx, cfn::stoulx, ac, base, false);
        fail = c.fail;
        value = static_cast<u32>(c.bits);
    } else {
        const s32 pten = getffld<E>(ctx, ac, first, last, ios.loc());
        const Callee& f = K == NumKind::Float ? cfn::stofx : K == NumKind::Double ? cfn::stodx : cfn::stoldx;
        const Converted c = convert(ctx, f, ac, pten, true);
        fail = c.fail;
        value = c.bits;
    }
    if (at_end(ctx, first, last)) bits |= kEofbit;
    if (fail) bits |= kFailbit;
    numget_store_state<E>(g, state, bits);
    if (!fail && g.need("num_get::do_get", val, 1)) {
        if constexpr (K == NumKind::Bool) g.w8(val, static_cast<u8>(value));
        else if constexpr (K == NumKind::UShort) g.w16(val, static_cast<u16>(value));
        else if constexpr (K == NumKind::LLong || K == NumKind::ULLong || K == NumKind::Double || K == NumKind::LDouble)
            g.w64(val, value);
        else if constexpr (K == NumKind::Float) g.w32(val, std::bit_cast<u32>(static_cast<float>(std::bit_cast<double>(value))));
        else g.w32(val, static_cast<u32>(value));
    }
    (void)self;
    return first;
}

// do_get for char: (result, this, first, last, ios_base&, iostate&, T&); for wchar_t the iterators are two
// doublewords each, the value's reference the ninth argument.
template <NumKind K>
void numget_c_(PPUContext& ctx, u32 out, u32 self, u64 first, u64 last, u32 ios, u32 state, u32 val)
{
    G g(ctx);
    if (!g.need("num_get::do_get", out, 8) || !g.need("num_get::do_get", ios, kIosBaseBytes) ||
        !g.need("num_get::do_get", state, 4))
        return;
    numget<1, K>(ctx, self, InIt<1>::regs(first), InIt<1>::regs(last), ios, state, val).store(g, out);
}

template <NumKind K>
void numget_w_(PPUContext& ctx, u32 out, u32 self, u64 f0, u64 f1, u64 l0, u64 l1, u32 ios, u32 state, u32 val)
{
    G g(ctx);
    if (!g.need("num_get::do_get", out, 12) || !g.need("num_get::do_get", ios, kIosBaseBytes) ||
        !g.need("num_get::do_get", state, 4))
        return;
    numget<4, K>(ctx, self, InIt<4>::regs(f0, f1), InIt<4>::regs(l0, l1), ios, state, val).store(g, out);
}

// The exported _Getifld(char *, _InIt&, _InIt&, fmtflags, const locale&) and _Getffld(char *, _InIt&, _InIt&,
// const locale&): the field written to `ac` with its NUL, the iterators moved.
template <u32 E>
s32 getifld_(PPUContext& ctx, u32, u32 ac, u32 first_at, u32 last_at, u32 basefield, u32 loc)
{
    G g(ctx);
    if (!g.need("num_get::_Getifld", ac, kMaxIntDig) || !g.need("num_get::_Getifld", first_at, InIt<E>::kBytes) ||
        !g.need("num_get::_Getifld", last_at, InIt<E>::kBytes))
        return 10;
    InIt<E> first = InIt<E>::load(g, first_at), last = InIt<E>::load(g, last_at);
    std::string field;
    const s32 base = getifld<E>(ctx, field, first, last, basefield, loc);
    first.store(g, first_at);
    last.store(g, last_at);
    std::memcpy(g.b + ac, field.data(), field.size());
    g.w8(ac + static_cast<u32>(field.size()), 0);
    return base;
}

template <u32 E>
s32 getffld_(PPUContext& ctx, u32, u32 ac, u32 first_at, u32 last_at, u32 loc)
{
    G g(ctx);
    if (!g.need("num_get::_Getffld", ac, kMaxExpDig + kMaxSigDig + 16) ||
        !g.need("num_get::_Getffld", first_at, InIt<E>::kBytes) || !g.need("num_get::_Getffld", last_at, InIt<E>::kBytes))
        return 0;
    InIt<E> first = InIt<E>::load(g, first_at), last = InIt<E>::load(g, last_at);
    std::string field;
    const s32 pten = getffld<E>(ctx, field, first, last, loc);
    first.store(g, first_at);
    last.store(g, last_at);
    std::memcpy(g.b + ac, field.data(), field.size());
    g.w8(ac + static_cast<u32>(field.size()), 0);
    return pten;
}

template <u32 E, bool Deleting>
void numget_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kNumGetC : kNumGetW}, Deleting);
}

// ------------------------------------------------------------------------------------------------- num_put

// _Putgrouped: the characters (widened), a thousands separator for each NUL (none when the separator is 0).
template <u32 E>
void put_grouped(PPUContext& ctx, OutIt<E>& dest, u32 ct, std::string_view s, u32 ksep)
{
    for (const char c : s) {
        if (c == '\0') {
            if (ksep != 0) dest.put(ctx, ksep);
        } else {
            dest.put(ctx, widen<E>(ctx, ct, static_cast<u8>(c)));
        }
    }
}

// _Iput: an integer's text put with the locale's grouping and the width.
template <u32 E>
void iput(PPUContext& ctx, OutIt<E>& dest, const Ios& ios, u32 fill, std::string buf)
{
    const usize prefix = !buf.empty() && (buf[0] == '+' || buf[0] == '-') ? 1
        : buf.size() >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X') ? 2
                                                                                 : 0;
    const u32 ct = use_facet_of(ctx, ios.loc(), Of<E>::ctype);
    std::vector<u32> group = widen_all<E>(ctx, ct, buf);
    const u32 np = use_facet_of(ctx, ios.loc(), Of<E>::numpunct);
    const std::vector<u32> grouping = vstring<1>(ctx, np, kNpGrouping);
    if (group_at(grouping, 0) != kCharMax && 0 < group_at(grouping, 0)) {
        const u32 ksep = elem_of<E>(vint(ctx, np, kNpSep, {np}));
        usize count = buf.size();
        for (usize pg = 0; group_at(grouping, pg) != kCharMax && 0 < group_at(grouping, pg) &&
             group_at(grouping, pg) < count - prefix;) {
            count -= group_at(grouping, pg);
            group.insert(group.begin() + static_cast<std::ptrdiff_t>(count), ksep);
            if (0 < group_at(grouping, pg + 1)) ++pg;
        }
    }
    const usize count = group.size();
    usize fillcount = ios.width() <= 0 || static_cast<usize>(ios.width()) <= count ? 0 : static_cast<usize>(ios.width()) - count;
    const u32 adjust = ios.flags() & kAdjustfield;
    usize start = 0;
    if (adjust != kLeft && adjust != kInternal) {
        dest.rep(ctx, fill, fillcount);
        fillcount = 0;
    } else if (adjust == kInternal) {
        dest.put(ctx, group, 0, prefix);
        start = prefix;
        dest.rep(ctx, fill, fillcount);
        fillcount = 0;
    }
    dest.put(ctx, group, start);
    ios.clear_width();
    dest.rep(ctx, fill, fillcount);
}

// _Fput: a float's text (`buf`, from printf) put with the locale's decimal point and grouping, `before` zeros before
// its point, `after` after it, `trailing` at the end of its digits, padded to the width.
template <u32 E>
void fput(PPUContext& ctx, OutIt<E>& dest, const Ios& ios, u32 fill, std::string buf, usize before, usize after,
    usize trailing)
{
    G g(ctx);
    const u32 np = use_facet_of(ctx, ios.loc(), Of<E>::numpunct);
    const u32 ct = use_facet_of(ctx, ios.loc(), Of<E>::ctype);
    const std::vector<u32> grouping = vstring<1>(ctx, np, kNpGrouping);
    const u32 ksep = elem_of<E>(vint(ctx, np, kNpSep, {np}));
    const u32 e0 = widen<E>(ctx, ct, '0');
    const usize prefix = !buf.empty() && (buf[0] == '+' || buf[0] == '-') ? 1 : 0;
    const std::string cdp = lconv_str(g, lconv_of(ctx), kLcDp);
    const char point = cdp.empty() ? '.' : cdp[0];
    const usize eptr = buf.find('e');
    usize pointptr = buf.find(point);
    if (pointptr == std::string::npos) trailing = 0;
    if (group_at(grouping, 0) != kCharMax && 0 < group_at(grouping, 0)) {
        std::string gs = buf;
        if (eptr == std::string::npos) {
            gs.append(trailing, '0');
        } else {
            if (pointptr == std::string::npos) {
                gs.append(before, '0');
                before = 0;
            }
            gs.insert(eptr, trailing, '0');
        }
        trailing = 0;
        if (pointptr == std::string::npos) {
            gs.append(before, '0');
        } else {
            gs.insert(pointptr + 1, after, '0');
            gs.insert(pointptr, before, '0');
            after = 0;
        }
        before = 0;
        usize off = gs.find_first_of(std::string{point, 'e'});
        if (off == std::string::npos) off = gs.size();
        for (usize pg = 0; group_at(grouping, pg) != kCharMax && 0 < group_at(grouping, pg) &&
             group_at(grouping, pg) < off - prefix;) {
            off -= group_at(grouping, pg);
            gs.insert(off, 1, '\0');
            if (0 < group_at(grouping, pg + 1)) ++pg;
        }
        buf = gs;
    }
    usize fillcount = before + after + trailing + buf.size();
    fillcount = ios.width() <= 0 || static_cast<usize>(ios.width()) <= fillcount ? 0 : static_cast<usize>(ios.width()) - fillcount;
    const u32 adjust = ios.flags() & kAdjustfield;
    std::string_view rest = buf;
    if (adjust != kLeft && adjust != kInternal) {
        dest.rep(ctx, fill, fillcount);
        fillcount = 0;
    } else if (adjust == kInternal) {
        if (0 < prefix) {
            dest.put(ctx, widen<E>(ctx, ct, static_cast<u8>(rest[0])));
            rest.remove_prefix(1);
        }
        dest.rep(ctx, fill, fillcount);
        fillcount = 0;
    }
    pointptr = rest.find(point);
    if (pointptr != std::string_view::npos) {
        put_grouped<E>(ctx, dest, ct, rest.substr(0, pointptr), ksep);
        dest.rep(ctx, e0, before);
        dest.put(ctx, elem_of<E>(vint(ctx, np, kNpDp, {np})));
        dest.rep(ctx, e0, after);
        rest.remove_prefix(pointptr + 1);
    }
    const usize e = rest.find('e');
    if (e != std::string_view::npos) {
        put_grouped<E>(ctx, dest, ct, rest.substr(0, e), ksep);
        dest.rep(ctx, e0, trailing);
        trailing = 0;
        dest.put(ctx, widen<E>(ctx, ct, (ios.flags() & kUppercase) ? 'E' : 'e'));
        rest.remove_prefix(e + 1);
    }
    put_grouped<E>(ctx, dest, ct, rest, ksep);
    dest.rep(ctx, e0, trailing);
    ios.clear_width();
    dest.rep(ctx, fill, fillcount);
}

// The text printf gives an integer under _Ifmt's format: %[+][#]{l,L}{d,u,o,x,X}.
inline std::string int_text(u32 flags, u64 raw, bool is_signed, bool wide64)
{
    libc::Spec s;
    s.plus = (flags & kShowpos) != 0;
    s.alt = (flags & kShowbase) != 0;
    s.len = wide64 ? libc::Len::ll : libc::Len::l;
    const u32 basefield = flags & kBasefield;
    const char conv = basefield == kOct ? 'o' : basefield != kHex ? (is_signed ? 'd' : 'u')
        : (flags & kUppercase)                                     ? 'X'
                                                                   : 'x';
    std::string out;
    libc::put_integer<char>(out, s, conv, raw);
    return out;
}

// The text printf gives a double under _Ffmt's format: %[+][#].*{f,e,E,g,G} with `prec` significant digits.
inline std::string float_text(u32 flags, s32 prec, double v)
{
    libc::Spec s;
    s.plus = (flags & kShowpos) != 0;
    s.alt = (flags & kShowpoint) != 0;
    s.prec = prec < 0 ? -1 : prec;
    const u32 ff = flags & kFloatfield;
    const bool upper = (flags & kUppercase) != 0;
    const char conv = ff == kFixed ? 'f' : ff == kScientific ? (upper ? 'E' : 'e') : (upper ? 'G' : 'g');
    std::string out;
    libc::put_double<char>(out, s, conv, v);
    return out;
}

enum class PutKind : u8 { Bool, Long, ULong, LLong, ULLong, Double, LDouble, Ptr };

// numpunct<E>'s name for a bool.
template <u32 E>
void put_bool_name(PPUContext& ctx, OutIt<E>& dest, const Ios& ios, u32 fill, bool v)
{
    const u32 np = use_facet_of(ctx, ios.loc(), Of<E>::numpunct);
    const std::vector<u32> name = vstring<E>(ctx, np, v ? kNpTrue : kNpFalse);
    usize fillcount = ios.width() <= 0 || static_cast<usize>(ios.width()) <= name.size() ? 0 : static_cast<usize>(ios.width()) - name.size();
    if ((ios.flags() & kAdjustfield) != kLeft) {
        dest.rep(ctx, fill, fillcount);
        fillcount = 0;
    }
    dest.put(ctx, name);
    ios.clear_width();
    dest.rep(ctx, fill, fillcount);
}

template <u32 E, PutKind K>
OutIt<E> numput(PPUContext& ctx, u32 self, OutIt<E> dest, u32 ios_at, u32 fill, u64 raw, double fv)
{
    G g(ctx);
    const Ios ios{g, ios_at};
    fill = elem_of<E>(fill);
    if constexpr (K == PutKind::Bool) {
        if (!(ios.flags() & kBoolalpha)) {
            // do_put(_Dest, _Iosbase, _Fill, (long)_Val): the long overload, through the vtable.
            StackVar ret(ctx, g.b, 16);
            u64 d = (dest.failed ? (u64{1} << 56) : 0) | dest.sb;
            vcall(ctx, self, 3, {ret.addr(), self, d, ios_at, fill, raw & 0xFF ? 1u : 0u});
            return OutIt<E>::load(g, ret.addr());
        }
        put_bool_name<E>(ctx, dest, ios, fill, (raw & 0xFF) != 0);
    } else if constexpr (K == PutKind::Long || K == PutKind::ULong || K == PutKind::LLong || K == PutKind::ULLong) {
        const bool wide64 = K == PutKind::LLong || K == PutKind::ULLong;
        const bool is_signed = K == PutKind::Long || K == PutKind::LLong;
        iput<E>(ctx, dest, ios, fill, int_text(ios.flags(), wide64 ? raw : static_cast<u32>(raw), is_signed, wide64));
    } else if constexpr (K == PutKind::Ptr) {
        libc::Spec s;
        std::string text;
        libc::put_integer<char>(text, s, 'p', static_cast<u32>(raw));
        iput<E>(ctx, dest, ios, fill, text);
    } else {
        s32 precision = ios.precision() <= 0 && !(ios.flags() & kFixed) ? 6 : ios.precision();
        const s32 significance = static_cast<s32>(kMaxSigDig) < precision ? static_cast<s32>(kMaxSigDig) : precision;
        precision -= significance;
        usize before = 0, after = 0;
        double v = fv;
        if ((ios.flags() & kFloatfield) == kFixed) {
            const bool neg = v < 0;
            if (neg) v = -v;
            for (; 1e35 <= v && before < 5000; before += 10) v /= 1e10;
            if (0 < v)
                for (; 10 <= precision && v <= 1e-35 && after < 5000; after += 10) {
                    v *= 1e10;
                    precision -= 10;
                }
            if (neg) v = -v;
        }
        fput<E>(ctx, dest, ios, fill, float_text(ios.flags(), significance, v), before, after,
            precision < 0 ? 0 : static_cast<usize>(precision));
    }
    return dest;
}

// do_put: (result, this, dest, ios_base&, fill, value); a double's value in f1.
template <u32 E, PutKind K>
void numput_(PPUContext& ctx, u32 out, u32 self, u64 dest, u32 ios, u32 fill, u64 raw)
{
    G g(ctx);
    if (!g.need("num_put::do_put", out, 8) || !g.need("num_put::do_put", ios, kIosBaseBytes)) return;
    numput<E, K>(ctx, self, OutIt<E>::reg(dest), ios, fill, raw, 0).store(g, out);
}

template <u32 E, PutKind K>
void numput_f_(PPUContext& ctx, u32 out, u32 self, u64 dest, u32 ios, u32 fill, double v)
{
    G g(ctx);
    if (!g.need("num_put::do_put", out, 8) || !g.need("num_put::do_put", ios, kIosBaseBytes)) return;
    numput<E, K>(ctx, self, OutIt<E>::reg(dest), ios, fill, 0, v).store(g, out);
}

// The exported _Iput(dest, ios_base&, fill, char *buf, size_t count) and _Fput(dest, ios_base&, fill, const char
// *buf, size_t before, size_t after, size_t trailing, size_t count).
template <u32 E>
void iput_(PPUContext& ctx, u32 out, u32, u64 dest, u32 ios, u32 fill, u32 buf, u32 count)
{
    G g(ctx);
    if (!g.need("num_put::_Iput", out, 8) || !g.need("num_put::_Iput", ios, kIosBaseBytes) ||
        (count && !g.need("num_put::_Iput", buf, count)))
        return;
    OutIt<E> d = OutIt<E>::reg(dest);
    iput<E>(ctx, d, Ios{g, ios}, elem_of<E>(fill), std::string(reinterpret_cast<const char*>(g.b + buf), count));
    d.store(g, out);
}

template <u32 E>
void fput_(PPUContext& ctx, u32 out, u32, u64 dest, u32 ios, u32 fill, u32 buf, u32 before, u32 after, u32 trailing,
    u32 count)
{
    G g(ctx);
    if (!g.need("num_put::_Fput", out, 8) || !g.need("num_put::_Fput", ios, kIosBaseBytes) ||
        (count && !g.need("num_put::_Fput", buf, count)))
        return;
    OutIt<E> d = OutIt<E>::reg(dest);
    fput<E>(ctx, d, Ios{g, ios}, elem_of<E>(fill), std::string(reinterpret_cast<const char*>(g.b + buf), count), before,
        after, trailing);
    d.store(g, out);
}

template <u32 E, bool Deleting>
void numput_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kNumPutC : kNumPutW}, Deleting);
}

} // namespace pspocket::hle::stdcxx::facets
