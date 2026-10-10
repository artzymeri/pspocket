// sys_libstdcxx's facets (runtime/hle/src/stdcxx_facets*.hpp, D-561): Dinkumware's facets of the SDK's library in the
// "C" locale, checked against the C++ standard and Dinkumware's algorithms (num_put's text against printf's for the
// format its flags make), each function called as guest code calls it: aggregates through memory at r3, iterators by
// value in doublewords, a double in f1. Stream buffers are laid out by hand over guest memory. No game data.

#include <catch_amalgamated.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp" // runtime/hle/src (internal): call_guest
#include "hle_test_util.hpp"
#include "pspocket/recomp/nid.hpp"
#include "stdcxx.hpp" // runtime/hle/src (internal): the library's own data (the facet ids it keeps)

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr std::string_view kMod = "sys_libstdcxx";
constexpr u32 kIos = kData + 0x100, kSb = kData + 0x200, kVt = kData + 0x300, kRet = kData + 0x400,
              kState = kData + 0x480, kVal = kData + 0x490, kTm = kData + 0x500, kStr = kData + 0x600,
              kStr2 = kData + 0x640, kLoc = kData + 0x680, kSlot = kData + 0x690, kIn = kData + 0x1000,
              kOut = kData + 0x2000, kBuf = kData + 0x3000;

void put(u8* b, u32 at, std::string_view s)
{
    std::memcpy(b + at, s.data(), s.size());
    b[at + s.size()] = 0;
}

std::string cstr(u8* b, u32 at)
{
    std::string s;
    while (b[at + s.size()]) s += static_cast<char>(b[at + s.size()]);
    return s;
}

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

std::string fn_name(u8* b, u32 d)
{
    const LinkedFunction* f = HleProcess::current()->linked_at(ps_read32(b, d));
    return f && f->impl ? std::string(f->impl->name) : std::string();
}

// The type name of an object's dynamic type (its vtable's type_info).
std::string type_name(u8* b, u32 obj) { return cstr(b, ps_read32(b, ps_read32(b, ps_read32(b, obj) - 4) + 4)); }

// A basic_string object's elements: {allocator byte, _Bx (16), _Mysize, _Myres}.
template <u32 E>
std::u32string str_of(u8* b, u32 at)
{
    const u32 size = ps_read32(b, at + 20), res = ps_read32(b, at + 24);
    const u32 p = res >= 16 / E ? ps_read32(b, at + 4) : at + 4;
    std::u32string s;
    for (u32 i = 0; i < size; i++) s += static_cast<char32_t>(E == 1 ? b[p + i] : ps_read32(b, p + 4 * i));
    return s;
}

std::u32string text32(std::string_view s)
{
    std::u32string r;
    for (const char c : s) r += static_cast<char32_t>(static_cast<u8>(c));
    return r;
}

// The library's basic_streambuf<E> virtual functions (overflow and underflow answer EOF).
template <u32 E>
std::vector<std::string> streambuf_fns()
{
    const std::string c = E == 1 ? "_ZNSt15basic_streambufIcSt11char_traitsIcEE" : "_ZNSt15basic_streambufIwSt11char_traitsIwEE";
    const std::string el = E == 1 ? "c" : "w";
    return {c + "D1Ev", c + "D0Ev", c + "5_LockEv", c + "7_UnlockEv", c + "8overflowEi", c + "9pbackfailEi",
        c + "9showmanycEv", c + "9underflowEv", c + "5uflowEv", c + "6xsgetnEP" + el + "i",
        c + "6xsputnEPK" + el + "i"};
}

// The facet types and their members' mangled names.
struct Names {
    std::string_view type; // as type_info names it
    std::string_view cls;  // the _ZNSt prefix of its members
    u32 category;
};

constexpr Names kFacets[] = {
    {"St5ctypeIcE", "_ZNSt5ctypeIcE", 2},
    {"St5ctypeIwE", "_ZNSt5ctypeIwE", 2},
    {"St7codecvtIccSt9_MbstatetE", "_ZNSt7codecvtIccSt9_MbstatetE", 2},
    {"St7codecvtIwcSt9_MbstatetE", "_ZNSt7codecvtIwcSt9_MbstatetE", 2},
    {"St7collateIcE", "_ZNSt7collateIcE", 1},
    {"St7collateIwE", "_ZNSt7collateIwE", 1},
    {"St8messagesIcE", "_ZNSt8messagesIcE", 6},
    {"St8messagesIwE", "_ZNSt8messagesIwE", 6},
    {"St8numpunctIcE", "_ZNSt8numpunctIcE", 4},
    {"St8numpunctIwE", "_ZNSt8numpunctIwE", 4},
    {"St7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE", 4},
    {"St7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
        "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE", 4},
    {"St7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE", 4},
    {"St7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
        "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE", 4},
    {"St10moneypunctIcLb0EE", "_ZNSt10moneypunctIcLb0EE", 3},
    {"St10moneypunctIcLb1EE", "_ZNSt10moneypunctIcLb1EE", 3},
    {"St10moneypunctIwLb0EE", "_ZNSt10moneypunctIwLb0EE", 3},
    {"St10moneypunctIwLb1EE", "_ZNSt10moneypunctIwLb1EE", 3},
    {"St9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE", 3},
    {"St9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
        "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE", 3},
    {"St9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE", 3},
    {"St9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
        "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE", 3},
    {"St8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE", 5},
    {"St8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
        "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE", 5},
    {"St8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE", 5},
    {"St8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
        "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE", 5},
};

std::string getcat_name(const Names& n) { return std::string(n.cls) + "7_GetcatEPPKNSt6locale5facetE"; }

// A thread with an ios_base (the global "C" locale, dec, precision 6) and a stream buffer laid out by hand.
struct Env {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();

    Env()
    {
        std::memset(b + kIos, 0, 64);
        t.call("_ZNSt8ios_base5_InitEv", kIos);
    }

    void flags(u32 fl) { ps_write32(b, kIos + 16, fl); }
    void precision(s32 p) { ps_write32(b, kIos + 20, static_cast<u32>(p)); }
    void width(s32 w) { ps_write32(b, kIos + 24, static_cast<u32>(w)); }
    s32 width() { return static_cast<s32>(ps_read32(b, kIos + 24)); }

    // A facet of `type` from its _Getcat.
    u32 facet(std::string_view cls)
    {
        ps_write32(b, kSlot, 0);
        t.call(std::string(cls) + "7_GetcatEPPKNSt6locale5facetE", kSlot);
        return ps_read32(b, kSlot);
    }

    // The stream buffer: the library's virtual functions, its pointers at its own fields.
    template <u32 E>
    void streambuf()
    {
        std::memset(b + kSb, 0, 60);
        u32 i = 0;
        ps_write32(b, kVt, 0);
        ps_write32(b, kVt + 4, 0);
        for (const std::string& fn : streambuf_fns<E>())
            ps_write32(b, kVt + 8 + 4 * i++, f.hle->link_function(kMod, recomp::nid_of(fn), fn));
        ps_write32(b, kSb, kVt + 8);
        for (u32 k = 0; k < 6; k++)
            ps_write32(b, kSb + 16 + 4 * (k + (k / 2) * 2), kSb + 8 + 4 * (k + (k / 2) * 2));
    }

    // Input: the get area over `text`'s elements.
    template <u32 E>
    void input(std::u32string_view text)
    {
        streambuf<E>();
        for (u32 i = 0; i < text.size(); i++) {
            if (E == 1) b[kIn + i] = static_cast<u8>(text[i]);
            else ps_write32(b, kIn + 4 * i, static_cast<u32>(text[i]));
        }
        ps_write32(b, kSb + 8, kIn);
        ps_write32(b, kSb + 24, kIn);
        ps_write32(b, kSb + 40, kIn + E * static_cast<u32>(text.size()));
    }

    // Output: the put area over 4 KB.
    template <u32 E>
    void output()
    {
        streambuf<E>();
        ps_write32(b, kSb + 12, kOut);
        ps_write32(b, kSb + 28, kOut);
        ps_write32(b, kSb + 44, kOut + 0x1000);
    }

    // What was put, and the input's elements left (from gptr).
    template <u32 E>
    std::u32string written()
    {
        std::u32string s;
        for (u32 p = kOut; p < ps_read32(b, kSb + 28); p += E) s += static_cast<char32_t>(E == 1 ? b[p] : ps_read32(b, p));
        return s;
    }
    template <u32 E>
    u32 consumed()
    {
        return (ps_read32(b, kSb + 24) - kIn) / E;
    }

    // A call with a double in f1.
    template <class... A>
    u64 callf(std::string_view name, double v, A... args)
    {
        t.ctx.fpr[1] = v;
        return t.call(name, args...);
    }

