// sys_libc: the C library of the PS3 SDK (Dinkumware's, with Sony's additions), as a game would import it. RPCS3 names
// it (PPUFunction.cpp) under sys_libc and, nearly the same list, sys_libm, and implements none of it; no firmware
// module has it, so it is written to the C99 standard and Dinkumware's documented choices, and registered under both
// names (D-511). The math half is sys_libm.cpp (D-510); this file holds the strings, character classes, the stdlib
// numbers and searches, the random numbers; the other parts are sys_libc_*.cpp.
//
// Throughout: a guest `char` is unsigned, `int`, `long`, `size_t` and pointers are 32 bits, `long long` 64; a structure
// result (div_t, ldiv_t, lldiv_t) goes through a pointer the caller passes in r3 (the 64-bit PowerPC ABI returns
// aggregates in memory). Guest pointers are checked before use (D-042): a string or range that is not committed guest
// memory ends the call with the call site logged, as Sony's code would fault there.

#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <format>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

// ------------------------------------------------------------------------------------------------- guest access

// The guest string at `s` (NUL excluded), or nullopt after logging the call site.
std::optional<std::string> str_arg(PPUContext& ctx, const char* fn, u32 s)
{
    std::optional<std::string> r = read_string(thread_of(ctx), s);
    if (!r) refuse(ctx, std::format("{}: the string at {:#x} runs out of guest memory", fn, s));
    return r;
}


u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// Dinkumware's comparisons give -1 or +1 (string.h's memcmp.c, strcmp.c): the first differing bytes as unsigned char.
s32 order(u8 a, u8 b) { return a < b ? -1 : a > b ? 1 : 0; }

// ------------------------------------------------------------------------------------------------- string.h

u32 strlen_(PPUContext& ctx, u32 s)
{
    const auto t = str_arg(ctx, "strlen", s);
    return t ? static_cast<u32>(t->size()) : 0;
}

u32 strcpy_(PPUContext& ctx, u32 d, u32 s)
{
    const auto t = str_arg(ctx, "strcpy", s);
    if (t && range_ok(ctx, "strcpy", d, t->size() + 1)) std::memmove(gb(ctx) + d, t->c_str(), t->size() + 1);
    return d;
}

u32 strncpy_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    if (!n) return d;
    const auto t = read_string(thread_of(ctx), s, n); // at most n bytes looked at
    if (!t) {
        refuse(ctx, std::format("strncpy: the string at {:#x} runs out of guest memory", s));
        return d;
    }
    if (!range_ok(ctx, "strncpy", d, n)) return d;
    std::vector<u8> out(n, 0);
    std::memcpy(out.data(), t->data(), std::min<usize>(t->size(), n));
    std::memcpy(gb(ctx) + d, out.data(), n);
    return d;
}

u32 strcat_(PPUContext& ctx, u32 d, u32 s)
{
    const auto a = str_arg(ctx, "strcat", d);
    const auto b = str_arg(ctx, "strcat", s);
    if (a && b && range_ok(ctx, "strcat", d, a->size() + b->size() + 1))
        std::memmove(gb(ctx) + d + a->size(), b->c_str(), b->size() + 1);
    return d;
}

u32 strncat_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    const auto a = str_arg(ctx, "strncat", d);
    if (!a) return d;
    const auto b = read_string(thread_of(ctx), s, n);
    if (!b) {
        refuse(ctx, std::format("strncat: the string at {:#x} runs out of guest memory", s));
        return d;
    }
    if (range_ok(ctx, "strncat", d, a->size() + b->size() + 1)) {
        std::memmove(gb(ctx) + d + a->size(), b->data(), b->size());
        gb(ctx)[d + a->size() + b->size()] = 0;
    }
    return d;
}

s32 strcmp_(PPUContext& ctx, u32 a, u32 b)
{
    GuestBytes x(thread_of(ctx), a), y(thread_of(ctx), b);
    for (u64 i = 0;; i++) {
        const s32 c = x.at(i), e = y.at(i);
        if (c < 0 || e < 0) {
            refuse(ctx, std::format("strcmp: a string ({:#x} or {:#x}) runs out of guest memory", a, b));
            return 0;
        }
        if (c != e) return order(static_cast<u8>(c), static_cast<u8>(e));
        if (!c) return 0;
    }
}

