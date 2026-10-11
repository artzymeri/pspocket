// sys_libc's formatted input and number parsing (D-907): the SDK's scan engine (_Scanf, _Getfld, _Getint, _Getfloat,
// _Getstr and their wide forms) and its digit-array parsing (_Stopfx, _Stoflt, _Stoxflt, the _Sto* conversions and their
// scaling), pinned without any game. tests/hle/test_libc_scan_game.cpp compares the same functions with a game's copy.

#include <catch_amalgamated.hpp>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kText = kData + 0x10000, kFmt = kData + 0x8000, kOut = kData + 0x9000, kEnd = kData + 0x9800,
              kCode = kData + 0xA000, kSft = kData + 0xB000, kVa = kData + 0xB100, kCursor = kData + 0xB200;

void put(u8* b, u32 at, std::string_view s)
{
    std::memcpy(b + at, s.data(), s.size());
    b[at + s.size()] = 0;
}

void wput(u8* b, u32 at, std::u32string_view s)
{
    for (usize i = 0; i < s.size(); i++) ps_write32(b, at + 4 * static_cast<u32>(i), static_cast<u32>(s[i]));
    ps_write32(b, at + 4 * static_cast<u32>(s.size()), 0);
}

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }
u64 bits(double d) { return std::bit_cast<u64>(d); }

constexpr u32 kERANGE = 0x8001001Cu; // lv2's CELL_ERANGE, the library's ERANGE

struct Libc {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    u32 errno_word() { return ps_read32(b, static_cast<u32>(t.call("_Geterrno"))); }
    void clear_errno() { ps_write32(b, static_cast<u32>(t.call("_Geterrno")), 0); }
    // strtod's family on `s` at kText: the value (f1) and the end pointer's offset.
    double fcall(const char* fn, std::string_view s, s64 pten = 0, u32 perr = 0)
    {
        put(b, kText, s);
        ps_write32(b, kEnd, 0xDEAD);
        clear_errno();
        if (std::string_view(fn).starts_with("_Sto")) {
            if (std::string_view(fn).ends_with("x")) t.call(fn, kText, kEnd, pten, perr);
            else t.call(fn, kText, kEnd, pten);
        } else {
            t.call(fn, kText, kEnd);
        }
        return t.ctx.fpr[1];
    }
    u32 end() { return ps_read32(b, kEnd) - kText; }
    u64 icall(const char* fn, std::string_view s, s32 base)
    {
        put(b, kText, s);
        ps_write32(b, kEnd, 0xDEAD);
        clear_errno();
        return t.call(fn, kText, kEnd, static_cast<u64>(static_cast<s64>(base)));
    }
};

// Guest input functions as the SDK's string readers are: pfn(cursor, c, get) gives the next byte (EOF at the NUL,
// staying there) or pushes c back; the wide one reads 32-bit characters.
u32 guest_getc(u8* b, u32 at, bool wide)
{
    const u32 words[] = {0x80C30000u, 0x2C050000u, 0x41820028u, wide ? 0x80E60000u : 0x88E60000u, 0x2C070000u, 0x41820014u,
        wide ? 0x38C60004u : 0x38C60001u, 0x90C30000u, 0x7CE33B78u, 0x4E800020u, 0x3860FFFFu, 0x4E800020u,
        wide ? 0x38C6FFFCu : 0x38C6FFFFu, 0x90C30000u, 0x7C832378u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0x40, at);
    ps_write32(b, at + 0x44, 0);
    return at + 0x40;
}

