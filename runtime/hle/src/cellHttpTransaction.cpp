// cellHttp's transactions (D-524): cellHttpCreateTransaction/DestroyTransaction, a transaction's URI and request line,
// its headers (the game's and the ones libhttp adds), content length and chunked switch, the response's getters, the
// SSL getters, closing, aborting and releasing its connection, sending a request and receiving the response, as
// libhttp.sprx does them. See cellHttp.cpp for the method and cellHttp.hpp for the transaction's layout.
//
// The console is offline: pspocket's lv2 network has only loopback and libnet's resolver functions are not pspocket's
// (D-523), so a request ends where libhttp makes its connection's resolver (CELL_HTTP_ERROR_NET_RESOLVER and h_errno,
// as on a console whose resolver fails) and a transaction stays in state 0: what the response getters, the connection
// calls and cellHttpRecvResponse answer there is libhttp's. libhttp's code past a made connection (sending, receiving,
// redirections, authentication, the response) can't be reached; where it would run, unreachable() logs it and the
// call ends with CELL_HTTP_ERROR_INTERNAL.
//
// Every function checks its transaction as libhttp does: cellHttpInit first (CELL_HTTP_ERROR_NOT_INITIALIZED), then
// sys_memory_get_page_attribute on the address and the transaction's pointer to itself (CELL_HTTP_ERROR_BAD_TRANS),
// then its other arguments; then it holds the transaction's lock "_htttrn".

#include "cellHttp.hpp"

#include "pspocket/core/log.hpp"

