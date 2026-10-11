// sys_libc part 5a (D-515): time.h, the "C" locale's locale.h and ctype tables, and Dinkumware's number-conversion
// internals, under sys_libc and sys_libm.
// - time_t is 64 bits (the SDK's CellFsStat and CellFsUtimbuf hold time_t in 64-bit fields), counting seconds from
//   1970 (lv2's sys_time_get_current_time). Local time is lv2's: its time zone and summer-time offsets in minutes east
//   of UTC (sys_time_get_timezone), tm_isdst 1 while the summer offset is not 0.
// - clock() counts microseconds from the first call (POSIX's CLOCKS_PER_SEC of 1000000; the SDK's value is not
//   known).
// - strftime and wcsftime are C99's in the "C" locale (%c "%a %b %e %H:%M:%S %Y", %x "%m/%d/%y", %X "%H:%M:%S"); %Z
//   gives nothing (no zone names), %z lv2's offset.
// - The ctype tables a header's macros may read: Dinkumware's (_Ctype, _Tolotab, _Touptab and the _Getp* functions:
//   257 shorts from EOF, its C99 class bits) and newlib's __ctype_ptr (257 chars from EOF). The variables hold the
//   pointer to entry 0 (Plauger's `const short *_Ctype = &ctyp_tab[1]`), the table after it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_scan_guest.hpp"
#include "libc_malloc.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- calendar

// Days from 1970-01-01 to y-m-d (proleptic Gregorian; Howard Hinnant's days_from_civil), and back.
s64 days_from_civil(s64 y, u32 m, u32 d)
{
    y -= m <= 2;
    const s64 era = (y >= 0 ? y : y - 399) / 400;
    const u32 yoe = static_cast<u32>(y - era * 400);
    const u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<s64>(doe) - 719468;
}

void civil_from_days(s64 z, s64& y, u32& m, u32& d)
{
    z += 719468;
    const s64 era = (z >= 0 ? z : z - 146096) / 146097;
    const u32 doe = static_cast<u32>(z - era * 146097);
    const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u32 mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<s64>(yoe) + era * 400 + (m <= 2);
}

bool leap(s64 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

struct Tm {
    s32 sec = 0, min = 0, hour = 0, mday = 1, mon = 0, year = 70, wday = 4, yday = 0, isdst = 0;
};

Tm to_tm(s64 t, s32 isdst)
{
    Tm r;
    s64 days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    r.hour = static_cast<s32>(rem / 3600);
    r.min = static_cast<s32>(rem % 3600 / 60);
    r.sec = static_cast<s32>(rem % 60);
    s64 y;
    u32 m, d;
    civil_from_days(days, y, m, d);
    r.year = static_cast<s32>(y - 1900);
    r.mon = static_cast<s32>(m) - 1;
    r.mday = static_cast<s32>(d);
    r.wday = static_cast<s32>(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday
    r.yday = static_cast<s32>(days - days_from_civil(y, 1, 1));
    r.isdst = isdst;
    return r;
}

// The seconds of a broken-down time (its fields normalized as mktime does), as if it were UTC.
s64 from_tm(const Tm& t)
{
    s64 mon = t.mon, year = s64{t.year} + 1900;
    year += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }
    const s64 days = days_from_civil(year, static_cast<u32>(mon) + 1, 1) + t.mday - 1;
    return days * 86400 + s64{t.hour} * 3600 + s64{t.min} * 60 + t.sec;
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

// lv2's time zone and summer-time offsets, in seconds east of UTC.
struct Zone {
    s64 offset = 0, summer = 0;
};

Zone zone(PPUContext& ctx)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar v(ctx, t.kernel.base(), 8);
    Zone z;
    if (lv2::sys_time_get_timezone(t, v.addr(), v.addr() + 4) == lv2::CELL_OK) {
        z.offset = s64{static_cast<s32>(ps_read32(t.kernel.base(), v.addr()))} * 60;
        z.summer = s64{static_cast<s32>(ps_read32(t.kernel.base(), v.addr() + 4))} * 60;
    }
    return z;
}

s64 now(PPUContext& ctx)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar v(ctx, t.kernel.base(), 16);
    if (lv2::sys_time_get_current_time(t, v.addr(), v.addr() + 8) != lv2::CELL_OK) return 0;
    return static_cast<s64>(ps_read64(t.kernel.base(), v.addr()));
}

u32 tm_block(PPUContext& ctx)
{
    const u32 b = thread_block(ctx);
    return b ? b + tb::kTm : 0;
}

