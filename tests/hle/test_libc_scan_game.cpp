// sys_libc's formatted input and number parsing (D-907) compared with games' statically linked copies of the SDK's C
// library (game_executable.hpp), call for call over wide sets of made-up inputs:
// - the parsing (_Stopfx, _Stoflt, _Stoxflt, _Stodx, _Stofx, _Stoldx, _Stoulx, _Stolx, the scaling internals) with
//   Assassin's Creed's copy (PSPOCKET_TEST_AC_EBOOT, PSPOCKET_TEST_AC_LIBC_MAP), and the C functions built on them
//   (strtod, strtol, atoi ...) against the internals they call;
// - the scan engine (_Scanf, _Getfld, _Getint, _Getfloat, _Getstr) and the 64-bit integer conversions with Grand Theft
//   Auto V's copy (PSPOCKET_TEST_GTA5_EBOOT, PSPOCKET_TEST_GTA5_LIBC_MAP), Assassin's Creed linking no scanf. GTA V's
//   newer library parses floats differently (an extended-precision scaling); for the scan comparisons its _Stold is
//   pointed at pspocket's (the parsing itself being compared with Assassin's Creed's), and the differences between the
//   two SDKs' float parsing are counted and reported, not compared.
// test_hle "[gamelibc]"; each part skips without its game's executable and map (game data, kept in inventory/).

#include <catch_amalgamated.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <tuple>
#include <vector>

#include "game_executable.hpp"

using namespace pspocket;
using pspocket::test::GameExecutable;

namespace {

// xorshift64: the same inputs every run.
struct Rng {
    u64 x = 0x2545F4914F6CDD1Dull;
    u64 next()
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    }
    u32 below(u32 n) { return static_cast<u32>(next() % n); }
    bool chance(u32 percent) { return below(100) < percent; }
};

std::string digits(Rng& r, u32 n, const char* set = "0123456789")
{
    const usize k = std::strlen(set);
    std::string s;
    for (u32 i = 0; i < n; i++) s += set[r.below(static_cast<u32>(k))];
    return s;
}

// Floating texts: edge cases, then random decimal, printf-made and hex fields with junk around them.
std::vector<std::string> float_texts(u32 random_count)
{
    std::vector<std::string> v = {"", " ", "0", "-0", "+0", "00", "0.", ".0", ".", "-.", "+.e1", "1", "-1", "1.5",
        "-1.5e3", "0.3", "0.1", "0.2", "0.7", "3.14159", "2.5", "1e1", "15", "1e23", "8.5", "123.456", "1e308",
        "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "2e308", "1e309",
        "4.9e-324", "5e-324", "2.5e-324", "3e-324", "7e-324", "2.4703282292062327e-324",
        "2.2250738585072014e-308", "2.2250738585072011e-308", "1e-310", "1e-320", "1e-400", "1e-999999999",
        "1e9999999999", "123456789012345678901234567890", "0.000000000000000000000000000001",
        "000000000000000001", "1.000000000000000000000000001", "99999999999999999999",
        "9999999999999999999e-19", "123456789012345678", "1234567890123456789", "12345678901234567890",
        "100000000000000000000000", "0.000001", "1.0000000000000002", "0.9999999999999999", "3.4028235e38",
        "3.4028236e38", "1.17549435e-38", "1.4e-45", "7e-46", "1e-45", "16777217", "16777216.5",
        "0x", "0x1", "0X1P4", "0x1.8p3", "0x.8", "0x.", "0x1p-1074", "0x1p-1075", "0x1.fffffffffffffp1023",
        "0x1p1024", "0x123456789abcdef0123", "-0x1.8p-1", "0xg", "0x1p", "0x1p+", "0x1pz", "0x0.0001p0",
        "0x1fffffffffffff", "0x20000000000001", "0x1.000002p0", "0x1.0000018p0", "inf", "INF", "-inf",
        "+Infinity", "infinity", "infinit", "infin", "in", "i", "inx", "-in", "nan", "NAN", "-nan", "+nan",
        "nan(123)", "nan(abc_9)", "nan(", "nan()", "nan(x", "nan)", "nan(a b)", "na", "n", "nax", "-n", "1e",
        "1e+", "1e-", "1ex", "1.e5", "1e5.5", " \t\n1", "--1", "-+1", "+-1", "1,5", "1 5", "\x80" "1",
        "1\x80", "  -0.0e-0", "1e0000000000000000000005", "1e-0000000000000000000005", "0e999999",
        "0.0000000000000000000000000000000000000000000000000000000001e60", "5e-1", "05", "5.", "+.5", "-.5e-2"};
    Rng r;
    char buf[96];
    for (u32 i = 0; i < random_count; i++) {
        std::string s;
        switch (r.below(5)) {
        case 0:
        case 1: { // a decimal field of up to 40 digits, a point anywhere, an exponent
            if (r.chance(30)) s += r.chance(50) ? "-" : "+";
            if (r.chance(20)) s += std::string(r.below(5), '0');
            std::string d = digits(r, 1 + r.below(r.chance(50) ? 20 : 40));
            if (r.chance(70)) d.insert(r.below(static_cast<u32>(d.size()) + 1), ".");
            s += d;
            if (r.chance(60)) {
                s += r.chance(50) ? "e" : "E";
                if (r.chance(60)) s += r.chance(50) ? "-" : "+";
                s += std::to_string(r.chance(90) ? r.below(r.chance(50) ? 40 : 360) : r.below(2000000000));
            }
            break;
        }
        case 2: { // printf's text of a random double
            const double x = std::bit_cast<double>(r.next());
            const int prec = static_cast<int>(r.below(20));
            std::snprintf(buf, sizeof buf, r.chance(50) ? "%.*g" : "%.*e", prec, x);
            s = buf;
            break;
        }
        case 3: { // a value near a power of ten or two, or a float's text
            const double x = std::ldexp(1.0 + static_cast<double>(r.below(1u << 30)) / 1073741824.0,
                static_cast<int>(r.below(2100)) - 1075);
            std::snprintf(buf, sizeof buf, "%.*g", static_cast<int>(1 + r.below(17)), r.chance(50) ? x : static_cast<double>(static_cast<float>(x)));
            s = buf;
            break;
        }
        default: { // hex
            if (r.chance(30)) s += "-";
            s += r.chance(50) ? "0x" : "0X";
            std::string d = digits(r, r.below(22), "0123456789abcdefABCDEF");
            if (r.chance(50)) d.insert(r.below(static_cast<u32>(d.size()) + 1), ".");
            s += d;
            if (r.chance(70)) {
                s += r.chance(50) ? "p" : "P";
                if (r.chance(50)) s += r.chance(50) ? "-" : "+";
                s += std::to_string(r.below(r.chance(80) ? 1100 : 100000));
            }
            break;
        }
        }
        if (r.chance(20)) s += " ;x"[r.below(3)];
        if (r.chance(10)) s = std::string(r.below(3), ' ') + s;
        v.push_back(s);
    }
    return v;
}

