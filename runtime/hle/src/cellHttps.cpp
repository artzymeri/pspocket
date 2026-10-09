// cellHttp's HTTPS (D-524): cellHttpsInit/End, a client's SSL record, as libhttp.sprx keeps them. See cellHttp.cpp for
// the method.
//
// libhttp leans on libssl here (certificates, SSL contexts and connections): libssl is not pspocket's own yet (its
// cellSsl has cellSslInit/End and the certificate loader), so what libhttp asks of libssl is pspocket's own model of it,
// as the comments say.

#include "cellHttp.hpp"

#include "pspocket/core/log.hpp"
#include "ssl.hpp"

namespace pspocket::hle::http {

namespace {

// The HTTPS context (libhttp's 64-byte block): its certificate list +0, initialised +4, the lock "_httvfy" +8,
// verification flags +32/+40 (u64), the client certificate +56.
constexpr u32 kCtxSize = 64, kCtxCerts = 0, kCtxInit = 4, kCtxLock = 8;

// A client's SSL record (40 bytes): its SSL context +0, the client +4, the lock "_httssl" +8.
constexpr u32 kRecSize = 40;

// The client's SSL record, as its manager keeps it (the client and the manager checked, as libhttp checks them).
s32 ssl_record_get(PPUContext& ctx, u32 client, u32 out)
{
    if (!valid(ctx, client, client::kSelf)) return err(CELL_HTTP_ERROR_BAD_CLIENT);
    if (const s32 r = lw_lock(ctx, client + client::kLock); r < 0) return r;
    const u32 mgr = r32(ctx, client + client::kManager);
    lw_unlock(ctx, client + client::kLock);
    if (!valid(ctx, mgr, manager::kSelf)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return kOk;
    if (const s32 r = lw_lock(ctx, mgr + manager::kLock); r < 0) return r;
    w32(ctx, out, r32(ctx, mgr + manager::k132));
    lw_unlock(ctx, mgr + manager::kLock);
    return kOk;
}

s32 ssl_record_set(PPUContext& ctx, u32 client, u32 rec)
{
    if (!valid(ctx, client, client::kSelf)) return err(CELL_HTTP_ERROR_BAD_CLIENT);
    if (const s32 r = lw_lock(ctx, client + client::kLock); r < 0) return r;
    const u32 mgr = r32(ctx, client + client::kManager);
    lw_unlock(ctx, client + client::kLock);
    if (!valid(ctx, mgr, manager::kSelf)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + manager::kLock); r < 0) return r;
    w32(ctx, mgr + manager::k132, rec);
    lw_unlock(ctx, mgr + manager::kLock);
    return kOk;
}

} // namespace

// With HTTPS initialised a client gets an SSL record (40 bytes, the lock "_httssl") in its manager.
s32 client_ssl_create(PPUContext& ctx, u32 client)
{
    if (!https_initialized(ctx)) return kOk;
    const u32 rec = hmalloc(ctx, kRecSize);
    if (!rec) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, rec, kRecSize);
    w32(ctx, rec + 4, client);
    if (const s32 r = lw_create(ctx, rec + 8, "_httssl"); r < 0) {
        hfree(ctx, rec);
        return r;
    }
    if (const s32 r = ssl_record_set(ctx, client, rec); r < 0) {
        lw_destroy(ctx, rec + 8);
        hfree(ctx, rec);
        return r;
    }
    return kOk;
}

s32 client_ssl_destroy(PPUContext& ctx, u32 client)
{
    if (!https_initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    StackVar v(ctx, gb(ctx), 8);
    if (const s32 r = ssl_record_get(ctx, client, v.addr()); r < 0) return r;
    if (const u32 rec = r32(ctx, v.addr())) {
        if (r32(ctx, rec)) {
            PS_LOG_ERROR(kChan, "client {:#x}: an SSL context to free (libssl is not pspocket's)", client);
            w32(ctx, rec, 0);
        }
        lw_destroy(ctx, rec + 8);
        hfree(ctx, rec);
    }
    return ssl_record_set(ctx, client, 0);
}

s32 client_ssl_ctx_free(PPUContext& ctx, u32 client)
{
    if (!https_initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    StackVar v(ctx, gb(ctx), 8);
    if (const s32 r = ssl_record_get(ctx, client, v.addr()); r < 0) return r;
    const u32 rec = r32(ctx, v.addr());
    if (!rec || !r32(ctx, rec)) return kOk;
    PS_LOG_ERROR(kChan, "client {:#x}: an SSL context to free (libssl is not pspocket's)", client);
    w32(ctx, rec, 0);
    return kOk;
}

void socket_ssl_close(PPUContext& ctx, u32 sock, u32 ssl_record)
{
    (void)ssl_record;
    if (r32(ctx, sock + 48)) PS_LOG_ERROR(kChan, "socket {:#x}: an SSL connection to close (libssl is not pspocket's)", sock);
    w8(ctx, sock + 81, 0);
    w32(ctx, sock + 48, 0);
    w8(ctx, sock + 84, 0);
    w8(ctx, sock + 85, 0);
}

namespace {

// cellHttpsInit(u32 caCertNum, const CellHttpsData* caList): cellHttpInit and cellSslInit first; the context (64 bytes in
// the heap), its lock "_httvfy", the certificates.
s32 cellHttpsInit(PPUContext& ctx, u32 count, u32 list)
{
    const u32 slot = data(ctx) + kHttps;
    if (https_initialized(ctx)) return err(CELL_HTTP_ERROR_ALREADY_INITIALIZED);
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!ssl_initialized()) return err(CELL_HTTPS_ERROR_NO_SSL);
    const u32 hctx = hmalloc(ctx, kCtxSize);
    w32(ctx, slot, hctx);
    if (!hctx) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, hctx, kCtxSize);
    if (const s32 r = lw_create(ctx, hctx + kCtxLock, "_httvfy"); r < 0) {
        hfree(ctx, hctx);
        w32(ctx, slot, 0);
        return r;
    }
    PS_LOG_INFO(kChan, "cellHttpsInit: {} CA certificates at {:#x}", count, list);
    w8(ctx, hctx + kCtxInit, 1);
    return kOk;
}

s32 cellHttpsEnd(PPUContext& ctx)
{
    const u32 slot = data(ctx) + kHttps;
    if (!https_initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!ssl_initialized()) return err(CELL_HTTPS_ERROR_NO_SSL);
    const u32 hctx = r32(ctx, slot);
    w32(ctx, hctx + kCtxCerts, 0);
    lw_destroy(ctx, hctx + kCtxLock);
    w8(ctx, hctx + kCtxInit, 0);
    hfree(ctx, hctx);
    w32(ctx, slot, 0);
    return kOk;
}

} // namespace

void register_https(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0x522180BC, cellHttpsInit),
        PS_HLE_FUNC(0xE6D4202F, cellHttpsEnd),
    });
}

} // namespace pspocket::hle::http
