// sys_libc part 5a (D-515, D-906): time.h as the SDK's C library has it, the "C" locale's ctype tables, and
// Dinkumware's number-conversion internals, under sys_libc and sys_libm. The locale objects are sys_libc_locale.cpp's.
// - time_t is 64 bits (the SDK's CellFsStat and CellFsUtimbuf hold time_t in 64-bit fields), counting seconds from
//   1970 (lv2's sys_time_get_current_time). Local time is lv2's: its time zone and summer-time offsets in minutes east
//   of UTC (sys_time_get_timezone), tm_isdst 1 while the summer offset is not 0.
// - The calendar, gmtime, localtime, asctime, ctime, strftime and their internals (_Ttotm, _Daysto, _CStrftime,
//   _Gentime, _Gettime) are the SDK's, read from a game's statically linked copy of its library (D-906): Plauger's
//   calendar (every fourth year a leap year but 1900), Dinkumware's table-driven strftime over the locale's _Tinfo
//   (the variable _Times) with Sony's choices (an unknown conversion gives its letter and a NUL, numbers clamped at 0
//   and cut to their width, %z and %Z from the locale's time_zone only, empty in the "C" locale, %V without leap
//   years). mktime, _Isdst, _Getdst, _Wcsftime and wcsftime have no copy in that game: Dinkumware's design, said so
//   where they are.
// - clock() counts microseconds from the first call (POSIX's CLOCKS_PER_SEC of 1000000; the SDK's value is not
//   known).
// - The ctype tables a header's macros may read: Dinkumware's (_Ctype, _Tolotab, _Touptab and the _Getp* functions:
//   257 shorts from EOF, its C99 class bits) and newlib's __ctype_ptr (257 chars from EOF). The variables hold the
//   pointer to entry 0 (Plauger's `const short *_Ctype = &ctyp_tab[1]`), the table after it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_locale.hpp"
#include "libc_malloc.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- calendar

// The SDK's calendar is Plauger's (xttotm.c, xdaysto.c in Dinkumware's library): years counted from 1900, every fourth
// year a leap year but 1900 itself (his "correct for leap year: 1801-2099"), so 2100 is a leap year here as in the
// SDK's gmtime. Divisions truncate towards zero as the PPU's do.
constexpr s16 kLmos[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
constexpr s16 kMos[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
constexpr s64 kBias1900 = 2208988800; // seconds from 1900 to 1970

bool sdk_leap(s32 year) { return (year & 3) == 0 && year != 0; }

// _Daysto: the leap days from 1900 to `year` and the days before month `mon` in it.
s32 daysto(s32 year, s32 mon)
{
    s32 days;
    if (0 < year) days = (year - 1) / 4;
    else if (year <= -4) days = 1 + (4 - year) / 4;
    else days = 0;
    const s16* tab = sdk_leap(year) ? kLmos : kMos;
    return days + (mon >= 0 && mon < 12 ? tab[mon] : 0);
}

struct Tm {
    s32 sec = 0, min = 0, hour = 0, mday = 1, mon = 0, year = 70, wday = 4, yday = 0, isdst = 0;
};

// The SDK's _Ttotm: `secs` from 1970 broken down; a non-zero isdst adds lv2's summer offset (`summer` seconds) first
// and is kept in tm_isdst as given.
Tm ttotm(s64 secs, s32 isdst, s64 summer)
{
    Tm t;
    s64 s = static_cast<s64>(static_cast<u64>(secs) + static_cast<u64>(kBias1900));
    t.isdst = isdst;
    if (isdst != 0) s = static_cast<s64>(static_cast<u64>(s) + static_cast<u64>(summer));
    const s64 days64 = s / 86400;
    const s32 days = static_cast<s32>(days64);
    t.wday = static_cast<s32>(static_cast<u32>(days) + 1u) % 7;
    s32 year = days / 365, start = 0;
    for (s32 base = year * 365;; base -= 365, year--) {
        start = base + daysto(year, 0);
        if (days >= start) break;
    }
    t.year = year;
    t.yday = days - start;
    const s16* pm = sdk_leap(year) ? kLmos : kMos;
    s32 mon = 12;
    while (t.yday < pm[--mon]) {}
    t.mon = mon;
    t.mday = t.yday - pm[mon] + 1;
    s64 rem = s - days64 * 86400;
    const s64 hour = rem / 3600;
    rem -= hour * 3600;
    const s64 min = rem / 60;
    t.hour = static_cast<s32>(hour);
    t.min = static_cast<s32>(min);
    t.sec = static_cast<s32>(rem - min * 60);
    return t;
}

Tm read_tm(u8* b, u32 a)
{
    Tm t;
    s32* f[] = {&t.sec, &t.min, &t.hour, &t.mday, &t.mon, &t.year, &t.wday, &t.yday, &t.isdst};
    for (u32 i = 0; i < 9; i++) *f[i] = static_cast<s32>(ps_read32(b, a + 4 * i));
    return t;
}

void write_tm(u8* b, u32 a, const Tm& t)
{
    const s32 f[] = {t.sec, t.min, t.hour, t.mday, t.mon, t.year, t.wday, t.yday, t.isdst};
    for (u32 i = 0; i < 9; i++) ps_write32(b, a + 4 * i, static_cast<u32>(f[i]));
}

// lv2's time zone and summer-time offsets (sys_time_get_timezone, minutes east of UTC), in seconds; the SDK
// multiplies the minutes in 32 bits.
struct Zone {
    s64 offset = 0, summer = 0;
};

s64 minutes_to_seconds(u32 minutes) { return static_cast<s32>(minutes * 60u); }

Zone zone(PPUContext& ctx)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar v(ctx, t.kernel.base(), 8);
    Zone z;
    if (lv2::sys_time_get_timezone(t, v.addr(), v.addr() + 4) == lv2::CELL_OK) {
        z.offset = minutes_to_seconds(ps_read32(t.kernel.base(), v.addr()));
        z.summer = minutes_to_seconds(ps_read32(t.kernel.base(), v.addr() + 4));
    }
    return z;
}

// time(): lv2's clock; -1 (stored too) when lv2 fails, as the SDK's.
u64 time_(PPUContext& ctx, u32 out)
{
    lv2::PpuThread& t = thread_of(ctx);
    s64 now = -1;
    {
        StackVar v(ctx, t.kernel.base(), 16);
        if (lv2::sys_time_get_current_time(t, v.addr(), v.addr() + 8) == lv2::CELL_OK)
            now = static_cast<s64>(ps_read64(t.kernel.base(), v.addr()));
    }
    if (out && range_ok(ctx, "time", out, 8)) ps_write64(gb(ctx), out, static_cast<u64>(now));
    return static_cast<u64>(now);
}

u32 tm_block(PPUContext& ctx)
{
    const u32 b = thread_block(ctx);
    return b ? b + tb::kTm : 0;
}

// ------------------------------------------------------------------------------------------------- time.h

struct ClockState {
    std::mutex mutex;
    bool started = false;
    u64 start = 0;
};

u64 clock_(PPUContext& ctx)
{
    ClockState& s = HleProcess::current()->state<ClockState>();
    const u64 us = lv2::sys_time_get_system_time(thread_of(ctx));
    std::lock_guard l(s.mutex);
    if (!s.started) {
        s.started = true;
        s.start = us;
    }
    return us - s.start;
}

double difftime_(PPUContext&, u64 t1, u64 t0)
{
    return static_cast<double>(static_cast<s64>(t1)) - static_cast<double>(static_cast<s64>(t0));
}

u32 put_tm(PPUContext& ctx, const Tm& t)
{
    const u32 out = tm_block(ctx);
    if (out) write_tm(gb(ctx), out, t);
    return out;
}

// gmtime: _Ttotm(NULL, *t, 0), the thread's struct tm.
u32 gmtime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "gmtime", tp, 8)) return 0;
    return put_tm(ctx, ttotm(static_cast<s64>(ps_read64(gb(ctx), tp)), 0, 0));
}

