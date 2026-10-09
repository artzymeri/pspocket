// cellHttp's connections (D-524): the connection objects a client's manager keeps, the per-host records that count
// them, and their sockets (libnet's, pspocket's own sys_net), as libhttp.sprx keeps them. See cellHttp.cpp for the
// method and cellHttp.hpp for the layouts.
//
// A connection (libhttp's, 152 bytes): its host key {name, its copy, length, port} at +0, +20/+24 a buffer, the socket
// at +40 (a sockaddr at +40, the resolver at +84, the SSL object at +88, the descriptor at +92, the SSL switch +121),
// pending transactions +128 (their count +132), +140/+141 switches, +144, and its own address at +148.
// A host list (in the manager, 12 bytes): connection nodes +0, host entries +4, a count +8. A host entry (12 bytes:
// next, previous, record) leads to a host record (64 bytes): the lv2 mutex and condition "_httsyn" at +0, the host
// key at +24, transaction nodes +44 (their count +52), +56.

#include "cellHttp.hpp"

#include "pspocket/core/log.hpp"

namespace pspocket::hle::http {

namespace m = manager;

namespace {

// The socket (libhttp's, at a connection's +40) closed: its SSL object first, then shutdown and socketclose.
void socket_close(PPUContext& ctx, u32 sock, u32 ssl_record)
{
    if (r8(ctx, sock + 81)) socket_ssl_close(ctx, sock, ssl_record);
    const s32 fd = static_cast<s32>(r32(ctx, sock + 52));
    if (fd != -1) {
        call(ctx, "sys_net", "shutdown", {static_cast<u64>(static_cast<s64>(fd)), 2});
        call(ctx, "sys_net", "socketclose", {static_cast<u64>(static_cast<s64>(r32(ctx, sock + 52)))});
        w32(ctx, sock + 52, 0xFFFFFFFF);
    }
}

} // namespace

bool connection_valid(PPUContext& ctx, u32 conn) { return valid(ctx, conn, conn::kSelf); }

bool connection_flag140(PPUContext& ctx, u32 conn)
{
    if (!page_ok(ctx, conn) || r32(ctx, conn + conn::kSelf) != conn) return false;
    return r8(ctx, conn + 140) != 0;
}

bool connection_flag141(PPUContext& ctx, u32 conn)
{
    if (!page_ok(ctx, conn) || r32(ctx, conn + conn::kSelf) != conn) return false;
    return r8(ctx, conn + 141) != 0;
}

s32 connection_destroy(PPUContext& ctx, u32 conn, u32 ssl_record)
{
    if (!connection_valid(ctx, conn)) return err(CELL_HTTP_ERROR_BAD_CONN);
    socket_close(ctx, conn + conn::kSocket, ssl_record);
    w32(ctx, conn + conn::kSelf, 0);
    while (const u32 node = r32(ctx, conn + 128)) {
        w32(ctx, conn + 128, r32(ctx, node));
        w32(ctx, node, 0);
        w32(ctx, node + 4, 0);
        hfree(ctx, node);
        w32(ctx, conn + 132, r32(ctx, conn + 132) - 1);
    }
    if (const u32 p = r32(ctx, conn + 4)) {
        hfree(ctx, p);
        w32(ctx, conn + 4, 0);
    }
    w32(ctx, conn + 0, 0);
    if (const u32 p = r32(ctx, conn + 24)) {
        hfree(ctx, p);
        w32(ctx, conn + 24, 0);
    }
    w32(ctx, conn + 20, 0);
    hfree(ctx, conn);
    return kOk;
}

// A host key {name, its copy, length, port}: copied (the name into a block of its length), compared, freed.
s32 key_copy(PPUContext& ctx, u32 dst, u32 src)
{
    if (!dst) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 len = r32(ctx, src + 8);
    const u32 buf = hmalloc(ctx, len);
    w32(ctx, dst + 4, buf);
    if (!buf) return err(CELL_HTTP_ERROR_NO_MEMORY);
    // _sys_strncpy: the name's bytes up to its NUL, then NULs to the length.
    const u32 name = r32(ctx, src);
    u32 i = 0;
    for (; i < len && r8(ctx, name + i); i++) w8(ctx, buf + i, r8(ctx, name + i));
    for (; i < len; i++) w8(ctx, buf + i, 0);
    w32(ctx, dst + 12, r32(ctx, src + 12));
    w32(ctx, dst + 0, r32(ctx, dst + 4));
    w32(ctx, dst + 8, r32(ctx, src + 8));
    return kOk;
}

bool key_equal(PPUContext& ctx, u32 a, u32 b)
{
    if (!a || !b) return false;
    const u32 la = r32(ctx, a + 8), lb = r32(ctx, b + 8);
    if (str_ncasecmp(ctx, r32(ctx, a), r32(ctx, b), lb < la ? lb : la) != 0) return false;
    return r32(ctx, a + 12) == r32(ctx, b + 12);
}

void key_free(PPUContext& ctx, u32 key)
{
    if (!key) return;
    if (const u32 p = r32(ctx, key + 4)) {
        hfree(ctx, p);
        w32(ctx, key + 4, 0);
    }
    w32(ctx, key + 12, 0);
    w32(ctx, key + 0, 0);
    w32(ctx, key + 8, 0);
}

// A host record found by key in a host list, or made (an entry and a 64-byte record with its mutex and condition, the
// key copied in) and appended.
s32 host_find_or_add(PPUContext& ctx, u32 list, u32 key, u32 out)
{
    if (!list) return err(CELL_HTTP_ERROR_NO_POOL);
    for (u32 e = r32(ctx, list + 4); e; e = r32(ctx, e)) {
        const u32 rec = r32(ctx, e + 8);
        if (rec && key_equal(ctx, rec + 24, key)) {
            w32(ctx, out, rec);
            return kOk;
        }
    }
    const u32 e = hmalloc(ctx, 12);
    if (!e) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, e, 12);
    const u32 rec = hmalloc(ctx, 64);
    w32(ctx, e + 8, rec);
    if (!rec) {
        hfree(ctx, e);
        return err(CELL_HTTP_ERROR_NO_MEMORY);
    }
    zero(ctx, rec, 64);
    s32 r = sync_create(ctx, rec, 0);
    if (r >= 0) {
        r = key_copy(ctx, rec + 24, key);
        if (r < 0) sync_destroy(ctx, rec);
    }
    if (r < 0) {
        hfree(ctx, r32(ctx, e + 8));
        w32(ctx, e + 8, 0);
        hfree(ctx, e);
        return r;
    }
    u32 last = r32(ctx, list + 4);
    if (!last) {
        w32(ctx, list + 4, e);
    } else {
        while (r32(ctx, last)) last = r32(ctx, last);
        w32(ctx, last, e);
        w32(ctx, e + 4, last);
        w32(ctx, e, 0);
    }
    w32(ctx, out, r32(ctx, e + 8));
    return kOk;
}