// Mixed decimal, exponent, hex, long and special number texts, the same every run.
std::vector<std::string> mixed_numbers(usize n)
{
    std::vector<std::string> v;
    u64 x = 0x9E3779B97F4A7C15ull;
    const auto next = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    char buf[64];
    for (usize i = 0; i < n; i++) {
        const u64 r = next();
        switch (r % 6) {
        case 0: std::snprintf(buf, sizeof buf, "%d", static_cast<int>(r >> 20)); break;
        case 1: std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(r % 1000000) / 977.0); break;
        case 2: std::snprintf(buf, sizeof buf, "%.17g", std::bit_cast<double>((r & 0x3FFFFFFFFFFFFFFFull) | 0x3000000000000000ull)); break;
        case 3: std::snprintf(buf, sizeof buf, "-%.3e", static_cast<double>(r % 100000) * 1.5e-7); break;
        case 4: std::snprintf(buf, sizeof buf, "%a", static_cast<double>(r % 4096) / 64.0); break;
        default: std::snprintf(buf, sizeof buf, "%llu.%llu", static_cast<unsigned long long>(r % 1000),
                     static_cast<unsigned long long>(r >> 40)); break;
        }
        v.emplace_back(buf);
    }
    return v;
}

} // namespace

// Timing of strtod and sscanf over a few thousand mixed values (the report's before and after numbers, D-907): the
// functions called through their registered bodies, looked up once.
TEST_CASE("sys_libc: strtod and sscanf timing", "[.libcbench]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const std::vector<std::string> nums = mixed_numbers(4000);
    const Function& strtod_fn = GuestThread::find("strtod");
    const Function& sscanf_fn = GuestThread::find("sscanf");
    std::vector<std::string> lines;
    for (usize i = 0; i + 1 < nums.size(); i += 2)
        lines.push_back(std::to_string(i * 7919) + " " + nums[i] + " word" + std::to_string(i) + " 1f3a");
    constexpr int kReps = 20;
    double sum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < kReps; rep++) {
        for (const std::string& s : nums) {
            put(b, kText, s);
            t.ctx.gpr[3] = kText;
            t.ctx.gpr[4] = kEnd;
            strtod_fn.fn(t.ctx, b);
            sum += t.ctx.fpr[1];
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    put(b, kFmt, "%d %f %s %x");
    for (int rep = 0; rep < kReps; rep++) {
        for (const std::string& line : lines) {
            put(b, kText, line);
            t.ctx.gpr[3] = kText;
            t.ctx.gpr[4] = kFmt;
            t.ctx.gpr[5] = kOut;
            t.ctx.gpr[6] = kOut + 8;
            t.ctx.gpr[7] = kOut + 16;
            t.ctx.gpr[8] = kOut + 80;
            sscanf_fn.fn(t.ctx, b);
        }
    }
    const auto t2 = std::chrono::steady_clock::now();
    const double strtod_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kReps * static_cast<double>(nums.size()));
    const double sscanf_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / (kReps * static_cast<double>(lines.size()));
    std::printf("strtod: %.0f ns per call, sscanf (4 conversions): %.0f ns per call (checksum %g)\n", strtod_ns,
        sscanf_ns, sum);
    CHECK(strtod_ns > 0);
}