// localtime: lv2's zone added, then _Ttotm with tm_isdst 1 while lv2 has a summer offset (which _Ttotm adds).
u32 localtime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "localtime", tp, 8)) return 0;
    const Zone z = zone(ctx);
    const s64 t = static_cast<s64>(ps_read64(gb(ctx), tp));
    return put_tm(ctx, ttotm(static_cast<s64>(static_cast<u64>(t) + static_cast<u64>(z.offset)), z.summer ? 1 : 0,
                            z.summer));
}

// mktime (no copy in the game's library to read): Plauger's arithmetic (the months folded into the years, the days
// from the SDK's _Daysto) for the local time, lv2's zone taken off; tm_isdst > 0 takes the summer offset off too, < 0
// lv2's current choice. The fields come back normalized as the SDK's _Ttotm breaks the local time down.
u64 mktime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "mktime", tp, 36)) return ~u64{0};
    const Tm in = read_tm(gb(ctx), tp);
    s64 ymon = in.mon / 12, mon = in.mon - ymon * 12;
    if (mon < 0) {
        mon += 12;
        ymon--;
    }
    const s64 year = s64{in.year} + ymon;
    if (year < std::numeric_limits<s32>::min() || year > std::numeric_limits<s32>::max()) return ~u64{0};
    const double dsecs = 86400.0 * (daysto(static_cast<s32>(year), static_cast<s32>(mon)) - 1) +
                         31536000.0 * static_cast<double>(year) + 86400.0 * in.mday + 3600.0 * in.hour +
                         60.0 * in.min + static_cast<double>(in.sec);
    if (!(dsecs > -9.0e18 && dsecs < 9.0e18)) return ~u64{0};
    const s64 local = static_cast<s64>(dsecs) - kBias1900;
    const Zone z = zone(ctx);
    const bool dst = in.isdst > 0 || (in.isdst < 0 && z.summer);
    Tm out = ttotm(local, 0, 0);
    out.isdst = dst ? 1 : 0;
    write_tm(gb(ctx), tp, out);
    return static_cast<u64>(local - z.offset - (dst ? z.summer : 0));
}

// ------------------------------------------------------------------------------------------------- strftime

// The library's constant strings in guest memory (made once): the formats _Gentime hands back for %D %F %R %T, %n,
// %t and %%, the empty zone and era ("+:", the SDK's "no era"), asctime's "%c\n" and the "C" _Tinfo asctime formats
// with whatever the locale.
struct TimeConsts {
    u32 percent = 0, newline = 0, tab = 0, mdy = 0, ymd = 0, hm = 0, hms = 0, colon = 0, no_era = 0, ctime_fmt = 0,
        c_times = 0;
};

struct TimeState {
    std::mutex mutex;
    TimeConsts consts;
};

const TimeConsts* time_consts(PPUContext& ctx)
{
    TimeState& s = HleProcess::current()->state<TimeState>();
    std::lock_guard l(s.mutex);
    TimeConsts& c = s.consts;
    if (!c.percent) {
        const u32 a = HleProcess::current()->main_memory().alloc(0x200);
        if (!a) return nullptr;
        u8* b = gb(ctx);
        u32 at = a + ti::kSize;
        const auto put = [&](std::string_view text) {
            const u32 p = at;
            std::memcpy(b + at, text.data(), text.size());
            b[at + text.size()] = 0;
            at += static_cast<u32>(text.size()) + 1;
            return p;
        };
        c.c_times = a;
        at = lay_c_times(b, a, at);
        c.newline = put("\n");
        c.tab = put("\t");
        c.mdy = put("%m/%d/%y");
        c.ymd = put("%Y-%m-%d");
        c.hm = put("%H:%M");
        c.hms = put("%H:%M:%S");
        c.colon = put(":");
        c.no_era = put("+:");
        c.ctime_fmt = put("%c\n");
        c.percent = put("%");
    }
    return &c;
}

struct Field {
    u32 ptr = 0;
    s32 len = 0;
};

// A guest string whose reading failed stops the formatting (refused, logged).
struct Unreadable {};

std::string guest_text(PPUContext& ctx, u32 a)
{
    const auto t = read_string(thread_of(ctx), a, kMaxString);
    if (!t) {
        refuse(ctx, std::format("strftime: the string at {:#x} runs out of guest memory", a));
        throw Unreadable{};
    }
    return *t;
}

// _Gettime: field n of s, whose first character delimits its fields, and its length; n <= 0 is the first field, one
// past the last is the empty string at the end.
Field gettime(PPUContext& ctx, u32 s, s32 n)
{
    const std::string t = guest_text(ctx, s);
    usize at = 0;
    char delim = 0;
    if (!t.empty()) {
        delim = t[0];
        at = 1;
    }
    for (;;) {
        usize q = delim ? t.find(delim, at) : t.size();
        const bool found = q != std::string::npos;
        if (!found) q = t.size();
        if (n <= 0) return {s + static_cast<u32>(at), static_cast<s32>(q - at)};
        n--;
        if (!found || q == t.size()) return {s + static_cast<u32>(q), 0};
        at = q + 1;
    }
}

// strtol(s, &end, 10) as the library's (errno ERANGE beyond a long).
s32 strtol10(PPUContext& ctx, u32 s, u32* end)
{
    GuestBytes in(thread_of(ctx), s);
    const IntParse p = parse_int([&](u64 i) -> s64 { return in.at(i); }, 10);
    bool range = false;
    const u64 v = int_result(p, 32, true, &range);
    if (range) set_errno(ctx, kERANGE);
    if (end) *end = s + static_cast<u32>(p.end);
    return static_cast<s32>(static_cast<u32>(v));
}

u8 byte_at(PPUContext& ctx, u32 a)
{
    if (!guest_readable(thread_of(ctx), a, 1)) {
        refuse(ctx, std::format("strftime: {:#x} is not guest memory", a));
        throw Unreadable{};
    }
    return gb(ctx)[a];
}

u32 tin_field(PPUContext& ctx, u32 tin, u32 off) { return ps_read32(gb(ctx), tin + off); }

// The SDK's number field: `val` (0 when negative) in n digits, its low ones, into buf; with the O modifier the
// locale's alt_digits field of that number when it has one.
Field getval(PPUContext& ctx, u32 buf, s32 val, s32 n, char qual, u32 tin)
{
    if (val < 0) val = 0;
    if (qual == 'O') {
        const Field f = gettime(ctx, tin_field(ctx, tin, ti::kAltDigits), val);
        if (f.len > 0) return f;
    }
    u8* b = gb(ctx);
    b[buf + static_cast<u32>(n)] = 0;
    for (s32 i = n - 1; i >= 0; i--) {
        b[buf + static_cast<u32>(i)] = static_cast<u8>('0' + val % 10);
        val /= 10;
    }
    return {buf, n};
}

// The SDK's week arithmetic. Its leap-year test for these never answers true (its range tests on the year cannot
// pass), so %V, %G and %g count every year as 365 days; kept as found.
s32 week_leap(s32) { return 0; }

// The week of the year counting from the first `first` day (0 Sunday, 1 Monday), the days before it week 0.
s32 wkno(s32 first, s32 wday, s32 yday) { return (yday - (wday - first + 7) % 7 + 7) / 7; }

// The ISO 8601 week: 1..53, 0 for a day of the previous year's last week, -1 for one of the next year's first.
s32 isoweek(s32 year, s32 wday, s32 yday)
{
    const s32 leap = week_leap(year);
    const s32 jan1 = (wday - yday + 371) % 7;
    const s32 tail = yday - leap;
    const s32 next_jan1 = (jan1 + leap + 365) % 7;
    // The last days of the year in the next year's week 1: three of them when that year begins on a Thursday, two on
    // a Wednesday, one on a Tuesday.
    if (tail > 363) {
        if (next_jan1 == 2 || next_jan1 == 3 || next_jan1 == 4) return -1;
    } else if (tail > 362) {
        if (next_jan1 == 3 || next_jan1 == 4) return -1;
    } else if (tail > 361) {
        if (next_jan1 == 4) return -1;
    }
    s32 w = wkno(1, wday, yday);
    if (static_cast<u32>(jan1 - 2) <= 2) w++;
    return w;
}

