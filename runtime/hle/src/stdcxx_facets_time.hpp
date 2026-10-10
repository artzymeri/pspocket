#pragma once

// sys_libstdcxx's facets (D-561): time_get and time_put, Dinkumware's (xloctime of the SDK's library, VC8's
// generation). Included by stdcxx_facets.cpp only.
// - time_get keeps the locale's day and month names (":Sun:Sunday:...", ":Jan:January:...") and its date order (mdy
//   for the "C" locale's "%m/%d/%y"). Weekdays and months are matched with _Getloctxt (the longest name wins); numbers
//   with _Getint (an optional sign, digits, the range checked); a time is hh:mm:ss, a date three fields in the date
//   order separated by optional spaces and one of ':' ',' '/'.
// - time_put formats one conversion with the C library's strftime (wcsftime for wchar_t) behind a '!' (so an empty
//   conversion still counts), in a buffer doubled until it fits, and puts what follows the '!'.

#include "stdcxx_facets_money.hpp"

namespace pspocket::hle::stdcxx::facets {

// time_get's virtual functions after the destructors.
inline constexpr u32 kTgDateOrder = 2, kTgTime = 3, kTgDate = 4, kTgWeekday = 5, kTgMonthname = 6, kTgYear = 7;

// _Init(const _Locinfo&): the day and month names copied (new[]), the date order.
template <u32 E>
void timeget_init(PPUContext& ctx, u32 self, u32 lobj)
{
    G g(ctx);
    g.w32(self + kTgDays, 0);
    g.w32(self + kTgMonths, 0);
    g.w32(self + kTgDays, maklocstr<E>(ctx, info_text(ctx, lobj, kInfoDays, kCDays)));
    g.w32(self + kTgMonths, maklocstr<E>(ctx, info_text(ctx, lobj, kInfoMonths, kCMonths)));
    g.w32(self + kTgOrder, static_cast<u32>(date_order(ctx, lobj)));
}

template <u32 E>
void timeget_init_(PPUContext& ctx, u32 self, u32 lobj)
{
    G g(ctx);
    if (g.need("time_get::_Init", self, kTimeGetBytes) && g.need("time_get::_Init", lobj, 144))
        timeget_init<E>(ctx, self, lobj);
}

template <u32 E>
u32 make_timeget(PPUContext& ctx)
{
    Locinfo lobj(ctx);
    const u32 p = new_facet(ctx, E == 1 ? kTimeGetC : kTimeGetW, kTimeGetBytes);
    timeget_init<E>(ctx, p, lobj.addr());
    return p;
}

template <u32 E>
void timeget_tidy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    op_delete(ctx, g.r32(self + kTgDays));
    op_delete(ctx, g.r32(self + kTgMonths));
}

template <u32 E, bool Deleting>
void timeget_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kTimeGetC : kTimeGetW, kTimeBase}, Deleting, &timeget_tidy<E>);
}

s32 timeget_order_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    return g.need("time_get::do_date_order", self, kTimeGetBytes) ? static_cast<s32>(g.r32(self + kTgOrder)) : kNoOrder;
}

// _Getint(first, last, lo, hi, val): an integer in [lo, hi] (a sign, leading zeros as one, at most _MAX_INT_DIG - 1
// characters converted by _Stolx); the iostate bits (eofbit at the end, failbit for no number or one out of range).
template <u32 E>
u32 getint(PPUContext& ctx, InIt<E>& first, InIt<E>& last, s32 lo, s32 hi, s32& val)
{
    std::string ac;
    if (at_end(ctx, first, last)) {
    } else if (first(ctx) == '+') {
        ac += '+';
        first.inc(ctx);
    } else if (first(ctx) == '-') {
        ac += '-';
        first.inc(ctx);
    }
    bool seendigit = false;
    for (; !at_end(ctx, first, last) && first(ctx) == '0'; seendigit = true, first.inc(ctx)) {
    }
    if (seendigit) ac += '0';
    for (; !at_end(ctx, first, last) && first(ctx) >= '0' && first(ctx) <= '9'; seendigit = true, first.inc(ctx))
        if (ac.size() < kMaxIntDig - 1) ac += static_cast<char>(first(ctx));
    if (!seendigit) ac.clear();
    const Converted c = convert(ctx, cfn::stolx, ac, 10, false);
    const s32 ans = static_cast<s32>(static_cast<u32>(c.bits));
    u32 state = kGoodbit;
    if (at_end(ctx, first, last)) state |= kEofbit;
    if (c.fail || ans < lo || hi < ans) state |= kFailbit;
    else val = ans;
    return state;
}

