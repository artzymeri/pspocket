// cellHttp's clients (D-524): cellHttpCreateClient/DestroyClient, every client setting and its getter, the client's
// headers, user agent and proxy, its callbacks, and the connection manager each client owns (its timeouts, pool limits,
// buffer sizes, SSL version), as libhttp.sprx does them. See cellHttp.cpp for the method and cellHttp.hpp for the
// objects' layouts.
//
// Every function checks its client as libhttp does: cellHttpInit first (CELL_HTTP_ERROR_NOT_INITIALIZED; a few
// functions don't), then sys_memory_get_page_attribute on the address and the client's pointer to itself
// (CELL_HTTP_ERROR_BAD_CLIENT), then its other arguments; then it holds the client's lock "_httcln". What goes to the
// connection manager is checked again there (CELL_HTTP_ERROR_BAD_MAN) under the manager's lock "_httcmg".
//
// Arguments libhttp takes as bool are its register's low byte where libhttp stores a byte, the whole word where it
// compares one (cellHttpClientSetPipeline), as libhttp does.

#include "cellHttp.hpp"

#include <format>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::hle::http {

namespace c = client;
namespace m = manager;

namespace {

// " libhttp/4.9.2-000 (CellOS)", the user agent's suffix (libhttp's version; 27 characters).
constexpr std::string_view kUaPrefix = "PS3Application", kUaSuffix = " libhttp/4.9.2-000 (CellOS)";

s32 check_client(PPUContext& ctx, u32 client)
{
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!valid(ctx, client, c::kSelf)) return err(CELL_HTTP_ERROR_BAD_CLIENT);
    return kOk;
}

// The client's manager, read under the client's lock (as libhttp reads it before every manager call).
s32 client_manager(PPUContext& ctx, u32 client, u32& mgr)
{
    if (const s32 r = lw_lock(ctx, client + c::kLock); r < 0) return r;
    mgr = r32(ctx, client + c::kManager);
    lw_unlock(ctx, client + c::kLock);
    return kOk;
}

// ------------------------------------------------------------------------------------------------- the manager

bool manager_valid(PPUContext& ctx, u32 mgr) { return valid(ctx, mgr, m::kSelf); }

// A manager's timeouts (u64): 0 waiting for a connection, 1 receiving, 2 sending, 3 connecting.
u32 timeout_field(u32 index)
{
    switch (index) {
    case 0: return m::kWaitTimeout;
    case 1: return m::kRecvTimeout;
    case 2: return m::kSendTimeout;
    case 3: return m::kConnTimeout;
    default: return 0xFFFFFFFF;
    }
}

s32 manager_get_timeout(PPUContext& ctx, u32 mgr, u32 index, u32 out)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (const u32 f = timeout_field(index); f != 0xFFFFFFFF) {
        if (usable(ctx, out, 8, "cellHttpClientGet*Timeout")) w64(ctx, out, r64(ctx, mgr + f));
    }
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

s32 manager_set_timeout(PPUContext& ctx, u32 mgr, u32 index, u64 value)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (const u32 f = timeout_field(index); f != 0xFFFFFFFF) w64(ctx, mgr + f, value);
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

// A manager's limits (u32): 0 the total connections, 1 per host, 2 kept alive per host, 3 per pipeline, 4..9 libhttp's
// unnamed ones, 10 the buffer size.
u32 limit_field(u32 index)
{
    static constexpr u32 kFields[] = {m::kTotalPool, m::kPerHostPool, m::kPerHostKeepAliveMax, m::kPerPipelineMax, m::k96,
        m::k100, m::k108, m::k104, m::kCount64, m::kCount92, m::kBufSize};
    return index < std::size(kFields) ? kFields[index] : 0xFFFFFFFF;
}

s32 manager_get_limit(PPUContext& ctx, u32 mgr, u32 index, u32 out)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (const u32 f = limit_field(index); f != 0xFFFFFFFF) {
        if (usable(ctx, out, 4, "cellHttpClientGet* (a limit)")) w32(ctx, out, r32(ctx, mgr + f));
    }
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

// Each limit's maximum; a larger value is CELL_HTTP_ERROR_INVALID_VALUE, or the maximum for a process made with an SDK
// before 3.3 (libhttp asks lv2 for the SDK version first). The connections kept alive per host are the exception: up to
// 8 now, clamped to 4 for an old SDK.
s32 manager_set_limit(PPUContext& ctx, u32 mgr, u32 index, u32 value)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    const bool old_sdk = sdk_version(ctx) <= 0x32FFFF;
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    s32 r = kOk;
    const auto limited = [&](u32 field, u32 max) {
        if (value > max) {
            if (!old_sdk) {
                r = err(CELL_HTTP_ERROR_INVALID_VALUE);
                return;
            }
            value = max;
        }
        w32(ctx, mgr + field, value);
    };
    switch (index) {
    case 0: limited(m::kTotalPool, 64); break;
    case 1: limited(m::kPerHostPool, 16); break;
    case 2:
        if (old_sdk) {
            w32(ctx, mgr + m::kPerHostKeepAliveMax, value > 4 ? 4 : value);
        } else if (value > 8) {
            r = err(CELL_HTTP_ERROR_INVALID_VALUE);
        } else {
            w32(ctx, mgr + m::kPerHostKeepAliveMax, value);
        }
        break;
    case 3: limited(m::kPerPipelineMax, 8); break;
    case 4: limited(m::k96, 16); break;
    case 5: limited(m::k100, 8); break;
    case 6: limited(m::k108, 4); break;
    case 7: limited(m::k104, 8); break;
    case 10: w32(ctx, mgr + m::kBufSize, value); break;
    default: break; // 8, 9 and past 10: nothing
    }
    lw_unlock(ctx, mgr + m::kLock);
    return r;
}

// A manager's switches (bytes): 0 its +161, 1 waiting for a connection.
s32 manager_get_switch(PPUContext& ctx, u32 mgr, u32 index, u32 out)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (index <= 1 && usable(ctx, out, 1, "cellHttpClientGet* (a switch)"))
        w8(ctx, out, r8(ctx, mgr + (index == 0 ? m::k161 : m::kWaitStatus)));
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

s32 manager_set_switch(PPUContext& ctx, u32 mgr, u32 index, u8 value)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (index <= 1) w8(ctx, mgr + (index == 0 ? m::k161 : m::kWaitStatus), value);
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

// The send and receive buffer sizes: 0 (never set) reads as 0xFFFF; 1..0x80000 can be set.
s32 manager_get_buffer(PPUContext& ctx, u32 mgr, u32 field, u32 out)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    const s32 v = static_cast<s32>(r32(ctx, mgr + field));
    if (usable(ctx, out, 4, "cellHttpClientGet*BufferSize")) w32(ctx, out, v > 0 ? static_cast<u32>(v) : 0xFFFF);
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

s32 manager_set_buffer(PPUContext& ctx, u32 mgr, u32 field, u32 value)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (value - 1 > 0x7FFFF) return err(CELL_HTTP_ERROR_INVALID_VALUE);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    w32(ctx, mgr + field, value);
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

