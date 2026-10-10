// sys_libc's locale and time functions as the SDK's C library has them (D-906), pinned without a game: strftime and its
// internals, the calendar, setlocale and the locale objects, Dinkumware's locale internals and its locale files (the
// last pspocket's reading of Dinkumware's design: the SDK's library has no copy of the file reader). The game
// comparisons (test_libc_locale_game.cpp) found these answers in a game's copy of the library. No game data.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace fsys = std::filesystem;

namespace {

constexpr u32 kA = kData + 0x1000, kB = kData + 0x1800, kTm = kData + 0x2000, kOut = kData + 0x3000,
              kFmt = kData + 0x4000, kLen = kData + 0x4800, kTin = kData + 0x5000, kText = kData + 0x6000,
              kPcats = kData + 0x8000, kLinfo = kData + 0x9000, kLinfo2 = kData + 0xA000, kBuf = kData + 0xB000,
              kPath = kData + 0xC000, kMode = kData + 0xC100;

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

std::u32string wstr(u8* b, u32 at)
{
    std::u32string s;
    for (u32 c; (c = ps_read32(b, at + 4 * static_cast<u32>(s.size()))) != 0;) s += static_cast<char32_t>(c);
    return s;
}

void wput(u8* b, u32 at, std::u32string_view s)
{
    for (usize i = 0; i < s.size(); i++) ps_write32(b, at + 4 * static_cast<u32>(i), static_cast<u32>(s[i]));
    ps_write32(b, at + 4 * static_cast<u32>(s.size()), 0);
}

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

void put_tm(u8* b, u32 at, std::array<s32, 9> f)
{
    for (u32 i = 0; i < 9; i++) ps_write32(b, at + 4 * i, static_cast<u32>(f[i]));
}

std::array<s32, 9> get_tm(u8* b, u32 at)
{
    std::array<s32, 9> f{};
    for (u32 i = 0; i < 9; i++) f[i] = static_cast<s32>(ps_read32(b, at + 4 * i));
    return f;
}

u32 var(std::string_view name) { return HleProcess::current()->link_variable("sys_libc", recomp::nid_of(name)); }

// 2009-02-13 23:31:30, a Friday, the 44th day.
constexpr std::array<s32, 9> kFeb13 = {30, 31, 23, 13, 1, 109, 5, 43, 0};

struct Lib {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();

    // strftime of fmt for tm fields, the text (its length as strftime answers it).
    std::string fmt(std::string_view format, std::array<s32, 9> tm, u32 max = 512, u32* n = nullptr)
    {
        put(b, kFmt, format);
        put_tm(b, kTm, tm);
        std::memset(b + kOut, 0, 600);
        const u32 r = static_cast<u32>(t.call("strftime", kOut, max, kFmt, kTm));
        if (n) *n = r;
        return std::string(reinterpret_cast<const char*>(b + kOut), r);
    }
};

} // namespace

