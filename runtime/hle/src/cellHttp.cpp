// cellHttp: libhttp's HTTP client, pspocket's own (D-524). Every function does what Sony's libhttp.sprx (4.92) does,
// read from its code and compared call for call with it in tests/hle/test_http_compare.cpp: the same answers, the same
// bytes in the caller's memory and in libhttp's pools (its heaps: the same blocks at the same places, the objects'
// fields at libhttp's offsets), the same lv2 calls. RPCS3's rpcs3/Emu/Cell/Modules/cellHttp.cpp/.h gives the names, the
// error codes and the structures (its functions are stubs over a few flags; RPCS3 runs Sony's module).
//
// This file: the library's set-up (cellHttpInit/End, the heap made over the game's pool), its memory report, the global
// proxy, the header lists clients and transactions share, and the helpers of cellHttp.hpp. The other files: clients and
// their connection managers (cellHttpClient.cpp), and the rest as their names say.
//
// pspocket's console is offline: lv2's network has only the loopback interface (D-523), so connections to other hosts
// fail as they do on a PS3 with no network, through libnet (pspocket's own sys_net).

#include "cellHttp.hpp"

#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::http {

// ------------------------------------------------------------------------------------------------- guest memory

void zero(PPUContext& ctx, u32 a, u32 size)
{
    if (size) std::memset(gb(ctx) + a, 0, size);
}

void copy(PPUContext& ctx, u32 dst, u32 src, u32 size)
{
    if (size) std::memmove(gb(ctx) + dst, gb(ctx) + src, size);
}

u32 str_len(PPUContext& ctx, u32 s)
{
    const u8* b = gb(ctx);
    u32 n = 0;
    while (b[s + n]) n++;
    return n;
}

std::string str(PPUContext& ctx, u32 s)
{
    return std::string(reinterpret_cast<const char*>(gb(ctx) + s), str_len(ctx, s));
}

namespace {
int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
} // namespace

s32 str_ncasecmp(PPUContext& ctx, u32 a, u32 b, u32 n)
{
    const u8* g = gb(ctx);
    for (u32 i = 0; i < n; i++) {
        const int x = lower(g[a + i]), y = lower(g[b + i]);
        if (x != y) return x - y;
        if (!x) break;
    }
    return 0;
}

s32 str_ncmp(PPUContext& ctx, u32 a, u32 b, u32 n)
{
    const u8* g = gb(ctx);
    for (u32 i = 0; i < n; i++) {
        const int x = g[a + i], y = g[b + i];
        if (x != y) return x - y;
        if (!x) break;
    }
    return 0;
}

void md5(const u8* data, usize len, u8 out[16])
{
    static constexpr u32 kK[64] = {0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
        0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681,
        0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085,
        0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82,
        0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr u32 kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15,
        21, 6, 10, 15, 21, 6, 10, 15, 21};
    u32 h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::vector<u8> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const u64 bits = u64{len} * 8;
    for (u32 i = 0; i < 8; i++) msg.push_back(static_cast<u8>(bits >> (8 * i)));
    for (usize off = 0; off < msg.size(); off += 64) {
        u32 w[16];
        for (u32 i = 0; i < 16; i++) {
            w[i] = u32{msg[off + 4 * i]} | u32{msg[off + 4 * i + 1]} << 8 | u32{msg[off + 4 * i + 2]} << 16 |
                   u32{msg[off + 4 * i + 3]} << 24;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        for (u32 i = 0; i < 64; i++) {
            u32 f = 0, g = 0;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            const u32 t = d;
            d = c;
            c = b;
            const u32 x = a + f + kK[i] + w[g];
            b = b + (x << kS[i] | x >> (32 - kS[i]));
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    for (u32 i = 0; i < 4; i++)
        for (u32 k = 0; k < 4; k++) out[4 * i + k] = static_cast<u8>(h[i] >> (8 * k));
}

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    lv2::PpuThread& t = thread_of(ctx);
    bool ok = u64{addr} + size <= GuestMemory::kGuestSize;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); ok && size && a < u64{addr} + size; a += page)
        ok = t.kernel.memory().is_committed(static_cast<u32>(a));
    if (!ok) {
        log_call_site(ctx, gb(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory (libhttp faults)", what, addr,
            size));
    }
    return ok;
}