// The host record of a connection's host in a list (made when there is none).
s32 host_of(PPUContext& ctx, u32 conn, u32 list, u32 out)
{
    StackVar key(ctx, gb(ctx), 16);
    s32 r = connection_valid(ctx, conn) ? key_copy(ctx, key.addr(), conn) : err(CELL_HTTP_ERROR_BAD_CONN);
    if (r == kOk) {
        r = host_find_or_add(ctx, list, key.addr(), out);
        key_free(ctx, key.addr());
    }
    return r;
}

// A connection taken out of a host list (its node and its host record's transaction node for it):
// CELL_HTTP_ERROR_NO_CONNECTION when the list doesn't hold it.
s32 list_take(PPUContext& ctx, u32 list, u32 conn)
{
    u32 node = r32(ctx, list);
    while (node && r32(ctx, node + 8) != conn) node = r32(ctx, node);
    if (!node) return err(CELL_HTTP_ERROR_NO_CONNECTION);
    if (r32(ctx, list) == node) {
        w32(ctx, list, r32(ctx, node));
    } else {
        const u32 prev = r32(ctx, node + 4), next = r32(ctx, node);
        if (prev) w32(ctx, prev, next);
        if (next) w32(ctx, next + 4, prev);
    }
    w32(ctx, node + 4, 0);
    w32(ctx, node, 0);
    StackVar rec(ctx, gb(ctx), 8);
    const s32 r = host_of(ctx, conn, list, rec.addr());
    if (r != kOk) {
        // Back at the end of the list.
        u32 last = r32(ctx, list);
        if (!last) {
            w32(ctx, list, node);
        } else {
            while (r32(ctx, last)) last = r32(ctx, last);
            w32(ctx, last, node);
            w32(ctx, node + 4, last);
            w32(ctx, node, 0);
        }
        return r;
    }
    hfree(ctx, node);
    w32(ctx, list + 8, r32(ctx, list + 8) - 1);
    const u32 h = r32(ctx, rec.addr());
    u32 t = r32(ctx, h + 44);
    while (t && r32(ctx, t + 8) != conn) t = r32(ctx, t);
    if (!t) return kOk; // libhttp's: the record not holding it is no error
    if (r32(ctx, h + 44) == t) {
        w32(ctx, h + 44, r32(ctx, t));
    } else {
        const u32 prev = r32(ctx, t + 4), next = r32(ctx, t);
        if (prev) w32(ctx, prev, next);
        if (next) w32(ctx, next + 4, prev);
    }
    w32(ctx, t + 4, 0);
    w32(ctx, t, 0);
    hfree(ctx, t);
    w32(ctx, h + 52, r32(ctx, h + 52) - 1);
    if (connection_flag141(ctx, conn)) w32(ctx, h + 56, r32(ctx, h + 56) - 1);
    return kOk;
}