TEST_CASE("sys_libc: strftime, asctime and the calendar as the SDK's", "[hle][libc]")
{
    Lib l;
    auto& t = l.t;
    u8* b = l.b;
    u32 n = 0;
    CHECK(l.fmt("%c|%x|%X|%r|%D|%F|%R|%T", kFeb13) == "Fri Feb 13 23:31:30 2009|02/13/09|23:31:30|11:31:30 PM|02/13/09|"
                                                    "2009-02-13|23:31|23:31:30");
    CHECK(l.fmt("%a %A %b %B %h %p %j %u %w %U %W %V %G %g %C %y %e", kFeb13) ==
          "Fri Friday Feb February Feb PM 044 5 5 06 06 07 2009 09 20 09 13");
    // An unknown conversion is its letter and a NUL; the modifiers change nothing in the "C" locale; no zone.
    CHECK(l.fmt("%q|", kFeb13, 512, &n) == std::string("q\0|", 3));
    CHECK(n == 3);
    CHECK(l.fmt("%Ec|%Ey|%EC|%EY|%Od|%OH", kFeb13) == "Fri Feb 13 23:31:30 2009|09|20|2009|13|23");
    CHECK(l.fmt("[%z][%Z]", kFeb13) == "[][]");
    CHECK(l.fmt("100%", kFeb13) == "100%");
    // Numbers are clamped at 0 and cut to their width; the century is the year / 100 + 19.
    CHECK(l.fmt("%Y %C %y", {0, 0, 0, 1, 0, -1905, 0, 0, 0}) == "0000 00 95");
    CHECK(l.fmt("%Y %C", {0, 0, 0, 1, 0, 10445, 0, 0, 0}) == "2345 23");
    CHECK(l.fmt("%H %I %d %e", {0, 0, -1, 5, 0, 100, 0, 0, 0}) == "00 11 05  5");
    CHECK(l.fmt("%a|%b", {0, 0, 0, 1, 12, 100, 7, 0, 0}) == "|"); // past the names: nothing
    CHECK(l.fmt("%a|%b", {0, 0, 0, 1, -1, 100, -1, 0, 0}) == "Sun|Jan"); // before them: the first
    // The SDK's ISO weeks count every year as 365 days: 2020-12-30 and -31 are week 1 of 2021.
    CHECK(l.fmt("%V %G %g", {0, 0, 0, 31, 11, 120, 4, 365, 0}) == "01 2021 21");
    CHECK(l.fmt("%V %G %g", {0, 0, 0, 1, 0, 121, 5, 0, 0}) == "53 2020 20");
    // Too small: 0, the pieces that fit written.
    std::memset(b + kOut, '#', 32);
    put_tm(b, kTm, kFeb13);
    put(b, kFmt, "%Y-%m-%d");
    CHECK(t.call("strftime", kOut, 6, kFmt, kTm) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(b + kOut), 6) == "2009-#");
    CHECK(t.call("strftime", kOut, 10, kFmt, kTm) == 0); // no room for the NUL
    CHECK(t.call("strftime", kOut, 11, kFmt, kTm) == 10);
    // asctime and ctime: "%c\n" in the "C" locale's names, whatever the fields.
    put_tm(b, kTm, kFeb13);
    CHECK(cstr(b, static_cast<u32>(t.call("asctime", kTm))) == "Fri Feb 13 23:31:30 2009\n");
    put_tm(b, kTm, {0, 0, 25, 1, 0, 10445, 7, 0, 0});
    CHECK(cstr(b, static_cast<u32>(t.call("asctime", kTm))) == " Jan  1 25:00:00 2345\n");
    // gmtime with Plauger's calendar: 2100 is a leap year, so its 1 March is 29 February.
    ps_write64(b, kA, 4107542400ull);
    u32 tm = static_cast<u32>(t.call("gmtime", kA));
    CHECK(get_tm(b, tm) == std::array<s32, 9>{0, 0, 0, 29, 1, 200, 1, 59, 0});
    ps_write64(b, kA, 1234567890);
    tm = static_cast<u32>(t.call("gmtime", kA));
    CHECK(get_tm(b, tm) == kFeb13);
    ps_write64(b, kA, static_cast<u64>(-2208988801ll)); // a second before 1900: negative fields
    tm = static_cast<u32>(t.call("gmtime", kA));
    CHECK(get_tm(b, tm) == std::array<s32, 9>{-1, 0, 0, 1, 0, 0, 1, 0, 0});
    // localtime: lv2's zone and summer offset added, tm_isdst 1 while there is a summer offset.
    s32 zone = 0, summer = 0;
    lv2::host_time_zone(zone, summer);
    ps_write64(b, kA, 1234567890);
    tm = static_cast<u32>(t.call("localtime", kA));
    const auto local = get_tm(b, tm);
    ps_write64(b, kA, static_cast<u64>(1234567890 + 60 * (zone + summer)));
    auto want = get_tm(b, static_cast<u32>(t.call("gmtime", kA)));
    want[8] = summer ? 1 : 0;
    CHECK(local == want);
    // ctime is asctime of localtime.
    ps_write64(b, kA, 1234567890);
    const std::string ct = cstr(b, static_cast<u32>(t.call("ctime", kA)));
    put_tm(b, kTm, local);
    CHECK(ct == cstr(b, static_cast<u32>(t.call("asctime", kTm))));
    // mktime undoes localtime, normalizing its fields with the same calendar.
    put_tm(b, kTm, local);
    CHECK(static_cast<s64>(t.call("mktime", kTm)) == 1234567890);
    put_tm(b, kTm, {0, 0, 0, 29, 1, 200, 0, 0, 0});
    const s64 feb29 = static_cast<s64>(t.call("mktime", kTm));
    CHECK(feb29 == 4107542400ll - 60 * zone);
    CHECK(get_tm(b, kTm)[3] == 29);
    // _Ttotm: a non-zero tm_isdst adds lv2's summer offset and is kept as given.
    t.call("_Ttotm", kTm, 0, static_cast<u64>(-1));
    CHECK(get_tm(b, kTm) == std::array<s32, 9>{0, summer % 60, summer / 60, 1, 0, 70, 4, 0, -1});
    CHECK(i32(t.call("_Daysto", 200, 2)) == 49 + 60);
    CHECK(i32(t.call("_Daysto", -5, 0)) == 1 + 9 / 4);
    // _Gettime: past the fields, the NUL with length 0; before them, the first field.
    put(b, kA, ":AM:PM");
    CHECK(t.call("_Gettime", kA, 2, kLen) == kA + 6);
    CHECK(ps_read32(b, kLen) == 0);
    CHECK(t.call("_Gettime", kA, -1, kLen) == kA + 1);
    CHECK(ps_read32(b, kLen) == 2);
    // _Getptimes is the variable _Times, the "C" locale's names.
    const u32 times = var("_Times");
    CHECK(t.call("_Getptimes") == times);
    CHECK(cstr(b, ps_read32(b, times)) == ":AM:PM");
    CHECK(cstr(b, ps_read32(b, times + 28)) == "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p");
    CHECK(ps_read32(b, times + 32) == ps_read32(b, times + 28));
    CHECK(cstr(b, ps_read32(b, times + 80)).empty());
}