s32 isoyear(s32 year, s32 wday, s32 yday)
{
    const s32 w = isoweek(year, wday, yday);
    return w == 0 ? year - 1 : w > 0 ? year : year + 1;
}

// -1, 0 or 1 as the date of t comes before, on or after "yyyy/mm/dd" at s; 2 when s is no date.
s32 cmpdate(PPUContext& ctx, const Tm& t, u32 s)
{
    u32 e = 0;
    const s32 y = strtol10(ctx, s, &e);
    if (e == s || byte_at(ctx, e) != '/') return 2;
    const s32 ty = t.year + 1900;
    bool before = ty < y;
    if (y == ty) {
        const u32 s2 = e + 1;
        const s32 m = strtol10(ctx, s2, &e);
        if (e == s2 || byte_at(ctx, e) != '/') return 2;
        before = t.mon + 1 < m;
        if (m == t.mon + 1) {
            const u32 s3 = e + 1;
            const s32 d = strtol10(ctx, s3, &e);
            if (e == s3) return 2;
            before = t.mday < d;
            if (d == t.mday) return 0;
        }
    }
    return before ? -1 : 1;
}

// The era of t among the locale's era entries ("+:offset:start:end:name:format", '+' counting up from the start,
// '-' down), or "+:" (none).
u32 era_of(PPUContext& ctx, const Tm& t, u32 tin, const TimeConsts& c)
{
    const u32 eras = tin_field(ctx, tin, ti::kEra);
    for (s32 i = 0;; i++) {
        const u32 p = gettime(ctx, eras, i).ptr;
        const u8 dir = byte_at(ctx, p);
        if (dir == 0) return c.no_era;
        const u32 q = p + 1;
        const s32 c1 = cmpdate(ctx, t, gettime(ctx, q, 1).ptr);
        bool in = false;
        if (dir == '-') in = static_cast<u32>(c1 + 1) <= 1;
        else if (dir == '+') in = static_cast<u32>(c1) <= 1;
        if (!in) continue;
        const u32 end = gettime(ctx, q, 2).ptr;
        if (byte_at(ctx, end) == dir && byte_at(ctx, end + 1) == '*') return p;
        const s32 c2 = cmpdate(ctx, t, end);
        if (dir == '-' && static_cast<u32>(c2) <= 1) return p;
        if (dir == '+' && static_cast<u32>(c2 + 1) <= 1) return p;
    }
}

// Dinkumware's _Gentime with the SDK's choices: the text of one conversion (qual 'E', 'O' or 0, code its letter) for
// t, from the _Tinfo at tin, in buf (32 bytes) or the locale's strings; a negative length is a format to expand. An
// unknown conversion is its letter and a NUL.
Field gentime(PPUContext& ctx, const Tm& t, u32 tin, char qual, char code, u32 buf, const TimeConsts& c)
{
    u8* b = gb(ctx);
    const auto names = [&](u32 off, s32 n) { return gettime(ctx, tin_field(ctx, tin, off), n); };
    const auto expand = [&](u32 off, u32 era_off, s32 n) {
        Field f = gettime(ctx, tin_field(ctx, tin, qual == 'E' ? era_off : off), n);
        f.len = -f.len;
        return f;
    };
    const auto zone_text = [&]() {
        const u32 z = tin_field(ctx, tin, ti::kTzone);
        return byte_at(ctx, z) ? z : c.colon;
    };
    const auto year2 = [&](s32 y) {
        s32 r = y % 100;
        return r < 0 ? r + 100 : r;
    };
    switch (code) {
    case 'a': return names(ti::kAbday, static_cast<s32>(static_cast<u32>(t.wday) * 2));
    case 'A': return names(ti::kDay, static_cast<s32>(static_cast<u32>(t.wday) * 2 + 1));
    case 'b':
    case 'h': return names(ti::kAbmon, static_cast<s32>(static_cast<u32>(t.mon) * 2));
    case 'B': return names(ti::kMon, static_cast<s32>(static_cast<u32>(t.mon) * 2 + 1));
    case 'c': return expand(ti::kDTFmt, ti::kEraDTFmt, 0);
    case 'x': return expand(ti::kDFmt, ti::kEraDFmt, 1);
    case 'X': return expand(ti::kTFmt, ti::kEraTFmt, 2);
    case 'r': return expand(ti::kTFmtAmpm, ti::kEraTFmtAmpm, 3);
    case 'C':
        if (qual == 'E') {
            const Field f = gettime(ctx, era_of(ctx, t, tin, c) + 1, 3);
            if (byte_at(ctx, f.ptr)) return f;
        }
        return getval(ctx, buf, t.year / 100 + 19, 2, qual, tin);
    case 'd': return getval(ctx, buf, t.mday, 2, qual, tin);
    case 'e': {
        const Field f = getval(ctx, buf, t.mday, 2, qual, tin);
        if (b[buf] == '0') b[buf] = ' ';
        return f;
    }
    case 'D': return {c.mdy, -8};
    case 'F': return {c.ymd, -8};
    case 'R': return {c.hm, -5};
    case 'T': return {c.hms, -8};
    case 'n': return {c.newline, 1};
    case 't': return {c.tab, 1};
    case '%': return {c.percent, 1};
    case 'g': return getval(ctx, buf, year2(isoyear(t.year, t.wday, t.yday)), 2, qual, tin);
    case 'G': return getval(ctx, buf, isoyear(t.year, t.wday, t.yday) + 1900, 4, qual, tin);
    case 'H': return getval(ctx, buf, t.hour, 2, qual, tin);
    case 'I': return getval(ctx, buf, (t.hour + 11) % 12 + 1, 2, qual, tin);
    case 'j': return getval(ctx, buf, t.yday + 1, 3, qual, tin);
    case 'm': return getval(ctx, buf, t.mon + 1, 2, qual, tin);
    case 'M': return getval(ctx, buf, t.min, 2, qual, tin);
    case 'S': return getval(ctx, buf, t.sec, 2, qual, tin);
    case 'p': return names(ti::kAmPm, t.hour > 11 ? 1 : 0);
    case 'u': return getval(ctx, buf, t.wday == 0 ? 7 : t.wday, 1, qual, tin);
    case 'w': return getval(ctx, buf, t.wday, 1, qual, tin);
    case 'U': return getval(ctx, buf, wkno(0, t.wday, t.yday), 2, qual, tin);
    case 'W': return getval(ctx, buf, wkno(1, t.wday, t.yday), 2, qual, tin);
    case 'V': {
        s32 w = isoweek(t.year, t.wday, t.yday);
        if (w == 0) w = isoweek(t.year - 1, t.wday - t.yday + 7, week_leap(t.year - 1) ? 366 : 365);
        else if (w < 0) w = 1;
        return getval(ctx, buf, w, 2, qual, tin);
    }
    case 'y': {
        const s32 yy = year2(t.year);
        if (qual == 'E') {
            const u32 era = era_of(ctx, t, tin, c);
            if (byte_at(ctx, era + 2)) {
                const s32 y0 = strtol10(ctx, gettime(ctx, era + 1, 1).ptr, nullptr);
                s32 years = t.year - y0 + 1900;
                if (byte_at(ctx, era) == '-') years = -years;
                u32 end = 0;
                const s32 offset = strtol10(ctx, era + 2, &end);
                if (end != era + 2 && byte_at(ctx, end) == byte_at(ctx, era + 1)) {
                    const s32 v = years + offset;
                    s32 digits = 1;
                    for (s32 q = v / 10; q > 0; q /= 10) digits++;
                    return getval(ctx, buf, v, digits, qual, tin);
                }
            }
        }
        return getval(ctx, buf, yy, 2, qual, tin);
    }
    case 'Y':
        if (qual == 'E') {
            Field f = gettime(ctx, era_of(ctx, t, tin, c) + 1, 4);
            if (byte_at(ctx, f.ptr) != ';' && f.len != 0) {
                const std::string rest = guest_text(ctx, f.ptr);
                if (const usize semi = rest.find(';'); semi != std::string::npos) f.len = static_cast<s32>(semi);
                f.len = -f.len;
                return f;
            }
        }
        return getval(ctx, buf, t.year + 1900, 4, qual, tin);
    case 'Z': return gettime(ctx, zone_text(), t.isdst > 0 ? 1 : 0);
    case 'z': {
        const Field f = gettime(ctx, zone_text(), 2);
        if (f.len <= 0 || t.isdst <= 0) return f;
        s32 v = strtol10(ctx, f.ptr, nullptr) + 100;
        b[buf] = '+';
        if (v < 0) {
            b[buf] = '-';
            v = -v;
        }
        Field r = getval(ctx, buf + 1, v, 4, qual, tin);
        r.ptr -= 1;
        r.len += 1;
        return r;
    }
    default:
        b[buf] = static_cast<u8>(code);
        b[buf + 1] = 0;
        return {buf, 2};
    }
}