namespace pspocket::hle::http {

namespace t = trans;
namespace c = client;

namespace {

// ------------------------------------------------------------------------------------------------- checks and the client

s32 check_trans(PPUContext& ctx, u32 tr)
{
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!valid(ctx, tr, t::kSelf)) return err(CELL_HTTP_ERROR_BAD_TRANS);
    return kOk;
}

s32 lock(PPUContext& ctx, u32 tr) { return lw_lock(ctx, tr + t::kLock); }
void unlock(PPUContext& ctx, u32 tr) { lw_unlock(ctx, tr + t::kLock); }

s32 check_client(PPUContext& ctx, u32 client)
{
    return valid(ctx, client, c::kSelf) ? kOk : err(CELL_HTTP_ERROR_BAD_CLIENT);
}

// The client's transaction lock "_httcla" (+160), taken or given back after the client is checked.
s32 client_trans_lock(PPUContext& ctx, u32 client, bool take)
{
    if (const s32 r = check_client(ctx, client); r < 0) return r;
    return take ? lw_lock(ctx, client + c::kTransLock) : lw_unlock(ctx, client + c::kTransLock);
}

// The client's count of transactions (+36).
s32 client_count(PPUContext& ctx, u32 client, s32 delta)
{
    page_ok(ctx, client); // libhttp asks lv2 and goes on whatever it answers
    const s32 r = lw_lock(ctx, client + c::kLock);
    if (r < 0) return r;
    w32(ctx, client + c::kTransactions, r32(ctx, client + c::kTransactions) + static_cast<u32>(delta));
    lw_unlock(ctx, client + c::kLock);
    return r;
}

// The client's transaction state callback, called (transId, state, arg) when one is set.
s32 state_callback(PPUContext& ctx, u32 client, u32 tr, s32 state)
{
    if (const s32 r = check_client(ctx, client); r < 0) return r;
    if (const s32 r = lw_lock(ctx, client + c::kLock); r < 0) return r;
    const u32 cb = r32(ctx, client + c::kTransStateCb), arg = r32(ctx, client + c::kTransStateCb + 4);
    lw_unlock(ctx, client + c::kLock);
    if (cb) call_guest(ctx, cb, {tr, static_cast<u64>(static_cast<s64>(state)), arg});
    return kOk;
}

// What a new transaction takes from its client: the SSL version, the user agent and headers (copies), the settings.
s32 client_settings(PPUContext& ctx, u32 client, u32 tr)
{
    page_ok(ctx, client);
    if (const s32 r = client_ssl_version(ctx, client, tr + t::kSslVersion); r < 0) {
        lw_unlock(ctx, client + c::kLock); // libhttp's: a lock it doesn't hold
        return r;
    }
    if (const s32 r = lw_lock(ctx, client + c::kLock); r < 0) return r;
    const u32 ua_size = r32(ctx, client + c::kUaSize);
    const u32 ua = hmalloc(ctx, ua_size);
    w32(ctx, tr + t::kUa, ua);
    if (!ua) {
        lw_unlock(ctx, client + c::kLock);
        return err(CELL_HTTP_ERROR_NO_MEMORY);
    }
    copy(ctx, ua, r32(ctx, client + c::kUa), ua_size - 1);
    w8(ctx, ua + ua_size - 1, 0);
    w32(ctx, tr + t::kUaSize, ua_size);
    for (u32 node = r32(ctx, client + c::kHeaders); node; node = r32(ctx, node + hnode::kNext)) {
        if (const s32 r = headers_add(ctx, tr + t::kHeaders, node + hnode::kHeader); r < 0) {
            headers_free(ctx, r32(ctx, tr + t::kHeaders));
            w32(ctx, tr + t::kHeaders, 0);
            hfree(ctx, r32(ctx, tr + t::kUa));
            w32(ctx, tr + t::kUa, 0);
            lw_unlock(ctx, client + c::kLock);
            return r;
        }
    }
    static constexpr std::pair<u32, u32> kBytes[] = {{t::kKeepAlive, c::kKeepAlive}, {t::kAutoRedirect, c::kAutoRedirect},
        {t::kAutoAuth, c::kAutoAuth}, {t::kPipeline, c::kPipeline}, {t::kCookie, c::kCookie}, {t::k142, c::k214},
        {t::k143, c::k215}, {t::kAuthCache, c::kAuthCache}, {t::kCache, c::kCache}, {t::kExtCache, c::k218},
        {t::kDiskCache, c::kDiskCache}};
    for (const auto& [to, from] : kBytes) w8(ctx, tr + to, r8(ctx, client + from));
    static constexpr std::pair<u32, u32> kWords[] = {{t::kHeaderCount, c::kHeaderCount},
        {t::kResponseBufferMax, c::kResponseBufferMax}, {t::kMajor, c::kMajor}, {t::kMinor, c::kMinor},
        {t::k112, c::k32}, {t::k132, c::k156}, {t::k156, c::k148}};
    for (const auto& [to, from] : kWords) w32(ctx, tr + to, r32(ctx, client + from));
    w64(ctx, tr + t::k160, r64(ctx, client + c::k136));
    lw_unlock(ctx, client + c::kLock);
    return kOk;
}

// A transaction's state for a change to its request: one being built (0, 1, 6) can change.
s32 request_open(PPUContext& ctx, u32 tr)
{
    switch (static_cast<s32>(r32(ctx, tr + t::kState))) {
    case -2: return err(CELL_HTTP_ERROR_BROKEN_PIPELINE);
    case -1: return err(CELL_HTTP_ERROR_TRANS_ABORTED);
    case 0: case 1: case 6: return kOk;
    case 2: return err(CELL_HTTP_ERROR_ALREADY_BUILT);
    case 3: case 4: case 5: case 7: return err(CELL_HTTP_ERROR_ALREADY_SENT);
    default: return err(CELL_HTTP_ERROR_UNKNOWN);
    }
}

// A transaction's state for reading its response: one that got a response (4, 5, 7) can; before, no request was sent.
s32 response_ready(PPUContext& ctx, u32 tr)
{
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = err(CELL_HTTP_ERROR_NO_REQUEST_SENT);
    switch (static_cast<s32>(r32(ctx, tr + t::kState))) {
    case -2: r = err(CELL_HTTP_ERROR_BROKEN_PIPELINE); break;
    case -1: r = err(CELL_HTTP_ERROR_TRANS_ABORTED); break;
    case 4: case 5: case 7: r = unreachable(ctx, "a transaction with a response"); break;
    default: break;
    }
    unlock(ctx, tr);
    return r;
}

// The transaction's cache entries given back (cellHttpInitCache's and the disk cache's): offline nothing makes one
// without the caches.
void cache_entries_free(PPUContext& ctx, u32 tr)
{
    if (r32(ctx, tr + t::kCacheEntry) || r32(ctx, tr + t::k472) || r32(ctx, tr + t::k476) ||
        r32(ctx, tr + t::kDiskEntry) || r32(ctx, tr + t::kDiskEntry + 4))
        unreachable(ctx, "a transaction's cache entries");
}

// ------------------------------------------------------------------------------------------------- the URI as a string

// The digits of a number (libhttp's: at least one).
u32 digits(u32 n)
{
    u32 k = 0;
    do {
        k++;
        n /= 10;
    } while (n);
    return k;
}

// A CellHttpUri as text in a block of libhttp's heap: the scheme (http or https by the port when there is none),
// "://", the host, ":port" unless it is the scheme's own, the path.
s32 uri_text(PPUContext& ctx, u32 uri, u32& out)
{
    out = 0;
    const u32 host = r32(ctx, uri + 4);
    if (!host) return err(CELL_HTTP_ERROR_INVALID_URI);
    const u32 scheme = r32(ctx, uri + 0), path = r32(ctx, uri + 16), port = r32(ctx, uri + 20);
    const u32 hl = str_len(ctx, host), sl = scheme ? str_len(ctx, scheme) : 0, pl = path ? str_len(ctx, path) : 0;
    u32 implied = 0, port_digits = 0;
    if (sl == 0) {
        implied = port == 443 ? 5 : 4;
        if (port != 443 && port != 80) port_digits = digits(port);
    } else if (str_is(ctx, scheme, "HTTPS")) {
        if (port != 0 && port != 443) port_digits = digits(port);
    } else if (str_is(ctx, scheme, "HTTP")) {
        if (port != 0 && port != 80) port_digits = digits(port);
    } else {
        port_digits = digits(port);
    }
    const u32 len = implied + hl + sl + pl + 4 + (port_digits ? port_digits + 1 : 0);
    const u32 buf = hmalloc(ctx, len);
    if (!buf) return err(CELL_HTTP_ERROR_NO_MEMORY);
    u32 at = 0;
    const auto put = [&](std::string_view s) {
        std::memcpy(gb(ctx) + buf + at, s.data(), s.size());
        at += static_cast<u32>(s.size());
    };
    if (sl) {
        copy(ctx, buf, scheme, sl);
        at = sl;
    } else {
        put(port == 443 ? "https" : "http");
    }
    put("://");
    copy(ctx, buf + at, host, hl);
    at += hl;
    if (port_digits) {
        put(":");
        const std::string d = std::to_string(port);
        put(d);
    }
    if (pl) copy(ctx, buf + at, path, pl);
    at += pl;
    w8(ctx, buf + at, 0);
    out = buf;
    return kOk;
}

// The redirection record for the transaction's URI and method (libhttp keeps those of permanent redirections; offline
// nothing makes one).
s32 redirect_find(PPUContext& ctx, u32 tr, u32 method, u32& rec)
{
    rec = 0;
    u32 text = 0;
    s32 r = uri_text(ctx, tr + t::kUri, text);
    if (r >= 0) {
        const u32 lw = data(ctx) + kRec;
        r = lw_lock(ctx, lw);
        if (r >= 0) {
            rec = records_find(ctx, text, method);
            lw_unlock(ctx, lw);
        }
    }
    if (text) hfree(ctx, text);
    return r;
}

// The disk cache's count of requests (cellHttp's statistics), when there is a disk cache.
void disk_cache_count(PPUContext& ctx)
{
    const u32 d = data(ctx) + kDiskCache;
    if (!r8(ctx, d + 16)) return;
    const u32 obj = r32(ctx, d);
    if (lw_lock(ctx, obj) < 0) return;
    w64(ctx, obj + 17528, r64(ctx, obj + 17528) + 1);
    lw_unlock(ctx, obj);
}

// ------------------------------------------------------------------------------------------------- the headers libhttp adds

// The headers a request gets from libhttp (each unless the game set one of the name): Host, User-Agent, Connection or
// Proxy-Connection, Content-Length, Transfer-Encoding, Authorization, Proxy-Authorization, Cookie, Accept-Encoding,
// If-Modified-Since (with a cache), as a header list at *list (their count at *count).
s32 auto_headers(PPUContext& ctx, u32 tr, u32 list, u32 count)
{
    const u32 d = data(ctx);
    StackVar v(ctx, gb(ctx), 48); // the header {name, value}, a number's text (16 bytes), the found header
    const u32 hdr = v.addr(), number = v.addr() + 16, found = v.addr() + 40;
    const bool tunnel = r8(ctx, tr + t::kTunnel) != 0;
    u32 added = 0;
    s32 r = kOk;
    for (u32 i = 0; i <= 9; i++) {
        u32 name = 0, value = 0, block = 0; // a block of the heap the value is in, freed once added
        switch (i) {
        case 0: { // Host: the host, with the port unless it is the scheme's
            name = d + kStrHost;
            const u32 scheme = r32(ctx, tr + t::kUriScheme), port = r32(ctx, tr + t::kUriPort);
            bool own_port;
            if ((!scheme || !r8(ctx, scheme)) && port == 80) own_port = true;
            else if (!scheme) own_port = false; // libhttp would read the scheme at 0
            else own_port = (str_is(ctx, scheme, "HTTPS") && port == 443) || (str_is(ctx, scheme, "HTTP") && port == 80);
            const u32 host = r32(ctx, tr + t::kUriHost);
            if (own_port) {
                value = host;
                break;
            }
            const std::string p = std::to_string(port);
            const u32 hl = str_len(ctx, host);
            block = hmalloc(ctx, hl + 2 + static_cast<u32>(p.size()));
            if (!block) return err(CELL_HTTP_ERROR_NO_MEMORY);
            copy(ctx, block, host, hl);
            w8(ctx, block + hl, ':');
            std::memcpy(gb(ctx) + block + hl + 1, p.data(), p.size());
            w8(ctx, block + hl + 1 + static_cast<u32>(p.size()), 0);
            value = block;
            break;
        }
        case 1:
            if (tunnel) continue;
            name = d + kStrUserAgent;
            value = r32(ctx, tr + t::kUa);
            break;
        case 2:
            name = r8(ctx, tr + t::kProxied) ? d + kStrProxyConnection : d + kStrConnection;
            value = r8(ctx, tr + t::kKeepAlive) ? d + kStrKeepAlive : d + kStrClose;
            break;
        case 3: {
            if (tunnel || !r8(ctx, tr + t::kHasLength)) continue;
            zero(ctx, number, 16);
            const std::string n = std::to_string(r32(ctx, tr + t::kContentLength + 4));
            std::memcpy(gb(ctx) + number, n.data(), n.size());
            name = d + kStrContentLength;
            value = number;
            break;
        }
        case 4:
            if (tunnel || !r8(ctx, tr + t::kChunked)) continue;
            name = d + kStrTransferEncoding;
            value = d + kStrChunked;
            break;
        case 5:
            if (tunnel || !r32(ctx, tr + t::kAuthorization)) continue;
            name = d + kStrAuthorization;
            value = r32(ctx, tr + t::kAuthorization);
            break;
        case 6:
            if ((r8(ctx, tr + t::kHttps) && !tunnel) || !r32(ctx, tr + t::kProxyAuthorization)) continue;
            name = d + kStrProxyAuthorization;
            value = r32(ctx, tr + t::kProxyAuthorization);
            break;
        case 7: {
            if (tunnel || !r8(ctx, tr + t::kCookie)) continue;
            r = cookie_request_value(ctx, r32(ctx, tr + t::kUriHost), r32(ctx, tr + t::kUriPath),
                r32(ctx, tr + t::kUriScheme), block);
            if (r < 0) return r;
            if (!block) continue;
            name = d + kStrCookie;
            value = block;
            break;
        }
        case 8:
            if (tunnel) continue;
            name = d + kStrAcceptEncoding;
            value = r32(ctx, tr + t::k132) & 1 ? d + kStrGzip : d + kStrIdentity;
            break;
        case 9:
            if (tunnel) break;
            if (r32(ctx, tr + t::kCacheEntry) || r32(ctx, tr + t::kDiskEntry) || r32(ctx, tr + t::kDiskEntry + 4))
                return unreachable(ctx, "If-Modified-Since from a cache entry");
            break;
        }
        if (!name) continue;
        w32(ctx, hdr, name);
        w32(ctx, hdr + 4, value);
        w32(ctx, found, 0);
        if (headers_find(ctx, r32(ctx, tr + t::kHeaders), found, name) == kOk) {
            if (block) hfree(ctx, block);
            continue;
        }
        r = headers_add(ctx, list, hdr);
        if (block) hfree(ctx, block);
        if (r < 0) return r;
        added++;
    }
    if (count) w32(ctx, count, added);
    return r;
}

// ------------------------------------------------------------------------------------------------- creating, destroying

// cellHttpCreateTransaction(CellHttpTransId* transId, clientId, const char* method, const CellHttpUri* uri).
s32 cellHttpCreateTransaction(PPUContext& ctx, u32 out, u32 client, u32 method, u32 uri)
{
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = check_client(ctx, client); r < 0) return r;
    if (!uri) return err(CELL_HTTP_ERROR_INVALID_URI);
    if (!usable(ctx, uri, 28, "cellHttpCreateTransaction: uri")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 host = r32(ctx, uri + 4);
    if (!host || !r8(ctx, host)) return err(CELL_HTTP_ERROR_INVALID_URI);
    u8 https = 0;
    if (const u32 scheme = r32(ctx, uri); scheme && str_is(ctx, scheme, "https")) {
        if (!https_initialized(ctx)) return err(CELL_HTTP_ERROR_INVALID_URI);
        https = 1;
    }
    const u32 tr = hmalloc(ctx, t::kSize);
    if (!tr) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, tr, t::kSize);
    const auto fail = [&](s32 r) {
        cache_entries_free(ctx, tr);
        if (const u32 p = r32(ctx, tr + t::kUriPool)) {
            hfree(ctx, p);
            zero(ctx, tr + t::kUri, 28);
            w32(ctx, tr + t::kUriPool, 0);
        }
        for (const u32 f : {t::kUa, t::kMethodBlock, t::kProtocolBlock, t::k256}) {
            if (const u32 p = r32(ctx, tr + f)) hfree(ctx, p);
            w32(ctx, tr + f, 0);
        }
        hfree(ctx, tr);
        return r;
    };
    const u32 len = method ? str_len(ctx, method) : 0;
    if (!len) return fail(err(CELL_HTTP_ERROR_BAD_METHOD));
    const u32 protocol = hmalloc(ctx, 5);
    w32(ctx, tr + t::kProtocolBlock, protocol);
    if (!protocol) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
    std::memcpy(gb(ctx) + protocol, "HTTP", 5);
    const u32 m = hmalloc(ctx, len + 1);
    w32(ctx, tr + t::kMethodBlock, m);
    if (!m) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
    copy(ctx, m, method, len);
    w8(ctx, m + len, 0);
    w32(ctx, tr + t::kProtocol, protocol);
    w32(ctx, tr + t::k264, 1500);
    w32(ctx, tr + t::kMethod, m);
    for (const std::string_view idempotent : {"GET", "HEAD", "PUT", "DELETE", "TRACE", "OPTIONS"}) {
        if (str_is_exact(ctx, m, idempotent)) {
            w8(ctx, tr + t::kIdempotent, 1);
            break;
        }
    }
    w8(ctx, tr + t::kHttps, https);
    if (const s32 r = client_settings(ctx, client, tr); r < 0) return fail(r);
    StackVar need(ctx, gb(ctx), 8);
    w32(ctx, need.addr(), 0);
    if (const s32 r = util(ctx, "cellHttpUtilCopyUri", {0, uri, 0, 0, need.addr()}); r < 0) return fail(r);
    const u32 pool = hmalloc(ctx, r32(ctx, need.addr()));
    w32(ctx, tr + t::kUriPool, pool);
    if (!pool) return fail(err(CELL_HTTP_ERROR_NO_MEMORY));
    if (const s32 r = util(ctx, "cellHttpUtilCopyUri", {tr + t::kUri, uri, pool, r32(ctx, need.addr()), 0}); r < 0)
        return fail(r);
    if (r8(ctx, tr + t::kCache) || r8(ctx, tr + t::kExtCache))
        return fail(unreachable(ctx, "a transaction's entry in cellHttpInitCache's cache"));
    w32(ctx, tr + t::kDiskEntry, 0);
    w32(ctx, tr + t::kDiskEntry + 4, 0);
    if (r8(ctx, tr + t::kDiskCache)) return fail(unreachable(ctx, "a transaction's entry in the disk cache"));
    if (const s32 r = lw_create(ctx, tr + t::kLock, "_htttrn"); r < 0) return fail(r);
    if (const s32 r = lw_create(ctx, tr + t::kAbortLock, "_htttna"); r < 0) {
        lw_destroy(ctx, tr + t::kLock);
        return fail(r);
    }
    w32(ctx, tr + t::kSelf, tr);
    w8(ctx, tr + t::kAborted, 0);
    w32(ctx, tr + t::kClient, client);
    w32(ctx, out, tr);
    client_count(ctx, client, 1);
    return kOk;
}