s32 strncmp_(PPUContext& ctx, u32 a, u32 b, u32 n)
{
    GuestBytes x(thread_of(ctx), a), y(thread_of(ctx), b);
    for (u32 i = 0; i < n; i++) {
        const s32 c = x.at(i), e = y.at(i);
        if (c < 0 || e < 0) {
            refuse(ctx, std::format("strncmp: a string ({:#x} or {:#x}) runs out of guest memory", a, b));
            return 0;
        }
        if (c != e) return order(static_cast<u8>(c), static_cast<u8>(e));
        if (!c) return 0;
    }
    return 0;
}

// strcasecmp and strncasecmp (POSIX), the C locale's folding: the difference of the first differing lower-cased bytes.
s32 casecmp(PPUContext& ctx, const char* fn, u32 a, u32 b, u64 n)
{
    GuestBytes x(thread_of(ctx), a), y(thread_of(ctx), b);
    for (u64 i = 0; i < n; i++) {
        const s32 c = x.at(i), e = y.at(i);
        if (c < 0 || e < 0) {
            refuse(ctx, std::format("{}: a string ({:#x} or {:#x}) runs out of guest memory", fn, a, b));
            return 0;
        }
        const s32 lc = static_cast<s32>(c_tolower(c)), le = static_cast<s32>(c_tolower(e));
        if (lc != le) return lc - le;
        if (!c) return 0;
    }
    return 0;
}

s32 strcasecmp_(PPUContext& ctx, u32 a, u32 b) { return casecmp(ctx, "strcasecmp", a, b, ~u64{0}); }
s32 strncasecmp_(PPUContext& ctx, u32 a, u32 b, u32 n) { return casecmp(ctx, "strncasecmp", a, b, n); }

// strcoll and strxfrm in the "C" locale: the bytes' order.
s32 strcoll_(PPUContext& ctx, u32 a, u32 b) { return strcmp_(ctx, a, b); }

u32 strxfrm_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    const auto t = str_arg(ctx, "strxfrm", s);
    if (!t) return 0;
    if (t->size() < n && range_ok(ctx, "strxfrm", d, t->size() + 1)) std::memcpy(gb(ctx) + d, t->c_str(), t->size() + 1);
    return static_cast<u32>(t->size());
}

u32 strchr_(PPUContext& ctx, u32 s, s32 c)
{
    const auto t = str_arg(ctx, "strchr", s);
    if (!t) return 0;
    const u8 ch = static_cast<u8>(c);
    if (!ch) return s + static_cast<u32>(t->size());
    const usize i = t->find(static_cast<char>(ch));
    return i == std::string::npos ? 0 : s + static_cast<u32>(i);
}

u32 strrchr_(PPUContext& ctx, u32 s, s32 c)
{
    const auto t = str_arg(ctx, "strrchr", s);
    if (!t) return 0;
    const u8 ch = static_cast<u8>(c);
    if (!ch) return s + static_cast<u32>(t->size());
    const usize i = t->rfind(static_cast<char>(ch));
    return i == std::string::npos ? 0 : s + static_cast<u32>(i);
}

u32 memchr_(PPUContext& ctx, u32 s, s32 c, u32 n)
{
    if (!range_ok(ctx, "memchr", s, n)) return 0;
    const u8* p = gb(ctx) + s;
    const void* r = std::memchr(p, static_cast<u8>(c), n);
    return r ? s + static_cast<u32>(static_cast<const u8*>(r) - p) : 0;
}

u32 strspn_(PPUContext& ctx, u32 s, u32 set)
{
    const auto a = str_arg(ctx, "strspn", s), b = str_arg(ctx, "strspn", set);
    return a && b ? static_cast<u32>(std::min(a->find_first_not_of(*b), a->size())) : 0;
}

u32 strcspn_(PPUContext& ctx, u32 s, u32 set)
{
    const auto a = str_arg(ctx, "strcspn", s), b = str_arg(ctx, "strcspn", set);
    return a && b ? static_cast<u32>(std::min(a->find_first_of(*b), a->size())) : 0;
}