    // The id the library gives a standard facet type (its own id object).
    u32 facet_id(std::string_view type)
    {
        const std::string name = std::string("_ZNSt") + std::string(type.substr(2)) + "2idE";
        return static_cast<u32>(t.call("_ZNSt6locale2idcvjEv", stdcxx::static_object(name, 4)));
    }
};

// The ostreambuf_iterator over the stream buffer, by value: {_Failed, _Strbuf}.
constexpr u64 kOit = kSb;
// istreambuf_iterators by value: first over the stream buffer, last the end of stream.
constexpr u64 kIitC = u64{kSb} << 32;

const std::string kNumPutC = "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basec";
const std::string kNumPutW = "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basew";
const std::string kNumGetC =
    "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_RSt8ios_baseRNSt5_IosbIiE8_IostateER";
const std::string kNumGetW =
    "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_RSt8ios_baseRNSt5_IosbIiE8_IostateER";

constexpr u32 kUppercase = 0x4, kShowbase = 0x8, kShowpoint = 0x10, kShowpos = 0x20, kLeft = 0x40,
              kRight = 0x80, kInternal = 0x100, kDec = 0x200, kOct = 0x400, kHex = 0x800, kScientific = 0x1000,
              kFixed = 0x2000, kBoolalpha = 0x4000;

std::string printf_text(const char* fmt, auto... v)
{
    char buf[512];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    std::snprintf(buf, sizeof buf, fmt, v...);
#pragma clang diagnostic pop
    return buf;
}

// The text printf gives for the format num_put's _Ifmt makes of these flags, for a value of `bits` (a long is 32).
std::string int_printf(u32 fl, long long v, bool is_signed, u32 bits)
{
    std::string f = "%";
    if (fl & kShowpos) f += '+';
    if (fl & kShowbase) f += '#';
    f += "ll";
    const u32 base = fl & 0xE00;
    const char conv = base == kOct ? 'o' : base != kHex ? (is_signed ? 'd' : 'u') : (fl & kUppercase) ? 'X' : 'x';
    f += conv;
    if (conv != 'd' && bits == 32) v = static_cast<long long>(static_cast<u32>(v));
    return printf_text(f.c_str(), v);
}

// The width padding num_put does: fill before (right), after (left), or after the sign or 0x (internal).
std::string pad(std::string s, s32 width, char fill, u32 fl)
{
    if (width <= static_cast<s32>(s.size())) return s;
    const usize n = static_cast<usize>(width) - s.size();
    const u32 adjust = fl & 0x1C0;
    if (adjust == kLeft) return s + std::string(n, fill);
    if (adjust == kInternal) {
        const usize prefix = !s.empty() && (s[0] == '+' || s[0] == '-') ? 1
            : s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 2
                                                                           : 0;
        return s.substr(0, prefix) + std::string(n, fill) + s.substr(prefix);
    }
    return std::string(n, fill) + s;
}

} // namespace

TEST_CASE("sys_libstdcxx facets: every function registered, the vtables in the header's order", "[hle][libstdcxx]")
{
    usize n = 0;
    for (const Function& f : registry().functions())
        if (f.module == kMod && !f.internal) n++;
    CHECK(n == 556); // every name RPCS3 gives sys_libstdcxx: both parts
    // The four _Getcat the export list doesn't name, internal functions of the library.
    for (const char* name : {"_ZNSt7codecvtIccSt9_MbstatetE7_GetcatEPPKNSt6locale5facetE",
             "_ZNSt8numpunctIcE7_GetcatEPPKNSt6locale5facetE",
             "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE7_GetcatEPPKNSt6locale5facetE",
             "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE7_GetcatEPPKNSt6locale5facetE"}) {
        const Function* f = registry().find(kMod, recomp::nid_of(name));
        REQUIRE(f != nullptr);
        CHECK(f->internal);
    }

    Env e;
    u8* b = e.b;
    // ctype<char>: [~D1, ~D0, do_tolower x2, do_toupper x2, do_widen x2, do_narrow x2]; type_info chain.
    const u32 ct = e.facet("_ZNSt5ctypeIcE");
    const u32 vt = ps_read32(b, ct);
    CHECK(fn_name(b, ps_read32(b, vt)) == "_ZNSt5ctypeIcED1Ev");
    CHECK(fn_name(b, ps_read32(b, vt + 4 * 6)) == "_ZNKSt5ctypeIcE8do_widenEc");
    CHECK(fn_name(b, ps_read32(b, vt + 4 * 9)) == "_ZNKSt5ctypeIcE9do_narrowEPKcS2_cPc");
    const u32 ti = ps_read32(b, vt - 4);
    CHECK(cstr(b, ps_read32(b, ti + 4)) == "St5ctypeIcE");
    CHECK(cstr(b, ps_read32(b, ps_read32(b, ti + 8) + 4)) == "St10ctype_base");
    // ctype<wchar_t>'s do_widen(char) at 10 (what basic_ios::init calls); codecvt's do_in, do_out, do_unshift at 5-7.
    const u32 ctw = e.facet("_ZNSt5ctypeIwE");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, ctw) + 40)) == "_ZNKSt5ctypeIwE8do_widenEc");
    const u32 cvt = e.facet("_ZNSt7codecvtIccSt9_MbstatetE");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, cvt) + 8)) == "_ZNKSt12codecvt_base16do_always_noconvEv");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, cvt) + 20)) == "_ZNKSt7codecvtIccSt9_MbstatetE5do_inERS0_PKcS4_RS4_PcS6_RS6_");
    // num_get: do_get(long&) at 5, (void*&) last; num_put: do_put(double) at 7.
    const u32 ng = e.facet("_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, ng) + 20)) == kNumGetC + "l");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, ng) + 48)) == kNumGetC + "Pv");
    const u32 np = e.facet("_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, np) + 28)) == kNumPutC + "d");
    // moneypunct<char, true>: its own destructors, _Mpunct's functions; type_info moneypunct -> _Mpunct -> money_base.
    const u32 mp = e.facet("_ZNSt10moneypunctIcLb1EE");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, mp))) == "_ZNSt10moneypunctIcLb1EED1Ev");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, mp) + 40)) == "_ZNKSt7_MpunctIcE13do_neg_formatEv");
    const u32 mti = ps_read32(b, ps_read32(b, mp) - 4);
    CHECK(cstr(b, ps_read32(b, ps_read32(b, mti + 8) + 4)) == "St7_MpunctIcE");
    CHECK(cstr(b, ps_read32(b, ps_read32(b, ps_read32(b, mti + 8) + 8) + 4)) == "St10money_base");
    // time_get: do_date_order first, do_get_year last.
    const u32 tg = e.facet("_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, tg) + 8)) ==
          "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE13do_date_orderEv");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, tg) + 28)).starts_with(
        "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE11do_get_year"));
}

TEST_CASE("sys_libstdcxx facets: _Getcat answers the category and makes each facet from the \"C\" locale",
    "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    for (const Names& n : kFacets) {
        INFO(n.type);
        CHECK(e.t.call(getcat_name(n), 0) == n.category);
        ps_write32(b, kSlot, 0x1234); // a facet already there is kept
        CHECK(e.t.call(getcat_name(n), kSlot) == n.category);
        CHECK(ps_read32(b, kSlot) == 0x1234);
        const u32 fac = e.facet(n.cls);
        REQUIRE(fac != 0);
        CHECK(type_name(b, fac) == n.type);
        CHECK(ps_read32(b, fac + 4) == 0); // _Refs
        // delete through the vtable's deleting destructor, as the library does
        call_guest(e.t.ctx, ps_read32(b, ps_read32(b, fac) + 4), {fac});
    }
    // The bases' destructors leave their own vtable, then facet's; D0 frees.
    const u32 ct = e.facet("_ZNSt5ctypeIcE");
    e.t.call("_ZNSt5ctypeIcED1Ev", ct);
    CHECK(type_name(b, ct) == "NSt6locale5facetE");
    const u32 mp = e.facet("_ZNSt10moneypunctIwLb0EE");
    e.t.call("_ZNSt7_MpunctIwED2Ev", mp);
    CHECK(type_name(b, mp) == "NSt6locale5facetE");
    const u32 tg = e.facet("_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE");
    e.t.call("_ZNSt9time_baseD2Ev", tg);
    CHECK(type_name(b, tg) == "NSt6locale5facetE");
}

