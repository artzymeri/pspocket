#pragma once

// sys_libstdcxx, the locale (D-560): std::locale, its _Locimp, locale::facet and locale::id, _Locinfo, the global and
// classic locales, the lazy facets (_Register, _Fac_tidy) and use_facet. Included by stdcxx_streams.cpp only.
//
// Layouts (the SDK's, VC8's generation):
// - locale {_Locimp *_Ptr}; locale::id {size_t _Id}, numbered from 1 at first use.
// - locale::facet {vptr, size_t _Refs} (8 bytes); its vtable [~D1, ~D0]. A _Refs of (size_t)-1 is never counted.
// - _Locimp : facet {facet **_Facetvec (+8, from malloc), size_t _Facetcount (+12), int _Catmask (+16), bool _Xparent
//   (+20), string _Name (+24)}: 52 bytes.
// - _Locinfo {_Lockit (+0, the lock's kind), _Tinfo _Times (+4, 84 bytes: the "C" locale's time names, Dinkumware's
//   21 pointers), string _Oldlocname (+88), string _Newlocname (+116)}: 144 bytes. It holds the locale lock (0) while
//   it lives.
// - _Ctypevec {const short *_Table, *_Tolotab, *_Touptab; int _Delfl} (16 bytes), _Cvtvec {_Statab *_Mbstate,
//   *_Wcstate} and _Collvec {_Statab *_Costate, *_WCostate} (8 bytes each): sys_libc's tables and state pointers.
// The global locale is per thread in the SDK (its pointer is a thread-local variable): each thread's first
// locale::_Init makes a "C" _Locimp for it and makes it the classic one (_Clocptr and classic()'s object), as the SDK's
// does. The C++ library's own setlocale knows "C" only: a name other than "C" is a bad name.
//
// The facets are the facets part's (D-561), reached through the registry: each standard facet type's
// `static size_t _Getcat(const locale::facet **)` (which makes the facet from the "C" _Locinfo when given a null
// pointer to fill, and answers the facet's category) and, when that part exports it, its `static locale::id id` as a
// variable (otherwise this part keeps an id object of its own for the type). _Makeloc makes a facet with _Getcat:
// "C" is the only locale there is, so _Getcat's facet is the one `new Facet(_Locinfo("C"))` would give.

#include "stdcxx.hpp"

namespace pspocket::hle::stdcxx {

inline constexpr u32 kFacetRefs = 4;
inline constexpr u32 kImpFacetvec = 8, kImpFacetcount = 12, kImpCatmask = 16, kImpXparent = 20, kImpName = 24,
                     kImpBytes = 52;
inline constexpr u32 kInfoTimes = 4, kInfoOld = 88, kInfoNew = 116, kInfoBytes = 144, kTinfoBytes = 84;
inline constexpr u32 kCatAll = 63; // Dinkumware's _M_ALL: collate 1, ctype 2, monetary 4, numeric 8, time 16, messages 32

// Dinkumware's _CATMASK(n) for the category numbers _Getcat answers (_LC_COLLATE 1 .. _LC_MESSAGES 6).
inline u32 catmask(s32 n) { return n <= 0 || n > 6 ? 0 : (1u << n) >> 1; }

// The library's locale state, per process.
struct LocaleState {
    std::mutex mutex;
    u32 clocptr = 0;                     // _Locimp::_Clocptr
    u32 id_cnt = 0;                      // locale::id::_Id_cnt
    std::vector<u32> lazy;               // _Fac_head: the facets _Register queued, newest last
    bool tidy_registered = false;
    std::unordered_map<std::string_view, u32> psave; // use_facet's _Facetptr<_Facet>::_Psave, by facet type
};

inline LocaleState& lstate() { return HleProcess::current()->state<LocaleState>(); }

// ------------------------------------------------------------------------------------------------- facets

inline u32 refs_of(const G& g, u32 f) { return g.r32(f + kFacetRefs); }

// facet::_Incref: under the locale lock, _Refs + 1 unless it is (size_t)-1.
inline void incref(PPUContext& ctx, u32 f)
{
    G g(ctx);
    if (!g.need("locale::facet::_Incref", f, 8)) return;
    Lockit lock(ctx, kLockLocale);
    const u32 r = refs_of(g, f);
    if (r != 0xFFFFFFFFu) g.w32(f + kFacetRefs, r + 1);
}

// facet::_Decref: _Refs - 1 unless it is 0 or (size_t)-1; the facet when its _Refs is then 0 (to delete), else 0.
inline u32 decref(PPUContext& ctx, u32 f)
{
    G g(ctx);
    if (!g.need("locale::facet::_Decref", f, 8)) return 0;
    Lockit lock(ctx, kLockLocale);
    u32 r = refs_of(g, f);
    if (r - 1 <= 0xFFFFFFFDu) g.w32(f + kFacetRefs, r = r - 1);
    return r == 0 ? f : 0;
}

// delete p: its deleting destructor (vtable slot 1).
inline void delete_object(PPUContext& ctx, u32 p)
{
    if (p) vcall(ctx, p, 1, {p});
}

// delete f->_Decref()
inline void release(PPUContext& ctx, u32 f)
{
    if (f) delete_object(ctx, decref(ctx, f));
}

void facet_incref_(PPUContext& ctx, u32 self) { incref(ctx, self); }
u32 facet_decref_(PPUContext& ctx, u32 self) { return decref(ctx, self); }

void fac_tidy_(PPUContext& ctx);

// facet::_Register: queues a lazy facet for _Fac_tidy, which it registers with _Atexit the first time (the SDK's takes
// no lock here; this one guards its own list).
void facet_register_(PPUContext& ctx, u32 self)
{
    LocaleState& s = lstate();
    bool first = false;
    {
        std::lock_guard lock(s.mutex);
        first = !s.tidy_registered;
        s.tidy_registered = true;
        s.lazy.push_back(self);
    }
    if (first) libc_fn::atexit(ctx, {descriptor("_Fac_tidy")});
}

// _Fac_tidy: under the locale lock, every lazy facet released, newest first.
void fac_tidy_(PPUContext& ctx)
{
    Lockit lock(ctx, kLockLocale);
    for (;;) {
        u32 f = 0;
        {
            LocaleState& s = lstate();
            std::lock_guard l(s.mutex);
            if (s.lazy.empty()) break;
            f = s.lazy.back();
            s.lazy.pop_back();
        }
        release(ctx, f);
    }
}

// ~facet: D1 and D2 set the facet's vptr; D0 frees it too.
template <bool Deleting>
void facet_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("locale::facet::~facet", self, 8)) return;
    g.w32(self, vtable(ctx, "NSt6locale5facetE"));
    if (Deleting) op_delete(ctx, self);
}

