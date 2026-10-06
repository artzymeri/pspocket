// cellRtc as RPCS3, every function it implements: ticks and dates both ways (expected ticks and dates computed with
// Python's datetime), the current tick and local clock, the secure clock, Win32 file times, time_t, system times and
// MS-DOS times both ways, RFC 2822 and RFC 3339 formatting, the date parsers (and their quirk for a year of 0), tick
// arithmetic with libcellRtc's clamping of months and years, the calendar helpers, the checks and error codes of each,
// and the setters of the host's clock and time zone refused as lv2 refuses them.

#include "hle_test_util.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "pspocket/lv2/syscalls.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kTick = kData + 0x100, kTime = kData + 0x200, kStr = kData + 0x300, kOut = kData + 0x400;
constexpr u64 kInvalidPointer = 0xFFFFFFFF80010602ull, kBadParse = 0xFFFFFFFF80010607ull;
constexpr u64 kInvalidValue = 0xFFFFFFFF80010603ull, kInvalidArg = 0xFFFFFFFF80010604ull;
constexpr u32 kUnmapped = 0x0FFF0000; // never committed by these tests

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

TEST_CASE("cellRtc: every function RPCS3 implements, under its NID", "[hle][rtc]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"cellRtcGetCurrentTick", 0x9DAFC0D9}, {"cellRtcGetCurrentClock", 0x32C941CF},
        {"cellRtcGetCurrentClockLocalTime", 0x2CCE9CF5}, {"cellRtcFormatRfc2822", 0x5491B9D5},
        {"cellRtcFormatRfc2822LocalTime", 0xA07C3D2F}, {"cellRtcFormatRfc3339", 0xD9C0B463},
        {"cellRtcFormatRfc3339LocalTime", 0x1324948A}, {"cellRtcParseDateTime", 0xC5BC0FAC},
        {"cellRtcParseRfc3339", 0xCF11C3D6}, {"cellRtcGetTick", 0xC7BDB7EB}, {"cellRtcSetTick", 0x99B13034},
        {"cellRtcTickAddTicks", 0x269A1882}, {"cellRtcTickAddMicroseconds", 0xF8509925},
        {"cellRtcTickAddSeconds", 0xCCCE71BD}, {"cellRtcTickAddMinutes", 0x2F010BFA},
        {"cellRtcTickAddHours", 0xD41D3BD2}, {"cellRtcTickAddDays", 0x75744E2A}, {"cellRtcTickAddWeeks", 0x64C63FD5},
        {"cellRtcTickAddMonths", 0xE0ECBB45}, {"cellRtcTickAddYears", 0x332A74DD},
        {"cellRtcConvertUtcToLocalTime", 0xC48D5002}, {"cellRtcConvertLocalTimeToUtc", 0x46CA7FE0},
        {"cellRtcGetCurrentSecureTick", 0xC4808FD7}, {"cellRtcGetDosTime", 0xDFFF32CF},
        {"cellRtcGetTickResolution", 0x748BEDA3}, {"cellRtcGetSystemTime", 0xF582308D},
        {"cellRtcGetTime_t", 0xCB90C761}, {"cellRtcGetWin32FileTime", 0xE7086F05}, {"cellRtcSetConf", 0x5E78D18E},
        {"cellRtcSetCurrentSecureTick", 0x3DC09A3E}, {"cellRtcSetCurrentTick", 0xEB22BB86},
        {"cellRtcSetDosTime", 0x9598D4B3}, {"cellRtcSetTime_t", 0xBB543189}, {"cellRtcSetSystemTime", 0xE22D74A4},
        {"cellRtcSetWin32FileTime", 0x5F68C268}, {"cellRtcIsLeapYear", 0x5316B4A8},
        {"cellRtcGetDaysInMonth", 0x5B6A0A1D}, {"cellRtcGetDayOfWeek", 0xC2D8CF95}, {"cellRtcCheckValid", 0x7F1086E6},
        {"cellRtcCompareTick", 0xFB51FC61},
    };
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellRtc", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

