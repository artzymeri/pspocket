// sys_libc's printf compared with a game's statically linked copy of the SDK's C library (D-905; the fixture is
// game_executable.hpp): _Printf, _WPrintf and their steps (_Putfld, _Puttxt, _Putstr, _Litob, _Ldtob, _Genld and the
// wide forms), sprintf and its kin, and the SPU printf on a running SPU thread. Every call is made on the game's
// function and on pspocket's with the same inputs; their answers, what their output function received piece by
// piece, and the memory they wrote are compared. The game's library was built with a 16-bit wchar_t (sys_libc's is 32
// bits, D-512): its wide functions get the same text as 16-bit characters. test_hle "[gamelibc]", skipped without the
// game's executable and its map.

#include <catch_amalgamated.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <random>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#include "game_executable.hpp"
#include "spu_program.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"

using namespace pspocket;
using pspocket::test::GameExecutable;

namespace {

// An output function pfn(log, text, n) appending [n][the n characters] to a log ({cursor, calls left, data...}),
// failing (NULL) once the calls left run out, and for a NULL log (the library calls on with the NULL it got where it
// ignores a failure). `shift`: the character size (0 bytes, 1 halfwords, 2 words).
u32 logging_pfn(GameExecutable& g, u32 shift)
{
    const u32 slwi = 0x54A90000u | (shift << 11) | ((31 - shift) << 1);
    return g.guest_code({0x2C030000u, 0x41820050u, 0x81030004u, 0x2C080000u, 0x41820044u, 0x3908FFFFu, 0x91030004u,
        0x80C30000u, 0x90A60000u, 0x38C60004u, slwi, 0x2C090000u, 0x4182001Cu, 0x7D2903A6u, 0x88E40000u, 0x98E60000u,
        0x38840001u, 0x38C60001u, 0x4200FFF0u, 0x90C30000u, 0x4E800020u, 0x38600000u, 0x4E800020u});
}

struct Log {
    u32 at = 0;
    u32 size = 0;
};

Log new_log(GameExecutable& g, u32 size, u32 calls = 0x7FFFFFFF)
{
    Log l{g.alloc(size + 8), size};
    ps_write32(g.b, l.at, l.at + 8);
    ps_write32(g.b, l.at + 4, calls);
    return l;
}

void reset_log(GameExecutable& g, const Log& l, u32 calls = 0x7FFFFFFF)
{
    ps_write32(g.b, l.at, l.at + 8);
    ps_write32(g.b, l.at + 4, calls);
}

// The pieces logged, each as its characters (unit bytes each).
std::vector<std::u32string> pieces(GameExecutable& g, const Log& l, u32 unit)
{
    std::vector<std::u32string> out;
    const u32 end = ps_read32(g.b, l.at);
    for (u32 a = l.at + 8; a + 4 <= end;) {
        const u32 n = ps_read32(g.b, a);
        a += 4;
        std::u32string s;
        for (u32 i = 0; i < n; i++, a += unit)
            s += static_cast<char32_t>(unit == 1 ? g.b[a] : unit == 2 ? ps_read16(g.b, a) : ps_read32(g.b, a));
        out.push_back(std::move(s));
    }
    return out;
}

std::string show(const std::vector<std::u32string>& p)
{
    std::string s;
    for (const auto& x : p) {
        s += '[';
        for (const char32_t c : x) s += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        s += ']';
    }
    return s;
}

std::string narrow(const std::u32string& s)
{
    std::string r;
    for (const char32_t c : s) r += static_cast<char>(c);
    return r;
}

// An argument of a va_list: a 64-bit slot, or a vector (at a 16-byte boundary).
struct Arg {
    u64 v = 0;
    bool vec = false;
    std::array<u8, 16> q{};
};
Arg I(u64 v) { return Arg{v, false, {}}; }
Arg D(double d) { return Arg{std::bit_cast<u64>(d), false, {}}; }
Arg V(std::array<u8, 16> q) { return Arg{0, true, q}; }

u32 list_of(GameExecutable& g, const std::vector<Arg>& args)
{
    const u32 base = g.alloc(16 * static_cast<u32>(args.size()) + 32);
    u32 a = base;
    for (const Arg& x : args) {
        if (x.vec) {
            a = (a + 15) & ~15u;
            std::memcpy(g.b + a, x.q.data(), 16);
            a += 16;
        } else {
            ps_write64(g.b, a, x.v);
            a += 8;
        }
    }
    return base;
}

u32 wstr16(GameExecutable& g, std::u32string_view s)
{
    const u32 a = g.alloc(2 * static_cast<u32>(s.size()) + 2);
    for (usize i = 0; i < s.size(); i++) ps_write16(g.b, a + 2 * static_cast<u32>(i), static_cast<u16>(s[i]));
    return a;
}
u32 wstr32(GameExecutable& g, std::u32string_view s)
{
    const u32 a = g.alloc(4 * static_cast<u32>(s.size()) + 4);
    for (usize i = 0; i < s.size(); i++) ps_write32(g.b, a + 4 * static_cast<u32>(i), static_cast<u32>(s[i]));
    return a;
}

// Differences collected (the first few shown).
struct Diffs {
    u64 compared = 0;
    std::vector<std::string> list;
    void add(std::string s)
    {
        if (list.size() < 400) list.push_back(std::move(s));
        else if (list.size() == 400) list.push_back("...");
    }
    void report(const char* what)
    {
        for (usize i = 0; i < list.size() && i < 60; i++) UNSCOPED_INFO(list[i]);
        WARN(std::format("{}: {} calls compared, {} differences", what, compared, list.size()));
        CHECK(list.empty());
    }
};

// One narrow _Printf compared: the format, the arguments (pointers to %n targets: `targets` of them, 8 bytes each,
// passed first-to-last as the arguments marked kTarget).
constexpr u64 kTarget = 0xFEEDF00D00000000ull;

struct PrintfRig {
    GameExecutable& g;
    u32 pfn;
    Log lg, lo;
    PrintfRig(GameExecutable& game) : g(game), pfn(logging_pfn(game, 0)), lg(new_log(game, 0x40000)), lo(new_log(game, 0x40000)) {}