// The game's errno word (its _Geterrno) and pspocket's.
u32 game_errno(GameExecutable& g) { return ps_read32(g.b, static_cast<u32>(g.call_game("_Geterrno", {}))); }
u32 own_errno(GameExecutable& g) { return ps_read32(g.b, static_cast<u32>(g.call_own("sys_libc", "_Geterrno", {}))); }
void clear_errno(GameExecutable& g)
{
    ps_write32(g.b, static_cast<u32>(g.call_game("_Geterrno", {})), 0);
    ps_write32(g.b, static_cast<u32>(g.call_own("sys_libc", "_Geterrno", {})), 0);
}

std::string show(const std::string& s)
{
    std::string o;
    for (const char c : s) {
        if (c >= 32 && c < 127) o += c;
        else o += std::format("\\x{:02x}", static_cast<u8>(c));
    }
    return o;
}

struct Tally {
    u64 calls = 0, differences = 0;
    std::vector<std::string> first;
    void differ(std::string what)
    {
        differences++;
        if (first.size() < 25) first.push_back(std::move(what));
    }
    void report(const char* name) const
    {
        std::printf("%s: %llu calls compared, %llu differences\n", name, static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(differences));
        for (const std::string& f : first) std::printf("  %s\n", f.c_str());
    }
};

} // namespace

TEST_CASE("sys_libc's _Stodx, _Stofx and _Stoldx parse as the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    const u32 text = g.alloc(256), end = g.alloc(8), perr = g.alloc(8);
    const std::vector<std::string> texts = float_texts(6000);
    Rng r;
    Tally t;
    for (const std::string& s : texts) {
        std::memset(g.b + text, 0, 256);
        std::memcpy(g.b + text, s.data(), s.size());
        for (const char* fn : {"_Stodx", "_Stofx", "_Stoldx"}) {
            const s32 pten = r.chance(75) ? 0 : static_cast<s32>(r.below(80)) - 40;
            clear_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            ps_write32(g.b, perr, 0x55555555);
            g.call_game(fn, {text, end, static_cast<u64>(static_cast<s64>(pten)), perr});
            const u64 game_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
            const u32 game_end = ps_read32(g.b, end), game_perr = ps_read32(g.b, perr), game_err = game_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            ps_write32(g.b, perr, 0x55555555);
            g.call_own("sys_libc", fn, {text, end, static_cast<u64>(static_cast<s64>(pten)), perr});
            const u64 own_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
            const u32 own_end = ps_read32(g.b, end), own_perr = ps_read32(g.b, perr), own_err = own_errno(g);
            t.calls++;
            if (game_bits != own_bits || game_end != own_end || game_perr != own_perr || game_err != own_err)
                t.differ(std::format("{}(\"{}\", pten {}): game {:#018x} ({}) end +{} perr {} errno {:#x}; own {:#018x} ({}) end "
                                     "+{} perr {} errno {:#x}",
                    fn, show(s), pten, game_bits, std::bit_cast<double>(game_bits), game_end - text, game_perr, game_err,
                    own_bits, std::bit_cast<double>(own_bits), own_end - text, own_perr, own_err));
        }
        // The C functions are the internals without a power of ten or perr.
        for (const auto& [own, game, single] : {std::tuple{"strtod", "_Stodx", false}, std::tuple{"strtold", "_Stoldx", false},
                 std::tuple{"strtof", "_Stofx", true}}) {
            clear_errno(g);
            g.call_game(game, {text, end, 0, 0});
            const u64 game_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
            const u32 game_end = ps_read32(g.b, end), game_err = game_errno(g);
            g.call_own("sys_libc", own, {text, end});
            const u64 own_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
            const u32 own_end = ps_read32(g.b, end), own_err = own_errno(g);
            t.calls++;
            if (game_bits != own_bits || game_end != own_end || game_err != own_err)
                t.differ(std::format("{}(\"{}\"): game {:#018x} end +{} errno {:#x}; own {:#018x} end +{} errno {:#x}", own, show(s),
                    game_bits, game_end - text, game_err, own_bits, own_end - text, own_err));
            (void)single;
        }
        g.call_game("_Stodx", {text, 0, 0, 0});
        const u64 game_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
        g.call_own("sys_libc", "atof", {text});
        t.calls++;
        if (game_bits != std::bit_cast<u64>(g.ctx.fpr[1])) t.differ(std::format("atof(\"{}\")", show(s)));
    }
    t.report("_Stodx/_Stofx/_Stoldx and strtod/strtold/strtof/atof");
    CHECK(t.differences == 0);
}