s32 manager_set_ssl_version(PPUContext& ctx, u32 mgr, u32 version)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (version > 2) return err(CELL_HTTP_ERROR_INVALID_VALUE);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    w32(ctx, mgr + m::kSslVersion, version);
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

s32 manager_set_state_callback(PPUContext& ctx, u32 mgr, u32 cb, u32 arg)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    w32(ctx, mgr + m::kStateCb + 4, arg);
    w32(ctx, mgr + m::kStateCb, cb);
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

} // namespace

s32 manager_ssl_version(PPUContext& ctx, u32 mgr, u32 out)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (usable(ctx, out, 4, "cellHttpClientGetSslVersion")) w32(ctx, out, r32(ctx, mgr + m::kSslVersion));
    lw_unlock(ctx, mgr + m::kLock);
    return kOk;
}

// libhttp's manager (168 bytes) for a client: the limits given, the lock "_httcmg", the default timeouts (120 s to
// receive and to send, 30 s to connect, 150 s to wait for a connection), the SSL version 2 for a process made with an
// SDK before 4.7 (0 after), the library's buffer size; the transaction functions table at +116.
s32 manager_create(PPUContext& ctx, u32 out, u32 total, u32 per_host, u32 keep_alive, u32 per_pipeline, u32 client)
{
    const u32 mgr = hmalloc(ctx, m::kSize);
    if (!mgr) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, mgr, m::kSize);
    if (const s32 r = lw_create(ctx, mgr + m::kLock, "_httcmg"); r < 0) {
        hfree(ctx, mgr);
        return r;
    }
    w32(ctx, mgr + m::k104, 4);
    w32(ctx, mgr + m::k96, 5);
    w32(ctx, mgr + m::k100, 4);
    w32(ctx, mgr + m::kAllocator, data(ctx) + kTransactionTable);
    w32(ctx, mgr + m::kSelf, mgr);
    w32(ctx, mgr + m::kTotalPool, total);
    w32(ctx, mgr + m::kPerHostPool, per_host);
    w32(ctx, mgr + m::kPerHostKeepAliveMax, keep_alive);
    w32(ctx, mgr + m::kPerPipelineMax, per_pipeline);
    w32(ctx, mgr + m::k108, 2);
    w64(ctx, mgr + m::kRecvTimeout, 120000000);
    w64(ctx, mgr + m::kSendTimeout, 120000000);
    w32(ctx, mgr + m::kBufSize, r32(ctx, data(ctx) + kSys + kSysBufSize));
    w64(ctx, mgr + m::kConnTimeout, 30000000);
    w64(ctx, mgr + m::kWaitTimeout, 150000000);
    w8(ctx, mgr + m::kWaitStatus, 0);
    w32(ctx, mgr + m::kRecvBuf, 0);
    w32(ctx, mgr + m::kSendBuf, 0);
    w8(ctx, mgr + m::k161, 0);
    w32(ctx, mgr + m::kSslVersion, sdk_version(ctx) <= 0x465FFF ? 2 : 0);
    w32(ctx, out, mgr);
    w32(ctx, mgr + m::kClient, client);
    return kOk;
}

s32 manager_destroy(PPUContext& ctx, u32 mgr)
{
    if (!manager_valid(ctx, mgr)) return err(CELL_HTTP_ERROR_BAD_MAN);
    const u32 ssl = r32(ctx, mgr + m::k132);
    if (const s32 r = hosts_clear(ctx, mgr + m::kHosts, ssl); r < 0) return r;
    if (const s32 r = hosts_clear(ctx, mgr + m::kHosts2, ssl); r < 0) return r;
    while (const u32 node = r32(ctx, mgr + m::kConnections)) {
        const u32 conn = r32(ctx, node + 8);
        w32(ctx, mgr + m::kConnections, r32(ctx, node));
        w32(ctx, node + 4, 0);
        w32(ctx, node, 0);
        hfree(ctx, node);
        const bool second = connection_flag140(ctx, conn);
        connection_destroy(ctx, conn, r32(ctx, mgr + m::k132));
        if (r8(ctx, mgr + m::k161) && second) w32(ctx, mgr + m::kCount92, r32(ctx, mgr + m::kCount92) - 1);
        else w32(ctx, mgr + m::kCount64, r32(ctx, mgr + m::kCount64) - 1);
        w32(ctx, mgr + m::kCount128, r32(ctx, mgr + m::kCount128) - 1);
    }
    lw_destroy(ctx, mgr + m::kLock);
    w32(ctx, mgr + m::kSelf, 0);
    hfree(ctx, mgr);
    return kOk;
}