TEST_CASE("sys_libc: strtod's family parses as the SDK's C library", "[hle][libc]")
{
    Libc l;
    // The digits as words of nine, scaled by a product of powers of ten applied with one rounding: not the correctly
    // rounded value (Assassin's Creed's copy gives the same, tests/hle/test_libc_scan_game.cpp).
    CHECK(bits(l.fcall("strtod", "  -1.5e3xyz")) == 0xC097700000000001ull);
    CHECK(l.end() == 8);
    CHECK(bits(l.fcall("strtod", "0.3")) == 0x3FD3333333333334ull);
    CHECK(bits(l.fcall("strtod", "123.456")) == 0x405EDD2F1A9FBE76ull);
    CHECK(bits(l.fcall("strtod", "1e23")) == 0x44B52D02C7E14AF6ull);
    CHECK(l.fcall("strtod", "12") == 12.0);
    // DBL_MAX's text overflows; ERANGE only for a zero or infinite result; a denormal one truncated.
    CHECK(std::isinf(l.fcall("strtod", "1.7976931348623157e308")));
    CHECK(l.errno_word() == kERANGE);
    CHECK(bits(l.fcall("strtod", "5e-324")) == 1);
    CHECK(l.errno_word() == 0);
    CHECK(bits(l.fcall("strtod", "3e-324")) == 0);
    CHECK(l.errno_word() == kERANGE);
    CHECK(bits(l.fcall("strtod", "2.2250738585072014e-308")) == 0x000FFFFFFFFFFFFFull);
    CHECK(std::isinf(l.fcall("strtod", "1e400")));
    CHECK(l.errno_word() == kERANGE);
    CHECK(bits(l.fcall("strtod", "-1e-400")) == 0x8000000000000000ull);
    CHECK(l.errno_word() == kERANGE);
    CHECK(l.fcall("strtod", "0e999999") == 0.0);
    CHECK(l.errno_word() == 0);
    // The prefix: a word that fails half way leaves the end pointer where it failed; a NaN has no sign; "0x" is hex
    // whatever follows; no digits roll back to the start, keeping the sign.
    CHECK(l.fcall("strtod", "inx") == 0.0);
    CHECK(l.end() == 2);
    CHECK(l.fcall("strtod", "-i") == 0.0);
    CHECK(l.end() == 2);
    CHECK(bits(l.fcall("strtod", "-x")) == 0x8000000000000000ull);
    CHECK(l.end() == 0);
    CHECK(l.fcall("strtod", "0x") == 0.0);
    CHECK(l.end() == 0);
    CHECK(l.fcall("strtod", "0xg") == 0.0);
    CHECK(l.end() == 0);
    CHECK(bits(l.fcall("strtod", "-nan(abc)x")) == 0x7FF8000000000000ull);
    CHECK(l.end() == 9);
    CHECK(std::isnan(l.fcall("strtod", "nan)")));
    CHECK(l.end() == 4);
    CHECK(std::isnan(l.fcall("strtod", "nan(a b)")));
    CHECK(l.end() == 3);
    CHECK(l.fcall("strtod", "-infinity") == -std::numeric_limits<double>::infinity());
    CHECK(l.end() == 9);
    CHECK(std::isinf(l.fcall("strtod", "infinit")));
    CHECK(l.end() == 3);
    CHECK(l.fcall("strtod", "1e+") == 1.0);
    CHECK(l.end() == 1);
    CHECK(l.fcall("strtod", "1e") == 1.0);
    CHECK(l.end() == 1);
    CHECK(l.fcall("strtod", "  +.5") == 0.5);
    CHECK(l.end() == 5);
    CHECK(l.fcall("strtod", ".") == 0.0);
    CHECK(l.end() == 0);
    // Hex: the words times 2^28, scaled by the binary exponent with no range check.
    CHECK(l.fcall("strtod", "0x1.8p3") == 12.0);
    CHECK(l.end() == 7);
    CHECK(l.fcall("strtod", "0x1p-1075") == 0.0);
    CHECK(l.errno_word() == 0);
    CHECK(std::isinf(l.fcall("strtod", "0x1p1024")));
    CHECK(l.errno_word() == 0);
    // strtold and atof are strtod; strtof scales in float (its 1e-32 one float below the nearest).
    CHECK(bits(l.fcall("strtold", "0.3")) == 0x3FD3333333333334ull);
    put(l.b, kText, "0.3");
    l.t.call("atof", kText);
    CHECK(bits(l.t.ctx.fpr[1]) == 0x3FD3333333333334ull);
    CHECK(std::bit_cast<u32>(static_cast<float>(l.fcall("strtof", "2e-24"))) == 0x181ABE14u);
    CHECK(static_cast<float>(l.fcall("strtof", "0.5")) == 0.5f);
    // _Stod adds a power of ten to the digits' own; the x forms set *perr (0 or 1) and errno.
    const u64 shifted = bits(l.fcall("strtod", "1.5e2"));
    CHECK(bits(l.fcall("_Stod", "1.5", 2)) == shifted);
    CHECK(l.end() == 3);
    ps_write32(l.b, kOut, 7);
    CHECK(std::isinf(l.fcall("_Stodx", "1e999", 0, kOut)));
    CHECK(ps_read32(l.b, kOut) == 1);
    CHECK(l.errno_word() == kERANGE);
    ps_write32(l.b, kOut, 7);
    l.fcall("_Stofx", "2.5", 0, kOut);
    CHECK(ps_read32(l.b, kOut) == 0);
    // The wide forms: the same over 32-bit characters; one past 127 ends the number.
    wput(l.b, kText, U"  -1.5e3");
    l.t.call("wcstod", kText, kEnd);
    CHECK(bits(l.t.ctx.fpr[1]) == 0xC097700000000001ull);
    CHECK(ps_read32(l.b, kEnd) == kText + 4 * 8);
    wput(l.b, kText, U"12٣");
    l.t.call("wcstod", kText, kEnd);
    CHECK(l.t.ctx.fpr[1] == 12.0);
    CHECK(ps_read32(l.b, kEnd) == kText + 8);
    wput(l.b, kText, U"0x1p3");
    l.t.call("_WStof", kText, kEnd, 0);
    CHECK(l.t.ctx.fpr[1] == 8.0);
}