TEST_CASE("sys_libstdcxx facets: ctype<char> and ctype<wchar_t> in the \"C\" locale", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    // ctype<char>: the C library's tables at +8 (the header's inline is() reads _Table), no table of its own.
    const u32 ct = e.facet("_ZNSt5ctypeIcE");
    CHECK(ps_read32(b, ct + 8) == e.t.call("_Getpctype"));
    CHECK(ps_read32(b, ct + 12) == e.t.call("_Getptolower"));
    CHECK(ps_read32(b, ct + 16) == e.t.call("_Getptoupper"));
    CHECK(ps_read32(b, ct + 20) == 0);
    CHECK(e.t.call("_Getctyptab") == ps_read32(b, ct + 8));
    CHECK(e.t.call("_ZNKSt5ctypeIcE10do_tolowerEc", ct, 'Q') == 'q');
    CHECK(e.t.call("_ZNKSt5ctypeIcE10do_tolowerEc", ct, 'q') == 'q');
    CHECK(e.t.call("_ZNKSt5ctypeIcE10do_toupperEc", ct, 'z') == 'Z');
    CHECK(e.t.call("_ZNKSt5ctypeIcE10do_toupperEc", ct, 0xE9) == 0xE9); // nothing past ASCII
    put(b, kStr, "Hello, World 9");
    CHECK(e.t.call("_ZNKSt5ctypeIcE10do_toupperEPcPKc", ct, kStr, kStr + 14) == kStr + 14);
    CHECK(cstr(b, kStr) == "HELLO, WORLD 9");
    CHECK(e.t.call("_ZNKSt5ctypeIcE10do_tolowerEPcPKc", ct, kStr, kStr + 5) == kStr + 5);
    CHECK(cstr(b, kStr) == "hello, WORLD 9");
    CHECK(e.t.call("_ZNKSt5ctypeIcE8do_widenEc", ct, 0xC8) == 0xC8);
    CHECK(e.t.call("_ZNKSt5ctypeIcE9do_narrowEcc", ct, 'x', '?') == 'x');
    CHECK(e.t.call("_ZNKSt5ctypeIcE8do_widenEPKcS2_Pc", ct, kStr, kStr + 5, kStr2) == kStr + 5);
    CHECK(std::string(reinterpret_cast<char*>(b + kStr2), 5) == "hello");
    CHECK(e.t.call("_ZNKSt5ctypeIcE9do_narrowEPKcS2_cPc", ct, kStr, kStr + 3, '?', kStr2 + 8) == kStr + 3);
    CHECK(std::string(reinterpret_cast<char*>(b + kStr2 + 8), 3) == "hel");
    // A facet that owns its table frees it with free (_Delfl > 0).
    const u32 tab = static_cast<u32>(e.t.call("malloc", 514));
    ps_write32(b, ct + 8, tab);
    ps_write32(b, ct + 20, 1);
    e.t.call("_ZNSt5ctypeIcED1Ev", ct);

    // ctype<wchar_t>: classes from the same table for bytes, none past 255; the masks are Dinkumware's bits.
    const u32 cw = e.facet("_ZNSt5ctypeIwE");
    constexpr u32 kAlpha = 0x10 | 0x02 | 0x200, kDigit = 0x20, kSpace = 0x40 | 0x04 | 0x100, kUpper = 0x02;
    CHECK(e.t.call("_ZNKSt5ctypeIwE5do_isEsw", cw, kAlpha, 'a') == 1);
    CHECK(e.t.call("_ZNKSt5ctypeIwE5do_isEsw", cw, kDigit, 'a') == 0);
    CHECK(e.t.call("_ZNKSt5ctypeIwE5do_isEsw", cw, kSpace, '\t') == 1);
    CHECK(e.t.call("_ZNKSt5ctypeIwE5do_isEsw", cw, kAlpha, 0x3B1) == 0);
    const std::u32string text = U"aZ 7\x3B1";
    for (u32 i = 0; i < text.size(); i++) ps_write32(b, kStr + 4 * i, static_cast<u32>(text[i]));
    const u32 end = kStr + 4 * static_cast<u32>(text.size());
    CHECK(e.t.call("_ZNKSt5ctypeIwE5do_isEPKwS2_Ps", cw, kStr, end, kStr2) == end);
    CHECK((ps_read16(b, kStr2) & 0x10) != 0);      // 'a': _LO
    CHECK((ps_read16(b, kStr2 + 2) & kUpper) != 0); // 'Z'
    CHECK(ps_read16(b, kStr2 + 8) == 0);            // alpha: none
    CHECK(e.t.call("_ZNKSt5ctypeIwE10do_scan_isEsPKwS2_", cw, kDigit, kStr, end) == kStr + 12);
    CHECK(e.t.call("_ZNKSt5ctypeIwE11do_scan_notEsPKwS2_", cw, kAlpha, kStr, end) == kStr + 8);
    CHECK(e.t.call("_ZNKSt5ctypeIwE10do_scan_isEsPKwS2_", cw, 0x08, kStr, end) == end); // no punctuation
    CHECK(e.t.call("_ZNKSt5ctypeIwE10do_toupperEw", cw, 'k') == 'K');
    CHECK(e.t.call("_ZNKSt5ctypeIwE10do_tolowerEw", cw, 0x391) == 0x391);
    CHECK(e.t.call("_ZNKSt5ctypeIwE10do_toupperEPwPKw", cw, kStr, end) == end);
    CHECK(ps_read32(b, kStr) == 'A');
    CHECK(ps_read32(b, kStr + 16) == 0x3B1);
    CHECK(e.t.call("_ZNKSt5ctypeIwE10do_tolowerEPwPKw", cw, kStr, kStr + 8) == kStr + 8);
    CHECK(ps_read32(b, kStr + 4) == 'z');
    CHECK(e.t.call("_ZNKSt5ctypeIwE8do_widenEc", cw, 0xE9) == 0xE9);
    CHECK(e.t.call("_ZNKSt5ctypeIwE8_DowidenEc", cw, 'w') == 'w');
    CHECK(e.t.call("_ZNKSt5ctypeIwE9do_narrowEwc", cw, 'n', '?') == 'n');
    CHECK(e.t.call("_ZNKSt5ctypeIwE9do_narrowEwc", cw, 0x20AC, '?') == '?');
    CHECK(e.t.call("_ZNKSt5ctypeIwE9_DonarrowEwc", cw, 0x100, '#') == '#');
    put(b, kBuf, "ab");
    CHECK(e.t.call("_ZNKSt5ctypeIwE8do_widenEPKcS2_Pw", cw, kBuf, kBuf + 2, kStr2) == kBuf + 2);
    CHECK(ps_read32(b, kStr2 + 4) == 'b');
    CHECK(e.t.call("_ZNKSt5ctypeIwE9do_narrowEPKwS2_cPc", cw, kStr, end, '?', kBuf) == end);
    CHECK(std::string(reinterpret_cast<char*>(b + kBuf), 5) == "az 7?");
}

