// cellHttp's cookie store (D-524): cellHttpInitCookie/EndCookie, adding cookies (cellHttpAddCookieWithClientId, and the
// Set-Cookie headers of responses), the Cookie header for a request (cellHttpGetCookie), export and import, the
// flushes, as libhttp.sprx keeps cookies. See cellHttp.cpp for the method.
//
// The store has a heap of its own ("cellHttpCookie_Library", over cellHttpInitCookie's pool; cellHttpInit isn't needed)
// and 16 buckets of domain records, a domain's bucket being the high nibble of the first MD5 byte of its registrable
// part. A domain record (36 bytes): bucket chain +0/+4, the store's list of records (most recently used first)
// +8/+12, its cookies +16, a "gone" switch +20, its users +24, its cookie count +28, the domain +32.
// A cookie (64 bytes): name +0, value +4, domain +8, path +12, two strings of libhttp's +16/+20, binary data +24 (+44
// halfwords of it), the next cookie +28, the expiry (u64, cellRtc ticks; 0 for a session cookie) +32, its size +40,
// +48, +52, secure +56, a session switch +57, deleted +58, the client it came from +60.
//
// A Set-Cookie value is read as libhttp reads it: its first attribute is the cookie (a name without '=' still makes
// one), then "secure", "expires" (cellRtcParseDateTime), "domain" (one with too few labels refuses the cookie) and
// "path"; the default path is the request path up to its last '/'; a domain gets a leading dot unless it has one or is
// an IPv4 address, and must match the host. The export is libhttp's binary format: the data's length (u32), its MD5,
// then per cookie type-length-value fields (1 expiry, 2 domain, 3 name, 4 value, 5 path, 6/7 libhttp's strings, 8
// binary data and 9 its length, 255 the end), session and expired cookies left out.

#include "cellHttp.hpp"

#include <initializer_list>
#include <utility>

#include "pspocket/core/log.hpp"