// cellHttpDestroyTransaction(transId): under the client's transaction lock, every block it holds freed, its locks
// destroyed, the client's count down; a connection it holds goes back to the manager (libhttp's, not reachable).
s32 cellHttpDestroyTransaction(PPUContext& ctx, u32 tr)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    const u32 client = r32(ctx, tr + t::kClient);
    if (const s32 r = client_trans_lock(ctx, client, true); r < 0) return r;
    if (!valid(ctx, tr, t::kSelf)) return err(CELL_HTTP_ERROR_BAD_TRANS); // libhttp's: the client's lock kept
    const u32 conn = r32(ctx, tr + t::kConn);
    w32(ctx, tr + t::kConn, 0);
    w32(ctx, tr + t::kClient, 0);
    if (r8(ctx, tr + t::k366) || r32(ctx, tr + 332) || r8(ctx, tr + t::k367))
        unreachable(ctx, "destroying a transaction with a response");
    if (r32(ctx, tr + t::kAuth) || r32(ctx, tr + t::kProxyAuth))
        unreachable(ctx, "destroying a transaction with authentication cache entries");
    for (const u32 f : {t::kUser, t::kPass}) {
        if (const u32 p = r32(ctx, tr + f)) {
            hfree(ctx, p);
            w32(ctx, tr + f, 0);
        }
    }
    if (const u32 p = r32(ctx, tr + t::kUriPool)) {
        hfree(ctx, p);
        zero(ctx, tr + t::kUri, 28);
        w32(ctx, tr + t::kUriPool, 0);
    }
    if (r32(ctx, tr + t::kMethod)) {
        hfree(ctx, r32(ctx, tr + t::kMethodBlock));
        w32(ctx, tr + t::kMethod, 0);
        w32(ctx, tr + t::kMethodBlock, 0);
    }
    if (r32(ctx, tr + t::kPath) || r32(ctx, tr + t::kPathBlock)) {
        hfree(ctx, r32(ctx, tr + t::kPathBlock));
        w32(ctx, tr + t::kPath, 0);
        w32(ctx, tr + t::kPathBlock, 0);
    }
    if (r32(ctx, tr + t::kProtocol)) {
        hfree(ctx, r32(ctx, tr + t::kProtocolBlock));
        w32(ctx, tr + t::kProtocol, 0);
        w32(ctx, tr + t::kProtocolBlock, 0);
    }
    if (const u32 p = r32(ctx, tr + t::kUa)) hfree(ctx, p);
    w32(ctx, tr + t::kUa, 0);
    headers_free(ctx, r32(ctx, tr + t::kHeaders));
    for (const u32 f : {t::k124, t::kAuthorization, t::kProxyAuthorization}) {
        if (const u32 p = r32(ctx, tr + f)) hfree(ctx, p);
        w32(ctx, tr + f, 0);
    }
    if ((r32(ctx, tr + t::k132) & 1) && r8(ctx, tr + t::k370)) unreachable(ctx, "a transaction's inflate stream");
    if (const u32 p = r32(ctx, tr + t::k256)) hfree(ctx, p);
    w32(ctx, tr + t::k256, 0);
    if (const u32 p = r32(ctx, tr + t::kStatusPool)) {
        hfree(ctx, p);
        zero(ctx, tr + t::kStatusLine, 24);
        w32(ctx, tr + t::kStatusPool, 0);
    }
    headers_free(ctx, r32(ctx, tr + t::kResponseHeaders));
    cache_entries_free(ctx, tr);
    if (conn) unreachable(ctx, "a destroyed transaction's connection");
    lw_destroy(ctx, tr + t::kAbortLock);
    const s32 r = lw_destroy(ctx, tr + t::kLock);
    w32(ctx, tr + t::kSelf, 0);
    client_count(ctx, client, -1);
    hfree(ctx, tr);
    client_trans_lock(ctx, client, false);
    return r;
}

