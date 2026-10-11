// sys_libc's printf as the SDK's C library has it (runtime/hle/src/libc_printf.hpp, D-905): its conversions pinned
// without a game (the values the game's copy gives, found by test_libc_format_game.cpp), and a timing of the engine.

#include <catch_amalgamated.hpp>

#include <bit>
#include <chrono>
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

constexpr u32 kFmt = kData + 0x1000, kStr = kData + 0x1400, kList = kData + 0x2000, kBuf = kData + 0x8000;

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

} // namespace

// printf of a few thousand mixed values through vsprintf, timed (run with test_hle "[.bench]").
TEST_CASE("sys_libc: printf's speed on mixed conversions", "[.bench][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Function& vsprintf = GuestThread::find("vsprintf");
    put(b, kFmt, "%d %5.2f %s|%x %e %g %-8u %c %lld %.3f %08.3e");
    put(b, kStr, "word");
    const usize kRecords = 4000;
    std::vector<std::array<u64, 11>> lists(kRecords);
    for (usize i = 0; i < kRecords; i++) {
        const double x = (static_cast<double>(i) - 2000.0) * 1.37e-3 * static_cast<double>(1 + (i % 17) * (i % 5));
        lists[i] = {static_cast<u64>(static_cast<s64>(i * 7919) - 10000), std::bit_cast<u64>(x), kStr, i * 2654435761u,
            std::bit_cast<u64>(x * 1e100), std::bit_cast<u64>(1.0 / (x + 0.5)), i, 'a' + i % 26,
            static_cast<u64>(static_cast<s64>(i) * -123456789012345ll), std::bit_cast<u64>(x * 1e-5),
            std::bit_cast<u64>(x * 3.25e7)};
    }
    usize total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < 5; round++) {
        for (usize i = 0; i < kRecords; i++) {
            for (usize k = 0; k < 11; k++) ps_write64(b, kList + 8 * static_cast<u32>(k), lists[i][k]);
            t.ctx.gpr[3] = kBuf;
            t.ctx.gpr[4] = kFmt;
            t.ctx.gpr[5] = kList;
            vsprintf.fn(t.ctx, b);
            total += static_cast<u32>(t.ctx.gpr[3]);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    WARN(std::format("printf: {} calls of 11 conversions, {} characters, {:.0f} us ({:.2f} us a call)", 5 * kRecords,
        total, us, us / (5.0 * static_cast<double>(kRecords))));
    CHECK(total > 0);
    CHECK(!cstr(b, kBuf).empty());
}

namespace {

struct Libc {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    u32 heap = kData + 0x10000;

    u32 alloc(u32 n)
    {
        const u32 a = (heap + 15) & ~15u;
        heap = a + n;
        std::memset(b + a, 0, n);
        return a;
    }
    u32 str(std::string_view s)
    {
        const u32 a = alloc(static_cast<u32>(s.size()) + 1);
        put(b, a, s);
        return a;
    }
    u32 wstr(std::u32string_view s)
    {
        const u32 a = alloc(4 * static_cast<u32>(s.size()) + 4);
        for (usize i = 0; i < s.size(); i++) ps_write32(b, a + 4 * static_cast<u32>(i), static_cast<u32>(s[i]));
        return a;
    }
    // vsprintf of the format with these 64-bit slots; the text, and the count in *count.
    std::string vsprintf(std::string_view fmt, std::initializer_list<u64> args, s32* count = nullptr)
    {
        const u32 fm = str(fmt), list = alloc(8 * static_cast<u32>(args.size()) + 16), buf = alloc(0x2000);
        u32 a = list;
        for (const u64 x : args) {
            ps_write64(b, a, x);
            a += 8;
        }
        const s32 n = static_cast<s32>(t.call("vsprintf", buf, fm, list));
        if (count) *count = n;
        return cstr(b, buf);
    }
    std::string vsprintf_list(std::string_view fmt, u32 list)
    {
        const u32 fm = str(fmt), buf = alloc(0x2000);
        t.call("vsprintf", buf, fm, list);
        return cstr(b, buf);
    }
};

u64 dbl(double d) { return std::bit_cast<u64>(d); }
constexpr double kInf = std::numeric_limits<double>::infinity(), kNan = std::numeric_limits<double>::quiet_NaN();

} // namespace

TEST_CASE("sys_libc: printf's floating point is the SDK's", "[hle][libc]")
{
    Libc c;
    // _Ldtob: digits from double arithmetic (19 at most), the first dropped digit rounding half up
    CHECK(c.vsprintf("%.0f|%.1f|%.2f|%.0f|%.0f", {dbl(0.5), dbl(0.25), dbl(0.125), dbl(1.5), dbl(2.5)}) == "1|0.3|0.13|2|3");
    CHECK(c.vsprintf("%.20f", {dbl(0.1)}) == "0.10000000000000000000");
    CHECK(c.vsprintf("%.17e|%.25e", {dbl(0.1), dbl(1.0 / 3)}) == "1.00000000000000000e-01|3.3333333333333330301000000e-01");
    CHECK(c.vsprintf("%.20f", {dbl(1.0 / 3)}) == "0.33333333333333330301");
    CHECK(c.vsprintf("%.1f|%.3f|%g|%G", {dbl(1e20), dbl(123456789.125), dbl(1e-5), dbl(1e20)}) ==
          "100000000000000000000.0|123456789.125|1e-05|1E+20");
    CHECK(c.vsprintf("%e|%E|%.3e|%10.2e", {dbl(1e300), dbl(-1234.5678), dbl(2.5e-10), dbl(99.95)}) ==
          "1.000000e+300|-1.234568E+03|2.500e-10|  1.00e+02");
    CHECK(c.vsprintf("%g|%g|%g|%.3g|%#g|%g", {dbl(0.0001), dbl(100000.0), dbl(1000000.0), dbl(2.675), dbl(1.0), dbl(0.0)}) ==
          "0.0001|100000|1e+06|2.68|1.00000|0");
    // a and A: exact, as few digits as the value needs; a precision rounds half up in hex
    CHECK(c.vsprintf("%a|%A|%.3a|%.0a|%a|%a", {dbl(1.0), dbl(-0.75), dbl(1.0), dbl(1.5), dbl(0.0), dbl(5e-324)}) ==
          "0x1p+0|-0X1.8P-1|0x1.000p+0|0x2p+0|0x0p+0|0x1p-1074");
    // special values, signs, flags
    CHECK(c.vsprintf("%f|%F|%e|%E|%5.1f|%-6f|", {dbl(kInf), dbl(-kInf), dbl(kNan), dbl(-kNan), dbl(kInf), dbl(kNan)}) ==
          "inf|-INF|nan|-NAN|  inf|nan   |");
    CHECK(c.vsprintf("%08.3f|%+.2e|% f|%#.0f|%-8.1f|%+08.2f", {dbl(-2.5), dbl(1234.5678), dbl(1.0), dbl(1.0), dbl(3.25), dbl(3.14159)}) ==
          "-002.500|+1.23e+03| 1.000000|1.|3.3     |+0003.14");
    CHECK(c.vsprintf("%f|%f", {dbl(-0.0), dbl(4.9406564584124654e-324)}) == "-0.000000|0.000000");
    CHECK(c.vsprintf("%.3Lf|%lf", {dbl(2.0005), dbl(7.0)}) == "2.001|7.000000");
}

TEST_CASE("sys_libc: printf's integers, strings and the rest are the SDK's", "[hle][libc]")
{
    Libc c;
    // L is 32 bits for an integer, ll and j 64; q is no qualifier (an unknown conversion); "% Ld" has no space
    CHECK(c.vsprintf("%Ld|%lld|%qd|%jd|% Ld|% d|% ld", {0x100000001ull, 0x100000001ull, 0x100000001ull, 5, 5, 5}) ==
          "1|4294967297|qd|4294967297|5| 5| 5");
    // %p is the hex digits alone; '#' adds nothing
    CHECK(c.vsprintf("%p|%#p|%10p|%-6p|%08p", {0x1234, 0xABCD, 0x1F, 0x2E, 0x3D}) == "1234|abcd|        1f|2e    |0000003d");
    // a conversion character the library has none for prints itself, padded; so does %%
    CHECK(c.vsprintf("%k|%5k|%-3k|%5%|%-4%|%C", {}) == "k|    k|k  |    %|%   |C");
    // C99's corners
    CHECK(c.vsprintf("%#o|%.0d|%#x|%#.3o|%+.0d|%5.0x|", {0, 0, 0, 8, 0, 0}) == "0||0|010|+|     |");
    CHECK(c.vsprintf("%hhd|%hd|%hhu|%hu|%td|%zu", {0x1FF, 0x18000, 0x1FF, 0x18000, 0xFFFFFFFF, 0xFFFFFFFF}) ==
          "-1|-32768|255|32768|-1|4294967295");
    CHECK(c.vsprintf("%*d|%-*d|%.*d|%*.*s|", {static_cast<u64>(-6), 42, 4, 7, 5, 3, 3, 2, c.str("abc")}) ==
          "42    |7   |00003| ab|");
    // %s of a null pointer is "(null)" whatever the precision
    CHECK(c.vsprintf("%s|%.2s|%8s|", {0, 0, 0}) == "(null)|(null)|  (null)|");
    // %ls and %lc: UTF-8 (sys_libc's 32-bit wchar_t goes on to six bytes); a 64-byte buffer for '-' fields, put out
    // without padding when it fills
    CHECK(c.vsprintf("%ls|%lc|%lc|%.3ls|", {c.wstr(U"é€"), 0x10000, 0x7FFFFFFF, c.wstr(U"é€")}) ==
          "\xc3\xa9\xe2\x82\xac|\xf0\x90\x80\x80|\xfd\xbf\xbf\xbf\xbf\xbf|\xc3\xa9|");
    CHECK(c.vsprintf("%-10ls|%-70ls|", {c.wstr(U"abc"), c.wstr(std::u32string(66, U'w'))}) ==
          "abc       |" + std::string(66, 'w') + "|");
    CHECK(c.vsprintf("%10ls|", {c.wstr(U"abc")}) == "       abc|");
    s32 n = 0;
    CHECK(c.vsprintf("a%lcb", {0x80000000ull}, &n) == "a");
    CHECK(n == -1); // no multibyte form: EOF, what came before written (no NUL: the buffer holds it from before)
    // %n stores the count so far, as wide as its length says, and is not padded
    const u32 targets = c.alloc(32);
    std::memset(c.b + targets, 0xAA, 32);
    CHECK(c.vsprintf("abc%n|%5n|%hhn%lln", {targets, targets + 4, targets + 8, targets + 16}) == "abc||");
    CHECK(ps_read32(c.b, targets) == 3);
    CHECK(ps_read32(c.b, targets + 4) == 4);
    CHECK(c.b[targets + 8] == 5);
    CHECK(c.b[targets + 9] == 0xAA);
    CHECK(ps_read64(c.b, targets + 16) == 5);
}

TEST_CASE("sys_libc: printf's vectors are Sony's", "[hle][libc]")
{
    Libc c;
    const auto vlist = [&](std::initializer_list<u32> words, u64 before = 0, bool lead = false) {
        const u32 l = c.alloc(64);
        u32 a = l;
        if (lead) {
            ps_write64(c.b, a, before);
            a += 16; // the vector at the next 16-byte boundary
        }
        for (const u32 w : words) {
            ps_write32(c.b, a, w);
            a += 4;
        }
        return l;
    };
    CHECK(c.vsprintf_list("%vd", vlist({1, 0xFFFFFFFE, 3, 4})) == "1 -2 3 4");
    CHECK(c.vsprintf_list("%d:%vlx", vlist({0xA, 0xB, 0xC, 0xD}, 7, true)) == "7:a b c d");
    CHECK(c.vsprintf_list("%,vx", vlist({0xA, 0xB, 0xC, 0xD})) == "a,b,c,d");
    CHECK(c.vsprintf_list("%;vu|", vlist({0xA, 0xB, 0xC, 0xD})) == "10;11;12;13|");
    CHECK(c.vsprintf_list("%:vo|", vlist({0xA, 0xB, 0xC, 0xD})) == "12:13:14:15|");
    CHECK(c.vsprintf_list("%_vX|", vlist({0xA, 0xB, 0xC, 0xD})) == "A_B_C_D|");
    CHECK(c.vsprintf_list("%vhd", vlist({0x0001FFFF, 0x00020003, 0x80007FFF, 0})) == "1 -1 2 3 -32768 32767 0 0");
    CHECK(c.vsprintf_list("%vhhu", vlist({0x01020304, 0xFF000000, 0, 0x7F80FEFF})) == "1 2 3 4 255 0 0 0 0 0 0 0 127 128 254 255");
    CHECK(c.vsprintf_list("%vc", vlist({0x41424344, 0x45464748, 0x494A4B4C, 0x4D4E4F50})) == "ABCDEFGHIJKLMNOP");
    CHECK(c.vsprintf_list("%#vx", vlist({0, 1, 0xFF, 0x10})) == "0 0x1 0xff 0x10");
    CHECK(c.vsprintf_list("%+vd|", vlist({1, 0xFFFFFFFF, 0, 2})) == "+1 -1 +0 +2|");
    CHECK(c.vsprintf_list("% ,vd|", vlist({1, 0xFFFFFFFF, 0, 2})) == "1 ,-1 ,0 ,2|"); // a space, then the comma
    CHECK(c.vsprintf_list("%4vd|", vlist({1, 22, 333, 4444})) == "   1  22 333 4444|"); // the separator in the width
    // floats: four (v) or two doubles (vL); the space between them is also each one's sign space
    CHECK(c.vsprintf_list("%.1vf", vlist({std::bit_cast<u32>(1.5f), std::bit_cast<u32>(-0.25f), std::bit_cast<u32>(2.0f),
                                    std::bit_cast<u32>(0.0f)})) == " 1.5 -0.3  2.0  0.0");
    CHECK(c.vsprintf_list("%,.1vf", vlist({std::bit_cast<u32>(1.5f), std::bit_cast<u32>(-0.25f), std::bit_cast<u32>(2.0f),
                                     std::bit_cast<u32>(0.0f)})) == "1.5,-0.3,2.0,0.0");
    CHECK(c.vsprintf_list("%vLg", vlist({0x40040000, 0, 0xBFF00000, 0})) == " 2.5 -1");
    // "%xc" / "%cx": sixteen bytes
    CHECK(c.vsprintf_list("%xc|", vlist({0x00010A0F, 0x10FF8000, 0, 0x01020304})) == "0 1 a f 10 ff 80 0 0 0 0 0 1 2 3 4|");
    CHECK(c.vsprintf_list("%cX", vlist({0x00010A0F, 0x10FF8000, 0, 0x01020304})) == "0 1 A F 10 FF 80 0 0 0 0 0 1 2 3 4");
}

TEST_CASE("sys_libc: the wide printf is the SDK's", "[hle][libc]")
{
    Libc c;
    const auto swprintf = [&](std::u32string_view fmt, std::initializer_list<u64> args, u32 n, s32* count) {
        const u32 f = c.wstr(fmt), list = c.alloc(8 * static_cast<u32>(args.size()) + 8), buf = c.alloc(4 * 256);
        u32 a = list;
        for (const u64 x : args) {
            ps_write64(c.b, a, x);
            a += 8;
        }
        *count = static_cast<s32>(c.t.call("vswprintf", buf, n, f, list));
        std::u32string s;
        for (u32 i = 0; i < 256 && ps_read32(c.b, buf + 4 * i); i++) s += static_cast<char32_t>(ps_read32(c.b, buf + 4 * i));
        return s;
    };
    s32 n = 0;
    // %n stores nothing and its field is padded; %c of a byte past 0x7F has no wide character (UTF-8): EOF
    CHECK(swprintf(U"<%5n|%d>", {c.alloc(8), 5}, 256, &n) == U"<     |5>");
    CHECK(n == 9);
    CHECK(swprintf(U"%c%c", {'A', 0xE9}, 256, &n) == U"A");
    CHECK(n == -1);
    CHECK(swprintf(U"%s|%5.1s|%ls", {c.str("\xc3\xa9t\xc3\xa9"), c.str("\xe2\x82\xac!"), c.wstr(U"w€")}, 256, &n) ==
          U"été|    €|w€");
    // swprintf: negative when a piece does not fit, the pieces before it written
    CHECK(swprintf(U"%d-%d", {123, 456}, 6, &n) == U"123-4");
    CHECK(n == -1);
    CHECK(swprintf(U"%.0f|%p|%5%|%Ld", {std::bit_cast<u64>(0.5), 0xBEEF, 0x100000002ull}, 256, &n) == U"1|beef|    %|2");
}

TEST_CASE("sys_libc: _Printf's pieces, and the SPU printf on a raw SPU", "[hle][libc]")
{
    Libc c;
    // pfn(log, text, n): appends n and the text to a log, returning the log (NULL from the given call on)
    const u32 code = c.alloc(0x60);
    const u32 words[] = {0x2C030000u, 0x41820050u, 0x81030004u, 0x2C080000u, 0x41820044u, 0x3908FFFFu, 0x91030004u,
        0x80C30000u, 0x90A60000u, 0x38C60004u, 0x54A9003Eu, 0x2C090000u, 0x4182001Cu, 0x7D2903A6u, 0x88E40000u,
        0x98E60000u, 0x38840001u, 0x38C60001u, 0x4200FFF0u, 0x90C30000u, 0x4E800020u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(c.b, code + 4 * i, words[i]);
    const u32 pfn = c.alloc(8);
    ps_write32(c.b, pfn, code);
    const u32 log = c.alloc(0x1000);
    const auto pieces = [&]() {
        std::vector<std::string> out;
        for (u32 a = log + 8; a < ps_read32(c.b, log);) {
            const u32 n = ps_read32(c.b, a);
            out.emplace_back(reinterpret_cast<const char*>(c.b + a + 4), n);
            a += 4 + n;
        }
        return out;
    };
    const auto printf = [&](std::string_view fmt, std::initializer_list<u64> args, u32 calls) {
        ps_write32(c.b, log, log + 8);
        ps_write32(c.b, log + 4, calls);
        const u32 list = c.alloc(8 * static_cast<u32>(args.size()) + 8);
        u32 a = list;
        for (const u64 x : args) {
            ps_write64(c.b, a, x);
            a += 8;
        }
        return static_cast<s32>(c.t.call("_Printf", pfn, log, c.str(fmt), list));
    };
    // padding 32 characters a call, the sign and the digits each a piece of their own, literal text as one
    CHECK(printf("ab%70dcd", {static_cast<u64>(-5)}, 0x7FFFFFFF) == 74);
    CHECK(pieces() == std::vector<std::string>{"ab", std::string(32, ' '), std::string(32, ' '), std::string(4, ' '), "-",
                          "5", "cd"});
    CHECK(printf("%-6.3f|", {std::bit_cast<u64>(2.5)}, 0x7FFFFFFF) == 7);
    CHECK(pieces() == std::vector<std::string>{"2.5", "00", " ", "|"});
    // NULL from the output function: EOF
    CHECK(printf("ab%5dcd", {7}, 2) == -1);
    CHECK(pieces() == std::vector<std::string>{"ab", "    "});

    // A raw SPU's local store is guest memory: its printf reads the format and the arguments there, every conversion
    // taking a quadword (%% too).
    constexpr u32 kRaw = 0xE0000000u;
    REQUIRE(c.f.mem.commit(kRaw, 0x40000));
    const auto ls = [&](u32 at, std::string_view s) {
        std::memcpy(c.b + kRaw + at, s.data(), s.size());
        c.b[kRaw + at + s.size()] = 0;
    };
    ls(0x100, "<%d %% %d|%s|%5.1f|%x>");
    ls(0x200, "raw");
    const u32 q = kRaw + 0x400;
    std::memset(c.b + q, 0, 0x100);
    ps_write32(c.b, q, 0x100);
    ps_write32(c.b, q + 0x10, 1);
    ps_write32(c.b, q + 0x20, 2); // taken by %%
    ps_write32(c.b, q + 0x30, 3);
    ps_write32(c.b, q + 0x40, 0x200);
    ps_write64(c.b, q + 0x50, std::bit_cast<u64>(2.25));
    ps_write32(c.b, q + 0x60, 0xBEEF);
    const u32 buf = c.alloc(64);
    CHECK(static_cast<s32>(c.t.call("spu_raw_sprintf", buf, 0, 0x400)) == 22);
    CHECK(cstr(c.b, buf) == "<1 % 3|raw|  2.3|beef>");
    CHECK(static_cast<s32>(c.t.call("spu_raw_snprintf", buf, 5, 0, 0x400)) == 22);
    CHECK(cstr(c.b, buf) == "<1 %");
    ps_write32(c.b, log, log + 8);
    ps_write32(c.b, log + 4, 0x7FFFFFFF);
    CHECK(static_cast<s32>(c.t.call("__raw_spu_printf", pfn, log, 0, 0x400)) == 22);
    const auto p = pieces();
    REQUIRE(p.size() > 3);
    CHECK(p[0] == "<"); // literal characters one at a time
    CHECK(p[1] == "1");
    CHECK(p[2] == " ");
}
