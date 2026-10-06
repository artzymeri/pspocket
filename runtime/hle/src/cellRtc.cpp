// cellRtc: the real-time clock library, every function RPCS3 implements, as its rpcs3/Emu/Cell/Modules/cellRtc.cpp/.h
// does them (that file follows libcellRtc closely, including its quirks; cited per function).
//
// A tick is microseconds since 0001-01-01 00:00:00 (RTC_MAGIC_OFFSET is 1970-01-01). The current time and time zone
// are what lv2's sys_time_get_current_time and sys_time_get_timezone report (lv2::host_current_time,
// host_time_zone). RPCS3 checks pointers with sys_memory_get_page_attribute (any mapped page): guest_readable here.
// Where RPCS3 checks a pointer only against NULL, or ensure()s it, and would fault on another bad one, the call is
// refused with INVALID_POINTER and the call site logged (D-042).
//
// Nothing here sets the host's clock. RPCS3's cellRtcSetCurrentTick and cellRtcSetConf go through lv2's
// sys_time_set_current_time and sys_time_set_timezone, which change nothing and refuse a process without root
// permissions (CELL_ENOSYS), as lv2 here does; cellRtcSetCurrentSecureTick goes through sys_ss_secure_rtc, whose RPCS3
// stub accepts the time and keeps nothing: CELL_OK, nothing kept, here too. cellRtcGetCurrentSecureTick reads the secure
// clock (seconds since 2000-01-01, sys_ss_secure_rtc 0x3002), which RPCS3's stub answers with the timebase counter (the
// date it gives moves 150 years ahead per minute the process runs); here it is the host's UTC time in whole seconds.

#include <array>
#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellRtc";

enum : u32 {
    CELL_RTC_ERROR_INVALID_POINTER = 0x80010602,
    CELL_RTC_ERROR_INVALID_VALUE = 0x80010603,
    CELL_RTC_ERROR_INVALID_ARG = 0x80010604,
    CELL_RTC_ERROR_BAD_PARSE = 0x80010607,
    CELL_RTC_ERROR_INVALID_YEAR = 0x80010621,
    CELL_RTC_ERROR_INVALID_MONTH = 0x80010622,
    CELL_RTC_ERROR_INVALID_DAY = 0x80010623,
    CELL_RTC_ERROR_INVALID_HOUR = 0x80010624,
    CELL_RTC_ERROR_INVALID_MINUTE = 0x80010625,
    CELL_RTC_ERROR_INVALID_SECOND = 0x80010626,
    CELL_RTC_ERROR_INVALID_MICROSECOND = 0x80010627,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u64 kTicksPerSecond = 1000000; // cellRtcGetTickResolution
constexpr u64 RTC_MAGIC_OFFSET = 62135596800000000ull;    // 1970-01-01 00:00:00.000000
constexpr u64 RTC_FILETIME_OFFSET = 50491123200000000ull; // 1601-01-01 00:00:00.000000
constexpr u64 RTC_SYSTEM_TIME_MIN = 63082281600000000ull; // 2000-01-01 00:00:00.000000
constexpr u64 RTC_SYSTEM_TIME_MAX = 66238041599999999ull; // 2099-12-31 23:59:59.999999
constexpr u32 DAYS_IN_400_YEARS = 365 * 400 + 97;
constexpr u32 DAYS_IN_100_YEARS = 365 * 100 + 24;
constexpr u32 DAYS_IN_4_YEARS = 365 * 4 + 1;
constexpr std::array<u8, 12> DAYS_IN_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr std::array<u8, 12> DAYS_IN_MONTH_LEAP = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr std::array<u16, 13> MONTH_OFFSET = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 0xFFFF};
constexpr std::array<u16, 13> MONTH_OFFSET_LEAP = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 0xFFFF};
constexpr std::array<std::string_view, 7> WEEKDAY_NAMES = {"sunday", "monday", "tuesday", "wednesday", "thursday",
    "friday", "saturday"};
constexpr std::array<std::string_view, 12> MONTH_NAMES = {"january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december"};
constexpr std::array<std::string_view, 70> TIME_ZONE_NAMES = {"gmt", "est", "edt", "cst", "cdt", "mst", "mdt", "pst",
    "pdt", "nzdt", "nzst", "idle", "nzt", "aesst", "acsst", "cadt", "sadt", "aest", "east", "gst", "ligt", "acst", "sast",
    "cast", "awsst", "jst", "kst", "wdt", "mt", "awst", "cct", "wadt", "wst", "jt", "wast", "it", "bt", "eetdst", "eet",
    "cetdst", "fwt", "ist", "mest", "metdst", "sst", "bst", "cet", "dnt", "fst", "met", "mewt", "mez", "nor", "set",
    "swt", "wetdst", "wet", "wat", "ndt", "adt", "nft", "nst", "ast", "ydt", "hdt", "yst", "ahst", "cat", "nt", "idlw"};
// In units of 30 minutes.
constexpr std::array<s8, 70> TIME_ZONE_VALUES = {0, -10, -8, -12, -10, -14, -12, -16, -14, 26, 24, 24, 24, 22, 21, 21,
    21, 20, 20, 20, 20, 19, 19, 19, 18, 18, 18, 18, 17, 16, 16, 16, 16, 15, 14, 7, 6, 6, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 0, -2, -3, -6, -5, -5, -8, -16, -18, -18, -20, -20, -22, -24};

// CellRtcDateTime (16 bytes): year, month, day, hour, minute, second (u16), microsecond (u32).
struct DateTime {
    u16 year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    u32 microsecond = 0;
};

DateTime read_date_time(u8* base, u32 at)
{
    return {ps_read16(base, at), ps_read16(base, at + 2), ps_read16(base, at + 4), ps_read16(base, at + 6),
        ps_read16(base, at + 8), ps_read16(base, at + 10), ps_read32(base, at + 12)};
}

void write_date_time(u8* base, u32 at, const DateTime& d)
{
    ps_write16(base, at, d.year);
    ps_write16(base, at + 2, d.month);
    ps_write16(base, at + 4, d.day);
    ps_write16(base, at + 6, d.hour);
    ps_write16(base, at + 8, d.minute);
    ps_write16(base, at + 10, d.second);
    ps_write32(base, at + 12, d.microsecond);
}