// ------------------------------------------------------------------------------------------------- other libraries

namespace {

const Function* find(std::string_view module, u32 nid)
{
    static std::mutex m;
    static std::unordered_map<std::string, const Function*> cache;
    std::lock_guard lock(m);
    std::string key = std::format("{}:{:08x}", module, nid);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    const Function* f = registry().find(module, nid);
    cache.emplace(std::move(key), f);
    return f;
}

u64 invoke(PPUContext& ctx, const Function& f, std::initializer_list<u64> args)
{
    std::array<u64, 8> saved{};
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f.fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

} // namespace

u64 call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = find(module, recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "{}::{}, which libhttp imports, is not pspocket's", module, name);
    return invoke(ctx, *f, args);
}

u64 call_nid(PPUContext& ctx, std::string_view module, u32 nid, std::initializer_list<u64> args)
{
    if (const Function* f = find(module, nid)) return invoke(ctx, *f, args);
    PS_LOG_ERROR(kChan, "UNIMPLEMENTED {}::{:#010x} (imported by libhttp): CELL_ENOSYS, as the loader's stub answers",
        module, nid);
    return lv2::to_r3(lv2::err(lv2::CELL_ENOSYS));
}

bool page_ok(PPUContext& ctx, u32 addr)
{
    StackVar attr(ctx, gb(ctx), 24); // sys_page_attr_t
    return sc(ctx, 351, {addr, attr.addr()}) == 0;
}

bool valid(PPUContext& ctx, u32 obj, u32 self)
{
    if (!page_ok(ctx, obj)) return false;
    if (!guest_readable(thread_of(ctx), obj + self, 4)) {
        log_call_site(ctx, gb(ctx), std::format("ID {:#x}: its word at +{} is not guest memory (libhttp faults)", obj,
            self));
        return false;
    }
    return r32(ctx, obj + self) == obj;
}

u32 sdk_version(PPUContext& ctx)
{
    const u32 pid = static_cast<u32>(sc(ctx, 1, {}));
    StackVar v(ctx, gb(ctx), 8);
    sc(ctx, 25, {pid, v.addr()});
    return r32(ctx, v.addr());
}

s32 lw_create(PPUContext& ctx, u32 lw, std::string_view name)
{
    StackVar attr(ctx, gb(ctx), 16); // sys_lwmutex_attribute_t {protocol, recursive, name[8]}
    w32(ctx, attr.addr(), 2);        // SYS_SYNC_PRIORITY
    w32(ctx, attr.addr() + 4, 0x20); // SYS_SYNC_NOT_RECURSIVE
    for (u32 i = 0; i < 7 && i < name.size(); i++) w8(ctx, attr.addr() + 8 + i, static_cast<u8>(name[i]));
    return static_cast<s32>(call(ctx, "sysPrxForUser", "sys_lwmutex_create", {lw, attr.addr()}));
}

s32 lw_destroy(PPUContext& ctx, u32 lw)
{
    return static_cast<s32>(call(ctx, "sysPrxForUser", "sys_lwmutex_destroy", {lw}));
}
s32 lw_lock(PPUContext& ctx, u32 lw) { return static_cast<s32>(call(ctx, "sysPrxForUser", "sys_lwmutex_lock", {lw, 0})); }
s32 lw_unlock(PPUContext& ctx, u32 lw)
{
    return static_cast<s32>(call(ctx, "sysPrxForUser", "sys_lwmutex_unlock", {lw}));
}