    void compare(Diffs& d, const std::string& fmt, std::vector<Arg> args, u32 calls = 0x7FFFFFFF)
    {
        const u32 f = g.cstring(fmt);
        // %n targets: separate areas for each side
        std::vector<Arg> ga = args, oa = args;
        u32 tg = 0, to = 0;
        usize nt = 0;
        for (const Arg& a : args)
            if (!a.vec && (a.v & 0xFFFFFFFF00000000ull) == kTarget) nt++;
        if (nt) {
            tg = g.alloc(8 * static_cast<u32>(nt));
            to = g.alloc(8 * static_cast<u32>(nt));
            std::memset(g.b + tg, 0xAA, 8 * nt);
            std::memset(g.b + to, 0xAA, 8 * nt);
            usize k = 0;
            for (usize i = 0; i < args.size(); i++)
                if (!args[i].vec && (args[i].v & 0xFFFFFFFF00000000ull) == kTarget) {
                    ga[i].v = tg + 8 * static_cast<u32>(k);
                    oa[i].v = to + 8 * static_cast<u32>(k);
                    k++;
                }
        }
        const u32 apg = list_of(g, ga), apo = list_of(g, oa);
        reset_log(g, lg, calls);
        reset_log(g, lo, calls);
        const s32 rg = static_cast<s32>(g.call_game("_Printf", {pfn, lg.at, f, apg}));
        const s32 ro = static_cast<s32>(g.call_own("sys_libc", "_Printf", {pfn, lo.at, f, apo}));
        const auto pg = pieces(g, lg, 1), po = pieces(g, lo, 1);
        d.compared++;
        if (rg != ro || pg != po || (nt && g.bytes(tg, 8 * static_cast<u32>(nt)) != g.bytes(to, 8 * static_cast<u32>(nt))))
            d.add(std::format("_Printf(\"{}\"): game {} {} / own {} {}", fmt, rg, show(pg), ro, show(po)));
    }
};

std::array<u8, 16> q_bytes(std::initializer_list<u8> b)
{
    std::array<u8, 16> q{};
    usize i = 0;
    for (const u8 x : b) q[i++] = x;
    return q;
}
std::array<u8, 16> q_words(u32 a, u32 b, u32 c, u32 e)
{
    std::array<u8, 16> q{};
    ps_write32(q.data(), 0, a);
    ps_write32(q.data(), 4, b);
    ps_write32(q.data(), 8, c);
    ps_write32(q.data(), 12, e);
    return q;
}
std::array<u8, 16> q_floats(float a, float b, float c, float e)
{
    return q_words(std::bit_cast<u32>(a), std::bit_cast<u32>(b), std::bit_cast<u32>(c), std::bit_cast<u32>(e));
}
std::array<u8, 16> q_doubles(double a, double b)
{
    std::array<u8, 16> q{};
    ps_write64(q.data(), 0, std::bit_cast<u64>(a));
    ps_write64(q.data(), 8, std::bit_cast<u64>(b));
    return q;
}

// Doubles that probe the conversion: special values, ties, powers of ten and two, denormals, and random bit patterns.
std::vector<double> probe_doubles(u32 randoms)
{
    std::vector<double> v = {0.0, -0.0, 0.5, 1.5, 2.5, 0.25, 0.125, 0.375, 0.1, 0.2, 0.3, 1.0 / 3, 2.0 / 3, 1e-5,
        5e-5, 0.05, 0.005, 0.0005, 0.95, 9.5, 99.5, 99.95, 999.5, 1.25, 1.35, 1.45, 2.675, 1e-7, 1e-10, 1e-20,
        1e-300, 1e-307, 1e-308, 2.2250738585072014e-308, 2.2250738585072009e-308, 4.9406564584124654e-324,
        1.7976931348623157e308, 8.98846567431158e307, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23,
        123456789.125, 0.1 + 0.2, 3.141592653589793, 2.718281828459045, 1.7976931348623157e300, 6.02214076e23,
        1.602176634e-19, 4294967295.0, 4294967296.0, 9007199254740992.0, 9007199254740993.0, 65536.0, 1024.0,
        0.000123456, 123.456, -123.456, -1e-5, -0.5, -1.5, -2.5, 7.0, 77.77, 1e100, 1e-100, 1e200, 1e-200,
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::quiet_NaN(),
        std::bit_cast<double>(0x7FF0000000000001ull), std::bit_cast<double>(0x000FFFFFFFFFFFFFull),
        std::bit_cast<double>(0x8000000000000001ull), 0.999999, 0.9999995, 9.9999995, 99999.95, 0.0000000001234};
    std::mt19937_64 rng(905);
    for (u32 i = 0; i < randoms; i++) {
        u64 bits = rng();
        if (i % 3 == 0) {
            // a moderate exponent: values printed with many digits
            const u64 e = 1023 - 30 + rng() % 70;
            bits = (bits & 0x800FFFFFFFFFFFFFull) | (e << 52);
        }
        v.push_back(std::bit_cast<double>(bits));
    }
    return v;
}

const std::vector<u64> kInts = {0, 1, static_cast<u64>(-1), 42, static_cast<u64>(-42), 0x7FFFFFFF, 0xFFFFFFFF80000000ull,
    0xFFFFFFFF, 0x80000000, 255, 256, static_cast<u64>(-129), 65535, 0x123456789ABCDEF0ull, 0x8000000000000000ull,
    0xFFFFFFFFFFFFFFFFull, 1000000, 0x10000, 0xFFFF8000, 7, 8, 9, 10, 100, 0x7FFFFFFFFFFFFFFFull};

} // namespace

TEST_CASE("sys_libc's _Printf gives the game's: integers, characters, strings, pointers", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    PrintfRig rig(g);
    Diffs d;
    const char* flags[] = {"", "-", "+", " ", "#", "0", "-0", "+0", " 0", "#0", "-#", "+ ", " #", "+#0", "-+ #0"};
    const char* widths[] = {"", "1", "3", "8", "20"};
    const char* precs[] = {"", ".", ".0", ".1", ".3", ".8", ".25"};
    const char* quals[] = {"", "hh", "h", "l", "ll", "j", "t", "z", "L", "q"};
    const char* convs = "diuoxX";
    std::mt19937 rng(9051);
    for (u32 n = 0; n < 1600; n++) {
        const std::string f = std::string("<%") + flags[rng() % std::size(flags)] + widths[rng() % std::size(widths)] +
                              precs[rng() % std::size(precs)] + quals[rng() % std::size(quals)] + convs[rng() % 6] + ">";
        rig.compare(d, f, {I(kInts[rng() % kInts.size()])});
    }
    // * for the width and precision, negative ones too
    for (const s32 w : {0, 1, 5, -5, 12, -12, 0x7FFFFFFF - 0x7FFFFFF0})
        for (const s32 p : {-1, 0, 2, 7, -3})
            for (const char* f : {"%*.*d", "%-*.*x", "%0*.*u", "%+*.*i", "%#*.*o", "%*.*s", "%*.*c", "%*.*%"}) {
                const u32 s = g.cstring("text");
                rig.compare(d, f, {I(static_cast<u64>(static_cast<s64>(w))), I(static_cast<u64>(static_cast<s64>(p))),
                                      I(std::string_view(f).back() == 's' ? s : 12345)});
            }
    // digits in the width past INT_MAX
    rig.compare(d, "%4294967301d|", {I(7)});
    rig.compare(d, "%.4294967298d|", {I(7)});
    // characters, %%, unknown conversions (padded), %p
    for (const char* f : {"%c", "%5c", "%-5c", "%05c", "%%", "%5%", "%-5%", "%k", "%5k", "%-3k", "%C", "%y", "%v",
             "%lv", "%h", "%p", "%#p", "%10p", "%-10p", "%010p", "%.10p", "%+p", "%hhc", "%lc|", "%5lc|", "%-4lc|"})
        for (const u64 v : {u64{'A'}, u64{0}, u64{0xFF41}, u64{0x5678}, u64{0xE9}, u64{0x7FF}})
            rig.compare(d, std::string("<") + f + ">", {I(v)});
    // strings: widths, precisions, a null pointer, long ones
    const std::string longs(300, 'x');
    for (const char* f : {"%s", "%10s", "%-10s", "%.3s", "%10.3s", "%-10.3s", "%.0s", "%.10s", "%5.1s", "%05s",
             "%S", "%10S", "%.2S", "%ls", "%-12ls", "%.4ls", "%70ls", "%-70ls", "%3.70ls"})
        for (const std::string& s : {std::string(""), std::string("hello"), std::string("ab\x7f\xe9"), longs}) {
            const bool wide = std::string_view(f).find('S') != std::string_view::npos ||
                              std::string_view(f).find('l') != std::string_view::npos;
            if (!wide) {
                rig.compare(d, f, {I(g.cstring(s))});
            } else {
                // The game's wide strings are 16-bit: compared through a test of their own below; here only text
                // both widths spell the same way, with NUL-free 8-bit characters, is used, through %ls of empty text.
                (void)s;
            }
        }
    for (const char* f : {"%s|", "%10s|", "%.2s|", "%-8.1s|"}) rig.compare(d, f, {I(0)});
    // %n of every length, its stored count
    for (const char* f : {"abc%n", "ab%hhn|", "abcd%hn", "%ln", "x%lln", "%jn", "%zn", "%tn", "%Ln", "%qn", "%5n|", "%-5n"})
        rig.compare(d, std::string("12345") + f, {I(kTarget)});
    // several conversions in one format
    rig.compare(d, "%d %s %x %c %% %u|%5.2s|%-6d|%+.3d", {I(1), I(g.cstring("two")), I(3), I('4'), I(5), I(g.cstring("six")), I(7), I(8)});
    // the output function failing (NULL) at each call in turn: EOF, and what went out before
    for (u32 calls = 0; calls < 12; calls++) {
        rig.compare(d, "ab%5dcd%-40s|%%%c", {I(42), I(g.cstring("text")), I('z')}, calls);
        rig.compare(d, "%70ls|%lc", {I(0), I('q')}, calls);
        rig.compare(d, "%.3f %40e", {D(2.5), D(1e-10)}, calls);
    }
    d.report("_Printf integers, characters, strings");
}