TEST_CASE("sys_libstdcxx facets: the codecvts", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    // codecvt<char, char>: codecvt_base's always-noconv, nothing converted.
    const u32 cc = e.facet("_ZNSt7codecvtIccSt9_MbstatetE");
    CHECK(e.t.call("_ZNKSt12codecvt_base16do_always_noconvEv", cc) == 1);
    CHECK(e.t.call("_ZNKSt12codecvt_base13do_max_lengthEv", cc) == 1);
    CHECK(e.t.call("_ZNKSt12codecvt_base11do_encodingEv", cc) == 1);
    put(b, kStr, "bytes");
    const u32 mid1 = kVal, mid2 = kVal + 4;
    CHECK(e.t.call("_ZNKSt7codecvtIccSt9_MbstatetE5do_inERS0_PKcS4_RS4_PcS6_RS6_", cc, kState, kStr, kStr + 5, mid1, kBuf,
              kBuf + 8, mid2) == 3); // noconv
    CHECK(ps_read32(b, mid1) == kStr);
    CHECK(ps_read32(b, mid2) == kBuf);
    CHECK(e.t.call("_ZNKSt7codecvtIccSt9_MbstatetE10do_unshiftERS0_PcS3_RS3_", cc, kState, kBuf, kBuf + 8, mid2) == 3);
    CHECK(e.t.call("_ZNKSt7codecvtIccSt9_MbstatetE9do_lengthERKS0_PKcS5_j", cc, kState, kStr, kStr + 5, 3) == 3);
    CHECK(e.t.call("_ZNKSt7codecvtIccSt9_MbstatetE9do_lengthERKS0_PKcS5_j", cc, kState, kStr, kStr + 5, 9) == 5);

    // codecvt<wchar_t, char>: a byte for each wide character up to 255.
    const u32 cw = e.facet("_ZNSt7codecvtIwcSt9_MbstatetE");
    const char* in = "_ZNKSt7codecvtIwcSt9_MbstatetE5do_inERS0_PKcS4_RS4_PwS6_RS6_";
    const char* out = "_ZNKSt7codecvtIwcSt9_MbstatetE6do_outERS0_PKwS4_RS4_PcS6_RS6_";
    CHECK(e.t.call("_ZNKSt7codecvtIwcSt9_MbstatetE16do_always_noconvEv", cw) == 0);
    CHECK(e.t.call("_ZNKSt7codecvtIwcSt9_MbstatetE13do_max_lengthEv", cw) == 8);
    CHECK(e.t.call("_ZNKSt7codecvtIwcSt9_MbstatetE11do_encodingEv", cw) == 0);
    put(b, kStr, "w\xE9z");
    CHECK(e.t.call(in, cw, kState, kStr, kStr + 3, mid1, kBuf, kBuf + 16, mid2) == 0); // ok
    CHECK(ps_read32(b, mid1) == kStr + 3);
    CHECK(ps_read32(b, mid2) == kBuf + 12);
    CHECK(ps_read32(b, kBuf + 4) == 0xE9);
    // Room for two: ok all the same (Dinkumware's), the pointers after the two.
    CHECK(e.t.call(in, cw, kState, kStr, kStr + 3, mid1, kBuf, kBuf + 8, mid2) == 0);
    CHECK(ps_read32(b, mid1) == kStr + 2);
    // No room at all: partial.
    CHECK(e.t.call(in, cw, kState, kStr, kStr + 3, mid1, kBuf, kBuf, mid2) == 1);
    CHECK(e.t.call(in, cw, kState, kStr, kStr, mid1, kBuf, kBuf + 8, mid2) == 0);
    ps_write32(b, kBuf, 'o');
    ps_write32(b, kBuf + 4, 'k');
    ps_write32(b, kBuf + 8, 0x3A9);
    CHECK(e.t.call(out, cw, kState, kBuf, kBuf + 8, mid1, kStr2, kStr2 + 8, mid2) == 0);
    CHECK(std::string(reinterpret_cast<char*>(b + kStr2), 2) == "ok");
    CHECK(ps_read32(b, mid2) == kStr2 + 2);
    CHECK(e.t.call(out, cw, kState, kBuf, kBuf + 12, mid1, kStr2, kStr2 + 8, mid2) == 2); // error at the omega
    CHECK(ps_read32(b, mid1) == kBuf + 8);
    CHECK(ps_read32(b, mid2) == kStr2 + 2);
    CHECK(e.t.call("_ZNKSt7codecvtIwcSt9_MbstatetE10do_unshiftERS0_PcS3_RS3_", cw, kState, kStr2, kStr2 + 8, mid2) == 0);
    CHECK(ps_read32(b, mid2) == kStr2);
    CHECK(e.t.call("_ZNKSt7codecvtIwcSt9_MbstatetE9do_lengthERKS0_PKcS5_j", cw, kState, kStr, kStr + 3, 2) == 2);
}

TEST_CASE("sys_libstdcxx facets: collate and messages", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    const u32 co = e.facet("_ZNSt7collateIcE");
    const char* cmp = "_ZNKSt7collateIcE10do_compareEPKcS2_S2_S2_";
    put(b, kStr, "apple");
    put(b, kStr2, "apricot");
    CHECK(i32(e.t.call(cmp, co, kStr, kStr + 5, kStr2, kStr2 + 7)) == -1);
    CHECK(i32(e.t.call(cmp, co, kStr2, kStr2 + 7, kStr, kStr + 5)) == 1);
    CHECK(i32(e.t.call(cmp, co, kStr, kStr + 2, kStr2, kStr2 + 2)) == 0);
    CHECK(i32(e.t.call(cmp, co, kStr, kStr + 2, kStr2, kStr2 + 3)) == -1); // a prefix sorts first
    put(b, kBuf, "\xE0");
    CHECK(i32(e.t.call(cmp, co, kBuf, kBuf + 1, kStr, kStr + 1)) == 1); // bytes compare unsigned
    e.t.call("_ZNKSt7collateIcE12do_transformEPKcS2_", kRet, co, kStr, kStr + 5);
    CHECK(str_of<1>(b, kRet) == U"apple");
    u32 h = 0;
    for (const char c : std::string_view("apple")) h = ((h << 8) | (h >> 24)) + static_cast<u8>(c);
    CHECK(e.t.call("_ZNKSt7collateIcE7do_hashEPKcS2_", co, kStr, kStr + 5) == h);
    const u32 cw = e.facet("_ZNSt7collateIwE");
    ps_write32(b, kBuf, 0x100);
    ps_write32(b, kBuf + 4, 'a');
    CHECK(i32(e.t.call("_ZNKSt7collateIwE10do_compareEPKwS2_S2_S2_", cw, kBuf, kBuf + 4, kBuf + 4, kBuf + 8)) == 1);
    e.t.call("_ZNKSt7collateIwE12do_transformEPKwS2_", kRet, cw, kBuf, kBuf + 8);
    CHECK(str_of<4>(b, kRet) == std::u32string{0x100, 'a'});
    CHECK(e.t.call("_ZNKSt7collateIwE7do_hashEPKwS2_", cw, kBuf, kBuf + 8) == ((0x100u << 8) + 'a'));

    // messages: no catalogs; do_get answers the default.
    const u32 ms = e.facet("_ZNSt8messagesIcE");
    e.t.call("_ZNSsC1EPKc", kStr2, kStr);
    CHECK(i32(e.t.call("_ZNKSt8messagesIcE7do_openERKSsRKSt6locale", ms, kStr2, ps_read32(b, kIos + 36))) == -1);
    e.t.call("_ZNKSt8messagesIcE6do_getEiiiRKSs", kRet, ms, 0, 1, 2, kStr2);
    CHECK(str_of<1>(b, kRet) == U"apple");
    e.t.call("_ZNKSt8messagesIcE8do_closeEi", ms, 0);
    const u32 mw = e.facet("_ZNSt8messagesIwE");
    e.t.call("_ZNSbIwSt11char_traitsIwESaIwEEC1Ev", kStr2 + 32);
    e.t.call("_ZNSbIwSt11char_traitsIwESaIwEE6appendEjw", kStr2 + 32, 3, 'm');
    e.t.call("_ZNKSt8messagesIwE6do_getEiiiRKSbIwSt11char_traitsIwESaIwEE", kRet, mw, -1, 0, 0, kStr2 + 32);
    CHECK(str_of<4>(b, kRet) == U"mmm");
}