namespace {

// ------------------------------------------------------------------------------------------------- clients

// The user agent's text after "PS3Application" (libhttp's version), its size into *required: 27.
void ua_suffix(PPUContext& ctx, u32 dest, u32 room, u32 required)
{
    if (required) w32(ctx, required, static_cast<u32>(kUaSuffix.size()));
    if (!dest || room <= kUaSuffix.size() - 1) return;
    for (usize i = 0; i < kUaSuffix.size(); i++) w8(ctx, dest + static_cast<u32>(i), static_cast<u8>(kUaSuffix[i]));
}

// cellHttpCreateClient(CellHttpClientId* clientId): a 224-byte client with its manager (24 connections in all, 8 per
// host, 2 kept alive per host, 4 per pipeline), a copy of the global proxy when one is set, the user agent
// "PS3Application libhttp/4.9.2-000 (CellOS)", HTTP/1.1, keep-alive, automatic redirection and authentication, the
// authentication cache, a response buffer maximum of 5000, the locks "_httcln" and "_httcla", and an SSL record when
// HTTPS is initialised.
s32 cellHttpCreateClient(PPUContext& ctx, u32 out)
{
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (!usable(ctx, out, 4, "cellHttpCreateClient: clientId")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 cl = hmalloc(ctx, c::kSize);
    if (!cl) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, cl, c::kSize);
    StackVar v(ctx, gb(ctx), 16); // the manager, the proxy's size, the user agent's size
    const u32 mgr_out = v.addr(), need = v.addr() + 4, ua_need = v.addr() + 8;
    s32 r = manager_create(ctx, mgr_out, 24, 8, 2, 4, cl);
    if (r < 0) {
        hfree(ctx, cl);
        return r;
    }
    const auto fail = [&](s32 code, bool proxy) {
        manager_destroy(ctx, r32(ctx, mgr_out));
        if (proxy && r32(ctx, cl + c::kProxyPool)) hfree(ctx, r32(ctx, cl + c::kProxyPool));
        w32(ctx, cl + c::kProxyPool, 0);
        hfree(ctx, cl);
        return code;
    };
    r = static_cast<s32>(call(ctx, "cellHttp", "cellHttpGetProxy", {0, 0, 0, need}));
    if (r < 0) return fail(r, false);
    if (r32(ctx, need)) {
        const u32 pool = hmalloc(ctx, r32(ctx, need));
        w32(ctx, cl + c::kProxyPool, pool);
        if (!pool) return fail(err(CELL_HTTP_ERROR_NO_MEMORY), false);
        r = static_cast<s32>(call(ctx, "cellHttp", "cellHttpGetProxy", {cl + c::kProxy, pool, r32(ctx, need), 0}));
        if (r < 0) return fail(r, true);
        w8(ctx, cl + c::kUseProxy, 1);
    } else {
        w8(ctx, cl + c::kUseProxy, 0);
    }
    ua_suffix(ctx, 0, 0, ua_need);
    const u32 ua_len = static_cast<u32>(kUaPrefix.size()) + r32(ctx, ua_need); // 41
    const u32 ua = hmalloc(ctx, ua_len + 1);
    w32(ctx, cl + c::kUa, ua);
    if (!ua) return fail(err(CELL_HTTP_ERROR_NO_MEMORY), true);
    for (usize i = 0; i < kUaPrefix.size(); i++) w8(ctx, ua + static_cast<u32>(i), static_cast<u8>(kUaPrefix[i]));
    ua_suffix(ctx, ua + static_cast<u32>(kUaPrefix.size()), ua_len - static_cast<u32>(kUaPrefix.size()) + 1, 0);
    w8(ctx, ua + ua_len, 0);
    w32(ctx, cl + c::kMajor, 1);
    w32(ctx, cl + c::kResponseBufferMax, 5000);
    w32(ctx, cl + c::kUaSize, ua_len + 1);
    w32(ctx, cl + c::k32, 5);
    w32(ctx, cl + c::kMinor, 1);
    w32(ctx, cl + c::kHeaderCount, 0);
    w32(ctx, cl + c::k156, 0);
    w32(ctx, cl + c::k148, 0);
    for (const u32 f : {c::kAuthCache, c::kKeepAlive, c::kAutoRedirect, c::kAutoAuth, c::k214}) w8(ctx, cl + f, 1);
    w32(ctx, cl + c::kHeaders, 0);
    for (const u32 f : {c::kPipeline, c::kCookie, c::k215, c::kCache, c::k218, c::kDiskCache}) w8(ctx, cl + f, 0);
    w64(ctx, cl + c::k136, 0);
    w32(ctx, cl + c::kSelf, cl);
    zero(ctx, cl + c::kAuthCb, 24); // the authentication, transaction state and redirection callbacks
    w32(ctx, cl + c::kManager, r32(ctx, mgr_out));
    if (r = lw_create(ctx, cl + c::kLock, "_httcln"); r < 0) {
        manager_destroy(ctx, r32(ctx, mgr_out));
        hfree(ctx, r32(ctx, cl + c::kUa));
        w32(ctx, cl + c::kUa, 0);
        hfree(ctx, cl);
        return r;
    }
    if (r = lw_create(ctx, cl + c::kTransLock, "_httcla"); r < 0) {
        manager_destroy(ctx, r32(ctx, mgr_out));
        lw_destroy(ctx, cl + c::kLock);
        hfree(ctx, r32(ctx, cl + c::kUa));
        w32(ctx, cl + c::kUa, 0);
        if (const u32 p = r32(ctx, cl + c::kProxyPool)) hfree(ctx, p);
        w32(ctx, cl + c::kProxyPool, 0);
        hfree(ctx, cl);
        return r;
    }
    zero(ctx, cl + c::kSslCb, 16); // the SSL and SSL ID destruction callbacks
    if (r = client_ssl_create(ctx, r32(ctx, cl + c::kSelf)); r < 0) {
        manager_destroy(ctx, r32(ctx, mgr_out));
        lw_destroy(ctx, cl + c::kLock);
        hfree(ctx, r32(ctx, cl + c::kUa));
        w32(ctx, cl + c::kUa, 0);
        if (const u32 p = r32(ctx, cl + c::kProxyPool)) hfree(ctx, p);
        w32(ctx, cl + c::kProxyPool, 0);
        hfree(ctx, cl);
        return r;
    }
    w32(ctx, out, cl);
    return kOk;
}

// cellHttpDestroyClient: the SSL context and record, the manager (and its connections), the proxy's pool, the client's
// session cookies, its headers and user agent, both locks; the client's pointer to itself cleared, the block freed.
// Its transactions are not destroyed (libhttp leaves them).
s32 cellHttpDestroyClient(PPUContext& ctx, u32 cl)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    client_ssl_ctx_free(ctx, cl);
    client_ssl_destroy(ctx, cl);
    manager_destroy(ctx, r32(ctx, cl + c::kManager));
    if (const u32 p = r32(ctx, cl + c::kProxyPool)) {
        hfree(ctx, p);
        w32(ctx, cl + c::kProxyPool, 0);
        zero(ctx, cl + c::kProxy, 28);
    }
    session_cookie_flush(ctx, cl);
    headers_free(ctx, r32(ctx, cl + c::kHeaders));
    w32(ctx, cl + c::kHeaderCount, 0);
    if (const u32 ua = r32(ctx, cl + c::kUa)) hfree(ctx, ua);
    w32(ctx, cl + c::kUa, 0);
    lw_destroy(ctx, cl + c::kLock);
    lw_destroy(ctx, cl + c::kTransLock);
    w32(ctx, cl + c::kSelf, 0);
    hfree(ctx, cl);
    return kOk;
}

// ---- the response buffer maximum and its three neighbours (u32 fields)

u32 word_field(u32 index)
{
    switch (index) {
    case 0: return c::kResponseBufferMax;
    case 1: return c::k32;
    case 2: return c::k136; // a u64: its low word is read, the value zero-extended is written
    case 3: return c::k148;
    default: return 0xFFFFFFFF;
    }
}

s32 get_word(PPUContext& ctx, u32 cl, u32 index, u32 out)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    if (const u32 f = word_field(index); f != 0xFFFFFFFF && usable(ctx, out, 4, "cellHttpClientGet* (a word)"))
        w32(ctx, out, f == c::k136 ? static_cast<u32>(r64(ctx, cl + f)) : r32(ctx, cl + f));
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}

// The response buffer maximum is at least 1500.
s32 set_word(PPUContext& ctx, u32 cl, u32 index, u32 value)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    switch (index) {
    case 0: w32(ctx, cl + c::kResponseBufferMax, value < 1500 ? 1500 : value); break;
    case 1: w32(ctx, cl + c::k32, value); break;
    case 2: w64(ctx, cl + c::k136, value); break;
    case 3: w32(ctx, cl + c::k148, value); break;
    default: break;
    }
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}

// ---- the switches (bytes): 0 keep-alive, 1 pipelining, 2 automatic redirection, 3 automatic authentication, 4 cookies,
// 5 and 6 libhttp's unnamed, 7 the cache, 8 the external cache, 9 the disk cache, 10 bit 0 of +156, 11 the
// authentication cache.

u32 switch_field(u32 index)
{
    static constexpr u32 kFields[] = {c::kKeepAlive, c::kPipeline, c::kAutoRedirect, c::kAutoAuth, c::kCookie, c::k214,
        c::k215, c::kCache, c::k218, c::kDiskCache, c::k156, c::kAuthCache};
    return index < std::size(kFields) ? kFields[index] : 0xFFFFFFFF;
}

s32 get_switch(PPUContext& ctx, u32 cl, u32 index, u32 out)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    if (const u32 f = switch_field(index); f != 0xFFFFFFFF && usable(ctx, out, 1, "cellHttpClientGet* (a switch)"))
        w8(ctx, out, index == 10 ? static_cast<u8>(r32(ctx, cl + f) & 1) : r8(ctx, cl + f));
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}