bool is_leap_year(u32 year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

// RPCS3 tick_to_date_time.
DateTime tick_to_date_time(u64 tick)
{
    const u32 microseconds = static_cast<u32>(tick % 1000000ull);
    const u16 seconds = static_cast<u16>((tick / 1000000ull) % 60);
    const u16 minutes = static_cast<u16>((tick / (60ull * 1000000ull)) % 60);
    const u16 hours = static_cast<u16>((tick / (60ull * 60ull * 1000000ull)) % 24);
    u32 days = static_cast<u32>(tick / (24ull * 60ull * 60ull * 1000000ull));
    const u32 year_400 = days / DAYS_IN_400_YEARS;
    days -= year_400 * DAYS_IN_400_YEARS;
    const u32 year_in_400 =
        (days - days / (DAYS_IN_4_YEARS - 1) + days / DAYS_IN_100_YEARS - days / (DAYS_IN_400_YEARS - 1)) / 365;
    days -= year_in_400 * 365 + year_in_400 / 4 - year_in_400 / 100 + year_in_400 / 400;
    const u16 years = static_cast<u16>(year_400 * 400 + year_in_400 + 1);
    const auto& month_offset = is_leap_year(years) ? MONTH_OFFSET_LEAP : MONTH_OFFSET;
    u32 month = days / 29;
    if (month_offset[month] > days) month--;
    days -= month_offset[month];
    return {years, static_cast<u16>(month + 1), static_cast<u16>(days + 1), hours, minutes, seconds, microseconds};
}

// RPCS3 date_time_to_tick (a month outside 1..12 is clamped; libcellRtc does not check it).
u64 date_time_to_tick(DateTime d)
{
    const u32 days_in_previous_years =
        u32{d.year} * 365 + (u32{d.year} + 3) / 4 - (u32{d.year} + 99) / 100 + (u32{d.year} + 399) / 400 - 366;
    if (d.month == 0) d.month = 1;
    else if (d.month > 12) d.month = 12;
    const u32 days_in_previous_months = is_leap_year(d.year) ? MONTH_OFFSET_LEAP[d.month - 1u] : MONTH_OFFSET[d.month - 1u];
    const u64 days = u64{days_in_previous_years} + days_in_previous_months + d.day - 1;
    return d.microsecond + u64{d.second} * 1000000ull + u64{d.minute} * 60ull * 1000000ull +
           u64{d.hour} * 60ull * 60ull * 1000000ull + days * 24ull * 60ull * 60ull * 1000000ull;
}

u64 current_tick()
{
    s64 sec = 0, nsec = 0;
    lv2::host_current_time(sec, nsec);
    return static_cast<u64>(nsec / 1000) + static_cast<u64>(sec) * kTicksPerSecond + RTC_MAGIC_OFFSET;
}

s32 local_time_zone()
{
    s32 timezone = 0, summertime = 0;
    lv2::host_time_zone(timezone, summertime);
    return timezone + summertime;
}

// RPCS3 cellRtcTickAddMinutes's arithmetic, wrapping at 64 bits as the PS3's does (RPCS3's s64 product would overflow).
u64 add_minutes(u64 tick, s64 minutes) { return tick + static_cast<u64>(minutes) * 60 * kTicksPerSecond; }

// RPCS3 cellRtcGetTick on a date the functions build themselves: a year of 0 or from 10000 is INVALID_VALUE and writes
// nothing, so the tick keeps what it held (RPCS3's vm::var starts at 0).
u64 tick_or(const DateTime& d, u64 otherwise) { return d.year == 0 || d.year >= 10000 ? otherwise : date_time_to_tick(d); }

// RPCS3 cellRtcCheckValid.
s32 check_valid(const DateTime& d)
{
    if (d.year == 0 || d.year >= 10000) return error(CELL_RTC_ERROR_INVALID_YEAR);
    if (d.month < 1 || d.month > 12) return error(CELL_RTC_ERROR_INVALID_MONTH);
    const auto& days_in_month = is_leap_year(d.year) ? DAYS_IN_MONTH_LEAP : DAYS_IN_MONTH;
    if (d.day == 0 || d.day > days_in_month[d.month - 1u]) return error(CELL_RTC_ERROR_INVALID_DAY);
    if (d.hour >= 24) return error(CELL_RTC_ERROR_INVALID_HOUR);
    if (d.minute >= 60) return error(CELL_RTC_ERROR_INVALID_MINUTE);
    if (d.second >= 60) return error(CELL_RTC_ERROR_INVALID_SECOND);
    if (d.microsecond >= kTicksPerSecond) return error(CELL_RTC_ERROR_INVALID_MICROSECOND);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetDayOfWeek: Zeller's congruence, 0 for Sunday (no checks). In 64 bits, where RPCS3's s32 arithmetic
// would overflow for years beyond a hundred million.
s32 day_of_week(s32 year_in, s32 month_in, s32 day)
{
    s64 year = year_in, month = month_in;
    if (month == 1 || month == 2) {
        year--;
        month += 12;
    }
    return static_cast<s32>(((month * 13 + 8) / 5 + year + year / 4 - year / 100 + year / 400 + day) % 7);
}

bool pointer_ok(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

// A pointer RPCS3 reads without a check of its page (it ensure()s it isn't NULL, or tests only that): refused with
// INVALID_POINTER and the call site logged, where RPCS3 and a PS3 would fault (D-042).
s32 bad_pointer(PPUContext& ctx, std::string_view fn, u32 addr)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}: the pointer {:#x} is not guest memory; refused with "
        "CELL_RTC_ERROR_INVALID_POINTER", fn, addr));
    return error(CELL_RTC_ERROR_INVALID_POINTER);
}