TEST_CASE("cellRtcCheckValid and cellRtcCompareTick: RPCS3's order of errors; -1, 0 or 1", "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    struct Case {
        u16 y, mo, d, h, mi, s;
        u32 us;
        u32 error;
    };
    for (const Case& c : {Case{2024, 2, 29, 23, 59, 59, 999999, 0}, Case{0, 13, 0, 24, 60, 60, 1000000, 0x80010621},
             Case{10000, 1, 1, 0, 0, 0, 0, 0x80010621}, Case{2024, 0, 1, 0, 0, 0, 0, 0x80010622},
             Case{2024, 13, 1, 0, 0, 0, 0, 0x80010622}, Case{2023, 2, 29, 0, 0, 0, 0, 0x80010623},
             Case{2026, 4, 31, 0, 0, 0, 0, 0x80010623}, Case{2026, 4, 0, 0, 0, 0, 0, 0x80010623},
             Case{2026, 4, 30, 24, 60, 0, 0, 0x80010624}, Case{2026, 4, 30, 23, 60, 60, 0, 0x80010625},
             Case{2026, 4, 30, 23, 59, 60, 1000000, 0x80010626}, Case{2026, 4, 30, 23, 59, 59, 1000000, 0x80010627}}) {
        INFO(c.y << "-" << c.mo << "-" << c.d << " " << c.h << ":" << c.mi << ":" << c.s << "." << c.us);
        put_date(b, c.y, c.mo, c.d, c.h, c.mi, c.s, c.us);
        CHECK(a.call("cellRtcCheckValid", kTime) == (c.error ? r3_of_code(c.error) : 0));
    }
    CHECK(a.call("cellRtcCheckValid", 0) == kInvalidPointer); // RPCS3 ensure()s it; refused here (D-042)

    ps_write64(b, kTick, 1000);
    ps_write64(b, kTick + 8, 2000);
    CHECK(a.call("cellRtcCompareTick", kTick, kTick + 8) == r3_of_code(0xFFFFFFFF)); // -1
    CHECK(a.call("cellRtcCompareTick", kTick + 8, kTick) == 1);
    CHECK(a.call("cellRtcCompareTick", kTick, kTick) == 0);
    CHECK(a.call("cellRtcCompareTick", 0, kTick) == kInvalidPointer);
    CHECK(a.call("cellRtcCompareTick", kTick, kUnmapped) == kInvalidPointer);
}

TEST_CASE("cellRtc: the calendar helpers, the tick resolution", "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    for (const auto& [y, mo, d, dow] : {std::array<s32, 4>{1994, 11, 6, 0}, {2000, 1, 1, 6}, {2026, 10, 6, 2},
             {2024, 2, 29, 4}, {1970, 1, 1, 4}, {1, 1, 1, 1}}) {
        INFO(y << "-" << mo << "-" << d);
        CHECK(a.call("cellRtcGetDayOfWeek", y, mo, d) == static_cast<u64>(dow));
    }
    for (const auto& [y, mo, days] : {std::array<s32, 3>{2024, 2, 29}, {2023, 2, 28}, {1900, 2, 28}, {2000, 2, 29},
             {2026, 4, 30}, {2026, 12, 31}}) {
        INFO(y << "-" << mo);
        CHECK(a.call("cellRtcGetDaysInMonth", y, mo) == static_cast<u64>(days));
    }
    for (const auto& [y, mo] : {std::pair<s32, s32>{0, 1}, {-5, 3}, {2026, 0}, {2026, 13}, {2026, -1}}) {
        CHECK(a.call("cellRtcGetDaysInMonth", y, mo) == kInvalidArg);
    }
    CHECK(a.call("cellRtcIsLeapYear", 2000) == 1);
    CHECK(a.call("cellRtcIsLeapYear", 1900) == 0);
    CHECK(a.call("cellRtcIsLeapYear", 2024) == 1);
    CHECK(a.call("cellRtcIsLeapYear", 2023) == 0);
    CHECK(a.call("cellRtcIsLeapYear", 1) == 0);
    CHECK(a.call("cellRtcIsLeapYear", 0) == kInvalidArg);
    CHECK(a.call("cellRtcIsLeapYear", -4) == kInvalidArg);
    CHECK(a.call("cellRtcGetTickResolution") == 1000000);
}