s32 sync_create(PPUContext& ctx, u32 sync, u64 value)
{
    // sys_mutex_attribute_t {protocol, recursive, pshared, adaptive, ipc_key, flags, pad, name}, then
    // sys_cond_attribute_t {pshared, flags, ipc_key, name}: libhttp leaves the IPC keys as its stack holds them.
    StackVar attr(ctx, gb(ctx), 0x40);
    const u32 m = attr.addr(), c = attr.addr() + 0x28;
    w32(ctx, m, 2);
    w32(ctx, m + 4, 0x20);
    w32(ctx, m + 8, 0x200);
    w32(ctx, m + 12, 0x2000);
    w32(ctx, m + 24, 0);
    std::memcpy(gb(ctx) + m + 32, "_httsyn", 8);
    s32 r = sc(ctx, 100, {sync, m});
    if (r < 0) return r;
    w32(ctx, c, 0x200);
    w32(ctx, c + 4, 0);
    std::memcpy(gb(ctx) + c + 16, "_httsyn", 8);
    r = sc(ctx, 105, {sync + 4, r32(ctx, sync), c});
    if (r < 0) {
        sc(ctx, 101, {r32(ctx, sync)});
        return r;
    }
    w64(ctx, sync + 8, value);
    w64(ctx, sync + 16, 0);
    return kOk;
}

void sync_destroy(PPUContext& ctx, u32 sync)
{
    sc(ctx, 106, {r32(ctx, sync + 4)});
    sc(ctx, 101, {r32(ctx, sync)});
}

// ------------------------------------------------------------------------------------------------- the library's state

Lib& lib() { return HleProcess::current()->state<Lib>(); }

u32 data(PPUContext& ctx)
{
    Lib& l = lib();
    if (!l.data) {
        l.data = HleProcess::current()->main_memory().alloc(kDataSize, 0x80);
        if (!l.data) PS_FATAL(kChan, "no guest memory for libhttp's globals");
        zero(ctx, l.data, kDataSize);
        std::memcpy(gb(ctx) + l.data + kStrHeap, "cellHttp_Library", 17);
        std::memcpy(gb(ctx) + l.data + kStrCookieHeap, "cellHttpCookie_Library", 23);
    }
    return l.data;
}

bool initialized(PPUContext& ctx) { return r8(ctx, data(ctx) + kSys + kSysInit) != 0; }

bool https_initialized(PPUContext& ctx)
{
    const u32 h = r32(ctx, data(ctx) + kHttps);
    return h && r8(ctx, h + 4) != 0;
}

u32 hmalloc(PPUContext& ctx, u32 size)
{
    return static_cast<u32>(
        call(ctx, "sysPrxForUser", "_sys_heap_malloc", {r32(ctx, data(ctx) + kSys + kSysHeap), size}));
}

void hfree(PPUContext& ctx, u32 addr)
{
    call(ctx, "sysPrxForUser", "_sys_heap_free", {r32(ctx, data(ctx) + kSys + kSysHeap), addr});
}

// ------------------------------------------------------------------------------------------------- header lists

s32 headers_add(PPUContext& ctx, u32 head, u32 header)
{
    const u32 node = hmalloc(ctx, hnode::kSize);
    if (!node) return err(CELL_HTTP_ERROR_NO_MEMORY);
    zero(ctx, node, hnode::kSize);
    StackVar need(ctx, gb(ctx), 8);
    s32 r = util(ctx, "cellHttpUtilCopyHeader", {0, header, 0, 0, need.addr()});
    if (r < 0) {
        hfree(ctx, node);
        return r;
    }
    const u32 pool = hmalloc(ctx, r32(ctx, need.addr()));
    w32(ctx, node + hnode::kPool, pool);
    if (!pool) {
        hfree(ctx, node);
        return err(CELL_HTTP_ERROR_NO_MEMORY);
    }
    r = util(ctx, "cellHttpUtilCopyHeader", {node + hnode::kHeader, header, pool, r32(ctx, need.addr()), 0});
    if (r < 0) {
        hfree(ctx, r32(ctx, node + hnode::kPool));
        w32(ctx, node + hnode::kPool, 0);
        hfree(ctx, node);
        return r;
    }
    u32 last = r32(ctx, head);
    if (!last) {
        w32(ctx, head, node);
        return kOk;
    }
    while (r32(ctx, last + hnode::kNext)) last = r32(ctx, last + hnode::kNext);
    w32(ctx, last + hnode::kNext, node);
    w32(ctx, node + hnode::kPrev, last);
    w32(ctx, node + hnode::kNext, 0);
    return kOk;
}