TEST_CASE("sys_libc's scaling internals are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    const u32 perr = g.alloc(8), px = g.alloc(16), pe = g.alloc(8);
    Rng r;
    Tally t;
    const auto random_double = [&] {
        switch (r.below(4)) {
        case 0: return std::bit_cast<double>(r.next());
        case 1: return std::ldexp(1.0 + static_cast<double>(r.below(1u << 30)) / 1073741824.0, static_cast<int>(r.below(200)) - 100);
        case 2: return static_cast<double>(r.below(1000000000));
        default: return std::bit_cast<double>(r.next() & 0x800FFFFFFFFFFFFFull); // a denormal
        }
    };
    for (u32 i = 0; i < 6000; i++) {
        const double x = random_double();
        const float xf = static_cast<float>(x);
        const s32 n = r.chance(70) ? static_cast<s32>(r.below(120)) - 60 : static_cast<s32>(r.below(1400)) - 700;
        for (const char* fn : {"_Dtentox", "_FDtentox", "_LDtentox"}) {
            const bool single = fn[1] == 'F';
            ps_write32(g.b, perr, 0x10);
            clear_errno(g);
            g.ctx.fpr[1] = single ? static_cast<double>(xf) : x;
            g.call_game(fn, {0, static_cast<u64>(static_cast<s64>(n)), perr});
            const u64 game_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
            const u32 game_perr = ps_read32(g.b, perr), game_err = game_errno(g);
            ps_write32(g.b, perr, 0x10);
            g.ctx.fpr[1] = single ? static_cast<double>(xf) : x;
            g.call_own("sys_libm", fn, {0, static_cast<u64>(static_cast<s64>(n)), perr});
            const u64 own_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
            t.calls++;
            if (game_bits != own_bits || game_perr != ps_read32(g.b, perr) || game_err != own_errno(g))
                t.differ(std::format("{}({:a}, {}): game {:#018x} perr {:#x} errno {:#x}; own {:#018x} perr {:#x} errno {:#x}", fn,
                    single ? static_cast<double>(xf) : x, n, game_bits, game_perr, game_err, own_bits, ps_read32(g.b, perr),
                    own_errno(g)));
        }
        const s32 e = r.chance(70) ? static_cast<s32>(r.below(200)) - 100 : static_cast<s32>(r.below(5000)) - 2500;
        for (const char* fn : {"_Dscale", "_LDscale", "_FDscale"}) {
            const bool single = fn[1] == 'F';
            const auto setx = [&] {
                if (single) ps_write32(g.b, px, std::bit_cast<u32>(xf));
                else ps_write64(g.b, px, std::bit_cast<u64>(x));
            };
            setx();
            const s32 game_r = static_cast<s32>(g.call_game(fn, {px, static_cast<u64>(static_cast<s64>(e))}));
            const u64 game_v = ps_read64(g.b, px);
            setx();
            const s32 own_r = static_cast<s32>(g.call_own("sys_libm", fn, {px, static_cast<u64>(static_cast<s64>(e))}));
            t.calls++;
            if (static_cast<s16>(game_r) != static_cast<s16>(own_r) || game_v != ps_read64(g.b, px))
                t.differ(std::format("{}({:a}, {}): game {} {:#018x}; own {} {:#018x}", fn, single ? static_cast<double>(xf) : x, e,
                    game_r, game_v, own_r, ps_read64(g.b, px)));
        }
        for (const char* fn : {"_Dunscale", "_FDunscale", "_Dnorm", "_FDnorm"}) {
            const bool single = fn[1] == 'F';
            const bool norm = std::string_view(fn).find("norm") != std::string_view::npos;
            const auto setx = [&] {
                if (single) ps_write32(g.b, px, std::bit_cast<u32>(xf));
                else ps_write64(g.b, px, std::bit_cast<u64>(x));
                ps_write32(g.b, pe, 0x7777);
            };
            setx();
            const s32 game_r = static_cast<s32>(norm ? g.call_game(fn, {px}) : g.call_game(fn, {pe, px}));
            const u64 game_v = ps_read64(g.b, px);
            const u32 game_e = ps_read32(g.b, pe);
            setx();
            const s32 own_r = static_cast<s32>(norm ? g.call_own("sys_libm", fn, {px}) : g.call_own("sys_libm", fn, {pe, px}));
            t.calls++;
            if (static_cast<s16>(game_r) != static_cast<s16>(own_r) || game_v != ps_read64(g.b, px) || game_e != ps_read32(g.b, pe))
                t.differ(std::format("{}({:a}): game {} {:#018x} e {:#x}; own {} {:#018x} e {:#x}", fn, single ? static_cast<double>(xf) : x,
                    game_r, game_v, game_e, own_r, ps_read64(g.b, px), ps_read32(g.b, pe)));
        }
    }
    t.report("_Dtentox/_FDtentox/_LDtentox, _Dscale/_FDscale/_LDscale, _Dunscale/_FDunscale, _Dnorm/_FDnorm");
    CHECK(t.differences == 0);
}

TEST_CASE("sys_libc's _Stopfx, _Stoflt and _Stoxflt are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    const u32 text = g.alloc(256), ps = g.alloc(8), end = g.alloc(8), lo = g.alloc(64);
    const std::vector<std::string> texts = float_texts(4000);
    Rng r;
    Tally t;
    for (const std::string& s : texts) {
        std::memset(g.b + text, 0, 256);
        std::memcpy(g.b + text, s.data(), s.size());
        // _Stopfx(&s, endptr): the code, *ps and *endptr.
        for (const bool with_end : {true, false}) {
            ps_write32(g.b, ps, text);
            ps_write32(g.b, end, 0xDEAD);
            const s32 game_r = static_cast<s32>(g.call_game("_Stopfx", {ps, with_end ? end : 0}));
            const u32 game_ps = ps_read32(g.b, ps), game_end = ps_read32(g.b, end);
            ps_write32(g.b, ps, text);
            ps_write32(g.b, end, 0xDEAD);
            const s32 own_r = static_cast<s32>(g.call_own("sys_libc", "_Stopfx", {ps, with_end ? end : 0}));
            t.calls++;
            if (game_r != own_r || game_ps != ps_read32(g.b, ps) || game_end != ps_read32(g.b, end))
                t.differ(std::format("_Stopfx(\"{}\"): game {} ps +{} end {:#x}; own {} ps +{} end {:#x}", show(s), game_r, game_ps - text,
                    game_end, own_r, ps_read32(g.b, ps) - text, ps_read32(g.b, end)));
        }
        // _Stoflt and _Stoxflt from where _Stopfx leaves the digits, with maxsig 1 to 5.
        ps_write32(g.b, ps, text);
        const s32 code = static_cast<s32>(g.call_game("_Stopfx", {ps, 0}));
        const u32 digits_at = ps_read32(g.b, ps);
        const s32 maxsig = 1 + static_cast<s32>(r.below(5));
        for (const char* fn : {"_Stoflt", "_Stoxflt"}) {
            if ((code & 7) != (fn[3] == 'x' ? 2 : 1) && !r.chance(10)) continue;
            std::memset(g.b + lo, 0xA5, 64);
            ps_write32(g.b, end, 0xDEAD);
            const s32 game_r = static_cast<s32>(g.call_game(fn, {text, digits_at, end, lo, static_cast<u64>(maxsig)}));
            const std::string game_lo = g.bytes(lo, 64);
            const u32 game_end = ps_read32(g.b, end);
            std::memset(g.b + lo, 0xA5, 64);
            ps_write32(g.b, end, 0xDEAD);
            const s32 own_r = static_cast<s32>(g.call_own("sys_libc", fn, {text, digits_at, end, lo, static_cast<u64>(maxsig)}));
            t.calls++;
            if (game_r != own_r || game_lo != g.bytes(lo, 64) || game_end != ps_read32(g.b, end)) {
                std::string words;
                for (u32 k = 0; k < 7; k++)
                    words += std::format(" {:08x}/{:08x}", ps_ld32(reinterpret_cast<const u8*>(game_lo.data()) + 4 * k), ps_read32(g.b, lo + 4 * k));
                t.differ(std::format("{}(\"{}\", maxsig {}): game {} end +{}; own {} end +{}; lo game/own{}", fn, show(s), maxsig, game_r,
                    game_end - text, own_r, ps_read32(g.b, end) - text, words));
            }
        }
    }
    t.report("_Stopfx/_Stoflt/_Stoxflt");
    CHECK(t.differences == 0);
}