TEST_CASE("sys_libc's _Printf gives the game's: floating point", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    PrintfRig rig(g);
    Diffs d;
    const std::vector<double> vals = probe_doubles(300);
    const char* precs[] = {"", ".0", ".1", ".2", ".3", ".5", ".6", ".10", ".15", ".16", ".17", ".18", ".20", ".25", ".40"};
    const char* convs[] = {"f", "F", "e", "E", "g", "G", "a", "A"};
    for (const double v : vals)
        for (const char* c : convs)
            for (const char* p : precs) rig.compare(d, std::string("%") + p + c, {D(v)});
    // flags and widths
    const char* flags[] = {"", "-", "+", " ", "#", "0", "-0", "+0", " 0", "#0", "-#", "+ #"};
    const char* widths[] = {"", "1", "8", "15", "30"};
    std::mt19937 rng(9052);
    for (u32 n = 0; n < 3000; n++) {
        const std::string f = std::string("<%") + flags[rng() % std::size(flags)] + widths[rng() % std::size(widths)] +
                              precs[rng() % std::size(precs)] + (rng() % 4 == 0 ? "L" : rng() % 3 == 0 ? "l" : "") +
                              convs[rng() % std::size(convs)] + ">";
        rig.compare(d, f, {D(vals[rng() % vals.size()])});
    }
    // huge precisions (the zeros go out 32 at a time)
    for (const char* f : {"%.100f", "%.300e", "%.70g", "%#.50g", "%.60a", "%400.3f", "%-400.3e", "%0300.2f"})
        for (const double v : {0.1, 1e300, 5e-324, -2.5, 0.0})
            rig.compare(d, f, {D(v)});
    d.report("_Printf floating point");
}

TEST_CASE("sys_libc's _Printf gives the game's: Sony's vectors", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    PrintfRig rig(g);
    Diffs d;
    const std::array<u8, 16> qs[] = {
        q_bytes({0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0xFE, 'A', 'b', ' ', '9', 0x10, 0x20, 0x30, 0x40}),
        q_words(0, 1, 0xFFFFFFFF, 0x80000000), q_words(0x12345678, 0x9ABCDEF0, 0x7FFFFFFF, 42),
        q_floats(1.5f, -0.25f, 3.14159f, 1e30f), q_floats(0.0f, -0.0f, std::numeric_limits<float>::infinity(), 1e-40f),
        q_doubles(2.5, -1e-300), q_doubles(0.1, 123456.789),
        q_bytes({0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50})};
    const char* fmts[] = {"%vd", "%vld", "%lvd", "%vhd", "%hvd", "%vhhd", "%hhvd", "%vlld", "%llvd", "%vi", "%vu", "%vx",
        "%vX", "%vo", "%#vx", "%#vo", "%vc", "%vhc", "%vhhc", "%vf", "%vLf", "%Lvf", "%ve", "%vg", "%va", "%vE",
        "%,vd", "%;vx", "%:vu", "%_vd", "% vd", "%+vd", "%,+vd", "% ,vd", "%08vx", "%-6vd", "%6.3vd", "%.2vf",
        "%10.4vf", "%,vf", "%_vc", "%,vc", "% vc", "%xc", "%cx", "%Xc", "%cX", "%uc", "%cu", "%oc", "%co",
        "%#xc", "%5cx", "%vxc", "%vcx", "%hvu", "%vhu", "%vlu", "%lvx", "%vLe", "%-5vc", "%vp"};
    for (const char* f : fmts)
        for (const auto& q : qs) {
            rig.compare(d, std::string("<") + f + ">", {V(q)});
            rig.compare(d, std::string("<%d") + f + "%d>", {I(7), V(q), I(9)}); // the vector at a 16-byte boundary
        }
    d.report("_Printf vectors");
}

namespace {

// An argument of a wide printf: a 64-bit slot, or a wide string (16-bit for the game, 32-bit for pspocket).
struct WArg {
    u64 v = 0;
    bool wide = false;
    std::u32string w;
};
WArg WI(u64 v) { return WArg{v, false, {}}; }
WArg WD(double d) { return WArg{std::bit_cast<u64>(d), false, {}}; }
WArg WS(std::u32string w) { return WArg{0, true, std::move(w)}; }

struct WPrintfRig {
    GameExecutable& g;
    u32 pfn16, pfn32;
    Log lg, lo;
    WPrintfRig(GameExecutable& game)
        : g(game), pfn16(logging_pfn(game, 1)), pfn32(logging_pfn(game, 2)), lg(new_log(game, 0x40000)),
          lo(new_log(game, 0x40000))
    {
    }
    void compare(Diffs& d, std::u32string_view fmt, const std::vector<WArg>& args, u32 calls = 0x7FFFFFFF)
    {
        std::vector<Arg> ga, oa;
        for (const WArg& a : args) {
            ga.push_back(I(a.wide ? wstr16(g, a.w) : a.v));
            oa.push_back(I(a.wide ? wstr32(g, a.w) : a.v));
        }
        const u32 fg = wstr16(g, fmt), fo = wstr32(g, fmt);
        const u32 apg = list_of(g, ga), apo = list_of(g, oa);
        reset_log(g, lg, calls);
        reset_log(g, lo, calls);
        const s32 rg = static_cast<s32>(g.call_game("_WPrintf", {pfn16, lg.at, fg, apg}));
        const s32 ro = static_cast<s32>(g.call_own("sys_libc", "_WPrintf", {pfn32, lo.at, fo, apo}));
        const auto pg = pieces(g, lg, 2), po = pieces(g, lo, 4);
        d.compared++;
        if (rg != ro || pg != po)
            d.add(std::format("_WPrintf(\"{}\"): game {} {} / own {} {}", narrow(std::u32string(fmt)), rg, show(pg), ro, show(po)));
    }
};

std::u32string widen(std::string_view s)
{
    std::u32string w;
    for (const char c : s) w += static_cast<char32_t>(static_cast<u8>(c));
    return w;
}

} // namespace

TEST_CASE("sys_libc's _WPrintf gives the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    WPrintfRig rig(g);
    Diffs d;
    const char* flags[] = {"", "-", "+", " ", "#", "0", "-0", "+0", " 0", "#0", "+ #"};
    const char* widths[] = {"", "1", "8", "20"};
    const char* precs[] = {"", ".", ".0", ".1", ".3", ".8", ".17", ".25"};
    const char* iquals[] = {"", "hh", "h", "l", "ll", "j", "t", "z", "L"};
    std::mt19937 rng(9053);
    for (u32 n = 0; n < 1500; n++) {
        const std::string f = std::string("<%") + flags[rng() % std::size(flags)] + widths[rng() % std::size(widths)] +
                              precs[rng() % std::size(precs)] + iquals[rng() % std::size(iquals)] + "diuoxX"[rng() % 6] + ">";
        rig.compare(d, widen(f), {WI(kInts[rng() % kInts.size()])});
    }
    const std::vector<double> vals = probe_doubles(120);
    for (u32 n = 0; n < 2500; n++) {
        const std::string f = std::string("<%") + flags[rng() % std::size(flags)] + widths[rng() % std::size(widths)] +
                              precs[rng() % std::size(precs)] + (rng() % 4 == 0 ? "L" : "") + "fFeEgGaA"[rng() % 8] + ">";
        rig.compare(d, widen(f), {WD(vals[rng() % vals.size()])});
    }
    // characters (btowc: no wide character for a byte past 0x7F), wide characters, strings both ways, %n, %%, others
    for (const char* f : {"%c", "%5c", "%-5c", "%lc", "%5lc", "%-3lc", "%%", "%5%", "%k", "%5k", "%S", "%C", "%p",
             "%#p", "%10p", "%5n|", "%-5n|", "%n|", "%hhn", "%lln"})
        for (const u64 v : {u64{'A'}, u64{0}, u64{0xE9}, u64{0x80}, u64{0x7F}, u64{0xFFFFFFFF}, u64{0x141}, u64{0x20AC}}) {
            // (a wint_t past 16 bits is the game's build's own: its wchar_t is 16-bit)
            if (std::string_view(f).find('l') != std::string_view::npos && v > 0xFFFF) continue;
            rig.compare(d, widen(std::string("<") + f + ">"), {WI(std::string_view(f).back() == 'n' || std::string_view(f) == "%5n|" ? g.alloc(8) : v)});
        }
    for (const char* f : {"%s", "%10s", "%-10s", "%.3s", "%10.3s", "%.0s", "%70s", "%-70s", "%5.1s"})
        for (const char* s : {"", "hello", "\xc3\xa9t\xc3\xa9", "A\xe9Z", "A\xc0\x80Z", "\xe2\x82\xac\xe2\x82\xac", "A\xe0\x9f\xbfZ"})
            rig.compare(d, widen(f), {WI(g.cstring(s))});
    const std::string longs(150, 'q');
    rig.compare(d, U"%s|%100s|%-100s|", {WI(g.cstring(longs)), WI(g.cstring(longs)), WI(g.cstring("x"))});
    for (const char* f : {"%ls", "%10ls", "%-10ls", "%.3ls", "%10.3ls", "%.0ls"})
        for (const char32_t* w : {U"", U"wide", U"é€￿", U"abcdefghijklmnopqrstuvwxyz"})
            rig.compare(d, widen(f), {WS(w)});
    for (u32 calls = 0; calls < 8; calls++) rig.compare(d, U"ab%5dcd%-20ls|%s", {WI(42), WS(U"text"), WI(g.cstring("q\xc3\xa9"))}, calls);
    d.report("_WPrintf");
}