// locale::id::operator size_t(): the id, given under the locale lock at first use (_Id_cnt + 1).
inline u32 id_value(PPUContext& ctx, u32 id)
{
    G g(ctx);
    if (!g.need("locale::id::operator size_t", id, 4)) return 0;
    if (const u32 v = g.r32(id)) return v;
    Lockit lock(ctx, kLockLocale);
    if (g.r32(id) == 0) {
        LocaleState& s = lstate();
        std::lock_guard l(s.mutex);
        g.w32(id, ++s.id_cnt);
    }
    return g.r32(id);
}

u32 id_value_(PPUContext& ctx, u32 self) { return id_value(ctx, self); }

// ------------------------------------------------------------------------------------------------- _Locimp

// The calling thread's global _Locimp, 0 before its first _Init: kept by the C library (its _Getgloballocale and
// _Setgloballocale, a thread-local pointer in the SDK's, D-906), as the SDK's C++ library keeps it.
inline u32 global_imp(PPUContext& ctx) { return static_cast<u32>(libc_fn::getgloballocale(ctx, {})); }

inline void set_global_imp(PPUContext& ctx, u32 imp) { libc_fn::setgloballocale(ctx, {imp}); }

// _Locimp(bool _Transparent): facet(1), no facets, no categories, named "*".
inline void imp_construct(PPUContext& ctx, u32 self, bool transparent)
{
    G g(ctx);
    g.w32(self, vtable(ctx, "NSt6locale7_LocimpE"));
    g.w32(self + kFacetRefs, 1);
    g.w32(self + kImpFacetvec, 0);
    g.w32(self + kImpFacetcount, 0);
    g.w32(self + kImpCatmask, 0);
    g.w8(self + kImpXparent, transparent ? 1 : 0);
    const GStr<1> name(g, self + kImpName);
    name.construct();
    name.set("*");
}

void imp_ctor_(PPUContext& ctx, u32 self, bool transparent)
{
    G g(ctx);
    if (g.need("locale::_Locimp::_Locimp", self, kImpBytes)) imp_construct(ctx, self, transparent);
}

// _Locimp::_New_Locimp(bool): operator new and the constructor.
inline u32 new_imp(PPUContext& ctx, bool transparent)
{
    const u32 p = op_new(ctx, kImpBytes);
    imp_construct(ctx, p, transparent);
    return p;
}

u32 makeloc(PPUContext& ctx, u32 lobj, s32 cat, u32 imp, u32 ploc);

// _Locimp(const _Locimp& _Right): a copy sharing _Right's facets; a copy of the classic locale gets all the standard
// facets made (_Makeloc of the "C" _Locinfo).
inline void imp_copy(PPUContext& ctx, u32 self, u32 right);

inline u32 new_imp_copy(PPUContext& ctx, u32 right)
{
    const u32 p = op_new(ctx, kImpBytes);
    imp_copy(ctx, p, right);
    return p;
}