namespace {

// Integer texts: bases' digits, prefixes, signs (doubled too), white space, leading zeros, long and overflowing values.
std::vector<std::string> int_texts(u32 random_count)
{
    std::vector<std::string> v = {"", " ", "0", "-0", "+0", "00", "0x", "0X", "0x0", "0xg", "-0x", "0x1f", "0X1F", "  -0x1fz",
        "0777", "08", "09", "0b101", "z", "Z", "zz", "-", "+", "--5", "-+5", "+-5", "- 5", "+ 5", " \t\n\v\f\r42",
        "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295", "4294967296", "-4294967295",
        "-4294967296", "9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809",
        "18446744073709551615", "18446744073709551616", "-18446744073709551615", "99999999999", "0xffffffff",
        "0x100000000", "0xffffffffffffffff", "0x10000000000000000", "037777777777", "040000000000",
        "11111111111111111111111111111111", "111111111111111111111111111111111", "000000000000000000000000001",
        "1z", "1Z", "\x80" "5", "5\x80", "0x\x80"};
    Rng r;
    for (u32 i = 0; i < random_count; i++) {
        std::string s;
        if (r.chance(15)) s += std::string(r.below(3), " \t"[r.below(2)]);
        if (r.chance(30)) s += "+-"[r.below(2)];
        if (r.chance(5)) s += "+- "[r.below(3)];
        if (r.chance(20)) s += r.chance(50) ? "0x" : "0X";
        if (r.chance(20)) s += std::string(r.below(4), '0');
        s += digits(r, 1 + r.below(r.chance(70) ? 12 : 30), r.chance(50) ? "0123456789" : "0123456789abcdefghijklmnopqrstuvwxyzABCDEFXYZ");
        if (r.chance(20)) s += " .g-"[r.below(4)];
        v.push_back(s);
    }
    return v;
}

s32 random_base(Rng& r)
{
    static constexpr s32 kBases[] = {0, 0, 0, 10, 10, 16, 16, 8, 2, 36, -1, 1, 37, 3, 7, 35};
    return r.chance(80) ? kBases[r.below(16)] : static_cast<s32>(r.below(40)) - 2;
}

} // namespace

TEST_CASE("sys_libc's _Stoulx, _Stolx and strtol's family are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    const u32 text = g.alloc(256), end = g.alloc(8), perr = g.alloc(8);
    const std::vector<std::string> texts = int_texts(6000);
    Rng r;
    Tally t;
    for (const std::string& s : texts) {
        std::memset(g.b + text, 0, 256);
        std::memcpy(g.b + text, s.data(), s.size());
        const s32 base = random_base(r);
        for (const char* fn : {"_Stoulx", "_Stolx"}) {
            clear_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            ps_write32(g.b, perr, 0x55555555);
            const u32 game_v = static_cast<u32>(g.call_game(fn, {text, end, static_cast<u64>(static_cast<s64>(base)), perr}));
            const u32 game_end = ps_read32(g.b, end), game_perr = ps_read32(g.b, perr), game_err = game_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            ps_write32(g.b, perr, 0x55555555);
            const u32 own_v = static_cast<u32>(g.call_own("sys_libc", fn, {text, end, static_cast<u64>(static_cast<s64>(base)), perr}));
            t.calls++;
            if (game_v != own_v || game_end != ps_read32(g.b, end) || game_perr != ps_read32(g.b, perr) || game_err != own_errno(g))
                t.differ(std::format("{}(\"{}\", {}): game {:#x} end +{} perr {} errno {:#x}; own {:#x} end +{} perr {} errno {:#x}", fn,
                    show(s), base, game_v, game_end - text, game_perr, game_err, own_v, ps_read32(g.b, end) - text,
                    ps_read32(g.b, perr), own_errno(g)));
        }
        // The C functions: strtol is _Stolx and strtoul _Stoulx without perr, atoi and atol (int)_Stoulx(s, 0, 10).
        for (const auto& [own, game, b] : {std::tuple{"strtol", "_Stolx", base}, std::tuple{"strtoul", "_Stoulx", base},
                 std::tuple{"_Stoul", "_Stoulx", base}, std::tuple{"atoi", "_Stoulx", 10}, std::tuple{"atol", "_Stoulx", 10}}) {
            const bool ato = own[0] == 'a';
            clear_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            const u32 game_v = static_cast<u32>(g.call_game(game, {text, ato ? 0u : end, static_cast<u64>(static_cast<s64>(b)), 0}));
            const u32 game_end = ps_read32(g.b, end), game_err = game_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            const u32 own_v = static_cast<u32>(ato ? g.call_own("sys_libc", own, {text})
                                                   : g.call_own("sys_libc", own, {text, end, static_cast<u64>(static_cast<s64>(b))}));
            t.calls++;
            if (game_v != own_v || game_end != ps_read32(g.b, end) || game_err != own_errno(g))
                t.differ(std::format("{}(\"{}\", {}): game {:#x} end {:#x}; own {:#x} end {:#x}", own, show(s), b, game_v, game_end, own_v,
                    ps_read32(g.b, end)));
        }
    }
    t.report("_Stoulx/_Stolx and strtol/strtoul/_Stoul/atoi/atol");
    CHECK(t.differences == 0);
}