// A host record freed: its key's copy, its transaction nodes, its mutex and condition.
void host_free(PPUContext& ctx, u32 rec)
{
    hfree(ctx, r32(ctx, rec + 28));
    w32(ctx, rec + 28, 0);
    w32(ctx, rec + 24, 0);
    while (const u32 t = r32(ctx, rec + 44)) {
        w32(ctx, rec + 44, r32(ctx, t));
        w32(ctx, rec + 52, r32(ctx, rec + 52) - 1);
        w32(ctx, t + 4, 0);
        w32(ctx, t, 0);
        hfree(ctx, t);
    }
    w32(ctx, rec + 56, 0);
    sync_destroy(ctx, rec);
    hfree(ctx, rec);
}

// A host list emptied (the manager's destruction): its connections destroyed (one that can't be stays, at the end, and
// its error is the answer), then every host record.
s32 hosts_clear(PPUContext& ctx, u32 list, u32 ssl_record)
{
    if (!list) return kOk;
    while (const u32 node = r32(ctx, list)) {
        w32(ctx, list, r32(ctx, node));
        w32(ctx, node + 4, 0);
        w32(ctx, node, 0);
        const s32 r = connection_destroy(ctx, r32(ctx, node + 8), ssl_record);
        if (r != kOk) {
            u32 last = r32(ctx, list);
            if (!last) {
                w32(ctx, list, node);
            } else {
                while (r32(ctx, last)) last = r32(ctx, last);
                w32(ctx, last, node);
                w32(ctx, node + 4, last);
                w32(ctx, node, 0);
            }
            return r;
        }
        w32(ctx, list + 8, r32(ctx, list + 8) - 1);
        hfree(ctx, node);
    }
    for (u32 e = r32(ctx, list + 4); e;) {
        const u32 next = r32(ctx, e);
        w32(ctx, e + 4, 0);
        w32(ctx, e, 0);
        host_free(ctx, r32(ctx, e + 8));
        w32(ctx, e + 8, 0);
        hfree(ctx, e);
        e = next;
    }
    w32(ctx, list + 4, 0);
    return kOk;
}

// A host list's idle connections closed, then its host records with no transactions freed.
s32 hosts_close(PPUContext& ctx, u32 list, u32 ssl_record)
{
    if (!list) return kOk;
    for (u32 node = r32(ctx, list); node;) {
        const u32 conn = r32(ctx, node + 8), next = r32(ctx, node);
        if (const s32 r = list_take(ctx, list, conn); r < 0) return r;
        if (const s32 r = connection_destroy(ctx, conn, ssl_record); r != kOk) return r;
        node = next;
    }
    for (u32 e = r32(ctx, list + 4); e;) {
        const u32 next = r32(ctx, e);
        const u32 rec = r32(ctx, e + 8);
        if (r32(ctx, rec + 52) == 0) {
            if (r32(ctx, list + 4) == e) w32(ctx, list + 4, next);
            const u32 prev = r32(ctx, e + 4);
            if (prev) w32(ctx, prev, r32(ctx, e));
            if (r32(ctx, e)) w32(ctx, r32(ctx, e) + 4, prev);
            hfree(ctx, r32(ctx, rec + 28));
            w32(ctx, e, 0);
            w32(ctx, e + 4, 0);
            w32(ctx, rec + 28, 0);
            w32(ctx, rec + 24, 0);
            sync_destroy(ctx, rec);
            hfree(ctx, r32(ctx, e + 8));
            w32(ctx, e + 8, 0);
            hfree(ctx, e);
        }
        e = next;
    }
    return kOk;
}