// The conversion's letter at `p` (after a '%'), with an E or O modifier.
struct Conversion {
    char qual = 0, code = 0;
    u32 taken = 1;
};

// Dinkumware's _CStrftime: fmt (len bytes) formatted for t from the _Tinfo at tin, the pieces that fit appended to
// out; false when the text (and its NUL) does not fit in max. The format is read as multibyte characters (the thread's
// state tables; a byte that is no character stands for itself) and a '%' with something after it starts a conversion;
// a conversion that is a format (%c %x %X %r %D %F %R %T, the era's %EY) is expanded in place, three deep at most (the
// rest of a format whose expansion finds no room is dropped, as the SDK's does).
bool cstrftime_run(PPUContext& ctx, std::string& out, u32 max, u32 fmt, const Tm& t, u32 tin, u32 buf)
{
    const TimeConsts* c = time_consts(ctx);
    if (!c) return false;
    struct Src {
        u32 p = 0, len = 0;
    };
    Src cur{fmt, static_cast<u32>(guest_text(ctx, fmt).size())};
    Src stack[3];
    u32 depth = 0;
    MbState st;
    u32 wc = 0, room = max, expansions = 0;
    const u32 tables = mb_tables(ctx);
    lv2::PpuThread& th = thread_of(ctx);
    while (cur.len || depth) {
        if (!cur.len) cur = stack[--depth];
        GuestBytes bytes(th, cur.p);
        bool unreadable = false;
        const MbStep r = mbtowc_step(ctx, [&](u64 i) { return bytes.at(i); }, cur.len, st, tables, &unreadable);
        if (unreadable) {
            refuse(ctx, std::format("strftime: the format at {:#x} runs out of guest memory", cur.p));
            throw Unreadable{};
        }
        if (r.have) wc = r.wc;
        u32 m = r.result;
        if (static_cast<s32>(m) <= 0) m = byte_at(ctx, cur.p) ? 1 : 0;
        if (m == 0) {
            cur.len = 0; // a NUL inside a counted format (only after a modifier that ended it): nothing more
            continue;
        }
        cur.p += m;
        cur.len -= m;
        if (wc == '%' && cur.len != 0) {
            char qual = 0, code = static_cast<char>(byte_at(ctx, cur.p));
            if (code == 'E' || code == 'O') {
                qual = code;
                code = static_cast<char>(byte_at(ctx, cur.p + 1));
                cur.p += 2;
                cur.len = cur.len >= 2 ? cur.len - 2 : 0; // the SDK's reads on past the end of the format here
            } else {
                cur.p += 1;
                cur.len -= 1;
            }
            const Field f = gentime(ctx, t, tin, qual, code, buf, *c);
            if (f.len > 0) {
                const u32 n = static_cast<u32>(f.len);
                if (room < n) return false;
                if (!guest_readable(th, f.ptr, n)) {
                    refuse(ctx, std::format("strftime: [{:#x}, +{:#x}) is not guest memory", f.ptr, n));
                    throw Unreadable{};
                }
                out.append(reinterpret_cast<const char*>(gb(ctx) + f.ptr), n);
                room -= n;
            } else {
                // A locale whose formats expand into themselves (an era format "%EY") makes the SDK's loop for ever.
                if (++expansions > 100000) {
                    refuse(ctx, "strftime: the locale's formats expand without end (the SDK's would loop for ever)");
                    throw Unreadable{};
                }
                if (cur.len != 0 && depth <= 2) stack[depth++] = cur;
                cur = {f.ptr, static_cast<u32>(-f.len)};
            }
        } else {
            if (room < m) return false;
            if (!guest_readable(th, cur.p - m, m)) throw Unreadable{};
            out.append(reinterpret_cast<const char*>(gb(ctx) + cur.p - m), m);
            room -= m;
        }
    }
    return room != 0;
}

// _CStrftime(char *s, size_t max, const char *fmt, const struct tm *t, const _Tinfo *tin): the length of the text, or
// 0 when it and its NUL do not fit (the pieces that did are left in s, as the SDK's leaves them).
u32 cstrftime(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp, u32 tin)
{
    if (!range_ok(ctx, "strftime", tp, 36) || !range_ok(ctx, "strftime", tin, ti::kSize)) return 0;
    StackVar buf(ctx, gb(ctx), 32);
    std::string out;
    bool fits = false;
    try {
        fits = cstrftime_run(ctx, out, max, fmt, read_tm(gb(ctx), tp), tin, buf.addr());
    } catch (const Unreadable&) {
        return 0;
    }
    if (!range_ok(ctx, "strftime", s, out.size() + (fits ? 1 : 0))) return 0;
    std::memcpy(gb(ctx) + s, out.data(), out.size());
    if (!fits) return 0;
    gb(ctx)[s + out.size()] = 0;
    return static_cast<u32>(out.size());
}

u32 times_var(PPUContext&) { return variable(recomp::nid_of("_Times")); }

u32 _CStrftime_(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp, u32 tin) { return cstrftime(ctx, s, max, fmt, tp, tin); }
u32 strftime_(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp) { return cstrftime(ctx, s, max, fmt, tp, times_var(ctx)); }

// _Gentime(const struct tm *t, const _Tinfo *tin, char qual, char code, int *plen, char *buf): the conversion's text
// (in buf or the locale's strings) and its length in *plen, negative for a format to expand.
u32 _Gentime_(PPUContext& ctx, u32 tp, u32 tin, s32 qual, s32 code, u32 plen, u32 buf)
{
    const TimeConsts* c = time_consts(ctx);
    if (!c || !range_ok(ctx, "_Gentime", tp, 36) || !range_ok(ctx, "_Gentime", tin, ti::kSize) ||
        !range_ok(ctx, "_Gentime", plen, 4) || !range_ok(ctx, "_Gentime", buf, 12))
        return 0;
    try {
        const Field f = gentime(ctx, read_tm(gb(ctx), tp), tin, static_cast<char>(qual), static_cast<char>(code), buf, *c);
        ps_write32(gb(ctx), plen, static_cast<u32>(f.len));
        return f.ptr;
    } catch (const Unreadable&) {
        return 0;
    }
}

// _Gettime(const char *s, int n, int *len).
u32 _Gettime_(PPUContext& ctx, u32 s, s32 n, u32 len)
{
    if (!range_ok(ctx, "_Gettime", len, 4)) return 0;
    try {
        const Field f = gettime(ctx, s, n);
        ps_write32(gb(ctx), len, static_cast<u32>(f.len));
        return f.ptr;
    } catch (const Unreadable&) {
        return 0;
    }
}