u32 strpbrk_(PPUContext& ctx, u32 s, u32 set)
{
    const auto a = str_arg(ctx, "strpbrk", s), b = str_arg(ctx, "strpbrk", set);
    if (!a || !b) return 0;
    const usize i = a->find_first_of(*b);
    return i == std::string::npos ? 0 : s + static_cast<u32>(i);
}

u32 strstr_(PPUContext& ctx, u32 s, u32 sub)
{
    const auto a = str_arg(ctx, "strstr", s), b = str_arg(ctx, "strstr", sub);
    if (!a || !b) return 0;
    const usize i = a->find(*b);
    return i == std::string::npos ? 0 : s + static_cast<u32>(i);
}

// strtok_r's work: the next token of `s` (or of *save when s is NULL), `save` updated; 0 when none.
u32 tokenize(PPUContext& ctx, const char* fn, u32 s, u32 delim, u32& save)
{
    if (!s) s = save;
    if (!s) return 0;
    const auto t = str_arg(ctx, fn, s);
    const auto d = str_arg(ctx, fn, delim);
    if (!t || !d) return 0;
    const usize start = t->find_first_not_of(*d);
    if (start == std::string::npos) {
        save = 0;
        return 0;
    }
    const usize end = t->find_first_of(*d, start);
    if (end == std::string::npos) {
        save = 0;
    } else {
        gb(ctx)[s + end] = 0;
        save = s + static_cast<u32>(end) + 1;
    }
    return s + static_cast<u32>(start);
}

// strtok keeps its position in the thread's block (Dinkumware keeps it per thread).
u32 strtok_(PPUContext& ctx, u32 s, u32 delim)
{
    const u32 b = thread_block(ctx);
    u32 save = b ? ps_read32(gb(ctx), b + tb::kStrtok) : 0;
    const u32 r = tokenize(ctx, "strtok", s, delim, save);
    if (b) ps_write32(gb(ctx), b + tb::kStrtok, save);
    return r;
}

u32 strtok_r_(PPUContext& ctx, u32 s, u32 delim, u32 savep)
{
    if (!range_ok(ctx, "strtok_r", savep, 4)) return 0;
    u32 save = ps_read32(gb(ctx), savep);
    const u32 r = tokenize(ctx, "strtok_r", s, delim, save);
    ps_write32(gb(ctx), savep, save);
    return r;
}

// The messages of strerror (Dinkumware's _Strerror, for the C library's errno codes, which are lv2's).
std::string_view error_text(u32 code)
{
    switch (code) {
    case 0: return "no error";
    case kEDOM: return "domain error";
    case kERANGE: return "range error";
    case kEILSEQ: return "invalid multibyte sequence";
    case kEFPOS: return "invalid file position";
    case kEINVAL: return "invalid argument";
    case kENOMEM: return "not enough memory";
    case kEBADF: return "bad file descriptor";
    case kEFAULT: return "bad address";
    case kENOSYS: return "function not implemented";
    case kEOVERFLOW: return "value too large";
    case kESPIPE: return "invalid seek";
    case kEMFILE: return "too many open files";
    case kENOTSUP: return "not supported";
    case kEIO: return "input/output error";
    case kEEXIST: return "file exists";
    default: return "unknown error";
    }
}

// strerror(code): the message in the thread's block (80 bytes).
u32 strerror_(PPUContext& ctx, u32 code)
{
    const u32 b = thread_block(ctx);
    if (!b) return 0;
    const std::string_view text = error_text(code);
    std::memcpy(gb(ctx) + b + tb::kStrerror, text.data(), text.size());
    gb(ctx)[b + tb::kStrerror + text.size()] = 0;
    return b + tb::kStrerror;
}

// memcpy16 and memset_vmx (Sony's: for 16-byte aligned memory, the same results as memcpy and memset); _memset_int
// fills with the int's low byte as memset does (its other behaviour is not known; logged once if the value is wider).
u32 memcpy16_(PPUContext& ctx, u32 d, u32 s, u32 n)
{
    if (range_ok(ctx, "memcpy16", d, n) && range_ok(ctx, "memcpy16", s, n)) std::memmove(gb(ctx) + d, gb(ctx) + s, n);
    return d;
}