namespace pspocket::hle::http {

namespace {

// The store's record (libhttp's globals): sizes +0..+12, the record list +16/+20, buckets +24, the lock +88, limits
// +112/+116, the cookie count +120, the heap +124, initialised +128.
constexpr u32 kTotalMax = 0, kTotal = 4, kMaxSize = 8, kMaxExtra = 12, kFirst = 16, kLast = 20, kBuckets = 24,
              kLock = 88, kPerDomain = 112, kMaxCount = 116, kCount = 120, kHeap = 124, kInit = 128;

// A cookie's fields.
constexpr u32 cName = 0, cValue = 4, cDomain = 8, cPath = 12, c16 = 16, c20 = 20, cData = 24, cNext = 28, cExpires = 32,
              cSize = 40, cDataLen = 44, c48 = 48, c52 = 52, cSecure = 56, cSession = 57, cDeleted = 58, cClient = 60;
constexpr u32 kCookieSize = 64;
// A domain record's fields.
constexpr u32 dNext = 0, dPrev = 4, dLruNext = 8, dLruPrev = 12, dCookies = 16, dGone = 20, dUsers = 24, dCount = 28,
              dDomain = 32;
constexpr u32 kRecordSize = 36;

u32 store(PPUContext& ctx) { return data(ctx) + kCookie; }
bool cookie_ready(PPUContext& ctx) { return r8(ctx, store(ctx) + kInit) != 0; }

u32 cmalloc(PPUContext& ctx, u32 size)
{
    return static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_malloc", {r32(ctx, store(ctx) + kHeap), size}));
}

void cfree(PPUContext& ctx, u32 addr)
{
    call(ctx, "sysPrxForUser", "_sys_heap_free", {r32(ctx, store(ctx) + kHeap), addr});
}

// The C locale's space class (liblv2's ctype table), for bytes below 0x80 (libhttp asks about no other).
bool is_space(u8 c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

u8 lower(u8 c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// A copy of `len` bytes at `src` with a NUL after, in the store's heap (0 when there's no room).
u32 cstrdup(PPUContext& ctx, u32 src, u32 len)
{
    const u32 p = cmalloc(ctx, len + 1);
    if (!p) return 0;
    copy(ctx, p, src, len);
    w8(ctx, p + len, 0);
    return p;
}

// Whether a string is `word` (any case; strncasecmp over the word and its NUL).
bool is_word(PPUContext& ctx, u32 s, std::string_view word)
{
    for (u32 i = 0; i <= word.size(); i++) {
        const u8 c = r8(ctx, s + i);
        const u8 w = i < word.size() ? static_cast<u8>(word[i]) : 0;
        if (lower(c) != w) return false;
        if (!c) break;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------- cookies

// A cookie's strings freed (+16, +20, +8, +0, +12, +4, +24): its size.
u32 cookie_strings_free(PPUContext& ctx, u32 c)
{
    const u32 size = r32(ctx, c + cSize);
    for (const u32 f : {c16, c20, cDomain, cName, cPath, cValue, cData}) {
        if (const u32 p = r32(ctx, c + f)) {
            cfree(ctx, p);
            w32(ctx, c + f, 0);
        }
    }
    return size;
}

// A list of cookies (by +28) freed: their total size.
u32 cookie_list_free(PPUContext& ctx, u32 c)
{
    u32 sum = 0;
    while (c) {
        sum += cookie_strings_free(ctx, c);
        const u32 next = r32(ctx, c + cNext);
        cfree(ctx, c);
        c = next;
    }
    return sum;
}

// The fields of a cookie beyond its strings.
struct CookieExtra {
    u32 data_halfwords = 0; // +44
    u32 expires = 0;        // the expiry's address (a u64)
    u32 f48 = 0;
    u8 secure = 0;
    u8 session = 0;
    u32 f52 = 0;
    u32 client = 0;
};

// A cookie made (64 bytes) with copies of its strings; the path swept (cellHttpUtilSweepPath). The name and value may
// take 4096 bytes, all strings 4608; else CELL_HTTP_ERROR_INSUFFICIENT.
s32 cookie_create(PPUContext& ctx, u32 out, u32 name, u32 value, u32 s16, u32 s20, u32 domain, u32 path, u32 bin,
    const CookieExtra& x)
{
    const u32 st = store(ctx);
    const u32 c = cmalloc(ctx, kCookieSize);
    if (!c) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, c, kCookieSize);
    const auto fail = [&](s32 code) {
        cookie_strings_free(ctx, c);
        cfree(ctx, c);
        return code;
    };
    const u32 max = r32(ctx, st + kMaxSize), max_all = r32(ctx, st + kMaxSize) + r32(ctx, st + kMaxExtra);
    u32 size = 0;
    if (name) {
        size = str_len(ctx, name) + 1;
        if (max < size) return fail(err(CELL_HTTP_ERROR_INSUFFICIENT));
        const u32 p = cstrdup(ctx, name, str_len(ctx, name));
        w32(ctx, c + cName, p);
        if (!p) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
    }
    if (value) {
        size += str_len(ctx, value) + 1;
        if (max < size) return fail(err(CELL_HTTP_ERROR_INSUFFICIENT));
        const u32 p = cstrdup(ctx, value, str_len(ctx, value));
        w32(ctx, c + cValue, p);
        if (!p) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
    }
    for (const auto& [src, field] : {std::pair{s16, c16}, std::pair{s20, c20}, std::pair{domain, cDomain}}) {
        if (!src) continue;
        size += str_len(ctx, src) + 1;
        if (max_all < size) return fail(err(CELL_HTTP_ERROR_INSUFFICIENT));
        const u32 p = cstrdup(ctx, src, str_len(ctx, src));
        w32(ctx, c + field, p);
        if (!p) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
    }
    if (path) {
        size += str_len(ctx, path) + 1;
        if (max_all < size) return fail(err(CELL_HTTP_ERROR_INSUFFICIENT));
        const u32 p = cmalloc(ctx, str_len(ctx, path) + 1);
        w32(ctx, c + cPath, p);
        if (!p) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
        const s32 r = util(ctx, "cellHttpUtilSweepPath", {p, path, str_len(ctx, path) + 1});
        if (r < 0) return fail(r);
    }
    if (bin) {
        const u32 n = 2 * x.data_halfwords;
        size += n;
        if (max_all < size) return fail(err(CELL_HTTP_ERROR_INSUFFICIENT));
        const u32 p = cmalloc(ctx, n);
        w32(ctx, c + cData, p);
        if (!p) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
        copy(ctx, p, bin, n);
        w32(ctx, c + cDataLen, x.data_halfwords);
    }
    w8(ctx, c + cSecure, x.secure);
    w32(ctx, c + c48, x.f48);
    w32(ctx, out, c);
    w8(ctx, c + cSession, x.session);
    w32(ctx, c + c52, x.f52);
    w64(ctx, c + cExpires, r64(ctx, x.expires));
    w32(ctx, c + cSize, size);
    w32(ctx, c + cClient, x.client);
    return kOk;
}

// ------------------------------------------------------------------------------------------------- domains

// The part of a domain the store files it under (libhttp's): its last two labels, or three when the last label isn't
// one of the generic three-letter ones and the label before it is at most two characters ("co.jp"); 0 when the domain
// has fewer labels. Trailing dots are passed over; a domain starting with a dot counts that dot as no label.
u32 domain_base(PPUContext& ctx, u32 s)
{
    if (!s || str_len(ctx, s) == 0) return 0;
    const auto at = [&](u32 a) { return r8(ctx, a); };
    u32 r3 = s + str_len(ctx, s) - 1;
    u32 r10 = 0, r11 = 0, r7 = 0, sld = 0, labels = 0;
    bool same = false; // the last label reaches back to s
    bool trailing = at(r3) == '.';
    if (trailing) {
        while (s != r3) {
            r3--;
            if (at(r3) != '.') {
                trailing = false;
                break;
            }
        }
    }
    if (trailing) { // only dots back to s
        r11 = r3;
        r10 = 0;
        r7 = r11 + 1;
        same = s == r11;
    } else if (s == r3) {
        r10 = 1;
        r7 = s;
        same = true;
    } else {
        u32 ctr = r3 - s;
        r10 = 0;
        bool dot = false;
        for (;;) {
            r10++;
            r11 = r3 - r10;
            if (at(r11) == '.') {
                dot = true;
                break;
            }
            if (--ctr == 0) break;
        }
        if (dot) {
            r3 = r11;
            r7 = r11 + 1;
            same = s == r11;
        } else {
            same = s == r11;
            r3 = r11;
            r10++;
            r7 = s;
        }
    }
    bool counted = false;
    if (!same) {
        r11 = r3 - 1;
        u32 ctr = r3 - s;
        bool dot = at(r11) == '.';
        if (!dot) {
            for (;;) {
                r11--;
                sld++;
                if (--ctr == 0) break;
                if (at(r11) == '.') {
                    dot = true;
                    break;
                }
            }
        }
        if (dot) {
            labels = 2;
            for (u32 n = r11 - s; n; n--) {
                r11--;
                if (at(r11) == '.') labels++;
            }
            counted = true;
        }
    }
    if (!counted) labels = 1;
    if (at(s) != '.') labels++;
    u32 want = 3;
    if (sld > 2) {
        want = 2;
    } else if (r10 == 3) {
        for (const char* g : {"com", "edu", "net", "org", "gov", "mil", "int"}) {
            u32 k = 0;
            for (; k < 3; k++) {
                const u8 c = at(r7 + k);
                if (lower(c) != static_cast<u8>(g[k]) || !c) break;
            }
            if (k == 3) {
                want = 2;
                break;
            }
        }
    }
    if (labels < want) return 0;
    r11 = s + str_len(ctx, s) - 1;
    bool done = false;
    if (s != r11) {
        u32 ctr = r11 - s;
        for (;;) {
            r11--;
            if (at(r11) == '.') done = --want == 0;
            if (--ctr == 0) break;
            if (done) return r11 + 1;
        }
    }
    if (done) return r11 + 1;
    if (want != 1 || at(s) == '.') return 0;
    return s;
}

// The bucket of a domain: the high nibble of the first byte of the MD5 of its base (domain_base, else the domain), a
// leading dot left out, a trailing dot's byte left out of the length (libhttp hashes that many bytes from where it
// starts, so a leading dot without a trailing one takes the NUL in).
u32 domain_bucket(PPUContext& ctx, u32 domain)
{
    u32 r = domain_base(ctx, domain);
    if (!r) r = domain;
    u32 start = r, len = str_len(ctx, r);
    if (len != 0) {
        if (r8(ctx, r + len - 1) == '.') {
            len--;
            if (len != 0 && r8(ctx, r) == '.') start = r + 1;
        } else if (r8(ctx, r) == '.') {
            start = r + 1;
        }
    }
    u8 digest[16];
    md5(gb(ctx) + start, len, digest);
    return digest[0] >> 4;
}

// Whether libnet reads a string as an IPv4 address (inet_pton, AF_INET).
bool is_ipv4(PPUContext& ctx, u32 s)
{
    StackVar buf(ctx, gb(ctx), 16);
    return static_cast<s32>(call(ctx, "sys_net", "inet_pton", {2, s, buf.addr()})) == 1;
}

// A cookie domain against a host (libhttp's): 0 on a match. A trailing dot on either doesn't count. An IPv4 host must
// be the domain; a domain with a leading dot matches the host it names or its tail; another must be the host.
s32 domain_match(PPUContext& ctx, u32 domain, u32 host)
{
    u32 dl = str_len(ctx, domain), hl = str_len(ctx, host);
    if (dl == 0 || hl == 0) return -1;
    if (r8(ctx, domain + dl - 1) == '.') dl--;
    if (r8(ctx, host + hl - 1) == '.') hl--;
    if (is_ipv4(ctx, host)) return dl != hl ? -1 : str_ncasecmp(ctx, domain, host, dl + 1);
    if (r8(ctx, domain) == '.') {
        if (dl - 1 == hl) return str_ncasecmp(ctx, domain + 1, host, hl);
        if (dl > hl) return -1;
        return str_ncasecmp(ctx, domain, host + (hl - dl), dl);
    }
    if (dl != hl) return -1;
    return str_ncasecmp(ctx, domain, host, dl);
}

// ------------------------------------------------------------------------------------------------- domain records

// A domain record freed with every cookie it holds; the store's total lowered by their sizes.
void record_free(PPUContext& ctx, u32 rec)
{
    const u32 s = store(ctx);
    const u32 total = r32(ctx, s + kTotal);
    w32(ctx, s + kTotal, total - cookie_list_free(ctx, r32(ctx, rec + dCookies)));
    if (const u32 d = r32(ctx, rec + dDomain)) cfree(ctx, d);
    cfree(ctx, rec);
}

// A gone record taken out of the store's list and its bucket, then freed.
void record_unlink_free(PPUContext& ctx, u32 rec)
{
    const u32 s = store(ctx);
    const u32 lprev = r32(ctx, rec + dLruPrev), lnext = r32(ctx, rec + dLruNext);
    if (!lprev) w32(ctx, s + kFirst, lnext);
    else w32(ctx, lprev + dLruNext, lnext);
    if (!lnext) w32(ctx, s + kLast, lprev);
    else w32(ctx, lnext + dLruPrev, lprev);
    const u32 next = r32(ctx, rec + dNext), prev = r32(ctx, rec + dPrev);
    if (next) w32(ctx, next + dPrev, prev);
    if (!prev) w32(ctx, s + kBuckets + 4 * domain_bucket(ctx, r32(ctx, rec + dDomain)), next);
    else w32(ctx, prev + dNext, next);
    w32(ctx, rec + dNext, 0);
    w32(ctx, rec + dPrev, 0);
    record_free(ctx, rec);
}

// A record taken for use: CELL_HTTP_ERROR_LOCK_FAILED when it is gone.
s32 record_use(PPUContext& ctx, u32 rec)
{
    if (!rec || r8(ctx, rec + dGone)) return err(CELL_HTTP_ERROR_LOCK_FAILED);
    w32(ctx, rec + dUsers, r32(ctx, rec + dUsers) + 1);
    return kOk;
}

// A record given back: its last user removes its deleted cookies, and the record itself when it is gone or empty.
s32 record_release(PPUContext& ctx, u32 rec, bool lock)
{
    const u32 s = store(ctx);
    if (!rec) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (lock) {
        if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return r;
    }
    w32(ctx, rec + dUsers, r32(ctx, rec + dUsers) - 1);
    if (r32(ctx, rec + dUsers) == 0) {
        if (!r8(ctx, rec + dGone)) {
            u32 sum = 0, prev = 0;
            for (u32 c = r32(ctx, rec + dCookies); c;) {
                const u32 next = r32(ctx, c + cNext);
                if (r8(ctx, c + cDeleted)) {
                    if (prev) w32(ctx, prev + cNext, next);
                    else w32(ctx, rec + dCookies, next);
                    sum += cookie_strings_free(ctx, c);
                    cfree(ctx, c);
                } else {
                    prev = c;
                }
                c = next;
            }
            w32(ctx, s + kTotal, r32(ctx, s + kTotal) - sum);
            if (!r32(ctx, rec + dCookies)) w8(ctx, rec + dGone, 1);
        }
        if (r8(ctx, rec + dGone)) record_unlink_free(ctx, rec);
    }
    if (lock) lw_unlock(ctx, s + kLock);
    return kOk;
}

// A record made the most recently used.
void record_touch(PPUContext& ctx, u32 rec)
{
    const u32 s = store(ctx);
    const u32 lprev = r32(ctx, rec + dLruPrev), lnext = r32(ctx, rec + dLruNext);
    if (!lprev) w32(ctx, s + kFirst, lnext);
    else w32(ctx, lprev + dLruNext, lnext);
    if (!lnext) w32(ctx, s + kLast, lprev);
    else w32(ctx, lnext + dLruPrev, lprev);
    w32(ctx, rec + dLruPrev, 0);
    const u32 head = r32(ctx, s + kFirst);
    w32(ctx, rec + dLruNext, head);
    if (head) w32(ctx, head + dLruPrev, rec);
    else w32(ctx, s + kLast, rec);
    w32(ctx, s + kFirst, rec);
}

// A list of cookies (one domain, `size` bytes, `count` of them) stored: room made first by giving up the least
// recently used records (at least ten, or `count`), then the domain's record found or made; a cookie of the same name
// in it is deleted; past 50 cookies in a record its oldest are. With no room still, CELL_OK and nothing stored (the
// list stays the caller's; libhttp leaves it).
s32 cookies_store(PPUContext& ctx, u32 list, u32 size, u32 count)
{
    const u32 s = store(ctx);
    if (!list) return err(CELL_HTTP_ERROR_NO_BUFFER);
    bool evict = r32(ctx, s + kTotalMax) - r32(ctx, s + kTotal) < size;
    if (!evict) {
        const u32 max = r32(ctx, s + kMaxCount), now = r32(ctx, s + kCount);
        evict = max <= now || max - now < count;
    }
    if (evict && lw_lock(ctx, s + kLock) >= 0) {
        if (u32 rec = r32(ctx, s + kLast)) {
            u32 want = count < 10 ? 10 : count, seen = 0;
            for (;;) {
                if (record_use(ctx, rec) >= 0) {
                    seen++;
                    w32(ctx, s + kCount, r32(ctx, s + kCount) - r32(ctx, rec + dCount));
                    w8(ctx, rec + dGone, 1);
                    if (r32(ctx, rec + dCount) == 0) want++;
                    const u32 prev = r32(ctx, rec + dLruPrev); // libhttp reads it after the release: the same
                    record_release(ctx, rec, false);
                    rec = prev;
                } else {
                    want++;
                    seen++;
                    rec = r32(ctx, rec + dLruPrev);
                }
                if (want <= seen || !rec) break;
            }
        }
        lw_unlock(ctx, s + kLock);
    }
    if (r32(ctx, s + kTotalMax) - r32(ctx, s + kTotal) < size) return kOk;
    const u32 bucket = domain_bucket(ctx, r32(ctx, list + cDomain));
    u32 rec = r32(ctx, s + kBuckets + 4 * bucket);
    if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return r;
    u32 last = 0;
    for (; rec; rec = r32(ctx, rec + dNext)) {
        const u32 d = r32(ctx, rec + dDomain);
        if (str_ncmp(ctx, d, r32(ctx, list + cDomain), str_len(ctx, d) + 1) == 0) break;
        last = rec;
    }
    if (!rec) {
        lw_unlock(ctx, s + kLock);
        rec = cmalloc(ctx, kRecordSize);
        if (!rec) return err(CELL_HTTP_ERROR_NO_MEMORY);
        zero(ctx, rec, kRecordSize);
        const u32 domain = r32(ctx, list + cDomain);
        const u32 d = cstrdup(ctx, domain, str_len(ctx, domain));
        w32(ctx, rec + dDomain, d);
        if (!d) return err(CELL_HTTP_ERROR_NO_MEMORY);
        w32(ctx, rec + dCookies, list);
        w32(ctx, rec + dCount, count);
        if (last) {
            w32(ctx, rec + dPrev, last);
            w32(ctx, last + dNext, rec);
        } else {
            w32(ctx, s + kBuckets + 4 * bucket, rec);
        }
        w32(ctx, rec + dLruPrev, 0);
        const u32 head = r32(ctx, s + kFirst);
        w32(ctx, rec + dLruNext, head);
        if (head) w32(ctx, head + dLruPrev, rec);
        else w32(ctx, s + kLast, rec);
        w32(ctx, s + kFirst, rec);
    } else {
        record_use(ctx, rec);
        lw_unlock(ctx, s + kLock);
        s32 excess = static_cast<s32>(count + r32(ctx, rec + dCount) - r32(ctx, s + kPerDomain));
        if (excess > 0) {
            if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return r;
            for (u32 c = r32(ctx, rec + dCookies); c; c = r32(ctx, c + cNext)) {
                if (r8(ctx, c + cDeleted)) continue;
                excess--;
                w8(ctx, c + cDeleted, 1);
                w32(ctx, rec + dCount, r32(ctx, rec + dCount) - 1);
                w32(ctx, s + kCount, r32(ctx, s + kCount) - 1);
                if (excess == 0) break;
            }
            lw_unlock(ctx, s + kLock);
        }
        w32(ctx, rec + dCount, r32(ctx, rec + dCount) + count);
        u32 old = r32(ctx, rec + dCookies);
        if (!old) {
            w32(ctx, rec + dCookies, list);
        } else {
            for (;;) {
                for (u32 c = list; c; c = r32(ctx, c + cNext)) {
                    if (r8(ctx, c + cDeleted)) continue;
                    const u32 cn = r32(ctx, c + cName), on = r32(ctx, old + cName);
                    bool same = false;
                    if (!cn && !on) same = true;
                    else if (!r32(ctx, c + cValue) && !r32(ctx, old + cValue)) same = true;
                    else if (cn && on) same = str_ncmp(ctx, cn, on, str_len(ctx, cn) + 1) == 0;
                    if (same) {
                        w8(ctx, old + cDeleted, 1);
                        w32(ctx, rec + dCount, r32(ctx, rec + dCount) - 1);
                        w32(ctx, s + kCount, r32(ctx, s + kCount) - 1);
                        break;
                    }
                }
                if (!r32(ctx, old + cNext)) break;
                old = r32(ctx, old + cNext);
            }
            w32(ctx, old + cNext, list);
        }
        record_touch(ctx, rec);
        record_release(ctx, rec, true);
    }
    w32(ctx, s + kTotal, size + r32(ctx, s + kTotal));
    w32(ctx, s + kCount, count + r32(ctx, s + kCount));
    return kOk;
}

// ------------------------------------------------------------------------------------------------- Set-Cookie

// An attribute list (12-byte nodes {name, value, next}) freed from `node` on.
void attrs_free(PPUContext& ctx, u32 node)
{
    while (node) {
        for (const u32 f : {0u, 4u}) {
            if (const u32 p = r32(ctx, node + f)) {
                cfree(ctx, p);
                w32(ctx, node + f, 0);
            }
        }
        const u32 next = r32(ctx, node + 8);
        w32(ctx, node + 8, 0);
        cfree(ctx, node);
        node = next;
    }
}

// Whether the text at `p` (with `rem` bytes left) is the attribute `word` (any case), followed by the end, ';' or a
// space.
bool attr_is(PPUContext& ctx, u32 p, u32 rem, std::string_view word)
{
    const u32 n = static_cast<u32>(word.size());
    if (n > rem) return false;
    for (u32 i = 0; i < n; i++) {
        const u8 c = r8(ctx, p + i);
        if (lower(c) != static_cast<u8>(word[i]) || !c) return false;
    }
    if (n == rem) return true;
    const u8 c = r8(ctx, p + n);
    if (c == ';') return true;
    if (c >= 0x80) return false;
    return is_space(c);
}

// Trailing spaces of a string cleared.
void trim_end(PPUContext& ctx, u32 s)
{
    if (!s || str_len(ctx, s) == 0) return;
    for (u32 p = s + str_len(ctx, s) - 1;; p--) {
        const u8 c = r8(ctx, p);
        if (c >= 0x80 || !is_space(c)) return;
        w8(ctx, p, 0);
    }
}

} // namespace

// A Set-Cookie value for a host and path parsed into cookies (libhttp's): *list the cookies made, *count and *size
// theirs; the answer their size, or an error.
s32 cookie_parse(PPUContext& ctx, u32 str, u32 host, u32 path, u32 out_list, u32 out_count, u32 out_size, u32 client)
{
    const auto fail_out = [&](s32 code) {
        if (out_list) {
            if (const u32 l = r32(ctx, out_list)) {
                cookie_list_free(ctx, l);
                w32(ctx, out_list, 0);
            }
        }
        return code;
    };
    if (!str || !host || !path) return fail_out(err(CELL_HTTP_ERROR_NO_STRING));
    if (!out_list || !out_count || !out_size) return fail_out(err(CELL_HTTP_ERROR_NO_BUFFER));
    w32(ctx, out_size, 0);
    w32(ctx, out_list, 0);
    w32(ctx, out_count, 0);
    const u32 len = str_len(ctx, str);
    if (len == 0) return fail_out(err(CELL_HTTP_ERROR_NO_BUFFER));
    const auto ch = [&](u32 i) { return r8(ctx, str + i); };

    // The attributes, the latest first (each node's +8 the one before it).
    u8 secure = 0;
    u32 pos = 0, nodes = 0, last = 0;
    for (;;) {
        u32 next_pos = 0;
        if (attr_is(ctx, str + pos, len - pos, "secure")) {
            u32 ctr = (pos + 1 > len || len == 0) ? 1 : len - pos;
            bool semi = false;
            for (u32 q = pos;; q++) {
                if (ch(q) == ';') {
                    semi = true;
                    break;
                }
                pos++;
                if (--ctr == 0) break;
            }
            secure = 1;
            if (!semi) break;
            pos++;
            if (len > pos) continue;
            break;
        }
        // The name: spaces passed over (up to a byte from 0x80), then up to '=' or ';'.
        while (pos < len && ch(pos) < 0x80 && is_space(ch(pos))) pos++;
        u32 namelen = 0, end = pos;
        if (pos < len && ch(pos) != '=' && ch(pos) != ';') {
            for (u32 ctr = len - pos;;) {
                namelen++;
                end = pos + namelen;
                if (--ctr == 0) break;
                if (ch(end) == '=' || ch(end) == ';') break;
            }
        }
        const bool at_end = end == len;
        u8 noval = 0;
        bool bare_later = false;
        if (at_end || ch(end) == ';') {
            if (last) {
                if (at_end) break;
                next_pos = end + 1; // a bare attribute after the first is passed over
                bare_later = true;
            } else {
                noval = 1;
            }
        }
        if (!bare_later) {
            const u32 node = cmalloc(ctx, 12);
            if (!node) {
                if (last) attrs_free(ctx, last);
                return fail_out(err(CELL_HTTP_ERROR_NO_MEMORY));
            }
            zero(ctx, node, 12);
            w32(ctx, node + 8, last);
            nodes = node;
            if (namelen) {
                const u32 n = cstrdup(ctx, str + pos, namelen);
                w32(ctx, node + 0, n);
                if (!n) {
                    attrs_free(ctx, node);
                    return fail_out(err(CELL_HTTP_ERROR_NO_MEMORY));
                }
            }
            u32 vstart = end, vlen = 0;
            if (!noval && !at_end) {
                // After '=': spaces passed over (up to a byte from 0x80), then up to ';'.
                u32 q = end + 1;
                bool any = len > q;
                if (any) {
                    for (;;) {
                        const u8 c = ch(q);
                        if (c >= 0x80 || !is_space(c)) break;
                        q++;
                        if (len <= q) {
                            any = false;
                            break;
                        }
                    }
                }
                vstart = q;
                if (any && ch(vstart) != ';') {
                    u32 ctr = (vstart + 1 > len || len == 0) ? 1 : len - vstart;
                    for (u32 k = vstart;;) {
                        k++;
                        vlen++;
                        if (--ctr == 0) break;
                        if (ch(k) == ';') break;
                    }
                }
            }
            if (vlen != 0 || (namelen != 0 && !noval)) {
                const u32 v = cstrdup(ctx, str + vstart, vlen);
                w32(ctx, node + 4, v);
                if (!v) {
                    attrs_free(ctx, node);
                    return fail_out(err(CELL_HTTP_ERROR_NO_MEMORY));
                }
            }
            next_pos = vstart + vlen;
            if (next_pos < len && ch(next_pos) == ';') next_pos = vlen + vstart + 1;
            trim_end(ctx, r32(ctx, node + 0));
            trim_end(ctx, r32(ctx, node + 4));
        }
        if (len <= next_pos) break;
        pos = next_pos;
        bool done = false;
        for (;;) {
            const u8 c = ch(pos);
            if (c >= 0x80 || !is_space(c)) break;
            pos++;
            if (len == pos) {
                done = true;
                break;
            }
        }
        if (done) break;
        last = nodes;
        if (len <= pos) break;
    }
    if (!nodes) return fail_out(err(CELL_HTTP_ERROR_NO_BUFFER));

    // The cookie (the first attribute) and its attributes.
    w32(ctx, out_size, 0);
    w32(ctx, out_count, 0);
    StackVar v(ctx, gb(ctx), 0x10); // the expiry (u64), the cookie made
    const u32 expires = v.addr(), made = v.addr() + 8;
    w64(ctx, expires, 0);
    w32(ctx, made, 0);
    u32 dpath_src = path, dlen = 0;
    {
        u32 slash = 0;
        for (u32 i = 0, n = str_len(ctx, path); i < n; i++)
            if (r8(ctx, path + i) == '/') slash = path + i;
        if (slash) {
            dlen = slash - path + 1;
        } else {
            dpath_src = data(ctx) + kStrSlash;
            dlen = 1;
        }
    }
    const u32 dpath = cmalloc(ctx, dlen + 1);
    if (!dpath) {
        attrs_free(ctx, nodes);
        return fail_out(err(CELL_HTTP_ERROR_NO_MEMORY));
    }
    copy(ctx, dpath, dpath_src, dlen);
    w8(ctx, dpath + dlen, 0);
    s32 result = kOk;
    u32 recs = 0, made_list = 0, dombuf = 0, domain_attr = 0, path_attr = 0;
    const auto cleanup = [&](s32 code) {
        if (recs) attrs_free(ctx, recs);
        if (made_list) cookie_list_free(ctx, made_list);
        cfree(ctx, dpath);
        if (dombuf) cfree(ctx, dombuf);
        attrs_free(ctx, nodes);
        return code < 0 ? fail_out(code) : code;
    };
    for (u32 node = nodes; node;) {
        const u32 next = r32(ctx, node + 8), name = r32(ctx, node + 0), value = r32(ctx, node + 4);
        if (!next) {
            const u32 rec = cmalloc(ctx, 12);
            if (!rec) return cleanup(err(CELL_HTTP_ERROR_NO_MEMORY));
            zero(ctx, rec, 12);
            if (recs) w32(ctx, rec + 8, recs);
            recs = rec;
            if (name) {
                const u32 n = cstrdup(ctx, name, str_len(ctx, name));
                w32(ctx, rec + 0, n);
                if (!n) return cleanup(err(CELL_HTTP_ERROR_NO_MEMORY));
            }
            if (value) {
                const u32 n = cstrdup(ctx, value, str_len(ctx, value));
                w32(ctx, rec + 4, n);
                if (!n) return cleanup(err(CELL_HTTP_ERROR_NO_MEMORY));
            }
        } else if (name) {
            if (is_word(ctx, name, "expires")) {
                if (value) result = static_cast<s32>(call(ctx, "cellRtc", "cellRtcParseDateTime", {expires, value}));
            } else if (is_word(ctx, name, "domain")) {
                if (value) {
                    if (!domain_base(ctx, value)) return cleanup(result);
                    domain_attr = value;
                }
            } else if (is_word(ctx, name, "path")) {
                if (value) path_attr = value;
            }
        }
        node = next;
    }
    const u32 cpath = path_attr ? path_attr : dpath;
    const u32 domain = domain_attr ? domain_attr : host;
    u32 dom = domain;
    if (r8(ctx, domain) != '.' && !is_ipv4(ctx, domain)) {
        const u32 dl = str_len(ctx, domain);
        dombuf = cmalloc(ctx, dl + 2);
        if (!dombuf) return cleanup(err(CELL_HTTP_ERROR_NO_MEMORY));
        w8(ctx, dombuf, '.');
        copy(ctx, dombuf + 1, domain, dl);
        w8(ctx, dombuf + dl + 1, 0);
        dom = dombuf;
    }
    if (domain_match(ctx, dom, host) != 0) return cleanup(err(CELL_HTTP_ERROR_COOKIE_INVALID_DOMAIN));
    for (u32 rec = recs; rec; rec = r32(ctx, rec + 8)) {
        CookieExtra x;
        x.expires = expires;
        x.secure = secure;
        x.client = client;
        const s32 r = cookie_create(ctx, made, r32(ctx, rec + 0), r32(ctx, rec + 4), 0, 0, dom, cpath, 0, x);
        if (r < 0) return cleanup(r);
        const u32 c = r32(ctx, made);
        w32(ctx, out_count, r32(ctx, out_count) + 1);
        w32(ctx, out_size, r32(ctx, out_size) + r32(ctx, c + cSize));
        if (made_list) w32(ctx, c + cNext, made_list);
        made_list = c;
    }
    w32(ctx, out_list, made_list);
    made_list = 0;
    return cleanup(static_cast<s32>(r32(ctx, out_size)));
}

namespace {

// ------------------------------------------------------------------------------------------------- finding cookies

// The cookies for a host, path and scheme, in libhttp's order: blocks of ten 12-byte entries {cookie, previous, next}
// (the next block at +120, the count at +124), linked longest path first (then longest domain); the records used are
// listed in 8-byte nodes {record, next} at *records, held until cookies_found_free.
constexpr u32 kBlockSize = 132;

s32 cookie_order(PPUContext& ctx, u32 a, u32 b)
{
    s32 d = static_cast<s32>(str_len(ctx, r32(ctx, a + cPath)) - str_len(ctx, r32(ctx, b + cPath)));
    if (d == 0) d = static_cast<s32>(str_len(ctx, r32(ctx, a + cDomain)) - str_len(ctx, r32(ctx, b + cDomain)));
    return d;
}

void cookies_found_free(PPUContext& ctx, u32 block, u32 records)
{
    while (block) {
        const u32 next = r32(ctx, block + 120);
        cfree(ctx, block);
        block = next;
    }
    while (records) {
        const u32 next = r32(ctx, records + 4);
        record_release(ctx, r32(ctx, records), true);
        cfree(ctx, records);
        records = next;
    }
}

s32 cookies_find(PPUContext& ctx, u32 out, u32 host, u32 path, u32 scheme, u32 records)
{
    const u32 s = store(ctx);
    if (!records) return err(CELL_HTTP_ERROR_NO_BUFFER);
    u32 first = 0;
    // Before the first block: the records found are given back; after: the blocks too.
    const auto fail_early = [&](s32 code) {
        if (const u32 r = r32(ctx, records)) {
            cookies_found_free(ctx, 0, r);
            w32(ctx, records, 0);
        }
        return code;
    };
    const auto fail_late = [&](s32 code) {
        cookies_found_free(ctx, first, r32(ctx, records));
        w32(ctx, records, 0);
        return code;
    };
    if (!out) return fail_early(err(CELL_HTTP_ERROR_NO_BUFFER));
    w32(ctx, out, 0);
    w32(ctx, records, 0);
    const u32 bucket = domain_bucket(ctx, host);
    u32 rec = r32(ctx, s + kBuckets + 4 * bucket);
    if (!rec) return fail_early(err(CELL_HTTP_ERROR_COOKIE_NOT_FOUND));
    first = cmalloc(ctx, kBlockSize);
    if (!first) return fail_early(err(CELL_HTTP_ERROR_NO_MEMORY));
    zero(ctx, first, kBlockSize);
    bool restart = true;
    for (;;) {
        if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return fail_late(r);
        if (restart) {
            rec = r32(ctx, s + kBuckets + 4 * bucket);
            restart = false;
        }
        while (rec && (domain_match(ctx, r32(ctx, rec + dDomain), host) != 0 || record_use(ctx, rec) < 0))
            rec = r32(ctx, rec + dNext);
        lw_unlock(ctx, s + kLock);
        if (!rec) break;
        const u32 node = cmalloc(ctx, 8);
        if (!node) {
            record_release(ctx, rec, true);
            return fail_late(err(CELL_HTTP_ERROR_NO_MEMORY));
        }
        w32(ctx, node, rec);
        w32(ctx, node + 4, r32(ctx, records));
        w32(ctx, records, node);
        record_touch(ctx, rec);
        u32 block = first;
        while (r32(ctx, block + 120)) block = r32(ctx, block + 120);
        for (u32 c = r32(ctx, rec + dCookies); c; c = r32(ctx, c + cNext)) {
            if (r32(ctx, block + 124) > 9) {
                const u32 nb = cmalloc(ctx, kBlockSize);
                if (!nb) break;
                zero(ctx, nb, kBlockSize);
                w32(ctx, block + 120, nb);
                block = nb;
            }
            if (r8(ctx, c + cDeleted)) continue;
            const u32 cp = r32(ctx, c + cPath);
            const u32 want = path && str_len(ctx, path) != 0 ? path : data(ctx) + kStrSlash;
            if (str_ncasecmp(ctx, cp, want, str_len(ctx, cp)) != 0) continue;
            if (r8(ctx, c + cSecure) && !is_word(ctx, scheme, "https")) continue;
            const u32 n = r32(ctx, block + 124);
            const u32 e = block + 12 * n;
            w32(ctx, e, c);
            if (e != first) {
                const s32 cmp = cookie_order(ctx, r32(ctx, first), r32(ctx, e));
                if (cmp == 0) {
                    w32(ctx, e + 8, first);
                    w32(ctx, e + 4, r32(ctx, first + 4));
                    w32(ctx, first + 4, e);
                    if (r32(ctx, e + 4)) w32(ctx, r32(ctx, e + 4) + 8, e);
                } else if (cmp > 0) {
                    u32 at = first;
                    while (r32(ctx, at + 8) && cookie_order(ctx, r32(ctx, r32(ctx, at + 8)), r32(ctx, e)) > 0)
                        at = r32(ctx, at + 8);
                    w32(ctx, e + 4, at);
                    w32(ctx, e + 8, r32(ctx, at + 8));
                    w32(ctx, at + 8, e);
                    if (r32(ctx, e + 8)) w32(ctx, r32(ctx, e + 8) + 4, e);
                } else {
                    u32 at = first;
                    while (r32(ctx, at + 4) && cookie_order(ctx, r32(ctx, r32(ctx, at + 4)), r32(ctx, e)) < 0)
                        at = r32(ctx, at + 4);
                    w32(ctx, e + 8, at);
                    w32(ctx, e + 4, r32(ctx, at + 4));
                    w32(ctx, at + 4, e);
                    if (r32(ctx, e + 4)) w32(ctx, r32(ctx, e + 4) + 8, e);
                }
            }
            w32(ctx, block + 124, n + 1);
        }
        rec = r32(ctx, rec + dNext);
        if (!rec) break;
    }
    if (!r32(ctx, first)) return fail_late(err(CELL_HTTP_ERROR_COOKIE_NOT_FOUND));
    w32(ctx, out, first);
    return kOk;
}

// The Cookie header's value for found cookies ("name=value; ..."; expired ones left out): written into `buf` when there
// is one, its size with the NUL into *required.
s32 cookies_header(PPUContext& ctx, u32 buf, u32 first, u32 required, u32 size)
{
    bool write = true;
    if (!buf) {
        if (!required) return err(0x80711002); // libhttp answers cellHttpUtil's CELL_HTTP_UTIL_ERROR_NO_BUFFER
        write = false;
    } else {
        w8(ctx, buf, 0);
    }
    StackVar v(ctx, gb(ctx), 8);
    const s32 tick = static_cast<s32>(call(ctx, "cellRtc", "cellRtcGetCurrentTick", {v.addr()}));
    if (tick < 0) return tick;
    const u64 now = r64(ctx, v.addr());
    u32 e = first;
    while (r32(ctx, e + 4)) e = r32(ctx, e + 4);
    u32 total = 1, count = 0;
    const auto cat = [&](u32 src) { copy(ctx, buf + str_len(ctx, buf), src, str_len(ctx, src) + 1); };
    for (; e; e = r32(ctx, e + 8)) {
        const u32 c = r32(ctx, e);
        const u64 x = r64(ctx, c + cExpires);
        if (now > x && x != 0) continue;
        const u32 name = r32(ctx, c + cName), value = r32(ctx, c + cValue);
        if (!name && !value) {
            count++;
            continue;
        }
        const u32 n = name && value ? str_len(ctx, name) + 1 + str_len(ctx, value) + 2 : str_len(ctx, name ? name : value) + 2;
        total += n;
        count++;
        if (!write) continue;
        if (total > size) return err(CELL_HTTP_ERROR_INSUFFICIENT);
        if (name && value) {
            cat(name);
            cat(data(ctx) + kStrEquals);
            cat(value);
        } else {
            cat(name ? name : value);
        }
        cat(data(ctx) + kStrSemi);
    }
    if (write && str_len(ctx, buf) > 2) w8(ctx, buf + str_len(ctx, buf) - 2, 0);
    if (required) w32(ctx, required, total);
    if (count == 0) return err(CELL_HTTP_ERROR_COOKIE_NOT_FOUND);
    return tick;
}

// ------------------------------------------------------------------------------------------------- the API

// cellHttpInitCookie(void* pool, u32 poolSize): the store's heap over the pool, its lock "_httcok", its limits (80 KB
// of cookies, 4096-byte names and values, 50 cookies a domain, 300 in all).
s32 cellHttpInitCookie(PPUContext& ctx, u32 pool, u32 pool_size)
{
    const u32 s = store(ctx);
    if (!pool || !pool_size) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (cookie_ready(ctx)) return err(CELL_HTTP_ERROR_ALREADY_INITIALIZED);
    StackVar attr(ctx, gb(ctx), 8);
    w32(ctx, attr.addr(), 8);
    w32(ctx, attr.addr() + 4, pool);
    const u32 heap = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_create_heap",
        {data(ctx) + kStrCookieHeap, pool_size, 0x80000000u, attr.addr()}));
    if (!heap) return err(CELL_HTTP_ERROR_NO_MEMORY);
    if (const s32 r = lw_create(ctx, s + kLock, "_httcok"); r < 0) return r; // libhttp leaves the heap made
    w32(ctx, s + kTotalMax, 0x14000);
    w8(ctx, s + kInit, 1);
    w32(ctx, s + kMaxExtra, 512);
    w32(ctx, s + kMaxSize, 4096);
    w32(ctx, s + kHeap, heap);
    w32(ctx, s + kPerDomain, 50);
    w32(ctx, s + kMaxCount, 300);
    w32(ctx, s + kLast, 0);
    w32(ctx, s + kTotal, 0);
    w32(ctx, s + kFirst, 0);
    return kOk;
}

// cellHttpEndCookie: every record and cookie freed (without the lock), the lock destroyed, the heap deleted.
s32 cellHttpEndCookie(PPUContext& ctx)
{
    const u32 s = store(ctx);
    if (!cookie_ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    for (u32 b = 0; b < 16; b++) {
        for (u32 rec = r32(ctx, s + kBuckets + 4 * b); rec;) {
            const u32 next = r32(ctx, rec + dNext);
            record_free(ctx, rec);
            rec = next;
        }
        w32(ctx, s + kBuckets + 4 * b, 0);
    }
    w8(ctx, s + kInit, 0);
    w32(ctx, s + kTotal, 0);
    w32(ctx, s + kCount, 0);
    lw_destroy(ctx, s + kLock);
    const u32 heap = r32(ctx, s + kHeap);
    w32(ctx, s + kHeap, 0);
    call(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap});
    return kOk;
}

// cellHttpCookieFlush: every record marked gone (the store's count lowered by its cookies) and given back.
s32 cellHttpCookieFlush(PPUContext& ctx)
{
    const u32 s = store(ctx);
    if (!cookie_ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return r;
    for (u32 b = 0; b < 16; b++) {
        for (u32 rec = r32(ctx, s + kBuckets + 4 * b); rec;) {
            const u32 next = r32(ctx, rec + dNext);
            if (record_use(ctx, rec) >= 0) {
                w32(ctx, s + kCount, r32(ctx, s + kCount) - r32(ctx, rec + dCount));
                w8(ctx, rec + dGone, 1);
                record_release(ctx, rec, false);
            }
            rec = next;
        }
    }
    lw_unlock(ctx, s + kLock);
    return kOk;
}

// cellHttpAddCookieWithClientId(const CellHttpUri* uri, const char* cookie, clientId): a Set-Cookie value for the URI's
// host and path, stored. A list the store has no room for is left (libhttp's).
s32 cellHttpAddCookieWithClientId(PPUContext& ctx, u32 uri, u32 cookie, u32 client)
{
    if (!uri) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (!cookie) return err(CELL_HTTP_ERROR_NO_STRING);
    if (!usable(ctx, uri, 28, "cellHttpAddCookieWithClientId: uri")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    StackVar v(ctx, gb(ctx), 16); // the count, the size, the list
    const u32 count = v.addr(), size = v.addr() + 4, list = v.addr() + 8;
    s32 r = cookie_parse(ctx, cookie, r32(ctx, uri + 4), r32(ctx, uri + 16), list, count, size, client);
    if (r >= 0) {
        r = cookies_store(ctx, r32(ctx, list), r32(ctx, size), r32(ctx, count));
        if (r >= 0) return r;
    }
    if (const u32 l = r32(ctx, list)) cookie_list_free(ctx, l);
    return r;
}

// cellHttpGetCookie(const CellHttpUri* uri, char* buf, u32 size, u32* required): the Cookie header's value for the URI.
s32 cellHttpGetCookie(PPUContext& ctx, u32 uri, u32 buf, u32 size, u32 required)
{
    if (!uri) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (!usable(ctx, uri, 28, "cellHttpGetCookie: uri")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    StackVar v(ctx, gb(ctx), 8);
    const u32 first = v.addr(), records = v.addr() + 4;
    s32 r = cookies_find(ctx, first, r32(ctx, uri + 4), r32(ctx, uri + 16), r32(ctx, uri + 0), records);
    if (r < 0) r = err(CELL_HTTP_ERROR_COOKIE_NOT_FOUND);
    else if (r32(ctx, first)) r = cookies_header(ctx, buf, r32(ctx, first), required, size);
    else r = err(CELL_HTTP_ERROR_COOKIE_NOT_FOUND);
    if (r32(ctx, first) || r32(ctx, records)) cookies_found_free(ctx, r32(ctx, first), r32(ctx, records));
    return r;
}

// A type-length-value field's header: the type, then the length (big-endian).
void tlv(PPUContext& ctx, u32 at, u8 type, u16 len)
{
    w8(ctx, at, type);
    w16(ctx, at + 1, len);
}

// cellHttpCookieExport(WithClientId)(void* buffer, u32 size, u32* exportSize[, clientId]): the store's lasting cookies
// (not session, not expired; the client's when one is given) in libhttp's format. A cookie that doesn't fit ends its
// domain's (the export goes on with the next domain). Without a buffer only *exportSize.
s32 cookie_export(PPUContext& ctx, u32 buf, u32 size, u32 export_size, u32 client)
{
    const u32 s = store(ctx);
    const bool count_only = !buf;
    if (buf && size <= 19) return err(CELL_HTTP_ERROR_INSUFFICIENT);
    if (!cookie_ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    StackVar v(ctx, gb(ctx), 0x140); // now (u64), two date texts
    const u32 now = v.addr(), text1 = v.addr() + 0x10, text2 = v.addr() + 0xA0;
    if (const s32 r = static_cast<s32>(call(ctx, "cellRtc", "cellRtcGetCurrentTick", {now})); r < 0) return r;
    if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return r;
    const u32 data_at = buf + 20;
    u32 used = 0;
    for (u32 rec = r32(ctx, s + kLast); rec; rec = r32(ctx, rec + dLruPrev)) {
        if (r8(ctx, rec + dGone)) continue;
        for (u32 c = r32(ctx, rec + dCookies); c; c = r32(ctx, c + cNext)) {
            const u64 x = r64(ctx, c + cExpires);
            if (r8(ctx, c + cSession) || r8(ctx, c + cDeleted) || x == 0 || r64(ctx, now) > x) {
                // libhttp formats both dates here and drops them
                call(ctx, "cellRtc", "cellRtcFormatRfc3339LocalTime", {text1, now});
                call(ctx, "cellRtc", "cellRtcFormatRfc3339LocalTime", {text2, c + cExpires});
                continue;
            }
            if (client && r32(ctx, c + cClient) != client) continue;
            if (!count_only && r32(ctx, c + cSize) + 38 + used > size) break;
            const u32 domain = r32(ctx, rec + dDomain);
            u32 at = used;
            if (!count_only) {
                tlv(ctx, data_at + at, 1, 8);
                copy(ctx, data_at + at + 3, c + cExpires, 8);
                tlv(ctx, data_at + at + 11, 2, static_cast<u16>(str_len(ctx, domain)));
                copy(ctx, data_at + at + 14, domain, str_len(ctx, domain));
            }
            at += 14 + str_len(ctx, domain);
            const auto field = [&](u8 type, u32 p) {
                if (!count_only) {
                    tlv(ctx, data_at + at, type, static_cast<u16>(str_len(ctx, p)));
                    copy(ctx, data_at + at + 3, p, str_len(ctx, p));
                }
                at += 3 + str_len(ctx, p);
            };
            if (const u32 p = r32(ctx, c + cName)) field(3, p);
            if (const u32 p = r32(ctx, c + cValue)) field(4, p);
            field(5, r32(ctx, c + cPath));
            if (const u32 p = r32(ctx, c + c16)) field(6, p);
            if (const u32 p = r32(ctx, c + c20)) field(7, p);
            if (r32(ctx, c + cData)) {
                const u32 n = 2 * r32(ctx, c + cDataLen);
                if (!count_only) {
                    tlv(ctx, data_at + at, 8, static_cast<u16>(n));
                    copy(ctx, data_at + at + 3, r32(ctx, c + cData), n);
                    tlv(ctx, data_at + at + 3 + n, 9, 4);
                    copy(ctx, data_at + at + 6 + n, c + cDataLen, 4);
                }
                at += n + 10;
            }
            if (!count_only) tlv(ctx, data_at + at, 255, 0);
            used = at + 3;
        }
    }
    if (!count_only) {
        u8 digest[16];
        md5(gb(ctx) + data_at, used, digest);
        w32(ctx, buf, used);
        for (u32 i = 0; i < 16; i++) w8(ctx, buf + 4 + i, digest[i]);
    }
    lw_unlock(ctx, s + kLock);
    if (export_size) w32(ctx, export_size, used + 20);
    return kOk;
}

s32 cellHttpCookieExport(PPUContext& ctx, u32 buf, u32 size, u32 export_size)
{
    return cookie_export(ctx, buf, size, export_size, 0);
}
s32 cellHttpCookieExportWithClientId(PPUContext& ctx, u32 buf, u32 size, u32 export_size, u32 client)
{
    return cookie_export(ctx, buf, size, export_size, client);
}

// cellHttpCookieImportWithClientId(const void* buffer, u32 size, clientId): libhttp's export read back (a buffer whose
// length or MD5 doesn't hold is CELL_OK with nothing imported); each lasting cookie made for the client and stored.
s32 cellHttpCookieImportWithClientId(PPUContext& ctx, u32 buf, u32 size, u32 client)
{
    StackVar v(ctx, gb(ctx), 0x60); // the cookie read (64 bytes), now, the expiry, the cookie made
    const u32 tmp = v.addr(), now = v.addr() + 0x40, expires = v.addr() + 0x48, made = v.addr() + 0x50;
    zero(ctx, tmp, 64);
    if (!cookie_ready(ctx)) {
        cookie_strings_free(ctx, tmp);
        return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    }
    s32 result = static_cast<s32>(call(ctx, "cellRtc", "cellRtcGetCurrentTick", {now}));
    const auto finish = [&](s32 code) {
        cookie_strings_free(ctx, tmp);
        return code;
    };
    if (result < 0) return finish(result);
    if (!usable(ctx, buf, 20, "cellHttpCookieImport: buffer")) return finish(err(CELL_HTTP_ERROR_NO_BUFFER));
    const u32 len = r32(ctx, buf);
    if (len + 20 > size) return finish(result);
    u8 digest[16];
    md5(gb(ctx) + buf + 20, len, digest);
    for (u32 i = 0; i < 16; i++)
        if (r8(ctx, buf + 4 + i) != digest[i]) return finish(result);
    if (len == 0) return finish(kOk);
    if (len <= 2) return finish(result);
    u32 p = buf + 20, rem = len;
    bool expired = false;
    for (;;) {
        const u8 type = r8(ctx, p);
        const u32 flen = r16(ctx, p + 1);
        const u32 after = rem - 3;
        if (after < flen) return finish(result);
        p += 3;
        const auto string_field = [&](u32 field) {
            if (const u32 old = r32(ctx, tmp + field)) cfree(ctx, old);
            const u32 sp = cmalloc(ctx, flen + 1);
            w32(ctx, tmp + field, sp);
            if (!sp) return false;
            copy(ctx, sp, p, flen);
            w8(ctx, sp + flen, 0);
            return true;
        };
        switch (type) {
        case 1:
            if (flen != 8) return finish(err(CELL_HTTP_ERROR_NO_MEMORY));
            copy(ctx, expires, p, 8);
            if (r64(ctx, expires) < r64(ctx, now)) expired = true;
            break;
        case 2: if (!string_field(cDomain)) return finish(err(CELL_HTTP_ERROR_NO_MEMORY)); break;
        case 3: if (!string_field(cName)) return finish(err(CELL_HTTP_ERROR_NO_MEMORY)); break;
        case 4: if (!string_field(cValue)) return finish(err(CELL_HTTP_ERROR_NO_MEMORY)); break;
        case 5: if (!string_field(cPath)) return finish(err(CELL_HTTP_ERROR_NO_MEMORY)); break;
        case 6: if (!string_field(c16)) return finish(err(CELL_HTTP_ERROR_NO_MEMORY)); break;
        case 7: if (!string_field(c20)) return finish(err(CELL_HTTP_ERROR_NO_MEMORY)); break;
        case 8: {
            if (const u32 old = r32(ctx, tmp + cData)) cfree(ctx, old);
            const u32 sp = cmalloc(ctx, flen + 1);
            w32(ctx, tmp + cData, sp);
            if (!sp) return finish(err(CELL_HTTP_ERROR_NO_MEMORY));
            copy(ctx, sp, p, flen);
            break;
        }
        case 9:
            if (flen != 4) return finish(result);
            copy(ctx, tmp + cDataLen, p, 4);
            break;
        case 255: {
            const u32 domain = r32(ctx, tmp + cDomain);
            if (!expired && domain && (r32(ctx, tmp + cName) || r32(ctx, tmp + cValue))) {
                CookieExtra x;
                x.data_halfwords = r32(ctx, tmp + cDataLen);
                x.expires = expires;
                x.f48 = r32(ctx, tmp + c48);
                x.secure = r8(ctx, tmp + cSecure);
                x.client = client;
                result = cookie_create(ctx, made, r32(ctx, tmp + cName), r32(ctx, tmp + cValue), r32(ctx, tmp + c16),
                    r32(ctx, tmp + c20), domain, r32(ctx, tmp + cPath), r32(ctx, tmp + cData), x);
                if (result < 0) return finish(result);
                const u32 c = r32(ctx, made);
                result = cookies_store(ctx, c, r32(ctx, c + cSize), 1);
                if (result < 0) cookie_strings_free(ctx, c);
            }
            expired = false;
            cookie_strings_free(ctx, tmp);
            zero(ctx, tmp, 64);
            break;
        }
        default: break;
        }
        const u32 left = after - flen;
        if (left == 0) return finish(kOk);
        if (left <= 2) return finish(result);
        p += flen;
        rem = left;
    }
}

// cellHttpCookieImport(buffer, size): the store flushed first; out of memory answers CELL_OK (libhttp's).
s32 cellHttpCookieImport(PPUContext& ctx, u32 buf, u32 size)
{
    cellHttpCookieFlush(ctx);
    const s32 r = cellHttpCookieImportWithClientId(ctx, buf, size, 0);
    return r == err(CELL_HTTP_ERROR_NO_MEMORY) ? kOk : r;
}

} // namespace

// cellHttpSessionCookieFlush(clientId): the client's session cookies (no expiry, or marked so) deleted. Without the
// store, CELL_OK.
s32 session_cookie_flush(PPUContext& ctx, u32 client)
{
    const u32 s = store(ctx);
    if (!cookie_ready(ctx)) return kOk;
    if (const s32 r = lw_lock(ctx, s + kLock); r < 0) return r;
    for (u32 b = 0; b < 16; b++) {
        for (u32 rec = r32(ctx, s + kBuckets + 4 * b); rec;) {
            const u32 next = r32(ctx, rec + dNext);
            if (record_use(ctx, rec) >= 0) {
                for (u32 c = r32(ctx, rec + dCookies); c; c = r32(ctx, c + cNext)) {
                    if (r32(ctx, c + cClient) != client) continue;
                    if (!r8(ctx, c + cSession) && r64(ctx, c + cExpires) != 0) continue;
                    if (r8(ctx, c + cDeleted)) continue;
                    w8(ctx, c + cDeleted, 1);
                    w32(ctx, rec + dCount, r32(ctx, rec + dCount) - 1);
                }
                record_release(ctx, rec, false);
            }
            rec = next;
        }
    }
    lw_unlock(ctx, s + kLock);
    return kOk;
}

namespace {
s32 cellHttpSessionCookieFlush(PPUContext& ctx, u32 client) { return session_cookie_flush(ctx, client); }
} // namespace

void register_cookies(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0x9638F766, cellHttpInitCookie),
        PS_HLE_FUNC(0x61B2BADE, cellHttpEndCookie),
        PS_HLE_FUNC(0x8FC281F7, cellHttpCookieFlush),
        PS_HLE_FUNC(0xAD6A2E5B, cellHttpSessionCookieFlush),
        PS_HLE_FUNC(0x1B5BDCC6, cellHttpAddCookieWithClientId),
        PS_HLE_FUNC(0x8BEDEFA8, cellHttpGetCookie),
        PS_HLE_FUNC(0xC79173FF, cellHttpCookieExport),
        PS_HLE_FUNC(0xF972C733, cellHttpCookieExportWithClientId),
        PS_HLE_FUNC(0x6C1FFE4E, cellHttpCookieImport),
        PS_HLE_FUNC(0x0D846D63, cellHttpCookieImportWithClientId),
    });
}

} // namespace pspocket::hle::http
