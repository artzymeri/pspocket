#pragma once

// sys_libstdcxx, the facets part (D-561): what every facet shares. Included by stdcxx_facets.cpp only (with the
// facet families: stdcxx_facets_ctype.hpp, _num.hpp, _money.hpp, _time.hpp), in its own namespace so that nothing
// here meets a name of the streams part (stdcxx_streams.cpp) in the linker.
//
// The facets are Dinkumware's of the SDK's library (VC8's generation, D-560), made from the "C" locale, the only one
// pspocket's C library knows: each type's _Getcat makes `new F` from a default _Locinfo ("C"), as the header's does.
// Their layouts, after locale::facet {vptr, _Refs} (8 bytes):
// - ctype<char> {_Ctypevec _Ctype {_Table, _Tolotab, _Touptab, _Delfl}} (24); ctype<wchar_t> adds _Cvtvec _Cvt (+24)
//   (32). The header's inline is(), scan_is(), scan_not() and table() read _Table at +8.
// - codecvt<char, char> nothing (8); codecvt<wchar_t, char> {_Cvtvec _Cvt} (16). collate {_Collvec _Coll} (16).
//   messages, num_get, num_put, money_get, money_put, time_put nothing (8).
// - numpunct {const char *_Grouping, _Elem _Dp, _Elem _Kseparator, const _Elem *_Falsename, *_Truename}: 24 for char
//   (+8, +12, +13, +16, +20), 28 for wchar_t (+8, +12, +16, +20, +24).
// - _Mpunct (moneypunct's base) {_Grouping, _Decimalpoint, _Kseparator, _Currencysign, _Plussign, _Minussign, int
//   _Fracdigits, bool _International, pattern _Plusformat, _Minusformat}: 44 for char, 48 for wchar_t.
// - time_get {const _Elem *_Days, *_Months, dateorder _Dateorder} (20).
// Their virtual functions are in each class's declaration order in the header (the vtables in stdcxx_facets.cpp); the
// facets they use (ctype, numpunct, moneypunct) are found with use_facet and called through their vtables, so a
// game's own facet in a locale is used as the SDK's library would use it.
//
// The ABI of the iterators the facets take and give by value: istreambuf_iterator {_Strbuf, bool _Got (+4), _Elem
// _Val (+5 for char, +8 for wchar_t)} is 8 bytes for char (one doubleword) and 12 for wchar_t (two, the second
// left-justified); ostreambuf_iterator {bool _Failed, _Strbuf (+4)} is 8 bytes. An iterator, a string, a pattern
// returned by value goes through memory at r3 with `this` in r4; a string passed by value (money_put::_Putmfld) is
// passed by reference to a temporary (Itanium ABI: it has a non-trivial copy constructor).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "libc_format.hpp"
#include "stdcxx.hpp"