s32 manager_close_all(PPUContext& ctx, u32 mgr)
{
    if (!valid(ctx, mgr, m::kSelf)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    if (r8(ctx, mgr + m::k161)) hosts_close(ctx, mgr + m::kHosts2, r32(ctx, mgr + m::k132));
    const s32 r = hosts_close(ctx, mgr + m::kHosts, r32(ctx, mgr + m::k132));
    lw_unlock(ctx, mgr + m::kLock);
    return r;
}

namespace {

// ------------------------------------------------------------------------------------------------- making connections

// The socket of a new connection (at its +40, 88 bytes): the client +40, the timeouts for receiving +16, sending +24
// and connecting +32 (u64), no resolver yet (+44, -1), the SSL object +48, the descriptor +52, the SSL version +60
// (and whether it is 1, +64), the buffer size +68, the receive and send buffer sizes +72/+76, the SSL switch +81, +82;
// libnet's socket made (a TCP one).
s32 socket_open(PPUContext& ctx, u32 sock, u8 ssl, u64 conn_timeout, u64 recv_timeout, u64 send_timeout, u32 recv_buf,
    u32 send_buf, u32 ssl_version, u32 buf_size, u32 client)
{
    zero(ctx, sock, 88);
    w32(ctx, sock + 72, recv_buf);
    w32(ctx, sock + 76, send_buf);
    w32(ctx, sock + 44, 0xFFFFFFFF);
    w32(ctx, sock + 40, client);
    w8(ctx, sock + 81, ssl);
    w64(ctx, sock + 16, recv_timeout);
    w64(ctx, sock + 24, send_timeout);
    w64(ctx, sock + 32, conn_timeout);
    w32(ctx, sock + 68, buf_size);
    w8(ctx, sock + 82, 0);
    w32(ctx, sock + 52, 0xFFFFFFFF);
    w32(ctx, sock + 60, ssl_version);
    w32(ctx, sock + 64, ssl_version == 1 ? 1 : 0);
    const s32 fd = static_cast<s32>(call(ctx, "sys_net", "socket", {2, 1, 0})); // AF_INET, SOCK_STREAM
    w32(ctx, sock + 52, static_cast<u32>(fd));
    if (fd >= 0) return kOk;
    const u32 e = static_cast<u32>(call(ctx, "sys_net", "_sys_net_errno_loc", {}));
    return static_cast<s32>(0x80710400u | r8(ctx, e + 3));
}

// A connection (152 bytes) for a host key (and a proxy's), its socket opened.
s32 connection_create(PPUContext& ctx, u32 out, u32 host_key, u32 proxy_key, u32 mgr, u32 ssl_version, u8 keep_alive,
    u8 ssl, u8 https)
{
    const u32 conn = hmalloc(ctx, 152);
    if (!conn) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, conn, 152);
    if (const s32 r = key_copy(ctx, conn, host_key); r < 0) {
        hfree(ctx, conn);
        return r;
    }
    if (proxy_key) {
        if (const s32 r = key_copy(ctx, conn + 20, proxy_key); r < 0) {
            hfree(ctx, r32(ctx, conn + 4));
            w32(ctx, conn + 4, 0);
            hfree(ctx, conn);
            return r;
        }
        w8(ctx, conn + 143, 1);
    }
    const s32 r = socket_open(ctx, conn + conn::kSocket, ssl, r64(ctx, mgr + m::kConnTimeout),
        r64(ctx, mgr + m::kRecvTimeout), r64(ctx, mgr + m::kSendTimeout), r32(ctx, mgr + m::kRecvBuf),
        r32(ctx, mgr + m::kSendBuf), ssl_version, r32(ctx, mgr + m::kBufSize), r32(ctx, mgr + m::kClient));
    if (r < 0) {
        if (const u32 p = r32(ctx, conn + 24)) {
            hfree(ctx, p);
            w32(ctx, conn + 24, 0);
        }
        hfree(ctx, r32(ctx, conn + 4));
        w32(ctx, conn + 4, 0);
        hfree(ctx, conn);
        return r;
    }
    w32(ctx, out, conn);
    w8(ctx, conn + 141, keep_alive);
    w32(ctx, conn + conn::kSelf, conn);
    w8(ctx, conn + 140, https);
    return kOk;
}

// The connection's resolver (libnet's, NID 0x012F752F): an error is CELL_HTTP_ERROR_NET_RESOLVER with h_errno.
s32 connection_resolver(PPUContext& ctx, u32 conn)
{
    if (!connection_valid(ctx, conn)) return err(CELL_HTTP_ERROR_BAD_CONN);
    if (r8(ctx, conn + 122)) return kOk;
    const s32 id = static_cast<s32>(call_nid(ctx, "sys_net", 0x012F752F, {0}));
    if (id >= 0) {
        w32(ctx, conn + 84, static_cast<u32>(id));
        return kOk;
    }
    const u32 e = static_cast<u32>(call(ctx, "sys_net", "_sys_net_h_errno_loc", {}));
    return static_cast<s32>(CELL_HTTP_ERROR_NET_RESOLVER | r8(ctx, e + 3));
}

// The connection a transaction asks its manager for, when the host has room for one more: made, its resolver made.
s32 connection_new(PPUContext& ctx, u32 mgr, u32 host_key, u32 proxy_key, u8 keep_alive, u8 ssl, u32 t, u8 https)
{
    StackVar v(ctx, gb(ctx), 16); // the host record, the SSL version, the connection
    const u32 rec = v.addr(), version = v.addr() + 4, made = v.addr() + 8;
    w32(ctx, rec, 0);
    w32(ctx, version, 0);
    w32(ctx, made, 0);
    const u32 list = r8(ctx, mgr + m::k161) && https ? mgr + m::kHosts2 : mgr + m::kHosts;
    if (const s32 r = host_find_or_add(ctx, list, host_key, rec); r < 0) return r;
    // The transaction's SSL version (checked as libhttp checks a transaction).
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!valid(ctx, t, trans::kSelf)) return err(CELL_HTTP_ERROR_BAD_TRANS);
    w32(ctx, version, r32(ctx, t + trans::kSslVersion));
    const u32 h = r32(ctx, rec);
    if (r32(ctx, list + 8) >= r32(ctx, list + 12)) return err(CELL_HTTP_ERROR_NO_CONNECTION);
    if (keep_alive ? r32(ctx, h + 56) >= r32(ctx, list + 24) : r32(ctx, h + 52) >= r32(ctx, list + 16))
        return err(CELL_HTTP_ERROR_NO_CONNECTION);
    if (const s32 r = connection_create(ctx, made, host_key, proxy_key, mgr, r32(ctx, version), keep_alive, ssl, https);
        r < 0)
        return r;
    const u32 conn = r32(ctx, made);
    if (const s32 r = connection_resolver(ctx, conn); r < 0) {
        connection_destroy(ctx, conn, r32(ctx, mgr + m::k132));
        return r;
    }
    // libhttp goes on to connect: the resolver given back, the connection destroyed.
    const s32 r = unreachable(ctx, "a connection with a resolver");
    call_nid(ctx, "sys_net", 0x3575B141, {r32(ctx, conn + 84)});
    connection_destroy(ctx, conn, r32(ctx, mgr + m::k132));
    return r;
}