// _Getptimes: the current locale's _Tinfo, the variable _Times.
u32 _Getptimes_(PPUContext& ctx) { return times_var(ctx); }

// _Wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *t, const _Tinfo *tin) has no copy in the
// game's library (its C++ library formats through strftime): derived from _CStrftime with sys_libc's 32-bit wchar_t.
// The wide format's characters are taken one by one ('%' and a modifier and letter, the letter's low byte, as the
// narrow conversion takes a char); a conversion's narrow text and the formats it expands are read as multibyte
// characters (a byte that is none stands for itself, a NUL inside a text is a NUL character) and delivered as wide
// characters.
bool wcsftime_run(PPUContext& ctx, std::u32string& out, u32 max, u32 fmt, const Tm& t, u32 tin, u32 buf)
{
    const TimeConsts* c = time_consts(ctx);
    if (!c) return false;
    const auto f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("wcsftime: the format at {:#x} runs out of guest memory", fmt));
        throw Unreadable{};
    }
    struct Src {
        u32 p = 0, len = 0;
    };
    u32 room = max, expansions = 0;
    const u32 tables = mb_tables(ctx);
    lv2::PpuThread& th = thread_of(ctx);
    const auto put = [&](char32_t w) {
        if (room == 0) return false;
        out += w;
        room--;
        return true;
    };
    // The narrow text [p, p + n) delivered as wide characters.
    const auto widen = [&](u32 p, u32 n) {
        MbState st;
        while (n) {
            GuestBytes bytes(th, p);
            bool unreadable = false;
            const MbStep r = mbtowc_step(ctx, [&](u64 i) { return bytes.at(i); }, n, st, tables, &unreadable);
            if (unreadable) throw Unreadable{};
            u32 m = r.result;
            char32_t w = r.wc;
            if (static_cast<s32>(m) <= 0 || !r.have) {
                w = byte_at(ctx, p);
                m = 1;
            }
            if (!put(w)) return false;
            p += m;
            n -= m;
        }
        return true;
    };
    // A narrow format expanded (as _CStrftime does, three deep).
    const auto narrow = [&](auto&& self, u32 p, u32 n, u32 depth) -> bool {
        MbState st;
        u32 wc = 0;
        while (n) {
            GuestBytes bytes(th, p);
            bool unreadable = false;
            const MbStep r = mbtowc_step(ctx, [&](u64 i) { return bytes.at(i); }, n, st, tables, &unreadable);
            if (unreadable) throw Unreadable{};
            if (r.have) wc = r.wc;
            u32 m = r.result;
            if (static_cast<s32>(m) <= 0) m = byte_at(ctx, p) ? 1 : 0;
            if (m == 0) break;
            p += m;
            n -= m;
            if (wc == '%' && n != 0) {
                char qual = 0, code = static_cast<char>(byte_at(ctx, p));
                if (code == 'E' || code == 'O') {
                    qual = code;
                    code = static_cast<char>(byte_at(ctx, p + 1));
                    p += 2;
                    n = n >= 2 ? n - 2 : 0;
                } else {
                    p++;
                    n--;
                }
                const Field g = gentime(ctx, t, tin, qual, code, buf, *c);
                if (g.len > 0) {
                    if (!widen(g.ptr, static_cast<u32>(g.len))) return false;
                } else if (depth < 3) {
                    if (!self(self, g.ptr, static_cast<u32>(-g.len), depth + 1)) return false;
                } else {
                    if (++expansions > 100000) {
                        refuse(ctx, "wcsftime: the locale's formats expand without end");
                        throw Unreadable{};
                    }
                    p = g.ptr;
                    n = static_cast<u32>(-g.len);
                }
            } else if (!widen(p - m, m)) {
                return false;
            }
        }
        return true;
    };
    for (usize i = 0; i < f->size(); i++) {
        const char32_t w = (*f)[i];
        if (w != U'%' || i + 1 >= f->size()) {
            if (!put(w)) return false;
            continue;
        }
        char qual = 0, code = static_cast<char>((*f)[++i]);
        if (code == 'E' || code == 'O') {
            qual = code;
            code = i + 1 < f->size() ? static_cast<char>((*f)[++i]) : 0;
        }
        const Field g = gentime(ctx, t, tin, qual, code, buf, *c);
        if (g.len > 0) {
            if (!widen(g.ptr, static_cast<u32>(g.len))) return false;
        } else if (!narrow(narrow, g.ptr, static_cast<u32>(-g.len), 1)) {
            return false;
        }
    }
    return room != 0;
}

u32 wcsftime_common(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp, u32 tin)
{
    if (!range_ok(ctx, "wcsftime", tp, 36) || !range_ok(ctx, "wcsftime", tin, ti::kSize)) return 0;
    StackVar buf(ctx, gb(ctx), 32);
    std::u32string out;
    bool fits = false;
    try {
        fits = wcsftime_run(ctx, out, max, fmt, read_tm(gb(ctx), tp), tin, buf.addr());
    } catch (const Unreadable&) {
        return 0;
    }
    if (!write_wstring(thread_of(ctx), s, out, fits)) {
        refuse(ctx, std::format("wcsftime: the array at {:#x} is not guest memory", s));
        return 0;
    }
    return fits ? static_cast<u32>(out.size()) : 0;
}

u32 _Wcsftime_(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp, u32 tin) { return wcsftime_common(ctx, s, max, fmt, tp, tin); }
u32 wcsftime_(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp) { return wcsftime_common(ctx, s, max, fmt, tp, times_var(ctx)); }

// asctime: _CStrftime(the thread's 26 bytes, 26, "%c\n", t, the "C" _Tinfo), the buffer answered whatever it gave.
u32 asctime_(PPUContext& ctx, u32 tp)
{
    const TimeConsts* c = time_consts(ctx);
    const u32 b = thread_block(ctx);
    if (!c || !b || !tp) return 0;
    cstrftime(ctx, b + tb::kAsctime, 26, c->ctime_fmt, tp, c->c_times);
    return b + tb::kAsctime;
}

u32 ctime_(PPUContext& ctx, u32 tp)
{
    const u32 t = localtime_(ctx, tp);
    return t ? asctime_(ctx, t) : 0;
}

// Dinkumware's time internals.
s32 _Daysto_(PPUContext&, s32 year, s32 mon) { return daysto(year, mon); }

// struct tm *_Ttotm(struct tm *t, time_t secs, int isdst): the SDK's (above); the thread's tm when t is NULL.
u32 _Ttotm_(PPUContext& ctx, u32 tp, u64 secs, s32 isdst)
{
    const u32 out = tp ? tp : tm_block(ctx);
    if (!out || !range_ok(ctx, "_Ttotm", out, 36)) return 0;
    write_tm(gb(ctx), out, ttotm(static_cast<s64>(secs), isdst, isdst ? zone(ctx).summer : 0));
    return out;
}