s32 cellRtcGetCurrentTick(PPUContext& ctx, u32 tick)
{
    if (!pointer_ok(ctx, tick, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    ps_write64(guest_base(ctx), tick, current_tick());
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetCurrentClock(pClock, iTimeZone): the current tick moved by iTimeZone minutes.
s32 cellRtcGetCurrentClock(PPUContext& ctx, u32 clock, s32 time_zone)
{
    if (!pointer_ok(ctx, clock, 16)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    write_date_time(guest_base(ctx), clock, tick_to_date_time(add_minutes(current_tick(), time_zone)));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetCurrentClockLocalTime: the current tick moved by the time zone and daylight saving offset.
s32 cellRtcGetCurrentClockLocalTime(PPUContext& ctx, u32 clock)
{
    if (!pointer_ok(ctx, clock, 16)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    write_date_time(guest_base(ctx), clock, tick_to_date_time(add_minutes(current_tick(), local_time_zone())));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetTick: a year of 0 or past 9999 is INVALID_VALUE; nothing else is checked.
s32 cellRtcGetTick(PPUContext& ctx, u32 time, u32 tick)
{
    if (!pointer_ok(ctx, time, 16) || !pointer_ok(ctx, tick, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    const DateTime d = read_date_time(base, time);
    if (d.year >= 10000 || d.year == 0) return error(CELL_RTC_ERROR_INVALID_VALUE);
    ps_write64(base, tick, date_time_to_tick(d));
    return lv2::CELL_OK;
}

s32 cellRtcSetTick(PPUContext& ctx, u32 time, u32 tick)
{
    if (!pointer_ok(ctx, time, 16) || !pointer_ok(ctx, tick, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    write_date_time(base, time, tick_to_date_time(ps_read64(base, tick)));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcSetWin32FileTime: 100 ns units since 1601.
s32 cellRtcSetWin32FileTime(PPUContext& ctx, u32 date_time, u64 file_time)
{
    if (!pointer_ok(ctx, date_time, 16)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    write_date_time(guest_base(ctx), date_time, tick_to_date_time(file_time / 10 + RTC_FILETIME_OFFSET));
    return lv2::CELL_OK;
}

char ascii(u32 n) { return static_cast<char>('0' + n); }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_blank(char c) { return c == ' ' || c == '\t'; }
s32 digit(char c) { return c - '0'; }

// RPCS3 cellRtcFormatRfc2822 ("Sun, 06 Nov 1994 08:49:37 +0000", 31 characters and a NUL), for the time zone given in
// minutes.
s32 format_rfc2822(PPUContext& ctx, u32 out, u32 utc, s32 time_zone)
{
    if (!pointer_ok(ctx, out, 32) || !pointer_ok(ctx, utc, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    const DateTime d = tick_to_date_time(add_minutes(ps_read64(base, utc), time_zone));
    if (const s32 r = check_valid(d); r != lv2::CELL_OK) return r;
    char s[32];
    const std::string_view weekday = WEEKDAY_NAMES[static_cast<u32>(day_of_week(d.year, d.month, d.day))];
    const std::string_view month = MONTH_NAMES[d.month - 1u];
    s[0] = upper(weekday[0]);
    s[1] = weekday[1];
    s[2] = weekday[2];
    s[3] = ',';
    s[4] = ' ';
    s[5] = ascii(d.day / 10u);
    s[6] = ascii(d.day % 10u);
    s[7] = ' ';
    s[8] = upper(month[0]);
    s[9] = month[1];
    s[10] = month[2];
    s[11] = ' ';
    s[12] = ascii(d.year / 1000u);
    s[13] = ascii(d.year / 100u % 10u);
    s[14] = ascii(d.year / 10u % 10u);
    s[15] = ascii(d.year % 10u);
    s[16] = ' ';
    s[17] = ascii(d.hour / 10u);
    s[18] = ascii(d.hour % 10u);
    s[19] = ':';
    s[20] = ascii(d.minute / 10u);
    s[21] = ascii(d.minute % 10u);
    s[22] = ':';
    s[23] = ascii(d.second / 10u);
    s[24] = ascii(d.second % 10u);
    s[25] = ' ';
    s[26] = time_zone < 0 ? '-' : '+';
    const u32 zone = static_cast<u32>(time_zone < 0 ? -time_zone : time_zone);
    const u32 zone_hours = zone / 60 % 100, zone_minutes = zone % 60;
    s[27] = ascii(zone_hours / 10);
    s[28] = ascii(zone_hours % 10);
    s[29] = ascii(zone_minutes / 10);
    s[30] = ascii(zone_minutes % 10);
    s[31] = '\0';
    std::memcpy(base + out, s, sizeof s);
    return lv2::CELL_OK;
}

s32 cellRtcFormatRfc2822LocalTime(PPUContext& ctx, u32 out, u32 utc)
{
    if (!pointer_ok(ctx, out, 32) || !pointer_ok(ctx, utc, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    return format_rfc2822(ctx, out, utc, local_time_zone());
}

// The parsers read the guest string as RPCS3 does: character by character, the NUL ending any match.
class Parser {
public:
    Parser(u8* base, u32 str) : base_(base), str_(str) {}
    char operator[](u32 pos) const { return static_cast<char>(base_[str_ + pos]); }

    // RPCS3 rtcParseComponent: an optional delimiter, then one or two digits; 0xFFFF when there are none.
    u16 component(u32& pos, char delimiter) const
    {
        if (delimiter) {
            if ((*this)[pos] != delimiter) return 0xFFFF;
            pos++;
        }
        if (!is_digit((*this)[pos])) return 0xFFFF;
        u16 v = static_cast<u16>(digit((*this)[pos++]));
        if (is_digit((*this)[pos])) v = static_cast<u16>(v * 10 + digit((*this)[pos++]));
        return v;
    }

    // RPCS3 rtcParseName: a full name, or (when allowed) its first three letters or more, case-insensitively; the index,
    // or the table's size for no match.
    template <usize N>
    u32 name(u32& pos, const std::array<std::string_view, N>& names, bool allow_short = true) const
    {
        for (u32 i = 0; i < N; i++) {
            const u32 length = static_cast<u32>(names[i].length());
            u32 c = 0;
            while (c < length && lower((*this)[pos + c]) == names[i][c]) c++;
            if (c == length) {
                pos += length;
                return i;
            }
            if (allow_short && c >= 3) {
                pos += 3; // only 3, however many letters matched
                return i;
            }
        }
        return static_cast<u32>(N);
    }

    bool four_digits(u32 pos) const
    {
        return is_digit((*this)[pos]) && is_digit((*this)[pos + 1]) && is_digit((*this)[pos + 2]) &&
               is_digit((*this)[pos + 3]);
    }

private:
    u8* base_;
    u32 str_;
};

// How RPCS3's RFC parsers end: cellRtcGetTick(date, pUtc), then cellRtcTickAddMinutes(pUtc, pUtc, minutes). A year of 0
// (or from 10000) makes cellRtcGetTick write nothing (INVALID_VALUE, ignored), and the minutes go onto whatever *pUtc
// held.
void store_parsed(u8* base, u32 utc, const DateTime& d, s64 minutes)
{
    ps_write64(base, utc, add_minutes(tick_or(d, ps_read64(base, utc)), minutes));
}

// RPCS3 rtcParseRfc2822: "06 Nov 1994 08:49:37 +0000" and its variants (two-digit years, no seconds, named and military
// time zones), from after the optional day of the week.
s32 parse_rfc2822(u8* base, u32 utc, const Parser& p, u32 pos)
{
    const u16 day = p.component(pos, 0);
    if (day == 0xFFFF) return error(CELL_RTC_ERROR_BAD_PARSE);
    if (p[pos] != ' ' && p[pos] != '-') return error(CELL_RTC_ERROR_BAD_PARSE);
    pos++;
    const u32 month = p.name(pos, MONTH_NAMES) + 1;
    if (month > MONTH_NAMES.size()) return error(CELL_RTC_ERROR_BAD_PARSE);
    if (p[pos] != ' ' && p[pos] != '-') return error(CELL_RTC_ERROR_BAD_PARSE);
    pos++;
    if (!is_digit(p[pos]) || !is_digit(p[pos + 1])) return error(CELL_RTC_ERROR_BAD_PARSE);
    u16 year = 0;
    if (!is_digit(p[pos + 2]) || !is_digit(p[pos + 3])) {
        year = static_cast<u16>(digit(p[pos]) * 10 + digit(p[pos + 1]));
        year = static_cast<u16>(year + (year < 50 ? 2000 : 1900));
        pos += 2;
    } else {
        year = static_cast<u16>(digit(p[pos]) * 1000 + digit(p[pos + 1]) * 100 + digit(p[pos + 2]) * 10 + digit(p[pos + 3]));
        pos += 4;
    }
    const u16 hour = p.component(pos, ' ');
    if (hour == 0xFFFF || hour > 25) return error(CELL_RTC_ERROR_BAD_PARSE); // libcellRtc allows 25
    const u16 minute = p.component(pos, ':');
    if (minute == 0xFFFF) return error(CELL_RTC_ERROR_BAD_PARSE);
    u16 second = 0;
    if (p[pos] != ' ') {
        second = p.component(pos, ':');
        if (second == 0xFFFF) return error(CELL_RTC_ERROR_BAD_PARSE);
    } else {
        pos++; // without seconds, a time zone needs two spaces before it
    }
    s32 time_zone = 0;
    if (p[pos] == ' ') {
        pos++;
        if (p[pos] == '+' || p[pos] == '-') {
            if (p.four_digits(pos + 1)) {
                const s32 hhmm = digit(p[pos + 1]) * 1000 + digit(p[pos + 2]) * 100 + digit(p[pos + 3]) * 10 + digit(p[pos + 4]);
                time_zone = hhmm / 100 * 60 + hhmm % 60; // libcellRtc: % 60
            } else {
                time_zone = -1; // no error, as libcellRtc
            }
            if (p[pos] == '-') time_zone = -time_zone;
        } else if (p[pos] != 'U' && p[pos + 1] != 'T') { // libcellRtc: &&
            const u32 zone = p.name(pos, TIME_ZONE_NAMES, false);
            if (zone < TIME_ZONE_NAMES.size()) {
                time_zone = TIME_ZONE_VALUES[zone] * 30;
            } else {
                // Military time zones, off by one as in libcellRtc.
                const char letter = upper(p[pos]);
                if (letter >= 'A' && letter <= 'M' && letter != 'J') time_zone = (letter - 'A') * 60;
                else if (letter >= 'N' && letter <= 'Y') time_zone = ('N' - letter) * 60;
                else if (letter != 'Z') return error(CELL_RTC_ERROR_BAD_PARSE);
            }
        }
    }
    const DateTime d{year, static_cast<u16>(month), day, hour, minute, second, 0};
    store_parsed(base, utc, d, -time_zone);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcParseRfc3339: "1995-12-03T13:23:00.00Z" or with a "+hh:mm" offset.
s32 parse_rfc3339(u8* base, u32 utc, const Parser& p)
{
    DateTime d;
    d.year = p.four_digits(0) ? static_cast<u16>(digit(p[0]) * 1000 + digit(p[1]) * 100 + digit(p[2]) * 10 + digit(p[3]))
                              : 0xFFFF;
    if (p[4] != '-') return error(CELL_RTC_ERROR_INVALID_YEAR);
    const auto two = [&](u32 at) -> u16 {
        return is_digit(p[at]) && is_digit(p[at + 1]) ? static_cast<u16>(digit(p[at]) * 10 + digit(p[at + 1])) : 0xFFFF;
    };
    d.month = two(5);
    if (p[7] != '-') return error(CELL_RTC_ERROR_INVALID_MONTH);
    d.day = two(8);
    if (p[10] != 'T' && p[10] != 't') return error(CELL_RTC_ERROR_INVALID_DAY);
    d.hour = two(11);
    if (p[13] != ':') return error(CELL_RTC_ERROR_INVALID_HOUR);
    d.minute = two(14);
    if (p[16] != ':') return error(CELL_RTC_ERROR_INVALID_MINUTE);
    d.second = two(17);
    u32 pos = 19;
    if (p[pos] == '.') {
        u32 mul = 100000;
        for (char c = p[++pos]; is_digit(c); c = p[++pos]) {
            d.microsecond += static_cast<u32>(digit(c)) * mul;
            mul /= 10;
        }
    }
    const char sign = p[pos];
    if (sign != 'Z' && sign != 'z' && sign != '+' && sign != '-') return error(CELL_RTC_ERROR_BAD_PARSE);
    s64 minutes = 0;
    if (sign == '+' || sign == '-') {
        if (!is_digit(p[pos + 1]) || !is_digit(p[pos + 2]) || p[pos + 3] != ':' || !is_digit(p[pos + 4]) ||
            !is_digit(p[pos + 5])) {
            return error(CELL_RTC_ERROR_BAD_PARSE);
        }
        minutes = (digit(p[pos + 1]) * 10 + digit(p[pos + 2])) * 60 + digit(p[pos + 4]) * 10 + digit(p[pos + 5]);
        if (sign == '+') minutes = -minutes;
    }
    store_parsed(base, utc, d, minutes);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcParseDateTime: RFC 3339 when it starts with a four-digit year, else asctime ("Sun Nov  6 08:49:37 1994")
// after a day of the week, else RFC 2822.
s32 parse_date_time(u8* base, u32 utc, u32 str)
{
    const Parser whole(base, str);
    u32 pos = 0;
    while (is_blank(whole[pos])) pos++;
    if (whole.four_digits(pos)) return parse_rfc3339(base, utc, Parser(base, str + pos));
    if (whole.name(pos, WEEKDAY_NAMES) == WEEKDAY_NAMES.size()) return error(CELL_RTC_ERROR_BAD_PARSE);
    if (whole[pos] == ',') pos++;
    while (is_blank(whole[pos])) pos++;
    const u32 month = whole.name(pos, MONTH_NAMES) + 1;
    if (month > MONTH_NAMES.size()) return parse_rfc2822(base, utc, whole, pos);
    if (whole[pos] != ' ') return error(CELL_RTC_ERROR_BAD_PARSE);
    pos++;
    u16 day = 0;
    if (whole[pos] == ' ') {
        pos++;
        // libcellRtc compares a signed char against '9' only, so anything below '0' passes (RPCS3 logs it).
        if (whole[pos] > '9') return error(CELL_RTC_ERROR_BAD_PARSE);
        day = static_cast<u16>(static_cast<s32>(static_cast<signed char>(whole[pos])) - '0');
        pos++;
    } else if (is_digit(whole[pos])) {
        day = static_cast<u16>(digit(whole[pos++]));
        if (is_digit(whole[pos])) day = static_cast<u16>(day * 10 + digit(whole[pos++]));
    } else {
        return error(CELL_RTC_ERROR_BAD_PARSE);
    }
    const u16 hour = whole.component(pos, ' ');
    if (hour == 0xFFFF) return error(CELL_RTC_ERROR_BAD_PARSE);
    const u16 minute = whole.component(pos, ':');
    if (minute == 0xFFFF) return error(CELL_RTC_ERROR_BAD_PARSE);
    const u16 second = whole.component(pos, ':');
    if (second == 0xFFFF) return error(CELL_RTC_ERROR_BAD_PARSE);
    if (whole[pos] != ' ') return error(CELL_RTC_ERROR_BAD_PARSE);
    pos++;
    if (!whole.four_digits(pos)) return error(CELL_RTC_ERROR_BAD_PARSE);
    const u16 year = static_cast<u16>(digit(whole[pos]) * 1000 + digit(whole[pos + 1]) * 100 + digit(whole[pos + 2]) * 10 +
                                      digit(whole[pos + 3]));
    const DateTime d{year, static_cast<u16>(month), day, hour, minute, second, 0};
    // RPCS3 goes through cellRtcGetTick, whose year check fails only for 0, which four digits can't give... except
    // "0000": then no tick is written (INVALID_VALUE ignored), as there.
    if (d.year != 0) ps_write64(base, utc, date_time_to_tick(d));
    return lv2::CELL_OK;
}

s32 cellRtcParseDateTime(PPUContext& ctx, u32 utc, u32 str)
{
    if (!pointer_ok(ctx, utc, 8) || !pointer_ok(ctx, str, 1)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    const s32 r = parse_date_time(base, utc, str);
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellRtcParseDateTime(\"{}\"): {:#x}", std::string_view(reinterpret_cast<const char*>(base + str),
            strnlen(reinterpret_cast<const char*>(base + str), 64)), static_cast<u32>(r));
    }
    return r;
}

// RPCS3 cellRtcParseRfc3339(pUtc, pszDateTime): the parser cellRtcParseDateTime uses for a string starting with a year,
// on its own: "1995-12-03T13:23:00.00Z" or with a "+hh:mm" offset; INVALID_YEAR..INVALID_DAY, INVALID_HOUR and
// INVALID_MINUTE for a missing separator after that field, BAD_PARSE for a bad offset.
s32 cellRtcParseRfc3339(PPUContext& ctx, u32 utc, u32 str)
{
    if (!pointer_ok(ctx, utc, 8) || !pointer_ok(ctx, str, 1)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    return parse_rfc3339(base, utc, Parser(base, str));
}

s32 cellRtcFormatRfc2822(PPUContext& ctx, u32 out, u32 utc, s32 time_zone)
{
    return format_rfc2822(ctx, out, utc, time_zone);
}

// RPCS3 cellRtcFormatRfc3339(pszDateTime, pUtc, iTimeZone): "2020-04-13T10:56:31.35Z" (hundredths of a second), or with
// "+hh:mm" for a time zone other than 0 (in minutes; the hours modulo 100), for the tick moved by the time zone; the
// date's cellRtcCheckValid error if it is not one.
s32 format_rfc3339(PPUContext& ctx, u32 out, u32 utc, s32 time_zone)
{
    if (!pointer_ok(ctx, out, time_zone ? 29 : 24) || !pointer_ok(ctx, utc, 8)) {
        return error(CELL_RTC_ERROR_INVALID_POINTER);
    }
    u8* base = guest_base(ctx);
    const DateTime d = tick_to_date_time(add_minutes(ps_read64(base, utc), time_zone));
    if (const s32 r = check_valid(d); r != lv2::CELL_OK) return r;
    char s[29] = {};
    s[0] = ascii(d.year / 1000u);
    s[1] = ascii(d.year / 100u % 10u);
    s[2] = ascii(d.year / 10u % 10u);
    s[3] = ascii(d.year % 10u);
    s[4] = '-';
    s[5] = ascii(d.month / 10u);
    s[6] = ascii(d.month % 10u);
    s[7] = '-';
    s[8] = ascii(d.day / 10u);
    s[9] = ascii(d.day % 10u);
    s[10] = 'T';
    s[11] = ascii(d.hour / 10u);
    s[12] = ascii(d.hour % 10u);
    s[13] = ':';
    s[14] = ascii(d.minute / 10u);
    s[15] = ascii(d.minute % 10u);
    s[16] = ':';
    s[17] = ascii(d.second / 10u);
    s[18] = ascii(d.second % 10u);
    s[19] = '.';
    s[20] = ascii(d.microsecond / 100000u);
    s[21] = ascii(d.microsecond / 10000u % 10u);
    u32 length = 24;
    if (time_zone == 0) {
        s[22] = 'Z';
    } else {
        s[22] = time_zone < 0 ? '-' : '+';
        const u32 zone = static_cast<u32>(time_zone < 0 ? -static_cast<s64>(time_zone) : time_zone);
        const u32 zone_hours = zone / 60 % 100, zone_minutes = zone % 60;
        s[23] = ascii(zone_hours / 10);
        s[24] = ascii(zone_hours % 10);
        s[25] = ':';
        s[26] = ascii(zone_minutes / 10);
        s[27] = ascii(zone_minutes % 10);
        length = 29;
    }
    std::memcpy(base + out, s, length);
    return lv2::CELL_OK;
}

s32 cellRtcFormatRfc3339(PPUContext& ctx, u32 out, u32 utc, s32 time_zone)
{
    return format_rfc3339(ctx, out, utc, time_zone);
}

// RPCS3 cellRtcFormatRfc3339LocalTime: in the local time zone (time zone and daylight saving offset).
s32 cellRtcFormatRfc3339LocalTime(PPUContext& ctx, u32 out, u32 utc)
{
    if (!pointer_ok(ctx, out, 1) || !pointer_ok(ctx, utc, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    return format_rfc3339(ctx, out, utc, local_time_zone());
}

// RPCS3 cellRtcTickAddTicks .. cellRtcTickAddWeeks(pTick0, pTick1, add): *pTick0 = *pTick1 + add units, both pointers
// checked (pTick0 first); 64-bit arithmetic that wraps.
s32 tick_add(PPUContext& ctx, u32 tick0, u32 tick1, u64 delta)
{
    if (!pointer_ok(ctx, tick0, 8) || !pointer_ok(ctx, tick1, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    ps_write64(base, tick0, ps_read64(base, tick1) + delta);
    return lv2::CELL_OK;
}

s32 cellRtcTickAddTicks(PPUContext& ctx, u32 tick0, u32 tick1, s64 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(add));
}
s32 cellRtcTickAddMicroseconds(PPUContext& ctx, u32 tick0, u32 tick1, s64 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(add));
}
s32 cellRtcTickAddSeconds(PPUContext& ctx, u32 tick0, u32 tick1, s64 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(add) * kTicksPerSecond);
}
s32 cellRtcTickAddMinutes(PPUContext& ctx, u32 tick0, u32 tick1, s64 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(add) * 60 * kTicksPerSecond);
}
s32 cellRtcTickAddHours(PPUContext& ctx, u32 tick0, u32 tick1, s32 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(static_cast<s64>(add)) * 60 * 60 * kTicksPerSecond);
}
s32 cellRtcTickAddDays(PPUContext& ctx, u32 tick0, u32 tick1, s32 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(static_cast<s64>(add)) * 24 * 60 * 60 * kTicksPerSecond);
}
s32 cellRtcTickAddWeeks(PPUContext& ctx, u32 tick0, u32 tick1, s32 add)
{
    return tick_add(ctx, tick0, tick1, static_cast<u64>(static_cast<s64>(add)) * 7 * 24 * 60 * 60 * kTicksPerSecond);
}

// The days in a month; RPCS3's TickAddMonths and TickAddYears put libcellRtc's INVALID_ARG there (a negative number, so
// the day is then clamped to its low 16 bits, 0x0604) for a year of 0 or a month outside 1..12.
s32 month_days(u16 year, u16 month)
{
    if (year == 0 || month < 1 || month > 12) return error(CELL_RTC_ERROR_INVALID_ARG);
    return (is_leap_year(year) ? DAYS_IN_MONTH_LEAP : DAYS_IN_MONTH)[month - 1u];
}

// RPCS3 cellRtcTickAddMonths(pTick0, pTick1, iAdd): the date of *pTick1 moved by iAdd months (the year and month
// truncated to 16 bits as RPCS3's u16s), the day clamped to the new month's last; then cellRtcGetTick into *pTick0,
// which writes nothing for a year of 0 or from 10000. Returns CELL_OK even then.
s32 cellRtcTickAddMonths(PPUContext& ctx, u32 tick0, u32 tick1, s32 add)
{
    if (!pointer_ok(ctx, tick0, 8) || !pointer_ok(ctx, tick1, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    DateTime d = tick_to_date_time(ps_read64(base, tick1));
    // 32-bit arithmetic that wraps, as the PS3's (RPCS3's s32 sums would overflow for an iAdd near the limits).
    const s32 total_months = static_cast<s32>(u32{d.year} * 12u + d.month + static_cast<u32>(add) - 1u);
    const u16 new_year = static_cast<u16>(total_months / 12);
    const u16 new_month = static_cast<u16>(static_cast<u32>(total_months) - u32{new_year} * 12u + 1u);
    const s32 days = month_days(new_year, new_month);
    if (days < static_cast<s32>(d.day)) d.day = static_cast<u16>(days);
    d.month = new_month;
    d.year = new_year;
    ps_write64(base, tick0, tick_or(d, ps_read64(base, tick0)));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcTickAddYears(pTick0, pTick1, iAdd): the year moved (truncated to 16 bits), the day clamped to the
// month's last (29 February into a common year), then cellRtcGetTick into *pTick0 as TickAddMonths.
s32 cellRtcTickAddYears(PPUContext& ctx, u32 tick0, u32 tick1, s32 add)
{
    if (!pointer_ok(ctx, tick0, 8) || !pointer_ok(ctx, tick1, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    DateTime d = tick_to_date_time(ps_read64(base, tick1));
    const u16 new_year = static_cast<u16>(u32{d.year} + static_cast<u32>(add));
    const s32 days = month_days(new_year, d.month);
    if (days < static_cast<s32>(d.day)) d.day = static_cast<u16>(days);
    d.year = new_year;
    ps_write64(base, tick0, tick_or(d, ps_read64(base, tick0)));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcConvertUtcToLocalTime(pUtc, pLocalTime): cellRtcTickAddMinutes(pLocalTime, pUtc, time zone + daylight
// saving offset).
s32 cellRtcConvertUtcToLocalTime(PPUContext& ctx, u32 utc, u32 local)
{
    return tick_add(ctx, local, utc, static_cast<u64>(static_cast<s64>(local_time_zone())) * 60 * kTicksPerSecond);
}

// RPCS3 cellRtcConvertLocalTimeToUtc(pLocalTime, pUtc): cellRtcTickAddMinutes(pUtc, pLocalTime, -(time zone + daylight
// saving offset)).
s32 cellRtcConvertLocalTimeToUtc(PPUContext& ctx, u32 local, u32 utc)
{
    return tick_add(ctx, utc, local, static_cast<u64>(-static_cast<s64>(local_time_zone())) * 60 * kTicksPerSecond);
}

// RPCS3 cellRtcGetCurrentSecureTick(tick): 2000-01-01 plus the secure clock's seconds. The secure clock here is the
// host's UTC time (see the top of the file).
s32 cellRtcGetCurrentSecureTick(PPUContext& ctx, u32 tick)
{
    if (!pointer_ok(ctx, tick, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    const u64 now = current_tick();
    const u64 seconds = now > RTC_SYSTEM_TIME_MIN ? (now - RTC_SYSTEM_TIME_MIN) / kTicksPerSecond : 0;
    ps_write64(guest_base(ctx), tick, RTC_SYSTEM_TIME_MIN + seconds * kTicksPerSecond);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetDosTime(pDateTime, puiDosTime): the date as an MS-DOS time (years from 1980 in 7 bits, month, day;
// hours, minutes, seconds / 2). Before 1980: 0 and -1; from 2108: 0xFF9FBF7D (the highest) and -1.
s32 cellRtcGetDosTime(PPUContext& ctx, u32 date_time, u32 dos_time)
{
    if (!pointer_ok(ctx, date_time, 16) || !pointer_ok(ctx, dos_time, 4)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    const DateTime d = read_date_time(base, date_time);
    if (d.year < 1980) {
        ps_write32(base, dos_time, 0);
        return -1;
    }
    if (d.year >= 2108) {
        ps_write32(base, dos_time, 0xFF9FBF7D);
        return -1;
    }
    const u32 ymd = ((u32{d.year} - 1980) & 0x7F) << 9 | (d.month & 0xFu) << 5 | (d.day & 0x1Fu);
    const u32 hms = (d.hour & 0x1Fu) << 11 | (d.minute & 0x3Fu) << 5 | ((d.second >> 1) & 0x1Fu);
    ps_write32(base, dos_time, ymd << 16 | hms);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetSystemTime(pDateTime, pTimeStamp): seconds since 2000-01-01 (through cellRtcGetTick, 0 for a year it
// refuses). Before 2000: 0 and INVALID_VALUE; from 2100: the seconds to the end of 2099 and INVALID_VALUE.
s32 cellRtcGetSystemTime(PPUContext& ctx, u32 date_time, u32 time_stamp)
{
    if (!pointer_ok(ctx, date_time, 16) || !pointer_ok(ctx, time_stamp, 8)) {
        return error(CELL_RTC_ERROR_INVALID_POINTER);
    }
    u8* base = guest_base(ctx);
    const u64 tick = tick_or(read_date_time(base, date_time), 0);
    if (tick < RTC_SYSTEM_TIME_MIN) {
        ps_write64(base, time_stamp, 0);
        return error(CELL_RTC_ERROR_INVALID_VALUE);
    }
    if (tick >= RTC_SYSTEM_TIME_MAX + kTicksPerSecond) {
        ps_write64(base, time_stamp, (RTC_SYSTEM_TIME_MAX - RTC_SYSTEM_TIME_MIN) / kTicksPerSecond);
        return error(CELL_RTC_ERROR_INVALID_VALUE);
    }
    ps_write64(base, time_stamp, (tick - RTC_SYSTEM_TIME_MIN) / kTicksPerSecond);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetTime_t(pDateTime, piTime): seconds since 1970-01-01; before it, 0 and INVALID_VALUE.
s32 cellRtcGetTime_t(PPUContext& ctx, u32 date_time, u32 time)
{
    if (!pointer_ok(ctx, date_time, 16) || !pointer_ok(ctx, time, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    const u64 tick = tick_or(read_date_time(base, date_time), 0);
    if (tick < RTC_MAGIC_OFFSET) {
        ps_write64(base, time, 0);
        return error(CELL_RTC_ERROR_INVALID_VALUE);
    }
    ps_write64(base, time, (tick - RTC_MAGIC_OFFSET) / kTicksPerSecond);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetWin32FileTime(pDateTime, pulWin32FileTime): 100 ns units since 1601-01-01; before it, 0 and
// INVALID_VALUE.
s32 cellRtcGetWin32FileTime(PPUContext& ctx, u32 date_time, u32 file_time)
{
    if (!pointer_ok(ctx, date_time, 16) || !pointer_ok(ctx, file_time, 8)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    u8* base = guest_base(ctx);
    const u64 tick = tick_or(read_date_time(base, date_time), 0);
    if (tick < RTC_FILETIME_OFFSET) {
        ps_write64(base, file_time, 0);
        return error(CELL_RTC_ERROR_INVALID_VALUE);
    }
    ps_write64(base, file_time, (tick - RTC_FILETIME_OFFSET) * 10);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcGetTickResolution: ticks per second.
u32 cellRtcGetTickResolution(PPUContext&) { return static_cast<u32>(kTicksPerSecond); }

// RPCS3 cellRtcSetConf(unk1, unk2, timezone, summertime): the first two are ignored; lv2's sys_time_set_timezone,
// which changes nothing and refuses a process without root permissions (CELL_ENOSYS).
s32 cellRtcSetConf(PPUContext& ctx, s64, s64, u32 time_zone, u32 summer_time)
{
    return lv2::sys_time_set_timezone(thread_of(ctx), static_cast<s32>(time_zone), static_cast<s32>(summer_time));
}

// RPCS3 cellRtcSetCurrentSecureTick(pTick): INVALID_POINTER for NULL, INVALID_VALUE after 2099; then the secure clock is
// set (sys_ss_secure_rtc 0x3003), which RPCS3's stub accepts without keeping it: CELL_OK, nothing kept.
s32 cellRtcSetCurrentSecureTick(PPUContext& ctx, u32 tick)
{
    if (!tick) return error(CELL_RTC_ERROR_INVALID_POINTER);
    if (!pointer_ok(ctx, tick, 8)) return bad_pointer(ctx, "cellRtcSetCurrentSecureTick", tick);
    const u64 value = ps_read64(guest_base(ctx), tick);
    if (value > RTC_SYSTEM_TIME_MAX) return error(CELL_RTC_ERROR_INVALID_VALUE);
    PS_LOG_INFO(kChan, "cellRtcSetCurrentSecureTick({:#x}): not kept, as RPCS3", value);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcSetCurrentTick(pTick): INVALID_POINTER for NULL, INVALID_VALUE before 1970; then lv2's
// sys_time_set_current_time, which changes nothing and refuses a process without root permissions (CELL_ENOSYS, which is
// returned).
s32 cellRtcSetCurrentTick(PPUContext& ctx, u32 tick)
{
    if (!tick) return error(CELL_RTC_ERROR_INVALID_POINTER);
    if (!pointer_ok(ctx, tick, 8)) return bad_pointer(ctx, "cellRtcSetCurrentTick", tick);
    const u64 value = ps_read64(guest_base(ctx), tick);
    if (value < RTC_MAGIC_OFFSET) return error(CELL_RTC_ERROR_INVALID_VALUE);
    const u64 unix_time = value - RTC_MAGIC_OFFSET;
    const s32 r = lv2::sys_time_set_current_time(thread_of(ctx), static_cast<s64>(unix_time / kTicksPerSecond),
        static_cast<s64>(unix_time % kTicksPerSecond * 1000));
    return r >= lv2::CELL_OK ? lv2::CELL_OK : r;
}

// RPCS3 cellRtcSetDosTime(pDateTime, uiDosTime): an MS-DOS time to a date (no checks of the fields).
s32 cellRtcSetDosTime(PPUContext& ctx, u32 date_time, u32 dos_time)
{
    if (!pointer_ok(ctx, date_time, 16)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    const u32 hms = dos_time & 0xFFFF, ymd = dos_time >> 16;
    const DateTime d{static_cast<u16>((ymd >> 9) + 1980), static_cast<u16>((ymd >> 5) & 0xF), static_cast<u16>(ymd & 0x1F),
        static_cast<u16>(hms >> 11), static_cast<u16>((hms >> 5) & 0x3F), static_cast<u16>((hms << 1) & 0x3E), 0};
    write_date_time(guest_base(ctx), date_time, d);
    return lv2::CELL_OK;
}

// RPCS3 cellRtcSetSystemTime(pDateTime, iTime): the date iTime seconds after 2000-01-01.
s32 cellRtcSetSystemTime(PPUContext& ctx, u32 date_time, u64 time)
{
    if (!pointer_ok(ctx, date_time, 16)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    write_date_time(guest_base(ctx), date_time, tick_to_date_time(time * kTicksPerSecond + RTC_SYSTEM_TIME_MIN));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcSetTime_t(pDateTime, iTime): the date iTime seconds after 1970-01-01.
s32 cellRtcSetTime_t(PPUContext& ctx, u32 date_time, u64 time)
{
    if (!pointer_ok(ctx, date_time, 16)) return error(CELL_RTC_ERROR_INVALID_POINTER);
    write_date_time(guest_base(ctx), date_time, tick_to_date_time(time * kTicksPerSecond + RTC_MAGIC_OFFSET));
    return lv2::CELL_OK;
}

// RPCS3 cellRtcIsLeapYear(year): INVALID_ARG below 1, else 1 or 0 (as the result, not an error).
s32 cellRtcIsLeapYear(PPUContext&, s32 year)
{
    if (year < 1) return error(CELL_RTC_ERROR_INVALID_ARG);
    return is_leap_year(static_cast<u32>(year)) ? 1 : 0;
}

// RPCS3 cellRtcGetDaysInMonth(year, month): INVALID_ARG for a year below 1 or a month outside 1..12, else the days.
s32 cellRtcGetDaysInMonth(PPUContext&, s32 year, s32 month)
{
    if (year <= 0 || month <= 0 || month > 12) return error(CELL_RTC_ERROR_INVALID_ARG);
    return (is_leap_year(static_cast<u32>(year)) ? DAYS_IN_MONTH_LEAP : DAYS_IN_MONTH)[static_cast<u32>(month) - 1];
}

// RPCS3 cellRtcGetDayOfWeek(year, month, day): 0 (Sunday) to 6, no checks.
s32 cellRtcGetDayOfWeek(PPUContext&, s32 year, s32 month, s32 day) { return day_of_week(year, month, day); }

// RPCS3 cellRtcCheckValid(pTime): YEAR (0, from 10000), MONTH, DAY (for the month and year), HOUR, MINUTE, SECOND,
// MICROSECOND, in that order. RPCS3 ensure()s the pointer (libcellRtc doesn't check it).
s32 cellRtcCheckValid(PPUContext& ctx, u32 time)
{
    if (!pointer_ok(ctx, time, 16)) return bad_pointer(ctx, "cellRtcCheckValid", time);
    return check_valid(read_date_time(guest_base(ctx), time));
}

// RPCS3 cellRtcCompareTick(pTick0, pTick1): -1 when *pTick0 is earlier, 0 when equal, 1 when later. RPCS3 ensure()s
// both pointers (libcellRtc doesn't check them).
s32 cellRtcCompareTick(PPUContext& ctx, u32 tick0, u32 tick1)
{
    if (!pointer_ok(ctx, tick0, 8)) return bad_pointer(ctx, "cellRtcCompareTick", tick0);
    if (!pointer_ok(ctx, tick1, 8)) return bad_pointer(ctx, "cellRtcCompareTick", tick1);
    u8* base = guest_base(ctx);
    const u64 t0 = ps_read64(base, tick0), t1 = ps_read64(base, tick1);
    return t1 <= t0 ? (t1 < t0 ? 1 : 0) : -1;
}

} // namespace

void register_cellRtc(Registry& r)
{
    r.add("cellRtc", {
        PS_HLE_FUNC(0x9DAFC0D9, cellRtcGetCurrentTick),
        PS_HLE_FUNC(0x32C941CF, cellRtcGetCurrentClock),
        PS_HLE_FUNC(0x2CCE9CF5, cellRtcGetCurrentClockLocalTime),
        PS_HLE_FUNC(0x5491B9D5, cellRtcFormatRfc2822),
        PS_HLE_FUNC(0xA07C3D2F, cellRtcFormatRfc2822LocalTime),
        PS_HLE_FUNC(0xD9C0B463, cellRtcFormatRfc3339),
        PS_HLE_FUNC(0x1324948A, cellRtcFormatRfc3339LocalTime),
        PS_HLE_FUNC(0xC5BC0FAC, cellRtcParseDateTime),
        PS_HLE_FUNC(0xCF11C3D6, cellRtcParseRfc3339),
        PS_HLE_FUNC(0xC7BDB7EB, cellRtcGetTick),
        PS_HLE_FUNC(0x99B13034, cellRtcSetTick),
        PS_HLE_FUNC(0x269A1882, cellRtcTickAddTicks),
        PS_HLE_FUNC(0xF8509925, cellRtcTickAddMicroseconds),
        PS_HLE_FUNC(0xCCCE71BD, cellRtcTickAddSeconds),
        PS_HLE_FUNC(0x2F010BFA, cellRtcTickAddMinutes),
        PS_HLE_FUNC(0xD41D3BD2, cellRtcTickAddHours),
        PS_HLE_FUNC(0x75744E2A, cellRtcTickAddDays),
        PS_HLE_FUNC(0x64C63FD5, cellRtcTickAddWeeks),
        PS_HLE_FUNC(0xE0ECBB45, cellRtcTickAddMonths),
        PS_HLE_FUNC(0x332A74DD, cellRtcTickAddYears),
        PS_HLE_FUNC(0xC48D5002, cellRtcConvertUtcToLocalTime),
        PS_HLE_FUNC(0x46CA7FE0, cellRtcConvertLocalTimeToUtc),
        PS_HLE_FUNC(0xC4808FD7, cellRtcGetCurrentSecureTick),
        PS_HLE_FUNC(0xDFFF32CF, cellRtcGetDosTime),
        PS_HLE_FUNC(0x748BEDA3, cellRtcGetTickResolution),
        PS_HLE_FUNC(0xF582308D, cellRtcGetSystemTime),
        PS_HLE_FUNC(0xCB90C761, cellRtcGetTime_t),
        PS_HLE_FUNC(0xE7086F05, cellRtcGetWin32FileTime),
        PS_HLE_FUNC(0x5E78D18E, cellRtcSetConf),
        PS_HLE_FUNC(0x3DC09A3E, cellRtcSetCurrentSecureTick),
        PS_HLE_FUNC(0xEB22BB86, cellRtcSetCurrentTick),
        PS_HLE_FUNC(0x9598D4B3, cellRtcSetDosTime),
        PS_HLE_FUNC(0xBB543189, cellRtcSetTime_t),
        PS_HLE_FUNC(0xE22D74A4, cellRtcSetSystemTime),
        PS_HLE_FUNC(0x5F68C268, cellRtcSetWin32FileTime),
        PS_HLE_FUNC(0x5316B4A8, cellRtcIsLeapYear),
        PS_HLE_FUNC(0x5B6A0A1D, cellRtcGetDaysInMonth),
        PS_HLE_FUNC(0xC2D8CF95, cellRtcGetDayOfWeek),
        PS_HLE_FUNC(0x7F1086E6, cellRtcCheckValid),
        PS_HLE_FUNC(0xFB51FC61, cellRtcCompareTick),
    });
}

} // namespace pspocket::hle