// A connection from the manager: the host's record (made when there is none), then a new connection if the limits
// leave room (the manager's total, the host's, its kept-alive ones), else CELL_HTTP_ERROR_NO_CONNECTION.
s32 acquire(PPUContext& ctx, u32 mgr, u32 out_conn, u32 host_key, u8 keep_alive, u8 https, u32 flag, u32 t,
    u32 proxy_key)
{
    const u8 tunnel = r8(ctx, flag);
    StackVar v(ctx, gb(ctx), 8);
    const u32 rec = v.addr();
    w32(ctx, rec, 0);
    if (!valid(ctx, mgr, m::kSelf)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    const auto done = [&](s32 r) {
        lw_unlock(ctx, mgr + m::kLock);
        return r;
    };
    const u32 list = r8(ctx, mgr + m::k161) && https ? mgr + m::kHosts2 : mgr + m::kHosts;
    if (const s32 r = host_find_or_add(ctx, list, host_key, rec); r < 0) return done(r);
    if (r32(ctx, out_conn)) return done(unreachable(ctx, "a transaction's connection given back"));
    const u32 h = r32(ctx, rec);
    if (r32(ctx, h + 44)) return done(unreachable(ctx, "a host's idle connection"));
    const bool room = r32(ctx, list + 8) < r32(ctx, list + 12) &&
                      (keep_alive ? r32(ctx, h + 56) < r32(ctx, list + 24) : r32(ctx, h + 52) < r32(ctx, list + 16));
    if (room) {
        const s32 r = connection_new(ctx, mgr, host_key, proxy_key, keep_alive, tunnel ? 0 : https, t, https);
        if (r < 0 && !r32(ctx, out_conn)) return done(r);
        return done(unreachable(ctx, "a new connection"));
    }
    if (r32(ctx, list)) return done(unreachable(ctx, "a connection to wait for"));
    if (r8(ctx, mgr + m::kWaitStatus)) return done(unreachable(ctx, "waiting for a connection (no room)"));
    return done(err(CELL_HTTP_ERROR_NO_CONNECTION));
}

} // namespace

