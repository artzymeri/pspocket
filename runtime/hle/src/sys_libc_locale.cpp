// sys_libc's locale (D-906): setlocale and localeconv as the SDK's C library has them, its locale objects (_Clocale,
// _Locale, _Loctab, _Times) in its layouts (libc_locale.hpp), and Dinkumware's locale internals, under sys_libc and
// sys_libm (the variables under sys_libc only, as RPCS3 lists them).
//
// Read from a game's statically linked copy of the SDK's library (Assassin's Creed's): the SDK's setlocale knows the
// "C" locale alone (a query or "C" answers "C", any other name NULL, whatever the category), localeconv is the
// variable _Locale, and the locale machinery Dinkumware's setlocale would use is linked but its file reader is not:
// _Findloc answers NULL (no locale file is ever read), so _Getloc finds "C" only. That machinery is the SDK's here
// (_Getloc, _Getnloc, _Getlname, _Getcloc, _Freeloc, _Defloc, _Skip, the global locale pointer); _Setloc and the
// locale-file functions (_Readloc, _Makeloc, _Makestab, _Makewct, _Locsum, _Locterm, _Locvar, _Nnl) have no copy in
// the game and follow Dinkumware's design as Plauger's book describes it, extended to the C99 keywords of the SDK's
// _Loctab; their syntax is pspocket's reading of it (said where they are). _Getlname and _Getnloc are the names given
// to the two category-mask helpers the SDK links beside setlocale.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_locale.hpp"
#include "libc_malloc.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }
u32 rd(PPUContext& ctx, u32 a) { return ps_read32(gb(ctx), a); }
void wr(PPUContext& ctx, u32 a, u32 v) { ps_write32(gb(ctx), a, v); }

