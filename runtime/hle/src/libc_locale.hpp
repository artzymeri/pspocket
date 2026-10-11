#pragma once

// sys_libc's locale objects and multibyte conversions in the SDK's layouts (D-906), shared by sys_libc_locale.cpp,
// sys_libc_time.cpp and sys_libc_wide.cpp. Read from a game's statically linked copy of the SDK's C library
// (Dinkumware's C99 library with Sony's changes); nothing of it is copied here, the layouts and the rules are written
// from what was understood.
//
// - A locale is Dinkumware's _Linfo (452 bytes): its name and the next locale of the library's list, the collation
//   state tables (narrow and wide), the ctype, tolower and toupper tables, MB_CUR_MAX, the multibyte and wide state
//   tables, the wide class and translation tables, a struct lconv and a _Tinfo (the time names and formats).
// - struct lconv is 80 bytes: seven monetary strings, fourteen chars, decimal_point at +44, grouping, thousands_sep,
//   Dinkumware's frac_grouping and frac_sep, and the false, true, no and yes strings.
// - _Tinfo is 21 string pointers: am_pm, days, abday, day, months, abmon, mon, time_formats, d_t_fmt, d_fmt, t_fmt,
//   t_fmt_ampm, the five era formats, era, alt_digits, dst_rules, time_zone. Each name list is a string whose first
//   character delimits its fields (":Sun:Sunday:..."); the "C" locale's ten format pointers all point at one string of
//   four fields ("|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p"), each conversion taking its own field.
// - A state table (_Statab) is 16 pointers, each to 256 halfword entries (or NULL): the conversion's state machine.
//   The SDK keeps the multibyte, wide, collation and wide collation tables per thread (thread-local); here they are in
//   the thread's block. All NULL is the "C" locale: UTF-8 for the multibyte conversions (at most three bytes, wide
//   characters up to 0xFFFF: the SDK's wchar_t is 16 bits, MB_CUR_MAX 3) and code order for collation.

#include <cstring>
#include <optional>
#include <string>

#include "libc_guest.hpp"