void headers_free(PPUContext& ctx, u32 node)
{
    while (node) {
        const u32 next = r32(ctx, node + hnode::kNext);
        w32(ctx, node + hnode::kPrev, 0);
        w32(ctx, node + hnode::kNext, 0);
        if (const u32 pool = r32(ctx, node + hnode::kPool)) {
            hfree(ctx, pool);
            w32(ctx, node + hnode::kPool, 0);
            zero(ctx, node + hnode::kHeader, 8);
        }
        hfree(ctx, node);
        node = next;
    }
}

s32 headers_find(PPUContext& ctx, u32 node, u32 out, u32 name)
{
    const u32 n = str_len(ctx, name) + 1;
    for (; node; node = r32(ctx, node + hnode::kNext)) {
        if (str_ncasecmp(ctx, r32(ctx, node + hnode::kName), name, n) == 0) {
            w32(ctx, out, node + hnode::kHeader);
            return kOk;
        }
    }
    return err(CELL_HTTP_ERROR_NO_HEADER);
}

s32 headers_copy_all(PPUContext& ctx, u32 node, u32 headers, u32 pool, u32 size, u32 required)
{
    if (required) w32(ctx, required, 0);
    StackVar used(ctx, gb(ctx), 8);
    for (; node; node = r32(ctx, node + hnode::kNext)) {
        w32(ctx, used.addr(), 0);
        const s32 r = util(ctx, "cellHttpUtilCopyHeader", {headers, node + hnode::kHeader, pool, size, used.addr()});
        if (required) w32(ctx, required, r32(ctx, required) + r32(ctx, used.addr()));
        if (r < 0) return r;
        if (headers) headers += 8;
        if (pool) {
            size -= r32(ctx, used.addr());
            pool += r32(ctx, used.addr());
        }
    }
    return kOk;
}

// ------------------------------------------------------------------------------------------------- proxies

s32 proxy_prepare(PPUContext& ctx, u32 proxy, u32 uri, u32& pool, u32& parse_pool)
{
    StackVar v(ctx, gb(ctx), 0x30); // the size, then the parsed CellHttpUri at +0x10
    const u32 need = v.addr(), parsed = v.addr() + 0x10;
    s32 r = util(ctx, "cellHttpUtilParseUri", {0, r32(ctx, proxy + 4), 0, 0, need});
    if (r < 0) return r;
    parse_pool = hmalloc(ctx, r32(ctx, need));
    if (!parse_pool) return err(CELL_HTTP_ERROR_NO_MEMORY);
    r = util(ctx, "cellHttpUtilParseUri", {parsed, r32(ctx, proxy + 4), parse_pool, r32(ctx, need), 0});
    if (r < 0) {
        hfree(ctx, parse_pool);
        return r;
    }
    w32(ctx, parsed + 0, r32(ctx, proxy + 0));   // scheme
    w32(ctx, parsed + 8, r32(ctx, proxy + 8));   // username
    w32(ctx, parsed + 12, r32(ctx, proxy + 12)); // password
    w32(ctx, parsed + 16, r32(ctx, proxy + 16)); // path
    w32(ctx, parsed + 20, r32(ctx, proxy + 20)); // port
    zero(ctx, uri, 28);
    w32(ctx, need, 0);
    r = util(ctx, "cellHttpUtilCopyUri", {0, parsed, 0, 0, need});
    if (r < 0) {
        hfree(ctx, parse_pool);
        return r;
    }
    pool = hmalloc(ctx, r32(ctx, need));
    if (!pool) {
        hfree(ctx, parse_pool);
        return err(CELL_HTTP_ERROR_NO_MEMORY);
    }
    r = util(ctx, "cellHttpUtilCopyUri", {uri, parsed, pool, r32(ctx, need), 0});
    if (r < 0) {
        hfree(ctx, pool);
        hfree(ctx, parse_pool);
        return r;
    }
    return kOk;
}

