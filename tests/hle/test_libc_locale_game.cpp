// sys_libc's locale and time functions compared with a game's statically linked copy of the SDK's C library (D-906):
// see game_executable.hpp. The tests run with test_hle "[gamelibc]" and skip without the game's executable and map.
// Each compares answers and the memory they write, call for call, over made-up inputs; the game's library keeps its
// own state (its _Times, _Locale, _Clocale, its thread-local tables) and pspocket's its own, both starting as the
// SDK's "C" locale. The game's wchar_t is 16 bits, sys_libc's 32: wide answers are compared as values.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "game_executable.hpp"

using namespace pspocket;
using pspocket::test::GameExecutable;

namespace {

// Differences found, the first ones kept for the report.
struct Diffs {
    u64 calls = 0, count = 0;
    std::vector<std::string> first;
    void add(std::string what)
    {
        if (first.size() < 40) first.push_back(std::move(what));
        count++;
    }
    void report(const char* what) const
    {
        std::printf("%s: %llu calls compared, %llu differences\n", what, static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(count));
        for (const std::string& s : first) std::printf("  %s\n", s.c_str());
    }
};

std::string hex(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

std::string show(const std::string& s)
{
    std::string r;
    for (const char c : s) {
        if (c >= 0x20 && c < 0x7F) r += c;
        else r += std::format("\\x{:02x}", static_cast<u8>(c));
    }
    return r;
}

// A small deterministic generator.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed) {}
    u32 next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
    s32 range(s32 lo, s32 hi) { return lo + static_cast<s32>(next() % static_cast<u32>(hi - lo + 1)); }
    template <class T, usize N>
    T pick(const T (&a)[N])
    {
        return a[next() % N];
    }
};

using TmFields = std::array<s32, 9>; // sec min hour mday mon year wday yday isdst

void put_tm(u8* b, u32 t, const TmFields& f)
{
    for (u32 i = 0; i < 9; i++) ps_write32(b, t + 4 * i, static_cast<u32>(f[i]));
}

TmFields get_tm(const u8* b, u32 t)
{
    TmFields f{};
    for (u32 i = 0; i < 9; i++) f[i] = static_cast<s32>(ps_read32(const_cast<u8*>(b), t + 4 * i));
    return f;
}

std::string tm_text(const TmFields& f)
{
    return std::format("{{{} {} {} {} {} {} {} {} {}}}", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
}

// Consistent dates (Gregorian, years from 1900) around the year's ends and the leap days, and odd ones.
std::vector<TmFields> dates()
{
    std::vector<TmFields> v;
    const auto civil = [](s64 days, s32& y, s32& m, s32& d) {
        days += 719468;
        const s64 era = (days >= 0 ? days : days - 146096) / 146097;
        const u32 doe = static_cast<u32>(days - era * 146097);
        const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const u32 mp = (5 * doy + 2) / 153;
        d = static_cast<s32>(doy - (153 * mp + 2) / 5 + 1);
        m = static_cast<s32>(mp < 10 ? mp + 3 : mp - 9);
        y = static_cast<s32>(static_cast<s64>(yoe) + era * 400 + (m <= 2));
    };
    const auto from_civil = [](s64 y, u32 m, u32 d) {
        y -= m <= 2;
        const s64 era = (y >= 0 ? y : y - 399) / 400;
        const u32 yoe = static_cast<u32>(y - era * 400);
        const u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        return era * 146097 + static_cast<s64>(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    };
    const auto add = [&](s64 days, s32 sec) {
        s32 y, m, d;
        civil(days, y, m, d);
        const s64 jan1 = from_civil(y, 1, 1);
        const s32 wday = static_cast<s32>(((days % 7) + 11) % 7);
        v.push_back({sec % 60, (sec / 60) % 60, (sec / 3600) % 24, d, m - 1, y - 1900, wday,
            static_cast<s32>(days - jan1), 0});
    };
    for (const s32 year : {1899, 1900, 1901, 1969, 1970, 1999, 2000, 2003, 2004, 2008, 2009, 2010, 2015, 2016, 2020,
             2021, 2026, 2027, 2037, 2038, 2099, 2100, 2101, 2400}) {
        const s64 jan1 = from_civil(year, 1, 1);
        for (s64 d = -9; d < 9; d++) add(jan1 + d, static_cast<s32>((d + 20) * 3607 % 86400));
        add(from_civil(year, 2, 28), 43200);
        add(from_civil(year, 3, 1), 3599);
        add(from_civil(year, 6, 15), 86399);
    }
    Rng r(7);
    for (u32 i = 0; i < 120; i++) add(r.range(-60000, 120000), r.range(0, 86399));
    // Fields out of their ranges.
    const s32 odd[][9] = {
        {-1, -1, -1, 0, -1, -1, -1, -1, -1}, {60, 60, 24, 32, 12, 8100, 7, 366, 1}, {61, 99, 99, 99, 13, 9999, 8, 400, 2},
        {0, 0, 0, 1, 0, -1900, 0, 0, 0}, {5, 5, 12, 5, 5, -1905, 3, 150, 0}, {0, 0, 11, 1, 0, 100, 0, 0, 0},
        {0, 0, 12, 1, 0, 100, 0, 0, 0}, {0, 0, -12, 1, 0, 0, 0, 0, 0}, {0, 0, -11, 1, 0, 0, 0, 0, 0},
        {0, 0, 0, 1, 0, 300, -7, 0, 0}, {0, 0, 0, 1, 0, 10000, 6, 364, 0}, {0, 0, 0, 1, 0, 200, 6, 365, 0},
        {0, 0, 0, 1, 0, 2147483647, 3, 3, 0}, {0, 0, 0, 1, 0, -2147483647 - 1, 3, 3, 0},
    };
    for (const auto& o : odd) v.push_back({o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8]});
    for (u32 i = 0; i < 60; i++)
        v.push_back({r.range(-3, 70), r.range(-3, 70), r.range(-30, 30), r.range(-5, 40), r.range(-3, 14),
            r.range(-2000, 9000), r.range(-3, 9), r.range(-3, 400), r.range(-2, 2)});
    return v;
}

// The conversions strftime has (and some it has not).
constexpr std::string_view kCodes = "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%qQkls#&+@0";

// A _Tinfo in guest memory with every field given (the era, alt_digits and time_zone fields exercised).
u32 make_tinfo(GameExecutable& g, const std::array<std::string, 21>& f)
{
    const u32 t = g.alloc(84);
    for (u32 i = 0; i < 21; i++) ps_write32(g.b, t + 4 * i, g.cstring(f[i]));
    return t;
}

std::array<std::string, 21> c_tinfo_fields()
{
    const std::string days = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
    const std::string months = ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:"
                               "Sep:September:Oct:October:Nov:November:Dec:December";
    const std::string fmts = "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p";
    return {":AM:PM", days, days, days, months, months, months, fmts, fmts, fmts, fmts, fmts, fmts, fmts, fmts, fmts,
        fmts, "", "", "", ""};
}

} // namespace

TEST_CASE("sys_libc's strftime writes what the game's strftime writes", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    const u32 tm = g.alloc(64), out_g = g.alloc(512), out_o = g.alloc(512), fmt = g.alloc(512);
    const auto both = [&](const std::string& f, u32 max, const TmFields& t) {
        std::memcpy(g.b + fmt, f.c_str(), f.size() + 1);
        put_tm(g.b, tm, t);
        std::memset(g.b + out_g, 0xAA, 300);
        std::memset(g.b + out_o, 0xAA, 300);
        const u32 ng = static_cast<u32>(g.call_game("strftime", {out_g, max, fmt, tm}));
        const u32 no = static_cast<u32>(g.call_own("sys_libc", "strftime", {out_o, max, fmt, tm}));
        d.calls++;
        if (ng != no || std::memcmp(g.b + out_g, g.b + out_o, 300) != 0)
            d.add(std::format("strftime(\"{}\", {}) of {}: game {} \"{}\", sys_libc {} \"{}\"", show(f), max, tm_text(t),
                ng, hex(g.b + out_g, std::min<u32>(ng + 2, 48)), no, hex(g.b + out_o, std::min<u32>(no + 2, 48))));
    };
    const std::vector<TmFields> all = dates();
    for (const TmFields& t : all) {
        for (const char c : kCodes) {
            for (const char* q : {"", "E", "O"}) both(std::string("%") + q + c + "|", 200, t);
        }
        both("%a %A %b %B %c %C %d %D %e %F %g %G %h %H %I %j %m %M %n %p %r %R %S %t %T %u %U %V %w %W %x %X %y %Y "
             "%% %Ey %OH %EC %EY %Oe %Ex %EX %Ec",
            400, t);
    }
    // Literal text, multibyte characters and bytes that are none, a '%' at the end, nested expansions. (A format
    // whose first character is no multibyte character is left out: the SDK's then takes the character its stack
    // held before, whatever the call before left there.)
    const TmFields t0 = all[30];
    for (const std::string f : {"", "plain", "%", "x%", "\xC3\xA9t\xC3\xA9 %d", "\xE2\x82\xAC%Y\xE2\x82\xAC", "%d\x80x",
             "a\xC3\xA9\x80%d", "%Y\xC3%d", "%\xC3\xA9", "%H\xF0\x9F\x98\x80%H", "%c%c%c%c", "%r%x%X%D%F%R%T", "%%%%%%",
             "%E%", "%O%", "%Ez", "%EZ", "%Oz", "%%d"})
        both(f, 300, t0);
    // Buffers too small, and empty.
    for (const std::string f : {"%c", "%Y-%m-%d %H:%M:%S", "abc%q", "%n%t", "%%"})
        for (u32 max = 0; max < 30; max++) both(f, max, t0);
    d.report("strftime");
    CHECK(d.count == 0);
}

TEST_CASE("sys_libc's _CStrftime and _Gentime with a locale's eras, digits and zones are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    std::vector<u32> tinfos;
    const u32 strings_lo = g.heap;
    tinfos.push_back(make_tinfo(g, c_tinfo_fields()));
    {
        auto f = c_tinfo_fields();
        f[0] = "/am/pm";
        f[1] = "-di-Dimanche-lu-Lundi-ma-Mardi-me-Mercredi-je-Jeudi-ve-Vendredi-sa-Samedi";
        f[2] = ":S:Sun";
        f[3] = ":";
        f[17] = ";+:1:1989/01/08:+*:Heisei:%EC%Ey\xE5\xB9\xB4;+:1:1926/12/25:1989/01/07:Showa:%EC%Ey;"
                "-:1:-0001/12/31:-*:BC:%EC %Ey";
        f[18] = ";zero;one;two;three;four;five;six;seven;eight;nine;ten;eleven;twelve";
        f[19] = "";
        f[20] = ":EST:EDT:-0500";
        f[8] = "|%A %d %B %Y|%Ex";
        f[13] = "|%EC%Ey|%EY %Ex";
        f[14] = ":%Ey/%Om/%Od";
        tinfos.push_back(make_tinfo(g, f));
        f[17] = ";-:0:2000/01/01:1990/01/01:Down:%Ey!;+:5:2010/06/15:2012/02/29:Mid:%Y-%m;x:bad;+:7:2030/1:+*:Bad;"
                "+:2:2040/01/01:+*:Late:%Y%Ey";
        f[20] = ":CET:CEST:+0100:(2010)0331+0:1031-0";
        f[18] = "";
        tinfos.push_back(make_tinfo(g, f));
        f[20] = "";
        f[17] = ":+:3:1900/01/01";
        f[7] = "";
        f[8] = "%c";
        f[11] = "%r";
        tinfos.push_back(make_tinfo(g, f));
    }
    const u32 strings_hi = g.heap;
    const u32 tm = g.alloc(64), out_g = g.alloc(512), out_o = g.alloc(512), fmt = g.alloc(512), buf_g = g.alloc(64),
              buf_o = g.alloc(64), len_g = g.alloc(8), len_o = g.alloc(8);
    const std::vector<TmFields> all = dates();
    for (const u32 tin : tinfos) {
        for (const TmFields& t : all) {
            put_tm(g.b, tm, t);
            // _Gentime: the text (at the pointer, |length| bytes) and the length.
            for (u32 code = 0; code < 256; code++) {
                for (const u32 qual : {0u, u32{'E'}, u32{'O'}}) {
                    if (code != 0 && code < 0x20 && (code & 7)) continue;
                    if (code > 0x7F && code != 0x80 && code != 0xFF) continue;
                    std::memset(g.b + buf_g, 0x55, 64);
                    std::memset(g.b + buf_o, 0x55, 64);
                    const u32 pg = static_cast<u32>(g.call_game("_Gentime", {tm, tin, qual, code, len_g, buf_g}));
                    const u32 po = static_cast<u32>(g.call_own("sys_libc", "_Gentime", {tm, tin, qual, code, len_o, buf_o}));
                    const s32 lg = static_cast<s32>(ps_read32(g.b, len_g)), lo = static_cast<s32>(ps_read32(g.b, len_o));
                    d.calls++;
                    const u32 n = static_cast<u32>(lg < 0 ? -lg : lg);
                    const bool in_buf_g = pg >= buf_g && pg < buf_g + 64, in_buf_o = po >= buf_o && po < buf_o + 64;
                    const bool in_strings = pg >= strings_lo && pg < strings_hi;
                    const bool same_place = in_buf_g ? (in_buf_o && pg - buf_g == po - buf_o)
                                                     : (!in_buf_o && (!in_strings || pg == po));
                    if (lg != lo || n > 400 || !same_place || std::memcmp(g.b + pg, g.b + po, n) != 0 ||
                        (in_buf_g && std::memcmp(g.b + buf_g, g.b + buf_o, 64) != 0))
                        d.add(std::format("_Gentime({:#x} {:#x}) tin {} of {}: game {} \"{}\", sys_libc {} \"{}\"", qual,
                            code, tin, tm_text(t), lg, show(g.bytes(pg, std::min<u32>(n, 60))), lo,
                            show(g.bytes(po, std::min<u32>(static_cast<u32>(lo < 0 ? -lo : lo), 60)))));
                }
            }
            // _CStrftime over formats with every conversion.
            for (const std::string& f : {std::string("%c|%x|%X|%r|%Ec|%Ex|%EX|%Er"),
                     std::string("%EC %Ey %EY %Oy %Od %Oe %OH %OI %Om %OM %OS %Ou %Ow %OU %OV %OW %Oj"),
                     std::string("%Z %z %EZ %Ez %p %a %A %b %B"), std::string("%Ec%Ec%Ec%Ec%EY%EY")}) {
                std::memcpy(g.b + fmt, f.c_str(), f.size() + 1);
                for (const u32 max : {400u, 12u}) {
                    std::memset(g.b + out_g, 0xAA, 420);
                    std::memset(g.b + out_o, 0xAA, 420);
                    const u32 ng = static_cast<u32>(g.call_game("_CStrftime", {out_g, max, fmt, tm, tin}));
                    const u32 no = static_cast<u32>(g.call_own("sys_libc", "_CStrftime", {out_o, max, fmt, tm, tin}));
                    d.calls++;
                    if (ng != no || std::memcmp(g.b + out_g, g.b + out_o, 420) != 0)
                        d.add(std::format("_CStrftime(\"{}\", {}) tin {} of {}: game {} \"{}\", sys_libc {} \"{}\"", f, max,
                            tin, tm_text(t), ng, show(g.cstr(out_g).substr(0, 120)), no, show(g.cstr(out_o).substr(0, 120))));
                }
            }
        }
    }
    d.report("_Gentime and _CStrftime");
    CHECK(d.count == 0);
}

TEST_CASE("sys_libc's time conversions are the game's: gmtime, localtime, asctime, ctime, _Ttotm, _Daysto, _Gettime",
    "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    const u32 tp = g.alloc(16), tm = g.alloc(64), tm2 = g.alloc(64);
    std::vector<s64> times = {0, -1, 1, 59, 60, 3599, 3600, 86399, 86400, -86400, -86401, 951782400, 951868800,
        978307199, 1234567890, 2147483647, -2147483647 - 1, 4102444799, 4102444800, 4107542400, -2208988800,
        -2208988801, -2208988800 - 86400 * 365, 32503680000, -62135596800, 253402300799, 1ll << 40, -(1ll << 40),
        1ll << 50, 1760000000, 1774000000};
    Rng r(99);
    for (u32 i = 0; i < 400; i++)
        times.push_back(static_cast<s64>(static_cast<s32>(r.next())) * (i % 3 == 0 ? 1 : (i % 3 == 1 ? 8 : 64)));
    for (const s64 t : times) {
        ps_write64(g.b, tp, static_cast<u64>(t));
        for (const char* fn : {"gmtime", "localtime"}) {
            const u32 pg = static_cast<u32>(g.call_game(fn, {tp}));
            const TmFields fg = get_tm(g.b, pg);
            const u32 po = static_cast<u32>(g.call_own("sys_libc", fn, {tp}));
            const TmFields fo = get_tm(g.b, po);
            d.calls++;
            if (fg != fo) d.add(std::format("{}({}): game {}, sys_libc {}", fn, t, tm_text(fg), tm_text(fo)));
            // asctime of it.
            std::memcpy(g.b + tm, g.b + pg, 36);
            const std::string ag = g.cstr(static_cast<u32>(g.call_game("asctime", {tm})));
            const std::string ao = g.cstr(static_cast<u32>(g.call_own("sys_libc", "asctime", {tm})));
            d.calls++;
            if (ag != ao) d.add(std::format("asctime{}: game \"{}\", sys_libc \"{}\"", tm_text(fg), show(ag), show(ao)));
        }
        const std::string cg = g.cstr(static_cast<u32>(g.call_game("ctime", {tp})));
        const std::string co = g.cstr(static_cast<u32>(g.call_own("sys_libc", "ctime", {tp})));
        d.calls++;
        if (cg != co) d.add(std::format("ctime({}): game \"{}\", sys_libc \"{}\"", t, show(cg), show(co)));
        for (const s32 isdst : {0, 1, -1, 7}) {
            std::memset(g.b + tm, 0x77, 40);
            std::memset(g.b + tm2, 0x77, 40);
            g.call_game("_Ttotm", {tm, static_cast<u64>(t), static_cast<u64>(static_cast<s64>(isdst))});
            g.call_own("sys_libc", "_Ttotm", {tm2, static_cast<u64>(t), static_cast<u64>(static_cast<s64>(isdst))});
            d.calls++;
            if (std::memcmp(g.b + tm, g.b + tm2, 40) != 0)
                d.add(std::format("_Ttotm({}, {}): game {}, sys_libc {}", t, isdst, tm_text(get_tm(g.b, tm)),
                    tm_text(get_tm(g.b, tm2))));
        }
    }
    // asctime of odd fields.
    for (const TmFields& f : dates()) {
        put_tm(g.b, tm, f);
        const std::string ag = g.cstr(static_cast<u32>(g.call_game("asctime", {tm})));
        const std::string ao = g.cstr(static_cast<u32>(g.call_own("sys_libc", "asctime", {tm})));
        d.calls++;
        if (ag != ao) d.add(std::format("asctime{}: game \"{}\", sys_libc \"{}\"", tm_text(f), show(ag), show(ao)));
    }
    for (s32 year = -500; year <= 500; year++)
        for (s32 mon = 0; mon < 12; mon++) {
            const s32 a = static_cast<s32>(g.call_game("_Daysto", {static_cast<u64>(year), static_cast<u64>(mon)}));
            const s32 b = static_cast<s32>(g.call_own("sys_libc", "_Daysto", {static_cast<u64>(year), static_cast<u64>(mon)}));
            d.calls++;
            if (a != b) d.add(std::format("_Daysto({}, {}): game {}, sys_libc {}", year, mon, a, b));
        }
    const u32 len_g = g.alloc(8), len_o = g.alloc(8);
    for (const std::string s : {"", ":", "::", ":a", ":a:", ":AM:PM", "|x|yy|zzz|", "abc", ";one;;three", "-", "--x"}) {
        const u32 str = g.cstring(s);
        for (s32 n = -2; n < 7; n++) {
            const u32 pg = static_cast<u32>(g.call_game("_Gettime", {str, static_cast<u64>(n), len_g}));
            const u32 po = static_cast<u32>(g.call_own("sys_libc", "_Gettime", {str, static_cast<u64>(n), len_o}));
            d.calls++;
            if (pg != po || ps_read32(g.b, len_g) != ps_read32(g.b, len_o))
                d.add(std::format("_Gettime(\"{}\", {}): game +{} {}, sys_libc +{} {}", s, n, pg - str,
                    ps_read32(g.b, len_g), po - str, ps_read32(g.b, len_o)));
        }
    }
    // time: the same clock.
    const s64 tg = static_cast<s64>(g.call_game("time", {tp}));
    const s64 to = static_cast<s64>(g.call_own("sys_libc", "time", {tp}));
    d.calls++;
    if (std::llabs(tg - to) > 2) d.add(std::format("time: game {}, sys_libc {}", tg, to));
    d.report("time conversions");
    CHECK(d.count == 0);
}

TEST_CASE("sys_libc's locale objects and setlocale are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    // The game's _Once takes a lightweight mutex its start-up code makes (the executable is never started): made
    // here, so _Getloc's and _Getcloc's once-only work runs as in the game. (Its _Getloc registers an exit function
    // the first time a name is not found; pspocket's has nothing to free then and registers none.)
    if (g.has("_Once_mutex_init")) g.call_game("_Once_mutex_init", {});
    // The variables, compared by content (pointers by the text they point at).
    const auto var_g = [&](const char* n) { return g.code.at(n); };
    const auto var_o = [&](const char* n) { return g.own_variable("sys_libc", n); };
    const auto strp = [&](u32 p) { return p ? g.cstr(p) : std::string("(null)"); };
    {
        const u32 a = var_g("_Times"), b = var_o("_Times");
        for (u32 i = 0; i < 21; i++) {
            d.calls++;
            if (strp(ps_read32(g.b, a + 4 * i)) != strp(ps_read32(g.b, b + 4 * i)))
                d.add(std::format("_Times[{}]: game \"{}\", sys_libc \"{}\"", i, strp(ps_read32(g.b, a + 4 * i)),
                    strp(ps_read32(g.b, b + 4 * i))));
        }
        d.calls++;
        if (g.call_own("sys_libc", "_Getptimes", {}) != b || g.call_game("_Getptimes", {}) != a)
            d.add("_Getptimes is not &_Times");
    }
    {
        const u32 a = var_g("_Locale"), b = var_o("_Locale");
        for (u32 off = 0; off < 80; off += 4) {
            d.calls++;
            const bool chars = off >= 28 && off < 44;
            if (chars ? ps_read32(g.b, a + off) != ps_read32(g.b, b + off)
                      : strp(ps_read32(g.b, a + off)) != strp(ps_read32(g.b, b + off)))
                d.add(std::format("_Locale+{}: game {:08x} \"{}\", sys_libc {:08x} \"{}\"", off, ps_read32(g.b, a + off),
                    chars ? "" : strp(ps_read32(g.b, a + off)), ps_read32(g.b, b + off),
                    chars ? "" : strp(ps_read32(g.b, b + off))));
        }
        d.calls++;
        if (g.call_game("localeconv", {}) != a || g.call_own("sys_libc", "localeconv", {}) != b) d.add("localeconv");
    }
    {
        const u32 a = var_g("_Loctab"), b = var_o("_Loctab");
        for (u32 i = 0; i <= 64; i++) {
            const u32 ea = a + 12 * i, eb = b + 12 * i;
            d.calls++;
            if (strp(ps_read32(g.b, ea)) != strp(ps_read32(g.b, eb)) || ps_read32(g.b, ea + 4) != ps_read32(g.b, eb + 4) ||
                ps_read32(g.b, ea + 8) != ps_read32(g.b, eb + 8))
                d.add(std::format("_Loctab[{}]: game {} {:#x} {}, sys_libc {} {:#x} {}", i, strp(ps_read32(g.b, ea)),
                    ps_read32(g.b, ea + 4), ps_read32(g.b, ea + 8), strp(ps_read32(g.b, eb)), ps_read32(g.b, eb + 4),
                    ps_read32(g.b, eb + 8)));
        }
    }
    {
        const u32 a = var_g("_Clocale"), b = var_o("_Clocale");
        d.calls++;
        if (strp(ps_read32(g.b, a)) != strp(ps_read32(g.b, b))) d.add("_Clocale's name");
        for (u32 off = 4; off < 0x1C4; off += 4) {
            d.calls++;
            if (ps_read32(g.b, a + off) != ps_read32(g.b, b + off))
                d.add(std::format("_Clocale+{:#x} before _Getcloc: game {:#x}, sys_libc {:#x}", off, ps_read32(g.b, a + off),
                    ps_read32(g.b, b + off)));
        }
    }
    // The ctype tables (257 entries from EOF).
    for (const char* n : {"_Ctype", "_Tolotab", "_Touptab"}) {
        const u32 a = ps_read32(g.b, var_g(n)) - 2, b = ps_read32(g.b, var_o(n)) - 2;
        d.calls++;
        if (std::memcmp(g.b + a, g.b + b, 2 * 257) != 0)
            for (u32 i = 0; i < 257; i++)
                if (ps_read16(g.b, a + 2 * i) != ps_read16(g.b, b + 2 * i))
                    d.add(std::format("{}[{}]: game {:#x}, sys_libc {:#x}", n, static_cast<s32>(i) - 1,
                        ps_read16(g.b, a + 2 * i), ps_read16(g.b, b + 2 * i)));
    }
    // setlocale: names and categories (the game's knows "C" alone).
    const u32 cg = ps_read32(g.b, var_g("_Clocale")), co = ps_read32(g.b, var_o("_Clocale"));
    const std::vector<std::string> names = {"C", "", "POSIX", "c", "C ", " C", "fr_FR", "C;collate:C", "collate:C",
        "C;ctype:X", "X;collate:C", "C:", ";C", "CC", "\xC3\xA9"};
    for (const s32 cat : {0, 1, 2, 3, 4, 5, 6, 7, 63, -1, 1000}) {
        for (s32 k = -1; k < static_cast<s32>(names.size()); k++) {
            const u32 n = k < 0 ? 0 : g.cstring(names[static_cast<usize>(k)]);
            const u32 rg = static_cast<u32>(g.call_game("setlocale", {static_cast<u64>(cat), n}));
            const u32 ro = static_cast<u32>(g.call_own("sys_libc", "setlocale", {static_cast<u64>(cat), n}));
            d.calls++;
            if ((rg == 0) != (ro == 0) || (rg && (rg != cg || ro != co || g.cstr(rg) != g.cstr(ro))))
                d.add(std::format("setlocale({}, {}): game {:#x}, sys_libc {:#x}", cat, k < 0 ? "NULL" : names[static_cast<usize>(k)], rg, ro));
        }
    }
    d.calls++;
    if (g.call_game("_Defloc", {}) != cg || g.call_own("sys_libc", "_Defloc", {}) != co) d.add("_Defloc is not \"C\"");
    // _Getloc: the category's locale of a (composite) name: the "C" locale or none.
    const u32 lcg = var_g("_Clocale"), lco = var_o("_Clocale");
    const std::vector<std::string> lnames = {"C", "", "X", "C;", ";C", "C;collate:X", "X;collate:C", "collate:C",
        "ctype:C", "collate:C;ctype:C", "C;ctype:X;collate:C", "X;ctype:C", "collate:", "collate:;C", ":C", "C:",
        "time:C;C", "messages:C", "numeric:X;C", "C;;", "CX", "monetary:C;time:X"};
    for (const char* cat : {"", "collate:", "ctype:", "monetary:", "numeric:", "time:", "messages:", "bogus:", "collate"}) {
        const u32 c = g.cstring(cat);
        for (const std::string& ln : lnames) {
            const u32 n = g.cstring(ln);
            const u32 rg = static_cast<u32>(g.call_game("_Getloc", {c, n}));
            const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Getloc", {c, n}));
            d.calls++;
            const s32 ag = rg == 0 ? 0 : rg == lcg ? 1 : 2, ao = ro == 0 ? 0 : ro == lco ? 1 : 2;
            if (ag != ao) d.add(std::format("_Getloc(\"{}\", \"{}\"): game {}, sys_libc {}", cat, ln, ag, ao));
        }
    }
    // _Getnloc over category masks: the answers and the categories set.
    const u32 pg = g.alloc(28), po = g.alloc(28);
    for (const s32 mask : {0, 1, 2, 4, 8, 16, 32, 63, 62, 3, 64, 127, -1}) {
        for (const std::string& ln : lnames) {
            for (const bool pre : {false, true}) {
                for (u32 i = 0; i < 7; i++) {
                    ps_write32(g.b, pg + 4 * i, pre && i % 2 ? lcg : 0);
                    ps_write32(g.b, po + 4 * i, pre && i % 2 ? lco : 0);
                }
                const u32 n = g.cstring(ln);
                const s32 rg = static_cast<s32>(g.call_game("_Getnloc", {pg, static_cast<u64>(mask), n}));
                const s32 ro = static_cast<s32>(g.call_own("sys_libc", "_Getnloc", {po, static_cast<u64>(mask), n}));
                d.calls++;
                bool same = rg == ro;
                for (u32 i = 0; i < 7; i++) {
                    const u32 a = ps_read32(g.b, pg + 4 * i), b = ps_read32(g.b, po + 4 * i);
                    if ((a == 0) != (b == 0) || (a && (a == lcg) != (b == lco))) same = false;
                }
                if (!same) d.add(std::format("_Getnloc({}, \"{}\"): game {}, sys_libc {}", mask, ln, rg, ro));
            }
        }
    }
    // (_Getlname always frees or allocates the name buffer through the game's own allocator, which the unstarted
    // executable has not set up: it is pinned by the firmware-free tests.)
    // _Skip and _Findloc.
    for (const std::string s : {"", "a", "a b", "a \t b", "ab  c", " ", "  x", "\t\t", "x\t"}) {
        const u32 str = g.cstring(s);
        const u32 rg = static_cast<u32>(g.call_game("_Skip", {str}));
        const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Skip", {str}));
        d.calls++;
        if (rg != ro) d.add(std::format("_Skip(\"{}\"): game +{}, sys_libc +{}", s, rg - str, ro - str));
    }
    for (const std::string s : {"C", "X", ""}) {
        const u32 str = g.cstring(s);
        d.calls++;
        if (g.call_game("_Findloc", {str, s.size()}) != 0 || g.call_own("sys_libc", "_Findloc", {str, s.size()}) != 0)
            d.add("_Findloc found a locale");
    }
    // The thread's global locale pointer.
    for (const u32 v : {0x12345678u, 0u, 0xDEADBEEFu}) {
        g.call_game("_Setgloballocale", {v});
        g.call_own("sys_libc", "_Setgloballocale", {v});
        d.calls++;
        if (g.call_game("_Getgloballocale", {}) != v || g.call_own("sys_libc", "_Getgloballocale", {}) != v)
            d.add(std::format("_Getgloballocale after {:#x}", v));
    }
    // _Getcloc: the "C" locale filled from the current tables, the same way.
    const u32 rg = static_cast<u32>(g.call_game("_Getcloc", {}));
    const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Getcloc", {}));
    d.calls++;
    if (rg != lcg || ro != lco) d.add("_Getcloc is not _Clocale");
    for (u32 off = 0; off < 0x1C4; off += 4) {
        const u32 a = ps_read32(g.b, lcg + off), b = ps_read32(g.b, lco + off);
        d.calls++;
        bool same = a == b;
        if (off == 0) same = g.cstr(a) == g.cstr(b);
        else if (off == 0x88 || off == 0x8C || off == 0x90) same = std::memcmp(g.b + a - 2, g.b + b - 2, 514) == 0;
        else if (off == 0x94) same = (a >> 24) == (b >> 24);
        else if (off == 0x11C) same = a && b; // the wide class tables, compared below
        else if (off >= 0x120 && off < 0x170 && !(off >= 0x13C && off < 0x14C)) same = strp(a) == strp(b);
        else if (off >= 0x170) same = strp(a) == strp(b);
        if (!same) d.add(std::format("_Clocale+{:#x} after _Getcloc: game {:#x}, sys_libc {:#x}", off, a, b));
    }
    d.report("locale objects and setlocale");
    CHECK(d.count == 0);
}

TEST_CASE("sys_libc's multibyte conversions and wide classes are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    const u32 errno_g = static_cast<u32>(g.call_game("_Geterrno", {}));
    const u32 errno_o = static_cast<u32>(g.call_own("sys_libc", "_Geterrno", {}));
    const u32 st_g = g.alloc(16), st_o = g.alloc(16), wc_g = g.alloc(8), wc_o = g.alloc(8), src = g.alloc(64),
              out_g = g.alloc(16), out_o = g.alloc(16);
    d.calls++;
    if (g.call_game("_Getmbcurmax", {}) != g.call_own("sys_libc", "_Getmbcurmax", {})) d.add("MB_CUR_MAX");
    // _Mbtowc over byte strings, lengths and states (fresh, and left by the call before).
    std::vector<std::vector<u8>> inputs = {{0}, {'A', 0}, {0x7F}, {0xC3, 0xA9}, {0xE2, 0x82, 0xAC}, {0x80}, {0xBF},
        {0xC0, 0x80}, {0xC1, 0xBF}, {0xDF, 0xBF}, {0xE0, 0x80, 0x80}, {0xEF, 0xBF, 0xBF}, {0xED, 0xA0, 0x80},
        {0xF0, 0x9F, 0x98, 0x80}, {0xF8, 0x80}, {0xFF}, {0xFE}, {0xC3, 0x41}, {0xE2, 0x82, 0x41}, {0xC3, 0x00},
        {0xE2, 0x00}, {0xC3, 0xC3, 0xA9}};
    Rng r(5);
    for (u32 i = 0; i < 300; i++) {
        std::vector<u8> v(r.range(1, 4));
        for (u8& c : v) c = static_cast<u8>(r.pick<u32, 8>({0x41u, 0x80u, 0xA9u, 0xC3u, 0xE2u, 0x82u, 0xACu, 0xF0u}));
        inputs.push_back(v);
    }
    std::memset(g.b + st_g, 0, 8);
    std::memset(g.b + st_o, 0, 8);
    for (const auto& in : inputs) {
        for (u32 n = 0; n <= in.size() + 1; n++) {
            for (const bool fresh : {true, false}) {
                std::memset(g.b + src, 0, 64);
                std::memcpy(g.b + src, in.data(), in.size());
                if (fresh) {
                    std::memset(g.b + st_g, 0, 8);
                    std::memset(g.b + st_o, 0, 8);
                }
                ps_write32(g.b, errno_g, 0);
                ps_write32(g.b, errno_o, 0);
                ps_write32(g.b, wc_g, 0xAAAAAAAA);
                ps_write32(g.b, wc_o, 0xAAAAAAAA);
                const u32 rg = static_cast<u32>(g.call_game("_Mbtowc", {wc_g, src, n, st_g}));
                const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Mbtowc", {wc_o, src, n, st_o}));
                d.calls++;
                const u32 wg = ps_read16(g.b, wc_g) == 0xAAAA ? 0xAAAAAAAAu : ps_read16(g.b, wc_g);
                const u32 wo = ps_read32(g.b, wc_o);
                if (rg != ro || wg != wo || std::memcmp(g.b + st_g, g.b + st_o, 8) != 0 ||
                    ps_read32(g.b, errno_g) != ps_read32(g.b, errno_o))
                    d.add(std::format("_Mbtowc({}, {}, {}): game {:#x} wc {:#x} state {} errno {:#x}, sys_libc {:#x} wc {:#x} "
                                      "state {} errno {:#x}",
                        hex(in.data(), static_cast<u32>(in.size())), n, fresh ? "fresh" : "kept", rg, wg,
                        hex(g.b + st_g, 8), ps_read32(g.b, errno_g), ro, wo, hex(g.b + st_o, 8), ps_read32(g.b, errno_o)));
            }
        }
    }
    // s NULL resets the state and answers 0 (no shift states).
    ps_write32(g.b, st_g, 0x12345678);
    ps_write32(g.b, st_o, 0x12345678);
    d.calls++;
    if (g.call_game("_Mbtowc", {wc_g, 0, 1, st_g}) != g.call_own("sys_libc", "_Mbtowc", {wc_o, 0, 1, st_o}) ||
        std::memcmp(g.b + st_g, g.b + st_o, 8) != 0)
        d.add("_Mbtowc(NULL)");
    // _Wctomb over every wide character of the SDK's.
    for (u32 wc = 0; wc <= 0xFFFF; wc++) {
        std::memset(g.b + out_g, 0xEE, 8);
        std::memset(g.b + out_o, 0xEE, 8);
        std::memset(g.b + st_g, 0, 8);
        std::memset(g.b + st_o, 0, 8);
        const u32 rg = static_cast<u32>(g.call_game("_Wctomb", {out_g, wc, st_g}));
        const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Wctomb", {out_o, wc, st_o}));
        d.calls++;
        if (rg != ro || std::memcmp(g.b + out_g, g.b + out_o, 8) != 0)
            d.add(std::format("_Wctomb({:#x}): game {:#x} {}, sys_libc {:#x} {}", wc, rg, hex(g.b + out_g, 4), ro,
                hex(g.b + out_o, 4)));
    }
    // The state-table machine: tables in guest memory given to _Mbtowcx and _Wctombx (a shift encoding: SO 0x0E to
    // state 1, where bytes pair up into wide characters; SI 0x0F back).
    const u32 tabs = g.alloc(64), t0 = g.alloc(512), t1 = g.alloc(512), t2 = g.alloc(512), wtabs = g.alloc(64),
              w0 = g.alloc(512);
    for (u32 c = 0; c < 256; c++) {
        ps_write16(g.b, t0 + 2 * c, static_cast<u16>(0x8000 | 0x4000 | 0x2000 | c)); // fold, input, output, state 0
        ps_write16(g.b, t1 + 2 * c, static_cast<u16>(0x8000 | 0x4000 | 0x1000 | 0x0200 | c)); // fold+rotate to 2
        ps_write16(g.b, t2 + 2 * c, static_cast<u16>(0x8000 | 0x4000 | 0x2000 | 0x0100 | c)); // fold, out, back to 1
        ps_write16(g.b, w0 + 2 * c, static_cast<u16>(c < 0x80 ? (0x4000 | 0x2000 | c) : (0x2000 | 0x0300 | 0x3F)));
    }
    ps_write16(g.b, t0 + 2 * 0x0E, 0x4100); // SO: take it, to state 1
    ps_write16(g.b, t1 + 2 * 0x0F, 0x4000); // SI: take it, to state 0
    ps_write16(g.b, t0 + 2 * 0xFF, 0);      // no character
    std::memset(g.b + tabs, 0, 64);
    ps_write32(g.b, tabs, t0 + 0);
    ps_write32(g.b, tabs + 4, t1);
    ps_write32(g.b, tabs + 8, t2);
    std::memset(g.b + wtabs, 0, 64);
    ps_write32(g.b, wtabs, w0);
    std::vector<std::vector<u8>> tin = {{'A'}, {0x0E, 0x12, 0x34}, {0x0E}, {0x0E, 0x12}, {0x0E, 0x0F, 'B'}, {0xFF},
        {0}, {0x0E, 0}, {0x0E, 0x12, 0x34, 0x56, 0x78}};
    for (const auto& in : tin) {
        for (u32 n = 0; n <= in.size() + 1; n++) {
            for (const u32 st0 : {0u, 1u, 2u, 3u, 15u, 16u}) {
                std::memset(g.b + src, 0, 64);
                std::memcpy(g.b + src, in.data(), in.size());
                std::memset(g.b + st_g, 0, 8);
                std::memset(g.b + st_o, 0, 8);
                ps_write16(g.b, st_g + 6, static_cast<u16>(st0));
                ps_write16(g.b, st_o + 6, static_cast<u16>(st0));
                ps_write32(g.b, wc_g, 0xAAAAAAAA);
                ps_write32(g.b, wc_o, 0xAAAAAAAA);
                ps_write32(g.b, errno_g, 0);
                ps_write32(g.b, errno_o, 0);
                const u32 rg = static_cast<u32>(g.call_game("_Mbtowcx", {wc_g, src, n, st_g, tabs}));
                const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Mbtowcx", {wc_o, src, n, st_o, tabs}));
                d.calls++;
                const u32 wg = ps_read16(g.b, wc_g) == 0xAAAA ? 0xAAAAAAAAu : ps_read16(g.b, wc_g);
                if (rg != ro || wg != ps_read32(g.b, wc_o) || std::memcmp(g.b + st_g, g.b + st_o, 8) != 0 ||
                    ps_read32(g.b, errno_g) != ps_read32(g.b, errno_o))
                    d.add(std::format("_Mbtowcx({}, {}, state {}): game {:#x} wc {:#x} {}, sys_libc {:#x} wc {:#x} {}",
                        hex(in.data(), static_cast<u32>(in.size())), n, st0, rg, wg, hex(g.b + st_g, 8), ro,
                        ps_read32(g.b, wc_o), hex(g.b + st_o, 8)));
            }
        }
    }
    d.calls++;
    if (g.call_game("_Mbtowcx", {wc_g, 0, 1, st_g, tabs}) != g.call_own("sys_libc", "_Mbtowcx", {wc_o, 0, 1, st_o, tabs}))
        d.add("_Mbtowcx(NULL) with tables");
    for (u32 wc = 0; wc < 0x200; wc += 3) {
        for (const u32 st0 : {0u, 1u, 3u}) {
            std::memset(g.b + out_g, 0xEE, 8);
            std::memset(g.b + out_o, 0xEE, 8);
            std::memset(g.b + st_g, 0, 8);
            std::memset(g.b + st_o, 0, 8);
            ps_write16(g.b, st_g + 6, static_cast<u16>(st0));
            ps_write16(g.b, st_o + 6, static_cast<u16>(st0));
            const u32 rg = static_cast<u32>(g.call_game("_Wctombx", {out_g, wc, st_g, tabs, wtabs}));
            const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Wctombx", {out_o, wc, st_o, tabs, wtabs}));
            d.calls++;
            if (rg != ro || std::memcmp(g.b + out_g, g.b + out_o, 8) != 0 || std::memcmp(g.b + st_g, g.b + st_o, 8) != 0)
                d.add(std::format("_Wctombx({:#x}, state {}): game {:#x} {} {}, sys_libc {:#x} {} {}", wc, st0, rg,
                    hex(g.b + out_g, 4), hex(g.b + st_g, 8), ro, hex(g.b + out_o, 4), hex(g.b + st_o, 8)));
        }
    }
    // The wide classes: every descriptor over the first characters, and the table's layout.
    for (u32 desc = 0; desc <= 12; desc++) {
        for (u32 wc = 0; wc < 0x180; wc++) {
            const u32 rg = static_cast<u32>(g.call_game("_Iswctype", {wc, desc}));
            const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_Iswctype", {wc, desc}));
            d.calls++;
            if (rg != ro) d.add(std::format("_Iswctype({:#x}, {}): game {}, sys_libc {}", wc, desc, rg, ro));
        }
        const u32 wc = 0xFFFF;
        d.calls++;
        if (g.call_game("_Iswctype", {wc, desc}) != g.call_own("sys_libc", "_Iswctype", {wc, desc})) d.add("_Iswctype(WEOF)");
    }
    {
        const u32 a = static_cast<u32>(g.call_game("_Getpwctytab", {}));
        const u32 b = static_cast<u32>(g.call_own("sys_libc", "_Getpwctytab", {}));
        for (u32 k = 1; k <= 13; k++) {
            const u32 na = ps_read32(g.b, a + 8 * k), nb = ps_read32(g.b, b + 8 * k);
            d.calls++;
            if ((na ? g.cstr(na) : "") != (nb ? g.cstr(nb) : "") || ps_read32(g.b, a + 8 * k + 4) != ps_read32(g.b, b + 8 * k + 4))
                d.add(std::format("_Wctype[{}] differs", k));
        }
        d.calls++;
        if (std::memcmp(g.b + ps_read32(g.b, a), g.b + ps_read32(g.b, b), 2 * 66) != 0) d.add("_Wctype's ranges differ");
    }
    d.report("multibyte conversions and wide classes");
    CHECK(d.count == 0);
}

TEST_CASE("sys_libc's collation in the C locale is the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    const std::vector<std::string> strs = {"", "a", "b", "ab", "abc", "abd", "B", "\xE9", "\x7F", "a\xFF", "zz", "a b"};
    const u32 out_g = g.alloc(64), out_o = g.alloc(64);
    for (const std::string& x : strs) {
        const u32 a = g.cstring(x);
        for (const std::string& y : strs) {
            const u32 b = g.cstring(y);
            const s32 rg = static_cast<s32>(g.call_game("strcoll", {a, b}));
            const s32 ro = static_cast<s32>(g.call_own("sys_libc", "strcoll", {a, b}));
            d.calls++;
            if ((rg < 0) != (ro < 0) || (rg > 0) != (ro > 0) || rg != ro)
                d.add(std::format("strcoll(\"{}\", \"{}\"): game {}, sys_libc {}", show(x), show(y), rg, ro));
        }
        for (const u32 n : {0u, 1u, 2u, 3u, 4u, 40u}) {
            std::memset(g.b + out_g, 0xCC, 64);
            std::memset(g.b + out_o, 0xCC, 64);
            const u32 rg = static_cast<u32>(g.call_game("strxfrm", {out_g, a, n}));
            const u32 ro = static_cast<u32>(g.call_own("sys_libc", "strxfrm", {out_o, a, n}));
            d.calls++;
            if (rg != ro || std::memcmp(g.b + out_g, g.b + out_o, 64) != 0)
                d.add(std::format("strxfrm(\"{}\", {}): game {} {}, sys_libc {} {}", show(x), n, rg, hex(g.b + out_g, 8), ro,
                    hex(g.b + out_o, 8)));
        }
    }
    // _CStrxfrm piece by piece from a save area (its first word the position), and _CWcsxfrm (the game's wide
    // characters 16 bits, sys_libc's 32: counts, characters and positions compared as characters).
    const u32 cs_g = g.alloc(32), cs_o = g.alloc(32), costate = g.alloc(64), wps_g = g.alloc(8), wps_o = g.alloc(8),
              wsrc_g = g.alloc(64), wsrc_o = g.alloc(128), wout_g = g.alloc(64), wout_o = g.alloc(128), mbs = g.alloc(16);
    for (const std::string& x : strs) {
        const u32 a = g.cstring(x);
        for (const u32 size : {0u, 1u, 2u, 5u, 32u}) {
            std::memset(g.b + cs_g, 0, 32);
            std::memset(g.b + cs_o, 0, 32);
            ps_write32(g.b, cs_g, a);
            ps_write32(g.b, cs_o, a);
            for (u32 round = 0; round < 4; round++) {
                std::memset(g.b + out_g, 0xCC, 64);
                std::memset(g.b + out_o, 0xCC, 64);
                const u32 rg = static_cast<u32>(g.call_game("_CStrxfrm", {out_g, size, cs_g, costate}));
                const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_CStrxfrm", {out_o, size, cs_o, costate}));
                d.calls++;
                if (rg != ro || std::memcmp(g.b + out_g, g.b + out_o, 64) != 0 ||
                    std::memcmp(g.b + cs_g, g.b + cs_o, 8) != 0)
                    d.add(std::format("_CStrxfrm(\"{}\", {}) round {}: game {} at +{}, sys_libc {} at +{}", show(x), size,
                        round, rg, ps_read32(g.b, cs_g) - a, ro, ps_read32(g.b, cs_o) - a));
            }
        }
        std::memset(g.b + wsrc_g, 0, 64);
        std::memset(g.b + wsrc_o, 0, 128);
        for (u32 i = 0; i < x.size(); i++) {
            ps_write16(g.b, wsrc_g + 2 * i, static_cast<u8>(x[i]));
            ps_write32(g.b, wsrc_o + 4 * i, static_cast<u8>(x[i]));
        }
        for (const u32 size : {0u, 1u, 2u, 5u, 32u}) {
            ps_write32(g.b, wps_g, wsrc_g);
            ps_write32(g.b, wps_o, wsrc_o);
            std::memset(g.b + wout_g, 0xCC, 64);
            std::memset(g.b + wout_o, 0xCC, 128);
            std::memset(g.b + mbs, 0, 16);
            const u32 rg = static_cast<u32>(g.call_game("_CWcsxfrm", {wout_g, wps_g, size, mbs, costate}));
            const u32 ro = static_cast<u32>(g.call_own("sys_libc", "_CWcsxfrm", {wout_o, wps_o, size, mbs + 8, costate}));
            bool same = rg == ro && (ps_read32(g.b, wps_g) - wsrc_g) / 2 == (ps_read32(g.b, wps_o) - wsrc_o) / 4;
            for (u32 i = 0; i < 16; i++) {
                const u32 a16 = ps_read16(g.b, wout_g + 2 * i), a32 = ps_read32(g.b, wout_o + 4 * i);
                if ((a16 == 0xCCCC) != (a32 == 0xCCCCCCCC) || (a16 != 0xCCCC && a16 != a32)) same = false;
            }
            d.calls++;
            if (!same) d.add(std::format("_CWcsxfrm(\"{}\", {}): game {}, sys_libc {}", show(x), size, rg, ro));
        }
    }
    d.report("collation");
    CHECK(d.count == 0);
}