template <u32 E>
u32 getint_(PPUContext& ctx, u32, u32 first_at, u32 last_at, s32 lo, s32 hi, u32 val_at)
{
    G g(ctx);
    if (!g.need("time_get::_Getint", first_at, InIt<E>::kBytes) || !g.need("time_get::_Getint", last_at, InIt<E>::kBytes) ||
        !g.need("time_get::_Getint", val_at, 4))
        return kFailbit;
    InIt<E> first = InIt<E>::load(g, first_at), last = InIt<E>::load(g, last_at);
    s32 val = static_cast<s32>(g.r32(val_at));
    const u32 state = getint<E>(ctx, first, last, lo, hi, val);
    first.store(g, first_at);
    last.store(g, last_at);
    g.w32(val_at, static_cast<u32>(val));
    return state;
}

// A tm field read and written as the parse goes.
struct TmField {
    G& g;
    u32 at;
    s32 get() const { return static_cast<s32>(g.r32(at)); }
    void set(s32 v) const { g.w32(at, static_cast<u32>(v)); }
};

template <u32 E>
u32 getint_field(PPUContext& ctx, InIt<E>& first, InIt<E>& last, s32 lo, s32 hi, const TmField& f)
{
    s32 v = f.get();
    const u32 st = getint<E>(ctx, first, last, lo, hi, v);
    f.set(v);
    return st;
}

// A call of one of the facet's own get_* through its vtable (a derived class's override is the one called): first is
// moved to what it answers.
template <u32 E>
void vget(PPUContext& ctx, u32 self, u32 slot, InIt<E>& first, InIt<E>& last, u32 ios, u32 state, u32 tm)
{
    G g(ctx);
    StackVar ret(ctx, g.b, 16);
    StackVar tmp(ctx, g.b, 32);
    first.store(g, tmp.addr());
    last.store(g, tmp.addr() + 16);
    const u64 f0 = g.r64(tmp.addr()), l0 = g.r64(tmp.addr() + 16);
    if constexpr (E == 1) {
        vcall(ctx, self, slot, {ret.addr(), self, f0, l0, ios, state, tm});
    } else {
        const u64 f1 = g.r64(tmp.addr() + 8), l1 = g.r64(tmp.addr() + 24);
        vcall(ctx, self, slot, {ret.addr(), self, f0, f1, l0, l1, ios, state, tm});
    }
    first = InIt<E>::load(g, ret.addr());
}

template <u32 E>
bool is_digit(PPUContext& ctx, InIt<E>& it)
{
    const u32 c = it(ctx);
    return c >= '0' && c <= '9';
}

// The separator between a date's fields: spaces, one of ':' ',' '/', spaces.
template <u32 E>
void skip_sep(PPUContext& ctx, InIt<E>& first, InIt<E>& last)
{
    for (; !at_end(ctx, first, last) && first(ctx) == ' '; first.inc(ctx)) {
    }
    if (!at_end(ctx, first, last)) {
        const u32 c = first(ctx);
        if (c == ':' || c == ',' || c == '/') first.inc(ctx);
    }
    for (; !at_end(ctx, first, last) && first(ctx) == ' '; first.inc(ctx)) {
    }
}

enum class TimeKind : u8 { Time, Date, Weekday, Monthname, Year };