namespace pspocket::hle::libc {

// _Linfo.
namespace li {
inline constexpr u32 kName = 0, kNext = 4, kCostate = 0x8, kWCostate = 0x48, kCtype = 0x88, kTolotab = 0x8C,
                     kTouptab = 0x90, kMbcurmax = 0x94, kMbstate = 0x98, kWcstate = 0xD8, kWctrans = 0x118,
                     kWctype = 0x11C, kLconv = 0x120, kTimes = 0x170, kSize = 0x1C4;
}

// struct lconv.
namespace lcv {
inline constexpr u32 kCurrencySymbol = 0, kIntCurrSymbol = 4, kMonDecimalPoint = 8, kMonGrouping = 12,
                     kMonThousandsSep = 16, kNegativeSign = 20, kPositiveSign = 24, kFracDigits = 28,
                     kDecimalPoint = 44, kGrouping = 48, kThousandsSep = 52, kFracGrouping = 56, kFracSep = 60,
                     kFalse = 64, kTrue = 68, kNo = 72, kYes = 76, kSize = 80;
}

// _Tinfo.
namespace ti {
inline constexpr u32 kAmPm = 0, kDays = 4, kAbday = 8, kDay = 12, kMonths = 16, kAbmon = 20, kMon = 24, kFormats = 28,
                     kDTFmt = 32, kDFmt = 36, kTFmt = 40, kTFmtAmpm = 44, kEraFormats = 48, kEraDTFmt = 52,
                     kEraDFmt = 56, kEraTFmt = 60, kEraTFmtAmpm = 64, kEra = 68, kAltDigits = 72, kDstRules = 76,
                     kTzone = 80, kSize = 84;
}

// The "C" locale's time strings (C99's names; the SDK's arrangement).
inline constexpr std::string_view kCAmPm = ":AM:PM";
inline constexpr std::string_view kCDays = ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:"
                                           "Saturday";
inline constexpr std::string_view kCMonths = ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:"
                                             "Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";
inline constexpr std::string_view kCFormats = "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p";

// Lays a "C" _Tinfo out at `a` (84 bytes), its strings from `text` on; the end of the strings is answered.
inline u32 lay_c_times(u8* b, u32 a, u32 text)
{
    const auto put = [&](std::string_view s) {
        const u32 at = text;
        std::memcpy(b + text, s.data(), s.size());
        b[text + s.size()] = 0;
        text += static_cast<u32>(s.size()) + 1;
        return at;
    };
    const u32 ampm = put(kCAmPm), days = put(kCDays), months = put(kCMonths), formats = put(kCFormats);
    ps_write32(b, a + ti::kAmPm, ampm);
    for (u32 i = 0; i < 3; i++) ps_write32(b, a + ti::kDays + 4 * i, days);
    for (u32 i = 0; i < 3; i++) ps_write32(b, a + ti::kMonths + 4 * i, months);
    for (u32 i = 0; i < 10; i++) ps_write32(b, a + ti::kFormats + 4 * i, formats);
    // era, alt_digits, dst_rules and time_zone: four empty strings of their own.
    for (u32 i = 0; i < 4; i++) ps_write32(b, a + ti::kEra + 4 * i, put(""));
    return text;
}

inline constexpr u32 kNState = 16, kStatab = 4 * kNState;

// The calling thread's state tables and its global locale pointer, in its block.
inline u32 thread_slot(PPUContext& ctx, u32 offset)
{
    const u32 b = thread_block(ctx);
    return b ? b + offset : 0;
}
inline u32 mb_tables(PPUContext& ctx) { return thread_slot(ctx, tb::kMbTab); }
inline u32 wc_tables(PPUContext& ctx) { return thread_slot(ctx, tb::kWcTab); }
inline u32 co_tables(PPUContext& ctx) { return thread_slot(ctx, tb::kCoTab); }
inline u32 wco_tables(PPUContext& ctx) { return thread_slot(ctx, tb::kWcoTab); }

// ------------------------------------------------------------------------------------------------- conversions

// mbstate_t, Dinkumware's {unsigned long _Wchar; unsigned short _Byte, _State} (8 bytes); the conversions read
// _State's low byte as a signed char.
struct MbState {
    u32 wchar = 0;
    u16 byte = 0, state = 0;
};

inline MbState get_mbstate(u8* b, u32 a)
{
    return {ps_read32(b, a), ps_read16(b, a + 4), ps_read16(b, a + 6)};
}

// Dinkumware's state table entries: the next state (bits 8..11), fold the low byte into the character, take the input
// byte, deliver the character, swap the character's two bytes.
inline constexpr u16 kStFold = 0x8000, kStInput = 0x4000, kStOutput = 0x2000, kStRotate = 0x1000;
inline constexpr u32 kMbError = 0xFFFFFFFFu, kMbPartial = 0xFFFFFFFEu;

// What a conversion step leaves in the state: nothing (an error), _State alone (a character from UTF-8) or _State and
// _Wchar (an incomplete character, or one from the tables).
enum class StateWrite { None, State, Both };

struct MbStep {
    u32 result = 0;         // the size_t answer: bytes, 0 (the NUL), kMbError or kMbPartial
    bool have = false;      // a character was delivered (into wc)
    u32 wc = 0;             // the character (16 bits, as the SDK's wchar_t holds it)
    StateWrite write = StateWrite::None;
};

// Dinkumware's _Mbtowcx on its state: the next multibyte character of the bytes `at(0..n)` (at(i) < 0: not guest
// memory), by the state tables at `tables` (0 or a _Statab whose first table is NULL: UTF-8). Sets errno (EILSEQ) on an
// invalid sequence. `unreadable` is set when the bytes run out of guest memory.
template <class At>
MbStep mbtowc_step(PPUContext& ctx, At&& at, u32 n, MbState& st, u32 tables, bool* unreadable)
{
    u8* b = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    const auto table = [&](s32 state) -> u32 {
        if (!tables || state < 0 || state >= static_cast<s32>(kNState)) return 0;
        return ps_read32(b, tables + 4 * static_cast<u32>(state));
    };
    const auto fail = [&]() {
        set_errno(ctx, kEILSEQ);
        MbStep r;
        r.result = kMbError;
        return r;
    };
    s32 state = static_cast<s8>(st.state & 0xFF);
    u32 wc = st.wchar & 0xFFFF;
    const u32 tab0 = table(0);
    if (!tab0) {
        // UTF-8 of up to three bytes.
        for (u32 i = 0; i < n; i++) {
            const int c = at(i);
            if (c < 0) {
                *unreadable = true;
                return MbStep{kMbError};
            }
            bool done = false;
            if (state < 1) {
                if (c < 0x80) {
                    wc = static_cast<u32>(c);
                    done = true;
                } else if ((c & 0xE0) == 0xC0) {
                    wc = static_cast<u32>(c & 0x1F);
                    state = 1;
                } else if ((c & 0xF0) == 0xE0) {
                    wc = static_cast<u32>(c & 0x0F);
                    state = 2;
                } else {
                    return fail();
                }
            } else {
                state--;
                wc = ((wc & 0xFFFF) << 6) | static_cast<u32>(c & 0x3F);
                if ((c & 0xC0) != 0x80) return fail();
                done = true;
            }
            if (done && state == 0) {
                st.state = 0;
                MbStep r;
                r.have = true;
                r.wc = wc & 0xFFFF;
                r.result = r.wc ? i + 1 : 0;
                r.write = StateWrite::State;
                return r;
            }
        }
        st.state = static_cast<u16>(static_cast<s16>(state));
        st.wchar = wc & 0xFFFF;
        return MbStep{kMbPartial, false, 0, StateWrite::Both};
    }
    // The state tables.
    if (n == 0) {
        st.state = static_cast<u16>(static_cast<s16>(state));
        st.wchar = wc & 0xFFFF;
        return MbStep{kMbPartial, false, 0, StateWrite::Both};
    }
    u32 tab = table(state);
    if (!tab) return fail();
    u32 limit = 1, i = 0;
    for (;;) {
        const int c = at(i);
        if (c < 0) {
            *unreadable = true;
            return MbStep{kMbError};
        }
        const u32 entry = tab + 2 * static_cast<u32>(c);
        if (!guest_readable(t, entry, 2)) {
            *unreadable = true;
            return MbStep{kMbError};
        }
        const u16 code = ps_read16(b, entry);
        if (code == 0) return fail();
        state = (code >> 8) & 0xF;
        if (code & kStFold) wc = (wc & ~0xFFu) | (code & 0xFF);
        if (code & kStRotate) wc = ((wc & 0xFFFF) << 8) | ((wc & 0xFFFF) >> 8);
        if ((code & kStInput) && c != 0) {
            i++;
            n--;
            limit = 0;
        }
        if (code & kStOutput) {
            st.state = static_cast<u16>(state);
            st.wchar = wc & 0xFFFF;
            MbStep r;
            r.have = true;
            r.wc = wc & 0xFFFF;
            r.result = r.wc ? i : 0;
            r.write = StateWrite::Both;
            return r;
        }
        if (n == 0) {
            st.state = static_cast<u16>(state);
            st.wchar = wc & 0xFFFF;
            return MbStep{kMbPartial, false, 0, StateWrite::Both};
        }
        tab = table(state);
        if (!tab || ++limit > 0xFEF) return fail();
    }
}

// _Mbtowcx(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps, _Statab *tables) on guest memory. s NULL resets the
// state and answers whether the encoding has shift states (the first table's first entry's state bits).
inline u32 mbtowcx(PPUContext& ctx, std::string_view fn, u32 pwc, u32 s, u32 n, u32 ps, u32 tables)
{
    u8* b = guest_base(ctx);
    if (!range_ok(ctx, fn, ps, 8) || !range_ok(ctx, fn, tables, kStatab)) return kMbError;
    const u32 tab0 = ps_read32(b, tables);
    if (!s) {
        std::memset(b + ps, 0, 8);
        if (!tab0) return 0;
        return range_ok(ctx, fn, tab0, 2) ? (ps_read16(b, tab0) & 0xF00u) : 0;
    }
    MbState st = get_mbstate(b, ps);
    GuestBytes bytes(thread_of(ctx), s);
    bool unreadable = false;
    const MbStep r = mbtowc_step(ctx, [&](u64 i) { return bytes.at(i); }, n, st, tables, &unreadable);
    if (unreadable) {
        refuse(ctx, std::format("{}: the bytes at {:#x} run out of guest memory", fn, s));
        return kMbError;
    }
    if (r.write != StateWrite::None) ps_write16(b, ps + 6, st.state);
    if (r.write == StateWrite::Both) ps_write32(b, ps, st.wchar);
    if (r.have && pwc) {
        if (!range_ok(ctx, fn, pwc, 4)) return kMbError;
        ps_write32(b, pwc, r.wc);
    }
    return r.result;
}

// Dinkumware's _Wctombx(char *s, wchar_t wc, mbstate_t *ps, _Statab *mbtables, _Statab *wctables): the multibyte
// form of wc into s, by the wide state tables when the multibyte ones have any, else UTF-8 (a wide character past
// 0xFFFF has none: EILSEQ, as no form of it fits the SDK's MB_CUR_MAX). s NULL resets the state.
inline u32 wctombx(PPUContext& ctx, std::string_view fn, u32 s, u32 wc, u32 ps, u32 mbtables, u32 wctables)
{
    u8* b = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    if (!range_ok(ctx, fn, ps, 8) || !range_ok(ctx, fn, mbtables, kStatab)) return kMbError;
    const u32 tab0 = ps_read32(b, mbtables);
    const auto fail = [&]() {
        set_errno(ctx, kEILSEQ);
        return kMbError;
    };
    if (!s) {
        std::memset(b + ps, 0, 8);
        if (!tab0) return 0;
        return range_ok(ctx, fn, tab0, 2) ? (ps_read16(b, tab0) & 0xF00u) : 0;
    }
    if (!tab0) {
        if (wc > 0xFFFF) return fail();
        u8 out[3];
        u32 k;
        if ((wc & 0xFF80) == 0) {
            out[0] = static_cast<u8>(wc);
            k = 1;
        } else {
            const u32 more = (wc & 0xF800) == 0 ? 1 : 2;
            out[0] = static_cast<u8>(more == 1 ? (wc >> 6) | 0xC0 : (wc >> 12) | 0xE0);
            for (u32 j = 1; j <= more; j++) out[j] = static_cast<u8>(((wc >> (6 * (more - j))) & 0x3F) | 0x80);
            k = more + 1;
        }
        if (!range_ok(ctx, fn, s, k)) return kMbError;
        std::memcpy(b + s, out, k);
        return k;
    }
    if (!range_ok(ctx, fn, wctables, kStatab)) return kMbError;
    s32 state = static_cast<s8>(ps_read16(b, ps + 6) & 0xFF);
    const auto table = [&](s32 st) -> u32 {
        return st < 0 || st >= static_cast<s32>(kNState) ? 0 : ps_read32(b, wctables + 4 * static_cast<u32>(st));
    };
    u32 tab = table(state);
    if (!tab) return fail();
    u32 w = wc, k = 0, limit = 1;
    bool nul = false;
    const auto entry = [&](u32 tb_, u32 x, u16* code) {
        const u32 e = tb_ + 2 * (x & 0xFF);
        if (!guest_readable(t, e, 2)) return false;
        *code = ps_read16(b, e);
        return true;
    };
    u16 code = 0;
    if (!entry(tab, w, &code)) return kMbError;
    while (code != 0) {
        state = (code >> 8) & 0xF;
        if (code & kStFold) w = (w & ~0xFFu) | (code & 0xFF);
        if (code & kStRotate) w = ((w & 0xFFFF) << 8) | ((w & 0xFFFF) >> 8);
        if (code & kStOutput) {
            const u8 byte = static_cast<u8>((code & 0xFF) ? code : w);
            if (!range_ok(ctx, fn, s + k, 1)) return kMbError;
            b[s + k++] = byte;
            limit = 0;
            if (byte == 0) nul = true;
        }
        if ((code & kStInput) || nul) {
            ps_write16(b, ps + 6, static_cast<u16>(state));
            return k;
        }
        tab = table(state);
        if (!tab || k > 2 || ++limit > 0xFEF) break;
        if (!entry(tab, w, &code)) return kMbError;
    }
    return fail();
}

} // namespace pspocket::hle::libc