TEST_CASE("sys_libc: a locale's eras, digits and zones through _CStrftime and _Gentime", "[hle][libc]")
{
    Lib l;
    auto& t = l.t;
    u8* b = l.b;
    // A _Tinfo of the C locale's names with Japanese eras, alternative digits and a zone.
    const std::string days = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
    const std::string months = ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:"
                               "Sep:September:Oct:October:Nov:November:Dec:December";
    const std::string fmts = "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p";
    const std::array<std::string, 21> f = {":AM:PM", days, days, days, months, months, months, fmts, fmts, fmts, fmts,
        fmts, fmts, fmts, fmts, fmts, fmts,
        ";+:1:1989/01/08:+*:Heisei:%EC%Ey\xE5\xB9\xB4;+:1:1926/12/25:1989/01/07:Showa:%EC%Ey",
        ";zero;one;two;three;four;five", "", ":EST:EDT:-0500"};
    u32 at = kText;
    for (u32 i = 0; i < 21; i++) {
        put(b, at, f[i]);
        ps_write32(b, kTin + 4 * i, at);
        at += static_cast<u32>(f[i].size()) + 1;
    }
    put(b, kFmt, "%EC|%Ey|%EY|%Od|%OH|%Z|%z|%Om");
    put_tm(b, kTm, {1, 2, 3, 4, 5, 100, 1, 2, 1});
    CHECK(t.call("_CStrftime", kOut, 256, kFmt, kTm, kTin) == 45);
    CHECK(cstr(b, kOut) == "Heisei|12|Heisei12\xE5\xB9\xB4|four|three|EDT|-0400|06");
    put_tm(b, kTm, {1, 2, 3, 4, 5, 50, 1, 2, 0});
    t.call("_CStrftime", kOut, 256, kFmt, kTm, kTin);
    CHECK(cstr(b, kOut) == "Showa|25|Showa25|four|three|EST|-0500|06");
    // _Gentime: %c is the locale's format to expand (a negative length), %d a number in the buffer.
    const u32 p = static_cast<u32>(t.call("_Gentime", kTm, kTin, 0, 'c', kLen, kBuf));
    CHECK(i32(ps_read32(b, kLen)) == -14);
    CHECK(std::string(reinterpret_cast<const char*>(b + p), 14) == "%a %b %e %T %Y");
    CHECK(t.call("_Gentime", kTm, kTin, 0, 'd', kLen, kBuf) == kBuf);
    CHECK(cstr(b, kBuf) == "04");
    CHECK(t.call("_Gentime", kTm, kTin, 'O', 'd', kLen, kBuf) == ps_read32(b, kTin + 72) + 20);
    CHECK(ps_read32(b, kLen) == 4);
    // A locale whose formats expand into themselves stops (the SDK's loops for ever).
    ps_write32(b, kTin + 32, kText + 0x1000);
    put(b, kText + 0x1000, "%c");
    put(b, kFmt, "%c");
    CHECK(t.call("_CStrftime", kOut, 256, kFmt, kTm, kTin) == 1); // "c": %c's field 0 of "%c" is "c"
    put(b, kText + 0x1000, ":%Ec");
    ps_write32(b, kTin + 52, kText + 0x1000);
    put(b, kFmt, "%Ec");
    CHECK(t.call("_CStrftime", kOut, 256, kFmt, kTm, kTin) == 0);
}