// ------------------------------------------------------------------------------------------------- URI, method, credentials

s32 cellHttpTransactionGetUri(PPUContext& ctx, u32 tr, u32 uri, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    const s32 r = util(ctx, "cellHttpUtilCopyUri", {uri, tr + t::kUri, pool, size, required});
    unlock(ctx, tr);
    return r;
}

// cellHttpTransactionSetUri(transId, const CellHttpUri* uri): before the request is sent (CELL_HTTP_ERROR_ALREADY_SENT).
s32 cellHttpTransactionSetUri(PPUContext& ctx, u32 tr, u32 uri)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!uri) return err(CELL_HTTP_ERROR_INVALID_URI);
    if (!usable(ctx, uri, 28, "cellHttpTransactionSetUri: uri")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 host = r32(ctx, uri + 4);
    if (!host || !r8(ctx, host)) return err(CELL_HTTP_ERROR_INVALID_URI);
    u8 https = 0;
    if (const u32 scheme = r32(ctx, uri); scheme && str_is(ctx, scheme, "https")) {
        if (!https_initialized(ctx)) return err(CELL_HTTP_ERROR_INVALID_URI);
        https = 1;
    }
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = err(CELL_HTTP_ERROR_ALREADY_SENT);
    if (r32(ctx, tr + t::kState) == 0) {
        StackVar v(ctx, gb(ctx), 48); // the size, the copy (a CellHttpUri) at +16
        w32(ctx, v.addr(), 0);
        r = util(ctx, "cellHttpUtilCopyUri", {0, uri, 0, 0, v.addr()});
        if (r >= 0) {
            const u32 pool = hmalloc(ctx, r32(ctx, v.addr()));
            r = err(CELL_HTTP_ERROR_NO_MEMORY);
            if (pool) {
                r = util(ctx, "cellHttpUtilCopyUri", {v.addr() + 16, uri, pool, r32(ctx, v.addr()), 0});
                if (r >= 0) { // a copy that fails leaves its block (libhttp's)
                    hfree(ctx, r32(ctx, tr + t::kUriPool));
                    w32(ctx, tr + t::kUriPool, pool);
                    copy(ctx, tr + t::kUri, v.addr() + 16, 28);
                    w8(ctx, tr + t::kHttps, https);
                }
            }
        }
    }
    unlock(ctx, tr);
    return r;
}

