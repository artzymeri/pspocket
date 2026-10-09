// cellHttp's cookie store (D-524): cellHttpInitCookie/EndCookie, the flushes, as libhttp.sprx keeps cookies. See
// cellHttp.cpp for the method.
//
// The store has a heap of its own ("cellHttpCookie_Library", over cellHttpInitCookie's pool; cellHttpInit isn't
// needed) and 16 buckets of domain records. A domain record: bucket chain +0/+4, the store's list of records +8/+12,
// its cookies +16, a "gone" switch +20, its users +24, its cookies' total size +28, the domain +32.

#include "cellHttp.hpp"

#include "pspocket/core/log.hpp"

namespace pspocket::hle::http {

namespace {

// The store's record (libhttp's globals): sizes +0..+12, the record list +16/+20, buckets +24, the lock +88, limits
// +112/+116, the cookies' total +120, the heap +124, initialised +128.
constexpr u32 kTotalMax = 0, kTotal = 4, kMaxSize = 8, k12 = 12, kFirst = 16, kLast = 20, kBuckets = 24, kLock = 88,
              k112 = 112, k116 = 116, kCount = 120, kHeap = 124, kInit = 128;

u32 store(PPUContext& ctx) { return data(ctx) + kCookie; }
bool cookie_ready(PPUContext& ctx) { return r8(ctx, store(ctx) + kInit) != 0; }

void cfree(PPUContext& ctx, u32 addr)
{
    call(ctx, "sysPrxForUser", "_sys_heap_free", {r32(ctx, store(ctx) + kHeap), addr});
}

// A cookie's strings freed (+16, +20, +8, +0, +12, +4, +24): its size (+40).
u32 cookie_strings_free(PPUContext& ctx, u32 c)
{
    const u32 size = r32(ctx, c + 40);
    for (const u32 f : {16u, 20u, 8u, 0u, 12u, 4u, 24u}) {
        if (const u32 p = r32(ctx, c + f)) {
            cfree(ctx, p);
            w32(ctx, c + f, 0);
        }
    }
    return size;
}

// A domain record freed with every cookie it holds; the store's total lowered by their sizes.
void record_free(PPUContext& ctx, u32 rec)
{
    const u32 s = store(ctx);
    u32 sum = 0;
    for (u32 c = r32(ctx, rec + 16); c;) {
        sum += cookie_strings_free(ctx, c);
        const u32 next = r32(ctx, c + 28);
        cfree(ctx, c);
        c = next;
    }
    w32(ctx, s + kTotal, r32(ctx, s + kTotal) - sum);
    if (const u32 d = r32(ctx, rec + 32)) cfree(ctx, d);
    cfree(ctx, rec);
}

// The part of a domain the store files it under (libhttp's): its last two labels, or three when the last label isn't
// three letters (a country) and the one before is at most two characters ("co.jp"); 0 when the domain has fewer
// labels. Trailing dots are passed over; a domain starting with a dot counts that dot as no label.
u32 domain_base(PPUContext& ctx, u32 s)
{
    if (!s || str_len(ctx, s) == 0) return 0;
    const auto at = [&](u32 a) { return r8(ctx, a); };
    u32 r3 = s + str_len(ctx, s) - 1;
    u32 r10 = 0, r11 = 0, r7 = 0, r8 = 0, r29 = 0;
    bool same = false; // s == r11 at the second label's search
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
        r3 = r11;
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
    bool counted = false; // r29 set by the dot count
    if (same) {
        r8 = 0;
    } else {
        r11 = r3 - 1;
        u32 ctr = r3 - s;
        r8 = 0;
        bool dot = at(r11) == '.';
        if (!dot) {
            for (;;) {
                r11--;
                r8++;
                if (--ctr == 0) break;
                if (at(r11) == '.') {
                    dot = true;
                    break;
                }
            }
        } else {
            r8 = 0;
        }
        if (dot) {
            r29 = 2;
            if (s != r11) {
                for (u32 n = r11 - s; n; n--) {
                    r11--;
                    if (at(r11) == '.') r29++;
                }
            }
            counted = true;
        }
    }
    if (!counted) r29 = 1;
    if (static_cast<s8>(at(s)) != '.') r29++;
    u32 r28 = 3;
    if (r8 > 2) {
        r28 = 2;
    } else if (r10 == 3) {
        for (const char* g : {"com", "edu", "net", "org", "gov", "mil", "int"}) {
            u32 k = 0;
            for (; k < 3; k++) {
                const u8 c = at(r7 + k);
                const u8 lc = c >= 'A' && c <= 'Z' ? c + 32 : c;
                if (lc != static_cast<u8>(g[k]) || !c) break;
            }
            if (k == 3) {
                r28 = 2;
                break;
            }
        }
    }
    if (r29 < r28) return 0;
    if (str_len(ctx, s) == 0) return 0;
    r11 = s + str_len(ctx, s) - 1;
    bool done = r28 == 0;
    if (s != r11 && !done) {
        u32 ctr = r11 - s;
        for (;;) {
            r11--;
            if (at(r11) == '.') done = --r28 == 0;
            if (--ctr == 0) break;
            if (done) return r11 + 1;
        }
    }
    if (done) return r11 + 1;
    if (r28 != 1 || at(s) == '.') return 0;
    return s;
}

// The bucket of a domain: the high nibble of the first byte of the MD5 of its base (as domain_base, else the domain),
// a leading dot left out, a trailing dot's byte left out of the length (libhttp hashes that many bytes from where it
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

// A gone record taken out of the store's list and its bucket, then freed.
void record_unlink_free(PPUContext& ctx, u32 rec)
{
    const u32 s = store(ctx);
    const u32 lprev = r32(ctx, rec + 12), lnext = r32(ctx, rec + 8);
    if (!lprev) w32(ctx, s + kFirst, lnext);
    else w32(ctx, lprev + 8, lnext);
    if (!lnext) w32(ctx, s + kLast, lprev);
    else w32(ctx, lnext + 12, lprev);
    const u32 next = r32(ctx, rec), prev = r32(ctx, rec + 4);
    if (next) w32(ctx, next + 4, prev);
    if (!prev) w32(ctx, s + kBuckets + 4 * domain_bucket(ctx, r32(ctx, rec + 32)), next);
    else w32(ctx, prev, next);
    w32(ctx, rec, 0);
    w32(ctx, rec + 4, 0);
    record_free(ctx, rec);
}

// A record taken for use: CELL_HTTP_ERROR_LOCK_FAILED when it is gone.
s32 record_use(PPUContext& ctx, u32 rec)
{
    if (!rec || r8(ctx, rec + 20)) return err(CELL_HTTP_ERROR_LOCK_FAILED);
    w32(ctx, rec + 24, r32(ctx, rec + 24) + 1);
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
    w32(ctx, rec + 24, r32(ctx, rec + 24) - 1);
    if (r32(ctx, rec + 24) == 0) {
        if (!r8(ctx, rec + 20)) {
            u32 sum = 0, prev = 0;
            for (u32 c = r32(ctx, rec + 16); c;) {
                const u32 next = r32(ctx, c + 28);
                if (r8(ctx, c + 58)) {
                    if (prev) w32(ctx, prev + 28, next);
                    else w32(ctx, rec + 16, next);
                    sum += cookie_strings_free(ctx, c);
                    cfree(ctx, c);
                } else {
                    prev = c;
                }
                c = next;
            }
            w32(ctx, s + kTotal, r32(ctx, s + kTotal) - sum);
            if (!r32(ctx, rec + 16)) w8(ctx, rec + 20, 1);
        }
        if (r8(ctx, rec + 20)) record_unlink_free(ctx, rec);
    }
    if (lock) lw_unlock(ctx, s + kLock);
    return kOk;
}

// cellHttpInitCookie(void* pool, u32 poolSize): the store's heap over the pool, its lock "_httcok", its limits.
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
    w32(ctx, s + k12, 512);
    w32(ctx, s + kMaxSize, 4096);
    w32(ctx, s + kHeap, heap);
    w32(ctx, s + k112, 50);
    w32(ctx, s + k116, 300);
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
            const u32 next = r32(ctx, rec);
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
            const u32 next = r32(ctx, rec);
            if (record_use(ctx, rec) >= 0) {
                w32(ctx, s + kCount, r32(ctx, s + kCount) - r32(ctx, rec + 28));
                w8(ctx, rec + 20, 1);
                record_release(ctx, rec, false);
            }
            rec = next;
        }
    }
    lw_unlock(ctx, s + kLock);
    return kOk;
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
            const u32 next = r32(ctx, rec);
            if (record_use(ctx, rec) >= 0) {
                for (u32 c = r32(ctx, rec + 16); c; c = r32(ctx, c + 28)) {
                    if (r32(ctx, c + 60) != client) continue;
                    if (!r8(ctx, c + 57) && r64(ctx, c + 32) != 0) continue;
                    if (r8(ctx, c + 58)) continue;
                    w8(ctx, c + 58, 1);
                    w32(ctx, rec + 28, r32(ctx, rec + 28) - 1);
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
    });
}

} // namespace pspocket::hle::http