void imp_copy_ctor_(PPUContext& ctx, u32 self, u32 right)
{
    G g(ctx);
    if (g.need("locale::_Locimp::_Locimp", self, kImpBytes) && g.need("locale::_Locimp::_Locimp", right, kImpBytes))
        imp_copy(ctx, self, right);
}

// ~_Locimp: under the locale lock every facet released, the vector freed; then the name and the facet base.
template <bool Deleting>
void imp_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("locale::_Locimp::~_Locimp", self, kImpBytes)) return;
    g.w32(self, vtable(ctx, "NSt6locale7_LocimpE"));
    {
        Lockit lock(ctx, kLockLocale);
        const u32 vec = g.r32(self + kImpFacetvec);
        for (u32 n = g.r32(self + kImpFacetcount); 0 < n;) {
            --n;
            if (const u32 f = g.ok(vec + 4 * n, 4) ? g.r32(vec + 4 * n) : 0) release(ctx, f);
        }
        libc_fn::free(ctx, {vec});
    }
    GStr<1>(g, self + kImpName).destroy();
    g.w32(self, vtable(ctx, "NSt6locale5facetE"));
    if (Deleting) op_delete(ctx, self);
}

// _Locimp::_Addfac(facet *, size_t _Id): the vector grown to at least 40 (realloc, new entries null), the facet
// counted, the one it replaces released.
inline void imp_addfac(PPUContext& ctx, u32 self, u32 fac, u32 id)
{
    G g(ctx);
    if (!g.need("locale::_Locimp::_Addfac", self, kImpBytes) || !g.need("locale::_Locimp::_Addfac", fac, 8)) return;
    Lockit lock(ctx, kLockLocale);
    const u32 count = g.r32(self + kImpFacetcount);
    if (count <= id) {
        u32 n = id + 1;
        if (n < 40) n = 40;
        const u32 vec = static_cast<u32>(libc_fn::realloc(ctx, {g.r32(self + kImpFacetvec), u64{n} * 4}));
        if (!vec) raise_new(ctx, "St9bad_alloc");
        g.w32(self + kImpFacetvec, vec);
        for (u32 i = count; i < n; i++) g.w32(vec + 4 * i, 0);
        g.w32(self + kImpFacetcount, n);
    }
    incref(ctx, fac);
    const u32 slot = g.r32(self + kImpFacetvec) + 4 * id;
    if (const u32 old = g.r32(slot)) release(ctx, old);
    g.w32(slot, fac);
}

void imp_addfac_(PPUContext& ctx, u32 self, u32 fac, u32 id) { imp_addfac(ctx, self, fac, id); }

// ------------------------------------------------------------------------------------------------- locale

// locale::_Init(): the calling thread's global _Locimp, made the first time ("C", every category) and made the
// classic one.
inline u32 locale_init(PPUContext& ctx)
{
    if (const u32 p = global_imp(ctx)) return p;
    Lockit lock(ctx, kLockLocale);
    if (const u32 p = global_imp(ctx)) return p;
    const u32 p = new_imp(ctx, false);
    set_global_imp(ctx, p);
    G g(ctx);
    g.w32(p + kImpCatmask, kCatAll);
    GStr<1>(g, p + kImpName).set("C");
    {
        LocaleState& s = lstate();
        std::lock_guard l(s.mutex);
        s.clocptr = p;
    }
    incref(ctx, p);
    g.w32(static_object("classic_locale", 4), p);
    return p;
}

u32 locale_init_(PPUContext& ctx) { return locale_init(ctx); }

// locale(): the global locale, counted.
inline void locale_default(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, locale_init(ctx));
    incref(ctx, global_imp(ctx));
}

void locale_ctor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("locale::locale", self, 4)) locale_default(ctx, self);
}

// ~locale: delete _Ptr->_Decref().
inline void locale_destroy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("locale::~locale", self, 4)) release(ctx, g.r32(self));
}

void locale_dtor_(PPUContext& ctx, u32 self) { locale_destroy(ctx, self); }

// locale = locale (the header's inline operator=).
inline void locale_assign(PPUContext& ctx, u32 self, u32 right)
{
    G g(ctx);
    const u32 rp = g.r32(right);
    if (g.r32(self) != rp) {
        release(ctx, g.r32(self));
        g.w32(self, rp);
        incref(ctx, rp);
    }
}

// A copy of a locale (the header's inline copy constructor).
inline void locale_copy(PPUContext& ctx, u32 self, u32 right)
{
    G g(ctx);
    const u32 p = g.r32(right);
    g.w32(self, p);
    incref(ctx, p);
}

u32 classic_(PPUContext& ctx)
{
    locale_init(ctx);
    return static_object("classic_locale", 4);
}