TEST_CASE("sys_libstdcxx facets: numpunct and moneypunct of the \"C\" locale", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    // numpunct: '.', no separator, no grouping, "false" and "true".
    const u32 np = e.facet("_ZNSt8numpunctIcE");
    CHECK(e.t.call("_ZNKSt8numpunctIcE16do_decimal_pointEv", np) == '.');
    CHECK(e.t.call("_ZNKSt8numpunctIcE16do_thousands_sepEv", np) == 0);
    e.t.call("_ZNKSt8numpunctIcE11do_groupingEv", kRet, np);
    CHECK(str_of<1>(b, kRet).empty());
    e.t.call("_ZNKSt8numpunctIcE12do_falsenameEv", kRet, np);
    CHECK(str_of<1>(b, kRet) == U"false");
    e.t.call("_ZNKSt8numpunctIcE11do_truenameEv", kRet, np);
    CHECK(str_of<1>(b, kRet) == U"true");
    const u32 nw = e.facet("_ZNSt8numpunctIwE");
    CHECK(e.t.call("_ZNKSt8numpunctIwE16do_decimal_pointEv", nw) == '.');
    e.t.call("_ZNKSt8numpunctIwE11do_truenameEv", kRet, nw);
    CHECK(str_of<4>(b, kRet) == U"true");
    e.t.call("_ZNKSt8numpunctIwE11do_groupingEv", kRet, nw);
    CHECK(str_of<1>(b, kRet).empty());
    // _Init again on the facet (its strings new again), then the destructors free them.
    StackVar info(e.t.ctx, b, 160);
    put(b, kStr, "C");
    e.t.call("_ZNSt8_LocinfoC1EPKc", info.addr(), kStr);
    e.t.call("_ZNSt8numpunctIcE5_InitERKSt8_Locinfo", np, info.addr());
    CHECK(cstr(b, ps_read32(b, np + 16)) == "false");
    e.t.call("_ZNSt8numpunctIcED1Ev", np);

    // moneypunct<char, false>: no symbol, decimal point or separator, signs "" and "-", no fraction digits, "$+xv".
    const u32 mp = e.facet("_ZNSt10moneypunctIcLb0EE");
    CHECK(e.t.call("_ZNKSt7_MpunctIcE16do_decimal_pointEv", mp) == 0);
    CHECK(e.t.call("_ZNKSt7_MpunctIcE16do_thousands_sepEv", mp) == 0);
    CHECK(e.t.call("_ZNKSt7_MpunctIcE14do_frac_digitsEv", mp) == 0);
    e.t.call("_ZNKSt7_MpunctIcE14do_curr_symbolEv", kRet, mp);
    CHECK(str_of<1>(b, kRet).empty());
    e.t.call("_ZNKSt7_MpunctIcE16do_positive_signEv", kRet, mp);
    CHECK(str_of<1>(b, kRet).empty());
    e.t.call("_ZNKSt7_MpunctIcE16do_negative_signEv", kRet, mp);
    CHECK(str_of<1>(b, kRet) == U"-");
    e.t.call("_ZNKSt7_MpunctIcE13do_pos_formatEv", kRet, mp);
    CHECK(std::string(reinterpret_cast<char*>(b + kRet), 4) == "$+xv");
    e.t.call("_ZNKSt7_MpunctIcE13do_neg_formatEv", kRet, mp);
    CHECK(std::string(reinterpret_cast<char*>(b + kRet), 4) == "$+xv");
    CHECK(b[mp + 32] == 0); // _International
    const u32 mi = e.facet("_ZNSt10moneypunctIwLb1EE");
    CHECK(b[mi + 36] == 1);
    e.t.call("_ZNKSt7_MpunctIwE16do_negative_signEv", kRet, mi);
    CHECK(str_of<4>(b, kRet) == U"-");
    // _Mpunct's base-object constructors: the vtable _Mpunct's, the refs and the international flag kept.
    e.t.call("_ZNSt7_MpunctIcEC2Ejb", kBuf, 7, 1);
    CHECK(type_name(b, kBuf) == "St7_MpunctIcE");
    CHECK(ps_read32(b, kBuf + 4) == 7);
    CHECK(b[kBuf + 32] == 1);
    e.t.call("_ZNSt7_MpunctIcEC2ERKSt8_Locinfojb", kBuf + 0x80, info.addr(), 0, 0);
    CHECK(cstr(b, ps_read32(b, kBuf + 0x80 + 24)) == "-");
    e.t.call("_ZNSt7_MpunctIcE5_InitERKSt8_Locinfo", kBuf + 0x80, info.addr());
    e.t.call("_ZNSt7_MpunctIcED2Ev", kBuf);
    e.t.call("_ZNSt7_MpunctIcED1Ev", kBuf + 0x80);
    e.t.call("_ZNSt8_LocinfoD1Ev", info.addr());
}

TEST_CASE("sys_libstdcxx facets: num_put formats as printf does under _Ifmt's and _Ffmt's formats",
    "[hle][libstdcxx]")
{
    Env e;
    const u32 np = e.facet("_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE");
    const auto put_long = [&](const std::string& kind, u64 v) {
        e.output<1>();
        e.t.call(kNumPutC + kind, kRet, np, kOit, kIos, '*', v);
        CHECK(ps_read32(e.b, kRet + 4) == kSb);
        CHECK(e.b[kRet] == 0);
        CHECK(e.width() == 0);
        std::string s;
        for (const char32_t c : e.written<1>()) s += static_cast<char>(c);
        return s;
    };
    // Integers: every base, showbase, showpos, uppercase, each width adjustment.
    const s32 values[] = {0, 7, -7, 255, -255, 2147483647, -2147483647 - 1};
    const u32 flag_sets[] = {kDec, kOct, kHex, kHex | kUppercase, kDec | kShowpos, kOct | kShowbase,
        kHex | kShowbase, kHex | kShowbase | kUppercase, kDec | kShowpos | kShowbase};
    const u32 adjusts[] = {0, kLeft, kRight, kInternal};
    for (const s32 v : values)
        for (const u32 fl : flag_sets)
            for (const u32 adj : adjusts)
                for (const s32 w : {0, 3, 14}) {
                    e.flags(fl | adj);
                    e.width(w);
                    INFO(std::format("{} flags {:#x} width {}", v, fl | adj, w));
                    CHECK(put_long("l", static_cast<u64>(static_cast<s64>(v))) ==
                          pad(int_printf(fl, v, true, 32), w, '*', fl | adj));
                    e.flags(fl | adj);
                    e.width(w);
                    const u32 u = static_cast<u32>(v);
                    CHECK(put_long("m", u) == pad(int_printf(fl, u, false, 32), w, '*', fl | adj));
                }
    for (const s64 v : {s64{0}, s64{-1}, s64{1} << 40, -(s64{1} << 62), s64{0x7FFFFFFFFFFFFFFF}}) {
        e.flags(kDec | kShowpos);
        CHECK(put_long("x", static_cast<u64>(v)) == int_printf(kDec | kShowpos, v, true, 64));
        e.flags(kHex | kShowbase);
        CHECK(put_long("y", static_cast<u64>(v)) == int_printf(kHex | kShowbase, v, false, 64));
    }
    // bool: as a long, or its name with boolalpha (padded, left or not).
    e.flags(kDec);
    CHECK(put_long("b", 1) == "1");
    e.flags(kDec | kBoolalpha);
    e.width(7);
    CHECK(put_long("b", 1) == "***true");
    e.flags(kDec | kBoolalpha | kLeft);
    e.width(7);
    CHECK(put_long("b", 0) == "false**");
    // A pointer: "0x" and its hex digits.
    e.flags(kDec | kInternal);
    e.width(12);
    CHECK(put_long("PKv", 0x1234ABCD) == "0x**1234abcd");

    // Floats: fixed, scientific, general, precision, showpoint, showpos, uppercase, padding.
    const auto put_double = [&](double v) {
        e.output<1>();
        e.callf(kNumPutC + "d", v, kRet, np, kOit, kIos, '_');
        std::string s;
        for (const char32_t c : e.written<1>()) s += static_cast<char>(c);
        return s;
    };
    const double dvalues[] = {0.0, -0.0, 1.0, -2.5, 3.14159265358979, 1e-7, 123456789.125, 6.02214076e23, -1e300,
        1.0 / 3.0};
    const u32 fflags[] = {0, kFixed, kScientific, kFixed | kShowpoint, kScientific | kUppercase, kShowpos,
        kShowpoint, kUppercase};
    for (const double v : dvalues)
        for (const u32 fl : fflags)
            for (const s32 prec : {0, 1, 6, 17})
                for (const s32 w : {0, 30}) {
                    if ((fl & kFixed) && std::abs(v) >= 1e35) continue; // scaled: below
                    e.flags(kDec | fl);
                    e.precision(prec);
                    e.width(w);
                    std::string f = "%";
                    if (fl & kShowpos) f += '+';
                    if (fl & kShowpoint) f += '#';
                    f += ".*";
                    const u32 ff = fl & (kFixed | kScientific);
                    f += ff == kFixed ? 'f' : ff == kScientific ? ((fl & kUppercase) ? 'E' : 'e') : ((fl & kUppercase) ? 'G' : 'g');
                    const s32 p = prec <= 0 && !(fl & kFixed) ? 6 : prec;
                    INFO(std::format("{} flags {:#x} precision {} width {}", v, fl, prec, w));
                    CHECK(put_double(v) == pad(printf_text(f.c_str(), p, v), w, '_', 0));
                }
    // More than 36 significant digits: the rest are zeros put after the digits (before an exponent).
    e.flags(kDec | kFixed);
    e.precision(40);
    CHECK(put_double(0.5) == printf_text("%.36f", 0.5) + "0000");
    e.flags(kDec | kScientific);
    e.precision(38);
    const std::string sci = printf_text("%.36e", 1.25);
    CHECK(put_double(1.25) == sci.substr(0, sci.find('e')) + "00" + sci.substr(sci.find('e')));
    // A fixed value from 1e35 up is scaled by 1e10 steps, the zeros put before its point.
    e.flags(kDec | kFixed);
    e.precision(1);
    const std::string big = printf_text("%.1f", 1e40 / 1e10);
    CHECK(put_double(1e40) == big.substr(0, big.find('.')) + "0000000000" + big.substr(big.find('.')));
    // Internal: the sign, then the fill.
    e.flags(kDec | kFixed | kInternal);
    e.precision(2);
    e.width(9);
    CHECK(put_double(-1.5) == "-____1.50");
    // long double is double on the PPU.
    e.output<1>();
    e.flags(kDec);
    e.precision(6);
    e.callf(kNumPutC + "e", 2.75, kRet, np, kOit, kIos, ' ');
    CHECK(e.written<1>() == U"2.75");
    // A failed stream buffer (no buffer: null) leaves the iterator failed.
    e.t.call(kNumPutC + "l", kRet, np, u64{0}, kIos, ' ', 5);
    CHECK(e.b[kRet] == 1);
}