// ------------------------------------------------------------------------------------------------- Grand Theft Auto V

namespace {

const char* gta5_eboot() { return std::getenv("PSPOCKET_TEST_GTA5_EBOOT"); }
const char* gta5_map() { return std::getenv("PSPOCKET_TEST_GTA5_LIBC_MAP"); }

#define PS_REQUIRE_GTA5_LIBC()                                                                                       \
    do {                                                                                                             \
        if (!gta5_eboot() || !gta5_map())                                                                            \
            SKIP("PSPOCKET_TEST_GTA5_EBOOT and PSPOCKET_TEST_GTA5_LIBC_MAP are not set: no copy of the SDK's scanf "  \
                 "to compare with");                                                                                 \
    } while (0)

// GTA V's _Stoldx made a jump to pspocket's _Stold (its only caller, _Stold, passes perr 0): GTA V's scan engine then
// parses its floats with the parsing compared with Assassin's Creed's above, so the scan comparisons see the engines.
void route_stold_to_own(GameExecutable& g)
{
    const u32 d = g.hle->link_function("sys_libc", recomp::nid_of("_Stold"), "_Stold");
    REQUIRE(d);
    const u32 at = g.code.at("_Stoldx");
    const u32 words[] = {0x3D800000u | (d >> 16), 0x618C0000u | (d & 0xFFFF), 0x800C0000u, 0x7C0903A6u, 0x4E800420u};
    for (u32 i = 0; i < 5; i++) ps_write32(g.b, at + 4 * i, words[i]);
}

// A scanf call's world: the input string and the pointer the game's string reader moves, eight 64-byte targets.
struct ScanSetup {
    GameExecutable& g;
    u32 input = 0, strp = 0, fmt = 0, targets = 0;
    static constexpr u32 kTargets = 8, kTargetSize = 64;
    explicit ScanSetup(GameExecutable& game) : g(game)
    {
        input = g.alloc(512);
        strp = g.alloc(8);
        fmt = g.alloc(512);
        targets = g.alloc(kTargets * kTargetSize);
    }
    void reset(const std::string& in)
    {
        std::memset(g.b + input, 0, 512);
        std::memcpy(g.b + input, in.data(), std::min<usize>(in.size(), 511));
        ps_write32(g.b, strp, input);
        std::memset(g.b + targets, 0xA5, kTargets * kTargetSize);
    }
    u32 va() // a va_list of the eight targets
    {
        std::vector<u64> a;
        for (u32 i = 0; i < kTargets; i++) a.push_back(targets + i * kTargetSize);
        return g.va_list_of(a);
    }
    std::string state() const { return g.bytes(targets, kTargets * kTargetSize) + g.bytes(strp, 4); }
};

// Random scanf formats and inputs from pieces.
std::string random_directive(Rng& r)
{
    static const char* kConvs[] = {"d", "i", "o", "u", "x", "X", "p", "f", "e", "E", "g", "G", "a", "A", "s", "c", "[", "n", "%"};
    static const char* kQuals[] = {"", "", "", "h", "hh", "l", "ll", "L", "j", "z", "t", "q"};
    static const char* kSets[] = {"a-z", "^,", "]a", "^]a", "a-", "-a", "z-a", "0-9", "^ ", "abc", "^0-9a-f", "-", "^-", "a-c-e"};
    switch (r.below(10)) {
    case 0: return " ";
    case 1: return std::string(1, ",x:-0"[r.below(5)]);
    case 2: return r.chance(50) ? "%%" : "%C";
    default: {
        std::string d = "%";
        if (r.chance(15)) d += "*";
        if (r.chance(35)) d += std::to_string(r.chance(90) ? r.below(12) : 2147483646 + r.below(3));
        const char* conv = kConvs[r.below(19)];
        const bool chars = conv[0] == 's' || conv[0] == 'c' || conv[0] == '[';
        if (conv[0] != '%') {
            const char* q = kQuals[r.below(12)];
            if (!(chars && q[0] == 'l')) d += q; // %ls and kin: compared by character in their own case
        }
        d += conv;
        if (conv[0] == '[') d += std::string(kSets[r.below(14)]) + "]";
        return d;
    }
    }
}

std::string random_input(Rng& r, const std::vector<std::string>& ints, const std::vector<std::string>& floats)
{
    static const char* kWords[] = {"abc", "nan", "NaN(x_1)", "inf", "-Infinity", "infin", "0x", "0X1p3", "1e", "1e+",
        "  ", "\t\n", ",", ":", "x", "]", "-", "+", "%", "%%", "zz9", "\x80\xff", "a-b", "0x1fg", "00012", "-0", ".5", "5."};
    std::string s;
    const u32 n = 1 + r.below(5);
    for (u32 i = 0; i < n && s.size() < 50; i++) {
        switch (r.below(4)) {
        case 0: s += ints[r.below(static_cast<u32>(ints.size()))]; break;
        case 1: s += floats[r.below(static_cast<u32>(floats.size()))]; break;
        default: s += kWords[r.below(28)]; break;
        }
        if (r.chance(60)) s += " ,\t"[r.below(3)];
    }
    return s.substr(0, 60);
}

} // namespace