// empty(): a transparent locale with no facets.
void empty_(PPUContext& ctx, u32 out)
{
    G g(ctx);
    if (!g.need("locale::empty", out, 4)) return;
    locale_init(ctx);
    g.w32(out, new_imp(ctx, true));
}

// The C++ library's setlocale (the SDK's own): "C" is the only locale; a query (no name) answers "C". The category (a
// mask) changes nothing.
inline u32 cxx_setlocale(PPUContext& ctx, u32 /*catmask*/, std::optional<std::string> name)
{
    if (!name || *name == "C") return cstring(ctx, "C");
    return 0;
}

// global(const locale&): the old global locale (in *out); the thread's global replaced and the C library's locale
// told, category by category.
void global_(PPUContext& ctx, u32 out, u32 loc)
{
    G g(ctx);
    if (!g.need("locale::global", out, 4) || !g.need("locale::global", loc, 4)) return;
    locale_default(ctx, out);
    Lockit lock(ctx, kLockLocale);
    u32 p = global_imp(ctx);
    const u32 lp = g.r32(loc);
    if (p != lp) {
        release(ctx, p);
        set_global_imp(ctx, p = lp);
        incref(ctx, p);
        const u32 cmask = g.r32(p + kImpCatmask) & kCatAll;
        const std::string name = GStr<1>(g, p + kImpName).text();
        if (cmask == kCatAll) cxx_setlocale(ctx, kCatAll, name);
        else {
            for (s32 i = 1; i <= 6; i++)
                if (catmask(i) & cmask) cxx_setlocale(ctx, catmask(i), name);
        }
    }
}

// _Getfacet(size_t _Id) const: the locale's facet, or for a transparent one the global locale's.
inline u32 getfacet(PPUContext& ctx, u32 loc, u32 id)
{
    G g(ctx);
    const u32 p = g.r32(loc);
    if (!g.need("locale::_Getfacet", p, kImpBytes)) return 0;
    const auto at = [&](u32 imp) -> u32 {
        const u32 vec = g.r32(imp + kImpFacetvec);
        return id < g.r32(imp + kImpFacetcount) && g.ok(vec + 4 * id, 4) ? g.r32(vec + 4 * id) : 0;
    };
    const u32 f = at(p);
    if (f || !g.r8(p + kImpXparent)) return f;
    const u32 gp = global_imp(ctx);
    return gp ? at(gp) : 0;
}

u32 getfacet_(PPUContext& ctx, u32 self, u32 id)
{
    G g(ctx);
    return g.need("locale::_Getfacet", self, 4) ? getfacet(ctx, self, id) : 0;
}

// operator==: the same _Locimp, or both named alike and not "*".
u32 locale_eq_(PPUContext& ctx, u32 self, u32 other)
{
    G g(ctx);
    if (!g.need("locale::operator==", self, 4) || !g.need("locale::operator==", other, 4)) return 0;
    const u32 a = g.r32(self), b = g.r32(other);
    if (a == b) return 1;
    const std::string na = GStr<1>(g, a + kImpName).text(), nb = GStr<1>(g, b + kImpName).text();
    return na != "*" && na == nb ? 1 : 0;
}

// locale::_Addfac(facet *, size_t _Id, size_t _Catmask): copy on write, then the facet; a locale with a facet added
// for a category has no name.
u32 locale_addfac_(PPUContext& ctx, u32 self, u32 fac, u32 id, u32 cmask)
{
    G g(ctx);
    if (!g.need("locale::_Addfac", self, 4)) return self;
    u32 p = g.r32(self);
    if (!g.need("locale::_Addfac", p, kImpBytes)) return self;
    if (1 < refs_of(g, p)) {
        decref(ctx, p);
        p = new_imp_copy(ctx, p);
        g.w32(self, p);
    }
    imp_addfac(ctx, p, fac, id);
    if (cmask != 0) GStr<1>(g, p + kImpName).set("*");
    return self;
}

// _Deletegloballocale(void *): delete (*(_Locimp **)ptr)->_Decref().
void deletegloballocale_(PPUContext& ctx, u32 ptr)
{
    G g(ctx);
    if (!g.need("_Deletegloballocale", ptr, 4)) return;
    if (const u32 p = g.r32(ptr)) release(ctx, p);
}

// __Setgloballocale(void *): the calling thread's global _Locimp.
void setgloballocale_(PPUContext& ctx, u32 imp) { set_global_imp(ctx, imp); }

// ------------------------------------------------------------------------------------------------- _Locinfo