namespace {

// A print-formatting structure, laid out for the game or for pspocket: narrow (the same for both), or wide (the game's
// with a 16-bit qual at +58, pspocket's with a 32-bit one at +60). s is kept relative to a buffer (rel) when it points
// into it, so two sides with buffers of their own compare equal.
struct Image {
    u64 v = 0;
    std::array<u8, 16> vec{};
    s64 s = 0;
    s32 n0 = 0, nz0 = 0, n1 = 0, nz1 = 0, n2 = 0, nz2 = 0, prec = -1, nchar = 0, width = 0;
    u16 flags = 0;
    u32 qual = 0;
    bool operator==(const Image&) const = default;
    std::string str() const
    {
        return std::format("v={:#x} s={} n0={} nz0={} n1={} nz1={} n2={} nz2={} prec={} nchar={} width={} flags={:#x} qual={:#x}",
            v, s, n0, nz0, n1, nz1, n2, nz2, prec, nchar, width, flags, qual);
    }
};

enum class Layout { Narrow, Wide16, Wide32 };


void put_image(GameExecutable& g, u32 at, Layout l, const Image& p, u32 rel, u32 pfn, u32 arg)
{
    u8* b = g.b;
    std::memset(b + at, 0, 0x60);
    const u32 s = static_cast<u32>(rel + p.s * (l == Layout::Narrow ? 1 : l == Layout::Wide16 ? 2 : 4));
    ps_write64(b, at, p.v);
    if (l == Layout::Narrow) {
        std::memcpy(b + at + 0x10, p.vec.data(), 16);
        ps_write32(b, at + 0x20, pfn);
        ps_write32(b, at + 0x24, arg);
        ps_write32(b, at + 0x28, s);
        const s32 f[] = {p.n0, p.nz0, p.n1, p.nz1, p.n2, p.nz2, p.prec, p.nchar, p.width};
        for (u32 k = 0; k < 9; k++) ps_write32(b, at + 0x2C + 4 * k, static_cast<u32>(f[k]));
        ps_write16(b, at + 0x50, p.flags);
        b[at + 0x52] = static_cast<u8>(p.qual);
    } else {
        ps_write32(b, at + 8, pfn);
        ps_write32(b, at + 12, arg);
        ps_write32(b, at + 16, s);
        const s32 f[] = {p.n0, p.nz0, p.n1, p.nz1, p.n2, p.nz2, p.nchar, p.prec, p.width};
        for (u32 k = 0; k < 9; k++) ps_write32(b, at + 20 + 4 * k, static_cast<u32>(f[k]));
        ps_write16(b, at + 56, p.flags);
        if (l == Layout::Wide16) ps_write16(b, at + 58, static_cast<u16>(p.qual));
        else ps_write32(b, at + 60, p.qual);
    }
}

Image get_image(GameExecutable& g, u32 at, Layout l, u32 rel, u32* arg = nullptr)
{
    u8* b = g.b;
    Image p;
    p.v = ps_read64(b, at);
    if (l == Layout::Narrow) {
        std::memcpy(p.vec.data(), b + at + 0x10, 16);
        if (arg) *arg = ps_read32(b, at + 0x24);
        p.s = static_cast<s64>(ps_read32(b, at + 0x28)) - rel;
        s32* f[] = {&p.n0, &p.nz0, &p.n1, &p.nz1, &p.n2, &p.nz2, &p.prec, &p.nchar, &p.width};
        for (u32 k = 0; k < 9; k++) *f[k] = static_cast<s32>(ps_read32(b, at + 0x2C + 4 * k));
        p.flags = ps_read16(b, at + 0x50);
        p.qual = b[at + 0x52];
    } else {
        if (arg) *arg = ps_read32(b, at + 12);
        p.s = (static_cast<s64>(ps_read32(b, at + 16)) - rel) / (l == Layout::Wide16 ? 2 : 4);
        s32* f[] = {&p.n0, &p.nz0, &p.n1, &p.nz1, &p.n2, &p.nz2, &p.nchar, &p.prec, &p.width};
        for (u32 k = 0; k < 9; k++) *f[k] = static_cast<s32>(ps_read32(b, at + 20 + 4 * k));
        p.flags = ps_read16(b, at + 56);
        p.qual = l == Layout::Wide16 ? ps_read16(b, at + 58) : ps_read32(b, at + 60);
    }
    return p;
}

// n characters of text at `at` (bytes, or 16- or 32-bit ones).
std::u32string text_at(GameExecutable& g, u32 at, u32 n, u32 unit)
{
    std::u32string s;
    for (u32 i = 0; i < n; i++)
        s += static_cast<char32_t>(unit == 1 ? g.b[at + i] : unit == 2 ? ps_read16(g.b, at + 2 * i) : ps_read32(g.b, at + 4 * i));
    return s;
}
void fill_text(GameExecutable& g, u32 at, u32 n, u32 unit, char32_t c)
{
    for (u32 i = 0; i < n; i++) {
        if (unit == 1) g.b[at + i] = static_cast<u8>(c);
        else if (unit == 2) ps_write16(g.b, at + 2 * i, static_cast<u16>(c));
        else ps_write32(g.b, at + 4 * i, static_cast<u32>(c));
    }
}

Image random_image(std::mt19937& rng)
{
    Image p;
    const s32 precs[] = {-1, -1, 0, 1, 2, 3, 5, 6, 10, 17, 20, 25, 40};
    const s32 widths[] = {0, 0, 1, 5, 12, 30, -4};
    p.prec = precs[rng() % std::size(precs)];
    p.width = widths[rng() % std::size(widths)];
    p.flags = static_cast<u16>(rng() & 0x1F);
    p.n0 = static_cast<s32>(rng() % 3);
    p.nz0 = rng() % 4 == 0 ? 3 : 0;
    p.nchar = static_cast<s32>(rng() % 100);
    return p;
}

} // namespace

