// cellHttp's caches (D-524): cellHttpInitCache's (responses kept in libhttp's heap up to a size) and the external one
// (cellHttp_009E4B77: in a buffer of the game's, in 512-byte blocks), their set-up, end, flushes and statistics, and a
// transaction's lookup of its URI, as libhttp.sprx keeps them. See cellHttp.cpp for the method and cellHttp.hpp for the
// layouts.
//
// An entry is made from a response, which an offline console never gets (D-523): the caches stay empty, and every
// walk over their entries finds none. Where libhttp's code would work on an entry, unreachable() logs it.

#include "cellHttp.hpp"

#include <cstring>

#include "pspocket/core/log.hpp"

namespace pspocket::hle::http {

namespace {

u32 internal(PPUContext& ctx) { return data(ctx) + kCache; }
u32 external(PPUContext& ctx) { return data(ctx) + kExtCache; }

// The entry for a URI's text in a cache's 256 buckets (by the first byte of the text's MD5).
u32 table_find(PPUContext& ctx, u32 table, u32 text)
{
    u8 digest[16];
    md5(gb(ctx) + text, str_len(ctx, text), digest);
    if (r32(ctx, table + 4 * u32{digest[0]})) unreachable(ctx, "a cache entry");
    return 0;
}

// cellHttpInitCache(u32 size): the cache's lock "_httcac", its buckets; cellHttpInit isn't needed.
s32 cellHttpInitCache(PPUContext& ctx, u32 size)
{
    const u32 c = internal(ctx);
    if (r8(ctx, c + 24)) return err(CELL_HTTP_ERROR_CACHE_ALREADY_INITIALIZED);
    zero(ctx, c + 48, 1024);
    if (const s32 r = lw_create(ctx, c, "_httcac"); r < 0) return r;
    w32(ctx, c + 40, 0);
    w8(ctx, c + 24, 1);
    w32(ctx, c + 28, size);
    w32(ctx, c + 32, 0);
    w32(ctx, c + 36, 0);
    return kOk;
}

s32 cellHttpEndCache(PPUContext& ctx)
{
    const u32 c = internal(ctx);
    if (!r8(ctx, c + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    if (r32(ctx, c + 36)) unreachable(ctx, "freeing the cache's entries");
    lw_destroy(ctx, c);
    w32(ctx, c + 40, 0);
    w8(ctx, c + 24, 0);
    w32(ctx, c + 28, 0);
    w32(ctx, c + 32, 0);
    w32(ctx, c + 36, 0);
    zero(ctx, c + 48, 1024);
    return kOk;
}

s32 cellHttpFlushCache(PPUContext& ctx)
{
    const u32 c = internal(ctx);
    if (!r8(ctx, c + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    const s32 r = lw_lock(ctx, c);
    if (r < 0) return r;
    if (r32(ctx, c + 36)) unreachable(ctx, "flushing the cache's entries");
    lw_unlock(ctx, c);
    return r;
}

// cellHttpGetCacheInfo(CellHttpCacheInfo* info): {size, bytes used}.
s32 cellHttpGetCacheInfo(PPUContext& ctx, u32 info)
{
    const u32 c = internal(ctx);
    if (!r8(ctx, c + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    const s32 r = lw_lock(ctx, c);
    if (r < 0) return r;
    if (usable(ctx, info, 8, "cellHttpGetCacheInfo: info")) {
        w32(ctx, info, r32(ctx, c + 28));
        w32(ctx, info + 4, r32(ctx, c + 32));
    }
    lw_unlock(ctx, c);
    return r;
}

// Every entry of the cache given to a function of the game's (cellHttp_003F88AE(func, arg)).
s32 cellHttp_003F88AE(PPUContext& ctx, u32 func, u32 arg)
{
    (void)func, (void)arg;
    const u32 c = internal(ctx);
    if (!r8(ctx, c + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    const s32 r = lw_lock(ctx, c);
    if (r < 0) return r;
    if (r32(ctx, c + 36)) unreachable(ctx, "the cache's entries given to a function");
    lw_unlock(ctx, c);
    return r;
}

// The external cache (cellHttp_009E4B77(void* buffer, u32 size)): the buffer zeroed and cut into 512-byte blocks, a
// free list of them (the last one first).
s32 cellHttp_009E4B77(PPUContext& ctx, u32 buffer, u32 size)
{
    const u32 x = external(ctx);
    if (r8(ctx, x + 24)) return err(CELL_HTTP_ERROR_CACHE_ALREADY_INITIALIZED);
    zero(ctx, data(ctx) + kExtTable, 1024);
    if (const s32 r = lw_create(ctx, x, "_httxca"); r < 0) return r;
    w8(ctx, x + 24, 1);
    w32(ctx, x + 28, buffer);
    w32(ctx, x + 32, size);
    for (const u32 f : {48u, 36u, 40u, 44u}) w32(ctx, x + f, 0);
    if (!usable(ctx, buffer, size, "cellHttp_009E4B77: buffer")) return kOk;
    zero(ctx, buffer, size);
    for (u32 at = 0; size > 511 && at + 512 <= size; at += 512) {
        w32(ctx, buffer + at, r32(ctx, x + 40));
        w32(ctx, x + 40, buffer + at);
    }
    return kOk;
}

s32 cellHttpEndExternalCache(PPUContext& ctx)
{
    const u32 x = external(ctx);
    if (!r8(ctx, x + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    lw_destroy(ctx, x);
    for (const u32 f : {48u, 28u, 32u, 36u, 40u, 44u}) w32(ctx, x + f, 0);
    w8(ctx, x + 24, 0);
    zero(ctx, data(ctx) + kExtTable, 1024);
    return kOk;
}

// The external cache's statistics (cellHttp_6C89A986(info)): {size, bytes in blocks used}.
s32 cellHttp_6C89A986(PPUContext& ctx, u32 info)
{
    const u32 x = external(ctx);
    if (!r8(ctx, x + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    const s32 r = lw_lock(ctx, x);
    if (r < 0) return r;
    if (usable(ctx, info, 8, "cellHttp_6C89A986: info")) {
        w32(ctx, info, r32(ctx, x + 32));
        w32(ctx, info + 4, r32(ctx, x + 36) << 9);
    }
    lw_unlock(ctx, x);
    return r;
}

// The external cache flushed (cellHttp_8DC2D51D).
s32 cellHttp_8DC2D51D(PPUContext& ctx)
{
    const u32 x = external(ctx);
    if (!r8(ctx, x + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    const s32 r = lw_lock(ctx, x);
    if (r < 0) return r;
    if (r32(ctx, x + 44)) unreachable(ctx, "flushing the external cache's entries");
    lw_unlock(ctx, x);
    return r;
}

// Every entry of the external cache given to a function of the game's (cellHttp_9BF4365D(func, arg)).
s32 cellHttp_9BF4365D(PPUContext& ctx, u32 func, u32 arg)
{
    (void)func, (void)arg;
    const u32 x = external(ctx);
    if (!r8(ctx, x + 24)) return err(CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED);
    const s32 r = lw_lock(ctx, x);
    if (r < 0) return r;
    if (r32(ctx, x + 44)) unreachable(ctx, "the external cache's entries given to a function");
    lw_unlock(ctx, x);
    return r;
}

} // namespace

s32 cache_find(PPUContext& ctx, u32 t, u32 out)
{
    u32 text = 0, entry = 0;
    s32 r = uri_text(ctx, t + trans::kUri, text);
    if (r >= 0) {
        const bool own = r8(ctx, t + trans::kCache) != 0;
        if (own || r8(ctx, t + trans::kExtCache)) {
            const u32 lock = own ? internal(ctx) : external(ctx);
            r = lw_lock(ctx, lock);
            if (r >= 0) {
                entry = table_find(ctx, own ? internal(ctx) + 48 : data(ctx) + kExtTable, text);
                lw_unlock(ctx, lock);
            }
        }
    }
    if (text) hfree(ctx, text);
    w32(ctx, out, entry);
    return r;
}

void register_caches(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0xB4FA3111, cellHttpInitCache),
        PS_HLE_FUNC(0xA39FE9DC, cellHttpEndCache),
        PS_HLE_FUNC(0x1AC58D11, cellHttpFlushCache),
        PS_HLE_FUNC(0xC6771F43, cellHttpGetCacheInfo),
        PS_HLE_FUNC(0x003F88AE, cellHttp_003F88AE),
        PS_HLE_FUNC(0x009E4B77, cellHttp_009E4B77),
        PS_HLE_FUNC(0xD632A727, cellHttpEndExternalCache),
        PS_HLE_FUNC(0x6C89A986, cellHttp_6C89A986),
        PS_HLE_FUNC(0x8DC2D51D, cellHttp_8DC2D51D),
        PS_HLE_FUNC(0x9BF4365D, cellHttp_9BF4365D),
    });
}

} // namespace pspocket::hle::http