// The "C" locale's time names as Dinkumware's _Tinfo holds them (_Getptimes): am/pm, the day names three times, the
// month names three times, the formats ten times, four empty strings (the era fields).
inline u32 c_times(PPUContext& ctx)
{
    bool fresh = false;
    const u32 t = static_object("C _Tinfo", kTinfoBytes, &fresh);
    if (fresh) {
        G g(ctx);
        const u32 ampm = cstring(ctx, ":AM:PM");
        const u32 days = cstring(ctx, ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:"
                                      "Saturday");
        const u32 months = cstring(ctx, ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:"
                                        "Aug:August:Sep:September:Oct:October:Nov:November:Dec:December");
        const u32 formats = cstring(ctx, "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p");
        const u32 empty = cstring(ctx, "");
        u32 i = 0;
        g.w32(t + 4 * i++, ampm);
        for (u32 k = 0; k < 3; k++) g.w32(t + 4 * i++, days);
        for (u32 k = 0; k < 3; k++) g.w32(t + 4 * i++, months);
        for (u32 k = 0; k < 10; k++) g.w32(t + 4 * i++, formats);
        for (u32 k = 0; k < 4; k++) g.w32(t + 4 * i++, empty);
    }
    return t;
}

// The common start of the constructors: the lock taken (kept until the destructor), the time names, two empty names
// and the old locale's name.
inline void info_begin(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, kLockLocale);
    libc_fn::lock(ctx, {kLockLocale});
    std::memcpy(g.b + self + kInfoTimes, g.b + c_times(ctx), kTinfoBytes);
    const GStr<1> oldn(g, self + kInfoOld), newn(g, self + kInfoNew);
    oldn.construct();
    newn.construct();
    oldn.set(g.str(cxx_setlocale(ctx, kCatAll, std::nullopt)).value_or("C"));
}

inline std::optional<std::string> name_arg(PPUContext& ctx, u32 name)
{
    if (!name) return std::nullopt;
    return G(ctx).str(name);
}

// _Addcats(int _Cat, const char *_Locname): the named categories merged; a bad name makes the result's name "*"; no
// category keeps the current name.
inline void info_addcats(PPUContext& ctx, u32 self, s32 cat, u32 locname)
{
    G g(ctx);
    const std::optional<std::string> name = name_arg(ctx, locname);
    if (!name) {
        log_call_site(ctx, g.b, "_Locinfo::_Addcats: no locale name (the SDK's reads through it)");
        return;
    }
    u32 old = 0;
    if (*name == "*") {
        old = 0;
    } else if (cat == 0) {
        old = cxx_setlocale(ctx, kCatAll, std::nullopt);
    } else {
        // Each category named, then all of them (the SDK's asks for the whole locale by the name, not for the
        // current one's name, so a bad name is "*" whatever the categories).
        if (cat != static_cast<s32>(kCatAll))
            for (s32 i = 1; i <= 6; i++)
                if (catmask(i) & static_cast<u32>(cat)) cxx_setlocale(ctx, catmask(i), name);
        old = cxx_setlocale(ctx, kCatAll, name);
    }
    const GStr<1> newn(g, self + kInfoNew);
    if (old == 0) newn.set("*");
    else if (newn.text() != "*") newn.set(g.str(old).value_or("C"));
}

void info_ctor_name_(PPUContext& ctx, u32 self, u32 locname)
{
    G g(ctx);
    if (!g.need("_Locinfo::_Locinfo", self, kInfoBytes)) return;
    info_begin(ctx, self);
    const std::optional<std::string> name = name_arg(ctx, locname);
    const u32 n = name ? cxx_setlocale(ctx, kCatAll, name) : 0;
    GStr<1>(g, self + kInfoNew).set(n ? g.str(n).value_or("C") : "*");
}

void info_ctor_cat_(PPUContext& ctx, u32 self, s32 cat, u32 locname)
{
    G g(ctx);
    if (!g.need("_Locinfo::_Locinfo", self, kInfoBytes)) return;
    info_begin(ctx, self);
    info_addcats(ctx, self, cat, locname);
}

u32 info_addcats_(PPUContext& ctx, u32 self, s32 cat, u32 locname)
{
    G g(ctx);
    if (g.need("_Locinfo::_Addcats", self, kInfoBytes)) info_addcats(ctx, self, cat, locname);
    return self;
}

// ~_Locinfo: the old locale's name given back to setlocale, the names freed, the lock released.
void info_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("_Locinfo::~_Locinfo", self, kInfoBytes)) return;
    const GStr<1> oldn(g, self + kInfoOld), newn(g, self + kInfoNew);
    if (0 < oldn.size()) cxx_setlocale(ctx, kCatAll, oldn.text());
    newn.destroy();
    oldn.destroy();
    libc_fn::unlock(ctx, {g.r32(self)});
}

// _Getctype(): sys_libc's ctype tables (the pointers its variables hold), no table to delete.
void info_getctype_(PPUContext& ctx, u32 out, u32)
{
    G g(ctx);
    if (!g.need("_Locinfo::_Getctype", out, 16)) return;
    g.w32(out, g.r32(libc_variable("_Ctype")));
    g.w32(out + 4, g.r32(libc_variable("_Tolotab")));
    g.w32(out + 8, g.r32(libc_variable("_Touptab")));
    g.w32(out + 12, 0);
}