TEST_CASE("sys_libstdcxx facets: num_put<wchar_t> and the exported _Iput and _Fput", "[hle][libstdcxx]")
{
    Env e;
    const u32 np = e.facet("_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE");
    e.output<4>();
    e.flags(kHex | kShowbase | kUppercase | kRight);
    e.width(8);
    e.t.call(kNumPutW + "l", kRet, np, kOit, kIos, '.', 0xBEEF);
    CHECK(e.written<4>() == U"..0XBEEF");
    e.output<4>();
    e.flags(kDec | kScientific);
    e.precision(3);
    e.callf(kNumPutW + "d", -1234.5, kRet, np, kOit, kIos, ' ');
    CHECK(e.written<4>() == U"-1.234e+03");
    e.output<4>();
    e.flags(kDec | kBoolalpha);
    e.t.call(kNumPutW + "b", kRet, np, kOit, kIos, ' ', 1);
    CHECK(e.written<4>() == U"true");
    // _Iput and _Fput on text the caller formatted.
    const u32 npc = e.facet("_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE");
    e.output<1>();
    e.flags(kDec | kInternal);
    e.width(6);
    put(e.b, kBuf, "-42");
    e.t.call("_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE5_IputES3_RSt8ios_basecPcj", kRet, npc,
        kOit, kIos, '0', kBuf, 3);
    CHECK(e.written<1>() == U"-00042");
    e.output<1>();
    e.flags(kDec);
    put(e.b, kBuf, "1.5e+07");
    e.t.call("_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE5_FputES3_RSt8ios_basecPKcjjjj", kRet, npc,
        kOit, kIos, ' ', kBuf, 0, 0, 3, 7);
    CHECK(e.written<1>() == U"1.5000e+07");
}

TEST_CASE("sys_libstdcxx facets: num_put and num_get with a locale's grouping", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    // A numpunct<char> grouping by three with ',' in a locale of its own (an empty() one: the rest from the global).
    const u32 punct = e.facet("_ZNSt8numpunctIcE");
    put(b, kStr, "\3");
    ps_write32(b, punct + 8, kStr);
    b[punct + 13] = ',';
    e.t.call("_ZNSt6locale5emptyEv", kLoc);
    e.t.call("_ZNSt6locale7_AddfacEPNS_5facetEjj", kLoc, punct, e.facet_id("St8numpunctIcE"), 0);
    ps_write32(b, kIos + 36, kLoc);
    const u32 np = e.facet("_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE");
    e.output<1>();
    e.flags(kDec);
    e.t.call(kNumPutC + "l", kRet, np, kOit, kIos, ' ', static_cast<u64>(static_cast<s64>(-1234567)));
    CHECK(e.written<1>() == U"-1,234,567");
    e.output<1>();
    e.flags(kDec | kFixed);
    e.precision(1);
    e.callf(kNumPutC + "d", 1234567.5, kRet, np, kOit, kIos, ' ');
    CHECK(e.written<1>() == U"1,234,567.5");
    e.output<1>();
    e.flags(kDec);
    e.t.call(kNumPutC + "l", kRet, np, kOit, kIos, ' ', 999);
    CHECK(e.written<1>() == U"999");

    const u32 ng = e.facet("_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE");
    const auto get_long = [&](std::string_view text, s32& out) {
        e.input<1>(text32(text));
        ps_write32(b, kState, 0);
        ps_write32(b, kVal, 0xDEAD);
        e.t.call(kNumGetC + "l", kRet, ng, kIitC, u64{0}, kIos, kState, kVal);
        out = static_cast<s32>(ps_read32(b, kVal));
        return ps_read32(b, kState);
    };
    s32 v = 0;
    CHECK(get_long("1,234,567", v) == 1);
    CHECK(v == 1234567);
    CHECK(get_long("12,34", v) == 3); // a group of two after the first: failbit (and the end)
    CHECK(v == 0xDEAD);
    CHECK(get_long("1,234,", v) == 3); // a trailing separator
    CHECK(get_long("1234", v) == 1);   // no separators at all is fine
    CHECK(v == 1234);
    CHECK(get_long("12,345 x", v) == 0);
    CHECK(v == 12345);
    CHECK(e.consumed<1>() == 6);
}

TEST_CASE("sys_libstdcxx facets: num_get gathers fields as _Getifld and _Getffld and converts them",
    "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    const u32 ng = e.facet("_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE");
    struct Got {
        u32 state;
        u64 value;
        u32 consumed;
        u32 it_sb;
    };
    const auto get = [&](const std::string& kind, std::string_view text, u32 fl, u32 bytes) {
        e.input<1>(text32(text));
        e.flags(fl);
        ps_write32(b, kState, 0);
        ps_write64(b, kVal, 0xDDDDDDDDDDDDDDDDull);
        e.t.call(kNumGetC + kind, kRet, ng, kIitC, u64{0}, kIos, kState, kVal);
        const u64 raw = ps_read64(b, kVal);
        const u64 value = bytes == 8 ? raw : bytes == 4 ? raw >> 32 : bytes == 2 ? raw >> 48 : raw >> 56;
        return Got{ps_read32(b, kState), value, e.consumed<1>(), ps_read32(b, kRet)};
    };
    constexpr u32 kEofb = 1, kFailb = 2;
    // long: signs, bases, leading zeros, the end.
    Got g = get("l", "123", kDec, 4);
    CHECK(g.state == kEofb);
    CHECK(g.value == 123);
    CHECK(g.it_sb == 0); // at the end: the end-of-stream iterator
    g = get("l", "-42 rest", kDec, 4);
    CHECK(g.state == 0);
    CHECK(static_cast<s32>(g.value) == -42);
    CHECK(g.consumed == 3);
    CHECK(g.it_sb == kSb);
    CHECK(get("l", "+0007", kDec, 4).value == 7);
    CHECK(get("l", "ff", kHex, 4).value == 0xFF);
    CHECK(get("l", "0x1F", kHex, 4).value == 0x1F);
    CHECK(get("l", "0x1F", 0, 4).value == 0x1F); // basefield 0: the prefix decides
    CHECK(get("l", "017", 0, 4).value == 15);
    CHECK(get("l", "19", 0, 4).value == 19);
    CHECK(get("l", "78", kOct, 4).value == 7); // '8' ends an octal field
    g = get("l", "x1", kDec, 4);
    CHECK(g.state == kFailb);
    CHECK(g.value == 0xDDDDDDDD); // left alone
    CHECK(g.consumed == 0);
    g = get("l", "2147483648", kDec, 4); // out of range: failbit, nothing stored
    CHECK(g.state == (kEofb | kFailb));
    CHECK(g.value == 0xDDDDDDDD);
    CHECK(get("l", "-2147483648", kDec, 4).value == 0x80000000u);
    g = get("l", "", kDec, 4);
    CHECK(g.state == (kEofb | kFailb));
    CHECK(get("l", "000000000000000000000000000000000000000012", kDec, 4).value == 12);
    // unsigned: a '-' negates; ushort range.
    CHECK(get("m", "4294967295", kDec, 4).value == 0xFFFFFFFFu);
    CHECK(get("j", "-1", kDec, 4).value == 0xFFFFFFFFu);
    CHECK(get("t", "65535", kDec, 2).value == 0xFFFF);
    CHECK(get("t", "-1", kDec, 2).value == 0xFFFF);
    CHECK(get("t", "65536", kDec, 2).state == (kEofb | kFailb));
    CHECK(get("x", "-9223372036854775808", kDec, 8).value == 0x8000000000000000ull);
    CHECK(get("y", "18446744073709551615", kDec, 8).value == ~0ull);
    CHECK(get("y", "18446744073709551616", kDec, 8).state == (kEofb | kFailb));
    // bool: 0 or 1, else failbit; boolalpha: the names, the longest match.
    CHECK(get("b", "1", kDec, 1).value == 1);
    CHECK(get("b", "0", kDec, 1).value == 0);
    CHECK(get("b", "2", kDec, 1).state == (kEofb | kFailb));
    g = get("b", "true!", kDec | kBoolalpha, 1);
    CHECK(g.value == 1);
    CHECK(g.state == 0);
    CHECK(g.consumed == 4);
    CHECK(get("b", "false", kDec | kBoolalpha, 1).value == 0);
    CHECK(get("b", "tru", kDec | kBoolalpha, 1).state == (kEofb | kFailb));
    CHECK(get("b", "fax", kDec | kBoolalpha, 1).state == kFailb);
    // void *: hex.
    CHECK(get("Pv", "0x10203040", kDec, 4).value == 0x10203040);
    CHECK(get("Pv", "abc", kDec, 4).value == 0xABC);
    // Floats: the field's significant digits, an exponent, the C library's conversion.
    const auto dbl = [&](std::string_view text) { return std::bit_cast<double>(get("d", text, kDec, 8).value); };
    CHECK(dbl("3.25") == 3.25);
    CHECK(dbl("-0.000125") == -0.000125);
    CHECK(dbl("1e10") == 1e10);
    CHECK(dbl("1.5E-3x") == 1.5e-3);
    CHECK(dbl(".5") == 0.5);
    CHECK(dbl("12345678901234567890123456789012345678901234567890") == 12345678901234567890123456789012345678901234567890.0);
    CHECK(dbl("0.000000000000000000000000000000000000000000000000000000001") == 1e-57);
    g = get("d", "1e400", kDec, 8);
    CHECK((g.state & kFailb) != 0); // out of range
    g = get("d", "-", kDec, 8);
    CHECK(g.state == (kEofb | kFailb));
    g = get("d", "1e", kDec, 8); // an exponent with no digits fails the field
    CHECK((g.state & kFailb) != 0);
    CHECK(std::bit_cast<float>(static_cast<u32>(get("f", "0.1", kDec, 4).value)) == 0.1f);
    CHECK(std::bit_cast<double>(get("e", "2.5e2", kDec, 8).value) == 250.0);
    // The exported _Getifld and _Getffld: the field, its base or power of ten, the iterators moved.
    e.input<1>(U"-0x00ab;");
    ps_write64(b, kStr, kIitC);
    ps_write64(b, kStr + 8, 0);
    const char* getifld =
        "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8_GetifldEPcRS3_S6_NSt5_IosbIiE9_FmtflagsERKSt6locale";
    CHECK(e.t.call(getifld, ng, kBuf, kStr, kStr + 8, 0, ps_read32(b, kIos + 36)) == 16);
    CHECK(cstr(b, kBuf) == "-ab");
    CHECK(ps_read32(b, kStr) == kSb);
    CHECK(e.consumed<1>() == 7);
    e.input<1>(U"000.00012e+0005");
    ps_write64(b, kStr, kIitC);
    const char* getffld = "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8_GetffldEPcRS3_S6_RKSt6locale";
    CHECK(i32(e.t.call(getffld, ng, kBuf, kStr, kStr + 8, ps_read32(b, kIos + 36))) == -2);
    CHECK(cstr(b, kBuf) == "0.012e+05"); // 0.012e5 x 10^-2: 12
    CHECK(ps_read32(b, kStr) == 0); // at the end
}