u32 memset_vmx_(PPUContext& ctx, u32 d, s32 c, u32 n)
{
    if (range_ok(ctx, "memset_vmx", d, n)) std::memset(gb(ctx) + d, static_cast<u8>(c), n);
    return d;
}

u32 memset_int_(PPUContext& ctx, u32 d, s32 c, u32 n)
{
    if (static_cast<u32>(c) > 0xFF) {
        static std::once_flag once;
        std::call_once(once, [&] { PS_LOG_WARN(kChan, "_memset_int({:#x}, {:#x}, {:#x}): filled with the low byte", d, c, n); });
    }
    if (range_ok(ctx, "_memset_int", d, n)) std::memset(gb(ctx) + d, static_cast<u8>(c), n);
    return d;
}

// ------------------------------------------------------------------------------------------------- ctype.h

// The "C" locale's classes (Dinkumware's default tables): ASCII only; EOF and bytes past 127 have none. The _ascii
// forms are Sony's, the same in any locale.
bool in_byte(s32 c) { return c >= 0 && c <= 0x7F; }
#define PS_CTYPE(name, test)                                                                                         \
    s32 name##_(PPUContext&, s32 c) { return in_byte(c) && (test) ? 1 : 0; }                                         \
    s32 name##_ascii_(PPUContext&, s32 c) { return in_byte(c) && (test) ? 1 : 0; }
PS_CTYPE(isalnum, c_isalpha(c) || c_isdigit(c))
PS_CTYPE(isalpha, c_isalpha(c))
PS_CTYPE(isblank, c == ' ' || c == '\t')
PS_CTYPE(iscntrl, c < 0x20 || c == 0x7F)
PS_CTYPE(isdigit, c_isdigit(c))
PS_CTYPE(isgraph, c > 0x20 && c < 0x7F)
PS_CTYPE(islower, c >= 'a' && c <= 'z')
PS_CTYPE(isprint, c >= 0x20 && c < 0x7F)
PS_CTYPE(ispunct, c > 0x20 && c < 0x7F && !c_isalpha(c) && !c_isdigit(c))
PS_CTYPE(isspace, c_isspace(c))
PS_CTYPE(isupper, c >= 'A' && c <= 'Z')
PS_CTYPE(isxdigit, c_isxdigit(c))
#undef PS_CTYPE

s32 tolower_(PPUContext&, s32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
s32 toupper_(PPUContext&, s32 c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// ------------------------------------------------------------------------------------------------- stdlib.h numbers

s32 abs_(PPUContext&, s32 v) { return v < 0 ? static_cast<s32>(0u - static_cast<u32>(v)) : v; }
s32 labs_(PPUContext& ctx, s32 v) { return abs_(ctx, v); }
u64 llabs_(PPUContext&, s64 v) { return v < 0 ? 0ull - static_cast<u64>(v) : static_cast<u64>(v); }
u64 imaxabs_(PPUContext& ctx, s64 v) { return llabs_(ctx, v); }

// div and ldiv (32-bit), lldiv and imaxdiv (64-bit): {quot, rem} through the caller's buffer in r3, truncating.
void div_(PPUContext& ctx, u32 out, s32 n, s32 d)
{
    if (!d || !range_ok(ctx, "div", out, 8)) return;
    const s32 q = (n == INT_MIN && d == -1) ? INT_MIN : n / d, r = (n == INT_MIN && d == -1) ? 0 : n % d;
    ps_write32(gb(ctx), out, static_cast<u32>(q));
    ps_write32(gb(ctx), out + 4, static_cast<u32>(r));
}

void lldiv_(PPUContext& ctx, u32 out, s64 n, s64 d)
{
    if (!d || !range_ok(ctx, "lldiv", out, 16)) return;
    const bool ov = n == std::numeric_limits<s64>::min() && d == -1;
    ps_write64(gb(ctx), out, static_cast<u64>(ov ? n : n / d));
    ps_write64(gb(ctx), out + 8, static_cast<u64>(ov ? 0 : n % d));
}

// strtol's family: C99 7.20.1.4 on the guest string, *end set (when given) past the subject sequence, ERANGE
// clamping; long is 32 bits.
u64 to_int(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 base, u32 bits, bool is_signed)
{
    GuestBytes in(thread_of(ctx), s);
    const auto at = [&](u64 i) -> s64 { return in.at(i); };
    if (base < 0 || base == 1 || base > 36) {
        set_errno(ctx, kEINVAL);
        if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(gb(ctx), endp, s);
        return 0;
    }
    const IntParse p = parse_int(at, base);
    bool range = false;
    const u64 v = int_result(p, bits, is_signed, &range);
    if (range) set_errno(ctx, kERANGE);
    if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(gb(ctx), endp, p.end ? s + static_cast<u32>(p.end) : s);
    return v;
}

s32 strtol_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<s32>(to_int(ctx, "strtol", s, e, b, 32, true)); }
u32 strtoul_(PPUContext& ctx, u32 s, u32 e, s32 b) { return static_cast<u32>(to_int(ctx, "strtoul", s, e, b, 32, false)); }
u64 strtoll_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "strtoll", s, e, b, 64, true); }
u64 strtoull_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "strtoull", s, e, b, 64, false); }
u64 strtoimax_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "strtoimax", s, e, b, 64, true); }
u64 strtoumax_(PPUContext& ctx, u32 s, u32 e, s32 b) { return to_int(ctx, "strtoumax", s, e, b, 64, false); }
s32 atoi_(PPUContext& ctx, u32 s) { return static_cast<s32>(to_int(ctx, "atoi", s, 0, 10, 32, true)); }
s32 atol_(PPUContext& ctx, u32 s) { return atoi_(ctx, s); }
u64 atoll_(PPUContext& ctx, u32 s) { return to_int(ctx, "atoll", s, 0, 10, 64, true); }