TEST_CASE("sys_libc's _Litob, _Ldtob and _Genld (and the wide ones) give the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    std::mt19937 rng(9054);
    const std::vector<double> vals = probe_doubles(400);
    struct Side {
        const char* name;
        Layout layout;
        u32 unit;
        bool game;
    };
    for (const bool wide : {false, true}) {
        Diffs d;
        const Side sides[2] = {{wide ? "_WLitob" : "_Litob", wide ? Layout::Wide16 : Layout::Narrow, wide ? 2u : 1u, true},
            {wide ? "_WLitob" : "_Litob", wide ? Layout::Wide32 : Layout::Narrow, wide ? 4u : 1u, false}};
        const u32 pf[2] = {g.alloc(0x60), g.alloc(0x60)}, buf[2] = {g.alloc(0x400), g.alloc(0x800)};
        const auto run = [&](const char* fn, const Image& p, std::initializer_list<u64> extra, const std::string& what) {
            Image out[2];
            std::u32string txt[2];
            for (int k = 0; k < 2; k++) {
                const Side& sd = sides[k];
                fill_text(g, buf[k], 0x100, sd.unit, 0xCC);
                put_image(g, pf[k], sd.layout, p, buf[k] + 8 * sd.unit, 0, 0);
                std::vector<u64> args = {pf[k]};
                args.insert(args.end(), extra.begin(), extra.end());
                const std::string name = (wide ? std::string("_W") : std::string("_")) + fn;
                if (sd.game) {
                    if (args.size() == 2) g.call_game(name, {args[0], args[1]});
                    else g.call_game(name, {args[0], args[1], args[2], args[3], args[4]});
                } else {
                    if (args.size() == 2) g.call_own("sys_libc", name, {args[0], args[1]});
                    else g.call_own("sys_libc", name, {args[0], args[1], args[2], args[3], args[4]});
                }
                out[k] = get_image(g, pf[k], sd.layout, buf[k] + 8 * sd.unit);
                txt[k] = text_at(g, buf[k], 0x100, sd.unit);
            }
            d.compared++;
            if (!(out[0] == out[1]) || txt[0] != txt[1])
                d.add(std::format("{} {}: game {} / own {}{}", fn, what, out[0].str(), out[1].str(), txt[0] != txt[1] ? " (text differs)" : ""));
        };
        // _Litob: every code on random structures
        for (u32 n = 0; n < 3000; n++) {
            Image p = random_image(rng);
            p.v = rng() % 3 ? kInts[rng() % kInts.size()] : (static_cast<u64>(rng()) << 32 | rng());
            const char code = "diouxXpc"[rng() % 8];
            run("Litob", p, {static_cast<u64>(code)}, std::format("'{}' {}", code, p.str()));
        }
        // _Ldtob: every floating code
        for (u32 n = 0; n < 6000; n++) {
            Image p = random_image(rng);
            p.v = std::bit_cast<u64>(vals[rng() % vals.size()]);
            const char code = "eEfFgGaA"[rng() % 8];
            run("Ldtob", p, {static_cast<u64>(code)}, std::format("'{}' {}", code, p.str()));
        }
        // _Genld: digit strings of every length (none and negative too), exponents near and far
        const u32 digits[2] = {g.alloc(64), g.alloc(128)};
        for (u32 n = 0; n < 3000; n++) {
            Image p = random_image(rng);
            p.n1 = static_cast<s32>(rng() % 2);
            const char code = "eEfFgGaA"[rng() % 8];
            // (_Ldtob hands f and F a precision of 0 or more: with a negative one the library's copies run away)
            if ((code == 'f' || code == 'F') && p.prec < 0) p.prec = 0;
            const s32 nsig = static_cast<s32>(rng() % 26) - 3;
            const s32 xexp = rng() % 2 ? static_cast<s32>(rng() % 41) - 20 : static_cast<s32>(rng() % 700) - 350;
            std::string ds;
            for (int k = 0; k < 30; k++) ds += "0123456789abcdef"[rng() % (code == 'a' || code == 'A' ? 16 : 10)];
            for (u32 k = 0; k < 30; k++) {
                if (!wide) g.b[digits[0] + k] = static_cast<u8>(ds[k]);
                if (wide) {
                    ps_write16(g.b, digits[0] + 2 * k, static_cast<u8>(ds[k]));
                    ps_write32(g.b, digits[1] + 4 * k, static_cast<u8>(ds[k]));
                } else {
                    g.b[digits[1] + k] = static_cast<u8>(ds[k]);
                }
            }
            Image out[2];
            std::u32string txt[2];
            for (int k = 0; k < 2; k++) {
                const Side& sd = sides[k];
                fill_text(g, buf[k], 0x100, sd.unit, 0xCC);
                put_image(g, pf[k], sd.layout, p, buf[k] + 8 * sd.unit, 0, 0);
                const std::string name = wide ? "_WGenld" : "_Genld";
                const std::initializer_list<u64> a = {pf[k], static_cast<u64>(code), digits[k],
                    static_cast<u64>(static_cast<s64>(nsig)), static_cast<u64>(static_cast<s64>(xexp))};
                if (sd.game) g.call_game(name, a);
                else g.call_own("sys_libc", name, a);
                out[k] = get_image(g, pf[k], sd.layout, buf[k] + 8 * sd.unit);
                txt[k] = text_at(g, buf[k], 0x100, sd.unit);
            }
            d.compared++;
            if (!(out[0] == out[1]) || txt[0] != txt[1])
                d.add(std::format("Genld '{}' \"{}\" nsig {} xexp {}: game {} / own {}{}", code, ds.substr(0, 25), nsig,
                    xexp, out[0].str(), out[1].str(), txt[0] != txt[1] ? " (text differs)" : ""));
        }
        d.report(wide ? "_WLitob, _WLdtob, _WGenld" : "_Litob, _Ldtob, _Genld");
    }
}