// The caches can be switched on only when initialised (cellHttpInitCache, the external cache, the disk cache); one on
// switches the others off, and gives the client the cache's size limit when it had none.
s32 set_switch(PPUContext& ctx, u32 cl, u32 index, u32 value)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    const u8 b = static_cast<u8>(value);
    s32 r = kOk;
    const auto cache_on = [&](u32 on, u32 off1, u32 off2, bool ready, u32 error, u64 limit) {
        if (!ready) {
            r = err(error);
            return;
        }
        w8(ctx, cl + off1, 0);
        w8(ctx, cl + on, 1);
        w8(ctx, cl + off2, 0);
        if (r64(ctx, cl + c::k136) == 0) w64(ctx, cl + c::k136, limit);
    };
    const u32 d = data(ctx);
    switch (index) {
    case 0:
        w8(ctx, cl + c::kKeepAlive, b);
        if (!b) w8(ctx, cl + c::kPipeline, 0);
        break;
    case 1:
        if (r8(ctx, cl + c::kKeepAlive)) {
            w8(ctx, cl + c::kPipeline, b);
            if (value) {
                w8(ctx, cl + c::kAutoAuth, 0);
                w8(ctx, cl + c::kAutoRedirect, 0);
            }
        }
        break;
    case 2: w8(ctx, cl + c::kAutoRedirect, b); break;
    case 3: w8(ctx, cl + c::kAutoAuth, b); break;
    case 4: w8(ctx, cl + c::kCookie, b); break;
    case 5: w8(ctx, cl + c::k214, b); break;
    case 6: w8(ctx, cl + c::k215, b); break;
    case 7:
        if (!b) {
            w8(ctx, cl + c::kCache, 0);
            w64(ctx, cl + c::k136, 0);
        } else {
            cache_on(c::kCache, c::kDiskCache, c::k218, r8(ctx, d + kCache + 24) != 0,
                CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED, r32(ctx, d + kCache + 28));
        }
        break;
    case 8:
        if (!b) {
            w8(ctx, cl + c::k218, 0);
            w64(ctx, cl + c::k136, 0);
        } else {
            cache_on(c::k218, c::kDiskCache, c::kCache, r8(ctx, d + kExtCache + 24) != 0,
                CELL_HTTP_ERROR_CACHE_NOT_INITIALIZED, r32(ctx, d + kExtCache + 32));
        }
        break;
    case 9:
        if (!b) {
            w8(ctx, cl + c::kDiskCache, 0);
            w64(ctx, cl + c::k136, 0);
        } else {
            cache_on(c::kDiskCache, c::k218, c::kCache, r8(ctx, d + kDiskCache + 16) != 0,
                CELL_HTTP_ERROR_DCACHE_NOT_INITIALIZED, 0x6400000);
        }
        break;
    case 10:
        w32(ctx, cl + c::k156, value ? r32(ctx, cl + c::k156) | 1 : r32(ctx, cl + c::k156) & ~1u);
        break;
    case 11: w8(ctx, cl + c::kAuthCache, b); break;
    default: break;
    }
    lw_unlock(ctx, cl + c::kLock);
    return r;
}

// ---- what goes to the manager

template <class F>
s32 with_manager(PPUContext& ctx, u32 cl, F&& f)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    u32 mgr = 0;
    if (const s32 r = client_manager(ctx, cl, mgr); r < 0) return r;
    return f(mgr);
}

s32 set_limit(PPUContext& ctx, u32 cl, u32 index, u32 value)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_set_limit(ctx, mgr, index, value); });
}
s32 get_limit(PPUContext& ctx, u32 cl, u32 index, u32 out)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_get_limit(ctx, mgr, index, out); });
}
s32 set_mswitch(PPUContext& ctx, u32 cl, u32 index, u32 value)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_set_switch(ctx, mgr, index, static_cast<u8>(value)); });
}
s32 get_mswitch(PPUContext& ctx, u32 cl, u32 index, u32 out)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_get_switch(ctx, mgr, index, out); });
}
s32 get_timeout(PPUContext& ctx, u32 cl, u32 index, u32 out)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_get_timeout(ctx, mgr, index, out); });
}
s32 set_timeout(PPUContext& ctx, u32 cl, u32 index, u64 value)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_set_timeout(ctx, mgr, index, value); });
}

// ---- the exported functions

s32 cellHttpClientGetResponseBufferMax(PPUContext& ctx, u32 cl, u32 out) { return get_word(ctx, cl, 0, out); }
s32 cellHttpClientSetResponseBufferMax(PPUContext& ctx, u32 cl, u32 v) { return set_word(ctx, cl, 0, v); }

// libhttp's exports with no known name, beside the named ones they share their code with (by index).
s32 cellHttp_D5C9C1DF(PPUContext& ctx, u32 cl, u32 out) { return get_word(ctx, cl, 1, out); }
s32 cellHttp_415087D1(PPUContext& ctx, u32 cl, u32 out) { return get_word(ctx, cl, 2, out); }
s32 cellHttp_44A86F38(PPUContext& ctx, u32 cl, u32 out) { return get_word(ctx, cl, 3, out); }
s32 cellHttp_42D600B1(PPUContext& ctx, u32 cl, u32 v) { return set_word(ctx, cl, 1, v); }
s32 cellHttp_D5198360(PPUContext& ctx, u32 cl, u32 v) { return set_word(ctx, cl, 2, v); }
s32 cellHttp_9619B961(PPUContext& ctx, u32 cl, u32 v) { return set_word(ctx, cl, 3, v); }
s32 cellHttp_0DE7B221(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 5, out); }
s32 cellHttp_F764FD69(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 6, out); }
s32 cellHttp_C2DD615C(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 7, out); }
s32 cellHttp_832AE77D(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 8, out); }
s32 cellHttp_57738BDA(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 9, out); }
s32 cellHttp_E07FDAA2(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 10, out); }
s32 cellHttp_6869A969(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 5, v); }
s32 cellHttp_C0C891E3(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 6, v); }
s32 cellHttp_BB2B5341(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 8, v); }
s32 cellHttp_F113C441(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 9, v); }
s32 cellHttp_CF03B49F(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 10, v); }

s32 cellHttpClientGetKeepAlive(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 0, out); }
s32 cellHttpClientGetPipeline(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 1, out); }
s32 cellHttpClientGetAutoRedirect(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 2, out); }
s32 cellHttpClientGetAutoAuthentication(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 3, out); }
s32 cellHttpClientGetCookieStatus(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 4, out); }
s32 cellHttpClientGetAuthenticationCacheStatus(PPUContext& ctx, u32 cl, u32 out) { return get_switch(ctx, cl, 11, out); }
s32 cellHttpClientSetKeepAlive(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 0, v); }
s32 cellHttpClientSetPipeline(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 1, v); }
s32 cellHttpClientSetAutoRedirect(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 2, v); }
s32 cellHttpClientSetAutoAuthentication(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 3, v); }
s32 cellHttpClientSetCookieStatus(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 4, v); }
s32 cellHttpClientSetCacheStatus(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 7, v); }
s32 cellHttpClientSetAuthenticationCacheStatus(PPUContext& ctx, u32 cl, u32 v) { return set_switch(ctx, cl, 11, v); }

