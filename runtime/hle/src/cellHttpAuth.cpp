// cellHttp's authentication cache (D-524): the credentials libhttp keeps for the servers that asked for them, with
// cellHttpAuthCacheGetEntryMax/SetEntryMax, Flush, Export and Import, as libhttp.sprx keeps them. See cellHttp.cpp for
// the method. The cache is made by cellHttpInit and freed by cellHttpEnd (cellHttp.cpp).
//
// An entry (644 bytes, in libhttp's heap): users +0, its state +4, six strings of the challenge at +8..+28 (pointers
// into its pool) with their lengths at +36..+56, +32, the scheme switch +60 (Basic or Digest), the pool +64, the URI
// (a CellHttpUri) +68 with its pool +96, a digest +100, the user name +116 and password +372 (256 bytes each), the
// previous and next entries +628/+632, a "saved" switch +636. The newest entry is at the head.
//
// The export: 1, the cache's entry maximum and count (two big-endian words), then per saved entry from the oldest:
// 2 and the digest, 3 and the user name, 4 and the password, 5, the scheme switch and the words +32..+56 (big-endian),
// the strings, 6 and the URI as cellHttpUtilBuildUri writes it (with its NUL), and 255.

#include "cellHttp.hpp"

#include "pspocket/core/log.hpp"