TEST_CASE("sys_libc: wcsftime, _Isdst and _Getdst, which the SDK's game copy does not have", "[hle][libc]")
{
    Lib l;
    auto& t = l.t;
    u8* b = l.b;
    // wcsftime: the conversions' text as wide characters (multibyte characters converted), the format's own wide.
    wput(b, kFmt, U"%A %d é€ %Y%%");
    put_tm(b, kTm, kFeb13);
    CHECK(t.call("wcsftime", kOut, 64, kFmt, kTm) == 18);
    CHECK(wstr(b, kOut) == U"Friday 13 é€ 2009%");
    wput(b, kFmt, U"%c|%q");
    CHECK(t.call("wcsftime", kOut, 64, kFmt, kTm) == 27);
    CHECK(wstr(b, kOut) == U"Fri Feb 13 23:31:30 2009|q");
    CHECK(t.call("wcsftime", kOut, 10, kFmt, kTm) == 0);
    CHECK(t.call("_Wcsftime", kOut, 64, kFmt, kTm, var("_Times")) == 27);
    // _Getdst: Plauger's rules, five-byte records {wday, hour, day, mon, year}, 255 ending them.
    put(b, kA, ":0401:1001");
    const u32 rules = static_cast<u32>(t.call("_Getdst", kA));
    REQUIRE(rules != 0);
    const u8 want[] = {0, 0, 0, 3, 0, 0, 0, 0, 9, 0, 0xFF};
    for (u32 i = 0; i < sizeof want; i++) CHECK(b[rules + i] == want[i]);
    put(b, kA, ":(2010)0331+0");
    const u32 r2 = static_cast<u32>(t.call("_Getdst", kA));
    REQUIRE(r2 != 0);
    CHECK(b[r2] == 14); // "+0": the first Sunday (7) on or after the date (+ 7)
    CHECK(b[r2 + 3] == 2);
    CHECK(b[r2 + 2] == 30);
    CHECK(b[r2 + 4] == 110);
    put(b, kA, ":1301");
    CHECK(t.call("_Getdst", kA) == 0); // no month 13
    put(b, kA, "");
    CHECK(t.call("_Getdst", kA) == 0);
    // _Isdst: the "C" locale has no rules: lv2's summer time.
    s32 zone = 0, summer = 0;
    lv2::host_time_zone(zone, summer);
    put_tm(b, kTm, {0, 0, 12, 15, 5, 109, 1, 165, 0});
    CHECK(i32(t.call("_Isdst", kTm)) == (summer ? 1 : 0));
    // With the locale's rules: 1 April to 1 October.
    const u32 times = var("_Times");
    const u32 old = ps_read32(b, times + 76);
    put(b, kB, ":0401:1001");
    ps_write32(b, times + 76, kB);
    CHECK(i32(t.call("_Isdst", kTm)) == 1);
    put_tm(b, kTm, {0, 0, 12, 15, 0, 109, 4, 14, 0});
    CHECK(i32(t.call("_Isdst", kTm)) == 0);
    put_tm(b, kTm, {0, 0, 12, 15, 11, 109, 2, 348, 0});
    CHECK(i32(t.call("_Isdst", kTm)) == 0);
    ps_write32(b, times + 76, old);
}