// strtod's family (7.20.1.3), the host's correctly rounded parse in the "C" locale.
FloatParse to_float(PPUContext& ctx, const char* fn, u32 s, u32 endp)
{
    const auto text = read_string(thread_of(ctx), s, 0x10000);
    FloatParse p{};
    if (!text) {
        refuse(ctx, std::format("{}: the string at {:#x} runs out of guest memory", fn, s));
    } else {
        p = parse_float(*text);
    }
    if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(gb(ctx), endp, s + static_cast<u32>(p.end));
    return p;
}

// ERANGE for the result's own type (double, or float for strtof and atoff).
double float_d(PPUContext& ctx, const char* fn, u32 s, u32 e)
{
    const FloatParse p = to_float(ctx, fn, s, e);
    if (p.range_d) set_errno(ctx, kERANGE);
    return p.d;
}

float float_f(PPUContext& ctx, const char* fn, u32 s, u32 e)
{
    const FloatParse p = to_float(ctx, fn, s, e);
    if (p.range_f) set_errno(ctx, kERANGE);
    return p.f;
}

double strtod_(PPUContext& ctx, u32 s, u32 e) { return float_d(ctx, "strtod", s, e); }
double strtold_(PPUContext& ctx, u32 s, u32 e) { return float_d(ctx, "strtold", s, e); }
float strtof_(PPUContext& ctx, u32 s, u32 e) { return float_f(ctx, "strtof", s, e); }
double atof_(PPUContext& ctx, u32 s) { return float_d(ctx, "atof", s, 0); }
float atoff_(PPUContext& ctx, u32 s) { return float_f(ctx, "atoff", s, 0); }

// ------------------------------------------------------------------------------------------------- searching, sorting

// qsort (7.20.5.2): a merge sort of the elements' order, compared through the guest function `cmp` (its function
// descriptor) on the elements where they lie, then the elements moved whole. The standard leaves the order of equal
// elements open; a stable sort is one of its orders.
void qsort_(PPUContext& ctx, u32 base, u32 n, u32 size, u32 cmp)
{
    if (n < 2 || !size) return;
    if (!range_ok(ctx, "qsort", base, u64{n} * size)) return;
    std::vector<u32> idx(n);
    for (u32 i = 0; i < n; i++) idx[i] = i;
    const auto element = [&](u32 i) { return u64{base + i * size}; };
    std::stable_sort(idx.begin(), idx.end(),
        [&](u32 a, u32 b) { return static_cast<s32>(call_guest(ctx, cmp, {element(a), element(b)})) < 0; });
    std::vector<u8> copy(gb(ctx) + base, gb(ctx) + base + u64{n} * size);
    for (u32 i = 0; i < n; i++) std::memcpy(gb(ctx) + base + i * size, copy.data() + u64{idx[i]} * size, size);
}