s32 cellHttpClientSetTotalPoolSize(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 0, v); }
s32 cellHttpClientSetPerHostPoolSize(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 1, v); }
s32 cellHttpClientSetPerHostKeepAliveMax(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 2, v); }
s32 cellHttpClientSetPerPipelineMax(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 3, v); }
s32 cellHttpClientGetTotalPoolSize(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 0, out); }
s32 cellHttpClientGetPerHostPoolSize(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 1, out); }
s32 cellHttpClientGetPerHostKeepAliveMax(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 2, out); }
s32 cellHttpClientGetPerPipelineMax(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 3, out); }

s32 cellHttp_9C902AB4(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 4, v); }
s32 cellHttp_104E9662(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 5, v); }
s32 cellHttp_561BE85B(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 6, v); }
s32 cellHttp_E9A53480(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 7, v); }
s32 cellHttp_9ED1C0B5(PPUContext& ctx, u32 cl, u32 v) { return set_limit(ctx, cl, 10, v); }
s32 cellHttp_9823EB7D(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 4, out); }
s32 cellHttp_9FE6021B(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 5, out); }
s32 cellHttp_923D3BD9(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 6, out); }
s32 cellHttp_CCFB621A(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 7, out); }
s32 cellHttp_2A9BD960(PPUContext& ctx, u32 cl, u32 out) { return get_limit(ctx, cl, 10, out); }

s32 cellHttpClientSetConnectionWaitStatus(PPUContext& ctx, u32 cl, u32 v) { return set_mswitch(ctx, cl, 1, v); }
s32 cellHttp_E1DDBD73(PPUContext& ctx, u32 cl, u32 v) { return set_mswitch(ctx, cl, 0, v); }
s32 cellHttp_15CD67D8(PPUContext& ctx, u32 cl, u32 out) { return get_mswitch(ctx, cl, 0, out); }
s32 cellHttpClientGetConnectionWaitStatus(PPUContext& ctx, u32 cl, u32 out) { return get_mswitch(ctx, cl, 1, out); }

s32 cellHttpClientGetConnectionWaitTimeout(PPUContext& ctx, u32 cl, u32 out) { return get_timeout(ctx, cl, 0, out); }
s32 cellHttpClientGetRecvTimeout(PPUContext& ctx, u32 cl, u32 out) { return get_timeout(ctx, cl, 1, out); }
s32 cellHttpClientGetSendTimeout(PPUContext& ctx, u32 cl, u32 out) { return get_timeout(ctx, cl, 2, out); }
s32 cellHttpClientGetConnTimeout(PPUContext& ctx, u32 cl, u32 out) { return get_timeout(ctx, cl, 3, out); }
s32 cellHttpClientSetConnectionWaitTimeout(PPUContext& ctx, u32 cl, u64 v) { return set_timeout(ctx, cl, 0, v); }
s32 cellHttpClientSetRecvTimeout(PPUContext& ctx, u32 cl, u64 v) { return set_timeout(ctx, cl, 1, v); }
s32 cellHttpClientSetSendTimeout(PPUContext& ctx, u32 cl, u64 v) { return set_timeout(ctx, cl, 2, v); }
s32 cellHttpClientSetConnTimeout(PPUContext& ctx, u32 cl, u64 v) { return set_timeout(ctx, cl, 3, v); }

s32 cellHttpClientGetSendBufferSize(PPUContext& ctx, u32 cl, u32 out)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_get_buffer(ctx, mgr, m::kSendBuf, out); });
}
s32 cellHttpClientSetSendBufferSize(PPUContext& ctx, u32 cl, u32 v)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_set_buffer(ctx, mgr, m::kSendBuf, v); });
}
s32 cellHttpClientGetRecvBufferSize(PPUContext& ctx, u32 cl, u32 out)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_get_buffer(ctx, mgr, m::kRecvBuf, out); });
}
s32 cellHttpClientSetRecvBufferSize(PPUContext& ctx, u32 cl, u32 v)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_set_buffer(ctx, mgr, m::kRecvBuf, v); });
}

s32 cellHttpClientGetSslVersion(PPUContext& ctx, u32 cl, u32 out)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_ssl_version(ctx, mgr, out); });
}
s32 cellHttpClientSetSslVersion(PPUContext& ctx, u32 cl, u32 v)
{
    return with_manager(ctx, cl, [&](u32 mgr) { return manager_set_ssl_version(ctx, mgr, v); });
}

// The minimum SSL version: 0 is version 0, 1 is version 2 (and reads back as 1); anything else is invalid.
s32 cellHttpClientGetMinSslVersion(PPUContext& ctx, u32 cl, u32 out)
{
    StackVar v(ctx, gb(ctx), 8);
    const s32 r = cellHttpClientGetSslVersion(ctx, cl, v.addr());
    if (r < 0) return r;
    const u32 version = r32(ctx, v.addr());
    if (!usable(ctx, out, 4, "cellHttpClientGetMinSslVersion")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    w32(ctx, out, version == 2 ? 1 : version);
    return r;
}
s32 cellHttpClientSetMinSslVersion(PPUContext& ctx, u32 cl, u32 v)
{
    if (v == 0) return cellHttpClientSetSslVersion(ctx, cl, 0);
    if (v == 1) return cellHttpClientSetSslVersion(ctx, cl, 2);
    return err(CELL_HTTP_ERROR_INVALID_VALUE);
}

// cellHttpClientSetConnectionStateCallback: the one client function that doesn't ask for cellHttpInit first.
s32 cellHttpClientSetConnectionStateCallback(PPUContext& ctx, u32 cl, u32 cb, u32 arg)
{
    if (!valid(ctx, cl, c::kSelf)) return err(CELL_HTTP_ERROR_BAD_CLIENT);
    u32 mgr = 0;
    if (const s32 r = client_manager(ctx, cl, mgr); r < 0) return r;
    return manager_set_state_callback(ctx, mgr, cb, arg);
}

// The client's callbacks (function, argument).
s32 set_callback(PPUContext& ctx, u32 cl, u32 field, u32 cb, u32 arg)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    w32(ctx, cl + field + 4, arg);
    w32(ctx, cl + field, cb);
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}
s32 cellHttpClientSetAuthenticationCallback(PPUContext& ctx, u32 cl, u32 f, u32 a)
{
    return set_callback(ctx, cl, c::kAuthCb, f, a);
}
s32 cellHttpClientSetTransactionStateCallback(PPUContext& ctx, u32 cl, u32 f, u32 a)
{
    return set_callback(ctx, cl, c::kTransStateCb, f, a);
}
s32 cellHttpClientSetRedirectCallback(PPUContext& ctx, u32 cl, u32 f, u32 a)
{
    return set_callback(ctx, cl, c::kRedirectCb, f, a);
}
s32 cellHttpClientSetCookieSendCallback(PPUContext& ctx, u32 cl, u32 f, u32 a)
{
    return set_callback(ctx, cl, c::kCookieSendCb, f, a);
}
s32 cellHttpClientSetCookieRecvCallback(PPUContext& ctx, u32 cl, u32 f, u32 a)
{
    return set_callback(ctx, cl, c::kCookieRecvCb, f, a);
}
s32 cellHttpClientSetSslCallback(PPUContext& ctx, u32 cl, u32 f, u32 a) { return set_callback(ctx, cl, c::kSslCb, f, a); }
s32 cellHttpClientSetSslIdDestroyCallback(PPUContext& ctx, u32 cl, u32 f, u32 a)
{
    return set_callback(ctx, cl, c::kSslIdDestroyCb, f, a);
}