template <u32 E, TimeKind K>
InIt<E> timeget(PPUContext& ctx, u32 self, InIt<E> first, InIt<E> last, u32 ios, u32 state_at, u32 tm)
{
    G g(ctx);
    const TmField sec{g, tm + kTmSec}, min{g, tm + kTmMin}, hour{g, tm + kTmHour}, mday{g, tm + kTmMday},
        mon{g, tm + kTmMon}, year{g, tm + kTmYear}, wday{g, tm + kTmWday};
    u32 st = g.r32(state_at);
    const auto commit = [&] { g.w32(state_at, st); };
    if constexpr (K == TimeKind::Time) {
        st |= getint_field<E>(ctx, first, last, 0, 23, hour);
        if (st != kGoodbit || first(ctx) != ':') st |= kFailbit;
        else {
            first.inc(ctx);
            st |= getint_field<E>(ctx, first, last, 0, 59, min);
        }
        if (st != kGoodbit || first(ctx) != ':') st |= kFailbit;
        else {
            first.inc(ctx);
            st |= getint_field<E>(ctx, first, last, 0, 59, sec);
        }
        commit();
    } else if constexpr (K == TimeKind::Weekday || K == TimeKind::Monthname) {
        const u32 names = g.r32(self + (K == TimeKind::Weekday ? kTgDays : kTgMonths));
        const s32 n = getloctxt<E>(ctx, first, last, 0, [&](usize i) -> u32 {
            const u32 a = names + E * static_cast<u32>(i);
            return names && g.ok(a, E) ? el<E>(g, a) : 0;
        });
        if (n < 0) st |= kFailbit;
        else (K == TimeKind::Weekday ? wday : mon).set(n >> 1);
        if (at_end(ctx, first, last)) st |= kEofbit;
        commit();
    } else if constexpr (K == TimeKind::Year) {
        s32 ans = 0;
        const u32 res = getint<E>(ctx, first, last, 0, 9999, ans);
        st |= res;
        if (!(res & kFailbit)) year.set(ans < 69 ? ans + 100 : ans < 100 ? ans : ans - 1900);
        commit();
    } else {
        // a date: the order's fields with separators; a month may be a name
        s32 order = static_cast<s32>(timeget_order_(ctx, self));
        if (order == kNoOrder) order = kMdy;
        const auto month_number = [&] {
            st |= getint_field<E>(ctx, first, last, 1, 12, mon);
            mon.set(mon.get() - 1);
        };
        const auto month_name = [&] {
            commit();
            vget<E>(ctx, self, kTgMonthname, first, last, ios, state_at, tm);
            st = g.r32(state_at);
        };
        const auto get_year = [&] {
            commit();
            vget<E>(ctx, self, kTgYear, first, last, ios, state_at, tm);
            st = g.r32(state_at);
        };
        if (!at_end(ctx, first, last)) {
            if (!is_digit<E>(ctx, first)) {
                month_name();
                order = kMdy;
            } else if (order == kMdy) {
                month_number();
            } else if (order == kDmy) {
                st |= getint_field<E>(ctx, first, last, 1, 31, mday);
            } else {
                get_year();
            }
        }
        skip_sep<E>(ctx, first, last);
        if (!at_end(ctx, first, last)) {
            if (!is_digit<E>(ctx, first)) {
                if (order == kMdy) st |= kFailbit; // a month was read already
                else {
                    month_name();
                    if (order == kDmy) order = kMdy; // the day then the month: the year is next
                }
            } else if (order == kMdy || order == kYdm) {
                st |= getint_field<E>(ctx, first, last, 1, 31, mday);
            } else {
                month_number();
            }
        }
        skip_sep<E>(ctx, first, last);
        if (at_end(ctx, first, last)) {
            st |= kFailbit; // a field is missing
        } else if (!is_digit<E>(ctx, first)) {
            if (order != kYdm) st |= kFailbit;
            else month_name();
        } else if (order == kYmd) {
            st |= getint_field<E>(ctx, first, last, 1, 31, mday);
        } else if (order == kYdm) {
            month_number();
        } else {
            get_year();
        }
        if (at_end(ctx, first, last)) st |= kEofbit;
        commit();
    }
    return first;
}