s32 connection_acquire(PPUContext& ctx, u32 client, u32 uri, u32 out_conn, u32 proxied, u8 keep_alive, u8 https,
    u32 flag, u32 t)
{
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!valid(ctx, client, client::kSelf)) return err(CELL_HTTP_ERROR_BAD_CLIENT);
    StackVar keys(ctx, gb(ctx), 48); // the host's key {name, copy, length, port} +0, the proxy's +24
    const u32 hk = keys.addr(), pk = keys.addr() + 24;
    zero(ctx, hk, 20);
    zero(ctx, pk, 20);
    if (const s32 r = lw_lock(ctx, client + client::kLock); r < 0) return r;
    const u8 use_proxy = r8(ctx, client + client::kUseProxy);
    u32 proxy_copy = 0;
    if (use_proxy) {
        const u32 name = r32(ctx, client + client::kProxy + 4);
        const u32 n = str_len(ctx, name) + 1;
        proxy_copy = hmalloc(ctx, n);
        if (!proxy_copy) {
            lw_unlock(ctx, client + client::kLock);
            return err(CELL_HTTP_ERROR_NO_MEMORY);
        }
        copy(ctx, proxy_copy, name, n);
        w32(ctx, pk + 8, n);
        w32(ctx, pk + 12, r32(ctx, client + client::kProxy + 20));
        w32(ctx, pk + 4, proxy_copy);
        w32(ctx, pk + 0, proxy_copy);
    }
    const u32 mgr = r32(ctx, client + client::kManager);
    lw_unlock(ctx, client + client::kLock);
    const u32 host = r32(ctx, uri + 4);
    w32(ctx, hk + 0, host);
    w32(ctx, hk + 4, 0);
    w32(ctx, hk + 8, str_len(ctx, host) + 1);
    w32(ctx, hk + 12, r32(ctx, uri + 20));
    s32 r = 0;
    if (r32(ctx, out_conn)) {
        r = unreachable(ctx, "a transaction's connection used again");
    } else {
        w8(ctx, flag, use_proxy ? (https != 0) : 0);
        r = acquire(ctx, mgr, out_conn, hk, keep_alive, https, flag, t, use_proxy ? pk : 0);
        if (r == 0) w8(ctx, proxied, use_proxy && !r8(ctx, flag) ? 1 : 0);
    }
    if (proxy_copy) hfree(ctx, proxy_copy);
    return r;
}

