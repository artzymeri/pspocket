// cellHttpUtil: libhttp's URI, header, request and status line utilities, pspocket's own (D-500, D-5NN). Every function
// does what Sony's libhttp.sprx (4.92, whose module exports cellHttpUtil beside cellHttp) does, read from its code and
// compared call for call with it in tests/hle/test_httputil.cpp: the same answers, the same bytes in the pool and the
// structures, the same required sizes, including where Sony's code is surprising (each such place says so). RPCS3's
// rpcs3/Emu/Cell/Modules/cellHttpUtil.cpp/.h gives the structures, the error codes and the flags of cellHttpUtilBuildUri
// (and parses URIs with a general URL parser, which libhttp's own parser does not match).
//
// The structures, in guest memory (big-endian words, pointers into the caller's pool):
//   CellHttpUri         scheme, hostname, username, password, path (char*), port (u32), 4 reserved: 28 bytes
//   CellHttpUriPath     path, query, fragment: 12 bytes
//   CellHttpRequestLine method, path, protocol (char*), majorVersion, minorVersion (u32): 20 bytes
//   CellHttpStatusLine  protocol (char*), majorVersion, minorVersion (u32), reasonPhrase (char*), statusCode (s32),
//                       4 reserved: 24 bytes
//   CellHttpHeader      name, value (char*): 8 bytes
// The parsers and copies have two modes, as libhttp's: with a structure and a pool they fill both (the structure, and
// for the copies the whole pool, zeroed first); with either missing they only add up the pool size needed into
// *required (and refuse without `required`). Character classes are the C library's in the "C" locale (liblv2's
// __sys_look_ctype_table): a byte from 0x80 up is never a space, digit or letter. Copies between overlapping guest
// ranges are memmove's, as pspocket's _sys_memcpy, which libhttp calls. A pointer to memory that is not there, where
// Sony's library would fault, ends the call with CELL_HTTP_UTIL_ERROR_NO_BUFFER and the call site logged (D-042).

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {
namespace {

enum : u32 {
    CELL_HTTP_UTIL_ERROR_NO_MEMORY = 0x80711001,
    CELL_HTTP_UTIL_ERROR_NO_BUFFER = 0x80711002,
    CELL_HTTP_UTIL_ERROR_NO_STRING = 0x80711003,
    CELL_HTTP_UTIL_ERROR_INSUFFICIENT = 0x80711004,
    CELL_HTTP_UTIL_ERROR_INVALID_URI = 0x80711005,
    CELL_HTTP_UTIL_ERROR_INVALID_HEADER = 0x80711006,
    CELL_HTTP_UTIL_ERROR_INVALID_REQUEST = 0x80711007,
    CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE = 0x80711008,
    CELL_HTTP_UTIL_ERROR_INVALID_LENGTH = 0x80711009,
    CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER = 0x8071100A,
};
constexpr s32 kOk = 0;
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// cellHttpUtilBuildUri's flags.
constexpr u32 CELL_HTTP_UTIL_URI_FLAG_NO_SCHEME = 1, CELL_HTTP_UTIL_URI_FLAG_NO_CREDENTIALS = 2,
              CELL_HTTP_UTIL_URI_FLAG_NO_PASSWORD = 4, CELL_HTTP_UTIL_URI_FLAG_NO_PATH = 8;

// ------------------------------------------------------------------------------------------------- guest memory

struct Fault {
    u32 addr;
};

// Byte and word access to guest memory, each address checked (committed), the last 4 KB block remembered.
class Guest {
public:
    explicit Guest(PPUContext& ctx) : t_(thread_of(ctx)), base_(t_.kernel.base()) {}

    u8 r8(u32 a)
    {
        check(a);
        return ps_read8(base_, a);
    }
    s8 s8_at(u32 a) { return static_cast<s8>(r8(a)); }
    u32 r32(u32 a)
    {
        check(a);
        check(a + 3);
        return ps_read32(base_, a);
    }
    void w8(u32 a, u8 v)
    {
        check(a);
        ps_write8(base_, a, v);
    }
    void w32(u32 a, u32 v)
    {
        check(a);
        check(a + 3);
        ps_write32(base_, a, v);
    }
    // _sys_memcpy as pspocket's (memmove).
    void copy(u32 dst, u32 src, u32 n)
    {
        if (!n) return;
        range(dst, n);
        range(src, n);
        std::memmove(base_ + dst, base_ + src, n);
    }
    void fill(u32 dst, u8 v, u32 n)
    {
        if (!n) return;
        range(dst, n);
        std::memset(base_ + dst, v, n);
    }
    u32 strlen(u32 s)
    {
        u32 n = 0;
        while (r8(s + n)) n++;
        return n;
    }
    // _sys_strchr and _sys_strrchr for a character other than NUL: its address, or 0.
    u32 strchr(u32 s, u8 c)
    {
        for (u32 p = s;; p++) {
            const u8 b = r8(p);
            if (b == c) return p;
            if (!b) return 0;
        }
    }
    u32 strrchr(u32 s, u8 c)
    {
        u32 found = 0;
        for (u32 p = s;; p++) {
            const u8 b = r8(p);
            if (b == c) found = p;
            if (!b) return found;
        }
    }
    // _sys_strncasecmp(s, text, n) == 0 for an upper-case ASCII `text` of n characters: a prefix, case folded.
    bool starts_with_nocase(u32 s, std::string_view text)
    {
        for (usize i = 0; i < text.size(); i++) {
            u8 c = r8(s + static_cast<u32>(i));
            if (c >= 'a' && c <= 'z') c = static_cast<u8>(c - 0x20);
            if (c != static_cast<u8>(text[i])) return false;
        }
        return true;
    }

private:
    void check(u32 a)
    {
        if ((a >> 12) == block_) return;
        if (!t_.kernel.memory().is_committed(a)) throw Fault{a};
        block_ = a >> 12;
    }
    void range(u32 a, u32 n)
    {
        if (u64{a} + n > GuestMemory::kGuestSize) throw Fault{a};
        for (u64 b = a & ~u64{0xFFF}; b < u64{a} + n; b += 0x1000) check(static_cast<u32>(std::max<u64>(b, a)));
    }

    lv2::PpuThread& t_;
    u8* base_;
    u32 block_ = 0xFFFFFFFF;
};

// Runs a function body; memory that is not there ends it as described at the top.
template <class Body>
s32 guarded(PPUContext& ctx, std::string_view what, Body&& body)
{
    try {
        Guest g(ctx);
        return body(g);
    } catch (const Fault& f) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there (Sony's library faults); NO_BUFFER", what, f.addr));
        return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
    }
}

// The C locale's classes, for the bytes libhttp asks liblv2's ctype table about (it never asks about a byte >= 0x80).
bool is_space(s8 c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
bool is_digit(s8 c) { return c >= '0' && c <= '9'; }
bool is_alnum(s8 c) { return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

constexpr char kHex[] = "0123456789ABCDEF";

// Decimal digits of an unsigned word (libhttp's own formatter: at least one).
u32 digits(u32 v)
{
    u32 n = 0;
    do {
        n++;
        v /= 10;
    } while (v);
    return n;
}

void put_digits(Guest& g, u32 at, u32 v, u32 n)
{
    for (u32 k = n; k-- > 0;) {
        g.w8(at + k, static_cast<u8>('0' + v % 10));
        v /= 10;
    }
}

// ------------------------------------------------------------------------------------------------- URI parts
// libhttp's internal parsers of a URI's parts. Each takes the part's text (`str`, `len` bytes before the end) and a
// pool area (`dest`, `size` bytes) it copies the part into when `write`; *consumed is the length of text it took,
// *required the pool bytes the part needs.

// The scheme: letters, digits, '+', '-' and '.' up to a ':' (the first not a digit; an empty scheme ":" counts);
// none: an empty string. The text is read one byte past `len`.
s32 parse_scheme(Guest& g, u32 dest, u32 size, u32 str, u32 len, u32& consumed, u32& required, bool write)
{
    if (str && len) {
        u32 n = 0;
        for (;;) {
            const s8 c = g.s8_at(str + n);
            if (!(is_alnum(c) || c == '+' || c == '-' || c == '.')) break;
            if (n == len) break;
            n++;
        }
        if (!is_digit(g.s8_at(str)) && g.r8(str + n) == ':') {
            required = n + 1;
            if (write) {
                if (n + 1 > size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
                g.copy(dest, str, n);
                g.w8(dest + n, 0);
            }
            consumed = n + 1;
            return kOk;
        }
    }
    required = 1;
    if (write) {
        if (!size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
        g.w8(dest, 0);
    }
    consumed = 0;
    return kOk;
}

// RFC 2396's userinfo characters (unreserved, escaped, ";:&=+$,").
bool userinfo_char(s8 c)
{
    if (is_alnum(c)) return true;
    for (const char x : std::string_view("-_.!~*'()%;:&=+$,"))
        if (c == x) return true;
    return false;
}

// "user[:password]@": both copied, each with its NUL (the user name at dest, the password after it); *user_size and
// *pass_size their sizes with the NUL. Without an '@' before a character outside the class: two empty strings.
s32 parse_userinfo(Guest& g, u32 dest, u32 size, u32 str, u32 len, u32& consumed, u32& user_size, u32& pass_size,
    bool write)
{
    bool colon = false;
    u32 colon_at = 0;
    for (u32 i = 0;; i++) {
        if (i == len) break;
        const s8 c = g.s8_at(str + i);
        if (!c) break;
        if (c == '@') {
            const u32 after = i + 1;
            u32 pass_len = 0;
            if (colon) {
                pass_size = after - colon_at - 1;
                pass_len = after - colon_at - 2;
            } else {
                colon_at = i;
                user_size = after;
                pass_size = 1;
            }
            if (write) {
                if (user_size + pass_size > size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
                g.copy(dest, str, colon_at);
                g.w8(dest + colon_at, 0);
                g.copy(dest + user_size, str + user_size, pass_len);
                g.w8(dest + user_size + pass_len, 0);
            }
            consumed = after;
            return kOk;
        }
        if (!colon && c == ':') {
            colon = true;
            colon_at = i;
            user_size = i + 1;
        } else if (!userinfo_char(c)) {
            break;
        }
    }
    user_size = 1;
    pass_size = 1;
    if (write) {
        if (2 > size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
        g.w8(dest + 1, 0);
        g.w8(dest, 0);
    }
    consumed = 0;
    return kOk;
}

// The host name: letters, digits, '.', '_' and '-', trailing dots left out (and not consumed).
s32 parse_host(Guest& g, u32 dest, u32 size, u32 str, u32 len, u32& consumed, u32& required, bool write)
{
    u32 n = 0;
    while (n != len) {
        const s8 c = g.s8_at(str + n);
        if (!(is_alnum(c) || c == '.' || c == '_' || c == '-')) break;
        n++;
    }
    while (n > 0 && g.r8(str + n - 1) == '.') n--;
    if (!n) {
        required = 1;
        if (write) {
            if (!size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            g.w8(dest, 0);
        }
    } else {
        if (write) {
            if (n + 1 > size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            g.copy(dest, str, n);
            g.w8(dest + n, 0);
        }
        required = n + 1;
    }
    consumed = n;
    return kOk;
}

// The port: up to five digits (not past `len`), at most 65536, then '/' or the end (the byte at `len` is read too).
s32 parse_port(Guest& g, u32 str, u32 len, u32& port, u32& consumed)
{
    u32 i = 0, v = 0;
    for (;;) {
        const s8 c = g.s8_at(str + i);
        if (!is_digit(c) || i == len) break;
        if (i == 5) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
        v = v * 10 + static_cast<u32>(c - '0');
        i++;
    }
    if (v > 0x10000) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
    const s8 e = g.s8_at(str + i);
    if (e != '/' && e != 0) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
    port = v;
    consumed = i;
    return kOk;
}

// libhttp's path merge: appends the segments of `src` (`size` bytes with its NUL) at `out`, the path being built
// starting at `base`: "./" dropped, "../" cutting `base` back to its previous '/', the rest (from a '?' or, without
// one, a '#') copied as it is. With out == base it sweeps a path (cellHttpUtilSweepPath); a relative `src`, or one
// appended to a base not starting with '/', is copied as it is.
s32 merge_path(Guest& g, u32 out, u32 base, u32 src, u32 size)
{
    if (!out || !base || !src) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
    if (!size) return kOk;
    u32 cur = 0; // the last character written
    u32 i = 0;   // in src
    if (g.r8(src) == '/' && base == out) {
        g.w8(out + 1, 0);
        g.w8(out, '/');
        cur = base;
        i = 1;
    } else if (g.r8(src) == '/' || (base != out && g.r8(base) == '/')) {
        cur = out - 1;
    } else {
        g.copy(out, src, size - 1);
        g.w8(out + size - 1, 0);
        return kOk;
    }
    u32 query = g.strchr(src + i, '?');
    if (!query) query = g.strchr(src + i, '#');
    while (i < size - 1) {
        const u32 at = src + i;
        if (g.r8(at) == '.') {
            const s8 c1 = g.s8_at(at + 1);
            if (c1 == '.' && g.r8(at + 2) == '/') {
                if (cur != base) {
                    g.w8(cur, 0);
                    cur = g.strrchr(base, '/');
                    if (cur) g.w8(cur + 1, 0);
                }
                i += 3;
                continue;
            }
            if (c1 == '/') {
                i += 2;
                continue;
            }
        }
        const u32 slash = g.strchr(at, '/');
        u32 n = size - i - 1;
        if (slash && !(query && slash > query)) n = std::min(n, slash - at + 1);
        g.copy(cur + 1, at, n);
        cur += n;
        i += n;
        g.w8(cur + 1, 0);
    }
    return kOk;
}

// The path: the rest of the text up to a NUL, swept.
s32 parse_path(Guest& g, u32 dest, u32 size, u32 str, u32 len, u32& consumed, u32& required, bool write)
{
    u32 n = 0;
    if (str && len)
        while (n < len && g.r8(str + n)) n++;
    if (!n) {
        required = 1;
        if (write) {
            if (!size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            g.w8(dest, 0);
        }
        consumed = 0;
        return kOk;
    }
    if (write) {
        if (n + 1 > size) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
        g.copy(dest, str, n);
        g.w8(dest + n, 0);
        merge_path(g, dest, dest, str, n + 1);
    }
    required = n + 1;
    consumed = n;
    return kOk;
}

// The port a scheme implies: a case-insensitive prefix of a scheme name (so "httpx" is HTTP's), 80 for none.
u32 default_port(Guest& g, u32 scheme)
{
    if (!scheme) return 80;
    const u32 n = g.strlen(scheme);
    if (n > 127) return 0;
    if (!n) return 80;
    struct Known {
        std::string_view name;
        u32 port;
    };
    static constexpr Known kKnown[] = {{"HTTPS", 443}, {"HTTP", 80}, {"FTP", 21}, {"SVN+SSH", 22}, {"IMAP", 143},
        {"GOPHER", 70}, {"RTSP", 554}, {"SSH", 22}};
    for (const Known& k : kKnown)
        if (g.starts_with_nocase(scheme, k.name)) return k.port;
    return 0;
}

// Skips spaces (TAB, LF, VT, FF, CR, space) from `i` up to `len`.
u32 skip_spaces(Guest& g, u32 str, u32 i, u32 len)
{
    while (i != len && is_space(g.s8_at(str + i))) i++;
    return i;
}

// ------------------------------------------------------------------------------------------------- parsers

s32 cellHttpUtilParseUri(PPUContext& ctx, u32 uri, u32 str, u32 pool, u32 size, u32 required)
{
    return guarded(ctx, "cellHttpUtilParseUri", [&](Guest& g) -> s32 {
        if (!str) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = pool && uri;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (write) g.fill(uri, 0, 28);
        const auto fail = [&](s32 e) {
            if (write) g.fill(uri, 0, 28);
            return e;
        };
        const u32 len = g.strlen(str);
        u32 i = skip_spaces(g, str, 0, len);
        if (!g.r8(str + i)) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
        u32 consumed = 0, req = 0;
        if (const s32 r = parse_scheme(g, pool, size, str + i, len - i, consumed, req, write); r < 0) return fail(r);
        if (write) g.w32(uri + 0, pool);
        const u32 scheme_size = req;
        // "//" after a scheme (only after one) is skipped.
        const bool had_scheme = consumed != 0;
        i += consumed;
        u32 slashes = 0;
        while (g.r8(str + i + slashes) == '/') slashes++;
        if (slashes > 1 && had_scheme) i += 2;
        u32 user_size = 0, pass_size = 0;
        if (const s32 r = parse_userinfo(g, pool + scheme_size, size - scheme_size, str + i, len - i, consumed,
                user_size, pass_size, write);
            r < 0)
            return fail(r);
        if (write) {
            g.w32(uri + 8, pool + scheme_size);
            g.w32(uri + 12, pool + scheme_size + user_size);
        }
        const u32 used = scheme_size + user_size + pass_size;
        const u32 at = i + consumed;
        if (const s32 r = parse_host(g, pool + used, size - used, str + at, len - at, consumed, req, write); r < 0)
            return fail(r);
        if (write) g.w32(uri + 4, pool + used);
        const u32 host_size = req;
        u32 j = at + consumed;
        bool port_given = false;
        if (g.r8(str + j) == ':') {
            j++;
            u32 port = 0;
            if (const s32 r = parse_port(g, str + j, len - j, port, consumed); r < 0) return fail(r);
            if (consumed) {
                if (write) g.w32(uri + 20, port);
                j += consumed;
                port_given = true;
            }
        }
        if (!port_given && write) g.w32(uri + 20, default_port(g, g.r32(uri + 0)));
        const u32 path_at = used + host_size;
        if (const s32 r = parse_path(g, pool + path_at, size - path_at, str + j, len - j, consumed, req, write); r < 0)
            return fail(r);
        if (write) g.w32(uri + 16, pool + path_at);
        if (required) g.w32(required, path_at + req);
        return kOk;
    });
}

// A proxy: [user[:password]@]host[:port], the port 8080 when none is given. libhttp reserves an empty scheme in the
// pool but leaves the scheme and the path NULL, and ignores what follows the port.
s32 cellHttpUtilParseProxy(PPUContext& ctx, u32 uri, u32 str, u32 pool, u32 size, u32 required)
{
    return guarded(ctx, "cellHttpUtilParseProxy", [&](Guest& g) -> s32 {
        if (!str) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = pool && uri;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (write) g.fill(uri, 0, 28);
        const auto fail = [&](s32 e) {
            if (write) g.fill(uri, 0, 28);
            return e;
        };
        const u32 len = g.strlen(str);
        const u32 i = skip_spaces(g, str, 0, len);
        if (!g.r8(str + i)) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
        u32 consumed = 0, req = 0;
        if (const s32 r = parse_scheme(g, pool, size, 0, 0, consumed, req, write); r < 0) return fail(r);
        const u32 scheme_size = req;
        u32 user_size = 0, pass_size = 0;
        if (const s32 r = parse_userinfo(g, pool + scheme_size, size - scheme_size, str + i, len - i, consumed,
                user_size, pass_size, write);
            r < 0)
            return fail(r);
        if (write) {
            g.w32(uri + 8, pool + scheme_size);
            g.w32(uri + 12, pool + scheme_size + user_size);
        }
        const u32 at = i + consumed;
        const u32 used = scheme_size + user_size + pass_size;
        if (const s32 r = parse_host(g, pool + used, size - used, str + at, len - at, consumed, req, write); r < 0)
            return fail(r);
        if (write) g.w32(uri + 4, pool + used);
        const u32 host_size = req;
        u32 j = at + consumed;
        bool port_given = false;
        if (g.r8(str + j) == ':') {
            j++;
            u32 port = 0;
            if (const s32 r = parse_port(g, str + j, len - j, port, consumed); r < 0) return fail(r);
            if (consumed) {
                if (write) g.w32(uri + 20, port);
                port_given = true;
            }
        }
        if (!port_given && write) g.w32(uri + 20, 8080);
        const u32 path_at = used + host_size;
        if (const s32 r = parse_path(g, pool + path_at, size - path_at, 0, 0, consumed, req, write); r < 0)
            return fail(r);
        if (required) g.w32(required, path_at + req);
        return kOk;
    });
}

// path?query#fragment: the query keeps its '?' and the fragment its '#'; the path is swept.
s32 cellHttpUtilParseUriPath(PPUContext& ctx, u32 path, u32 str, u32 pool, u32 size, u32 required)
{
    return guarded(ctx, "cellHttpUtilParseUriPath", [&](Guest& g) -> s32 {
        if (!str) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = pool && path;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (write) g.fill(path, 0, 12);
        const auto no_memory = [&] {
            g.fill(path, 0, 12);
            return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
        };
        const u32 len = g.strlen(str);
        const u32 i = skip_spaces(g, str, 0, len);
        const u32 p = str + i;
        if (!g.r8(p)) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
        // The path: up to '?', '#' or the end.
        u32 n = 0;
        for (const u32 rest = len - i; n < rest; n++) {
            const s8 c = g.s8_at(p + n);
            if (c == '?' || c == '#' || !c) break;
        }
        const u32 path_size = n + 1;
        if (write) {
            if (path_size > size) return no_memory();
            if (!n) {
                g.w8(pool, 0);
            } else {
                g.copy(pool, p, n);
                g.w8(pool + n, 0);
                merge_path(g, pool, pool, p, path_size);
            }
            g.w32(path + 0, pool);
        }
        // The query: from the '?' up to '#' or the end.
        const u32 q_at = i + n;
        const u32 q = str + q_at;
        const u32 q_room = size - path_size;
        const u32 q_dest = pool + path_size;
        u32 qn = 0;
        if (q)
            for (const u32 rest = len - q_at; qn < rest; qn++) {
                const s8 c = g.s8_at(q + qn);
                if (c == '#' || !c) break;
            }
        const u32 query_size = qn + 1;
        if (write) {
            if (query_size > q_room) return no_memory();
            g.copy(q_dest, q, qn);
            g.w8(q_dest + qn, 0);
            g.w32(path + 4, q_dest);
        }
        // The fragment: from the '#' to the end.
        const u32 f_at = q_at + qn;
        const u32 f = str + f_at;
        const u32 used = path_size + query_size;
        const u32 f_dest = pool + used;
        u32 fn = 0;
        if (f)
            for (const u32 rest = len - f_at; fn < rest && g.r8(f + fn); fn++) {
            }
        const u32 fragment_size = fn + 1;
        if (write) {
            if (fragment_size > size - used) return no_memory();
            g.copy(f_dest, f, fn);
            g.w8(f_dest + fn, 0);
            g.w32(path + 8, f_dest);
        }
        if (required) g.w32(required, used + fragment_size);
        return kOk;
    });
}

// "HTTP/1.1 200 OK\r\n". libhttp's quirks: when the status code is not followed by a space, the reason phrase starts as
// many bytes after the code's start as the version took ("1.1 "); spaces before the phrase are skipped but its length
// is measured from before them (so as many bytes past the phrase are copied); *parsedLength counts from the phrase's
// start (before the spaces) to past its LF.
s32 cellHttpUtilParseStatusLine(
    PPUContext& ctx, u32 resp, u32 str, u32 len, u32 pool, u32 size, u32 required, u32 parsed_length)
{
    return guarded(ctx, "cellHttpUtilParseStatusLine", [&](Guest& g) -> s32 {
        if (!str) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = pool && resp;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (write) g.fill(resp, 0, 24);
        const auto fail = [&](u32 e) {
            if (write) g.fill(resp, 0, 24);
            return error(e);
        };
        const u32 i = skip_spaces(g, str, 0, len);
        const u32 p = str + i;
        const u32 rest = len - i;
        if (!g.r8(p)) return error(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        // The protocol, up to '/'.
        u32 s = 0;
        for (u64 budget = u64{rest} + 1; --budget != 0 && g.r8(p + s) != '/';) s++;
        if (!s || s >= rest || g.r8(p + s) != '/') return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        const u32 proto_size = s + 1;
        if (write) {
            if (proto_size > size) return fail(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            g.copy(pool, p, s);
            g.w8(pool + s, 0);
            g.w32(resp + 0, pool);
        }
        // major.minor and a space.
        const u32 v_at = i + proto_size;
        const u32 v = str + v_at;
        const u32 v_rest = len - v_at;
        if (!is_digit(g.s8_at(v))) return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        u32 a = 0, major = 0;
        for (s8 c; a != v_rest && is_digit(c = g.s8_at(v + a)); a++) major = major * 10 + static_cast<u32>(c - '0');
        if (g.r8(v + a) != '.') return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        u32 b = a + 1, minor = 0;
        if (!is_digit(g.s8_at(v + b))) return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        for (s8 c; b < v_rest && is_digit(c = g.s8_at(v + b)); b++) minor = minor * 10 + static_cast<u32>(c - '0');
        if (g.r8(v + b) != ' ') return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        u32 skip = b + 1;
        if (write) {
            g.w32(resp + 4, major);
            g.w32(resp + 8, minor);
        }
        // The status code.
        const u32 c_at = v_at + skip;
        const u32 code = str + c_at;
        const u32 c_rest = len - c_at;
        if (!is_digit(g.s8_at(code))) return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
        u32 m = 0, status = 0;
        for (s8 c; m != c_rest && is_digit(c = g.s8_at(code + m)); m++) status = status * 10 + static_cast<u32>(c - '0');
        if (g.r8(code + m) == ' ') skip = m + 1; // else `skip` stays the version's (Sony's code)
        if (write) g.w32(resp + 16, status);
        // The reason phrase, up to LF (a CR before it left out).
        const u32 r_at = c_at + skip;
        const u32 r = str + r_at;
        const u32 r_rest = len - r_at;
        const u32 w = skip_spaces(g, r, 0, r_rest);
        u32 e = 0;
        for (u64 budget = u64{r_rest} + 1;; e++) {
            if (--budget == 0) return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
            const s8 c = g.s8_at(r + e);
            if (!c) return fail(CELL_HTTP_UTIL_ERROR_INVALID_RESPONSE);
            if (c == '\n') break;
        }
        u32 n = e;
        if (e && g.r8(r + e - 1) == '\r') n--;
        const u32 reason_size = n + 1;
        if (write) {
            if (reason_size > size - proto_size) return fail(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            const u32 d = pool + proto_size;
            if (!n) {
                g.w8(d, 0);
            } else {
                g.copy(d, r + w, n);
                g.w8(d + n, 0);
            }
            g.w32(resp + 12, d);
        }
        if (required) g.w32(required, proto_size + reason_size);
        if (parsed_length) g.w32(parsed_length, e + 1);
        return kOk;
    });
}

// "Name: value\r\n", continuation lines (starting with a space) joined with one space each. libhttp's quirks: the ':' may
// be anywhere before `len`, past NULs; a value that ends exactly at `len` needs a NUL at str[len]; spaces at a line's
// end are dropped, not at the value's start of a continuation's line; the pool's room is checked even when nothing is
// written; *parsedLength counts from the value's start (after the spaces following the ':') to past the line break.
s32 cellHttpUtilParseHeader(
    PPUContext& ctx, u32 header, u32 str, u32 len, u32 pool, u32 size, u32 required, u32 parsed_length)
{
    return guarded(ctx, "cellHttpUtilParseHeader", [&](Guest& g) -> s32 {
        if (!str) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = pool && header;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (write) g.fill(header, 0, 8);
        const auto fail = [&](u32 e) {
            if (write) g.fill(header, 0, 8);
            return error(e);
        };
        const u32 i = skip_spaces(g, str, 0, len);
        const u32 p = str + i;
        const u32 rest = len - i;
        if (!g.r8(p)) return error(CELL_HTTP_UTIL_ERROR_INVALID_HEADER);
        // The name, up to ':' (searched through rest + 1 bytes).
        u64 colon = 0;
        while (colon <= rest && g.r8(p + static_cast<u32>(colon)) != ':') colon++;
        if (colon > rest || colon >= rest || colon == 0) return fail(CELL_HTTP_UTIL_ERROR_INVALID_HEADER);
        const u32 name_len = static_cast<u32>(colon);
        const u32 name_size = name_len + 1;
        if (write) {
            if (name_size > size) return fail(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            g.copy(pool, p, name_len);
            g.w8(pool + name_len, 0);
            g.w32(header + 0, pool);
        }
        u32 j = i + name_size;
        while (j < len && is_space(g.s8_at(str + j))) j++;
        if (j > len || (j == len && g.r8(str + j))) return fail(CELL_HTTP_UTIL_ERROR_INVALID_HEADER);
        const u32 vrest = len - j;
        const u32 room = size - name_size; // wraps when nothing is written, as Sony's
        const u32 dest = pool + name_size;
        const u32 src = str + j;
        u32 seg = 0, out = 0, last = 0, k = 0;
        bool continued = false;
        u32 at = 0;
        for (;;) {
            // One line's part: up to CR, LF, NUL or the end; `last` past its last character not a space.
            for (; k < vrest; k++) {
                const s8 c = g.s8_at(src + k);
                if (c == '\r' || c == '\n' || !c) break;
                if (!is_space(c)) last = k + 1;
            }
            const u32 n = last - seg;
            u32 add = n + (continued ? 1 : 0);
            if (out + add > room) return fail(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            if (continued && n) {
                if (write) g.w8(dest + out, ' ');
                out++;
                add--;
            }
            if (write && n) g.copy(dest + out, src + seg, n);
            out += add;
            // The line break: CRs and one LF.
            at = k;
            u32 lfs = 0;
            for (u64 budget = u64{vrest} - k + 1; --budget != 0; at++) {
                const s8 c = g.s8_at(src + at);
                if (c == '\n') {
                    if (++lfs > 1) break;
                } else if (c != '\r') {
                    break;
                }
            }
            if (at == vrest) break;
            const s8 c = g.s8_at(src + at);
            if (!is_space(c)) break;
            // A continuation line: its leading spaces skipped (they must end before `len`).
            for (k = at;; k++) {
                if (k >= vrest) return fail(CELL_HTTP_UTIL_ERROR_INVALID_HEADER);
                if (!is_space(g.s8_at(src + k))) break;
            }
            seg = k;
            continued = true;
        }
        const u32 value_size = out + 1;
        if (write) {
            if (value_size > room) return fail(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            g.w8(dest + out, 0);
            g.w32(header + 4, dest);
        }
        if (required) g.w32(required, name_size + value_size);
        if (parsed_length) g.w32(parsed_length, at);
        return kOk;
    });
}

// ------------------------------------------------------------------------------------------------- builders

// scheme://user:password@host:port/path with its NUL. With no scheme, "https" for port 443 and "http" for any other;
// the port is left out when it is the scheme's own ("http" 80, "https" 443, either 0; with no scheme 80, 443 or 0).
s32 cellHttpUtilBuildUri(PPUContext& ctx, u32 uri, u32 buf, u32 len, u32 required, u32 flags)
{
    return guarded(ctx, "cellHttpUtilBuildUri", [&](Guest& g) -> s32 {
        if (!uri || !g.r32(uri + 4)) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
        const bool no_scheme = flags & CELL_HTTP_UTIL_URI_FLAG_NO_SCHEME;
        const u32 host_len = g.strlen(g.r32(uri + 4));
        const u32 scheme = g.r32(uri + 0);
        const u32 scheme_len = scheme ? g.strlen(scheme) : 0;
        const auto length_of = [&](u32 field, u32 flag) -> u32 {
            if (flags & flag) return 0;
            const u32 s = g.r32(uri + field);
            return s ? g.strlen(s) : 0;
        };
        const u32 path_len = length_of(16, CELL_HTTP_UTIL_URI_FLAG_NO_PATH);
        const u32 user_len = length_of(8, CELL_HTTP_UTIL_URI_FLAG_NO_CREDENTIALS);
        const u32 pass_len = length_of(12, CELL_HTTP_UTIL_URI_FLAG_NO_PASSWORD);
        const u32 port = g.r32(uri + 20);
        u32 prefix = 0, port_digits = 0;
        if (!scheme_len) {
            if (port == 443) {
                prefix = no_scheme ? 0 : 8; // "https://"
            } else {
                if (port != 80 && port) port_digits = digits(port);
                prefix = no_scheme ? 0 : 7; // "http://"
            }
        } else {
            if (g.starts_with_nocase(scheme, std::string_view("HTTPS\0", 6))) {
                if (port && port != 443) port_digits = digits(port);
            } else if (g.starts_with_nocase(scheme, std::string_view("HTTP\0", 5))) {
                if (port && port != 80) port_digits = digits(port);
            } else {
                port_digits = digits(port);
            }
            prefix = no_scheme ? 0 : scheme_len + 3;
        }
        u32 total = prefix + path_len + host_len + 1;
        if (user_len) {
            total += user_len + 1;
            if (pass_len) total += pass_len + 1;
        }
        if (port_digits) total += port_digits + 1;
        if (required) g.w32(required, total);
        if (!buf) return kOk;
        if (len < total) return error(CELL_HTTP_UTIL_ERROR_INSUFFICIENT);
        u32 pos = 0;
        const auto put = [&](std::string_view text) {
            for (const char c : text) g.w8(buf + pos++, static_cast<u8>(c));
        };
        if (!no_scheme) {
            if (!scheme_len) {
                put(g.r32(uri + 20) == 443 ? "https" : "http");
            } else {
                g.copy(buf, g.r32(uri + 0), scheme_len);
                pos = scheme_len;
            }
            put("://");
        }
        if (user_len) {
            g.copy(buf + pos, g.r32(uri + 8), user_len);
            pos += user_len;
            if (pass_len) {
                put(":");
                g.copy(buf + pos, g.r32(uri + 12), pass_len);
                pos += pass_len;
            }
            put("@");
        }
        g.copy(buf + pos, g.r32(uri + 4), host_len);
        pos += host_len;
        if (port_digits) {
            put(":");
            put_digits(g, buf + pos, g.r32(uri + 20), port_digits);
            pos += port_digits;
        }
        if (path_len) {
            g.copy(buf + pos, g.r32(uri + 16), path_len);
            pos += path_len;
        }
        g.w8(buf + pos, 0);
        if (required) g.w32(required, pos + 1);
        return kOk;
    });
}

// "name: value\r\n", no NUL.
s32 cellHttpUtilBuildHeader(PPUContext& ctx, u32 header, u32 buf, u32 len, u32 required)
{
    return guarded(ctx, "cellHttpUtilBuildHeader", [&](Guest& g) -> s32 {
        if (!header || !g.r32(header)) return error(CELL_HTTP_UTIL_ERROR_INVALID_HEADER);
        const u32 name_len = g.strlen(g.r32(header));
        const u32 value = g.r32(header + 4);
        const u32 value_len = value ? g.strlen(value) : 0;
        const u32 total = name_len + value_len + 4;
        if (required) g.w32(required, total);
        if (!buf) return kOk;
        if (len < total) return error(CELL_HTTP_UTIL_ERROR_INSUFFICIENT);
        g.copy(buf, g.r32(header), name_len);
        g.w8(buf + name_len, ':');
        g.w8(buf + name_len + 1, ' ');
        g.copy(buf + name_len + 2, g.r32(header + 4), value_len);
        g.w8(buf + name_len + 2 + value_len, '\r');
        g.w8(buf + name_len + 3 + value_len, '\n');
        if (required) g.w32(required, total);
        return kOk;
    });
}

// "METHOD path PROTOCOL/major.minor\r\n" ("/" for an empty path), no NUL.
s32 cellHttpUtilBuildRequestLine(PPUContext& ctx, u32 req, u32 buf, u32 len, u32 required)
{
    return guarded(ctx, "cellHttpUtilBuildRequestLine", [&](Guest& g) -> s32 {
        if (!req || !g.r32(req) || !g.r32(req + 4) || !g.r32(req + 8))
            return error(CELL_HTTP_UTIL_ERROR_INVALID_REQUEST);
        const u32 method_len = g.strlen(g.r32(req));
        const u32 path_len = g.strlen(g.r32(req + 4));
        const u32 proto_len = g.strlen(g.r32(req + 8));
        const u32 major = g.r32(req + 12), minor = g.r32(req + 16);
        const u32 major_digits = digits(major), minor_digits = digits(minor);
        const u32 total = method_len + (path_len ? path_len : 1) + minor_digits + major_digits + proto_len + 6;
        if (required) g.w32(required, total);
        if (!buf) return kOk;
        if (len < total) return error(CELL_HTTP_UTIL_ERROR_INSUFFICIENT);
        u32 pos = 0;
        g.copy(buf, g.r32(req), method_len);
        pos = method_len;
        g.w8(buf + pos++, ' ');
        if (path_len) {
            g.copy(buf + pos, g.r32(req + 4), path_len);
            pos += path_len;
        } else {
            g.w8(buf + pos++, '/');
        }
        g.w8(buf + pos++, ' ');
        g.copy(buf + pos, g.r32(req + 8), proto_len);
        pos += proto_len;
        g.w8(buf + pos++, '/');
        put_digits(g, buf + pos, g.r32(req + 12), major_digits);
        pos += major_digits;
        g.w8(buf + pos++, '.');
        put_digits(g, buf + pos, g.r32(req + 16), minor_digits);
        pos += minor_digits;
        g.w8(buf + pos, '\r');
        g.w8(buf + pos + 1, '\n');
        if (required) g.w32(required, pos + 2);
        return kOk;
    });
}

// ------------------------------------------------------------------------------------------------- copies

// libhttp's string copy into a pool: `s` (NULL as "") with its NUL at dest, its address into the field; *used grows by
// the size before the room is checked.
s32 copy_string(Guest& g, u32 field, u32 dest, u32 room, u32 s, u32& used, bool write)
{
    const u32 n = s ? g.strlen(s) : 0;
    used += n + 1;
    if (!write) return kOk;
    if (n + 1 > room) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
    if (n) g.copy(dest, s, n);
    g.w8(dest + n, 0);
    g.w32(field, dest);
    return kOk;
}

// The copies' common frame: `fields` (offsets in the structure) copied in that order into the pool, zeroed first.
struct CopyFrame {
    Guest& g;
    u32 dest, pool, size, used = 0;
    u32 struct_size;
    bool write;

    s32 field(u32 src, u32 offset)
    {
        return copy_string(g, write ? dest + offset : 0, write ? pool + used : 0, write ? size - used : 0,
            g.r32(src + offset), used, write);
    }
    s32 fail(s32 e)
    {
        if (write) {
            g.fill(dest, 0, struct_size);
            g.fill(pool, 0, size);
        }
        return e;
    }
};

template <class More>
s32 copy_struct(PPUContext& ctx, std::string_view what, u32 dest, u32 src, u32 pool, u32 size, u32 required,
    u32 struct_size, std::initializer_list<u32> fields, More&& more)
{
    return guarded(ctx, what, [&](Guest& g) -> s32 {
        if (!src) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        CopyFrame f{g, dest, pool, size, 0, struct_size, pool && dest};
        if (!f.write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (f.write) {
            g.fill(dest, 0, struct_size);
            g.fill(pool, 0, size);
        }
        for (const u32 offset : fields)
            if (const s32 r = f.field(src, offset); r < 0) return f.fail(r);
        if (const s32 r = more(f); r < 0) return f.fail(r);
        if (required) g.w32(required, f.used);
        return kOk;
    });
}

s32 cellHttpUtilCopyUri(PPUContext& ctx, u32 dest, u32 src, u32 pool, u32 size, u32 required)
{
    return copy_struct(ctx, "cellHttpUtilCopyUri", dest, src, pool, size, required, 28, {0, 4, 16, 8, 12},
        [&](CopyFrame& f) {
            if (f.write) f.g.w32(dest + 20, f.g.r32(src + 20));
            return kOk;
        });
}

s32 cellHttpUtilCopyHeader(PPUContext& ctx, u32 dest, u32 src, u32 pool, u32 size, u32 required)
{
    return copy_struct(ctx, "cellHttpUtilCopyHeader", dest, src, pool, size, required, 8, {0, 4},
        [](CopyFrame&) { return kOk; });
}

s32 cellHttpUtilCopyStatusLine(PPUContext& ctx, u32 dest, u32 src, u32 pool, u32 size, u32 required)
{
    return copy_struct(ctx, "cellHttpUtilCopyStatusLine", dest, src, pool, size, required, 24, {0, 12},
        [&](CopyFrame& f) {
            if (f.write) {
                f.g.w32(dest + 16, f.g.r32(src + 16));
                f.g.w32(dest + 4, f.g.r32(src + 4));
                f.g.w32(dest + 8, f.g.r32(src + 8));
            }
            return kOk;
        });
}

// The header copied with `value` appended to its value after one space (spaces before `value` dropped, `value` cut at
// CR, LF or NUL; no space when either is empty).
s32 cellHttpUtilAppendHeaderValue(PPUContext& ctx, u32 dest, u32 src, u32 value, u32 pool, u32 size, u32 required)
{
    return copy_struct(ctx, "cellHttpUtilAppendHeaderValue", dest, src, pool, size, required, 8, {0},
        [&](CopyFrame& f) -> s32 {
            Guest& g = f.g;
            u32 out = 0, room = 0;
            if (f.write) {
                out = pool + f.used;
                room = size - f.used;
            }
            const u32 old = g.r32(src + 4);
            const u32 old_len = old ? g.strlen(old) : 0;
            u32 ws = 0;
            while (is_space(g.s8_at(value + ws))) ws++;
            u32 e = ws;
            for (s8 c; (c = g.s8_at(value + e)) != '\r' && c != '\n' && c; e++) {
            }
            const u32 n = e - ws;
            const u32 sep = old_len && n ? 1 : 0;
            const u32 total = old_len + sep + n;
            f.used += total + 1;
            if (!f.write) return kOk;
            if (total + 1 > room) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            if (old_len) g.copy(out, old, old_len);
            if (sep) g.w8(out + old_len, ' ');
            if (n) g.copy(out + old_len + sep, value + ws, n);
            g.w8(out + total, 0);
            g.w32(dest + 4, out);
            return kOk;
        });
}

s32 cellHttpUtilMergeUriPath(PPUContext& ctx, u32 uri, u32 src, u32 path, u32 pool, u32 size, u32 required)
{
    return guarded(ctx, "cellHttpUtilMergeUriPath", [&](Guest& g) -> s32 {
        if (!path) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        if (!src) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        CopyFrame f{g, uri, pool, size, 0, 28, pool && uri};
        if (!f.write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        if (f.write) {
            g.fill(uri, 0, 28);
            g.fill(pool, 0, size);
        }
        for (const u32 offset : {0u, 4u, 8u, 12u})
            if (const s32 r = f.field(src, offset); r < 0) return f.fail(r);
        u32 dest = 0, room = 0;
        if (f.write) {
            dest = pool + f.used;
            room = size - f.used;
        }
        const u32 base = g.r32(src + 16);
        const u32 path_len = g.strlen(path);
        // An absolute path replaces the base's; a relative one follows the base's directory (up to its last '/', before
        // a '?' or '#'), or "/" when the base has none.
        bool absolute = false, slash = false;
        u32 dir = 0;
        if (g.r8(path) == '/') {
            absolute = true;
        } else {
            u32 k = 0;
            if (base)
                for (u8 c; (c = g.r8(base + k)) && c != '#' && c != '?';) k++;
            if (!k) {
                dir = 1;
                slash = true;
            } else {
                dir = k;
                while (dir && g.r8(base + dir - 1) != '/') dir--;
            }
        }
        const u32 need = absolute ? path_len + 1 : dir + path_len + 1;
        f.used += need;
        if (f.write) {
            if (need > room) return f.fail(error(CELL_HTTP_UTIL_ERROR_NO_MEMORY));
            if (slash) {
                // Sony's code puts this '/' at dest + need, past the path (one byte past the pool when the path fills
                // it), so the path stays relative.
                g.w8(dest + need, '/');
            } else if (absolute) {
                g.w8(dest, 0);
            } else {
                merge_path(g, dest, dest, base, dir + 1);
            }
            merge_path(g, dest + g.strlen(dest), dest, path, path_len + 1);
            g.w32(uri + 20, g.r32(src + 20));
            g.w32(uri + 16, dest);
        }
        if (required) g.w32(required, f.used);
        return kOk;
    });
}

s32 cellHttpUtilSweepPath(PPUContext& ctx, u32 dst, u32 src, u32 size)
{
    return guarded(ctx, "cellHttpUtilSweepPath", [&](Guest& g) { return merge_path(g, dst, dst, src, size); });
}

// ------------------------------------------------------------------------------------------------- escapes

// Everything but letters, digits and "*-._" is escaped (%XX, upper case); a form encodes ' ' as '+'. `in_size` bytes
// are read, NULs included (none when in + in_size wraps or ends at 0). Without `out` only the size is counted.
s32 escape(PPUContext& ctx, std::string_view what, u32 out, u32 out_size, u32 in, u32 in_size, u32 required, bool form)
{
    return guarded(ctx, what, [&](Guest& g) -> s32 {
        if (!in || !in_size) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = out != 0;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        const u32 end = in + in_size;
        const u32 n = in > end || !end ? 0 : in_size;
        u32 count = 0, o = out;
        for (u32 i = 0; i < n; i++) {
            const u8 c = g.r8(in + i);
            const bool plain = is_alnum(static_cast<s8>(c)) || c == '*' || c == '-' || c == '.' || c == '_';
            count += form && c == ' ' ? 1 : plain ? 1 : 3;
            if (count > out_size) {
                if (write) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
                continue;
            }
            if (!write) continue;
            if (form && c == ' ') {
                g.w8(o++, '+');
            } else if (plain) {
                g.w8(o++, c);
            } else {
                g.w8(o, '%');
                g.w8(o + 1, static_cast<u8>(kHex[c >> 4]));
                g.w8(o + 2, static_cast<u8>(kHex[c & 15]));
                o += 3;
            }
        }
        count++;
        if (count > out_size) {
            if (write) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
        } else if (write) {
            g.w8(o, 0);
        }
        if (required) g.w32(required, count);
        return kOk;
    });
}

int hex_value(u8 c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// %XX decoded (a '%' without two hex digits: INVALID_URI); a form decodes '+' as ' '. Up to the NUL.
s32 unescape(PPUContext& ctx, std::string_view what, u32 out, u32 size, u32 in, u32 required, bool form)
{
    return guarded(ctx, what, [&](Guest& g) -> s32 {
        if (!in) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        const bool write = out != 0;
        if (!write && !required) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        u32 count = 0, p = in, o = out;
        for (;; count++) {
            const u8 c = g.r8(p);
            if (!c) break;
            if (count + 1 > size && write) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
            if (c == '%') {
                const u8 c1 = g.r8(p + 1);
                const int h1 = c1 ? hex_value(c1) : -1;
                if (h1 < 0) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
                const u8 c2 = g.r8(p + 2);
                const int h2 = c2 ? hex_value(c2) : -1;
                if (h2 < 0) return error(CELL_HTTP_UTIL_ERROR_INVALID_URI);
                if (write) g.w8(o++, static_cast<u8>((h1 << 4) + h2));
                p += 3;
            } else {
                if (write) g.w8(o++, form && c == '+' ? ' ' : c);
                p++;
            }
        }
        if (count + 1 > size) {
            if (write) return error(CELL_HTTP_UTIL_ERROR_NO_MEMORY);
        } else if (write) {
            g.w8(o, 0);
        }
        if (required) g.w32(required, count + 1);
        return kOk;
    });
}

s32 cellHttpUtilEscapeUri(PPUContext& ctx, u32 out, u32 out_size, u32 in, u32 in_size, u32 required)
{
    return escape(ctx, "cellHttpUtilEscapeUri", out, out_size, in, in_size, required, false);
}

s32 cellHttpUtilFormUrlEncode(PPUContext& ctx, u32 out, u32 out_size, u32 in, u32 in_size, u32 required)
{
    return escape(ctx, "cellHttpUtilFormUrlEncode", out, out_size, in, in_size, required, true);
}

s32 cellHttpUtilUnescapeUri(PPUContext& ctx, u32 out, u32 size, u32 in, u32 required)
{
    return unescape(ctx, "cellHttpUtilUnescapeUri", out, size, in, required, false);
}

s32 cellHttpUtilFormUrlDecode(PPUContext& ctx, u32 out, u32 size, u32 in, u32 required)
{
    return unescape(ctx, "cellHttpUtilFormUrlDecode", out, size, in, required, true);
}

// ------------------------------------------------------------------------------------------------- Base64

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// A character's 6 bits, -1 for one outside the alphabet. libhttp looks a character up in its table with the byte
// sign-extended, so a byte from 0x80 up reads the 128 bytes before the table: the end of its URI character class table
// (0x80..0xE7: 3), its hex digits ("0123456789ABCDEF" for 0xE8..0xF7, values up to 70 that spill into the next
// character's bits) and padding (0xF8..0xFF: 0).
int base64_value(u8 c)
{
    if (c >= 0x80) return c <= 0xE7 ? 3 : c <= 0xF7 ? kHex[c - 0xE8] : 0;
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Returns the number of characters written (no NUL).
s32 cellHttpUtilBase64Encoder(PPUContext& ctx, u32 out, u32 input, u32 len)
{
    return guarded(ctx, "cellHttpUtilBase64Encoder", [&](Guest& g) -> s32 {
        if (!input || !len) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        if (!out) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        u32 o = out, p = input, rest = len;
        for (u32 k = 0; k < len / 3; k++, p += 3, o += 4, rest -= 3) {
            const u32 v = u32{g.r8(p)} << 16 | u32{g.r8(p + 1)} << 8 | g.r8(p + 2);
            g.w8(o, static_cast<u8>(kBase64[v >> 18]));
            g.w8(o + 1, static_cast<u8>(kBase64[(v >> 12) & 63]));
            g.w8(o + 2, static_cast<u8>(kBase64[(v >> 6) & 63]));
            g.w8(o + 3, static_cast<u8>(kBase64[v & 63]));
        }
        if (rest) {
            u32 v = u32{g.r8(p)} << 16;
            if (rest == 2) v |= u32{g.r8(p + 1)} << 8;
            g.w8(o, static_cast<u8>(kBase64[v >> 18]));
            g.w8(o + 1, static_cast<u8>(kBase64[(v >> 12) & 63]));
            g.w8(o + 3, '=');
            g.w8(o + 2, rest == 2 ? static_cast<u8>(kBase64[(v >> 6) & 63]) : '=');
            o += 4;
        }
        return static_cast<s32>(o - out);
    });
}

// Returns the number of bytes decoded. libhttp writes the last group's three bytes whatever its padding.
s32 cellHttpUtilBase64Decoder(PPUContext& ctx, u32 output, u32 in, u32 len)
{
    return guarded(ctx, "cellHttpUtilBase64Decoder", [&](Guest& g) -> s32 {
        if (!in) return error(CELL_HTTP_UTIL_ERROR_NO_STRING);
        if (len & 3) return error(CELL_HTTP_UTIL_ERROR_INVALID_LENGTH);
        if (!output) return error(CELL_HTTP_UTIL_ERROR_NO_BUFFER);
        const u32 groups = len ? (len - 1) >> 2 : 0; // all but the last
        u32 p = in, written = 0;
        const auto value = [&](u32 at) {
            const int v = base64_value(g.r8(at));
            if (v < 0) throw error(CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER);
            return static_cast<u32>(v);
        };
        try {
            for (u32 k = 0; k < groups; k++, p += 4) {
                const u32 a = value(p), b = value(p + 1), c = value(p + 2), d = value(p + 3);
                const u32 v = a << 18 | b << 12 | c << 6 | d;
                const u32 o = output + written;
                written += 3;
                g.w8(o + 2, static_cast<u8>(v));
                g.w8(o, static_cast<u8>(v >> 16));
                g.w8(o + 1, static_cast<u8>(v >> 8));
            }
            if (len - 4 * groups != 4) return error(CELL_HTTP_UTIL_ERROR_INVALID_LENGTH);
            u32 v = value(p) << 18;
            v |= value(p + 1) << 12;
            s32 n = 0;
            if (g.r8(p + 2) == '=') {
                if (g.r8(p + 3) != '=') return error(CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER);
                n = static_cast<s32>(written + 1);
            } else {
                v |= value(p + 2) << 6;
                if (g.r8(p + 3) == '=') {
                    n = static_cast<s32>(written + 2);
                } else {
                    v |= value(p + 3);
                    n = static_cast<s32>(written + 3);
                }
            }
            const u32 o = output + written;
            g.w8(o + 2, static_cast<u8>(v));
            g.w8(o, static_cast<u8>(v >> 16));
            g.w8(o + 1, static_cast<u8>(v >> 8));
            return n;
        } catch (s32 e) {
            return e;
        }
    });
}

} // namespace

void register_cellHttpUtil(Registry& r)
{
    r.add("cellHttpUtil", {
        PS_HLE_FUNC(0x32FAAF58, cellHttpUtilParseUri),
        PS_HLE_FUNC(0x8BB608E4, cellHttpUtilParseUriPath),
        PS_HLE_FUNC(0xA3457869, cellHttpUtilParseProxy),
        PS_HLE_FUNC(0x2BCBCED4, cellHttpUtilParseStatusLine),
        PS_HLE_FUNC(0xE1FB0EBD, cellHttpUtilParseHeader),
        PS_HLE_FUNC(0x1C6E4DBB, cellHttpUtilBuildRequestLine),
        PS_HLE_FUNC(0x04ACCEBF, cellHttpUtilBuildHeader),
        PS_HLE_FUNC(0x6F0F7667, cellHttpUtilBuildUri),
        PS_HLE_FUNC(0xF05DF789, cellHttpUtilCopyUri),
        PS_HLE_FUNC(0x8EA23DEB, cellHttpUtilMergeUriPath),
        PS_HLE_FUNC(0xAABEB869, cellHttpUtilSweepPath),
        PS_HLE_FUNC(0x50EA75BC, cellHttpUtilCopyStatusLine),
        PS_HLE_FUNC(0x97F9FBE5, cellHttpUtilCopyHeader),
        PS_HLE_FUNC(0x37BB53A2, cellHttpUtilAppendHeaderValue),
        PS_HLE_FUNC(0x9003B1F2, cellHttpUtilEscapeUri),
        PS_HLE_FUNC(0x2763FD66, cellHttpUtilUnescapeUri),
        PS_HLE_FUNC(0x44D756D6, cellHttpUtilFormUrlEncode),
        PS_HLE_FUNC(0x8E6C5BB9, cellHttpUtilFormUrlDecode),
        PS_HLE_FUNC(0x83FAA354, cellHttpUtilBase64Encoder),
        PS_HLE_FUNC(0x8E52EE08, cellHttpUtilBase64Decoder),
    });
}

} // namespace pspocket::hle