TEST_CASE("sys_libc's _Stoullx, _Stollx and strtoll's family are GTA V's", "[gamelibc][libc]")
{
    PS_REQUIRE_GTA5_LIBC();
    GameExecutable g(gta5_eboot(), gta5_map());
    const u32 text = g.alloc(256), end = g.alloc(8), perr = g.alloc(8);
    const std::vector<std::string> texts = int_texts(6000);
    Rng r;
    Tally t;
    for (const std::string& s : texts) {
        std::memset(g.b + text, 0, 256);
        std::memcpy(g.b + text, s.data(), s.size());
        const s32 base = random_base(r);
        for (const char* fn : {"_Stoullx", "_Stollx"}) {
            clear_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            ps_write32(g.b, perr, 0x55555555);
            const u64 game_v = g.call_game(fn, {text, end, static_cast<u64>(static_cast<s64>(base)), perr});
            const u32 game_end = ps_read32(g.b, end), game_perr = ps_read32(g.b, perr), game_err = game_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            ps_write32(g.b, perr, 0x55555555);
            const u64 own_v = g.call_own("sys_libc", fn, {text, end, static_cast<u64>(static_cast<s64>(base)), perr});
            t.calls++;
            if (game_v != own_v || game_end != ps_read32(g.b, end) || game_perr != ps_read32(g.b, perr) || game_err != own_errno(g))
                t.differ(std::format("{}(\"{}\", {}): game {:#x} end +{} perr {} errno {:#x}; own {:#x} end +{} perr {} errno {:#x}", fn,
                    show(s), base, game_v, game_end - text, game_perr, game_err, own_v, ps_read32(g.b, end) - text,
                    ps_read32(g.b, perr), own_errno(g)));
        }
        for (const auto& [own, game, b] : {std::tuple{"strtoll", "_Stollx", base}, std::tuple{"strtoull", "_Stoullx", base},
                 std::tuple{"strtoimax", "_Stollx", base}, std::tuple{"strtoumax", "_Stoullx", base},
                 std::tuple{"_Stoll", "_Stollx", base}, std::tuple{"_Stoull", "_Stoullx", base}, std::tuple{"atoll", "_Stoullx", 10}}) {
            const bool ato = own[0] == 'a';
            clear_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            const u64 game_v = g.call_game(game, {text, ato ? 0u : end, static_cast<u64>(static_cast<s64>(b)), 0});
            const u32 game_end = ps_read32(g.b, end), game_err = game_errno(g);
            ps_write32(g.b, end, 0xDEAD);
            const u64 own_v = ato ? g.call_own("sys_libc", own, {text}) : g.call_own("sys_libc", own, {text, end, static_cast<u64>(static_cast<s64>(b))});
            t.calls++;
            if (game_v != own_v || game_end != ps_read32(g.b, end) || game_err != own_errno(g))
                t.differ(std::format("{}(\"{}\", {}): game {:#x} end {:#x}; own {:#x} end {:#x}", own, show(s), b, game_v, game_end, own_v,
                    ps_read32(g.b, end)));
        }
    }
    t.report("_Stoullx/_Stollx and strtoll/strtoull/strtoimax/strtoumax/_Stoll/_Stoull/atoll (GTA V)");
    CHECK(t.differences == 0);
}

// The two SDKs' float parsing side by side: reported, not compared (Assassin's Creed's is sys_libc's, D-907).
TEST_CASE("The float parsing of GTA V's newer SDK, counted against sys_libc's", "[gamelibc][libc]")
{
    PS_REQUIRE_GTA5_LIBC();
    GameExecutable g(gta5_eboot(), gta5_map());
    const u32 text = g.alloc(256), end = g.alloc(8);
    const std::vector<std::string> texts = float_texts(6000);
    u64 calls = 0, values = 0, ends = 0;
    std::vector<std::string> first;
    for (const std::string& s : texts) {
        std::memset(g.b + text, 0, 256);
        std::memcpy(g.b + text, s.data(), s.size());
        g.call_game("_Stodx", {text, end, 0, 0});
        const u64 game_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
        const u32 game_end = ps_read32(g.b, end);
        g.call_own("sys_libc", "_Stodx", {text, end, 0, 0});
        const u64 own_bits = std::bit_cast<u64>(g.ctx.fpr[1]);
        calls++;
        if (game_bits != own_bits) values++;
        if (game_end != ps_read32(g.b, end)) ends++;
        if ((game_bits != own_bits || game_end != ps_read32(g.b, end)) && first.size() < 12)
            first.push_back(std::format("\"{}\": GTA V {:a} end +{}, sys_libc {:a} end +{}", show(s), std::bit_cast<double>(game_bits),
                game_end - text, std::bit_cast<double>(own_bits), ps_read32(g.b, end) - text));
    }
    std::printf("_Stodx, GTA V's against sys_libc's (Assassin's Creed's): %llu texts, %llu values and %llu end pointers differ\n",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(values), static_cast<unsigned long long>(ends));
    for (const std::string& f : first) std::printf("  %s\n", f.c_str());
    SUCCEED();
}