// ------------------------------------------------------------------------------------------------- time.h

u64 time_(PPUContext& ctx, u32 out)
{
    const s64 t = now(ctx);
    if (out && range_ok(ctx, "time", out, 8)) ps_write64(gb(ctx), out, static_cast<u64>(t));
    return static_cast<u64>(t);
}

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

u32 gmtime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "gmtime", tp, 8)) return 0;
    return put_tm(ctx, to_tm(static_cast<s64>(ps_read64(gb(ctx), tp)), 0));
}

Tm local(PPUContext& ctx, s64 t)
{
    const Zone z = zone(ctx);
    return to_tm(t + z.offset + z.summer, z.summer ? 1 : 0);
}

u32 localtime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "localtime", tp, 8)) return 0;
    return put_tm(ctx, local(ctx, static_cast<s64>(ps_read64(gb(ctx), tp))));
}

// mktime: the local time's seconds, its fields normalized (wday and yday set); tm_isdst > 0 takes the summer offset,
// < 0 lv2's current choice.
u64 mktime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "mktime", tp, 36)) return ~u64{0};
    const Tm in = read_tm(gb(ctx), tp);
    const Zone z = zone(ctx);
    const bool dst = in.isdst > 0 || (in.isdst < 0 && z.summer);
    const s64 t = from_tm(in) - z.offset - (dst ? z.summer : 0);
    Tm out = to_tm(t + z.offset + (dst ? z.summer : 0), dst ? 1 : 0);
    write_tm(gb(ctx), tp, out);
    return static_cast<u64>(t);
}

constexpr std::array<const char*, 7> kDays = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"};
constexpr std::array<const char*, 12> kMonths = {"January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"};

std::string_view day_name(s32 d, bool full)
{
    if (d < 0 || d > 6) return "?";
    const std::string_view n = kDays[static_cast<usize>(d)];
    return full ? n : n.substr(0, 3);
}

std::string_view month_name(s32 m, bool full)
{
    if (m < 0 || m > 11) return "?";
    const std::string_view n = kMonths[static_cast<usize>(m)];
    return full ? n : n.substr(0, 3);
}

// asctime: C99's algorithm (7.23.3.1), "Sun Sep 16 01:03:52 1973\n", in the thread's 26 bytes.
u32 asctime_(PPUContext& ctx, u32 tp)
{
    if (!tp || !range_ok(ctx, "asctime", tp, 36)) return 0;
    const Tm t = read_tm(gb(ctx), tp);
    const std::string s = std::format("{:.3} {:.3}{:3} {:02}:{:02}:{:02} {}\n", day_name(t.wday, false),
        month_name(t.mon, false), t.mday, t.hour, t.min, t.sec, s64{t.year} + 1900);
    const u32 b = thread_block(ctx);
    if (!b) return 0;
    const usize n = std::min<usize>(s.size(), 25);
    std::memcpy(gb(ctx) + b + tb::kAsctime, s.data(), n);
    gb(ctx)[b + tb::kAsctime + n] = 0;
    return b + tb::kAsctime;
}

u32 ctime_(PPUContext& ctx, u32 tp)
{
    const u32 t = localtime_(ctx, tp);
    return t ? asctime_(ctx, t) : 0;
}

// ISO 8601 week-based year and week of a broken-down time.
void iso_week(const Tm& t, s64& year, s32& week)
{
    year = s64{t.year} + 1900;
    const s32 wday = (t.wday + 6) % 7; // Monday 0
    s32 w = (t.yday - wday + 10) / 7;
    if (w < 1) {
        year--;
        const s32 days = leap(year) ? 366 : 365;
        w = (t.yday + days - wday + 10) / 7;
    } else if (w == 53) {
        const s32 days = leap(year) ? 366 : 365;
        if (t.yday - wday + 3 >= days) {
            year++;
            w = 1;
        }
    }
    week = w;
}