TEST_CASE("sys_libc: _Stopfx, _Stoflt and _Stoxflt as the SDK's", "[hle][libc]")
{
    Libc l;
    const auto prefix = [&](std::string_view s, bool with_end) {
        put(l.b, kText, s);
        ps_write32(l.b, kOut, kText);
        ps_write32(l.b, kEnd, 0xDEAD);
        return i32(l.t.call("_Stopfx", kOut, with_end ? kEnd : 0));
    };
    CHECK(prefix("  -inf", true) == 11); // FL_INF | FL_NEG
    CHECK(ps_read32(l.b, kOut) == kText + 6);
    CHECK(ps_read32(l.b, kEnd) == kText + 6);
    CHECK(prefix("-nan(x)", true) == 4); // no sign for a NaN
    CHECK(ps_read32(l.b, kOut) == kText + 7);
    CHECK(prefix("0x1p", true) == 2);
    CHECK(ps_read32(l.b, kOut) == kText + 2);
    CHECK(ps_read32(l.b, kEnd) == 0xDEAD); // the digits' step sets it
    CHECK(prefix("-12", false) == 9);
    CHECK(ps_read32(l.b, kOut) == kText + 1);
    CHECK(prefix("inq", true) == 0);
    CHECK(ps_read32(l.b, kEnd) == kText + 2);
    const auto digits = [&](const char* fn, std::string_view s, s32 maxsig) {
        put(l.b, kText, s);
        std::memset(l.b + kOut, 0xA5, 32);
        return i32(l.t.call(fn, kText, kText, kEnd, kOut, static_cast<u64>(maxsig)));
    };
    const auto lo = [&](u32 i) { return static_cast<s32>(ps_read32(l.b, kOut + 4 * i)); };
    CHECK(digits("_Stoflt", "123.456e2;", 2) == 1);
    CHECK(lo(0) == -4);
    CHECK(lo(1) == 123456000);
    CHECK(ps_read32(l.b, kEnd) == kText + 9);
    // The first excess integer digit above 4 rounds the last word up (here the second one, after a 4).
    CHECK(digits("_Stoflt", "1234567891234567894999", 2) == 2);
    CHECK(lo(0) == 4);
    CHECK(lo(1) == 123456789);
    CHECK(lo(2) == 123456790);
    // Excess fraction digits are dropped; zeros after the point counted; whole zero words dropped.
    CHECK(digits("_Stoflt", "0.1234567899", 1) == 1);
    CHECK(lo(0) == -9);
    CHECK(lo(1) == 123456789);
    CHECK(digits("_Stoflt", "0.000123", 1) == 1);
    CHECK(lo(0) == -12);
    CHECK(lo(1) == 123000000);
    CHECK(digits("_Stoflt", "1000000000", 2) == 1);
    CHECK(lo(0) == 1);
    CHECK(lo(1) == 100000000);
    CHECK(digits("_Stoflt", "x", 2) == 0);
    CHECK(ps_read32(l.b, kEnd) == kText);
    CHECK(digits("_Stoxflt", "1.8p3", 2) == 1);
    CHECK(lo(0) == -21);
    CHECK(lo(1) == 0x1800000);
    wput(l.b, kText, U"1.8p3");
    std::memset(l.b + kOut, 0xA5, 32);
    CHECK(i32(l.t.call("_WStoxflt", kText, kText, kEnd, kOut, 2)) == 1);
    CHECK(lo(1) == 0x1800000);
    CHECK(ps_read32(l.b, kEnd) == kText + 4 * 5);
    wput(l.b, kText, U"  -inf");
    ps_write32(l.b, kEnd, kText);
    CHECK(i32(l.t.call("_WStopfx", kEnd, 0)) == 11);
    CHECK(ps_read32(l.b, kEnd) == kText + 4 * 6);
}