TEST_CASE("sys_libc's _Puttxt, _Putstr and _Putfld (and the wide ones) give the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    std::mt19937 rng(9055);
    for (const bool wide : {false, true}) {
        Diffs d;
        const Layout layout[2] = {wide ? Layout::Wide16 : Layout::Narrow, wide ? Layout::Wide32 : Layout::Narrow};
        const u32 unit[2] = {wide ? 2u : 1u, wide ? 4u : 1u};
        const u32 pfn[2] = {logging_pfn(g, wide ? 1 : 0), logging_pfn(g, wide ? 2 : 0)};
        const Log log[2] = {new_log(g, 0x10000), new_log(g, 0x20000)};
        const u32 pf[2] = {g.alloc(0x60), g.alloc(0x60)}, ac[2] = {g.alloc(0x200), g.alloc(0x400)},
                  text[2] = {g.alloc(0x200), g.alloc(0x400)};
        const std::string pre = wide ? "_W" : "_";
        u32 known[2] = {0, 0}; // a pointer each side has of its own (a wide string argument): s at it compares equal
        // Runs `fn` on both sides: the structure, ac (64 characters "a.."), the text at s ("0123..."), the log.
        const auto both = [&](const std::string& what, const Image& p, u32 calls, const auto& call) {
            Image out[2];
            u32 arg[2];
            s32 r[2];
            std::vector<std::u32string> pc[2];
            std::u32string acs[2];
            for (int k = 0; k < 2; k++) {
                for (u32 i = 0; i < 64; i++) fill_text(g, ac[k] + i * unit[k], 1, unit[k], static_cast<char32_t>('a' + i % 26));
                for (u32 i = 0; i < 100; i++) fill_text(g, text[k] + i * unit[k], 1, unit[k], static_cast<char32_t>('0' + i % 10));
                reset_log(g, log[k], calls);
                put_image(g, pf[k], layout[k], p, text[k], pfn[k], log[k].at);
                r[k] = call(k, pf[k], ac[k]);
                out[k] = get_image(g, pf[k], layout[k], text[k], &arg[k]);
                // s relative to the side's text or ac buffer when it points into one, else where it points
                const s64 sabs = out[k].s * unit[k] + text[k];
                if (sabs >= ac[k] && sabs < ac[k] + 0x100 * unit[k]) out[k].s = (sabs - ac[k]) / unit[k] + 0x100000;
                else if (known[k] && sabs == known[k]) out[k].s = 0x300000000ll;
                else if (sabs < text[k] || sabs >= text[k] + 0x100 * unit[k]) out[k].s = sabs + 0x200000000ll;
                pc[k] = pieces(g, log[k], unit[k]);
                acs[k] = text_at(g, ac[k], 80, unit[k]);
            }
            d.compared++;
            const bool args_ok = (arg[0] == 0) == (arg[1] == 0);
            if (r[0] != r[1] || !(out[0] == out[1]) || pc[0] != pc[1] || acs[0] != acs[1] || !args_ok)
                d.add(std::format("{}: game {} {} {} / own {} {} {}{}", what, r[0], show(pc[0]), out[0].str(), r[1],
                    show(pc[1]), out[1].str(), acs[0] != acs[1] ? " (ac differs)" : ""));
        };
        // _Puttxt: every combination of the counts, the width and '-'
        for (u32 n = 0; n < 2500; n++) {
            Image p;
            const s32 counts[] = {0, 0, 1, 2, 5, 31, 32, 33, 40};
            p.n0 = counts[rng() % std::size(counts)];
            p.nz0 = counts[rng() % std::size(counts)];
            p.n1 = counts[rng() % std::size(counts)];
            p.nz1 = counts[rng() % std::size(counts)];
            p.n2 = counts[rng() % std::size(counts)];
            p.nz2 = counts[rng() % std::size(counts)];
            const s32 widths[] = {0, -3, 10, 40, 70, 100, 200};
            p.width = widths[rng() % std::size(widths)];
            p.flags = static_cast<u16>(rng() & 0x1F);
            p.nchar = static_cast<s32>(rng() % 50);
            const u32 calls = rng() % 5 == 0 ? rng() % 6 : 0x7FFFFFFF;
            both(std::format("{}Puttxt {} calls {}", pre, p.str(), calls), p, calls, [&](int k, u32 pfk, u32 ack) {
                return static_cast<s32>(k == 0 ? g.call_game(pre + "Puttxt", {pfk, ack}) : g.call_own("sys_libc", pre + "Puttxt", {pfk, ack}));
            });
        }
        // _Putstr (wide text, narrow out) / _WPutstr (narrow text, wide out)
        const std::u32string wtexts[] = {U"", U"abc", U"éè€", std::u32string(100, U'w'), U"x翿y"};
        const char* ntexts[] = {"", "abc", "\xc3\xa9t\xc3\xa9", "A\xe9Z", "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac"};
        for (u32 n = 0; n < 1200; n++) {
            Image p;
            const s32 precs[] = {-1, -1, 0, 1, 2, 3, 5, 50};
            const s32 widths[] = {0, 3, 10, 61, 62, 70, 120, -2};
            p.prec = precs[rng() % std::size(precs)];
            p.width = widths[rng() % std::size(widths)];
            // (a negative width makes _WPutstr's buffer a malloc of 4 GB, which the game's own allocator answers)
            if (wide && p.width < 0) p.width = 0;
            p.flags = static_cast<u16>(rng() & 0x1F);
            const u32 calls = rng() % 6 == 0 ? rng() % 4 : 0x7FFFFFFF;
            if (!wide) {
                const std::u32string& w = wtexts[rng() % std::size(wtexts)];
                const u32 s16 = wstr16(g, w), s32a = wstr32(g, w);
                both(std::format("_Putstr {} \"{}\" calls {}", p.str(), narrow(w), calls), p, calls, [&](int k, u32 pfk, u32) {
                    return static_cast<s32>(k == 0 ? g.call_game("_Putstr", {pfk, s16}) : g.call_own("sys_libc", "_Putstr", {pfk, s32a}));
                });
            } else {
                const u32 s = g.cstring(ntexts[rng() % std::size(ntexts)]);
                both(std::format("_WPutstr {} calls {}", p.str(), calls), p, calls, [&](int k, u32 pfk, u32) {
                    return static_cast<s32>(k == 0 ? g.call_game("_WPutstr", {pfk, s}) : g.call_own("sys_libc", "_WPutstr", {pfk, s}));
                });
            }
        }
        // _Putfld / _WPutfld: one conversion with its argument from the va_list, as _Printf calls it
        const std::vector<double> vals = probe_doubles(60);
        const char* codes = wide ? "diouxXeEfFgGaAcspn%kS" : "diouxXeEfFgGaAcspn%kSCv";
        const char* quals = wide ? "\0hbljqtzL" : "\0hbljqtzLvacgde";
        const usize nq = wide ? 9 : 15;
        for (u32 n = 0; n < 4000; n++) {
            Image p;
            p.prec = std::array<s32, 6>{-1, -1, 0, 2, 6, 20}[rng() % 6];
            p.width = std::array<s32, 5>{0, 0, 4, 12, 70}[rng() % 5];
            p.flags = static_cast<u16>(rng() & (wide ? 0x1F : 0x79F));
            p.qual = static_cast<u8>(quals[rng() % nq]);
            p.nchar = static_cast<s32>(rng() % 40);
            const char code = codes[rng() % std::strlen(codes)];
            // the argument: an integer, a double, a string (narrow; wide ones 16- or 32-bit), a vector
            const u64 iv = kInts[rng() % kInts.size()];
            const double dv = vals[rng() % vals.size()];
            const u32 str = g.cstring(rng() % 2 ? "string" : "\xc3\xa9t\xc3\xa9");
            const std::u32string wv = rng() % 2 ? U"wide" : U"é€";
            const auto q = q_words(static_cast<u32>(rng()), static_cast<u32>(rng()), static_cast<u32>(rng()), static_cast<u32>(rng()));
            const bool is_float = std::strchr("eEfFgGaA", code) != nullptr;
            const bool is_string = code == 's' || code == 'S';
            const bool wide_string = is_string && (code == 'S' || p.qual == 'l') && !(wide && code == 'S');
            const bool narrow_string = is_string && !wide_string;
            const bool vector = !wide && (std::strchr("vacgde", static_cast<char>(p.qual)) && p.qual) &&
                                (std::strchr("diouxXc", code) || (is_float && (p.qual == 'v' || p.qual == 'e')));
            if (code == 'n' || (code == 'c' && p.qual == 'l' && iv > 0xFFFF)) continue;
            u32 lists[2], aps[2];
            known[0] = known[1] = 0;
            for (int k = 0; k < 2; k++) {
                std::vector<Arg> a;
                if (vector) a = {I(0x1111), V(q), I(0x2222)};
                else if (is_float) a = {D(dv), I(0x2222)};
                else if (wide_string) a = {I(known[k] = (k == 0 ? wstr16(g, wv) : wstr32(g, wv))), I(0x2222)};
                else if (narrow_string) a = {I(str), I(0x2222)};
                else a = {I(iv), I(0x2222)};
                lists[k] = list_of(g, a);
                if (vector) lists[k] += 8; // past the first slot: the vector is at the next 16-byte boundary
                aps[k] = g.alloc(8);
                ps_write32(g.b, aps[k], lists[k]);
            }
            const u32 calls = rng() % 8 == 0 ? rng() % 3 : 0x7FFFFFFF;
            both(std::format("{}Putfld '{}' qual {:#x} {} calls {}", pre, code, p.qual, p.str(), calls), p, calls,
                [&](int k, u32 pfk, u32 ack) {
                    const s32 r = static_cast<s32>(k == 0 ? g.call_game(pre + "Putfld", {pfk, aps[k], static_cast<u64>(code), ack})
                                                          : g.call_own("sys_libc", pre + "Putfld", {pfk, aps[k], static_cast<u64>(code), ack}));
                    // the va_list moved as far
                    return r * 1000 + static_cast<s32>(ps_read32(g.b, aps[k]) - lists[k]);
                });
        }
        d.report(wide ? "_WPuttxt, _WPutstr, _WPutfld" : "_Puttxt, _Putstr, _Putfld");
    }
}

namespace {

namespace as = pspocket::recomp::spu::as;

// The SPU printf's rig: a spinning SPU thread whose store holds the format (0x1000), strings (0x3000), the arguments
// (one set for the game's call at 0x8000, one for pspocket's at 0x9000) and %n's targets (0x4000 and 0x5000).
struct SpuRig {
    GameExecutable& g;
    u32 spu = 0, group = 0, pfn;
    Log lg, lo;
    explicit SpuRig(GameExecutable& game) : g(game), pfn(logging_pfn(game, 0)), lg(new_log(game, 0x20000)), lo(new_log(game, 0x20000))
    {
        // It goes at once to a loop at 0x200, so that the store from 0 on can hold data (what %s of address 0 reads).
        pspocket::test::SpuProgram p;
        p.emit(as::bra(0x200));
        p.data(0x200, {0x32, 0x00, 0x00, 0x00}); // br 0 (to itself)
        p.data(0xFFF0, std::vector<u8>(16, 0));
        const u32 scratch = g.alloc(0x14000);
        const auto grp = pspocket::test::make_spu_group(*g.thread, g.b, scratch, p);
        REQUIRE(grp.thread);
        REQUIRE(lv2::sys_spu_thread_group_start(*g.thread, grp.group) == lv2::CELL_OK);
        spu = grp.thread;
        group = grp.group;
    }
    ~SpuRig() { lv2::sys_spu_thread_group_terminate(*g.thread, group, 0); }
    SpuRig(const SpuRig&) = delete;
    SpuRig& operator=(const SpuRig&) = delete;

    void write(u32 ls, const std::vector<u8>& bytes)
    {
        for (usize i = 0; i < bytes.size(); i++)
            REQUIRE(lv2::sys_spu_thread_write_ls(*g.thread, spu, ls + static_cast<u32>(i), bytes[i], 1) == lv2::CELL_OK);
    }
    std::vector<u8> read(u32 ls, u32 n)
    {
        std::vector<u8> out;
        const u32 v = g.alloc(8);
        for (u32 i = 0; i < n; i++) {
            REQUIRE(lv2::sys_spu_thread_read_ls(*g.thread, spu, ls + i, v, 1) == lv2::CELL_OK);
            out.push_back(static_cast<u8>(ps_read64(g.b, v)));
        }
        return out;
    }
    // The arguments at `at`: the format's address, then a quadword each (a 32-bit value in its first word, a 64-bit
    // one or a double in its first doubleword); kTarget arguments become %n targets (base + 8k).
    void args(u32 at, u32 fmt, const std::vector<Arg>& a, u32 target_base, bool wide64 = false)
    {
        std::vector<u8> q = pspocket::test::quad32(fmt);
        u32 k = 0;
        for (const Arg& x : a) {
            u64 v = x.v;
            if ((v & 0xFFFFFFFF00000000ull) == kTarget) v = target_base + 8 * k++;
            const auto quad = (wide64 || v > 0xFFFFFFFFull) ? pspocket::test::quad64(v) : pspocket::test::quad32(static_cast<u32>(v));
            q.insert(q.end(), quad.begin(), quad.end());
        }
        write(at, q);
    }
    void compare(Diffs& d, const std::string& fmt, const std::vector<Arg>& a, u32 calls = 0x7FFFFFFF)
    {
        std::vector<u8> f(fmt.begin(), fmt.end());
        f.push_back(0);
        write(0x1000, f);
        args(0x8000, 0x1000, a, 0x4000);
        args(0x9000, 0x1000, a, 0x5000);
        write(0x4000, std::vector<u8>(64, 0xAA));
        write(0x5000, std::vector<u8>(64, 0xAA));
        reset_log(g, lg, calls);
        reset_log(g, lo, calls);
        const s32 rg = static_cast<s32>(g.call_game("__spu_thread_printf", {pfn, lg.at, spu, 0x8000}));
        const s32 ro = static_cast<s32>(g.call_own("sys_libc", "__spu_thread_printf", {pfn, lo.at, spu, 0x9000}));
        const auto pg = pieces(g, lg, 1), po = pieces(g, lo, 1);
        d.compared++;
        if (rg != ro || pg != po || read(0x4000, 64) != read(0x5000, 64))
            d.add(std::format("__spu_thread_printf(\"{}\"): game {} {} / own {} {}", fmt, rg, show(pg), ro, show(po)));
    }
};

} // namespace