namespace {

// ------------------------------------------------------------------------------------------------- set-up

// The redirection records' set-up and end (libhttp's two functions beside cellHttpInit).
s32 records_init(PPUContext& ctx)
{
    const u32 rec = data(ctx) + kRec;
    if (r8(ctx, rec + 1572)) return err(CELL_HTTP_ERROR_ALREADY_INITIALIZED);
    zero(ctx, rec + 24, 1536);
    if (const s32 r = lw_create(ctx, rec, "_httrec"); r < 0) return r;
    w32(ctx, rec + 1560, rec + 24);
    for (u32 i = 0; i < 15; i++) w32(ctx, rec + 24 + 96 * i + 88, rec + 24 + 96 * (i + 1));
    w8(ctx, rec + 1572, 1);
    w32(ctx, rec + 1568, 0);
    w32(ctx, rec + 1552, 0);
    w32(ctx, rec + 1564, 0);
    return kOk;
}

void records_end(PPUContext& ctx)
{
    const u32 rec = data(ctx) + kRec;
    if (!r8(ctx, rec + 1572)) return;
    for (u32 e = r32(ctx, rec + 1564); e; e = r32(ctx, e + 84)) {
        for (const u32 field : {28u, 48u, 76u, 24u}) {
            hfree(ctx, r32(ctx, e + field));
            w32(ctx, e + field, 0);
        }
    }
    lw_destroy(ctx, rec);
    w8(ctx, rec + 1572, 0);
    zero(ctx, rec + 24, 1536);
    w32(ctx, rec + 1568, 0);
    w32(ctx, rec + 1560, 0);
    w32(ctx, rec + 1564, 0);
}

// The authentication cache's set-up and end.
s32 auth_init(PPUContext& ctx)
{
    const u32 a = data(ctx) + kAuth;
    if (r8(ctx, a + 40)) return err(CELL_HTTP_ERROR_ALREADY_INITIALIZED);
    if (const s32 r = lw_create(ctx, a, "_httauc"); r < 0) return r;
    w8(ctx, a + 40, 1);
    w32(ctx, a + 36, 0);
    w32(ctx, a + 28, 0);
    w32(ctx, a + 32, 0);
    w32(ctx, a + 24, 4);
    return kOk;
}

void auth_end(PPUContext& ctx)
{
    const u32 a = data(ctx) + kAuth;
    if (!r8(ctx, a + 40)) return;
    // Each entry: next +628, previous +632, two blocks at +96 and +64.
    for (u32 e = r32(ctx, a + 32); e;) {
        const u32 next = r32(ctx, e + 628), prev = r32(ctx, e + 632);
        if (next) w32(ctx, next + 632, prev);
        else w32(ctx, a + 36, prev);
        if (prev) w32(ctx, prev + 628, next);
        else w32(ctx, a + 32, next);
        for (const u32 field : {96u, 64u}) {
            if (const u32 p = r32(ctx, e + field)) {
                hfree(ctx, p);
                w32(ctx, e + field, 0);
            }
        }
        hfree(ctx, e);
        e = next;
    }
    lw_destroy(ctx, a);
    w32(ctx, a + 36, 0);
    w8(ctx, a + 40, 0);
    w32(ctx, a + 32, 0);
}

// cellHttpInit(void* pool, u32 poolSize): the redirection records, the authentication cache, liblv2's heap
// "cellHttp_Library" over the pool (the caller's memory), the lock "_httsys".
s32 cellHttpInit(PPUContext& ctx, u32 pool, u32 pool_size)
{
    const u32 sys = data(ctx) + kSys;
    if (initialized(ctx)) return err(CELL_HTTP_ERROR_ALREADY_INITIALIZED);
    if (!pool || !pool_size) return err(CELL_HTTP_ERROR_NO_BUFFER);
    if (const s32 r = records_init(ctx); r < 0) return r;
    if (const s32 r = auth_init(ctx); r < 0) {
        records_end(ctx);
        return r;
    }
    StackVar attr(ctx, gb(ctx), 8); // {8, the memory}
    w32(ctx, attr.addr(), 8);
    w32(ctx, attr.addr() + 4, pool);
    const u32 heap = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_create_heap",
        {data(ctx) + kStrHeap, pool_size, 0x80000000u, attr.addr()}));
    if (!heap) {
        auth_end(ctx);
        records_end(ctx);
        return err(CELL_HTTP_ERROR_NO_MEMORY);
    }
    if (const s32 r = lw_create(ctx, sys + kSysLock, "_httsys"); r < 0) {
        call(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap});
        auth_end(ctx);
        records_end(ctx);
        return r;
    }
    w32(ctx, sys + kSysBufSize, 1024);
    w8(ctx, sys + kSysInit, 1);
    w32(ctx, sys + kSysHeap, heap);
    PS_LOG_INFO(kChan, "cellHttpInit: pool {:#x}+{:#x}", pool, pool_size);
    return kOk;
}