TEST_CASE("sys_libc's _Scanf is GTA V's", "[gamelibc][libc]")
{
    PS_REQUIRE_GTA5_LIBC();
    GameExecutable g(gta5_eboot(), gta5_map());
    route_stold_to_own(g);
    ScanSetup sc(g);
    const u32 scin = g.game("sscanf_scin");
    const std::vector<std::string> ints = int_texts(400), floats = float_texts(400);
    struct Case {
        std::string fmt, in;
    };
    std::vector<Case> cases = {{"%d", "42"}, {"%d", "  -17x"}, {"%d", ""}, {"%d", "x"}, {"%d %d", "1 2"}, {"%d,%d", "1, 2"},
        {"%5d", "1234567"}, {"%hd", "70000"}, {"%hhd", "300"}, {"%lld", "-9223372036854775809"}, {"%jd", "99999999999999999999"},
        {"%Ld", "4294967297"}, {"%zd", "5"}, {"%i", "0x1f"}, {"%i", "017"}, {"%i", "-0X"}, {"%x", "0x"}, {"%x", "0xg"},
        {"%2x", "0x1"}, {"%o", "0x7"}, {"%u", "-1"}, {"%p", "0x10000"}, {"%f", "1.5"}, {"%lf", "1e"}, {"%lf", "1e+"},
        {"%lf", "0x1.8p3"}, {"%lf", "nan(abc)x"}, {"%lf", "nan(abc"}, {"%lf", "-infinity"}, {"%lf", "infinx"},
        {"%lf", "infinity"}, {"%3lf", "nan"}, {"%2lf", "nan"}, {"%Lf", ".5e-3"}, {"%f", "3.4028236e38"}, {"%s", "  word next"},
        {"%3s", "abcdef"}, {"%c", " x"}, {"%3c", "ab"}, {"%[a-z]", "abc123"}, {"%[^,],%s", "one,two"}, {"%[]a]", "]a]b"},
        {"%[a-]", "-a-b"}, {"%[z-a]", "zza"}, {"%[", "abc"}, {"%5[0-9]", "1234567"}, {"%d%n", "12 "}, {"%n", ""},
        {"%*d %d", "1 2"}, {"%%", " %"}, {"%%", "x"}, {"%%", ""}, {"%C", "x"}, {"x%d", "y5"}, {"x%d", ""}, {" %d", " "},
        {"%d ", "5"}, {"%", "5"}, {"%5", "5"}, {"%h", "5"}, {"%y", "5"},
        {"%2147483647d", "12"}, {"%2147483648d", "12"}, {"%99999999999d", "12"}, {"%hhn%d", "12"},
        {"%f %e %g %a", "1 2.5 -3e2 0x1p-2"}, {"%d%s%c%f", "12ab c1.25"}};
    Rng r;
    for (u32 i = 0; i < 20000; i++) {
        std::string fmt;
        const u32 n = 1 + r.below(4);
        for (u32 k = 0; k < n; k++) fmt += random_directive(r);
        cases.push_back({fmt, random_input(r, ints, floats)});
    }
    Tally t;
    for (const Case& c : cases) {
        std::memset(g.b + sc.fmt, 0, 512);
        std::memcpy(g.b + sc.fmt, c.fmt.data(), c.fmt.size());
        sc.reset(c.in);
        clear_errno(g);
        const s32 game_r = static_cast<s32>(g.call_game("_Scanf", {scin, sc.strp, sc.fmt, sc.va()}));
        const std::string game_state = sc.state();
        const u32 game_err = game_errno(g) | own_errno(g);
        sc.reset(c.in);
        clear_errno(g);
        const s32 own_r = static_cast<s32>(g.call_own("sys_libc", "_Scanf", {scin, sc.strp, sc.fmt, sc.va()}));
        t.calls++;
        if (game_r != own_r || game_state != sc.state() || game_err != own_errno(g)) {
            std::string where;
            const std::string own_state = sc.state();
            for (usize k = 0; k < game_state.size(); k++)
                if (game_state[k] != own_state[k]) {
                    where = std::format(" first byte differing {} (target {}): game {:02x} own {:02x}", k, k / ScanSetup::kTargetSize,
                        static_cast<u8>(game_state[k]), static_cast<u8>(own_state[k]));
                    break;
                }
            t.differ(std::format("_Scanf(\"{}\", \"{}\"): game {} own {} errno {:#x}/{:#x} consumed {}/{}{}", show(c.fmt), show(c.in), game_r,
                own_r, game_err, own_errno(g), ps_ld32(game_state.data() + game_state.size() - 4) - sc.input,
                ps_read32(g.b, sc.strp) - sc.input, where));
        }
    }
    t.report("_Scanf (GTA V)");
    CHECK(t.differences == 0);
}

// %ls, %lc and %l[ (and %S): GTA V's wchar_t is 16 bits, sys_libc's 32 (D-512); the same characters and counts.
// Inputs are ASCII: a byte past 127 is a multibyte question (GTA V's copy, never started, decodes UTF-8 there; sys_libc's
// "C" locale has single bytes, D-512).
TEST_CASE("sys_libc's wide-character scan targets are GTA V's, character for character", "[gamelibc][libc]")
{
    PS_REQUIRE_GTA5_LIBC();
    GameExecutable g(gta5_eboot(), gta5_map());
    route_stold_to_own(g);
    const u32 scin = g.game("sscanf_scin");
    const u32 input = g.alloc(512), strp = g.alloc(8), fmt = g.alloc(64);
    constexpr u32 kSize = 512;
    const u32 targets = g.alloc(2 * kSize);
    const u32 va = g.va_list_of({targets, targets + kSize});
    const std::vector<std::string> ints = int_texts(200), floats = float_texts(200);
    struct Wide {
        const char* fmt;
        char kinds[3]; // per target: 'w' wide characters, 'i' an int
    };
    static const Wide kWide[] = {{"%ls", "w"}, {"%3ls", "w"}, {"%lc", "w"}, {"%4lc", "w"}, {"%l[a-z]", "w"}, {"%2l[^ ]", "w"},
        {"%S", "w"}, {"%5S", "w"}, {"%*ls %d", "i"}, {"%ls%n", "wi"}, {"%lc%lc", "ww"}, {"%*l[0-9]%lc", "w"}};
    Rng r;
    Tally t;
    for (u32 i = 0; i < 3000; i++) {
        const Wide& w = kWide[r.below(12)];
        std::string in = random_input(r, ints, floats);
        for (char& c : in)
            if (static_cast<u8>(c) > 127) c = 'q';
        std::memset(g.b + fmt, 0, 64);
        std::memcpy(g.b + fmt, w.fmt, std::strlen(w.fmt));
        const auto run = [&](bool game) {
            std::memset(g.b + input, 0, 512);
            std::memcpy(g.b + input, in.data(), in.size());
            ps_write32(g.b, strp, input);
            std::memset(g.b + targets, 0xA5, 2 * kSize);
            const s32 res = static_cast<s32>(game ? g.call_game("_Scanf", {scin, strp, fmt, va}) : g.call_own("sys_libc", "_Scanf", {scin, strp, fmt, va}));
            std::vector<u32> seen{static_cast<u32>(res), ps_read32(g.b, strp) - input};
            for (u32 k = 0; w.kinds[k]; k++) {
                const u32 at = targets + k * kSize;
                if (w.kinds[k] == 'i') {
                    seen.push_back(ps_read32(g.b, at));
                    continue;
                }
                for (u32 j = 0; j < kSize / 4; j++) { // the characters up to the untouched fill
                    const u32 c = game ? ps_read16(g.b, at + 2 * j) : ps_read32(g.b, at + 4 * j);
                    if (c == (game ? 0xA5A5u : 0xA5A5A5A5u)) break;
                    seen.push_back(c);
                }
                seen.push_back(0xFFFFFFFFu);
            }
            return seen;
        };
        const std::vector<u32> game_seen = run(true), own_seen = run(false);
        t.calls++;
        if (game_seen != own_seen) t.differ(std::format("_Scanf(\"{}\", \"{}\"): game {} own {}", w.fmt, show(in), game_seen[0], own_seen[0]));
    }
    t.report("_Scanf %ls/%lc/%l[/%S (GTA V, by character)");
    CHECK(t.differences == 0);
}