namespace {

// ------------------------------------------------------------------------------------------------- polling, closing

// The idle connections of one host closed (libhttp's): offline there are none.
s32 host_close_idle(PPUContext& ctx, u32 rec)
{
    if (r32(ctx, rec + 44)) return unreachable(ctx, "closing a host's connections");
    return kOk;
}

// The connections of one host closed (its record made when there is none).
s32 manager_close_host(PPUContext& ctx, u32 mgr, u32 key)
{
    if (!valid(ctx, mgr, m::kSelf)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    StackVar v(ctx, gb(ctx), 8);
    const u32 rec = v.addr();
    w32(ctx, rec, 0);
    s32 r = host_find_or_add(ctx, mgr + m::kHosts, key, rec);
    if (r >= 0) {
        r = host_close_idle(ctx, r32(ctx, rec));
        if (r8(ctx, mgr + m::k161)) {
            w32(ctx, rec, 0);
            r = host_find_or_add(ctx, mgr + m::kHosts2, key, rec);
            if (r >= 0) r = host_close_idle(ctx, r32(ctx, rec));
        }
    }
    lw_unlock(ctx, mgr + m::kLock);
    return r;
}

// The connections with something to read, waited for: offline there are none (CELL_HTTP_ERROR_NO_CONNECTION).
s32 manager_poll(PPUContext& ctx, u32 mgr, u32 out, u64 timeout)
{
    (void)timeout;
    if (!valid(ctx, mgr, m::kSelf)) return err(CELL_HTTP_ERROR_BAD_MAN);
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = lw_lock(ctx, mgr + m::kLock); r < 0) return r;
    bool any = r32(ctx, mgr + m::kConnections) != 0;
    for (u32 e = r32(ctx, mgr + m::kHosts + 4); e; e = r32(ctx, e)) any = any || r32(ctx, r32(ctx, e + 8) + 48) != 0;
    if (r8(ctx, mgr + m::k161)) {
        for (u32 e = r32(ctx, mgr + m::kHosts2 + 4); e; e = r32(ctx, e))
            any = any || r32(ctx, r32(ctx, e + 8) + 48) != 0;
    }
    const s32 r = any ? unreachable(ctx, "polling connections") : err(CELL_HTTP_ERROR_NO_CONNECTION);
    lw_unlock(ctx, mgr + m::kLock);
    return r;
}

s32 check_client(PPUContext& ctx, u32 client)
{
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!valid(ctx, client, client::kSelf)) return err(CELL_HTTP_ERROR_BAD_CLIENT);
    return kOk;
}

s32 client_manager(PPUContext& ctx, u32 client, u32& mgr)
{
    if (const s32 r = lw_lock(ctx, client + client::kLock); r < 0) return r;
    mgr = r32(ctx, client + client::kManager);
    lw_unlock(ctx, client + client::kLock);
    return kOk;
}

// cellHttpClientPollConnections(clientId, CellHttpTransId* transId, s64 usec).
s32 cellHttpClientPollConnections(PPUContext& ctx, u32 client, u32 out, u64 timeout)
{
    if (const s32 r = check_client(ctx, client); r < 0) return r;
    if (!out) return err(CELL_HTTP_ERROR_NO_BUFFER);
    u32 mgr = 0;
    if (const s32 r = client_manager(ctx, client, mgr); r < 0) return r;
    return manager_poll(ctx, mgr, out, timeout);
}

// cellHttpClientCloseConnections(clientId, const CellHttpUri* uri): the host's connections (its key: the URI's host
// and port).
s32 cellHttpClientCloseConnections(PPUContext& ctx, u32 client, u32 uri)
{
    if (const s32 r = check_client(ctx, client); r < 0) return r;
    if (!usable(ctx, uri, 24, "cellHttpClientCloseConnections: uri")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    StackVar v(ctx, gb(ctx), 24);
    const u32 key = v.addr();
    zero(ctx, key, 20);
    const u32 host = r32(ctx, uri + 4);
    w32(ctx, key, host);
    w32(ctx, key + 8, str_len(ctx, host) + 1);
    w32(ctx, key + 12, r32(ctx, uri + 20));
    u32 mgr = 0;
    if (const s32 r = client_manager(ctx, client, mgr); r < 0) return r;
    return manager_close_host(ctx, mgr, key);
}

s32 cellHttpClientCloseAllConnections(PPUContext& ctx, u32 client)
{
    if (const s32 r = check_client(ctx, client); r < 0) return r;
    u32 mgr = 0;
    if (const s32 r = client_manager(ctx, client, mgr); r < 0) return r;
    return manager_close_all(ctx, mgr);
}

} // namespace

void register_connections(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0xADC0A4B2, cellHttpClientPollConnections),
        PS_HLE_FUNC(0x27F86D70, cellHttpClientCloseConnections),
        PS_HLE_FUNC(0x2033B878, cellHttpClientCloseAllConnections),
    });
}

} // namespace pspocket::hle::http
