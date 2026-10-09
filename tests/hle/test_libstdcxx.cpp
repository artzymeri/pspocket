// sys_libstdcxx, the foundation and the streams (runtime/hle/src/stdcxx_*.hpp, D-560): the SDK's C++ library
// (Dinkumware's) checked against the C++ standard and Dinkumware's documented choices, each function called as guest
// code calls it, virtual functions through the vtables pspocket makes. The facets are the other part's (D-561): where
// a stream needs one (ctype's widen, codecvt), the test puts a guest facet of its own in the thread's global locale.
// No game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "guest.hpp" // runtime/hle/src (internal): call_guest
#include "hle_test_util.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/nid.hpp"
#include "stdcxx.hpp" // runtime/hle/src (internal): the library's own data (the facet ids it keeps)

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
namespace fsys = std::filesystem;

namespace {

constexpr u32 kCode = kData + 0x10000, kVt = kData + 0x14000, kObj = kData + 0x18000, kObj2 = kData + 0x19000,
              kObj3 = kData + 0x1A000, kStr = kData + 0x1B000, kBuf = kData + 0x20000, kBuf2 = kData + 0x21000,
              kRec = kData + 0x1F000, kOut = kData + 0x1F800;
constexpr std::string_view kMod = "sys_libstdcxx";

void put(u8* b, u32 at, std::string_view s)
{
    std::memcpy(b + at, s.data(), s.size());
    b[at + s.size()] = 0;
}

std::string cstr(u8* b, u32 at)
{
    std::string s;
    while (b[at + s.size()]) s += static_cast<char>(b[at + s.size()]);
    return s;
}

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

// A function's descriptor (an exported or internal one of the library).
u32 desc(std::string_view name) { return HleProcess::current()->link_function(kMod, recomp::nid_of(name), name); }

// The name of the library function a descriptor leads to ("" for guest code).
std::string fn_name(u8* b, u32 d)
{
    const LinkedFunction* f = HleProcess::current()->linked_at(ps_read32(b, d));
    return f && f->impl ? std::string(f->impl->name) : std::string();
}

// The type name of an object's dynamic type (its vtable's type_info).
std::string type_name(u8* b, u32 obj) { return cstr(b, ps_read32(b, ps_read32(b, ps_read32(b, obj) - 4) + 4)); }

// Guest functions, each with its descriptor after its code.
struct Code {
    u8* b;
    u32 at = kCode;
    u32 add(std::initializer_list<u32> words)
    {
        const u32 entry = at;
        for (const u32 w : words) {
            ps_write32(b, at, w);
            at += 4;
        }
        const u32 opd = at;
        ps_write32(b, opd, entry);
        ps_write32(b, opd + 4, 0);
        at += 16;
        return opd;
    }
};

constexpr u32 kBlr = 0x4E800020, kMrR3R4 = 0x7C832378, kNop = 0x60000000;
u32 li3(s16 v) { return 0x38600000u | static_cast<u16>(v); }
u32 lis(u32 r, u32 hi) { return 0x3C000000u | (r << 21) | (hi & 0xFFFF); }
u32 ori(u32 ra, u32 rs, u32 lo) { return 0x60000000u | (rs << 21) | (ra << 16) | (lo & 0xFFFF); }
u32 stw(u32 rs, u32 ra, s16 d) { return 0x90000000u | (rs << 21) | (ra << 16) | static_cast<u16>(d); }
u32 lwz(u32 rt, u32 ra, s16 d) { return 0x80000000u | (rt << 21) | (ra << 16) | static_cast<u16>(d); }
u32 addi(u32 rt, u32 ra, s16 d) { return 0x38000000u | (rt << 21) | (ra << 16) | static_cast<u16>(d); }

// A guest function recording its call: r3, r4 and r5 at rec, rec + 4 and rec + 8, a count at rec + 12; it returns
// `ret` (or r3 when there is none).
u32 recorder(Code& c, u32 rec, std::optional<s16> ret = std::nullopt)
{
    return c.add({lis(11, rec >> 16), ori(11, 11, rec), stw(3, 11, 0), stw(4, 11, 4), stw(5, 11, 8), lwz(12, 11, 12),
        addi(12, 12, 1), stw(12, 11, 12), ret ? li3(*ret) : kNop, kBlr});
}

// The TTY's text written while it lives.
struct Tty {
    std::mutex mutex;
    std::string text;
    Tty()
    {
        log::set_sink(
            [](void* user, log::Level, std::string_view channel, std::string_view message) {
                if (channel != "tty") return;
                auto* self = static_cast<Tty*>(user);
                std::lock_guard lock(self->mutex);
                self->text += message;
            },
            this);
    }
    ~Tty() { log::set_sink(nullptr, nullptr); }
    Tty(const Tty&) = delete;
    Tty& operator=(const Tty&) = delete;
    std::string get()
    {
        std::lock_guard lock(mutex);
        return text;
    }
};

// A string object's fields: {allocator byte, _Bx (16), _Mysize, _Myres}.
struct GString {
    u8* b;
    u32 at;
    u32 size() const { return ps_read32(b, at + 20); }
    u32 res() const { return ps_read32(b, at + 24); }
    u32 ptr(u32 bufsize) const { return res() >= bufsize ? ps_read32(b, at + 4) : at + 4; }
    std::string text() const { return std::string(reinterpret_cast<const char*>(b + ptr(16)), size()); }
    std::u32string wtext() const
    {
        std::u32string s;
        for (u32 i = 0; i < size(); i++) s += static_cast<char32_t>(ps_read32(b, ptr(4) + 4 * i));
        return s;
    }
    u32 terminator() const { return b[ptr(16) + size()]; }
};

// The id the library gives a standard facet type (the facets part's variable when it has one, else its own object).
u32 facet_id(GuestThread& t, std::string_view type)
{
    const std::string name = std::string("_ZNSt") + std::string(type.substr(2)) + "2idE";
    u32 obj = HleProcess::current()->link_variable(kMod, recomp::nid_of(name));
    if (!obj) obj = stdcxx::static_object(name, 4);
    return static_cast<u32>(t.call("_ZNSt6locale2idcvjEv", obj));
}

// A guest facet whose virtual functions answer their first argument (ctype's widen, narrow, tolower...), or for
// codecvt "always no conversion", put in the thread's global locale for `type`.
u32 test_facet(GuestThread& t, Code& c, u32 at, u32 vt, std::string_view type, u32 slots, bool codecvt = false)
{
    u8* b = t.f.base();
    const u32 nop = c.add({kBlr}), ident = c.add({kMrR3R4, kBlr}), yes = c.add({li3(1), kBlr});
    ps_write32(b, vt, 0);
    ps_write32(b, vt + 4, 0);
    for (u32 i = 0; i < slots; i++) ps_write32(b, vt + 8 + 4 * i, i < 2 ? nop : codecvt && i == 2 ? yes : ident);
    ps_write32(b, at, vt + 8);
    ps_write32(b, at + 4, 0);
    const u32 imp = static_cast<u32>(t.call("_ZNSt6locale5_InitEv"));
    t.call("_ZNSt6locale7_Locimp7_AddfacEPNS_5facetEj", imp, at, facet_id(t, type));
    return at;
}

// The facets the streams need: ctype<char>, ctype<wchar_t>, the two codecvts (no conversion).
void stream_facets(GuestThread& t, Code& c)
{
    test_facet(t, c, kObj3 + 0x800, kVt + 0x800, "St5ctypeIcE", 10);
    test_facet(t, c, kObj3 + 0x810, kVt + 0x840, "St5ctypeIwE", 14);
    test_facet(t, c, kObj3 + 0x820, kVt + 0x880, "St7codecvtIccSt9_MbstatetE", 9, true);
    test_facet(t, c, kObj3 + 0x830, kVt + 0x8C0, "St7codecvtIwcSt9_MbstatetE", 9, true);
}

// A locale object made by locale() at `at`.
u32 make_locale(GuestThread& t, u32 at)
{
    t.call("_ZNSt6localeC1Ev", at);
    return at;
}

// A vtable in guest memory made of the library's functions (for objects the test lays out by hand).
u32 lib_vtable(u8* b, u32 at, std::initializer_list<std::string_view> fns)
{
    ps_write32(b, at, 0);
    ps_write32(b, at + 4, 0);
    u32 i = 0;
    for (const std::string_view f : fns) ps_write32(b, at + 8 + 4 * i++, desc(f));
    return at + 8;
}

// A basic_streambuf laid out by hand: its vtable, its pointers at its own fields (none set).
void hand_streambuf(u8* b, u32 at, u32 vt)
{
    std::memset(b + at, 0, 60);
    ps_write32(b, at, vt);
    for (u32 k = 0; k < 6; k++) ps_write32(b, at + 16 + 4 * (k + (k / 2) * 2), at + 8 + 4 * (k + (k / 2) * 2));
}

void setg(u8* b, u32 sb, u32 first, u32 next, u32 last)
{
    ps_write32(b, sb + 8, first);
    ps_write32(b, sb + 24, next);
    ps_write32(b, sb + 40, last);
}

void setp(u8* b, u32 sb, u32 first, u32 next, u32 last)
{
    ps_write32(b, sb + 12, first);
    ps_write32(b, sb + 28, next);
    ps_write32(b, sb + 44, last);
}

fsys::path temp_root()
{
    const fsys::path root =
        fsys::temp_directory_path() / std::format("pspocket-stdcxx-{}", static_cast<long>(getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "hdd0" / "d");
    return root;
}

std::string slurp(const fsys::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const std::initializer_list<std::string_view> kStreambufC = {"_ZNSt15basic_streambufIcSt11char_traitsIcEED1Ev",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEED0Ev", "_ZNSt15basic_streambufIcSt11char_traitsIcEE5_LockEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE7_UnlockEv", "_ZNSt15basic_streambufIcSt11char_traitsIcEE8overflowEi",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi", "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE9underflowEv", "_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv",
    "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci"};

} // namespace

TEST_CASE("sys_libstdcxx: every function registered under its mangled name, the vtables the SDK's", "[hle][libstdcxx]")
{
    // The functions of this part (D-560) are all native; the internal ones it hands out by address too.
    usize n = 0;
    for (const Function& f : registry().functions())
        if (f.module == kMod && !f.internal) n++;
    CHECK(n >= 295);

    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    // An exception's vtable: [~D1, ~D0, what, _Doraise] after offset-to-top and type_info; its type_info's name.
    t.call("_ZNSt8bad_castC1Ev", kObj);
    const u32 vt = ps_read32(b, kObj);
    CHECK(ps_read32(b, vt - 8) == 0);
    const u32 ti = ps_read32(b, vt - 4);
    CHECK(cstr(b, ps_read32(b, ti + 4)) == "St8bad_cast");
    CHECK(cstr(b, ps_read32(b, ps_read32(b, ti + 8) + 4)) == "St9exception"); // its base's
    CHECK(fn_name(b, ps_read32(b, vt)) == "_ZNSt8bad_castD1Ev");
    CHECK(fn_name(b, ps_read32(b, vt + 4)) == "_ZNSt8bad_castD0Ev");
    CHECK(fn_name(b, ps_read32(b, vt + 8)) == "_ZNKSt8bad_cast4whatEv");
    CHECK(fn_name(b, ps_read32(b, vt + 12)) == "_ZNKSt9exception8_DoraiseEv");
    // what() through the vtable, as guest code calls it.
    const u32 what = static_cast<u32>(call_guest(t.ctx, ps_read32(b, vt + 8), {kObj}));
    CHECK(cstr(b, what) == "bad cast");
    CHECK(cstr(b, static_cast<u32>(t.call("_ZNKSt9exception4whatEv", kObj))).empty());
    CHECK(cstr(b, static_cast<u32>(t.call("_ZNKSt9bad_alloc4whatEv", kObj))) == "bad allocation");
    // The destructor leaves exception's vptr; D0 frees the object.
    t.call("_ZNSt8bad_castD1Ev", kObj);
    CHECK(type_name(b, kObj) == "St9exception");
    const u32 heap = static_cast<u32>(t.call("_Znwj", 4));
    t.call("_ZNSt8bad_castC1Ev", heap);
    call_guest(t.ctx, ps_read32(b, ps_read32(b, heap) + 4), {heap});
}

TEST_CASE("sys_libstdcxx: operator new and delete, the new handler", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    const u32 p = static_cast<u32>(t.call("_Znwj", 100));
    REQUIRE(p != 0);
    std::memset(b + p, 0xAB, 100);
    t.call("_ZdlPv", p);
    t.call("_ZdlPv", 0); // deleting null does nothing
    const u32 a = static_cast<u32>(t.call("_Znajj", 64, 256));
    CHECK(a % 256 == 0);
    t.call("_ZdaPvj", a, 256);
    const u32 z = static_cast<u32>(t.call("_Znaj", 0));
    CHECK(z != 0);
    t.call("_ZdaPv", z);
    t.call("_ZdlPvS_", z, z); // placement: nothing
    // nothrow: null for what malloc can't give, no handler set.
    CHECK(t.call("_ZnwjRKSt9nothrow_t", 0xFFFFFFF0u, 0) == 0);
    CHECK(t.call("_ZnajjRKSt9nothrow_t", 0xFFFFFFF0u, 16, 0) == 0);
    // set_new_handler answers the old one (none at first); a handler runs while malloc fails. This one removes
    // itself (set_new_handler(0)), so the nothrow form then gives null.
    const u32 setnh = desc("_ZSt15set_new_handlerPFvvE");
    const u32 rec = recorder(c, kRec);
    const u32 self_removing =
        c.add({0x7C0802A6u /*mflr r0*/, 0xF8010010u /*std r0,16(r1)*/, 0xF821FF81u /*stdu r1,-128(r1)*/,
            lis(11, kRec >> 16), ori(11, 11, kRec), lwz(12, 11, 12), addi(12, 12, 1), stw(12, 11, 12), li3(0),
            lis(12, setnh >> 16), ori(12, 12, setnh), lwz(0, 12, 0), 0x7C0903A6u /*mtctr r0*/, lwz(2, 12, 4),
            0x4E800421u /*bctrl*/, 0x38210080u /*addi r1,r1,128*/, 0xE8010010u /*ld r0,16(r1)*/,
            0x7C0803A6u /*mtlr r0*/, kBlr});
    CHECK(t.call("_ZSt15set_new_handlerPFvvE", rec) == 0);
    CHECK(t.call("_ZSt15set_new_handlerPFvvE", self_removing) == rec);
    ps_write32(b, kRec + 12, 0);
    CHECK(t.call("_ZnwjRKSt9nothrow_t", 0xFFFFFFF0u, 0) == 0);
    CHECK(ps_read32(b, kRec + 12) == 1);
    CHECK(t.call("_ZSt15set_new_handlerPFvvE", 0) == 0);
}

TEST_CASE("sys_libstdcxx: operator new raises bad_alloc with no handler", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    Tty tty;
    CHECK_THROWS_AS(t.call("_Znwj", 0xFFFFFFF0u), lv2::ThreadExit);
    CHECK(tty.get().find("exception: bad allocation") != std::string::npos);
}

TEST_CASE("sys_libstdcxx: _Raise calls the raise handler and _Doraise, then _Throw ends the program", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    Tty tty;
    const u32 rec = recorder(c, kRec);
    CHECK(t.call("_ZNSt9exception18_Set_raise_handlerEPFvRKS_E", rec) == 0);
    CHECK(t.call("_ZNSt9exception18_Set_raise_handlerEPFvRKS_E", rec) == rec);
    t.call("_ZNSt8bad_castC1Ev", kObj);
    t.call("_ZNKSt9exception8_DoraiseEv", kObj); // exception's: nothing
    CHECK_THROWS_AS(t.call("_ZNKSt9exception6_RaiseEv", kObj), lv2::ThreadExit);
    CHECK(ps_read32(b, kRec) == kObj); // the handler got the exception
    CHECK(ps_read32(b, kRec + 12) == 1);
    CHECK(tty.get().find("exception: bad cast") != std::string::npos);
}

TEST_CASE("sys_libstdcxx: the strings' _Xlen and _Xran raise length_error and out_of_range", "[hle][libstdcxx]")
{
    SECTION("_Xlen")
    {
        Fixture f;
        GuestThread t(f, 0, "ppu");
        Tty tty;
        CHECK_THROWS_AS(t.call("_ZNKSt12_String_base5_XlenEv", 0), lv2::ThreadExit);
        CHECK(tty.get().find("exception: string too long") != std::string::npos);
    }
    SECTION("erase past the end")
    {
        Fixture f;
        GuestThread t(f, 0, "ppu");
        Tty tty;
        t.call("_ZNSsC1Ev", kStr);
        CHECK_THROWS_AS(t.call("_ZNSs5eraseEjj", kStr, 1, 0), lv2::ThreadExit);
        CHECK(tty.get().find("exception: invalid string position") != std::string::npos);
    }
}

TEST_CASE("sys_libstdcxx: terminate and unexpected through their handlers", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    const u32 rec = recorder(c, kRec);
    // The defaults handed back are the library's own (terminate's aborts, unexpected's terminates).
    const u32 def_t = static_cast<u32>(t.call("_ZSt13set_terminatePFvvE", rec));
    CHECK(fn_name(b, def_t) == "_pspocket_stdcxx_default_terminate");
    CHECK(t.call("_ZSt13set_terminatePFvvE", rec) == rec);
    const u32 def_u = static_cast<u32>(t.call("_ZSt14set_unexpectedPFvvE", rec));
    CHECK(fn_name(b, def_u) == "_pspocket_stdcxx_default_unexpected");
    // terminate: the handler, then abort (a handler that returns doesn't save the program).
    CHECK_THROWS_AS(t.call("_ZSt9terminatev"), lv2::ThreadExit);
    CHECK(ps_read32(b, kRec + 12) == 1);
}

TEST_CASE("sys_libstdcxx: _Debug_message reports on stderr and ends the program", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Tty tty;
    put(b, kBuf, "invalid iterator");
    put(b, kBuf2, "vector.h");
    CHECK_THROWS_AS(t.call("_ZSt14_Debug_messagePKcS0_", kBuf, kBuf2), lv2::ThreadExit);
    CHECK(tty.get().find("vector.h: invalid iterator") != std::string::npos);
}

TEST_CASE("sys_libstdcxx: basic_string<char>, Dinkumware's capacities", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const GString s{b, kStr}, r{b, kStr + 0x40};
    // basic_string(): empty, in the small buffer, capacity 15.
    t.call("_ZNSsC1Ev", kStr);
    CHECK(s.size() == 0);
    CHECK(s.res() == 15);
    CHECK(s.terminator() == 0);
    // append(n, c) within the buffer, then past it: _Copy rounds up to _ALLOC_MASK (15).
    CHECK(t.call("_ZNSs6appendEjc", kStr, 5, 'a') == kStr);
    CHECK(s.text() == "aaaaa");
    CHECK(s.res() == 15);
    t.call("_ZNSs6appendEjc", kStr, 15, 'b');
    CHECK(s.text() == "aaaaabbbbbbbbbbbbbbb");
    CHECK(s.res() == 31);
    CHECK(s.ptr(16) != kStr + 4);
    // ... and half again when that is more.
    t.call("_ZNSs6appendEjc", kStr, 13, 'c'); // 33 | 15 = 47; 47 / 3 = 15 < 31 / 2 = 15 is false: 47
    CHECK(s.size() == 33);
    CHECK(s.res() == 47);
    t.call("_ZNSs6appendEjc", kStr, 15, 'd'); // 48 | 15 = 63; 63 / 3 = 21 < 47 / 2 = 23: 47 + 23
    CHECK(s.res() == 70);
    CHECK(s.terminator() == 0);
    // insert, erase, replace.
    t.call("_ZNSs5eraseEjj", kStr, 5, 0xFFFFFFFFu);
    CHECK(s.text() == "aaaaa");
    t.call("_ZNSs6insertEjjc", kStr, 2, 3, 'x');
    CHECK(s.text() == "aaxxxaaa");
    put(b, kBuf, "HELLO");
    t.call("_ZNSs7replaceEjjPKcj", kStr, 1, 4, kBuf, 5);
    CHECK(s.text() == "aHELLOaaa");
    // replace with a piece of itself: as if copied out first.
    t.call("_ZNSs7replaceEjjPKcj", kStr, 0, 2, s.ptr(16) + 3, 4);
    CHECK(s.text() == "LLOaELLOaaa");
    t.call("_ZNSs7replaceEjjPKcj", kStr, 3, 100, kBuf, 0);
    CHECK(s.text() == "LLO");
    // _Grow with _Trim to a small size goes back into the buffer.
    CHECK(t.call("_ZNSs5_GrowEjb", kStr, 2, 1) == 1);
    CHECK(s.res() == 15);
    CHECK(s.text() == "LL");
    CHECK(t.call("_ZNSs5_GrowEjb", kStr, 0, 0) == 0);
    CHECK(s.size() == 0);
    // basic_string(const char *), basic_string(const string&), assign(str, off, n).
    put(b, kBuf, "the quick brown fox");
    t.call("_ZNSsC1EPKc", kStr + 0x40, kBuf);
    CHECK(r.text() == "the quick brown fox");
    CHECK(r.res() == 31);
    t.call("_ZNSsD1Ev", kStr);
    t.call("_ZNSsC1ERKSs", kStr, kStr + 0x40);
    CHECK(s.text() == "the quick brown fox");
    t.call("_ZNSs6assignERKSsjj", kStr, kStr + 0x40, 4, 5);
    CHECK(s.text() == "quick");
    t.call("_ZNSs6assignERKSsjj", kStr, kStr, 1, 3); // itself: a substring
    CHECK(s.text() == "uic");
    // _Tidy(true, n) keeps the first n elements in the buffer.
    t.call("_ZNSs5_TidyEbj", kStr + 0x40, 1, 3);
    CHECK(r.res() == 15);
    CHECK(r.text() == "the");
    t.call("_ZNSsD1Ev", kStr);
    t.call("_ZNSsD1Ev", kStr + 0x40);
}

TEST_CASE("sys_libstdcxx: basic_string<wchar_t>, four-byte elements", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const GString s{b, kStr}, r{b, kStr + 0x40};
    t.call("_ZNSbIwSt11char_traitsIwESaIwEEC1Ev", kStr);
    CHECK(s.res() == 3); // _BUF_SIZE 4
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE6appendEjw", kStr, 3, 0x263A);
    CHECK(s.res() == 3);
    CHECK(s.wtext() == U"☺☺☺");
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE6appendEjw", kStr, 2, 'z'); // 5 | 3 = 7
    CHECK(s.res() == 7);
    CHECK(s.wtext() == U"☺☺☺zz");
    CHECK(ps_read32(b, s.ptr(4) + 4 * 5) == 0);
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE6insertEjjw", kStr, 0, 1, 'A');
    CHECK(s.wtext() == U"A☺☺☺zz");
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE5eraseEjj", kStr, 1, 3);
    CHECK(s.wtext() == U"Azz");
    for (u32 i = 0; i < 3; i++) ps_write32(b, kBuf + 4 * i, U"xyz"[i]);
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE7replaceEjjPKwj", kStr, 1, 1, kBuf, 3);
    CHECK(s.wtext() == U"Axyzz");
    t.call("_ZNSbIwSt11char_traitsIwESaIwEEC1Ev", kStr + 0x40);
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_jj", kStr + 0x40, kStr, 1, 2);
    CHECK(r.wtext() == U"xy");
    CHECK(t.call("_ZNSbIwSt11char_traitsIwESaIwEE5_GrowEjb", kStr, 2, 1) == 1);
    CHECK(s.res() == 3);
    CHECK(s.wtext() == U"Ax");
    t.call("_ZNSbIwSt11char_traitsIwESaIwEE5_TidyEbj", kStr, 1, 0);
    CHECK(s.size() == 0);
}

TEST_CASE("sys_libstdcxx: _Mutex over the C library's locks", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    t.call("_ZNSt6_MutexC1Ev", kObj);
    CHECK(ps_read32(b, kObj) != 0); // the lock object, from operator new
    t.call("_ZNSt6_Mutex5_LockEv", kObj);
    t.call("_ZNSt6_Mutex5_LockEv", kObj); // recursive
    t.call("_ZNSt6_Mutex7_UnlockEv", kObj);
    t.call("_ZNSt6_Mutex7_UnlockEv", kObj);
    t.call("_ZNSt6_MutexD1Ev", kObj);
}

TEST_CASE("sys_libstdcxx: the global and classic locales, facets and their counts", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    // _Init: the thread's global _Locimp, "C", every category, counted by the classic locale too.
    const u32 imp = static_cast<u32>(t.call("_ZNSt6locale5_InitEv"));
    REQUIRE(imp != 0);
    CHECK(t.call("_ZNSt6locale5_InitEv") == imp);
    CHECK(ps_read32(b, imp + 4) == 2);
    CHECK(ps_read32(b, imp + 16) == 63);
    CHECK(GString{b, imp + 24}.text() == "C");
    CHECK(b[imp + 20] == 0);
    const u32 classic = static_cast<u32>(t.call("_ZNSt6locale7classicEv"));
    CHECK(ps_read32(b, classic) == imp);
    CHECK(type_name(b, imp) == "NSt6locale7_LocimpE");
    // locale() counts the global one; ~locale gives it back.
    make_locale(t, kObj);
    CHECK(ps_read32(b, kObj) == imp);
    CHECK(ps_read32(b, imp + 4) == 3);
    CHECK(t.call("_ZNKSt6localeeqERKS_", kObj, classic) == 1);
    t.call("_ZNSt6localeD1Ev", kObj);
    CHECK(ps_read32(b, imp + 4) == 2);
    // locale::id numbers ids from 1 at first use.
    ps_write32(b, kBuf, 0);
    ps_write32(b, kBuf + 4, 0);
    const u32 id1 = static_cast<u32>(t.call("_ZNSt6locale2idcvjEv", kBuf));
    const u32 id2 = static_cast<u32>(t.call("_ZNSt6locale2idcvjEv", kBuf + 4));
    CHECK(id1 >= 1);
    CHECK(id2 == id1 + 1);
    CHECK(t.call("_ZNSt6locale2idcvjEv", kBuf) == id1);
    // A guest facet (its deleting destructor recorded) added to a shared locale: copy on write, then counted. (A
    // copy of the classic locale would get every standard facet, the facets part's: an empty() one here.)
    const u32 rec = recorder(c, kRec);
    ps_write32(b, kVt, 0);
    ps_write32(b, kVt + 4, 0);
    ps_write32(b, kVt + 8, rec);
    ps_write32(b, kVt + 12, rec);
    ps_write32(b, kObj2, kVt + 8);
    ps_write32(b, kObj2 + 4, 0);
    t.call("_ZNSt6locale5emptyEv", kObj);
    const u32 shared = ps_read32(b, kObj);
    ps_write32(b, kObj + 4, shared); // a second locale on it
    t.call("_ZNSt6locale5facet7_IncrefEv", shared);
    CHECK(t.call("_ZNSt6locale7_AddfacEPNS_5facetEjj", kObj, kObj2, id2, 0) == kObj);
    const u32 mine = ps_read32(b, kObj);
    CHECK(mine != shared); // a private copy
    CHECK(ps_read32(b, shared + 4) == 1);
    CHECK(ps_read32(b, mine + 4) == 1);
    CHECK(ps_read32(b, mine + 12) >= 40); // at least _MINCAT entries
    CHECK(ps_read32(b, kObj2 + 4) == 1);
    CHECK(t.call("_ZNKSt6locale9_GetfacetEj", kObj, id2) == kObj2);
    CHECK(t.call("_ZNKSt6locale9_GetfacetEj", kObj, id1) == 0);
    CHECK(t.call("_ZNKSt6locale9_GetfacetEj", classic, id2) == 0);
    CHECK(t.call("_ZNKSt6localeeqERKS_", kObj, kObj) == 1);
    CHECK(t.call("_ZNKSt6localeeqERKS_", kObj, kObj + 4) == 0); // unnamed ("*") locales: only the same _Locimp
    t.call("_ZNSt6locale7_AddfacEPNS_5facetEjj", kObj, kObj2, id2, 2); // not shared: no copy this time
    CHECK(ps_read32(b, kObj) == mine);
    CHECK(GString{b, mine + 24}.text() == "*");
    t.call("_ZNSt6localeD1Ev", kObj + 4);
    // _Incref/_Decref: _Decref answers the facet when its count reaches 0.
    t.call("_ZNSt6locale5facet7_IncrefEv", kObj2);
    CHECK(ps_read32(b, kObj2 + 4) == 2);
    CHECK(t.call("_ZNSt6locale5facet7_DecrefEv", kObj2) == 0);
    // The locale's end releases the facet: its count 0, its deleting destructor (slot 1) called.
    ps_write32(b, kRec + 12, 0);
    t.call("_ZNSt6localeD1Ev", kObj);
    CHECK(ps_read32(b, kObj2 + 4) == 0);
    CHECK(ps_read32(b, kRec + 12) == 1);
    CHECK(ps_read32(b, kRec) == kObj2);
    // A count of (size_t)-1 is never changed.
    ps_write32(b, kObj2 + 4, 0xFFFFFFFFu);
    t.call("_ZNSt6locale5facet7_IncrefEv", kObj2);
    CHECK(t.call("_ZNSt6locale5facet7_DecrefEv", kObj2) == 0);
    CHECK(ps_read32(b, kObj2 + 4) == 0xFFFFFFFFu);
    // _Register queues a lazy facet; _Fac_tidy releases it.
    ps_write32(b, kObj2 + 4, 1);
    ps_write32(b, kRec + 12, 0);
    t.call("_ZNSt6locale5facet9_RegisterEv", kObj2);
    t.call("_Fac_tidy");
    CHECK(ps_read32(b, kRec + 12) == 1);
    t.call("_Fac_tidy");
    CHECK(ps_read32(b, kRec + 12) == 1);
    // empty(): a transparent locale with no facets; _Getfacet looks in the global locale for it.
    t.call("_ZNSt6locale5emptyEv", kObj);
    const u32 e = ps_read32(b, kObj);
    CHECK(b[e + 20] == 1);
    CHECK(ps_read32(b, e + 12) == 0);
    ps_write32(b, kObj2 + 4, 0);
    t.call("_ZNSt6locale7_Locimp7_AddfacEPNS_5facetEj", imp, kObj2, id1);
    CHECK(t.call("_ZNKSt6locale9_GetfacetEj", kObj, id1) == kObj2);
    // global(loc): the old global back; the thread's global is the new one.
    t.call("_ZNSt6locale6globalERKS_", kOut, kObj);
    CHECK(ps_read32(b, kOut) == imp);
    CHECK(t.call("_ZNSt6locale5_InitEv") == e);
    t.call("_ZNSt6locale6globalERKS_", kOut + 4, kOut);
    CHECK(t.call("_ZNSt6locale5_InitEv") == imp);
    t.call("_ZNSt6localeD1Ev", kOut);
    t.call("_ZNSt6localeD1Ev", kOut + 4);
    t.call("_ZNSt6localeD1Ev", kObj);
    // _Locimp(const _Locimp&) of a locale other than the classic one shares its facets.
    t.call("_ZNSt6locale7_LocimpC1ERKS0_", kObj3, e);
    CHECK(ps_read32(b, kObj3 + 12) == ps_read32(b, e + 12));
    CHECK(b[kObj3 + 20] == 1);
    t.call("_ZNSt6locale7_LocimpD1Ev", kObj3);
    // _Deletegloballocale(&ptr) releases the _Locimp a variable holds.
    t.call("_ZNSt6locale7_LocimpC1Eb", kObj3, 1);
    CHECK(GString{b, kObj3 + 24}.text() == "*");
    CHECK(ps_read32(b, kObj3 + 4) == 1);
    t.call("_ZNSt6locale5facet7_IncrefEv", kObj3);
    ps_write32(b, kOut, kObj3);
    t.call("_Deletegloballocale", kOut);
    CHECK(ps_read32(b, kObj3 + 4) == 1);
    // __Setgloballocale: the thread's global pointer.
    t.call("__Setgloballocale", kObj3);
    CHECK(t.call("_ZNSt6locale5_InitEv") == kObj3);
    t.call("__Setgloballocale", imp);
}

TEST_CASE("sys_libstdcxx: _Locinfo, the C++ library's \"C\" locale", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    put(b, kBuf, "C");
    t.call("_ZNSt8_LocinfoC1EPKc", kObj, kBuf);
    CHECK(GString{b, kObj + 88}.text() == "C");
    CHECK(GString{b, kObj + 116}.text() == "C");
    CHECK(cstr(b, ps_read32(b, kObj + 4)) == ":AM:PM"); // the time names (_Tinfo)
    CHECK(cstr(b, ps_read32(b, kObj + 8)).starts_with(":Sun:Sunday:Mon"));
    CHECK(cstr(b, ps_read32(b, kObj + 20)).starts_with(":Jan:January"));
    CHECK(cstr(b, ps_read32(b, kObj + 32)) == "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p");
    // _Addcats with a name that isn't a locale makes the name "*"; "C" leaves "C".
    put(b, kBuf2, "fr_FR");
    CHECK(t.call("_ZNSt8_Locinfo8_AddcatsEiPKc", kObj, 8, kBuf) == kObj);
    CHECK(GString{b, kObj + 116}.text() == "C");
    t.call("_ZNSt8_Locinfo8_AddcatsEiPKc", kObj, 8, kBuf2);
    CHECK(GString{b, kObj + 116}.text() == "*");
    // The tables and states.
    t.call("_ZNKSt8_Locinfo9_GetctypeEv", kOut, kObj);
    const u32 ctab = HleProcess::current()->link_variable("sys_libc", recomp::nid_of("_Ctype"));
    CHECK(ps_read32(b, kOut) == ps_read32(b, ctab));
    CHECK(ps_read32(b, kOut + 12) == 0);
    t.call("_ZNKSt8_Locinfo7_GetcvtEv", kOut, kObj);
    CHECK(ps_read32(b, kOut) != 0);
    t.call("_ZNKSt8_Locinfo8_GetcollEv", kOut, kObj);
    CHECK(ps_read32(b, kOut) != 0);
    t.call("_ZNSt8_LocinfoD1Ev", kObj);
    // A name that isn't "C".
    t.call("_ZNSt8_LocinfoC1EiPKc", kObj, 63, kBuf2);
    CHECK(GString{b, kObj + 116}.text() == "*");
    t.call("_ZNSt8_LocinfoD1Ev", kObj);
}

TEST_CASE("sys_libstdcxx: a bad locale name raises runtime_error", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Tty tty;
    put(b, kBuf, "de_DE");
    CHECK_THROWS_AS(t.call("_ZNSt6localeC1EPKci", kObj, kBuf, 63), lv2::ThreadExit);
    CHECK(tty.get().find("exception: bad locale name") != std::string::npos);
}

TEST_CASE("sys_libstdcxx: use_facet takes the locale's facet, or raises bad_cast without one", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    const u32 fac = test_facet(t, c, kObj2, kVt, "St5ctypeIwE", 14);
    make_locale(t, kObj);
    CHECK(t.call("_ZSt9use_facetISt5ctypeIwEERKT_RKSt6locale", kObj) == fac);
    if (registry().find(kMod, recomp::nid_of("_ZNSt8numpunctIcE7_GetcatEPPKNSt6locale5facetE"))) {
        // With the facets part: a lazy numpunct<char>, the same one each time.
        const u32 np = static_cast<u32>(t.call("_ZSt9use_facetISt8numpunctIcEERKT_RKSt6locale", kObj));
        CHECK(np != 0);
        CHECK(t.call("_ZSt9use_facetISt8numpunctIcEERKT_RKSt6locale", kObj) == np);
    } else {
        Tty tty;
        CHECK_THROWS_AS(t.call("_ZSt9use_facetISt8numpunctIcEERKT_RKSt6locale", kObj), lv2::ThreadExit);
        CHECK(tty.get().find("exception: bad cast") != std::string::npos);
    }
}

TEST_CASE("sys_libstdcxx: ios_base's state, format, arrays, callbacks and locale", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    const u32 ios = kObj, other = kObj + 0x100;
    std::memset(b + ios, 0, 0x200);
    t.call("_ZNSt8ios_base5_InitEv", ios);
    CHECK(ps_read32(b, ios + 16) == 0x201); // skipws | dec
    CHECK(ps_read32(b, ios + 20) == 6);
    CHECK(ps_read32(b, ios + 8) == 0);
    const u32 ploc = ps_read32(b, ios + 36);
    REQUIRE(ploc != 0);
    CHECK(ps_read32(b, ploc) == t.call("_ZNSt6locale5_InitEv"));
    // clear: the state masked; no raise while the exception mask is empty.
    t.call("_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", ios, 0xFF, 0);
    CHECK(ps_read32(b, ios + 8) == 0x17);
    t.call("_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", ios, 0, 0);
    // _Findarr: one element per index, recycled when empty.
    const u32 a5 = static_cast<u32>(t.call("_ZNSt8ios_base8_FindarrEi", ios, 5));
    CHECK(ps_read32(b, a5 + 4) == 5);
    ps_write32(b, a5 + 8, 77);
    CHECK(t.call("_ZNSt8ios_base8_FindarrEi", ios, 5) == a5);
    const u32 a6 = static_cast<u32>(t.call("_ZNSt8ios_base8_FindarrEi", ios, 6));
    CHECK(a6 != a5);
    CHECK(t.call("_ZNSt8ios_base8_FindarrEi", ios, 7) == a6); // a6 is empty: recycled
    // Callbacks: the latest first, (event, ios, index).
    const u32 r1 = recorder(c, kRec), r2 = recorder(c, kRec + 0x20);
    t.call("_ZNSt8ios_base17register_callbackEPFvNS_5eventERS_iEi", ios, r1, 11);
    t.call("_ZNSt8ios_base17register_callbackEPFvNS_5eventERS_iEi", ios, r2, 22);
    t.call("_ZNSt8ios_base8_CallfnsENS_5eventE", ios, 1);
    CHECK(ps_read32(b, kRec) == 1);
    CHECK(ps_read32(b, kRec + 4) == ios);
    CHECK(ps_read32(b, kRec + 8) == 11);
    CHECK(ps_read32(b, kRec + 0x28) == 22);
    // copyfmt: the format, the nonzero array values and the callbacks copied; copyfmt_event (2) on the copy.
    std::memset(b + other, 0, 40);
    t.call("_ZNSt8ios_base5_InitEv", other);
    ps_write32(b, ios + 20, 9);
    ps_write32(b, ios + 24, 4);
    CHECK(t.call("_ZNSt8ios_base7copyfmtERKS_", other, ios) == other);
    CHECK(ps_read32(b, other + 20) == 9);
    CHECK(ps_read32(b, other + 24) == 4);
    CHECK(ps_read32(b, static_cast<u32>(t.call("_ZNSt8ios_base8_FindarrEi", other, 5)) + 8) == 77);
    CHECK(ps_read32(b, kRec) == 2);
    CHECK(ps_read32(b, kRec + 4) == other);
    // imbue: the old locale back, imbue_event.
    make_locale(t, kObj2);
    ps_write32(b, kRec, 0xFF);
    t.call("_ZNSt8ios_base5imbueERKSt6locale", kOut, ios, kObj2);
    CHECK(ps_read32(b, kOut) == ps_read32(b, kObj2));
    CHECK(ps_read32(b, kRec) == 1);
    t.call("_ZNSt6localeD1Ev", kOut);
    t.call("_ZNSt6localeD1Ev", kObj2);
    // _Addstd: a standard stream's slot, counted; the destructor only tidies at the last close.
    t.call("_ZNSt8ios_base7_AddstdEv", other);
    const u32 slot = ps_read32(b, other + 4);
    CHECK(slot >= 1);
    t.call("_ZNSt8ios_base7_AddstdEv", other);
    CHECK(ps_read32(b, other + 4) == slot);
    ps_write32(b, kRec + 12, 0);
    t.call("_ZNSt8ios_baseD1Ev", other); // two opens: nothing yet
    CHECK(ps_read32(b, kRec + 12) == 0);
    t.call("_ZNSt8ios_baseD1Ev", other); // the last: erase_event (0) and the arrays freed
    CHECK(ps_read32(b, kRec + 12) == 1);
    CHECK(ps_read32(b, kRec) == 0);
    t.call("_ZNSt8ios_base5_TidyEv", ios);
    CHECK(ps_read32(b, ios + 28) == 0);
    CHECK(ps_read32(b, ios + 32) == 0);
    t.call("_ZNSt8ios_baseD1Ev", ios);
}

TEST_CASE("sys_libstdcxx: clear raises ios_base::failure for a state the mask asks for", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Tty tty;
    std::memset(b + kObj, 0, 40);
    t.call("_ZNSt8ios_base5_InitEv", kObj);
    ps_write32(b, kObj + 12, 2); // exceptions(failbit)
    t.call("_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", kObj, 1, 0); // eofbit: not asked for
    t.call("_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", kObj, 2, 1); // _Reraise: nothing to rethrow
    CHECK_THROWS_AS(t.call("_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", kObj, 6, 0), lv2::ThreadExit);
    CHECK(tty.get().find("exception: ios_base::failbit set") != std::string::npos);
}

TEST_CASE("sys_libstdcxx: basic_streambuf's xsputn, xsgetn and uflow through the vtable", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    // A stream buffer of the library's vtable, but for overflow and underflow (guest functions that record).
    const u32 vt = lib_vtable(b, kVt, kStreambufC);
    const u32 over = recorder(c, kRec, 0x41), under = recorder(c, kRec + 0x20, -1);
    ps_write32(b, vt + 16, over);
    ps_write32(b, vt + 28, under);
    hand_streambuf(b, kObj, vt);
    // xsputn into an 8-byte put area: the rest goes through overflow, one character each.
    setp(b, kObj, kBuf, kBuf, kBuf + 8);
    put(b, kStr, "0123456789AB");
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", kObj, kStr, 12) == 12);
    CHECK(std::string(reinterpret_cast<const char*>(b + kBuf), 8) == "01234567");
    CHECK(ps_read32(b, kObj + 28) == kBuf + 8);
    CHECK(ps_read32(b, kRec + 12) == 4);
    CHECK(ps_read32(b, kRec + 4) == 'B'); // the last character offered
    // xsgetn from a get area of 5, then uflow (underflow answering eof) ends it.
    put(b, kBuf2, "hello");
    setg(b, kObj, kBuf2, kBuf2, kBuf2 + 5);
    std::memset(b + kOut, 0, 16);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", kObj, kOut, 10) == 5);
    CHECK(cstr(b, kOut) == "hello");
    CHECK(ps_read32(b, kRec + 0x20 + 12) == 1);
    CHECK(i32(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv", kObj)) == -1); // underflow is eof
    // The defaults: eof, 0, this, BADOFF.
    CHECK(i32(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE9underflowEv", kObj)) == -1);
    CHECK(i32(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE8overflowEi", kObj, 'x')) == -1);
    CHECK(i32(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi", kObj, 'x')) == -1);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv", kObj) == 0);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv", kObj) == 0);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPci", kObj, 0, 0) == kObj);
    t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE", kOut,
        kObj, 3, 0, 3);
    CHECK(i32(ps_read32(b, kOut)) == -1);
    CHECK(ps_read32(b, kOut + 4) == 0);
    t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE", kOut,
        kObj, 0, 0, 0, 3);
    CHECK(i32(ps_read32(b, kOut)) == -1);
    // The wide stream buffer's xsputn with four-byte elements (its overflow the library's: eof).
    const u32 wvt = lib_vtable(b, kVt + 0x100,
        {"_ZNSt15basic_streambufIwSt11char_traitsIwEED1Ev", "_ZNSt15basic_streambufIwSt11char_traitsIwEED0Ev",
            "_ZNSt15basic_streambufIwSt11char_traitsIwEE5_LockEv",
            "_ZNSt15basic_streambufIwSt11char_traitsIwEE7_UnlockEv",
            "_ZNSt15basic_streambufIwSt11char_traitsIwEE8overflowEi",
            "_ZNSt15basic_streambufIwSt11char_traitsIwEE9pbackfailEi",
            "_ZNSt15basic_streambufIwSt11char_traitsIwEE9showmanycEv",
            "_ZNSt15basic_streambufIwSt11char_traitsIwEE9underflowEv"});
    hand_streambuf(b, kObj2, wvt);
    setp(b, kObj2, kBuf, kBuf, kBuf + 8);
    for (u32 i = 0; i < 3; i++) ps_write32(b, kStr + 4 * i, 0x3041 + i);
    CHECK(t.call("_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwi", kObj2, kStr, 3) == 2);
    CHECK(ps_read32(b, kBuf) == 0x3041);
    CHECK(ps_read32(b, kBuf + 4) == 0x3042);
    // The stream buffer iterators over it: _Peek, operator*, _Inc; ostreambuf_iterator's operator=.
    for (u32 i = 0; i < 2; i++) ps_write32(b, kBuf2 + 4 * i, 0x263A + i);
    setg(b, kObj2, kBuf2, kBuf2, kBuf2 + 8);
    ps_write32(b, kOut, kObj2);
    b[kOut + 4] = 0;
    CHECK(t.call("_ZNKSt19istreambuf_iteratorIwSt11char_traitsIwEEdeEv", kOut) == 0x263A);
    CHECK(b[kOut + 4] == 1);
    t.call("_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEE4_IncEv", kOut);
    CHECK(b[kOut + 4] == 0);
    CHECK(t.call("_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEE5_PeekEv", kOut) == 0x263B);
    t.call("_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEE4_IncEv", kOut);
    t.call("_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEE5_PeekEv", kOut); // at the end: the buffer gone
    CHECK(ps_read32(b, kOut) == 0);
    setp(b, kObj2, kBuf, kBuf, kBuf + 4);
    b[kOut + 8] = 0;
    ps_write32(b, kOut + 12, kObj2);
    t.call("_ZNSt19ostreambuf_iteratorIwSt11char_traitsIwEEaSEw", kOut + 8, 'Q');
    CHECK(ps_read32(b, kBuf) == 'Q');
    CHECK(b[kOut + 8] == 0);
    t.call("_ZNSt19ostreambuf_iteratorIwSt11char_traitsIwEEaSEw", kOut + 8, 'R'); // full, overflow eof: failed
    CHECK(b[kOut + 8] == 1);
    // istreambuf_iterator<char>::_Peek.
    put(b, kBuf2, "z");
    setg(b, kObj, kBuf2, kBuf2, kBuf2 + 1);
    ps_write32(b, kOut, kObj);
    CHECK(t.call("_ZNSt19istreambuf_iteratorIcSt11char_traitsIcEE5_PeekEv", kOut) == 'z');
    CHECK(b[kOut + 5] == 'z');
}

TEST_CASE("sys_libstdcxx: strstreambuf, dynamic and over a static array", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const u32 vt = lib_vtable(b, kVt,
        {"_ZNSt12strstreambufD1Ev", "_ZNSt12strstreambufD0Ev", "_ZNSt15basic_streambufIcSt11char_traitsIcEE5_LockEv",
            "_ZNSt15basic_streambufIcSt11char_traitsIcEE7_UnlockEv", "_ZNSt12strstreambuf8overflowEi",
            "_ZNSt12strstreambuf9pbackfailEi", "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
            "_ZNSt12strstreambuf9underflowEv", "_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv",
            "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci",
            "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci"});
    const u32 sb = kObj;
    std::memset(b + sb, 0, 84);
    ps_write32(b, sb, vt);
    // Dynamic: written through xsputn (overflow allocates), read back through xsgetn (underflow follows writes).
    t.call("_ZNSt12strstreambuf5_InitEiPcS0_i", sb, 0, 0, 0, 0);
    CHECK(ps_read32(b, sb + 72) == 4); // _Dynamic
    CHECK(ps_read32(b, sb + 68) == 512);
    put(b, kStr, "streams of text");
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", sb, kStr, 15) == 15);
    CHECK((ps_read32(b, sb + 72) & 1) == 1); // _Allocated
    std::memset(b + kOut, 0, 32);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", sb, kOut, 7) == 7);
    CHECK(cstr(b, kOut) == "streams");
    // seekoff: in from the beginning, out from the end; seekpos.
    t.call("_ZNSt12strstreambuf7seekoffElNSt5_IosbIiE8_SeekdirENS1_9_OpenmodeE", kOut, sb, 3, 0, 1);
    CHECK(ps_read32(b, kOut) == 3);
    CHECK(i32(t.call("_ZNSt12strstreambuf9underflowEv", sb)) == 'e');
    t.call("_ZNSt12strstreambuf7seekoffElNSt5_IosbIiE8_SeekdirENS1_9_OpenmodeE", kOut, sb, -4, 2, 2);
    CHECK(ps_read32(b, kOut) == 11);
    t.call("_ZNSt12strstreambuf7seekoffElNSt5_IosbIiE8_SeekdirENS1_9_OpenmodeE", kOut, sb, 99, 0, 1);
    CHECK(i32(ps_read32(b, kOut)) == -1);
    t.call("_ZNSt12strstreambuf7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE", kOut, sb, u64{1} << 32, 0, 0,
        1);
    CHECK(ps_read32(b, kOut) == 1);
    CHECK(i32(t.call("_ZNSt12strstreambuf9underflowEv", sb)) == 't');
    // pbackfail: back one, writing a different character (not constant).
    CHECK(i32(t.call("_ZNSt12strstreambuf9pbackfailEi", sb, 'S')) == 'S');
    CHECK(i32(t.call("_ZNSt12strstreambuf9underflowEv", sb)) == 'S');
    // freeze: no more writing; then unfrozen, writing again.
    t.call("_ZNSt12strstreambuf6freezeEb", sb, 1);
    CHECK((ps_read32(b, sb + 72) & 8) == 8);
    CHECK(i32(t.call("_ZNSt12strstreambuf8overflowEi", sb, 'x')) == -1);
    t.call("_ZNSt12strstreambuf6freezeEb", sb, 0);
    CHECK(i32(t.call("_ZNSt12strstreambuf8overflowEi", sb, 'x')) == 'x');
    t.call("_ZNSt12strstreambufD1Ev", sb);
    CHECK((ps_read32(b, sb + 72) & 1) == 0);
    // Static, read-only: over "abc" (a count of 0 is the string's length).
    put(b, kBuf, "abc");
    std::memset(b + sb, 0, 84);
    ps_write32(b, sb, vt);
    t.call("_ZNSt12strstreambuf5_InitEiPcS0_i", sb, 0, kBuf, 0, 2);
    std::memset(b + kOut, 0, 8);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", sb, kOut, 8) == 3);
    CHECK(cstr(b, kOut) == "abc");
    CHECK(i32(t.call("_ZNSt12strstreambuf9pbackfailEi", sb, 'z')) == -1); // constant and different
    CHECK(i32(t.call("_ZNSt12strstreambuf9pbackfailEi", sb, 'c')) == 'c');
    CHECK(i32(t.call("_ZNSt12strstreambuf8overflowEi", sb, 'x')) == -1);
    // Static and writable: put area from the put pointer to the end.
    std::memset(b + kBuf, '.', 8);
    std::memset(b + sb, 0, 84);
    ps_write32(b, sb, vt);
    t.call("_ZNSt12strstreambuf5_InitEiPcS0_i", sb, 8, kBuf, kBuf + 2, 0);
    put(b, kStr, "XYZ");
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", sb, kStr, 3) == 3);
    CHECK(std::string(reinterpret_cast<const char*>(b + kBuf), 8) == "..XYZ...");
    t.call("_ZNSt12strstreambuf5_TidyEv", sb);
}

TEST_CASE("sys_libstdcxx: basic_filebuf over sys_libc's FILEs, _Fiopen", "[hle][libstdcxx]")
{
    const fsys::path root = temp_root();
    {
        Fixture f;
        GuestThread t(f, 0, "ppu");
        u8* b = f.base();
        Code c{b};
        REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
        stream_facets(t, c);
        const u32 vt = lib_vtable(b, kVt,
            {"_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev", "_ZNSt13basic_filebufIcSt11char_traitsIcEED0Ev",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE5_LockEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE7_UnlockEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE8overflowEi",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE9pbackfailEi",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE9underflowEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci"});
        // _Fiopen: invalid combinations refused; "w" for out.
        put(b, kStr, "/dev_hdd0/d/f.txt");
        CHECK(t.call("_ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi", kStr, 0x4 /*ate alone*/, 0) == 0);
        const u32 fp = static_cast<u32>(t.call("_ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi", kStr, 0x2, 0));
        REQUIRE(fp != 0);
        // _Noreplace on an existing file fails (the SDK tests it when the mode reads too).
        CHECK(t.call("_ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi", kStr, 0x83, 0) == 0);
        const u32 fb = kObj;
        std::memset(b + fb, 0, 84);
        ps_write32(b, fb, vt);
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5_InitEPSt6_FiletNS2_7_InitflE", fb, fp, 1);
        CHECK(ps_read32(b, fb + 80) == fp);
        CHECK(b[fb + 76] == 1); // _Closef
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5imbueERKSt6locale", fb, make_locale(t, kObj2));
        CHECK(ps_read32(b, fb + 60) == 0); // the codecvt converts nothing
        put(b, kBuf, "line one\nline two\n");
        CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", fb, kBuf, 18) == 18);
        CHECK(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE4syncEv", fb) == 0);
        CHECK(slurp(root / "hdd0" / "d" / "f.txt") == "line one\nline two\n");
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5_LockEv", fb);
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE7_UnlockEv", fb);
        // seekoff to the second line (the position from ftell); the destructor closes the file.
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE", kOut, fb,
            9, 0, 2);
        CHECK(ps_read32(b, kOut) == 0);
        CHECK(ps_read32(b, kOut + 4) == 9);
        put(b, kBuf, "LINE");
        t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", fb, kBuf, 4);
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev", fb);
        CHECK(ps_read32(b, fb + 80) == 0);
        CHECK(slurp(root / "hdd0" / "d" / "f.txt") == "line one\nLINE two\n");
        // Reading: uflow and underflow, a putback through ungetc, seekpos.
        const u32 rp = static_cast<u32>(t.call("_ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi", kStr, 0x1, 0));
        REQUIRE(rp != 0);
        std::memset(b + fb, 0, 84);
        ps_write32(b, fb, vt);
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5_InitEPSt6_FiletNS2_7_InitflE", fb, rp, 1);
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE9underflowEv", fb)) == 'l');
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv", fb)) == 'l');
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv", fb)) == 'i');
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE9pbackfailEi", fb, 'I')) == 'I');
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv", fb)) == 'I');
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE", kOut,
            fb, u64{9}, 0, 0, 1);
        CHECK(ps_read32(b, kOut + 4) == 9);
        std::memset(b + kOut + 32, 0, 8);
        CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", fb, kOut + 32, 4) == 4);
        CHECK(cstr(b, kOut + 32) == "LINE");
        // setbuf(0, 0): unbuffered.
        CHECK(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE6setbufEPci", fb, 0, 0) == fb);
        CHECK(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE9_EndwriteEv", fb) == 1);
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev", fb);
        // Without a file: eof and failures.
        std::memset(b + fb, 0, 84);
        ps_write32(b, fb, vt);
        t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5_InitEPSt6_FiletNS2_7_InitflE", fb, 0, 0);
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE8overflowEi", fb, 'a')) == -1);
        CHECK(i32(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv", fb)) == -1);
        CHECK(t.call("_ZNSt13basic_filebufIcSt11char_traitsIcEE6setbufEPci", fb, 0, 0) == 0);
        // The wide _Fiopen: the name's characters as bytes.
        const std::u32string wname = U"/dev_hdd0/d/w.txt";
        for (u32 i = 0; i < wname.size(); i++) ps_write32(b, kStr + 0x40 + 4 * i, wname[i]);
        ps_write32(b, kStr + 0x40 + 4 * static_cast<u32>(wname.size()), 0);
        const u32 wf = static_cast<u32>(t.call("_ZSt7_FiopenPKwNSt5_IosbIiE9_OpenmodeEi", kStr + 0x40, 0x2, 0));
        REQUIRE(wf != 0);
        // The wide file buffer: elements as the C library's wide characters (bytes in the "C" locale).
        const u32 wvt = lib_vtable(b, kVt + 0x100,
            {"_ZNSt13basic_filebufIwSt11char_traitsIwEED1Ev", "_ZNSt13basic_filebufIwSt11char_traitsIwEED0Ev",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE5_LockEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE7_UnlockEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE8overflowEi"});
        std::memset(b + fb, 0, 88);
        ps_write32(b, fb, wvt);
        t.call("_ZNSt13basic_filebufIwSt11char_traitsIwEE5_InitEPSt6_FiletNS2_7_InitflE", fb, wf, 1);
        t.call("_ZNSt13basic_filebufIwSt11char_traitsIwEE5imbueERKSt6locale", fb, kObj2);
        for (u32 i = 0; i < 4; i++) ps_write32(b, kBuf + 4 * i, U"wide"[i]);
        CHECK(t.call("_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwi", fb, kBuf, 4) == 4);
        CHECK(ps_read32(b, fb + 84) == wf);
        t.call("_ZNSt13basic_filebufIwSt11char_traitsIwEED1Ev", fb);
        CHECK(slurp(root / "hdd0" / "d" / "w.txt") == "wide");
        t.call("_ZNSt6localeD1Ev", kObj2);
    }
    fsys::remove_all(root);
}

TEST_CASE("sys_libstdcxx: the standard streams, cout to the TTY", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    stream_facets(t, c);
    Tty tty;
    t.call("_ZNSt8ios_base4InitC1Ev", kObj);
    t.call("_ZNSt8ios_base4InitC1Ev", kObj); // counted
    const u32 cout = HleProcess::current()->link_variable(kMod, recomp::nid_of("_ZSt4cout"));
    const u32 cerr = HleProcess::current()->link_variable(kMod, recomp::nid_of("_ZSt4cerr"));
    const u32 cin = HleProcess::current()->link_variable(kMod, recomp::nid_of("_ZSt3cin"));
    REQUIRE(cout != 0);
    // ostream {vptr, basic_ios}: its vtable's type_info "So", the basic_ios' vptr the virtual thunks' group.
    const u32 vt = ps_read32(b, cout);
    CHECK(type_name(b, cout) == "So");
    CHECK(i32(ps_read32(b, vt - 12)) == 4);
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, cout + 4))) == "_ZTv0_n12_NSoD1Ev");
    CHECK(i32(ps_read32(b, ps_read32(b, cout + 4) - 12)) == -4);
    CHECK(b[cout + 4 + 48] == ' '); // the fill, widened by ctype
    CHECK(ps_read32(b, cerr + 4 + 44) == cout); // tied to cout
    CHECK((ps_read32(b, cerr + 4 + 16) & 2) == 2); // unitbuf
    CHECK(ps_read32(b, cin + 8 + 44) == cout);
    CHECK(ps_read32(b, cout + 4 + 4) != 0); // a standard stream's slot
    // Written through cout's filebuf (xsputn, then overflow, then sys_libc's stdout).
    const u32 sb = ps_read32(b, cout + 4 + 40);
    CHECK(type_name(b, sb) == "St13basic_filebufIcSt11char_traitsIcEE");
    put(b, kBuf, "hello from cout\n");
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", sb, kBuf, 16) == 16);
    t.call("_ZNSt8ios_base4InitD1Ev", kObj);
    t.call("_ZNSt8ios_base4InitD1Ev", kObj); // the last: flushes
    CHECK(tty.get().find("hello from cout") != std::string::npos);
    // The wide ones.
    t.call("_ZNSt6_WinitC1Ev", kObj);
    const u32 wcout = HleProcess::current()->link_variable(kMod, recomp::nid_of("_ZSt5wcout"));
    CHECK(ps_read32(b, wcout + 4 + 48) == ' ');
    const u32 wsb = ps_read32(b, wcout + 4 + 40);
    for (u32 i = 0; i < 3; i++) ps_write32(b, kBuf + 4 * i, U"wo\n"[i]);
    CHECK(t.call("_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwi", wsb, kBuf, 3) == 3);
    t.call("_ZNSt6_WinitD1Ev", kObj);
    CHECK(tty.get().find("wo") != std::string::npos);
}

TEST_CASE("sys_libstdcxx: ostrstream and strstream over an array, destroyed through their thunks", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    stream_facets(t, c);
    // ostrstream(buf, 16, out | app): writing after the array's string.
    put(b, kBuf, "abc");
    std::memset(b + kBuf + 4, '.', 11);
    b[kBuf + 15] = 0;
    t.call("_ZNSt10ostrstreamC1EPciNSt5_IosbIiE9_OpenmodeE", kObj, kBuf, 16, 0x2 | 0x8);
    const u32 vt = ps_read32(b, kObj);
    CHECK(type_name(b, kObj) == "St10ostrstream");
    CHECK(i32(ps_read32(b, vt - 12)) == 88);
    CHECK(ps_read32(b, kObj + 88 + 40) == kObj + 4); // rdbuf: the member strstreambuf
    CHECK(type_name(b, kObj + 4) == "St12strstreambuf");
    put(b, kStr, "XY");
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", kObj + 4, kStr, 2) == 2);
    CHECK(std::string(reinterpret_cast<const char*>(b + kBuf), 6) == "abcXY.");
    // Destroyed through the basic_ios subobject's virtual thunk (the complete-object one).
    const u32 ios_vt = ps_read32(b, kObj + 88);
    CHECK(fn_name(b, ps_read32(b, ios_vt)) == "_ZTv0_n12_NSt10ostrstreamD1Ev");
    call_guest(t.ctx, ps_read32(b, ios_vt), {kObj + 88});
    CHECK(type_name(b, kObj + 88) == "St8ios_base");
    // strstream(buf, 8, in | out): an iostream; the ostream-in-strstream's non-virtual thunk at +8.
    std::memset(b + kBuf, 0, 16);
    t.call("_ZNSt9strstreamC1EPciNSt5_IosbIiE9_OpenmodeE", kObj, kBuf, 8, 0x3);
    CHECK(i32(ps_read32(b, ps_read32(b, kObj) - 12)) == 96);
    CHECK(i32(ps_read32(b, ps_read32(b, kObj + 8) - 12)) == 88);
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, kObj + 8))) == "_ZThn8_NSt9strstreamD1Ev");
    put(b, kStr, "ok");
    t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", kObj + 12, kStr, 2);
    CHECK(cstr(b, kBuf) == "ok");
    std::memset(b + kOut, 0, 4);
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", kObj + 12, kOut, 2) == 2);
    CHECK(cstr(b, kOut) == "ok");
    call_guest(t.ctx, ps_read32(b, ps_read32(b, kObj + 8)), {kObj + 8});
    CHECK(type_name(b, kObj + 96) == "St8ios_base");
    // A heap ostrstream with a dynamic buffer, deleted (D0).
    const u32 heap = static_cast<u32>(t.call("_Znwj", 140));
    t.call("_ZNSt10ostrstreamC1EPciNSt5_IosbIiE9_OpenmodeE", heap, 0, 0, 0x2);
    put(b, kStr, "dynamic text that is longer than the first block");
    CHECK(t.call("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", heap + 4, kStr, 48) == 48);
    t.call("_ZNSt10ostrstreamD0Ev", heap);
    // The manipulators: _Smanip {function, argument}, the function applied to an ios_base.
    std::memset(b + kObj2, 0, 52);
    t.call("_ZNSt8ios_base5_InitEv", kObj2);
    t.call("_ZSt4setwi", kOut, 12);
    call_guest(t.ctx, ps_read32(b, kOut), {kObj2, ps_read32(b, kOut + 4)});
    CHECK(ps_read32(b, kObj2 + 24) == 12);
    t.call("_ZSt12setprecisioni", kOut, 3);
    call_guest(t.ctx, ps_read32(b, kOut), {kObj2, ps_read32(b, kOut + 4)});
    CHECK(ps_read32(b, kObj2 + 20) == 3);
    t.call("_ZSt7setbasei", kOut, 16);
    call_guest(t.ctx, ps_read32(b, kOut), {kObj2, ps_read32(b, kOut + 4)});
    CHECK(ps_read32(b, kObj2 + 16) == (0x1u | 0x800));
    t.call("_ZSt11setiosflagsNSt5_IosbIiE9_FmtflagsE", kOut, 0x4040);
    call_guest(t.ctx, ps_read32(b, kOut), {kObj2, ps_read32(b, kOut + 4)});
    CHECK(ps_read32(b, kObj2 + 16) == (0x1u | 0x800 | 0x4040));
    t.call("_ZSt13resetiosflagsNSt5_IosbIiE9_FmtflagsE", kOut, 0x0801);
    call_guest(t.ctx, ps_read32(b, kOut), {kObj2, ps_read32(b, kOut + 4)});
    CHECK(ps_read32(b, kObj2 + 16) == 0x4040u);
    t.call("_ZNSt8ios_baseD1Ev", kObj2);
}

TEST_CASE("sys_libstdcxx: basic_ios::init and the istream, ostream and iostream destructors", "[hle][libstdcxx]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    stream_facets(t, c);
    // An istream laid out by hand: {vptr, _Chcount, basic_ios}; init with no buffer sets badbit.
    std::memset(b + kObj, 0, 60);
    t.call("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_Eb", kObj + 8, 0, 0);
    CHECK(ps_read32(b, kObj + 8 + 8) == 4);
    CHECK(b[kObj + 8 + 48] == ' ');
    const u32 rec = recorder(c, kRec);
    t.call("_ZNSt8ios_base17register_callbackEPFvNS_5eventERS_iEi", kObj + 8, rec, 0);
    ps_write32(b, kRec + 12, 0);
    t.call("_ZNSiD1Ev", kObj);
    CHECK(ps_read32(b, kRec + 12) == 1); // the virtual base destroyed: erase_event
    CHECK(type_name(b, kObj) == "Si");
    // The wide istream.
    std::memset(b + kObj2, 0, 60);
    t.call("_ZNSt9basic_iosIwSt11char_traitsIwEE4initEPSt15basic_streambufIwS1_Eb", kObj2 + 8, kObj3, 0);
    CHECK(ps_read32(b, kObj2 + 8 + 48) == ' ');
    CHECK(ps_read32(b, kObj2 + 8 + 8) == 0);
    t.call("_ZNSt13basic_istreamIwSt11char_traitsIwEED1Ev", kObj2);
    CHECK(type_name(b, kObj2) == "St13basic_istreamIwSt11char_traitsIwEE");
    // iostream: D1 sets the three vptrs; then deleted through the ostream part's non-virtual thunk.
    const u32 heap = static_cast<u32>(t.call("_Znwj", 64));
    std::memset(b + heap, 0, 64);
    t.call("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_Eb", heap + 12, kObj3, 0);
    t.call("_ZNSdD1Ev", heap);
    CHECK(type_name(b, heap) == "Sd");
    CHECK(fn_name(b, ps_read32(b, ps_read32(b, heap + 8))) == "_ZThn8_NSdD1Ev");
    CHECK(type_name(b, heap + 12) == "St8ios_base"); // the virtual base destroyed last
    t.call("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_Eb", heap + 12, kObj3, 0);
    t.call("_ZThn8_NSdD0Ev", heap + 8);
    // basic_ios's own destructors.
    std::memset(b + kObj, 0, 52);
    t.call("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_Eb", kObj, kObj3, 0);
    t.call("_ZNSt9basic_iosIcSt11char_traitsIcEED1Ev", kObj);
    CHECK(type_name(b, kObj) == "St8ios_base");
}

TEST_CASE("sys_libstdcxx: named locales and copies of the classic one get the facets part's facets",
    "[hle][libstdcxx]")
{
    if (!registry().find(kMod, recomp::nid_of("_ZNSt5ctypeIcE7_GetcatEPPKNSt6locale5facetE")))
        SKIP("the facets part (D-561) is not in this build");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    put(b, kBuf, "C");
    t.call("_ZNSt6localeC1EPKci", kObj, kBuf, 63);
    const u32 imp = ps_read32(b, kObj);
    CHECK(GString{b, imp + 24}.text() == "C");
    CHECK(ps_read32(b, imp + 16) == 63);
    CHECK(ps_read32(b, imp + 12) >= 40);
    CHECK(t.call("_ZSt9use_facetISt5ctypeIwEERKT_RKSt6locale", kObj) != 0);
    const u32 classic = static_cast<u32>(t.call("_ZNSt6locale7classicEv"));
    CHECK(t.call("_ZNKSt6localeeqERKS_", kObj, classic) == 1);
    t.call("_ZNSt6localeC1ERKS_S1_i", kObj2, classic, kObj, 2);
    CHECK(ps_read32(b, ps_read32(b, kObj2) + 12) >= 40);
    t.call("_ZNSt6localeD1Ev", kObj2);
    t.call("_ZNSt6localeC1ERKS_PKci", kObj2, kObj, kBuf, 8);
    t.call("_ZNSt6localeD1Ev", kObj2);
    t.call("_ZNSt6localeD1Ev", kObj);
}