// cellHttpClientGetVersion(clientId, u32* major, u32* minor) / SetVersion: any numbers.
s32 cellHttpClientGetVersion(PPUContext& ctx, u32 cl, u32 major, u32 minor)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!major || !minor) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, cl + c::kLock); r < 0) return r;
    if (usable(ctx, major, 4, "cellHttpClientGetVersion: major") && usable(ctx, minor, 4, "cellHttpClientGetVersion: minor")) {
        w32(ctx, major, r32(ctx, cl + c::kMajor));
        w32(ctx, minor, r32(ctx, cl + c::kMinor));
    }
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}
s32 cellHttpClientSetVersion(PPUContext& ctx, u32 cl, u32 major, u32 minor)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    w32(ctx, cl + c::kMinor, minor);
    w32(ctx, cl + c::kMajor, major);
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}

// cellHttpClientGetUserAgent(clientId, char* buf, u32 size, u32* required): with no buffer only *required (the size
// with its NUL); else the text and its NUL, then (libhttp's own) one more NUL past it, so a buffer of exactly the size
// gets a byte written after its end.
s32 cellHttpClientGetUserAgent(PPUContext& ctx, u32 cl, u32 buf, u32 size, u32 required)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (const s32 r = lw_lock(ctx, cl + c::kLock); r < 0) return r;
    const u32 n = r32(ctx, cl + c::kUaSize);
    s32 r = kOk;
    if (!buf || !size) {
        if (!required) r = err(CELL_HTTP_ERROR_NO_BUFFER);
        else if (usable(ctx, required, 4, "cellHttpClientGetUserAgent: required")) w32(ctx, required, n);
    } else {
        if (required && usable(ctx, required, 4, "cellHttpClientGetUserAgent: required")) w32(ctx, required, n);
        if (n > size) {
            r = err(CELL_HTTP_ERROR_INSUFFICIENT);
        } else if (usable(ctx, buf, n + 1, "cellHttpClientGetUserAgent: buf")) {
            copy(ctx, buf, r32(ctx, cl + c::kUa), n);
            w8(ctx, buf + n, 0);
        }
    }
    lw_unlock(ctx, cl + c::kLock);
    return r;
}

s32 cellHttpClientSetUserAgent(PPUContext& ctx, u32 cl, u32 ua)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!ua) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 self = r32(ctx, cl + c::kSelf);
    const u32 n = str_len(ctx, ua);
    const u32 copy_block = hmalloc(ctx, n + 1);
    if (!copy_block) return err(CELL_HTTP_ERROR_NO_MEMORY);
    copy(ctx, copy_block, ua, n);
    w8(ctx, copy_block + n, 0);
    if (const s32 r = lw_lock(ctx, self + c::kLock); r < 0) {
        hfree(ctx, copy_block);
        return r;
    }
    if (const u32 old = r32(ctx, cl + c::kUa)) hfree(ctx, old);
    w32(ctx, cl + c::kUaSize, n + 1);
    w32(ctx, cl + c::kUa, copy_block);
    lw_unlock(ctx, self + c::kLock);
    return kOk;
}

// ---- headers

s32 cellHttpClientAddHeader(PPUContext& ctx, u32 cl, u32 header)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!header) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 self = r32(ctx, cl + c::kSelf);
    if (const s32 r = lw_lock(ctx, self + c::kLock); r < 0) return r;
    const s32 r = headers_add(ctx, self + c::kHeaders, header);
    if (r == kOk) w32(ctx, cl + c::kHeaderCount, r32(ctx, cl + c::kHeaderCount) + 1);
    lw_unlock(ctx, self + c::kLock);
    return r;
}

// cellHttpClientSetHeader: replaces the value of the first header of that name (case-insensitive), or adds one.
s32 cellHttpClientSetHeader(PPUContext& ctx, u32 cl, u32 header)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!header) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 self = r32(ctx, cl + c::kSelf);
    if (const s32 r = lw_lock(ctx, self + c::kLock); r < 0) return r;
    const u32 name = r32(ctx, header);
    const u32 n = str_len(ctx, name) + 1;
    u32 node = r32(ctx, cl + c::kHeaders);
    while (node && str_ncasecmp(ctx, r32(ctx, node + hnode::kName), name, n) != 0) node = r32(ctx, node + hnode::kNext);
    s32 r = kOk;
    if (!node) {
        r = headers_add(ctx, self + c::kHeaders, header);
        if (r == kOk) w32(ctx, cl + c::kHeaderCount, r32(ctx, cl + c::kHeaderCount) + 1);
    } else {
        const u32 old_pool = r32(ctx, node + hnode::kPool), old_name = r32(ctx, node + hnode::kName),
                  old_value = r32(ctx, node + hnode::kValue);
        StackVar need(ctx, gb(ctx), 8);
        r = util(ctx, "cellHttpUtilCopyHeader", {0, header, 0, 0, need.addr()});
        if (r == kOk) {
            const u32 pool = hmalloc(ctx, r32(ctx, need.addr()));
            w32(ctx, node + hnode::kPool, pool);
            if (!pool) {
                w32(ctx, node + hnode::kPool, old_pool);
                r = err(CELL_HTTP_ERROR_NO_MEMORY);
            } else {
                r = util(ctx, "cellHttpUtilCopyHeader", {node + hnode::kHeader, header, pool, r32(ctx, need.addr()), 0});
                if (r == kOk) {
                    hfree(ctx, old_pool);
                } else {
                    hfree(ctx, pool);
                    w32(ctx, node + hnode::kValue, old_value);
                    w32(ctx, node + hnode::kPool, old_pool);
                    w32(ctx, node + hnode::kName, old_name);
                }
            }
        }
    }
    lw_unlock(ctx, self + c::kLock);
    return r;
}

s32 cellHttpClientGetHeader(PPUContext& ctx, u32 cl, u32 header, u32 name, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!name) return err(CELL_HTTP_ERROR_NO_STRING);
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    StackVar found(ctx, gb(ctx), 8);
    s32 r = headers_find(ctx, r32(ctx, cl + c::kHeaders), found.addr(), name);
    if (r == kOk) r = util(ctx, "cellHttpUtilCopyHeader", {header, r32(ctx, found.addr()), pool, size, required});
    lw_unlock(ctx, cl + c::kLock);
    return r;
}