TEST_CASE("sys_libc: strtol's family as the SDK's", "[hle][libc]")
{
    Libc l;
    CHECK(i32(l.icall("strtol", "  -0x1fz", 0)) == -31);
    CHECK(l.end() == 7);
    // "0x" is taken whatever follows: alone it converts nothing.
    CHECK(l.icall("strtol", "0x", 16) == 0);
    CHECK(l.end() == 0);
    CHECK(l.icall("strtoul", "0xg", 0) == 0);
    CHECK(l.end() == 0);
    // A bad base: no errno.
    CHECK(l.icall("strtol", "12", 1) == 0);
    CHECK(l.end() == 0);
    CHECK(l.errno_word() == 0);
    // strtol's own sign, then strtoul's.
    CHECK(static_cast<u32>(l.icall("strtol", "--5", 10)) == 0x80000000u);
    CHECK(l.errno_word() == kERANGE);
    CHECK(l.end() == 3);
    CHECK(i32(l.icall("strtol", "-+5", 10)) == -5);
    CHECK(i32(l.icall("strtol", "+-5", 10)) == 0x7FFFFFFF);
    CHECK(i32(l.icall("strtol", "- 5", 10)) == -5);
    CHECK(static_cast<u32>(l.icall("strtoul", "-1", 10)) == 0xFFFFFFFFu);
    CHECK(static_cast<u32>(l.icall("strtoul", "-4294967296", 10)) == 0xFFFFFFFFu);
    CHECK(l.errno_word() == kERANGE);
    CHECK(l.icall("strtoll", "9223372036854775808", 10) == 0x7FFFFFFFFFFFFFFFull);
    CHECK(l.errno_word() == kERANGE);
    // atoi and atol are (int)_Stoul(s, 0, 10): a value past INT_MAX wraps; atoll (long long)_Stoull.
    const auto ato = [&](const char* fn, std::string_view s) {
        put(l.b, kText, s);
        l.clear_errno();
        return l.t.call(fn, kText);
    };
    CHECK(i32(ato("atoi", "4294967295")) == -1);
    CHECK(i32(ato("atoi", "2147483648")) == INT32_MIN);
    CHECK(i32(ato("atoi", "99999999999")) == -1);
    CHECK(l.errno_word() == kERANGE);
    CHECK(i32(ato("atol", "  -7")) == -7);
    CHECK(ato("atoll", "18446744073709551615") == ~u64{0});
    // The x forms: *perr and errno both.
    put(l.b, kText, "99999999999");
    ps_write32(l.b, kOut, 7);
    l.clear_errno();
    CHECK(i32(l.t.call("_Stolx", kText, 0, 10, kOut)) == 0x7FFFFFFF);
    CHECK(ps_read32(l.b, kOut) == 1);
    CHECK(l.errno_word() == kERANGE);
    wput(l.b, kText, U"  -0x1fz");
    CHECK(i32(l.t.call("wcstol", kText, kEnd, 0)) == -31);
    CHECK(ps_read32(l.b, kEnd) == kText + 4 * 7);
}