// do_get_*(first, last, ios_base&, iostate&, tm *): for char (result, this, first, last, ios, state, tm); for wchar_t
// the iterators in two doublewords each, the tm pointer the ninth argument.
template <TimeKind K>
void timeget_c_(PPUContext& ctx, u32 out, u32 self, u64 first, u64 last, u32 ios, u32 state, u32 tm)
{
    G g(ctx);
    if (!g.need("time_get::do_get", out, 8) || !g.need("time_get::do_get", self, kTimeGetBytes) ||
        !g.need("time_get::do_get", state, 4) || !g.need("time_get::do_get", tm, 36))
        return;
    timeget<1, K>(ctx, self, InIt<1>::regs(first), InIt<1>::regs(last), ios, state, tm).store(g, out);
}

template <TimeKind K>
void timeget_w_(PPUContext& ctx, u32 out, u32 self, u64 f0, u64 f1, u64 l0, u64 l1, u32 ios, u32 state, u32 tm)
{
    G g(ctx);
    if (!g.need("time_get::do_get", out, 12) || !g.need("time_get::do_get", self, kTimeGetBytes) ||
        !g.need("time_get::do_get", state, 4) || !g.need("time_get::do_get", tm, 36))
        return;
    timeget<4, K>(ctx, self, InIt<4>::regs(f0, f1), InIt<4>::regs(l0, l1), ios, state, tm).store(g, out);
}

// The exported _Getloctxt<_Elem, _InIt>(_InIt&, _InIt&, size_t, const _Elem *).
template <u32 E>
s32 getloctxt_(PPUContext& ctx, u32 first_at, u32 last_at, u32 numfields, u32 ptr)
{
    G g(ctx);
    if (!g.need("_Getloctxt", first_at, InIt<E>::kBytes) || !g.need("_Getloctxt", last_at, InIt<E>::kBytes)) return -1;
    InIt<E> first = InIt<E>::load(g, first_at), last = InIt<E>::load(g, last_at);
    const s32 n = getloctxt<E>(ctx, first, last, numfields, [&](usize i) -> u32 {
        const u32 a = ptr + E * static_cast<u32>(i);
        return ptr && g.ok(a, E) ? el<E>(g, a) : 0;
    });
    first.store(g, first_at);
    last.store(g, last_at);
    return n;
}

// ------------------------------------------------------------------------------------------------- time_put

// do_put(dest, ios_base&, fill, const tm *, char specifier, char modifier): strftime of "!%<modifier><specifier>".
template <u32 E>
void timeput_(PPUContext& ctx, u32 out, u32, u64 dest, u32, u32, u32 tm, u32 spec, u32 mod)
{
    G g(ctx);
    if (!g.need("time_put::do_put", out, 8)) return;
    OutIt<E> d = OutIt<E>::reg(dest);
    std::string fmt = "!%";
    if ((mod & 0xFF) != 0) fmt += static_cast<char>(mod & 0xFF);
    fmt += static_cast<char>(spec & 0xFF);
    StackVar fmtbuf(ctx, g.b, E * 8);
    for (u32 i = 0; i <= fmt.size(); i++)
        g.template wel<E>(fmtbuf.addr() + E * i, i < fmt.size() ? static_cast<u8>(fmt[i]) : 0);
    for (u32 num = 16; num <= 0x4000; num *= 2) {
        StackVar buf(ctx, g.b, E * num);
        const u32 count = static_cast<u32>(
            (E == 1 ? fn::strftime : fn::wcsftime)(ctx, {buf.addr(), num, fmtbuf.addr(), tm}));
        if (0 < count) {
            for (u32 i = 1; i < count; i++) d.put(ctx, el<E>(g, buf.addr() + E * i));
            d.store(g, out);
            return;
        }
    }
    log_call_site(ctx, g.b, std::format("time_put::do_put: strftime gives nothing for \"{}\" of the tm at {:#x}", fmt, tm));
    d.store(g, out);
}

template <u32 E, bool Deleting>
void timeput_dtor_(PPUContext& ctx, u32 self)
{
    destroy(ctx, self, {E == 1 ? kTimePutC : kTimePutW}, Deleting);
}

} // namespace pspocket::hle::stdcxx::facets