// cellHttpClientDeleteHeader: the first header of that name. Removing the first node leaves the next one's back link
// as it was (libhttp's).
s32 cellHttpClientDeleteHeader(PPUContext& ctx, u32 cl, u32 name)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (!name) return err(CELL_HTTP_ERROR_NO_STRING);
    const u32 n = str_len(ctx, name) + 1;
    if (const s32 r = lw_lock(ctx, r32(ctx, cl + c::kSelf) + c::kLock); r < 0) return r;
    u32 node = r32(ctx, cl + c::kHeaders);
    while (node && str_ncasecmp(ctx, r32(ctx, node + hnode::kName), name, n) != 0) node = r32(ctx, node + hnode::kNext);
    s32 r = kOk;
    if (!node) {
        r = err(CELL_HTTP_ERROR_NO_HEADER);
    } else {
        if (r32(ctx, cl + c::kHeaders) == node) {
            w32(ctx, cl + c::kHeaders, r32(ctx, node + hnode::kNext));
        } else {
            const u32 prev = r32(ctx, node + hnode::kPrev), next = r32(ctx, node + hnode::kNext);
            if (prev) w32(ctx, prev + hnode::kNext, next);
            if (next) w32(ctx, next + hnode::kPrev, prev);
        }
        w32(ctx, node + hnode::kNext, 0);
        w32(ctx, node + hnode::kPrev, 0);
        zero(ctx, node + hnode::kHeader, 8);
        hfree(ctx, r32(ctx, node + hnode::kPool));
        w32(ctx, node + hnode::kPool, 0);
        hfree(ctx, node);
        w32(ctx, cl + c::kHeaderCount, r32(ctx, cl + c::kHeaderCount) - 1);
    }
    lw_unlock(ctx, cl + c::kLock);
    return r;
}

// cellHttpClientGetAllHeaders(clientId, CellHttpHeader** headers, u32* items, void* pool, u32 poolSize,
// u32* required): the array of CellHttpHeader at the pool's start, the strings after it. A pool too small for the
// array answers CELL_OK with nothing written (libhttp's); a header that doesn't fit is left out, its error dropped.
s32 cellHttpClientGetAllHeaders(PPUContext& ctx, u32 cl, u32 headers, u32 items, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if ((!pool || !headers) && !required) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, cl + c::kLock); r < 0) return r;
    const u32 count = r32(ctx, cl + c::kHeaderCount);
    const u32 array = count * 8;
    u32 strings = pool;
    StackVar used(ctx, gb(ctx), 8);
    if (pool && size) {
        if (array > size) {
            lw_unlock(ctx, cl + c::kLock);
            return kOk;
        }
        size -= array;
        strings = pool + array;
    }
    headers_copy_all(ctx, r32(ctx, cl + c::kHeaders), pool, strings, size, used.addr());
    if (items && usable(ctx, items, 4, "cellHttpClientGetAllHeaders: items")) w32(ctx, items, count);
    if (required && usable(ctx, required, 4, "cellHttpClientGetAllHeaders: required"))
        w32(ctx, required, array + r32(ctx, used.addr()));
    if (headers && usable(ctx, headers, 4, "cellHttpClientGetAllHeaders: headers")) w32(ctx, headers, pool);
    lw_unlock(ctx, cl + c::kLock);
    return kOk;
}

// ---- the proxy

// cellHttpClientSetProxy(clientId, const CellHttpUri* proxy): as cellHttpSetProxy, into the client; setting one closes
// the manager's idle connections.
s32 cellHttpClientSetProxy(PPUContext& ctx, u32 cl, u32 proxy)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    const u32 self = r32(ctx, cl + c::kSelf);
    if (!proxy) {
        if (const s32 r = lw_lock(ctx, self + c::kLock); r < 0) return r;
        w8(ctx, cl + c::kUseProxy, 0);
        if (const u32 p = r32(ctx, cl + c::kProxyPool)) {
            hfree(ctx, p);
            w32(ctx, cl + c::kProxyPool, 0);
            zero(ctx, self + c::kProxy, 28);
        }
        lw_unlock(ctx, self + c::kLock);
        return kOk;
    }
    if (!usable(ctx, proxy, 28, "cellHttpClientSetProxy: proxy")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 host = r32(ctx, proxy + 4);
    if (!host || !r8(ctx, host)) return err(CELL_HTTP_ERROR_INVALID_URI);
    StackVar uri(ctx, gb(ctx), 32);
    u32 pool = 0, parse_pool = 0;
    if (const s32 r = proxy_prepare(ctx, proxy, uri.addr(), pool, parse_pool); r < 0) return r;
    const s32 r = lw_lock(ctx, cl + c::kLock);
    if (r < 0) {
        hfree(ctx, pool);
    } else {
        if (const u32 old = r32(ctx, cl + c::kProxyPool)) {
            hfree(ctx, old);
            w32(ctx, cl + c::kProxyPool, 0);
        }
        copy(ctx, cl + c::kProxy, uri.addr(), 24);
        w8(ctx, cl + c::kUseProxy, 1);
        w32(ctx, cl + c::kProxyPool, pool);
        const u32 mgr = r32(ctx, cl + c::kManager);
        lw_unlock(ctx, cl + c::kLock);
        manager_close_all(ctx, mgr);
    }
    hfree(ctx, parse_pool);
    return r;
}

s32 cellHttpClientGetProxy(PPUContext& ctx, u32 cl, u32 proxy, u32 pool, u32 size, u32 required)
{
    if (const s32 r = check_client(ctx, cl); r < 0) return r;
    if (const s32 r = lw_lock(ctx, cl + c::kLock); r < 0) return r;
    s32 r = kOk;
    if (!r8(ctx, cl + c::kUseProxy)) {
        if (required) w32(ctx, required, 0);
    } else {
        r = util(ctx, "cellHttpUtilCopyUri", {proxy, cl + c::kProxy, pool, size, required});
    }
    lw_unlock(ctx, cl + c::kLock);
    return r;
}

} // namespace

s32 client_ssl_version(PPUContext& ctx, u32 client, u32 out) { return cellHttpClientGetSslVersion(ctx, client, out); }