TEST_CASE("sys_libc: sscanf is the SDK's scan engine", "[hle][libc]")
{
    Libc l;
    const auto scan = [&](std::string_view fmt, std::string_view in) {
        put(l.b, kFmt, fmt);
        put(l.b, kText, in);
        std::memset(l.b + kOut, 0xA5, 256);
        l.clear_errno();
        return i32(l.t.call("sscanf", kText, kFmt, kOut, kOut + 64, kOut + 128, kOut + 192));
    };
    const auto word = [&](u32 slot) { return ps_read32(l.b, kOut + 64 * slot); };
    // "0x" with no hex digit: EOF when it ends the input, a matching failure before anything else.
    CHECK(scan("%x", "0x") == -1);
    CHECK(scan("%x", "0xg") == 0);
    CHECK(scan("%x%n", " 0x1fz") == 1);
    CHECK(word(0) == 0x1F);
    CHECK(word(1) == 5);
    // "1e" with no exponent digits converts as 1.0, both characters taken.
    CHECK(scan("%lf%n", "1ex") == 1);
    CHECK(ps_read64(l.b, kOut) == bits(1.0));
    CHECK(word(1) == 2);
    // %c with fewer characters than its width stores them and succeeds; %C is no conversion.
    CHECK(scan("%3c", "ab") == 1);
    CHECK(std::memcmp(l.b + kOut, "ab\xA5", 3) == 0);
    CHECK(scan("%C", "x") == 0);
    // Integers go through _Stoll/_Stoull and store their low bits: L stores 32, ll 64, hh a byte.
    CHECK(scan("%Ld", "4294967297") == 1);
    CHECK(word(0) == 1);
    CHECK(word(0 + 1) == 0xA5A5A5A5u);
    CHECK(scan("%lld", "4294967297") == 1);
    CHECK(ps_read64(l.b, kOut) == 4294967297ull);
    CHECK(scan("%hhd", "300") == 1);
    CHECK(l.b[kOut] == 44);
    CHECK(l.b[kOut + 1] == 0xA5);
    CHECK(scan("%d", "99999999999999999999") == 1);
    CHECK(word(0) == 0xFFFFFFFFu); // _Stoll's LLONG_MAX, its low word
    CHECK(l.errno_word() == kERANGE);
    CHECK(scan("%hhn%d", "12") == 1); // %n assigns but is not counted
    CHECK(l.b[kOut] == 0);
    CHECK(word(1) == 12);
    // Scansets: a '-' that is neither first nor last makes a range; a reversed range matches nothing.
    CHECK(scan("%[a-]", "-a-b") == 1);
    CHECK(std::string(reinterpret_cast<const char*>(l.b + kOut)) == "-a-");
    CHECK(scan("%[z-a]", "zza") == 0);
    CHECK(scan("%[^,],%s", "one,two") == 2);
    CHECK(std::string(reinterpret_cast<const char*>(l.b + kOut + 64)) == "two");
    // NaN with an n-char sequence that does not close, an "infinity" that fails half way: matching failures.
    CHECK(scan("%lf", "nan(abc") == 0);
    CHECK(scan("%lf%n", "nan(abc)x") == 1);
    CHECK(std::isnan(std::bit_cast<double>(ps_read64(l.b, kOut))));
    CHECK(word(1) == 8);
    CHECK(scan("%lf", "infinx") == 0);
    // A width past INT_MAX wraps negative: no width.
    CHECK(scan("%2147483648d", "12") == 1);
    CHECK(word(0) == 12);
    // 36 significant digits kept; a float target gets the double rounded.
    CHECK(scan("%lf", "1234567890123456789012345678901234567890") == 1);
    CHECK(ps_read64(l.b, kOut) == bits(l.fcall("strtod", "1234567890123456789012345678901234567890")));
    CHECK(scan("%f", "0.3") == 1);
    CHECK(std::bit_cast<float>(word(0)) == static_cast<float>(l.fcall("strtod", "0.3")));
    // The end of input: EOF before the first conversion, but a literal that meets it gives the count (0).
    CHECK(scan("%d", "") == -1);
    CHECK(scan(" %d", "  ") == -1);
    CHECK(scan("x%d", "x") == -1);
    CHECK(scan("x%d", "") == 0);
    CHECK(scan("%%", "") == -1);
    CHECK(scan("%d %d", "5") == 1);
    CHECK(scan("%", "5") == 0);
    CHECK(scan("\x80", "\x80") == 0); // a format byte past 127 never matches
    // %ls stores 32-bit characters (sys_libc's wchar_t).
    CHECK(scan("%ls", "ab") == 1);
    CHECK(ps_read32(l.b, kOut) == 'a');
    CHECK(ps_read32(l.b, kOut + 4) == 'b');
    CHECK(ps_read32(l.b, kOut + 8) == 0);
}