TEST_CASE("sys_libc's SPU printf gives the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    SpuRig rig(g);
    Diffs d;
    // strings in the store
    rig.write(0x3000, pspocket::test::bytes_of("hello"));
    rig.write(0x3010, pspocket::test::bytes_of(""));
    std::vector<u8> longs(300, 'L');
    longs.push_back(0);
    rig.write(0x3100, longs);
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the thread past LS 0 and in its loop
    rig.write(0, std::vector<u8>{0x41, 0x42, 0x43, 0}); // what %s of address 0 measures (it prints "(null)")
    const char* flags[] = {"", "-", "+", " ", "#", "0", "-0", "+ ", "#0"};
    const char* widths[] = {"", "1", "8", "20"};
    const char* precs[] = {"", ".", ".0", ".1", ".3", ".17"};
    const char* quals[] = {"", "hh", "h", "l", "ll", "j", "t", "z", "L"};
    std::mt19937 rng(9056);
    for (u32 n = 0; n < 500; n++) {
        const std::string f = std::string("<%") + flags[rng() % std::size(flags)] + widths[rng() % std::size(widths)] +
                              precs[rng() % std::size(precs)] + quals[rng() % std::size(quals)] + "diuoxX"[rng() % 6] + ">";
        rig.compare(d, f, {I(kInts[rng() % kInts.size()])});
    }
    const std::vector<double> vals = probe_doubles(40);
    for (u32 n = 0; n < 500; n++) {
        const std::string f = std::string("<%") + flags[rng() % std::size(flags)] + widths[rng() % std::size(widths)] +
                              precs[rng() % std::size(precs)] + (rng() % 3 == 0 ? "L" : "") + "fFeEgGaA"[rng() % 8] + ">";
        Arg a = D(vals[rng() % vals.size()]);
        rig.compare(d, f, {a});
    }
    for (const char* f : {"%s", "%10s", "%-10s", "%.3s", "%.0s", "%5.1s", "%S", "%ls", "%.200s", "%-310s|"})
        for (const u32 s : {0x3000u, 0x3010u, 0x3100u, 0u})
            rig.compare(d, std::string("<") + f + ">", {I(s)});
    for (const char* f : {"%c", "%5c", "%-3c", "%%", "%5%", "%k", "%5k", "%p", "%#p", "%10p", "%*d", "%-*d", "%.*d",
             "%*.*f", "%d%%%d", "%%%d"})
        rig.compare(d, std::string("<") + f + ">", {I(5), I(42), I(7), I(9)});
    for (const char* f : {"abc%n", "ab%hhn", "abcd%hn", "%ln", "x%lln", "%jn", "%zn", "%tn", "%Ln", "%5n|"})
        rig.compare(d, std::string("12345") + f, {I(kTarget)});
    for (u32 calls = 0; calls < 10; calls++) rig.compare(d, "ab%5d|%-40s|%c", {I(42), I(0x3000), I('z')}, calls);
    d.report("__spu_thread_printf");

    // sprintf and snprintf on the store's arguments
    Diffs d2;
    for (const auto& [fmt, args] : std::vector<std::pair<std::string, std::vector<Arg>>>{
             {"%d|%s|%.2f|%x", {I(-17), I(0x3000), D(2.25), I(0xBEEF)}}, {"%40s", {I(0x3100)}}, {"", {}},
             {"%%%c%5.1e", {I('q'), D(1e-300)}}}) {
        std::vector<u8> f(fmt.begin(), fmt.end());
        f.push_back(0);
        rig.write(0x1000, f);
        rig.args(0x8000, 0x1000, args, 0x4000);
        for (const u32 size : {0u, 1u, 5u, 400u}) {
            const u32 bg = g.alloc(512), bo = g.alloc(512);
            std::memset(g.b + bg, 0x7E, 512);
            std::memset(g.b + bo, 0x7E, 512);
            const s32 rg = static_cast<s32>(g.call_game("spu_thread_snprintf", {bg, size, rig.spu, 0x8000}));
            const s32 ro = static_cast<s32>(g.call_own("sys_libc", "spu_thread_snprintf", {bo, size, rig.spu, 0x8000}));
            d2.compared++;
            if (rg != ro || g.bytes(bg, 512) != g.bytes(bo, 512)) d2.add(std::format("spu_thread_snprintf(\"{}\", {})", fmt, size));
        }
        const u32 bg = g.alloc(512), bo = g.alloc(512);
        std::memset(g.b + bg, 0x7E, 512);
        std::memset(g.b + bo, 0x7E, 512);
        const s32 rg = static_cast<s32>(g.call_game("spu_thread_sprintf", {bg, rig.spu, 0x8000}));
        const s32 ro = static_cast<s32>(g.call_own("sys_libc", "spu_thread_sprintf", {bo, rig.spu, 0x8000}));
        d2.compared++;
        if (rg != ro || g.bytes(bg, 512) != g.bytes(bo, 512)) d2.add(std::format("spu_thread_sprintf(\"{}\")", fmt));
    }
    d2.report("spu_thread_sprintf, spu_thread_snprintf");
}