namespace pspocket::hle::stdcxx::facets {

// ------------------------------------------------------------------------------------------------- type names

inline constexpr char kFacetT[] = "NSt6locale5facetE", kCtypeBase[] = "St10ctype_base", kCtypeC[] = "St5ctypeIcE",
                      kCtypeW[] = "St5ctypeIwE", kCvtBase[] = "St12codecvt_base",
                      kCodecvtC[] = "St7codecvtIccSt9_MbstatetE", kCodecvtW[] = "St7codecvtIwcSt9_MbstatetE",
                      kCollateC[] = "St7collateIcE", kCollateW[] = "St7collateIwE",
                      kMessagesBase[] = "St13messages_base", kMessagesC[] = "St8messagesIcE",
                      kMessagesW[] = "St8messagesIwE", kNumpunctC[] = "St8numpunctIcE",
                      kNumpunctW[] = "St8numpunctIwE",
                      kNumGetC[] = "St7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
                      kNumGetW[] = "St7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
                      kNumPutC[] = "St7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
                      kNumPutW[] = "St7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
                      kMoneyBase[] = "St10money_base", kMpunctC[] = "St7_MpunctIcE", kMpunctW[] = "St7_MpunctIwE",
                      kMoneypunctC0[] = "St10moneypunctIcLb0EE", kMoneypunctC1[] = "St10moneypunctIcLb1EE",
                      kMoneypunctW0[] = "St10moneypunctIwLb0EE", kMoneypunctW1[] = "St10moneypunctIwLb1EE",
                      kMoneyGetC[] = "St9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
                      kMoneyGetW[] = "St9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
                      kMoneyPutC[] = "St9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
                      kMoneyPutW[] = "St9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
                      kTimeBase[] = "St9time_base",
                      kTimeGetC[] = "St8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
                      kTimeGetW[] = "St8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
                      kTimePutC[] = "St8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
                      kTimePutW[] = "St8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE";

// The facets an element type's code uses.
template <u32 E>
struct Of {
    static constexpr std::string_view ctype = E == 1 ? kCtypeC : kCtypeW;
    static constexpr std::string_view numpunct = E == 1 ? kNumpunctC : kNumpunctW;
    static constexpr std::string_view moneypunct0 = E == 1 ? kMoneypunctC0 : kMoneypunctW0;
    static constexpr std::string_view moneypunct1 = E == 1 ? kMoneypunctC1 : kMoneypunctW1;
};

// Dinkumware's categories (_X_COLLATE ... _X_MESSAGES), which _Getcat answers.
inline constexpr u32 kXCollate = 1, kXCtype = 2, kXMonetary = 3, kXNumeric = 4, kXTime = 5, kXMessages = 6;

// ------------------------------------------------------------------------------------------------- layouts

inline constexpr u32 kRefs = 4;
// ctype: _Ctypevec at +8 {_Table, _Tolotab, _Touptab, _Delfl}; ctype<wchar_t>'s _Cvtvec at +24.
inline constexpr u32 kCtTable = 8, kCtTolo = 12, kCtToup = 16, kCtDelfl = 20, kCtCvt = 24, kCtypeCBytes = 24,
                     kCtypeWBytes = 32;
inline constexpr u32 kCvtCvt = 8, kCodecvtWBytes = 16, kCollColl = 8, kCollateBytes = 16, kPlainBytes = 8;

template <u32 E>
struct NpL { // numpunct
    static constexpr u32 kGrouping = 8, kDp = 12, kKsep = E == 1 ? 13 : 16, kFalse = E == 1 ? 16 : 20,
                         kTrue = E == 1 ? 20 : 24, kBytes = E == 1 ? 24 : 28;
};

template <u32 E>
struct MpL { // _Mpunct
    static constexpr u32 kGrouping = 8, kDp = 12, kKsep = E == 1 ? 13 : 16, kCurr = E == 1 ? 16 : 20,
                         kPlus = E == 1 ? 20 : 24, kMinus = E == 1 ? 24 : 28, kFrac = E == 1 ? 28 : 32,
                         kIntl = E == 1 ? 32 : 36, kPlusfmt = E == 1 ? 33 : 37, kMinusfmt = E == 1 ? 37 : 41,
                         kBytes = E == 1 ? 44 : 48;
};

inline constexpr u32 kTgDays = 8, kTgMonths = 12, kTgOrder = 16, kTimeGetBytes = 20;

// time_base::dateorder
enum DateOrder : s32 { kNoOrder = 0, kDmy = 1, kMdy = 2, kYmd = 3, kYdm = 4 };

// The C library's struct tm: nine ints.
inline constexpr u32 kTmSec = 0, kTmMin = 4, kTmHour = 8, kTmMday = 12, kTmMon = 16, kTmYear = 20, kTmWday = 24,
                     kTmYday = 28;

// Dinkumware's limits (xlocnum): _MAX_EXP_DIG, _MAX_INT_DIG, _MAX_SIG_DIG; and its _MBMAX (MB_LEN_MAX).
inline constexpr u32 kMaxExpDig = 8, kMaxIntDig = 32, kMaxSigDig = 36, kMbLenMax = 8;
// CHAR_MAX: 127, the SDK's libraries being built with a signed char (its C library's lconv holds 0x7F for "not
// available", D-906), which the facets compare grouping sizes and fraction digits with.
inline constexpr u32 kCharMax = 0x7F;

// ------------------------------------------------------------------------------------------------- elements

// An element read from or written to guest memory (a byte, or a big-endian wchar_t).
template <u32 E>
inline u32 el(const G& g, u32 a)
{
    return g.template rel<E>(a);
}

// The elements of a NUL-terminated guest string (nullopt when it runs out of guest memory first).
template <u32 E>
std::optional<std::vector<u32>> read_cstr(const G& g, u32 p, u32 max = 0x10000)
{
    std::vector<u32> v;
    if (!p) return std::nullopt;
    for (u32 i = 0; i < max; i++) {
        if (!g.ok(p + E * i, E)) return std::nullopt;
        const u32 c = el<E>(g, p + E * i);
        if (c == 0) return v;
        v.push_back(c);
    }
    return std::nullopt;
}

// A guest basic_string<E>'s elements.
template <u32 E>
std::vector<u32> read_str(G& g, u32 at)
{
    const GStr<E> s(g, at);
    std::vector<u32> v(s.size());
    for (u32 i = 0; i < v.size(); i++) v[i] = s.at(i);
    return v;
}

// A basic_string<E> made at `out` (an aggregate result's memory, or a string already constructed when `built`).
template <u32 E>
void make_str(G& g, u32 out, const std::vector<u32>& elems, bool built = false)
{
    const GStr<E> s(g, out);
    if (!built) s.construct();
    std::vector<u8> bytes(elems.size() * E);
    for (usize i = 0; i < elems.size(); i++) {
        if constexpr (E == 1) bytes[i] = static_cast<u8>(elems[i]);
        else ps_st32(bytes.data() + 4 * i, elems[i]);
    }
    s.assign_bytes(bytes, static_cast<u32>(elems.size()));
}

inline std::vector<u32> elems_of(std::string_view s)
{
    std::vector<u32> v;
    for (const char c : s) v.push_back(static_cast<u8>(c));
    return v;
}

// new _Elem[n + 1] holding `elems` and a NUL (the header's _Maklocstr result), from the library's operator new.
template <u32 E>
u32 new_cstr(PPUContext& ctx, const std::vector<u32>& elems)
{
    const u32 p = op_new(ctx, static_cast<u32>((elems.size() + 1) * E));
    G g(ctx);
    for (usize i = 0; i < elems.size(); i++) g.template wel<E>(p + E * static_cast<u32>(i), elems[i]);
    g.template wel<E>(p + E * static_cast<u32>(elems.size()), 0);
    return p;
}

// ------------------------------------------------------------------------------------------------- calls

// sys_libc's and the streams part's functions the facets call through the registry.
namespace fn {
inline const Callee localeconv{"sys_libc", "localeconv"}, strftime{"sys_libc", "strftime"},
    wcsftime{"sys_libc", "wcsftime"}, locinfo_ctor{"sys_libstdcxx", "_ZNSt8_LocinfoC1EPKc"},
    locinfo_dtor{"sys_libstdcxx", "_ZNSt8_LocinfoD1Ev"}, getctype{"sys_libstdcxx", "_ZNKSt8_Locinfo9_GetctypeEv"},
    getcvt{"sys_libstdcxx", "_ZNKSt8_Locinfo7_GetcvtEv"}, getcoll{"sys_libstdcxx", "_ZNKSt8_Locinfo8_GetcollEv"},
    facet_dtor{"sys_libstdcxx", "_ZNSt6locale5facetD2Ev"};
} // namespace fn

// A virtual call returning an int (or char, bool: the low bits).
inline u32 vint(PPUContext& ctx, u32 obj, u32 slot, std::initializer_list<u64> args, u64 fallback = 0)
{
    return static_cast<u32>(vcall(ctx, obj, slot, args, fallback));
}

// A virtual call returning a basic_string<E> by value (do_grouping, do_truename, do_curr_symbol...): its elements;
// the temporary is destroyed as the caller's code destroys it.
template <u32 E>
std::vector<u32> vstring(PPUContext& ctx, u32 obj, u32 slot)
{
    G g(ctx);
    StackVar tmp(ctx, g.b, StrLayout<E>::kBytes + 4);
    GStr<E>(g, tmp.addr()).construct();
    vcall(ctx, obj, slot, {tmp.addr(), obj});
    std::vector<u32> v = read_str<E>(g, tmp.addr());
    GStr<E>(g, tmp.addr()).destroy();
    return v;
}

// ios_base's members the facets read: flags(), precision(), width(), width(0), getloc() (the locale object it holds).
struct Ios {
    G& g;
    u32 at;
    u32 flags() const { return g.r32(at + kIosFmtfl); }
    s32 precision() const { return static_cast<s32>(g.r32(at + kIosPrec)); }
    s32 width() const { return static_cast<s32>(g.r32(at + kIosWide)); }
    void clear_width() const { g.w32(at + kIosWide, 0); }
    u32 loc() const { return g.r32(at + kIosPloc); }
};

// ------------------------------------------------------------------------------------------------- iterators

// istreambuf_iterator<E> as a value: Dinkumware's _Peek and _Inc on the stream buffer (its sgetc and sbumpc).
template <u32 E>
struct InIt {
    static constexpr u32 kVal = E == 1 ? 5 : 8, kBytes = E == 1 ? 8 : 12;
    u32 sb = 0;
    bool got = false;
    u32 val = 0;