// Dinkumware's summer-time rules (Plauger's xgetdst.c, the SDK's library has no copy: its localtime asks lv2):
// "<delim>[(yyyy)]mmdd[hh][+w|-w]<delim>..." in Dstrule records of five bytes {wday, hour, day, mon, year}, malloc'd,
// a wday of 255 ending them (its year the last rule's). wday 0 is the date itself; "+w" the first weekday w (0 Sunday)
// on or after it (w + 7, Sunday 14), "-w" the last one before it (w, Sunday 7).
u32 getdst(PPUContext& ctx, const std::string& s)
{
    if (s.empty()) return 0;
    const char delim = s[0];
    usize i = 1;
    u32 count = 2;
    for (usize k = 1; (k = s.find(delim, k)) != std::string::npos; k++) count++;
    const u32 rules = lib_malloc(ctx, 5 * count);
    if (!rules) return 0;
    u8* b = gb(ctx);
    const auto getint = [&](usize at, s32 n) {
        s32 value = 0;
        for (; 0 <= --n && at < s.size() && s[at] >= '0' && s[at] <= '9'; at++) value = value * 10 + s[at] - '0';
        return 0 <= n ? -1 : value;
    };
    const auto ch = [&](usize at) { return at < s.size() ? s[at] : '\0'; };
    s32 year = 0;
    for (u32 r = rules;; r += 5, i++) {
        if (ch(i) == '(') {
            year = getint(i + 1, 4) - 1900;
            if (year < 0 || ch(i + 5) != ')') break;
            i += 6;
        }
        const s32 mon = getint(i, 2) - 1;
        const s32 day = getint(i + 2, 2) - 1;
        i += 4;
        s32 hour = 0;
        if (ch(i) >= '0' && ch(i) <= '9') {
            hour = getint(i, 2);
            i += 2;
        }
        if (mon < 0 || 12 <= mon || day < 0 || 99 < day || hour < 0 || 99 < hour) break;
        s32 wday = 0;
        if (ch(i) == '+' || ch(i) == '-') {
            if (ch(i + 1) < '0' || '6' < ch(i + 1)) break;
            wday = ch(i + 1) == '0' ? 7 : ch(i + 1) - '0';
            if (ch(i) == '+') wday += 7;
            i += 2;
        }
        b[r] = static_cast<u8>(wday);
        b[r + 1] = static_cast<u8>(hour);
        b[r + 2] = static_cast<u8>(day);
        b[r + 3] = static_cast<u8>(mon);
        b[r + 4] = static_cast<u8>(year);
        if (ch(i) == '\0') {
            b[r + 5] = 0xFF;
            b[r + 9] = static_cast<u8>(year);
            return rules;
        }
        if (ch(i) != delim) break;
    }
    lib_free(ctx, rules, "_Getdst");
    return 0;
}

u32 _Getdst_(PPUContext& ctx, u32 s)
{
    const auto t = read_string(thread_of(ctx), s);
    if (!t) {
        refuse(ctx, std::format("_Getdst: the rules at {:#x} run out of guest memory", s));
        return 0;
    }
    return getdst(ctx, *t);
}

// int _Isdst(const struct tm *t): Plauger's xisdst.c over the current locale's dst_rules (or, when empty, the fourth
// field of its time_zone); with no rules (the "C" locale), lv2's summer time as the SDK's localtime has it.
s32 _Isdst_(PPUContext& ctx, u32 tp)
{
    if (!range_ok(ctx, "_Isdst", tp, 36)) return -1;
    const Tm t = read_tm(gb(ctx), tp);
    const u32 tin = times_var(ctx);
    std::string rules;
    if (tin) {
        if (const auto r = read_string(thread_of(ctx), tin_field(ctx, tin, ti::kDstRules))) rules = *r;
        if (rules.empty()) {
            const u32 z = tin_field(ctx, tin, ti::kTzone);
            if (const auto zt = read_string(thread_of(ctx), z); zt && !zt->empty()) {
                try {
                    const Field f = gettime(ctx, z, 3);
                    if (byte_at(ctx, f.ptr)) {
                        if (const auto rest = read_string(thread_of(ctx), f.ptr - 1)) rules = *rest;
                    }
                } catch (const Unreadable&) {
                    rules.clear();
                }
            }
        }
    }
    if (rules.empty()) return zone(ctx).summer ? 1 : 0;
    const u32 pr0 = getdst(ctx, rules);
    if (!pr0) return -1;
    const u8* b = gb(ctx);
    const s32 d0 = daysto(t.year, 0);
    const s32 hour = t.hour + 24 * t.yday;
    const s32 wd0 = static_cast<s32>((365 * s64{t.year} + d0 + 1) % 7) + 14;
    s32 ans = 0;
    for (u32 pr = pr0; b[pr] != 0xFF; pr += 5) {
        if (b[pr + 4] > t.year) continue;
        s32 rday = daysto(t.year, b[pr + 3]) - d0 + b[pr + 2];
        if (0 < b[pr]) {
            const s32 wd = (rday + wd0 - b[pr]) % 7;
            rday += wd == 0 ? 0 : 7 - wd;
            if (b[pr] <= 7) rday -= 7;
        }
        if (hour < rday * 24 + b[pr + 1]) break;
        ans = b[pr + 4] == b[pr + 9] ? !ans : 0;
    }
    lib_free(ctx, pr0, "_Isdst");
    return ans;
}

// ------------------------------------------------------------------------------------------------- ctype tables

// Dinkumware's C99 class bits.
constexpr u16 kXB = 0x400, kXD = 0x01, kUP = 0x02, kSP = 0x04, kPU = 0x08, kLO = 0x10, kDI = 0x20, kCN = 0x40,
              kBB = 0x80;

u16 dinkum_class(s32 c)
{
    if (c < 0 || c > 127) return 0;
    if (c == '\t') return kCN | kBB | kXB; // the SDK's table marks the space controls with both bits
    if (c >= '\n' && c <= '\r') return kCN | kBB;
    if (c < 0x20 || c == 0x7F) return kBB;
    if (c == ' ') return kSP;
    if (c >= '0' && c <= '9') return kDI | kXD;
    if (c >= 'A' && c <= 'Z') return kUP | (c <= 'F' ? kXD : 0);
    if (c >= 'a' && c <= 'z') return kLO | (c <= 'f' ? kXD : 0);
    return kPU;
}

// newlib's bits (__ctype_ptr): _U 01, _L 02, _N 04, _S 010, _P 020, _C 040, _X 0100, _B 0200.
u8 newlib_class(s32 c)
{
    if (c < 0 || c > 127) return 0;
    u8 r = 0;
    if (c >= 'A' && c <= 'Z') r |= 001;
    if (c >= 'a' && c <= 'z') r |= 002;
    if (c >= '0' && c <= '9') r |= 004;
    if (c == ' ' || (c >= '\t' && c <= '\r')) r |= 010;
    if (c > 0x20 && c < 0x7F && !(r & 007)) r |= 020;
    if (c < 0x20 || c == 0x7F) r |= 040;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) r |= 0100;
    if (c == ' ') r |= 0200;
    return r;
}

// The variables: the pointer to entry 0 (+0), then the 257 entries from EOF (+8).
void ctype_init(u8* b, u32 a)
{
    ps_write32(b, a, a + 8 + 2);
    for (s32 c = -1; c < 256; c++) ps_write16(b, a + 8 + 2 * static_cast<u32>(c + 1), dinkum_class(c));
}

void tolower_init(u8* b, u32 a)
{
    ps_write32(b, a, a + 8 + 2);
    for (s32 c = -1; c < 256; c++)
        ps_write16(b, a + 8 + 2 * static_cast<u32>(c + 1),
            static_cast<u16>(static_cast<s16>(c >= 'A' && c <= 'Z' ? c + 32 : c)));
}

void toupper_init(u8* b, u32 a)
{
    ps_write32(b, a, a + 8 + 2);
    for (s32 c = -1; c < 256; c++)
        ps_write16(b, a + 8 + 2 * static_cast<u32>(c + 1),
            static_cast<u16>(static_cast<s16>(c >= 'a' && c <= 'z' ? c - 32 : c)));
}

void newlib_ctype_init(u8* b, u32 a)
{
    ps_write32(b, a, a + 8 + 1);
    for (s32 c = -1; c < 256; c++) b[a + 8 + static_cast<u32>(c + 1)] = newlib_class(c);
}

constexpr u32 kTableBytes = 8 + 2 * 257 + 6;

u32 table_of(PPUContext& ctx, const char* name)
{
    const u32 v = variable(recomp::nid_of(name));
    return v ? ps_read32(gb(ctx), v) : 0;
}

u32 _Getpctype_(PPUContext& ctx) { return table_of(ctx, "_Ctype"); }
u32 _Getptolower_(PPUContext& ctx) { return table_of(ctx, "_Tolotab"); }
u32 _Getptoupper_(PPUContext& ctx) { return table_of(ctx, "_Touptab"); }