// The request's method (cellHttp_DAC62AB6(transId, char* buf, u32 size, u32* required)): its size with the NUL.
s32 cellHttp_DAC62AB6(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!buf && !required) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    const u32 m = r32(ctx, tr + t::kMethod);
    const u32 len = m ? str_len(ctx, m) : 0;
    s32 r = kOk;
    if (required) w32(ctx, required, len + 1);
    if (buf) {
        if (len + 1 > size) {
            r = err(CELL_HTTP_ERROR_INSUFFICIENT);
        } else {
            copy(ctx, buf, m, len);
            w8(ctx, buf + len, 0);
        }
    }
    unlock(ctx, tr);
    return r;
}

// The credentials the transaction would authenticate with (cellHttp_15C1FD80(transId, char* user, char* password)):
// its authentication cache entry's (256 bytes each), else the ones given, else the URI's
// (CELL_HTTP_ERROR_CANNOT_AUTHENTICATE when there are none).
s32 cellHttp_15C1FD80(PPUContext& ctx, u32 tr, u32 user, u32 pass)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!user || !pass) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = kOk;
    if (r32(ctx, tr + t::kAuth)) {
        r = unreachable(ctx, "a transaction's authentication cache entry");
    } else {
        u32 u = r32(ctx, tr + t::kUser), p = r32(ctx, tr + t::kPass);
        if (!u) {
            u = r32(ctx, tr + t::kUriUser);
            p = r32(ctx, tr + t::kUriPass);
            if (!u || !r8(ctx, u)) u = 0;
        }
        if (!u) {
            r = err(CELL_HTTP_ERROR_CANNOT_AUTHENTICATE);
        } else {
            const u32 ul = str_len(ctx, u);
            copy(ctx, user, u, ul);
            w8(ctx, user + ul, 0);
            if (p) {
                const u32 pl = str_len(ctx, p);
                copy(ctx, pass, p, pl);
                w8(ctx, pass + pl, 0);
            } else {
                w8(ctx, pass, 0);
            }
        }
    }
    unlock(ctx, tr);
    return r;
}

// ------------------------------------------------------------------------------------------------- the request's headers

// cellHttpRequestSetHeader(transId, const CellHttpHeader* header): the game's header of that name replaced (its pool
// made anew), else added. cellHttp_313D80D5 is the same without the transaction's lock.
s32 set_header(PPUContext& ctx, u32 tr, u32 header, bool locked)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!header) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (locked) {
        if (const s32 r = lock(ctx, tr); r < 0) return r;
    }
    s32 r = request_open(ctx, tr);
    if (r >= 0) {
        if (!usable(ctx, header, 8, "cellHttpRequestSetHeader: header")) {
            r = err(CELL_HTTP_ERROR_NO_BUFFER);
        } else {
            const u32 name = r32(ctx, header);
            const u32 n = str_len(ctx, name) + 1;
            u32 node = r32(ctx, tr + t::kHeaders);
            while (node && str_ncasecmp(ctx, r32(ctx, node + hnode::kName), name, n) != 0)
                node = r32(ctx, node + hnode::kNext);
            if (!node) {
                r = headers_add(ctx, tr + t::kHeaders, header);
                if (r == 0) w32(ctx, tr + t::kHeaderCount, r32(ctx, tr + t::kHeaderCount) + 1);
            } else {
                StackVar need(ctx, gb(ctx), 8);
                w32(ctx, need.addr(), 0);
                r = util(ctx, "cellHttpUtilCopyHeader", {0, header, 0, 0, need.addr()});
                if (r >= 0) {
                    const u32 old_pool = r32(ctx, node + hnode::kPool), old_name = r32(ctx, node + hnode::kName),
                              old_value = r32(ctx, node + hnode::kValue);
                    const u32 pool = hmalloc(ctx, r32(ctx, need.addr()));
                    w32(ctx, node + hnode::kPool, pool);
                    if (!pool) {
                        w32(ctx, node + hnode::kPool, old_pool);
                        r = err(CELL_HTTP_ERROR_NO_MEMORY);
                    } else {
                        r = util(ctx, "cellHttpUtilCopyHeader",
                            {node + hnode::kHeader, header, pool, r32(ctx, need.addr()), 0});
                        if (r < 0) {
                            hfree(ctx, r32(ctx, node + hnode::kPool));
                            w32(ctx, node + hnode::kValue, old_value);
                            w32(ctx, node + hnode::kPool, old_pool);
                            w32(ctx, node + hnode::kName, old_name);
                        } else {
                            hfree(ctx, old_pool);
                            r = kOk;
                        }
                    }
                }
            }
        }
    }
    if (locked) unlock(ctx, tr);
    return r;
}

s32 cellHttpRequestSetHeader(PPUContext& ctx, u32 tr, u32 header) { return set_header(ctx, tr, header, true); }
s32 cellHttp_313D80D5(PPUContext& ctx, u32 tr, u32 header) { return set_header(ctx, tr, header, false); }

s32 cellHttpRequestAddHeader(PPUContext& ctx, u32 tr, u32 header)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!header) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = request_open(ctx, tr);
    if (r >= 0) {
        r = headers_add(ctx, tr + t::kHeaders, header);
        if (r == 0) w32(ctx, tr + t::kHeaderCount, r32(ctx, tr + t::kHeaderCount) + 1);
    }
    unlock(ctx, tr);
    return r;
}

s32 cellHttpRequestDeleteHeader(PPUContext& ctx, u32 tr, u32 name)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!name) return err(CELL_HTTP_ERROR_NO_STRING);
    const u32 n = str_len(ctx, name);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = request_open(ctx, tr);
    if (r >= 0) {
        u32 node = r32(ctx, tr + t::kHeaders);
        while (node && str_ncasecmp(ctx, r32(ctx, node + hnode::kName), name, n + 1) != 0)
            node = r32(ctx, node + hnode::kNext);
        if (!node) {
            r = err(CELL_HTTP_ERROR_NO_HEADER);
        } else {
            const u32 prev = r32(ctx, node + hnode::kPrev), next = r32(ctx, node + hnode::kNext);
            if (r32(ctx, tr + t::kHeaders) == node) {
                w32(ctx, tr + t::kHeaders, next);
            } else {
                if (prev) w32(ctx, prev + hnode::kNext, next);
                if (next) w32(ctx, next + hnode::kPrev, prev);
            }
            w32(ctx, node + hnode::kNext, 0);
            w32(ctx, node + hnode::kPrev, 0);
            zero(ctx, node + hnode::kHeader, 8);
            hfree(ctx, r32(ctx, node + hnode::kPool));
            w32(ctx, node + hnode::kPool, 0);
            hfree(ctx, node);
            w32(ctx, tr + t::kHeaderCount, r32(ctx, tr + t::kHeaderCount) - 1);
            r = kOk;
        }
    }
    unlock(ctx, tr);
    return r;
}