// strftime's text of `fmt` for `t` (C99 7.23.3.5 in the "C" locale).
template <class C>
std::basic_string<C> strftime_text(std::basic_string_view<C> fmt, const Tm& t, const Zone& z)
{
    std::basic_string<C> out;
    const auto put = [&](std::string_view s) {
        for (const char c : s) out += static_cast<C>(static_cast<u8>(c));
    };
    for (usize i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }
        C c = fmt[++i];
        if ((c == 'E' || c == 'O') && i + 1 < fmt.size()) c = fmt[++i]; // the "C" locale's alternatives are the same
        const s64 year = s64{t.year} + 1900;
        switch (c) {
        case 'a': put(day_name(t.wday, false)); break;
        case 'A': put(day_name(t.wday, true)); break;
        case 'b':
        case 'h': put(month_name(t.mon, false)); break;
        case 'B': put(month_name(t.mon, true)); break;
        case 'c':
            put(std::format("{} {} {:2} {:02}:{:02}:{:02} {}", day_name(t.wday, false), month_name(t.mon, false),
                t.mday, t.hour, t.min, t.sec, year));
            break;
        case 'C': put(std::format("{:02}", year / 100)); break;
        case 'd': put(std::format("{:02}", t.mday)); break;
        case 'D':
        case 'x': put(std::format("{:02}/{:02}/{:02}", t.mon + 1, t.mday, ((year % 100) + 100) % 100)); break;
        case 'e': put(std::format("{:2}", t.mday)); break;
        case 'F': put(std::format("{}-{:02}-{:02}", year, t.mon + 1, t.mday)); break;
        case 'g':
        case 'G': {
            s64 y;
            s32 w;
            iso_week(t, y, w);
            put(c == 'g' ? std::format("{:02}", ((y % 100) + 100) % 100) : std::format("{}", y));
            break;
        }
        case 'H': put(std::format("{:02}", t.hour)); break;
        case 'I': put(std::format("{:02}", t.hour % 12 ? t.hour % 12 : 12)); break;
        case 'j': put(std::format("{:03}", t.yday + 1)); break;
        case 'm': put(std::format("{:02}", t.mon + 1)); break;
        case 'M': put(std::format("{:02}", t.min)); break;
        case 'n': out += static_cast<C>('\n'); break;
        case 'p': put(t.hour < 12 ? "AM" : "PM"); break;
        case 'r':
            put(std::format("{:02}:{:02}:{:02} {}", t.hour % 12 ? t.hour % 12 : 12, t.min, t.sec,
                t.hour < 12 ? "AM" : "PM"));
            break;
        case 'R': put(std::format("{:02}:{:02}", t.hour, t.min)); break;
        case 'S': put(std::format("{:02}", t.sec)); break;
        case 't': out += static_cast<C>('\t'); break;
        case 'T':
        case 'X': put(std::format("{:02}:{:02}:{:02}", t.hour, t.min, t.sec)); break;
        case 'u': put(std::format("{}", t.wday ? t.wday : 7)); break;
        case 'U': put(std::format("{:02}", (t.yday + 7 - t.wday) / 7)); break;
        case 'V': {
            s64 y;
            s32 w;
            iso_week(t, y, w);
            put(std::format("{:02}", w));
            break;
        }
        case 'w': put(std::format("{}", t.wday)); break;
        case 'W': put(std::format("{:02}", (t.yday + 7 - (t.wday + 6) % 7) / 7)); break;
        case 'y': put(std::format("{:02}", ((year % 100) + 100) % 100)); break;
        case 'Y': put(std::format("{}", year)); break;
        case 'z': {
            const s64 off = (z.offset + (t.isdst > 0 ? z.summer : 0)) / 60;
            put(std::format("{}{:02}{:02}", off < 0 ? '-' : '+', std::abs(off) / 60, std::abs(off) % 60));
            break;
        }
        case 'Z': break; // no zone names
        case '%': out += static_cast<C>('%'); break;
        default:
            out += static_cast<C>('%');
            out += c;
            break;
        }
    }
    return out;
}

// strftime: the text and a NUL when they fit in max, its length; else 0 (the array's contents indeterminate).
u32 strftime_(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp)
{
    const auto f = read_string(thread_of(ctx), fmt);
    if (!f || !tp || !range_ok(ctx, "strftime", tp, 36)) {
        if (!f) refuse(ctx, std::format("strftime: the format at {:#x} runs out of guest memory", fmt));
        return 0;
    }
    const std::string text = strftime_text<char>(*f, read_tm(gb(ctx), tp), zone(ctx));
    if (text.size() + 1 > max || !range_ok(ctx, "strftime", s, text.size() + 1)) return 0;
    write_string(thread_of(ctx), s, text);
    return static_cast<u32>(text.size());
}