// ------------------------------------------------------------------------------------------------- conversions

u64 to_int(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 base, u32 bits, bool is_signed, u32 perr, bool wide)
{
    if (base < 0 || base == 1 || base > 36) {
        if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(gb(ctx), perr, 1);
        else set_errno(ctx, kEINVAL);
        if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(gb(ctx), endp, s);
        return 0;
    }
    IntParse p;
    if (wide) {
        GuestWords in(thread_of(ctx), s);
        p = parse_int([&](u64 i) -> s64 { const s64 c = in.at(i); return c > 0x7F ? 0x7F : c; }, base);
    } else {
        GuestBytes in(thread_of(ctx), s);
        p = parse_int([&](u64 i) -> s64 { return in.at(i); }, base);
    }
    bool range = false;
    const u64 v = int_result(p, bits, is_signed, &range);
    if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(gb(ctx), perr, range ? 1 : 0);
    else if (range) set_errno(ctx, kERANGE);
    const u32 unit = wide ? 4 : 1;
    if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(gb(ctx), endp, p.end ? s + unit * static_cast<u32>(p.end) : s);
    return v;
}

// _Stoul(s, endptr, base) and the rest: strtoul and its kin; the x forms report a range error in *perr (0 or 1)
// instead of errno.
u32 _Stoul_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(to_int(ctx, "_Stoul", s, e, b, 32, false, 0, false)); }
u64 _Stoull_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "_Stoull", s, e, b, 64, false, 0, false); }
u64 _Stoll_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "_Stoll", s, e, b, 64, true, 0, false); }
u32 _Stolx_(PPUContext& ctx, u32 s, u32 e, s32 b, u32 perr) { return static_cast<u32>(to_int(ctx, "_Stolx", s, e, b, 32, true, perr, false)); }
u32 _Stoulx_(PPUContext& ctx, u32 s, u32 e, s32 b, u32 perr) { return static_cast<u32>(to_int(ctx, "_Stoulx", s, e, b, 32, false, perr, false)); }
u64 _Stollx_(PPUContext& ctx, u32 s, u32 e, s32 b, u32 perr) { return to_int(ctx, "_Stollx", s, e, b, 64, true, perr, false); }
u64 _Stoullx_(PPUContext& ctx, u32 s, u32 e, s32 b, u32 perr) { return to_int(ctx, "_Stoullx", s, e, b, 64, false, perr, false); }
u64 _WStoll_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "_WStoll", s, e, b, 64, true, 0, true); }
u32 _WStoul_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(to_int(ctx, "_WStoul", s, e, b, 32, false, 0, true)); }
u64 _WStoull_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "_WStoull", s, e, b, 64, false, 0, true); }

// _Stod(s, endptr, pten): strtod scaled by 10^pten, rounded once (the decimal exponent moved by pten before the
// conversion); the x forms report a range error in *perr. Hex, inf and nan are not scaled exactly (multiplied).
FloatParse parse_scaled(const std::string& text, s32 pten)
{
    FloatParse p = parse_float(text);
    if (!pten || !p.end) return p;
    std::string sub = text.substr(0, p.end);
    const usize start = sub.find_first_not_of(" \t\n\v\f\r");
    const bool hex = start != std::string::npos && sub.size() > start + 2 &&
                     (sub.compare(start, 2, "0x") == 0 || sub.compare(start, 2, "0X") == 0 ||
                         sub.compare(start + 1, 2, "0x") == 0 || sub.compare(start + 1, 2, "0X") == 0);
    if (!hex && std::isfinite(p.d)) {
        s64 exp = pten;
        const usize e = sub.find_first_of("eE");
        if (e != std::string::npos) {
            exp += std::strtoll(sub.c_str() + e + 1, nullptr, 10);
            sub.resize(e);
        }
        const usize end = p.end;
        p = parse_float(sub + "e" + std::to_string(exp));
        p.end = end;
        return p;
    }
    p.d *= std::pow(10.0, pten);
    p.f = static_cast<float>(p.d);
    return p;
}

FloatParse stod_parse(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 pten, bool wide)
{
    std::string text;
    u32 unit = 1;
    if (wide) {
        unit = 4;
        if (const auto w = read_wstring(thread_of(ctx), s, 0x10000)) {
            for (const char32_t c : *w) {
                if (c > 0x7F) break;
                text += static_cast<char>(c);
            }
        }
    } else if (const auto t = read_string(thread_of(ctx), s, 0x10000)) {
        text = *t;
    }
    const FloatParse p = parse_scaled(text, pten);
    if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(gb(ctx), endp, s + unit * static_cast<u32>(p.end));
    return p;
}

double stod_common(PPUContext& ctx, const char* fn, u32 s, u32 e, s32 pten, u32 perr, bool wide, bool single)
{
    const FloatParse p = stod_parse(ctx, fn, s, e, pten, wide);
    const bool range = single ? p.range_f : p.range_d;
    if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(gb(ctx), perr, range ? 1 : 0);
    else if (range) set_errno(ctx, kERANGE);
    return single ? static_cast<double>(p.f) : p.d;
}

double _Stod_(PPUContext& ctx, u32 s, u32 e, s32 pten) { return stod_common(ctx, "_Stod", s, e, pten, 0, false, false); }
double _Stold_(PPUContext& ctx, u32 s, u32 e, s32 pten) { return stod_common(ctx, "_Stold", s, e, pten, 0, false, false); }
float _Stof_(PPUContext& ctx, u32 s, u32 e, s32 pten) { return static_cast<float>(stod_common(ctx, "_Stof", s, e, pten, 0, false, true)); }
double _Stodx_(PPUContext& ctx, u32 s, u32 e, s32 pten, u32 perr) { return stod_common(ctx, "_Stodx", s, e, pten, perr, false, false); }
double _Stoldx_(PPUContext& ctx, u32 s, u32 e, s32 pten, u32 perr) { return stod_common(ctx, "_Stoldx", s, e, pten, perr, false, false); }
float _Stofx_(PPUContext& ctx, u32 s, u32 e, s32 pten, u32 perr) { return static_cast<float>(stod_common(ctx, "_Stofx", s, e, pten, perr, false, true)); }
double _WStod_(PPUContext& ctx, u32 s, u32 e, s32 pten) { return stod_common(ctx, "_WStod", s, e, pten, 0, true, false); }
double _WStold_(PPUContext& ctx, u32 s, u32 e, s32 pten) { return stod_common(ctx, "_WStold", s, e, pten, 0, true, false); }
float _WStof_(PPUContext& ctx, u32 s, u32 e, s32 pten) { return static_cast<float>(stod_common(ctx, "_WStof", s, e, pten, 0, true, true)); }
double _WStodx_(PPUContext& ctx, u32 s, u32 e, s32 pten, u32 perr) { return stod_common(ctx, "_WStodx", s, e, pten, perr, true, false); }
double _WStoldx_(PPUContext& ctx, u32 s, u32 e, s32 pten, u32 perr) { return stod_common(ctx, "_WStoldx", s, e, pten, perr, true, false); }
float _WStofx_(PPUContext& ctx, u32 s, u32 e, s32 pten, u32 perr) { return static_cast<float>(stod_common(ctx, "_WStofx", s, e, pten, perr, true, true)); }

// Collation. The "C" locale has no collation state tables (the thread's, _Getpcostate): its order is the code order
// and its transformation the string itself, as the SDK's. A locale with tables (only from a locale file's collate
// lines, which the SDK's setlocale never loads) is not done: logged, and the code order used.
void note_tables(PPUContext& ctx, const char* fn, u32 tables)
{
    if (tables && guest_readable(thread_of(ctx), tables, 4) && ps_read32(gb(ctx), tables))
        PS_LOG_ERROR(kChan, "{}: collation by a locale's state tables is not implemented; the code order is used", fn);
}