    // By value in registers: one doubleword for char, two for wchar_t.
    static InIt regs(u64 d0, u64 d1 = 0)
    {
        InIt it;
        it.sb = static_cast<u32>(d0 >> 32);
        it.got = ((d0 >> 24) & 0xFF) != 0;
        it.val = E == 1 ? static_cast<u32>((d0 >> 16) & 0xFF) : static_cast<u32>(d1 >> 32);
        return it;
    }
    static InIt load(const G& g, u32 at)
    {
        InIt it;
        it.sb = g.r32(at);
        it.got = g.r8(at + 4) != 0;
        it.val = el<E>(g, at + kVal);
        return it;
    }
    void store(const G& g, u32 at) const
    {
        g.w32(at, sb);
        g.w32(at + 4, got ? 0x01000000u : 0);
        if constexpr (E == 1) g.w8(at + kVal, static_cast<u8>(val));
        else g.w32(at + kVal, val);
    }
    u32 peek(PPUContext& ctx)
    {
        s32 meta = kEof;
        if (sb == 0 || (meta = streambuf_sgetc(ctx, sb, E)) == kEof) sb = 0;
        else val = E == 1 ? (static_cast<u32>(meta) & 0xFF) : static_cast<u32>(meta);
        got = true;
        return val;
    }
    void inc(PPUContext& ctx)
    {
        if (sb == 0 || streambuf_sbumpc(ctx, sb, E) == kEof) {
            sb = 0;
            got = true;
        } else {
            got = false;
        }
    }
    // operator*
    u32 operator()(PPUContext& ctx)
    {
        if (!got) peek(ctx);
        return val;
    }
};

// _First == _Last: both peeked, equal when both or neither are at the end.
template <u32 E>
bool at_end(PPUContext& ctx, InIt<E>& first, InIt<E>& last)
{
    if (!first.got) first.peek(ctx);
    if (!last.got) last.peek(ctx);
    return (first.sb == 0) == (last.sb == 0);
}

// _Getloctxt(first, last, numfields, ptr): which of the fields of `ptr` (each begins with ptr[0], the mark; `at(i)` is
// its i-th element, 0 past its end) the input matches, longest first: its number, or a negative value when none does
// (-1 once a longer field was being followed). Dinkumware's column-by-column scan: `numfields` plus one field for each
// mark in the text before its first 0.
template <u32 E, class At>
s32 getloctxt(PPUContext& ctx, InIt<E>& first, InIt<E>& last, usize numfields, At&& at)
{
    for (usize off = 0; at(off) != 0; ++off)
        if (at(off) == at(0)) ++numfields;
    std::vector<u32> seen(numfields, 0); // per field: the columns tested (0 while it is still a prefix)
    s32 ans = -2;
    for (u32 column = 1;; ++column, first.inc(ctx), ans = -1) {
        bool prefix = false;
        usize off = 0;
        for (usize field = 0; field < numfields; ++field) {
            for (; at(off) != 0 && at(off) != at(0); ++off) {
            }
            if (seen[field] != 0) {
                off += seen[field];
            } else if (at(off += column) == at(0) || at(off) == 0) {
                seen[field] = column < 127 ? column : 127;
                ans = static_cast<s32>(field);
            } else if (at_end(ctx, first, last) || at(off) != first(ctx)) {
                seen[field] = column < 127 ? column : 127;
            } else {
                prefix = true;
            }
        }
        if (!prefix || at_end(ctx, first, last)) break;
    }
    return ans;
}

// ostreambuf_iterator<E> as a value: operator= puts the element (sputc), remembering a failure.
template <u32 E>
struct OutIt {
    bool failed = false;
    u32 sb = 0;