// cellHttpRequestGetHeader(transId, CellHttpHeader* header, const char* name, void* pool, u32 poolSize, u32* required):
// the headers libhttp would add first, then the game's.
s32 cellHttpRequestGetHeader(PPUContext& ctx, u32 tr, u32 header, u32 name, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!name) return err(CELL_HTTP_ERROR_NO_STRING);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    StackVar v(ctx, gb(ctx), 8); // the list libhttp adds, the header found
    const u32 list = v.addr(), found = v.addr() + 4;
    w32(ctx, list, 0);
    w32(ctx, found, 0);
    s32 r = auto_headers(ctx, tr, list, 0);
    if (r >= 0) {
        r = headers_find(ctx, r32(ctx, list), found, name);
        if (r == err(CELL_HTTP_ERROR_NO_HEADER)) r = headers_find(ctx, r32(ctx, tr + t::kHeaders), found, name);
        if (r == 0) r = util(ctx, "cellHttpUtilCopyHeader", {header, r32(ctx, found), pool, size, required});
    }
    headers_free(ctx, r32(ctx, list));
    unlock(ctx, tr);
    return r;
}

// cellHttpRequestGetAllHeaders(transId, CellHttpHeader** headers, u32* items, void* pool, u32 poolSize, u32* required):
// libhttp's headers then the game's; the pool holds the array first (8 bytes a header of the game's, 20 one of
// libhttp's, as libhttp counts it), then the strings.
s32 cellHttpRequestGetAllHeaders(PPUContext& ctx, u32 tr, u32 headers, u32 items, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if ((!pool || !headers) && !required) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    StackVar v(ctx, gb(ctx), 16); // used, count, list
    const u32 used = v.addr(), count = v.addr() + 4, list = v.addr() + 8;
    w32(ctx, used, 0);
    w32(ctx, count, 0);
    w32(ctx, list, 0);
    const u32 own = r32(ctx, tr + t::kHeaderCount);
    s32 r = auto_headers(ctx, tr, list, count);
    if (r < 0) {
        unlock(ctx, tr);
        return r;
    }
    const u32 added = r32(ctx, count);
    const u32 array = own * 8 + added * 20;
    if (items) w32(ctx, items, added);
    u32 strings = pool, left = size;
    if (pool && size) {
        if (array > size) {
            headers_free(ctx, r32(ctx, list));
            unlock(ctx, tr);
            return err(CELL_HTTP_ERROR_INSUFFICIENT);
        }
        left = size - array;
        strings = pool + array;
    }
    r = headers_copy_all(ctx, r32(ctx, list), pool, strings, left, used);
    headers_free(ctx, r32(ctx, list));
    if (r < 0) {
        unlock(ctx, tr);
        return r;
    }
    if (required) w32(ctx, required, array + r32(ctx, used));
    if (items) w32(ctx, items, r32(ctx, items) + own);
    u32 at = pool;
    if (strings && left) {
        if (left < r32(ctx, used)) {
            unlock(ctx, tr);
            return err(CELL_HTTP_ERROR_INSUFFICIENT);
        }
        left -= r32(ctx, used);
        strings += r32(ctx, used);
        at = pool + added * 8;
        w32(ctx, used, 0);
    }
    r = headers_copy_all(ctx, r32(ctx, tr + t::kHeaders), at, strings, left, used);
    if (required) w32(ctx, required, r32(ctx, required) + r32(ctx, used));
    if (headers) w32(ctx, headers, pool);
    unlock(ctx, tr);
    return r < 0 ? r : kOk;
}

// ------------------------------------------------------------------------------------------------- content length, chunks

s32 cellHttpRequestSetContentLength(PPUContext& ctx, u32 tr, u64 length)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = request_open(ctx, tr);
    if (r >= 0) {
        if (r8(ctx, tr + t::kChunked)) {
            r = err(CELL_HTTP_ERROR_UNAVAILABLE);
        } else {
            w64(ctx, tr + t::kContentLength, length);
            w8(ctx, tr + t::kHasLength, 1);
            r = kOk;
        }
    }
    unlock(ctx, tr);
    return r;
}

s32 cellHttpRequestGetContentLength(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = err(CELL_HTTP_ERROR_NO_CONTENT_LENGTH);
    if (r8(ctx, tr + t::kHasLength) && usable(ctx, out, 8, "cellHttpRequestGetContentLength")) {
        w64(ctx, out, r64(ctx, tr + t::kContentLength));
        r = kOk;
    }
    unlock(ctx, tr);
    return r;
}

s32 cellHttpRequestSetChunkedTransferStatus(PPUContext& ctx, u32 tr, u32 enable)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = request_open(ctx, tr);
    if (r >= 0) {
        if (r8(ctx, tr + t::kHasLength)) {
            r = err(CELL_HTTP_ERROR_UNAVAILABLE);
        } else {
            w64(ctx, tr + t::kContentLength, 0);
            w8(ctx, tr + t::kChunked, static_cast<u8>(enable));
            r = kOk;
        }
    }
    unlock(ctx, tr);
    return r;
}

s32 cellHttpRequestGetChunkedTransferStatus(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const s32 r = lock(ctx, tr);
    if (r < 0) return r;
    if (usable(ctx, out, 1, "cellHttpRequestGetChunkedTransferStatus")) w8(ctx, out, r8(ctx, tr + t::kChunked));
    unlock(ctx, tr);
    return r;
}

// ------------------------------------------------------------------------------------------------- the response

s32 cellHttpResponseGetStatusLine(PPUContext& ctx, u32 tr, u32 line, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    const s32 r = util(ctx, "cellHttpUtilCopyStatusLine", {line, tr + t::kStatusLine, pool, size, required});
    unlock(ctx, tr);
    return r;
}

s32 cellHttpResponseGetAllHeaders(PPUContext& ctx, u32 tr, u32 headers, u32 items, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if ((!pool || !headers) && !required) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    StackVar v(ctx, gb(ctx), 8);
    const u32 used = v.addr();
    w32(ctx, used, 0);
    const u32 count = r32(ctx, tr + t::kResponseHeaderCount), array = count * 8;
    u32 strings = pool, left = size;
    s32 r = kOk;
    if (pool && size) {
        if (array > size) r = err(CELL_HTTP_ERROR_INSUFFICIENT);
        left = size - array;
        strings = pool + array;
    }
    if (r == kOk) {
        r = headers_copy_all(ctx, r32(ctx, tr + t::kResponseHeaders), pool, strings, left, used);
        if (items) w32(ctx, items, count);
        if (required) w32(ctx, required, array + r32(ctx, used));
        if (headers) w32(ctx, headers, pool);
    }
    unlock(ctx, tr);
    return r;
}

s32 cellHttpResponseGetHeader(PPUContext& ctx, u32 tr, u32 header, u32 name, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!name) return err(CELL_HTTP_ERROR_NO_STRING);
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    StackVar v(ctx, gb(ctx), 8);
    w32(ctx, v.addr(), 0);
    s32 r = headers_find(ctx, r32(ctx, tr + t::kResponseHeaders), v.addr(), name);
    if (r == 0) r = util(ctx, "cellHttpUtilCopyHeader", {header, r32(ctx, v.addr()), pool, size, required});
    unlock(ctx, tr);
    return r;
}

s32 cellHttpResponseGetStatusCode(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    if (usable(ctx, out, 4, "cellHttpResponseGetStatusCode")) w32(ctx, out, r32(ctx, tr + t::kStatusCode));
    unlock(ctx, tr);
    return kOk;
}

