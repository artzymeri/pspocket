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

} // namespace pspocket::hle::http