TEST_CASE("sys_libstdcxx facets: num_get<wchar_t> takes its iterators in two doublewords", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    const u32 ng = e.facet("_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE");
    e.input<4>(U"-9876 \x663");
    e.flags(kDec);
    ps_write32(b, kState, 0);
    e.t.call(kNumGetW + "l", kRet, ng, kIitC, u64{0}, u64{0}, u64{0}, kIos, kState, kVal);
    CHECK(static_cast<s32>(ps_read32(b, kVal)) == -9876);
    CHECK(ps_read32(b, kState) == 0);
    CHECK(ps_read32(b, kRet) == kSb);
    CHECK(e.consumed<4>() == 5);
    // A wide digit past ASCII is no digit.
    e.input<4>(U"\x663");
    e.t.call(kNumGetW + "l", kRet, ng, kIitC, u64{0}, u64{0}, u64{0}, kIos, kState, kVal);
    CHECK(ps_read32(b, kState) == 2);
    e.input<4>(U"6.5");
    ps_write32(b, kState, 0);
    e.t.call(kNumGetW + "d", kRet, ng, kIitC, u64{0}, u64{0}, u64{0}, kIos, kState, kVal);
    CHECK(std::bit_cast<double>(ps_read64(b, kVal)) == 6.5);
    CHECK(ps_read32(b, kState) == 1);
    // The iterator given back: at the end, the end of stream, _Got set.
    CHECK(ps_read32(b, kRet) == 0);
    CHECK(b[kRet + 4] == 1);
}

TEST_CASE("sys_libstdcxx facets: money_put and money_get in the \"C\" locale", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    const u32 mp = e.facet("_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE");
    const char* put_ld = "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_bRSt8ios_basece";
    const auto money = [&](double v, u32 fl, s32 w, bool intl = false) {
        e.output<1>();
        e.flags(kDec | fl);
        e.width(w);
        e.callf(put_ld, v, kRet, mp, kOit, intl ? 1 : 0, kIos, '*');
        std::string s;
        for (const char32_t c : e.written<1>()) s += static_cast<char>(c);
        return s;
    };
    CHECK(money(1234, 0, 0) == "1234");
    CHECK(money(-1234, 0, 0) == "-1234");
    CHECK(money(-1234, 0, 8) == "***-1234");
    CHECK(money(-1234, kLeft, 8) == "-1234***");
    CHECK(money(-1234, kInternal, 8) == "-***1234"); // the fill where the pattern has none
    CHECK(money(0.4, kShowbase, 0) == "0");          // "%.0Lf" of the value; no symbol
    CHECK(money(99.5, 0, 0, true) == "100");
    CHECK(e.width() == 0);
    // From digits: a leading '-', the digits up to anything else, "0" for none.
    const char* put_str = "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_bRSt8ios_basecRKSs";
    const auto money_str = [&](std::string_view digits) {
        e.output<1>();
        e.flags(kDec);
        put(b, kBuf, digits);
        e.t.call("_ZNSsC1EPKc", kStr, kBuf);
        e.t.call(put_str, kRet, mp, kOit, 0, kIos, ' ', kStr);
        std::string s;
        for (const char32_t c : e.written<1>()) s += static_cast<char>(c);
        return s;
    };
    CHECK(money_str("-00120") == "-00120");
    CHECK(money_str("12x34") == "12");
    CHECK(money_str("-") == "-0");
    // _Putmfld itself, its string by reference to the caller's copy.
    e.output<1>();
    put(b, kBuf, "77");
    e.t.call("_ZNSsC1EPKc", kStr, kBuf);
    e.t.call("_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE8_PutmfldES3_bRSt8ios_basecbSs", kRet, mp,
        kOit, 0, kIos, ' ', 1, kStr);
    CHECK(e.written<1>() == U"-77");

    // money_get: neg_format's "$+xv": an optional sign, white space, the digits.
    const u32 mg = e.facet("_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE");
    const char* get_str =
        "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_bRSt8ios_baseRNSt5_IosbIiE8_IostateERSs";
    const auto get_digits = [&](std::string_view text) {
        e.input<1>(text32(text));
        e.flags(kDec);
        ps_write32(b, kState, 0);
        e.t.call("_ZNSsC1Ev", kStr2);
        e.t.call(get_str, kRet, mg, kIitC, u64{0}, 0, kIos, kState, kStr2);
        return std::pair{str_of<1>(b, kStr2), ps_read32(b, kState)};
    };
    CHECK(get_digits("1234") == std::pair{std::u32string(U"1234"), 1u});
    CHECK(get_digits("-  56;") == std::pair{std::u32string(U"-56"), 0u});
    CHECK(e.consumed<1>() == 5);
    CHECK(get_digits("x").second == 2);
    CHECK(get_digits("-").second == 3);
    const char* get_ld =
        "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE6do_getES3_S3_bRSt8ios_baseRNSt5_IosbIiE8_IostateERe";
    e.input<1>(U"-250");
    ps_write32(b, kState, 0);
    e.t.call(get_ld, kRet, mg, kIitC, u64{0}, 0, kIos, kState, kVal);
    CHECK(std::bit_cast<double>(ps_read64(b, kVal)) == -250.0);
    // _Getmfld: the field as a string, the iterators moved.
    e.input<1>(U"-9 ");
    ps_write64(b, kBuf, kIitC);
    ps_write64(b, kBuf + 8, 0);
    e.t.call("_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE8_GetmfldERS3_S5_bRSt8ios_base", kRet, mg,
        kBuf, kBuf + 8, 0, kIos);
    CHECK(str_of<1>(b, kRet) == U"-9");
    CHECK(e.consumed<1>() == 2);
    // wchar_t: money_put of a long double, money_get into a wide string.
    const u32 mpw = e.facet("_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE");
    e.output<4>();
    e.flags(kDec);
    e.callf("_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_bRSt8ios_basewe", -3.0, kRet,
        mpw, kOit, 0, kIos, ' ');
    CHECK(e.written<4>() == U"-3");
    const u32 mgw = e.facet("_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE");
    e.input<4>(U"42");
    ps_write32(b, kState, 0);
    e.t.call("_ZNSbIwSt11char_traitsIwESaIwEEC1Ev", kStr2);
    e.t.call("_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE6do_getES3_S3_bRSt8ios_baseRNSt5_IosbIiE8_"
             "IostateERSbIwS2_SaIwEE",
        kRet, mgw, kIitC, u64{0}, u64{0}, u64{0}, 0, kIos, kState, kStr2);
    CHECK(str_of<4>(b, kStr2) == U"42");
    CHECK(ps_read32(b, kState) == 1);
}