TEST_CASE("sys_libc: setlocale, localeconv and the locale objects as the SDK's", "[hle][libc]")
{
    Lib l;
    auto& t = l.t;
    u8* b = l.b;
    const u32 c = var("_Clocale");
    const u32 cname = ps_read32(b, c);
    CHECK(cstr(b, cname) == "C");
    // setlocale: "C" alone, whatever the category; the answer is the "C" locale's name.
    CHECK(t.call("setlocale", 0, 0) == cname);
    put(b, kA, "C");
    CHECK(t.call("setlocale", 1000, kA) == cname);
    for (const char* name : {"", "POSIX", "C;collate:C", "fr_FR"}) {
        INFO(name);
        put(b, kA, name);
        CHECK(t.call("setlocale", 0, kA) == 0);
    }
    CHECK(t.call("_Defloc") == cname);
    // localeconv: _Locale, the SDK's "C" values.
    const u32 lc = static_cast<u32>(t.call("localeconv"));
    CHECK(lc == var("_Locale"));
    CHECK(cstr(b, ps_read32(b, lc + 44)) == ".");
    for (u32 i = 0; i < 14; i++) CHECK(b[lc + 28 + i] == 0x7F);
    CHECK(cstr(b, ps_read32(b, lc + 64)) == "false");
    CHECK(cstr(b, ps_read32(b, lc + 68)) == "true");
    CHECK(cstr(b, ps_read32(b, lc + 72)).empty());
    // _Loctab: 64 entries {name, _Linfo offset, kind} and an empty one.
    const u32 tab = var("_Loctab");
    const auto entry = [&](u32 i) {
        return std::tuple(cstr(b, ps_read32(b, tab + 12 * i)), ps_read32(b, tab + 12 * i + 4), ps_read32(b, tab + 12 * i + 8));
    };
    CHECK(entry(0) == std::tuple(std::string("LOCALE"), 0u, 1u));
    CHECK(entry(3) == std::tuple(std::string("collate"), 0x8u, 4u));
    CHECK(entry(8) == std::tuple(std::string("mb_cur_max"), 0x94u, 7u));
    CHECK(entry(16) == std::tuple(std::string("mon_grouping"), 0x12Cu, 0u));
    CHECK(entry(34) == std::tuple(std::string("decimal_point"), 0x14Cu, 5u));
    CHECK(entry(61) == std::tuple(std::string("time_zone"), 0x1C0u, 5u));
    CHECK(entry(63) == std::tuple(std::string("yes"), 0x16Cu, 5u));
    CHECK(ps_read32(b, tab + 12 * 64) == 0);
    // _Clocale: its name only until _Getcloc fills it from the current tables.
    CHECK(ps_read32(b, c + 0x94) == 0);
    CHECK(t.call("_Getcloc") == c);
    CHECK(b[c + 0x94] == 3);
    CHECK(ps_read32(b, c + 0x88) == ps_read32(b, var("_Ctype")));
    CHECK(ps_read32(b, c + 0x11C) == t.call("_Getpwctytab"));
    CHECK(ps_read32(b, c + 0x118) == 0); // the SDK's leaves the translation table out
    CHECK(cstr(b, ps_read32(b, c + 0x120 + 44)) == ".");
    CHECK(cstr(b, ps_read32(b, c + 0x170)) == ":AM:PM");
    // _Getloc: a category's locale in a (composite) name: the "C" locale, or none.
    const auto getloc = [&](const char* cat, const char* name) {
        put(b, kA, cat);
        put(b, kB, name);
        return static_cast<u32>(t.call("_Getloc", kA, kB));
    };
    CHECK(getloc("collate:", "C") == c);
    CHECK(getloc("ctype:", "X;ctype:C") == c);
    CHECK(getloc("ctype:", "C;ctype:X") == 0);
    CHECK(getloc("collate:", "C;ctype:X") == c);
    CHECK(getloc("", "collate:C") == 0);
    CHECK(getloc("collate:", "X") == 0);
    CHECK(getloc("", "") == 0);
    // _Getnloc: the categories of a mask (the whole one with all of them); -1 for a bad mask or name.
    std::memset(b + kPcats, 0, 28);
    put(b, kA, "C");
    CHECK(i32(t.call("_Getnloc", kPcats, 63, kA)) == 1);
    for (u32 i = 0; i < 7; i++) CHECK(ps_read32(b, kPcats + 4 * i) == c);
    CHECK(i32(t.call("_Getnloc", kPcats, 63, kA)) == 0);
    put(b, kA, "");
    CHECK(i32(t.call("_Getnloc", kPcats, 2, kA)) == 0); // "" is the default, "C"
    CHECK(i32(t.call("_Getnloc", kPcats, 64, kA)) == -1);
    put(b, kA, "X");
    CHECK(i32(t.call("_Getnloc", kPcats, 1, kA)) == -1);
    // _Getlname: the whole locale's name, or "name;category:name;..." for categories that differ (malloc'd).
    ps_write32(b, kBuf, 0);
    CHECK(cstr(b, static_cast<u32>(t.call("_Getlname", kPcats, 63, kBuf))) == "C");
    CHECK(ps_read32(b, kBuf) == 0);
    std::memset(b + kLinfo, 0, 0x1C4);
    put(b, kText, "X");
    ps_write32(b, kLinfo, kText);
    ps_write32(b, kPcats + 8, kLinfo); // ctype: X
    const u32 name = static_cast<u32>(t.call("_Getlname", kPcats, 63, kBuf));
    CHECK(cstr(b, name) == "C;ctype:X");
    CHECK(ps_read32(b, kBuf) == name);
    ps_write32(b, kPcats, kLinfo); // the whole: X, every category C
    CHECK(cstr(b, static_cast<u32>(t.call("_Getlname", kPcats, 63, kBuf))) ==
          "X;collate:C;monetary:C;numeric:C;time:C;messages:C");
    ps_write32(b, kPcats + 8, c);
    CHECK(cstr(b, static_cast<u32>(t.call("_Getlname", kPcats, 63, kBuf))) ==
          "collate:C;ctype:C;monetary:C;numeric:C;time:C;messages:C");
    CHECK(t.call("_Getlname", kPcats, 0, kBuf) == 0);
    CHECK(ps_read32(b, kBuf) == 0);
    // _Skip, _Findloc, the global locale pointer.
    put(b, kA, "a \t b");
    CHECK(t.call("_Skip", kA) == kA + 4);
    put(b, kA, "");
    CHECK(t.call("_Skip", kA) == kA);
    put(b, kA, "C");
    CHECK(t.call("_Findloc", kA, 1) == 0);
    CHECK(t.call("_Getgloballocale") == 0);
    t.call("_Setgloballocale", 0x1234);
    CHECK(t.call("_Getgloballocale") == 0x1234);
    t.call("_Setgloballocale", 0);
}