// bsearch (7.20.5.1): a binary search, `cmp(key, element)`.
u32 bsearch_(PPUContext& ctx, u32 key, u32 base, u32 n, u32 size, u32 cmp)
{
    u32 lo = 0, hi = n;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2, at = base + mid * size;
        const s32 r = static_cast<s32>(call_guest(ctx, cmp, {key, at}));
        if (r == 0) return at;
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------- rand, TT800

// rand and srand as Dinkumware's rand.c: a linear congruential seed, RAND_MAX 0x7FFF, one seed for the process.
struct RandState {
    std::atomic<u32> seed{1};
};

s32 rand_(PPUContext&)
{
    RandState& s = HleProcess::current()->state<RandState>();
    u32 old = s.seed.load(), next = 0;
    do {
        next = old * 1103515245u + 12345u;
    } while (!s.seed.compare_exchange_weak(old, next));
    return static_cast<s32>((next >> 16) & 0x7FFF);
}

void srand_(PPUContext&, u32 seed) { HleProcess::current()->state<RandState>().seed = seed; }

// TT800 (M. Matsumoto and Y. Kurita, 1994; the tempering of its 1996 version), with mt19937ar's interface: its 25
// words and position (104 bytes) in guest memory, the _ forms on a state the caller gives, the others on the
// variable __TT800. init_TT800 seeds as mt19937ar's init_genrand, init_by_array_TT800 as its init_by_array.
constexpr u32 kN = 25, kM = 7, kTTSize = 4 * kN + 4;

void tt_init(u8* b, u32 st, u32 seed)
{
    u32 x = seed;
    ps_write32(b, st, x);
    for (u32 i = 1; i < kN; i++) {
        x = 1812433253u * (x ^ (x >> 30)) + i;
        ps_write32(b, st + 4 * i, x);
    }
    ps_write32(b, st + 4 * kN, kN); // regenerate at the next draw
}

void tt_init_by_array(u8* b, u32 st, const std::vector<u32>& key)
{
    tt_init(b, st, 19650218u);
    const auto x = [&](u32 i) { return ps_read32(b, st + 4 * i); };
    const auto set = [&](u32 i, u32 v) { ps_write32(b, st + 4 * i, v); };
    u32 i = 1, j = 0;
    for (u32 k = std::max<u32>(kN, static_cast<u32>(key.size())); k; k--) {
        set(i, (x(i) ^ ((x(i - 1) ^ (x(i - 1) >> 30)) * 1664525u)) + key[j] + j);
        i++;
        j++;
        if (i >= kN) {
            set(0, x(kN - 1));
            i = 1;
        }
        if (j >= key.size()) j = 0;
    }
    for (u32 k = kN - 1; k; k--) {
        set(i, (x(i) ^ ((x(i - 1) ^ (x(i - 1) >> 30)) * 1566083941u)) - i);
        i++;
        if (i >= kN) {
            set(0, x(kN - 1));
            i = 1;
        }
    }
    set(0, 0x80000000u);
}

u32 tt_next(u8* b, u32 st)
{
    static constexpr u32 kMag[2] = {0, 0x8EBFD028u};
    u32 k = ps_read32(b, st + 4 * kN);
    if (k >= kN) {
        const auto x = [&](u32 i) { return ps_read32(b, st + 4 * i); };
        for (u32 kk = 0; kk < kN; kk++) {
            const u32 v = x((kk + kM) % kN) ^ (x(kk) >> 1) ^ kMag[x(kk) & 1];
            ps_write32(b, st + 4 * kk, v);
        }
        k = 0;
    }
    u32 y = ps_read32(b, st + 4 * k);
    y ^= (y << 7) & 0x2B5B2500u;
    y ^= (y << 15) & 0xDB8B0000u;
    y ^= y >> 16;
    ps_write32(b, st + 4 * kN, k + 1);
    return y;
}

u32 tt_global() { return variable(recomp::nid_of("__TT800")); } // the generator's state of the process

bool tt_ok(PPUContext& ctx, const char* fn, u32 st) { return st && range_ok(ctx, fn, st, kTTSize); }

void init_TT800_(PPUContext& ctx, u32 seed) { tt_init(gb(ctx), tt_global(), seed); }
void _init_TT800_(PPUContext& ctx, u32 st, u32 seed)
{
    if (tt_ok(ctx, "_init_TT800", st)) tt_init(gb(ctx), st, seed);
}

void init_by_array_impl(PPUContext& ctx, const char* fn, u32 st, u32 key, u32 len)
{
    if (!tt_ok(ctx, fn, st) || !range_ok(ctx, fn, key, u64{len} * 4) || !len) return;
    std::vector<u32> k(len);
    for (u32 i = 0; i < len; i++) k[i] = ps_read32(gb(ctx), key + 4 * i);
    tt_init_by_array(gb(ctx), st, k);
}

void init_by_array_TT800_(PPUContext& ctx, u32 key, u32 len) { init_by_array_impl(ctx, "init_by_array_TT800", tt_global(), key, len); }
void _init_by_array_TT800_(PPUContext& ctx, u32 st, u32 key, u32 len) { init_by_array_impl(ctx, "_init_by_array_TT800", st, key, len); }

u32 draw(PPUContext& ctx, const char* fn, u32 st) { return tt_ok(ctx, fn, st) ? tt_next(gb(ctx), st) : 0; }

u32 rand_int32_TT800_(PPUContext& ctx) { return draw(ctx, "rand_int32_TT800", tt_global()); }
s32 rand_int31_TT800_(PPUContext& ctx) { return static_cast<s32>(draw(ctx, "rand_int31_TT800", tt_global()) >> 1); }
double rand_real1_TT800_(PPUContext& ctx) { return draw(ctx, "rand_real1_TT800", tt_global()) * (1.0 / 4294967295.0); }
double rand_real2_TT800_(PPUContext& ctx) { return draw(ctx, "rand_real2_TT800", tt_global()) * (1.0 / 4294967296.0); }
double rand_real3_TT800_(PPUContext& ctx) { return (draw(ctx, "rand_real3_TT800", tt_global()) + 0.5) * (1.0 / 4294967296.0); }
u32 _rand_int32_TT800_(PPUContext& ctx, u32 st) { return draw(ctx, "_rand_int32_TT800", st); }
s32 _rand_int31_TT800_(PPUContext& ctx, u32 st) { return static_cast<s32>(draw(ctx, "_rand_int31_TT800", st) >> 1); }
double _rand_real1_TT800_(PPUContext& ctx, u32 st) { return draw(ctx, "_rand_real1_TT800", st) * (1.0 / 4294967295.0); }
double _rand_real2_TT800_(PPUContext& ctx, u32 st) { return draw(ctx, "_rand_real2_TT800", st) * (1.0 / 4294967296.0); }
double _rand_real3_TT800_(PPUContext& ctx, u32 st) { return (draw(ctx, "_rand_real3_TT800", st) + 0.5) * (1.0 / 4294967296.0); }

// get_state_TT800(dst) and _get_state_TT800(dst, st): the 104 bytes of the generator's state copied out.
void get_state_impl(PPUContext& ctx, const char* fn, u32 dst, u32 st)
{
    if (tt_ok(ctx, fn, st) && range_ok(ctx, fn, dst, kTTSize)) std::memmove(gb(ctx) + dst, gb(ctx) + st, kTTSize);
}
void get_state_TT800_(PPUContext& ctx, u32 dst) { get_state_impl(ctx, "get_state_TT800", dst, tt_global()); }
void _get_state_TT800_(PPUContext& ctx, u32 dst, u32 st) { get_state_impl(ctx, "_get_state_TT800", dst, st); }

// The __TT800 variable starts seeded with mt19937ar's default seed (5489), as its genrand_int32 does when unseeded.
void tt800_init(u8* b, u32 addr) { tt_init(b, addr, 5489u); }

} // namespace