void register_clients(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0x4E4EE53A, cellHttpCreateClient),
        PS_HLE_FUNC(0x980855AC, cellHttpDestroyClient),
        PS_HLE_FUNC(0x6884CDB7, cellHttpClientGetResponseBufferMax),
        PS_HLE_FUNC(0xADD66B5C, cellHttpClientSetResponseBufferMax),
        PS_HLE_FUNC(0x591C21A8, cellHttpClientGetKeepAlive),
        PS_HLE_FUNC(0x2A1F28F6, cellHttpClientGetPipeline),
        PS_HLE_FUNC(0x2960E309, cellHttpClientGetAutoRedirect),
        PS_HLE_FUNC(0x5980A293, cellHttpClientGetAutoAuthentication),
        PS_HLE_FUNC(0xEB9C1E5E, cellHttpClientGetCookieStatus),
        PS_HLE_FUNC(0xFCE39343, cellHttpClientGetAuthenticationCacheStatus),
        PS_HLE_FUNC(0x5D473170, cellHttpClientSetKeepAlive),
        PS_HLE_FUNC(0x296A46CF, cellHttpClientSetPipeline),
        PS_HLE_FUNC(0x211D8BA3, cellHttpClientSetAutoRedirect),
        PS_HLE_FUNC(0x8EAF47A3, cellHttpClientSetAutoAuthentication),
        PS_HLE_FUNC(0x434419C8, cellHttpClientSetCookieStatus),
        PS_HLE_FUNC(0x88F79869, cellHttpClientSetCacheStatus),
        PS_HLE_FUNC(0x6EED4999, cellHttpClientSetAuthenticationCacheStatus),
        PS_HLE_FUNC(0x8AA5FCD3, cellHttpClientSetTotalPoolSize),
        PS_HLE_FUNC(0xAB1C55AB, cellHttpClientSetPerHostPoolSize),
        PS_HLE_FUNC(0x595ADEE9, cellHttpClientSetPerHostKeepAliveMax),
        PS_HLE_FUNC(0xDC7ED599, cellHttpClientSetPerPipelineMax),
        PS_HLE_FUNC(0x070F1020, cellHttpClientGetTotalPoolSize),
        PS_HLE_FUNC(0xFFC74003, cellHttpClientGetPerHostPoolSize),
        PS_HLE_FUNC(0x46BCC9FF, cellHttpClientGetPerHostKeepAliveMax),
        PS_HLE_FUNC(0xD06C90A4, cellHttpClientGetPerPipelineMax),
        PS_HLE_FUNC(0x88B71A4C, cellHttpClientSetConnectionWaitStatus),
        PS_HLE_FUNC(0x431E1407, cellHttpClientGetConnectionWaitStatus),
        PS_HLE_FUNC(0x1249C1D1, cellHttpClientGetConnectionWaitTimeout),
        PS_HLE_FUNC(0xBA78E51F, cellHttpClientGetRecvTimeout),
        PS_HLE_FUNC(0x271A0B06, cellHttpClientGetSendTimeout),
        PS_HLE_FUNC(0x14BFC765, cellHttpClientGetConnTimeout),
        PS_HLE_FUNC(0xB7CB7D05, cellHttpClientSetConnectionWaitTimeout),
        PS_HLE_FUNC(0x224E1610, cellHttpClientSetRecvTimeout),
        PS_HLE_FUNC(0x71714CDC, cellHttpClientSetSendTimeout),
        PS_HLE_FUNC(0xD7471088, cellHttpClientSetConnTimeout),
        PS_HLE_FUNC(0xD3FCD411, cellHttpClientGetSendBufferSize),
        PS_HLE_FUNC(0xBB6C0EB2, cellHttpClientSetSendBufferSize),
        PS_HLE_FUNC(0x130150EA, cellHttpClientGetRecvBufferSize),
        PS_HLE_FUNC(0xBF6E3659, cellHttpClientSetRecvBufferSize),
        PS_HLE_FUNC(0xCCF57336, cellHttpClientGetSslVersion),
        PS_HLE_FUNC(0x65691795, cellHttpClientSetSslVersion),
        PS_HLE_FUNC(0x98CE061C, cellHttpClientGetMinSslVersion),
        PS_HLE_FUNC(0xE6C7D333, cellHttpClientSetMinSslVersion),
        PS_HLE_FUNC(0x85FB7363, cellHttpClientSetConnectionStateCallback),
        PS_HLE_FUNC(0x660D42A9, cellHttpClientSetAuthenticationCallback),
        PS_HLE_FUNC(0xB6FEB84B, cellHttpClientSetTransactionStateCallback),
        PS_HLE_FUNC(0x473CD9F1, cellHttpClientSetRedirectCallback),
        PS_HLE_FUNC(0x4D915204, cellHttpClientSetCookieSendCallback),
        PS_HLE_FUNC(0x13FE767B, cellHttpClientSetCookieRecvCallback),
        PS_HLE_FUNC(0x1395D8D1, cellHttpClientSetSslCallback),
        PS_HLE_FUNC(0x7313C78D, cellHttpClientSetSslIdDestroyCallback),
        PS_HLE_FUNC(0xDC405507, cellHttpClientGetVersion),
        PS_HLE_FUNC(0x40547D8B, cellHttpClientSetVersion),
        PS_HLE_FUNC(0xEE05B0C1, cellHttpClientGetUserAgent),
        PS_HLE_FUNC(0xCAC9FC34, cellHttpClientSetUserAgent),
        PS_HLE_FUNC(0x4B33942A, cellHttpClientAddHeader),
        PS_HLE_FUNC(0xA34C4B6F, cellHttpClientSetHeader),
        PS_HLE_FUNC(0xD1EC0B25, cellHttpClientGetHeader),
        PS_HLE_FUNC(0x617EEC02, cellHttpClientDeleteHeader),
        PS_HLE_FUNC(0x0D9C65BE, cellHttpClientGetAllHeaders),
        PS_HLE_FUNC(0xD7D3CD5D, cellHttpClientSetProxy),
        PS_HLE_FUNC(0x4D40CF98, cellHttpClientGetProxy),
        // libhttp's exports with no known name
        PS_HLE_FUNC(0xD5C9C1DF, cellHttp_D5C9C1DF),
        PS_HLE_FUNC(0x415087D1, cellHttp_415087D1),
        PS_HLE_FUNC(0x44A86F38, cellHttp_44A86F38),
        PS_HLE_FUNC(0x42D600B1, cellHttp_42D600B1),
        PS_HLE_FUNC(0xD5198360, cellHttp_D5198360),
        PS_HLE_FUNC(0x9619B961, cellHttp_9619B961),
        PS_HLE_FUNC(0x0DE7B221, cellHttp_0DE7B221),
        PS_HLE_FUNC(0xF764FD69, cellHttp_F764FD69),
        PS_HLE_FUNC(0xC2DD615C, cellHttp_C2DD615C),
        PS_HLE_FUNC(0x832AE77D, cellHttp_832AE77D),
        PS_HLE_FUNC(0x57738BDA, cellHttp_57738BDA),
        PS_HLE_FUNC(0xE07FDAA2, cellHttp_E07FDAA2),
        PS_HLE_FUNC(0x6869A969, cellHttp_6869A969),
        PS_HLE_FUNC(0xC0C891E3, cellHttp_C0C891E3),
        PS_HLE_FUNC(0xBB2B5341, cellHttp_BB2B5341),
        PS_HLE_FUNC(0xF113C441, cellHttp_F113C441),
        PS_HLE_FUNC(0xCF03B49F, cellHttp_CF03B49F),
        PS_HLE_FUNC(0x9C902AB4, cellHttp_9C902AB4),
        PS_HLE_FUNC(0x104E9662, cellHttp_104E9662),
        PS_HLE_FUNC(0x561BE85B, cellHttp_561BE85B),
        PS_HLE_FUNC(0xE9A53480, cellHttp_E9A53480),
        PS_HLE_FUNC(0x9ED1C0B5, cellHttp_9ED1C0B5),
        PS_HLE_FUNC(0x9823EB7D, cellHttp_9823EB7D),
        PS_HLE_FUNC(0x9FE6021B, cellHttp_9FE6021B),
        PS_HLE_FUNC(0x923D3BD9, cellHttp_923D3BD9),
        PS_HLE_FUNC(0xCCFB621A, cellHttp_CCFB621A),
        PS_HLE_FUNC(0x2A9BD960, cellHttp_2A9BD960),
        PS_HLE_FUNC(0xE1DDBD73, cellHttp_E1DDBD73),
        PS_HLE_FUNC(0x15CD67D8, cellHttp_15CD67D8),
    });
}

} // namespace pspocket::hle::http