TEST_CASE("sys_libc: Dinkumware's locale files, _Setloc and _Freeloc (pspocket's reading of the format)",
    "[hle][libc]")
{
    Lib l;
    auto& t = l.t;
    u8* b = l.b;
    const fsys::path root = fsys::temp_directory_path() / std::format("pspocket-libc-locale-{}", static_cast<long>(getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "d");
    REQUIRE(l.f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    {
        std::ofstream out(root / "d" / "loc.txt");
        out << "NOTE a test locale\n"
               "LOCALE fr\n"
               "\n"
               "decimal_point \",\"\n"
               "thousands_sep \".\"\n"
               "grouping \"3\"\n"
               "currency_symbol \"EUR\\101\"\n"
               "frac_digits 2\n"
               "SET B 'a'\n"
               "mb_cur_max 1\n"
               "ctype['a':'z'] $$ + 0\n"
               "tolower['A':'Z'] $@ + 32\n"
               "mbtowc[0, 0:$^] $@ $F $I $O $0\n"
               "wctomb[0, 0:$^] $@ $F $I $O $0\n"
               "day \":dim:dimanche:lun:lundi:mar:mardi:mer:mercredi:jeu:jeudi:ven:vendredi:sam:\\\n"
               "samedi\"\n"
               "wctype vowel 'a' 1 'e' 1 $B:$B 1\n"
               "LOCALE de\n"
               "decimal_point \",\"\n"
               "bogus 1\n";
    }
    // The terms and sums.
    put(b, kA, "'a' + 2 $t");
    CHECK(t.call("_Locsum", kA, kLen) == kA + 10);
    CHECK(ps_read32(b, kLen) == 'a' + 2 + '\t');
    put(b, kA, "-0x10 rest");
    ps_write32(b, kB, kA);
    CHECK(t.call("_Locterm", kB, kLen) == 1);
    CHECK(i32(ps_read32(b, kLen)) == -16);
    CHECK(ps_read32(b, kB) == kA + 6);
    put(b, kA, "?");
    CHECK(t.call("_Locsum", kA, kLen) == 0);
    CHECK(t.call("_Locvar", 'Q', 5) == 1);
    put(b, kA, "$Q $2");
    CHECK(t.call("_Locsum", kA, kLen) == kA + 5);
    CHECK(ps_read32(b, kLen) == 5 + 0x200);
    CHECK(t.call("_Locvar", 'q', 5) == 0);
    put(b, kA, "abc\n");
    CHECK(t.call("_Nnl", kA) == 1);
    CHECK(cstr(b, kA) == "abc");
    CHECK(t.call("_Nnl", kA) == 0);
    // _Readloc: lines by keyword.
    put(b, kPath, "/dev_hdd0/d/loc.txt");
    put(b, kMode, "r");
    const u32 lf = static_cast<u32>(t.call("fopen", kPath, kMode));
    REQUIRE(lf != 0);
    const u32 tab = var("_Loctab");
    CHECK(t.call("_Readloc", lf, kBuf, kA) == tab + 12); // NOTE
    CHECK(cstr(b, ps_read32(b, kA)) == "a test locale");
    CHECK(t.call("_Readloc", lf, kBuf, kA) == tab); // LOCALE
    CHECK(cstr(b, ps_read32(b, kA)) == "fr");
    // _Makeloc: a copy of the "C" locale filled from the lines up to the next LOCALE.
    const u32 c = static_cast<u32>(t.call("_Getcloc"));
    std::memcpy(b + kLinfo, b + c, 0x1C4);
    const u32 fr = static_cast<u32>(t.call("malloc", 8)); // a locale's name is its own (_Freeloc frees it)
    put(b, fr, "fr");
    ps_write32(b, kLinfo, fr);
    CHECK(t.call("_Makeloc", lf, kBuf, kLinfo) == 1);
    CHECK(cstr(b, ps_read32(b, kLinfo + 0x120 + 44)) == ",");
    CHECK(cstr(b, ps_read32(b, kLinfo + 0x120 + 48)) == "\x03");
    CHECK(cstr(b, ps_read32(b, kLinfo + 0x120)) == "EURA");
    CHECK(b[kLinfo + 0x120 + 28] == 2);
    CHECK(b[kLinfo + 0x94] == 1);
    CHECK(cstr(b, ps_read32(b, kLinfo + 0x170 + 12)) ==
          ":dim:dimanche:lun:lundi:mar:mardi:mer:mercredi:jeu:jeudi:ven:vendredi:sam:samedi");
    const u32 lower = ps_read32(b, kLinfo + 0x8C);
    CHECK(lower != ps_read32(b, c + 0x8C));
    CHECK(ps_read16(b, lower + 2 * 'Q') == 'q');
    CHECK(static_cast<s16>(ps_read16(b, lower - 2)) == -1); // EOF kept
    const u32 mbt = ps_read32(b, kLinfo + 0x98);
    REQUIRE(mbt != 0);
    CHECK(ps_read16(b, mbt - 2) == 1); // made: _Freeloc frees it
    CHECK(ps_read16(b, mbt + 2 * 0xE9) == (0x8000 | 0x4000 | 0x2000 | 0xE9));
    // The next definition fails on its unknown keyword.
    std::memcpy(b + kLinfo2, b + c, 0x1C4);
    CHECK(t.call("_Makeloc", lf, kBuf, kLinfo2) == 0);
    t.call("fclose", lf);
    // _Makestab and _Makewct on their own.
    put(b, kA, "[300] 1");
    CHECK(t.call("_Makestab", kLinfo2, tab + 12 * 5, kA) == 0); // ctype past 255
    put(b, kA, "[16, 0:1] 1");
    CHECK(t.call("_Makestab", kLinfo2, tab + 12 * 9, kA) == 0); // no state 16
    put(b, kA, "upper 'A':'Z' 1 0xC0:0xDE 1");
    CHECK(t.call("_Makewct", kLinfo2, tab + 12 * 12, kA) == 1);
    // _Setloc: the locale made current; strftime's names, localeconv, the conversions follow.
    CHECK(t.call("_Setloc", 0, kLinfo) == kLinfo);
    CHECK(cstr(b, ps_read32(b, static_cast<u32>(t.call("localeconv")) + 44)) == ",");
    CHECK(l.fmt("%A", {0, 0, 0, 1, 0, 100, 1, 0, 0}) == "lundi");
    CHECK(t.call("_Getmbcurmax") == 1);
    b[kA] = 0xE9;
    CHECK(t.call("mbrtowc", kB, kA, 1, 0) == 1); // the file's one-byte table, not UTF-8
    CHECK(ps_read32(b, kB) == 0xE9);
    put(b, kA, "vowel");
    const u32 vowel = static_cast<u32>(t.call("wctype", kA));
    CHECK(vowel == 13);
    CHECK(t.call("iswctype", 'e', vowel) == 1);
    CHECK(t.call("iswctype", 'b', vowel) == 0);
    CHECK(t.call("iswalpha", 'q') == 1); // the "C" classes kept
    // Back to the "C" locale.
    CHECK(t.call("_Setloc", 0, c) == c);
    CHECK(cstr(b, ps_read32(b, static_cast<u32>(t.call("localeconv")) + 44)) == ".");
    CHECK(t.call("_Getmbcurmax") == 3);
    put(b, kA, "\xC3\xA9");
    CHECK(t.call("mbrtowc", kB, kA, 2, 0) == 2);
    put(b, kA, "vowel");
    CHECK(t.call("wctype", kA) == 0);
    // _Freeloc frees what the locale made; the "C" locale's own pointers are left alone.
    t.call("_Freeloc", kLinfo);
    t.call("_Freeloc", kLinfo2);
    t.call("_Freeloc", c);
    fsys::remove_all(root);
}
