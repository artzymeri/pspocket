// cellRtc as RPCS3: ticks and dates both ways (expected ticks computed with Python's datetime), the current tick and
// local clock, Win32 file times, RFC 2822 formatting in the local time zone, and the date parser's three formats.

#include "hle_test_util.hpp"

#include <chrono>
#include <cstring>
#include <string>

#include "pspocket/lv2/syscalls.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kTick = kData + 0x100, kTime = kData + 0x200, kStr = kData + 0x300, kOut = kData + 0x400;
constexpr u64 kInvalidPointer = 0xFFFFFFFF80010602ull, kBadParse = 0xFFFFFFFF80010607ull;

void put_date(u8* b, u16 y, u16 mo, u16 d, u16 h, u16 mi, u16 s, u32 us)
{
    ps_write16(b, kTime, y);
    ps_write16(b, kTime + 2, mo);
    ps_write16(b, kTime + 4, d);
    ps_write16(b, kTime + 6, h);
    ps_write16(b, kTime + 8, mi);
    ps_write16(b, kTime + 10, s);
    ps_write32(b, kTime + 12, us);
}

std::string date(u8* b)
{
    return std::to_string(ps_read16(b, kTime)) + "-" + std::to_string(ps_read16(b, kTime + 2)) + "-" +
           std::to_string(ps_read16(b, kTime + 4)) + " " + std::to_string(ps_read16(b, kTime + 6)) + ":" +
           std::to_string(ps_read16(b, kTime + 8)) + ":" + std::to_string(ps_read16(b, kTime + 10)) + "." +
           std::to_string(ps_read32(b, kTime + 12));
}

} // namespace

TEST_CASE("cellRtc: ticks and dates, the current tick and clock, Win32 file times", "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();

    struct Case {
        u16 y, mo, d, h, mi, s;
        u32 us;
        u64 tick;
    };
    for (const Case& c : {Case{1970, 1, 1, 0, 0, 0, 0, 62135596800000000ull},
             Case{1994, 11, 6, 8, 49, 37, 0, 62919708577000000ull},
             Case{2000, 2, 29, 12, 34, 56, 789012, 63087424496789012ull},
             Case{2026, 10, 4, 15, 30, 0, 0, 63926724600000000ull},
             Case{9999, 12, 31, 23, 59, 59, 999999, 315537897599999999ull}}) {
        put_date(b, c.y, c.mo, c.d, c.h, c.mi, c.s, c.us);
        REQUIRE(a.call("cellRtcGetTick", kTime, kTick) == 0);
        CHECK(ps_read64(b, kTick) == c.tick);
        std::memset(b + kTime, 0, 16);
        REQUIRE(a.call("cellRtcSetTick", kTime, kTick) == 0);
        CHECK(ps_read16(b, kTime) == c.y);
        CHECK(ps_read16(b, kTime + 2) == c.mo);
        CHECK(ps_read16(b, kTime + 4) == c.d);
        CHECK(ps_read16(b, kTime + 6) == c.h);
        CHECK(ps_read16(b, kTime + 8) == c.mi);
        CHECK(ps_read16(b, kTime + 10) == c.s);
        CHECK(ps_read32(b, kTime + 12) == c.us);
    }
    put_date(b, 0, 1, 1, 0, 0, 0, 0);
    CHECK(a.call("cellRtcGetTick", kTime, kTick) == 0xFFFFFFFF80010603ull); // year 0: INVALID_VALUE
    CHECK(a.call("cellRtcGetTick", 0, kTick) == kInvalidPointer);
    CHECK(a.call("cellRtcSetTick", kTime, 0) == kInvalidPointer);

    // The current tick: the host's wall clock.
    REQUIRE(a.call("cellRtcGetCurrentTick", kTick) == 0);
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()) + 62135596800000000ull;
    CHECK(ps_read64(b, kTick) <= now);
    CHECK(ps_read64(b, kTick) + 5'000'000 > now);
    CHECK(a.call("cellRtcGetCurrentTick", 0) == kInvalidPointer);

    // The local clock: that tick moved by the host's time zone and daylight saving offset.
    s32 zone = 0, dst = 0;
    lv2::host_time_zone(zone, dst);
    REQUIRE(a.call("cellRtcGetCurrentClockLocalTime", kTime) == 0);
    REQUIRE(a.call("cellRtcGetTick", kTime, kTick) == 0);
    const s64 local_minus_utc = static_cast<s64>(ps_read64(b, kTick)) - static_cast<s64>(now);
    CHECK(local_minus_utc > (zone + dst) * 60'000'000ll - 5'000'000);
    CHECK(local_minus_utc < (zone + dst) * 60'000'000ll + 5'000'000);
    INFO(date(b));

    // Win32 file times: 100 ns since 1601.
    REQUIRE(a.call("cellRtcSetWin32FileTime", kTime, 116444736000000000ull) == 0);
    CHECK(date(b) == "1970-1-1 0:0:0.0");
    REQUIRE(a.call("cellRtcSetWin32FileTime", kTime, 0ull) == 0);
    CHECK(date(b) == "1601-1-1 0:0:0.0");
    CHECK(a.call("cellRtcSetWin32FileTime", 0, 0ull) == kInvalidPointer);
}