// _Getcvt(): the thread's multibyte and wide conversion states (_Getpmbstate, _Getpwcstate).
void info_getcvt_(PPUContext& ctx, u32 out, u32)
{
    G g(ctx);
    if (!g.need("_Locinfo::_Getcvt", out, 8)) return;
    g.w32(out, static_cast<u32>(libc_fn::getpmbstate(ctx, {})));
    g.w32(out + 4, static_cast<u32>(libc_fn::getpwcstate(ctx, {})));
}

// _Getcoll(): the thread's collation states (_Getpcostate, _Getpwcostate).
void info_getcoll_(PPUContext& ctx, u32 out, u32)
{
    G g(ctx);
    if (!g.need("_Locinfo::_Getcoll", out, 8)) return;
    g.w32(out, static_cast<u32>(libc_fn::getpcostate(ctx, {})));
    g.w32(out + 4, static_cast<u32>(libc_fn::getpwcostate(ctx, {})));
}

// ------------------------------------------------------------------------------------------------- the facets part

inline std::string member_name(std::string_view type, std::string_view member)
{
    // "St5ctypeIcE" -> "_ZNSt5ctypeIcE" + member
    return std::string("_ZNSt") + std::string(type.substr(2)) + std::string(member);
}

// F::id: the facets part's variable when it exports one, else an id object of this part's.
inline u32 facet_id_object(std::string_view type)
{
    const std::string name = member_name(type, "2idE");
    if (const u32 v = HleProcess::current()->link_variable(kModule, recomp::nid_of(name))) return v;
    return static_object(name, 4);
}

inline u32 facet_id(PPUContext& ctx, std::string_view type) { return id_value(ctx, facet_id_object(type)); }

// F::_Getcat(const facet **): the facets part's. A missing one is logged with its name and answers -1 (no facet).
inline s32 facet_getcat(PPUContext& ctx, std::string_view type, u32 ppf)
{
    const std::string name = member_name(type, "7_GetcatEPPKNSt6locale5facetE");
    const Function* f = registry().find(kModule, recomp::nid_of(name));
    if (!f) {
        PS_LOG_ERROR(kChan, "{} is not registered (the facets part, D-561): no {} facet can be made", name, type);
        return -1;
    }
    return static_cast<s32>(static_cast<u32>(invoke(ctx, *f, {ppf})));
}

// use_facet<F>(const locale&): the locale's facet, else the lazy one (made once with _Getcat, counted and queued
// for _Fac_tidy); bad_cast when none can be made.
inline u32 use_facet(PPUContext& ctx, u32 loc, std::string_view type)
{
    G g(ctx);
    if (!g.need("use_facet", loc, 4)) return 0;
    u32 pf = 0;
    {
        Lockit lock(ctx, kLockLocale);
        const u32 id = facet_id(ctx, type);
        pf = getfacet(ctx, loc, id);
        if (!pf) {
            LocaleState& s = lstate();
            u32 psave = 0;
            {
                std::lock_guard l(s.mutex);
                if (const auto it = s.psave.find(type); it != s.psave.end()) psave = it->second;
            }
            if (psave) {
                pf = psave;
            } else {
                StackVar slot(ctx, g.b, 8);
                if (facet_getcat(ctx, type, slot.addr()) == -1 || !(pf = g.r32(slot.addr()))) {
                    pf = 0;
                } else {
                    {
                        std::lock_guard l(s.mutex);
                        s.psave[type] = pf;
                    }
                    incref(ctx, pf);
                    facet_register_(ctx, pf);
                }
            }
        }
    }
    if (!pf) raise_new(ctx, "St8bad_cast");
    return pf;
}

template <const char* Type>
u32 use_facet_(PPUContext& ctx, u32 loc)
{
    return use_facet(ctx, loc, Type);
}

inline constexpr char kCtypeC[] = "St5ctypeIcE", kCtypeW[] = "St5ctypeIwE",
                      kNumGetC[] = "St7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
                      kNumGetW[] = "St7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
                      kNumPutC[] = "St7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
                      kNumPutW[] = "St7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
                      kNumpunctC[] = "St8numpunctIcE", kNumpunctW[] = "St8numpunctIwE",
                      kCollateC[] = "St7collateIcE", kCollateW[] = "St7collateIwE",
                      kMessagesC[] = "St8messagesIcE", kMessagesW[] = "St8messagesIwE",
                      kMoneyGetC[] = "St9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
                      kMoneyGetW[] = "St9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
                      kMoneyPutC[] = "St9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
                      kMoneyPutW[] = "St9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
                      kMoneypunctC0[] = "St10moneypunctIcLb0EE", kMoneypunctC1[] = "St10moneypunctIcLb1EE",
                      kMoneypunctW0[] = "St10moneypunctIwLb0EE", kMoneypunctW1[] = "St10moneypunctIwLb1EE",
                      kTimeGetC[] = "St8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE",
                      kTimeGetW[] = "St8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE",
                      kTimePutC[] = "St8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
                      kTimePutW[] = "St8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE",
                      kCodecvtC[] = "St7codecvtIccSt9_MbstatetE", kCodecvtW[] = "St7codecvtIwcSt9_MbstatetE";