TEST_CASE("cellRtc: time_t, system times, Win32 file times and MS-DOS times, both ways", "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    for (const char* fn : {"cellRtcGetTime_t", "cellRtcGetSystemTime", "cellRtcGetWin32FileTime", "cellRtcGetDosTime"}) {
        INFO(fn);
        CHECK(a.call(fn, 0, kTick) == kInvalidPointer);
        CHECK(a.call(fn, kTime, 0) == kInvalidPointer);
    }
    for (const char* fn : {"cellRtcSetTime_t", "cellRtcSetSystemTime", "cellRtcSetDosTime"}) {
        INFO(fn);
        CHECK(a.call(fn, 0, 0) == kInvalidPointer);
    }

    // time_t: seconds since 1970.
    put_date(b, 1994, 11, 6, 8, 49, 37, 999999);
    REQUIRE(a.call("cellRtcGetTime_t", kTime, kTick) == 0);
    CHECK(ps_read64(b, kTick) == 784111777);
    put_date(b, 1969, 12, 31, 23, 59, 59, 0);
    CHECK(a.call("cellRtcGetTime_t", kTime, kTick) == kInvalidValue);
    CHECK(ps_read64(b, kTick) == 0);
    REQUIRE(a.call("cellRtcSetTime_t", kTime, 784111777) == 0);
    CHECK(date(b) == "1994-11-6 8:49:37.0");
    REQUIRE(a.call("cellRtcSetTime_t", kTime, ~0ull) == 0); // -1
    CHECK(date(b) == "1969-12-31 23:59:59.0");

    // System time: seconds since 2000, up to the end of 2099.
    put_date(b, 2026, 10, 6, 15, 30, 42, 500000);
    REQUIRE(a.call("cellRtcGetSystemTime", kTime, kTick) == 0);
    CHECK(ps_read64(b, kTick) == 844615842);
    REQUIRE(a.call("cellRtcSetSystemTime", kTime, 844615842) == 0);
    CHECK(date(b) == "2026-10-6 15:30:42.0");
    put_date(b, 2099, 12, 31, 23, 59, 59, 999999);
    REQUIRE(a.call("cellRtcGetSystemTime", kTime, kTick) == 0);
    CHECK(ps_read64(b, kTick) == 3155759999);
    put_date(b, 2100, 1, 1, 0, 0, 0, 999998); // RPCS3's bound is the maximum plus one second: still in
    REQUIRE(a.call("cellRtcGetSystemTime", kTime, kTick) == 0);
    CHECK(ps_read64(b, kTick) == 3155760000);
    put_date(b, 2100, 1, 1, 0, 0, 1, 0);
    CHECK(a.call("cellRtcGetSystemTime", kTime, kTick) == kInvalidValue);
    CHECK(ps_read64(b, kTick) == 3155759999);
    put_date(b, 1999, 12, 31, 23, 59, 59, 0);
    CHECK(a.call("cellRtcGetSystemTime", kTime, kTick) == kInvalidValue);
    CHECK(ps_read64(b, kTick) == 0);
    put_date(b, 0, 1, 1, 0, 0, 0, 0); // cellRtcGetTick refuses the year: tick 0
    ps_write64(b, kTick, 5);
    CHECK(a.call("cellRtcGetSystemTime", kTime, kTick) == kInvalidValue);
    CHECK(ps_read64(b, kTick) == 0);

    // Win32 file time: 100 ns since 1601.
    put_date(b, 2026, 10, 6, 15, 30, 42, 123456);
    REQUIRE(a.call("cellRtcGetWin32FileTime", kTime, kTick) == 0);
    CHECK(ps_read64(b, kTick) == 134357742421234560ull);
    put_date(b, 1601, 1, 1, 0, 0, 0, 0);
    REQUIRE(a.call("cellRtcGetWin32FileTime", kTime, kTick) == 0);
    CHECK(ps_read64(b, kTick) == 0);
    put_date(b, 1600, 12, 31, 0, 0, 0, 0);
    ps_write64(b, kTick, 5);
    CHECK(a.call("cellRtcGetWin32FileTime", kTime, kTick) == kInvalidValue);
    CHECK(ps_read64(b, kTick) == 0);

    // MS-DOS time: years from 1980, two-second steps. Out of its range: the low or high value, and -1.
    put_date(b, 2026, 10, 6, 15, 30, 42, 0);
    REQUIRE(a.call("cellRtcGetDosTime", kTime, kTick) == 0);
    CHECK(ps_read32(b, kTick) == 0x5D467BD5);
    std::memset(b + kTime, 0xEE, 16);
    REQUIRE(a.call("cellRtcSetDosTime", kTime, 0x5D467BD5) == 0);
    CHECK(date(b) == "2026-10-6 15:30:42.0");
    REQUIRE(a.call("cellRtcSetDosTime", kTime, 0xFFFFFFFF) == 0); // no checks: month 15, hour 31, second 62
    CHECK(date(b) == "2107-15-31 31:63:62.0");
    put_date(b, 1979, 12, 31, 0, 0, 0, 0);
    CHECK(a.call("cellRtcGetDosTime", kTime, kTick) == ~0ull);
    CHECK(ps_read32(b, kTick) == 0);
    put_date(b, 2108, 1, 1, 0, 0, 0, 0);
    CHECK(a.call("cellRtcGetDosTime", kTime, kTick) == ~0ull);
    CHECK(ps_read32(b, kTick) == 0xFF9FBF7D);
}