s32 coll(PPUContext& ctx, const char* fn, u32 a, u32 b, bool wide, u32 tables)
{
    note_tables(ctx, fn, tables);
    if (wide) {
        const auto x = read_wstring(thread_of(ctx), a), y = read_wstring(thread_of(ctx), b);
        if (!x || !y) {
            refuse(ctx, std::format("{}: a wide string runs out of guest memory", fn));
            return 0;
        }
        return *x < *y ? -1 : *x > *y ? 1 : 0;
    }
    const auto x = read_string(thread_of(ctx), a), y = read_string(thread_of(ctx), b);
    if (!x || !y) {
        refuse(ctx, std::format("{}: a string runs out of guest memory", fn));
        return 0;
    }
    const s32 c = x->compare(*y); // bytes as unsigned char: std::char_traits<char> compares them so
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// The transformation (strxfrm's): the string and its NUL into d, as much of it as n holds (the SDK's writes the
// pieces that fit even when the whole does not); its length answered.
u32 xfrm(PPUContext& ctx, const char* fn, u32 d, u32 s, u32 n, bool wide, u32 tables)
{
    note_tables(ctx, fn, tables);
    if (wide) {
        const auto t = read_wstring(thread_of(ctx), s);
        if (!t) {
            refuse(ctx, std::format("{}: the wide string at {:#x} runs out of guest memory", fn, s));
            return 0;
        }
        const u32 k = std::min<u32>(static_cast<u32>(t->size()) + 1, n);
        if (k && range_ok(ctx, fn, d, 4 * u64{k}))
            write_wstring(thread_of(ctx), d, std::u32string_view(t->data(), std::min<usize>(k, t->size())), k > t->size());
        return static_cast<u32>(t->size());
    }
    const auto t = read_string(thread_of(ctx), s);
    if (!t) {
        refuse(ctx, std::format("{}: the string at {:#x} runs out of guest memory", fn, s));
        return 0;
    }
    const u32 k = std::min<u32>(static_cast<u32>(t->size()) + 1, n);
    if (k && range_ok(ctx, fn, d, k)) std::memcpy(gb(ctx) + d, t->c_str(), k);
    return static_cast<u32>(t->size());
}

s32 _Strcollx_(PPUContext& ctx, u32 a, u32 b, u32 tab) { return coll(ctx, "_Strcollx", a, b, false, tab); }
s32 _Wcscollx_(PPUContext& ctx, u32 a, u32 b, u32 tab) { return coll(ctx, "_Wcscollx", a, b, true, tab); }
u32 _Strxfrmx_(PPUContext& ctx, u32 d, u32 s, u32 n, u32 tab) { return xfrm(ctx, "_Strxfrmx", d, s, n, false, tab); }
u32 _Wcsxfrmx_(PPUContext& ctx, u32 d, u32 s, u32 n, u32 tab) { return xfrm(ctx, "_Wcsxfrmx", d, s, n, true, tab); }

// size_t _CStrxfrm(char *sout, size_t size, _Cosave *ps, const _Statab *tables): the next piece of the
// transformation of the string ps holds (its position the save area's first word, the second the SDK's scratch copy
// of it), at most size bytes into sout, the NUL included when reached; the bytes written answered.
u32 _CStrxfrm_(PPUContext& ctx, u32 sout, u32 size, u32 ps, u32 tables)
{
    note_tables(ctx, "_CStrxfrm", tables);
    if (!range_ok(ctx, "_CStrxfrm", ps, 8)) return 0;
    u8* b = gb(ctx);
    u32 s = ps_read32(b, ps);
    ps_write32(b, ps + 4, s);
    if (size == 0) return 0;
    lv2::PpuThread& th = thread_of(ctx);
    for (u32 n = 0;;) {
        if (!guest_readable(th, s, 1) || !range_ok(ctx, "_CStrxfrm", sout + n, 1)) return n;
        const u8 c = b[s];
        b[sout + n++] = c;
        if (c == 0) {
            ps_write32(b, ps, s);
            return n;
        }
        ps_write32(b, ps + 4, ++s);
        if (n == size) {
            ps_write32(b, ps, s);
            return n;
        }
    }
}

// size_t _CWcsxfrm(wchar_t *sout, const wchar_t **psin, size_t size, mbstate_t *ps, const _Statab *tables): the same
// for wide characters, *psin moved past what was written (at the NUL once it was).
u32 _CWcsxfrm_(PPUContext& ctx, u32 sout, u32 psin, u32 size, u32, u32 tables)
{
    note_tables(ctx, "_CWcsxfrm", tables);
    if (!range_ok(ctx, "_CWcsxfrm", psin, 4)) return 0;
    u8* b = gb(ctx);
    u32 s = ps_read32(b, psin), n = 0;
    while (n < size) {
        if (!range_ok(ctx, "_CWcsxfrm", s, 4) || !range_ok(ctx, "_CWcsxfrm", sout + 4 * n, 4)) break;
        const u32 c = ps_read32(b, s);
        ps_write32(b, sout + 4 * n++, c);
        if (c == 0) break;
        s += 4;
    }
    ps_write32(b, psin, s);
    return n;
}

// char *_Strerror(int code, char *buf): the message, copied into buf when one is given (Dinkumware's buffer holds
// any of them), else the thread's strerror text.
u32 _Strerror_(PPUContext& ctx, u32 code, u32 buf)
{
    const std::string_view text = error_text(code);
    u32 out = buf;
    if (!out) {
        const u32 b = thread_block(ctx);
        if (!b) return 0;
        out = b + tb::kStrerror;
    }
    if (!range_ok(ctx, "_Strerror", out, text.size() + 1)) return 0;
    std::memcpy(gb(ctx) + out, text.data(), text.size());
    gb(ctx)[out + text.size()] = 0;
    return out;
}


} // namespace

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_time(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(time), LIBC(clock), LIBC(difftime), LIBC(gmtime), LIBC(localtime), LIBC(mktime), LIBC(asctime),
            LIBC(ctime), LIBC(strftime), LIBC(wcsftime), LIBC(_Daysto), LIBC(_Ttotm), LIBC(_Isdst), LIBC(_Gettime),
            LIBC(_CStrftime), LIBC(_Gentime), LIBC(_Getdst), LIBC(_Getptimes), LIBC(_Wcsftime),
            LIBC(_Getpctype), LIBC(_Getptolower), LIBC(_Getptoupper),
            LIBC(_Stoul), LIBC(_Stoull), LIBC(_Stoll), LIBC(_Stolx), LIBC(_Stoulx), LIBC(_Stollx), LIBC(_Stoullx),
            LIBC(_WStoll), LIBC(_WStoul), LIBC(_WStoull), LIBC(_Stod), LIBC(_Stold), LIBC(_Stof), LIBC(_Stodx),
            LIBC(_Stoldx), LIBC(_Stofx), LIBC(_WStod), LIBC(_WStold), LIBC(_WStof), LIBC(_WStodx), LIBC(_WStoldx),
            LIBC(_WStofx), LIBC(_Strcollx), LIBC(_Wcscollx), LIBC(_Strxfrmx), LIBC(_Wcsxfrmx), LIBC(_CStrxfrm),
            LIBC(_CWcsxfrm), LIBC(_Strerror),
        });
    }
    r.add_variables("sys_libc", {
        Variable{{}, "_Ctype", recomp::nid_of("_Ctype"), kTableBytes, 8, &ctype_init},
        Variable{{}, "_Tolotab", recomp::nid_of("_Tolotab"), kTableBytes, 8, &tolower_init},
        Variable{{}, "_Touptab", recomp::nid_of("_Touptab"), kTableBytes, 8, &toupper_init},
        Variable{{}, "__ctype_ptr", recomp::nid_of("__ctype_ptr"), 8 + 257 + 7, 8, &newlib_ctype_init},
    });
}
#undef LIBC

} // namespace pspocket::hle