u32 wcsftime_(PPUContext& ctx, u32 s, u32 max, u32 fmt, u32 tp)
{
    const auto f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f || !tp || !range_ok(ctx, "wcsftime", tp, 36)) {
        if (!f) refuse(ctx, std::format("wcsftime: the format at {:#x} runs out of guest memory", fmt));
        return 0;
    }
    const std::u32string text = strftime_text<char32_t>(*f, read_tm(gb(ctx), tp), zone(ctx));
    if (text.size() + 1 > max || !range_ok(ctx, "wcsftime", s, 4 * (text.size() + 1))) return 0;
    write_wstring(thread_of(ctx), s, text);
    return static_cast<u32>(text.size());
}

// Dinkumware's time internals (Plauger's xttotm.c, xgettime.c, xisdst.c).
// int _Daysto(int year, int mon): the leap days from 1900 to `year` (years since 1900) and the days before month
// `mon` in it.
s32 _Daysto_(PPUContext&, s32 year, s32 mon)
{
    static constexpr s16 kMos[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    static constexpr s16 kLmos[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
    s32 days;
    if (0 < year) days = (year - 1) / 4;
    else if (year <= -4) days = 1 + (4 - year) / 4;
    else days = 0;
    const s16* tab = ((year & 3) || year == 0) ? kMos : kLmos;
    return days + (mon >= 0 && mon < 12 ? tab[mon] : 0);
}

// struct tm *_Ttotm(struct tm *t, time_t secs, int isdst): the broken-down UTC time of secs (the thread's tm when t
// is NULL); isdst < 0 takes lv2's summer time, > 0 adds its offset.
u32 _Ttotm_(PPUContext& ctx, u32 tp, u64 secs, s32 isdst)
{
    const Zone z = zone(ctx);
    if (isdst < 0) isdst = z.summer ? 1 : 0;
    const Tm t = to_tm(static_cast<s64>(secs) + (isdst > 0 ? (z.summer ? z.summer : 3600) : 0), isdst);
    const u32 out = tp ? tp : tm_block(ctx);
    if (!out || !range_ok(ctx, "_Ttotm", out, 36)) return 0;
    write_tm(gb(ctx), out, t);
    return out;
}

// int _Isdst(const struct tm *t): lv2's summer time.
s32 _Isdst_(PPUContext& ctx, u32) { return zone(ctx).summer ? 1 : 0; }

// const char *_Gettime(const char *s, int n, int *len): field n of s, whose first character delimits its fields
// (":Sun:Sunday:..."), and its length.
u32 _Gettime_(PPUContext& ctx, u32 s, s32 n, u32 len)
{
    const auto text = read_string(thread_of(ctx), s);
    if (!text || !range_ok(ctx, "_Gettime", len, 4)) return 0;
    usize at = 0;
    const char delim = text->empty() ? '\0' : (*text)[at++];
    for (;; --n) {
        usize end = delim ? text->find(delim, at) : std::string::npos;
        if (end == std::string::npos) end = text->size();
        if (n <= 0) {
            ps_write32(gb(ctx), len, static_cast<u32>(end - at));
            return s + static_cast<u32>(at);
        }
        if (end >= text->size()) {
            ps_write32(gb(ctx), len, 1);
            return s + static_cast<u32>(end);
        }
        at = end + 1;
    }
}

// ------------------------------------------------------------------------------------------------- locale

// Dinkumware's C99 class bits.
constexpr u16 kXB = 0x400, kXD = 0x01, kUP = 0x02, kSP = 0x04, kPU = 0x08, kLO = 0x10, kDI = 0x20, kCN = 0x40,
              kBB = 0x80;

u16 dinkum_class(s32 c)
{
    if (c < 0 || c > 127) return 0;
    if (c == '\t') return kCN | kXB;
    if (c >= '\n' && c <= '\r') return kCN;
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

// The library's constant strings in guest memory (made once): "C", "", "." and the lconv of the "C" locale.
struct LocaleState {
    std::mutex mutex;
    u32 strings = 0; // "C\0" at +0, "" at +2, ".\0" at +4
    u32 lconv = 0;
};

u32 locale_strings(PPUContext& ctx, LocaleState& s)
{
    if (!s.strings) {
        s.strings = lib_malloc(ctx, 8);
        if (s.strings) std::memcpy(gb(ctx) + s.strings, "C\0\0\0.\0\0", 8);
    }
    return s.strings;
}

// setlocale: the "C" locale only ("C", "POSIX" or "" for it); NULL asks which (always "C"). Categories LC_ALL (0) to
// LC_MESSAGES (6).
u32 setlocale_(PPUContext& ctx, s32 category, u32 name)
{
    if (category < 0 || category > 6) return 0;
    LocaleState& s = HleProcess::current()->state<LocaleState>();
    std::lock_guard l(s.mutex);
    const u32 strings = locale_strings(ctx, s);
    if (!strings) return 0;
    if (!name) return strings;
    const auto n = read_string(thread_of(ctx), name, 256);
    if (!n) {
        refuse(ctx, std::format("setlocale: the name at {:#x} runs out of guest memory", name));
        return 0;
    }
    return (n->empty() || *n == "C" || *n == "POSIX") ? strings : 0;
}

// localeconv: the "C" locale's struct lconv, Dinkumware's C99 layout: seven monetary strings, fourteen chars
// (CHAR_MAX, 255: a guest char is unsigned), then decimal_point (+44), grouping, thousands_sep and Dinkumware's five
// more strings.
u32 localeconv_(PPUContext& ctx)
{
    LocaleState& s = HleProcess::current()->state<LocaleState>();
    std::lock_guard l(s.mutex);
    const u32 strings = locale_strings(ctx, s);
    if (!strings) return 0;
    if (!s.lconv) {
        s.lconv = lib_malloc(ctx, 80);
        if (!s.lconv) return 0;
        u8* b = gb(ctx);
        std::memset(b + s.lconv, 0, 80);
        for (u32 i = 0; i < 7; i++) ps_write32(b, s.lconv + 4 * i, strings + 2);
        for (u32 i = 0; i < 14; i++) b[s.lconv + 28 + i] = 0xFF;
        ps_write32(b, s.lconv + 44, strings + 4);
        for (u32 i = 1; i < 9; i++) ps_write32(b, s.lconv + 44 + 4 * i, strings + 2);
    }
    return s.lconv;
}

// ------------------------------------------------------------------------------------------------- conversions

// Dinkumware's conversions as the SDK's C library has them (libc_scan.hpp, D-907, a game's copy the reference): the
// x forms set *perr (0, or 1 on a range error) and errno as well; _Stoul is _Stoulx without perr, and so on. _Stod,
// _Stof and _Stold scale by 10^pten in the same arithmetic (the power of ten added to the digits' own).
u64 to_int(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 base, u32 bits, bool is_signed, u32 perr, bool wide)
{
    return guest_strtoint(ctx, fn, s, endp, base, bits, is_signed, perr, wide);
}

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

double stod_common(PPUContext& ctx, const char* fn, u32 s, u32 e, s32 pten, u32 perr, bool wide, bool single)
{
    return guest_strtod(ctx, fn, s, e, pten, perr, wide, single);
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

// Collation in the "C" locale is the code order, whatever table the x forms are given.
s32 coll(PPUContext& ctx, const char* fn, u32 a, u32 b, bool wide)
{
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

u32 xfrm(PPUContext& ctx, const char* fn, u32 d, u32 s, u32 n, bool wide)
{
    if (wide) {
        const auto t = read_wstring(thread_of(ctx), s);
        if (!t) return 0;
        if (t->size() < n && range_ok(ctx, fn, d, 4 * (t->size() + 1))) write_wstring(thread_of(ctx), d, *t);
        return static_cast<u32>(t->size());
    }
    const auto t = read_string(thread_of(ctx), s);
    if (!t) return 0;
    if (t->size() < n && range_ok(ctx, fn, d, t->size() + 1)) write_string(thread_of(ctx), d, *t);
    return static_cast<u32>(t->size());
}

s32 _Strcollx_(PPUContext& ctx, u32 a, u32 b, u32) { return coll(ctx, "_Strcollx", a, b, false); }
s32 _Wcscollx_(PPUContext& ctx, u32 a, u32 b, u32) { return coll(ctx, "_Wcscollx", a, b, true); }
u32 _Strxfrmx_(PPUContext& ctx, u32 d, u32 s, u32 n, u32) { return xfrm(ctx, "_Strxfrmx", d, s, n, false); }
u32 _Wcsxfrmx_(PPUContext& ctx, u32 d, u32 s, u32 n, u32) { return xfrm(ctx, "_Wcsxfrmx", d, s, n, true); }
u32 _CStrxfrm_(PPUContext& ctx, u32 d, u32 s, u32 n, u32) { return xfrm(ctx, "_CStrxfrm", d, s, n, false); }
u32 _CWcsxfrm_(PPUContext& ctx, u32 d, u32 s, u32 n, u32) { return xfrm(ctx, "_CWcsxfrm", d, s, n, true); }

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
            LIBC(setlocale), LIBC(localeconv), LIBC(_Getpctype), LIBC(_Getptolower), LIBC(_Getptoupper),
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