// ADDFAC: a facet of the categories in `cat` added to `imp`: made from the "C" locale (no `ploc`), or `ploc`'s.
inline void addfac(PPUContext& ctx, std::string_view type, s32 cat, u32 imp, u32 ploc)
{
    const s32 own = facet_getcat(ctx, type, 0);
    if (own == -1) raise_new(ctx, "St8bad_cast");
    if ((catmask(own) & static_cast<u32>(cat)) == 0) return;
    u32 f = 0;
    if (ploc == 0) {
        G g(ctx);
        StackVar slot(ctx, g.b, 8);
        if (facet_getcat(ctx, type, slot.addr()) == -1 || !(f = g.r32(slot.addr()))) raise_new(ctx, "St8bad_cast");
    } else {
        f = use_facet(ctx, ploc, type);
    }
    imp_addfac(ctx, imp, f, facet_id(ctx, type));
}

// _Makexloc: the exotic char facets (collate, messages, money_get/put, moneypunct both, time_get/put).
void makexloc(PPUContext& ctx, u32, s32 cat, u32 imp, u32 ploc)
{
    for (const char* t : {kCollateC, kMessagesC, kMoneyGetC, kMoneyPutC, kMoneypunctC0, kMoneypunctC1, kTimeGetC,
             kTimePutC})
        addfac(ctx, t, cat, imp, ploc);
}

// _Makewloc: the wide facets.
void makewloc(PPUContext& ctx, u32, s32 cat, u32 imp, u32 ploc)
{
    for (const char* t : {kCtypeW, kNumGetW, kNumPutW, kNumpunctW, kCollateW, kMessagesW, kMoneyGetW, kMoneyPutW,
             kMoneypunctW0, kMoneypunctW1, kTimeGetW, kTimePutW, kCodecvtW})
        addfac(ctx, t, cat, imp, ploc);
}

// _Makeloc: ctype, num_get, num_put, numpunct and codecvt for char, then _Makexloc and _Makewloc; the categories and
// the _Locinfo's name taken.
u32 makeloc(PPUContext& ctx, u32 lobj, s32 cat, u32 imp, u32 ploc)
{
    G g(ctx);
    for (const char* t : {kCtypeC, kNumGetC, kNumPutC, kNumpunctC, kCodecvtC}) addfac(ctx, t, cat, imp, ploc);
    makexloc(ctx, lobj, cat, imp, ploc);
    makewloc(ctx, lobj, cat, imp, ploc);
    g.w32(imp + kImpCatmask, g.r32(imp + kImpCatmask) | static_cast<u32>(cat));
    GStr<1>(g, imp + kImpName).set(GStr<1>(g, lobj + kInfoNew).text());
    return imp;
}

u32 makeloc_(PPUContext& ctx, u32 lobj, s32 cat, u32 imp, u32 ploc)
{
    G g(ctx);
    if (!g.need("_Locimp::_Makeloc", lobj, kInfoBytes) || !g.need("_Locimp::_Makeloc", imp, kImpBytes)) return imp;
    return makeloc(ctx, lobj, cat, imp, ploc);
}

u32 makexloc_(PPUContext& ctx, u32 lobj, s32 cat, u32 imp, u32 ploc)
{
    G g(ctx);
    if (g.need("_Locimp::_Makexloc", imp, kImpBytes)) makexloc(ctx, lobj, cat, imp, ploc);
    return imp;
}

u32 makewloc_(PPUContext& ctx, u32 lobj, s32 cat, u32 imp, u32 ploc)
{
    G g(ctx);
    if (g.need("_Locimp::_Makewloc", imp, kImpBytes)) makewloc(ctx, lobj, cat, imp, ploc);
    return imp;
}

// A _Locinfo on the guest stack for the length of a scope (_BEGIN_LOCINFO).
class LocinfoVar {
public:
    LocinfoVar(PPUContext& ctx, std::optional<s32> cat, u32 name) : ctx_(ctx), var_(ctx, guest_base(ctx), kInfoBytes)
    {
        if (cat) info_ctor_cat_(ctx_, var_.addr(), *cat, name);
        else info_ctor_name_(ctx_, var_.addr(), name);
    }
    ~LocinfoVar() { info_dtor_(ctx_, var_.addr()); }
    LocinfoVar(const LocinfoVar&) = delete;
    LocinfoVar& operator=(const LocinfoVar&) = delete;
    u32 addr() const { return var_.addr(); }
    std::string name() const
    {
        G g(ctx_);
        return GStr<1>(g, var_.addr() + kInfoNew).text();
    }

private:
    PPUContext& ctx_;
    StackVar var_;
};

