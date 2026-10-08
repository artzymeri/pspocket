// sys_libc (runtime/hle/src/sys_libc*.cpp, D-511): the C library of the PS3 SDK checked against C99 and Dinkumware's
// documented choices, each function called as guest code calls it. No firmware module has the library, so there is no
// Sony code to compare with. No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kA = kData + 0x1000, kB = kData + 0x2000, kOut = kData + 0x3000, kCode = kData + 0x4000,
              kOpd = kData + 0x4100, kCounter = kData + 0x4200, kArr = kData + 0x5000;

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

// A guest comparator at kCode: counter += 1, then *r3 - *r4 as s32.
void write_comparator(u8* b)
{
    const u32 words[] = {0x3CE00000u | (kCounter >> 16), 0x60E70000u | (kCounter & 0xFFFF), 0x81070000u, 0x39080001u,
        0x91070000u, 0x80A30000u, 0x80C40000u, 0x7C662850u, 0x7C6307B4u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, kCode + 4 * i, words[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    ps_write32(b, kCounter, 0);
}

} // namespace

TEST_CASE("sys_libc: string.h, Dinkumware's way", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    put(b, kA, "hello world");
    CHECK(t.call("strlen", kA) == 11);
    CHECK(t.call("strcpy", kOut, kA) == kOut);
    CHECK(cstr(b, kOut) == "hello world");
    // strncpy pads with NULs to n, and leaves the string unterminated when it fills n.
    std::memset(b + kOut, 0x7E, 16);
    CHECK(t.call("strncpy", kOut, kA, 14) == kOut);
    CHECK(std::memcmp(b + kOut, "hello world\0\0\0\x7E", 15) == 0);
    std::memset(b + kOut, 0x7E, 16);
    t.call("strncpy", kOut, kA, 5);
    CHECK(std::memcmp(b + kOut, "hello\x7E", 6) == 0);
    // Comparisons give -1, 0 or +1, bytes as unsigned char.
    put(b, kB, "hello\xC0");
    CHECK(i32(t.call("strcmp", kA, kA)) == 0);
    CHECK(i32(t.call("strcmp", kA, kB)) == -1); // ' ' < 0xC0
    CHECK(i32(t.call("strcmp", kB, kA)) == 1);
    CHECK(i32(t.call("strncmp", kA, kB, 5)) == 0);
    put(b, kB, "HELLO WORLD");
    CHECK(i32(t.call("strcasecmp", kA, kB)) == 0);
    CHECK(i32(t.call("strncasecmp_ascii", kA, kB, 3)) == 0);
    CHECK(i32(t.call("strcoll", kA, kB)) == 1);
    // Searches.
    CHECK(t.call("strchr", kA, 'o') == kA + 4);
    CHECK(t.call("strrchr", kA, 'o') == kA + 7);
    CHECK(t.call("strchr", kA, 0) == kA + 11);
    CHECK(t.call("strchr", kA, 'z') == 0);
    CHECK(t.call("memchr", kA, 'w', 11) == kA + 6);
    put(b, kB, "wor");
    CHECK(t.call("strstr", kA, kB) == kA + 6);
    put(b, kB, "leh");
    CHECK(t.call("strspn", kA, kB) == 4);
    put(b, kB, "wol");
    CHECK(t.call("strcspn", kA, kB) == 2);
    CHECK(t.call("strpbrk", kA, kB) == kA + 2);
    // strcat, strncat.
    put(b, kOut, "ab");
    put(b, kB, "cdef");
    t.call("strcat", kOut, kB);
    CHECK(cstr(b, kOut) == "abcdef");
    t.call("strncat", kOut, kB, 2);
    CHECK(cstr(b, kOut) == "abcdefcd");
    // strtok keeps its place per thread; strtok_r in the caller's pointer.
    put(b, kA, "  a,b,,c  ");
    put(b, kB, " ,");
    CHECK(cstr(b, static_cast<u32>(t.call("strtok", kA, kB))) == "a");
    CHECK(cstr(b, static_cast<u32>(t.call("strtok", 0, kB))) == "b");
    CHECK(cstr(b, static_cast<u32>(t.call("strtok", 0, kB))) == "c");
    CHECK(t.call("strtok", 0, kB) == 0);
    put(b, kA, "x;y");
    put(b, kB, ";");
    CHECK(cstr(b, static_cast<u32>(t.call("strtok_r", kA, kB, kOut))) == "x");
    CHECK(cstr(b, static_cast<u32>(t.call("strtok_r", 0, kB, kOut))) == "y");
    // strerror's message lives in the thread's block.
    CHECK(cstr(b, static_cast<u32>(t.call("strerror", 0))) == "no error");
    // A string that runs out of guest memory ends the call (logged).
    CHECK(t.call("strlen", 0x0FFF0000) == 0);
}

TEST_CASE("sys_libc: ctype.h in the C locale", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    for (s32 c = -1; c < 256; c++) {
        INFO(c);
        const bool ascii = c >= 0 && c < 128;
        CHECK((t.call("isalpha", c) != 0) == (ascii && std::isalpha(c)));
        CHECK((t.call("isdigit", c) != 0) == (ascii && std::isdigit(c)));
        CHECK((t.call("isspace", c) != 0) == (ascii && std::isspace(c)));
        CHECK((t.call("ispunct_ascii", c) != 0) == (ascii && std::ispunct(c)));
        CHECK((t.call("isxdigit", c) != 0) == (ascii && std::isxdigit(c)));
        CHECK(i32(t.call("toupper", static_cast<u64>(c))) == (c >= 'a' && c <= 'z' ? c - 32 : c));
    }
}

TEST_CASE("sys_libc: stdlib.h numbers, sorting and random numbers", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    CHECK(i32(t.call("abs", static_cast<u64>(-5))) == 5);
    CHECK(i32(t.call("abs", static_cast<u64>(static_cast<s64>(INT_MIN)))) == INT_MIN);
    CHECK(static_cast<s64>(t.call("llabs", static_cast<u64>(-7ll))) == 7);
    // div and lldiv fill the caller's buffer (r3).
    t.call("div", kOut, static_cast<u64>(-7), 2);
    CHECK(static_cast<s32>(ps_read32(b, kOut)) == -3);
    CHECK(static_cast<s32>(ps_read32(b, kOut + 4)) == -1);
    t.call("lldiv", kOut, 100000000000ll, 7);
    CHECK(ps_read64(b, kOut) == 14285714285ull);
    CHECK(ps_read64(b, kOut + 8) == 5);
    // strtol: bases, the end pointer, ERANGE clamping at 32 bits (long), 64 for long long.
    put(b, kA, "  -0x1fz");
    CHECK(i32(t.call("strtol", kA, kOut, 0)) == -31);
    CHECK(ps_read32(b, kOut) == kA + 7);
    put(b, kA, "0777");
    CHECK(i32(t.call("strtol", kA, 0, 0)) == 0777);
    put(b, kA, "99999999999");
    CHECK(i32(t.call("strtol", kA, 0, 10)) == INT_MAX);
    CHECK(static_cast<u32>(t.call("strtoul", kA, 0, 10)) == 0xFFFFFFFFu);
    CHECK(t.call("strtoll", kA, 0, 10) == 99999999999ull);
    put(b, kA, "zz");
    CHECK(t.call("strtol", kA, kOut, 36) == 35 * 36 + 35);
    put(b, kA, "xyz");
    CHECK(t.call("strtol", kA, kOut, 10) == 0);
    CHECK(ps_read32(b, kOut) == kA); // no digits: end = the string
    put(b, kA, "  42abc");
    CHECK(i32(t.call("atoi", kA)) == 42);
    // strtod: the C locale's text, hex, inf and nan; the result in f1.
    put(b, kA, "  -1.5e3xyz");
    t.call("strtod", kA, kOut);
    CHECK(t.ctx.fpr[1] == -1500.0);
    CHECK(ps_read32(b, kOut) == kA + 8);
    put(b, kA, "0x1p-2");
    t.call("atof", kA);
    CHECK(t.ctx.fpr[1] == 0.25);
    put(b, kA, "-inf");
    t.call("strtof", kA, 0);
    CHECK(std::isinf(t.ctx.fpr[1]));
    // qsort and bsearch through a guest comparator.
    write_comparator(b);
    const s32 values[] = {5, -3, 9, 0, 9, -100, 42, 7};
    for (u32 i = 0; i < std::size(values); i++) ps_write32(b, kArr + 4 * i, static_cast<u32>(values[i]));
    t.call("qsort", kArr, 8, 4, kOpd);
    std::vector<s32> sorted(values, values + 8);
    std::sort(sorted.begin(), sorted.end());
    for (u32 i = 0; i < 8; i++) CHECK(static_cast<s32>(ps_read32(b, kArr + 4 * i)) == sorted[i]);
    CHECK(ps_read32(b, kCounter) > 0);
    ps_write32(b, kOut, 7);
    CHECK(t.call("bsearch", kOut, kArr, 8, 4, kOpd) == kArr + 4 * 4);
    ps_write32(b, kOut, 8);
    CHECK(t.call("bsearch", kOut, kArr, 8, 4, kOpd) == 0);
    // rand: Dinkumware's linear congruential generator, RAND_MAX 0x7FFF.
    t.call("srand", 1);
    CHECK(t.call("rand") == 16838);
    CHECK(t.call("rand") == 5758);
    // TT800: the _ forms on a given state equal the others on __TT800 after the same seed; reals in their ranges.
    t.call("init_TT800", 1234);
    t.call("_init_TT800", kArr, 1234);
    for (u32 i = 0; i < 60; i++) CHECK(t.call("rand_int32_TT800") == t.call("_rand_int32_TT800", kArr));
    for (u32 i = 0; i < 100; i++) {
        t.call("rand_real2_TT800");
        CHECK((t.ctx.fpr[1] >= 0.0 && t.ctx.fpr[1] < 1.0));
        t.call("rand_real3_TT800");
        CHECK((t.ctx.fpr[1] > 0.0 && t.ctx.fpr[1] < 1.0));
        CHECK(i32(t.call("rand_int31_TT800")) >= 0);
    }
    const u32 key[] = {0x123, 0x234, 0x345, 0x456};
    for (u32 i = 0; i < 4; i++) ps_write32(b, kOut + 4 * i, key[i]);
    t.call("_init_by_array_TT800", kArr, kOut, 4);
    const u64 a1 = t.call("_rand_int32_TT800", kArr);
    t.call("_init_by_array_TT800", kArr, kOut, 4);
    CHECK(t.call("_rand_int32_TT800", kArr) == a1);
}

namespace {

void wput(u8* b, u32 at, std::u32string_view s)
{
    for (usize i = 0; i < s.size(); i++) ps_write32(b, at + 4 * static_cast<u32>(i), static_cast<u32>(s[i]));
    ps_write32(b, at + 4 * static_cast<u32>(s.size()), 0);
}

std::u32string wstr(u8* b, u32 at)
{
    std::u32string s;
    while (const u32 c = ps_read32(b, at + 4 * static_cast<u32>(s.size()))) s += static_cast<char32_t>(c);
    return s;
}

} // namespace

TEST_CASE("sys_libc: wide strings in the C locale", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    wput(b, kA, U"hello world");
    CHECK(t.call("wcslen", kA) == 11);
    CHECK(t.call("wcscpy", kOut, kA) == kOut);
    CHECK(wstr(b, kOut) == U"hello world");
    // wcsncpy pads with NULs to n and leaves the string unterminated when it fills n.
    std::memset(b + kOut, 0x7E, 64);
    t.call("wcsncpy", kOut, kA, 13);
    CHECK(wstr(b, kOut) == U"hello world");
    CHECK(ps_read32(b, kOut + 4 * 12) == 0);
    CHECK(ps_read32(b, kOut + 4 * 13) == 0x7E7E7E7Eu);
    // Comparisons give -1, 0 or +1, wide characters as unsigned.
    wput(b, kB, U"hello\U0010FFFF");
    CHECK(i32(t.call("wcscmp", kA, kA)) == 0);
    CHECK(i32(t.call("wcscmp", kA, kB)) == -1);
    CHECK(i32(t.call("wcscmp", kB, kA)) == 1);
    CHECK(i32(t.call("wcsncmp", kA, kB, 5)) == 0);
    ps_write32(b, kB, 0x80000000u);
    CHECK(i32(t.call("wcscmp", kB, kA)) == 1); // unsigned: 0x80000000 > 'h'
    CHECK(i32(t.call("wmemcmp", kB, kA, 1)) == 1);
    // Searches.
    wput(b, kA, U"hello world");
    CHECK(t.call("wcschr", kA, 'o') == kA + 4 * 4);
    CHECK(t.call("wcsrchr", kA, 'o') == kA + 4 * 7);
    CHECK(t.call("wcschr", kA, 0) == kA + 4 * 11);
    CHECK(t.call("wcschr", kA, 'z') == 0);
    CHECK(t.call("wmemchr", kA, 'w', 11) == kA + 4 * 6);
    wput(b, kB, U"wor");
    CHECK(t.call("wcsstr", kA, kB) == kA + 4 * 6);
    wput(b, kB, U"leh");
    CHECK(t.call("wcsspn", kA, kB) == 4);
    wput(b, kB, U"wol");
    CHECK(t.call("wcscspn", kA, kB) == 2);
    CHECK(t.call("wcspbrk", kA, kB) == kA + 4 * 2);
    // wcscat, wcsncat, wcsxfrm (the C locale's collation is the code order).
    wput(b, kOut, U"ab");
    wput(b, kB, U"cdef");
    t.call("wcscat", kOut, kB);
    CHECK(wstr(b, kOut) == U"abcdef");
    t.call("wcsncat", kOut, kB, 2);
    CHECK(wstr(b, kOut) == U"abcdefcd");
    CHECK(t.call("wcsxfrm", kOut, kB, 10) == 4);
    CHECK(wstr(b, kOut) == U"cdef");
    CHECK(t.call("wcsxfrm", kOut, kB, 4) == 4); // too small: not written
    CHECK(i32(t.call("wcscoll", kA, kB)) == 1);
    // wcstok keeps its place in the caller's pointer.
    wput(b, kA, U"  a,b,,c  ");
    wput(b, kB, U" ,");
    CHECK(wstr(b, static_cast<u32>(t.call("wcstok", kA, kB, kArr))) == U"a");
    CHECK(wstr(b, static_cast<u32>(t.call("wcstok", 0, kB, kArr))) == U"b");
    CHECK(wstr(b, static_cast<u32>(t.call("wcstok", 0, kB, kArr))) == U"c");
    CHECK(t.call("wcstok", 0, kB, kArr) == 0);
    // wmemset, wmemcpy, wmemmove (overlapping).
    t.call("wmemset", kOut, 0x12345678u, 4);
    CHECK(ps_read32(b, kOut + 12) == 0x12345678u);
    wput(b, kA, U"abcdef");
    t.call("wmemmove", kA + 4, kA, 5);
    CHECK(wstr(b, kA) == U"aabcde");
    t.call("wmemcpy", kOut, kA, 7);
    CHECK(wstr(b, kOut) == U"aabcde");
}

TEST_CASE("sys_libc: wide numbers, multibyte conversions and wide classes", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const auto err = [&] { return ps_read32(b, static_cast<u32>(t.call("_Geterrno"))); };
    // wcstol: the narrow forms' rules over wide characters; one past 127 ends the number.
    wput(b, kA, U"  -0x1fz");
    CHECK(i32(t.call("wcstol", kA, kOut, 0)) == -31);
    CHECK(ps_read32(b, kOut) == kA + 4 * 7);
    wput(b, kA, U"12٣");
    CHECK(t.call("wcstoul", kA, kOut, 10) == 12);
    CHECK(ps_read32(b, kOut) == kA + 8);
    wput(b, kA, U"99999999999");
    CHECK(i32(t.call("wcstol", kA, 0, 10)) == INT_MAX);
    CHECK(err() == 0x8001001Cu); // ERANGE (lv2's code)
    CHECK(t.call("wcstoll", kA, 0, 10) == 99999999999ull);
    wput(b, kA, U"xyz");
    CHECK(t.call("wcstol", kA, kOut, 10) == 0);
    CHECK(ps_read32(b, kOut) == kA);
    wput(b, kA, U"  -1.5e3xyz");
    t.call("wcstod", kA, kOut);
    CHECK(t.ctx.fpr[1] == -1500.0);
    CHECK(ps_read32(b, kOut) == kA + 4 * 8);
    wput(b, kA, U"0x1p-2");
    t.call("wcstof", kA, 0);
    CHECK(t.ctx.fpr[1] == 0.25);
    // The C locale's multibyte characters are bytes, each the wide character of its value.
    b[kA] = 0xC0;
    b[kA + 1] = 0;
    CHECK(t.call("mbrtowc", kOut, kA, 4, 0) == 1);
    CHECK(ps_read32(b, kOut) == 0xC0);
    CHECK(t.call("mbrtowc", kOut, kA + 1, 4, 0) == 0);
    CHECK(static_cast<u32>(t.call("mbrtowc", kOut, kA, 0, 0)) == 0xFFFFFFFEu); // (size_t)-2
    CHECK(i32(t.call("mblen", kA, 1)) == 1);
    CHECK(i32(t.call("mbtowc", 0, 0, 0)) == 0); // no shift states
    CHECK(t.call("wcrtomb", kOut, 0xE9, 0) == 1);
    CHECK(b[kOut] == 0xE9);
    CHECK(static_cast<u32>(t.call("wcrtomb", kOut, 0x100, 0)) == 0xFFFFFFFFu);
    CHECK(err() == 0x8001001Du); // EILSEQ
    CHECK(i32(t.call("wctomb", kOut, 0x41)) == 1);
    CHECK(i32(t.call("btowc", static_cast<u64>(-1))) == -1);
    CHECK(t.call("btowc", 0xFF) == 0xFF);
    CHECK(i32(t.call("wctob", 0x100)) == -1);
    CHECK(t.call("mbsinit", 0) == 1);
    put(b, kA, "h\xE9llo");
    CHECK(t.call("mbstowcs", 0, kA, 0) == 5);
    CHECK(t.call("mbstowcs", kOut, kA, 16) == 5);
    CHECK(wstr(b, kOut) == U"héllo");
    // mbsrtowcs moves the source pointer past what it converted, or to NULL after the NUL.
    ps_write32(b, kArr, kA);
    CHECK(t.call("mbsrtowcs", kOut, kArr, 2, 0) == 2);
    CHECK(ps_read32(b, kArr) == kA + 2);
    CHECK(t.call("mbsrtowcs", kOut, kArr, 16, 0) == 3);
    CHECK(ps_read32(b, kArr) == 0);
    wput(b, kA, U"wide");
    CHECK(t.call("wcstombs", kOut, kA, 16) == 4);
    CHECK(cstr(b, kOut) == "wide");
    CHECK(t.call("wcstombs", 0, kA, 0) == 4);
    wput(b, kA, U"aĀ");
    CHECK(static_cast<u32>(t.call("wcstombs", kOut, kA, 16)) == 0xFFFFFFFFu);
    ps_write32(b, kArr, kA);
    wput(b, kA, U"abc");
    CHECK(t.call("wcsrtombs", kOut, kArr, 2, 0) == 2);
    CHECK(ps_read32(b, kArr) == kA + 8);
    CHECK(t.call("_Getmbcurmax") == 1);
    // The classes are ASCII's; nothing past 127 is in one.
    for (u32 c = 0; c < 300; c++) {
        INFO(c);
        const bool ascii = c < 128;
        CHECK((t.call("iswalpha", c) != 0) == (ascii && std::isalpha(static_cast<int>(c))));
        CHECK((t.call("iswdigit", c) != 0) == (ascii && std::isdigit(static_cast<int>(c))));
        CHECK((t.call("iswspace", c) != 0) == (ascii && std::isspace(static_cast<int>(c))));
        CHECK((t.call("iswpunct", c) != 0) == (ascii && std::ispunct(static_cast<int>(c))));
        CHECK((t.call("iswcntrl", c) != 0) == (ascii && std::iscntrl(static_cast<int>(c))));
        CHECK((t.call("iswprint", c) != 0) == (ascii && std::isprint(static_cast<int>(c))));
        CHECK((t.call("iswblank", c) != 0) == (c == ' ' || c == '\t'));
        CHECK(t.call("towupper", c) == (c >= 'a' && c <= 'z' ? c - 32 : c));
    }
    put(b, kA, "xdigit");
    const u64 xdigit = t.call("wctype", kA);
    CHECK(xdigit != 0);
    CHECK(t.call("iswctype", 'f', xdigit) == 1);
    CHECK(t.call("iswctype", 'g', xdigit) == 0);
    put(b, kA, "bogus");
    CHECK(t.call("wctype", kA) == 0);
    put(b, kA, "toupper");
    CHECK(t.call("towctrans", 'q', t.call("wctrans", kA)) == 'Q');
    // Dinkumware's table of the class names, as wctype reads it.
    const u32 tab = static_cast<u32>(t.call("_Getpwctytab"));
    REQUIRE(tab != 0);
    CHECK(cstr(b, ps_read32(b, tab)) == "alnum");
    CHECK(t.call("iswctype", 'A', ps_read32(b, tab + 4)) == 1); // the first entry's class
    CHECK(t.call("iswctype", '!', ps_read32(b, tab + 4)) == 0);
}

TEST_CASE("sys_libc: malloc and its family on dlmalloc", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const auto err = [&] { return ps_read32(b, static_cast<u32>(t.call("_Geterrno"))); };
    // malloc: 16-byte aligned blocks of at least the size asked.
    const u32 p = static_cast<u32>(t.call("malloc", 100));
    REQUIRE(p != 0);
    CHECK((p & 15) == 0);
    CHECK(t.call("malloc_usable_size", p) >= 100);
    std::memset(b + p, 0xAB, 100);
    // calloc clears; an overflowing product fails with ENOMEM.
    const u32 c = static_cast<u32>(t.call("calloc", 10, 10));
    REQUIRE(c != 0);
    CHECK(std::all_of(b + c, b + c + 100, [](u8 x) { return x == 0; }));
    CHECK(t.call("calloc", 0x10000, 0x10000) == 0);
    CHECK(err() == 0x80010004u); // ENOMEM (lv2's)
    // realloc keeps the contents; realloc(NULL, n) is malloc.
    const u32 q = static_cast<u32>(t.call("realloc", p, 5000));
    REQUIRE(q != 0);
    CHECK(b[q + 99] == 0xAB);
    CHECK(t.call("realloc", q, 4000) == q); // shrinking a little keeps the block
    CHECK(t.call("realloc", 0, 10) != 0);
    // memalign (a boundary that is not a power of two rounds up), valloc, pvalloc, reallocalign.
    const u32 a = static_cast<u32>(t.call("memalign", 256, 10));
    CHECK((a & 255) == 0);
    CHECK((t.call("memalign", 48, 10) & 63) == 0);
    CHECK((t.call("valloc", 10) & 0xFFF) == 0);
    const u32 pv = static_cast<u32>(t.call("pvalloc", 10));
    CHECK((pv & 0xFFF) == 0);
    CHECK(t.call("malloc_usable_size", pv) >= 0x1000);
    b[a] = 0x5A;
    const u32 ra = static_cast<u32>(t.call("reallocalign", a, 1000, 1024));
    CHECK((ra & 1023) == 0);
    CHECK(b[ra] == 0x5A);
    // A request larger than the first 1 MB adds an mspace; mallinfo and malloc_footprint count it, malloc_trim gives
    // it back once it is empty.
    const u32 big = static_cast<u32>(t.call("malloc", 3u << 20));
    REQUIRE(big != 0);
    CHECK(t.call("malloc_footprint") >= (4u << 20));
    t.call("mallinfo", kOut);
    CHECK(ps_read32(b, kOut + 28) >= (3u << 20)); // uordblks
    CHECK(ps_read32(b, kOut + 20) >= (4u << 20)); // usmblks: the most the heap has had
    t.call("free", big);
    CHECK(t.call("malloc_trim", 0) == 1);
    CHECK(t.call("malloc_footprint") == (1u << 20));
    t.call("malloc_stats");
    // independent_comalloc: blocks of their own, their pointers in the caller's array.
    const u32 sizes[] = {8, 100, 3000};
    for (u32 i = 0; i < 3; i++) ps_write32(b, kA + 4 * i, sizes[i]);
    CHECK(t.call("independent_comalloc", 3, kA, kB) == kB);
    for (u32 i = 0; i < 3; i++) {
        CHECK(t.call("malloc_usable_size", ps_read32(b, kB + 4 * i)) >= sizes[i]);
        t.call("free", ps_read32(b, kB + 4 * i));
    }
    const u32 arr = static_cast<u32>(t.call("independent_calloc", 4, 32, 0));
    REQUIRE(arr != 0);
    CHECK(std::all_of(b + ps_read32(b, arr + 12), b + ps_read32(b, arr + 12) + 32, [](u8 x) { return x == 0; }));
    // strdup.
    put(b, kA, "duplicate me");
    const u32 d = static_cast<u32>(t.call("strdup", kA));
    REQUIRE(d != 0);
    CHECK(cstr(b, d) == "duplicate me");
    // mallopt: the granularity must be a power of two of a page at least; unknown parameters are refused.
    CHECK(t.call("mallopt", static_cast<u64>(-2), 0x200000) == 1);
    CHECK(t.call("mallopt", static_cast<u64>(-2), 0x1234) == 0);
    CHECK(t.call("mallopt", 7, 1) == 0);
    // Freeing what is not a block of the library's heaps is refused (logged), nothing changes.
    t.call("free", kA + 16);
    // _malloc_limit caps the heap: the request that would pass it fails.
    const u32 limit = HleProcess::current()->link_variable("sys_libc", 0x0B2E15ED);
    REQUIRE(limit != 0);
    ps_write32(b, limit, 0x300000);
    CHECK(t.call("malloc", 3u << 20) == 0);
    CHECK(t.call("malloc", 0x100000) != 0);
}

TEST_CASE("sys_libc: mspaces in the caller's memory and in lv2's", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kSpace = kData + 0x100000;
    const u32 msp = static_cast<u32>(t.call("mspace_create", kSpace, 0x10000));
    REQUIRE(msp != 0);
    CHECK(t.call("mspace_is_heap_empty", msp) == 1);
    const u32 m1 = static_cast<u32>(t.call("mspace_malloc", msp, 100));
    CHECK((m1 >= kSpace && m1 < kSpace + 0x10000));
    CHECK(t.call("mspace_is_heap_empty", msp) == 0);
    CHECK(t.call("mspace_malloc", msp, 0x20000) == 0); // it does not grow
    const u32 m2 = static_cast<u32>(t.call("mspace_calloc", msp, 4, 8));
    CHECK(std::all_of(b + m2, b + m2 + 32, [](u8 x) { return x == 0; }));
    const u32 m3 = static_cast<u32>(t.call("mspace_memalign", msp, 512, 8));
    CHECK((m3 & 511) == 0);
    b[m1] = 0x11;
    const u32 m4 = static_cast<u32>(t.call("mspace_realloc", msp, m1, 2000));
    CHECK((m4 >= kSpace && m4 < kSpace + 0x10000));
    CHECK(b[m4] == 0x11);
    CHECK(t.call("mspace_malloc_usable_size", m4) >= 2000);
    t.call("mspace_malloc_stats", msp);
    for (const u32 m : {m2, m3, m4}) t.call("mspace_free", msp, m);
    CHECK(t.call("mspace_is_heap_empty", msp) == 1);
    CHECK(t.call("mspace_destroy", msp) == 0); // the caller's memory: nothing to give back
    CHECK(t.call("mspace_malloc", msp, 8) == 0);  // gone (refused, logged)
    // With no base the mspace takes its memory from lv2, and destroy gives it back.
    const u32 lv2 = static_cast<u32>(t.call("mspace_create", 0, 0x20000));
    REQUIRE(lv2 != 0);
    CHECK(t.call("mspace_malloc", lv2, 0x10000) != 0);
    CHECK(t.call("mspace_destroy", lv2) == 0x20000);
    // Too small a memory for an mspace.
    CHECK(t.call("mspace_create", kSpace, 64) == 0);
}