// A libc function registered by its name (its NID computed from it, as the SDK's linker does): `name##_` here.
#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

// The libc functions are registered under sys_libc and sys_libm, as RPCS3 lists them under both.
void register_sys_libc(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(strlen), LIBC(strcpy), LIBC(strncpy), LIBC(strcat), LIBC(strncat), LIBC(strcmp), LIBC(strncmp),
            LIBC(strcasecmp), LIBC(strncasecmp), LIBC(strcoll), LIBC(strxfrm), LIBC(strchr), LIBC(strrchr),
            LIBC(memchr), LIBC(strspn), LIBC(strcspn), LIBC(strpbrk), LIBC(strstr), LIBC(strtok), LIBC(strtok_r),
            LIBC(strerror), LIBC(memcpy16), LIBC(memset_vmx),
            ::pspocket::hle::Function{{}, "_memset_int", ::pspocket::recomp::nid_of("_memset_int"), &detail::thunk<&memset_int_>, false},
            ::pspocket::hle::Function{{}, "strcasecmp_ascii", ::pspocket::recomp::nid_of("strcasecmp_ascii"), &detail::thunk<&strcasecmp_>, false},
            ::pspocket::hle::Function{{}, "strncasecmp_ascii", ::pspocket::recomp::nid_of("strncasecmp_ascii"), &detail::thunk<&strncasecmp_>, false},
            LIBC(isalnum), LIBC(isalpha), LIBC(isblank), LIBC(iscntrl), LIBC(isdigit), LIBC(isgraph), LIBC(islower),
            LIBC(isprint), LIBC(ispunct), LIBC(isspace), LIBC(isupper), LIBC(isxdigit), LIBC(isalnum_ascii),
            LIBC(isalpha_ascii), LIBC(isblank_ascii), LIBC(iscntrl_ascii), LIBC(isdigit_ascii), LIBC(isgraph_ascii),
            LIBC(islower_ascii), LIBC(isprint_ascii), LIBC(ispunct_ascii), LIBC(isspace_ascii), LIBC(isupper_ascii),
            LIBC(isxdigit_ascii), LIBC(tolower), LIBC(toupper),
            ::pspocket::hle::Function{{}, "tolower_ascii", ::pspocket::recomp::nid_of("tolower_ascii"), &detail::thunk<&tolower_>, false},
            ::pspocket::hle::Function{{}, "toupper_ascii", ::pspocket::recomp::nid_of("toupper_ascii"), &detail::thunk<&toupper_>, false},
            LIBC(abs), LIBC(labs), LIBC(llabs), LIBC(imaxabs), LIBC(lldiv),
            ::pspocket::hle::Function{{}, "div", ::pspocket::recomp::nid_of("div"), &detail::thunk<&div_>, false},
            ::pspocket::hle::Function{{}, "ldiv", ::pspocket::recomp::nid_of("ldiv"), &detail::thunk<&div_>, false},
            ::pspocket::hle::Function{{}, "imaxdiv", ::pspocket::recomp::nid_of("imaxdiv"), &detail::thunk<&lldiv_>, false},
            LIBC(strtol), LIBC(strtoul), LIBC(strtoll), LIBC(strtoull), LIBC(strtoimax), LIBC(strtoumax), LIBC(atoi),
            LIBC(atol), LIBC(atoll), LIBC(strtod), LIBC(strtold), LIBC(strtof), LIBC(atof), LIBC(atoff), LIBC(qsort),
            LIBC(bsearch), LIBC(rand), LIBC(srand), LIBC(init_TT800), LIBC(_init_TT800), LIBC(init_by_array_TT800),
            LIBC(_init_by_array_TT800), LIBC(rand_int32_TT800), LIBC(rand_int31_TT800), LIBC(rand_real1_TT800),
            LIBC(rand_real2_TT800), LIBC(rand_real3_TT800), LIBC(_rand_int32_TT800), LIBC(_rand_int31_TT800),
            LIBC(_rand_real1_TT800), LIBC(_rand_real2_TT800), LIBC(_rand_real3_TT800), LIBC(get_state_TT800),
            LIBC(_get_state_TT800),
        });
    }
    r.add_variables("sys_libc", {Variable{{}, "__TT800", recomp::nid_of("__TT800"), kTTSize, 8, &tt800_init}});
}
#undef LIBC

} // namespace pspocket::hle