    static OutIt reg(u64 d) { return {((d >> 56) & 0xFF) != 0, static_cast<u32>(d)}; }
    static OutIt load(const G& g, u32 at) { return {g.r8(at) != 0, g.r32(at + 4)}; }
    void store(const G& g, u32 at) const
    {
        g.w32(at, failed ? 0x01000000u : 0);
        g.w32(at + 4, sb);
    }
    void put(PPUContext& ctx, u32 ch)
    {
        if (sb == 0 || streambuf_sputc(ctx, sb, E, ch) == kEof) failed = true;
    }
    void rep(PPUContext& ctx, u32 ch, u64 count)
    {
        for (; 0 < count; --count) put(ctx, ch);
    }
    void put(PPUContext& ctx, const std::vector<u32>& elems, usize from = 0, usize count = ~usize{0})
    {
        for (usize i = from; i < elems.size() && 0 < count; i++, count--) put(ctx, elems[i]);
    }
};

// ------------------------------------------------------------------------------------------------- the "C" locale

// A default _Locinfo ("C") on the guest stack for a scope (_BEGIN_LOCINFO), made and destroyed by the streams part's
// constructor and destructor (it holds the locale lock while it lives).
class Locinfo {
public:
    explicit Locinfo(PPUContext& ctx) : ctx_(ctx), var_(ctx, guest_base(ctx), 160)
    {
        fn::locinfo_ctor(ctx_, {var_.addr(), cstring(ctx_, "C")});
    }
    ~Locinfo() { fn::locinfo_dtor(ctx_, {var_.addr()}); }
    Locinfo(const Locinfo&) = delete;
    Locinfo& operator=(const Locinfo&) = delete;
    u32 addr() const { return var_.addr(); }

private:
    PPUContext& ctx_;
    StackVar var_;
};

// _Locinfo's members the facets' _Init use: _Getlconv() (the C library's localeconv), _Getcvt(), _Getctype(),
// _Getcoll() (the streams part's, copied to `out`), _Getdays(), _Getmonths() (its time names: _Tinfo's _Days at +8
// and _Months at +20 of the _Locinfo), _Getfalse() and _Gettrue() (constants), _Getdateorder().
inline constexpr u32 kInfoDays = 8, kInfoMonths = 20, kInfoFormats = 32;

inline u32 lconv_of(PPUContext& ctx) { return static_cast<u32>(fn::localeconv(ctx, {})); }

inline void info_vec(PPUContext& ctx, const Callee& f, u32 lobj, u32 out, u32 bytes)
{
    G g(ctx);
    StackVar tmp(ctx, g.b, 16);
    f(ctx, {tmp.addr(), lobj});
    std::memcpy(g.b + out, g.b + tmp.addr(), bytes);
}

inline std::string info_text(PPUContext& ctx, u32 lobj, u32 field, std::string_view fallback)
{
    G g(ctx);
    const u32 p = g.ok(lobj + field, 4) ? g.r32(lobj + field) : 0;
    if (p)
        if (auto s = g.str(p)) return *s;
    return std::string(fallback);
}

inline constexpr std::string_view kCDays = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:"
                                           "Saturday";
inline constexpr std::string_view kCMonths = ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:"
                                             "Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";

// _Getdateorder(): the order of %d, %m and %y in the locale's date format (_Tinfo's formats, "|%c|%x|%X|..."): mdy for
// the "C" locale's "%m/%d/%y"; no_order when it has none of them.
inline s32 date_order(PPUContext& ctx, u32 lobj)
{
    const std::string formats = info_text(ctx, lobj, kInfoFormats, "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S");
    std::string x;
    if (!formats.empty()) {
        const char sep = formats[0];
        const usize a = formats.find(sep, 1);
        if (a != std::string::npos) {
            const usize b = formats.find(sep, a + 1);
            x = formats.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        }
    }
    std::string order;
    for (usize i = 0; i + 1 < x.size(); i++) {
        if (x[i] != '%') continue;
        const char c = x[++i];
        if (c == 'd' || c == 'e') order += 'd';
        else if (c == 'm') order += 'm';
        else if (c == 'y' || c == 'Y') order += 'y';
    }
    return order == "dmy" ? kDmy : order == "mdy" ? kMdy : order == "ymd" ? kYmd : order == "ydm" ? kYdm : kNoOrder;
}

// The C locale's multibyte conversions (Dinkumware's "C" locale, D-512): a byte is the wide character of its value; a
// wide character past 255 has no byte.
inline u32 c_widen(u32 byte) { return byte & 0xFF; }
inline std::optional<u8> c_narrow(u32 wc)
{
    if (wc > 0xFF) return std::nullopt;
    return static_cast<u8>(wc);
}

// _Maklocstr<_Elem>(const char *, _Elem *, const _Cvtvec&): new _Elem[] with the string's characters converted.
template <u32 E>
u32 maklocstr(PPUContext& ctx, std::string_view s)
{
    std::vector<u32> v;
    for (const char c : s) v.push_back(c_widen(static_cast<u8>(c)));
    return new_cstr<E>(ctx, v);
}

// The C string at a facet's field (the facet's own copy).
template <u32 E>
std::vector<u32> field_cstr(const G& g, u32 self, u32 field)
{
    return read_cstr<E>(g, g.r32(self + field)).value_or(std::vector<u32>{});
}

// ------------------------------------------------------------------------------------------------- facet objects

// `new F` of the library's operator new with F's vtable and _Refs 0 (the header's facet(size_t _Refs = 0)).
inline u32 new_facet(PPUContext& ctx, std::string_view type, u32 bytes)
{
    const u32 p = op_new(ctx, bytes);
    G g(ctx);
    std::memset(g.b + p, 0, bytes);
    g.w32(p, vtable(ctx, type));
    g.w32(p + kRefs, 0);
    return p;
}

// _Getcat(const locale::facet **_Ppf): the category; with a pointer to null, `make()` stores the facet there.
template <class Make>
u32 getcat(PPUContext& ctx, u32 ppf, u32 category, Make&& make)
{
    G g(ctx);
    if (ppf && g.need("_Getcat", ppf, 4) && g.r32(ppf) == 0) g.w32(ppf, make());
    return category;
}

// A destructor chain: each class's vtable set in turn (the derived class's first, as each destructor body runs with
// its own class's), `tidy` between the first and the rest; facet's last; D0 frees the object.
inline void destroy(PPUContext& ctx, u32 self, std::initializer_list<std::string_view> chain, bool deleting,
    void (*tidy)(PPUContext&, u32) = nullptr)
{
    G g(ctx);
    if (!g.need("~facet", self, 8)) return;
    bool first = true;
    for (const std::string_view cls : chain) {
        g.w32(self, vtable(ctx, cls));
        if (first && tidy) tidy(ctx, self);
        first = false;
    }
    g.w32(self, vtable(ctx, kFacetT));
    if (deleting) op_delete(ctx, self);
}

} // namespace pspocket::hle::stdcxx::facets