TEST_CASE("cellRtcTickAdd*: units of every size; months and years clamp the day, a year of 0 writes nothing",
    "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u64 T = 63926724642000000ull; // 2026-10-06 15:30:42
    const std::pair<const char*, u64> adds[] = {
        {"cellRtcTickAddTicks", T + 5},
        {"cellRtcTickAddMicroseconds", T + 5},
        {"cellRtcTickAddSeconds", T + 5'000'000},
        {"cellRtcTickAddMinutes", T + 300'000'000},
        {"cellRtcTickAddHours", T + 5 * 3'600'000'000ull},
        {"cellRtcTickAddDays", T + 5 * 86'400'000'000ull},
        {"cellRtcTickAddWeeks", T + 35 * 86'400'000'000ull},
    };
    for (const auto& [fn, result] : adds) {
        INFO(fn);
        CHECK(a.call(fn, 0, kTick, 5) == kInvalidPointer);
        CHECK(a.call(fn, kTick + 8, kUnmapped, 5) == kInvalidPointer);
        ps_write64(b, kTick, T);
        REQUIRE(a.call(fn, kTick + 8, kTick, 5) == 0);
        CHECK(ps_read64(b, kTick + 8) == result);
        REQUIRE(a.call(fn, kTick, kTick + 8, -5) == 0); // and back
        CHECK(ps_read64(b, kTick) == T);
    }
    // The s32 ones take the low word: -1 hour, in place.
    ps_write64(b, kTick, T);
    REQUIRE(a.call("cellRtcTickAddHours", kTick, kTick, 0xFFFFFFFFull) == 0);
    CHECK(ps_read64(b, kTick) == T - 3'600'000'000ull);

    struct Case {
        const char* fn;
        u64 from;
        s32 add;
        u64 to;
    };
    for (const Case& c : {
             Case{"cellRtcTickAddMonths", 63842299200000000ull, 1, 63844804800000000ull},  // 2024-01-31 12:00 -> 02-29
             Case{"cellRtcTickAddMonths", 63842299200000000ull, 13, 63876340800000000ull}, // -> 2025-02-28
             Case{"cellRtcTickAddMonths", 63842299200000000ull, -1, 63839620800000000ull}, // -> 2023-12-31
             Case{"cellRtcTickAddYears", 63844804800000000ull, 1, 63876340800000000ull},   // 2024-02-29 -> 2025-02-28
             Case{"cellRtcTickAddYears", 63844804800000000ull, 4, 63971035200000000ull},   // -> 2028-02-29
         }) {
        INFO(c.fn << " " << c.add);
        ps_write64(b, kTick, c.from);
        REQUIRE(a.call(c.fn, kTick + 8, kTick, c.add) == 0);
        CHECK(ps_read64(b, kTick + 8) == c.to);
    }
    // Into year 0: cellRtcGetTick refuses it, the result is left as it was, and the call still succeeds.
    ps_write64(b, kTick, 1209600000000ull); // 0001-01-15
    ps_write64(b, kTick + 8, 77);
    REQUIRE(a.call("cellRtcTickAddMonths", kTick + 8, kTick, -1) == 0);
    CHECK(ps_read64(b, kTick + 8) == 77);
    ps_write64(b, kTick, 63844804800000000ull); // 2024-02-29
    REQUIRE(a.call("cellRtcTickAddYears", kTick + 8, kTick, -2024) == 0);
    CHECK(ps_read64(b, kTick + 8) == 77);
    for (const char* fn : {"cellRtcTickAddMonths", "cellRtcTickAddYears"}) {
        CHECK(a.call(fn, 0, kTick, 1) == kInvalidPointer);
        CHECK(a.call(fn, kTick, 0, 1) == kInvalidPointer);
    }
}

TEST_CASE("cellRtc: UTC and local time; the secure clock; the setters change nothing", "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    s32 zone = 0, dst = 0;
    lv2::host_time_zone(zone, dst);
    const s64 local_offset = static_cast<s64>(zone + dst) * 60'000'000;
    constexpr u64 T = 63926724642000000ull;
    ps_write64(b, kTick, T);
    REQUIRE(a.call("cellRtcConvertUtcToLocalTime", kTick, kTick + 8) == 0);
    CHECK(ps_read64(b, kTick + 8) == T + static_cast<u64>(local_offset));
    REQUIRE(a.call("cellRtcConvertLocalTimeToUtc", kTick + 8, kTick + 16) == 0);
    CHECK(ps_read64(b, kTick + 16) == T);
    CHECK(a.call("cellRtcConvertUtcToLocalTime", 0, kTick) == kInvalidPointer);
    CHECK(a.call("cellRtcConvertLocalTimeToUtc", kTick, 0) == kInvalidPointer);

    // The secure clock: whole seconds of the host's UTC time.
    REQUIRE(a.call("cellRtcGetCurrentSecureTick", kTick) == 0);
    REQUIRE(a.call("cellRtcGetCurrentTick", kTick + 8) == 0);
    const u64 secure = ps_read64(b, kTick), now = ps_read64(b, kTick + 8);
    CHECK(secure % 1'000'000 == 0);
    CHECK(secure <= now);
    CHECK(secure + 3'000'000 > now);
    CHECK(a.call("cellRtcGetCurrentSecureTick", 0) == kInvalidPointer);

    // The setters: NULL and range checks, then lv2 refuses a process without root (CELL_ENOSYS) and changes nothing.
    constexpr u64 kEnosys = 0xFFFFFFFF80010003ull;
    CHECK(a.call("cellRtcSetCurrentTick", 0) == kInvalidPointer);
    CHECK(a.call("cellRtcSetCurrentTick", kUnmapped) == kInvalidPointer);
    ps_write64(b, kTick, 62135596799999999ull); // just before 1970
    CHECK(a.call("cellRtcSetCurrentTick", kTick) == kInvalidValue);
    ps_write64(b, kTick, 63082281600000000ull); // 2000
    CHECK(a.call("cellRtcSetCurrentTick", kTick) == kEnosys);
    CHECK(a.call("cellRtcSetConf", 1, 2, 60, 0) == kEnosys);
    CHECK(a.call("cellRtcSetCurrentSecureTick", 0) == kInvalidPointer);
    CHECK(a.call("cellRtcSetCurrentSecureTick", kUnmapped) == kInvalidPointer);
    ps_write64(b, kTick, 66238041600000000ull); // 2100
    CHECK(a.call("cellRtcSetCurrentSecureTick", kTick) == kInvalidValue);
    ps_write64(b, kTick, 63082281600000000ull);
    CHECK(a.call("cellRtcSetCurrentSecureTick", kTick) == 0); // accepted, not kept
    REQUIRE(a.call("cellRtcGetCurrentTick", kTick + 8) == 0);
    CHECK(ps_read64(b, kTick + 8) >= now); // the clock didn't move back to 2000
    REQUIRE(a.call("cellRtcGetCurrentSecureTick", kTick + 8) == 0);
    CHECK(ps_read64(b, kTick + 8) >= secure);
    s32 zone_after = 0, dst_after = 0;
    lv2::host_time_zone(zone_after, dst_after);
    CHECK(zone_after == zone);
}

TEST_CASE("cellRtcSetCurrentTick and cellRtcSetConf: a root process gets lv2's CELL_OK, and nothing changes",
    "[hle][rtc]")
{
    lv2::Config config;
    config.root = true;
    Fixture f({}, config);
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    ps_write64(b, kTick, 63082281600000000ull);
    CHECK(a.call("cellRtcSetCurrentTick", kTick) == 0);
    CHECK(a.call("cellRtcSetConf", 0, 0, 60, 60) == 0);
    REQUIRE(a.call("cellRtcGetCurrentTick", kTick) == 0);
    CHECK(ps_read64(b, kTick) > 63926000000000000ull); // still now (2026), not 2000
}

TEST_CASE("cellRtc: RFC 2822 and RFC 3339 for a time zone given; cellRtcParseRfc3339 on its own; a year of 0 parsed",
    "[hle][rtc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    const auto text = [&] { return std::string(reinterpret_cast<const char*>(b + kOut)); };
    ps_write64(b, kTick, 62919708577250000ull); // 1994-11-06 08:49:37.25 UTC
    REQUIRE(a.call("cellRtcFormatRfc2822", kOut, kTick, 90) == 0);
    CHECK(text() == "Sun, 06 Nov 1994 10:19:37 +0130");
    REQUIRE(a.call("cellRtcFormatRfc2822", kOut, kTick, -300) == 0);
    CHECK(text() == "Sun, 06 Nov 1994 03:49:37 -0500");
    REQUIRE(a.call("cellRtcFormatRfc3339", kOut, kTick, 0) == 0);
    CHECK(text() == "1994-11-06T08:49:37.25Z");
    REQUIRE(a.call("cellRtcFormatRfc3339", kOut, kTick, 90) == 0);
    CHECK(text() == "1994-11-06T10:19:37.25+01:30");
    REQUIRE(a.call("cellRtcFormatRfc3339", kOut, kTick, -300) == 0);
    CHECK(text() == "1994-11-06T03:49:37.25-05:00");
    for (const char* fn : {"cellRtcFormatRfc2822", "cellRtcFormatRfc3339"}) {
        INFO(fn);
        CHECK(a.call(fn, 0, kTick, 0) == kInvalidPointer);
        CHECK(a.call(fn, kOut, 0, 0) == kInvalidPointer);
    }
    CHECK(a.call("cellRtcFormatRfc3339LocalTime", 0, kTick) == kInvalidPointer);
    // A date that is not one (year 10000): cellRtcCheckValid's error.
    ps_write64(b, kTick + 8, 315537897600000000ull);
    CHECK(a.call("cellRtcFormatRfc3339", kOut, kTick + 8, 0) == r3_of_code(0x80010621));
    CHECK(a.call("cellRtcFormatRfc2822", kOut, kTick + 8, 0) == r3_of_code(0x80010621));

    // The local time zone: as the explicit one with the host's offset.
    s32 zone = 0, dst = 0;
    lv2::host_time_zone(zone, dst);
    REQUIRE(a.call("cellRtcFormatRfc3339", kOut + 0x40, kTick, zone + dst) == 0);
    REQUIRE(a.call("cellRtcFormatRfc3339LocalTime", kOut, kTick) == 0);
    CHECK(text() == std::string(reinterpret_cast<const char*>(b + kOut + 0x40)));

    // cellRtcParseRfc3339 by itself; each missing separator has its own error.
    const auto parse = [&](const char* s) {
        std::strcpy(reinterpret_cast<char*>(b + kStr), s);
        return a.call("cellRtcParseRfc3339", kTick, kStr);
    };
    REQUIRE(parse("1994-11-06T10:49:37.25+02:00") == 0);
    CHECK(ps_read64(b, kTick) == 62919708577250000ull);
    CHECK(parse("1994/11-06T08:49:37Z") == r3_of_code(0x80010621));
    CHECK(parse("1994-11/06T08:49:37Z") == r3_of_code(0x80010622));
    CHECK(parse("1994-11-06 08:49:37Z") == r3_of_code(0x80010623));
    CHECK(parse("1994-11-06T08-49:37Z") == r3_of_code(0x80010624));
    CHECK(parse("1994-11-06T08:49-37Z") == r3_of_code(0x80010625));
    CHECK(parse("1994-11-06T08:49:37X") == kBadParse);
    CHECK(parse("1994-11-06T08:49:37+0200") == kBadParse);
    CHECK(a.call("cellRtcParseRfc3339", 0, kStr) == kInvalidPointer);
    CHECK(a.call("cellRtcParseRfc3339", kTick, 0) == kInvalidPointer);

    // A year cellRtcGetTick refuses (0, or not digits: 0xFFFF): no tick is written, and the offset is added to what the
    // tick held (RPCS3's cellRtcGetTick, then cellRtcTickAddMinutes), through cellRtcParseDateTime as well.
    ps_write64(b, kTick, 10'000'000'000ull);
    REQUIRE(parse("abcd-01-01T00:00:00Z") == 0);
    CHECK(ps_read64(b, kTick) == 10'000'000'000ull);
    REQUIRE(parse("0000-01-01T00:00:00+01:00") == 0);
    CHECK(ps_read64(b, kTick) == 10'000'000'000ull - 3'600'000'000ull);
    std::strcpy(reinterpret_cast<char*>(b + kStr), "0000-01-01T00:00:00-01:00");
    REQUIRE(a.call("cellRtcParseDateTime", kTick, kStr) == 0);
    CHECK(ps_read64(b, kTick) == 10'000'000'000ull);
    std::strcpy(reinterpret_cast<char*>(b + kStr), "Sun, 06 Nov 0000 08:49:37 +0300");
    REQUIRE(a.call("cellRtcParseDateTime", kTick, kStr) == 0);
    CHECK(ps_read64(b, kTick) == 10'000'000'000ull - 10'800'000'000ull);
}