// _Getfld, _Getint, _Getfloat and _Getstr called on an _Sft made here (random counts, widths, qualifiers, flags): the
// answer, every byte of the _Sft after it, the input consumed and the targets.
TEST_CASE("sys_libc's _Getfld, _Getint, _Getfloat and _Getstr are GTA V's", "[gamelibc][libc]")
{
    PS_REQUIRE_GTA5_LIBC();
    GameExecutable g(gta5_eboot(), gta5_map());
    route_stold_to_own(g);
    ScanSetup sc(g);
    const u32 scin = g.game("sscanf_scin");
    const u32 px = g.alloc(64);
    const std::vector<std::string> ints = int_texts(400), floats = float_texts(400);
    static const char* kSets[] = {"[a-z]", "[^,]", "[]a]", "[^]a]", "[a-]", "[-a]", "[z-a]", "[0-9]", "[^ ]", "[abc", "[", "[^0-9a-f]"};
    static const u8 kQuals[] = {0, 0, 'h', 'b', 'l', 'q', 'j', 'L', 't', 'z'};
    Rng r;
    Tally t;
    for (u32 i = 0; i < 20000; i++) {
        const char* fn;
        std::string conv;
        s32 sfl = 0;
        u8 qual = kQuals[r.below(10)];
        switch (r.below(4)) {
        case 0:
            fn = "_Getint";
            conv = std::string(1, "diouxXp"[r.below(7)]);
            break;
        case 1:
            fn = "_Getfloat";
            conv = std::string(1, "aAeEfFgG"[r.below(8)]);
            break;
        case 2:
            fn = "_Getstr";
            sfl = static_cast<s32>(r.below(3)) - 1;
            conv = sfl < 0 ? kSets[r.below(12)] : sfl == 0 ? "c" : "s";
            if (qual == 'l') qual = 0; // compared by character above
            break;
        default:
            fn = "_Getfld";
            conv = std::string(1, "%scn[aAeEfFgGXdiopuxyC"[r.below(22)]);
            if (conv == "[") conv = kSets[r.below(12)];
            if (qual == 'l' && (conv[0] == 's' || conv[0] == 'c' || conv[0] == '[')) qual = 0;
            break;
        }
        const std::string in = random_input(r, ints, floats);
        std::memset(g.b + sc.fmt, 0, 512);
        std::memcpy(g.b + sc.fmt, conv.data(), conv.size());
        u8 sft[32];
        const u32 va = sc.va();
        const s32 width = r.chance(30) ? 0 : r.chance(10) ? -static_cast<s32>(r.below(3)) : static_cast<s32>(1 + r.below(r.chance(90) ? 12 : 100));
        const auto put = [&] {
            ps_write32(g.b, px, scin);
            ps_write32(g.b, px + 4, sc.strp);
            ps_write32(g.b, px + 8, va);
            ps_write32(g.b, px + 12, sc.fmt);
            ps_write32(g.b, px + 16, r.below(50));
            ps_write32(g.b, px + 20, static_cast<u32>(r.next()));
            ps_write32(g.b, px + 24, static_cast<u32>(width));
            g.b[px + 28] = r.chance(20) ? '*' : r.chance(5) ? 1 : 0;
            g.b[px + 29] = qual;
            g.b[px + 30] = static_cast<u8>(r.below(3));
            g.b[px + 31] = 0x5A;
            std::memcpy(sft, g.b + px, 32);
        };
        put();
        sc.reset(in);
        const s32 game_r = sfl || std::string_view(fn) == "_Getstr" ? static_cast<s32>(g.call_game(fn, {px, static_cast<u64>(static_cast<s64>(sfl))}))
                                                                     : static_cast<s32>(g.call_game(fn, {px}));
        const std::string game_state = sc.state() + g.bytes(px, 32);
        std::memcpy(g.b + px, sft, 32);
        sc.reset(in);
        const s32 own_r = sfl || std::string_view(fn) == "_Getstr" ? static_cast<s32>(g.call_own("sys_libc", fn, {px, static_cast<u64>(static_cast<s64>(sfl))}))
                                                                    : static_cast<s32>(g.call_own("sys_libc", fn, {px}));
        const std::string own_state = sc.state() + g.bytes(px, 32);
        t.calls++;
        if (game_r != own_r || game_state != own_state) {
            std::string where;
            for (usize k = 0; k < game_state.size(); k++)
                if (game_state[k] != own_state[k]) {
                    where = std::format(" first byte differing {} ({}): game {:02x} own {:02x}", k,
                        k >= game_state.size() - 32 ? std::format("_Sft +{}", k - (game_state.size() - 32)) : std::format("target {}", k / ScanSetup::kTargetSize),
                        static_cast<u8>(game_state[k]), static_cast<u8>(own_state[k]));
                    break;
                }
            t.differ(std::format("{}(\"{}\", sfl {}, width {}, qual {:#x}, noconv {}, in \"{}\"): game {} own {}{}", fn, show(conv), sfl, width,
                qual, sft[28], show(in), game_r, own_r, where));
        }
    }
    t.report("_Getfld/_Getint/_Getfloat/_Getstr (GTA V)");
    CHECK(t.differences == 0);
}