// cellHttpEnd: the authentication cache and the redirection records freed, the global proxy's pool freed and the
// proxy cleared (libhttp leaves its "set" flag), the lock destroyed and the heap deleted. Clients and transactions
// are not destroyed (their blocks go with the heap).
s32 cellHttpEnd(PPUContext& ctx)
{
    const u32 sys = data(ctx) + kSys;
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    auth_end(ctx);
    records_end(ctx);
    if (const u32 p = r32(ctx, sys + kSysProxyPool)) {
        hfree(ctx, p);
        w32(ctx, sys + kSysProxyPool, 0);
        zero(ctx, sys + kSysProxy, 28);
    }
    lw_destroy(ctx, sys + kSysLock);
    const u32 heap = r32(ctx, sys + kSysHeap);
    w8(ctx, sys + kSysInit, 0);
    w32(ctx, sys + kSysHeap, 0);
    call(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap});
    return kOk;
}

// cellHttpGetMemoryInfo(u32* free, u32* p2, struct mallinfo* info): the heap's free bytes and its mallinfo (liblv2's
// _sys_heap_get_total_free_size and _sys_heap_get_mallinfo; neither needs cellHttpInit: with no heap liblv2 prints
// "invalid heap area 0x0"). libhttp stores into `p2` the word `info` held before (it reads info's first word): kept.
// Before cellHttpInit libhttp copies what its stack held as the mallinfo; pspocket copies zeros.
s32 cellHttpGetMemoryInfo(PPUContext& ctx, u32 free_size, u32 p2, u32 info)
{
    const u32 heap = r32(ctx, data(ctx) + kSys + kSysHeap);
    const u32 total_free = static_cast<u32>(call(ctx, "sysPrxForUser", "_sys_heap_get_total_free_size", {heap}));
    StackVar mi(ctx, gb(ctx), 40);
    call(ctx, "sysPrxForUser", "_sys_heap_get_mallinfo", {heap, mi.addr()});
    if (free_size) {
        if (!usable(ctx, free_size, 4, "cellHttpGetMemoryInfo: free")) return err(CELL_HTTP_ERROR_NO_BUFFER);
        w32(ctx, free_size, total_free);
    }
    if (p2 || info) {
        if (!usable(ctx, info, 40, "cellHttpGetMemoryInfo: info")) return err(CELL_HTTP_ERROR_NO_BUFFER);
        if (p2) {
            if (!usable(ctx, p2, 4, "cellHttpGetMemoryInfo: p2")) return err(CELL_HTTP_ERROR_NO_BUFFER);
            w32(ctx, p2, r32(ctx, info));
        }
        copy(ctx, info, mi.addr(), 40);
    }
    return kOk;
}