s32 cellHttpResponseGetContentLength(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = err(CELL_HTTP_ERROR_NO_CONTENT_LENGTH);
    if (r8(ctx, tr + t::kHasResponseLength) && !r8(ctx, tr + t::k368) &&
        usable(ctx, out, 8, "cellHttpResponseGetContentLength")) {
        w64(ctx, out, r64(ctx, tr + t::kResponseLength));
        r = kOk;
    }
    unlock(ctx, tr);
    return r;
}

// Whether the response is chunked (cellHttp_E0CAF386(transId, bool* chunked)).
s32 cellHttp_E0CAF386(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    const s32 r = lock(ctx, tr);
    if (r < 0) return r;
    if (usable(ctx, out, 1, "cellHttp_E0CAF386")) w8(ctx, out, r8(ctx, tr + t::k368));
    unlock(ctx, tr);
    return r;
}

// cellHttpRecvResponse(transId, char* buf, u32 size, u32* recvd).
s32 cellHttpRecvResponse(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 recvd)
{
    (void)buf, (void)size, (void)recvd;
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = response_ready(ctx, tr); r != 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    const s32 r = unreachable(ctx, "receiving a response");
    unlock(ctx, tr);
    return r;
}

// ------------------------------------------------------------------------------------------------- SSL

// The SSL getters: HTTPS transactions only (CELL_HTTP_ERROR_UNAVAILABLE), then the client checked and locked, then the
// connection (CELL_HTTP_ERROR_BAD_CONN without one), then its SSL object (libssl's).
s32 ssl_info(PPUContext& ctx, u32 tr, std::string_view what)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    s32 r = err(CELL_HTTP_ERROR_UNAVAILABLE);
    if (r8(ctx, tr + t::kHttps)) {
        const u32 client = r32(ctx, tr + t::kClient), conn = r32(ctx, tr + t::kConn);
        r = check_client(ctx, client);
        if (r >= 0) r = lw_lock(ctx, client + c::kLock);
        if (r >= 0) {
            if (!connection_valid(ctx, conn)) r = err(CELL_HTTP_ERROR_BAD_CONN);
            else if (!r8(ctx, conn + 121)) r = err(CELL_HTTP_ERROR_UNAVAILABLE);
            else r = unreachable(ctx, what);
            lw_unlock(ctx, client + c::kLock);
        }
    }
    unlock(ctx, tr);
    return r;
}

s32 cellHttpTransactionGetSslId(PPUContext& ctx, u32 tr, u32 id)
{
    (void)id;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslId");
}
s32 cellHttpTransactionGetSslVersion(PPUContext& ctx, u32 tr, u32 version)
{
    (void)version;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslVersion");
}
s32 cellHttpTransactionGetSslCipherString(PPUContext& ctx, u32 tr, u32 buf, u32 size)
{
    (void)buf, (void)size;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslCipherString");
}
s32 cellHttpTransactionGetSslCipherBits(PPUContext& ctx, u32 tr, u32 effective, u32 algorithm)
{
    (void)effective, (void)algorithm;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslCipherBits");
}
s32 cellHttpTransactionGetSslCipherVersion(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 required)
{
    (void)buf, (void)size, (void)required;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslCipherVersion");
}
s32 cellHttpTransactionGetSslCipherId(PPUContext& ctx, u32 tr, u32 id)
{
    (void)id;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslCipherId");
}
s32 cellHttpTransactionGetSslCipherName(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 required)
{
    (void)buf, (void)size, (void)required;
    return ssl_info(ctx, tr, "cellHttpTransactionGetSslCipherName");
}

// ------------------------------------------------------------------------------------------------- the connection

// A call on the transaction's connection: one that has none (state 0) answers CELL_HTTP_ERROR_TRANS_NO_CONNECTION.
s32 on_connection(PPUContext& ctx, u32 tr, std::string_view what)
{
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    const s32 r = r32(ctx, tr + t::kState) == 0 ? err(CELL_HTTP_ERROR_TRANS_NO_CONNECTION) : unreachable(ctx, what);
    unlock(ctx, tr);
    return r;
}

s32 cellHttpTransactionCloseConnection(PPUContext& ctx, u32 tr)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    return on_connection(ctx, tr, "cellHttpTransactionCloseConnection");
}

// The connection closed and its pending work aborted first (cellHttp_3E30440C(transId)).
s32 cellHttp_3E30440C(PPUContext& ctx, u32 tr)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    return on_connection(ctx, tr, "cellHttp_3E30440C");
}

s32 cellHttpTransactionReleaseConnection(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    return on_connection(ctx, tr, "cellHttpTransactionReleaseConnection");
}

// The connection's pipeline (cellHttp_8633468C(transId, u32* out)).
s32 cellHttp_8633468C(PPUContext& ctx, u32 tr, u32 out)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    return on_connection(ctx, tr, "cellHttp_8633468C");
}

// cellHttpTransactionAbortConnection(transId): under the client's transaction lock, a transaction being worked on
// (states 0..6) is marked aborted (+461); its connection, when it has one, closed.
s32 cellHttpTransactionAbortConnection(PPUContext& ctx, u32 tr)
{
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    const u32 client = r32(ctx, tr + t::kClient);
    if (const s32 r = client_trans_lock(ctx, client, true); r < 0) return r;
    if (!valid(ctx, tr, t::kSelf)) {
        client_trans_lock(ctx, client, false);
        return err(CELL_HTTP_ERROR_BAD_TRANS);
    }
    const s32 state = static_cast<s32>(r32(ctx, tr + t::kState));
    s32 r = kOk;
    if (state == -1) {
        r = err(CELL_HTTP_ERROR_TRANS_ABORTED);
    } else if (state == -2) {
        r = err(CELL_HTTP_ERROR_BROKEN_PIPELINE);
    } else if (state < -2 || state > 6) {
        r = err(CELL_HTTP_ERROR_TRANS_NO_CONNECTION);
    } else {
        r = lw_lock(ctx, tr + t::kAbortLock);
        if (r >= 0) {
            w8(ctx, tr + t::kAborted, 1);
            const u32 waiting = r32(ctx, tr + t::k448);
            lw_unlock(ctx, tr + t::kAbortLock);
            if (r32(ctx, tr + t::kConn2) || r32(ctx, tr + t::kConn)) r = unreachable(ctx, "aborting a connection");
            if (waiting) r = unreachable(ctx, "aborting a wait");
        }
    }
    client_trans_lock(ctx, r32(ctx, tr + t::kClient), false);
    return r;
}

// ------------------------------------------------------------------------------------------------- sending