TEST_CASE("sys_libstdcxx facets: time_get and time_put", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    const u32 tg = e.facet("_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE");
    const std::string pre = "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE";
    const std::string args = "ES3_S3_RSt8ios_baseRNSt5_IosbIiE8_IostateEPSt2tm";
    CHECK(e.t.call(pre + "13do_date_orderEv", tg) == 2); // mdy, the "C" locale's %m/%d/%y
    const auto tm_get = [&](const std::string& what, std::string_view text) {
        e.input<1>(text32(text));
        ps_write32(b, kState, 0);
        std::memset(b + kTm, 0, 36);
        e.t.call(pre + what + args, kRet, tg, kIitC, u64{0}, kIos, kState, kTm);
        return ps_read32(b, kState);
    };
    const auto tm = [&](u32 field) { return static_cast<s32>(ps_read32(b, kTm + 4 * field)); };
    CHECK(tm_get("11do_get_time", "12:34:56") == 1);
    CHECK(tm(2) == 12);
    CHECK(tm(1) == 34);
    CHECK(tm(0) == 56);
    CHECK(tm_get("11do_get_time", "24:00:00") == 2); // the hour out of range
    CHECK(tm_get("11do_get_time", "7-30") == 2);
    CHECK(tm_get("11do_get_date", "10/09/26") == 1);
    CHECK(tm(4) == 9);    // October
    CHECK(tm(3) == 9);    // the 9th
    CHECK(tm(5) == 126);  // 26: 2026
    CHECK(tm_get("11do_get_date", "Feb 3, 1999") == 1);
    CHECK(tm(4) == 1);
    CHECK(tm(3) == 3);
    CHECK(tm(5) == 99);
    CHECK(tm_get("11do_get_date", "12/31") == 3); // the year is missing
    CHECK(tm_get("14do_get_weekday", "Saturday") == 1);
    CHECK(tm(6) == 6);
    CHECK(tm_get("14do_get_weekday", "Tue,") == 0);
    CHECK(tm(6) == 2);
    CHECK(e.consumed<1>() == 3);
    CHECK(tm_get("14do_get_weekday", "Sundx") == 2); // past "Sun" toward "Sunday", then neither
    CHECK(tm_get("16do_get_monthname", "September") == 1);
    CHECK(tm(4) == 8);
    CHECK(tm_get("16do_get_monthname", "May") == 1);
    CHECK(tm(4) == 4);
    CHECK(tm_get("11do_get_year", "2026") == 1);
    CHECK(tm(5) == 126);
    CHECK(tm_get("11do_get_year", "68") == 1);
    CHECK(tm(5) == 168);
    CHECK(tm_get("11do_get_year", "69") == 1);
    CHECK(tm(5) == 69);
    CHECK(tm_get("11do_get_year", "10000") == 3);
    // _Getint and _Getloctxt as exported.
    e.input<1>(U"-0042x");
    ps_write64(b, kBuf, kIitC);
    ps_write64(b, kBuf + 8, 0);
    ps_write32(b, kVal, 0);
    CHECK(e.t.call(pre + "7_GetintERS3_S5_iiRi", tg, kBuf, kBuf + 8, -100, 100, kVal) == 0);
    CHECK(static_cast<s32>(ps_read32(b, kVal)) == -42);
    CHECK(e.consumed<1>() == 5);
    e.input<1>(U"noon");
    ps_write64(b, kBuf, kIitC);
    put(b, kStr, "|no|noon|nine");
    CHECK(e.t.call("_ZSt10_GetloctxtIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEiRT0_S5_jPKT_", kBuf, kBuf + 8, 0,
              kStr) == 1);
    // wchar_t: its iterators in two doublewords, the tm pointer on the stack.
    const u32 tgw = e.facet("_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE");
    e.input<4>(U"Wednesday");
    ps_write32(b, kState, 0);
    e.t.call("_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE14do_get_weekday" + args, kRet, tgw, kIitC,
        u64{0}, u64{0}, u64{0}, kIos, kState, kTm);
    CHECK(tm(6) == 3);
    CHECK(ps_read32(b, kState) == 1);

    // time_put: one conversion of strftime, with or without a modifier.
    const u32 tp = e.facet("_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE");
    const char* put_c = "_ZNKSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE6do_putES3_RSt8ios_basecPKSt2tmcc";
    std::memset(b + kTm, 0, 36);
    const s32 fields[] = {5, 4, 3, 9, 9, 126, 5, 281, 0};
    for (u32 i = 0; i < 9; i++) ps_write32(b, kTm + 4 * i, static_cast<u32>(fields[i]));
    const auto tput = [&](char spec, char mod) {
        e.output<1>();
        e.t.call(put_c, kRet, tp, kOit, kIos, ' ', kTm, spec, mod);
        std::string s;
        for (const char32_t c : e.written<1>()) s += static_cast<char>(c);
        return s;
    };
    CHECK(tput('Y', 0) == "2026");
    CHECK(tput('A', 0) == "Friday");
    CHECK(tput('x', 0) == "10/09/26");
    CHECK(tput('X', 'E') == "03:04:05");
    CHECK(tput('Z', 0).empty()); // no zone names: nothing, but no failure
    CHECK(tput('j', 0) == "282");
    const u32 tpw = e.facet("_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE");
    e.output<4>();
    e.t.call("_ZNKSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE6do_putES3_RSt8ios_basewPKSt2tmcc", kRet,
        tpw, kOit, kIos, ' ', kTm, 'B', 0);
    CHECK(e.written<4>() == U"October");
}

TEST_CASE("sys_libstdcxx facets: _Maklocstr, _Maklocchr and _Maklocbyte for wchar_t", "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    put(b, kStr, "w\xFC!");
    const u32 p = static_cast<u32>(e.t.call("_ZSt10_MaklocstrIwEPT_PKcS1_RKSt7_Cvtvec", kStr, 0, kBuf));
    REQUIRE(p != 0);
    CHECK(ps_read32(b, p) == 'w');
    CHECK(ps_read32(b, p + 4) == 0xFC);
    CHECK(ps_read32(b, p + 12) == 0);
    e.t.call("_ZdaPv", p);
    CHECK(e.t.call("_ZSt10_MaklocchrIwET_cPS0_RKSt7_Cvtvec", 0xA0, 0, kBuf) == 0xA0);
    CHECK(e.t.call("_ZSt11_MaklocbyteIwEcT_RKSt7_Cvtvec", 'Q', kBuf) == 'Q');
    CHECK(e.t.call("_ZSt11_MaklocbyteIwEcT_RKSt7_Cvtvec", 0x4E2D, kBuf) == 0);
}

TEST_CASE("sys_libstdcxx facets: the streams use them: basic_ios::init's fill, a classic copy's facets",
    "[hle][libstdcxx]")
{
    Env e;
    u8* b = e.b;
    // basic_ios<wchar_t>::init widens ' ' through ctype<wchar_t> made by _Getcat.
    std::memset(b + kBuf, 0, 64);
    e.t.call("_ZNSt9basic_iosIwSt11char_traitsIwEE4initEPSt15basic_streambufIwS1_Eb", kBuf, 0, 0);
    CHECK(ps_read32(b, kBuf + 48) == ' ');
    // locale("C"): every standard facet, each made by its type's _Getcat.
    put(b, kStr, "C");
    e.t.call("_ZNSt6localeC1EPKci", kLoc, kStr, 63);
    const u32 imp = ps_read32(b, kLoc);
    u32 facets = 0;
    for (u32 i = 0; i < ps_read32(b, imp + 12); i++)
        if (ps_read32(b, ps_read32(b, imp + 8) + 4 * i)) facets++;
    CHECK(facets == 26);
    CHECK(type_name(b, static_cast<u32>(e.t.call("_ZSt9use_facetISt8numpunctIwEERKT_RKSt6locale", kLoc))) ==
          "St8numpunctIwE");
    CHECK(type_name(b, static_cast<u32>(e.t.call("_ZSt9use_facetISt10moneypunctIcLb1EEERKT_RKSt6locale", kLoc))) ==
          "St10moneypunctIcLb1EE");
    e.t.call("_ZNSt6localeD1Ev", kLoc);
}