inline void imp_copy(PPUContext& ctx, u32 self, u32 right)
{
    G g(ctx);
    g.w32(self, vtable(ctx, "NSt6locale7_LocimpE"));
    g.w32(self + kFacetRefs, 1);
    g.w32(self + kImpFacetvec, 0);
    g.w32(self + kImpFacetcount, g.r32(right + kImpFacetcount));
    g.w32(self + kImpCatmask, g.r32(right + kImpCatmask));
    g.w8(self + kImpXparent, g.r8(right + kImpXparent));
    const GStr<1> name(g, self + kImpName);
    name.construct();
    name.assign(right + kImpName, 0, npos);
    u32 cloc = 0;
    {
        LocaleState& s = lstate();
        std::lock_guard l(s.mutex);
        cloc = s.clocptr;
    }
    if (right == cloc) {
        LocinfoVar lobj(ctx, std::nullopt, cstring(ctx, "C"));
        makeloc(ctx, lobj.addr(), kCatAll, self, 0);
        return;
    }
    Lockit lock(ctx, kLockLocale);
    const u32 count = g.r32(self + kImpFacetcount);
    if (0 < count) {
        const u32 vec = static_cast<u32>(libc_fn::malloc(ctx, {u64{count} * 4}));
        if (!vec) raise_new(ctx, "St9bad_alloc");
        g.w32(self + kImpFacetvec, vec);
        const u32 rvec = g.r32(right + kImpFacetvec);
        for (u32 n = count; 0 < n;) {
            --n;
            const u32 f = g.r32(rvec + 4 * n);
            g.w32(vec + 4 * n, f);
            if (f) incref(ctx, f);
        }
    }
}

// locale(const char *_Locname, category _Cat): a new _Locimp with the named categories' facets; runtime_error("bad
// locale name") for a name that isn't a locale.
inline void locale_named(PPUContext& ctx, u32 self, u32 locname, s32 cat)
{
    G g(ctx);
    const u32 p = new_imp(ctx, false);
    g.w32(self, p);
    locale_init(ctx);
    LocinfoVar lobj(ctx, cat, locname);
    if (lobj.name() == "*") {
        release(ctx, p);
        raise_new(ctx, "St13runtime_error", "bad locale name");
    }
    makeloc(ctx, lobj.addr(), cat, p, 0);
}

void locale_ctor_name_(PPUContext& ctx, u32 self, u32 locname, s32 cat)
{
    G g(ctx);
    if (g.need("locale::locale", self, 4)) locale_named(ctx, self, locname, cat);
}

// locale(const locale& _Loc, const char *_Locname, category _Cat)
void locale_ctor_loc_name_(PPUContext& ctx, u32 self, u32 loc, u32 locname, s32 cat)
{
    G g(ctx);
    if (!g.need("locale::locale", self, 4) || !g.need("locale::locale", loc, 4)) return;
    const u32 lp = g.r32(loc);
    const u32 p = new_imp_copy(ctx, lp);
    g.w32(self, p);
    const std::string base = GStr<1>(g, lp + kImpName).text();
    LocinfoVar lobj(ctx, static_cast<s32>(g.r32(lp + kImpCatmask)), cstring(ctx, base));
    const bool hadname = lobj.name() != "*";
    info_addcats(ctx, lobj.addr(), cat, locname);
    if (hadname && lobj.name() == "*") {
        release(ctx, p);
        raise_new(ctx, "St13runtime_error", "bad locale name");
    }
    makeloc(ctx, lobj.addr(), cat, p, 0);
}

// locale(const locale& _Loc, const locale& _Other, category _Cat): _Loc's copy with _Other's facets of _Cat.
void locale_ctor_loc_loc_(PPUContext& ctx, u32 self, u32 loc, u32 other, s32 cat)
{
    G g(ctx);
    if (!g.need("locale::locale", self, 4) || !g.need("locale::locale", loc, 4) ||
        !g.need("locale::locale", other, 4))
        return;
    const u32 lp = g.r32(loc), op = g.r32(other);
    const u32 p = new_imp_copy(ctx, lp);
    g.w32(self, p);
    LocinfoVar lobj(ctx, static_cast<s32>(g.r32(lp + kImpCatmask)), cstring(ctx, GStr<1>(g, lp + kImpName).text()));
    info_addcats(ctx, lobj.addr(), cat & static_cast<s32>(g.r32(op + kImpCatmask)),
        cstring(ctx, GStr<1>(g, op + kImpName).text()));
    makeloc(ctx, lobj.addr(), cat, p, other);
}

} // namespace pspocket::hle::stdcxx