// Another function of sys_libc (its stdio, its locks), called as guest code would call it.
u64 call_libc(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find("sys_libc", recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "sys_libc::{} is not registered: the locale functions call it", name);
    std::array<u64, 8> saved{};
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    const u64 sp = ctx.gpr[1];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    ctx.gpr[1] = sp;
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

u32 var(std::string_view name) { return variable(recomp::nid_of(name)); }

// ------------------------------------------------------------------------------------------------- the variables

// Dinkumware's _Locitem codes (the third word of a _Loctab entry).
enum LocCode : u32 { kLGString = 0, kLName, kLNote, kLSet, kLState, kLString, kLTable, kLValue, kLWctype };

struct LocItem {
    std::string_view name;
    u32 offset;
    LocCode code;
};

// The SDK's _Loctab: each locale-file keyword with its _Linfo field and kind, in the SDK's order.
constexpr std::array<LocItem, 64> kLoctab = {{
    {"LOCALE", 0, kLName},
    {"NOTE", 0, kLNote},
    {"SET", 0, kLSet},
    {"collate", li::kCostate, kLState},
    {"wcollate", li::kWCostate, kLState},
    {"ctype", li::kCtype, kLTable},
    {"tolower", li::kTolotab, kLTable},
    {"toupper", li::kTouptab, kLTable},
    {"mb_cur_max", li::kMbcurmax, kLValue},
    {"mbtowc", li::kMbstate, kLState},
    {"wctomb", li::kWcstate, kLState},
    {"wctrans", li::kWctrans, kLWctype},
    {"wctype", li::kWctype, kLWctype},
    {"currency_symbol", li::kLconv + lcv::kCurrencySymbol, kLString},
    {"int_curr_symbol", li::kLconv + lcv::kIntCurrSymbol, kLString},
    {"mon_decimal_point", li::kLconv + lcv::kMonDecimalPoint, kLString},
    {"mon_grouping", li::kLconv + lcv::kMonGrouping, kLGString},
    {"mon_thousands_sep", li::kLconv + lcv::kMonThousandsSep, kLString},
    {"negative_sign", li::kLconv + lcv::kNegativeSign, kLString},
    {"positive_sign", li::kLconv + lcv::kPositiveSign, kLString},
    {"frac_digits", li::kLconv + lcv::kFracDigits, kLValue},
    {"n_cs_precedes", li::kLconv + lcv::kFracDigits + 1, kLValue},
    {"n_sep_by_space", li::kLconv + lcv::kFracDigits + 2, kLValue},
    {"n_sign_posn", li::kLconv + lcv::kFracDigits + 3, kLValue},
    {"p_cs_precedes", li::kLconv + lcv::kFracDigits + 4, kLValue},
    {"p_sep_by_space", li::kLconv + lcv::kFracDigits + 5, kLValue},
    {"p_sign_posn", li::kLconv + lcv::kFracDigits + 6, kLValue},
    {"int_frac_digits", li::kLconv + lcv::kFracDigits + 7, kLValue},
    {"int_n_cs_precedes", li::kLconv + lcv::kFracDigits + 8, kLValue},
    {"int_n_sep_by_space", li::kLconv + lcv::kFracDigits + 9, kLValue},
    {"int_n_sign_posn", li::kLconv + lcv::kFracDigits + 10, kLValue},
    {"int_p_cs_precedes", li::kLconv + lcv::kFracDigits + 11, kLValue},
    {"int_p_sep_by_space", li::kLconv + lcv::kFracDigits + 12, kLValue},
    {"int_p_sign_posn", li::kLconv + lcv::kFracDigits + 13, kLValue},
    {"decimal_point", li::kLconv + lcv::kDecimalPoint, kLString},
    {"grouping", li::kLconv + lcv::kGrouping, kLGString},
    {"thousands_sep", li::kLconv + lcv::kThousandsSep, kLString},
    {"frac_grouping", li::kLconv + lcv::kFracGrouping, kLGString},
    {"frac_sep", li::kLconv + lcv::kFracSep, kLString},
    {"false", li::kLconv + lcv::kFalse, kLString},
    {"true", li::kLconv + lcv::kTrue, kLString},
    {"am_pm", li::kTimes + ti::kAmPm, kLString},
    {"days", li::kTimes + ti::kDays, kLString},
    {"abday", li::kTimes + ti::kAbday, kLString},
    {"day", li::kTimes + ti::kDay, kLString},
    {"months", li::kTimes + ti::kMonths, kLString},
    {"abmon", li::kTimes + ti::kAbmon, kLString},
    {"mon", li::kTimes + ti::kMon, kLString},
    {"time_formats", li::kTimes + ti::kFormats, kLString},
    {"d_t_fmt", li::kTimes + ti::kDTFmt, kLString},
    {"d_fmt", li::kTimes + ti::kDFmt, kLString},
    {"t_fmt", li::kTimes + ti::kTFmt, kLString},
    {"t_fmt_ampm", li::kTimes + ti::kTFmtAmpm, kLString},
    {"era_time_formats", li::kTimes + ti::kEraFormats, kLString},
    {"era_d_t_fmt", li::kTimes + ti::kEraDTFmt, kLString},
    {"era_d_fmt", li::kTimes + ti::kEraDFmt, kLString},
    {"era_t_fmt", li::kTimes + ti::kEraTFmt, kLString},
    {"era_t_fmt_ampm", li::kTimes + ti::kEraTFmtAmpm, kLString},
    {"era", li::kTimes + ti::kEra, kLString},
    {"alt_digits", li::kTimes + ti::kAltDigits, kLString},
    {"dst_rules", li::kTimes + ti::kDstRules, kLString},
    {"time_zone", li::kTimes + ti::kTzone, kLString},
    {"no", li::kLconv + lcv::kNo, kLString},
    {"yes", li::kLconv + lcv::kYes, kLString},
}};

static_assert(kLoctab.back().name == "yes");
constexpr u32 kLoctabEntries = 12 * (kLoctab.size() + 1);

// _Loctab: the 64 entries {name, offset, code} and an empty one, the names after them.
void loctab_init(u8* b, u32 a)
{
    u32 text = a + kLoctabEntries;
    for (u32 i = 0; i < kLoctab.size(); i++) {
        const LocItem& q = kLoctab[i];
        ps_write32(b, a + 12 * i, text);
        ps_write32(b, a + 12 * i + 4, q.offset);
        ps_write32(b, a + 12 * i + 8, q.code);
        std::memcpy(b + text, q.name.data(), q.name.size());
        b[text + q.name.size()] = 0;
        text += static_cast<u32>(q.name.size()) + 1;
    }
}

constexpr u32 loctab_bytes()
{
    u32 n = kLoctabEntries;
    for (const LocItem& q : kLoctab) n += static_cast<u32>(q.name.size()) + 1;
    return (n + 7) & ~7u;
}

// _Locale, the "C" locale's struct lconv as the SDK's: every string "" but decimal_point "." and the false and true
// names, the fourteen chars CHAR_MAX (127: the SDK's library has a signed char).
void lconv_init(u8* b, u32 a)
{
    const u32 empty = a + lcv::kSize, point = empty + 1, no = point + 2, yes = no + 6;
    b[empty] = 0;
    std::memcpy(b + point, ".", 2);
    std::memcpy(b + no, "false", 6);
    std::memcpy(b + yes, "true", 5);
    for (u32 i = 0; i < 7; i++) ps_write32(b, a + 4 * i, empty);
    std::memset(b + a + lcv::kFracDigits, 0x7F, 14);
    ps_write32(b, a + lcv::kDecimalPoint, point);
    for (const u32 off : {lcv::kGrouping, lcv::kThousandsSep, lcv::kFracGrouping, lcv::kFracSep, lcv::kNo, lcv::kYes})
        ps_write32(b, a + off, empty);
    ps_write32(b, a + lcv::kFalse, no);
    ps_write32(b, a + lcv::kTrue, yes);
}

// _Times, the current locale's time names: the "C" locale's.
void times_init(u8* b, u32 a) { lay_c_times(b, a, a + ti::kSize); }

// _Clocale, the "C" locale's _Linfo: named "C" (its name the "C" setlocale answers), the rest filled the first time
// _Getcloc is asked, as the SDK's.
void clocale_init(u8* b, u32 a)
{
    ps_write32(b, a + li::kName, a + li::kSize);
    std::memcpy(b + a + li::kSize, "C", 2);
}

u32 clocale(PPUContext&) { return var("_Clocale"); }
u32 c_name(PPUContext& ctx)
{
    const u32 c = clocale(ctx);
    return c ? rd(ctx, c + li::kName) : 0;
}

// ------------------------------------------------------------------------------------------------- setlocale

// setlocale(int category, const char *locale): the SDK's knows "C" alone. NULL or "C" answers "C"; any other name
// ("", "POSIX", a composite name, a locale file's) NULL; the category is not looked at.
u32 setlocale_(PPUContext& ctx, s32, u32 name)
{
    const u32 c = c_name(ctx);
    if (!name) return c;
    const auto n = read_string(thread_of(ctx), name, 2);
    if (!n) {
        refuse(ctx, std::format("setlocale: the name at {:#x} runs out of guest memory", name));
        return 0;
    }
    return *n == "C" ? c : 0;
}

// localeconv: the variable _Locale.
u32 localeconv_(PPUContext&) { return var("_Locale"); }

// const char *_Defloc(void): the default locale's name, "C" (no environment on the PS3).
u32 _Defloc_(PPUContext& ctx) { return c_name(ctx); }

// _Linfo *_Findloc(const char *name, size_t n): the SDK's reads no locale file: NULL.
u32 _Findloc_(PPUContext&, u32, u32) { return 0; }

// The library's locale lock (the SDK's _Getloc takes system lock 0, the C++ library's locale lock).
struct SysLock {
    PPUContext& ctx;
    explicit SysLock(PPUContext& c) : ctx(c) { call_libc(ctx, "_Locksyslock", {0}); }
    ~SysLock() { call_libc(ctx, "_Unlocksyslock", {0}); }
    SysLock(const SysLock&) = delete;
    SysLock& operator=(const SysLock&) = delete;
};

// _Linfo *_Getloc(const char *nmcat, const char *lname): the locale named for category nmcat ("collate:" ...
// "messages:", or "" for the whole) in lname, a name or "name;category:name;...": the plain component, overridden by
// the category's own; looked up in the library's list from _Clocale, then by _Findloc (NULL). NULL when lname names
// nothing for it.
u32 getloc(PPUContext& ctx, const std::string& cat, const std::string& name)
{
    std::string_view found;
    bool have = false;
    usize at = 0;
    for (;;) {
        const usize n = name.find_first_of(":;", at) == std::string::npos ? name.size() - at
                                                                           : name.find_first_of(":;", at) - at;
        const usize end = at + n;
        const char c = end < name.size() ? name[end] : '\0';
        if (c == ':') {
            // "category:name": the category's own when it is this one.
            const std::string_view head(name.data() + at, n + 1);
            if (cat.compare(0, n + 1, head) == 0 && cat.size() >= n + 1) {
                const usize v = end + 1;
                const usize vn = name.find(';', v) == std::string::npos ? name.size() - v : name.find(';', v) - v;
                found = std::string_view(name.data() + v, vn);
                have = true;
                break;
            }
            const usize semi = name.find(';', end + 1);
            if (semi == std::string::npos) break;
            at = semi + 1;
            continue;
        }
        if (n != 0 && !have) {
            found = std::string_view(name.data() + at, n);
            have = true;
        }
        if (c == '\0') break;
        at = end + 1;
    }
    if (!have) return 0;
    {
        SysLock lock(ctx);
        lv2::PpuThread& t = thread_of(ctx);
        for (u32 p = clocale(ctx); p; p = rd(ctx, p + li::kNext)) {
            if (!guest_readable(t, p, li::kSize)) break;
            const auto pn = read_string(t, rd(ctx, p + li::kName), found.size() + 1);
            if (pn && *pn == found) return p;
        }
    }
    return 0; // _Findloc: no locale files
}

u32 _Getloc_(PPUContext& ctx, u32 nmcat, u32 lname)
{
    const auto cat = read_string(thread_of(ctx), nmcat, 64);
    const auto name = read_string(thread_of(ctx), lname);
    if (!cat || !name) {
        refuse(ctx, "_Getloc: a name runs out of guest memory");
        return 0;
    }
    return getloc(ctx, *cat, *name);
}

// The category names Dinkumware's composite names use, for _LC_COLLATE (1) to _LC_MESSAGES (6).
constexpr std::array<std::string_view, 6> kCatNames = {"collate:", "ctype:", "monetary:", "numeric:", "time:",
    "messages:"};

u32 catmask(u32 i) { return (1u << i) >> 1; }

// int _Getnloc(_Linfo *pcats[7], int mask, const char *lname): the locales lname names for the categories of mask
// (Dinkumware's _M_COLLATE 1 ... _M_MESSAGES 32), into pcats[1..6], and for the whole (pcats[0]) when mask is all of
// them; "" is the default locale. -1 for a mask past them or a category lname names nothing for (the categories
// before it already set, as the SDK's leaves them), else 1 when one changed, 0 when none did.
s32 _Getnloc_(PPUContext& ctx, u32 pcats, s32 mask, u32 lname)
{
    if ((static_cast<u32>(mask) & 0x3F) != static_cast<u32>(mask)) return -1;
    if (!range_ok(ctx, "_Getnloc", pcats, 28)) return -1;
    auto name = read_string(thread_of(ctx), lname);
    if (!name) {
        refuse(ctx, std::format("_Getnloc: the name at {:#x} runs out of guest memory", lname));
        return -1;
    }
    if (name->empty()) name = "C";
    s32 changed = 0;
    for (u32 i = 1; i <= 6; i++) {
        if (!(catmask(i) & static_cast<u32>(mask))) continue;
        const u32 p = getloc(ctx, std::string(kCatNames[i - 1]), *name);
        if (!p) return -1;
        if (rd(ctx, pcats + 4 * i) != p) changed = 1;
        wr(ctx, pcats + 4 * i, p);
    }
    if (mask == 0x3F) {
        const u32 p = getloc(ctx, "", *name);
        if (p && rd(ctx, pcats) != p) {
            changed = 1;
            wr(ctx, pcats, p);
        }
    }
    return changed;
}

// const char *_Getlname(_Linfo *pcats[7], int mask, char **pname): the name of the locale pcats holds for mask: the
// whole one's name when every category of mask is it (*pname freed and cleared), else "name;category:name;..." (or
// "category:name;..." when none is) in a new malloc'd *pname, the old freed. NULL when mask has no category or
// malloc fails.
u32 _Getlname_(PPUContext& ctx, u32 pcats, s32 mask, u32 pname)
{
    if (!range_ok(ctx, "_Getlname", pcats, 28) || !range_ok(ctx, "_Getlname", pname, 4)) return 0;
    lv2::PpuThread& t = thread_of(ctx);
    const u32 p0 = rd(ctx, pcats);
    const auto name_of = [&](u32 p) -> std::string {
        if (!guest_readable(t, p, 4)) return {};
        return read_string(t, rd(ctx, p + li::kName)).value_or("");
    };
    bool whole = false;
    u32 n = 0;
    for (u32 i = 1; i <= 6; i++) {
        if (!(catmask(i) & static_cast<u32>(mask))) continue;
        if (rd(ctx, pcats + 4 * i) == p0) whole = true;
        else n++;
    }
    if (n == 0) {
        lib_free(ctx, rd(ctx, pname), "_Getlname");
        wr(ctx, pname, 0);
        return whole && guest_readable(t, p0, 4) ? rd(ctx, p0 + li::kName) : 0;
    }
    std::string s = whole ? name_of(p0) : std::string();
    bool sep = whole;
    for (u32 i = 1; i <= 6; i++) {
        const u32 p = rd(ctx, pcats + 4 * i);
        if (!(catmask(i) & static_cast<u32>(mask)) || p == p0) continue;
        if (sep) s += ';';
        s += kCatNames[i - 1];
        s += name_of(p);
        sep = true;
    }
    const u32 buf = lib_malloc(ctx, static_cast<u32>(s.size()) + 1);
    if (!buf) return 0;
    lib_free(ctx, rd(ctx, pname), "_Getlname");
    wr(ctx, pname, buf);
    write_string(t, buf, s);
    return buf;
}

// The library's once-made state.
struct LocaleState {
    std::mutex mutex;
    bool cloc_made = false;
    u32 vars[26] = {};
    bool vars_set = false;
    u32 index = 0, value = 0; // $@ and $$ while a table line is applied
};

LocaleState& lstate() { return HleProcess::current()->state<LocaleState>(); }

// _Linfo *_Getcloc(void): _Clocale, filled the first time from the library's current tables (the calling thread's
// state tables, the ctype tables, MB_CUR_MAX 3, the wide class table, _Locale and _Times), as the SDK's.
u32 _Getcloc_(PPUContext& ctx)
{
    const u32 c = clocale(ctx);
    if (!c) return 0;
    LocaleState& s = lstate();
    std::lock_guard l(s.mutex);
    if (!s.cloc_made) {
        s.cloc_made = true;
        u8* b = gb(ctx);
        const auto copy = [&](u32 to, u32 from, u32 n) {
            if (from) std::memmove(b + c + to, b + from, n);
        };
        copy(li::kCostate, co_tables(ctx), kStatab);
        copy(li::kWCostate, wco_tables(ctx), kStatab);
        wr(ctx, c + li::kCtype, rd(ctx, var("_Ctype")));
        wr(ctx, c + li::kTolotab, rd(ctx, var("_Tolotab")));
        wr(ctx, c + li::kTouptab, rd(ctx, var("_Touptab")));
        copy(li::kMbstate, mb_tables(ctx), kStatab);
        copy(li::kWcstate, wc_tables(ctx), kStatab);
        wr(ctx, c + li::kWctype, rd(ctx, var("_Wctype")));
        b[c + li::kMbcurmax] = 3;
        copy(li::kLconv, var("_Locale"), lcv::kSize);
        copy(li::kTimes, var("_Times"), ti::kSize);
    }
    return c;
}

// void *_Getgloballocale(void) / void _Setgloballocale(void *): the calling thread's global locale (the C++
// library's), a thread-local pointer in the SDK's.
u32 _Getgloballocale_(PPUContext& ctx)
{
    const u32 a = thread_slot(ctx, tb::kGlobalLocale);
    return a ? rd(ctx, a) : 0;
}

void _Setgloballocale_(PPUContext& ctx, u32 p)
{
    if (const u32 a = thread_slot(ctx, tb::kGlobalLocale)) wr(ctx, a, p);
}

// void _Freeloc(_Linfo *p): what p holds that the "C" locale does not, freed (p itself is not): its name and strings,
// its ctype tables, its own state tables (the ones marked made), its wide tables and their names; as the SDK's, by
// _Loctab's kinds.
void _Freeloc_(PPUContext& ctx, u32 p)
{
    const u32 c = clocale(ctx);
    if (!c || !range_ok(ctx, "_Freeloc", p, li::kSize)) return;
    u8* b = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    for (const LocItem& q : kLoctab) {
        const u32 mine = rd(ctx, p + q.offset), theirs = rd(ctx, c + q.offset);
        switch (q.code) {
        case kLGString:
        case kLName:
        case kLString:
            if (mine != theirs) lib_free(ctx, mine, "_Freeloc");
            break;
        case kLTable:
            if (mine != theirs) lib_free(ctx, mine - 2, "_Freeloc");
            break;
        case kLState:
            for (u32 k = 0; k < kNState; k++) {
                const u32 tab = rd(ctx, p + q.offset + 4 * k);
                if (tab && guest_readable(t, tab - 2, 2) && ps_read16(b, tab - 2) != 0) lib_free(ctx, tab - 2, "_Freeloc");
            }
            break;
        case kLWctype: {
            if (mine == theirs) break;
            if (!guest_readable(t, mine, 8)) break;
            if (!theirs || rd(ctx, mine) != rd(ctx, theirs)) lib_free(ctx, rd(ctx, mine), "_Freeloc");
            u32 n = 0;
            if (theirs)
                while (guest_readable(t, theirs + 8 * n, 4) && rd(ctx, theirs + 8 * n)) n++;
            for (u32 k = n; guest_readable(t, mine + 8 * k, 4) && rd(ctx, mine + 8 * k); k++)
                lib_free(ctx, rd(ctx, mine + 8 * k), "_Freeloc");
            lib_free(ctx, mine, "_Freeloc");
            break;
        }
        default: break;
        }
    }
}

// _Linfo *_Setloc(int cat, _Linfo *p): category cat (Dinkumware's _LC_COLLATE 1 ... _LC_MESSAGES 6; 0 all of them)
// of the library's current locale taken from p, p answered. No copy in the game's library: Plauger's _Setloc for the
// C99 categories: collation the calling thread's collation state tables; ctype the ctype tables, MB_CUR_MAX (at most
// MB_LEN_MAX), the thread's multibyte and wide state tables and the wide class and translation tables (when p has
// them); monetary and numeric _Locale's fields (decimal_point "." when p's is empty; false and true with the
// numeric ones); time _Times; messages _Locale's no and yes.
u32 _Setloc_(PPUContext& ctx, s32 cat, u32 p)
{
    if (!range_ok(ctx, "_Setloc", p, li::kSize)) return 0;
    if (cat < 0 || cat > 6) return p;
    u8* b = gb(ctx);
    const u32 lc = var("_Locale");
    const auto field = [&](u32 off) { return rd(ctx, p + off); };
    const bool all = cat == 0;
    if (all || cat == 1) {
        if (const u32 t = co_tables(ctx)) std::memmove(b + t, b + p + li::kCostate, kStatab);
        if (const u32 t = wco_tables(ctx)) std::memmove(b + t, b + p + li::kWCostate, kStatab);
    }
    if (all || cat == 2) {
        wr(ctx, var("_Ctype"), field(li::kCtype));
        wr(ctx, var("_Tolotab"), field(li::kTolotab));
        wr(ctx, var("_Touptab"), field(li::kTouptab));
        const s32 mbmax = static_cast<s8>(b[p + li::kMbcurmax]);
        b[var("_Mbcurmax")] = static_cast<u8>(std::min(mbmax, 8)); // MB_LEN_MAX (_MBMAX)
        if (const u32 t = mb_tables(ctx)) std::memmove(b + t, b + p + li::kMbstate, kStatab);
        if (const u32 t = wc_tables(ctx)) std::memmove(b + t, b + p + li::kWcstate, kStatab);
        if (field(li::kWctype)) wr(ctx, var("_Wctype"), field(li::kWctype));
        if (field(li::kWctrans)) wr(ctx, var("_Wctrans"), field(li::kWctrans));
    }
    if (all || cat == 3) {
        for (u32 off = lcv::kCurrencySymbol; off <= lcv::kPositiveSign; off += 4) wr(ctx, lc + off, field(li::kLconv + off));
        std::memmove(b + lc + lcv::kFracDigits, b + p + li::kLconv + lcv::kFracDigits, 14);
    }
    if (all || cat == 4) {
        const u32 dp = field(li::kLconv + lcv::kDecimalPoint);
        const u32 point = lc + lcv::kSize + 1; // _Locale's own "."
        wr(ctx, lc + lcv::kDecimalPoint, guest_readable(thread_of(ctx), dp, 1) && b[dp] ? dp : point);
        for (const u32 off : {lcv::kGrouping, lcv::kThousandsSep, lcv::kFracGrouping, lcv::kFracSep, lcv::kFalse,
                 lcv::kTrue})
            wr(ctx, lc + off, field(li::kLconv + off));
    }
    if (all || cat == 5) std::memmove(b + var("_Times"), b + p + li::kTimes, ti::kSize);
    if (all || cat == 6) {
        wr(ctx, lc + lcv::kNo, field(li::kLconv + lcv::kNo));
        wr(ctx, lc + lcv::kYes, field(li::kLconv + lcv::kYes));
    }
    return p;
}

// ------------------------------------------------------------------------------------------------- locale files

// const char *_Skip(const char *s): past the character at s and the blanks after it (s itself at the NUL), as the
// SDK's.
u32 skip(const std::string& s, u32 base, usize i)
{
    if (i >= s.size()) return base + static_cast<u32>(i);
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return base + static_cast<u32>(i);
}

usize skip_at(const std::string& s, usize i)
{
    if (i >= s.size()) return i;
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

u32 _Skip_(PPUContext& ctx, u32 s)
{
    const auto t = read_string(thread_of(ctx), s);
    if (!t) {
        refuse(ctx, std::format("_Skip: the string at {:#x} runs out of guest memory", s));
        return s;
    }
    return skip(*t, s, 0);
}

// The locale file's terms (no copy in the game's library; Plauger's design): a number (C's decimal, octal or hex), a
// character 'c', or $x: $^ UCHAR_MAX, $a $b $f $n $r $t $v the escapes, $@ the entry a table line sets and $$ its
// value, $0..$9 the state numbers of state table entries (n << 8), $A..$Z the variables SET gives (F, I, O and R start
// as the state table entry's fold, input, output and rotate bits). Signs before a term negate it.
bool term(LocaleState& s, const std::string& t, usize& i, u32& ans)
{
    bool neg = false;
    while (i < t.size() && (t[i] == '+' || t[i] == '-')) {
        if (t[i] == '-') neg = !neg;
        i = skip_at(t, i);
    }
    if (i >= t.size()) return false;
    u32 v = 0;
    const char c = t[i];
    if (c >= '0' && c <= '9') {
        const IntParse p = parse_int([&](u64 k) -> s64 { return i + k < t.size() ? static_cast<u8>(t[i + k]) : -1; }, 0);
        bool range = false;
        v = static_cast<u32>(int_result(p, 32, false, &range));
        i = skip_at(t, i + p.end - 1);
    } else if (c == '\'' && i + 2 < t.size() && t[i + 2] == '\'') {
        v = static_cast<u8>(t[i + 1]);
        i = skip_at(t, i + 2);
    } else if (c == '$' && i + 1 < t.size()) {
        const char d = t[i + 1];
        static constexpr std::string_view kEsc = "abfnrtv";
        static constexpr char kEscVal[] = {'\a', '\b', '\f', '\n', '\r', '\t', '\v'};
        if (d == '^') v = 255;
        else if (const usize e = kEsc.find(d); e != std::string_view::npos) v = static_cast<u8>(kEscVal[e]);
        else if (d == '@') v = s.index;
        else if (d == '$') v = s.value;
        else if (d >= '0' && d <= '9') v = static_cast<u32>(d - '0') << 8;
        else if (d >= 'A' && d <= 'Z') v = s.vars[d - 'A'];
        else return false;
        i = skip_at(t, i + 1);
    } else {
        return false;
    }
    ans = neg ? 0u - v : v;
    return true;
}

bool term_start(char c) { return c == '+' || c == '-' || c == '\'' || c == '$' || (c >= '0' && c <= '9'); }

// A sum: terms one after another (blanks between them), added.
bool sum(LocaleState& s, const std::string& t, usize& i, u32& ans)
{
    if (!term(s, t, i, ans)) return false;
    u32 v = 0;
    while (i < t.size() && term_start(t[i])) {
        if (!term(s, t, i, v)) return false;
        ans += v;
    }
    return true;
}

void vars_default(LocaleState& s)
{
    if (s.vars_set) return;
    s.vars_set = true;
    s.vars['F' - 'A'] = kStFold;
    s.vars['I' - 'A'] = kStInput;
    s.vars['O' - 'A'] = kStOutput;
    s.vars['R' - 'A'] = kStRotate;
}

// int _Locterm(const char **ps, unsigned long *ans): one term at *ps into *ans, *ps moved past it and its blanks; 0
// when there is none.
s32 _Locterm_(PPUContext& ctx, u32 ps, u32 ans)
{
    if (!range_ok(ctx, "_Locterm", ps, 4) || !range_ok(ctx, "_Locterm", ans, 4)) return 0;
    const u32 s = rd(ctx, ps);
    const auto t = read_string(thread_of(ctx), s);
    if (!t) return 0;
    LocaleState& st = lstate();
    std::lock_guard l(st.mutex);
    vars_default(st);
    usize i = 0;
    u32 v = 0;
    if (!term(st, *t, i, v)) return 0;
    wr(ctx, ans, v);
    wr(ctx, ps, s + static_cast<u32>(i));
    return 1;
}

// const char *_Locsum(const char *s, unsigned long *ans): the sum of the terms at s into *ans, past them; NULL when s
// has no term.
u32 _Locsum_(PPUContext& ctx, u32 s, u32 ans)
{
    if (!range_ok(ctx, "_Locsum", ans, 4)) return 0;
    const auto t = read_string(thread_of(ctx), s);
    if (!t) return 0;
    LocaleState& st = lstate();
    std::lock_guard l(st.mutex);
    vars_default(st);
    usize i = 0;
    u32 v = 0;
    if (!sum(st, *t, i, v)) return 0;
    wr(ctx, ans, v);
    return s + static_cast<u32>(i);
}

// int _Locvar(char ch, unsigned long val): the variable $ch (A..Z) set to val (SET's work); 0 for another letter.
s32 _Locvar_(PPUContext&, s32 ch, u32 val)
{
    const char c = static_cast<char>(ch);
    if (c < 'A' || c > 'Z') return 0;
    LocaleState& st = lstate();
    std::lock_guard l(st.mutex);
    vars_default(st);
    st.vars[c - 'A'] = val;
    return 1;
}

constexpr u32 kMaxLine = 256;

// int _Nnl(char *buf): the line in buf without its newline; 1 when it had one, 0 when not (a line too long for the
// buffer, or a file's last line). (pspocket's reading of the name: no copy in the game's library.)
s32 nnl(PPUContext& ctx, u32 buf)
{
    const auto t = read_string(thread_of(ctx), buf, kMaxLine);
    if (!t || t->empty() || t->back() != '\n') return 0;
    gb(ctx)[buf + t->size() - 1] = 0;
    return 1;
}

s32 _Nnl_(PPUContext& ctx, u32 buf) { return nnl(ctx, buf); }

// const _Locitem *_Readloc(FILE *lf, char *buf, const char **ps): the next line of the locale file lf (buf holds
// MAXLIN, 256, bytes; a line ending in '\' goes on on the next), skipping blank ones, whose first word (ended by a
// blank or a table line's '[') is one of _Loctab's: that entry, *ps at what follows the keyword. NULL at the end of the file, for a line too long or an
// unknown keyword.
u32 readloc(PPUContext& ctx, u32 lf, u32 buf, u32 ps, bool* at_end = nullptr)
{
    u8* b = gb(ctx);
    const u32 table = var("_Loctab");
    if (at_end) *at_end = false;
    for (;;) {
        u32 n = 0;
        for (;;) {
            if (!call_libc(ctx, "fgets", {buf + n, kMaxLine - n, lf})) {
                if (n == 0) {
                    if (at_end) *at_end = true;
                    return 0;
                }
                break; // the file ended inside a continued line: what is there is the line
            }
            const bool ended = nnl(ctx, buf + n) != 0;
            n += static_cast<u32>(std::strlen(reinterpret_cast<const char*>(b + buf + n)));
            if (!ended && !call_libc(ctx, "feof", {lf})) return 0; // too long
            if (ended && n > 0 && b[buf + n - 1] == '\\' && n + 1 < kMaxLine) {
                n--; // the line goes on
                b[buf + n] = 0;
                continue;
            }
            break;
        }
        const std::string line(reinterpret_cast<const char*>(b + buf), n);
        usize i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        const usize len = std::min(line.find_first_of(" \t[", i), line.size()) - i; // "ctype[...]": the keyword before its '['
        const std::string_view word(line.data() + i, len);
        for (u32 k = 0; k < kLoctab.size(); k++) {
            if (kLoctab[k].name != word) continue;
            usize at = i + len;
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) at++;
            if (ps) wr(ctx, ps, buf + static_cast<u32>(at));
            return table + 12 * k;
        }
        return 0;
    }
}

u32 _Readloc_(PPUContext& ctx, u32 lf, u32 buf, u32 ps)
{
    if (!range_ok(ctx, "_Readloc", buf, kMaxLine) || (ps && !range_ok(ctx, "_Readloc", ps, 4))) return 0;
    return readloc(ctx, lf, buf, ps);
}

// The _Loctab entry at a guest address.
const LocItem* item_of(PPUContext& ctx, u32 q)
{
    const u32 table = var("_Loctab");
    if (q < table || q >= table + 12 * kLoctab.size() || (q - table) % 12) {
        refuse(ctx, std::format("locale: {:#x} is no entry of _Loctab", q));
        return nullptr;
    }
    return &kLoctab[(q - table) / 12];
}

// One table line: "ctype[lo:hi] value" (lo..hi from -1, EOF, to 255; "[c]" one entry) for the ctype tables, or
// "mbtowc[state, lo:hi] value" for the state tables (state 0..15, entries 0..255). The value is a sum evaluated per
// entry with $@ the entry and $$ its old value. A table the locale shares with the "C" locale is copied first (a
// ctype table to 257 halfwords from EOF, the field at entry 0; a state table to 256 entries after a halfword marking
// it made, as the SDK's _Freeloc expects).
s32 makestab(PPUContext& ctx, u32 p, const LocItem& q, const std::string& t)
{
    u8* b = gb(ctx);
    lv2::PpuThread& th = thread_of(ctx);
    LocaleState& st = lstate();
    std::lock_guard l(st.mutex);
    vars_default(st);
    usize i = 0;
    if (i >= t.size() || t[i] != '[') return 0;
    i = skip_at(t, i);
    u32 state = 0;
    if (q.code == kLState) {
        if (!sum(st, t, i, state) || state >= kNState || i >= t.size() || t[i] != ',') return 0;
        i = skip_at(t, i);
    }
    u32 lo = 0, hi = 0;
    if (!sum(st, t, i, lo)) return 0;
    hi = lo;
    if (i < t.size() && t[i] == ':') {
        i = skip_at(t, i);
        if (!sum(st, t, i, hi)) return 0;
    }
    if (i >= t.size() || t[i] != ']') return 0;
    i = skip_at(t, i);
    const s32 first = q.code == kLState ? 0 : -1;
    const s32 slo = static_cast<s32>(lo), shi = static_cast<s32>(hi);
    if (slo < first || shi > 255 || slo > shi) return 0;
    u32 entries = 0; // the halfword of entry 0
    if (q.code == kLTable) {
        const u32 field = p + q.offset;
        const u32 cur = rd(ctx, field), c = rd(ctx, clocale(ctx) + q.offset);
        if (cur == 0 || cur == c) {
            const u32 made = lib_malloc(ctx, 2 * 257);
            if (!made) return 0;
            if (cur && guest_readable(th, cur - 2, 2 * 257)) std::memmove(b + made, b + cur - 2, 2 * 257);
            else std::memset(b + made, 0, 2 * 257);
            wr(ctx, field, made + 2);
        }
        entries = rd(ctx, field);
    } else {
        const u32 slot = p + q.offset + 4 * state;
        const u32 cur = rd(ctx, slot);
        if (!cur || !guest_readable(th, cur - 2, 2) || ps_read16(b, cur - 2) == 0) {
            const u32 made = lib_malloc(ctx, 2 * 257);
            if (!made) return 0;
            ps_write16(b, made, 1);
            if (cur && guest_readable(th, cur, 2 * 256)) std::memmove(b + made + 2, b + cur, 2 * 256);
            else std::memset(b + made + 2, 0, 2 * 256);
            wr(ctx, slot, made + 2);
        }
        entries = rd(ctx, slot);
    }
    for (s32 k = slo; k <= shi; k++) {
        const u32 at = entries + static_cast<u32>(2 * k);
        st.index = static_cast<u32>(k);
        st.value = static_cast<u32>(static_cast<s32>(static_cast<s16>(ps_read16(b, at))));
        usize j = i;
        u32 v = 0;
        if (!sum(st, t, j, v)) return 0;
        ps_write16(b, at, static_cast<u16>(v));
    }
    return 1;
}

s32 _Makestab_(PPUContext& ctx, u32 p, u32 q, u32 s)
{
    const LocItem* item = item_of(ctx, q);
    const auto t = read_string(thread_of(ctx), s);
    if (!item || !t || !range_ok(ctx, "_Makestab", p, li::kSize)) return 0;
    if (item->code != kLTable && item->code != kLState) return 0;
    return makestab(ctx, p, *item, *t);
}

// One wide table line: "wctype name lo:hi value [lo:hi value ...]" (a class's ranges, value 1) or "wctrans name lo:hi
// delta ..." (a translation's ranges, the delta added), each a single term ("lo" alone for one character): the class or translation given, replacing a same-named one or
// added after the others. The table is the SDK's {ranges, 0}, {name, first range}... {NULL, end} with ranges of three
// halfwords {lo, hi, value}; the locale's copy keeps the "C" table's name pointers and mallocs its own names and
// ranges, as the SDK's _Freeloc expects.
s32 makewct(PPUContext& ctx, u32 p, const LocItem& q, const std::string& t)
{
    u8* b = gb(ctx);
    lv2::PpuThread& th = thread_of(ctx);
    LocaleState& st = lstate();
    std::unique_lock l(st.mutex);
    vars_default(st);
    usize i = 0;
    const usize nlen = std::min(t.find_first_of(" \t", i), t.size());
    if (nlen == 0) return 0;
    const std::string name = t.substr(0, nlen);
    i = nlen;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
    std::vector<std::array<u16, 3>> mine;
    while (i < t.size()) {
        u32 lo = 0, hi = 0, v = 0;
        if (!term(st, t, i, lo)) return 0;
        hi = lo;
        if (i < t.size() && t[i] == ':') {
            i = skip_at(t, i);
            if (!term(st, t, i, hi)) return 0;
        }
        if (!term(st, t, i, v)) return 0;
        mine.push_back({static_cast<u16>(lo), static_cast<u16>(hi), static_cast<u16>(v)});
    }
    l.unlock();
    const u32 field = p + q.offset;
    u32 cur = rd(ctx, field);
    if (!cur) cur = rd(ctx, var(q.offset == li::kWctype ? "_Wctype" : "_Wctrans"));
    const u32 c = rd(ctx, clocale(ctx) + q.offset);
    // The current classes: name pointers and ranges.
    struct Class {
        u32 name;
        std::vector<std::array<u16, 3>> ranges;
    };
    std::vector<Class> classes;
    if (cur && guest_readable(th, cur, 8)) {
        const u32 ranges = rd(ctx, cur);
        for (u32 k = 1; guest_readable(th, cur + 8 * k + 8, 8) && rd(ctx, cur + 8 * k); k++) {
            Class cl{rd(ctx, cur + 8 * k), {}};
            const u32 from = rd(ctx, cur + 8 * k + 4), to = rd(ctx, cur + 8 * k + 12);
            for (u32 r = from; r + 3 <= to; r += 3) {
                const u32 at = ranges + 2 * r;
                if (!guest_readable(th, at, 6)) break;
                cl.ranges.push_back({ps_read16(b, at), ps_read16(b, at + 2), ps_read16(b, at + 4)});
            }
            classes.push_back(std::move(cl));
        }
    }
    bool replaced = false;
    for (Class& cl : classes) {
        if (read_string(th, cl.name, 64).value_or("") == name) {
            cl.ranges = mine;
            replaced = true;
        }
    }
    if (!replaced) {
        const u32 nm = lib_malloc(ctx, static_cast<u32>(name.size()) + 1);
        if (!nm) return 0;
        write_string(th, nm, name);
        classes.push_back({nm, mine});
    }
    u32 total = 0;
    for (const Class& cl : classes) total += static_cast<u32>(cl.ranges.size());
    const u32 table = lib_malloc(ctx, 8 * static_cast<u32>(classes.size() + 2));
    const u32 ranges = lib_malloc(ctx, std::max<u32>(6 * total, 2));
    if (!table || !ranges) {
        lib_free(ctx, table, "_Makewct");
        lib_free(ctx, ranges, "_Makewct");
        return 0;
    }
    wr(ctx, table, ranges);
    wr(ctx, table + 4, 0);
    u32 r = 0;
    for (u32 k = 0; k < classes.size(); k++) {
        wr(ctx, table + 8 * (k + 1), classes[k].name);
        wr(ctx, table + 8 * (k + 1) + 4, 3 * r);
        for (const auto& rg : classes[k].ranges) {
            for (u32 h = 0; h < 3; h++) ps_write16(b, ranges + 6 * r + 2 * h, rg[h]);
            r++;
        }
    }
    wr(ctx, table + 8 * (static_cast<u32>(classes.size()) + 1), 0);
    wr(ctx, table + 8 * (static_cast<u32>(classes.size()) + 1) + 4, 3 * r);
    // The locale's old copy (not the "C" table's) goes: its ranges and the array; its names live on in the new one.
    if (rd(ctx, field) && rd(ctx, field) != c) {
        const u32 old = rd(ctx, field);
        if (!c || rd(ctx, old) != rd(ctx, c)) lib_free(ctx, rd(ctx, old), "_Makewct");
        lib_free(ctx, old, "_Makewct");
    }
    wr(ctx, field, table);
    return 1;
}

s32 _Makewct_(PPUContext& ctx, u32 p, u32 q, u32 s)
{
    const LocItem* item = item_of(ctx, q);
    const auto t = read_string(thread_of(ctx), s);
    if (!item || !t || !range_ok(ctx, "_Makewct", p, li::kSize) || item->code != kLWctype) return 0;
    return makewct(ctx, p, *item, *t);
}

// A string line's text: double-quoted parts (C's escapes: \n \t \\ \" \ooo and the like) and terms (each a
// character), one after another. A grouping string's characters are its digits' values (0-9, a-f; ^ is CHAR_MAX), as
// lconv's grouping wants them.
bool string_text(LocaleState& st, const std::string& t, bool grouping, std::string& out)
{
    usize i = 0;
    while (i < t.size()) {
        if (t[i] == '"') {
            for (i++; i < t.size() && t[i] != '"'; i++) {
                char c = t[i];
                if (c == '\\' && i + 1 < t.size()) {
                    c = t[++i];
                    static constexpr std::string_view kEsc = "abfnrtv";
                    static constexpr char kEscVal[] = {'\a', '\b', '\f', '\n', '\r', '\t', '\v'};
                    if (const usize e = kEsc.find(c); e != std::string_view::npos) c = kEscVal[e];
                    else if (c >= '0' && c <= '7') {
                        u32 v = 0;
                        for (u32 k = 0; k < 3 && i < t.size() && t[i] >= '0' && t[i] <= '7'; k++, i++) v = v * 8 + static_cast<u32>(t[i] - '0');
                        i--;
                        c = static_cast<char>(v);
                    }
                }
                if (grouping) {
                    static constexpr std::string_view kDigits = "0123456789abcdef^";
                    const usize d = kDigits.find(c);
                    if (d == std::string_view::npos) return false;
                    c = static_cast<char>(d == 16 ? 0x7F : d);
                }
                out += c;
            }
            if (i >= t.size()) return false;
            i = skip_at(t, i);
        } else {
            u32 v = 0;
            if (!term(st, t, i, v)) return false;
            out += static_cast<char>(v);
        }
    }
    return true;
}

// int _Makeloc(FILE *lf, char *buf, _Linfo *p): p filled from the locale file's lines up to the next LOCALE line (or
// its end): strings and grouping strings (malloc'd, a string the locale had of its own freed), values, SET's
// variables, table and wide table lines; NOTE lines are comments. 1 when it is complete, 0 on a line it cannot use.
s32 _Makeloc_(PPUContext& ctx, u32 lf, u32 buf, u32 p)
{
    if (!range_ok(ctx, "_Makeloc", buf, kMaxLine) || !range_ok(ctx, "_Makeloc", p, li::kSize)) return 0;
    lv2::PpuThread& th = thread_of(ctx);
    StackVar ps(ctx, gb(ctx), 8);
    for (;;) {
        bool at_end = false;
        const u32 qa = readloc(ctx, lf, buf, ps.addr(), &at_end);
        if (!qa) return at_end ? 1 : 0;
        const LocItem* q = item_of(ctx, qa);
        if (!q) return 0;
        const std::string s = read_string(th, rd(ctx, ps.addr()), kMaxLine).value_or("");
        switch (q->code) {
        case kLName: return 1;
        case kLNote: break;
        case kLSet: {
            if (s.size() < 2 || s[0] < 'A' || s[0] > 'Z' || (s[1] != ' ' && s[1] != '\t')) return 0;
            LocaleState& st = lstate();
            std::lock_guard l(st.mutex);
            vars_default(st);
            usize i = skip_at(s, 0);
            u32 v = 0;
            if (!sum(st, s, i, v) || i != s.size()) return 0;
            st.vars[s[0] - 'A'] = v;
            break;
        }
        case kLGString:
        case kLString: {
            std::string text;
            {
                LocaleState& st = lstate();
                std::lock_guard l(st.mutex);
                vars_default(st);
                if (!string_text(st, s, q->code == kLGString, text)) return 0;
            }
            const u32 m = lib_malloc(ctx, static_cast<u32>(text.size()) + 1);
            if (!m) return 0;
            std::memcpy(gb(ctx) + m, text.data(), text.size());
            gb(ctx)[m + text.size()] = 0;
            const u32 old = rd(ctx, p + q->offset);
            if (old && old != rd(ctx, clocale(ctx) + q->offset)) lib_free(ctx, old, "_Makeloc");
            wr(ctx, p + q->offset, m);
            break;
        }
        case kLValue: {
            LocaleState& st = lstate();
            std::lock_guard l(st.mutex);
            vars_default(st);
            usize i = 0;
            u32 v = 0;
            if (!sum(st, s, i, v) || i != s.size()) return 0;
            gb(ctx)[p + q->offset] = static_cast<u8>(v);
            break;
        }
        case kLState:
        case kLTable:
            if (!makestab(ctx, p, *q, s)) return 0;
            break;
        case kLWctype:
            if (!makewct(ctx, p, *q, s)) return 0;
            break;
        }
    }
}

} // namespace

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_locale(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(setlocale), LIBC(localeconv), LIBC(_Defloc), LIBC(_Findloc), LIBC(_Freeloc), LIBC(_Getcloc),
            LIBC(_Getgloballocale), LIBC(_Setgloballocale), LIBC(_Getlname), LIBC(_Getloc), LIBC(_Getnloc),
            LIBC(_Locsum), LIBC(_Locterm), LIBC(_Locvar), LIBC(_Makeloc), LIBC(_Makestab), LIBC(_Makewct),
            LIBC(_Nnl), LIBC(_Readloc), LIBC(_Setloc), LIBC(_Skip),
        });
    }
    r.add_variables("sys_libc", {
        Variable{{}, "_Clocale", recomp::nid_of("_Clocale"), li::kSize + 4, 8, &clocale_init},
        Variable{{}, "_Locale", recomp::nid_of("_Locale"), lcv::kSize + 16, 8, &lconv_init},
        Variable{{}, "_Loctab", recomp::nid_of("_Loctab"), loctab_bytes(), 8, &loctab_init},
        Variable{{}, "_Times", recomp::nid_of("_Times"), ti::kSize + 340, 8, &times_init},
    });
}
#undef LIBC

} // namespace pspocket::hle