// cellHttpGetProxy(CellHttpUri* proxy, void* pool, u32 poolSize, u32* required): cellHttpUtilCopyUri of the global
// proxy; with none set, *required = 0.
s32 cellHttpGetProxy(PPUContext& ctx, u32 proxy, u32 pool, u32 pool_size, u32 required)
{
    const u32 sys = data(ctx) + kSys;
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (const s32 r = lw_lock(ctx, sys + kSysLock); r < 0) return r;
    s32 r = kOk;
    if (!r8(ctx, sys + kSysProxySet)) {
        if (required) w32(ctx, required, 0);
    } else {
        r = util(ctx, "cellHttpUtilCopyUri", {proxy, sys + kSysProxy, pool, pool_size, required});
    }
    lw_unlock(ctx, sys + kSysLock);
    return r;
}

// cellHttpSetProxy(const CellHttpUri* proxy): no proxy clears it; else its hostname must be a non-empty string.
s32 cellHttpSetProxy(PPUContext& ctx, u32 proxy)
{
    const u32 sys = data(ctx) + kSys;
    if (!initialized(ctx)) return err(CELL_HTTP_ERROR_NOT_INITIALIZED);
    if (!proxy) {
        if (const s32 r = lw_lock(ctx, sys + kSysLock); r < 0) return r;
        w8(ctx, sys + kSysProxySet, 0);
        if (const u32 p = r32(ctx, sys + kSysProxyPool)) {
            hfree(ctx, p);
            w32(ctx, sys + kSysProxyPool, 0);
            zero(ctx, sys + kSysProxy, 28);
        }
        lw_unlock(ctx, sys + kSysLock);
        return kOk;
    }
    if (!usable(ctx, proxy, 28, "cellHttpSetProxy: proxy")) return err(CELL_HTTP_ERROR_NO_BUFFER);
    const u32 host = r32(ctx, proxy + 4);
    if (!host || !r8(ctx, host)) return err(CELL_HTTP_ERROR_INVALID_URI);
    StackVar uri(ctx, gb(ctx), 32);
    u32 pool = 0, parse_pool = 0;
    if (const s32 r = proxy_prepare(ctx, proxy, uri.addr(), pool, parse_pool); r < 0) return r;
    const s32 r = lw_lock(ctx, sys + kSysLock);
    if (r < 0) {
        hfree(ctx, pool);
    } else {
        if (const u32 old = r32(ctx, sys + kSysProxyPool)) {
            hfree(ctx, old);
            w32(ctx, sys + kSysProxyPool, 0);
        }
        copy(ctx, sys + kSysProxy, uri.addr(), 24);
        w32(ctx, sys + kSysProxyPool, pool);
        w8(ctx, sys + kSysProxySet, 1);
        lw_unlock(ctx, sys + kSysLock);
    }
    hfree(ctx, parse_pool);
    return r;
}

} // namespace

void register_core(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0x250C386C, cellHttpInit),
        PS_HLE_FUNC(0xD276FF1F, cellHttpEnd),
        PS_HLE_FUNC(0xE95BFC2C, cellHttpGetMemoryInfo),
        PS_HLE_FUNC(0x2A87603A, cellHttpGetProxy),
        PS_HLE_FUNC(0x0D896B97, cellHttpSetProxy),
    });
}

} // namespace pspocket::hle::http

namespace pspocket::hle {

void register_cellHttp(Registry& r)
{
    http::register_core(r);
    http::register_clients(r);
    http::register_cookies(r);
    http::register_https(r);
}

} // namespace pspocket::hle