// cellHttpSendRequest(transId, const char* buf, u32 size, u32* sent) and cellHttp_DCD9D484 (the same with the caches
// consulted): a fresh transaction (state 0) calls the client's state callback (1), looks for a redirection record when
// the client follows redirections, then asks the manager for a connection; offline the connection's resolver fails and
// that is the answer (*sent 0).
s32 send_request(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 sent, bool cached)
{
    (void)buf, (void)size;
    if (const s32 r = check_trans(ctx, tr); r < 0) return r;
    if (const s32 r = lock(ctx, tr); r < 0) return r;
    const u32 client = r32(ctx, tr + t::kClient);
    if (const s32 r = check_client(ctx, client); r < 0) {
        unlock(ctx, tr);
        return r;
    }
    const auto end = [&](s32 r) {
        if (sent && usable(ctx, sent, 4, "cellHttpSendRequest: sent")) w32(ctx, sent, 0);
        unlock(ctx, tr);
        return r;
    };
    switch (static_cast<s32>(r32(ctx, tr + t::kState))) {
    case -2: unlock(ctx, tr); return err(CELL_HTTP_ERROR_BROKEN_PIPELINE);
    case -1: unlock(ctx, tr); return err(CELL_HTTP_ERROR_TRANS_ABORTED);
    case 0: case 6: break;
    case 1: case 2: case 3: case 4: return end(unreachable(ctx, "sending on a connection"));
    case 5: case 7: unlock(ctx, tr); return err(CELL_HTTP_ERROR_ALREADY_SENT);
    default: unlock(ctx, tr); return err(CELL_HTTP_ERROR_UNKNOWN);
    }
    state_callback(ctx, client, r32(ctx, tr + t::kSelf), 1);
    if (r8(ctx, tr + t::kAutoRedirect)) {
        u32 rec = 0;
        if (const s32 r = redirect_find(ctx, tr, r32(ctx, tr + t::kMethod), rec); r < 0) return end(r);
        if (rec) return end(unreachable(ctx, "following a redirection record"));
    }
    if (cached && r32(ctx, tr + t::kCacheEntry) && !r8(ctx, tr + t::kHasLength) && !r8(ctx, tr + t::kChunked))
        return end(unreachable(ctx, "a response from cellHttpInitCache's cache"));
    disk_cache_count(ctx);
    if (cached && (r32(ctx, tr + t::kDiskEntry) || r32(ctx, tr + t::kDiskEntry + 4)))
        return end(unreachable(ctx, "a response from the disk cache"));
    StackVar flag(ctx, gb(ctx), 8);
    w8(ctx, flag.addr(), 0);
    const u32 scheme = r32(ctx, tr + t::kUriScheme);
    if (scheme && str_is(ctx, scheme, "https")) {
        if (!https_initialized(ctx)) return end(err(CELL_HTTP_ERROR_INVALID_URI));
        w8(ctx, tr + t::kHttps, 1);
    } else {
        w8(ctx, tr + t::kHttps, 0);
    }
    const s32 r = connection_acquire(ctx, client, tr + t::kUri, tr + t::kConn, tr + t::kProxied,
        r8(ctx, tr + t::kKeepAlive), r8(ctx, tr + t::kHttps), flag.addr(), tr);
    if (r == 0 || r32(ctx, tr + t::kConn)) return end(unreachable(ctx, "a transaction's new connection"));
    if (r == err(CELL_HTTP_ERROR_NO_CONNECTION) && r8(ctx, tr + t::kPipeline))
        return end(unreachable(ctx, "a pipelined connection"));
    return end(r);
}

s32 cellHttpSendRequest(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 sent)
{
    return send_request(ctx, tr, buf, size, sent, false);
}
s32 cellHttp_DCD9D484(PPUContext& ctx, u32 tr, u32 buf, u32 size, u32 sent)
{
    return send_request(ctx, tr, buf, size, sent, true);
}

} // namespace

void register_transactions(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0x052A80D9, cellHttpCreateTransaction),
        PS_HLE_FUNC(0x32F5CAE2, cellHttpDestroyTransaction),
        PS_HLE_FUNC(0x0EF17399, cellHttpTransactionGetUri),
        PS_HLE_FUNC(0x5C8AE162, cellHttpTransactionSetUri),
        PS_HLE_FUNC(0xDAC62AB6, cellHttp_DAC62AB6),
        PS_HLE_FUNC(0x15C1FD80, cellHttp_15C1FD80),
        PS_HLE_FUNC(0x54F2A4DE, cellHttpRequestSetHeader),
        PS_HLE_FUNC(0x313D80D5, cellHttp_313D80D5),
        PS_HLE_FUNC(0xED993147, cellHttpRequestAddHeader),
        PS_HLE_FUNC(0x16214411, cellHttpRequestDeleteHeader),
        PS_HLE_FUNC(0x0B9FEA5F, cellHttpRequestGetHeader),
        PS_HLE_FUNC(0x42205FE0, cellHttpRequestGetAllHeaders),
        PS_HLE_FUNC(0xAF73A64E, cellHttpRequestSetContentLength),
        PS_HLE_FUNC(0x958323CF, cellHttpRequestGetContentLength),
        PS_HLE_FUNC(0x8E3F7EE1, cellHttpRequestSetChunkedTransferStatus),
        PS_HLE_FUNC(0x4137A1F6, cellHttpRequestGetChunkedTransferStatus),
        PS_HLE_FUNC(0x6A81B5E4, cellHttpResponseGetStatusLine),
        PS_HLE_FUNC(0xBEA17389, cellHttpResponseGetAllHeaders),
        PS_HLE_FUNC(0x4F5D8D20, cellHttpResponseGetHeader),
        PS_HLE_FUNC(0x10D0D7FC, cellHttpResponseGetStatusCode),
        PS_HLE_FUNC(0x464FF889, cellHttpResponseGetContentLength),
        PS_HLE_FUNC(0xE0CAF386, cellHttp_E0CAF386),
        PS_HLE_FUNC(0x61C90691, cellHttpRecvResponse),
        PS_HLE_FUNC(0x2A78FF04, cellHttpTransactionGetSslId),
        PS_HLE_FUNC(0xAD1C6F02, cellHttpTransactionGetSslVersion),
        PS_HLE_FUNC(0xE3C424B3, cellHttpTransactionGetSslCipherString),
        PS_HLE_FUNC(0x38954133, cellHttpTransactionGetSslCipherBits),
        PS_HLE_FUNC(0x93E938E5, cellHttpTransactionGetSslCipherVersion),
        PS_HLE_FUNC(0x34061E49, cellHttpTransactionGetSslCipherId),
        PS_HLE_FUNC(0x895C604C, cellHttpTransactionGetSslCipherName),
        PS_HLE_FUNC(0xA0D9223C, cellHttpTransactionCloseConnection),
        PS_HLE_FUNC(0x3E30440C, cellHttp_3E30440C),
        PS_HLE_FUNC(0xD47CC666, cellHttpTransactionReleaseConnection),
        PS_HLE_FUNC(0x8633468C, cellHttp_8633468C),
        PS_HLE_FUNC(0x2D52848B, cellHttpTransactionAbortConnection),
        PS_HLE_FUNC(0xA755B005, cellHttpSendRequest),
        PS_HLE_FUNC(0xDCD9D484, cellHttp_DCD9D484),
    });
}

} // namespace pspocket::hle::http