TEST_CASE("sys_libc: swscanf and the wide engine", "[hle][libc]")
{
    Libc l;
    const auto wscan = [&](std::u32string_view fmt, std::u32string_view in) {
        wput(l.b, kFmt, fmt);
        wput(l.b, kText, in);
        std::memset(l.b + kOut, 0xA5, 256);
        return i32(l.t.call("swscanf", kText, kFmt, kOut, kOut + 64, kOut + 128));
    };
    CHECK(wscan(U"%d %ls", U"12 abc") == 2);
    CHECK(ps_read32(l.b, kOut) == 12);
    CHECK(ps_read32(l.b, kOut + 64) == 'a');
    CHECK(ps_read32(l.b, kOut + 72) == 'c');
    CHECK(ps_read32(l.b, kOut + 76) == 0);
    CHECK(wscan(U"%d %s", U"12 abc") == 2);
    CHECK(std::string(reinterpret_cast<const char*>(l.b + kOut + 64)) == "abc");
    CHECK(wscan(U"%s", U"aĀb") == 0); // a character past 255 has no byte: a matching failure
    CHECK(wscan(U"%l[a-c]%n", U"abcd") == 1);
    CHECK(ps_read32(l.b, kOut + 12) == 0);
    CHECK(ps_read32(l.b, kOut + 64) == 3);
    CHECK(wscan(U"%x", U"0x") == -1);
    CHECK(wscan(U"%lf", U"0x1.8p1") == 1);
    CHECK(ps_read64(l.b, kOut) == bits(3.0));
    CHECK(wscan(U"ā%d", U"ā5") == 1); // wide literals compare as characters
    CHECK(wscan(U"%d", U"١") == 0);        // no digit past ASCII
}