TEST_CASE("cellRtc: RFC 2822 in the local time zone; RFC 2822, RFC 3339 and asctime parsed to one tick",
    "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();

    // 1994-11-06 08:49:37 UTC, formatted in the host's zone.
    s32 zone = 0, dst = 0;
    lv2::host_time_zone(zone, dst);
    const s32 tz = zone + dst;
    ps_write64(b, kTick, 62919708577000000ull);
    REQUIRE(a.call("cellRtcFormatRfc2822LocalTime", kOut, kTick) == 0);
    const std::string s(reinterpret_cast<const char*>(b + kOut));
    REQUIRE(s.size() == 31);
    if (tz == 0) CHECK(s == "Sun, 06 Nov 1994 08:49:37 +0000");
    CHECK(s.substr(7, 9) == " Nov 1994");
    CHECK(s[26] == (tz < 0 ? '-' : '+'));
    const s32 az = tz < 0 ? -tz : tz;
    CHECK(s.substr(27) == std::to_string(az / 60 / 10) + std::to_string(az / 60 % 10) + std::to_string(az % 60 / 10) +
                              std::to_string(az % 10));
    // Parsing it back gives the same tick, with libcellRtc's quirk for "+hhmm" zones (as RPCS3): hh * 60 + hhmm % 60
    // minutes, so +0100 reads as 100 minutes and +0200 as 140; only whole multiples of 3 hours come out right.
    REQUIRE(a.call("cellRtcParseDateTime", kTick + 8, kOut) == 0);
    const s32 hhmm = std::stoi(s.substr(27, 4));
    const s32 parsed_zone = (tz < 0 ? -1 : 1) * (hhmm / 100 * 60 + hhmm % 60);
    CHECK(ps_read64(b, kTick + 8) == 62919708577000000ull + static_cast<u64>(static_cast<s64>(tz - parsed_zone) * 60'000'000));
    CHECK(a.call("cellRtcFormatRfc2822LocalTime", 0, kTick) == kInvalidPointer);

    for (const char* text : {"Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994",
             "  1994-11-06T08:49:37Z", "1994-11-06T10:49:37+02:00", "Sun, 06 Nov 1994 11:49:37 +0300", "sun, 6 nov 1994 08:49:37 z"}) {
        std::strcpy(reinterpret_cast<char*>(b + kStr), text);
        ps_write64(b, kTick, 0);
        INFO(text);
        REQUIRE(a.call("cellRtcParseDateTime", kTick, kStr) == 0);
        CHECK(ps_read64(b, kTick) == 62919708577000000ull);
    }
    std::strcpy(reinterpret_cast<char*>(b + kStr), "Sun, 06 Nov 1994 09:49:37 +0100"); // the quirk: 100 minutes
    REQUIRE(a.call("cellRtcParseDateTime", kTick, kStr) == 0);
    CHECK(ps_read64(b, kTick) == 62919708577000000ull - 2'400'000'000ull);
    std::strcpy(reinterpret_cast<char*>(b + kStr), "1994-11-06T08:49:37.250000Z");
    REQUIRE(a.call("cellRtcParseDateTime", kTick, kStr) == 0);
    CHECK(ps_read64(b, kTick) == 62919708577250000ull);
    for (const char* text : {"Someday, 06 Nov 1994", "Sun Nox 06", "Sun Nov 6 08-49-37 1994", "1994-11-06 08:49:37Z"}) {
        std::strcpy(reinterpret_cast<char*>(b + kStr), text);
        INFO(text);
        CHECK(a.call("cellRtcParseDateTime", kTick, kStr) != 0);
    }
    std::strcpy(reinterpret_cast<char*>(b + kStr), "Sun Nov 6 08:49:37 1994");
    CHECK(a.call("cellRtcParseDateTime", 0, kStr) == kInvalidPointer);
    std::strcpy(reinterpret_cast<char*>(b + kStr), "Someday");
    CHECK(a.call("cellRtcParseDateTime", kTick, kStr) == kBadParse);
}