TEST_CASE("sys_libc's __spu_thread_putfld and __spu_thread_puttxt give the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    SpuRig rig(g);
    Diffs d;
    rig.write(0x3000, pspocket::test::bytes_of("spu text"));
    std::vector<u8> longs(300, 'M');
    longs.push_back(0);
    rig.write(0x3100, longs);
    const u32 pf[2] = {g.alloc(0x60), g.alloc(0x60)}, ac[2] = {g.alloc(0x100), g.alloc(0x100)}, ap[2] = {g.alloc(8), g.alloc(8)};
    const Log* logs[2] = {&rig.lg, &rig.lo};
    std::mt19937 rng(9057);
    const std::vector<double> vals = probe_doubles(20);
    for (u32 n = 0; n < 1500; n++) {
        Image p;
        p.prec = std::array<s32, 6>{-1, -1, 0, 3, 8, 200}[rng() % 6];
        p.width = std::array<s32, 5>{0, 0, 6, 20, 310}[rng() % 5];
        p.flags = static_cast<u16>(rng() & 0x1F);
        p.qual = static_cast<u8>("\0hbljqtzL"[rng() % 9]);
        p.nchar = static_cast<s32>(rng() % 30);
        const char code = "diouxXeEfFgGaAcspn%kS"[rng() % 21];
        const bool fl = std::strchr("eEfFgGaA", code) != nullptr;
        const u64 v = code == 's' || code == 'S' ? std::array<u64, 3>{0x3000, 0x3100, 0}[rng() % 3]
                      : code == 'n'             ? 0x4000 + 8 * (rng() % 4)
                      : fl                      ? std::bit_cast<u64>(vals[rng() % vals.size()])
                                                : kInts[rng() % kInts.size()];
        const auto quad = (fl || v > 0xFFFFFFFFull) ? pspocket::test::quad64(v) : pspocket::test::quad32(static_cast<u32>(v));
        rig.write(0x8000, quad);
        rig.write(0x4000, std::vector<u8>(64, 0xAA));
        Image out[2];
        s32 r[2], rt[2];
        std::vector<u8> mem[2];
        std::vector<std::u32string> pc[2];
        for (int k = 0; k < 2; k++) {
            put_image(g, pf[k], Layout::Narrow, p, ac[k], rig.pfn, logs[k]->at);
            ps_write32(g.b, pf[k] + 0x54, 0xDEAD);
            reset_log(g, *logs[k]);
            for (u32 i = 0; i < 64; i++) g.b[ac[k] + i] = static_cast<u8>('a' + i % 26);
            ps_write32(g.b, ap[k], 0x8000);
            const std::initializer_list<u64> a = {pf[k], rig.spu, ap[k], static_cast<u64>(code), ac[k]};
            r[k] = static_cast<s32>(k == 0 ? g.call_game("__spu_thread_putfld", a) : g.call_own("sys_libc", "__spu_thread_putfld", a));
            rt[k] = static_cast<s32>(k == 0 ? g.call_game("__spu_thread_puttxt", {pf[k], ac[k]})
                                            : g.call_own("sys_libc", "__spu_thread_puttxt", {pf[k], ac[k]}));
            out[k] = get_image(g, pf[k], Layout::Narrow, 0);
            if (out[k].s >= ac[k] && out[k].s < ac[k] + 0x100) out[k].s = out[k].s - ac[k] + 0x100000; // else an LS address
            mem[k] = rig.read(0x4000, 32);
            mem[k].push_back(static_cast<u8>(ps_read32(g.b, ap[k]) - 0x8000));
            for (u32 i = 0; i < 64; i++) mem[k].push_back(g.b[ac[k] + i]);
            const u32 id = ps_read32(g.b, pf[k] + 0x54);
            mem[k].push_back(static_cast<u8>(id == rig.spu ? 1 : id == 0xDEAD ? 2 : 3));
            pc[k] = pieces(g, *logs[k], 1);
            if (k == 0) rig.write(0x4000, std::vector<u8>(64, 0xAA));
        }
        d.compared++;
        if (r[0] != r[1] || rt[0] != rt[1] || !(out[0] == out[1]) || mem[0] != mem[1] || pc[0] != pc[1])
            d.add(std::format("putfld '{}' {}: game {} {} {} / own {} {} {}", code, p.str(), r[0], show(pc[0]),
                out[0].str(), r[1], show(pc[1]), out[1].str()));
    }
    d.report("__spu_thread_putfld, __spu_thread_puttxt");
}

TEST_CASE("sys_libc's sprintf, snprintf, vsprintf, vsnprintf and swprintf give the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    Diffs d;
    struct Case {
        std::string fmt;
        std::vector<u64> args;
    };
    const u32 str = g.cstring("text"), longs = g.cstring(std::string(200, 'z'));
    const std::vector<Case> cases = {{"%d|%s|%.2f|%x", {static_cast<u64>(-17), str, std::bit_cast<u64>(2.25), 0xBEEF}},
        {"%40s", {longs}}, {"", {}}, {"plain", {}}, {"%%%c%5.1e", {'q', std::bit_cast<u64>(1e-300)}},
        {"%.0f %.1f %.20f", {std::bit_cast<u64>(0.5), std::bit_cast<u64>(0.25), std::bit_cast<u64>(0.1)}},
        {"%s%s", {longs, longs}}, {"%-8d|%08.3f|%+i", {5, std::bit_cast<u64>(-3.75), 9}}};
    const auto list = [&](const std::vector<u64>& a) {
        std::vector<Arg> v;
        for (const u64 x : a) v.push_back(I(x));
        return list_of(g, v);
    };
    for (const Case& c : cases) {
        const u32 f = g.cstring(c.fmt);
        for (const u32 size : {0u, 1u, 2u, 7u, 100u, 600u}) {
            const u32 bg = g.alloc(700), bo = g.alloc(700);
            std::memset(g.b + bg, 0x7E, 700);
            std::memset(g.b + bo, 0x7E, 700);
            const s32 rg = static_cast<s32>(g.call_game("vsnprintf", {bg, size, f, list(c.args)}));
            const s32 ro = static_cast<s32>(g.call_own("sys_libc", "vsnprintf", {bo, size, f, list(c.args)}));
            d.compared++;
            if (rg != ro || g.bytes(bg, 700) != g.bytes(bo, 700)) d.add(std::format("vsnprintf(\"{}\", {}): {} / {}", c.fmt, size, rg, ro));
            if (c.args.size() <= 5) {
                std::memset(g.b + bg, 0x7E, 700);
                std::memset(g.b + bo, 0x7E, 700);
                std::vector<u64> a = {bg, size, f};
                a.insert(a.end(), c.args.begin(), c.args.end());
                a.resize(8, 0);
                const s32 sg = static_cast<s32>(g.call_game("snprintf", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
                a[0] = bo;
                const s32 so = static_cast<s32>(g.call_own("sys_libc", "snprintf", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
                d.compared++;
                if (sg != so || g.bytes(bg, 700) != g.bytes(bo, 700)) d.add(std::format("snprintf(\"{}\", {}): {} / {}", c.fmt, size, sg, so));
            }
        }
        const u32 bg = g.alloc(700), bo = g.alloc(700);
        std::memset(g.b + bg, 0x7E, 700);
        std::memset(g.b + bo, 0x7E, 700);
        const s32 rg = static_cast<s32>(g.call_game("vsprintf", {bg, f, list(c.args)}));
        const s32 ro = static_cast<s32>(g.call_own("sys_libc", "vsprintf", {bo, f, list(c.args)}));
        d.compared++;
        if (rg != ro || g.bytes(bg, 700) != g.bytes(bo, 700)) d.add(std::format("vsprintf(\"{}\"): {} / {}", c.fmt, rg, ro));
        if (c.args.size() <= 6) {
            std::memset(g.b + bg, 0x7E, 700);
            std::memset(g.b + bo, 0x7E, 700);
            std::vector<u64> a = {bg, f};
            a.insert(a.end(), c.args.begin(), c.args.end());
            a.resize(8, 0);
            const s32 sg = static_cast<s32>(g.call_game("sprintf", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
            a[0] = bo;
            const s32 so = static_cast<s32>(g.call_own("sys_libc", "sprintf", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
            d.compared++;
            if (sg != so || g.bytes(bg, 700) != g.bytes(bo, 700)) d.add(std::format("sprintf(\"{}\"): {} / {}", c.fmt, sg, so));
        }
    }
    // swprintf: the same text as 16-bit characters for the game, 32-bit ones for pspocket
    const std::vector<std::pair<std::u32string, std::vector<u64>>> wcases = {{U"%d|%s|%.2f", {static_cast<u64>(-17), str, std::bit_cast<u64>(2.25)}},
        {U"%40s", {longs}}, {U"", {}}, {U"%c%c", {'A', 0xE9}}, {U"%5n|%d", {g.alloc(8), 5}}, {U"%-10s|%%", {str}},
        {U"%d-%d|%40d", {123, 456, 7}}, {U"abcdef%.3f%%", {std::bit_cast<u64>(0.5)}}, {U"%-35d|", {9}}};
    for (const auto& [fmt, args] : wcases)
        for (const u32 n : {0u, 1u, 3u, 50u, 300u}) {
            // A %s whose text does not fit: the game's _WPutstr ignores the NULL its output function answers and calls
            // it again with that NULL, which faults; pspocket stops there (-1). Only text that fits is compared then.
            if (fmt.find(U's') != std::u32string::npos && n < 300) continue;
            if (n == 0) continue; // (the same with no room at all)
            const u32 bg = g.alloc(1300), bo = g.alloc(1300);
            fill_text(g, bg, 300, 2, 0x7E7E);
            fill_text(g, bo, 300, 4, 0x7E7E);
            std::vector<u64> a = {bg, n, wstr16(g, fmt)};
            a.insert(a.end(), args.begin(), args.end());
            a.resize(8, 0);
            const s32 sg = static_cast<s32>(g.call_game("swprintf", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
            a[0] = bo;
            a[2] = wstr32(g, fmt);
            const s32 so = static_cast<s32>(g.call_own("sys_libc", "swprintf", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
            d.compared++;
            if (sg != so || text_at(g, bg, 300, 2) != text_at(g, bo, 300, 4))
                d.add(std::format("swprintf(\"{}\", {}): {} \"{}\" / {} \"{}\"", narrow(fmt), n, sg,
                    show({text_at(g, bg, 8, 2)}), so, show({text_at(g, bo, 8, 4)})));
        }
    d.report("sprintf and kin");
}