namespace pspocket::hle::http {

namespace {

// The cache's record: the lock +0, the entry maximum +24, the count +28, the head +32 and tail +36, initialised +40.
constexpr u32 kMax = 24, kCountF = 28, kHead = 32, kTail = 36, kReady = 40;
// An entry's fields.
constexpr u32 eUsers = 0, eState = 4, eStrings = 8, e32 = 32, eLengths = 36, eScheme = 60, ePool = 64, eUri = 68,
              eUriPool = 96, eDigest = 100, eUser = 116, ePass = 372, ePrev = 628, eNext = 632, eSaved = 636;
constexpr u32 kEntrySize = 644;

u32 cache(PPUContext& ctx) { return data(ctx) + kAuth; }
bool ready(PPUContext& ctx) { return r8(ctx, cache(ctx) + kReady) != 0; }

// An entry nobody uses taken out and freed (the count lowered).
void entry_remove(PPUContext& ctx, u32 e)
{
    const u32 a = cache(ctx);
    if (r32(ctx, e + eUsers) != 0) return;
    const u32 prev = r32(ctx, e + ePrev), next = r32(ctx, e + eNext);
    if (prev) w32(ctx, prev + eNext, next);
    else w32(ctx, a + kHead, next);
    if (next) w32(ctx, next + ePrev, prev);
    else w32(ctx, a + kTail, prev);
    if (const u32 p = r32(ctx, e + eUriPool)) {
        hfree(ctx, p);
        zero(ctx, e + eUri, 28);
        w32(ctx, e + eUriPool, 0);
    }
    if (const u32 p = r32(ctx, e + ePool)) {
        hfree(ctx, p);
        zero(ctx, e + eStrings, 56);
        w32(ctx, e + ePool, 0);
    }
    hfree(ctx, e);
    if (const u32 n = r32(ctx, a + kCountF)) w32(ctx, a + kCountF, n - 1);
}

u32 strings_size(PPUContext& ctx, u32 e)
{
    u32 n = 0;
    for (u32 i = 0; i < 6; i++) n += r32(ctx, e + eLengths + 4 * i);
    return n;
}

// cellHttpAuthCacheGetEntryMax(u32* max) / SetEntryMax(u32 max): any number.
s32 cellHttpAuthCacheGetEntryMax(PPUContext& ctx, u32 out)
{
    const u32 a = cache(ctx);
    if (!ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, a); r < 0) return r;
    if (usable(ctx, out, 4, "cellHttpAuthCacheGetEntryMax")) w32(ctx, out, r32(ctx, a + kMax));
    lw_unlock(ctx, a);
    return kOk;
}

s32 cellHttpAuthCacheSetEntryMax(PPUContext& ctx, u32 max)
{
    const u32 a = cache(ctx);
    if (!ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (const s32 r = lw_lock(ctx, a); r < 0) return r;
    w32(ctx, a + kMax, max);
    lw_unlock(ctx, a);
    return kOk;
}

// cellHttpAuthCacheFlush: every entry nobody uses removed.
s32 cellHttpAuthCacheFlush(PPUContext& ctx)
{
    const u32 a = cache(ctx);
    if (!ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (const s32 r = lw_lock(ctx, a); r < 0) return r;
    for (u32 e = r32(ctx, a + kHead); e;) {
        const u32 next = r32(ctx, e + eNext);
        entry_remove(ctx, e);
        e = next;
    }
    lw_unlock(ctx, a);
    return kOk;
}

// cellHttpAuthCacheExport(void* buf, u32 size, u32* exportSize): the saved entries in libhttp's format; without a buffer
// only *exportSize. CELL_HTTP_ERROR_INSUFFICIENT when one doesn't fit.
s32 cellHttpAuthCacheExport(PPUContext& ctx, u32 buf, u32 size, u32 export_size)
{
    const u32 a = cache(ctx);
    if (!initialized(ctx) || !ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    bool write = true;
    if (!buf) {
        if (!export_size) return err(CELL_HTTP_ERROR_NO_BUFFER);
        write = false;
    }
    if (const s32 r = lw_lock(ctx, a); r < 0) return r;
    StackVar need(ctx, gb(ctx), 8);
    s32 result = kOk;
    u32 used = 9, at = 0; // the size so far; where the next entry is written
    if (write) {
        if (size <= 8) {
            lw_unlock(ctx, a);
            return err(CELL_HTTP_ERROR_INSUFFICIENT);
        }
        if (!usable(ctx, buf, size, "cellHttpAuthCacheExport: buf")) {
            lw_unlock(ctx, a);
            return err(CELL_HTTP_ERROR_NO_BUFFER);
        }
        w8(ctx, buf, 1);
        copy(ctx, buf + 1, a + kMax, 8);
        at = 9;
    }
    for (u32 e = r32(ctx, a + kTail); e; e = r32(ctx, e + ePrev)) {
        if (!r8(ctx, e + eSaved)) continue;
        const u32 head_end = used + 561;
        const u32 strings = strings_size(ctx, e);
        u32 total = 0;
        if (write) {
            if (head_end > size) {
                result = err(CELL_HTTP_ERROR_INSUFFICIENT);
                break;
            }
            const u32 p = buf + at;
            w8(ctx, p, 2);
            copy(ctx, p + 1, e + eDigest, 16);
            w8(ctx, p + 17, 3);
            copy(ctx, p + 18, e + eUser, 256);
            w8(ctx, p + 274, 4);
            copy(ctx, p + 275, e + ePass, 256);
            w8(ctx, p + 531, 5);
            w8(ctx, p + 532, r8(ctx, e + eScheme));
            copy(ctx, p + 533, e + e32, 28);
            if (size < strings + head_end) {
                result = err(CELL_HTTP_ERROR_INSUFFICIENT);
                break;
            }
            copy(ctx, buf + at + 561, r32(ctx, e + ePool), strings);
            total = strings + head_end + 1;
            if (size < total) {
                result = err(CELL_HTTP_ERROR_INSUFFICIENT);
                break;
            }
            w8(ctx, buf + at + 561 + strings, 6);
            at = at + 561 + strings + 1;
        } else {
            total = strings + used + 562;
        }
        w32(ctx, need.addr(), 0);
        s32 r = util(ctx, "cellHttpUtilBuildUri", {e + eUri, 0, 0, need.addr(), 0});
        if (r < 0) {
            result = r;
            break;
        }
        total += r32(ctx, need.addr());
        if (!write) {
            used = total + 1;
            continue;
        }
        if (total > size) {
            result = err(CELL_HTTP_ERROR_INSUFFICIENT);
            break;
        }
        r = util(ctx, "cellHttpUtilBuildUri", {e + eUri, buf + at, size - at, need.addr(), 0});
        if (r < 0) {
            result = r;
            break;
        }
        used = total + 1;
        if (size < used) {
            result = err(CELL_HTTP_ERROR_INSUFFICIENT);
            break;
        }
        w8(ctx, buf + at + r32(ctx, need.addr()), 0xFF);
        at = at + r32(ctx, need.addr()) + 1;
    }
    if (result == kOk && export_size) w32(ctx, export_size, used);
    lw_unlock(ctx, a);
    return result;
}

// cellHttpAuthCacheImport(const void* buf, u32 size): libhttp's export read back, each entry added at the head (saved,
// in state 1) while the cache has room (else CELL_HTTP_ERROR_INSUFFICIENT); a buffer not in the format is
// CELL_HTTP_ERROR_INVALID_DATA.
s32 cellHttpAuthCacheImport(PPUContext& ctx, u32 buf, u32 size)
{
    const u32 a = cache(ctx);
    if (!initialized(ctx) || !ready(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!buf || !size) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, a); r < 0) return r;
    const auto done = [&](s32 code) {
        lw_unlock(ctx, a);
        return code;
    };
    if (size <= 8) return done(err(CELL_HTTP_ERROR_INSUFFICIENT));
    if (!usable(ctx, buf, 9, "cellHttpAuthCacheImport: buf")) return done(err(CELL_HTTP_ERROR_NO_BUFFER));
    if (r8(ctx, buf) != 1) return done(err(CELL_HTTP_ERROR_INVALID_DATA));
    const u32 end = buf + size;
    u32 p = buf + 9;
    if (end <= p) return done(kOk);
    if (r32(ctx, a + kMax) <= r32(ctx, a + kCountF)) return done(err(CELL_HTTP_ERROR_INSUFFICIENT));
    if (size <= 560) return done(err(CELL_HTTP_ERROR_INSUFFICIENT));
    StackVar need(ctx, gb(ctx), 8);
    for (;;) {
        // (libhttp reads an entry's fixed part, even past the buffer's end.)
        if (!usable(ctx, p, 561, "cellHttpAuthCacheImport: an entry")) return done(err(CELL_HTTP_ERROR_NO_BUFFER));
        if (r8(ctx, p) != 2 || r8(ctx, p + 17) != 3 || r8(ctx, p + 274) != 4 || r8(ctx, p + 531) != 5)
            return done(err(CELL_HTTP_ERROR_INVALID_DATA));
        const u32 e = hmalloc(ctx, kEntrySize);
        if (!e) return done(err(CELL_HTTP_ERROR_NO_MEMORY));
        zero(ctx, e, kEntrySize);
        const auto drop = [&](s32 code) {
            if (const u32 q = r32(ctx, e + eUriPool)) {
                hfree(ctx, q);
                w32(ctx, e + eUriPool, 0);
            }
            if (const u32 q = r32(ctx, e + ePool)) {
                hfree(ctx, q);
                w32(ctx, e + ePool, 0);
            }
            hfree(ctx, e);
            return done(code);
        };
        copy(ctx, e + eDigest, p + 1, 16);
        copy(ctx, e + eUser, p + 18, 256);
        copy(ctx, e + ePass, p + 275, 256);
        w8(ctx, e + eScheme, r8(ctx, p + 532) == 1 ? 1 : 0);
        copy(ctx, e + e32, p + 533, 28);
        const u32 strings = strings_size(ctx, e);
        if (!usable(ctx, p + 561, strings + 1, "cellHttpAuthCacheImport: an entry's strings"))
            return drop(err(CELL_HTTP_ERROR_NO_BUFFER));
        const u32 pool = hmalloc(ctx, strings);
        w32(ctx, e + ePool, pool);
        if (!pool) return drop(err(CELL_HTTP_ERROR_NO_MEMORY));
        copy(ctx, pool, p + 561, strings);
        u32 off = 0;
        for (u32 i = 0; i < 6; i++) {
            w32(ctx, e + eStrings + 4 * i, pool + off);
            off += r32(ctx, e + eLengths + 4 * i);
        }
        const u32 q = p + 561 + strings;
        if (r8(ctx, q) != 6) return drop(err(CELL_HTTP_ERROR_INVALID_DATA));
        const u32 text = q + 1;
        const GuestString uri_text = read_cstr(thread_of(ctx), text, 0x10000);
        if (!uri_text.terminated) return drop(err(CELL_HTTP_ERROR_NO_BUFFER));
        const u32 len = static_cast<u32>(uri_text.text.size());
        w32(ctx, need.addr(), 0);
        s32 r = util(ctx, "cellHttpUtilParseUri", {e + eUri, text, 0, 0, need.addr()});
        if (r < 0) return drop(r);
        const u32 upool = hmalloc(ctx, r32(ctx, need.addr()));
        w32(ctx, e + eUriPool, upool);
        if (!upool) return drop(err(CELL_HTTP_ERROR_NO_MEMORY));
        r = util(ctx, "cellHttpUtilParseUri", {e + eUri, text, upool, r32(ctx, need.addr()), 0});
        if (r < 0) return drop(r);
        const u32 tail = text + len + 1;
        if (!usable(ctx, tail, 1, "cellHttpAuthCacheImport: an entry's end")) return drop(err(CELL_HTTP_ERROR_NO_BUFFER));
        if (r8(ctx, tail) != 0xFF) return drop(err(CELL_HTTP_ERROR_INVALID_DATA));
        w32(ctx, e + ePrev, 0);
        const u32 head = r32(ctx, a + kHead);
        w8(ctx, e + eSaved, 1);
        w32(ctx, e + eState, 1);
        w32(ctx, e + eNext, head);
        if (head) w32(ctx, head + ePrev, e);
        else w32(ctx, a + kTail, e);
        w32(ctx, a + kHead, e);
        w32(ctx, a + kCountF, r32(ctx, a + kCountF) + 1);
        p = tail + 1;
        if (end <= p) return done(kOk);
        if (r32(ctx, a + kMax) <= r32(ctx, a + kCountF)) return done(err(CELL_HTTP_ERROR_INSUFFICIENT));
    }
}

} // namespace

void register_auth(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0xF92217C5, cellHttpAuthCacheGetEntryMax),
        PS_HLE_FUNC(0xD33AD4D0, cellHttpAuthCacheSetEntryMax),
        PS_HLE_FUNC(0xDCDF003A, cellHttpAuthCacheFlush),
        PS_HLE_FUNC(0x33347DDD, cellHttpAuthCacheExport),
        PS_HLE_FUNC(0xDA56BFAA, cellHttpAuthCacheImport),
    });
}

} // namespace pspocket::hle::http