TEST_CASE("sys_libc: _Getfld's family on the SDK's _Sft, and _Scanf through the caller's function", "[hle][libc]")
{
    Libc l;
    const u32 getc = guest_getc(l.b, kCode, false), wgetc = guest_getc(l.b, kCode + 0x100, true);
    // _Sft: pfn, arg, ap, s, nchar, nget, width, noconv, qual, stored.
    const auto sft = [&](u32 pfn, u32 fmt_at, s32 nchar, s32 width, u8 noconv, u8 qual) {
        ps_write32(l.b, kSft, pfn);
        ps_write32(l.b, kSft + 4, kCursor);
        ps_write32(l.b, kSft + 8, kVa);
        ps_write32(l.b, kSft + 12, fmt_at);
        ps_write32(l.b, kSft + 16, static_cast<u32>(nchar));
        ps_write32(l.b, kSft + 20, 0x12345678);
        ps_write32(l.b, kSft + 24, static_cast<u32>(width));
        l.b[kSft + 28] = noconv;
        l.b[kSft + 29] = qual;
        l.b[kSft + 30] = 7;
        ps_write64(l.b, kVa, kOut);
        ps_write64(l.b, kVa + 8, kOut + 64);
        std::memset(l.b + kOut, 0xA5, 128);
    };
    put(l.b, kText, "  -0x1fz");
    ps_write32(l.b, kCursor, kText + 2); // _Scanf skips the white space before calling _Getfld
    put(l.b, kFmt, "i");
    sft(getc, kFmt, 2, 0, 0, 0);
    CHECK(i32(l.t.call("_Getint", kSft)) == 1);
    CHECK(i32(ps_read32(l.b, kOut)) == -31);
    CHECK(ps_read32(l.b, kCursor) == kText + 7); // 'z' pushed back
    CHECK(ps_read32(l.b, kSft + 8) == kVa + 8);   // ap past one argument
    CHECK(ps_read32(l.b, kSft + 16) == 7);        // nchar
    CHECK(i32(ps_read32(l.b, kSft + 20)) == 0x7FFFFFFF - 6); // nget after six reads with no width
    CHECK(l.b[kSft + 30] == 1);                   // stored
    // A width counts down; the pretended EOF at its end.
    put(l.b, kText, "12345");
    ps_write32(l.b, kCursor, kText);
    put(l.b, kFmt, "d");
    sft(getc, kFmt, 0, 3, 0, 'h');
    CHECK(i32(l.t.call("_Getfld", kSft)) == 1);
    CHECK(ps_read16(l.b, kOut) == 123);
    CHECK(ps_read32(l.b, kCursor) == kText + 3);
    CHECK(ps_read32(l.b, kSft + 16) == 3);
    // _Getstr with %[: px->s left at the closing ']'; suppressed: nothing stored, stored left alone (_Getfld clears it).
    put(l.b, kText, "abc,d");
    ps_write32(l.b, kCursor, kText);
    put(l.b, kFmt, "[^,]");
    sft(getc, kFmt, 0, 0, '*', 0);
    CHECK(i32(l.t.call("_Getstr", kSft, static_cast<u64>(-1))) == 1);
    CHECK(ps_read32(l.b, kSft + 12) == kFmt + 3);
    CHECK(l.b[kSft + 30] == 7);
    CHECK(ps_read32(l.b, kSft + 8) == kVa);
    CHECK(l.b[kOut] == 0xA5);
    CHECK(ps_read32(l.b, kCursor) == kText + 3);
    // _Getfloat: a float target gets the double rounded.
    put(l.b, kText, "2.5e1x");
    ps_write32(l.b, kCursor, kText);
    put(l.b, kFmt, "f");
    sft(getc, kFmt, 0, 0, 0, 0);
    CHECK(i32(l.t.call("_Getfloat", kSft)) == 1);
    CHECK(std::bit_cast<float>(ps_read32(l.b, kOut)) == 25.0f);
    // The wide forms on a _WSft (a wide format and wide input).
    wput(l.b, kText, U"77 xy");
    ps_write32(l.b, kCursor, kText);
    wput(l.b, kFmt, U"d");
    sft(wgetc, kFmt, 0, 0, 0, 0);
    CHECK(i32(l.t.call("_WGetfld", kSft)) == 1);
    CHECK(ps_read32(l.b, kOut) == 77);
    CHECK(ps_read32(l.b, kCursor) == kText + 8);
    wput(l.b, kFmt, U"ls");
    ps_write32(l.b, kCursor, kText + 12);
    sft(wgetc, kFmt, 0, 0, 0, 'l');
    CHECK(i32(l.t.call("_WGetstr", kSft, 1)) == 1);
    CHECK(ps_read32(l.b, kOut) == 'x');
    CHECK(ps_read32(l.b, kOut + 8) == 0);
    // _Scanf and _WScanf through the caller's function.
    put(l.b, kText, "  123 word!");
    ps_write32(l.b, kCursor, kText);
    put(l.b, kFmt, "%d %[a-z]");
    ps_write64(l.b, kVa, kOut);
    ps_write64(l.b, kVa + 8, kOut + 64);
    CHECK(i32(l.t.call("_Scanf", getc, kCursor, kFmt, kVa)) == 2);
    CHECK(ps_read32(l.b, kOut) == 123);
    CHECK(std::string(reinterpret_cast<const char*>(l.b + kOut + 64)) == "word");
    CHECK(ps_read32(l.b, kCursor) == kText + 10);
    wput(l.b, kText, U"5,6");
    ps_write32(l.b, kCursor, kText);
    wput(l.b, kFmt, U"%d,%d");
    CHECK(i32(l.t.call("_WScanf", wgetc, kCursor, kFmt, kVa)) == 2);
    CHECK(ps_read32(l.b, kOut + 64) == 6);
}
